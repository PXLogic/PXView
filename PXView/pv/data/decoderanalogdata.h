/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef PXVIEW_PV_DATA_DECODERANALOGDATA_H
#define PXVIEW_PV_DATA_DECODERANALOGDATA_H

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <atomic>

namespace pv {
namespace data {

/**
 * Stores analog sample data produced by a protocol decoder
 * (e.g. TDM audio decoder outputting float audio samples).
 *
 * Each instance holds one channel's worth of float data with
 * corresponding sample-number positions.
 */
struct DecoderAnalogSample {
    uint64_t start_sample;
    uint64_t end_sample;
    float    value;
};

/** Aggregate metrics for decoded samples overlapping a capture range. */
struct DecoderAnalogStatistics {
    bool valid = false;
    uint64_t sample_count = 0;
    uint64_t first_sample = 0;
    uint64_t last_sample = 0;
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    double rms = 0.0;
};

/** Cycle and pulse metrics calculated from a selected capture range. */
struct DecoderAnalogCycleMetrics {
    bool valid = false;
    bool rise_valid = false;
    bool fall_valid = false;
    bool time_valid = false;
    bool overshoot_valid = false;
    bool cycle_rms_valid = false;

    double rise_samples = 0.0;
    double fall_samples = 0.0;
    double period_samples = 0.0;
    double positive_width_samples = 0.0;
    double negative_width_samples = 0.0;
    double positive_duty_cycle = 0.0;
    double negative_duty_cycle = 0.0;
    double positive_overshoot = 0.0;
    double negative_overshoot = 0.0;
    double cycle_mean = 0.0;
    double cycle_rms = 0.0;
};

/** How a decoder's normalized [-1,+1] sample maps to an engineering value. */
enum class DecoderAnalogRangeMode {
    Bipolar,  ///< -1..+1 maps to -Max..+Max
    Unipolar, ///< -1..+1 maps to 0..Max
    Custom    ///< -1..+1 maps to Min..Max
};

/** Edge selection for the post-decode repeat display trigger. */
enum class DecoderAnalogTriggerEdge {
    Rising,
    Falling,
    Either
};

/** Behaviour when a repeat frame contains no threshold crossing. */
enum class DecoderAnalogTriggerMode {
    Auto,   ///< Display the decoded frame even when no crossing was found.
    Normal  ///< Keep the previous display until a crossing is found.
};

/** Configuration for aligning a decoder-generated analog waveform. */
struct DecoderAnalogTriggerConfig {
    bool enabled = false;
    DecoderAnalogTriggerMode mode = DecoderAnalogTriggerMode::Auto;
    int channel = 0;
    DecoderAnalogTriggerEdge edge = DecoderAnalogTriggerEdge::Rising;
    double level = 0.0;
    int display_position_percent = 50;
};

class DecoderAnalogData {
public:
    class ReadView;

    DecoderAnalogData(int channel, int num_channels, const std::string &label);
    ~DecoderAnalogData();

    /** Append one analog sample */
    void append_sample(uint64_t start_sample, uint64_t end_sample, float value);

    /** Append a uniformly-spaced block under a single lock. */
    void append_samples(uint64_t start_sample, uint64_t end_sample,
                        const float *values, size_t count);

    /** Append a block with exact per-sample capture spans under one lock. */
    void append_samples_timed(const uint64_t *start_samples,
                              const uint64_t *end_samples,
                              const float *values, size_t count);

    /** Clear all samples (e.g. on re-decode) */
    void clear();

    /** Get sample count */
    size_t get_sample_count() const;

    /** Get interpolated value at a given sample position */
    float get_value_at(uint64_t sample) const;

    /** Get the decoded sample span containing a capture sample position. */
    bool get_sample_at(uint64_t sample, DecoderAnalogSample &out) const;

    /** Calculate un-decimated sample statistics over a capture range. */
    bool get_statistics(uint64_t start_sample, uint64_t end_sample,
                        DecoderAnalogStatistics &out) const;

    /** Analyze complete cycles inside a capture range using 10/50/90%. */
    bool get_range_cycle_metrics(uint64_t start_sample, uint64_t end_sample,
                                 DecoderAnalogCycleMetrics &out) const;

    /** Hold a shared lock while inspecting the sample buffer. */
    ReadView read_samples() const;

    /**
     * Copy a bounded set of representative samples from the visible range.
     * The shared lock is held only while selecting/copying samples, so UI
     * painting never blocks decoder appends for the duration of QPainter work.
     */
    void copy_samples_for_render(uint64_t start_sample, uint64_t end_sample,
                                 size_t max_samples,
                                 std::vector<DecoderAnalogSample> &out,
                                 float &min_value, float &max_value) const;

    /** Get min/max for autoscaling */
    float min_value() const;
    float max_value() const;

    /** Channel info */
    int channel() const { return _channel; }
    int num_channels() const { return _num_channels; }
    std::string label() const { return _label; }

    /** Per-channel display config */
    void set_visible(bool v) { _visible = v; }
    bool visible() const { return _visible; }
    void set_v_offset(float o) { _v_offset = o; }
    float v_offset() const { return _v_offset; }
    void set_v_scale(float s) { _v_scale = (s >= 0.0f && s < 0.001f) ? 0.0f : s; }
    float v_scale() const { return _v_scale; }

    /** Per-channel engineering-unit calibration (independent of V-ZOOM). */
    void set_engineering_config(DecoderAnalogRangeMode mode,
                                double minimum, double maximum,
                                const std::string &unit);
    DecoderAnalogRangeMode range_mode() const { return _range_mode; }
    double engineering_minimum() const;
    double engineering_maximum() const;
    std::string engineering_unit() const { return _engineering_unit; }
    double engineering_value(float normalized) const;

    /** Unique ID */
    int64_t id() const { return _id; }

private:
    int _channel;
    int _num_channels;
    std::string _label;
    int64_t _id;
    bool    _visible = true;
    float   _v_offset = 1.0f;
    float   _v_scale = 0.0f;

    DecoderAnalogRangeMode _range_mode = DecoderAnalogRangeMode::Bipolar;
    double _engineering_minimum = -1.0;
    double _engineering_maximum = 1.0;
    std::string _engineering_unit = "V";

    std::vector<DecoderAnalogSample> _samples;
    mutable std::shared_mutex _mutex;

    // Incremental min/max cache — avoids rescanning the whole sample buffer
    // on every repaint for auto-scaling (was a per-frame O(N) scan over
    // hundreds of thousands of samples per channel).
    float _min_value;
    float _max_value;
    bool  _has_value;

    static std::atomic<int64_t> _next_id;
};

class DecoderAnalogData::ReadView {
public:
    explicit ReadView(const DecoderAnalogData &owner)
        : _owner(owner), _lock(owner._mutex) {}
    ReadView(ReadView &&) = default;
    ReadView &operator=(ReadView &&) = delete;
    ReadView(const ReadView &) = delete;
    ReadView &operator=(const ReadView &) = delete;

    const std::vector<DecoderAnalogSample> &samples() const {
        return _owner._samples;
    }
    float min_value() const {
        return _owner._has_value ? _owner._min_value : -1.0f;
    }
    float max_value() const {
        return _owner._has_value ? _owner._max_value : 1.0f;
    }

private:
    const DecoderAnalogData &_owner;
    std::shared_lock<std::shared_mutex> _lock;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_DECODERANALOGDATA_H
