/*
 * This file is part of the PulseView project.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2014 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
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

/* __STDC_FORMAT_MACROS is required for PRIu64 and friends (in C++). */
#define __STDC_FORMAT_MACROS

#include "pv/session/storesession.h"
#include "pv/session/sigsession.h"

#include <shared_mutex>

#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/snapshot/dsosnapshot.h"
#include "pv/data/snapshot/analogsnapshot.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/data/decode/decoder.h"
#include "pv/data/decode/row.h"
#include "pv/data/model/signalmodel.h"
#include "pv/view/trace/trace.h"
#include "pv/view/signal/signal.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/view/signal/dsosignal.h"
#include "pv/view/trace/decodetrace.h"
#include "pv/dock/protocoldock.h"
 
#include <QFileDialog>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <cmath>
#include <QTextStream>
#include <list>



#include <libsigrokdecode.h>
#include "pv/config/appconfig.h"
#include "pv/base/pxvdef.h"
#include "pv/utility/encoding.h"
#include "pv/utility/path.h"
#include "pv/base/log.h" 

#include "pv/ui/langresource.h"

#define DEOCDER_CONFIG_VERSION  2
 
namespace pv {

StoreSession::StoreSession(SigSession *session) :
	_session(session),
    _outModule(nullptr)
{ 
    _sessionDataGetter = nullptr;
    _start_index = 0;
    _end_index = 0;
    _is_busy = false;
}

void StoreSession::set_error(const QString &err)
{
    std::lock_guard<std::mutex> lk(_error_mutex);
    _error = err;
}

StoreSession::~StoreSession()
{
	wait();
}

SigSession* StoreSession::session()
{
    return _session;
}

void StoreSession::get_progress(uint64_t *writed, uint64_t *total)
{
    assert(writed);
    assert(total);
    if (!writed || !total) {
        pxv_warn("StoreSession::get_progress called with nullptr out-parameter.");
        return;
    }

    *writed = _units_stored.load();
    *total = _unit_count.load();
}

QString StoreSession::error()
{
    std::lock_guard<std::mutex> lk(_error_mutex);
    return _error;
}

void StoreSession::wait()
{
// Gap 2: future.wait() replaces thread.join().
if (_save_future.valid())
_save_future.wait();
}

void StoreSession::cancel()
{ 
    _canceled = true; 
}

QList<QString> StoreSession::getSuportedExportFormats(){
    const struct sr_output_module** supportedModules = sr_output_list();
    QList<QString> list;
    while(*supportedModules){
        if(*supportedModules == nullptr)
            break;
        // Upstream libsigrok makes sr_output_module opaque — use accessor
        // functions sr_output_id_get() / sr_output_description_get() instead
        // of direct field access (fork libsigrok exposed ->id / ->desc).
        const char *mod_id = sr_output_id_get(*supportedModules);
        const char *mod_desc = sr_output_description_get(*supportedModules);
        // In non-LOGIC modes (DSO/ANALOG/MSO), only CSV is supported.
        // Use 'continue' to skip non-CSV modules instead of 'break' which
        // would abort the entire traversal before reaching the CSV module
        // (CSV is the 4th entry in the output module list, after
        // ascii/binary/bits).
        if (_session->get_device()->get_work_mode() != LOGIC &&
            strcmp(mod_id, "csv") != 0) {
            supportedModules++;
            continue;
        }
        QString format(mod_desc ? mod_desc : "");
        format.append(" (*.");
        format.append(mod_id ? mod_id : "");
        format.append(")");
        list.append(format);
        supportedModules++;
    }
    return list;
}

bool StoreSession::save_start()
{
    assert(_sessionDataGetter);
    if (!_sessionDataGetter) {
        pxv_warn("StoreSession::save_start called with no _sessionDataGetter.");
        set_error(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STORESESS_SAVESTART_ERROR2), "No data to save."));
        return false;
    }

    std::set<int> type_set;
    std::vector<std::shared_ptr<data::SignalModel>> _sm_models = _session->get_signal_models_snapshot(); for(auto m : _sm_models) {
        type_set.insert(m->type());
    }

    if (type_set.size() > 1) {
        // 架构修复：MSO 模式（混合 LOGIC + ANALOG）现在支持保存。
        // 不再拒绝混合类型，save_proc 会分别保存 logic 和 analog 数据块。
        pxv_info("MSO mode: saving mixed data types (%d types).", (int)type_set.size());
    }

    if (type_set.size() == 0) {
        set_error(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STORESESS_SAVESTART_ERROR2), "No data to save."));
        return false;
    }

    if (_file_name == ""){
        set_error(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STORESESS_SAVESTART_ERROR3), "No file name."));
        return false;
    }

    // 架构修复：MSO 模式下，从所有可用类型中找到第一个有数据的 snapshot。
    // 不再只检查 type_set 的第一个类型（可能是空的 logic），而是遍历所有类型。
    data::Snapshot *snapshot = nullptr;
    for (auto t : type_set) {
        auto snap = _session->get_snapshot(t);
        if (snap && !snap->empty()) {
            snapshot = snap;
            break;
        }
    }
    if (!snapshot) {
        pxv_warn("StoreSession::save_start: no snapshot with data found.");
        set_error(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STORESESS_SAVESTART_ERROR2), "No data to save."));
        return false;
    }

    std::string meta_data;
    std::string decoder_data;
    std::string session_data;
    
    meta_gen(snapshot, meta_data);
    decoders_gen(decoder_data);
    _sessionDataGetter->genSessionData(session_data);

    if (meta_data.empty()) {
        set_error(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STORESESS_SAVESTART_ERROR4), "Generate temp file data failed."));
        QFile::remove(_file_name);
        return false;
    }
    if (decoder_data.empty()){
        set_error(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STORESESS_SAVESTART_ERROR5), "Generate decoder file data failed."));
        QFile::remove(_file_name);
        return false;
    }
    if (session_data.empty()){
        set_error(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STORESESS_SAVESTART_ERROR6), "Generate session file data failed."));
        QFile::remove(_file_name);
        return false;
    }
   
    auto _filename = path::ConvertPath(_file_name);
    
    if (m_zipDoc.CreateNew(_filename.c_str(), false))
    {    
        if ( !m_zipDoc.AddFromBuffer("header", meta_data.c_str(), meta_data.size())
            || !m_zipDoc.AddFromBuffer("decoders", decoder_data.c_str(), decoder_data.size())
            || !m_zipDoc.AddFromBuffer("session", session_data.c_str(), session_data.size())
        )
        {
            _has_error.store(true);
            set_error(m_zipDoc.GetError());
        }
        else
        {
            // Gap 2: join previous save if still running
            if (_save_future.valid())
                _save_future.wait();
            // PulseView pattern: set busy flag in the CALLER thread before
            // starting the worker, eliminating the race window.
            _is_busy.store(true);
            _save_future = std::async(std::launch::async,
                &StoreSession::save_proc, this, snapshot);
            return !_has_error.load();
        }
    }
    else{
         set_error(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STORESESS_SAVESTART_ERROR7), "Generate zip file failed."));
    }

    QFile::remove(_file_name);
    return false;
}

void StoreSession::save_logic(pv::data::LogicSnapshot *logic_snapshot)
{
    char chunk_name[20] = {0};
    uint16_t to_save_probes = 0;
    bool sample;
    int ret = SR_ERR;
    int num;

    std::vector<std::shared_ptr<data::SignalModel>> _sm_models = _session->get_signal_models_snapshot(); for(auto m : _sm_models) {
        if (m->enabled() && logic_snapshot->has_data(m->index()))
            to_save_probes++;
    }

    _unit_count.store(logic_snapshot->get_ring_sample_count() / 8 * to_save_probes);
    num = logic_snapshot->get_block_num();

    uint64_t start_index = _start_index;
    uint64_t end_index = _end_index;
    uint64_t start_offset = 0;
    uint64_t end_offset = 0;
    int start_block = 0;
    int end_block = 0;

    if (start_index > logic_snapshot->get_ring_sample_count()){
        pxv_err("ERROR:the start curosr is invalid!");
        _units_stored.store((uint64_t)-1);
        progress_updated();
        return;
    }
    if (end_index > logic_snapshot->get_ring_sample_count()){
        end_index = 0;
    }

    if (start_index > 0){
        // 问题1修复：对齐从 64 改为 8（位图 1bit/样本、8 样本/字节的最小字节对齐）。
        // 64 对齐会在保存范围前多写入最多 63 个前置样本，重载后光标特征整体右移
        // （如 200us 区间变成 256us），用户反馈"导出后再打开光标偏移 240us+"。
        start_index -= start_index % 8;         
        start_block = LogicSnapshot::get_block_with_sample(start_index, &start_offset);
    }
    if (end_index > 0){
        if (end_index % 8 != 0){
            end_index += (8 - end_index % 8);
        }        

        if (end_index > logic_snapshot->get_ring_sample_count()){
            end_index = 0;
        }
        else{
            end_block = LogicSnapshot::get_block_with_sample(end_index, &end_offset);
        }
    }

    if (start_index > 0 && end_index > 0){
        _unit_count.store((end_index - start_index) / 8 * to_save_probes);
    }
    else if (start_index > 0){
        _unit_count.store((logic_snapshot->get_ring_sample_count() - start_index) / 8 * to_save_probes);
    }
    else if (end_index > 0){
        _unit_count.store(end_index / 8 * to_save_probes);
    }

    std::vector<std::shared_ptr<data::SignalModel>> _sm_models2 = _session->get_signal_models_snapshot(); for(auto m : _sm_models2)
    {
        auto ch_type = m->type();
        if (ch_type == SR_CHANNEL_LOGIC) {
            int ch_index = m->index();
            if (!m->enabled() || !logic_snapshot->has_data(ch_index))
                continue;

            for (int i = 0; !_canceled && i < num; i++) 
            {
                if (i < start_block){
                    continue;
                }
                if (i > end_block && end_block > 0){
                    break;
                }

                uint8_t *buf = logic_snapshot->get_block_buf(i, ch_index, sample);
                uint64_t size = logic_snapshot->get_block_size(i);
                bool need_malloc = (buf == nullptr);

                if (i == end_block && end_offset / 8 < size && end_offset > 0){
                    size = end_offset / 8;
                }

                if (i == start_block && start_offset > 0){
                    if (buf != nullptr){
                        buf += start_offset / 8;
                    }
                    size -= start_offset / 8;
                }
                
                if (need_malloc) {
                    buf = (uint8_t *)malloc(size);
                    if (buf == nullptr) {
                        _has_error.store(true);
set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_SAVEPROC_ERROR1),
            "Failed to create zip file. Malloc error."));
                    } else {
                        memset(buf, sample ? 0xff : 0x0, size);
                    }
                }
                
                MakeChunkName(chunk_name, i - start_block, ch_index, (int)ch_type, HEADER_FORMAT_VERSION);
                ret = m_zipDoc.AddFromBuffer(chunk_name, (const char*)buf, size) ? SR_OK : -1;

                if (ret != SR_OK) {
                    if (need_malloc && buf) { free(buf); buf = nullptr; }
                    if (!_has_error.load()) {
                        _has_error.store(true);
set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_SAVEPROC_ERROR2),
            "Failed to create zip file. Please check write permission of this path."));
                    }
                    progress_updated();
                    if (_has_error.load())
                        QFile::remove(_file_name);
                    return;
                }
                _units_stored.fetch_add(size);

                if (_units_stored.load() > _unit_count.load() 
                        && start_index == 0
                        && end_index == 0){
                    pxv_err("Read block data error!");
                    break;
                }

                if (need_malloc)
                    free(buf);
                progress_updated();
            }
        }
    }

    progress_updated();

    // MSO 模式修复：不在此处关闭/释放 zip，由 save_proc 统一处理，
    // 这样 save_logic 结束后 save_analog 仍可向同一 zip 写入数据。
}

