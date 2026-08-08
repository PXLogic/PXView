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

#include "pv/dock/triggerdock.h"
#include "pv/dialogs/dsmessagebox.h"
#include "pv/session/sigsession.h"
#include "pv/view/view.h"


#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QObject>
#include <QPainter>
#include <QRegularExpressionValidator>
#include <QSplitter>
#include <QVBoxLayout>
#include <libsigrok/libsigrok.h>
#include <cmath>


#include <QRegularExpression>
#include <QRegularExpressionValidator>

#include "pv/mainwindow/appcontrol.h"
#include "pv/config/appconfig.h"
#include "pv/data/decode/annotationrestable.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/data/triggerconfig.h"
#include "pv/session/deviceagent.h"
#include "pv/base/log.h"
#include "pv/session/tabcontext.h"
#include "pv/ui/dockfonts.h"
#include "pv/ui/fn.h"
#include "pv/ui/langresource.h"
#include "pv/ui/msgbox.h"
#include "pv/view/signal/logicsignal.h"


// Split a combined "ext32 lower" trigger value (32 space-separated tokens) into
// its ext32 (upper 16) and lower (lower 16) parts. For 16-channel values the
// ext32 part is empty and lower_out receives the input unchanged.
static void split_trigger_value(const QString &combined, QString &ext32_out,
                                QString &lower_out) {
  const QStringList parts = combined.split(' ', Qt::SkipEmptyParts);
  if (parts.size() == 32) {
    ext32_out = parts.mid(0, 16).join(' ');
    lower_out = parts.mid(16, 16).join(' ');
  } else {
    ext32_out.clear();
    lower_out = combined;
  }
}

namespace pv {
namespace dock {

const int TriggerDock::MinTrigPosition = 1;

TriggerDock::TriggerDock(QWidget *parent, SigSession *session)
    : pv::widgets::SmoothScrollArea(parent), _session(session), _signals(session), _capture(session),
      _context(nullptr) {
  _cur_ch_num = 16;
  if (_signals->device()->have_instance()) {
    _signals->device()->get_config_int16(SR_CONF_TOTAL_CH_NUM, _cur_ch_num);
  }

  _serial_hex_label = nullptr;
  _serial_hex_lineEdit = nullptr;
  _serial_hex_ck_label = nullptr;
  _is_serial_val_setting = false;

  _widget = new QWidget(this);
  _simple_radioButton = new QRadioButton(_widget);
  _simple_radioButton->setChecked(true);
  _adv_radioButton = new QRadioButton(_widget);

  _position_label = new QLabel(_widget);
  _position_label->setObjectName("dock_label");
  _position_spinBox = new PopupLineEdit(_widget);
  _position_spinBox->setRange(MinTrigPosition, DS_MAX_TRIG_PERCENT);
  _position_spinBox->setValue(1);
  // _position_spinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
  _position_slider = new QSlider(Qt::Horizontal, _widget);
  _position_slider->setRange(MinTrigPosition, DS_MAX_TRIG_PERCENT);
  connect(_position_slider, &QSlider::valueChanged, _position_spinBox,
          &KeyLineEdit::setValue);
  connect(_position_spinBox, &KeyLineEdit::valueChanged, _position_slider,
          &QSlider::setValue);

  _stages_label = new QLabel(_widget);
  _stages_label->setObjectName("dock_label");
  _stages_label->setDisabled(true);
  stages_comboBox = new DsComboBox(_widget);

  for (int i = 1; i <= TriggerStages; i++) {
    stages_comboBox->addItem(QString::number(i));
  }

  stages_comboBox->setDisabled(true);

  _adv_tabWidget = new QTabWidget(_widget);
  _adv_tabWidget->setTabPosition(QTabWidget::North);
  _adv_tabWidget->setDisabled(true);
  setup_adv_tab();

  connect(_simple_radioButton, &QRadioButton::clicked, this,
          &TriggerDock::simple_trigger);
  connect(_adv_radioButton, &QRadioButton::clicked, this,
          &TriggerDock::adv_trigger);
  connect(stages_comboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &TriggerDock::widget_enable);

  QVBoxLayout *layout = new QVBoxLayout(_widget);
  layout->setContentsMargins(12, 8, 12, 8);
  QGridLayout *gLayout = new QGridLayout();
  gLayout->setVerticalSpacing(5);
  gLayout->addWidget(_simple_radioButton, 0, 0);
  gLayout->addWidget(_adv_radioButton, 1, 0);
  gLayout->addWidget(_position_label, 2, 0);
  gLayout->addWidget(_position_spinBox, 2, 1);
  // tr
  gLayout->addWidget(new QLabel("%", _widget), 2, 2);
  gLayout->addWidget(_position_slider, 3, 0, 1, 3);
  gLayout->addWidget(_stages_label, 4, 0);
  gLayout->addWidget(stages_comboBox, 4, 1);
  gLayout->addWidget(new QLabel(_widget), 4, 2);
  gLayout->setColumnStretch(2, 1);

  layout->addLayout(gLayout);
  layout->addWidget(_adv_tabWidget);
  layout->addStretch(1);
  _widget->setLayout(layout);

  this->setFrameShape(QFrame::NoFrame);
  this->setObjectName("dock_trigger_scroll");
  this->setWidget(_widget);
  _widget->setObjectName("triggerWidget");

  ADD_UI(this);
}

TriggerDock::~TriggerDock() { REMOVE_UI(this); }

void TriggerDock::retranslateUi() {
  _simple_radioButton->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SIMPLE_TRIGGER), "Simple Trigger"));
  _adv_radioButton->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ADVANCED_TRIGGER), "Advanced Trigger"));
  _position_label->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_POSITION), "Trigger Position: "));
  _stages_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TOTAL_TRIGGER_STAGES),
                             "Total Trigger Stages: "));
  _serial_start_label->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_START_FLAG), "Start Flag: "));
  _serial_stop_label->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STOP_FLAG), "Stop Flag: "));
  _serial_edge_label->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CLOCK_FLAG), "Clock Flag: "));
  _serial_data_label->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DATA_CHANNEL), "Data Channel: "));
  _serial_value_label->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DATA_VALUE), "Data Value: "));
  {
    QLabel *serial_title =
        _serial_groupBox->findChild<QLabel *>("dock_section_title");
    if (serial_title)
      serial_title->setText(
          L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SERIAL_TRIGGER), "Serial Trigger"));
  }
  _serial_hex_label->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SERIAL_HEX), "Hex: "));
  _serial_hex_ck_label->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SERIAL_INPUT_AS_HEX), "Input hex"));

  _adv_tabWidget->setTabText(
      0, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STAGE_TRIGGER), "Stage Trigger"));
  _adv_tabWidget->setTabText(
      1, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SERIAL_TRIGGER), "Serial Trigger"));
  _serial_note_label->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SERIAL_NOTE_LABEL),
          "X: Don't care\n0: Low level\n1: High level\nR: Rising edge\nF: "
          "Falling edge\nC: Rising/Falling edge"));
  _data_bits_label->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DATA_BITS), "Data Bits"));

  for (int i = 0; i < _inv_exp_label_list.length(); i++) {
    _inv_exp_label_list.at(i)->setText(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_INV), "Inv"));
  }

  for (int i = 0; i < _count_exp_label_list.length(); i++) {
    _count_exp_label_list.at(i)->setText(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COUNTER), "Counter"));
  }

  for (int i = 0; i < _contiguous_label_list.length(); i++) {
    _contiguous_label_list.at(i)->setText(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CONTIGUOUS), "Contiguous"));
  }

  for (int i = 0; i < _stage_groupBox_list.length(); i++) {
    QLabel *stage_title =
        _stage_groupBox_list.at(i)->findChild<QLabel *>("dock_section_title");
    if (stage_title)
      stage_title->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STAGE), "Stage") +
                           QString::number(i));
  }

  for (int i = 0; i < _stage_note_label_list.length(); i++) {
    _stage_note_label_list.at(i)->setText(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SERIAL_NOTE_LABEL),
            "X: Don't care\n0: Low level\n1: High level\nR: Rising edge\nF: "
            "Falling edge\nC: Rising/Falling edge"));
  }
}

void TriggerDock::reStyle() {}

void TriggerDock::simple_trigger() {
  _stages_label->setDisabled(true);
  stages_comboBox->setDisabled(true);
  _adv_tabWidget->setDisabled(true);
}

