/*******************************************************

   CoolReader Engine

   lvstream.cpp:  stream classes implementation

   (c) Vadim Lopatin, 2000-2009
   This source code is distributed under the terms of
   GNU General Public License
   See LICENSE file for details

 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * UNRAR library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.

*******************************************************/

#include "../include/lvstream.h"
#include "../include/lvptrvec.h"
#include "../include/crtxtenc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if (USE_ZLIB==1)
#include <zlib.h>
#endif

#if !defined(__SYMBIAN32__) && defined(_WIN32)
extern "C" {
#include <windows.h>
}
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#endif

#ifdef _LINUX
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#endif


#ifndef USE_ANSI_FILES

#if !defined(__SYMBIAN32__) && defined(_WIN32)
#define USE_ANSI_FILES 0
#else
#define USE_ANSI_FILES 1
#endif

#endif

// LVStorageObject stubs
const lChar16 * LVStorageObject::GetName()
{
    return NULL;
}

LVContainer * LVStorageObject::GetParentContainer()
{
    return NULL;
}

void LVStorageObject::SetName(const lChar16 *)
{
}

bool LVStorageObject::IsContainer()
{
    return false;
}

lvsize_t LVStorageObject::GetSize( )
{
    lvsize_t sz;
    if ( GetSize( &sz )!=LVERR_OK )
        return LV_INVALID_SIZE;
    return sz;
}


/// calculate crc32 code for stream, if possible
lverror_t LVNamedStream::crc32( lUInt32 & dst )
{
    if ( _crc!=0 ) {
        dst = _crc;
        return LVERR_OK;
    } else {
        if ( !_crcFailed ) {
            lverror_t res = LVStream::crc32( dst );
            if ( res==LVERR_OK ) {
                _crc = dst;
                return LVERR_OK;
            }
            _crcFailed = true;
        }
        dst = 0;
        return LVERR_FAIL;
    }
}
/// returns stream/container name, may be NULL if unknown
const lChar16 * LVNamedStream::GetName()
{
    if (m_fname.empty())
        return NULL;
    return m_fname.c_str();
}
/// sets stream/container name, may be not implemented for some objects
void LVNamedStream::SetName(const lChar16 * name)
{
    m_fname = name;
    m_filename.clear();
    m_path.clear();
    if (m_fname.empty())
        return;
    const lChar16 * fn = m_fname.c_str();

    const lChar16 * p = fn + m_fname.length() - 1;
    for ( ;p>fn; p--) {
        if (p[-1] == '/' || p[-1]=='\\')
            break;
    }
    int pos = p-fn;
    if (p>fn)
        m_path = m_fname.substr(0, pos);
    m_filename = m_fname.substr(pos, m_fname.length() - pos);
}

/// Universal Read or write buffer for stream region for non-meped streams
// default implementation, with RAM buffer
class LVDefStreamBuffer : public LVStreamBuffer
{
protected:
    LVStreamRef m_stream;
    lUInt8 * m_buf;
    lvpos_t m_pos;
    lvsize_t m_size;
    bool m_readonly;
    bool m_writeonly;
public:
    static LVStreamBufferRef create( LVStreamRef stream, lvpos_t pos, lvsize_t size, bool readonly )
    {
        LVStreamBufferRef res;
        switch ( stream->GetMode() ) {
        case LVOM_ERROR:       ///< to indicate error state
        case LVOM_CLOSED:        ///< to indicate closed state
            return res;
        case LVOM_READ:          ///< readonly mode, use for r/o
            if ( !readonly )
                return res;
            break;
        case LVOM_WRITE:         ///< writeonly mode
        case LVOM_APPEND:        ///< append (readwrite) mode, use for r/w
        case LVOM_READWRITE:      ///< readwrite mode
            if ( readonly )
                return res;
            break;
        }
        lvsize_t sz;
        if ( stream->GetSize(&sz)!=LVERR_OK )
            return res;
        if ( pos + size > sz )
            return res; // wrong position/size
        LVDefStreamBuffer * buf = new LVDefStreamBuffer( stream, pos, size, readonly );
        if ( !buf->m_buf ) {
            delete buf;
            return res;
        }
        if ( stream->SetPos( pos )!=LVERR_OK ) {
            delete buf;
            return res;
        }
        lvsize_t bytesRead = 0;
        if ( stream->Read( buf->m_buf, size, &bytesRead )!=LVERR_OK || bytesRead!=size ) {
            delete buf;
            return res;
        }
        return LVStreamBufferRef( buf );
    }

    LVDefStreamBuffer( LVStreamRef stream, lvpos_t pos, lvsize_t size, bool readonly )
    : m_stream( stream ), m_buf( NULL ), m_pos(pos), m_size( size ), m_readonly( readonly )
    {
        m_buf = (lUInt8*)malloc( size );
        m_writeonly = (m_stream->GetMode()==LVOM_WRITE);
    }
    /// get pointer to read-only buffer, returns NULL if unavailable
    virtual const lUInt8 * getReadOnly()
    {
        return m_writeonly ? NULL : m_buf;
    }
    /// get pointer to read-write buffer, returns NULL if unavailable
    virtual lUInt8 * getReadWrite()
    {
        return m_readonly ? NULL : m_buf;
    }

    /// get buffer size
    virtual lvsize_t getSize()
    {
        return m_size;
    }

    /// write on close
    virtual bool close()
    {
        bool res = true;
        if ( m_buf ) {
            if ( !m_readonly ) {
                if ( m_stream->SetPos( m_pos )!=LVERR_OK ) {
                    res = false;
                } else {
                    lvsize_t bytesWritten = 0;
                    if ( m_stream->Write( m_buf, m_size, &bytesWritten )!=LVERR_OK || bytesWritten!=m_size ) {
                        res = false;
                    }
                }
            }
            free( m_buf );
        }
        m_buf = NULL;
        m_stream = NULL;
        m_size = 0;
        m_pos = 0;
        return res;
    }
    /// flush on destroy
    virtual ~LVDefStreamBuffer()
    {
        close();
    }
};

/// Get read buffer - default implementation, with RAM buffer
LVStreamBufferRef LVStream::GetReadBuffer( lvpos_t pos, lvpos_t size )
{
    LVStreamBufferRef res = LVDefStreamBuffer::create( LVStreamRef(this), pos, size, true );
    return res;
}

/// Get read/write buffer - default implementation, with RAM buffer
LVStreamBufferRef LVStream::GetWriteBuffer( lvpos_t pos, lvpos_t size )
{
    LVStreamBufferRef res = LVDefStreamBuffer::create( LVStreamRef(this), pos, size, false );
    return res;
}


#define CRC_BUF_SIZE 16384

/// calculate crc32 code for stream, if possible
lverror_t LVStream::crc32( lUInt32 & dst )
{
    dst = 0;
    if ( GetMode() == LVOM_READ || GetMode() == LVOM_APPEND ) {
        lvpos_t savepos = GetPos();
        lvsize_t size = GetSize();
        lUInt8 buf[CRC_BUF_SIZE];
        SetPos( 0 );
        lvsize_t bytesRead = 0;
        for ( lvpos_t pos = 0; pos<size; pos+=CRC_BUF_SIZE ) {
            lvsize_t sz = size - pos;
            if ( sz > CRC_BUF_SIZE )
                sz = CRC_BUF_SIZE;
            Read( buf, sz, &bytesRead );
            if ( bytesRead!=sz ) {
                SetPos(savepos);
                return LVERR_FAIL;
            }
            dst = lStr_crc32( dst, buf, sz );
        }
        SetPos( savepos );
        return LVERR_OK;
    } else {
        // not supported
        return LVERR_NOTIMPL;
    }
}


//#if USE__FILES==1
#if defined(_LINUX) || defined(_WIN32)

class LVFileMappedStream : public LVNamedStream
{
private:
#if defined(_WIN32)
    HANDLE m_hFile;
    HANDLE m_hMap;
#else
    int m_fd;
#endif
    lUInt8* m_map;
    lvsize_t m_size;
    lvpos_t m_pos;

    /// Read or write buffer for stream region
    class LVBuffer : public LVStreamBuffer
    {
    protected:
        LVStreamRef m_stream;
        lUInt8 * m_buf;
        lvsize_t m_size;
        bool m_readonly;
    public:
        LVBuffer( LVStreamRef stream, lUInt8 * buf, lvsize_t size, bool readonly )
        : m_stream( stream ), m_buf( buf ), m_size( size ), m_readonly( readonly )
        {
        }

        /// get pointer to read-only buffer, returns NULL if unavailable
        virtual const lUInt8 * getReadOnly()
        {
            return m_buf;
        }

        /// get pointer to read-write buffer, returns NULL if unavailable
        virtual lUInt8 * getReadWrite()
        {
            return m_readonly ? NULL : m_buf;
        }

        /// get buffer size
        virtual lvsize_t getSize()
        {
            return m_size;
        }

        /// flush on destroy
        virtual ~LVBuffer() { }

    };


public:

    /// Get read buffer (optimal for )
    virtual LVStreamBufferRef GetReadBuffer( lvpos_t pos, lvpos_t size )
    {
        LVStreamBufferRef res;
        if ( !m_map )
            return res;
        if ( (m_mode!=LVOM_APPEND && m_mode!=LVOM_READ) || pos + size > m_size || size==0 )
            return res;
        return LVStreamBufferRef ( new LVBuffer( LVStreamRef(this), m_map + pos, size, true ) );
    }

    /// Get read/write buffer (optimal for )
    virtual LVStreamBufferRef GetWriteBuffer( lvpos_t pos, lvpos_t size )
    {
        LVStreamBufferRef res;
        if ( !m_map )
            return res;
        if ( m_mode!=LVOM_APPEND || pos + size > m_size || size==0 )
            return res;
        return LVStreamBufferRef ( new LVBuffer( LVStreamRef(this), m_map + pos, size, false ) );
    }

    virtual lverror_t Seek( lvoffset_t offset, lvseek_origin_t origin, lvpos_t * pNewPos )
    {
        //
        lvpos_t newpos = m_pos;
        switch ( origin )
        {
        case LVSEEK_SET:
            newpos = offset;
            break;
        case LVSEEK_CUR:
            newpos += offset;
            break;
        case LVSEEK_END:
            newpos = m_size + offset;
            break;
        }
        if ( newpos>m_size )
            return LVERR_FAIL;
        if ( pNewPos!=NULL )
            *pNewPos = newpos;
        m_pos = newpos;
        return LVERR_OK;
    }

    /// Tell current file position
    /**
        \param pNewPos points to place to store file position
        \return lverror_t status: LVERR_OK if success
    */
    virtual lverror_t Tell( lvpos_t * pPos )
    {
        *pPos = m_pos;
        return LVERR_OK;
    }

    virtual lvpos_t SetPos(lvpos_t p)
    {
        if ( p<=m_size ) {
            m_pos = p;
            return m_pos;
        }
        return (lvpos_t)(~0);
    }

    /// Get file position
    /**
        \return lvpos_t file position
    */
    virtual lvpos_t   GetPos()
    {
        return m_pos;
    }

    /// Get file size
    /**
        \return lvsize_t file size
    */
    virtual lvsize_t  GetSize()
    {
        return m_size;
    }

    virtual lverror_t GetSize( lvsize_t * pSize )
    {
        *pSize = m_size;
        return LVERR_OK;
    }

    lverror_t error()
    {
#if defined(_WIN32)
		if ( m_hFile!=NULL ) {
			UnMap();
			if ( !CloseHandle(m_hFile) )
				CRLog::error("Error while closing file handle");
			m_hFile = NULL;
		}
#else
		if ( m_fd!= -1 ) {
            CRLog::trace("Closing mapped file %s", UnicodeToUtf8(GetName()).c_str() );
			UnMap();
            close(m_fd);
		}
        m_fd = -1;
#endif
        m_map = NULL;
        m_size = 0;
        m_mode = LVOM_ERROR;
        return LVERR_FAIL;
    }

    virtual lverror_t Read( void * buf, lvsize_t count, lvsize_t * nBytesRead )
    {
        if ( !m_map )
            return LVERR_FAIL;
        int cnt = (int)count;
        if ( m_pos + cnt > m_size )
            cnt = (int)(m_size - m_pos);
        if ( cnt <= 0 )
            return LVERR_FAIL;
        memcpy( buf, m_map + m_pos, cnt );
        m_pos += cnt;
        if (nBytesRead)
            *nBytesRead = cnt;
        return LVERR_OK;
    }

    virtual bool Read( lUInt8 * buf )
    {
        if ( m_pos < m_size ) {
            *buf = m_map[ m_pos++ ];
            return true;
        }
        return false;
    }

    virtual bool Read( lUInt16 * buf )
    {
        if ( m_pos+1 < m_size ) {
            *buf = m_map[ m_pos ] | ( ( (lUInt16)m_map[ m_pos+1 ] )<<8 );
            m_pos += 2;
            return true;
        }
        return false;
    }

    virtual bool Read( lUInt32 * buf )
    {
        if ( m_pos+3 < m_size ) {
            *buf = m_map[ m_pos ] | ( ( (lUInt32)m_map[ m_pos+1 ] )<<8 )
                | ( ( (lUInt32)m_map[ m_pos+2 ] )<<16 )
                | ( ( (lUInt32)m_map[ m_pos+3 ] )<<24 )
                ;
            m_pos += 4;
            return true;
        }
        return false;
    }

    virtual int ReadByte()
    {
        if ( m_pos < m_size ) {
            return m_map[ m_pos++ ];
        }
        return -1;
    }

    virtual lverror_t Write( const void * buf, lvsize_t count, lvsize_t * nBytesWritten )
    {
        if ( m_mode!=LVOM_APPEND )
            return LVERR_FAIL;
        lvsize_t maxSize = (lvsize_t)(m_size - m_pos);
        if ( maxSize<=0 )
            return LVERR_FAIL; // end of file reached: resize is not supported yet
        if ( count > maxSize || count > m_size )
            count = maxSize;
        memcpy( m_map + m_pos, buf, count );
        m_pos += count;
        if ( nBytesWritten )
            *nBytesWritten = count;
        return LVERR_OK;
    }

    virtual bool Eof()
    {
        return (m_pos >= m_size);
    }

    static LVFileMappedStream * CreateFileStream( lString16 fname, lvopen_mode_t mode, int minSize )
    {
        LVFileMappedStream * f = new LVFileMappedStream();
        if ( f->OpenFile( fname, mode, minSize )==LVERR_OK ) {
            return f;
        } else {
            delete f;
            return NULL;
        }
    }

	lverror_t Map()
	{
#if defined(_WIN32)
		m_hMap = CreateFileMapping(
			m_hFile,
			NULL,
			(m_mode==LVOM_READ)?PAGE_READONLY:PAGE_READWRITE, //flProtect,
			0,
			0,
			NULL
		);
		if ( m_hMap==NULL ) {
			DWORD err = GetLastError();
            CRLog::error( "LVFileMappedStream::Map() -- Cannot map file to memory, err=%08x, hFile=%08x", err, (lUInt32)m_hFile );
            return error();
		}
		m_map = (lUInt8*) MapViewOfFile(
			m_hMap,
			m_mode==LVOM_READ ? FILE_MAP_READ : FILE_MAP_READ|FILE_MAP_WRITE,
			0,
			0,
			m_size
		);
		if ( m_map==NULL ) {
            CRLog::error( "LVFileMappedStream::Map() -- Cannot map file to memory" );
            return error();
		}
		return LVERR_OK;
#else
        int mapFlags = (m_mode==LVOM_READ) ? PROT_READ : PROT_READ | PROT_WRITE;
        m_map = (lUInt8*)mmap( 0, m_size, mapFlags, MAP_SHARED, m_fd, 0 );
        if ( m_map == MAP_FAILED ) {
            CRLog::error( "LVFileMappedStream::Map() -- Cannot map file to memory" );
            return error();
        }
        return LVERR_OK;
#endif
	}

	lverror_t UnMap()
	{
#if defined(_WIN32)
		lverror_t res = LVERR_OK;
		if ( m_map!=NULL ) {
			if ( !UnmapViewOfFile( m_map ) ) {
	            CRLog::error("LVFileMappedStream::UnMap() -- Error while unmapping file");
				res = LVERR_FAIL;
			}
			m_map = NULL;
		}
		if ( m_hMap!=NULL ) {
			if ( !CloseHandle( m_hMap ) ) {
	            CRLog::error("LVFileMappedStream::UnMap() -- Error while unmapping file");
				res = LVERR_FAIL;
			}
			m_hMap = NULL;
		}
		if ( res!=LVERR_OK )
			return error();
		return res;
#else
        if ( m_map!=NULL && munmap( m_map, m_size ) == -1 ) {
            m_map = NULL;
            CRLog::error("LVFileMappedStream::UnMap() -- Error while unmapping file");
            return error();
        }
        return LVERR_OK;
#endif
	}

