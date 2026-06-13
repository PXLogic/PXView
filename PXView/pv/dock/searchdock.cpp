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

#include "searchdock.h"
#include "../data/logicsnapshot.h"
#include "../data/snapshot.h"
#include "../sigsession.h"
#include "../view/logicsignal.h"
#include "../view/view.h"
#include "../widgets/smoothtablehelper.h"

#include "../appcontrol.h"
#include "../config/appconfig.h"
#include "../data/sessiondocument.h"
#include "../tabcontext.h"
#include "../ui/dockfonts.h"
#include "../ui/fn.h"
#include "../ui/iconcache.h"
#include "../ui/langresource.h"
#include "../ui/msgbox.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QPainter>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrent>
#include <stdint.h>

namespace pv {
namespace dock {

using namespace pv::view;

static const int kMaxResults = 100000;

// ============================================================================
// SearchResultModel 实现
// ============================================================================

SearchResultModel::SearchResultModel(std::vector<SearchData> &results,
                                     QMutex &mutex, QObject *parent)
    : QAbstractTableModel(parent), _results(results), _mutex(mutex),
      _current_count(0), _samplerate(0) {}

int SearchResultModel::rowCount(const QModelIndex & /*parent*/) const {
  return _current_count;
}

int SearchResultModel::columnCount(const QModelIndex & /*parent*/) const {
  return 3;
}

QVariant SearchResultModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || role != Qt::DisplayRole)
    return QVariant();

  int row = index.row();
  int col = index.column();

  QMutexLocker locker(&_mutex);
  if (row >= (int)_results.size())
    return QVariant();

  const SearchData &sd = _results[row];

  if (col == 0)
    return QString::number(row + 1);
  if (col == 1)
    return format_time(sd.start);
  if (col == 2)
    return format_time(sd.end - sd.start + 1);

  return QVariant();
}

QVariant SearchResultModel::headerData(int section, Qt::Orientation orientation,
                                       int role) const {
  if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
    return QVariant();

  if (section == 0)
    return QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_COL_INDEX), "序号"));
  if (section == 1)
    return QString(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_COL_TIME), "起始时间"));
  if (section == 2)
    return QString(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_COL_DURATION), "持续时间"));

  return QVariant();
}

void SearchResultModel::updateRowCount(int newCount) {
  if (newCount > _current_count) {
    beginInsertRows(QModelIndex(), _current_count, newCount - 1);
    _current_count = newCount;
    endInsertRows();
  } else if (newCount == 0 && _current_count > 0) {
    beginResetModel();
    _current_count = 0;
    endResetModel();
  } else if (newCount < _current_count) {
    beginRemoveRows(QModelIndex(), newCount, _current_count - 1);
    _current_count = newCount;
    endRemoveRows();
  }
}

void SearchResultModel::clear() {
  if (_current_count > 0) {
    beginResetModel();
    _current_count = 0;
    endResetModel();
  }
}

void SearchResultModel::set_samplerate(uint64_t samplerate) {
  _samplerate = samplerate;
  if (_current_count > 0) {
    emit dataChanged(index(0, 1), index(_current_count - 1, 2));
    headerDataChanged(Qt::Horizontal, 0, 2);
  }
}

QString SearchResultModel::format_time(int64_t sample) const {
  if (_samplerate == 0)
    return QString::number(sample);

  double seconds = (double)sample / (double)_samplerate;

  if (seconds >= 1.0)
    return QString::number(seconds, 'f', 6) + "s";
  if (seconds >= 1e-3)
    return QString::number(seconds * 1e3, 'f', 5) + "ms";
  if (seconds >= 1e-6)
    return QString::number(seconds * 1e6, 'f', 3) + "us";
  return QString::number(seconds * 1e9, 'f', 2) + "ns";
}

// ============================================================================
// SearchDock 实现
// ============================================================================

