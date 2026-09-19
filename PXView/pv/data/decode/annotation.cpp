/*
 * This file is part of the PulseView project.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include <libsigrokdecode.h>

#include <vector>
#include <stdexcept>

#include "pv/base/log.h"
#include "pv/data/decode/annotation.h"
#include "pv/data/decode/annotationrestable.h"
#include <cstring>
#include <cassert>
#include <cstring>
#include <cstdlib>

#include "pv/config/appconfig.h"
#include "pv/data/decode/decoderstatus.h"
#include "pv/base/pxvdef.h"
 

namespace pv {
namespace data {
namespace decode {
 
Annotation::Annotation(const srd_proto_data *const pdata, DecoderStatus *status)
{
	if (!pdata) {
		throw std::invalid_argument("Annotation: pdata is nullptr");
	}
	assert(pdata);
	const srd_proto_data_annotation *const pda =
		(const srd_proto_data_annotation*)pdata->data;
	if (!pda) {
		throw std::invalid_argument("Annotation: pda is nullptr");
	}
	assert(pda);
	if (!status) {
		throw std::invalid_argument("Annotation: status is nullptr");
	}
	assert(status);

	_start_sample =	pdata->start_sample;
	_end_sample	  =	pdata->end_sample;
	_format 	= static_cast<short>(pda->ann_class);
    _type 		= static_cast<short>(pda->ann_type);
 
	// 收集原文与数值 hex（规则与旧实现一致），交给去重表换回一个不可变的
	// AnnotationText。这里只借用 status 完成去重与数值标记，不保存它。
	std::vector<QString> src_lines;
	std::string key;

    char **annotations = pda->ann_text;
    while(annotations && *annotations) {
		if ((*annotations)[0] != '\n'){
			key.append(*annotations, strlen(*annotations));
			src_lines.push_back(QString::fromUtf8(*annotations));
		}		
		annotations++;  
	}

	QString hex;
	bool is_numeric = false;

	if (pda->str_number_hex[0]){
		//append numeric string
		key.append(pda->str_number_hex, strlen(pda->str_number_hex));

		//get numeric data
		const int str_len = (int)strlen(pda->str_number_hex);
		if (str_len <= DECODER_MAX_DATA_BLOCK_LEN){
			hex = QString::fromUtf8(pda->str_number_hex, str_len);
			is_numeric = true;
		}
	}

	_text = status->m_resTable.intern(key, std::move(src_lines), hex, is_numeric);

	// 数值标记只增不减，语义与旧的 "|= is_numeric" 等价：条目存在即代表
	// 本解码轮次已经出现过数值，而 clear() 会同时清表与标记。
	if (is_numeric)
		status->m_bNumeric.store(true, std::memory_order_relaxed);
}

Annotation::Annotation()
{
    _start_sample = 0;
    _end_sample = 0;
	_format = 0;
	_type = 0;
}
 
Annotation::~Annotation()
{
}
  
const std::vector<QString>& Annotation::annotations(int fmt) const
{
	// 默认构造（无文本）的注解返回空表。旧实现此时会解引用未初始化的
	// _status 指针 —— 这也是把所有权切开后顺带修掉的一处 UB。
	static const std::vector<QString> empty_vec;
	if (!_text)
		return empty_vec;

	// 文本不可变，格式派生与缓存全部在 AnnotationText 内部完成；
	// 这里不再读写任何共享可变状态。
	return _text->lines(fmt);
}

bool Annotation::is_numberic() const
{
	return _text && _text->is_numeric();
}

} // namespace decode
} // namespace data
} // namespace pv
