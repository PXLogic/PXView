/*
 * test_sr_options.cpp — 导入/导出模块选项与导出格式白名单回归。
 *
 * 背景（2026-09-24 用户报告：Demo device-LA-260924-213312.srzip 为 0KB）：
 *
 *   1) StoreSession::getSuportedExportFormats() 原先把 sr_output_list() 里的
 *      **所有**模块都列进导出格式下拉框，其中就有 srzip。srzip 是"会话归档"
 *      模块：它自己创建/替换目标文件（libzip），而 zip_create() 开头是
 *      g_unlink() + zip_open(ZIP_CREATE)；调用方（StoreSession::export_exec()）
 *      此时已经用 QFile::open(WriteOnly) 持有同一路径的句柄（不含
 *      FILE_SHARE_DELETE），于是 unlink 必然 EACCES、zip_open 返回
 *      ZIP_ER_NOZIP → 每个数据包都失败。而 sr_output_send() 的返回值当时被
 *      丢弃 → 导出"成功"、文件停在 QFile 截断出的 0 字节、零日志零提示。
 *
 *   2) 导入 binary 这类无头部格式时，通道数/采样率以前只能靠"当前设备的逻辑
 *      通道数"猜（猜不到就 8），用户无法指定。上游 PulseView 的做法是弹一个
 *      由模块自己声明的选项（numchannels / samplerate）生成的通用对话框。
 *
 * 本文件锁定的契约：
 *   A) 导出白名单：枚举真实的 sr_output_list()，srzip/null 必须在列表里但被
 *      白名单拒绝（旧代码"枚举全部"必然把它们列出来 —— 这就是本 bug 的入口），
 *      且被拒绝的只能是那几个有据可查的非流式模块；csv/binary/vcd 等仍可用；
 *      非逻辑数据只允许 csv。
 *   B) 选项值必须是模块声明的**同型** GVariant：sr_input_new() 对混型值是整单
 *      拒绝的（"Invalid type for '...' option."），所以 make_variant() 不匹配时
 *      必须返回 nullptr 而不是硬转；make_option_table() 必须丢掉未知键/错型值。
 *   C) 端到端：binary 输入模块 + {numchannels=16, samplerate=1MHz} → 实例的通道
 *      数必须是 16（模块自身默认是 8，即用户线上看到的错误值）。
 *
 * 依赖：只走 libsigrok 公开 API（sr_input_list / sr_input_find /
 * sr_input_options_get / sr_input_options_free / sr_input_new / sr_input_send /
 * sr_input_dev_inst_get / sr_input_free / sr_output_list / sr_output_id_get /
 * sr_output_find）。注意 struct sr_input / sr_dev_inst 在公开头里只有前向声明，
 * 所以通道只能经 sr_dev_inst_channels_get() 取，sdi 只能经
 * sr_input_dev_inst_get() 取。
 *
 * 链接方式与 test_csv_export_channels 相同：普通链接 libsigrok（不用
 * -Wl,--whole-archive —— 本机 binutils 在 whole-archive + 未解析符号时会静默
 * 失败）。
 */

#include <QtTest/QtTest>

#include <glib.h>
#include <libsigrok/libsigrok.h>

#include <QString>
#include <QVariantMap>

#include <cstring>
#include <map>
#include <string>

#include "pv/data/sr_options.h"

namespace sr_options = pv::data::sr_options;

namespace {

/* 造一个 binary 输入实例、喂几个字节让它把 sdi 标记为 ready（binary.c 是在
 * receive() 里置 sdi_ready 的，init() 只建通道），返回它创建的逻辑通道数；
 * 实例创建失败返回 -1。sdi 归输入实例所有，sr_input_free() 会一并释放。 */
int binary_channel_count(const struct sr_input_module *module, GHashTable *options)
{
    const struct sr_input *input = sr_input_new(module, options);
    if (!input)
        return -1;

    GString *chunk = g_string_new_len("\x01\x02\x03\x04", 4);
    const int send_ret = sr_input_send(input, chunk);
    g_string_free(chunk, TRUE);

    int count = -1;
    if (send_ret == SR_OK) {
        struct sr_dev_inst *sdi = sr_input_dev_inst_get(input);
        if (sdi)
            count = static_cast<int>(g_slist_length(sr_dev_inst_channels_get(sdi)));
    }

    sr_input_free(input);
    return count;
}

/* 找 binary 的某个选项声明（不拥有返回值，随 OptionsHandle 一起释放）。 */
const struct sr_option *binary_option(const struct sr_option **options, const char *id)
{
    for (int i = 0; options && options[i]; i++) {
        if (options[i]->id && std::strcmp(options[i]->id, id) == 0)
            return options[i];
    }
    return nullptr;
}

} // namespace