SearchDock::SearchDock(QWidget *parent, View *view, SigSession *session)
    : pv::widgets::SmoothScrollArea(parent), _session(session), _view(view),
      _context(nullptr), _widget(nullptr), _pattern_input(nullptr),
      _result_view(nullptr), _result_model(nullptr), _legend_x(nullptr),
      _legend_r(nullptr), _legend_0(nullptr), _legend_f(nullptr),
      _legend_1(nullptr), _legend_c(nullptr), _logic_channel_count(0),
      _time_search_pre_button(nullptr), _time_search_nxt_button(nullptr),
      _time_search_edit(nullptr), _time_search_cur_index(-1), _search_state(0) {
  _widget = new QWidget(this);

  _pattern_input = new widgets::SearchPatternInput(_widget);
  connect(_pattern_input, &widgets::SearchPatternInput::pattern_changed, this,
          &SearchDock::on_pattern_changed);

  QHBoxLayout *input_layout = new QHBoxLayout();
  input_layout->addStretch(1);
  input_layout->addWidget(_pattern_input);
  input_layout->addStretch(1);

  // 创建 Model 和 View
  _result_model =
      new SearchResultModel(_search_results, _results_mutex, _widget);

  _result_view = new QTableView(_widget);
  _result_view->setModel(_result_model);

  _result_view->horizontalHeader()->setSectionResizeMode(
      0, QHeaderView::Interactive);
  _result_view->setColumnWidth(0, 60);
  _result_view->horizontalHeader()->setSectionResizeMode(1,
                                                         QHeaderView::Stretch);
  _result_view->horizontalHeader()->setSectionResizeMode(2,
                                                         QHeaderView::Stretch);
  _result_view->setSelectionBehavior(QAbstractItemView::SelectRows);
  _result_view->setSelectionMode(QAbstractItemView::SingleSelection);
  _result_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
  _result_view->verticalHeader()->setVisible(false);
  _result_view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
  _result_view->verticalHeader()->setDefaultSectionSize(36);
  _result_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  _result_view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  _result_view->setShowGrid(false);

  _result_view->horizontalHeader()->setHighlightSections(false);
  _result_view->setFrameShape(QFrame::StyledPanel);
  _result_view->setMinimumWidth(0);
  _result_view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  _result_view->horizontalHeader()->setVisible(true);
  _result_view->setObjectName("dock_search_result_view");
  _result_view->setMouseTracking(true);

  new pv::widgets::SmoothTableHelper(_result_view, this);

  _hover_delegate = new RowHoverDelegate();
  _result_view->setItemDelegate(_hover_delegate);

  _result_view->viewport()->installEventFilter(this);

  connect(_result_view, &QAbstractItemView::clicked, this,
          &SearchDock::on_result_clicked);
  connect(_result_view, &QAbstractItemView::entered, this,
          &SearchDock::on_table_hover);

  _legend_x = new QLabel(_widget);
  _legend_x->setObjectName("dock_label");
  _legend_r = new QLabel(_widget);
  _legend_r->setObjectName("dock_label");
  _legend_0 = new QLabel(_widget);
  _legend_0->setObjectName("dock_label");
  _legend_f = new QLabel(_widget);
  _legend_f->setObjectName("dock_label");
  _legend_1 = new QLabel(_widget);
  _legend_1->setObjectName("dock_label");
  _legend_c = new QLabel(_widget);
  _legend_c->setObjectName("dock_label");

  QFont legendFont = dock_font_label();

  QColor legendColor = AppConfig::Instance().GetThemeColor("@fg-muted");
  if (!legendColor.isValid())
    legendColor = QColor("#8e8e8e");

  QList<QLabel *> legendLabels = {_legend_x, _legend_r, _legend_0,
                                  _legend_f, _legend_1, _legend_c};
  for (auto *label : legendLabels) {
    label->setFont(legendFont);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, legendColor);
    label->setPalette(pal);
  }

  QVBoxLayout *col1 = new QVBoxLayout();
  col1->setSpacing(5);
  col1->setContentsMargins(0, 0, 0, 0);
  col1->addWidget(_legend_x);
  col1->addWidget(_legend_r);

  QVBoxLayout *col2 = new QVBoxLayout();
  col2->setSpacing(5);
  col2->setContentsMargins(0, 0, 0, 0);
  col2->addWidget(_legend_0);
  col2->addWidget(_legend_f);

  QVBoxLayout *col3 = new QVBoxLayout();
  col3->setSpacing(5);
  col3->setContentsMargins(0, 0, 0, 0);
  col3->addWidget(_legend_1);
  col3->addWidget(_legend_c);

  QHBoxLayout *legend_layout = new QHBoxLayout();
  legend_layout->setSpacing(0);
  legend_layout->setContentsMargins(0, 0, 0, 0);
  legend_layout->addLayout(col1);
  legend_layout->addStretch(1);
  legend_layout->addLayout(col2);
  legend_layout->addStretch(1);
  legend_layout->addLayout(col3);

  // Time search bar: [←] [time input] [→]
  _time_search_pre_button = new QPushButton(_widget);
  _time_search_pre_button->setObjectName("dock_content");
  _time_search_pre_button->setFlat(true);

  _time_search_nxt_button = new QPushButton(_widget);
  _time_search_nxt_button->setObjectName("dock_content");
  _time_search_nxt_button->setFlat(true);

  _time_search_edit = new QLineEdit(_widget);
  _time_search_edit->setObjectName("dock_content");

  QHBoxLayout *time_search_layout = new QHBoxLayout();
  time_search_layout->setSpacing(2);
  time_search_layout->addWidget(_time_search_pre_button);
  time_search_layout->addWidget(_time_search_edit, 1);
  time_search_layout->addWidget(_time_search_nxt_button);

  connect(_time_search_pre_button, &QPushButton::clicked, this,
          &SearchDock::on_time_search_pre);
  connect(_time_search_nxt_button, &QPushButton::clicked, this,
          &SearchDock::on_time_search_nxt);
  connect(_time_search_edit, &QLineEdit::returnPressed, this,
          &SearchDock::on_time_search_return);

  QVBoxLayout *main_layout = new QVBoxLayout(_widget);
  main_layout->setContentsMargins(12, 8, 12, 8);
  main_layout->addLayout(input_layout);
  main_layout->addLayout(legend_layout);
  main_layout->addLayout(time_search_layout);
  main_layout->addWidget(_result_view, 1);

  _widget->setLayout(main_layout);

  this->setFrameShape(QFrame::NoFrame);
  this->setObjectName("dock_search_scroll");
  this->setWidgetResizable(true);
  this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  this->setWidget(_widget);
  _widget->setObjectName("searchWidget");

  connect(_session->device_event_object(), &DeviceEventObject::device_updated,
          this, &SearchDock::on_device_updated);

  connect(this, &SearchDock::search_result_found, this,
          &SearchDock::refresh_ui_model, Qt::QueuedConnection);

  connect(&_search_watcher, &QFutureWatcher<void>::finished, this,
          &SearchDock::on_search_finished);

  retranslateUi();
  rebuild_pattern();

  ADD_UI(this);
}

