/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2025 DreamSourceLab <support@dreamsourcelab.com>
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

#include "functiondock.h"

#include <QHBoxLayout>
#include <QIcon>
#include <cmath>

#include "../sigsession.h"
#include "../data/lissajousmodel.h"
#include "../data/signalmodel.h"
#include "../dialogs/fftoptions.h"
#include "../dialogs/mathoptions.h"
#include "../ui/langresource.h"
#include "../ui/dockfonts.h"
#include "../config/appconfig.h"
#include "../pxvdef.h"
#include "../log.h"

namespace pv {
namespace dock {

static const int WellLen = SR_Kn(16);

FunctionDock::FunctionDock(QWidget *parent, SigSession *session)
    : SmoothScrollArea(parent), _session(session), _signals(session), _data(session)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);

    _content = new QWidget(this);
    setWidget(_content);

    QVBoxLayout *mainLay = new QVBoxLayout(_content);
    mainLay->setContentsMargins(8, 8, 8, 8);
    mainLay->setSpacing(10);

    // ---- FFT / Math button row ----
    QHBoxLayout *btnLay = new QHBoxLayout();
    btnLay->setSpacing(6);
    _fft_btn = new QPushButton(this);
    _math_btn = new QPushButton(this);
    btnLay->addWidget(_fft_btn);
    btnLay->addWidget(_math_btn);
    btnLay->addStretch();
    mainLay->addLayout(btnLay);

    connect(_fft_btn, &QPushButton::clicked, this, &FunctionDock::on_fft_clicked);
    connect(_math_btn, &QPushButton::clicked, this, &FunctionDock::on_math_clicked);

    // ---- Lissajous section ----
    _lisa_enable = new QCheckBox(this);
    mainLay->addWidget(_lisa_enable);

    _lisa_percent = new QSlider(Qt::Horizontal, this);
    _lisa_percent->setRange(100, 100);
    _lisa_percent->setEnabled(false);
    mainLay->addWidget(_lisa_percent);

    _x_group = new QGroupBox(this);
    _y_group = new QGroupBox(this);
    QHBoxLayout *xlay = new QHBoxLayout();
    QHBoxLayout *ylay = new QHBoxLayout();
    xlay->setContentsMargins(5, 15, 5, 5);
    ylay->setContentsMargins(5, 15, 5, 5);

    // Build radio buttons from DSO signal models
    if (_session) {
        for (auto &m : _signals->get_signal_models()) {
            if (m->type() == SR_CHANNEL_DSO) {
                QString idx_str = QString::number(m->index());
                QRadioButton *xr = new QRadioButton(idx_str, _x_group);
                xr->setProperty("index", m->index());
                xlay->addWidget(xr);
                _x_radio.append(xr);

                QRadioButton *yr = new QRadioButton(idx_str, _y_group);
                yr->setProperty("index", m->index());
                ylay->addWidget(yr);
                _y_radio.append(yr);
            }
        }
    }

    // Default-select first channel for X and Y
    if (!_x_radio.isEmpty())
        _x_radio[0]->setChecked(true);
    if (!_y_radio.isEmpty())
        _y_radio[0]->setChecked(true);

    _x_group->setLayout(xlay);
    _y_group->setLayout(ylay);

    QHBoxLayout *xyLay = new QHBoxLayout();
    xyLay->setSpacing(6);
    xyLay->addWidget(_x_group);
    xyLay->addWidget(_y_group);
    mainLay->addLayout(xyLay);

    // Restore state from existing LissajousModel
    if (_session) {
        auto *lisa = _signals->get_lissajous_model();
        if (lisa) {
            _lisa_enable->setChecked(lisa->enabled());
            _lisa_percent->setValue(lisa->percent());
            for (auto *r : _x_radio) {
                if (r->property("index").toInt() == lisa->x_index()) {
                    r->setChecked(true);
                    break;
                }
            }
            for (auto *r : _y_radio) {
                if (r->property("index").toInt() == lisa->y_index()) {
                    r->setChecked(true);
                    break;
                }
            }
        }
    }

    // Slider range
    if (_session && _data->cur_samplelimits() > WellLen) {
        int min = ceil(WellLen * 100.0 / _data->cur_samplelimits());
        _lisa_percent->setEnabled(true);
        _lisa_percent->setRange(min, 100);
        _lisa_percent->setValue(min);
    }

    connect(_lisa_enable, &QCheckBox::stateChanged,
            this, &FunctionDock::on_lissajous_enable_changed);
    connect(_lisa_percent, &QSlider::valueChanged,
            this, &FunctionDock::on_lissajous_percent_changed);

    // Connect radio buttons for channel selection changes
    for (auto *r : _x_radio)
        connect(r, &QRadioButton::toggled, this, &FunctionDock::on_lissajous_channel_changed);
    for (auto *r : _y_radio)
        connect(r, &QRadioButton::toggled, this, &FunctionDock::on_lissajous_channel_changed);

    mainLay->addStretch();

    ADD_UI(this);
    retranslateUi();
    reStyle();
}

FunctionDock::~FunctionDock()
{
    REMOVE_UI(this);
}

