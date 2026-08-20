/*
 * test_rasterize.cpp — modernize-thread-model Task 2.6
 *
 * Direct unit test of the pure waveform rasterizers:
 *   rasterize_logic_channel / rasterize_dso_channel / rasterize_analog_channel
 * (pv/view/renderer/rasterize.h)
 *
 * The point of this test is that these functions can be exercised WITHOUT
 * constructing any View/QWidget — just a snapshot + value parameters drawn
 * into a QImage-backed QPainter, then assert on the resulting pixels.
 * Pixel parity vs. the old member methods is verified separately by the
 * Task 2.7 screenshot diff (manual); here we assert behavioural invariants:
 *   - the waveform actually draws (non-transparent pixels in the channel band)
 *   - the glitch live-preview overlay draws (orange rect over the band)
 *   - both DSO/Analog dual-modes (spp<1 polyline, spp>=1 min/max) draw
 *
 * 依赖链: rasterize.cpp + snapshot/logicsnapshot 全套 + dsosnapshot +
 * analogsnapshot + mmap_allocator + libsigrok 头 + common (xlog.h)。
 * 内部 stub xlog; QImage/QPainter 属 QtGui (offscreen platform 可用)。
 * 无 View/QWidget 依赖。
 */

#include <QtTest/QtTest>

// ── 标准库头必须在目标头之前 include ──
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <libsigrok/libsigrok.h>   // sr_datafeed_logic / sr_datafeed_dso / sr_channel / GSList

