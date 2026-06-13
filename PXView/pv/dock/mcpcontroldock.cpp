/*
 * This file is part of the PXView project.
 * Copyright (C) 2026 DreamSourceLab <support@dreamsourcelab.com>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "mcpcontroldock.h"
#include "../appcontrol.h"
#include "../api/mcp_transport.h"
#include "../config/appconfig.h"
#include "../ui/langresource.h"
#include "../ui/dockfonts.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QClipboard>
#include <QGuiApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QFrame>

namespace pv {
namespace dock {

static const int MCP_PORT = 10110;

McpControlDock::McpControlDock(QWidget *parent)
    : QWidget(parent)
    , _status_label(nullptr)
    , _address_label(nullptr)
    , _btn_open_web(nullptr)
    , _btn_restart(nullptr)
{
    setup_ui();
    refresh_status();
}

void McpControlDock::setup_ui()
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    QFont title_font = dock_font_section_title();
    QFont content_font = dock_font_content();

    // --- Section 1: Web Console ---
    QLabel *section1_title = new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MCP_WEB_CONSOLE), "Web Console"), this);
    section1_title->setFont(title_font);
    layout->addWidget(section1_title);

    QLabel *section1_desc = new QLabel(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MCP_WEB_CONSOLE_DESC),
            "Open the visual diagnostic interface to control devices using natural language."),
        this);
    section1_desc->setWordWrap(true);
    section1_desc->setFont(content_font);
    layout->addWidget(section1_desc);

    _btn_open_web = new QPushButton(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MCP_OPEN_WEB), "Open MCP Web Console"), this);
    _btn_open_web->setMinimumHeight(36);
    connect(_btn_open_web, &QPushButton::clicked, this, &McpControlDock::on_open_web_console);
    layout->addWidget(_btn_open_web);

    layout->addSpacing(8);

    // --- Section 2: Connect AI Tool ---
    QLabel *section2_title = new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MCP_DEVELOPER), "Connect AI Tool"), this);
    section2_title->setFont(title_font);
    layout->addWidget(section2_title);

    // Status row
    QHBoxLayout *status_layout = new QHBoxLayout();
    _status_label = new QLabel(this);
    _status_label->setFont(content_font);
    status_layout->addWidget(_status_label);
    status_layout->addStretch();
    _address_label = new QLabel(this);
    _address_label->setFont(content_font);
    status_layout->addWidget(_address_label);
    layout->addLayout(status_layout);

    QLabel *section2_desc = new QLabel(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MCP_DEVELOPER_DESC),
            "Run the command below in your terminal to connect your AI tool to PXView."),
        this);
    section2_desc->setWordWrap(true);
    section2_desc->setFont(content_font);
    layout->addWidget(section2_desc);

    // Command rows
    QString port_str = QString::number(MCP_PORT);
    add_command_row(layout, "Claude Code",
        QString("claude mcp add --transport http pxview http://127.0.0.1:%1").arg(port_str));
    add_command_row(layout, "Codex",
        QString("codex mcp add --url http://127.0.0.1:%1 pxview").arg(port_str));
    add_command_row(layout, "OpenCode",
        QString("opencode --mcp http://127.0.0.1:%1").arg(port_str));

    layout->addSpacing(8);

    _btn_restart = new QPushButton(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MCP_RESTART), "Restart MCP Service"), this);
    _btn_restart->setMinimumHeight(36);
    connect(_btn_restart, &QPushButton::clicked, this, &McpControlDock::on_restart_mcp);
    layout->addWidget(_btn_restart);

    layout->addStretch();
}

void McpControlDock::add_command_row(QVBoxLayout *parent_layout, const QString &tool_name,
                                      const QString &command)
{
    QFont content_font = dock_font_content();

    QLabel *name_label = new QLabel(tool_name, this);
    name_label->setFont(content_font);
    parent_layout->addWidget(name_label);

    // Command text in a frame that looks like a code block
    QFrame *cmd_frame = new QFrame(this);
    cmd_frame->setFrameShape(QFrame::StyledPanel);
    cmd_frame->setObjectName("mcpCmdFrame");
    QHBoxLayout *frame_layout = new QHBoxLayout(cmd_frame);
    frame_layout->setContentsMargins(8, 4, 4, 4);

    QLabel *cmd_label = new QLabel(command, this);
    cmd_label->setFont(QFont("Consolas", 9));
    cmd_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cmd_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    frame_layout->addWidget(cmd_label);

    QPushButton *copy_btn = new QPushButton("Copy", this);
    copy_btn->setMinimumHeight(24);
    copy_btn->setFont(QFont("", 8));
    copy_btn->setToolTip(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MCP_COPY_TOOLTIP), "Copy to clipboard"));
    // Store command as property for the slot
    copy_btn->setProperty("cmd_text", command);
    connect(copy_btn, &QPushButton::clicked, this, &McpControlDock::on_copy_command);
    frame_layout->addWidget(copy_btn);

    parent_layout->addWidget(cmd_frame);
}

void McpControlDock::refresh_status()
{
    pv::api::McpTransport *transport = get_mcp_transport();
    bool running = transport && transport->is_running();

    if (running) {
        _status_label->setText(QString::fromUtf8("\xe2\x97\x8f ") +
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MCP_RUNNING), "Running"));
        QColor okColor = AppConfig::Instance().GetThemeColor("@dock-status-ok");
        _status_label->setStyleSheet(QString("color: %1;").arg(okColor.isValid() ? okColor.name() : "#4ec9b0"));
        _address_label->setText(QString("127.0.0.1:%1").arg(MCP_PORT));
    } else {
        _status_label->setText(QString::fromUtf8("\xe2\x97\x8f ") +
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MCP_STOPPED), "Stopped"));
        QColor errColor = AppConfig::Instance().GetThemeColor("@dock-status-error");
        _status_label->setStyleSheet(QString("color: %1;").arg(errColor.isValid() ? errColor.name() : "#f44747"));
        _address_label->setText("-");
    }
}

void McpControlDock::on_open_web_console()
{
    QDesktopServices::openUrl(QUrl(QString("http://127.0.0.1:%1/").arg(MCP_PORT)));
}

void McpControlDock::on_copy_command()
{
    QPushButton *btn = qobject_cast<QPushButton*>(sender());
    if (!btn)
        return;
    QString cmd = btn->property("cmd_text").toString();
    if (!cmd.isEmpty())
        QGuiApplication::clipboard()->setText(cmd);
}

void McpControlDock::on_restart_mcp()
{
    pv::api::McpTransport *transport = get_mcp_transport();
    if (!transport)
        return;

    transport->stop();
    transport->start();
    refresh_status();
}

pv::api::McpTransport* McpControlDock::get_mcp_transport() const
{
    auto *app = ::AppControl::Instance();
    if (!app)
        return nullptr;
    return app->get_mcp_transport();
}

} // namespace dock
} // namespace pv
