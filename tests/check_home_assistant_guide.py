#!/usr/bin/env python3
"""Validate local HA resources, example YAML, and actual availability templates."""
from html.parser import HTMLParser
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

try:
    import yaml
    from jinja2 import Environment, StrictUndefined
except ImportError as exc:
    raise SystemExit("Install PyYAML and Jinja2 in the validation environment: " + str(exc))

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "firmware/servers/tconnectpro_868"
EXAMPLES = ROOT / "integrations/home-assistant"


class Loader(yaml.SafeLoader):
    def construct_mapping(self, node, deep=False):
        keys = [self.construct_object(key, deep=deep) for key, _ in node.value]
        if len(keys) != len(set(keys)):
            raise ValueError("Duplicate YAML mapping key")
        return super().construct_mapping(node, deep=deep)


Loader.add_constructor("!secret", lambda loader, node: "SECRET:" + loader.construct_scalar(node))


class GuideParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids, self.anchors, self.assets, self.code, self.blocks = set(), [], [], None, []
    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            assert attrs["id"] not in self.ids
            self.ids.add(attrs["id"])
        if tag == "a" and attrs.get("href", "").startswith("#"):
            self.anchors.append(attrs["href"][1:])
        if tag in ("script", "img", "iframe", "link"):
            self.assets.append((tag, attrs))
        if tag == "code" and attrs.get("class") == "language-yaml":
            self.code = []
    def handle_data(self, data):
        if self.code is not None:
            self.code.append(data)
    def handle_endtag(self, tag):
        if tag == "code" and self.code is not None:
            self.blocks.append("".join(self.code))
            self.code = None


def load(name):
    return yaml.load((EXAMPLES / name).read_text(), Loader=Loader)


def render(template, rows, available=True, gateway=None, **extra):
    environment = Environment(undefined=StrictUndefined)
    environment.globals.update(
        has_value=lambda name: available if name == "sensor.wireless_spd_snapshot" else False,
        state_attr=lambda name, field: {"spds": rows, "gateway": gateway}.get(field),
        **extra,
    )
    return environment.from_string(template).render().strip()