    virtual lverror_t SetSize( lvsize_t size )
    {
        // support only size grow
        if ( m_mode!=LVOM_APPEND )
            return LVERR_FAIL;
        if ( size == m_size )
            return LVERR_OK;
        //if ( size < m_size )
        //    return LVERR_FAIL;

		bool wasMapped = false;
        if ( m_map!=NULL ) {
			wasMapped = true;
			if ( UnMap()!=LVERR_OK )
	            return LVERR_FAIL;
        }
        m_size = size;

#if defined(_WIN32)
		// WIN32
		__int64 offset = size - 1;
        lUInt32 pos_low = (lUInt32)((__int64)offset & 0xFFFFFFFF);
        long pos_high = (long)(((__int64)offset >> 32) & 0xFFFFFFFF);
		pos_low = SetFilePointer(m_hFile, pos_low, &pos_high, FILE_BEGIN );
        if (pos_low == 0xFFFFFFFF) {
            lUInt32 err = GetLastError();
            if (err == ERROR_NOACCESS)
                pos_low = (lUInt32)offset;
            else if ( err != ERROR_SUCCESS)
                return error();
        }
		DWORD bytesWritten = 0;
		if ( !WriteFile( m_hFile, "", 1, &bytesWritten, NULL ) || bytesWritten!=1 )
			return error();
#else
		// LINUX
		if ( lseek( m_fd, size-1, SEEK_SET ) == -1 ) {
            CRLog::error("LVFileMappedStream::SetSize() -- Seek error");
            return error();
        }
        if ( write(m_fd, "", 1) != 1 ) {
            CRLog::error("LVFileMappedStream::SetSize() -- File resize error");
            return error();
        }
#endif
		if ( wasMapped ) {
			if ( Map() != LVERR_OK ) {
				return error();
			}
		}
        return LVERR_OK;
    }
	
	lverror_t OpenFile( lString16 fname, lvopen_mode_t mode, lvsize_t minSize = -1 )
    {
        m_mode = mode;
        if ( mode!=LVOM_READ && mode!=LVOM_APPEND )
            return LVERR_FAIL; // not supported
        if ( minSize==-1 ) {
            if ( !LVFileExists(fname) )
                return LVERR_FAIL;
        }
        //if ( mode==LVOM_APPEND && minSize<=0 )
        //    return LVERR_FAIL;
        SetName(fname.c_str());
        lString8 fn8 = UnicodeToUtf8( fname );
#if defined(_WIN32)
		//========================================================
		// WIN32 IMPLEMENTATION
        lUInt32 m = 0;
        lUInt32 s = 0;
        lUInt32 c = 0;
        SetName(fname.c_str());
        switch (mode) {
        case LVOM_READWRITE:
            m |= GENERIC_WRITE|GENERIC_READ;
            s |= FILE_SHARE_WRITE|FILE_SHARE_READ;
            c |= OPEN_ALWAYS;
            break;
        case LVOM_READ:
            m |= GENERIC_READ;
            s |= FILE_SHARE_READ;
            c |= OPEN_EXISTING;
            break;
        case LVOM_WRITE:
            m |= GENERIC_WRITE;
            s |= FILE_SHARE_WRITE;
            c |= CREATE_ALWAYS;
            break;
        case LVOM_APPEND:
            m |= GENERIC_WRITE|GENERIC_READ;
            s |= FILE_SHARE_WRITE;
            c |= OPEN_ALWAYS;
            break;
        case LVOM_CLOSED:
        case LVOM_ERROR:
            crFatalError();
            break;
        }
        m_hFile = CreateFileW( fname.c_str(), m, s, NULL, c, FILE_ATTRIBUTE_NORMAL, NULL);
        if (m_hFile == INVALID_HANDLE_VALUE || !m_hFile) {
			// unicode not implemented?
			lUInt32 err = GetLastError();
			if (err==ERROR_CALL_NOT_IMPLEMENTED)
				m_hFile = CreateFileA( fn8.c_str(), m, s, NULL, c, FILE_ATTRIBUTE_NORMAL, NULL);
			if ( (m_hFile == INVALID_HANDLE_VALUE) || (!m_hFile) ) {
                CRLog::error("Error opening file %s", UnicodeToUtf8(fname).c_str() );
                m_hFile = NULL;
				// error
				return error();
			}
		}
		// check size
        lUInt32 hw=0;
        m_size = GetFileSize( m_hFile, &hw );
#if LVLONG_FILE_SUPPORT
        if (hw)
            m_size |= (((lvsize_t)hw)<<32);
#endif

        if ( minSize>=0 && mode == LVOM_APPEND && m_size < minSize ) {
            if ( SetSize( minSize ) != LVERR_OK ) {
                CRLog::error( "Cannot set file size for %s", fn8.c_str() );
                return error();
            }
        }

		if ( Map()!=LVERR_OK )
			return error();

		return LVERR_OK;


#else
		//========================================================
		// LINUX IMPLEMENTATION
        m_fd = -1;

        int flags = (mode==LVOM_READ) ? O_RDONLY : O_RDWR | O_CREAT | O_SYNC;
        m_fd = open( fn8.c_str(), flags, (mode_t)0666);
        if (m_fd == -1) {
            CRLog::error( "Error opening file %s for %s, errno=%d, msg=%s", fn8.c_str(), (mode==LVOM_READ) ? "reading" : "read/write",  (int)errno, strerror(errno) );
            return error();
        }
        struct stat stat;
        if ( fstat( m_fd, &stat ) ) {
            CRLog::error( "Cannot get file size for %s", fn8.c_str() );
            return error();
        }
        m_size = (lvsize_t) stat.st_size;
        if ( mode == LVOM_APPEND && m_size < minSize ) {
            if ( SetSize( minSize ) != LVERR_OK ) {
                CRLog::error( "Cannot set file size for %s", fn8.c_str() );
                return error();
            }
        }

        int mapFlags = (mode==LVOM_READ) ? PROT_READ : PROT_READ | PROT_WRITE;
        m_map = (lUInt8*)mmap( 0, m_size, mapFlags, MAP_SHARED, m_fd, 0 );
        if ( m_map == MAP_FAILED ) {
            CRLog::error( "Cannot map file %s to memory", fn8.c_str() );
            return error();
        }
        return LVERR_OK;
#endif
    }
    LVFileMappedStream() 
#if defined(_WIN32)
		: m_hFile(NULL), m_hMap(NULL),
#else
		: m_fd(-1),
#endif
		m_map(NULL), m_size(0), m_pos(0)
    {
        m_mode=LVOM_ERROR;
    }
    virtual ~LVFileMappedStream()
    {
		// reuse error() to close file
		error();
    }
};
#endif


/// Open memory mapped file
/**
    \param pathname is file name to open (unicode)
    \param mode is mode file should be opened in (LVOM_READ or LVOM_APPEND only)
	\param minSize is minimum file size for R/W mode
    \return reference to opened stream if success, NULL if error
*/
LVStreamRef LVMapFileStream( const lChar8 * pathname, lvopen_mode_t mode, lvsize_t minSize )
{
	lString16 fn = LocalToUnicode( lString8(pathname) );
	return LVMapFileStream( fn.c_str(), mode, minSize );
}


#if (USE_ANSI_FILES==1)

class LVFileStream : public LVNamedStream
{
private:
    FILE * m_file;
public:


    virtual lverror_t Seek( lvoffset_t offset, lvseek_origin_t origin, lvpos_t * pNewPos )
    {
       //
       int res = -1;
       switch ( origin )
       {
       case LVSEEK_SET:
           res = fseek( m_file, offset, SEEK_SET );
           break;
       case LVSEEK_CUR:
           res = fseek( m_file, offset, SEEK_CUR );
           break;
       case LVSEEK_END:
           res = fseek( m_file, offset, SEEK_END );
           break;
       }
       if (res==0)
       {
          if ( pNewPos )
              * pNewPos = ftell(m_file);
          return LVERR_OK;
       }
       CRLog::error("error setting file position to %d (%d)", (int)offset, (int)origin );
       return LVERR_FAIL;
    }
    virtual lverror_t SetSize( lvsize_t )
    {
        /*
        int64 sz = m_file->SetSize( size );
        if (sz==-1)
           return LVERR_FAIL;
        else
           return LVERR_OK;
        */
        return LVERR_FAIL;
    }
    virtual lverror_t Read( void * buf, lvsize_t count, lvsize_t * nBytesRead )
    {
        lvsize_t sz = fread( buf, 1, count, m_file );
        if (nBytesRead)
            *nBytesRead = sz;
        if ( sz==0 )
        {
            return LVERR_FAIL;
        }
        return LVERR_OK;
    }
    virtual lverror_t Write( const void * buf, lvsize_t count, lvsize_t * nBytesWritten )
    {
        lvsize_t sz = fwrite( buf, 1, count, m_file );
        if (nBytesWritten)
            *nBytesWritten = sz;
        if (sz < count)
        {
            return LVERR_FAIL;
        }
        return LVERR_OK;
    }
    virtual bool Eof()
    {
        return feof(m_file)!=0;
    }
    static LVFileStream * CreateFileStream( lString16 fname, lvopen_mode_t mode )
    {
        LVFileStream * f = new LVFileStream;
        if (f->OpenFile( fname, mode )==LVERR_OK) {
            return f;
        } else {
            delete f;
            return NULL;
        }
    }
    lverror_t OpenFile( lString16 fname, lvopen_mode_t mode )
    {
        m_mode = mode;
        m_file = NULL;
        SetName(fname.c_str());
        const char * modestr = "r";
        switch (mode) {
        case LVOM_READ:
            modestr = "rb";
            break;
        case LVOM_WRITE:
            modestr = "wb";
            break;
        case LVOM_READWRITE:
        case LVOM_APPEND:
            modestr = "a+b";
            break;
        case LVOM_CLOSED:
        case LVOM_ERROR:
            break;
        }
        FILE * file = fopen(UnicodeToLocal(fname).c_str(), modestr);
        if (!file)
        {
            //printf("cannot open file %s\n", UnicodeToLocal(fname).c_str());
            m_mode = LVOM_ERROR;
            return LVERR_FAIL;
        }
        m_file = file;
        //printf("file %s opened ok\n", UnicodeToLocal(fname).c_str());
        // set filename
        SetName( fname.c_str() );
        return LVERR_OK;
    }
    LVFileStream() : m_file(NULL)
    {
        m_mode=LVOM_ERROR;
    }
    virtual ~LVFileStream()
    {
        if (m_file)
            fclose(m_file);
    }
};

#else

class LVDirectoryContainer;
class LVFileStream : public LVNamedStream
{
    friend class LVDirectoryContainer;
protected:
    HANDLE                 m_hFile;
    LVDirectoryContainer * m_parent;
    lvsize_t               m_size;
    lvpos_t                m_pos;
public:
    virtual bool Eof()
    {
        return m_size<=m_pos;
    }
    virtual LVContainer * GetParentContainer()
    {
        return (LVContainer*)m_parent;
    }
    virtual lverror_t Read( void * buf, lvsize_t count, lvsize_t * nBytesRead )
    {
        //fprintf(stderr, "Read(%08x, %d)\n", buf, count);

        if (m_hFile == INVALID_HANDLE_VALUE || m_mode==LVOM_WRITE || m_mode==LVOM_APPEND )
            return LVERR_FAIL;
        //
        lUInt32 dwBytesRead = 0;
        if (ReadFile( m_hFile, buf, (lUInt32)count, &dwBytesRead, NULL )) {
            if (nBytesRead)
                *nBytesRead = dwBytesRead;
            m_pos += dwBytesRead;
        } else {
            return LVERR_FAIL;
        }

        return LVERR_OK;
    }
    virtual lverror_t GetSize( lvsize_t * pSize )
    {
        if (m_hFile == INVALID_HANDLE_VALUE || !pSize)
            return LVERR_FAIL;
        if (m_size<m_pos)
            m_size = m_pos;
        *pSize = m_size;
        return LVERR_OK;
    }
    virtual lvsize_t GetSize()
    {
        if (m_hFile == INVALID_HANDLE_VALUE)
            return 0;
        if (m_size<m_pos)
            m_size = m_pos;
        return m_size;
    }
    virtual lverror_t SetSize( lvsize_t size )
    {
        //
        if (m_hFile == INVALID_HANDLE_VALUE || m_mode==LVOM_READ )
            return LVERR_FAIL;
        lvpos_t oldpos;
        Tell(&oldpos);
        if (!Seek(size, LVSEEK_SET, NULL))
            return LVERR_FAIL;
        SetEndOfFile( m_hFile);
        Seek(oldpos, LVSEEK_SET, NULL);
        return LVERR_OK;
    }
    virtual lverror_t Write( const void * buf, lvsize_t count, lvsize_t * nBytesWritten )
    {
        if (m_hFile == INVALID_HANDLE_VALUE || m_mode==LVOM_READ )
            return LVERR_FAIL;
        //
        lUInt32 dwBytesWritten = 0;
        if (WriteFile( m_hFile, buf, (lUInt32)count, &dwBytesWritten, NULL )) {
            if (nBytesWritten)
                *nBytesWritten = dwBytesWritten;
            m_pos += dwBytesWritten;
        } else {
            return LVERR_FAIL;
        }

        return LVERR_OK;
    }
    virtual lverror_t Seek( lvoffset_t offset, lvseek_origin_t origin, lvpos_t * pNewPos )
    {
        //fprintf(stderr, "Seek(%d,%d)\n", offset, origin);
        if (m_hFile == INVALID_HANDLE_VALUE)
            return LVERR_FAIL;
        lUInt32 pos_low = (lUInt32)((__int64)offset & 0xFFFFFFFF);
        long pos_high = (long)(((__int64)offset >> 32) & 0xFFFFFFFF);
        lUInt32 m=0;
        switch (origin) {
        case LVSEEK_SET:
            m = FILE_BEGIN;
            break;
        case LVSEEK_CUR:
            m = FILE_CURRENT;
            break;
        case LVSEEK_END:
            m = FILE_END;
            break;
        }

        pos_low = SetFilePointer(m_hFile, pos_low, &pos_high, m );
        if (pos_low == 0xFFFFFFFF) {
            lUInt32 err = GetLastError();
            if (err == ERROR_NOACCESS)
                pos_low = (lUInt32)offset;
            else if ( err != ERROR_SUCCESS)
                return LVERR_FAIL;
        }
        m_pos = pos_low
#if LVLONG_FILE_SUPPORT
         | ((lvpos_t)pos_high<<32)
#endif
          ;
        if (pNewPos)
            *pNewPos = m_pos;
        return LVERR_OK;
    }
    lverror_t Close()
    {
        if (m_hFile == INVALID_HANDLE_VALUE)
            return LVERR_FAIL;
        CloseHandle( m_hFile );
        m_hFile = INVALID_HANDLE_VALUE;
        SetName(NULL);
        return LVERR_OK;
    }
    static LVFileStream * CreateFileStream( lString16 fname, lvopen_mode_t mode )
    {
        LVFileStream * f = new LVFileStream;
        if (f->OpenFile( fname, mode )==LVERR_OK) {
            return f;
        } else {
            delete f;
            return NULL;
        }
    }
    lverror_t OpenFile( lString16 fname, lvopen_mode_t mode )
    {
        lUInt32 m = 0;
        lUInt32 s = 0;
        lUInt32 c = 0;
        SetName(fname.c_str());
        switch (mode) {
        case LVOM_READWRITE:
            m |= GENERIC_WRITE|GENERIC_READ;
            s |= FILE_SHARE_WRITE|FILE_SHARE_READ;
            c |= OPEN_ALWAYS;
            break;
        case LVOM_READ:
            m |= GENERIC_READ;
            s |= FILE_SHARE_READ;
            c |= OPEN_EXISTING;
            break;
        case LVOM_WRITE:
            m |= GENERIC_WRITE;
            s |= FILE_SHARE_WRITE;
            c |= CREATE_ALWAYS;
            break;
        case LVOM_APPEND:
            m |= GENERIC_WRITE;
            s |= FILE_SHARE_WRITE;
            c |= OPEN_ALWAYS;
            break;
        case LVOM_CLOSED:
        case LVOM_ERROR:
            crFatalError();
            break;
        }
        m_hFile = CreateFileW( fname.c_str(), m, s, NULL, c, FILE_ATTRIBUTE_NORMAL, NULL);
        if (m_hFile == INVALID_HANDLE_VALUE || !m_hFile) {
         // unicode not implemented?
            lUInt32 err = GetLastError();
            if (err==ERROR_CALL_NOT_IMPLEMENTED)
                m_hFile = CreateFileA( UnicodeToLocal(fname).c_str(), m, s, NULL, c, FILE_ATTRIBUTE_NORMAL, NULL);
            if ( (m_hFile == INVALID_HANDLE_VALUE) || (!m_hFile) ) {
                // error
                return LVERR_FAIL;
            }
        }

        // set file size and position
        m_mode = mode;
        lUInt32 hw=0;
        m_size = GetFileSize( m_hFile, &hw );
#if LVLONG_FILE_SUPPORT
        if (hw)
            m_size |= (((lvsize_t)hw)<<32);
#endif
        m_pos = 0;

        // set filename
        SetName( fname.c_str() );

        // move to end of file
        if (mode==LVOM_APPEND)
            Seek( 0, LVSEEK_END, NULL );

        return LVERR_OK;
    }
    LVFileStream() : m_hFile(INVALID_HANDLE_VALUE), m_parent(NULL), m_size(0), m_pos(0)
    {
    }
    virtual ~LVFileStream()
    {
        Close();
        m_parent = NULL;
    }
};
#endif

