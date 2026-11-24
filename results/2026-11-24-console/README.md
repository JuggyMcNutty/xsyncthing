# Console run, 2026-11-24

**Console:** a JTAG Jasper with kernel 2.0.17559.0 and XBDM 2.0.21076.11, reached over XBDM at 192.168.1.118
using `tools/xbdm.py`.

**PC peer:** stock Syncthing v2.1.7 (linux-amd64) at 192.168.1.125:22000.

| Title | Result |
|---|---|
| `xsyncthing.xex` (hello, 720p) | 5/5: GPU, present, text and frames until autoexit; address 192.168.1.118 |
| `xst-console-test.xex` | 12/12, broken down below |

What `xst-console-test.xex` covered:
- wolfCrypt's suite passes, taking 46 s.
- An Ed25519 identity is generated in 1 ms, written and read back, and the RNG is live.
- TLS 1.3 completes in 10 ms with `TLS_AES_128_GCM_SHA256`, ALPN `bep/1.0`, and the peer's device ID checks out.
- Hello is exchanged.

Syncthing's side of it: `New device connection (device=A2UYUF2 address=192.168.1.118:1066 remote.name=xbox360
remote.client=xsyncthing remote.version=v0.0.1)`.

**Earlier attempt:** the console hung once, in an earlier run. `tools/xbdm.py` had sent `magicboot` while the console
was still booting (`lhelper.xex` was running). The script now waits for a settled dashboard first.
