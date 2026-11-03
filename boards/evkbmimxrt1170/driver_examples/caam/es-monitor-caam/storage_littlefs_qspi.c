/*
 * Copyright 2024 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * littlefs / on-board QSPI (FlexSPI1) NOR flash backend for the storage API
 * declared in storage.h.
 *
 * Uses FlexSPI1 - the same controller this project's own firmware
 * executes-in-place (XIP) from (see armgcc/MIMXRT1176xxxxx_cm7_
 * flexspi_nor_ocram.ld - m_text lives at 0x30002400, inside FlexSPI1's AMBA
 * window). mflash_drv's init/erase/program path calls FLEXSPI_UpdateLUT()
 * and FLEXSPI_SoftwareReset() on FlexSPI1; doing that while the CPU is
 * actively fetching instructions from FlexSPI1 is unsafe UNLESS the code
 * performing the reconfiguration itself runs from RAM instead of from
 * FlexSPI1. That relocation is handled entirely in the linker script
 * (armgcc/MIMXRT1176xxxxx_cm7_flexspi_nor_ocram.ld excludes mflash_drv.c.obj
 * and fsl_flexspi.c.obj from the .text/flash placement and places them in
 * .data/RAM instead - the exact same pattern NXP's own
 * boards/evkbmimxrt1170/driver_examples/flexspi/nor/polling_transfer
 * example uses for flexspi_nor_flash_ops.c.obj/fsl_flexspi.c.obj). No
 * application code changes are needed to benefit from that relocation;
 * it is transparent to this file and to mflash_drv.c itself.
 *
 * Uses the mflash_drv (components/flash/mflash) low-level read/page-
 * program/sector-erase driver, same pattern as NXP's own littlefs_shell
 * example, targeting the default FLEXSPI1/FlexSPI1_AMBA_BASE (no override
 * macros needed).
 *
 * Mirrors the structure of storage_fatfs_sd.c / storage_fatfs_ramdisk.c:
 * this file owns all littlefs/flash details (lfs_t, lfs_file_t, lfs_config)
 * and is not exposed to the application; it only sees storage_ops_t and
 * storage_file_t.
 *
 * Because the reserved flash region may not contain a valid littlefs
 * filesystem yet (e.g. first boot, or blank/erased flash), init() attempts
 * lfs_mount() first and, if that fails, formats with lfs_format() and
 * mounts again - the same fallback pattern used by the FatFs/RAM-disk
 * backend for its blank-on-every-boot RAM disk.
 */


#include "storage.h"

#include <stdlib.h>
#include <string.h>

#include "fsl_debug_console.h"
#include "lfs.h"

#include "mflash_drv.h"



/*******************************************************************************
 * Definitions
 ******************************************************************************/

/*! @brief Backend-defined contents of the opaque storage_file_t handle. */
struct storage_file
{
    lfs_file_t file;
};

/* Byte offset into the FlexSPI1 NOR flash chip where the littlefs partition
 * starts. Must be past the end of this project's own firmware image
 * (m_text/m_flash_config/etc., all well under 1MB for this example) to
 * avoid overlapping code currently executing via XIP. 4MB matches NXP's
 * own littlefs_shell example for this exact board. */
#ifndef LITTLEFS_QSPI_START_ADDR
#define LITTLEFS_QSPI_START_ADDR (0x400000U)
#endif

/* Littlefs geometry - matches NXP's own littlefs_shell example for this
 * board/flash part (4KB erase sectors, 256B program pages). */
#define LITTLEFS_QSPI_READ_SIZE      (16U)
#define LITTLEFS_QSPI_PROG_SIZE      (MFLASH_PAGE_SIZE)
#define LITTLEFS_QSPI_BLOCK_SIZE     (MFLASH_SECTOR_SIZE)
#define LITTLEFS_QSPI_BLOCK_COUNT    (1024U) /* 1024 * 4KB = 4MB partition */
#define LITTLEFS_QSPI_BLOCK_CYCLES   (100)
#define LITTLEFS_QSPI_CACHE_SIZE     (LITTLEFS_QSPI_PROG_SIZE)
#define LITTLEFS_QSPI_LOOKAHEAD_SIZE (16U)

static lfs_t s_lfs;
static bool s_mounted = false;

