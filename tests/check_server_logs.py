#!/usr/bin/env python3
"""Exercise session ring retention, reconstructed dates and JSON formatting."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'firmware/servers/tconnectpro_868'
DRIVER = r'''
#include "session_log.h"
#include "json_format.h"
#include <cassert>
#include <string>
#include <vector>
#include <iostream>
int main(int argc, char **argv) {
  if (argc > 1) {
    std::string in, out; std::getline(std::cin, in);
    wspdPrettyJson(in.c_str(), in.size(), [&](char c) { out += c; });
    std::cout << out; return 0;
  }
  WspdSessionLog ring;
  ring.append(0, "Before allocation"); assert(ring.capacity() == 0);
  std::vector<WspdSessionLog::Record> records(4096);
  assert(records.size() * sizeof(records[0]) == 2 * 1024 * 1024);
  ring.begin(records.data(), records.size());
  assert(ring.oldest() == 1 && ring.next() == 1 && !ring.get(1));
  for (uint64_t i = 1; i <= 17000; ++i) {
    ring.append(i * 1000ULL, "Packet");
    assert(ring.used() == (i > 4096 ? 4096 : i));
    assert(ring.next() == i + 1);
    assert(ring.oldest() == (i > 4096 ? i - 4095 : 1));
    assert(ring.dropped() == (i > 4096 ? i - 4096 : 0));
    auto entry = ring.get(i);
    assert(entry && entry->seq == i && entry->bootMs == i * 1000ULL);
    assert(!ring.get(ring.oldest() - 1) && !ring.get(ring.next()));
    assert(ring.get(ring.oldest())->seq == ring.oldest());
  }
  // No wall date is invented before synchronization. A64-bit boot clock does
  // not wrap after49 days, and the first NTP anchor backfills startup entries.
  assert(ring.unixMs(100) == 0);
  ring.syncClock(9999, 100); assert(!ring.clockSynced());
  ring.syncClock(50000, 1800000000000ULL);
  assert(ring.clockSynced() && ring.unixMs(1000) == 1799999951000ULL);
  assert(ring.unixMs((1ULL << 32) + 1) == 1799999950000ULL + (1ULL << 32) + 1);
  std::string huge(2000, 'X'); ring.append(1ULL << 33, huge.c_str());
  assert(strlen(ring.get(17001)->message) == 495);
  ring.append(1, "Line\n\tcontrol\001 DEL\177");
  assert(std::string(ring.get(17002)->message) == "Line  control  DEL ");
  std::string boundary(494, 'A'); boundary += "\xC5\xBD";
  ring.append(2, boundary.c_str()); assert(strlen(ring.get(17003)->message) == 494);
  std::string broken(79, 'A'); broken += "\xC5' name";
  ring.append(3, broken.c_str()); assert(std::string(ring.get(17004)->message) == std::string(79, 'A') + "?' name");
  ring.append(4, "\xF0\x9F\x98\x80\xED\xA0\x80");
  assert(std::string(ring.get(17005)->message) == "\xF0\x9F\x98\x80???");
  ring.begin(records.data(), 16); assert(ring.next() == 1 && !ring.clockSynced() && ring.dropped() == 0);
  ring.append(0, nullptr); assert(std::string(ring.get(1)->message).empty());
  uint64_t v;
  assert(wspdLogNumber("18446744073709551615", v) && v == UINT64_MAX);
  for (const char *bad : {"", "-1", "+2", "1.5", " 1", "1 ", "0x10", "18446744073709551616"}) assert(!wspdLogNumber(bad, v));
  assert(!wspdLogNumber(nullptr, v));
  std::cout << "PASS:17000 ring writes,4096-record/2MiB retention, rollover, overflow, timestamp reconstruction and bounded messages.\n";
}
'''

def main():
    compiler = shutil.which('c++')
    if not compiler:
        raise SystemExit('A host C++ compiler is required.')
    with tempfile.TemporaryDirectory(prefix='wspd-logs-') as temp:
        cpp, binary = Path(temp)/'logs.cpp', Path(temp)/'logs'
        cpp.write_text(DRIVER)
        subprocess.run([compiler, '-std=c++11', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I', str(SERVER), str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        for value in ({}, [], {'quoted': '}{[]\\\"', 'controls': '\n\t', 'utf8': 'Ž'}, {'nested':[{},[],{'a':1,'b':True,'c':None}]}):
            source = json.dumps(value, ensure_ascii=False, separators=(',', ':'))
            output = subprocess.run([str(binary), 'pretty'], input=source+'\n', capture_output=True, text=True, check=True).stdout
            assert json.loads(output) == value
            assert output.endswith('\n')
    ino = (SERVER/'tconnectpro_868.ino').read_text()
    assert 'sessionLogLine(text);' in ino[ino.index('static void bootLogLine(uint16_t color, const char *text) {'):]
    for text in ('[LoRa] Received', 'Reject auth/tag failure', 'Reject old nonce', 'raw=%s', 'RSSI=%.1f', 'SNR=%.1f', '[Alarm]', 'System start:', 'network time synchronized'):
        assert text in ino
    assert 'MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT' in ino
    print('PASS: generated JSON formatting preserves escaped strings/values; production boot, packet, rejection and state logging hooks.')
    print('Host checks do not qualify real PSRAM allocation, NTP or HTTP transport.')

if __name__ == '__main__':
    main()