void TriggerDock::adv_trigger() {
  if (_signals->device()->is_hardware_logic()) {
    // SR_CONF_STREAM fork key deleted — use DeviceAgent typed wrapper.
    bool stream = _signals->device()->is_stream_mode();

    if (stream) {
      QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STREAM_NO_AD_TRIGGER),
                         "Stream Mode Don't Support Advanced Trigger!"));
      MsgBox::Show(strMsg);
      _simple_radioButton->setChecked(true);
    } else {
      widget_enable(0);
    }
  } else if (_signals->device()->is_file() == false) {
    QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_AD_TRIGGER_NEED_HARDWARE),
                       "Advanced Trigger need DSLogic Hardware Support!"));
    MsgBox::Show(strMsg);
    _simple_radioButton->setChecked(true);
  }
}

void TriggerDock::widget_enable(int index) {
  (void)index;

  int enable_stages;
  _stages_label->setDisabled(false);
  stages_comboBox->setVisible(true);
  stages_comboBox->setDisabled(false);
  _adv_tabWidget->setDisabled(false);
  enable_stages = stages_comboBox->currentText().toInt();

  for (int i = 0; i < enable_stages; i++) {
    _stage_tabWidget->setTabEnabled(i, true);
  }

  for (int i = enable_stages; i < TriggerStages; i++) {
    _stage_tabWidget->setTabEnabled(i, false);
  }
}

void TriggerDock::value_changed() {
  PopupLineEdit *sc = dynamic_cast<PopupLineEdit *>(sender());
  if (sc != nullptr) {
    for (int i = 0; i < TriggerProbes * 2 - 1; i++) {
      if ((i >= sc->text().size()) || (i % 2 == 0 && sc->text().at(i) == ' ')) {
        sc->setText(sc->text().insert(i, 'X'));
        i++;
      }
    }
    sc->setText(sc->text().toUpper());
    lineEdit_highlight(sc);
  }
}

void TriggerDock::device_updated() {
  // SR_CONF_HW_DEPTH and SR_CONF_STREAM fork keys were deleted from pxlogic.c.
  // hw_depth used to come from the driver; now we derive the trigger position
  // range from the sample limit (the user-configured capture depth). When the
  // capture is smaller than the hardware buffer, the trigger can be positioned
  // across the full capture; in stream mode the trigger is effectively
  // position-fixed (maxRange = 1).
  bool stream = false;
  uint8_t maxRange;
  int mode = _signals->device()->get_work_mode();
  int ch_num;

  if (mode == LOGIC) {
    // SR_CONF_STREAM deleted — use DeviceAgent::is_stream_mode() instead.
    stream = _signals->device()->is_stream_mode();

    _adv_radioButton->setEnabled(!stream);
    _position_spinBox->setEnabled(!stream);
    _position_slider->setEnabled(!stream);

    // Without SR_CONF_HW_DEPTH, we treat the configured sample limit as the
    // effective hardware depth — the trigger can range across the full
    // capture (maxRange = DS_MAX_TRIG_PERCENT). Stream mode pins it (maxRange = 1).
    if (stream)
      maxRange = 1;
    else
      maxRange = DS_MAX_TRIG_PERCENT;

    _position_spinBox->setRange(MinTrigPosition, maxRange);
    _position_slider->setRange(MinTrigPosition, maxRange);

    if (_signals->device()->is_virtual() || stream) {
      _simple_radioButton->setChecked(true);
      simple_trigger();
    }
  }

  bool ret = _signals->device()->get_config_int16(SR_CONF_TOTAL_CH_NUM, ch_num);
  if (ret) {
    if (ch_num != _cur_ch_num) {
      _cur_ch_num = ch_num;
      setup_adv_tab();
      retranslateUi();
    }
  }

  this->setEnabled(_session->is_loop_mode() == false);
}

void TriggerDock::select_simple_trigger() {
  if (_simple_radioButton && !_simple_radioButton->isChecked()) {
    _simple_radioButton->setChecked(true);
    simple_trigger();
  }
}

bool TriggerDock::commit_trigger() {
  // trigger mode update
  if (_simple_radioButton->isChecked()) {
    // Task 8.4: mirror simple-mode state into Core TriggerConfig.
    // sync_trigger_to_libsigrok() in start_capture will push it to ds_trigger_*.
    data::TriggerConfig cfg;
    cfg.set_mode(data::TriggerConfig::Simple);
    cfg.set_trigger_pos(_position_slider->value());
    _session->set_trigger_config(cfg);
    return false;
  } else {
    // Task 8.4: build Core TriggerConfig so headless/API layers can read
    // the same trigger state without touching the UI. sync_trigger_to_libsigrok()
    // in start_capture pushes cfg to ds_trigger_* as the single sync point.
    data::TriggerConfig cfg;
    std::vector<data::TriggerConfig::Stage> stages;
    cfg.set_trigger_pos(_position_slider->value());
    cfg.set_adv_enabled(true);
    cfg.set_adv_tab_index(_adv_tabWidget->currentIndex());
    if (_adv_tabWidget->currentIndex() == 0)
      cfg.set_mode(data::TriggerConfig::Adv);
    else
      cfg.set_mode(data::TriggerConfig::Serial);
    const int stage_n = stages_comboBox->currentText().toInt();
    cfg.set_stage_count(stage_n);
    if (_adv_tabWidget->currentIndex() == 1) {
      cfg.set_serial_data_channel(_serial_data_comboBox->currentText().toInt());
      cfg.set_serial_bits(_serial_bits_comboBox->currentText().toInt());
      cfg.set_serial_value(_serial_value_lineEdit->text());
    }

    // trigger value update
    if (_adv_tabWidget->currentIndex() == 0) {
      for (int i = 0; i < stage_n; i++) {
        QString value0_str, value1_str;
        if (_cur_ch_num == 32) {
          value0_str = _value0_ext32_lineEdit_list.at(i)->text() + " " +
                       _value0_lineEdit_list.at(i)->text();
          value1_str = _value1_ext32_lineEdit_list.at(i)->text() + " " +
                       _value1_lineEdit_list.at(i)->text();
        } else {
          value0_str = _value0_lineEdit_list.at(i)->text();
          value1_str = _value1_lineEdit_list.at(i)->text();
        }
        data::TriggerConfig::Stage st;
        st.value0 = value0_str;
        st.value1 = value1_str;
        stages.push_back(st);
      }
    } else if (_adv_tabWidget->currentIndex() == 1) {
      QString start_str, stop_str, edge_str, comp_str;
      if (_cur_ch_num == 32) {
        start_str = _serial_start_ext32_lineEdit->text() + " " +
                    _serial_start_lineEdit->text();
        stop_str = _serial_stop_ext32_lineEdit->text() + " " +
                   _serial_stop_lineEdit->text();
        edge_str = _serial_edge_ext32_lineEdit->text() + " " +
                   _serial_edge_lineEdit->text();
        comp_str = _value1_ext32_lineEdit_list.at(1)->text() + " " +
                   _value1_lineEdit_list.at(1)->text();
      } else {
        start_str = _serial_start_lineEdit->text();
        stop_str = _serial_stop_lineEdit->text();
        edge_str = _serial_edge_lineEdit->text();
        comp_str = _value1_lineEdit_list.at(1)->text();
      }

      //_serial_data_comboBox
      const int data_channel = _serial_data_comboBox->currentText().toInt();
      QString channel = "X X X X X X X X X X X X X X X X";
      QString channel_ext32 = "X X X X X X X X X X X X X X X X";
      if (_cur_ch_num == 32) {
        if (data_channel < 16)
          channel.replace(30 - 2 * data_channel, 1, '0');
        else
          channel_ext32.replace(30 - 2 * (data_channel - 16), 1, '0');
      } else {
        channel.replace(30 - 2 * data_channel, 1, '0');
      }

      // SERIAL stages: 0=start/stop, 1=edge/comp, 2=channel/channel_ext32,
      // 3=data/comp_ext32
      data::TriggerConfig::Stage s0;
      s0.value0 = start_str;
      s0.value1 = stop_str;
      data::TriggerConfig::Stage s1;
      s1.value0 = edge_str;
      s1.value1 = comp_str;
      data::TriggerConfig::Stage s2;
      s2.value0 = channel;
      s2.value1 = channel_ext32;
      data::TriggerConfig::Stage s3;
      s3.value0 = _serial_value_lineEdit->text();
      s3.value1 = _value1_lineEdit_list.at(3)->text();
      stages.push_back(s0);
      stages.push_back(s1);
      stages.push_back(s2);
      stages.push_back(s3);
    }

    // trigger logic update
    for (int i = 0; i < stage_n; i++) {
      const char logic = (_contiguous_checkbox_list.at(i)->isChecked() << 1) +
                         _logic_comboBox_list.at(i)->currentIndex();
      if (i < (int)stages.size())
        stages[i].logic = logic;
    }

    // trigger inv update
    for (int i = 0; i < stage_n; i++) {
      if (i < (int)stages.size()) {
        stages[i].inv0 = _inv0_comboBox_list.at(i)->currentIndex();
        stages[i].inv1 = _inv1_comboBox_list.at(i)->currentIndex();
      }
    }

    // trigger count update
    if (_adv_tabWidget->currentIndex() == 0) {
      for (int i = 0; i < stage_n; i++) {
        if (i < (int)stages.size()) {
          stages[i].count0 = _count_spinBox_list.at(i)->value();
          stages[i].count1 = 0;
        }
      }
    } else if (_adv_tabWidget->currentIndex() == 1) {
      if (1 < (int)stages.size()) {
        stages[1].count0 = 1;
        stages[1].count1 = 0;
      }
      if (3 < (int)stages.size()) {
        stages[3].count0 = _serial_bits_comboBox->currentText().toInt() - 1;
        stages[3].count1 = 0;
      }
    }

    cfg.set_stages(stages);
    _session->set_trigger_config(cfg);
    return true;
  }
}

