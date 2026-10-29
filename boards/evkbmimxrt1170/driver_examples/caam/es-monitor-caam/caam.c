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

/* Path used for the RAM disk manual test. The RAM disk starts out blank
 * every boot, so there is no pre-existing source file to read on it - the
 * test instead writes a known in-code pattern itself, then reads it back. */
#define STORAGE_RAMDISK_TEST_PATH      "/ramtest.bin"
#define STORAGE_RAMDISK_TEST_DATA_SIZE (128U)

/*!
 * @brief Manual RAM disk test: calls the individual open/write/close and
 * open/read/close function pointers directly (same style as
 * RunStorageFileTestManual() above), to verify the RAM disk backend's ops
 * are actually wired up and working, independent of the SD card backend.
 *
 * Writes a known in-code pattern to STORAGE_RAMDISK_TEST_PATH on the RAM
 * disk, reads it back, and compares byte-for-byte.
 */
static storage_status_t RunRamDiskFileTestManual(const storage_ops_t *ramDisk)
{
    storage_file_t *file = NULL;
    uint8_t writeBuf[STORAGE_RAMDISK_TEST_DATA_SIZE];
    uint8_t readBuf[STORAGE_RAMDISK_TEST_DATA_SIZE];
    size_t bytesIo = 0U;
    storage_status_t ret;

    for (size_t i = 0U; i < sizeof(writeBuf); i++)
    {
        writeBuf[i] = (uint8_t)(i & 0xFFU);
    }

    /* 1) open + write, then close as soon as the write is done. */
    ret = ramDisk->open(STORAGE_RAMDISK_TEST_PATH, kStorage_OpenWriteCreate, &file);
    if (ret != kStorage_Success)
    {
        PRINTF("RAM disk open(%s) for write failed, status=%d\r\n", STORAGE_RAMDISK_TEST_PATH, (int)ret);
        return ret;
    }

    ret = ramDisk->write(file, writeBuf, sizeof(writeBuf), &bytesIo);
    (void)ramDisk->close(file);
    file = NULL;

    if ((ret != kStorage_Success) || (bytesIo != sizeof(writeBuf)))
    {
        PRINTF("RAM disk write(%s) failed, status=%d, wrote %u of %u bytes\r\n", STORAGE_RAMDISK_TEST_PATH, (int)ret,
               (unsigned)bytesIo, (unsigned)sizeof(writeBuf));
        return (ret != kStorage_Success) ? ret : kStorage_WriteFailed;
    }
    PRINTF("RAM disk: wrote %u bytes to %s\r\n", (unsigned)bytesIo, STORAGE_RAMDISK_TEST_PATH);

    /* 2) open + read, then close as soon as the read is done. */
    ret = ramDisk->open(STORAGE_RAMDISK_TEST_PATH, kStorage_OpenRead, &file);
    if (ret != kStorage_Success)
    {
        PRINTF("RAM disk open(%s) for read failed, status=%d\r\n", STORAGE_RAMDISK_TEST_PATH, (int)ret);
        return ret;
    }

    ret = ramDisk->read(file, readBuf, sizeof(readBuf), &bytesIo);
    (void)ramDisk->close(file);
    file = NULL;

    if ((ret != kStorage_Success) || (bytesIo != sizeof(readBuf)))
    {
        PRINTF("RAM disk read(%s) failed, status=%d, got %u of %u bytes\r\n", STORAGE_RAMDISK_TEST_PATH, (int)ret,
               (unsigned)bytesIo, (unsigned)sizeof(readBuf));
        return (ret != kStorage_Success) ? ret : kStorage_ReadFailed;
    }
    PRINTF("RAM disk: read back %u bytes from %s\r\n", (unsigned)bytesIo, STORAGE_RAMDISK_TEST_PATH);

    /* 3) verify. */
    if (memcmp(writeBuf, readBuf, sizeof(writeBuf)) == 0)
    {
        PRINTF("RAM disk verify OK: read-back data matches written pattern (%u bytes)\r\n",
               (unsigned)sizeof(writeBuf));
    }
    else
    {
        PRINTF("RAM disk verify FAILED: read-back data does not match written pattern\r\n");
        return kStorage_Error;
    }

    return kStorage_Success;
}

/* Path used for the littlefs/QSPI manual test. Like the RAM disk, the
 * littlefs partition may be blank on first boot - the test writes a known
 * in-code pattern itself, then reads it back, instead of depending on a
 * pre-existing source file. */
#define STORAGE_LITTLEFS_TEST_PATH      "/lfstest.bin"
#define STORAGE_LITTLEFS_TEST_DATA_SIZE (128U)

