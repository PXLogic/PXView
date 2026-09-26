/*
 * test_srzip_load.cpp — 「打开 .pxl / .sr / .srzip」这条入口的可读性回归。
 *
 * 背景（2026-09-24）：文件 → 打开 的过滤串过去只给 *.pxl，用户拿不到 sigrok
 * 会话归档（*.sr / *.srzip）。而这条入口背后的加载器
 * SigSession::set_file() → sr_session_load_file_device() 本来就同时支持两种
 * 容器（session_file.c：upstream 的 version/metadata + logic-N/data-N 与 PXView
 * 自己的 header + L-<ch>/<n>），所以缺的只是对话框里的后缀。
 *
 * 本文件锁定的契约：把 srzip 输出模块（libsigrok 里唯一的
 * SR_OUTPUT_INTERNAL_IO_HANDLING 模块 —— 它自己创建/替换目标文件，调用方不得
 * 预先 open 该路径）写出的归档，用 PXView 打开路径**实际调用的同一个 API**
 * （sr_session_load_file_device）读回来，通道数与采样率必须与写入时一致。
 *
 * 这样任何让 .sr/.srzip 变成"能选但读不了"的改动都会在这里失败，而不是等到
 * 用户点开文件才发现是空白 tab。
 *
 * 依赖：只走 libsigrok 公开 API（sr_init/sr_exit、sr_dev_inst_user_new、
 * sr_dev_inst_channel_add、sr_dev_channel_enable、sr_output_find/sr_output_new/
 * sr_output_send/sr_output_free、sr_session_load_file_device、
 * sr_dev_inst_driver_get/sr_config_get、sr_dev_inst_channels_get、
 * sr_dev_inst_free）。链接方式同 test_csv_export_channels：普通链接 libsigrok。
 */

#include <QtTest/QtTest>

#include <QFile>
#include <QFileInfo>

#include <glib.h>
#include <libsigrok/libsigrok.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr int kChannels = 4;
constexpr uint64_t kSamplerate = 1000000;
constexpr size_t kSamples = 256;  // 4 通道 → unitsize 1 字节/样本

/* 假设备：只走公开 API（struct sr_dev_inst 在公开头里是不完整类型，
 * 通道只能经 sr_dev_inst_channels_get() 取）。 */
class FakeDevice
{
public:
    FakeDevice()
    {
        _sdi = sr_dev_inst_user_new("qtest", "srzip-load", "1.0");
        for (int i = 0; i < kChannels; i++) {
            const std::string name = std::to_string(i);
            sr_dev_inst_channel_add(_sdi, i, SR_CHANNEL_LOGIC, name.c_str());
        }
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

/* 用 srzip 输出模块写一个会话归档。该模块自己创建/替换文件（这正是
 * SR_OUTPUT_INTERNAL_IO_HANDLING 的含义），所以这里**不能**先 open 目标路径。 */
class SrzipWriter
{
public:
    SrzipWriter(struct sr_dev_inst *sdi, const char *path)
    {
        const struct sr_output_module *module =
            sr_output_find(const_cast<char *>("srzip"));
        if (!module)
            return;

        GHashTable *opts = g_hash_table_new(g_str_hash, g_str_equal);
        _out = sr_output_new(module, opts, sdi, path);
        g_hash_table_destroy(opts);
    }

    ~SrzipWriter()
    {
        if (_out)
            sr_output_free(_out);
    }

    SrzipWriter(const SrzipWriter &) = delete;
    SrzipWriter &operator=(const SrzipWriter &) = delete;

    bool valid() const { return _out != nullptr; }

    /* META(samplerate) + 一个 LOGIC 包 + END（srzip 在 END 上 flush）。 */
    bool write(uint64_t samplerate, const std::vector<uint8_t> &samples)
    {
        if (!_out)
            return false;

        GString *out = nullptr;

        struct sr_config cfg;
        cfg.key = SR_CONF_SAMPLERATE;
        cfg.data = g_variant_ref_sink(g_variant_new_uint64(samplerate));

        struct sr_datafeed_meta meta;
        meta.config = g_slist_append(nullptr, &cfg);
        struct sr_datafeed_packet packet;
        packet.type = SR_DF_META;
        packet.payload = &meta;

        bool ok = sr_output_send(_out, &packet, &out) == SR_OK;
        if (out) {
            g_string_free(out, TRUE);
            out = nullptr;
        }
        g_slist_free(meta.config);
        g_variant_unref(cfg.data);

        if (!ok)
            return false;

        struct sr_datafeed_logic logic;
        logic.data = const_cast<uint8_t *>(samples.data());
        logic.length = samples.size();
        logic.unitsize = static_cast<uint16_t>((kChannels + 7) / 8);
        packet.type = SR_DF_LOGIC;
        packet.payload = &logic;

        ok = sr_output_send(_out, &packet, &out) == SR_OK;
        if (out) {
            g_string_free(out, TRUE);
            out = nullptr;
        }

        if (!ok)
            return false;

        packet.type = SR_DF_END;
        packet.payload = nullptr;
        ok = sr_output_send(_out, &packet, &out) == SR_OK;
        if (out)
            g_string_free(out, TRUE);

        return ok;
    }

private:
    const struct sr_output *_out = nullptr;
};

} // namespace

class TestSrzipLoad : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    /* srzip 归档 → sr_session_load_file_device() 必须读回同样的通道数/采样率。 */
    void srzipArchiveLoadsThroughSessionLoader();

private:
    struct sr_context *_ctx = nullptr;
    std::string _path;
};

