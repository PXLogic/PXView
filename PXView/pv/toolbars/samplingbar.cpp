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
#include "../dsvdef.h"
#include "../interface/icallbacks.h"
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

namespace {

// 按 1-2-5 序列在 [min_rate, max_rate] 范围生成离散采样率列表
// 例如 [1Hz, 1GHz] → 1Hz/2Hz/5Hz/10Hz/.../1GHz（约 30 个点）
QVector<uint64_t> generate_1_2_5_steps(uint64_t min_rate, uint64_t max_rate)
{
    QVector<uint64_t> steps;
    if (min_rate == 0 || max_rate == 0 || min_rate > max_rate)
        return steps;

    // 1-2-5 序列的尾数
    static const int mantissas[] = {1, 2, 5};
    const int num_mantissas = sizeof(mantissas) / sizeof(mantissas[0]);

    // 找到 >= min_rate 的起始点
    uint64_t cur = 1;  // 10^0 * 1
    while (cur < min_rate)
        cur *= 10;

    // 对每个十进制量级，生成 1×/2×/5× 三个点
    while (cur <= max_rate) {
        for (int i = 0; i < num_mantissas; ++i) {
            uint64_t rate = cur * mantissas[i] / 1;  // cur 已是 10^n
            if (rate >= min_rate && rate <= max_rate) {
                // 去重（避免 10×1 == 1×10 重复）
                if (steps.isEmpty() || steps.last() != rate)
                    steps.append(rate);
            }
        }
        cur *= 10;
    }
    return steps;
}

}  // namespace

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

  connect(_device_selector, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &SamplingBar::on_device_selected);
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
  if (!ctx) {
    pxv_warn("%s", "SamplingBar::bind_context: ctx is NULL");
    return;
  }
  assert(ctx);
  _context = ctx;
  _session = ctx->session();
  _view = ctx->view();
  _device_agent = _session->get_device();
  set_readonly(!ctx->is_live());
  if (_device_agent && _device_agent->have_instance()) {
    update_device_list();
    auto &ui = ctx->view()->dock_ui_state();
    if (ui.dock_sample_rate > 0) {
      _device_agent->set_config_uint64(SR_CONF_SAMPLERATE, ui.dock_sample_rate);
      _device_agent->set_config_uint64(SR_CONF_LIMIT_SAMPLES,
                                       ui.dock_sample_limit);
      _session->set_collect_mode((DEVICE_COLLECT_MODE)ui.dock_collect_mode);
    }

    update_sample_rate_selector();

    if (ui.dock_sample_rate > 0) {
      for (int i = _sample_rate->count() - 1; i >= 0; i--) {
        if (ui.dock_sample_rate >=
            _sample_rate->itemData(i).value<uint64_t>()) {
          _sample_rate->setCurrentIndex(i);
          break;
        }
      }
    }

    if (ui.dock_sample_limit > 0 && ui.dock_sample_rate > 0) {
      double duration =
          (double)ui.dock_sample_limit / ui.dock_sample_rate * SR_SEC(1);
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
  if (_context && _context->view() && _device_agent && _session &&
      _device_agent->have_instance()) {
    auto &ui = _context->view()->dock_ui_state();

    if (_sample_rate->count() > 0 && _sample_rate->currentIndex() >= 0) {
      ui.dock_sample_rate = _sample_rate->itemData(_sample_rate->currentIndex())
                                .value<uint64_t>();
    } else {
      ui.dock_sample_rate = _device_agent->get_sample_rate();
    }

    if (_sample_count->count() > 0 && _sample_count->currentIndex() >= 0) {
      double duration = _sample_count->itemData(_sample_count->currentIndex())
                            .value<double>();
      uint64_t s_rate = ui.dock_sample_rate > 0
                            ? ui.dock_sample_rate
                            : _device_agent->get_sample_rate();
      if (s_rate > 0) {
        ui.dock_sample_limit =
            ((uint64_t)ceil(duration / SR_SEC(1) * s_rate) + SAMPLES_ALIGN) &
            ~SAMPLES_ALIGN;
      } else {
        ui.dock_sample_limit = _device_agent->get_sample_limit();
      }
    } else {
      ui.dock_sample_limit = _device_agent->get_sample_limit();
    }

    ui.dock_collect_mode = (int)_session->get_collect_mode();
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
      // SR_CONF_USB_SPEED fork key deleted — use DeviceAgent typed wrapper.
      int usb_speed = _device_agent->get_usb_speed();

      if (usb_speed == LIBUSB_SPEED_HIGH) {
        _device_type_label->setText("USB 2.0");
        _device_type->setToolTip(
            tr("USB 2.0 (High Speed)\nMax bandwidth: ~40 MB/s\nStream mode: "
               "lower sample rates\nBuffer mode: full sample rates"));
      } else if (usb_speed == LIBUSB_SPEED_SUPER) {
        _device_type_label->setText("USB 3.0");
        _device_type->setToolTip(
            tr("USB 3.0 (SuperSpeed)\nMax bandwidth: ~400 MB/s\nStream mode: "
               "higher sample rates available"));
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
    _settings_title_label->setText(
        L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_SAMPLING_SETTINGS), "采样设置"));
    _dev_label->setText(
        L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_DEVICE), "设备"));
    _depth_label->setText(
        L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_SAMPLE_DEPTH), "采样深度"));
    _rate_label->setText(
        L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_SAMPLE_RATE), "采样率"));
    _mode_label->setText(
        L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_ROW), "捕获模式"));
    _radio_single->setText(
        L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_SINGLE), "单次"));
    _radio_repeat->setText(
        L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_REPEAT), "重复"));
    _radio_loop->setText(
        L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_CAPTURE_MODE_LOOP), "循环"));
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
      // SR_CONF_USB_SPEED fork key deleted — use DeviceAgent typed wrapper.
      int usb_speed = _device_agent->get_usb_speed();

      if (usb_speed == LIBUSB_SPEED_SUPER)
        _device_type->setIcon(IconCache::Instance().icon(":/icons/usb3.svg"));
      else
        _device_type->setIcon(IconCache::Instance().icon(":/icons/usb2.svg"));
    }
  }

  if (true) {
    QString iconPath = GetIconPath();
    QColor iconColor =
        AppConfig::Instance().GetThemeColor("@titlebar-icon-accent");

    auto getIcon = [&](const QString &name) {
      return iconColor.isValid()
                 ? IconCache::Instance().tintedIcon(iconPath + name, iconColor)
                 : IconCache::Instance().icon(iconPath + name);
    };

    _action_single->setIcon(getIcon(SINGLE_ACTION_ICON));
    _action_repeat->setIcon(getIcon(REPEAT_ACTION_ICON));
    _action_loop->setIcon(getIcon(LOOP_ACTION_ICON));

    update_mode_icon();
  }
}

