# Interop tests

| Script | What it checks |
|---|---|
| `run.py` | Sync with stock Syncthing v2.1.7 on loopback, summarized below |
| `api_check.py` | The REST API as the stock Web GUI uses it, summarized below: 42 checks |
| `api_compare.py` | Response shape for every endpoint the GUI uses, against stock Syncthing sharing the same folder (38 endpoints, every key and JSON type) |
| `gui_check.py` | The stock Web GUI in headless Chromium (Playwright), summarized below |

`run.py` checks:
- new files and directories, modifications and deletions, in both directions
- that a concurrent edit settles on one winner, with a `.sync-conflict-` copy
- 100% completion, and a stop within 15 s while connected

Variants: `--fatx` applies the console's volume rules. `--relay <strelaysrv>` connects only through a local relay;
add `--relay-inbound` to have stock Syncthing dial instead.

`api_check.py` checks:
- the CSRF cookie and header
- the API key
- the GUI's startup calls
- a config round trip
- `/rest/events` long-polling
- user/password sessions (passwords stored as bcrypt)

`gui_check.py` checks:
- the page renders the device and folder panels
- there are no JavaScript errors and no failed `/rest` calls
- the GUI's own Add Device dialog saves to `config.json`

```sh
scripts/build-host.sh
ST=$(tests/interop/get-syncthing.sh)               # stock Syncthing v2.1.7, checksum-verified
RELAY=$(tests/interop/get-syncthing.sh --relay)    # stock strelaysrv v2.1.7
tests/interop/run.py --syncthing "$ST" --xsyncthing build-host/xsyncthing
tests/interop/run.py --syncthing "$ST" --xsyncthing build-host/xsyncthing --fatx --work /tmp/xst-interop-fatx
tests/interop/run.py --syncthing "$ST" --xsyncthing build-host/xsyncthing --relay "$RELAY" --work /tmp/xst-interop-relay
tests/interop/api_check.py --xsyncthing build-host/xsyncthing
pip install playwright && playwright install chromium   # once
tests/interop/gui_check.py --xsyncthing build-host/xsyncthing --screenshot /tmp/gui.png
```

These run as plain processes rather than Docker containers. CI and the console tests have no container runtime, and
loopback ports keep the setup to a single script. CI runs all of them on every push.
