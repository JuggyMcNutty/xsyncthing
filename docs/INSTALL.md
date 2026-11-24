# xsyncthing for the Xbox 360

Syncthing for the Xbox 360: it syncs folders on the console's hard disk or USB drives with Syncthing on your PC,
phone or server. It's unofficial, and not affiliated with the Syncthing Foundation or Microsoft.

## What you need

- An Xbox 360 that runs unsigned code (RGH or JTAG) with a way to copy files to it, such as FTP through Aurora,
  FreeStyle Dash or DashLaunch.
- The console on your network. Wired is best.
- Syncthing v2.x on the devices you want to sync with.

## Install

1. Copy the `xsyncthing` folder from the zip to the console, for example to `Hdd:\Apps\xsyncthing\`. The `.xex`,
   `gui\` and `cacert.pem` must stay together.
2. Launch `xsyncthing.xex` from your dashboard.

Its settings and database are kept in `Hdd:\xsyncthing\`, so they survive reinstalling.

## Pair with a PC or phone

1. On the console, press **RB** to go to **This Device**. It shows the console's device ID and a QR code.
2. In Syncthing on the other device, choose **Add Remote Device** and scan the code or type the ID.
3. The console shows the request under **Requests**. Choose **Add device**.
4. Share a folder from the other device. The console shows the offer under **Requests**: choose **Accept...**, then
   pick where it goes (by default `hdd:/xsyncthing/folders/<name>`).

You can also add devices and folders from the console itself (**Devices**, then **Y**). The keyboard opens for
text fields.

## The Web GUI

The full Syncthing Web GUI runs from the console at the address shown on **This Device**, for example
`http://192.168.1.50:8384/`. Open it from a browser on your PC.

**Set a GUI password** in **Settings** (or in the Web GUI) before you use it on a shared network. You can switch to
HTTPS in the Web GUI's settings ("Use HTTPS for GUI"); the console makes its own certificate.

## Controls

| Button | Action |
|---|---|
| LB / RB | Previous / next screen |
| D-pad, left stick | Move the focus |
| A | Select |
| B | Back, cancel |
| X | Pause or resume (Devices), rescan (Folders) |
| Y | Add a device (Devices) |
| Hold Back for 1 second | Quit |

## Good to know

- **Sync only happens while xsyncthing is running.** The Xbox 360 doesn't run apps in the background.
- **The internal hard disk (FATX)** keeps file names of up to 42 characters and files under 4 GB. A file that doesn't
  fit is listed as a failed item. It's never deleted on your other devices. A USB drive formatted exFAT has neither
  limit.
- **Some features aren't supported:**
  - IPv6 and QUIC
  - file versioning
  - untrusted (encrypted) devices
  - automatic upgrades: to update, copy the new `xsyncthing.xex` over the old one
- **Large folders sync slower on the console** than on a PC. Each new file costs about 40 ms on the hard disk (most
  of it flushing to disk). See `docs/COMPAT.md` in the source for details.

Source, issues and updates: https://github.com/JuggyMcNutty/xsyncthing (GPL-3.0).
