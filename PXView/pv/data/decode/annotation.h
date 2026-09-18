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

#include <cstdint>
#include <memory>

#include <QString>
#include <vector>

// 与 AnnotationResTable / DecoderStatus 一致：全局作用域声明（见
// annotationrestable.h 顶部的说明）。
class AnnotationText;

class AnnotationResTable;
class DecoderStatus;

struct srd_proto_data;

namespace pv {
namespace data {
namespace decode {

// P2-7 fix: Annotation is now a value type. It is stored directly in
// RowData's deque<Annotation> — no new/delete, no custom memory pool.
// The operator new/delete overrides and AnnotationPool have been removed.
//
// 所有权拆分（Plan D）：Annotation 不再持有 DecoderStatus* + _resIndex。
// 那两个字段把"文本内容"和"解码侧可变状态"绑在一起，导致渲染线程要透过
// 解码器的 status 去解引用一个会被 reset() 释放的条目。现在 Annotation 只
// 持 shared_ptr<const AnnotationText>：文本自包含、不可变、引用计数保活，
// 解码重启清表也不影响已发布快照。DecoderStatus* 只在构造时用于去重
// （intern）与数值标记，不再被存下来。
class Annotation
{
public:
	Annotation(const srd_proto_data *const pdata, DecoderStatus *status);
    Annotation();
	~Annotation();

    // Copy and move semantics (needed for deque storage and get_annotation copy)
    Annotation(const Annotation&) = default;
    Annotation& operator=(const Annotation&) = default;
    Annotation(Annotation&&) = default;
    Annotation& operator=(Annotation&&) = default;

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

	bool is_numberic() const;

	// 显示格式是**视图参数**（bin/oct/dec/hex/ascii），由调用方传入，不再
	// 从解码器的可变 status 里读，也不再回写进文本对象。
	// 返回的引用在本次 Annotation 存活期内有效。
	const std::vector<QString>& annotations(int fmt) const;

private:
	uint64_t 		_start_sample;
	uint64_t 		_end_sample;
	short 			_format;
	short 			_type;
	// 不可变文本句柄：取代旧的 _resIndex + DecoderStatus*。
	// 为空表示默认构造的注解（无文本）。
	std::shared_ptr<const AnnotationText> _text;
};

} // namespace decode
} // namespace data
} // namespace pv

#endif // PXVIEW_PV_VIEW_DECODE_ANNOTATION_H
