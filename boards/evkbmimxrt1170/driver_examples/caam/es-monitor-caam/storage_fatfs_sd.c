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
        return kStorage_Success;

    BOARD_SD_Config(&g_sd, NULL, BOARD_SDMMC_SD_HOST_IRQ_PRIORITY, NULL);

    if (SD_HostInit(&g_sd) != kStatus_Success)
        return kStorage_Error;

    if (SD_PollingCardInsert(&g_sd, kSD_Inserted) != kStatus_Success)
        return kStorage_Error;

    SD_SetCardPower(&g_sd, false);
    SD_SetCardPower(&g_sd, true);

    if (f_mount(&s_fileSystem, drive, 0U) != FR_OK)
        return kStorage_Error;

#if (FF_FS_RPATH >= 2U)
    if (f_chdrive(drive) != FR_OK)
        return kStorage_Error;
#endif

    s_mounted = true;
    return kStorage_Success;
}

static storage_status_t FatFsSd_Open(const char *path, storage_open_mode_t mode, storage_file_t **outFile)
{
    BYTE ffMode;
    storage_file_t *file;

    if ((path == NULL) || (outFile == NULL))
        return kStorage_InvalidParameter;

    if (!s_mounted)
        return kStorage_NotMounted;

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

    if (f_open(&file->fil, path, ffMode) != FR_OK) {
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
        return kStorage_InvalidParameter;

    res = f_read(&file->fil, buf, (UINT)bufSize, &br);

    if (bytesRead != NULL)
        *bytesRead = (size_t)br;

    return (res == FR_OK) ? kStorage_Success : kStorage_ReadFailed;
}

static storage_status_t FatFsSd_Write(storage_file_t *file, const uint8_t *buf, size_t size, size_t *bytesWritten)
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

static storage_status_t FatFsSd_Close(storage_file_t *file)
{
    FRESULT res;

    if (file == NULL)
        return kStorage_InvalidParameter;

    res = f_close(&file->fil); /* commits data and directory entry */
    free(file);

    return (res == FR_OK) ? kStorage_Success : kStorage_Error;
}

static storage_status_t FatFsSd_GetFileSize(const char *path, size_t *size)
{
    FIL file;

    if ((path == NULL) || (size == NULL))
        return kStorage_InvalidParameter;

    if (!s_mounted)
        return kStorage_NotMounted;

    if (f_open(&file, path, FA_READ) != FR_OK)
        return kStorage_OpenFailed;

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
 * Registration / probe
 ******************************************************************************/

static const storage_ops_t s_fatFsSdOps = {
    .init        = FatFsSd_Init,
    .open        = FatFsSd_Open,
    .read        = FatFsSd_Read,
    .write       = FatFsSd_Write,
    .close       = FatFsSd_Close,
    .getFileSize = FatFsSd_GetFileSize,
    .deinit      = FatFsSd_Deinit,
};

/*!
 * @brief Probe function for the FatFs+SD0 backend.
 *
 * Called once by STORAGE_ProbeAll(). Attempts to bring the SD card/USDHC
 * host up and mount FatFs on it; on success, registers this backend's ops
 * table under (kStorageFs_FatFs, kStorageDevice_Sd0) so the application can
 * later obtain it via STORAGE_GetOps(). If the hardware/mount is not
 * available, simply does not register - STORAGE_GetOps() will then return
 * NULL for this (fs, device) pair.
 */

static void __attribute__((constructor))
STORAGE_IF_RegisterFatFsSd(void)
{
    STORAGE_RegisterOps(kStorageFs_FatFs, kStorageDevice_Sd0, &s_fatFsSdOps);
}
