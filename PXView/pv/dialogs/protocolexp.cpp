/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
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

#include "pv/dialogs/protocolexp.h"
  
#include <QFormLayout>
#include <QListWidget>
#include <QFile>
#include <QFileDialog>
#include <QTextStream>
#include <QProgressDialog>
#include <QFuture>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>

#include "pv/session/sigsession.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/data/decode/decoder.h"
#include "pv/data/decode/row.h"
#include "pv/data/decode/annotation.h"
#include "pv/view/trace/decodetrace.h"
#include "pv/data/model/decodermodel.h"
#include "pv/base/eventobject.h"
#include "pv/config/appconfig.h"
#include "pv/base/pxvdef.h"
#include "pv/utility/encoding.h"
#include "pv/utility/path.h"
#include "pv/base/log.h"
#include "pv/core/langresource.h"
#include "pv/ui/msgbox.h"

// Upper bound on the number of export columns. In multi-stack (All) mode the
// columns of every decoder are listed, so a few simultaneous protocols must
// still fit (e.g. 4 stacks x 4 rows).
#define EXPORT_DEC_ROW_COUNT_MAX 64

using namespace pv::data::decode;

namespace pv {
namespace dialogs {

ProtocolExp::ProtocolExp(QWidget *parent, SigSession *session, pv::view::DecoderModel *decoder_model) :
    PxDialog(parent),
    _session(session), _data_src(session),
    _decoder_model(decoder_model),
    _button_box(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        Qt::Horizontal, this),
    _export_cancel(false)
{
    _format_combobox = new DsComboBox(this);
    //tr
    _format_combobox->addItem("Comma-Separated Values (*.csv)");
    _format_combobox->addItem("Text files (*.txt)");

    _flayout = new QFormLayout();
    _flayout->setVerticalSpacing(5);
    _flayout->setFormAlignment(Qt::AlignLeft);
    _flayout->setLabelAlignment(Qt::AlignLeft);
    _flayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    _flayout->addRow(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_EXPORT_FORMAT), "Export Format: "), this), _format_combobox);

    _multi_stack = decoder_model->isMultiStackMode();
    if (_multi_stack)
        _export_stacks = decoder_model->allStacks();

    if (_multi_stack) {
        // "All" (multi-stack) mode: one checkbox per decode row of every stack.
        // The label mirrors the table column header ("UART(CH1):tx") so two
        // instances of the same protocol — e.g. UART on TX and UART on RX —
        // stay distinguishable. Every row is pre-checked: exporting several
        // stacks in one file is the whole point of this mode.
        for (size_t si = 0; si < _export_stacks.size(); si++) {
            pv::data::DecoderStack *stack = _export_stacks[si];
            if (!stack)
                continue;

            const QString prefix =
                pv::view::DecoderModel::stackDisplayName(stack, (int)si);
            auto rows = stack->get_rows_lshow();
            int row_index = 0; // visible decode-row index inside this stack

            for (auto i = rows.begin(); i != rows.end(); i++) {
                if (!(*i).second)
                    continue;

                QString desc = (*i).first.description();
                if (desc.isEmpty())
                    desc = (*i).first.title();
                if (desc.isEmpty())
                    desc = QString::number(row_index);

                const QString title = prefix + ":" + desc;
                QLabel *row_label = new QLabel(title, this);
                QCheckBox *row_sel = new QCheckBox(this);
                row_sel->setChecked(true);
                _row_label_list.push_back(row_label);
                _row_sel_list.push_back(row_sel);
                _flayout->addRow(row_label, row_sel);
                row_sel->setProperty("stack", (int)si);
                row_sel->setProperty("rowindex", row_index);
                row_sel->setProperty("title", title);
                row_index++;
            }
        }
    } else {
        const auto decoder_stack = decoder_model->getDecoderStack();
        if (decoder_stack) {
            int row_index = 0;
            auto rows = decoder_stack->get_rows_lshow();

            for (auto i = rows.begin();i != rows.end(); i++) {
                if ((*i).second) {
                    QLabel *row_label = new QLabel((*i).first.title(), this);
                    QCheckBox *row_sel = new QCheckBox(this);
                    if (row_index == 0) {
                        row_sel->setChecked(true);
                    }
                    _row_label_list.push_back(row_label);
                    _row_sel_list.push_back(row_sel);
                    _flayout->addRow(row_label, row_sel);
                    row_sel->setProperty("stack", 0);
                    row_sel->setProperty("rowindex", row_index);
                    row_sel->setProperty("title", (*i).first.title());
                    row_index++;
                }
            }
        }
    }

    _layout = new QVBoxLayout();
    _layout->addLayout(_flayout);
    _layout->addWidget(&_button_box);

    layout()->addLayout(_layout);
    setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PROTOCOL_EXPORT), "Protocol Export"));

    connect(&_button_box, &QDialogButtonBox::accepted, this, &ProtocolExp::accept);
    connect(&_button_box, &QDialogButtonBox::rejected, this, &ProtocolExp::reject);
    connect(_session->device_event_object(), &DeviceEventObject::device_updated, this, &QDialog::reject);

}

