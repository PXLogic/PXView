/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <math.h>
#include <iterator>

#include "measuredock.h"
#include "../sigsession.h"
#include "../view/cursor.h"
#include "../view/view.h"
#include "../view/viewport.h"
#include "../view/timemarker.h"
#include "../view/ruler.h"
#include "../view/logicsignal.h"
#include "../data/signaldata.h"
#include "../data/snapshot.h" 
#include "../dialogs/dsdialog.h"
#include "../dialogs/dsmessagebox.h"
#include "../config/appconfig.h"
#include "../ui/langresource.h"
#include "../ui/msgbox.h"
#include <QObject>
#include <QPainter> 
#include "../appcontrol.h"
#include "../ui/fn.h"
#include "../log.h"
#include "../ui/xtoolbutton.h"
#include "../tabcontext.h"
#include "../data/sessiondocument.h"
#include "../ui/dockfonts.h"
#include "../ui/iconcache.h"

using namespace boost;

namespace pv {
namespace dock {

using namespace pv::view;

class NoteLabel : public QLabel {
public:
    explicit NoteLabel(QWidget* parent = nullptr) : QLabel(parent) {}
    QSize sizeHint() const override {
        return QSize(10, 10);
    }
    QSize minimumSizeHint() const override {
        return QSize(10, 10);
    }
    bool hasHeightForWidth() const override {
        return true;
    }
    int heightForWidth(int w) const override {
        return QLabel::heightForWidth(w);
    }
};

MeasureDock::MeasureDock(QWidget *parent, View *view, SigSession *session) :
    pv::widgets::SmoothScrollArea(parent),
    _session(session),
    _view(view),
    _context(nullptr)
{     
    _widget = new QWidget(this);  

    _dist_pannel = NULL;
    _edge_pannel = NULL;
    _bSetting = false;

    _mouse_section = new QWidget(_widget);
    _mouse_title = new QLabel(_mouse_section);
    _mouse_title->setObjectName("dock_section_title");
    _fen_checkBox = new QCheckBox(_mouse_section);
    _fen_checkBox->setChecked(true);
    _width_label = new QLabel(_mouse_section);
    _width_label->setObjectName("dock_label");
    _period_label = new QLabel(_mouse_section);
    _period_label->setObjectName("dock_label");
    _freq_label = new QLabel(_mouse_section);
    _freq_label->setObjectName("dock_label");
    _duty_label = new QLabel(_mouse_section);
    _duty_label->setObjectName("dock_label");

    _panel_alpha_note = new NoteLabel(_widget);
    _panel_alpha_note->setObjectName("dock_label");
    _panel_alpha_note->setWordWrap(true);
    _panel_alpha_note->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    _w_label = new QLabel(_mouse_section);
    _w_label->setObjectName("dock_label");
    _p_label = new QLabel(_mouse_section);
    _p_label->setObjectName("dock_label");
    _f_label = new QLabel(_mouse_section);
    _f_label->setObjectName("dock_label");
    _d_label = new QLabel(_mouse_section);
    _d_label->setObjectName("dock_label");

    QGridLayout *mouse_layout = new QGridLayout();
    mouse_layout->setVerticalSpacing(5);
    mouse_layout->setHorizontalSpacing(5);
    mouse_layout->addWidget(_fen_checkBox, 0, 0, 1, 5);
    mouse_layout->addWidget(_w_label, 1, 0);
    mouse_layout->addWidget(_width_label, 1, 1);
    mouse_layout->addWidget(_p_label, 1, 3);
    mouse_layout->addWidget(_period_label, 1, 4);

    mouse_layout->addWidget(_d_label, 2, 0);
    mouse_layout->addWidget(_duty_label, 2, 1);
    mouse_layout->addWidget(_f_label, 2, 3);
    mouse_layout->addWidget(_freq_label, 2, 4);

    mouse_layout->setContentsMargins(5, 2, 5, 5);
    QVBoxLayout *mouse_vbox = new QVBoxLayout(_mouse_section);
    mouse_vbox->setContentsMargins(0, 0, 0, 0);
    mouse_vbox->setSpacing(0);
    mouse_vbox->addWidget(_mouse_title);
    mouse_vbox->addLayout(mouse_layout);

    _dist_section = new QWidget(_widget);
    _dist_title = new QLabel(_dist_section);
    _dist_title->setObjectName("dock_section_title");

    _dist_add_btn = new XToolButton(_dist_section);   

    _dist_layout = new QGridLayout();
    _dist_layout->setVerticalSpacing(5);
    _dist_layout->addWidget(_dist_add_btn, 0, 0);
    _dist_layout->addWidget(new QLabel(_dist_section), 0, 1, 1, 3);
    _add_dec_label = new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TIME_SAMPLES), "Time/Samples"), _dist_section);
    _add_dec_label->setObjectName("dock_label");
    _dist_layout->addWidget(_add_dec_label, 0, 4);
    _dist_layout->addWidget(new QLabel(_dist_section), 0, 5, 1, 2);
    _dist_layout->setColumnStretch(1, 50);
    _dist_layout->setColumnStretch(6, 100);
    _dist_layout->setContentsMargins(5, 2, 5, 5);
    QVBoxLayout *dist_vbox = new QVBoxLayout(_dist_section);
    dist_vbox->setContentsMargins(0, 0, 0, 0);
    dist_vbox->setSpacing(0);
    dist_vbox->addWidget(_dist_title);
    dist_vbox->addLayout(_dist_layout);

    _edge_section = new QWidget(_widget);
    _edge_title = new QLabel(_edge_section);
    _edge_title->setObjectName("dock_section_title");
    _edge_add_btn = new XToolButton(_edge_section);

    _channel_label = new QLabel(_edge_section);
    _channel_label->setObjectName("dock_label");
    _edge_label = new QLabel(_edge_section);
    _edge_label->setObjectName("dock_label");
    _edge_layout = new QGridLayout();
    _edge_layout->setVerticalSpacing(5);
    _edge_layout->addWidget(_edge_add_btn, 0, 0);
    _edge_layout->addWidget(new QLabel(_edge_section), 0, 1, 1, 4);
    _edge_layout->addWidget(_channel_label, 0, 5);
    _edge_layout->addWidget(_edge_label, 0, 6);
    _edge_layout->setColumnStretch(1, 50);
    _edge_layout->setContentsMargins(5, 2, 5, 5);
    QVBoxLayout *edge_vbox = new QVBoxLayout(_edge_section);
    edge_vbox->setContentsMargins(0, 0, 0, 0);
    edge_vbox->setSpacing(0);
    edge_vbox->addWidget(_edge_title);
    edge_vbox->addLayout(_edge_layout);

    _cursor_section = new QWidget(_widget);
    _cursor_title = new QLabel(_cursor_section);
    _cursor_title->setObjectName("dock_section_title");
    _time_label = new QLabel(_cursor_section);
    _time_label->setObjectName("dock_label");
    _cursor_layout = new QGridLayout();
    _cursor_layout->addWidget(_time_label, 0, 2);
    _cursor_layout->addWidget(new QLabel(_cursor_section), 0, 3);
    _cursor_layout->setColumnStretch(3, 1);
    _cursor_layout->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    _cursor_layout->setContentsMargins(5, 2, 5, 5);
    QVBoxLayout *cursor_vbox = new QVBoxLayout(_cursor_section);
    cursor_vbox->setContentsMargins(0, 0, 0, 0);
    cursor_vbox->setSpacing(0);
    cursor_vbox->addWidget(_cursor_title);
    cursor_vbox->addLayout(_cursor_layout);

    QVBoxLayout *layout = new QVBoxLayout(_widget);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->addWidget(_mouse_section);
    layout->addWidget(_panel_alpha_note);
    QFrame *sep1 = new QFrame(_widget);
    sep1->setObjectName("dock_section_separator");
    sep1->setFrameShape(QFrame::HLine);
    layout->addWidget(sep1);
    layout->addWidget(_dist_section);
    _edge_sep = new QFrame(_widget);
    _edge_sep->setObjectName("dock_section_separator");
    _edge_sep->setFrameShape(QFrame::HLine);
    layout->addWidget(_edge_sep);
    layout->addWidget(_edge_section);
    QFrame *sep3 = new QFrame(_widget);
    sep3->setObjectName("dock_section_separator");
    sep3->setFrameShape(QFrame::HLine);
    layout->addWidget(sep3);
    layout->addWidget(_cursor_section);
    layout->addStretch(1);

    this->setFrameShape(QFrame::NoFrame);
    this->setObjectName("dock_measure_scroll");
    this->setWidgetResizable(true);
    this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->setWidget(_widget);
    _widget->setGeometry(0, 0, sizeHint().width(), 2000);
    _widget->setObjectName("measureWidget");

    add_dist_measure();

    connect(_dist_add_btn, &QAbstractButton::clicked, this, &MeasureDock::add_dist_measure);
    connect(_edge_add_btn, &QAbstractButton::clicked, this, &MeasureDock::add_edge_measure);
    connect(_fen_checkBox, &QCheckBox::checkStateChanged, _view, &view::View::set_measure_en);
    connect(_view, &view::View::measure_updated, this, &MeasureDock::measure_updated);
    connect(_view, &view::View::cursor_update, this, &MeasureDock::cursor_update);
    connect(_view, &view::View::cursor_moving, this, &MeasureDock::cursor_moving);
    connect(_view, &view::View::cursor_moved, this, &MeasureDock::reCalc);

    ADD_UI(this);
}

