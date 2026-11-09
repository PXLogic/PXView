/*
 * test_path_encoding.cpp — Windows 中文路径保存/打开往返回归
 *
 * 现场现象：.pxl 能存进中文目录，从同一目录打开却报加载失败；把文件移到
 * 纯英文路径就正常。原因是 pv::path 里有两个语义相反的转换函数：
 *   ConvertPath()   -> UTF-8                 （保存路径走这个）
 *   ToUnicodePath() -> QStringEncoder::System（Windows 本机 ANSI/GBK，打开路径走这个）
 * 下游 C 库（minizip ioapi.c、GLib g_file_test、libzip zip_open、xlog）在
 * Windows 上一律按 UTF-8 解释 char* 路径，GBK 中文还带非法 UTF-8 字节，
 * 于是 unzOpen64() 直接返回 NULL。
 *
 * 本测试锁住两件事：
 *   1) 两个函数必须给出相同的 UTF-8 字节（旧实现在中文路径上确定性失败）；
 *   2) 用这两个字节真的能在中文目录完成 zip 写入 → 读回、xlog 建文件
 *      （旧 xlog 用裸 fopen(ANSI) 时确定性失败）。
 */

#include <QtTest>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <string>

#include "pv/utility/path.h"
#include "pv/base/ZipMaker.h"
#include "log/xlog.h"

namespace {

// "测试波形.pxl" / "采集数据"：中文字面量在 UTF-8 源文件里，与现场路径同类。
const char kChineseDir[] = "采集数据";
const char kChineseFile[] = "测试波形.pxl";

struct ChinesePath {
    // QTemporaryDir is neither copyable nor movable; hold it behind a
    // unique_ptr so the factory can return the bundle by value.
    std::unique_ptr<QTemporaryDir> temp;
    QString dir;
    QString file;
};

ChinesePath make_chinese_path()
{
    ChinesePath p;
    p.temp = std::make_unique<QTemporaryDir>();
    p.dir = p.temp->filePath(QString::fromUtf8(kChineseDir));
    p.file = p.dir + QStringLiteral("/") + QString::fromUtf8(kChineseFile);
    return p;
}

QByteArray bytes(const std::string &s)
{
    return QByteArray(s.data(), static_cast<qsizetype>(s.size()));
}

} // namespace

class TestPathEncoding : public QObject {
    Q_OBJECT

private slots:
    void asciiPathIsUnchanged();
    void bothConversionsAreUtf8();
    void chinesePathIsNotAnsiEncoded();
    void zipRoundTripInChineseDir();
    void xlogCreatesFileInChineseDir();
};

void TestPathEncoding::asciiPathIsUnchanged()
{
    const QString path = QStringLiteral("C:/PXView/data/session.pxl");
    const QByteArray expected = path.toUtf8();

    // Pure ASCII is where the two old encodings agreed, which is why the bug
    // only ever showed up with Chinese paths.
    QCOMPARE(bytes(pv::path::ConvertPath(path)), expected);
    QCOMPARE(bytes(pv::path::ToUnicodePath(path)), expected);
}

void TestPathEncoding::bothConversionsAreUtf8()
{
    const QString path = QString::fromUtf8("C:/用户/桌面/采集数据/") +
                         QString::fromUtf8(kChineseFile);
    const QByteArray expected = path.toUtf8();

    QCOMPARE(bytes(pv::path::ConvertPath(path)), expected);
    QCOMPARE(bytes(pv::path::ToUnicodePath(path)), expected);
    QCOMPARE(bytes(pv::path::ToUnicodePath(path)),
             bytes(pv::path::ConvertPath(path)));
}

void TestPathEncoding::chinesePathIsNotAnsiEncoded()
{
    const QString path = QString::fromUtf8("C:/用户/测试.pxl");
    const QByteArray converted = bytes(pv::path::ToUnicodePath(path));

    // "测试" in GBK/CP936 is B2 E2 CA D4; in UTF-8 it is E6 B5 8B E8 AF 95.
    // The old QStringEncoder::System produced the former.
    QVERIFY2(!converted.contains(QByteArray::fromHex("B2E2CAD4")),
             "path was encoded with the system ANSI code page, not UTF-8");
    QVERIFY(converted.contains(QByteArray::fromHex("E6B58BE8AF95")));
}

void TestPathEncoding::zipRoundTripInChineseDir()
{
    ChinesePath p = make_chinese_path();
    QVERIFY(p.temp->isValid());
    QVERIFY(QDir().mkpath(p.dir));

    // Save: StoreSession::export_exec() uses path::ConvertPath() + ZipMaker.
    {
        ZipMaker maker;
        const std::string out = pv::path::ConvertPath(p.file);
        QVERIFY2(maker.CreateNew(out.c_str(), false),
                 maker.GetError() ? maker.GetError() : "zipOpen64 failed");
        QVERIFY(maker.AddFromBuffer("header", "PXView", 6));
        QVERIFY(maker.Close());
    }
    QVERIFY2(QFileInfo::exists(p.file), "zip was not created at the Chinese path");

    // Open: SigSession::set_file() used path::ToUnicodePath(). With GBK bytes,
    // ioapi.c's MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...) fails
    // and unzOpen64() returns NULL -- exactly the reported "load failed".
    const std::string in = pv::path::ToUnicodePath(p.file);
    ZipReader reader(in.c_str());
    QVERIFY2(reader.HaveArchive(),
             "unzOpen64() could not open the archive at the Chinese path");

    auto header = reader.GetInnterFileData("header");
    QVERIFY(header != nullptr);
    QCOMPARE(QByteArray(header->data(), header->size()), QByteArray("PXView"));
}

void TestPathEncoding::xlogCreatesFileInChineseDir()
{
    ChinesePath p = make_chinese_path();
    QVERIFY(p.temp->isValid());
    QVERIFY(QDir().mkpath(p.dir));

    xlog_context *ctx = xlog_new2(0);
    QVERIFY(ctx != nullptr);

    const std::string file = pv::path::ToUnicodePath(p.file + QStringLiteral(".log"));
    int index = -1;
    const int added = xlog_add_receiver_from_file(ctx, file.c_str(), &index, 0);
    QCOMPARE(added, 0);
    QVERIFY2(QFileInfo::exists(QString::fromStdString(file)),
             "xlog's fopen() mangled the Chinese directory name");

    // The reset path opens the file a second way; it must use the same
    // UTF-8 -> wide-char conversion.
    QCOMPARE(xlog_reset_log_file(ctx, index, file.c_str()), 0);
    QVERIFY(QFileInfo::exists(QString::fromStdString(file)));

    xlog_free(ctx);
}

QTEST_MAIN(TestPathEncoding)
#include "test_path_encoding.moc"
