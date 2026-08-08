#ifndef PULSEVIEW_VIEW_WAVEFORM_COPY_HELPER_H
#define PULSEVIEW_VIEW_WAVEFORM_COPY_HELPER_H

#include <QString>
#include <utility>
#include <vector>
#include <cstdint>

namespace pv {
namespace view {

class View;
class LogicSignal;
class DecodeTrace;

class WaveformCopyHelper
{
public:
    enum class Scope {
        ThisChannel,
        DecoderTrack,
        ThisDecoderGroup,
        AllChannels
    };

    /**
     * Main entry point. Resolves the cursor range based on click position,
     * collects signals/annotations according to scope, formats them.
     * Returns the formatted text (empty on failure).
     */
    static QString format_range(View &view, int click_x, int click_y, Scope scope);

    /**
     * Resolve the [start_index, end_index] range based on cursor positions
     * relative to the click X pixel.
     */
    static std::pair<uint64_t, uint64_t> resolve_cursor_range(View &view, int click_x);

    /**
     * Format a single logic signal's waveform between [start, end] as
     * "@timestamp 电平" lines, one transition per line.
     */
    static QString format_signal(LogicSignal *signal, uint64_t start, uint64_t end);

    /**
     * Format multiple logic signals, each block prefixed with channel name.
     */
    static QString format_signals(const std::vector<LogicSignal*> &sigs, uint64_t start, uint64_t end);

    /**
     * Format decoded annotations from a DecodeTrace between [start, end] as
     * "@timestamp: annotation_text" lines.
     */
    static QString format_decoder_annotations(DecodeTrace *dt, uint64_t start, uint64_t end);

    /**
     * Find the LogicSignal under the click Y position.
     */
    static LogicSignal* hit_test_signal(View &view, int click_x, int click_y);

    /**
     * Find the DecodeTrace under the click Y position.
     */
    static DecodeTrace* hit_test_decode_trace(View &view, int click_x, int click_y);

    /**
     * Find the DecodeTrace that uses the given signal as an input channel.
     */
    static DecodeTrace* find_decoder_for_signal(View &view, LogicSignal *signal);

    /**
     * Collect all input LogicSignals of a DecodeTrace.
     */
    static std::vector<LogicSignal*> collect_decoder_input_signals(View &view, DecodeTrace *dt);

    /**
     * Collect all visible logic signals in the view.
     */
    static std::vector<LogicSignal*> collect_all_logic_signals(View &view);

private:
    /**
     * Pick the best annotation text (longest non-empty).
     */
    static QString pick_annotation_text(const std::vector<QString> &ann_list);
};

} // namespace view
} // namespace pv

#endif
