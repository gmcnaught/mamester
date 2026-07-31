// Behavioural stand-in for the Altera dcfifo megafunction, for lint and
// simulation only. Show-ahead dual-clock FIFO; the model is deliberately
// simple (a shared array with independent pointers) — good enough for the
// reader's line/audio FIFO usage, not a CDC-accurate model.
module dcfifo #(
    parameter intended_device_family = "Cyclone V",
    parameter lpm_numwords           = 256,
    parameter lpm_showahead          = "ON",
    parameter lpm_type               = "dcfifo",
    parameter lpm_width              = 64,
    parameter lpm_widthu             = 8,
    parameter overflow_checking      = "ON",
    parameter rdsync_delaypipe       = 4,
    parameter underflow_checking     = "ON",
    parameter use_eab                = "ON",
    parameter wrsync_delaypipe       = 4
) (
    input  wire                    aclr,
    input  wire [lpm_width-1:0]    data,
    input  wire                    rdclk,
    input  wire                    rdreq,
    input  wire                    wrclk,
    input  wire                    wrreq,
    output wire [lpm_width-1:0]    q,
    output wire                    rdempty,
    output wire                    wrfull,
    output wire [lpm_widthu-1:0]   wrusedw,
    output wire [lpm_widthu-1:0]   rdusedw,
    output wire                    rdfull,
    output wire                    wrempty,
    output wire [1:0]              eccstatus
);

reg [lpm_width-1:0]  mem [0:lpm_numwords-1];
reg [lpm_widthu:0]   wr_ptr = 0;
reg [lpm_widthu:0]   rd_ptr = 0;

wire [lpm_widthu:0] used = wr_ptr - rd_ptr;

always @(posedge wrclk or posedge aclr) begin
    if (aclr) wr_ptr <= 0;
    else if (wrreq && !wrfull) begin
        mem[wr_ptr[lpm_widthu-1:0]] <= data;
        wr_ptr <= wr_ptr + 1'b1;
    end
end

always @(posedge rdclk or posedge aclr) begin
    if (aclr) rd_ptr <= 0;
    else if (rdreq && !rdempty) rd_ptr <= rd_ptr + 1'b1;
end

assign q         = mem[rd_ptr[lpm_widthu-1:0]];
assign rdempty   = (used == 0);
assign wrfull    = (used >= lpm_numwords);
assign wrusedw   = used[lpm_widthu-1:0];
assign rdusedw   = used[lpm_widthu-1:0];
assign rdfull    = wrfull;
assign wrempty   = rdempty;
assign eccstatus = 2'b00;

endmodule