void StoreSession::save_analog(pv::data::AnalogSnapshot *analog_snapshot)
{
    char chunk_name[20] = {0};
    int num = 0;
    int ret = SR_ERR;

    // MSO 模式修复：必须显式查找 ANALOG 类型的 model，
    // 否则当 signal_models 第一个是 LOGIC 时会错误地使用 SR_CHANNEL_LOGIC 作为 ch_type，
    // 导致 MakeChunkName 生成的 chunk name 与 save_logic 生成的重名（如 L-0/0），
    // AddFromBuffer 因此返回失败，触发 "Failed to create zip file" 错误。
    int ch_type = -1;
    std::vector<std::shared_ptr<data::SignalModel>> _sm_models = _session->get_signal_models_snapshot(); for(auto m : _sm_models) {
        if (m->type() == SR_CHANNEL_ANALOG) {
            ch_type = (int)m->type();
            break;
        }
    }
    if (ch_type == -1) {
        // 兜底：函数本身只处理 analog snapshot，类型固定为 ANALOG
        ch_type = SR_CHANNEL_ANALOG;
    }

    if (ch_type != -1) {
        num = analog_snapshot->get_block_num();
_unit_count.store(analog_snapshot->get_sample_count() *
                    analog_snapshot->get_unit_bytes() *
                    analog_snapshot->get_channel_num());
        uint8_t *buf = nullptr;
        uint8_t *buf_start = nullptr;

        buf = (uint8_t *)analog_snapshot->get_data() +
                        (analog_snapshot->get_ring_start() * analog_snapshot->get_unit_bytes()
                                         * analog_snapshot->get_channel_num());

        buf_start = (uint8_t *)analog_snapshot->get_data();

        const uint8_t *buf_end = buf_start + _unit_count.load();

        for (int i = 0; !_canceled && i < num; i++) {
            const uint64_t size = analog_snapshot->get_block_size(i);
            if ((buf + size) > buf_end) {
                uint8_t *tmp = (uint8_t *)malloc(size);
                if (tmp == nullptr) {
                    _has_error.store(true);
set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_SAVEPROC_ERROR1),
            "Failed to create zip file. Malloc error."));
                } else {
                    memcpy(tmp, buf, buf_end-buf);
                    memcpy(tmp+(buf_end-buf), buf_start, buf+size-buf_end);
                } 

                MakeChunkName(chunk_name, i, 0, ch_type, HEADER_FORMAT_VERSION);
                ret = m_zipDoc.AddFromBuffer(chunk_name, (const char*)tmp, size) ? SR_OK : -1;

                /* Wrap-around: buf should now point to the start of the
                 * wrapped data in buf_start. The number of bytes that
                 * wrapped past buf_end is (buf + size - buf_end).
                 * Previous code used `buf += (size - _unit_count)` which
                 * caused unsigned underflow (size < _unit_count normally),
                 * making buf point to invalid memory. */
                buf = buf_start + (buf + size - buf_end);
                if (tmp)
                    free(tmp);
            } 
            else { 
                MakeChunkName(chunk_name, i, 0, ch_type, HEADER_FORMAT_VERSION);
                ret = m_zipDoc.AddFromBuffer(chunk_name, (const char*)buf, size) ? SR_OK : -1;

                buf += size;
            }

            if (ret != SR_OK) {
                if (!_has_error.load()) {
                    _has_error.store(true);
set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_SAVEPROC_ERROR2),
            "Failed to create zip file. Please check write permission of this path."));
                }
                progress_updated();
                if (_has_error.load())
                    QFile::remove(_file_name);
                return;
            }
            _units_stored.fetch_add(size);
            progress_updated();
        }
    }

    progress_updated();

    // MSO 模式修复：不在此处关闭/释放 zip，由 save_proc 统一处理。
}

void StoreSession::save_dso(pv::data::DsoSnapshot *dso_snapshot)
{
    char chunk_name[20] = {0};
    int ret = SR_ERR; 
 
    uint64_t size = dso_snapshot->get_sample_count();
    int ch_num = dso_snapshot->get_channel_num();
    _unit_count.store(size * ch_num);

    std::vector<std::shared_ptr<data::SignalModel>> _sm_models = _session->get_signal_models_snapshot(); for(auto m : _sm_models)
    {
        if (m->type() == SR_CHANNEL_DSO) {
            int ch_index = m->index();

            if (!dso_snapshot->has_data(ch_index))
                continue;

            if (_canceled)
                break;

            const uint8_t *data_buffer = dso_snapshot->get_samples(0, 0, ch_index);
        
            snprintf(chunk_name, 19, "O-%d/0", ch_index);
            ret = m_zipDoc.AddFromBuffer(chunk_name, (const char*)data_buffer, size) ? SR_OK : -1;

            if (ret != SR_OK) {
                if (!_has_error.load()) {
                    _has_error.store(true);
set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_SAVEPROC_ERROR2),
            "Failed to create zip file. Please check write permission of this path."));
                }
                progress_updated();
                if (_has_error.load())
                    QFile::remove(_file_name);
                return;
            }

            _units_stored.fetch_add(size);
            progress_updated();
        }
    }

    progress_updated();

    if (_canceled || size == 0 || ch_num == 0){
        // 无数据或被取消时不在此处关闭 zip，由 save_proc 统一处理
    }

    // MSO 模式修复：不在此处关闭/释放 zip，由 save_proc 统一处理。
}

