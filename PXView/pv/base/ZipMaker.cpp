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

#include "pv/base/ZipMaker.h" 

#include <cassert>
#include <ctime>
#include <fstream>
#include <new>
#include <string>
#include <vector>

// The single-entry size cap both minizip APIs share: zipWriteInFileInZip()
// and unzReadCurrentFile() take an unsigned int length, so an entry can never
// be larger than 4 GiB no matter which side asks. Expressed once so both sides
// agree on what "too large" means.
namespace {
constexpr size_t kMaxZipEntryBytes = 0xFFFFFFFFull;
}

ZipMaker::ZipMaker() :
    m_zDoc(nullptr)
{
    m_opt_compress_level = Z_BEST_SPEED;
    m_zi = nullptr;
}

ZipMaker::~ZipMaker()
{
    Release();
}

bool ZipMaker::CreateNew(const char *fileName, bool bAppend)
{
     if (!fileName)
         return false;
     assert(fileName);

     Release();
 
     m_zDoc = zipOpen64(fileName, bAppend); 
     if (m_zDoc == nullptr) {
        m_error = "zipOpen64 error";
    } 

//make zip inner file time 
    m_zi = std::make_unique<zip_fileinfo>();

    time_t rawtime;
    time (&rawtime);
    struct tm *tinf= localtime(&rawtime);

    struct tm &ti = *tinf;
    zip_fileinfo &zi = *m_zi;

    zi.tmz_date.tm_year = ti.tm_year;
    zi.tmz_date.tm_mon  = ti.tm_mon;
    zi.tmz_date.tm_mday = ti.tm_mday;
    zi.tmz_date.tm_hour = ti.tm_hour;
    zi.tmz_date.tm_min  = ti.tm_min;
    zi.tmz_date.tm_sec  = ti.tm_sec;
    zi.dosDate = 0;
      
    return m_zDoc != nullptr;
}

void ZipMaker::Release()
{  
    if (m_zDoc){
       zipClose((zipFile)m_zDoc, nullptr);
       m_zDoc = nullptr;       
   }
   m_zi.reset();
}

bool ZipMaker::Close(){
    if (m_zDoc){
       zipClose((zipFile)m_zDoc, nullptr);
       m_zDoc = nullptr;
       return true;
   }
   return false;     
}

bool ZipMaker::AddFromBuffer(const char *innerFile, const char *buffer, size_t buferSize)
{
    if (!buffer || !innerFile || !m_zDoc)
        return false;
    assert(buffer);
    assert(innerFile);
    assert(m_zDoc);

    if (buferSize > kMaxZipEntryBytes) {
        m_error = "entry exceeds 4GiB zip limit";
        return false;
    }

    int level = m_opt_compress_level;

    if (level < Z_DEFAULT_COMPRESSION  || level > Z_BEST_COMPRESSION){
        level = Z_DEFAULT_COMPRESSION;
    }

    zipOpenNewFileInZip((zipFile)m_zDoc,innerFile,m_zi.get(),
                                nullptr,0,nullptr,0,nullptr ,
                                Z_DEFLATED,
                                level);

    zipWriteInFileInZip((zipFile)m_zDoc, buffer, static_cast<unsigned int>(buferSize));

    zipCloseFileInZip((zipFile)m_zDoc);

    return true;
}

bool ZipMaker::AddFromFile(const char *localFile, const char *innerFile)
{
    if (!localFile)
        return false;
    assert(localFile);

    std::ifstream in(localFile, std::ios::binary);
    if (!in) {
        m_error = "open file error";
        return false;
    }

    // Size the buffer from the stream itself: seekg(end) + tellg() replaces the
    // old fstat() call and works with no POSIX header involved.
    in.seekg(0, std::ios::end);
    const std::streamoff end_pos = in.tellg();
    if (!in || end_pos < 0) {
        m_error = "seek file error";
        return false;
    }

    const size_t size = static_cast<size_t>(end_pos);
    if (size > kMaxZipEntryBytes) {
        m_error = "file exceeds 4GiB zip limit";
        return false;
    }

    std::vector<char> data;
    try {
        data.resize(size);
    } catch (const std::bad_alloc &) {
        m_error = "can't allocate read buffer";
        return false;
    }

    if (size > 0) {
        in.seekg(0, std::ios::beg);
        in.read(data.data(), static_cast<std::streamsize>(size));
        if (in.gcount() != static_cast<std::streamsize>(size)) {
            m_error = "read file error";
            return false;
        }
    }

    // A zero-length vector has no data() to hand out, and AddFromBuffer()
    // rejects a null buffer — pass a valid empty address instead.
    return AddFromBuffer(innerFile, size ? data.data() : "", size);
}

const char *ZipMaker::GetError() const
{
    return m_error.empty() ? nullptr : m_error.c_str();
}

//-----------------ZipReader

ZipReader::ZipReader(const char *filePath)
{
    m_archive = nullptr;
    m_archive = unzOpen64(filePath);
}

ZipReader::~ZipReader()
{
    Close();
}

void ZipReader::Close()
{
    if (m_archive != nullptr){
        unzClose(m_archive);
        m_archive = nullptr;
    }
}

std::unique_ptr<ZipInnerFileData> ZipReader::GetInnterFileData(const char *innerFile)
{
    char szFilePath[15];
    unz_file_info64 fileInfo;
   
    if (m_archive == nullptr){
        return nullptr;
    }
  
    // inner file not exists
    if (unzLocateFile(m_archive, innerFile, 0) != UNZ_OK){
        return nullptr;
    }

    if (unzGetCurrentFileInfo64(m_archive, &fileInfo, szFilePath,
                                sizeof(szFilePath), nullptr, 0, nullptr, 0) != UNZ_OK)
    {  
        return nullptr;
    }

    if (unzOpenCurrentFile(m_archive) != UNZ_OK)
    { 
        return nullptr;
    }

    // Every failure from here on must still close the current file: the old
    // code only did that on the success path (and left it open when the read
    // buffer could not be allocated).
    if (fileInfo.uncompressed_size == 0 ||
        fileInfo.uncompressed_size > kMaxZipEntryBytes) {
        unzCloseCurrentFile(m_archive);
        return nullptr;
    }

    const size_t size = static_cast<size_t>(fileInfo.uncompressed_size);
    std::vector<char> metafile;
    try {
        metafile.resize(size);
    } catch (const std::bad_alloc &) {
        unzCloseCurrentFile(m_archive);
        return nullptr;
    }

    if (unzReadCurrentFile(m_archive, metafile.data(),
                           static_cast<unsigned int>(size)) < 0) {
        unzCloseCurrentFile(m_archive);
        return nullptr;
    }

    unzCloseCurrentFile(m_archive);
    return std::make_unique<ZipInnerFileData>(std::move(metafile));
}
