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

#include "pv/dock/protocoldock.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/dialogs/protocolexp.h"
#include "pv/dialogs/protocollist.h"
#include "pv/session/sigsession.h"
#include "pv/view/trace/decodetrace.h"
#include "pv/view/trace/decodermodel.h"
#include "pv/view/view.h"
#include "pv/widgets/hoversplitter.h"
#include "pv/widgets/smoothtablehelper.h"
#include "pv/dock/searchdock.h"
#include <QElapsedTimer>

#include "pv/mainwindow/appcontrol.h"
#include "pv/config/appconfig.h"
#include "pv/data/decode/decoder.h"
#include "pv/data/decode/decoderstatus.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/session/tabcontext.h"
#include "pv/ui/dockfonts.h"
#include "pv/ui/fn.h"
#include "pv/ui/iconcache.h"
#include "pv/ui/langresource.h"
#include "pv/ui/msgbox.h"
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QObject>
#include <QPainter>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QTableView>
#include <QTableWidgetItem>
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <algorithm>
#include <cassert>
#include <map>
#include <string>

using namespace std;

namespace pv {
namespace dock {

//-----------ProtocolDock

ProtocolDock::ProtocolDock(QWidget *parent, view::View *view,
                           SigSession *session)
    : pv::widgets::SmoothScrollArea(parent), _view(view), _context(nullptr) {
  _session = session;
      _signals = _session;
  _decoder_host = _session;
  _capture = _session;
  _cur_search_index = -1;
  _search_edited = false;
  _pro_add_button = nullptr;
  // View-owned DecoderModel (Task 10): was previously a Core singleton
  // owned by SigSession. Qt parent ownership (this) handles destruction.
  _decoder_model = new pv::view::DecoderModel(this);

  //-----------------------------get protocol list
  GSList *l = const_cast<GSList *>(srd_decoder_list());
  std::map<std::string, int> pro_key_table;
  QString repeatNammes;

  for (; l; l = l->next) {
    const srd_decoder *const d = (srd_decoder *)l->data;
    if (!d) {
      pxv_warn("%s", "ProtocolDock: decoder list item d is nullptr, skipping");
      continue;
    }
    assert(d);
    (void)d;

    DecoderInfoItem *info = new DecoderInfoItem();
    srd_decoder *dec = (srd_decoder *)(l->data);
    info->_data_handle = dec;
    _decoderInfoList.push_back(info);

    std::string prokey(dec->id);
    if (pro_key_table.find(prokey) != pro_key_table.end()) {
      if (repeatNammes != "")
        repeatNammes += ",";
      repeatNammes += QString(dec->id);
    } else {
      pro_key_table[prokey] = 1;
    }
  }
  g_slist_free(l);

  sort(_decoderInfoList.begin(), _decoderInfoList.end(),
       ProtocolDock::protocol_sort_callback);

  if (repeatNammes != "") {
    QString err = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODER_REPEAT),
                      "Any decoder have repeated id or name:");
    err += repeatNammes;
    MsgBox::Show(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_ERROR), "error"),
                 err.toUtf8().data());
  }

  //-----------------------------top panel
  QWidget *top_panel = new QWidget();
  top_panel->setMinimumHeight(70);
  _top_panel = top_panel;
  QWidget *bot_panel = new QWidget();

  _pro_add_button = new QPushButton(top_panel);
  _pro_add_button->setObjectName("dock_content");
  _pro_add_button->setFlat(true);
  _del_all_button = new QPushButton(top_panel);
  _del_all_button->setObjectName("dock_content");
  _del_all_button->setFlat(true);
  _del_all_button->setCheckable(true);
  _pro_keyword_edit = new KeywordLineEdit(top_panel, this);
  _pro_keyword_edit->setReadOnly(true);

  _pro_type_combo = new DsComboBox(top_panel);
  _pro_type_combo->setObjectName("dock_content");
  _pro_type_combo->addItem("All");
  _pro_type_combo->addItem("C");
  _pro_type_combo->addItem("Python");
  _pro_type_combo->setCurrentIndex(0);
  _pro_type_combo->setFixedWidth(70);
  _pro_type_combo->setFixedHeight(28);
  QHBoxLayout *pro_search_lay = new QHBoxLayout();
  pro_search_lay->setSpacing(2);
  pro_search_lay->setAlignment(Qt::AlignVCenter);
  pro_search_lay->addWidget(_pro_add_button, 0, Qt::AlignVCenter);
  pro_search_lay->addWidget(_del_all_button, 0, Qt::AlignVCenter);
  pro_search_lay->addWidget(_pro_keyword_edit, 1, Qt::AlignVCenter);
  pro_search_lay->addWidget(_pro_type_combo, 0, Qt::AlignVCenter);

  _top_layout = new QVBoxLayout();
  _top_layout->addLayout(pro_search_lay);
  _top_layout->addStretch(1);
  _top_layout->setSpacing(5);
  top_panel->setLayout(_top_layout);

  //-----------------------------bottom panel
  _bot_set_button = new QPushButton(bot_panel);
  _bot_set_button->setObjectName("dock_content");
  _bot_set_button->setFlat(true);
  _bot_save_button = new QPushButton(bot_panel);
  _bot_save_button->setObjectName("dock_content");
  _bot_save_button->setFlat(true);
  _dn_nav_button = new QPushButton(bot_panel);
  _dn_nav_button->setObjectName("dock_content");
  _dn_nav_button->setFlat(true);
  _follow_viewport_btn = new QPushButton(bot_panel);
  _follow_viewport_btn->setObjectName("dock_content");
  _follow_viewport_btn->setFlat(true);
  _follow_viewport_btn->setCheckable(true);
  _follow_viewport_btn->setChecked(true);
  _follow_viewport_btn->setToolTip(L_S(STR_PAGE_DLG,
      S_ID(IDS_DLG_FOLLOW_VIEWPORT), "List follows waveform visible range"));
  _bot_title_label = new QLabel(bot_panel);
  _bot_title_label->setObjectName("dock_label");

  // Multi-decoder inline switch combo (Phase 3)
  _decoder_combo = new DsComboBox(bot_panel);
  _decoder_combo->setObjectName("dock_content");
  _decoder_combo->setFixedHeight(28);
  _decoder_combo->setMinimumWidth(80);

  QHBoxLayout *bot_title_layout = new QHBoxLayout();
  bot_title_layout->setSpacing(2);
  bot_title_layout->addWidget(_bot_set_button);
  bot_title_layout->addWidget(_bot_save_button);
  bot_title_layout->addWidget(_bot_title_label, 1);
  bot_title_layout->addWidget(_dn_nav_button);
  bot_title_layout->addWidget(_follow_viewport_btn);

  // Inline search bar: combo + [◀] [search box] [▶]
  _pre_button = new QPushButton(bot_panel);
  _pre_button->setObjectName("dock_content");
  _nxt_button = new QPushButton(bot_panel);
  _nxt_button->setObjectName("dock_content");
  _ann_search_edit = new QLineEdit(bot_panel);
  _ann_search_edit->setClearButtonEnabled(true);
  _ann_search_edit->setObjectName("dock_content");

  QHBoxLayout *search_bar_layout = new QHBoxLayout();
  search_bar_layout->setSpacing(2);
  search_bar_layout->addWidget(_pre_button);
  search_bar_layout->addWidget(_decoder_combo);
  search_bar_layout->addWidget(_ann_search_edit, 1);
  search_bar_layout->addWidget(_nxt_button);

  _matchs_title_label = new QLabel(bot_panel);
  _matchs_title_label->setObjectName("dock_label");
  _matchs_label = new QLabel(bot_panel);
  _matchs_label->setObjectName("dock_label");
  _matchs_label->setText("0");
  QHBoxLayout *match_layout = new QHBoxLayout();
  match_layout->addWidget(_matchs_title_label, 0, Qt::AlignLeft);
  match_layout->addWidget(_matchs_label, 0, Qt::AlignLeft);
  match_layout->addStretch(1);

  _pro_keyword_edit->setFixedHeight(_pro_add_button->sizeHint().height());
  _ann_search_edit->setFixedHeight(_pre_button->sizeHint().height());

  _table_view = new QTableView(bot_panel);
  // Use the proxy model as the table's model so that setFilterFixedString
  // in search_done() actually filters the displayed rows in real time.
  _model_proxy.setSourceModel(_decoder_model);
  _model_proxy.setFilterKeyColumn(-1); // search across all columns
  _model_proxy.setFilterCaseSensitivity(Qt::CaseInsensitive);
  _table_view->setModel(&_model_proxy);
  _table_view->setObjectName("dock_protocol_table_view");
  _table_view->setShowGrid(false);
  _table_view->horizontalHeader()->setStretchLastSection(true);
  _table_view->horizontalHeader()->setHighlightSections(false);
  _table_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  _table_view->horizontalHeader()->setSortIndicatorShown(false);
  _table_view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  _table_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

  new pv::widgets::SmoothTableHelper(_table_view, this);

  _table_view->verticalHeader()->setVisible(false);
  _table_view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
  _table_view->verticalHeader()->setDefaultSectionSize(28);
  _table_view->setFrameShape(QFrame::StyledPanel);
  _table_view->setSelectionBehavior(QAbstractItemView::SelectRows);
  _table_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
  _table_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
  _table_view->setMouseTracking(true);

  _hover_delegate = new RowHoverDelegate();
  _table_view->setItemDelegate(_hover_delegate);

  _table_view->viewport()->installEventFilter(this);

  connect(_table_view, &QAbstractItemView::entered, this,
          &ProtocolDock::on_table_hover);

  // Right-click context menu for copy operations.
  _table_view->setContextMenuPolicy(Qt::CustomContextMenu);
  _table_context_menu = new QMenu(_table_view);
  _copy_cell_action = _table_context_menu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_CELL), "Copy Cell"));
  _copy_row_action = _table_context_menu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_ROW), "Copy Row"));
  _copy_column_action = _table_context_menu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_COLUMN), "Copy Column"));
  _copy_all_action = _table_context_menu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_ALL), "Copy All"));
  _table_context_menu->addSeparator();
  _select_all_action = _table_context_menu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SELECT_ALL), "Select All"));

  connect(_table_view, &QWidget::customContextMenuRequested, this,
          &ProtocolDock::on_table_context_menu);
  connect(_copy_cell_action, &QAction::triggered, this,
          &ProtocolDock::copy_cell);
  connect(_copy_row_action, &QAction::triggered, this,
          &ProtocolDock::copy_row);
  connect(_copy_column_action, &QAction::triggered, this,
          &ProtocolDock::copy_column);
  connect(_copy_all_action, &QAction::triggered, this,
          &ProtocolDock::copy_all);
  connect(_select_all_action, &QAction::triggered, this,
          &ProtocolDock::select_all_rows);

  QVBoxLayout *bot_layout = new QVBoxLayout();
  bot_layout->addLayout(bot_title_layout);
  bot_layout->addLayout(search_bar_layout);
  bot_layout->addLayout(match_layout);
  bot_layout->addWidget(_table_view);
  bot_panel->setLayout(bot_layout);

  QSplitter *split_widget = new pv::widgets::HoverSplitter(this);
  split_widget->insertWidget(0, top_panel);
  split_widget->insertWidget(1, bot_panel);
  split_widget->setOrientation(Qt::Vertical);
  split_widget->setCollapsible(0, false);
  split_widget->setCollapsible(1, false);

  this->setFrameShape(QFrame::NoFrame);
  this->setObjectName("dock_protocol_scroll");
  this->setWidgetResizable(true);

  QWidget *scroll_content = new QWidget();
  QVBoxLayout *scroll_content_layout = new QVBoxLayout(scroll_content);
  scroll_content_layout->setContentsMargins(12, 8, 12, 8);
  scroll_content_layout->setSpacing(0);
  scroll_content_layout->addWidget(split_widget);
  this->setWidget(scroll_content);

  split_widget->setObjectName("protocolWidget");

  connect(_dn_nav_button, &QPushButton::clicked, this,
          &ProtocolDock::nav_table_view);
  connect(_bot_save_button, &QPushButton::clicked, this,
          &ProtocolDock::export_table_view);
  connect(_bot_set_button, &QPushButton::clicked, this,
          &ProtocolDock::set_model);
  connect(_pre_button, &QPushButton::clicked, this, &ProtocolDock::search_pre);
  connect(_nxt_button, &QPushButton::clicked, this, &ProtocolDock::search_nxt);
  connect(_pro_add_button, &QPushButton::clicked, this,
          &ProtocolDock::on_add_protocol);
  connect(_del_all_button, &QPushButton::clicked, this,
          &ProtocolDock::on_del_all_protocol);

  connect(this, &ProtocolDock::protocol_updated, this,
          &ProtocolDock::update_model);
  connect(_table_view, &QAbstractItemView::clicked, this,
          &ProtocolDock::item_clicked);

  connect(_table_view->horizontalHeader(), &QHeaderView::sectionResized, this,
          &ProtocolDock::column_resize);

  connect(_pro_type_combo, QOverload<int>::of(&QComboBox::activated), this,
          &ProtocolDock::show_protocol_select);

  // Real-time filtering: debounce text changes and trigger search_done.
  _search_timer = new QTimer(this);
  _search_timer->setSingleShot(true);
  _search_timer->setInterval(300);
  connect(_search_timer, &QTimer::timeout, this, &ProtocolDock::search_done);
  connect(_ann_search_edit, &QLineEdit::textChanged, this,
          &ProtocolDock::on_search_text_changed);

  // Enter = next match, Shift+Enter = previous match
  connect(_ann_search_edit, &QLineEdit::returnPressed, this, [this]() {
    if (QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier)
      search_pre();
    else
      search_nxt();
  });

  // Header right-click: column show/hide menu
  _table_view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(_table_view->horizontalHeader(), &QWidget::customContextMenuRequested,
          this, &ProtocolDock::on_header_context_menu);

  // Decoder combo switch: use currentIndexChanged instead of activated
  // so it fires reliably even when the selection is changed programmatically
  // (activated only fires on user popup interaction, which can miss some cases).
  connect(_decoder_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &ProtocolDock::on_decoder_combo_changed);

  connect(_follow_viewport_btn, &QPushButton::toggled, this,
          &ProtocolDock::on_follow_viewport_toggled);

  ADD_UI(this);
}

