#!/usr/bin/env python3
"""Rebuild the editable protocol SVGs using Python's standard library."""
from html import escape
from pathlib import Path

OUTPUT = Path(__file__).resolve().parents[1] / "docs" / "images"
INK = "#16324a"
MUTED = "#51687b"
BLUE = "#166a94"
GREEN = "#247562"


class SVG:
    def __init__(self, width, height, title, description):
        self.parts = [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
            f'<title id="title">{escape(title)}</title>',
            f'<desc id="desc">{escape(description)}</desc>',
            '<defs><marker id="arrow" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0 0 L10 5 L0 10 Z" fill="#51687b"/></marker></defs>',
            '<style>text {font-family:Arial,Helvetica,sans-serif} .mono {font-family:Consolas,Menlo,monospace}</style>',
            f'<rect width="{width}" height="{height}" fill="#fff"/>',
        ]

    def rect(self, x, y, w, h, fill="#f2f6f9", stroke="#d7e2eb", radius=14):
        self.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" fill="{fill}" stroke="{stroke}"/>')

    def text(self, x, y, value, size=19, color=INK, bold=False, anchor="start", mono=False):
        weight = ' font-weight="700"' if bold else ""
        klass = ' class="mono"' if mono else ""
        self.parts.append(f'<text x="{x}" y="{y}" font-size="{size}" fill="{color}" text-anchor="{anchor}"{weight}{klass}>{escape(value)}</text>')

    def lines(self, x, y, values, size=19, gap=29, **kwargs):
        for i, value in enumerate(values):
            self.text(x, y + i * gap, value, size, **kwargs)

    def arrow(self, x1, y1, x2, y2):
        self.parts.append(f'<path d="M{x1} {y1} L{x2} {y2}" stroke="{MUTED}" stroke-width="2.5" fill="none" marker-end="url(#arrow)"/>')

    def path(self, points, color=MUTED):
        self.parts.append(f'<path d="{points}" stroke="{color}" stroke-width="2" fill="none"/>')

    def save(self, name):
        OUTPUT.mkdir(parents=True, exist_ok=True)
        (OUTPUT / name).write_text("\n".join(self.parts + ["</svg>", ""]), encoding="utf-8")


def communication():
    s = SVG(1280, 640, "Wireless SPD communication", "An SPD dry contact and temperature sensor feed either beacon. A 20-byte one-way LoRa message is authenticated by the T-Connect Pro receiver and held in RAM for its display and local web interface.")
    s.text(48, 58, "Wireless SPD System", 34, bold=True)
    s.text(48, 94, "One-way telemetry from configured beacons to a local receiver", 21, MUTED)

    s.rect(48, 170, 202, 226)
    s.text(70, 207, "SPD inputs", 23, bold=True)
    s.lines(70, 248, ["Dry contact", "OK / FAIL", "", "10 kΩ NTC", "Temperature"], 19, gap=27)

    s.rect(291, 170, 270, 226, "#edf6fb", "#bedce9")
    s.text(314, 207, "TX beacon", 23, BLUE, bold=True)
    s.lines(314, 246, ["HW 1 · AC-DC", "AVR128DA32", "HW 2 · LiFePO4", "AVR128DB32", "Measure → pack → tag"], 19, gap=28)
    s.arrow(254, 283, 284, 283)

    s.rect(697, 151, 286, 264, "#edf6f2", "#c2ded5")
    s.text(720, 191, "T-Connect Pro RX", 23, GREEN, bold=True)
    s.lines(720, 233, ["1  Length + configured ID", "2  Verify keyed hash tag", "3  Require newer nonce"], 18, gap=34)
    s.path("M720 325 L959 325", "#c2ded5")
    s.lines(720, 356, ["Update live state in RAM", "Status · age · link quality"], 19, gap=28)
    s.arrow(568, 283, 689, 283)
    s.lines(629, 244, ["LoRa", "20 bytes"], 18, gap=24, anchor="middle", color=BLUE, bold=True)
    s.text(629, 323, "No ACK", 16, MUTED, anchor="middle")

    s.rect(1034, 169, 199, 99)
    s.text(1054, 205, "Built-in display", 21, bold=True)
    s.text(1054, 237, "Touch + relay alarm", 17, MUTED)
    s.rect(1034, 300, 199, 99)
    s.text(1054, 336, "Local web UI", 21, bold=True)
    s.text(1054, 368, "Browser + JSON API", 17, MUTED)
    s.path("M991 283 L1010 283 L1010 219 L1026 219")
    s.arrow(1020, 219, 1027, 219)
    s.path("M1010 283 L1010 349 L1026 349")
    s.arrow(1020, 349, 1027, 349)

    for x, title, content in [
        (48, "Routine report", ["Randomized intervals reduce", "simultaneous transmissions."]),
        (453, "Detected fault", ["OK → FAIL sends an event", "with repeated identical frames."]),
        (858, "Missing report", ["Receiver marks a seen beacon", "stale after its timeout."]),
    ]:
        s.rect(x, 471, 375, 118, "#f8fafc")
        s.text(x + 21, 507, title, 21, bold=True)
        s.lines(x + 21, 540, content, 18, gap=26, color=MUTED)
    s.save("communication.svg")


