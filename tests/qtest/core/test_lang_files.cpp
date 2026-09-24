/*
 * This file is part of the PXView project.
 *
 * 语言文件契约测试。
 *
 * 起因（2026-09-25）：导入选项对话框整体显示英文——新增的
 * lang/cn/input_output.json 用了 {"language":…,"items":[…]} 的对象包装，而
 * LangResource::load_page() 只认**顶层数组**（QJsonDocument::array()）：对象会
 * 被当成空数组，整页静默不加载，于是每个 id 都回退到 libsigrok 模块自带的英文
 * 原文。文件本身语法完全合法，任何"能不能解析 JSON"的检查都发现不了，只有从
 * 解析器视角查一次才行——本用例就是这个视角。
 *
 * 锁定三件事：
 *   1) lang/<lang>/*.json（含 dec/）必须是顶层数组，条目必须带非空 id/text；
 *   2) 注册在 lange_page_keys[] 里的页面文件在参考语言（cn）下必须存在；
 *   3) InputOutput 用到的选项 id（IDS_OPTION_<选项 id> 及其 _DESC）三语齐全。
 *
 * 只依赖 Qt（lange_page_keys 是纯数据），不需要 libsigrok / pxview-core。
 */

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

#include "pv/core/langresource.h"

#ifndef PXVIEW_SOURCE_DIR
#error "PXVIEW_SOURCE_DIR must be defined by tests/qtest/CMakeLists.txt"
#endif

namespace {

QString langRoot()
{
    return QStringLiteral(PXVIEW_SOURCE_DIR "/lang");
}

QStringList languages()
{
    return QStringList{QStringLiteral("cn"), QStringLiteral("en"),
                       QStringLiteral("traditional")};
}

/* 从解析器的视角读一个语言文件。
 * QVERIFY 只能在 void 函数里使用（失败路径是 return;），所以这里用返回值 +
 * 错误串表达失败，由调用方断言。 */
bool loadEntries(const QString &path, QSet<QString> *ids, QString *error)
{
    QFile file(path);
    if (!file.exists()) {
        *error = path + QStringLiteral(": file does not exist");
        return false;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *error = path + QStringLiteral(": cannot open");
        return false;
    }

    QJsonParseError parse_error{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        *error = path + QStringLiteral(": JSON error: ") + parse_error.errorString();
        return false;
    }

    // 关键：LangResource::load_page() 用的是 doc.array()。对象包装会被当成空
    // 数组 → 整页不加载 → 每个 id 静默回退成英文原文。
    if (!doc.isArray()) {
        *error = path + QStringLiteral(": top level must be a JSON array "
                                       "(LangResource::load_page uses doc.array())");
        return false;
    }

    const QJsonArray array = doc.array();
    if (array.isEmpty()) {
        *error = path + QStringLiteral(": no entries");
        return false;
    }

    for (const QJsonValue &value : array) {
        const QJsonObject obj = value.toObject();
        const QString id = obj.value(QStringLiteral("id")).toString().trimmed();
        const QString text = obj.value(QStringLiteral("text")).toString().trimmed();
        if (id.isEmpty()) {
            *error = path + QStringLiteral(": entry without id");
            return false;
        }
        if (text.isEmpty()) {
            *error = path + QStringLiteral(": empty text for '") + id + QLatin1Char('\'');
            return false;
        }
        ids->insert(id);
    }

    return true;
}

} // namespace

