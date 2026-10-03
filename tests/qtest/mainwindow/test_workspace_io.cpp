/*
 * This file is part of the PXView project.
 *
 * Unit tests for the workspace (tab session) persistence layer.
 *
 * workspace_io.cpp is deliberately free of View/QWidget dependencies so it can
 * be compiled straight into this target — the per-tab view-density conversion
 * lives in tab_manager.cpp for exactly that reason. Path isolation goes through
 * pv::set_workspace_path_override() into a QTemporaryDir: the real user profile
 * directory is never touched.
 *
 * Locked invariants (see AGENT_CONTRACTS.md "Workspace (tab session)
 * persistence"):
 *   - round-trip fidelity of every persisted field, including `connid`,
 *     `isFileDevice` and the `uiLayout` nested inside `session`;
 *   - degradation is never fatal — missing file / malformed JSON / unsupported
 *     Version / empty tab list all return false so the caller keeps today's
 *     single-tab startup behaviour.
 */

#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

// ── xlog stub: workspace_io.cpp reaches pxv_* through log.h ──
#include "log/xlog.h"
xlog_writer *pxv_log = nullptr;
extern "C" {
int xlog_err(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_warn(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_info(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_dbg(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_detail(xlog_writer *w, const char *, ...) { (void)w; return 0; }
}

// ── config stub: only GetProfileDir() is referenced, and only when no path
//    override is installed (never the case in this test). Keeping this a stub
//    avoids dragging the whole config layer into the test. ──
#include "pv/config/appconfig.h"
QString GetProfileDir() { return QString(); }

#include "pv/mainwindow/workspace_io.h"

class TestWorkspaceIo : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void PathOverrideIsHonored();
  void RoundTripPreservesAllFields();
  void RoundTripPreservesPerTabViewDensity();
  void RoundTripPreservesImportedFileLoader();
  void LegacyTabWithoutImportFieldsDefaultsToPxl();
  void RoundTripPreservesDecoderStacks();
  void LegacyTabWithoutDecoderFieldIsEmpty();
  void MissingFileDegradesGracefully();
  void MalformedJsonDegradesGracefully();
  void UnsupportedVersionIsRejected();
  void EmptyTabListIsRejected();
  void TabsWithoutSessionAreSkipped();

private:
  QTemporaryDir _dir;
  QString path() const { return _dir.filePath("workspace.json"); }
  bool writeRaw(const QByteArray &payload);
};

void TestWorkspaceIo::init() {
  QVERIFY(_dir.isValid());
  pv::set_workspace_path_override(path());
}

void TestWorkspaceIo::cleanup() {
  pv::set_workspace_path_override(QString());
  QFile::remove(path());
}

bool TestWorkspaceIo::writeRaw(const QByteArray &payload) {
  QFile f(path());
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  const bool ok = (f.write(payload) == payload.size());
  f.close();
  return ok;
}

void TestWorkspaceIo::PathOverrideIsHonored() {
  QCOMPARE(pv::workspace_file_path(), path());
}

void TestWorkspaceIo::RoundTripPreservesAllFields() {
  pv::Workspace ws;
  ws.activeTab = 1;

  pv::WorkspaceTab device;
  device.title = "demo";
  device.driver = "demo";
  device.connid = QString();
  device.workMode = 0;
  device.isFileDevice = false;
  device.session = QJsonObject{
      {"work_mode", 0},
      {"channels", QJsonArray{QJsonObject{{"index", 0}, {"own_height", 48}}}}};
  ws.tabs.push_back(device);

  pv::WorkspaceTab file;
  file.title = "捕获 2";
  file.filePath = "C:/data/foo.pxl";
  file.driver = "pxlogic";
  file.connid = "conn-42";
  file.workMode = 1;
  file.isFileDevice = true;
  file.session = QJsonObject{{"work_mode", 1}};
  ws.tabs.push_back(file);

  QVERIFY(pv::write_workspace_file(ws));

  pv::Workspace out;
  QVERIFY(pv::read_workspace_file(out));

  QCOMPARE(out.activeTab, 1);
  QCOMPARE(static_cast<int>(out.tabs.size()), 2);

  QCOMPARE(out.tabs[0].title, QString("demo"));
  QCOMPARE(out.tabs[0].driver, QString("demo"));
  QCOMPARE(out.tabs[0].workMode, 0);
  QVERIFY(!out.tabs[0].isFileDevice);
  QCOMPARE(out.tabs[0].filePath, QString());
  // session 必须原样往返（own_height 是通道布局的核心字段）
  const QJsonArray channels =
      out.tabs[0].session.value("channels").toArray();
  QCOMPARE(channels.size(), 1);
  QCOMPARE(channels.at(0).toObject().value("own_height").toInt(), 48);

  // 文件 tab：filePath / isFileDevice / 设备身份三元组必须保住 —— 恢复时要靠
  // (driver, connid) 重解析设备，靠 isFileDevice 决定"不假装绑定设备"。
  QCOMPARE(out.tabs[1].title, QString("捕获 2"));
  QCOMPARE(out.tabs[1].filePath, QString("C:/data/foo.pxl"));
  QCOMPARE(out.tabs[1].driver, QString("pxlogic"));
  QCOMPARE(out.tabs[1].connid, QString("conn-42"));
  QCOMPARE(out.tabs[1].workMode, 1);
  QVERIFY(out.tabs[1].isFileDevice);
}

void TestWorkspaceIo::RoundTripPreservesImportedFileLoader() {
  // The loader kind decides how a restored file tab is replayed: native .pxl
  // goes through set_file(); VCD/CSV/... through import_file(). Losing this
  // field made imported tabs use the .pxl loader on restore and never come
  // back. `importFormat` remembers an explicit module choice (e.g. ".bin" →
  // "binary").
  pv::Workspace ws;
  pv::WorkspaceTab native;
  native.title = "session";
  native.filePath = "C:/data/session.pxl";
  native.isFileDevice = true;
  native.isImportedFile = false;
  native.session = QJsonObject{{"work_mode", 0}};
  ws.tabs.push_back(native);

  pv::WorkspaceTab vcd;
  vcd.title = "wave";
  vcd.filePath = "C:/data/wave.vcd";
  vcd.isFileDevice = true;
  vcd.isImportedFile = true;
  vcd.importFormat = "vcd";
  vcd.session = QJsonObject{{"work_mode", 0}};
  ws.tabs.push_back(vcd);

  pv::WorkspaceTab rawbin;
  rawbin.title = "raw";
  rawbin.filePath = "C:/data/raw.bin";
  rawbin.isFileDevice = true;
  rawbin.isImportedFile = true;
  rawbin.importFormat = "binary";
  rawbin.session = QJsonObject{{"work_mode", 0}};
  ws.tabs.push_back(rawbin);

  QVERIFY(pv::write_workspace_file(ws));

  pv::Workspace out;
  QVERIFY(pv::read_workspace_file(out));
  QCOMPARE(static_cast<int>(out.tabs.size()), 3);

  QVERIFY(!out.tabs[0].isImportedFile);
  QCOMPARE(out.tabs[0].importFormat, QString());

  QVERIFY(out.tabs[1].isImportedFile);
  QCOMPARE(out.tabs[1].importFormat, QString("vcd"));

  QVERIFY(out.tabs[2].isImportedFile);
  QCOMPARE(out.tabs[2].importFormat, QString("binary"));
}

void TestWorkspaceIo::LegacyTabWithoutImportFieldsDefaultsToPxl() {
  // An older workspace has no isImportedFile/importFormat. Absent must mean
  // "native .pxl" (false / empty), preserving the pre-existing behaviour for
  // sessions written before the fields existed.
  QVERIFY(writeRaw(R"({"Version":1,"activeTab":0,"tabs":[
      {"title":"old","filePath":"C:/data/old.pxl",
       "device":{"driver":"virtual-session","connid":"","workMode":0,
                 "isFileDevice":true},
       "session":{"work_mode":0}}]})"));
  pv::Workspace out;
  QVERIFY(pv::read_workspace_file(out));
  QCOMPARE(static_cast<int>(out.tabs.size()), 1);
  QVERIFY(out.tabs[0].isFileDevice);
  QVERIFY(!out.tabs[0].isImportedFile);
  QCOMPARE(out.tabs[0].importFormat, QString());
}

void TestWorkspaceIo::RoundTripPreservesDecoderStacks() {
  // Decoder (protocol analyzer) stacks on an *imported* file tab are persisted
  // NOWHERE else: an imported tab has no `.pxl` (whose embedded `decoders` zip
  // entry is the native path) and file devices never write a `.pxc` (only
  // hardware/demo do — see MainWindowConfigIO::save_config). So the workspace
  // carries them, per tab. Losing this array made everyone's VCD/CSV analyzers
  // vanish on the next launch.
  pv::Workspace ws;
  pv::WorkspaceTab vcd;
  vcd.title = "wave";
  vcd.filePath = "C:/data/wave.vcd";
  vcd.isFileDevice = true;
  vcd.isImportedFile = true;
  vcd.importFormat = "vcd";
  vcd.session = QJsonObject{{"work_mode", 0}};
  vcd.decoder = QJsonArray{
      QJsonObject{{"id", "uart"}, {"stacked_ok", false}},
      QJsonObject{{"id", "spi"}, {"stacked_ok", true}}};
  ws.tabs.push_back(vcd);

  // A native .pxl tab records none: set_file() replays them from the file, so a
  // workspace copy would be a duplicate. Empty stays empty.
  pv::WorkspaceTab native;
  native.title = "session";
  native.filePath = "C:/data/session.pxl";
  native.isFileDevice = true;
  native.session = QJsonObject{{"work_mode", 0}};
  ws.tabs.push_back(native);

  QVERIFY(pv::write_workspace_file(ws));

  pv::Workspace out;
  QVERIFY(pv::read_workspace_file(out));
  QCOMPARE(static_cast<int>(out.tabs.size()), 2);

  const QJsonArray dec = out.tabs[0].decoder;
  QCOMPARE(dec.size(), 2);
  QCOMPARE(dec.at(0).toObject().value("id").toString(), QString("uart"));
  QVERIFY(!dec.at(0).toObject().value("stacked_ok").toBool());
  QCOMPARE(dec.at(1).toObject().value("id").toString(), QString("spi"));
  QVERIFY(dec.at(1).toObject().value("stacked_ok").toBool());

  QVERIFY(out.tabs[1].decoder.isEmpty());
}

void TestWorkspaceIo::LegacyTabWithoutDecoderFieldIsEmpty() {
  // A workspace written before the `decoder` field existed must read back as an
  // empty array — the restore path then does nothing extra, i.e. pre-existing
  // sessions keep their old behaviour instead of failing to parse.
  QVERIFY(writeRaw(R"({"Version":1,"activeTab":0,"tabs":[
      {"title":"old","filePath":"C:/data/old.vcd",
       "isFileDevice":true,"isImportedFile":true,"importFormat":"vcd",
       "session":{"work_mode":0}}]})"));
  pv::Workspace out;
  QVERIFY(pv::read_workspace_file(out));
  QCOMPARE(static_cast<int>(out.tabs.size()), 1);
  QVERIFY(out.tabs[0].decoder.isEmpty());
}

void TestWorkspaceIo::RoundTripPreservesPerTabViewDensity() {  // 视图密度是 per-tab 状态，住在 session.uiLayout 里（不进 .pxc）。
  pv::Workspace ws;
  pv::WorkspaceTab a;
  a.title = "A";
  a.session = QJsonObject{{"work_mode", 0}};
  a.session["uiLayout"] = QJsonObject{{"signalHeightScale", 44}};
  ws.tabs.push_back(a);

  pv::WorkspaceTab b;
  b.title = "B";
  b.session = QJsonObject{{"work_mode", 0}};
  b.session["uiLayout"] = QJsonObject{{"signalHeightScale", 72}};
  ws.tabs.push_back(b);

  QVERIFY(pv::write_workspace_file(ws));

  pv::Workspace out;
  QVERIFY(pv::read_workspace_file(out));
  QCOMPARE(static_cast<int>(out.tabs.size()), 2);
  QCOMPARE(out.tabs[0].session.value("uiLayout")
               .toObject()
               .value("signalHeightScale")
               .toInt(),
           44);
  QCOMPARE(out.tabs[1].session.value("uiLayout")
               .toObject()
               .value("signalHeightScale")
               .toInt(),
           72);
}

void TestWorkspaceIo::MissingFileDegradesGracefully() {
  pv::Workspace out;
  QVERIFY(!pv::read_workspace_file(out));
  QVERIFY(out.tabs.empty());  // 调用方随后走单 tab 启动路径
}

void TestWorkspaceIo::MalformedJsonDegradesGracefully() {
  QVERIFY(writeRaw("{ this is not json"));
  pv::Workspace out;
  QVERIFY(!pv::read_workspace_file(out));
  QVERIFY(out.tabs.empty());
}

void TestWorkspaceIo::UnsupportedVersionIsRejected() {
  QVERIFY(writeRaw(R"({"Version":999,"activeTab":0,"tabs":[{"session":{}}]})"));
  pv::Workspace out;
  QVERIFY(!pv::read_workspace_file(out));
}

void TestWorkspaceIo::EmptyTabListIsRejected() {
  QVERIFY(writeRaw(R"({"Version":1,"activeTab":0,"tabs":[]})"));
  pv::Workspace out;
  QVERIFY(!pv::read_workspace_file(out));
}

void TestWorkspaceIo::TabsWithoutSessionAreSkipped() {
  // 没有 session 对象的条目是垃圾数据：跳过而不是造出一个空 tab 让用户困惑。
  QVERIFY(writeRaw(R"({"Version":1,"activeTab":0,)"
                   R"("tabs":[{"title":"bad"},{"title":"ok","session":{}}]})"));
  pv::Workspace out;
  QVERIFY(pv::read_workspace_file(out));
  QCOMPARE(static_cast<int>(out.tabs.size()), 1);
  QCOMPARE(out.tabs[0].title, QString("ok"));
}

QTEST_MAIN(TestWorkspaceIo)
#include "test_workspace_io.moc"