SearchDock::~SearchDock() {
  stop_search();
  REMOVE_UI(this);
}

void SearchDock::set_view(view::View *view) { _view = view; }

void SearchDock::bind_context(TabContext *ctx) {
  assert(ctx);
  _context = ctx;
  _session = ctx->session();
  _view = ctx->view();
  stop_search();
  _results_mutex.lock();
  _search_results.clear();
  _results_mutex.unlock();
  _result_model->clear();
  _result_model->set_samplerate(_session->cur_snap_samplerate());
  _time_search_cur_index = -1;
  if (ctx && ctx->document()) {
    auto &saved = ctx->document()->_dock_search_pattern;
    if (!saved.empty()) {
      _pattern = saved;
    }
  }
  rebuild_pattern();
}

void SearchDock::unbind_context() {
  if (_context && _context->document()) {
    _context->document()->_dock_search_pattern = _pattern;
  }
  _context = nullptr;
  stop_search();
  _results_mutex.lock();
  _search_results.clear();
  _results_mutex.unlock();
  _result_model->clear();
  _time_search_cur_index = -1;
}

void SearchDock::rebuild_pattern() {
  int count = 0;
  for (auto s : _session->get_signals()) {
    if (s->signal_type() == SR_CHANNEL_LOGIC)
      count++;
  }

  _logic_channel_count = count;

  _pattern_input->set_channel_count(count);
  _pattern_input->set_pattern(_pattern);

  std::set<uint16_t> active_indices;
  for (auto s : _session->get_signals()) {
    if (s->signal_type() == SR_CHANNEL_LOGIC)
      active_indices.insert(s->get_index());
  }
  for (auto it = _pattern.begin(); it != _pattern.end();) {
    if (active_indices.find(it->first) == active_indices.end())
      it = _pattern.erase(it);
    else
      ++it;
  }
}