void StoreSession::save_proc(data::Snapshot *snapshot)
{
	assert(snapshot);
    if (!snapshot) {
        pxv_warn("StoreSession::save_proc called with nullptr snapshot.");
        return;
    }

    // _is_busy is now set by save_start() in the caller thread before
    // spawning this thread, so there's no race window.

    pxv_info("save task start.");

    // 架构修复：MSO 模式支持。不再只保存单个 snapshot 类型，
    // 而是分别检查并保存所有可用类型（logic + analog）。
    data::LogicSnapshot *logic_snapshot = _session->get_snapshot(SR_CHANNEL_LOGIC) ?
        dynamic_cast<data::LogicSnapshot*>(_session->get_snapshot(SR_CHANNEL_LOGIC)) : nullptr;
    data::AnalogSnapshot *analog_snapshot = _session->get_snapshot(SR_CHANNEL_ANALOG) ?
        dynamic_cast<data::AnalogSnapshot*>(_session->get_snapshot(SR_CHANNEL_ANALOG)) : nullptr;
    data::DsoSnapshot *dso_snapshot = _session->get_snapshot(SR_CHANNEL_DSO) ?
        dynamic_cast<data::DsoSnapshot*>(_session->get_snapshot(SR_CHANNEL_DSO)) : nullptr;

    // 保存传入的 snapshot 对应类型的数据（保持向后兼容）
    logic_snapshot = dynamic_cast<data::LogicSnapshot*>(snapshot);
    if (logic_snapshot) {
        save_logic(logic_snapshot);
    }
    else {
        analog_snapshot = dynamic_cast<data::AnalogSnapshot*>(snapshot);
        if (analog_snapshot) {
            save_analog(analog_snapshot);
        }
        else {
            dso_snapshot = dynamic_cast<data::DsoSnapshot*>(snapshot);
            if (dso_snapshot) {
                save_dso(dso_snapshot);
            }
        }
    }

    // MSO 模式：如果传入的是 logic，但还有 analog 数据，也一并保存
    if (dynamic_cast<data::LogicSnapshot*>(snapshot) && !_canceled && !_has_error.load()) {
        auto analog = _session->get_snapshot(SR_CHANNEL_ANALOG);
        if (analog && !analog->empty()) {
            auto analog_snap = dynamic_cast<data::AnalogSnapshot*>(analog);
            if (analog_snap)
                save_analog(analog_snap);
        }
    }
    // MSO 模式：如果传入的是 analog，但还有 logic 数据，也一并保存
    if (dynamic_cast<data::AnalogSnapshot*>(snapshot) && !_canceled && !_has_error.load()) {
        auto logic = _session->get_snapshot(SR_CHANNEL_LOGIC);
        if (logic && !logic->empty()) {
            auto logic_snap = dynamic_cast<data::LogicSnapshot*>(logic);
            if (logic_snap)
                save_logic(logic_snap);
        }
    }
 
    pxv_info("save task end.");

    // MSO 模式修复：统一在此处关闭/释放 zip 文件。
    // 此前 save_logic/save_analog/save_dso 各自末尾都调用 Close/Release，
    // 导致 MSO 模式下第一个 save 函数关闭 zip 后，后续 save 函数的 AddFromBuffer 必然失败，
    // 触发 "Failed to create zip file. Please check write permission of this path." 错误。
    if (_canceled.load() || _has_error.load()) {
        QFile::remove(_file_name);
    }
    else {
        bool bret = m_zipDoc.Close();
        m_zipDoc.Release();
        if (!bret) {
            _has_error.store(true);
            set_error(m_zipDoc.GetError());
            QFile::remove(_file_name);
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    _is_busy = false;
}

bool StoreSession::meta_gen(data::Snapshot *snapshot, std::string &str)
{
    GSList *l;
    struct sr_channel *probe;
    char *s;
    char meta[300] = {0};
  
    sprintf(meta, "%s", "[version]\n"); str += meta;
    sprintf(meta, "version = %d\n", HEADER_FORMAT_VERSION); str += meta;
    sprintf(meta, "%s", "[header]\n"); str += meta;

    int mode = _session->get_device()->get_work_mode();

    if (true) {
        sprintf(meta, "driver = %s\n", _session->get_device()->driver_name().toLocal8Bit().data()); str += meta;
        sprintf(meta, "device mode = %d\n", mode); str += meta;
    }
 
    // 光标范围保存：header 中的 "total samples" 必须与实际写入文件的数据长度一致，
    // 否则加载时 get_sample_limit() 使用整个捕获长度，而 .pxl 内实际数据只有光标
    // 范围，导致视图时间轴/光标/触发标记错位（用户反馈的"范围异常/数据异常"）。
    // 同时 "trigger pos" 要按保存起点偏移，使触发标记在重载后仍对齐到正确采样点。
    // 此处与 save_logic() 使用相同的 64 采样对齐规则，避免二者口径漂移。
    uint64_t saved_samples = snapshot->get_sample_count();
    uint64_t saved_trig_pos = _session->get_trigger_pos();
    data::LogicSnapshot *logic_snap_for_meta =
        dynamic_cast<data::LogicSnapshot*>(snapshot);
    if (logic_snap_for_meta) {
        uint64_t ring = logic_snap_for_meta->get_ring_sample_count();
        uint64_t start_index = _start_index;
        uint64_t end_index = _end_index;
        if (start_index > ring)
            start_index = 0;
        if (end_index > ring)
            end_index = 0;
        if (start_index > 0)
            // 问题1修复：对齐从 64 改为 8，与 save_logic 保持一致（见上）。
            start_index -= start_index % 8;
        if (end_index > 0) {
            if (end_index % 8 != 0)
                end_index += 8 - end_index % 8;
            if (end_index > ring)
                end_index = 0;
        }
        if (start_index > 0 && end_index > 0) {
            saved_samples = end_index - start_index;
            if (saved_trig_pos >= start_index)
                saved_trig_pos -= start_index;
            else
                saved_trig_pos = 0;
            if (saved_trig_pos > saved_samples)
                saved_trig_pos = saved_samples;
        } else if (start_index > 0) {
            saved_samples = ring - start_index;
            if (saved_trig_pos >= start_index)
                saved_trig_pos -= start_index;
            else
                saved_trig_pos = 0;
        } else if (end_index > 0) {
            saved_samples = end_index;
            if (saved_trig_pos > end_index)
                saved_trig_pos = end_index;
        }
    }

    sprintf(meta, "capturefile = data\n"); str += meta;
    sprintf(meta, "total samples = %" PRIu64 "\n", saved_samples); str += meta;

    // MSO 架构修复：按通道类型分别统计 logic/analog 通道数。
    // session_file.c 解析时：total probes → 创建 SR_CHANNEL_LOGIC，
    // total analog → 创建 SR_CHANNEL_ANALOG。若不区分，所有通道都会被当成 logic。
    int logic_count = 0, analog_count = 0;
    for (l = _session->get_device()->get_channels(); l; l = l->next) {
        probe = (struct sr_channel *)l->data;
        if (probe->type == SR_CHANNEL_LOGIC)
            logic_count++;
        else if (probe->type == SR_CHANNEL_ANALOG || probe->type == SR_CHANNEL_DSO)
            analog_count++;
    }

    data::LogicSnapshot *logic_snapshot = nullptr;
    logic_snapshot = dynamic_cast<data::LogicSnapshot*>(snapshot);
    if (logic_snapshot) {
        uint16_t to_save_probes = 0;
        for (l = _session->get_device()->get_channels(); l; l = l->next) {
            probe = (struct sr_channel *)l->data;
            if (probe->enabled && probe->type == SR_CHANNEL_LOGIC
                && logic_snapshot->has_data(probe->index))
                to_save_probes++;
        }

        int block_count = logic_snapshot->get_block_num();

        uint64_t start_index = _start_index;
        uint64_t end_index = _end_index;
        uint64_t start_offset = 0;
        uint64_t end_offset = 0;
        int start_block = 0;
        int end_block = 0;

        if (end_index > logic_snapshot->get_ring_sample_count()){
            end_index = 0;
        }
        if (start_index > 0){
            start_block = LogicSnapshot::get_block_with_sample(start_index, &start_offset);
        }
        if (end_index > 0){
            end_block = LogicSnapshot::get_block_with_sample(end_index, &end_offset);
        }

        if (start_index > 0 && end_index > 0){
            block_count = end_block - start_block + 1;
        }
        else if (start_index > 0){
            block_count = block_count - start_block;
        }
        else if (end_index > 0){
            block_count = end_block + 1;
        }

        sprintf(meta, "total probes = %d\n", to_save_probes); str += meta;
        sprintf(meta, "total blocks = %d\n", block_count); str += meta;
    }
    else {
        // 非 LOGIC 模式（ANALOG/DSO）：logic_count 可能为 0，analog_count > 0
        sprintf(meta, "total probes = %d\n", logic_count); str += meta;
        sprintf(meta, "total blocks = %d\n", snapshot->get_block_num()); str += meta;
    }

    // MSO 架构修复：写入 total analog，让 session_file.c 创建 SR_CHANNEL_ANALOG 通道。
    if (analog_count > 0) {
        sprintf(meta, "total analog = %d\n", analog_count); str += meta;
    }

    s = sr_samplerate_string(_session->cur_snap_samplerate());

    sprintf(meta, "samplerate = %s\n", s); str += meta;

    uint64_t tmp_u64;
    int tmp_u8;
    uint32_t tmp_u32;

    if (mode == DSO) {
        if (_session->get_device()->get_config_uint64(SR_CONF_TIMEBASE, tmp_u64)) {
            sprintf(meta, "hDiv = %" PRIu64 "\n", tmp_u64); str += meta;
        }

        if (_session->get_device()->get_config_byte(SR_CONF_UNIT_BITS, tmp_u8)) {
            sprintf(meta, "bits = %d\n", tmp_u8); str += meta;
        }
 
        if (_session->get_device()->get_config_uint32(SR_CONF_REF_MIN, tmp_u32)) {
            sprintf(meta, "ref min = %d\n", tmp_u32); str += meta;
        }

        if (_session->get_device()->get_config_uint32(SR_CONF_REF_MAX, tmp_u32)) {
            sprintf(meta, "ref max = %d\n", tmp_u32); str += meta;
        }
    }
    else if (mode == ANALOG) {
        data::AnalogSnapshot *analog_snapshot = nullptr;
        analog_snapshot = dynamic_cast<data::AnalogSnapshot*>(snapshot);
        if (analog_snapshot) {
            uint8_t tmp_u8 = analog_snapshot->get_unit_bytes();
            sprintf(meta, "bits = %d\n", tmp_u8*8); str += meta;
        }

        if (_session->get_device()->get_config_uint32(SR_CONF_REF_MIN, tmp_u32)) {
            sprintf(meta, "ref min = %d\n", tmp_u32); str += meta;
        }

        if (_session->get_device()->get_config_uint32(SR_CONF_REF_MAX, tmp_u32)) {
            sprintf(meta, "ref max = %d\n", tmp_u32); str += meta;
        }
    }
    sprintf(meta, "trigger pos = %" PRIu64 "\n", saved_trig_pos); str += meta;

    /* trigger time: written in ALL modes (not just LOGIC) so the frontend
     * can restore the original capture timestamp when reopening a .pxl file.
     * Format: milliseconds since Unix epoch (int64). */
    sprintf(meta, "trigger time = %lld\n", (long long)_session->get_session_time().toMSecsSinceEpoch()); str += meta;

    int analogcnt = 0;

    // MSO 架构修复：meta_gen 传入的 snapshot 可能是 LogicSnapshot，
    // 其 has_data() 对模拟通道返回 false，导致 analog0/analog1/... 键
    // 从不被写入 header。加载时模拟通道因此保持未命名/未启用状态。
    // 修复：对模拟通道检查 AnalogSnapshot 而非传入的 snapshot。
    data::AnalogSnapshot *analog_snap_for_has_data = nullptr;
    if (mode == MSO) {
        auto snap_a = _session->get_snapshot(SR_CHANNEL_ANALOG);
        if (snap_a && !snap_a->empty())
            analog_snap_for_has_data = dynamic_cast<data::AnalogSnapshot*>(snap_a);
    }

    for (l = _session->get_device()->get_channels(); l; l = l->next) {
        
        probe = (struct sr_channel *)l->data;
        
        // MSO 模式：对模拟通道检查 AnalogSnapshot，而非传入的 LogicSnapshot
        gboolean is_logic = (probe->type == SR_CHANNEL_LOGIC);
        if (mode == MSO && !is_logic && analog_snap_for_has_data) {
            if (!analog_snap_for_has_data->has_data(probe->index))
                continue;
        } else {
            if (!snapshot->has_data(probe->index))
                continue;
        }

        if (mode == LOGIC && !probe->enabled)
            continue;
        if (probe->name)
        {
            if (is_logic) {
                sprintf(meta, "probe%d = %s\n", probe->index, probe->name);
            } else {
                sprintf(meta, "analog%d = %s\n", analogcnt, probe->name);
            }
            str += meta;
        }

        // Find matching SignalModel by probe->index for fork field replacements.
        std::shared_ptr<data::SignalModel> matched_model;
        std::vector<std::shared_ptr<data::SignalModel>> _sm_models = _session->get_signal_models_snapshot(); for(auto m : _sm_models) {
            if (m && m->index() == probe->index) {
                matched_model = m;
                break;
            }
        }

        if (mode == DSO)
        {
            /* DSO/ANALOG per-channel fields use analogcnt (not probecnt)
             * because probecnt only counts logic channels and stays 0
             * in DSO/ANALOG mode, causing all channels to write to
             * index 0 and overwrite each other. */
            sprintf(meta, " enable%d = %d\n", analogcnt, probe->enabled);
            str += meta;
            int coupling = matched_model ? matched_model->coupling() : 0;
            double vdiv = matched_model ? matched_model->vdiv() : 0;
            double vfactor = matched_model ? matched_model->vfactor() : 1;
            double hw_offset = matched_model ? matched_model->hw_offset() : 0;
            double trig_value = matched_model ? matched_model->trig_value() : 0;
            sprintf(meta, " coupling%d = %d\n", analogcnt, coupling);
            str += meta;
            sprintf(meta, " vDiv%d = %" PRIu64 "\n", analogcnt, (uint64_t)vdiv);
            str += meta;
            sprintf(meta, " vFactor%d = %" PRIu64 "\n", analogcnt, (uint64_t)vfactor);
            str += meta;
            sprintf(meta, " vOffset%d = %d\n", analogcnt, (int)hw_offset);
            str += meta;
            sprintf(meta, " vTrig%d = %d\n", analogcnt, (int)trig_value);
            str += meta;
        }
        else if (mode == ANALOG)
        {
            sprintf(meta, " enable%d = %d\n", analogcnt, probe->enabled);
            str += meta;
            int coupling = matched_model ? matched_model->coupling() : 0;
            double vdiv = matched_model ? matched_model->vdiv() : 0;
            double hw_offset = matched_model ? matched_model->hw_offset() : 0;
            sprintf(meta, " coupling%d = %d\n", analogcnt, coupling);
            str += meta;
            sprintf(meta, " vDiv%d = %" PRIu64 "\n", analogcnt, (uint64_t)vdiv);
            str += meta;
            sprintf(meta, " vOffset%d = %d\n", analogcnt, (int)hw_offset);
            str += meta;
            sprintf(meta, " mapUnit%d = %s\n", analogcnt, "");
            str += meta;
            sprintf(meta, " mapMax%d = %lf\n", analogcnt, 0.0);
            str += meta;
            sprintf(meta, " mapMin%d = %lf\n", analogcnt, 0.0);
            str += meta;
        }

        if (!is_logic)
            analogcnt++;
    }

    // MSO 架构修复：写入模拟数据格式信息，供 session_driver 读取。
    data::AnalogSnapshot *analog_snap_for_meta = nullptr;
    auto snap_analog = _session->get_snapshot(SR_CHANNEL_ANALOG);
    if (snap_analog && !snap_analog->empty()) {
        analog_snap_for_meta = dynamic_cast<data::AnalogSnapshot*>(snap_analog);
    }
    if (analog_snap_for_meta) {
        sprintf(meta, "analog bytes = %d\n", analog_snap_for_meta->get_unit_bytes());
        str += meta;
        sprintf(meta, "analog float = %d\n", analog_snap_for_meta->is_float() ? 1 : 0);
        str += meta;
    }

    return true;
}

//export as csv file
bool StoreSession::export_start()
{
    std::set<int> type_set;
    std::vector<std::shared_ptr<data::SignalModel>> _sm_models = _session->get_signal_models_snapshot(); for(auto m : _sm_models) {
        if (!_export_channels.empty()) {
            if (std::find(_export_channels.begin(), _export_channels.end(), m->index()) == _export_channels.end()) {
                continue;
            }
        } else if (_export_channel_type >= 0 && (int)m->type() != _export_channel_type) {
            continue;
        }
        int _tp = m->type();
        type_set.insert(_tp);
    }

    if (type_set.size() > 1) {
set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_EXPORTSTART_ERROR1),
                "PXView does not currently support\nfile export for multiple data types."));
        return false;
    } else if (type_set.size() == 0) {
        set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_EXPORTSTART_ERROR2), "No data to save."));
        return false;
    }

    const auto snapshot = _session->get_snapshot(*type_set.begin());
    if (!snapshot) {
        // Don't dereference a nullptr snapshot (the original `assert(snapshot)`
        // is a no-op in Release builds and would crash on the next line).
        set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_EXPORTSTART_ERROR2), "No data to save."));
        return false;
    }
    // Check we have data
    if (snapshot->empty()) {
        set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_EXPORTSTART_ERROR2), "No data to save."));
        return false;
    }

    if (_file_name == ""){
        set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_EXPORTSTART_ERROR3), "No set file name."));
        return false;
    }

    const struct sr_output_module **supportedModules = sr_output_list();
    while (*supportedModules)
    {
        if (*supportedModules == nullptr)
            break;
        // Upstream libsigrok makes sr_output_module opaque — use sr_output_id_get()
        // instead of direct field access (fork libsigrok exposed ->id).
        const char *mod_id = sr_output_id_get(*supportedModules);
        if (mod_id && !strcmp(mod_id, _suffix.toUtf8().data()))
        {
            _outModule = *supportedModules;
            break;
        }
        supportedModules++;
    }

    if (_outModule == nullptr)
    {
        // Preserve the error message — the previous code fell through to
        // `_error.clear(); return false;` here, which wiped the "Invalid
        // export format" message set just above and left callers with an
        // empty error string. Return immediately so the message survives.
        set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_EXPORTSTART_ERROR4), "Invalid export format."));
        return false;
    }

    // Gap 2: join previous export if still running
    if (_save_future.valid())
        _save_future.wait();
    _is_busy.store(true);
    _save_future = std::async(std::launch::async,
        &StoreSession::export_proc, this, snapshot);
    return !_has_error.load();
}

