/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * CH32H417 IAP 固件升级（CDC 虚拟串口传输）实现。
 *
 * 见 iapdialog.h 的协议说明。要点：
 *   - 传输是 USB CDC ACM（IAP 模式枚举为 1A86:5539）：Windows 用系统自带
 *     usbser 驱动，免装；找口/收发走 Qt6::SerialPort，天然跨平台。
 *   - 进 IAP 复用已有 libusb 传输层 ch32_usb_enter_iap()，向 APP 的 EP1 发 0xAE。
 *   - **不使用 hidapi**：那是旧 1v0 固件（IAP = HID 1A86:FE17）才有的通道。
 */

#include "iapdialog.h"

#include "pv/core/langresource.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTextEdit>
#include <QThread>
#include <QVBoxLayout>
#include <QVector>
#include <QPair>

/* libsigrok 侧新增的 libusb 实现：按 VID/PID 打开 APP 设备并发 0xAE。
 * 直接声明原型即可（SR_PRIV 在 Windows 上为空，静态库可正常链接），
 * 这样无需把驱动内部头文件路径引入 PXView。 */
extern "C" int ch32_usb_enter_iap(uint16_t vid, uint16_t pid);
/* 最近一次 ch32_usb_enter_iap() 的失败原因（"not present" / "claim ... failed" 等）。 */
extern "C" const char *ch32_usb_iap_last_error(void);

namespace pv {
namespace dialogs {

/* ======================================================================== */
/* IAPWorker                                                                */
/* ======================================================================== */

IAPWorker::IAPWorker(const QString &firmware_path, QObject *parent)
    : QObject(parent),
      _firmware_path(firmware_path),
      _serial(nullptr),
      _cancelled(false),
      _enter_iap_only(false)
{
}

IAPWorker::~IAPWorker()
{
    close_serial();
}

void IAPWorker::cancel()
{
    QMutexLocker locker(&_mutex);
    _cancelled = true;
}

void IAPWorker::set_enter_iap_only(bool on)
{
    QMutexLocker locker(&_mutex);
    _enter_iap_only = on;
}

bool IAPWorker::is_cancelled()
{
    QMutexLocker locker(&_mutex);
    return _cancelled;
}

void IAPWorker::log(const QString &message)
{
    Q_EMIT log_message(message);
}

void IAPWorker::set_progress(int percent, const QString &status)
{
    Q_EMIT progress_updated(percent, status);
}

/* ------------------------------ 串口（CDC）层 --------------------------- */

/* 按 VID/PID 定位 IAP 的 CDC 串口名。
 *
 * Windows/macOS 上 QSerialPortInfo 的 vendorIdentifier()/productIdentifier()
 * 直接可用；Linux 上是否可用取决于 Qt 构建时有没有 libudev —— 取不到时
 * 退回读 sysfs 的 idVendor/idProduct（不需要 libudev）。 */
static QString find_iap_port()
{
    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();

    for (const QSerialPortInfo &info : ports) {
        if (info.vendorIdentifier() == CH32_IAP_VID &&
            info.productIdentifier() == CH32_IAP_CDC_PID)
            return info.portName();
    }

#ifdef Q_OS_LINUX
    for (const QSerialPortInfo &info : ports) {
        QDir dir(QStringLiteral("/sys/class/tty/%1/device").arg(info.portName()));
        for (int up = 0; up < 4; ++up) {
            QFile fv(dir.filePath(QStringLiteral("idVendor")));
            QFile fp(dir.filePath(QStringLiteral("idProduct")));
            if (fv.open(QIODevice::ReadOnly) && fp.open(QIODevice::ReadOnly)) {
                bool ok_v = false, ok_p = false;
                const uint16_t vid =
                    (uint16_t)fv.readAll().trimmed().toUShort(&ok_v, 16);
                const uint16_t pid =
                    (uint16_t)fp.readAll().trimmed().toUShort(&ok_p, 16);
                if (ok_v && ok_p && vid == CH32_IAP_VID &&
                    pid == CH32_IAP_CDC_PID)
                    return info.portName();
                break;
            }
            if (!dir.cdUp())
                break;
        }
    }
#endif

    return QString();
}

bool IAPWorker::open_serial()
{
    if (_serial)
        return true;

    const QString port = find_iap_port();
    if (port.isEmpty())
        return false;

    QSerialPort *sp = new QSerialPort(this);
    sp->setPortName(port);
    /* CDC 的波特率不影响实际速率（设备侧忽略），但必须给个合法值。 */
    sp->setBaudRate(QSerialPort::Baud115200);
    sp->setDataBits(QSerialPort::Data8);
    sp->setParity(QSerialPort::NoParity);
    sp->setStopBits(QSerialPort::OneStop);
    sp->setFlowControl(QSerialPort::NoFlowControl);

    if (!sp->open(QIODevice::ReadWrite)) {
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_OPEN_PORT_FAILED), "打开串口 %1 失败: %2")).arg(port, sp->errorString()));
        delete sp;
        return false;
    }

    sp->clear(QSerialPort::AllDirections);
    _serial = sp;
    log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_PORT_OPENED), "已打开 IAP 串口 %1")).arg(port));
    return true;
}

