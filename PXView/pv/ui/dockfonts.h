#ifndef DOCKFONTS_H
#define DOCKFONTS_H

#include <QFont>
#include <QApplication>
#include <QFontDatabase>
#include "../config/appconfig.h"

// ATK QML font.pixelSize reference:
//   Panel main title (e.g. "测量", "设置"):  18px
//   Panel section title / sub-heading:       16px
//   Content labels / option text:            14px
//   Sidebar tab text:                        12px
//   Ribbon menu items:                       11px
namespace DockFontSizes
{
    constexpr int MainTitle = 18;
    constexpr int SectionTitle = 16;
    constexpr int Label = 14;
    constexpr int Content = 12;
}

// Ensure a font inherits the global rendering strategy (NoSubpixelAntialias, hinting).
// Call this after any manual setFamily/setWeight/setPointSize that might reset the strategy.
inline void apply_global_font_strategy(QFont &font)
{
    const QFont &appFont = QApplication::font();
    font.setHintingPreference(appFont.hintingPreference());
    font.setStyleStrategy(appFont.styleStrategy());
}

inline int get_dock_font_size(const QString& token, int defaultSize) {
    QString val = AppConfig::Instance().GetThemeTokenValue(token).trimmed();
    // Token values may have "px" suffix (e.g. "12px"), strip it before conversion
    if (val.endsWith("px", Qt::CaseInsensitive))
        val.chop(2);
    bool ok = false;
    int sz = val.toInt(&ok);
    return (ok && sz > 0) ? sz : defaultSize;
}

// --- Dock panel fonts (pixel-perfect ATK alignment) ---

inline QFont dock_font_main_title()
{
    QFont font = QApplication::font();
    font.setPixelSize(get_dock_font_size("@dock-font-main-title", DockFontSizes::MainTitle));
    apply_global_font_strategy(font);
    return font;
}

inline QFont dock_font_section_title()
{
    QFont font = QApplication::font();
    font.setPixelSize(get_dock_font_size("@dock-font-section-title", DockFontSizes::SectionTitle));
    apply_global_font_strategy(font);
    return font;
}

inline QFont dock_font_label()
{
    QFont font = QApplication::font();
    font.setPixelSize(get_dock_font_size("@dock-font-label", DockFontSizes::Label));
    apply_global_font_strategy(font);
    return font;
}

inline QFont dock_font_content()
{
    QFont font = QApplication::font();
    font.setPixelSize(get_dock_font_size("@dock-font-content", DockFontSizes::Content));
    apply_global_font_strategy(font);
    return font;
}

// --- Floating panel (viewport overlay) ---

inline int floating_panel_font_label_size()
{
    return get_dock_font_size("@floating-panel-font-label", 10);
}

inline int floating_panel_font_value_size()
{
    return get_dock_font_size("@floating-panel-font-value", 16);
}

// --- UI chrome fonts (pixel-perfect ATK alignment) ---

inline QFont theme_font_titlebar()
{
    QFont font = QApplication::font();
    font.setPixelSize(get_dock_font_size("@titlebar-font-size", 13));
    apply_global_font_strategy(font);
    return font;
}

inline QFont theme_font_toolbar()
{
    QFont font = QApplication::font();
    font.setPixelSize(get_dock_font_size("@toolbar-font-size", 12));
    apply_global_font_strategy(font);
    return font;
}

inline QFont theme_font_sidebar()
{
    QFont font = QApplication::font();
    font.setPixelSize(get_dock_font_size("@sidebar-font-size", 12));
    apply_global_font_strategy(font);
    return font;
}

inline QFont theme_font_dialog()
{
    QFont font = QApplication::font();
    font.setPixelSize(get_dock_font_size("@dialog-font-size", 12));
    apply_global_font_strategy(font);
    return font;
}

inline QFont theme_font_trace_label()
{
    QFont font;
    font.setFamily("Arial");
    font.setPixelSize(get_dock_font_size("@trace-label-font-size", 10));
    font.setHintingPreference(QFont::PreferVerticalHinting);
    font.setStyleStrategy(QFont::NoAntialias);
    return font;
}

inline QFont theme_font_ruler()
{
    QFont font;
    font.setFamily("Arial");
    font.setPixelSize(get_dock_font_size("@ruler-font-size", 10));
    font.setHintingPreference(QFont::PreferVerticalHinting);
    font.setStyleStrategy(QFont::NoAntialias);
    return font;
}

inline QFont theme_font_decoder()
{
    QFont font;
    font.setFamily("Arial");
    font.setPixelSize(get_dock_font_size("@decoder-font-size", 12));
    font.setHintingPreference(QFont::PreferVerticalHinting);
    font.setStyleStrategy(QFont::NoAntialias);
    return font;
}

inline QFont theme_font_cursor()
{
    QFont font = QApplication::font();
    font.setPixelSize(get_dock_font_size("@cursor-font-size", 10));
    apply_global_font_strategy(font);
    return font;
}



#endif
