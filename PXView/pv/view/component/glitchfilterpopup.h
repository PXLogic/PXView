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

#ifndef GLITCHFILTERPOPUP_H
#define GLITCHFILTERPOPUP_H

#include <QWidget>
#include <cstdint>
#include <vector>

#include "pv/data/snapshot/logicsnapshot.h"  // GlitchFilterMode
#include "pv/data/pulse_analyzer.h"
#include "pv/ui/uimanager.h"

class QSlider;
class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
class QHBoxLayout;
class QKeyEvent;
class QCloseEvent;
class QShowEvent;
class QSpinBox;
class QCheckBox;

namespace pv {

class SigSession;

namespace view {

class LogicSignal;
class PulseHistogramWidget;
class View;

/*
 * GlitchFilterPopup —— 毛刺滤波 UX 重设计的核心浮窗控件(View 层,纯 QWidget)。
 *
 * 匹配 HTML 原型(prototype/glitch_filter_ux.html)的交互体验:
 *   - 脉冲宽度分布直方图(由 PulseHistogramWidget 渲染)
 *   - 推荐阈值线 + 当前阈值线 + 实时着色
 *   - 阈值滑块 + 模式下拉 + 预设方案
 *   - 实时统计"将滤除/剩余有效"脉冲数
 *   - 应用本通道 / 应用到所有逻辑通道
 *
 * 控件只 emit 信号让上层(View/MainWindow)处理实际滤波写入,
 * 不直接调用 Core 的 set_glitch_filter 或 ds_* libsigrok API。
 * 不持有 LogicSnapshot 指针,每次 open_for_signal 重新获取,避免悬垂。
 */
class GlitchFilterPopup : public QWidget, public IUiWindow {
    Q_OBJECT
public:
    explicit GlitchFilterPopup(View& view, QWidget* parent = nullptr);
    ~GlitchFilterPopup() override;

    void open_for_signal(LogicSignal* sig, const QPoint& anchor_pos);
    // 批量模式:对多个逻辑通道统一滤波(如 DecodeTrace 绑定的子通道)。
    // 直方图合并(sum counts)各通道,应用时对所有 _target_sigs 设同一阈值。
    void open_for_batch(const std::vector<LogicSignal*>& sigs, const QPoint& anchor_pos);
    bool is_open() const { return isVisible(); }
    LogicSignal* target_signal() const { return _target_sig; }
    bool is_batch_mode() const { return _is_batch_mode; }

    // 重新计算直方图与默认值,并同步直方图控件/统计显示。
    // 注意:不重新扫描 LogicSnapshot — 滤波后 snapshot 中短脉冲已被滤除,
    // 重新扫描会得到错误的分布。始终使用 open_for_signal 时缓存的原始脉冲数据。
    // 仅更新 UI 控件状态(阈值线、统计数字)。
    void refresh();

public slots:
    // 毛刺滤波完成后,若 popup 已打开,刷新直方图反映滤波后的脉冲分布
    // (主要是长脉冲,短毛刺已消失)。
    void on_filter_completed();
    // 毛刺滤波清除后,若 popup 已打开,刷新直方图反映未滤波的原始脉冲分布。
    void on_filter_cleared();

signals:
    void preview_changed(pv::view::LogicSignal* sig, uint32_t threshold, GlitchFilterMode mode);
    void apply_requested(pv::view::LogicSignal* sig, uint32_t threshold, GlitchFilterMode mode, bool all_channels);
    // 批量模式应用:对 _target_sigs 中所有通道设同一阈值。
    void apply_batch_requested(const std::vector<pv::view::LogicSignal*>& sigs, uint32_t threshold, GlitchFilterMode mode);
    // 批量模式预览:对每个 sig 都更新 preview_ranges。
    void preview_batch_changed(const std::vector<pv::view::LogicSignal*>& sigs, uint32_t threshold, GlitchFilterMode mode);
    void cleared(pv::view::LogicSignal* sig, bool all_channels);
    void closed();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void on_slider_moved(int value);
    void on_mode_changed(int index);
    void on_apply_one_clicked();
    void on_apply_all_clicked();
    void on_cancel_clicked();
    void on_max_changed(int val);
    void on_auto_apply_toggled(bool checked);
    void on_show_overlay_toggled(bool checked);

private:
    void build_ui();
    void apply_qss();
    void update_histogram_coloring();
    void update_stats();
    void refresh_from_signal();  // 重新计算直方图与默认值
    void rebuild_histogram();    // 用当前 _max_spinbox 值作为 cap 重建直方图+滑块范围
    void show_and_position(const QPoint& anchor_pos);  // 定位 + 淡入显示
    void retranslateUi();
    void retranslate_title_and_buttons();  // 标题/按钮文案(依赖 _is_batch_mode + _open_display_name)

    uint32_t current_threshold() const;
    GlitchFilterMode current_mode() const;

    // IUiWindow
    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

    View& _view;
    LogicSignal* _target_sig = nullptr;  // 单通道模式主信号(batch 模式 = _target_sigs[0])
    std::vector<LogicSignal*> _target_sigs;  // 批量模式所有目标信号(单通道模式仅含 _target_sig)
    bool _is_batch_mode = false;

    // 缓存的脉冲数据(打开时计算一次,预览时复用)
    std::vector<pv::data::PulseAnalyzer::Pulse> _cached_pulses;
    pv::data::PulseAnalyzer::Histogram _cached_hist;
    uint32_t _recommended_threshold = 3;

    // 控件
    QLabel* _title_label = nullptr;
    QPushButton* _close_btn = nullptr;
    PulseHistogramWidget* _histogram = nullptr;
    QLabel* _filter_count_lbl = nullptr;
    QLabel* _remain_count_lbl = nullptr;
    QComboBox* _mode_combo = nullptr;
    QSlider* _threshold_slider = nullptr;
    QLabel* _threshold_value_lbl = nullptr;
    QSpinBox* _max_spinbox = nullptr;  // 用户自定义统计上限(默认 30)
    QCheckBox* _auto_apply_chk = nullptr;  // 采集后自动重新应用
    QCheckBox* _show_overlay_chk = nullptr; // 显示波形轨道红色滤波提示
    QPushButton* _apply_one_btn = nullptr;
    QPushButton* _apply_all_btn = nullptr;
    QPushButton* _cancel_btn = nullptr;
    // 仅用于 retranslateUi 重设文本的 label(构造时一次性加入布局)
    QLabel* _section_dist_label = nullptr;
    QLabel* _will_remove_label = nullptr;
    QLabel* _pulses_unit_label = nullptr;
    QLabel* _remain_label = nullptr;
    QLabel* _type_label = nullptr;
    QLabel* _threshold_label = nullptr;
    QLabel* _cycles_label = nullptr;
    QLabel* _max_label = nullptr;
    QLabel* _max_hint_label = nullptr;
    // 当前打开模式缓存(用于 retranslateUi 重建动态标题)
    QString _open_display_name;       // 单通道模式:通道显示名
    bool _has_open_display_name = false;
};

} // namespace view
} // namespace pv

#endif // GLITCHFILTERPOPUP_H