// facility functions
LVStreamRef LVOpenFileStream( const lChar16 * pathname, lvopen_mode_t mode )
{
    lString16 fn(pathname);
#if 0
    //defined(_LINUX) || defined(_WIN32)
    if ( mode==LVOM_READ ) {
        LVFileMappedStream * stream = LVFileMappedStream::CreateFileStream( fn, mode, 0 );
        if ( stream != NULL )
        {
            return LVStreamRef( stream );
        }
        return LVStreamRef();
    }
#endif

    LVFileStream * stream = LVFileStream::CreateFileStream( fn, mode );
    if ( stream!=NULL )
    {
        return LVStreamRef( stream );
    }
    return LVStreamRef();
}

LVStreamRef LVOpenFileStream( const lChar8 * pathname, lvopen_mode_t mode )
{
    lString16 fn = LocalToUnicode(lString8(pathname));
    return LVOpenFileStream( fn.c_str(), mode );
}


lvopen_mode_t LVTextStream::GetMode()
{
    return m_base_stream->GetMode();
}

lverror_t LVTextStream::Seek( lvoffset_t offset, lvseek_origin_t origin, lvpos_t * pNewPos )
{
    return m_base_stream->Seek(offset, origin, pNewPos);
}

lverror_t LVTextStream::Tell( lvpos_t * pPos )
{
    return m_base_stream->Tell(pPos);
}

lvpos_t   LVTextStream::SetPos(lvpos_t p)
{
    return m_base_stream->SetPos(p);
}

lvpos_t   LVTextStream::GetPos()
{
    return m_base_stream->GetPos();
}

lverror_t LVTextStream::SetSize( lvsize_t size )
{
    return m_base_stream->SetSize(size);
}

lverror_t LVTextStream::Read( void * buf, lvsize_t count, lvsize_t * nBytesRead )
{
    return m_base_stream->Read(buf, count, nBytesRead);
}

lverror_t LVTextStream::Write( const void * buf, lvsize_t count, lvsize_t * nBytesWritten )
{
    return m_base_stream->Write(buf, count, nBytesWritten);
}

bool LVTextStream::Eof()
{
    return m_base_stream->Eof();
}


class LVDirectoryContainerItemInfo : public LVCommonContainerItemInfo
{
    friend class LVDirectoryContainer;
};

class LVDirectoryContainer : public LVNamedContainer
{
protected:
    LVDirectoryContainer * m_parent;
public:
    virtual LVStreamRef OpenStream( const wchar_t * fname, lvopen_mode_t mode )
    {
        int found_index = -1;
        for (int i=0; i<m_list.length(); i++) {
            if ( !lStr_cmp( fname, m_list[i]->GetName() ) ) {
                if ( m_list[i]->IsContainer() ) {
                    // found directory with same name!!!
                    return LVStreamRef();
                }
                found_index = i;
                break;
            }
        }
        // make filename
        lString16 fn = m_fname;
        fn << fname;
        //const char * fb8 = UnicodeToUtf8( fn ).c_str();
        //printf("Opening directory container file %s : %s fname=%s path=%s\n", UnicodeToUtf8( lString16(fname) ).c_str(), UnicodeToUtf8( fn ).c_str(), UnicodeToUtf8( m_fname ).c_str(), UnicodeToUtf8( m_path ).c_str());
        LVStreamRef stream( LVOpenFileStream( fn.c_str(), mode ) );
        if (!stream) {
            return stream;
        }
        //stream->m_parent = this;
        if (found_index<0) {
            // add new info
            LVDirectoryContainerItemInfo * item = new LVDirectoryContainerItemInfo;
            item->m_name = fname;
            stream->GetSize(&item->m_size);
            Add(item);
        }
        return stream;
    }
    virtual LVContainer * GetParentContainer()
    {
        return (LVContainer*)m_parent;
    }
    virtual const LVContainerItemInfo * GetObjectInfo(int index)
    {
        if (index>=0 && index<m_list.length())
            return m_list[index];
        return NULL;
    }
    virtual int GetObjectCount() const
    {
        return m_list.length();
    }
    virtual lverror_t GetSize( lvsize_t * pSize )
    {
        if (m_fname.empty())
            return LVERR_FAIL;
        *pSize = GetObjectCount();
        return LVERR_OK;
    }
    LVDirectoryContainer() : m_parent(NULL)
    {
    }
    virtual ~LVDirectoryContainer()
    {
        SetName(NULL);
        Clear();
    }
    static LVDirectoryContainer * OpenDirectory( const wchar_t * path, const wchar_t * mask = L"*.*" )
    {
        if (!path || !path[0])
            return NULL;


        // container object
        LVDirectoryContainer * dir = new LVDirectoryContainer;

        // make filename
        lString16 fn( path );
        lChar16 lastch = 0;
        if ( !fn.empty() )
            lastch = fn[fn.length()-1];
        if ( lastch!='\\' && lastch!='/' )
            fn << dir->m_path_separator;

        dir->SetName(fn.c_str());

#if !defined(__SYMBIAN32__) && defined(_WIN32)
        // WIN32 API
        fn << mask;
        WIN32_FIND_DATAW data;
        WIN32_FIND_DATAA dataa;
        memset(&data, 0, sizeof(data));
        memset(&dataa, 0, sizeof(dataa));
        //lString8 bs = DOMString(path).ToAnsiString();
        HANDLE hFind = FindFirstFileW(fn.c_str(), &data);
        bool unicode=true;
        if (hFind == INVALID_HANDLE_VALUE || !hFind) {
            lUInt32 err=GetLastError();
            if (err == ERROR_CALL_NOT_IMPLEMENTED) {
                hFind = FindFirstFileA(UnicodeToLocal(fn).c_str(), &dataa);
                unicode=false;
                if (hFind == INVALID_HANDLE_VALUE || !hFind)
                {
                    delete dir;
                    return NULL;
                }
            } else {
                delete dir;
                return NULL;
            }
        }

        if (unicode) {
            // unicode
            while (1) {
                lUInt32 dwAttrs = data.dwFileAttributes;
                wchar_t * pfn = data.cFileName;
                for (int i=0; data.cFileName[i]; i++) {
                    if (data.cFileName[i]=='/' || data.cFileName[i]=='\\')
                        pfn = data.cFileName + i + 1;
                }

                if ( (dwAttrs & FILE_ATTRIBUTE_DIRECTORY) ) {
                    // directory
                    if (!lStr_cmp(pfn, L"..") || !lStr_cmp(pfn, L".")) {
                        // .. or .
                    } else {
                        // normal directory
                        LVDirectoryContainerItemInfo * item = new LVDirectoryContainerItemInfo;
                        item->m_name = pfn;
                        item->m_is_container = true;
                        dir->Add(item);
                    }
                } else {
                    // file
                    LVDirectoryContainerItemInfo * item = new LVDirectoryContainerItemInfo;
                    item->m_name = pfn;
                    item->m_size = data.nFileSizeLow;
                    item->m_flags = data.dwFileAttributes;
                    dir->Add(item);
                }

                if (!FindNextFileW(hFind, &data)) {
                    // end of list
                    break;
                }

            }
        } else {
            // ANSI
            while (1) {
                lUInt32 dwAttrs = dataa.dwFileAttributes;
                char * pfn = dataa.cFileName;
                for (int i=0; dataa.cFileName[i]; i++) {
                    if (dataa.cFileName[i]=='/' || dataa.cFileName[i]=='\\')
                        pfn = dataa.cFileName + i + 1;
                }

                if ( (dwAttrs & FILE_ATTRIBUTE_DIRECTORY) ) {
                    // directory
                    if (!strcmp(pfn, "..") || !strcmp(pfn, ".")) {
                        // .. or .
                    } else {
                        // normal directory
                        LVDirectoryContainerItemInfo * item = new LVDirectoryContainerItemInfo;
                        item->m_name = LocalToUnicode( lString8( pfn ) );
                        item->m_is_container = true;
                        dir->Add(item);
                    }
                } else {
                    // file
                    LVDirectoryContainerItemInfo * item = new LVDirectoryContainerItemInfo;
                    item->m_name = LocalToUnicode( lString8( pfn ) );
                    item->m_size = data.nFileSizeLow;
                    item->m_flags = data.dwFileAttributes;
                    dir->Add(item);
                }

                if (!FindNextFileA(hFind, &dataa)) {
                    // end of list
                    break;
                }

            }
        }

        FindClose( hFind );
#else
        // POSIX
        lString16 p( fn );
        p.erase( p.length()-1, 1 );
        lString8 p8 = UnicodeToLocal( p );
        if ( p8.empty() )
            p8 = ".";
        const char * p8s = p8.c_str();
        DIR * d = opendir(p8s);
        if ( d ) {
            struct dirent * pde;
            while ( (pde = readdir(d))!=NULL ) {
                lString8 fpath = p8 + "/" + pde->d_name;
                struct stat st;
                stat( fpath.c_str(), &st );
                if ( S_ISDIR(st.st_mode) ) {
                    // dir
                    if ( strcmp(pde->d_name, ".") && strcmp(pde->d_name, "..") ) {
                        // normal directory
                        LVDirectoryContainerItemInfo * item = new LVDirectoryContainerItemInfo;
                        item->m_name = LocalToUnicode(lString8(pde->d_name));
                        item->m_is_container = true;
                        dir->Add(item);
                    }
                } else if ( S_ISREG(st.st_mode) ) {
                    // file
                    LVDirectoryContainerItemInfo * item = new LVDirectoryContainerItemInfo;
                    item->m_name = LocalToUnicode(lString8(pde->d_name));
                    item->m_size = st.st_size;
                    item->m_flags = st.st_mode;
                    dir->Add(item);
                }
            }
            closedir(d);
        } else {
            delete dir;
            return NULL;
        }


#endif
        return dir;
    }
};

class LVCachedStream : public LVNamedStream
{
private:

    #define CACHE_BUF_BLOCK_SHIFT 12
    #define CACHE_BUF_BLOCK_SIZE (1<<CACHE_BUF_BLOCK_SHIFT)
    class BufItem
    {
    public:
        lUInt32   start;
        lUInt32   size;
        BufItem * prev;
        BufItem * next;
        lUInt8    buf[CACHE_BUF_BLOCK_SIZE];

        int getIndex() { return start >> CACHE_BUF_BLOCK_SHIFT; }
        BufItem() : prev(NULL), next(NULL) { }
    };

    LVStreamRef m_stream;
    int m_bufSize;
    lvsize_t    m_size;
    lvpos_t     m_pos;
    BufItem * * m_buf;
    BufItem *   m_head;
    BufItem *   m_tail;
    int         m_bufItems;
    int         m_bufLen;

    /// add item to head
    BufItem * addNewItem( int start )
    {
        //
        int index = (start >> CACHE_BUF_BLOCK_SHIFT);
        BufItem * item = new BufItem();
        if (!m_head)
        {
            m_head = m_tail = item;
        }
        else
        {
            item->next = m_head;
            m_head->prev = item;
            m_head = item;
        }
        item->start = start;
        int sz = CACHE_BUF_BLOCK_SIZE;
        if ( start + sz > (int)m_size )
            sz = (int)(m_size - start);
        item->size = sz;
        m_buf[ index ] = item;
        m_bufLen++;
        assert( !(m_head && !m_tail) );
        return item;
    }
    /// move item to top
    void moveToTop( int index )
    {
        BufItem * item = m_buf[index];
        if ( !item || m_head == item )
            return;
        if ( m_tail == item )
            m_tail = item->prev;
        if ( item->next )
            item->next->prev = item->prev;
        if ( item->prev )
            item->prev->next = item->next;
        m_head->prev = item;
        item->next = m_head;
        item->prev = NULL;
        m_head = item;
        assert( !(m_head && !m_tail) );
    }
    /// reuse existing item from tail of list
    BufItem * reuseItem( int start )
    {
        //
        int rem_index = m_tail->start >> CACHE_BUF_BLOCK_SHIFT;
        if (m_tail->prev)
            m_tail->prev->next = NULL;
        m_tail = m_tail->prev;
        BufItem * item = m_buf[rem_index];
        m_buf[ rem_index ] = NULL;
        int index = (start >> CACHE_BUF_BLOCK_SHIFT);
        m_buf[ index ] = item;
        item->start = start;
        int sz = CACHE_BUF_BLOCK_SIZE;
        if ( start + sz > (int)m_size )
            sz = (int)(m_size - start);
        item->size = sz;
        item->next = m_head;
        item->prev = NULL;
        m_head->prev = item;
        m_head = item;
        assert( !(m_head && !m_tail) );
        return item;
    }
    /// read item content from base stream
    bool fillItem( BufItem * item )
    {
        if ( m_stream->SetPos( item->start )==(lvpos_t)(~0) )
            return false;

        lvsize_t bytesRead;
        if ( m_stream->Read( item->buf, item->size, &bytesRead )!=LVERR_OK || bytesRead!=item->size )
            return false;
        return true;
    }
    BufItem * addOrReuseItem( int start )
    {
        //assert( !(m_head && !m_tail) );
        if ( m_bufLen < m_bufSize )
            return addNewItem( start );
        else
            return reuseItem( start );
    }
    /// checks several items, loads if necessary
    bool fillFragment( int startIndex, int count )
    {
        if (count<=0 || startIndex<0 || startIndex+count>m_bufItems)
        {
            return false;
        }
        int firstne = -1;
        int lastne = -1;
        int i;
        for ( i=startIndex; i<startIndex+count; i++)
        {
            if ( m_buf[i] )
            {
                moveToTop( i );
            }
            else
            {
                if (firstne == -1)
                    firstne = i;
                lastne = i;
            }
        }
        if ( firstne<0 )
            return true;
        for ( i=firstne; i<=lastne; i++)
        {
            if ( !m_buf[i] )
            {
                BufItem * item = addOrReuseItem( i << CACHE_BUF_BLOCK_SHIFT );
                if ( !fillItem ( item ) )
                    return false;
            }
            else
            {
                moveToTop( i );
            }
        }
        return true;
    }
public:

    LVCachedStream( LVStreamRef stream, int bufSize ) : m_stream(stream), m_pos(0),
            m_head(NULL), m_tail(NULL), m_bufLen(0)
    {
        m_size = m_stream->GetSize();
        m_bufItems = (int)((m_size + CACHE_BUF_BLOCK_SIZE - 1) >> CACHE_BUF_BLOCK_SHIFT);
        if (!m_bufItems)
            m_bufItems = 1;
        m_bufSize = (bufSize + CACHE_BUF_BLOCK_SIZE - 1) >> CACHE_BUF_BLOCK_SHIFT;
        if (m_bufSize<3)
            m_bufSize = 3;
        m_buf = new BufItem* [m_bufItems];
        memset( m_buf, 0, sizeof( BufItem*) * m_bufItems );
        SetName( stream->GetName() );
    }
    virtual ~LVCachedStream()
    {
        if (m_buf)
        {
            for (int i=0; i<m_bufItems; i++)
                if (m_buf[i])
                    delete m_buf[i];
            delete[] m_buf;
        }
    }

