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
#include <QtConcurrent>
#include <assert.h>

#include "../appcontrol.h"
#include "../config/appconfig.h"
#include "../data/sessiondocument.h"
#include "../deviceagent.h"
#include "../dsvdef.h"
#include "../interface/icallbacks.h"
#include "../prop/property.h"
#include "../prop/string.h"
#include "../sigsession.h"
#include "../tabcontext.h"
#include "../ui/dockfonts.h"
#include "../ui/fn.h"
#include "../ui/langresource.h"
#include "../ui/msgbox.h"
#include "../ui/toast.h"

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
    _device_options_binding = new pv::prop::binding::DeviceOptions();

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

    _device_agent->get_config_int16(SR_CONF_OPERATION_MODE, _opt_mode);

    if (_device_agent->is_demo())
      _demo_operation_mode = _device_agent->get_demo_operation_mode();

    try_resize_scroll();
  }

  connect(&_mode_check_timer, &QTimer::timeout, this,
          &DeviceOptionsDock::mode_check_timeout);

  _mode_check_timer.setInterval(500);
}

DeviceOptionsDock::~DeviceOptionsDock() {
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
  if (mode == LOGIC || mode == ANALOG) {
    int index = 0;
    for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
      sr_channel *const probe = (sr_channel *)l->data;
      assert(probe);
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
      _session->broadcast_msg(DSV_MSG_DEVICE_OPTIONS_UPDATED);
      _session->broadcast_msg(DSV_MSG_END_DEVICE_OPTIONS);
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
      lable_text = QString(lang_str);
    }

    QWidget *wid = p->get_widget(parent, true);

    if (p->labeled_widget()) {
      wid->setFont(contentFont);
      wid->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      layout->addWidget(wid, i, 0, 1, 3);
    } else {
      QLabel *lb = new QLabel(lable_text, parent);
      lb->setObjectName("dock_label");
      lb->setFont(labelFont);
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
  int vld_ch_num = 0;
  int cur_ch_num = 0;
  int contentHeight = 0;

  _probes_checkBox_list.clear();

  QFont contentFont = dock_font_content();

  if (_device_agent->get_work_mode() == LOGIC) {
    GVariant *gvar_opts =
        _device_agent->get_config_list(NULL, SR_CONF_CHANNEL_MODE);

    if (gvar_opts != NULL) {
      struct sr_list_item *plist =
          (struct sr_list_item *)g_variant_get_uint64(gvar_opts);
      g_variant_unref(gvar_opts);

      int ch_mode = 0;
      _device_agent->get_config_int16(SR_CONF_CHANNEL_MODE, ch_mode);
      _channel_mode_indexs.clear();

      while (plist != NULL && plist->id >= 0) {
        row1++;
        QString mode_bt_text = LangResource::Instance()->get_lang_text(
            STR_PAGE_DSL, plist->name, plist->name);
        QRadioButton *mode_button = new QRadioButton(mode_bt_text);
        mode_button->setFont(contentFont);
        ChannelModePair mode_index;
        mode_index.key = mode_button;
        mode_index.value = plist->id;
        _channel_mode_indexs.push_back(mode_index);

        layout.addWidget(mode_button);
        contentHeight += mode_button->sizeHint().height();

        connect(mode_button, &QRadioButton::clicked, this,
                &DeviceOptionsDock::channel_check);

        if (plist->id == ch_mode)
          mode_button->setChecked(true);

        plist++;
      }
    }
  }

  _device_agent->get_config_int16(SR_CONF_VLD_CH_NUM, vld_ch_num);

  QWidget *channel_pannel = new QWidget();
  QGridLayout *channel_grid = new QGridLayout();
  channel_grid->setContentsMargins(0, 0, 0, 0);
  channel_grid->setSpacing(3);
  channel_pannel->setLayout(channel_grid);

  int channel_row = 0;
  int channel_column = 0;
  int channel_line_height = 0;
  int channel_columns = 8;
  row2++;

  for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
    sr_channel *const probe = (sr_channel *)l->data;

    if (probe->enabled)
      cur_ch_num++;

    if (cur_ch_num > vld_ch_num)
      probe->enabled = false;

    ChannelLabel *ch_item = new ChannelLabel(this, NULL, probe->index);
    channel_grid->addWidget(ch_item, channel_row, channel_column++,
                            Qt::AlignLeft | Qt::AlignTop);
    _probes_checkBox_list.push_back(ch_item->getCheckBox());
    ch_item->getCheckBox()->setCheckState(probe->enabled ? Qt::Checked
                                                         : Qt::Unchecked);
    channel_line_height = ch_item->height();

    if (channel_column == channel_columns) {
      channel_column = 0;
      channel_row++;

      if (l->next != NULL) {
        row2++;
      }
    }
  }

  for (int c = 0; c < channel_columns; c++) {
    channel_grid->setColumnStretch(c, 0);
  }

  layout.addWidget(channel_pannel);

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

  int vld_ch_num;

  if (_device_agent->get_config_int16(SR_CONF_VLD_CH_NUM, vld_ch_num) == false)
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
  bool stream_mode;

  if (_device_agent->get_config_bool(SR_CONF_STREAM, stream_mode)) {
    if (stream_mode) {
      enable_max_probes();
      commit_channels();
      return;
    }
  }

  set_all_probes(true);
  commit_channels();
}