void ProtocolExp::accept()
{   
    if (_session->have_decoded_result() == false)
    {
        QString errMsg = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_NO_DECODED_RESULT), "No data to export");
        MsgBox::Show(errMsg);
        return;
    }

    // Nothing exportable: tell the user instead of closing the dialog and
    // silently doing nothing (the old behaviour in "All" mode, where the row
    // list could not be built at all).
    if (_row_sel_list.empty()) {
        QString errMsg = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_NO_DECODED_RESULT),
                             "No data to export");
        MsgBox::Show(errMsg);
        return;
    }

    bool any_checked = false;
    for (std::list<QCheckBox *>::const_iterator i = _row_sel_list.begin();
         i != _row_sel_list.end(); i++)
    {
        if ((*i)->isChecked()) {
            any_checked = true;
            break;
        }
    }

    if (!any_checked) {
        QString errMsg = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_NO_EXPORT_ROW),
                             "Please select at least one decode row!");
        MsgBox::Show(errMsg);
        return;
    }

    QDialog::accept();

    QList<QString> supportedFormats;
    for (int i = _format_combobox->count() - 1; i >= 0; i--)
    {
        supportedFormats.push_back(_format_combobox->itemText(i));
    }

    QString filter;
    for (int i = 0; i < supportedFormats.count(); i++)
    {
        filter.append(supportedFormats[i]);
        if (i < supportedFormats.count() - 1)
            filter.append(";;");
    }

    AppConfig &app = AppConfig::Instance();
    QString default_filter = _format_combobox->currentText();
    QString default_name = app.userHistory.protocolExportPath + "/" + "decoder-";
    default_name += _session->get_session_time().toString("-yyMMdd-hhmmss");

    QString file_name = QFileDialog::getSaveFileName(
        this,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_EXPORT_DATA), "Export Data"),
        default_name, filter,
        &default_filter);

    if (file_name == ""){
        return;
    }

    QFileInfo f(file_name);
    QStringList list = default_filter.split('.').last().split(')');
    QString ext = list.first();
    if (f.suffix().compare(ext))
        //tr
        file_name += "." + ext;

    QString fname = path::GetDirectoryName(file_name);
    if (fname != app.userHistory.openDir)
    {
        app.userHistory.protocolExportPath = fname;
        app.SaveHistory();
    }
    _fileName = file_name;
 
    QFuture<void> future;
    future = QtConcurrent::run([&]{
                    save_proc();
               });

    Qt::WindowFlags flags = Qt::CustomizeWindowHint;
    QProgressDialog dlg(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_EXPORT_PROTOCOL_LIST_RESULT), 
                        "Export Protocol List Result... It can take a while."),
                        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CANCEL), "Cancel"), 0, 100, this, flags);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint |
                       Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint);

    QFutureWatcher<void> watcher;

    connect(&watcher, &QFutureWatcher<void>::finished, &dlg, &QProgressDialog::cancel);
    connect(this, &ProtocolExp::export_progress, &dlg, &QProgressDialog::setValue);
    connect(&dlg, &QProgressDialog::canceled, this, &ProtocolExp::cancel_export);

    watcher.setFuture(future);
    dlg.exec();

    future.waitForFinished();   
}

