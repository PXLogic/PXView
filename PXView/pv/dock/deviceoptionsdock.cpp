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

#include "deviceoptionsdock.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QTabWidget>
#include <QThreadPool>
#include <QToolButton>
#include <assert.h>

#include "../appcontrol.h"
#include "../config/appconfig.h"
#include "../data/sessiondocument.h"
#include "../deviceagent.h"
#include "../dsvdef.h"
#include "../interface/icallbacks.h"
#include "../log.h"
#include "../prop/property.h"
#include "../prop/string.h"
#include "../sigsession.h"
#include "../tabcontext.h"
#include "../ui/dockfonts.h"
#include "../ui/fn.h"
#include "../ui/langresource.h"
#include "../ui/msgbox.h"
#include "../ui/toast.h"
#include "../view/view.h"


using namespace boost;
using namespace std;

namespace pv {
namespace dock {

DeviceOptionsDock::DeviceOptionsDock(QWidget *parent, SigSession *session)
    : QWidget(parent), _session(session), _context(nullptr) {
  _scroll_panel = NULL;
  _container_panel = NULL;
  _container_lay = NULL;
  _dynamic_panel = NULL;
  _width = 0;
  _groupHeight1 = 0;
  _groupHeight2 = 0;
  _isBuilding = false;
  _cur_analog_tag_index = 0;
  _opt_mode = 0;
  _sampling_settings_widget = NULL;

  _device_agent = session->get_device();
  _device_options_binding = NULL;

  this->setObjectName("dock_device_options_scroll");

  _container_panel = new QWidget();
  _container_lay = new QVBoxLayout();
  _container_lay->setDirection(QBoxLayout::TopToBottom);
  _container_lay->setAlignment(Qt::AlignTop);
  _container_lay->setContentsMargins(12, 10, 12, 10);
  _container_lay->setSpacing(10);
  _container_panel->setLayout(_container_lay);

  QVBoxLayout *mainLay = new QVBoxLayout(this);
  mainLay->setContentsMargins(0, 0, 0, 0);
  mainLay->setSpacing(0);
  mainLay->addWidget(_container_panel);
  mainLay->addStretch();

  if (_device_agent->have_instance()) {
    _device_options_binding = new pv::prop::binding::DeviceOptions(_session);

    if (_sampling_settings_widget) {
      _container_lay->addWidget(_sampling_settings_widget);

      QFrame *sep0 = new QFrame(_container_panel);
      sep0->setObjectName("dock_section_separator");
      sep0->setFrameShape(QFrame::HLine);
      _container_lay->addWidget(sep0);
    }

    this->build_dynamic_panel();

    // 通道和 Mode 之间的分隔线
    QFrame *sep_mode = new QFrame(_container_panel);
    sep_mode->setObjectName("dock_section_separator");
    sep_mode->setFrameShape(QFrame::HLine);
    _container_lay->addWidget(sep_mode);

    // Mode 部分
    QLabel *mode_title = new QLabel(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MODE), "Mode"), _container_panel);
    mode_title->setObjectName("dock_section_title");
    mode_title->setFont(dock_font_section_title());
    mode_title->setProperty("lang_id", S_ID(IDS_DLG_MODE));
    QWidget *mode_section = new QWidget(_container_panel);
    mode_section->setObjectName("dock_mode_section");
    QVBoxLayout *mode_vbox = new QVBoxLayout(mode_section);
    mode_vbox->setContentsMargins(0, 0, 0, 0);
    mode_vbox->setSpacing(5);
    mode_vbox->addWidget(mode_title);
    QWidget *mode_inner = new QWidget(mode_section);
    QLayout *props_lay = get_property_form(mode_inner);
    props_lay->setContentsMargins(5, 8, 5, 10);
    mode_vbox->addWidget(mode_inner);
    mode_vbox->setAlignment(Qt::AlignTop);
    _container_lay->addWidget(mode_section);

    _device_agent->get_config_string(SR_CONF_OPERATION_MODE, _opt_mode);

    if (_device_agent->is_demo())
      _demo_operation_mode = _device_agent->get_demo_operation_mode();

    try_resize_scroll();
  }

  connect(&_mode_check_timer, &QTimer::timeout, this,
          &DeviceOptionsDock::mode_check_timeout);

  _mode_check_timer.setInterval(500);

  ADD_UI(this);
}

DeviceOptionsDock::~DeviceOptionsDock() {
  REMOVE_UI(this);
  for (auto ptr : _probe_options_binding_list) {
    const auto &props = ptr->properties();
    for (auto p : props) {
      delete p;
    }
    delete ptr;
  }
  _probe_options_binding_list.clear();

  if (_device_options_binding) {
    const auto &dev_props = _device_options_binding->properties();
    for (auto p : dev_props) {
      delete p;
    }
    delete _device_options_binding;
    _device_options_binding = NULL;
  }
}

void DeviceOptionsDock::ChannelChecked(int index, QObject *object) {
  (void)index;

  QCheckBox *sc = dynamic_cast<QCheckBox *>(object);
  channel_checkbox_clicked(sc);
  commit_channels();
}

void DeviceOptionsDock::on_property_committed() { emit settings_applied(); }

void DeviceOptionsDock::commit_channels() {
  using namespace Qt;
  bool hasEnabled = false;

  int mode = _device_agent->get_work_mode();
  if (mode == LOGIC || mode == ANALOG || mode == MSO) {
    int index = 0;
    for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
      sr_channel *const probe = (sr_channel *)l->data;
      if (!probe) {
        pxv_warn("%s", "DeviceOptionsDock: probe is NULL in channel loop, skipping");
        continue;
      }
      assert(probe);
      // _probes_checkBox_list alignment varies by mode:
      //   LOGIC/MSO — logic_probes() pushes a checkbox for every channel
      //               (DSO channels get a hidden checkbox), so the list is
      //               1:1 with get_channels().
      //   ANALOG    — analog_probes() only pushes checkboxes for ANALOG
      //               channels (skips LOGIC/DSO). Iterating all channels
      //               with a single index would run past the end of the
      //               list (vector::_M_range_check). Skip channels that
      //               have no checkbox in this mode.
      if (mode == ANALOG && probe->type != SR_CHANNEL_ANALOG) {
        continue;
      }
      if (index >= (int)_probes_checkBox_list.size()) {
        pxv_warn("commit_channels: index %d >= _probes_checkBox_list size %d "
                 "(mode=%d, ch[%d] '%s' type=%d) — list out of sync, skipping",
                 index, (int)_probes_checkBox_list.size(), mode,
                 probe->index, probe->name ? probe->name : "(null)",
                 probe->type);
        break;
      }
      probe->enabled = _probes_checkBox_list.at(index)->isChecked();
      index++;
      if (probe->enabled)
        hasEnabled = true;
    }
  } else {
    hasEnabled = true;
  }

  if (hasEnabled) {
    auto it = _probe_options_binding_list.begin();
    while (it != _probe_options_binding_list.end()) {
      const auto &probe_props = (*it)->properties();

      for (auto p : probe_props) {
        p->commit();
      }
      it++;
    }

    QTimer::singleShot(0, this, [this]() {
      _session->broadcast_async<interface::DeviceOptionsUpdated>({});
      _session->broadcast_async<interface::EndDeviceOptions>({});
      emit settings_applied();
    });
  } else {
    QString strMsg(
        L_S(STR_PAGE_MSG, S_ID(IDS_MSG_ALL_CHANNEL_DISABLE),
            "All channel disabled! Please enable at least one channel."));
    pv::ui::Toast::show(this, strMsg, pv::ui::Toast::Warning);
  }
}

