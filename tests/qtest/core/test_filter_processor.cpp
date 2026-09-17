/*
 * test_filter_processor.cpp — QTest unit tests for FilterProcessor (Core)
 *
 * 缺口审计③：这批最容易写错的 Core 逻辑（清除排队、最新写者优先序号、
 * 同步等待、状态对账）此前只有手工 MCP pytest 套件覆盖（需 app 在 10110
 * 端口，已移出 CTest），CI 零覆盖。本测试用与 test_capture_manager /
 * test_decode_task_manager 相同的离线外壳（FakeDispatcher + 全接口桩 +
 * 真实 LogicSnapshot），不依赖 GUI、不依赖 app 进程。
 *
 * 确定性说明（刻意避免 sleep 类时序断言）：
 *   - set_glitch_filter() 在提交线程池**之前**同步置位 _glitch_filter_running，
 *     因此"set 返回后立刻提交"的两个请求必然落在运行窗口内（滤波趟自身
 *     至少要跑几十 ms 的扫描 + mipmap 重建，测试线程提交只需 µs），
 *     "已排队"返回值不依赖任何时序假设。
 *   - 完成检测用单调不变式：编辑日志 has_filter_edits() 在趟内提交后为真、
 *     清除消费后为假；_glitch_filter_active 只在趟末置真、清除后置假。
 *     不用"active==false"作为完成信号——它在趟运行中同样为假，不单调。
 */

#include <QtTest>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <QDateTime>
#include <QString>
#include <vector>

#include <libsigrok/libsigrok.h>

#include "pv/core/filterprocessor.h"
#include "pv/core/eventbus.h"
#include "pv/interface/events.h"
#include "pv/data/document/sessiondata.h"
#include "pv/data/model/signalmodel.h"
#include "pv/base/pxvdef.h"        // GlitchFilterMode 完整定义

// ---- 链接桩（同 test_decode_task_manager：pxview-config 的 QColor 主题助手
// 经 __imp__ 间接引用，--as-needed 下 Qt6Gui 导入库在其之前被丢弃；离线路径
// 永不解引用，空槽位满足链接器即可）----
void *__imp__ZN6QColor10fromStringE14QAnyStringView = nullptr;
void *__imp__ZNK6QColor10lightnessFEv = nullptr;

using pv::core::EventBus;
using pv::core::FilterProcessor;
using pv::core::IAsyncDispatcher;

namespace {

// ---- 可注入的假派发器：异步事件排队不投递，测试显式 drain ----
class FakeDispatcher : public IAsyncDispatcher {
public:
    void post(std::function<void()> fn) override {
        std::lock_guard<std::mutex> lk(_m);
        ++posted;
        _pending.push_back(std::move(fn));
    }
    bool on_target_thread() const override { return true; }
    // 投递所有排队事件（在构造 EventBus 的同一线程上调用 → broadcast()
    // 走同步路径 → 订阅回调立即执行）
    void drain() {
        std::vector<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lk(_m);
            run.swap(_pending);
        }
        for (auto &f : run)
            f();
    }
    unsigned posted = 0;
private:
    std::mutex _m;
    std::vector<std::function<void()>> _pending;
};

// ---- 全接口桩：只有 view_data()/计数器有真实行为 ----
class StubSession : public pv::ISessionState {
public:
    // --- 测试计数器（worker 线程调用 → atomic）---
    std::atomic<int> data_updated_count{0};
    std::atomic<int> restart_count{0};
    std::atomic<int> pause_count{0};
    std::atomic<int> error_count{0};
    std::atomic<int> session_error_count{0};

