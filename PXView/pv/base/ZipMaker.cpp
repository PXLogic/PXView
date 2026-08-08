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
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <time.h>
  
ZipMaker::ZipMaker() :
    m_zDoc(nullptr)
{
    m_error[0] = 0; 
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
        strcpy(m_error, "zipOpen64 error");
    } 

//make zip inner file time 
    m_zi = new zip_fileinfo();

    time_t rawtime;
    time (&rawtime);
    struct tm *tinf= localtime(&rawtime);

    struct tm &ti = *tinf;
    zip_fileinfo &zi= *(zip_fileinfo*)m_zi;

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
   if (m_zi){
       delete ((zip_fileinfo*)m_zi);
       m_zi = nullptr;
   }
}

bool ZipMaker::Close(){
    if (m_zDoc){
       zipClose((zipFile)m_zDoc, nullptr);
       m_zDoc = nullptr;
       return true;
   }
   return false;     
}

bool ZipMaker::AddFromBuffer(const char *innerFile, const char *buffer, unsigned int buferSize)
{
    if (!buffer || !innerFile || !m_zDoc)
        return false;
    assert(buffer);
    assert(innerFile);
    assert(m_zDoc);
    int level = m_opt_compress_level;

    if (level < Z_DEFAULT_COMPRESSION  || level > Z_BEST_COMPRESSION){
        level = Z_DEFAULT_COMPRESSION;
    }

    zipOpenNewFileInZip((zipFile)m_zDoc,innerFile,(zip_fileinfo*)m_zi,
                                nullptr,0,nullptr,0,nullptr ,
                                Z_DEFLATED,
                                level);

    zipWriteInFileInZip((zipFile)m_zDoc, buffer, (unsigned int)buferSize);

    zipCloseFileInZip((zipFile)m_zDoc);

    return true;
}

bool ZipMaker::AddFromFile(const char *localFile, const char *innerFile)
{
    if (!localFile)
        return false;
    assert(localFile);

    struct stat st;
    FILE *fp;
    char *data = nullptr;
    long long size = 0;

    if ((fp = fopen(localFile, "rb")) == nullptr) {
        strcpy(m_error, "fopen error");        
        return false;
    }

    if (fstat(fileno(fp), &st) < 0) {
        strcpy(m_error, "fstat error");    
        fclose(fp);
        return -1;
    } 

    if ((data = (char*)malloc((size_t)st.st_size)) == nullptr) {
        strcpy(m_error, "can't malloc buffer");
        fclose(fp);
        return false;
    }

    if (fread(data, 1, (size_t)st.st_size, fp) < (size_t)st.st_size) {
        strcpy(m_error, "fread error");
        free(data);
        fclose(fp);
        return false;
    }

    fclose(fp);
    size = (size_t)st.st_size;

    bool ret = AddFromBuffer(innerFile, data, size);
    return ret;
}

const char *ZipMaker::GetError()
{
    if (m_error[0])
        return m_error;
    return nullptr;
}

//-----------------ZipReader

ZipInnerFileData::ZipInnerFileData(char *data, int size)
{
    _data = data;
    _size = size;
}

ZipInnerFileData::~ZipInnerFileData()
{
    if (_data != nullptr){
        free(_data);
        _data = nullptr;
    }
}

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

ZipInnerFileData* ZipReader::GetInnterFileData(const char *innerFile)
{
    char *metafile = nullptr;
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

    if (fileInfo.uncompressed_size > 0 && (metafile = (char *)malloc(fileInfo.uncompressed_size)))
    {
        unzReadCurrentFile(m_archive, metafile, fileInfo.uncompressed_size);
        unzCloseCurrentFile(m_archive);

         ZipInnerFileData *data = new ZipInnerFileData(metafile, fileInfo.uncompressed_size);
         return data;
    } 
 
    return nullptr;
}

void ZipReader::ReleaseInnerFileData(ZipInnerFileData *data)
{
    if (data){
        delete data;
    }
}