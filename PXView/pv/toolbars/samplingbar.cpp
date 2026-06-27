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

#include "samplingbar.h"
#include "../config/appconfig.h"
#include "../data/sessiondocument.h"
#include "../deviceagent.h"
#include "../dialogs/deviceoptions.h"
#include "../dialogs/dsmessagebox.h"
#include "../dialogs/interval.h"
#include "../dialogs/waitingdialog.h"
#include "../dsvdef.h"
#include "../log.h"
#include "../tabcontext.h"
#include "../ui/dockfonts.h"
#include "../ui/fn.h"
#include "../ui/iconcache.h"
#include "../ui/langresource.h"
#include "../ui/msgbox.h"
#include "../view/dsosignal.h"
#include "../view/view.h"
#include <QAbstractItemView>
#include <QAction>
#include <QLabel>
#include <assert.h>
#include <libusb-1.0/libusb.h>
#include <math.h>

#include <QButtonGroup>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QRadioButton>
#include <QSpacerItem>
#include <QWidgetAction>

#define SINGLE_ACTION_ICON "/once.svg"
#define REPEAT_ACTION_ICON "/repeat.svg"
#define LOOP_ACTION_ICON "/loop.svg"

using std::map;
using std::max;
using std::min;
using std::string;

namespace pv {
namespace toolbars {

const QString SamplingBar::RLEString = "(RLE)";
const QString SamplingBar::DIVString = " / div";

SamplingBar::SamplingBar(SigSession *session, QWidget *parent)
    : QToolBar("Sampling Bar", parent) {
  _device_type = new XToolButton(this);
  _device_selector = new DsComboBox(this);
  _sample_count = new DsComboBox(this);
  _sample_rate = new DsComboBox(this);
  _mode_button = new XToolButton(this);
  _updating_device_list = false;
  _updating_sample_rate = false;
  _updating_sample_count = false;
  _is_run_as_instant = false;
  _is_readonly = false;
  _context = nullptr;

  _last_device_handle = NULL_HANDLE;
  _last_device_index = -1;
  _next_switch_device = NULL_HANDLE;
  _view = NULL;
  _mode_group = nullptr;
  _radio_single = nullptr;
  _radio_repeat = nullptr;
  _radio_loop = nullptr;

  _session = session;
  _device_agent = _session->get_device();

  setMovable(false);
  setContentsMargins(0, 0, 0, 0);
  layout()->setSpacing(0);

  _mode_button->setPopupMode(QToolButton::InstantPopup);

  _device_selector->setSizeAdjustPolicy(
      DsComboBox::AdjustToMinimumContentsLengthWithIcon);
  _sample_rate->setSizeAdjustPolicy(
      DsComboBox::AdjustToMinimumContentsLengthWithIcon);
  _sample_count->setSizeAdjustPolicy(
      DsComboBox::AdjustToMinimumContentsLengthWithIcon);
  _device_selector->setMinimumContentsLength(15);
  _sample_rate->setMinimumContentsLength(15);
  _sample_count->setMinimumContentsLength(15);
  _device_selector->setMaximumWidth(ComboBoxMaxWidth);

  QWidget *leftMargin = new QWidget(this);
  leftMargin->setFixedWidth(4);
  addWidget(leftMargin);

  // _device_type->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  _device_type->setToolButtonStyle(Qt::ToolButtonIconOnly);
  addWidget(_device_type);
  addWidget(new QLabel("  "));
  _device_type_label = new QLabel(this);
  addWidget(_device_type_label);
  addWidget(new QLabel("  "));
  addWidget(_device_selector);

  addWidget(_sample_count);
  // tr
  //  addWidget(new QLabel(" @ "));
  addWidget(new QLabel("  "));
  addWidget(_sample_rate);

  _action_single = new QAction(this);
  _action_repeat = new QAction(this);
  _action_loop = new QAction(this);

  _mode_menu = new QMenu(this);
  _mode_menu->addAction(_action_single);
  _mode_menu->addAction(_action_repeat);
  _mode_menu->addAction(_action_loop);
  _mode_button->setMenu(_mode_menu);

  auto widgetToAction = [](QWidget *widget,
                           QWidget *parent = nullptr) -> QAction * {
    QWidgetAction *action = new QWidgetAction(parent);
    action->setDefaultWidget(widget);
    return action;
  };

  _mode_button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  _mode_action = widgetToAction(_mode_button);

  update_view_status();

  connect(_device_selector,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &SamplingBar::on_device_selected);
  connect(_sample_count, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &SamplingBar::on_samplecount_sel);
  connect(_action_single, &QAction::triggered, this,
          &SamplingBar::on_collect_mode);
  connect(_action_repeat, &QAction::triggered, this,
          &SamplingBar::on_collect_mode);
  connect(_action_loop, &QAction::triggered, this,
          &SamplingBar::on_collect_mode);
  connect(_sample_rate, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &SamplingBar::on_samplerate_sel);

  ADD_UI(this);
}

SamplingBar::~SamplingBar() { REMOVE_UI(this); }

QWidget *SamplingBar::createSamplingSettingsWidget(QWidget *parent) {
  QWidget *group = new QWidget(parent);
  QVBoxLayout *vbox = new QVBoxLayout(group);
  vbox->setContentsMargins(0, 0, 0, 0);
  vbox->setSpacing(0);

  _settings_title_label = new QLabel(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_SAMPLING_SETTINGS), "采样设置"),
      group);
  _settings_title_label->setObjectName("dock_section_title");
  vbox->addWidget(_settings_title_label);

  QWidget *inner = new QWidget(group);
  QGridLayout *grid = new QGridLayout(inner);
  int target_w = 200;

  // 设置为 3 列布局
  grid->setColumnStretch(0, 0);             // 第0列：文字标签
  grid->setColumnStretch(1, 0);             // 第1列：USB图标（紧凑）
  grid->setColumnStretch(2, 1);             // 第2列：下拉框（拉伸填满）
  grid->setColumnMinimumWidth(2, target_w); // 仅约束下拉框列的最小宽度

