/*
 * Copyright 2024 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * FatFs / in-memory RAM disk backend for the storage API declared in
 * storage.h.
 *
 * Unlike storage_fatfs_sd.c, this backend has no real external hardware
 * dependency: "the device" is the ready-made FatFs RAM disk diskio driver
 * (middleware/fatfs/source/fsl_ram_disk/fsl_ram_disk.c), which stores all
 * sector data in a static array. It is gated by RAM_DISK_ENABLE in
 * ffconf.h, and bound to physical drive number RAMDISK (0), as defined by
 * middleware/fatfs/source/diskio.h. SDDISK (used by storage_fatfs_sd.c) is
 * physical drive 2, so the two never collide.
 *
 * Mirrors the structure of storage_fatfs_sd.c: this file owns all FatFs/
 * RAM-disk details (FIL, FRESULT, mount state) and is not exposed to the
 * application; it only sees storage_ops_t and storage_file_t.
 *
 * IMPORTANT: this backend never calls f_chdrive(). FatFs's "current
 * drive" is process-wide state shared by every backend; if this backend
 * changed it, paths passed by the application/other backends without an
 * explicit "N:" prefix (e.g. "/T64K.BIN") would silently resolve against
 * the wrong drive. Instead, every path this backend is given is prefixed
 * with its own drive number ("0:") before being passed to FatFs, so this
 * backend's behavior never depends on, and never disturbs, whichever
 * drive happens to be "current".
 *
 * Because the RAM disk starts out blank (unformatted) every boot, init()
 * attempts f_mount() first and, if that fails because there is no valid
 * filesystem on it yet, formats it with f_mkfs() and mounts again.
 *
 * The self-test (runFileTest) is self-contained: it writes known in-code
 * test data onto the RAM disk, reads it back, and verifies it - it has no
 * dependency on the SD card backend or any pre-existing file.
 */

#include "storage.h"

#include <stdlib.h>
#include <string.h>

#include "fsl_debug_console.h"
#include "ff.h"
#include "diskio.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/

/*! @brief Backend-defined contents of the opaque storage_file_t handle. */
struct storage_file
{
    FIL fil;
};

static FATFS s_fileSystem;
static bool s_mounted = false;

/* RAMDISK is defined in diskio.h as physical drive 0; distinct from
 * SDDISK (2) used by storage_fatfs_sd.c, so there is no collision. */
#define FATFS_RAMDISK_DRIVE_NUM (RAMDISK)

/* Longest path this backend accepts from a caller, including the "N:"
 * prefix this backend adds internally. */
#define FATFS_RAMDISK_MAX_PATH (64U)

/* Builds "<drive>:<path>" (e.g. "0:/test.bin") into out. Returns false if
 * path does not fit. Never relies on / changes FatFs's "current drive". */
static bool FatFsRamDisk_BuildPath(const char *path, char *out, size_t outSize)
{
    size_t pathLen = strlen(path);

    /* "N:" + path + '\0' */
    if ((pathLen + 3U) > outSize)
    {
        return false;
    }

    out[0] = (char)(FATFS_RAMDISK_DRIVE_NUM + '0');
    out[1] = ':';
    out[2] = '\0';
    strcat(out, path);
    return true;
}

/*******************************************************************************
 * Low-level ops: init/open/read/write/close/getFileSize/deinit
 ******************************************************************************/

static storage_status_t FatFsRamDisk_Init(void)
{
    const TCHAR drive[3U] = {FATFS_RAMDISK_DRIVE_NUM + '0', ':', '/'};
    FRESULT mountResult;

    if (s_mounted)
    {
        return kStorage_Success;
    }

    /* opt=1 forces an immediate mount (reads the volume right now instead
     * of lazily on first file access), so a missing filesystem is detected
     * here and can be formatted immediately. With opt=0, f_mount() always
     * returns FR_OK right away and the real failure would only surface
     * later, on the first f_open() - which is exactly the bug this fixes
     * (RAM disk open() was failing with FR_NO_FILESYSTEM at first use). */
    mountResult = f_mount(&s_fileSystem, drive, 1U);
    if (mountResult != FR_OK)
    {
        /* RAM disk is blank on every boot: no filesystem is present yet.
         * Format it, then mount again. */
        uint8_t work[FF_MAX_SS];
        FRESULT mkfsResult;
        FRESULT remountResult;

        PRINTF("RAM disk: initial mount failed, FRESULT=%d - formatting\r\n", (int)mountResult);

        mkfsResult = f_mkfs(drive, NULL, work, sizeof(work));
        if (mkfsResult != FR_OK)
        {
            PRINTF("RAM disk: f_mkfs failed, FRESULT=%d\r\n", (int)mkfsResult);
            return kStorage_Error;
        }

        remountResult = f_mount(&s_fileSystem, drive, 1U);
        if (remountResult != FR_OK)
        {
            PRINTF("RAM disk: mount after format failed, FRESULT=%d\r\n", (int)remountResult);
            return kStorage_Error;
        }

        PRINTF("RAM disk: formatted and mounted successfully\r\n");
    }

    /* Deliberately not calling f_chdrive(): see file header comment. This
     * backend always builds an explicit drive prefix into every path via
     * FatFsRamDisk_BuildPath() instead. */

    s_mounted = true;
    return kStorage_Success;
}