ProtocolDock::~ProtocolDock() {
  // destroy protocol item layers
  for (auto it = _protocol_lay_items.begin(); it != _protocol_lay_items.end();
       it++) {
    DESTROY_QT_LATER(*it);
  }
  _protocol_lay_items.clear();

  // clear protocol infos list
  RELEASE_ARRAY(_decoderInfoList);

  REMOVE_UI(this);
}

void ProtocolDock::set_view(view::View *view) { _view = view; }

void ProtocolDock::bind_context(TabContext *ctx) {
  if (!ctx) {
    pxv_warn("%s", "ProtocolDock::bind_context: ctx is nullptr");
    return;
  }
  assert(ctx);
  _context = ctx;
  _session = ctx->session();
      _signals = _session;
  _decoder_host = _session;
  _capture = _session;
  // Disconnect the previous View's visible-range signal before switching
  // to the new tab's View (multi-tab scenario: the old View stays alive).
  if (_view) {
    disconnect(_view, &view::View::visible_range_changed, this,
               &ProtocolDock::on_visible_range_changed);
  }
  _view = ctx->view();
  // Table already uses _model_proxy (set in constructor); just ensure
  // the proxy's source is the current tab's decoder model.
  _model_proxy.setSourceModel(_decoder_model);
  rebuild_protocol_layers();
  update_view_status();

  // Wire visible-range notifications from the View to the filter handler.
  // on_visible_range_changed() checks _follow_viewport internally so the
  // connection can stay alive across toggle changes.
  if (_view) {
    connect(_view, &view::View::visible_range_changed, this,
            &ProtocolDock::on_visible_range_changed);
    // Apply the current viewport state immediately on tab switch.
    if (_follow_viewport) {
      on_visible_range_changed();
    }
  }

  if (ctx->view()) {
    auto &ui = ctx->view()->dock_ui_state();
    if (!ui.dock_protocol_search_text.isEmpty()) {
      _ann_search_edit->setText(ui.dock_protocol_search_text);
      search_done();
    }
    const QJsonArray &expanded_states = ui.dock_protocol_expanded_states;
    for (int i = 0;
         i < (int)_protocol_lay_items.size() && i < expanded_states.size();
         i++) {
      _protocol_lay_items[i]->m_expanded = expanded_states[i].toBool(true);
    }
  }
}

