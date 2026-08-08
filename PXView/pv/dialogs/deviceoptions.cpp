/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include "pv/dialogs/deviceoptions.h"

#include <QDoubleSpinBox>
#include <QGuiApplication>
#include <QLayoutItem>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <cassert>


#include "pv/config/appconfig.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/prop/property.h"
#include "pv/session/sigsession.h"
#include "pv/ui/dockfonts.h"
#include "pv/ui/langresource.h"
#include "pv/ui/msgbox.h"
#include "pv/ui/toast.h"
#include "pv/dialogs/dsmessagebox.h"


using namespace std;

//--------------------------ChannelLabel

const QColor ChannelLabel::PROBE_COLORS[8] = {
    QColor(0x75, 0x50, 0x7B), QColor(0x34, 0x65, 0xA4),
    QColor(0x73, 0xD2, 0x16), QColor(0xED, 0xD4, 0x00),
    QColor(0xF5, 0x79, 0x00), QColor(0xCC, 0x00, 0x00),
    QColor(0x8F, 0x52, 0x02), QColor(0x50, 0x50, 0x50),
};

ChannelLabel::ChannelLabel(IChannelCheck *check, QWidget *parent, int chanIndex,
                           ChannelType type)
    : QWidget(parent) {
  _checked = check;
  _index = chanIndex;
  _type = type;

  _box = new QCheckBox(this);
  _box->hide();

  setFixedSize(23, 20);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  setCursor(Qt::PointingHandCursor);

  connect(_box, &QCheckBox::checkStateChanged, this, [this]() { update(); });
}

void ChannelLabel::paintEvent(QPaintEvent *event) {
  Q_UNUSED(event);
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);

  QColor color;
  if (_type == Analog) {
    static const char *analog_tokens[4] = {
        "@signal-orange", "@signal-green", "@signal-red", "@signal-blue"};
    color = AppConfig::Instance().GetThemeColor(analog_tokens[_index % 4]);
  } else {
    color = AppConfig::Instance().GetThemeColor(
        QString("@logic-channel-%1").arg(_index % 8));
    if (!color.isValid())
      color = PROBE_COLORS[_index % 8];
  }

  bool checked = _box->isChecked();
  bool enabled = _box->isEnabled();

  QRectF r = rect();

  if (!enabled) {
    QColor disabledBg =
        AppConfig::Instance().GetThemeColor("@bg-overlay");
    if (!disabledBg.isValid())
      disabledBg = QColor(240, 240, 240);
    QColor disabledFg = AppConfig::Instance().GetThemeColor("@fg-muted");
    if (!disabledFg.isValid())
      disabledFg = QColor(200, 200, 200);
    p.setBrush(disabledBg);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(r, 4, 4);
    p.setPen(disabledFg);
    QFont font = theme_font_dialog();
    font.setBold(false);
    p.setFont(font);
    p.drawText(rect(), Qt::AlignCenter, QString::number(_index));
    return;
  }

  if (checked) {
    p.setBrush(color);
    p.setPen(Qt::NoPen);
  } else {
    p.setBrush(Qt::transparent);
    p.setPen(QPen(color, 1));
  }

  p.drawRoundedRect(r, 4, 4);

  p.setPen(checked ? Qt::white : color);
  QFont font = theme_font_dialog();
  font.setBold(checked);
  p.setFont(font);
  p.drawText(rect(), Qt::AlignCenter, QString::number(_index));
}

void ChannelLabel::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton && _box->isEnabled()) {
    _box->setChecked(!_box->isChecked());
    if (_checked) {
      _checked->ChannelChecked(_index, _box);
    }
  }
}

void ChannelLabel::on_checked() {
  if (!_checked) {
    return;
  }
  assert(_checked);
  _checked->ChannelChecked(_index, _box);
}

//--------------------------DeviceOptions

namespace pv {
namespace dialogs {

DeviceOptions::DeviceOptions(SigSession *session, QWidget *parent) 
  : PxDialog(parent)
  , _session(session)
  , _device_options_binding(session)
{
  _scroll_panel = nullptr;
  _container_panel = nullptr;
  _scroll = nullptr;
  _width = 0;
  _groupHeight1 = 0;
  _groupHeight2 = 0;
  _dynamic_panel = nullptr;
  _container_lay = nullptr;
  _isBuilding = false;
  _cur_analog_tag_index = 0;

  _device_agent = session->get_device();

  this->setTitle(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DEVICE_OPTIONS), "Device Options"));
  this->SetTitleSpace(0);
  this->layout()->setSpacing(0);
  this->layout()->setDirection(QBoxLayout::TopToBottom);
  this->layout()->setAlignment(Qt::AlignTop);