void IAPWorker::close_serial()
{
    if (_serial) {
        _serial->close();
        delete _serial;
        _serial = nullptr;
    }
}

/* 入参沿用 [Cmd][Len][payload...] 布局，这里套上同步头/校验/尾：
 *   AA 55 | Cmd | Len | data[Len] | sumL | sumH | 55 AA
 *   sum = Cmd + Len + Σdata                                        */
bool IAPWorker::serial_write_frame(const uint8_t *data, size_t len)
{
    if (!_serial)
        return false;
    if (len < 2 || len > CH32_IAP_DATA_LEN) {
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_BAD_FRAME_LEN), "帧长度非法: %1")).arg((int)len));
        return false;
    }

    uint8_t frame[CH32_IAP_DATA_LEN + 8];
    frame[0] = CH32_IAP_FRAME_HEAD1;
    frame[1] = CH32_IAP_FRAME_HEAD2;
    frame[2] = data[0];                    /* Cmd */
    frame[3] = data[1];                    /* Len */

    uint16_t sum = (uint16_t)data[0] + (uint16_t)data[1];
    for (size_t i = 2; i < len; ++i) {
        frame[2 + i] = data[i];
        sum = (uint16_t)(sum + data[i]);
    }
    frame[len + 2] = (uint8_t)(sum & 0xFF);
    frame[len + 3] = (uint8_t)(sum >> 8);
    frame[len + 4] = CH32_IAP_FRAME_TAIL1;
    frame[len + 5] = CH32_IAP_FRAME_TAIL2;

    qint64 left = (qint64)(len + 6);
    const char *p = reinterpret_cast<const char *>(frame);
    while (left > 0) {
        if (is_cancelled())
            return false;
        const qint64 n = _serial->write(p, left);
        if (n < 0) {
            log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_WRITE_FAILED), "串口写入失败: %1")).arg(_serial->errorString()));
            return false;
        }
        if (n > 0) {
            p += n;
            left -= n;
        }
        /* 阻塞等本批字节出栈 —— 取代旧 HID 版的忙等轮询，既不占满 CPU，
         * 也不受 Windows 15.6ms 定时器粒度影响（因此无需 winmm）。 */
        if (!_serial->waitForBytesWritten(CH32_IAP_TIMEOUT_MS)) {
            log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_WRITE_TIMEOUT), "串口写入超时")));
            return false;
        }
    }
    return true;
}

/* 读应答：AA 55 | 00 | status | 55 AA（6 字节）。成功把 status 写给调用者。 */
bool IAPWorker::serial_read_ack(uint8_t *status, int timeout_ms)
{
    if (!_serial)
        return false;

    QByteArray acc;
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeout_ms;

    while (true) {
        if (is_cancelled())
            return false;

        acc.append(_serial->readAll());
        for (int i = 0; i + 5 < acc.size(); ++i) {
            const uint8_t *b =
                reinterpret_cast<const uint8_t *>(acc.constData()) + i;
            if (b[0] == CH32_IAP_FRAME_HEAD1 && b[1] == CH32_IAP_FRAME_HEAD2 &&
                b[4] == CH32_IAP_FRAME_TAIL1 && b[5] == CH32_IAP_FRAME_TAIL2) {
                if (status)
                    *status = b[3];
                return true;
            }
        }
        /* 只留尾部，防噪声把缓冲撑大。 */
        if (acc.size() > 64)
            acc = acc.right(16);

        const qint64 remain = deadline - QDateTime::currentMSecsSinceEpoch();
        if (remain <= 0)
            break;
        _serial->waitForReadyRead((int)qMin<qint64>(remain, 200));
    }

    log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_ACK_TIMEOUT), "等待应答超时 (%1 ms)")).arg(timeout_ms));
    return false;
}