void TriggerDock::update_view() {}

QJsonObject TriggerDock::get_session() {
  // Task 8.5: serialize from Core TriggerConfig (the canonical state) instead
  // of reading UI controls directly. Original JSON key names are preserved.
  const auto &cfg = _signals->trigger_config();
  QJsonObject trigSes;
  trigSes["advTriggerMode"] = cfg.adv_enabled();
  trigSes["triggerPos"] = cfg.trigger_pos();
  // original semantics: stages_comboBox currentIndex (= stage_count - 1)
  trigSes["triggerStages"] = qMax(0, cfg.stage_count() - 1);
  trigSes["triggerTab"] = cfg.adv_tab_index();

  // SERIAL scalar fields
  trigSes["serialTriggerChannel"] = cfg.serial_data_channel();
  // original semantics: _serial_bits_comboBox currentIndex (= bits - 1)
  trigSes["serialTriggerBits"] = qMax(0, cfg.serial_bits() - 1);
  trigSes["serialTriggerData"] = cfg.serial_value();

  // SERIAL start/stop/clock live inside Core stages (layout: 0=start/stop,
  // 1=edge/comp)
  QString start_lower, start_ext32, stop_lower, stop_ext32, edge_lower,
      edge_ext32;
  if (cfg.mode() == data::TriggerConfig::Serial && cfg.stages().size() >= 2) {
    split_trigger_value(cfg.stages()[0].value0, start_ext32, start_lower);
    split_trigger_value(cfg.stages()[0].value1, stop_ext32, stop_lower);
    split_trigger_value(cfg.stages()[1].value0, edge_ext32, edge_lower);
  }
  trigSes["serialTriggerStart"] = start_lower;
  trigSes["serialTriggerStop"] = stop_lower;
  trigSes["serialTriggerClock"] = edge_lower;
  if (_cur_ch_num == 32) {
    trigSes["serialTriggerExt32Start"] = start_ext32;
    trigSes["serialTriggerExt32Stop"] = stop_ext32;
    trigSes["serialTriggerExt32Clock"] = edge_ext32;
  }

  // Per-stage data from Core
  for (int i = 0; i < (int)cfg.stages().size(); i++) {
    const auto &s = cfg.stages()[i];
    QString v0_ext, v0_low, v1_ext, v1_low;
    split_trigger_value(s.value0, v0_ext, v0_low);
    split_trigger_value(s.value1, v1_ext, v1_low);

    trigSes["stageTriggerValue0" + QString::number(i)] = v0_low;
    trigSes["stageTriggerValue1" + QString::number(i)] = v1_low;
    trigSes["stageTriggerInv0" + QString::number(i)] = s.inv0;
    trigSes["stageTriggerInv1" + QString::number(i)] = s.inv1;
    trigSes["stageTriggerLogic" + QString::number(i)] = s.logic & 1;
    trigSes["stageTriggerCount" + QString::number(i)] = s.count0;
    trigSes["stageTriggerContiguous" + QString::number(i)] = (s.logic >> 1) & 1;

    if (_cur_ch_num == 32) {
      trigSes["stageTriggerExt32Value0" + QString::number(i)] = v0_ext;
      trigSes["stageTriggerExt32Value1" + QString::number(i)] = v1_ext;
    }
  }

  return trigSes;
}

void TriggerDock::set_session(QJsonObject ses) {
  // Task 8.5: build Core TriggerConfig from JSON, push it to Core, then fill
  // the UI from Core (the canonical state). Original JSON key names preserved.
  const bool adv_mode = ses.value("advTriggerMode").toBool(false);
  const int tab = ses.value("triggerTab").toInt(0);
  const int stage_count =
      ses.value("triggerStages").toInt(0) + 1; // index -> count

  data::TriggerConfig cfg;
  cfg.set_adv_enabled(adv_mode);
  cfg.set_adv_tab_index(tab);
  cfg.set_trigger_pos(ses.value("triggerPos").toInt(1));
  cfg.set_stage_count(stage_count);
  if (!adv_mode)
    cfg.set_mode(data::TriggerConfig::Simple);
  else if (tab == 1)
    cfg.set_mode(data::TriggerConfig::Serial);
  else
    cfg.set_mode(data::TriggerConfig::Adv);
  cfg.set_serial_data_channel(ses.value("serialTriggerChannel").toInt(0));
  cfg.set_serial_bits(ses.value("serialTriggerBits").toInt(0) +
                      1); // index -> count
  cfg.set_serial_value(ses.value("serialTriggerData").toString());

  std::vector<data::TriggerConfig::Stage> stages;
  if (cfg.mode() == data::TriggerConfig::Adv) {
    for (int i = 0; i < stage_count; i++) {
      data::TriggerConfig::Stage st;
      const QString v0_low =
          ses.value("stageTriggerValue0" + QString::number(i)).toString();
      const QString v1_low =
          ses.value("stageTriggerValue1" + QString::number(i)).toString();
      QString v0_ext, v1_ext;
      if (_cur_ch_num == 32) {
        v0_ext = ses.value("stageTriggerExt32Value0" + QString::number(i))
                     .toString();
        v1_ext = ses.value("stageTriggerExt32Value1" + QString::number(i))
                     .toString();
      }
      st.value0 = v0_ext.isEmpty() ? v0_low : (v0_ext + " " + v0_low);
      st.value1 = v1_ext.isEmpty() ? v1_low : (v1_ext + " " + v1_low);
      st.inv0 = ses.value("stageTriggerInv0" + QString::number(i)).toInt(0);
      st.inv1 = ses.value("stageTriggerInv1" + QString::number(i)).toInt(0);
      const int logic_index =
          ses.value("stageTriggerLogic" + QString::number(i)).toInt(0);
      const bool conti =
          ses.value("stageTriggerContiguous" + QString::number(i))
              .toBool(false);
      st.logic = (conti ? 2 : 0) | (logic_index & 1);
      st.count0 = ses.value("stageTriggerCount" + QString::number(i)).toInt(1);
      st.count1 = 0;
      stages.push_back(st);
    }
  } else if (cfg.mode() == data::TriggerConfig::Serial) {
    // SERIAL stages: 0=start/stop, 1=edge/comp, 2=channel, 3=data
    const QString start_low = ses.value("serialTriggerStart").toString();
    const QString stop_low = ses.value("serialTriggerStop").toString();
    const QString edge_low = ses.value("serialTriggerClock").toString();
    QString start_ext, stop_ext, edge_ext;
    if (_cur_ch_num == 32) {
      if (ses.contains("serialTriggerExt32Start"))
        start_ext = ses.value("serialTriggerExt32Start").toString();
      if (ses.contains("serialTriggerExt32Stop"))
        stop_ext = ses.value("serialTriggerExt32Stop").toString();
      if (ses.contains("serialTriggerExt32Clock"))
        edge_ext = ses.value("serialTriggerExt32Clock").toString();
    }
    const QString start_str =
        start_ext.isEmpty() ? start_low : (start_ext + " " + start_low);
    const QString stop_str =
        stop_ext.isEmpty() ? stop_low : (stop_ext + " " + stop_low);
    const QString edge_str =
        edge_ext.isEmpty() ? edge_low : (edge_ext + " " + edge_low);

    const int data_channel = cfg.serial_data_channel();
    QString channel = "X X X X X X X X X X X X X X X X";
    QString channel_ext32 = "X X X X X X X X X X X X X X X X";
    if (_cur_ch_num == 32) {
      if (data_channel < 16)
        channel.replace(30 - 2 * data_channel, 1, '0');
      else
        channel_ext32.replace(30 - 2 * (data_channel - 16), 1, '0');
    } else {
      channel.replace(30 - 2 * data_channel, 1, '0');
    }

    // comp (stage1.value1) and comp_ext32 lower (stage3.value1) from per-stage
    // keys
    const QString comp_low =
        ses.value("stageTriggerValue1" + QString::number(1)).toString();
    QString comp_ext;
    if (_cur_ch_num == 32)
      comp_ext =
          ses.value("stageTriggerExt32Value1" + QString::number(1)).toString();
    const QString comp_str =
        comp_ext.isEmpty() ? comp_low : (comp_ext + " " + comp_low);
    const QString s3v1 =
        ses.value("stageTriggerValue1" + QString::number(3)).toString();

    data::TriggerConfig::Stage s0;
    s0.value0 = start_str;
    s0.value1 = stop_str;
    data::TriggerConfig::Stage s1;
    s1.value0 = edge_str;
    s1.value1 = comp_str;
    data::TriggerConfig::Stage s2;
    s2.value0 = channel;
    s2.value1 = channel_ext32;
    data::TriggerConfig::Stage s3;
    s3.value0 = cfg.serial_value();
    s3.value1 = s3v1;
    stages.push_back(s0);
    stages.push_back(s1);
    stages.push_back(s2);
    stages.push_back(s3);
  }
  cfg.set_stages(stages);
  _session->set_trigger_config(cfg);

  // Task 6: UI 填充统一走 refresh_ui_from_core()，不再内联读 JSON。
  refresh_ui_from_core();
}

