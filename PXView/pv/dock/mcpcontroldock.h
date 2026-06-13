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

#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace pv {

class AppControl;

namespace api { class McpTransport; }

namespace dock {

class McpControlDock : public QWidget
{
    Q_OBJECT

public:
    explicit McpControlDock(QWidget *parent = nullptr);

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

    QLabel *_status_label;
    QLabel *_address_label;
    QPushButton *_btn_open_web;
    QPushButton *_btn_restart;
};

} // namespace dock
} // namespace pv

#endif // PXVIEW_PV_MCPCONTROLDOCK_H
