/*
 * test_rowdata_batch.cpp — QTest: 方案 E 批量注解落库与逐注解落库的等价性。
 *
 * 背景: PXView 现在把 libsigrokdecode 的 SRD_OUTPUT_ANN 注解以 srd_ann_batch
 * 批量交付给 DecoderStack::annotation_callback_batch → RowData::emplace_annotations
 * （每批一次锁）。逐注解路径 RowData::emplace_annotation 仍保留作为回退（引擎在
 * 未注册批回调时使用）。本测试用同一组随机注解分别走两条路径，断言两条路径产出的
 * _annotations 序列完全一致：start_sample / end_sample / format / type / 文本向量。
 *
 * 注解集合刻意覆盖:
 *   - 随机 start/end（含 end==start 瞬时注解）
 *   - 随机 ann_class / ann_type
 *   - 多段不同文本 + 部分相同文本（触发 AnnotationResTable 文本 intern）
 *   - '@' 数字前缀形式文本 + str_number_hex（覆盖数值注解路径）
 *   - '\n' 文本（Annotation 构造时跳过，不进入 intern key）
 *   - 空文本 / 空 str_number_hex（边界）
 *
 * 依赖: rowdata.cpp / annotation.cpp / annotationrestable.cpp / decoderstatus.cpp /
 *       annotation_heap.cpp 直接编入目标; xlog 打桩（同 test_signal_model.cpp）。
 */

#include <QtTest>
#include <QString>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <libsigrokdecode/libsigrokdecode.h>

// ---- xlog stub: annotationrestable.cpp / annotation.cpp include log.h ----
#include "log/xlog.h"
xlog_writer *pxv_log = nullptr;
extern "C" {
int xlog_err(xlog_writer *, const char *, ...) { return 0; }
int xlog_warn(xlog_writer *, const char *, ...) { return 0; }
int xlog_info(xlog_writer *, const char *, ...) { return 0; }
int xlog_dbg(xlog_writer *, const char *, ...) { return 0; }
int xlog_detail(xlog_writer *, const char *, ...) { return 0; }
}

#include "pv/data/decode/annotation.h"
#include "pv/data/decode/decoderstatus.h"
#include "pv/data/decode/rowdata.h"

using pv::data::decode::Annotation;
using pv::data::decode::RowData;

namespace {

// 一条逻辑注解：同时喂给逐注解路径与批量路径的输入描述。
struct AnnSpec {
    uint64_t start = 0;
    uint64_t end = 0;
    int ann_class = 0;
    int ann_type = 0;
    std::vector<const char *> texts;       // 以 nullptr 结尾
    char hex[DECODE_NUM_HEX_MAX_LEN] = {0};  // str_number_hex（与引擎定义同尺寸）
    long long numeric = 0;                 // numberic_value
};

// 逐注解路径: 构造 srd_proto_data + srd_proto_data_annotation 视图。
void fill_pdata(srd_proto_data &pdata, srd_proto_data_annotation &pda,
                const AnnSpec &a) {
    std::memset(&pdata, 0, sizeof(pdata));
    std::memset(&pda, 0, sizeof(pda));
    pdata.start_sample = a.start;
    pdata.end_sample = a.end;
    pdata.pdo = nullptr;
    pdata.data = &pda;
    pda.ann_class = a.ann_class;
    pda.ann_type = a.ann_type;
    pda.ann_text = const_cast<char **>(a.texts.data());
    std::memcpy(pda.str_number_hex, a.hex, sizeof(pda.str_number_hex));
    pda.numberic_value = a.numeric;
}

// 批量路径: 构造 srd_ann_item。
void fill_item(srd_ann_item &it, const AnnSpec &a) {
    std::memset(&it, 0, sizeof(it));
    it.start_sample = a.start;
    it.end_sample = a.end;
    it.ann_class = a.ann_class;
    it.ann_type = a.ann_type;
    it.ann_text = a.texts.data();
    std::memcpy(it.str_number_hex, a.hex, sizeof(it.str_number_hex));
    it.numberic_value = a.numeric;
    it.decoder = nullptr;  // 批量路径不参与 RowData 落库（decoder 归因在栈层）
}

} // namespace

