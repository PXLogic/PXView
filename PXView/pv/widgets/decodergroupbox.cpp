/*
 * This file is part of the PulseView project.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2013 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include "pv/widgets/decodergroupbox.h"
#include "pv/data/decode/decoder.h"
#include "pv/data/decode/row.h"
#include "pv/data/stack/decoderstack.h"
#include <libsigrokdecode.h>


#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVariant>

#include "pv/config/appconfig.h"

#include "pv/ui/iconcache.h"
#include <cassert>


#include <cstring>


namespace pv {
namespace widgets {

DecoderGroupBox::DecoderGroupBox(data::DecoderStack *decoder_stack,
                                 data::decode::Decoder *dec,
                                 QLayout *dec_layout, QWidget *parent,
                                 QFont font)
    : QWidget(parent) {
  _row_num = 0;
  _content_width = 0;

  _dec = dec;
  _decoder_stack = decoder_stack;
  _widget = new QWidget(this);
  _layout = new QGridLayout(_widget);
  _layout->setContentsMargins(0, 0, 0, 0);
  _layout->setVerticalSpacing(2);

  QString iconPath = GetIconPath();
  _layout->addWidget(new QLabel(QString("<h3 style='font-style:italic'>%1</h3>")
                                    .arg(_dec->decoder()->name),
                                _widget),
                     0, 0);
  _layout->setColumnStretch(0, 1);

  const srd_decoder *const d = _dec->decoder();
  assert(d);
  (void)d;

  _index = 0;
  for (auto &up : _decoder_stack->stack()) {
    auto dec = up.get();
    if (dec == _dec)
      break;
    _index++;
  }
  _show_button = new QPushButton(
      IconCache::Instance().icon(_dec->shown() ? iconPath + "/shown.svg"
                                               : iconPath + "/hidden.svg"),
      QString(), _widget);
  _show_button->setProperty("index", -1);

  connect(_show_button, &QAbstractButton::clicked, this,
          &DecoderGroupBox::tog_icon);

  _layout->addWidget(_show_button, 0, 2);

  _row_num++;

  // add row show/hide
  int index = 0;
  auto rows = _decoder_stack->get_rows_gshow();

  for (auto i = rows.begin(); i != rows.end(); i++) {
    if ((*i).first.decoder() == _dec->decoder()) {
      QPushButton *show_button = new QPushButton(
          IconCache::Instance().icon((*i).second ? iconPath + "/shown.svg"
                                                 : iconPath + "/hidden.svg"),
          QString(), _widget);
      show_button->setProperty("index", index);
      connect(show_button, &QAbstractButton::clicked, this,
              &DecoderGroupBox::tog_icon);

      _row_show_button.push_back(show_button);
      QLabel *lb = new QLabel((*i).first.title(), _widget);
      lb->setFont(font);
      _layout->addWidget(lb, _row_show_button.size(), 0);
      _layout->addWidget(show_button, _row_show_button.size(), 2);
    }
    index++;

    _row_num++;
  }

  if (is_tdm_fast())
    refresh_tdm_fast_row_eyes();

  _layout->addLayout(dec_layout, _row_show_button.size() + 1, 0, 1, 3);
  _widget->setLayout(_layout);

  _content_width = _widget->sizeHint().width();

  parent->layout()->addWidget(_widget);
}

DecoderGroupBox::~DecoderGroupBox() {}

bool DecoderGroupBox::eventFilter(QObject *o, QEvent *e) {
  (void)e;
  (void)o;
  return false;
}

bool DecoderGroupBox::is_tdm_fast() const {
  return _dec && _dec->decoder() && _dec->decoder()->id &&
         std::strcmp(_dec->decoder()->id, "tdm_audio_fast") == 0;
}

void DecoderGroupBox::sync_tdm_fast_output() {
  if (!is_tdm_fast())
    return;

  bool any_row_enabled = false;
  const auto rows = _decoder_stack->get_rows_gshow();
  for (const auto &entry : rows) {
    if (entry.first.decoder() == _dec->decoder() && entry.second) {
      any_row_enabled = true;
      break;
    }
  }

  // Master eye is a gate only. Child row states are never overwritten.
  // Text generation is enabled only when master AND at least one child are on.
  const bool emit_text = _dec->shown() && any_row_enabled;
  _dec->set_option("output", g_variant_new_string(emit_text ? "both" : "waveform"));
}

void DecoderGroupBox::refresh_tdm_fast_row_eyes() {
  if (!is_tdm_fast())
    return;

  const QString iconPath = GetIconPath();
  const bool master = _dec->shown();
  const auto rows = _decoder_stack->get_rows_gshow();
  auto button = _row_show_button.begin();
  for (const auto &entry : rows) {
    if (entry.first.decoder() != _dec->decoder())
      continue;
    if (button == _row_show_button.end())
      break;
    QPushButton *btn = *button++;
    const bool effective_visible = master && entry.second;
    btn->setEnabled(master);
    btn->setIcon(IconCache::Instance().icon(
        effective_visible ? iconPath + "/shown.svg" : iconPath + "/hidden.svg"));
  }
}

void DecoderGroupBox::tog_icon() {
  QString iconPath = GetIconPath();
  QPushButton *sc = dynamic_cast<QPushButton *>(sender());
  int index = sc->property("index").toInt();

  if (index == -1) {
    int i = _index;

for (auto &up : _decoder_stack->stack()) {
    auto dec = up.get();
    if (i-- == 0) {
      dec->show(!dec->shown());
        sc->setIcon(IconCache::Instance().icon(
            dec->shown() ? iconPath + "/shown.svg" : iconPath + "/hidden.svg"));
        if (is_tdm_fast()) {
          // V16.4: master annotation eye gates the child rows but does not
          // destroy their saved per-channel selection.
          sync_tdm_fast_output();
          refresh_tdm_fast_row_eyes();
        }
        break;
      }
    }
  } else {
    auto rows = _decoder_stack->get_rows_gshow();

    for (auto i = rows.begin(); i != rows.end(); i++) {
      if (index-- == 0) {
        const bool new_show = !(*i).second;
        _decoder_stack->set_rows_gshow((*i).first, new_show);

        if (is_tdm_fast()) {
          // Per-channel eye changes only this row. The master eye remains a
          // pure gate and the other CH states are untouched.
          sync_tdm_fast_output();
          refresh_tdm_fast_row_eyes();
        } else {
          sc->setIcon(IconCache::Instance().icon(
              new_show ? iconPath + "/shown.svg" : iconPath + "/hidden.svg"));
        }
        break;
      }
    }
  }
}

void DecoderGroupBox::on_del_stack() {
  int i = _index;
  for (auto &up : _decoder_stack->stack()) {
    auto dec = up.get();
    if (i-- == 0) {
      del_stack(dec);
      break;
    }
  }
}

} // namespace widgets
} // namespace pv
