/* SPDX-License-Identifier: GPL-2.0+ */
#include "decoderanalogdata.h"

#include <cmath>

namespace pv {
namespace data {

std::atomic<int64_t> DecoderAnalogData::_next_id{0};

DecoderAnalogData::DecoderAnalogData(int channel, int num_channels,
                                     const std::string &label)
    : _channel(channel)
    , _num_channels(num_channels)
    , _label(label)
    , _id(_next_id++)
    , _min_value(0.0f)
    , _max_value(0.0f)
    , _has_value(false)
{
}

DecoderAnalogData::~DecoderAnalogData()
{
}

void DecoderAnalogData::append_sample(uint64_t start_sample,
                                       uint64_t end_sample, float value)
{
    std::unique_lock<std::shared_mutex> lock(_mutex);
    _samples.push_back({start_sample, end_sample, value});
    if (!_has_value) {
        _min_value = _max_value = value;
        _has_value = true;
    } else {
        if (value < _min_value) _min_value = value;
        if (value > _max_value) _max_value = value;
    }
}

void DecoderAnalogData::append_samples(uint64_t start_sample,
                                       uint64_t end_sample,
                                       const float *values, size_t count)
{
    if (!values || count == 0)
        return;

    std::unique_lock<std::shared_mutex> lock(_mutex);

    // Do not reserve exactly one callback's worth of additional space here.
    // TDM audio arrives in many batches; exact growth makes every batch move
    // the complete history and turns a long decode into O(N^2) memory work.
    // Grow geometrically so appends remain amortized O(1), while still making
    // enough room to keep this whole callback under one lock.
    const size_t required = _samples.size() + count;
    if (required > _samples.capacity()) {
        const size_t capacity = _samples.capacity();
        const size_t grown = capacity > _samples.max_size() / 2
                                 ? _samples.max_size()
                                 : capacity * 2;
        _samples.reserve(std::max(required, grown));
    }

    const uint64_t span = end_sample - start_sample;
    const uint64_t quotient = span / count;
    const uint64_t remainder = span % count;
    uint64_t ss = start_sample;
    uint64_t error = 0;

    for (size_t i = 0; i < count; ++i) {
        uint64_t step = quotient;
        if (remainder != 0) {
            const uint64_t threshold = (uint64_t)count - remainder;
            if (error >= threshold) {
                ++step;
                error -= threshold;
            } else {
                error += remainder;
            }
        }
        const uint64_t es = ss + step;
        const float value = values[i];
        _samples.push_back({ss, es, value});
        ss = es;

        if (!_has_value) {
            _min_value = _max_value = value;
            _has_value = true;
        } else {
            if (value < _min_value) _min_value = value;
            if (value > _max_value) _max_value = value;
        }
    }
}

void DecoderAnalogData::append_samples_timed(const uint64_t *start_samples,
                                              const uint64_t *end_samples,
                                              const float *values, size_t count)
{
    if (!start_samples || !end_samples || !values || count == 0)
        return;

    std::unique_lock<std::shared_mutex> lock(_mutex);

    const size_t required = _samples.size() + count;
    if (required > _samples.capacity()) {
        const size_t capacity = _samples.capacity();
        const size_t grown = capacity > _samples.max_size() / 2
                                 ? _samples.max_size()
                                 : capacity * 2;
        _samples.reserve(std::max(required, grown));
    }

    for (size_t i = 0; i < count; ++i) {
        const float value = values[i];
        _samples.push_back({start_samples[i], end_samples[i], value});

        if (!_has_value) {
            _min_value = _max_value = value;
            _has_value = true;
        } else {
            if (value < _min_value) _min_value = value;
            if (value > _max_value) _max_value = value;
        }
    }
}

void DecoderAnalogData::clear()
{
    std::unique_lock<std::shared_mutex> lock(_mutex);
    _samples.clear();
    _has_value = false;
    _min_value = _max_value = 0.0f;
}

size_t DecoderAnalogData::get_sample_count() const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _samples.size();
}

float DecoderAnalogData::get_value_at(uint64_t sample) const
{
    DecoderAnalogSample found{};
    return get_sample_at(sample, found) ? found.value : 0.0f;
}

bool DecoderAnalogData::get_sample_at(uint64_t sample,
                                      DecoderAnalogSample &out) const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);
    if (_samples.empty() || sample < _samples.front().start_sample ||
        sample > _samples.back().end_sample)
        return false;

    // First decoded span whose end reaches the requested capture sample.
    size_t lo = 0, hi = _samples.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (_samples[mid].end_sample < sample)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= _samples.size() || sample < _samples[lo].start_sample)
        return false;

    out = _samples[lo];
    return true;
}