void StoreSession::export_proc(data::Snapshot *snapshot)
{
    // _is_busy is now set by export_start() in the caller thread before
    // spawning this thread, so there's no race window.

    pxv_info("export task start.");

    export_exec(snapshot);

    pxv_info("export task end.");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    _is_busy = false;
}

void StoreSession::export_exec(data::Snapshot *snapshot)
{
    assert(snapshot);
    if (!snapshot) {
        pxv_warn("StoreSession::export_exec called with nullptr snapshot.");
        _has_error.store(true);
        set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_EXPORTSTART_ERROR2), "No data to save."));
        return;
    }

    // Fork sr_datafeed_packet.bExportOriginalData field removed in upstream
    // libsigrok — "export original data" flag is no longer carried per-packet.
    // AppConfig::appOptions.originalData is still respected by other paths.

    data::LogicSnapshot *logic_snapshot = nullptr;
    data::AnalogSnapshot *analog_snapshot = nullptr;
    data::DsoSnapshot *dso_snapshot = nullptr;
    int channel_type;

    logic_snapshot = dynamic_cast<data::LogicSnapshot*>(snapshot);
    if (logic_snapshot) {
        channel_type = SR_CHANNEL_LOGIC;
    } else {
        dso_snapshot = dynamic_cast<data::DsoSnapshot*>(snapshot);
        if (dso_snapshot) {
            channel_type = SR_CHANNEL_DSO;
        } else {
            analog_snapshot = dynamic_cast<data::AnalogSnapshot*>(snapshot);
            if (analog_snapshot) {
                channel_type = SR_CHANNEL_ANALOG;
            } else {
                _has_error.store(true);
                set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_EXPORTPROC_ERROR1), "data type don't support."));
                return;
            }
        }
    }

    // sr_output_new() takes a filename parameter separately (4th arg) and
    // stores it in op->filename.  The params hash table is ONLY for module
    // options declared via sr_output_module.options().  Putting 'filename' or
    // 'type' in the hash table causes sr_output_new() to reject them as
    // unknown options and return NULL — which previously caused a silent
    // failure (no file created, no error reported).
    GHashTable *params = g_hash_table_new(g_str_hash, g_str_equal);

    // Upstream libsigrok makes sr_output opaque — use sr_output_new() to create
    // an instance instead of stack-allocating and manually assigning fields
    // (fork libsigrok exposed module/sdi/param/start_sample_index on sr_output).
    // sr_output_new() calls the module's init handler internally.
    const struct sr_output *output = sr_output_new(_outModule, params,
                                                   _session->get_device()->inst(),
                                                   _file_name.toUtf8().data());
    if (!output) {
        pxv_err("Failed to init export module (sr_output_new returned nullptr).");
        _has_error.store(true);
        set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_EXPORTSTART_ERROR4),
                     "Failed to init export module."));
        g_hash_table_destroy(params);
        return;
    }

    struct ChannelStateRestorer {
        GSList *channels;
        std::vector<bool> original_states;
        ChannelStateRestorer(GSList *channels, const std::vector<int32_t> &export_channels) : channels(channels) {
            if (channels) {
                for (GSList *l = channels; l; l = l->next) {
                    struct sr_channel *ch = (struct sr_channel *)l->data;
                    original_states.push_back(ch->enabled);
                    if (!export_channels.empty() && std::find(export_channels.begin(), export_channels.end(), ch->index) == export_channels.end()) {
                        ch->enabled = FALSE;
                    }
                }
            }
        }
        ~ChannelStateRestorer() {
            if (channels) {
                size_t i = 0;
                for (GSList *l = channels; l; l = l->next) {
                    struct sr_channel *ch = (struct sr_channel *)l->data;
                    if (i < original_states.size()) {
                        ch->enabled = original_states[i++];
                    }
                }
            }
        }
    } restorer(_session->get_device()->get_channels(), _export_channels);

    // Binary output format must be written as raw bytes — using QTextStream
    // or QString::fromUtf8() corrupts binary data in three ways:
    //   1. QString::fromUtf8(str) without length stops at the first 0x00 byte
    //      (C-string convention), truncating the output.
    //   2. Invalid UTF-8 sequences in binary data get replaced/dropped.
    //   3. QIODevice::Text on Windows converts 0x0A to 0x0D 0x0A, inserting
    //      spurious bytes that corrupt the unitsize alignment.
    // For binary format, open in raw mode and use file.write() directly.
    bool is_binary_output = (_suffix == "binary");

    QFile file(_file_name);
    if (!file.open(QIODevice::WriteOnly | (is_binary_output ? QIODevice::OpenModeFlag(0) : QIODevice::Text))) {
        pxv_err("Failed to open export file: %s", _file_name.toUtf8().data());
        _has_error.store(true);
        set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_EXPORTSTART_ERROR3),
                     "Failed to open export file."));
        sr_output_free(output);
        g_hash_table_destroy(params);
        return;
    }
    QTextStream out(&file);
    encoding::set_utf8(out);
    //out.setGenerateByteOrderMark(true);  // UTF-8 without BOM

    // Meta
    GString *data_out;
    struct sr_datafeed_packet p;
    struct sr_datafeed_meta meta;
    struct sr_config *src;

    src = _session->get_device()->new_config(SR_CONF_SAMPLERATE,
                g_variant_new_uint64(_session->cur_snap_samplerate()));

    meta.config = g_slist_append(nullptr, src);

    src = _session->get_device()->new_config(SR_CONF_LIMIT_SAMPLES,
                g_variant_new_uint64(snapshot->get_sample_count()));

    meta.config = g_slist_append(meta.config, src);

    GVariant *gvar;
    int bits=0;

    _session->get_device()->get_config_byte(SR_CONF_UNIT_BITS, bits);

    gvar = _session->get_device()->get_config(SR_CONF_REF_MIN);
    if (gvar != nullptr) {
        src = _session->get_device()->new_config(SR_CONF_REF_MIN, gvar);
        g_variant_unref(gvar);
    }
    else {
        src = _session->get_device()->new_config(SR_CONF_REF_MIN, g_variant_new_uint32(1));
    }

    meta.config = g_slist_append(meta.config, src);

    gvar = _session->get_device()->get_config(SR_CONF_REF_MAX);
    if (gvar != nullptr) {
        src = _session->get_device()->new_config(SR_CONF_REF_MAX, gvar);
        g_variant_unref(gvar);
    }
    else {
        src = _session->get_device()->new_config(SR_CONF_REF_MAX, g_variant_new_uint32((1 << bits) - 1));
    }
    meta.config = g_slist_append(meta.config, src);

    // Fork sr_datafeed_packet.status / bExportOriginalData fields removed in
    // upstream libsigrok — only type and payload remain.
    p.type = SR_DF_META;
    p.payload = &meta;
    sr_output_send(output, &p, &data_out);

    if(data_out){
        if (is_binary_output)
            file.write(data_out->str, data_out->len);
        else
            out << QString::fromUtf8((char*) data_out->str);
        g_string_free(data_out,TRUE);
    }
    for (GSList *l = meta.config; l; l = l->next) {
        src = (struct sr_config *)l->data;
        _session->get_device()->free_config(src);
    }
    g_slist_free(meta.config);

    if (channel_type == SR_CHANNEL_LOGIC) {
        // Use get_sample_count() (the true captured length) rather than
        // get_ring_sample_count(), which can be short by up to one byte's
        // worth of samples. This keeps the saved byte count consistent with
        // the binary export path and makes the load→save round-trip lossless.
        _unit_count.store(logic_snapshot->get_sample_count());
        int blk_num = logic_snapshot->get_block_num();
        bool sample;
        std::vector<uint8_t *> buf_vec;
        std::vector<bool> buf_sample;

        uint64_t start_index = _start_index;
        uint64_t end_index = _end_index;
        uint64_t start_offset = 0;
        uint64_t end_offset = 0;
        int start_block = 0;
        int end_block = 0;

        if (start_index > logic_snapshot->get_ring_sample_count()){
            pxv_err("ERROR:the start curosr is invalid!");
            _units_stored.store((uint64_t)-1);
            progress_updated();
            return;
        }
        if (end_index > logic_snapshot->get_ring_sample_count()){
            end_index = 0;
        }

        if (start_index > 0){
            start_block = LogicSnapshot::get_block_with_sample(start_index, &start_offset);
        }
        if (end_index > 0){
            end_block = LogicSnapshot::get_block_with_sample(end_index, &end_offset);
        }

        if (start_index > 0 && end_index > 0){
            _unit_count.store((end_index - start_index));
        }
        else if (start_index > 0){
            _unit_count.store((logic_snapshot->get_ring_sample_count() - start_index));
        }
        else if (end_index > 0){
            _unit_count.store(end_index);
        }

        // Total bytes that must be written per channel (= ceil(unit_count/8)).
        // get_block_size() floors to whole bytes, so a trailing partial byte in
        // the last block would be dropped, shifting every following block on
        // reload and corrupting the round-trip. Clamp the final block to the
        // exact remaining byte count.
        uint64_t total_out_bytes = (_unit_count.load() + 7) / 8;
        uint64_t written_bytes = 0;

        for (int blk = 0; !_canceled  &&  blk < blk_num; blk++) {
            // 先做范围过滤，跳过不需要导出的块（避免被跳过的块仍计入 written_bytes）
            if (blk < start_block)
                continue;
            if (blk > end_block && end_block > 0)
                break;

            uint64_t blk_bytes = logic_snapshot->get_block_size(blk);
            uint64_t block_samples = blk_bytes * 8;

            // 光标偏移处理：起始块跳过 start_offset 个采样，结束块截断到 end_offset。
            // 与 RLE 时代 7a635336 的 actual_start/actual_end 语义对齐（raw 版为采样级）。
            uint64_t exp_start = 0;
            if (blk == start_block && start_offset > 0)
                exp_start = start_offset;
            uint64_t exp_end = block_samples - 1;
            if (blk == end_block && end_block > 0 && end_offset > 0)
                exp_end = (end_offset < block_samples) ? end_offset : block_samples - 1;
            if (exp_start > exp_end)
                continue;

            uint64_t buf_sample_num = exp_end - exp_start + 1;
            if (written_bytes + (buf_sample_num + 7) / 8 > total_out_bytes) {
                // Last (partial) block: write only the remaining valid bytes.
                uint64_t remain = total_out_bytes - written_bytes;
                if (remain == 0)
                    break;
                buf_sample_num = remain * 8;
            }
            written_bytes += (buf_sample_num + 7) / 8;
            buf_vec.clear();
            buf_sample.clear();

            std::vector<std::shared_ptr<data::SignalModel>> _sm_models = _session->get_signal_models_snapshot(); for(auto m : _sm_models) {
                if (!_export_channels.empty() && std::find(_export_channels.begin(), _export_channels.end(), m->index()) == _export_channels.end()) {
                    continue;
                }
                auto ch_type = m->type();
                if (ch_type == SR_CHANNEL_LOGIC) {
                    int ch_index = m->index();
                    if (!logic_snapshot->has_data(ch_index))
                        continue;
                    uint8_t *buf = logic_snapshot->get_block_buf(blk, ch_index, sample);
                    buf_vec.push_back(buf);
                    buf_sample.push_back(sample);
                }
            }

            uint16_t unitsize = ceil(buf_vec.size() / 8.0);
            unsigned int usize = 8192;
            unsigned int size = usize;
            struct sr_datafeed_logic lp;

            for(uint64_t i = 0; !_canceled && i < buf_sample_num; i+=usize){
                if(buf_sample_num - i < usize)
                    size = buf_sample_num - i;
                uint8_t *xbuf = (uint8_t *)malloc((size_t)size * unitsize);
                if (xbuf == nullptr) {
                    _has_error.store(true);
                    set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_EXPORTPROC_ERROR2), "xbuffer malloc failed."));
                    return;
                }                
                memset(xbuf, 0, (size_t)size * unitsize);

                for (uint64_t j = 0; j < size; j++) {
                    for (unsigned int k = 0; k < buf_vec.size(); k++) {
                        if (buf_vec[k] == nullptr && buf_sample[k])
                            xbuf[j*unitsize+k/8] +=  1 << k%8;
                        else if (buf_vec[k] && (buf_vec[k][(exp_start+i+j)/8] & (1 << (exp_start+i+j)%8)))
                            xbuf[j*unitsize+k/8] +=  1 << k%8;
                    }
                }

                lp.data = xbuf;
                lp.length = (uint64_t)size * unitsize;
                lp.unitsize = unitsize;
                p.type = SR_DF_LOGIC;
                p.payload = &lp;
                sr_output_send(output, &p, &data_out);

                if(data_out){
                    if (is_binary_output)
                        file.write(data_out->str, data_out->len);
                    else
                        out << QString::fromUtf8((char*) data_out->str);
                    g_string_free(data_out,TRUE);
                }

                _units_stored.fetch_add(size);
                if (xbuf)
                    free(xbuf);
                progress_updated();
            }
        }
    }
    else if (channel_type == SR_CHANNEL_DSO) {
        _unit_count.store(snapshot->get_sample_count()); 
        unsigned int usize = 8192;
        unsigned int size = usize;
        struct sr_datafeed_dso dp;

        uint8_t *ch_data_buffer = (uint8_t*)malloc(usize * dso_snapshot->get_channel_num() + 1);
        if (ch_data_buffer == nullptr){
            pxv_err("StoreSession::export_proc, malloc failed.");
            // PulseView RAII pattern: ensure all resources are cleaned up
            // on early return. Previously this path jumped directly to
            // return, leaking sr_output and GHashTable. Match the cleanup
            // pattern already used by the file-open failure path above.
            _has_error.store(true);
            set_error(L_S(STR_PAGE_DLG, S_ID(IDS_MSG_STORESESS_EXPORTPROC_ERROR2),
                "Failed to allocate memory for DSO export."));
            file.close();
            sr_output_free(output);
            g_hash_table_destroy(params);
            return;
        }

        int ch_num = dso_snapshot->get_channel_num();

        /* Initialize DSO packet fields. Previously dp was uninitialized,
         * causing sample_bits/en_ch_num/trig_flag to contain garbage. */
        memset(&dp, 0, sizeof(dp));
        dp.en_ch_num = (uint8_t)ch_num;
        int bits = 0;
        _session->get_device()->get_config_byte(SR_CONF_UNIT_BITS, bits);
        dp.sample_bits = bits ? bits : 8;

        for(uint64_t i = 0; !_canceled.load() && i < _unit_count.load(); i+=usize){
            if(_unit_count.load() - i < usize)
                size = _unit_count.load() - i;

            int ch = 0;
            // Make the cross data buffer.
           std::vector<std::shared_ptr<data::SignalModel>> _sm_models = _session->get_signal_models_snapshot(); for(auto m : _sm_models)
            {
                if (m->type() != SR_CHANNEL_DSO)
                    continue;

                if (!dso_snapshot->has_data(m->index()))
                    continue;

                uint8_t *wr = ch_data_buffer + ch;
                ch++;
                const uint8_t *rd = dso_snapshot->get_samples(0,0, m->index()) + i;
                const uint8_t *rd_end = rd + size;

                while (rd < rd_end)
                {
                    *wr = *rd;
                    wr += ch_num;
                    rd++;
                }
            }

            dp.data = ch_data_buffer;
            dp.num_samples = size;
            p.type = SR_DF_DSO;
            p.payload = &dp;
            sr_output_send(output, &p, &data_out);

            if(data_out){
                if (is_binary_output)
                    file.write(data_out->str, data_out->len);
                else
                    out << (char*) data_out->str;
                g_string_free(data_out,TRUE);
            }

            _units_stored.fetch_add(size);
            progress_updated();
        }

        if (ch_data_buffer){
            free(ch_data_buffer);
            ch_data_buffer = nullptr;
        }

    } else if (channel_type == SR_CHANNEL_ANALOG) {
        _unit_count.store(snapshot->get_sample_count());
        uint64_t unit_count = _unit_count.load();
        void* data_buffer = analog_snapshot->get_data();
        unsigned int usize = 8192;        
        struct sr_datafeed_analog ap;
        struct sr_analog_encoding encoding;
        struct sr_analog_meaning meaning;
        struct sr_analog_spec spec;

        const uint64_t ring_start = analog_snapshot->get_ring_start();
 
        int ch_count = snapshot->get_channel_num();  
        int unit_bytes = analog_snapshot->get_unit_bytes();

        /* Build channel list for analog meaning (all enabled analog channels) */
        GSList *analog_ch_list = nullptr;
        std::vector<std::shared_ptr<data::SignalModel>> _sm_models = _session->get_signal_models_snapshot(); for(auto m : _sm_models) {
            if (m->type() == SR_CHANNEL_ANALOG) {
                for (GSList *l = _session->get_device()->get_channels(); l; l = l->next) {
                    struct sr_channel *ch = (struct sr_channel *)l->data;
                    if (ch->index == m->index() && ch->type == SR_CHANNEL_ANALOG) {
                        analog_ch_list = g_slist_append(analog_ch_list, ch);
                        break;
                    }
                }
            }
        }

        /* sr_analog_init is SR_PRIV (internal-only), not exported in the
         * public libsigrok API. Manually initialize the analog structs here.
         * This replicates what sr_analog_init() does (see analog.c). */
        memset(&ap, 0, sizeof(ap));
        memset(&encoding, 0, sizeof(encoding));
        memset(&meaning, 0, sizeof(meaning));
        memset(&spec, 0, sizeof(spec));
        ap.encoding = &encoding;
        ap.meaning = &meaning;
        ap.spec = &spec;
        encoding.unitsize = sizeof(float);
        encoding.is_float = TRUE;
        encoding.is_bigendian = FALSE;
        encoding.digits = 2;
        encoding.is_digits_decimal = TRUE;
        encoding.scale.p = 1;
        encoding.scale.q = 1;
        encoding.offset.p = 0;
        encoding.offset.q = 1;
        spec.spec_digits = 2;
        /* Override with actual snapshot format */
        encoding.unitsize = unit_bytes;
        encoding.is_float = analog_snapshot->is_float();
        encoding.is_signed = TRUE;
        encoding.is_bigendian = FALSE;
        ap.meaning->channels = analog_ch_list;
        ap.meaning->mq = SR_MQ_VOLTAGE;
        ap.meaning->unit = SR_UNIT_VOLT;
        ap.meaning->mqflags = SR_MQFLAG_DC;
    
        void *block_buffer[2];
        uint64_t block_samples[2];
        block_buffer[0] =  (unsigned char*)data_buffer + ring_start * ch_count * unit_bytes;
        block_samples[0] = unit_count - ring_start;
        block_buffer[1] = data_buffer;
        block_samples[1] = ring_start;

        for (int j=0; j<2; j++)
        {  
            uint64_t sample_count = block_samples[j];

            if (sample_count == 0)
                break;

            //pxv_info("sample_count:%llu,total:%llu", sample_count, unit_count);

            for(uint64_t i = 0; i < sample_count; i += usize){
                
                if (_canceled)
                    break;

                unsigned int size = usize;

                if(sample_count - i < usize){
                    size = sample_count - i;
                }
         
                ap.data = (unsigned char*)block_buffer[j] + i * ch_count * unit_bytes;
                ap.num_samples = size;
                p.type = SR_DF_ANALOG;
                p.payload = &ap;
                sr_output_send(output, &p, &data_out);

                if(data_out){
                    if (is_binary_output)
                        file.write(data_out->str, data_out->len);
                    else
                        out << (char*) data_out->str;
                    g_string_free(data_out,TRUE);
                }           

                _units_stored.fetch_add(size);
                progress_updated();

               // pxv_info("size:%llu;_units_stored:%llu", size, _units_stored);
            }
        }

        g_slist_free(analog_ch_list);
    }

    // Buffering output modules (notably "csv") only flush their accumulated
    // sample data when they receive SR_DF_END — the SR_DF_LOGIC/DSO/ANALOG
    // receive() calls merely buffer into per-packet sample arrays. Without
    // this final packet the CSV export would be silently empty even though
    // every data packet was consumed. Emit SR_DF_END to trigger the module's
    // end-of-stream dump_saved_values().
    // SR_DF_END carries no payload (see enum sr_packettype in libsigrok.h).
    p.type = SR_DF_END;
    p.payload = nullptr;
    sr_output_send(output, &p, &data_out);
    if (data_out) {
        if (is_binary_output)
            file.write(data_out->str, data_out->len);
        else
            out << QString::fromUtf8((char*) data_out->str);
        g_string_free(data_out, TRUE);
    }

    // optional, as QFile destructor will already do it:
    file.close();
    // Upstream libsigrok: sr_output_free() replaces fork _outModule->cleanup().
    sr_output_free(output);
    g_hash_table_destroy(params);

    progress_updated();
}

 
bool StoreSession::decoders_gen(std::string &str)
{  
    QJsonArray dec_array;
    if (!gen_decoders_json(dec_array))
        return false;
    QJsonDocument sessionDoc(dec_array);
    // 使用 toJson() 的原始 QByteArray（UTF-8）按长度写入，避免经
    // QString/const char* 的 C 字符串转换在遇到 NUL 字节时截断 JSON，
    // 否则 .pxl 的 "decoders" 入口可能被截断，加载时无法恢复解码器设置。
    QByteArray ba = sessionDoc.toJson();
    str = std::string(ba.constData(), ba.size());
    return true;
}

