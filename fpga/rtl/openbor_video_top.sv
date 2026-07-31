//============================================================================
//
//  OpenBOR Native Video Top-Level Wrapper
//
//  Instantiates the timing generator and DDR3 reader, providing a clean
//  interface for integration into OpenBOR.sv.
//
//  Runs on CLK_VIDEO (53.693 MHz) with variable CE_PIXEL for exact
//  Genesis H40 timing — 47.68 µs active, 15,700 Hz H rate.
//
//  Differences from pico8_video_top:
//    - 320x240 instead of 256x256
//    - 1:1 pixel mapping instead of 2x doubling
//
//  Cart loading via ioctl is preserved exactly as in PICO-8.
//
//  Adapted from MiSTer_PICO-8 by MiSTer Organize
//  Copyright (C) 2026 MiSTer Organize -- GPL-3.0
//
//============================================================================

module openbor_video_top #(
    // Pass-through to openbor_video_reader. 1 = lean scanout (no hybrid
    // OpenBOR-engine DDR handshake: no joystick/cart/audio-ring writes).
    parameter LEAN_SCANOUT = 0
) (
    input  wire        clk_sys,       // system clock for DDR3
    input  wire        clk_vid,       // video clock (53.693 MHz, CLK_VIDEO)
    input  wire        ce_pix,        // pixel enable (variable rate — exact Genesis H40)
    input  wire        reset,

    // DDR3 Avalon-MM master
    input  wire        ddr_busy,
    output wire  [7:0] ddr_burstcnt,
    output wire [28:0] ddr_addr,
    input  wire [63:0] ddr_dout,
    input  wire        ddr_dout_ready,
    output wire        ddr_rd,
    output wire [63:0] ddr_din,
    output wire  [7:0] ddr_be,
    output wire        ddr_we,

    // Video output (clk_vid domain)
    output wire  [7:0] vga_r,
    output wire  [7:0] vga_g,
    output wire  [7:0] vga_b,
    output wire        vga_hs,
    output wire        vga_vs,
    output wire        vga_de,

    // Control
    input  wire        enable,        // from ARM: activate native video
    output wire        active,        // module is outputting valid video
    output wire        vsync_out,     // active-low vsync for frame sync

    // CE_PIXEL phase increment for the current game (see mame_ce_pixel.sv).
    // Comes from the HPS timing registers via the reader; 0 = built-in default.
    output wire [23:0] ce_inc,

    // CRT position adjustment (0..6 from OSD)
    input  wire  [2:0] h_offset,
    input  wire  [2:0] v_offset,

    // Joystick (from hps_io, written to DDR3 for ARM)
    input  wire [31:0] joystick_0,
    input  wire [31:0] joystick_1,
    input  wire [31:0] joystick_2,
    input  wire [31:0] joystick_3,
    input  wire [15:0] joystick_l_analog_0,

    // Cart loading via ioctl
    input  wire        ioctl_download,
    input  wire        ioctl_wr,
    input  wire [26:0] ioctl_addr,
    input  wire  [7:0] ioctl_dout,
    output wire        ioctl_wait,

    // Audio output (clk_audio domain)
    input  wire        clk_audio,
    output wire [15:0] audio_l,
    output wire [15:0] audio_r
);

// -- Timing Generator --------------------------------------------------
wire        tim_hsync, tim_vsync;
wire        tim_hblank, tim_vblank;
wire        tim_de;
wire [11:0] tim_hcount;
wire [11:0] tim_vcount;
wire        tim_new_frame, tim_new_line;

// Per-game geometry, published by the reader from the HPS timing registers.
// These are ddr_clk-domain registers read in the clk_vid domain; the reader
// only publishes a value it has read twice identically, and the timing
// generator latches them at a frame boundary, so no per-bit synchroniser is
// needed (a geometry change happens once, at game launch).
wire [11:0] geo_h_active, geo_h_fp, geo_h_sync, geo_h_total;
wire [11:0] geo_v_active, geo_v_fp, geo_v_sync, geo_v_total;

