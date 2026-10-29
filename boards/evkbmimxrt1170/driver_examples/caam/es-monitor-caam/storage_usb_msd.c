/*
 * Copyright 2024 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * USB host MSD / FatFs backend for the storage API declared in storage.h.
 *
 * Adapted from NXP's usb_host_msd_fatfs/bm/cm7 reference demo (app.c +
 * host_msd_fatfs.c), but rebuilt as a storage_ops_t backend: all USB host
 * bring-up, enumeration polling and disk mount happen inside this file's
 * init(), so the application never deals with USB host state machines -
 * it only sees storage_ops_t and storage_file_t, exactly like
 * storage_fatfs_sd.c and storage_fatfs_ramdisk.c.
 *
 * Underneath, FatFs talks to the USB MSD device through the ready-made
 * middleware/fatfs/source/fsl_usb_disk/fsl_usb_disk_bm.c diskio driver,
 * which is gated by USB_DISK_ENABLE in ffconf.h and bound to physical
 * drive number USBDISK (1), as defined by middleware/fatfs/source/diskio.h.
 * That driver talks through the global g_UsbFatfsClassHandle, which this
 * file sets from the MSD class handle obtained after enumeration.
 *
 * Unlike storage_fatfs_sd.c (hardware is always present) and
 * storage_fatfs_ramdisk.c (no real hardware at all), a USB MSD device may
 * or may not be plugged in when init() is called. init() blocks, polling
 * USB_HostTaskFn() (i.e. the EHCI task function) and this file's own MSD
 * attach/enumerate state machine, until either the device attaches and
 * FatFs successfully mounts it, or a timeout elapses - in which case
 * init() returns kStorage_Error so the application can report that no
 * USB drive was present, instead of hanging forever.
 *
 * This backend never calls f_chdrive(), for the same reason given in
 * storage_fatfs_ramdisk.c: FatFs's "current drive" is process-wide state
 * shared by every backend. Every path this backend is given is prefixed
 * with its own drive number ("1:") before being passed to FatFs.
 */

#include "storage.h"

#include <stdlib.h>
#include <string.h>

#include "fsl_debug_console.h"
#include "fsl_common.h"
#include "ff.h"
#include "diskio.h"
#include "fsl_device_registers.h"
#include "board.h"

#include "usb_host_config.h"
#include "usb_host.h"
#include "usb_host_msd.h"
#include "usb_phy.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/

/*! @brief Backend-defined contents of the opaque storage_file_t handle. */
struct storage_file
{
    FIL fil;
};

/*! @brief USB host controller instance this backend drives. Must match the
 *  physical port the user's micro-USB-OTG adapter is plugged into;
 *  NOT independently re-verified against board schematics by this file -
 *  see the caveat in the accompanying status report. */
#ifndef CONTROLLER_ID
#define CONTROLLER_ID kUSB_ControllerEhci0
#endif

#if defined(__GIC_PRIO_BITS)
#define USB_HOST_INTERRUPT_PRIORITY (25U)
#elif defined(__NVIC_PRIO_BITS) && (__NVIC_PRIO_BITS >= 3)
#define USB_HOST_INTERRUPT_PRIORITY (6U)
#else
#define USB_HOST_INTERRUPT_PRIORITY (3U)
#endif

/*! @brief device attach/detach status, mirrors app.h's usb_host_app_state_t. */
typedef enum _usb_msd_dev_state
{
    kUsbMsdDev_Idle = 0,
    kUsbMsdDev_Attached,
    kUsbMsdDev_Detached,
} usb_msd_dev_state_t;

/*! @brief internal attach/enumerate run states, mirrors host_msd_fatfs.h's
 *  usb_host_msd_run_state_t. */
typedef enum _usb_msd_run_state
{
    kUsbMsdRun_Idle = 0,
    kUsbMsdRun_SetInterface,
    kUsbMsdRun_WaitSetInterface,
    kUsbMsdRun_Ready, /* set interface done; device usable by FatFs */
} usb_msd_run_state_t;

/*! @brief One instance's worth of state - mirrors
 *  usb_host_msd_fatfs_instance_t, trimmed to what this backend needs. */
typedef struct _usb_msd_instance
{
    usb_host_configuration_handle configHandle;
    usb_device_handle deviceHandle;
    usb_host_class_handle classHandle;
    usb_host_interface_handle interfaceHandle;
    usb_msd_dev_state_t prevDeviceState;
    usb_msd_dev_state_t deviceState;
    usb_msd_run_state_t runWaitState;
    usb_msd_run_state_t runState;
} usb_msd_instance_t;