// ---------------------------------------------------------------------------
// Test class
// ---------------------------------------------------------------------------
class TestRowDataBatch : public QObject {
    Q_OBJECT
private slots:
    void EmptyBatchIsNoOp();
    void RandomizedBatchMatchesPerAnnotation();
    void BothPathsInternIdentically();
    void SnapshotMatchesDeque();
};

// 空批量必须是 no-op。
void TestRowDataBatch::EmptyBatchIsNoOp() {
    RowData row;
    DecoderStatus status;
    QVERIFY(row.emplace_annotations(std::vector<const srd_ann_item *>{}, &status));
    QCOMPARE((qulonglong)row.get_annotation_size(), 0ULL);
    QCOMPARE((qulonglong)row.get_max_sample(), 0ULL);
}

// 核心等价性: 同一组随机注解，逐注解路径（RowData A）与批量路径（RowData B）
// 产出的注解序列逐条一致。
void TestRowDataBatch::RandomizedBatchMatchesPerAnnotation() {
    std::mt19937_64 rng(0x5EEDBADCULL);
    auto rnd = [&rng](uint64_t lo, uint64_t hi) {
        return lo + rng() % (hi - lo + 1);
    };

    // 文本池: 若干不同文本 + 特殊形式（'\n' 跳过、'@' 前缀、空文本）。
    const char *const text_pool[] = {
        "START", "Address", "Data", "STOP", "0x1F", "length=5",
        "\n",                 // 数值注解的忽略标记（引擎把 '@'+hex 转成它）
        "@1A",                // 字面 '@' 前缀（非数值路径，按原文入 key）
        "",                   // 空文本边界
    };

    const int kCount = 300;
    std::vector<AnnSpec> anns;
    anns.reserve(kCount);
    std::vector<std::vector<const char *>> owned_texts;  // 保持 NULL 结尾存活

    for (int i = 0; i < kCount; i++) {
        AnnSpec a;
        a.start = rnd(0, 100000);
        a.end = (rnd(0, 3) == 0) ? a.start : a.start + rnd(0, 1000);  // 含瞬时注解
        a.ann_class = (int)rnd(0, 8);
        a.ann_type = (int)rnd(0, 15);

        // 文本行数 1..3，从池中随机取（含重复 → 触发 intern）。
        int nlines = (int)rnd(1, 3);
        std::vector<const char *> texts;
        for (int l = 0; l < nlines; l++)
            texts.push_back(text_pool[rng() % (sizeof(text_pool) / sizeof(text_pool[0]))]);
        texts.push_back(nullptr);

        // 约 1/3 的注解携带数值（str_number_hex + numeric）。
        if (rng() % 3 == 0) {
            uint32_t v = (uint32_t)rng();
            std::snprintf(a.hex, sizeof(a.hex), "%X", v);
            a.numeric = (long long)v;
        }

        owned_texts.push_back(std::move(texts));
        a.texts = owned_texts.back();
        anns.push_back(a);
    }

    // ---- 逐注解路径 ----
    RowData rowA;
    DecoderStatus statusA;
    for (const AnnSpec &a : anns) {
        srd_proto_data pdata;
        srd_proto_data_annotation pda;
        fill_pdata(pdata, pda, a);
        QVERIFY(rowA.emplace_annotation(&pdata, &statusA));
    }

    // ---- 批量路径 ----
    RowData rowB;
    DecoderStatus statusB;
    {
        std::vector<srd_ann_item> items(anns.size());
        std::vector<const srd_ann_item *> ptrs(anns.size());
        for (size_t i = 0; i < anns.size(); i++) {
            fill_item(items[i], anns[i]);
            ptrs[i] = &items[i];
        }
        QVERIFY(rowB.emplace_annotations(ptrs, &statusB));
    }

    // ---- 断言: 两行完全一致 ----
    QCOMPARE((qulonglong)rowA.get_annotation_size(),
             (qulonglong)rowB.get_annotation_size());
    QCOMPARE((qulonglong)rowA.get_annotation_size(), (qulonglong)kCount);
    QCOMPARE((qulonglong)rowA.get_max_sample(), (qulonglong)rowB.get_max_sample());
    QCOMPARE((qulonglong)rowA.get_max_annotation(),
             (qulonglong)rowB.get_max_annotation());
    QCOMPARE((qulonglong)rowA.get_min_annotation(),
             (qulonglong)rowB.get_min_annotation());

    for (uint64_t i = 0; i < rowA.get_annotation_size(); i++) {
        Annotation aA, aB;
        QVERIFY(rowA.get_annotation(&aA, i));
        QVERIFY(rowB.get_annotation(&aB, i));
        QCOMPARE((qulonglong)aA.start_sample(), (qulonglong)aB.start_sample());
        QCOMPARE((qulonglong)aA.end_sample(), (qulonglong)aB.end_sample());
        QCOMPARE(aA.format(), aB.format());
        QCOMPARE(aA.type(), aB.type());
        const auto &ta = aA.annotations();
        const auto &tb = aB.annotations();
        QCOMPARE(ta.size(), tb.size());
        for (size_t t = 0; t < ta.size(); t++)
            QCOMPARE(ta[t], tb[t]);
    }
}