class TestSrOptions : public QObject
{
    Q_OBJECT

private slots:
    /* A) 白名单 vs 真实模块列表：srzip 在列表里，但不会被导出处列出来。 */
    void exportWhiteListFiltersOutNonStreamModules();

    /* A) 非逻辑数据（DSO/ANALOG/MSO）只允许 csv，与旧行为一致。 */
    void exportWhiteListKeepsOnlyCsvForNonLogicData();

    /* B) make_variant() 按选项声明的 GVariant 类型产出，不做隐式转换。 */
    void makeVariantFollowsDeclaredType();

    /* B) 不匹配的 Qt 值一律返回 nullptr（否则 sr_input_new 会整单拒绝）。 */
    void makeVariantRejectsMismatch();

    /* B) make_option_table() 丢掉未知键与错型值，且结果能被 sr_input_new 接受。 */
    void optionTableDropsUnknownKeysAndWrongTypes();

    /* 文档化 sr_input_new() 的两条硬约束（这是上面过滤存在的原因）。 */
    void srInputNewRejectsUnknownKeysAndIllTypedValues();

    /* D) 控件侧的值统一是 int32/int64（所有整数选项共用 Int 控件），必须按模块
     *    声明的类型转换回去，否则 csv 会因为 uint32 选项收到 int32 而拒绝整次
     *    导入——线上日志里的 "Invalid type for 'single_column' option."。 */
    void widgetValuesAreCoercedToDeclaredTypes();
    void csvImportAcceptsWidgetTypedValues();

    /* C) binary 导入的通道数来自用户给的选项，不再是写死的 8。 */
    void binaryImportUsesUserChannelCount();

    /* A) 历史持久化的导出格式必须被"归一化"到白名单内的格式。 */
    void exportFormatHistoryIsSanitized();

private:
    static const struct sr_input_module *binaryModule()
    {
        return sr_input_find("binary");
    }
};

void TestSrOptions::exportWhiteListFiltersOutNonStreamModules()
{
    bool saw_srzip = false;
    bool saw_null = false;
    bool saw_csv = false;
    bool saw_binary = false;

    const struct sr_output_module **modules = sr_output_list();
    QVERIFY(modules != nullptr);

    for (int i = 0; modules[i]; i++) {
        const char *id = sr_output_id_get(modules[i]);
        if (!id)
            continue;

        if (std::strcmp(id, "srzip") == 0) saw_srzip = true;
        if (std::strcmp(id, "null") == 0) saw_null = true;
        if (std::strcmp(id, "csv") == 0) saw_csv = true;
        if (std::strcmp(id, "binary") == 0) saw_binary = true;

        // Everything the filter rejects must have a documented reason; a new
        // libsigrok module must not be dropped silently.
        const bool offered = sr_options::is_export_module_supported(id, true);
        const bool documented_reject =
            std::strcmp(id, "srzip") == 0 || std::strcmp(id, "null") == 0 ||
            std::strcmp(id, "wav") == 0 || std::strcmp(id, "analog") == 0;
        QVERIFY2(offered || documented_reject,
                 qPrintable(QString("undocumented export filter change for '%1'")
                                .arg(QString::fromUtf8(id))));
    }

    // The modules that exist and are the whole point of this filter.
    QVERIFY2(saw_srzip, "srzip output module disappeared from libsigrok");
    QVERIFY(saw_null);
    QVERIFY(saw_csv);
    QVERIFY(saw_binary);

    // ... and are not offered (previously they were, producing 0-byte files).
    QVERIFY(!sr_options::is_export_module_supported("srzip", true));
    QVERIFY(!sr_options::is_export_module_supported("null", true));
    QVERIFY(!sr_options::is_export_module_supported(nullptr, true));

    QVERIFY(sr_options::is_export_module_supported("csv", true));
    QVERIFY(sr_options::is_export_module_supported("binary", true));
    QVERIFY(sr_options::is_export_module_supported("vcd", true));
    QVERIFY(sr_options::is_export_module_supported("wavedrom", true));
    // Module ids are not file names: chronovu_la8.c registers "chronovu-la8".
    // (This assertion caught exactly that typo while the change was written.)
    QVERIFY(sr_options::is_export_module_supported("chronovu-la8", true));
}