  // scroll panel
  _scroll_panel = new QWidget();
  QVBoxLayout *scroll_lay = new QVBoxLayout();
  scroll_lay->setContentsMargins(0, 0, 0, 0);
  scroll_lay->setAlignment(Qt::AlignLeft);
  scroll_lay->setDirection(QBoxLayout::TopToBottom);
  _scroll_panel->setLayout(scroll_lay);
  this->layout()->addWidget(_scroll_panel);

  // container
  _container_panel = new QWidget();
  _container_lay = new QVBoxLayout();
  _container_lay->setDirection(QBoxLayout::TopToBottom);
  _container_lay->setAlignment(Qt::AlignTop);
  _container_lay->setContentsMargins(0, 0, 0, 0);
  _container_lay->setSpacing(5);
  _container_panel->setLayout(_container_lay);
  scroll_lay->addWidget(_container_panel);

  QFont font = theme_font_dialog();

  // mode group box
  QGroupBox *props_box =
      new QGroupBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MODE), "Mode"), this);
  props_box->setFont(font);
  props_box->setMinimumHeight(70);
  props_box->setAlignment(Qt::AlignTop);
  QLayout *props_lay = get_property_form(props_box);
  props_lay->setContentsMargins(5, 20, 5, 5);
  props_box->setLayout(props_lay);
  _container_lay->addWidget(props_box);

  QWidget *minWid = new QWidget();
  minWid->setFixedHeight(1);
  minWid->setMinimumWidth(230);
  _container_lay->addWidget(minWid);

  // chnnels group box
  this->build_dynamic_panel();

  // space
  QWidget *space = new QWidget();
  space->setMinimumHeight(5);
  this->layout()->addWidget(space);

  // button
  auto button_box =
      new QDialogButtonBox(QDialogButtonBox::Ok, Qt::Horizontal, this);
  this->layout()->addWidget(button_box);

  _device_agent->get_config_string(SR_CONF_OPERATION_MODE, _opt_mode);

  if (_device_agent->is_demo())
    _demo_operation_mode = _device_agent->get_demo_operation_mode();

  try_resize_scroll();

  connect(&_mode_check_timer, &QTimer::timeout, this,
          &DeviceOptions::mode_check_timeout);
  connect(button_box, &QDialogButtonBox::accepted, this,
          &DeviceOptions::accept);

  _mode_check_timer.setInterval(100);
  _mode_check_timer.start();
}

DeviceOptions::~DeviceOptions() {}

void DeviceOptions::ChannelChecked(int index, QObject *object) {
  (void)index;

  QCheckBox *sc = dynamic_cast<QCheckBox *>(object);
  channel_checkbox_clicked(sc);
}

void DeviceOptions::accept() {
  using namespace Qt;
  bool hasEnabled = false;

  // Commit the properties
  const auto &dev_props = _device_options_binding.properties();

  for (auto p : dev_props) {
    p->commit();
  }

  // Commit the probes
  int mode = _device_agent->get_work_mode();
  if (mode == LOGIC || mode == ANALOG) {
    int index = 0;
    for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
      sr_channel *const probe = (sr_channel *)l->data;
      if (!probe) continue;
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

    QDialog::accept();
  } else {
    QString strMsg(
        L_S(STR_PAGE_MSG, S_ID(IDS_MSG_ALL_CHANNEL_DISABLE),
            "All channel disabled! Please enable at least one channel."));
    pv::ui::Toast::show(this, strMsg, pv::ui::Toast::Warning);
  }
}

void DeviceOptions::reject() {
  using namespace Qt;

  QDialog::reject();
}

