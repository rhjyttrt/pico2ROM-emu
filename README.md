# pico2ROM-emu

High-speed ROM / EEPROM emulator powered by the Raspberry Pi Pico 2 (RP2350) and RP2040, built for retro CPU bus integration (6502, 6800, Z80, etc.).

## Repository Layout
- picoROM-emu.ino         : Core Arduino firmware.
- pico_flasher.py         : Python tool to stream Intel HEX files directly to Pico SRAM over USB.
- release/picoROM-emu.uf2 : Ready-to-flash binary for BOOTSEL mode.

## Usage

1. Flash the Board
Hold BOOTSEL, connect USB, and drop release/picoROM-emu.uf2 onto the RPI-RP2 drive.

2. Stream a ROM Payload
Ensure pyserial is installed, then pass your .hex payload to the flasher:
   pip install pyserial
   python3 pico_flasher.py /path/to/rom_payload.hex

## you MUST have level shifters like the 74lvc245 and 74hct245 because if 5v are apllied to the picos pins it WILL fry the internal output drivers