void TestSrOptions::exportWhiteListKeepsOnlyCsvForNonLogicData()
{
    QVERIFY(sr_options::is_export_module_supported("csv", false));
    QVERIFY(!sr_options::is_export_module_supported("binary", false));
    QVERIFY(!sr_options::is_export_module_supported("vcd", false));
    QVERIFY(!sr_options::is_export_module_supported("srzip", false));
}

void TestSrOptions::makeVariantFollowsDeclaredType()
{
    const struct sr_input_module *module = binaryModule();
    QVERIFY(module != nullptr);

    sr_options::OptionsHandle handle = sr_options::OptionsHandle::for_input(module);
    QVERIFY(!handle.empty());

    const struct sr_option *num_channels = binary_option(handle.options(), "numchannels");
    const struct sr_option *sample_rate = binary_option(handle.options(), "samplerate");
    QVERIFY(num_channels != nullptr);
    QVERIFY(sample_rate != nullptr);

    // The module's own defaults: 8 channels @ 0 Hz — the value the user used to
    // be stuck with.
    QCOMPARE(g_variant_get_int32(num_channels->def), 8);
    QCOMPARE(g_variant_get_uint64(sample_rate->def), static_cast<guint64>(0));

    GVariant *channels = sr_options::make_variant(num_channels, QVariant(16));
    QVERIFY(channels != nullptr);
    QVERIFY(g_variant_is_of_type(channels, G_VARIANT_TYPE_INT32));
    QVERIFY(!g_variant_is_floating(channels));  // must be sunk for the hash table
    QCOMPARE(g_variant_get_int32(channels), 16);

    GVariant *rate = sr_options::make_variant(
        sample_rate, QVariant::fromValue<qulonglong>(1000000));
    QVERIFY(rate != nullptr);
    QVERIFY(g_variant_is_of_type(rate, G_VARIANT_TYPE_UINT64));
    QCOMPARE(g_variant_get_uint64(rate), static_cast<guint64>(1000000));

    g_variant_unref(channels);
    g_variant_unref(rate);
    handle.reset();
}

void TestSrOptions::makeVariantRejectsMismatch()
{
    const struct sr_input_module *module = binaryModule();
    QVERIFY(module != nullptr);

    sr_options::OptionsHandle handle = sr_options::OptionsHandle::for_input(module);
    QVERIFY(!handle.empty());

    const struct sr_option *num_channels = binary_option(handle.options(), "numchannels");
    const struct sr_option *sample_rate = binary_option(handle.options(), "samplerate");
    QVERIFY(num_channels != nullptr);
    QVERIFY(sample_rate != nullptr);

    // Wrong Qt type for an int32 option.
    QVERIFY(sr_options::make_variant(num_channels, QVariant(QStringLiteral("sixteen"))) == nullptr);
    QVERIFY(sr_options::make_variant(num_channels, QVariant()) == nullptr);

    // Out of the declared range: must not wrap around into a valid int32.
    QVERIFY(sr_options::make_variant(
                num_channels, QVariant::fromValue<qlonglong>(Q_INT64_C(1) << 40)) == nullptr);

    // Same for a uint64 option given something that is not a number.
    QVERIFY(sr_options::make_variant(sample_rate, QVariant(QStringLiteral("-5"))) == nullptr);

    // Only the *type* is policed here: a value that fits int32 but is
    // semantically impossible (0 channels) still becomes a valid int32 variant —
    // binary.c's init() is the one that rejects it ("must be at least 1").
    GVariant *zero = sr_options::make_variant(num_channels, QVariant(0));
    QVERIFY(zero != nullptr);
    QCOMPARE(g_variant_get_int32(zero), 0);
    g_variant_unref(zero);

    handle.reset();
}

