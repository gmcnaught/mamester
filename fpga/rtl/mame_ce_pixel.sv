//============================================================================
//
//  MAMESTer fractional CE_PIXEL generator
//
//  A phase accumulator produces a pixel enable whose average rate is
//
//      f_pix = f_clk * inc / 2^ACC_W
//
//  from the fixed CLK_VIDEO (53.693182 MHz). This replaces the hardcoded
//  Genesis H40 /8,/9,/10 schedule: with per-game geometry the pixel clock has
//  to be arbitrary (sf2 at 384x224 and gng at 256x224 do not share a divider),
//  and only a fractional divider can hold the H rate near 15.7 kHz across
//  widths without reconfiguring the PLL.
//
//  Cost: the pixel edge jitters by at most one CLK_VIDEO period (18.6 ns),
//  which the scaler does not see (it samples on ce) and which is far below a
//  pixel on the analog path.
//
//  Copyright (C) 2026 -- GPL-3.0
//
//============================================================================

module mame_ce_pixel #(
    parameter ACC_W = 24
) (
    input  wire              clk,
    input  wire              reset,
    input  wire [ACC_W-1:0]  inc,      // 0 selects DEF_INC (6.594 MHz)
    output reg               ce_pix
);

// 6.594000 MHz / 53.693182 MHz * 2^24 — the default 320x240 @ 15.700 kHz mode.
localparam [ACC_W-1:0] DEF_INC = 24'd2060470;

wire [ACC_W-1:0] inc_use = (inc == {ACC_W{1'b0}}) ? DEF_INC : inc;

reg  [ACC_W-1:0] acc;
wire [ACC_W:0]   sum = {1'b0, acc} + {1'b0, inc_use};

always @(posedge clk) begin
    if (reset) begin
        acc    <= {ACC_W{1'b0}};
        ce_pix <= 1'b0;
    end
    else begin
        acc    <= sum[ACC_W-1:0];
        ce_pix <= sum[ACC_W];
    end
end

endmodule
