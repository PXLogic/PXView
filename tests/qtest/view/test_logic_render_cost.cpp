/*
 * test_logic_render_cost.cpp — logic 模式波形渲染成本量化（决策门禁）
 *
 * 目的
 * ----
 * logic 模式（TimeView + is_logic_rendering_mode）每帧要重建整幅信号位图：
 * SignalPixmapPass::render 对每个启用的 logic 通道调用
 * rasterize_logic_channel()（render_pass.cpp:455），后者内部经
 * LogicSnapshot::get_display_edges() 走 mipmap 边沿扫描 + 构建 wave_lines +
 * drawLines。
 *
 * 在改任何代码之前，先把这条纯路径的耗时量化出来，回答两个问题：
 *   1) 每通道耗时随「缩放级别（samples_per_pixel）」和「数据密度」怎么变？
 *      —— 若随可见样本数线性增长，说明 mipmap 没有起到加速作用。
 *   2) 16 通道整帧重建（fill + N × rasterize）相对 60 FPS / 30 FPS 帧预算占多少？
 *      —— 这决定 logic 模式"帧率低"是渲染本身超预算，还是别处（重绘频率、
 *     解码轨实时绘制）的问题。
 *
 * 本文件只测「纯渲染路径」，不构造 View/QWidget：
 *   依赖链同 test_rasterize（rasterize.cpp + snapshot 全套 + mmap_allocator +
 *   libsigrok 头 + common)，QPainter 画进 QImage。
 *
 * 不属于本测试的（需要 View/AppConfig/Session，纯单测覆盖不到）：
 *   - View/Viewport 层的重绘触发频率（viewport_update / progress timer）
 *   - DecodeTracePass 每帧实时绘制解码注解
 *   - SignalPixmapPass 里每通道的 GetThemeColor()/get_preview_ranges() 查找
 *
 * 测量纪律（改动此文件时必须保持）
 * --------------------------------
 * - 多轮取最小值（ROUNDS）抑制调度抖动；先预热一轮。
 * - 每个被测调用后必须用 volatile 读消费输出，否则 -O2 会消除不产生可见
 *   副作用的读取路径，得出假数字。
 * - 本目标强制 -O2（见 CMakeLists 注释）：DEBUG(-O0) 的绝对耗时不可用于决策。
 *
 * 读数约定：绝对耗时必须在 **Release / -O2** 下解读；相对趋势（缩放、通道数、
 * 密度）在任意构建类型下都成立。
 */

#include <QtTest/QtTest>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include <libsigrok/libsigrok.h>   // sr_datafeed_logic / sr_channel / GSList

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

#include <QImage>
#include <QLine>
#include <QPainter>
#include <QRect>

#include "pv/view/renderer/rasterize.h"
#include "pv/data/snapshot/logicsnapshot.h"

using namespace pv::data;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int ROUNDS = 5;          // 多轮取最小值
constexpr int WIDTH = 1920;        // 视口宽度（px）
constexpr int CHANNELS = 16;       // 逻辑通道数
constexpr int CH_HEIGHT = 20;      // 每通道波形带高（px）
constexpr uint64_t SAMPLES = 16ULL * 1024 * 1024;   // 16M 样本/通道
constexpr double SAMPLERATE = 1e9; // 1 GS/s → samples_per_pixel = 1e9 * scale

// 防止 -O2 消除被测读取路径。
volatile uint64_t g_sink = 0;

double ms_since(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double best_of(int rounds, const std::function<void()> &fn)
{
    double best = 1e18;
    for (int r = 0; r < rounds; ++r) {
        const auto t0 = Clock::now();
        fn();
        best = std::min(best, ms_since(t0));
    }
    return best;
}

// ---- 位打包 logic fixture ----------------------------------------------
// 布局（logicsnapshot.cpp:766）：unitsize = (channel_num+7)/8 字节/样本，
// sample s channel ch 的位 = src[s*unitsize + ch/8] 的 bit (ch%8)。
struct PackedFixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;
    const int ch_count;

    explicit PackedFixture(int n) : chs(n), nodes(n), ch_count(n)
    {
        for (int i = 0; i < n; ++i) {
            chs[i].index = i;
            chs[i].type = SR_CHANNEL_LOGIC;
            chs[i].enabled = TRUE;
            chs[i].name = nullptr;
            nodes[i].data = &chs[i];
            nodes[i].next = (i + 1 < n) ? &nodes[i + 1] : nullptr;
        }
    }

    // periods[ch] = 方波周期（样本数），占空比 50%。period<=1 → 恒低（稀疏）。
    // 通道按 periods[ch % periods.size()] 取周期，模拟真实"快慢混合"总线。
    std::vector<uint8_t> make_payload(const std::vector<uint32_t> &periods) const
    {
        const uint64_t unitsize = (uint64_t)(ch_count + 7) / 8;
        std::vector<uint8_t> data((size_t)(SAMPLES * unitsize), 0);
        for (uint64_t s = 0; s < SAMPLES; ++s) {
            uint8_t *row = data.data() + s * unitsize;
            for (int ch = 0; ch < ch_count; ++ch) {
                const uint32_t p = periods[(size_t)ch % periods.size()];
                if (p > 1 && (s % p) < (p / 2))
                    row[ch >> 3] |= (uint8_t)(1u << (ch & 7));
            }
        }
        return data;
    }
};

void feed(LogicSnapshot &snap, PackedFixture &fx,
          const std::vector<uint8_t> &payload)
{
    sr_datafeed_logic l{};
    l.length = payload.size();
    l.data = const_cast<uint8_t *>(payload.data());
    l.unitsize = (uint8_t)((fx.ch_count + 7) / 8);
    l.format = 0;
    // first_payload 内部已喂入一次（logicsnapshot.cpp:688），不要再 append。
    snap.first_payload(l, SAMPLES, &fx.nodes[0], true);
    snap.capture_ended();
    snap.set_samplerate(SAMPLERATE);
}

struct Pattern {
    const char *name;
    std::vector<uint32_t> periods;
};

