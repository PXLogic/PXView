/*
 * test_csv_roundtrip.cpp — PXView 导出的 CSV 能否用 csv 输入模块的**默认选项**读回来。
 *
 * 背景（2026-09-25 用户提问）：导入 CSV 时弹出的选项对话框里有十来项
 * （Column format specs / Single column / Number of logic channels / Start line /
 * Get channel names from first line / Samplerate (Hz) / Column separator /
 * Comment leader character ...），用户问"为什么还要自己填、是不是没提取到元数据"。
 *
 * 这些字段全部来自 libsigrok csv **输入模块自己声明的 options**（csv.c 的
 * options[]），没有任何一项是 PXView 加的，上游 PulseView 弹出的也是同一批。
 * 而 csv 输入模块只用 comment_leader 把注释行**剥掉**，不解析里面的
 * "Samplerate:" / "Channels (...)"——它要么用 samplerate 选项，要么从行内时间戳
 * 推断。所以"按确定就能用吗"必须实测，本文件就是那次实测的固化：
 *
 *   1) 用 csv 输出模块按 PXView 现在的配置（label=channel + SR_DF_HEADER 注释块）
 *      生成一段 CSV；
 *   2) 把这段文本喂给 csv **输入**模块，选项表留空（= 模块默认值，等价于用户在
 *      对话框里什么都不改直接确定）；
 *   3) 断言通道数、通道名（必须是我们导出的 D0/D1，即头部行被当成了列标题）、
 *      以及 sr_input_scan_file() 能把这种文件识别成 csv（Auto detect 入口）。
 *
 * 依赖：libsigrok 公开 API（sr_output_* + sr_input_*），普通链接 libsigrok。
 */

#include <QtTest/QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <glib.h>
#include <libsigrok/libsigrok.h>

#include <cstdint>
#include <string>
#include <vector>

#include "pv/data/csv_header_hints.h"

namespace {

constexpr int kChannels = 2;
constexpr uint64_t kSamplerate = 1000000;
constexpr size_t kSamples = 8;

/* 假设备：struct sr_dev_inst 在公开头里是不完整类型，通道只能经 getter 取。 */
class FakeDevice
{
public:
    FakeDevice()
    {
        _sdi = sr_dev_inst_user_new("qtest", "csv-roundtrip", "1.0");
        sr_dev_inst_channel_add(_sdi, 0, SR_CHANNEL_LOGIC, "D0");
        sr_dev_inst_channel_add(_sdi, 1, SR_CHANNEL_LOGIC, "D1");
    }
    ~FakeDevice()
    {
        if (_sdi)
            sr_dev_inst_free(_sdi);
    }

    FakeDevice(const FakeDevice &) = delete;
    FakeDevice &operator=(const FakeDevice &) = delete;

    struct sr_dev_inst *inst() const { return _sdi; }

private:
    struct sr_dev_inst *_sdi = nullptr;
};

/* 用 csv 输出模块生成与 PXView 导出一致的文本：META + HEADER + LOGIC + END，
 * 且 label=channel（PXView 现在的固定值）。 */
std::string export_csv(struct sr_dev_inst *sdi)
{
    const struct sr_output_module *module = sr_output_find(const_cast<char *>("csv"));
    if (!module)
        return std::string();

    GHashTable *opts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify)g_variant_unref);
    g_hash_table_insert(opts, g_strdup("label"),
                        g_variant_ref_sink(g_variant_new_string("channel")));
    const struct sr_output *out = sr_output_new(module, opts, sdi, "qtest_roundtrip.csv");
    g_hash_table_destroy(opts);
    if (!out)
        return std::string();

    std::string text;
    struct sr_datafeed_packet packet = {};

    /* META（采样率）——PXView 也是先发它。 */
    struct sr_config cfg;
    cfg.key = SR_CONF_SAMPLERATE;
    cfg.data = g_variant_ref_sink(g_variant_new_uint64(kSamplerate));
    struct sr_datafeed_meta meta;
    meta.config = g_slist_append(nullptr, &cfg);
    packet.type = SR_DF_META;
    packet.payload = &meta;
    GString *chunk = nullptr;
    sr_output_send(out, &packet, &chunk);
    if (chunk) {
        text.append(chunk->str, chunk->len);
        g_string_free(chunk, TRUE);
    }
    g_slist_free(meta.config);
    g_variant_unref(cfg.data);

    /* HEADER——注释块（gen_header() 只在这个包上跑）。 */
    struct sr_datafeed_header header = {};
    header.feed_version = 1;
    header.starttime.tv_sec = 1769212800;
    packet.type = SR_DF_HEADER;
    packet.payload = &header;
    chunk = nullptr;
    sr_output_send(out, &packet, &chunk);
    if (chunk) {
        text.append(chunk->str, chunk->len);
        g_string_free(chunk, TRUE);
    }

    /* 一包逻辑数据：8 样本 × 2 通道，unitsize 1 字节。 */
    std::vector<uint8_t> frame(kSamples, 0x01);
    struct sr_datafeed_logic logic = {};
    logic.data = frame.data();
    logic.length = frame.size();
    logic.unitsize = 1;
    packet.type = SR_DF_LOGIC;
    packet.payload = &logic;
    chunk = nullptr;
    sr_output_send(out, &packet, &chunk);
    if (chunk) {
        text.append(chunk->str, chunk->len);
        g_string_free(chunk, TRUE);
    }

    packet.type = SR_DF_END;
    packet.payload = nullptr;
    chunk = nullptr;
    sr_output_send(out, &packet, &chunk);
    if (chunk) {
        text.append(chunk->str, chunk->len);
        g_string_free(chunk, TRUE);
    }

    sr_output_free(out);
    return text;
}