  QFont sectionFont = dock_font_section_title();
  QFont labelFont = dock_font_label();
  QFont contentFont = dock_font_content();
  _settings_title_label->setFont(sectionFont);
  _settings_title_label->setProperty("cssClass", "SectionTitleText");

  // Row 0: 设备
  _dev_label = new QLabel(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_DEVICE), "设备"), inner);
  _dev_label->setFont(labelFont);
  _dev_label->setProperty("cssClass", "LabelText");
  _dev_label->setObjectName("dock_label");
  grid->addWidget(_dev_label, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);

  // 拆除原来的 QHBoxLayout，图标放第1列，下拉框放第2列
  grid->addWidget(_device_type, 0, 1, Qt::AlignCenter);

  _device_selector->setMinimumWidth(target_w);
  _device_selector->setMaximumWidth(QWIDGETSIZE_MAX);
  _device_selector->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  _device_selector->setFont(contentFont);
  _device_selector->setObjectName("dock_content");
  grid->addWidget(_device_selector, 0, 2);

  // Row 1: 采样深度
  _depth_label = new QLabel(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_SAMPLE_DEPTH), "采样深度"), inner);
  _depth_label->setFont(labelFont);
  _depth_label->setProperty("cssClass", "LabelText");
  _depth_label->setObjectName("dock_label");
  grid->addWidget(_depth_label, 1, 0, Qt::AlignLeft | Qt::AlignVCenter);

  _sample_count->setMinimumWidth(target_w);
  _sample_count->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  _sample_count->setFont(contentFont);
  _sample_count->setObjectName("dock_content");
  grid->addWidget(_sample_count, 1, 2); // 注意这里放在第2列

  // Row 2: 采样率
  _rate_label = new QLabel(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_SAMPLE_RATE), "采样率"), inner);
  _rate_label->setFont(labelFont);
  _rate_label->setProperty("cssClass", "LabelText");
  _rate_label->setObjectName("dock_label");
  grid->addWidget(_rate_label, 2, 0, Qt::AlignLeft | Qt::AlignVCenter);

  _sample_rate->setMinimumWidth(target_w);
  _sample_rate->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  _sample_rate->setFont(contentFont);
  _sample_rate->setObjectName("dock_content");
  grid->addWidget(_sample_rate, 2, 2); // 注意这里放在第2列

  // Row 3: 捕获模式
  _mode_label = new QLabel(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_ROW), "捕获模式"),
      inner);
  _mode_label->setFont(labelFont);
  _mode_label->setProperty("cssClass", "LabelText");
  _mode_label->setObjectName("mode_label");
  grid->addWidget(_mode_label, 3, 0, Qt::AlignLeft | Qt::AlignVCenter);

  _mode_group = new QButtonGroup(inner);
  _radio_single = new QRadioButton(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_SINGLE), "单次"),
      inner);
  _radio_repeat = new QRadioButton(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_REPEAT), "重复"),
      inner);
  _radio_loop = new QRadioButton(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_LOOP), "循环"),
      inner);
  _radio_single->setFont(contentFont);
  _radio_single->setObjectName("dock_content");
  _radio_repeat->setFont(contentFont);
  _radio_repeat->setObjectName("dock_content");
  _radio_loop->setFont(contentFont);
  _radio_loop->setObjectName("dock_content");
  _mode_group->addButton(_radio_single, COLLECT_SINGLE);
  _mode_group->addButton(_radio_repeat, COLLECT_REPEAT);
  _mode_group->addButton(_radio_loop, COLLECT_LOOP);

  QHBoxLayout *modeRow = new QHBoxLayout();
  modeRow->setSpacing(4);
  modeRow->setContentsMargins(0, 0, 0, 0);
  modeRow->addStretch();
  modeRow->addWidget(_radio_single);
  modeRow->addWidget(_radio_repeat);
  modeRow->addWidget(_radio_loop);

  // 底部单选框，跨两列（第1列和第2列），靠右对齐
  grid->addLayout(modeRow, 3, 1, 1, 2, Qt::AlignRight | Qt::AlignVCenter);

  connect(_mode_group, &QButtonGroup::idClicked, this,
          &SamplingBar::on_mode_radio_clicked);

  vbox->addWidget(inner);

  // 控件从 QToolBar 移出时 QWidgetAction::releaseWidget() 会自动 hide()，
  // 需要显式 show() 恢复可见性
  _device_type->show();
  _device_selector->show();
  _sample_count->show();
  _sample_rate->show();

  return group;
}

void SamplingBar::bind_context(TabContext *ctx) {
  assert(ctx);
  _context = ctx;
  _session = ctx->session();
  _view = ctx->view();
  _device_agent = _session->get_device();
  set_readonly(!ctx->is_live());
  if (_device_agent && _device_agent->have_instance()) {
    update_device_list();
    auto doc = ctx->document();
    if (doc && doc->_dock_sample_rate > 0) {
      _device_agent->set_config_uint64(SR_CONF_SAMPLERATE,
                                       doc->_dock_sample_rate);
      _device_agent->set_config_uint64(SR_CONF_LIMIT_SAMPLES,
                                       doc->_dock_sample_limit);
      _session->set_collect_mode((DEVICE_COLLECT_MODE)doc->_dock_collect_mode);
    }

    update_sample_rate_selector();

    if (doc && doc->_dock_sample_rate > 0) {
      for (int i = _sample_rate->count() - 1; i >= 0; i--) {
        if (doc->_dock_sample_rate >=
            _sample_rate->itemData(i).value<uint64_t>()) {
          _sample_rate->setCurrentIndex(i);
          break;
        }
      }
    }

    if (doc && doc->_dock_sample_limit > 0 && doc->_dock_sample_rate > 0) {
      double duration =
          (double)doc->_dock_sample_limit / doc->_dock_sample_rate * SR_SEC(1);
      for (int i = 0; i < _sample_count->count(); i++) {
        if (duration >= _sample_count->itemData(i).value<double>()) {
          _sample_count->setCurrentIndex(i);
          break;
        }
      }
    }

    update_sample_rate_selector_value();
    update_sample_count_selector_value();
    reload();
    if (_device_selector->parentWidget()) {
      _device_selector->parentWidget()->adjustSize();
      if (_device_selector->parentWidget()->parentWidget())
        _device_selector->parentWidget()->parentWidget()->adjustSize();
    }
  }
}

