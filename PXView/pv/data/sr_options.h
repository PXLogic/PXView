/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2015 Joel Holdsworth <joel@airwebreathe.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef PXVIEW_PV_DATA_SR_OPTIONS_H
#define PXVIEW_PV_DATA_SR_OPTIONS_H

#include <glib.h>
#include <libsigrok/libsigrok.h>

#include <QString>
#include <QVariant>
#include <QVariantMap>

#include <map>
#include <string>

namespace pv {
namespace data {
namespace sr_options {

/**
 * RAII owner of the NULL-terminated option array returned by
 * sr_input_options_get() / sr_output_options_get().
 *
 * libsigrok hands out pointers into the module's *static* option table, and the
 * matching *_options_free() unrefs each entry's `def` / `values` and NULLs them
 * (every input/output module re-creates its defaults lazily on the next call,
 * so get/free may be repeated). Consequently a caller must
 *
 *   * never unref `opt->def` / `opt->values` itself, and
 *   * always release the array through this guard.
 *
 * The array itself is only needed while the option values are read out; the
 * widgets keep their own references afterwards.
 */
class OptionsHandle
{
public:
    OptionsHandle() = default;
    OptionsHandle(const struct sr_option **options, bool is_input)
        : _options(options), _is_input(is_input) {}

    ~OptionsHandle() { reset(); }

    OptionsHandle(const OptionsHandle &) = delete;
    OptionsHandle &operator=(const OptionsHandle &) = delete;

    OptionsHandle(OptionsHandle &&other) noexcept
        : _options(other._options), _is_input(other._is_input)
    {
        other._options = nullptr;
    }

    OptionsHandle &operator=(OptionsHandle &&other) noexcept
    {
        if (this != &other) {
            reset();
            _options = other._options;
            _is_input = other._is_input;
            other._options = nullptr;
        }
        return *this;
    }

    static OptionsHandle for_input(const struct sr_input_module *module)
    {
        if (!module)
            return OptionsHandle();
        return OptionsHandle(sr_input_options_get(module), true);
    }

    static OptionsHandle for_output(const struct sr_output_module *module)
    {
        if (!module)
            return OptionsHandle();
        return OptionsHandle(sr_output_options_get(module), false);
    }

    const struct sr_option **options() const { return _options; }
    bool empty() const { return _options == nullptr; }

    void reset()
    {
        if (!_options)
            return;
        if (_is_input)
            sr_input_options_free(_options);
        else
            sr_output_options_free(_options);
        _options = nullptr;
    }

private:
    const struct sr_option **_options = nullptr;
    bool _is_input = true;
};

/**
 * Whether a module id names an integer-only option type supported by
 * make_variant() below.
 */
inline bool is_integer_type(const GVariantType *type)
{
    return type &&
        (g_variant_type_equal(type, G_VARIANT_TYPE_BYTE) ||
         g_variant_type_equal(type, G_VARIANT_TYPE_INT16) ||
         g_variant_type_equal(type, G_VARIANT_TYPE_UINT16) ||
         g_variant_type_equal(type, G_VARIANT_TYPE_INT32) ||
         g_variant_type_equal(type, G_VARIANT_TYPE_UINT32) ||
         g_variant_type_equal(type, G_VARIANT_TYPE_INT64) ||
         g_variant_type_equal(type, G_VARIANT_TYPE_UINT64));
}

/**
 * Convert a Qt-side value into the GVariant type the option declares.
 *
 * @return a sunk (non-floating) variant owning one reference, or nullptr when
 *         the option has no default value, the type is unsupported, or the
 *         value does not fit the declared type. Never returns a variant of a
 *         different type than `opt->def`: sr_input_new() rejects a mistyped
 *         value for the whole import ("Invalid type for '<id>' option."), so a
 *         stale or hand-written prefill must be dropped, not converted.
 */
inline GVariant *make_variant(const struct sr_option *option, const QVariant &value)
{
    if (!option || !option->def || !value.isValid())
        return nullptr;

    const GVariantType *const type = g_variant_get_type(option->def);
    if (!type)
        return nullptr;

    if (g_variant_type_equal(type, G_VARIANT_TYPE_BYTE)) {
        bool ok = false;
        const qlonglong v = value.toLongLong(&ok);
        if (!ok || v < 0 || v > G_MAXUINT8)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_byte(static_cast<guint8>(v)));
    }