bool DecoderAnalogData::get_statistics(
    uint64_t start_sample, uint64_t end_sample,
    DecoderAnalogStatistics &out) const
{
    out = DecoderAnalogStatistics{};
    if (end_sample < start_sample)
        std::swap(start_sample, end_sample);

    std::shared_lock<std::shared_mutex> lock(_mutex);
    if (_samples.empty() || end_sample < _samples.front().start_sample ||
        start_sample > _samples.back().start_sample)
        return false;

    // Statistics use each decoded sample's timestamp exactly once. Sample
    // spans can share boundaries, so overlap-based inclusion would double
    // count values at those boundaries.
    size_t lo = 0;
    size_t hi = _samples.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (_samples[mid].start_sample < start_sample)
            lo = mid + 1;
        else
            hi = mid;
    }

    long double sum = 0.0L;
    long double sum_squares = 0.0L;
    for (size_t index = lo; index < _samples.size(); ++index) {
        const DecoderAnalogSample &sample = _samples[index];
        if (sample.start_sample > end_sample)
            break;

        const double value = static_cast<double>(sample.value);
        if (out.sample_count == 0) {
            out.minimum = value;
            out.maximum = value;
            out.first_sample = sample.start_sample;
        } else {
            out.minimum = std::min(out.minimum, value);
            out.maximum = std::max(out.maximum, value);
        }
        // Keep representative decoded-sample positions for rate estimation.
        // Using the enclosing span end would bias short selections low.
        out.last_sample = sample.start_sample;
        ++out.sample_count;
        sum += value;
        sum_squares += value * value;
    }

    if (out.sample_count == 0)
        return false;

    const long double count = static_cast<long double>(out.sample_count);
    out.mean = static_cast<double>(sum / count);
    out.rms = std::sqrt(static_cast<double>(sum_squares / count));
    out.valid = true;
    return true;
}