MeasureDock::~MeasureDock()
{
    REMOVE_UI(this);
}

void MeasureDock::set_view(view::View *view)
{
    if (_view) {
        disconnect(_view, &view::View::cursor_update, this, &MeasureDock::cursor_update);
        disconnect(_view, &view::View::cursor_moving, this, &MeasureDock::cursor_moving);
        disconnect(_view, &view::View::cursor_moved, this, &MeasureDock::reCalc);
        disconnect(_view, &view::View::measure_updated, this, &MeasureDock::measure_updated);
        disconnect(_fen_checkBox, &QCheckBox::checkStateChanged, _view, &view::View::set_measure_en);
    }

    _view = view;

    if (_view) {
        connect(_view, &view::View::cursor_update, this, &MeasureDock::cursor_update);
        connect(_view, &view::View::cursor_moving, this, &MeasureDock::cursor_moving);
        connect(_view, &view::View::cursor_moved, this, &MeasureDock::reCalc);
        connect(_view, &view::View::measure_updated, this, &MeasureDock::measure_updated);
        connect(_fen_checkBox, &QCheckBox::checkStateChanged, _view, &view::View::set_measure_en);
    }
}

void MeasureDock::bind_context(TabContext *ctx)
{
    assert(ctx);
    _context = ctx;
    _session = ctx->session();
    set_view(ctx->view());
    reload();
    if (ctx && ctx->document()) {
        _fen_checkBox->setChecked(ctx->document()->_dock_measure_fen_enabled);

        auto &dist_rows = ctx->document()->_dock_measure_dist_rows;
        auto &edge_rows = ctx->document()->_dock_measure_edge_rows;

        if ((dist_rows.size() > 0 || edge_rows.size() > 0) && _session->get_device() && _session->get_device()->have_instance()) {
            auto mode_rows = get_mode_rows();

            if (mode_rows) {
                mode_rows->_dist_row_list.clear();
                for (int i = 0; i < dist_rows.size(); i++) {
                    QJsonObject row_obj = dist_rows[i].toObject();
                    cursor_row_info inf;
                    inf.cursor1 = row_obj["cursor1"].toInt();
                    inf.cursor2 = row_obj["cursor2"].toInt();
                    inf.channelIndex = 0;
                    inf.del_bt = NULL;
                    inf.start_bt = NULL;
                    inf.end_bt = NULL;
                    inf.r_label = NULL;
                    inf.box = NULL;
                    mode_rows->_dist_row_list.push_back(inf);
                }

                mode_rows->_edge_row_list.clear();
                for (int i = 0; i < edge_rows.size(); i++) {
                    QJsonObject row_obj = edge_rows[i].toObject();
                    cursor_row_info inf;
                    inf.cursor1 = row_obj["cursor1"].toInt();
                    inf.cursor2 = row_obj["cursor2"].toInt();
                    inf.channelIndex = row_obj["channelIndex"].toInt();
                    inf.del_bt = NULL;
                    inf.start_bt = NULL;
                    inf.end_bt = NULL;
                    inf.r_label = NULL;
                    inf.box = NULL;
                    mode_rows->_edge_row_list.push_back(inf);
                }

                build_dist_pannel();
                build_edge_pannel();
            }
        }
    }
}

