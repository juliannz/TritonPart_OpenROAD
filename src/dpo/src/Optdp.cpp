///////////////////////////////////////////////////////////////////////////////
// BSD 3-Clause License
//
// Copyright (c) 2021, Andrew Kennings
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "dpo/Optdp.h"

#include <odb/db.h>
#include <boost/format.hpp>
#include <cfloat>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>

#include "dpl/Opendp.h"
#include "ord/OpenRoad.hh"  // closestPtInRect
#include "utl/Logger.h"

// My stuff.
#include "architecture.h"
#include "detailed.h"
#include "detailed_manager.h"
#include "legalize_shift.h"
#include "network.h"
#include "router.h"

namespace dpo {

using utl::DPO;

using odb::dbBlock;
using odb::dbInst;
using odb::dbBox;
using odb::dbBTerm;
using odb::dbITerm;
using odb::dbMaster;
using odb::dbMasterType;
using odb::dbMPin;
using odb::dbMTerm;
using odb::dbNet;
using odb::dbRegion;
using odb::dbSBox;
using odb::dbSet;
using odb::dbSigType;
using odb::dbSWire;
using odb::dbTechLayer;
using odb::dbWireType;
using odb::dbOrientType;
using odb::dbRow;
using odb::dbSite;
using odb::Rect;

////////////////////////////////////////////////////////////////
Optdp::Optdp() : db_(nullptr), logger_(nullptr), opendp_(nullptr),
    arch_(nullptr), network_(nullptr), routeinfo_(nullptr) {}

////////////////////////////////////////////////////////////////
Optdp::~Optdp() {}

////////////////////////////////////////////////////////////////
void Optdp::init(odb::dbDatabase* db, utl::Logger* logger, dpl::Opendp* opendp) {
  db_ = db;
  logger_ = logger;
  opendp_ = opendp;
}

////////////////////////////////////////////////////////////////
void Optdp::improvePlacement() {
  logger_->report("Detailed placement improvement.");

  hpwlBefore_ = opendp_->hpwl();

  if (hpwlBefore_ != 0) {
    // Get needed information from DB.
    import();

    // A manager to track cells.
    dpo::DetailedMgr mgr(arch_, network_, routeinfo_);
    mgr.setLogger(logger_);

    // Legalization.  Doesn't particularly do much.  It only
    // populates the data structures required for detailed
    // improvement.  If it errors or prints a warning when
    // given a legal placement, that likely means there is
    // a bug in my code somewhere.
    dpo::ShiftLegalizerParams lgParams;
    dpo::ShiftLegalizer lg(lgParams);
    lg.legalize(mgr);

    // Detailed improvement.  Runs through a number of different
    // optimizations aimed at wirelength improvement.  The last
    // call to the random improver can be set to consider things
    // like density, displacement, etc. in addition to wirelength.
    // Everything done through a script string.

    dpo::DetailedParams dtParams;
    dtParams.m_script = "";
    // Maximum independent set matching.
    dtParams.m_script += "mis -p 10 -t 0.005;";
    // Global swaps.
    dtParams.m_script += "gs -p 10 -t 0.005;";
    // Vertical swaps.
    dtParams.m_script += "vs -p 10 -t 0.005;";
    // Small reordering.
    dtParams.m_script += "ro -p 10 -t 0.005;";
    // Random moves and swaps with hpwl as a cost function.  Use
    // random moves and hpwl objective right now.
    dtParams.m_script +=
       "default -p 5 -f 20 -gen rng -obj hpwl -cost (hpwl);";

    // Run the script.
    dpo::Detailed dt(dtParams);
    dt.improve(mgr);

    // Write solution back.
    updateDbInstLocations();

    // Get final hpwl.
    hpwlAfter_ = opendp_->hpwl();

    // Cleanup.
    delete network_;
    delete arch_;
    delete routeinfo_;
  }
  else {
    logger_->report("Skipping detailed improvement since hpwl is zero.");
    hpwlAfter_ = hpwlBefore_;
  }

  double dbu_micron = db_->getTech()->getDbUnitsPerMicron();

  // Statistics.
  logger_->report("Detailed Improvement Results");
  logger_->report("------------------------------------------");
  logger_->report("Original HPWL         {:10.1f} u", (hpwlBefore_/dbu_micron));
  logger_->report("Final HPWL            {:10.1f} u", (hpwlAfter_/dbu_micron));
  double hpwl_delta = (hpwlBefore_ == 0.0)
    ? 0.0
    : ((double)(hpwlAfter_ - hpwlBefore_) / (double)hpwlBefore_) * 100.;
  logger_->report("Delta HPWL            {:10.1f} %", hpwl_delta);
  logger_->report("");
}
////////////////////////////////////////////////////////////////
void Optdp::import() {
  logger_->report("Importing netlist into detailed improver.");

  network_ = new Network;
  arch_ = new Architecture;
  routeinfo_ = new RoutingParams;

  //createLayerMap(); // Does nothing right now.
  //createNdrMap(); // Does nothing right now.
  setupMasterPowers();  // Call prior to network and architecture creation.
  createNetwork(); // Create network; _MUST_ do before architecture.
  createArchitecture(); // Create architecture.
  //createRouteInformation(); // Does nothing right now.
  initPadding(); // Need to do after network creation.
  //setUpNdrRules(); // Does nothing right now.
  setUpPlacementRegions(); // Regions.
}
////////////////////////////////////////////////////////////////
void Optdp::updateDbInstLocations() {
  std::unordered_map<odb::dbInst*, Node*>::iterator it_n;
  dbBlock* block = db_->getChip()->getBlock();
  dbSet<dbInst> insts = block->getInsts();
  for (dbInst* inst : insts) {
    auto type = inst->getMaster()->getType();
    if (!type.isCore() && !type.isBlock()) {
      continue;
    }
    it_n = instMap_.find(inst);
    if (instMap_.end() != it_n) {
      Node* nd = it_n->second;

      int y = (int)(nd->getY() - 0.5 * nd->getHeight());
      int x = (int)(nd->getX() - 0.5 * nd->getWidth());

      dbOrientType orient = dbOrientType::R0;
      switch (nd->getCurrOrient()) {
      case Orientation_N :
        orient = dbOrientType::R0;
        break;
      case Orientation_FN:
        orient = dbOrientType::MY;
        break;
      case Orientation_FS:
        orient = dbOrientType::MX;
        break;
      case Orientation_S :
        orient = dbOrientType::R180;
        break;
      default:
        // ?
        break;
      }
      if (inst->getOrient() != orient) {
        inst->setOrient(orient);
      }
      int inst_x, inst_y;
      inst->getLocation(inst_x, inst_y);
      if (x != inst_x || y != inst_y) {
        inst->setLocation(x, y);
      }
    }
  }
}
////////////////////////////////////////////////////////////////
void Optdp::initPadding() {
  // Grab information from OpenDP.

  // Need to turn off spacing tables and turn on padding.
  arch_->setUseSpacingTable(false);
  arch_->setUsePadding(true);
  arch_->init_edge_type();


  // Create and edge type for each amount of padding.  This
  // can be done by querying OpenDP.
  dbSet<dbRow> rows = db_->getChip()->getBlock()->getRows();
  if (rows.empty()) {
    return;
  }
  int siteWidth = (*rows.begin())->getSite()->getWidth();
  std::unordered_map<odb::dbInst*, Node*>::iterator it_n;

  dbSet<dbInst> insts = db_->getChip()->getBlock()->getInsts();
  for (dbInst* inst : insts) {
    it_n = instMap_.find(inst);
    if (instMap_.end() != it_n) {
      Node* ndi = it_n->second;
      int leftPadding = opendp_->padLeft(inst);
      int rightPadding = opendp_->padRight(inst);
      arch_->addCellPadding(ndi, leftPadding * siteWidth,
                            rightPadding * siteWidth);
    }
  }
}
////////////////////////////////////////////////////////////////
void Optdp::createLayerMap() {
  // Relates to pin blockages, etc. Not used rignt now.
  ;
}
////////////////////////////////////////////////////////////////
void Optdp::createNdrMap() {
  // Relates to pin blockages, etc. Not used rignt now.
  ;
}
////////////////////////////////////////////////////////////////
void Optdp::createRouteInformation() {
  // Relates to pin blockages, etc. Not used rignt now.
  ;
}
////////////////////////////////////////////////////////////////
void Optdp::setUpNdrRules() {
  // Relates to pin blockages, etc. Not used rignt now.
  ;
}
////////////////////////////////////////////////////////////////
void Optdp::setupMasterPowers() {
  // Need to try and figure out which voltages are on the
  // top and bottom of the masters/insts in order to set
  // stuff up for proper row alignment of multi-height
  // cells.  What I do it look at the individual ports
  // (MTerms) and see which ones correspond to POWER and
  // GROUND and then figure out which one is on top and
  // which one is on bottom.  I also record the layers
  // and use that information later when setting up the
  // row powers.
  dbBlock* block = db_->getChip()->getBlock();
  std::vector<dbMaster*> masters;
  block->getMasters(masters);

  for (dbMaster* master : masters) {

    double maxPwr = std::numeric_limits<double>::lowest();
    double minPwr = std::numeric_limits<double>::max();
    double maxGnd = std::numeric_limits<double>::lowest();
    double minGnd = std::numeric_limits<double>::max();

    bool isVdd = false;
    bool isGnd = false;
    for (dbMTerm* mterm : master->getMTerms()) {
      if (mterm->getSigType() == dbSigType::POWER) {
        isVdd = true;
        for (dbMPin* mpin : mterm->getMPins()) {
          // Geometry or box?
          double y = 0.5 * (mpin->getBBox().yMin() + mpin->getBBox().yMax());
          minPwr = std::min(minPwr, y);
          maxPwr = std::max(maxPwr, y);

          for (dbBox* mbox : mpin->getGeometry()) {
            dbTechLayer* layer = mbox->getTechLayer();
            pwrLayers_.insert(layer);
          }
        }
      } else if (mterm->getSigType() == dbSigType::GROUND) {
        isGnd = true;
        for (dbMPin* mpin : mterm->getMPins()) {
          // Geometry or box?
          double y = 0.5 * (mpin->getBBox().yMin() + mpin->getBBox().yMax());
          minGnd = std::min(minGnd, y);
          maxGnd = std::max(maxGnd, y);

          for (dbBox* mbox : mpin->getGeometry()) {
            dbTechLayer* layer = mbox->getTechLayer();
            gndLayers_.insert(layer);
          }
        }
      }
    }
    int topPwr = RowPower_UNK;
    int botPwr = RowPower_UNK;
    if (isVdd && isGnd) {
      topPwr = (maxPwr > maxGnd) ? RowPower_VDD : RowPower_VSS;
      botPwr = (minPwr < minGnd) ? RowPower_VDD : RowPower_VSS;
    }

    masterPwrs_[master] = std::make_pair(topPwr, botPwr);
  }
}

////////////////////////////////////////////////////////////////
void Optdp::createNetwork() {
  std::unordered_map<odb::dbInst*, Node*>::iterator it_n;
  std::unordered_map<odb::dbNet*, Edge*>::iterator it_e;
  std::unordered_map<odb::dbBTerm*, Node*>::iterator it_p;
  std::unordered_map<dbMaster*, std::pair<int, int> >::iterator it_m;

  dbBlock* block = db_->getChip()->getBlock();

  pwrLayers_.clear();
  gndLayers_.clear();

  // I allocate things statically, so I need to do some counting.

  dbSet<dbInst> insts = block->getInsts();
  dbSet<dbNet> nets = block->getNets();
  dbSet<dbBTerm> bterms = block->getBTerms();

  int errors = 0;

  // Number of this and that.
  int nTerminals = bterms.size();
  int nNodes = 0;
  int nEdges = 0;
  int nPins = 0;
  for (dbInst* inst : insts) {
    dbMasterType type = inst->getMaster()->getType();
    if (!type.isCore() && !type.isBlock()) {
      continue;
    }
    ++nNodes;
  }

  for (dbNet* net : nets) {
    //dbSigType netType = net->getSigType();
    // Should probably skip global nets.
    ++nEdges;

    nPins += net->getITerms().size();
    nPins += net->getBTerms().size();
  }

  logger_->info(DPO, 100, "Created network with {:d} cells, {:d} terminals, "
                "{:d} edges and {:d} pins.",
                nNodes, nTerminals, nEdges, nPins);

  // Create and allocate the nodes.  I require nodes for
  // placeable instances as well as terminals.
  network_->resizeNodes(nNodes + nTerminals);
  network_->resizeEdges(nEdges);

  // Return instances to a north orientation.  This makes
  // importing easier.
  for (dbInst* inst : insts) {
    dbMasterType type = inst->getMaster()->getType();
    if (!type.isCore() && !type.isBlock()) {
      continue;
    }
    inst->setLocationOrient(dbOrientType::R0);
  }

  // Populate nodes.
  int n = 0;
  for (dbInst* inst : insts) {
    auto type = inst->getMaster()->getType();
    if (!type.isCore() && !type.isBlock()) {
      continue;
    }

    Node* ndi = network_->getNode(n);
    instMap_[inst] = ndi;

    double xc = inst->getBBox()->xMin() + 0.5 * inst->getMaster()->getWidth();
    double yc = inst->getBBox()->yMin() + 0.5 * inst->getMaster()->getHeight();

    // Name of inst.
    network_->setNodeName(n, inst->getName().c_str());

    // Fill in data.
    ndi->setType(NodeType_CELL);
    ndi->setId(n);
    ndi->setFixed(inst->isFixed() ? NodeFixed_FIXED_XY : NodeFixed_NOT_FIXED);
    ndi->setAttributes(NodeAttributes_EMPTY);

    // Determine allowed orientations.  Current orientation
    // is N, since we reset everything to this orientation.
    unsigned orientations = Orientation_N;
    if (inst->getMaster()->getSymmetryX() &&
        inst->getMaster()->getSymmetryY()) {
      orientations |= Orientation_FN;
      orientations |= Orientation_FS;
      orientations |= Orientation_S;
    } else if (inst->getMaster()->getSymmetryX()) {
      orientations |= Orientation_FS;
    } else if (inst->getMaster()->getSymmetryY()) {
      orientations |= Orientation_FN;
    }
    // else...  Account for R90?
    ndi->setAvailOrient(orientations);
    ndi->setCurrOrient(Orientation_N);
    ndi->setHeight(inst->getMaster()->getHeight());
    ndi->setWidth(inst->getMaster()->getWidth());

    ndi->setOrigX(xc);
    ndi->setOrigY(yc);
    ndi->setX(xc);
    ndi->setY(yc);

    // Won't use edge types.
    ndi->setRightEdgeType(EDGETYPE_DEFAULT);
    ndi->setLeftEdgeType(EDGETYPE_DEFAULT);

    // Set the top and bottom power.
    it_m = masterPwrs_.find(inst->getMaster());
    if (masterPwrs_.end() == it_m) {
      ndi->setBottomPower(RowPower_UNK);
      ndi->setTopPower(RowPower_UNK);
    } else {
      ndi->setBottomPower(it_m->second.second);
      ndi->setTopPower(it_m->second.first);
    }

    // Regions setup later!
    ndi->setRegionId(0);

    ++n;  // Next node.
  }
  for (dbBTerm* bterm : bterms) {
    Node* ndi = network_->getNode(n);
    termMap_[bterm] = ndi;

    // Name of terminal.
    network_->setNodeName(n, bterm->getName().c_str());

    // Fill in data.
    ndi->setId(n);
    ndi->setType(NodeType_TERMINAL); // Should be terminal, not terminal_NI, I think.
    ndi->setFixed(NodeFixed_FIXED_XY);
    ndi->setAttributes(NodeAttributes_EMPTY);
    ndi->setAvailOrient(Orientation_N);
    ndi->setCurrOrient(Orientation_N);

    double ww = (bterm->getBBox().xMax() - bterm->getBBox().xMin());
    double hh = (bterm->getBBox().yMax() - bterm->getBBox().yMax());
    double xx = (bterm->getBBox().xMax() + bterm->getBBox().xMin()) * 0.5;
    double yy = (bterm->getBBox().yMax() + bterm->getBBox().yMax()) * 0.5;

    ndi->setHeight(hh);
    ndi->setWidth(ww);

    ndi->setOrigX(xx);
    ndi->setOrigY(yy);
    ndi->setX(xx);
    ndi->setY(yy);

    // Not relevant for terminal.
    ndi->setRightEdgeType(EDGETYPE_DEFAULT);
    ndi->setLeftEdgeType(EDGETYPE_DEFAULT);

    // Not relevant for terminal.
    ndi->setBottomPower(RowPower_UNK);
    ndi->setTopPower(RowPower_UNK);

    // Not relevant for terminal.
    ndi->setRegionId(0);  // Set in another routine.

    ++n;  // Next node.
  }
  if (n != nNodes + nTerminals) {
    logger_->error(DPO, 104, "Unexpected total node count.  Expected {:d}, but got {:d}",
        (nNodes+nTerminals), n);
    ++errors;
  }

  // Populate edges and pins.
  int e = 0;
  int p = 0;
  for (dbNet* net : nets) {
    // Skip globals and pre-routes?
    // dbSigType netType = net->getSigType();

    Edge* edi = network_->getEdge(e);
    edi->setId(e);
    netMap_[net] = edi;

    // Name of edge.
    network_->setEdgeName(e, net->getName().c_str());

    for (dbITerm* iTerm : net->getITerms()) {
      it_n = instMap_.find(iTerm->getInst());
      if (instMap_.end() != it_n) {
        n = it_n->second->getId();  // The node id.

        if (network_->getNode(n)->getId() != n || network_->getEdge(e)->getId() != e) {
          logger_->error(DPO, 108, "Improper node indexing while connecting pins.");
          ++errors;
        }

        Pin* ptr = network_->createAndAddPin(network_->getNode(n),network_->getEdge(e));

        // Pin offset. 
        dbMTerm* mTerm = iTerm->getMTerm();
        dbMaster* master = mTerm->getMaster();
        // Due to old bookshelf, my offsets are from the
        // center of the cell whereas in DEF, it's from
        // the bottom corner.
        double ww = (mTerm->getBBox().xMax() - mTerm->getBBox().xMin());
        double hh = (mTerm->getBBox().yMax() - mTerm->getBBox().yMax());
        double xx = (mTerm->getBBox().xMax() + mTerm->getBBox().xMin()) * 0.5;
        double yy = (mTerm->getBBox().yMax() + mTerm->getBBox().yMax()) * 0.5;
        double dx = xx - ((double)master->getWidth() / 2.);
        double dy = yy - ((double)master->getHeight() / 2.);

        ptr->setOffsetX(dx);
        ptr->setOffsetY(dy);
        ptr->setPinHeight(hh);
        ptr->setPinWidth(ww);
        ptr->setPinLayer(0); // Set to zero since not currently used.

        ++p;  // next pin.
      } else {
        logger_->error(DPO, 106, "Could not find node for instance while connecting pins.");
        ++errors;
      }
    }
    for (dbBTerm* bTerm : net->getBTerms()) {
      it_p = termMap_.find(bTerm);
      if (termMap_.end() != it_p) {
        n = it_p->second->getId();  // The node id.

        if (network_->getNode(n)->getId() != n || network_->getEdge(e)->getId() != e) {
          logger_->error(DPO, 109, "Improper terminal indexing while connecting pins.");
          ++errors;
        }

        Pin* ptr = network_->createAndAddPin(network_->getNode(n),network_->getEdge(e));

        // These don't need an offset.
        ptr->setOffsetX(0.0);
        ptr->setOffsetY(0.0);
        ptr->setPinHeight(0.0);
        ptr->setPinWidth(0.0);
        ptr->setPinLayer(0); // Set to zero since not currently used.

        ++p;  // next pin.
      } else {
        logger_->error(DPO, 107, "Could not find node for terminal while connecting pins.");
        ++errors;
      }
    }

    ++e;  // next edge.
  }
  if (e != nEdges) {
    logger_->error(DPO, 104, "Unexpected total edge count.  Expected {:d}, but got {:d}",
        nEdges, e);
    ++errors;
  }
  if (p != nPins) {
    logger_->error(DPO, 105, "Unexpected total pin count.  Expected {:d}, but got {:d}",
        nPins, p);
    ++errors;
  }

  if (errors != 0) {
    logger_->error(DPO, 101, "Error creating network.");
  } else {
    logger_->info(DPO, 102, "Network stats: inst {}, edges {}, pins {}",
                  network_->getNumNodes(), network_->getNumEdges(), network_->getNumPins());
  }
}
////////////////////////////////////////////////////////////////
void Optdp::createArchitecture() {
  dbBlock* block = db_->getChip()->getBlock();

  dbSet<dbRow> rows = block->getRows();

  odb::Rect coreRect;
  block->getCoreArea(coreRect);
  odb::Rect dieRect;
  block->getDieArea(dieRect);



  for (dbRow* row : rows) {
    if (row->getDirection() != odb::dbRowDir::HORIZONTAL) {
      // error.
      continue;
    }
    dbSite* site = row->getSite();
    int originX;
    int originY;
    row->getOrigin(originX, originY);

    Architecture::Row* archRow = arch_->createAndAddRow();

    archRow->setBottom((double)originY);
    archRow->setHeight((double)site->getHeight());
    archRow->setSiteWidth((double)site->getWidth());
    archRow->setSiteSpacing((double)row->getSpacing());
    archRow->m_subRowOrigin = (double)originX;
    archRow->setNumSites(row->getSiteCount());

    // Set defaults.  Top and bottom power is set below.
    archRow->m_powerBot = RowPower_UNK;
    archRow->m_powerTop = RowPower_UNK;

    // Symmetry.  From the site.
    unsigned symmetry = 0x00000000;
    if (site->getSymmetryX()) {
      symmetry |= dpo::Symmetry_X;
    }
    if (site->getSymmetryY()) {
      symmetry |= dpo::Symmetry_Y;
    }
    if (site->getSymmetryR90()) {
      symmetry |= dpo::Symmetry_ROT90;
    }
    archRow->m_siteSymmetry = symmetry;

    // Orientation.  From the row.
    unsigned orient = Orientation_N;
    switch (row->getOrient()) {
    case dbOrientType::R0    : orient = dpo::Orientation_N  ; break;
    case dbOrientType::MY    : orient = dpo::Orientation_FN ; break;
    case dbOrientType::MX    : orient = dpo::Orientation_FS ; break;
    case dbOrientType::R180  : orient = dpo::Orientation_S  ; break;
    case dbOrientType::R90   : orient = dpo::Orientation_E  ; break;
    case dbOrientType::MXR90 : orient = dpo::Orientation_FE ; break;
    case dbOrientType::R270  : orient = dpo::Orientation_W  ; break;
    case dbOrientType::MYR90 : orient = dpo::Orientation_FW ; break;
    default: break;
    }
    archRow->m_siteOrient = orient;
  }

  // Get surrounding box.
  {
    double xmin = std::numeric_limits<double>::max();
    double xmax = std::numeric_limits<double>::lowest();
    double ymin = std::numeric_limits<double>::max();
    double ymax = std::numeric_limits<double>::lowest();
    for (int r = 0; r < arch_->getNumRows(); r++) {
      Architecture::Row* row = arch_->getRow(r);

      double lx = row->getLeft();
      double rx = row->getRight();

      double yb = row->getBottom();
      double yt = row->getTop();

      xmin = std::min(xmin, lx);
      xmax = std::max(xmax, rx);
      ymin = std::min(ymin, yb);
      ymax = std::max(ymax, yt);
    }
    if (xmin != (double)dieRect.xMin() ||
        xmax != (double)dieRect.xMax()) {
      xmin = dieRect.xMin();
      xmax = dieRect.xMax();
    }
    arch_->setMinX(xmin);
    arch_->setMaxX(xmax);
    arch_->setMinY(ymin);
    arch_->setMaxY(ymax);
  }

  for (int r = 0; r < arch_->getNumRows(); r++) {
    int numSites = arch_->getRow(r)->getNumSites();
    double originX = arch_->getRow(r)->getLeft();
    double siteSpacing = arch_->getRow(r)->getSiteSpacing();

    double lx = originX;
    double rx = originX + numSites * siteSpacing;
    if (lx < arch_->getMinX() || rx > arch_->getMaxX()) {
      if (lx < arch_->getMinX()) {
        originX = arch_->getMinX();
      }
      rx = originX + numSites * siteSpacing;
      if (rx > arch_->getMaxX()) {
        numSites = (int)((arch_->getMaxX() - originX) / siteSpacing);
      }

      if (arch_->getRow(r)->m_subRowOrigin != originX) {
        arch_->getRow(r)->m_subRowOrigin = originX;
      }
      if (arch_->getRow(r)->getNumSites() != numSites) {
        arch_->getRow(r)->setNumSites(numSites);
      }
    }
  }

  // Need the power running across the bottom and top of each
  // row.  I think the way to do this is to look for power
  // and ground nets and then look at the special wires.
  // Not sure, though, of the best way to pick those that
  // actually touch the cells (i.e., which layer?).
  for (dbNet* net : block->getNets()) {
    if (!net->isSpecial()) {
      continue;
    }
    if (!(net->getSigType() == dbSigType::POWER ||
          net->getSigType() == dbSigType::GROUND)) {
      continue;
    }
    int pwr =
        (net->getSigType() == dbSigType::POWER) ? RowPower_VDD : RowPower_VSS;
    for (dbSWire* swire : net->getSWires()) {
      if (swire->getWireType() != dbWireType::ROUTED) {
        continue;
      }

      for (dbSBox* sbox : swire->getWires()) {
        if (sbox->getDirection() != dbSBox::HORIZONTAL) {
          continue;
        }
        if (sbox->isVia()) {
          continue;
        }
        dbTechLayer* layer = sbox->getTechLayer();
        if (pwr == RowPower_VDD) {
          if (pwrLayers_.end() == pwrLayers_.find(layer)) {
            continue;
          }
        } else if (pwr == RowPower_VSS) {
          if (gndLayers_.end() == gndLayers_.find(layer)) {
            continue;
          }
        }

        Rect rect;
        sbox->getBox(rect);
        for (size_t r = 0; r < arch_->getNumRows(); r++) {
          double yb = arch_->getRow(r)->getBottom();
          double yt = arch_->getRow(r)->getTop();

          if (yb >= rect.yMin() && yb <= rect.yMax()) {
            arch_->getRow(r)->m_powerBot = pwr;
          }
          if (yt >= rect.yMin() && yt <= rect.yMax()) {
            arch_->getRow(r)->m_powerTop = pwr;
          }
        }
      }
    }
  }
  arch_->postProcess(network_);
}
////////////////////////////////////////////////////////////////
void Optdp::setUpPlacementRegions() {
  double xmin, xmax, ymin, ymax;
  xmin = arch_->getMinX();
  xmax = arch_->getMaxX();
  ymin = arch_->getMinY();
  ymax = arch_->getMaxY();

  dbBlock* block = db_->getChip()->getBlock();

  std::unordered_map<odb::dbInst*, Node*>::iterator it_n;
  Architecture::Region* rptr = nullptr;
  int count = 0;

  // Default region.
  rptr = arch_->createAndAddRegion();
  rptr->m_id = count++;
  rptr->m_rects.push_back(Rectangle(xmin, ymin, xmax, ymax));
  rptr->m_xmin = xmin;
  rptr->m_xmax = xmax;
  rptr->m_ymin = ymin;
  rptr->m_ymax = ymax;

  // Hmm.  I noticed a comment in the OpenDP interface that
  // the OpenDB represents groups as regions.  I'll follow
  // the same approach and hope it is correct.
  // DEF GROUP => dbRegion with instances, no boundary, parent->region
  // DEF REGION => dbRegion no instances, boundary, parent = null
  auto db_regions = block->getRegions();
  for (auto db_region : db_regions) {
    dbRegion* parent = db_region->getParent();
    if (parent) {
      rptr = arch_->createAndAddRegion();
      rptr->m_id = count++;

      // Assuming these are the rectangles making up the region...
      auto boundaries = db_region->getParent()->getBoundaries();
      for (dbBox* boundary : boundaries) {
        Rect box;
        boundary->getBox(box);

        xmin = std::max(arch_->getMinX(), (double)box.xMin());
        xmax = std::min(arch_->getMaxX(), (double)box.xMax());
        ymin = std::max(arch_->getMinY(), (double)box.yMin());
        ymax = std::min(arch_->getMaxY(), (double)box.yMax());

        rptr->m_rects.push_back(Rectangle(xmin, ymin, xmax, ymax));
        rptr->m_xmin = std::min(xmin, rptr->m_xmin);
        rptr->m_xmax = std::max(xmax, rptr->m_xmax);
        rptr->m_ymin = std::min(ymin, rptr->m_ymin);
        rptr->m_ymax = std::max(ymax, rptr->m_ymax);
      }

      // The instances within this region.
      for (auto db_inst : db_region->getRegionInsts()) {
        it_n = instMap_.find(db_inst);
        if (instMap_.end() != it_n) {
          Node* nd = it_n->second;
          if (nd->getRegionId() == 0) {
            nd->setRegionId(rptr->m_id);
          }
        }
      }
    }
  }
  logger_->info(DPO, 103, "Number of regions is {:d}", arch_->getNumRegions());
}

}  // namespace dpo
