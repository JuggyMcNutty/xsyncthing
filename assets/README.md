# Assets shipped beside the .xex

| File | What | Source |
|---|---|---|
| `cacert.pem` | Mozilla's CA bundle, used to check the certificates of global discovery and relay pool servers | https://curl.se/ca/cacert.pem, data as of 2026-11-05, SHA-256 `427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285` (MPL-2.0) |

On the console these files go beside `xsyncthing.xex` (`game:\`). On the host they are read from the `assets/`
directory, or from the path in `$XST_ASSETS`.