bool StoreSession::gen_decoders_json(QJsonArray &array)
{
    // Serialize the decoder stacks of _decoder_doc if set (headless API save
    // has MCP decoders on the dedicated API document), else the active doc.
    auto &stacks = _session->get_decoder_stacks(_decoder_doc);
    for(auto stack : stacks) {
        QJsonObject dec_obj;
        QJsonArray stack_array;
        QJsonObject show_obj;
        const auto &decoderList = stack->stack();

        for(auto &up : decoderList) 
        {
            auto dec = up.get();
            QJsonArray ch_array;
            const srd_decoder *const d = dec->decoder();;

            auto binded_probes = dec->binded_probe_list();
            for(auto probe : binded_probes) {
                QJsonObject ch_obj;
                int binded_index = dec->binded_probe_index(probe);
                ch_obj[probe->id] = QJsonValue::fromVariant(binded_index);
                ch_array.push_back(ch_obj);
            }

            QJsonObject options_obj;
            // PulseView RAII pattern: use unique_ptr instead of raw new.
            // Previously this was `new` without a matching `delete`,
            // leaking a DecoderOptions object on every save.
            auto dec_binding = std::make_unique<prop::binding::DecoderOptions>(stack, dec);

            for (GSList *l = d->options; l; l = l->next)
            {
                const srd_decoder_option *const opt =
                    (srd_decoder_option*)l->data;

                if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("d"))) {
                    GVariant *const var = dec_binding->getter(opt->id);
                    if (var != nullptr) {
                        options_obj[opt->id] = QJsonValue::fromVariant(g_variant_get_double(var));
                        g_variant_unref(var);
                    }
                } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("x"))) {
                    GVariant *const var = dec_binding->getter(opt->id);
                    if (var != nullptr) {
                        options_obj[opt->id] = QJsonValue::fromVariant(get_integer(var));
                        g_variant_unref(var);
                    }
                } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("s"))) {
                    GVariant *const var = dec_binding->getter(opt->id);
                    if (var != nullptr) {
                        const char *sz = g_variant_get_string(var, nullptr);
                        options_obj[opt->id] = QJsonValue::fromVariant(QString(sz));
                        g_variant_unref(var);
                    }
                }else {
                    continue;
                }
            }

            // The base (front) decoder always goes into dec_obj, while every
            // sub decoder goes into "stacked decoders". This must mirror the
            // read side in load_decoders(), which restores options/channel for
            // the front decoder from dec_obj and sub decoders from the stacked
            // array. Previously this branch keyed on whether the decoder defined
            // input probes (have_probes), which overwrote the same dec_obj
            // fields for every probe-bearing decoder in the stack and dropped
            // sub-decoder options/channel mappings entirely.
            if (dec == decoderList.front().get()) {
                dec_obj["id"] = QJsonValue::fromVariant(QString(d->id));
                dec_obj["channel"] = ch_array;
                dec_obj["options"] = options_obj;
            } else {
                QJsonObject stack_obj;
                stack_obj["id"] = QJsonValue::fromVariant(QString(d->id));
                stack_obj["channel"] = ch_array;
                stack_obj["options"] = options_obj;
                stack_array.push_back(stack_obj);
            }
            show_obj[d->id] = QJsonValue::fromVariant(dec->shown());
        }
        
        dec_obj["version"] = DEOCDER_CONFIG_VERSION;
        // Save the custom label from the DecoderStack. If no custom label
        // is set, fall back to the first decoder's id.
        QString saved_label = stack->label();
        if (saved_label.isEmpty() && !decoderList.empty()) {
            saved_label = QString(decoderList.front()->decoder()->id);
        }
        dec_obj["label"] = saved_label;
        dec_obj["stacked decoders"] = stack_array;
        // TODO: adapt — view_index is UI state owned by view::DecodeTrace;
        // DecoderStack does not expose it. Persist 0 for now and let the
        // View layer restore the index after it creates DecodeTrace.
        dec_obj["view_index"] = 0;

        auto rows = stack->get_rows_gshow();
        for (auto i = rows.begin(); i != rows.end(); i++) {
            pv::data::decode::Row _row = (*i).first;
            QString kn = _row.title_id();
            show_obj[kn] = QJsonValue::fromVariant((*i).second);
        }
        dec_obj["show"] = show_obj;

        array.push_back(dec_obj);
    }

    return true;
}