void SamplingBar::unbind_context() {
  if (_context && _context->document() && _device_agent && _session &&
      _device_agent->have_instance()) {
    auto doc = _context->document();

    if (_sample_rate->count() > 0 && _sample_rate->currentIndex() >= 0) {
      doc->_dock_sample_rate =
          _sample_rate->itemData(_sample_rate->currentIndex()).value<uint64_t>();
    } else {
      doc->_dock_sample_rate = _device_agent->get_sample_rate();
    }

    if (_sample_count->count() > 0 && _sample_count->currentIndex() >= 0) {
      double duration =
          _sample_count->itemData(_sample_count->currentIndex()).value<double>();
      uint64_t s_rate = doc->_dock_sample_rate > 0
                            ? doc->_dock_sample_rate
                            : _device_agent->get_sample_rate();
      if (s_rate > 0) {
        doc->_dock_sample_limit =
            ((uint64_t)ceil(duration / SR_SEC(1) * s_rate) + SAMPLES_ALIGN) &
            ~SAMPLES_ALIGN;
      } else {
        doc->_dock_sample_limit = _device_agent->get_sample_limit();
      }
    } else {
      doc->_dock_sample_limit = _device_agent->get_sample_limit();
    }

    doc->_dock_collect_mode = (int)_session->get_collect_mode();
  }
  _context = nullptr;
  set_readonly(false);
}

void SamplingBar::retranslateUi() {
  bool bDev = _device_agent->have_instance();

  if (bDev) {
    if (_device_agent->is_demo()) {
      _device_type_label->setText(
          L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_DEVICE_TYPE_DEMO), "Demo"));
    } else if (_device_agent->is_file()) {
      _device_type_label->setText(
          L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_DEVICE_TYPE_FILE), "File"));
    } else {
      int usb_speed = LIBUSB_SPEED_HIGH;
      _device_agent->get_config_int32(SR_CONF_USB_SPEED, usb_speed);

      if (usb_speed == LIBUSB_SPEED_HIGH) {
        _device_type_label->setText("USB 2.0");
        _device_type->setToolTip(tr("USB 2.0 (High Speed)\nMax bandwidth: ~40 MB/s\nStream mode: lower sample rates\nBuffer mode: full sample rates"));
      } else if (usb_speed == LIBUSB_SPEED_SUPER) {
        _device_type_label->setText("USB 3.0");
        _device_type->setToolTip(tr("USB 3.0 (SuperSpeed)\nMax bandwidth: ~400 MB/s\nStream mode: higher sample rates available"));
      } else {
        _device_type_label->setText("USB UNKNOWN");
        _device_type->setToolTip(tr("USB speed unknown"));
      }
    }
  }
  _mode_button->setText(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE), "Mode"));

  _action_single->setText(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_SINGLE), "&Single"));
  _action_repeat->setText(L_S(
      STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_REPEAT), "&Repetitive"));
  _action_loop->setText(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_LOOP), "&Loop"));

  if (_settings_title_label) {
      _settings_title_label->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_SAMPLING_SETTINGS), "采样设置"));
      _dev_label->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_DEVICE), "设备"));
      _depth_label->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_SAMPLE_DEPTH), "采样深度"));
      _rate_label->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_SAMPLE_RATE), "采样率"));
      _mode_label->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_ROW), "捕获模式"));
      _radio_single->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_SINGLE), "单次"));
      _radio_repeat->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_REPEAT), "重复"));
      _radio_loop->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_LOOP), "循环"));
  }
}

void SamplingBar::reStyle() {
  bool bDev = _device_agent->have_instance();

  if (bDev) {
    if (_device_agent->is_demo())
      _device_type->setIcon(IconCache::Instance().icon(":/icons/demo.svg"));
    else if (_device_agent->is_file())
      _device_type->setIcon(IconCache::Instance().icon(":/icons/data.svg"));
    else {
      int usb_speed = LIBUSB_SPEED_HIGH;
      _device_agent->get_config_int32(SR_CONF_USB_SPEED, usb_speed);

      if (usb_speed == LIBUSB_SPEED_SUPER)
        _device_type->setIcon(IconCache::Instance().icon(":/icons/usb3.svg"));
      else
        _device_type->setIcon(IconCache::Instance().icon(":/icons/usb2.svg"));
    }
  }

  if (true) {
    QString iconPath = GetIconPath();
    QColor iconColor = AppConfig::Instance().GetThemeColor("@titlebar-icon-accent");

    auto getIcon = [&](const QString &name) {
        return iconColor.isValid() ? IconCache::Instance().tintedIcon(iconPath + name, iconColor)
                                   : IconCache::Instance().icon(iconPath + name);
    };

    _action_single->setIcon(getIcon(SINGLE_ACTION_ICON));
    _action_repeat->setIcon(getIcon(REPEAT_ACTION_ICON));
    _action_loop->setIcon(getIcon(LOOP_ACTION_ICON));

    update_mode_icon();
  }
}

void SamplingBar::zero_adj() {
  for (auto s : _session->get_signals()) {
    if (s->signal_type() == SR_CHANNEL_DSO) {
      view::DsoSignal *dsoSig = (view::DsoSignal *)s;
      dsoSig->set_enable(true);
    }
  }

  const int index_back = _sample_count->currentIndex();
  int i = 0;

  for (i = 0; i < _sample_count->count(); i++) {
    if (_sample_count->itemData(i).value<uint64_t>() == ZeroTimeBase)
      break;
  }

  set_sample_count_index(i);
  commit_hori_res();

  if (_session->is_working() == false)
    _session->start_capture(false);

  pv::dialogs::WaitingDialog wait(this, _session, SR_CONF_ZERO);
  if (wait.start() == QDialog::Rejected) {
    for (auto s : _session->get_signals()) {
      if (s->signal_type() == SR_CHANNEL_DSO) {
        view::DsoSignal *dsoSig = (view::DsoSignal *)s;
        dsoSig->commit_settings();
      }
    }
  }

  if (_session->is_working())
    _session->stop_capture();

  set_sample_count_index(index_back);
  commit_hori_res();
}

