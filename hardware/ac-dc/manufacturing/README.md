# Hardware 1 fabrication package

[Download Gerbers ZIP](https://github.com/djeZo888/WirelessSPDSystem/raw/refs/heads/main/hardware/ac-dc/manufacturing/hw1-acdc-gerbers.zip)

Two-layer, 34 × 75 mm AC-DC beacon PCB; four routing slots and four 3.2 mm mounting holes.
Checked against the published PCB render, schematic and firmware pin assignments.

| Files | Purpose |
| --- | --- |
| GTL / GBL | Top / bottom copper |
| GTS / GBS | Top / bottom solder mask |
| GTO / GBO | Top / bottom silkscreen |
| GTP | Top solder paste |
| GKO | Board outline and routed slots |
| Drill_PTH_Through.DRL | 123 plated holes, including all 99 vias |
| Drill_NPTH_Through.DRL | Four nonplated mounting holes |

All ten files retain their supplied bytes. The redundant via-only drill file,
reference drawings and fabricator processing/customer/order data are excluded.
Checksums and comparison details are in [verification.json](verification.json).

The source ZIP filename contains `lite-v1-1`; bottom silkscreen reads `Lite v1.0`.
The layout matches hardware 1. The PCB's 3V3EN jumper matches the render; the
schematic simplifies the converter output connection as continuous VCC.
This comparison establishes hardware identity, not full copper connectivity,
DRC compliance or electrical qualification. Assembly values and board stack-up
specifications are separate from these fabrication artwork files.
