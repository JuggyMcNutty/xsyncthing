# Contributing

Thanks for your interest! The project is early, so please open an issue before starting large work.

## Ground rules

- **Clean room.** Implement from the protocol specs (https://docs.syncthing.net/specs/) and the upstream behaviour.
  Don't copy Go code verbatim. Vendored upstream files (for example `proto/`) are marked as such.
- **No Microsoft material.** No XDK code, leaked SDK headers, keys, firmware or binaries. Follow xsdk's
  CODE_GUIDELINES.md.
- **Portable core.** Code in `src/core`, `src/net` and `src/api` must build on the Linux host. Console-specific
  code goes in `src/platform/xsdk`.
- **Big-endian.** The console is big-endian. Never assume the host's byte order on the wire or on disk.
- **Tests.** Add host unit tests for new logic. Note console results (Xenia or real hardware) in the PR.

## Commits

Use short imperative subjects with a component prefix, for example `bep: parse Hello` or `ui: device QR screen`.
By submitting a contribution you agree to license it under GPL-3.0-or-later.