// 两条路径对相同输入应产生相同的 intern 结构（资源表条目数一致）。
void TestRowDataBatch::BothPathsInternIdentically() {
    RowData rowA, rowB;
    DecoderStatus statusA, statusB;

    const char *pool[] = {"AAAA", "BBBB", "CCCC"};
    std::vector<AnnSpec> anns;
    std::vector<std::vector<const char *>> owned_texts;  // 保持 NULL 结尾存活
    owned_texts.reserve(20);
    for (int i = 0; i < 20; i++) {
        AnnSpec a;
        a.start = i * 10;
        a.end = i * 10 + 5;
        a.ann_class = i % 2;
        owned_texts.emplace_back();                      // 每条单行文本
        owned_texts.back().push_back(pool[i % 3]);
        owned_texts.back().push_back(nullptr);
        a.texts = owned_texts.back();
        anns.push_back(std::move(a));
    }

    for (const AnnSpec &a : anns) {
        srd_proto_data pdata;
        srd_proto_data_annotation pda;
        fill_pdata(pdata, pda, a);
        QVERIFY(rowA.emplace_annotation(&pdata, &statusA));
    }

    std::vector<srd_ann_item> items(anns.size());
    std::vector<const srd_ann_item *> ptrs;
    ptrs.reserve(anns.size());
    for (size_t i = 0; i < anns.size(); i++) {
        fill_item(items[i], anns[i]);
        ptrs.push_back(&items[i]);
    }
    QVERIFY(rowB.emplace_annotations(ptrs, &statusB));

    // 三条不同文本 → 每个 status 的资源表应恰好 3 个条目（intern 生效）。
    QCOMPARE(statusA.m_resTable.GetCount(), 3);
    QCOMPARE(statusB.m_resTable.GetCount(), 3);
    QCOMPARE(statusA.m_resTable.GetCount(), statusB.m_resTable.GetCount());
}

// 快照（渲染路径读取的不可变视图）与 live deque 数据一致。
void TestRowDataBatch::SnapshotMatchesDeque() {
    RowData row;
    DecoderStatus status;

    const char *texts[] = {"A", "B", nullptr};
    for (int i = 0; i < 50; i++) {
        srd_proto_data pdata;
        srd_proto_data_annotation pda;
        AnnSpec a;
        a.start = i * 2;
        a.end = i * 2 + 1;
        a.ann_class = 1;
        a.ann_type = 2;
        a.texts.assign(texts, texts + 3);
        fill_pdata(pdata, pda, a);
        QVERIFY(row.emplace_annotation(&pdata, &status));
    }

    auto snap = row.frozen_snapshot();
    QVERIFY(snap);
    QCOMPARE((qulonglong)snap->get_annotation_size(), 50ULL);
    QCOMPARE((qulonglong)snap->get_max_sample(), (qulonglong)row.get_max_sample());

    for (uint64_t i = 0; i < 50; i++) {
        Annotation aLive, aSnap;
        QVERIFY(row.get_annotation(&aLive, i));
        QVERIFY(snap->get_annotation(&aSnap, i));
        QCOMPARE((qulonglong)aLive.start_sample(), (qulonglong)aSnap.start_sample());
        QCOMPARE((qulonglong)aLive.end_sample(), (qulonglong)aSnap.end_sample());
        QCOMPARE(aLive.format(), aSnap.format());
        QCOMPARE(aLive.type(), aSnap.type());
    }
}

QTEST_MAIN(TestRowDataBatch)
#include "test_rowdata_batch.moc"
