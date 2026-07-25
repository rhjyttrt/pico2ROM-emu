# pico2ROM-emu

High speed ROM or EEPROM emulator powered by the Raspberry Pi Pico 2 or RP2040, built for retro CPU bus integration such as the 6502, 6800, and Z80 families.

## Repository Layout

* `picoROM-emu.ino` : Core Arduino firmware.
* `pico_flasher.py` : Python tool to stream Intel HEX files directly to Pico SRAM over USB.
* `release/picoROM-emu.uf2` : Ready to flash binary for BOOTSEL mode.

---

## Wiring Guide

Connecting your Raspberry Pi Pico 2 to a retro computer bus requires careful attention to pin assignments.

### Pin Mapping Table

| Pico Pin | GPIO Number | Retro Bus Signal | Description |
| :--- | :--- | :--- | :--- |
| Pin 1 to 20 | GPIO 0 to 14 | A0 to A14 | Address Bus lines |
| Pin 21 | GPIO 16 | D0 | Data Bus line bit zero |
| Pin 22 | GPIO 17 | D1 | Data Bus line bit one |
| Pin 24 | GPIO 18 | D2 | Data Bus line bit two |
| Pin 25 | GPIO 19 | D3 | Data Bus line bit three |
| Pin 26 | GPIO 20 | D4 | Data Bus line bit four |
| Pin 27 | GPIO 21 | D5 | Data Bus line bit five |
| Pin 28 | GPIO 22 | D6 | Data Bus line bit six |
| Pin 31 | GPIO 26 | D7 | Data Bus line bit seven |
| Pin 20 | GPIO 15 | PHI2 | Phase 2 clock input |
| Pin 32 | GPIO 27 | RW | Read or Write control signal |
| Pin 34 | GPIO 28 | ROMCS | Chip Select active low input |

> [!IMPORTANT]
> You **must** use bidirectional level shifters such as the 74LVC245 or 74HCT245 chips between the retro computer bus and the Pico board. Applying 5V logic directly to the Pico pins will permanently damage the internal output drivers.

---

## Usage Instructions

1. **Flash the Board**
   Hold BOOTSEL, connect USB, and drop `release/picoROM-emu.uf2` onto the RPI-RP2 drive.

2. **Stream a ROM Payload**
   Ensure pyserial is installed, then pass your hex payload to the flasher script:
   ```bash
   pip install pyserial
   python3 pico_flasher.py /path/to/rom_payload.hex
