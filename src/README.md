# Sources

- `core/`: protocol messages, index database, model, scanner, puller (portable)
- `net/`: TLS, connections, local and global discovery, relays (portable, using the platform's sockets)
- `api/`: the REST API and the Web GUI server (portable)
- `ui/`: the console UI (console only)
- `platform/`: `platform.hpp` is the seam; `xsdk/` is the console backend, `posix/` the host's