/*******************************************************************************
 * littlefs block-device callbacks (read/prog/erase/sync), backed by mflash_drv
 ******************************************************************************/

static int little_fs_qspi_blockread(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer,
                                   lfs_size_t size)
{
    uint32_t flashAddr = LITTLEFS_QSPI_START_ADDR + (block * c->block_size) + off;

    if (mflash_drv_read(flashAddr, buffer, size) != kStatus_Success)
        return LFS_ERR_IO;

    return LFS_ERR_OK;
}

static int little_fs_qspi_blockprog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer,
                                   lfs_size_t size)
{
    uint32_t flashAddr = LITTLEFS_QSPI_START_ADDR + (block * c->block_size) + off;
    status_t status    = kStatus_Success;

    if (!mflash_drv_is_page_aligned(size))
        return LFS_ERR_INVAL;

    for (uint32_t pageOfs = 0U; pageOfs < size; pageOfs += MFLASH_PAGE_SIZE) {
        status = mflash_drv_page_program(flashAddr + pageOfs, (void *)((uintptr_t)buffer + pageOfs));
        if (status != kStatus_Success)
            break;
    }

    return (status == kStatus_Success) ? LFS_ERR_OK : LFS_ERR_IO;
}

static int little_fs_qspi_blockerase(const struct lfs_config *c, lfs_block_t block)
{
    uint32_t flashAddr = LITTLEFS_QSPI_START_ADDR + (block * c->block_size);
    status_t status    = kStatus_Success;

    for (uint32_t sectorOfs = 0U; sectorOfs < c->block_size; sectorOfs += MFLASH_SECTOR_SIZE) {
        status = mflash_drv_sector_erase(flashAddr + sectorOfs);
        if (status != kStatus_Success)
            break;
    }

    return (status == kStatus_Success) ? LFS_ERR_OK : LFS_ERR_IO;
}

static int little_fs_qspi_blocksync(const struct lfs_config *c)
{
    (void)c;
    return LFS_ERR_OK;
}

static const struct lfs_config s_lfsConfig = {
    .context     = NULL,
    .read        = little_fs_qspi_blockread,
    .prog        = little_fs_qspi_blockprog,
    .erase       = little_fs_qspi_blockerase,
    .sync        = little_fs_qspi_blocksync,
    .read_size   = LITTLEFS_QSPI_READ_SIZE,
    .prog_size   = LITTLEFS_QSPI_PROG_SIZE,
    .block_size  = LITTLEFS_QSPI_BLOCK_SIZE,
    .block_count = LITTLEFS_QSPI_BLOCK_COUNT,
    .block_cycles      = LITTLEFS_QSPI_BLOCK_CYCLES,
    .cache_size        = LITTLEFS_QSPI_CACHE_SIZE,
    .lookahead_size    = LITTLEFS_QSPI_LOOKAHEAD_SIZE,
};

/*******************************************************************************
 * Low-level ops: init/open/read/write/close/getFileSize/deinit
 ******************************************************************************/

static storage_status_t little_fs_qspi_init(void)
{
    int lfsResult;

    if (s_mounted)
        return kStorage_Success;

    if (mflash_drv_init() != kStatus_Success) {
        PRINTF("littlefs/QSPI: mflash_drv_init() failed\r\n");
        return kStorage_Error;
    }

    lfsResult = lfs_mount(&s_lfs, &s_lfsConfig);
    if (lfsResult != LFS_ERR_OK) {
        /* Reserved region may be blank/corrupt (e.g. first boot) - format
         * then mount again, same fallback pattern as the RAM disk backend. */
        PRINTF("littlefs/QSPI: mount failed (err=%d) - formatting\r\n", lfsResult);

        lfsResult = lfs_format(&s_lfs, &s_lfsConfig);
        if (lfsResult != LFS_ERR_OK) {
            PRINTF("littlefs/QSPI: lfs_format failed, err=%d\r\n", lfsResult);
            return kStorage_Error;
        }

        lfsResult = lfs_mount(&s_lfs, &s_lfsConfig);
        if (lfsResult != LFS_ERR_OK) {
            PRINTF("littlefs/QSPI: mount after format failed, err=%d\r\n", lfsResult);
            return kStorage_Error;
        }

        PRINTF("littlefs/QSPI: formatted and mounted successfully\r\n");
    }

    s_mounted = true;
    return kStorage_Success;
}

