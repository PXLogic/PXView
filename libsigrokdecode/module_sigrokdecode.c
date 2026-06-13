/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012 Bert Vermeulen <bert@biot.com>
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
#include "libsigrokdecode-internal.h" /* First, so we avoid a _POSIX_C_SOURCE warning. */
#include "libsigrokdecode.h"

/** @cond PRIVATE */

/*
 * When initialized, a reference to this module inside the Python interpreter
 * lives here.
 */
SRD_PRIV PyObject* mod_sigrokdecode = NULL;
SRD_PRIV PyObject* srd_ChunkDone_exc = NULL;

/** @endcond */

static struct PyModuleDef sigrokdecode_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "sigrokdecode",
    .m_doc = "sigrokdecode module",
    .m_size = -1,
};

/** @cond PRIVATE */
PyMODINIT_FUNC PyInit_sigrokdecode(void)
{
    PyObject *mod, *Decoder_type;
    PyGILState_STATE gstate;

    gstate = PyGILState_Ensure();

    mod = PyModule_Create(&sigrokdecode_module);
    if (!mod)
        goto err_out;

    Decoder_type = srd_Decoder_type_new();
    if (!Decoder_type)
        goto err_out;
    if (PyModule_AddObject(mod, "Decoder", Decoder_type) < 0)
        goto err_out;

    /* Expose output types as symbols in the sigrokdecode module */
    if (PyModule_AddIntConstant(mod, "OUTPUT_ANN", SRD_OUTPUT_ANN) < 0)
        goto err_out;
    if (PyModule_AddIntConstant(mod, "OUTPUT_PYTHON", SRD_OUTPUT_PYTHON) < 0)
        goto err_out;
    if (PyModule_AddIntConstant(mod, "OUTPUT_BINARY", SRD_OUTPUT_BINARY) < 0)
        goto err_out;
    if (PyModule_AddIntConstant(mod, "OUTPUT_META", SRD_OUTPUT_META) < 0)
        goto err_out;
    if (PyModule_AddIntConstant(mod, "OUTPUT_LOGIC", SRD_OUTPUT_LOGIC) < 0)
        goto err_out;

    /* Expose search terms */
    if (PyModule_AddIntConstant(mod, "TERM_HIGH", SRD_TERM_HIGH) < 0)
        goto err_out;
    if (PyModule_AddIntConstant(mod, "TERM_LOW", SRD_TERM_LOW) < 0)
        goto err_out;
    if (PyModule_AddIntConstant(mod, "TERM_RISING_EDGE", SRD_TERM_RISING_EDGE) < 0)
        goto err_out;
    if (PyModule_AddIntConstant(mod, "TERM_FALLING_EDGE", SRD_TERM_FALLING_EDGE) < 0)
        goto err_out;
    if (PyModule_AddIntConstant(mod, "TERM_EITHER_EDGE", SRD_TERM_EITHER_EDGE) < 0)
        goto err_out;
    if (PyModule_AddIntConstant(mod, "TERM_NO_EDGE", SRD_TERM_NO_EDGE) < 0)
        goto err_out;
    if (PyModule_AddIntConstant(mod, "TERM_SKIP", SRD_TERM_SKIP) < 0)
        goto err_out;
    /* Expose meta input symbols. */
    if (PyModule_AddIntConstant(mod, "SRD_CONF_SAMPLERATE", SRD_CONF_SAMPLERATE) < 0)
        goto err_out;

    srd_ChunkDone_exc = PyErr_NewException("sigrokdecode.ChunkDone", NULL, NULL);
    if (!srd_ChunkDone_exc)
        goto err_out;
    Py_INCREF(srd_ChunkDone_exc);
    if (PyModule_AddObject(mod, "ChunkDone", srd_ChunkDone_exc) < 0)
        goto err_out;

    mod_sigrokdecode = mod;

    PyGILState_Release(gstate);

    return mod;

err_out:
    Py_XDECREF(mod);
    srd_exception_catch(NULL, "Failed to initialize module");
    PyGILState_Release(gstate);

    return NULL;
}

/** @endcond */