    /// fastly return already known CRC
    virtual lverror_t crc32( lUInt32 & dst )
    {
        return m_stream->crc32( dst );
    }

    virtual bool Eof()
    {
        return m_pos >= m_size;
    }
    virtual lvsize_t  GetSize()
    {
        return m_size;
    }

    virtual lverror_t Seek(lvoffset_t offset, lvseek_origin_t origin, lvpos_t* newPos)
    {
        lvpos_t npos = 0;
        lvpos_t currpos = m_pos;
        switch (origin) {
        case LVSEEK_SET:
            npos = offset;
            break;
        case LVSEEK_CUR:
            npos = currpos + offset;
            break;
        case LVSEEK_END:
            npos = m_size + offset;
            break;
        }
        if (npos > m_size)
            return LVERR_FAIL;
        m_pos = npos;
        if (newPos)
        {
            *newPos =  m_pos;
        }
        return LVERR_OK;
    }

    virtual lverror_t Write(const void*, lvsize_t, lvsize_t*)
    {
        return LVERR_NOTIMPL;
    }

    virtual lverror_t Read(void* buf, lvsize_t size, lvsize_t* pBytesRead)
    {
        if ( m_pos + size > m_size )
            size = m_size - m_pos;
        if ( size <= 0 ) {
            if ( pBytesRead )
                *pBytesRead = 0;
            return LVERR_FAIL;
        }
        int startIndex = (int)(m_pos >> CACHE_BUF_BLOCK_SHIFT);
        int endIndex = (int)((m_pos + size - 1) >> CACHE_BUF_BLOCK_SHIFT);
        int count = endIndex - startIndex + 1;
        int extraItems = (m_bufSize - count); // max move backward
        if (extraItems<0)
            extraItems = 0;
        char * flags = new char[ count ];
        memset( flags, 0, count );

        //if ( m_stream
        int start = (int)m_pos;
        lUInt8 * dst = (lUInt8 *) buf;
        int dstsz = (int)size;
        int i;
        int istart = start & (CACHE_BUF_BLOCK_SIZE - 1);
        for ( i=startIndex; i<=endIndex; i++ )
        {
            BufItem * item = m_buf[i];
            if (item)
            {
                int isz = item->size - istart;
                if (isz > dstsz)
                    isz = dstsz;
                memcpy( dst, item->buf + istart, isz );
                flags[i - startIndex] = 1;
            }
            dstsz -= CACHE_BUF_BLOCK_SIZE - istart;
            dst += CACHE_BUF_BLOCK_SIZE - istart;
            istart = 0;
        }

        dst = (lUInt8 *) buf;

        bool flgFirstNE = true;
        istart = start & (CACHE_BUF_BLOCK_SIZE - 1);
        dstsz = (int)size;
        for ( i=startIndex; i<=endIndex; i++ )
        {
            if (!flags[ i - startIndex])
            {
                if ( !m_buf[i] )
                {
                    int fillStart = i;
                    if ( flgFirstNE )
                    {
                        fillStart -= extraItems;
                    }
                    if (fillStart<0)
                        fillStart = 0;
                    int fillEnd = fillStart + m_bufSize - 1;
                    if (fillEnd>endIndex)
                        fillEnd = endIndex;
                    bool res = fillFragment( fillStart, fillEnd - fillStart + 1 );
                    if ( !res )
                    {
                        fprintf( stderr, "cannot fill fragment %d .. %d\n", fillStart, fillEnd );
                        exit(-1);
                    }
                    flgFirstNE = false;
                }
                BufItem * item = m_buf[i];
                int isz = item->size - istart;
                if (isz > dstsz)
                    isz = dstsz;
                memcpy( dst, item->buf + istart, isz );
            }
            dst += CACHE_BUF_BLOCK_SIZE - istart;
            dstsz -= CACHE_BUF_BLOCK_SIZE - istart;
            istart = 0;
        }
        delete[] flags;

        lvsize_t bytesRead = size;
        if ( m_pos + size > m_size )
            bytesRead = m_size - m_pos;
        m_pos += bytesRead;
        if (pBytesRead)
            *pBytesRead = bytesRead;
        return LVERR_OK;
    }

    virtual lverror_t SetSize(lvsize_t)
    {
        return LVERR_NOTIMPL;
    }
};


#pragma pack(push, 1)
typedef struct {
    lUInt32  Mark;      // 0
    lUInt8   UnpVer;    // 4
    lUInt8   UnpOS;     // 5
    lUInt16  Flags;     // 6
    lUInt16  others[11]; //
    //lUInt16  Method;    // 8
    //lUInt32  ftime;     // A
    //lUInt32  CRC;       // E
    //lUInt32  PackSize;  //12
    //lUInt32  UnpSize;   //16
    //lUInt16  NameLen;   //1A
    //lUInt16  AddLen;    //1C

    lUInt16  getMethod() { return others[0]; }    // 8
    lUInt32  getftime() { return others[1] | ( (lUInt32)others[2] << 16); }     // A
    lUInt32  getCRC() { return others[3] | ( (lUInt32)others[4] << 16); }       // E
    lUInt32  getPackSize() { return others[5] | ( (lUInt32)others[6] << 16); }  //12
    lUInt32  getUnpSize() { return others[7] | ( (lUInt32)others[8] << 16); }   //16
    lUInt16  getNameLen() { return others[9]; }   //1A
    lUInt16  getAddLen() { return others[10]; }    //1C
    void byteOrderConv()
    {
        //
        lvByteOrderConv cnv;
        if ( cnv.msf() )
        {
            cnv.rev( &Mark );
            cnv.rev( &Flags );
            for ( int i=0; i<11; i++) {
                cnv.rev( &others[i] );
            }
            //cnv.rev( &Method );
            //cnv.rev( &ftime );
            //cnv.rev( &CRC );
            //cnv.rev( &PackSize );
            //cnv.rev( &UnpSize );
            //cnv.rev( &NameLen );
            //cnv.rev( &AddLen );
        }
    }
} ZipLocalFileHdr;
#pragma pack(pop)

#pragma pack(push, 1)
struct ZipHd2
{
    lUInt32  Mark;      // 0
    lUInt8   PackVer;   // 4
    lUInt8   PackOS;
    lUInt8   UnpVer;
    lUInt8   UnpOS;
    lUInt16     Flags;  // 8
    lUInt16     Method; // A
    lUInt32    ftime;   // C
    lUInt32    CRC;     // 10
    lUInt32    PackSize;// 14
    lUInt32    UnpSize; // 18
    lUInt16     NameLen;// 1C
    lUInt16     AddLen; // 1E
    lUInt16     CommLen;// 20
    lUInt16     DiskNum;// 22
    //lUInt16     ZIPAttr;// 24
    //lUInt32     Attr;   // 26
    //lUInt32     Offset; // 2A
    lUInt16     _Attr_and_Offset[5];   // 24
    lUInt16     getZIPAttr() { return _Attr_and_Offset[0]; }
    lUInt32     getAttr() { return _Attr_and_Offset[1] | ((lUInt32)_Attr_and_Offset[2]<<16); }
    lUInt32     getOffset() { return _Attr_and_Offset[3] | ((lUInt32)_Attr_and_Offset[4]<<16); }
    void byteOrderConv()
    {
        //
        lvByteOrderConv cnv;
        if ( cnv.msf() )
        {
            cnv.rev( &Mark );
            cnv.rev( &Flags );
            cnv.rev( &Method );
            cnv.rev( &ftime );
            cnv.rev( &CRC );
            cnv.rev( &PackSize );
            cnv.rev( &UnpSize );
            cnv.rev( &NameLen );
            cnv.rev( &AddLen );
            cnv.rev( &CommLen );
            cnv.rev( &DiskNum );
            cnv.rev( &_Attr_and_Offset[0] );
            cnv.rev( &_Attr_and_Offset[1] );
            cnv.rev( &_Attr_and_Offset[2] );
            cnv.rev( &_Attr_and_Offset[3] );
            cnv.rev( &_Attr_and_Offset[4] );
        }
    }
};
#pragma pack(pop)

#define ARC_INBUF_SIZE  8192
#define ARC_OUTBUF_SIZE 16384

#if (USE_ZLIB==1)

class LVZipDecodeStream : public LVNamedStream
{
private:
    LVStreamRef m_stream;
    lvsize_t    m_start;
    lvsize_t    m_packsize;
    lvsize_t    m_unpacksize;
    z_stream_s  m_zstream;
    lvpos_t     m_inbytesleft; // bytes left
    lvpos_t     m_outbytesleft;
    bool        m_zInitialized;
    int         m_decodedpos;
    lUInt8 *    m_inbuf;
    lUInt8 *    m_outbuf;
    lUInt32     m_CRC;
    lUInt32     m_originalCRC;


    LVZipDecodeStream( LVStreamRef stream, lvsize_t start, lvsize_t packsize, lvsize_t unpacksize, lUInt32 crc )
        : m_stream(stream), m_start(start), m_packsize(packsize), m_unpacksize(unpacksize),
        m_inbytesleft(0), m_outbytesleft(0), m_zInitialized(false), m_decodedpos(0),
        m_inbuf(NULL), m_outbuf(NULL), m_CRC(0), m_originalCRC(crc)
    {
        m_inbuf = new lUInt8[ARC_INBUF_SIZE];
        m_outbuf = new lUInt8[ARC_OUTBUF_SIZE];
        rewind();
    }

    ~LVZipDecodeStream()
    {
        zUninit();
        if (m_inbuf)
            delete[] m_inbuf;
        if (m_outbuf)
            delete[] m_outbuf;
    }

    /// Get stream open mode
    /** \return lvopen_mode_t open mode */
    virtual lvopen_mode_t GetMode()
    {
        return LVOM_READ;
    }

    void zUninit()
    {
        if (!m_zInitialized)
            return;
        inflateEnd(&m_zstream);
        m_zInitialized = false;
    }

    /// Fill input buffer: returns -1 if fails.
    int fillInBuf()
    {
        if (m_zstream.avail_in < ARC_INBUF_SIZE / 4 && m_inbytesleft > 0)
        {
            int inpos = m_zstream.next_in ? (m_zstream.next_in - m_inbuf) : 0;
            if ( inpos > ARC_INBUF_SIZE/2 )
            {
                // move rest of data to beginning of buffer
                for ( int i=0; i<(int)m_zstream.avail_in; i++)
                    m_inbuf[i] = m_inbuf[ i+inpos ];
                m_zstream.next_in = m_inbuf;
                inpos = 0;
            }
            int tailpos = inpos + m_zstream.avail_in;
            int bytes_to_read = ARC_INBUF_SIZE - tailpos;
            if ( bytes_to_read > (int)m_inbytesleft )
                bytes_to_read = (int)m_inbytesleft;
            if (bytes_to_read > 0)
            {
                lvsize_t bytesRead = 0;
                if ( m_stream->Read( m_inbuf + tailpos, bytes_to_read, &bytesRead ) != LVERR_OK )
                {
                    // read error
                    m_zstream.avail_in = 0;
                    return -1;
                }
                m_CRC = lStr_crc32( m_CRC, m_inbuf + tailpos, (int)(bytesRead) );
                m_zstream.avail_in += (int)bytesRead;
                m_inbytesleft -= bytesRead;
            }
            else
            {
                //check CRC
                if ( m_CRC != m_originalCRC )
                    return -1; // CRC error
            }
        }
        return m_zstream.avail_in;
    }

    bool rewind()
    {
        zUninit();
        // stream
        m_stream->SetPos( 0 );

        m_CRC = 0;
        memset( &m_zstream, 0, sizeof(m_zstream) );
        // inbuf
        m_inbytesleft = m_packsize;
        m_zstream.next_in = m_inbuf;
        fillInBuf();
        // outbuf
        m_zstream.next_out = m_outbuf;
        m_zstream.avail_out = ARC_OUTBUF_SIZE;
        m_decodedpos = 0;
        m_outbytesleft = m_unpacksize;
        // Z
        if ( inflateInit2( &m_zstream, -15 ) != Z_OK )
        {
            return false;
        }
        m_zInitialized = true;
        return true;
    }
    // returns count of available decoded bytes in buffer
    inline int getAvailBytes()
    {
        return m_zstream.next_out - m_outbuf - m_decodedpos;
    }
    /// decode next portion of data, returns number of decoded bytes available, -1 if error
    int decodeNext()
    {
        int avail = getAvailBytes();
        if (avail>0)
            return avail;
        // fill in buffer
        int in_bytes = fillInBuf();
        if (in_bytes<0)
            return -1;
        // reserve space for output
        if (m_zstream.avail_out < ARC_OUTBUF_SIZE / 4 && m_outbytesleft > 0)
        {

            int outpos = m_zstream.next_out - m_outbuf;
            if ( outpos > ARC_OUTBUF_SIZE*3/4 )
            {
                // move rest of data to beginning of buffer
                for ( int i=(int)m_decodedpos; i<outpos; i++)
                    m_inbuf[i - m_decodedpos] = m_inbuf[ i ];
                m_zstream.next_out -= m_decodedpos;
                outpos -= m_decodedpos;
                m_decodedpos = 0;
                m_zstream.avail_out = ARC_OUTBUF_SIZE - outpos;
            }
        }
        int res = inflate( &m_zstream, m_inbytesleft > 0 ? Z_NO_FLUSH : Z_FINISH );
        if (res == Z_STREAM_ERROR)
        {
            return -1;
        }
        avail = getAvailBytes();
        return avail;
    }
    /// skip bytes from out stream
    bool skip( int bytesToSkip )
    {
        while (bytesToSkip > 0)
        {
            int avail = decodeNext();

            if (avail < 0)
            {
                return false; // error
            }
            else if (avail==0)
            {
                return true;
            }

            if (avail >= bytesToSkip)
                avail = bytesToSkip;

            m_decodedpos += avail;
            m_outbytesleft -= avail;
            bytesToSkip -= avail;
        }
        if (bytesToSkip == 0)
            return true;
        return false;
    }
    /// decode bytes
    int read( lUInt8 * buf, int bytesToRead )
    {
        int bytesRead = 0;
        //
        while (bytesToRead > 0)
        {
            int avail = decodeNext();

            if (avail < 0)
            {
                return -1; // error
            }
            else if (avail==0)
            {
                return bytesRead;
            }

            if (avail >= bytesToRead)
                avail = bytesToRead;

            // copy data
            lUInt8 * src = m_outbuf + m_decodedpos;
            for (int i=avail; i>0; --i)
                *buf++ = *src++;

            m_decodedpos += avail;
            m_outbytesleft -= avail;
            bytesRead += avail;
            bytesToRead -= avail;
        }
        return bytesRead;
    }
public:

    /// fastly return already known CRC
    virtual lverror_t crc32( lUInt32 & dst )
    {
        dst = m_originalCRC;
        return LVERR_OK;
    }