static storage_status_t FatFsRamDisk_Open(const char *path, storage_open_mode_t mode, storage_file_t **outFile)
{
    char fullPath[FATFS_RAMDISK_MAX_PATH];
    BYTE ffMode;
    storage_file_t *file;
    FRESULT openResult;

    if ((path == NULL) || (outFile == NULL))
    {
        return kStorage_InvalidParameter;
    }
    if (!s_mounted)
    {
        return kStorage_NotMounted;
    }
    if (!FatFsRamDisk_BuildPath(path, fullPath, sizeof(fullPath)))
    {
        return kStorage_InvalidParameter;
    }

    switch (mode)
    {
        case kStorage_OpenRead:
            ffMode = FA_READ;
            break;
        case kStorage_OpenWriteCreate:
            ffMode = FA_WRITE | FA_CREATE_ALWAYS;
            break;
        default:
            return kStorage_InvalidParameter;
    }

    file = (storage_file_t *)malloc(sizeof(storage_file_t));
    if (file == NULL)
    {
        return kStorage_OutOfMemory;
    }

    openResult = f_open(&file->fil, fullPath, ffMode);
    if (openResult != FR_OK)
    {
        PRINTF("RAM disk: f_open(%s) failed, FRESULT=%d\r\n", fullPath, (int)openResult);
        free(file);
        return kStorage_OpenFailed;
    }

    *outFile = file;
    return kStorage_Success;
}

static storage_status_t FatFsRamDisk_Read(storage_file_t *file, uint8_t *buf, size_t bufSize, size_t *bytesRead)
{
    UINT br = 0U;
    FRESULT res;

    if ((file == NULL) || (buf == NULL))
    {
        return kStorage_InvalidParameter;
    }

    res = f_read(&file->fil, buf, (UINT)bufSize, &br);

    if (bytesRead != NULL)
    {
        *bytesRead = (size_t)br;
    }

    return (res == FR_OK) ? kStorage_Success : kStorage_ReadFailed;
}

static storage_status_t FatFsRamDisk_Write(storage_file_t *file, const uint8_t *buf, size_t size, size_t *bytesWritten)
{
    UINT bw = 0U;
    FRESULT res;

    if ((file == NULL) || (buf == NULL))
    {
        return kStorage_InvalidParameter;
    }

    res = f_write(&file->fil, buf, (UINT)size, &bw);

    if (bytesWritten != NULL)
    {
        *bytesWritten = (size_t)bw;
    }

    if ((res != FR_OK) || (bw != (UINT)size))
    {
        return kStorage_WriteFailed;
    }

    return kStorage_Success;
}

static storage_status_t FatFsRamDisk_Close(storage_file_t *file)
{
    FRESULT res;

    if (file == NULL)
    {
        return kStorage_InvalidParameter;
    }

    res = f_close(&file->fil);
    free(file);

    return (res == FR_OK) ? kStorage_Success : kStorage_Error;
}

static storage_status_t FatFsRamDisk_GetFileSize(const char *path, size_t *size)
{
    char fullPath[FATFS_RAMDISK_MAX_PATH];
    FIL file;

    if ((path == NULL) || (size == NULL))
    {
        return kStorage_InvalidParameter;
    }
    if (!s_mounted)
    {
        return kStorage_NotMounted;
    }
    if (!FatFsRamDisk_BuildPath(path, fullPath, sizeof(fullPath)))
    {
        return kStorage_InvalidParameter;
    }

    if (f_open(&file, fullPath, FA_READ) != FR_OK)
    {
        return kStorage_OpenFailed;
    }

    *size = (size_t)f_size(&file);
    (void)f_close(&file);

    return kStorage_Success;
}

static storage_status_t FatFsRamDisk_Deinit(void)
{
    /* RAM contents are discarded naturally on reset/power loss; just mark
     * the state so init() can mount again if desired. */
    s_mounted = false;
    return kStorage_Success;
}

/*******************************************************************************
 * High-level workflow: runFileTest (self-contained write -> read -> verify)
 *
 * Does not depend on srcPath existing anywhere (e.g. on the SD card); it
 * generates its own known test pattern in RAM, writes it to dstPath on the
 * RAM disk, reads it back, and compares. srcPath is accepted for interface
 * compatibility with other backends but is otherwise unused here.
 ******************************************************************************/