const std::vector<Pattern> &patterns()
{
    static const std::vector<Pattern> p = {
        {"dense  (周期2, 全通道每样本翻转)", {2}},
        {"mixed  (周期 2/16/256/4096 循环)", {2, 16, 256, 4096}},
        {"sparse (周期 4096/8192/16384/32768)", {4096, 8192, 16384, 32768}},
    };
    return p;
}

// 缩放级别：samples_per_pixel。8738 ≈ 16M/1920 → 恰好铺满整个捕获。
const std::vector<double> &spp_levels()
{
    static const std::vector<double> v = {0.5, 1.0, 8.0, 64.0, 512.0,
                                          4096.0, 8738.0};
    return v;
}

// 与 rasterize_logic_channel 内部一致地推导可见窗口。
void window_for(double spp, uint64_t &start_index, uint64_t &end_index)
{
    const int64_t last = (int64_t)SAMPLES - 1;
    const double end = (double)(WIDTH + 1) * spp;
    end_index = (uint64_t)std::min<double>(std::floor(end), (double)last);
    start_index = 0;
}

double measure_edges(LogicSnapshot &snap, int ch, double spp, int rounds)
{
    std::vector<std::pair<bool, bool>> edges;
    std::vector<std::pair<uint16_t, bool>> togs;
    uint64_t s = 0, e = 0;
    window_for(spp, s, e);
    const uint16_t max_togs = (uint16_t)(WIDTH / 10);
    return best_of(rounds, [&]() {
        edges.clear();
        togs.clear();
        snap.get_display_edges(edges, togs, s, e, (uint16_t)WIDTH, max_togs,
                               0.0, spp, (uint16_t)ch);
        g_sink += edges.size() + togs.size();
    });
}

// 一个通道窗口内所有通道的边沿扫描合计（不含绘制）。
double measure_edges_all(LogicSnapshot &snap, int ch_count, double spp,
                         int rounds)
{
    std::vector<std::pair<bool, bool>> edges;
    std::vector<std::pair<uint16_t, bool>> togs;
    uint64_t s = 0, e = 0;
    window_for(spp, s, e);
    const uint16_t max_togs = (uint16_t)(WIDTH / 10);
    return best_of(rounds, [&]() {
        for (int ch = 0; ch < ch_count; ++ch) {
            edges.clear();
            togs.clear();
            snap.get_display_edges(edges, togs, s, e, (uint16_t)WIDTH,
                                   max_togs, 0.0, spp, (uint16_t)ch);
            g_sink += edges.size() + togs.size();
        }
    });
}

// 整幅位图清屏（SignalPixmapPass 重建分支的 fill(Qt::transparent)）。
double measure_fill(QImage &img, int rounds)
{
    return best_of(rounds, [&]() { img.fill(Qt::transparent); });
}

pv::view::PaintContext make_ctx(double scale)
{
    pv::view::PaintContext ctx;
    ctx.scale = scale;
    ctx.offset = 0;
    ctx.view_width = WIDTH;
    ctx.is_logic_mode = true;
    ctx.is_stopped_status = true;
    ctx.show_glitch_overlay = false;
    return ctx;
}

// 整帧重建：fill(整幅位图) + ch_count × rasterize_logic_channel，共用一个
// QPainter —— 与 SignalPixmapPass::render 的重建分支同形。
// offset: 像素偏移（pan 序列逐帧前移，模拟拖动）。
double measure_frame_rebuild(LogicSnapshot &snap, QImage &img, double spp,
                             int ch_count, int rounds, int64_t offset = 0,
                             int ch_height = CH_HEIGHT)
{
    const double scale = spp / SAMPLERATE;
    const pv::view::PaintContext ctx = make_ctx(scale);
    const uint64_t end_align = SAMPLES - 1;
    return best_of(rounds, [&]() {
        img.fill(Qt::transparent);
        QPainter p(&img);
        for (int ch = 0; ch < ch_count; ++ch) {
            const int y = (ch + 1) * ch_height;
            pv::view::rasterize_logic_channel(
                p, &snap, ch, 0, WIDTH, y, ch_height, QColor(255, 255, 255),
                scale, offset, end_align, ctx, nullptr);
        }
        p.end();
        g_sink += (uint64_t)img.sizeInBytes();
    });
}

// ---- drawLines vs drawRects / 分批对照用的几何 -----------------------------
// 用与 rasterize_logic_channel **完全相同**的循环从 get_display_edges 的输出
// 构建几何，同时准备三种视图：
//   hv_lines —— 与生产完全一致的交错数组（水平段、竖直段依次 push，最后
//               **一次** drawLines 画完），见 rasterize.cpp 的 wave_lines
//   h_lines  —— 仅水平段（电平线）
//   v_lines  —— 仅竖直段（跳变）
// 竖直段另可用 1px 宽填充矩形表示（dso/analog 的 min/max 分支正是这么做的）。
struct FrameGeometry {
    std::vector<QLine> hv_lines;   // 生产的交错数组
    std::vector<QLine> h_lines;    // 仅水平
    std::vector<QLine> v_lines;    // 仅竖直
};

// 线段 → 1px 矩形。ext 为末端扩展：0 = 半开 [a,b)，1 = 含端点 [a,b]。
inline QRect hline_to_rect(const QLine &l, int ext)
{
    const int x1 = std::min(l.x1(), l.x2());
    const int x2 = std::max(l.x1(), l.x2());
    return QRect(x1, l.y1(), std::max(1, x2 - x1 + ext), 1);
}

inline QRect vline_to_rect(const QLine &l, int ext)
{
    const int y1 = std::min(l.y1(), l.y2());
    const int y2 = std::max(l.y1(), l.y2());
    return QRect(l.x1(), y1, 1, std::max(1, y2 - y1 + ext));
}