void MeasureDock::unbind_context()
{
    if (_context && _context->document() && _session && _session->get_device() && _session->get_device()->have_instance()) {
        _context->document()->_dock_measure_fen_enabled = _fen_checkBox->isChecked();

        QJsonArray dist_rows;
        QJsonArray edge_rows;
        auto mode_rows = get_mode_rows();

        if (mode_rows) {
            for (auto &inf : mode_rows->_dist_row_list) {
                QJsonObject row_obj;
                row_obj["cursor1"] = inf.cursor1;
                row_obj["cursor2"] = inf.cursor2;
                dist_rows.append(row_obj);
            }

            for (auto &inf : mode_rows->_edge_row_list) {
                QJsonObject row_obj;
                row_obj["cursor1"] = inf.cursor1;
                row_obj["cursor2"] = inf.cursor2;
                row_obj["channelIndex"] = inf.channelIndex;
                edge_rows.append(row_obj);
            }
        }

        _context->document()->_dock_measure_dist_rows = dist_rows;
        _context->document()->_dock_measure_edge_rows = edge_rows;
    }
    set_view(nullptr);
    _context = nullptr;

    for (int i = 0; i < MODE_ROWS_LENGTH; i++) {
        _mode_rows[i]._dist_row_list.clear();
        _mode_rows[i]._edge_row_list.clear();
        for (auto &row : _mode_rows[i]._opt_row_list) {
            if (row.del_bt != NULL) {
                row.del_bt->deleteLater();
                row.goto_bt->deleteLater();
                row.info_label->deleteLater();
                row.del_bt = NULL;
                row.goto_bt = NULL;
                row.info_label = NULL;
            }
        }
        _mode_rows[i]._opt_row_list.clear();
    }
}

void MeasureDock::retranslateUi()
{
    _mouse_title->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MOUSE_MEASUREMENT), "Mouse measurement"));
    _fen_checkBox->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE_FLOATING_MEASUREMENT), "Enable floating measurement"));
    _panel_alpha_note->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PANEL_ALPHA_NOTE), "*Adjust the alpha channel of \"Panel Background\" in Settings > Style > Global Colors to change panel transparency"));
    _dist_title->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CURSOR_DISTANCE), "Cursor Distance"));
    _edge_title->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_EDGES), "Edges"));
    _cursor_title->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CURSORS), "Cursors"));

    _channel_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL), "Channel"));
    _edge_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RIS_OR_FAL_EDGE), "Rising/Falling/Edges"));
    _time_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TIME_SAMPLES), "Time/Samples"));
    _add_dec_label->setText(_time_label->text());

    /*
    _w_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_W), "W: "));
    _p_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_P), "P: "));
    _f_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_F), "F: "));
    _d_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_D), "D: "));
    */

    _w_label->setText("W:");
    _p_label->setText("P:");
    _f_label->setText("F:");
    _d_label->setText("D:");
}

void MeasureDock::reStyle()
{
    QString iconPath = GetIconPath();

    _dist_add_btn->setIcon(IconCache::Instance().icon(iconPath+"/add.svg"));
    _edge_add_btn->setIcon(IconCache::Instance().icon(iconPath+"/add.svg"));

    auto mode_rows = get_mode_rows();

    for (auto it = mode_rows->_dist_row_list.begin(); it != mode_rows->_dist_row_list.end(); it++)
    {
        (*it).del_bt->setIcon(IconCache::Instance().icon(iconPath+"/del.svg"));
    }

    for (auto it = mode_rows->_edge_row_list.begin(); it != mode_rows->_edge_row_list.end(); it++)
    {
        (*it).del_bt->setIcon(IconCache::Instance().icon(iconPath+"/del.svg"));
    }

    for (auto it = mode_rows->_opt_row_list.begin(); it != mode_rows->_opt_row_list.end(); it++)
    {
        (*it).del_bt->setIcon(IconCache::Instance().icon(iconPath+"/del.svg"));
    }

    update_dist();
}