/* ------------------------------ IAP 操作 ------------------------------- */

bool IAPWorker::enter_iap_mode()
{
    /* 情况一：设备已在 IAP(CDC) 模式 —— 用调试器刚烧完 IAP、或上次升级中断
     * 之后就是这种状态。此时总线上没有 APP 设备可发 0xAE，直接开串口即可。 */
    if (open_serial()) {
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_ALREADY_IN_IAP), "设备已在 IAP 模式（CDC 串口就绪），跳过 0xAE")));
        return true;
    }

    /* 情况二：设备跑着 APP —— 向 APP 的 EP1 发 0xAE，设备写下载标志后复位。
     * APP 的 USB2 与 USB3 使用不同 PID(0x5537/0x5538)，两个都要试。 */
    log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_SEEK_APP), "查找 APP 设备 (1A86:5537/5538) 并发送进入 IAP 命令 (EP1 0xAE)...")));
    QString reason2, reason3;
    int rc = ch32_usb_enter_iap(CH32_APP_VID, CH32_APP_PID_USB2);
    if (rc != 0) {
        reason2 = QString::fromUtf8(ch32_usb_iap_last_error());
        rc = ch32_usb_enter_iap(CH32_APP_VID, CH32_APP_PID_USB3);
        if (rc != 0)
            reason3 = QString::fromUtf8(ch32_usb_iap_last_error());
    }
    if (rc != 0) {
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_NO_APP),
                        "未发现 APP 设备 (1A86:5537/5538)，也未发现 IAP 串口 (1A86:5539)。")));
        /* 把驱动的具体原因打出来 —— 是"不存在"、"打不开"还是"被占用"，处置完全不同。 */
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_PROBE_RESULT),
                        "探测结果: 0x5537 → %1；0x5538 → %2"))
                .arg(reason2.isEmpty() ? QStringLiteral("-") : reason2,
                     reason3.isEmpty() ? QStringLiteral("-") : reason3));
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_NO_APP_HINT),
                        "请检查：设备是否已被本程序打开（正在采集 → 先停止采集）；"
                        "USB 线缆/接口是否正常；若设备已在 IAP 模式却看不到串口，"
                        "请确认系统已为 1A86:5539 加载 usbser 驱动。")));
        return false;
    }

    /* 设备重枚举为 CDC 需要一点时间（实测约 2s），轮询等串口出现。 */
    log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_WAIT_ENUM), "等待设备切换到 IAP(CDC) 并枚举串口...")));
    for (int i = 0; i < 72; i++) {          /* 72 × 250ms ≈ 18s */
        if (is_cancelled())
            return false;
        QThread::msleep(250);
        if (open_serial()) {
            log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_ENTERED), "已进入 IAP 模式")));
            return true;
        }
    }

    log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_WAIT_TIMEOUT), "等待 IAP 串口超时 (18s)")));
    return false;
}

bool IAPWorker::erase_flash(uint32_t start_addr)
{
    if (is_cancelled())
        return false;

    log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_ERASE_START), "擦除 FLASH（设备自 FLASH_Base 起擦；镜像起始地址 0x%1）"))
            .arg(start_addr, 8, 16, QChar('0')));
    set_progress(20, QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_ERASING), "擦除 FLASH...")));

    /* ERASE 帧不含地址：设备把 Program_addr / Verify_addr 重置到 FLASH_Base。 */
    uint8_t cmd[2];
    cmd[0] = CH32_IAP_CMD_ERASE;
    cmd[1] = 0;

    if (!serial_write_frame(cmd, 2)) {
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_ERASE_SEND_FAILED), "擦除命令发送失败")));
        return false;
    }

    uint8_t status = 0xFF;
    if (!serial_read_ack(&status, CH32_IAP_ERASE_TIMEOUT)) {
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_ERASE_TIMEOUT), "擦除响应超时")));
        return false;
    }

    if (status != 0) {
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_ERASE_FAILED), "擦除失败, 错误码 %1")).arg(status));
        return false;
    }

    log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_ERASE_OK), "擦除成功")));
    return true;
}

