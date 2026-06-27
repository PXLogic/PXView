/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2010 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2012 Bert Vermeulen <bert@biot.com>
 * Copyright (C) 2019 DreamSourceLab <support@dreamsourcelab.com>
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

#include "config.h"
#include "libsigrokdecode-internal.h"
#include "libsigrokdecode.h"
#include "log.h"
#include "dll_registry.h"
#include <glib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/**
 * @file
 *
 * Listing, loading, unloading, and handling protocol decoders.
 */

/**
 * @defgroup grp_decoder Protocol decoders
 *
 * Handling protocol decoders.
 *
 * @{
 */

/** @cond PRIVATE */

/*
        The list of loaded protocol decoders.
        Is srd_decoder* type
*/
static GSList* pd_list = NULL;

/* srd.c */
extern SRD_PRIV GSList* searchpaths;

/* session.c */
extern SRD_PRIV GSList* sessions;
extern SRD_PRIV int max_session_id;

/* module_sigrokdecode.c */
extern SRD_PRIV PyObject* mod_sigrokdecode;

/** @endcond */

static gboolean srd_check_init(void)
{
    if (max_session_id < 0) {
        srd_err("Library is not initialized.");
        return FALSE;
    } else
        return TRUE;
}

/**
 * Returns the list of loaded protocol decoders.
 *
 * This is a GSList of pointers to struct srd_decoder items.
 *
 * @return List of decoders, NULL if none are supported or loaded.
 *
 * @since 0.2.0
 */
SRD_API const GSList* srd_decoder_list(void)
{
    return pd_list;
}

/**
 * Get the decoder with the specified ID.
 *
 * @param id The ID string of the decoder to return.
 *
 * @return The decoder with the specified ID, or NULL if not found.
 *
 * @since 0.1.0
 */
SRD_API struct srd_decoder* srd_decoder_get_by_id(const char* id)
{
    GSList* l;
    struct srd_decoder* dec;

    for (l = pd_list; l; l = l->next) {
        dec = l->data;
        if (!strcmp(dec->id, id))
            return dec;
    }

    return NULL;
}

static void channel_free(void* data)
{
    struct srd_channel* ch = data;

    if (!ch)
        return;

    safe_free(ch->desc);
    safe_free(ch->name);
    safe_free(ch->id);
    safe_free(ch->idn);
    g_free(ch);
}

static void variant_free(void* data)
{
    GVariant* var = data;

    if (!var)
        return;

    g_variant_unref(var);
}

static void annotation_row_free(void* data)
{
    struct srd_decoder_annotation_row* row = data;

    if (!row)
        return;

    g_slist_free(row->ann_classes);
    g_free(row->desc);
    g_free(row->id);
    g_free(row);
}

static void decoder_option_free(void* data)
{
    struct srd_decoder_option* opt = data;

    if (!opt)
        return;

    g_slist_free_full(opt->values, &variant_free);
    variant_free(opt->def);
    safe_free(opt->desc);
    safe_free(opt->id);
    safe_free(opt->idn);
    g_free(opt);
}

static void decoder_free(struct srd_decoder* dec)
{
    PyGILState_STATE gstate;

    if (!dec)
        return;

    if (!dec->is_c_decoder) {
        gstate = PyGILState_Ensure();
        Py_XDECREF(dec->py_dec);
        Py_XDECREF(dec->py_mod);
        PyGILState_Release(gstate);
    }

    g_slist_free_full(dec->options, &decoder_option_free);
    g_slist_free_full(dec->binary, (GDestroyNotify)&g_strfreev);
    g_slist_free_full(dec->annotation_rows, &annotation_row_free);
    g_slist_free_full(dec->annotations, (GDestroyNotify)&g_strfreev);
    g_slist_free_full(dec->opt_channels, &channel_free);
    g_slist_free_full(dec->channels, &channel_free);

    g_slist_free_full(dec->outputs, g_free);
    g_slist_free_full(dec->inputs, g_free);
    g_slist_free_full(dec->tags, g_free);
    g_free(dec->license);
    g_free(dec->desc);
    g_free(dec->longname);
    g_free(dec->name);
    g_free(dec->id);

    g_free(dec);
}

static int get_channels(const struct srd_decoder* d, const char* attr,
    GSList** out_pdchl, int offset)
{
    PyObject *py_channellist, *py_entry;
    struct srd_channel* pdch;
    GSList* pdchl;
    ssize_t i;
    PyGILState_STATE gstate;

    gstate = PyGILState_Ensure();

    if (!PyObject_HasAttrString(d->py_dec, attr)) {
        /* No channels of this type specified. */
        PyGILState_Release(gstate);
        return SRD_OK;
    }

    pdchl = NULL;

    py_channellist = PyObject_GetAttrString(d->py_dec, attr);
    if (!py_channellist)
        goto except_out;

    if (!PyTuple_Check(py_channellist)) {
        srd_err("Protocol decoder %s %s attribute is not a tuple.",
            d->name, attr);
        goto err_out;
    }

    for (i = PyTuple_Size(py_channellist) - 1; i >= 0; i--) {
        py_entry = PyTuple_GetItem(py_channellist, i);
        if (!py_entry)
            goto except_out;

        if (!PyDict_Check(py_entry)) {
            srd_err("Protocol decoder %s %s attribute is not "
                    "a list of dict elements.",
                d->name, attr);
            goto err_out;
        }
        pdch = g_malloc0(sizeof(struct srd_channel));
        if (pdch == NULL) {
            srd_err("%s,ERROR:failed to alloc memory.", __func__);
            goto err_out;
        }

        /* Add to list right away so it doesn't get lost. */
        pdchl = g_slist_prepend(pdchl, pdch);

        if (py_dictitem_as_str(py_entry, "id", &pdch->id) != SRD_OK)
            goto err_out;
        if (py_dictitem_as_str(py_entry, "name", &pdch->name) != SRD_OK)
            goto err_out;
        if (py_dictitem_as_str(py_entry, "desc", &pdch->desc) != SRD_OK)
            goto err_out;

        py_dictitem_as_str(py_entry, "idn", &pdch->idn);

        pdch->type = py_dictitem_to_int(py_entry, "type");
        if (pdch->type < 0)
            pdch->type = SRD_CHANNEL_COMMON;
        pdch->order = offset + i;
    }

    Py_DECREF(py_channellist);
    *out_pdchl = pdchl;

    PyGILState_Release(gstate);

    return SRD_OK;

except_out:
    srd_exception_catch(NULL, "Failed to get %s list of %s decoder",
        attr, d->name);

err_out:
    g_slist_free_full(pdchl, &channel_free);
    Py_XDECREF(py_channellist);
    PyGILState_Release(gstate);

    return SRD_ERR_PYTHON;
}