void SamplingBar::set_sample_rate(uint64_t sample_rate) {
  for (int i = _sample_rate->count() - 1; i >= 0; i--) {
    uint64_t cur_index_sample_rate = _sample_rate->itemData(i).value<uint64_t>();
    if (sample_rate >= cur_index_sample_rate) {
      _sample_rate->setCurrentIndex(i);
      break;
    }
  }
  commit_settings();
}

void SamplingBar::update_sample_rate_selector() {
  GVariant *gvar_dict, *gvar_list;
  const uint64_t *elements = NULL;
  gsize num_elements;

  pxv_info("Update rate list.");

  if (_updating_sample_rate) {
    pxv_err("Error! The rate list is updating.");
    return;
  }

  disconnect(_sample_rate, QOverload<int>::of(&QComboBox::currentIndexChanged),
             this, &SamplingBar::on_samplerate_sel);

  if (_device_agent->have_instance() == false) {
    pxv_info("SamplingBar::update_sample_rate_selector, have no device.");
    return;
  }

  _updating_sample_rate = true;

  gvar_dict = _device_agent->get_config_list(NULL, SR_CONF_SAMPLERATE);
  if (gvar_dict == NULL) {
    _sample_rate->clear();
    _sample_rate->show();
    _updating_sample_rate = false;
    return;
  }

  if ((gvar_list = g_variant_lookup_value(gvar_dict, "samplerates",
                                          G_VARIANT_TYPE("at")))) {
    elements = (const uint64_t *)g_variant_get_fixed_array(
        gvar_list, &num_elements, sizeof(uint64_t));
    _sample_rate->clear();

    for (unsigned int i = 0; i < num_elements; i++) {
      char *const s = sr_samplerate_string(elements[i]);
      _sample_rate->addItem(QString(s), QVariant::fromValue(elements[i]));
      g_free(s);
    }

    _sample_rate->show();
    g_variant_unref(gvar_list);
  }

  // _sample_rate->setMinimumWidth(_sample_rate->sizeHint().width() + 15);
  _sample_rate->view()->setMinimumWidth(_sample_rate->sizeHint().width() + 30);

  _updating_sample_rate = false;
  g_variant_unref(gvar_dict);

  update_sample_rate_selector_value();

  connect(_sample_rate, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &SamplingBar::on_samplerate_sel);

  update_sample_count_selector();
}

void SamplingBar::update_sample_rate_selector_value() {
  if (_updating_sample_rate)
    return;
  _updating_sample_rate = true;

  const uint64_t samplerate = _device_agent->get_sample_rate();
  uint64_t cur_value =
      _sample_rate->itemData(_sample_rate->currentIndex()).value<uint64_t>();

  if (samplerate != cur_value) {
    for (int i = _sample_rate->count() - 1; i >= 0; i--) {
      if (samplerate >= _sample_rate->itemData(i).value<uint64_t>()) {
        _sample_rate->setCurrentIndex(i);
        break;
      }
    }
  }

  _updating_sample_rate = false;
}

void SamplingBar::on_samplerate_sel(int index) {
  (void)index;
  if (_device_agent->get_work_mode() != DSO)
    update_sample_count_selector();

  if (_context && _context->document()) {
    _context->document()->_dock_sample_rate = _device_agent->get_sample_rate();
  }
}

