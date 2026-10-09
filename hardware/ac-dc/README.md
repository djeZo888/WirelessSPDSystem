# Hardware 1 · AC-DC beacon

This transmitter monitors an SPD dry contact and a 10 kΩ NTC, then sends status and temperature to the receiver over LoRa. An **AVR128DA32-E/PT** runs the firmware, the **IRM-02-3.3** provides the nominal 3.3 V supply, and the **Wio-SX1262** handles the radio link.

<img src="pcb-render.png" alt="Hardware 1 AC-DC SPD beacon PCB render showing the AVR128DA32, Wio-SX1262 footprint, IRM-02-3.3 footprint, fuse, and UPDI header" width="420">

Supplied PCB render of this hardware version.

- [Firmware and configuration template](../../firmware/beacons/acdc_avr128da32/)
- [Flashing instructions](../../docs/flashing.md)
- [Original schematic, SVG](schematic.svg) · [schematic PNG preview](schematic-preview.png)
- [Pin mapping and programming connections](../README.md)

Bench-program with mains disconnected and an isolated compatible low-voltage supply. Installation and mains wiring require appropriate electrical qualification.
