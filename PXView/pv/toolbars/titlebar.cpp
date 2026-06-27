/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 * ... (License header omitted for brevity) ...
 */

#include "titlebar.h"
#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QSpacerItem>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOption>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <assert.h>

#include "../appcontrol.h"
#include "../config/appconfig.h"
#include "../ui/langresource.h"
#include "../ui/dockfonts.h"
#include "../dsvdef.h"
#include "../log.h"
#include "../ui/fn.h"
#include "../ui/iconcache.h"

namespace pv {
namespace toolbars {

static qreal cssBezierEasing(qreal t) {
  static const double x1 = 0.4, y1 = 0.0;
  static const double x2 = 0.2, y2 = 1.0;

  double cx = 3.0 * x1;
  double bx = 3.0 * (x2 - x1) - cx;
  double ax = 1.0 - cx - bx;

  double cy = 3.0 * y1;
  double by = 3.0 * (y2 - y1) - cy;
  double ay = 1.0 - cy - by;

  auto sampleCurveX = [&](double s) { return ((ax * s + bx) * s + cx) * s; };
  auto sampleCurveY = [&](double s) { return ((ay * s + by) * s + cy) * s; };

  double s = t;
  for (int i = 0; i < 8; i++) {
    double err = sampleCurveX(s) - t;
    if (qAbs(err) < 1e-7)
      break;
    double deriv = (3.0 * ax * s + 2.0 * bx) * s + cx;
    if (qAbs(deriv) < 1e-10)
      break;
    s -= err / deriv;
  }
  return qBound(0.0, sampleCurveY(s), 1.0);
}

static QEasingCurve makeTailwindCurve() {
  QEasingCurve curve;
  curve.setCustomType(cssBezierEasing);
  return curve;
}

TitleBar::TitleBar(bool top, QWidget *parent, ITitleParent *titleParent,
                   bool hasClose, bool enableRibbon)
    : QMenuBar(parent) {
  _minimizeButton = NULL;
  _maximizeButton = NULL;
  _closeButton = NULL;
  _moving = false;
  _is_draging = false;
  _parent = parent;
  _isTop = top;
  _hasClose = hasClose;
  _title = NULL;
  _is_native = false;
  _titleParent = titleParent;
  _is_done_moved = false;
  _is_able_drag = true;
  _enableRibbon = enableRibbon;
  _ribbonContainer = NULL;
  _ribbonPanel = NULL;
  _ribbonLayout = NULL;
  _categoryStack = NULL;
  _ribbonAnimation = NULL;
  _ribbonExpanded = false;
  _ribbonExpandedHeight = 65;
  _slideOffset = 65;
  _pinButton = NULL;
  _ribbonPinned = true;
  _bottomLine = NULL;

  assert(parent);

  setObjectName(_enableRibbon ? "TitleBar" : "SubTitleBar");
  setContentsMargins(0, 0, 0, 0);

  QWidget *titleRow = new QWidget(this);
  titleRow->setObjectName(_enableRibbon ? "TitleRow" : "SubTitleRow");
  titleRow->setFixedHeight(32);
  titleRow->setContentsMargins(0, 0, 0, 0);

  QHBoxLayout *titleRowLayout = new QHBoxLayout(titleRow);
  titleRowLayout->setContentsMargins(0, 0, 0, 0);
  titleRowLayout->setSpacing(0);

  if (_enableRibbon) {
    _tabBar = new QTabBar(titleRow);
    _tabBar->setDrawBase(false);
    _tabBar->setFixedHeight(32);
    _tabBar->setShape(QTabBar::RoundedNorth);
    _tabBar->setMinimumWidth(100);
    titleRowLayout->addWidget(_tabBar);
  } else {
    _tabBar = NULL;
  }

  titleRowLayout->addStretch(500);

  _title = new QLabel(titleRow);
  _title->setProperty("cssClass", "TitleText");
  _title->setAlignment(Qt::AlignCenter);
  titleRowLayout->addWidget(_title);

  titleRowLayout->addStretch(500);

  if (_isTop) {
    _minimizeButton = new QToolButton(titleRow);
    _minimizeButton->setObjectName("MinimizeButton");
    _minimizeButton->setFixedSize(46, 32);
    _minimizeButton->setIconSize(QSize(16, 16));
    _minimizeButton->setAutoRaise(true);
    _maximizeButton = new QToolButton(titleRow);
    _maximizeButton->setObjectName("MaximizeButton");
    _maximizeButton->setFixedSize(46, 32);
    _maximizeButton->setIconSize(QSize(16, 16));
    _maximizeButton->setAutoRaise(true);

    titleRowLayout->addWidget(_minimizeButton);
    titleRowLayout->addWidget(_maximizeButton);

    connect(_minimizeButton, &QAbstractButton::clicked, parent, [parent]() {
        QMetaObject::invokeMethod(parent, "showMinimized");
    });
    connect(_maximizeButton, &QAbstractButton::clicked, this, &TitleBar::showMaxRestore);
  }

  if (_isTop || _hasClose) {
    _closeButton = new QToolButton(titleRow);
    _closeButton->setObjectName("CloseButton");
    _closeButton->setFixedSize(46, 32);
    _closeButton->setIconSize(QSize(16, 16));
    _closeButton->setAutoRaise(true);
    titleRowLayout->addWidget(_closeButton);
    connect(_closeButton, &QAbstractButton::clicked, parent, &QWidget::close);
  }

  titleRow->setParent(this);
  titleRow->move(0, 0);
  titleRow->show();

  setFixedHeight(32);

  if (_enableRibbon) {
    _bottomLine = new QWidget(this);
    _bottomLine->setObjectName("TitleBarBottomLine");
    _bottomLine->setFixedHeight(1);
    _bottomLine->setAttribute(Qt::WA_StyledBackground, true);
    _bottomLine->move(0, 31);
    _bottomLine->show();
  }

  if (_enableRibbon) {
    _ribbonContainer = new QWidget(parent);
    _ribbonContainer->setObjectName("RibbonContainer");
    _ribbonContainer->setContentsMargins(0, 0, 0, 0);
    _ribbonContainer->setFixedHeight(_ribbonExpandedHeight);
    _ribbonContainer->setAutoFillBackground(false);
    _ribbonContainer->setAttribute(Qt::WA_TranslucentBackground);
    _ribbonContainer->hide();

    _ribbonPanel = new QWidget(this);
    _ribbonPanel->setObjectName("RibbonPanel");
    _ribbonPanel->setContentsMargins(0, 0, 0, 0);
    _ribbonPanel->setFixedSize(1, _ribbonExpandedHeight);
    _ribbonPanel->setAttribute(Qt::WA_StyledBackground);
    _ribbonPanel->move(0, 32);
    _ribbonPanel->hide();

    _ribbonLayout = new QVBoxLayout(_ribbonPanel);
    _ribbonLayout->setContentsMargins(0, 0, 0, 0);
    _ribbonLayout->setSpacing(0);

    _categoryStack = new QStackedWidget(_ribbonPanel);
    _categoryStack->setFrameShape(QFrame::NoFrame);
    _categoryStack->setContentsMargins(0, 0, 0, 0);
    _categoryStack->setFixedHeight(_ribbonExpandedHeight - 1);
    _categoryStack->setMinimumWidth(800);
    _ribbonLayout->addWidget(_categoryStack, 0, Qt::AlignTop);

    QWidget *_ribbonBottomLine = new QWidget(_ribbonPanel);
    _ribbonBottomLine->setObjectName("RibbonBottomLine");
    _ribbonBottomLine->setFixedHeight(1);
    _ribbonBottomLine->setAttribute(Qt::WA_StyledBackground, true);
    _ribbonLayout->addWidget(_ribbonBottomLine);

    _pinButton = new QToolButton(_ribbonPanel);
    _pinButton->setObjectName("RibbonPinButton");
    _pinButton->setFixedSize(20, 20);
    _pinButton->setIconSize(QSize(14, 14));
    _pinButton->setAutoRaise(true);
    _pinButton->setCheckable(true);
    _pinButton->setChecked(true);
    _pinButton->setToolTip(tr("Unpin Ribbon"));
    _pinButton->setIcon(QIcon(GetIconPath() + "/unpin.svg"));
    connect(_pinButton, &QToolButton::toggled, this, &TitleBar::onPinToggled);

    _ribbonAnimation = new QPropertyAnimation(this, "slideOffset");
    _ribbonAnimation->setDuration(250);
    _ribbonAnimation->setEasingCurve(QEasingCurve::OutCubic);

    connect(_ribbonAnimation, &QPropertyAnimation::finished, this, [this]() {
      QWidget *tlw = window();
      if (_ribbonExpanded && _ribbonPinned) {
        if (tlw)
          tlw->setUpdatesEnabled(false);
        _ribbonPanel->setParent(this);
        _ribbonPanel->move(0, 32);
        _ribbonPanel->setFixedWidth(width());
        _ribbonPanel->show();
        setFixedHeight(32 + _ribbonExpandedHeight);
        _ribbonContainer->hide();
        positionPinButton();
        if (tlw)
          tlw->setUpdatesEnabled(true);
      } else if (!_ribbonExpanded) {
        if (tlw)
          tlw->setUpdatesEnabled(false);
        _ribbonContainer->hide();
        if (_ribbonPinned) {
          setFixedHeight(32);
        }
        if (tlw)
          tlw->setUpdatesEnabled(true);
      }
      updateBottomLine();
    });

    connect(_tabBar, &QTabBar::tabBarClicked, this, &TitleBar::onTabClicked);
    connect(_tabBar, &QTabBar::currentChanged, this, &TitleBar::onTabChanged);
  }

  ADD_UI(this);
}

TitleBar::~TitleBar() {
  DESTROY_QT_OBJECT(_minimizeButton);
  DESTROY_QT_OBJECT(_maximizeButton);
  DESTROY_QT_OBJECT(_closeButton);

  REMOVE_UI(this);
}

int TitleBar::addCategory(const QString &title) {
  if (!_enableRibbon || !_tabBar || !_categoryStack)
    return -1;

  int index = _tabBar->addTab(title);

  QWidget *categoryWidget = new QWidget(_categoryStack);
  categoryWidget->setContentsMargins(0, 0, 0, 0);
  QHBoxLayout *categoryLayout = new QHBoxLayout(categoryWidget);
  categoryLayout->setContentsMargins(20, 4, 20, 4); // Reduced top margin to bring icons closer to tabs
  categoryLayout->setSpacing(20); // Match ATK's larger 20px spacing
  categoryLayout->addSpacerItem(new QSpacerItem(1, 1, QSizePolicy::Expanding));

  _categoryStack->addWidget(categoryWidget);
  _categoryLayouts.append(categoryLayout);

  pxv_info("TitleBar::addCategory index=%d title='%s'", index,
           title.toUtf8().constData());

  return index;
}

void TitleBar::addAction(int categoryIndex, QAction *action) {
  if (categoryIndex < 0 || categoryIndex >= _categoryLayouts.size()) {
    pxv_info("TitleBar::addAction invalid categoryIndex=%d", categoryIndex);
    return;
  }

  QHBoxLayout *layout = _categoryLayouts[categoryIndex];

  QToolButton *btn = new QToolButton;
  btn->setProperty("cssClass", "ActionText");
  btn->setIconSize(QSize(32, 32)); // Restored 32x32 size for better visual weight
  btn->setMinimumWidth(64);        // Force uniform width to ensure equal icon spacing
  btn->setAutoRaise(true);
  btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);