void TriggerDock::refresh_ui_from_core() {
  // Task 6: 纯 UI 刷新——从 Core TriggerConfig 重新填充所有触发控件。
  // View 层不再解析 .pxc trigger 段 JSON；反序列化由 Core from_json 完成，
  // 本方法只负责把 Core 状态映射到 QWidget 控件。
  const auto &tcfg = _signals->trigger_config();
  _position_slider->setValue(tcfg.trigger_pos());
  stages_comboBox->setCurrentIndex(tcfg.stage_count() - 1);
  _adv_tabWidget->setCurrentIndex(tcfg.adv_tab_index());
  if (tcfg.adv_enabled())
    _adv_radioButton->click();
  else
    _simple_radioButton->click();

  // Per-stage UI: ADV mode reads from Core stages; otherwise defaults。
  // （Simple 模式下 stages 为空，per-stage 控件本就隐藏，填默认值无害。）
  for (int i = 0; i < stages_comboBox->count(); i++) {
    QString v0_low, v1_low, v0_ext, v1_ext;
    int inv0 = 0, inv1 = 0, logic_index = 0, count0 = 1;
    bool conti = false;
    if (tcfg.mode() == data::TriggerConfig::Adv &&
        i < (int)tcfg.stages().size()) {
      const auto &s = tcfg.stages()[i];
      split_trigger_value(s.value0, v0_ext, v0_low);
      split_trigger_value(s.value1, v1_ext, v1_low);
      inv0 = s.inv0;
      inv1 = s.inv1;
      logic_index = s.logic & 1;
      conti = (s.logic >> 1) & 1;
      count0 = s.count0;
    }

    _value0_lineEdit_list.at(i)->setText(v0_low);
    lineEdit_highlight(_value0_lineEdit_list.at(i));
    _value1_lineEdit_list.at(i)->setText(v1_low);
    lineEdit_highlight(_value1_lineEdit_list.at(i));
    _inv0_comboBox_list.at(i)->setCurrentIndex(inv0);
    _inv1_comboBox_list.at(i)->setCurrentIndex(inv1);
    _logic_comboBox_list.at(i)->setCurrentIndex(logic_index);
    _count_spinBox_list.at(i)->setValue(count0);
    _contiguous_checkbox_list.at(i)->setChecked(conti);

    if (_cur_ch_num == 32) {
      if (!v0_ext.isEmpty()) {
        _value0_ext32_lineEdit_list.at(i)->setText(v0_ext);
        lineEdit_highlight(_value0_ext32_lineEdit_list.at(i));
      }
      if (!v1_ext.isEmpty()) {
        _value1_ext32_lineEdit_list.at(i)->setText(v1_ext);
        lineEdit_highlight(_value1_ext32_lineEdit_list.at(i));
      }
    }
  }

  // Serial UI: SERIAL mode reads from Core stages; otherwise defaults。
  QString s_start_low, s_stop_low, s_edge_low, s_start_ext, s_stop_ext,
      s_edge_ext;
  if (tcfg.mode() == data::TriggerConfig::Serial && tcfg.stages().size() >= 2) {
    split_trigger_value(tcfg.stages()[0].value0, s_start_ext, s_start_low);
    split_trigger_value(tcfg.stages()[0].value1, s_stop_ext, s_stop_low);
    split_trigger_value(tcfg.stages()[1].value0, s_edge_ext, s_edge_low);
  }
  _serial_start_lineEdit->setText(s_start_low);
  lineEdit_highlight(_serial_start_lineEdit);
  _serial_stop_lineEdit->setText(s_stop_low);
  lineEdit_highlight(_serial_stop_lineEdit);
  _serial_edge_lineEdit->setText(s_edge_low);
  lineEdit_highlight(_serial_edge_lineEdit);
  _serial_data_comboBox->setCurrentIndex(tcfg.serial_data_channel());
  _serial_value_lineEdit->setText(tcfg.serial_value());
  lineEdit_highlight(_serial_value_lineEdit);
  _serial_bits_comboBox->setCurrentIndex(tcfg.serial_bits() - 1);

  if (_cur_ch_num == 32) {
    if (!s_start_ext.isEmpty()) {
      _serial_start_ext32_lineEdit->setText(s_start_ext);
      lineEdit_highlight(_serial_start_ext32_lineEdit);
    }
    if (!s_stop_ext.isEmpty()) {
      _serial_stop_ext32_lineEdit->setText(s_stop_ext);
      lineEdit_highlight(_serial_stop_ext32_lineEdit);
    }
    if (!s_edge_ext.isEmpty()) {
      _serial_edge_ext32_lineEdit->setText(s_edge_ext);
      lineEdit_highlight(_serial_edge_ext32_lineEdit);
    }
  }
}

