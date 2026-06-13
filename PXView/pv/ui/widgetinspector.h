#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>

class QLabel;
class QWidget;

namespace pv {
namespace ui {

class WidgetInspector : public QObject
{
    Q_OBJECT
public:
    static WidgetInspector* Instance();

    void setPickerModeEnabled(bool enabled);
    bool isPickerModeEnabled() const;

signals:
    void widgetPicked(QWidget *widget);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    explicit WidgetInspector(QObject *parent = nullptr);
    ~WidgetInspector();
    
    WidgetInspector(const WidgetInspector&) = delete;
    WidgetInspector& operator=(const WidgetInspector&) = delete;

    void updateTooltip();
    void showTooltip(const QString &text, const QPoint &pos, QWidget *widget);
    void hideTooltip();
    QString buildTooltipText(QWidget *widget);

    bool _pickerModeEnabled;
    QLabel *_tooltipLabel;
    QTimer *_pollTimer;
    QPointer<QWidget> _currentWidget;
};

} // namespace ui
} // namespace pv