void ProtocolDock::unbind_context() {
  if (_context && _context->view()) {
    auto &ui = _context->view()->dock_ui_state();
    ui.dock_protocol_search_text = _ann_search_edit->text();
    QJsonArray expanded_states;
    for (auto layer : _protocol_lay_items) {
      expanded_states.append(layer->m_expanded);
    }
    ui.dock_protocol_expanded_states = expanded_states;
  }
  _context = nullptr;
}

void ProtocolDock::retranslateUi() {
  _ann_search_edit->setPlaceholderText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH), "search"));
  _matchs_title_label->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MATCHING_ITEMS), "Matching Items:"));
  _bot_title_label->setText(L_S(STR_PAGE_DLG,
                                S_ID(IDS_DLG_PROTOCOL_LIST_VIEWER),
                                "Protocol List Viewer"));
  _follow_viewport_btn->setToolTip(L_S(
      STR_PAGE_DLG, S_ID(IDS_DLG_FOLLOW_VIEWPORT),
      "List follows waveform visible range"));
  _pro_keyword_edit->ResetText();

  if (_copy_cell_action)
    _copy_cell_action->setText(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_CELL), "Copy Cell"));
  if (_copy_row_action)
    _copy_row_action->setText(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_ROW), "Copy Row"));
  if (_copy_column_action)
    _copy_column_action->setText(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_COLUMN), "Copy Column"));
  if (_copy_all_action)
    _copy_all_action->setText(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_ALL), "Copy All"));
  if (_select_all_action)
    _select_all_action->setText(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SELECT_ALL), "Select All"));
}

void ProtocolDock::reStyle() {
  QString iconPath = GetIconPath();

  if (_pro_add_button == nullptr) {
    return;
  }

  _pro_add_button->setIcon(IconCache::Instance().icon(iconPath + "/add.svg"));
  _del_all_button->setIcon(IconCache::Instance().icon(iconPath + "/del.svg"));
  _bot_set_button->setIcon(IconCache::Instance().icon(iconPath + "/gear.svg"));
  _bot_save_button->setIcon(IconCache::Instance().icon(iconPath + "/save.svg"));
  _dn_nav_button->setIcon(IconCache::Instance().icon(iconPath + "/nav.svg"));
  _pre_button->setIcon(IconCache::Instance().icon(iconPath + "/pre.svg"));
  _nxt_button->setIcon(IconCache::Instance().icon(iconPath + "/next.svg"));
  _follow_viewport_btn->setIcon(
      IconCache::Instance().icon(iconPath + "/display.svg"));

  for (auto item : _protocol_lay_items) {
    item->ResetStyle();
  }
}

int ProtocolDock::decoder_name_cmp(const void *a, const void *b) {
  return strcmp(((const srd_decoder *)a)->name, ((const srd_decoder *)b)->name);
}

int ProtocolDock::get_protocol_index_by_id(QString id) {
  int dex = 0;
  for (auto info : _decoderInfoList) {
    srd_decoder *dec = (srd_decoder *)(info->_data_handle);
    QString proid(dec->id);
    if (id == proid) {
      return dex;
    }
    ++dex;
  }
  return -1;
}

void ProtocolDock::on_add_protocol() {
  if (_decoderInfoList.size() == 0) {
    MsgBox::Show(nullptr, L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODER_LIST_EMPTY),
                           "Decoder list is empty!"));
    return;
  }
  if (_selected_protocol_id == "") {
    MsgBox::Show(nullptr, L_S(STR_PAGE_MSG, S_ID(IDS_MSG_NO_SEL_DECODER),
                           "Please select a decoder!"));
    return;
  }

  int dex = this->get_protocol_index_by_id(_selected_protocol_id);
  assert(dex >= 0);

  // check the base protocol
  srd_decoder *const dec = (srd_decoder *)(_decoderInfoList[dex]->_data_handle);
  QString pro_id(dec->id);
  std::list<data::decode::Decoder *> sub_decoders;

  assert(dec->inputs);

  QString input_id = parse_protocol_id((char *)dec->inputs->data);

  if (input_id != "logic") {
    pro_id = ""; // reset base protocol

    int base_dex = get_output_protocol_by_id(input_id);
    sub_decoders.push_front(new data::decode::Decoder(dec));

    while (base_dex != -1) {
      srd_decoder *base_dec =
          (srd_decoder *)(_decoderInfoList[base_dex]->_data_handle);
      pro_id = QString(base_dec->id); // change base protocol

      assert(base_dec->inputs);

      input_id = parse_protocol_id((char *)base_dec->inputs->data);

      if (input_id == "logic") {
        break;
      }

      sub_decoders.push_front(new data::decode::Decoder(base_dec));
      pro_id = ""; // reset base protocol
      base_dex = get_output_protocol_by_id(input_id);
    }
  }

  if (pro_id == "") {
    MsgBox::Show(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_ERROR), "error"),
                 L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FIND_BASE_DECODER_ERROR),
                     "find the base decoder error!"));

    for (auto sub : sub_decoders) {
      delete sub;
    }
    sub_decoders.clear();

    return;
  }

  add_protocol_by_id(pro_id, false, sub_decoders);
}

bool ProtocolDock::add_protocol_by_id(
    QString id, bool silent,
    std::list<pv::data::decode::Decoder *> &sub_decoders) {
  int cur_mode = _signals->device()->get_work_mode();
  if (cur_mode != LOGIC && cur_mode != MSO) {
    pxv_info(
        "Protocol Analyzer\nProtocol Analyzer is only valid in Digital Mode!");
    return false;
  }

  int dex = this->get_protocol_index_by_id(id);
  if (dex == -1) {
    pxv_err("Protocol not exists! id:%s", id.toUtf8().data());
    return false;
  }

  srd_decoder *const decoder =
      (srd_decoder *)(_decoderInfoList[dex]->_data_handle);
  DecoderStatus *dstatus = new DecoderStatus();
  dstatus->m_format = (int)DecoderDataFormat::hex;

  QString protocolName(decoder->name);
  QString protocolId(decoder->id);

  if (sub_decoders.size()) {
    auto it = sub_decoders.end();
    it--;
    protocolName = QString((*it)->decoder()->name);
    protocolId = QString((*it)->decoder()->id);
  }

  std::shared_ptr<pv::data::DecoderStack> stack;

  // Route through the View layer so the View can create its own DecodeTrace
  // wrapper for the newly created DecoderStack. The View internally calls
  // Core (SigSession::add_decoder) to create the stack, then creates the
  pxv_info("ProtocolDock: calling _view->add_decoder for %s, silent=%d",
           id.toUtf8().data(), silent);
  if (_view->add_decoder(decoder, silent, dstatus, sub_decoders, stack) ==
      false) {
    pxv_info("ProtocolDock: _view->add_decoder returned false");
    return false;
  }
  pxv_info("ProtocolDock: _view->add_decoder returned true");

  // create item layer
  ProtocolItemLayer *layer =
      new ProtocolItemLayer(_top_panel, protocolName, this);
  _protocol_lay_items.push_back(layer);
  _top_layout->insertLayout(_protocol_lay_items.size(), layer);
  layer->m_decoderStatus = dstatus;
  layer->m_protocolId = protocolId;
  layer->_trace = stack.get();
  layer->SetVisibilityState(true);

  // set current protocol format
  string fmt =
      AppConfig::Instance().GetProtocolFormat(protocolId.toStdString());
  if (fmt != "") {
    layer->SetProtocolFormat(fmt.c_str());
    dstatus->m_format = DecoderDataFormat::Parse(fmt.c_str());
  }

  // progress connection
  protocol_updated();
  if (stack) {
    connect(stack.get(), &data::DecoderStack::new_decode_data, this,
            &ProtocolDock::on_decoder_progress);
    connect(stack.get(), &data::DecoderStack::decode_done, this,
            &ProtocolDock::on_decoder_progress);
  }

  adjustPannelSize();

  return true;
}

