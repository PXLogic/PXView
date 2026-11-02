/*
 * Test case for dynamic user decoder loading and hot-reloading without restart.
 */

#include <QTest>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>
#include <libsigrokdecode/libsigrokdecode.h>

class TestUserDecoderReload : public QObject {
    Q_OBJECT

private:
    QTemporaryDir *_temp_user_dir;

    bool create_decoder_package(const QString &dir_path, const QString &id,
                                const QString &name, const QString &longname) {
        QDir d(dir_path);
        if (!d.exists() && !d.mkpath("."))
            return false;

        QFile init_file(dir_path + "/__init__.py");
        if (!init_file.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
        QTextStream init_ts(&init_file);
        init_ts << "from .pd import Decoder\n";
        init_file.close();

        QFile pd_file(dir_path + "/pd.py");
        if (!pd_file.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
        QTextStream ts(&pd_file);
        ts << "import sigrokdecode as srd\n"
           << "class Decoder(srd.Decoder):\n"
           << "    api_version = 3\n"
           << "    id = '" << id << "'\n"
           << "    name = '" << name << "'\n"
           << "    longname = '" << longname << "'\n"
           << "    desc = 'Test decoder'\n"
           << "    license = 'gplv2+'\n"
           << "    inputs = ['logic']\n"
           << "    outputs = []\n"
           << "    tags = ['Util']\n"
           << "    def __init__(self):\n"
           << "        super().__init__()\n"
           << "    def start(self):\n"
           << "        pass\n"
           << "    def reset(self):\n"
           << "        pass\n"
           << "    def decode(self):\n"
           << "        pass\n";
        pd_file.close();
        return true;
    }

private slots:
    void initTestCase() {
        _temp_user_dir = new QTemporaryDir();
        QVERIFY(_temp_user_dir->isValid());

        QString sys_dec_dir = "share/libsigrokdecode/decoders";
        if (!QDir(sys_dec_dir).exists()) {
            sys_dec_dir = "../share/libsigrokdecode/decoders";
        }
        if (!QDir(sys_dec_dir).exists()) {
            sys_dec_dir = "package/decoders";
        }
        if (!QDir(sys_dec_dir).exists()) {
            sys_dec_dir = "../package/decoders";
        }
        if (!QDir(sys_dec_dir).exists()) {
            sys_dec_dir = "install.dir/share/libsigrokdecode/decoders";
        }
        if (!QDir(sys_dec_dir).exists()) {
            sys_dec_dir = "../install.dir/share/libsigrokdecode/decoders";
        }

        int ret = srd_init(sys_dec_dir.toUtf8().data());
        QCOMPARE(ret, SRD_OK);
    }

    void cleanupTestCase() {
        srd_exit();
        delete _temp_user_dir;
    }

    void test_system_decoders_loaded() {
        int ret = srd_decoder_load_all();
        QCOMPARE(ret, SRD_OK);

        struct srd_decoder *dec = srd_decoder_get_by_id("i2c");
        QVERIFY(dec != nullptr);
        QVERIFY(!dec->is_user_decoder);

        struct srd_decoder *counter_dec = srd_decoder_get_by_id("counter");
        QVERIFY(counter_dec != nullptr);
        QVERIFY(!counter_dec->is_user_decoder);
    }

    void test_dynamic_add_new_user_decoder() {
        QString dec_dir = _temp_user_dir->path() + "/custom_test_pd";
        QVERIFY(create_decoder_package(dec_dir, "custom_test_pd",
                                       "Custom Test PD", "Custom Test Long Name"));

        // Register user search path dynamically during runtime
        int ret = srd_decoder_user_searchpath_add(_temp_user_dir->path().toUtf8().data());
        QCOMPARE(ret, SRD_OK);

        // Hot rescan & reload without restarting application
        ret = srd_decoder_load_all();
        QCOMPARE(ret, SRD_OK);

        struct srd_decoder *dec = srd_decoder_get_by_id("custom_test_pd");
        QVERIFY(dec != nullptr);
        QVERIFY(dec->is_user_decoder);
        QCOMPARE(QString(dec->id), QString("custom_test_pd"));
        QCOMPARE(QString(dec->name), QString("Custom Test PD"));
    }

    void test_dynamic_override_system_decoder() {
        // Create user decoder with same ID as system decoder ("i2c")
        QString dec_dir = _temp_user_dir->path() + "/i2c";
        QVERIFY(create_decoder_package(dec_dir, "i2c",
                                       "User Custom I2C", "User Custom I2C Decoder"));

        // Trigger hot rescan & reload
        int ret = srd_decoder_load_all();
        QCOMPARE(ret, SRD_OK);

        // System decoder "i2c" must NOT be overwritten!
        struct srd_decoder *sys_dec = srd_decoder_get_by_id("i2c");
        QVERIFY(sys_dec != nullptr);
        QVERIFY(!sys_dec->is_user_decoder);
        QVERIFY(QString(sys_dec->name) != "User Custom I2C");

        // User conflict copy must be retained with renamed id and name with suffix
        struct srd_decoder *user_dec = srd_decoder_get_by_id("i2c_user");
        QVERIFY(user_dec != nullptr);
        QVERIFY(user_dec->is_user_decoder);
        QCOMPARE(QString(user_dec->name), QString("User Custom I2C (user)"));

        // Verify both coexist in pd_list
        bool found_sys = false;
        bool found_user = false;
        for (const GSList *l = srd_decoder_list(); l; l = l->next) {
            const struct srd_decoder *d = (const struct srd_decoder *)l->data;
            if (d && QString(d->id) == "i2c") found_sys = true;
            if (d && QString(d->id) == "i2c_user") found_user = true;
        }
        QVERIFY(found_sys);
        QVERIFY(found_user);
    }

    void test_dynamic_update_user_decoder() {
        // Modify existing user decoder file on disk while running
        QString dec_dir = _temp_user_dir->path() + "/custom_test_pd";
        QVERIFY(create_decoder_package(dec_dir, "custom_test_pd",
                                       "Custom Test PD V2", "Custom Test Long Name V2"));

        // Trigger hot rescan & reload
        int ret = srd_decoder_load_all();
        QCOMPARE(ret, SRD_OK);

        struct srd_decoder *dec = srd_decoder_get_by_id("custom_test_pd");
        QVERIFY(dec != nullptr);
        QVERIFY(dec->is_user_decoder);
        QCOMPARE(QString(dec->name), QString("Custom Test PD V2"));
    }
};

QTEST_MAIN(TestUserDecoderReload)
#include "test_user_decoder_reload.moc"