static int get_options(struct srd_decoder* d)
{
    PyObject *py_opts, *py_opt, *py_str, *py_values, *py_default, *py_item;
    GSList* options;
    struct srd_decoder_option* o;
    GVariant* gvar;
    ssize_t opt, i;
    PyGILState_STATE gstate;

    gstate = PyGILState_Ensure();

    if (!PyObject_HasAttrString(d->py_dec, "options")) {
        /* No options, that's fine. */
        PyGILState_Release(gstate);
        return SRD_OK;
    }

    options = NULL;

    /* If present, options must be a tuple. */
    py_opts = PyObject_GetAttrString(d->py_dec, "options");
    if (!py_opts)
        goto except_out;

    if (!PyTuple_Check(py_opts)) {
        srd_err("Protocol decoder %s: options attribute is not "
                "a tuple.",
            d->id);
        goto err_out;
    }

    for (opt = PyTuple_Size(py_opts) - 1; opt >= 0; opt--) {
        py_opt = PyTuple_GetItem(py_opts, opt);
        if (!py_opt)
            goto except_out;

        if (!PyDict_Check(py_opt)) {
            srd_err("Protocol decoder %s options: each option "
                    "must consist of a dictionary.",
                d->name);
            goto err_out;
        }

        o = g_malloc0(sizeof(struct srd_decoder_option));
        if (o == NULL) {
            srd_err("%s,ERROR:failed to alloc memory.", __func__);
            goto err_out;
        }

        /* Add to list right away so it doesn't get lost. */
        options = g_slist_prepend(options, o);

        py_str = PyDict_GetItemString(py_opt, "id");
        if (!py_str) {
            srd_err("Protocol decoder %s option %zd has no ID.",
                d->name, opt);
            goto err_out;
        }
        if (py_str_as_str(py_str, &o->id) != SRD_OK)
            goto err_out;

        py_str = PyDict_GetItemString(py_opt, "desc");
        if (py_str) {
            if (py_str_as_str(py_str, &o->desc) != SRD_OK)
                goto err_out;
        }

        py_str = PyDict_GetItemString(py_opt, "idn");
        if (py_str) {
            py_str_as_str(py_str, &o->idn);
        }

        py_default = PyDict_GetItemString(py_opt, "default");
        if (py_default) {
            gvar = py_obj_to_variant(py_default);
            if (!gvar) {
                srd_err("Protocol decoder %s option 'default' has "
                        "invalid default value.",
                    d->name);
                goto err_out;
            }
            o->def = g_variant_ref_sink(gvar);
        }

        py_values = PyDict_GetItemString(py_opt, "values");
        if (py_values) {
            /*
             * A default is required if a list of values is
             * given, since it's used to verify their type.
             */
            if (!o->def) {
                srd_err("No default for option '%s'.", o->id);
                goto err_out;
            }
            if (!PyTuple_Check(py_values)) {
                srd_err("Option '%s' values should be a tuple.", o->id);
                goto err_out;
            }

            for (i = PyTuple_Size(py_values) - 1; i >= 0; i--) {
                py_item = PyTuple_GetItem(py_values, i);
                if (!py_item)
                    goto except_out;

                if (py_default && (Py_TYPE(py_default) != Py_TYPE(py_item))) {
                    srd_err("All values for option '%s' must be "
                            "of the same type as the default.",
                        o->id);
                    goto err_out;
                }
                gvar = py_obj_to_variant(py_item);
                if (!gvar) {
                    srd_err("Protocol decoder %s option 'values' "
                            "contains invalid value.",
                        d->name);
                    goto err_out;
                }
                o->values = g_slist_prepend(o->values,
                    g_variant_ref_sink(gvar));
            }
        }
    }
    d->options = options;
    Py_DECREF(py_opts);
    PyGILState_Release(gstate);

    return SRD_OK;

except_out:
    srd_exception_catch(NULL, "Failed to get %s decoder options", d->name);

err_out:
    g_slist_free_full(options, &decoder_option_free);
    Py_XDECREF(py_opts);
    PyGILState_Release(gstate);

    return SRD_ERR_PYTHON;
}

/* Convert annotation class attribute to GSList of char **. */
static int get_annotations(struct srd_decoder* dec)
{
    PyObject *py_annlist, *py_ann;
    GSList* annotations;
    char** annpair;
    ssize_t i;
    int ann_type = 7;
    unsigned int j;
    PyGILState_STATE gstate;

    assert(dec);

    gstate = PyGILState_Ensure();

    if (!PyObject_HasAttrString(dec->py_dec, "annotations")) {
        PyGILState_Release(gstate);
        return SRD_OK;
    }

    annotations = NULL;

    py_annlist = PyObject_GetAttrString(dec->py_dec, "annotations");
    if (!py_annlist)
        goto except_out;

    if (!PyTuple_Check(py_annlist)) {
        srd_err("Protocol decoder %s annotations should "
                "be a tuple.",
            dec->name);
        goto err_out;
    }

    for (i = 0; i < PyTuple_Size(py_annlist); i++) {
        py_ann = PyTuple_GetItem(py_annlist, i);
        if (!py_ann)
            goto except_out;

        if (!PyTuple_Check(py_ann) || (PyTuple_Size(py_ann) != 3 && PyTuple_Size(py_ann) != 2)) {
            srd_err("Protocol decoder %s annotation %zd should "
                    "be a tuple with two or three elements.",
                dec->name, i + 1);
            goto err_out;
        }
        if (py_strseq_to_char(py_ann, &annpair) != SRD_OK)
            goto err_out;

        annotations = g_slist_prepend(annotations, annpair);

        if (PyTuple_Size(py_ann) == 3) {
            ann_type = 0;
            for (j = 0; j < strlen(annpair[0]); j++)
                ann_type = ann_type * 10 + (annpair[0][j] - '0');
            dec->ann_types = g_slist_append(dec->ann_types, GINT_TO_POINTER(ann_type));
        } else if (PyTuple_Size(py_ann) == 2) {
            dec->ann_types = g_slist_append(dec->ann_types, GINT_TO_POINTER(ann_type));
            ann_type++;
        }
    }
    dec->annotations = annotations;
    Py_DECREF(py_annlist);
    PyGILState_Release(gstate);

    return SRD_OK;

except_out:
    srd_exception_catch(NULL, "Failed to get %s decoder annotations", dec->name);

err_out:
    g_slist_free_full(annotations, (GDestroyNotify)&g_strfreev);
    Py_XDECREF(py_annlist);
    PyGILState_Release(gstate);

    return SRD_ERR_PYTHON;
}