// Convert OSD 3-bit (0..6) to signed adjustment
wire signed [4:0] h_adj = (h_offset == 3'd0) ?  5'sd0 :
                          (h_offset == 3'd1) ?  5'sd4 :
                          (h_offset == 3'd2) ?  5'sd8 :
                          (h_offset == 3'd3) ?  5'sd12 :
                          (h_offset == 3'd4) ? -5'sd12 :
                          (h_offset == 3'd5) ? -5'sd8 :
                                               -5'sd4;
wire signed [3:0] v_adj = (v_offset == 3'd0) ?  4'sd0 :
                          (v_offset == 3'd1) ?  4'sd1 :
                          (v_offset == 3'd2) ?  4'sd2 :
                          (v_offset == 3'd3) ?  4'sd3 :
                          (v_offset == 3'd4) ? -4'sd3 :
                          (v_offset == 3'd5) ? -4'sd2 :
                                               -4'sd1;

openbor_video_timing timing (
    .clk       (clk_vid),
    .ce_pix    (ce_pix),
    .reset     (reset),
    .h_active_in (geo_h_active),
    .h_fp_in     (geo_h_fp),
    .h_sync_in   (geo_h_sync),
    .h_total_in  (geo_h_total),
    .v_active_in (geo_v_active),
    .v_fp_in     (geo_v_fp),
    .v_sync_in   (geo_v_sync),
    .v_total_in  (geo_v_total),
    .h_adj     (h_adj),
    .v_adj     (v_adj),
    .hsync     (tim_hsync),
    .vsync     (tim_vsync),
    .hblank    (tim_hblank),
    .vblank    (tim_vblank),
    .de        (tim_de),
    .hcount    (tim_hcount),
    .vcount    (tim_vcount),
    .new_frame (tim_new_frame),
    .new_line  (tim_new_line),
    .h_active_cur (),
    .v_active_cur ()
);

// -- DDR3 Pixel Reader -------------------------------------------------
wire [7:0]  reader_r, reader_g, reader_b;
wire        reader_frame_ready;

openbor_video_reader #(.LEAN_SCANOUT(LEAN_SCANOUT)) reader (
    .ddr_clk        (clk_sys),
    .ddr_busy       (ddr_busy),
    .ddr_burstcnt   (ddr_burstcnt),
    .ddr_addr       (ddr_addr),
    .ddr_dout       (ddr_dout),
    .ddr_dout_ready (ddr_dout_ready),
    .ddr_rd         (ddr_rd),
    .ddr_din        (ddr_din),
    .ddr_be         (ddr_be),
    .ddr_we         (ddr_we),

    .clk_vid        (clk_vid),
    .ce_pix         (ce_pix),
    .reset          (reset),

    .de             (tim_de),
    .hblank         (tim_hblank),
    .vblank         (tim_vblank),
    .new_frame      (tim_new_frame),
    .new_line       (tim_new_line),
    .vcount         (tim_vcount),

    .tim_h_active   (geo_h_active),
    .tim_h_fp       (geo_h_fp),
    .tim_h_sync     (geo_h_sync),
    .tim_h_total    (geo_h_total),
    .tim_v_active   (geo_v_active),
    .tim_v_fp       (geo_v_fp),
    .tim_v_sync     (geo_v_sync),
    .tim_v_total    (geo_v_total),
    .tim_ce_inc     (ce_inc),

    .r_out          (reader_r),
    .g_out          (reader_g),
    .b_out          (reader_b),

    .enable         (enable),
    .frame_ready    (reader_frame_ready),

    .joystick_0     (joystick_0),
    .joystick_1     (joystick_1),
    .joystick_2     (joystick_2),
    .joystick_3     (joystick_3),
    .joystick_l_analog_0 (joystick_l_analog_0),

    .ioctl_download (ioctl_download),
    .ioctl_wr       (ioctl_wr),
    .ioctl_addr     (ioctl_addr),
    .ioctl_dout     (ioctl_dout),
    .ioctl_wait     (ioctl_wait),

    .clk_audio      (clk_audio),
    .audio_l        (audio_l),
    .audio_r        (audio_r)
);

// -- Output assignments ------------------------------------------------
assign vga_r     = reader_r;
assign vga_g     = reader_g;
assign vga_b     = reader_b;
assign vga_hs    = tim_hsync;
assign vga_vs    = tim_vsync;
assign vga_de    = tim_de;
assign active    = enable & reader_frame_ready;
assign vsync_out = tim_vsync;

endmodule
