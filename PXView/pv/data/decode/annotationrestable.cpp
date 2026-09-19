/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 * 
 * Copyright (C) 2021 DreamSourceLab <support@dreamsourcelab.com>
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

#include "pv/data/decode/annotationrestable.h"
#include <cassert>
#include <cstdlib> 
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <QByteArray>
#include "pv/base/log.h"
#include "pv/base/pxvdef.h"
 
const char g_bin_cvt_table[] = "0000000100100011010001010110011110001001101010111100110111101111";
 
 char* bin2oct_string(char *buf, int size, const char *bin, int len){
	char *wr = buf + size - 1;
	*wr = 0; //end flag

	char *rd = const_cast<char*>(bin) + len - 1; //move to last byte
	char tmp[3]; 

	while (rd >= bin && wr > buf)
	{  
		wr--;
		int num = 0;

		while (rd >= bin && num < 3)
		{
			tmp[2-num] = *rd;			 
			rd--;
			num++;
		}

		//fill
		while (num < 3)
		{
			tmp[2-num] = '0';
			++num;
		}		

	    if (strncmp(tmp, "000", 3) == 0)
			*wr = '0';
		else if (strncmp(tmp, "001", 3) == 0)
			*wr = '1';
		else if (strncmp(tmp, "010", 3) == 0)
			*wr = '2';
		else if (strncmp(tmp, "011", 3) == 0)
			*wr = '3';
		else if (strncmp(tmp, "100", 3) == 0)
			*wr = '4';
		else if (strncmp(tmp, "101", 3) == 0)
			*wr = '5';
		else if (strncmp(tmp, "110", 3) == 0)
			*wr = '6';
		else if (strncmp(tmp, "111", 3) == 0)
			*wr = '7';
	} 

	return wr;
}

long long bin2long_string(const char *bin, int len)
{
	char *rd = const_cast<char*>(bin) + len - 1; //move to last byte
	int dex = 0;
	long long value = 0;
	long long bv = 0;

    while (rd >= bin)
	{
		if (*rd == '1')
		{
			bv = 1 << dex;
			value += bv;
		}
		rd--;
		++dex;
	}

	return value;
}

// ---------------------------------------------------------------------------
// 数值格式转换（Plan D：scratch buffer 全部改为调用方提供的局部缓冲）
//
// 旧实现把 bin/oct/number 三个临时缓冲做成 AnnotationResTable 的成员，并且
// format_numberic 直接返回指向这些成员的指针 —— 两个渲染线程（多视图/多 tab
// 或 MCP 导出线程）并发调用时会互相覆盖字符串。现在缓冲由调用方在栈上提供，
// 返回值指向调用方的缓冲，生命周期由调用方控制。
// ---------------------------------------------------------------------------

