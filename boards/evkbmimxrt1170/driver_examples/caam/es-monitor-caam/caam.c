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

/* Source file: must already exist on the SD card and on the USB drive. */
#define STORAGE_TEST_SRC_PATH        "/T64K.BIN"
/* Destination file: created / overwritten by the write operation. */
#define STORAGE_TEST_DST_PATH        "/test.bin"
/* Upper bound on file size; malloc() is bounded to this. */
#define STORAGE_TEST_MAX_FILE_SIZE   (64U * 1024U)

/* Time given to the USB flash drive to power up and present itself before the
 * first USB test. The USB backend only waits 2 s for the attach event, which
 * is not enough if the drive is still starting up. Earlier test sequences
 * gave the drive this time implicitly by running other tests first. */
#define STORAGE_TEST_USB_SETTLE_US   (5000000U)

/*! @brief Operation selected by the caller of RunStorageTest(). */
typedef enum _storage_test_op
{
    kTestOp_Read = 0, /*!< Read STORAGE_TEST_SRC_PATH, print size and tail. */
    kTestOp_Write,    /*!< Write an incrementing pattern to STORAGE_TEST_DST_PATH. */
    kTestOp_Both      /*!< Read the source file, then write its data to STORAGE_TEST_DST_PATH. */
} storage_test_op_t;

/*! @brief Short label for (fs, device), used only in PRINTF log lines. */
static const char *storage_label(storage_fs_type_t fs, storage_device_type_t dev)
/* Chunk size used when reading back for verification (avoids a second
 * full-file heap allocation running concurrently with the first). */
#define STORAGE_TEST_VERIFY_CHUNK    (512U)
/* Small write-pattern test used on backends that start empty (RAM disk,
 * littlefs/QSPI): a simple incrementing byte sequence of this length. */
#define STORAGE_TEST_PATTERN_PATH    "/storetest.bin"
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
 * Storage test:  init -> file size -> malloc -> switch(op) -> cleanup
 *
 *   kTestOp_Read  - open src, read, close, print size and last 16 bytes
 *   kTestOp_Write - fill buffer with a pattern, open dst, write, close
 *   kTestOp_Both  - open src, read, close, then open dst, write, close
 *
 * init() of a backend returns immediately if it is already initialized, so
 * calling RunStorageTest() several times for the same device is fine.
 ******************************************************************************/
