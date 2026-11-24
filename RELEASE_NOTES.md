# xsyncthing v0.1.0 (alpha)

The first release of xsyncthing: Syncthing for the Xbox 360, as a native `.xex` for RGH/JTAG consoles. It syncs
with stock Syncthing v2.x on PCs, phones and servers.

**Install:** unzip `xsyncthing-v0.1.0.zip`, copy the `xsyncthing` folder to the console, and launch
`xsyncthing.xex`. The zip's `README.md` has the steps, pairing and the controls.

## What works

Everything below was verified on a JTAG Jasper (kernel 17559) against stock Syncthing v2.1.7, and is covered in CI
(`tests/interop`).

- **Syncing:** send & receive folders, conflict copies, `.stignore`, LZ4, TLS 1.3 with Ed25519 device certificates
- **Finding devices:** local discovery (IPv4), global discovery, relays (joining one, and dialing through others)
- **Storage:** the internal hard disk (FATX) and FAT32 USB drives. Names a volume can't hold are reported, never
  deleted elsewhere.
- **The stock Syncthing Web GUI,** served from the console over HTTP or HTTPS
- **A controller UI on the TV:** overview, a QR code for pairing, devices, folders, requests, settings, log
- **Recovery after crashes or power loss:** the database, temp files and partially pulled folders recover

## Known limitations

- Sync runs only while the title is running: the console has no background apps.
- No IPv6, QUIC, file versioning or untrusted (encrypted) devices. They're turned off with a notice if configured.
- exFAT through xsdk's plugin is implemented, but not yet verified on hardware.
- **Speed on the console's hard disk:** each new file costs about 40 ms, mostly flushing it to disk. Large trees of
  small files sync slowly; large files sync at disk speed.

See `docs/COMPAT.md` for the full compatibility table, and `results/` for the hardware logs.