namespace {

const char *format_to_string(const char *hex_str, int fmt,
                             char *bin_buf, size_t bin_buf_len,
                             char *oct_buf, size_t oct_buf_len,
                             char *num_buf, size_t num_buf_len)
{
	//flow, convert to oct\dec\bin format
	const char *data = hex_str;
	if (data[0] == 0 || fmt == DecoderDataFormat::hex){
		return data;
	}

	//convert to bin format
	char *buf = bin_buf + bin_buf_len - 2;
	buf[1] = 0; //set the end flag
	buf[0] = 0;

	int len = static_cast<int>(strlen(data));
	 //buffer is not enough
	if (len > DECODER_MAX_DATA_BLOCK_LEN){
		return data;
	}

	char *rd = const_cast<char*>(data) + len - 1; //move to last byte
	char c = 0;
	int dex = 0;

	while (rd >= data)
	{
		c = *rd;

		if (c >= '0' && c <= '9'){
			dex = static_cast<int>((c - '0'));	 
		}
		else if (c >= 'A' && c <= 'F'){
			dex = static_cast<int>((c - 'A')) + 10;
		}
		else if (c >= 'a' && c <= 'f'){
			dex = static_cast<int>((c - 'a')) + 10;
		}
		else{
			pxv_err("is not a hex string");
			rd--;
			continue;
		}

		char *ptable = const_cast<char*>(g_bin_cvt_table) + dex * 4;

		if (buf < bin_buf + 4){ //out of buffer
			break;
		}
		buf -= 4; //move to left for 4 bytes
		buf[0] = ptable[0];
		buf[1] = ptable[1];
		buf[2] = ptable[2];
		buf[3] = ptable[3];
	
		rd--;
	} 

	//get bin format 
	if (fmt == DecoderDataFormat::bin){
		return buf;
	}

	//get oct format
	if (fmt == DecoderDataFormat::oct){
		return bin2oct_string(oct_buf, static_cast<int>(oct_buf_len), buf, len * 4);
	}

	//64 bit integer
	if (fmt == DecoderDataFormat::dec && len * 4 <= 64){
		long long lv = bin2long_string(buf, len * 4);
		num_buf[0] = 0;
		snprintf(num_buf, num_buf_len, "%lld", lv);
		return num_buf;
	}
	
	//ascii
	if (fmt == DecoderDataFormat::ascii && len < 30 - 3){
		if (len == 2){
			int lv = static_cast<int>(bin2long_string(buf, len * 4));
			//can display chars
			if (lv >= 33 && lv <= 126){
				snprintf(num_buf, num_buf_len, "%c", static_cast<char>(lv));
				return num_buf;
			}
		}
		// "[hex]"：num_buf 至少 30 字节，len <= 26 时 len+3 <= 29 不会越界。
		num_buf[0] = '[';
		memcpy(num_buf + 1, data, static_cast<size_t>(len));
		num_buf[len + 1] = ']';
		num_buf[len + 2] = 0;
		return num_buf;
	}

	return data;
}

// hex 串 -> 目标进制显示串。含分隔字母（如 "12-34"）时逐段转换后拼接，
// 与旧实现语义完全一致，只是所有缓冲改为局部变量。
QString format_numeric_string(const QString &hex, int fmt)
{
	const QByteArray hex_utf8 = hex.toUtf8();
	const char *hex_str = hex_utf8.constData();

	if (hex_str[0] == 0 || fmt == DecoderDataFormat::hex){
		return hex;
	}

	//check if have split letter
	bool bMutil = false;
	for (const char *rd = hex_str; *rd; rd++){
		const char c = *rd;
		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
		      (c >= 'a' && c <= 'f'))){
			bMutil = true;
			break;
		}
	}

	char bin_buf[DECODER_MAX_DATA_BLOCK_LEN * 4 + 2];
	char oct_buf[DECODER_MAX_DATA_BLOCK_LEN * 3 + 2];
	char num_buf[30];

	if (!bMutil){
		return QString::fromUtf8(format_to_string(hex_str, fmt,
		                                          bin_buf, sizeof(bin_buf),
		                                          oct_buf, sizeof(oct_buf),
		                                          num_buf, sizeof(num_buf)));
	}

	//convert each sub string 
	char sub_buf[DECODER_MAX_DATA_BLOCK_LEN + 1];
	char *sub_wr = sub_buf;
	char *sub_end = sub_wr + DECODER_MAX_DATA_BLOCK_LEN;
	char all_buf[CONVERT_STR_MAX_LEN + 1];
	char *all_wr = all_buf;

	for (const char *rd = hex_str; *rd; rd++)
	{
		const char c = *rd;

		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
		    (c >= 'a' && c <= 'f')){
			if (sub_wr == sub_end){
				printf("conver error,sub string length is too long!\n");
				return hex;
			}

			*sub_wr = c; //make sub string
			sub_wr++;
			continue;
		}

		//convert sub string
		if (sub_wr != sub_buf){
			*sub_wr = 0;
			const char *sub_str = format_to_string(sub_buf, fmt,
			                                       bin_buf, sizeof(bin_buf),
			                                       oct_buf, sizeof(oct_buf),
			                                       num_buf, sizeof(num_buf));
			unsigned int sublen = static_cast<unsigned int>(strlen(sub_str));

			if ((all_wr - all_buf) + sublen > CONVERT_STR_MAX_LEN){
				printf("convert error,write buffer is full!\n");
				return hex;
			}

			memcpy(all_wr, sub_str, sublen);
			all_wr += sublen;
			sub_wr = sub_buf; //reset write buffer
		}

		//the split letter
		if ((all_wr - all_buf) + 1 > CONVERT_STR_MAX_LEN){
			printf("convert error,write buffer is full!\n");
			return hex;
		}

		*all_wr = c;
		all_wr++;
	}

	//convert the last sub string
	if (sub_wr != sub_buf)
	{
		*sub_wr = 0;
		const char *sub_str = format_to_string(sub_buf, fmt,
		                                       bin_buf, sizeof(bin_buf),
		                                       oct_buf, sizeof(oct_buf),
		                                       num_buf, sizeof(num_buf));
		unsigned int sublen = static_cast<unsigned int>(strlen(sub_str));

		if ((all_wr - all_buf) + sublen > CONVERT_STR_MAX_LEN){
			printf("convert error,write buffer is full!\n");
			return hex;
		}

		memcpy(all_wr, sub_str, sublen);
		all_wr[sublen] = '\0';
		all_wr += sublen;
	}

	*all_wr = 0;

	return QString::fromUtf8(all_buf);
}

