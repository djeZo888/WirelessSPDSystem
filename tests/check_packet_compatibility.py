#!/usr/bin/env python3
"""Exercise the actual TX encoder and RX tag verifier against independent vectors."""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"static\s+(?:void|bool)\s+" + name + r"\([^;{]*\)\s*\{", source)
    if not match:
        raise RuntimeError(f"Cannot find firmware function: {name}")
    at = match.end()
    depth = 1
    while depth:
        depth += (source[at] == "{") - (source[at] == "}")
        at += 1
    return source[match.start():at]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, default=ROOT / ".arduino/user/libraries/usha256")
    args = parser.parse_args()
    compiler = shutil.which("c++")
    if not compiler:
        raise SystemExit("A host C++ compiler is required.")
    expected_hashes = {
        "usha256.h": "2a7162a9d3deb2037777612c092ad706cf0667dfb70ba043ad830a43bdac8988",
        "usha256.cpp": "0488742624f793811f04c76af41f0c331e50758a0a0b4e44dd9f586b90200c71"}
    for name, expected in expected_hashes.items():
        if hashlib.sha256((args.library / name).read_bytes()).hexdigest() != expected:
            raise SystemExit(f"Dependency checksum mismatch: {name}")
    receiver = (ROOT / "firmware/servers/tconnectpro_868/tconnectpro_868.ino").read_text()
    tests = 0
    with tempfile.TemporaryDirectory(prefix="wspd-packets-") as temp:
        work = Path(temp)
        (work / "avr").mkdir()
        (work / "avr/pgmspace.h").write_text(
            "#pragma once\n#include <stdint.h>\n#define PROGMEM\n"
            "#define pgm_read_dword(p) (*(const uint32_t *)(p))\n"
            "#define pgm_read_byte(p) (*(const uint8_t *)(p))\n")
        for folder, hw, fw in [("acdc_avr128da32", 1, 1), ("battery_avr128db32", 2, 1)]:
            beacon = (ROOT / "firmware/beacons" / folder / (folder + ".ino")).read_text()
            for constant, expected in [("BEACON_HW_TYPE", hw), ("BEACON_FIRMWARE_VERSION", fw)]:
                value = re.search(r"constexpr uint8_t\s+" + constant + r"\s*=\s*(\d+);", beacon)
                if not value or int(value[1]) != expected:
                    raise SystemExit(f"Unexpected {constant}: {folder}")
            body = """#include <usha256.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
constexpr uint32_t SECRET=0x7C9E4A21UL;
constexpr uint8_t SPD_ID=10, BEACON_DATA_LEN=12, BEACON_HASH_LEN=8;
constexpr size_t SPD_AUTH_PAYLOAD_LEN=12;
void mbedtls_sha256(const uint8_t* input, size_t count, uint8_t* hash, int mode) {
  if (mode!=0) std::abort();
  uint8_t data[16]; std::memcpy(data,input,count);
  Sha256Context ctx; sha256_init(&ctx); sha256_update(&ctx,data,count); sha256_final(&ctx,hash);
}
"""
            body += f"constexpr uint8_t BEACON_HW_TYPE={hw}, BEACON_FIRMWARE_VERSION={fw};\n"
            for name in ["packUint64LE", "buildBeaconPacket"]:
                body += function(beacon, name) + "\n"
            for name in ["constantTimeEqual8", "verifyTag"]:
                body += function(receiver, name) + "\n"
            body += """int main(int argc,char** argv) {
  if(argc!=5) return 2;
  uint8_t packet[20];
  buildBeaconPacket(std::atoi(argv[1]),std::atoi(argv[2]),std::strtoull(argv[3],nullptr,10),std::atoi(argv[4]),packet);
  if(!verifyTag(packet,SECRET) || verifyTag(packet,SECRET^1)) return 3;
  for(unsigned i=0;i<20;i++) { packet[i]^=1; if(verifyTag(packet,SECRET)) return 4; packet[i]^=1; }
  for(auto byte:packet) std::printf("%02x",byte);
}
"""
            driver = work / (folder + ".cpp")
            driver.write_text(body)
            binary = work / folder
            subprocess.run([compiler, "-std=c++11", "-O2", "-I", str(work), "-I", str(args.library),
                            str(driver), str(args.library / "usha256.cpp"), "-o", str(binary)], check=True)
            for status, temperature, nonce, cell in [
                (1, 25, 0, 255), (0, -20, 0x0123456789ABCDEF, 100),
                (1, -128, 0xFFFFFFFFFFFFFFFF, 0), (0, 127, 1, 150)]:
                prefix = bytes([(hw << 4) | fw, 21 if status else 20, temperature & 255])
                prefix += nonce.to_bytes(8, "little") + bytes([cell])
                expected = prefix + hashlib.sha256(prefix + (0x7C9E4A21).to_bytes(4, "little")).digest()[:8]
                actual = subprocess.check_output([str(binary), str(status), str(temperature), str(nonce), str(cell)], text=True)
                if bytes.fromhex(actual) != expected:
                    raise SystemExit(f"Packet mismatch: {folder}")
                tests += 1
    print(f"PASS: {tests} TX/RX packet vectors; wrong-key and all 20 byte-tampering cases rejected per vector.")
    print("Host PROGMEM and SHA-256 API shims do not qualify AVR flash placement or RF reception.")


if __name__ == "__main__":
    main()