void SearchDock::on_pattern_changed() {
  _pattern = _pattern_input->get_pattern();
  _view->set_search_pos(_view->get_search_pos(), false);

  // Debounce search to avoid lag during rapid input
  QTimer::singleShot(150, this, &SearchDock::do_search);
}

void SearchDock::on_device_updated() {
  rebuild_pattern();
  _result_model->set_samplerate(_session->cur_snap_samplerate());
}

void SearchDock::on_frame_ended() {
  if (!_pattern.empty()) {
    do_search();
  }
}

int64_t SearchDock::find_match_end(data::LogicSnapshot *snapshot,
                                   int64_t start_pos) {
  const int64_t end = snapshot->get_sample_count() - 1;
  bool has_edge = false;
  for (auto &it : _pattern) {
    QChar ch = it.second.at(0).toUpper();
    if (ch == 'R' || ch == 'F' || ch == 'C') {
      has_edge = true;
      break;
    }
  }

  if (has_edge)
    return start_pos;

  int64_t pos = start_pos + 1;
  while (pos <= end) {
    bool match = true;
    for (auto &it : _pattern) {
      QChar ch = it.second.at(0).toUpper();
      int sig_index = it.first;
      if (ch == '0') {
        if (snapshot->get_sample(pos, sig_index)) {
          match = false;
          break;
        }
      } else if (ch == '1') {
        if (!snapshot->get_sample(pos, sig_index)) {
          match = false;
          break;
        }
      }
    }
    if (!match)
      break;
    pos++;
  }
  return pos - 1;
}

void SearchDock::stop_search() {
  int expected = 1;
  if (_search_state.compare_exchange_strong(expected, 3)) {
    // State was running, set to stop requested
  } else {
    expected = 2;
    if (_search_state.compare_exchange_strong(expected, 3)) {
      // State was params changed, set to stop requested
    }
  }

  _search_watcher.waitForFinished();

  _results_mutex.lock();
  _search_state.store(0);
  _results_mutex.unlock();
}

void SearchDock::start_search_async() {
  stop_search();

  _results_mutex.lock();
  _search_results.clear();
  // 预分配内存，避免搜索过程中频繁重分配导致锁竞争
  _search_results.reserve(kMaxResults);
  _results_mutex.unlock();

  // 清空 UI
  _result_model->clear();
  _time_search_cur_index = -1;

  _search_state.store(1);
  _search_future = QtConcurrent::run([this]() { search_worker(); });
  _search_watcher.setFuture(_search_future);
}

