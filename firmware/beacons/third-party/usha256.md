# usha256 source provenance

The firmware depends on these upstream files (not redistributed here):

- Repository: https://github.com/hypoactiv/usha256
- Commit: `1c62a3f6e7c66b16d84db5940c4e01e5cc0a6d41`
- Commit date: 2017-08-28 UTC
- Retrieval date: 2026-10-09
- Upstream release version: none; the repository has no tagged release.

The dependency installer creates Arduino packaging metadata locally. Version
`0.0.0` is a local identifier and does not claim an upstream release.

## SHA-256 file checksums

| File | SHA-256 |
| --- | --- |
| usha256.h | 2a7162a9d3deb2037777612c092ad706cf0667dfb70ba043ad830a43bdac8988 |
| usha256.cpp | 0488742624f793811f04c76af41f0c331e50758a0a0b4e44dd9f586b90200c71 |
| README.md | 71e213acbd049dc7e2e591bf5481695aed5a47f167ec376c97e607e995a553c2 |

## License provenance

The upstream repository does not contain a LICENSE file or an explicit license
grant for this modified implementation. The upstream README describes it as a
heavily modified version of Brad Conte's public-domain implementation at
https://github.com/B-Con/crypto-algorithms. This statement is retained in its
original README; it is not treated as a confirmed license grant for these files.

## Validation

The original setup verified the exact source against Python hashlib, including
known SHA-256 vectors and beacon packets. Firmware builds use the pinned unchanged upstream
source files fetched into a local ignored dependency directory. This does not qualify RF behavior or the on-air authentication
scheme. Keep this dependency's licensing status separate from project code.

## Required Arduino wrapper metadata

The installer generates a local `library.properties` with name `usha256`, local
version `0.0.0`, architectures `avr,megaavr` and includes `usha256.h`. The sketches
use `Sha256Context` and upstream `sha256_init`, `sha256_update`, `sha256_final`.
No cryptographic implementation is copied into this public source tree.
