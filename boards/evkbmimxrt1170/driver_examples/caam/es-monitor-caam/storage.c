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

/*******************************************************************************
 * Registry storage
 ******************************************************************************/

/*! @brief One row of the registry: ops pointer for a given (fs, device) pair. */
static const storage_ops_t *s_registry[kStorageFs_Count][kStorageDevice_Count];

/*******************************************************************************
 * Backend probe function declarations
 *
 * Each backend .c file implements one of these. Add a new line here (and a
 * call to it from STORAGE_ProbeAll()) when adding a new backend - nothing
 * else in this file needs to change.
 ******************************************************************************/

extern void STORAGE_FatFsSd_Probe(void);
extern void STORAGE_LittleFsQspi_Probe(void);
extern void STORAGE_FatFsRamDisk_Probe(void);

/*******************************************************************************
 * Code
 ******************************************************************************/

void STORAGE_RegisterOps(storage_fs_type_t fs, storage_device_type_t device, const storage_ops_t *ops)
{
    if ((fs >= kStorageFs_Count) || (device >= kStorageDevice_Count) || (ops == NULL))
    {
        return;
    }

    s_registry[fs][device] = ops;
}

const storage_ops_t *STORAGE_GetOps(storage_fs_type_t fs, storage_device_type_t device)
{
    if ((fs >= kStorageFs_Count) || (device >= kStorageDevice_Count))
    {
        return NULL;
    }

    return s_registry[fs][device];
}

void STORAGE_ProbeAll(void)
{
    /* Each probe function brings up its own hardware/dependencies and
     * self-registers via STORAGE_RegisterOps() on success. A probe that
     * fails (e.g. no SD card inserted) simply does not register; the
     * corresponding STORAGE_GetOps() call will then return NULL. */
    STORAGE_FatFsSd_Probe();
    STORAGE_LittleFsQspi_Probe();
    STORAGE_FatFsRamDisk_Probe();

    /* Add further backends' probe calls here. */

}