void MeasureDock::reload()
{
    bool isLogic = _session->get_device()->get_work_mode() == LOGIC;
    _edge_section->setVisible(isLogic);
    _edge_sep->setVisible(isLogic);

    _bSetting = true;

    build_dist_pannel();
    build_edge_pannel();
    build_cursor_pannel();

    _bSetting = false;

    reCalc();
}

void MeasureDock::measure_updated()
{
    _width_label->setText(_view->get_measure("width"));
    _period_label->setText(_view->get_measure("period"));
    _freq_label->setText(_view->get_measure("frequency"));
    _duty_label->setText(_view->get_measure("duty"));
    adjusLabelSize();
}

void MeasureDock::build_dist_pannel()
{
    if (_dist_pannel != NULL){
        _dist_pannel->deleteLater();
        _dist_pannel = NULL;
    }

    QFont labelFont = dock_font_label();
    QFont contentFont = dock_font_content();

    QGridLayout  *lay = new QGridLayout();
    _dist_pannel = new QWidget();    
    _dist_pannel->setLayout(lay);
    lay->setColumnStretch(1, 50);
    lay->setColumnStretch(6, 100);
    lay->setVerticalSpacing(5);
    lay->setContentsMargins(0,0,0,0);
    
    int dex = 0;
    QLabel cal_lb;
    cal_lb.setFont(contentFont);

    int bt_w = cal_lb.fontMetrics().horizontalAdvance("22") + 8;


    auto mode_rows = get_mode_rows();

    for (auto &o : mode_rows->_dist_row_list)
    {
        QWidget *row_widget = new QWidget(_widget);
        row_widget->setContentsMargins(0,0,0,0);
        QHBoxLayout *row_layout = new QHBoxLayout(row_widget);
        row_layout->setAlignment(Qt::AlignLeft);
        row_layout->setContentsMargins(0,0,0,0);
        row_layout->setSpacing(0);
        row_widget->setLayout(row_layout);

        QString iconPath = GetIconPath();
        XToolButton *del_btn = new XToolButton(row_widget);
        del_btn->setIcon(IconCache::Instance().icon(iconPath+"/del.svg"));
        del_btn->setCheckable(true);
        //tr
        QPushButton *s_btn = new QPushButton("", row_widget);
        //tr
        QPushButton *e_btn = new QPushButton("", row_widget);
        QLabel *r_label = new QLabel(row_widget);
        //tr
        QLabel *g_label = new QLabel("-", row_widget);
        g_label->setContentsMargins(0,0,0,0);

        row_layout->addWidget(del_btn);
        row_layout->addSpacing(5);
        row_layout->addWidget(s_btn);
        row_layout->addWidget(g_label);
        row_layout->addWidget(e_btn);
        row_layout->addSpacing(5);
        row_layout->addWidget(r_label, 100);

        r_label->setObjectName("dock_label");
        g_label->setObjectName("dock_label");
        s_btn->setObjectName("dock_content");
        e_btn->setObjectName("dock_content");

        r_label->setFont(labelFont);
        s_btn->setFont(contentFont);
        e_btn->setFont(contentFont);
        g_label->setFont(labelFont);

        s_btn->setFixedWidth(bt_w);
        e_btn->setFixedWidth(bt_w);

        lay->addWidget(row_widget, dex++, 0, 1, 7);

        if (o.r_label != NULL){
            r_label->setText(o.r_label->text());
        }

        o.del_bt = del_btn;
        o.start_bt = s_btn;
        o.end_bt = e_btn;
        o.r_label = r_label;

        if (o.cursor1 != -1){
            o.start_bt->setText(QString::number(o.cursor1));
            set_cursor_btn_color(o.start_bt);
        }
        if (o.cursor2 != -1){
            o.end_bt->setText(QString::number(o.cursor2));
            set_cursor_btn_color(o.end_bt);
        }

        connect(del_btn, &QPushButton::clicked, this, &MeasureDock::del_dist_measure);
        connect(s_btn, &QPushButton::clicked, this, &MeasureDock::popup_all_coursors);
        connect(e_btn, &QPushButton::clicked, this, &MeasureDock::popup_all_coursors);
    }

    _dist_layout->addWidget(_dist_pannel, 1, 0, 1, 7);
}

void MeasureDock::add_dist_measure()
{
    auto mode_rows = get_mode_rows();

    if (mode_rows->_dist_row_list.size() < Max_Measure_Limits)
    {
        cursor_row_info inf;
        inf.cursor1 = -1;
        inf.cursor2 = -1;
        inf.box = NULL;
        inf.del_bt = NULL;
        inf.start_bt = NULL;
        inf.end_bt = NULL;
        inf.r_label = NULL;
        inf.channelIndex = 0;

        mode_rows->_dist_row_list.push_back(inf);

        build_dist_pannel();

        adjusLabelSize();      
    }  
}

void MeasureDock::del_dist_measure()
{
    auto src = dynamic_cast<QToolButton *>(sender());
    assert(src);

    auto mode_rows = get_mode_rows();

    for (auto it = mode_rows->_dist_row_list.begin(); it != mode_rows->_dist_row_list.end(); it++)
    {
        if ((*it).del_bt == src){
            mode_rows->_dist_row_list.erase(it);
            build_dist_pannel();
            break;
        }
    }
}

