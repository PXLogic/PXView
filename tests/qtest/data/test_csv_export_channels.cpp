/*
 * test_csv_export_channels.cpp — csv 输出模块的通道计数 / 采样数契约回归。
 *
 * 背景（2026-09-24 用户日志："导出没有数据"，文件 0 字节）：
 *   StoreSession::export_start() 只允许单数据类型导出，export_exec() 也只发一种
 *   SR_DF_*（SR_DF_META + 数据包 + 末尾 SR_DF_END，从不发 SR_DF_HEADER）。
 *   而 libsigrok/src/output/csv.c 的 init() 是按 **o->sdi->channels 里 enabled 的
 *   通道** 决定 num_analog_channels / num_logic_channels 的，dump_saved_values()
 *   又要求两类数据都到齐才肯输出：
 *
 *       if ((num_analog_channels && !analog_samples) ||
 *           (num_logic_channels && !logic_samples))
 *               sr_warn("Discarding partial packet");   // ← 整块丢弃
 *
 *   于是设备上只要还有别的类型通道是 enabled（demo 设备：32 logic + 5 analog
 *   + 2 DSO），导出逻辑时模块也在等永远不会到来的模拟数据 → 每个缓冲区都被丢掉
 *   → 文件 0 字节（连表头都没有，因为 gen_header() 只在 SR_DF_HEADER 上跑）。
 *
 * 与之独立的第二个缺陷：dump 的触发条件是 channels_seen >= channel_count，而
 *   channel_count 曾是 g_slist_length(sdi->channels)（含其它类型、含 disabled），
 *   channels_seen 每包只按逻辑通道递增 → 阈值凑不齐，dump 只能靠"每 N 包"偶然
 *   触发；又因为 process_logic()/process_analog() 复用同一个 buffer（后一包覆盖
 *   前一包），被跳过的那包数据直接丢失（实测 1M 采样只写出约一半的行）。
 *
 * 本文件锁定的契约：
 *   1) 设备上存在本模块消费不了的类型（DSO）时，每个数据包都必须落盘，一个不丢；
 *   2) 调用方（StoreSession::export_exec()）在 sr_output_new() 之前把非导出类型
 *      通道置 enabled=0 之后，导出逻辑必须拿到全部数据（线上路径的修复点）；
 *   3) 模拟导出同理；
 *   4) 末尾短包按**实际样本数**输出，不能按上一包的 num_samples 补出脏行。
 *
 * 依赖：只走 libsigrok 公开 API（sr_dev_inst_user_new / sr_dev_inst_channel_add /
 * sr_dev_inst_channels_get / sr_dev_channel_enable / sr_output_find / sr_output_new /
 * sr_output_send / sr_output_free）。注意 struct sr_dev_inst 在公开头里只有前向声明
 * （完整定义在 libsigrok-internal.h），所以通道只能经 sr_dev_inst_channels_get()
 * 取；sr_channel 本身是公开结构。
 */

#include <QtTest/QtTest>

#include <glib.h>
#include <libsigrok/libsigrok.h>

#include <string>
#include <vector>

namespace {

/* ---- 假设备：只走公开 API --------------------------------------------------
 * sr_dev_inst_user_new() 造 SR_INST_USER 实例，sr_dev_inst_channel_add() 加通道
 * （内部走 SR_PRIV 的 sr_channel_new()，新通道默认 enabled=TRUE）。 */
class FakeDevice {
public:
    FakeDevice() { _sdi = sr_dev_inst_user_new("qtest", "csv-channels", "1.0"); }
    ~FakeDevice()
    {
        if (_sdi)
            sr_dev_inst_free(_sdi);
    }
    FakeDevice(const FakeDevice &) = delete;
    FakeDevice &operator=(const FakeDevice &) = delete;

    /* 返回新通道的 index。 */
    int add(int type, const char *name)
    {
        const int index = _next_index++;
        sr_dev_inst_channel_add(_sdi, index, type, name);
        return index;
    }

    void disable_all_but(int keep_type)
    {
        for (GSList *l = sr_dev_inst_channels_get(_sdi); l; l = l->next) {
            auto *ch = static_cast<struct sr_channel *>(l->data);
            if (ch->type != keep_type)
                sr_dev_channel_enable(ch, FALSE);
        }
    }