void TriggerDock::setup_adv_tab() {
  int row;

  for (int i = _adv_tabWidget->count(); i > 0; i--) {
    _adv_tabWidget->widget(i - 1)->deleteLater();
    _adv_tabWidget->removeTab(i - 1);
  }

  _logic_comboBox_list.clear();
  _value0_lineEdit_list.clear();
  _count_spinBox_list.clear();
  _inv0_comboBox_list.clear();
  _value1_lineEdit_list.clear();
  _inv1_comboBox_list.clear();
  _contiguous_checkbox_list.clear();
  _inv_exp_label_list.clear();
  _count_exp_label_list.clear();
  _contiguous_label_list.clear();
  _stage_note_label_list.clear();
  _stage_groupBox_list.clear();

  _value0_ext32_lineEdit_list.clear();
  _value1_ext32_lineEdit_list.clear();

  // QFont font("Monaco");
  // font.setStyleHint(QFont::Monospace);
  // font.setFixedPitch(true);

  QFont labelFont = dock_font_label();
  QFont contentFont = dock_font_content();

  _stage_tabWidget = new QTabWidget(_widget);
  _stage_tabWidget->setTabPosition(QTabWidget::East);
  _stage_tabWidget->setUsesScrollButtons(false);

  const QString mask = "N N N N N N N N N N N N N N N N";
  QRegularExpression value_rx("[10XRFCxrfc ]+");
  QValidator *value_validator =
      new QRegularExpressionValidator(value_rx, _stage_tabWidget);

  for (int i = 0; i < TriggerStages; i++) {
    DsComboBox *_logic_comboBox = new DsComboBox(_stage_tabWidget);
    _logic_comboBox->setObjectName("dock_content");
    _logic_comboBox->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_OR), "Or"));
    _logic_comboBox->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_AND), "And"));
    _logic_comboBox->setCurrentIndex(1);
    _logic_comboBox_list.push_back(_logic_comboBox);

    PopupLineEdit *_value0_lineEdit =
        new PopupLineEdit("X X X X X X X X X X X X X X X X", _stage_tabWidget);
    _value0_lineEdit->setObjectName("dock_content");
    _value0_lineEdit->setFont(contentFont);
    _value0_lineEdit->setValidator(value_validator);
    _value0_lineEdit->setMaxLength(TriggerProbes * 2 - 1);
    _value0_lineEdit->setInputMask(mask);
    _value0_lineEdit->setSizePolicy(QSizePolicy::Preferred,
                                    QSizePolicy::Preferred);
    _value0_lineEdit_list.push_back(_value0_lineEdit);
    PopupLineEdit *_count_spinBox = new PopupLineEdit(_stage_tabWidget);
    _count_spinBox->set_number_mode(true);
    _count_spinBox->setRange(1, INT32_MAX);
    //_count_spinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
    _count_spinBox_list.push_back(_count_spinBox);
    DsComboBox *_inv0_comboBox = new DsComboBox(_stage_tabWidget);
    _inv0_comboBox->setObjectName("dock_content");
    // tr
    _inv0_comboBox->addItem("==");
    _inv0_comboBox->addItem("!=");
    _inv0_comboBox_list.push_back(_inv0_comboBox);

    PopupLineEdit *_value1_lineEdit =
        new PopupLineEdit("X X X X X X X X X X X X X X X X", _stage_tabWidget);
    _value1_lineEdit->setObjectName("dock_content");
    _value1_lineEdit->setFont(contentFont);
    _value1_lineEdit->setValidator(value_validator);
    _value1_lineEdit->setMaxLength(TriggerProbes * 2 - 1);
    _value1_lineEdit->setInputMask(mask);
    _value1_lineEdit->setSizePolicy(QSizePolicy::Preferred,
                                    QSizePolicy::Preferred);
    _value1_lineEdit_list.push_back(_value1_lineEdit);
    DsComboBox *_inv1_comboBox = new DsComboBox(_stage_tabWidget);
    _inv1_comboBox->setObjectName("dock_content");
    // tr
    _inv1_comboBox->addItem("==");
    _inv1_comboBox->addItem("!=");
    _inv1_comboBox_list.push_back(_inv1_comboBox);

    connect(_value0_lineEdit, &QLineEdit::editingFinished, this,
            &TriggerDock::value_changed);
    connect(_value1_lineEdit, &QLineEdit::editingFinished, this,
            &TriggerDock::value_changed);

    QCheckBox *_contiguous_checkbox = new QCheckBox(_stage_tabWidget);
    _contiguous_checkbox_list.push_back(_contiguous_checkbox);

    QLabel *value0_exp_label =
        new QLabel("15 ---------- 8 7 ----------- 0 ", _stage_tabWidget);
    value0_exp_label->setObjectName("dock_label");
    value0_exp_label->setFont(labelFont);
    QLabel *inv0_exp_label = new QLabel(_stage_tabWidget);
    inv0_exp_label->setObjectName("dock_label");
    _inv_exp_label_list.push_back(inv0_exp_label);
    QLabel *value1_exp_label =
        new QLabel("15 ---------- 8 7 ----------- 0 ", _stage_tabWidget);
    value1_exp_label->setObjectName("dock_label");
    value1_exp_label->setFont(labelFont);
    QLabel *inv1_exp_label = new QLabel(_stage_tabWidget);
    inv1_exp_label->setObjectName("dock_label");
    _inv_exp_label_list.push_back(inv1_exp_label);

    QLabel *count_exp_label = new QLabel(_stage_tabWidget);
    count_exp_label->setObjectName("dock_label");
    _count_exp_label_list.push_back(count_exp_label);

    QVBoxLayout *stage_layout = new QVBoxLayout();
    QGridLayout *stage_glayout = new QGridLayout();
    stage_glayout->setVerticalSpacing(5);

    row = 1;
    if (_cur_ch_num == 32) {
      PopupLineEdit *_value0_ext32_lineEdit = new PopupLineEdit(
          "X X X X X X X X X X X X X X X X", _stage_tabWidget);
      _value0_ext32_lineEdit->setObjectName("dock_content");
      _value0_ext32_lineEdit->setFont(contentFont);
      _value0_ext32_lineEdit->setValidator(value_validator);
      _value0_ext32_lineEdit->setMaxLength(TriggerProbes * 2 - 1);
      _value0_ext32_lineEdit->setInputMask(mask);
      _value0_ext32_lineEdit->setSizePolicy(QSizePolicy::Preferred,
                                            QSizePolicy::Preferred);
      _value0_ext32_lineEdit_list.push_back(_value0_ext32_lineEdit);

      PopupLineEdit *_value1_ext32_lineEdit = new PopupLineEdit(
          "X X X X X X X X X X X X X X X X", _stage_tabWidget);
      _value1_ext32_lineEdit->setObjectName("dock_content");
      _value1_ext32_lineEdit->setFont(contentFont);
      _value1_ext32_lineEdit->setValidator(value_validator);
      _value1_ext32_lineEdit->setMaxLength(TriggerProbes * 2 - 1);
      _value1_ext32_lineEdit->setInputMask(mask);
      _value1_ext32_lineEdit->setSizePolicy(QSizePolicy::Preferred,
                                            QSizePolicy::Preferred);
      _value1_ext32_lineEdit_list.push_back(_value1_ext32_lineEdit);

      QLabel *value0_ext32_exp_label =
          new QLabel("31 --------- 24 23 ---------- 16", _stage_tabWidget);
      value0_ext32_exp_label->setObjectName("dock_label");
      value0_ext32_exp_label->setFont(labelFont);
      QLabel *value1_ext32_exp_label =
          new QLabel("31 --------- 24 23 ---------- 16", _stage_tabWidget);
      value1_ext32_exp_label->setObjectName("dock_label");
      value1_ext32_exp_label->setFont(labelFont);

      stage_glayout->addWidget(value0_ext32_exp_label, row++, 0);
      stage_glayout->addWidget(_value0_ext32_lineEdit, row++, 0);
      stage_glayout->addWidget(value0_exp_label, row, 0);
      stage_glayout->addWidget(inv0_exp_label, row++, 1);
      stage_glayout->addWidget(_value0_lineEdit, row, 0);
      stage_glayout->addWidget(_inv0_comboBox, row, 1);
      stage_glayout->addWidget(_logic_comboBox, row++, 2);

      stage_glayout->addWidget(new QLabel(_stage_tabWidget), row++, 0);

      stage_glayout->addWidget(value1_ext32_exp_label, row++, 0);
      stage_glayout->addWidget(_value1_ext32_lineEdit, row++, 0);
      stage_glayout->addWidget(value1_exp_label, row, 0);
      stage_glayout->addWidget(inv1_exp_label, row++, 1);
      stage_glayout->addWidget(_value1_lineEdit, row, 0);
      stage_glayout->addWidget(_inv1_comboBox, row++, 1);

      connect(_value0_ext32_lineEdit, &QLineEdit::editingFinished, this,
              &TriggerDock::value_changed);
      connect(_value1_ext32_lineEdit, &QLineEdit::editingFinished, this,
              &TriggerDock::value_changed);
    } else {
      stage_glayout->addWidget(value0_exp_label, row, 0);
      stage_glayout->addWidget(inv0_exp_label, row++, 1);
      stage_glayout->addWidget(_value0_lineEdit, row, 0);
      stage_glayout->addWidget(_inv0_comboBox, row, 1);
      stage_glayout->addWidget(_logic_comboBox, row++, 2);

      stage_glayout->addWidget(new QLabel(_stage_tabWidget), row++, 0);

      stage_glayout->addWidget(value1_exp_label, row, 0);
      stage_glayout->addWidget(inv1_exp_label, row++, 1);
      stage_glayout->addWidget(_value1_lineEdit, row, 0);
      stage_glayout->addWidget(_inv1_comboBox, row++, 1);
    }

    stage_glayout->addWidget(new QLabel(_stage_tabWidget), row++, 0);

    QLabel *contiguous_label = new QLabel(_stage_tabWidget);
    contiguous_label->setObjectName("dock_label");
    _contiguous_label_list.push_back(contiguous_label);
    stage_glayout->addWidget(contiguous_label, row, 1, 1, 2);
    stage_glayout->addWidget(_contiguous_checkbox, row++, 0, 1, 1,
                             Qt::AlignRight);
    stage_glayout->addWidget(count_exp_label, row, 1, 1, 2);
    stage_glayout->addWidget(_count_spinBox, row++, 0);

    stage_layout->addLayout(stage_glayout);
    stage_layout->addSpacing(20);
    QLabel *stage_note_label = new QLabel(_stage_tabWidget);
    stage_note_label->setObjectName("dock_label");
    _stage_note_label_list.push_back(stage_note_label);
    stage_layout->addWidget(stage_note_label);
    stage_layout->addStretch(1);

    QWidget *stage_groupBox = new QWidget(_stage_tabWidget);
    stage_groupBox->setContentsMargins(5, 5, 5, 5);
    QVBoxLayout *stage_vbox = new QVBoxLayout(stage_groupBox);
    stage_vbox->setContentsMargins(0, 0, 0, 0);
    stage_vbox->setSpacing(0);
    stage_vbox->addLayout(stage_layout);
    _stage_groupBox_list.push_back(stage_groupBox);

    _stage_tabWidget->addTab(stage_groupBox, QString::number(i));
  }

  _serial_groupBox = new QWidget(_widget);
  _serial_groupBox->setContentsMargins(5, 5, 5, 5);

  _serial_start_label = new QLabel(_serial_groupBox);
  _serial_start_label->setObjectName("dock_label");
  _serial_start_lineEdit =
      new PopupLineEdit("X X X X X X X X X X X X X X X X", _serial_groupBox);
  _serial_start_lineEdit->setObjectName("dock_content");
  _serial_start_lineEdit->setFont(contentFont);
  _serial_start_lineEdit->setValidator(value_validator);
  _serial_start_lineEdit->setMaxLength(TriggerProbes * 2 - 1);
  _serial_start_lineEdit->setInputMask(mask);
  _serial_start_lineEdit->setSizePolicy(QSizePolicy::Preferred,
                                        QSizePolicy::Preferred);

  _serial_stop_label = new QLabel(_serial_groupBox);
  _serial_stop_label->setObjectName("dock_label");
  _serial_stop_lineEdit =
      new PopupLineEdit("X X X X X X X X X X X X X X X X", _serial_groupBox);
  _serial_stop_lineEdit->setObjectName("dock_content");
  _serial_stop_lineEdit->setFont(contentFont);
  _serial_stop_lineEdit->setValidator(value_validator);
  _serial_stop_lineEdit->setMaxLength(TriggerProbes * 2 - 1);
  _serial_stop_lineEdit->setInputMask(mask);
  _serial_stop_lineEdit->setSizePolicy(QSizePolicy::Preferred,
                                       QSizePolicy::Preferred);

  _serial_edge_label = new QLabel(_serial_groupBox);
  _serial_edge_label->setObjectName("dock_label");
  _serial_edge_lineEdit =
      new PopupLineEdit("X X X X X X X X X X X X X X X X", _serial_groupBox);
  _serial_edge_lineEdit->setObjectName("dock_content");
  _serial_edge_lineEdit->setFont(contentFont);
  _serial_edge_lineEdit->setValidator(value_validator);
  _serial_edge_lineEdit->setMaxLength(TriggerProbes * 2 - 1);
  _serial_edge_lineEdit->setInputMask(mask);
  _serial_edge_lineEdit->setSizePolicy(QSizePolicy::Preferred,
                                       QSizePolicy::Preferred);

  _serial_data_label = new QLabel(_serial_groupBox);
  _serial_data_label->setObjectName("dock_label");
  _serial_data_comboBox = new DsComboBox(_serial_groupBox);
  _serial_data_comboBox->setObjectName("dock_content");

  for (int i = 0; i < _cur_ch_num; i++) {
    _serial_data_comboBox->addItem(QString::number(i));
  }

  _serial_value_label = new QLabel(_serial_groupBox);
  _serial_value_label->setObjectName("dock_label");
  _serial_value_lineEdit =
      new PopupLineEdit("X X X X X X X X X X X X X X X X", _serial_groupBox);
  _serial_value_lineEdit->setObjectName("dock_content");
  _serial_value_lineEdit->setFont(contentFont);
  _serial_value_lineEdit->setMaxLength(TriggerProbes * 2 - 1);
  _serial_value_lineEdit->setInputMask(mask);
  _serial_value_lineEdit->setSizePolicy(QSizePolicy::Preferred,
                                        QSizePolicy::Preferred);

  QRegularExpression value_rx2("[10Xx ]+");
  QValidator *value_validator2 =
      new QRegularExpressionValidator(value_rx2, _stage_tabWidget);
  _serial_value_lineEdit->setValidator(value_validator2);

  _serial_hex_label = new QLabel(_serial_groupBox);
  _serial_hex_label->setObjectName("dock_label");
  _serial_hex_lineEdit = new PopupLineEdit("", _serial_groupBox);
  _serial_hex_lineEdit->setObjectName("dock_content");
  _serial_hex_lineEdit->setMaxLength(4);
  QRegularExpression value_rx_hex("[0-9a-fA-F]+");
  QValidator *value_validator_hex =
      new QRegularExpressionValidator(value_rx_hex, _stage_tabWidget);
  _serial_hex_lineEdit->setValidator(value_validator_hex);
  _serial_hex_lineEdit->setMaximumWidth(70);
  _serial_hex_lineEdit->setReadOnly(true);

  QCheckBox *hex_ckbox = new QCheckBox();
  _serial_hex_ck_label = new QLabel();
  _serial_hex_ck_label->setObjectName("dock_label");
  hex_ckbox->setMaximumWidth(18);

  QHBoxLayout *hex_lay = new QHBoxLayout();
  hex_lay->setSpacing(5);
  hex_lay->setContentsMargins(0, 0, 0, 0);
  QWidget *hex_wid = new QWidget();
  hex_wid->setLayout(hex_lay);
  hex_lay->setAlignment(Qt::AlignLeft);
  hex_lay->addWidget(_serial_hex_lineEdit);
  hex_lay->addWidget(hex_ckbox);
  hex_lay->addWidget(_serial_hex_ck_label);

  connect(hex_ckbox, &QCheckBox::clicked, this,
          &TriggerDock::on_hex_checkbox_click);

  _serial_bits_comboBox = new DsComboBox(_serial_groupBox);
  _serial_bits_comboBox->setObjectName("dock_content");

  for (int i = 1; i <= 16; i++) {
    _serial_bits_comboBox->addItem(QString::number(i));
  }

  QVBoxLayout *serial_layout = new QVBoxLayout();
  QGridLayout *serial_glayout = new QGridLayout();
  serial_glayout->setVerticalSpacing(5);

  row = 1;
  if (_cur_ch_num == 32) {
    _serial_start_ext32_lineEdit =
        new PopupLineEdit("X X X X X X X X X X X X X X X X", _serial_groupBox);
    _serial_start_ext32_lineEdit->setObjectName("dock_content");
    _serial_start_ext32_lineEdit->setFont(contentFont);
    _serial_start_ext32_lineEdit->setValidator(value_validator);
    _serial_start_ext32_lineEdit->setMaxLength(TriggerProbes * 2 - 1);
    _serial_start_ext32_lineEdit->setInputMask(mask);
    _serial_start_ext32_lineEdit->setSizePolicy(QSizePolicy::Preferred,
                                                QSizePolicy::Preferred);

    _serial_stop_ext32_lineEdit =
        new PopupLineEdit("X X X X X X X X X X X X X X X X", _serial_groupBox);
    _serial_stop_ext32_lineEdit->setObjectName("dock_content");
    _serial_stop_ext32_lineEdit->setFont(contentFont);
    _serial_stop_ext32_lineEdit->setValidator(value_validator);
    _serial_stop_ext32_lineEdit->setMaxLength(TriggerProbes * 2 - 1);
    _serial_stop_ext32_lineEdit->setInputMask(mask);
    _serial_stop_ext32_lineEdit->setSizePolicy(QSizePolicy::Preferred,
                                               QSizePolicy::Preferred);

    _serial_edge_ext32_lineEdit =
        new PopupLineEdit("X X X X X X X X X X X X X X X X", _serial_groupBox);
    _serial_edge_ext32_lineEdit->setObjectName("dock_content");
    _serial_edge_ext32_lineEdit->setFont(contentFont);
    _serial_edge_ext32_lineEdit->setValidator(value_validator);
    _serial_edge_ext32_lineEdit->setMaxLength(TriggerProbes * 2 - 1);
    _serial_edge_ext32_lineEdit->setInputMask(mask);
    _serial_edge_ext32_lineEdit->setSizePolicy(QSizePolicy::Preferred,
                                               QSizePolicy::Preferred);

    connect(_serial_start_ext32_lineEdit, &QLineEdit::editingFinished, this,
            &TriggerDock::value_changed);
    connect(_serial_stop_ext32_lineEdit, &QLineEdit::editingFinished, this,
            &TriggerDock::value_changed);
    connect(_serial_edge_ext32_lineEdit, &QLineEdit::editingFinished, this,
            &TriggerDock::value_changed);

    QLabel *serial0_value_exp_label =
        new QLabel("31 --------- 24 23 ---------- 16", _serial_groupBox);
    serial0_value_exp_label->setObjectName("dock_label");
    serial0_value_exp_label->setFont(labelFont);
    serial_glayout->addWidget(serial0_value_exp_label, row++, 1, 1, 3);
    serial_glayout->addWidget(_serial_start_ext32_lineEdit, row, 1, 1, 3);
    serial_glayout->addWidget(new QLabel(_serial_groupBox), row++, 4);
    QLabel *serial1_value_exp_label =
        new QLabel("15 ---------- 8 7 ----------- 0 ", _serial_groupBox);
    serial1_value_exp_label->setObjectName("dock_label");
    serial1_value_exp_label->setFont(labelFont);
    serial_glayout->addWidget(serial1_value_exp_label, row++, 1, 1, 3);
    serial_glayout->addWidget(_serial_start_label, row, 0);
    serial_glayout->addWidget(_serial_start_lineEdit, row, 1, 1, 3);
    serial_glayout->addWidget(new QLabel(_serial_groupBox), row++, 4);

    serial_glayout->addWidget(new QLabel(_stage_tabWidget), row++, 0);

    QLabel *serial2_value_exp_label =
        new QLabel("31 --------- 24 23 ---------- 16", _serial_groupBox);
    serial2_value_exp_label->setObjectName("dock_label");
    serial2_value_exp_label->setFont(labelFont);
    serial_glayout->addWidget(serial2_value_exp_label, row++, 1, 1, 3);
    serial_glayout->addWidget(_serial_stop_ext32_lineEdit, row++, 1, 1, 3);
    QLabel *serial3_value_exp_label =
        new QLabel("15 ---------- 8 7 ----------- 0 ", _serial_groupBox);
    serial3_value_exp_label->setObjectName("dock_label");
    serial3_value_exp_label->setFont(labelFont);
    serial_glayout->addWidget(serial3_value_exp_label, row++, 1, 1, 3);
    serial_glayout->addWidget(_serial_stop_label, row, 0);
    serial_glayout->addWidget(_serial_stop_lineEdit, row++, 1, 1, 3);

    serial_glayout->addWidget(new QLabel(_stage_tabWidget), row++, 0);

    QLabel *serial4_value_exp_label =
        new QLabel("31 --------- 24 23 ---------- 16", _serial_groupBox);
    serial4_value_exp_label->setObjectName("dock_label");
    serial4_value_exp_label->setFont(labelFont);
    serial_glayout->addWidget(serial4_value_exp_label, row++, 1, 1, 3);
    serial_glayout->addWidget(_serial_edge_ext32_lineEdit, row++, 1, 1, 3);
    QLabel *serial5_value_exp_label =
        new QLabel("15 ---------- 8 7 ----------- 0 ", _serial_groupBox);
    serial5_value_exp_label->setObjectName("dock_label");
    serial5_value_exp_label->setFont(labelFont);
    serial_glayout->addWidget(serial5_value_exp_label, row++, 1, 1, 3);
    serial_glayout->addWidget(_serial_edge_label, row, 0);
    serial_glayout->addWidget(_serial_edge_lineEdit, row++, 1, 1, 3);
  } else {
    QLabel *serial0_value_exp_label =
        new QLabel("15 ---------- 8 7 ----------- 0 ", _serial_groupBox);
    serial0_value_exp_label->setObjectName("dock_label");
    serial0_value_exp_label->setFont(labelFont);
    serial_glayout->addWidget(serial0_value_exp_label, row++, 1, 1, 3);
    serial_glayout->addWidget(_serial_start_label, row, 0);
    serial_glayout->addWidget(_serial_start_lineEdit, row, 1, 1, 3);
    serial_glayout->addWidget(new QLabel(_serial_groupBox), row++, 4);

    serial_glayout->addWidget(new QLabel(_stage_tabWidget), row++, 0);

    QLabel *serial1_value_exp_label =
        new QLabel("15 ---------- 8 7 ----------- 0 ", _serial_groupBox);
    serial1_value_exp_label->setObjectName("dock_label");
    serial1_value_exp_label->setFont(labelFont);
    serial_glayout->addWidget(serial1_value_exp_label, row++, 1, 1, 3);
    serial_glayout->addWidget(_serial_stop_label, row, 0);
    serial_glayout->addWidget(_serial_stop_lineEdit, row++, 1, 1, 3);

    serial_glayout->addWidget(new QLabel(_stage_tabWidget), row++, 0);

    QLabel *serial2_value_exp_label =
        new QLabel("15 ---------- 8 7 ----------- 0 ", _serial_groupBox);
    serial2_value_exp_label->setObjectName("dock_label");
    serial2_value_exp_label->setFont(labelFont);
    serial_glayout->addWidget(serial2_value_exp_label, row++, 1, 1, 3);
    serial_glayout->addWidget(_serial_edge_label, row, 0);
    serial_glayout->addWidget(_serial_edge_lineEdit, row++, 1, 1, 3);
  }

  serial_glayout->addWidget(new QLabel(_serial_groupBox), row++, 0, 1, 5);
  serial_glayout->addWidget(_serial_data_label, row, 0);
  serial_glayout->addWidget(_serial_data_comboBox, row++, 1);
  _data_bits_label = new QLabel(_serial_groupBox);
  _data_bits_label->setObjectName("dock_label");
  serial_glayout->addWidget(_data_bits_label, row, 0);
  serial_glayout->addWidget(_serial_bits_comboBox, row++, 1);
  serial_glayout->addWidget(_serial_value_label, row, 0);
  serial_glayout->addWidget(_serial_value_lineEdit, row++, 1, 1, 3);
  serial_glayout->addWidget(_serial_hex_label, row, 0);
  serial_glayout->addWidget(hex_wid, row++, 1, 1, 3);

  _serial_note_label = new QLabel(_serial_groupBox);
  _serial_note_label->setObjectName("dock_label");
  serial_layout->addLayout(serial_glayout);
  serial_layout->addSpacing(20);
  serial_layout->addWidget(_serial_note_label);
  serial_layout->addStretch(1);

  _serial_groupBox->setLayout(serial_layout);

  connect(_serial_start_lineEdit, &QLineEdit::editingFinished, this,
          &TriggerDock::value_changed);
  connect(_serial_stop_lineEdit, &QLineEdit::editingFinished, this,
          &TriggerDock::value_changed);
  connect(_serial_edge_lineEdit, &QLineEdit::editingFinished, this,
          &TriggerDock::value_changed);
  connect(_serial_value_lineEdit, &QLineEdit::editingFinished, this,
          &TriggerDock::value_changed);

  connect(_serial_value_lineEdit, &QLineEdit::textChanged, this,
          &TriggerDock::on_serial_value_changed);

  connect(_serial_hex_lineEdit, &QLineEdit::editingFinished, this,
          &TriggerDock::on_serial_hex_changed);

  _adv_tabWidget->addTab(
      (QWidget *)_stage_tabWidget,
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STAGE_TRIGGER), "Stage Trigger"));
  _adv_tabWidget->addTab(
      (QWidget *)_serial_groupBox,
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SERIAL_TRIGGER), "Serial Trigger"));

  UpdateFont();
}