/* 用 csv 输入模块读一段文本；options 为 nullptr 表示完全用模块默认值。 */
struct ImportResult {
    bool created = false;
    bool sent_ok = false;
    int channels = -1;
    std::vector<std::string> names;
};

ImportResult import_csv(const std::string &text, GHashTable *options)
{
    ImportResult result;

    const struct sr_input_module *module = sr_input_find("csv");
    if (!module)
        return result;

    const struct sr_input *input = sr_input_new(module, options);
    if (!input)
        return result;
    result.created = true;

    GString *buf = g_string_new_len(text.data(), static_cast<gssize>(text.size()));
    result.sent_ok = sr_input_send(input, buf) == SR_OK;
    g_string_free(buf, TRUE);

    struct sr_dev_inst *sdi = sr_input_dev_inst_get(input);
    if (sdi) {
        GSList *channels = sr_dev_inst_channels_get(sdi);
        result.channels = static_cast<int>(g_slist_length(channels));
        for (GSList *l = channels; l; l = l->next) {
            auto *ch = static_cast<struct sr_channel *>(l->data);
            result.names.push_back(ch->name ? ch->name : "");
        }
    }

    /* sdi 归输入实例所有，sr_input_free() 一并释放。 */
    sr_input_free(input);
    return result;
}

} // namespace

class TestCsvRoundtrip : public QObject
{
    Q_OBJECT

private slots:
    /* 模块默认选项（= 用户什么都不改直接确定）必须能读回我们导出的 CSV。 */
    void defaultsImportOurOwnExport();

    /* Auto detect 入口（sr_input_scan_file）必须把这种文件识别成 csv。 */
    void scanFileDetectsOurOwnExportAsCsv();

    /* 我们自己写的注释头必须能被解析回来（采样率/通道数 → 导入对话框预填）。 */
    void headerHintsAreParsedFromOurOwnExport();

    /* 非生成的 CSV / 缺 marker / 数值畸形的文本不得产生假预填。 */
    void headerHintsRefuseForeignText();
};

void TestCsvRoundtrip::defaultsImportOurOwnExport()
{
    FakeDevice device;
    QVERIFY(device.inst() != nullptr);

    const std::string text = export_csv(device.inst());
    QVERIFY(!text.empty());

    // 导出文本应当是我们现在写的形态：注释块在最前、随后是通道名行。
    QVERIFY(text.rfind("; CSV generated by", 0) == 0);
    QVERIFY(text.find("D0,D1") != std::string::npos);

    // 空选项表 → sr_input_new() 会填模块自己的默认值，等价于对话框直接确定。
    const ImportResult result = import_csv(text, nullptr);
    QVERIFY(result.created);
    QVERIFY(result.sent_ok);
    QCOMPARE(result.channels, kChannels);

    // 通道名来自导出时的那一行表头（"Get channel names from first line" 默认开）。
    QCOMPARE(result.names.size(), static_cast<size_t>(kChannels));
    QCOMPARE(QString::fromStdString(result.names[0]), QStringLiteral("D0"));
    QCOMPARE(QString::fromStdString(result.names[1]), QStringLiteral("D1"));
}

