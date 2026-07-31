`timescale 1ns/1ps
//============================================================================
//  Testbench: mame_ce_pixel fractional pixel-enable generator
//
//  Checks the average rate against f_clk * inc / 2^24 for several pixel
//  clocks, and that ce_pix is never asserted on consecutive cycles (the
//  downstream timing generator and the framework assume one clock per pixel).
//
//  Run: make -C fpga/sim ce
//============================================================================

module tb_ce_pixel;

localparam real F_CLK = 53.693182e6;

reg clk = 0;
always #5 clk = ~clk;

reg         reset = 1;
reg  [23:0] inc = 24'd0;
wire        ce_pix;

mame_ce_pixel dut (.clk(clk), .reset(reset), .inc(inc), .ce_pix(ce_pix));

integer errors = 0;
integer ce_cnt, cyc_cnt;
reg     ce_prev;
reg     counting = 0;

always @(posedge clk) begin
    if (counting) begin
        cyc_cnt <= cyc_cnt + 1;
        if (ce_pix) begin
            ce_cnt <= ce_cnt + 1;
            if (ce_prev) begin
                $display("  FAIL: back-to-back ce_pix at cycle %0d", cyc_cnt);
                errors = errors + 1;
            end
        end
        ce_prev <= ce_pix;
    end
end

// Measure the realised rate for a given increment.
task check_rate(input [255:0] label, input [23:0] the_inc, input real want_hz);
    real got_hz, err_ppm;
begin
    inc = the_inc;
    @(posedge clk);
    ce_cnt = 0; cyc_cnt = 0; ce_prev = 0; counting = 1;
    repeat (200000) @(posedge clk);
    counting = 0;
    got_hz  = (ce_cnt * F_CLK) / cyc_cnt;
    err_ppm = ((got_hz - want_hz) / want_hz) * 1.0e6;
    $display("%0s: inc=%0d -> %.4f MHz (want %.4f MHz, err %.1f ppm)",
             label, the_inc, got_hz / 1.0e6, want_hz / 1.0e6, err_ppm);
    if (err_ppm > 200.0 || err_ppm < -200.0) begin
        $display("  FAIL: rate error over 200 ppm");
        errors = errors + 1;
    end
end
endtask

initial begin
    repeat (10) @(posedge clk);
    reset = 0;
    @(posedge clk);

    // inc = 0 selects the built-in default (320x240 @ 15.700 kHz H).
    check_rate("default (inc=0)", 24'd0, 6.594e6);

    // 320x240, 420x262 total, 59.92 Hz -> 420*262*59.92
    check_rate("320x240@59.92", 24'd2060470, 6.5940e6);
    // 384x224, 504x262 total, 59.94 Hz -> 504*262*59.94 = 7.9152 MHz
    check_rate("384x224@59.94", 24'd2473198, 7.9152e6);
    // 256x224, 336x262 total, 60.0 Hz -> 336*262*60 = 5.2819 MHz
    check_rate("256x224@60.00", 24'd1650478, 5.2819e6);

    if (errors == 0) $display("tb_ce_pixel: PASS");
    else             $display("tb_ce_pixel: FAIL (%0d errors)", errors);
    $finish;
end

endmodule