void TestSrOptions::optionTableDropsUnknownKeysAndWrongTypes()
{
    const struct sr_input_module *module = binaryModule();
    QVERIFY(module != nullptr);

    sr_options::OptionsHandle handle = sr_options::OptionsHandle::for_input(module);
    QVERIFY(!handle.empty());

    QVariantMap prefill;
    prefill.insert(QStringLiteral("numchannels"), 16);
    prefill.insert(QStringLiteral("samplerate"), QVariant::fromValue<qulonglong>(2000000));
    prefill.insert(QStringLiteral("does_not_exist"), 1);           // unknown key
    QVariantMap wrong_type;
    wrong_type.insert(QStringLiteral("numchannels"), QStringLiteral("nope"));

    GHashTable *table = sr_options::make_option_table(handle.options(), prefill);
    QVERIFY(table != nullptr);
    QVERIFY(g_hash_table_size(table) == 2);  // unknown key dropped

    // A rejected prefill must leave the table empty rather than mistyped.
    GHashTable *rejected = sr_options::make_option_table(handle.options(), wrong_type);
    QVERIFY(rejected != nullptr);
    QCOMPARE(g_hash_table_size(rejected), guint(0));

    handle.reset();

    // Both tables must be acceptable as-is for the module (no unknown keys).
    QCOMPARE(binary_channel_count(module, table), 16);
    QCOMPARE(binary_channel_count(module, rejected), 8);  // module default

    g_hash_table_destroy(table);
    g_hash_table_destroy(rejected);
}

void TestSrOptions::srInputNewRejectsUnknownKeysAndIllTypedValues()
{
    const struct sr_input_module *module = binaryModule();
    QVERIFY(module != nullptr);

    // Unknown option id -> the whole import is refused.
    GHashTable *unknown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                (GDestroyNotify)g_variant_unref);
    g_hash_table_insert(unknown, g_strdup("not_an_option"),
                        g_variant_ref_sink(g_variant_new_int32(1)));
    QVERIFY(sr_input_new(module, unknown) == nullptr);
    g_hash_table_destroy(unknown);

    // Mistyped value for a declared option -> also refused. This is why
    // make_variant() must be type-exact instead of coercing.
    GHashTable *mistyped = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                 (GDestroyNotify)g_variant_unref);
    g_hash_table_insert(mistyped, g_strdup("samplerate"),
                        g_variant_ref_sink(g_variant_new_int32(1000)));
    QVERIFY(sr_input_new(module, mistyped) == nullptr);
    g_hash_table_destroy(mistyped);
}

void TestSrOptions::widgetValuesAreCoercedToDeclaredTypes()
{
    using sr_options::coerce_variant;

    // int32（Int 控件的产物）→ uint32（csv 的 single_column / logic_channels…）
    GVariant *value = g_variant_ref_sink(g_variant_new_int32(16));
    GVariant *converted = coerce_variant(value, G_VARIANT_TYPE_UINT32);
    QVERIFY(converted != nullptr);
    QCOMPARE(QString::fromUtf8(g_variant_get_type_string(converted)), QStringLiteral("u"));
    QCOMPARE(g_variant_get_uint32(converted), static_cast<guint32>(16));
    g_variant_unref(converted);

    // 跨族拒绝：字符串不能变数字（否则会把 "bin" 静默当成 0）。
    QVERIFY(coerce_variant(value, G_VARIANT_TYPE_STRING) == nullptr);
    g_variant_unref(value);

    // int64 → uint64（samplerate 的常见组合：Int 控件核 64 位时给 int64）。
    value = g_variant_ref_sink(g_variant_new_int64(1000000));
    converted = coerce_variant(value, G_VARIANT_TYPE_UINT64);
    QVERIFY(converted != nullptr);
    QCOMPARE(QString::fromUtf8(g_variant_get_type_string(converted)), QStringLiteral("t"));
    QCOMPARE(g_variant_get_uint64(converted), static_cast<guint64>(1000000));
    g_variant_unref(converted);

    // 越界拒绝：负数进无符号、超范围进窄类型。
    GVariant *negative = g_variant_ref_sink(g_variant_new_int32(-1));
    QVERIFY(coerce_variant(negative, G_VARIANT_TYPE_UINT32) == nullptr);
    QVERIFY(coerce_variant(negative, G_VARIANT_TYPE_UINT64) == nullptr);
    g_variant_unref(negative);

    GVariant *wide = g_variant_ref_sink(g_variant_new_int64(70000));
    QVERIFY(coerce_variant(wide, G_VARIANT_TYPE_UINT16) == nullptr);
    converted = coerce_variant(wide, G_VARIANT_TYPE_UINT32);
    QVERIFY(converted != nullptr);
    QCOMPARE(g_variant_get_uint32(converted), static_cast<guint32>(70000));
    g_variant_unref(converted);
    g_variant_unref(wide);

    // 同型直接可用（字符串/布尔/枚举值永远走这条路）。
    GVariant *text = g_variant_ref_sink(g_variant_new_string("bin"));
    converted = coerce_variant(text, G_VARIANT_TYPE_STRING);
    QVERIFY(converted != nullptr);
    QCOMPARE(QString::fromUtf8(g_variant_get_string(converted, nullptr)),
             QStringLiteral("bin"));
    g_variant_unref(converted);
    g_variant_unref(text);
}

