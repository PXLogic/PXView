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

#include "decoderoptionsdlg.h"
#include <libsigrokdecode.h>
#include <QScrollArea>
#include <QDialogButtonBox>
#include <assert.h>
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
#include "../ui/popupdlglist.h"

#include "../data/decoderstack.h"
#include "../prop/binding/decoderoptions.h"
#include "../data/decode/decoder.h"
#include "../ui/dscombobox.h"
#include "../view/logicsignal.h"
#include "../sigsession.h"
#include "../view/view.h"
#include "../view/cursor.h"
#include "../widgets/decodergroupbox.h"
#include "../view/decodetrace.h"
#include "../ui/msgbox.h"

#include "../ui/langresource.h"
#include "../config/appconfig.h"
#include "../ui/dockfonts.h"

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
        
        for (auto c : cursor_list){
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
 
    for(auto dec : _trace->decoder()->stack()) 
    { 
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

    const auto &sigs = _trace->get_view()->session().get_signal_models();

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
    const uint64_t last_samples = _trace->get_view()->session().cur_samplelimits() - 1;
    const int index1 = _start_comboBox->currentIndex();
    const int index2 = _end_comboBox->currentIndex();
    uint64_t decode_start, decode_end;

    auto view = _trace->get_view();

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
  
    for(auto dec : _trace->decoder()->stack()) {
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

        const char *desc_str = NULL;
        const char *lang_str = NULL;

        if (pdch->idn != NULL && LangResource::Instance()->is_lang_en() == false){
            lang_str = LangResource::Instance()->get_lang_text(STR_PAGE_DECODER, pdch->idn, pdch->desc);
        }

        if (lang_str != NULL && bLang){
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
		
        const char *desc_str = NULL;
        const char *lang_str = NULL;
        
        if (pdch->idn != NULL && LangResource::Instance()->is_lang_en() == false){
            lang_str = LangResource::Instance()->get_lang_text(STR_PAGE_DECODER, pdch->idn, pdch->desc);
        }

        if (lang_str != NULL && bLang){
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

	// Add the options
    auto binding = new prop::binding::DecoderOptions(_trace->decoder(), dec);
    binding->add_properties_to_form(decoder_form, false, font);
	_bindings.push_back(binding);
  
    auto group = new pv::widgets::DecoderGroupBox(_trace->decoder().get(), 
                            dec, 
                            decoder_form, 
                            parent, font);

    if (group->_content_width > _content_width){
        _content_width = group->_content_width;
    }

	form->addRow(group);
}

void DecoderOptionsDlg::commit_probes()
{ 
    for(auto dec : _trace->decoder()->stack()){
		commit_decoder_probes(dec);
    }
}

void DecoderOptionsDlg::commit_decoder_probes(data::decode::Decoder *dec)
{
	if (!dec || !_trace) return;
	assert(dec);
    assert(_trace);

    std::map<const srd_channel*, int> probe_map;
    const auto &sigs = _trace->get_view()->session().get_signal_models();

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
}

}//dialogs
}//pv