void SamplingBar::update_sample_count_selector() {
  bool stream_mode = false;
  uint64_t hw_depth = 0;
  uint64_t sw_depth;
  uint64_t rle_depth = 0;
  uint64_t max_timebase = 0;
  uint64_t min_timebase = SR_NS(10);
  double pre_duration = SR_SEC(1);
  double duration;
  bool rle_support = false;

  pxv_info("Update sample count list.");

  if (_updating_sample_count) {
    pxv_err("Error! The sample count is updating.");
    return;
  }

  disconnect(_sample_count,
             QOverload<int>::of(&QComboBox::currentIndexChanged), this,
             &SamplingBar::on_samplecount_sel);

  assert(!_updating_sample_count);
  _updating_sample_count = true;

  _device_agent->get_config_bool(SR_CONF_STREAM, stream_mode);
  _device_agent->get_config_uint64(SR_CONF_HW_DEPTH, hw_depth);
  int mode = _device_agent->get_work_mode();

  if (mode == LOGIC) {
#if defined(__x86_64__) || defined(_M_X64)
    sw_depth = LogicMaxSWDepth64;
#elif defined(__i386) || defined(_M_IX86)
    int ch_num = _session->get_ch_num(SR_CHANNEL_LOGIC);
    if (ch_num <= 0)
      sw_depth = LogicMaxSWDepth32;
    else
      sw_depth = LogicMaxSWDepth32 / ch_num;
#endif
  } else {
    sw_depth = AnalogMaxSWDepth;
  }

  if (mode == LOGIC) {
    _device_agent->get_config_bool(SR_CONF_RLE_SUPPORT, rle_support);
    if (rle_support)
      rle_depth = min(hw_depth * SR_KB(1), sw_depth);
  } else if (mode == DSO) {
    _device_agent->get_config_uint64(SR_CONF_MAX_TIMEBASE, max_timebase);
    _device_agent->get_config_uint64(SR_CONF_MIN_TIMEBASE, min_timebase);
  }

  if (0 != _sample_count->count())
    pre_duration =
        _sample_count->itemData(_sample_count->currentIndex()).value<double>();
  _sample_count->clear();
  const uint64_t samplerate =
      _sample_rate->itemData(_sample_rate->currentIndex()).value<uint64_t>();
  const double hw_duration = (samplerate > 0) ? (hw_depth / (samplerate * (1.0 / SR_SEC(1)))) : 0;

  if (mode == DSO)
    duration = max_timebase;
  else if (stream_mode) {
    // Stream mode: data flows continuously via mmap, not limited by hardware FIFO.
    // mmap is backed by either memory (no disk cache) or disk file (with cache).
    // The two modes are mutually exclusive — not additive.
    // - No disk cache: use SR_CONF_STREAM_MEM_BUFF (memory mmap size)
    // - Disk cache:    use SR_CONF_STREAM_BUFF (disk mmap size)
    int ch_num = _session->get_ch_num(SR_CHANNEL_LOGIC);
    if (ch_num <= 0)
      ch_num = 1;
    bool disk_cache_enabled = false;
    _device_agent->get_config_bool(SR_CONF_DISK_CACHE_ENABLE,
                                   disk_cache_enabled);

    double buff_gb = 16.0;
    if (disk_cache_enabled) {
      _device_agent->get_config_double(SR_CONF_STREAM_BUFF, buff_gb);
    } else {
      _device_agent->get_config_double(SR_CONF_STREAM_MEM_BUFF, buff_gb);
    }
    uint64_t total_samples = (uint64_t)(buff_gb * SR_GB(1)) * 8 / ch_num;
    duration = total_samples / (samplerate * (1.0 / SR_SEC(1)));
  } else if (rle_support)
    duration = rle_depth / (samplerate * (1.0 / SR_SEC(1)));
  else
    duration = hw_duration;

  assert(duration > 0);
  bool not_last = true;

  do {
    QString suffix = (mode == DSO)                              ? DIVString
                     : (!stream_mode && duration > hw_duration) ? RLEString
                                                                : "";
    char *const s = sr_time_string(duration);
    _sample_count->addItem(QString(s) + suffix, QVariant::fromValue(duration));
    g_free(s);

    double unit;
    if (duration >= SR_DAY(1))
      unit = SR_DAY(1);
    else if (duration >= SR_HOUR(1))
      unit = SR_HOUR(1);
    else if (duration >= SR_MIN(1))
      unit = SR_MIN(1);
    else
      unit = 1;

    const double log10_duration = pow(10, floor(log10(duration / unit)));

    if (duration > 5 * log10_duration * unit)
      duration = 5 * log10_duration * unit;
    else if (duration > 2 * log10_duration * unit)
      duration = 2 * log10_duration * unit;
    else if (duration > log10_duration * unit)
      duration = log10_duration * unit;
    else
      duration = log10_duration > 1 ? duration * 0.5
                                    : (unit == SR_DAY(1)    ? SR_HOUR(20)
                                       : unit == SR_HOUR(1) ? SR_MIN(50)
                                       : unit == SR_MIN(1)  ? SR_SEC(50)
                                                            : duration * 0.5);

    if (mode == DSO)
      not_last = duration >= min_timebase;
    else if (mode == ANALOG)
      not_last = (duration >= SR_MS(200)) &&
                 (duration / SR_SEC(1) * samplerate >= SR_KB(1));
    else
      not_last = (duration / SR_SEC(1) * samplerate >= SR_KB(1));

  } while (not_last);

  _sample_count->view()->setMinimumWidth(_sample_count->sizeHint().width() + 30);

  _updating_sample_count = true;

  if (pre_duration > _sample_count->itemData(0).value<double>()) {
    set_sample_count_index(0);
  } else if (pre_duration < _sample_count->itemData(_sample_count->count() - 1)
                                .value<double>()) {
    set_sample_count_index(_sample_count->count() - 1);
  } else {
    for (int i = 0; i < _sample_count->count(); i++) {
      double sel_val = _sample_count->itemData(i).value<double>();
      if (pre_duration >= sel_val) {
        set_sample_count_index(i);
        break;
      }
    }
  }
  _updating_sample_count = false;

  update_sample_count_selector_value();
  on_samplecount_sel(_sample_count->currentIndex());

  connect(_sample_count, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &SamplingBar::on_samplecount_sel);
}

void SamplingBar::update_sample_count_selector_value() {
  if (_updating_sample_count)
    return;

  double duration;
  uint64_t v;

  if (_device_agent->get_work_mode() == DSO) {
    if (_device_agent->get_config_uint64(SR_CONF_TIMEBASE, v)) {
      duration = (double)v;
    } else {
      pxv_err("ERROR: config_get SR_CONF_TIMEBASE failed.");
      return;
    }
  } else {
    if (_device_agent->get_config_uint64(SR_CONF_LIMIT_SAMPLES, v)) {
      duration = (double)v;
    } else {
      pxv_err("ERROR: config_get SR_CONF_TIMEBASE failed.");
      return;
    }
    const uint64_t samplerate = _device_agent->get_sample_rate();
    duration = duration / samplerate * SR_SEC(1);
  }
  assert(!_updating_sample_count);
  _updating_sample_count = true;

  double cur_duration =
      _sample_count->itemData(_sample_count->currentIndex()).value<double>();
  if (duration != cur_duration) {
    for (int i = 0; i < _sample_count->count(); i++) {
      double sel_val = _sample_count->itemData(i).value<double>();
      if (duration >= sel_val) {
        set_sample_count_index(i);
        break;
      }
    }
  }

  _updating_sample_count = false;
}

void SamplingBar::apply_sample_count(double &hori_res) {
  hori_res = -1;

  if (_device_agent->get_work_mode() == DSO) {
    hori_res = commit_hori_res();

    if (_session->have_view_data() == false) {
      _session->apply_samplerate();
    }
  }

  _session->broadcast_msg(DSV_MSG_DEVICE_DURATION_UPDATED);
}

void SamplingBar::on_samplecount_sel(int index) {
  (void)index;

  double hori_res = -1;
  apply_sample_count(hori_res);

  if (_context && _context->document()) {
    _context->document()->_dock_sample_limit =
        _device_agent->get_sample_limit();
  }
}

double SamplingBar::get_hori_res() {
  return _sample_count->itemData(_sample_count->currentIndex()).value<double>();
}

