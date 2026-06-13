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

#ifndef PXVIEW_PV_SEARCHDOCK_H
#define PXVIEW_PV_SEARCHDOCK_H

#include <QAbstractTableModel>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFrame>
#include <QFuture>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMutex>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <set>
#include <vector>

#include "../interface/icallbacks.h"
#include "../interface/icontextaware.h"
#include "../ui/dscombobox.h"
#include "../ui/uimanager.h"
#include "../widgets/searchpatterninput.h"
#include "../widgets/smoothscrollarea.h"

namespace pv {

class SigSession;

namespace view {
class View;
}

namespace data {
class LogicSnapshot;
}

namespace dock {

struct SearchData {
  int64_t start;
  int64_t end;
  SearchData(int64_t s, int64_t e) : start(s), end(e) {}
};

class RowHoverDelegate : public QStyledItemDelegate {
public:
  int _hover_row = -1;

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyleOptionViewItem opt = option;
    if (index.row() == _hover_row) {
      opt.state |= QStyle::State_MouseOver;
    }
    QStyledItemDelegate::paint(painter, opt, index);
  }
};

// 自定义 Model 类，用于高效显示大量搜索结果
class SearchResultModel : public QAbstractTableModel {
  Q_OBJECT

public:
  SearchResultModel(std::vector<SearchData> &results, QMutex &mutex,
                    QObject *parent = nullptr);

  int rowCount(const QModelIndex &parent = QModelIndex()) const override;
  int columnCount(const QModelIndex &parent = QModelIndex()) const override;
  QVariant data(const QModelIndex &index,
                int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;

  void updateRowCount(int newCount);
  void clear();
  void set_samplerate(uint64_t samplerate);

private:
  QString format_time(int64_t sample) const;

  std::vector<SearchData> &_results;
  QMutex &_mutex;
  int _current_count;
  uint64_t _samplerate;
};

class SearchDock : public pv::widgets::SmoothScrollArea,
                   public IContextAware,
                   public IUiWindow {
  Q_OBJECT

public:
  SearchDock(QWidget *parent, pv::view::View *view, SigSession *session);
  ~SearchDock();

  void set_view(view::View *view);

  void bind_context(TabContext *ctx) override;
  void unbind_context() override;

  bool eventFilter(QObject *obj, QEvent *event) override;

private:
  void retranslateUi();
  void reStyle();
  void rebuild_pattern();
  int64_t find_match_end(pv::data::LogicSnapshot *snapshot, int64_t start_pos);

  void UpdateLanguage() override;
  void UpdateTheme() override;
  void UpdateFont() override;

  void stop_search();
  void start_search_async();
  void search_worker();
  bool gpu_edge_search_worker(data::LogicSnapshot *logic_snapshot, int64_t end,
                              const std::map<uint16_t, QString> &local_pattern,
                              std::vector<SearchData> &local_batch,
                              QElapsedTimer &ui_timer, bool &has_new_results,
                              bool &first_flush);
  int binary_search_time_index(int64_t sample_pos, bool find_next);
  int64_t parse_time_text(const QString &text, bool &is_row_index);

public slots:
  void on_pattern_changed();
  void on_device_updated();
  void on_frame_ended();
  void on_result_clicked(const QModelIndex &index);
  void on_table_hover(const QModelIndex &index);
  void do_search();
  void on_search_finished();
  void refresh_ui_model(); // 用于接收搜索线程的信号更新UI
  void on_time_search_nxt();
  void on_time_search_pre();
  void on_time_search_return();

signals:
  void search_result_found(); // 搜索线程发射此信号通知UI更新

private:
  SigSession *_session;
  view::View *_view;
  std::map<uint16_t, QString> _pattern;
  TabContext *_context;

  QWidget *_widget;

  widgets::SearchPatternInput *_pattern_input;

  // 使用 QTableView + Model 替代 QTableWidget
  QTableView *_result_view;
  SearchResultModel *_result_model;
  RowHoverDelegate *_hover_delegate;

  QLabel *_legend_x;
  QLabel *_legend_r;
  QLabel *_legend_0;
  QLabel *_legend_f;
  QLabel *_legend_1;
  QLabel *_legend_c;
  int _logic_channel_count;

  // Time search bar
  QPushButton *_time_search_pre_button;
  QPushButton *_time_search_nxt_button;
  QLineEdit *_time_search_edit;
  int _time_search_cur_index;

  // Search result storage
  std::vector<SearchData> _search_results;
  QMutex _results_mutex;

  // Async search state
  std::atomic<int>
      _search_state; // 0=idle, 1=running, 2=params_changed, 3=stop_requested
  QFuture<void> _search_future;
  QFutureWatcher<void> _search_watcher;
};

} // namespace dock
} // namespace pv

#endif // PXVIEW_PV_SEARCHDOCK_H
