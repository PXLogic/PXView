#ifndef PULSEVIEW_VIEW_WAVEFORM_COPY_HELPER_H
#define PULSEVIEW_VIEW_WAVEFORM_COPY_HELPER_H

#include <QString>
#include <utility>
#include <vector>
#include <cstdint>
#include <memory>
#include <array>

namespace pv {
namespace data { class DecoderAnalogData; }
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

    /** Configuration for exporting decoder analog audio as a WAV file. */
    struct WavExportConfig {
        struct MixRow {
            int channel = 0;
            bool enabled = false;
            std::array<float, 8> outputs{};
        };
        uint32_t sample_rate = 44100;
        int bits = 16;
        int output_channels = 2;
        std::vector<int> channel_indices;
        std::vector<MixRow> mix;
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

    /**
     * Export decoder analog audio data to a WAV file.
     * @param dt        Decode trace with analog data.
     * @param filepath  Output WAV file path.
     * @param cfg       Export configuration (sample rate, bit depth, mix matrix).
     * @param message   Error message on failure.
     * @return true on success, false on error.
     */
    static bool export_decoder_audio_wav(DecodeTrace *dt, const QString &filepath,
                                         const WavExportConfig &cfg,
                                         QString &message);

private:
    /**
     * Pick the best annotation text (longest non-empty).
     */
    static QString pick_annotation_text(const std::vector<QString> &ann_list);

    /**
     * Write a single PCM sample to a QByteArray at the given bit depth.
     */
    static void write_wav_pcm_sample(QByteArray &buf, float sample, int bitsPerSample);
};

} // namespace view
} // namespace pv

#endif