QLayout *DeviceOptionsDock::get_property_form(QWidget *parent) {
  QGridLayout *const layout = new QGridLayout(parent);
  layout->setVerticalSpacing(6);
  const auto &properties = _device_options_binding->properties();

  QFont labelFont = dock_font_label();
  QFont contentFont = dock_font_content();
  QFontMetrics fm(labelFont);

  int maxLabelWidth = 0;
  int i = 0;
  for (auto p : properties) {
    const QString label = p->labeled_widget() ? QString() : p->label();
    QString lable_text = "";

    if (label != "") {
      QByteArray bytes = label.toLocal8Bit();
      const char *lang_str = LangResource::Instance()->get_lang_text(
          STR_PAGE_DSL, bytes.data(), bytes.data());
      lable_text = QString::fromUtf8(lang_str);
    }

    QWidget *wid = p->get_widget_live(parent);

    // Property::get_widget may return NULL when the underlying getter fails
    // (e.g. driver doesn't support the key). Skip such properties instead of
    // dereferencing a NULL widget pointer.
    if (!wid) {
      pxv_info("DeviceOptionsDock: skipping property '%s' — get_widget returned NULL",
               p->label().toUtf8().constData());
      continue;
    }

    if (p->labeled_widget()) {
      wid->setFont(contentFont);
      wid->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      layout->addWidget(wid, i, 0, 1, 3);
    } else {
      QLabel *lb = new QLabel(lable_text, parent);
      lb->setObjectName("dock_label");
      lb->setFont(labelFont);
      lb->setProperty("lang_src", label);
      lb->setProperty("lang_page", STR_PAGE_DSL);
      layout->addWidget(lb, i, 0, Qt::AlignRight | Qt::AlignVCenter);

      // For path/dir properties, split the browse button and line edit
      // into separate grid columns so all line edits stay equal width.
      prop::String *sp = dynamic_cast<prop::String *>(p);
      if (sp && sp->is_path_or_dir()) {
        QToolButton *btn = sp->get_browse_btn();
        QLineEdit *lineEdit = sp->get_line_edit();

        QLayout *cl = wid->layout();
        if (cl) {
          while (cl->count() > 0)
            cl->takeAt(0);
        }

        if (btn) {
          btn->setParent(parent);
          btn->setFont(contentFont);
          btn->setFixedWidth(28);
          layout->addWidget(btn, i, 1, Qt::AlignVCenter);
        }
        if (lineEdit) {
          lineEdit->setParent(parent);
          lineEdit->setFont(contentFont);
          lineEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
          layout->addWidget(lineEdit, i, 2);
        }
        wid->hide();
      } else {
        wid->setFont(contentFont);
        wid->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        layout->addWidget(wid, i, 2);
      }

      int labelWidth = fm.boundingRect(lable_text).width() + 15;
      if (labelWidth > maxLabelWidth)
        maxLabelWidth = labelWidth;
    }

    layout->setRowMinimumHeight(i, 28);

    connect(p, &pv::prop::Property::committed, this,
            &DeviceOptionsDock::on_property_committed);

    i++;
  }

  for (int row = 0; row < layout->rowCount(); row++) {
    QLayoutItem *labelItem = layout->itemAtPosition(row, 0);
    if (labelItem && labelItem->widget()) {
      QLabel *lb = qobject_cast<QLabel *>(labelItem->widget());
      if (lb)
        lb->setFixedWidth(maxLabelWidth);
    }
  }

  layout->setColumnMinimumWidth(0, maxLabelWidth);
  layout->setColumnStretch(0, 0);
  layout->setColumnStretch(1, 0);
  layout->setColumnStretch(2, 1);
  layout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  return layout;
}