void ProtocolDock::rebuild_protocol_layers() {
  for (auto layer : _protocol_lay_items) {
    if (layer->_trace) {
      auto stack = static_cast<data::DecoderStack *>(layer->_trace);
      disconnect(stack, &data::DecoderStack::new_decode_data, this,
                 &ProtocolDock::on_decoder_progress);
      disconnect(stack, &data::DecoderStack::decode_done, this,
                 &ProtocolDock::on_decoder_progress);
    }
    _top_layout->removeItem(layer);
    DESTROY_QT_LATER(layer);
  }
  _protocol_lay_items.clear();

  // Read decoder traces from the View layer (View-owned DecodeTrace list)
  // instead of querying Core's DecoderStack list directly. Each DecodeTrace
  // wraps a Core-owned DecoderStack accessed via trace->decoder().
  const auto &decode_traces = _view->get_own_decode_traces();
  for (auto &trace : decode_traces) {
    auto stack = trace ? trace->decoder() : nullptr;
    if (!stack)
      continue;

    DecoderStatus *dstatus = (DecoderStatus *)stack->get_key_handel();

    auto &decoders = stack->stack();
    QString protocolName(decoders.back()->decoder()->name);
    QString protocolId(decoders.back()->decoder()->id);

    // Distinguish instances:
    // 1. If custom label exists: use "Name(label)"
    // 2. If no custom label: auto-generate from bound channel "Name(CH1)"
    QString lbl = stack->label();
    if (lbl.isEmpty())
      lbl = stack->auto_label();
    if (!lbl.isEmpty())
      protocolName += "(" + lbl + ")";

    ProtocolItemLayer *layer =
        new ProtocolItemLayer(_top_panel, protocolName, this);
    _protocol_lay_items.push_back(layer);
    _top_layout->insertLayout(_protocol_lay_items.size(), layer);
    layer->m_decoderStatus = dstatus;
    layer->m_protocolId = protocolId;
    layer->_trace = stack.get();
    layer->SetVisibilityState(decoders.front()->shown());

    static const char *formatNames[] = {"hex", "dec", "oct", "bin", "ascii"};
    int fmt = dstatus->m_format;
    if (fmt >= 0 && fmt <= 4) {
      layer->SetProtocolFormat(formatNames[fmt]);
    }

    int pg = stack->get_progress();
    QString err;
    if (stack->out_of_memory())
      err = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_OUT_OF_MEMORY), "Out of Memory");
    layer->SetProgress(pg, err);
    if (pg == 100 && dstatus != nullptr) {
      layer->enable_format(dstatus->m_bNumeric);
    }

    connect(stack.get(), &data::DecoderStack::new_decode_data, this,
            &ProtocolDock::on_decoder_progress);
    connect(stack.get(), &data::DecoderStack::decode_done, this,
            &ProtocolDock::on_decoder_progress);
  }

  protocol_updated();
  adjustPannelSize();
}

void ProtocolDock::on_del_all_protocol() {
  if (_protocol_lay_items.size() == 0) {
    MsgBox::Show(nullptr,
                 L_S(STR_PAGE_MSG, S_ID(IDS_MSG_NO_DECODER_DEL),
                     "Have no decoder to remove!"),
                 this);
    return;
  }

  QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODER_CONFIRM_DEL_ALL),
                     "Are you sure to remove all decoder?"));
  if (MsgBox::Confirm(strMsg, this)) {
    del_all_protocol();
  }
}

void ProtocolDock::del_all_protocol() {
  if (_protocol_lay_items.size() > 0) {
    // Call View layer to delete all DecodeTrace, then View will notify Core
    // to clear all DecoderStacks. Directly calling _decoder_host->clear_all_decoder()
    // would bypass View and leave DecodeTrace objects orphaned, causing stale UI.
    if (_view) {
      _view->clear_all_decoders();
    } else {
      // Headless fallback: directly call Core if no View exists
      _decoder_host->clear_all_decoder();
    }

    for (auto it = _protocol_lay_items.begin(); it != _protocol_lay_items.end();
         it++) {
      DESTROY_QT_LATER((*it)); // destory control
    }

    _protocol_lay_items.clear();
    this->update();
    protocol_updated();

    adjustPannelSize();
  }
}

void ProtocolDock::decoded_progress(int progress) {
  const auto &decode_sigs = _decoder_host->get_decoder_stacks();
  unsigned int index = 0;

  for (auto d : decode_sigs) {
    int pg = d->get_progress();
    QString err;

    if (d->out_of_memory())
      err = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_OUT_OF_MEMORY), "Out of Memory");

    if (index < _protocol_lay_items.size()) {
      ProtocolItemLayer &lay = *(_protocol_lay_items.at(index));
      lay.SetProgress(pg, err);

      // have custom data format
      if (pg == 100 && lay.m_decoderStatus != nullptr) {
        lay.enable_format(lay.m_decoderStatus->m_bNumeric);
      }
    }

    index++;
  }

  static QElapsedTimer update_timer;
  if (!update_timer.isValid())
    update_timer.start();

  if (progress == 0 || progress == 100 ||
      (progress % 10 == 1 && update_timer.elapsed() > 500)) {
    update_model();
    update_timer.start();
  }
}

void ProtocolDock::on_decoder_progress() {
  auto *stack = qobject_cast<data::DecoderStack *>(sender());
  decoded_progress(stack ? stack->get_progress() : 0);
}

void ProtocolDock::set_model() {
  pv::dialogs::ProtocolList *protocollist_dlg =
      new pv::dialogs::ProtocolList(this, _session, _decoder_model);
  protocollist_dlg->exec();
  resize_table_view(_decoder_model);
  _model_proxy.setSourceModel(_decoder_model);
  search_done();

  // clear mark_index of all DecoderStacks
  const auto &decode_sigs = _decoder_host->get_decoder_stacks();

  for (auto d : decode_sigs) {
    d->set_mark_index(-1);
  }
}

void ProtocolDock::update_model() {
  pv::view::DecoderModel *decoder_model = _decoder_model;
  const auto &decode_sigs = _decoder_host->get_decoder_stacks();

  // Update decoder combo: "All" at index 0, individual decoders after.
  _decoder_combo->blockSignals(true);
  _decoder_combo->clear();
  _decoder_combo->addItem(QStringLiteral("All"));
  int current_combo_index = 0; // default to "All"
  for (size_t i = 0; i < decode_sigs.size(); i++) {
    auto &stack = decode_sigs[i];
    auto &decoders = stack->stack();
    QString name;
    if (!decoders.empty())
      name = QString(decoders.back()->decoder()->name);
    else
      name = QString("Decoder %1").arg(i);
    // Distinguish instances:
    // 1. If custom label exists: use "Name(label)"
    // 2. If no custom label: auto-generate from bound channel "Name(CH1)"
    QString lbl = stack->label();
    if (lbl.isEmpty())
      lbl = stack->auto_label();
    if (!lbl.isEmpty())
      name += "(" + lbl + ")";
    _decoder_combo->addItem(name);
    if (decoder_model->getDecoderStack() == stack.get())
      current_combo_index = (int)(i + 1); // +1 for "All" offset
  }
  _decoder_combo->blockSignals(false);

  if (decode_sigs.size() == 0) {
    decoder_model->setDecoderStack(nullptr);
    current_combo_index = 0;
  } else if (decoder_model->isMultiStackMode()) {
    // Already in "All" mode — preserve it across decode progress updates.
    // This check must come before the !getDecoderStack() check because
    // setAllStacks() sets _decoder_stack to nullptr, so getDecoderStack()
    // returns nullptr in multi-stack mode.
    if (decoder_model->stackCount() != (int)decode_sigs.size()) {
      // Stack count changed: full rebuild (prefix sums + column map, O(stacks))
      std::vector<pv::data::DecoderStack *> stacks;
      for (auto &s : decode_sigs)
        stacks.push_back(s.get());
      decoder_model->setAllStacks(stacks);
    }
    // else: no rebuild needed — rowCount reads list_annotation_size()
    // on demand, and viewport following rebuilds _visible_merged lazily.
    current_combo_index = 0;
  } else if (!decoder_model->getDecoderStack()) {
    // No stack selected yet: default to first decoder (single-stack,
    // fast O(1) path). User can switch to "All" via combo if desired.
    decoder_model->setDecoderStack(decode_sigs.at(0).get());
    current_combo_index = 1; // first decoder after "All"
  } else {
    // Find the currently selected single stack
    unsigned int index = 0;
    bool found = false;
    for (auto d : decode_sigs) {
      if (d.get() == decoder_model->getDecoderStack()) {
        decoder_model->setDecoderStack(d.get());
        current_combo_index = (int)(index + 1); // +1 for "All" offset
        found = true;
        break;
      }
      index++;
    }
    if (!found) {
      decoder_model->setDecoderStack(decode_sigs.at(0).get());
      current_combo_index = 1;
    }
  }

  // Sync combo selection
  _decoder_combo->blockSignals(true);
  _decoder_combo->setCurrentIndex(current_combo_index);
  _decoder_combo->blockSignals(false);

  search_done();
  resize_table_view(decoder_model);
}

