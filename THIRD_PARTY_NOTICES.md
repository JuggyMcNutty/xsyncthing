# Third-party notices

xsyncthing is GPL-3.0-or-later. It builds on, or ships, the components below under their own licences. This file
grows as components are added.

| Component | Use | Licence |
|---|---|---|
| [Syncthing](https://github.com/syncthing/syncthing) | Protocol definitions (`proto/`); behaviour reference; Web GUI assets (shipped unmodified) | MPL-2.0 |
| [xsdk](https://github.com/xenosworks/xsdk) | Toolchain, runtime and console library (`third_party/xsdk`) | MIT (some files BSD-3-Clause; see its THIRD_PARTY_NOTICES.md) |

Planned:

| Component | Use | Licence |
|---|---|---|
| wolfSSL | TLS 1.3/1.2 and crypto | GPL-3.0 |
| Dear ImGui / imgui-xenon | Console UI | MIT |
| SQLite | Index database | Public domain |
| LZ4 | BEP message compression | BSD-2-Clause |
| nanopb | Protobuf encoding | Zlib |
| QR Code generator (Nayuki) | Device ID QR code | MIT |

The Syncthing Web GUI assets are distributed under MPL-2.0. Their source is the upstream repository at the pinned
commit. MPL-2.0 is compatible with distribution alongside GPL-3.0 code (MPL-2.0, section 3.3).