void TestSrOptions::csvImportAcceptsWidgetTypedValues()
{
    const struct sr_input_module *module = sr_input_find("csv");
    QVERIFY(module != nullptr);

    sr_options::OptionsHandle handle = sr_options::OptionsHandle::for_input(module);
    QVERIFY(!handle.empty());

    // 对话框里点“确定”时控件交出的值：所有整数选项都用同一个 Int 控件，因此
    // 一律是 int32/int64，而 csv 声明的是 uint32/uint64。这就是线上那份日志里
    // "sr: input: Invalid type for 'single_column' option." 的来源。
    std::map<std::string, GVariant *> widget_values;
    widget_values.emplace("single_column", g_variant_ref_sink(g_variant_new_int32(0)));
    widget_values.emplace("first_column", g_variant_ref_sink(g_variant_new_int32(1)));
    widget_values.emplace("logic_channels", g_variant_ref_sink(g_variant_new_int32(2)));
    widget_values.emplace("start_line", g_variant_ref_sink(g_variant_new_int32(1)));
    widget_values.emplace("samplerate", g_variant_ref_sink(g_variant_new_int64(1000000)));

    // 复现：原样交给 libsigrok，整次导入被拒。
    GHashTable *raw = sr_options::make_option_table(widget_values);
    QVERIFY(sr_input_new(module, raw) == nullptr);
    g_hash_table_destroy(raw);

    // 按模块声明转换后：类型正确、导入成立，而且值真的生效。
    std::map<std::string, GVariantType *> declared;
    for (int i = 0; handle.options()[i]; i++) {
        const struct sr_option *const option = handle.options()[i];
        if (option->id && option->def) {
            declared.emplace(option->id,
                             g_variant_type_copy(g_variant_get_type(option->def)));
        }
    }

    GHashTable *coerced = sr_options::make_option_table(declared, widget_values);

    auto *single_column =
        static_cast<GVariant *>(g_hash_table_lookup(coerced, "single_column"));
    QVERIFY(single_column != nullptr);
    QCOMPARE(QString::fromUtf8(g_variant_get_type_string(single_column)),
             QStringLiteral("u"));

    auto *samplerate =
        static_cast<GVariant *>(g_hash_table_lookup(coerced, "samplerate"));
    QVERIFY(samplerate != nullptr);
    QCOMPARE(QString::fromUtf8(g_variant_get_type_string(samplerate)),
             QStringLiteral("t"));

    const struct sr_input *input = sr_input_new(module, coerced);
    QVERIFY(input != nullptr);

    // 两列逻辑数据 + logic_channels=2 → 实例必须恰好建出 2 个逻辑通道（值生效，
    // 而不仅仅是类型被接受）。
    GString *chunk = g_string_new("0,1\n1,0\n");
    QCOMPARE(sr_input_send(input, chunk), SR_OK);
    g_string_free(chunk, TRUE);

    struct sr_dev_inst *sdi = sr_input_dev_inst_get(input);
    QVERIFY(sdi != nullptr);
    QCOMPARE(g_slist_length(sr_dev_inst_channels_get(sdi)), 2u);

    sr_input_free(input);
    g_hash_table_destroy(coerced);

    for (auto &entry : declared)
        g_variant_type_free(entry.second);
    for (auto &entry : widget_values)
        g_variant_unref(entry.second);
}

void TestSrOptions::binaryImportUsesUserChannelCount()
{
    const struct sr_input_module *module = binaryModule();
    QVERIFY(module != nullptr);

    // Baseline: with no options at all the module's own default (8) applies.
    QCOMPARE(binary_channel_count(module, nullptr), 8);

    // The dialog's output must actually reach sr_input_new().
    sr_options::OptionsHandle handle = sr_options::OptionsHandle::for_input(module);
    QVERIFY(!handle.empty());

    QVariantMap prefill;
    prefill.insert(QStringLiteral("numchannels"), 16);
    prefill.insert(QStringLiteral("samplerate"), QVariant::fromValue<qulonglong>(1000000));

    GHashTable *table = sr_options::make_option_table(handle.options(), prefill);
    handle.reset();  // the module's option array is gone; the table must not care

    QCOMPARE(binary_channel_count(module, table), 16);

    g_hash_table_destroy(table);
}

