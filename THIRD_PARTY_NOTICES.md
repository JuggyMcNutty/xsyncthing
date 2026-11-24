# Third-party notices

xsyncthing is GPL-3.0-or-later. It builds on, or ships, the components below under their own licences. This file
grows as components are added.

| Component | Use | Licence |
|---|---|---|
| [Syncthing](https://github.com/syncthing/syncthing) | Protocol definitions (`proto/`); behaviour reference; Web GUI assets (shipped unmodified) | MPL-2.0 |
| [xsdk](https://github.com/xenosworks/xsdk) | Toolchain, runtime and console library (`third_party/xsdk`) | MIT (some files BSD-3-Clause; see its THIRD_PARTY_NOTICES.md) |
| [wolfSSL](https://github.com/wolfSSL/wolfssl) v5.9.4 | TLS 1.3/1.2 and crypto (`third_party/wolfssl`, settings in `third_party/wolfssl-config`) | GPL-3.0 |
| [LZ4](https://github.com/lz4/lz4) v1.11.1 | BEP message compression (`third_party/lz4/lib`) | BSD-2-Clause |

Planned:

| Component | Use | Licence |
|---|---|---|
| Dear ImGui / imgui-xenon | Console UI | MIT |
| SQLite | Index database | Public domain |
| QR Code generator (Nayuki) | Device ID QR code | MIT |

The Syncthing Web GUI assets are distributed under MPL-2.0. Their source is the upstream repository at the pinned
commit. MPL-2.0 is compatible with distribution alongside GPL-3.0 code (MPL-2.0, section 3.3).
