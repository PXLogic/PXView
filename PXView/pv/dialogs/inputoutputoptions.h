/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2015 Joel Holdsworth <joel@airwebreathe.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef PXVIEW_PV_DIALOGS_INPUTOUTPUTOPTIONS_H
#define PXVIEW_PV_DIALOGS_INPUTOUTPUTOPTIONS_H

#include <QVariantMap>

#include <QDialogButtonBox>

#include "pv/dialogs/pxdialog.h"
#include "pv/prop/binding/inputoutput.h"

class QFormLayout;

namespace pv {
namespace dialogs {

/**
 * Presents a libsigrok input/output module's own options to the user.
 *
 * The form is generated from `struct sr_option` declarations, so there is no
 * per-format code here: "binary" gets its numchannels/samplerate spin boxes,
 * csv/vcd/saleae get whatever they declare, and a module without options never
 * needs this dialog at all (check option_count()).
 *
 * Ported from PulseView's pv::dialogs::InputOutputOptions, where the import
 * path asks instead of guessing; the same dialog serves export.
 */
class InputOutputOptions : public PxDialog
{
    Q_OBJECT

public:
    /**
     * @param parent the parent widget.
     * @param title the dialog title (e.g. "Import Binary").
     * @param options the module's option array; only read in the constructor,
     *                so the caller may free it right after construction.
     * @param prefill per-option initial values used instead of the module
     *                defaults (e.g. the current device's channel count).
     */
    InputOutputOptions(QWidget *parent, const QString &title,
                       const struct sr_option **options,
                       const QVariantMap &prefill = QVariantMap());

    ~InputOutputOptions() override;

    /// Number of options offered; 0 means the dialog adds nothing.
    int option_count() const { return _binding.option_count(); }

    /**
     * The values the user confirmed, as a hash table for sr_input_new() /
     * sr_output_new(). Caller owns the table (g_hash_table_destroy()).
     */
    GHashTable *make_options_table() const
    {
        return _binding.make_options_table();
    }

protected:
    void accept() override;

private:
    QFormLayout *_form;
    QDialogButtonBox *_button_box;
    prop::binding::InputOutput _binding;
};

} // dialogs
} // pv

#endif // PXVIEW_PV_DIALOGS_INPUTOUTPUTOPTIONS_H