class TestLangFiles : public QObject
{
    Q_OBJECT

private slots:
    /* 1) 每个语言文件都必须能被解析器读懂（顶层数组 + 非空 id/text）。 */
    void langFilesAreTopLevelArrays()
    {
        for (const QString &lang : languages()) {
            const QDir dir(langRoot() + QLatin1Char('/') + lang);
            QVERIFY2(dir.exists(), qPrintable(QStringLiteral("missing dir: ") + dir.path()));

            QStringList relatives;
            for (const QString &name : dir.entryList(QStringList{QStringLiteral("*.json")},
                                                     QDir::Files))
                relatives << name;
            const QDir dec(dir.filePath(QStringLiteral("dec")));
            if (dec.exists()) {
                for (const QString &name : dec.entryList(QStringList{QStringLiteral("*.json")},
                                                         QDir::Files))
                    relatives << QStringLiteral("dec/") + name;
            }
            QVERIFY2(!relatives.isEmpty(), qPrintable(QStringLiteral("no language files in ") + dir.path()));

            for (const QString &relative : relatives) {
                QSet<QString> ids;
                QString error;
                const QString path = dir.filePath(relative);
                QVERIFY2(loadEntries(path, &ids, &error), qPrintable(error));
                QVERIFY2(!ids.isEmpty(), qPrintable(path + QStringLiteral(": no ids")));
            }
        }
    }

    /* 2) 注册了页面却缺文件（文件名打错、忘了加）同样是静默回退英文。 */
    void registeredPagesExistInReferenceLanguage()
    {
        bool option_page_registered = false;

        // lange_page_keys / lang_page_item 在全局命名空间（见 langresource.h）。
        for (const lang_page_item &page : lange_page_keys) {
            const QString source = QString::fromLatin1(page.source);
            if (source.contains(QStringLiteral("input_output.json")))
                option_page_registered = true;

            for (const QString &raw : source.split(QLatin1Char(','))) {
                const QString relative = raw.trimmed();
                if (relative.isEmpty())
                    continue;

                const QString path = langRoot() + QStringLiteral("/cn/") + relative;
                QVERIFY2(QFile::exists(path),
                         qPrintable(QStringLiteral("cn is missing registered page: ") + relative));
            }
        }

        // 导入/导出选项页必须仍在注册表里（文件在、注册丢了 → 一样回退英文）。
        QVERIFY(option_page_registered);
    }

    /* 3) InputOutput 按选项 id 拼 id 查表，任一语言缺条目都会让对话框显示英文。 */
    void inputOutputOptionIdsAreTranslated()
    {
        // 与 pv::prop::binding::InputOutput 的拼法保持一致：
        // "IDS_OPTION_" + 选项 id 的大写形式（+ "_DESC" 表示说明）。
        const QStringList option_ids{
            // binary / chronovu-la8 / raw_analog
            QStringLiteral("numchannels"),     QStringLiteral("samplerate"),
            QStringLiteral("format"),
            // csv
            QStringLiteral("column_formats"),  QStringLiteral("single_column"),
            QStringLiteral("first_column"),    QStringLiteral("logic_channels"),
            QStringLiteral("single_format"),   QStringLiteral("start_line"),
            QStringLiteral("header"),          QStringLiteral("column_separator"),
            QStringLiteral("comment_leader"),
            // vcd
            QStringLiteral("samplerate_overwrite"), QStringLiteral("downsample"),
            QStringLiteral("skip"),            QStringLiteral("compress"),
        };

        for (const QString &lang : languages()) {
            const QString relative = lang + QStringLiteral("/input_output.json");
            QSet<QString> ids;
            QString error;
            QVERIFY2(loadEntries(langRoot() + QLatin1Char('/') + relative, &ids, &error),
                     qPrintable(error));

            // 控件装不下时用来显示"实际值"的模板（由对话框层合成，不在 Int 里）。
            QVERIFY2(ids.contains(QStringLiteral("IDS_OPTION_VALUE_CLAMPED")),
                     qPrintable(relative + QStringLiteral(": missing ") +
                                QStringLiteral("IDS_OPTION_VALUE_CLAMPED")));

            for (const QString &option_id : option_ids) {
                const QString base = QStringLiteral("IDS_OPTION_") + option_id.toUpper();
                QVERIFY2(ids.contains(base),
                         qPrintable(relative + QStringLiteral(": missing ") + base));
                QVERIFY2(ids.contains(base + QStringLiteral("_DESC")),
                         qPrintable(relative + QStringLiteral(": missing ") + base + QStringLiteral("_DESC")));
            }
        }
    }
};

QTEST_MAIN(TestLangFiles)

#include "test_lang_files.moc"
