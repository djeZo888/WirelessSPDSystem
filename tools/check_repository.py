#!/usr/bin/env python3
"""Check publishable files, local documentation links, and configuration isolation."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
files = subprocess.check_output(
    ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
    cwd=ROOT).decode().split("\0")
errors = []
for name in filter(None, files):
    path = ROOT / name
    if path.name == "config.h" or ".private." in name or path.suffix in (".hex", ".bin", ".elf"):
        errors.append(f"Private build/configuration selected for publication: {name}")
    if path.suffix not in (".ino", ".h", ".md", ".json", ".py", ".yml"):
        continue
    content = path.read_text()
    if re.search(r"github_pat_[A-Za-z0-9_]{20,}|gh[pousr]_[A-Za-z0-9]{20,}", content):
        errors.append(f"Possible GitHub credential: {name}")
    if path.suffix == ".md":
        for target in re.findall(r"\]\(([^)]+)\)", content):
            if ":" in target or target.startswith("#"):
                continue
            target = target.split("#", 1)[0]
            if not (path.parent / target).exists():
                errors.append(f"Broken documentation link in {name}: {target}")

sketches = list((ROOT / "firmware").glob("**/*.ino"))
if len(sketches) != 3:
    errors.append("Expected exactly three Arduino projects.")
for sketch in sketches:
    if sketch.stem != sketch.parent.name:
        errors.append(f"Arduino sketch folder/name mismatch: {sketch}")
    if '#include "config.h"' not in sketch.read_text():
        errors.append(f"Sketch lacks local configuration include: {sketch}")
    for name in ("config.example.h", "build-options.json"):
        if not (sketch.parent / name).exists():
            errors.append(f"Sketch missing {name}: {sketch}")
    ignored = subprocess.run(["git", "check-ignore", "--quiet", str(sketch.parent / "config.h")], cwd=ROOT)
    if ignored.returncode:
        errors.append(f"Local config.h is not ignored: {sketch}")

if errors:
    raise SystemExit("\n".join(errors))
print(f"Repository hygiene passed: {len(sketches)} sketches; no selected configs/tokens/images; documentation links valid.")
