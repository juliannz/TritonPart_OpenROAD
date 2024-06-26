# repair_timing -hold with global route parasitics
source helpers.tcl
read_liberty sky130hd/sky130hd_tt.lib
read_lef sky130hd/sky130hd.tlef
read_lef sky130hd/sky130hd_std_cell.lef
read_def repair_hold10.def

create_clock -period 2 clk
set_propagated_clock clk

source sky130hd/sky130hd.vars
source sky130hd/sky130hd.rc
# These are the values for met2 pF/um, kohm/um
#set_wire_rc -resistance 0.000893 -capacitance 0.000078
#set_wire_rc -layer met2
# Intentionaly reduce RC so there is a large discrepancy between
# placement and global route parasitics.
set_wire_rc -resistance 0.0001 -capacitance 0.00001
set_routing_layers -signal $global_routing_layers
global_route
estimate_parasitics -global_routing

report_worst_slack -min

write_verilog_for_eqy repair_hold10 before "None"
repair_timing -hold
run_equivalence_test repair_hold10 ./sky130hd/work_around_yosys/ "None"

# to compare with updated parasitics below
report_worst_slack -min

detailed_placement

# check slacks with fresh parasitics
set_routing_layers -signal met1-met5
global_route
estimate_parasitics -global_routing
report_worst_slack -min