    virtual bool Eof()
    {
        return m_outbytesleft==0; //m_pos >= m_size;
    }
    virtual lvsize_t  GetSize()
    {
        return m_unpacksize;
    }
    virtual lvpos_t GetPos()
    {
        return m_unpacksize - m_outbytesleft;
    }
    virtual lverror_t GetPos( lvpos_t * pos )
    {
        if (pos)
            *pos = m_unpacksize - m_outbytesleft;
        return LVERR_OK;
    }
    virtual lverror_t Seek(lvoffset_t offset, lvseek_origin_t origin, lvpos_t* newPos)
    {
        lvpos_t npos = 0;
        lvpos_t currpos = GetPos();
        switch (origin) {
        case LVSEEK_SET:
            npos = offset;
            break;
        case LVSEEK_CUR:
            npos = currpos + offset;
            break;
        case LVSEEK_END:
            npos = m_unpacksize + offset;
            break;
        }
        if (npos > m_unpacksize)
            return LVERR_FAIL;
        if ( npos != currpos )
        {
            if (npos < currpos)
            {
                if ( !rewind() || !skip((int)npos) )
                    return LVERR_FAIL;
            }
            else
            {
                skip( (int)(npos - currpos) );
            }
        }
        if (newPos)
            *newPos = npos;
        return LVERR_OK;
    }
    virtual lverror_t Write(const void*, lvsize_t, lvsize_t*)
    {
        return LVERR_NOTIMPL;
    }
    virtual lverror_t Read(void* buf, lvsize_t count, lvsize_t* bytesRead)
    {
        int readBytes = read( (lUInt8 *)buf, (int)count );
        if ( readBytes<0 )
            return LVERR_FAIL;
        if (bytesRead)
            *bytesRead = (lvsize_t)readBytes;
        return LVERR_OK;
    }
    virtual lverror_t SetSize(lvsize_t)
    {
        return LVERR_NOTIMPL;
    }
    static LVStream * Create( LVStreamRef stream, lvpos_t pos, lString16 name, lUInt32 srcPackSize, lUInt32 srcUnpSize )
    {
        ZipLocalFileHdr hdr;
        unsigned hdr_size = 0x1E; //sizeof(hdr);
        if ( stream->Seek( pos, LVSEEK_SET, NULL )!=LVERR_OK )
            return NULL;
        lvsize_t sz = 0;
        if ( stream->Read( &hdr, hdr_size, &sz)!=LVERR_OK || sz!=hdr_size )
            return NULL;
        hdr.byteOrderConv();
        pos += 0x1e + hdr.getNameLen() + hdr.getAddLen();
        if ( stream->Seek( pos, LVSEEK_SET, NULL )!=LVERR_OK )
            return NULL;
        lUInt32 packSize = hdr.getPackSize();
        lUInt32 unpSize = hdr.getUnpSize();
        if ( packSize==0 && unpSize==0 ) {
            packSize = srcPackSize;
            unpSize = srcUnpSize;
        }
        if ((lvpos_t)(pos + packSize) > (lvpos_t)stream->GetSize())
            return NULL;
        if (hdr.getMethod() == 0)
        {
            // store method, copy as is
            if ( hdr.getPackSize() != hdr.getUnpSize() )
                return NULL;
            LVStreamFragment * fragment = new LVStreamFragment( stream, pos, hdr.getPackSize());
            fragment->SetName( name.c_str() );
            return fragment;
        }
        else if (hdr.getMethod() == 8)
        {
            // deflate
            LVStreamRef srcStream( new LVStreamFragment( stream, pos, hdr.getPackSize()) );
            LVZipDecodeStream * res = new LVZipDecodeStream( srcStream, pos,
                packSize, unpSize, hdr.getCRC() );
            res->SetName( name.c_str() );
            return res;
        }
        else
            return NULL;
    }
};

class LVZipArc : public LVArcContainerBase
{
public:
    virtual LVStreamRef OpenStream( const wchar_t * fname, lvopen_mode_t mode )
    {
        if ( fname[0]=='/' )
            fname++;
        int found_index = -1;
        for (int i=0; i<m_list.length(); i++) {
            if ( !lStr_cmp( fname, m_list[i]->GetName() ) ) {
                if ( m_list[i]->IsContainer() ) {
                    // found directory with same name!!!
                    return LVStreamRef();
                }
                found_index = i;
                break;
            }
        }
        if (found_index<0)
            return LVStreamRef(); // not found
        // make filename
        lString16 fn = fname;
        LVStreamRef strm = m_stream; // fix strange arm-linux-g++ bug
        LVStreamRef stream(
		LVZipDecodeStream::Create(
			strm,
			m_list[found_index]->GetSrcPos(),
            fn,
            m_list[found_index]->GetSrcSize(),
            m_list[found_index]->GetSize() )
        );
        if (!stream.isNull()) {
            stream->SetName(m_list[found_index]->GetName());
            return LVCreateBufferedStream( stream, ZIP_STREAM_BUFFER_SIZE );
        }
        return stream;
    }
    LVZipArc( LVStreamRef stream ) : LVArcContainerBase(stream)
    {
    }
    virtual ~LVZipArc()
    {
    }
    virtual int ReadContents()
    {
        lvByteOrderConv cnv;
        bool arcComment = false;
        bool truncated = false;

        m_list.clear();
        if (!m_stream || m_stream->Seek(0, LVSEEK_SET, NULL)!=LVERR_OK)
            return 0;

        SetName( m_stream->GetName() );


        lvsize_t sz = 0;
        if (m_stream->GetSize( &sz )!=LVERR_OK)
                return 0;
        lvsize_t m_FileSize = (unsigned)sz;

        char ReadBuf[1024];
        lUInt32 NextPosition;
        lvpos_t CurPos;
        lvsize_t ReadSize;
        int Buf;
        bool found = false;
        CurPos=NextPosition=(int)m_FileSize;
        if (CurPos < sizeof(ReadBuf)-18)
            CurPos = 0;
        else
            CurPos -= sizeof(ReadBuf)-18;
        for ( Buf=0; Buf<64 && !found; Buf++ )
        {
            //SetFilePointer(ArcHandle,CurPos,NULL,FILE_BEGIN);
            m_stream->Seek( CurPos, LVSEEK_SET, NULL );
            m_stream->Read( ReadBuf, sizeof(ReadBuf), &ReadSize);
            if (ReadSize==0)
                break;
            for (int I=(int)ReadSize-4;I>=0;I--)
            {
                if (ReadBuf[I]==0x50 && ReadBuf[I+1]==0x4b && ReadBuf[I+2]==0x05 &&
                    ReadBuf[I+3]==0x06)
                {
                    m_stream->Seek( CurPos+I+16, LVSEEK_SET, NULL );
                    m_stream->Read( &NextPosition, sizeof(NextPosition), &ReadSize);
		    		cnv.lsf( &NextPosition );
                    found=true;
                    break;
                }
            }
            if (CurPos==0)
                break;
            if (CurPos<sizeof(ReadBuf)-4)
                CurPos=0;
            else
                CurPos-=sizeof(ReadBuf)-4;
        }

        truncated = !found;
        if (truncated)
            NextPosition=0;

        //================================================================
        // get files


        ZipLocalFileHdr ZipHd1;
        ZipHd2 ZipHeader;
        unsigned ZipHeader_size = 0x2E; //sizeof(ZipHd2); //0x34; //
        unsigned ZipHd1_size = 0x1E; //sizeof(ZipHd1); //sizeof(ZipHd1)
          //lUInt32 ReadSize;

        while (1) {

            if (m_stream->Seek( NextPosition, LVSEEK_SET, NULL )!=LVERR_OK)
                return 0;

            if (truncated)
            {
                m_stream->Read( &ZipHd1, ZipHd1_size, &ReadSize);
                ZipHd1.byteOrderConv();

                //ReadSize = fread(&ZipHd1, 1, sizeof(ZipHd1), f);
                if (ReadSize != ZipHd1_size) {
                        //fclose(f);
                    if (ReadSize==0 && NextPosition==m_FileSize)
                        return m_list.length();
                    if ( ReadSize==0 )
                        return m_list.length();
                    return 0;
                }

                memset(&ZipHeader,0,ZipHeader_size);

                ZipHeader.UnpVer=ZipHd1.UnpVer;
                ZipHeader.UnpOS=ZipHd1.UnpOS;
                ZipHeader.Flags=ZipHd1.Flags;
                ZipHeader.ftime=ZipHd1.getftime();
                ZipHeader.PackSize=ZipHd1.getPackSize();
                ZipHeader.UnpSize=ZipHd1.getUnpSize();
                ZipHeader.NameLen=ZipHd1.getNameLen();
                ZipHeader.AddLen=ZipHd1.getAddLen();
                ZipHeader.Method=ZipHd1.getMethod();
            } else {

                m_stream->Read( &ZipHeader, ZipHeader_size, &ReadSize);

                ZipHeader.byteOrderConv();
                    //ReadSize = fread(&ZipHeader, 1, sizeof(ZipHeader), f);
                if (ReadSize!=ZipHeader_size) {
                            if (ReadSize>16 && ZipHeader.Mark==0x06054B50 ) {
                                    break;
                            }
                            //fclose(f);
                            return 0;
                }
            }

            if (ReadSize==0 || ZipHeader.Mark==0x06054b50 ||
                    (truncated && ZipHeader.Mark==0x02014b50) )
            {
                if (!truncated && *(lUInt16 *)((char *)&ZipHeader+20)!=0)
                    arcComment=true;
                break; //(GETARC_EOF);
            }

            const int NM = 513;
            lUInt32 SizeToRead=(ZipHeader.NameLen<NM) ? ZipHeader.NameLen : NM;
            char fnbuf[1025];
            m_stream->Read( fnbuf, SizeToRead, &ReadSize);

            if (ReadSize!=SizeToRead) {
                return 0;
            }

            fnbuf[ZipHeader.NameLen]=0;

            long SeekLen=ZipHeader.AddLen+ZipHeader.CommLen;

            LVCommonContainerItemInfo * item = new LVCommonContainerItemInfo();

            if (truncated)
                SeekLen+=ZipHeader.PackSize;

            NextPosition = (lUInt32)m_stream->GetPos();
            NextPosition += SeekLen;
            m_stream->Seek(NextPosition, LVSEEK_SET, NULL);

	        // {"DOS","Amiga","VAX/VMS","Unix","VM/CMS","Atari ST",
			//  "OS/2","Mac-OS","Z-System","CP/M","TOPS-20",
			//  "Win32","SMS/QDOS","Acorn RISC OS","Win32 VFAT","MVS",
			//  "BeOS","Tandem"};
            const lChar16 * enc_name = (ZipHeader.PackOS==0) ? L"cp866" : L"cp1251";
            const lChar16 * table = GetCharsetByte2UnicodeTable( enc_name );
            lString16 fName = ByteToUnicode( lString8(fnbuf), table );

            item->SetItemInfo(fName.c_str(), ZipHeader.UnpSize, (ZipHeader.getAttr() & 0x3f));
            item->SetSrc( ZipHeader.getOffset(), ZipHeader.PackSize, ZipHeader.Method );
            m_list.add(item);
        }
        int sz2 = m_list.length();
        return sz2;
    }

    static LVArcContainerBase * OpenArchieve( LVStreamRef stream )
    {
        // read beginning of file
        const lvsize_t hdrSize = 4;
        char hdr[hdrSize];
        stream->SetPos(0);
        lvsize_t bytesRead = 0;
        if (stream->Read(hdr, hdrSize, &bytesRead)!=LVERR_OK || bytesRead!=hdrSize)
                return NULL;
        stream->SetPos(0);
        // detect arc type
        if (hdr[0]!='P' || hdr[1]!='K' || hdr[2]!=3 || hdr[3]!=4)
                return NULL;
        LVZipArc * arc = new LVZipArc( stream );
        int itemCount = arc->ReadContents();
        if ( itemCount <= 0 )
        {
            delete arc;
            return NULL;
        }
        return arc;
    }

};
#endif

#if (USE_UNRAR==1)

// unrar dll header
#define ERAR_END_ARCHIVE     10
#define ERAR_NO_MEMORY       11
#define ERAR_BAD_DATA        12
#define ERAR_BAD_ARCHIVE     13
#define ERAR_UNKNOWN_FORMAT  14
#define ERAR_EOPEN           15
#define ERAR_ECREATE         16
#define ERAR_ECLOSE          17
#define ERAR_EREAD           18
#define ERAR_EWRITE          19
#define ERAR_SMALL_BUF       20
#define ERAR_UNKNOWN         21

#define RAR_OM_LIST           0
#define RAR_OM_EXTRACT        1

#define RAR_SKIP              0
#define RAR_TEST              1
#define RAR_EXTRACT           2

#define RAR_VOL_ASK           0
#define RAR_VOL_NOTIFY        1

#define RAR_DLL_VERSION       4

struct RARHeaderData
{
  char         ArcName[260];
  char         FileName[260];
  unsigned int Flags;
  unsigned int PackSize;
  unsigned int UnpSize;
  unsigned int HostOS;
  unsigned int FileCRC;
  unsigned int FileTime;
  unsigned int UnpVer;
  unsigned int Method;
  unsigned int FileAttr;
  char         *CmtBuf;
  unsigned int CmtBufSize;
  unsigned int CmtSize;
  unsigned int CmtState;
};


struct RARHeaderDataEx
{
  char         ArcName[1024];
  wchar_t      ArcNameW[1024];
  char         FileName[1024];
  wchar_t      FileNameW[1024];
  unsigned int Flags;
  unsigned int PackSize;
  unsigned int PackSizeHigh;
  unsigned int UnpSize;
  unsigned int UnpSizeHigh;
  unsigned int HostOS;
  unsigned int FileCRC;
  unsigned int FileTime;
  unsigned int UnpVer;
  unsigned int Method;
  unsigned int FileAttr;
  char         *CmtBuf;
  unsigned int CmtBufSize;
  unsigned int CmtSize;
  unsigned int CmtState;
  unsigned int Reserved[1024];
};


struct RAROpenArchiveData
{
  char         *ArcName;
  unsigned int OpenMode;
  unsigned int OpenResult;
  char         *CmtBuf;
  unsigned int CmtBufSize;
  unsigned int CmtSize;
  unsigned int CmtState;
};

struct RAROpenArchiveDataEx
{
  char         *ArcName;
  wchar_t      *ArcNameW;
  unsigned int OpenMode;
  unsigned int OpenResult;
  char         *CmtBuf;
  unsigned int CmtBufSize;
  unsigned int CmtSize;
  unsigned int CmtState;
  unsigned int Flags;
  unsigned int Reserved[32];
};

enum UNRARCALLBACK_MESSAGES {
  UCM_CHANGEVOLUME,UCM_PROCESSDATA,UCM_NEEDPASSWORD
};

typedef int (CALLBACK *UNRARCALLBACK)(UINT msg,LONG UserData,LONG P1,LONG P2);

typedef int (PASCAL *CHANGEVOLPROC)(char *ArcName,int Mode);
typedef int (PASCAL *PROCESSDATAPROC)(unsigned char *Addr,int Size);

#if !defined(__SYMBIAN32__) && defined(_WIN32)
#undef PASCAL
#define PASCAL
#endif
class LVUnRarDll
{
#if !defined(__SYMBIAN32__) && defined(_WIN32)
    typedef HMODULE libtype;
#else
    typedef void libtype;
#endif
    libtype _lib;
    HANDLE _arc;
    HANDLE PASCAL (*RAROpenArchive)(struct RAROpenArchiveData *ArchiveData);
    HANDLE PASCAL (*RAROpenArchiveEx)(struct RAROpenArchiveDataEx *ArchiveData);
    int    PASCAL (*RARCloseArchive)(HANDLE hArcData);
    int    PASCAL (*RARReadHeader)(HANDLE hArcData,struct RARHeaderData *HeaderData);
    int    PASCAL (*RARReadHeaderEx)(HANDLE hArcData,struct RARHeaderDataEx *HeaderData);
    int    PASCAL (*RARProcessFile)(HANDLE hArcData,int Operation,char *DestPath,char  *DestName);
    int    PASCAL (*RARProcessFileW)(HANDLE hArcData,int Operation,wchar_t *DestPath,wchar_t *DestName);
    void   PASCAL (*RARSetCallback)(HANDLE hArcData,UNRARCALLBACK Callback,LONG UserData);
    void   PASCAL (*RARSetChangeVolProc)(HANDLE hArcData,CHANGEVOLPROC ChangeVolProc);
    void   PASCAL (*RARSetProcessDataProc)(HANDLE hArcData,PROCESSDATAPROC ProcessDataProc);
    void   PASCAL (*RARSetPassword)(HANDLE hArcData,char *Password);
    int    PASCAL (*RARGetDllVersion)();
public:
    void * getProc( const char * procName )
    {
#if !defined(__SYMBIAN32__) && defined(_WIN32)
        return LoadLibraryA( procName );
#else
        return dlsym( _lib, procName );
#endif
    }
    bool OpenArchive( const char * fname )
    {
        return false;
    }
    bool load( const char * libName )
    {
        if ( !_lib ) {
#if !defined(__SYMBIAN32__) && defined(_WIN32)
            _lib = LoadLibraryA( libName );
#else
            _lib = dlopen( libName, RTLD_NOW | RTLD_LOCAL );
#endif
        }
        if ( _lib ) {
            RAROpenArchive = (HANDLE PASCAL (*)(struct RAROpenArchiveData *ArchiveData)) getProc( "RAROpenArchive" );
            RAROpenArchiveEx = (HANDLE PASCAL (*)(struct RAROpenArchiveDataEx *ArchiveData)) getProc( "RAROpenArchiveEx" );
            RARCloseArchive = (int    PASCAL (*)(HANDLE hArcData)) getProc( "RARCloseArchive" );
            RARReadHeader = (int    PASCAL (*)(HANDLE hArcData,struct RARHeaderData *HeaderData)) getProc( "RARReadHeader" );
            RARReadHeaderEx = (int    PASCAL (*)(HANDLE hArcData,struct RARHeaderDataEx *HeaderData)) getProc( "RARReadHeaderEx" );
            RARProcessFile = (int    PASCAL (*)(HANDLE hArcData,int Operation,char *DestPath,char  *DestName)) getProc( "RARProcessFile" );
            RARProcessFileW = (int    PASCAL (*)(HANDLE hArcData,int Operation,wchar_t *DestPath,wchar_t *DestName)) getProc( "RARProcessFileW" );
            RARSetCallback = (void   PASCAL (*)(HANDLE hArcData,UNRARCALLBACK Callback,LONG UserData)) getProc( "RARSetCallback" );
            RARSetChangeVolProc = (void   PASCAL (*)(HANDLE hArcData,CHANGEVOLPROC ChangeVolProc)) getProc( "RARSetChangeVolProc" );
            RARSetProcessDataProc = (void   PASCAL (*)(HANDLE hArcData,PROCESSDATAPROC ProcessDataProc)) getProc( "RARSetProcessDataProc" );
            RARSetPassword = (void   PASCAL (*)(HANDLE hArcData,char *Password)) getProc( "RARSetPassword" );
            RARGetDllVersion = (int    PASCAL (*)()) getProc( "RARGetDllVersion" );
            if ( !RAROpenArchive || !RAROpenArchiveEx || !RARCloseArchive
                || !RARReadHeader || !RARReadHeaderEx || !RARProcessFile
                || !RARProcessFileW || !RARSetCallback || !RARSetChangeVolProc
                || !RARSetProcessDataProc || !RARSetPassword || !RARGetDllVersion )
                // not all functions found in library, fail
                unload();
        }
        return ( _lib!=NULL );
    }
    bool unload()
    {
        bool res = false;
        if ( _lib ) {
#if !defined(__SYMBIAN32__) && defined(_WIN32)
            FreeLibrary( _lib );
#else
            dlclose( _lib );
#endif
            res = true;
        }
        _lib = NULL;
        return res;
    }
    LVUnRarDll()
    : _lib(NULL) {
    }
    ~LVUnRarDll() {
        unload();
    }
};
#endif