QLayout *DeviceOptions::get_property_form(QWidget *parent) {
  QGridLayout *const layout = new QGridLayout(parent);
  layout->setVerticalSpacing(2);
  const auto &properties = _device_options_binding.properties();

  QFont font = theme_font_dialog();

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

    QLabel *lb = new QLabel(lable_text, parent);
    lb->setFont(font);
    layout->addWidget(lb, i, 0);

    if (label == QString("Operation mode")) {
      QWidget *wid = p->get_widget_live(parent);
      wid->setFont(font);
      layout->addWidget(wid, i, 1);
    } else {
      QWidget *wid = p->get_widget_deferred(parent);
      wid->setFont(font);
      layout->addWidget(wid, i, 1);
    }
    layout->setRowMinimumHeight(i, 22);
    i++;
  }

  _groupHeight1 = parent->sizeHint().height() + 30;

  parent->setFixedHeight(_groupHeight1);

  return layout;
}

void DeviceOptions::logic_probes(QVBoxLayout &layout) {
  using namespace Qt;

  layout.setSpacing(2);

  int row1 = 0;
  int row2 = 0;
  // vld_ch_num: "当前 channel_mode 下可启用的最大通道数".
  //  - PXLogic used to expose this via the fork key SR_CONF_VLD_CH_NUM
  //    (deleted). Different channel_modes mapped to different hardware
  //    resource caps (32ch@250MHz / 16ch@500MHz / 8ch@1GHz).
  //  - Now derived from sdi->channels directly — the driver's scan()
  //    populates this with the maximum available channels for the device.
  //    For PXLogic this equals the profile's max channel count; for upstream
  //    drivers (fx2lafw/demo) it is the full channel list.
  int vld_ch_num = 0;
  for (const GSList *l = _device_agent->get_channels(); l; l = l->next)
    vld_ch_num++;
  int cur_ch_num = 0;
  int contentHeight = 0;

  _probes_checkBox_list.clear();

  QFont font = theme_font_dialog();

  // channel count checked
  if (_device_agent->get_work_mode() == LOGIC) {
    GVariant *gvar_opts =
        _device_agent->get_config_list(nullptr, SR_CONF_CHANNEL_MODE);

    if (gvar_opts != nullptr) {
      /* Task 10.6/Phase 3: config_list now returns a GVariant string array
       * (g_variant_new_strv). Read via g_variant_get_strv instead of the
       * fork-style uint64 bare-pointer cast. Driver config_get also returns
       * the current channel mode as a string, so highlight by string match.
       * Note: g_variant_get_strv returns pointers into the GVariant's
       * internal buffer — unref the variant AFTER we're done using strs[]. */
      gsize n_items;
      const gchar **strs = g_variant_get_strv(gvar_opts, &n_items);

      QString cur_ch_mode;
      _device_agent->get_config_string(SR_CONF_CHANNEL_MODE, cur_ch_mode);
      _channel_mode_indexs.clear();

      for (gsize i = 0; i < n_items; i++) {
        row1++;
        QString mode_bt_text = QString::fromUtf8(
            LangResource::Instance()->get_lang_text(
                STR_PAGE_DSL, strs[i], strs[i]));
        QRadioButton *mode_button = new QRadioButton(mode_bt_text);
        mode_button->setFont(font);
        ChannelModePair mode_index;
        mode_index.key = mode_button;
        mode_index.value = QString::fromUtf8(strs[i]);
        _channel_mode_indexs.push_back(mode_index);

        layout.addWidget(mode_button);
        contentHeight += mode_button->sizeHint().height();

        connect(mode_button, &QRadioButton::pressed, this,
                &DeviceOptions::channel_check);

        if (cur_ch_mode == QString::fromUtf8(strs[i]))
          mode_button->setChecked(true);
      }

      g_variant_unref(gvar_opts);
      g_free((gpointer)strs);
    }
  }

  // SR_CONF_VLD_CH_NUM fork key deleted — vld_ch_num already derived above
  // from sdi->channels. No re-fetch needed.
  (void)vld_ch_num;

  // channels
  int total_channels = 0;
  for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
    total_channels++;
  }

  int cols = qMin(total_channels, 8);
  if (cols <= 0)
    cols = 8;

  QWidget *channel_pannel = new QWidget();
  QGridLayout *channel_grid = new QGridLayout();
  channel_grid->setContentsMargins(0, 0, 0, 0);
  channel_grid->setSpacing(0);
  channel_pannel->setLayout(channel_grid);

  int channel_row = 0;
  int channel_column = 0;
  int channel_line_height = 0;
  row2++;

  for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
    sr_channel *const probe = (sr_channel *)l->data;

    if (probe->enabled)
      cur_ch_num++;

    if (cur_ch_num > vld_ch_num)
      probe->enabled = false;

    ChannelLabel *ch_item = new ChannelLabel(this, nullptr, probe->index);
    channel_grid->addWidget(ch_item, channel_row, channel_column++);
    _probes_checkBox_list.push_back(ch_item->getCheckBox());
    ch_item->getCheckBox()->setCheckState(probe->enabled ? Qt::Checked
                                                         : Qt::Unchecked);
    channel_line_height = ch_item->height();

    if (channel_column == cols) {
      channel_column = 0;
      channel_row++;

      if (l->next != nullptr) {
        row2++;
      }
    }
  }

  for (int c = 0; c < cols; c++) {
    channel_grid->setColumnStretch(c, 1);
  }

  layout.addWidget(channel_pannel);

  // space
  QWidget *space = new QWidget();
  space->setFixedHeight(10);
  layout.addWidget(space);
  contentHeight += 10;

  // buttons
  QHBoxLayout *line_lay = new QHBoxLayout();
  layout.addLayout(line_lay);
  line_lay->setSpacing(10);

  QPushButton *enable_all_probes = new QPushButton(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE_ALL), "Enable All"));
  QPushButton *disable_all_probes = new QPushButton(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISABLE_ALL), "Disable All"));
  enable_all_probes->setMaximumHeight(33);
  disable_all_probes->setMaximumHeight(33);
  enable_all_probes->setFont(font);
  disable_all_probes->setFont(font);

  int bt_width = enable_all_probes->fontMetrics().horizontalAdvance(
                     enable_all_probes->text()) +
                 20;

  enable_all_probes->setMaximumWidth(bt_width);
  disable_all_probes->setMaximumWidth(bt_width);

  this->update_font();

  contentHeight += enable_all_probes->sizeHint().height();
  contentHeight += channel_line_height * row2 + 50;

  connect(enable_all_probes, &QPushButton::clicked, this,
          &DeviceOptions::enable_all_probes);
  connect(disable_all_probes, &QPushButton::clicked, this,
          &DeviceOptions::disable_all_probes);

  line_lay->addWidget(enable_all_probes);
  line_lay->addWidget(disable_all_probes);

  _groupHeight2 = contentHeight + (row1 + row2) * 2 + 38;