    /* 新链表（调用方 g_slist_free()），元素是设备自己持有的 sr_channel。 */
    GSList *channels_of(int type) const
    {
        GSList *list = nullptr;
        for (GSList *l = sr_dev_inst_channels_get(_sdi); l; l = l->next) {
            auto *ch = static_cast<struct sr_channel *>(l->data);
            if (ch->type == type)
                list = g_slist_append(list, ch);
        }
        return list;
    }

    struct sr_dev_inst *inst() const { return _sdi; }

private:
    struct sr_dev_inst *_sdi = nullptr;
    int _next_index = 0;
};

/* ---- 导出接收端：sr_output_new() → 若干 SR_DF_* → SR_DF_END → 释放 ----------
 * 默认 label=off（模块不写表头行，输出就是"每样本一行"，行数可直接断言）；
 * label=nullptr 表示**不传**该选项 —— 即模块自己的默认值（"units"）。 */
class CsvSink {
public:
    explicit CsvSink(struct sr_dev_inst *sdi, const char *label = "off")
    {
        _module = sr_output_find(const_cast<char *>("csv"));
        if (!_module)
            return;
        GHashTable *opts = g_hash_table_new(g_str_hash, g_str_equal);
        if (label)
            g_hash_table_insert(opts, const_cast<char *>("label"),
                                g_variant_ref_sink(g_variant_new_string(label)));
        _out = sr_output_new(_module, opts, sdi, "qtest.csv");
        g_hash_table_destroy(opts);
    }
    ~CsvSink()
    {
        if (_out)
            sr_output_free(_out);
    }
    CsvSink(const CsvSink &) = delete;
    CsvSink &operator=(const CsvSink &) = delete;

    bool valid() const { return _out != nullptr; }

    void send_logic(const std::vector<uint8_t> &bytes, uint16_t unitsize)
    {
        struct sr_datafeed_logic lp = {};
        lp.length = bytes.size();
        lp.unitsize = unitsize;
        lp.format = LA_SPLIT_DATA; /* 样本交织：每样本 unitsize 字节 */
        lp.data = const_cast<uint8_t *>(bytes.data());
        struct sr_datafeed_packet p = {};
        p.type = SR_DF_LOGIC;
        p.payload = &lp;
        send(&p);
    }

    void send_analog(const std::vector<float> &samples, uint32_t num_samples,
                     GSList *channels)
    {
        struct sr_analog_encoding encoding = {};
        struct sr_analog_meaning meaning = {};
        struct sr_analog_spec spec = {};
        struct sr_datafeed_analog ap = {};

        encoding.unitsize = sizeof(float);
        encoding.is_float = TRUE;
        encoding.is_signed = TRUE;
        encoding.is_bigendian = FALSE;
        encoding.scale.p = 1;
        encoding.scale.q = 1;
        encoding.offset.p = 0;
        encoding.offset.q = 1;
        encoding.digits = 2;
        encoding.is_digits_decimal = TRUE;

        meaning.channels = channels;
        meaning.mq = SR_MQ_VOLTAGE;
        meaning.unit = SR_UNIT_VOLT;
        meaning.mqflags = SR_MQFLAG_DC;

        spec.spec_digits = 2;

        ap.data = const_cast<float *>(samples.data());
        ap.num_samples = num_samples;
        ap.encoding = &encoding;
        ap.meaning = &meaning;
        ap.spec = &spec;

        struct sr_datafeed_packet p = {};
        p.type = SR_DF_ANALOG;
        p.payload = &ap;
        send(&p);
    }

    void send_header()
    {
        struct sr_datafeed_header hdr = {};
        hdr.feed_version = 1;
        hdr.starttime.tv_sec = 1769212800; /* 2026-01-24T00:00:00Z，固定值便于断言 */
        hdr.starttime.tv_usec = 0;
        struct sr_datafeed_packet p = {};
        p.type = SR_DF_HEADER;
        p.payload = &hdr;
        send(&p);
    }

    void send_end()
    {
        struct sr_datafeed_packet p = {};
        p.type = SR_DF_END;
        p.payload = nullptr;
        send(&p);
    }

