#!/usr/bin/env python3
"""Pinned Arduino setup, private builds, synthetic validation, and explicit upload."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {
    "acdc": ROOT / "firmware/beacons/acdc_avr128da32",
    "battery": ROOT / "firmware/beacons/battery_avr128db32",
    "server": ROOT / "firmware/servers/tconnectpro_868",
}
URLS = ["https://drazzy.com/package_drazzy.com_index.json",
        "https://espressif.github.io/arduino-esp32/package_esp32_index.json"]
SHA_COMMIT = "1c62a3f6e7c66b16d84db5940c4e01e5cc0a6d41"
DEFAULT_CONFIG = ROOT / ".arduino/arduino-cli.yaml"
SHA_LIBRARY = ROOT / ".arduino/user/libraries/usha256"
SHA_FILES = {
    "usha256.h": "2a7162a9d3deb2037777612c092ad706cf0667dfb70ba043ad830a43bdac8988",
    "usha256.cpp": "0488742624f793811f04c76af41f0c331e50758a0a0b4e44dd9f586b90200c71"}


def run(command):
    subprocess.run([str(x) for x in command], check=True)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cli(args, *command):
    return [args.cli, "--config-file", str(args.config_file), *map(str, command)]


def initialize(args):
    if args.config_file == DEFAULT_CONFIG and not DEFAULT_CONFIG.exists():
        DEFAULT_CONFIG.parent.mkdir(parents=True, exist_ok=True)
        # JSON is valid YAML; avoid an additional Python dependency.
        config = {"board_manager": {"additional_urls": URLS}, "directories": {
            "data": str(ROOT / ".arduino/data"),
            "downloads": str(ROOT / ".arduino/downloads"),
            "user": str(ROOT / ".arduino/user")}}
        DEFAULT_CONFIG.write_text(json.dumps(config, indent=2) + "\n")
    if not args.config_file.exists():
        raise SystemExit(f"Arduino configuration missing: {args.config_file}")


def setup(args, options):
    run(cli(args, "core", "update-index", "--additional-urls", ",".join(URLS)))
    run(cli(args, "core", "install", options["core"], "--additional-urls", ",".join(URLS)))
    run(cli(args, "lib", "install", "RadioLib@7.8.1"))
    if args.target == "server":
        run(cli(args, "lib", "install", "GFX Library for Arduino@1.4.6"))
    else:
        library = args.sha_library
        library.parent.mkdir(parents=True, exist_ok=True)
        if not library.exists():
            run(["git", "clone", "https://github.com/hypoactiv/usha256.git", library])
            run(["git", "-C", library, "checkout", "--detach", SHA_COMMIT])
        current = subprocess.check_output(
            ["git", "-C", str(library), "rev-parse", "HEAD"], text=True).strip()
        if current != SHA_COMMIT:
            raise SystemExit("usha256 checkout differs from pinned commit; inspect it before rebuilding.")
        (library / "library.properties").write_text(
            "name=usha256\nversion=0.0.0\nauthor=Upstream authors\n"
            "maintainer=Local packaging\nsentence=Pinned packet SHA-256 implementation.\n"
            "paragraph=See repository third-party provenance.\ncategory=Data Processing\n"
            "url=https://github.com/hypoactiv/usha256\narchitectures=avr,megaavr\nincludes=usha256.h\n")


def validation_config(sketch, server):
    content = (sketch / "config.example.h").read_text()
    if server:
        content = content.replace("CONFIGURED = false", "CONFIGURED = true")
        content = content.replace("0x00000000UL", "0x7C9E4A21UL")
        content = content.replace('"CHANGE_ME_AP_PASSWORD"', '"validation-only-ap"')
        # Exercise the largest addressable list, including a local row and
        # multiple LCD pages. These synthetic credentials are never flashed.
        entries = ",\n".join(
            f'  {{ {i}, "Validation {i}", 0x7C9E4A21UL }}' for i in range(1, 128))
        content, count = re.subn(
            r"static const SpdConfig SPD_CONFIGS\[\] = \{.*?\n\};",
            "static const SpdConfig SPD_CONFIGS[] = {\n" + entries + "\n};",
            content, flags=re.S)
        if count != 1:
            raise SystemExit("Cannot prepare synthetic server beacon configuration.")
        content = re.sub(r"constexpr int16_t SPD_LOCAL_ID = -?\d+;",
                         "constexpr int16_t SPD_LOCAL_ID = 0;", content)
    else:
        content = content.replace("0x00000000UL", "0x7C9E4A21UL")
    (sketch / "config.h").write_text(content)


def source_hashes(sketch):
    return {p.name: digest(p) for p in sorted(sketch.iterdir())
            if p.is_file() and p.suffix in (".ino", ".h", ".cpp", ".json")}


def compile_sketch(args, options, sketch, out):
    out.mkdir(parents=True, exist_ok=True)
    command = cli(args, "compile", "--fqbn", options["fqbn"], "--warnings", "default",
                  "--build-path", out / "cache", "--output-dir", out / "artifacts")
    if args.target != "server":
        if not args.sha_library.exists():
            raise SystemExit("Run setup first, or supply --sha-library with the pinned usha256 library.")
        for name, expected in SHA_FILES.items():
            path = args.sha_library / name
            if not path.exists() or digest(path) != expected:
                raise SystemExit(f"Pinned SHA dependency differs: {name}")
        command.extend(["--library", str(args.sha_library)])
    run([*command, sketch])


def build(args, options):
    original = TARGETS[args.target]
    out = ROOT / "build" / args.target
    if not (original / "config.h").exists():
        raise SystemExit("Copy config.example.h to config.h in the selected sketch and configure it first.")
    compile_sketch(args, options, original, out)
    images = {str(p.relative_to(out)): digest(p)
              for p in sorted((out / "artifacts").iterdir()) if p.is_file()}
    manifest = {"target": args.target, "fqbn": options["fqbn"],
                "source": source_hashes(original), "images": images}
    (out / "manifest.private.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Private firmware build: {out}")


def validate(args, options):
    out = ROOT / "build/validation" / args.target
    with tempfile.TemporaryDirectory(prefix="wspd-validation-") as temp:
        sketch = Path(temp) / TARGETS[args.target].name
        shutil.copytree(TARGETS[args.target], sketch, ignore=shutil.ignore_patterns("config.h"))
        validation_config(sketch, args.target == "server")
        compile_sketch(args, options, sketch, out)
    print("Synthetic compile passed. Validation images are never selected by flash.")


def prepare_beacon_storage(args, options):
    """Protect the nonce journal before the uploader's automatic chip erase.

    DxCore's ordinary upload does not set BODCFG and may erase before applying
    EESAVE. Program those fuses separately first, with signature checks and
    readback verification; never issue an erase or override a wrong signature.
    """
    output = subprocess.check_output(cli(args, "board", "details", "--fqbn",
                                         options["fqbn"], "--show-properties"), text=True)
    properties = dict(line.split("=", 1) for line in output.splitlines() if "=" in line)
    # CLI board details does not expand tool-local {path} placeholders.
    tool_path = properties["tools.avrdude.path"]
    executable = properties["tools.avrdude.cmd.path"].replace("{path}", tool_path)
    configuration = properties["tools.avrdude.config.path"].replace("{path}", tool_path)
    if "{" in executable + configuration:
        raise SystemExit("Cannot resolve AVRDUDE paths; no target operation attempted.")
    command = [executable, "-C", configuration,
               "-p", properties["build.mcu"], "-c", "pickit4_updi", "-P", "usb"]
    bodcfg = int(properties["bootloader.BODCFG"], 0)
    if (bodcfg & 0x0C) != 0x04:
        raise SystemExit("Beacon profile must enable continuous BOD while active for EEPROM writes.")
    with tempfile.TemporaryDirectory(prefix="wspd-fuses-") as temp:
        saved = Path(temp) / "fuse5.bin"
        run([*command, "-n", "-Ufuse5:r:" + str(saved) + ":r"])
        value = saved.read_bytes()
        if len(value) != 1:
            raise SystemExit("Cannot read EESAVE fuse; no firmware upload attempted.")
        # Keep every other existing fuse5 bit until the normal DxCore recipe
        # applies its configured RESET/EEPROM value. This invocation cannot erase.
        eesave = value[0] | 0x01
        run([*command, f"-Ufuse5:w:0x{eesave:02x}:m", f"-Ufuse5:v:0x{eesave:02x}:m",
             f"-Ufuse1:w:0x{bodcfg:02x}:m", f"-Ufuse1:v:0x{bodcfg:02x}:m"])
    print("EEPROM retention and active BOD fuses verified before firmware upload.")


def flash(args, options):
    out = ROOT / "build" / args.target
    manifest_path = out / "manifest.private.json"
    if not manifest_path.exists():
        raise SystemExit("Build configured production firmware before flashing.")
    manifest = json.loads(manifest_path.read_text())
    if manifest["target"] != args.target or manifest["fqbn"] != options["fqbn"]:
        raise SystemExit("Build target differs; rebuild before flashing.")
    if manifest["source"] != source_hashes(TARGETS[args.target]):
        raise SystemExit("Source/configuration changed; rebuild before flashing.")
    for name, expected in manifest["images"].items():
        path = out / name
        if not path.is_file() or digest(path) != expected:
            raise SystemExit("Build image changed; rebuild before flashing.")
    command = cli(args, "upload", "--fqbn", options["fqbn"], "--input-dir", out / "artifacts")
    if args.target == "server":
        if not args.port:
            raise SystemExit("Specify --port for the T Connect Pro USB connection.")
        command.extend(["--port", args.port])
    else:
        prepare_beacon_storage(args, options)
        command.extend(["--programmer", options["programmer"], "--verify"])
    run([*command, TARGETS[args.target]])


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["setup", "build", "validate", "flash"])
    parser.add_argument("target", choices=TARGETS)
    parser.add_argument("--cli", default="arduino-cli")
    parser.add_argument("--config-file", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--sha-library", type=Path, default=SHA_LIBRARY)
    parser.add_argument("--port")
    args = parser.parse_args()
    args.config_file = args.config_file.expanduser().resolve()
    args.sha_library = args.sha_library.expanduser().resolve()
    options = json.loads((TARGETS[args.target] / "build-options.json").read_text())
    initialize(args)
    {"setup": setup, "build": build, "validate": validate, "flash": flash}[args.action](args, options)


if __name__ == "__main__":
    main()