#ifdef Q_OS_DARWIN
  _groupHeight2 += 5;
#endif

  _dynamic_panel->setFixedHeight(_groupHeight2);
}

void DeviceOptions::set_all_probes(bool set) {
  for (auto box : _probes_checkBox_list) {
    box->setCheckState(set ? Qt::Checked : Qt::Unchecked);
  }
}

void DeviceOptions::enable_max_probes() {
  int cur_ch_num = 0;
  for (auto box : _probes_checkBox_list) {
    if (box->isChecked())
      cur_ch_num++;
  }

  // SR_CONF_VLD_CH_NUM fork key deleted — derive from sdi->channels.
  int vld_ch_num = 0;
  for (const GSList *l = _device_agent->get_channels(); l; l = l->next)
    vld_ch_num++;

  while (cur_ch_num < vld_ch_num &&
         cur_ch_num < (int)_probes_checkBox_list.size()) {
    auto box = _probes_checkBox_list[cur_ch_num];
    if (box->isChecked() == false) {
      box->setChecked(true);
      cur_ch_num++;
    }
  }
}

void DeviceOptions::enable_all_probes() {
  // SR_CONF_STREAM fork key deleted — use DeviceAgent typed wrapper.
  if (_device_agent->is_stream_mode()) {
    enable_max_probes();
    return;
  }

  set_all_probes(true);
}

void DeviceOptions::disable_all_probes() { set_all_probes(false); }

void DeviceOptions::zero_adj() {
  // DSO zero calibration removed: SR_CONF_ZERO fork key was deleted
  // (DSO mode deprecated, DSCope hardware dropped). No-op stub kept so the
  // signal-slot connection from the calibration button compiles.
  using namespace Qt;
  QDialog::accept();
}

