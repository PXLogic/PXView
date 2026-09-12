/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2014 DreamSourceLab <support@dreamsourcelab.com>
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

// DecodeTrace::create_popup — GUI translation unit (QML migration Task 3.2).
//
// create_popup was split out of decodetrace.cpp so that the decode *paint*
// pipeline (the rest of the class) compiles widget-free into pxview-render
// and can be linked by the QML shell (PXViewQml). The decoder options dialog
// (QDialog anchoring + DecoderOptionsDlg + MsgBox) stays in the GUI archive
// — same split pattern as the DEBT LEDGER entry this replaces in
// CMake/render_sources.cmake.

#include "pv/view/trace/decodetrace.h"

#include <QDialog>
#include <QWidget>

#include "pv/data/stack/decoderstack.h"
#include "pv/data/decode/decoder.h"
#include "pv/dialogs/decoderoptionsdlg.h"
#include "pv/ui/msgbox.h"
#include "pv/base/log.h"
#include "pv/core/langresource.h"

namespace pv {
namespace view {

// to show decoder's property setting dialog
bool DecodeTrace::create_popup(bool isnew, QPoint anchor) {
  (void)isnew;

  int ret = false; // setting have changed flag
  bool bOpenDlg = true;

  pxv_info("DecodeTrace: enter create_popup");
  while (bOpenDlg) {
    bOpenDlg = false;
    // Task 3.1: _view is now the widget-free IRenderView interface; recover
    // the concrete widget identity via qt_object() for dialog anchoring
    // (same top-level window the former _view->window() resolved to).
    QWidget *view_w = _view ? qobject_cast<QWidget *>(_view->qt_object())
                            : nullptr;
    QWidget *top = view_w ? view_w->window() : nullptr;
    pxv_info("DecodeTrace: GetTopWindow returned %p", top);
    dialogs::DecoderOptionsDlg dlg(top);
    dlg.set_cursor_range(_decode_cursor1, _decode_cursor2);
    dlg.load_options(this);

    // 锚点定位(与毛刺滤波浮窗相同的弹出逻辑):若调用方提供了有效锚点,
    // 在 exec() 前移动对话框,避免 QDialog 默认居中。
    if (!anchor.isNull())
      dlg.move(anchor);

    pxv_info("DecodeTrace: before dlg.exec()");
    int dlg_ret = dlg.exec();
    pxv_info("DecodeTrace: after dlg.exec(), ret=%d (Accepted=%d)", dlg_ret,
             QDialog::Accepted);

    if (QDialog::Accepted == dlg_ret) {
      dlg.apply_setting();

  for (auto &up : _decoder_stack->stack()) {
    auto dec = up.get();
    if (dec->commit() || _decoder_stack->options_changed()) {
          _decoder_stack->set_options_changed(true);
          ret = true;
        }
      }

      dlg.get_cursor_range(_decode_cursor1, _decode_cursor2);

      // Reopen the dialog to select the required probes.
      if (ret && _decoder_stack->check_required_probes() == false) {
        QString errMsg =
            L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_WORK_ERROR),
                "One or more required channels have not been specified");
        MsgBox::Show(errMsg);

        ret = false;
        bOpenDlg = true;
      }
    }

    if (dlg.is_reload_form()) {
      ret = false;
      bOpenDlg = true;
    }
  }

  pxv_info("DecodeTrace: exit create_popup, returning %d", ret);
  return ret;
}

} // namespace view
} // namespace pv