    // --- ISessionCoordination ---
    void clear_all_decode_task2() override { ++pause_count; }
    void restart_decode_tasks() override { ++restart_count; }
    void add_decode_task(std::shared_ptr<pv::data::DecoderStack>) override {}
    void attach_data_to_signal(pv::SessionData *) override {}
    void sync_trigger_to_libsigrok(bool) override {}
    void clear_glitch_filter_state_for_capture() override {}
    uint16_t get_ch_num(int) override { return 0; }
    uint64_t cur_samplelimits() override { return 0; }
    uint64_t cur_snap_samplerate() override { return 0; }
    void set_cur_snap_samplerate(uint64_t) override {}
    void set_cur_samplelimits(uint64_t) override {}
    void data_updated() override { ++data_updated_count; }
    void signals_changed() override {}
    void update_capture() override {}
    void receive_header() override {}
    void receive_trigger(uint64_t) override {}
    void frame_began() override {}
    void arm_frame_end_pending() override {}
    bool consume_frame_end_pending() override { return false; }
    void session_error() override { ++session_error_count; }
    void set_trigger_flag(bool) override {}
    void set_trigger_ch(int) override {}
    void set_hw_replied(bool) override {}
    void set_receive_data_len(uint64_t) override {}
    void set_capture_data(pv::SessionData *) override {}
    void set_session_time(QDateTime) override {}
    void set_is_working(bool) override {}
    void set_is_triged(bool) override {}
    void set_trig_time(QDateTime) override {}
    void set_error(int) override { ++error_count; }
    bool bClose() const override { return false; }

    // --- ISessionState: manager back-pointers（本测试不触达）---
    pv::core::DataFeedParser *data_feed_parser() override { return nullptr; }
    pv::core::DocumentRegistry *document_registry() override { return nullptr; }
    pv::core::FilterProcessor *filter_processor() override { return nullptr; }

    // --- Mutexes ---
    std::mutex &data_mutex() override { return _data_mutex; }
    std::mutex &sampling_mutex() override { return _sampling_mutex; }

    // --- Business objects ---
    std::vector<std::shared_ptr<pv::data::SignalModel>> &signal_models() override {
        return _signal_models;
    }
    std::vector<std::shared_ptr<pv::data::SignalModel>> signal_models_snapshot() override {
        return _signal_models;
    }
    std::vector<std::shared_ptr<pv::data::SpectrumStack>> &spectrum_stacks() override {
        return _spectrum_stacks;
    }
    pv::data::LissajousModel *lissajous_model() const override { return nullptr; }
    void set_lissajous_model(std::unique_ptr<pv::data::LissajousModel>) override {}
    const std::shared_ptr<pv::data::MathStack> &math_stack() const override {
        return _math_stack;
    }
    void set_math_stack(std::shared_ptr<pv::data::MathStack> m) override {
        _math_stack = std::move(m);
    }

    // --- Time ---
    QDateTime session_time() const override { return _session_time; }
    QDateTime trig_time() const override { return _trig_time; }

    // --- Bool state ---
    bool is_triged() const override { return false; }
    bool trigger_flag() const override { return false; }
    bool hw_replied() const override { return false; }
    void set_bClose(bool) override {}
    bool is_saving() const override { return false; }
    void set_saving(bool) override {}

    // --- Numeric state ---
    int trigger_ch() const override { return 0; }
    uint64_t error_pattern() const override { return 0; }
    void set_error_pattern(uint64_t) override {}
    uint64_t save_start() const override { return 0; }
    void set_save_start(uint64_t) override {}
    uint64_t save_end() const override { return 0; }
    void set_save_end(uint64_t) override {}
    int map_zoom() const override { return 0; }
    void set_map_zoom(int) override {}

    // --- Atomic state ---
    bool is_working() const override { return false; }
    int device_status() const override { return 0; }
    void set_device_status(int) override {}

    // --- Device ---
    DeviceAgent &device_agent() override { return _agent; }

    // --- Data buffers ---
    pv::SessionData *view_data() override { return &_view_data; }
    void set_view_data(pv::SessionData *) override {}
    pv::SessionData *capture_data() override { return &_view_data; }
    std::vector<std::unique_ptr<pv::SessionData>> &data_list() override {
        return _data_list;
    }
    bool is_single_buffer() const override { return false; }