/*!
 * @brief Manual littlefs/QSPI test: calls the individual open/write/close
 * and open/read/close function pointers directly (same style as
 * RunRamDiskFileTestManual() above), to verify the littlefs+QSPI backend's
 * ops are actually wired up and working, independent of the SD card and
 * RAM disk backends.
 *
 * Writes a known in-code pattern to STORAGE_LITTLEFS_TEST_PATH on the
 * littlefs/QSPI partition, reads it back, and compares byte-for-byte.
 */
static storage_status_t RunLittleFsQspiFileTestManual(const storage_ops_t *littleFs)
{
    storage_file_t *file = NULL;
    uint8_t writeBuf[STORAGE_LITTLEFS_TEST_DATA_SIZE];
    uint8_t readBuf[STORAGE_LITTLEFS_TEST_DATA_SIZE];
    size_t bytesIo = 0U;
    storage_status_t ret;

    for (size_t i = 0U; i < sizeof(writeBuf); i++)
    {
        writeBuf[i] = (uint8_t)(i & 0xFFU);
    }

    /* 1) open + write, then close as soon as the write is done. */
    ret = littleFs->open(STORAGE_LITTLEFS_TEST_PATH, kStorage_OpenWriteCreate, &file);
    if (ret != kStorage_Success)
    {
        PRINTF("littlefs/QSPI open(%s) for write failed, status=%d\r\n", STORAGE_LITTLEFS_TEST_PATH, (int)ret);
        return ret;
    }

    ret = littleFs->write(file, writeBuf, sizeof(writeBuf), &bytesIo);
    (void)littleFs->close(file);
    file = NULL;

    if ((ret != kStorage_Success) || (bytesIo != sizeof(writeBuf)))
    {
        PRINTF("littlefs/QSPI write(%s) failed, status=%d, wrote %u of %u bytes\r\n", STORAGE_LITTLEFS_TEST_PATH,
               (int)ret, (unsigned)bytesIo, (unsigned)sizeof(writeBuf));
        return (ret != kStorage_Success) ? ret : kStorage_WriteFailed;
    }
    PRINTF("littlefs/QSPI: wrote %u bytes to %s\r\n", (unsigned)bytesIo, STORAGE_LITTLEFS_TEST_PATH);

    /* 2) open + read, then close as soon as the read is done. */
    ret = littleFs->open(STORAGE_LITTLEFS_TEST_PATH, kStorage_OpenRead, &file);
    if (ret != kStorage_Success)
    {
        PRINTF("littlefs/QSPI open(%s) for read failed, status=%d\r\n", STORAGE_LITTLEFS_TEST_PATH, (int)ret);
        return ret;
    }

    ret = littleFs->read(file, readBuf, sizeof(readBuf), &bytesIo);
    (void)littleFs->close(file);
    file = NULL;

    if ((ret != kStorage_Success) || (bytesIo != sizeof(readBuf)))
    {
        PRINTF("littlefs/QSPI read(%s) failed, status=%d, got %u of %u bytes\r\n", STORAGE_LITTLEFS_TEST_PATH,
               (int)ret, (unsigned)bytesIo, (unsigned)sizeof(readBuf));
        return (ret != kStorage_Success) ? ret : kStorage_ReadFailed;
    }
    PRINTF("littlefs/QSPI: read back %u bytes from %s\r\n", (unsigned)bytesIo, STORAGE_LITTLEFS_TEST_PATH);

    /* 3) verify. */
    if (memcmp(writeBuf, readBuf, sizeof(writeBuf)) == 0)
    {
        PRINTF("littlefs/QSPI verify OK: read-back data matches written pattern (%u bytes)\r\n",
               (unsigned)sizeof(writeBuf));
    }
    else
    {
        PRINTF("littlefs/QSPI verify FAILED: read-back data does not match written pattern\r\n");
        return kStorage_Error;
    }

    return kStorage_Success;
}

/*!
 * @brief Manual USB MSD test: calls the individual open/read/write/close
 * function pointers directly, same style as RunStorageFileTestManual()
 * (SD card) above, instead of going through the single combined
 * usbMsd->runFileTest() call.
 *
 * Reads STORAGE_TEST_SRC_PATH from the USB drive, writes it back as
 * STORAGE_TEST_DST_PATH, and verifies the two match byte-for-byte.
 */