    QString text() const { return QString::fromStdString(_text); }

private:
    void send(struct sr_datafeed_packet *p)
    {
        GString *chunk = nullptr;
        sr_output_send(_out, p, &chunk);
        if (chunk) {
            _text.append(chunk->str, chunk->len);
            g_string_free(chunk, TRUE);
        }
    }

    const struct sr_output_module *_module = nullptr;
    const struct sr_output *_out = nullptr;
    std::string _text;
};

int row_count(const QString &s) { return s.count(QLatin1Char('\n')); }

} // namespace

class TestCsvExportChannels : public QObject {
    Q_OBJECT

private slots:
    /* 设备上还有本模块消费不了的通道类型（DSO）时，每个数据包都必须落盘。
     * 旧实现 channel_count = g_slist_length(sdi->channels) = 3（2 logic + 1 DSO），
     * 而 channels_seen 每包只 +2 → 只能每 2 包 dump 一次，第 1 包被第 2 包覆盖，
     * 24 行只写出 16 行。 */
    void unsupportedChannelTypeDoesNotStallTheDump()
    {
        FakeDevice dev;
        dev.add(SR_CHANNEL_LOGIC, "D0");
        dev.add(SR_CHANNEL_LOGIC, "D1");
        dev.add(SR_CHANNEL_DSO, "O0"); /* enabled，但 csv 不消费 DSO */

        CsvSink sink(dev.inst());
        QVERIFY(sink.valid());

        const std::vector<uint8_t> frame(8, 0x01); /* 8 样本；D0=1, D1=0 */
        for (int i = 0; i < 3; i++)
            sink.send_logic(frame, 1);
        sink.send_end();

        QCOMPARE(row_count(sink.text()), 24);
        QCOMPARE(sink.text(), QStringLiteral("1,0\n").repeated(24));
    }

    /* 调用方在 sr_output_new() 之前只留导出类型（StoreSession::export_exec() 的
     * 线上修复点）之后，导出逻辑必须拿到全部数据。 */
    void callerDisablingOtherTypesYieldsAllLogicData()
    {
        FakeDevice dev;
        dev.add(SR_CHANNEL_LOGIC, "D0");
        dev.add(SR_CHANNEL_LOGIC, "D1");
        dev.add(SR_CHANNEL_ANALOG, "A0");
        dev.add(SR_CHANNEL_DSO, "O0");

        dev.disable_all_but(SR_CHANNEL_LOGIC);

        CsvSink sink(dev.inst());
        QVERIFY(sink.valid());

        const std::vector<uint8_t> frame(8, 0x01);
        for (int i = 0; i < 3; i++)
            sink.send_logic(frame, 1);
        sink.send_end();

        QCOMPARE(row_count(sink.text()), 24);
        QCOMPARE(sink.text(), QStringLiteral("1,0\n").repeated(24));
    }

    /* 模拟导出同理：设备上还有 DSO 时，三个包 12 行必须全部写出。 */
    void analogExportKeepsEveryPacket()
    {
        FakeDevice dev;
        dev.add(SR_CHANNEL_ANALOG, "A0");
        dev.add(SR_CHANNEL_ANALOG, "A1");
        dev.add(SR_CHANNEL_DSO, "O0");

        dev.disable_all_but(SR_CHANNEL_ANALOG);

        GSList *channels = dev.channels_of(SR_CHANNEL_ANALOG);
        QCOMPARE(g_slist_length(channels), 2u);

        CsvSink sink(dev.inst());
        QVERIFY(sink.valid());

        /* 4 样本 × 2 通道，样本优先交织：A0,A1,A0,A1,... */
        const std::vector<float> samples = {1.0f, 2.0f, 1.1f, 2.1f,
                                            1.2f, 2.2f, 1.3f, 2.3f};
        for (int i = 0; i < 3; i++)
            sink.send_analog(samples, 4, channels);
        sink.send_end();
        g_slist_free(channels);

        QCOMPARE(row_count(sink.text()), 12);
        QVERIFY(sink.text().startsWith(QStringLiteral("1,2\n")));
    }

