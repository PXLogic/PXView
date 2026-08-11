#ifndef DSCOMBOBOX_H
#define DSCOMBOBOX_H

#include <QComboBox>
#include <QDialog>
#include <QList>
#include <QPointer>
#include <QPushButton>

class QWheelEvent;

class DsComboPopup : public QDialog
{
    Q_OBJECT
    friend class DsComboBox;

public:
    DsComboPopup(QComboBox *combo, QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void on_item_clicked();

private:
    QPointer<QComboBox> _combo;
    QList<QPushButton*> _itemButtons;
    bool _bReady = false;  // 延迟就绪标志，防止 show() 过程中的激活切换导致弹窗过早关闭
    int _id = 0;           // 弹窗实例编号，用于调试日志追踪
};

class DsComboBox : public QComboBox
{
public:
    explicit DsComboBox(QWidget *parent = nullptr);

    ~DsComboBox();

public:
    void showPopup() override;

    void hidePopup() override;

    inline bool  IsPopup(){
        return _bPopup;
    }

private:
    void measureSize();

private:
    bool    _bPopup;
    QPointer<DsComboPopup> _popup;  // 跟踪当前弹窗实例，用于重入守卫和同步关闭
    int _popup_seq = 0;             // 弹窗序号生成器，用于调试
};


#endif // DSCOMBOBOX_H
