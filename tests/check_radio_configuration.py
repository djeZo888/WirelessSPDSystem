#!/usr/bin/env python3
"""Compile actual firmware configuration guards against valid/invalid settings.

This performs syntax checks only. It never opens a programmer or a radio.
The beacon checks use GCC because their AVR-compatible integral-type guard is
not supported by every host compiler. Server checks use the host C++ compiler.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BEACONS = ROOT / "firmware/beacons"
SERVER = ROOT / "firmware/servers/tconnectpro_868"


def section(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise RuntimeError(f"Firmware guard markers changed: {start!r}, {end!r}")
    return source.split(start, 1)[1].split(end, 1)[0]


def function(source, name):
    match = re.search(r"static\s+bool\s+" + name + r"\([^;{]*\)\s*\{", source)
    if not match:
        raise RuntimeError(f"Cannot find actual firmware function: {name}")
    at, depth = match.end(), 1
    while depth:
        depth += (source[at] == "{") - (source[at] == "}")
        at += 1
    return source[match.start():at]


def replace_macro(config, name, value):
    result, count = re.subn(r"^#define " + re.escape(name) + r"\s+.*$",
                            "#define " + name + " " + value, config, flags=re.M)
    if count != 1:
        raise RuntimeError(f"Expected one configuration macro: {name}")
    return result


def replace_constant(config, name, value):
    result, count = re.subn(
        r"(^constexpr\s+\S+\s+" + re.escape(name) + r"\s*=\s*)[^;]+;",
        lambda match: match[1] + value + ";", config, flags=re.M)
    if count != 1:
        raise RuntimeError(f"Expected one configuration constant: {name}")
    return result


def beacon_compiler(work, explicit):
    if explicit:
        candidates = [explicit]
    else:
        candidates = []
        for pattern in ("/opt/homebrew/bin/g++-*", "/usr/local/bin/g++-*"):
            candidates.extend(sorted(Path("/").glob(pattern.lstrip("/")), reverse=True))
        candidates.extend(filter(None, [shutil.which("g++"), shutil.which("avr-g++")]))
        for directory in (ROOT / ".arduino/data", Path.home() / "Library/Arduino15",
                          Path.home() / ".arduino15"):
            candidates.extend(directory.glob("packages/DxCore/tools/avr-gcc/*/bin/avr-g++"))
    probe = work / "compiler-probe.cpp"
    probe.write_text('static_assert(__builtin_classify_type(1) == 1, "integer");\n')
    for candidate in candidates:
        result = subprocess.run([str(candidate), "-std=c++11", "-fsyntax-only", str(probe)],
                                text=True, capture_output=True)
        if result.returncode == 0:
            return str(candidate)
    raise SystemExit("A GCC compiler supporting __builtin_classify_type is required. "
                     "Run Arduino beacon setup or supply --beacon-compiler.")


class GuardChecks:
    def __init__(self, work):
        self.work = work
        self.count = 0
        self.accepted = 0
        self.rejected = 0

    def check(self, compiler, config, guards, name, accepted, diagnostic="", server=False):
        (self.work / "config.h").write_text(config)
        driver = self.work / "guards.cpp"
        prefix = "#include <stdint.h>\n#include <stddef.h>\n"
        if server:
            prefix += "#include <type_traits>\n#define LOW 0\n#define HIGH 1\n"
        driver.write_text(prefix + '#include "config.h"\n' + guards)
        result = subprocess.run([compiler, "-std=c++11", "-fsyntax-only", "-I", str(SERVER),
                                 str(driver)], text=True, capture_output=True)
        if accepted and result.returncode != 0:
            raise RuntimeError(f"Valid settings rejected: {name}\n{result.stderr}")
        if not accepted and result.returncode == 0:
            raise RuntimeError(f"Invalid settings accepted: {name}")
        if not accepted and diagnostic not in result.stderr:
            raise RuntimeError(f"Wrong rejection for {name}; expected {diagnostic!r}\n"
                               + result.stderr)
        self.count += 1
        self.accepted += accepted
        self.rejected += not accepted


def fixed_radio_contract(sketch, config, server=False):
    expected = {"LORA_BW_KHZ": "125.0f", "LORA_CR": "5", "LORA_SYNCWORD": "0x12",
                "LORA_PREAMBLE": "8", "LORA_CRC_ENABLED": "true"}
    for name, value in expected.items():
        expression = r"\bconstexpr\s+\S+\s+" + name + r"\s*=\s*" + re.escape(value) + r"\s*;"
        if not re.search(expression, sketch):
            raise RuntimeError(f"Fixed modem setting changed or moved: {name}")
        if re.search(r"\b(?:const|constexpr|#define)\b[^\n]*\b" + name + r"\b", config):
            raise RuntimeError(f"Fixed modem setting exposed in configuration: {name}")
    if "LORA_CHANNEL_FREQS_MHZ" in sketch or "LORA_CHANNEL_COUNT" in sketch:
        raise RuntimeError("Legacy beacon channel table remains")
    frequency_use = (r"radio\.setFrequency\(LORA_FREQ_MHZ\)" if server
                     else r"radio\.begin\(\s*LORA_FREQ_MHZ\s*,")
    if not re.search(frequency_use, sketch):
        raise RuntimeError("Radio initialization does not use configured frequency directly")


def server_string_checks(work, compiler, config, guards, sketch):
    """Execute the actual startup validator with Wi-Fi boundary settings."""
    cases = [
        ("baseline", "", "", True),
        ("station password seven", "WIFI_PASSWORD", '"1234567"', False),
        ("station password eight", "WIFI_PASSWORD", '"12345678"', True),
        ("station open network", "WIFI_PASSWORD", '""', True),
        ("station 64 hex digits", "WIFI_PASSWORD", '"' + "a" * 64 + '"', True),
        ("station 64 nonhex", "WIFI_PASSWORD", '"' + "g" * 64 + '"', False),
        ("station SSID 32", "WIFI_SSID", '"' + "s" * 32 + '"', True),
        ("station SSID 33", "WIFI_SSID", '"' + "s" * 33 + '"', False),
        ("AP SSID empty", "FALLBACK_AP_SSID", '""', False),
        ("AP SSID one", "FALLBACK_AP_SSID", '"s"', True),
        ("AP SSID 32", "FALLBACK_AP_SSID", '"' + "s" * 32 + '"', True),
        ("AP SSID 33", "FALLBACK_AP_SSID", '"' + "s" * 33 + '"', False),
        ("AP password seven", "FALLBACK_AP_PASSWORD", '"1234567"', False),
        ("AP password eight", "FALLBACK_AP_PASSWORD", '"12345678"', True),
        ("AP password 63", "FALLBACK_AP_PASSWORD", '"' + "p" * 63 + '"', True),
        ("AP password 64", "FALLBACK_AP_PASSWORD", '"' + "p" * 64 + '"', False),
        ("AP password null", "FALLBACK_AP_PASSWORD", "nullptr", False),
    ]
    (work / "config.h").write_text(config)
    driver = work / "runtime-config.cpp"
    body = ('#include <stdint.h>\n#include <stddef.h>\n#include <type_traits>\n'
            '#include <cstdio>\n#include <cstring>\n#define LOW 0\n#define HIGH 1\n'
            '#include "config.h"\n' + guards
            + '\nchar lastSetupFailureText[112];\nint16_t lastSetupFailureCode;\n'
            + function(sketch, "validateSpdConfig") + '\nint main() {\n')
    for name, field, value, accepted in cases:
        body += ('WIFI_SSID=""; WIFI_PASSWORD=""; FALLBACK_AP_SSID="validation"; '
                 'FALLBACK_AP_PASSWORD="validation-only-ap";\n')
        if field:
            body += field + "=" + value + ";\n"
        body += ('if (validateSpdConfig() != ' + str(accepted).lower() + ') { '
                 'std::fprintf(stderr,"Runtime configuration check failed: ' + name
                 + '\\n"); return 1; }\n')
    body += "return 0;\n}\n"
    driver.write_text(body)
    binary = work / "runtime-config"
    subprocess.run([compiler, "-std=c++11", "-I", str(SERVER), str(driver), "-o", str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True)
    return len(cases)


BEACON_CASES = {
    "WSPD_SHARED_SECRET": [
        ("0x00000001UL", True), ("0xFFFFFFFFUL", True), ("0UL", False),
        ("-1LL", False), ("0x100000000ULL", False), ("1.5f", False)],
    "WSPD_BEACON_ID": [
        ("0", False), ("1", True), ("127", True), ("128", False), ("10.5f", False)],
    "WSPD_LORA_SF": [
        ("4", False), ("5", True), ("12", True), ("13", False),
        ("10.5f", False), ("261", False)],
    "WSPD_LORA_TX_DBM": [
        ("-10", False), ("-9", True), ("22", True), ("23", False),
        ("10.5f", False), ("246", False)],
    "WSPD_LORA_FREQ_MHZ": [
        ("863.0625f", True), ("869.9375f", True), ("865.4321f", True),
        ("863.0f", False), ("870.0f", False),
        ('__builtin_nanf("")', False), ("__builtin_inff()", False)],
    "WSPD_NONCE_START": [
        ("0ULL", False), ("1ULL", True), ("18446744073709551614ULL", True),
        ("18446744073709551615ULL", False), ("1.5f", False)],
}


SERVER_CASES = {
    "LORA_SF": [("4", False), ("5", True), ("12", True), ("13", False),
                ("10.5f", False), ("261", False)],
    "LORA_RX_TX_DBM": [("-10", False), ("-9", True), ("22", True), ("23", False),
                       ("10.5f", False), ("246", False)],
    "LORA_FREQ_MHZ": BEACON_CASES["WSPD_LORA_FREQ_MHZ"],
    "DISPLAY_PAGE_SECONDS": [("0", False), ("1", True), ("3600", True), ("3601", False),
                             ("65537", False), ("5.5f", False), ("-1LL", False)],
    "WIFI_CONNECT_TIMEOUT_MS": [("0UL", True), ("2147483647UL", True),
                                ("2147483648UL", False), ("-1LL", False),
                                ("1.5f", False), ("0x100000001ULL", False)],
    "DISPLAY_PERIODIC_REFRESH_MS": [("0UL", False), ("1UL", True),
                                    ("2147483647UL", True), ("2147483648UL", False),
                                    ("-1LL", False), ("1.5f", False),
                                    ("0x100000001ULL", False)],
    "TOUCH_POLL_MS": [("0UL", False), ("1UL", True), ("2147483647UL", True),
                      ("2147483648UL", False), ("-1LL", False),
                      ("1.5f", False), ("0x100000001ULL", False)],
    "TOUCH_DEBOUNCE_MS": [("0UL", True), ("2147483647UL", True),
                          ("2147483648UL", False), ("-1LL", False),
                          ("1.5f", False), ("0x100000001ULL", False)],
    "TOUCH_RELEASE_STABLE_MS": [("0UL", True), ("2147483647UL", True),
                                ("2147483648UL", False), ("-1LL", False),
                                ("1.5f", False), ("0x100000001ULL", False)],
    "SPD_LOCAL_POLL_MS": [("0UL", False), ("1UL", True), ("2147483647UL", True),
                          ("2147483648UL", False), ("-1LL", False),
                          ("1.5f", False), ("0x100000001ULL", False)],
    "SPD_LOCAL_DEBOUNCE_MS": [("0UL", True), ("2147483647UL", True),
                              ("2147483648UL", False), ("-1LL", False),
                              ("1.5f", False), ("0x100000001ULL", False)],
    "STALE_AFTER_SECONDS": [("0UL", False), ("1UL", True), ("2147483UL", True),
                            ("2147484UL", False), ("-1LL", False),
                            ("1.5f", False), ("0x100000001ULL", False)],
    "PACKET_LOSS_WINDOW_SIZE": [("0", False), ("1", True), ("65535", True),
                                ("65536", False), ("65537", False),
                                ("-1LL", False), ("1000.5f", False)],
    "CONFIGURED": [("2", False)],
    "LOW_BATTERY_WARNING_ENABLED": [("true", True), ("false", True), ("2", False)],
    "LOW_BATTERY_WARNING_V": [("2.20f", True), ("4.74f", True), ("2.19f", False),
                              ("4.75f", False), ('__builtin_nanf("")', False),
                              ("__builtin_inff()", False)],
    "ALARM_PERIOD_MS": [("0UL", False), ("2147483647UL", True), ("2147483648UL", False),
                        ("-1LL", False), ("1.5f", False), ("0x100000001ULL", False)],
    "ALARM_FAIL_ON_MS": [("0UL", True), ("60000UL", True), ("60001UL", False),
                         ("-1LL", False), ("1.5f", False), ("0x100000001ULL", False)],
    "ALARM_LOW_BATTERY_ON_MS": [("0UL", True), ("60000UL", True), ("60001UL", False),
                                ("-1LL", False), ("1.5f", False), ("0x100000001ULL", False)],
    "RELAY_ACTIVE_LEVEL": [("HIGH", False), ("2", False), ("0.5f", False), ("256", False)],
    "SPD_LOCAL_ID": [("-2", False), ("-1", True), ("0", True), ("127", True), ("128", False),
                     ("65536", False), ("-65536", False), ("0.5f", False)],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--beacon-compiler", help="GCC/AVR GCC path; discovered by default")
    parser.add_argument("--host-compiler", default=shutil.which("c++"))
    args = parser.parse_args()
    if not args.host_compiler:
        raise SystemExit("A host C++ compiler is required for server guard checks.")
    with tempfile.TemporaryDirectory(prefix="wspd-radio-config-") as temp:
        work = Path(temp)
        compiler = beacon_compiler(work, args.beacon_compiler)
        checks = GuardChecks(work)
        for folder in ("acdc_avr128da32", "battery_avr128db32"):
            directory = BEACONS / folder
            sketch = (directory / (folder + ".ino")).read_text()
            config = (directory / "config.example.h").read_text()
            fixed_radio_contract(sketch, config)
            guards = section(sketch, "// ---------------------- NETWORK / DEVICE CONFIG ----------------------",
                             "// Payload identity byte:")
            nonce = (directory / "nonce_eeprom.h").read_text()
            guards += "\n#ifndef WSPD_NONCE_START\n" + section(
                nonce, "#ifndef WSPD_NONCE_START\n", "static_assert(EEPROM_SIZE")
            config = replace_macro(config, "WSPD_SHARED_SECRET", "0x7C9E4A21UL")
            checks.check(compiler, config, guards, folder + " baseline", True)
            for field, cases in BEACON_CASES.items():
                for value, accepted in cases:
                    checks.check(compiler, replace_macro(config, field, value), guards,
                                 f"{folder} {field}={value}", accepted, field)
            checks.check(compiler, config + "\n#define WSPD_LORA_CHANNEL_ID 1\n", guards,
                         folder + " old channel configuration", False,
                         "Replace WSPD_LORA_CHANNEL_ID with WSPD_LORA_FREQ_MHZ")
            no_frequency = re.sub(r"^#define WSPD_LORA_FREQ_MHZ.*\n", "", config, flags=re.M)
            checks.check(compiler, no_frequency, guards, folder + " missing frequency", False,
                         "Set WSPD_LORA_FREQ_MHZ in config.h")

        sketch = (SERVER / "tconnectpro_868.ino").read_text()
        config = (SERVER / "config.example.h").read_text()
        fixed_radio_contract(sketch, config, server=True)
        guards = section(sketch, "// BEGIN CONFIG VALIDATION", "// END CONFIG VALIDATION")
        config = replace_constant(config, "CONFIGURED", "true")
        config = config.replace("0x00000000UL", "0x7C9E4A21UL")
        config = config.replace('"CHANGE_ME_AP_PASSWORD"', '"validation-only-ap"')
        checks.check(args.host_compiler, config, guards, "server baseline", True, server=True)
        for field, cases in SERVER_CASES.items():
            for value, accepted in cases:
                diagnostic = "Relay levels" if field == "RELAY_ACTIVE_LEVEL" and value == "HIGH" else field
                checks.check(args.host_compiler, replace_constant(config, field, value), guards,
                             f"server {field}={value}", accepted, diagnostic, server=True)
        local = replace_constant(config, "SPD_LOCAL_ID", "0")
        for pin, accepted in [("-1", False), ("0", True), ("21", True), ("22", False),
                              ("25", False), ("26", True), ("48", True), ("49", False),
                              ("271", False), ("-256", False), ("15.5f", False)]:
            checks.check(args.host_compiler, replace_constant(local, "SPD_LOCAL_PIN", pin),
                         guards, f"server local GPIO={pin}", accepted, "SPD_LOCAL_PIN", server=True)
        checks.check(args.host_compiler, replace_constant(config, "CONFIGURED", "false"),
                     guards, "server unconfigured", False, "Configure config.h", server=True)
        for value in ("-1LL", "0x100000000ULL", "1.5f"):
            checks.check(args.host_compiler, config.replace("0x7C9E4A21UL", value), guards,
                         f"server secret={value}", False, "narrow", server=True)
        runtime = server_string_checks(work, args.host_compiler, config, guards, sketch)
        print(f"PASS: {checks.count} actual firmware guard checks "
              f"({checks.accepted} valid settings accepted, {checks.rejected} invalid rejected); "
              "fixed modem settings and direct frequency use verified for all three sketches.")
        print(f"PASS: {runtime} runtime Wi-Fi configuration boundary cases using validateSpdConfig().")
        print("Syntax checks do not qualify RF reception, antenna coverage, or legal sub-band operation.")


if __name__ == "__main__":
    main()