    // --- Trigger config ---
    const pv::data::TriggerConfig &trigger_config() const override {
        return _trigger_config;
    }
    void set_trigger_config(const pv::data::TriggerConfig &c) override {
        _trigger_config = c;
    }

    // --- Cursor registry ---
    pv::core::CursorRegistry &cursor_registry() override { return _cursors; }
    const pv::core::CursorRegistry &cursor_registry() const override {
        return _cursors;
    }

    // --- Decode-stack helpers ---
    std::vector<std::shared_ptr<pv::data::DecoderStack>> &
    decode_traces(pv::data::SessionDocument *) override { return _decode_traces; }
    std::vector<std::shared_ptr<pv::data::DecoderStack>> &
    get_decoder_stacks(pv::data::SessionDocument *) override { return _decode_traces; }
    std::shared_ptr<pv::data::DecoderStack>
    get_decoder_trace(int, pv::data::SessionDocument *) override { return nullptr; }
    int get_trace_index_by_key_handel(void *, pv::data::SessionDocument *) override {
        return -1;
    }

    // --- Misc ---
    void cur_snap_samplerate_changed() override {}
    void frame_ended() override {}
    void repeat_hold(int) override {}
    void show_wait_trigger() override {}
    void delay_prop_msg(QString) override {}
    uint64_t next_decoder_handle_id() override { return 1; }
    void notify_decode_complete() override {}
    void reset_capture_complete() override {}

    // --- 持有成员 ---
    pv::SessionData _view_data;
    pv::data::TriggerConfig _trigger_config;
    pv::core::CursorRegistry _cursors;
    DeviceAgent _agent;
    std::mutex _data_mutex, _sampling_mutex;
    std::vector<std::shared_ptr<pv::data::SignalModel>> _signal_models;
    std::vector<std::shared_ptr<pv::data::SpectrumStack>> _spectrum_stacks;
    std::shared_ptr<pv::data::MathStack> _math_stack;
    QDateTime _session_time, _trig_time;
    std::vector<std::unique_ptr<pv::SessionData>> _data_list;
    std::vector<std::shared_ptr<pv::data::DecoderStack>> _decode_traces;
};

// ---- 数据馈送：ch0 每 20 采样一个 1 采样窄高脉冲，ch1 常量低 ----
// 与 test_logic_snapshot_raw 的 Fixture 同构（LA_SPLIT_DATA interleaved）。
void feed_logic(pv::SessionData &vd, size_t n)
{
    std::vector<sr_channel> chs(2);
    std::vector<GSList> nodes(2);
    for (int i = 0; i < 2; ++i) {
        chs[(size_t)i].index = i;
        chs[(size_t)i].type = SR_CHANNEL_LOGIC;
        chs[(size_t)i].enabled = TRUE;
        chs[(size_t)i].name = nullptr;
        nodes[(size_t)i].data = &chs[(size_t)i];
        nodes[(size_t)i].next = (i + 1 < 2) ? &nodes[(size_t)i + 1] : nullptr;
    }
    std::vector<uint8_t> data(n);
    for (size_t s = 0; s < n; ++s)
        data[s] = ((s % 20) == 0) ? 0x01 : 0x00;

    sr_datafeed_logic l{};
    l.length = (uint64_t)data.size();
    l.unitsize = 1;
    l.format = 0;   // LA_SPLIT_DATA (interleaved)
    l.data = data.data();

    auto *logic = vd.get_logic();
    logic->first_payload(l, (uint64_t)n, &nodes[0], true);
    logic->append_payload(l);
    logic->capture_ended();
}

// apply_signal_invert 按 SignalModel 列表决定要翻转哪些通道（列表为空时
// 一个通道都不翻），因此数据级断言必须先登记一个 LOGIC 模型。
void add_logic_channel(StubSession &st, int index)
{
    auto m = std::make_shared<pv::data::SignalModel>();
    m->set_index(index);
    m->set_type(SR_CHANNEL_LOGIC);
    m->set_enabled(true, false);
    st._signal_models.push_back(m);
}