static storage_status_t RunUsbMsdFileTestManual(const storage_ops_t *usbMsd)
{
    storage_file_t *file = NULL;
    size_t fileSize       = 0U;
    uint8_t *buf          = NULL;
    size_t bytesIo        = 0U;
    storage_status_t ret;

    ret = usbMsd->getFileSize(STORAGE_TEST_SRC_PATH, &fileSize);
    if (ret != kStorage_Success)
    {
        PRINTF("USB MSD getFileSize(%s) failed, status=%d\r\n", STORAGE_TEST_SRC_PATH, (int)ret);
        return ret;
    }
    if (fileSize == 0U)
    {
        PRINTF("USB MSD: %s is empty, nothing to read/write\r\n", STORAGE_TEST_SRC_PATH);
        return kStorage_Success;
    }
    if (fileSize > STORAGE_TEST_MAX_FILE_SIZE)
    {
        PRINTF("USB MSD: %s too large (%u bytes)\r\n", STORAGE_TEST_SRC_PATH, (unsigned)fileSize);
        return kStorage_FileTooLarge;
    }

    buf = (uint8_t *)malloc(fileSize);
    if (buf == NULL)
    {
        PRINTF("USB MSD: malloc(%u) failed\r\n", (unsigned)fileSize);
        return kStorage_OutOfMemory;
    }

    /* 1) open + read from source, then close as soon as the read is done. */
    ret = usbMsd->open(STORAGE_TEST_SRC_PATH, kStorage_OpenRead, &file);
    if (ret != kStorage_Success)
    {
        PRINTF("USB MSD open(%s) failed, status=%d\r\n", STORAGE_TEST_SRC_PATH, (int)ret);
        free(buf);
        return ret;
    }

    ret = usbMsd->read(file, buf, fileSize, &bytesIo);
    (void)usbMsd->close(file);
    file = NULL;

    if ((ret != kStorage_Success) || (bytesIo != fileSize))
    {
        PRINTF("USB MSD read(%s) failed, status=%d, got %u of %u bytes\r\n", STORAGE_TEST_SRC_PATH, (int)ret,
               (unsigned)bytesIo, (unsigned)fileSize);
        free(buf);
        return (ret != kStorage_Success) ? ret : kStorage_ReadFailed;
    }

    PRINTF("USB MSD: read %u bytes from %s\r\n", (unsigned)bytesIo, STORAGE_TEST_SRC_PATH);
    PRINTF("Last 16 bytes of read data: ");
    for (size_t i = (bytesIo > 16U) ? (bytesIo - 16U) : 0U; i < bytesIo; i++) /* last 16 bytes */
    {
        PRINTF("%02x ", (unsigned)buf[i]);
    }
    PRINTF("\r\n");

    /* 2) open + write to destination, then close as soon as the write is done. */
    ret = usbMsd->open(STORAGE_TEST_DST_PATH, kStorage_OpenWriteCreate, &file);
    if (ret != kStorage_Success)
    {
        PRINTF("USB MSD open(%s) failed, status=%d\r\n", STORAGE_TEST_DST_PATH, (int)ret);
        free(buf);
        return ret;
    }

    ret = usbMsd->write(file, buf, bytesIo, NULL);
    (void)usbMsd->close(file);
    file = NULL;

    if (ret != kStorage_Success)
    {
        PRINTF("USB MSD write(%s) failed, status=%d\r\n", STORAGE_TEST_DST_PATH, (int)ret);
        free(buf);
        return ret;
    }

    PRINTF("USB MSD: wrote %u bytes to %s\r\n", (unsigned)bytesIo, STORAGE_TEST_DST_PATH);

    /* 3) open + read back the destination in small fixed-size chunks, then
     * close, then verify. A second full-file-sized heap buffer is NOT
     * used here: on this bare-metal target's small newlib-nano heap, that
     * would require buf (already allocated) and a second same-size buffer
     * to be alive simultaneously, which can exceed the available heap and
     * fail with kStorage_OutOfMemory. Instead, verify using a small,
     * fixed-size stack buffer and compare each chunk against the
     * corresponding offset into buf, matching the already-proven-correct
     * pattern used by storage_usb_msd.c's own (now-bypassed)
     * UsbMsd_VerifyFile() helper. */
    size_t writtenSize = bytesIo;
    uint8_t verifyChunk[512];
    size_t offset   = 0U;
    bool verifyFailed = false;

    ret = usbMsd->open(STORAGE_TEST_DST_PATH, kStorage_OpenRead, &file);
    if (ret != kStorage_Success)
    {
        PRINTF("USB MSD verify-open(%s) failed, status=%d\r\n", STORAGE_TEST_DST_PATH, (int)ret);
        free(buf);
        return ret;
    }

    while (offset < writtenSize)
    {
        size_t want = writtenSize - offset;
        size_t got  = 0U;

        if (want > sizeof(verifyChunk))
        {
            want = sizeof(verifyChunk);
        }

        ret = usbMsd->read(file, verifyChunk, want, &got);
        if ((ret != kStorage_Success) || (got != want))
        {
            PRINTF("USB MSD verify-read(%s) failed, status=%d, got %u of %u bytes\r\n", STORAGE_TEST_DST_PATH,
                   (int)ret, (unsigned)got, (unsigned)want);
            (void)usbMsd->close(file);
            free(buf);
            return (ret != kStorage_Success) ? ret : kStorage_ReadFailed;
        }

        if (memcmp(verifyChunk, buf + offset, want) != 0)
        {
            verifyFailed = true;
            break;
        }

        offset += want;
    }

    (void)usbMsd->close(file);
    file = NULL;

    if (verifyFailed)
    {
        PRINTF("USB MSD verify FAILED: written file does not match original data\r\n");
    }
    else
    {
        PRINTF("USB MSD verify OK: written file matches original data (%u bytes)\r\n", (unsigned)writtenSize);
    }

    free(buf);
    return kStorage_Success;
}


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


    /* Look up the ops table generically by (filesystem, device) instead of
     * calling a backend-specific function like STORAGE_GetFatFsSdOps().
     * Returns NULL if that combination was never registered (not probed,
     * or probe failed), which must be checked before use. */
    const storage_ops_t *storage = STORAGE_GetOps(kStorageFs_FatFs, kStorageDevice_Sd0);
    storage_status_t fsStatus;

    if (storage == NULL)
    {
        PRINTF("Storage (FatFs, Sd0) not available (not probed/registered)\r\n");
    }
    else
    {
        fsStatus = storage->init();
        if (fsStatus != kStorage_Success)
        {
            PRINTF("SD mount failed (status=%d)\r\n", (int)fsStatus);
            return 0;
        }

        fsStatus = RunStorageFileTestManual(storage);
        if (fsStatus != kStorage_Success)
        {
            PRINTF("Storage file test failed, status=%d\r\n", (int)fsStatus);
        }
    }

    /* Independent RAM disk backend test, to confirm its ops are wired up
     * and working regardless of the SD card backend's state above. */
    const storage_ops_t *ramDisk = STORAGE_GetOps(kStorageFs_FatFs, kStorageDevice_RamDisk0);

    if (ramDisk == NULL)
    {
        PRINTF("Storage (FatFs, RamDisk0) not available (not probed/registered)\r\n");
    }
    else
    {
        storage_status_t ramDiskStatus = ramDisk->init();

        if (ramDiskStatus != kStorage_Success)
        {
            PRINTF("RAM disk mount failed (status=%d)\r\n", (int)ramDiskStatus);
        }
        else
        {
            ramDiskStatus = RunRamDiskFileTestManual(ramDisk);
            if (ramDiskStatus != kStorage_Success)
            {
                PRINTF("RAM disk file test failed, status=%d\r\n", (int)ramDiskStatus);
            }
        }
    }

    /* Independent littlefs/QSPI (FlexSPI1) backend test, to confirm its
     * ops are wired up and working regardless of the SD card / RAM disk
     * backends' state above. Safe under flexspi_nor_debug/release builds
     * because the linker script relocates mflash_drv.c.obj/fsl_flexspi.c.obj
     * to RAM - see the comment in storage.c's STORAGE_ProbeAll(). */
    const storage_ops_t *littleFs = STORAGE_GetOps(kStorageFs_LittleFs, kStorageDevice_QspiNor0);

    if (littleFs == NULL)
    {
        PRINTF("Storage (LittleFs, QspiNor0) not available (not probed/registered)\r\n");
    }
    else
    {
        storage_status_t littleFsStatus = littleFs->init();

        if (littleFsStatus != kStorage_Success)
        {
            PRINTF("littlefs/QSPI mount failed (status=%d)\r\n", (int)littleFsStatus);
        }
        else
        {
            littleFsStatus = RunLittleFsQspiFileTestManual(littleFs);
            if (littleFsStatus != kStorage_Success)
            {
                PRINTF("littlefs/QSPI file test failed, status=%d\r\n", (int)littleFsStatus);
            }
        }
    }

    /* Independent USB host MSD/FatFs backend test. Unlike the other three
     * backends, this one depends on a USB flash drive actually being
     * plugged into the board's USB host port when init() runs - init()
     * blocks internally (polling the EHCI task function and the MSD
     * attach/enumerate state machine) until the drive attaches and mounts,
     * or a timeout elapses, so this call may take a while or simply fail
     * with kStorage_Error if nothing is plugged in. */
    const storage_ops_t *usbMsd = STORAGE_GetOps(kStorageFs_FatFs, kStorageDevice_Usb0);

    if (usbMsd == NULL)
    {
        PRINTF("Storage (FatFs, Usb0) not available (not probed/registered)\r\n");
    }
    else
    {
        storage_status_t usbMsdStatus = usbMsd->init();

        if (usbMsdStatus != kStorage_Success)
        {
            PRINTF("USB MSD mount failed (status=%d) - is a USB drive plugged in?\r\n", (int)usbMsdStatus);
        }
        else
        {
            usbMsdStatus = RunUsbMsdFileTestManual(usbMsd);
            if (usbMsdStatus != kStorage_Success)
            {
                PRINTF("USB MSD file test failed, status=%d\r\n", (int)usbMsdStatus);
            }
        }

    }

    while (1)
    {
    }
}
