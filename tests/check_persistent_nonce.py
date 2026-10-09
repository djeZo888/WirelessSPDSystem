#!/usr/bin/env python3
"""Run failure injection against the actual, identical beacon nonce headers."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BEACONS = ROOT / "firmware/beacons"


def main():
    headers = [BEACONS / name / "persistent_nonce.h" for name in
               ("acdc_avr128da32", "battery_avr128db32")]
    contents = [path.read_bytes() for path in headers]
    if contents[0] != contents[1]:
        raise SystemExit("Persistent nonce headers differ between hardware versions.")
    compiler = shutil.which("c++")
    if not compiler:
        raise SystemExit("A host C++ compiler is required.")
    with tempfile.TemporaryDirectory(prefix="wspd-nonce-") as temp:
        binary = Path(temp) / "test_persistent_nonce"
        subprocess.run([compiler, "-std=c++11", "-O2", "-Wall", "-Wextra", "-pedantic",
                        "-I", str(headers[0].parent),
                        str(ROOT / "tests/test_persistent_nonce.cpp"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS: both hardware versions use identical persistent nonce headers "
          f"(SHA-256 {hashlib.sha256(contents[0]).hexdigest()}).")
    print("Host fault injection does not qualify physical brownout behavior or EEPROM endurance.")


if __name__ == "__main__":
    main()
