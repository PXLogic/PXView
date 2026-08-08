#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace pv::api {

// ============================================================================
// P0-3: Binary frame codec for efficient waveform data transport over WS.
//
// Frame format (all multi-byte fields are little-endian):
//
//   [Header 8 bytes]
//     byte 0:    frame_type
//     byte 1:    signal_mask (bit0=ch0, bit1=ch1, ...)
//     byte 2-3:  reserved (0)
//     byte 4-7:  timestamp_ms (uint32)
//
//   [Payload — depends on frame_type]
//
// Frame types:
//   0x01 = Logic edge data
//   0x02 = Analog/DSO envelope data
//   0x03 = Decoder annotation update
//   0x04 = State event (JSON, sent as text frame instead)
//   0x05 = Error event
//   0x06 = Viewport reset
//
// Logic edge payload (frame_type = 0x01):
//   For each signal in signal_mask (ascending order):
//     [edge_count: uint16]
//     [edge_count × (position: varint, value: uint8)]
//   position is relative to the start_sample of this frame.
//   varint encoding: 7 bits per byte, high bit = continuation.
//
// Analog/DSO envelope payload (frame_type = 0x02):
//   For each signal in signal_mask (ascending order):
//     [start_sample: uint64]
//     [scale:        uint32]   — samples per min/max pair
//     [length:       uint32]   — number of min/max pairs
//     [length × 2 × float32]   — interleaved min, max, min, max, ...
//
// ============================================================================

enum class BinaryFrameType : uint8_t {
    LogicEdges      = 0x01,
    AnalogEnvelope  = 0x02,
    DecoderUpdate   = 0x03,
    StateEvent      = 0x04,
    ErrorEvent      = 0x05,
    ViewportReset   = 0x06,
};

class BinaryCodec {
public:
    // ---- Encoding ----

    // Build a logic-edge binary frame.
    // edges: for each channel, a vector of (position, level) pairs.
    // timestamp_ms: millisecond timestamp for the frame header.
    // start_sample: the absolute sample position this frame's edges are relative to.
    static std::vector<uint8_t> encode_logic_edges(
        uint32_t timestamp_ms,
        uint64_t start_sample,
        const std::vector<std::vector<std::pair<uint64_t, uint8_t>>>& edges_per_channel,
        uint16_t signal_mask);

    // Build an analog/DSO envelope binary frame.
    // envelopes: for each channel, a flat vector of min/max float pairs.
    // start_sample, scale: metadata for the envelope section.
    static std::vector<uint8_t> encode_analog_envelope(
        uint32_t timestamp_ms,
        uint64_t start_sample,
        uint32_t scale,
        const std::vector<std::vector<float>>& envelopes_per_channel,
        uint16_t signal_mask);

    // Build a viewport-reset binary frame (sent when viewport changes
    // so the client clears its canvas before drawing new data).
    static std::vector<uint8_t> encode_viewport_reset(
        uint32_t timestamp_ms,
        uint64_t start_sample,
        uint64_t end_sample,
        int32_t  width_px);

    // ---- Varint encoding (used internally, exposed for testing) ----
    static void encode_varint(std::vector<uint8_t>& out, uint64_t value);
    static uint64_t decode_varint(const uint8_t* data, size_t len, size_t& bytes_consumed);

    // ---- Header helpers ----
    static void write_header(std::vector<uint8_t>& out,
                             BinaryFrameType type,
                             uint16_t signal_mask,
                             uint32_t timestamp_ms);
};

} // namespace pv::api