    if (g_variant_type_equal(type, G_VARIANT_TYPE_INT16)) {
        bool ok = false;
        const qlonglong v = value.toLongLong(&ok);
        if (!ok || v < G_MININT16 || v > G_MAXINT16)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_int16(static_cast<gint16>(v)));
    }

    if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT16)) {
        bool ok = false;
        const qulonglong v = value.toULongLong(&ok);
        if (!ok || v > G_MAXUINT16)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_uint16(static_cast<guint16>(v)));
    }

    if (g_variant_type_equal(type, G_VARIANT_TYPE_INT32)) {
        bool ok = false;
        const qlonglong v = value.toLongLong(&ok);
        if (!ok || v < G_MININT32 || v > G_MAXINT32)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_int32(static_cast<gint32>(v)));
    }

    if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT32)) {
        bool ok = false;
        const qulonglong v = value.toULongLong(&ok);
        if (!ok || v > G_MAXUINT32)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_uint32(static_cast<guint32>(v)));
    }

    if (g_variant_type_equal(type, G_VARIANT_TYPE_INT64)) {
        bool ok = false;
        const qlonglong v = value.toLongLong(&ok);
        if (!ok)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_int64(static_cast<gint64>(v)));
    }

    if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT64)) {
        bool ok = false;
        const qulonglong v = value.toULongLong(&ok);
        if (!ok)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_uint64(static_cast<guint64>(v)));
    }

    if (g_variant_type_equal(type, G_VARIANT_TYPE_DOUBLE)) {
        bool ok = false;
        const double v = value.toDouble(&ok);
        if (!ok)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_double(v));
    }

    if (g_variant_type_equal(type, G_VARIANT_TYPE_BOOLEAN))
        return g_variant_ref_sink(g_variant_new_boolean(value.toBool() ? TRUE : FALSE));

    if (g_variant_type_equal(type, G_VARIANT_TYPE_STRING)) {
        const QByteArray utf8 = value.toString().toUtf8();
        return g_variant_ref_sink(g_variant_new_string(utf8.constData()));
    }

    return nullptr;
}

/**
 * Convert @p value into the GVariant type the module declared for that option.
 *
 * The property widgets are deliberately type-agnostic: every integer option is
 * edited by the same Int widget, which yields int32 (or int64 for the wide
 * cases), and Enum/String/Bool yield their own type. libsigrok however validates
 * the table strictly -- csv.c refuses the whole import with
 *
 *     sr: input: Invalid type for 'single_column' option.
 *
 * when its uint32 `single_column` arrives as int32. So the dialog's values must
 * be mapped onto the declaration before they reach sr_input_new(). This is that
 * mapping; it is also what makes an option's GVariant type a detail of the
 * module rather than of the widget.
 *
 * Only the numeric family is convertible, and only within that family: a string
 * never becomes a number (that would silently import nonsense). A value that
 * cannot be represented in @p declared returns nullptr, and the caller then
 * drops the option so the module keeps its own default.
 *
 * @return a sunk (non-floating) variant owning one reference, or nullptr.
 */