FrameGeometry build_geometry(LogicSnapshot &snap, int ch, double spp,
                             int width, int y, int total_height)
{
    FrameGeometry g;
    const int high_offset = y - total_height;
    const int low_offset = y;
    const uint64_t ring = snap.get_ring_sample_count();
    if (ring == 0)
        return g;
    const uint64_t end_index = std::min<uint64_t>(
        (uint64_t)std::floor((double)(width + 1) * spp), ring - 1);
    const uint16_t max_togs = (uint16_t)(width / 10);

    std::vector<std::pair<bool, bool>> pulses;
    std::vector<std::pair<uint16_t, bool>> edges;
    const bool first =
        snap.get_display_edges(pulses, edges, 0, end_index, (uint16_t)width,
                               max_togs, 0.0, spp, (uint16_t)ch);

    int preX = 0;
    int preY = first ? high_offset : low_offset;
    int x = 0;

    // 与 rasterize.cpp 一致：先 push 水平段，再 push 竖直跳变（交错），
    // 最后收一条水平段；生产把它们放进同一个数组一次 drawLines。
    auto push_h = [&](int xa, int xb, int yy) {
        const QLine l(xa, yy, xb, yy);
        g.hv_lines.push_back(l);
        g.h_lines.push_back(l);
    };
    auto push_v = [&](int xx) {
        const QLine l(xx, high_offset, xx, low_offset);
        g.hv_lines.push_back(l);
        g.v_lines.push_back(l);
    };

    if (edges.size() < max_togs) {
        if (edges.size() < 2)
            return g;
        for (auto i = edges.begin() + 1; i != edges.end() - 1; i++) {
            x = i->first;
            push_h(preX, x, preY);
            push_v(x);
            preX = x;
            preY = i->second ? high_offset : low_offset;
        }
        x = edges.back().first;
        push_h(preX, x, preY);
    } else if (!pulses.empty()) {
        auto i = pulses.begin();
        while (i != pulses.end() - 1) {
            if (i->first) {
                push_h(preX, x, preY);
                push_v(x);
                preX = x;
                preY = i->second ? high_offset : low_offset;
            }
            x++;
            i++;
        }
        push_h(preX, x, preY);
    }
    return g;
}

// ---- 一帧的图元表示方式 -----------------------------------------------------
enum class Repr {
    LinesAll,        // 水平、竖直都用 QLine（生产现状）
    H_Lines_V_Rects, // 水平 QLine + 竖直 1px 填充矩形
    AllRects,        // 全部用矩形（水平 1px 高、竖直 1px 宽）
};

// 把一帧几何画进 img（先清空）。
//   ext     —— 线段→矩形转换的末端扩展（0 = 半开，1 = 含端点）
//   batched —— true：把全部通道的同类图元拼成一个大数组，每类只发 1 次调用；
//              false：逐通道发调用（= 生产的调用形状）。
void render_frame(QImage &img, const std::vector<FrameGeometry> &ges, Repr repr,
                  bool batched, int ext)
{
    img.fill(Qt::transparent);
    QPainter p(&img);
    const QColor c(255, 255, 255);
    const QPen pen(c);
    const QBrush brush(c);

    std::vector<QLine> h_lines, v_lines;
    std::vector<QRect> h_rects, v_rects;

    const bool h_as_lines = (repr == Repr::LinesAll);
    const bool v_as_lines = (repr == Repr::LinesAll);

    auto flush = [&]() {
        if (h_as_lines) {
            if (!h_lines.empty()) {
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                p.drawLines(h_lines.data(), (int)h_lines.size());
            }
        } else if (!h_rects.empty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(brush);
            p.drawRects(h_rects.data(), (int)h_rects.size());
        }
        if (v_as_lines) {
            if (!v_lines.empty()) {
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                p.drawLines(v_lines.data(), (int)v_lines.size());
            }
        } else if (!v_rects.empty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(brush);
            p.drawRects(v_rects.data(), (int)v_rects.size());
        }
        h_lines.clear();
        v_lines.clear();
        h_rects.clear();
        v_rects.clear();
    };

    for (const auto &g : ges) {
        if (h_as_lines)
            h_lines.insert(h_lines.end(), g.h_lines.begin(), g.h_lines.end());
        else
            for (const auto &l : g.h_lines)
                h_rects.push_back(hline_to_rect(l, ext));
        if (v_as_lines)
            v_lines.insert(v_lines.end(), g.v_lines.begin(), g.v_lines.end());
        else
            for (const auto &l : g.v_lines)
                v_rects.push_back(vline_to_rect(l, ext));
        if (!batched)
            flush();
    }
    if (batched)
        flush();
    p.end();
}

// 生产形状：每通道一次 drawLines(交错数组)。
void render_frame_production(QImage &img, const std::vector<FrameGeometry> &ges)
{
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setPen(QColor(255, 255, 255));
    p.setBrush(Qt::NoBrush);
    for (const auto &g : ges)
        p.drawLines(g.hv_lines.data(), (int)g.hv_lines.size());
    p.end();
}

// 返回不一致像素数，并把前几个坐标写进 first。
size_t first_pixel_mismatches(const QImage &a, const QImage &b, QString &first,
                              int max_report = 5)
{
    if (a.size() != b.size())
        return (size_t)-1;
    size_t n = 0;
    QStringList coords;
    for (int yy = 0; yy < a.height(); ++yy)
        for (int xx = 0; xx < a.width(); ++xx) {
            if (a.pixel(xx, yy) != b.pixel(xx, yy)) {
                if ((int)coords.size() < max_report)
                    coords << QString("(%1,%2)").arg(xx).arg(yy);
                ++n;
            }
        }
    first = coords.join(", ");
    return n;
}

} // anonymous namespace

class TestLogicRenderCost : public QObject
{
    Q_OBJECT

private slots:
    // 单通道 mipmap 边沿扫描成本随缩放/密度的变化。
    void edge_scan_scaling();
    // 16 通道整帧重建相对 60/30 FPS 帧预算的占比。
    void frame_rebuild_budget();
    // 把整帧重建拆成「清屏 / 边沿扫描 / 构建+绘制」三段，定位主导项。
    void frame_rebuild_decomposition();
    // 通道数扩展性：1/8/16/32 通道下的整帧成本。
    void channel_count_scaling();
    // zoom/pan 浏览（纯波形、无解码轨）：连续帧的持续吞吐与等效帧率。
    void zoom_pan_sustained_throughput();
    // 通道高度敏感性：竖直边线段长 = 通道高，直接影响栅格化填充量。
    void channel_height_sensitivity();
    // drawRects 与 drawLines 的像素等价性验证（合批不变性 + 矩形末端约定）。
    void draw_primitives_pixel_parity();
    // 调用形状/图元/合批的绘制成本矩阵。
    void draw_batching_comparison();
    // 装配成本对照：交错 QLine（旧形状）vs 水平 QLine + 竖直 QRect（新形状）。
    void build_cost_lines_vs_rects();
    // 变更门禁：真实 rasterize_logic_channel 的输出必须与旧线段形状逐像素一致。
    void production_parity();
};

