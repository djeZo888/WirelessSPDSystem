# Beacon hardware

| Version | Supply / MCU | Drawing |
| --- | --- | --- |
| [1 · AC-DC](ac-dc/README.md) | IRM-02-3.3 / AVR128DA32-E/PT | [Original schematic, SVG](ac-dc/schematic.svg), [PNG preview](ac-dc/schematic-preview.png) |
| [2 · Battery](battery/README.md) | LiFePO4 cell / LM66100 / AVR128DB32-E/PT | [Full schematic, SVG](battery/schematic.svg), [PNG preview](battery/schematic-preview.png) |

Both use a Wio-SX1262 module, an SPD dry-contact input, and a 10 kΩ NTC (nominal beta 3435 K). Each hardware page displays its full schematic. PNG previews are browser-rendered from the unchanged SVG exports to avoid CSS-variable rendering issues in some SVG viewers.

The recovered battery schematic replaces the earlier partial drawings and combined PDF. Its MCU mapping matches the supported firmware: SPD_STATUS=PD4, BUTTON1=PD6, LEDs=PF4/PF3/PF2.

| Signal | AC-DC · AVR128DA32 | Battery · AVR128DB32 |
| --- | --- | --- |
| MOSI / MISO / SCK / NSS | PA4 / PA5 / PA6 / PA7 | PA4 / PA5 / PA6 / PA7 |
| RF switch / radio reset / BUSY / DIO1 | PC0 / PC1 / PC2 / PC3 | PC0 / PC1 / PC2 / PC3 |
| SPD status | PD7 | PD4 |
| Thermistor | PD0 | PD1 |
| NTC divider power / SPD divider power | Always supplied | PD2 / PD3 |
| Manual report button | None | PD6, active low |
| Power / TX / SPD LED | PA1 / PF5 / PF4 | PF4 / PF3 / PF2, active low |
| Programming | UPDI (MCU pin 27), GND, target VCC | UPDI (MCU pin 27), GND, target VCC |

The supplied drawings show the same four-pin programming header numbering:

| Hardware / drawing | Pin 1 | Pin 2 | Pin 3 | Pin 4 |
| --- | --- | --- | --- | --- |
| AC-DC · original SVG | RESET | VCC | GND | UPDI |
| Battery · recovered full SVG | RESET | VCC | GND | UPDI |

Check actual connector orientation and continuity before connection. UPDI is distinct from RESET (PF6, MCU pin 26). Supply pins are VDD 28, AVDD 18, GND 19/29; battery DB hardware also ties VDDIO2 pin 10 to VCC.

Use a programmer with target-compatible voltage, and only one supply source. AC-DC hardware is nominally 3.3 V; battery VCC follows the cell through the ideal-diode circuit. Bench-program AC-DC boards with mains disconnected and an isolated compatible low-voltage supply. Installation, mains isolation, and SPD wiring require appropriate electrical qualification. The firmware and these supplied drawings do not establish electrical safety or authorize energization.

The supplied drawings and PCB renders are reference exports. [Hardware 1 Gerbers](ac-dc/manufacturing/README.md) and [hardware 2 Gerbers](battery/manufacturing/README.md) are available. No editable EDA project, PCB layout source, BOM, or assembly files were provided for inclusion. See the [AC-DC input-fuse requirement](ac-dc/README.md#input-fuse) when selecting hardware 1's fuse.