void MeasureDock::build_edge_pannel()
{
    if (_edge_pannel != NULL)
    {
        _edge_pannel->deleteLater();
        _edge_pannel = NULL;
    }

    QFont labelFont = dock_font_label();
    QFont contentFont = dock_font_content();
    QGridLayout  *lay = new QGridLayout();
    _edge_pannel = new QWidget();   
    _edge_pannel->setLayout(lay);
    lay->setColumnStretch(1, 50);
    lay->setColumnStretch(6, 100);
    lay->setVerticalSpacing(5);
    lay->setContentsMargins(0,0,0,0);
  
    int dex = 0;
    QLabel cal_lb;
    cal_lb.setFont(contentFont);
    int bt_w = cal_lb.fontMetrics().horizontalAdvance("22") + 8;
    auto mode_rows = get_mode_rows();

    for (auto &o : mode_rows->_edge_row_list)
    {
        QWidget *row_widget = new QWidget(_widget);
        row_widget->setContentsMargins(0,0,0,0);
        QHBoxLayout *row_layout = new QHBoxLayout(row_widget);
        row_layout->setAlignment(Qt::AlignLeft);
        row_layout->setContentsMargins(0,0,0,0);
        row_layout->setSpacing(0);
        row_widget->setLayout(row_layout);

        QString iconPath = GetIconPath();
        XToolButton *del_btn = new XToolButton(row_widget);
        del_btn->setIcon(IconCache::Instance().icon(iconPath+"/del.svg"));
        del_btn->setCheckable(true);
        //tr
        QPushButton *s_btn = new QPushButton(" ", row_widget);
        //tr
        QPushButton *e_btn = new QPushButton(" ", row_widget);
        QLabel *r_label = new QLabel(row_widget);
        //tr
        QLabel *g_label = new QLabel("-", row_widget);
        g_label->setContentsMargins(0,0,0,0);
        //tr
        QLabel *a_label = new QLabel("@", row_widget);
        a_label->setContentsMargins(0,0,0,0);
        QComboBox *ch_cmb = create_probe_selector(row_widget);
        ch_cmb->setFixedWidth(50);

        if (o.channelIndex < ch_cmb->count()){
            ch_cmb->setCurrentIndex(o.channelIndex);
        }

        row_layout->addWidget(del_btn);
        row_layout->addSpacing(5);
        row_layout->addWidget(s_btn);
        row_layout->addWidget(g_label);
        row_layout->addWidget(e_btn);
        row_layout->addWidget(a_label);
        row_layout->addWidget(ch_cmb);
        row_layout->addSpacing(5);
        row_layout->addWidget(r_label, 100);

        g_label->setObjectName("dock_label");
        a_label->setObjectName("dock_label");
        r_label->setObjectName("dock_label");
        s_btn->setObjectName("dock_content");
        e_btn->setObjectName("dock_content");
        ch_cmb->setObjectName("dock_content");

        g_label->setFont(labelFont);
        a_label->setFont(labelFont);
        s_btn->setFont(contentFont);
        e_btn->setFont(contentFont);
        r_label->setFont(labelFont);
        ch_cmb->setFont(contentFont);

        s_btn->setFixedWidth(bt_w);
        e_btn->setFixedWidth(bt_w);

        lay->addWidget(row_widget, dex++, 0, 1, 7);

        if (o.r_label != NULL){
            r_label->setText(o.r_label->text());
        }

        o.del_bt = del_btn;
        o.start_bt = s_btn;
        o.end_bt = e_btn;
        o.r_label = r_label;
        o.box = ch_cmb;

        if (o.cursor1 != -1){
            o.start_bt->setText(QString::number(o.cursor1));
            set_cursor_btn_color(o.start_bt);
        }
        if (o.cursor2 != -1){
            o.end_bt->setText(QString::number(o.cursor2));
            set_cursor_btn_color(o.end_bt);
        }

        connect(del_btn, &QPushButton::clicked, this, &MeasureDock::del_edge_measure);
        connect(s_btn, &QPushButton::clicked, this, &MeasureDock::popup_all_coursors);
        connect(e_btn, &QPushButton::clicked, this, &MeasureDock::popup_all_coursors);
        connect(ch_cmb, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MeasureDock::on_edge_channel_selected);
    }

    _edge_layout->addWidget(_edge_pannel, 1, 0, 1, 7);
}

void MeasureDock::on_edge_channel_selected()
{
    QComboBox *box = dynamic_cast<QComboBox*>(sender());
    auto mode_rows = get_mode_rows();

    if (box != NULL && !_bSetting){        
        for (auto &o : mode_rows->_edge_row_list)
        {
            if (o.box == box){
                o.channelIndex = box->currentIndex();
                break;
            }
        }
    }

    update_edge();
    adjusLabelSize();
}

void MeasureDock::add_edge_measure()
{
    auto mode_rows = get_mode_rows();

    if (mode_rows->_edge_row_list.size() < Max_Measure_Limits)
    {
        cursor_row_info inf;
        inf.cursor1 = -1;
        inf.cursor2 = -1;
        inf.box = NULL;
        inf.del_bt = NULL;
        inf.start_bt = NULL;
        inf.end_bt = NULL;
        inf.r_label = NULL;
        inf.channelIndex = 0;

        mode_rows->_edge_row_list.push_back(inf);
        build_edge_pannel();

        adjusLabelSize();
    } 
}

void MeasureDock::del_edge_measure()
{
    QToolButton* src = dynamic_cast<QToolButton *>(sender());
    assert(src); 
    auto mode_rows = get_mode_rows();

    for (auto it =mode_rows->_edge_row_list.begin(); it != mode_rows->_edge_row_list.end(); it++)
    {
        if ((*it).del_bt == src){
            mode_rows->_edge_row_list.erase(it);
            build_edge_pannel();
            break;
        }
    }
}