void TestSrzipLoad::initTestCase()
{
    QVERIFY2(sr_init(&_ctx) == SR_OK, "sr_init failed");

    // Keep the scratch archive next to the test binary's CWD.
    _path = "qtest_srzip_load.sr";
    QFile::remove(QString::fromStdString(_path));
}

void TestSrzipLoad::cleanupTestCase()
{
    QFile::remove(QString::fromStdString(_path));
    if (_ctx)
        sr_exit(_ctx);
}

void TestSrzipLoad::srzipArchiveLoadsThroughSessionLoader()
{
    // The module itself must be the one and only INTERNAL_IO_HANDLING module —
    // that flag is why StoreSession's export pipeline can never drive it (it
    // opens the target file itself), and why the archive has to be produced by
    // the module here without us touching the path first.
    const struct sr_output_module *module = sr_output_find(const_cast<char *>("srzip"));
    QVERIFY(module != nullptr);
    QVERIFY(sr_output_test_flag(module, SR_OUTPUT_INTERNAL_IO_HANDLING));

    FakeDevice device;
    QVERIFY(device.inst() != nullptr);

    std::vector<uint8_t> samples(kSamples);
    for (size_t i = 0; i < samples.size(); i++)
        samples[i] = static_cast<uint8_t>((i * 7) & 0x0f);

    SrzipWriter writer(device.inst(), _path.c_str());
    QVERIFY(writer.valid());
    QVERIFY(writer.write(kSamplerate, samples));

    // The module writes the file itself: it must exist and not be empty.
    QFileInfo info(QString::fromStdString(_path));
    QVERIFY(info.exists());
    QVERIFY(info.size() > 0);

    // Exactly what SigSession::set_file() calls for the Open entry.
    struct sr_dev_inst *loaded = sr_session_load_file_device(_ctx, _path.c_str());
    QVERIFY2(loaded != nullptr, "sr_session_load_file_device could not read the srzip archive");

    QCOMPARE(g_slist_length(sr_dev_inst_channels_get(loaded)), guint(kChannels));

    GVariant *gvar = nullptr;
    struct sr_dev_driver *driver = sr_dev_inst_driver_get(loaded);
    QVERIFY(driver != nullptr);
    QCOMPARE(sr_config_get(driver, loaded, NULL, SR_CONF_SAMPLERATE, &gvar), SR_OK);
    QVERIFY(gvar != nullptr);
    QCOMPARE(g_variant_get_uint64(gvar), kSamplerate);
    g_variant_unref(gvar);

    // srzip metadata has no "total samples" key (upstream sigrok format), so
    // sr_session_load must compute the sample count from the capture data
    // size (256 samples / unitsize 1). Without this the device reports
    // limit_samples=0 and the frontend falls back to its default sample
    // limit (1M), truncating larger captures (e.g. 5s → 1s).
    GVariant *limit = nullptr;
    QCOMPARE(sr_config_get(driver, loaded, NULL, SR_CONF_LIMIT_SAMPLES, &limit), SR_OK);
    QVERIFY(limit != nullptr);
    QCOMPARE(g_variant_get_uint64(limit), static_cast<uint64_t>(kSamples));
    g_variant_unref(limit);

    sr_dev_inst_free(loaded);
}

QTEST_MAIN(TestSrzipLoad)
#include "test_srzip_load.moc"
