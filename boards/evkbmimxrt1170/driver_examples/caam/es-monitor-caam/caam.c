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

/*
 * Storage/file test configuration. All storage implementation (FatFs/SD,
 * buffer allocation, read/write/verify workflow) lives in
 * storage_fatfs_sd.c behind the storage_ops_t function-pointer table in
 * storage.h. caam.c only ever calls through those function pointers.
 */
#define STORAGE_TEST_SRC_PATH   "/T64K.BIN"
#define STORAGE_TEST_DST_PATH   "/test.bin"
#define STORAGE_TEST_MAX_FILE_SIZE (64U * 1024U)

#if 1
/*!
 * @brief Manual storage test: calls the individual open/read/write/close
 * function pointers directly, in the same order the registered
 * runFileTest() uses internally, instead of going through that single
 * combined call.
 *
 * The data buffer is allocated/owned by the caller, per the storage API's
 * ownership rules; the backend only reads/writes through the pointer. Each
 * open() is paired with its own close() as soon as it is no longer needed.
 *
 * A single "ret" status is checked after each step and simply returned on
 * the first failure, instead of nesting an if/else per step.
 */
static storage_status_t RunStorageFileTestManual(const storage_ops_t *storage)
{
    storage_file_t *file = NULL;
    size_t fileSize       = 0U;
    uint8_t *buf          = NULL;
    size_t bytesIo        = 0U;
    storage_status_t ret;

    ret = storage->getFileSize(STORAGE_TEST_SRC_PATH, &fileSize);
    if (ret != kStorage_Success)
    {
        PRINTF("getFileSize(%s) failed, status=%d\r\n", STORAGE_TEST_SRC_PATH, (int)ret);
        return ret;
    }
    if (fileSize == 0U)
    {
        PRINTF("%s is empty, nothing to read/write\r\n", STORAGE_TEST_SRC_PATH);
        return kStorage_Success;
    }
    if (fileSize > STORAGE_TEST_MAX_FILE_SIZE)
    {
        PRINTF("%s too large (%u bytes)\r\n", STORAGE_TEST_SRC_PATH, (unsigned)fileSize);
        return kStorage_FileTooLarge;
    }

    buf = (uint8_t *)malloc(fileSize);
    if (buf == NULL)
    {
        PRINTF("malloc(%u) failed\r\n", (unsigned)fileSize);
        return kStorage_OutOfMemory;
    }

    /* 1) open + read from source, then close as soon as the read is done. */
    ret = storage->open(STORAGE_TEST_SRC_PATH, kStorage_OpenRead, &file);
    if (ret != kStorage_Success)
    {
        PRINTF("open(%s) failed, status=%d\r\n", STORAGE_TEST_SRC_PATH, (int)ret);
        free(buf);
        return ret;
    }

    ret = storage->read(file, buf, fileSize, &bytesIo);
    (void)storage->close(file);
    file = NULL;

    if ((ret != kStorage_Success) || (bytesIo != fileSize))
    {
        PRINTF("read(%s) failed, status=%d, got %u of %u bytes\r\n", STORAGE_TEST_SRC_PATH, (int)ret,
               (unsigned)bytesIo, (unsigned)fileSize);
        free(buf);
        return (ret != kStorage_Success) ? ret : kStorage_ReadFailed;
    }

    PRINTF("Read %u bytes from %s\r\n", (unsigned)bytesIo, STORAGE_TEST_SRC_PATH);
    PRINTF("Last 16 bytes of read data: ");
    for (size_t i = (bytesIo > 16U) ? (bytesIo - 16U) : 0U; i < bytesIo; i++) /* last 16 bytes */
    {
        PRINTF("%02x ", (unsigned)buf[i]);
    }
    PRINTF("\r\n");

    /* 2) open + write to destination, then close as soon as the write is done. */
    ret = storage->open(STORAGE_TEST_DST_PATH, kStorage_OpenWriteCreate, &file);
    if (ret != kStorage_Success)
    {
        PRINTF("open(%s) failed, status=%d\r\n", STORAGE_TEST_DST_PATH, (int)ret);
        free(buf);
        return ret;
    }

    ret = storage->write(file, buf, bytesIo, NULL);
    (void)storage->close(file);
    file = NULL;
    free(buf);

    if (ret != kStorage_Success)
    {
        PRINTF("write(%s) failed, status=%d\r\n", STORAGE_TEST_DST_PATH, (int)ret);
        return ret;
    }

    PRINTF("Wrote %u bytes to %s\r\n", (unsigned)bytesIo, STORAGE_TEST_DST_PATH);
    return kStorage_Success;
}
#endif

/*!
 * @brief Main function.
 */
int main(void)
{
    /* Init hardware */
    BOARD_ConfigMPU();
    BOARD_InitPins();
    BOARD_BootClockRUN();
    BOARD_InitDebugConsole();

    /* Obtain the storage backend's function-pointer table once, then only
     * call through storage->... from here on. All implementation details
     * (FatFs/SD init, buffer allocation, read/write/verify workflow) live
     * in storage_fatfs_sd.c behind this API. */
    const storage_ops_t *storage = STORAGE_GetFatFsSdOps();
    storage_status_t fsStatus;

    fsStatus = storage->init();
    if (fsStatus != kStorage_Success)
    {
        PRINTF("SD mount failed (status=%d)\r\n", (int)fsStatus);
    }
    else
    {
#if 1
        fsStatus = RunStorageFileTestManual(storage);
        if (fsStatus != kStorage_Success)
        {
            PRINTF("Storage file test failed, status=%d\r\n", (int)fsStatus);
        }
#else
        /* Reads STORAGE_TEST_SRC_PATH, writes it to STORAGE_TEST_DST_PATH and
         * verifies the two match. All open()/read()/write()/close() calls
         * happen inside this single function pointer call. */
        bool fileTestMatched = false;

        fsStatus = storage->runFileTest(STORAGE_TEST_SRC_PATH, STORAGE_TEST_DST_PATH, STORAGE_TEST_MAX_FILE_SIZE,
                                         &fileTestMatched);
        if (fsStatus != kStorage_Success)
        {
            PRINTF("Storage file test failed, status=%d\r\n", (int)fsStatus);
        }
        else if (!fileTestMatched)
        {
            PRINTF("Storage file test: no verified match (see diagnostics above)\r\n");
        }
#endif
    }

    while (1)
    {
    }
}