bool DecoderAnalogData::get_range_cycle_metrics(
    uint64_t start_sample, uint64_t end_sample,
    DecoderAnalogCycleMetrics &out) const
{
    out = DecoderAnalogCycleMetrics{};
    if (end_sample < start_sample)
        std::swap(start_sample, end_sample);

    std::shared_lock<std::shared_mutex> lock(_mutex);
    if (_samples.size() < 8)
        return false;

    const auto first_it = std::lower_bound(
        _samples.begin(), _samples.end(), start_sample,
        [](const DecoderAnalogSample &sample, uint64_t position) {
            return sample.start_sample < position;
        });
    const auto last_it = std::upper_bound(
        first_it, _samples.end(), end_sample,
        [](uint64_t position, const DecoderAnalogSample &sample) {
            return position < sample.start_sample;
        });
    const size_t first = (size_t)(first_it - _samples.begin());
    const size_t last = (size_t)(last_it - _samples.begin());
    if (last - first < 8)
        return false;

    double raw_min = _samples[first].value;
    double raw_max = raw_min;
    for (size_t i = first + 1; i < last; ++i) {
        raw_min = std::min(raw_min, (double)_samples[i].value);
        raw_max = std::max(raw_max, (double)_samples[i].value);
    }
    const double raw_range = raw_max - raw_min;
    if (!std::isfinite(raw_range) || raw_range < 1e-12)
        return false;

    // Estimate settled low/high levels from the outer 20% of the local value
    // range. This suppresses ordinary plateau noise while leaving overshoot
    // peaks available for comparison with the settled levels.
    const double low_limit = raw_min + raw_range * 0.20;
    const double high_limit = raw_max - raw_range * 0.20;
    long double low_sum = 0.0L;
    long double high_sum = 0.0L;
    size_t low_count = 0;
    size_t high_count = 0;
    for (size_t i = first; i < last; ++i) {
        const double value = _samples[i].value;
        if (value <= low_limit) {
            low_sum += value;
            ++low_count;
        }
        if (value >= high_limit) {
            high_sum += value;
            ++high_count;
        }
    }
    if (low_count == 0 || high_count == 0)
        return false;

    const double low_level = (double)(low_sum / low_count);
    const double high_level = (double)(high_sum / high_count);
    const double amplitude = high_level - low_level;
    if (!std::isfinite(amplitude) || amplitude < 1e-12)
        return false;

    const double threshold10 = low_level + amplitude * 0.10;
    const double threshold50 = low_level + amplitude * 0.50;
    const double threshold90 = low_level + amplitude * 0.90;

    std::vector<double> rising10, rising50, rising90;
    std::vector<double> falling90, falling50, falling10;
    rising10.reserve(32);
    rising50.reserve(32);
    rising90.reserve(32);
    falling90.reserve(32);
    falling50.reserve(32);
    falling10.reserve(32);

    const auto crossing_position = [](const DecoderAnalogSample &a,
                                      const DecoderAnalogSample &b,
                                      double threshold) {
        const double dv = (double)b.value - (double)a.value;
        if (std::abs(dv) < 1e-20)
            return (double)a.start_sample;
        const double fraction = std::clamp(
            (threshold - (double)a.value) / dv, 0.0, 1.0);
        return (double)a.start_sample +
               fraction * ((double)b.start_sample -
                           (double)a.start_sample);
    };

    for (size_t i = first + 1; i < last; ++i) {
        const DecoderAnalogSample &a = _samples[i - 1];
        const DecoderAnalogSample &b = _samples[i];
        if (b.start_sample <= a.start_sample)
            continue;
        if (a.value < threshold10 && b.value >= threshold10)
            rising10.push_back(crossing_position(a, b, threshold10));
        if (a.value < threshold50 && b.value >= threshold50)
            rising50.push_back(crossing_position(a, b, threshold50));
        if (a.value < threshold90 && b.value >= threshold90)
            rising90.push_back(crossing_position(a, b, threshold90));
        if (a.value > threshold90 && b.value <= threshold90)
            falling90.push_back(crossing_position(a, b, threshold90));
        if (a.value > threshold50 && b.value <= threshold50)
            falling50.push_back(crossing_position(a, b, threshold50));
        if (a.value > threshold10 && b.value <= threshold10)
            falling10.push_back(crossing_position(a, b, threshold10));
    }

    if (rising50.size() < 2)
        return false;

    // Logic 2-style range measurements summarize all complete cycles that
    // fall inside the selected annotation. Partial cycles at either edge do
    // not influence pulse widths, duty cycle, or edge timing.
    long double period_sum = 0.0L;
    long double positive_width_sum = 0.0L;
    long double negative_width_sum = 0.0L;
    long double positive_duty_sum = 0.0L;
    long double negative_duty_sum = 0.0L;
    long double rise_sum = 0.0L;
    long double fall_sum = 0.0L;
    size_t time_count = 0;
    size_t rise_count = 0;
    size_t fall_count = 0;

    for (size_t cycle_index = 0;
         cycle_index + 1 < rising50.size(); ++cycle_index) {
        const double cycle_start = rising50[cycle_index];
        const double cycle_end = rising50[cycle_index + 1];
        const double period = cycle_end - cycle_start;
        if (!std::isfinite(period) || period <= 0.0)
            continue;

        auto falling_mid = std::upper_bound(
            falling50.begin(), falling50.end(), cycle_start);
        if (falling_mid == falling50.end() || *falling_mid >= cycle_end)
            continue;

        const double positive_width = *falling_mid - cycle_start;
        const double negative_width = cycle_end - *falling_mid;
        period_sum += period;
        positive_width_sum += positive_width;
        negative_width_sum += negative_width;
        positive_duty_sum += positive_width * 100.0 / period;
        negative_duty_sum += negative_width * 100.0 / period;
        ++time_count;

        auto rise10 = std::upper_bound(
            rising10.begin(), rising10.end(), cycle_start);
        auto rise90 = std::lower_bound(
            rising90.begin(), rising90.end(), cycle_start);
        if (rise10 != rising10.begin() && rise90 != rising90.end() &&
            *rise90 < cycle_end) {
            --rise10;
            const double duration = *rise90 - *rise10;
            if (duration >= 0.0 && duration <= period * 0.5) {
                rise_sum += duration;
                ++rise_count;
            }
        }

        auto fall90 = std::upper_bound(
            falling90.begin(), falling90.end(), *falling_mid);
        auto fall10 = std::lower_bound(
            falling10.begin(), falling10.end(), *falling_mid);
        if (fall90 != falling90.begin() && fall10 != falling10.end() &&
            *fall10 < cycle_end) {
            --fall90;
            const double duration = *fall10 - *fall90;
            if (duration >= 0.0 && duration <= period * 0.5) {
                fall_sum += duration;
                ++fall_count;
            }
        }
    }

    if (time_count > 0) {
        const long double count = (long double)time_count;
        out.period_samples = (double)(period_sum / count);
        out.positive_width_samples = (double)(positive_width_sum / count);
        out.negative_width_samples = (double)(negative_width_sum / count);
        out.positive_duty_cycle = (double)(positive_duty_sum / count);
        out.negative_duty_cycle = (double)(negative_duty_sum / count);
        out.time_valid = true;
    }
    if (rise_count > 0) {
        out.rise_samples = (double)(rise_sum / (long double)rise_count);
        out.rise_valid = true;
    }
    if (fall_count > 0) {
        out.fall_samples = (double)(fall_sum / (long double)fall_count);
        out.fall_valid = true;
    }

    const uint64_t cycle_first =
        rising50.front() <= 0.0
            ? 0
            : (uint64_t)std::ceil(rising50.front());
    const uint64_t cycle_last =
        rising50.back() <= 0.0
            ? 0
            : (uint64_t)std::floor(rising50.back());
    auto cycle_it = std::lower_bound(
        _samples.begin() + first, _samples.begin() + last, cycle_first,
        [](const DecoderAnalogSample &sample, uint64_t position) {
            return sample.start_sample < position;
        });
    long double cycle_sum = 0.0L;
    long double cycle_sum_squares = 0.0L;
    size_t cycle_count = 0;
    double cycle_min = 0.0;
    double cycle_max = 0.0;
    for (; cycle_it != _samples.begin() + last &&
           cycle_it->start_sample <= cycle_last; ++cycle_it) {
        const double value = cycle_it->value;
        if (cycle_count == 0) {
            cycle_min = cycle_max = value;
        } else {
            cycle_min = std::min(cycle_min, value);
            cycle_max = std::max(cycle_max, value);
        }
        cycle_sum += value;
        cycle_sum_squares += value * value;
        ++cycle_count;
    }

    if (cycle_count > 0) {
        const long double count = (long double)cycle_count;
        out.cycle_mean = (double)(cycle_sum / count);
        out.cycle_rms = std::sqrt((double)(cycle_sum_squares / count));
        out.cycle_rms_valid = true;
        out.positive_overshoot =
            std::max(0.0, (cycle_max - high_level) * 100.0 / amplitude);
        out.negative_overshoot =
            std::max(0.0, (low_level - cycle_min) * 100.0 / amplitude);
        out.overshoot_valid = true;
    }

    out.valid = out.time_valid || out.rise_valid || out.fall_valid ||
                out.overshoot_valid || out.cycle_rms_valid;
    return out.valid;
}

