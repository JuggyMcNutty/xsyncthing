# Compatibility with stock Syncthing

Filled in as features land (Phase 7). Target: interoperate with stock Syncthing v2.x.

| Feature | Status |
|---|---|
| BEP over TLS 1.3, Ed25519 device certs | planned |
| Send & receive, send only, receive only folders | planned |
| Local discovery (IPv4 broadcast) | planned |
| Global discovery | planned |
| Relays | planned |
| Web GUI | planned |
| Storage: internal HDD (FATX) | verified on hardware: 42-byte names, files < 4 GB, 2 s mtimes, no permissions |
| Storage: USB FAT32 | verified on hardware: 255-byte names, files < 4 GB, 2 s mtimes |
| Storage: exFAT (xsdk plugin) | rules implemented; not yet verified on hardware |
| Untrusted (encrypted) devices | not planned for v1 |
| Versioning | not planned for v1 |
| IPv6, QUIC, UPnP/NAT-PMP | not planned for v1 |