  if (action->menu()) {
    btn->setPopupMode(QToolButton::MenuButtonPopup);
  }

  btn->setCheckable(action->isCheckable());
  btn->setChecked(action->isChecked());

  if (action->icon().isNull()) {
    static QIcon defaultIcon(":/icons/light/gear.svg");
    action->setIcon(defaultIcon);
    pxv_info("TitleBar::addAction '%s' icon was NULL, set default",
             action->text().toUtf8().constData());
  }

  btn->setDefaultAction(action);

  int insertPos = layout->count() - 1;
  if (insertPos < 0)
    insertPos = 0;
  layout->insertWidget(insertPos, btn);

  pxv_info("TitleBar::addAction categoryIndex=%d action='%s'", categoryIndex,
           action->text().toUtf8().constData());
}

void TitleBar::addSeparator(int categoryIndex) {
  if (categoryIndex < 0 || categoryIndex >= _categoryLayouts.size()) {
    pxv_info("TitleBar::addSeparator invalid categoryIndex=%d", categoryIndex);
    return;
  }

  QHBoxLayout *layout = _categoryLayouts[categoryIndex];

  QWidget *line = new QWidget();
  line->setObjectName("RibbonSeparator");
  line->setFixedWidth(1);

  int insertPos = layout->count() - 1;
  if (insertPos < 0)
    insertPos = 0;
  layout->insertWidget(insertPos, line);
}

void TitleBar::retranslateUi(int categoryIndex, const QString &title) {
  if (!_enableRibbon || !_tabBar)
    return;

  if (categoryIndex >= 0 && categoryIndex < _tabBar->count()) {
    _tabBar->setTabText(categoryIndex, title);
  }
}

void TitleBar::expandRibbon() {
  if (!_enableRibbon || !_ribbonAnimation)
    return;

  if (_ribbonAnimation->state() == QAbstractAnimation::Running) {
    _ribbonAnimation->stop();
  }

  positionRibbonContainer();
  _ribbonContainer->show();
  _ribbonContainer->raise();

  _ribbonPanel->show();

  _ribbonAnimation->setStartValue(_slideOffset);
  _ribbonAnimation->setEndValue(0);
  _ribbonAnimation->setEasingCurve(QEasingCurve::OutCubic);
  _ribbonAnimation->start();
  _ribbonExpanded = true;

  qApp->installEventFilter(this);
  updateBottomLine();
}

void TitleBar::hideRibbon() {
  if (!_enableRibbon || !_ribbonAnimation)
    return;

  if (_ribbonAnimation->state() == QAbstractAnimation::Running) {
    _ribbonAnimation->stop();
  }

  _ribbonAnimation->setStartValue(_slideOffset);
  _ribbonAnimation->setEndValue(_ribbonExpandedHeight);
  _ribbonAnimation->setEasingCurve(makeTailwindCurve());
  _ribbonAnimation->start();
  _ribbonExpanded = false;

  qApp->removeEventFilter(this);
  updateBottomLine();
}

void TitleBar::expandRibbonPinned() {
  if (!_enableRibbon || !_ribbonAnimation)
    return;

  pxv_info("expandRibbonPinned");

  if (_ribbonAnimation->state() == QAbstractAnimation::Running)
    _ribbonAnimation->stop();

  if (_ribbonPanel->parentWidget() != _ribbonContainer) {
    _ribbonPanel->setParent(_ribbonContainer);
  }

  _ribbonPanel->setFixedSize(_parent->width(), _ribbonExpandedHeight);
  _ribbonPanel->move(0, -_ribbonExpandedHeight);
  _ribbonPanel->show();

  positionRibbonContainer();
  _ribbonContainer->show();
  _ribbonContainer->raise();

  _ribbonAnimation->setStartValue(_ribbonExpandedHeight);
  _ribbonAnimation->setEndValue(0);
  _ribbonAnimation->setEasingCurve(QEasingCurve::OutCubic);
  _ribbonAnimation->start();

  _ribbonExpanded = true;
  updateBottomLine();
}

void TitleBar::hideRibbonPinned() {
  if (!_enableRibbon || !_ribbonAnimation)
    return;

  pxv_info("hideRibbonPinned");

  if (_ribbonAnimation->state() == QAbstractAnimation::Running)
    _ribbonAnimation->stop();

  setFixedHeight(32);

  if (_ribbonPanel->parentWidget() != _ribbonContainer) {
    _ribbonPanel->setParent(_ribbonContainer);
  }

  _ribbonPanel->setFixedSize(_parent->width(), _ribbonExpandedHeight);
  _ribbonPanel->move(0, 0);
  _ribbonPanel->show();

  positionRibbonContainer();
  _ribbonContainer->show();
  _ribbonContainer->raise();

  _ribbonAnimation->setStartValue(0);
  _ribbonAnimation->setEndValue(_ribbonExpandedHeight);
  _ribbonAnimation->setEasingCurve(makeTailwindCurve());
  _ribbonAnimation->start();

  _ribbonExpanded = false;
  updateBottomLine();
}

void TitleBar::positionRibbonContainer() {
  if (!_enableRibbon || !_ribbonContainer || !_parent)
    return;

  int y = mapTo(_parent, QPoint(0, 32)).y();
  _ribbonContainer->move(0, y);
  _ribbonContainer->setFixedWidth(_parent->width());
}

void TitleBar::onTabClicked(int index) {
  if (!_enableRibbon || !_tabBar)
    return;

  if (_ribbonExpanded && index == _tabBar->currentIndex()) {
    _ribbonPinned ? hideRibbonPinned() : hideRibbon();
  } else if (!_ribbonExpanded) {
    _ribbonPinned ? expandRibbonPinned() : expandRibbon();
  }
}

void TitleBar::onTabChanged(int index) {
  if (!_enableRibbon || !_categoryStack)
    return;

  if (index >= 0 && index < _categoryStack->count()) {
    _categoryStack->setCurrentIndex(index);
  }
}

int TitleBar::slideOffset() const { return _slideOffset; }

void TitleBar::setSlideOffset(int offset) {
  _slideOffset = offset;
  if (!_enableRibbon || !_ribbonPanel || !_ribbonContainer)
    return;

  _ribbonPanel->move(0, -offset);
  if (_ribbonPanel->width() != _ribbonContainer->width()) {
    _ribbonPanel->setFixedWidth(_ribbonContainer->width());
  }
  positionPinButton();
}

bool TitleBar::isOnTabBar(const QPoint &pos) const {
  if (!_enableRibbon || !_tabBar)
    return false;

  if (_tabBar->rect().contains(_tabBar->mapFrom(this, pos))) {
    return true;
  }
  if (_ribbonExpanded) {
    if (_ribbonPinned && _ribbonPanel->parent() == this) {
      QPoint posInPanel = _ribbonPanel->mapFrom(this, pos);
      if (_ribbonPanel->rect().contains(posInPanel)) {
        return true;
      }
    } else if (_ribbonContainer->isVisible()) {
      QPoint posInContainer = _ribbonContainer->mapFrom(this, pos);
      if (_ribbonContainer->rect().contains(posInContainer)) {
        return true;
      }
    }
  }
  return false;
}

void TitleBar::reStyle() {
  QString iconPath = GetIconPath();
  QColor iconColor = AppConfig::Instance().GetThemeColor("@titlebar-icon-color");
  
  auto getIcon = [&](const QString& name) {
    return iconColor.isValid() ? IconCache::Instance().tintedIcon(iconPath + name, iconColor) : IconCache::Instance().icon(iconPath + name);
  };
  
  if (_isTop) {
    _minimizeButton->setIcon(getIcon("/minimize.svg"));
    if (ParentIsMaxsized())
      _maximizeButton->setIcon(getIcon("/restore.svg"));
    else
      _maximizeButton->setIcon(getIcon("/maximize.svg"));
  }
  if (_isTop || _hasClose)
    _closeButton->setIcon(getIcon("/close.svg"));

  if (_pinButton && _enableRibbon) {
    if (_ribbonPinned)
      _pinButton->setIcon(getIcon("/unpin.svg"));
    else
      _pinButton->setIcon(getIcon("/pin.svg"));
  }
}

bool TitleBar::ParentIsMaxsized() {
  if (_titleParent != NULL) {
    return _titleParent->ParentIsMaxsized();
  } else {
    return parentWidget()->isMaximized();
  }
}

void TitleBar::paintEvent(QPaintEvent *event) { QMenuBar::paintEvent(event); }

void TitleBar::resizeEvent(QResizeEvent *event) {
  pxv_info("TitleBar::resizeEvent old=%dx%d new=%dx%d minH=%d maxH=%d",
           event->oldSize().width(), event->oldSize().height(),
           event->size().width(), event->size().height(), minimumHeight(),
           maximumHeight());
  QMenuBar::resizeEvent(event);

  // --- DIAGNOSTIC: Check if QSS has stripped font rendering strategy ---
  {
    QFont appFont = QApplication::font();
    pxv_info("FONT DIAG: App font: family='%s' strategy=0x%x hinting=%d",
             appFont.family().toUtf8().constData(),
             (int)appFont.styleStrategy(),
             (int)appFont.hintingPreference());
    if (_title) {
      QFont titleFont = _title->font();
      pxv_info("FONT DIAG: Title label font: family='%s' strategy=0x%x hinting=%d",
               titleFont.family().toUtf8().constData(),
               (int)titleFont.styleStrategy(),
               (int)titleFont.hintingPreference());
    }
    // Check first ribbon action button
    if (!_categoryLayouts.isEmpty()) {
      QHBoxLayout *layout = _categoryLayouts[0];
      for (int i = 0; i < layout->count(); i++) {
        QWidget *w = layout->itemAt(i)->widget();
        if (w) {
          QFont wf = w->font();
          pxv_info("FONT DIAG: Ribbon btn[%d] font: family='%s' strategy=0x%x hinting=%d",
                   i, wf.family().toUtf8().constData(),
                   (int)wf.styleStrategy(),
                   (int)wf.hintingPreference());
          break;
        }
      }
    }
  }

  QWidget *titleRow = findChild<QWidget *>(_enableRibbon ? "TitleRow" : "SubTitleRow");
  if (titleRow) {
    titleRow->setFixedWidth(width());
  }

  if (_enableRibbon && _ribbonContainer && _ribbonContainer->isVisible()) {
    positionRibbonContainer();
  }
  if (_enableRibbon && _ribbonPinned && _ribbonPanel && _ribbonPanel->parentWidget() == this &&
      _ribbonPanel->isVisible()) {
    _ribbonPanel->setFixedWidth(width());
    _ribbonPanel->move(0, 32);
    positionPinButton();
  }

  updateBottomLine();
}

void TitleBar::setTitle(QString title) {
  if (!_is_native) {
    _title->setText(title);
  } else if (_parent != NULL) {
    _parent->setWindowTitle(title);
  }
}

QString TitleBar::title() {
  if (!_is_native) {
    return _title->text();
  } else if (_parent != NULL) {
    return _parent->windowTitle();
  }
  return "";
}

void TitleBar::showMaxRestore() {
    QString iconPath = GetIconPath();
    QColor iconColor = AppConfig::Instance().GetThemeColor("@titlebar-icon-color");
    auto getIcon = [&](const QString& name) {
        return iconColor.isValid() ? IconCache::Instance().tintedIcon(iconPath + name, iconColor) : IconCache::Instance().icon(iconPath + name);
    };

    if (ParentIsMaxsized()) {
        _maximizeButton->setIcon(getIcon("/maximize.svg"));
        if (_titleParent)
            _titleParent->ParentShowNormal();
        else
            parentWidget()->showNormal();
    } else {
        _maximizeButton->setIcon(getIcon("/restore.svg"));
        if (_titleParent)
            _titleParent->ParentShowMaximized();
        else
            parentWidget()->showMaximized();
    }
}

void TitleBar::setRestoreButton(bool max) {
  QString iconPath = GetIconPath();
  QColor iconColor = AppConfig::Instance().GetThemeColor("@titlebar-icon-color");
  auto getIcon = [&](const QString& name) {
    return iconColor.isValid() ? IconCache::Instance().tintedIcon(iconPath + name, iconColor) : IconCache::Instance().icon(iconPath + name);
  };

  if (!max) {
    _maximizeButton->setIcon(getIcon("/maximize.svg"));
  } else {
    _maximizeButton->setIcon(getIcon("/restore.svg"));
  }
}

bool TitleBar::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::MouseButtonPress) {
    QMouseEvent *me = static_cast<QMouseEvent *>(event);
    QPoint globalPos = me->globalPosition().toPoint();

    bool onTitleBar = rect().contains(mapFromGlobal(globalPos));
    bool onRibbon = _enableRibbon && _ribbonContainer && _ribbonContainer->isVisible() &&
                    _ribbonContainer->rect().contains(
                        _ribbonContainer->mapFromGlobal(globalPos));

    if (!onTitleBar && !onRibbon) {
      if (!_ribbonPinned) {
        hideRibbon();
      }
      return true;
    }
  }
  return QMenuBar::eventFilter(watched, event);
}

