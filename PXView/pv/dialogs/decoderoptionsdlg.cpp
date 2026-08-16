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

#include "pv/dialogs/decoderoptionsdlg.h"
#include <libsigrokdecode.h>
#include <QScrollArea>
#include <QDialogButtonBox>
#include <cassert>
#include <QVBoxLayout>
#include <QLabel>
#include <QGridLayout>
#include <QFormLayout>
#include <QScrollArea>
#include <QVariant>
#include <QGuiApplication>
#include <QScreen>
#include <QCheckBox>
#include <QEvent>
#include <QMouseEvent>
#include <QEventLoop>
#include <QGroupBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QStackedWidget>
#include <QToolButton>
#include <QRadioButton>
#include <QButtonGroup>
#include <QPushButton>
#include <QComboBox>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <algorithm>
#include <cstring>
#include "pv/ui/popupdlglist.h"

#include "pv/data/stack/decoderstack.h"
#include "pv/data/decoderanalogdata.h"
#include "pv/prop/binding/decoderoptions.h"
#include "pv/data/decode/decoder.h"
#include "pv/ui/dscombobox.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/session/sigsession.h"
#include "pv/view/view.h"
#include "pv/view/cursor/cursor.h"
#include "pv/widgets/decodergroupbox.h"
#include "pv/view/trace/decodetrace.h"
#include "pv/ui/msgbox.h"

#include "pv/ui/langresource.h"
#include "pv/config/appconfig.h"
#include "pv/ui/dockfonts.h"

namespace pv {
namespace dialogs {


DecoderOptionsDlg::DecoderOptionsDlg(QWidget *parent)
:PxDialog(parent)
{
    _cursor1 = 0;
    _cursor2 = 0;
    _contentHeight = 0;
    _is_reload_form = false;
    _content_width = 0;
    // 用 Qt::Tool 替代 Qt::Dialog:Tool 窗口不走模态态路径,不 grabMouse,
    // qApp 事件过滤器能正常收到外部点击事件。配合 show()+QEventLoop 实现
    // "exec 语义但非模态",由事件过滤器检测外部点击调用 reject() 关闭。
    // FramelessWindowHint 保留 PxDialog 的无边框样式。
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    setWindowModality(Qt::NonModal);
}

DecoderOptionsDlg::~DecoderOptionsDlg()
{
    for (auto *ui : _tdm_fast_ui)
        delete ui;
    _tdm_fast_ui.clear();

    for (auto *ui : _pwm_fast_ui)
        delete ui;
    _pwm_fast_ui.clear();

    for(auto p : _bindings){
        delete p;
    }
    _bindings.clear();
}

int DecoderOptionsDlg::exec()
{
    // 不调用 PxDialog::exec()(其内部 QDialog::exec() 会 grabMouse 进入模态态,
    // 导致 qApp 事件过滤器收不到外部点击)。改用 show() + 本地 QEventLoop:
    // Tool 窗口非模态,不 grabMouse,事件过滤器能正常检测外部点击,
    // 由 finished 信号驱动 QEventLoop 退出,保持 exec() 同步返回语义。
    update_font();
    PopupDlgList::AddDlgTolist(this);
    qApp->installEventFilter(this);
    show();
    raise();
    activateWindow();
    QEventLoop loop;
    connect(this, &QDialog::finished, &loop, &QEventLoop::quit);
    loop.exec();
    qApp->removeEventFilter(this);
    return result();
}

bool DecoderOptionsDlg::eventFilter(QObject *obj, QEvent *event)
{
    // 全局事件过滤器(安装在 qApp 上):检测鼠标按下事件落在对话框几何范围外时
    // 调用 reject() 关闭(与毛刺滤波浮窗 Qt::Popup 的行为一致)。
    // Tool 窗口非模态不 grabMouse,外部 widget 的鼠标事件能正常到达 qApp 过滤器。
    if (event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        QPoint globalPos = me->globalPosition().toPoint();
        if (!rect().contains(mapFromGlobal(globalPos))) {
            // 点击在对话框 rect 外。但需排除对话框子控件(如 DsComboBox 的
            // DsComboPopup 下拉列表——它是 Qt::Popup 类型的独立顶层窗口,
            // 可能延伸到 rect 之外,且其 parent 链可能因 Qt 内部 viewport
            // reparent 机制而无法遍历到 this)。
            // 修复:先用 qApp->activePopupWidget() 检查是否有活跃 popup,
            // 如果鼠标点击落在活跃 popup 的几何范围内,则不关闭对话框。
            QWidget *popup = qApp->activePopupWidget();
            if (popup && popup->isVisible()) {
                QRect popupRect = popup->geometry();
                if (popupRect.contains(globalPos)) {
                    return PxDialog::eventFilter(obj, event);
                }
            }

            // 向上查找父链,若属于 this 则不关闭。
            QWidget *w = qobject_cast<QWidget *>(obj);
            bool is_child = false;
            while (w) {
                if (w == this) { is_child = true; break; }
                w = w->parentWidget();
            }
            if (!is_child) {
                reject();
                return true;
            }
        }
    }
    return PxDialog::eventFilter(obj, event);
}

void DecoderOptionsDlg::accept()
{
    PxDialog::accept();
}

void DecoderOptionsDlg::reject()
{
    PxDialog::reject();
}

void DecoderOptionsDlg::load_options(view::DecodeTrace *trace)
{
    if (!trace) {
        return;
    }
    assert(trace);
    _trace = trace;

    const char *dec_id = trace->decoder()->get_root_decoder_id();

    if (LangResource::Instance()->is_new_decoder(dec_id))
        LangResource::Instance()->reload_dynamic();

    load_options_view();

    LangResource::Instance()->release_dynamic();
}

void DecoderOptionsDlg::load_options_view()
{   
    PxDialog *dlg = this;   

    dlg->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DECODER_OPTIONS), "Decoder Options"));   

    QFormLayout *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setVerticalSpacing(5);
    form->setFormAlignment(Qt::AlignLeft);
    form->setLabelAlignment(Qt::AlignLeft);
    dlg->layout()->addLayout(form);
    
    //scroll pannel
    QWidget *scroll_pannel  = new QWidget();
    QVBoxLayout *scroll_lay = new QVBoxLayout();
    scroll_lay->setContentsMargins(0, 0, 0, 0);
    scroll_lay->setAlignment(Qt::AlignLeft);
    scroll_pannel->setLayout(scroll_lay);
    form->addRow(scroll_pannel);

    // decoder options 
    QWidget *container_panel = new QWidget();      
    QVBoxLayout *decoder_lay = new QVBoxLayout();
    decoder_lay->setContentsMargins(0, 0, 0, 0);
    decoder_lay->setDirection(QBoxLayout::TopToBottom);
    container_panel->setLayout(decoder_lay);
    scroll_lay->addWidget(container_panel);
  
    load_decoder_forms(container_panel);
 
    //Add region combobox
    _start_comboBox = new DsComboBox(dlg);
    _end_comboBox = new DsComboBox(dlg);
    _start_comboBox->addItem("Start");
    _end_comboBox->addItem("End"); 
    _start_comboBox->setMinimumContentsLength(7);
    _end_comboBox->setMinimumContentsLength(7);
    _start_comboBox->setMinimumWidth(30);
    _end_comboBox->setMinimumWidth(30);
    
    // Add cursor list
    auto view = _trace->get_view();
    int dex1 = 0;
    int dex2 = 0;

    if (view)
    {  
        int num = 1;
        auto &cursor_list = view->get_cursorList();
        
        for (auto &c : cursor_list){
            //tr
            QString cursor_name = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CURSOR), "Cursor") + 
                                QString::number(num);
            _start_comboBox->addItem(cursor_name, QVariant((quint64)c->get_key()));
            _end_comboBox->addItem(cursor_name, QVariant((quint64)c->get_key()));

            if (c->get_key() == _cursor1)
                dex1 = num;
            if (c->get_key() == _cursor2) 
                dex2 = num; 

            num++;
        }
    }

    if (dex1 == 0)
        _cursor1 = 0;
    if (dex2 == 0)
        _cursor2 = 0;

    _start_comboBox->setCurrentIndex(dex1);
    _end_comboBox->setCurrentIndex(dex2);
 
    update_decode_range(); // set default sample range
 
    int h_ex2 = 0;
    bool bLang = AppConfig::Instance().appOptions.transDecoderDlg;

    if (LangResource::Instance()->is_lang_en() == false){
        QWidget *sp1 = new QWidget();
        sp1->setFixedHeight(5);
        form->addRow(sp1);
        QString trans_lable(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DECODER_IF_TRANS), "Translate param names"));
        QCheckBox *ck_trans = new QCheckBox();
        ck_trans->setFixedSize(20,20);
        ck_trans->setChecked(bLang);
        connect(ck_trans, &QCheckBox::released, this, &DecoderOptionsDlg::on_trans_pramas);
        ck_trans->setStyleSheet("margin-top:5px");
        QLabel *trans_lb = new QLabel(trans_lable);
        
        QHBoxLayout *trans_lay = new QHBoxLayout();
        QWidget *trans_wid = new QWidget();
        trans_wid->setLayout(trans_lay);
        trans_lay->setSpacing(0);
        trans_lay->setContentsMargins(10,0,0,0);
        trans_lay->addWidget(ck_trans);
        trans_lay->addWidget(trans_lb);
        form->addRow("", trans_wid);
        h_ex2 = 40;   
    }  

  //tr
    QLabel *lb1 = new QLabel(
                     L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CURSOR_FOR_DECODE_START), "The cursor for decode start time"));
    QLabel *lb2 = new QLabel(
                     L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CURSOR_FOR_DECODE_END), "The cursor for decode end time"));

    form->addRow(_start_comboBox, lb1);
    form->addRow(_end_comboBox, lb2);
 
    // Add ButtonBox (OK/Cancel)
    QDialogButtonBox *button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                Qt::Horizontal, dlg);

    QHBoxLayout *confirm_button_box = new QHBoxLayout;
    confirm_button_box->addWidget(button_box, 0, Qt::AlignRight);
    form->addRow(confirm_button_box);

    this->update_font();

    int real_content_width = _content_width;
    int content_height = _contentHeight;

     // scroll     
    QSize tsize = dlg->sizeHint();
    int w = tsize.width(); 
    int other_height = 190 + h_ex2; 
    content_height += 20;

    int cursor_line_width = lb1->sizeHint().width() + _start_comboBox->sizeHint().width();

    if (w < real_content_width){
        w = real_content_width;
    }
    if (w < cursor_line_width){
        w = cursor_line_width;
    }