// 按 format 派生一行行显示文本：原文里的 "{$}" 用转换后的数值替换；
// 原文为空（只有数值）时输出单行数值。与旧 annotations() 的规则一致。
std::vector<QString> build_variant_lines(const std::vector<QString> &src,
                                         const QString &hex, int fmt)
{
	std::vector<QString> out;
	const QString num = format_numeric_string(hex, fmt);

	if (!src.empty()){
		out.reserve(src.size());
		for (const QString &line : src){
			QString t = line;
			t.replace(QStringLiteral("{$}"), num);
			out.push_back(std::move(t));
		}
	}
	else{
		out.push_back(num);
	}

	return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// AnnotationText：不可变文本 + 按 format 的惰性派生缓存
// ---------------------------------------------------------------------------

AnnotationText::AnnotationText(std::vector<QString> src_lines, QString hex,
                               bool is_numeric)
    : _src_lines(std::move(src_lines)),
      _hex(std::move(hex)),
      _is_numeric(is_numeric),
      // 只有数值注解才需要格式派生缓存，非数值注解不为此付费。
      _slots(is_numeric ? std::make_unique<CvtSlots>() : nullptr) {
}

AnnotationText::~AnnotationText() = default;

const std::vector<QString> &AnnotationText::lines(int fmt) const {
	// 非数值注解没有格式派生，直接返回原文（零成本、无锁）。
	if (!_is_numeric || !_slots)
		return _src_lines;
	if (fmt < 0 || fmt >= kAnnotationFormatSlotCount)
		return _src_lines;

	pv::atomic_shared_ptr<const std::vector<QString>> &slot = _slots->slot[fmt];
	std::shared_ptr<const std::vector<QString>> cur =
	    slot.load(std::memory_order_acquire);
	if (!cur) {
		// 槽位写入一次后永不改写（双重检查 + 锁），所以已返回的引用在本对象
		// 存活期内始终有效；多个线程首次并发读取同一 format 时也只会有一个
		// 结果被发布。
		std::lock_guard<std::mutex> lock(_slots->mutex);
		cur = slot.load(std::memory_order_relaxed);
		if (!cur) {
			cur = std::make_shared<const std::vector<QString>>(
			    build_variant_lines(_src_lines, _hex, fmt));
			slot.store(cur, std::memory_order_release);
		}
	}
	return *cur;
}

// ---------------------------------------------------------------------------
// AnnotationResTable：解码线程私有的去重表（只存 weak_ptr，不拥有文本）
// ---------------------------------------------------------------------------

std::shared_ptr<const AnnotationText>
AnnotationResTable::intern(const std::string &key,
                           std::vector<QString> src_lines, QString hex,
                           bool is_numeric) {
	auto it = m_indexs.find(key);
	if (it != m_indexs.end()) {
		// 命中且文本仍存活 -> 复用同一个不可变对象（去重的意义所在）。
		if (std::shared_ptr<const AnnotationText> alive = it->second.lock())
			return alive;
	}

	auto text = std::make_shared<const AnnotationText>(
	    std::move(src_lines), std::move(hex), is_numeric);
	// weak_ptr：本表不持有所有权，文本一旦无人引用即自然过期。
	m_indexs[key] = text;
	return text;
}

void AnnotationResTable::reset() {
	// 只清表、不释放文本：已发布快照里的注解仍持有 shared_ptr，它们的生命期
	// 由引用计数决定。旧实现在这里 delete 条目，而渲染线程仍按 index 取用，
	// 解码重启（DecoderStatus::clear()）时会读到已释放的内存。
	m_indexs.clear();
}

int AnnotationResTable::hexToDecimal(char * hex)
{
	if (!hex) {
		pxv_warn("%s", "AnnotationResTable::hexToDecimal: hex is nullptr");
		return 0;
	}
	assert(hex);
    int len = static_cast<int>(strlen(hex));

    double b = 16;
    int result = 0;
    char *p = hex;

    while(*p) {
        if(*p >= '0' && *p <= '9')
            result += static_cast<int>(pow(b, --len)) * (*p - '0');
        else if(*p >= 'a' && *p <= 'f')
            result += static_cast<int>(pow(b, --len)) * (*p - 'a' + 10);
        else if(*p >= 'A' && *p <= 'F')
            result += static_cast<int>(pow(b, --len)) * (*p - 'A' + 10);

        p++;
    }

    return result;
}

void AnnotationResTable::decimalToBinString(unsigned long long num, int bitSize, char *buffer, int buffer_size)
{
    (void)buffer_size;

	if (!buffer) {
		pxv_warn("%s", "AnnotationResTable::decimalToBinString: buffer is nullptr");
		return;
	}
	assert(buffer);
	assert(buffer_size);
	 
	if (bitSize < 8)
		bitSize = 8;
	if (bitSize > 64)
		bitSize = 64;

	assert(bitSize < buffer_size);

	int v;
	char *wr = buffer + bitSize;
	*wr = 0;
	wr--;

	while (num > 0 && wr >= buffer)
	{
		v = num % 2;
		*wr = v ? '1' : '0';
		wr--;
		num = num / 2;
	}

	while (wr >= buffer)
	{
		*wr = '0';
		wr--;
	}
}
 
