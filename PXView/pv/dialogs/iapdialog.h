/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * CH32H417 IAP 固件升级（CDC 虚拟串口传输）。
 *
 * 设备侧身份（下位机 OpenSourceLogic-CH32H417/.../usb_desc.h）：
 *   APP 采集  USB2 = 1A86:5537 / USB3 = 1A86:5538   —— 需 WinUSB 供 libusb 采集
 *   IAP 烧写          1A86:5539                    —— USB CDC ACM（usbser 免驱）
 *
 * 流程（与下位机 IAP/Common/iap.c 一一对应）：
 *   1. 设备若已在 IAP(CDC) → 直接用串口；否则用 libusb 向 APP 的 EP1 发 0xAE，
 *      设备写下载标志后复位并重枚举成 CDC 串口（ch32_usb_enter_iap()）。
 *   2. 串口帧  AA 55 | Cmd | Len | data[Len] | sumL | sumH | 55 AA   (sum = Cmd+Len+Σdata)
 *      设备应答 AA 55 | 00 | status | 55 AA                        (status 0 = 成功)
 *   3. 命令：0x81 擦除 → 0x80 编程 → (0x82 校验, 可选) → 0x83 结束。
 *      ERASE/PROM/VERIFY 帧内**不含地址**：设备的 Program_addr / Verify_addr
 *      自 FLASH_Base(0x08006000) 内部自增。END 会补写最后一页并清下载标志，
 *      随后设备复位进入 APP —— 所以 END 必须发，否则设备会一直停在 IAP。
 *
 * 本程序为自由软件；你可以依据 GNU 通用公共许可证（第 2 版或更高）条款
 * 重新发布和/或修改它。
 */

#ifndef PXVIEW_PV_DIALOGS_IAPDIALOG_H
#define PXVIEW_PV_DIALOGS_IAPDIALOG_H

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QString>
#include <cstdint>

/* 与项目其它对话框一致，继承 PxDialog（统一标题栏/阴影/主题/字体）。 */
#include "pv/dialogs/pxdialog.h"

class QThread;
class QLineEdit;
class QPushButton;
class QLabel;
class QTextEdit;
class QProgressBar;
class QCloseEvent;
class QSerialPort;

namespace pv {
namespace dialogs {

/* --- CH32H417 IAP 命令（CDC 串口通道，见下位机 IAP/Common/iap.h） --------- */
#define CH32_IAP_CMD_PROM      0x80   /* 编程：payload = 镜像数据（地址设备内部自增） */
#define CH32_IAP_CMD_ERASE     0x81   /* 擦除：无 payload（重置内部地址） */
#define CH32_IAP_CMD_VERIFY    0x82   /* 校验：payload = 期望数据（地址设备内部自增） */
#define CH32_IAP_CMD_END       0x83   /* 结束：无 payload，设备补页+清标志+复位 */
#define CH32_IAP_CMD_JUMP_IAP  0x84   /* 设备侧保留，主机不发 */

/* 串口帧：AA 55 | Cmd | Len | data[Len] | sumL | sumH | 55 AA
 *   设备应答：AA 55 | 00 | status | 55 AA                   （status 0 = 成功） */
#define CH32_IAP_FRAME_HEAD1   0xAA
#define CH32_IAP_FRAME_HEAD2   0x55
#define CH32_IAP_FRAME_TAIL1   0x55
#define CH32_IAP_FRAME_TAIL2   0xAA

/* 下位机 USBD_DATA_SIZE=128（Cmd+Len+payload 一整包），payload 上限 120。 */
#define CH32_IAP_DATA_LEN      128    /* 命令缓冲总长（含 Cmd/Len 两个字节） */
#define CH32_IAP_MAX_PAYLOAD   120    /* 单包最大 payload（编程/校验共用） */

#define CH32_IAP_TIMEOUT_MS    1000   /* 单条命令响应超时 */
#define CH32_IAP_ERASE_TIMEOUT 5000   /* 擦除响应超时（整片擦除较慢） */
#define CH32_IAP_END_TIMEOUT   500    /* 结束响应超时（设备随即复位，常常无应答） */

/* 编程后是否整片回读校验。
 *
 * **默认 0 = 跳过**，与参考上位机 ALL-LOGIC 一致（其注释：跳过整片回读可省约
 * 一半时间）。安全性不因此下降：
 *   - 每个 PROM 包都有设备应答，写失败会立刻被逐包重试捕获；
 *   - END 前设备执行 IAP_Flush_Remaining() 补写最后一页（不足一页的部分）；
 *   - END 还会清下载标志并复位，失败的话设备会停在 IAP，不会"假装成功"。
 * 需要严格整片校验时置 1（会慢约一倍）。 */
#define CH32_IAP_DO_VERIFY     0

/* 设备 ID（见下位机 APP/Common/usb_desc.h 与 IAP/Common/USB/usb_desc.h） */
#define CH32_APP_VID           0x1A86
#define CH32_APP_PID_USB2      0x5537   /* APP 模式：USB2 High-Speed */
#define CH32_APP_PID_USB3      0x5538   /* APP 模式：USB3 SuperSpeed */
#define CH32_IAP_VID           0x1A86
#define CH32_IAP_CDC_PID       0x5539   /* IAP 模式：CDC 虚拟串口 */

/**
 * 后台升级工作对象：在独立线程里完成
 *   进入 IAP → 擦除 → 编程 → 校验 → 结束。
 * 所有耗时段都检查 cancel() 标志。
 */
class IAPWorker : public QObject
{
    Q_OBJECT

public:
    explicit IAPWorker(const QString &firmware_path, QObject *parent = nullptr);
    ~IAPWorker() override;

