# Architecture

```
+-----------------------------+   +-------------------------+
| Console UI (src/ui, ImGui)  |   | Web GUI (stock assets)  |
+--------------+--------------+   +------------+------------+
               |  in-process                   | HTTP(S) :8384
               v                               v
        +------+-------------------------------+------+
        |  Model (src/core): folders, devices, index  |<--- REST API (src/api)
        +------+---------------+---------------+------+
               |               |               |
        Scanner/Puller     Index DB        Connections (src/net)
        (src/core)         (SQLite)        TLS 1.3 BEP, relays, discovery
               |               |               |
        +------+---------------+---------------+------+
        |  Platform (src/platform/{xsdk,posix})       |
        |  files, sockets, threads, entropy, log      |
        +---------------------------------------------+
```

- **One source of truth.** The console UI and the REST API both read and change the same in-process model. Neither
  keeps state of its own.
- **Portable core.** Everything above the platform layer builds on the Linux host, so the protocol logic is tested
  there against stock Syncthing.
- **Console constraints** are handled in the platform layer and the fs rules:
  - IPv4 only
  - FATX's 42-character names and 2-second mtimes
  - no rename or remove of open files
  - foreground only