void DeviceOptionsDock::disable_all_probes() {
  set_all_probes(false);
  commit_channels();
}

void DeviceOptionsDock::zero_adj() {
  commit_channels();

  QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_AUTO_CALIB_START),
                     "Auto Calibration program will be started. Don't connect "
                     "any probes. \nIt can take a while!"));
  bool bRet = MsgBox::Confirm(strMsg);

  if (bRet) {
    _device_agent->set_config_bool(SR_CONF_ZERO, true);
  } else {
    _device_agent->set_config_bool(SR_CONF_ZERO, false);
  }
}

void DeviceOptionsDock::on_calibration() {
  commit_channels();
  _device_agent->set_config_bool(SR_CONF_CALI, true);
}

void DeviceOptionsDock::mode_check_timeout() {
  if (_isBuilding)
    return;

  if (_device_agent->is_hardware()) {
    DeviceAgent *agent = _device_agent;
    int saved_opt_mode = _opt_mode;
    QThreadPool::globalInstance()->start([this, agent, saved_opt_mode]() {
      int mode;
      bool got_mode = agent->get_config_int16(SR_CONF_OPERATION_MODE, mode);
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

    (void)QtConcurrent::run([this, agent]() {
      bool test;
      bool got_test = agent->get_config_bool(SR_CONF_TEST, test);
      if (!got_test || !test)
        return;

      QMetaObject::invokeMethod(this, [this]() {
        setUpdatesEnabled(false);
        for (auto box : _probes_checkBox_list) {
          box->setCheckState(Qt::Checked);
          box->setDisabled(true);
        }
        setUpdatesEnabled(true);
      });
    });
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
  assert(bt);

  int mode_index = -1;

  for (auto p : _channel_mode_indexs) {
    if (p.key == bt) {
      mode_index = p.value;
      break;
    }
  }
  assert(mode_index >= 0);
  _device_agent->set_config_int16(SR_CONF_CHANNEL_MODE, mode_index);

  build_dynamic_panel();
  try_resize_scroll();

  QTimer::singleShot(0, this, [this]() {
    _session->broadcast_msg(DSV_MSG_DEVICE_OPTIONS_UPDATED);
    _session->broadcast_msg(DSV_MSG_END_DEVICE_OPTIONS);
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
  if (_device_agent->get_work_mode() == LOGIC) {
    if (sc == NULL || !sc->isChecked())
      return;

    bool stream_mode;
    if (_device_agent->get_config_bool(SR_CONF_STREAM, stream_mode) == false)
      return;

    if (!stream_mode)
      return;

    int cur_ch_num = 0;
    for (auto box : _probes_checkBox_list) {
      if (box->isChecked())
        cur_ch_num++;
    }

    int vld_ch_num;
    if (_device_agent->get_config_int16(SR_CONF_VLD_CH_NUM, vld_ch_num) ==
        false)
      return;

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
    assert(probe);

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
    probe_layout->addWidget(en_label, 0, 0, 1, 1);
    probe_layout->addWidget(probe_checkBox, 0, 1, 1, 3);

    auto *probe_options_binding = new pv::prop::binding::ProbeOptions(probe);
    const auto &properties = probe_options_binding->properties();
    int i = 1;

    for (auto p : properties) {
      const QString label = p->labeled_widget() ? QString() : p->label();
      QLabel *lb = new QLabel(label, probe_widget);
      lb->setObjectName("dock_label");
      lb->setFont(labelFont);
      probe_layout->addWidget(lb, i, 0, 1, 1);

      QWidget *pow = p->get_widget(probe_widget);
      pow->setEnabled(probe_checkBox->isChecked());
      pow->setFont(contentFont);

      if (p->name().contains("Map Default")) {
        pow->setProperty("index", probe->index);
        connect(qobject_cast<QPushButton *>(pow), &QPushButton::clicked, this,
                &DeviceOptionsDock::analog_channel_check);
      } else {
        if (probe_checkBox->isChecked() && p->name().contains("Map")) {
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

  if (mode == LOGIC) {
    QVBoxLayout *grid = dynamic_cast<QVBoxLayout *>(lay);
    assert(grid);
    logic_probes(*grid);
    return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL), "Channel");
  } else if (mode == DSO) {
    bool have_zero;

    if (_device_agent->get_config_bool(SR_CONF_HAVE_ZERO, have_zero)) {
      QGridLayout *grid = dynamic_cast<QGridLayout *>(lay);
      assert(grid);

      QFont contentFont = dock_font_content();

      if (have_zero) {
        auto config_button =
            new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_AUTO_CALIBRATION),
                                "Auto Calibration"),
                            _container_panel);
        config_button->setObjectName("dock_content");
        config_button->setFont(contentFont);
        grid->addWidget(config_button, 0, 0, 1, 1);
        connect(config_button, &QPushButton::clicked, this,
                &DeviceOptionsDock::zero_adj);

        auto cali_button =
            new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MANUAL_CALIBRATION),
                                "Manual Calibration"),
                            _container_panel);
        cali_button->setObjectName("dock_content");
        cali_button->setFont(contentFont);
        grid->addWidget(cali_button, 1, 0, 1, 1);
        connect(cali_button, &QPushButton::clicked, this,
                &DeviceOptionsDock::on_calibration);

        config_button->setFixedHeight(35);
        cali_button->setFixedHeight(35);

        _groupHeight2 = 135;

        return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CALIBRATION), "Calibration");
      }
    }
  } else if (mode == ANALOG) {
    QGridLayout *grid = dynamic_cast<QGridLayout *>(lay);
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
    if (_device_agent->get_work_mode() == LOGIC)
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
  if (dyn_title)
    dyn_title->setFont(sectionTitleFont);
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
  auto labels = _dynamic_panel->findChildren<QLabel *>();
  int max_label_width = 0;

  setUpdatesEnabled(false);
  _container_lay->setEnabled(false);
  for (auto o : labels) {
    QFontMetrics labelFm(o->font());
    QRect rc = labelFm.boundingRect(o->text());
    QSize size(rc.width() + 15, rc.height());
    o->setFixedSize(size);

    if (size.width() > max_label_width) {
      max_label_width = size.width();
    }
  }

  if (_device_agent->get_work_mode() == LOGIC && _device_agent->is_demo()) {
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
    _device_options_binding = new pv::prop::binding::DeviceOptions();
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

void DeviceOptionsDock::UpdateLanguage() { update_view(); }

void DeviceOptionsDock::UpdateTheme() { update_view(); }

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
    auto &saved = ctx->document()->_dock_device_options_session;
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
  if (_context && _context->document() && _device_agent &&
      _device_agent->have_instance()) {
    _context->document()->_dock_device_options_session = get_session();
  }
  _context = nullptr;
}

QJsonObject DeviceOptionsDock::get_session() {
  QJsonObject obj;

  int mode = _device_agent->get_work_mode();
  obj["work_mode"] = mode;

  obj["operation_mode"] = _opt_mode;

  if (mode == LOGIC) {
    int ch_mode = 0;
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
            QWidget *w = p->get_widget(nullptr);
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
            QWidget *w = p->get_widget(nullptr);
            QComboBox *combo = qobject_cast<QComboBox *>(w);
            if (combo && combo->currentIndex() >= 0) {
              GVariant *gvar =
                  (GVariant *)combo->itemData(combo->currentIndex())
                      .value<void *>();
              if (gvar && g_variant_is_of_type(gvar, G_VARIANT_TYPE("y"))) {
                int coupling = g_variant_get_byte(gvar);
                ch_obj["coupling"] = coupling;
              }
            }
          } else if (p->name().contains("Map Default")) {
            QWidget *w = p->get_widget(nullptr);
            QCheckBox *checkBox = qobject_cast<QCheckBox *>(w);
            if (checkBox) {
              ch_obj["map_default"] = checkBox->checkState() == Qt::Checked;
            }
          }
        }
      } else {
        uint64_t vdiv;
        if (_device_agent->get_config_uint64(SR_CONF_PROBE_VDIV, vdiv, probe,
                                             NULL))
          ch_obj["vdiv"] = (qint64)vdiv;

        int coupling;
        if (_device_agent->get_config_int16(SR_CONF_PROBE_COUPLING, coupling,
                                            probe, NULL))
          ch_obj["coupling"] = coupling;

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
  if (obj.contains("operation_mode")) {
    _device_agent->set_config_int16(SR_CONF_OPERATION_MODE,
                                    obj["operation_mode"].toInt());
  }

  if (obj.contains("channel_mode")) {
    _device_agent->set_config_int16(SR_CONF_CHANNEL_MODE,
                                    obj["channel_mode"].toInt());
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
          if (ch_obj.contains("vdiv")) {
            _device_agent->set_config_uint64(
                SR_CONF_PROBE_VDIV,
                (uint64_t)ch_obj["vdiv"].toVariant().toULongLong(), probe,
                NULL);
          }
          if (ch_obj.contains("coupling")) {
            _device_agent->set_config_int16(SR_CONF_PROBE_COUPLING,
                                            ch_obj["coupling"].toInt(), probe,
                                            NULL);
          }
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

  _device_agent->get_config_int16(SR_CONF_OPERATION_MODE, _opt_mode);
  if (_device_agent->is_demo())
    _demo_operation_mode = _device_agent->get_demo_operation_mode();
}

void DeviceOptionsDock::set_sampling_widget(QWidget *widget) {
  _sampling_settings_widget = widget;
}

} // namespace dock
} // namespace pv