void SamplingBar::zero_adj() {
  // DSO zero calibration removed: WaitingDialog and SR_CONF_ZERO fork key were
  // deleted (DSO mode deprecated, DSCope hardware dropped). No-op stub kept
  // so call sites in action_run()/action_instant_stop() compile.
}

void SamplingBar::set_sample_rate(uint64_t sample_rate) {
  for (int i = _sample_rate->count() - 1; i >= 0; i--) {
    uint64_t cur_index_sample_rate =
        _sample_rate->itemData(i).value<uint64_t>();
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

  _sample_rate->clear();

  // 优先处理离散列表格式（"samplerates"）
  if ((gvar_list = g_variant_lookup_value(gvar_dict, "samplerates",
                                          G_VARIANT_TYPE("at")))) {
    elements = (const uint64_t *)g_variant_get_fixed_array(
        gvar_list, &num_elements, sizeof(uint64_t));

    for (unsigned int i = 0; i < num_elements; i++) {
      char *const s = sr_samplerate_string(elements[i]);
      _sample_rate->addItem(QString(s), QVariant::fromValue(elements[i]));
      g_free(s);
    }

    g_variant_unref(gvar_list);
  }
  // 回退处理 step 格式（"samplerate-steps"）—— demo 等上游驱动使用
  else if ((gvar_list = g_variant_lookup_value(gvar_dict, "samplerate-steps",
                                               G_VARIANT_TYPE("at")))) {
    elements = (const uint64_t *)g_variant_get_fixed_array(
        gvar_list, &num_elements, sizeof(uint64_t));

    // step 格式为 [min, max, step]，在应用层按 1-2-5 序列生成离散列表
    if (num_elements >= 2 && elements[0] > 0 && elements[1] > 0) {
      QVector<uint64_t> rates = generate_1_2_5_steps(elements[0], elements[1]);
      for (uint64_t r : rates) {
        char *const s = sr_samplerate_string(r);
        _sample_rate->addItem(QString(s), QVariant::fromValue(r));
        g_free(s);
      }
    }

    g_variant_unref(gvar_list);
  }

  _sample_rate->show();
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

  if (_context && _context->view()) {
    _context->view()->dock_ui_state().dock_sample_rate =
        _device_agent->get_sample_rate();
  }
}

void SamplingBar::update_sample_count_selector() {
  bool stream_mode = false;
  uint64_t hw_depth = 0;
  uint64_t sw_depth;
  double pre_duration = SR_SEC(1);
  double duration;

  pxv_info("Update sample count list.");

  // DSO 模式下拉内容是 time/div（时基），非 DSO 模式是采样深度（总捕获时长）。
  // 标签需要随模式切换，否则用户看到"采样深度"标签下显示"ms/div"会误解为电压刻度。
  if (_device_agent && _device_agent->get_work_mode() == DSO) {
    _depth_label->setText(
        L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_TIMEBASE), "时基"));
  } else {
    _depth_label->setText(
        L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_SAMPLE_DEPTH), "采样深度"));
  }

  if (_updating_sample_count) {
    pxv_err("Error! The sample count is updating.");
    return;
  }

  disconnect(_sample_count, QOverload<int>::of(&QComboBox::currentIndexChanged),
             this, &SamplingBar::on_samplecount_sel);

  assert(!_updating_sample_count);
  _updating_sample_count = true;

  // 从驱动获取真实硬件存储深度（SR_CONF_HW_DEPTH），用于构建采样深度下拉框
  // 上限，防止用户选择超过硬件存储能力的深度。驱动不支持时（如 fx2lafw、demo）
  // 返回 0，回退到 get_ring_sample_count() 作为参考。
  stream_mode = _device_agent->is_stream_mode();
  hw_depth = _device_agent->get_hw_depth();
  if (hw_depth == 0)
    hw_depth = _device_agent->get_ring_sample_count();
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

  // DSO mode uses driver-provided timebase range (SR_CONF_MAX/MIN_TIMEBASE)
  // to populate the time-per-division selector. Demo and PXLogic both support
  // these keys. Fall back to 500ms/10ns if the driver doesn't.
  uint64_t max_timebase = SR_MS(500);
  uint64_t min_timebase = SR_NS(10);
  if (mode == DSO) {
    if (!_device_agent->get_config_uint64(SR_CONF_MAX_TIMEBASE, max_timebase))
      max_timebase = SR_MS(500);
    if (!_device_agent->get_config_uint64(SR_CONF_MIN_TIMEBASE, min_timebase))
      min_timebase = SR_NS(10);
  }

  if (0 != _sample_count->count())
    pre_duration =
        _sample_count->itemData(_sample_count->currentIndex()).value<double>();
  _sample_count->clear();
  const uint64_t samplerate =
      _sample_rate->itemData(_sample_rate->currentIndex()).value<uint64_t>();
  const double hw_duration =
      (samplerate > 0) ? (hw_depth / (samplerate * (1.0 / SR_SEC(1)))) : 0;

  if (mode == DSO) {
    // DSO mode: the selector lists time-per-division values from
    // max_timebase down to min_timebase (e.g. 500ms/div ... 10ns/div).
    // This is fundamentally different from LOGIC/ANALOG where the selector
    // lists total capture duration. Using the sw_depth/samplerate formula
    // here yields huge garbage values (5e14 s) and breaks commit_hori_res()
    // which divides by hori_res.
    duration = (double)max_timebase;
  } else if (stream_mode) {
    // Stream mode: data flows continuously via mmap, not limited by hardware
    // FIFO. mmap is backed by either memory (no disk cache) or disk file (with
    // cache). The two modes are mutually exclusive — not additive.
    // - No disk cache: use SR_CONF_STREAM_MEM_BUFF (memory mmap size)
    // - Disk cache:    use SR_CONF_STREAM_BUFF (disk mmap size)
    int ch_num = _session->get_ch_num(SR_CHANNEL_LOGIC);
    if (ch_num <= 0)
      ch_num = 1;
    bool disk_cache_enabled = false;
    if (_device_agent->is_dsl_device())
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
  } else if (hw_duration > 0)
    duration = hw_duration;
  else
    // Devices without hardware depth limits (fx2lafw, demo): use software
    // depth (memory-mapped) as the upper bound. PulseView uses 1 trillion
    // samples as default; we use sw_depth which is 16GB on 64-bit.
    duration = sw_depth / (samplerate * (1.0 / SR_SEC(1)));

  if (duration <= 0) {
    // Final fallback: 1 second (should rarely hit after sw_depth fallback)
    duration = SR_SEC(1);
  }

  if (duration <= 0) {
    pxv_err("update_sample_count_selector: duration<=0, aborting");
    _updating_sample_count = false;
    return;
  }
  bool not_last = true;

  do {
    QString suffix = (mode == DSO) ? DIVString : "";
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
      // DSO: continue down to min_timebase (e.g. 10ns/div).
      not_last = (duration > (double)min_timebase);
    else if (mode == ANALOG)
      not_last = (duration >= SR_MS(200)) &&
                 (duration / SR_SEC(1) * samplerate >= SR_KB(1));
    else
      not_last = (duration / SR_SEC(1) * samplerate >= SR_KB(1));

  } while (not_last);

  _sample_count->view()->setMinimumWidth(_sample_count->sizeHint().width() +
                                         30);

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

  // NOTE: keep _updating_sample_count = true across the manual on_samplecount_sel
  // call below. on_samplecount_sel -> apply_sample_count -> commit_hori_res ->
  // set_config_uint64(SR_CONF_TIMEBASE) -> DeviceAgent::config_changed (sync) ->
  // SigSession::DeviceConfigChanged -> broadcast_async<SampleCountUpdated>
  // -> MainWindow::on_device_options -> update_sample_count_selector (re-entry).
  // The entry guard at the top of this function (line 650) breaks the loop ONLY
  // if _updating_sample_count is still true on re-entry. Clearing the flag here
  // (as the previous code did) defeats the guard and causes unbounded recursion
  // -> stack overflow -> SIGSEGV (visible crash frame in MeasureDock::adjust_form_size).
  // update_sample_count_selector_value() below early-returns while the flag is
  // true; that is intentional and safe — the combo index was just set above by
  // set_sample_count_index, so no device re-sync is needed at this point.
  // Clear the re-entry guard BEFORE calling update_sample_count_selector_value()
  // so it actually executes and syncs the dropdown with the device's actual
  // sample limit (loaded from .pxc). Without this, the dropdown stays at
  // pre_duration (default 1s) and on_samplecount_sel() commits 1s to the
  // device, overwriting the restored sample limit.
  // The signal is still disconnected (reconnected below), so no recursion.
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

  _session->broadcast_async<interface::SampleRateChanged>({});
}

