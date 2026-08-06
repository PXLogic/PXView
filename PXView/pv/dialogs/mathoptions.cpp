/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2015 DreamSourceLab <support@dreamsourcelab.com>
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

#include "mathoptions.h"
#include <QCheckBox>
#include <QVariant>
#include <QLabel>
#include <QTabBar>
#include <QBitmap>

#include "../sigsession.h"
#include "../view/view.h"
#include "../view/mathtrace.h"
#include "../data/mathstack.h"
#include "../ui/langresource.h"
#include "../ui/fn.h"
#include "../ui/dockfonts.h"
#include "../config/appconfig.h"

using namespace std;
using namespace pv::view;

namespace pv {
namespace dialogs {

MathOptions::MathOptions(SigSession *session, QWidget *parent) :
    PxDialog(parent),
    _session(session), _data_src(session),
    _button_box(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        Qt::Horizontal, this)
{
    setMinimumSize(300, 300);

    _enable = new QCheckBox(this);

    QLabel *lisa_label = new QLabel(this);
    lisa_label->setPixmap(QPixmap(":/icons/math.svg"));

    _math_group = new QGroupBox(this);
    QHBoxLayout *type_layout = new QHBoxLayout();
    QRadioButton *add_radio = new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ADD), "Add"), _math_group);
    add_radio->setProperty("type", data::MathStack::MATH_ADD);
    type_layout->addWidget(add_radio);
    QRadioButton *sub_radio = new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SUBSTRACT), "Substract"), _math_group);
    sub_radio->setProperty("type", data::MathStack::MATH_SUB);
    type_layout->addWidget(sub_radio);
    QRadioButton *mul_radio = new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MULTIPLY), "Multiply"), _math_group);
    mul_radio->setProperty("type", data::MathStack::MATH_MUL);
    type_layout->addWidget(mul_radio);
    QRadioButton *div_radio = new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DIVIDE), "Divide"), _math_group);
    div_radio->setProperty("type", data::MathStack::MATH_DIV);
    type_layout->addWidget(div_radio);
    _math_radio.append(add_radio);
    _math_radio.append(sub_radio);
    _math_radio.append(mul_radio);
    _math_radio.append(div_radio);
    _math_group->setLayout(type_layout);

    _src1_group = new QGroupBox(this);
    _src2_group = new QGroupBox(this);
    QHBoxLayout *src1_layout = new QHBoxLayout();
    QHBoxLayout *src2_layout = new QHBoxLayout();

    src1_layout->setContentsMargins(5, 15, 5, 5);
    src2_layout->setContentsMargins(5, 15, 5, 5);
    type_layout->setContentsMargins(5, 15, 5, 5);

    for(auto m : _data_src->get_signal_models()) {
        if (m->type() == SR_CHANNEL_DSO) {
            QString index_str = QString::number(m->index());
            QRadioButton *xradio = new QRadioButton(index_str, _src1_group);
            xradio->setProperty("index", m->index());
            src1_layout->addWidget(xradio);
            QRadioButton *yradio = new QRadioButton(index_str, _src2_group);
            yradio->setProperty("index", m->index());
            src2_layout->addWidget(yradio);
            _src1_radio.append(xradio);
            _src2_radio.append(yradio);
        }
    }
    _src1_group->setLayout(src1_layout);
    _src2_group->setLayout(src2_layout);


    auto math = _data_src->get_math_stack();
    if (math) {
        // TODO: adapt — MathStack no longer exposes enabled()/src1()/src2();
        // these were UI state owned by view::MathTrace. Default the
        // enable checkbox to false and skip source radio restoration
        // until the corresponding accessors are added to MathStack.
        _enable->setChecked(false);
        for (QVector<QRadioButton *>::const_iterator i = _src1_radio.begin();
            i != _src1_radio.end(); i++) {
           (*i)->setChecked(true);
            break;
        }
        for (QVector<QRadioButton *>::const_iterator i = _src2_radio.begin();
            i != _src2_radio.end(); i++) {
           (*i)->setChecked(true);
            break;
        }
        for (QVector<QRadioButton *>::const_iterator i = _math_radio.begin();
            i != _math_radio.end(); i++) {
            if ((*i)->property("type").toInt() == math->get_type()) {
                (*i)->setChecked(true);
                break;
            }
        }
    } else {
        _enable->setChecked(false);
        for (QVector<QRadioButton *>::const_iterator i = _src1_radio.begin();
            i != _src1_radio.end(); i++) {
           (*i)->setChecked(true);
            break;
        }
        for (QVector<QRadioButton *>::const_iterator i = _src2_radio.begin();
            i != _src2_radio.end(); i++) {
           (*i)->setChecked(true);
            break;
        }
        for (QVector<QRadioButton *>::const_iterator i = _math_radio.begin();
            i != _math_radio.end(); i++) {
            (*i)->setChecked(true);
            break;
        }
    }

    _layout = new QGridLayout(); 
    _layout->setSpacing(0);
    _layout->addWidget(lisa_label, 0, 0, 1, 2, Qt::AlignCenter);
    _layout->addWidget(_enable, 1, 0, 1, 1);
    _layout->addWidget(_math_group, 2, 0, 1, 2);
    _layout->addWidget(_src1_group, 3, 0, 1, 1);
    _layout->addWidget(_src2_group, 3, 1, 1, 1);
    _layout->addWidget(new QLabel(this), 4, 1, 1, 1);
    _layout->addWidget(&_button_box, 5, 1, 1, 1, Qt::AlignHCenter | Qt::AlignBottom);

    layout()->addLayout(_layout);

    connect(&_button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(&_button_box, &QDialogButtonBox::accepted, this, &QDialog::accept);

    ADD_UI(this);
}