void TitleBar::mousePressEvent(QMouseEvent *event) {
  if (isOnTabBar(event->position().toPoint())) {
    QMenuBar::mousePressEvent(event);
    return;
  }

  bool ableMove = !ParentIsMaxsized();

  if (event->button() == Qt::LeftButton && ableMove && _is_able_drag) {
    int x = (int)event->position().x();
    int y = (int)event->position().y();

    bool bTopWidow = AppControl::Instance()->GetTopWindow() == _parent;
    bool bClick = (x >= 6 && y >= 5 && x <= width() - 6);

    if (!bTopWidow || bClick) {
      _is_draging = true;

      _clickPos = event->globalPosition().toPoint();

      if (_titleParent != NULL) {
        _oldPos = _titleParent->GetParentPos();
      } else {
        _oldPos = _parent->pos();
      }

      _is_done_moved = false;

      event->accept();
      return;
    }
  }
  QMenuBar::mousePressEvent(event);
}

void TitleBar::mouseMoveEvent(QMouseEvent *event) {
  if (_is_draging) {

    int datX = 0;
    int datY = 0;

    datX = (event->globalPosition().toPoint().x() - _clickPos.x());
    datY = (event->globalPosition().toPoint().y() - _clickPos.y());

    int x = _oldPos.x() + datX;
    int y = _oldPos.y() + datY;

    if (!_moving) {
      if (ABS_VAL(datX) >= 2 || ABS_VAL(datY) >= 2) {
        _moving = true;
      } else {
        return;
      }
    }

    if (_titleParent != NULL) {

      if (!_is_done_moved) {
        _is_done_moved = true;
        _titleParent->MoveBegin();
      }

      _titleParent->MoveWindow(x, y);
    } else {

#ifdef _WIN32

      QRect screenRect = AppControl::Instance()->_screenRect;

      if (screenRect.width() > 0 && QGuiApplication::screens().size() > 1) {

        if (x < screenRect.left()) {
          x = screenRect.left();
        }
        if (x + _parent->frameGeometry().width() > screenRect.right()) {
          x = screenRect.right() - _parent->frameGeometry().width();
        }
      }
#endif

      _parent->move(x, y);
    }

    event->accept();
    return;
  }
  QMenuBar::mouseMoveEvent(event);
}

