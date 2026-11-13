-- SPDX-License-Identifier: GPL-2.0-or-later
-- RAMP pause test (13 Nov 2026), after Videodr0me's finding (13 Nov 2026):
-- "The same VIA stream must produce identical drawing with and without injected transport stalls."
--
-- The real vectrex.vhd (VIA, delay lines, analogue frontend) in PiTrex mode, fed one fixed VIA stream the way
-- rtl/pitrex/pitrex_player.sv feeds it: one access presented at a falling-E enable, then the idle $FFFF read.
-- The stream draws one vertical vector: X S/H = 0, REF = 0, PA = Y_RATE, RAMP on for RAMP_E E-cycles, unblanked
-- (CB2 high) from the E-cycle after RAMP on to the E-cycle after RAMP off.
--
-- A stall is `pause` held high for STALL_LEN clocks (24 MHz), as pitrex_player's `stall` (command FIFO empty) does.
-- It starts STALL_AT clocks after the E-cycle that presents the RAMP-on write (STALL_ANCHOR = 0) or the RAMP-off
-- write (STALL_ANCHOR = 1). STALL_AT < 0: no stall (the reference run).
--
-- Reported in emulated time only (counted on beam_ce, the 12 MHz enable that stops during a pause):
--   y, x        the beam's end point after the vector
--   ramp_ticks  12 MHz ticks with the integrators' RAMP active
--   lit_ticks   12 MHz ticks with the beam on (CB2 path after its delay)
-- Expected: every run reports the same four numbers as the reference run.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb_ramp_pause is
	generic (
		STALL_AT     : integer := -1;
		STALL_ANCHOR : integer := 0;
		STALL_LEN    : integer := 480;      -- 20 us: longer than the RAMP delay (204 clocks) + release (24)
		Y_RATE       : integer := 64;
		RAMP_E       : integer := 32
	);
end entity;

architecture sim of tb_ramp_pause is
	signal clock    : std_logic := '0';
	signal reset    : std_logic := '1';
	signal pause    : std_logic := '0';
	signal ptx_addr : std_logic_vector(15 downto 0) := x"FFFF";
	signal ptx_do   : std_logic_vector(7 downto 0)  := x"00";
	signal ptx_rw   : std_logic := '1';
	signal e_fall   : std_logic;
	signal beam_x, beam_y : signed(19 downto 0);
	signal beam_on, beam_ce, beam_zero_n : std_logic;
	signal done     : boolean := false;

	type cmd_t is record
		wait_e : natural;                         -- idle E-cycles before the access
		addr   : std_logic_vector(15 downto 0);
		data   : std_logic_vector(7 downto 0);
	end record;
	type cmds_t is array (natural range <>) of cmd_t;
	constant Y8 : std_logic_vector(7 downto 0) := std_logic_vector(to_signed(Y_RATE, 8));
	constant CMDS : cmds_t := (
		(2,  x"D003", x"FF"),    -- DDRA: all out
		(1,  x"D002", x"9F"),    -- DDRB as the BIOS sets it
		(1,  x"D00B", x"00"),    -- ACR: PB7 from ORB (RAMP under program control), no shift register
		(1,  x"D00C", x"CC"),    -- PCR: CA2 low (ZERO on), CB2 low (blank)
		(1,  x"D001", x"00"),    -- ORA (DAC) = 0
		(1,  x"D000", x"80"),    -- ORB: RAMP off, S/H on, mux 0: X S/H = 0
		(20, x"D000", x"82"),    -- mux 1: REF = 0
		(20, x"D000", x"81"),    -- S/H off
		(20, x"D00C", x"CE"),    -- ZERO off
		(1,  x"D001", Y8),       -- DAC = Y rate
		(30, x"D000", x"01"),    -- RAMP on                      <- index 10
		(0,  x"D00C", x"EE"),    -- unblank
		(RAMP_E, x"D000", x"81"),-- RAMP off                     <- index 12
		(0,  x"D00C", x"CE"),    -- blank
		(60, x"D001", x"00")     -- settle, then report
	);
	constant RAMP_ON_IDX  : natural := 10;
	constant RAMP_OFF_IDX : natural := 12;

	signal clk_n      : integer := 0;
	signal anchor_clk : integer := -1;
	signal ramp_ticks, lit_ticks : integer := 0;
	signal counting : boolean := false;     -- from the RAMP-on write on (before it, PB7/CB2 sit at their reset levels)