DecoderAnalogData::ReadView DecoderAnalogData::read_samples() const
{
    return ReadView(*this);
}

void DecoderAnalogData::copy_samples_for_render(
    uint64_t start_sample, uint64_t end_sample, size_t max_samples,
    std::vector<DecoderAnalogSample> &out, float &min_value,
    float &max_value) const
{
    out.clear();
    min_value = -1.0f;
    max_value = 1.0f;
    if (max_samples == 0 || end_sample < start_sample)
        return;

    // Allocate outside the data lock. The caller normally reuses this vector,
    // so subsequent paint frames do not allocate at all.
    out.reserve(max_samples + 1);

    std::shared_lock<std::shared_mutex> lock(_mutex);
    if (_samples.empty())
        return;

    if (_has_value) {
        min_value = _min_value;
        max_value = _max_value;
    }

    // First sample whose span reaches the visible range.
    size_t lo = 0;
    size_t hi = _samples.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (_samples[mid].end_sample < start_sample)
            lo = mid + 1;
        else
            hi = mid;
    }
    const size_t first = lo;

    // One-past-last sample starting inside the visible range.
    lo = first;
    hi = _samples.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (_samples[mid].start_sample <= end_sample)
            lo = mid + 1;
        else
            hi = mid;
    }
    const size_t last = lo;
    if (first >= last)
        return;

    const size_t visible_count = last - first;
    const size_t stride =
        std::max<size_t>(1, (visible_count + max_samples - 1) / max_samples);
    size_t index = first;
    for (; index < last; index += stride)
        out.push_back(_samples[index]);

    // Preserve the range endpoint even when stride skipped it.
    if (out.empty() || out.back().start_sample != _samples[last - 1].start_sample)
        out.push_back(_samples[last - 1]);
}