MathOptions::~MathOptions()
{
    REMOVE_UI(this);
}

void MathOptions::retranslateUi()
{
    _enable->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE), "Enable"));
    _math_group->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MATH_TYPE), "Math Type"));
    _src1_group->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_1ST_SOURCE), "1st Source"));
    _src2_group->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_2ST_SOURCE), "2st Source"));
    setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MATH_OPTIONS), "Math Options"));
}

void MathOptions::accept()
{
    using namespace Qt;
    QDialog::accept();
}

void MathOptions::Apply()
{
    int src1 = -1;
    int src2 = -1;
    data::MathStack::MathType type = data::MathStack::MATH_ADD;
    for (QVector<QRadioButton *>::const_iterator i = _src1_radio.begin();
        i != _src1_radio.end(); i++) {
        if ((*i)->isChecked()) {
            src1 = (*i)->property("index").toInt();
            break;
        }
    }
    for (QVector<QRadioButton *>::const_iterator i = _src2_radio.begin();
        i != _src2_radio.end(); i++) {
        if ((*i)->isChecked()) {
            src2 = (*i)->property("index").toInt();
            break;
        }
    }
    for (QVector<QRadioButton *>::const_iterator i = _math_radio.begin();
        i != _math_radio.end(); i++) {
        if ((*i)->isChecked()) {
            type = (data::MathStack::MathType)(*i)->property("type").toInt();
            break;
        }
    }
    bool enable = (src1 != -1 && src2 != -1 && _enable->isChecked());

    // math_rebuild now takes channel indices directly (int) instead of
    // view::DsoSignal pointers, so no Signal lookup is required here.
    if (src1 != -1 && src2 != -1){
        _session->math_rebuild(enable, src1, src2, type);
    }
}

void MathOptions::reject()
{
    using namespace Qt;
    QDialog::reject();
}

void MathOptions::UpdateLanguage()
{
    retranslateUi();
}

void MathOptions::UpdateTheme()
{
}

void MathOptions::UpdateFont()
{ 
    QFont font = theme_font_dialog();
    ui::set_form_font(this, font);
}

} // namespace dialogs
} // namespace pv
