#include "pv/api/binary_codec.h"

#include <cstring>

namespace pv::api {

// ---- Varint ----

void BinaryCodec::encode_varint(std::vector<uint8_t>& out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

uint64_t BinaryCodec::decode_varint(const uint8_t* data, size_t len, size_t& bytes_consumed) {
    uint64_t result = 0;
    int shift = 0;
    bytes_consumed = 0;
    for (size_t i = 0; i < len; ++i) {
        result |= static_cast<uint64_t>(data[i] & 0x7F) << shift;
        bytes_consumed = i + 1;
        if (!(data[i] & 0x80))
            break;
        shift += 7;
    }
    return result;
}

// ---- Header ----

void BinaryCodec::write_header(std::vector<uint8_t>& out,
                               BinaryFrameType type,
                               uint16_t signal_mask,
                               uint32_t timestamp_ms) {
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(static_cast<uint8_t>(signal_mask & 0xFF));
    out.push_back(static_cast<uint8_t>((signal_mask >> 8) & 0xFF));
    // reserved byte
    out.push_back(0x00);
    // timestamp (little-endian uint32)
    out.push_back(static_cast<uint8_t>(timestamp_ms & 0xFF));
    out.push_back(static_cast<uint8_t>((timestamp_ms >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((timestamp_ms >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((timestamp_ms >> 24) & 0xFF));
}

// ---- Logic edge encoding ----

std::vector<uint8_t> BinaryCodec::encode_logic_edges(
        uint32_t timestamp_ms,
        uint64_t start_sample,
        const std::vector<std::vector<std::pair<uint64_t, uint8_t>>>& edges_per_channel,
        uint16_t signal_mask) {

    std::vector<uint8_t> out;
    write_header(out, BinaryFrameType::LogicEdges, signal_mask, timestamp_ms);

    // start_sample (uint64 LE) so the client knows the base offset
    for (int b = 0; b < 8; ++b)
        out.push_back(static_cast<uint8_t>((start_sample >> (b * 8)) & 0xFF));

    // For each bit set in signal_mask, encode the channel's edges
    for (int ch = 0; ch < 16; ++ch) {
        if (!(signal_mask & (1 << ch)))
            continue;

        const auto* edges = (ch < static_cast<int>(edges_per_channel.size()))
                          ? &edges_per_channel[ch] : nullptr;

        uint16_t edge_count = edges ? static_cast<uint16_t>(edges->size()) : 0;
        // edge_count (uint16 LE)
        out.push_back(static_cast<uint8_t>(edge_count & 0xFF));
        out.push_back(static_cast<uint8_t>((edge_count >> 8) & 0xFF));

        if (edges) {
            for (const auto& [abs_pos, level] : *edges) {
                // Position is relative to start_sample
                uint64_t rel_pos = (abs_pos >= start_sample)
                                 ? (abs_pos - start_sample) : 0;
                encode_varint(out, rel_pos);
                out.push_back(level ? 1 : 0);
            }
        }
    }

    return out;
}

// ---- Analog/DSO envelope encoding ----

std::vector<uint8_t> BinaryCodec::encode_analog_envelope(
        uint32_t timestamp_ms,
        uint64_t start_sample,
        uint32_t scale,
        const std::vector<std::vector<float>>& envelopes_per_channel,
        uint16_t signal_mask) {

    std::vector<uint8_t> out;
    write_header(out, BinaryFrameType::AnalogEnvelope, signal_mask, timestamp_ms);

    // start_sample (uint64 LE)
    for (int b = 0; b < 8; ++b)
        out.push_back(static_cast<uint8_t>((start_sample >> (b * 8)) & 0xFF));

    for (int ch = 0; ch < 16; ++ch) {
        if (!(signal_mask & (1 << ch)))
            continue;

        const auto* env = (ch < static_cast<int>(envelopes_per_channel.size()))
                        ? &envelopes_per_channel[ch] : nullptr;

        // scale (uint32 LE)
        out.push_back(static_cast<uint8_t>(scale & 0xFF));
        out.push_back(static_cast<uint8_t>((scale >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((scale >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((scale >> 24) & 0xFF));

        uint32_t length = env ? static_cast<uint32_t>(env->size() / 2) : 0;
        // length (uint32 LE)
        out.push_back(static_cast<uint8_t>(length & 0xFF));
        out.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((length >> 24) & 0xFF));

        if (env) {
            // Interleaved min/max float32 pairs
            for (size_t i = 0; i + 1 < env->size(); i += 2) {
                float vals[2] = {(*env)[i], (*env)[i + 1]};
                uint8_t bytes[8];
                std::memcpy(bytes, vals, 8);
                out.insert(out.end(), bytes, bytes + 8);
            }
        }
    }

    return out;
}

// ---- Viewport reset ----

std::vector<uint8_t> BinaryCodec::encode_viewport_reset(
        uint32_t timestamp_ms,
        uint64_t start_sample,
        uint64_t end_sample,
        int32_t  width_px) {

    std::vector<uint8_t> out;
    write_header(out, BinaryFrameType::ViewportReset, 0, timestamp_ms);

    // start_sample (uint64 LE)
    for (int b = 0; b < 8; ++b)
        out.push_back(static_cast<uint8_t>((start_sample >> (b * 8)) & 0xFF));
    // end_sample (uint64 LE)
    for (int b = 0; b < 8; ++b)
        out.push_back(static_cast<uint8_t>((end_sample >> (b * 8)) & 0xFF));
    // width_px (int32 LE)
    uint32_t w = static_cast<uint32_t>(width_px);
    out.push_back(static_cast<uint8_t>(w & 0xFF));
    out.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((w >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((w >> 24) & 0xFF));

    return out;
}

} // namespace pv::api