void MeasureDock::popup_all_coursors()
{
    auto &cursor_list = _view->get_cursorList();

    if (cursor_list.empty()) {
        QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_PLEASE_INSERT_CURSOR), 
                                       "Please insert cursor before using cursor measure."));
        MsgBox::Show(strMsg);
        return;
    }

    _sel_btn = qobject_cast<QPushButton *>(sender());

    QDialog cursor_dlg(_widget);
    cursor_dlg.setWindowFlags(Qt::FramelessWindowHint | Qt::Popup | Qt::WindowSystemMenuHint |
                              Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint);

    QFont contentFont = dock_font_content();

    int index = 0;
    QGridLayout *glayout = new QGridLayout(&cursor_dlg);

    for(auto i = cursor_list.begin(); i != cursor_list.end(); i++) {
        QPushButton *cursor_btn = new QPushButton(&cursor_dlg);
        cursor_btn->setText(QString::number(index+1));
        set_cursor_btn_color(cursor_btn);
        cursor_btn->setObjectName("dock_content");
        cursor_btn->setFont(contentFont);
        glayout->addWidget(cursor_btn, index/4, index%4, 1, 1);

        connect(cursor_btn, &QPushButton::clicked, &cursor_dlg, &QDialog::accept);
        connect(cursor_btn, &QPushButton::clicked, this, &MeasureDock::set_sel_cursor);
        index++;
    }

    QRect sel_btn_rect = _sel_btn->geometry();
    sel_btn_rect.moveTopLeft(_sel_btn->parentWidget()->mapToGlobal(sel_btn_rect.topLeft()));
    cursor_dlg.setGeometry(sel_btn_rect.left(), sel_btn_rect.bottom()+10,
                           cursor_dlg.width(), cursor_dlg.height());
    cursor_dlg.exec();
}

void MeasureDock::set_sel_cursor()
{
    assert(_sel_btn);
    QPushButton *sel_cursor_bt = qobject_cast<QPushButton *>(sender());
    int type = 0;
    cursor_row_info *inf = NULL;
    auto mode_rows = get_mode_rows();

    if (type == 0)
    {
        for (auto &o : mode_rows->_dist_row_list){
            if (o.start_bt == _sel_btn || o.end_bt == _sel_btn){
                inf = &o;
                type = 1;
                break;
            }
        }
    }

    if (type == 0)
    {
        for (auto &o : mode_rows->_edge_row_list){
            if (o.start_bt == _sel_btn || o.end_bt == _sel_btn){
                inf = &o;
                type = 2;
                break;
            }
        }
    } 

    assert(inf);

    _sel_btn->setText(sel_cursor_bt->text());
    set_cursor_btn_color(_sel_btn);

    if (inf->start_bt == _sel_btn){
        inf->cursor1 = sel_cursor_bt->text().toInt();
    }
    else if (inf->end_bt == _sel_btn){
        inf->cursor2 = sel_cursor_bt->text().toInt();
    }

    if (type == 1)
        update_dist();
    else
        update_edge();

    adjusLabelSize();
}

void MeasureDock::update_dist()
{
    auto &cursor_list = _view->get_cursorList();

    auto mode_rows = get_mode_rows();

    for (auto &inf : mode_rows->_dist_row_list)
    {
        if (inf.start_bt == NULL)
            break;

        if (inf.cursor1 != -1) {
            if (inf.cursor1 > (int)cursor_list.size()) {
                inf.start_bt->setText("");
                inf.cursor1 = -1;
            }
        }
        set_cursor_btn_color(inf.start_bt);

        if (inf.cursor2 != -1) {
            if (inf.cursor2 > (int)cursor_list.size()) {
                inf.end_bt->setText("");                
                inf.cursor2 = -1;
            }
        }
        set_cursor_btn_color(inf.end_bt);

        if (inf.cursor1 != -1 && inf.cursor2 != -1) {
            int64_t delta = _view->get_cursor_samples(inf.cursor1-1) -
                            _view->get_cursor_samples(inf.cursor2-1);
            QString delta_text = _view->get_cm_delta(inf.cursor1-1, inf.cursor2-1) +
                                 "/" + QString::number(delta);
            if (delta < 0)
                delta_text.replace('+', '-');
            inf.r_label->setText(delta_text); 
        }
        else {
            inf.r_label->setText(" ");
        }
    }
}

void MeasureDock::update_edge()
{ 
    auto &cursor_list = _view->get_cursorList();
    auto mode_rows = get_mode_rows();

    for (auto &inf : mode_rows->_edge_row_list)
    {
        if (inf.start_bt == NULL)
            break;

        if (inf.cursor1 != -1) {
            if (inf.cursor1 > (int)cursor_list.size()) {
                inf.start_bt->setText("");
                set_cursor_btn_color(inf.start_bt);
                inf.cursor1 = -1;
            }
        }
        if (inf.cursor2 != -1) {
            if (inf.cursor2 > (int)cursor_list.size()) {
                inf.end_bt->setText("");
                set_cursor_btn_color(inf.end_bt);
                inf.cursor2 = -1;
            }
        }

        bool mValid = false;
        if (inf.cursor1 != -1 && inf.cursor2 != -1) {
            uint64_t rising_edges;
            uint64_t falling_edges;

            for(auto s : _view->get_own_signals()) {
                if (s->signal_type() == SR_CHANNEL_LOGIC
                        && s->enabled()
                        && s->get_index() == inf.box->currentText().toInt())
                  {
                    view::LogicSignal *logicSig = (view::LogicSignal*)s;

                    if (logicSig->edges(_view->get_cursor_samples(inf.cursor2-1),
                            _view->get_cursor_samples(inf.cursor1-1), rising_edges, falling_edges)) 
                    {
                        QString delta_text = QString::number(rising_edges) + "/" +
                                             QString::number(falling_edges) + "/" +
                                             QString::number(rising_edges + falling_edges);
                        inf.r_label->setText(delta_text);
                        mValid = true;
                        break;
                    }
                }
            }
        }

        if (!mValid)
            inf.r_label->setText("-/-/-");
    }
}