bool IAPWorker::program_flash(const uint8_t *data, size_t len)
{
    log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_PROM_START), "编程 FLASH, 共 %1 字节")).arg((int)len));
    set_progress(25, QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_PROGRAMMING), "编程 FLASH...")));

    size_t offset = 0;
    int retry = 0;

    while (offset < len) {
        if (is_cancelled())
            return false;

        size_t chunk = (len - offset) > CH32_IAP_MAX_PAYLOAD
                           ? CH32_IAP_MAX_PAYLOAD : (len - offset);

        /* PROM 帧 = [0x80][Len][数据]：设备的 Program_addr 自 FLASH_Base 自增，
         * 所以帧内不带地址；满一页时设备自行擦写。 */
        uint8_t cmd[CH32_IAP_DATA_LEN];
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = CH32_IAP_CMD_PROM;
        cmd[1] = (uint8_t)chunk;
        memcpy(cmd + 2, data + offset, chunk);

        uint8_t status = 0xFF;
        bool ok = serial_write_frame(cmd, 2 + chunk)
                  && serial_read_ack(&status, CH32_IAP_TIMEOUT_MS)
                  && status == 0;

        if (ok) {
            retry = 0;
            offset += chunk;
            /* 编程占 25..90：跳过校验时（默认）进度条仍能连贯走到 90，
             * 不会卡在 70 再跳到 95。 */
            set_progress(25 + (int)(65 * offset / len),
                         QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_PROM_PROGRESS), "编程 FLASH... %1%")).arg((int)(offset * 100 / len)));
            continue;
        }

        if (++retry > 3) {
            log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_PROM_FAILED), "编程失败 (偏移 %1, 错误码 %2)")).arg((int)offset).arg(status));
            return false;
        }
        QThread::msleep(100);
    }

    /* 不再需要旧固件那种"收尾 len=0 编程包"：剩余不足一页的数据由 VERIFY
     * 首包或 END 里的 IAP_Flush_Remaining() 负责补写落盘。 */
    log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_PROM_DONE), "编程完成")));
    return true;
}

bool IAPWorker::verify_flash(uint32_t start_addr, const uint8_t *data, size_t len)
{
    log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_VERIFY_START), "校验 FLASH（设备自 FLASH_Base 起比对；镜像起始地址 0x%1）"))
            .arg(start_addr, 8, 16, QChar('0')));
    set_progress(90, QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_VERIFYING), "校验 FLASH...")));

    size_t offset = 0;
    int retry = 0;

    while (offset < len) {
        if (is_cancelled())
            return false;

        size_t chunk = (len - offset) > CH32_IAP_MAX_PAYLOAD
                           ? CH32_IAP_MAX_PAYLOAD : (len - offset);

        /* VERIFY 帧 = [0x82][Len][期望数据]：设备用自己的 Verify_addr
         * （同样自 FLASH_Base 自增）与数据逐一比对，帧内也不带地址。 */
        uint8_t cmd[CH32_IAP_DATA_LEN];
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = CH32_IAP_CMD_VERIFY;
        cmd[1] = (uint8_t)chunk;
        memcpy(cmd + 2, data + offset, chunk);

        uint8_t status = 0xFF;
        bool ok = serial_write_frame(cmd, 2 + chunk)
                  && serial_read_ack(&status, CH32_IAP_TIMEOUT_MS)
                  && status == 0;

        if (ok) {
            retry = 0;
            offset += chunk;
            set_progress(70 + (int)(25 * offset / len),
                         QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_VERIFY_PROGRESS), "校验 FLASH... %1%")).arg((int)(offset * 100 / len)));
            continue;
        }

        if (++retry > 3) {
            log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_VERIFY_FAILED), "校验失败 (偏移 %1, 错误码 %2)")).arg((int)offset).arg(status));
            return false;
        }
        QThread::msleep(100);
    }

    log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_VERIFY_OK), "校验成功")));
    return true;
}

