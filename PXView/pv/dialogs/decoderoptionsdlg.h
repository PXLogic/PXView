/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
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

#ifndef DECODER_OPTIONS_DLG_H
#define DECODER_OPTIONS_DLG_H

#include <QObject>
#include <QWidget>
#include <vector>
#include <QString>

class QGridLayout;
class DsComboBox;
class QFormLayout;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QLineEdit;
class QLabel;
class QRadioButton;
class QStackedWidget;
class QToolButton;

struct srd_channel;

#include "pv/dialogs/pxdialog.h"

namespace pv {
    namespace data{
        class DecoderStack;

        namespace decode{
            class Decoder;
        }
    }
    namespace prop{   
        namespace binding{
            class DecoderOptions;
        }
    }
    namespace view{
        class View;
        class Cursor;
        class DecodeTrace;
    }

namespace dialogs {
 

class DecoderOptionsDlg: public PxDialog
{
    Q_OBJECT

private:
	struct ProbeSelector
	{
		const DsComboBox *_combo;
        const pv::data::decode::Decoder *_decoder;
		const srd_channel *_pdch;
	};

    struct TdmFastUi
    {
        pv::data::decode::Decoder *decoder = nullptr;
        prop::binding::DecoderOptions *binding = nullptr;

        QSpinBox *bits = nullptr;
        QComboBox *channels = nullptr;
        QComboBox *align = nullptr;
        QComboBox *data_format = nullptr;
        QComboBox *edge = nullptr;
        QComboBox *frame_edge = nullptr;
        QCheckBox *realtime_decode = nullptr;
        QCheckBox *hide_decode_text = nullptr;

        QToolButton *select_button[8] = {};
        QCheckBox *wave_enable[8] = {};
        QCheckBox *text_enable[8] = {};
        QLabel *zoom_summary[8] = {};
        QLabel *pos_summary[8] = {};
        QRadioButton *trigger_channel[8] = {};

        QDoubleSpinBox *vzoom[8] = {};
        QDoubleSpinBox *vpos[8] = {};
        QComboBox *range_mode[8] = {};
        QDoubleSpinBox *eng_min[8] = {};
        QDoubleSpinBox *eng_max[8] = {};
        QLineEdit *unit[8] = {};

        QLabel *selected_label = nullptr;
        QComboBox *selected_combo = nullptr;
        QStackedWidget *advanced_stack = nullptr;
        QCheckBox *trigger_enable = nullptr;
        QComboBox *trigger_mode = nullptr;
        QComboBox *trigger_edge = nullptr;
        QDoubleSpinBox *trigger_level = nullptr;
        QSpinBox *trigger_position = nullptr;
        int selected_channel = 0;
    };
    struct PwmFastUi
    {
        pv::data::decode::Decoder *decoder = nullptr;
        prop::binding::DecoderOptions *binding = nullptr;

        QComboBox *polarity_a = nullptr;
        QComboBox *polarity_b = nullptr;
        QComboBox *output = nullptr;
        QComboBox *analog_mode = nullptr;
        QComboBox *filter = nullptr;
        QComboBox *filter_cycles = nullptr;
        QDoubleSpinBox *filter_cutoff = nullptr;
        QComboBox *duty_decimals = nullptr;
        QComboBox *time_decimals = nullptr;
        QComboBox *freq_decimals = nullptr;
        QCheckBox *realtime_decode = nullptr;

        QToolButton *select_button[4] = {};
        QCheckBox *wave_enable[4] = {};
        QLabel *zoom_summary[4] = {};
        QLabel *pos_summary[4] = {};
        QRadioButton *trigger_channel[4] = {};
        QDoubleSpinBox *vzoom[4] = {};
        QDoubleSpinBox *vpos[4] = {};
        QComboBox *range_mode[4] = {};
        QDoubleSpinBox *eng_min[4] = {};
        QDoubleSpinBox *eng_max[4] = {};
        QLineEdit *unit[4] = {};

        QLabel *selected_label = nullptr;
        QComboBox *selected_combo = nullptr;
        QStackedWidget *advanced_stack = nullptr;
        QCheckBox *trigger_enable = nullptr;
        QComboBox *trigger_mode = nullptr;
        QComboBox *trigger_edge = nullptr;
        QDoubleSpinBox *trigger_level = nullptr;
        QSpinBox *trigger_position = nullptr;
        int selected_channel = 0;
    };

public:
    DecoderOptionsDlg(QWidget *parent);
    ~DecoderOptionsDlg(); 

    inline void set_cursor_range(uint64_t cursor1, uint64_t cursor2)
    {
        _cursor1 = cursor1;
        _cursor2 = cursor2;
    }

    inline void get_cursor_range(uint64_t &cursor1, uint64_t &cursor2)
    {
        cursor1 = _cursor1;
        cursor2 = _cursor2;
    }

    void load_options(view::DecodeTrace *trace);

    inline bool is_reload_form(){
        return _is_reload_form;
    }

    void apply_setting();

public slots:
    int exec() override;
    void accept() override;
    void reject() override;

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void load_options_view();

    void load_decoder_forms(QWidget *container);  

    DsComboBox* create_probe_selector(QWidget *parent, const data::decode::Decoder *dec,
            const srd_channel *const pdch);
 
    void create_decoder_form(pv::data::decode::Decoder *dec,
            QWidget *parent, QFormLayout *form);
    void create_tdm_audio_fast_options(pv::data::decode::Decoder *dec,
            QWidget *parent, QFormLayout *decoder_form,
            prop::binding::DecoderOptions *binding);
    void commit_tdm_audio_fast_options();
    void create_pwm_fast_options(pv::data::decode::Decoder *dec,
            QWidget *parent, QFormLayout *decoder_form,
            prop::binding::DecoderOptions *binding);
    void commit_pwm_fast_options();

    void commit_probes();    
    void commit_decoder_probes(data::decode::Decoder *dec);
    void update_decode_range(); 
 
private slots:
    void on_region_set(int index);
    void on_accept();
    void on_trans_pramas();

private: 
    std::vector<prop::binding::DecoderOptions*> _bindings;
    std::vector<TdmFastUi*> _tdm_fast_ui;
    std::vector<PwmFastUi*> _pwm_fast_ui;
    DsComboBox 		*_start_comboBox;
	DsComboBox 		*_end_comboBox;
    view::DecodeTrace   *_trace;
    uint64_t     _cursor1; //cursor key
    uint64_t     _cursor2;
    int          _contentHeight;
    
    std::vector<ProbeSelector> _probe_selectors;
    bool        _is_reload_form;
    int         _content_width;
};

}//dialogs
}//pv

#endif
