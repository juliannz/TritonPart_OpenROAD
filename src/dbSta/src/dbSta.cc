// OpenStaDB, OpenSTA on OpenDB
// Copyright (c) 2019, Parallax Software, Inc.
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "db_sta/dbSta.hh"

#include <tcl.h>

#include "sta/StaMain.hh"
#include "sta/Graph.hh"
#include "sta/Clock.hh"
#include "sta/Sdc.hh"
#include "sta/Search.hh"
#include "sta/Bfs.hh"
#include "db_sta/dbNetwork.hh"
#include "db_sta/MakeDbSta.hh"
#include "opendb/db.h"
#include "openroad/OpenRoad.hh"
#include "dbSdcNetwork.hh"

namespace ord {

sta::dbSta *
makeDbSta()
{
  return new sta::dbSta;
}

void
initDbSta(OpenRoad *openroad)
{
  auto* sta = openroad->getSta();
  sta->init(openroad->tclInterp(), openroad->getDb());
  openroad->addObserver(sta);
}

void
deleteDbSta(sta::dbSta *sta)
{
  delete sta;
}

} // namespace ord

////////////////////////////////////////////////////////////////

namespace sta {

dbSta *
makeBlockSta(dbBlock *block)
{
  Sta *sta = Sta::sta();
  dbSta *sta2 = new dbSta;
  sta2->makeComponents();
  sta2->getDbNetwork()->setBlock(block);
  sta2->setTclInterp(sta->tclInterp());
  sta2->copyUnits(sta->units());
  return sta2;
}

extern "C" {
extern int Dbsta_Init(Tcl_Interp *interp);
}

extern const char *dbsta_tcl_inits[];

dbSta::dbSta() :
  Sta(),
  db_(nullptr)
{
}

void
dbSta::init(Tcl_Interp *tcl_interp,
	    dbDatabase *db)
{
  initSta();
  Sta::setSta(this);
  db_ = db;
  makeComponents();
  setTclInterp(tcl_interp);
  // Define swig TCL commands.
  Dbsta_Init(tcl_interp);
  // Eval encoded sta TCL sources.
  evalTclInit(tcl_interp, dbsta_tcl_inits);
}

// Wrapper to init network db.
void
dbSta::makeComponents()
{
  Sta::makeComponents();
  db_network_->setDb(db_);
}

void
dbSta::makeNetwork()
{
  db_network_ = new class dbNetwork();
  network_ = db_network_;
}

void
dbSta::makeSdcNetwork()
{
  sdc_network_ = new dbSdcNetwork(network_);
}

void dbSta::postReadLef(dbTech* tech,
			dbLib* library)
{
  if (library) {
    db_network_->readLefAfter(library);
  }
}

void dbSta::postReadDef(dbBlock* block)
{
  db_network_->readDefAfter(block);
}

void dbSta::postReadDb(dbDatabase* db)
{
  db_network_->readDbAfter(db);
}

Slack
dbSta::netSlack(const dbNet *db_net,
		const MinMax *min_max)
{
  const Net *net = db_network_->dbToSta(db_net);
  return netSlack(net, min_max);
}

void
dbSta::findClkNets(// Return value.
		   std::set<dbNet*> &clk_nets)
{
  ensureClkNetwork();
  for (Clock *clk : sdc_->clks()) {
    for (const Pin *pin : *pins(clk)) {
      Net *net = network_->net(pin);      
      if (net)
	clk_nets.insert(db_network_->staToDb(net));
    }
  }
}

void
dbSta::findClkNets(const Clock *clk,
		   // Return value.
		   std::set<dbNet*> &clk_nets)
{
  ensureClkNetwork();
  for (const Pin *pin : *pins(clk)) {
    Net *net = network_->net(pin);      
    if (net)
      clk_nets.insert(db_network_->staToDb(net));
  }
}

} // namespace sta