bool IAPWorker::end_upgrade()
{
    log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_END_SENDING), "发送升级结束命令...")));
    set_progress(95, QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_FINISHING), "完成升级...")));

    uint8_t cmd[2];
    cmd[0] = CH32_IAP_CMD_END;
    cmd[1] = 0;

    if (!serial_write_frame(cmd, 2)) {
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_END_FAILED), "发送结束命令失败")));
        return false;
    }

    /* 设备收到 END 后会补写最后一页、清下载标志并立刻复位，常常来不及应答，
     * 所以"无应答"属正常；有应答时 status == ERR_End(0x02) 同样算成功。 */
    uint8_t status = 0xFF;
    if (serial_read_ack(&status, CH32_IAP_END_TIMEOUT))
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_END_ACK), "设备确认结束 (status=%1)")).arg(status));
    else
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_DEVICE_RESET), "设备已复位（无应答为正常情况）")));

    return true;
}

/* ------------------------------ 固件文件 ------------------------------- */

bool IAPWorker::read_firmware(QByteArray &out_data, uint32_t &start_addr)
{
    QFile file(_firmware_path);
    if (!file.open(QIODevice::ReadOnly)) {
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_OPEN_FILE_FAILED), "无法打开固件文件: %1")).arg(_firmware_path));
        return false;
    }

    const QByteArray raw = file.readAll();
    file.close();

    if (raw.isEmpty()) {
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_FILE_EMPTY), "固件文件为空")));
        return false;
    }

    if (_firmware_path.toLower().endsWith(QStringLiteral(".hex"))) {
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_HEX_DETECTED), "检测到 HEX 文件，正在转换为二进制...")));
        if (!hex_to_bin(raw, out_data, start_addr)) {
            log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_HEX_FAILED), "HEX 转换失败")));
            return false;
        }
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_HEX_DONE), "HEX 转换完成: 起始地址 0x%1, %2 字节"))
                .arg(start_addr, 8, 16, QChar('0'))
                .arg(out_data.size()));
    } else {
        out_data = raw;
        start_addr = 0;
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_BIN_READ), "读取 BIN 文件: %1 字节")).arg(out_data.size()));
    }

    return true;
}

bool IAPWorker::hex_to_bin(const QByteArray &hex_data, QByteArray &bin_data,
                           uint32_t &start_addr)
{
    bin_data.clear();

    QVector<QPair<uint32_t, QByteArray> > records;
    uint32_t base = 0;
    uint32_t min_addr = 0xFFFFFFFFu;
    uint32_t max_addr = 0;
    bool saw_eof = false;

    const QList<QByteArray> lines = hex_data.split('\n');
    for (const QByteArray &raw_line : lines) {
        const QByteArray line = raw_line.trimmed();
        if (line.isEmpty() || line.at(0) != ':')
            continue;

        const QByteArray rec = QByteArray::fromHex(line.mid(1));
        if (rec.size() < 5)
            return false;

        const uint8_t len  = (uint8_t)rec.at(0);
        const uint16_t off = (uint16_t)((((uint8_t)rec.at(1)) << 8) |
                                         ((uint8_t)rec.at(2)));
        const uint8_t type = (uint8_t)rec.at(3);
        if (rec.size() < 5 + len)
            return false;

        /* 校验和：全部字节（含校验字节）相加应为 0。 */
        uint8_t sum = 0;
        for (int i = 0; i < 4 + len; i++)
            sum += (uint8_t)rec.at(i);
        sum += (uint8_t)rec.at(4 + len);
        if (sum != 0) {
            log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_HEX_SUM_ERROR), "HEX 校验和错误")));
            return false;
        }

        const uint8_t *d = reinterpret_cast<const uint8_t *>(rec.constData()) + 4;

        if (type == 0x00) {                 /* 数据记录 */
            const uint32_t a = base + off;
            if (a < min_addr)
                min_addr = a;
            if (a + len > max_addr)
                max_addr = a + len;
            records.append(qMakePair(a, QByteArray(reinterpret_cast<const char *>(d), len)));
        } else if (type == 0x01) {          /* 文件结束 */
            saw_eof = true;
            break;
        } else if (type == 0x02) {          /* 扩展段地址 */
            if (len >= 2)
                base = (uint32_t)((((uint8_t)d[0] << 8) | (uint8_t)d[1]) << 4);
        } else if (type == 0x04) {          /* 扩展线性地址 */
            if (len >= 2)
                base = (uint32_t)((((uint32_t)(uint8_t)d[0] << 8) | (uint8_t)d[1]) << 16);
        }
        /* 0x03/0x05 起始地址记录：本用途忽略。 */
    }

    /* saw_eof 缺失不作为硬错误（部分工具裁剪结尾）。 */
    (void)saw_eof;

    if (records.isEmpty() || min_addr == 0xFFFFFFFFu) {
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_HEX_NO_DATA), "HEX 中没有有效数据记录")));
        return false;
    }

    bin_data = QByteArray((int)(max_addr - min_addr), '\0');
    for (int i = 0; i < records.size(); i++) {
        const uint32_t a = records.at(i).first;
        const QByteArray &b = records.at(i).second;
        const int at = (int)(a - min_addr);
        if (at >= 0 && at + b.size() <= bin_data.size())
            memcpy(bin_data.data() + at, b.constData(), (size_t)b.size());
    }

    start_addr = min_addr;
    return true;
}