double SamplingBar::hori_knob(int dir) {
  double hori_res = -1;

  if (_session->get_device()->get_work_mode() != DSO) {
    assert(false);
  }

  disconnect(_sample_count,
             QOverload<int>::of(&QComboBox::currentIndexChanged), this,
             &SamplingBar::on_samplecount_sel);

  if (0 == dir) {
    hori_res = commit_hori_res();
  } else if ((dir > 0) && (_sample_count->currentIndex() > 0)) {
    set_sample_count_index(_sample_count->currentIndex() - 1);
    hori_res = commit_hori_res();

    if (_session->have_view_data() == false) {
      _session->apply_samplerate();
      _session->broadcast_msg(DSV_MSG_DEVICE_DURATION_UPDATED);
    }
  } else if ((dir < 0) &&
             (_sample_count->currentIndex() < _sample_count->count() - 1)) {
    set_sample_count_index(_sample_count->currentIndex() + 1);
    hori_res = commit_hori_res();

    if (_session->have_view_data() == false) {
      _session->apply_samplerate();
      _session->broadcast_msg(DSV_MSG_DEVICE_DURATION_UPDATED);
    }
  }

  connect(_sample_count, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &SamplingBar::on_samplecount_sel);

  return hori_res;
}

double SamplingBar::commit_hori_res() {
  const double hori_res =
      _sample_count->itemData(_sample_count->currentIndex()).value<double>();

  const uint64_t sample_limit = _device_agent->get_sample_limit();
  uint64_t max_sample_rate;

  if (_device_agent->get_config_uint64(SR_CONF_MAX_DSO_SAMPLERATE,
                                       max_sample_rate) == false) {
    pxv_err("ERROR: config_get SR_CONF_MAX_DSO_SAMPLERATE failed.");
    return -1;
  }

  const uint64_t sample_rate = min(
      (uint64_t)(sample_limit * SR_SEC(1) / (hori_res * DS_CONF_DSO_HDIVS)),
      (uint64_t)(max_sample_rate / (_session->get_ch_num(SR_CHANNEL_DSO)
                                        ? _session->get_ch_num(SR_CHANNEL_DSO)
                                        : 1)));
  set_sample_rate(sample_rate);

  _device_agent->set_config_uint64(SR_CONF_TIMEBASE, hori_res);

  return hori_res;
}

void SamplingBar::commit_settings() {
  bool test = false;
  if (_device_agent->have_instance()) {
    _device_agent->get_config_bool(SR_CONF_TEST, test);
  }

  if (test) {
    update_sample_rate_selector_value();
    update_sample_count_selector_value();
  } else {
    const double sample_duration =
        _sample_count->itemData(_sample_count->currentIndex()).value<double>();
    const uint64_t sample_rate =
        _sample_rate->itemData(_sample_rate->currentIndex()).value<uint64_t>();

    if (_device_agent->have_instance()) {
      if (sample_rate != _device_agent->get_sample_rate())
        _device_agent->set_config_uint64(SR_CONF_SAMPLERATE, sample_rate);

      if (_device_agent->get_work_mode() != DSO) {
        const uint64_t sample_count =
            ((uint64_t)ceil(sample_duration / SR_SEC(1) * sample_rate) +
             SAMPLES_ALIGN) &
            ~SAMPLES_ALIGN;
        if (sample_count != _device_agent->get_sample_limit())
          _device_agent->set_config_uint64(SR_CONF_LIMIT_SAMPLES, sample_count);

        bool rle_mode = _sample_count->currentText().contains(RLEString);
        _device_agent->set_config_bool(SR_CONF_RLE, rle_mode);
      }
    }
  }
}

void SamplingBar::on_run_stop() {
  QTimer::singleShot(10, this, &SamplingBar::on_run_stop_action);
}

void SamplingBar::on_run_stop_action() { action_run_stop(); }

// start or stop capture
bool SamplingBar::action_run_stop() {
  if (_is_readonly)
    return false;

  if (_session->is_doing_action()) {
    pxv_info("Task is busy.");
    return false;
  }

  if (_session->is_working()) {
    return _session->stop_capture();
  }

  if (_device_agent->have_instance() == false) {
    pxv_info("Have no device, can't to collect data.");
    return false;
  }

  commit_settings();

  if (_device_agent->get_work_mode() == DSO) {
    bool zero;

    bool ret = _device_agent->get_config_bool(SR_CONF_ZERO, zero);
    if (ret && zero) {
      QString str1(
          L_S(STR_PAGE_MSG, S_ID(IDS_MSG_AUTO_CALIB), "Auto Calibration"));
      QString str2(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_ADJUST_SAVE),
                       "Please adjust zero skew and save the result"));
      bool bRet = MsgBox::Confirm(str1, str2);

      if (bRet) {
        zero_adj();
      } else {
        _device_agent->set_config_bool(SR_CONF_ZERO, false);
        update_view_status();
      }
      return false;
    }
  }

  if (_device_agent->get_work_mode() == LOGIC && _view != NULL) {
    if (_session->is_realtime_refresh())
      _view->auto_set_max_scale();
  }

  _is_run_as_instant = false;
  bool ret = _session->start_capture(false);

  return ret;
}

void SamplingBar::on_instant_stop() {
  QTimer::singleShot(10, this, &SamplingBar::on_instant_stop_action);
}

void SamplingBar::on_instant_stop_action() { action_instant_stop(); }

bool SamplingBar::action_instant_stop() {
  if (_is_readonly)
    return false;

  if (_session->is_doing_action()) {
    pxv_info("Task is busy.");
    return false;
  }

  if (_session->is_working()) {
    return _session->stop_capture();
  }

  if (_device_agent->have_instance() == false) {
    pxv_info("Error! Have no device, can't to collect data.");
    return false;
  }

  commit_settings();

  if (_device_agent->get_work_mode() == DSO) {
    bool zero;

    bool ret = _device_agent->get_config_bool(SR_CONF_ZERO, zero);
    if (ret && zero) {
      QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_AUTO_CALIB_START),
                         "Auto Calibration program will be started. Don't "
                         "connect any probes. \nIt can take a while!"));

      if (MsgBox::Confirm(strMsg)) {
        zero_adj();
      } else {
        _device_agent->set_config_bool(SR_CONF_ZERO, false);
        update_view_status();
      }
      return false;
    }
  }

  if (_device_agent->get_work_mode() == LOGIC &&
      _session->is_realtime_refresh()) {
    if (_view != NULL)
      _view->auto_set_max_scale();
  }

  _is_run_as_instant = true;
  bool ret = _session->start_capture(true);

  return ret;
}

