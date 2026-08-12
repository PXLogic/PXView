/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
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

#ifndef PXVIEW_PV_DOCK_FUNCTIONDOCK_H
#define PXVIEW_PV_DOCK_FUNCTIONDOCK_H

#include <QWidget>
#include <QCheckBox>
#include <QGroupBox>
#include <QRadioButton>
#include <QSlider>
#include <QPushButton>
#include <QLabel>
#include <QVector>
#include <QVBoxLayout>
#include <QGridLayout>

#include "pv/ui/uimanager.h"
#include "pv/widgets/smoothscrollarea.h"

namespace pv {

class SigSession;
namespace data { class ISignalSource; class IDataSource; }

namespace dock {

/// FunctionDock provides an inline panel for FFT, Math, and Lissajous
/// controls. Replaces the old popup QMenu approach so the user can
/// adjust Lissajous parameters without a modal dialog.
class FunctionDock : public pv::widgets::SmoothScrollArea, public IUiWindow
{
    Q_OBJECT

public:
    FunctionDock(QWidget *parent, SigSession *session);
    ~FunctionDock();

    void reload();

private:
    void retranslateUi();
    void reStyle();

    // IUiWindow
    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

private slots:
    void on_fft_clicked();
    void on_math_clicked();
    void on_lissajous_enable_changed(int state);
    void on_lissajous_percent_changed(int val);
    void on_lissajous_channel_changed();

private:
    SigSession *_session;
    data::ISignalSource *_signals = nullptr;
  data::IDataSource *_data = nullptr;

    // FFT / Math buttons
    QPushButton *_fft_btn;
    QPushButton *_math_btn;

    // Lissajous controls
    QCheckBox   *_lisa_enable;
    QGroupBox   *_x_group;
    QGroupBox   *_y_group;
    QSlider     *_lisa_percent;
    QVector<QRadioButton *> _x_radio;
    QVector<QRadioButton *> _y_radio;

    QWidget     *_content;
};

} // namespace dock
} // namespace pv

#endif // PXVIEW_PV_DOCK_FUNCTIONDOCK_H