#if 0 //(USE_UNRAR==1)
class LVRarArc : public LVArcContainerBase
{
    LVUnRarDll dll;
public:

    virtual LVStreamRef OpenStream( const wchar_t * fname, lvopen_mode_t mode )
    {
        int found_index = -1;
        for (int i=0; i<m_list.length(); i++) {
            if ( !lStr_cmp( fname, m_list[i]->GetName() ) ) {
                if ( m_list[i]->IsContainer() ) {
                    // found directory with same name!!!
                    return LVStreamRef();
                }
                found_index = i;
                break;
            }
        }
        if (found_index<0)
            return LVStreamRef(); // not found

        // TODO
        return LVStreamRef(); // not found
/*
        // make filename
        lString16 fn = fname;
        LVStreamRef strm = m_stream; // fix strange arm-linux-g++ bug
        LVStreamRef stream(
		LVZipDecodeStream::Create(
			strm,
			m_list[found_index]->GetSrcPos(), fn ) );
        if (!stream.isNull()) {
            return LVCreateBufferedStream( stream, ZIP_STREAM_BUFFER_SIZE );
        }
        stream->SetName(m_list[found_index]->GetName());
        return stream;
*/
    }
    LVRarArc( LVStreamRef stream ) : LVArcContainerBase(stream)
    {
    }
    virtual ~LVRarArc()
    {
    }

    virtual int ReadContents()
    {
        const char * dllName =
#if !defined(__SYMBIAN32__) && defined(_WIN32)
            "unrar.dll";
#else
            "unrar.so";
#endif
        if ( !dll.load( dllName ) )
            return 0; // DLL not found!

        lvByteOrderConv cnv;
        bool arcComment = false;
        bool truncated = false;

        m_list.clear();

        if (!m_stream || m_stream->Seek(0, LVSEEK_SET, NULL)!=LVERR_OK)
            return 0;

        SetName( m_stream->GetName() );


        lvsize_t sz = 0;
        if (m_stream->GetSize( &sz )!=LVERR_OK)
                return 0;
        lvsize_t m_FileSize = (unsigned)sz;

        char ReadBuf[1024];
        lUInt32 NextPosition;
        lvpos_t CurPos;
        lvsize_t ReadSize;
        int Buf;
        bool found = false;
        CurPos=NextPosition=(int)m_FileSize;
        if (CurPos < sizeof(ReadBuf)-18)
            CurPos = 0;
        else
            CurPos -= sizeof(ReadBuf)-18;
        for ( Buf=0; Buf<64 && !found; Buf++ )
        {
            //SetFilePointer(ArcHandle,CurPos,NULL,FILE_BEGIN);
            m_stream->Seek( CurPos, LVSEEK_SET, NULL );
            m_stream->Read( ReadBuf, sizeof(ReadBuf), &ReadSize);
            if (ReadSize==0)
                break;
            for (int I=(int)ReadSize-4;I>=0;I--)
            {
                if (ReadBuf[I]==0x50 && ReadBuf[I+1]==0x4b && ReadBuf[I+2]==0x05 &&
                    ReadBuf[I+3]==0x06)
                {
                    m_stream->Seek( CurPos+I+16, LVSEEK_SET, NULL );
                    m_stream->Read( &NextPosition, sizeof(NextPosition), &ReadSize);
		    		cnv.lsf( &NextPosition );
                    found=true;
                    break;
                }
            }
            if (CurPos==0)
                break;
            if (CurPos<sizeof(ReadBuf)-4)
                CurPos=0;
            else
                CurPos-=sizeof(ReadBuf)-4;
        }

        truncated = !found;
        if (truncated)
            NextPosition=0;

        //================================================================
        // get files


        ZipLocalFileHdr ZipHd1;
        ZipHd2 ZipHeader;
        unsigned ZipHeader_size = 0x2E; //sizeof(ZipHd2); //0x34; //
        unsigned ZipHd1_size = 0x1E; //sizeof(ZipHd1); //sizeof(ZipHd1)
          //lUInt32 ReadSize;

        while (1) {

            if (m_stream->Seek( NextPosition, LVSEEK_SET, NULL )!=LVERR_OK)
                return 0;

            if (truncated)
            {
                m_stream->Read( &ZipHd1, ZipHd1_size, &ReadSize);
                ZipHd1.byteOrderConv();

                //ReadSize = fread(&ZipHd1, 1, sizeof(ZipHd1), f);
                if (ReadSize != ZipHd1_size) {
                        //fclose(f);
                    if (ReadSize==0 && NextPosition==m_FileSize)
                        return m_list.length();
                    return 0;
                }

                memset(&ZipHeader,0,ZipHeader_size);

                ZipHeader.UnpVer=ZipHd1.UnpVer;
                ZipHeader.UnpOS=ZipHd1.UnpOS;
                ZipHeader.Flags=ZipHd1.Flags;
                ZipHeader.ftime=ZipHd1.getftime();
                ZipHeader.PackSize=ZipHd1.getPackSize();
                ZipHeader.UnpSize=ZipHd1.getUnpSize();
                ZipHeader.NameLen=ZipHd1.getNameLen();
                ZipHeader.AddLen=ZipHd1.getAddLen();
                ZipHeader.Method=ZipHd1.getMethod();
            } else {

                m_stream->Read( &ZipHeader, ZipHeader_size, &ReadSize);

                ZipHeader.byteOrderConv();
                    //ReadSize = fread(&ZipHeader, 1, sizeof(ZipHeader), f);
                if (ReadSize!=ZipHeader_size) {
                            if (ReadSize>16 && ZipHeader.Mark==0x06054B50 ) {
                                    break;
                            }
                            //fclose(f);
                            return 0;
                }
            }

            if (ReadSize==0 || ZipHeader.Mark==0x06054b50 ||
                    truncated && ZipHeader.Mark==0x02014b50)
            {
                if (!truncated && *(lUInt16 *)((char *)&ZipHeader+20)!=0)
                    arcComment=true;
                break; //(GETARC_EOF);
            }

            const int NM = 513;
            lUInt32 SizeToRead=(ZipHeader.NameLen<NM) ? ZipHeader.NameLen : NM;
            char fnbuf[1025];
            m_stream->Read( fnbuf, SizeToRead, &ReadSize);

            if (ReadSize!=SizeToRead) {
                return 0;
            }

            fnbuf[ZipHeader.NameLen]=0;

            long SeekLen=ZipHeader.AddLen+ZipHeader.CommLen;

            LVCommonContainerItemInfo * item = new LVCommonContainerItemInfo();

            if (truncated)
                SeekLen+=ZipHeader.PackSize;

            NextPosition = (lUInt32)m_stream->GetPos();
            NextPosition += SeekLen;
            m_stream->Seek(NextPosition, LVSEEK_SET, NULL);

	        // {"DOS","Amiga","VAX/VMS","Unix","VM/CMS","Atari ST",
			//  "OS/2","Mac-OS","Z-System","CP/M","TOPS-20",
			//  "Win32","SMS/QDOS","Acorn RISC OS","Win32 VFAT","MVS",
			//  "BeOS","Tandem"};
            const lChar16 * enc_name = (ZipHeader.PackOS==0) ? L"cp866" : L"cp1251";
            const lChar16 * table = GetCharsetByte2UnicodeTable( enc_name );
            lString16 fName = ByteToUnicode( lString8(fnbuf), table );

            item->SetItemInfo(fName.c_str(), ZipHeader.UnpSize, (ZipHeader.getAttr() & 0x3f));
            item->SetSrc( ZipHeader.getOffset(), ZipHeader.PackSize, ZipHeader.Method );
            m_list.add(item);
        }

        return m_list.length();
    }

    static LVArcContainerBase * OpenArchieve( LVStreamRef stream )
    {
        // read beginning of file
        const lvsize_t hdrSize = 4;
        char hdr[hdrSize];
        stream->SetPos(0);
        lvsize_t bytesRead = 0;
        if (stream->Read(hdr, hdrSize, &bytesRead)!=LVERR_OK || bytesRead!=hdrSize)
                return NULL;
        stream->SetPos(0);
        // detect arc type
        if (hdr[0]!='P' || hdr[1]!='K' || hdr[2]!=3 || hdr[3]!=4)
                return NULL;
        LVZipArc * arc = new LVZipArc( stream );
        int itemCount = arc->ReadContents();
        if ( itemCount <= 0 )
        {
            delete arc;
            return NULL;
        }
        return arc;
    }

};

class LVRarArc : public LVArcContainerBase
{
public:
    virtual LVStreamRef OpenStream( const wchar_t * fname, lvopen_mode_t mode )
    {
        int found_index = -1;
        for (int i=0; i<m_list.length(); i++) {
            if ( !lStr_cmp( fname, m_list[i]->GetName() ) ) {
                if ( m_list[i]->IsContainer() ) {
                    // found directory with same name!!!
                    return LVStreamRef();
                }
                found_index = i;
                break;
            }
        }
        if (found_index<0)
            return LVStreamRef(); // not found
        // make filename
        lString16 fn = fname;
        LVStreamRef strm = m_stream; // fix strange arm-linux-g++ bug
        LVStreamRef stream(
		LVRarDecodeStream::Create(
			strm,
			m_list[found_index]->GetSrcPos(), fn ) );
        if (!stream.isNull()) {
            return LVCreateBufferedStream( stream, ZIP_STREAM_BUFFER_SIZE );
        }
        stream->SetName(m_list[found_index]->GetName());
        return stream;
    }
    LVRarArc( LVStreamRef stream ) : LVArcContainerBase(stream)
    {
    }
    virtual ~LVRarArc()
    {
    }

    virtual int ReadContents()
    {
        lvByteOrderConv cnv;
        bool arcComment = false;
        bool truncated = false;

        m_list.clear();

        if (!m_stream || m_stream->Seek(0, LVSEEK_SET, NULL)!=LVERR_OK)
            return 0;

        SetName( m_stream->GetName() );


        lvsize_t sz = 0;
        if (m_stream->GetSize( &sz )!=LVERR_OK)
                return 0;
        lvsize_t m_FileSize = (unsigned)sz;

        char ReadBuf[1024];
        lUInt32 NextPosition;
        lvpos_t CurPos;
        lvsize_t ReadSize;
        int Buf;
        bool found = false;
        CurPos=NextPosition=(int)m_FileSize;
        if (CurPos < sizeof(ReadBuf)-18)
            CurPos = 0;
        else
            CurPos -= sizeof(ReadBuf)-18;
        for ( Buf=0; Buf<64 && !found; Buf++ )
        {
            //SetFilePointer(ArcHandle,CurPos,NULL,FILE_BEGIN);
            m_stream->Seek( CurPos, LVSEEK_SET, NULL );
            m_stream->Read( ReadBuf, sizeof(ReadBuf), &ReadSize);
            if (ReadSize==0)
                break;
            for (int I=(int)ReadSize-4;I>=0;I--)
            {
                if (ReadBuf[I]==0x50 && ReadBuf[I+1]==0x4b && ReadBuf[I+2]==0x05 &&
                    ReadBuf[I+3]==0x06)
                {
                    m_stream->Seek( CurPos+I+16, LVSEEK_SET, NULL );
                    m_stream->Read( &NextPosition, sizeof(NextPosition), &ReadSize);
		    		cnv.lsf( &NextPosition );
                    found=true;
                    break;
                }
            }
            if (CurPos==0)
                break;
            if (CurPos<sizeof(ReadBuf)-4)
                CurPos=0;
            else
                CurPos-=sizeof(ReadBuf)-4;
        }

        truncated = !found;
        if (truncated)
            NextPosition=0;

        //================================================================
        // get files


        ZipLocalFileHdr ZipHd1;
        ZipHd2 ZipHeader;
        unsigned ZipHeader_size = 0x2E; //sizeof(ZipHd2); //0x34; //
        unsigned ZipHd1_size = 0x1E; //sizeof(ZipHd1); //sizeof(ZipHd1)
          //lUInt32 ReadSize;

        while (1) {

            if (m_stream->Seek( NextPosition, LVSEEK_SET, NULL )!=LVERR_OK)
                return 0;

            if (truncated)
            {
                m_stream->Read( &ZipHd1, ZipHd1_size, &ReadSize);
                ZipHd1.byteOrderConv();

                //ReadSize = fread(&ZipHd1, 1, sizeof(ZipHd1), f);
                if (ReadSize != ZipHd1_size) {
                        //fclose(f);
                    if (ReadSize==0 && NextPosition==m_FileSize)
                        return m_list.length();
                    return 0;
                }

                memset(&ZipHeader,0,ZipHeader_size);

                ZipHeader.UnpVer=ZipHd1.UnpVer;
                ZipHeader.UnpOS=ZipHd1.UnpOS;
                ZipHeader.Flags=ZipHd1.Flags;
                ZipHeader.ftime=ZipHd1.getftime();
                ZipHeader.PackSize=ZipHd1.getPackSize();
                ZipHeader.UnpSize=ZipHd1.getUnpSize();
                ZipHeader.NameLen=ZipHd1.getNameLen();
                ZipHeader.AddLen=ZipHd1.getAddLen();
                ZipHeader.Method=ZipHd1.getMethod();
            } else {

                m_stream->Read( &ZipHeader, ZipHeader_size, &ReadSize);

                ZipHeader.byteOrderConv();
                    //ReadSize = fread(&ZipHeader, 1, sizeof(ZipHeader), f);
                if (ReadSize!=ZipHeader_size) {
                            if (ReadSize>16 && ZipHeader.Mark==0x06054B50 ) {
                                    break;
                            }
                            //fclose(f);
                            return 0;
                }
            }

            if (ReadSize==0 || ZipHeader.Mark==0x06054b50 ||
                    truncated && ZipHeader.Mark==0x02014b50)
            {
                if (!truncated && *(lUInt16 *)((char *)&ZipHeader+20)!=0)
                    arcComment=true;
                break; //(GETARC_EOF);
            }

            const int NM = 513;
            lUInt32 SizeToRead=(ZipHeader.NameLen<NM) ? ZipHeader.NameLen : NM;
            char fnbuf[1025];
            m_stream->Read( fnbuf, SizeToRead, &ReadSize);

            if (ReadSize!=SizeToRead) {
                return 0;
            }

            fnbuf[ZipHeader.NameLen]=0;

            long SeekLen=ZipHeader.AddLen+ZipHeader.CommLen;

            LVCommonContainerItemInfo * item = new LVCommonContainerItemInfo();

            if (truncated)
                SeekLen+=ZipHeader.PackSize;

            NextPosition = (lUInt32)m_stream->GetPos();
            NextPosition += SeekLen;
            m_stream->Seek(NextPosition, LVSEEK_SET, NULL);

	        // {"DOS","Amiga","VAX/VMS","Unix","VM/CMS","Atari ST",
			//  "OS/2","Mac-OS","Z-System","CP/M","TOPS-20",
			//  "Win32","SMS/QDOS","Acorn RISC OS","Win32 VFAT","MVS",
			//  "BeOS","Tandem"};
            const lChar16 * enc_name = (ZipHeader.PackOS==0) ? L"cp866" : L"cp1251";
            const lChar16 * table = GetCharsetByte2UnicodeTable( enc_name );
            lString16 fName = ByteToUnicode( lString8(fnbuf), table );

            item->SetItemInfo(fName.c_str(), ZipHeader.UnpSize, (ZipHeader.getAttr() & 0x3f));
            item->SetSrc( ZipHeader.getOffset(), ZipHeader.PackSize, ZipHeader.Method );
            m_list.add(item);
        }

        return m_list.length();
    }

