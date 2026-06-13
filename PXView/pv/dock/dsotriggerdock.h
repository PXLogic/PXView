/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
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

#ifndef PXVIEW_PV_DSOTRIGGERDOCK_H
#define PXVIEW_PV_DSOTRIGGERDOCK_H

#include <QDockWidget>
#include <QSlider>
#include <QSpinBox>
#include <QButtonGroup>
#include <QScrollArea>
#include <QLabel>
#include <QRadioButton>
#include <vector>
#include <QJsonObject>
#include "../ui/dscombobox.h"
#include "../widgets/smoothscrollarea.h"
#include "../interface/icallbacks.h"
#include "../ui/uimanager.h"
#include "keywordlineedit.h"
#include "../interface/icontextaware.h"

namespace pv {

class SigSession;

namespace dock {

class DsoTriggerDock : public pv::widgets::SmoothScrollArea, public IUiWindow, public IContextAware
{
    Q_OBJECT

public:
    DsoTriggerDock(QWidget *parent, SigSession *session);
    ~DsoTriggerDock();
 
    void device_change();
    void update_view();
    void check_setting();

    QJsonObject get_session();
    void set_session(QJsonObject &obj);

    void bind_context(TabContext *ctx) override;
    void unbind_context() override;

private:
    void paintEvent(QPaintEvent *e);
    void retranslateUi();
    void reStyle();
    bool check_trig_channel();

    //IUiWindow
    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

signals:
    void set_trig_pos(int percent);

public slots:
    void auto_trig(int index);

private slots:
    void pos_changed(int pos);
    void hold_changed(int hold);
    void margin_changed(int margin);
    void source_changed();
    void type_changed();
    void channel_changed(int ch);

private:
    SigSession *_session;

    QWidget *_widget;

    DsComboBox *_holdoff_comboBox;
    PopupLineEdit *_holdoff_spinBox;
    QSlider *_holdoff_slider;

    QSlider *_margin_slider;

    PopupLineEdit *_position_spinBox;
    QSlider *_position_slider;

    QButtonGroup *_source_group;
    DsComboBox *_channel_comboBox;
    QButtonGroup *_type_group;

    QLabel *_position_label;
    QLabel *_holdoff_label;
    QLabel *_margin_label;
    QLabel *_tSource_label;
    QLabel *_tType_label;
    QRadioButton *_rising_radioButton;
    QRadioButton *_falling_radioButton;

    QRadioButton *_auto_radioButton;
    QRadioButton *_ch0_radioButton;
    QRadioButton *_ch1_radioButton;
    QRadioButton *_ch0a1_radioButton;
    QRadioButton *_ch0o1_radioButton;
    TabContext *_context;
};

} // namespace dock
} // namespace pv

#endif // PXVIEW_PV_DSOTRIGGERDOCK_H
