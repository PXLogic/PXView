/*
 * test_binary_name_hints.cpp — raw binary 文件名里的参数提示（通道数/采样率）。
 *
 * 背景（2026-09-24 用户提问）：raw ".binary" 没有任何头部，导入时通道数与采样率
 * 只能靠猜（当前设备 enabled 逻辑通道数，猜不到就 8；采样率取当前设备，取不到
 * 1 MHz），一旦文件不是从"当前开着的设备"导出的就必错。现约定把这两个参数写进
 * 文件名（导出时自动写入，导入时自动填入对话框）：
 *
 *     <base>-<channels>ch-<samplerate>Hz.binary
 *
 * 本文件锁定该约定的两侧：parse() 的宽容度（大小写、顺序、SI 后缀、只给一半、
 * 目录名里的点）与 apply() 的幂等性/不臆造值，并做 parse∘apply 往返。
 */

#include <QtTest/QtTest>

#include "pv/data/binary_name_hints.h"

namespace hints = pv::data::binary_name_hints;

class TestBinaryNameHints : public QObject
{
    Q_OBJECT

private slots:
    /* PXView 自己写出的名字必须能被自己读回。 */
    void parsesOwnWrittenForm();

    /* 手改名/其它工具的名字：大小写、顺序、SI 后缀、只有一半信息。 */
    void parsesTolerantForms();

    /* 不得把时间戳当采样率、不得把 "channels" 当通道数。 */
    void doesNotInventHints();

    /* 写入位置：扩展名之前；无扩展名也不出错；目录名里的点不算扩展名。 */
    void writesHintBlockInPlace();

    /* 幂等：重复写入不会叠加 token，旧块被替换。 */
    void applyIsIdempotent();

    /* 未知参数不写（不臆造 0 值），空名字原样返回。 */
    void applySkipsUnknownValues();

    /* 往返：parse(apply(name, ch, rate)) == {ch, rate}。 */
    void roundTrips();
};

void TestBinaryNameHints::parsesOwnWrittenForm()
{
    const hints::Hints h = hints::parse(
        QStringLiteral("Demo device-LA-260924-213100-32ch-1000000Hz.binary"));
    QCOMPARE(h.channels, 32);
    QCOMPARE(h.samplerate, static_cast<uint64_t>(1000000));
    QVERIFY(!h.empty());

    // Full paths work the same (the tokens are what matters).
    const hints::Hints p = hints::parse(
        QStringLiteral("C:/Users/x/Downloads/a-8ch-500000Hz.binary"));
    QCOMPARE(p.channels, 8);
    QCOMPARE(p.samplerate, static_cast<uint64_t>(500000));
}

void TestBinaryNameHints::parsesTolerantForms()
{
    QCOMPARE(hints::parse(QStringLiteral("dump-16CH-2kHz.bin")).channels, 16);
    QCOMPARE(hints::parse(QStringLiteral("dump-16CH-2kHz.bin")).samplerate,
             static_cast<uint64_t>(2000));
    QCOMPARE(hints::parse(QStringLiteral("a-1MHz-8ch.bin")).samplerate,
             static_cast<uint64_t>(1000000));
    QCOMPARE(hints::parse(QStringLiteral("a-1MHz-8ch.bin")).channels, 8);
    QCOMPARE(hints::parse(QStringLiteral("a-3ghz-2ch.bin")).samplerate,
             static_cast<uint64_t>(3000000000ULL));
    QCOMPARE(hints::parse(QStringLiteral("a_4ch_250000Hz.bin")).channels, 4);
    QCOMPARE(hints::parse(QStringLiteral("a 4 ch 250000 hz.bin")).channels, 4);

    // Half-known names are useful too (the other value keeps its own default).
    QCOMPARE(hints::parse(QStringLiteral("x-4ch.bin")).channels, 4);
    QCOMPARE(hints::parse(QStringLiteral("x-4ch.bin")).samplerate,
             static_cast<uint64_t>(0));
    QCOMPARE(hints::parse(QStringLiteral("x-1MHz.bin")).channels, 0);
    QCOMPARE(hints::parse(QStringLiteral("x-1MHz.bin")).samplerate,
             static_cast<uint64_t>(1000000));
}

void TestBinaryNameHints::doesNotInventHints()
{
    // The pre-fix export name: a timestamp, no tokens.
    const hints::Hints h = hints::parse(
        QStringLiteral("Demo device-LA-260924-213312.binary"));
    QVERIFY(h.empty());
    QCOMPARE(h.channels, 0);
    QCOMPARE(h.samplerate, static_cast<uint64_t>(0));

    // "channels"/"8channels" must not be read as a channel count.
    QVERIFY(hints::parse(QStringLiteral("channels-8.bin")).empty());
    QVERIFY(hints::parse(QStringLiteral("8channels.bin")).empty());
    QVERIFY(hints::parse(QString::fromUtf8("中文名-无参数.bin")).empty());
    QVERIFY(hints::parse(QString()).empty());
}

void TestBinaryNameHints::writesHintBlockInPlace()
{
    QCOMPARE(hints::apply(QStringLiteral("x.binary"), 32, 1000000),
             QStringLiteral("x-32ch-1000000Hz.binary"));
    QCOMPARE(hints::apply(QStringLiteral("/d/x.binary"), 8, 1000),
             QStringLiteral("/d/x-8ch-1000Hz.binary"));
    // No extension at all (MakeExportFile appends it afterwards).
    QCOMPARE(hints::apply(QStringLiteral("/d/x"), 8, 1000),
             QStringLiteral("/d/x-8ch-1000Hz"));
    // A dot in the directory must not be mistaken for the file's extension.
    QCOMPARE(hints::apply(QStringLiteral("/a.b/x"), 8, 1000),
             QStringLiteral("/a.b/x-8ch-1000Hz"));
}

void TestBinaryNameHints::applyIsIdempotent()
{
    const QString once = hints::apply(QStringLiteral("x.binary"), 32, 1000000);
    QCOMPARE(hints::apply(once, 32, 1000000), once);

    // Re-exporting with different values replaces the block, not appends to it.
    QCOMPARE(hints::apply(once, 16, 500000),
             QStringLiteral("x-16ch-500000Hz.binary"));

    // A block written by hand in another style is replaced as well.
    QCOMPARE(hints::apply(QStringLiteral("x-4CH-2kHz.bin"), 8, 1000),
             QStringLiteral("x-8ch-1000Hz.bin"));
}

void TestBinaryNameHints::applySkipsUnknownValues()
{
    QCOMPARE(hints::apply(QStringLiteral("x.binary"), 0, 0),
             QStringLiteral("x.binary"));
    QCOMPARE(hints::apply(QString(), 8, 1000), QString());
    QCOMPARE(hints::apply(QStringLiteral("x.binary"), 0, 1000000),
             QStringLiteral("x-1000000Hz.binary"));
}

void TestBinaryNameHints::roundTrips()
{
    const QList<QPair<int, uint64_t>> cases = {
        {1, 1},
        {2, 1000},
        {8, 1000000},
        {16, 20000000},
        {32, 1000000000ULL},
    };

    for (const auto &c : cases) {
        const QString name = hints::apply(
            QStringLiteral("Demo device-LA-260924-213100.binary"), c.first, c.second);
        const hints::Hints h = hints::parse(name);
        QCOMPARE(h.channels, c.first);
        QCOMPARE(h.samplerate, c.second);
    }
}

QTEST_MAIN(TestBinaryNameHints)
#include "test_binary_name_hints.moc"