void DeviceOptions::on_calibration() {
  // DSO manual calibration removed: SR_CONF_CALI fork key was deleted.
  // No-op stub kept so the signal-slot connection compiles.
  using namespace Qt;
  QDialog::accept();
}

void DeviceOptions::mode_check_timeout() {
  if (_isBuilding)
    return;

  if (_device_agent->is_hardware()) {
    QString mode;

    if (_device_agent->get_config_string(SR_CONF_OPERATION_MODE, mode)) {
      if (mode != _opt_mode) {
        _opt_mode = mode;
        build_dynamic_panel();
        try_resize_scroll();
      }
    }

    // SR_CONF_TEST fork key deleted from pxlogic.c — the test-mode auto-check
    // block below would never execute. Test mode is now a hardware-specific
    // concept handled in the driver's scan() if needed.
  } else if (_device_agent->is_demo()) {
    QString opt_mode = _device_agent->get_demo_operation_mode();
    if (opt_mode != _demo_operation_mode) {
      _demo_operation_mode = opt_mode;
      build_dynamic_panel();
      try_resize_scroll();
    }
  }
}

void DeviceOptions::channel_check() {
  QRadioButton *bt = dynamic_cast<QRadioButton *>(sender());
  if (!bt) return;
  assert(bt);

  /* Task 10/Phase 3: ChannelModePair.value is now a QString. Driver
   * config_set expects a string. */
  QString mode_index;

  for (auto p : _channel_mode_indexs) {
    if (p.key == bt) {
      mode_index = p.value;
      break;
    }
  }
  if (mode_index.isEmpty())
    return;
  _device_agent->set_config_string(SR_CONF_CHANNEL_MODE,
                                  mode_index.toUtf8().constData());

  build_dynamic_panel();
  try_resize_scroll();
}