static usb_msd_instance_t s_msdInstance;
/* NOT static: fsl_usb_disk_bm.c (middleware/fatfs/source/fsl_usb_disk)
 * references this exact global by name via "extern usb_host_handle
 * g_HostHandle;", the same way app.c does in the reference demo. */
usb_host_handle g_HostHandle;
static FATFS s_fileSystem;

static bool s_mounted = false;

/* control transfer on-going state/status, used by USB_HostMsdSetInterface's
 * callback - same pattern as the reference demo's controlIng/controlStatus. */
static volatile uint8_t s_controlIng;
static volatile usb_status_t s_controlStatus;

/* FatFs physical drive number for the USB disk - defined by
 * middleware/fatfs/source/diskio.h, matches fsl_usb_disk_bm.c's pdrv. */
static const TCHAR s_usbDrive[] = {(TCHAR)(USBDISK + '0'), ':', '/', '\0'};

/* Maximum time init() waits for a USB MSD device to attach and enumerate. */
#define USB_MSD_MOUNT_TIMEOUT_MS (2000U)

/* DMA/cache alignment, matches fsl_usb_disk_bm.c's own buffer alignment
 * requirement for the data buffer used in runFileTest()'s internal
 * read/write. */
#define USB_MSD_BUF_ALIGN (USB_DATA_ALIGN_SIZE)

/*******************************************************************************
 * USB host glue: ISR, clock init, task function
 *
 * Mirrors app.c from the usb_host_msd_fatfs/bm/cm7 reference demo.
 ******************************************************************************/

/*! @brief msd class handle used by fsl_usb_disk_bm.c's diskio functions. */
extern usb_host_class_handle g_UsbFatfsClassHandle;

void USB_OTG1_IRQHandler(void)
{
    USB_HostEhciIsrFunction(g_HostHandle);
}

void USB_OTG2_IRQHandler(void)
{
    USB_HostEhciIsrFunction(g_HostHandle);
}


static void UsbMsd_ClockInit(void)
{
    uint32_t usbClockFreq;
    usb_phy_config_struct_t phyConfig = {
        BOARD_USB_PHY_D_CAL,
        BOARD_USB_PHY_TXCAL45DP,
        BOARD_USB_PHY_TXCAL45DM,
    };
    usbClockFreq = 24000000;
    if (CONTROLLER_ID == kUSB_ControllerEhci0)
    {
        CLOCK_EnableUsbhs0PhyPllClock(kCLOCK_Usbphy480M, usbClockFreq);
        CLOCK_EnableUsbhs0Clock(kCLOCK_Usb480M, usbClockFreq);
    }
    else
    {
        CLOCK_EnableUsbhs1PhyPllClock(kCLOCK_Usbphy480M, usbClockFreq);
        CLOCK_EnableUsbhs1Clock(kCLOCK_Usb480M, usbClockFreq);
    }
    USB_EhciPhyInit(CONTROLLER_ID, BOARD_XTAL0_CLK_HZ, &phyConfig);
}

static void UsbMsd_IsrEnable(void)
{
    uint8_t irqNumber;
    uint8_t usbHostEhciIrq[] = USBHS_IRQS;

    irqNumber = usbHostEhciIrq[CONTROLLER_ID - kUSB_ControllerEhci0];

#if defined(__GIC_PRIO_BITS)
    GIC_SetPriority((IRQn_Type)irqNumber, USB_HOST_INTERRUPT_PRIORITY);
#else
    NVIC_SetPriority((IRQn_Type)irqNumber, USB_HOST_INTERRUPT_PRIORITY);
#endif
    EnableIRQ((IRQn_Type)irqNumber);
}

static inline void UsbMsd_TaskFn(void)
{
    USB_HostEhciTaskFunction(g_HostHandle);
}

static void USB_HostMsdControlCallback(void *param, uint8_t *data, uint32_t dataLength, usb_status_t status)
{
    usb_msd_instance_t *inst = (usb_msd_instance_t *)param;

    if (inst->runWaitState == kUsbMsdRun_WaitSetInterface)
    {
        inst->runWaitState = kUsbMsdRun_Idle;
        inst->runState     = kUsbMsdRun_Ready;
    }
    s_controlIng    = 0;
    s_controlStatus = status;
}

/*! @brief Host callback: tracks attach/enumerate/detach of the MSD
 * interface, same logic as host_msd_fatfs.c's USB_HostMsdEvent(). */