void ProtocolDock::resize_table_view(view::DecoderModel *decoder_model) {
  bool has_data = decoder_model->getDecoderStack() ||
                  decoder_model->isMultiStackMode();
  if (has_data) {
    int column_count = decoder_model->columnCount(QModelIndex());
    int row_count = decoder_model->rowCount(QModelIndex());
    QFontMetrics fm(_table_view->font());
    QFontMetrics hfm(_table_view->horizontalHeader()->font());

    if (row_count < 500) {
      for (int i = 0; i < column_count; i++) {
        // Start with header text width.
        QVariant hdr = decoder_model->headerData(i, Qt::Horizontal,
                                                  Qt::DisplayRole);
        int best = fm.height(); // minimum sensible width
        if (hdr.isValid())
          best = std::max(best, hfm.boundingRect(hdr.toString()).width() + 20);
        // Then scan data rows.
        int max_scan = std::min(row_count, 200);
        for (int r = 0; r < max_scan; r++) {
          QModelIndex idx = decoder_model->index(r, i);
          QString text = decoder_model->data(idx, Qt::DisplayRole).toString();
          int w = fm.boundingRect(text).width() + 20;
          if (w > best)
            best = w;
        }
        if (best > 300)
          best = 300;
        _table_view->setColumnWidth(i, best);
      }
    } else {
      // Large model: scan only first 200 rows, but always check header.
      int max_scan = 200;
      for (int col = 0; col < column_count; col++) {
        // Header width as baseline.
        QVariant hdr = decoder_model->headerData(col, Qt::Horizontal,
                                                 Qt::DisplayRole);
        int max_width = fm.height();
        if (hdr.isValid())
          max_width = std::max(max_width,
                               hfm.boundingRect(hdr.toString()).width() + 20);
        for (int row = 0; row < max_scan; row++) {
          QModelIndex idx = decoder_model->index(row, col);
          QString text = decoder_model->data(idx, Qt::DisplayRole).toString();
          int width = fm.boundingRect(text).width() + 20;
          if (width > max_width)
            max_width = width;
        }
        if (max_width > 300)
          max_width = 300;
        _table_view->setColumnWidth(col, max_width);
      }
    }

    int top_row = _table_view->rowAt(0);
    int bom_row = _table_view->rowAt(_table_view->height());
    if (bom_row >= top_row && top_row >= 0) {
      for (int i = top_row; i <= bom_row; i++)
        _table_view->resizeRowToContents(i);
    }
  }
}

void ProtocolDock::item_clicked(const QModelIndex &index) {
  if (!index.isValid())
    return;

  // Map proxy index to source (DecoderModel) index for annotation lookup.
  QModelIndex source_index = _model_proxy.mapToSource(index);
  if (!source_index.isValid())
    return;

  pv::view::DecoderModel *decoder_model = _decoder_model;

  // Map model row to the owning decoder stack and row-within-stack.
  // This handles both single-stack and multi-stack (All) modes.
  pv::data::DecoderStack *decoder_stack = nullptr;
  uint64_t query_row = 0;
  decoder_model->mapRowToStack(source_index.row(), decoder_stack, query_row);

  // In multi-stack mode, _nav_column is always 0 (annotation column 0).
  // In single-stack mode, subtract PRESET_COLUMN_COUNT from the model column.
  int ann_col;
  if (decoder_model->isMultiStackMode()) {
    ann_col = 0;
  } else {
    ann_col = source_index.column() - pv::view::DecoderModel::PRESET_COLUMN_COUNT;
    if (ann_col < 0)
      ann_col = 0;
  }
  _nav_column = ann_col;

  if (decoder_stack) {
    pv::data::decode::Annotation ann;
    if (decoder_stack->list_annotation(&ann, ann_col, query_row)) {
      const auto &decode_sigs = _decoder_host->get_decoder_stacks();

      for (auto d : decode_sigs) {
        d->set_mark_index(-1);
      }

      decoder_stack->set_mark_index((ann.start_sample() + ann.end_sample()) /
                                    2);

      // Set the jump guard before show_region() so the async visible_range
      // notification triggered by the view change preserves this row.
      _jumping_to_row = true;
      _jumping_target_row = (int64_t)query_row;
      _session->show_region(ann.start_sample(), ann.end_sample(), false);
    }
  }

  _table_view->resizeRowToContents(index.row());
}

void ProtocolDock::column_resize(int index, int old_size, int new_size) {
  (void)index;
  (void)old_size;
  (void)new_size;
  pv::view::DecoderModel *decoder_model = _decoder_model;
  bool has_data = decoder_model->getDecoderStack() ||
                  decoder_model->isMultiStackMode();
  if (has_data) {
    int top_row = _table_view->rowAt(0);
    int bom_row = _table_view->rowAt(_table_view->height());
    if (bom_row >= top_row && top_row >= 0) {
      for (int i = top_row; i <= bom_row; i++)
        _table_view->resizeRowToContents(i);
    }
  }
}

void ProtocolDock::export_table_view() {
  pv::dialogs::ProtocolExp *protocolexp_dlg =
      new pv::dialogs::ProtocolExp(this, _session, _decoder_model);
  protocolexp_dlg->exec();
}

void ProtocolDock::nav_table_view() {
  pv::view::DecoderModel *decoder_model = _decoder_model;

  // ===== Multi-stack (All) mode: binary search the merged list =====
  if (decoder_model->isMultiStackMode()) {
    const auto &decode_sigs = _decoder_host->get_decoder_stacks();
    if (decode_sigs.empty())
      return;

    uint64_t samplerate = decode_sigs[0]->samplerate();
    if (samplerate == 0)
      return;

    // Compute the viewport center sample position.
    double scale = _view->scale();
    int64_t offset = _view->offset();
    int viewport_width = _view->viewport()->width();
    if (viewport_width <= 0)
      return;
    uint64_t center_sample = (uint64_t)((double)(offset + viewport_width / 2)
                                        * (double)samplerate * scale);

    // Binary search the merged list for the closest entry.
    int model_row = decoder_model->findRowBySample(center_sample);
    if (model_row < 0 || model_row >= decoder_model->rowCount(QModelIndex()))
      return;

    QModelIndex source_index = decoder_model->index(model_row, 0);
    QModelIndex proxy_index = _model_proxy.mapFromSource(source_index);
    if (proxy_index.isValid()) {
      _table_view->scrollTo(proxy_index);
      _table_view->setCurrentIndex(proxy_index);

      const auto &decode_sigs2 = _decoder_host->get_decoder_stacks();
      for (auto d : decode_sigs2)
        d->set_mark_index(-1);

      pv::data::DecoderStack *target_stack = nullptr;
      uint64_t row_in_stack = 0;
      decoder_model->mapRowToStack(model_row, target_stack, row_in_stack);
      if (target_stack) {
        pv::data::decode::Annotation ann;
        if (target_stack->list_annotation(&ann, 0, row_in_stack)) {
          target_stack->set_mark_index(
              (ann.start_sample() + ann.end_sample()) / 2);
        }
      }
      _view->set_all_update(true);
      _view->update();
    }
    return;
  }

  // ===== Single-stack mode =====
  uint64_t row_index = 0;
  auto decoder_stack = decoder_model->getDecoderStack();
  if (!decoder_stack)
    return;

  uint64_t offset =
      _view->offset() * (decoder_stack->samplerate() * _view->scale());
  std::map<const pv::data::decode::Row, bool> rows =
      decoder_stack->get_rows_lshow();
  int column = _nav_column;
  for (std::map<const pv::data::decode::Row, bool>::const_iterator i =
           rows.begin();
       i != rows.end(); i++) {
    if ((*i).second && column-- == 0) {
      row_index = decoder_stack->get_annotation_index((*i).first, offset);
      break;
    }
  }

  int64_t source_row = (int64_t)row_index;
  if (decoder_model->visible_start_row() >= 0) {
    source_row = (int64_t)row_index - decoder_model->visible_start_row();
  }

  if (source_row < 0 || source_row >= decoder_model->rowCount(QModelIndex()))
    return;

  QModelIndex source_index = decoder_model->index(source_row,
      _nav_column + pv::view::DecoderModel::PRESET_COLUMN_COUNT);
  QModelIndex proxy_index = _model_proxy.mapFromSource(source_index);

  if (proxy_index.isValid()) {
    pv::data::decode::Annotation ann;

    if (decoder_stack->list_annotation(&ann, _nav_column, row_index)) {
      _table_view->scrollTo(proxy_index);
      _table_view->setCurrentIndex(proxy_index);

      const auto &decode_sigs = _decoder_host->get_decoder_stacks();

      for (auto d : decode_sigs) {
        d->set_mark_index(-1);
      }

      decoder_stack->set_mark_index((ann.start_sample() + ann.end_sample()) /
                                    2);
      _view->set_all_update(true);
      _view->update();
    }
  }
}

