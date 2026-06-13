#include "widgetinspector.h"

#include <QApplication>
#include <QMouseEvent>
#include <QCursor>
#include <QScreen>
#include <QLabel>
#include <QDebug>
#include <QPushButton>
#include <QToolButton>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QDockWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QListWidget>
#include <QTabWidget>
#include <QGroupBox>
#include <QMenu>

namespace pv {
namespace ui {

static WidgetInspector *s_instance = nullptr;

static QString getWidgetTypeDesc(QWidget *widget)
{
    if (!widget) return "Unknown";

    if (qobject_cast<QPushButton*>(widget)) return "Button";
    if (qobject_cast<QToolButton*>(widget)) return "ToolButton";
    if (qobject_cast<QLineEdit*>(widget)) return "LineEdit";
    if (qobject_cast<QComboBox*>(widget)) return "ComboBox";
    if (qobject_cast<QCheckBox*>(widget)) return "CheckBox";
    if (qobject_cast<QRadioButton*>(widget)) return "RadioButton";
    if (qobject_cast<QSlider*>(widget)) return "Slider";
    if (qobject_cast<QSpinBox*>(widget)) return "SpinBox";
    if (qobject_cast<QDoubleSpinBox*>(widget)) return "DoubleSpinBox";
    if (qobject_cast<QTextEdit*>(widget)) return "TextEdit";
    if (qobject_cast<QPlainTextEdit*>(widget)) return "PlainTextEdit";
    if (qobject_cast<QTableWidget*>(widget)) return "TableWidget";
    if (qobject_cast<QTreeWidget*>(widget)) return "TreeWidget";
    if (qobject_cast<QListWidget*>(widget)) return "ListWidget";
    if (qobject_cast<QTabWidget*>(widget)) return "TabWidget";
    if (qobject_cast<QGroupBox*>(widget)) return "GroupBox";
    if (qobject_cast<QLabel*>(widget)) return "Label";
    if (qobject_cast<QMenu*>(widget)) return "Menu";
    if (qobject_cast<QDockWidget*>(widget)) return "DockWidget";

    return "Widget";
}

WidgetInspector::WidgetInspector(QObject *parent)
    : QObject(parent),
      _pickerModeEnabled(false),
      _tooltipLabel(nullptr)
{
    _pollTimer = new QTimer(this);
    _pollTimer->setInterval(50);
    connect(_pollTimer, &QTimer::timeout, this, &WidgetInspector::updateTooltip);
}

WidgetInspector::~WidgetInspector()
{
    if (_tooltipLabel) {
        _tooltipLabel->deleteLater();
    }
}

WidgetInspector* WidgetInspector::Instance()
{
    if (!s_instance) {
        s_instance = new WidgetInspector();
    }
    return s_instance;
}

void WidgetInspector::setPickerModeEnabled(bool enabled)
{
    if (_pickerModeEnabled == enabled)
        return;

    _pickerModeEnabled = enabled;

    if (_pickerModeEnabled) {
        QApplication::setOverrideCursor(Qt::CrossCursor);
        _pollTimer->start();
        qApp->installEventFilter(this);
    } else {
        _pollTimer->stop();
        qApp->removeEventFilter(this);
        QApplication::restoreOverrideCursor();
        hideTooltip();
    }
}

bool WidgetInspector::isPickerModeEnabled() const
{
    return _pickerModeEnabled;
}

bool WidgetInspector::eventFilter(QObject *obj, QEvent *event)
{
    Q_UNUSED(obj);
    if (!_pickerModeEnabled)
        return false;

    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
        if (event->type() == QEvent::MouseButtonPress) {
            QWidget *w = QApplication::widgetAt(QCursor::pos());
            if (w) {
                emit widgetPicked(w);
            }
            // Auto exit picker mode
            setPickerModeEnabled(false);
        }
        return true; // Block click
    }

    return false;
}

void WidgetInspector::updateTooltip()
{
    QWidget *w = QApplication::widgetAt(QCursor::pos());
    if (w) {
        if (w != _currentWidget) {
            _currentWidget = w;
        }
        QPoint pos = QCursor::pos();
        pos.setX(pos.x() + 15);
        pos.setY(pos.y() + 15);
        showTooltip(buildTooltipText(w), pos, w);
    } else {
        hideTooltip();
    }
}

void WidgetInspector::showTooltip(const QString &text, const QPoint &pos, QWidget *widget)
{
    (void)widget;
    if (!_tooltipLabel) {
        _tooltipLabel = new QLabel();
        _tooltipLabel->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        _tooltipLabel->setAttribute(Qt::WA_ShowWithoutActivating);
        _tooltipLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        _tooltipLabel->setMargin(4);
    }

    _tooltipLabel->setText(text);
    
    QScreen *screen = QApplication::screenAt(pos);
    if (screen) {
        QRect screenRect = screen->geometry();
        int x = pos.x();
        int y = pos.y();
        
        if (x + _tooltipLabel->width() > screenRect.right()) {
            x = pos.x() - _tooltipLabel->width() - 5;
        }
        if (y + _tooltipLabel->height() > screenRect.bottom()) {
            y = pos.y() - _tooltipLabel->height() - 5;
        }
        _tooltipLabel->move(x, y);
    } else {
        _tooltipLabel->move(pos);
    }

    if (_tooltipLabel->isHidden()) {
        _tooltipLabel->show();
    }
}

void WidgetInspector::hideTooltip()
{
    if (_tooltipLabel && !_tooltipLabel->isHidden()) {
        _tooltipLabel->hide();
    }
}

QString WidgetInspector::buildTooltipText(QWidget *widget)
{
    if (!widget) return "";

    QString className = widget->metaObject()->className();
    QString typeDesc = getWidgetTypeDesc(widget);
    QString objName = widget->objectName();

    QStringList classes;
    classes << className;
    
    const QMetaObject *meta = widget->metaObject()->superClass();
    while (meta && QString(meta->className()) != "QWidget") {
        classes << meta->className();
        meta = meta->superClass();
    }

    QString text = QString("<b>%1</b> (%2)").arg(className, typeDesc);
    if (!objName.isEmpty()) {
        text += QString("<br/>ID: <span style='color:#3daee9;'>#%1</span>").arg(objName);
    }
    
    text += QString("<br/><br/><i>Inherits:</i><br/><span style='color:#888888;'>%1</span>")
            .arg(classes.join(" &rarr; "));

    return text;
}

} // namespace ui
} // namespace pv
