/*
 * Copyright (c) 2016, Freescale Semiconductor, Inc.
 * Copyright 2016-2022 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include "fsl_device_registers.h"
#include "fsl_debug_console.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "board.h"
#include "fsl_caam.h"
#include "storage.h"
#include "stdlib.h"
#include "stdint.h"
#include <string.h>

/*******************************************************************************
 * Storage test configuration
 ******************************************************************************/

/* Source file expected on SD card and USB flash drive (must be pre-copied). */
#define STORAGE_TEST_SRC_PATH        "/T64K.BIN"
/* Destination written during the copy/verify step. */
#define STORAGE_TEST_DST_PATH        "/test.bin"
/* Upper bound on source file size; malloc() is bounded to this. */
#define STORAGE_TEST_MAX_FILE_SIZE   (64U * 1024U)
/* Chunk size used when reading back for verification (avoids a second
 * full-file heap allocation running concurrently with the first). */
#define STORAGE_TEST_VERIFY_CHUNK    (512U)
/* Small write-pattern test used on backends that start empty (RAM disk,
 * littlefs/QSPI): a simple incrementing byte sequence of this length. */
#define STORAGE_TEST_PATTERN_PATH    "/storage_test.bin"
#define STORAGE_TEST_PATTERN_SIZE    (128U)

/*******************************************************************************
 * Helpers
 ******************************************************************************/

/*! @brief Returns a short human-readable label for the (fs, device) pair,
 * used only in PRINTF log lines so the caller does not have to repeat it. */
static const char *StorageLabel(storage_fs_type_t fs, storage_device_type_t dev)
{
    if (fs == kStorageFs_FatFs)
    {
        switch (dev)
        {
            case kStorageDevice_Sd0:      return "FatFs/SD";
            case kStorageDevice_RamDisk0: return "FatFs/RamDisk";
            case kStorageDevice_Usb0:     return "FatFs/USB-MSD";
            default:                      break;
        }
    }
    else if (fs == kStorageFs_LittleFs)
    {
        if (dev == kStorageDevice_QspiNor0)
        {
            return "littlefs/QSPI";
        }
    }
    return "Storage";
}

/*******************************************************************************
 * Unified storage test
 *
 * Single entry point called from main() for every backend. Automatically
 * chooses the test strategy based on whether a pre-existing source file
 * (STORAGE_TEST_SRC_PATH) is present on the medium:
 *
 *   - File present (SD card, USB flash drive): read the source file into a
 *     heap buffer, write it to STORAGE_TEST_DST_PATH, then read the
 *     destination back in fixed-size chunks and compare against the
 *     in-memory buffer to verify the write.  Only one heap allocation
 *     (the source data buffer) is ever alive at a time; the verify step
 *     reuses a small stack-local chunk buffer to avoid a second same-size
 *     heap allocation which would exceed the bare-metal heap on this target.
 *
 *   - No file / empty medium (RAM disk, littlefs/QSPI - blank at every
 *     boot): write a 128-byte incrementing pattern to
 *     STORAGE_TEST_PATTERN_PATH, read it back into a second stack buffer,
 *     and memcmp the two. No heap allocation needed at all for this path.
 ******************************************************************************/
