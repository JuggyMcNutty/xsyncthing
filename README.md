# xsyncthing

**Syncthing for the Xbox 360.** xsyncthing is a native homebrew title (`.xex`) for RGH/JTAG consoles that acts as a
[Syncthing](https://syncthing.net) peer. It syncs folders on the console's hard disk or USB drives with stock
Syncthing running on PCs, phones and servers.

> **Unofficial.** xsyncthing is an independent clean-room reimplementation of the Syncthing protocols. It is not
> affiliated with or endorsed by the Syncthing Foundation. It is not affiliated with or endorsed by Microsoft either.
> Xbox and Xbox 360 are trademarks of Microsoft.

## Status

**Pre-alpha. Nothing syncs yet.** See [PLAN.md](PLAN.md) for the roadmap and its progress.

| Area | State |
|---|---|
| Repo, toolchain bootstrap, CI | in progress |
| TLS 1.3 + Ed25519 (wolfSSL port) | planned |
| BEP sync core | planned |
| Local / global discovery, relays | planned |
| REST API + stock Web GUI | planned |
| Console UI (controller) | planned |

## Goals for v1

- Send & receive folders with stock Syncthing v2.x peers
- LAN sync, global discovery and relays
- A controller-driven console UI at 720p: dashboard, device ID with QR code, adding devices and folders
- The stock Syncthing Web GUI, served from the console at `http://<xbox-ip>:8384`, for anyone who prefers it
- Storage on the internal HDD (FATX), USB drives (FAT32) and exFAT volumes (through xsdk's exFAT plugin)

## How it works

Syncthing is written in Go, and Go can't target the Xbox 360's Xenon CPU (PowerPC, big-endian, no POSIX OS). So
xsyncthing reimplements the protocols in C++23 instead:

- **Block Exchange Protocol** over TLS 1.3
- **Local discovery** over IPv4 broadcast
- **Global discovery** over HTTPS
- **Relay protocol**

It's built with [xsdk](https://github.com/xenosworks/xsdk), an open-source homebrew SDK that targets the retail kernel.
The upstream Syncthing source is the reference for behaviour. The protocol definitions in `proto/` are vendored from it.

The core is portable and also builds on Linux. That host build runs the unit tests and the interop tests against stock
Syncthing in CI.

## Console limitations

- **Foreground only.** The title has to be running to sync. Going to the dashboard stops syncing.
- **IPv4 only.** The console's network stack has no IPv6.
- **FATX** keeps names of at most 42 characters, no permissions and no symlinks, and stores mtimes in 2-second steps.
  Files whose names don't fit are reported as folder errors rather than synced. Use exFAT to avoid this.
- **FAT32** can't hold files over 4 GB.

## Building

See [docs/BUILDING.md](docs/BUILDING.md). In short, on Linux:

```sh
git clone --recursive https://github.com/JuggyMcNutty/xsyncthing
cd xsyncthing
scripts/bootstrap.sh        # builds the xsdk GCC toolchain into ~/xsdk-gcc (takes a while)
scripts/build-xex.sh        # builds build-xsdk/xsyncthing.xex
scripts/build-host.sh       # builds and tests the portable core on the host
```

## Layout

```
src/core/      protocol, index database, model, scanner, puller
src/net/       TLS, connections, discovery, relays
src/api/       REST API and Web GUI server
src/ui/        console UI
src/platform/  xsdk (console) and posix (host) backends
proto/         protocol definitions vendored from upstream Syncthing
tests/         unit, interop and on-console tests
third_party/   submodules: xsdk and libraries
docs/          architecture, building, testing, compatibility
```

## Licence

GPL-3.0-or-later (see [LICENSE](LICENSE)). Third-party components keep their own licences, listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
