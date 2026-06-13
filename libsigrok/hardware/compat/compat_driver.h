/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2025 Compat Layer Authors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBSIGROK_COMPAT_DRIVER_H
#define LIBSIGROK_COMPAT_DRIVER_H

/**
 * @file
 *
 * Compat driver adaptation layer for wrapping standard sigrok driver
 * callbacks into PXView-compatible sr_dev_driver structs.
 *
 * Usage pattern for each compat driver:
 *
 * 1. Store a static pointer to the driver's sr_dev_driver struct
 * 2. Define thin wrapper functions that adapt PXView signatures to standard
 * 3. Declare the PXView-compatible sr_dev_driver struct
 *
 * Example:
 *
 *   static struct sr_dev_driver *my_driver_ptr;
 *
 *   // Wrapper: PXView init(sr_ctx) -> standard init(driver, sr_ctx)
 *   static int my_init(struct sr_context *sr_ctx) {
 *       return std_init(my_driver_ptr, sr_ctx);
 *   }
 *
 *   // Wrapper: PXView cleanup(void) -> standard cleanup(driver)
 *   static int my_cleanup(void) {
 *       return std_cleanup(my_driver_ptr);
 *   }
 *
 *   // Wrapper: PXView scan(options) -> standard scan(driver, options)
 *   static GSList *my_scan(GSList *options) {
 *       return my_driver_scan(my_driver_ptr, options);
 *   }
 *
 *   // Wrapper: PXView config_get(id,data,sdi,ch,cg) -> standard(key,data,sdi,cg)
 *   static int my_config_get(int id, GVariant **data,
 *       const struct sr_dev_inst *sdi, const struct sr_channel *ch,
 *       const struct sr_channel_group *cg) {
 *       (void)ch;
 *       return my_driver_config_get((uint32_t)id, data, sdi, cg);
 *   }
 *
 *   // Similarly for config_set, config_list, dev_acquisition_start/stop...
 *
 *   struct sr_dev_driver my_driver_driver_info = {
 *       .driver_type = DRIVER_TYPE_HARDWARE,
 *       .init = my_init,
 *       .cleanup = my_cleanup,
 *       .scan = my_scan,
 *       .dev_mode_list = compat_dev_mode_list_default,
 *       .config_get = my_config_get,
 *       .config_set = my_config_set,
 *       .config_list = my_config_list,
 *       .dev_open = my_driver_dev_open,
 *       .dev_close = my_driver_dev_close,
 *       .dev_destroy = compat_dev_destroy_default,
 *       .dev_status_get = compat_dev_status_get_default,
 *       .dev_acquisition_start = my_dev_acquisition_start,
 *       .dev_acquisition_stop = my_dev_acquisition_stop,
 *       .priv = NULL,
 *       .name = "my-driver",
 *       .longname = "My Driver",
 *       .api_version = 1,
 *   };
 *
 *   // In driver init, set the static pointer:
 *   static int my_init(struct sr_context *sr_ctx) {
 *       my_driver_ptr = &my_driver_driver_info;
 *       return std_init(my_driver_ptr, sr_ctx);
 *   }
 */

#include <glib.h>

/*
 * drv_context compat - standard sigrok stores driver private data
 * in driver->context (which maps to driver->priv in PXView).
 * This struct holds the common fields that standard sigrok's
 * drv_context provides.
 */
struct compat_drv_context {
    struct sr_context *sr_ctx;
    GSList *instances;
};

/* Helper to get compat_drv_context from driver's priv field */
#define COMPAT_DRV_CTX(driver) ((struct compat_drv_context *)(driver)->priv)

/* Default dev_mode_list for compat drivers - returns NULL (no mode list) */
static inline const GSList *compat_dev_mode_list_default(const struct sr_dev_inst *sdi)
{
    (void)sdi;
    return NULL;
}

/* Default dev_destroy for compat drivers */
static inline int compat_dev_destroy_default(struct sr_dev_inst *sdi)
{
    (void)sdi;
    return SR_OK;
}

/* Default dev_status_get for compat drivers */
static inline int compat_dev_status_get_default(const struct sr_dev_inst *sdi,
    struct sr_status *status, gboolean prg)
{
    (void)sdi; (void)status; (void)prg;
    memset(status, 0, sizeof(*status));
    return SR_ERR_NA;
}

/*
 * std_scan_complete_compat: sets sdi->driver and adds to instances list.
 * Standard sigrok drivers call std_scan_complete() at the end of scan()
 * to register discovered devices. This provides the same functionality.
 */
static inline GSList *std_scan_complete_compat(struct sr_dev_driver *driver,
    GSList *devices)
{
    struct compat_drv_context *drvc;
    GSList *l;
    struct sr_dev_inst *sdi;

    if (!driver || !driver->priv)
        return devices;

    drvc = driver->priv;
    for (l = devices; l; l = l->next) {
        sdi = l->data;
        sdi->driver = driver;
        drvc->instances = g_slist_append(drvc->instances, sdi);
    }
    return devices;
}

