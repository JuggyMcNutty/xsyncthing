# Building

The build runs on Linux (Debian, Arch, or WSL on Windows). It needs `git`, `cmake` (3.25 or later), `ninja`,
`python3` and a host C/C++ compiler. The toolchain build needs xsdk's prerequisites as well; see
`third_party/xsdk/docs/GETTING_STARTED.md`.

## 1. Clone with submodules

```sh
git clone --recursive https://github.com/JuggyMcNutty/xsyncthing
# or, in an existing clone:
git submodule update --init --recursive
```

## 2. Toolchain and xsdk

```sh
scripts/bootstrap.sh
```

This builds GCC 15, binutils and newlib for `powerpc-xsdk-elf` into `~/xsdk-gcc`, using xsdk's
`tools/toolchain/build.sh`. Set `XSDK_TOOLCHAIN=<prefix>` to install somewhere else. Then it runs `make` in
`third_party/xsdk` to build `libxsdk.a`. The toolchain step is skipped when the toolchain is already there.

## 3. Console build

```sh
scripts/build-xex.sh            # -> build-xsdk/xsyncthing.xex
```

Copy `xsyncthing.xex` to the console, for example over FTP to `hdd:\Apps\xsyncthing\`, and launch it from the
dashboard. The log is written beside it as `xsyncthing.log`.

## 4. Host build and tests

```sh
scripts/build-host.sh           # configures build-host/, builds, runs ctest
```
