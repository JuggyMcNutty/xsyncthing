# xsyncthing — Syncthing for Xbox 360

## Context
- **Goal:** a native Xbox 360 homebrew title (`.xex`, retail kernel and dashboard, RGH/JTAG) that acts as a full
  Syncthing peer and works with stock Syncthing v2.x on PCs and phones.
- **Toolchain:** [xsdk](https://github.com/xenosworks/xsdk). It provides GCC 15 `powerpc-xsdk-elf`, newlib and C17/C++23,
  a CMake toolchain file (`cmake/xsdk.cmake`) and `xsdk_add_xex()`. It builds on Debian or WSL.
- **Port strategy:** a clean-room C++23 reimplementation of Syncthing's protocols, because Go can't target the Xenon CPU.
  Upstream syncthing (MPL-2.0, reference commit `992ae89`, 2026-11-23) is the spec reference. Its Web GUI
  (`gui/default`) is shipped unmodified.
- **v1 scope:**
  - LAN sync
  - Global discovery and relays
  - Send & receive folders
  - Stock Web GUI compatibility
  - A controller-driven console UI
- **Repo:** public `JuggyMcNutty/xsyncthing`, licensed **GPL-3.0** (needed because we link wolfSSL; the MPL-2.0 GUI
  assets are compatible). The README will say this is unofficial and not affiliated with the Syncthing Foundation.
- **Test setup:** an RGH/JTAG console for real checks, Xenia Canary for smoke tests. In Xenia, `getentropy` returns
  the same bytes every time, so keys made there are test-only.

## Key findings
### What xsdk provides (docs/PARITY.md)
- **Threads:** pthreads, `std::thread`, mutexes and condition variables, placement on the 6 hardware threads.
  Threads get a 1 MB stack by default, so big buffers go on the heap.
- **Network:** BSD sockets with TCP, UDP, `poll`/`select` and non-blocking mode; up to 232 sockets (raise the limit
  with `xsdk_network_limits`); `SO_BROADCAST` is defined.
  - **IPv4 only.** Local discovery uses IPv4 broadcast on UDP 21027; IPv6 multicast `[ff12::8384]` is not possible.
  - XAM refuses `SO_ERROR`, so connect errors are opaque.
- **Files:** POSIX calls and `std::filesystem` on `hdd:`, `usb:`–`usb2:` and `game:`; `pwrite`, `fsync`, `statvfs`,
  `utimes`, `ftruncate`.
  - **FATX:** names of at most 42 characters, no permissions, no symlinks, mtimes in 2-second steps.
  - **An open file can't be renamed or removed.**
  - The exFAT plugin (`plugins/exfat`) gives long names and files over 4 GB on USB, or on an HDD reformatted as exFAT.
- **System and UI:** XAM on-screen keyboard, message boxes and controllers; `xsdk_on_terminate` runs when the user
  presses Xbox Home. The title only runs in the foreground: there is no background sync.
- **Libraries already ported:** imgui-xenon (Dear ImGui 1.93 with an xsdk GPU backend), mbedtls-xenon (TLS 1.2 only),
  curl-xenon (on mbedtls).

### What a stock Syncthing peer requires
- **BEP connections need TLS 1.3**, ALPN `bep/1.0` and mutual client certificates.
  - The device ID is the SHA-256 of the peer's certificate DER, in base32 with Luhn check digits.
  - New device certificates are **Ed25519**; older peers use ECDSA or RSA.
  - Hello message, then protobuf messages from `proto/bep/bep.proto` (278 lines), with LZ4 compression above 128 bytes.
- **Relays:** XDR messages (`lib/relay/protocol/packets.go`) over TLS 1.2 with ECDHE AES-GCM/CBC, then BEP over the
  relayed stream.
- **Global discovery:** HTTPS JSON announces (with the client certificate) and queries; HTTP/1.1 is enough.
- **Local discovery:** `proto/discoproto/local.proto` with a magic number, every 30 seconds.
- **DB:** SQLite (`internal/db/sqlite`).
- **Web GUI:** AngularJS. It calls about 40 `/rest/*` endpoints (`config`, `system/*`, `db/*`, `events`, `cluster/pending/*`,
  `stats/*`, `svc/*`, `folder/*`, `noauth/auth`), with CSRF/API-key auth and `/rest/events` long-polling.

### TLS decision
**wolfSSL** (5.9.x) covers everything: TLS 1.3 and 1.2, Ed25519/ECDSA/RSA verification and Ed25519 certificate
generation. It builds on newlib with `WOLFSSL_USER_SETTINGS`.
- We build a new port, `third_party/wolfssl` with `user_settings_xsdk.h`:
  - big-endian, no assembly, `getentropy` as the RNG seed, xsdk sockets, pthreads.
- **One TLS stack for everything:** BEP, relays, discovery HTTPS and the GUI's HTTPS. So we write a small HTTP/1.1
  client and skip curl and mbedtls.
- Build this as a `wolfssl-xenon` fork on our account, following xsdk's convention for ported libraries.
  `docs/README-xsdk.md` holds the port notes.

## Approach
- **Portable core:** C++23, built on Linux x86 for unit and interop tests.
- **Platform layer:** a thin layer with `src/platform/xsdk` for the console and `src/platform/posix` for the host.
  The core never calls xsdk directly.

| Component | Implementation |
|---|---|
| TLS/crypto | wolfSSL; SHA-256 for block hashes; cert/key PEM in `hdd:\xsyncthing\` |
| Protobuf | nanopb, generated from upstream `bep.proto` and `local.proto` (vendored copies) |
| LZ4 | lz4 (BSD-2) |
| DB | SQLite amalgamation, our own VFS on xsdk files (no locking); bounded page cache for 512 MB RAM |
| Model | folder/device state, index exchange, version vectors and conflict handling, as in `lib/model` |
| Scanner | worker threads on the hardware threads; FATX-aware walk; ignore patterns (`.stignore`) |
| Puller | block requests and reuse; short temp names `.st~<hash8>.tmp` that fit in 42 characters; close before rename; 2 s modtime window |
| Connections | TCP listen/dial on 22000; relay client; connection priorities |
| Discovery | IPv4 broadcast local discovery; global announce/query to `discovery.syncthing.net` |
| REST + GUI | embedded HTTP/HTTPS server on 8384; the endpoint subset the GUI needs; GUI assets in `game:\gui\`; CSRF and API key |
| Console UI | ImGui on imgui-xenon with controller navigation, XAM keyboard for text, QR code of the device ID (qrcodegen, MIT) |
| Config | `config.xml` in a Syncthing-compatible schema, so it's familiar and can be edited from the Web GUI |

**Filesystem rules:** each volume reports its type (FATX, FAT32 or exFAT).
- Remote files that can't be stored are skipped and shown as folder errors, the same way stock Syncthing reports them:
  - names that are too long, or that contain characters the volume refuses
  - files over 4 GB on FAT32
- Permissions are always ignored. Symlinks are always unsupported.

## Repo layout
```
xsyncthing/
  README.md  LICENSE (GPL-3.0)  THIRD_PARTY_NOTICES.md  CONTRIBUTING.md
  .github/workflows/ci.yml       # host build + tests; xsdk cross-build (cached toolchain); .xex release artifacts
  CMakeLists.txt  cmake/
  third_party/   # submodules: xsdk, imgui-xenon, wolfssl(-xenon), sqlite, lz4, nanopb, qrcodegen; syncthing gui (pinned)
  proto/         # vendored bep.proto, local.proto (+ upstream commit hash)
  src/core/ src/net/ src/api/ src/ui/ src/platform/{xsdk,posix}/  src/main_xsdk.cpp src/main_host.cpp
  tests/unit/  tests/interop/ (docker-compose: stock syncthing vs host build)  tests/console/ (on-console test .xex)
  docs/        # ARCHITECTURE.md, BUILDING.md, CONSOLE_TESTING.md, COMPAT.md (what's supported vs stock)
```

## Steps
### Phase 0: Repo and toolchain
- [x] `git init`; add the skeleton above, the GPL-3.0 LICENSE and a README (unofficial, scope, status).
- [x] `gh repo create JuggyMcNutty/xsyncthing --public --source . --push`; set the description and topics
  (xbox360, syncthing, homebrew, xsdk).
- [x] Add xsdk as a submodule. Write a `scripts/bootstrap.sh` that runs xsdk's `tools/toolchain/build.sh` to install
  `~/xsdk-gcc`.
- [x] Hello-world `.xex` built from `samples/template`; it runs in Xenia and on the console.
- [x] CI: a host job, plus an xsdk job that caches the toolchain and uploads the `.xex`.

### Phase 1: TLS spike (biggest risk)
- [x] Port wolfSSL to xsdk with the settings above and run its tests (`wolfcrypt_test`) on the console.
- [x] Generate an Ed25519 cert on the console and compute the device ID.
- [x] Complete a TLS 1.3 handshake with mutual certs (ALPN `bep/1.0`) against a stock Syncthing on the LAN, then exchange Hello.

### Phase 2: BEP core (host-first)
- [x] Device ID, nanopb messages, framing, LZ4, Hello/ClusterConfig/Index/Request/Response/Ping/Close.
- [x] SQLite schema and index DB; the scanner with ignores; the model and version vectors; the puller with conflicts.
- [x] Docker interop test: a send-receive folder between the host build and stock Syncthing, both directions.

### Phase 3: Console platform layer
- [ ] fs backends for FATX, FAT32 and exFAT, with the name and size checks; an SQLite VFS; thread placement;
  `xsdk_on_terminate` to close the DB cleanly.
- [ ] A LAN sync test on the console against a PC (static address first).

### Phase 4: Discovery and relays
- [ ] Local discovery over IPv4 broadcast, sending and receiving.
- [ ] HTTP/1.1 client; global discovery announce and query.
- [ ] Relay client: pool lookup, join, session invites, BEP over a relay.

### Phase 5: REST API and Web GUI
- [ ] HTTP server, API key, CSRF, `/rest/events` long-polling.
- [ ] The endpoint subset the GUI uses, one endpoint at a time.
- [ ] Optional HTTPS with an ECDSA-P256 cert.
- [ ] Unsupported features (`system/upgrade`, `svc/report`, browse on symlinks, …) return safe stubs.
- [ ] GUI listens on the LAN; the console UI shows its address and API key/password.

### Phase 6: Console UI (ImGui)
- [ ] Screens: dashboard (folders and devices with progress), device ID with QR code, add or accept a device, add or
  share a folder (device and path picker), pending requests, folder errors, settings (GUI on/off, listen address,
  discovery and relays), logs.
- [ ] 720p, safe-area margins, A/B/X/Y and the D-pad with focus navigation, XAM keyboard for text fields.
- [ ] The UI only reads the same in-process model the REST API uses; there's no second source of truth.

### Phase 7: Hardening and release
- [ ] Memory limits and soak tests (large folders, about 100k files); throughput tuning
  (thread placement, SHA-256 speed).
- [ ] Recover after power loss (SQLite journal, temp-file cleanup).
- [ ] Write `docs/COMPAT.md`; tag v0.1.0 with the `.xex` and GUI assets as a GitHub Release.

## Verification
- **Host unit tests (CI):** protocol encode/decode against golden bytes from upstream, device ID and Luhn vectors,
  the version-vector and conflict logic, the FATX name rules.
- **Interop (CI, docker):** stock `syncthing` v2.x against the host build covering:
  - adding a device
  - sharing a folder
  - creating, modifying and deleting files, both directions
  - a conflict
  - relay-only connections, using a local `strelaysrv`
  - global discovery, using a local `stdiscosrv`
- **Console:**
  - a test `.xex` writes pass/fail lines to `game:\xsyncthing_test.log`
  - manual run: pair with a PC by QR code or device ID, sync a folder to HDD, USB FAT32 and exFAT, then open the
    Web GUI from the PC browser at `http://<xbox-ip>:8384`
- **Xenia Canary:** smoke tests per milestone (crashes and UI); real hardware decides.
