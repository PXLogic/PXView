/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2013 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include "pv/view/trace/selectableitem.h"

namespace pv {
namespace view {

const int SelectableItem::HighlightRadius = 6;

// Highlight pen provider installed by the GUI shell (QApplication palette
// mapping). Null until installed — the fallback pen is used then.
static SelectableItem::HighlightPenProvider s_highlight_pen_provider = nullptr;

void SelectableItem::set_highlight_pen_provider(HighlightPenProvider provider)
{
	s_highlight_pen_provider = provider;
}

SelectableItem::SelectableItem() :
    _selected(false)
{
}

bool SelectableItem::selected()
{
	return _selected;
}

void SelectableItem::select(bool select)
{
	_selected = select;
}

QPen SelectableItem::highlight_pen()
{
	if (s_highlight_pen_provider)
		return s_highlight_pen_provider(HighlightRadius);
	// Widget-free fallback (used when no GUI shell installed a provider).
	return QPen(QColor(60, 140, 230), HighlightRadius,
		Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

} // namespace view
} // namespace pv
