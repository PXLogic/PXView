#include "iconcache.h"
#include "pv/config/appconfig.h"
#include <QPainter>
#include <QColor>
#include <QIconEngine>
#include <QFileInfo>

// Icon auto-tinting mapping: filename (without path) -> theme token
// Category A: accent color icons (originally #1E90FF blue)
// Category B: foreground color icons (window controls, search, etc.)
static const struct { const char *name; const char *token; } kIconTokenMap[] = {
    // Category A — accent color icons
    {"next.svg",           "@icon-accent"},
    {"add.svg",            "@icon-accent"},
    {"gear.svg",           "@icon-accent"},
    {"del.svg",            "@icon-accent"},
    {"open.svg",           "@icon-accent"},
    {"shown.svg",          "@icon-accent"},
    {"hidden.svg",         "@icon-accent"},
    {"save.svg",           "@icon-accent"},
    {"nav.svg",            "@icon-accent"},
    {"pre.svg",            "@icon-accent"},
    {"trigger.svg",        "@icon-accent"},
    {"capture.svg",        "@icon-accent"},
    {"dark.svg",           "@icon-accent"},
    {"light.svg",          "@icon-accent"},
    {"measure.svg",        "@icon-accent"},
    {"about.svg",          "@icon-accent"},
    {"bug.svg",            "@icon-accent"},
    {"display.svg",        "@icon-accent"},
    {"export.svg",         "@icon-accent"},
    {"fft.svg",            "@icon-accent"},
    {"file.svg",           "@icon-accent"},
    {"function.svg",       "@icon-accent"},
    {"log.svg",            "@icon-accent"},
    {"manual.svg",         "@icon-accent"},
    {"math.svg",           "@icon-accent"},
    {"once.svg",           "@icon-accent"},
    {"params.svg",         "@icon-accent"},
    {"protocol.svg",       "@icon-accent"},
    {"repeat.svg",         "@icon-accent"},
    {"search-bar.svg",     "@icon-accent"},
    {"settings.svg",       "@icon-accent"},
    {"sliders.svg",        "@icon-accent"},
    {"ruler.svg",          "@icon-accent"},
    {"binary.svg",         "@icon-accent"},
    {"step-forward.svg",   "@icon-accent"},
    {"scroll-bottom.svg",  "@icon-accent"},
    {"logo_noColor.svg",   "@icon-accent"},
    {"loop.svg",           "@icon-accent"},
    {"update.svg",         "@icon-accent"},
    {"modes.svg",          "@icon-accent"},
    {"moder.svg",          "@icon-accent"},
    {"single.svg",         "@icon-accent"},
    {"osc.svg",            "@icon-special"},
    {"daq.svg",            "@icon-special"},
    {"la.svg",             "@icon-special"},
    {"pwm.svg",            "@icon-special"},
    {"lissajous.svg",      "@icon-accent"},
    {"logo_color.svg",     "@icon-accent"},
    {"usb2.svg",           "@icon-special"},
    {"usb3.svg",           "@icon-special"},
    {"demo.svg",           "@icon-special"},
    {"data.svg",           "@icon-special"},
    {"square-la.svg",      "@icon-special"},
    {"square-daq.svg",     "@icon-special"},
    {"square-osc.svg",     "@icon-special"},
    {"square-pwm.svg",     "@icon-special"},
    {"status-warning.svg", "@icon-special"},

    // Category B — foreground color icons
    {"close.svg",          "@icon-foreground"},
    {"minimize.svg",       "@icon-foreground"},
    {"maximize.svg",       "@icon-foreground"},
    {"restore.svg",        "@icon-foreground"},
    {"pin.svg",            "@icon-foreground"},
    {"unpin.svg",          "@icon-foreground"},
    {"search.svg",         "@icon-accent"},
    {"stop.svg",           "@icon-foreground"},
    {"play.svg",           "@icon-foreground"},
    {"zap.svg",            "@icon-foreground"},
    {"audio-waveform.svg", "@icon-foreground"},
    {"header-expand.svg",  "@icon-foreground"},
    {"header-collapse.svg","@icon-foreground"},
    {"scroll-text.svg",    "@icon-foreground"},

    {nullptr, nullptr} // sentinel
};

static QString lookupIconToken(const QString &fileName)
{
    for (int i = 0; kIconTokenMap[i].name; ++i) {
        if (fileName == QLatin1String(kIconTokenMap[i].name))
            return QLatin1String(kIconTokenMap[i].token);
    }
    return QString();
}