void SearchDock::search_worker() {
  const auto snapshot = _session->get_snapshot(SR_CHANNEL_LOGIC);
  if (!snapshot)
    return;
  const auto logic_snapshot = dynamic_cast<data::LogicSnapshot *>(snapshot);
  if (!logic_snapshot || logic_snapshot->empty()) {
    _search_state.store(0);
    return;
  }

  std::map<uint16_t, QString> local_pattern;
  {
    _results_mutex.lock();
    local_pattern = _pattern;
    _results_mutex.unlock();
  }

  const int64_t end = logic_snapshot->get_sample_count() - 1;
  int64_t pos = 0;

  std::vector<SearchData> local_batch;
  local_batch.reserve(1000);

  QElapsedTimer ui_timer;
  ui_timer.start();
  bool has_new_results = false;
  bool first_flush = true;

  if (gpu_edge_search_worker(logic_snapshot, end, local_pattern, local_batch,
                             ui_timer, has_new_results, first_flush)) {
    if (!local_batch.empty()) {
      _results_mutex.lock();
      if ((int)_search_results.size() < kMaxResults) {
        _search_results.insert(_search_results.end(), local_batch.begin(),
                               local_batch.end());
      }
      _results_mutex.unlock();
    }
    if (has_new_results) {
      emit search_result_found();
    }
    _search_state.store(0);
    return;
  }

  while (pos <= end) {
    int state = _search_state.load();
    if (state == 3) {
      break;
    }

    bool ret = logic_snapshot->pattern_search(0, end, pos, local_pattern, true);
    if (!ret)
      break;

    int64_t match_end = find_match_end(logic_snapshot, pos);

    local_batch.push_back(SearchData(pos, match_end));
    has_new_results = true;

    bool should_flush =
        first_flush ? (local_batch.size() >= 40) : (ui_timer.elapsed() >= 500);

    if (should_flush) {
      _results_mutex.lock();
      _search_results.insert(_search_results.end(), local_batch.begin(),
                             local_batch.end());
      _results_mutex.unlock();

      local_batch.clear();

      ui_timer.restart();
      emit search_result_found();
      first_flush = false;

      if ((int)_search_results.size() >= kMaxResults)
        break;
    }

    pos = match_end + 1;
  }

  if (!local_batch.empty()) {
    _results_mutex.lock();
    if ((int)_search_results.size() < kMaxResults) {
      _search_results.insert(_search_results.end(), local_batch.begin(),
                             local_batch.end());
    }
    _results_mutex.unlock();
  }

  if (has_new_results) {
    emit search_result_found();
  }

  _search_state.store(0);
}

bool SearchDock::gpu_edge_search_worker(
    data::LogicSnapshot *logic_snapshot, int64_t end,
    const std::map<uint16_t, QString> &local_pattern,
    std::vector<SearchData> &local_batch, QElapsedTimer &ui_timer,
    bool &has_new_results, bool &first_flush) {
  (void)logic_snapshot;
  (void)end;
  (void)local_batch;
  (void)ui_timer;
  (void)has_new_results;
  (void)first_flush;

  if (!_view)
    return false;

  bool has_edge_pattern = false;
  for (auto &it : local_pattern) {
    QChar ch = it.second.at(0).toUpper();
    if (ch == 'R' || ch == 'F' || ch == 'C') {
      has_edge_pattern = true;
      break;
    }
  }

  if (!has_edge_pattern)
    return false;

  return false;
}

void SearchDock::do_search() {
  _result_model->set_samplerate(_session->cur_snap_samplerate());
  start_search_async();
}

void SearchDock::on_search_finished() {
  // 确保最后一次数据更新到 UI
  refresh_ui_model();
}

void SearchDock::refresh_ui_model() {
  _results_mutex.lock();
  int current_size = (int)_search_results.size();
  _results_mutex.unlock();

  _result_model->updateRowCount(current_size);
}

void SearchDock::on_result_clicked(const QModelIndex &index) {
  if (!index.isValid())
    return;

  int row = index.row();

  _results_mutex.lock();
  if (row >= 0 && row < (int)_search_results.size()) {
    int64_t start_pos = _search_results[row].start;
    _results_mutex.unlock();
    _time_search_cur_index = row;
    _view->set_search_pos(start_pos, true);
  } else {
    _results_mutex.unlock();
  }
}

