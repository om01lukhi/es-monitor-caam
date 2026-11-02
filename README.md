# es-monitor-caam: Multi-Backend Storage Layer on i.MX RT1170 (EVKB)

A bare-metal example for the NXP **MIMXRT1170-EVKB** (Cortex-M7) built on the
MCUXpresso SDK 2.12.2 `caam` driver example. It adds a small, filesystem- and
device-agnostic **storage abstraction** and four interchangeable backends, plus
one unified self-test that runs the same way on every backend.

| Backend | Filesystem | Device |
|---|---|---|
| `storage_fatfs_sd.c` | FatFs | SD card (USDHC) |
| `storage_fatfs_ramdisk.c` | FatFs | In-memory RAM disk |
| `storage_littlefs_qspi.c` | littlefs | On-board QSPI/FlexSPI NOR |
| `storage_usb_msd.c` | FatFs | USB host mass-storage (EHCI) |

Everything above is added on top of the stock SDK example. The original CAAM
example files (board, clock, pin mux, DCD, linker scripts) are kept.

---

## 1. What was added, and where

All paths are relative to
`boards/evkbmimxrt1170/driver_examples/caam/es-monitor-caam/`.

### New files

| File | Purpose |
|---|---|
| `storage.h` | Public API: status codes, open modes, `storage_fs_type_t`, `storage_device_type_t`, the `storage_ops_t` function-pointer table, and the registry calls `STORAGE_RegisterOps()` / `STORAGE_GetOps()`. |
| `storage.c` | The registry itself: a static `[fs][device]` table of `storage_ops_t *`. |
| `storage_fatfs_sd.c` | FatFs on SD card. Uses `sdmmc_config.c` and the SDK `fsl_sd` driver. |
| `storage_fatfs_ramdisk.c` | FatFs on a RAM disk. Mounts at init and, if the blank RAM has no filesystem, runs `f_mkfs` and re-mounts. |
| `storage_littlefs_qspi.c` | littlefs on the on-board QSPI NOR, via `mflash_drv`. |
| `storage_usb_msd.c` | FatFs on a USB flash drive: USB host stack init, waits for MSD attach, mounts. |
| `sdmmc_config.c/.h` | Board glue for the SD card host controller. |
| `usb_host_config.h` | USB host stack configuration for the MSD backend. |
| `ffconf.h` | FatFs configuration (note `FF_USE_LFN = 0`, see Limitations). |
| `README.md` | This document. |

### Modified files

| File | Change |
|---|---|
| `caam.c` | Replaced the original CAAM demo `main()` with the storage test described below. |
| `armgcc/CMakeLists.txt`, `armgcc/config.cmake`, `armgcc/flags.cmake` | Added the new sources and the FatFs, littlefs, sdmmc and USB host middleware to the build. |
| `pin_mux.c/.h` | Pin muxing for SD (USDHC), USB and the debug console. A backup of the earlier version is kept as `pin_mux.c.bak`. |

Middleware under `middleware/` (fatfs, littlefs, sdmmc, usb) is the stock SDK
code and is only referenced by the build.

---

## 2. Design

### 2.1 Storage abstraction

Applications never call FatFs or littlefs directly. They ask the registry for
an ops table and call through it:

```c
const storage_ops_t *ops = STORAGE_GetOps(kStorageFs_FatFs, kStorageDevice_Sd0);
ops->init();
ops->open("/file.bin", kStorage_OpenRead, &f);
ops->read(f, buf, len, &got);
ops->close(f);
```

`storage_ops_t` provides: `init`, `open`, `read`, `write`, `close`,
`getFileSize`, `deinit`.

### 2.2 Self-registering backends

Each backend defines a static `storage_ops_t` and registers it from a
`__attribute__((constructor))` function. That function runs before `main()`,
so the backend only has to be linked in. Adding a new backend means adding
one `.c` file and one enum value. `main()` and the other backends do not change.

```
  main() -> RunStorageTest(fs, dev) -> STORAGE_GetOps(fs, dev) -> ops->...
                                              ^
          constructors in storage_*.c register their ops before main()
```

### 2.3 Unified test: `RunStorageTest(fs, dev)`

The earlier version had four near-identical test functions plus repeated setup
code in `main()`. These are now one function, and `main()` is four calls:

```c
RunStorageTest(kStorageFs_FatFs,    kStorageDevice_Sd0);
RunStorageTest(kStorageFs_FatFs,    kStorageDevice_RamDisk0);
RunStorageTest(kStorageFs_LittleFs, kStorageDevice_QspiNor0);
RunStorageTest(kStorageFs_FatFs,    kStorageDevice_Usb0);
```

`RunStorageTest` looks up the backend, calls `init()`, then chooses a strategy
by checking whether `/T64K.BIN` exists on the medium:

* **Source file present (SD card, USB drive).** It reads `/T64K.BIN` (up to
  64 KB) into one heap buffer, writes it to `/test.bin`, then reads
  `/test.bin` back in 512-byte chunks and compares each chunk to the heap
  buffer.
* **No source file (RAM disk, QSPI littlefs).** It writes a 128-byte
  incrementing pattern to `/stortest.bin`, reads it back and compares with
  `memcmp`. This path uses only stack buffers.

Each log line is prefixed with a label such as `[FatFs/SD]` or `[littlefs/QSPI]`.

---

## 3. Problems found and fixed

1. **Out-of-memory on USB verify (`status=7`).** The verify step allocated a
   second 64 KB buffer while the first was still live. With the bare-metal
   newlib-nano heap, a second 64 KB `malloc` failed. Verification now uses a
   512-byte stack chunk, so only one large allocation exists at a time.
2. **RAM disk open failed (`FRESULT=6`, `FR_INVALID_NAME`).** The pattern file
   was named `/storage_test.bin`. FatFs is built with `FF_USE_LFN = 0`, so only
   8.3 names are valid. It is now `/stortest.bin`.
3. **RAM disk had no filesystem at first use.** `f_mount` with option 0 always
   returns OK and defers the real mount. The backend now mounts with option 1
   so a missing filesystem is detected immediately and formatted.

---

## 4. Building

Requires the Arm GNU toolchain and CMake.

```bash
export ARMGCC_DIR=/path/to/arm-gnu-toolchain
cd armgcc
bash build_flexspi_nor_debug.sh      # output: flexspi_nor_debug/caam_cm7.elf
```

Other targets: `build_debug.sh`, `build_release.sh`, `build_sdram_*.sh`,
`build_flexspi_nor_release.sh`, and `build_all.sh`. Use `clean.sh` to remove
build products.

## 5. Running

1. Copy a 64 KB file named `T64K.BIN` to the root of the SD card and of the
   USB flash drive. The file is not needed for the RAM disk or QSPI tests.
2. Insert the SD card and plug in the USB drive.
3. Flash and run, and open the debug console (LPUART, 115200 baud).

Expected output, abbreviated:

```
[FatFs/SD] verify OK: /test.bin matches /T64K.BIN (65536 bytes)
RAM disk: formatted and mounted successfully
[FatFs/RamDisk] verify OK: read-back matches written pattern (128 bytes)
[littlefs/QSPI] verify OK: read-back matches written pattern (128 bytes)
[FatFs/USB-MSD] verify OK: /test.bin matches /T64K.BIN (65536 bytes)
```

If a backend is not registered or its init fails, the test prints a message
and moves on to the next one.

