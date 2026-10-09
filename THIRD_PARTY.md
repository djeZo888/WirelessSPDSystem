# Third-party dependencies

| Dependency | Pin | Upstream terms/source |
| --- | --- | --- |
| DxCore | 1.6.2 | [Repository and licensing](https://github.com/SpenceKonde/DxCore/tree/1.6.2) |
| Arduino ESP32 core | 2.0.14 | [Repository and notices](https://github.com/espressif/arduino-esp32/tree/2.0.14) |
| RadioLib | 7.8.1 | [MIT license](https://github.com/jgromes/RadioLib/blob/7.8.1/LICENSE) |
| GFX Library for Arduino | 1.4.6 | [Repository and licenses](https://github.com/moononournation/Arduino_GFX/tree/v1.4.6) |
| usha256 | commit `1c62a3f` | [Exact provenance and checksums](firmware/beacons/third-party/usha256.md) |

The root MIT license covers this project's code and documentation, not third-party
dependencies. `usha256` has no confirmed upstream license grant for its modified
implementation. The setup tool fetches the exact source into an ignored local
directory; review its terms before redistributing a product that includes it.
No dependency source or compiled firmware is included in this repository.
