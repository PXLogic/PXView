/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
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


#ifndef PXVIEW_PV_MAINWINDOW_H
#define PXVIEW_PV_MAINWINDOW_H

#include <list>
#include <vector>
#include <memory>
#include <QMainWindow>
#include <QTranslator> 
#include "pv/dialogs/dsmessagebox.h"
#include "pv/interface/icallbacks.h"
#include "pv/interface/events.h"
#include "pv/base/eventobject.h"
#include <QJsonDocument>
#include <chrono>
#include <QTimer>
#include "pv/base/dstimer.h"

#include <QWidgetAction>
#include<QShortcut>
#include "pv/ui/draggabletabwidget.h"
#include "pv/session/tabcontext.h"
#include "pv/widgets/slidingdrawer.h"
#include "pv/widgets/sidebar.h"
#include <map>
#include "pv/config/appconfig.h"
#include "pv/api/types.h"
#include "pv/data/document/sessiondocument.h"

class QAction;
class QMenu;
class QMenuBar;
class QVBoxLayout;
class QStatusBar;
class QToolBar;
class QWidget;
class QDockWidget;
class QLabel;
class AppControl;
class DeviceAgent;

using std::chrono::high_resolution_clock;
using std::chrono::milliseconds;

namespace pv {

class SigSession;

namespace toolbars {
class SamplingBar;
class TrigBar;
class FileBar;
class LogoBar;
class TitleBar;
}

namespace dock{
class ProtocolDock;
class TriggerDock;
class DsoTriggerDock;
class MeasureDock;
class SearchDock;
class DeviceOptionsDock;
class LogDock;
class McpControlDock;
class FunctionDock;
}

namespace view {
class View;
}

// Spec v2 Task 2: forward declarations for delegate classes (needed for
// accessor return types before unique_ptr members are declared below).
class MainWindowEventDispatcher;
class SessionEventDispatcher;
class MainWindowConfigIO;
class MainWindowSignalConnector;
class MainWindowFileOps;
class TabManager;
class DockManager;
class MainWindowThemeManager;
class MainWindowStatusBar;
class MainWindowShortcutManager;

// removed WidgetInspector fwd decl

//The mainwindow,referenced by MainFrame
//TODO: create graph view,toolbar,and show device list
class MainWindow :
    public QMainWindow,
    public IMainForm,
    public ISessionDataGetter,
    public pv::api::IServiceEventListener
{
	Q_OBJECT

public:
    static const int Min_Width  = 350;
    static const int Min_Height = 300;
    static const int Base_Height = 150;
    static const int Per_Chan_Height = 35;
     
public:
    explicit MainWindow(toolbars::TitleBar *title_bar, QWidget *parent = 0);
    ~MainWindow();

    void openDoc();

public slots: 
    void switchTheme(QString style);
    void restore_dock();
    // All on_* slots are public because delegate classes
    // (DockManager / TabManager / ShortcutManager / SignalConnector / FileOps)
    // connect signals to these from outside MainWindow.
    void on_side_bar_dock_clicked(int index);
    void on_side_bar_action_clicked(int index);
    void on_tab_changed(int index);
    void on_tab_moved(int from, int to);
    void on_tab_detach(int index, QWidget *widget, const QString &title);
    void on_tab_attached(QWidget *widget, const QString &title);
    void on_new_tab_requested();
	void on_load_file(QString file_name);
    void on_open_doc();  
    void on_screenShot();
    void on_save();
    void on_export();
    void on_import_file(QString file_name);
    bool on_load_session(QString name);  
    bool on_store_session(QString name); 
    void on_data_updated();
    void on_session_error();
    void on_signals_changed();
    void on_receive_trigger(quint64 trigger_pos);
    void on_frame_ended();
    void on_frame_began();
    void on_decode_done();
    void on_receive_data_len(quint64 len);
    void on_cur_snap_samplerate_changed();
    void on_delay_prop_msg();
    void on_load_device_first();
    // Task 1.3: ICaptureCallback methods now emit EventObject signals (cross-
    // thread safe); these on_* slots run on the GUI thread to touch the View.
    void on_update_capture();
    void on_show_region(quint64 start, quint64 end, bool keep);
    void on_show_wait_trigger();
    void on_repeat_hold(int percent);
  
signals:
    void prgRate(int progress);

public:
    //IMainForm
    void switchLanguage(int language) override;
    bool able_to_close();
    QWidget* GetBodyView();

    // Phase 2: exposed for SessionEventDispatcher
    std::map<int, pv::data::ChannelLayoutState> build_channel_layout(pv::view::View *view);
    
public:
	void setup_ui();
    void retranslateUi(); 
    bool eventFilter(QObject *object, QEvent *event);
    int resolveShortcutAction(int key, int modifiers);
    void check_usb_device_speed();
    void reset_all_view();
    bool confirm_to_store_data();
    void update_toolbar_view_status();
    void update_capture_ui_status();
    void calc_min_height();    
    void update_title_bar_text();
    void update_disk_cache_status();
    void update_fps();

    pv::view::View* current_view();
    pv::TabContext* current_context();
    void add_tab(pv::TabContext *ctx);
    void remove_tab(int index);
    void update_tab_style(int index);

    //json operation
public:
    QString gen_config_file_path(bool isNewFormat);
    bool load_config_from_file(QString file);
    bool load_config_from_json(QJsonDocument &doc, bool &haveDecoder);
    void load_device_config();
    bool gen_config_json(QJsonObject &sessionVar);
    void save_config();
    bool save_config_to_file(QString file);
    QJsonDocument get_config_json_from_data_file(QString file, bool &bSucesss);
    QJsonArray get_decoder_json_from_data_file(QString file, bool &bSucesss);
    void check_config_file_version(); 
    void load_demo_decoder_config(QString optname);

    // ---- Delegate accessors (Spec v2 Task 2) ----
    // unique_ptr delegate members are now private; delegates use these.
    MainWindowConfigIO* config_io() const { return _config_io.get(); }
    MainWindowSignalConnector* signal_connector() const { return _signal_connector.get(); }
    MainWindowFileOps* file_ops() const { return _file_ops.get(); }
    TabManager* tab_manager() const { return _tab_manager.get(); }
    DockManager* dock_manager() const { return _dock_manager.get(); }
    MainWindowThemeManager* theme_manager() const { return _theme_manager.get(); }
    MainWindowStatusBar* status_bar() const { return _status_bar.get(); }
    MainWindowShortcutManager* shortcut_manager() const { return _shortcut_manager.get(); }


public:
    // Spec v3 Task 6: delay_prop_msg has real logic (set message + start timer)
    void delay_prop_msg(QString strMsg);

private:

    //ISessionDataGetter
    bool genSessionData(std::string &str) override;

    //IServiceEventListener — receive View operation broadcasts from SessionService
    //(show_region, zoom_fit, zoom_in/out, cursor operations). In GUI mode these are
    //routed to the active View; in Headless mode there is no MainWindow so these
    //events are simply not consumed.
    void on_service_event(const pv::api::ServiceEventData &data) override;

    // ---- SessionEventDispatcher delegate ----
    // Events are registered via EventBus::subscribe<T>() in the dispatcher's
    // constructor. No IEventListener forwarding needed.
    std::unique_ptr<class SessionEventDispatcher> _event_dispatcher;

public:
    // ---- Spec v2 Task 2: Public accessors for delegate classes ----
    // Member variables are now private; delegates use these accessors.

    // Session / Device
    SigSession*& session() { return _session; }
    DeviceAgent*& device_agent() { return _device_agent; }

    // Toolbars
    toolbars::SamplingBar*& sampling_bar() { return _sampling_bar; }
    toolbars::TrigBar*& trig_bar() { return _trig_bar; }
    toolbars::FileBar*& file_bar() { return _file_bar; }
    toolbars::LogoBar*& logo_bar() { return _logo_bar; }
    toolbars::TitleBar*& title_bar() { return _title_bar; }

    // UI widgets
    QWidget*& central_widget() { return _central_widget; }
    QVBoxLayout*& vertical_layout() { return _vertical_layout; }
    QWidget*& frame() { return _frame; }
    dialogs::DSMessageBox*& msg() { return _msg; }
    EventObject& event_object() { return _event; }

    // State
    bool& is_auto_switch_device() { return _is_auto_switch_device; }
    bool& is_save_confirm_msg() { return _is_save_confirm_msg; }
    QString& pattern_mode() { return _pattern_mode; }
    QString& lst_title_string() { return _lst_title_string; }
    QString& title_ext_string() { return _title_ext_string; }
    high_resolution_clock::time_point& last_key_press_time() { return _last_key_press_time; }

    // Status labels
    QLabel*& disk_cache_status_label() { return _disk_cache_status_label; }
    QLabel*& trig_time_label() { return _trig_time_label; }
    QLabel*& fps_label() { return _fps_label; }
    QLabel*& sample_period_label() { return _sample_period_label; }

    // Timers / counters
    DsTimer& delay_prop_msg_timer() { return _delay_prop_msg_timer; }
    QString& strMsg() { return _strMsg; }
    QTimer& disk_cache_status_timer() { return _disk_cache_status_timer; }
    QTimer& fps_timer() { return _fps_timer; }
    int& acq_count() { return _acq_count; }
    int& key_value() { return _key_value; }
    bool& key_vaild() { return _key_vaild; }
    QTranslator& qtTrans() { return _qtTrans; }
    QTranslator& myTrans() { return _myTrans; }

    // Menu / categories
    QMenuBar*& menu_bar() { return _menu_bar; }
    QMenu*& category_file() { return _category_file; }
    QMenu*& category_display() { return _category_display; }
    QMenu*& category_help() { return _category_help; }
    int& category_file_index() { return _category_file_index; }
    int& category_display_index() { return _category_display_index; }
    int& category_help_index() { return _category_help_index; }

    // Methods that remain public
    void update_sample_period();
    void MainWindowRibbonHelper();
    void Ribbon_setupUi();
    void Ribbon_retranslateUi();
    void setupQuickAccessBar();
    void setupFileCategory();
    void setupDisplayCategory();
    void setupHelpCategory();

    enum {
        SIDEBAR_TRIGGER = 0,
        SIDEBAR_DECODE = 1,
        SIDEBAR_MEASURE = 2,
        SIDEBAR_SEARCH = 3,
        SIDEBAR_FUNCTION = 4,
        SIDEBAR_OPTIONS = 5,
        SIDEBAR_MCP = 6,
        SIDEBAR_LOG = 7,
        SIDEBAR_RUNSTOP = 8,
        SIDEBAR_INSTANT = 9
    };

    // Phase 2: getDockOptions forwarded to DockManager
    ::DockOptions* getDockOptions();

private:
    // ---- Member variables (private, Spec v2 Task 2) ----
    dialogs::DSMessageBox   *_msg;

	QWidget                 *_central_widget;
	QVBoxLayout             *_vertical_layout;

	toolbars::SamplingBar   *_sampling_bar;
    toolbars::TrigBar       *_trig_bar;
    toolbars::FileBar       *_file_bar;
    toolbars::LogoBar       *_logo_bar; //help button, on top right
    toolbars::TitleBar      *_title_bar;

    QTranslator     _qtTrans;
    QTranslator     _myTrans;
    EventObject     _event; 
    SigSession      *_session;
    DeviceAgent     *_device_agent;
    bool            _is_auto_switch_device;
    high_resolution_clock::time_point _last_key_press_time;
    bool            _is_save_confirm_msg;
    QString         _pattern_mode;
    QWidget         *_frame;
    DsTimer         _delay_prop_msg_timer;
    QString         _strMsg;
    QString         _lst_title_string;
    QString         _title_ext_string;

    QLabel          *_disk_cache_status_label;
    QLabel          *_trig_time_label;
    QLabel          *_fps_label;
    QLabel          *_sample_period_label;
    QTimer          _disk_cache_status_timer;
    QTimer          _fps_timer;
    int             _acq_count;

    int         _key_value;
    bool        _key_vaild;

    int _category_file_index;
    int _category_display_index;
    int _category_help_index;

    QMenuBar      *_menu_bar;
    QMenu         *_category_file;
    QMenu         *_category_display;
    QMenu         *_category_help;

    // unique_ptr delegate members (moved from public section)
    std::unique_ptr<class MainWindowConfigIO> _config_io;
    std::unique_ptr<class MainWindowSignalConnector> _signal_connector;
    std::unique_ptr<class MainWindowFileOps> _file_ops;
    std::unique_ptr<class TabManager> _tab_manager;
    std::unique_ptr<class DockManager> _dock_manager;
    std::unique_ptr<class MainWindowThemeManager> _theme_manager;
    std::unique_ptr<class MainWindowStatusBar> _status_bar;
    std::unique_ptr<class MainWindowShortcutManager> _shortcut_manager;

};

} // namespace pv

#endif // PXVIEW_PV_MAINWINDOW_H
