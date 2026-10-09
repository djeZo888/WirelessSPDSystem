#!/usr/bin/env python3
"""Offline checks for EEPROM-preserving beacon upload; never opens USB."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("wspd_firmware", ROOT / "tools/firmware.py")
firmware = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(firmware)
PROFILES = {
    "acdc": ("acdc_avr128da32", "avr128da32", "enabled", 0x05),
    "battery": ("battery_avr128db32", "avr128db32", "ensampslow", 0x16),
}


class FuseUploadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="wspd-fuse-tests-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.events = []
        self.probe = bytes([0xC8])
        self.fail_stage = None
        self.profile = "acdc"
        self.bodcfg = 0x05
        self.avrdude_path = "/offline/mock-avrdude-tool"
        self.options = self.read_profile(self.profile)
        self.args = SimpleNamespace(target=self.profile, cli="mock-arduino-cli",
                                    config_file=self.root / "arduino-cli.yaml", port=None)
        self.targets = {name: self.root / folder for name, (folder, _, _, _) in PROFILES.items()}
        for name, sketch in self.targets.items():
            sketch.mkdir()
            (sketch / (sketch.name + ".ino")).write_text("// Offline manifest fixture\n")
            artifacts = self.root / "build" / name / "artifacts"
            artifacts.mkdir(parents=True)
            image = artifacts / (sketch.name + ".hex")
            image.write_text(":00000001FF\n")
            manifest = {"target": name, "fqbn": self.read_profile(name)["fqbn"],
                        "source": firmware.source_hashes(sketch),
                        "images": {"artifacts/" + image.name: firmware.digest(image)}}
            (artifacts.parent / "manifest.private.json").write_text(json.dumps(manifest))
        self.patches = [patch.object(firmware, "ROOT", self.root),
                        patch.object(firmware, "TARGETS", self.targets),
                        patch.object(firmware.subprocess, "check_output", self.board_details),
                        patch.object(firmware.subprocess, "run", self.run_command),
                        patch.object(firmware, "print", create=True)]
        for mocked in self.patches:
            mocked.start()
            self.addCleanup(mocked.stop)

    @staticmethod
    def read_profile(target):
        folder = PROFILES[target][0]
        return json.loads((ROOT / "firmware/beacons" / folder / "build-options.json").read_text())

    def select_profile(self, target):
        self.profile = target
        self.args.target = target
        self.options = self.read_profile(target)
        self.bodcfg = PROFILES[target][3]

    def board_details(self, command, text):
        self.assertTrue(text)
        self.assertIn("--show-properties", command)
        self.assertEqual(command[command.index("--fqbn") + 1], self.options["fqbn"])
        self.events.append(("details", list(command)))
        if self.fail_stage == "details":
            raise subprocess.CalledProcessError(1, command)
        return "\n".join([
            "tools.avrdude.path=" + self.avrdude_path,
            "tools.avrdude.cmd.path={path}/bin/avrdude",
            "tools.avrdude.config.path={path}/etc/avrdude.conf",
            "build.mcu=" + PROFILES[self.profile][1],
            "bootloader.BODCFG=" + bin(self.bodcfg),
        ])

    def run_command(self, command, check):
        self.assertTrue(check, "Device-command errors must stop the upload.")
        command = list(command)
        if command[0] == self.args.cli:
            self.assertIn("upload", command)
            self.events.append(("upload", command))
            return subprocess.CompletedProcess(command, 0)
        self.assertEqual(command[0], "/offline/mock-avrdude-tool/bin/avrdude")
        self.assertEqual(command[command.index("-C") + 1], "/offline/mock-avrdude-tool/etc/avrdude.conf")
        self.assertEqual(command[command.index("-p") + 1], PROFILES[self.profile][1])
        self.assertEqual(command[command.index("-c") + 1], "pickit4_updi")
        self.assertEqual(command[command.index("-P") + 1], "usb")
        for forbidden in ("-e", "-F", "--erase", "--force"):
            self.assertNotIn(forbidden, command)
        self.assertFalse(any("flash:w:" in argument for argument in command))
        read = next((argument for argument in command if argument.startswith("-Ufuse5:r:")), None)
        stage = "probe" if read else "verify"
        self.events.append((stage, command))
        if self.fail_stage == stage:
            # A signature mismatch or AVRdude's failed verification is fatal.
            raise subprocess.CalledProcessError(1, command)
        if read:
            self.assertIn("-n", command)
            self.assertFalse(any(":w:" in argument for argument in command))
            self.assertTrue(read.endswith(":r"), "The fuse read must use raw bytes.")
            if self.probe is not None:
                Path(read[len("-Ufuse5:r:"):-len(":r")]).write_bytes(self.probe)
        else:
            self.assertNotIn("-n", command)
        return subprocess.CompletedProcess(command, 0)

    def test_both_profiles_prepare_and_verify_before_upload(self):
        for target in PROFILES:
            with self.subTest(target=target):
                self.events.clear()
                self.select_profile(target)
                firmware.flash(self.args, self.options)
                self.assertEqual([stage for stage, _ in self.events],
                                 ["details", "probe", "verify", "upload"])
                fuse_command = self.events[2][1]
                self.assertEqual(fuse_command[-4:], [
                    "-Ufuse5:w:0xc9:m", "-Ufuse5:v:0xc9:m",
                    f"-Ufuse1:w:0x{self.bodcfg:02x}:m",
                    f"-Ufuse1:v:0x{self.bodcfg:02x}:m"])
                upload = self.events[3][1]
                self.assertIn("--verify", upload)
                self.assertEqual(upload[upload.index("--programmer") + 1], "pickit4")

    def test_profiles_enable_retention_and_expected_bod_mode(self):
        for target, (_, _, bodmode, expected) in PROFILES.items():
            with self.subTest(target=target):
                profile = self.read_profile(target)
                options = dict(option.split("=", 1) for option in profile["fqbn"].split(":", 3)[3].split(","))
                self.assertEqual(options["eesave"], "enable")
                self.assertEqual(options["bodmode"], bodmode)
                self.assertEqual(options["bodvoltage"], "1v9")
                self.assertEqual(profile["firmware_version"], 1)
                self.assertEqual(expected & 0x0C, 0x04)

    def test_every_other_existing_fuse5_bit_is_preserved(self):
        for value in range(256):
            with self.subTest(fuse5=value):
                self.events.clear()
                self.probe = bytes([value])
                firmware.prepare_beacon_storage(self.args, self.options)
                command = self.events[-1][1]
                written = int(next(arg for arg in command if arg.startswith("-Ufuse5:w:")).split(":")[2], 0)
                self.assertEqual(written & 0xFE, value & 0xFE)
                self.assertEqual(written & 0x01, 0x01)
                self.assertIn(f"-Ufuse5:v:0x{written:02x}:m", command)

    def test_bad_fuse_read_size_stops_before_fuse_write_or_upload(self):
        for probe in (b"", b"\xc8\xff", bytes(range(256))):
            with self.subTest(size=len(probe)):
                self.events.clear()
                self.probe = probe
                with self.assertRaisesRegex(SystemExit, "Cannot read EESAVE fuse"):
                    firmware.flash(self.args, self.options)
                self.assertEqual([stage for stage, _ in self.events], ["details", "probe"])

    def test_missing_fuse_read_file_stops_before_upload(self):
        self.probe = None
        with self.assertRaises(FileNotFoundError):
            firmware.flash(self.args, self.options)
        self.assertEqual([stage for stage, _ in self.events], ["details", "probe"])

    def test_failed_details_probe_or_verification_never_uploads(self):
        for stage, expected in [("details", ["details"]),
                                ("probe", ["details", "probe"]),
                                ("verify", ["details", "probe", "verify"])]:
            with self.subTest(stage=stage):
                self.events.clear()
                self.fail_stage = stage
                with self.assertRaises(subprocess.CalledProcessError):
                    firmware.flash(self.args, self.options)
                self.assertEqual([event for event, _ in self.events], expected)

    def test_disabled_or_sampled_active_bod_is_rejected_before_programmer(self):
        for bodcfg in (0x00, 0x02, 0x0A, 0x1A, 0x0C, 0x20, 0xFF):
            with self.subTest(bodcfg=bodcfg):
                self.events.clear()
                self.bodcfg = bodcfg
                with self.assertRaisesRegex(SystemExit, "continuous BOD while active"):
                    firmware.flash(self.args, self.options)
                self.assertEqual([stage for stage, _ in self.events], ["details"])

    def test_unresolved_tool_property_stops_before_programmer(self):
        self.avrdude_path = "{runtime.unresolved.path}"
        with self.assertRaises(SystemExit):
            firmware.flash(self.args, self.options)
        self.assertEqual([stage for stage, _ in self.events], ["details"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
