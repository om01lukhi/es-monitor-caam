/*
 * Copyright 2024 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * FatFs/SD-card backend for the storage API declared in storage.h.
 *
 * This file owns all FatFs/SD-card details: FIL, FRESULT, SD-card globals
 * (g_sd) and mount state. None of that is exposed to the application; it
 * only sees storage_ops_t and storage_file_t.
 *
 * All higher-level workflow (runFileTest: read -> write -> verify) also
 * lives here and is built exclusively on top of this file's own
 * open()/read()/write()/close()/getFileSize() implementations - it never
 * touches FatFs/SD APIs directly.
 */

#include "storage.h"

#include <stdlib.h>
#include <string.h>

#include "fsl_debug_console.h"
#include "ff.h"
#include "diskio.h"
#include "fsl_sd.h"
#include "fsl_sd_disk.h"
#include "sdmmc_config.h"
#include "board.h"

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

/* DMA/cache alignment required by the USDHC/SD backend for the data
 * buffer used in runFileTest()'s internal read/write. */
#define FATFS_SD_BUF_ALIGN (BOARD_SDMMC_DATA_BUFFER_ALIGN_SIZE)

/*******************************************************************************
 * Low-level ops: init/open/read/write/close/getFileSize/deinit
 ******************************************************************************/

static storage_status_t FatFsSd_Init(void)
{
    const TCHAR drive[3U] = {SDDISK + '0', ':', '/'};

    if (s_mounted)
    {
        return kStorage_Success;
    }

    BOARD_SD_Config(&g_sd, NULL, BOARD_SDMMC_SD_HOST_IRQ_PRIORITY, NULL);

    if (SD_HostInit(&g_sd) != kStatus_Success)
    {
        return kStorage_Error;
    }
    if (SD_PollingCardInsert(&g_sd, kSD_Inserted) != kStatus_Success)
    {
        return kStorage_Error;
    }
    SD_SetCardPower(&g_sd, false);
    SD_SetCardPower(&g_sd, true);

    if (f_mount(&s_fileSystem, drive, 0U) != FR_OK)
    {
        return kStorage_Error;
    }
#if (FF_FS_RPATH >= 2U)
    if (f_chdrive(drive) != FR_OK)
    {
        return kStorage_Error;
    }
#endif

    s_mounted = true;
    return kStorage_Success;
}

