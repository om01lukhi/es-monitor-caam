/*
 * Copyright 2024 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef STORAGE_H_
#define STORAGE_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>


/*******************************************************************************
 * APIs
 ******************************************************************************/

/*! @brief Common return/error codes used by the storage API. */
typedef enum _storage_status
{
    kStorage_Success = 0,     /*!< Operation completed successfully. */
    kStorage_Error,            /*!< Unspecified/generic failure. */
    kStorage_NotMounted,       /*!< Backend not initialized/mounted. */
    kStorage_OpenFailed,       /*!< File could not be opened. */
    kStorage_ReadFailed,       /*!< Read error, or short read. */
    kStorage_WriteFailed,      /*!< Write error, or short write. */
    kStorage_InvalidParameter, /*!< NULL pointer or otherwise invalid argument. */
    kStorage_OutOfMemory,      /*!< Allocation failure. */
    kStorage_FileTooLarge,     /*!< File exceeds caller-provided size limit. */
    kStorage_DeviceNotFound    /*!< Requested (fs, device) combination was never
                                 *   probed/registered, or probing it failed. */
} storage_status_t;

/*! @brief File open modes supported by the storage API. */
typedef enum _storage_open_mode
{
    kStorage_OpenRead = 0,  /*!< Open existing file for reading. */
    kStorage_OpenWriteCreate /*!< Create (or truncate) file for writing. */
} storage_open_mode_t;

/*! @brief Opaque file handle. Backend-defined, application must not peek inside. */
typedef struct storage_file storage_file_t;

/*!
 * @brief Filesystem types the storage API can be backed by.
 *
 * This is intentionally a flat enum rather than a string/name, so lookups
 * are O(1) table indexing instead of string comparisons. Add new values
 * here as new filesystem backends are implemented; existing backends are
 * unaffected.
 */
typedef enum _storage_fs_type
{
    kStorageFs_FatFs = 0, /*!< FatFs (middleware/fatfs). */
    kStorageFs_LittleFs,   /*!< littlefs (middleware/littlefs). */
    kStorageFs_Count       /*!< Not a real value; number of fs types defined above. */
} storage_fs_type_t;


/*!
 * @brief Physical/logical device types the storage API can target.
 *
 * Only one device is implemented by this example (the on-board SD card
 * over USDHC), but the enum leaves room to add more (e.g. a second SD
 * slot, SPI NOR flash, eMMC) without changing the lookup API.
 */
typedef enum _storage_device_type
{
    kStorageDevice_Sd0 = 0,  /*!< First/only SD card slot (USDHC + fsl_sd). */
    kStorageDevice_QspiNor0, /*!< On-board QSPI/FlexSPI NOR flash. */
    kStorageDevice_RamDisk0, /*!< In-memory RAM disk (no real hardware). */
    kStorageDevice_Usb0,     /*!< First/only USB host MSD (EHCI + usb_host_msd). */
    kStorageDevice_Count     /*!< Not a real value; number of device types defined above. */

} storage_device_type_t;




/*!
 * @brief Table of function pointers implementing the storage operations.
 *
 * A backend provides a (typically static, singleton) instance of this
 * struct and registers it (see STORAGE_RegisterOps()) for one
 * (storage_fs_type_t, storage_device_type_t) combination. The application
 * looks the table up once via STORAGE_GetOps() and then calls through the
 * function pointers for every operation, e.g. storage->open(...).
 */
typedef struct _storage_ops
{
    /*!
     * @brief Initializes/mounts the backend. Must be called once before any
     * other operation. Safe to call again after deinit().
     */
    storage_status_t (*init)(void);

    /*!
     * @brief Opens a file.
     *
     * @param path   Null-terminated path.
     * @param mode   kStorage_OpenRead or kStorage_OpenWriteCreate.
     * @param outFile On success, receives a newly allocated handle. Caller
     *               must close() it. Left untouched on failure.
     */
    storage_status_t (*open)(const char *path, storage_open_mode_t mode, storage_file_t **outFile);

    /*!
     * @brief Reads up to bufSize bytes from file into buf.
     *
     * @param bytesRead Optional, if non-NULL receives the actual number of
     *                  bytes read (may be less than bufSize at EOF).
     */
    storage_status_t (*read)(storage_file_t *file, uint8_t *buf, size_t bufSize, size_t *bytesRead);

    /*!
     * @brief Writes size bytes from buf to file. Fails (kStorage_WriteFailed)
     * on short write.
     *
     * @param bytesWritten Optional, if non-NULL receives the actual number of
     *                     bytes written.
     */
    storage_status_t (*write)(storage_file_t *file, const uint8_t *buf, size_t size, size_t *bytesWritten);

    /*!
     * @brief Closes/commits and frees a file handle. Safe to call with a
     * handle obtained from a successful open() exactly once; must not be
     * used again afterward.
     */
    storage_status_t (*close)(storage_file_t *file);

    /*!
     * @brief Looks up size, in bytes, of an existing file, without needing
     * the caller to open() it first.
     */
    storage_status_t (*getFileSize)(const char *path, size_t *size);

    /*!
     * @brief Releases/unmounts anything init() acquired. Optional - may be
     * NULL if the backend has nothing to release.
     */
    storage_status_t (*deinit)(void);

} storage_ops_t;

/*******************************************************************************
 * Registry API
 *
 * A small, static registry: each backend registers its ops table for a
 * given (fs, device) key by defining a constructor function (see the
 * __attribute__((constructor)) functions at the bottom of each backend's
 * .c file), which runs automatically before main() and calls
 * STORAGE_RegisterOps(). The application then looks up ops via
 * STORAGE_GetOps() and never needs to know backend-specific function names.
 ******************************************************************************/


/*!
 * @brief Registers a backend's ops table for a given (fs, device) pair.
 *
 * Called by a backend's own probe function after it has verified its
 * hardware/dependencies are present and ready. Overwrites any previous
 * registration for the same (fs, device) pair.
 *
 * @param fs     Filesystem type this ops table implements.
 * @param device Device type this ops table talks to.
 * @param ops    Pointer to a backend-owned, static storage_ops_t. Must
 *               remain valid for the lifetime of the application; the
 *               registry only stores the pointer, it does not copy or
 *               free it.
 */
void STORAGE_RegisterOps(storage_fs_type_t fs, storage_device_type_t device, const storage_ops_t *ops);

/*!
 * @brief Looks up a previously registered ops table.

 *
 * @param fs     Filesystem type being requested.
 * @param device Device type being requested.
 * @return Pointer to the registered storage_ops_t, or NULL if that (fs,
 *         device) combination was never registered (not probed, or probe
 *         failed). Callers must check for NULL before using the result.
 */
const storage_ops_t *STORAGE_GetOps(storage_fs_type_t fs, storage_device_type_t device);

#endif /* STORAGE_H_ */