void DeviceOptions::analog_channel_check() {
  QCheckBox *sc = dynamic_cast<QCheckBox *>(sender());
  if (sc != nullptr) {
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

void DeviceOptions::on_analog_channel_enable() {
  QCheckBox *sc = dynamic_cast<QCheckBox *>(sender());
  channel_checkbox_clicked(sc);
}

void DeviceOptions::channel_checkbox_clicked(QCheckBox *sc) {
  if (_device_agent->get_work_mode() == LOGIC) {
    if (sc == nullptr || !sc->isChecked())
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
    if (sc != nullptr) {
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
                                       _dso_channel_list[ck_index], nullptr);
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

void DeviceOptions::analog_probes(QGridLayout &layout) {
  using namespace Qt;

  _probes_checkBox_list.clear();
  _probe_options_binding_list.clear();
  _dso_channel_list.clear();

  QTabWidget *tabWidget = new QTabWidget();
  tabWidget->setTabPosition(QTabWidget::North);
  tabWidget->setUsesScrollButtons(false);

  QFont font = theme_font_dialog();

  int ch_dex = 0;

  for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
    sr_channel *const probe = (sr_channel *)l->data;
    if (!probe) continue;
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

    QCheckBox *probe_checkBox = new QCheckBox(this);
    QVariant vlayout = QVariant::fromValue((void *)probe_layout);
    probe_checkBox->setProperty("Layout", vlayout);
    probe_checkBox->setProperty("Enable", true);
    probe_checkBox->setChecked(ch_enabled);
    // probe_checkBox->setCheckState(probe->enabled ? Qt::Checked :
    // Qt::Unchecked);
    _probes_checkBox_list.push_back(probe_checkBox);

    QLabel *en_label =
        new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE), "Enable: "), this);
    en_label->setFont(font);
    en_label->setProperty("Enable", true);
    probe_layout->addWidget(en_label, 0, 0, 1, 1);
    probe_layout->addWidget(probe_checkBox, 0, 1, 1, 3);

    auto *probe_options_binding = new pv::prop::binding::ProbeOptions(_session, probe);
    const auto &properties = probe_options_binding->properties();
    int i = 1;

    for (auto p : properties) {
      const QString label = p->labeled_widget() ? QString() : p->label();
      QLabel *lb = new QLabel(label, probe_widget);
      lb->setFont(font);
      probe_layout->addWidget(lb, i, 0, 1, 1);

      QWidget *pow = p->get_widget_deferred(probe_widget);
      pow->setEnabled(probe_checkBox->isChecked());
      pow->setFont(font);

      // sr_config_info->name 是全小写 ("probe_map_default" 等), 必须用
      // CaseInsensitive 匹配; 旧代码 "Map Default"/"Map" 匹配不到 →
      // map default 复选框的 connect 失效 + map 字段不被标记为 map-row。
      if (p->name().contains("map default", Qt::CaseInsensitive)) {
        // Bool 属性创建的是 QCheckBox (bool.cpp:51), 不是 QPushButton。
        // 旧代码 qobject_cast<QPushButton*> 返回 nullptr → connect 失效。
        pow->setProperty("index", probe->index);
        QCheckBox *map_ckbox = qobject_cast<QCheckBox *>(pow);
        if (map_ckbox) {
          connect(map_ckbox, &QCheckBox::released, this,
                  &DeviceOptions::analog_channel_check);
        }
      } else {
        if (probe_checkBox->isChecked() && p->name().contains("map", Qt::CaseInsensitive)) {
          bool map_default = true;

          _device_agent->get_config_bool(SR_CONF_PROBE_MAP_DEFAULT, map_default,
                                         probe, nullptr);

          if (map_default)
            pow->setEnabled(false);

          pow->setObjectName("map-row");
        }
      }
      probe_layout->addWidget(pow, i, 1, 1, 3);
      i++;
    }
    _probe_options_binding_list.push_back(probe_options_binding);

    // Diagnostic: verify driver returns correct coupling/vdiv defaults for
    // this ANALOG channel. If GET returns 0 (GND) for coupling but the driver
    // is demo (whose scan-time init sets analog_coupling[i]=1=DC), proactively
    // SET DC to match the documented default. Same for vdiv (default 1000).
    // This ensures the ProbeOptions enum widget shows the correct initial
    // state instead of falling back to GND when the binding's config_getter
    // returns a stale/zero value.
    {
      int coupling_val = -1;
      /* demo 驱动 GET 返回 int32 ("i"), 匹配 sr_key_info_config SR_T_INT32。
       * 用 get_config_int32 读取。 */
      if (_device_agent->get_config_int32(SR_CONF_PROBE_COUPLING,
                                          coupling_val, probe, nullptr)) {
        pxv_info("analog_probes: probe=%s coupling=%d (expected 1=DC)",
                 probe->name, coupling_val);
        if (coupling_val == 0 && _device_agent->is_demo()) {
          pxv_info("  -> syncing DC default (was GND=0)");
          _device_agent->set_config_int32(SR_CONF_PROBE_COUPLING, 1,
                                           probe, nullptr);
        }
      } else {
        pxv_warn("analog_probes: probe=%s GET SR_CONF_PROBE_COUPLING failed",
                 probe->name);
      }
      uint64_t vdiv_val = 0;
      if (_device_agent->get_config_uint64(SR_CONF_PROBE_VDIV,
                                           vdiv_val, probe, nullptr)) {
        pxv_info("analog_probes: probe=%s vdiv=%llu (expected 1000)",
                 probe->name, (unsigned long long)vdiv_val);
        if (vdiv_val == 0 && _device_agent->is_demo()) {
          pxv_info("  -> syncing default vdiv=1000 (was 0)");
          _device_agent->set_config_uint64(SR_CONF_PROBE_VDIV, 1000,
                                           probe, nullptr);
        }
      } else {
        pxv_warn("analog_probes: probe=%s GET SR_CONF_PROBE_VDIV failed",
                 probe->name);
      }
    }

    connect(probe_checkBox, &QCheckBox::released, this,
            &DeviceOptions::on_analog_channel_enable);

    QString tabName = QString::fromUtf8(probe->name);
    tabName += " ";

    tabWidget->addTab(probe_widget, tabName);
  }

  layout.addWidget(tabWidget, 0, 0, 1, 1);

  this->update_font();
  _groupHeight2 = tabWidget->sizeHint().height() + 50;
  _dynamic_panel->setFixedHeight(_groupHeight2);

  connect(tabWidget, &QTabWidget::currentChanged, this,
          &DeviceOptions::on_anlog_tab_changed);
  tabWidget->setCurrentIndex(_cur_analog_tag_index);
}

void DeviceOptions::on_anlog_tab_changed(int index) {
  _cur_analog_tag_index = index;
}