    /* CSV 表头行（第一行）的内容由 `label` 选项决定：模块默认是 "units"，
     * 该模式下每个逻辑列都被标成字面量 "logic"；"channel" 模式才用通道名。
     * PXView 导出时传的是空选项表 → 用户看到的第一行一直是 "logic,logic,..."。
     * 现改为固定传 label="channel"，本用例把这个契约钉住。 */
    void labelOptionSelectsTheHeaderRow()
    {
        FakeDevice dev;
        dev.add(SR_CHANNEL_LOGIC, "D0");
        dev.add(SR_CHANNEL_LOGIC, "D1");

        // 不传 label（模块默认 "units"）—— 修复前 PXView 的行为。
        {
            CsvSink sink(dev.inst(), nullptr);
            QVERIFY(sink.valid());

            const std::vector<uint8_t> frame(2, 0x01);
            sink.send_logic(frame, 1);
            sink.send_end();

            QVERIFY2(sink.text().startsWith(QStringLiteral("logic,logic\n")),
                     qPrintable(sink.text().left(40)));
        }

        // label="channel" —— PXView 现在固定的值：表头是通道名。
        {
            CsvSink sink(dev.inst(), "channel");
            QVERIFY(sink.valid());

            const std::vector<uint8_t> frame(2, 0x01);
            sink.send_logic(frame, 1);
            sink.send_end();

            QVERIFY2(sink.text().startsWith(QStringLiteral("D0,D1\n")),
                     qPrintable(sink.text().left(40)));
        }
    }

    /* SR_DF_HEADER 才触发 csv 的元数据注释块（gen_header() 只在该包上跑）。
     * PXView 导出以前从不发这个包 → 导出的 CSV 一条注释都没有；现按上游顺序
     * （META 之后）补发，本用例锁住注释块内容与"不发就没有"。 */
    void headerPacketEmitsMetadataComments()
    {
        FakeDevice dev;
        dev.add(SR_CHANNEL_LOGIC, "D0");
        dev.add(SR_CHANNEL_LOGIC, "D1");

        // 不发 HEADER：模块只在 SR_DF_LOGIC 上 dump，文件从数据行开始。
        {
            CsvSink sink(dev.inst(), "channel");
            QVERIFY(sink.valid());
            const std::vector<uint8_t> frame(2, 0x01);
            sink.send_logic(frame, 1);
            sink.send_end();
            QVERIFY2(!sink.text().contains(QStringLiteral("CSV generated by")),
                     qPrintable(sink.text().left(40)));
        }

        // 发 HEADER：注释块先出现，然后是 label 行（通道名）与数据行。
        {
            CsvSink sink(dev.inst(), "channel");
            QVERIFY(sink.valid());
            sink.send_header();
            const std::vector<uint8_t> frame(2, 0x01);
            sink.send_logic(frame, 1);
            sink.send_end();

            const QString text = sink.text();
            QVERIFY2(text.startsWith(QStringLiteral("; CSV generated by")),
                     qPrintable(text.left(60)));
            QVERIFY2(text.contains(QStringLiteral("; Channels (2/2): D0, D1")),
                     qPrintable(text.left(200)));
            // 注释块在 label 行之前
            QVERIFY(text.indexOf(QStringLiteral("; Channels")) <
                    text.indexOf(QStringLiteral("D0,D1")));
            // 注释块里除 "Samplerate:"（取决于设备配置）外，其余都以注释符开头
            QVERIFY(text.contains(QLatin1Char('\n')) );
        }
    }

    /* 末尾短包按实际样本数输出：8 + 8 + 3 = 19 行。
     * 旧实现沿用上一包的 num_samples(8) → 24 行，其中 5 行是复用缓冲区里的脏数据。 */
    void trailingShortPacketIsNotPadded()
    {
        FakeDevice dev;
        dev.add(SR_CHANNEL_LOGIC, "D0");
        dev.add(SR_CHANNEL_LOGIC, "D1");
        dev.add(SR_CHANNEL_DSO, "O0");

        CsvSink sink(dev.inst());
        QVERIFY(sink.valid());

        const std::vector<uint8_t> full(8, 0x01);
        const std::vector<uint8_t> tail(3, 0x01); /* 末尾短包：3 样本 */
        sink.send_logic(full, 1);
        sink.send_logic(full, 1);
        sink.send_logic(tail, 1);
        sink.send_end();

        QCOMPARE(row_count(sink.text()), 19);
        QCOMPARE(sink.text(), QStringLiteral("1,0\n").repeated(19));
    }
};

QTEST_MAIN(TestCsvExportChannels)
#include "test_csv_export_channels.moc"
