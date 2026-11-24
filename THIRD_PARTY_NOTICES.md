# Third-party notices

xsyncthing is GPL-3.0-or-later. It builds on, or ships, the components below under their own licences. This file
grows as components are added.

| Component | Use | Licence |
|---|---|---|
| [Syncthing](https://github.com/syncthing/syncthing) | Protocol definitions (`proto/`); behaviour reference; the Web GUI (`assets/gui`, unmodified, with its own vendored libraries: AngularJS, Bootstrap, jQuery, moment, fancytree, Fork Awesome, daterangepicker — see their headers) | MPL-2.0 (Syncthing); MIT / SIL OFL (vendored libraries) |
| [xsdk](https://github.com/xenosworks/xsdk) | Toolchain, runtime and console library (`third_party/xsdk`) | MIT (some files BSD-3-Clause; see its THIRD_PARTY_NOTICES.md) |
| [wolfSSL](https://github.com/wolfSSL/wolfssl) v5.9.4 | TLS 1.3/1.2 and crypto (`third_party/wolfssl`, settings in `third_party/wolfssl-config`) | GPL-3.0 |
| [LZ4](https://github.com/lz4/lz4) v1.11.1 | BEP message compression (`third_party/lz4/lib`) | BSD-2-Clause |
| [SQLite](https://sqlite.org) 3.54.1 | Index database (`third_party/sqlite`) | Public domain |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.93 via [imgui-xenon](https://github.com/xenosworks/imgui-xenon) | Console UI (`third_party/imgui`) | MIT |
| [QR Code generator](https://github.com/nayuki/QR-Code-generator) v1.8.0 | Device ID QR code (`third_party/qrcodegen`) | MIT |
| Mozilla CA bundle (curl's `cacert.pem`) | Roots for discovery and relay pool HTTPS (`assets/cacert.pem`) | MPL-2.0 |
| [nlohmann/json](https://github.com/nlohmann/json) v3.12.0 | Config and REST JSON (`third_party/include/nlohmann`) | MIT |


The Syncthing Web GUI assets are distributed under MPL-2.0. Their source is the upstream repository at the pinned
commit. MPL-2.0 is compatible with distribution alongside GPL-3.0 code (MPL-2.0, section 3.3).