void SearchDock::on_table_hover(const QModelIndex &index) {
  if (!_hover_delegate)
    return;
  int old_row = _hover_delegate->_hover_row;
  int new_row = index.isValid() ? index.row() : -1;
  if (old_row == new_row)
    return;
  _hover_delegate->_hover_row = new_row;
  _result_view->viewport()->update();
}

bool SearchDock::eventFilter(QObject *obj, QEvent *event) {
  if (obj == _result_view->viewport() && event->type() == QEvent::Leave) {
    if (_hover_delegate && _hover_delegate->_hover_row != -1) {
      _hover_delegate->_hover_row = -1;
      _result_view->viewport()->update();
    }
  }
  return QWidget::eventFilter(obj, event);
}

void SearchDock::retranslateUi() {
  _legend_x->setText(QString(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_LABEL_X), "X: Don't care")));
  _legend_r->setText(QString(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_LABEL_R), "R: Rising edge")));
  _legend_0->setText(
      QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_LABEL_0), "0: Low level")));
  _legend_f->setText(QString(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_LABEL_F), "F: Falling edge")));
  _legend_1->setText(QString(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_LABEL_1), "1: High level")));
  _legend_c->setText(QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_LABEL_C),
                                 "C: Rising/Falling edge")));

  _time_search_edit->setPlaceholderText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_TIME_HINT),
          "输入时间(如 1.5us, 100ns, 2ms)或序号并按回车键进行搜索"));

  // 更新表头
  _result_model->headerDataChanged(Qt::Horizontal, 0, 2);
}

void SearchDock::reStyle() {
  QString iconPath = GetIconPath();
  _time_search_pre_button->setIcon(
      IconCache::Instance().icon(iconPath + "/pre.svg"));
  _time_search_nxt_button->setIcon(
      IconCache::Instance().icon(iconPath + "/next.svg"));
}

void SearchDock::UpdateLanguage() { retranslateUi(); }

void SearchDock::UpdateTheme() { reStyle(); }

void SearchDock::UpdateFont() {
  QFont font("Source Code Pro");
  font.setStyleHint(QFont::Monospace);
  font.setFixedPitch(true);
  font.setPixelSize(dock_font_content().pixelSize());
  _pattern_input->setFont(font);
  _pattern_input->update();
}

int64_t SearchDock::parse_time_text(const QString &text, bool &is_row_index) {
  // Supports formats:
  //   "123"         -> row index (1-based, matching # column)
  //   "1.5us"       -> 1.5 microseconds
  //   "100ns"       -> 100 nanoseconds
  //   "2ms"         -> 2 milliseconds
  //   "1s"          -> 1 second
  //   "1.5u"        -> 1.5 microseconds (short form)
  is_row_index = false;
  QString s = text.trimmed();
  if (s.isEmpty())
    return -1;

  // Try to parse as time string (has a unit suffix)
  QRegularExpression re(R"(^([\d.]+)\s*(s|ms|us|ns|u|n)?$)",
                        QRegularExpression::CaseInsensitiveOption);
  QRegularExpressionMatch match = re.match(s);
  if (!match.hasMatch())
    return -1;

  double value = match.captured(1).toDouble();
  QString unit = match.captured(2).toLower();

  if (unit.isEmpty()) {
    // No unit: treat as row index (1-based)
    is_row_index = true;
    return (int64_t)value;
  }

  uint64_t samplerate = 0;
  if (_session)
    samplerate = _session->cur_snap_samplerate();

  if (samplerate == 0)
    return -1;

  double seconds = 0;
  if (unit == "s") {
    seconds = value;
  } else if (unit == "ms") {
    seconds = value * 1e-3;
  } else if (unit == "us" || unit == "u") {
    seconds = value * 1e-6;
  } else if (unit == "ns" || unit == "n") {
    seconds = value * 1e-9;
  } else {
    return -1;
  }

  int64_t sample = (int64_t)(seconds * samplerate + 0.5);
  return sample;
}

