#ifndef PULSEVIEW_WIDGETS_HOVERSPLITTER_H
#define PULSEVIEW_WIDGETS_HOVERSPLITTER_H

#include <QEvent>
#include <QMouseEvent>
#include <QSplitter>
#include <QEnterEvent>

namespace pv {
namespace widgets {

class HoverSplitter : public QSplitter {
public:
  using QSplitter::QSplitter;

protected:
  QSplitterHandle *createHandle() override {
    class HoverHandle : public QSplitterHandle {
    public:
      HoverHandle(Qt::Orientation o, QSplitter *parent)
          : QSplitterHandle(o, parent) {
        setAttribute(Qt::WA_Hover);
        setMouseTracking(true);
      }

    protected:
      void enterEvent(QEnterEvent *e) override {
        QSplitterHandle::enterEvent(e);
        update();
      }

      void leaveEvent(QEvent *e) override {
        QSplitterHandle::leaveEvent(e);
        update();
      }

      void mousePressEvent(QMouseEvent *e) override {
        QSplitterHandle::mousePressEvent(e);
        update();
      }

      void mouseReleaseEvent(QMouseEvent *e) override {
        QSplitterHandle::mouseReleaseEvent(e);
        update();
      }
    };
    return new HoverHandle(orientation(), this);
  }
};

} // namespace widgets
} // namespace pv

#endif