/* ------------------------------ 主流程 --------------------------------- */

void IAPWorker::do_work()
{
    bool ok = false;
    QByteArray fw;
    uint32_t start_addr = 0;

    /* 「进入 IAP」按钮：只把设备切到 IAP 串口，不读固件、不擦写。 */
    if (_enter_iap_only) {
        set_progress(5, QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_CONNECTING), "连接设备...")));
        const bool entered = enter_iap_mode();
        if (!entered)
            log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_FAILED), "升级失败或已取消")));
        close_serial();
        Q_EMIT finished(entered);
        return;
    }

    do {
        if (!read_firmware(fw, start_addr))
            break;

        set_progress(5, QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_CONNECTING), "连接设备...")));

        /* enter_iap_mode() 内部已覆盖两种情况：
         *   设备已在 IAP(CDC) → 直接开串口；
         *   设备跑着 APP      → EP1 发 0xAE 后等 CDC 串口枚举出来。 */
        if (!enter_iap_mode())
            break;

        if (!erase_flash(start_addr))
            break;
        if (!program_flash(reinterpret_cast<const uint8_t *>(fw.constData()),
                           (size_t)fw.size()))
            break;
#if CH32_IAP_DO_VERIFY
        if (!verify_flash(start_addr,
                          reinterpret_cast<const uint8_t *>(fw.constData()),
                          (size_t)fw.size()))
            break;
#endif
        if (!end_upgrade())
            break;

        set_progress(100, QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_DONE), "升级完成")));
        ok = true;
    } while (0);

    if (!ok)
        log(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_FAILED), "升级失败或已取消")));

    close_serial();
    Q_EMIT finished(ok);
}

/* ======================================================================== */
/* IAPDialog                                                                */
/* ======================================================================== */