void TestLogicRenderCost::edge_scan_scaling()
{
    PackedFixture fx(CHANNELS);
    qInfo("%s", "");
    qInfo("==== logic 每通道 get_display_edges 成本 "
          "(样本/通道=16M, 宽=%d, ch0, best-of-%d) ====", WIDTH, ROUNDS);
    qInfo("%-38s %10s %12s %12s", "pattern / spp", "edges", "togs", "ms/ch");

    for (const auto &pat : patterns()) {
        LogicSnapshot snap;
        std::vector<uint8_t> payload = fx.make_payload(pat.periods);
        feed(snap, fx, payload);
        QVERIFY2(snap.get_ring_sample_count() == SAMPLES,
                 qPrintable(QString("fixture precondition: ring=%1, expected %2")
                                .arg(snap.get_ring_sample_count()).arg(SAMPLES)));

        for (double spp : spp_levels()) {
            // 预热一轮（mipmap 块首次触碰有页错误成本）。
            measure_edges(snap, 0, spp, 1);
            std::vector<std::pair<bool, bool>> edges;
            std::vector<std::pair<uint16_t, bool>> togs;
            uint64_t s = 0, e = 0;
            window_for(spp, s, e);
            edges.clear();
            togs.clear();
            snap.get_display_edges(edges, togs, s, e, (uint16_t)WIDTH,
                                   (uint16_t)(WIDTH / 10), 0.0, spp, 0);
            const double ms = measure_edges(snap, 0, spp, ROUNDS);
            qInfo("%-38s %10zu %12zu %12.3f",
                  qPrintable(QString("%1 spp=%2")
                                 .arg(pat.name).arg(spp, 0, 'g', 6)),
                  edges.size(), togs.size(), ms);
            QVERIFY(ms > 0.0);
        }
        qInfo("%s", "");
    }

    // 决策断言：mipmap 生效时，可见样本数增长 ~16000x（spp 0.5 → 8738）时，
    // 每通道扫描耗时不得线性增长。放宽到 200x（远低于 16000x）作为回归门禁。
    LogicSnapshot snap;
    std::vector<uint8_t> payload = fx.make_payload(patterns()[1].periods);
    feed(snap, fx, payload);
    measure_edges(snap, 0, 1.0, 1);
    const double t_zoom_in = measure_edges(snap, 0, 1.0, ROUNDS);
    const double t_zoom_out = measure_edges(snap, 0, 8738.0, ROUNDS);
    const double ratio = t_zoom_in > 0.0 ? t_zoom_out / t_zoom_in : 0.0;
    qInfo("mipmap 缩放趋势: spp=1 %.3f ms -> spp=8738 %.3f ms  (x%.1f)",
          t_zoom_in, t_zoom_out, ratio);
    qInfo("  （可见样本量增长约 16000x；若耗时同步增长说明 mipmap 未生效）");
    QVERIFY2(ratio < 200.0,
             qPrintable(QString("mipmap 边沿扫描疑似退化为线性扫描: "
                                "spp=1 %1 ms -> spp=8738 %2 ms (x%3)")
                            .arg(t_zoom_in).arg(t_zoom_out).arg(ratio)));
}