def main():
    subprocess.run([shutil.which("python3"), str(ROOT / "tools/generate_ha_guide.py"), "--check"], check=True)
    header = (SERVER / "ha_guide.h").read_text()
    html = re.search(r'R"HAHTML\((.*?)\)HAHTML";', header, re.S).group(1)
    embedded_yaml = re.search(r'R"HAYAML\((.*?)\)HAYAML";', header, re.S).group(1)
    assert html == (EXAMPLES / "guide.html").read_text()
    assert embedded_yaml == (EXAMPLES / "rest-example.yaml").read_text()
    parser = GuideParser()
    parser.feed(html)
    assert len(parser.anchors) >= 10 and set(parser.anchors) <= parser.ids
    assert not parser.assets, "The offline guide must not load external assets or scripts"
    assert "{{include:" not in html and "<!-- ha-example:" not in html
    canonical = (ROOT / "docs/home-assistant.md").read_text()
    assert "Home Assistant Core 2025.8 or newer" in canonical
    preparation = canonical.split("## 1. Prepare", 1)[1].split("## 2A.", 1)[0]
    installation = canonical.split("## 4. Configure", 1)[1].split("## 5.", 1)[0]
    assert "python3 tools/firmware.py setup server" in preparation
    assert "always refreshes online package indexes" in preparation
    assert "same computer" in preparation and "`.arduino/`" in preparation
    assert "docker pull eclipse-mosquitto:2" in preparation
    broker = canonical.split("## 2B.", 1)[1].split("## 3.", 1)[0]
    assert "docker pull " not in broker
    assert len(re.findall(r"docker run --pull never ", broker)) == 3
    broker_app = canonical.split("## 2A.", 1)[1].split("## 2B.", 1)[0]
    assert "**while online**" in broker_app and "**already installed**" in broker_app
    rest_setup = canonical.split("## 9.", 1)[1].split("## 10.", 1)[0]
    assert "already installed\nFile editor" in rest_setup
    assert "python3 tools/firmware.py setup server" not in installation
    assert "python3 tools/firmware.py build server" in installation
    for text in parser.blocks:
        yaml.load(text, Loader=Loader)
    for name in ("fault-notification.yaml", "unavailable-notification.yaml", "mobile-notification-action.yaml", "rest-example.yaml"):
        assert (EXAMPLES / name).read_text().strip() in parser.blocks, f"Complete {name} missing from offline guide"

    rest = load("rest-example.yaml")
    assert set(rest) == {"rest", "rest_command", "template", "recorder"}
    assert len(rest["rest"]) == 1 and rest["rest"][0]["resource"] == "SECRET:wireless_spd_api_url"
    assert rest["rest"][0]["scan_interval"] == 10
    assert rest["rest"][0]["sensor"][0]["json_attributes"] == ["gateway", "summary", "spds"]
    template = rest["template"][0]
    status, fault, mute = (template[key][0] for key in ("sensor", "binary_sensor", "switch"))
    scenarios = [
        (True, [{"spd_id": 1, "status": "OK"}], "OK", "True", "False"),
        (True, [{"spd_id": 1, "status": "FAIL"}], "FAIL", "True", "True"),
        (True, [{"spd_id": 1, "status": "UNKNOWN"}], "UNKNOWN", "False", "False"),
        (True, [{"spd_id": 1, "status": "STALE"}], "STALE", "False", "False"),
        (True, [{"spd_id": 2, "status": "OK"}], "UNKNOWN", "False", "False"),
        (False, [{"spd_id": 1, "status": "OK"}], "OK", "False", "False"),
        (False, None, "UNKNOWN", "False", "False"),
    ]
    for available, rows, expected_status, expected_available, expected_fault in scenarios:
        assert render(status["state"], rows, available) == expected_status
        assert render(fault["availability"], rows, available) == expected_available
        assert render(fault["state"], rows, available) == expected_fault
    for available, gateway, expected_available, expected_state in [
        (True, {"alarm_muted": True}, "True", "True"),
        (True, {"alarm_muted": False}, "True", "False"),
        (False, {"alarm_muted": True}, "False", "True"),
        (False, None, "False", "False"),
        (True, {}, "False", "False"),
        (True, {"alarm_muted": "OFF"}, "False", "OFF"),
    ]:
        assert render(mute["availability"], [], available, gateway) == expected_available
        assert render(mute["state"], [], available, gateway) == expected_state
    for operation, value in (("mute", 1), ("unmute", 0)):
        action = "rest_command.wireless_spd_" + operation
        path = rest["rest_command"]["wireless_spd_" + operation]
        assert path["method"] == "POST" and path["timeout"] == 5
        assert render(path["url"], [], receiver_url="http://192.0.2.10") == f"http://192.0.2.10/api/v1/mute?value={value}"
        control = mute["turn_on" if value else "turn_off"][0]
        assert control["action"] == action
        assert control["data"]["receiver_url"] == "SECRET:wireless_spd_base_url"
    assert rest["recorder"]["exclude"]["entities"] == ["sensor.wireless_spd_snapshot"]

    notification = load("fault-notification.yaml")
    missing = load("unavailable-notification.yaml")
    mobile = load("mobile-notification-action.yaml")
    entities = notification["variables"]["spd_fault_entities"]
    assert set(notification) == {"alias", "mode", "variables", "triggers", "conditions", "actions"}
    assert len(entities) == len(set(entities)) == 3
    assert notification["triggers"][0]["entity_id"] == entities
    assert notification["triggers"][0]["to"] == "on" and notification["triggers"][0]["for"] == "00:00:05"
    assert notification["actions"][0]["action"] == "persistent_notification.create"
    assert missing["variables"]["spd_fault_entities"] == entities
    assert all(trigger["entity_id"] == entities for trigger in missing["triggers"] if trigger["trigger"] == "state")
    assert [trigger["to"] for trigger in missing["triggers"] if trigger["trigger"] == "state"] == ["unavailable", "unknown"]
    assert mobile[0]["action"] == "notify.mobile_app_your_phone"
    from types import SimpleNamespace
    for states, fault_expected, missing_expected in [
        (["off", "off", "off"], "False", "False"),
        (["on", "off", "off"], "True", "False"),
        (["on", "unavailable", "off"], "True", "True"),
        (["unknown", "off", "off"], "False", "True"),
        (["unavailable", "unavailable", "unavailable"], "False", "True"),
        (["off", "off"], "False", "True"),
    ]:
        records = [SimpleNamespace(state=state, name=f"SPD {i+1}") for i, state in enumerate(states)]
        context = dict(expand=lambda _: records, spd_fault_entities=entities)
        fault_condition = notification["conditions"][0]["value_template"]
        missing_condition = missing["actions"][1]["value_template"]
        assert render(fault_condition, [], **context) == fault_expected
        assert render(missing_condition, [], **context) == missing_expected
        if fault_expected == "True":
            assert "SPD 1" in render(notification["actions"][0]["data"]["message"], [], **context)

    compiler = shutil.which("c++")
    if not compiler:
        raise SystemExit("A host C++ compiler is required to check the generated resource header")
    with tempfile.TemporaryDirectory(prefix="wspd-ha-guide-") as directory:
        source = Path(directory) / "header.cpp"
        source.write_text('#define PROGMEM\n#include "ha_guide.h"\nint main(){ return HA_GUIDE_HTML[0] != \'<\' || HA_REST_YAML[0] != \'#\'; }\n')
        executable = Path(directory) / "check"
        subprocess.run([compiler, "-std=c++11", "-I", str(SERVER), str(source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print("PASS: offline HTML/header/example synchronization and no external resource loads.")
    print("PASS: 7 SPD availability, 6 mute-state, 2 POST command and 6 notification-condition cases.")
    print("These tests use YAML/Jinja host validation, not a live Home Assistant installation.")


if __name__ == "__main__":
    main()
