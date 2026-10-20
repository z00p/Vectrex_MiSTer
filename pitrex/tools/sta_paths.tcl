# Worst setup paths per failing clock. Run from the repository root:
#   quartus_sta -t pitrex/tools/sta_paths.tcl
project_open Vectrex
create_timing_netlist -model slow
read_sdc
update_timing_netlist
set clk [get_clocks {emu|pll_vfb|pll_vfb_inst|altera_pll_i|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}]
report_timing -setup -npaths 40 -detail summary -to_clock $clk -file sta_vfb_paths.txt
set hc [get_clocks {pll_hdmi|pll_hdmi_inst|altera_pll_i|cyclonev_pll|counter[0].output_counter|divclk}]
report_timing -setup -npaths 10 -detail summary -to_clock $hc -file sta_hdmi_paths.txt
delete_timing_netlist
project_close
