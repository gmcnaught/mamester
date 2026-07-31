# Icarus Verilog simulation for the MAMESTer video RTL.
#
#   make          # run every testbench, fail on any FAIL/TIMEOUT
#   make timing   # programmable timing generator only
#   make ce       # fractional CE_PIXEL generator only
#   make clean
#
# Needs iverilog (brew install icarus-verilog).

IVERILOG ?= iverilog
VVP      ?= vvp
RTL      := ../rtl
FLAGS    := -g2012 -Wall -I$(RTL)

.PHONY: all timing ce reader clean

all: timing ce reader
	@echo "fpga/sim: all testbenches passed"

timing: tb_video_timing.vvp
	@$(VVP) $< | tee $@.log
	@! grep -qE 'FAIL|TIMEOUT|ERROR' $@.log

ce: tb_ce_pixel.vvp
	@$(VVP) $< | tee $@.log
	@! grep -qE 'FAIL|TIMEOUT|ERROR' $@.log

tb_video_timing.vvp: tb_video_timing.sv $(RTL)/openbor_video_timing.sv
	$(IVERILOG) $(FLAGS) -o $@ $^

tb_ce_pixel.vvp: tb_ce_pixel.sv $(RTL)/mame_ce_pixel.sv
	$(IVERILOG) $(FLAGS) -o $@ $^

clean:
	rm -f *.vvp *.vcd *.log

reader: tb_video_reader.vvp
	@$(VVP) $< | tee $@.log
	@! grep -qE 'FAIL|TIMEOUT|ERROR' $@.log

tb_video_reader.vvp: tb_video_reader.sv $(RTL)/openbor_video_top.sv $(RTL)/openbor_video_timing.sv $(RTL)/openbor_video_reader.sv $(RTL)/mame_ce_pixel.sv stubs/dcfifo.v
	$(IVERILOG) $(FLAGS) -o $@ $^