float DecoderAnalogData::min_value() const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);
    if (!_has_value)
        return -1.0f;
    return _min_value;
}

float DecoderAnalogData::max_value() const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);
    if (!_has_value)
        return 1.0f;
    return _max_value;
}

void DecoderAnalogData::set_engineering_config(
    DecoderAnalogRangeMode mode, double minimum, double maximum,
    const std::string &unit)
{
    _range_mode = mode;
    _engineering_minimum = std::isfinite(minimum) ? minimum : -1.0;
    _engineering_maximum = std::isfinite(maximum) ? maximum : 1.0;
    _engineering_unit = unit.empty() ? "V" : unit;

    if (_range_mode == DecoderAnalogRangeMode::Custom &&
        _engineering_minimum > _engineering_maximum)
        std::swap(_engineering_minimum, _engineering_maximum);
}

double DecoderAnalogData::engineering_minimum() const
{
    switch (_range_mode) {
    case DecoderAnalogRangeMode::Bipolar:
        return -std::abs(_engineering_maximum);
    case DecoderAnalogRangeMode::Unipolar:
        return 0.0;
    case DecoderAnalogRangeMode::Custom:
        return _engineering_minimum;
    }
    return _engineering_minimum;
}

double DecoderAnalogData::engineering_maximum() const
{
    return _range_mode == DecoderAnalogRangeMode::Custom
               ? _engineering_maximum
               : std::abs(_engineering_maximum);
}

double DecoderAnalogData::engineering_value(float normalized) const
{
    const double raw = static_cast<double>(normalized);
    const double minimum = engineering_minimum();
    const double maximum = engineering_maximum();
    return minimum + (raw + 1.0) * 0.5 * (maximum - minimum);
}

} // namespace data
} // namespace pv