#ifdef Q_OS_DARWIN
    other_height += 40;
#endif

    int dlgHeight = content_height + other_height; 
     
    float sk = QGuiApplication::primaryScreen()->logicalDotsPerInch() / 96;
    int srcHeight = 600;
    container_panel->setFixedHeight(content_height);

    if (dlgHeight * sk > srcHeight)
    { 
        QScrollArea *scroll = new QScrollArea(scroll_pannel);
        scroll->setWidget(container_panel);
        scroll->setStyleSheet("QScrollArea{border:none;}");
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        dlg->setFixedSize(w + 20, srcHeight);
        scroll_pannel->setFixedSize(w, srcHeight - other_height);
        int sclw = w - 18;
#ifdef Q_OS_DARWIN
        sclw -= 20;
#endif
        scroll->setFixedSize(sclw, srcHeight - other_height);
    }
    else{
        dlg->setFixedSize(w + 20,dlgHeight);
    }
 
    connect(_start_comboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &DecoderOptionsDlg::on_region_set);
    connect(_end_comboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &DecoderOptionsDlg::on_region_set);
    connect(button_box, &QDialogButtonBox::accepted, this, &DecoderOptionsDlg::on_accept);
    connect(button_box, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
}

void DecoderOptionsDlg::load_decoder_forms(QWidget *container)
{
	using pv::data::decode::Decoder;
	if (!container) return;
	assert(container);

    int dex = 0;
 
    for(auto &up : _trace->decoder()->stack()) 
    { 
        auto dec = up.get();
        ++dex;
        QWidget *panel = new QWidget(container);
        QFormLayout *form = new QFormLayout();
        form->setContentsMargins(0,0,0,0);
        panel->setLayout(form);
        container->layout()->addWidget(panel);
       
        create_decoder_form(dec, panel, form); 

        _contentHeight += panel->sizeHint().height();
	} 
}
 

DsComboBox* DecoderOptionsDlg::create_probe_selector(
    QWidget *parent, const data::decode::Decoder *dec,
	const srd_channel *const pdch)
{
	if (!dec || !_trace) return nullptr;
	assert(dec);
    assert(_trace);

    auto *view = _trace->get_view();
    if (!view)
        return nullptr;
    const auto &sigs = view->session().get_signal_models();

    data::decode::Decoder *decoder = const_cast<data::decode::Decoder*>(dec);

	DsComboBox *selector = new DsComboBox(parent);
    selector->addItem("-", QVariant::fromValue(-1));

    int dex = 1; // index 0 is the "-" placeholder item
    const int binded_index = decoder->binded_probe_index(pdch);

	for(auto s : sigs)
    {
        if (s->type() == SR_CHANNEL_LOGIC && s->enabled()){
			selector->addItem(QString::fromStdString(s->name()), QVariant::fromValue(s->index()));

            if (binded_index == s->index()){
                selector->setCurrentIndex(dex);
            }
            dex++;
		}
	}

    if (binded_index == -1){
        selector->setCurrentIndex(0);
    }

	return selector;
}

void DecoderOptionsDlg::on_region_set(int index)
{
    (void)index;
    update_decode_range();
}

void DecoderOptionsDlg::update_decode_range()
{
    if (!_trace) return;
    assert(_trace);
    auto *view = _trace->get_view();
    if (!view) return;
    const uint64_t last_samples = view->session().cur_samplelimits() - 1;
    const int index1 = _start_comboBox->currentIndex();
    const int index2 = _end_comboBox->currentIndex();
    uint64_t decode_start, decode_end;

    if (index1 == 0) {
        decode_start = 0;
        _cursor1 = 0;

    } else {
        _cursor1 = _start_comboBox->itemData(index1).toULongLong();
        int cusrsor_index = view->get_cursor_index_by_key(_cursor1);
        if (cusrsor_index != -1){
            decode_start = view->get_cursor_samples(cusrsor_index);
        }
        else{
            decode_start = 0;
            _cursor1 = 0;
        }        
    }

    if (index2 == 0) {
        decode_end = 0; // 0 is a special value meaning "to the end"
        _cursor2 = 0;

    } else {
        _cursor2 = _end_comboBox->itemData(index2).toULongLong();
        int cusrsor_index = view->get_cursor_index_by_key(_cursor2);
        if (cusrsor_index != -1){
            decode_end = view->get_cursor_samples(cusrsor_index);
        }
        else{
            decode_end = 0;
            _cursor2 = 0;
        }       
    }

    if (decode_start > last_samples)
        decode_start = 0;
    if (decode_end != 0 && decode_end > last_samples)
        decode_end = last_samples;

    if (decode_end != 0 && decode_start > decode_end) {
        uint64_t tmp = decode_start;
        decode_start = decode_end;
        decode_end = tmp;
    }
  
    for(auto &up : _trace->decoder()->stack()) {
        auto dec = up.get();
        dec->set_decode_region(decode_start, decode_end);
    }
}
 

void DecoderOptionsDlg::create_decoder_form(
    data::decode::Decoder *dec, QWidget *parent,
    QFormLayout *form)
{
	const GSList *l;

    if (!dec) return;
    assert(dec);
	const srd_decoder *const decoder = dec->decoder();
	if (!decoder) return;
	assert(decoder);

    QFont font = theme_font_dialog();

    QFormLayout *const decoder_form = new QFormLayout();
    decoder_form->setContentsMargins(0,0,0,0);
    decoder_form->setVerticalSpacing(4);
    decoder_form->setFormAlignment(Qt::AlignLeft);
    decoder_form->setLabelAlignment(Qt::AlignLeft);
    decoder_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    bool bLang = AppConfig::Instance().appOptions.transDecoderDlg;
    if (LangResource::Instance()->is_lang_en()){
        bLang = false;
    }
 
	// Add the mandatory channels
	for(l = decoder->channels; l; l = l->next) {
		const struct srd_channel *const pdch = (struct srd_channel *)l->data;
		DsComboBox *const combo = create_probe_selector(parent, dec, pdch);

        const char *desc_str = nullptr;
        const char *lang_str = nullptr;

        if (pdch->idn != nullptr && LangResource::Instance()->is_lang_en() == false){
            lang_str = LangResource::Instance()->get_lang_text(STR_PAGE_DECODER, pdch->idn, pdch->desc);
        }

        if (lang_str != nullptr && bLang){
            desc_str = lang_str;
        }
        else{
            desc_str = pdch->desc;
        }

        //tr
        decoder_form->addRow(QString("<b>%1</b> (%2) *")
			.arg(QString::fromUtf8(pdch->name))
			.arg(QString::fromUtf8(desc_str)), combo);

        const ProbeSelector s = {combo, dec, pdch};
    	_probe_selectors.push_back(s);
	} 

	// Add the optional channels
	for(l = decoder->opt_channels; l; l = l->next) {
		const struct srd_channel *const pdch = (struct srd_channel *)l->data;
		DsComboBox *const combo = create_probe_selector(parent, dec, pdch);
		
        const char *desc_str = nullptr;
        const char *lang_str = nullptr;
        
        if (pdch->idn != nullptr && LangResource::Instance()->is_lang_en() == false){
            lang_str = LangResource::Instance()->get_lang_text(STR_PAGE_DECODER, pdch->idn, pdch->desc);
        }

        if (lang_str != nullptr && bLang){
            desc_str = lang_str;
        }
        else{
            desc_str = pdch->desc;
        }

        //tr
        decoder_form->addRow(QString("<b>%1</b> (%2)")
			.arg(QString::fromUtf8(pdch->name))
			.arg(QString::fromUtf8(desc_str)), combo);

        const ProbeSelector s = {combo, dec, pdch};
        _probe_selectors.push_back(s);
	}

	// Add the options. TDM Audio Fast gets a dedicated compact editor instead
    // of flattening 70+ decoder options into one long QFormLayout. Other
    // decoders retain the original generic property binding unchanged.
    auto binding = new prop::binding::DecoderOptions(_trace->decoder(), dec);
	_bindings.push_back(binding);

    const bool is_tdm_fast = decoder->id &&
        std::strcmp(decoder->id, "tdm_audio_fast") == 0;
    const bool is_pwm_fast = decoder->id &&
        std::strncmp(decoder->id, "pwm_waveform", std::strlen("pwm_waveform")) == 0;

    if (is_tdm_fast) {
        create_tdm_audio_fast_options(dec, parent, decoder_form, binding);

        auto *group = new QGroupBox(QString::fromUtf8(decoder->name), parent);
        group->setFont(font);
        group->setLayout(decoder_form);
        form->addRow(group);
        _content_width = std::max(_content_width, group->sizeHint().width());
        return;
    }

    if (is_pwm_fast) {
        create_pwm_fast_options(dec, parent, decoder_form, binding);
        auto *group = new QGroupBox(QString::fromUtf8(decoder->name), parent);
        group->setFont(font);
        group->setLayout(decoder_form);
        form->addRow(group);
        _content_width = std::max(_content_width, group->sizeHint().width());
        return;
    }

    binding->add_properties_to_form(decoder_form, false, font);
  
    auto group = new pv::widgets::DecoderGroupBox(_trace->decoder().get(), 
                            dec, 
                            decoder_form, 
                            parent, font);

    if (group->_content_width > _content_width){
        _content_width = group->_content_width;
    }

	form->addRow(group);
}