void TitleBar::mouseReleaseEvent(QMouseEvent *event) {
  if (_moving && _titleParent != NULL) {
    _titleParent->MoveEnd();
  }
  _moving = false;
  _is_draging = false;
  QMenuBar::mouseReleaseEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event) {
  QMenuBar::mouseDoubleClickEvent(event);

  if (_isTop) {

    QTimer::singleShot(200, this, [this]() { showMaxRestore(); });
  }
}

void TitleBar::UpdateLanguage() {}

void TitleBar::UpdateTheme() { reStyle(); }

void TitleBar::UpdateFont() {
  _title->setFont(theme_font_titlebar());
}

void TitleBar::onPinToggled(bool checked) {
  if (!_enableRibbon)
    return;

  pxv_info("onPinToggled: checked=%d", checked);
  _ribbonPinned = checked;
  QString iconPath = GetIconPath();

  if (checked) {
    QColor iconColor = AppConfig::Instance().GetThemeColor("@titlebar-icon-color");
    if (iconColor.isValid())
      _pinButton->setIcon(IconCache::Instance().tintedIcon(iconPath + "/unpin.svg", iconColor));
    else
      _pinButton->setIcon(IconCache::Instance().icon(iconPath + "/unpin.svg"));
    _pinButton->setToolTip(tr("Unpin Ribbon"));
    qApp->removeEventFilter(this);

    if (_ribbonExpanded) {
      if (_ribbonAnimation->state() == QAbstractAnimation::Running)
        _ribbonAnimation->stop();
      _ribbonContainer->hide();
      _ribbonPanel->setParent(this);
      _ribbonPanel->move(0, 32);
      _ribbonPanel->setFixedWidth(width());
      _ribbonPanel->show();
      setFixedHeight(32 + _ribbonExpandedHeight);
      positionPinButton();
    } else {
      _ribbonPanel->setParent(this);
      _ribbonPanel->move(0, 32);
      _ribbonPanel->setFixedWidth(width());
      _ribbonPanel->hide();
    }
  } else {
    QColor iconColor = AppConfig::Instance().GetThemeColor("@titlebar-icon-color");
    if (iconColor.isValid())
      _pinButton->setIcon(IconCache::Instance().tintedIcon(iconPath + "/pin.svg", iconColor));
    else
      _pinButton->setIcon(IconCache::Instance().icon(iconPath + "/pin.svg"));
    _pinButton->setToolTip(tr("Pin Ribbon"));

    if (_ribbonExpanded) {
      qApp->installEventFilter(this);
      if (_ribbonAnimation->state() == QAbstractAnimation::Running)
        _ribbonAnimation->stop();
      setFixedHeight(32);
      _ribbonPanel->setParent(_ribbonContainer);
      _slideOffset = 0;
      _ribbonPanel->move(0, 0);
      _ribbonPanel->setFixedWidth(_ribbonContainer->width());
      _ribbonPanel->show();
      positionRibbonContainer();
      _ribbonContainer->show();
      _ribbonContainer->raise();
    } else {
      _ribbonPanel->setParent(_ribbonContainer);
      _ribbonPanel->move(0, -_ribbonExpandedHeight);
      _ribbonPanel->setFixedWidth(_ribbonContainer->width());
      setFixedHeight(32);
    }
  }
  updateBottomLine();
}

void TitleBar::positionPinButton() {
  if (!_pinButton || !_ribbonPanel || !_enableRibbon)
    return;
  int pinX = _ribbonPanel->width() - _pinButton->width() - 6;
  int pinY = _ribbonPanel->height() - _pinButton->height() - 4;
  _pinButton->move(pinX, pinY);
}

void TitleBar::updateBottomLine() {
  if (!_bottomLine)
    return;
  if (_enableRibbon && _ribbonExpanded) {
    _bottomLine->hide();
  } else {
    _bottomLine->setFixedWidth(width());
    _bottomLine->move(0, height() - 1);
    _bottomLine->show();
  }
}

void TitleBar::EnableAbleDrag(bool bEnabled) { _is_able_drag = bEnabled; }

} // namespace toolbars
} // namespace pv