    /** 取消升级（线程安全）。 */
    void cancel();

    /** 只执行"进入 IAP"（对应对话框的「进入 IAP」按钮），不做擦写。 */
    void set_enter_iap_only(bool on);

Q_SIGNALS:
    /** 进度百分比(0-100)与状态文本。 */
    void progress_updated(int percent, const QString &status);
    /** 一行日志。 */
    void log_message(const QString &message);
    /** 升级结束。 */
    void finished(bool success);

public Q_SLOTS:
    /** 线程入口：执行整个升级流程。 */
    void do_work();

private:
    /* CDC 串口操作 */
    bool open_serial();
    void close_serial();
    bool serial_write_frame(const uint8_t *data, size_t len);
    bool serial_read_ack(uint8_t *status, int timeout_ms);

    /* IAP 操作 */
    bool enter_iap_mode();
    bool erase_flash(uint32_t start_addr);
    bool program_flash(const uint8_t *data, size_t len);
    bool verify_flash(uint32_t start_addr, const uint8_t *data, size_t len);
    bool end_upgrade();

    /* 固件文件 */
    bool read_firmware(QByteArray &out_data, uint32_t &start_addr);
    bool hex_to_bin(const QByteArray &hex_data, QByteArray &bin_data,
                    uint32_t &start_addr);

    /* 辅助 */
    void log(const QString &message);
    void set_progress(int percent, const QString &status);
    bool is_cancelled();

private:
    QString _firmware_path;

    QSerialPort *_serial;    /* 已打开的 IAP CDC 串口；未打开时为 nullptr */
    bool _cancelled;
    bool _enter_iap_only;    /* true 时只切到 IAP，不擦写（「进入 IAP」按钮） */
    QMutex _mutex;
};

/**
 * 固件升级对话框：选择固件 → 后台线程升级 → 进度/日志。
 */
class IAPDialog : public PxDialog
{
    Q_OBJECT

public:
    explicit IAPDialog(QWidget *parent = nullptr);
    ~IAPDialog() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private Q_SLOTS:
    void on_browse();
    void on_enter_iap();
    void on_start();
    void on_progress(int percent, const QString &status);
    void on_log(const QString &message);
    void on_finished(bool ok);

private:
    void append_log(const QString &message);
    void set_busy(bool busy);

private:
    QLineEdit *_path_edit;
    QPushButton *_browse_btn;
    QPushButton *_enter_iap_btn;
    QPushButton *_start_btn;
    QPushButton *_close_btn;
    QProgressBar *_progress;
    QLabel *_status;
    QTextEdit *_log_view;

    QThread *_thread;
    IAPWorker *_worker;
    bool _busy;
    bool _enter_iap_only;    /* 本次任务只是"进入 IAP"，结束时提示语不同 */
};

} // namespace dialogs
} // namespace pv

#endif // PXVIEW_PV_DIALOGS_IAPDIALOG_H