struct EventCounters {
    std::atomic<int> started{0};
    std::atomic<int> completed{0};
    std::atomic<int> cleared{0};
};

std::unique_ptr<EventBus> make_bus(FakeDispatcher **disp_out)
{
    auto disp = std::make_unique<FakeDispatcher>();
    *disp_out = disp.get();
    return std::make_unique<EventBus>(std::move(disp));
}

} // anonymous namespace

class TestFilterProcessor : public QObject
{
    Q_OBJECT

private slots:
    // 运行中提交清除 → 排队（不丢弃）→ 趟结束后自动执行 → 数据还原 + Cleared
    void test_clear_queued_while_pass_running_executes();
    // 运行中：先清除后应用 → 最新指令优先，清除被序号判定跳过
    void test_queued_clear_superseded_by_newer_apply();
    // 无活动滤波时请求/同步清除均为空操作（零广播、零状态翻转）
    void test_clear_without_active_is_noop();
    // 趟完成后同步清除：等待真实还原后才返回
    void test_sync_clear_waits_for_restore();
    // C1：滤波趟运行中"先清除后取反"——两个意图都必须兑现（按特性分计数器）
    void test_clear_queued_then_invert_honours_both_intents();

private:
    // 共用装配：订阅事件计数。返回值生命周期须覆盖整个用例。
    struct Harness {
        std::unique_ptr<EventBus> bus;
        FakeDispatcher *disp = nullptr;
        StubSession state;
        std::unique_ptr<FilterProcessor> processor;
        EventCounters events;
        std::vector<pv::core::Subscription> subs;
    };
    std::unique_ptr<Harness> make_harness(size_t samples);
};

std::unique_ptr<TestFilterProcessor::Harness>
TestFilterProcessor::make_harness(size_t samples)
{
    auto h = std::make_unique<Harness>();
    h->bus = make_bus(&h->disp);
    feed_logic(h->state._view_data, samples);
    h->processor = std::make_unique<FilterProcessor>(h->bus.get(), &h->state,
                                                     &h->state);
    auto *bus = h->bus.get();
    auto *ev = &h->events;
    h->subs.push_back(bus->subscribe<pv::interface::GlitchFilterStarted>(
        [ev](const pv::interface::GlitchFilterStarted &) { ++ev->started; }));
    h->subs.push_back(bus->subscribe<pv::interface::GlitchFilterCompleted>(
        [ev](const pv::interface::GlitchFilterCompleted &) { ++ev->completed; }));
    h->subs.push_back(bus->subscribe<pv::interface::GlitchFilterCleared>(
        [ev](const pv::interface::GlitchFilterCleared &) { ++ev->cleared; }));
    return h;
}

void TestFilterProcessor::test_clear_queued_while_pass_running_executes()
{
    auto h = make_harness(4000000);   // ~20 万毛刺：趟至少几十 ms，提交窗口充裕

    std::map<int, uint32_t> th;
    th[0] = 2;
    std::map<int, GlitchFilterMode> modes;
    modes[0] = GlitchFilterMode::Both;
    h->processor->set_glitch_filter(th, modes);

    // set_glitch_filter 同步置位 running 才返回 → 此刻提交必然落在运行窗口
    const bool queued = h->processor->request_clear_glitch_filter();
    QVERIFY2(queued, "a clear submitted while a pass is running must be "
                     "QUEUED, not silently dropped (gap audit ③)");

    // 完成判据（确定性，不依赖瞬态窗口）：GlitchFilterCleared 只在清除任务
    // 成功尾部（revert + 状态翻转之后）广播；异步事件由 FakeDispatcher 排队，
    // drain 才投递。数据判别一律用采样 20 —— 采样 0 是状态机初始基准、
    // 永远不会被抹平（见 test_logic_snapshot_raw 的同名注释），不作判据。
    for (int i = 0; i < 3000 && h->events.cleared.load() < 1; ++i) {
        h->disp->drain();
        QTest::qWait(10);
    }
    h->disp->drain();
    QCOMPARE(h->events.cleared.load(), 1);

    // 清除成功的后验状态（与 Cleared 广播在同一临界区内先行完成）
    QVERIFY2(!h->processor->is_glitch_filter_active(),
             "queued clear must leave the filter inactive");
    QVERIFY(h->state._view_data._glitch_filter_thresholds.empty());
    QVERIFY2(h->state._view_data.get_logic()->get_sample(20, 0),
             "queued clear must restore capture-original samples "
             "(sample 20 was flattened by the pass)");
    QVERIFY(!h->state._view_data.get_logic()->has_filter_edits());
}

