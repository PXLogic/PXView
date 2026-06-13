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

#ifndef PXVIEW_PV_DEVICEOPTIONSDOCK_H
#define PXVIEW_PV_DEVICEOPTIONSDOCK_H

#include <QCheckBox>
#include <QFrame>
#include <QGridLayout>
#include <QJsonObject>
#include <QLabel>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <vector>

#include "../deviceagent.h"
#include "../dialogs/deviceoptions.h"
#include "../interface/icontextaware.h"
#include "../prop/binding/deviceoptions.h"
#include "../prop/binding/probeoptions.h"
#include "../sigsession.h"
#include "../ui/uimanager.h"

namespace pv {

namespace dock {

class DeviceOptionsDock : public QWidget,
                          public IUiWindow,
                          public IChannelCheck,
                          public IContextAware {
  Q_OBJECT

public:
  DeviceOptionsDock(QWidget *parent, SigSession *session);
  ~DeviceOptionsDock();

  void update_view();
  void device_updated();
  void update_widgets_status();

  void bind_context(TabContext *ctx) override;
  void unbind_context() override;

  QJsonObject get_session();
  void set_session(QJsonObject &obj);

  void set_sampling_widget(QWidget *widget);

signals:
  void settings_applied();

private:
  QLayout *get_property_form(QWidget *parent);
  void logic_probes(QVBoxLayout &layout);
  void analog_probes(QGridLayout &layout);
  QString dynamic_widget(QLayout *lay);
  void set_all_probes(bool set);
  void enable_max_probes();
  void build_dynamic_panel();
  void update_dynamic_panel_visibility(bool visible);
  void try_resize_scroll();
  void channel_checkbox_clicked(QCheckBox *sc);
  void ChannelChecked(int index, QObject *object);

  void UpdateLanguage() override;
  void UpdateTheme() override;
  void UpdateFont() override;

  void showEvent(QShowEvent *event) override;
  void hideEvent(QHideEvent *event) override;

private slots:
  void on_property_committed();
  void commit_channels();
  void enable_all_probes();
  void disable_all_probes();
  void zero_adj();
  void mode_check_timeout();
  void channel_check();
  void analog_channel_check();
  void on_calibration();
  void on_analog_channel_enable();
  void on_anlog_tab_changed(int index);

private:
  std::vector<QCheckBox *> _probes_checkBox_list;
  QTimer _mode_check_timer;
  int _opt_mode;
  QWidget *_scroll_panel;
  QWidget *_container_panel;
  QVBoxLayout *_container_lay;
  QWidget *_dynamic_panel;
  int _width;
  int _groupHeight1;
  int _groupHeight2;
  volatile bool _isBuilding;
  DeviceAgent *_device_agent;
  int _cur_analog_tag_index;
  QString _demo_operation_mode;
  pv::prop::binding::DeviceOptions *_device_options_binding;
  std::vector<pv::prop::binding::ProbeOptions *> _probe_options_binding_list;
  std::vector<ChannelModePair> _channel_mode_indexs;
  std::vector<struct sr_channel *> _dso_channel_list;
  std::vector<bool> _lst_probe_enabled_status;
  SigSession *_session;
  TabContext *_context;
  QWidget *_sampling_settings_widget;
};

} // namespace dock
} // namespace pv

#endif // PXVIEW_PV_DEVICEOPTIONSDOCK_H
