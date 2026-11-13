-- SPDX-License-Identifier: GPL-2.0-or-later
-- VHDL stand-ins for the Verilog blocks vectrex.vhd instantiates, so the drawing path can be simulated in GHDL alone.
-- Only for the PiTrex-mode RAMP pause test (tb_ramp_pause.vhd): there the 6809 is held in reset and the bus comes from ptx_*.
--   mc6809:   only the E/Q phase enables are modelled, exactly as rtl/mc6809.v makes them (2-bit counter on CLKEN);
--             ADDR/Dout/RnW are idle (the core selects ptx_* in PiTrex mode anyway).
--   jt49_bus, vectrex_high_scores, random_xor, vectrex_eeprom: outputs tied off (sound, scores, LFSR, cart EEPROM: not in the
--             drawing path).
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity mc6809 is port (
	CLK   : in  std_logic;
	CLKEN : in  std_logic;
	riseE : out std_logic;
	fallE : out std_logic;
	riseQ : out std_logic;
	Din   : in  std_logic_vector(7 downto 0);
	Dout  : out std_logic_vector(7 downto 0);
	ADDR  : out std_logic_vector(15 downto 0);
	RnW   : out std_logic;
	nIRQ  : in  std_logic;
	nFIRQ : in  std_logic;
	nRESET : in std_logic := '1'
);
end entity;
architecture stub of mc6809 is
	signal clk_phase : unsigned(1 downto 0) := "00";
begin
	Dout <= (others => '0'); ADDR <= (others => '1'); RnW <= '1';
	process (CLK) begin
		if rising_edge(CLK) then
			fallE <= '0'; riseQ <= '0'; riseE <= '0';
			if CLKEN = '1' then
				clk_phase <= clk_phase + 1;
				case clk_phase is
					when "00"   => fallE <= '1';
					when "01"   => riseQ <= '1';
					when "10"   => riseE <= '1';
					when others => null;
				end case;
			end if;
		end if;
	end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;
entity jt49_bus is
	generic (COMP : std_logic_vector(2 downto 0) := "000"; MUTE_NULL_PERIOD : integer := 1);
	port (
		rst_n, clk, clk_en, bdir, bc1 : in std_logic;
		din     : in  std_logic_vector(7 downto 0);
		sel     : in  std_logic;
		dout    : out std_logic_vector(7 downto 0);
		sound   : out std_logic_vector(9 downto 0);
		A, B, C : out std_logic_vector(7 downto 0);
		sample  : out std_logic;
		IOA_in  : in  std_logic_vector(7 downto 0);
		IOA_out : out std_logic_vector(7 downto 0);
		IOA_oe  : out std_logic;
		IOB_in  : in  std_logic_vector(7 downto 0);
		IOB_out : out std_logic_vector(7 downto 0);
		IOB_oe  : out std_logic
	);
end entity;
architecture stub of jt49_bus is
begin
	dout <= (others => '1'); sound <= (others => '0'); A <= (others => '0'); B <= (others => '0'); C <= (others => '0');
	sample <= '0'; IOA_out <= (others => '1'); IOA_oe <= '0'; IOB_out <= (others => '1'); IOB_oe <= '0';
end architecture;

library ieee;
use ieee.std_logic_1164.all;
entity vectrex_high_scores is port (
	clk, reset, download : in  std_logic;
	score_kind           : in  std_logic_vector(2 downto 0);
	cpu_read, ram_write  : in  std_logic;
	cpu_addr             : in  std_logic_vector(15 downto 0);
	cpu_data             : in  std_logic_vector(7 downto 0);
	ram_wr    : out std_logic;
	ram_addr  : out std_logic_vector(9 downto 0);
	ram_data  : out std_logic_vector(7 downto 0);
	ram_ready : in  std_logic;
	changed   : out std_logic;
	backup_req, backup_wr : in std_logic;
	backup_addr  : in  std_logic_vector(11 downto 0);
	backup_data  : in  std_logic_vector(7 downto 0);
	backup_ready : out std_logic;
	backup_q     : out std_logic_vector(7 downto 0)
);
end entity;
architecture stub of vectrex_high_scores is
begin
	ram_wr <= '0'; ram_addr <= (others => '0'); ram_data <= (others => '0'); changed <= '0';
	backup_ready <= '0'; backup_q <= (others => '0');
end architecture;

library ieee;
use ieee.std_logic_1164.all;
entity random_xor is port (
	clock : in  std_logic;
	reset : in  std_logic;
	seed  : in  std_logic_vector(31 downto 0);
	lfsr  : out std_logic_vector(31 downto 0)
);
end entity;
architecture stub of random_xor is
begin
	lfsr <= (others => '0');
end architecture;

library ieee;
use ieee.std_logic_1164.all;
entity vectrex_eeprom is
	generic (WRITE_CLOCKS : integer := 120000);
	port (
		clk, download, reset : in  std_logic;
		kind                 : in  std_logic_vector(2 downto 0);
		pb6, host_low, irq_n : in  std_logic;
		line_out, busy       : out std_logic;
		changed              : out std_logic;
		protect              : out std_logic_vector(1 downto 0);
		protect_restore      : in  std_logic;
		protect_data         : in  std_logic_vector(1 downto 0);
		mem_req, mem_wr      : out std_logic;
		mem_addr, mem_data   : out std_logic_vector(7 downto 0);
		mem_q                : in  std_logic_vector(7 downto 0);
		mem_ready            : in  std_logic
	);
end entity;
architecture stub of vectrex_eeprom is
begin
	line_out <= '1'; busy <= '0'; changed <= '0'; protect <= "00";
	mem_req <= '0'; mem_wr <= '0'; mem_addr <= (others => '0'); mem_data <= (others => '0');
end architecture;
