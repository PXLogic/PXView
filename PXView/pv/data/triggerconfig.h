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

#ifndef PXVIEW_PV_DATA_TRIGGERCONFIG_H
#define PXVIEW_PV_DATA_TRIGGERCONFIG_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <vector>

namespace pv {
namespace data {

class TriggerConfig
{
public:
    // 触发模式：0=SIMPLE, 1=ADV, 2=SERIAL（对应 libsigrok 的 SIMPLE_TRIGGER/ADV_TRIGGER/SERIAL_TRIGGER 宏，但 Core 层不 include libsigrok 的 trigger 头，用 int 存储）
    enum Mode { Simple = 0, Adv = 1, Serial = 2 };

    struct Stage
    {
        QString value0;     // ADV: value0 字符串（如 "1 0 X ..."）；SERIAL stage0=start,stage1=edge,stage2=channel,stage3=data
        QString value1;     // ADV: value1 字符串；SERIAL stage0=stop,stage1=comp,stage2=channel_ext32,stage3=comp_ext32
        int      logic;     // (contiguous<<1) + logic_index
        int      inv0;      // inv0 combobox index
        int      inv1;      // inv1 combobox index
        int      count0;    // count spinbox value
        int      count1;    // 通常 0
    };

    TriggerConfig();
    ~TriggerConfig();

    // getters
    inline Mode mode() const { return _mode; }
    inline int trigger_pos() const { return _trigger_pos; }
    inline int stage_count() const { return _stage_count; }
    inline const std::vector<Stage>& stages() const { return _stages; }
    inline bool adv_enabled() const { return _adv_enabled; }
    inline int adv_tab_index() const { return _adv_tab_index; }  // 0=ADV, 1=SERIAL

    // SERIAL 专用
    inline int serial_data_channel() const { return _serial_data_channel; }
    inline int serial_bits() const { return _serial_bits; }
    inline const QString& serial_value() const { return _serial_value; }

    // setters
    void set_mode(Mode m);
    void set_trigger_pos(int pos);
    void set_stage_count(int n);
    void set_stages(const std::vector<Stage>& s);
    void set_adv_enabled(bool e);
    void set_adv_tab_index(int idx);
    void set_serial_data_channel(int ch);
    void set_serial_bits(int bits);
    void set_serial_value(const QString& v);

    // JSON 序列化（供 SessionDocument 持久化 / MainWindow .pxc 读写）
    // Task 6: from_json 改为静态工厂（返回新对象），消除 View 层经
    // _trigger_widget->get_session() 产出旧 JSON 的旁路；MainWindow load 路径
    // 直接 `set_trigger_config(TriggerConfig::from_json(obj))`。
    QJsonObject to_json() const;
    static TriggerConfig from_json(const QJsonObject& obj);

private:
    Mode                 _mode = Simple;
    int                  _trigger_pos = 0;
    int                  _stage_count = 0;
    std::vector<Stage>   _stages;
    bool                 _adv_enabled = false;
    int                  _adv_tab_index = 0;
    int                  _serial_data_channel = 0;
    int                  _serial_bits = 0;
    QString              _serial_value;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_TRIGGERCONFIG_H
