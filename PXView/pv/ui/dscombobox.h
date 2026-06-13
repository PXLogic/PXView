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

public:
    DsComboPopup(QComboBox *combo, QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void on_item_clicked();

private:
    QPointer<QComboBox> _combo;
    QList<QPushButton*> _itemButtons;
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
};


#endif // DSCOMBOBOX_H
