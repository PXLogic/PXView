/*
 * This file is part of the PXView project.
 *
 * Multi-tab (Session-Centric) integration tests.
 *
 * 这些用例锁定 Session-Centric 演进（阶段1-13）引入的契约，防止后续
 * 重构回归：
 *   1) SessionDocument per-tab 状态机演进（Idle/Collecting/Copying/Stopped）
 *   2) SignalModel stash/restore 语义（切 tab 零重建的基础）
 *   3) CaptureBuffers 执行缓冲（双缓冲 + 单缓冲判定）
 *   4) DeviceManager handle 身份与稳定性（原"关闭文件重开 handle 错位"
 *      的回归保护）
 *
 * 设计原则：只依赖轻量可构造的 Core/Data 类型，不构造 SigSession /
 * MainWindow，也不需要真实 libsigrok 设备（sdi 用非解引用的占位指针）。
 */

#include <QtTest>

#include <memory>
#include <vector>

#include "pv/core/capturebuffers.h"
#include "pv/core/documentregistry.h"
#include "pv/data/document/sessiondata.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/data/model/signalmodel.h"
#include "pv/session/devicemanager.h"

// 非解引用占位指针：DeviceManager 只做指针身份比较，不访问结构体成员。
static struct sr_dev_inst *placeholder_sdi(uintptr_t v)
{
    return reinterpret_cast<struct sr_dev_inst *>(v);
}

class TestMultiTabSession : public QObject
{
    Q_OBJECT

private slots:
    // ---- 1) per-tab 状态机 ----
    void perTabStateMachine()
    {
        pv::data::SessionDocument doc(nullptr);
        using S = pv::data::SessionDocument::SessionState;

        QVERIFY(doc.state() == S::Idle);
        QVERIFY(!doc.is_collecting());

        doc.set_state(S::Collecting);
        QVERIFY(doc.is_collecting());

        doc.set_state(S::Stopped);
        QVERIFY(!doc.is_collecting());
        QVERIFY(doc.state() == S::Stopped);

        // clear() 释放快照引用并复位为 Idle（阶段3a 契约）。
        doc.clear();
        QVERIFY(doc.state() == S::Idle);
    }

    // ---- 2) SignalModel 归文档所有（数据模型重构步骤2）----
    void signalModelDocOwnership()
    {
        pv::data::SessionDocument doc(nullptr);

        // 初始为空列表。
        QVERIFY(doc.signal_models().empty());
        QVERIFY(doc.signal_models_snapshot().empty());

        // 文档拥有列表：写入后 live 引用与 snapshot 一致（模型对象随文档保活，
        // 取代旧 stash/take 搬运语义）。
        doc.signal_models().push_back(nullptr);
        doc.signal_models().push_back(nullptr);
        QCOMPARE(doc.signal_models().size(), (size_t)2);
        QCOMPARE(doc.signal_models_snapshot().size(), (size_t)2);

        // DataSource override 仍必须保持空 stub（document_snapshot_source
        // 只裁决快照，不裁决模型——view_signal_sync 的裁决警示）。
        QVERIFY(doc.get_signal_models().empty());

        // clear() 只清数据，不触碰模型列表（采集启动会清 owner 文档数据，
        // 模型必须存活）。
        doc.clear();
        QCOMPARE(doc.signal_models().size(), (size_t)2);
    }

    // ---- 3) CaptureBuffers 执行缓冲（阶段6）----
    void captureBuffers()
    {
        pv::core::CaptureBuffers buffers;

        // 初始双缓冲：view 与 capture 指向同一缓冲（单缓冲语义）。
        QVERIFY(buffers.is_single_buffer());
        QVERIFY(buffers.view_data() != nullptr);
        QCOMPARE(buffers.view_data(), buffers.capture_data());
        QCOMPARE(buffers.data_list().size(), (size_t)2);

        // repeat 帧交换：capture 切到另一个缓冲 → 非单缓冲。
        auto *other = buffers.data_list()[1].get();
        buffers.set_capture_data(other);
        QVERIFY(!buffers.is_single_buffer());
        QCOMPARE(buffers.capture_data(), other);
        QVERIFY(buffers.view_data() != other);

        // 交换回 view（对应 RevEndPacket 的 set_view_data(capture_data)）。
        buffers.set_view_data(other);
        QVERIFY(buffers.is_single_buffer());
    }

    // ---- 4a) DeviceManager handle 身份往返 ----
    void deviceManagerHandleIdentity()
    {
        pv::DeviceManager mgr;

        struct sr_dev_inst *sdi = placeholder_sdi(0x1000);
        auto h = mgr.register_file_device(sdi);
        QVERIFY(h != NULL_HANDLE);

        // sdi → handle → sdi 往返一致（get_device_list 依赖此一致性）。
        QCOMPARE(mgr.handle_of_sdi(sdi), h);
        QCOMPARE(mgr.find_sdi_by_handle(h), sdi);

        // 未知 sdi 返回 NULL_HANDLE（不得臆造 handle）。
        QCOMPARE(mgr.handle_of_sdi(placeholder_sdi(0x9999)), NULL_HANDLE);
        QCOMPARE(mgr.handle_of_sdi(nullptr), NULL_HANDLE);
    }

    // ---- 4b) handle 稳定性：关闭文件后重开不得复用 handle ----
    // 回归保护：原实现自创 "index+1" 与真实 handle 错位，导致关闭文件后
    // 重开时设备列表 actived_index 越界（未选中 / 可能选错设备）。
    void deviceManagerHandleStableAcrossClose()
    {
        pv::DeviceManager mgr;

        struct sr_dev_inst *a = placeholder_sdi(0x2000);
        auto ha = mgr.register_file_device(a);

        // 关闭（不释放 sdi —— 测试用占位指针不能交给 sr_dev_inst_free）。
        QVERIFY(mgr.unregister_file_device(ha, /*free_sdi=*/false));
        QCOMPARE(mgr.handle_of_sdi(a), NULL_HANDLE);

        // 重开新文件：handle 单调递增、不复用（旧 handle 指向已注销设备）。
        struct sr_dev_inst *b = placeholder_sdi(0x3000);
        auto hb = mgr.register_file_device(b);
        QVERIFY(hb != ha);
        QCOMPARE(mgr.handle_of_sdi(b), hb);
        QVERIFY(mgr.find_sdi_by_handle(ha) != a); // 旧 handle 已失效
    }
};

QTEST_MAIN(TestMultiTabSession)

#include "test_multi_tab_session.moc"
