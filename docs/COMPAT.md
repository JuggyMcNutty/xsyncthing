# Compatibility with stock Syncthing

xsyncthing aims to work with stock Syncthing v2.x. It's tested against v2.1.7 (`tests/interop`) and on a JTAG Jasper
(`results/`).

## Sync

| Feature | Status |
|---|---|
| BEP over TLS 1.3, Ed25519 device certificates (ECDSA and RSA peers accepted) | supported; verified on the console |
| Send & receive, send only, receive only folders | supported (send & receive verified end to end) |
| LZ4 message compression | supported |
| Conflict copies (`.sync-conflict-<date>-<time>-<device>`) | supported, `maxConflicts` honoured |
| `.stignore` (with `!`, `(?i)`, `(?d)`, `#include`, `**`, classes, alternatives) | supported |
| Local discovery | IPv4 broadcast on UDP 21027; no IPv6 multicast (the console has no IPv6) |
| Global discovery | supported (HTTPS announce and lookup; IPv4 servers) |
| Relays | supported (joining one, and dialing through peers' relays) |
| QUIC, UPnP/NAT-PMP, IPv6 | not supported |
| Untrusted (encrypted) devices, receive-encrypted folders | not supported: turned off in the config with a notice |
| File versioning | not supported: turned off in the config with a notice |
| Symlinks | not supported: shown as folder errors |
| Introducers | the flag is kept and sent; introductions aren't acted on yet |

## Storage on the console

| Volume | Names | Files | Mtimes | Permissions | Verified |
|---|---|---|---|---|---|
| Internal hard disk (FATX) | 42 bytes | < 4 GB | 2 s steps | none | yes |
| USB, FAT32 | 255 bytes | < 4 GB | 2 s steps | none | yes |
| exFAT (xsdk plugin) | 255 bytes | no limit | 10 ms | none | not yet |

A peer's file that a volume can't store keeps its place on the peers. Examples are a name that's too long, a
character the volume refuses, or a FAT32 file over 4 GB. On the console it's announced as invalid, so it's never
deleted elsewhere. It's listed as a folder error, and the folder still reports 100%.

## REST API and Web GUI

The stock Web GUI is served unmodified. The endpoints it uses answer with the same JSON shape as v2.1.7
(`tests/interop/api_compare.py` compares 38 endpoints key by key and type by type).

| Endpoint | Behaviour on xsyncthing |
|---|---|
| `GET /rest/system/upgrade` | reports the running version as the latest; nothing is fetched |
| `POST /rest/system/upgrade` | `{"ok": "up to date"}`: new versions are new `.xex` files, installed by hand |
| `GET /rest/svc/report` | shows what a usage report would hold. Reports are never sent, crash reports neither |
| `/rest/folder/versions` | `500 folder has no versioner`, as upstream answers for a folder without versioning |
| `/rest/system/browse` | lists drives (`hdd:/`, `usb0:/`…) and directories; no symlinks to follow |
| `/rest/system/paths` | no audit, panic or lock files (they don't exist here) |
| `/rest/system/loglevels` | one level for everything |
| `/rest/system/log` | the last 500 lines of the title's log |
| `/rest/debug/*`, `/metrics` | not available (404) |
| `/rest/db/need` | everything needed is listed under `rest`; `progress` and `queued` stay empty |
| LDAP authentication | not supported; user and password are stored as bcrypt, as upstream |

## Performance

Measured with `tools/soak.cpp`, 1 KiB files, about 100 to a directory:

| Where | Scan | Pull | Memory |
|---|---|---|---|
| Host (Linux, NVMe), 100,000 files | 3.5 s (29k files/s) | 9 s (11k files/s) | 24 MiB |
| Console hard disk (FATX), 2,000 files | 1,000 files/s | 22 files/s (fsync on); 250 files/s (fsync off) | 3 MiB |
| Console hard disk (FATX), 100,000 files | 273 files/s (6.2 min) | 19 files/s (fsync off) | 17 MiB |

**Power loss:** the 100,000-file pull on the console was cut off by a manual reboot partway through. After restarting,
it finished the remaining 29,405 files. All 100,000 files then re-hashed identical to the source, with no conflict
copies and no leftover temp files (`results/2026-11-24-console`).

On the console, a pulled file's cost is dominated by `fsync`: about 37 ms per file on the stock hard disk. Like
upstream, xsyncthing syncs each file to disk before renaming it into place, so a power cut can't leave a half-written
file under its real name. Setting a folder's `disableFsync` (Web GUI: folder > Advanced) trades that safety for about
10× more files a second.