static usb_status_t UsbMsd_HostEvent(usb_device_handle deviceHandle,
                                      usb_host_configuration_handle configurationHandle,
                                      uint32_t eventCode)
{
    usb_status_t status = kStatus_USB_Success;
    uint32_t eventId = eventCode & 0x0000FFFFU;
    usb_host_configuration_t *configuration;
    uint8_t interfaceIndex;
    usb_host_interface_t *interface;
    uint32_t infoValue = 0U;
    uint8_t id;

    PRINTF("USB MSD host event=0x%08x\r\n", (unsigned)eventCode);

    switch (eventId)
    {
        case kUSB_HostEventAttach:
            configuration = (usb_host_configuration_t *)configurationHandle;
            for (interfaceIndex = 0; interfaceIndex < configuration->interfaceCount; ++interfaceIndex)
            {
                interface = &configuration->interfaceList[interfaceIndex];
                id        = interface->interfaceDesc->bInterfaceClass;
                if (id != USB_HOST_MSD_CLASS_CODE)
                {
                    continue;
                }
                id = interface->interfaceDesc->bInterfaceSubClass;
                if ((id != USB_HOST_MSD_SUBCLASS_CODE_UFI) && (id != USB_HOST_MSD_SUBCLASS_CODE_SCSI))
                {
                    continue;
                }
                id = interface->interfaceDesc->bInterfaceProtocol;
                if (id != USB_HOST_MSD_PROTOCOL_BULK)
                {
                    continue;
                }
                if (s_msdInstance.deviceState == kUsbMsdDev_Idle)
                {
                    s_msdInstance.deviceHandle    = deviceHandle;
                    s_msdInstance.interfaceHandle = interface;
                    s_msdInstance.configHandle    = configurationHandle;
                    return kStatus_USB_Success;
                }
                continue;
            }
            status = kStatus_USB_NotSupported;
            break;

        case kUSB_HostEventNotSupported:
            break;

        case kUSB_HostEventEnumerationDone:
            if (s_msdInstance.configHandle == configurationHandle)
            {
                if ((s_msdInstance.deviceHandle != NULL) && (s_msdInstance.interfaceHandle != NULL))
                {
                    if (s_msdInstance.deviceState == kUsbMsdDev_Idle)
                    {
                        s_msdInstance.deviceState = kUsbMsdDev_Attached;

                        USB_HostHelperGetPeripheralInformation(deviceHandle, kUSB_HostGetDevicePID, &infoValue);
                        PRINTF("USB MSD attached: pid=0x%x ", (unsigned)infoValue);
                        USB_HostHelperGetPeripheralInformation(deviceHandle, kUSB_HostGetDeviceVID, &infoValue);
                        PRINTF("vid=0x%x ", (unsigned)infoValue);
                        USB_HostHelperGetPeripheralInformation(deviceHandle, kUSB_HostGetDeviceAddress, &infoValue);
                        PRINTF("address=%u\r\n", (unsigned)infoValue);
                    }
                    else
                    {
                        status = kStatus_USB_Error;
                    }
                }
            }
            break;

        case kUSB_HostEventDetach:
            if (s_msdInstance.configHandle == configurationHandle)
            {
                g_UsbFatfsClassHandle   = NULL;
                s_msdInstance.configHandle = NULL;
                if (s_msdInstance.deviceState != kUsbMsdDev_Idle)
                {
                    s_msdInstance.deviceState = kUsbMsdDev_Detached;
                }
            }
            break;
        
        case kUSB_HostEventEnumerationFail:
            PRINTF("USB MSD: enumeration failed, status=%u\r\n", (unsigned)(eventCode >> 16U));
            break;

        default:
            break;
    }
    return status;
}

/*! @brief Runs the attach -> set-interface -> ready state machine one
 * step, same logic as host_msd_fatfs.c's USB_HostMsdTask(), minus the
 * FatFs test code (this backend's init() does the FatFs mount itself,
 * once the device reaches kUsbMsdRun_Ready). */