void SamplingBar::on_device_selected() {
  if (_updating_device_list) {
    return;
  }
  if (_device_selector->currentIndex() == -1) {
    pxv_err("Have no selected device.");
    return;
  }
  _session->stop_capture();
  _session->session_save();

  ds_device_handle devHandle =
      (ds_device_handle)_device_selector->currentData().toULongLong();
  if (_session->have_hardware_data() && _session->is_first_store_confirm()) {
    if (MsgBox::Confirm(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_SAVE_CAPDATE),
                            "Save captured data?"))) {
      _updating_device_list = true;
      _device_selector->setCurrentIndex(_last_device_index);
      _updating_device_list = false;
      _next_switch_device = devHandle; // Save end, auto switch to this device.
      sig_store_session_data();
      return;
    }
  }

  if (_session->set_device(devHandle)) {
    _last_device_index = _device_selector->currentIndex();
  } else {
    update_device_list(); // Reload the list.
  }
}

void SamplingBar::enable_toggle(bool enable) {
  bool test = false;

  if (_device_agent->have_instance()) {
    _device_agent->get_config_bool(SR_CONF_TEST, test);
  }
  if (!test) {
    _sample_count->setDisabled(!enable);

    if (_device_agent->get_work_mode() == DSO)
      _sample_rate->setDisabled(true);
    else
      _sample_rate->setDisabled(!enable);
  } else {
    _sample_count->setDisabled(true);
    _sample_rate->setDisabled(true);
  }
}

void SamplingBar::reload() {
  QString iconPath = GetIconPath();

  bool show_mode_row = false;
  bool show_loop = false;
  int mode = _device_agent->get_work_mode();

  if (mode == LOGIC) {
    if (!_device_agent->is_file()) {
      show_mode_row = true;
      if (_device_agent->is_stream_mode() || _device_agent->is_demo())
        show_loop = true;

      if (_session->is_loop_mode() && !_device_agent->is_stream_mode() &&
          _device_agent->is_hardware()) {
        _session->set_collect_mode(COLLECT_SINGLE);
      }
    }
  } else if (mode == ANALOG) {
  } else if (mode == DSO) {
  }

  if (_radio_single) {
    _radio_single->setVisible(show_mode_row);
    _radio_repeat->setVisible(show_mode_row);
    _radio_loop->setVisible(show_mode_row && show_loop);
    QLabel *ml =
        _radio_single->parentWidget()->findChild<QLabel *>("mode_label");
    if (ml)
      ml->setVisible(show_mode_row);

    if (show_mode_row) {
      int cur_mode = _session->get_collect_mode();
      if (cur_mode == COLLECT_SINGLE)
        _radio_single->setChecked(true);
      else if (cur_mode == COLLECT_REPEAT)
        _radio_repeat->setChecked(true);
      else if (cur_mode == COLLECT_LOOP)
        _radio_loop->setChecked(true);
    }
  }

  retranslateUi();
  reStyle();
  update();
}

void SamplingBar::on_mode_radio_clicked(int id) {
  if (_is_readonly)
    return;

  switch (id) {
  case COLLECT_SINGLE:
    _session->set_collect_mode(COLLECT_SINGLE);
    if (_device_agent->is_demo()) {
      _device_agent->set_config_string(SR_CONF_PATTERN_MODE, "protocol");
      _session->broadcast_msg(DSV_MSG_DEMO_OPERATION_MODE_CHNAGED);
    }
    if (_context && _context->document()) {
      _context->document()->_dock_collect_mode =
          (int)_session->get_collect_mode();
    }
    break;
  case COLLECT_REPEAT:
    if (_device_agent->is_stream_mode() || _device_agent->is_demo()) {
      _session->set_repeat_intvl(0.1);
      _session->set_collect_mode(COLLECT_REPEAT);
    } else {
      pv::dialogs::Interval interval_dlg(this);
      interval_dlg.set_interval(_session->get_repeat_intvl());
      interval_dlg.exec();
      if (interval_dlg.is_done()) {
        _session->set_repeat_intvl(interval_dlg.get_interval());
        _session->set_collect_mode(COLLECT_REPEAT);
      } else {
        return;
      }
    }
    if (_device_agent->is_demo()) {
      _device_agent->set_config_string(SR_CONF_PATTERN_MODE, "random");
      _session->broadcast_msg(DSV_MSG_DEMO_OPERATION_MODE_CHNAGED);
    }
    if (_context && _context->document()) {
      _context->document()->_dock_collect_mode =
          (int)_session->get_collect_mode();
    }
    break;
  case COLLECT_LOOP:
    _session->set_collect_mode(COLLECT_LOOP);
    if (_device_agent->is_demo()) {
      _device_agent->set_config_string(SR_CONF_PATTERN_MODE, "random");
      _session->broadcast_msg(DSV_MSG_DEMO_OPERATION_MODE_CHNAGED);
    }
    if (_context && _context->document()) {
      _context->document()->_dock_collect_mode =
          (int)_session->get_collect_mode();
    }
    break;
  }
}