/* Convert annotation_rows to GSList of 'struct srd_decoder_annotation_row'. */
static int get_annotation_rows(struct srd_decoder* dec)
{
    PyObject *py_ann_rows, *py_ann_row, *py_ann_classes, *py_item;
    GSList* annotation_rows;
    struct srd_decoder_annotation_row* ann_row;
    ssize_t i, k;
    size_t class_idx;
    PyGILState_STATE gstate;

    gstate = PyGILState_Ensure();

    if (!PyObject_HasAttrString(dec->py_dec, "annotation_rows")) {
        PyGILState_Release(gstate);
        return SRD_OK;
    }

    annotation_rows = NULL;

    py_ann_rows = PyObject_GetAttrString(dec->py_dec, "annotation_rows");
    if (!py_ann_rows)
        goto except_out;

    if (!PyTuple_Check(py_ann_rows)) {
        srd_err("Protocol decoder %s annotation_rows "
                "must be a tuple.",
            dec->name);
        goto err_out;
    }

    for (i = PyTuple_Size(py_ann_rows) - 1; i >= 0; i--) {
        py_ann_row = PyTuple_GetItem(py_ann_rows, i);
        if (!py_ann_row)
            goto except_out;

        if (!PyTuple_Check(py_ann_row) || PyTuple_Size(py_ann_row) != 3) {
            srd_err("Protocol decoder %s annotation_rows "
                    "must contain only tuples of 3 elements.",
                dec->name);
            goto err_out;
        }
        ann_row = g_malloc0(sizeof(struct srd_decoder_annotation_row));
        if (ann_row == NULL) {
            srd_err("%s,ERROR:failed to alloc memory.", __func__);
            goto err_out;
        }

        /* Add to list right away so it doesn't get lost. */
        annotation_rows = g_slist_prepend(annotation_rows, ann_row);

        py_item = PyTuple_GetItem(py_ann_row, 0);
        if (!py_item)
            goto except_out;
        if (py_str_as_str(py_item, &ann_row->id) != SRD_OK)
            goto err_out;

        py_item = PyTuple_GetItem(py_ann_row, 1);
        if (!py_item)
            goto except_out;
        if (py_str_as_str(py_item, &ann_row->desc) != SRD_OK)
            goto err_out;

        py_ann_classes = PyTuple_GetItem(py_ann_row, 2);
        if (!py_ann_classes)
            goto except_out;

        if (!PyTuple_Check(py_ann_classes)) {
            srd_err("Protocol decoder %s annotation_rows tuples "
                    "must have a tuple of numbers as 3rd element.",
                dec->name);
            goto err_out;
        }

        for (k = PyTuple_Size(py_ann_classes) - 1; k >= 0; k--) {
            py_item = PyTuple_GetItem(py_ann_classes, k);
            if (!py_item)
                goto except_out;

            if (!PyLong_Check(py_item)) {
                srd_err("Protocol decoder %s annotation row "
                        "class tuple must only contain numbers.",
                    dec->name);
                goto err_out;
            }
            class_idx = PyLong_AsSize_t(py_item);
            if (PyErr_Occurred())
                goto except_out;

            ann_row->ann_classes = g_slist_prepend(ann_row->ann_classes,
                GSIZE_TO_POINTER(class_idx));
        }
    }
    dec->annotation_rows = annotation_rows;
    Py_DECREF(py_ann_rows);
    PyGILState_Release(gstate);

    return SRD_OK;

except_out:
    srd_exception_catch(NULL, "Failed to get %s decoder annotation rows",
        dec->name);

err_out:
    g_slist_free_full(annotation_rows, &annotation_row_free);
    Py_XDECREF(py_ann_rows);
    PyGILState_Release(gstate);

    return SRD_ERR_PYTHON;
}

/* Convert binary classes to GSList of char **. */
static int get_binary_classes(struct srd_decoder* dec)
{
    PyObject *py_bin_classes, *py_bin_class;
    GSList* bin_classes;
    char** bin;
    ssize_t i;
    PyGILState_STATE gstate;

    gstate = PyGILState_Ensure();

    if (!PyObject_HasAttrString(dec->py_dec, "binary")) {
        PyGILState_Release(gstate);
        return SRD_OK;
    }

    bin_classes = NULL;

    py_bin_classes = PyObject_GetAttrString(dec->py_dec, "binary");
    if (!py_bin_classes)
        goto except_out;

    if (!PyTuple_Check(py_bin_classes)) {
        srd_err("Protocol decoder %s binary classes should "
                "be a tuple.",
            dec->name);
        goto err_out;
    }

    for (i = PyTuple_Size(py_bin_classes) - 1; i >= 0; i--) {
        py_bin_class = PyTuple_GetItem(py_bin_classes, i);
        if (!py_bin_class)
            goto except_out;

        if (!PyTuple_Check(py_bin_class)
            || PyTuple_Size(py_bin_class) != 2) {
            srd_err("Protocol decoder %s binary classes should "
                    "consist only of tuples of 2 elements.",
                dec->name);
            goto err_out;
        }
        if (py_strseq_to_char(py_bin_class, &bin) != SRD_OK)
            goto err_out;

        bin_classes = g_slist_prepend(bin_classes, bin);
    }
    dec->binary = bin_classes;
    Py_DECREF(py_bin_classes);
    PyGILState_Release(gstate);

    return SRD_OK;

except_out:
    srd_exception_catch(NULL, "Failed to get %s decoder binary classes",
        dec->name);

err_out:
    g_slist_free_full(bin_classes, (GDestroyNotify)&g_strfreev);
    Py_XDECREF(py_bin_classes);
    PyGILState_Release(gstate);

    return SRD_ERR_PYTHON;
}