static void UsbMsd_Task(void)
{
    usb_status_t status;

    if (s_msdInstance.deviceState != s_msdInstance.prevDeviceState)
    {
        s_msdInstance.prevDeviceState = s_msdInstance.deviceState;
        switch (s_msdInstance.deviceState)
        {
            case kUsbMsdDev_Idle:
                break;

            case kUsbMsdDev_Attached:
                status = USB_HostMsdInit(s_msdInstance.deviceHandle, &s_msdInstance.classHandle);
                g_UsbFatfsClassHandle = s_msdInstance.classHandle;
                if (status != kStatus_USB_Success)
                {
                    PRINTF("USB host MSD init failed\r\n");
                    return;
                }
                s_msdInstance.runState = kUsbMsdRun_SetInterface;
                break;

            case kUsbMsdDev_Detached:
                s_msdInstance.deviceState = kUsbMsdDev_Idle;
                s_msdInstance.runState    = kUsbMsdRun_Idle;
                USB_HostMsdDeinit(s_msdInstance.deviceHandle, s_msdInstance.classHandle);
                s_msdInstance.classHandle = NULL;
                PRINTF("USB MSD device detached\r\n");
                break;

            default:
                break;
        }
    }

    switch (s_msdInstance.runState)
    {
        case kUsbMsdRun_Idle:
            break;

        case kUsbMsdRun_SetInterface:
            s_msdInstance.runState     = kUsbMsdRun_Idle;
            s_msdInstance.runWaitState = kUsbMsdRun_WaitSetInterface;
            status = USB_HostMsdSetInterface(s_msdInstance.classHandle, s_msdInstance.interfaceHandle, 0,
                                              USB_HostMsdControlCallback, &s_msdInstance);
            if (status != kStatus_USB_Success)
            {
                PRINTF("USB host MSD set interface failed\r\n");
            }
            break;

        case kUsbMsdRun_WaitSetInterface:
        case kUsbMsdRun_Ready:
        default:
            break;
    }
}

/*******************************************************************************
 * Low-level ops: init/open/read/write/close/getFileSize/deinit
 ******************************************************************************/
static storage_status_t UsbMsd_Init(void)
{
    usb_status_t usbStatus;
    uint32_t elapsedMs;

    if (s_mounted)
    {
        return kStorage_Success;
    }

    memset(&s_msdInstance, 0, sizeof(s_msdInstance));

    UsbMsd_ClockInit();

#if ((defined FSL_FEATURE_SOC_SYSMPU_COUNT) && (FSL_FEATURE_SOC_SYSMPU_COUNT))
    SYSMPU_Enable(SYSMPU, 0);
#endif

    usbStatus = USB_HostInit(CONTROLLER_ID, &g_HostHandle, UsbMsd_HostEvent);

    if (usbStatus != kStatus_USB_Success)
    {
        PRINTF("USB host init failed (status=%d)\r\n", (int)usbStatus);
        return kStorage_Error;
    }
    UsbMsd_IsrEnable();

    PRINTF("USB host init done, waiting for MSD device attach...\r\n");

    /* Poll long enough for USB attach debounce and asynchronous enumeration
     * to complete, while continuing to service the EHCI state machine. */
    for (elapsedMs = 0U; elapsedMs < USB_MSD_MOUNT_TIMEOUT_MS; elapsedMs++)
    {
        UsbMsd_TaskFn();
        UsbMsd_Task();
        if (s_msdInstance.runState == kUsbMsdRun_Ready)
        {
            break;
        }
        SDK_DelayAtLeastUs(1000U, SystemCoreClock);
    }

    if (s_msdInstance.runState != kUsbMsdRun_Ready)
    {
        PRINTF("USB MSD device did not attach/enumerate within timeout\r\n");
        return kStorage_Error;
    }

    PRINTF("USB MSD device ready, mounting FatFs...\r\n");

    if (f_mount(&s_fileSystem, s_usbDrive, 0U) != FR_OK)
    {
        PRINTF("USB MSD FatFs mount failed\r\n");
        return kStorage_Error;
    }

    s_mounted = true;
    return kStorage_Success;
}

static storage_status_t UsbMsd_PrefixPath(const char *path, char *out, size_t outSize)
{
    int n;

    if ((path == NULL) || (out == NULL))
    {
        return kStorage_InvalidParameter;
    }

    n = snprintf(out, outSize, "%c:%s", (char)(USBDISK + '0'), path);
    if ((n < 0) || ((size_t)n >= outSize))
    {
        return kStorage_InvalidParameter;
    }
    return kStorage_Success;
}