/*
 * Wrapper init: adapts PXView's init(sr_ctx) to standard's init(driver, sr_ctx).
 * Each driver needs its own init wrapper because it needs the driver pointer.
 * Use this pattern:
 *   static struct sr_dev_driver *my_driver_ptr;
 *   static int my_init(struct sr_context *sr_ctx) {
 *       return std_init_compat(my_driver_ptr, sr_ctx);
 *   }
 */
static inline int std_init_compat(struct sr_dev_driver *driver,
    struct sr_context *sr_ctx)
{
    struct compat_drv_context *drvc;
    drvc = g_malloc0(sizeof(struct compat_drv_context));
    drvc->sr_ctx = sr_ctx;
    drvc->instances = NULL;
    driver->priv = drvc;
    return SR_OK;
}

/*
 * Wrapper cleanup: adapts PXView's cleanup(void) to standard's cleanup(driver).
 * Each driver needs its own cleanup wrapper because it needs the driver pointer.
 * Use this pattern:
 *   static int my_cleanup(void) {
 *       return std_cleanup_compat(my_driver_ptr);
 *   }
 */
static inline int std_cleanup_compat(struct sr_dev_driver *driver)
{
    struct compat_drv_context *drvc;
    if (!driver || !driver->priv)
        return SR_ERR_ARG;
    drvc = driver->priv;
    g_slist_free_full(drvc->instances, (GDestroyNotify)sr_dev_inst_free);
    drvc->instances = NULL;
    g_free(drvc);
    driver->priv = NULL;
    return SR_OK;
}

/*
 * Wrapper scan: adapts PXView's scan(options) to standard's scan(driver, options).
 * Each driver needs its own scan wrapper because it needs the driver pointer.
 * Use this pattern:
 *   static GSList *my_scan(GSList *options) {
 *       return my_driver_scan(my_driver_ptr, options);
 *   }
 */

/* std_dev_list_compat: returns the driver's instance list */
static inline GSList *std_dev_list_compat(const struct sr_dev_driver *driver)
{
    struct compat_drv_context *drvc;
    if (!driver || !driver->priv)
        return NULL;
    drvc = driver->priv;
    return drvc->instances;
}

/* std_dev_clear_compat: clears all device instances */
static inline int std_dev_clear_compat(const struct sr_dev_driver *driver)
{
    struct compat_drv_context *drvc;
    if (!driver || !driver->priv)
        return SR_ERR_ARG;
    drvc = driver->priv;
    g_slist_free_full(drvc->instances, (GDestroyNotify)sr_dev_inst_free);
    drvc->instances = NULL;
    return SR_OK;
}

/*
 * Wrapper for config_get: adapts PXView's config_get(id,data,sdi,ch,cg)
 * to standard's config_get(key,data,sdi,cg) by dropping the ch parameter
 * and casting int id to uint32_t key.
 *
 * Use this pattern:
 *   static int my_config_get(int id, GVariant **data,
 *       const struct sr_dev_inst *sdi, const struct sr_channel *ch,
 *       const struct sr_channel_group *cg) {
 *       (void)ch;
 *       return my_driver_config_get((uint32_t)id, data, sdi, cg);
 *   }
 */

/*
 * Wrapper for config_set: adapts PXView's config_set(id,data,sdi,ch,cg)
 * to standard's config_set(key,data,sdi,cg) by dropping the ch parameter,
 * casting int id to uint32_t key, and making sdi const.
 *
 * Use this pattern:
 *   static int my_config_set(int id, GVariant *data,
 *       struct sr_dev_inst *sdi, struct sr_channel *ch,
 *       struct sr_channel_group *cg) {
 *       (void)ch;
 *       return my_driver_config_set((uint32_t)id, data, sdi, cg);
 *   }
 */

/*
 * Wrapper for config_list: adapts PXView's config_list(info_id,data,sdi,cg)
 * to standard's config_list(key,data,sdi,cg) by casting int info_id to uint32_t key.
 *
 * Use this pattern:
 *   static int my_config_list(int info_id, GVariant **data,
 *       const struct sr_dev_inst *sdi, const struct sr_channel_group *cg) {
 *       return my_driver_config_list((uint32_t)info_id, data, sdi, cg);
 *   }
 */

/*
 * Wrapper for dev_acquisition_start: adapts PXView's
 * dev_acquisition_start(sdi, cb_data) to standard's
 * dev_acquisition_start(sdi) by dropping cb_data.
 *
 * Use this pattern:
 *   static int my_dev_acquisition_start(struct sr_dev_inst *sdi,
 *       void *cb_data) {
 *       (void)cb_data;
 *       return my_driver_dev_acquisition_start(sdi);
 *   }
 */

/*
 * Wrapper for dev_acquisition_stop: adapts PXView's
 * dev_acquisition_stop(const sdi, cb_data) to standard's
 * dev_acquisition_stop(sdi) by dropping cb_data and making sdi non-const.
 *
 * Use this pattern:
 *   static int my_dev_acquisition_stop(const struct sr_dev_inst *sdi,
 *       void *cb_data) {
 *       (void)cb_data;
 *       return my_driver_dev_acquisition_stop((struct sr_dev_inst *)sdi);
 *   }
 */

#endif