class TintedIconEngine : public QIconEngine {
public:
    TintedIconEngine(const QString &svgPath, const QColor &color)
        : _baseIcon(svgPath), _color(color), _svgPath(svgPath) {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        QPixmap pix = scaledPixmap(rect.size(), mode, state, dpr);
        painter->drawPixmap(rect, pix);
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap pix = _baseIcon.pixmap(size, mode, state);
        if (!pix.isNull() && _color.isValid()) {
            QPainter p(&pix);
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(pix.rect(), _color);
            p.end();
        }
        return pix;
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap pix = _baseIcon.pixmap(size, scale, mode, state);
        if (!pix.isNull() && _color.isValid()) {
            QPainter p(&pix);
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(pix.rect(), _color);
            p.end();
        }
        return pix;
    }

    QIconEngine *clone() const override {
        return new TintedIconEngine(_svgPath, _color);
    }

    QSize actualSize(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return _baseIcon.actualSize(size, mode, state);
    }

private:
    QIcon _baseIcon;
    QColor _color;
    QString _svgPath;
};

class StatefulTintedIconEngine : public QIconEngine {
public:
    StatefulTintedIconEngine(const QString &svgPath, const QColor &normalColor, const QColor &activeColor)
        : _baseIcon(svgPath), _normalColor(normalColor), _activeColor(activeColor), _svgPath(svgPath) {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        QPixmap pix = scaledPixmap(rect.size(), mode, state, dpr);
        painter->drawPixmap(rect, pix);
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap pix = _baseIcon.pixmap(size, mode, state);
        QColor color = (mode == QIcon::Active || mode == QIcon::Selected) ? _activeColor : _normalColor;
        if (!pix.isNull() && color.isValid()) {
            QPainter p(&pix);
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(pix.rect(), color);
            p.end();
        }
        return pix;
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap pix = _baseIcon.pixmap(size, scale, mode, state);
        QColor color = (mode == QIcon::Active || mode == QIcon::Selected) ? _activeColor : _normalColor;
        if (!pix.isNull() && color.isValid()) {
            QPainter p(&pix);
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(pix.rect(), color);
            p.end();
        }
        return pix;
    }

    QIconEngine *clone() const override {
        return new StatefulTintedIconEngine(_svgPath, _normalColor, _activeColor);
    }

    QSize actualSize(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return _baseIcon.actualSize(size, mode, state);
    }

private:
    QIcon _baseIcon;
    QColor _normalColor;
    QColor _activeColor;
    QString _svgPath;
};

IconCache::IconCache()
{
}

IconCache::~IconCache()
{
}

IconCache &IconCache::Instance()
{
    static IconCache *ins = nullptr;
    if (!ins)
        ins = new IconCache();
    return *ins;
}

QIcon IconCache::icon(const QString &svgPath)
{
    // Extract filename from path for token lookup
    QString fileName = QFileInfo(svgPath).fileName();
    QString token = lookupIconToken(fileName);

    if (!token.isEmpty()) {
        QColor color = AppConfig::Instance().GetThemeColor(token);
        if (color.isValid())
            return tintedIcon(svgPath, color);
    }

    // No mapping or invalid token — use original icon
    auto it = _iconCache.find(svgPath);
    if (it != _iconCache.end())
        return it.value();

    QIcon ic(svgPath);
    _iconCache.insert(svgPath, ic);
    return ic;
}

QIcon IconCache::tintedIcon(const QString &svgPath, const QColor &color, const QSize &size)
{
    Q_UNUSED(size);
    QString key = svgPath + "_" + color.name();
    auto it = _iconCache.find(key);
    if (it != _iconCache.end())
        return it.value();

    QIcon ic(svgPath);
    if (ic.isNull()) {
        _iconCache.insert(key, ic);
        return ic;
    }

    QIcon tinted(new TintedIconEngine(svgPath, color));
    _iconCache.insert(key, tinted);
    return tinted;
}

QIcon IconCache::statefulTintedIcon(const QString &svgPath, const QColor &normalColor, const QColor &activeColor)
{
    QString key = svgPath + "_stateful_" + normalColor.name() + "_" + activeColor.name();
    auto it = _iconCache.find(key);
    if (it != _iconCache.end())
        return it.value();

    QIcon ic(svgPath);
    if (ic.isNull()) {
        _iconCache.insert(key, ic);
        return ic;
    }

    QIcon tinted(new StatefulTintedIconEngine(svgPath, normalColor, activeColor));
    _iconCache.insert(key, tinted);
    return tinted;
}

QPixmap IconCache::pixmap(const QString &svgPath, const QSize &size)
{
    return icon(svgPath).pixmap(size);
}

void IconCache::clearCache()
{
    _iconCache.clear();
}
