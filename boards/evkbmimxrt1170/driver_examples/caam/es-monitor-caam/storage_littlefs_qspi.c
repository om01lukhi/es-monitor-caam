/*
 * Copyright 2024 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * littlefs / on-board QSPI (FlexSPI) NOR flash backend stub for the storage
 * API declared in storage.h.
 *
 * This file is a placeholder: it declares the full storage_ops_t table and
 * registers it under (kStorageFs_LittleFs, kStorageDevice_QspiNor0), but
 * every operation currently just returns kStorage_Error ("not yet
 * implemented") without touching any real littlefs/FlexSPI driver. This
 * lets the generic STORAGE_GetOps() lookup and the registry/probe
 * machinery be exercised for this (fs, device) pair, and gives a clear,
 * minimal skeleton to fill in with a real littlefs_hal (read/prog/erase)
 * and middleware/littlefs integration later.
 *
 * Mirrors the structure of storage_fatfs_sd.c, with the backend-specific
 * details (FlexSPI/littlefs config, lfs_t, lfs_file_t, flash erase/program)
 * intentionally left out until a real implementation is needed.
 */

#include "storage.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/

/*! @brief Backend-defined contents of the opaque storage_file_t handle.
 *  Placeholder - would hold an lfs_file_t (+ its buffers) once littlefs is
 *  wired up. */
struct storage_file
{
    int reserved;
};

/*******************************************************************************
 * Low-level ops: init/open/read/write/close/getFileSize/deinit
 *
 * All stubs for now - return kStorage_Error to signal "not implemented".
 ******************************************************************************/

static storage_status_t LittleFsQspi_Init(void)
{
    /* TODO: configure FlexSPI NOR flash HAL (read/prog/erase callbacks) and
     * lfs_mount() (or lfs_format() + lfs_mount() on first use). */
    return kStorage_Error;
}

static storage_status_t LittleFsQspi_Open(const char *path, storage_open_mode_t mode, storage_file_t **outFile)
{
    (void)path;
    (void)mode;
    (void)outFile;
    /* TODO: lfs_file_open() with mode translated to LFS_O_RDONLY / LFS_O_WRONLY|LFS_O_CREAT. */
    return kStorage_Error;
}

static storage_status_t LittleFsQspi_Read(storage_file_t *file, uint8_t *buf, size_t bufSize, size_t *bytesRead)
{
    (void)file;
    (void)buf;
    (void)bufSize;
    if (bytesRead != NULL)
    {
        *bytesRead = 0U;
    }
    /* TODO: lfs_file_read(). */
    return kStorage_Error;
}

static storage_status_t LittleFsQspi_Write(storage_file_t *file, const uint8_t *buf, size_t size, size_t *bytesWritten)
{
    (void)file;
    (void)buf;
    (void)size;
    if (bytesWritten != NULL)
    {
        *bytesWritten = 0U;
    }
    /* TODO: lfs_file_write(). */
    return kStorage_Error;
}

static storage_status_t LittleFsQspi_Close(storage_file_t *file)
{
    (void)file;
    /* TODO: lfs_file_close(). */
    return kStorage_Error;
}

static storage_status_t LittleFsQspi_GetFileSize(const char *path, size_t *size)
{
    (void)path;
    if (size != NULL)
    {
        *size = 0U;
    }
    /* TODO: lfs_stat() and read info.size. */
    return kStorage_Error;
}

static storage_status_t LittleFsQspi_Deinit(void)
{
    /* TODO: lfs_unmount(). */
    return kStorage_Success;
}

static storage_status_t LittleFsQspi_RunFileTest(const char *srcPath, const char *dstPath, size_t maxFileSize, bool *matched)
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

static const storage_ops_t s_littleFsQspiOps = {
    .init        = LittleFsQspi_Init,
    .open        = LittleFsQspi_Open,
    .read        = LittleFsQspi_Read,
    .write       = LittleFsQspi_Write,
    .close       = LittleFsQspi_Close,
    .getFileSize = LittleFsQspi_GetFileSize,
    .deinit      = LittleFsQspi_Deinit,
    .runFileTest = LittleFsQspi_RunFileTest,
};

/*!
 * @brief Probe function for the littlefs+QSPI-NOR backend.
 *
 * Called once by STORAGE_ProbeAll(). Currently always attempts init() and
 * only registers on success; since LittleFsQspi_Init() is a stub that
 * returns kStorage_Error, this backend will not register and
 * STORAGE_GetOps(kStorageFs_LittleFs, kStorageDevice_QspiNor0) will return
 * NULL until a real implementation is filled in.
 */
void STORAGE_LittleFsQspi_Probe(void)
{
    if (LittleFsQspi_Init() == kStorage_Success)
    {
        STORAGE_RegisterOps(kStorageFs_LittleFs, kStorageDevice_QspiNor0, &s_littleFsQspiOps);
    }
}
