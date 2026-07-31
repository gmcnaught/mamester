//============================================================================
//
//  MAMESTer Native Video Timing Generator (register-programmable)
//
//  Geometry is supplied as inputs and latched at the end of each frame, so a
//  MAME driver can be presented at its native H/V/refresh instead of a fixed
//  320x240. Back porch is implied (total - active - front porch - sync) and is
//  therefore not an input.
//
//  Defaults (used after reset, and whenever the supplied geometry fails the
//  sanity check) are the previous fixed mode: 320x240 active, 420x262 total —
//  exact Genesis H40 / NTSC. With the fractional CE_PIXEL generator
//  (mame_ce_pixel.sv) at 6.594 MHz that is 15.700 kHz H / 59.92 Hz V.
//
//  Pixel clock is CLK_VIDEO gated by ce_pix; every count below is in pixels.
//
//  Adapted from MiSTer_PICO-8 by MiSTer Organize
//  Copyright (C) 2026 MiSTer Organize -- GPL-3.0
//
//============================================================================

module openbor_video_timing (
    input  wire        clk,        // CLK_VIDEO (53.693 MHz)
    input  wire        ce_pix,     // pixel enable (fractional, from mame_ce_pixel)
    input  wire        reset,

    // -- Programmable geometry (latched at frame end) --------------------
    input  wire [11:0] h_active_in,
    input  wire [11:0] h_fp_in,
    input  wire [11:0] h_sync_in,
    input  wire [11:0] h_total_in,
    input  wire [11:0] v_active_in,
    input  wire [11:0] v_fp_in,
    input  wire [11:0] v_sync_in,
    input  wire [11:0] v_total_in,

    // CRT position offset (signed, from OSD)
    input  wire signed [4:0] h_adj,  // horizontal: positive = shift right
    input  wire signed [3:0] v_adj,  // vertical: positive = shift down

    output reg         hsync,      // active low
    output reg         vsync,      // active low
    output reg         hblank,
    output reg         vblank,
    output reg         de,         // data enable = ~(hblank | vblank)
    output reg  [11:0] hcount,
    output reg  [11:0] vcount,
    output reg         new_frame,  // pulse at vblank start
    output reg         new_line,   // pulse at hblank start

    // Geometry actually in use (for the reader's line fetch)
    output wire [11:0] h_active_cur,
    output wire [11:0] v_active_cur
);

// -- Fallback geometry -------------------------------------------------
localparam [11:0] DEF_H_ACTIVE = 12'd320;
localparam [11:0] DEF_H_FP     = 12'd17;
localparam [11:0] DEF_H_SYNC   = 12'd38;
localparam [11:0] DEF_H_TOTAL  = 12'd420;   // 320+17+38+45 (Genesis H40)
localparam [11:0] DEF_V_ACTIVE = 12'd240;
localparam [11:0] DEF_V_FP     = 12'd2;
localparam [11:0] DEF_V_SYNC   = 12'd3;
localparam [11:0] DEF_V_TOTAL  = 12'd262;   // 240+2+3+17 (Genesis NTSC)

// -- Latched geometry --------------------------------------------------
reg [11:0] h_active, h_fp, h_sync_w, h_total;
reg [11:0] v_active, v_fp, v_sync_w, v_total;

assign h_active_cur = h_active;
assign v_active_cur = v_active;