// ── xlog stub: rasterize.cpp + snapshot 源文件经 log.h 引用 pxv_log + xlog_* ──
#include "log/xlog.h"
xlog_writer *pxv_log = nullptr;
extern "C" {
int xlog_err(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_warn(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_info(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_dbg(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_detail(xlog_writer *w, const char *, ...) { (void)w; return 0; }
}

#include "pv/view/renderer/rasterize.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/snapshot/dsosnapshot.h"
#include "pv/data/snapshot/analogsnapshot.h"

using namespace pv::data;

namespace {

// ---- LogicSnapshot fixture (square wave on ch0, complement on ch1) ----
struct LogicFixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;
    std::vector<uint8_t> data;
    sr_datafeed_logic logic{};

    LogicFixture(size_t ch_count, size_t total_samples)
        : chs(ch_count), nodes(ch_count)
    {
        for (size_t i = 0; i < ch_count; ++i) {
            chs[i].index = (int)i;
            chs[i].type = SR_CHANNEL_LOGIC;
            chs[i].enabled = TRUE;
            chs[i].name = nullptr;
            nodes[i].data = &chs[i];
            nodes[i].next = (i + 1 < ch_count) ? &nodes[i + 1] : nullptr;
        }
        logic.length = 0;
        logic.unitsize = (uint8_t)((ch_count + 7) / 8);
        logic.format = 0;
        logic.data = nullptr;
        (void)total_samples;
    }

    // bit_fn(s, ch) defines the level of channel `ch` at sample `s`.
    void feed(LogicSnapshot &snap, uint64_t total_sample_count,
              const std::function<bool(size_t s, int ch)> &bit_fn)
    {
        data.resize(total_sample_count);
        for (size_t s = 0; s < total_sample_count; ++s) {
            uint8_t v = 0;
            if (bit_fn(s, 0)) v |= 0x01;
            if (bit_fn(s, 1)) v |= 0x02;
            data[s] = v;
        }
        sr_datafeed_logic l = logic;
        l.length = (uint64_t)data.size();
        l.data = data.data();
        l.unitsize = 1;
        snap.first_payload(l, total_sample_count, &nodes[0], true);
        snap.append_payload(l);
        snap.capture_ended();
        snap.set_samplerate(1000.0);
    }
};

// ---- DsoSnapshot fixture (single 8-bit channel) ----
struct DsoFixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;
    sr_datafeed_dso dso{};

    explicit DsoFixture(const std::vector<int> &ch_indexes)
        : chs(ch_indexes.size()), nodes(ch_indexes.size())
    {
        for (size_t i = 0; i < chs.size(); ++i) {
            chs[i].index = ch_indexes[i];
            chs[i].type = SR_CHANNEL_DSO;
            chs[i].enabled = TRUE;
            chs[i].name = nullptr;
            nodes[i].data = &chs[i];
            nodes[i].next = (i + 1 < chs.size()) ? &nodes[i + 1] : nullptr;
        }
    }

    void feed(DsoSnapshot &snap, uint64_t total, const void *data,
              uint32_t num_samples)
    {
        dso.data = const_cast<void *>(data);
        dso.num_samples = num_samples;
        dso.trig_flag = 0;
        dso.trig_ch = 0;
        dso.en_ch_num = (uint8_t)chs.size();
        dso.sample_bits = 8;
        dso.trig_offset = 0;
        dso.packet_len = 0;
        dso.samplerate_tog = 0;
        snap.first_payload(dso, total, &nodes[0], false, false);
        snap.capture_ended();
        snap.set_samplerate(1000.0);
    }
};

// ---- AnalogSnapshot fixture (single 8-bit integer channel) ----
struct AnalogFixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;
    sr_analog_encoding encoding{};
    sr_analog_meaning meaning{};
    sr_analog_spec spec{};
    sr_datafeed_analog analog{};

    AnalogFixture(const std::vector<int> &ch_indexes, uint8_t unitsize, bool is_float)
        : chs(ch_indexes.size()), nodes(ch_indexes.size())
    {
        for (size_t i = 0; i < chs.size(); ++i) {
            chs[i].index = ch_indexes[i];
            chs[i].type = SR_CHANNEL_ANALOG;
            chs[i].enabled = TRUE;
            chs[i].name = nullptr;
            nodes[i].data = &chs[i];
            nodes[i].next = (i + 1 < chs.size()) ? &nodes[i + 1] : nullptr;
        }
        encoding.unitsize = unitsize;
        encoding.is_float = is_float ? TRUE : FALSE;
        encoding.is_signed = FALSE;
        encoding.is_bigendian = FALSE;
        encoding.digits = 0;
        encoding.is_digits_decimal = FALSE;
        meaning.mq = SR_MQ_VOLTAGE;
        meaning.unit = SR_UNIT_VOLT;
        meaning.mqflags = static_cast<sr_mqflag>(0);
        meaning.channels = nullptr;
        analog.data = nullptr;
        analog.num_samples = 0;
        analog.encoding = &encoding;
        analog.meaning = &meaning;
        analog.spec = &spec;
    }

    void feed_all_channels(AnalogSnapshot &snap, uint64_t total,
                           const void *data, uint32_t num_samples)
    {
        meaning.channels = &nodes[0];
        analog.data = const_cast<void *>(data);
        analog.num_samples = num_samples;
        snap.first_payload(analog, total, &nodes[0]);
        snap.capture_ended();
        snap.set_samplerate(1000.0);
    }
};

// Count pixels with alpha > 0 inside `rect` (clamped to the image).
int countOpaque(const QImage &img, const QRect &rect)
{
    int n = 0;
    const QRect r = rect.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (img.pixelColor(x, y).alpha() > 0)
                ++n;
    return n;
}

} // anonymous namespace

class TestRasterize : public QObject
{
    Q_OBJECT

private slots:
    // Logic: square wave draws non-transparent pixels in the channel band.
    void logic_waveform_draws_pixels();
    // Logic: glitch live-preview ranges draw the full-band overlay.
    void logic_glitch_preview_overlay();
    // Logic: no preview -> the overlay region stays transparent.
    void logic_no_preview_no_overlay();
    // DSO: min/max mode (spp >= 1) draws.
    void dso_minmax_mode_draws();
    // DSO: polyline mode (spp < 1) draws.
    void dso_polyline_mode_draws();
    // Analog: min/max mode (spp >= 1) draws.
    void analog_minmax_mode_draws();
    // Analog: polyline mode (spp < 1) draws.
    void analog_polyline_mode_draws();
};

void TestRasterize::logic_waveform_draws_pixels()
{
    const size_t N = 200;
    LogicFixture fx(2, N);
    LogicSnapshot snap;
    // ch0: 50% duty square wave, period 40 samples. ch1: all low.
    fx.feed(snap, N, [](size_t s, int ch) {
        if (ch == 0)
            return (s % 40) < 20;
        return false;
    });

    const int left = 0, right = 200, y = 25, total_height = 20;
    const int high_offset = y - total_height;   // 5
    const int low_offset = y;                   // 25

    QImage img(right, total_height * 2, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);

    pv::view::PaintContext ctx;
    ctx.scale = 0.001;   // samplerate 1000 * 0.001 => samples_per_pixel = 1
    ctx.offset = 0;
    pv::view::rasterize_logic_channel(
        p, &snap, 0, left, right, y, total_height, QColor(255, 255, 255),
        ctx.scale, 0, N - 1, ctx, nullptr);
    p.end();

    const QRect band(left, 0, right, total_height * 2);
    QVERIFY2(countOpaque(img, band) > 0,
             "logic waveform must draw non-transparent pixels in the band");
    // ch0 is high for samples 0..19  => horizontal line at high_offset (y=5).
    QVERIFY2(img.pixelColor(10, high_offset).alpha() > 0,
             "pixel on the high-level row must be drawn");
    // ch0 is low for samples 20..39 => horizontal line at low_offset (y=25).
    QVERIFY2(img.pixelColor(30, low_offset).alpha() > 0,
             "pixel on the low-level row must be drawn");
}

void TestRasterize::logic_glitch_preview_overlay()
{
    const size_t N = 200;
    LogicFixture fx(2, N);
    LogicSnapshot snap;
    fx.feed(snap, N, [](size_t s, int ch) {
        if (ch == 0)
            return (s % 40) < 20;
        return false;
    });

    const int left = 0, right = 200, y = 25, total_height = 20;
    // Mid-band row inside the preview rect: y in [high_offset, low_offset).
    const int mid_band = y - total_height / 2;

    QImage img(right, total_height * 2, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);

    pv::view::PaintContext ctx;
    ctx.scale = 0.001;
    ctx.offset = 0;
    // Samples 120..139 are HIGH (120%40==0..19), so the waveform line sits at
    // high_offset; the overlay must still cover the full band at those pixels.
    std::vector<pv::view::GlitchRange> preview = {{120, 140}};
    pv::view::rasterize_logic_channel(
        p, &snap, 0, left, right, y, total_height, QColor(255, 255, 255),
        ctx.scale, 0, N - 1, ctx, &preview);
    p.end();

    // Pixel (130, mid_band): outside the waveform line (it is on the high
    // row), but inside the preview rect (full band) => alpha > 0 (orange).
    QVERIFY2(img.pixelColor(130, mid_band).alpha() > 0,
             "glitch live-preview overlay must paint the full band");
}

void TestRasterize::logic_no_preview_no_overlay()
{
    const size_t N = 200;
    LogicFixture fx(2, N);
    LogicSnapshot snap;
    fx.feed(snap, N, [](size_t s, int ch) {
        if (ch == 0)
            return (s % 40) < 20;
        return false;
    });

    const int left = 0, right = 200, y = 25, total_height = 20;
    const int mid_band = y - total_height / 2;

    QImage img(right, total_height * 2, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);

    pv::view::PaintContext ctx;
    ctx.scale = 0.001;
    ctx.offset = 0;
    pv::view::rasterize_logic_channel(
        p, &snap, 0, left, right, y, total_height, QColor(255, 255, 255),
        ctx.scale, 0, N - 1, ctx, nullptr);
    p.end();

    // Samples 120..139 are HIGH => the waveform line is at high_offset; the
    // mid-band row is not on the waveform and there is no overlay => transparent.
    QVERIFY2(img.pixelColor(130, mid_band).alpha() == 0,
             "without preview ranges no overlay must be drawn");
}

void TestRasterize::dso_minmax_mode_draws()
{
    const size_t N = 200;
    std::vector<uint8_t> data(N);
    for (size_t i = 0; i < N; ++i)
        data[i] = (uint8_t)(i & 0xFF);   // ramp 0..255, then repeats

    DsoFixture fx({0});
    DsoSnapshot snap;
    fx.feed(snap, N, data.data(), (uint32_t)N);

    QImage img(200, 30, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);

    // samples_per_pixel = 2 => min/max mode.
    pv::view::rasterize_dso_channel(
        p, &snap, 15, 0, 200, 0, (int64_t)N - 1, 128, 2.0, 0,
        0.0f, 30.0f, 0.1f, QColor(255, 255, 255));
    p.end();

    QVERIFY2(countOpaque(img, img.rect()) > 0,
             "DSO min/max mode must draw non-transparent pixels");
}

void TestRasterize::dso_polyline_mode_draws()
{
    const size_t N = 200;
    std::vector<uint8_t> data(N);
    for (size_t i = 0; i < N; ++i)
        data[i] = (uint8_t)(i & 0xFF);

    DsoFixture fx({0});
    DsoSnapshot snap;
    fx.feed(snap, N, data.data(), (uint32_t)N);

    QImage img(200, 30, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);

    // samples_per_pixel = 0.5 => polyline (interpolation) mode.
    pv::view::rasterize_dso_channel(
        p, &snap, 15, 0, 200, 0, (int64_t)N - 1, 128, 0.5, 0,
        0.0f, 30.0f, 0.1f, QColor(255, 255, 255));
    p.end();

    QVERIFY2(countOpaque(img, img.rect()) > 0,
             "DSO polyline mode must draw non-transparent pixels");
}

void TestRasterize::analog_minmax_mode_draws()
{
    const size_t N = 200;
    std::vector<uint8_t> data(N);
    for (size_t i = 0; i < N; ++i)
        data[i] = (uint8_t)(i & 0xFF);

    AnalogFixture fx({0}, 1, false);
    AnalogSnapshot snap;
    fx.feed_all_channels(snap, N, data.data(), (uint32_t)N);

    QImage img(200, 30, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);

    // samples_per_pixel = 2 => min/max mode.
    pv::view::rasterize_analog_channel(
        p, &snap, 15, 0, 200, 0, (int64_t)N, 2.0, 0,
        0.0f, 30.0f, 128, 0.1f, 1.0f, QColor(255, 255, 255));
    p.end();

    QVERIFY2(countOpaque(img, img.rect()) > 0,
             "analog min/max mode must draw non-transparent pixels");
}

void TestRasterize::analog_polyline_mode_draws()
{
    const size_t N = 200;
    std::vector<uint8_t> data(N);
    for (size_t i = 0; i < N; ++i)
        data[i] = (uint8_t)(i & 0xFF);

    AnalogFixture fx({0}, 1, false);
    AnalogSnapshot snap;
    fx.feed_all_channels(snap, N, data.data(), (uint32_t)N);

    QImage img(200, 30, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);

    // samples_per_pixel = 0.5 => polyline (interpolation) mode.
    pv::view::rasterize_analog_channel(
        p, &snap, 15, 0, 200, 0, (int64_t)N, 0.5, 0,
        0.0f, 30.0f, 128, 0.1f, 1.0f, QColor(255, 255, 255));
    p.end();

    QVERIFY2(countOpaque(img, img.rect()) > 0,
             "analog polyline mode must draw non-transparent pixels");
}

QTEST_MAIN(TestRasterize)
#include "test_rasterize.moc"
