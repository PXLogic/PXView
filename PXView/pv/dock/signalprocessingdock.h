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

#ifndef PXVIEW_PV_SIGNALPROCESSINGDOCK_H
#define PXVIEW_PV_SIGNALPROCESSINGDOCK_H

#include <QCheckBox>
#include "../ui/dscombobox.h"
#include <QFrame>
#include <QGridLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <vector>

#include "../deviceagent.h"
#include "../interface/icontextaware.h"
#include "../sigsession.h"
#include "../ui/dsspinbox.h"
#include "../ui/uimanager.h"

namespace pv {
namespace dock {

class SignalProcessingDock : public QWidget,
                             public IUiWindow,
                             public IContextAware {
  Q_OBJECT

public:
  SignalProcessingDock(QWidget *parent, SigSession *session);
  ~SignalProcessingDock();

  void update_view();
  void device_updated();
  void update_widgets_status();
  void update_invert_state();
  void update_glitch_filter_state();
  void auto_apply_settings();

  void bind_context(TabContext *ctx) override;
  void unbind_context() override;

  QJsonObject get_session();
  void set_session(QJsonObject &obj);

signals:
  void settings_applied();

private:
  void build_ui();
  void build_invert_panel();
  void build_glitch_filter_panel();
  void rebuild_panels();

  void UpdateLanguage() override;
  void UpdateTheme() override;
  void UpdateFont() override;

  void showEvent(QShowEvent *event) override;
  void hideEvent(QHideEvent *event) override;

private slots:
  void on_apply_invert();
  void on_restore_original_data();
  void on_invert_select_all();
  void on_invert_deselect_all();
  void on_apply_glitch_filter();
  void on_restore_glitch_data();
  void on_glitch_select_all();
  void on_glitch_deselect_all();

private:
  SigSession *_session;
  TabContext *_context;
  DeviceAgent *_device_agent;

  QWidget *_container_panel;
  QVBoxLayout *_container_lay;
  QWidget *_invert_group;
  QWidget *_glitch_filter_group;
  QWidget *_no_logic_hint;

  std::vector<QCheckBox *> _invert_checkBox_list;
  std::vector<QCheckBox *> _glitch_checkBox_list;
  std::vector<pv::ui::DsSpinBox *> _glitch_spinbox_list;

  QPushButton *_apply_invert_btn;
  QPushButton *_restore_invert_btn;
  QLabel *_invert_status_label;

  QPushButton *_apply_filter_btn;
  QPushButton *_restore_data_btn;
  QLabel *_filter_status_label;
  std::vector<DsComboBox *> _glitch_mode_combo_list;
};

} // namespace dock
} // namespace pv

#endif