    static LVArcContainerBase * OpenArchieve( LVStreamRef stream )
    {
        // read beginning of file
        const lvsize_t hdrSize = 6;
        char hdr[hdrSize];
        stream->SetPos(0);
        lvsize_t bytesRead = 0;
        if (stream->Read(hdr, hdrSize, &bytesRead)!=LVERR_OK || bytesRead!=hdrSize)
                return NULL;
        stream->SetPos(0);
        // detect arc type
        if (hdr[0]!='R' || hdr[1]!='a' || hdr[2]!='r' || hdr[3]!='!' || hdr[4]!=7 || cdr[5]!=0)
                return NULL;
        LVRarArc * arc = new LVRarArc( stream );
        int itemCount = arc->ReadContents();
        if ( itemCount <= 0 )
        {
            delete arc;
            return NULL;
        }
        return arc;
    }

};


#endif // UNRAR





class LVMemoryStream : public LVNamedStream
{
protected:
	lUInt8 *               m_pBuffer;
	bool                   m_own_buffer;
	LVContainer *          m_parent;
	lvsize_t               m_size;
	lvsize_t               m_bufsize;
	lvpos_t                m_pos;
	lvopen_mode_t          m_mode;
public:
    /// Check whether end of file is reached
    /**
        \return true if end of file reached
    */
    virtual bool Eof()
    {
        return m_pos>=m_size;
    }
    virtual lvopen_mode_t GetMode()
    {
        return m_mode;
    }
    /** \return LVERR_OK if change is ok */
    virtual lverror_t SetMode( lvopen_mode_t mode )
    {
    	if ( m_mode==mode )
    		return LVERR_OK;
    	if ( m_mode==LVOM_WRITE && mode==LVOM_READ ) {
    		m_mode = LVOM_READ;
    		m_pos = 0;
    		return LVERR_OK;
    	}
    	// TODO: READ -> WRITE/APPEND
    	return LVERR_FAIL;
    }
	virtual LVContainer * GetParentContainer()
	{
		return (LVContainer*)m_parent;
	}
	virtual lverror_t Read( void * buf, lvsize_t count, lvsize_t * nBytesRead )
	{
		if (!m_pBuffer || m_mode==LVOM_WRITE || m_mode==LVOM_APPEND )
			return LVERR_FAIL;
		//
		int bytesAvail = (int)(m_size - m_pos);
		if (bytesAvail>0) {
			int bytesRead = bytesAvail;
			if (bytesRead>(int)count)
				bytesRead = (int)count;
			if (bytesRead>0)
				memcpy( buf, m_pBuffer+(int)m_pos, bytesRead );
			if (nBytesRead)
				*nBytesRead = bytesRead;
			m_pos += bytesRead;
		} else {
			if (nBytesRead)
				*nBytesRead = 0; // EOF
		}
		return LVERR_OK;
	}
	virtual lverror_t GetSize( lvsize_t * pSize )
	{
		if (!m_pBuffer || !pSize)
			return LVERR_FAIL;
		if (m_size<m_pos)
			m_size = m_pos;
		*pSize = m_size;
		return LVERR_OK;
	}
	// ensure that buffer is at least new_size long
	lverror_t SetBufSize( lvsize_t new_size )
	{
		if (!m_pBuffer || m_mode==LVOM_READ )
			return LVERR_FAIL;
		if (new_size<=m_bufsize)
			return LVERR_OK;
		if (m_own_buffer!=true)
			return LVERR_FAIL; // cannot resize foreign buffer
		//
		int newbufsize = (int)(new_size * 2 + 4096);
		m_pBuffer = (lUInt8*) realloc( m_pBuffer, newbufsize );
		m_bufsize = newbufsize;
		return LVERR_OK;
	}
	virtual lverror_t SetSize( lvsize_t size )
	{
		//
		if (SetBufSize( size )!=LVERR_OK)
			return LVERR_FAIL;
		m_size = size;
		if (m_pos>m_size)
			m_pos = m_size;
		return LVERR_OK;
	}
	virtual lverror_t Write( const void * buf, lvsize_t count, lvsize_t * nBytesWritten )
	{
		if (!m_pBuffer || m_mode==LVOM_READ )
			return LVERR_FAIL;
		SetBufSize( m_pos+count ); // check buf size
		int bytes_avail = (int)(m_bufsize-m_pos);
		if (bytes_avail>(int)count)
			bytes_avail = (int)count;
		if (bytes_avail>0) {
			memcpy( m_pBuffer+m_pos, buf, bytes_avail );
			m_pos+=bytes_avail;
			if (m_size<m_pos)
				m_size = m_pos;
		}
		if (nBytesWritten)
			*nBytesWritten = bytes_avail;
		return LVERR_OK;
	}
	virtual lverror_t Seek( lvoffset_t offset, lvseek_origin_t origin, lvpos_t * pNewPos )
	{
		if (!m_pBuffer)
			return LVERR_FAIL;
		lvpos_t newpos = m_pos;
		switch (origin) {
		case LVSEEK_SET:
			newpos = offset;
			break;
		case LVSEEK_CUR:
			newpos = m_pos + offset;
			break;
		case LVSEEK_END:
			newpos = m_size + offset;
			break;
		}
                if (newpos>m_size)
			return LVERR_FAIL;
		m_pos = newpos;
		if (pNewPos)
			*pNewPos = m_pos;
		return LVERR_OK;
	}
	virtual lverror_t Close()
	{
		if (!m_pBuffer)
			return LVERR_FAIL;
		if (m_pBuffer && m_own_buffer)
			delete[] m_pBuffer;
		m_pBuffer = NULL;
		m_size = 0;
		m_bufsize = 0;
		m_pos = 0;
		return LVERR_OK;
	}
	lverror_t Create( )
	{
		Close();
		m_bufsize = 4096;
		m_size = 0;
		m_pos = 0;
		m_pBuffer = new lUInt8[(int)m_bufsize];
		m_own_buffer = true;
		m_mode = LVOM_READWRITE;
		return LVERR_OK;
	}
    /// Creates memory stream as copy of another stream.
	lverror_t CreateCopy( LVStreamRef srcStream, lvopen_mode_t mode )
	{
		Close();
        if ( mode!=LVOM_READ || srcStream.isNull() )
            return LVERR_FAIL;
        lvsize_t sz = srcStream->GetSize();
        if ( (int)sz <= 0 || sz > 0x200000 )
            return LVERR_FAIL;
		m_bufsize = sz;
		m_size = 0;
		m_pos = 0;
		m_pBuffer = new lUInt8[(int)m_bufsize];
		if (m_pBuffer) {
            lvsize_t bytesRead = 0;
            srcStream->Read( m_pBuffer, m_bufsize, &bytesRead );
            if ( bytesRead!=m_bufsize ) {
                delete m_pBuffer;
                m_pBuffer = 0;
                m_size = 0;
                m_pos = 0;
                m_bufsize = 0;
                return LVERR_FAIL;
            }
		}
        m_size = sz;
		m_own_buffer = true;
		m_mode = mode;
		return LVERR_OK;
	}


	lverror_t CreateCopy( const lUInt8 * pBuf, lvsize_t size, lvopen_mode_t mode )
	{
		Close();
		m_bufsize = size;
		m_pos = 0;
		m_pBuffer = new lUInt8[(int)m_bufsize];
		if (m_pBuffer) {
			memcpy( m_pBuffer, pBuf, (int)size );
		}
		m_own_buffer = true;
		m_mode = mode;
        m_size = size;
		if (mode==LVOM_APPEND)
			m_pos = m_size;
		return LVERR_OK;
	}
	lverror_t Open( lUInt8 * pBuf, lvsize_t size )
	{
                if (!pBuf)
			return LVERR_FAIL;
		m_own_buffer = false;
		m_pBuffer = pBuf;
		m_bufsize = size;
		// set file size and position
		m_pos = 0;
		m_size = size;
		m_mode = LVOM_READ;

		return LVERR_OK;
	}
	LVMemoryStream() : m_pBuffer(NULL), m_own_buffer(false), m_parent(NULL), m_size(0), m_pos(0)
	{
	}
	virtual ~LVMemoryStream()
	{
		Close();
		m_parent = NULL;
	}
};

#if (USE_ZLIB==1)
LVContainerRef LVOpenArchieve( LVStreamRef stream )
{
    LVContainerRef ref;
    if (stream.isNull())
        return ref;

    // try ZIP
    ref = LVZipArc::OpenArchieve( stream );
    if (!ref.isNull())
            return ref;

    // try RAR: todo

    // not found: return null ref
    return ref;
}
#endif

/// Creates memory stream as copy of string contents
LVStreamRef LVCreateStringStream( lString8 data )
{
    LVMemoryStream * stream = new LVMemoryStream();
    stream->CreateCopy( (const lUInt8*)data.c_str(), data.length(), LVOM_READ );
    return LVStreamRef( stream );
}

/// Creates memory stream as copy of string contents
LVStreamRef LVCreateStringStream( lString16 data )
{
    return LVCreateStringStream( UnicodeToUtf8( data ) );
}

LVStreamRef LVCreateMemoryStream( void * buf, int bufSize, bool createCopy, lvopen_mode_t mode )
{
    LVMemoryStream * stream = new LVMemoryStream();
    if ( !buf )
        stream->Create();
    else if ( createCopy )
        stream->CreateCopy( (lUInt8*)buf, bufSize, mode );
    else
        stream->Open( (lUInt8*)buf, bufSize );
    return LVStreamRef( stream );
}

LVStreamRef LVCreateMemoryStream( LVStreamRef srcStream )
{
    LVMemoryStream * stream = new LVMemoryStream();
    if ( stream->CreateCopy(srcStream, LVOM_READ)==LVERR_OK )
        return LVStreamRef( stream );
    else
        delete stream;
    return LVStreamRef();
}

/// Creates memory stream as copy of file contents.
LVStreamRef LVCreateMemoryStream( lString16 filename )
{
    LVStreamRef fs = LVOpenFileStream( filename.c_str(), LVOM_READ );
    if ( fs.isNull() )
        return fs;
    return LVCreateMemoryStream( fs );
}

LVStreamRef LVCreateBufferedStream( LVStreamRef stream, int bufSize )
{
    if ( stream.isNull() || bufSize < 512 )
        return stream;
    return LVStreamRef( new LVCachedStream( stream, bufSize ) );
}

lvsize_t LVPumpStream( LVStreamRef out, LVStreamRef in )
{
    char buf[4096];
    lvsize_t totalBytesRead = 0;
    lvsize_t bytesRead = 0;
    in->SetPos(0);
    while ( in->Read( buf, 4096, &bytesRead )==LVERR_OK && bytesRead>0 )
    {
        out->Write( buf, bytesRead, NULL );
        totalBytesRead += bytesRead;
    }
    return totalBytesRead;
}


LVContainerRef LVOpenDirectory( const wchar_t * path, const wchar_t * mask )
{
    LVContainerRef dir( LVDirectoryContainer::OpenDirectory( path ) );
    return dir;
}

/// Stream base class
class LVTCRStream : public LVNamedStream
{
    class TCRCode {
    public:
        int len;
        char * str;
        TCRCode()
            : len(0), str(NULL)
        {
        }
        void set( const char * s, int sz )
        {
            if ( sz>0 ) {
                str = (char *)malloc( sz + 1 );
                memcpy( str, s, sz );
                str[sz] = 0;
                len = sz;
            }
        }
        ~TCRCode()
        {
            if ( str )
                free( str );
        }
    };
    LVStreamRef _stream;
    TCRCode _codes[256];
    lvpos_t _packedStart;
    lvsize_t _packedSize;
    lvsize_t _unpSize;
    lUInt32 * _index;
    lUInt8 * _decoded;
    int _decodedSize;
    int _decodedLen;
    unsigned _partIndex;
    lvpos_t _decodedStart;
    int _indexSize;
    lvpos_t _pos;
    //int _indexPos;
    #define TCR_READ_BUF_SIZE 4096
    lUInt8 _readbuf[TCR_READ_BUF_SIZE];
    LVTCRStream( LVStreamRef stream )
    : _stream(stream), _index(NULL), _decoded(NULL),
      _decodedSize(0), _decodedLen(0), _partIndex((unsigned)-1), _decodedStart(0), _indexSize(0), _pos(0)
    {
    }

    bool decodePart( unsigned index )
    {
        if ( _partIndex==index )
            return true;
        lvsize_t bytesRead;
        int bytesToRead = TCR_READ_BUF_SIZE;
        if ( (index+1)*TCR_READ_BUF_SIZE > _packedSize )
            bytesToRead = TCR_READ_BUF_SIZE - ((index+1)*TCR_READ_BUF_SIZE - _packedSize);
        if ( bytesToRead<=0 || bytesToRead>TCR_READ_BUF_SIZE )
            return false;
        if ( _stream->SetPos(_packedStart + index * TCR_READ_BUF_SIZE)==(lvpos_t)(~0) )
            return false;
        if ( _stream->Read( _readbuf, bytesToRead, &bytesRead )!=LVERR_OK )
            return false;
        if ( bytesToRead!=(int)bytesRead )
            return false;
        //TODO
        if ( !_decoded ) {
            _decodedSize = TCR_READ_BUF_SIZE * 2;
            _decoded = (lUInt8 *)malloc( _decodedSize );
        }
        _decodedLen = 0;
        for ( unsigned i=0; i<bytesRead; i++ ) {
            TCRCode * item = &_codes[_readbuf[i]];
            for ( int j=0; j<item->len; j++ )
                _decoded[_decodedLen++] = item->str[j];
            if ( _decodedLen >= _decodedSize - 256 ) {
                _decodedSize += TCR_READ_BUF_SIZE / 2;
                _decoded = (lUInt8*)realloc( _decoded, _decodedSize );
            }
        }
        _decodedStart = _index[index];
        _partIndex = index;
        return true;
    }
public:
    ~LVTCRStream()
    {
        if ( _index )
            free(_index);
    }
    bool init()
    {
        lUInt8 sz;
        char buf[256];
        lvsize_t bytesRead;
        for ( int i=0; i<256; i++ ) {
            if ( _stream->Read( &sz, 1, &bytesRead )!=LVERR_OK || bytesRead!=1 )
                return false;
            if ( sz==0 && i!=0 )
                return false; // only first entry may be 0
            if ( sz && (_stream->Read( buf, sz, &bytesRead )!=LVERR_OK || bytesRead!=sz) )
                return false;
            _codes[i].set( buf, sz );
        }
        _packedStart = _stream->GetPos();
        if ( _packedStart==(lvpos_t)(~0) )
            return false;
        _packedSize = _stream->GetSize() - _packedStart;
        if ( _packedSize<10 || _packedSize>0x8000000 )
            return false;
        _indexSize = (_packedSize + TCR_READ_BUF_SIZE - 1) / TCR_READ_BUF_SIZE;
        _index = (lUInt32*)malloc( sizeof(lUInt32) * (_indexSize + 1) );
        lvpos_t pos = 0;
        lvsize_t size = 0;
        for (;;) {
            bytesRead = 0;
            int res = _stream->Read( _readbuf, TCR_READ_BUF_SIZE, &bytesRead );
            if ( res!=LVERR_OK && res!=LVERR_EOF )
                return false;
            if ( bytesRead>0 ) {
                for ( unsigned i=0; i<bytesRead; i++ ) {
                    int sz = _codes[_readbuf[i]].len;
                    if ( (pos & (TCR_READ_BUF_SIZE-1)) == 0 ) {
                        // add pos index
                        int index = pos / TCR_READ_BUF_SIZE;
                        _index[index] = size;
                    }
                    size += sz;
                    pos ++;
                }
            }
            if ( res==LVERR_EOF || bytesRead==0 ) {
                if ( _packedStart + pos != _stream->GetSize() )
                    return false;
                break;
            }
        }
        _index[ _indexSize ] = size;
        _unpSize = size;
        return decodePart( 0 );
    }
    static LVStreamRef create( LVStreamRef stream, int mode )
    {
        LVStreamRef res;
        if ( stream.isNull() || mode != LVOM_READ )
            return res;
        static const char * signature = "!!8-Bit!!";
        char buf[9];
        if ( stream->SetPos(0)!=0 )
            return res;
        lvsize_t bytesRead = 0;
        if ( stream->Read(buf, 9, &bytesRead)!=LVERR_OK
            || bytesRead!=9 )
            return res;
        if ( memcmp(signature, buf, 9) )
            return res;
        LVTCRStream * decoder = new LVTCRStream( stream );
        if ( !decoder->init() ) {
            delete decoder;
            return res;
        }
        return LVStreamRef ( decoder );
    }

