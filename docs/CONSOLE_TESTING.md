# Testing on the console

The console is the final judge; Xenia Canary is only for quick crash and smoke checks. Xenia has no display
controller, so `xsdk_present_open` fails there.

## Running a build

1. Build `build-xsdk/xsyncthing.xex` (docs/BUILDING.md).
2. Copy it to the console (for example over FTP with Aurora or DashLaunch) and launch it.
3. Read `xsyncthing.log`, which is written beside the `.xex`:
   - each check writes `PASS: …` or `FAIL: …`
   - the last line is `checks: <n> passed, <m> failed`
   - a log that stops early means a hang or a crash

Put an empty `net_log.txt` beside the `.xex` to stream the log to the PC while the title runs. Run
`third_party/xsdk/tools/net_log.py` on the PC to receive it.

## Recording results

For each run, note the console revision, the dashboard and kernel version, the build's commit and the log. Attach
them to the PR or issue.

## xst-console-test.xex (Phases 0–1)

`build-xsdk/xst-console-test.xex` runs the TLS port's checks on the console and logs them to
`xst-console-test.log` beside it:

1. **wolfCrypt's own test suite:** AES, SHA-2, RSA, ECC, Curve25519 and Ed25519, among others.
2. **An Ed25519 identity** made from the console's RNG:
   - it's written to `hdd:\xsyncthing-test\cert.pem` and `key.pem`, then read back
   - a second identity must differ from the first, which shows the RNG is live (this fails in Xenia)
3. **Optionally, a handshake with a stock Syncthing on the LAN.**
   - Put `peer.txt` beside the `.xex` containing `<ip> 22000 <that Syncthing's device ID>`.
   - Run the test once and copy the device ID it logs.
   - Add that ID as a device in the PC's Syncthing, then run the test again.
   - It expects TLS 1.3, ALPN `bep/1.0`, the right peer device ID and a Hello exchange.