void ProtocolDock::search_pre() {
  // Navigate to the previous row in the filtered results.
  int row_count = _model_proxy.rowCount();
  if (row_count == 0) {
    _table_view->scrollToTop();
    _table_view->clearSelection();
    _matchs_label->setText(QString::number(0));
    _cur_search_index = -1;
    return;
  }
  _cur_search_index--;
  if (_cur_search_index < 0)
    _cur_search_index = row_count - 1;
  QModelIndex idx = _model_proxy.index(_cur_search_index, 0);
  if (idx.isValid()) {
    _table_view->scrollTo(idx);
    _table_view->setCurrentIndex(idx);
    _table_view->clicked(idx); // trigger item_clicked → show_region → viewport jump
  }
}

void ProtocolDock::search_nxt() {
  // Navigate to the next row in the filtered results.
  int row_count = _model_proxy.rowCount();
  if (row_count == 0) {
    _table_view->scrollToTop();
    _table_view->clearSelection();
    _matchs_label->setText(QString::number(0));
    _cur_search_index = -1;
    return;
  }
  _cur_search_index++;
  if (_cur_search_index >= row_count)
    _cur_search_index = 0;
  QModelIndex idx = _model_proxy.index(_cur_search_index, 0);
  if (idx.isValid()) {
    _table_view->scrollTo(idx);
    _table_view->setCurrentIndex(idx);
    _table_view->clicked(idx); // trigger item_clicked → show_region → viewport jump
  }
}

void ProtocolDock::search_done() {
  QString str = _ann_search_edit->text().trimmed();
  _model_proxy.setFilterFixedString(str);
  _matchs_label->setText(QString::number(_model_proxy.rowCount()));
  _cur_search_index = -1;
}

void ProtocolDock::on_search_text_changed(const QString &text) {
  (void)text;
  // Show pending indicator while the debounce timer is running.
  _matchs_label->setText("...");
  _search_timer->start(); // fires search_done() after 300ms of inactivity
}

void ProtocolDock::search_changed() {
  // Legacy stub: real-time filtering is handled by on_search_text_changed.
}

void ProtocolDock::search_update() {
  // Legacy stub: real-time filtering is handled by on_search_text_changed.
}

//-------------------IProtocolItemLayerCallback
void ProtocolDock::OnProtocolSetting(void *handle) {

  for (auto it = _protocol_lay_items.begin(); it != _protocol_lay_items.end();
       it++) {
    if ((*it) == handle) {
      void *key_handel = (*it)->get_protocol_key_handel();
      // Route through the View layer so the DecoderOptionsDlg is shown
      // (View owns the DecodeTrace; Core cannot show Qt dialogs). If the
      // user cancels the dialog, no reset happens. Previously this called
      // _data_src->rst_decoder_by_key_handel() directly, which never showed
      // the dialog after de-view-ization (create_popup was moved out of
      // Core but never re-added in the View layer).
      _view->rst_decoder_by_key_handel(key_handel);
      protocol_updated();
      break;
    }
  }
}

void ProtocolDock::OnProtocolDelete(void *handle) {
  QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODER_CONFIRM_DEL),
                     "Are you sure to remove this decoder?"));

  if (!MsgBox::Confirm(strMsg, this)) {
    return;
  }

  for (auto it = _protocol_lay_items.begin(); it != _protocol_lay_items.end();
       it++) {
    if ((*it) == handle) {
      auto lay = (*it);
      void *key_handel = lay->get_protocol_key_handel();
      _protocol_lay_items.erase(it);
      DESTROY_QT_LATER(lay);
      // Call View layer to delete DecodeTrace, then View will notify Core
      // to delete DecoderStack. Directly calling _decoder_host->remove_decoder_by_key_handel()
      // would bypass View and leave the DecodeTrace orphaned, causing stale UI.
      if (_view) {
        _view->remove_decoder_by_key_handel(key_handel);
      } else {
        // Headless fallback: directly call Core if no View exists
        _decoder_host->remove_decoder_by_key_handel(key_handel);
      }
      protocol_updated();
      break;
    }
  }

  adjustPannelSize();
}

void ProtocolDock::OnProtocolVisibilityChanged(void *handle) {
  for (auto it = _protocol_lay_items.begin(); it != _protocol_lay_items.end();
       it++) {
    if ((*it) == handle) {
      auto lay = (*it);
      auto stack = static_cast<pv::data::DecoderStack *>(lay->_trace);
      if (stack) {
        if (!stack->stack().empty()) {
          auto root_dec = stack->stack().front().get();
          bool current_shown = root_dec->shown();
          bool new_shown = !current_shown;
          root_dec->show(new_shown);
          lay->SetVisibilityState(new_shown);

          // Sync Trace::_visible so the View layer (layout, header) also
          // hides/shows the decode track.  Without this, only Decoder::_shown
          // is toggled, but signals_changed() / paint_label() check
          // Trace::visible(), so the header and track space remain.
          if (_view) {
            auto &traces = _view->get_own_decode_traces();
            for (auto &dt : traces) {
              if (dt && dt->decoder().get() == stack) {
                dt->set_visible(new_shown);
                break;
              }
            }
            _view->signals_changed(nullptr);
          }
        }
      }
      break;
    }
  }

  _session->broadcast_async<interface::DeviceOptionsUpdated>({});
}

void ProtocolDock::OnProtocolFormatChanged(QString format, void *handle) {
  for (auto it = _protocol_lay_items.begin(); it != _protocol_lay_items.end();
       it++) {
    if ((*it) == handle) {
      auto lay = (*it);
      AppConfig::Instance().SetProtocolFormat(lay->m_protocolId.toStdString(),
                                              format.toStdString());

      if (lay->m_decoderStatus != nullptr) {
        lay->m_decoderStatus->m_format =
            DecoderDataFormat::Parse(format.toStdString().c_str());
        protocol_updated();
      }

      break;
    }
  }
}

bool ProtocolDock::protocol_sort_callback(const DecoderInfoItem *o1,
                                          const DecoderInfoItem *o2) {
  srd_decoder *dec1 = (srd_decoder *)(o1->_data_handle);
  srd_decoder *dec2 = (srd_decoder *)(o2->_data_handle);
  const char *s1 = dec1->name;
  const char *s2 = dec2->name;
  char c1 = 0;
  char c2 = 0;

  while (*s1 && *s2) {
    c1 = *s1;
    c2 = *s2;

    if (c1 >= 'a' && c1 <= 'z')
      c1 -= 32;
    if (c2 >= 'a' && c2 <= 'z')
      c2 -= 32;

    if (c1 > c2)
      return false;
    else if (c1 < c2)
      return true;

    s1++;
    s2++;
  }

  if (*s1)
    return false;
  else if (*s2)
    return true;

  return true;
}

