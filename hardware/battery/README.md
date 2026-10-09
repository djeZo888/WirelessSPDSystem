# Hardware 2 · Battery beacon

This transmitter monitors an SPD dry contact and a 10 kΩ NTC, then sends status, temperature, and cell voltage to the receiver over LoRa. An **AVR128DB32-E/PT** runs the firmware, a **LiFePO4 cell** supplies the board through an **LM66100** ideal-diode circuit, and the **Wio-SX1262** handles the radio link. The MCU sleeps between measurements; the button requests a report.

The [full schematic with MCU update](schematic-revised.pdf) combines the supplied drawing and update: only the MCU section is replaced; the remaining circuit is retained. The supported firmware uses this mapping: SPD_STATUS=PD4, BUTTON1=PD6, and power/TX/SPD LEDs=PF4/PF3/PF2.

![Revised MCU pin map for hardware 2, used by the supported battery firmware](mcu-pin-map-revised.png)

- [Firmware and configuration template](../../firmware/beacons/battery_avr128db32/)
- [Flashing instructions](../../docs/flashing.md)
- [Full schematic with MCU update, PDF](schematic-revised.pdf)
- [Original schematic v1.0, PDF](schematic-v1.0-original.pdf), preserved as a source reference
- [Pin mapping and programming connections](../README.md)

The original PDF and update image are unchanged. In the consolidated PDF, the MCU update remains raster while the other circuit sections remain vector. Regenerate it with `python3 tools/revise_battery_schematic.py` (requires PyMuPDF 1.28.2 and pypdf).