/* Check whether the Decoder class defines the named method. */
static int check_method(PyObject* py_dec, const char* mod_name,
    const char* method_name)
{
    PyObject* py_method;
    int is_callable;
    PyGILState_STATE gstate;

    gstate = PyGILState_Ensure();

    py_method = PyObject_GetAttrString(py_dec, method_name);
    if (!py_method) {
        srd_exception_catch(NULL, "Protocol decoder %s Decoder class "
                                  "has no %s() method",
            mod_name, method_name);
        PyGILState_Release(gstate);
        return SRD_ERR_PYTHON;
    }

    is_callable = PyCallable_Check(py_method);
    Py_DECREF(py_method);

    PyGILState_Release(gstate);

    if (!is_callable) {
        srd_err("Protocol decoder %s Decoder class attribute '%s' "
                "is not a method.",
            mod_name, method_name);
        return SRD_ERR_PYTHON;
    }

    return SRD_OK;
}

/**
 * Get the API version of the specified decoder.
 *
 * @param d The decoder to use. Must not be NULL.
 *
 * @return The API version of the decoder, or 0 upon errors.
 *
 * @private
 */
SRD_PRIV long srd_decoder_apiver(const struct srd_decoder* d)
{
    PyObject* py_apiver;
    long apiver;
    PyGILState_STATE gstate;

    if (!d)
        return 0;

    gstate = PyGILState_Ensure();

    py_apiver = PyObject_GetAttrString(d->py_dec, "api_version");
    apiver = (py_apiver && PyLong_Check(py_apiver))
        ? PyLong_AsLong(py_apiver)
        : 0;
    Py_XDECREF(py_apiver);

    PyGILState_Release(gstate);

    return apiver;
}

/**
 * Load a protocol decoder module into the embedded Python interpreter.
 *
 * @param module_name The module name to be loaded.
 *
 * @return SRD_OK upon success, a (negative) error code otherwise.
 *
 * @since 0.1.0
 */
SRD_API int srd_decoder_load(const char* module_name)
{
    PyObject* py_basedec;
    struct srd_decoder* d;
    long apiver;
    int is_subclass;
    const char* fail_txt = NULL;
    PyGILState_STATE gstate;

    if (!srd_check_init())
        return SRD_ERR;

    if (!module_name)
        return SRD_ERR_ARG;

    gstate = PyGILState_Ensure();

    if (PyDict_GetItemString(PyImport_GetModuleDict(), module_name)) {
        /* Module was already imported. */
        PyGILState_Release(gstate);
        return SRD_OK;
    }

    d = g_malloc0(sizeof(struct srd_decoder));
    if (d == NULL) {
        srd_err("%s,ERROR:failed to alloc memory.", __func__);
        goto err_out;
    }

    fail_txt = NULL;

    // Load module from python script file,module_name is a sub directory
    d->py_mod = py_import_by_name(module_name);
    if (!d->py_mod) {
        fail_txt = "import by name failed";
        goto except_out;
    }

    if (!mod_sigrokdecode) {
        srd_err("sigrokdecode module not loaded.");
        fail_txt = "sigrokdecode(3) not loaded";
        goto err_out;
    }

    /*
            Get the 'Decoder' class as Python object.
            Here, Decoder is python class type
    */
    d->py_dec = PyObject_GetAttrString(d->py_mod, "Decoder");
    if (!d->py_dec) {
        fail_txt = "no 'Decoder' attribute in imported module";
        goto except_out;
    }

    /*
       Here, Decoder is c class type
    */
    py_basedec = PyObject_GetAttrString(mod_sigrokdecode, "Decoder");
    if (!py_basedec) {
        fail_txt = "no 'Decoder' attribute in sigrokdecode(3)";
        goto except_out;
    }

    is_subclass = PyObject_IsSubclass(d->py_dec, py_basedec);
    Py_DECREF(py_basedec);

    if (!is_subclass) {
        srd_err("Decoder class in protocol decoder module %s is not "
                "a subclass of sigrokdecode.Decoder.",
            module_name);
        fail_txt = "not a subclass of sigrokdecode.Decoder";
        goto err_out;
    }

    /*
     * Check that this decoder has the correct PD API version.
     * PDs of different API versions are incompatible and cannot work.
     */
    apiver = srd_decoder_apiver(d);
    if (apiver != 3) {
        srd_exception_catch(NULL, "Only PD API version 3 is supported, "
                                  "decoder %s has version %ld",
            module_name, apiver);
        fail_txt = "API version mismatch";
        goto err_out;
    }

    /* Check Decoder class for required methods. */

    if (check_method(d->py_dec, module_name, "reset") != SRD_OK) {
        fail_txt = "no 'reset()' method";
        goto err_out;
    }

    if (check_method(d->py_dec, module_name, "start") != SRD_OK) {
        fail_txt = "no 'start()' method";
        goto err_out;
    }

    if (check_method(d->py_dec, module_name, "decode") != SRD_OK) {
        fail_txt = "no 'decode()' method";
        goto err_out;
    }

    /* Store required fields in newly allocated strings. */
    if (py_attr_as_str(d->py_dec, "id", &(d->id)) != SRD_OK) {
        fail_txt = "no 'id' attribute";
        goto err_out;
    }

    if (py_attr_as_str(d->py_dec, "name", &(d->name)) != SRD_OK) {
        fail_txt = "no 'name' attribute";
        goto err_out;
    }

    if (py_attr_as_str(d->py_dec, "longname", &(d->longname)) != SRD_OK) {
        fail_txt = "no 'longname' attribute";
        goto err_out;
    }

    if (py_attr_as_str(d->py_dec, "desc", &(d->desc)) != SRD_OK) {
        fail_txt = "no 'desc' attribute";
        goto err_out;
    }

    if (py_attr_as_str(d->py_dec, "license", &(d->license)) != SRD_OK) {
        fail_txt = "no 'license' attribute";
        goto err_out;
    }

    if (py_attr_as_strlist(d->py_dec, "inputs", &(d->inputs)) != SRD_OK) {
        fail_txt = "missing or malformed 'inputs' attribute";
        goto err_out;
    }

    if (py_attr_as_strlist(d->py_dec, "outputs", &(d->outputs)) != SRD_OK) {
        fail_txt = "missing or malformed 'outputs' attribute";
        goto err_out;
    }

    if (py_attr_as_strlist(d->py_dec, "tags", &(d->tags)) != SRD_OK) {
        fail_txt = "missing or malformed 'tags' attribute";
        goto err_out;
    }

    /* All options and their default values. */
    if (get_options(d) != SRD_OK) {
        fail_txt = "cannot get options";
        goto err_out;
    }

    /* Check and import required channels. */
    if (get_channels(d, "channels", &d->channels, 0) != SRD_OK) {
        fail_txt = "cannot get channels";
        goto err_out;
    }

    /* Check and import optional channels. */
    if (get_channels(d, "optional_channels", &d->opt_channels,
            g_slist_length(d->channels))
        != SRD_OK) {
        fail_txt = "cannot get optional channels";
        goto err_out;
    }

    if (get_annotations(d) != SRD_OK) {
        fail_txt = "cannot get annotations";
        goto err_out;
    }

    if (get_annotation_rows(d) != SRD_OK) {
        fail_txt = "cannot get annotation rows";
        goto err_out;
    }

    if (get_binary_classes(d) != SRD_OK) {
        fail_txt = "cannot get binary classes";
        goto err_out;
    }

    PyGILState_Release(gstate);

    /* Append it to the list of loaded decoders. */
    pd_list = g_slist_append(pd_list, d);

    return SRD_OK;

except_out:
    /* Don't show a message for the "common" directory, it's not a PD. */
    if (strcmp(module_name, "common")) {
        srd_exception_catch(NULL, "Failed to load decoder %s: %s",
            module_name, fail_txt);
    }
    fail_txt = NULL;

err_out:
    if (fail_txt != NULL) {
        srd_err("Failed to load decoder %s: %s", module_name, fail_txt);
    }

    decoder_free(d);
    PyGILState_Release(gstate);

    return SRD_ERR_PYTHON;
}