void DeviceOptionsDock::logic_probes(QVBoxLayout &layout) {
  using namespace Qt;

  layout.setSpacing(6);

  int row1 = 0;
  int row2 = 0;
  // vld_ch_num: "当前可启用的最大通道数".
  //  - PXLogic used to expose this via the fork key SR_CONF_VLD_CH_NUM (deleted).
  //    Different channel_modes mapped to different hardware resource caps
  //    (32ch@250MHz / 16ch@500MHz / 8ch@1GHz).
  //  - Now we derive the channel count from sdi->channels directly — the
  //    driver's scan() callback already populates this with the maximum
  //    available channels for the device. For PXLogic this equals the
  //    profile's max channel count; for upstream drivers (fx2lafw/demo) it
  //    is the full channel list.
  int vld_ch_num = 0;
  for (const GSList *l = _device_agent->get_channels(); l; l = l->next)
    vld_ch_num++;
  int cur_ch_num = 0;
  int contentHeight = 0;

  _probes_checkBox_list.clear();

  QFont contentFont = dock_font_content();

  if (_device_agent->get_work_mode() == LOGIC) {
    GVariant *gvar_opts =
        _device_agent->get_config_list(NULL, SR_CONF_CHANNEL_MODE);

    if (gvar_opts != NULL) {
      /* Task 10.6: config_list now returns a GVariant string array
       * (g_variant_new_strv) instead of a uint64 bare-pointer cast.
       * Note: g_variant_get_strv returns pointers into the GVariant's
       * internal buffer — the variant must NOT be unref'd until we're
       * done using strs[]. Unref after the loop, just before g_free. */
      gsize n_items;
      const gchar **strs = g_variant_get_strv(gvar_opts, &n_items);

      /* Task 10/Phase 3: driver config_get now returns the current channel
       * mode as a string (channel_mode_str[ch_mode]). Compare against the
       * filtered list strings to highlight the matching radio button —
       * the filtered list index does NOT equal the PX_CHANNEL_ID, so the
       * old `ch_mode == (int)i` comparison was wrong. */
      QString cur_ch_mode;
      _device_agent->get_config_string(SR_CONF_CHANNEL_MODE, cur_ch_mode);
      _channel_mode_indexs.clear();

      for (gsize i = 0; i < n_items; i++) {
        row1++;
        QString mode_bt_text = QString::fromUtf8(
            LangResource::Instance()->get_lang_text(
                STR_PAGE_DSL, strs[i], strs[i]));
        QRadioButton *mode_button = new QRadioButton(mode_bt_text);
        mode_button->setFont(contentFont);
        mode_button->setProperty("lang_src", QString::fromUtf8(strs[i]));
        mode_button->setProperty("lang_page", STR_PAGE_DSL);
        ChannelModePair mode_index;
        mode_index.key = mode_button;
        mode_index.value = QString::fromUtf8(strs[i]);
        _channel_mode_indexs.push_back(mode_index);

        layout.addWidget(mode_button);
        contentHeight += mode_button->sizeHint().height();

        connect(mode_button, &QRadioButton::clicked, this,
                &DeviceOptionsDock::channel_check);

        if (cur_ch_mode == QString::fromUtf8(strs[i]))
          mode_button->setChecked(true);
      }

      g_variant_unref(gvar_opts);
      g_free((gpointer)strs);
    }
  }

  // SR_CONF_VLD_CH_NUM fork key deleted — re-derive vld_ch_num from sdi->channels.
  // (Already computed above, but this code path may reassign based on channel
  // mode selection. For upstream drivers without channel_mode, the count from
  // above is still valid.)
  // No-op: vld_ch_num retains the value computed at the top of logic_probes().

  int channel_columns = 8;
  int channel_line_height = 0;

  // --- Digital (Logic) channel group ---
  QWidget *digital_group = new QWidget();
  QVBoxLayout *digital_lay = new QVBoxLayout(digital_group);
  digital_lay->setContentsMargins(0, 0, 0, 0);
  digital_lay->setSpacing(4);

  QLabel *digital_title = new QLabel(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DIGITAL_CHANNEL), "Digital Channel"),
      digital_group);
  digital_title->setObjectName("dock_section_title");
  digital_title->setFont(dock_font_section_title());
  digital_title->setProperty("lang_id", S_ID(IDS_DLG_DIGITAL_CHANNEL));
  digital_lay->addWidget(digital_title);

  QWidget *digital_grid_widget = new QWidget();
  QGridLayout *digital_grid = new QGridLayout(digital_grid_widget);
  digital_grid->setContentsMargins(0, 0, 0, 0);
  digital_grid->setSpacing(3);
  digital_lay->addWidget(digital_grid_widget);

  // --- Analog channel group ---
  QWidget *analog_group = new QWidget();
  QVBoxLayout *analog_lay = new QVBoxLayout(analog_group);
  analog_lay->setContentsMargins(0, 0, 0, 0);
  analog_lay->setSpacing(4);

  QLabel *analog_title = new QLabel(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ANALOG_CHANNEL), "Analog Channel"),
      analog_group);
  analog_title->setObjectName("dock_section_title");
  analog_title->setFont(dock_font_section_title());
  analog_title->setProperty("lang_id", S_ID(IDS_DLG_ANALOG_CHANNEL));
  analog_lay->addWidget(analog_title);

  QWidget *analog_grid_widget = new QWidget();
  QGridLayout *analog_grid = new QGridLayout(analog_grid_widget);
  analog_grid->setContentsMargins(0, 0, 0, 0);
  analog_grid->setSpacing(3);
  analog_lay->addWidget(analog_grid_widget);

  int digital_row = 0;
  int digital_column = 0;
  int analog_row = 0;
  int analog_column = 0;
  bool has_digital = false;
  bool has_analog = false;
  row2++;

  for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
    sr_channel *const probe = (sr_channel *)l->data;

    if (probe->enabled)
      cur_ch_num++;

    if (cur_ch_num > vld_ch_num)
      probe->enabled = false;

    // DSO channels (oscilloscope) are not part of LOGIC/MSO channel
    // selection — Core (sigsession.cpp init_signals) skips them when
    // building signal models for these modes. Keep a ChannelLabel alive
    // (parented to this dock, hidden) so _probes_checkBox_list stays
    // aligned 1:1 with get_channels() for commit_channels(), but do NOT
    // add it to any visible grid — otherwise O0/O1 show up in the Analog
    // Channel group (demo device, indices 13/14).
    int cur_mode = _device_agent->get_work_mode();
    bool is_dso_hidden = (probe->type == SR_CHANNEL_DSO &&
                          (cur_mode == LOGIC || cur_mode == MSO));
    bool is_analog = (probe->type == SR_CHANNEL_ANALOG ||
                      probe->type == SR_CHANNEL_DSO);
    ChannelLabel *ch_item = new ChannelLabel(
        this, NULL, probe->index,
        is_analog ? ChannelLabel::Analog : ChannelLabel::Logic);

    if (is_dso_hidden) {
      ch_item->setVisible(false);
      _probes_checkBox_list.push_back(ch_item->getCheckBox());
      ch_item->getCheckBox()->setCheckState(probe->enabled ? Qt::Checked
                                                           : Qt::Unchecked);
      continue;
    }

    if (is_analog) {
      analog_grid->addWidget(ch_item, analog_row, analog_column++,
                             Qt::AlignLeft | Qt::AlignTop);
      has_analog = true;
      if (analog_column == channel_columns) {
        analog_column = 0;
        analog_row++;
        if (l->next != NULL)
          row2++;
      }
    } else {
      digital_grid->addWidget(ch_item, digital_row, digital_column++,
                              Qt::AlignLeft | Qt::AlignTop);
      has_digital = true;
      if (digital_column == channel_columns) {
        digital_column = 0;
        digital_row++;
        if (l->next != NULL)
          row2++;
      }
    }

    _probes_checkBox_list.push_back(ch_item->getCheckBox());
    ch_item->getCheckBox()->setCheckState(probe->enabled ? Qt::Checked
                                                         : Qt::Unchecked);
    channel_line_height = ch_item->height();
  }

  for (int c = 0; c < channel_columns; c++) {
    digital_grid->setColumnStretch(c, 0);
    analog_grid->setColumnStretch(c, 0);
  }

  if (has_digital)
    layout.addWidget(digital_group);
  else
    digital_group->setVisible(false);

  // In LOGIC mode, hide the analog channel selection group.
  // The group widget (and its checkboxes) must remain alive so that
  // _probes_checkBox_list indices stay aligned with get_channels().
  if (has_analog) {
    layout.addWidget(analog_group);
    if (_device_agent->get_work_mode() == LOGIC)
      analog_group->setVisible(false);
  } else {
    analog_group->setVisible(false);
  }

  QWidget *space = new QWidget();
  space->setFixedHeight(10);
  layout.addWidget(space);
  contentHeight += 10;

  QHBoxLayout *line_lay = new QHBoxLayout();
  layout.addLayout(line_lay);
  line_lay->setSpacing(10);

  QPushButton *enable_all_probes = new QPushButton(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE_ALL), "Enable All"));
  QPushButton *disable_all_probes = new QPushButton(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISABLE_ALL), "Disable All"));
  enable_all_probes->setMaximumHeight(33);
  disable_all_probes->setMaximumHeight(33);
  enable_all_probes->setObjectName("dock_content");
  disable_all_probes->setObjectName("dock_content");
  enable_all_probes->setProperty("lang_id", S_ID(IDS_DLG_ENABLE_ALL));
  disable_all_probes->setProperty("lang_id", S_ID(IDS_DLG_DISABLE_ALL));
  enable_all_probes->setFont(contentFont);
  disable_all_probes->setFont(contentFont);

  int bt_width = enable_all_probes->fontMetrics().horizontalAdvance(
                     enable_all_probes->text()) +
                 20;

  enable_all_probes->setMinimumWidth(bt_width);
  disable_all_probes->setMinimumWidth(bt_width);

  ::ui::set_dock_form_font(this);

  contentHeight += enable_all_probes->sizeHint().height();
  contentHeight += channel_line_height * row2 + 50;

  connect(enable_all_probes, &QPushButton::clicked, this,
          &DeviceOptionsDock::enable_all_probes);
  connect(disable_all_probes, &QPushButton::clicked, this,
          &DeviceOptionsDock::disable_all_probes);

  line_lay->addWidget(enable_all_probes, 1);
  line_lay->addWidget(disable_all_probes, 1);

  _groupHeight2 = contentHeight + (row1 + row2) * 2 + 38;

#ifdef Q_OS_DARWIN
  _groupHeight2 += 5;
#endif
}

void DeviceOptionsDock::set_all_probes(bool set) {
  setUpdatesEnabled(false);
  for (auto box : _probes_checkBox_list) {
    box->setCheckState(set ? Qt::Checked : Qt::Unchecked);
  }
  setUpdatesEnabled(true);
}