QString ProtocolDock::parse_protocol_id(const char *id) {
  if (id == nullptr || *id == 0) {
    pxv_warn("ProtocolDock::parse_protocol_id: id is null or empty");
    return QString();
  }
  char buf[25];
  strncpy(buf, id, sizeof(buf) - 1);
  char *rd = buf;
  char *start = nullptr;
  unsigned int len = 0;

  while (*rd && len - 1 < sizeof(buf)) {
    if (*rd == '[') {
      start = rd++;
    } else if (*rd == ']') {
      *rd = 0;
      break;
    }
    ++rd;
    len++;
  }
  if (start == nullptr) {
    start = const_cast<char *>(id);
  }

  return QString(start);
}

int ProtocolDock::get_output_protocol_by_id(QString id) {
  int dex = 0;

  for (auto info : _decoderInfoList) {
    srd_decoder *dec = (srd_decoder *)(info->_data_handle);
    if (dec->outputs) {
      QString output_id = parse_protocol_id((char *)dec->outputs->data);
      if (output_id == id) {
        QString proid(dec->id);
        if (!proid.startsWith("0:") || output_id == proid) {
          return dex;
        }
      }
    }

    ++dex;
  }

  return -1;
}

void ProtocolDock::BeginEditKeyword() { show_protocol_select(); }

void ProtocolDock::show_protocol_select() {
  SearchComboBox *panel = new SearchComboBox(this);

  for (auto info : _decoderInfoList) {
    srd_decoder *dec = (srd_decoder *)(info->_data_handle);
    panel->AddDataItem(QString(dec->id), QString(dec->name), info,
                       dec->is_c_decoder);
  }

  // Set the filter to match the current combo selection
  panel->SetFilterIndex(_pro_type_combo->currentIndex());

  QFont font = dock_font_content();
  ui::set_dock_form_font(panel);

  panel->SetItemClickHandle(this);
  panel->ShowDlg(_pro_keyword_edit);
}

void ProtocolDock::OnItemClick(void *sender, void *data_handle) {
  (void)sender;

  if (data_handle != nullptr) {
    DecoderInfoItem *info = (DecoderInfoItem *)data_handle;
    srd_decoder *dec = (srd_decoder *)(info->_data_handle);
    this->_pro_keyword_edit->SetInputText(QString(dec->name));
    _selected_protocol_id = QString(dec->id);
    this->on_add_protocol();
  }
}

void ProtocolDock::reset_view() {
  decoded_progress(0);
  update();
}

void ProtocolDock::update_view_status() {
  bool bEnable = _capture->is_working() == false;
  _pro_keyword_edit->setEnabled(bEnable);
  _pro_add_button->setEnabled(bEnable);
  _pro_type_combo->setEnabled(bEnable);
}

void ProtocolDock::update_deocder_item_name(void *trace_handel,
                                            const char *name) {
  for (auto p : _protocol_lay_items) {
    if (p->_trace == trace_handel) {
      p->set_label_name(QString(name));
      break;
    }
  }
}

void ProtocolDock::rebuild_layers() { rebuild_protocol_layers(); }

void ProtocolDock::UpdateLanguage() { retranslateUi(); }

void ProtocolDock::UpdateTheme() { reStyle(); }

void ProtocolDock::UpdateFont() {
  ui::set_dock_form_font(this);
  QFont contentFont = dock_font_content();
  _table_view->setFont(contentFont);

  for (auto lay : _protocol_lay_items) {
    lay->update_font();
  }

  QFont labelFont = dock_font_label();
  this->parentWidget()->setFont(labelFont);
  _table_view->horizontalHeader()->setFont(labelFont);
  _table_view->verticalHeader()->setFont(labelFont);

  adjustPannelSize();
}

void ProtocolDock::adjustPannelSize() {
  QString str = "DECODER";
  QFont contentFont = dock_font_content();
  QFontMetrics fm(contentFont);
  QRect rc = fm.boundingRect(str);

  int lineHeight = rc.height() + 15;
  int btnHeight = _pro_add_button->sizeHint().height();
  _pro_keyword_edit->setFixedHeight(btnHeight);
  _ann_search_edit->setFixedHeight(_pre_button->sizeHint().height());
  int pannelHeight = lineHeight * _protocol_lay_items.size() + btnHeight;

  if (pannelHeight < 100) {
    pannelHeight = 100;
  }

  _top_panel->setMinimumHeight(pannelHeight);
}

void ProtocolDock::on_table_hover(const QModelIndex &index) {
  if (!_hover_delegate)
    return;
  int old_row = _hover_delegate->_hover_row;
  int new_row = index.isValid() ? index.row() : -1;
  if (old_row == new_row)
    return;
  _hover_delegate->_hover_row = new_row;
  _table_view->viewport()->update();
}

void ProtocolDock::on_table_context_menu(const QPoint &pos) {
  QModelIndex index = _table_view->indexAt(pos);
  _context_menu_index = index;

  bool has_valid_index = index.isValid();
  int row_count = _model_proxy.rowCount(QModelIndex());
  int col_count = _model_proxy.columnCount(QModelIndex());

  _copy_cell_action->setEnabled(has_valid_index);
  _copy_row_action->setEnabled(has_valid_index && col_count > 0);
  _copy_column_action->setEnabled(has_valid_index && row_count > 0);
  _copy_all_action->setEnabled(row_count > 0 && col_count > 0);
  _select_all_action->setEnabled(row_count > 0);

  _table_context_menu->exec(_table_view->viewport()->mapToGlobal(pos));
}

void ProtocolDock::copy_cell() {
  if (!_context_menu_index.isValid())
    return;
  // _context_menu_index is a proxy index; use the proxy model to get data.
  QVariant data = _model_proxy.data(_context_menu_index, Qt::DisplayRole);
  if (data.isValid()) {
    QApplication::clipboard()->setText(data.toString());
  }
}

void ProtocolDock::copy_row() {
  if (!_context_menu_index.isValid())
    return;
  int row = _context_menu_index.row();
  int col_count = _model_proxy.columnCount(QModelIndex());
  QStringList cells;
  for (int col = 0; col < col_count; col++) {
    QModelIndex idx = _model_proxy.index(row, col);
    QVariant data = _model_proxy.data(idx, Qt::DisplayRole);
    cells << (data.isValid() ? data.toString() : QString(""));
  }
  QApplication::clipboard()->setText(cells.join("\t"));
}

void ProtocolDock::copy_column() {
  if (!_context_menu_index.isValid())
    return;
  int col = _context_menu_index.column();
  int row_count = _model_proxy.rowCount(QModelIndex());
  QStringList cells;
  for (int row = 0; row < row_count; row++) {
    QModelIndex idx = _model_proxy.index(row, col);
    QVariant data = _model_proxy.data(idx, Qt::DisplayRole);
    cells << (data.isValid() ? data.toString() : QString(""));
  }
  QApplication::clipboard()->setText(cells.join("\n"));
}

void ProtocolDock::copy_all() {
  int row_count = _model_proxy.rowCount(QModelIndex());
  int col_count = _model_proxy.columnCount(QModelIndex());
  if (row_count == 0 || col_count == 0)
    return;

  // Build header row from model headerData.
  QStringList headers;
  for (int col = 0; col < col_count; col++) {
    QVariant header = _model_proxy.headerData(col, Qt::Horizontal, Qt::DisplayRole);
    headers << (header.isValid() ? header.toString() : QString(""));
  }

  QStringList lines;
  lines << headers.join("\t");

  for (int row = 0; row < row_count; row++) {
    QStringList cells;
    for (int col = 0; col < col_count; col++) {
      QModelIndex idx = _model_proxy.index(row, col);
      QVariant data = _model_proxy.data(idx, Qt::DisplayRole);
      cells << (data.isValid() ? data.toString() : QString(""));
    }
    lines << cells.join("\t");
  }

  QApplication::clipboard()->setText(lines.join("\n"));
}

void ProtocolDock::select_all_rows() {
  _table_view->selectAll();
}

bool ProtocolDock::eventFilter(QObject *obj, QEvent *event) {
  if (obj == _table_view->viewport() && event->type() == QEvent::Leave) {
    if (_hover_delegate && _hover_delegate->_hover_row != -1) {
      _hover_delegate->_hover_row = -1;
      _table_view->viewport()->update();
    }
  }
  return QScrollArea::eventFilter(obj, event);
}

void ProtocolDock::on_follow_viewport_toggled(bool checked) {
  _follow_viewport = checked;
  if (!checked) {
    _decoder_model->clear_visible_range();
  } else {
    // Immediately apply current viewport state; subsequent scale/offset
    // changes arrive via visible_range_changed.
    on_visible_range_changed();
  }
}