int SearchDock::binary_search_time_index(int64_t sample_pos, bool find_next) {
  // Binary search in sorted _search_results by start sample position.
  // If find_next is true, returns the index of the first result with start >=
  // sample_pos. If find_next is false, returns the index of the last result
  // with start <= sample_pos.
  QMutexLocker locker(&_results_mutex);
  int n = (int)_search_results.size();
  if (n == 0)
    return -1;

  int lo = 0, hi = n - 1;
  int result = -1;

  if (find_next) {
    // Find first index where start >= sample_pos (lower_bound)
    while (lo <= hi) {
      int mid = lo + (hi - lo) / 2;
      if (_search_results[mid].start >= sample_pos) {
        result = mid;
        hi = mid - 1;
      } else {
        lo = mid + 1;
      }
    }
  } else {
    // Find last index where start <= sample_pos (upper_bound - 1)
    while (lo <= hi) {
      int mid = lo + (hi - lo) / 2;
      if (_search_results[mid].start <= sample_pos) {
        result = mid;
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
  }

  return result;
}

void SearchDock::on_time_search_return() {
  bool is_row_index = false;
  int64_t value = parse_time_text(_time_search_edit->text(), is_row_index);
  if (value < 0)
    return;

  int idx = -1;

  if (is_row_index) {
    // Pure number: treat as 1-based row index
    idx = (int)value - 1; // convert to 0-based
    _results_mutex.lock();
    int n = (int)_search_results.size();
    _results_mutex.unlock();
    if (idx < 0 || idx >= n)
      return;
  } else {
    // Time value: use binary search to find closest result
    idx = binary_search_time_index(value, true);
    if (idx < 0) {
      // No result at or after this time; try the last result
      QMutexLocker locker(&_results_mutex);
      if (!_search_results.empty())
        idx = (int)_search_results.size() - 1;
      else
        return;
    }
  }

  _time_search_cur_index = idx;

  _results_mutex.lock();
  if (idx >= 0 && idx < (int)_search_results.size()) {
    int64_t start_pos = _search_results[idx].start;
    _results_mutex.unlock();
    _result_view->selectRow(idx);
    _result_view->scrollTo(_result_model->index(idx, 0));
    _view->set_search_pos(start_pos, true);
  } else {
    _results_mutex.unlock();
  }
}

void SearchDock::on_time_search_nxt() {
  QMutexLocker locker(&_results_mutex);
  int n = (int)_search_results.size();
  if (n == 0)
    return;

  // If user typed a time, start from binary search result
  bool is_row_index = false;
  int64_t sample_pos = parse_time_text(_time_search_edit->text(), is_row_index);
  if (sample_pos >= 0 && _time_search_cur_index < 0) {
    locker.unlock();
    on_time_search_return();
    return;
  }

  int idx = _time_search_cur_index + 1;
  if (idx >= n)
    idx = 0; // wrap around

  _time_search_cur_index = idx;
  int64_t start_pos = _search_results[idx].start;
  locker.unlock();

  _result_view->selectRow(idx);
  _result_view->scrollTo(_result_model->index(idx, 0));
  _view->set_search_pos(start_pos, true);
}

void SearchDock::on_time_search_pre() {
  QMutexLocker locker(&_results_mutex);
  int n = (int)_search_results.size();
  if (n == 0)
    return;

  // If user typed a time, start from binary search result
  bool is_row_index = false;
  int64_t sample_pos = parse_time_text(_time_search_edit->text(), is_row_index);
  if (sample_pos >= 0 && _time_search_cur_index < 0) {
    locker.unlock();
    on_time_search_return();
    return;
  }

  int idx = _time_search_cur_index - 1;
  if (idx < 0)
    idx = n - 1; // wrap around

  _time_search_cur_index = idx;
  int64_t start_pos = _search_results[idx].start;
  locker.unlock();

  _result_view->selectRow(idx);
  _result_view->scrollTo(_result_model->index(idx, 0));
  _view->set_search_pos(start_pos, true);
}

} // namespace dock
} // namespace pv
