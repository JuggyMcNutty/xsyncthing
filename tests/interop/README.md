# Interop tests

`run.py` runs the host build of xsyncthing against stock Syncthing on loopback (discovery and relays off) and checks:

- new files and directories, in both directions
- modifications, in both directions
- deletions, in both directions
- that a concurrent edit settles on one winner, with a `.sync-conflict-` copy
- that stock Syncthing reports the folder 100% complete on xsyncthing's side

`--fatx` runs xsyncthing with the console's FATX rules: 42-character names, no permissions, and 2-second mtimes.

```sh
scripts/build-host.sh
ST=$(tests/interop/get-syncthing.sh)          # stock Syncthing v2.1.7, checksum-verified
tests/interop/run.py --syncthing "$ST" --xsyncthing build-host/xsyncthing
tests/interop/run.py --syncthing "$ST" --xsyncthing build-host/xsyncthing --fatx --work /tmp/xst-interop-fatx
```

CI runs both on every push (`.github/workflows/ci.yml`, job `interop`). These are plain processes rather than Docker
containers. CI and the console tests have no container runtime, and loopback ports keep the setup to a single script.