def packet():
    s = SVG(1280, 630, "20-byte beacon frame", "Byte 0 packs hardware and firmware nibbles. Byte 1 packs a 7-bit beacon ID and the OK bit. Byte 2 is signed Celsius. Bytes 3 to 10 are a little-endian 64-bit nonce. Byte 11 is encoded cell voltage. Bytes 12 to 19 are the first 8 bytes of SHA-256 over bytes 0 to 11 followed by the 4-byte little-endian secret.")
    s.text(56, 57, "20-byte beacon frame", 34, bold=True)
    s.text(56, 95, "Application bytes only · little-endian integers · telemetry is unencrypted", 20, MUTED)

    x, unit, top, height = 56, 58.4, 161, 78
    for i in range(20):
        s.text(x + unit * (i + 0.5), 145, str(i), 17, MUTED, anchor="middle", mono=True)
    fields = [
        (0, 1, "HW/FW", "#d9edf7", BLUE, 12),
        (1, 1, "ID/OK", "#e5f2ec", GREEN, 13),
        (2, 1, "°C", "#f3e9d5", "#8a6323", 20),
        (3, 8, "64-bit nonce · LE", "#e5edf5", INK, 22),
        (11, 1, "Vbat", "#f3e9d5", "#8a6323", 14),
        (12, 8, "Authentication tag · 8 bytes", "#dcefe8", GREEN, 22),
    ]
    for start, count, label, fill, color, size in fields:
        s.rect(x + unit * start, top, unit * count, height, fill, "#fff", radius=0)
        s.text(x + unit * (start + count / 2), 206, label, size, color, bold=True, anchor="middle")
    s.path("M56 250 L56 260 L756.8 260 L756.8 250", BLUE)
    s.text(406.4, 286, "Authenticated data · bytes 0-11", 19, BLUE, anchor="middle", bold=True)
    s.path("M756.8 250 L756.8 260 L1224 260 L1224 250", GREEN)
    s.text(990.4, 286, "Tag · bytes 12-19", 19, GREEN, anchor="middle", bold=True)

    s.rect(56, 315, 700, 242)
    s.text(80, 353, "Compact field encoding", 23, bold=True)
    s.lines(80, 391, [
        "Byte 0    bits 7-4: hardware · bits 3-0: firmware",
        "Byte 1    bits 7-1: ID 1-127 · bit 0: 1=OK, 0=FAIL",
        "Byte 2    signed int8 °C (faults: -128 / -127 / -126)",
        "Byte 11   voltage = 2.20 V + code × 0.01 V",
        "                 0-150 valid · 255 unavailable / error",
    ], 18, gap=31)

    s.rect(790, 315, 434, 242, "#edf6f2", "#c2ded5")
    s.text(814, 353, "Keyed hash calculation", 23, GREEN, bold=True)
    s.lines(814, 397, ["SHA-256(", "  data[0..11] || SECRET_LE32", ")[0..7]"], 18, gap=31, mono=True)
    s.text(814, 524, "The secret is never transmitted.", 19, GREEN)
    s.text(56, 595, "Compatible format: SHA-256 suffix construction, 32-bit key, 64-bit tag. See protocol.md for limits.", 18, MUTED)
    s.save("packet-layout.svg")


if __name__ == "__main__":
    communication()
    packet()
    print("Rebuilt docs/images/communication.svg and packet-layout.svg")