void DeviceOptionsDock::enable_max_probes() {
  int cur_ch_num = 0;
  for (auto box : _probes_checkBox_list) {
    if (box->isChecked())
      cur_ch_num++;
  }

  // SR_CONF_VLD_CH_NUM fork key deleted — derive vld_ch_num from sdi->channels.
  // The driver's scan() callback populates the channel list with the maximum
  // available channels for the device, so this equals the channel_mode cap.
  int vld_ch_num = 0;
  for (const GSList *l = _device_agent->get_channels(); l; l = l->next)
    vld_ch_num++;
  if (vld_ch_num <= 0)
    return;

  for (auto box : _probes_checkBox_list) {
    if (cur_ch_num >= vld_ch_num) {
      break;
    }
    if (box->isChecked() == false) {
      box->setChecked(true);
      cur_ch_num++;
    }
  }
}

void DeviceOptionsDock::enable_all_probes() {
  // SR_CONF_STREAM fork key deleted — use DeviceAgent::is_stream_mode().
  // In stream mode, the device FIFO is small so enabling all channels at
  // once can overflow it; enable_max_probes() picks a safe subset.
  if (_device_agent->is_stream_mode()) {
    enable_max_probes();
    commit_channels();
    return;
  }

  set_all_probes(true);
  commit_channels();
}

void DeviceOptionsDock::disable_all_probes() {
  set_all_probes(false);
  commit_channels();
}

void DeviceOptionsDock::zero_adj() {
  // DSO zero calibration removed: SR_CONF_ZERO fork key was deleted
  // (DSO mode deprecated, DSCope hardware dropped). No-op stub kept so the
  // signal-slot connection from the calibration button compiles.
}

void DeviceOptionsDock::on_calibration() {
  // DSO manual calibration removed: SR_CONF_CALI fork key was deleted.
  // No-op stub kept so the signal-slot connection compiles.
}

void DeviceOptionsDock::mode_check_timeout() {
  if (_isBuilding)
    return;

  if (_device_agent->is_hardware()) {
    DeviceAgent *agent = _device_agent;
    QString saved_opt_mode = _opt_mode;
    QThreadPool::globalInstance()->start([this, agent, saved_opt_mode]() {
      QString mode;
      bool got_mode = agent->get_config_string(SR_CONF_OPERATION_MODE, mode);
      if (!got_mode || mode == saved_opt_mode)
        return;

      QMetaObject::invokeMethod(this, [this, mode]() {
        if (_isBuilding)
          return;
        _opt_mode = mode;
        build_dynamic_panel();
        try_resize_scroll();
      });
    });

    // SR_CONF_TEST fork key deleted from pxlogic.c — the test-mode auto-check
    // branch below would never execute (get_config_bool always returns false).
    // Test mode is now a hardware-specific concept that should be handled in
    // the driver's scan() if needed, not in the application layer.
  } else if (_device_agent->is_demo()) {
    QString opt_mode = _device_agent->get_demo_operation_mode();
    if (opt_mode != _demo_operation_mode) {
      _demo_operation_mode = opt_mode;
      build_dynamic_panel();
      try_resize_scroll();
    }
  }
}

void DeviceOptionsDock::channel_check() {
  QRadioButton *bt = dynamic_cast<QRadioButton *>(sender());
  if (!bt) {
    pxv_warn("%s", "DeviceOptionsDock::channel_check: bt is NULL");
    return;
  }
  assert(bt);

  /* Task 10/Phase 3: ChannelModePair.value is now a QString (the channel
   * mode string from config_list). Driver config_set expects a string. */
  QString mode_index;

  for (auto p : _channel_mode_indexs) {
    if (p.key == bt) {
      mode_index = p.value;
      break;
    }
  }
  if (mode_index.isEmpty()) {
    pxv_warn("%s", "DeviceOptionsDock::channel_check: mode_index is empty");
    return;
  }
  _device_agent->set_config_string(SR_CONF_CHANNEL_MODE,
                                  mode_index.toUtf8().constData());

  build_dynamic_panel();
  try_resize_scroll();

  QTimer::singleShot(0, this, [this]() {
    _session->broadcast_async<interface::DeviceOptionsUpdated>({});
    _session->broadcast_async<interface::EndDeviceOptions>({});
    emit settings_applied();
  });
}

void DeviceOptionsDock::analog_channel_check() {
  QCheckBox *sc = dynamic_cast<QCheckBox *>(sender());
  if (sc != NULL) {
    for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
      sr_channel *const probe = (sr_channel *)l->data;

      if (sc->property("index").toInt() == probe->index) {
        _device_agent->set_config_bool(SR_CONF_PROBE_MAP_DEFAULT,
                                       sc->isChecked(), probe);
      }
    }
  }

  _lst_probe_enabled_status.clear();
  for (auto ck : _probes_checkBox_list) {
    _lst_probe_enabled_status.push_back(ck->isChecked());
  }

  build_dynamic_panel();
  try_resize_scroll();
}

void DeviceOptionsDock::on_analog_channel_enable() {
  QCheckBox *sc = dynamic_cast<QCheckBox *>(sender());
  channel_checkbox_clicked(sc);
  commit_channels();
}

void DeviceOptionsDock::channel_checkbox_clicked(QCheckBox *sc) {
  if (_device_agent->get_work_mode() == LOGIC ||
      _device_agent->get_work_mode() == MSO) {
    if (sc == NULL || !sc->isChecked())
      return;

    // SR_CONF_STREAM fork key deleted — use DeviceAgent typed wrapper.
    if (!_device_agent->is_stream_mode())
      return;

    int cur_ch_num = 0;
    for (auto box : _probes_checkBox_list) {
      if (box->isChecked())
        cur_ch_num++;
    }

    // SR_CONF_VLD_CH_NUM fork key deleted — derive from sdi->channels.
    // The driver's scan() populates sdi->channels with the maximum available
    // channels for the current channel_mode, which is exactly what the old
    // SR_CONF_VLD_CH_NUM used to report.
    int vld_ch_num = 0;
    for (const GSList *l = _device_agent->get_channels(); l; l = l->next)
      vld_ch_num++;

    if (cur_ch_num > vld_ch_num) {
      QString msg_str(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_MAX_CHANNEL_COUNT_WARNING),
                          "max count of channels!"));
      msg_str = msg_str.replace("{0}", QString::number(vld_ch_num));
      MsgBox::Show(msg_str);

      sc->setChecked(false);
    }
  } else if (_device_agent->get_work_mode() == ANALOG) {
    if (sc != NULL) {
      QGridLayout *const layout =
          (QGridLayout *)sc->property("Layout").value<void *>();
      int i = layout->count();

      int ck_index = -1;
      int i_dex = 0;
      bool map_default = false;

      for (auto ck : _probes_checkBox_list) {
        if (ck == sc) {
          ck_index = i_dex;
          break;
        }
        i_dex++;
      }

      if (ck_index != -1) {
        _device_agent->get_config_bool(SR_CONF_PROBE_MAP_DEFAULT, map_default,
                                       _dso_channel_list[ck_index], NULL);
      }

      while (i--) {
        QWidget *w = layout->itemAt(i)->widget();

        if (w->objectName() == "map-enable") {
          QCheckBox *map_ckbox = dynamic_cast<QCheckBox *>(w);
          map_ckbox->isChecked();
        }

        if (w->property("Enable").isNull()) {

          if (map_default && w->objectName() == "map-row") {
            w->setEnabled(false);
          } else {
            w->setEnabled(sc->isChecked());
          }
        }
      }
    }
  }
}

