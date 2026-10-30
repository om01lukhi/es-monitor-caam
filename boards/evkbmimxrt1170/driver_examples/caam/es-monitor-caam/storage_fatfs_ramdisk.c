/*
 * Copyright 2024 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * FatFs / in-memory RAM disk backend for the storage API declared in
 * storage.h.
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
        return kStorage_Success;

    /* opt=1 forces an immediate mount (reads the volume right now instead
     * of lazily on first file access), so a missing filesystem is detected
     * here and can be formatted immediately. With opt=0, f_mount() always
     * returns FR_OK right away and the real failure would only surface
     * later, on the first f_open() - which is exactly the bug this fixes
     * (RAM disk open() was failing with FR_NO_FILESYSTEM at first use). */
    mountResult = f_mount(&s_fileSystem, drive, 1U);
    if (mountResult != FR_OK) {
        /* RAM disk is blank on every boot: no filesystem is present yet.
         * Format it, then mount again. */
        uint8_t work[FF_MAX_SS];
        FRESULT mkfsResult;
        FRESULT remountResult;

        PRINTF("RAM disk: initial mount failed, FRESULT=%d - formatting\r\n", (int)mountResult);

        mkfsResult = f_mkfs(drive, NULL, work, sizeof(work));
        if (mkfsResult != FR_OK) {
            PRINTF("RAM disk: f_mkfs failed, FRESULT=%d\r\n", (int)mkfsResult);
            return kStorage_Error;
        }

        remountResult = f_mount(&s_fileSystem, drive, 1U);
        if (remountResult != FR_OK) {
            PRINTF("RAM disk: mount after format failed, FRESULT=%d\r\n", (int)remountResult);
            return kStorage_Error;
        }

        PRINTF("RAM disk: formatted and mounted successfully\r\n");
    }

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
        return kStorage_InvalidParameter;

    if (!s_mounted)
        return kStorage_NotMounted;

    if (!FatFsRamDisk_BuildPath(path, fullPath, sizeof(fullPath)))
        return kStorage_InvalidParameter;

    switch (mode) {
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
        return kStorage_OutOfMemory;

    openResult = f_open(&file->fil, fullPath, ffMode);
    if (openResult != FR_OK) {
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
        return kStorage_InvalidParameter;

    res = f_read(&file->fil, buf, (UINT)bufSize, &br);

    if (bytesRead != NULL)
        *bytesRead = (size_t)br;

    return (res == FR_OK) ? kStorage_Success : kStorage_ReadFailed;
}

static storage_status_t FatFsRamDisk_Write(storage_file_t *file, const uint8_t *buf, size_t size, size_t *bytesWritten)
{
    UINT bw = 0U;
    FRESULT res;

    if ((file == NULL) || (buf == NULL))
        return kStorage_InvalidParameter;

    res = f_write(&file->fil, buf, (UINT)size, &bw);

    if (bytesWritten != NULL)
        *bytesWritten = (size_t)bw;

    if ((res != FR_OK) || (bw != (UINT)size))
        return kStorage_WriteFailed;

    return kStorage_Success;
}

static storage_status_t FatFsRamDisk_Close(storage_file_t *file)
{
    FRESULT res;

    if (file == NULL)
        return kStorage_InvalidParameter;

    res = f_close(&file->fil);
    free(file);

    return (res == FR_OK) ? kStorage_Success : kStorage_Error;
}

static storage_status_t FatFsRamDisk_GetFileSize(const char *path, size_t *size)
{
    char fullPath[FATFS_RAMDISK_MAX_PATH];
    FIL file;

    if ((path == NULL) || (size == NULL))
        return kStorage_InvalidParameter;

    if (!s_mounted)
        return kStorage_NotMounted;

    if (!FatFsRamDisk_BuildPath(path, fullPath, sizeof(fullPath)))
        return kStorage_InvalidParameter;

    if (f_open(&file, fullPath, FA_READ) != FR_OK)
        return kStorage_OpenFailed;

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
};

/*!
 * @brief Probe function for the FatFs+RAM-disk backend.
 *
 * Called once by STORAGE_ProbeAll(). Attempts to mount (formatting first,
 * if necessary) the in-memory RAM disk; on success, registers this
 * backend's ops table under (kStorageFs_FatFs, kStorageDevice_RamDisk0).
 */
static void __attribute__((constructor))
STORAGE_IF_RegisterFatFsRamDisk(void)
{
    STORAGE_RegisterOps(kStorageFs_FatFs, kStorageDevice_RamDisk0, &s_fatFsRamDiskOps);
}