// Reject geometry that would lock the counters up (total must leave room for
// active + front porch + sync, and the active area must be a sane size).
wire [12:0] h_need = {1'b0, h_active_in} + {1'b0, h_fp_in} + {1'b0, h_sync_in};
wire [12:0] v_need = {1'b0, v_active_in} + {1'b0, v_fp_in} + {1'b0, v_sync_in};
wire params_sane = (h_active_in >= 12'd64) && (v_active_in >= 12'd64) &&
                   ({1'b0, h_total_in} > h_need) && ({1'b0, v_total_in} > v_need);

// Latch geometry at the very last pixel of the frame so a change can never
// take effect mid-frame.
wire frame_last_pixel = (hcount == h_total - 12'd1) && (vcount == v_total - 12'd1);

// -- Derived boundaries — adjusted by OSD H/V position offset. ----------
wire [11:0] h_sync_start = h_active + h_fp + {{7{h_adj[4]}}, h_adj};
wire [11:0] h_sync_end   = h_sync_start + h_sync_w;
wire [11:0] v_sync_start = v_active + v_fp + {{8{v_adj[3]}}, v_adj};
wire [11:0] v_sync_end   = v_sync_start + v_sync_w;

always @(posedge clk) begin
    if (reset) begin
        hcount    <= 12'd0;
        vcount    <= 12'd0;
        hsync     <= 1'b1;
        vsync     <= 1'b1;
        hblank    <= 1'b0;
        vblank    <= 1'b0;
        de        <= 1'b1;
        new_frame <= 1'b0;
        new_line  <= 1'b0;
        h_active  <= DEF_H_ACTIVE;
        h_fp      <= DEF_H_FP;
        h_sync_w  <= DEF_H_SYNC;
        h_total   <= DEF_H_TOTAL;
        v_active  <= DEF_V_ACTIVE;
        v_fp      <= DEF_V_FP;
        v_sync_w  <= DEF_V_SYNC;
        v_total   <= DEF_V_TOTAL;
    end
    else if (ce_pix) begin
        new_frame <= 1'b0;
        new_line  <= 1'b0;

        // Geometry update — frame boundary only, and only if plausible.
        if (frame_last_pixel && params_sane) begin
            h_active <= h_active_in;
            h_fp     <= h_fp_in;
            h_sync_w <= h_sync_in;
            h_total  <= h_total_in;
            v_active <= v_active_in;
            v_fp     <= v_fp_in;
            v_sync_w <= v_sync_in;
            v_total  <= v_total_in;
        end

        // Horizontal counter
        if (hcount >= h_total - 12'd1) begin
            hcount <= 12'd0;
            if (vcount >= v_total - 12'd1)
                vcount <= 12'd0;
            else
                vcount <= vcount + 12'd1;
        end
        else begin
            hcount <= hcount + 12'd1;
        end

        // Horizontal blanking
        if (hcount == h_active - 12'd1)
            hblank <= 1'b1;
        else if (hcount >= h_total - 12'd1)
            hblank <= 1'b0;

        // Horizontal sync (active low)
        if (hcount == h_sync_start - 12'd1)
            hsync <= 1'b0;
        else if (hcount == h_sync_end - 12'd1)
            hsync <= 1'b1;

        // Vertical blanking (transitions on line boundaries)
        if (hcount >= h_total - 12'd1) begin
            if (vcount == v_active - 12'd1)
                vblank <= 1'b1;
            else if (vcount >= v_total - 12'd1)
                vblank <= 1'b0;
        end

        // Vertical sync (active low)
        if (hcount >= h_total - 12'd1) begin
            if (vcount == v_sync_start - 12'd1)
                vsync <= 1'b0;
            else if (vcount == v_sync_end - 12'd1)
                vsync <= 1'b1;
        end

        // New line pulse
        if (hcount == h_active - 12'd1)
            new_line <= 1'b1;

        // New frame pulse
        if (hcount >= h_total - 12'd1 && vcount == v_active - 12'd1)
            new_frame <= 1'b1;

        // Data enable (from next-cycle blanking state)
        begin
            reg next_hblank, next_vblank;

            if (hcount == h_active - 12'd1)
                next_hblank = 1'b1;
            else if (hcount >= h_total - 12'd1)
                next_hblank = 1'b0;
            else
                next_hblank = hblank;

            if (hcount >= h_total - 12'd1) begin
                if (vcount == v_active - 12'd1)
                    next_vblank = 1'b1;
                else if (vcount >= v_total - 12'd1)
                    next_vblank = 1'b0;
                else
                    next_vblank = vblank;
            end
            else
                next_vblank = vblank;

            de <= ~next_hblank & ~next_vblank;
        end
    end
end

endmodule