void DecoderOptionsDlg::create_tdm_audio_fast_options(
    data::decode::Decoder *dec, QWidget *parent,
    QFormLayout *decoder_form, prop::binding::DecoderOptions *binding)
{
    if (!dec || !parent || !decoder_form || !binding || !_trace)
        return;

    auto *ui = new TdmFastUi();
    ui->decoder = dec;
    ui->binding = binding;
    _tdm_fast_ui.push_back(ui);

    auto opt_int = [binding](const char *id, qint64 fallback) -> qint64 {
        GVariant *v = binding->getter(id);
        if (!v)
            return fallback;
        qint64 value = fallback;
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64))
            value = g_variant_get_int64(v);
        g_variant_unref(v);
        return value;
    };
    auto opt_double = [binding](const char *id, double fallback) -> double {
        GVariant *v = binding->getter(id);
        if (!v)
            return fallback;
        double value = fallback;
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE))
            value = g_variant_get_double(v);
        g_variant_unref(v);
        return value;
    };
    auto opt_bool = [binding](const char *id, bool fallback) -> bool {
        GVariant *v = binding->getter(id);
        if (!v)
            return fallback;
        bool value = fallback;
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
            value = g_variant_get_boolean(v);
        g_variant_unref(v);
        return value;
    };
    auto opt_string = [binding](const char *id, const char *fallback) -> QString {
        GVariant *v = binding->getter(id);
        if (!v)
            return QString::fromUtf8(fallback);
        QString value = QString::fromUtf8(fallback);
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
            value = QString::fromUtf8(g_variant_get_string(v, nullptr));
        g_variant_unref(v);
        return value;
    };
    auto set_combo_data = [](QComboBox *combo, const QString &value) {
        if (!combo)
            return;
        for (int i = 0; i < combo->count(); ++i) {
            if (combo->itemData(i).toString() == value) {
                combo->setCurrentIndex(i);
                return;
            }
        }
    };

    // ---- Common TDM bus settings: always visible. ----
    auto *bus_group = new QGroupBox(QStringLiteral("TDM 总线"), parent);
    auto *bus = new QGridLayout(bus_group);
    bus->setContentsMargins(8, 20, 8, 6);
    bus->setHorizontalSpacing(6);
    bus->setVerticalSpacing(5);

    ui->bits = new QSpinBox(bus_group);
    ui->bits->setRange(1, 32);
    ui->bits->setValue(static_cast<int>(opt_int("bps", 32)));
    ui->bits->setSuffix(QStringLiteral(" bit"));
    ui->bits->setMaximumWidth(110);

    ui->channels = new DsComboBox(bus_group);
    for (int i = 1; i <= 8; ++i)
        ui->channels->addItem(QString::number(i), i);
    ui->channels->setCurrentIndex(std::max(0, std::min(7,
        static_cast<int>(opt_int("channels", 8)) - 1)));
    ui->channels->setMaximumWidth(110);

    ui->align = new DsComboBox(bus_group);
    ui->align->addItem(QStringLiteral("I2S"), QStringLiteral("I2S"));
    ui->align->addItem(QStringLiteral("Left Justified"), QStringLiteral("left-justified"));
    set_combo_data(ui->align, opt_string("align", "I2S"));
    ui->align->setMaximumWidth(130);

    ui->data_format = new DsComboBox(bus_group);
    ui->data_format->addItem(QStringLiteral("Signed"), QStringLiteral("signed"));
    ui->data_format->addItem(QStringLiteral("Unsigned"), QStringLiteral("unsigned"));
    set_combo_data(ui->data_format, opt_string("data_format", "signed"));
    ui->data_format->setMaximumWidth(130);

    ui->edge = new DsComboBox(bus_group);
    ui->edge->addItem(QStringLiteral("Rising / 上升沿"), QStringLiteral("rising"));
    ui->edge->addItem(QStringLiteral("Falling / 下降沿"), QStringLiteral("falling"));
    set_combo_data(ui->edge, opt_string("edge", "rising"));
    ui->edge->setMaximumWidth(130);

    ui->frame_edge = new DsComboBox(bus_group);
    ui->frame_edge->addItem(QStringLiteral("High / 高有效"), QStringLiteral("high"));
    ui->frame_edge->addItem(QStringLiteral("Low / 低有效"), QStringLiteral("low"));
    set_combo_data(ui->frame_edge, opt_string("frame_edge", "high"));
    ui->frame_edge->setMaximumWidth(130);

    ui->realtime_decode = new QCheckBox(QStringLiteral("实时解码"), bus_group);
    ui->realtime_decode->setChecked(opt_bool("realtime_decode", false));

    bus->addWidget(new QLabel(QStringLiteral("位宽"), bus_group), 0, 0);
    bus->addWidget(ui->bits, 0, 1);
    bus->addWidget(new QLabel(QStringLiteral("通道数"), bus_group), 0, 2);
    bus->addWidget(ui->channels, 0, 3);
    bus->addWidget(new QLabel(QStringLiteral("对齐"), bus_group), 0, 4);
    bus->addWidget(ui->align, 0, 5);
    bus->addWidget(new QLabel(QStringLiteral("格式"), bus_group), 1, 0);
    bus->addWidget(ui->data_format, 1, 1);
    bus->addWidget(new QLabel(QStringLiteral("BCK沿"), bus_group), 1, 2);
    bus->addWidget(ui->edge, 1, 3);
    bus->addWidget(new QLabel(QStringLiteral("FS极性"), bus_group), 1, 4);
    bus->addWidget(ui->frame_edge, 1, 5);
    bus->addWidget(ui->realtime_decode, 2, 0, 1, 6);
    decoder_form->addRow(bus_group);

    // Snapshot the current annotation-row visibility. DecoderStack keeps these
    // separately from chN_enable (analog waveform visibility).
    bool text_visible[8] = {true, true, true, true, true, true, true, true};
    int text_index = 0;
    for (const auto &entry : _trace->decoder()->get_rows_gshow()) {
        if (entry.first.decoder() == dec->decoder() && text_index < 8)
            text_visible[text_index++] = entry.second;
    }

    const int saved_trigger_channel = std::max(0, std::min(7,
        static_cast<int>(opt_int("display_trigger_channel", 0))));

    // ---- Compact CH0..CH7 matrix. ----
    auto *matrix_group = new QGroupBox(QStringLiteral("CH0～CH7"), parent);
    auto *matrix = new QGridLayout(matrix_group);
    matrix->setContentsMargins(8, 20, 8, 6);
    matrix->setHorizontalSpacing(8);
    matrix->setVerticalSpacing(3);

    const QString headers[] = {
        QStringLiteral("CH"), QStringLiteral("波"), QStringLiteral("字"),
        QStringLiteral("Zoom"), QStringLiteral("Pos"), QStringLiteral("Trig")};
    for (int col = 0; col < 6; ++col) {
        auto *label = new QLabel(headers[col], matrix_group);
        label->setAlignment(Qt::AlignCenter);
        matrix->addWidget(label, 0, col);
    }

    auto *select_group = new QButtonGroup(matrix_group);
    select_group->setExclusive(true);
    auto *trigger_group = new QButtonGroup(matrix_group);
    trigger_group->setExclusive(true);

    for (int ch = 0; ch < 8; ++ch) {
        char key[64];

        ui->select_button[ch] = new QToolButton(matrix_group);
        ui->select_button[ch]->setText(QStringLiteral("CH%1").arg(ch));
        ui->select_button[ch]->setCheckable(true);
        ui->select_button[ch]->setMinimumWidth(44);
        ui->select_button[ch]->setMaximumWidth(52);
        ui->select_button[ch]->setToolTip(QStringLiteral("编辑 CH%1 高级参数").arg(ch));
        select_group->addButton(ui->select_button[ch], ch);

        std::snprintf(key, sizeof(key), "ch%d_enable", ch);
        ui->wave_enable[ch] = new QCheckBox(matrix_group);
        ui->wave_enable[ch]->setChecked(opt_int(key, 1) != 0);
        ui->wave_enable[ch]->setToolTip(QStringLiteral("显示 CH%1 模拟波形").arg(ch));

        ui->text_enable[ch] = new QCheckBox(matrix_group);
        ui->text_enable[ch]->setChecked(text_visible[ch]);
        ui->text_enable[ch]->setToolTip(QStringLiteral("显示 CH%1 解码文字").arg(ch));

        std::snprintf(key, sizeof(key), "ch%d_vzoom", ch);
        const double saved_zoom = opt_double(key, 1.0);
        ui->zoom_summary[ch] = new QLabel(QString::number(saved_zoom, 'f', 2), matrix_group);
        ui->zoom_summary[ch]->setAlignment(Qt::AlignCenter);

        std::snprintf(key, sizeof(key), "ch%d_vpos", ch);
        const double saved_pos = opt_double(key, 1.0);
        ui->pos_summary[ch] = new QLabel(QString::number(saved_pos, 'f', 2), matrix_group);
        ui->pos_summary[ch]->setAlignment(Qt::AlignCenter);

        ui->trigger_channel[ch] = new QRadioButton(matrix_group);
        ui->trigger_channel[ch]->setChecked(ch == saved_trigger_channel);
        ui->trigger_channel[ch]->setToolTip(QStringLiteral("选择 CH%1 为显示触发通道").arg(ch));
        trigger_group->addButton(ui->trigger_channel[ch], ch);

        matrix->addWidget(ui->select_button[ch], ch + 1, 0, Qt::AlignCenter);
        matrix->addWidget(ui->wave_enable[ch], ch + 1, 1, Qt::AlignCenter);
        matrix->addWidget(ui->text_enable[ch], ch + 1, 2, Qt::AlignCenter);
        matrix->addWidget(ui->zoom_summary[ch], ch + 1, 3, Qt::AlignCenter);
        matrix->addWidget(ui->pos_summary[ch], ch + 1, 4, Qt::AlignCenter);
        matrix->addWidget(ui->trigger_channel[ch], ch + 1, 5, Qt::AlignCenter);
    }
    decoder_form->addRow(matrix_group);

    // ---- Selected-channel advanced editor. ----
    auto *advanced_group = new QGroupBox(QStringLiteral("当前通道"), parent);
    auto *advanced_layout = new QVBoxLayout(advanced_group);
    advanced_layout->setContentsMargins(8, 20, 8, 6);
    advanced_layout->setSpacing(5);
    auto *advanced_head = new QWidget(advanced_group);
    auto *advanced_head_layout = new QHBoxLayout(advanced_head);
    advanced_head_layout->setContentsMargins(0, 0, 0, 0);
    advanced_head_layout->setSpacing(6);
    ui->selected_label = new QLabel(QStringLiteral("当前: CH0"), advanced_group);
    ui->selected_combo = new DsComboBox(advanced_group);
    for (int ch = 0; ch < 8; ++ch)
        ui->selected_combo->addItem(QStringLiteral("CH%1").arg(ch), ch);
    ui->selected_combo->setMaximumWidth(90);
    advanced_head_layout->addWidget(ui->selected_label);
    advanced_head_layout->addStretch(1);
    advanced_head_layout->addWidget(new QLabel(QStringLiteral("切换"), advanced_group));
    advanced_head_layout->addWidget(ui->selected_combo);
    ui->advanced_stack = new QStackedWidget(advanced_group);
    advanced_layout->addWidget(advanced_head);
    advanced_layout->addWidget(ui->advanced_stack);

    for (int ch = 0; ch < 8; ++ch) {
        char key[64];
        auto *page = new QWidget(ui->advanced_stack);
        auto *grid = new QGridLayout(page);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(5);

        ui->vzoom[ch] = new QDoubleSpinBox(page);
        ui->vzoom[ch]->setDecimals(3);
        ui->vzoom[ch]->setRange(0.01, 100.0);
        ui->vzoom[ch]->setSingleStep(0.05);
        ui->vzoom[ch]->setSuffix(QStringLiteral(" x"));
        ui->vzoom[ch]->setMaximumWidth(120);
        std::snprintf(key, sizeof(key), "ch%d_vzoom", ch);
        ui->vzoom[ch]->setValue(opt_double(key, 1.0));

        ui->vpos[ch] = new QDoubleSpinBox(page);
        ui->vpos[ch]->setDecimals(3);
        ui->vpos[ch]->setRange(-800.0, 800.0);
        ui->vpos[ch]->setSingleStep(0.05);
        ui->vpos[ch]->setMaximumWidth(120);
        std::snprintf(key, sizeof(key), "ch%d_vpos", ch);
        ui->vpos[ch]->setValue(opt_double(key, 1.0));

        ui->range_mode[ch] = new DsComboBox(page);
        ui->range_mode[ch]->addItem(QStringLiteral("Bipolar / 双极性"), QStringLiteral("bipolar"));
        ui->range_mode[ch]->addItem(QStringLiteral("Unipolar / 单极性"), QStringLiteral("unipolar"));
        ui->range_mode[ch]->addItem(QStringLiteral("Custom / 自定义"), QStringLiteral("custom"));
        std::snprintf(key, sizeof(key), "ch%d_range_mode", ch);
        set_combo_data(ui->range_mode[ch], opt_string(key, "bipolar"));
        ui->range_mode[ch]->setMaximumWidth(150);

        ui->eng_min[ch] = new QDoubleSpinBox(page);
        ui->eng_min[ch]->setDecimals(6);
        ui->eng_min[ch]->setRange(-1.0e12, 1.0e12);
        ui->eng_min[ch]->setMaximumWidth(120);
        std::snprintf(key, sizeof(key), "ch%d_eng_min", ch);
        ui->eng_min[ch]->setValue(opt_double(key, -1.0));

        ui->eng_max[ch] = new QDoubleSpinBox(page);
        ui->eng_max[ch]->setDecimals(6);
        ui->eng_max[ch]->setRange(-1.0e12, 1.0e12);
        ui->eng_max[ch]->setMaximumWidth(120);
        std::snprintf(key, sizeof(key), "ch%d_eng_max", ch);
        ui->eng_max[ch]->setValue(opt_double(key, 1.0));

        ui->unit[ch] = new QLineEdit(page);
        ui->unit[ch]->setMaxLength(16);
        ui->unit[ch]->setMaximumWidth(100);
        std::snprintf(key, sizeof(key), "ch%d_unit", ch);
        ui->unit[ch]->setText(opt_string(key, "V"));

        grid->addWidget(new QLabel(QStringLiteral("V-Zoom"), page), 0, 0);
        grid->addWidget(ui->vzoom[ch], 0, 1);
        grid->addWidget(new QLabel(QStringLiteral("V-Pos"), page), 0, 2);
        grid->addWidget(ui->vpos[ch], 0, 3);
        grid->addWidget(new QLabel(QStringLiteral("量程"), page), 1, 0);
        grid->addWidget(ui->range_mode[ch], 1, 1);
        grid->addWidget(new QLabel(QStringLiteral("Min"), page), 1, 2);
        grid->addWidget(ui->eng_min[ch], 1, 3);
        grid->addWidget(new QLabel(QStringLiteral("Max"), page), 2, 0);
        grid->addWidget(ui->eng_max[ch], 2, 1);
        grid->addWidget(new QLabel(QStringLiteral("单位"), page), 2, 2);
        grid->addWidget(ui->unit[ch], 2, 3);

        ui->advanced_stack->addWidget(page);

        connect(ui->vzoom[ch], QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [ui, ch](double value) {
            if (ui->zoom_summary[ch])
                ui->zoom_summary[ch]->setText(QString::number(value, 'f', 2));
        });
        connect(ui->vpos[ch], QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [ui, ch](double value) {
            if (ui->pos_summary[ch])
                ui->pos_summary[ch]->setText(QString::number(value, 'f', 2));
        });
    }

    auto select_channel = [ui](int ch) {
        if (ch < 0 || ch >= 8)
            return;
        ui->selected_channel = ch;
        ui->selected_label->setText(QStringLiteral("当前: CH%1").arg(ch));
        ui->advanced_stack->setCurrentIndex(ch);
        if (ui->select_button[ch])
            ui->select_button[ch]->setChecked(true);
        if (ui->selected_combo) {
            const QSignalBlocker blocker(ui->selected_combo);
            ui->selected_combo->setCurrentIndex(ch);
        }
    };
    for (int ch = 0; ch < 8; ++ch) {
        connect(ui->select_button[ch], &QToolButton::clicked,
                this, [select_channel, ch](bool) { select_channel(ch); });
        connect(ui->trigger_channel[ch], &QRadioButton::clicked,
                this, [select_channel, ch](bool) { select_channel(ch); });
    }
    connect(ui->selected_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [select_channel](int index) { select_channel(index); });
    select_channel(saved_trigger_channel);

    // ---- Display / trigger common controls. ----
    auto *display_group = new QGroupBox(QStringLiteral("显示 / 触发"), parent);
    auto *display = new QGridLayout(display_group);
    display->setContentsMargins(8, 20, 8, 6);
    display->setHorizontalSpacing(8);
    display->setVerticalSpacing(5);

    ui->hide_decode_text = new QCheckBox(QStringLiteral("隐藏解码文字"), display_group);
    const QString saved_output = opt_string("output", "waveform");
    ui->hide_decode_text->setChecked(!dec->shown() || saved_output == QStringLiteral("waveform"));

    ui->trigger_enable = new QCheckBox(QStringLiteral("启用模拟显示触发"), display_group);
    ui->trigger_enable->setChecked(opt_bool("display_trigger_enable", false));

    ui->trigger_mode = new DsComboBox(display_group);
    ui->trigger_mode->addItem(QStringLiteral("Auto"), QStringLiteral("auto"));
    ui->trigger_mode->addItem(QStringLiteral("Normal"), QStringLiteral("normal"));
    set_combo_data(ui->trigger_mode, opt_string("display_trigger_mode", "auto"));
    ui->trigger_mode->setMaximumWidth(110);

    ui->trigger_edge = new DsComboBox(display_group);
    ui->trigger_edge->addItem(QStringLiteral("Rising"), QStringLiteral("rising"));
    ui->trigger_edge->addItem(QStringLiteral("Falling"), QStringLiteral("falling"));
    ui->trigger_edge->addItem(QStringLiteral("Either"), QStringLiteral("either"));
    set_combo_data(ui->trigger_edge, opt_string("display_trigger_edge", "rising"));
    ui->trigger_edge->setMaximumWidth(110);

    ui->trigger_level = new QDoubleSpinBox(display_group);
    ui->trigger_level->setDecimals(6);
    ui->trigger_level->setRange(-1.0e12, 1.0e12);
    ui->trigger_level->setValue(opt_double("display_trigger_level", 0.0));
    ui->trigger_level->setMaximumWidth(120);

    ui->trigger_position = new QSpinBox(display_group);
    ui->trigger_position->setRange(0, 100);
    ui->trigger_position->setSuffix(QStringLiteral(" %"));
    ui->trigger_position->setValue(static_cast<int>(opt_int("display_trigger_position", 50)));
    ui->trigger_position->setMaximumWidth(90);

    display->addWidget(ui->hide_decode_text, 0, 0, 1, 4);
    display->addWidget(ui->trigger_enable, 1, 0, 1, 4);
    display->addWidget(new QLabel(QStringLiteral("Mode"), display_group), 2, 0);
    display->addWidget(ui->trigger_mode, 2, 1);
    display->addWidget(new QLabel(QStringLiteral("Edge"), display_group), 2, 2);
    display->addWidget(ui->trigger_edge, 2, 3);
    display->addWidget(new QLabel(QStringLiteral("Level"), display_group), 3, 0);
    display->addWidget(ui->trigger_level, 3, 1);
    display->addWidget(new QLabel(QStringLiteral("Position"), display_group), 3, 2);
    display->addWidget(ui->trigger_position, 3, 3);

    auto *all_wave = new QPushButton(QStringLiteral("全部通道"), display_group);
    auto *selected_only = new QPushButton(QStringLiteral("仅当前"), display_group);
    auto *auto_range = new QPushButton(QStringLiteral("自动量程"), display_group);
    auto *full_range = new QPushButton(QStringLiteral("全量程"), display_group);
    auto *copy_all = new QPushButton(QStringLiteral("复制→全部"), display_group);
    auto *reset = new QPushButton(QStringLiteral("恢复默认"), display_group);

    all_wave->setToolTip(QStringLiteral("显示全部通道的模拟波形"));
    selected_only->setToolTip(QStringLiteral("只显示当前选中通道的模拟波形"));
    auto_range->setToolTip(QStringLiteral("按当前可见区域自动量程"));
    full_range->setToolTip(QStringLiteral("按当前通道全数据范围自动量程"));
    copy_all->setToolTip(QStringLiteral("将当前通道的高级参数复制到其它通道"));

    display->addWidget(all_wave, 4, 0);
    display->addWidget(selected_only, 4, 1);
    display->addWidget(auto_range, 4, 2);
    display->addWidget(full_range, 4, 3);
    display->addWidget(copy_all, 5, 0, 1, 2);
    display->addWidget(reset, 5, 2, 1, 2);

    auto refresh_text_enable = [ui]() {
        const bool enabled = !ui->hide_decode_text->isChecked();
        for (int ch = 0; ch < 8; ++ch)
            if (ui->text_enable[ch])
                ui->text_enable[ch]->setEnabled(enabled);
    };
    connect(ui->hide_decode_text, &QCheckBox::toggled,
            this, [refresh_text_enable](bool) { refresh_text_enable(); });
    refresh_text_enable();

    auto refresh_trigger_enable = [ui]() {
        const bool enabled = ui->trigger_enable->isChecked();
        ui->trigger_mode->setEnabled(enabled);
        ui->trigger_edge->setEnabled(enabled);
        ui->trigger_level->setEnabled(enabled);
        ui->trigger_position->setEnabled(enabled);
    };
    connect(ui->trigger_enable, &QCheckBox::toggled,
            this, [refresh_trigger_enable](bool) { refresh_trigger_enable(); });
    refresh_trigger_enable();

    connect(all_wave, &QPushButton::clicked, this, [ui]() {
        for (int ch = 0; ch < 8; ++ch)
            ui->wave_enable[ch]->setChecked(true);
    });
    connect(selected_only, &QPushButton::clicked, this, [ui]() {
        for (int ch = 0; ch < 8; ++ch)
            ui->wave_enable[ch]->setChecked(ch == ui->selected_channel);
    });

    connect(copy_all, &QPushButton::clicked, this, [ui]() {
        const int src = std::max(0, std::min(7, ui->selected_channel));
        for (int ch = 0; ch < 8; ++ch) {
            if (ch == src)
                continue;
            ui->vzoom[ch]->setValue(ui->vzoom[src]->value());
            ui->vpos[ch]->setValue(ui->vpos[src]->value());
            ui->range_mode[ch]->setCurrentIndex(ui->range_mode[src]->currentIndex());
            ui->eng_min[ch]->setValue(ui->eng_min[src]->value());
            ui->eng_max[ch]->setValue(ui->eng_max[src]->value());
            ui->unit[ch]->setText(ui->unit[src]->text());
        }
    });

    auto fit_selected = [this, ui](bool full) {
        if (!_trace || !_trace->decoder())
            return;
        const int ch = std::max(0, std::min(7, ui->selected_channel));
        std::shared_ptr<pv::data::DecoderAnalogData> data;
        for (const auto &candidate : _trace->decoder()->analog_data_copy()) {
            if (candidate && candidate->channel() == ch) {
                data = candidate;
                break;
            }
        }
        if (!data)
            return;

        float mn = 0.0f, mx = 0.0f;
        bool found = false;
        if (full) {
            if (data->get_sample_count() == 0)
                return;
            mn = data->min_value();
            mx = data->max_value();
            found = true;
        } else {
            auto samples_view = data->read_samples();
            const auto &samples = samples_view.samples();
            if (!samples.empty() && _trace->get_view()) {
                double sr = _trace->decoder()->samplerate();
                if (sr <= 0.0)
                    sr = 1.0;
                const double spp = sr * _trace->get_view()->scale();
                const int64_t pix = _trace->get_view()->offset();
                const int w = _trace->get_view()->get_view_width();
                const uint64_t s0 = static_cast<uint64_t>(std::max(0.0, pix * spp));
                const uint64_t s1 = static_cast<uint64_t>(std::max(0.0, (pix + w) * spp));
                for (const auto &sample : samples) {
                    if (sample.end_sample < s0)
                        continue;
                    if (sample.start_sample > s1)
                        break;
                    if (!found) {
                        mn = mx = sample.value;
                        found = true;
                    } else {
                        mn = std::min(mn, sample.value);
                        mx = std::max(mx, sample.value);
                    }
                }
            }
        }
        if (!found)
            return;

        const float range = mx - mn;
        double vzoom = 1.0;
        double vpos = 1.0;
        if (range >= 0.0001f) {
            vzoom = 2.0 / static_cast<double>(range);
            vzoom = std::min(100.0, std::max(0.05, vzoom));
            vpos = 1.0 + 0.9 * ((static_cast<double>(mn) + mx) * 0.5) * vzoom;
            vpos = std::min(3.0, std::max(-3.0, vpos));
        }
        ui->vzoom[ch]->setValue(vzoom);
        ui->vpos[ch]->setValue(vpos);
    };
    connect(auto_range, &QPushButton::clicked, this,
            [fit_selected]() { fit_selected(false); });
    connect(full_range, &QPushButton::clicked, this,
            [fit_selected]() { fit_selected(true); });

    connect(reset, &QPushButton::clicked, this, [ui, set_combo_data, refresh_text_enable, refresh_trigger_enable]() {
        ui->bits->setValue(32);
        ui->channels->setCurrentIndex(7);
        set_combo_data(ui->align, QStringLiteral("I2S"));
        set_combo_data(ui->data_format, QStringLiteral("signed"));
        set_combo_data(ui->edge, QStringLiteral("rising"));
        set_combo_data(ui->frame_edge, QStringLiteral("high"));
        ui->realtime_decode->setChecked(false);
        ui->hide_decode_text->setChecked(true); // Fast decoder default: waveform only.
        ui->trigger_enable->setChecked(false);
        set_combo_data(ui->trigger_mode, QStringLiteral("auto"));
        set_combo_data(ui->trigger_edge, QStringLiteral("rising"));
        ui->trigger_level->setValue(0.0);
        ui->trigger_position->setValue(50);
        for (int ch = 0; ch < 8; ++ch) {
            ui->wave_enable[ch]->setChecked(true);
            ui->text_enable[ch]->setChecked(true);
            ui->vzoom[ch]->setValue(1.0);
            ui->vpos[ch]->setValue(1.0);
            set_combo_data(ui->range_mode[ch], QStringLiteral("bipolar"));
            ui->eng_min[ch]->setValue(-1.0);
            ui->eng_max[ch]->setValue(1.0);
            ui->unit[ch]->setText(QStringLiteral("V"));
        }
        ui->trigger_channel[0]->setChecked(true);
        ui->selected_channel = 0;
        ui->select_button[0]->setChecked(true);
        ui->selected_label->setText(QStringLiteral("当前: CH0"));
        if (ui->selected_combo)
            ui->selected_combo->setCurrentIndex(0);
        ui->advanced_stack->setCurrentIndex(0);
        refresh_text_enable();
        refresh_trigger_enable();
    });

    auto *bottom = new QWidget(parent);
    auto *bottom_layout = new QHBoxLayout(bottom);
    bottom_layout->setContentsMargins(0, 0, 0, 0);
    bottom_layout->setSpacing(8);
    bottom_layout->addWidget(advanced_group, 1);
    bottom_layout->addWidget(display_group, 1);
    decoder_form->addRow(bottom);

    auto *note = new QLabel(
        QStringLiteral("提示：矩阵里的 CH 可切换高级参数；\"波\"=波形，\"字\"=注释；Trig 为单选；Shift+左键拖动可做模拟区间测量。"),
        parent);
    note->setWordWrap(true);
    decoder_form->addRow(note);
}