void MeasureDock::update_cursor_info()
{ 
    auto &cursor_list = _view->get_cursorList();
    auto mode_rows = get_mode_rows();

    int num_cursors = cursor_list.size();
    int num_rows = mode_rows->_opt_row_list.size();

    if (num_rows == 0){
        return;
    }

    assert(num_cursors == num_rows);

    for(int i = 0; i < num_cursors; i++)
    {   
        if (mode_rows->_opt_row_list[i].info_label != NULL){
            QString cur_pos = _view->get_cm_time(i) + "/" 
                    + QString::number(_view->get_cursor_samples(i));
            mode_rows->_opt_row_list[i].info_label->setText(cur_pos);
        }
    }
}

void MeasureDock::set_cursor_btn_color(QPushButton *btn)
{
    QColor bkColor = AppConfig::Instance().GetStyleColor();
    bool isCursor = false;
    const unsigned int start = btn->text().toInt(&isCursor);
    QColor cursor_color = isCursor ? view::Ruler::GetColorByCursorOrder(start) : bkColor;
    set_cursor_btn_color(btn, cursor_color, bkColor, isCursor);
}

void MeasureDock::set_cursor_btn_color(QPushButton *btn, QColor cursorColor, QColor bkColor, bool isCursor)
{  
    QString border_width = isCursor ? "0px" : "1px";
    QString hoverColor = isCursor ? cursorColor.darker().name() : bkColor.name();
    QString textColor = palette().color(QPalette::WindowText).name();
    QString normal = "{background-color:" + cursorColor.name() +
            "; color:" + textColor + "; border-width:" + border_width + ";}";
    QString hover = "{background-color:" + hoverColor +
            "; color:" + textColor + "; border-width:" + border_width + ";}";
    QString style = "QPushButton:hover" + hover +
                    "QPushButton" + normal;
    btn->setStyleSheet(style);
}

QComboBox* MeasureDock::create_probe_selector(QWidget *parent)
{
    DsComboBox *selector = new DsComboBox(parent);
    update_probe_selector(selector);
    return selector;
}

void MeasureDock::update_probe_selector(QComboBox *selector)
{
    selector->clear(); 

    for(auto s : _view->get_own_signals()) {
        if (s->signal_type() == SR_CHANNEL_LOGIC && s->enabled()){
            selector->addItem(QString::number(s->get_index()));
        }
    }
}

void MeasureDock::adjusLabelSize()
{  
   this->adjust_form_size(this);
}

void MeasureDock::cursor_moving()
{
    if (_view->cursors_shown()) {      
        update_cursor_info();
    }

    update_dist();
}

void MeasureDock::reCalc()
{ 
    update_dist();
    update_edge();
    update_cursor_info();

    adjusLabelSize();
}

void MeasureDock::goto_cursor()
{
    QPushButton *src = qobject_cast<QPushButton *>(sender());
    assert(src);

    int index = 0;
    auto mode_rows = get_mode_rows();

    for (auto it = mode_rows->_opt_row_list.begin(); it != mode_rows->_opt_row_list.end(); it++)
    {
        if ( (*it).goto_bt == src){
            _view->set_cursor_middle(index);
            break;
        }
        index++;
    }
}

void MeasureDock::cursor_update()
{
    update_dist();
    update_edge();
    build_cursor_pannel();
}

