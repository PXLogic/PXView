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

#include "pv/view/component/waveform_copy_helper.h"

#include "pv/view/view.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/view/trace/decodetrace.h"
#include "pv/view/cursor/timemarker.h"
#include "pv/view/cursor/cursor.h"
#include "pv/view/signal/signal.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/decode/decoder.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/data/decode/rowdata.h"
#include "pv/data/decode/annotation.h"
#include "pv/ui/langresource.h"

#include <QGuiApplication>
#include <QClipboard>
#include <cmath>
#include <set>

namespace pv {
namespace view {

// Format a sample index as a timestamp string in seconds (fixed 6 decimals).
// LLM-friendly: "0.000500" not "500.000us" — consistent numeric column for CSV parsing.
static QString format_time_seconds(double seconds)
{
    return QString::number(seconds, 'f', 6);
}

// Pick the best (longest) annotation text from the list — most detailed representation.
QString WaveformCopyHelper::pick_annotation_text(const std::vector<QString> &ann_list)
{
    if (ann_list.empty())
        return QString();
    QString best = ann_list.front();
    for (const auto &txt : ann_list) {
        if (txt.length() > best.length())
            best = txt;
    }
    return best;
}

QString WaveformCopyHelper::format_signal(LogicSignal *signal, uint64_t start, uint64_t end)
{
    if (!signal)
        return QString();

    data::LogicSnapshot *snapshot = signal->data();
    if (!snapshot)
        return QString();

    double sample_rate = (double)snapshot->samplerate();
    if (sample_rate <= 0)
        return QString();

    int sig_index = signal->get_index();

    if (start > end)
        std::swap(start, end);

    if (start >= end)
        return QString();

    QString high_label = L_S(STR_PAGE_SIGNAL_PROC, "IDS_GLITCH_FILTER_HIGH", "High");
    QString low_label = L_S(STR_PAGE_SIGNAL_PROC, "IDS_GLITCH_FILTER_LOW", "Low");

    // Metadata header — gives LLM context about the data
    QString result;
    result += "[Timing Analysis]\n";
    result += "Signal: " + signal->get_name() + "\n";
    result += "Sample Rate: " + QString::number(sample_rate, 'f', 0) + " Hz\n";
    result += "Format: [Time(s)] [Level] [Duration(s)]\n";

    // CSV body — one row per edge transition
    bool level = snapshot->get_sample(start, sig_index);
    uint64_t current = start;

    while (current < end) {
        uint64_t segment_start = current;
        bool found = snapshot->get_nxt_edge(current, level, end, 1, sig_index);

        uint64_t segment_end;
        if (!found || current >= end)
            segment_end = end;
        else
            segment_end = current;

        double t_start = (double)segment_start / sample_rate;
        double duration = (double)(segment_end - segment_start) / sample_rate;

        result += format_time_seconds(t_start) + ", " +
                  (level ? high_label : low_label) + ", " +
                  format_time_seconds(duration) + "\n";

        if (!found || current >= end)
            break;

        level = !level;
    }

    return result;
}

QString WaveformCopyHelper::format_signals(const std::vector<LogicSignal*> &sigs,
                                           uint64_t start, uint64_t end)
{
    if (sigs.empty())
        return QString();

    // Use first signal's sample rate for the header
    double sample_rate = 0;
    for (auto *s : sigs) {
        if (s && s->data()) {
            sample_rate = (double)s->data()->samplerate();
            if (sample_rate > 0)
                break;
        }
    }

    QString result;
    result += "[Timing Analysis]\n";
    result += "Sample Rate: " + QString::number(sample_rate, 'f', 0) + " Hz\n";
    result += "Format: [Time(s)] [Channel] [Level] [Duration(s)]\n";

    QString high_label = L_S(STR_PAGE_SIGNAL_PROC, "IDS_GLITCH_FILTER_HIGH", "High");
    QString low_label = L_S(STR_PAGE_SIGNAL_PROC, "IDS_GLITCH_FILTER_LOW", "Low");

    // Each signal's edges interleaved into the CSV, sorted by timestamp
    // For simplicity, output each signal's block sequentially (channel column disambiguates)
    for (auto *signal : sigs) {
        if (!signal)
            continue;
        data::LogicSnapshot *snapshot = signal->data();
        if (!snapshot)
            continue;

        double sr = (double)snapshot->samplerate();
        if (sr <= 0)
            continue;

        int sig_index = signal->get_index();
        QString name = signal->get_name();

        uint64_t s = start, e = end;
        if (s > e) std::swap(s, e);
        if (s >= e) continue;

        bool level = snapshot->get_sample(s, sig_index);
        uint64_t current = s;

        while (current < e) {
            uint64_t segment_start = current;
            bool found = snapshot->get_nxt_edge(current, level, e, 1, sig_index);

            uint64_t segment_end;
            if (!found || current >= e)
                segment_end = e;
            else
                segment_end = current;

            double t_start = (double)segment_start / sr;
            double duration = (double)(segment_end - segment_start) / sr;

            result += format_time_seconds(t_start) + ", " +
                      name + ", " +
                      (level ? high_label : low_label) + ", " +
                      format_time_seconds(duration) + "\n";

            if (!found || current >= e)
                break;

            level = !level;
        }
    }

    return result;
}

QString WaveformCopyHelper::format_decoder_annotations(DecodeTrace *dt, uint64_t start, uint64_t end)
{
    if (!dt)
        return QString();

    auto stack = dt->decoder();
    if (!stack)
        return QString();

    double sample_rate = (double)stack->sample_rate();
    if (sample_rate <= 0)
        return QString();

    if (start > end)
        std::swap(start, end);

    if (start >= end)
        return QString();

    // Extract protocol name from the first decoder in the stack.
    // If a custom label is set, append it in parentheses so multiple
    // instances of the same decoder can be distinguished.
    QString protocol = "Unknown";
    auto &decoder_list = stack->stack();
    if (!decoder_list.empty()) {
        auto *first_decoder = decoder_list.front().get();
        if (first_decoder) {
            const srd_decoder *srd_dec = first_decoder->get_dec_handel();
            if (srd_dec && srd_dec->name)
                protocol = QString::fromUtf8(srd_dec->name);
        }
    }
    QString custom_label = stack->label();
    if (!custom_label.isEmpty())
        protocol += "(" + custom_label + ")";

    // Metadata header
    QString result;
    result += "[Logic Analyzer Export]\n";
    result += "Protocol: " + protocol + "\n";
    result += "Sample Rate: " + QString::number(sample_rate, 'f', 0) + " Hz\n";
    result += "Format: [Timestamp(s)] [Data]\n";

    // CSV body — one row per decoded annotation
    auto rows = stack->get_rows_gshow();

    for (auto &row_pair : rows) {
        if (!row_pair.second)
            continue; // row not shown

        const auto &row = row_pair.first;
        if (!stack->has_annotations(row))
            continue;

        std::vector<pv::data::decode::Annotation*> anns;
        stack->get_annotation_subset(anns, row, start, end);

        for (auto *ann : anns) {
            if (!ann)
                continue;

            double ts = (double)ann->start_sample() / sample_rate;
            QString text = pick_annotation_text(ann->annotations());

            if (text.isEmpty())
                result += format_time_seconds(ts) + ",\n";
            else
                result += format_time_seconds(ts) + ", " + text + "\n";
        }
    }

    return result;
}

std::pair<uint64_t, uint64_t> WaveformCopyHelper::resolve_cursor_range(View &view, int click_x)
{
    uint64_t click_index = view.pixel2index(click_x);
    int view_width = view.get_view_width();
    uint64_t left_edge = view.pixel2index(0);
    uint64_t right_edge = view.pixel2index(view_width);

    auto &cursors = view.get_cursorList();

    bool has_left = false;
    bool has_right = false;
    uint64_t left_index = 0;
    uint64_t right_index = 0;

    for (auto &cursor : cursors) {
        if (!cursor)
            continue;
        uint64_t idx = cursor->get_index();
        if (idx <= click_index) {
            if (!has_left || idx > left_index) {
                left_index = idx;
                has_left = true;
            }
        } else {
            if (!has_right || idx < right_index) {
                right_index = idx;
                has_right = true;
            }
        }
    }

    uint64_t s, e;
    if (has_left && has_right) {
        s = left_index;
        e = right_index;
    } else if (has_left) {
        s = left_index;
        e = right_edge;
    } else if (has_right) {
        s = left_edge;
        e = right_index;
    } else {
        s = left_edge;
        e = right_edge;
    }

    if (s > e)
        std::swap(s, e);

    return {s, e};
}

LogicSignal* WaveformCopyHelper::hit_test_signal(View &view, int click_x, int click_y)
{
    (void)click_x;
    int mouseY = click_y + view.get_vOffset();
    for (auto &s : view.get_own_signals()) {
        if (!s)
            continue;
        if (s->signal_type() == SR_CHANNEL_LOGIC && s->enabled()) {
            int sigY = s->get_v_offset();
            int halfH = s->get_totalHeight() / 2 + View::SignalMargin;
            if (std::abs(mouseY - sigY) < halfH)
                return s->as_logic();
        }
    }
    return nullptr;
}

DecodeTrace* WaveformCopyHelper::hit_test_decode_trace(View &view, int click_x, int click_y)
{
    (void)click_x;
    int mouseY = click_y + view.get_vOffset();
    for (auto &t : view.get_own_decode_traces()) {
        if (!t || !t->enabled())
            continue;
        int sigY = t->get_v_offset();
        int halfH = t->get_totalHeight() / 2 + View::SignalMargin;
        if (std::abs(mouseY - sigY) < halfH)
            return t.get();
    }
    return nullptr;
}

DecodeTrace* WaveformCopyHelper::find_decoder_for_signal(View &view, LogicSignal *signal)
{
    if (!signal)
        return nullptr;

    int sig_index = signal->get_index();

    for (auto &dt : view.get_own_decode_traces()) {
        if (!dt)
            continue;
        auto stack = dt->decoder();
        if (!stack)
            continue;
        for (auto &up : stack->stack()) {
            auto decoder = up.get();
            if (!decoder)
                continue;
            auto probes = decoder->binded_probe_list();
            for (auto *probe : probes) {
                if (decoder->binded_probe_index(probe) == sig_index)
                    return dt.get();
            }
        }
    }
    return nullptr;
}

std::vector<LogicSignal*> WaveformCopyHelper::collect_decoder_input_signals(View &view, DecodeTrace *dt)
{
    std::vector<LogicSignal*> result;
    if (!dt)
        return result;

    auto stack = dt->decoder();
    if (!stack)
        return result;

    std::set<int> channel_indices;
    for (auto &up : stack->stack()) {
        auto decoder = up.get();
        if (!decoder)
            continue;
        auto probes = decoder->binded_probe_list();
        for (auto *probe : probes) {
            int idx = decoder->binded_probe_index(probe);
            if (idx >= 0)
                channel_indices.insert(idx);
        }
    }

    for (auto &s : view.get_own_signals()) {
        if (!s)
            continue;
        if (s->signal_type() == SR_CHANNEL_LOGIC && s->enabled()) {
            if (channel_indices.count(s->get_index()))
                result.push_back(s->as_logic());
        }
    }

    return result;
}

std::vector<LogicSignal*> WaveformCopyHelper::collect_all_logic_signals(View &view)
{
    std::vector<LogicSignal*> result;
    for (auto &s : view.get_own_signals()) {
        if (!s)
            continue;
        if (s->signal_type() == SR_CHANNEL_LOGIC && s->enabled())
            result.push_back(s->as_logic());
    }
    return result;
}

QString WaveformCopyHelper::format_range(View &view, int click_x, int click_y, Scope scope)
{
    auto range = resolve_cursor_range(view, click_x);
    uint64_t start = range.first;
    uint64_t end = range.second;

    if (start >= end)
        return QString();

    if (scope == Scope::ThisChannel) {
        LogicSignal *sig = hit_test_signal(view, click_x, click_y);
        if (!sig)
            return QString();
        return format_signal(sig, start, end);
    }

    if (scope == Scope::DecoderTrack) {
        // Copy decoded annotation results (not raw input signals)
        DecodeTrace *dt = hit_test_decode_trace(view, click_x, click_y);
        if (!dt)
            return QString();
        return format_decoder_annotations(dt, start, end);
    }

    if (scope == Scope::AllChannels) {
        // Output all logic signals (Timing Analysis) + all decoder annotations
        // LLM-friendly: combines raw timing for logic channels + decoded protocol data
        QString result;

        // Part 1: All logic signals as Timing Analysis
        auto logic_sigs = collect_all_logic_signals(view);
        if (!logic_sigs.empty()) {
            result += format_signals(logic_sigs, start, end);
        }

        // Part 2: All decoder annotations
auto &traces = view.get_own_decode_traces();
for (auto &dt : traces) {
            if (!dt || !dt->enabled())
                continue;
            QString ann = format_decoder_annotations(dt.get(), start, end);
            if (!ann.isEmpty()) {
                if (!result.isEmpty())
                    result += "\n";
                result += ann;
            }
        }

        return result;
    }

    if (scope == Scope::ThisDecoderGroup) {
        // Output decoded annotations from the decoder group (not raw input signals)
        LogicSignal *sig = hit_test_signal(view, click_x, click_y);
        DecodeTrace *dt = nullptr;
        if (sig)
            dt = find_decoder_for_signal(view, sig);
        if (!dt)
            dt = hit_test_decode_trace(view, click_x, click_y);

        if (!dt)
            return QString();

        return format_decoder_annotations(dt, start, end);
    }

    return QString();
}

} // namespace view
} // namespace pv