void TriggerDock::lineEdit_highlight(PopupLineEdit *dst) {
  if (dst == nullptr)
    return;

  QTextCharFormat fmt;
  fmt.setForeground(view::View::Red);
  QList<QInputMethodEvent::Attribute> attributes;
  for (int i = 0; i < dst->text().size(); i++) {
    if (dst->text().at(i) != 'X' && dst->text().at(i) != ' ')
      attributes.append(QInputMethodEvent::Attribute(
          QInputMethodEvent::TextFormat, i - dst->cursorPosition(), 1, fmt));
  }
  QInputMethodEvent event(QString(), attributes);
  QCoreApplication::sendEvent(dst, &event);
}

void TriggerDock::try_commit_trigger() {
  AppConfig &app = AppConfig::Instance();
  int num = 0;

  int mode = _signals->device()->get_work_mode();
  bool bInstant = _capture->is_instant();

  // ds_trigger_reset() removed: sync_trigger_to_libsigrok() in start_capture
  // is now the single sync point that resets + pushes _trigger_config to
  // ds_trigger_*, eliminating GUI/MCP cross-overwrites.
  if (mode != LOGIC || bInstant) {
    return;
  }

  if (commit_trigger() == false) {
    /* simple trigger check trigger_enable */
    bool has_view = _context && _context->view();

    if (has_view) {
      auto &sigs = _context->view()->get_own_signals();
      for (auto &s : sigs) {
        if (s->signal_type() == SR_CHANNEL_LOGIC) {
          view::LogicSignal *logicSig = (view::LogicSignal *)s.get();
          if (logicSig->commit_trig())
            num++;
        }
      }
    } else {
      auto &sigs = _signals->get_signal_models();
      for (auto s : sigs) {
        if (s->type() == SR_CHANNEL_LOGIC) {
          if (s->commit_trig())
            num++;
        }
      }
    }

    if (app.appOptions.warnofMultiTrig && num > 1) {
      dialogs::DSMessageBox msg(this);
      msg.mBox()->setText(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_TRIGGER), "Trigger"));
      msg.mBox()->setInformativeText(
          L_S(STR_PAGE_MSG, S_ID(IDS_MSG_SET_TRI_MULTI_CHANNEL),
              "Trigger setted on multiple channels!\nCapture will Only "
              "triggered when all setted channels fullfill at one sample"));
      msg.mBox()->setIcon(QMessageBox::Information);

      QPushButton *noMoreButton = msg.mBox()->addButton(
          L_S(STR_PAGE_MSG, S_ID(IDS_MSG_NOT_SHOW_AGAIN), "Not Show Again"),
          QMessageBox::ActionRole);
      QPushButton *cancelButton = msg.mBox()->addButton(
          L_S(STR_PAGE_MSG, S_ID(IDS_MSG_CLEAR_TRIG), "Clear Trig"),
          QMessageBox::ActionRole);
      msg.mBox()->addButton(
          L_S(STR_PAGE_MSG, S_ID(IDS_MSG_CONTINUE), "Continue"),
          QMessageBox::ActionRole);

      msg.exec();

      if (msg.mBox()->clickedButton() == cancelButton) {
        if (has_view) {
          auto &sigs = _context->view()->get_own_signals();
          for (auto &s : sigs) {
            if (s->signal_type() == SR_CHANNEL_LOGIC) {
              view::LogicSignal *logicSig = (view::LogicSignal *)s.get();
              logicSig->set_trig(view::LogicSignal::NONTRIG);
              logicSig->commit_trig();
            }
          }
        } else {
          auto &sigs = _signals->get_signal_models();
          for (auto s : sigs) {
            if (s->type() == SR_CHANNEL_LOGIC) {
              s->set_trig_type(data::SignalModel::NONTRIG);
              s->commit_trig();
            }
          }
        }
      }

      if (msg.mBox()->clickedButton() == noMoreButton) {
        app.appOptions.warnofMultiTrig = false;
      }
    }
  }
}