QString DeviceOptions::dynamic_widget(QLayout *lay) {
  int mode = _device_agent->get_work_mode();

  if (mode == LOGIC) {
    QVBoxLayout *grid = dynamic_cast<QVBoxLayout *>(lay);
    if (!grid) return QString();
    assert(grid);
    logic_probes(*grid);
    // tr
    return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL), "Channel");
  } else if (mode == DSO) {
    // DSO calibration UI removed: SR_CONF_HAVE_ZERO fork key was deleted
    // (DSO mode deprecated, DSCope hardware dropped). DSO mode shows no
    // dynamic panel content.
    (void)lay;
  } else if (mode == ANALOG) {
    QGridLayout *grid = dynamic_cast<QGridLayout *>(lay);
    if (!grid) return QString();
    assert(grid);
    analog_probes(*grid);
    // tr
    return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL), "Channel");
  }
  return nullptr;
}

void DeviceOptions::build_dynamic_panel() {
  _isBuilding = true;

  if (_dynamic_panel != nullptr) {
    _dynamic_panel->deleteLater();
    _dynamic_panel = nullptr;
  }

  QFont font = theme_font_dialog();

  if (_dynamic_panel == nullptr) {
    _dynamic_panel = new QGroupBox("group", _dynamic_panel);
    _dynamic_panel->setFont(font);
    _container_lay->addWidget(_dynamic_panel);

    if (_device_agent->get_work_mode() == LOGIC)
      _dynamic_panel->setLayout(new QVBoxLayout());
    else
      _dynamic_panel->setLayout(new QGridLayout());
  }

  QString title = dynamic_widget(_dynamic_panel->layout());
  QGroupBox *box = dynamic_cast<QGroupBox *>(_dynamic_panel);
  box->setFont(font);
  box->setTitle(title);

  if (title == "") {
    box->setVisible(false);
  }

  _dynamic_panel->layout()->setContentsMargins(5, 20, 5, 5);

  _isBuilding = false;
}

void DeviceOptions::try_resize_scroll() {
  this->update_font();

  // content area height
  int contentHeight = _groupHeight1 + _groupHeight2 + 20; // +space
  // dialog height
  int dlgHeight = contentHeight + 100; // +bottom buttton

#ifdef Q_OS_DARWIN
  dlgHeight += 20;
#endif

  float sk = QGuiApplication::primaryScreen()->logicalDotsPerInch() / 96;

  int srcHeight = 600;
  int w = _width;

#ifdef _WIN32
  QFont font = theme_font_dialog();
  QFontMetrics fm(font);

  auto labels = this->findChildren<QLabel *>();
  int max_label_width = 0;
  for (auto o : labels) {
    QRect rc = fm.boundingRect(o->text());
    QSize size(rc.width() + 15, rc.height());
    o->setFixedSize(size);

    if (size.width() > max_label_width) {
      max_label_width = size.width();
    }
  }

  if (_device_agent->get_work_mode() == LOGIC && _device_agent->is_demo()) {
    _dynamic_panel->setFixedWidth(max_label_width + 250);
  }
#endif

  if (w == 0) {
    w = this->sizeHint().width();
    _width = w;
  }

  QScrollArea *scroll = _scroll;
  if (scroll == nullptr) {
    scroll = new QScrollArea(_scroll_panel);
    scroll->setWidget(_container_panel);
    scroll->setStyleSheet("QScrollArea{border:none;}");
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll = scroll;
  }

  _container_panel->setFixedHeight(contentHeight);
  int sclw = w - 23;

#ifdef Q_OS_DARWIN
  sclw -= 20;
#endif

  if (sk * dlgHeight > srcHeight) {
    int exth = 120;
    this->setFixedSize(w + 12, srcHeight);
    _scroll_panel->setFixedSize(w, srcHeight - exth);
    _scroll->setFixedSize(sclw, srcHeight - exth);
  } else {
    this->setFixedSize(w + 12, dlgHeight);
    _scroll_panel->setFixedSize(w, contentHeight);
    _scroll->setFixedSize(sclw, contentHeight);
  }
}

void DeviceOptions::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape) {
    event->ignore();
    return;
  }

  QDialog::keyPressEvent(event);
}

} // namespace dialogs
} // namespace pv