bool StoreSession::load_decoders(dock::ProtocolDock *widget, QJsonArray &dec_array)
{
    if (_session->get_device()->get_work_mode() != LOGIC)
    {
        pxv_info("StoreSession::load_decoders(), is not LOGIC mode.");
        return false;
    }

    if (dec_array.isEmpty()){
        pxv_info("StoreSession::load_decoders(), json object array is empty.");
        return false;
    }

    int dec_index = -1;
    
    pxv_info("StoreSession::load_decoders: starting to process %d decoders", dec_array.size());
    for (const QJsonValue &dec_value : dec_array)
    {
        QJsonObject dec_obj = dec_value.toObject();
        pxv_info("StoreSession::load_decoders: processing decoder %s", dec_obj["id"].toString().toStdString().c_str());
        auto &pre_dsigs = _session->get_decoder_stacks();
        std::list<pv::data::decode::Decoder*> sub_decoders;

        //get sub decoders
        if (dec_obj.contains("stacked decoders")) {
                for(const QJsonValue &value : dec_obj["stacked decoders"].toArray()) {
                    QJsonObject stacked_obj = value.toObject();

                    GSList *dl = g_slist_copy((GSList*)srd_decoder_list());
                    for(; dl; dl = dl->next) {
                        const srd_decoder *const d = (srd_decoder*)dl->data;
                        assert(d);
                        if (!d) {
                            pxv_warn("StoreSession::load_decoders: srd_decoder list node has nullptr data, skipping.");
                            continue;
                        }

                        if (QString::fromUtf8(d->id) == stacked_obj["id"].toString()) {
                            sub_decoders.push_back(new data::decode::Decoder(d));
                            break;
                        }
                    }
                    g_slist_free(dl);
                }
        }

        //create protocol
        bool ret = widget->add_protocol_by_id(dec_obj["id"].toString(), true, sub_decoders);
        if (!ret)
        {
            for(auto sub : sub_decoders){
                delete sub;
            }
            sub_decoders.clear();

            continue; //protocol is not exists;
        }

        dec_index++;

        if (dec_obj.contains("label")){
            _session->set_decoder_row_label(dec_index, dec_obj["label"].toString());    
        }

        if (dec_obj.contains("view_index")){
            int chan_view_index = dec_obj["view_index"].toInt();
            // TODO: adapt — DecoderStack no longer exposes set_view_index; UI state
            // should be restored by the View layer after it creates DecodeTrace.
            (void)chan_view_index;
        }

        std::list<int> bind_indexs;

        auto &aft_dsigs = _session->get_decoder_stacks();
        pxv_info("StoreSession::load_decoders: pre_dsigs.size()=%d, aft_dsigs.size()=%d", (int)pre_dsigs.size(), (int)aft_dsigs.size());

        if (aft_dsigs.size() >= pre_dsigs.size()) {
            const GSList *l;

            auto new_dsig = aft_dsigs.back();
            auto stack = new_dsig;
            pxv_info("StoreSession::load_decoders: new_dsig=%p", new_dsig.get());

            auto &decoder_list = stack->stack();

            for(auto &up : decoder_list) 
            {
                auto dec = up.get();
                const srd_decoder *const d = dec->decoder();
                QJsonObject options_obj;
                QJsonArray channel_array;

                if (dec == decoder_list.front().get()) {
                    options_obj = dec_obj["options"].toObject();
                    channel_array = dec_obj["channel"].toArray();
                }
                else {
                    for(const QJsonValue &value : dec_obj["stacked decoders"].toArray()) {
                        QJsonObject stacked_obj = value.toObject();
                        if (QString::fromUtf8(d->id) == stacked_obj["id"].toString()) {
                            options_obj = stacked_obj["options"].toObject();
                            channel_array = stacked_obj["channel"].toArray();
                            break;
                        }
                    }
                }

                // Restore the probe (channel) mapping for this decoder.
                // Both the base decoder (from dec_obj) and sub decoders (from
                // their stacked entry) carry a "channel" array that maps each
                // channel id to a bound signal index.
                {
                    std::map<const srd_channel*, int> probe_map;
                    // Load the mandatory channels
                    for(l = d->channels; l; l = l->next) {
                        const struct srd_channel *const pdch = (struct srd_channel *)l->data;
                        pxv_info("StoreSession::load_decoders: checking mandatory channel '%s'", pdch->id);

                        for (const QJsonValue &value : channel_array) {
                            QJsonObject ch_obj = value.toObject();
                            if (ch_obj.contains(pdch->id)) {
                                int bind_chan = ch_obj[pdch->id].toInt();
                                probe_map[pdch] = bind_chan;
                                pxv_info("StoreSession::load_decoders: mapped mandatory channel '%s' to bind_chan %d", pdch->id, bind_chan);

                                auto fd_it = find(bind_indexs.begin(), bind_indexs.end(), bind_chan);
                                if (fd_it == bind_indexs.end())
                                    bind_indexs.push_back(bind_chan);
                                break;
                            }
                        }
                    }

                    // Load the optional channels
                    for(l = d->opt_channels; l; l = l->next) {
                        const struct srd_channel *const pdch = (struct srd_channel *)l->data;
                        pxv_info("StoreSession::load_decoders: checking optional channel '%s'", pdch->id);

                        for (const QJsonValue &value : channel_array) {
                            QJsonObject ch_obj = value.toObject();
                            if (ch_obj.contains(pdch->id)) {
                                int bind_chan = ch_obj[pdch->id].toInt();
                                probe_map[pdch] = bind_chan;
                                pxv_info("StoreSession::load_decoders: mapped optional channel '%s' to bind_chan %d", pdch->id, bind_chan);

                                auto fd_it = find(bind_indexs.begin(), bind_indexs.end(), bind_chan);
                                if (fd_it == bind_indexs.end())
                                    bind_indexs.push_back(bind_chan);
                                break;
                            }
                        }
                    }
                    pxv_info("StoreSession::load_decoders: setting %d probes on decoder", (int)probe_map.size());
                    if (!probe_map.empty())
                        dec->set_probes(probe_map);
                }

                for (l = d->options; l; l = l->next) {
                    const srd_decoder_option *const opt = (srd_decoder_option*)l->data;

                    if (options_obj.contains(opt->id)) 
                    {
                        GVariant *new_value = nullptr;
                        // When the numberic value is a string, it got zero always,
                        // so must convert from string.
                        QString vs = options_obj[opt->id].toString();

                        if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("d"))) 
                        {
                            double vi = options_obj[opt->id].toDouble();
                            if (vs != "") vi = vs.toDouble();
                            new_value = g_variant_new_double(vi);
                        }
                        else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("x"))) {
                            const GVariantType *const type = g_variant_get_type(opt->def);

                            if (g_variant_type_equal(type, G_VARIANT_TYPE_BYTE)){
                                int vi = options_obj[opt->id].toInt();                               
                                if (vs != "") vi = vs.toInt();
                                new_value = g_variant_new_byte(vi);
                            }
                            else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT16)){
                                int vi = options_obj[opt->id].toInt();                               
                                if (vs != "") vi = vs.toInt();
                                new_value = g_variant_new_int16(vi);
                            }
                            else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT16)){
                                int vi = options_obj[opt->id].toInt();                               
                                if (vs != "") vi = vs.toInt();
                                new_value = g_variant_new_uint16(vi);
                            }
                            else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT32)){
                                int vi = options_obj[opt->id].toInt();                               
                                if (vs != "") vi = vs.toInt();
                                new_value = g_variant_new_int32(vi);
                            }
                            else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT32)){
                                int vi = options_obj[opt->id].toInt();                               
                                if (vs != "") vi = vs.toInt();
                                new_value = g_variant_new_uint32(vi);
                            }
                            else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT64)){
                                int vi = options_obj[opt->id].toInt();                               
                                if (vs != "") vi = vs.toInt();
                                new_value = g_variant_new_int64(vi);
                            }
                            else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT64)){
                                int vi = options_obj[opt->id].toInt();                               
                                if (vs != "") vi = vs.toInt();
                                new_value = g_variant_new_uint64(vi);
                            }
                        }
                        else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("s"))) {
                            new_value = g_variant_new_string(vs.toUtf8().data());
                        }

                        if (new_value != nullptr){
                            dec->set_option(opt->id, new_value);
                        }
                    }
                }
                dec->commit();

                if (dec_obj.contains("show")) {
                    QJsonObject show_obj = dec_obj["show"].toObject();
                    if (show_obj.contains(d->id)) {
                        dec->show(show_obj[d->id].toBool());
                    }
                }
            }

            // Restore the binded channel index
            if (bind_indexs.size() > 0){
                // TODO: adapt — DecoderStack no longer exposes set_index_list;
                // channel binding should be set via the decoder's probe map API.
                // auto dec_trace = _session->get_decoder_trace(dec_index);
                // if (dec_trace != nullptr) dec_trace->set_index_list(bind_indexs);
            }

            int decoder_cfg_version = -1;

            if (dec_obj.contains("version")){
                decoder_cfg_version = dec_obj["version"].toInt();
            }

            if (dec_obj.contains("show")) {
                QJsonObject show_obj = dec_obj["show"].toObject();
                std::map<const pv::data::decode::Row, bool> rows = stack->get_rows_gshow();

                for (auto i = rows.begin();i != rows.end(); i++) {
                    QString key;

                    if (decoder_cfg_version == -1)
                        key = (*i).first.title();
                    else
                        key = (*i).first.title_id();

                    if (show_obj.contains(key)) {
                        bool bShow = show_obj[key].toBool();
                        const pv::data::decode::Row r = (*i).first;
                        stack->set_rows_gshow(r, bShow);
                    }
                }
            }

            // TDM Fast: sync hidden output option with effective row visibility.
            if (!decoder_list.empty()) {
                auto *tdm_dec = decoder_list.front().get();
                const srd_decoder *tdm_def = tdm_dec ? tdm_dec->decoder() : nullptr;
                if (tdm_def && tdm_def->id &&
                    QString::fromUtf8(tdm_def->id) == QStringLiteral("tdm_audio_fast")) {
                    bool any_text_row_enabled = false;
                    const auto final_rows = stack->get_rows_gshow();
                    for (const auto &entry : final_rows) {
                        if (entry.first.decoder() == tdm_def && entry.second) {
                            any_text_row_enabled = true;
                            break;
                        }
                    }
                    const bool emit_text = tdm_dec->shown() && any_text_row_enabled;
                    tdm_dec->set_option(
                        "output", g_variant_new_string(emit_text ? "both" : "waveform"));
                }
            }

            // Call frame_ended() to set _options_changed flag, allowing decode to work properly
            new_dsig->frame_ended();
        }
    }

    return true;
}
 