static storage_status_t UsbMsd_Open(const char *path, storage_open_mode_t mode, storage_file_t **outFile)
{
    BYTE ffMode;
    storage_file_t *file;
    char prefixed[64];
    storage_status_t ret;

    if ((path == NULL) || (outFile == NULL))
    {
        return kStorage_InvalidParameter;
    }
    if (!s_mounted)
    {
        return kStorage_NotMounted;
    }

    ret = UsbMsd_PrefixPath(path, prefixed, sizeof(prefixed));
    if (ret != kStorage_Success)
    {
        return ret;
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

    if (f_open(&file->fil, prefixed, ffMode) != FR_OK)
    {
        free(file);
        return kStorage_OpenFailed;
    }

    *outFile = file;
    return kStorage_Success;
}

static storage_status_t UsbMsd_Read(storage_file_t *file, uint8_t *buf, size_t bufSize, size_t *bytesRead)
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

static storage_status_t UsbMsd_Write(storage_file_t *file, const uint8_t *buf, size_t size, size_t *bytesWritten)
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

static storage_status_t UsbMsd_Close(storage_file_t *file)
{
    FRESULT res;

    if (file == NULL)
    {
        return kStorage_InvalidParameter;
    }

    res = f_close(&file->fil);
    free(file);

    return (res == FR_OK) ? kStorage_Success : kStorage_Error;
}

static storage_status_t UsbMsd_GetFileSize(const char *path, size_t *size)
{
    FIL file;
    char prefixed[64];
    storage_status_t ret;

    if ((path == NULL) || (size == NULL))
    {
        return kStorage_InvalidParameter;
    }
    if (!s_mounted)
    {
        return kStorage_NotMounted;
    }

    ret = UsbMsd_PrefixPath(path, prefixed, sizeof(prefixed));
    if (ret != kStorage_Success)
    {
        return ret;
    }

    if (f_open(&file, prefixed, FA_READ) != FR_OK)
    {
        return kStorage_OpenFailed;
    }

    *size = (size_t)f_size(&file);
    (void)f_close(&file);

    return kStorage_Success;
}

static storage_status_t UsbMsd_Deinit(void)
{
    /* No explicit unmount API is used by this backend; mark state so
     * init() can be called again (e.g. after re-plugging the drive). */
    s_mounted = false;
    return kStorage_Success;
}

/*******************************************************************************
 * High-level workflow: runFileTest (read -> write -> verify)
 *
 * Mirrors storage_fatfs_sd.c's FatFsSd_RunFileTest(): built purely on top
 * of this file's own open()/read()/write()/close()/getFileSize() - never
 * touches FatFs/USB APIs directly.
 ******************************************************************************/

typedef struct
{
    uint8_t *data;
    size_t size;
    void *raw;
} usb_msd_buf_t;

static void UsbMsd_FreeBuf(usb_msd_buf_t *buf)
{
    free(buf->raw);
    buf->raw  = NULL;
    buf->data = NULL;
    buf->size = 0U;
}

static storage_status_t UsbMsd_ReadWholeFile(const char *path, size_t maxFileSize, usb_msd_buf_t *buf)
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

    status = UsbMsd_GetFileSize(path, &fsize);
    if (status != kStorage_Success)
    {
        return status;
    }

    if (fsize == 0U)
    {
        return kStorage_Success;
    }
    if (fsize > maxFileSize)
    {
        return kStorage_FileTooLarge;
    }

    status = UsbMsd_Open(path, kStorage_OpenRead, &file);
    if (status != kStorage_Success)
    {
        return status;
    }

    allocSize = ((fsize + USB_MSD_BUF_ALIGN - 1U) & ~((size_t)USB_MSD_BUF_ALIGN - 1U)) + USB_MSD_BUF_ALIGN;

    raw = malloc(allocSize);
    if (raw == NULL)
    {
        (void)UsbMsd_Close(file);
        return kStorage_OutOfMemory;
    }
    aligned = (uint8_t *)(((uintptr_t)raw + USB_MSD_BUF_ALIGN - 1U) & ~((uintptr_t)USB_MSD_BUF_ALIGN - 1U));

    status = UsbMsd_Read(file, aligned, fsize, &br);
    (void)UsbMsd_Close(file);

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

static storage_status_t UsbMsd_WriteWholeFile(const char *path, const uint8_t *data, size_t size)
{
    storage_file_t *file = NULL;
    storage_status_t status;
    size_t written = 0U;
    storage_status_t closeStatus;

    status = UsbMsd_Open(path, kStorage_OpenWriteCreate, &file);
    if (status != kStorage_Success)
    {
        return status;
    }

    status      = UsbMsd_Write(file, data, size, &written);
    closeStatus = UsbMsd_Close(file);

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

#define USB_MSD_VERIFY_CHUNK (512U)

static storage_status_t UsbMsd_VerifyFile(const char *path, const uint8_t *expected, size_t expectedSize,
                                           bool *matched)
{
    SDK_ALIGN(static uint8_t s_verifyChunk[USB_MSD_VERIFY_CHUNK], USB_MSD_BUF_ALIGN);
    storage_file_t *file = NULL;
    storage_status_t status;
    size_t offset = 0U;

    *matched = false;

    status = UsbMsd_Open(path, kStorage_OpenRead, &file);
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

        status = UsbMsd_Read(file, s_verifyChunk, want, &got);
        if ((status != kStorage_Success) || (got != want))
        {
            (void)UsbMsd_Close(file);
            return (status == kStorage_Success) ? kStorage_ReadFailed : status;
        }

        if (memcmp(s_verifyChunk, expected + offset, want) != 0)
        {
            (void)UsbMsd_Close(file);
            return kStorage_Success;
        }

        offset += want;
    }

    (void)UsbMsd_Close(file);

    {
        size_t actualSize = 0U;
        status             = UsbMsd_GetFileSize(path, &actualSize);
        if ((status == kStorage_Success) && (actualSize == expectedSize))
        {
            *matched = true;
        }
    }

    return kStorage_Success;
}

static storage_status_t UsbMsd_RunFileTest(const char *srcPath, const char *dstPath, size_t maxFileSize,
                                            bool *matched)
{
    usb_msd_buf_t src = {0};
    storage_status_t status;

    if (matched != NULL)
    {
        *matched = false;
    }
    if ((srcPath == NULL) || (dstPath == NULL))
    {
        return kStorage_InvalidParameter;
    }

    status = UsbMsd_ReadWholeFile(srcPath, maxFileSize, &src);
    if (status != kStorage_Success)
    {
        PRINTF("USB MSD read failed (open/size limit/malloc/read), status=%d\r\n", (int)status);
        return status;
    }

    PRINTF("USB MSD: read %u bytes from %s\r\n", (unsigned)src.size, srcPath);

    if ((src.data == NULL) || (src.size == 0U))
    {
        PRINTF("USB MSD: no data read from file to write\r\n");
        UsbMsd_FreeBuf(&src);
        return kStorage_Success;
    }

    status = UsbMsd_WriteWholeFile(dstPath, src.data, src.size);
    if (status != kStorage_Success)
    {
        PRINTF("USB MSD write failed, status=%d\r\n", (int)status);
        UsbMsd_FreeBuf(&src);
        return status;
    }
    PRINTF("USB MSD: wrote %u bytes to %s\r\n", (unsigned)src.size, dstPath);

    bool verified = false;
    status         = UsbMsd_VerifyFile(dstPath, src.data, src.size, &verified);
    if (status != kStorage_Success)
    {
        PRINTF("USB MSD verify read failed, status=%d\r\n", (int)status);
        UsbMsd_FreeBuf(&src);
        return status;
    }

    if (verified)
    {
        PRINTF("USB MSD verify OK: written file matches original data (%u bytes)\r\n", (unsigned)src.size);
        if (matched != NULL)
        {
            *matched = true;
        }
    }
    else
    {
        PRINTF("USB MSD verify FAILED: written file does not match original data\r\n");
    }

    UsbMsd_FreeBuf(&src);
    return kStorage_Success;
}

/*******************************************************************************
 * Registration
 ******************************************************************************/

static const storage_ops_t s_usbMsdOps = {
    .init        = UsbMsd_Init,
    .open        = UsbMsd_Open,
    .read        = UsbMsd_Read,
    .write       = UsbMsd_Write,
    .close       = UsbMsd_Close,
    .getFileSize = UsbMsd_GetFileSize,
    .deinit      = UsbMsd_Deinit,
    .runFileTest = UsbMsd_RunFileTest,
};

/*!
 * @brief Registers this backend's ops table unconditionally under
 * (kStorageFs_FatFs, kStorageDevice_Usb0). Mirrors storage_fatfs_sd.c /
 * storage_fatfs_ramdisk.c / storage_littlefs_qspi.c: registration always
 * succeeds here, and Init() (which is where the actual USB host bring-up
 * and device-present check happens) is deferred to the caller.
 */
static void __attribute__((constructor))
STORAGE_IF_RegisterUsbMsd(void)
{
    STORAGE_RegisterOps(kStorageFs_FatFs, kStorageDevice_Usb0, &s_usbMsdOps);
}
