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

## Phase 3: storage and the daemon (same day)

| Run | Result |
|---|---|
| `xst-console-test.xex` | 21/21, broken down below |
| `xsyncthing.xex`, the daemon | 7/7, broken down below |

What `xst-console-test.xex` covered:
- SQLite on `hdd:` through `src/platform/xsdk/sqlite_vfs.c`: a record written, read back and reopened
- The volume type from the kernel's `FileFsAttributeInformation`:
  - `hdd:` reports "FATX", with names of at most 42 bytes
  - a USB stick on `usb0:` reports "FAT32", with names of at most 255 bytes
- A scan of 300 KiB on `hdd:` in 129 ms; a rescan finds no changes, thanks to the 2 s mtime window
- A file is replaced by rename on `hdd:`
- On both FATX and FAT32: a file with the longest name each takes is written, renamed and removed

What the daemon run covered:
- It starts: identity, `config.json` and `index.db` on `hdd:/xsyncthing`, listening on 22000
- It shows its status screen
- It stops in 100 ms

**Before the fix:** the first daemon run never ended. A blocked `accept()` isn't woken by `shutdown()` under XAM's
Winsock, so stopping waited for it. `xbdm.py` then rebooted the console under the running title, and the console hung.
Fixed by:
- bounded accept and connect waits
- a `stop.txt` request instead of rebooting under a running title
- a 15 s watchdog on stopping

**Not covered on hardware:** exFAT. No exFAT drive was on hand. Its rules (255-byte names, no 4 GB limit) are unit-tested on the host.

## Phase 3: LAN sync between the console and a PC (step 13)

| Side | Setup |
|---|---|
| Console | `xsyncthing.xex`, the daemon, folder `lan` at `hdd:/xsyncthing/folders/lan` (FATX), static address of the PC |
| PC | stock Syncthing v2.1.7 at 192.168.1.125:22301 (`tests/console/lan_setup.py`) |

What passed:
- The PC's files reach the console: `docs/random.bin` (3 MB) is byte-identical, and `hello.txt` arrives.
- A file written on the console reaches the PC.
- Deletions go both ways.
- An edit on the PC reaches the console.
- The PC reports 100% completion for the console.
- On a clean stop, the PC sees the console disconnect.

`tests/console/lan_check.py` checked the run: 6/6.

**Bugs this run found, now fixed:**
1. **A name too long for FATX was deleted on the PC.** It was recorded as unsupported. The next scan found it missing
   on disk and announced a deletion. Fixed: unsupported and ignored entries are skipped by the deletion pass. Now
   covered by a unit regression test and an interop check with `--fatx`.
2. **Recording an entry as unsupported wasn't announced to peers.** So the PC never showed 100%. Fixed.
3. **Stopping waited forever on a connection mid-handshake, and the 15 s watchdog ended the title.** Now every live
   stream is interrupted on stop: the daemon stops in 100 ms while connected.
4. **`config.json` was kept open while rewriting it.** FATX refuses to replace an open file. Fixed.

## Phase 4: local discovery (step 14)

The console and the PC both have each other only as `dynamic`, and global discovery and relays are off. So the only
way they can find each other is the IPv4 broadcast on UDP 21027:
- The console logged `discovery: 4FX6QBR at tcp://192.168.1.125:22301` and connected.
- The PC's `/rest/system/discovery` lists the console at `tcp://192.168.1.118:22000`.
- The packet format matches upstream's byte for byte (unit test against `tools/golden`).

## Phase 4: global discovery (step 15)

The console ran against Syncthing's public discovery servers over HTTPS/1.1. Our wolfSSL TLS checked their
certificates against `cacert.pem`, which is copied beside the `.xex`.

- **Announce:** the console announced with its device certificate. Looking it up with `xst-lookup` then returned
  `tcp://152.117.116.12:22000` (the public address the server saw) and `tcp://192.168.1.118:22000` (the LAN address
  we sent).
- **Lookup:** with local discovery turned off on the console, and the PC configured only as `dynamic`, the console
  still dialled the PC at `192.168.1.125:22301`. It learned that address from `discovery-lookup.syncthing.net`.

## Phase 4: relays (step 16)

- **Setup:** the console had relays on and no address for the PC. The PC listened on loopback only, so neither side
  could dial the other directly.
- **Joining:** the console fetched `https://relays.syncthing.net/endpoint`, joined `relay://195.72.60.105:22067`
  (a public relay), and announced it through global discovery.
- **Connecting:** the PC's stock Syncthing reached the console through that relay. It saw a `relay-client`
  connection; the console logged `relay-server` after accepting the session invitation.
- **Syncing:** a file written on the PC (`relay.txt`) arrived on the console over the relayed connection.

On the host, `tests/interop/run.py --relay <strelaysrv>` runs the full interop suite through a local strelaysrv v2.1.7
in both directions:
- xsyncthing dials: 14/14
- stock Syncthing dials, xsyncthing accepts the invitation: 12/12