double StoreSession::get_integer(GVariant *var)
{
    double val = 0;
    const GVariantType *const type = g_variant_get_type(var);
    assert(type);
    if (!type) {
        pxv_warn("StoreSession::get_integer: g_variant_get_type returned nullptr.");
        return 0.0;
    }

    if (g_variant_type_equal(type, G_VARIANT_TYPE_BYTE))
        val = g_variant_get_byte(var);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT16))
        val = g_variant_get_int16(var);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT16))
        val = g_variant_get_uint16(var);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT32))
        val = g_variant_get_int32(var);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT32))
        val = g_variant_get_uint32(var);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT64))
        val = g_variant_get_int64(var);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT64))
        val = g_variant_get_uint64(var);
    else {
        pxv_err("StoreSession: unsupported GVariant type for uint64 conversion");
        val = 0;
    }

    return val;
}

QString StoreSession::MakeSaveFile(bool bDlg)
{
    QString default_name;

    AppConfig &app = AppConfig::Instance(); 
    if (app.userHistory.saveDir != "")
    {
        default_name = app.userHistory.saveDir + "/"  + _session->get_device()->name() + "-";
    } 
    else{
        QDir _dir;
        QString _root = _dir.home().path();                
        default_name =  _root + "/" + _session->get_device()->name() + "-";
    } 

    for (const GSList *l = _session->get_device()->get_device_mode_list(); l; l = l->next) 
    {
        const sr_dev_mode *mode = (const sr_dev_mode *)l->data;
        if (_session->get_device()->get_work_mode() == mode->mode) {
            default_name += mode->acronym;
            break;
        }
    }

    default_name += _session->get_session_time().toString("-yyMMdd-hhmmss");

    // Show the dialog
    if (bDlg)
    {
        default_name = QFileDialog::getSaveFileName(
            nullptr,
            L_S(STR_PAGE_MSG, S_ID(IDS_MSG_SAVE_FILE),"Save File"),
            default_name,
            //tr
            "PXView Data (*.pxl)");

        if (default_name.isEmpty())
        {
            return ""; //no select file
        }

        QString _dir_path = path::GetDirectoryName(default_name);

        if (_dir_path != app.userHistory.saveDir)
        {
            app.userHistory.saveDir = _dir_path;
            app.SaveHistory();
        }
    }

    QFileInfo f(default_name);
    if (f.suffix().compare("pxl"))
    {
        //Tr
        default_name.append(".pxl");
    }
    _file_name = default_name;
    return default_name;     
}

