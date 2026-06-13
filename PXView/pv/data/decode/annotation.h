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

#ifndef PXVIEW_PV_VIEW_DECODE_ANNOTATION_H
#define PXVIEW_PV_VIEW_DECODE_ANNOTATION_H

#include <stdint.h>

#include <QString>
#include <vector>
#include <QFont>
#include "annotation_pool.h"

#include <QFontMetrics>

class AnnotationResTable;
class DecoderStatus;

struct srd_proto_data;

namespace pv {
namespace data {
namespace decode {

//create at DecoderStack.annotation_callback
class Annotation
{
public:
	Annotation(const srd_proto_data *const pdata, DecoderStatus *status);
    Annotation();
	~Annotation();

	// Pool-based allocation to prevent heap fragmentation
	static void* operator new(size_t size) {
		return AnnotationPool::instance().allocate(size);
	}
	static void operator delete(void* p, size_t) {
		AnnotationPool::instance().deallocate(p);
	}
	static void operator delete(void* p) {
		AnnotationPool::instance().deallocate(p);
	}

public:
	inline uint64_t start_sample() const{
		return _start_sample;
	}

	inline uint64_t end_sample() const{
		return _end_sample;
	}

	inline int format() const{
		return _format;
	}

    inline int type() const{
		return _type;
	}  

	bool is_numberic();

	const std::vector<QString>& annotations() const;

	QString get_cached_best_annotation(double rect_width, const QFont &font, const QFontMetrics &fm) const;

private:
	uint64_t 		_start_sample;
	uint64_t 		_end_sample;
	short 			_format;
	short 			_type;
	int 			_resIndex;
	DecoderStatus 	*_status; /*a global variable*/

	mutable QString     _cached_best_annotation;
	mutable double      _cached_rect_width;
	mutable QFont       _cached_width_font;
};

} // namespace decode
} // namespace data
} // namespace pv

#endif // PXVIEW_PV_VIEW_DECODE_ANNOTATION_H
