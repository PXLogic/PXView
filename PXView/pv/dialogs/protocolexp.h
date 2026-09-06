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


#ifndef PXVIEW_PV_PROTOCOLEXP_H
#define PXVIEW_PV_PROTOCOLEXP_H

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QCheckBox> 
#include <QString>
#include <vector>
 
#include "pv/prop/binding/deviceoptions.h"
#include "pv/toolbars/titlebar.h"
#include "pv/dialogs/pxdialog.h"
#include "pv/ui/dscombobox.h"

namespace pv {

class SigSession;
namespace data { class DataSource; class DecoderStack; }

namespace data {
namespace decode {
class Row;
class Annotation;
}
}

namespace view {
class DecoderModel;
}

namespace dialogs {

class ProtocolExp : public PxDialog
{
    Q_OBJECT

private:
    // One selectable export column. In multi-stack (All) mode a column is
    // identified by (stack_index, row_index) so that the same decode row name
    // coming from two different stacks (e.g. two UART instances bound to TX
    // and RX) stays distinguishable and both can be exported at once.
    struct ExportRowInfo
    {
        QString title;
        const data::decode::Row *row;
        int     row_index;    // 0-based visible decode row inside its stack
        int     stack_index;  // index into _export_stacks (0 in single-stack mode)
        data::DecoderStack *stack;
        uint64_t    read_index;
    };

public:
    ProtocolExp(QWidget *parent, SigSession *session, pv::view::DecoderModel *decoder_model);

protected:
    void accept();
    void reject();
    void save_proc();
    static bool compare_ann_index(const data::decode::Annotation *a,
                    const data::decode::Annotation *b);

signals:
    void export_progress(int percent);

private slots:
    void cancel_export();

private:
    SigSession *_session;
    data::DataSource *_data_src = nullptr;  // Spec v2 Task 9: route DataSource methods through interface
    // View-owned DecoderModel passed in from ProtocolDock (Task 10): the
    // dialog reads the current decoder stack from this instance to know
    // which protocol to export.
    pv::view::DecoderModel *_decoder_model;

    // Snapshot taken at construction time: true when the protocol dock was in
    // "All" (multi-stack) mode. Export then spans every stack, merged onto one
    // shared sample timeline.
    bool _multi_stack = false;
    std::vector<pv::data::DecoderStack *> _export_stacks;

    toolbars::TitleBar *_titlebar;
    DsComboBox *_format_combobox;
    std::list<QCheckBox *> _row_sel_list;
    std::list<QLabel *> _row_label_list;
    QFormLayout *_flayout;
    QVBoxLayout *_layout;
    QDialogButtonBox _button_box;

    bool _export_cancel;
    QString     _fileName; 
};

} // namespace dialogs
} // namespace pv

#endif // PXVIEW_PV_PROTOCOLEXP_H