void TriggerDock::on_hex_checkbox_click(bool ck) {
  _serial_hex_lineEdit->setReadOnly(!ck);
  if (ck) {
    _serial_hex_lineEdit->setFocus();
  }
}

void TriggerDock::on_serial_value_changed(const QString &v) {
  if (_is_serial_val_setting)
    return;

  QString s(v);
  s = s.replace(" ", "").toLower();
  _serial_hex_lineEdit->setText("");

  if (s != "" && s.indexOf("x") == -1) {
    char *buf = s.toLocal8Bit().data();
    int len = s.length();
    unsigned long val = 0;

    if (len == 16) {
      for (int i = 0; i < len; i++) {
        if (*(buf + i) == '1') {
          val += 1 << (len - i - 1);
        }
      }

      char tmp[10];
      sprintf(tmp, "%02lX", val);
      _serial_hex_lineEdit->setText(QString(tmp));
    }
  }
}

void TriggerDock::on_serial_hex_changed() {
  if (_is_serial_val_setting)
    return;

  _is_serial_val_setting = true;

  QString s = _serial_hex_lineEdit->text();
  _serial_hex_lineEdit->setText(s.toUpper());

  if (s.length() <= 4) {
    while (s.length() < 4) {
      s = "0" + s;
    }

    const char *str = s.toLocal8Bit().data();
    char *endptr = nullptr;
    unsigned long val = strtoul(str, &endptr, 16);
    char buffer[18];
    AnnotationResTable::decimalToBinString(val, 16, buffer, sizeof(buffer));
    _serial_value_lineEdit->setText(QString(buffer));
  }

  _is_serial_val_setting = false;
}