void TestFilterProcessor::test_queued_clear_superseded_by_newer_apply()
{
    auto h = make_harness(4000000);

    std::map<int, uint32_t> thA, thB;
    thA[0] = 2;
    thB[0] = 3;

    std::map<int, GlitchFilterMode> mdA;
    mdA[0] = GlitchFilterMode::Both;
    h->processor->set_glitch_filter(thA, mdA);
    const bool queued = h->processor->request_clear_glitch_filter();
    QVERIFY2(queued, "clear while running must be queued");
    // 更新的写意图：排队一个新 apply（序号递增）→ 先前的排队清除必须让位
    std::map<int, GlitchFilterMode> mdB;
    mdB[0] = GlitchFilterMode::Both;
    h->processor->set_glitch_filter(thB, mdB);

    // 趟末的 pending 循环会以 thB 重建 → active 置真（单调：跳过的清除不会翻转它）
    QTRY_VERIFY_WITH_TIMEOUT(h->processor->is_glitch_filter_active(), 30000);
    // 给排队的清除任务机会拿到 _edit_mutex（拿到即因序号不符而跳过）
    QTest::qWait(200);
    h->disp->drain();

    QCOMPARE(h->events.cleared.load(), 0);
    QVERIFY2(h->processor->is_glitch_filter_active(),
             "the newer apply intent must win over the older queued clear");
    {
        std::lock_guard<std::mutex> lk(h->state._view_data._filter_state_mutex);
        QCOMPARE(h->state._view_data._glitch_filter_thresholds.at(0),
                 (uint32_t)3);
    }
    // 数据仍是滤波态：采样 20 的窄脉冲没有还原（采样 0 是初始基准，不作判据）
    QVERIFY2(!h->state._view_data.get_logic()->get_sample(20, 0),
             "the superseded clear must NOT have restored the samples");
}

void TestFilterProcessor::test_clear_without_active_is_noop()
{
    auto h = make_harness(200000);

    QVERIFY2(!h->processor->request_clear_glitch_filter(),
             "nothing active/running → the request is dropped, not queued");
    h->processor->clear_glitch_filter();   // 同步版：立即返回，无任务提交
    h->disp->drain();
    QCOMPARE(h->events.cleared.load(), 0);
    QVERIFY(!h->processor->is_glitch_filter_active());
    QCOMPARE(h->state._view_data._glitch_filter_thresholds.size(),
             (size_t)0);
}

void TestFilterProcessor::test_sync_clear_waits_for_restore()
{
    auto h = make_harness(2000000);

    std::map<int, uint32_t> th;
    th[0] = 2;
    std::map<int, GlitchFilterMode> modes;
    modes[0] = GlitchFilterMode::Both;
    h->processor->set_glitch_filter(th, modes);

    // 趟完成（active 置真且单调——此刻没有清除会翻转它）
    QTRY_VERIFY_WITH_TIMEOUT(h->processor->is_glitch_filter_active(), 30000);
    QVERIFY(h->state._view_data.get_logic()->has_filter_edits());
    QVERIFY2(!h->state._view_data.get_logic()->get_sample(20, 0),
             "precondition: the pass actually flattened the pulses");

    h->processor->clear_glitch_filter();   // 同步契约：返回即已还原

    QVERIFY2(!h->processor->is_glitch_filter_active(),
             "sync clear must leave the filter inactive");
    QVERIFY2(h->state._view_data.get_logic()->get_sample(20, 0),
             "sync clear returns only after the samples are restored");
    QVERIFY(!h->state._view_data.get_logic()->has_filter_edits());

    h->disp->drain();
    QCOMPARE(h->events.cleared.load(), 1);
}