void DeviceOptionsDock::analog_probes(QGridLayout &layout) {
  using namespace Qt;

  pxv_info("DeviceOptionsDock::analog_probes: ENTER");

  _probes_checkBox_list.clear();
  _probe_options_binding_list.clear();
  _dso_channel_list.clear();

  QTabWidget *tabWidget = new QTabWidget();
  tabWidget->setTabPosition(QTabWidget::North);
  tabWidget->setUsesScrollButtons(false);

  QFont labelFont = dock_font_label();
  QFont contentFont = dock_font_content();

  int ch_dex = 0;

  for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
    sr_channel *const probe = (sr_channel *)l->data;
    if (!probe) {
      pxv_warn("%s", "DeviceOptionsDock: probe is NULL in dso channel loop, skipping");
      continue;
    }
    assert(probe);

    // ANALOG mode: only show ANALOG channels (skip LOGIC/DSO channels that
    // the demo device still exposes in its channel list but are disabled
    // in this mode). Without this filter, ProbeOptions was created for
    // LOGIC channels (D0-D7) whose cg is the "Logic" group — VDIV/COUPLING
    // are not in that group's devopts, so every _getter() returned NULL.
    if (probe->type != SR_CHANNEL_ANALOG) {
      pxv_info("DeviceOptionsDock::analog_probes: skipping non-analog channel "
               "'%s' (type=%d)", probe->name ? probe->name : "(null)", probe->type);
      continue;
    }

    _dso_channel_list.push_back(probe);

    QWidget *probe_widget = new QWidget(tabWidget);
    QGridLayout *probe_layout = new QGridLayout(probe_widget);
    probe_widget->setLayout(probe_layout);

    bool ch_enabled = probe->enabled;
    if (ch_dex < (int)_lst_probe_enabled_status.size()) {
      ch_enabled = _lst_probe_enabled_status[ch_dex];
    }

    ch_dex++;

    QCheckBox *probe_checkBox = new QCheckBox(_container_panel);
    probe_checkBox->setObjectName("dock_content");
    QVariant vlayout = QVariant::fromValue((void *)probe_layout);
    probe_checkBox->setProperty("Layout", vlayout);
    probe_checkBox->setProperty("Enable", true);
    probe_checkBox->setChecked(ch_enabled);
    _probes_checkBox_list.push_back(probe_checkBox);

    QLabel *en_label = new QLabel(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE), "Enable: "), _container_panel);
    en_label->setObjectName("dock_label");
    en_label->setFont(labelFont);
    en_label->setProperty("Enable", true);
    en_label->setProperty("lang_id", S_ID(IDS_DLG_ENABLE));
    probe_layout->addWidget(en_label, 0, 0, 1, 1);
    probe_layout->addWidget(probe_checkBox, 0, 1, 1, 3);

    auto *probe_options_binding = new pv::prop::binding::ProbeOptions(_session, probe);
    const auto &properties = probe_options_binding->properties();
    int i = 1;

    for (auto p : properties) {
      const QString label = p->labeled_widget() ? QString() : p->label();
      QString lb_text = label;
      if (!label.isEmpty()) {
        lb_text = QString::fromUtf8(LangResource::Instance()->get_lang_text(
            STR_PAGE_DSL, label.toUtf8().data(), label.toUtf8().data()));
      }
      QLabel *lb = new QLabel(lb_text, probe_widget);
      lb->setObjectName("dock_label");
      lb->setFont(labelFont);
      lb->setProperty("lang_src", label);
      lb->setProperty("lang_page", STR_PAGE_DSL);
      probe_layout->addWidget(lb, i, 0, 1, 1);

      /* Live widget: dock panel commits immediately — changing VDIV/Coupling/Map
       * must write to the driver right away (unlike DeviceOptions dialog
       * which commits all at once on OK). Without this, coupling dropdown
       * changes were silently lost — the value stayed at DC (default) and
       * the driver never received SR_CONF_PROBE_COUPLING SET. */
      QWidget *pow = p->get_widget_live(probe_widget);
      if (!pow) {
        pxv_warn("DeviceOptionsDock::analog_probes: get_widget returned NULL "
                 "for property '%s' (name='%s'), skipping",
                 label.toUtf8().data(), p->name().toUtf8().data());
        delete lb;
        continue;
      }
      pow->setEnabled(probe_checkBox->isChecked());
      pow->setFont(contentFont);

      /* Notify dock on commit so settings_applied() fires for downstream
       * UI sync (same pattern as device-level properties at line 320). */
      connect(p, &pv::prop::Property::committed, this,
              &DeviceOptionsDock::on_property_committed);

      if (p->name().contains("map default", Qt::CaseInsensitive)) {
        // Bool 属性创建的是 QCheckBox (bool.cpp:51), 不是 QPushButton。
        // 旧代码 qobject_cast<QPushButton*> 返回 NULL → connect 失败 →
        // 点击 "map default" 复选框不触发重建 → map unit/min/max 永远禁用。
        pow->setProperty("index", probe->index);
        QCheckBox *map_ckbox = qobject_cast<QCheckBox *>(pow);
        if (map_ckbox) {
          connect(map_ckbox, &QCheckBox::released, this,
                  &DeviceOptionsDock::analog_channel_check);
        }
      } else {
        if (probe_checkBox->isChecked() && p->name().contains("map", Qt::CaseInsensitive)) {
          bool map_default = true;

          _device_agent->get_config_bool(SR_CONF_PROBE_MAP_DEFAULT, map_default,
                                         probe, NULL);

          if (map_default)
            pow->setEnabled(false);

          pow->setObjectName("map-row");
        }
      }
      probe_layout->addWidget(pow, i, 1, 1, 3);
      i++;
    }
    _probe_options_binding_list.push_back(probe_options_binding);

    connect(probe_checkBox, &QCheckBox::released, this,
            &DeviceOptionsDock::on_analog_channel_enable);

    QString tabName = QString::fromUtf8(probe->name);
    tabName += " ";

    tabWidget->addTab(probe_widget, tabName);
  }

  layout.addWidget(tabWidget, 0, 0, 1, 1);

  ::ui::set_dock_form_font(this);
  _groupHeight2 = tabWidget->sizeHint().height() + 50;

  connect(tabWidget, &QTabWidget::currentChanged, this,
          &DeviceOptionsDock::on_anlog_tab_changed);
  tabWidget->setCurrentIndex(_cur_analog_tag_index);
}

void DeviceOptionsDock::on_anlog_tab_changed(int index) {
  _cur_analog_tag_index = index;
}

QString DeviceOptionsDock::dynamic_widget(QLayout *lay) {
  int mode = _device_agent->get_work_mode();

  if (mode == LOGIC || mode == MSO) {
    QVBoxLayout *grid = dynamic_cast<QVBoxLayout *>(lay);
    if (!grid) {
      pxv_warn("%s", "DeviceOptionsDock::dynamic_widget: grid is NULL (LOGIC/MSO)");
      return QString();
    }
    assert(grid);
    logic_probes(*grid);
    return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL), "Channel");
  } else if (mode == DSO) {
    // DSO calibration UI removed: SR_CONF_HAVE_ZERO fork key was deleted
    // (DSO mode deprecated, DSCope hardware dropped). DSO mode shows no
    // dynamic panel content.
    (void)lay;
  } else if (mode == ANALOG) {
    QGridLayout *grid = dynamic_cast<QGridLayout *>(lay);
    if (!grid) {
      pxv_warn("%s", "DeviceOptionsDock::dynamic_widget: grid is NULL (ANALOG)");
      return QString();
    }
    assert(grid);
    analog_probes(*grid);
    return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL), "Channel");
  }
  return NULL;
}

