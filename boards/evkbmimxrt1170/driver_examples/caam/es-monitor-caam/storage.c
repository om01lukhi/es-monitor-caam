/*
 * Copyright 2024 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Generic storage registry: a small, static, Linux-"probe"-style registry
 * that decouples the application from backend-specific function names
 * (e.g. STORAGE_GetFatFsSdOps()). Each backend implements its own probe
 * function and registers its ops table here; the application looks ops up
 * by (fs, device) via STORAGE_GetOps().
 *
 * This file knows the *names* of each backend's probe function (so it can
 * call them all from STORAGE_ProbeAll()), but it does not know, and does
 * not need to know, anything about how a given backend actually talks to
 * its hardware/filesystem - that remains entirely inside the backend's own
 * .c file (e.g. storage_fatfs_sd.c).
 */

#include "storage.h"
#include "fsl_debug_console.h"

/*******************************************************************************
 * Registry storage
 ******************************************************************************/

/*! @brief One row of the registry: ops pointer for a given (fs, device) pair. */
static const storage_ops_t *s_registry[kStorageFs_Count][kStorageDevice_Count];

/*******************************************************************************
 * Code
 ******************************************************************************/

void storage_register_ops(storage_fs_type_t fs, storage_device_type_t device, const storage_ops_t *ops)
{
    if ((fs >= kStorageFs_Count) || (device >= kStorageDevice_Count) || (ops == NULL))
        return;

    if (s_registry[fs][device] != NULL) {
        PRINTF("WARNING: Storage ops already assigned for FS %d, Device %d. Assignment skipped.\n", fs, device);
        return;
    }

    s_registry[fs][device] = ops;
}

const storage_ops_t *storage_get_ops(storage_fs_type_t fs, storage_device_type_t device)
{
    if ((fs >= kStorageFs_Count) || (device >= kStorageDevice_Count))
        return NULL;

    return s_registry[fs][device];
}
