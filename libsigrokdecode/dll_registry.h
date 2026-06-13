#ifndef DLL_REGISTRY_H
#define DLL_REGISTRY_H

#include <glib.h>
#include "libsigrokdecode.h"

#ifdef __cplusplus
extern "C" {
#endif

enum srd_c_dll_status {
    SRD_C_DLL_LOADED,
    SRD_C_DLL_VERSION_MISMATCH,
    SRD_C_DLL_ENTRY_MISSING,
    SRD_C_DLL_LOAD_FAILED,
    SRD_C_DLL_UNLOADED,
};

struct srd_c_dll_entry {
    char *file_path;
    void *handle;
    int api_version;
    enum srd_c_dll_status status;
    char *decoder_id;
    struct srd_c_decoder *c_dec;
    time_t load_time;
};

extern GSList *c_dll_registry;

struct srd_c_dll_entry *srd_c_dll_registry_add(const char *file_path,
    void *handle, int api_version, enum srd_c_dll_status status,
    const char *decoder_id, struct srd_c_decoder *c_dec);
struct srd_c_dll_entry *srd_c_dll_registry_find_by_path(const char *file_path);
struct srd_c_dll_entry *srd_c_dll_registry_find_by_id(const char *decoder_id);
int srd_c_dll_registry_remove(const char *decoder_id);
void srd_c_dll_registry_cleanup(void);
int srd_c_dll_registry_count(void);

#ifdef __cplusplus
}
#endif

#endif