IAPDialog::IAPDialog(QWidget *parent)
    : PxDialog(parent, true /* hasClose */),
      _path_edit(nullptr),
      _browse_btn(nullptr),
      _enter_iap_btn(nullptr),
      _start_btn(nullptr),
      _close_btn(nullptr),
      _progress(nullptr),
      _status(nullptr),
      _log_view(nullptr),
      _thread(nullptr),
      _worker(nullptr),
      _busy(false),
      _enter_iap_only(false)
{
    /* 与项目其它对话框一致：标题交给基类标题栏，内容挂到基类主布局。 */
    setTitle(QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_TITLE), "固件升级")));
    setMinimumWidth(520);

    QWidget *host = new QWidget(this);
    QVBoxLayout *root = new QVBoxLayout(host);
    root->setContentsMargins(10, 0, 10, 10);
    root->setSpacing(8);

    /* 使用步骤提示 */
    QLabel *hint = new QLabel(
        QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_HINT),
                    "1. 若设备正在采集，可先点「进入 IAP」切到升级串口\n"
                    "2. 选择固件文件（.bin 或 .hex）\n"
                    "3. 点「开始升级」\n"
                    "提示：仅空片/首次使用才需先用 WCH-Link 烧录 IAP 引导；"
                    "已有 IAP 的板子可直接升级。")),
        host);
    hint->setWordWrap(true);
    root->addWidget(hint);

    /* 固件文件选择：路径框外面套一层带边框的 QFrame，与下面的日志框共用
     * theme.qss 里的同一条规则（QFrame#iapFileFrame / #iapLogFrame），内层控件
     * 由该规则置为无边框 + 透明 —— 这样两个框外观完全一致。 */
    QHBoxLayout *file_row = new QHBoxLayout();
    file_row->addWidget(new QLabel(QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_FIRMWARE_FILE), "固件文件:")), host));

    QFrame *file_frame = new QFrame(host);
    file_frame->setFrameShape(QFrame::StyledPanel);
    file_frame->setObjectName("iapFileFrame");
    QHBoxLayout *file_frame_lay = new QHBoxLayout(file_frame);
    file_frame_lay->setContentsMargins(8, 4, 8, 4);
    _path_edit = new QLineEdit(file_frame);
    _path_edit->setReadOnly(true);
    _path_edit->setFrame(false);   /* QLineEdit 不是 QFrame，用 setFrame(false) */
    _path_edit->setPlaceholderText(QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_PATH_HINT), "请选择 .bin 或 .hex 固件")));
    file_frame_lay->addWidget(_path_edit);
    file_row->addWidget(file_frame, 1);

    _browse_btn = new QPushButton(QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_BROWSE), "浏览...")), host);
    file_row->addWidget(_browse_btn);
    root->addLayout(file_row);

    /* 进度 */
    _progress = new QProgressBar(host);
    _progress->setRange(0, 100);
    _progress->setValue(0);
    root->addWidget(_progress);

    _status = new QLabel(QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_READY), "就绪")), host);
    root->addWidget(_status);

    /* 日志：同样套一层带边框的 QFrame（与上面的路径框共用一条 QSS 规则）。
     * 内层边框必须由 theme.qss 的
     *   QFrame#iapLogFrame QTextEdit { border: none; background: transparent; }
     * 去掉 —— 只调 setFrameShape(NoFrame) 不够，全局 QTextEdit 规则里的 border
     * 会覆盖它，否则会出现"框里还有一圈框"，且 hover 时内层边框变蓝。 */
    QFrame *log_frame = new QFrame(host);
    log_frame->setFrameShape(QFrame::StyledPanel);
    log_frame->setObjectName("iapLogFrame");
    QVBoxLayout *log_layout = new QVBoxLayout(log_frame);
    log_layout->setContentsMargins(8, 8, 8, 8);
    log_layout->setSpacing(8);

    _log_view = new QTextEdit(log_frame);
    _log_view->setReadOnly(true);
    _log_view->setMinimumHeight(160);
    _log_view->setFrameShape(QFrame::NoFrame);
    log_layout->addWidget(_log_view);

    root->addWidget(log_frame, 1);

    /* 按钮：与 ALL-LOGIC 的 btnRow 排布一致 ——
     * 「进入 IAP」在最左，中间 addStretch 撑开，右侧「开始升级」「关闭」。 */
    QHBoxLayout *btn_row = new QHBoxLayout();
    _enter_iap_btn = new QPushButton(QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_ENTER_IAP), "进入 IAP")), host);
    btn_row->addWidget(_enter_iap_btn);
    btn_row->addStretch();
    _start_btn = new QPushButton(QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_START), "开始升级")), host);
    _close_btn = new QPushButton(QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_CLOSE), "关闭")), host);
    btn_row->addWidget(_start_btn);
    btn_row->addWidget(_close_btn);
    root->addLayout(btn_row);

    layout()->addWidget(host, 1);

    connect(_browse_btn, &QPushButton::clicked, this, &IAPDialog::on_browse);
    connect(_enter_iap_btn, &QPushButton::clicked, this, &IAPDialog::on_enter_iap);
    connect(_start_btn, &QPushButton::clicked, this, &IAPDialog::on_start);
    connect(_close_btn, &QPushButton::clicked, this, &QDialog::close);

    set_busy(false);
}

IAPDialog::~IAPDialog()
{
    if (_worker)
        _worker->cancel();
    if (_thread) {
        _thread->quit();
        _thread->wait();
    }
    delete _worker;
    delete _thread;
}

void IAPDialog::closeEvent(QCloseEvent *event)
{
    if (_busy) {
        const QMessageBox::StandardButton r = QMessageBox::question(
            this, QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_MSG_TITLE), "固件升级")),
            QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_CANCEL_CONFIRM), "升级正在进行中，确定要取消并退出吗？")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        if (_worker)
            _worker->cancel();
    }
    event->accept();
}

void IAPDialog::append_log(const QString &message)
{
    _log_view->append(message);
}