void DecoderOptionsDlg::commit_tdm_audio_fast_options()
{
    if (!_trace)
        return;

    for (auto *ui : _tdm_fast_ui) {
        if (!ui || !ui->decoder || !ui->binding)
            continue;

        auto *binding = ui->binding;
        auto set_string = [binding](const char *id, const QString &value) {
            const QByteArray bytes = value.toUtf8();
            binding->setter(id, g_variant_new_string(bytes.constData()));
        };
        auto combo_value = [](QComboBox *combo) -> QString {
            return combo ? combo->currentData().toString() : QString();
        };

        binding->setter("bps", g_variant_new_int64(ui->bits->value()));
        binding->setter("channels", g_variant_new_int64(ui->channels->currentData().toInt()));
        set_string("align", combo_value(ui->align));
        set_string("data_format", combo_value(ui->data_format));
        set_string("edge", combo_value(ui->edge));
        set_string("frame_edge", combo_value(ui->frame_edge));
        binding->setter("realtime_decode", g_variant_new_boolean(ui->realtime_decode->isChecked()));

        bool any_text = false;
        bool any_wave = false;
        for (int ch = 0; ch < 8; ++ch) {
            char key[64];
            std::snprintf(key, sizeof(key), "ch%d_enable", ch);
            binding->setter(key, g_variant_new_int64(ui->wave_enable[ch]->isChecked() ? 1 : 0));

            std::snprintf(key, sizeof(key), "ch%d_vzoom", ch);
            binding->setter(key, g_variant_new_double(ui->vzoom[ch]->value()));
            std::snprintf(key, sizeof(key), "ch%d_vpos", ch);
            binding->setter(key, g_variant_new_double(ui->vpos[ch]->value()));
            std::snprintf(key, sizeof(key), "ch%d_range_mode", ch);
            set_string(key, combo_value(ui->range_mode[ch]));
            std::snprintf(key, sizeof(key), "ch%d_eng_min", ch);
            binding->setter(key, g_variant_new_double(ui->eng_min[ch]->value()));
            std::snprintf(key, sizeof(key), "ch%d_eng_max", ch);
            binding->setter(key, g_variant_new_double(ui->eng_max[ch]->value()));
            std::snprintf(key, sizeof(key), "ch%d_unit", ch);
            set_string(key, ui->unit[ch]->text().trimmed().isEmpty()
                                ? QStringLiteral("V") : ui->unit[ch]->text().trimmed());

            any_text = any_text || ui->text_enable[ch]->isChecked();
            any_wave = any_wave || ui->wave_enable[ch]->isChecked();
        }

        // Apply annotation-row visibility in the same stable order used by
        // DecoderGroupBox, while keeping analog visibility independent.
        int text_index = 0;
        auto rows = _trace->decoder()->get_rows_gshow();
        for (const auto &entry : rows) {
            if (entry.first.decoder() != ui->decoder->decoder() || text_index >= 8)
                continue;
            _trace->decoder()->set_rows_gshow(
                entry.first, ui->text_enable[text_index]->isChecked());
            ++text_index;
        }

        const bool show_text = !ui->hide_decode_text->isChecked();
        // Hiding text is implemented by the decoder output mode, not by
        // forcibly hiding the decoder object. This preserves the existing
        // trace height/master-eye state when a waveform-only configuration
        // is reopened and accepted without changes.
        if (show_text)
            ui->decoder->show(true);
        if (show_text && any_text && !any_wave)
            set_string("output", QStringLiteral("annotations"));
        else if (show_text && any_text)
            set_string("output", QStringLiteral("both"));
        else
            set_string("output", QStringLiteral("waveform"));

        int trigger_channel = 0;
        for (int ch = 0; ch < 8; ++ch) {
            if (ui->trigger_channel[ch] && ui->trigger_channel[ch]->isChecked()) {
                trigger_channel = ch;
                break;
            }
        }
        binding->setter("display_trigger_enable",
                        g_variant_new_boolean(ui->trigger_enable->isChecked()));
        set_string("display_trigger_mode", combo_value(ui->trigger_mode));
        binding->setter("display_trigger_channel", g_variant_new_int64(trigger_channel));
        set_string("display_trigger_edge", combo_value(ui->trigger_edge));
        binding->setter("display_trigger_level",
                        g_variant_new_double(ui->trigger_level->value()));
        binding->setter("display_trigger_position",
                        g_variant_new_int64(ui->trigger_position->value()));
    }
}