#define FATFS_RAMDISK_TEST_PATTERN_SIZE (256U)

/* Fills buf[0..size) with a simple, deterministic, verifiable pattern. */
static void FatFsRamDisk_FillPattern(uint8_t *buf, size_t size)
{
    for (size_t i = 0U; i < size; i++)
    {
        buf[i] = (uint8_t)(i & 0xFFU);
    }
}

static storage_status_t FatFsRamDisk_RunFileTest(const char *srcPath, const char *dstPath, size_t maxFileSize, bool *matched)
{
    uint8_t testData[FATFS_RAMDISK_TEST_PATTERN_SIZE];
    uint8_t readBack[FATFS_RAMDISK_TEST_PATTERN_SIZE];
    storage_file_t *file = NULL;
    storage_status_t status;
    size_t written = 0U;
    size_t read    = 0U;

    (void)srcPath;   /* self-contained test: no external source file needed */
    (void)maxFileSize;

    if (matched != NULL)
    {
        *matched = false;
    }
    if (dstPath == NULL)
    {
        return kStorage_InvalidParameter;
    }

    FatFsRamDisk_FillPattern(testData, sizeof(testData));

    /* 1) Write known test data (open -> write -> close). */
    status = FatFsRamDisk_Open(dstPath, kStorage_OpenWriteCreate, &file);
    if (status != kStorage_Success)
    {
        PRINTF("RAM disk write-open failed, status=%d\r\n", (int)status);
        return status;
    }

    status = FatFsRamDisk_Write(file, testData, sizeof(testData), &written);
    (void)FatFsRamDisk_Close(file);
    file = NULL;

    if ((status != kStorage_Success) || (written != sizeof(testData)))
    {
        PRINTF("RAM disk write failed, status=%d\r\n", (int)status);
        return (status == kStorage_Success) ? kStorage_WriteFailed : status;
    }
    PRINTF("RAM disk: wrote %u bytes to %s\r\n", (unsigned)written, dstPath);

    /* 2) Read it back (open -> read -> close). */
    status = FatFsRamDisk_Open(dstPath, kStorage_OpenRead, &file);
    if (status != kStorage_Success)
    {
        PRINTF("RAM disk read-open failed, status=%d\r\n", (int)status);
        return status;
    }

    status = FatFsRamDisk_Read(file, readBack, sizeof(readBack), &read);
    (void)FatFsRamDisk_Close(file);
    file = NULL;

    if ((status != kStorage_Success) || (read != sizeof(testData)))
    {
        PRINTF("RAM disk read failed, status=%d\r\n", (int)status);
        return (status == kStorage_Success) ? kStorage_ReadFailed : status;
    }
    PRINTF("RAM disk: read back %u bytes from %s\r\n", (unsigned)read, dstPath);

    /* 3) Verify. */
    if (memcmp(testData, readBack, sizeof(testData)) == 0)
    {
        PRINTF("RAM disk verify OK: read-back data matches written pattern (%u bytes)\r\n", (unsigned)sizeof(testData));
        if (matched != NULL)
        {
            *matched = true;
        }
    }
    else
    {
        PRINTF("RAM disk verify FAILED: read-back data does not match written pattern\r\n");
    }

    return kStorage_Success;
}

/*******************************************************************************
 * Registration / probe
 ******************************************************************************/

static const storage_ops_t s_fatFsRamDiskOps = {
    .init        = FatFsRamDisk_Init,
    .open        = FatFsRamDisk_Open,
    .read        = FatFsRamDisk_Read,
    .write       = FatFsRamDisk_Write,
    .close       = FatFsRamDisk_Close,
    .getFileSize = FatFsRamDisk_GetFileSize,
    .deinit      = FatFsRamDisk_Deinit,
    .runFileTest = FatFsRamDisk_RunFileTest,
};

/*!
 * @brief Probe function for the FatFs+RAM-disk backend.
 *
 * Called once by STORAGE_ProbeAll(). Attempts to mount (formatting first,
 * if necessary) the in-memory RAM disk; on success, registers this
 * backend's ops table under (kStorageFs_FatFs, kStorageDevice_RamDisk0).
 */
void STORAGE_FatFsRamDisk_Probe(void)
{
    storage_status_t initStatus = FatFsRamDisk_Init();

    if (initStatus == kStorage_Success)
    {
        STORAGE_RegisterOps(kStorageFs_FatFs, kStorageDevice_RamDisk0, &s_fatFsRamDiskOps);
    }
    else
    {
        PRINTF("RAM disk: probe failed, init status=%d - not registering backend\r\n", (int)initStatus);
    }
}
