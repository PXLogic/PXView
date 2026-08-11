/*
 * test_binary_codec.cpp — QTest unit tests for BinaryCodec
 *
 * New tests for P0 (API layer coverage).
 * Tests varint encoding/decoding and binary frame encoding.
 */

#include <QtTest>
#include <vector>
#include <cstdint>
#include "pv/api/binary_codec.h"

using pv::api::BinaryCodec;
using pv::api::BinaryFrameType;

class TestBinaryCodec : public QObject {
    Q_OBJECT
private slots:
    void EncodeVarintZero();
    void EncodeVarintOne();
    void EncodeVarint127();
    void EncodeVarint128();
    void EncodeVarint300();
    void EncodeVarintMax();
    void DecodeVarintZero();
    void DecodeVarintSingleByte();
    void DecodeVarintMultiByte();
    void DecodeVarintTruncated();
    void EncodeDecodeRoundTrip_data();
    void EncodeDecodeRoundTrip();
    void WriteHeaderProducesCorrectBytes();
    void EncodeViewportResetFrame();
    void EncodeLogicEdgesEmpty();
};

void TestBinaryCodec::EncodeVarintZero() {
    std::vector<uint8_t> out;
    BinaryCodec::encode_varint(out, 0);
    QCOMPARE(out.size(), 1u);
    QCOMPARE(out[0], 0x00);
}
void TestBinaryCodec::EncodeVarintOne() {
    std::vector<uint8_t> out;
    BinaryCodec::encode_varint(out, 1);
    QCOMPARE(out.size(), 1u);
    QCOMPARE(out[0], 0x01);
}
void TestBinaryCodec::EncodeVarint127() {
    std::vector<uint8_t> out;
    BinaryCodec::encode_varint(out, 127);
    QCOMPARE(out.size(), 1u);
    QCOMPARE(out[0], 0x7F);
}
void TestBinaryCodec::EncodeVarint128() {
    std::vector<uint8_t> out;
    BinaryCodec::encode_varint(out, 128);
    QCOMPARE(out.size(), 2u);
    QCOMPARE(out[0], 0x80);
    QCOMPARE(out[1], 0x01);
}
void TestBinaryCodec::EncodeVarint300() {
    std::vector<uint8_t> out;
    BinaryCodec::encode_varint(out, 300);
    QVERIFY(out.size() >= 2u);
    // 300 = 0x12C → varint: 0xAC 0x02
    QCOMPARE(out[0], 0xAC);
    QCOMPARE(out[1], 0x02);
}
void TestBinaryCodec::EncodeVarintMax() {
    std::vector<uint8_t> out;
    BinaryCodec::encode_varint(out, 0xFFFFFFFFFFFFFFFFULL);
    QCOMPARE(out.size(), 10u);
}
void TestBinaryCodec::DecodeVarintZero() {
    std::vector<uint8_t> data = {0x00};
    size_t consumed = 0;
    uint64_t val = BinaryCodec::decode_varint(data.data(), data.size(), consumed);
    QCOMPARE(val, 0ull);
    QCOMPARE(consumed, 1u);
}
void TestBinaryCodec::DecodeVarintSingleByte() {
    std::vector<uint8_t> data = {0x42};
    size_t consumed = 0;
    uint64_t val = BinaryCodec::decode_varint(data.data(), data.size(), consumed);
    QCOMPARE(val, 0x42ull);
    QCOMPARE(consumed, 1u);
}
void TestBinaryCodec::DecodeVarintMultiByte() {
    std::vector<uint8_t> data = {0xAC, 0x02};
    size_t consumed = 0;
    uint64_t val = BinaryCodec::decode_varint(data.data(), data.size(), consumed);
    QCOMPARE(val, 300ull);
    QCOMPARE(consumed, 2u);
}
void TestBinaryCodec::DecodeVarintTruncated() {
    std::vector<uint8_t> data = {0x80}; // continuation bit set but no more bytes
    size_t consumed = 0;
    uint64_t val = BinaryCodec::decode_varint(data.data(), data.size(), consumed);
    (void)val;
    // Should return 0 or handle gracefully; consumed should be 0 or 1
    QVERIFY(consumed <= 1u);
}
void TestBinaryCodec::EncodeDecodeRoundTrip_data() {
    QTest::addColumn<uint64_t>("value");
    QTest::newRow("zero") << 0ull;
    QTest::newRow("one") << 1ull;
    QTest::newRow("127") << 127ull;
    QTest::newRow("128") << 128ull;
    QTest::newRow("255") << 255ull;
    QTest::newRow("300") << 300ull;
    QTest::newRow("16383") << 16383ull;
    QTest::newRow("16384") << 16384ull;
    QTest::newRow("1M") << 1048576ull;
    QTest::newRow("max32") << 0xFFFFFFFFull;
    QTest::newRow("max64") << 0xFFFFFFFFFFFFFFFFull;
}
void TestBinaryCodec::EncodeDecodeRoundTrip() {
    QFETCH(uint64_t, value);
    std::vector<uint8_t> out;
    BinaryCodec::encode_varint(out, value);
    size_t consumed = 0;
    uint64_t decoded = BinaryCodec::decode_varint(out.data(), out.size(), consumed);
    QCOMPARE(decoded, value);
    QCOMPARE(consumed, out.size());
}
void TestBinaryCodec::WriteHeaderProducesCorrectBytes() {
    std::vector<uint8_t> out;
    BinaryCodec::write_header(out, BinaryFrameType::LogicEdges, 0x03, 12345);
    QCOMPARE(out.size(), 8u);
    QCOMPARE(out[0], static_cast<uint8_t>(BinaryFrameType::LogicEdges));
    QCOMPARE(out[1], 0x03);
    // bytes 2-3 are reserved (0)
    QCOMPARE(out[2], 0x00);
    QCOMPARE(out[3], 0x00);
    // bytes 4-7 are timestamp_ms (little-endian)
    uint32_t ts = out[4] | (out[5] << 8) | (out[6] << 16) | (out[7] << 24);
    QCOMPARE(ts, 12345u);
}
void TestBinaryCodec::EncodeViewportResetFrame() {
    auto frame = BinaryCodec::encode_viewport_reset(100, 0, 1000, 800);
    QVERIFY(frame.size() >= 8u);
    QCOMPARE(frame[0], static_cast<uint8_t>(BinaryFrameType::ViewportReset));
}
void TestBinaryCodec::EncodeLogicEdgesEmpty() {
    std::vector<std::vector<std::pair<uint64_t, uint8_t>>> empty;
    auto frame = BinaryCodec::encode_logic_edges(200, 0, empty, 0x00);
    QVERIFY(frame.size() >= 8u);
    QCOMPARE(frame[0], static_cast<uint8_t>(BinaryFrameType::LogicEdges));
}

QTEST_MAIN(TestBinaryCodec)
#include "test_binary_codec.moc"