void DecoderOptionsDlg::create_pwm_fast_options(
    data::decode::Decoder *dec, QWidget *parent,
    QFormLayout *decoder_form, prop::binding::DecoderOptions *binding)
{
    if (!dec || !parent || !decoder_form || !binding || !_trace)
        return;

    auto *ui = new PwmFastUi();
    ui->decoder = dec;
    ui->binding = binding;
    _pwm_fast_ui.push_back(ui);

    auto opt_int = [binding](const char *id, qint64 fallback) -> qint64 {
        GVariant *v = binding->getter(id);
        if (!v) return fallback;
        qint64 r = fallback;
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64)) r = g_variant_get_int64(v);
        g_variant_unref(v);
        return r;
    };
    auto opt_double = [binding](const char *id, double fallback) -> double {
        GVariant *v = binding->getter(id);
        if (!v) return fallback;
        double r = fallback;
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE)) r = g_variant_get_double(v);
        g_variant_unref(v);
        return r;
    };
    auto opt_bool = [binding](const char *id, bool fallback) -> bool {
        GVariant *v = binding->getter(id);
        if (!v) return fallback;
        bool r = fallback;
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) r = g_variant_get_boolean(v);
        g_variant_unref(v);
        return r;
    };
    auto opt_string = [binding](const char *id, const char *fallback) -> QString {
        GVariant *v = binding->getter(id);
        if (!v) return QString::fromUtf8(fallback);
        QString r = QString::fromUtf8(fallback);
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
            r = QString::fromUtf8(g_variant_get_string(v, nullptr));
        g_variant_unref(v);
        return r;
    };
    auto set_combo_data = [](QComboBox *combo, const QString &value) {
        if (!combo) return;
        const int i = combo->findData(value);
        if (i >= 0) combo->setCurrentIndex(i);
    };
    auto set_combo_int = [](QComboBox *combo, int value) {
        if (!combo) return;
        const int i = combo->findData(value);
        if (i >= 0) combo->setCurrentIndex(i);
    };

    // ---- PWM common / reconstruction controls. ----
    auto *common_group = new QGroupBox(QStringLiteral("PWM / 重建"), parent);
    auto *common = new QGridLayout(common_group);
    common->setContentsMargins(8, 20, 8, 6);
    common->setHorizontalSpacing(6);
    common->setVerticalSpacing(5);

    ui->output = new DsComboBox(common_group);
    ui->output->addItem(QStringLiteral("仅波形"), QStringLiteral("Waveform"));
    ui->output->addItem(QStringLiteral("波形+注释"), QStringLiteral("Waveform + annotations"));
    ui->output->addItem(QStringLiteral("波形+参数"), QStringLiteral("Waveform + parameters"));
    ui->output->addItem(QStringLiteral("仅参数"), QStringLiteral("Parameters only"));
    ui->output->addItem(QStringLiteral("仅注释"), QStringLiteral("Annotations only"));
    set_combo_data(ui->output, opt_string("output", "Waveform"));
    ui->output->setMaximumWidth(155);

    ui->analog_mode = new DsComboBox(common_group);
    ui->analog_mode->addItem(QStringLiteral("Duty 0~100%"), QStringLiteral("Duty 0-100%"));
    ui->analog_mode->addItem(QStringLiteral("电源控制 0~100%"), QStringLiteral("Power-supply control 0-100%"));
    ui->analog_mode->addItem(QStringLiteral("Class-D 单端"), QStringLiteral("Class-D centered -100..+100%"));
    ui->analog_mode->addItem(QStringLiteral("Class-D 差分"), QStringLiteral("Class-D differential (PWM+ - PWM-)"));
    set_combo_data(ui->analog_mode, opt_string("analog_mode", "Duty 0-100%"));
    ui->analog_mode->setMaximumWidth(170);

    ui->polarity_a = new DsComboBox(common_group);
    ui->polarity_a->addItem(QStringLiteral("高有效"), QStringLiteral("active-high"));
    ui->polarity_a->addItem(QStringLiteral("低有效"), QStringLiteral("active-low"));
    set_combo_data(ui->polarity_a, opt_string("polarity", "active-high"));
    ui->polarity_a->setMaximumWidth(110);

    ui->polarity_b = new DsComboBox(common_group);
    ui->polarity_b->addItem(QStringLiteral("高有效"), QStringLiteral("active-high"));
    ui->polarity_b->addItem(QStringLiteral("低有效"), QStringLiteral("active-low"));
    set_combo_data(ui->polarity_b, opt_string("polarity_b", "active-high"));
    ui->polarity_b->setMaximumWidth(110);

    ui->filter = new DsComboBox(common_group);
    ui->filter->addItem(QStringLiteral("无"), QStringLiteral("None"));
    ui->filter->addItem(QStringLiteral("滑动平均"), QStringLiteral("Moving average"));
    ui->filter->addItem(QStringLiteral("指数平滑"), QStringLiteral("Exponential smoothing"));
    ui->filter->addItem(QStringLiteral("RC低通"), QStringLiteral("RC low-pass"));
    set_combo_data(ui->filter, opt_string("filter", "None"));
    ui->filter->setMaximumWidth(135);

    ui->filter_cycles = new DsComboBox(common_group);
    for (int v : {1,2,4,8,16,32,64,128}) ui->filter_cycles->addItem(QString::number(v), v);
    set_combo_int(ui->filter_cycles, static_cast<int>(opt_int("filter_cycles", 8)));
    ui->filter_cycles->setMaximumWidth(90);

    ui->filter_cutoff = new QDoubleSpinBox(common_group);
    ui->filter_cutoff->setDecimals(2);
    ui->filter_cutoff->setRange(0.01, 10000000.0);
    ui->filter_cutoff->setValue(opt_double("filter_cutoff_hz", 20000.0));
    ui->filter_cutoff->setSuffix(QStringLiteral(" Hz"));
    ui->filter_cutoff->setMaximumWidth(140);

    auto make_decimals = [common_group](int current) -> QComboBox * {
        auto *c = new DsComboBox(common_group);
        for (int v : {0,1,2,3,4,5,6,8}) c->addItem(QString::number(v), v);
        const int i = c->findData(current); if (i >= 0) c->setCurrentIndex(i);
        c->setMaximumWidth(80);
        return c;
    };
    ui->duty_decimals = make_decimals(static_cast<int>(opt_int("duty_decimals", 2)));
    ui->time_decimals = make_decimals(static_cast<int>(opt_int("time_decimals", 3)));
    ui->freq_decimals = make_decimals(static_cast<int>(opt_int("freq_decimals", 2)));
    ui->realtime_decode = new QCheckBox(QStringLiteral("实时解码"), common_group);
    ui->realtime_decode->setChecked(opt_bool("realtime_decode", false));

    common->addWidget(new QLabel(QStringLiteral("输出"), common_group), 0, 0);
    common->addWidget(ui->output, 0, 1);
    common->addWidget(new QLabel(QStringLiteral("模式"), common_group), 0, 2);
    common->addWidget(ui->analog_mode, 0, 3);
    common->addWidget(new QLabel(QStringLiteral("A极性"), common_group), 1, 0);
    common->addWidget(ui->polarity_a, 1, 1);
    common->addWidget(new QLabel(QStringLiteral("B极性"), common_group), 1, 2);
    common->addWidget(ui->polarity_b, 1, 3);
    common->addWidget(new QLabel(QStringLiteral("滤波"), common_group), 2, 0);
    common->addWidget(ui->filter, 2, 1);
    common->addWidget(new QLabel(QStringLiteral("周期数"), common_group), 2, 2);
    common->addWidget(ui->filter_cycles, 2, 3);
    common->addWidget(new QLabel(QStringLiteral("RC截止"), common_group), 3, 0);
    common->addWidget(ui->filter_cutoff, 3, 1);
    common->addWidget(new QLabel(QStringLiteral("Duty小数"), common_group), 3, 2);
    common->addWidget(ui->duty_decimals, 3, 3);
    common->addWidget(new QLabel(QStringLiteral("时间小数"), common_group), 4, 0);
    common->addWidget(ui->time_decimals, 4, 1);
    common->addWidget(new QLabel(QStringLiteral("频率小数"), common_group), 4, 2);
    common->addWidget(ui->freq_decimals, 4, 3);
    common->addWidget(ui->realtime_decode, 5, 0, 1, 4);
    decoder_form->addRow(common_group);

    // ---- CH0..CH3 compact matrix. ----
    auto *matrix_group = new QGroupBox(QStringLiteral("模拟通道"), parent);
    auto *matrix = new QGridLayout(matrix_group);
    matrix->setContentsMargins(8, 20, 8, 6);
    matrix->setHorizontalSpacing(8);
    matrix->setVerticalSpacing(3);
    const QString headers[] = {QStringLiteral("CH"), QStringLiteral("波"),
        QStringLiteral("Zoom"), QStringLiteral("Pos"), QStringLiteral("Trig")};
    for (int c = 0; c < 5; ++c) matrix->addWidget(new QLabel(headers[c], matrix_group), 0, c);
    auto *select_group = new QButtonGroup(matrix_group); select_group->setExclusive(true);
    auto *trig_group = new QButtonGroup(matrix_group); trig_group->setExclusive(true);
    const int saved_trig = std::max(0, std::min(3, static_cast<int>(opt_int("display_trigger_channel", 0))));

    for (int ch = 0; ch < 4; ++ch) {
        char key[64];
        ui->select_button[ch] = new QToolButton(matrix_group);
        ui->select_button[ch]->setText(QStringLiteral("CH%1").arg(ch));
        ui->select_button[ch]->setCheckable(true);
        ui->select_button[ch]->setMaximumWidth(52);
        select_group->addButton(ui->select_button[ch], ch);
        ui->wave_enable[ch] = new QCheckBox(matrix_group);
        std::snprintf(key, sizeof(key), "ch%d_enable", ch);
        ui->wave_enable[ch]->setChecked(opt_int(key, ch == 0 ? 1 : 0) != 0);
        std::snprintf(key, sizeof(key), "ch%d_vzoom", ch);
        ui->zoom_summary[ch] = new QLabel(QString::number(opt_double(key, 1.0), 'f', 3), matrix_group);
        std::snprintf(key, sizeof(key), "ch%d_vpos", ch);
        ui->pos_summary[ch] = new QLabel(QString::number(opt_double(key, -2.0 * ch), 'f', 3), matrix_group);
        ui->trigger_channel[ch] = new QRadioButton(matrix_group);
        trig_group->addButton(ui->trigger_channel[ch], ch);
        ui->trigger_channel[ch]->setChecked(ch == saved_trig);
        matrix->addWidget(ui->select_button[ch], ch + 1, 0);
        matrix->addWidget(ui->wave_enable[ch], ch + 1, 1, Qt::AlignCenter);
        matrix->addWidget(ui->zoom_summary[ch], ch + 1, 2);
        matrix->addWidget(ui->pos_summary[ch], ch + 1, 3);
        matrix->addWidget(ui->trigger_channel[ch], ch + 1, 4, Qt::AlignCenter);
    }
    decoder_form->addRow(matrix_group);

    // ---- Selected channel advanced editor. ----
    auto *adv_group = new QGroupBox(QStringLiteral("当前通道"), parent);
    auto *adv_layout = new QVBoxLayout(adv_group);
    adv_layout->setContentsMargins(8, 20, 8, 6);
    auto *head = new QWidget(adv_group);
    auto *head_l = new QHBoxLayout(head); head_l->setContentsMargins(0,0,0,0);
    ui->selected_label = new QLabel(QStringLiteral("当前: CH0"), head);
    ui->selected_combo = new DsComboBox(head);
    for (int ch = 0; ch < 4; ++ch) ui->selected_combo->addItem(QStringLiteral("CH%1").arg(ch), ch);
    ui->selected_combo->setMaximumWidth(90);
    head_l->addWidget(ui->selected_label); head_l->addStretch(1);
    head_l->addWidget(new QLabel(QStringLiteral("切换"), head)); head_l->addWidget(ui->selected_combo);
    ui->advanced_stack = new QStackedWidget(adv_group);
    adv_layout->addWidget(head); adv_layout->addWidget(ui->advanced_stack);

    for (int ch = 0; ch < 4; ++ch) {
        char key[64];
        auto *page = new QWidget(ui->advanced_stack);
        auto *g = new QGridLayout(page); g->setContentsMargins(0,0,0,0); g->setHorizontalSpacing(6);
        ui->vzoom[ch] = new QDoubleSpinBox(page); ui->vzoom[ch]->setDecimals(3);
        ui->vzoom[ch]->setRange(0.01, 100.0); ui->vzoom[ch]->setSingleStep(0.05); ui->vzoom[ch]->setSuffix(QStringLiteral(" x")); ui->vzoom[ch]->setMaximumWidth(120);
        std::snprintf(key,sizeof(key),"ch%d_vzoom",ch); ui->vzoom[ch]->setValue(opt_double(key,1.0));
        ui->vpos[ch] = new QDoubleSpinBox(page); ui->vpos[ch]->setDecimals(3); ui->vpos[ch]->setRange(-800.0,800.0); ui->vpos[ch]->setSingleStep(0.05); ui->vpos[ch]->setMaximumWidth(120);
        std::snprintf(key,sizeof(key),"ch%d_vpos",ch); ui->vpos[ch]->setValue(opt_double(key,-2.0*ch));
        ui->range_mode[ch] = new DsComboBox(page);
        ui->range_mode[ch]->addItem(QStringLiteral("单极性"),QStringLiteral("unipolar"));
        ui->range_mode[ch]->addItem(QStringLiteral("双极性"),QStringLiteral("bipolar"));
        ui->range_mode[ch]->addItem(QStringLiteral("自定义"),QStringLiteral("custom"));
        std::snprintf(key,sizeof(key),"ch%d_range_mode",ch); set_combo_data(ui->range_mode[ch],opt_string(key,"unipolar")); ui->range_mode[ch]->setMaximumWidth(120);
        ui->eng_min[ch] = new QDoubleSpinBox(page); ui->eng_min[ch]->setDecimals(6); ui->eng_min[ch]->setRange(-1e12,1e12); ui->eng_min[ch]->setMaximumWidth(120);
        std::snprintf(key,sizeof(key),"ch%d_eng_min",ch); ui->eng_min[ch]->setValue(opt_double(key,0.0));
        ui->eng_max[ch] = new QDoubleSpinBox(page); ui->eng_max[ch]->setDecimals(6); ui->eng_max[ch]->setRange(-1e12,1e12); ui->eng_max[ch]->setMaximumWidth(120);
        std::snprintf(key,sizeof(key),"ch%d_eng_max",ch); ui->eng_max[ch]->setValue(opt_double(key,100.0));
        ui->unit[ch] = new QLineEdit(page); ui->unit[ch]->setMaxLength(16); ui->unit[ch]->setMaximumWidth(90);
        std::snprintf(key,sizeof(key),"ch%d_unit",ch); ui->unit[ch]->setText(opt_string(key,"%"));
        g->addWidget(new QLabel(QStringLiteral("V-Zoom"),page),0,0); g->addWidget(ui->vzoom[ch],0,1);
        g->addWidget(new QLabel(QStringLiteral("V-Pos"),page),0,2); g->addWidget(ui->vpos[ch],0,3);
        g->addWidget(new QLabel(QStringLiteral("量程"),page),1,0); g->addWidget(ui->range_mode[ch],1,1);
        g->addWidget(new QLabel(QStringLiteral("Min"),page),1,2); g->addWidget(ui->eng_min[ch],1,3);
        g->addWidget(new QLabel(QStringLiteral("Max"),page),2,0); g->addWidget(ui->eng_max[ch],2,1);
        g->addWidget(new QLabel(QStringLiteral("单位"),page),2,2); g->addWidget(ui->unit[ch],2,3);
        ui->advanced_stack->addWidget(page);
        connect(ui->vzoom[ch], QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [ui,ch](double v){ if(ui->zoom_summary[ch]) ui->zoom_summary[ch]->setText(QString::number(v,'f',3)); });
        connect(ui->vpos[ch], QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [ui,ch](double v){ if(ui->pos_summary[ch]) ui->pos_summary[ch]->setText(QString::number(v,'f',3)); });
    }

    auto select_channel = [ui](int ch) {
        if (ch < 0 || ch >= 4) return;
        ui->selected_channel = ch;
        ui->selected_label->setText(QStringLiteral("当前: CH%1").arg(ch));
        ui->advanced_stack->setCurrentIndex(ch);
        if (ui->select_button[ch]) ui->select_button[ch]->setChecked(true);
        if (ui->selected_combo) { const QSignalBlocker b(ui->selected_combo); ui->selected_combo->setCurrentIndex(ch); }
    };
    for (int ch = 0; ch < 4; ++ch) {
        connect(ui->select_button[ch], &QToolButton::clicked, this, [select_channel,ch](bool){select_channel(ch);});
        connect(ui->trigger_channel[ch], &QRadioButton::clicked, this, [select_channel,ch](bool){select_channel(ch);});
    }
    connect(ui->selected_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [select_channel](int i){select_channel(i);});
    select_channel(saved_trig);

    // ---- Trigger + quick display controls. ----
    auto *disp_group = new QGroupBox(QStringLiteral("显示 / 触发"), parent);
    auto *disp = new QGridLayout(disp_group); disp->setContentsMargins(8,20,8,6); disp->setHorizontalSpacing(6);
    ui->trigger_enable = new QCheckBox(QStringLiteral("启用模拟显示触发"), disp_group);
    ui->trigger_enable->setChecked(opt_bool("display_trigger_enable", false));
    ui->trigger_mode = new DsComboBox(disp_group); ui->trigger_mode->addItem("Auto","auto"); ui->trigger_mode->addItem("Normal","normal"); set_combo_data(ui->trigger_mode,opt_string("display_trigger_mode","auto")); ui->trigger_mode->setMaximumWidth(100);
    ui->trigger_edge = new DsComboBox(disp_group); ui->trigger_edge->addItem("Rising","rising"); ui->trigger_edge->addItem("Falling","falling"); ui->trigger_edge->addItem("Either","either"); set_combo_data(ui->trigger_edge,opt_string("display_trigger_edge","rising")); ui->trigger_edge->setMaximumWidth(100);
    ui->trigger_level = new QDoubleSpinBox(disp_group); ui->trigger_level->setDecimals(6); ui->trigger_level->setRange(-1e12,1e12); ui->trigger_level->setValue(opt_double("display_trigger_level",50.0)); ui->trigger_level->setMaximumWidth(120);
    ui->trigger_position = new QSpinBox(disp_group); ui->trigger_position->setRange(0,100); ui->trigger_position->setSuffix(" %"); ui->trigger_position->setValue(static_cast<int>(opt_int("display_trigger_position",50))); ui->trigger_position->setMaximumWidth(90);
    disp->addWidget(ui->trigger_enable,0,0,1,4);
    disp->addWidget(new QLabel("Mode",disp_group),1,0); disp->addWidget(ui->trigger_mode,1,1); disp->addWidget(new QLabel("Edge",disp_group),1,2); disp->addWidget(ui->trigger_edge,1,3);
    disp->addWidget(new QLabel("Level",disp_group),2,0); disp->addWidget(ui->trigger_level,2,1); disp->addWidget(new QLabel("Pos",disp_group),2,2); disp->addWidget(ui->trigger_position,2,3);
    auto *all_wave = new QPushButton(QStringLiteral("全部通道"),disp_group);
    auto *only = new QPushButton(QStringLiteral("仅当前"),disp_group);
    auto *auto_range = new QPushButton(QStringLiteral("自动量程"),disp_group);
    auto *full_range = new QPushButton(QStringLiteral("全量程"),disp_group);
    auto *copy = new QPushButton(QStringLiteral("复制→全部"),disp_group);
    auto *reset = new QPushButton(QStringLiteral("恢复默认"),disp_group);
    disp->addWidget(all_wave,3,0); disp->addWidget(only,3,1); disp->addWidget(auto_range,3,2); disp->addWidget(full_range,3,3); disp->addWidget(copy,4,0,1,2); disp->addWidget(reset,4,2,1,2);

    auto trig_en = [ui](){ const bool e=ui->trigger_enable->isChecked(); ui->trigger_mode->setEnabled(e); ui->trigger_edge->setEnabled(e); ui->trigger_level->setEnabled(e); ui->trigger_position->setEnabled(e); };
    connect(ui->trigger_enable,&QCheckBox::toggled,this,[trig_en](bool){trig_en();}); trig_en();
    connect(all_wave,&QPushButton::clicked,this,[ui](){for(int ch=0;ch<4;++ch)ui->wave_enable[ch]->setChecked(true);});
    connect(only,&QPushButton::clicked,this,[ui](){for(int ch=0;ch<4;++ch)ui->wave_enable[ch]->setChecked(ch==ui->selected_channel);});
    connect(copy,&QPushButton::clicked,this,[ui](){int src=std::clamp(ui->selected_channel,0,3);for(int ch=0;ch<4;++ch){if(ch==src)continue;ui->vzoom[ch]->setValue(ui->vzoom[src]->value());ui->vpos[ch]->setValue(ui->vpos[src]->value());ui->range_mode[ch]->setCurrentIndex(ui->range_mode[src]->currentIndex());ui->eng_min[ch]->setValue(ui->eng_min[src]->value());ui->eng_max[ch]->setValue(ui->eng_max[src]->value());ui->unit[ch]->setText(ui->unit[src]->text());}});

    auto fit_selected = [this,ui](bool full){
        if(!_trace||!_trace->decoder())return; int ch=std::clamp(ui->selected_channel,0,3); std::shared_ptr<pv::data::DecoderAnalogData> data;
        for(const auto &x:_trace->decoder()->analog_data_copy()) if(x&&x->channel()==ch){data=x;break;} if(!data||data->get_sample_count()==0)return;
        float mn=0,mx=0; bool found=false;
        if(full){mn=data->min_value();mx=data->max_value();found=true;}
        else {auto rv=data->read_samples(); const auto &ss=rv.samples(); if(!ss.empty()&&_trace->get_view()){double sr=_trace->decoder()->samplerate();if(sr<=0)sr=1;double spp=sr*_trace->get_view()->scale();int64_t pix=_trace->get_view()->offset();int w=_trace->get_view()->get_view_width();uint64_t s0=(uint64_t)std::max(0.0,pix*spp),s1=(uint64_t)std::max(0.0,(pix+w)*spp);for(const auto &a:ss){if(a.end_sample<s0)continue;if(a.start_sample>s1)break;if(!found){mn=mx=a.value;found=true;}else{mn=std::min(mn,a.value);mx=std::max(mx,a.value);}}}}
        if(!found)return; float range=mx-mn; double vz=1.0,vp=1.0;if(range>=0.0001f){vz=std::clamp(2.0/(double)range,0.05,100.0);vp=std::clamp(1.0+0.9*((double)mn+mx)*0.5*vz,-3.0,3.0);}ui->vzoom[ch]->setValue(vz);ui->vpos[ch]->setValue(vp);
    };
    connect(auto_range,&QPushButton::clicked,this,[fit_selected](){fit_selected(false);});
    connect(full_range,&QPushButton::clicked,this,[fit_selected](){fit_selected(true);});

    connect(reset,&QPushButton::clicked,this,[ui,set_combo_data,set_combo_int,trig_en](){
        set_combo_data(ui->polarity_a,"active-high"); set_combo_data(ui->polarity_b,"active-high"); set_combo_data(ui->output,"Waveform"); set_combo_data(ui->analog_mode,"Duty 0-100%"); set_combo_data(ui->filter,"None"); set_combo_int(ui->filter_cycles,8); ui->filter_cutoff->setValue(20000.0); set_combo_int(ui->duty_decimals,2); set_combo_int(ui->time_decimals,3); set_combo_int(ui->freq_decimals,2); ui->realtime_decode->setChecked(false);
        for(int ch=0;ch<4;++ch){ui->wave_enable[ch]->setChecked(ch==0);ui->vzoom[ch]->setValue(1.0);ui->vpos[ch]->setValue(-2.0*ch);set_combo_data(ui->range_mode[ch],"unipolar");ui->eng_min[ch]->setValue(0.0);ui->eng_max[ch]->setValue(100.0);ui->unit[ch]->setText("%");}
        ui->trigger_enable->setChecked(false); set_combo_data(ui->trigger_mode,"auto"); set_combo_data(ui->trigger_edge,"rising"); ui->trigger_level->setValue(50.0); ui->trigger_position->setValue(50); ui->trigger_channel[0]->setChecked(true); ui->selected_channel=0; ui->advanced_stack->setCurrentIndex(0); ui->selected_combo->setCurrentIndex(0); ui->selected_label->setText("当前: CH0"); trig_en();
    });

    auto *bottom = new QWidget(parent); auto *bl = new QHBoxLayout(bottom); bl->setContentsMargins(0,0,0,0); bl->setSpacing(8); bl->addWidget(adv_group,1); bl->addWidget(disp_group,1); decoder_form->addRow(bottom);
    auto *note = new QLabel(QStringLiteral("提示：CH0=重建输出，CH1=Duty A，CH2=Duty B，CH3=共模；Shift+左键可做模拟区间测量。"), parent); note->setWordWrap(true); decoder_form->addRow(note);
}