begin
	clock <= not clock after 20833 ps when not done;   -- 24 MHz

	dut : entity work.vectrex port map (
		clock => clock, reset => reset, pause => pause, bios_factory => '0',
		cart_download => '0', cart_data => x"00", cart_addr => (others => '0'), cart_wr => '0',
		backup_ready => open, backup_q => open, flash_enabled => open, cart_busy => open, cart_changed => open, cart_sector => open,
		speech_mode => '0', speech_level => "00", audio_out => open,
		up_1 => '0', dn_1 => '0', lf_1 => '0', rt_1 => '0', pot_x_1 => x"00", pot_y_1 => x"00",
		up_2 => '0', dn_2 => '0', lf_2 => '0', rt_2 => '0', pot_x_2 => x"00", pot_y_2 => x"00",
		beam_x => beam_x, beam_y => beam_y, beam_on => beam_on, beam_z => open, beam_ce => beam_ce, beam_zero_n => beam_zero_n,
		t2_reload => open, t2_period => open,
		ptx_enable => '1', ptx_addr => ptx_addr, ptx_do => ptx_do, ptx_rw => ptx_rw,
		e_fall_o => e_fall, psg_ioa_o => open
	);

	-- the stream, presented like pitrex_player: an access for one E-cycle from a falling-E enable, else idle
	player : process
		variable i : natural := 0;
		variable w : natural;
	begin
		for k in 1 to 64 loop wait until rising_edge(clock); end loop;
		reset <= '0';
		for k in 1 to 400 loop wait until rising_edge(clock); end loop;   -- past the core's own reset handling
		while i <= CMDS'high loop
			w := CMDS(i).wait_e;
			loop
				wait until rising_edge(clock) and e_fall = '1';
				ptx_addr <= x"FFFF"; ptx_rw <= '1';
				if w = 0 then
					ptx_addr <= CMDS(i).addr; ptx_do <= CMDS(i).data; ptx_rw <= '0';
					if i = RAMP_ON_IDX then counting <= true; end if;
					if (i = RAMP_ON_IDX and STALL_ANCHOR = 0) or (i = RAMP_OFF_IDX and STALL_ANCHOR = 1) then
						anchor_clk <= clk_n;
					end if;
					exit;
				end if;
				w := w - 1;
			end loop;
			i := i + 1;
		end loop;
		for k in 1 to 10 loop wait until rising_edge(clock) and e_fall = '1'; ptx_addr <= x"FFFF"; ptx_rw <= '1'; end loop;
		report "RESULT stall_anchor=" & integer'image(STALL_ANCHOR) & " stall_at=" & integer'image(STALL_AT)
			& " y=" & integer'image(to_integer(beam_y)) & " x=" & integer'image(to_integer(beam_x))
			& " ramp_ticks=" & integer'image(ramp_ticks) & " lit_ticks=" & integer'image(lit_ticks);
		done <= true;
		wait;
	end process;

	-- clock counter, the injected stall, and the emulated-time counters
	counters : process (clock)
		alias ramp_n is << signal .tb_ramp_pause.dut.ramp_integrator_n : std_logic >>;   -- after dut: elaborated by now
	begin
		if rising_edge(clock) then
			clk_n <= clk_n + 1;
			if STALL_AT >= 0 and anchor_clk >= 0 and clk_n >= anchor_clk + STALL_AT and clk_n < anchor_clk + STALL_AT + STALL_LEN then
				pause <= '1';
			else
				pause <= '0';
			end if;
			if beam_ce = '1' and counting then
				if ramp_n = '0' then ramp_ticks <= ramp_ticks + 1; end if;
				if beam_on = '1' then lit_ticks <= lit_ticks + 1; end if;
			end if;
		end if;
	end process;
end architecture;
