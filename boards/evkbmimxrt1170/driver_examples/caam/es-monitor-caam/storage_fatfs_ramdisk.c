/*
 * Copyright 2024 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * FatFs / in-memory RAM disk backend stub for the storage API declared in
 * storage.h.
 *
 * Unlike storage_fatfs_sd.c and storage_littlefs_qspi.c, this backend has
 * no real external hardware dependency - "the device" is simply a static
 * RAM buffer acting as a block device. It is still left as a stub (every
 * operation returns kStorage_Error) for now, matching what was asked for:
 * just the files/protocol/registration wired up, not a working
 * implementation.
 *
 * A real implementation would typically:
 *   - provide a FatFs diskio.c-style driver (disk_read/disk_write/
 *     disk_ioctl) backed by a static uint8_t array instead of USDHC/SD,
 *   - f_mount() a drive number bound to that diskio driver,
 *   - then open/read/write/close exactly like storage_fatfs_sd.c does.
 *
 * Mirrors the structure of storage_fatfs_sd.c, with the backend-specific
 * details (RAM disk size, FatFs drive number, diskio glue) intentionally
 * left out until a real implementation is needed.
 */

#include "storage.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/

/*! @brief Backend-defined contents of the opaque storage_file_t handle.
 *  Placeholder - would hold a FatFs FIL once mounted on the RAM disk. */
struct storage_file
{
    int reserved;
};

/*******************************************************************************
 * Low-level ops: init/open/read/write/close/getFileSize/deinit
 *
 * All stubs for now - return kStorage_Error to signal "not implemented".
 ******************************************************************************/

static storage_status_t FatFsRamDisk_Init(void)
{
    /* TODO: register a diskio driver backed by a static RAM buffer, then
     * f_mkfs()/f_mount() on it (RAM disk starts out unformatted). */
    return kStorage_Error;
}

static storage_status_t FatFsRamDisk_Open(const char *path, storage_open_mode_t mode, storage_file_t **outFile)
{
    (void)path;
    (void)mode;
    (void)outFile;
    /* TODO: f_open() on the RAM-disk-backed drive. */
    return kStorage_Error;
}

static storage_status_t FatFsRamDisk_Read(storage_file_t *file, uint8_t *buf, size_t bufSize, size_t *bytesRead)
{
    (void)file;
    (void)buf;
    (void)bufSize;
    if (bytesRead != NULL)
    {
        *bytesRead = 0U;
    }
    /* TODO: f_read(). */
    return kStorage_Error;
}

static storage_status_t FatFsRamDisk_Write(storage_file_t *file, const uint8_t *buf, size_t size, size_t *bytesWritten)
{
    (void)file;
    (void)buf;
    (void)size;
    if (bytesWritten != NULL)
    {
        *bytesWritten = 0U;
    }
    /* TODO: f_write(). */
    return kStorage_Error;
}

static storage_status_t FatFsRamDisk_Close(storage_file_t *file)
{
    (void)file;
    /* TODO: f_close(). */
    return kStorage_Error;
}

static storage_status_t FatFsRamDisk_GetFileSize(const char *path, size_t *size)
{
    (void)path;
    if (size != NULL)
    {
        *size = 0U;
    }
    /* TODO: f_open()+f_size()+f_close(), same as FatFsSd_GetFileSize(). */
    return kStorage_Error;
}

static storage_status_t FatFsRamDisk_Deinit(void)
{
    /* TODO: f_unmount(); RAM contents are naturally discarded. */
    return kStorage_Success;
}

static storage_status_t FatFsRamDisk_RunFileTest(const char *srcPath, const char *dstPath, size_t maxFileSize, bool *matched)
{
    (void)srcPath;
    (void)dstPath;
    (void)maxFileSize;
    if (matched != NULL)
    {
        *matched = false;
    }
    /* TODO: implement using only this file's own open()/read()/write()/close(),
     * same pattern as FatFsSd_RunFileTest() in storage_fatfs_sd.c. */
    return kStorage_Error;
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
 * Called once by STORAGE_ProbeAll(). Currently always attempts init() and
 * only registers on success; since FatFsRamDisk_Init() is a stub that
 * returns kStorage_Error, this backend will not register and
 * STORAGE_GetOps(kStorageFs_FatFs, kStorageDevice_RamDisk0) will return
 * NULL until a real implementation is filled in.
 */
void STORAGE_FatFsRamDisk_Probe(void)
{
    if (FatFsRamDisk_Init() == kStorage_Success)
    {
        STORAGE_RegisterOps(kStorageFs_FatFs, kStorageDevice_RamDisk0, &s_fatFsRamDiskOps);
    }
}
