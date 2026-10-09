#!/usr/bin/env python3
"""Replace the supplied battery schematic's MCU section with the supplied update.

Requires PyMuPDF==1.28.2 and pypdf. Original PDF/PNG files remain unchanged.
The remainder stays vector; the replacement MCU section retains its supplied
raster pixels. Redaction removes the superseded MCU content before insertion.
"""
from io import BytesIO
from pathlib import Path
import json

import pymupdf
from pypdf import PdfReader, PdfWriter
from pypdf.generic import ContentStream, FloatObject

ROOT = Path(__file__).resolve().parents[1]
HARDWARE = ROOT / "hardware" / "battery"
SOURCE = HARDWARE / "schematic-v1.0-original.pdf"
UPDATE = HARDWARE / "mcu-pin-map-revised.png"
OUTPUT = HARDWARE / "schematic-revised.pdf"

# Coordinates are PDF points in PyMuPDF's top-left coordinate system.
REMOVAL = pymupdf.Rect(292, 143, 610, 303)
INSERTION = pymupdf.Rect(294, 144, 608, 144 + 314 * 876 / 1748)


def nearby_net_label_operations(reader):
    """Preserve the exact unrelated RFM_VCC label and arrow within the edit area."""
    ops = reader.pages[0].get_contents().operations
    for i, (args, command) in enumerate(ops):
        if command == b"Tj" and str(args[0]) == "RFM_VCC":
            start = i
            while ops[start][1] != b"BT":
                start -= 1
            end = i
            while ops[end][1] != b"ET":
                end += 1
            label = ops[start : end + 1]
            stroke_end = start - 1
            while ops[stroke_end][1] != b"S":
                stroke_end -= 1
            path_start = stroke_end
            while ops[path_start][1] != b"m":
                path_start -= 1
            return [
                ([], b"q"),
                ([FloatObject(0.72)], b"w"),
                ([FloatObject(1)], b"J"),
                ([FloatObject(1)], b"j"),
                ([FloatObject(0.63), FloatObject(0), FloatObject(0)], b"RG"),
                *ops[path_start : stroke_end + 1],
                *label,
                ([], b"Q"),
            ]
    raise ValueError("Expected nearby RFM_VCC label not found in source PDF")


def main():
    original_reader = PdfReader(SOURCE)
    nearby = nearby_net_label_operations(original_reader)
    doc = pymupdf.open(SOURCE)
    page = doc[0]
    page.add_redact_annot(REMOVAL, fill=(1, 1, 1))
    # Remove fully-contained paths. The large page frame remains untouched.
    page.apply_redactions(images=0, graphics=1, text=0)
    page.insert_image(INSERTION, filename=str(UPDATE), keep_proportion=True)
    # Cover a tiny blue canvas-corner artifact at the screenshot's top left.
    # This lies clear of every circuit label, line, and symbol.
    page.draw_rect(pymupdf.Rect(294, 144, 297, 147), color=None, fill=(1, 1, 1))
    intermediary = PdfReader(BytesIO(doc.tobytes(garbage=4, deflate=True)))
    doc.close()

    writer = PdfWriter()
    writer.clone_document_from_reader(intermediary)
    page_out = writer.pages[0]
    assert "/F2" in page_out["/Resources"]["/Font"], "Original Arial resource missing"
    stream = ContentStream(page_out.get_contents(), writer)
    stream.operations.extend(nearby)
    page_out.replace_contents(stream)
    writer.add_metadata({
        "/Title": "Battery beacon schematic with supplied MCU update",
        "/Subject": "Original vector drawing retained outside MCU section; supplied MCU screenshot inserted",
        "/Author": "",
        "/Creator": "WirelessSPDSystem/tools/revise_battery_schematic.py",
        "/Producer": "PyMuPDF 1.28.2 and pypdf",
    })
    with OUTPUT.open("wb") as output:
        writer.write(output)

    # The superseded MCU text is absent from the revised PDF, rather than hidden.
    check = pymupdf.open(OUTPUT)
    edited_text = check[0].get_text(clip=REMOVAL).strip()
    assert edited_text == "RFM_VCC", f"Unexpected text remains under MCU patch: {edited_text!r}"
    assert len(check) == 1
    print(json.dumps({
        "output": str(OUTPUT),
        "pages": len(check),
        "page_size_points": list(check[0].rect),
        "removed_region_points": list(REMOVAL),
        "updated_mcu_image_points": list(INSERTION),
        "old_mcu_text_removed": True,
        "nearby_net_label_restored": True,
    }, indent=2))
    check.close()


if __name__ == "__main__":
    main()