    /// Get stream open mode
    /** \return lvopen_mode_t open mode */
    virtual lvopen_mode_t GetMode()
    {
        return LVOM_READ;
    }

    /// Seek (change file pos)
    /**
        \param offset is file offset (bytes) relateve to origin
        \param origin is offset base
        \param pNewPos points to place to store new file position
        \return lverror_t status: LVERR_OK if success
    */
    virtual lverror_t Seek( lvoffset_t offset, lvseek_origin_t origin, lvpos_t * pNewPos )
    {
        lvpos_t npos = 0;
        lvpos_t currpos = _pos;
        switch (origin) {
        case LVSEEK_SET:
            npos = offset;
            break;
        case LVSEEK_CUR:
            npos = currpos + offset;
            break;
        case LVSEEK_END:
            npos = _unpSize + offset;
            break;
        }
        if (npos >= _unpSize)
            return LVERR_FAIL;
        _pos = npos;
        if ( _pos < _decodedStart || _pos >= _decodedStart + _decodedLen ) {
            // load another part
            int a = 0;
            int b = _indexSize;
            int c;
            for (;;) {
                c = (a + b) / 2;
                if ( a >= b-1 )
                    break;
                if ( _index[c] > _pos )
                    b = c;
                else if ( _index[c+1] <= _pos )
                    a = c + 1;
                else
                    break;
            }
            if ( _index[c]>_pos || _index[c+1]<=_pos )
                return LVERR_FAIL; // wrong algorithm?
            if ( !decodePart( c ) )
                return LVERR_FAIL;
        }
        if (pNewPos)
        {
            *pNewPos =  _pos;
        }
        return LVERR_OK;
    }


    /// Get file position
    /**
        \return lvpos_t file position
    */
    virtual lvpos_t   GetPos()
    {
        return _pos;
    }

    /// Get file size
    /**
        \return lvsize_t file size
    */
    virtual lvsize_t  GetSize()
    {
        return _unpSize;
    }

    virtual lverror_t GetSize( lvsize_t * pSize )
    {
        *pSize = _unpSize;
        return LVERR_OK;
    }

    /// Set file size
    /**
        \param size is new file size
        \return lverror_t status: LVERR_OK if success
    */
    virtual lverror_t SetSize( lvsize_t )
    {
        return LVERR_FAIL;
    }

    /// Read
    /**
        \param buf is buffer to place bytes read from stream
        \param count is number of bytes to read from stream
        \param nBytesRead is place to store real number of bytes read from stream
        \return lverror_t status: LVERR_OK if success
    */
    virtual lverror_t Read( void * buf, lvsize_t count, lvsize_t * nBytesRead )
    {
        // TODO
        if ( nBytesRead )
            *nBytesRead = 0;
        lUInt8 * dst = (lUInt8*) buf;
        while (count) {
            int bytesLeft = _decodedLen - (int)(_pos - _decodedStart);
            if ( bytesLeft<=0 || bytesLeft>_decodedLen ) {
                SetPos(_pos);
                bytesLeft = _decodedLen - (int)(_pos - _decodedStart);
                if ( bytesLeft==0 && _pos==_decodedStart+_decodedLen) {
                    return *nBytesRead ? LVERR_OK : LVERR_EOF;
                }
                if ( bytesLeft<=0 || bytesLeft>_decodedLen )
                    return LVERR_FAIL;
            }
            lUInt8 * src = _decoded + (_pos - _decodedStart);
            unsigned n = count;
            if ( n > (unsigned)bytesLeft )
                n = bytesLeft;
            for (unsigned i=0; i<n; i++) {
                *dst++ = *src++;
            }
            count -= n;
            bytesLeft -= n;
            if ( nBytesRead )
                *nBytesRead += n;
            _pos += n;
        }
        return LVERR_OK;
    }

    /// Write
    /**
        \param buf is data to write to stream
        \param count is number of bytes to write
        \param nBytesWritten is place to store real number of bytes written to stream
        \return lverror_t status: LVERR_OK if success
    */
    virtual lverror_t Write( const void *, lvsize_t, lvsize_t *)
    {
        return LVERR_FAIL;
    }

    /// Check whether end of file is reached
    /**
        \return true if end of file reached
    */
    virtual bool Eof()
    {
        //TODO
        return false;
    }
};

/// creates TCR decoder stream for stream
LVStreamRef LVCreateTCRDecoderStream( LVStreamRef stream )
{
    return LVTCRStream::create( stream, LVOM_READ );
}

/// returns path part of pathname (appended with / or \ delimiter)
lString16 LVExtractPath( lString16 pathName )
{
    int last_delim_pos = -1;
    for ( unsigned i=0; i<pathName.length(); i++ )
        if ( pathName[i]=='/' || pathName[i]=='\\' )
            last_delim_pos = i;
    if ( last_delim_pos==-1 )
#ifdef _LINUX
        return lString16(L"./");
#else
        return lString16(L".\\");
#endif
    return pathName.substr( 0, last_delim_pos+1 );
}

/// returns filename part of pathname
lString16 LVExtractFilename( lString16 pathName )
{
    int last_delim_pos = -1;
    for ( unsigned i=0; i<pathName.length(); i++ )
        if ( pathName[i]=='/' || pathName[i]=='\\' )
            last_delim_pos = i;
    if ( last_delim_pos==-1 )
        return pathName;
    return pathName.substr( last_delim_pos+1 );
}

/// returns filename part of pathname without extension
lString16 LVExtractFilenameWithoutExtension( lString16 pathName )
{
    lString16 s = LVExtractFilename( pathName );
    int lastDot = -1;
    for ( unsigned i=0; i<s.length(); i++ )
        if ( s[i]=='.' )
            lastDot = i;
    if ( lastDot<=0 || lastDot<(int)s.length()-7 )
        return s;
    return s.substr( 0, lastDot );
}

/// returns true if absolute path is specified
bool LVIsAbsolutePath( lString16 pathName )
{
    if ( pathName.empty() )
        return false;
    lChar16 c = pathName[0];
    if ( c=='\\' || c=='/' )
        return true;
#ifdef _WIN32
    if ( (c>='a' && c<='z') || (c>='A' && c<='Z') ) {
        return (pathName[1]==':');
    }
#endif
    return false;
}

/// removes first path part from pathname and returns it
lString16 LVExtractFirstPathElement( lString16 & pathName )
{
    if ( pathName.empty() )
        return lString16();
    if ( pathName[0]=='/' || pathName[0]=='\\' )
        pathName.erase(0, 1);
    int first_delim_pos = -1;
    for ( unsigned i=0; i<pathName.length(); i++ )
        if ( pathName[i]=='/' || pathName[i]=='\\' ) {
            first_delim_pos = i;
            break;
        }
    if ( first_delim_pos==-1 ) {
        lString16 res = pathName;
        pathName.clear();
        return res;
    }
    lString16 res = pathName.substr(0, first_delim_pos );
    pathName.erase(0, first_delim_pos+1 );
    return res;
}

/// appends path delimiter character to end of path, if absent
void LVAppendPathDelimiter( lString16 & pathName )
{
    if ( pathName.empty() )
        return;
    lChar16 delim = LVDetectPathDelimiter( pathName );
    if ( pathName[pathName.length()-1]!=delim )
        pathName << delim;
}

/// replaces any found / or \\ separator with specified one
void LVReplacePathSeparator( lString16 & pathName, lChar16 separator )
{
    lChar16 * buf = pathName.modify();
    for ( ; *buf; buf++ )
        if ( *buf=='/' || *buf=='\\' )
            *buf = separator;
}

// resolve relative links
lString16 LVCombinePaths( lString16 basePath, lString16 newPath )
{
    if ( newPath[0]=='/' || newPath[0]=='\\' || (newPath.length()>0 && newPath[1]==':' && newPath[2]=='\\') )
        return newPath; // absolute path
    lChar16 separator = 0;
    for ( int i=0; i<basePath.length(); i++ ) {
        if ( basePath[i]=='/' || basePath[i]=='\\' ) {
            separator = basePath[i];
            break;
        }
    }
    if ( separator == 0 )
        for ( int i=0; i<newPath.length(); i++ ) {
            if ( newPath[i]=='/' || newPath[i]=='\\' ) {
                separator = newPath[i];
                break;
            }
        }
    if ( separator == 0 )
        separator = '/';
    lString16 s = basePath;
    LVAppendPathDelimiter( s );
    s += newPath;
    //LVAppendPathDelimiter( s );
    LVReplacePathSeparator( s, separator );
    lString16 pattern;
    pattern << separator << L".." << separator;
    bool changed;
    do {
        changed = false;
        int lastElementStart = -1;
        for ( int i=0; i<s.length()-pattern.length(); i++ ) {
            if ( s[i]==separator && s[i+1]!='.' && s[i]!=separator )
                lastElementStart = i;
            else if ( s[i]==separator && s[i+1]=='.' && s[i+2]=='.' && s[i+3]==separator ) {
                if ( lastElementStart>=0 ) {
                    // /a/b/../c/
                    // 0123456789
                    //   ^ ^
                    s.erase( lastElementStart+1, i+4-lastElementStart-1 );
                    changed = true;
                    lastElementStart = -1;
                    break;
                }
            }
        }
    } while ( changed );
    return s;
}


/// removes last path part from pathname and returns it
lString16 LVExtractLastPathElement( lString16 & pathName )
{
    int l = pathName.length();
    if ( l==0 )
        return lString16();
    if ( pathName[l-1]=='/' || pathName[l-1]=='\\' )
        pathName.erase(l-1, 1);
    int last_delim_pos = -1;
    for ( unsigned i=0; i<pathName.length(); i++ )
        if ( pathName[i]=='/' || pathName[i]=='\\' )
            last_delim_pos = i;
    if ( last_delim_pos==-1 ) {
        lString16 res = pathName;
        pathName.clear();
        return res;
    }
    lString16 res = pathName.substr( last_delim_pos + 1, pathName.length()-last_delim_pos-1 );
    pathName.erase( last_delim_pos, pathName.length()-last_delim_pos );
    return res;
}

/// returns path delimiter character
lChar16 LVDetectPathDelimiter( lString16 pathName )
{
    for ( unsigned i=0; i<pathName.length(); i++ )
        if ( pathName[i]=='/' || pathName[i]=='\\' )
            return pathName[i];
#ifdef _LINUX
        return '/';
#else
        return '\\';
#endif
}

/// returns full path to file identified by pathName, with base directory == basePath
lString16 LVMakeRelativeFilename( lString16 basePath, lString16 pathName )
{
    if ( LVIsAbsolutePath( pathName ) )
        return pathName;
    lChar16 delim = LVDetectPathDelimiter( basePath );
    lString16 path = LVExtractPath( basePath );
    lString16 name = LVExtractFilename( pathName );
    lString16 dstpath = LVExtractPath( pathName );
    while ( !dstpath.empty() ) {
        lString16 element = LVExtractFirstPathElement( dstpath );
        if ( element==L"." )
            ;
        else if ( element==L".." )
            LVExtractLastPathElement( path );
        else
            path << element << delim;
    }
    LVAppendPathDelimiter( path );
    path << name;
    return path;
}

/// removes path delimiter character from end of path, if exists
void LVRemovePathDelimiter( lString16 & pathName )
{
    int len = pathName.length();
    if ( len>0 ) {
        if ( pathName.lastChar() == '/' || pathName.lastChar() == '\\' )
            pathName.erase( pathName.length()-1, 1 );
    }
}


/// returns true if specified file exists
bool LVFileExists( lString16 pathName )
{
#ifdef _WIN32
	LVStreamRef stream = LVOpenFileStream( pathName.c_str(), LVOM_READ );
	return !stream.isNull();
#else
    FILE * f = fopen(UnicodeToUtf8(pathName).c_str(), "rb");
    if ( f ) {
        fclose( f );
        return true;
    }
    return false;
#endif
}

/// returns true if specified directory exists
bool LVDirectoryExists( lString16 pathName )
{
	// TODO: optimize
    LVContainerRef dir = LVOpenDirectory( pathName.c_str() );
    return !dir.isNull();
}

/// Create directory if not exist
bool LVCreateDirectory( lString16 path )
{
    CRLog::trace("LVCreateDirectory(%s)", UnicodeToUtf8(path).c_str() );
    //LVRemovePathDelimiter(path);
    if ( path.length() <= 1 )
        return false;
    LVContainerRef dir = LVOpenDirectory( path.c_str() );
    if ( dir.isNull() ) {
        CRLog::trace("Directory %s not found", UnicodeToUtf8(path).c_str());
        LVRemovePathDelimiter(path);
        lString16 basedir = LVExtractPath( path );
        CRLog::trace("Checking base directory %s", UnicodeToUtf8(basedir).c_str());
        if ( !LVCreateDirectory( basedir ) ) {
            CRLog::error("Failed to create directory %s", UnicodeToUtf8(basedir).c_str());
            return false;
        }
#ifdef _WIN32
        return CreateDirectoryW( path.c_str(), NULL )!=0;
#else
        //LVRemovePathDelimiter( path );
        lString8 path8 = UnicodeToUtf8( path );
        CRLog::trace("Creating directory %s", path8.c_str() );
        if ( mkdir(path8.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) ) {
            CRLog::error("Cannot create directory %s", path8.c_str() );
            return false;
        }
        return true;
#endif
    }
    CRLog::trace("Directory %s exists", UnicodeToUtf8(path).c_str());
    return true;
}

/// Open memory mapped file
/**
    \param pathname is file name to open (unicode)
    \param mode is mode file should be opened in (LVOM_READ or LVOM_APPEND only)
        \param minSize is minimum file size for R/W mode
    \return reference to opened stream if success, NULL if error
*/
LVStreamRef LVMapFileStream( const lChar16 * pathname, lvopen_mode_t mode, lvsize_t minSize )
{
#if !defined(_WIN32) && !defined(_LINUX)
        // STUB for systems w/o mmap
    LVFileStream * stream = LVFileStream::CreateFileStream( pathname, mode );
    if ( stream!=NULL )
    {
        return LVStreamRef( stream );
    }
    return LVStreamRef();
#else
        LVFileMappedStream * stream = LVFileMappedStream::CreateFileStream( lString16(pathname), mode, (int)minSize );
        return LVStreamRef(stream);
#endif
}

/// delete file, return true if file found and successfully deleted
bool LVDeleteFile( lString16 filename )
{
#ifdef _WIN32
    return DeleteFileW( filename.c_str() ) ? true : false;
#else
    if ( unlink( UnicodeToUtf8( filename ).c_str() ) )
        return false;
    return true;
#endif
}

