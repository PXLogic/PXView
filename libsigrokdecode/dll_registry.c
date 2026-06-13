#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <glib.h>
#include "dll_registry.h"
#include "log.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

GSList *c_dll_registry = NULL;

struct srd_c_dll_entry *srd_c_dll_registry_add(const char *file_path,
    void *handle, int api_version, enum srd_c_dll_status status,
    const char *decoder_id, struct srd_c_decoder *c_dec)
{
    if (!file_path)
        return NULL;

    struct srd_c_dll_entry *existing = srd_c_dll_registry_find_by_path(file_path);
    if (existing) {
        srd_dbg("DLL registry: duplicate path '%s', skipping.", file_path);
        return NULL;
    }

    struct srd_c_dll_entry *entry = g_malloc0(sizeof(struct srd_c_dll_entry));
    entry->file_path = g_strdup(file_path);
    entry->handle = handle;
    entry->api_version = api_version;
    entry->status = status;
    entry->decoder_id = decoder_id ? g_strdup(decoder_id) : NULL;
    entry->c_dec = c_dec;
    entry->load_time = time(NULL);

    c_dll_registry = g_slist_append(c_dll_registry, entry);

    srd_dbg("DLL registry: added '%s' (decoder_id=%s, status=%d).",
        file_path, decoder_id ? decoder_id : "N/A", status);

    return entry;
}

struct srd_c_dll_entry *srd_c_dll_registry_find_by_path(const char *file_path)
{
    if (!file_path)
        return NULL;

    GSList *l;
    for (l = c_dll_registry; l; l = l->next) {
        struct srd_c_dll_entry *entry = l->data;
        if (g_strcmp0(entry->file_path, file_path) == 0)
            return entry;
    }
    return NULL;
}

struct srd_c_dll_entry *srd_c_dll_registry_find_by_id(const char *decoder_id)
{
    if (!decoder_id)
        return NULL;

    GSList *l;
    for (l = c_dll_registry; l; l = l->next) {
        struct srd_c_dll_entry *entry = l->data;
        if (entry->decoder_id && g_strcmp0(entry->decoder_id, decoder_id) == 0)
            return entry;
    }
    return NULL;
}

int srd_c_dll_registry_remove(const char *decoder_id)
{
    if (!decoder_id)
        return SRD_ERR_ARG;

    GSList *l;
    for (l = c_dll_registry; l; l = l->next) {
        struct srd_c_dll_entry *entry = l->data;
        if (entry->decoder_id && g_strcmp0(entry->decoder_id, decoder_id) == 0) {
            c_dll_registry = g_slist_remove(c_dll_registry, entry);
            g_free(entry->file_path);
            g_free(entry->decoder_id);
            g_free(entry);
            srd_dbg("DLL registry: removed '%s'.", decoder_id);
            return SRD_OK;
        }
    }
    return SRD_ERR_ARG;
}

void srd_c_dll_registry_cleanup(void)
{
    GSList *l;
    for (l = c_dll_registry; l; l = l->next) {
        struct srd_c_dll_entry *entry = l->data;
        if (entry->status == SRD_C_DLL_LOADED && entry->handle) {
#ifdef _WIN32
            FreeLibrary((HMODULE)entry->handle);
            srd_dbg("DLL registry: FreeLibrary '%s'.", entry->file_path);
#else
            dlclose(entry->handle);
            srd_dbg("DLL registry: dlclose '%s'.", entry->file_path);
#endif
        }
        g_free(entry->file_path);
        g_free(entry->decoder_id);
        g_free(entry);
    }
    g_slist_free(c_dll_registry);
    c_dll_registry = NULL;
    srd_dbg("DLL registry: cleanup complete.");
}

int srd_c_dll_registry_count(void)
{
    return g_slist_length(c_dll_registry);
}