void MeasureDock::build_cursor_pannel()
{  
    auto mode_rows = get_mode_rows();
    auto &cursor_list = _view->get_cursorList();
    int newCount = (int)cursor_list.size();
    int oldCount = (int)mode_rows->_opt_row_list.size();

    if (newCount == 0) {
        for (auto &row : mode_rows->_opt_row_list) {
            if (row.del_bt != NULL) {
                row.del_bt->deleteLater();
                row.goto_bt->deleteLater();
                row.info_label->deleteLater();
            }
        }
        mode_rows->_opt_row_list.clear();
        adjusLabelSize();
        return;
    }

    if (oldCount == newCount) {
        int index = 1;
        int cursor_dex = 0;
        for (auto it = cursor_list.begin(); it != cursor_list.end(); it++) {
            auto &row = mode_rows->_opt_row_list[cursor_dex];
            row.goto_bt->setText(QString::number(index));
            QString cur_pos = _view->get_cm_time(cursor_dex) + "/" 
                        + QString::number(_view->get_cursor_samples(cursor_dex));
            row.info_label->setText(cur_pos);
            row.cursor = (*it);
            index++;
            cursor_dex++;
        }
        adjusLabelSize();
        return;
    }

    if (newCount < oldCount) {
        for (int i = newCount; i < oldCount; i++) {
            auto &row = mode_rows->_opt_row_list[i];
            if (row.del_bt != NULL) {
                row.del_bt->deleteLater();
                row.goto_bt->deleteLater();
                row.info_label->deleteLater();
            }
        }
        mode_rows->_opt_row_list.resize(newCount);

        int index = 1;
        int cursor_dex = 0;
        for (auto it = cursor_list.begin(); it != cursor_list.end(); it++) {
            auto &row = mode_rows->_opt_row_list[cursor_dex];
            row.goto_bt->setText(QString::number(index));
            QString cur_pos = _view->get_cm_time(cursor_dex) + "/" 
                        + QString::number(_view->get_cursor_samples(cursor_dex));
            row.info_label->setText(cur_pos);
            row.cursor = (*it);
            index++;
            cursor_dex++;
        }
        adjusLabelSize();
        return;
    }

    QFont labelFont = dock_font_label();
    QFont contentFont = dock_font_content();

    QLabel cal_lb;
    cal_lb.setFont(contentFont);

    int bt_w = cal_lb.fontMetrics().horizontalAdvance("22") + 8;

    QString iconPath = GetIconPath();

    int index = oldCount + 1;
    int cursor_dex = oldCount;
    auto it = cursor_list.begin();
    std::advance(it, oldCount);
    for (; it != cursor_list.end(); it++) {
        XToolButton *del_btn = new XToolButton(_widget);
        del_btn->setIcon(IconCache::Instance().icon(iconPath+"/del.svg"));
        del_btn->setCheckable(true);
        QPushButton *cursor_pushButton = new QPushButton(QString::number(index), _widget);
        set_cursor_btn_color(cursor_pushButton);

        QString cur_pos = _view->get_cm_time(cursor_dex) + "/" 
                    + QString::number(_view->get_cursor_samples(cursor_dex));
        QLabel *curpos_label = new QLabel(cur_pos, _widget); 

        _cursor_layout->addWidget(del_btn, 1+index, 0);
        _cursor_layout->addWidget(cursor_pushButton, 1 + index, 1);
        _cursor_layout->addWidget(curpos_label, 1 + index, 2);
        curpos_label->setObjectName("dock_label");
        cursor_pushButton->setObjectName("dock_content");
        cursor_pushButton->setFont(contentFont);
        curpos_label->setFont(labelFont);
        cursor_pushButton->setFixedWidth(bt_w);

        connect(del_btn, &QPushButton::clicked, this, &MeasureDock::del_cursor);
        connect(cursor_pushButton, &QPushButton::clicked, this, &MeasureDock::goto_cursor);

        cursor_opt_info inf = {del_btn, cursor_pushButton, curpos_label, (*it)};
        mode_rows->_opt_row_list.push_back(inf);

        index++;
        cursor_dex++;
    }

    adjusLabelSize();
}

void MeasureDock::del_cursor()
{
    auto *src = qobject_cast<QToolButton *>(sender());
    assert(src);
    
    Cursor* cursor = NULL;
    auto &cursor_list = _view->get_cursorList();
    auto mode_rows = get_mode_rows();
    
    for (auto it = mode_rows->_opt_row_list.begin(); it != mode_rows->_opt_row_list.end(); it++)
    {
        if ((*it).del_bt == src){   
            cursor = (*it).cursor;
            break;
        }
    }

    if (cursor)
        _view->del_cursor(cursor);
    if (cursor_list.empty())
        _view->show_cursors(false);

    cursor_update();
    _view->update();
}

void MeasureDock::UpdateLanguage()
{
    retranslateUi();
}

void MeasureDock::UpdateTheme()
{
    reStyle();
}

void MeasureDock::UpdateFont()
{
    ui::set_dock_form_font(this);
    QFont labelFont = dock_font_label();
    this->parentWidget()->setFont(labelFont);

    adjusLabelSize();
}

void MeasureDock::adjust_form_size(QWidget *wid)
{
    assert(wid);

    QFont labelFont = dock_font_label();
    QFontMetrics fm(labelFont);

    auto labels = wid->findChildren<QLabel*>();
    for(auto o : labels)
    { 
        if (o->wordWrap()) continue; // Never force fixed width on wrapped labels
        
        QFontMetrics labelFm(o->font());
        QRect rc = labelFm.boundingRect(o->text());
        QSize size(rc.width() + 15, rc.height()); 
        o->setFixedSize(size);
    }

    int mouse_info_label_width = fm.horizontalAdvance("############");

    _width_label->setFixedWidth(mouse_info_label_width);
    _period_label->setFixedWidth(mouse_info_label_width);
    _freq_label->setFixedWidth(mouse_info_label_width);
    _duty_label->setFixedWidth(mouse_info_label_width);
}

row_list_item* MeasureDock::get_mode_rows()
{
    int mode = _session->get_device()->get_work_mode();
    int dex = 0;

    if (mode == LOGIC){
        dex = 0;
    }
    else if (mode == DSO){
        dex = 1;
    }
    else if (mode == ANALOG){
        dex = 2;
    }

    for (int i=0; i<MODE_ROWS_LENGTH; i++)
    {  
        if (i == dex){
            continue;
        }
    
        auto rows = &_mode_rows[i];

        for(auto &o : rows->_dist_row_list){               
            o.del_bt = NULL;
            o.start_bt = NULL;
            o.end_bt = NULL;
            o.r_label = NULL;
            o.box = NULL;
        }

        for(auto &o : rows->_edge_row_list){               
            o.del_bt = NULL;
            o.start_bt = NULL;
            o.end_bt = NULL;
            o.r_label = NULL;
            o.box = NULL;
        }

        for (auto &row : rows->_opt_row_list)
        {
            if (row.del_bt != NULL){
                row.del_bt->deleteLater();
                row.goto_bt->deleteLater();
                row.info_label->deleteLater();
                row.del_bt = NULL;
                row.goto_bt = NULL;
                row.info_label = NULL;
            }
        }
        rows->_opt_row_list.clear();       
    }

    _mode_rows[dex]._mode_type = mode;

    return &_mode_rows[dex];
}

} // namespace dock
} // namespace pv