static storage_status_t FatFsSd_Open(const char *path, storage_open_mode_t mode, storage_file_t **outFile)
{
    BYTE ffMode;
    storage_file_t *file;

    if ((path == NULL) || (outFile == NULL))
    {
        return kStorage_InvalidParameter;
    }
    if (!s_mounted)
    {
        return kStorage_NotMounted;
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

    if (f_open(&file->fil, path, ffMode) != FR_OK)
    {
        free(file);
        return kStorage_OpenFailed;
    }

    *outFile = file;
    return kStorage_Success;
}

static storage_status_t FatFsSd_Read(storage_file_t *file, uint8_t *buf, size_t bufSize, size_t *bytesRead)
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

static storage_status_t FatFsSd_Write(storage_file_t *file, const uint8_t *buf, size_t size, size_t *bytesWritten)
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

static storage_status_t FatFsSd_Close(storage_file_t *file)
{
    FRESULT res;

    if (file == NULL)
    {
        return kStorage_InvalidParameter;
    }

    res = f_close(&file->fil); /* commits data and directory entry */
    free(file);

    return (res == FR_OK) ? kStorage_Success : kStorage_Error;
}

static storage_status_t FatFsSd_GetFileSize(const char *path, size_t *size)
{
    FIL file;

    if ((path == NULL) || (size == NULL))
    {
        return kStorage_InvalidParameter;
    }
    if (!s_mounted)
    {
        return kStorage_NotMounted;
    }

    if (f_open(&file, path, FA_READ) != FR_OK)
    {
        return kStorage_OpenFailed;
    }

    *size = (size_t)f_size(&file);
    (void)f_close(&file);

    return kStorage_Success;
}

static storage_status_t FatFsSd_Deinit(void)
{
    /* No explicit unmount API is used by this example; mark state so init()
     * can be called again if desired. */
    s_mounted = false;
    return kStorage_Success;
}

/*******************************************************************************
 * High-level workflow: runFileTest (read -> write -> verify)
 *
 * Everything below is implemented purely in terms of the ops table's own
 * open()/read()/write()/close()/getFileSize() - every read and every write
 * is wrapped by its own open()/close() pair, as required.
 ******************************************************************************/

/*! @brief Internal buffer bookkeeping; never exposed outside this file. */
typedef struct
{
    uint8_t *data; /* aligned, use this */
    size_t size;   /* number of valid bytes in data */
    void *raw;     /* pointer returned by malloc, for free() */
} fatfs_sd_buf_t;

static void FatFsSd_FreeBuf(fatfs_sd_buf_t *buf)
{
    free(buf->raw);
    buf->raw  = NULL;
    buf->data = NULL;
    buf->size = 0U;
}

/* Reads the whole of path into a freshly allocated, aligned buffer.
 * Internally: getFileSize() -> open() -> read() -> close(). On success,
 * buf->data holds buf->size bytes; caller must FatFsSd_FreeBuf() it. */
static storage_status_t FatFsSd_ReadWholeFile(const char *path, size_t maxFileSize, fatfs_sd_buf_t *buf)
{
    storage_file_t *file = NULL;
    storage_status_t status;
    size_t fsize = 0U;
    size_t br    = 0U;
    void *raw;
    uint8_t *aligned;
    size_t allocSize;

    buf->data = NULL;
    buf->raw  = NULL;
    buf->size = 0U;

    status = FatFsSd_GetFileSize(path, &fsize);
    if (status != kStorage_Success)
    {
        return status;
    }

    if (fsize == 0U)
    {
        return kStorage_Success; /* empty file: size 0, data NULL */
    }
    if (fsize > maxFileSize)
    {
        return kStorage_FileTooLarge;
    }

    status = FatFsSd_Open(path, kStorage_OpenRead, &file);
    if (status != kStorage_Success)
    {
        return status;
    }

    /* round size up to a multiple of the alignment, plus one unit for
     * manual alignment of the returned pointer */
    allocSize = ((fsize + FATFS_SD_BUF_ALIGN - 1U) & ~((size_t)FATFS_SD_BUF_ALIGN - 1U)) + FATFS_SD_BUF_ALIGN;

    raw = malloc(allocSize);
    if (raw == NULL)
    {
        (void)FatFsSd_Close(file);
        return kStorage_OutOfMemory;
    }
    aligned = (uint8_t *)(((uintptr_t)raw + FATFS_SD_BUF_ALIGN - 1U) & ~((uintptr_t)FATFS_SD_BUF_ALIGN - 1U));

    status = FatFsSd_Read(file, aligned, fsize, &br);
    (void)FatFsSd_Close(file);

    if ((status != kStorage_Success) || (br != fsize))
    {
        free(raw);
        return (status == kStorage_Success) ? kStorage_ReadFailed : status;
    }

    buf->raw  = raw;
    buf->data = aligned;
    buf->size = br;
    return kStorage_Success;
}

/* Writes data/size to path. Internally: open() -> write() -> close(). */
static storage_status_t FatFsSd_WriteWholeFile(const char *path, const uint8_t *data, size_t size)
{
    storage_file_t *file = NULL;
    storage_status_t status;
    size_t written = 0U;
    storage_status_t closeStatus;

    status = FatFsSd_Open(path, kStorage_OpenWriteCreate, &file);
    if (status != kStorage_Success)
    {
        return status;
    }

    status      = FatFsSd_Write(file, data, size, &written);
    closeStatus = FatFsSd_Close(file);

    if (status != kStorage_Success)
    {
        return status;
    }
    if (written != size)
    {
        return kStorage_WriteFailed;
    }
    return closeStatus;
}

/* Chunk size used to verify the written file without doubling the memory
 * footprint of the whole source buffer. Must be a multiple of
 * FATFS_SD_BUF_ALIGN. */
#define FATFS_SD_VERIFY_CHUNK (512U)

/* Re-opens dstPath (open -> [read in small chunks, comparing against
 * expected] -> close) and reports whether every byte matched expected. Uses
 * a small fixed-size aligned chunk buffer instead of allocating a second
 * full-size buffer, so verifying a large file does not require holding two
 * copies of it in RAM at once. */
static storage_status_t FatFsSd_VerifyFile(const char *path, const uint8_t *expected, size_t expectedSize, bool *matched)
{
    SDK_ALIGN(static uint8_t s_verifyChunk[FATFS_SD_VERIFY_CHUNK], FATFS_SD_BUF_ALIGN);
    storage_file_t *file = NULL;
    storage_status_t status;
    size_t offset = 0U;

    *matched = false;

    status = FatFsSd_Open(path, kStorage_OpenRead, &file);
    if (status != kStorage_Success)
    {
        return status;
    }

    while (offset < expectedSize)
    {
        size_t want = expectedSize - offset;
        size_t got  = 0U;

        if (want > sizeof(s_verifyChunk))
        {
            want = sizeof(s_verifyChunk);
        }

        status = FatFsSd_Read(file, s_verifyChunk, want, &got);
        if ((status != kStorage_Success) || (got != want))
        {
            (void)FatFsSd_Close(file);
            return (status == kStorage_Success) ? kStorage_ReadFailed : status;
        }

        if (memcmp(s_verifyChunk, expected + offset, want) != 0)
        {
            (void)FatFsSd_Close(file);
            return kStorage_Success; /* read succeeded; comparison mismatch - matched stays false */
        }

        offset += want;
    }

    (void)FatFsSd_Close(file);

    /* Confirm the file is not longer than expected (EOF reached exactly). */
    {
        size_t actualSize = 0U;
        status             = FatFsSd_GetFileSize(path, &actualSize);
        if ((status == kStorage_Success) && (actualSize == expectedSize))
        {
            *matched = true;
        }
    }

    return kStorage_Success;
}

static storage_status_t FatFsSd_RunFileTest(const char *srcPath, const char *dstPath, size_t maxFileSize, bool *matched)
{
    fatfs_sd_buf_t src = {0};
    storage_status_t status;

    if (matched != NULL)
    {
        *matched = false;
    }
    if ((srcPath == NULL) || (dstPath == NULL))
    {
        return kStorage_InvalidParameter;
    }

    /* 1) Read source file (open -> read -> close happens inside). */
    status = FatFsSd_ReadWholeFile(srcPath, maxFileSize, &src);
    if (status != kStorage_Success)
    {
        PRINTF("Read failed (open/size limit/malloc/read), status=%d\r\n", (int)status);
        return status;
    }

    PRINTF("Read %u bytes from %s\r\n", (unsigned)src.size, srcPath);
    for (size_t i = (src.size > 16U) ? (src.size - 16U) : 0U; i < src.size; i++) /* last 16 bytes */
    {
        PRINTF("%02x ", (unsigned)src.data[i]);
    }
    PRINTF("\r\n");

    if ((src.data == NULL) || (src.size == 0U))
    {
        PRINTF("No data read from file to write\r\n");
        FatFsSd_FreeBuf(&src);
        return kStorage_Success; /* nothing to write/verify; not an error */
    }

    /* 2) Write destination file (open -> write -> close happens inside). */
    status = FatFsSd_WriteWholeFile(dstPath, src.data, src.size);
    if (status != kStorage_Success)
    {
        PRINTF("Write failed, status=%d\r\n", (int)status);
        FatFsSd_FreeBuf(&src);
        return status;
    }
    PRINTF("Wrote %u bytes to %s\r\n", (unsigned)src.size, dstPath);

    /* 3) Verify: re-open dstPath and compare it to the source buffer in
     * small chunks (open -> read -> close happens inside), without
     * allocating a second full-size buffer. */
    bool verified = false;
    status         = FatFsSd_VerifyFile(dstPath, src.data, src.size, &verified);
    if (status != kStorage_Success)
    {
        PRINTF("Verify read failed, status=%d\r\n", (int)status);
        FatFsSd_FreeBuf(&src);
        return status;
    }

    if (verified)
    {
        PRINTF("Verify OK: written file matches original data (%u bytes)\r\n", (unsigned)src.size);
        if (matched != NULL)
        {
            *matched = true;
        }
    }
    else
    {
        PRINTF("Verify FAILED: written file does not match original data\r\n");
    }

    FatFsSd_FreeBuf(&src);
    return kStorage_Success;
}


/*******************************************************************************
 * Registration
 ******************************************************************************/

static const storage_ops_t s_fatFsSdOps = {
    .init        = FatFsSd_Init,
    .open        = FatFsSd_Open,
    .read        = FatFsSd_Read,
    .write       = FatFsSd_Write,
    .close       = FatFsSd_Close,
    .getFileSize = FatFsSd_GetFileSize,
    .deinit      = FatFsSd_Deinit,
    .runFileTest = FatFsSd_RunFileTest,
};

const storage_ops_t *STORAGE_GetFatFsSdOps(void)
{
    return &s_fatFsSdOps;
}