QString StoreSession::MakeExportFile(bool bDlg)
{
    QString default_name;
    AppConfig &app = AppConfig::Instance();  
    
    if (app.userHistory.exportDir != "")
    {
        default_name = app.userHistory.exportDir  + "/"  + _session->get_device()->name() + "-";
    } 
    else{
        QDir _dir;
        QString _root = _dir.home().path();    
        default_name =  _root + "/" + _session->get_device()->name() + "-";
    }  

    for (const GSList *l = _session->get_device()->get_device_mode_list(); l; l = l->next) {
        const sr_dev_mode *mode = (const sr_dev_mode *)l->data;
        if (_session->get_device()->get_work_mode() == mode->mode) {
            default_name += mode->acronym;
            break;
        }
    }
    default_name += _session->get_session_time().toString("-yyMMdd-hhmmss");

    //ext name
    QList<QString> supportedFormats = getSuportedExportFormats();
    QString filter;
    for(int i = 0; i < supportedFormats.count();i++){
        filter.append(supportedFormats[i]);
        if(i < supportedFormats.count() - 1)
            filter.append(";;");
    }

    QString selfilter;
    if (app.userHistory.exportFormat != "" 
            && _session->get_device()->get_work_mode() == LOGIC){
        selfilter.append(app.userHistory.exportFormat);
    }
    else{
        selfilter.append(".csv");
    }

    if (bDlg)
    {
        default_name = QFileDialog::getSaveFileName(
            nullptr,
            L_S(STR_PAGE_MSG, S_ID(IDS_MSG_EXPORT_DATA),"Export Data"),
            default_name,
            filter,
            &selfilter);

        if (default_name == "")
        {
            return "";
        }

        bool bChange = false;
        QString _dir_path = path::GetDirectoryName(default_name);
        if (_dir_path != app.userHistory.exportDir)
        {
            app.userHistory.exportDir = _dir_path;
            bChange = true;
        }
        
        if (selfilter != app.userHistory.exportFormat 
                && _session->get_device()->get_work_mode() == LOGIC){
            app.userHistory.exportFormat = selfilter;
             bChange = true;            
        }

        if (bChange){
            app.SaveHistory();            
        }
    }

    QString extName = selfilter;
    if (extName == ""){
        extName = filter;
    }

    QStringList list = extName.split('.').last().split(')');
    _suffix = list.first();

    QFileInfo f(default_name);
    if(f.suffix().compare(_suffix)){
        //tr
         default_name += "." + _suffix;
    }           

    _file_name = default_name;
    return default_name;    
}

bool StoreSession::IsLogicDataType()
{
    std::set<int> type_set;
    std::vector<std::shared_ptr<data::SignalModel>> _sm_models = _session->get_signal_models_snapshot(); for(auto m : _sm_models) {
        type_set.insert((int)m->type());
    }

    if (type_set.size()){
        int type = *(type_set.begin());
        return type == (int)SR_CHANNEL_LOGIC;
    }

    return false;
}

void StoreSession::MakeChunkName(char *chunk_name, int chunk_num, int index, int type, int version)
{ 
    chunk_name[0] = 0;

    if (version >= 2)
    {
        const char *type_name = nullptr;
        type_name = (type == SR_CHANNEL_LOGIC) ? "L" : (type == SR_CHANNEL_DSO)  ? "O"
                                                   : (type == SR_CHANNEL_ANALOG) ? "A"
                                                                                 : "U";
        snprintf(chunk_name, 15, "%s-%d/%d", type_name, index, chunk_num);
    }
    else
    {
        snprintf(chunk_name, 15, "data");
    }
}

} // pv