/**
 * Return a protocol decoder's docstring.
 *
 * @param dec The loaded protocol decoder.
 *
 * @return A newly allocated buffer containing the protocol decoder's
 *         documentation. The caller is responsible for free'ing the buffer.
 *
 * @since 0.1.0
 */
SRD_API char* srd_decoder_doc_get(const struct srd_decoder* dec)
{
    PyObject* py_str;
    char* doc;
    PyGILState_STATE gstate;

    if (!srd_check_init())
        return NULL;

    if (!dec)
        return NULL;

    if (dec->is_c_decoder) {
        return g_strdup(dec->desc ? dec->desc : "");
    }

    gstate = PyGILState_Ensure();

    if (!PyObject_HasAttrString(dec->py_mod, "__doc__"))
        goto err;

    if (!(py_str = PyObject_GetAttrString(dec->py_mod, "__doc__"))) {
        srd_exception_catch(NULL, "Failed to get docstring");
        goto err;
    }

    doc = NULL;
    if (py_str != Py_None)
        py_str_as_str(py_str, &doc);
    Py_DECREF(py_str);

    PyGILState_Release(gstate);

    return doc;

err:
    PyGILState_Release(gstate);

    return NULL;
}

/**
 * Unload the specified protocol decoder.
 *
 * @param dec The struct srd_decoder to be unloaded.
 *
 * @return SRD_OK upon success, a (negative) error code otherwise.
 *
 * @since 0.1.0
 */
SRD_API int srd_decoder_unload(struct srd_decoder* dec)
{
    struct srd_session* sess;
    GSList* l;

    if (!srd_check_init())
        return SRD_ERR;

    if (!dec)
        return SRD_ERR_ARG;

    /*
     * Since any instances of this decoder need to be released as well,
     * but they could be anywhere in the stack, just free the entire
     * stack. A frontend reloading a decoder thus has to restart all
     * instances, and rebuild the stack.
     */
    for (l = sessions; l; l = l->next) {
        sess = l->data;
        srd_inst_free_all(sess);
    }

    /* Remove the PD from the list of loaded decoders. */
    pd_list = g_slist_remove(pd_list, dec);

    decoder_free(dec);

    return SRD_OK;
}

static void srd_decoder_load_all_zip_path(char* zip_path)
{
    PyObject *zipimport_mod, *zipimporter_class, *zipimporter;
    PyObject *prefix_obj, *files, *key, *value, *set, *modname;
    Py_ssize_t pos = 0;
    char* prefix;
    size_t prefix_len;
    PyGILState_STATE gstate;

    set = files = prefix_obj = zipimporter = zipimporter_class = NULL;

    gstate = PyGILState_Ensure();

    zipimport_mod = py_import_by_name("zipimport");
    if (zipimport_mod == NULL)
        goto err_out;

    zipimporter_class = PyObject_GetAttrString(zipimport_mod, "zipimporter");
    if (zipimporter_class == NULL)
        goto err_out;

    zipimporter = PyObject_CallFunction(zipimporter_class, "s", zip_path);
    if (zipimporter == NULL)
        goto err_out;

    prefix_obj = PyObject_GetAttrString(zipimporter, "prefix");
    if (prefix_obj == NULL)
        goto err_out;

    files = PyObject_GetAttrString(zipimporter, "_files");
    if (files == NULL || !PyDict_Check(files))
        goto err_out;

    set = PySet_New(NULL);
    if (set == NULL)
        goto err_out;

    if (py_str_as_str(prefix_obj, &prefix) != SRD_OK)
        goto err_out;

    prefix_len = strlen(prefix);

    while (PyDict_Next(files, &pos, &key, &value)) {
        char *path, *slash;
        if (py_str_as_str(key, &path) == SRD_OK) {
            if (strlen(path) > prefix_len
                && memcmp(path, prefix, prefix_len) == 0
                && (slash = strchr(path + prefix_len, '/'))) {

                modname = PyUnicode_FromStringAndSize(path + prefix_len,
                    slash - (path + prefix_len));
                if (modname == NULL) {
                    PyErr_Clear();
                } else {
                    PySet_Add(set, modname);
                    Py_DECREF(modname);
                }
            }
            g_free(path);
        }
    }
    g_free(prefix);

    while ((modname = PySet_Pop(set))) {
        char* modname_str;
        if (py_str_as_str(modname, &modname_str) == SRD_OK) {
            /* The directory name is the module name (e.g. "i2c"). */
            srd_decoder_load(modname_str);
            g_free(modname_str);
        }
        Py_DECREF(modname);
    }

err_out:
    Py_XDECREF(set);
    Py_XDECREF(files);
    Py_XDECREF(prefix_obj);
    Py_XDECREF(zipimporter);
    Py_XDECREF(zipimporter_class);
    Py_XDECREF(zipimport_mod);
    PyErr_Clear();
    PyGILState_Release(gstate);
}

