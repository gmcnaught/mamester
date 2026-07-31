`timescale 1ns/1ps
//============================================================================
//  Testbench: openbor_video_top (timing generator + DDR reader) end to end.
//
//  A behavioural DDR3 model serves the timing register block, the control word
//  and a test frame. The test checks that
//
//    - the published geometry reaches the timing generator (counters wrap at
//      the programmed h_total/v_total),
//    - the reader fetches lines with the programmed pitch and line count, and
//    - every displayed pixel matches the source frame, i.e. the pitch used by
//      the writer and the one used by the reader agree.
//
//  A stride bug shows up as a diagonal shear, which the per-pixel compare
//  catches on the first line.
//
//  Run: make -C fpga/sim reader
//============================================================================

module tb_video_reader;

// -- clocks ---------------------------------------------------------------
reg clk_sys = 0;                 // DDR / reader domain
always #5 clk_sys = ~clk_sys;    // 100 MHz

reg clk_vid = 0;
always #10 clk_vid = ~clk_vid;   // 50 MHz, one pixel per clock (ce_pix = 1)

reg reset = 1;
wire ce_pix = 1'b1;

// -- geometry under test (set per mode by run_mode) -----------------------
integer W  = 256;     // active width  (also the DDR pitch)
integer H  = 224;     // active height
integer HT = 336;     // h_total
integer VT = 262;     // v_total
integer HFP = 14, HSY = 30, VFP = 8, VSY = 3;
integer LINE_WORDS = 64;

// -- DDR model ------------------------------------------------------------
// Qword-addressed; the reader's addresses are physical>>3 and all sit inside
// the 1 MB region starting at 0x3A000000 (qword base 29'h07400000).
localparam [28:0] QBASE      = 29'h07400000;
localparam [28:0] CTRL_Q     = 29'h07400000;
localparam [28:0] BUF0_Q     = 29'h07400008;
localparam [28:0] TIMING_Q   = 29'h0741C000;
localparam integer MEM_WORDS = 131072;       // 1 MB / 8

reg [63:0] mem [0:MEM_WORDS-1];

wire        ddr_busy = 1'b0;
wire  [7:0] ddr_burstcnt;
wire [28:0] ddr_addr;
reg  [63:0] ddr_dout;
reg         ddr_dout_ready;
wire        ddr_rd;
wire [63:0] ddr_din;
wire  [7:0] ddr_be;
wire        ddr_we;

integer burst_rem = 0;
reg [28:0] burst_addr;

always @(posedge clk_sys) begin
    ddr_dout_ready <= 1'b0;
    if (reset) begin
        burst_rem <= 0;
    end
    else if (ddr_rd && burst_rem == 0) begin
        burst_addr <= ddr_addr;
        burst_rem  <= ddr_burstcnt;
    end
    else if (burst_rem > 0) begin
        ddr_dout       <= mem[(burst_addr - QBASE) % MEM_WORDS];
        ddr_dout_ready <= 1'b1;
        burst_addr     <= burst_addr + 29'd1;
        burst_rem      <= burst_rem - 1;
    end
end

// Writes (LEAN_SCANOUT=1 issues none, but accept them anyway).
always @(posedge clk_sys)
    if (ddr_we) mem[(ddr_addr - QBASE) % MEM_WORDS] <= ddr_din;

// -- DUT ------------------------------------------------------------------
wire [7:0] vga_r, vga_g, vga_b;
wire       vga_hs, vga_vs, vga_de;
wire       nv_active;
wire [23:0] ce_inc;

openbor_video_top #(.LEAN_SCANOUT(1)) dut (
    .clk_sys(clk_sys), .clk_vid(clk_vid), .ce_pix(ce_pix), .reset(reset),
    .ddr_busy(ddr_busy), .ddr_burstcnt(ddr_burstcnt), .ddr_addr(ddr_addr),
    .ddr_dout(ddr_dout), .ddr_dout_ready(ddr_dout_ready), .ddr_rd(ddr_rd),
    .ddr_din(ddr_din), .ddr_be(ddr_be), .ddr_we(ddr_we),
    .vga_r(vga_r), .vga_g(vga_g), .vga_b(vga_b),
    .vga_hs(vga_hs), .vga_vs(vga_vs), .vga_de(vga_de),
    .enable(1'b1), .active(nv_active), .vsync_out(), .ce_inc(ce_inc),
    .h_offset(3'd0), .v_offset(3'd0),
    .joystick_0(32'd0), .joystick_1(32'd0), .joystick_2(32'd0),
    .joystick_3(32'd0), .joystick_l_analog_0(16'd0),
    .ioctl_download(1'b0), .ioctl_wr(1'b0), .ioctl_addr(27'd0),
    .ioctl_dout(8'd0), .ioctl_wait(),
    .clk_audio(clk_sys), .audio_l(), .audio_r()
);

// -- test frame -----------------------------------------------------------
function [15:0] src_pixel(input integer x, input integer y);
    src_pixel = ((y * W + x) & 16'hFFFF) ^ 16'hA5A5;
endfunction

task load_frame;
    integer x, y, idx;
    reg [63:0] w;
begin
    for (y = 0; y < H; y = y + 1)
        for (x = 0; x < W; x = x + 4) begin
            w = {src_pixel(x+3, y), src_pixel(x+2, y),
                 src_pixel(x+1, y), src_pixel(x,   y)};
            idx = (BUF0_Q - QBASE) + y * LINE_WORDS + (x / 4);
            mem[idx] = w;
        end
end
endtask

task publish_timing(input integer ha, input integer hf, input integer hs, input integer ht,
                    input integer va, input integer vf, input integer vs, input integer vt,
                    input [23:0] inc);
    integer base;
begin
    base = TIMING_Q - QBASE;
    mem[base + 0] = 64'h0000_0000_5449_4D31;   // magic "TIM1" in [31:0]
    mem[base + 1] = {ht[15:0], hs[15:0], hf[15:0], ha[15:0]};
    mem[base + 2] = {vt[15:0], vs[15:0], vf[15:0], va[15:0]};
    mem[base + 3] = {40'd0, inc};
end
endtask

// -- pixel checker --------------------------------------------------------
integer errors = 0;
integer checked = 0;
integer px = 0, py = 0;
reg     checking = 0;

wire [15:0] exp_pix = src_pixel(px, py);
wire  [7:0] exp_r = {exp_pix[15:11], exp_pix[15:13]};
wire  [7:0] exp_g = {exp_pix[10:5],  exp_pix[10:9]};
wire  [7:0] exp_b = {exp_pix[4:0],   exp_pix[4:2]};

always @(posedge clk_vid) begin
    if (vga_de) begin
        if (checking) begin
            if (vga_r !== exp_r || vga_g !== exp_g || vga_b !== exp_b) begin
                if (errors < 10)
                    $display("  FAIL pixel (%0d,%0d): got %02x%02x%02x want %02x%02x%02x",
                             px, py, vga_r, vga_g, vga_b, exp_r, exp_g, exp_b);
                errors = errors + 1;
            end
            checked = checked + 1;
        end
        if (px == W - 1) begin px = 0; py = py + 1; end
        else px = px + 1;
    end
    else if (!vga_de && px != 0) begin
        // Line ended early — the active width does not match W.
        if (checking && errors < 10)
            $display("  FAIL line %0d ended after %0d pixels (want %0d)", py, px, W);
        if (checking) errors = errors + 1;
        px = 0; py = py + 1;
    end
    if (dut.tim_new_frame) begin px = 0; py = 0; end
end

// Run one geometry: load a frame at that pitch, publish the modeline, let the
// reader confirm and apply it, then compare a whole frame of pixels.
task run_mode(input [255:0] label,
              input integer w, input integer h,
              input integer hfp, input integer hsy, input integer ht,
              input integer vfp, input integer vsy, input integer vt,
              input [23:0] inc);
    integer i, err0;
begin
    W = w; H = h; HT = ht; VT = vt;
    HFP = hfp; HSY = hsy; VFP = vfp; VSY = vsy;
    LINE_WORDS = w / 4;

    load_frame;
    publish_timing(w, hfp, hsy, ht, h, vfp, vsy, vt, inc);

    // Two identical reads to confirm, one frame boundary to latch, and a
    // couple more for the reader to fetch a full frame at the new pitch.
    for (i = 0; i < 8; i = i + 1) begin
        @(posedge clk_vid);
        while (!dut.tim_new_frame) @(posedge clk_vid);
        mem[CTRL_Q - QBASE] = mem[CTRL_Q - QBASE] + 64'd4;
    end

    err0 = errors;
    if (dut.timing.h_total !== ht[11:0] || dut.timing.v_total !== vt[11:0]) begin
        $display("  FAIL geometry not applied: h_total=%0d v_total=%0d (want %0d/%0d)",
                 dut.timing.h_total, dut.timing.v_total, ht, vt);
        errors = errors + 1;
    end
    if (ce_inc !== inc) begin
        $display("  FAIL ce_inc=%0d (want %0d)", ce_inc, inc);
        errors = errors + 1;
    end
    if (!nv_active) begin
        $display("  FAIL reader never reported a ready frame");
        errors = errors + 1;
    end

    checked = 0;
    @(posedge clk_vid);
    while (!dut.tim_new_frame) @(posedge clk_vid);
    px = 0; py = 0; checking = 1;
    @(posedge clk_vid);
    while (!dut.tim_new_frame) @(posedge clk_vid);
    checking = 0;

    if (checked != w * h) begin
        $display("  FAIL checked %0d pixels, want %0d", checked, w * h);
        errors = errors + 1;
    end
    $display("mode %0s: %0dx%0d, %0d pixels compared, %0d errors",
             label, w, h, checked, errors - err0);
end
endtask

initial begin
    integer i;
    if ($test$plusargs("vcd")) begin
        $dumpfile("tb_video_reader.vcd");
        $dumpvars(0, tb_video_reader);
    end

    for (i = 0; i < MEM_WORDS; i = i + 1) mem[i] = 64'd0;
    load_frame;
    publish_timing(W, HFP, HSY, HT, H, VFP, VSY, VT, 24'd1650478);
    mem[CTRL_Q - QBASE] = 64'h0000_0000_0000_0004;   // frame 1, buffer 0

    repeat (20) @(posedge clk_sys);
    reset = 0;

    run_mode("256x224 (gng)",  256, 224, 14, 30, 336,  8, 3, 262, 24'd1650478);
    run_mode("320x240 (default)", 320, 240, 17, 38, 420,  2, 3, 262, 24'd2060470);
    run_mode("384x224 (sf2)",  384, 224, 20, 46, 504,  8, 3, 262, 24'd2473198);

    if (errors == 0) $display("tb_video_reader: PASS");
    else             $display("tb_video_reader: FAIL (%0d errors)", errors);
    $finish;
end

initial begin
    #200_000_000;
    $display("tb_video_reader: TIMEOUT");
    $finish;
end

endmodule
