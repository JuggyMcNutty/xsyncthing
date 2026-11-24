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