static void srd_decoder_load_all_path(char* path)
{
    GDir* dir;
    const gchar* direntry;

    assert(path);

    if (!(dir = g_dir_open(path, 0, NULL))) {
        /* Not really fatal. Try zipimport method too. */
        srd_decoder_load_all_zip_path(path);
        return;
    }

    /*
     * This ignores errors returned by srd_decoder_load(). That
     * function will have logged the cause, but in any case we
     * want to continue anyway.
     */
    while ((direntry = g_dir_read_name(dir)) != NULL) {
        /* Only attempt to load if it's a directory (standard for PDs) */
        gchar *full_path = g_build_filename(path, direntry, NULL);
        if (g_file_test(full_path, G_FILE_TEST_IS_DIR)) {
            srd_decoder_load(direntry);
        }
        g_free(full_path);
    }
    g_dir_close(dir);
}

/**
 * Load all installed protocol decoders.
 *
 * @return SRD_OK upon success, a (negative) error code otherwise.
 *
 * @since 0.1.0
 */
SRD_API int srd_decoder_load_all(void)
{
    GSList* l;

    if (!srd_check_init())
        return SRD_ERR;

    for (l = searchpaths; l; l = l->next) {
        srd_decoder_load_all_path(l->data);
    }

    srd_c_decoder_load_all();

    return SRD_OK;
}

static void srd_decoder_unload_cb(void* arg, void* ignored)
{
    (void)ignored;

    srd_decoder_unload((struct srd_decoder*)arg);
}

/**
 * Unload all loaded protocol decoders.
 *
 * @return SRD_OK upon success, a (negative) error code otherwise.
 *
 * @since 0.1.0
 */
SRD_API int srd_decoder_unload_all(void)
{
    g_slist_foreach(pd_list, srd_decoder_unload_cb, NULL);
    g_slist_free(pd_list);
    pd_list = NULL;

    return SRD_OK;
}

SRD_API int srd_c_decoder_register(struct srd_c_decoder* dec)
{
    struct srd_decoder* d;
    int i;

    if (!dec || !dec->id)
        return SRD_ERR_ARG;

    d = g_malloc0(sizeof(struct srd_decoder));
    if (!d)
        return SRD_ERR_MALLOC;

    d->id = g_strdup(dec->id);
    d->name = g_strdup(dec->name ? dec->name : dec->id);
    d->longname = g_strdup(dec->longname ? dec->longname : dec->name);
    d->desc = g_strdup(dec->desc ? dec->desc : "");
    d->license = g_strdup(dec->license ? dec->license : "gplv2+");
    d->is_c_decoder = TRUE;
    d->c_dec = dec;
    d->py_mod = NULL;
    d->py_dec = NULL;

    d->channels = NULL;
    if (dec->num_channels > 0 && dec->channels) {
        for (i = 0; i < dec->num_channels; i++) {
            struct srd_channel* ch = g_malloc0(sizeof(struct srd_channel));
            ch->id = dec->channels[i].id ? g_strdup(dec->channels[i].id) : NULL;
            ch->name = dec->channels[i].name ? g_strdup(dec->channels[i].name) : NULL;
            ch->desc = dec->channels[i].desc ? g_strdup(dec->channels[i].desc) : NULL;
            ch->order = dec->channels[i].order;
            ch->type = dec->channels[i].type;
            ch->idn = dec->channels[i].idn ? g_strdup(dec->channels[i].idn) : NULL;
            d->channels = g_slist_append(d->channels, ch);
        }
    }

    d->opt_channels = NULL;
    if (dec->num_optional_channels > 0 && dec->optional_channels) {
        for (i = 0; i < dec->num_optional_channels; i++) {
            struct srd_channel* ch = g_malloc0(sizeof(struct srd_channel));
            ch->id = dec->optional_channels[i].id ? g_strdup(dec->optional_channels[i].id) : NULL;
            ch->name = dec->optional_channels[i].name ? g_strdup(dec->optional_channels[i].name) : NULL;
            ch->desc = dec->optional_channels[i].desc ? g_strdup(dec->optional_channels[i].desc) : NULL;
            ch->order = dec->optional_channels[i].order;
            ch->type = dec->optional_channels[i].type;
            ch->idn = dec->optional_channels[i].idn ? g_strdup(dec->optional_channels[i].idn) : NULL;
            d->opt_channels = g_slist_append(d->opt_channels, ch);
        }
    }

    d->options = NULL;
    if (dec->num_options > 0 && dec->options) {
        for (i = 0; i < dec->num_options; i++) {
            struct srd_decoder_option* o = g_malloc0(sizeof(struct srd_decoder_option));
            o->id = dec->options[i].id ? g_strdup(dec->options[i].id) : NULL;
            o->idn = dec->options[i].idn ? g_strdup(dec->options[i].idn) : NULL;
            o->desc = dec->options[i].desc ? g_strdup(dec->options[i].desc) : NULL;
            if (dec->options[i].def) {
                o->def = g_variant_ref(dec->options[i].def);
            }
            if (dec->options[i].values) {
                GSList *l;
                for (l = dec->options[i].values; l; l = l->next) {
                    o->values = g_slist_append(o->values, g_variant_ref((GVariant*)l->data));
                }
            }
            d->options = g_slist_append(d->options, o);
        }
    }

    d->annotations = NULL;
    if (dec->num_annotations > 0 && dec->ann_labels) {
        for (i = 0; i < dec->num_annotations; i++) {
            char** pair = g_malloc0(3 * sizeof(char*));
            pair[0] = dec->ann_labels[i][1] ? g_strdup(dec->ann_labels[i][1]) : NULL;
            pair[1] = dec->ann_labels[i][2] ? g_strdup(dec->ann_labels[i][2]) : NULL;
            d->annotations = g_slist_append(d->annotations, pair);
            int ann_type = i + 7;
            if (dec->ann_labels[i][0] && dec->ann_labels[i][0][0] != '\0') {
                int parsed = atoi(dec->ann_labels[i][0]);
                if (parsed > 0)
                    ann_type = parsed;
            }
            d->ann_types = g_slist_append(d->ann_types, GINT_TO_POINTER(ann_type));
        }
    }

    d->annotation_rows = NULL;
    if (dec->num_annotation_rows > 0 && dec->annotation_rows) {
        for (i = 0; i < dec->num_annotation_rows; i++) {
            struct srd_decoder_annotation_row* r = g_malloc0(sizeof(struct srd_decoder_annotation_row));
            r->id = g_strdup(dec->annotation_rows[i].id ? dec->annotation_rows[i].id : "");
            r->desc = g_strdup(dec->annotation_rows[i].desc ? dec->annotation_rows[i].desc : "");
            r->ann_classes = NULL;
            if (dec->annotation_rows[i].ann_classes && dec->annotation_rows[i].num_ann_classes > 0) {
                int j;
                for (j = 0; j < dec->annotation_rows[i].num_ann_classes; j++) {
                    r->ann_classes = g_slist_append(r->ann_classes,
                        GSIZE_TO_POINTER(dec->annotation_rows[i].ann_classes[j]));
                }
            }
            d->annotation_rows = g_slist_append(d->annotation_rows, r);
        }
    }

    d->binary = NULL;
    if (dec->num_binary > 0 && dec->binary) {
        for (i = 0; i < dec->num_binary; i++) {
            char** pair = g_malloc0(3 * sizeof(char*));
            pair[0] = g_strdup(dec->binary[i].id ? dec->binary[i].id : "");
            pair[1] = g_strdup(dec->binary[i].desc ? dec->binary[i].desc : "");
            d->binary = g_slist_append(d->binary, pair);
        }
    }

    d->inputs = NULL;
    if (dec->num_inputs > 0 && dec->inputs) {
        for (i = 0; i < dec->num_inputs; i++) {
            d->inputs = g_slist_append(d->inputs, g_strdup(dec->inputs[i]));
        }
    }

    d->outputs = NULL;
    if (dec->num_outputs > 0 && dec->outputs) {
        for (i = 0; i < dec->num_outputs; i++) {
            d->outputs = g_slist_append(d->outputs, g_strdup(dec->outputs[i]));
        }
    }

    d->tags = NULL;
    if (dec->num_tags > 0 && dec->tags) {
        for (i = 0; i < dec->num_tags; i++) {
            d->tags = g_slist_append(d->tags, g_strdup(dec->tags[i]));
        }
    }

    pd_list = g_slist_append(pd_list, d);
    srd_dbg("Registered C decoder %s.", dec->id);

    return SRD_OK;
}