void TestCsvRoundtrip::scanFileDetectsOurOwnExportAsCsv()
{
    FakeDevice device;
    QVERIFY(device.inst() != nullptr);

    const std::string text = export_csv(device.inst());
    QVERIFY(!text.empty());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 故意用一个不叫 *.csv 的名字：识别必须靠内容而不是扩展名。
    const QString path = dir.filePath(QStringLiteral("export_without_extension"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(text.data(), static_cast<qint64>(text.size())) > 0);
    file.close();

    const struct sr_input *detected = nullptr;
    const int ret = sr_input_scan_file(path.toStdString().c_str(), &detected);
    QCOMPARE(ret, SR_OK);
    QVERIFY(detected != nullptr);

    const struct sr_input_module *module = sr_input_module_get(detected);
    QVERIFY(module != nullptr);
    QCOMPARE(QString::fromUtf8(sr_input_id_get(module)), QStringLiteral("csv"));

    sr_input_free(detected);
}

void TestCsvRoundtrip::headerHintsAreParsedFromOurOwnExport()
{
    FakeDevice device;
    QVERIFY(device.inst() != nullptr);

    // 用真实导出的文本（含 libsigrok gen_header() 的实际措辞），而不是手写的
    // 测试串——这正是"注释头里到底写了什么"的证据。
    const std::string text = export_csv(device.inst());
    QVERIFY(!text.empty());

    const pv::data::csv_header_hints::Hints hints =
        pv::data::csv_header_hints::parse(QString::fromStdString(text));
    QCOMPARE(hints.channels, kChannels);

    // 采样率这行在本测试里**故意不存在**：gen_header() 只从设备配置读
    // SR_CONF_SAMPLERATE（csv 的 receive() 没有 SR_DF_META 分支），而
    // sr_dev_inst_user_new() 造出来的 sdi 没有 driver → 读不到 → 整行不写。
    // 真实设备（或文件设备）有 driver，所以线上导出的 CSV 是带这行的；这里锁住
    // 解析器"没有就说没有"的行为，SI 形式的解析由下一个用例覆盖。
    QVERIFY(text.find("Samplerate:") == std::string::npos);
    QCOMPARE(hints.samplerate, static_cast<uint64_t>(0));
}

void TestCsvRoundtrip::headerHintsRefuseForeignText()
{
    using pv::data::csv_header_hints::parse;

    // 普通 CSV：没有我们的注释块 → 不得预填。
    QVERIFY(parse(QStringLiteral("a,b\n0,1\n0,1\n")).empty());
    QVERIFY(parse(QString()).empty());

    // 关键防线：光有一行 "Samplerate" 而没有生成标记的文本不算数（否则随便一个
    // 含该字样的文本文件都会伪造出预填值）。
    QVERIFY(parse(QStringLiteral("; Samplerate: 1 MHz\n")).empty());

    // 有标记时按 SI 后缀/小数解析（gen_header 用的是 sr_samplerate_string()）。
    const auto khz = parse(QStringLiteral(
        "; CSV generated by PXView 1.6.5\n"
        "; Channels (16/16): D0\n"
        "; Samplerate: 31.5 kHz\n"));
    QCOMPARE(khz.channels, 16);
    QCOMPARE(khz.samplerate, static_cast<uint64_t>(31500));

    const auto ghz = parse(QStringLiteral(
        "; CSV generated by PulseView\n"
        "; Samplerate: 1.5 GHz\n"));
    QCOMPARE(ghz.samplerate, static_cast<uint64_t>(1500000000));

    const auto plain = parse(QStringLiteral(
        "; CSV generated by PXView 1.6.5\n"
        "; Channels (2/2): D0, D1\n"
        "; Samplerate: 999 Hz\n"));
    QCOMPARE(plain.channels, 2);
    QCOMPARE(plain.samplerate, static_cast<uint64_t>(999));

    // 数值畸形：能认出来的部分照旧，认不出的留 0，而不是瞎猜。
    const auto broken = parse(QStringLiteral(
        "; CSV generated by PXView 1.6.5\n"
        "; Channels (8/8): D0\n"
        "; Samplerate: unknown\n"));
    QCOMPARE(broken.channels, 8);
    QCOMPARE(broken.samplerate, static_cast<uint64_t>(0));
}

QTEST_MAIN(TestCsvRoundtrip)
#include "test_csv_roundtrip.moc"