void TriggerDock::UpdateLanguage() { retranslateUi(); }

void TriggerDock::UpdateTheme() { reStyle(); }

void TriggerDock::UpdateFont() {
  ui::set_dock_form_font(this);
  QFont labelFont = dock_font_label();
  this->parentWidget()->setFont(labelFont);

  _adv_tabWidget->setFont(labelFont);
  _adv_tabWidget->widget(0)->setFont(labelFont);
  _adv_tabWidget->widget(1)->setFont(labelFont);
  _adv_tabWidget->tabBar()->setFont(labelFont);

  QFontMetrics fm(labelFont);

  auto edits = this->findChildren<PopupLineEdit *>();
  int lineH = 30;

  for (auto o : edits) {
    if (o != _serial_hex_lineEdit && !o->text().isEmpty()) {
      QRect rc = fm.boundingRect(o->text());
      if (!rc.isValid())
        continue;
      QSize size(rc.width() + 20, rc.height() + 6);
      o->setMinimumSize(size);
      lineH = size.height();
    }
  }

  _serial_hex_lineEdit->setFixedHeight(lineH);

  int lines = 3 * 2;
  if (_cur_ch_num == 32) {
    lines = 6 * 2;
  }

  int pageHeight = (lineH + 15) * lines;
  pageHeight += lineH * 10;
  pageHeight += 250;

  _serial_groupBox->setFixedHeight(pageHeight);
}

void TriggerDock::bind_context(TabContext *ctx) {
  if (!ctx) {
    pxv_warn("%s", "TriggerDock::bind_context: ctx is nullptr");
    return;
  }
  assert(ctx);
  _context = ctx;
  _session = ctx->session();
      _signals = _session;
  _capture = _session;
  if (ctx && ctx->view()) {
    auto &saved = ctx->view()->dock_ui_state().dock_trigger_session;
    if (!saved.isEmpty()) {
      set_session(saved);
    }
  }
}

void TriggerDock::unbind_context() {
  if (_context && _context->view()) {
    _context->view()->dock_ui_state().dock_trigger_session = get_session();
  }
  _context = nullptr;
}

} // namespace dock
} // namespace pv
