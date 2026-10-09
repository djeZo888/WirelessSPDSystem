# Contributing

Open an issue for hardware variants or behavior changes; keep changes focused.
Submit a pull request with the hardware affected and the checks performed.

- Never commit `config.h`, deployment identifiers, keys, Wi-Fi passwords or firmware images.
- Run the three synthetic builds and `python3 tools/check_repository.py`.
- Preserve the 20-byte packet contract or propose a coordinated protocol revision.
- Use explicit four-bit firmware revisions for behavior changes; document receiver compatibility.
- Compile checks are separate from hardware acceptance. Record contact transitions,
  reception and power checks when testing on a board.
