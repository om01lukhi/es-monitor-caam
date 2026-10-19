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
    kStorage_FileTooLarge      /*!< File exceeds caller-provided size limit. */
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
 * @brief Table of function pointers implementing the storage operations.
 *
 * A backend provides a (typically static, singleton) instance of this
 * struct. The application obtains a pointer to it once (e.g. via
 * STORAGE_GetFatFsSdOps()) and then calls through the function pointers for
 * every operation, e.g. storage->open(...), storage->read(...).
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

    /*!
     * @brief Runs the "read srcPath -> write dstPath -> verify" workflow
     * entirely inside the backend, using only open()/read()/write()/close()
     * internally (every read and every write is wrapped by its own
     * open()/close() pair). Allocates and frees its own working buffer;
     * never leaks a file handle or allocation on any failure path.
     *
     * @param srcPath Path of the file to read (e.g. "/T64K.BIN").
     * @param dstPath Path of the file to write a copy to (e.g. "/test.bin").
     * @param maxFileSize Largest file size, in bytes, this call will accept
     *                    (bounds the internal allocation).
     * @param matched Optional, if non-NULL receives true if dstPath was
     *                verified to match srcPath byte-for-byte, false
     *                otherwise (including when the test could not run).
     */
    storage_status_t (*runFileTest)(const char *srcPath, const char *dstPath, size_t maxFileSize, bool *matched);
} storage_ops_t;


/*!
 * @brief Returns the FatFs/SD-card backend operations table.
 *
 * This is a simple static table - there is no dynamic registration beyond
 * what this example needs. The application calls this once to obtain the
 * table, then calls ops->init() before using any other operation.
 */
const storage_ops_t *STORAGE_GetFatFsSdOps(void);

#endif /* STORAGE_H_ */
