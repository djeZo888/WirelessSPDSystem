# Contributing

Open an issue for hardware variants or behavior changes; keep changes focused.
Submit a pull request with the hardware affected and the checks performed.

- Never commit `config.h`, deployment identifiers, keys, Wi-Fi passwords or firmware images.
- Run the three synthetic builds and `python3 tools/check_repository.py`.
- Preserve the 20-byte packet contract or propose a coordinated protocol revision.
- Use explicit four-bit firmware revisions for behavior changes; document receiver compatibility.
- Compile checks are separate from hardware acceptance. Record contact transitions,
  reception and power checks when testing on a board.
- Add or update regression cases with each behavior change, including rejection
  and recovery paths; remove obsolete cases when a feature is removed. A bug fix
  needs a test that fails with the old behavior. Receiver changes must run the
  [scenario suite](tests/check_server_scenarios.py) and relevant focused suites
  listed in the [test plan](docs/test-plan.md). Keep the production path
  under test: replacing logging or control logic with no-op shims can hide stalls.