static void RunStorageTest(storage_fs_type_t fs, storage_device_type_t dev, storage_test_op_t op)
{
    const char *label        = storage_label(fs, dev);
    const storage_ops_t *ops = storage_get_ops(fs, dev);
    storage_file_t *file     = NULL;
    uint8_t *buf             = NULL;
    size_t fileSize          = 0U;
    size_t bytesIo           = 0U;
    storage_status_t ret;

    if (ops == NULL) {
        PRINTF("[%s] not available (not registered)\r\n", label);
        return;
    }

    /* init */
    ret = ops->init();
    if (ret != kStorage_Success) {
        PRINTF("[%s] init failed (status=%d)\r\n", label, (int)ret);
        return;
    }

    /* file size */
    ret = ops->getFileSize(STORAGE_TEST_SRC_PATH, &fileSize);
    if ((ret != kStorage_Success) || (fileSize == 0U) || (fileSize > STORAGE_TEST_MAX_FILE_SIZE)) {
        PRINTF("[%s] %s missing or bad size (status=%d, size=%u)\r\n",
               label, STORAGE_TEST_SRC_PATH, (int)ret, (unsigned)fileSize);
        return;
    }

    buf = (uint8_t *)malloc(fileSize);
    if (buf == NULL) {
        PRINTF("[%s] malloc(%u) failed\r\n", label, (unsigned)fileSize);
        return;
    }

    /* operation */
    switch (op) {
        case kTestOp_Read:
            ret = ops->open(STORAGE_TEST_SRC_PATH, kStorage_OpenRead, &file);
            if (ret != kStorage_Success) {
                PRINTF("[%s] open(%s) failed, status=%d\r\n", label, STORAGE_TEST_SRC_PATH, (int)ret);
                break;
            }
            ret = ops->read(file, buf, fileSize, &bytesIo);
            (void)ops->close(file);
            file = NULL;
            if ((ret != kStorage_Success) || (bytesIo != fileSize)) {
                PRINTF("[%s] read failed, status=%d, got %u of %u bytes\r\n",
                       label, (int)ret, (unsigned)bytesIo, (unsigned)fileSize);
                break;
            }
            PRINTF("[%s] read %u bytes from %s\r\n", label, (unsigned)bytesIo, STORAGE_TEST_SRC_PATH);
            PRINTF("[%s] last 16 bytes: ", label);
            for (size_t i = (bytesIo > 16U) ? (bytesIo - 16U) : 0U; i < bytesIo; i++)
                PRINTF("%02x ", (unsigned)buf[i]);
            PRINTF("\r\n");
            break;

        case kTestOp_Write:
            for (size_t i = 0U; i < fileSize; i++)
                buf[i] = (uint8_t)i;
            ret = ops->open(STORAGE_TEST_DST_PATH, kStorage_OpenWriteCreate, &file);
            if (ret != kStorage_Success) {
                PRINTF("[%s] open(%s) for write failed, status=%d\r\n", label, STORAGE_TEST_DST_PATH, (int)ret);
                break;
            }
            ret = ops->write(file, buf, fileSize, &bytesIo);
            (void)ops->close(file);
            file = NULL;
            if ((ret != kStorage_Success) || (bytesIo != fileSize)) {
                PRINTF("[%s] write failed, status=%d, wrote %u of %u bytes\r\n",
                       label, (int)ret, (unsigned)bytesIo, (unsigned)fileSize);
                break;
            }
            PRINTF("[%s] wrote %u bytes to %s\r\n", label, (unsigned)bytesIo, STORAGE_TEST_DST_PATH);
            break;

        case kTestOp_Both:
            /* read */
            ret = ops->open(STORAGE_TEST_SRC_PATH, kStorage_OpenRead, &file);
            if (ret != kStorage_Success) {
                PRINTF("[%s] open(%s) failed, status=%d\r\n", label, STORAGE_TEST_SRC_PATH, (int)ret);
                break;
            }
            ret = ops->read(file, buf, fileSize, &bytesIo);
            (void)ops->close(file);
            file = NULL;
            if ((ret != kStorage_Success) || (bytesIo != fileSize)) {
                PRINTF("[%s] read failed, status=%d, got %u of %u bytes\r\n",
                       label, (int)ret, (unsigned)bytesIo, (unsigned)fileSize);
                break;
            }
            PRINTF("[%s] read %u bytes from %s\r\n", label, (unsigned)bytesIo, STORAGE_TEST_SRC_PATH);

            /* write */
            ret = ops->open(STORAGE_TEST_DST_PATH, kStorage_OpenWriteCreate, &file);
            if (ret != kStorage_Success) {
                PRINTF("[%s] open(%s) for write failed, status=%d\r\n", label, STORAGE_TEST_DST_PATH, (int)ret);
                break;
            }
            ret = ops->write(file, buf, fileSize, &bytesIo);
            (void)ops->close(file);
            file = NULL;
            if ((ret != kStorage_Success) || (bytesIo != fileSize)) {
                PRINTF("[%s] write failed, status=%d, wrote %u of %u bytes\r\n",
                       label, (int)ret, (unsigned)bytesIo, (unsigned)fileSize);
                break;
            }
            PRINTF("[%s] wrote %u bytes to %s\r\n", label, (unsigned)bytesIo, STORAGE_TEST_DST_PATH);
            break;

        default:
            PRINTF("[%s] invalid operation %d\r\n", label, (int)op);
            break;
    }

    /* cleanup */
    if (file != NULL)
        (void)ops->close(file);

    /* deinit */
    ret = ops->deinit();
    if (ret != kStorage_Success) {
        PRINTF("[%s] deinit failed (status=%d)\r\n", label, (int)ret);
        return;
    }
    free(buf);
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

    RunStorageTest(kStorageFs_FatFs, kStorageDevice_Sd0,  kTestOp_Write);
    RunStorageTest(kStorageFs_FatFs, kStorageDevice_Usb0, kTestOp_Read);
    RunStorageTest(kStorageFs_FatFs, kStorageDevice_Usb0, kTestOp_Both);

    while (1)
    {
    }
}