void DeviceOptionsDock::build_dynamic_panel() {
  _isBuilding = true;

  if (_dynamic_panel != NULL) {
    int idx = 0;
    QLayoutItem *item = nullptr;
    while ((item = _container_lay->itemAt(idx)) != nullptr) {
      if (item->widget() == _dynamic_panel) {
        _container_lay->takeAt(idx);
        delete item;
        break;
      }
      idx++;
    }
    delete _dynamic_panel;
    _dynamic_panel = NULL;
  }

  QFont sectionTitleFont = dock_font_section_title();

  if (_dynamic_panel == NULL) {
    _dynamic_panel = new QWidget(_container_panel);
    int insert_idx = 0;
    if (_sampling_settings_widget) {
      insert_idx = _container_lay->indexOf(_sampling_settings_widget) + 1;
      // Skip separator if it exists
      if (insert_idx < _container_lay->count()) {
        QLayoutItem *item = _container_lay->itemAt(insert_idx);
        if (item && item->widget() &&
            item->widget()->objectName() == "dock_section_separator") {
          insert_idx++;
        }
      }
    }
    _container_lay->insertWidget(insert_idx, _dynamic_panel);

    QLabel *dyn_title = new QLabel("group", _dynamic_panel);
    dyn_title->setObjectName("dock_section_title");
    dyn_title->setFont(sectionTitleFont);

    QLayout *inner;
    if (_device_agent->get_work_mode() == LOGIC ||
        _device_agent->get_work_mode() == MSO)
      inner = new QVBoxLayout();
    else
      inner = new QGridLayout();

    QVBoxLayout *dyn_vbox = new QVBoxLayout(_dynamic_panel);
    dyn_vbox->setContentsMargins(0, 0, 0, 0);
    dyn_vbox->setSpacing(5);
    dyn_vbox->setAlignment(Qt::AlignTop);
    dyn_vbox->addWidget(dyn_title);
    dyn_vbox->addLayout(inner);
  }

  QVBoxLayout *outer_vbox =
      qobject_cast<QVBoxLayout *>(_dynamic_panel->layout());
  QLayout *inner = nullptr;
  if (outer_vbox && outer_vbox->count() > 1) {
    QLayoutItem *item = outer_vbox->itemAt(1);
    if (item)
      inner = item->layout();
  }
  QString title = dynamic_widget(inner);
  QLabel *dyn_title = _dynamic_panel->findChild<QLabel *>("dock_section_title");
  if (dyn_title) {
    dyn_title->setFont(sectionTitleFont);
    dyn_title->setProperty("lang_id", S_ID(IDS_DLG_CHANNEL));
  }
  dyn_title->setText(title);

  update_dynamic_panel_visibility(title != "");

  if (inner)
    inner->setContentsMargins(5, 2, 5, 5);

  _isBuilding = false;
}

void DeviceOptionsDock::update_dynamic_panel_visibility(bool visible) {
  if (!_dynamic_panel)
    return;

  _dynamic_panel->setVisible(visible);

  // Find and update adjacent separators
  int dyn_idx = _container_lay->indexOf(_dynamic_panel);
  if (dyn_idx < 0)
    return;

  for (int offset : {-1, 1}) {
    int sep_idx = dyn_idx + offset;
    if (sep_idx >= 0 && sep_idx < _container_lay->count()) {
      QLayoutItem *item = _container_lay->itemAt(sep_idx);
      if (item && item->widget() &&
          item->widget()->objectName() == "dock_section_separator") {
        item->widget()->setVisible(visible);
      }
    }
  }
}

void DeviceOptionsDock::try_resize_scroll() {
#ifdef _WIN32
  if (_dynamic_panel == NULL)
    return;

  auto labels = _dynamic_panel->findChildren<QLabel *>();
  int max_label_width = 0;

  setUpdatesEnabled(false);
  _container_lay->setEnabled(false);
  for (auto o : labels) {
    QFontMetrics labelFm(o->font());
    QRect rc = labelFm.boundingRect(o->text());
    QSize size(rc.width() + 15, rc.height());
    o->setMinimumSize(size);

    if (size.width() > max_label_width) {
      max_label_width = size.width();
    }
  }

  int wm = _device_agent->get_work_mode();
  if ((wm == LOGIC || wm == MSO) && _device_agent->is_demo()) {
    _dynamic_panel->setFixedWidth(max_label_width + 250);
  }
  _container_lay->setEnabled(true);
  setUpdatesEnabled(true);
#endif
}

void DeviceOptionsDock::update_view() {
  for (auto ptr : _probe_options_binding_list) {
    const auto &props = ptr->properties();
    for (auto p : props) {
      delete p;
    }
    delete ptr;
  }
  _probe_options_binding_list.clear();

  if (_device_options_binding) {
    const auto &old_dev_props = _device_options_binding->properties();
    for (auto p : old_dev_props) {
      delete p;
    }
    delete _device_options_binding;
    _device_options_binding = NULL;
  }

  // Preserve sampling settings widget from being deleted
  if (_sampling_settings_widget) {
    _container_lay->removeWidget(_sampling_settings_widget);
    _sampling_settings_widget->setParent(nullptr);
  }

  if (_device_agent->have_instance()) {
    _device_options_binding = new pv::prop::binding::DeviceOptions(_session);
  }

  QLayoutItem *item;
  while ((item = _container_lay->takeAt(0)) != NULL) {
    if (item->widget()) {
      delete item->widget();
    }
    delete item;
  }

  _dynamic_panel = NULL;
  _probes_checkBox_list.clear();
  _channel_mode_indexs.clear();
  _dso_channel_list.clear();

  if (_device_options_binding == NULL)
    return;

  QFont sectionTitleFont = dock_font_section_title();

  build_dynamic_panel();

  QWidget *minWid = new QWidget();
  minWid->setFixedHeight(1);
  minWid->setMinimumWidth(230);
  _container_lay->addWidget(minWid);

  // 通道和 Mode 之间的分隔线
  QFrame *sep_mode = new QFrame(_container_panel);
  sep_mode->setObjectName("dock_section_separator");
  sep_mode->setFrameShape(QFrame::HLine);
  _container_lay->addWidget(sep_mode);

  // Mode 部分
  QLabel *mode_title = new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MODE), "Mode"),
                                  _container_panel);
  mode_title->setObjectName("dock_section_title");
  mode_title->setFont(sectionTitleFont);
  mode_title->setProperty("lang_id", S_ID(IDS_DLG_MODE));
  QWidget *mode_section = new QWidget(_container_panel);
  mode_section->setObjectName("dock_mode_section");
  QVBoxLayout *mode_vbox = new QVBoxLayout(mode_section);
  mode_vbox->setContentsMargins(0, 0, 0, 0);
  mode_vbox->setSpacing(5);
  mode_vbox->addWidget(mode_title);
  QWidget *mode_inner = new QWidget(mode_section);
  QLayout *props_lay = get_property_form(mode_inner);
  props_lay->setContentsMargins(5, 8, 5, 10);
  mode_vbox->addWidget(mode_inner);
  mode_vbox->setAlignment(Qt::AlignTop);
  _container_lay->addWidget(mode_section);

  if (_sampling_settings_widget) {
    _container_lay->insertWidget(0, _sampling_settings_widget);

    QFrame *sep = new QFrame(_container_panel);
    sep->setObjectName("dock_section_separator");
    sep->setFrameShape(QFrame::HLine);
    _container_lay->insertWidget(1, sep);
  }

  _container_lay->addStretch();

  try_resize_scroll();

  // Ensure separator visibility matches dynamic panel content
  QLabel *dyn_title = _dynamic_panel->findChild<QLabel *>("dock_section_title");
  if (dyn_title)
    update_dynamic_panel_visibility(!dyn_title->text().isEmpty());
}

