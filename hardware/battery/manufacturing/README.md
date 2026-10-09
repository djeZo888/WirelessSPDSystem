# Hardware 2 fabrication package

[Download Gerbers ZIP](https://github.com/djeZo888/WirelessSPDSystem/raw/refs/heads/main/hardware/battery/manufacturing/hw2-battery-gerbers.zip)

Two-layer, 44.3124 × 77.54 mm battery beacon PCB; two 3.45 mm nonplated holes and no routed slots.
Checked against the recovered PCB render, full schematic and firmware pin assignments.

| Files | Purpose |
| --- | --- |
| GTL / GBL | Top / bottom copper |
| GTS / GBS | Top / bottom solder mask |
| GTO / GBO | Top / bottom silkscreen |
| GTP | Top solder paste |
| GKO | Board outline |
| Drill_PTH_Through.DRL | 101 plated holes, including all 85 vias |
| Drill_NPTH_Through.DRL | Two nonplated holes |

All ten files retain their supplied bytes. The redundant via-only drill file,
reference drawings (including the battery footprint custom layer) and fabricator
processing/customer/order data are excluded. Checksums and comparison details
are in [verification.json](verification.json).

The source ZIP filename contains `v3`; bottom silkscreen and the recovered full
schematic say `V1.0`. The layout corresponds to hardware 2.
This comparison establishes hardware identity, not full copper connectivity,
DRC compliance or electrical qualification. Assembly values and board stack-up
specifications are separate from these fabrication artwork files.
