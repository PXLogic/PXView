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

#include "pv/dialogs/lissajousoptions.h"
#include <QCheckBox>
#include <QVariant>
#include <QLabel>
#include <QTabBar>
#include <QBitmap>
#include <cmath>

#include "pv/session/sigsession.h"
#include "pv/data/stack/lissajousmodel.h"
#include "pv/view/view.h"
#include "pv/view/trace/lissajoustrace.h"
#include "pv/ui/langresource.h"
#include "pv/ui/fn.h"
#include "pv/ui/dockfonts.h"
#include "pv/config/appconfig.h"

using namespace std;
using namespace pv::view;

namespace pv {
namespace dialogs {

LissajousOptions::LissajousOptions(SigSession *session, QWidget *parent) :
    PxDialog(parent),
    _session(session), _data_src(session),
    _button_box(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        Qt::Horizontal, this)
{
    _enable = nullptr;
    _x_group = nullptr;
    _y_group = nullptr;
    _percent = nullptr;
    _layout = nullptr;

    setMinimumSize(300, 300);

    _enable = new QCheckBox(this);

    QLabel *lisa_label = new QLabel(this);
    lisa_label->setPixmap(QPixmap(":/icons/lissajous.svg"));

    _percent = new QSlider(Qt::Horizontal, this);
    _percent->setRange(100, 100);
    _percent->setEnabled(false);
    if (_data_src->cur_samplelimits() > WellLen) {
        int min = ceil(WellLen*100.0/_data_src->cur_samplelimits());
        _percent->setEnabled(true);
        _percent->setRange(min, 100);
        _percent->setValue(min);
    }

    _x_group = new QGroupBox(this);
    _y_group = new QGroupBox(this);
    QHBoxLayout *xlayout = new QHBoxLayout();
    QHBoxLayout *ylayout = new QHBoxLayout();

    xlayout->setContentsMargins(5, 15, 5, 5);
    ylayout->setContentsMargins(5, 15, 5, 5);

    for(auto m : _data_src->get_signal_models()) {
        if (m->type() == SR_CHANNEL_DSO) {
            QString index_str = QString::number(m->index());
            QRadioButton *xradio = new QRadioButton(index_str, _x_group);
            xradio->setProperty("index", m->index());
            xlayout->addWidget(xradio);
            QRadioButton *yradio = new QRadioButton(index_str, _y_group);
            yradio->setProperty("index", m->index());
            ylayout->addWidget(yradio);
            _x_radio.append(xradio);
            _y_radio.append(yradio);
        }
    }
    _x_group->setLayout(xlayout);
    _y_group->setLayout(ylayout);


    auto lissajous = _data_src->get_lissajous_model();
    if (lissajous) {
        _enable->setChecked(lissajous->enabled());
        _percent->setValue(lissajous->percent());
        for (QVector<QRadioButton *>::const_iterator i = _x_radio.begin();
            i != _x_radio.end(); i++) {
            if ((*i)->property("index").toInt() == lissajous->x_index()) {
               (*i)->setChecked(true);
                break;
            }
        }
        for (QVector<QRadioButton *>::const_iterator i = _y_radio.begin();
            i != _y_radio.end(); i++) {
            if ((*i)->property("index").toInt() == lissajous->y_index()) {
               (*i)->setChecked(true);
                break;
            }
        }
    } else {
        _enable->setChecked(false);
        for (QVector<QRadioButton *>::const_iterator i = _x_radio.begin();
            i != _x_radio.end(); i++) {
           (*i)->setChecked(true);
            break;
        }
        for (QVector<QRadioButton *>::const_iterator i = _y_radio.begin();
            i != _y_radio.end(); i++) {
           (*i)->setChecked(true);
            break;
        }
    }

    _layout = new QGridLayout();
    _layout->setSpacing(0);
    _layout->addWidget(lisa_label, 0, 0, 1, 2, Qt::AlignCenter);
    _layout->addWidget(_enable, 1, 0, 1, 1);
    _layout->addWidget(_percent, 2, 0, 1, 2);
    _layout->addWidget(_x_group, 3, 0, 1, 1);
    _layout->addWidget(_y_group, 3, 1, 1, 1);
    _layout->addWidget(new QLabel(this), 4, 1, 1, 1);
    _layout->addWidget(&_button_box, 5, 1, 1, 1, Qt::AlignHCenter | Qt::AlignBottom);

    layout()->addLayout(_layout);

    connect(&_button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(&_button_box, &QDialogButtonBox::accepted, this, &QDialog::accept);

   ADD_UI(this);
}

LissajousOptions::~LissajousOptions()
{
    REMOVE_UI(this);
}

void LissajousOptions::retranslateUi()
{
    _enable->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE), "Enable"));
    _x_group->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_X_AXIS), "X-axis"));
    _y_group->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_Y_AXIS), "Y-axis"));
    setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LISSAJOUS_OPTIONS), "Lissajous Options"));
}

void LissajousOptions::accept()
{
	using namespace Qt;
    QDialog::accept();

    int xindex = -1;
    int yindex = -1;
    for (QVector<QRadioButton *>::const_iterator i = _x_radio.begin();
        i != _x_radio.end(); i++) {
        if ((*i)->isChecked()) {
            xindex = (*i)->property("index").toInt();
            break;
        }
    }
    for (QVector<QRadioButton *>::const_iterator i = _y_radio.begin();
        i != _y_radio.end(); i++) {
        if ((*i)->isChecked()) {
            yindex = (*i)->property("index").toInt();
            break;
        }
    }
    bool enable = (xindex != -1 && yindex != -1 && _enable->isChecked());
    _session->lissajous_rebuild(enable, xindex, yindex, _percent->value());

    // TODO: adapt — toggling DsoSignal visibility (set_show) was a UI-side
    // effect previously performed by this dialog when enabling Lissajous.
    // The dialog no longer has access to view::DsoSignal (no View pointer)
    // and view::MathTrace/MathStack has no set_show accessor. The View layer
    // should observe the LissajousModel enabled state and toggle trace
    // visibility itself. Skip the call for now.
    (void)enable;
}

void LissajousOptions::reject()
{
    using namespace Qt;
    QDialog::reject();
}

void LissajousOptions::UpdateLanguage()
{
    retranslateUi();
}

void LissajousOptions::UpdateTheme()
{

}

void LissajousOptions::UpdateFont()
{ 
    QFont font = theme_font_dialog();
    ui::set_form_font(this, font);
}

} // namespace dialogs
} // namespace pv
