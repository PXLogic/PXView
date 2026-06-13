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

#ifndef PXVIEW_PV_TOOLBARS_SAMPLINGBAR_H
#define PXVIEW_PV_TOOLBARS_SAMPLINGBAR_H

#include "../sigsession.h"

#include <stdint.h>
#include <list>
#include <map>
#include <QDialog>
#include <QToolBar> 
#include <QAction>
#include <QMenu>
#include "../ui/dscombobox.h"
#include "../interface/icallbacks.h"
#include "../ui/xtoolbutton.h"
#include "../ui/uimanager.h"
#include "../interface/icontextaware.h"
#include <QLabel>

struct st_dev_inst;
class QAction;
class QGroupBox;
class QButtonGroup;
class QRadioButton;
struct ds_device_info;

class DeviceAgent;

namespace pv
{  
    class SigSession;

    namespace view{
        class View;
    }

    namespace dialogs
    {
        class deviceoptions;
        class Calibration;
    }

    namespace toolbars
    {

        class SamplingBar : public QToolBar, public IContextAware, public IUiWindow
        {
            Q_OBJECT

        private:
            static const int ComboBoxMaxWidth = 200;
            static const int RefreshShort = 500;
            static const uint64_t LogicMaxSWDepth64 = SR_GB(16);
            static const uint64_t LogicMaxSWDepth32 = SR_GB(8);
            
            static const uint64_t AnalogMaxSWDepth = SR_Mn(100);
            static const QString RLEString;
            static const QString DIVString;
            static const uint64_t ZeroTimeBase = SR_US(2);
 

        public:
            SamplingBar(SigSession *session, QWidget *parent);
            ~SamplingBar();    

            double hori_knob(int dir);           
            double get_hori_res();          
            void update_device_list();          
            void reload(); 
            void update_view_status();
            void config_device();
            ds_device_handle get_next_device_handle();

            inline void set_view(view::View *view){
                _view = view;
            }

            void run_or_stop();

            void device_selected();

            void run_or_stop_instant();

            QWidget* createSamplingSettingsWidget(QWidget *parent);

            inline void update_sample_rate_list()
            {
                update_sample_rate_selector();
            }

            void update_sample_count_selector();

            void commit_settings();

            void bind_context(TabContext *ctx) override;
            void unbind_context() override;

            void set_context(SigSession *session, pv::view::View *view);
            void set_readonly(bool readonly);

        signals:
            void sig_store_session_data();

        private:
            void retranslateUi();
            void reStyle();
            void set_sample_rate(uint64_t sample_rate);
            double commit_hori_res();

            void update_sample_rate_selector();
           
            void update_sample_rate_selector_value();
            void update_sample_count_selector_value();         
            void setting_adj();
            void enable_toggle(bool enable);
            void update_mode_icon();

            bool action_run_stop();
            bool action_instant_stop();
            
            //IUiWindow
            void UpdateLanguage() override;
            void UpdateTheme() override;
            void UpdateFont() override;

            void set_sample_count_index(int index);

            void apply_sample_count(double &hori_res);

        private slots:
            void on_collect_mode();
            void on_mode_radio_clicked(int id);
            void on_run_stop();
            void on_instant_stop();
            void on_device_selected();
            void on_samplerate_sel(int index);
            void on_samplecount_sel(int index);
            void zero_adj();
            void on_run_stop_action();
            void on_instant_stop_action();    

        // private:
        public:
            SigSession          *_session;
            QLabel              *_device_type_label;
            XToolButton         _device_type;
            DsComboBox          _device_selector;
            DsComboBox          _sample_count;
            DsComboBox          _sample_rate;
            XToolButton         _mode_button;

            QAction             *_mode_action;
         
            QMenu               *_mode_menu;
            QAction             *_action_repeat;
            QAction             *_action_single;
            QAction             *_action_loop;
        
            DeviceAgent         *_device_agent;
            ds_device_handle    _last_device_handle;
            ds_device_handle    _next_switch_device;
            int                 _last_device_index;
            bool                _is_run_as_instant;
            bool                _is_readonly;
            view::View          *_view;
            TabContext          *_context;

            QButtonGroup        *_mode_group;
            QRadioButton        *_radio_single;
            QRadioButton        *_radio_repeat;
            QRadioButton        *_radio_loop;
            bool                _updating_sample_rate;
            bool                _updating_sample_count;
            bool                _updating_device_list;
            QLabel              *_settings_title_label = nullptr;
            QLabel              *_dev_label = nullptr;
            QLabel              *_depth_label = nullptr;
            QLabel              *_rate_label = nullptr;
            QLabel              *_mode_label = nullptr;
        };

    } // namespace toolbars
} // namespace pv

#endif // PXVIEW_PV_TOOLBARS_SAMPLINGBAR_H