static GSList *c_decoder_paths = NULL;

SRD_API int srd_c_decoder_path_set(const char* path)
{
    g_slist_free_full(c_decoder_paths, g_free);
    c_decoder_paths = NULL;
    if (path)
        c_decoder_paths = g_slist_append(c_decoder_paths, g_strdup(path));
    return SRD_OK;
}

SRD_API int srd_c_decoder_path_add(const char *path)
{
    if (!path)
        return SRD_ERR_ARG;
    GSList *l;
    for (l = c_decoder_paths; l; l = l->next) {
        if (g_strcmp0((const char *)l->data, path) == 0)
            return SRD_OK;
    }
    c_decoder_paths = g_slist_append(c_decoder_paths, g_strdup(path));
    return SRD_OK;
}

SRD_API void srd_c_decoder_paths_clear(void)
{
    g_slist_free_full(c_decoder_paths, g_free);
    c_decoder_paths = NULL;
}

static int srd_c_decoder_check_version(int dll_version)
{
    if (dll_version == SRD_C_DECODER_API_VERSION)
        return SRD_OK;
    if (dll_version >= SRD_C_DECODER_API_MIN_VERSION &&
        dll_version < SRD_C_DECODER_API_VERSION) {
        srd_info("C decoder DLL API version %d is compatible (current=%d, min=%d).",
            dll_version, SRD_C_DECODER_API_VERSION, SRD_C_DECODER_API_MIN_VERSION);
        return SRD_OK;
    }
    srd_warn("C decoder DLL API version %d is incompatible (current=%d, min=%d).",
        dll_version, SRD_C_DECODER_API_VERSION, SRD_C_DECODER_API_MIN_VERSION);
    return SRD_ERR;
}

