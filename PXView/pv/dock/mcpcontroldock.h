/*
 * This file is part of the PXView project.
 * Copyright (C) 2026 DreamSourceLab <support@dreamsourcelab.com>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PXVIEW_PV_MCPCONTROLDOCK_H
#define PXVIEW_PV_MCPCONTROLDOCK_H

#include <QList>
#include <QWidget>
#include "pv/ui/uimanager.h"

class QLabel;
class QPushButton;
class QVBoxLayout;
class AppControl;

namespace pv {

namespace api { class McpTransport; }

namespace dock {

class McpControlDock : public QWidget, public IUiWindow
{
    Q_OBJECT

public:
    explicit McpControlDock(AppControl *app, QWidget *parent = nullptr);
    ~McpControlDock() override;

    void refresh_status();

private slots:
    void on_open_web_console();
    void on_restart_mcp();
    void on_copy_command();

private:
    void setup_ui();
    void add_command_row(QVBoxLayout *parent_layout, const QString &tool_name,
                         const QString &command);
    pv::api::McpTransport* get_mcp_transport() const;
    int get_mcp_port() const;
    void retranslateUi();

    // IUiWindow
    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

    AppControl *_app;
    QLabel *_status_label;
    QLabel *_address_label;
    QPushButton *_btn_open_web;
    QPushButton *_btn_restart;
    QLabel *_section1_title = nullptr;
    QLabel *_section1_desc = nullptr;
    QLabel *_section2_title = nullptr;
    QLabel *_section2_desc = nullptr;
    QLabel *_section3_title = nullptr;
    QLabel *_section3_desc = nullptr;
    QPushButton *_copy_prompt_btn = nullptr;
    QList<QPushButton*> _command_copy_btns;  // 命令行的 "复制" 按钮,语言切换时重译
};

} // namespace dock
} // namespace pv

#endif // PXVIEW_PV_MCPCONTROLDOCK_H
