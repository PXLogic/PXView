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

#include "pv/dialogs/inputoutputoptions.h"

#include <QFormLayout>
#include <QVBoxLayout>

#include "pv/ui/dockfonts.h"

namespace pv {
namespace dialogs {

InputOutputOptions::InputOutputOptions(QWidget *parent, const QString &title,
                                       const struct sr_option **options,
                                       const QVariantMap &prefill) :
    PxDialog(parent),
    _form(nullptr),
    _button_box(nullptr),
    _binding(options, prefill)
{
    setTitle(title);
    setMinimumWidth(420);

    // The host widget exists so the form layout has a parentWidget():
    // Binding::add_properties_to_form() builds every editor widget with
    // layout->parentWidget() as its parent.
    QWidget *host = new QWidget(this);
    _form = new QFormLayout(host);
    _form->setContentsMargins(10, 10, 10, 10);
    _form->setVerticalSpacing(8);
    // Deferred (auto_commit = false): nothing is written back until accept(),
    // so a rejected dialog cannot leak half-edited values into the import.
    _binding.add_properties_to_form(_form, false, theme_font_dialog());

    _button_box = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    // Pointer-to-member connections dispatch virtually, so these reach this
    // class' accept()/PxDialog's reject() rather than QDialog's.
    connect(_button_box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(_button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QVBoxLayout *vbox = new QVBoxLayout();
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->addWidget(host, 1);
    vbox->addWidget(_button_box, 0, Qt::AlignRight);
    layout()->addLayout(vbox);
}

InputOutputOptions::~InputOutputOptions() = default;

void InputOutputOptions::accept()
{
    // Deferred widgets: pull every edited value into the binding's value map
    // before make_options_table() is called on the accepted dialog.
    _binding.commit();

    PxDialog::accept();
}

} // dialogs
} // pv