void TestFilterProcessor::test_clear_queued_then_invert_honours_both_intents()
{
    // C1 回归：意图序号必须按特性分开。共用一把计数器时，紧跟"排队清除"之后
    // 提交的取反会把序号顶掉 ⇒ 排队的清除被判过期跳过；而取反任务发现
    // _glitch_filter_active 仍为真，会按 flag **重新应用滤波** ——
    // 于是用户已被 toast 告知"完成后将自动清除"的那次清除被静默丢弃。
    //
    // 两种任务执行顺序（清除先 / 取反先）的最终状态都必须是
    // "已取反 + 未滤波"，因此本用例对调度顺序不敏感。
    auto h = make_harness(4000000);
    add_logic_channel(h->state, 0);

    std::map<int, uint32_t> th;
    th[0] = 2;
    std::map<int, GlitchFilterMode> md;
    md[0] = GlitchFilterMode::Both;
    h->processor->set_glitch_filter(th, md);

    // 滤波趟运行中提交清除 → 排队（不是立即执行）
    QVERIFY2(h->processor->request_clear_glitch_filter(),
             "a clear submitted while the pass runs must be queued");

    // 紧接着提交一个**无关特性**的取反意图
    std::vector<bool> chans(1, true);
    h->processor->set_signal_invert(chans);

    // 两个意图都落地：GlitchFilterCleared 只在清除成功尾部广播；
    // 取反的 active 由取反任务末尾置真且此后无人翻转（单调）。
    for (int i = 0; i < 3000 && (h->events.cleared.load() < 1 ||
                                 !h->processor->is_signal_invert_active());
         ++i) {
        h->disp->drain();
        QTest::qWait(10);
    }
    h->disp->drain();

    QCOMPARE(h->events.cleared.load(), 1);
    QVERIFY2(h->processor->is_signal_invert_active(),
             "the invert intent must be applied");
    QVERIFY2(!h->processor->is_glitch_filter_active(),
             "the queued clear must NOT be cancelled by an unrelated invert "
             "submission (per-feature intent counters, gap-audit C1)");
    // 显式清除是**唯一**会丢掉用户配置的路径（用户明确要求移除滤波，配置
    // 随之作废 ⇒ 下次采集的 auto-apply 条件 !thresholds.empty() 不成立，
    // 不会背着用户把滤波加回来）；OOM 回滚与采集起点都保留配置。这里把该
    // 区分钉住，防止日后把"清除"改成保留配置而让用户被意外自动滤波。
    {
        std::lock_guard<std::mutex> flk(h->state._view_data._filter_state_mutex);
        QVERIFY(h->state._view_data._glitch_filter_thresholds.empty());
    }

    // 数据级判别 —— 采样 20 是采集里的 1 采样窄高脉冲：
    //   清除生效 ⇒ 数据 = 取反(采集原始) ⇒ 20 处为低
    //   清除被丢弃 ⇒ 数据 = 滤波(取反(采集原始)) ⇒ 20 处的低凹会被抹平为高
    auto *logic = h->state._view_data.get_logic();
    QVERIFY2(!logic->get_sample(20, 0),
             "sample 20 must be the INVERTED raw pulse (low); a re-applied "
             "filter would have flattened that one-sample dip back to high");
    QVERIFY(logic->get_sample(21, 0));   // 采集低 → 取反后高
    QVERIFY(!logic->get_sample(0, 0));   // 采集高 → 取反后低
}

QTEST_GUILESS_MAIN(TestFilterProcessor)
#include "test_filter_processor.moc"
