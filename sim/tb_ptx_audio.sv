// PCM pause test: samples must not advance while the machine is paused, and
// playback must resume without skipping. Not part of the Quartus project.
`timescale 1ns/1ps
module tb_ptx_audio;
logic clk = 0; always #5 clk = ~clk;
logic pause = 0, reset = 1;
wire fifo_rd_en; logic [63:0] fifo_rd_data = 0; logic fifo_rd_valid = 0;
wire signed [15:0] sample;

int k = 1;                                    // next sample value handed out
always @(posedge clk) begin
	fifo_rd_valid <= fifo_rd_en;
	if (fifo_rd_en) begin
		fifo_rd_data <= {16'(k+3), 16'(k+2), 16'(k+1), 16'(k)};
		k <= k + 4;
	end
end

pitrex_audio #(.DIVIDER(10)) dut (
	.clk(clk), .reset(reset),
`ifdef NEW
	.pause(pause),
`endif
	.fifo_rd_en(fifo_rd_en), .fifo_rd_data(fifo_rd_data), .fifo_rd_valid(fifo_rd_valid),
	.fifo_rd_empty(1'b0), .sample(sample));

int seq [$];
logic signed [15:0] last = 0;
always @(posedge clk) if (sample != last) begin seq.push_back(int'(sample)); last <= sample; end

int fails = 0;
task automatic check(input bit ok, input string msg);
	if (!ok) begin fails++; $display("  check FAILED: %s", msg); end else $display("  check ok:     %s", msg);
endtask

logic signed [15:0] s0; int n0;
initial begin
	repeat (5) @(posedge clk); reset = 0;
	repeat (400) @(posedge clk);
	pause = 1;                                 // user pause (pause_cpu)
	repeat (4) @(posedge clk);
	s0 = sample; n0 = seq.size();
	repeat (2000) @(posedge clk);
	check(sample == s0 && seq.size() == n0, $sformatf("sample frozen during pause (%0d -> %0d, %0d samples consumed)", s0, sample, seq.size() - n0));
	pause = 0;
	repeat (600) @(posedge clk);
	begin
		bit contiguous; contiguous = 1;
		for (int i = 1; i < seq.size(); i++) if (seq[i] != seq[i-1] + 1) contiguous = 0;
		check(contiguous, "stream contiguous across the pause (no skipped samples)");
	end
	$display("RESULT audio %s", fails == 0 ? "PASS" : "FAIL");
	$finish;
end
endmodule