void DecoderOptionsDlg::commit_pwm_fast_options()
{
    for (auto *ui : _pwm_fast_ui) {
        if (!ui || !ui->binding) continue;
        auto *b = ui->binding;
        auto set_string = [b](const char *id, QComboBox *c) {
            const QByteArray bytes = c->currentData().toString().toUtf8();
            b->setter(id, g_variant_new_string(bytes.constData()));
        };
        set_string("polarity", ui->polarity_a);
        set_string("polarity_b", ui->polarity_b);
        set_string("output", ui->output);
        set_string("analog_mode", ui->analog_mode);
        set_string("filter", ui->filter);
        b->setter("filter_cycles", g_variant_new_int64(ui->filter_cycles->currentData().toInt()));
        b->setter("filter_cutoff_hz", g_variant_new_double(ui->filter_cutoff->value()));
        b->setter("duty_decimals", g_variant_new_int64(ui->duty_decimals->currentData().toInt()));
        b->setter("time_decimals", g_variant_new_int64(ui->time_decimals->currentData().toInt()));
        b->setter("freq_decimals", g_variant_new_int64(ui->freq_decimals->currentData().toInt()));
        b->setter("realtime_decode", g_variant_new_boolean(ui->realtime_decode->isChecked()));
        for (int ch=0; ch<4; ++ch) {
            char key[64];
            std::snprintf(key,sizeof(key),"ch%d_enable",ch); b->setter(key,g_variant_new_int64(ui->wave_enable[ch]->isChecked()?1:0));
            std::snprintf(key,sizeof(key),"ch%d_vzoom",ch); b->setter(key,g_variant_new_double(ui->vzoom[ch]->value()));
            std::snprintf(key,sizeof(key),"ch%d_vpos",ch); b->setter(key,g_variant_new_double(ui->vpos[ch]->value()));
            std::snprintf(key,sizeof(key),"ch%d_range_mode",ch); set_string(key,ui->range_mode[ch]);
            std::snprintf(key,sizeof(key),"ch%d_eng_min",ch); b->setter(key,g_variant_new_double(ui->eng_min[ch]->value()));
            std::snprintf(key,sizeof(key),"ch%d_eng_max",ch); b->setter(key,g_variant_new_double(ui->eng_max[ch]->value()));
            std::snprintf(key,sizeof(key),"ch%d_unit",ch); {QByteArray x=ui->unit[ch]->text().trimmed().toUtf8(); if(x.isEmpty())x="%"; b->setter(key,g_variant_new_string(x.constData()));}
        }
        int tch=0; for(int ch=0;ch<4;++ch)if(ui->trigger_channel[ch]->isChecked()){tch=ch;break;}
        b->setter("display_trigger_enable",g_variant_new_boolean(ui->trigger_enable->isChecked()));
        set_string("display_trigger_mode",ui->trigger_mode);
        b->setter("display_trigger_channel",g_variant_new_int64(tch));
        set_string("display_trigger_edge",ui->trigger_edge);
        b->setter("display_trigger_level",g_variant_new_double(ui->trigger_level->value()));
        b->setter("display_trigger_position",g_variant_new_int64(ui->trigger_position->value()));
    }
}