void FunctionDock::reload()
{
    // Rebuild radio buttons if DSO channel count changed
    // Clear old radios
    for (auto *r : _x_radio)
        delete r;
    for (auto *r : _y_radio)
        delete r;
    _x_radio.clear();
    _y_radio.clear();

    QHBoxLayout *xlay = new QHBoxLayout();
    QHBoxLayout *ylay = new QHBoxLayout();
    xlay->setContentsMargins(5, 15, 5, 5);
    ylay->setContentsMargins(5, 15, 5, 5);

    if (_session) {
        for (auto &m : _signals->get_signal_models()) {
            if (m->type() == SR_CHANNEL_DSO) {
                QString idx_str = QString::number(m->index());
                QRadioButton *xr = new QRadioButton(idx_str, _x_group);
                xr->setProperty("index", m->index());
                xlay->addWidget(xr);
                _x_radio.append(xr);

                QRadioButton *yr = new QRadioButton(idx_str, _y_group);
                yr->setProperty("index", m->index());
                ylay->addWidget(yr);
                _y_radio.append(yr);
            }
        }
    }

    // Delete old layout and set new
    if (_x_group->layout()) {
        QLayout *old = _x_group->layout();
        _x_group->setLayout(nullptr);
        delete old;
    }
    if (_y_group->layout()) {
        QLayout *old = _y_group->layout();
        _y_group->setLayout(nullptr);
        delete old;
    }
    _x_group->setLayout(xlay);
    _y_group->setLayout(ylay);

    if (!_x_radio.isEmpty())
        _x_radio[0]->setChecked(true);
    if (!_y_radio.isEmpty())
        _y_radio[0]->setChecked(true);

    // Connect radio buttons for channel selection changes
    for (auto *r : _x_radio)
        connect(r, &QRadioButton::toggled, this, &FunctionDock::on_lissajous_channel_changed);
    for (auto *r : _y_radio)
        connect(r, &QRadioButton::toggled, this, &FunctionDock::on_lissajous_channel_changed);

    // Restore state
    if (_session) {
        auto *lisa = _signals->get_lissajous_model();
        if (lisa) {
            _lisa_enable->setChecked(lisa->enabled());
            _lisa_percent->setValue(lisa->percent());
            for (auto *r : _x_radio) {
                if (r->property("index").toInt() == lisa->x_index()) {
                    r->setChecked(true);
                    break;
                }
            }
            for (auto *r : _y_radio) {
                if (r->property("index").toInt() == lisa->y_index()) {
                    r->setChecked(true);
                    break;
                }
            }
        }
    }
}

void FunctionDock::on_fft_clicked()
{
    pv::dialogs::FftOptions dlg(this, _session);
    dlg.exec();
}

void FunctionDock::on_math_clicked()
{
    pv::dialogs::MathOptions dlg(_session, this);
    if (dlg.exec() == QDialog::Accepted)
        dlg.Apply();
}

void FunctionDock::on_lissajous_enable_changed(int state)
{
    bool enable = (state == Qt::Checked);
    _lisa_percent->setEnabled(enable);

    int xindex = -1, yindex = -1;
    for (auto *r : _x_radio) {
        if (r->isChecked()) {
            xindex = r->property("index").toInt();
            break;
        }
    }
    for (auto *r : _y_radio) {
        if (r->isChecked()) {
            yindex = r->property("index").toInt();
            break;
        }
    }

    if (enable && (xindex < 0 || yindex < 0)) {
        pxv_warn("%s", "FunctionDock: cannot enable Lissajous — no X/Y channel selected");
        return;
    }

    _session->lissajous_rebuild(enable, xindex, yindex,
                                _lisa_percent->value());
}

void FunctionDock::on_lissajous_percent_changed(int val)
{
    if (!_lisa_enable->isChecked())
        return;

    int xindex = -1, yindex = -1;
    for (auto *r : _x_radio) {
        if (r->isChecked()) {
            xindex = r->property("index").toInt();
            break;
        }
    }
    for (auto *r : _y_radio) {
        if (r->isChecked()) {
            yindex = r->property("index").toInt();
            break;
        }
    }

    _session->lissajous_rebuild(true, xindex, yindex, val);
}

void FunctionDock::on_lissajous_channel_changed()
{
    if (!_lisa_enable->isChecked())
        return;
    on_lissajous_percent_changed(_lisa_percent->value());
}

void FunctionDock::retranslateUi()
{
    _fft_btn->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FUNCTION_FFT), "FFT"));
    _math_btn->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FUNCTION_MATH), "Math"));
    _lisa_enable->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LISSAJOUS_FIGURE), "Lissajous Figure"));
    _x_group->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_X_AXIS), "X-axis"));
    _y_group->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_Y_AXIS), "Y-axis"));
}

void FunctionDock::reStyle()
{
    QString iconPath = GetIconPath();
    _fft_btn->setIcon(QIcon(iconPath + "/fft.svg"));
    _math_btn->setIcon(QIcon(iconPath + "/math.svg"));
}

void FunctionDock::UpdateLanguage()
{
    retranslateUi();
}

void FunctionDock::UpdateTheme()
{
    reStyle();
}

void FunctionDock::UpdateFont()
{
    QFont f = dock_font_content();
    _content->setFont(f);
    _fft_btn->setFont(f);
    _math_btn->setFont(f);
    _lisa_enable->setFont(f);
    _x_group->setFont(f);
    _y_group->setFont(f);
}

} // namespace dock
} // namespace pv