inline GVariant *coerce_variant(GVariant *value, const GVariantType *declared)
{
    if (!value || !declared || g_variant_is_floating(value))
        return nullptr;

    const GVariantType *const type = g_variant_get_type(value);
    if (!type)
        return nullptr;

    // Already exact (every value make_variant() produced, plus strings and
    // booleans that no widget widens): nothing to convert.
    if (g_variant_type_equal(type, declared))
        return g_variant_ref(value);

    double number = 0.0;
    if (g_variant_type_equal(type, G_VARIANT_TYPE_BYTE))
        number = g_variant_get_byte(value);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT16))
        number = g_variant_get_int16(value);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT16))
        number = g_variant_get_uint16(value);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT32))
        number = g_variant_get_int32(value);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT32))
        number = g_variant_get_uint32(value);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT64))
        number = static_cast<double>(g_variant_get_int64(value));
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT64))
        number = static_cast<double>(g_variant_get_uint64(value));
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_DOUBLE))
        number = g_variant_get_double(value);
    else
        return nullptr;  // string/bool/... : not numeric, no conversion

    // Option values are counts, rates and line numbers -- all far below 2^53,
    // so the double round-trip above is exact for every value in use.
    if (g_variant_type_equal(declared, G_VARIANT_TYPE_BYTE)) {
        if (number < 0 || number > G_MAXUINT8)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_byte(static_cast<guint8>(number)));
    }
    if (g_variant_type_equal(declared, G_VARIANT_TYPE_INT16)) {
        if (number < G_MININT16 || number > G_MAXINT16)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_int16(static_cast<gint16>(number)));
    }
    if (g_variant_type_equal(declared, G_VARIANT_TYPE_UINT16)) {
        if (number < 0 || number > G_MAXUINT16)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_uint16(static_cast<guint16>(number)));
    }
    if (g_variant_type_equal(declared, G_VARIANT_TYPE_INT32)) {
        if (number < G_MININT32 || number > G_MAXINT32)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_int32(static_cast<gint32>(number)));
    }
    if (g_variant_type_equal(declared, G_VARIANT_TYPE_UINT32)) {
        if (number < 0 || number > G_MAXUINT32)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_uint32(static_cast<guint32>(number)));
    }
    if (g_variant_type_equal(declared, G_VARIANT_TYPE_INT64))
        return g_variant_ref_sink(g_variant_new_int64(static_cast<gint64>(number)));
    if (g_variant_type_equal(declared, G_VARIANT_TYPE_UINT64)) {
        if (number < 0)
            return nullptr;
        return g_variant_ref_sink(g_variant_new_uint64(static_cast<guint64>(number)));
    }
    if (g_variant_type_equal(declared, G_VARIANT_TYPE_DOUBLE))
        return g_variant_ref_sink(g_variant_new_double(number));

    return nullptr;
}

/**
 * Build the option table handed to sr_input_new() / sr_output_new() from
 * already typed values (id -> variant).
 *
 * `values` must hold non-floating variants; the table takes its own reference
 * of each one, so it can be destroyed independently of the source map.
 */
inline GHashTable *make_option_table(const std::map<std::string, GVariant *> &values)
{
    GHashTable *const table = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);

    for (const auto &entry : values) {
        if (!entry.second || g_variant_is_floating(entry.second))
            continue;
        g_hash_table_insert(table, g_strdup(entry.first.c_str()),
                            g_variant_ref(entry.second));
    }

    return table;
}

/**
 * Build an option table from Qt-side values, filtered against the options the
 * module actually declares.
 *
 * sr_input_new() fails the entire import on an unknown key ("Input module 'x'
 * has no option 'y'") as well as on a mistyped value, so keys the module does
 * not know and values that do not fit the declared type are silently dropped
 * here -- the module then falls back to its own default for that option.
 * Returns an empty (but non-NULL) table when nothing matched, which is exactly
 * what sr_input_new() accepts to mean "use every default".
 */
inline GHashTable *make_option_table(const struct sr_option **options,
                                     const QVariantMap &values)
{
    GHashTable *const table = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);

    if (!options)
        return table;

    for (int i = 0; options[i]; i++) {
        const struct sr_option *const option = options[i];
        if (!option->id || !values.contains(QString::fromUtf8(option->id)))
            continue;

        GVariant *const value = make_variant(
            option, values.value(QString::fromUtf8(option->id)));
        if (!value)
            continue;

        // make_variant() returns a sunk reference: the table becomes its owner.
        g_hash_table_insert(table, g_strdup(option->id), value);
    }

    return table;
}

/**
 * Same as above, but for values that came out of a property widget: every entry
 * is first coerced to the option's declared type (see coerce_variant()) and
 * entries whose id the module does not declare are dropped.
 *
 * This is the variant the dialogs use. Properties cannot know the module's
 * declared type -- all integers share one Int widget -- so without this step a
 * uint32 option would arrive as int32 and sr_input_new() would reject the whole
 * import ("Invalid type for '<id>' option.").
 *
 * @param declared id -> declared type, copied via g_variant_type_copy() by the
 *                 caller and owned by it.
 */