void DecoderOptionsDlg::commit_probes()
{ 
    for(auto &up : _trace->decoder()->stack()){
        auto dec = up.get();
        commit_decoder_probes(dec);
    }
}

void DecoderOptionsDlg::commit_decoder_probes(data::decode::Decoder *dec)
{
	if (!dec || !_trace) return;
	assert(dec);
    assert(_trace);

    std::map<const srd_channel*, int> probe_map;
    auto *view = _trace->get_view();
    if (!view) {
        dec->set_probes(probe_map);
        return;
    }
    const auto &sigs = view->session().get_signal_models();

    std::list<int> index_list;

	for(auto &p : _probe_selectors)
	{
		if(p._decoder != dec)
			break;

        const int selection = p._combo->itemData(p._combo->currentIndex()).value<int>();

        for(auto s : sigs){
            if(s->index() == selection) {
                probe_map[p._pdch] = selection;
                index_list.push_back(selection);
				break;
			}
        }
	}

	dec->set_probes(probe_map);

    if (index_list.size())
        _trace->set_index_list(index_list);
}
 
void DecoderOptionsDlg::on_accept()
{ 
    if (_cursor1 > 0 && _cursor1 == _cursor2){
        MsgBox::Show(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_ERROR), "error"), 
        L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODE_INVAILD_CURSOR), "Invalid cursor index for sample range!"));
        return;
    }

    this->accept();
}

void DecoderOptionsDlg::on_trans_pramas()
{
    QCheckBox *ck_box = dynamic_cast<QCheckBox*>(sender());
    if (!ck_box) return;
    assert(ck_box);

    AppConfig::Instance().appOptions.transDecoderDlg = ck_box->isChecked();
    AppConfig::Instance().SaveApp();
    _is_reload_form = true;
    this->reject();
}

void DecoderOptionsDlg::apply_setting()
{
    commit_probes();
    for (auto b : _bindings) {
        b->commit();
    }
    commit_tdm_audio_fast_options();
    commit_pwm_fast_options();
}

}//dialogs
}//pv