void IAPDialog::set_busy(bool busy)
{
    _busy = busy;
    _browse_btn->setEnabled(!busy);
    _enter_iap_btn->setEnabled(!busy);
    _start_btn->setEnabled(!busy);
}

void IAPDialog::on_browse()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_SELECT_FILE), "选择固件文件")), QString(),
        QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_FILE_FILTER), "固件文件 (*.bin *.BIN *.hex *.HEX);;所有文件 (*)")));
    if (path.isEmpty())
        return;
    _path_edit->setText(path);
}

void IAPDialog::on_enter_iap()
{
    /* 只把设备切到 IAP 串口（设备跑着 APP 时手动触发），不读固件、不擦写。
     * 不要求先选固件文件。 */
    _log_view->clear();
    _progress->setValue(0);
    _status->setText(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_CONNECTING), "连接设备...")));
    _enter_iap_only = true;
    set_busy(true);

    _thread = new QThread(this);
    _worker = new IAPWorker(QString());
    _worker->set_enter_iap_only(true);
    _worker->moveToThread(_thread);

    connect(_thread, &QThread::started, _worker, &IAPWorker::do_work);
    connect(_worker, &IAPWorker::progress_updated, this, &IAPDialog::on_progress);
    connect(_worker, &IAPWorker::log_message, this, &IAPDialog::on_log);
    connect(_worker, &IAPWorker::finished, this, &IAPDialog::on_finished);

    _thread->start();
}

void IAPDialog::on_start()
{
    const QString path = _path_edit->text().trimmed();
    if (path.isEmpty()) {
        QMessageBox::warning(this, QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_MSG_TITLE), "固件升级")), QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_NO_FILE), "请先选择固件文件。")));
        return;
    }
    if (!QFile::exists(path)) {
        QMessageBox::warning(this, QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_MSG_TITLE), "固件升级")), QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_FILE_MISSING), "固件文件不存在。")));
        return;
    }

    _log_view->clear();
    _progress->setValue(0);
    _status->setText(QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_STARTING), "开始升级...")));
    _enter_iap_only = false;
    set_busy(true);

    append_log(QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_FILE_LABEL), "固件: %1")).arg(QFileInfo(path).fileName()));

    _thread = new QThread(this);
    _worker = new IAPWorker(path);
    _worker->moveToThread(_thread);

    connect(_thread, &QThread::started, _worker, &IAPWorker::do_work);
    connect(_worker, &IAPWorker::progress_updated, this, &IAPDialog::on_progress);
    connect(_worker, &IAPWorker::log_message, this, &IAPDialog::on_log);
    connect(_worker, &IAPWorker::finished, this, &IAPDialog::on_finished);

    _thread->start();
}

void IAPDialog::on_progress(int percent, const QString &status)
{
    _progress->setValue(percent);
    if (!status.isEmpty())
        _status->setText(status);
}

void IAPDialog::on_log(const QString &message)
{
    append_log(message);
}

void IAPDialog::on_finished(bool ok)
{
    if (_thread) {
        _thread->quit();
        _thread->wait();
    }
    delete _worker;
    _worker = nullptr;
    delete _thread;
    _thread = nullptr;

    set_busy(false);
    _progress->setValue(ok ? 100 : _progress->value());

    if (_enter_iap_only) {
        /* 「进入 IAP」：成功只报状态，不要说成"升级完成"。 */
        _status->setText(ok
            ? QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_ENTERED), "已进入 IAP 模式"))
            : QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_STATUS_FAILED), "升级失败")));
        if (!ok) {
            QMessageBox::warning(this, QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_MSG_TITLE), "固件升级")),
                                 QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_FAIL_MSG), "固件升级失败，详见日志。")));
        }
        return;
    }

    _status->setText(ok ? QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IAP_DONE), "升级完成")) : QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_STATUS_FAILED), "升级失败")));

    if (ok) {
        QMessageBox::information(this, QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_MSG_TITLE), "固件升级")),
                                 QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_DONE_MSG), "固件升级完成，设备将重新枚举。")));
    } else {
        QMessageBox::warning(this, QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_MSG_TITLE), "固件升级")),
                             QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IAP_FAIL_MSG), "固件升级失败，详见日志。")));
    }
}

} // namespace dialogs
} // namespace pv