static storage_status_t little_fs_qspi_open(const char *path, storage_open_mode_t mode, storage_file_t **outFile)
{
    storage_file_t *file;
    int flags;
    int lfsResult;

    if ((path == NULL) || (outFile == NULL))
        return kStorage_InvalidParameter;

    if (!s_mounted)
        return kStorage_NotMounted;

    switch (mode) {
        case kStorage_OpenRead:
            flags = LFS_O_RDONLY;
            break;
        case kStorage_OpenWriteCreate:
            flags = LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
            break;
        default:
            return kStorage_InvalidParameter;
    }

    file = (storage_file_t *)malloc(sizeof(storage_file_t));
    if (file == NULL)
        return kStorage_OutOfMemory;


    lfsResult = lfs_file_open(&s_lfs, &file->file, path, flags);
    if (lfsResult != LFS_ERR_OK) {
        free(file);
        return kStorage_OpenFailed;
    }

    *outFile = file;
    return kStorage_Success;
}

static storage_status_t little_fs_qspi_read(storage_file_t *file, uint8_t *buf, size_t bufSize, size_t *bytesRead)
{
    lfs_ssize_t result;

    if ((file == NULL) || (buf == NULL))
        return kStorage_InvalidParameter;

    result = lfs_file_read(&s_lfs, &file->file, buf, (lfs_size_t)bufSize);

    if (bytesRead != NULL)
        *bytesRead = (result >= 0) ? (size_t)result : 0U;

    return (result >= 0) ? kStorage_Success : kStorage_ReadFailed;
}

static storage_status_t little_fs_qspi_write(storage_file_t *file, const uint8_t *buf, size_t size,
                                            size_t *bytesWritten)
{
    lfs_ssize_t result;

    if ((file == NULL) || (buf == NULL))
        return kStorage_InvalidParameter;

    result = lfs_file_write(&s_lfs, &file->file, buf, (lfs_size_t)size);

    if (bytesWritten != NULL)
        *bytesWritten = (result >= 0) ? (size_t)result : 0U;

    if ((result < 0) || ((size_t)result != size))
        return kStorage_WriteFailed;

    return kStorage_Success;
}

static storage_status_t little_fs_qspi_close(storage_file_t *file)
{
    int lfsResult;

    if (file == NULL)
        return kStorage_InvalidParameter;

    lfsResult = lfs_file_close(&s_lfs, &file->file);
    free(file);

    return (lfsResult == LFS_ERR_OK) ? kStorage_Success : kStorage_Error;
}

static storage_status_t little_fs_qspi_get_filesize(const char *path, size_t *size)
{
    struct lfs_info info;
    int lfsResult;

    if ((path == NULL) || (size == NULL))
        return kStorage_InvalidParameter;

    if (!s_mounted)
        return kStorage_NotMounted;

    lfsResult = lfs_stat(&s_lfs, path, &info);
    if (lfsResult != LFS_ERR_OK)
        return kStorage_OpenFailed;

    *size = (size_t)info.size;
    return kStorage_Success;
}

static storage_status_t little_fs_qspi_deinit(void)
{
    if (s_mounted)
        (void)lfs_unmount(&s_lfs);

    s_mounted = false;
    return kStorage_Success;
}

/*******************************************************************************
 * Registration / probe
 ******************************************************************************/

static const storage_ops_t s_little_fs_qspi_ops = {
    .init        = little_fs_qspi_init,
    .open        = little_fs_qspi_open,
    .read        = little_fs_qspi_read,
    .write       = little_fs_qspi_write,
    .close       = little_fs_qspi_close,
    .getFileSize = little_fs_qspi_get_filesize,
    .deinit      = little_fs_qspi_deinit,
};

/*!
 * @brief Probe function for the littlefs+QSPI-NOR backend.
 *
 * Called once by STORAGE_ProbeAll(). Attempts to mount (formatting first,
 * if necessary) the reserved littlefs partition on FlexSPI1 NOR flash; on
 * success, registers this backend's ops table under
 * (kStorageFs_LittleFs, kStorageDevice_QspiNor0).
 */
static void __attribute__((constructor))
storage_if_register_littlefs_qspi(void)
{
    storage_register_ops(kStorageFs_LittleFs, kStorageDevice_QspiNor0, &s_little_fs_qspi_ops);
}