static int srd_c_decoder_load_single(const char *full_path)
{
    if (!full_path)
        return SRD_ERR_ARG;

    struct srd_c_dll_entry *existing = srd_c_dll_registry_find_by_path(full_path);
    if (existing) {
        srd_dbg("C decoder '%s' already loaded, skipping.", full_path);
        return SRD_OK;
    }

#ifdef _WIN32
    HMODULE handle = LoadLibraryA(full_path);
    if (!handle) {
        srd_warn("Failed to load C decoder DLL '%s': error %lu",
            full_path, GetLastError());
        srd_c_dll_registry_add(full_path, NULL, 0, SRD_C_DLL_LOAD_FAILED, NULL, NULL);
        return SRD_ERR;
    }

    srd_c_decoder_api_version_func version_func = (srd_c_decoder_api_version_func)(void*)GetProcAddress(handle, "srd_c_decoder_api_version");
    srd_c_decoder_entry_func entry_func = (srd_c_decoder_entry_func)(void*)GetProcAddress(handle, "srd_c_decoder_entry");

    if (!entry_func) {
        srd_warn("C decoder DLL '%s' has no srd_c_decoder_entry symbol.", full_path);
        FreeLibrary(handle);
        srd_c_dll_registry_add(full_path, NULL, 0, SRD_C_DLL_ENTRY_MISSING, NULL, NULL);
        return SRD_ERR;
    }

    int dll_version = version_func ? version_func() : 0;
    if (srd_c_decoder_check_version(dll_version) != SRD_OK) {
        FreeLibrary(handle);
        srd_c_dll_registry_add(full_path, NULL, dll_version, SRD_C_DLL_VERSION_MISMATCH, NULL, NULL);
        return SRD_ERR;
    }
#else
    void *handle = dlopen(full_path, RTLD_LAZY);
    if (!handle) {
        srd_warn("Failed to load C decoder SO '%s': %s", full_path, dlerror());
        srd_c_dll_registry_add(full_path, NULL, 0, SRD_C_DLL_LOAD_FAILED, NULL, NULL);
        return SRD_ERR;
    }

    srd_c_decoder_api_version_func version_func = (srd_c_decoder_api_version_func)dlsym(handle, "srd_c_decoder_api_version");
    srd_c_decoder_entry_func entry_func = (srd_c_decoder_entry_func)dlsym(handle, "srd_c_decoder_entry");

    if (!entry_func) {
        srd_warn("C decoder SO '%s' has no srd_c_decoder_entry symbol.", full_path);
        dlclose(handle);
        srd_c_dll_registry_add(full_path, NULL, 0, SRD_C_DLL_ENTRY_MISSING, NULL, NULL);
        return SRD_ERR;
    }

    int dll_version = version_func ? version_func() : 0;
    if (srd_c_decoder_check_version(dll_version) != SRD_OK) {
        dlclose(handle);
        srd_c_dll_registry_add(full_path, NULL, dll_version, SRD_C_DLL_VERSION_MISMATCH, NULL, NULL);
        return SRD_ERR;
    }
#endif

    struct srd_c_decoder *dec = entry_func();
    if (dec) {
        srd_c_decoder_register(dec);
        srd_c_dll_registry_add(full_path, handle, dll_version, SRD_C_DLL_LOADED, dec->id, dec);
        srd_dbg("Loaded C decoder '%s' from %s.", dec->id, full_path);
    } else {
        srd_warn("C decoder DLL '%s' entry returned NULL.", full_path);
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return SRD_ERR;
    }

    return SRD_OK;
}

SRD_API int srd_c_decoder_load(const char *dll_path)
{
    if (!dll_path)
        return SRD_ERR_ARG;
    if (!g_path_is_absolute(dll_path)) {
        srd_warn("srd_c_decoder_load: path must be absolute, got '%s'.", dll_path);
        return SRD_ERR_ARG;
    }
    return srd_c_decoder_load_single(dll_path);
}

SRD_API int srd_c_decoder_unload(const char *decoder_id)
{
    if (!decoder_id)
        return SRD_ERR_ARG;

    struct srd_c_dll_entry *entry = srd_c_dll_registry_find_by_id(decoder_id);
    if (!entry) {
        srd_warn("srd_c_decoder_unload: decoder '%s' not found in registry.", decoder_id);
        return SRD_ERR_ARG;
    }

    if (entry->status != SRD_C_DLL_LOADED || !entry->handle) {
        srd_warn("srd_c_decoder_unload: decoder '%s' is not in loaded state.", decoder_id);
        return SRD_ERR_ARG;
    }

    GSList *sl;
    for (sl = sessions; sl; sl = sl->next) {
        struct srd_session *sess = sl->data;
        GSList *dl;
        for (dl = sess->di_list; dl; dl = dl->next) {
            struct srd_decoder_inst *di = dl->data;
            if (di->is_c_inst && di->c_dec_inst &&
                g_strcmp0(di->c_dec_inst->id, decoder_id) == 0 &&
                di->decoder_state != 0) {
                srd_warn("srd_c_decoder_unload: decoder '%s' has active instances.", decoder_id);
                return SRD_ERR_ARG;
            }
        }
    }

    GSList *l;
    for (l = pd_list; l; l = l->next) {
        struct srd_decoder *d = l->data;
        if (d->is_c_decoder && d->c_dec &&
            g_strcmp0(d->c_dec->id, decoder_id) == 0) {
            pd_list = g_slist_remove(pd_list, d);
            break;
        }
    }

#ifdef _WIN32
    FreeLibrary((HMODULE)entry->handle);
    srd_dbg("Unloaded C decoder DLL '%s'.", entry->file_path);
#else
    dlclose(entry->handle);
    srd_dbg("Unloaded C decoder SO '%s'.", entry->file_path);
#endif

    srd_c_dll_registry_remove(decoder_id);
    return SRD_OK;
}

SRD_API const GSList *srd_c_dll_registry_get(void)
{
    return c_dll_registry;
}

SRD_API const struct srd_c_dll_entry *srd_c_dll_info_get(const char *decoder_id)
{
    return srd_c_dll_registry_find_by_id(decoder_id);
}

SRD_API int srd_c_decoder_load_all(void)
{
    GSList *search_paths_list = NULL, *l;
    const char* base_path;
    char* c_dec_path;

    for (l = c_decoder_paths; l; l = l->next) {
        search_paths_list = g_slist_append(search_paths_list, g_strdup((const char *)l->data));
    }

    for (l = searchpaths; l; l = l->next) {
        base_path = l->data;
        c_dec_path = g_build_filename(base_path, "c_decoders", NULL);
        search_paths_list = g_slist_append(search_paths_list, c_dec_path);
    }

    for (l = search_paths_list; l; l = l->next) {
        const char* dir_path = l->data;
        GDir* dir;
        const char* filename;

        dir = g_dir_open(dir_path, 0, NULL);
        if (!dir)
            continue;

        while ((filename = g_dir_read_name(dir)) != NULL) {
            const char* ext;
#ifdef _WIN32
            ext = strrchr(filename, '.');
            if (!ext || g_ascii_strcasecmp(ext, ".dll") != 0)
                continue;
#else
            ext = strrchr(filename, '.');
            if (!ext || strcmp(ext, ".so") != 0)
                continue;
#endif

            char* full_path = g_build_filename(dir_path, filename, NULL);
            srd_c_decoder_load_single(full_path);
            g_free(full_path);
        }

        g_dir_close(dir);
    }

    g_slist_free_full(search_paths_list, g_free);

    return SRD_OK;
}

/** @} */