void DeviceOptionsDock::on_mode_changed() {
  // Lightweight mode-switch refresh: only rebuild the dynamic panel (channel
  // area) and the Mode section (property form), preserving the surrounding
  // scaffolding (sampling widget, separators, minWid, stretch). This avoids
  // the full nuke-and-rebuild of update_view() which causes UI jumping.
  //
  // Cleanup order matters: Property destructors delete their owned widgets
  // (DsComboBox etc.) which are children of the mode_section / dynamic_panel.
  // We must delete bindings BEFORE deleting the container widgets to avoid
  // double-free.

  // 1. Delete probe options bindings (Property destructors delete their widgets
  //    which live inside _dynamic_panel's tabs)
  for (auto ptr : _probe_options_binding_list) {
    const auto &props = ptr->properties();
    for (auto p : props) {
      delete p;
    }
    delete ptr;
  }
  _probe_options_binding_list.clear();

  // 2. Delete device options binding (Property destructors delete their widgets
  //    which live inside the mode_section)
  if (_device_options_binding) {
    const auto &old_dev_props = _device_options_binding->properties();
    for (auto p : old_dev_props) {
      delete p;
    }
    delete _device_options_binding;
    _device_options_binding = NULL;
  }

  // 3. Delete old Mode section container (remaining labels/layout shell)
  for (int i = 0; i < _container_lay->count(); ++i) {
    QLayoutItem *item = _container_lay->itemAt(i);
    if (item && item->widget() &&
        item->widget()->objectName() == "dock_mode_section") {
      _container_lay->takeAt(i);
      delete item->widget();
      break;
    }
  }

  // 4. Create new device options binding (queries SR_CONF_DEVICE_OPTIONS for
  //    the new mode)
  if (_device_agent->have_instance()) {
    _device_options_binding = new pv::prop::binding::DeviceOptions(_session);
  }

  // 5. Rebuild dynamic panel in-place (deletes old _dynamic_panel, creates new)
  build_dynamic_panel();

  // 6. Rebuild Mode section and insert before the stretch
  if (_device_options_binding) {
    QFont sectionTitleFont = dock_font_section_title();
    QLabel *mode_title =
        new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MODE), "Mode"), _container_panel);
    mode_title->setObjectName("dock_section_title");
    mode_title->setFont(sectionTitleFont);
    mode_title->setProperty("lang_id", S_ID(IDS_DLG_MODE));
    QWidget *mode_section = new QWidget(_container_panel);
    mode_section->setObjectName("dock_mode_section");
    QVBoxLayout *mode_vbox = new QVBoxLayout(mode_section);
    mode_vbox->setContentsMargins(0, 0, 0, 0);
    mode_vbox->setSpacing(5);
    mode_vbox->addWidget(mode_title);
    QWidget *mode_inner = new QWidget(mode_section);
    QLayout *props_lay = get_property_form(mode_inner);
    props_lay->setContentsMargins(5, 8, 5, 10);
    mode_vbox->addWidget(mode_inner);
    mode_vbox->setAlignment(Qt::AlignTop);

    // Insert before the stretch (find first spacer item)
    int insert_idx = _container_lay->count();
    for (int i = 0; i < _container_lay->count(); ++i) {
      QLayoutItem *item = _container_lay->itemAt(i);
      if (item && item->spacerItem()) {
        insert_idx = i;
        break;
      }
    }
    _container_lay->insertWidget(insert_idx, mode_section);
  }

  // 7. Refresh cached mode strings (used by mode_check_timeout to detect
  //    operation_mode / pattern_mode changes)
  _device_agent->get_config_string(SR_CONF_OPERATION_MODE, _opt_mode);
  if (_device_agent->is_demo())
    _demo_operation_mode = _device_agent->get_demo_operation_mode();

  try_resize_scroll();
}

void DeviceOptionsDock::update_widgets_status() {
  bool bEnable = !_session->is_working();

  // Update all widgets in the container except the sampling widget (it handles
  // its own state)
  for (int i = 0; i < _container_lay->count(); ++i) {
    QLayoutItem *item = _container_lay->itemAt(i);
    if (item->widget() && item->widget() != _sampling_settings_widget) {
      item->widget()->setEnabled(bEnable);
    }
  }
}

void DeviceOptionsDock::device_updated() {
  if (_device_agent->have_instance() == false) {
    QLayoutItem *item;
    while ((item = _container_lay->takeAt(0)) != NULL) {
      if (item->widget()) {
        delete item->widget();
      }
      delete item;
    }
    _dynamic_panel = NULL;
    _probes_checkBox_list.clear();
    _channel_mode_indexs.clear();
    _dso_channel_list.clear();

    if (_device_options_binding) {
      const auto &old_dev_props = _device_options_binding->properties();
      for (auto p : old_dev_props) {
        delete p;
      }
      delete _device_options_binding;
      _device_options_binding = NULL;
    }
    for (auto ptr : _probe_options_binding_list) {
      const auto &props = ptr->properties();
      for (auto p : props) {
        delete p;
      }
      delete ptr;
    }
    _probe_options_binding_list.clear();
    return;
  }

  if (_device_options_binding == NULL) {
    update_view();
  }
}

void DeviceOptionsDock::UpdateLanguage() { retranslateUi(); }

void DeviceOptionsDock::UpdateTheme() { retranslateUi(); }

void DeviceOptionsDock::retranslateUi() {
  if (_container_panel == NULL || _device_options_binding == NULL)
    return;

  setUpdatesEnabled(false);

  auto items = _container_panel->findChildren<QObject *>();
  for (auto *obj : items) {
    // dlg-page items: retranslate by symbolic ID stored in "lang_id"
    QVariant idVar = obj->property("lang_id");
    if (idVar.isValid() && idVar.canConvert<QString>()) {
      QByteArray id = idVar.toString().toUtf8();
      QString txt = QString::fromUtf8(LangResource::Instance()->get_lang_text(
          STR_PAGE_DLG, id.constData(), id.constData()));
      if (auto *lb = qobject_cast<QLabel *>(obj))
        lb->setText(txt);
      else if (auto *btn = qobject_cast<QAbstractButton *>(obj))
        btn->setText(txt);
      continue;
    }
    // dsl-page items: retranslate by literal source text in "lang_src"
    QVariant srcVar = obj->property("lang_src");
    if (srcVar.isValid() && srcVar.canConvert<QString>()) {
      QByteArray bytes = srcVar.toString().toUtf8();
      QString txt = QString::fromUtf8(LangResource::Instance()->get_lang_text(
          STR_PAGE_DSL, bytes.constData(), bytes.constData()));
      if (auto *lb = qobject_cast<QLabel *>(obj))
        lb->setText(txt);
      else if (auto *btn = qobject_cast<QAbstractButton *>(obj))
        btn->setText(txt);
    }
  }

  setUpdatesEnabled(true);
  update();
}