void ProtocolExp::save_proc()
{
    _export_cancel = false;

    QFile file(_fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }
    QTextStream out(&file);
    encoding::set_utf8(out);
    // out.setGenerateByteOrderMark(true); // UTF-8 without BOM
    int row_num = 0;
    ExportRowInfo row_inf_arr[EXPORT_DEC_ROW_COUNT_MAX];
    std::vector<const Annotation*> annotations_arr[EXPORT_DEC_ROW_COUNT_MAX];

    for (std::list<QCheckBox *>::const_iterator i = _row_sel_list.begin();
         i != _row_sel_list.end(); i++)
    {
        if ((*i)->isChecked())
        {
            row_inf_arr[row_num].title = (*i)->property("title").toString();
            row_inf_arr[row_num].row_index = (*i)->property("rowindex").toInt();
            row_inf_arr[row_num].stack_index = (*i)->property("stack").toInt();
            row_inf_arr[row_num].row = nullptr;
            row_inf_arr[row_num].stack = nullptr;
            row_num++;

            if (row_num == EXPORT_DEC_ROW_COUNT_MAX)
                break;
        }
    }

    if (row_num == 0){
        pxv_info("ERROR: There have no decode data row to export.");
        return;
    }

    // Resolve the DecoderStack behind every selected column. In "All" mode all
    // stacks of the document are exported together onto one timeline; in
    // single-stack mode there is exactly one.
    std::vector<pv::data::DecoderStack*> stacks;
    if (_multi_stack) {
        stacks = _export_stacks;
    } else {
        stacks.push_back(_decoder_model->getDecoderStack());
    }

    // One rows-map per stack. These must outlive the export below:
    // row_inf_arr[].row points at the Row keys stored inside them.
    std::vector<std::map<const Row, bool>> rows_maps(stacks.size());
    for (size_t si = 0; si < stacks.size(); si++) {
        if (stacks[si])
            rows_maps[si] = stacks[si]->get_rows_lshow();
    }

    // Map (stack index, visible row index) -> const Row*
    for (size_t si = 0; si < rows_maps.size(); si++) {
        int fd_row_dex = 0;
        for (auto it = rows_maps[si].begin(); it != rows_maps[si].end(); it++) {
            if (!(*it).second)
                continue;

            for (int i=0; i<row_num; i++) {
                if (row_inf_arr[i].stack_index == (int)si &&
                    row_inf_arr[i].row_index == fd_row_dex) {
                    row_inf_arr[i].row = &(*it).first;
                    row_inf_arr[i].stack = stacks[si];
                    break;
                }
            }
            fd_row_dex++;
        }
    }

    // Drop columns whose Row could not be resolved (stack removed, or the
    // decode rows changed while the dialog was open) — otherwise the subset
    // call below would dereference a null Row.
    int resolved_num = 0;
    for (int i=0; i<row_num; i++) {
        if (row_inf_arr[i].row == nullptr || row_inf_arr[i].stack == nullptr)
            continue;
        if (resolved_num != i)
            row_inf_arr[resolved_num] = row_inf_arr[i];
        resolved_num++;
    }
    row_num = resolved_num;

    if (row_num == 0){
        pxv_info("ERROR: There have no decode data row to export.");
        return;
    }

    //get annotation list
    uint64_t total_ann_count = 0;

    for (int i=0; i<row_num; i++)
    {
        const uint64_t end_sample = row_inf_arr[i].stack->sample_count();
        row_inf_arr[i].stack->get_annotation_subset(annotations_arr[i], *row_inf_arr[i].row,
                                         0, end_sample > 0 ? end_sample - 1 : 0);
        total_ann_count += (uint64_t)annotations_arr[i].size();
        sort(annotations_arr[i].begin(), annotations_arr[i].end(), compare_ann_index);  
        row_inf_arr[i].read_index = 0;
    }

    // Decoder header. In "All" mode emit one line per stack so several
    // instances of the same protocol (e.g. UART on TX and UART on RX) stay
    // identifiable in the exported file.
    if (_multi_stack) {
        std::vector<int> written_stacks;
        for (int i=0; i<row_num; i++) {
            const int si = row_inf_arr[i].stack_index;
            bool dup = false;
            for (size_t k = 0; k < written_stacks.size(); k++) {
                if (written_stacks[k] == si) {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;
            written_stacks.push_back(si);
            out << "# Decoder: "
                << pv::view::DecoderModel::stackDisplayName(row_inf_arr[i].stack, si)
                << "\n";
        }
    } else {
        // Derive decoder name + custom label for the export header so multiple
        // instances of the same decoder can be distinguished (e.g. "SPI(CH2.SPI)").
        pv::data::DecoderStack *decoder_stack = row_inf_arr[0].stack;
        QString decoder_name;
        auto &dec_list = decoder_stack->stack();
        if (!dec_list.empty()) {
            auto *root_dec = dec_list.front().get();
            if (root_dec && root_dec->decoder() && root_dec->decoder()->name)
                decoder_name = QString::fromUtf8(root_dec->decoder()->name);
        }
        QString custom_label = decoder_stack->label();
        if (custom_label.isEmpty())
            custom_label = decoder_stack->auto_label();
        if (!custom_label.isEmpty())
            decoder_name += "(" + custom_label + ")";
        if (!decoder_name.isEmpty())
            out << "# Decoder: " << decoder_name << "\n";
    }

    //title
    QString title_str;

    for (int i=0; i<row_num; i++) {
        if (i > 0 && i < row_num){
            title_str.append(",");
        }
        title_str.append(row_inf_arr[i].title);
    }

    out << QString("%1,%2,%3\n")
            .arg("Id")
            .arg("Time[ns]")
            .arg(title_str);

    uint64_t write_row_dex = 0;
    uint64_t write_ann_num = 0;
    // Every stack belongs to the same capture, so one sample rate converts
    // sample positions to nanoseconds for the whole file. Take the first
    // non-zero one (guards against a stack that has not started decoding).
    double sample_rate = 0;
    for (int i=0; i<row_num; i++) {
        if (row_inf_arr[i].stack->samplerate() > 0) {
            sample_rate = row_inf_arr[i].stack->samplerate();
            break;
        }
    }
    double ns_per_sample = sample_rate > 0 ? (SR_SEC(1) * 1.0 / sample_rate) : 0.0;
    uint64_t sample_index = 0;
    uint64_t sample_index1 = 0;

    while (write_ann_num < total_ann_count && !_export_cancel)
    {    
        bool bFirtColumn = true;

        for (int i=0; i<row_num; i++)
        {   
            if (row_inf_arr[i].read_index >= annotations_arr[i].size())
                continue;
            
const Annotation *ann = annotations_arr[i].at(row_inf_arr[i].read_index);
    sample_index1 = ann->start_sample();

            if (bFirtColumn || sample_index1 < sample_index){
                sample_index = sample_index1;
                bFirtColumn = false;
            }
        }

        QString ann_row_str;

        for (int i=0; i<row_num; i++)
        {   
            if (i > 0 && i < row_num){
                ann_row_str.append(",");
            }

            if (row_inf_arr[i].read_index >= annotations_arr[i].size())
                continue;
            
const Annotation *ann = annotations_arr[i].at(row_inf_arr[i].read_index);

    if (ann->start_sample() == sample_index){
                const auto &ann_texts = ann->annotations();
                // An annotation always carries at least one string, but a row
                // from a different stack may not — never index blindly.
                ann_row_str.append(ann_texts.empty() ? QString() : ann_texts.at(0));
                row_inf_arr[i].read_index++;
                write_ann_num++;
            }
        }

        write_row_dex++;

        out << QString("%1,%2,%3\n")
                       .arg(QString::number(write_row_dex))
                       .arg(QString::number(sample_index * ns_per_sample, 'f', 2))
                       .arg(ann_row_str);

        emit export_progress(write_ann_num * 100 / total_ann_count);
    }

    file.close();
}

bool ProtocolExp::compare_ann_index(const data::decode::Annotation *a,
                    const data::decode::Annotation *b)
{
    if (!a || !b) {
        pxv_warn("%s", "ProtocolExp::compare_ann_index: annotation pointer is nullptr");
        return false;
    }
    assert(a);
    assert(b);
    return a->start_sample() < b->start_sample();
}

void ProtocolExp::reject()
{
    using namespace Qt;

    QDialog::reject();
}

void ProtocolExp::cancel_export()
{
    _export_cancel = true;
}

} // namespace dialogs
} // namespace pv
