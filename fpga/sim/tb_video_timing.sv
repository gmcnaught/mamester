`timescale 1ns/1ps
//============================================================================
//  Testbench: openbor_video_timing programmable geometry
//
//  For each geometry, measure one whole frame and check:
//    - active pixels (de high)      == h_active * v_active
//    - hsync low pixels             == h_sync   * v_total
//    - vsync low pixels             == v_sync   * h_total
//    - new_line pulses              == v_total
//    - new_frame pulses             == 1
//    - hcount/vcount wrap points    == h_total-1 / v_total-1
//
//  Run: make -C fpga/sim timing
//============================================================================

module tb_video_timing;

reg clk = 0;
always #5 clk = ~clk;          // 100 MHz sim clock (rate is irrelevant here)

reg reset = 1;

// One pixel per clock — geometry, not clock rate, is under test here; the
// fractional pixel enable is exercised by tb_ce_pixel.
wire ce_pix = 1'b1;

reg [11:0] h_active_in = 12'd320, h_fp_in = 12'd17, h_sync_in = 12'd38, h_total_in = 12'd420;
reg [11:0] v_active_in = 12'd240, v_fp_in = 12'd2,  v_sync_in = 12'd3,  v_total_in = 12'd262;

wire        hsync, vsync, hblank, vblank, de, new_frame, new_line;
wire [11:0] hcount, vcount, h_active_cur, v_active_cur;

openbor_video_timing dut (
    .clk(clk), .ce_pix(ce_pix), .reset(reset),
    .h_active_in(h_active_in), .h_fp_in(h_fp_in), .h_sync_in(h_sync_in), .h_total_in(h_total_in),
    .v_active_in(v_active_in), .v_fp_in(v_fp_in), .v_sync_in(v_sync_in), .v_total_in(v_total_in),
    .h_adj(5'sd0), .v_adj(4'sd0),
    .hsync(hsync), .vsync(vsync), .hblank(hblank), .vblank(vblank), .de(de),
    .hcount(hcount), .vcount(vcount), .new_frame(new_frame), .new_line(new_line),
    .h_active_cur(h_active_cur), .v_active_cur(v_active_cur)
);

integer errors = 0;

// -- per-frame measurement -------------------------------------------------
// Everything runs in this one block so arming/stopping cannot race the
// counters: arm, start on the next new_frame, stop on the one after that.
// The window is therefore exactly one frame period (h_total * v_total pixels).
integer de_cnt, hs_low, vs_low, nl_cnt, tot_cnt, max_h, max_v;
reg     armed = 0, measuring = 0;
integer meas_seq = 0;

always @(posedge clk) if (ce_pix) begin
    if (armed && new_frame) begin
        armed <= 0; measuring <= 1;
        de_cnt <= 0; hs_low <= 0; vs_low <= 0; nl_cnt <= 0; tot_cnt <= 0;
        max_h <= 0; max_v <= 0;
    end
    else if (measuring) begin
        tot_cnt <= tot_cnt + 1;
        if (de)     de_cnt <= de_cnt + 1;
        if (!hsync) hs_low <= hs_low + 1;
        if (!vsync) vs_low <= vs_low + 1;
        if (new_line) nl_cnt <= nl_cnt + 1;
        if (hcount > max_h) max_h <= hcount;
        if (vcount > max_v) max_v <= vcount;
        if (new_frame) begin measuring <= 0; meas_seq <= meas_seq + 1; end
    end
end

task expect_eq(input [127:0] name, input integer got, input integer want);
begin
    if (got !== want) begin
        $display("  FAIL %0s: got %0d want %0d", name, got, want);
        errors = errors + 1;
    end
end
endtask

task wait_frames(input integer n);
    integer i;
begin
    for (i = 0; i < n; i = i + 1) begin
        @(posedge clk); while (!(ce_pix && new_frame)) @(posedge clk);
    end
end
endtask

// Arm the measurement block and wait for one full frame to be tallied.
task measure_frame;
    integer seq0;
begin
    seq0  = meas_seq;
    armed = 1;
    while (meas_seq == seq0) @(posedge clk);
end
endtask

task check_mode(input [255:0] label,
                input integer ha, input integer hf, input integer hs, input integer ht,
                input integer va, input integer vf, input integer vs, input integer vt);
begin
    h_active_in = ha[11:0]; h_fp_in = hf[11:0]; h_sync_in = hs[11:0]; h_total_in = ht[11:0];
    v_active_in = va[11:0]; v_fp_in = vf[11:0]; v_sync_in = vs[11:0]; v_total_in = vt[11:0];

    wait_frames(3);                     // let the new geometry latch and settle
    measure_frame;

    $display("mode %0s (%0dx%0d active, %0dx%0d total)", label, ha, va, ht, vt);
    expect_eq("frame period",  tot_cnt, ht * vt);
    expect_eq("active pixels", de_cnt, ha * va);
    expect_eq("hsync pixels",  hs_low, hs * vt);
    expect_eq("vsync pixels",  vs_low, vs * ht);
    expect_eq("new_line",      nl_cnt, vt);
    expect_eq("hcount max",    max_h,  ht - 1);
    expect_eq("vcount max",    max_v,  vt - 1);
    expect_eq("h_active_cur",  h_active_cur, ha);
    expect_eq("v_active_cur",  v_active_cur, va);
end
endtask

initial begin
    if ($test$plusargs("vcd")) begin
        $dumpfile("tb_video_timing.vcd");
        $dumpvars(0, tb_video_timing);
    end

    repeat (10) @(posedge clk);
    reset = 0;

    // Default (reset) geometry must be the known-good fixed mode.
    wait_frames(2);
    expect_eq("default h_active_cur", h_active_cur, 320);
    expect_eq("default v_active_cur", v_active_cur, 240);

    check_mode("320x240 (default/OpenBOR)", 320, 17, 38, 420, 240, 2, 3, 262);
    check_mode("256x224 (gng, cps-ish)",    256, 14, 30, 336, 224, 8, 3, 262);
    check_mode("384x224 (sf2)",             384, 20, 46, 504, 224, 8, 3, 262);
    check_mode("400x254 (mk)",              400, 21, 48, 525, 254, 2, 3, 264);
    check_mode("224x384 (tate)",            224, 12, 27, 294, 384, 4, 3, 400);
    check_mode("back to 320x240",           320, 17, 38, 420, 240, 2, 3, 262);

    // Insane geometry must be ignored (counters keep the last good mode).
    h_active_in = 12'd320; h_fp_in = 12'd17; h_sync_in = 12'd38; h_total_in = 12'd100;
    wait_frames(3);
    expect_eq("insane geometry ignored (h_active_cur)", h_active_cur, 320);
    measure_frame;
    expect_eq("insane geometry ignored (active pixels)", de_cnt, 320 * 240);
    expect_eq("insane geometry ignored (frame period)", tot_cnt, 420 * 262);

    if (errors == 0) $display("tb_video_timing: PASS");
    else             $display("tb_video_timing: FAIL (%0d errors)", errors);
    $finish;
end

initial begin
    #500_000_000;
    $display("tb_video_timing: TIMEOUT");
    $finish;
end

endmodule