void ProtocolDock::on_visible_range_changed() {
  if (!_follow_viewport || !_view || !_decoder_model) {
    return;
  }

  // ===== Multi-stack (All) mode: binary search the merged list =====
  if (_decoder_model->isMultiStackMode()) {
    const auto &decode_sigs = _decoder_host->get_decoder_stacks();
    if (decode_sigs.empty())
      return;

    uint64_t samplerate = decode_sigs[0]->samplerate();
    if (samplerate == 0)
      return;

    int viewport_width = _view->viewport()->width();
    if (viewport_width <= 0)
      return;

    double scale = _view->scale();
    int64_t offset = _view->offset();
    double samples_per_pixel = (double)samplerate * scale;
    double start_sample_d = (double)offset * samples_per_pixel;
    double end_sample_d = (double)(offset + viewport_width) * samples_per_pixel;
    if (start_sample_d < 0)
      start_sample_d = 0;
    if (end_sample_d < 0)
      end_sample_d = 0;

    uint64_t start_sample = (uint64_t)start_sample_d;
    uint64_t end_sample = (uint64_t)end_sample_d;
    if (end_sample <= start_sample)
      return;

    // Binary search the merged list by sample range.
    _decoder_model->set_visible_range_by_samples(start_sample, end_sample);

    // Update match count to reflect the new visible range.
    _matchs_label->setText(QString::number(_model_proxy.rowCount()));

    if (_jumping_to_row) {
      QTimer::singleShot(150, this, [this]() { _jumping_to_row = false; });
    }
    return;
  }

  // ===== Single-stack mode =====
  auto decoder_stack = _decoder_model->getDecoderStack();
  if (!decoder_stack) {
    return;
  }

  uint64_t samplerate = decoder_stack->samplerate();
  if (samplerate == 0) {
    return;
  }

  int viewport_width = _view->viewport()->width();
  if (viewport_width <= 0) {
    return;
  }

  double scale = _view->scale();
  int64_t offset = _view->offset();
  double samples_per_pixel = (double)samplerate * scale;

  double start_sample_d = (double)offset * samples_per_pixel;
  double end_sample_d = (double)(offset + viewport_width) * samples_per_pixel;
  if (start_sample_d < 0)
    start_sample_d = 0;
  if (end_sample_d < 0)
    end_sample_d = 0;

  uint64_t start_sample = (uint64_t)start_sample_d;
  uint64_t end_sample = (uint64_t)end_sample_d;
  if (end_sample <= start_sample) {
    return;
  }

  std::map<const pv::data::decode::Row, bool> rows =
      decoder_stack->get_rows_lshow();
  int column = _nav_column;
  bool found_row = false;
  pv::data::decode::Row target_row;
  for (auto it = rows.begin(); it != rows.end(); ++it) {
    if (it->second && column-- == 0) {
      target_row = it->first;
      found_row = true;
      break;
    }
  }
  if (!found_row) {
    return;
  }

  auto range = decoder_stack->get_visible_range(target_row, start_sample,
                                                end_sample);
  int64_t start_idx = (int64_t)range.first;
  int64_t end_idx = (int64_t)range.second;

  int64_t mark = decoder_stack->get_mark_index();
  if (mark >= 0) {
    uint64_t mark_row_u = decoder_stack->get_annotation_index(target_row, mark);
    int64_t mark_row = (int64_t)mark_row_u;
    if (mark_row >= end_idx) {
      end_idx = mark_row + 1;
    } else if (mark_row < start_idx) {
      start_idx = mark_row;
    }
  }

  _decoder_model->set_visible_range(start_idx, end_idx);

  // Update match count to reflect the new visible range.
  _matchs_label->setText(QString::number(_model_proxy.rowCount()));

  if (_jumping_to_row && _jumping_target_row >= start_idx &&
      _jumping_target_row < end_idx) {
    int sliced_row = (int)(_jumping_target_row - start_idx);
    QModelIndex source_index = _decoder_model->index(
        sliced_row, _nav_column + pv::view::DecoderModel::PRESET_COLUMN_COUNT);
    QModelIndex proxy_index = _model_proxy.mapFromSource(source_index);
    if (proxy_index.isValid()) {
      _table_view->blockSignals(true);
      _table_view->setCurrentIndex(proxy_index);
      _table_view->scrollTo(proxy_index);
      _table_view->blockSignals(false);
    }
  }

  if (_jumping_to_row) {
    QTimer::singleShot(150, this, [this]() { _jumping_to_row = false; });
  }
}

//-------------------AnnotationProxyModel

bool AnnotationProxyModel::filterAcceptsRow(
    int source_row, const QModelIndex &source_parent) const {
  QString filter = filterRegularExpression().pattern();
  if (filter.isEmpty())
    return true; // no filter: accept all rows

  QAbstractItemModel *src = sourceModel();
  if (!src)
    return true;

  int cols = src->columnCount(source_parent);
  // Skip preset columns (Start=0, Duration=1); only search annotation data
  // columns (index >= PRESET_COLUMN_COUNT).
  for (int col = pv::view::DecoderModel::PRESET_COLUMN_COUNT; col < cols;
       col++) {
    QModelIndex idx = src->index(source_row, col, source_parent);
    QString text = src->data(idx, Qt::DisplayRole).toString();
    if (text.contains(filter, Qt::CaseInsensitive))
      return true;
  }
  return false;
}

//-------------------Key navigation

void ProtocolDock::keyPressEvent(QKeyEvent *event) {
  if (event->modifiers() == Qt::ControlModifier &&
      event->key() == Qt::Key_F) {
    _ann_search_edit->setFocus();
    _ann_search_edit->selectAll();
    return;
  }
  if (event->key() == Qt::Key_Escape && _ann_search_edit->hasFocus()) {
    _ann_search_edit->clear();
    _ann_search_edit->clearFocus();
    return;
  }
  pv::widgets::SmoothScrollArea::keyPressEvent(event);
}

//-------------------Header context menu (column show/hide)

void ProtocolDock::on_header_context_menu(const QPoint &pos) {
  int col_count = _model_proxy.columnCount();
  if (col_count <= 0)
    return;

  QMenu menu(this);
  for (int i = 0; i < col_count; i++) {
    QVariant header =
        _model_proxy.headerData(i, Qt::Horizontal, Qt::DisplayRole);
    QString name = header.toString();
    if (name.isEmpty())
      continue;

    QAction *action = menu.addAction(name);
    action->setCheckable(true);
    action->setChecked(!_table_view->isColumnHidden(i));
    const int col = i;
    connect(action, &QAction::toggled, this, [this, col](bool checked) {
      _table_view->setColumnHidden(col, !checked);
    });
  }
  menu.exec(_table_view->horizontalHeader()->mapToGlobal(pos));
}

//-------------------Decoder combo switch

void ProtocolDock::on_decoder_combo_changed(int index) {
  const auto &decode_sigs = _decoder_host->get_decoder_stacks();

  // Index 0 = "All" mode; index 1+ = individual decoder (index-1).
  if (index == 0) {
    // Multi-stack mode: show all decoders' annotations.
    std::vector<pv::data::DecoderStack *> stacks;
    for (auto &s : decode_sigs)
      stacks.push_back(s.get());
    _decoder_model->setAllStacks(stacks);
  } else {
    int stack_idx = index - 1; // offset by "All"
    if (stack_idx < 0 || stack_idx >= (int)decode_sigs.size())
      return;
    _decoder_model->setDecoderStack(decode_sigs[stack_idx].get());
  }

  // Clear visible range — different decoders have different sample ranges.
  _decoder_model->clear_visible_range();

  // Force the proxy model to re-evaluate its filter mapping against the
  // new source model contents.
  _model_proxy.invalidate();

  // Reset the table view to clear stale selection/scroll state.
  _table_view->reset();

  // Re-apply the viewport filter when in follow-viewport mode.
  // Without this, switching to "All" and back leaves the model in
  // full-list mode despite _follow_viewport being true, causing the
  // table to show all data instead of only the viewport slice.
  if (_follow_viewport) {
    on_visible_range_changed();
  }

  search_done();
  resize_table_view(_decoder_model);
}

} // namespace dock
} // namespace pv