void DeviceOptionsDock::UpdateFont() {
  if (_container_panel == NULL)
    return;

  QFont sectionTitleFont = dock_font_section_title();
  QFont labelFont = dock_font_label();
  QFont contentFont = dock_font_content();

  setUpdatesEnabled(false);

  auto section_titles =
      _container_panel->findChildren<QLabel *>("dock_section_title");
  for (auto lb : section_titles) {
    lb->setFont(sectionTitleFont);
  }

  auto labels = _container_panel->findChildren<QLabel *>("dock_label");
  for (auto lb : labels) {
    lb->setFont(labelFont);
  }

  auto content_widgets =
      _container_panel->findChildren<QWidget *>("dock_content");
  for (auto w : content_widgets) {
    w->setFont(contentFont);
  }

  auto check_boxes = _container_panel->findChildren<QCheckBox *>();
  for (auto cb : check_boxes) {
    cb->setFont(contentFont);
  }

  auto radio_buttons = _container_panel->findChildren<QRadioButton *>();
  for (auto rb : radio_buttons) {
    rb->setFont(contentFont);
  }

  auto push_buttons = _container_panel->findChildren<QPushButton *>();
  for (auto pb : push_buttons) {
    pb->setFont(contentFont);
  }

  auto comboboxes = _container_panel->findChildren<QComboBox *>();
  for (auto cb : comboboxes) {
    cb->setFont(contentFont);
  }

  setUpdatesEnabled(true);

  try_resize_scroll();
}

void DeviceOptionsDock::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  if (_device_agent->have_instance())
    _mode_check_timer.start();
}

void DeviceOptionsDock::hideEvent(QHideEvent *event) {
  QWidget::hideEvent(event);
  _mode_check_timer.stop();
}

void DeviceOptionsDock::bind_context(TabContext *ctx) {
  _context = ctx;
  if (_device_agent && _device_agent->have_instance()) {
    auto &saved = ctx->view()->dock_ui_state().dock_device_options_session;
    if (!saved.isEmpty()) {
      set_session(saved);
    } else {
      update_view();
    }
  } else {
    update_view();
  }
}

void DeviceOptionsDock::unbind_context() {
  if (_context && _context->view() && _device_agent &&
      _device_agent->have_instance()) {
    _context->view()->dock_ui_state().dock_device_options_session =
        get_session();
  }
  _context = nullptr;
}

QJsonObject DeviceOptionsDock::get_session() {
  QJsonObject obj;

  int mode = _device_agent->get_work_mode();
  obj["work_mode"] = mode;

  /* Task 10/Phase 3: operation_mode/channel_mode now stored as strings in
   * JSON (driver config_get returns strings). */
  obj["operation_mode"] = _opt_mode;

  if (mode == LOGIC) {
    QString ch_mode;
    for (auto &p : _channel_mode_indexs) {
      QRadioButton *bt = static_cast<QRadioButton *>(p.key);
      if (bt->isChecked()) {
        ch_mode = p.value;
        break;
      }
    }
    obj["channel_mode"] = ch_mode;
  }

  QJsonArray ch_array;
  int idx = 0;
  for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
    sr_channel *const probe = (sr_channel *)l->data;
    QJsonObject ch_obj;
    ch_obj["index"] = (int)probe->index;

    if (idx < (int)_probes_checkBox_list.size()) {
      ch_obj["enabled"] = _probes_checkBox_list[idx]->isChecked();
    } else {
      ch_obj["enabled"] = probe->enabled;
    }

    if (mode == ANALOG || mode == DSO) {
      if (idx < (int)_probe_options_binding_list.size()) {
        auto *binding = _probe_options_binding_list[idx];
        const auto &properties = binding->properties();

        for (auto p : properties) {
          if (p->name().contains("Volts/div")) {
            QWidget *w = p->get_widget_deferred(nullptr);
            QComboBox *combo = qobject_cast<QComboBox *>(w);
            if (combo && combo->currentIndex() >= 0) {
              GVariant *gvar =
                  (GVariant *)combo->itemData(combo->currentIndex())
                      .value<void *>();
              if (gvar && g_variant_is_of_type(gvar, G_VARIANT_TYPE("t"))) {
                uint64_t vdiv = g_variant_get_uint64(gvar);
                ch_obj["vdiv"] = (qint64)vdiv;
              }
            }
          } else if (p->name().contains("Coupling")) {
            QWidget *w = p->get_widget_deferred(nullptr);
            QComboBox *combo = qobject_cast<QComboBox *>(w);
            if (combo && combo->currentIndex() >= 0) {
              GVariant *gvar =
                  (GVariant *)combo->itemData(combo->currentIndex())
                      .value<void *>();
              if (gvar && g_variant_is_of_type(gvar, G_VARIANT_TYPE("i"))) {
                int coupling = g_variant_get_int32(gvar);
                ch_obj["coupling"] = coupling;
              }
            }
          } else if (p->name().contains("Map Default")) {
            QWidget *w = p->get_widget_deferred(nullptr);
            QCheckBox *checkBox = qobject_cast<QCheckBox *>(w);
            if (checkBox) {
              ch_obj["map_default"] = checkBox->checkState() == Qt::Checked;
            }
          }
        }
      } else {
        // PROBE_VDIV/PROBE_COUPLING fork DSO keys deleted; only map_default
        // is queried (still in dsvdef.h, migrated in Phase 2).
        bool map_default = true;
        _device_agent->get_config_bool(SR_CONF_PROBE_MAP_DEFAULT, map_default,
                                       probe, NULL);
        ch_obj["map_default"] = map_default;
      }
    }

    ch_array.append(ch_obj);
    idx++;
  }
  obj["channels"] = ch_array;

  if (_device_agent->is_demo()) {
    obj["demo_operation_mode"] = _device_agent->get_demo_operation_mode();
  }

  return obj;
}

void DeviceOptionsDock::set_session(QJsonObject &obj) {
  /* Task 10/Phase 3: operation_mode/channel_mode now read as strings from
   * JSON and written via set_config_string (driver config_set uses std_str_idx). */
  if (obj.contains("operation_mode")) {
    QString op_mode = obj["operation_mode"].toString();
    _device_agent->set_config_string(SR_CONF_OPERATION_MODE,
                                     op_mode.toLocal8Bit().data());
  }

  if (obj.contains("channel_mode")) {
    QString ch_mode = obj["channel_mode"].toString();
    _device_agent->set_config_string(SR_CONF_CHANNEL_MODE,
                                     ch_mode.toLocal8Bit().data());
  }

  if (obj.contains("demo_operation_mode")) {
    QString demo_mode = obj["demo_operation_mode"].toString();
    _device_agent->set_config_string(SR_CONF_PATTERN_MODE,
                                     demo_mode.toLocal8Bit().data());
  }

  update_view();

  int mode = _device_agent->get_work_mode();

  if (obj.contains("channels")) {
    QJsonArray ch_array = obj["channels"].toArray();
    int idx = 0;
    for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
      sr_channel *const probe = (sr_channel *)l->data;
      if (idx < ch_array.size()) {
        QJsonObject ch_obj = ch_array[idx].toObject();
        probe->enabled = ch_obj["enabled"].toBool();
        if (idx < (int)_probes_checkBox_list.size()) {
          _probes_checkBox_list[idx]->setChecked(probe->enabled);
        }

        if (mode == ANALOG || mode == DSO) {
          // PROBE_VDIV/PROBE_COUPLING fork DSO keys deleted; only
          // map_default is restored (still in dsvdef.h, migrated in Phase 2).
          if (ch_obj.contains("map_default")) {
            _device_agent->set_config_bool(SR_CONF_PROBE_MAP_DEFAULT,
                                           ch_obj["map_default"].toBool(),
                                           probe, NULL);
          }
        }
      }
      idx++;
    }
  }

  _device_agent->get_config_string(SR_CONF_OPERATION_MODE, _opt_mode);
  if (_device_agent->is_demo())
    _demo_operation_mode = _device_agent->get_demo_operation_mode();
}

void DeviceOptionsDock::set_sampling_widget(QWidget *widget) {
  _sampling_settings_widget = widget;
}

} // namespace dock
} // namespace pv