void SamplingBar::on_collect_mode() {
  QString iconPath = GetIconPath();
  QAction *act = qobject_cast<QAction *>(sender());

  if (act == _action_single) {
    _session->set_collect_mode(COLLECT_SINGLE);

    if (_device_agent->is_demo()) {
      _device_agent->set_config_string(SR_CONF_PATTERN_MODE, "protocol");
      _session->broadcast_msg(DSV_MSG_DEMO_OPERATION_MODE_CHNAGED);
    }
  } else if (act == _action_repeat) {
    if (_device_agent->is_stream_mode() || _device_agent->is_demo()) {
      _session->set_repeat_intvl(0.1);
      _session->set_collect_mode(COLLECT_REPEAT);
    } else {
      pv::dialogs::Interval interval_dlg(this);

      interval_dlg.set_interval(_session->get_repeat_intvl());
      interval_dlg.exec();

      if (interval_dlg.is_done()) {
        _session->set_repeat_intvl(interval_dlg.get_interval());
        _session->set_collect_mode(COLLECT_REPEAT);
      }
    }

    if (_device_agent->is_demo()) {
      _device_agent->set_config_string(SR_CONF_PATTERN_MODE, "random");
      _session->broadcast_msg(DSV_MSG_DEMO_OPERATION_MODE_CHNAGED);
    }
  } else if (act == _action_loop) {
    _session->set_collect_mode(COLLECT_LOOP);

    if (_device_agent->is_demo()) {
      _device_agent->set_config_string(SR_CONF_PATTERN_MODE, "random");
      _session->broadcast_msg(DSV_MSG_DEMO_OPERATION_MODE_CHNAGED);
    }
  }

  update_mode_icon();
}

void SamplingBar::update_device_list() {
  struct ds_device_base_info *array = NULL;
  int dev_count = 0;
  int select_index = 0;

  pxv_info("Update device list.");

  array = _session->get_device_list(dev_count, select_index);

  if (array == NULL) {
    pxv_err("Get deivce list error!");
    return;
  }

  _updating_device_list = true;
  struct ds_device_base_info *p = NULL;
  ds_device_handle cur_dev_handle = NULL_HANDLE;

  _device_selector->clear();

  for (int i = 0; i < dev_count; i++) {
    p = (array + i);
    _device_selector->addItem(
        QString(p->name), QVariant::fromValue((unsigned long long)p->handle));

    if (i == select_index)
      cur_dev_handle = p->handle;
  }
  free(array);

  _device_selector->setCurrentIndex(select_index);

  if (cur_dev_handle != _last_device_handle) {
    update_sample_rate_list();
    _last_device_handle = cur_dev_handle;
  }

  _last_device_index = select_index;
  int width = _device_selector->sizeHint().width();
  _device_selector->view()->setMinimumWidth(width + 30);

  _updating_device_list = false;
}

void SamplingBar::config_device() {}

void SamplingBar::update_view_status() {
  int bEnable = _session->is_working() == false;
  int mode = _session->get_device()->get_work_mode();

  _device_type->setEnabled(bEnable);
  _device_selector->setEnabled(bEnable);

  if (_radio_single) {
    _radio_single->setEnabled(bEnable);
    _radio_repeat->setEnabled(bEnable);
    _radio_loop->setEnabled(bEnable);
    _radio_loop->setVisible(false);
  }

  if (_session->get_device()->is_file()) {
    _sample_rate->setEnabled(false);
    _sample_count->setEnabled(false);
  } else if (mode == DSO) {
    _sample_rate->setEnabled(false);
    _sample_count->setEnabled(bEnable);

    if (_session->is_working() && _session->is_instant() == false) {
      _sample_count->setEnabled(true);
    }
  } else {
    _sample_rate->setEnabled(bEnable);
    _sample_count->setEnabled(bEnable);

    if (mode == LOGIC && _session->get_device()->is_hardware()) {
      int mode_val = 0;
      if (_session->get_device()->get_config_int16(SR_CONF_OPERATION_MODE,
                                                   mode_val)) {
        if (mode_val == LO_OP_INTEST) {
          _sample_rate->setEnabled(false);
          _sample_count->setEnabled(false);
        }
      }
    }

    if (mode == LOGIC && _device_agent->is_file() == false) {
      if (_device_agent->is_stream_mode() || _device_agent->is_demo())
        if (_radio_loop)
          _radio_loop->setVisible(true);
    }
  }

  retranslateUi();

  if (bEnable) {
    _is_run_as_instant = false;
  }

  update_mode_icon();

  if (_session->get_device()->is_demo() && bEnable) {
    QString opt_mode = _device_agent->get_demo_operation_mode();

    if (opt_mode != "random" && mode == LOGIC) {
      _sample_rate->setEnabled(false);
      _sample_count->setEnabled(false);
    }
  }
}

ds_device_handle SamplingBar::get_next_device_handle() {
  ds_device_handle h = _next_switch_device;
  _next_switch_device = NULL_HANDLE;
  return h;
}

void SamplingBar::update_mode_icon() {
  QString iconPath = GetIconPath();
  QColor iconColor = AppConfig::Instance().GetThemeColor("@titlebar-icon-accent");

  auto getIcon = [&](const QString &name) {
      return iconColor.isValid() ? IconCache::Instance().tintedIcon(iconPath + name, iconColor)
                                 : IconCache::Instance().icon(iconPath + name);
  };

  if (_session->is_repeat_mode())
    _mode_button->setIcon(getIcon(REPEAT_ACTION_ICON));
  else if (_session->is_loop_mode())
    _mode_button->setIcon(getIcon(LOOP_ACTION_ICON));
  else
    _mode_button->setIcon(getIcon(SINGLE_ACTION_ICON));
}

void SamplingBar::run_or_stop() { on_run_stop(); }

void SamplingBar::run_or_stop_instant() { on_instant_stop(); }

void SamplingBar::UpdateLanguage() { retranslateUi(); }

void SamplingBar::UpdateTheme() { reStyle(); }

void SamplingBar::UpdateFont() {
  QFont font = dock_font_content();
  ui::set_toolbar_font(this, font);

  update_view_status();
}

void SamplingBar::device_selected() { _mode_button->click(); }

void SamplingBar::set_context(SigSession *session, pv::view::View *view) {
  _session = session;
  _device_agent = _session->get_device();
  _view = view;
  update_device_list();
  update_sample_rate_list();
}

void SamplingBar::set_readonly(bool readonly) {
  _is_readonly = readonly;

  _device_selector->setEnabled(!readonly);
  _sample_rate->setEnabled(!readonly);
  _sample_count->setEnabled(!readonly);
  _mode_button->setEnabled(!readonly);
}

void SamplingBar::set_sample_count_index(int index) {
  _sample_count->setCurrentIndex(index);
}

} // namespace toolbars
} // namespace pv