static void RunStorageTest(storage_fs_type_t fs, storage_device_type_t dev)
{
    const char *label          = StorageLabel(fs, dev);
    const storage_ops_t *ops   = STORAGE_GetOps(fs, dev);
    storage_status_t ret;

    /* 1. Look up registered backend. */
    if (ops == NULL) {
        PRINTF("[%s] not available (not registered)\r\n", label);
        return;
    }

    /* 2. Init / mount. */
    ret = ops->init();
    if (ret != kStorage_Success) {
        PRINTF("[%s] init failed (status=%d)\r\n", label, (int)ret);
        return;
    }

    /* 3. Probe for a pre-existing source file to determine test strategy. */
    size_t fileSize = 0U;
    ret = ops->getFileSize(STORAGE_TEST_SRC_PATH, &fileSize);
    bool hasSrcFile = (ret == kStorage_Success) && (fileSize > 0U);

    if (hasSrcFile) {
        /*------------------------------------------------------------------
         * When source file exists - read it, write a copy, verify.
         * Used for SD card and USB flash drive which are pre-loaded with
         * STORAGE_TEST_SRC_PATH before the test runs (T64K.bin).
         *-----------------------------------------------------------------*/
        storage_file_t *file = NULL;
        uint8_t *buf         = NULL;
        size_t bytesIo       = 0U;

        if (fileSize > STORAGE_TEST_MAX_FILE_SIZE) {
            PRINTF("[%s] source file too large (%u bytes, limit=%u)\r\n",
                   label, (unsigned)fileSize, (unsigned)STORAGE_TEST_MAX_FILE_SIZE);
            return;
        }

        buf = (uint8_t *)malloc(fileSize);
        if (buf == NULL) {
            PRINTF("[%s] malloc(%u) failed\r\n", label, (unsigned)fileSize);
            return;
        }

        /* 3A-1: open + read source, close immediately after. */
        ret = ops->open(STORAGE_TEST_SRC_PATH, kStorage_OpenRead, &file);
        if (ret != kStorage_Success) {
            PRINTF("[%s] open(%s) failed, status=%d\r\n", label, STORAGE_TEST_SRC_PATH, (int)ret);
            free(buf);
            return;
        }

        ret = ops->read(file, buf, fileSize, &bytesIo);
        (void)ops->close(file);
        file = NULL;

        if ((ret != kStorage_Success) || (bytesIo != fileSize)) {
            PRINTF("[%s] read(%s) failed, status=%d, got %u of %u bytes\r\n",
                   label, STORAGE_TEST_SRC_PATH, (int)ret, (unsigned)bytesIo, (unsigned)fileSize);
            free(buf);
            return;
        }

        PRINTF("[%s] read %u bytes from %s\r\n", label, (unsigned)bytesIo, STORAGE_TEST_SRC_PATH);
        PRINTF("[%s] last 16 bytes: ", label);
        for (size_t i = (bytesIo > 16U) ? (bytesIo - 16U) : 0U; i < bytesIo; i++)
            PRINTF("%02x ", (unsigned)buf[i]);

        PRINTF("\r\n");

        /* 3A-2: open + write destination, close immediately after. */
        ret = ops->open(STORAGE_TEST_DST_PATH, kStorage_OpenWriteCreate, &file);
        if (ret != kStorage_Success) {
            PRINTF("[%s] open(%s) for write failed, status=%d\r\n", label, STORAGE_TEST_DST_PATH, (int)ret);
            free(buf);
            return;
        }

        ret = ops->write(file, buf, bytesIo, NULL);
        (void)ops->close(file);
        file = NULL;

        if (ret != kStorage_Success) {
            PRINTF("[%s] write(%s) failed, status=%d\r\n", label, STORAGE_TEST_DST_PATH, (int)ret);
            free(buf);
            return;
        }

        PRINTF("[%s] wrote %u bytes to %s\r\n", label, (unsigned)bytesIo, STORAGE_TEST_DST_PATH);

        /* 3A-3: verify - read destination back in fixed-size chunks and
         * compare against buf. No second full-size heap buffer needed. */
        uint8_t verifyChunk[STORAGE_TEST_VERIFY_CHUNK];
        size_t offset     = 0U;
        bool verifyFailed = false;

        ret = ops->open(STORAGE_TEST_DST_PATH, kStorage_OpenRead, &file);
        if (ret != kStorage_Success) {
            PRINTF("[%s] verify-open(%s) failed, status=%d\r\n", label, STORAGE_TEST_DST_PATH, (int)ret);
            free(buf);
            return;
        }

        while (offset < bytesIo) {
            size_t want = bytesIo - offset;
            size_t got  = 0U;

            if (want > sizeof(verifyChunk))
                want = sizeof(verifyChunk);

            ret = ops->read(file, verifyChunk, want, &got);
            if ((ret != kStorage_Success) || (got != want)) {
                PRINTF("[%s] verify-read failed at offset %u, status=%d, got %u of %u\r\n",
                       label, (unsigned)offset, (int)ret, (unsigned)got, (unsigned)want);
                verifyFailed = true;
                break;
            }

            if (memcmp(verifyChunk, buf + offset, want) != 0) {
                PRINTF("[%s] verify mismatch at offset %u\r\n", label, (unsigned)offset);
                verifyFailed = true;
                break;
            }

            offset += want;
        }

        (void)ops->close(file);
        free(buf);

        if (!verifyFailed)
            PRINTF("[%s] verify OK: %s matches %s (%u bytes)\r\n",
                   label, STORAGE_TEST_DST_PATH, STORAGE_TEST_SRC_PATH, (unsigned)bytesIo);
        else
            PRINTF("[%s] verify FAILED\r\n", label);

    } else {
        /*------------------------------------------------------------------
         * When testing RAM disk and QSPI : write a known pattern to a file named storage_test.bin created in
         * required storage device, read back, and compare.
         *-----------------------------------------------------------------*/
        storage_file_t *file = NULL;
        uint8_t writeBuf[STORAGE_TEST_PATTERN_SIZE];
        uint8_t readBuf[STORAGE_TEST_PATTERN_SIZE];
        size_t bytesIo = 0U;

        for (size_t i = 0U; i < sizeof(writeBuf); i++)
            writeBuf[i] = (uint8_t)(i & 0xFFU);

        /* 3B-1: write pattern. */
        ret = ops->open(STORAGE_TEST_PATTERN_PATH, kStorage_OpenWriteCreate, &file);
        if (ret != kStorage_Success) {
            PRINTF("[%s] open(%s) for write failed, status=%d\r\n",
                   label, STORAGE_TEST_PATTERN_PATH, (int)ret);
            return;
        }

        ret = ops->write(file, writeBuf, sizeof(writeBuf), &bytesIo);
        (void)ops->close(file);
        file = NULL;

        if ((ret != kStorage_Success) || (bytesIo != sizeof(writeBuf))) {
            PRINTF("[%s] write(%s) failed, status=%d, wrote %u of %u bytes\r\n",
                   label, STORAGE_TEST_PATTERN_PATH, (int)ret,
                   (unsigned)bytesIo, (unsigned)sizeof(writeBuf));
            return;
        }

        PRINTF("[%s] wrote %u bytes to %s\r\n", label, (unsigned)bytesIo, STORAGE_TEST_PATTERN_PATH);

        /* 3B-2: read back. */
        ret = ops->open(STORAGE_TEST_PATTERN_PATH, kStorage_OpenRead, &file);
        if (ret != kStorage_Success) {
            PRINTF("[%s] open(%s) for read failed, status=%d\r\n",
                   label, STORAGE_TEST_PATTERN_PATH, (int)ret);
            return;
        }

        ret = ops->read(file, readBuf, sizeof(readBuf), &bytesIo);
        (void)ops->close(file);
        file = NULL;

        if ((ret != kStorage_Success) || (bytesIo != sizeof(readBuf))) {
            PRINTF("[%s] read(%s) failed, status=%d, got %u of %u bytes\r\n",
                   label, STORAGE_TEST_PATTERN_PATH, (int)ret,
                   (unsigned)bytesIo, (unsigned)sizeof(readBuf));
            return;
        }

        PRINTF("[%s] read back %u bytes from %s\r\n", label, (unsigned)bytesIo, STORAGE_TEST_PATTERN_PATH);

        /* 3B-3: compare. */
        if (memcmp(writeBuf, readBuf, sizeof(writeBuf)) == 0)
            PRINTF("[%s] verify OK: read-back matches written pattern (%u bytes)\r\n",
                   label, (unsigned)sizeof(writeBuf));
        else
            PRINTF("[%s] verify FAILED: read-back does not match written pattern\r\n", label);
    }
}

/*******************************************************************************
 * Main
 ******************************************************************************/

int main(void)
{
    BOARD_ConfigMPU();
    BOARD_InitPins();
    BOARD_BootClockRUN();
    BOARD_InitDebugConsole();

    RunStorageTest(kStorageFs_FatFs, kStorageDevice_Sd0);
    RunStorageTest(kStorageFs_FatFs, kStorageDevice_RamDisk0);
    RunStorageTest(kStorageFs_LittleFs, kStorageDevice_QspiNor0);
    RunStorageTest(kStorageFs_FatFs, kStorageDevice_Usb0);

    while (1)
    {
    }
}
