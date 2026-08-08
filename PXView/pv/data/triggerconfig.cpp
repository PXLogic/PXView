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

#include "pv/data/triggerconfig.h"

namespace pv {
namespace data {

TriggerConfig::TriggerConfig()
{
}

TriggerConfig::~TriggerConfig()
{
}

void TriggerConfig::set_mode(Mode m)
{
    _mode = m;
}

void TriggerConfig::set_trigger_pos(int pos)
{
    _trigger_pos = pos;
}

void TriggerConfig::set_stage_count(int n)
{
    _stage_count = n;
}

void TriggerConfig::set_stages(const std::vector<Stage>& s)
{
    _stages = s;
}

void TriggerConfig::set_adv_enabled(bool e)
{
    _adv_enabled = e;
}

void TriggerConfig::set_adv_tab_index(int idx)
{
    _adv_tab_index = idx;
}

void TriggerConfig::set_serial_data_channel(int ch)
{
    _serial_data_channel = ch;
}

void TriggerConfig::set_serial_bits(int bits)
{
    _serial_bits = bits;
}

void TriggerConfig::set_serial_value(const QString& v)
{
    _serial_value = v;
}

QJsonObject TriggerConfig::to_json() const
{
    QJsonObject obj;
    obj["mode"] = static_cast<int>(_mode);
    obj["trigger_pos"] = _trigger_pos;
    obj["stage_count"] = _stage_count;

    QJsonArray stages_arr;
    for (const Stage& st : _stages) {
        QJsonObject s;
        s["value0"] = st.value0;
        s["value1"] = st.value1;
        s["logic"] = st.logic;
        s["inv0"] = st.inv0;
        s["inv1"] = st.inv1;
        s["count0"] = st.count0;
        s["count1"] = st.count1;
        stages_arr.append(s);
    }
    obj["stages"] = stages_arr;

    obj["adv_enabled"] = _adv_enabled;
    obj["adv_tab_index"] = _adv_tab_index;
    obj["serial_data_channel"] = _serial_data_channel;
    obj["serial_bits"] = _serial_bits;
    obj["serial_value"] = _serial_value;

    return obj;
}

// Task 6: 静态工厂——构造新 TriggerConfig 并从 JSON 填充。
// 供 SessionDocument / MainWindow load 路径使用：
//   `_trigger_config = TriggerConfig::from_json(obj)`
//   `set_trigger_config(TriggerConfig::from_json(obj))`
TriggerConfig TriggerConfig::from_json(const QJsonObject &obj)
{
    TriggerConfig cfg;

    if (obj.contains("mode")) {
        cfg._mode = static_cast<Mode>(
            obj.value("mode").toInt(static_cast<int>(Simple)));
    }
    if (obj.contains("trigger_pos")) {
        cfg._trigger_pos = obj.value("trigger_pos").toInt(0);
    }
    if (obj.contains("stage_count")) {
        cfg._stage_count = obj.value("stage_count").toInt(0);
    }

    if (obj.contains("stages")) {
        QJsonArray stages_arr = obj.value("stages").toArray();
        cfg._stages.clear();
        cfg._stages.reserve(stages_arr.size());
        for (int i = 0; i < stages_arr.size(); ++i) {
            QJsonObject s = stages_arr.at(i).toObject();
            Stage st;
            st.value0 = s.value("value0").toString();
            st.value1 = s.value("value1").toString();
            st.logic = s.value("logic").toInt(0);
            st.inv0 = s.value("inv0").toInt(0);
            st.inv1 = s.value("inv1").toInt(0);
            st.count0 = s.value("count0").toInt(0);
            st.count1 = s.value("count1").toInt(0);
            cfg._stages.push_back(st);
        }
    }

    if (obj.contains("adv_enabled")) {
        cfg._adv_enabled = obj.value("adv_enabled").toBool(false);
    }
    if (obj.contains("adv_tab_index")) {
        cfg._adv_tab_index = obj.value("adv_tab_index").toInt(0);
    }
    if (obj.contains("serial_data_channel")) {
        cfg._serial_data_channel = obj.value("serial_data_channel").toInt(0);
    }
    if (obj.contains("serial_bits")) {
        cfg._serial_bits = obj.value("serial_bits").toInt(0);
    }
    if (obj.contains("serial_value")) {
        cfg._serial_value = obj.value("serial_value").toString();
    }

    return cfg;
}

} // namespace data
} // namespace pv