void SamplingBar::on_samplecount_sel(int index) {
  (void)index;

  double hori_res = -1;
  apply_sample_count(hori_res);

  if (_context && _context->view()) {
    _context->view()->dock_ui_state().dock_sample_limit =
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

  pxv_info("[DEBUG-DSO] hori_knob: dir=%d currentIndex=%d count=%d",
           dir, _sample_count->currentIndex(), _sample_count->count());

  disconnect(_sample_count, QOverload<int>::of(&QComboBox::currentIndexChanged),
             this, &SamplingBar::on_samplecount_sel);

  if (0 == dir) {
    hori_res = commit_hori_res();
  } else if ((dir > 0) && (_sample_count->currentIndex() > 0)) {
    set_sample_count_index(_sample_count->currentIndex() - 1);
    hori_res = commit_hori_res();

    if (_session->have_view_data() == false) {
      _session->apply_samplerate();
      _session->broadcast_async<interface::SampleRateChanged>({});
    }
  } else if ((dir < 0) &&
             (_sample_count->currentIndex() < _sample_count->count() - 1)) {
    set_sample_count_index(_sample_count->currentIndex() + 1);
    hori_res = commit_hori_res();

    if (_session->have_view_data() == false) {
      _session->apply_samplerate();
      _session->broadcast_async<interface::SampleRateChanged>({});
    }
  }

  connect(_sample_count, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &SamplingBar::on_samplecount_sel);

  pxv_info("[DEBUG-DSO] hori_knob: returning hori_res=%.9g", hori_res);
  return hori_res;
}

double SamplingBar::commit_hori_res() {
  const double hori_res =
      _sample_count->itemData(_sample_count->currentIndex()).value<double>();

  const uint64_t sample_limit = _device_agent->get_sample_limit();
  uint64_t max_sample_rate = 0;
  if (!_device_agent->get_config_uint64(SR_CONF_MAX_DSO_SAMPLERATE,
                                        max_sample_rate)) {
    pxv_err("ERROR: config_get SR_CONF_MAX_DSO_SAMPLERATE failed.");
    return -1;
  }

  const int dso_ch_num = _session->get_ch_num(SR_CHANNEL_DSO);
  const uint64_t sample_rate = min(
      (uint64_t)(sample_limit * SR_SEC(1) / (hori_res * DS_CONF_DSO_HDIVS)),
      (uint64_t)(max_sample_rate /
                 (dso_ch_num ? dso_ch_num : 1)));

  pxv_info("[DEBUG-DSO] commit_hori_res: hori_res=%.9g sample_limit=%llu max_sample_rate=%llu dso_ch_num=%d -> sample_rate=%llu",
           hori_res, (unsigned long long)sample_limit,
           (unsigned long long)max_sample_rate, dso_ch_num,
           (unsigned long long)sample_rate);

  // 直接设置采样率到设备，不调用 set_sample_rate() → commit_settings()。
  // commit_settings() 会广播 DeviceOptionsUpdated → SigSession::reload()，
  // 重建所有 SignalModel（重操作）。时基切换不需要重建 SignalModel
  // （通道结构没变，只是采样率/时基变了），reload 会导致滚轮切换时基
  // 严重卡顿（每次 tick 重建一次 SignalModel + View 重新绑定）。
  // DSView 的 commit_settings() 不广播任何事件，所以滚轮很流畅。
  // 这里仍更新 _sample_rate 下拉框显示（断开信号避免递归触发 on_samplerate_sel）。
  disconnect(_sample_rate, QOverload<int>::of(&QComboBox::currentIndexChanged),
             this, &SamplingBar::on_samplerate_sel);
  for (int i = _sample_rate->count() - 1; i >= 0; i--) {
    uint64_t cur = _sample_rate->itemData(i).value<uint64_t>();
    if (sample_rate >= cur) {
      _sample_rate->setCurrentIndex(i);
      break;
    }
  }
  connect(_sample_rate, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &SamplingBar::on_samplerate_sel);

  if (sample_rate != _device_agent->get_sample_rate())
    _device_agent->set_config_uint64(SR_CONF_SAMPLERATE, sample_rate);

  // Only SET TIMEBASE if the value actually changed. Unconditional SET
  // triggers config_changed() -> broadcast_async<SampleCountUpdated> ->
  // MainWindow::on_event -> update_sample_count_selector ->
  // on_samplecount_sel -> commit_hori_res -> SET TIMEBASE -> ... infinite
  // async loop (visible as repeated "Update sample count list." log spam
  // and 100% CPU). The async dispatch means the _updating_sample_count
  // re-entry guard can't break the loop (the flag is cleared before the
  // async event arrives). Matching the SR_CONF_SAMPLERATE check above.
  uint64_t cur_timebase = 0;
  if (_device_agent->get_config_uint64(SR_CONF_TIMEBASE, cur_timebase) &&
      (uint64_t)hori_res != cur_timebase) {
    _device_agent->set_config_uint64(SR_CONF_TIMEBASE, hori_res);
  }

  return hori_res;
}

void SamplingBar::commit_settings() {
  // SR_CONF_TEST fork key deleted from pxlogic.c — test mode is no longer a
  // driver-exposed concept. The old `if (test)` branch (which only refreshed
  // selector values without committing) is removed; always commit settings.
  const double sample_duration =
      _sample_count->itemData(_sample_count->currentIndex()).value<double>();
  const uint64_t sample_rate =
      _sample_rate->itemData(_sample_rate->currentIndex()).value<uint64_t>();

  if (_device_agent->have_instance()) {
    if (sample_rate != 0 && sample_rate != _device_agent->get_sample_rate())
      _device_agent->set_config_uint64(SR_CONF_SAMPLERATE, sample_rate);

    if (_device_agent->get_work_mode() != DSO) {
      // 将用户选择的采样深度（sample_duration 秒）换算为样本数并下发到驱动。
      // fx2lafw 等上游驱动在 protocol.c 中实现 sent_samples/limit_samples 停止
      // 逻辑：到达 limit_samples 即调用 fx2lafw_abort_acquisition 停止采集。
      // hwdriver.c 只拒绝 set 0（= 不限制/持续流），非零值会被驱动接受。
      //
      // Stream 模式下若用户选择有限采样深度（如 1s），sample_count 非零，会
      // 被设置到驱动作为停止条件；若 UI 提供 "持续/∞" 选项（sample_duration
      // 为 0 或特殊值），sample_count 为 0，hwdriver.c 返回 SR_ERR_ARG，
      // DeviceAgent 静默忽略（pxv_dbg 日志），驱动保持 limit_samples=0 持续流。
      // Stream 模式的 ring buffer 大小仍由 DeviceAgent::get_sample_limit() 基于
      // _app_stream_mem_buff 计算，与停止条件解耦。
      const uint64_t sample_count =
          ((uint64_t)ceil(sample_duration / SR_SEC(1) * sample_rate) +
           SAMPLES_ALIGN) &
          ~SAMPLES_ALIGN;
      if (sample_count != 0 &&
          sample_count != _device_agent->get_driver_sample_limit())
        _device_agent->set_config_uint64(SR_CONF_LIMIT_SAMPLES, sample_count);
    }
    // R3: 采样率/采样数已修改，广播通知其他 GUI 组件刷新
    // (MainWindow::on_event(DeviceOptionsUpdated) -> rebuild_signals;
    // SigSession::on_event(DeviceOptionsUpdated) -> reload) R7: 同时发布
    // DEVICE_CONFIG_UPDATED（sample_rate/sample_limit 属于 设备配置变化），
    // 触发 SessionService 中此前为死代码的对应 case。
    if (_session) {
      _session->broadcast_async<interface::DeviceConfigUpdated>({});
      _session->broadcast_async<interface::DeviceOptionsUpdated>({});
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

  // DSO zero-calibration check removed (SR_CONF_ZERO fork key deleted,
  // DSO mode deprecated). zero_adj() is a no-op stub.

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

  // DSO zero-calibration check removed (SR_CONF_ZERO fork key deleted,
  // DSO mode deprecated). zero_adj() is a no-op stub.

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
  // SR_CONF_TEST fork key deleted from pxlogic.c — test mode no longer
  // disables the sample count/rate selectors. Always apply normal toggle logic.
  _sample_count->setDisabled(!enable);

  if (_device_agent->get_work_mode() == DSO)
    _sample_rate->setDisabled(true);
  else
    _sample_rate->setDisabled(!enable);
}

void SamplingBar::reload() {
  QString iconPath = GetIconPath();

  bool show_mode_row = false;
  bool show_loop = false;
  int mode = _device_agent->get_work_mode();

  const bool stream_mode = _device_agent->is_stream_mode();
  pxv_info("SamplingBar::reload: driver=%s, work_mode=%d, is_stream_mode=%d, "
           "is_demo=%d, is_file=%d",
           _device_agent->driver_name().toUtf8().constData(),
           mode, stream_mode,
           _device_agent->is_demo(), _device_agent->is_file());

  if (mode == LOGIC) {
    if (!_device_agent->is_file()) {
      show_mode_row = true;
      if (stream_mode || _device_agent->is_demo())
        show_loop = true;

      if (_session->is_loop_mode() && !stream_mode &&
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
    // Demo: do NOT override PATTERN_MODE here. The pattern is controlled
    // by the DeviceOptionsDock dropdown. The old code set "protocol"
    // which is not a valid logic_pattern_str[] entry (config_set rejects
    // it silently), and then update_view_status locked sample rate/depth
    // because the resulting pattern != "random".
    if (_context && _context->view()) {
      _context->view()->dock_ui_state().dock_collect_mode =
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
    // Demo: pattern is controlled by DeviceOptionsDock, not capture mode.
    if (_context && _context->view()) {
      _context->view()->dock_ui_state().dock_collect_mode =
          (int)_session->get_collect_mode();
    }
    break;
  case COLLECT_LOOP:
    _session->set_collect_mode(COLLECT_LOOP);
    // Demo: pattern is controlled by DeviceOptionsDock, not capture mode.
    if (_context && _context->view()) {
      _context->view()->dock_ui_state().dock_collect_mode =
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
    // Demo: pattern is controlled by DeviceOptionsDock, not capture mode.
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

    // Demo: pattern is controlled by DeviceOptionsDock, not capture mode.
  } else if (act == _action_loop) {
    _session->set_collect_mode(COLLECT_LOOP);
    // Demo: pattern is controlled by DeviceOptionsDock, not capture mode.
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
  // 设备未打开时用默认值 LOGIC，避免 _dev_handle NULL 警告
  int mode = LOGIC;
  if (_session->get_device()->have_instance()) {
    mode = _session->get_device()->get_work_mode();
  }

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
      /* Task 10/Phase 3: OPERATION_MODE config_get returns a string now;
       * use the int helper that converts "Buffer Mode"/"Stream Mode"/
       * "Internal Test" back to LO_OP_*. */
      int mode_val = _session->get_device()->get_hardware_operation_mode();
      if (mode_val == LO_OP_INTEST) {
        _sample_rate->setEnabled(false);
        _sample_count->setEnabled(false);
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

  // Demo device: sample rate and sample depth are always adjustable.
  // The old code locked these when pattern != "random", but all demo
  // patterns (sigrok/random/incremental/graycode/...) are math-generated
  // and respect cur_samplerate / limit_samples. Only .demo file replay
  // (sample_generator != DEMO_GEN_RANDOM) would have fixed rates, but
  // that is a driver-internal detail not exposed via PATTERN_MODE.
}

ds_device_handle SamplingBar::get_next_device_handle() {
  ds_device_handle h = _next_switch_device;
  _next_switch_device = NULL_HANDLE;
  return h;
}

void SamplingBar::update_mode_icon() {
  QString iconPath = GetIconPath();
  QColor iconColor =
      AppConfig::Instance().GetThemeColor("@titlebar-icon-accent");

  auto getIcon = [&](const QString &name) {
    return iconColor.isValid()
               ? IconCache::Instance().tintedIcon(iconPath + name, iconColor)
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