void TestLogicRenderCost::frame_rebuild_budget()
{
    PackedFixture fx(CHANNELS);
    LogicSnapshot snap;
    std::vector<uint8_t> payload = fx.make_payload(patterns()[1].periods);
    feed(snap, fx, payload);

    QImage img(WIDTH, CHANNELS * CH_HEIGHT, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    const double budget60 = 1000.0 / 60.0;   // 16.67 ms
    const double budget30 = 1000.0 / 30.0;   // 33.33 ms

    qInfo("%s", "");
    qInfo("==== logic 整帧重建成本 (fill + %d 通道 rasterize, 宽=%d, "
          "best-of-%d) ====", CHANNELS, WIDTH, ROUNDS);
    qInfo("%-14s %12s %12s %12s", "samples/px", "ms/frame", "60fps占用",
          "30fps占用");

    double worst_ratio60 = 0.0;
    for (double spp : spp_levels()) {
        measure_frame_rebuild(snap, img, spp, CHANNELS, 1);   // 预热
        const double ms = measure_frame_rebuild(snap, img, spp, CHANNELS, ROUNDS);
        const double r60 = ms / budget60 * 100.0;
        const double r30 = ms / budget30 * 100.0;
        worst_ratio60 = std::max(worst_ratio60, r60);
        qInfo("%-14g %12.2f %11.0f%% %11.0f%%", spp, ms, r60, r30);
        QVERIFY(ms > 0.0);
    }
    qInfo("%s", "");
    qInfo("判读：若整帧重建耗时已接近/超过 100%% 帧预算，则 logic 模式低帧率"
          "来自渲染本身；");
    qInfo("      若远低于预算，瓶颈在重绘触发频率或解码轨实时绘制（单测覆盖"
          "不到）。");
    qInfo("最差帧预算占用（60fps）: %.0f%%", worst_ratio60);
    qInfo("%s", "");

    // 宽松门禁：只拦灾难性回归（整帧重建 > 2 个 30fps 帧预算）。
    QVERIFY2(worst_ratio60 < 200.0,
             qPrintable(QString("logic 整帧重建严重超出预算: 最差占用 60fps 的 "
                                "%1%%").arg(worst_ratio60)));
}

// 把整帧重建拆成三段，定位主导项：
//   fill      —— 整幅位图清屏
//   edges     —— 16 通道 mipmap 边沿扫描合计
//   构建+绘制 —— 余项 = wave_lines 构建 + setPen + drawLines
void TestLogicRenderCost::frame_rebuild_decomposition()
{
    PackedFixture fx(CHANNELS);
    LogicSnapshot snap;
    std::vector<uint8_t> payload = fx.make_payload(patterns()[1].periods);
    feed(snap, fx, payload);

    QImage img(WIDTH, CHANNELS * CH_HEIGHT, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    const double spps[] = {1.0, 8.0, 512.0, 8738.0};
    qInfo("%s", "");
    qInfo("==== logic 整帧重建分段 (16ch, 宽=%d, mixed, best-of-%d) ====",
          WIDTH, ROUNDS);
    qInfo("%-10s %10s %10s %12s %10s", "samples/px", "fill/ms", "edges/ms",
          "构建+绘制/ms", "frame/ms");
    for (double spp : spps) {
        measure_frame_rebuild(snap, img, spp, CHANNELS, 1);   // 预热
        const double fill = measure_fill(img, ROUNDS);
        const double edges = measure_edges_all(snap, CHANNELS, spp, ROUNDS);
        const double frame = measure_frame_rebuild(snap, img, spp, CHANNELS, ROUNDS);
        const double draw = std::max(0.0, frame - fill - edges);
        qInfo("%-10g %10.2f %10.2f %12.2f %10.2f", spp, fill, edges, draw, frame);
        QVERIFY(frame > 0.0);
    }
    qInfo("%s", "");
}

// 通道数扩展性：整帧成本是否随通道数线性增长（决定 8ch 与 32ch 场景的余量）。
void TestLogicRenderCost::channel_count_scaling()
{
    const int counts[] = {1, 8, 16, 32};
    qInfo("%s", "");
    qInfo("==== logic 整帧重建 vs 通道数 (宽=%d, mixed, best-of-%d) ====",
          WIDTH, ROUNDS);
    qInfo("%-10s %16s %16s", "channels", "ms/frame(spp=8)",
          "ms/frame(spp=8738)");
    for (int n : counts) {
        PackedFixture fx(n);
        LogicSnapshot snap;
        std::vector<uint8_t> payload = fx.make_payload(patterns()[1].periods);
        feed(snap, fx, payload);
        QVERIFY2(snap.get_ring_sample_count() == SAMPLES,
                 "fixture precondition: ring sample count");

        QImage img(WIDTH, n * CH_HEIGHT, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        measure_frame_rebuild(snap, img, 8.0, n, 1);   // 预热
        const double a = measure_frame_rebuild(snap, img, 8.0, n, ROUNDS);
        const double b = measure_frame_rebuild(snap, img, 8738.0, n, ROUNDS);
        qInfo("%-10d %16.2f %16.2f", n, a, b);
        QVERIFY(a > 0.0 && b > 0.0);
    }
    qInfo("%s", "");
}

// zoom/pan 浏览纯波形（无解码轨）的**持续**吞吐。
//
// 这里是单测能覆盖 zoom 浏览的关键等价性：缩放/平移都会改 scale 或 offset，
// 于是 SignalPixmapPass 的 view_params_changed 为真 → **每帧全量重建**，本用
// 例逐帧调 measure_frame_rebuild 与生产逐帧行为等价。
// 无解码轨时 DecodeTracePass::should_run 为 false，不在成本内（与场景一致）。
// 与上面 best-of-5 的区别：best-of-5 给的是单帧乐观值，浏览手感由**持续**帧时间
// 决定（含画布缓存抖动、分配器状态），所以这里逐帧计时后取 avg/max。
void TestLogicRenderCost::zoom_pan_sustained_throughput()
{
    PackedFixture fx(CHANNELS);
    LogicSnapshot snap;
    std::vector<uint8_t> payload = fx.make_payload(patterns()[1].periods);
    feed(snap, fx, payload);

    QImage img(WIDTH, CHANNELS * CH_HEIGHT, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    const int FRAMES = 30;
    for (int f = 0; f < 5; ++f)   // 预热
        measure_frame_rebuild(snap, img, 8.0, CHANNELS, 1, f);

    const double spps[] = {8.0, 512.0, 8738.0};
    qInfo("%s", "");
    qInfo("==== logic zoom/pan 持续吞吐 (纯波形, %dch, 宽=%d, %d 连续帧) ====",
          CHANNELS, WIDTH, FRAMES);
    qInfo("%-8s %-12s %12s %12s %10s", "mode", "spp", "avg ms/fr",
          "max ms/fr", "~fps");
    for (double spp : spps) {
        // pan：每帧 offset 前移 1px（等价于水平拖动一帧）
        {
            double total = 0.0, mx = 0.0;
            for (int f = 0; f < FRAMES; ++f) {
                const auto t0 = Clock::now();
                measure_frame_rebuild(snap, img, spp, CHANNELS, 1, f);
                const double ms = ms_since(t0);
                total += ms;
                mx = std::max(mx, ms);
            }
            const double avg = total / FRAMES;
            qInfo("%-8s %-12g %12.3f %12.3f %10.0f", "pan", spp, avg, mx,
                  1000.0 / avg);
        }
        // zoom-in：每帧 spp *= 0.9（连续放大，窗口始终在捕获范围内）
        {
            double s = spp, total = 0.0, mx = 0.0;
            for (int f = 0; f < FRAMES; ++f) {
                const auto t0 = Clock::now();
                measure_frame_rebuild(snap, img, s, CHANNELS, 1);
                const double ms = ms_since(t0);
                total += ms;
                mx = std::max(mx, ms);
                s *= 0.9;
            }
            const double avg = total / FRAMES;
            qInfo("%-8s %-12g %12.3f %12.3f %10.0f", "zoom", spp, avg, mx,
                  1000.0 / avg);
        }
    }
    qInfo("%s", "");
    qInfo("说明：本表只含波形栅格化（SignalPixmapPass 的重建分支）。生产每帧还有");
    qInfo("      group card / 分隔线 / paint_back / paint_fore / cursor·measure");
    qInfo("      overlay —— 那些与通道数、宽度相关、与样本数无关，需运行时");
    qInfo("      FRAME_VIEWPORT 才能合计。");
    qInfo("%s", "");
}

// 通道高度敏感性。dense 分支每像素发一条竖线 QLine(x, high, x, low)，段长 =
// 通道高度；高度越大，栅格化要填充的像素越多，单帧成本随之上升。上面的用例
// 固定用 CH_HEIGHT=20，本用例扫 20/40/80 覆盖真实行高范围，检验结论是否受
// 行高假设影响。
void TestLogicRenderCost::channel_height_sensitivity()
{
    PackedFixture fx(CHANNELS);
    LogicSnapshot snap;
    std::vector<uint8_t> payload = fx.make_payload(patterns()[1].periods);
    feed(snap, fx, payload);

    const int heights[] = {20, 40, 80};
    qInfo("%s", "");
    qInfo("==== logic 整帧重建 vs 通道高度 (16ch, 宽=%d, mixed, best-of-%d) ====",
          WIDTH, ROUNDS);
    qInfo("%-10s %16s %16s", "ch height", "ms/frame(spp=8)",
          "ms/frame(spp=8738)");
    for (int h : heights) {
        QImage img(WIDTH, CHANNELS * h, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        measure_frame_rebuild(snap, img, 8.0, CHANNELS, 1, 0, h);   // 预热
        const double a = measure_frame_rebuild(snap, img, 8.0, CHANNELS,
                                               ROUNDS, 0, h);
        const double b = measure_frame_rebuild(snap, img, 8738.0, CHANNELS,
                                               ROUNDS, 0, h);
        qInfo("%-10d %16.2f %16.2f", h, a, b);
        QVERIFY(a > 0.0 && b > 0.0);
    }
    qInfo("%s", "");
}

// drawRects 与 drawLines 的**像素等价性**验证。
//
// 参考基准 = 生产形状 `render_frame_production`（每通道一次 drawLines 交错数组）。
// 对每个候选表示逐像素比对，报告不一致像素数与前几处坐标。
// 硬性断言：合批本身不得改变像素；且「竖直改用 1px 矩形」在两种末端约定里
// 必须至少有一种做到逐像素一致——否则这 2x 不能落地。
void TestLogicRenderCost::draw_primitives_pixel_parity()
{
    PackedFixture fx(CHANNELS);
    LogicSnapshot snap;
    std::vector<uint8_t> payload = fx.make_payload(patterns()[1].periods);
    feed(snap, fx, payload);

    const int heights[] = {40, 80};
    const double spps[] = {8.0, 512.0, 8738.0};

    qInfo("%s", "");
    qInfo("==== drawRects / drawLines 像素对拍 (16ch, 宽=%d) ====", WIDTH);
    qInfo("%-6s %-8s %-22s %10s  %s", "height", "spp", "variant", "mismatch",
          "first");

    size_t worst_batch = 0;          // 合批不变性
    size_t best_rects = (size_t)-1;  // 矩形表示的最小不一致数
    QString best_rects_desc;

    for (int h : heights) {
        QImage ref(WIDTH, CHANNELS * h, QImage::Format_ARGB32_Premultiplied);
        QImage got(WIDTH, CHANNELS * h, QImage::Format_ARGB32_Premultiplied);

        for (double spp : spps) {
            std::vector<FrameGeometry> ges;
            ges.reserve(CHANNELS);
            for (int ch = 0; ch < CHANNELS; ++ch)
                ges.push_back(build_geometry(snap, ch, spp, WIDTH, (ch + 1) * h, h));

            render_frame_production(ref, ges);

            auto check = [&](const char *name, size_t &acc) {
                QString first;
                const size_t m = first_pixel_mismatches(ref, got, first);
                acc = std::max(acc, m);
                qInfo("%-6d %-8g %-22s %10zu  %s", h, spp, name, m,
                      qPrintable(first));
                return m;
            };

            // (0) 合批不变性：把所有通道的 h 合成 1 次、v 合成 1 次
            render_frame(got, ges, Repr::LinesAll, true, 0);
            check("lines batched(1+1)", worst_batch);

            // (1) 竖直改 1px 矩形，逐通道调用；扫两种末端约定
            for (int ext = 0; ext <= 1; ++ext) {
                render_frame(got, ges, Repr::H_Lines_V_Rects, false, ext);
                const size_t m = check(ext == 0 ? "h:lines v:rects ext=0"
                                                : "h:lines v:rects ext=1",
                                       best_rects);
                if (m < best_rects) {
                    best_rects = m;
                    best_rects_desc = QString("ext=%1 batched=0").arg(ext);
                }
            }

            // (2) 同表示 + 合批（只发 2 次调用）
            for (int ext = 0; ext <= 1; ++ext) {
                render_frame(got, ges, Repr::H_Lines_V_Rects, true, ext);
                const size_t m = check(ext == 0 ? "h:lines v:rects ext=0 1c"
                                                : "h:lines v:rects ext=1 1c",
                                       best_rects);
                if (m < best_rects) {
                    best_rects = m;
                    best_rects_desc = QString("ext=%1 batched=1").arg(ext);
                }
            }

            // (3) 全部用矩形 + 合批（整个帧只发 1 次 drawRects）
            for (int ext = 0; ext <= 1; ++ext) {
                render_frame(got, ges, Repr::AllRects, true, ext);
                const size_t m = check(ext == 0 ? "all rects ext=0 1c"
                                                : "all rects ext=1 1c",
                                       best_rects);
                if (m < best_rects) {
                    best_rects = m;
                    best_rects_desc = QString("allrects ext=%1").arg(ext);
                }
            }
        }
    }

    qInfo("%s", "");
    qInfo("合批不改像素的最大不一致数: %zu（必须为 0）", worst_batch);
    qInfo("矩形表示的最小不一致数: %s（%zu）",
          best_rects == (size_t)-1 ? "n/a" : qPrintable(best_rects_desc),
          best_rects);
    qInfo("%s", "");

    QVERIFY2(worst_batch == 0,
             qPrintable(QString("合批改变了像素: 最大不一致 %1").arg(worst_batch)));
    QVERIFY2(best_rects == 0,
             qPrintable(QString("没有任何矩形约定能做到逐像素一致（最小不一致 "
                                "%1）—— drawRects 不可采用").arg(best_rects)));
}

// 调用形状 / 图元 / 合批的绘制成本矩阵。
//
// 回答两个问题：
//   * 把所有通道的图元拼成大数组、每类只发 1 次调用，能否省下调用开销？
//   * 竖直段能否用 1 次 drawRects 画完（甚至整帧只用 1 次 drawRects）？
//
// 关键方法学：**所有数组装配都在计时外完成**，计时区里只有 QPainter 的 draw
// 调用。否则"合批"会因为把装配（分配 + 拷贝）也算进去而被人为放大。
void TestLogicRenderCost::draw_batching_comparison()
{
    PackedFixture fx(CHANNELS);
    LogicSnapshot snap;
    std::vector<uint8_t> payload = fx.make_payload(patterns()[1].periods);
    feed(snap, fx, payload);

    const int heights[] = {40, 80};
    const double spps[] = {8.0, 512.0, 8738.0};
    // 像素对拍的结论：矩形必须含端点（QLine 含端点、QRect 半开）。
    const int ext = 1;

    qInfo("%s", "");
    qInfo("==== 绘制调用形状对照 (16ch, 宽=%d, best-of-%d, 只计 draw 调用) ====",
          WIDTH, ROUNDS);
    qInfo("%-7s %-8s %10s %10s %11s %11s %11s",
          "height", "spp", "prod/ms", "h1+v1/ms", "rects/ch/ms",
          "rects 1c/ms", "allrect1c/ms");

    for (int h : heights) {
        QImage img(WIDTH, CHANNELS * h, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        const QPen pen(QColor(255, 255, 255));
        const QBrush brush(QColor(255, 255, 255));

        for (double spp : spps) {
            std::vector<FrameGeometry> ges;
            ges.reserve(CHANNELS);
            for (int ch = 0; ch < CHANNELS; ++ch)
                ges.push_back(build_geometry(snap, ch, spp, WIDTH, (ch + 1) * h, h));
            size_t n = 0;
            for (const auto &g : ges)
                n += g.hv_lines.size();
            if (n == 0)
                continue;

            // ---- 计时外：装配各变体所需数组 ----
            std::vector<QLine> hv_all;                        // 全通道交错
            std::vector<QLine> h_all, v_all;                  // 分流
            std::vector<std::vector<QLine>> h_ch(CHANNELS);   // 逐通道（生产形状）
            std::vector<std::vector<QRect>> v_rects_ch(CHANNELS);
            std::vector<QRect> h_rects_all, v_rects_all;
            for (int c = 0; c < CHANNELS; ++c) {
                const auto &g = ges[(size_t)c];
                hv_all.insert(hv_all.end(), g.hv_lines.begin(), g.hv_lines.end());
                h_all.insert(h_all.end(), g.h_lines.begin(), g.h_lines.end());
                v_all.insert(v_all.end(), g.v_lines.begin(), g.v_lines.end());
                h_ch[(size_t)c] = g.h_lines;
                v_rects_ch[(size_t)c].reserve(g.v_lines.size());
                for (const auto &l : g.v_lines)
                    v_rects_ch[(size_t)c].push_back(vline_to_rect(l, ext));
                for (const auto &l : g.h_lines)
                    h_rects_all.push_back(hline_to_rect(l, ext));
                for (const auto &l : g.v_lines)
                    v_rects_all.push_back(vline_to_rect(l, ext));
            }
            std::vector<QRect> all_rects = h_rects_all;
            all_rects.insert(all_rects.end(), v_rects_all.begin(), v_rects_all.end());

            // ---- 计时区：只有 draw 调用 ----
            const double t_prod = best_of(ROUNDS, [&]() {
                QPainter p(&img);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                for (const auto &g : ges)
                    p.drawLines(g.hv_lines.data(), (int)g.hv_lines.size());
                p.end();
            });
            const double t_hv1 = best_of(ROUNDS, [&]() {
                QPainter p(&img);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                p.drawLines(h_all.data(), (int)h_all.size());
                p.drawLines(v_all.data(), (int)v_all.size());
                p.end();
            });
            const double t_rch = best_of(ROUNDS, [&]() {
                QPainter p(&img);
                for (int c = 0; c < CHANNELS; ++c) {
                    const auto &hl = h_ch[(size_t)c];
                    p.setPen(pen);
                    p.setBrush(Qt::NoBrush);
                    p.drawLines(hl.data(), (int)hl.size());
                    const auto &vr = v_rects_ch[(size_t)c];
                    p.setPen(Qt::NoPen);
                    p.setBrush(brush);
                    p.drawRects(vr.data(), (int)vr.size());
                }
                p.end();
            });
            const double t_r1 = best_of(ROUNDS, [&]() {
                QPainter p(&img);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                p.drawLines(h_all.data(), (int)h_all.size());
                p.setPen(Qt::NoPen);
                p.setBrush(brush);
                p.drawRects(v_rects_all.data(), (int)v_rects_all.size());
                p.end();
            });
            const double t_a1 = best_of(ROUNDS, [&]() {
                QPainter p(&img);
                p.setPen(Qt::NoPen);
                p.setBrush(brush);
                p.drawRects(all_rects.data(), (int)all_rects.size());
                p.end();
            });

            g_sink += n;
            qInfo("%-7d %-8g %10.3f %10.3f %11.3f %11.3f %11.3f",
                  h, spp, t_prod, t_hv1, t_rch, t_r1, t_a1);
            QVERIFY(t_prod > 0.0 && t_a1 > 0.0);
        }
    }
    qInfo("%s", "");
    qInfo("prod     = 生产现状：每通道 1 次 drawLines(交错 h+v)，共 16 次调用");
    qInfo("h1+v1    = 所有水平段合 1 次 drawLines + 所有竖直段合 1 次，共 2 次");
    qInfo("rects/ch = 竖直改 1px 填充矩形（ext=1），逐通道调用（= 生产调用形状）");
    qInfo("rects 1c = 竖直改矩形且合批：1×drawLines(全 h) + 1×drawRects(全 v)");
    qInfo("allrect1c= 水平也改 1px 高矩形：整帧只发 1×drawRects");
    qInfo("%s", "");
}

// 装配成本对照。
//
// rects/ch 方案把生产里"一个交错 QLine 数组"改成"水平 QLine 数组 + 竖直 QRect
// 数组"两个数组。绘制侧已证明更快，但装配侧多了一次 vector 与一次 push_back，
// 必须确认不是把收益吃回去。两者都 reserve 到精确大小（与生产一致）。
void TestLogicRenderCost::build_cost_lines_vs_rects()
{
    PackedFixture fx(CHANNELS);
    LogicSnapshot snap;
    std::vector<uint8_t> payload = fx.make_payload(patterns()[1].periods);
    feed(snap, fx, payload);

    const int heights[] = {40, 80};
    const double spps[] = {8.0, 512.0, 8738.0};

    qInfo("%s", "");
    qInfo("==== 装配成本：交错 QLine vs (水平 QLine + 竖直 QRect) ====");
    qInfo("%-7s %-8s %12s %12s %12s %10s",
          "height", "spp", "old(1 vec)", "new(2 vecs)", "lines", "new/old");

    for (int h : heights) {
        QImage img(WIDTH, CHANNELS * h, QImage::Format_ARGB32_Premultiplied);

        for (double spp : spps) {
            std::vector<FrameGeometry> ges;
            ges.reserve(CHANNELS);
            for (int ch = 0; ch < CHANNELS; ++ch)
                ges.push_back(build_geometry(snap, ch, spp, WIDTH, (ch + 1) * h, h));
            size_t nh = 0, nv = 0;
            for (const auto &g : ges) {
                nh += g.h_lines.size();
                nv += g.v_lines.size();
            }
            if (nh == 0)
                continue;
            const int ext = 1;

            // 预热
            {
                std::vector<QLine> all;
                all.reserve(nh + nv);
                for (const auto &g : ges)
                    all.insert(all.end(), g.hv_lines.begin(), g.hv_lines.end());
                g_sink += all.size();
            }

            // 旧形状：逐个 push_back 进**一个**交错 QLine 数组
            // （与生产一致：每处跳变推入"水平段 + 竖直段"两条）
            const double t_old = best_of(ROUNDS, [&]() {
                std::vector<QLine> all;
                all.reserve(nh + nv);
                for (const auto &g : ges)
                    for (const auto &l : g.hv_lines)
                        all.push_back(l);
                g_sink += all.size();
            });

            // 新形状：逐个 push_back 进"水平 QLine + 竖直 QRect"**两个**数组，
            // 竖直元素在推入时构造 1px 矩形（每处跳变仍是两次 push_back）
            const double t_new = best_of(ROUNDS, [&]() {
                std::vector<QLine> hl;
                std::vector<QRect> vr;
                hl.reserve(nh);
                vr.reserve(nv);
                for (const auto &g : ges) {
                    for (const auto &l : g.h_lines)
                        hl.push_back(l);
                    for (const auto &l : g.v_lines)
                        vr.push_back(vline_to_rect(l, ext));
                }
                g_sink += hl.size() + vr.size();
            });

            qInfo("%-7d %-8g %12.3f %12.3f %12zu %10.2f",
                  h, spp, t_old, t_new, nh + nv,
                  t_old > 0.0 ? t_new / t_old : 0.0);
            QVERIFY(t_old > 0.0 && t_new > 0.0);
        }
    }
    qInfo("%s", "");
    qInfo("判据：new/old 接近 1（>1.5 才算把绘制收益吃回去）。");
    qInfo("%s", "");
}

// 变更门禁：真实 rasterize_logic_channel 的一帧输出，必须与"旧线段形状"的
// 独立参考逐像素一致。
//
// 参考不是复制生产代码，而是本文件按 get_display_edges 输出独立重建的
// 交错 QLine 一帧（render_frame_production）——生产改完后仍应与其完全一致。
void TestLogicRenderCost::production_parity()
{
    PackedFixture fx(CHANNELS);
    LogicSnapshot snap;
    std::vector<uint8_t> payload = fx.make_payload(patterns()[1].periods);
    feed(snap, fx, payload);

    const int heights[] = {20, 40, 80};
    const double spps[] = {0.5, 8.0, 512.0, 8738.0};

    qInfo("%s", "");
    qInfo("==== 生产函数像素门禁 rasterize_logic_channel vs 旧线段形状 ====");
    qInfo("%-7s %-9s %10s  %s", "height", "spp", "mismatch", "first");

    size_t worst = 0;
    for (int h : heights) {
        QImage ref(WIDTH, CHANNELS * h, QImage::Format_ARGB32_Premultiplied);
        QImage got(WIDTH, CHANNELS * h, QImage::Format_ARGB32_Premultiplied);

        for (double spp : spps) {
            std::vector<FrameGeometry> ges;
            ges.reserve(CHANNELS);
            for (int ch = 0; ch < CHANNELS; ++ch)
                ges.push_back(build_geometry(snap, ch, spp, WIDTH, (ch + 1) * h, h));

            render_frame_production(ref, ges);

            const double scale = spp / SAMPLERATE;
            const pv::view::PaintContext ctx = make_ctx(scale);
            got.fill(Qt::transparent);
            {
                QPainter p(&got);
                for (int ch = 0; ch < CHANNELS; ++ch)
                    pv::view::rasterize_logic_channel(
                        p, &snap, ch, 0, WIDTH, (ch + 1) * h, h,
                        QColor(255, 255, 255), scale, 0, SAMPLES - 1, ctx,
                        nullptr);
                p.end();
            }

            QString first;
            const size_t m = first_pixel_mismatches(ref, got, first);
            worst = std::max(worst, m);
            qInfo("%-7d %-9g %10zu  %s", h, spp, m, qPrintable(first));
        }
    }
    qInfo("%s", "");
    qInfo("最大不一致: %zu（必须为 0 —— 否则生产改动破坏了像素等价）", worst);
    qInfo("%s", "");
    QVERIFY2(worst == 0,
             qPrintable(QString("rasterize_logic_channel 与旧线段形状不一致: "
                                "%1 像素").arg(worst)));
}

QTEST_MAIN(TestLogicRenderCost)
#include "test_logic_render_cost.moc"