inline GHashTable *make_option_table(
    const std::map<std::string, GVariantType *> &declared,
    const std::map<std::string, GVariant *> &values)
{
    GHashTable *const table = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);

    for (const auto &entry : values) {
        const auto type_it = declared.find(entry.first);
        if (type_it == declared.end() || !type_it->second)
            continue;  // unknown option id: sr_input_new() would refuse

        GVariant *const value = coerce_variant(entry.second, type_it->second);
        if (!value)
            continue;

        g_hash_table_insert(table, g_strdup(entry.first.c_str()), value);
    }

    return table;
}

/**
 * Whether PXView may offer a libsigrok output module in the data-export dialog.
 *
 * StoreSession::export_exec() hands the module a QFile it opened itself and
 * writes the module's returned GString into it -- a contract that silently
 * breaks for modules which do not produce their output that way:
 *
 *   * "srzip" -- a session *archive*. It creates/replaces the target file
 *     itself (libzip) and needs sr_session_save() semantics. Its
 *     g_unlink() + zip_open(ZIP_CREATE) cannot succeed while PXView holds the
 *     same path open (and the unlink fails on Windows without FILE_SHARE_DELETE),
 *     so every packet failed and the file stayed at the 0 bytes QFile::open()
 *     had truncated it to -- with no error message anywhere.
 *   * "null" -- a sink that discards everything: always an empty file.
 *   * "wav" / "analog" -- analog-only modules: they never see the SR_DF_LOGIC
 *     packets this pipeline sends, so a logic export would be junk/empty.
 *
 * The list is opt-in rather than "everything but a block list" so that a future
 * archive-style module cannot resurface the same silent-empty-file bug.
 */
inline bool is_export_module_supported(const char *module_id, bool logic_data)
{
    // Note: the ids are libsigrok's module ids, not the source file names —
    // chronovu_la8.c registers itself as "chronovu-la8" (hyphen).
    static const char *const logic_modules[] = {
        "ascii", "binary", "bits", "chronovu-la8", "csv", "hex", "ols",
        "vcd", "wavedrom", nullptr,
    };
    // Non-logic (DSO/analog/MSO) data has only ever been exportable as CSV.
    static const char *const analog_modules[] = { "csv", nullptr };

    if (!module_id)
        return false;

    const char *const *list = logic_data ? logic_modules : analog_modules;
    for (int i = 0; list[i]; i++) {
        if (g_strcmp0(list[i], module_id) == 0)
            return true;
    }

    return false;
}

/**
 * Extract the module id from an export filter / history string.
 *
 * Accepts every form the export path produces: the bare extension (".csv"), the
 * bare module id ("csv"), a full dialog filter entry ("csv (*.csv)") and the
 * "a;;b;;c" filter list (the last entry wins, mirroring the historical
 * `extName.split('.').last().split(')')` parsing). Returns an empty string when
 * nothing usable is found.
 */
inline QString export_module_id_from_filter(const QString &filter)
{
    return filter.split(QLatin1Char('.')).last()
                 .split(QLatin1Char(')')).first()
                 .trimmed();
}

/**
 * The export module id to actually use for @p requested: @p requested itself
 * when it is still offered for this data type, else @p fallback.
 *
 * `AppConfig::userHistory.exportFormat` persists whatever the user picked and
 * `StoreSession::MakeExportFile(false)` reuses it without a dialog, so a value
 * written by an older build (or by a module that has since been excluded, e.g.
 * ".srzip") must not be honoured -- otherwise the very next "Export" resolves a
 * module this pipeline cannot drive and fails instead of exporting.
 */
inline QString normalize_export_module_id(
    const QString &requested, bool logic_data,
    const QString &fallback = QStringLiteral("csv"))
{
    const QString id = export_module_id_from_filter(requested);
    const QByteArray id_utf8 = id.toUtf8();

    if (!id.isEmpty() &&
        is_export_module_supported(id_utf8.constData(), logic_data)) {
        return id;
    }

    return fallback;
}

} // sr_options
} // data
} // pv

#endif // PXVIEW_PV_DATA_SR_OPTIONS_H