void TestSrOptions::exportFormatHistoryIsSanitized()
{
    using sr_options::export_module_id_from_filter;
    using sr_options::normalize_export_module_id;

    // Every form the export path stores or produces must map back to a module id
    // (the dialog's filter entry, the bare id, the bare extension).
    QCOMPARE(export_module_id_from_filter(QStringLiteral(".csv")), QStringLiteral("csv"));
    QCOMPARE(export_module_id_from_filter(QStringLiteral("csv")), QStringLiteral("csv"));
    QCOMPARE(export_module_id_from_filter(QStringLiteral("csv (*.csv)")), QStringLiteral("csv"));
    QCOMPARE(export_module_id_from_filter(QStringLiteral("VCD (*.vcd)")), QStringLiteral("vcd"));
    QCOMPARE(export_module_id_from_filter(QStringLiteral(".srzip")), QStringLiteral("srzip"));
    QCOMPARE(export_module_id_from_filter(QString()), QString());

    // Offered formats survive, whatever form they were stored in.
    QCOMPARE(normalize_export_module_id(QStringLiteral(".csv"), true), QStringLiteral("csv"));
    QCOMPARE(normalize_export_module_id(QStringLiteral("binary (*.binary)"), true),
             QStringLiteral("binary"));
    QCOMPARE(normalize_export_module_id(QStringLiteral(".vcd"), true), QStringLiteral("vcd"));

    // Round-trip property over the real module list: PXView builds every filter
    // entry as "<description> (*.<module id>)" and that very string is stored in
    // AppConfig::userHistory.exportFormat and fed back in as the export suffix,
    // so it must parse back to exactly the same module id — a mismatch would
    // silently export in another format.
    for (const struct sr_output_module **m = sr_output_list(); m && *m; m++) {
        const char *mod_id = sr_output_id_get(*m);
        const char *mod_desc = sr_output_description_get(*m);
        if (!mod_id)
            continue;

        const QString entry = QStringLiteral("%1 (*.%2)")
                                  .arg(QString::fromUtf8(mod_desc ? mod_desc : mod_id),
                                       QString::fromUtf8(mod_id));
        QCOMPARE(export_module_id_from_filter(entry), QString::fromUtf8(mod_id));
    }

    // A format persisted before the whitelist existed MUST be repaired instead of
    // used: this is the ".srzip in AppConfig::userHistory.exportFormat" case that
    // made the next Export (which skips the dialog and reuses that value) fail
    // with "Export module 'srzip' failed while handling a logic data packet"
    // instead of exporting anything.
    // The exact string this bug left behind in
    // HKCU\Software\PXlogicV20\PXView\History\exportFormat -- Qt stores the whole
    // filter entry, description included (no '.' or ')' in it, so parsing the
    // last ".<id>)" still yields the module id).
    QCOMPARE(export_module_id_from_filter(
                 QStringLiteral("srzip session file format data (*.srzip)")),
             QStringLiteral("srzip"));
    QCOMPARE(normalize_export_module_id(
                 QStringLiteral("srzip session file format data (*.srzip)"), true),
             QStringLiteral("csv"));

    QCOMPARE(normalize_export_module_id(QStringLiteral(".srzip"), true), QStringLiteral("csv"));
    QCOMPARE(normalize_export_module_id(QStringLiteral("srzip (*.srzip)"), true), QStringLiteral("csv"));
    QCOMPARE(normalize_export_module_id(QStringLiteral(".null"), true), QStringLiteral("csv"));
    QCOMPARE(normalize_export_module_id(QStringLiteral(".wav"), true), QStringLiteral("csv"));
    QCOMPARE(normalize_export_module_id(QString(), true), QStringLiteral("csv"));

    // Non-logic data only ever exports as csv, so even an offered logic format
    // is not honoured there.
    QCOMPARE(normalize_export_module_id(QStringLiteral(".binary"), false), QStringLiteral("csv"));
    QCOMPARE(normalize_export_module_id(QStringLiteral(".vcd"), false), QStringLiteral("csv"));
}

QTEST_MAIN(TestSrOptions)
#include "test_sr_options.moc"
