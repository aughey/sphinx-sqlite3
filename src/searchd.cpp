//
// $Id: searchd.cpp 1564 2008-11-12 20:48:11Z xale $
//

//
// Copyright (c) 2001-2008, Andrew Aksyonoff. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "sphinx.h"
#include "sphinxutils.h"
#include "sphinxexcerpt.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>
#include <limits.h>

#define SEARCHD_BACKLOG			5
#define SEARCHD_DEFAULT_PORT	3312

#define SPH_ADDRESS_SIZE		sizeof("000.000.000.000")

/////////////////////////////////////////////////////////////////////////////

#if USE_WINDOWS
	// Win-specific headers and calls
	#include <io.h>
	#include <winsock2.h>
	#include <tlhelp32.h>

	#define sphSockRecv(_sock,_buf,_len)	::recv(_sock,_buf,_len,0)
	#define sphSockSend(_sock,_buf,_len)	::send(_sock,_buf,_len,0)
	#define sphSockClose(_sock)				::closesocket(_sock)

	#define stat		_stat

#else
	// UNIX-specific headers and calls
	#include <unistd.h>
	#include <netinet/in.h>
	#include <sys/file.h>
	#include <sys/socket.h>
	#include <sys/time.h>
	#include <sys/wait.h>
	#include <sys/un.h>
	#include <netdb.h>

	// there's no MSG_NOSIGNAL on OS X
	#ifndef MSG_NOSIGNAL
	#define MSG_NOSIGNAL 0
	#endif

	#define sphSockRecv(_sock,_buf,_len)	::recv(_sock,_buf,_len,MSG_NOSIGNAL)
	#define sphSockSend(_sock,_buf,_len)	::send(_sock,_buf,_len,MSG_NOSIGNAL)
	#define sphSockClose(_sock)				::close(_sock)

#endif

/////////////////////////////////////////////////////////////////////////////
// MISC GLOBALS
/////////////////////////////////////////////////////////////////////////////

struct ServedIndex_t
{
	CSphIndex *			m_pIndex;
	const CSphSchema *	m_pSchema;		///< pointer to index schema, managed by the index itself
	CSphString			m_sIndexPath;
	bool				m_bEnabled;		///< to disable index in cases when rotation fails
	bool				m_bMlock;
	bool				m_bPreopen;
	bool				m_bOnDiskDict;
	bool				m_bStar;
	bool				m_bToDelete;
	bool				m_bOnlyNew;
	int					m_iUpdateTag;

public:
						ServedIndex_t ();
						~ServedIndex_t ();
	void				Reset ();
};

/////////////////////////////////////////////////////////////////////////////

enum ESphAddIndex
{
	ADD_ERROR	= 0,
	ADD_LOCAL	= 1,
	ADD_DISTR	= 2
};


enum ESphLogLevel
{
	LOG_FATAL	= 0,
	LOG_WARNING	= 1,
	LOG_INFO	= 2
};

static bool				g_bService		= false;
#if USE_WINDOWS
static bool				g_bServiceStop	= false;
static const char *		g_sServiceName	= "searchd";
HANDLE					g_hPipe			= INVALID_HANDLE_VALUE;
#endif

static CSphVector<CSphString>	g_dArgs;

static bool				g_bHeadDaemon	= false;
static bool				g_bLogStdout	= true;

static bool				g_bCrashLog_Enabled			= false;
static const char *		g_sCrashLog_Path			= NULL;
static const BYTE *		g_pCrashLog_LastQuery		= NULL;
static int				g_iCrashLog_LastQuerySize	= 0;
static BYTE				g_dCrashLog_LastHello[12];

static ESphLogLevel		g_eLogLevel		= LOG_INFO;
static int				g_iLogFile		= STDOUT_FILENO;	// log file descriptor
static CSphString		g_sLogFile;							// log file name
static bool				g_bLogTty		= false;			// cached isatty(g_iLogFile)

static int				g_iReadTimeout	= 5;	// sec
static int				g_iWriteTimeout	= 5;
static int				g_iClientTimeout = 300;
static int				g_iChildren		= 0;
static int				g_iMaxChildren	= 0;
static bool				g_bPreopenIndexes = false;
static bool				g_bOnDiskDicts	= false;
static bool				g_bUnlinkOld	= true;
static CSphVector<int>	g_dSockets;
static int				g_iQueryLogFile	= -1;
static CSphString		g_sQueryLogFile;
static const char *		g_sPidFile		= NULL;
static int				g_iPidFD		= -1;
static int				g_iMaxMatches	= 1000;

static int				g_iAttrFlushPeriod	= 0;			// in seconds; 0 means "do not flush"
static int				g_iMaxPacketSize	= 8*1024*1024;	// in bytes; for both query packets from clients and response packets from agents
static int				g_iMaxFilters		= 256;
static int				g_iMaxFilterValues	= 4096;

//////////////////////////////////////////////////////////////////////////

static CSphString		g_sConfigFile;
static DWORD			g_uCfgCRC32		= 0;
static struct stat		g_tCfgStat;

static CSphConfigParser * g_pCfg			= NULL;

#if USE_WINDOWS
static bool				g_bSeamlessRotate	= false;
#else
static bool				g_bSeamlessRotate	= true;
#endif

static bool				g_bIOStats		= false;
static bool				g_bOptConsole	= false;
static bool				g_bOptNoDetach	= false;

static volatile bool	g_bDoDelete			= false;	// do we need to delete any indexes?
static volatile bool	g_bDoRotate			= false;	// flag that we are rotating now; set from SIGHUP; cleared on rotation success
static volatile bool	g_bGotSighup		= false;	// we just received SIGHUP; need to log
static volatile bool	g_bGotSigterm		= false;	// we just received SIGTERM; need to shutdown
static volatile bool	g_bGotSigchld		= false;	// we just received SIGCHLD; need to count dead children
static volatile bool	g_bGotSigusr1		= false;	// we just received SIGUSR1; need to reopen logs

static SmallStringHash_T<ServedIndex_t>		g_hIndexes;				// served indexes hash
static CSphVector<const char *>				g_dRotating;			// names of indexes to be rotated this time
static const char *							g_sPrereading	= NULL;	// name of index currently being preread
static CSphIndex *							g_pPrereading	= NULL;	// rotation "buffer"

static int				g_iUpdateTag		= 0;		// ever-growing update tag
static bool				g_bFlushing			= false;	// update flushing in progress
static int				g_iFlushTag			= 0;		// last flushed tag

enum
{
	SPH_PIPE_UPDATED_ATTRS,
	SPH_PIPE_SAVED_ATTRS,
	SPH_PIPE_PREREAD
};

struct  PipeInfo_t
{
	int		m_iFD;			///< read-pipe to child
	int		m_iHandler;		///< who's my handler (SPH_PIPE_xxx)

	PipeInfo_t () : m_iFD ( -1 ), m_iHandler ( -1 ) {}
};

static CSphVector<PipeInfo_t>	g_dPipes;		///< currently open read-pipes to children processes

static CSphVector<DWORD>		g_dMvaStorage;	///< per-query (!) pool to store MVAs received from remote agents

/////////////////////////////////////////////////////////////////////////////

/// known commands
enum SearchdCommand_e
{
	SEARCHD_COMMAND_SEARCH		= 0,
	SEARCHD_COMMAND_EXCERPT		= 1,
	SEARCHD_COMMAND_UPDATE		= 2,
	SEARCHD_COMMAND_KEYWORDS	= 3,
	SEARCHD_COMMAND_PERSIST		= 4,
	
	SEARCHD_COMMAND_TOTAL
};


/// known command versions
enum
{
	VER_COMMAND_SEARCH		= 0x116,
	VER_COMMAND_EXCERPT		= 0x100,
	VER_COMMAND_UPDATE		= 0x102,
	VER_COMMAND_KEYWORDS	= 0x100
};


/// known status return codes
enum SearchdStatus_e
{
	SEARCHD_OK		= 0,	///< general success, command-specific reply follows
	SEARCHD_ERROR	= 1,	///< general failure, error message follows
	SEARCHD_RETRY	= 2,	///< temporary failure, error message follows, client should retry later
	SEARCHD_WARNING	= 3		///< general success, warning message and command-specific reply follow
};

const int	MAX_RETRY_COUNT		= 8;
const int	MAX_RETRY_DELAY		= 1000;

/////////////////////////////////////////////////////////////////////////////
// MACHINE-DEPENDENT STUFF
/////////////////////////////////////////////////////////////////////////////

#if USE_WINDOWS

// Windows hacks
#undef EINTR
#define LOCK_EX			0
#define LOCK_UN			1
#define STDIN_FILENO	fileno(stdin)
#define STDOUT_FILENO	fileno(stdout)
#define STDERR_FILENO	fileno(stderr)
#define ETIMEDOUT		WSAETIMEDOUT
#define EWOULDBLOCK		WSAEWOULDBLOCK
#define EINPROGRESS		WSAEINPROGRESS
#define EINTR			WSAEINTR
#define ECONNRESET		WSAECONNRESET
#define ECONNABORTED	WSAECONNABORTED
#define socklen_t		int

#define ftruncate		_chsize
#define getpid			GetCurrentProcessId

#endif // USE_WINDOWS

const int EXT_COUNT = 7;
const char * g_dNewExts[EXT_COUNT] = { ".new.sph", ".new.spa", ".new.spi", ".new.spd", ".new.spp", ".new.spm", ".new.spk" };
const char * g_dOldExts[EXT_COUNT] = { ".old.sph", ".old.spa", ".old.spi", ".old.spd", ".old.spp", ".old.spm", ".old.spk" };
const char * g_dCurExts[EXT_COUNT] = { ".sph", ".spa", ".spi", ".spd", ".spp", ".spm", ".spk" };


/////////////////////////////////////////////////////////////////////////////
// MISC
/////////////////////////////////////////////////////////////////////////////

ServedIndex_t::ServedIndex_t ()
{
	Reset ();
}

void ServedIndex_t::Reset ()
{
	m_pIndex	= NULL;
	m_bEnabled	= true;
	m_bMlock	= false;
	m_bPreopen	= false;
	m_bOnDiskDict = false;
	m_bStar		= false;
	m_bToDelete	= false;
	m_bOnlyNew	= false;
	m_iUpdateTag= 0;
}

ServedIndex_t::~ServedIndex_t ()
{
	SafeDelete ( m_pIndex );
}

/////////////////////////////////////////////////////////////////////////////
// LOGGING
/////////////////////////////////////////////////////////////////////////////

void Shutdown (); // forward ref for sphFatal()


/// format current timestamp for logging
void sphFormatCurrentTime ( char * sTimeBuf )
{
#if !USE_WINDOWS
	struct timeval tv;
	gettimeofday ( &tv, NULL );

	struct tm tmp;
	time_t ts = (time_t) tv.tv_sec; // on some systems (eg. FreeBSD 6.2), tv.tv_sec has another type and we can't just pass it
	localtime_r ( &ts, &tmp );
#else
	struct
	{
		time_t	tv_sec;
		DWORD	tv_usec;
	} tv;

	FILETIME ft;
	GetSystemTimeAsFileTime ( &ft );

	uint64_t ts = ( uint64_t(ft.dwHighDateTime)<<32 ) + uint64_t(ft.dwLowDateTime) - 116444736000000000ULL; // Jan 1, 1970 magic
	ts /= 10; // to microseconds
	tv.tv_sec  = (DWORD)(ts/1000000);
	tv.tv_usec = (DWORD)(ts%1000000);

	struct tm tmp;
	tmp = *localtime ( &tv.tv_sec );
#endif

	static const char * sWeekday[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	static const char * sMonth[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

	sprintf ( sTimeBuf, "%.3s %.3s%3d %.2d:%.2d:%.2d.%.3d %d",
		sWeekday [ tmp.tm_wday ],
		sMonth [ tmp.tm_mon ],
		tmp.tm_mday, tmp.tm_hour,
		tmp.tm_min, tmp.tm_sec, (int)(tv.tv_usec/1000),
		1900+tmp.tm_year );
}


/// physically emit log entry
/// buffer must have 1 extra byte for linefeed
void sphLogEntry ( ESphLogLevel eLevel, char * sBuf )
{
#if USE_WINDOWS
	if ( g_bService && g_iLogFile==STDOUT_FILENO )
	{
		HANDLE hEventSource;
		LPCTSTR lpszStrings[2];

		hEventSource = RegisterEventSource ( NULL, g_sServiceName );
		if ( hEventSource )
		{
			lpszStrings[0] = g_sServiceName;
			lpszStrings[1] = sBuf;

			WORD eType = EVENTLOG_INFORMATION_TYPE;
			switch ( eLevel )
			{
				case LOG_FATAL:		eType = EVENTLOG_ERROR_TYPE; break;
				case LOG_WARNING:	eType = EVENTLOG_WARNING_TYPE; break;
				case LOG_INFO:		eType = EVENTLOG_INFORMATION_TYPE; break;
			}

			ReportEvent ( hEventSource,	// event log handle
				eType,					// event type
				0,						// event category
				0,						// event identifier
				NULL,					// no security identifier
				2,						// size of lpszStrings array
				0,						// no binary data
				lpszStrings,			// array of strings
				NULL );					// no binary data

			DeregisterEventSource ( hEventSource );
		}

	} else
#endif
	{
		strcat ( sBuf, "\n" );

		lseek ( g_iLogFile, 0, SEEK_END );
		write ( g_iLogFile, sBuf, strlen(sBuf) );

		if ( g_bLogStdout && g_iLogFile!=STDOUT_FILENO )
		{
			write ( STDOUT_FILENO, sBuf, strlen(sBuf) );
		}
	}
}


/// log entry (with log levels, dupe catching, etc)
/// call with NULL format for dupe flushing
void sphLog ( ESphLogLevel eLevel, const char * sFmt, va_list ap )
{
	// dupe catcher state
	static const float	FLUSH_THRESH_TIME	= 1.0f;
	static const int	FLUSH_THRESH_COUNT	= 100;

	static ESphLogLevel eLastLevel = LOG_INFO;
	static DWORD uLastEntry = 0;
	static float fLastStamp = -1.0f-FLUSH_THRESH_TIME;
	static int iLastRepeats = 0;

	// only if we can
	if ( ( sFmt && eLevel>g_eLogLevel ) || ( g_iLogFile<0 && !g_bService ) )
		return;

	// format the banner
	char sTimeBuf[128];
	sphFormatCurrentTime ( sTimeBuf );

	const char * sBanner = "";
	if ( sFmt==NULL ) eLevel = eLastLevel;
	if ( eLevel==LOG_FATAL ) sBanner = "FATAL: ";
	if ( eLevel==LOG_WARNING ) sBanner = "WARNING: ";

	char sBuf [ 1024 ];
	if ( !g_bLogTty )
		snprintf ( sBuf, sizeof(sBuf)-1, "[%s] [%5d] %s", sTimeBuf, (int)getpid(), sBanner );
	else
		strcpy ( sBuf, sBanner );
	int iLen = strlen(sBuf);

	// format the message
	if ( sFmt )
		vsnprintf ( sBuf+iLen, sizeof(sBuf)-iLen-1, sFmt, ap );

	// catch dupes
	DWORD uEntry = sFmt ? sphCRC32 ( (const BYTE*)( sBuf+iLen ) ) : 0;
	float fNow = sphLongTimer ();

	// accumulate while possible
	if ( sFmt && eLevel==eLastLevel && uEntry==uLastEntry && iLastRepeats<FLUSH_THRESH_COUNT && fNow<fLastStamp+FLUSH_THRESH_TIME )
	{
		fLastStamp = fNow;
		iLastRepeats++;
		return;
	}

	// flush if needed
	if ( iLastRepeats!=0 && ( sFmt || fNow>=fLastStamp+FLUSH_THRESH_TIME ) )
	{
		// flush if we actually have something to flush, and
		// case 1: got a message we can't accumulate
		// case 2: got a periodic flush and been otherwise idle for a thresh period
		char sLast[256];
		strncpy ( sLast, sBuf, iLen );
		snprintf ( sLast+iLen, sizeof(sLast)-iLen, "last message repeated %d times", iLastRepeats );
		sphLogEntry ( eLastLevel, sLast );

		fLastStamp = fNow;
		iLastRepeats = 0;
		eLastLevel = LOG_INFO;
		uLastEntry = 0;
	}

	// was that a flush-only call?
	if ( !sFmt )
		return;

	fLastStamp = fNow;
	iLastRepeats = 0;
	eLastLevel = eLevel;
	uLastEntry = uEntry;

	// do the logging
	sphLogEntry ( eLevel, sBuf );
}


void sphFatal ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	sphLog ( LOG_FATAL, sFmt, ap );
	va_end ( ap );
	Shutdown ();
	exit ( 1 );
}


void sphWarning ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	sphLog ( LOG_WARNING, sFmt, ap );
	va_end ( ap );
}


void sphInfo ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	sphLog ( LOG_INFO, sFmt, ap );
	va_end ( ap );
}

void sphLogFatal ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	sphLog ( LOG_FATAL, sFmt, ap );
	va_end ( ap );
}

void LogInternalError ( const char * sError )
{
	sphWarning( "INTERNAL ERROR: %s", sError );
}

/////////////////////////////////////////////////////////////////////////////

struct StrBuf_t
{
protected:
	char		m_sBuf [ 2048 ];
	char *		m_pBuf;
	int			m_iLeft;

public:
	StrBuf_t ()
	{
		memset ( m_sBuf, 0, sizeof(m_sBuf) );
		m_iLeft = sizeof(m_sBuf)-1;
		m_pBuf = m_sBuf;
	}

	const char * cstr ()
	{
		return m_sBuf;
	}

	int GetLength ()
	{
		return sizeof(m_sBuf)-1-m_iLeft;
	}

	bool Append ( const char * s, bool bWhole )
	{
		int iLen = strlen(s);
		if ( bWhole && m_iLeft<iLen )
			return false;

		iLen = Min ( m_iLeft, iLen );
		memcpy ( m_pBuf, s, iLen );
		m_pBuf += iLen;
		m_iLeft -= iLen;
		return true;
	}

	const StrBuf_t & operator += ( const char * s )
	{
		Append ( s, false );
		return *this;
	}
};


struct SearchFailure_t
{
public:
	CSphString	m_sIndex;	///< searched index name
	CSphString	m_sError;	///< search error message

public:
	SearchFailure_t () {}

	SearchFailure_t ( const CSphString & sIndex, const CSphString & sError )
	{
		m_sIndex = sIndex;
		m_sError = sError;
		if ( m_sIndex.IsEmpty() ) m_sIndex = "(no index name)";
		if ( m_sError.IsEmpty() ) m_sError = "(no message)";
	}

public:
	bool operator == ( const SearchFailure_t & r ) const
	{
		return m_sIndex==r.m_sIndex && m_sError==r.m_sError;
	}

	bool operator < ( const SearchFailure_t & r ) const
	{
		int iRes = strcmp ( m_sError.cstr(), r.m_sError.cstr() );
		if ( !iRes )
			iRes = strcmp ( m_sIndex.cstr(), r.m_sIndex.cstr() );
		return iRes<0;
	}

	const SearchFailure_t & operator = ( const SearchFailure_t & r )
	{
		m_sIndex = r.m_sIndex;
		m_sError = r.m_sError;
		return *this;
	}
};


class SearchFailuresLog_i
{
public:
	virtual ~SearchFailuresLog_i () {}
	virtual void SetIndex ( const char * sIndex ) = 0;
	virtual void SetPrefix ( const char * sTemplate, ... ) = 0;
	virtual void Submit ( const char * sTemplate, ... ) = 0;
};


class SearchFailuresLog_c : public SearchFailuresLog_i
{
protected:
	CSphString						m_sIndex;
	CSphString						m_sPrefix;
	CSphVector<SearchFailure_t>		m_dLog;

public:
	void SetIndex ( const char * sIndex )
	{
		m_sIndex = sIndex;
	}

	void SetPrefix ( const char * sTemplate, ... )
	{
		va_list ap;
		va_start ( ap, sTemplate );
		m_sPrefix.SetSprintfVa ( sTemplate, ap );
		va_end ( ap );
	}

	void Submit ( const char * sTemplate, ... )
	{
		va_list ap;
		va_start ( ap, sTemplate );
		VaSubmit ( ap, sTemplate );
		va_end ( ap );
	}

	void SubmitEx ( const char * sIndex, const char * sTemplate, ... )
	{
		m_sIndex = sIndex;
		m_sPrefix = "";

		va_list ap;
		va_start ( ap, sTemplate );
		VaSubmit ( ap, sTemplate );
		va_end ( ap );
	}

public:
	void VaSetPrefix ( va_list ap, const char * sTemplate )
	{
		m_sPrefix.SetSprintfVa ( sTemplate, ap );
	}

	void VaSubmit ( va_list ap, const char * sTemplate )
	{
		assert ( !m_sIndex.IsEmpty() );

		char sBuf [ 2048 ];
		snprintf ( sBuf, sizeof(sBuf), "%s", m_sPrefix.IsEmpty() ? "" : m_sPrefix.cstr() );

		int iLen = strlen(sBuf);
		vsnprintf ( sBuf+iLen, sizeof(sBuf)-iLen, sTemplate, ap );

		m_dLog.Add ( SearchFailure_t ( m_sIndex, sBuf ) );
	}

public:
	bool IsEmpty ()
	{
		return m_dLog.GetLength()==0;
	}

	void BuildReport ( StrBuf_t & sReport )
	{
		if ( IsEmpty() )
			return;

		// collapse same messages
		m_dLog.Sort ();
		int iSpanStart = 0;

		for ( int i=1; i<=m_dLog.GetLength(); i++ )
		{
			// keep scanning while error text is the same
			if ( i!=m_dLog.GetLength() )
				if ( m_dLog[i].m_sError==m_dLog[i-1].m_sError )
					continue;

			// build current span
			StrBuf_t sSpan;
			if ( iSpanStart )
				sSpan += "; ";
			sSpan += "index ";
			for ( int j=iSpanStart; j<i; j++ )
			{
				if ( j!=iSpanStart )
					sSpan += ",";
				sSpan += m_dLog[j].m_sIndex.cstr();
			}
			sSpan += ": ";
			if ( !sSpan.Append ( m_dLog[iSpanStart].m_sError.cstr(), true ) )
				break;

			// flush current span
			if ( !sReport.Append ( sSpan.cstr(), true ) )
				break;

			// done
			iSpanStart = i;
		}
	}
};


class SearchFailuresLogset_c : public SearchFailuresLog_i
{
protected:
	CSphVector<SearchFailuresLog_c>		m_dLogs;
	int									m_iStart;
	int									m_iEnd;

public:
	SearchFailuresLogset_c ()
		: m_iStart ( -1 )
		, m_iEnd ( -1 )
	{}

	virtual void SetSize ( int iSize )
	{
		m_dLogs.Resize ( iSize );
	}

	virtual void SetSubset ( int iStart, int iEnd )
	{
		m_iStart = iStart;
		m_iEnd = iEnd;
	}

public:
	virtual void SetIndex ( const char * sIndex )
	{
		for ( int i=m_iStart; i<=m_iEnd; i++ )
			m_dLogs[i].SetIndex ( sIndex );
	}

	virtual void SetPrefix ( const char * sTemplate, ... )
	{
		for ( int i=m_iStart; i<=m_iEnd; i++ )
		{
			va_list ap;
			va_start ( ap, sTemplate );
			m_dLogs[i].VaSetPrefix ( ap, sTemplate );
			va_end ( ap );
		}
	}

	virtual void Submit ( const char * sTemplate, ... )
	{
		for ( int i=m_iStart; i<=m_iEnd; i++ )
		{
			va_list ap;
			va_start ( ap, sTemplate );
			m_dLogs[i].VaSubmit ( ap, sTemplate );
			va_end ( ap );
		}
	}

	SearchFailuresLog_c & operator [] ( int iIndex )
	{
		return m_dLogs[iIndex];
	}
};

/////////////////////////////////////////////////////////////////////////////
// SIGNAL HANDLERS
/////////////////////////////////////////////////////////////////////////////

void Shutdown ()
{
	// some head-only shutdown procedures
	if ( g_bHeadDaemon )
	{
		SafeDelete ( g_pCfg );

		// save attribute updates for all local indexes
		g_hIndexes.IterateStart ();
		while ( g_hIndexes.IterateNext () )
		{
			const ServedIndex_t & tServed = g_hIndexes.IterateGet ();
			if ( !tServed.m_bEnabled )
				continue;

			if ( !tServed.m_pIndex->SaveAttributes () )
				sphWarning ( "index %s: attrs save failed: %s",
				g_hIndexes.IterateGetKey().cstr(), tServed.m_pIndex->GetLastError().cstr() );
		}

		// unlock indexes
		g_hIndexes.IterateStart();
		while ( g_hIndexes.IterateNext() )
			g_hIndexes.IterateGet().m_pIndex->Unlock();
		g_hIndexes.Reset();

		sphShutdownWordforms ();

		// remove pid
		if ( g_sPidFile )
		{
			::close ( g_iPidFD );
			::unlink ( g_sPidFile );
		}
	}

	ARRAY_FOREACH ( i, g_dSockets )
		if ( g_dSockets[i]>=0 )
			sphSockClose ( g_dSockets[i] );

#if USE_WINDOWS
	CloseHandle ( g_hPipe );
#endif

	if ( g_bHeadDaemon )
		sphInfo ( "shutdown complete" );
}


#if !USE_WINDOWS

void HandleCrash ( int )
{
	static char sBuffer[1024];
	int iFd;

	if ( !g_pCrashLog_LastQuery )
		return;
	
	snprintf ( sBuffer, sizeof(sBuffer), "%s.%d", g_sCrashLog_Path, getpid() );
	if ( ( iFd = open ( sBuffer, O_WRONLY | O_CREAT | O_TRUNC, 0644 ) ) != -1 )
	{
		const int iSize = Min( g_iCrashLog_LastQuerySize, g_iMaxPacketSize );
		write ( iFd, g_dCrashLog_LastHello, sizeof(g_dCrashLog_LastHello) );
		write ( iFd, g_pCrashLog_LastQuery, iSize );
		close ( iFd );
	}
	else
		sphWarning ( "crash log creation failed, errno=%d\n", errno );
}

void sighup ( int )
{
	g_bDoRotate = true;
	g_bGotSighup = true;
}


void sigterm ( int )
{
	// in child, bail out immediately
	if ( !g_bHeadDaemon )
		exit ( 0 );

	// in head, perform a clean shutdown
	g_bGotSigterm = true;
}


void sigchld ( int )
{
	g_bGotSigchld = true;
}


void sigusr1 ( int )
{
	g_bGotSigusr1 = true;
}

#endif // !USE_WINDOWS


void SetSignalHandlers ()
{
#if !USE_WINDOWS
	struct sigaction sa;
	sigfillset ( &sa.sa_mask );
	sa.sa_flags = SA_NOCLDSTOP;

	bool bSignalsSet = false;
	for ( ;; )
	{
		sa.sa_handler = sigterm;	if ( sigaction ( SIGTERM, &sa, NULL )!=0 ) break;
		sa.sa_handler = sigterm;	if ( sigaction ( SIGINT, &sa, NULL )!=0 ) break;
		sa.sa_handler = sighup;		if ( sigaction ( SIGHUP, &sa, NULL )!=0 ) break;
		sa.sa_handler = sigusr1;	if ( sigaction ( SIGUSR1, &sa, NULL )!=0 ) break;
		sa.sa_handler = sigchld;	if ( sigaction ( SIGCHLD, &sa, NULL )!=0 ) break;
		sa.sa_handler = SIG_IGN;	if ( sigaction ( SIGPIPE, &sa, NULL )!=0 ) break;
		if ( g_bCrashLog_Enabled )
		{
			sa.sa_flags |= SA_RESETHAND;
			sa.sa_handler = HandleCrash;	if ( sigaction ( SIGSEGV, &sa, NULL )!=0 ) break;
			sa.sa_handler = HandleCrash;	if ( sigaction ( SIGBUS, &sa, NULL )!=0 ) break;
			sa.sa_handler = HandleCrash;	if ( sigaction ( SIGABRT, &sa, NULL )!=0 ) break;
			sa.sa_handler = HandleCrash;	if ( sigaction ( SIGILL, &sa, NULL )!=0 ) break;
			sa.sa_handler = HandleCrash;	if ( sigaction ( SIGFPE, &sa, NULL )!=0 ) break;
		}
		bSignalsSet = true;
		break;
	}
	if ( !bSignalsSet )
		sphFatal ( "sigaction(): %s", strerror(errno) );
#endif
}


/////////////////////////////////////////////////////////////////////////////
// NETWORK STUFF
/////////////////////////////////////////////////////////////////////////////

const int		WIN32_PIPE_BUFSIZE		= 32;


#if USE_WINDOWS

/// on Windows, the wrapper just prevents the warnings
void sphFDSet ( int fd, fd_set * fdset )
{
	#pragma warning(disable:4127) // conditional expr is const
	#pragma warning(disable:4389) // signed/unsigned mismatch

	FD_SET ( fd, fdset );

	#pragma warning(default:4127) // conditional expr is const
	#pragma warning(default:4389) // signed/unsigned mismatch
}

#else // !USE_WINDOWS

#define SPH_FDSET_OVERFLOW(_fd) ((_fd) < 0 || (_fd) >= FD_SETSIZE)

/// on UNIX, we also check that the descript won't corrupt the stack
void sphFDSet ( int fd, fd_set * set)
{
	if ( SPH_FDSET_OVERFLOW(fd) )
		sphFatal ( "sphFDSet() failed fd=%d, FD_SETSIZE=%d", fd, FD_SETSIZE );
	else
		FD_SET ( fd, set );
}

#endif // USE_WINDOWS


const char * sphSockError ( int iErr=0 )
{
	#if USE_WINDOWS
		if ( iErr==0 )
			iErr = WSAGetLastError ();

		static char sBuf [ 256 ];
		_snprintf ( sBuf, sizeof(sBuf), "WSA error %d", iErr );
		return sBuf;
	#else
		return strerror ( errno );
	#endif
}


int sphSockGetErrno ()
{
	#if USE_WINDOWS
		return WSAGetLastError();
	#else
		return errno;
	#endif
}


void sphSockSetErrno ( int iErr )
{
	#if USE_WINDOWS
		WSASetLastError ( iErr );
	#else
		errno = iErr;
	#endif
}


/// formats IP address given in network byte order into sBuffer
/// returns the buffer
char * sphFormatIP ( char * sBuffer, int iBufferSize, DWORD uAddress )
{
	const BYTE *a = (const BYTE *)&uAddress;
	snprintf ( sBuffer, iBufferSize, "%u.%u.%u.%u", a[0], a[1], a[2], a[3] );
	return sBuffer;
}


static const bool GETADDR_STRICT = true; ///< strict check, will die with sphFatal() on failure

DWORD sphGetAddress ( const char * sHost, bool bFatal=false )
{
	struct hostent * pHost = gethostbyname ( sHost );

	if ( pHost==NULL || pHost->h_addrtype!=AF_INET)
	{
		if ( bFatal )
			sphFatal ( "no AF_INET address found for: %s", sHost );
		return 0;
	}

	struct in_addr ** ppAddrs = (struct in_addr **)pHost->h_addr_list;
	assert ( ppAddrs[0] );

	assert ( sizeof(DWORD)==pHost->h_length );
	DWORD uAddr;
	memcpy ( &uAddr, ppAddrs[0], sizeof(DWORD) );

	if ( ppAddrs[1] )
	{
		char sBuf [ SPH_ADDRESS_SIZE ];
		sphWarning ( "multiple addresses found for '%s', using the first one (ip=%s)",
			sHost, sphFormatIP ( sBuf, sizeof(sBuf), uAddr ) );
	}

	return uAddr;
}


#if !USE_WINDOWS
int sphCreateUnixSocket ( const char * sPath )
{
	static struct sockaddr_un uaddr;
	size_t len = strlen ( sPath );

	if ( len + 1 > sizeof( uaddr.sun_path ) )
		sphFatal ( "UNIX socket path is too long (len=%d)", len );

	sphInfo ( "listening on UNIX socket %s", sPath );
	
	memset ( &uaddr, 0, sizeof(uaddr) );
	uaddr.sun_family = AF_UNIX;
	memcpy ( uaddr.sun_path, sPath, len + 1 );
	
	int iSock = socket ( AF_UNIX, SOCK_STREAM, 0 );
	if ( iSock == -1 )
		sphFatal ( "failed to create UNIX socket: %s", sphSockError() );

	if ( unlink ( sPath ) == -1 )
	{
		if ( errno != ENOENT )
			sphFatal ( "unlink() on UNIX socket file failed: %s", sphSockError() );
	}
	
	if ( bind ( iSock, (struct sockaddr *)&uaddr, sizeof(uaddr) ) != 0 )
		sphFatal ( "bind() on UNIX socket failed: %s", sphSockError() );

	return iSock;
}
#endif // !USE_WINDOWS


int sphCreateInetSocket ( DWORD uAddr, int iPort )
{
	char sAddress[SPH_ADDRESS_SIZE];
	sphFormatIP( sAddress, SPH_ADDRESS_SIZE, uAddr );
		
	if ( uAddr == htonl(INADDR_ANY) )
		sphInfo ( "listening on all interfaces, port=%d", iPort );
	else
		sphInfo ( "listening on %s:%d", sAddress, iPort );
	
	static struct sockaddr_in iaddr;
	memset ( &iaddr, 0, sizeof(iaddr) );
	iaddr.sin_family = AF_INET;
	iaddr.sin_addr.s_addr = uAddr;
	iaddr.sin_port = htons ( (short)iPort );

	int iSock = socket ( AF_INET, SOCK_STREAM, 0 );
	if ( iSock == -1 )
		sphFatal ( "failed to create TCP socket: %s", sphSockError() );

	int iOn = 1;
	if ( setsockopt ( iSock, SOL_SOCKET, SO_REUSEADDR, (char*)&iOn, sizeof(iOn) ) )
		sphFatal ( "setsockopt() failed: %s", sphSockError() );

	int iTries = 12;
	int iRes;
	do
	{
		iRes = bind ( iSock, (struct sockaddr *)&iaddr, sizeof(iaddr) );
		if ( iRes==0 )
			break;

		sphInfo ( "bind() failed on %s, retrying...", sAddress );
		sphUsleep ( 3000 );
	} while ( --iTries>0 );
	if ( iRes )
		sphFatal ( "bind() failed on %s: %s", sAddress, sphSockError() );

	return iSock;
}


inline bool IsPortInRange ( int iPort )
{
	return ( iPort > 0 ) && ( iPort <= 0xFFFF );
}


void CheckPort ( int iPort )
{
	if ( !IsPortInRange(iPort) )
		sphFatal ( "port %d is out of range", iPort );
}


int sphParseAndBind ( const CSphString & sListen )
{
	const char * sSpec = sListen.cstr();
		
	if ( sSpec[0]=='/' )
	{
#if USE_WINDOWS
		sphFatal ( "UNIX sockets are not supported on Windows" );
#else
		return sphCreateUnixSocket ( sListen.cstr() );
#endif
	}

	int iLen = strlen ( sSpec );
	int iColon = -1;
	bool bAllDigits = true;
	
	for ( int i = 0; i < iLen; i++ )
	{
		if ( sSpec[i] == ':' )
		{
			iColon = i;
			bAllDigits = false;
		}
		else if ( bAllDigits && !isdigit(sSpec[i]) )
			bAllDigits = false;
	}
	
	if ( bAllDigits && iLen <= 5 )
	{
		int iPort = atol(sSpec);
		if ( IsPortInRange(iPort) )
			return sphCreateInetSocket ( htonl(INADDR_ANY), iPort );
	}

	int iPort, iAddress;
	if ( iColon == -1 )
	{
		iPort = SEARCHD_DEFAULT_PORT;
		iAddress = sphGetAddress ( sSpec, GETADDR_STRICT );
	}
	else
	{
		iPort = atol ( sSpec + iColon + 1 );
		CheckPort ( iPort );
		iAddress = iColon == 0
			? htonl(INADDR_ANY)
			: sphGetAddress ( sListen.SubString ( 0, iColon ).cstr(), GETADDR_STRICT );
	}
	
	return sphCreateInetSocket ( iAddress, iPort );
}


int sphSetSockNB ( int iSock )
{
	#if USE_WINDOWS
		u_long uMode = 1;
		return ioctlsocket ( iSock, FIONBIO, &uMode );
	#else
		return fcntl ( iSock, F_SETFL, O_NONBLOCK );
	#endif
}


int sphSockRead ( int iSock, void * buf, int iLen, int iReadTimeout )
{
	assert ( iLen>0 );

	int iTimeout = 1000*Max ( 1, iReadTimeout ); // ms to wait total
	int iLeftMs = iTimeout; // ms to wait left
	int iLeftBytes = iLen; // bytes to read left
	float tmStart = sphLongTimer ();
	char * pBuf = (char*) buf;

	int iRes = -1, iErr = 0;
	while ( iLeftBytes>0 && iLeftMs>0 )
	{
		fd_set fdRead;
		FD_ZERO ( &fdRead );
		sphFDSet ( iSock, &fdRead );

		fd_set fdExcept;
		FD_ZERO ( &fdExcept );
		sphFDSet ( iSock, &fdExcept );

		struct timeval tv;
		tv.tv_sec = iLeftMs / 1000;
		tv.tv_usec = iLeftMs % 1000;

		iRes = ::select ( iSock+1, &fdRead, NULL, &fdExcept, &tv );

		// if there was EINTR, retry
		if ( iRes==-1 )
		{
			iErr = sphSockGetErrno();
			if ( iErr==EINTR )
			{
				iLeftMs = iTimeout - (int)( 1000.0f*( sphLongTimer() - tmStart ) );
				continue;
			}
			sphSockSetErrno ( iErr );
			return -1;
		}

		// if there was a timeout, report it as an error
		if ( iRes==0 )
		{
			sphSockSetErrno ( ETIMEDOUT );
			return -1;
		}

		// try to receive next chunk
		iRes = sphSockRecv ( iSock, pBuf, iLeftBytes );

		// if there was eof, we're done
		if ( iRes==0 )
		{
			sphSockSetErrno ( ECONNRESET );
			return -1;
		}

		// if there was EINTR, retry
		if ( iRes==-1 )
		{
			iErr = sphSockGetErrno();
			if ( iErr==EINTR )
			{
				iLeftMs -= (int)( 1000.0f*( sphLongTimer() - tmStart ) );
				continue;
			}
			sphSockSetErrno ( iErr );
			return -1;
		}

		// update
		pBuf += iRes;
		iLeftBytes -= iRes;
		iLeftMs = iTimeout - (int)( 1000.0f*( sphLongTimer() - tmStart ) );
	}

	// if there was a timeout, report it as an error
	if ( iLeftBytes!=0 )
	{
		sphSockSetErrno ( ETIMEDOUT );
		return -1;
	}

	return iLen;
}

/////////////////////////////////////////////////////////////////////////////
// NETWORK BUFFERS
/////////////////////////////////////////////////////////////////////////////

/// fixed-memory response buffer
/// tracks usage, and flushes to network when necessary
class NetOutputBuffer_c
{
public:
				NetOutputBuffer_c ( int iSock );

	bool		SendInt ( int iValue )			{ return SendT<int> ( htonl ( iValue ) ); }
	bool		SendDword ( DWORD iValue )		{ return SendT<DWORD> ( htonl ( iValue ) ); }
	bool		SendWord ( WORD iValue )		{ return SendT<WORD> ( htons ( iValue ) ); }
	bool		SendUint64 ( uint64_t iValue )	{ SendT<DWORD> ( htonl ( (DWORD)(iValue>>32) ) ); return SendT<DWORD> ( htonl ( (DWORD)(iValue&0xffffffffUL) ) ); }
	bool		SendFloat ( float fValue )		{ return SendT<DWORD> ( htonl ( sphF2DW ( fValue ) ) ); }

#if USE_64BIT
	bool		SendDocid ( SphDocID_t iValue )	{ return SendUint64 ( iValue ); }
#else
	bool		SendDocid ( SphDocID_t iValue )	{ return SendDword ( iValue ); }
#endif

	bool		SendString ( const char * sStr );

	bool		Flush ();
	bool		GetError () { return m_bError; }
	int			GetSentCount () { return m_iSent; }

protected:
	BYTE		m_dBuffer[8192];	///< my buffer
	BYTE *		m_pBuffer;			///< my current buffer position
	int			m_iSock;			///< my socket
	bool		m_bError;			///< if there were any write errors
	int			m_iSent;

protected:
	bool		SetError ( bool bValue );	///< set error flag
	bool		FlushIf ( int iToAdd );		///< flush if there's not enough free space to add iToAdd bytes

	bool							SendBytes ( const void * pBuf, int iLen );	///< protected to avoid network-vs-host order bugs
	template < typename T > bool	SendT ( T tValue );							///< protected to avoid network-vs-host order bugs
};


/// generic request buffer
class InputBuffer_c
{
public:
					InputBuffer_c ( const BYTE * pBuf, int iLen );
	virtual			~InputBuffer_c () {}

	int				GetInt () { return ntohl ( GetT<int> () ); }
	WORD			GetWord () { return ntohs ( GetT<WORD> () ); }
	DWORD			GetDword () { return ntohl ( GetT<DWORD> () ); }
	uint64_t		GetUint64() { uint64_t uRes = GetDword(); return (uRes<<32)+GetDword(); };
	BYTE			GetByte () { return GetT<BYTE> (); }
	float			GetFloat () { return sphDW2F ( ntohl ( GetT<DWORD> () ) ); }
	CSphString		GetString ();
	int				GetDwords ( DWORD ** pBuffer, int iMax, const char * sErrorTemplate );
	bool			GetError () { return m_bError; }

	template < typename T > bool	GetDwords ( CSphVector<T> & dBuffer, int iMax, const char * sErrorTemplate );
	template < typename T > bool	GetQwords ( CSphVector<T> & dBuffer, int iMax, const char * sErrorTemplate );

	virtual void	SendErrorReply ( const char *, ... ) = 0;

protected:
	const BYTE *	m_pBuf;
	const BYTE *	m_pCur;
	bool			m_bError;
	int				m_iLen;

protected:
	void						SetError ( bool bError ) { m_bError = bError; }
	bool						GetBytes ( void * pBuf, int iLen );
	template < typename T > T	GetT ();
};


/// simple memory request buffer
class MemInputBuffer_c : public InputBuffer_c
{
public:
					MemInputBuffer_c ( const BYTE * pBuf, int iLen ) : InputBuffer_c ( pBuf, iLen ) {}
	virtual void	SendErrorReply ( const char *, ... ) {}
};


/// simple network request buffer
class NetInputBuffer_c : public InputBuffer_c
{
public:
					NetInputBuffer_c ( int iSock );
	virtual			~NetInputBuffer_c ();

	bool			ReadFrom ( int iLen, int iTimeout );
	bool			ReadFrom ( int iLen ) { return ReadFrom ( iLen, g_iReadTimeout ); };

	virtual void	SendErrorReply ( const char *, ... );

	const BYTE *	GetBufferPtr () const { return m_pBuf; }

protected:
	static const int	NET_MINIBUFFER_SIZE = 4096;

	int					m_iSock;

	BYTE				m_dMinibufer[NET_MINIBUFFER_SIZE];
	int					m_iMaxibuffer;
	BYTE *				m_pMaxibuffer;
};

/////////////////////////////////////////////////////////////////////////////

NetOutputBuffer_c::NetOutputBuffer_c ( int iSock )
	: m_pBuffer ( m_dBuffer )
	, m_iSock ( iSock )
	, m_bError ( false )
	, m_iSent ( 0 )
{
	assert ( m_iSock>0 );
}


template < typename T > bool NetOutputBuffer_c::SendT ( T tValue )
{
	if ( m_bError )
		return false;

	FlushIf ( sizeof(T) );

	sphUnalignedWrite ( m_pBuffer, tValue );
	m_pBuffer += sizeof(T);
	assert ( m_pBuffer<m_dBuffer+sizeof(m_dBuffer) );
	return true;
}


bool NetOutputBuffer_c::SendString ( const char * sStr )
{
	if ( m_bError )
		return false;

	FlushIf ( sizeof(DWORD) );

	int iLen = strlen(sStr);
	SendInt ( iLen );
	return SendBytes ( sStr, iLen );
}


bool NetOutputBuffer_c::SendBytes ( const void * pBuf, int iLen )
{
	BYTE * pMy = (BYTE*)pBuf;
	while ( iLen>0 && !m_bError )
	{
		int iLeft = sizeof(m_dBuffer) - ( m_pBuffer - m_dBuffer );
		if ( iLen<=iLeft )
		{
			memcpy ( m_pBuffer, pMy, iLen );
			m_pBuffer += iLen;
			break;
		}

		memcpy ( m_pBuffer, pMy, iLeft );
		m_pBuffer += iLeft;
		Flush ();

		pMy += iLeft;
		iLen -= iLeft;
	}
	return !m_bError;
}


bool NetOutputBuffer_c::Flush ()
{
	if ( m_bError )
		return false;

	int iLen = m_pBuffer-m_dBuffer;
	if ( iLen==0 )
		return true;

	assert ( iLen>0 );
	assert ( iLen<=(int)sizeof(m_dBuffer) );

	char * pBuffer = (char *)&m_dBuffer[0];
	int iTimeout = g_iWriteTimeout * 1000;
	int iStart = (int)(sphLongTimer() * 1000);
	for(;;)
	{
		int iRes = sphSockSend ( m_iSock, pBuffer, iLen );
		if ( iRes < 0 )
		{
			int iErrno = sphSockGetErrno();
			if ( iErrno != EAGAIN )
			{
				sphWarning ( "send() failed: %d: %s", iErrno, sphSockError(iErrno) );
				m_bError = true;
				break;
			}
		}
		else
		{
			m_iSent += iRes;
			pBuffer += iRes;
			iLen -= iRes;
			if ( iLen==0 )
				break;
		}

		if ( iTimeout > 0 )
		{
			fd_set fdWrite;
			FD_ZERO ( &fdWrite );
			sphFDSet ( m_iSock, &fdWrite );

			struct timeval tvTimeout;
			tvTimeout.tv_sec = iTimeout / 1000;
			tvTimeout.tv_usec = iTimeout % 1000;
			
			iRes = select ( m_iSock+1, NULL, &fdWrite, NULL, &tvTimeout );
		}
		else
			iRes = 0;

		switch ( iRes )
		{
			case 1: // ready for writing
				break;

			case 0: // timed out
			{
				sphWarning ( "timed out while trying to flush network buffers" );
				m_bError = true;
				break;
			}

			case -1: // error
			{
				int iErrno = sphSockGetErrno();
				if ( iErrno == EINTR )
					break;
				sphWarning ( "select() failed: %d: %s", iErrno, sphSockError(iErrno) );
				m_bError = true;
				break;
			}
		}

		if ( m_bError )
			break;
		else
		{
			int iNow = (int)(sphLongTimer() * 1000);
			iTimeout -= iStart - iNow;
			iStart = iNow;
		}
	}

	m_pBuffer = m_dBuffer;
	return !m_bError;
}


bool NetOutputBuffer_c::FlushIf ( int iToAdd )
{
	if ( m_pBuffer+iToAdd >= m_dBuffer+sizeof(m_dBuffer) )
		return Flush ();

	return !m_bError;
}

/////////////////////////////////////////////////////////////////////////////

InputBuffer_c::InputBuffer_c ( const BYTE * pBuf, int iLen )
	: m_pBuf ( pBuf )
	, m_pCur ( pBuf )
	, m_bError ( !pBuf || iLen<0 )
	, m_iLen ( iLen )
{}


template < typename T > T InputBuffer_c::GetT ()
{
	if ( m_bError || ( m_pCur+sizeof(T) > m_pBuf+m_iLen ) )
	{
		SetError ( true );
		return 0;
	}

	T iRes = sphUnalignedRead ( *(T*)m_pCur );
	m_pCur += sizeof(T);
	return iRes;
}


CSphString InputBuffer_c::GetString ()
{
	CSphString sRes;

	int iLen = GetInt ();
	if ( m_bError || iLen<0 || iLen>g_iMaxPacketSize || ( m_pCur+iLen > m_pBuf+m_iLen ) )
	{
		SetError ( true );
		return sRes;
	}

	sRes.SetBinary ( (char*)m_pCur, iLen );
	m_pCur += iLen;
	return sRes;
}


bool InputBuffer_c::GetBytes ( void * pBuf, int iLen )
{
	assert ( pBuf );
	assert ( iLen>0 && iLen<=g_iMaxPacketSize );

	if ( m_bError || ( m_pCur+iLen > m_pBuf+m_iLen ) )
	{
		SetError ( true );
		return false;
	}

	memcpy ( pBuf, m_pCur, iLen );
	m_pCur += iLen;
	return true;
}


int InputBuffer_c::GetDwords ( DWORD ** ppBuffer, int iMax, const char * sErrorTemplate )
{
	assert ( ppBuffer );
	assert ( !(*ppBuffer) );

	int iCount = GetInt ();
	if ( iCount<0 || iCount>iMax )
	{
		SendErrorReply ( sErrorTemplate, iCount, iMax );
		SetError ( true );
		return -1;
	}
	if ( iCount )
	{
		assert ( !(*ppBuffer) ); // potential leak
		(*ppBuffer) = new DWORD [ iCount ];
		if ( !GetBytes ( (*ppBuffer), sizeof(int)*iCount ) )
		{
			SafeDeleteArray ( (*ppBuffer) );
			return -1;
		}
		for ( int i=0; i<iCount; i++ )
			(*ppBuffer)[i] = htonl ( (*ppBuffer)[i] );
	}
	return iCount;
}


template < typename T > bool InputBuffer_c::GetDwords ( CSphVector<T> & dBuffer, int iMax, const char * sErrorTemplate )
{
	int iCount = GetInt ();
	if ( iCount<0 || iCount>iMax )
	{
		SendErrorReply ( sErrorTemplate, iCount, iMax );
		SetError ( true );
		return false;
	}

	dBuffer.Resize ( iCount );
	ARRAY_FOREACH ( i, dBuffer )
		dBuffer[i] = GetDword ();

	if ( m_bError )
		dBuffer.Reset ();

	return !m_bError;
}


template < typename T > bool InputBuffer_c::GetQwords ( CSphVector<T> & dBuffer, int iMax, const char * sErrorTemplate )
{
	int iCount = GetInt ();
	if ( iCount<0 || iCount>iMax )
	{
		SendErrorReply ( sErrorTemplate, iCount, iMax );
		SetError ( true );
		return false;
	}

	dBuffer.Resize ( iCount );
	ARRAY_FOREACH ( i, dBuffer )
		dBuffer[i] = GetUint64 ();

	if ( m_bError )
		dBuffer.Reset ();

	return !m_bError;
}
/////////////////////////////////////////////////////////////////////////////

NetInputBuffer_c::NetInputBuffer_c ( int iSock )
	: InputBuffer_c ( m_dMinibufer, sizeof(m_dMinibufer) )
	, m_iSock ( iSock )
	, m_iMaxibuffer ( 0 )
	, m_pMaxibuffer ( NULL )
{}


NetInputBuffer_c::~NetInputBuffer_c ()
{
	SafeDeleteArray ( m_pMaxibuffer );
}


bool NetInputBuffer_c::ReadFrom ( int iLen, int iTimeout )
{
	if ( iLen<=0 || iLen>g_iMaxPacketSize || m_iSock<0 )
		return false;

	BYTE * pBuf = m_dMinibufer;
	if ( iLen>NET_MINIBUFFER_SIZE )
	{
		if ( iLen>m_iMaxibuffer )
		{
			SafeDeleteArray ( m_pMaxibuffer );
			m_pMaxibuffer = new BYTE [ iLen ];
			m_iMaxibuffer = iLen;
		}
		pBuf = m_pMaxibuffer;
	}

	m_pCur = m_pBuf = pBuf;
	int iGot = sphSockRead ( m_iSock, pBuf, iLen, iTimeout );

	m_bError = ( iGot!=iLen );
	m_iLen = m_bError ? 0 : iLen;
	return !m_bError;
}


void NetInputBuffer_c::SendErrorReply ( const char * sTemplate, ... )
{
	char dBuf [ 2048 ];

	const int iHeaderLen = 12;
	const int iMaxStrLen = sizeof(dBuf) - iHeaderLen - 1;

	// fill header
	WORD * p0 = (WORD*)&dBuf[0];
	p0[0] = htons(SEARCHD_ERROR); // error code
	p0[1] = 0; // version doesn't matter

	// fill error string
	char * sBuf = dBuf + iHeaderLen;

	va_list ap;
	va_start ( ap, sTemplate );
	vsnprintf ( sBuf, iMaxStrLen, sTemplate, ap );
	va_end ( ap );

	sBuf[iMaxStrLen] = '\0';
	int iStrLen = strlen(sBuf);

	// fixup lengths
	DWORD * p4 = (DWORD*)&dBuf[4];
	p4[0] = htonl(4+iStrLen);
	p4[1] = htonl(iStrLen);

	// send!
	sphSockSend ( m_iSock, dBuf, iHeaderLen+iStrLen );
}

// fix MSVC 2005 fuckup
#if USE_WINDOWS
#pragma conform(forScope,on)
#endif

/////////////////////////////////////////////////////////////////////////////
// DISTRIBUTED QUERIES
/////////////////////////////////////////////////////////////////////////////

/// remote agent state
enum AgentState_e
{
	AGENT_UNUSED,				///< agent is unused for this request
	AGENT_CONNECT,				///< connecting to agent
	AGENT_HELLO,				///< waiting for "VER x" hello
	AGENT_QUERY,				///< query sent, wating for reply
	AGENT_REPLY,				///< reading reply
	AGENT_RETRY					///< should retry
};

/// remote agent host/port
struct Agent_t
{
public:
	CSphString		m_sHost;		///< remote searchd host
	int				m_iPort;		///< remote searchd port, 0 if local
	CSphString		m_sPath;		///< local searchd UNIX socket path
	CSphString		m_sIndexes;		///< remote index names to query
	bool			m_bBlackhole;	///< blackhole agent flag

	int				m_iSock;		///< socket number, -1 if not connected
	AgentState_e	m_eState;		///< current state

	bool			m_bSuccess;		///< whether last request was succesful (ie. there are available results)
	CSphString		m_sFailure;		///< failure message

	int				m_iReplyStatus;	///< reply status code
	int				m_iReplySize;	///< how many reply bytes are there
	int				m_iReplyRead;	///< how many reply bytes are alredy received
	BYTE *			m_pReplyBuf;	///< reply buffer

	CSphVector<CSphQueryResult>		m_dResults;		///< multi-query results

	int				m_iFamily;
	DWORD			m_uAddr;

public:
	Agent_t ()
		: m_iPort ( -1 )
		, m_bBlackhole ( false )
		, m_iSock ( -1 )
		, m_eState ( AGENT_UNUSED )
		, m_bSuccess ( false )
		, m_iReplyStatus ( -1 )
		, m_iReplySize ( 0 )
		, m_iReplyRead ( 0 )
		, m_pReplyBuf ( NULL )
		, m_uAddr ( 0 )
	{
	}

	~Agent_t ()
	{
		Close ();
	}

	void Close ()
	{
		SafeDeleteArray ( m_pReplyBuf );
		if ( m_iSock>0 )
		{
			sphSockClose ( m_iSock );
			m_iSock = -1;
			if ( m_eState!=AGENT_RETRY )
				m_eState = AGENT_UNUSED;
		}
	}

	CSphString GetName() const
	{
		CSphString sName;

		switch ( m_iFamily )
		{
			case AF_INET: sName.SetSprintf ( "%s:%u", m_sHost.cstr(), m_iPort ); break;
			case AF_UNIX: sName = m_sPath; break;
		}

		return sName;
	}
};

/// distributed index
struct DistributedIndex_t
{
	CSphVector<Agent_t>			m_dAgents;					///< remote agents
	CSphVector<CSphString>		m_dLocal;					///< local indexes
	int							m_iAgentConnectTimeout;		///< in msec
	int							m_iAgentQueryTimeout;		///< in msec
	bool						m_bToDelete;				///< should be deleted

	DistributedIndex_t ()
		: m_iAgentConnectTimeout ( 1000 )
		, m_iAgentQueryTimeout ( 3000 )
		, m_bToDelete ( false )
	{}
};

static SmallStringHash_T < DistributedIndex_t >		g_hDistIndexes;

/////////////////////////////////////////////////////////////////////////////

struct IRequestBuilder_t : public ISphNoncopyable
{
	virtual ~IRequestBuilder_t () {} // to avoid gcc4 warns
	virtual void BuildRequest ( const char * sIndexes, NetOutputBuffer_c & tOut ) const = 0;
};


struct IReplyParser_t
{
	virtual ~IReplyParser_t () {} // to avoid gcc4 warns
	virtual bool ParseReply ( MemInputBuffer_c & tReq, Agent_t & tAgent ) const = 0;
};


void ConnectToRemoteAgents ( CSphVector<Agent_t> & dAgents, bool bRetryOnly )
{
	ARRAY_FOREACH ( iAgent, dAgents )
	{
		Agent_t & tAgent = dAgents[iAgent];
		if ( bRetryOnly && tAgent.m_eState!=AGENT_RETRY )
			continue;

		tAgent.m_eState = AGENT_UNUSED;
		tAgent.m_bSuccess = false;

		socklen_t len = 0;
		struct sockaddr_storage ss;
		memset ( &ss, 0, sizeof(ss) );
		ss.ss_family = (short)tAgent.m_iFamily;

		if ( ss.ss_family == AF_INET )
		{
			struct sockaddr_in *in = (struct sockaddr_in *)&ss;
			in->sin_port = htons ( (unsigned short)tAgent.m_iPort );
			in->sin_addr.s_addr = tAgent.m_uAddr;
			len = sizeof(*in);
		}
		#if !USE_WINDOWS
		else if ( ss.ss_family == AF_UNIX )
		{
			struct sockaddr_un *un = (struct sockaddr_un *)&ss;
			snprintf ( un->sun_path, sizeof(un->sun_path), tAgent.m_sPath.cstr() );
			len = sizeof(*un);
		}
		#endif
			
		tAgent.m_iSock = socket ( tAgent.m_iFamily, SOCK_STREAM, 0 );
		if ( tAgent.m_iSock<0 )
		{
			tAgent.m_sFailure.SetSprintf ( "socket() failed: %s", sphSockError() );
			return;
		}

		if ( sphSetSockNB ( tAgent.m_iSock )<0 )
		{
			tAgent.m_sFailure.SetSprintf ( "sphSetSockNB() failed: %s", sphSockError() );
			return;
		}

		if ( connect ( tAgent.m_iSock, (struct sockaddr*)&ss, len )<0 )
		{
			int iErr = sphSockGetErrno();
			if ( iErr!=EINPROGRESS && iErr!=EINTR && iErr!=EWOULDBLOCK ) // check for EWOULDBLOCK is for winsock only
			{
				tAgent.Close ();
				tAgent.m_sFailure.SetSprintf ( "connect() failed: %s", sphSockError(iErr) );
				tAgent.m_eState = AGENT_RETRY; // do retry on connect() failures
				return;

			} else
			{
				// connection in progress
				tAgent.m_eState = AGENT_CONNECT;
			}
		} else
		{
			// socket connected, ready to read hello message
			tAgent.m_eState = AGENT_HELLO;
		}
	}
}


int QueryRemoteAgents ( CSphVector<Agent_t> & dAgents, int iTimeout, const IRequestBuilder_t & tBuilder )
{
	int iAgents = 0;
	assert ( iTimeout>=0 );

	int iPassed = 0;
	float tmStart = sphLongTimer ();
	while ( iPassed<=iTimeout )
	{
		fd_set fdsRead, fdsWrite;
		FD_ZERO ( &fdsRead );
		FD_ZERO ( &fdsWrite );

		int iMax = 0;
		bool bDone = true;
		ARRAY_FOREACH ( i, dAgents )
		{
			const Agent_t & tAgent = dAgents[i];
			if ( tAgent.m_eState==AGENT_CONNECT || tAgent.m_eState==AGENT_HELLO )
			{
				assert ( !tAgent.m_sPath.IsEmpty() || tAgent.m_iPort>0 );
				assert ( tAgent.m_iSock>0 );

				sphFDSet ( tAgent.m_iSock, ( tAgent.m_eState==AGENT_CONNECT ) ? &fdsWrite : &fdsRead );
				iMax = Max ( iMax, tAgent.m_iSock );
				bDone = false;
			}
		}
		if ( bDone )
			break;

		iPassed = int ( 1000.0f*( sphLongTimer() - tmStart ) );
		int iToWait = Max ( iTimeout-iPassed, 0 );

		struct timeval tvTimeout;
		tvTimeout.tv_sec = iToWait / 1000; // full seconds
		tvTimeout.tv_usec = ( iToWait % 1000 ) * 1000; // remainder is msec, so *1000 for usec

		// FIXME! check exceptfds for connect() failure as well, so that actively refused
		// connections would not stall for a full timeout
		if ( select ( 1+iMax, &fdsRead, &fdsWrite, NULL, &tvTimeout )<=0 )
			continue;

		ARRAY_FOREACH ( i, dAgents )
		{
			Agent_t & tAgent = dAgents[i];

			// check if connection completed
			if ( tAgent.m_eState==AGENT_CONNECT && FD_ISSET ( tAgent.m_iSock, &fdsWrite ) )
			{
				int iErr = 0;
				socklen_t iErrLen = sizeof(iErr);
				getsockopt ( tAgent.m_iSock, SOL_SOCKET, SO_ERROR, (char*)&iErr, &iErrLen );
				if ( iErr )
				{
					// connect() failure
					tAgent.m_sFailure.SetSprintf ( "connect() failed: %s", sphSockError(iErr) );
					tAgent.Close ();
				} else
				{
					// connect() success
					tAgent.m_eState = AGENT_HELLO;
				}
				continue;
			}

			// check if hello was received
			if ( tAgent.m_eState==AGENT_HELLO && FD_ISSET ( tAgent.m_iSock, &fdsRead ) )
			{
				// read reply
				int iRemoteVer;
				int iRes = sphSockRecv ( tAgent.m_iSock, (char*)&iRemoteVer, sizeof(iRemoteVer) );
				iRemoteVer = ntohl ( iRemoteVer );
				if ( iRes!=sizeof(iRemoteVer) || iRemoteVer<=0 )
				{
					tAgent.m_sFailure.SetSprintf ( "expected protocol v.%d, got v.%d", SPHINX_SEARCHD_PROTO, iRemoteVer );
					tAgent.Close ();
					continue;
				}

				// send request
				NetOutputBuffer_c tOut ( tAgent.m_iSock );
				tBuilder.BuildRequest ( tAgent.m_sIndexes.cstr(), tOut );
				tOut.Flush (); // FIXME! handle flush failure?

				tAgent.m_eState = AGENT_QUERY;
				iAgents++;
			}
		}
	}

	ARRAY_FOREACH ( i, dAgents )
	{
		// check if connection timed out
		Agent_t & tAgent = dAgents[i];
		if ( tAgent.m_eState!=AGENT_QUERY && tAgent.m_eState!=AGENT_UNUSED )
		{
			tAgent.Close ();
			tAgent.m_sFailure.SetSprintf ( "%s() timed out", tAgent.m_eState==AGENT_HELLO ? "read" : "connect" );
			tAgent.m_eState = AGENT_RETRY; // do retry on connect() failures
		}
	}

	return iAgents;
}


int WaitForRemoteAgents ( CSphVector<Agent_t> & dAgents, int iTimeout, IReplyParser_t & tParser )
{
	assert ( iTimeout>=0 );

	int iAgents = 0;
	int iPassed = 0;
	float tmStart = sphLongTimer ();
	while ( iPassed<=iTimeout )
	{
		fd_set fdsRead;
		FD_ZERO ( &fdsRead );

		int iMax = 0;
		bool bDone = true;
		ARRAY_FOREACH ( iAgent, dAgents )
		{
			Agent_t & tAgent = dAgents[iAgent];
			if ( tAgent.m_bBlackhole )
				continue;

			if ( tAgent.m_eState==AGENT_QUERY || tAgent.m_eState==AGENT_REPLY )
			{
				assert ( !tAgent.m_sPath.IsEmpty() || tAgent.m_iPort>0 );
				assert ( tAgent.m_iSock>0 );

				sphFDSet ( tAgent.m_iSock, &fdsRead );
				iMax = Max ( iMax, tAgent.m_iSock );
				bDone = false;
			}
		}
		if ( bDone )
			break;

		iPassed = int ( 1000.0f*( sphLongTimer() - tmStart ) );
		int iToWait = Max ( iTimeout-iPassed, 0 );

		struct timeval tvTimeout;
		tvTimeout.tv_sec = iToWait / 1000; // full seconds
		tvTimeout.tv_usec = ( iToWait % 1000 ) * 1000; // remainder is msec, so *1000 for usec

		if ( select ( 1+iMax, &fdsRead, NULL, NULL, &tvTimeout )<=0 )
			continue;

		ARRAY_FOREACH ( iAgent, dAgents )
		{
			Agent_t & tAgent = dAgents[iAgent];
			if ( tAgent.m_bBlackhole )
				continue;
			if (!( tAgent.m_eState==AGENT_QUERY || tAgent.m_eState==AGENT_REPLY ))
				continue;
			if ( !FD_ISSET ( tAgent.m_iSock, &fdsRead ) )
				continue;

			// if there was no reply yet, read reply header
			bool bFailure = true;
			for ( ;; )
			{
				if ( tAgent.m_eState==AGENT_QUERY )
				{
					// try to read
					struct
					{
						WORD	m_iStatus;
						WORD	m_iVer;
						int		m_iLength;
					} tReplyHeader;
					STATIC_SIZE_ASSERT ( tReplyHeader, 8 );

					if ( sphSockRecv ( tAgent.m_iSock, (char*)&tReplyHeader, sizeof(tReplyHeader) )!=sizeof(tReplyHeader) )
					{
						// bail out if failed
						tAgent.m_sFailure.SetSprintf ( "failed to receive reply header" );
						break;
					}

					tReplyHeader.m_iStatus = ntohs ( tReplyHeader.m_iStatus );
					tReplyHeader.m_iVer = ntohs ( tReplyHeader.m_iVer );
					tReplyHeader.m_iLength = ntohl ( tReplyHeader.m_iLength );

					// check the packet
					if ( tReplyHeader.m_iLength<0 || tReplyHeader.m_iLength>g_iMaxPacketSize ) // FIXME! add reasonable max packet len too
					{
						tAgent.m_sFailure.SetSprintf ( "invalid packet size (status=%d, len=%d, max_packet_size=%d)", tReplyHeader.m_iStatus, tReplyHeader.m_iLength, g_iMaxPacketSize );
						break;
					}

					// header received, switch the status
					assert ( tAgent.m_pReplyBuf==NULL );
					tAgent.m_eState = AGENT_REPLY;
					tAgent.m_pReplyBuf = new BYTE [ tReplyHeader.m_iLength ];
					tAgent.m_iReplySize = tReplyHeader.m_iLength;
					tAgent.m_iReplyRead = 0;
					tAgent.m_iReplyStatus = tReplyHeader.m_iStatus;

					if ( !tAgent.m_pReplyBuf )
					{
						// bail out if failed
						tAgent.m_sFailure.SetSprintf ( "failed to alloc %d bytes for reply buffer", tAgent.m_iReplySize );
						break;
					}
				}

				// if we are reading reply, read another chunk
				if ( tAgent.m_eState==AGENT_REPLY )
				{
					// do read
					assert ( tAgent.m_iReplyRead<tAgent.m_iReplySize );
					int iRes = sphSockRecv ( tAgent.m_iSock, (char*)tAgent.m_pReplyBuf+tAgent.m_iReplyRead,
						tAgent.m_iReplySize-tAgent.m_iReplyRead );

					// bail out if read failed
					if ( iRes<0 )
					{
						tAgent.m_sFailure.SetSprintf ( "failed to receive reply body: %s", sphSockError() );
						break;
					}

					assert ( iRes>0 );
					assert ( tAgent.m_iReplyRead+iRes<=tAgent.m_iReplySize );
					tAgent.m_iReplyRead += iRes;
				}

				// if reply was fully received, parse it
				if ( tAgent.m_eState==AGENT_REPLY && tAgent.m_iReplyRead==tAgent.m_iReplySize )
				{
					MemInputBuffer_c tReq ( tAgent.m_pReplyBuf, tAgent.m_iReplySize );

					// absolve thy former sins
					tAgent.m_sFailure = "";

					// check for general errors/warnings first
					if ( tAgent.m_iReplyStatus==SEARCHD_WARNING )
					{
						CSphString sAgentWarning = tReq.GetString ();
						tAgent.m_sFailure.SetSprintf ( "remote warning: %s", sAgentWarning.cstr() );

					} else if ( tAgent.m_iReplyStatus==SEARCHD_RETRY )
					{
						tAgent.m_eState = AGENT_RETRY;
						break;

					} else if ( tAgent.m_iReplyStatus!=SEARCHD_OK )
					{
						CSphString sAgentError = tReq.GetString ();
						tAgent.m_sFailure.SetSprintf ( "remote error: %s", sAgentError.cstr() );
						break;
					}

					// call parser
					if ( !tParser.ParseReply ( tReq, tAgent ) )
						break;

					// check if there was enough data
					if ( tReq.GetError() )
					{
						tAgent.m_sFailure.SetSprintf ( "incomplete reply" );
						break;
					}

					// all is well
					iAgents++;
					tAgent.Close ();
				
					tAgent.m_bSuccess = true;
				}

				bFailure = false;
				break;
			}

			if ( bFailure )
			{
				tAgent.Close ();
				tAgent.m_dResults.Reset ();
			}
		}
	}

	// close timed-out agents
	ARRAY_FOREACH ( iAgent, dAgents )
	{
		Agent_t & tAgent = dAgents[iAgent];
		if ( tAgent.m_bBlackhole )
			tAgent.Close ();
		else if ( tAgent.m_eState==AGENT_QUERY )
		{
			assert ( !tAgent.m_dResults.GetLength() );
			assert ( !tAgent.m_bSuccess );
			tAgent.Close ();
			tAgent.m_sFailure.SetSprintf ( "query timed out" );
		}
	}

	return iAgents;
}

/////////////////////////////////////////////////////////////////////////////
// SEARCH HANDLER
/////////////////////////////////////////////////////////////////////////////

inline bool operator < ( const CSphMatch & a, const CSphMatch & b )
{
	if ( a.m_iDocID==b.m_iDocID )
		return a.m_iTag > b.m_iTag;
	else
		return a.m_iDocID < b.m_iDocID;
};

/////////////////////////////////////////////////////////////////////////////

struct SearchRequestBuilder_t : public IRequestBuilder_t
{
						SearchRequestBuilder_t ( const CSphVector<CSphQuery> & dQueries, int iStart, int iEnd ) : m_dQueries ( dQueries ), m_iStart ( iStart ), m_iEnd ( iEnd ) {}
	virtual void		BuildRequest ( const char * sIndexes, NetOutputBuffer_c & tOut ) const;

protected:
	int					CalcQueryLen ( const char * sIndexes, const CSphQuery & q ) const;
	void				SendQuery ( const char * sIndexes, NetOutputBuffer_c & tOut, const CSphQuery & q ) const;

protected:
	const CSphVector<CSphQuery> &		m_dQueries;
	int									m_iStart;
	int									m_iEnd;
};


struct SearchReplyParser_t : public IReplyParser_t
{
						SearchReplyParser_t ( int iStart, int iEnd ) : m_iStart ( iStart ), m_iEnd ( iEnd ) {}
	virtual bool		ParseReply ( MemInputBuffer_c & tReq, Agent_t & tAgent ) const;

protected:
	int					m_iStart;
	int					m_iEnd;
};

/////////////////////////////////////////////////////////////////////////////

int SearchRequestBuilder_t::CalcQueryLen ( const char * sIndexes, const CSphQuery & q ) const
{
	int iReqSize = 104 + 2*sizeof(SphDocID_t) + 4*q.m_iWeights
		+ strlen ( q.m_sSortBy.cstr() )
		+ strlen ( q.m_sQuery.cstr() )
		+ strlen ( sIndexes )
		+ strlen ( q.m_sGroupBy.cstr() )
		+ strlen ( q.m_sGroupSortBy.cstr() )
		+ strlen ( q.m_sGroupDistinct.cstr() )
		+ strlen ( q.m_sComment.cstr() )
		+ strlen ( q.m_sSelect.cstr() );
	ARRAY_FOREACH ( j, q.m_dFilters )
	{
		const CSphFilterSettings & tFilter = q.m_dFilters[j];
		iReqSize += 12 + strlen ( tFilter.m_sAttrName.cstr() ); // string attr-name; int type; int exclude-flag
		switch ( tFilter.m_eType )
		{
			case SPH_FILTER_VALUES:		iReqSize += 4 + 8*tFilter.GetNumValues (); break; // int values-count; uint64[] values
			case SPH_FILTER_RANGE:		iReqSize += 16; break; // uint64 min-val, max-val
			case SPH_FILTER_FLOATRANGE:	iReqSize += 8; break; // int/float min-val,max-val
		}
	}
	if ( q.m_bGeoAnchor )
		iReqSize += 16 + strlen ( q.m_sGeoLatAttr.cstr() ) + strlen ( q.m_sGeoLongAttr.cstr() ); // string lat-attr, long-attr; float lat, long
	ARRAY_FOREACH ( i, q.m_dIndexWeights )
		iReqSize += 8 + strlen ( q.m_dIndexWeights[i].m_sName.cstr() ); // string index-name; int index-weight
	ARRAY_FOREACH ( i, q.m_dFieldWeights )
		iReqSize += 8 + strlen ( q.m_dFieldWeights[i].m_sName.cstr() ); // string field-name; int field-weight
	ARRAY_FOREACH ( i, q.m_dOverrides )
		iReqSize += 12 + strlen ( q.m_dOverrides[i].m_sAttr.cstr() ) + // string attr-name; int type; int values-count
			( q.m_dOverrides[i].m_uAttrType==SPH_ATTR_BIGINT ? 16 : 12 )*q.m_dOverrides[i].m_dValues.GetLength(); // ( bigint id; int/float/bigint value )[] values
	return iReqSize;
}


void SearchRequestBuilder_t::SendQuery ( const char * sIndexes, NetOutputBuffer_c & tOut, const CSphQuery & q ) const
{
	tOut.SendInt ( 0 ); // offset is 0
	tOut.SendInt ( q.m_iMaxMatches ); // limit is MAX_MATCHES
	tOut.SendInt ( (DWORD)q.m_eMode ); // match mode
	tOut.SendInt ( (DWORD)q.m_eRanker ); // ranking mode
	tOut.SendInt ( q.m_eSort ); // sort mode
	tOut.SendString ( q.m_sSortBy.cstr() ); // sort attr
	tOut.SendString ( q.m_sQuery.cstr() ); // query
	tOut.SendInt ( q.m_iWeights );
	for ( int j=0; j<q.m_iWeights; j++ )
		tOut.SendInt ( q.m_pWeights[j] ); // weights
	tOut.SendString ( sIndexes ); // indexes
	tOut.SendInt ( USE_64BIT ); // id range bits
	tOut.SendDocid ( q.m_iMinID ); // id/ts ranges
	tOut.SendDocid ( q.m_iMaxID );
	tOut.SendInt ( q.m_dFilters.GetLength() );
	ARRAY_FOREACH ( j, q.m_dFilters )
	{
		const CSphFilterSettings & tFilter = q.m_dFilters[j];
		tOut.SendString ( tFilter.m_sAttrName.cstr() );
		tOut.SendInt ( tFilter.m_eType );
		switch ( tFilter.m_eType )
		{
			case SPH_FILTER_VALUES:
				tOut.SendInt ( tFilter.GetNumValues () );
				for ( int k = 0; k < tFilter.GetNumValues (); k++ )
					tOut.SendUint64 ( tFilter.GetValue ( k ) );
				break;

			case SPH_FILTER_RANGE:
				tOut.SendUint64 ( tFilter.m_uMinValue );
				tOut.SendUint64 ( tFilter.m_uMaxValue );
				break;

			case SPH_FILTER_FLOATRANGE:
				tOut.SendFloat ( tFilter.m_fMinValue );
				tOut.SendFloat ( tFilter.m_fMaxValue );
				break;
		}
		tOut.SendInt ( tFilter.m_bExclude );
	}
	tOut.SendInt ( q.m_eGroupFunc );
	tOut.SendString ( q.m_sGroupBy.cstr() );
	tOut.SendInt ( q.m_iMaxMatches );
	tOut.SendString ( q.m_sGroupSortBy.cstr() );
	tOut.SendInt ( q.m_iCutoff );
	tOut.SendInt ( q.m_iRetryCount );
	tOut.SendInt ( q.m_iRetryDelay );
	tOut.SendString ( q.m_sGroupDistinct.cstr() );
	tOut.SendInt ( q.m_bGeoAnchor );
	if ( q.m_bGeoAnchor )
	{
		tOut.SendString ( q.m_sGeoLatAttr.cstr() );
		tOut.SendString ( q.m_sGeoLongAttr.cstr() );
		tOut.SendFloat ( q.m_fGeoLatitude );
		tOut.SendFloat ( q.m_fGeoLongitude );
	}
	tOut.SendInt ( q.m_dIndexWeights.GetLength() );
	ARRAY_FOREACH ( i, q.m_dIndexWeights )
	{
		tOut.SendString ( q.m_dIndexWeights[i].m_sName.cstr() );
		tOut.SendInt ( q.m_dIndexWeights[i].m_iValue );
	}
	tOut.SendDword ( q.m_uMaxQueryMsec );
	tOut.SendInt ( q.m_dFieldWeights.GetLength() );
	ARRAY_FOREACH ( i, q.m_dFieldWeights )
	{
		tOut.SendString ( q.m_dFieldWeights[i].m_sName.cstr() );
		tOut.SendInt ( q.m_dFieldWeights[i].m_iValue );
	}
	tOut.SendString ( q.m_sComment.cstr() );
	tOut.SendInt ( q.m_dOverrides.GetLength() );
	ARRAY_FOREACH ( i, q.m_dOverrides )
	{
		const CSphAttrOverride & tEntry = q.m_dOverrides[i];
		tOut.SendString ( tEntry.m_sAttr.cstr() );
		tOut.SendDword ( tEntry.m_uAttrType );
		tOut.SendInt ( tEntry.m_dValues.GetLength() );
		ARRAY_FOREACH ( j, tEntry.m_dValues )
		{
			tOut.SendUint64 ( tEntry.m_dValues[j].m_uDocID );
			switch ( tEntry.m_uAttrType )
			{
				case SPH_ATTR_FLOAT:	tOut.SendFloat ( tEntry.m_dValues[j].m_fValue ); break;
				case SPH_ATTR_BIGINT:	tOut.SendUint64 ( tEntry.m_dValues[j].m_uValue ); break;
				default:				tOut.SendDword ( (DWORD)tEntry.m_dValues[j].m_uValue ); break;
			}
		}
	}
	tOut.SendString ( q.m_sSelect.cstr() );
}


void SearchRequestBuilder_t::BuildRequest ( const char * sIndexes, NetOutputBuffer_c & tOut ) const
{
	int iReqLen = 4; // int num-queries
	for ( int i=m_iStart; i<=m_iEnd; i++ )
		iReqLen += CalcQueryLen ( sIndexes, m_dQueries[i] );

	tOut.SendDword ( SPHINX_SEARCHD_PROTO );
	tOut.SendWord ( SEARCHD_COMMAND_SEARCH ); // command id
	tOut.SendWord ( VER_COMMAND_SEARCH ); // command version
	tOut.SendInt ( iReqLen ); // request body length

	tOut.SendInt ( m_iEnd-m_iStart+1 );
	for ( int i=m_iStart; i<=m_iEnd; i++ )
		SendQuery ( sIndexes, tOut, m_dQueries[i] );
}

/////////////////////////////////////////////////////////////////////////////

bool SearchReplyParser_t::ParseReply ( MemInputBuffer_c & tReq, Agent_t & tAgent ) const
{
	int iResults = m_iEnd-m_iStart+1;
	assert ( iResults>0 );

	tAgent.m_dResults.Resize ( iResults );
	for ( int iRes=0; iRes<iResults; iRes++ )
		tAgent.m_dResults[iRes].m_iSuccesses = 0;

	for ( int iRes=0; iRes<iResults; iRes++ )
	{
		CSphQueryResult & tRes = tAgent.m_dResults [ iRes ];
		tRes.m_sError = "";
		tRes.m_sWarning = "";

		// get status and message
		DWORD eStatus = tReq.GetDword ();
		if ( eStatus!=SEARCHD_OK )
		{
			CSphString sMessage = tReq.GetString ();
			switch ( eStatus )
			{
				case SEARCHD_ERROR:		tRes.m_sError = sMessage; continue;
				case SEARCHD_RETRY:		tRes.m_sError = sMessage; break;
				case SEARCHD_WARNING:	tRes.m_sWarning = sMessage; break;
				default:				tAgent.m_sFailure.SetSprintf ( "internal error: unknown status %d", eStatus ); break;
			}
		}

		// get schema
		CSphSchema & tSchema = tRes.m_tSchema;
		tSchema.Reset ();

		tSchema.m_dFields.Resize ( tReq.GetInt() ); // FIXME! add a sanity check
		ARRAY_FOREACH ( j, tSchema.m_dFields )
			tSchema.m_dFields[j].m_sName = tReq.GetString ();

		int iNumAttrs = tReq.GetInt(); // FIXME! add a sanity check
		for ( int j=0; j<iNumAttrs; j++ )
		{
			CSphColumnInfo tCol;
			tCol.m_sName = tReq.GetString ();
			tCol.m_eAttrType = tReq.GetDword (); // FIXME! add a sanity check
			tSchema.AddAttr ( tCol );
		}

		// get matches
		int iMatches = tReq.GetInt ();
		if ( iMatches<0 || iMatches>g_iMaxMatches )
		{
			tAgent.m_sFailure.SetSprintf ( "invalid match count received (count=%d)", iMatches );
			return false;
		}

		int bAgent64 = tReq.GetInt ();
#if !USE_64BIT
		if ( bAgent64 )
			tAgent.m_sFailure.SetSprintf ( "id64 agent, id32 master, docids might be wrapped" );
#endif

		assert ( !tRes.m_dMatches.GetLength() );
		if ( iMatches )
		{
			tRes.m_dMatches.Resize ( iMatches );
			ARRAY_FOREACH ( i, tRes.m_dMatches )
			{
				CSphMatch & tMatch = tRes.m_dMatches[i];
				tMatch.Reset ( tSchema.GetRowSize() );
				tMatch.m_iDocID = bAgent64 ? (SphDocID_t)tReq.GetUint64() : tReq.GetDword();
				tMatch.m_iWeight = tReq.GetInt ();
				for ( int j=0; j<tSchema.GetAttrsCount(); j++ )
				{
					const CSphColumnInfo & tAttr = tSchema.GetAttr(j);
					if ( tAttr.m_eAttrType & SPH_ATTR_MULTI )
					{
						tMatch.SetAttr ( tAttr.m_tLocator, g_dMvaStorage.GetLength() );

						int iValues = tReq.GetDword ();
						g_dMvaStorage.Add ( iValues );
						while ( iValues-- )
							g_dMvaStorage.Add ( tReq.GetDword() );

					}
					else if ( tAttr.m_eAttrType == SPH_ATTR_FLOAT )
					{
						float fRes = tReq.GetFloat();
						tMatch.SetAttr ( tAttr.m_tLocator, sphF2DW(fRes) );
					}
					else if ( tAttr.m_eAttrType == SPH_ATTR_BIGINT )
					{
						tMatch.SetAttr ( tAttr.m_tLocator, tReq.GetUint64() );
					}
					else
					{
						tMatch.SetAttr ( tAttr.m_tLocator, tReq.GetDword() );
					}
				}
			}
		}

		// read totals (retrieved count, total count, query time, word count)
		int iRetrieved = tReq.GetInt ();
		tRes.m_iTotalMatches = tReq.GetInt ();
		tRes.m_iQueryTime = tReq.GetInt ();
		tRes.m_iNumWords = tReq.GetInt ();
		if ( iRetrieved!=iMatches )
		{
			tAgent.m_sFailure.SetSprintf ( "expected %d retrieved documents, got %d", iMatches, iRetrieved );
			return false;
		}

		// read per-word stats
		for ( int i=0; i<tRes.m_iNumWords; i++ )
		{
			CSphString sWord = tReq.GetString ();
			int iDocs = tReq.GetInt ();
			int iHits = tReq.GetInt ();

			if ( i<SPH_MAX_QUERY_WORDS )
			{
				tRes.m_tWordStats[i].m_sWord = sWord;
				tRes.m_tWordStats[i].m_iDocs = iDocs;
				tRes.m_tWordStats[i].m_iHits = iHits;
			}
		}

		// mark this result as ok
		tRes.m_iSuccesses = 1;
	}

	// all seems OK (and buffer length checks are performed by caller)
	return true;
}

/////////////////////////////////////////////////////////////////////////////

// returns true if incoming schema (src) is equal to existing (dst); false otherwise
bool MinimizeSchema ( CSphSchema & tDst, const CSphSchema & tSrc )
{
	// if dst is empty, result is also empty
	if ( tDst.GetAttrsCount()==0 )
		return tSrc.GetAttrsCount()==0;

	// check for equality, and remove all dst attributes that are not present in src
	CSphVector<CSphColumnInfo> dDst;
	for ( int i=0; i<tDst.GetAttrsCount(); i++ )
		dDst.Add ( tDst.GetAttr(i) );

	bool bEqual = ( tDst.GetAttrsCount()==tSrc.GetAttrsCount() );
	ARRAY_FOREACH ( i, dDst )
	{
		int iSrcIdx = tSrc.GetAttrIndex ( dDst[i].m_sName.cstr() );

		// check for index mismatch
		if ( iSrcIdx!=i )
			bEqual = false;

		// check for type/size mismatch (and fixup if needed)
		if ( iSrcIdx>=0 )
		{
			const CSphColumnInfo & tSrcAttr = tSrc.GetAttr(iSrcIdx);
			if ( tSrcAttr.m_eAttrType!=dDst[i].m_eAttrType )
			{
				// different types? remove the attr
				iSrcIdx = -1;
				bEqual = false;

			} else if ( tSrcAttr.m_tLocator.m_iBitCount!=dDst[i].m_tLocator.m_iBitCount )
			{
				// different bit sizes? choose the max one
				dDst[i].m_tLocator.m_iBitCount = Max ( dDst[i].m_tLocator.m_iBitCount, tSrcAttr.m_tLocator.m_iBitCount );
				bEqual = false;
			}
		}

		// check for presence
		if ( iSrcIdx<0 )
		{
			dDst.Remove ( i );
			i--;
		}
	}

	tDst.ResetAttrs ();
	ARRAY_FOREACH ( i, dDst )
		tDst.AddAttr ( dDst[i] );

	return bEqual;
}


bool FixupQuery ( CSphQuery * pQuery, const CSphSchema * pSchema, const char * sIndexName, CSphString & sError )
{
	// already?
	if ( !pQuery->m_iOldVersion )
		return true;

	if ( pQuery->m_iOldGroups>0 || pQuery->m_iOldMinGID!=0 || pQuery->m_iOldMaxGID!=UINT_MAX )
	{
		int iAttr = -1;
		for ( int i=0; i<pSchema->GetAttrsCount(); i++ ) 
			if ( pSchema->GetAttr(i).m_eAttrType==SPH_ATTR_INTEGER )
		{
			iAttr = i;
			break;
		}

		if ( iAttr<0 )
		{
			sError.SetSprintf ( "index '%s': no group attribute found", sIndexName );
			return false;
		}

		CSphFilterSettings tFilter;
		tFilter.m_sAttrName = pSchema->GetAttr(iAttr).m_sName;
		tFilter.m_dValues.Resize ( pQuery->m_iOldGroups );
		ARRAY_FOREACH ( i, tFilter.m_dValues )
			tFilter.m_dValues[i] = pQuery->m_pOldGroups[i];
		tFilter.m_uMinValue = pQuery->m_iOldMinGID;
		tFilter.m_uMaxValue = pQuery->m_iOldMaxGID;
		pQuery->m_dFilters.Add ( tFilter );
	}

	if ( pQuery->m_iOldMinTS!=0 || pQuery->m_iOldMaxTS!=UINT_MAX )
	{
		int iAttr = -1;
		for ( int i=0; i<pSchema->GetAttrsCount(); i++ ) 
			if ( pSchema->GetAttr(i).m_eAttrType==SPH_ATTR_TIMESTAMP )
		{
			iAttr = i;
			break;
		}

		if ( iAttr<0 )
		{
			sError.SetSprintf ( "index '%s': no timestamp attribute found", sIndexName );
			return false;
		}

		CSphFilterSettings tFilter;
		tFilter.m_sAttrName = pSchema->GetAttr(iAttr).m_sName;
		tFilter.m_uMinValue = pQuery->m_iOldMinTS;
		tFilter.m_uMaxValue = pQuery->m_iOldMaxTS;
		pQuery->m_dFilters.Add ( tFilter );
	}

	pQuery->m_iOldVersion = 0;
	return true;
}


void ParseIndexList ( const CSphString & sIndexes, CSphVector<CSphString> & dOut )
{
	CSphString sSplit = sIndexes;
	char * p = (char*)sSplit.cstr();
	while ( *p )
	{
		// skip non-alphas
		while ( (*p) && !sphIsAlpha(*p) ) p++;
		if ( !(*p) ) break;

		// this is my next index name
		const char * sNext = p;
		while ( sphIsAlpha(*p) ) p++;

		assert ( sNext!=p );
		if ( *p ) *p++ = '\0'; // if it was not the end yet, we'll continue from next char

		dOut.Add ( sNext );
	}
}


bool ParseSearchQuery ( InputBuffer_c & tReq, CSphQuery & tQuery, int iVer )
{
	tQuery.m_iOldVersion = iVer;

	// v.1.0. mode, limits, weights, ID/TS ranges
	tQuery.m_iOffset	= tReq.GetInt ();
	tQuery.m_iLimit		= tReq.GetInt ();
	tQuery.m_eMode		= (ESphMatchMode) tReq.GetInt ();
	if ( iVer>=0x110 )
		tQuery.m_eRanker= (ESphRankMode) tReq.GetInt ();
	tQuery.m_eSort		= (ESphSortOrder) tReq.GetInt ();
	if ( iVer<=0x101 )
		tQuery.m_iOldGroups = tReq.GetDwords ( &tQuery.m_pOldGroups, g_iMaxFilterValues, "invalid group count %d (should be in 0..%d range)" );
	if ( iVer>=0x102 )
	{
		tQuery.m_sSortBy = tReq.GetString ();
		tQuery.m_sSortBy.ToLower ();
	}
	tQuery.m_sQuery		= tReq.GetString ();
	tQuery.m_iWeights	= tReq.GetDwords ( (DWORD**)&tQuery.m_pWeights, SPH_MAX_FIELDS, "invalid weight count %d (should be in 0..%d range)" );
	tQuery.m_sIndexes	= tReq.GetString ();

	bool bIdrange64 = false;
	if ( iVer>=0x108 )
		bIdrange64 = ( tReq.GetInt()!=0 );

	if ( bIdrange64 )
	{
		tQuery.m_iMinID		= (SphDocID_t)tReq.GetUint64 ();
		tQuery.m_iMaxID		= (SphDocID_t)tReq.GetUint64 ();
		// FIXME? could report clamp here if I'm id32 and client passed id64 range,
		// but frequently this won't affect anything at all
	} else
	{
		tQuery.m_iMinID		= tReq.GetDword ();
		tQuery.m_iMaxID		= tReq.GetDword ();
	}

	if ( iVer<0x108 && tQuery.m_iMaxID==0xffffffffUL )
		tQuery.m_iMaxID = 0; // fixup older clients which send 32-bit UINT_MAX by default

	if ( tQuery.m_iMaxID==0 )
		tQuery.m_iMaxID = DOCID_MAX;

	// v.1.0, v.1.1
	if ( iVer<=0x101 )
	{
		tQuery.m_iOldMinTS = tReq.GetDword ();
		tQuery.m_iOldMaxTS = tReq.GetDword ();
	}

	// v.1.1 specific
	if ( iVer==0x101 )
	{
		tQuery.m_iOldMinGID = tReq.GetDword ();
		tQuery.m_iOldMaxGID = tReq.GetDword ();
	}

	// v.1.2
	if ( iVer>=0x102 )
	{
		int iAttrFilters = tReq.GetInt ();
		if ( iAttrFilters>g_iMaxFilters )
		{
			tReq.SendErrorReply ( "too much attribute filters (req=%d, max=%d)", iAttrFilters, g_iMaxFilters );
			return false;
		}

		tQuery.m_dFilters.Resize ( iAttrFilters );
		ARRAY_FOREACH ( iFilter, tQuery.m_dFilters )
		{
			CSphFilterSettings & tFilter = tQuery.m_dFilters[iFilter];
			tFilter.m_sAttrName = tReq.GetString ();
			tFilter.m_sAttrName.ToLower ();

			if ( iVer>=0x10E )
			{
				// v.1.14+
				tFilter.m_eType = (ESphFilter) tReq.GetDword ();
				switch ( tFilter.m_eType )
				{
					case SPH_FILTER_RANGE:
						tFilter.m_uMinValue = ( iVer>=0x114 ) ? tReq.GetUint64() : tReq.GetDword ();
						tFilter.m_uMaxValue = ( iVer>=0x114 ) ? tReq.GetUint64() : tReq.GetDword ();
						break;

					case SPH_FILTER_FLOATRANGE:
						tFilter.m_fMinValue = tReq.GetFloat ();
						tFilter.m_fMaxValue = tReq.GetFloat ();
						break;

					case SPH_FILTER_VALUES:
						{
							bool bRes = ( iVer>=0x114 )
								? tReq.GetQwords ( tFilter.m_dValues, g_iMaxFilterValues, "invalid attribute set length %d (should be in 0..%d range)" )
								: tReq.GetDwords ( tFilter.m_dValues, g_iMaxFilterValues, "invalid attribute set length %d (should be in 0..%d range)" );
							if ( !bRes )
								return false;
						}
						break;

					default:
						tReq.SendErrorReply ( "unknown filter type (type-id=%d)", tFilter.m_eType );
						return false;
				}

			} else
			{
				// pre-1.14
				if ( !tReq.GetDwords ( tFilter.m_dValues, g_iMaxFilterValues, "invalid attribute set length %d (should be in 0..%d range)" ) )
					return false;

				if ( !tFilter.m_dValues.GetLength() )
				{
					// 0 length means this is range, not set
					tFilter.m_uMinValue = tReq.GetDword ();
					tFilter.m_uMaxValue = tReq.GetDword ();
				}

				tFilter.m_eType = tFilter.m_dValues.GetLength() ? SPH_FILTER_VALUES : SPH_FILTER_RANGE;
			}

			if ( iVer>=0x106 )
				tFilter.m_bExclude = !!tReq.GetDword ();
		}
	}

	// v.1.3
	if ( iVer>=0x103 )
	{
		tQuery.m_eGroupFunc = (ESphGroupBy) tReq.GetDword ();
		tQuery.m_sGroupBy = tReq.GetString ();
		tQuery.m_sGroupBy.ToLower ();
	}

	// v.1.4
	tQuery.m_iMaxMatches = g_iMaxMatches;
	if ( iVer>=0x104 )
		tQuery.m_iMaxMatches = tReq.GetInt ();

	// v.1.5, v.1.7
	if ( iVer>=0x107 )
	{
		tQuery.m_sGroupSortBy = tReq.GetString ();
	} else if ( iVer>=0x105 )
	{
		bool bSortByGroup = ( tReq.GetInt()!=0 );
		if ( !bSortByGroup )
		{
			char sBuf[256];
			switch ( tQuery.m_eSort )
			{
			case SPH_SORT_RELEVANCE:
				tQuery.m_sGroupSortBy = "@weight desc";
				break;

			case SPH_SORT_ATTR_DESC:
			case SPH_SORT_ATTR_ASC:
				snprintf ( sBuf, sizeof(sBuf), "%s %s", tQuery.m_sSortBy.cstr(),
					tQuery.m_eSort==SPH_SORT_ATTR_ASC ? "asc" : "desc" );
				tQuery.m_sGroupSortBy = sBuf;
				break;

			case SPH_SORT_EXTENDED:	
				tQuery.m_sGroupSortBy = tQuery.m_sSortBy;
				break;

			default:
				tReq.SendErrorReply ( "INTERNAL ERROR: unsupported sort mode %d in groupby sort fixup", tQuery.m_eSort );
				return false;
			}
		}
	}

	// v.1.9
	if ( iVer>=0x109 )
		tQuery.m_iCutoff = tReq.GetInt();

	// v.1.10
	if ( iVer>=0x10A )
	{
		tQuery.m_iRetryCount = tReq.GetInt ();
		tQuery.m_iRetryDelay = tReq.GetInt ();
	}

	// v.1.11
	if ( iVer>=0x10B )
		tQuery.m_sGroupDistinct = tReq.GetString ();

	// v.1.14
	if ( iVer>=0x10E )
	{
		tQuery.m_bGeoAnchor = ( tReq.GetInt()!=0 );
		if ( tQuery.m_bGeoAnchor )
		{
			tQuery.m_sGeoLatAttr = tReq.GetString ();
			tQuery.m_sGeoLongAttr = tReq.GetString ();
			tQuery.m_fGeoLatitude = tReq.GetFloat ();
			tQuery.m_fGeoLongitude = tReq.GetFloat ();
		}
	}

	// v.1.15
	if ( iVer>=0x10F )
	{
		tQuery.m_dIndexWeights.Resize ( tReq.GetInt() ); // FIXME! add sanity check
		ARRAY_FOREACH ( i, tQuery.m_dIndexWeights )
		{
			tQuery.m_dIndexWeights[i].m_sName = tReq.GetString ();
			tQuery.m_dIndexWeights[i].m_iValue = tReq.GetInt ();
		}
	}

	// v.1.17
	if ( iVer>=0x111 )
		tQuery.m_uMaxQueryMsec = tReq.GetDword ();

	// v.1.18
	if ( iVer>=0x112 )
	{
		tQuery.m_dFieldWeights.Resize ( tReq.GetInt() ); // FIXME! add sanity check
		ARRAY_FOREACH ( i, tQuery.m_dFieldWeights )
		{
			tQuery.m_dFieldWeights[i].m_sName = tReq.GetString ();
			tQuery.m_dFieldWeights[i].m_iValue = tReq.GetInt ();
		}
	}

	// v.1.19
	if ( iVer>=0x113 )
		tQuery.m_sComment = tReq.GetString ();

	// v.1.21
	if ( iVer>=0x115 )
	{
		tQuery.m_dOverrides.Resize ( tReq.GetInt() ); // FIXME! add sanity check
		ARRAY_FOREACH ( i, tQuery.m_dOverrides )
		{
			CSphAttrOverride & tOverride = tQuery.m_dOverrides[i];
			tOverride.m_sAttr = tReq.GetString ();
			tOverride.m_uAttrType = tReq.GetDword ();

			tOverride.m_dValues.Resize ( tReq.GetInt() ); // FIXME! add sanity check
			ARRAY_FOREACH ( iVal, tOverride.m_dValues )
			{
				CSphAttrOverride::IdValuePair_t & tEntry = tOverride.m_dValues[iVal];
				tEntry.m_uDocID = (SphDocID_t) tReq.GetUint64 ();

				if ( tOverride.m_uAttrType==SPH_ATTR_FLOAT )		tEntry.m_fValue = tReq.GetFloat ();
				else if ( tOverride.m_uAttrType==SPH_ATTR_BIGINT )	tEntry.m_uValue = tReq.GetUint64 ();
				else												tEntry.m_uValue = tReq.GetDword ();
			}
		}
	}

	// v.1.22
	if ( iVer>=0x116 )
	{
		tQuery.m_sSelect = tReq.GetString ();

		CSphString sError;
		if ( !tQuery.ParseSelectList ( sError ) )
		{
			tReq.SendErrorReply ( "select: %s", sError.cstr() );
			return false;
		}
	}

	/////////////////////
	// additional checks
	/////////////////////

	if ( tReq.GetError() )
	{
		tReq.SendErrorReply ( "invalid or truncated request" );
		return false;
	}
	if ( tQuery.m_iMinID>tQuery.m_iMaxID )
	{
		tReq.SendErrorReply ( "invalid ID range (min greater than max)" );
		return false;
	}
	if ( tQuery.m_eMode<0 || tQuery.m_eMode>SPH_MATCH_TOTAL )
	{
		tReq.SendErrorReply ( "invalid match mode %d", tQuery.m_eMode );
		return false;
	}
	if ( tQuery.m_eRanker<0 || tQuery.m_eRanker>SPH_RANK_TOTAL )
	{
		tReq.SendErrorReply ( "invalid ranking mode %d", tQuery.m_eRanker );
		return false;
	}
	if ( tQuery.m_iMaxMatches<1 || tQuery.m_iMaxMatches>g_iMaxMatches )
	{
		tReq.SendErrorReply ( "per-query max_matches=%d out of bounds (per-server max_matches=%d)",
			tQuery.m_iMaxMatches, g_iMaxMatches );
		return false;
	}
	if ( tQuery.m_iOffset<0 || tQuery.m_iOffset>=tQuery.m_iMaxMatches )
	{
		tReq.SendErrorReply ( "offset out of bounds (offset=%d, max_matches=%d)",
			tQuery.m_iOffset, tQuery.m_iMaxMatches );
		return false;
	}
	if ( tQuery.m_iLimit<0 )
	{
		tReq.SendErrorReply ( "limit out of bounds (limit=%d)", tQuery.m_iLimit );
		return false;
	}
	if ( tQuery.m_iCutoff<0 )
	{
		tReq.SendErrorReply ( "cutoff out of bounds (cutoff=%d)", tQuery.m_iCutoff );
		return false;
	}
	if ( tQuery.m_iRetryCount<0 || tQuery.m_iRetryCount>MAX_RETRY_COUNT )
	{
		tReq.SendErrorReply ( "retry count out of bounds (count=%d)", tQuery.m_iRetryCount );
		return false;
	}
	if ( tQuery.m_iRetryDelay<0 || tQuery.m_iRetryDelay>MAX_RETRY_DELAY )
	{
		tReq.SendErrorReply ( "retry delay out of bounds (delay=%d)", tQuery.m_iRetryDelay );
		return false;
	}

	// all ok
	return true;
}


void LogQuery ( const CSphQuery & tQuery, const CSphQueryResult & tRes )
{
	if ( g_iQueryLogFile<0 || !tRes.m_sError.IsEmpty() )
		return;

	char sTimeBuf[128], sGroupBuf[128], sPerfBuf[128], sTagBuf[128], sBuf[2048];

	sphFormatCurrentTime ( sTimeBuf );

	sGroupBuf[0] = '\0';
	if ( !tQuery.m_sGroupBy.IsEmpty() )
		snprintf ( sGroupBuf, sizeof(sGroupBuf), " @%s", tQuery.m_sGroupBy.cstr() );

	static const char * sModes [ SPH_MATCH_TOTAL ] = { "all", "any", "phr", "bool", "ext", "scan", "ext2" };
	static const char * sSort [ SPH_SORT_TOTAL ] = { "rel", "attr-", "attr+", "tsegs", "ext", "expr" };

	if ( g_bIOStats )
	{
		const CSphIOStats & IOStats = sphStopIOStats ();
		snprintf ( sPerfBuf, sizeof(sPerfBuf), " [ios=%d kb=%.1f ms=%.1f]", 
			IOStats.m_iReadOps, IOStats.m_fReadKBytes, IOStats.m_fReadTime*1000.0f );
	}
	else
		sPerfBuf[0] = '\0';

	if ( tQuery.m_sComment.IsEmpty() )
		sTagBuf[0] = '\0';
	else
		snprintf ( sTagBuf, sizeof(sTagBuf), " [%s]", tQuery.m_sComment.cstr() );


	int iQueryTime = Max ( tRes.m_iQueryTime, 0 );
	snprintf ( sBuf, sizeof(sBuf), "[%s] %d.%03d sec [%s/%d/%s %d (%d,%d)%s] [%s]%s%s %s\n",
		sTimeBuf, iQueryTime/1000, iQueryTime%1000,
		sModes [ tQuery.m_eMode ], tQuery.m_dFilters.GetLength(), sSort [ tQuery.m_eSort ],
		tRes.m_iTotalMatches, tQuery.m_iOffset, tQuery.m_iLimit, sGroupBuf,
		tQuery.m_sIndexes.cstr(), sPerfBuf, sTagBuf, tQuery.m_sQuery.cstr() );

	// snprintf does not emit zero at some runtimes (eg. VS2005)
	sBuf[sizeof(sBuf)-2] = '\n';
	sBuf[sizeof(sBuf)-1] = '\0';

	lseek ( g_iQueryLogFile, 0, SEEK_END );
	write ( g_iQueryLogFile, sBuf, strlen(sBuf) );
}


int CalcResultLength ( int iVer, const CSphQueryResult * pRes, const CSphVector<const DWORD *> & dTag2MVA )
{
	int iRespLen = 0;

	// query status
	if ( iVer>=0x10D )
	{
		// multi-query status
		iRespLen += 4; // status code

		if ( !pRes->m_sError.IsEmpty() )
			return iRespLen + 4 +strlen ( pRes->m_sError.cstr() );
			
		if ( !pRes->m_sWarning.IsEmpty() )
			iRespLen += 4+strlen ( pRes->m_sWarning.cstr() );

	} else if ( iVer>=0x106 )
	{
		// warning message
		if ( !pRes->m_sWarning.IsEmpty() )
			iRespLen += 4 + strlen ( pRes->m_sWarning.cstr() );
	}

	// query stats
	iRespLen += 20;

	// schema
	if ( iVer>=0x102 ) 
	{
		iRespLen += 8; // 4 for field count, 4 for attr count
		ARRAY_FOREACH ( i, pRes->m_tSchema.m_dFields )
			iRespLen += 4 + strlen ( pRes->m_tSchema.m_dFields[i].m_sName.cstr() ); // namelen, name
		for ( int i=0; i<pRes->m_tSchema.GetAttrsCount(); i++ )
			iRespLen += 8 + strlen ( pRes->m_tSchema.GetAttr(i).m_sName.cstr() ); // namelen, name, type
	}

	// matches
	if ( iVer<0x102 )
		iRespLen += 16*pRes->m_iCount; // matches
	else if ( iVer<0x108 )
		iRespLen += ( 8+4*pRes->m_tSchema.GetAttrsCount() )*pRes->m_iCount; // matches
	else
		iRespLen += 4 + ( 8+4*USE_64BIT+4*pRes->m_tSchema.GetAttrsCount() )*pRes->m_iCount; // id64 tag and matches

	if ( iVer>=0x114 )
	{
		// 64bit matches
		int iWideAttrs = 0;
		for ( int i=0; i<pRes->m_tSchema.GetAttrsCount(); i++ )
			if ( pRes->m_tSchema.GetAttr(i).m_eAttrType==SPH_ATTR_BIGINT )
				iWideAttrs++;
		iRespLen += 4*pRes->m_iCount*iWideAttrs; // extra 4 bytes per attr per match
	}

	for ( int i=0; i<pRes->m_iNumWords; i++ ) // per-word stats
		iRespLen += 12 + strlen ( pRes->m_tWordStats[i].m_sWord.cstr() ); // wordlen, word, docs, hits

	// MVA values
	CSphVector<CSphAttrLocator> dMvaItems;
	for ( int i=0; i<pRes->m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tCol = pRes->m_tSchema.GetAttr(i);
		if ( tCol.m_eAttrType & SPH_ATTR_MULTI )
			dMvaItems.Add ( tCol.m_tLocator );
	}

	if ( iVer>=0x10C && dMvaItems.GetLength() )
	{
		for ( int i=0; i<pRes->m_iCount; i++ )
		{
			const CSphMatch & tMatch = pRes->m_dMatches [ pRes->m_iOffset+i ];
			const DWORD * pMvaPool = dTag2MVA [ tMatch.m_iTag ];
			ARRAY_FOREACH ( j, dMvaItems )
			{
				const DWORD * pMva = tMatch.GetAttrMVA ( dMvaItems[j], pMvaPool );
				if ( pMva )
					iRespLen += pMva[0]*4; // FIXME? maybe add some sanity check here
			}
		}
	}

	return iRespLen;
}


void SendResult ( int iVer, NetOutputBuffer_c & tOut, const CSphQueryResult * pRes, const CSphVector<const DWORD *> & dTag2MVA )
{
	// status
	if ( iVer>=0x10D )
	{
		// multi-query status
		bool bError = !pRes->m_sError.IsEmpty();
		bool bWarning = !bError && !pRes->m_sWarning.IsEmpty();

		if ( bError )
		{
			tOut.SendInt ( SEARCHD_ERROR );
			tOut.SendString ( pRes->m_sError.cstr() );
			return;

		} else if ( bWarning )
		{
			tOut.SendInt ( SEARCHD_WARNING );
			tOut.SendString ( pRes->m_sWarning.cstr() );
		} else
		{
			tOut.SendInt ( SEARCHD_OK );
		}

	} else
	{
		// single-query warning
		if ( iVer>=0x106 && !pRes->m_sWarning.IsEmpty() )
			tOut.SendString ( pRes->m_sWarning.cstr() );
	}

	// send schema
	if ( iVer>=0x102 )
	{
		tOut.SendInt ( pRes->m_tSchema.m_dFields.GetLength() );
		ARRAY_FOREACH ( i, pRes->m_tSchema.m_dFields )
			tOut.SendString ( pRes->m_tSchema.m_dFields[i].m_sName.cstr() );

		tOut.SendInt ( pRes->m_tSchema.GetAttrsCount() );
		for ( int i=0; i<pRes->m_tSchema.GetAttrsCount(); i++ )
		{
			const CSphColumnInfo & tCol = pRes->m_tSchema.GetAttr(i);
			tOut.SendString ( tCol.m_sName.cstr() );
			tOut.SendDword ( (DWORD)tCol.m_eAttrType );
		}
	}

	// send matches
	CSphAttrLocator iGIDLoc, iTSLoc;
	if ( iVer<=0x101 )
	{
		for ( int i=0; i<pRes->m_tSchema.GetAttrsCount(); i++ )
		{
			const CSphColumnInfo & tAttr = pRes->m_tSchema.GetAttr(i);

			if ( iTSLoc.m_iBitOffset<0 && tAttr.m_eAttrType==SPH_ATTR_TIMESTAMP )
				iTSLoc = tAttr.m_tLocator;

			if ( iGIDLoc.m_iBitOffset<0 && tAttr.m_eAttrType==SPH_ATTR_INTEGER )
				iGIDLoc = tAttr.m_tLocator;
		}
	}

	tOut.SendInt ( pRes->m_iCount );
	if ( iVer>=0x108 )
		tOut.SendInt ( USE_64BIT );

	for ( int i=0; i<pRes->m_iCount; i++ )
	{
		const CSphMatch & tMatch = pRes->m_dMatches [ pRes->m_iOffset+i ];
#if USE_64BIT
		if ( iVer>=0x108 )
			tOut.SendUint64 ( tMatch.m_iDocID );
		else
#endif
			tOut.SendDword ( (DWORD)tMatch.m_iDocID );

		if ( iVer<=0x101 )
		{
			tOut.SendDword ( iGIDLoc.m_iBitOffset>=0 ? (DWORD) tMatch.GetAttr ( iGIDLoc ) : 1 );
			tOut.SendDword ( iTSLoc.m_iBitOffset>=0 ? (DWORD) tMatch.GetAttr ( iTSLoc ) : 1 );
			tOut.SendInt ( tMatch.m_iWeight );
		} else
		{
			tOut.SendInt ( tMatch.m_iWeight );

			const DWORD * pMvaPool = dTag2MVA [ tMatch.m_iTag ];

			assert ( tMatch.m_iRowitems==pRes->m_tSchema.GetRowSize() );
			for ( int j=0; j<pRes->m_tSchema.GetAttrsCount(); j++ )
			{
				const CSphColumnInfo & tAttr = pRes->m_tSchema.GetAttr(j);
				if ( tAttr.m_eAttrType & SPH_ATTR_MULTI )
				{
					const DWORD * pValues = tMatch.GetAttrMVA ( tAttr.m_tLocator, pMvaPool );
					if ( iVer<0x10C || !pValues )
					{
						// for older clients, fixups column value to 0
						// for newer clients, means that there are 0 values
						tOut.SendDword ( 0 );
					} else
					{
						// send MVA values
						int iValues = *pValues++;
						tOut.SendDword ( iValues );
						while ( iValues-- )
							tOut.SendDword ( *pValues++ );
					}
				} else
				{
					// send plain attr
					if ( tAttr.m_eAttrType==SPH_ATTR_FLOAT )
						tOut.SendFloat ( tMatch.GetAttrFloat ( tAttr.m_tLocator ) );
					else if ( iVer>=0x114 && tAttr.m_eAttrType==SPH_ATTR_BIGINT )
						tOut.SendUint64 ( tMatch.GetAttr ( tAttr.m_tLocator ) );
					else
						tOut.SendDword ( (DWORD)tMatch.GetAttr ( tAttr.m_tLocator ) );
				}
			}
		}
	}
	tOut.SendInt ( pRes->m_dMatches.GetLength() );
	tOut.SendInt ( pRes->m_iTotalMatches );
	tOut.SendInt ( Max ( pRes->m_iQueryTime, 0 ) );
	tOut.SendInt ( pRes->m_iNumWords );

	for ( int i=0; i<pRes->m_iNumWords; i++ )
	{
		tOut.SendString ( pRes->m_tWordStats[i].m_sWord.cstr() );
		tOut.SendInt ( pRes->m_tWordStats[i].m_iDocs );
		tOut.SendInt ( pRes->m_tWordStats[i].m_iHits );
	}
}

/////////////////////////////////////////////////////////////////////////////

struct AggrResult_t : CSphQueryResult
{
	int							m_iTag;			///< current tag
	CSphVector<CSphSchema>		m_dSchemas;		///< aggregated resultsets schemas (for schema minimization)
	CSphVector<int>				m_dMatchCounts;	///< aggregated resultsets lengths (for schema minimization)
	CSphVector<int>				m_dIndexWeights;///< aggregated resultsets per-index weights (optional)
	CSphVector<const DWORD *>	m_dTag2MVA;		///< tag to mva-storage-ptr mapping
};


bool MinimizeAggrResult ( AggrResult_t & tRes, const CSphQuery & tQuery )
{
	// sanity check
	int iExpected = 0;
	ARRAY_FOREACH ( i, tRes.m_dMatchCounts )
		iExpected += tRes.m_dMatchCounts[i];

	if ( iExpected!=tRes.m_dMatches.GetLength() )
	{
		tRes.m_sError.SetSprintf ( "INTERNAL ERROR: expected %d matches in combined result set, got %d",
			iExpected, tRes.m_dMatches.GetLength() );
		return false;
	}

	if ( !tRes.m_dMatches.GetLength() )
		return true;

	// build minimal schema
	bool bAllEqual = true;
	tRes.m_tSchema = tRes.m_dSchemas[0];
	for ( int i=1; i<tRes.m_dSchemas.GetLength(); i++ )
	{
		if ( !MinimizeSchema ( tRes.m_tSchema, tRes.m_dSchemas[i] ) )
			bAllEqual = false;
	}

	// apply select-items on top of that
	bool bStar = false;
	ARRAY_FOREACH ( i, tQuery.m_dItems )
		if ( tQuery.m_dItems[i].m_sExpr=="*" )
	{
		bStar = true;
		break;
	}

	if ( !bStar && tQuery.m_dItems.GetLength() )
	{
		CSphSchema tItems;
		for ( int i=0; i<tRes.m_tSchema.GetAttrsCount(); i++ )
		{
			const CSphColumnInfo & tCol = tRes.m_tSchema.GetAttr(i);
			if ( !tCol.m_pExpr )
			{
				bool bAdd = false;
				ARRAY_FOREACH ( i, tQuery.m_dItems )
					if ( tQuery.m_dItems[i].m_sExpr==tCol.m_sName )
				{
					bAdd = true;
					break;
				}

				if ( !bAdd )
					continue;
			}
			tItems.AddAttr ( tCol );
		}

		if ( tRes.m_tSchema.GetAttrsCount()!=tItems.GetAttrsCount() )
		{
			tRes.m_tSchema = tItems;
			bAllEqual = false;
		}
	}

	// convert all matches to minimal schema
	if ( !bAllEqual )
	{
		int iCur = 0;
		int * dMapFrom = NULL;

		CSphDocInfo tRow;
		tRow.Reset ( tRes.m_tSchema.GetRowSize() );

		if ( tRow.m_iRowitems )
			dMapFrom = new int [ tRes.m_tSchema.GetAttrsCount() ];

		ARRAY_FOREACH ( iSchema, tRes.m_dSchemas )
		{
			for ( int i=0; i<tRes.m_tSchema.GetAttrsCount(); i++ ) 
			{
				dMapFrom[i] = tRes.m_dSchemas[iSchema].GetAttrIndex ( tRes.m_tSchema.GetAttr(i).m_sName.cstr() );
				assert ( dMapFrom[i]>=0 );
			}

			for ( int i=iCur; i<iCur+tRes.m_dMatchCounts[iSchema]; i++ )
			{
				CSphMatch & tMatch = tRes.m_dMatches[i];

				if ( tRow.m_iRowitems )
				{
					// remap attrs
					for ( int j=0; j<tRes.m_tSchema.GetAttrsCount(); j++ ) 
					{
						const CSphColumnInfo & tDst = tRes.m_tSchema.GetAttr(j);
						const CSphColumnInfo & tSrc = tRes.m_dSchemas[iSchema].GetAttr ( dMapFrom[j] );
						tRow.SetAttr ( tDst.m_tLocator, tMatch.GetAttr(tSrc.m_tLocator) );
					}

					// remapped row might need *more* space because of unpacked attributes; allocate if so
					if ( tMatch.m_iRowitems<tRow.m_iRowitems )
					{
						SafeDeleteArray ( tMatch.m_pRowitems );
						tMatch.m_iRowitems = tRow.m_iRowitems;
						tMatch.m_pRowitems = new CSphRowitem [ tRow.m_iRowitems ];
					}

					// copy remapped row
					for ( int j=0; j<tRow.m_iRowitems; j++ )
						tMatch.m_pRowitems[j] = tRow.m_pRowitems[j];
				}
				tMatch.m_iRowitems = tRow.m_iRowitems;
			}

			iCur += tRes.m_dMatchCounts[iSchema];
		}

		assert ( iCur==tRes.m_dMatches.GetLength() );
		SafeDeleteArray ( dMapFrom );
	}

	// we do not need to re-sort if there's exactly one result set
	if ( tRes.m_iSuccesses==1 )
		return true;

	// create queue
	// at this point, we do not need to compute anything; it all must be here
	ISphMatchSorter * pSorter = sphCreateQueue ( &tQuery, tRes.m_tSchema, tRes.m_sError, false );
	if ( !pSorter )
		return false;

	// kill all dupes
	int iDupes = 0;
	if ( pSorter->IsGroupby () )
	{
		// groupby sorter does that automagically
		pSorter->SetMVAPool ( NULL ); // because we must be able to group on @groupby anyway
		ARRAY_FOREACH ( i, tRes.m_dMatches )
			if ( !pSorter->Push ( tRes.m_dMatches[i] ) )
				iDupes++;
	} else
	{
		// normal sorter needs massasging
		// sort by docid and then by tag to guarantee the replacement order
		tRes.m_dMatches.Sort ();

		// fold them matches
		if ( tQuery.m_dIndexWeights.GetLength() )
		{
			// if there were per-index weights, compute weighted ranks sum
			int iCur = 0;
			int iMax = tRes.m_dMatches.GetLength();

			while ( iCur<iMax )
			{
				CSphMatch & tMatch = tRes.m_dMatches[iCur++];
				if ( tMatch.m_iTag>=0 )
					tMatch.m_iWeight *= tRes.m_dIndexWeights[tMatch.m_iTag];

				while ( iCur<iMax && tRes.m_dMatches[iCur].m_iDocID==tMatch.m_iDocID )
				{
					const CSphMatch & tDupe = tRes.m_dMatches[iCur];
					int iAddWeight = tDupe.m_iWeight;
					if ( tDupe.m_iTag>=0 )
						iAddWeight *= tRes.m_dIndexWeights[tDupe.m_iTag];
					tMatch.m_iWeight += iAddWeight;

					iDupes++;
					iCur++;
				}

				pSorter->Push ( tMatch );
			}

		} else
		{
			// by default, simply remove dupes (select first by tag)
			ARRAY_FOREACH ( i, tRes.m_dMatches )
			{
				if ( i==0 || tRes.m_dMatches[i].m_iDocID!=tRes.m_dMatches[i-1].m_iDocID )
					pSorter->Push ( tRes.m_dMatches[i] );
				else
					iDupes++;
			}
		}
	}

	tRes.m_dMatches.Reset ();
	sphFlattenQueue ( pSorter, &tRes, -1 );
	SafeDelete ( pSorter );

	tRes.m_iTotalMatches -= iDupes;
	return true;
}


void SetupKillListFilter ( CSphFilterSettings & tFilter, const SphAttr_t * pKillList, int nEntries )
{
	assert ( nEntries && pKillList );

	tFilter.m_bExclude = true;
	tFilter.m_eType = SPH_FILTER_VALUES;
	tFilter.m_uMinValue = pKillList [0];
	tFilter.m_uMaxValue = pKillList [nEntries-1];
	tFilter.m_sAttrName = "@id";
	tFilter.SetExternalValues ( pKillList, nEntries );
}

/////////////////////////////////////////////////////////////////////////////

class SearchHandler_c
{
public:
									SearchHandler_c ( int iQueries, int iClientVer );
	void							RunQueries ();					///< run all queries, get all results

public:
	int								m_iClientVer;					///< client version
	CSphVector<CSphQuery>			m_dQueries;						///< queries which i need to search
	CSphVector<AggrResult_t>		m_dResults;						///< results which i obtained
	SearchFailuresLogset_c			m_dFailuresSet;					///< failure logs for each query

protected:
	void							RunSubset ( int iStart, int iEnd );	///< run queries against index(es) from first query in the subset
};


SearchHandler_c::SearchHandler_c ( int iQueries, int iClientVer )
{
	m_iClientVer = iClientVer;
	m_dQueries.Resize ( iQueries );
	m_dResults.Resize ( iQueries );
	m_dFailuresSet.SetSize ( iQueries );

	ARRAY_FOREACH ( i, m_dResults )
	{
		assert ( m_dResults[i].m_dIndexWeights.GetLength()==0 );
		m_dResults[i].m_iTag = 1; // first avail tag for local storage ptrs
		m_dResults[i].m_dIndexWeights.Add ( 1 ); // reserved index 0 with weight 1 for remote matches
		m_dResults[i].m_dTag2MVA.Add ( NULL ); // reserved index 0 for remote mva storage ptr; we'll fix this up later
	}
}


void SearchHandler_c::RunQueries ()
{
	///////////////////////////////
	// choose path and run queries
	///////////////////////////////

	if ( g_bIOStats )
		sphStartIOStats ();

	g_dMvaStorage.Reserve ( 1024 );
	g_dMvaStorage.Resize ( 0 );
	g_dMvaStorage.Add ( 0 );	// dummy value

	// check if all queries are to the same index
	bool bSameIndex = false;
	if ( m_dQueries.GetLength()>1 )
	{
		bSameIndex = true;
		ARRAY_FOREACH ( i, m_dQueries )
			if ( m_dQueries[i].m_sIndexes!=m_dQueries[0].m_sIndexes )
		{
			bSameIndex = false;
			break;
		}
	}

	if ( bSameIndex )
	{
		///////////////////////////////
		// batch queries to same index
		///////////////////////////////

		RunSubset ( 0, m_dQueries.GetLength()-1 );
		ARRAY_FOREACH ( i, m_dQueries )
			LogQuery ( m_dQueries[i], m_dResults[i] );

	} else
	{
		/////////////////////////////////////////////
		// fallback; just work each query separately
		/////////////////////////////////////////////

		ARRAY_FOREACH ( i, m_dQueries )
		{
			RunSubset ( i, i );
			LogQuery ( m_dQueries[i], m_dResults[i] );
		}
	}

	// final fixup
	ARRAY_FOREACH ( i, m_dResults )
		m_dResults[i].m_dTag2MVA[0] = g_dMvaStorage.GetLength() ? &g_dMvaStorage[0] : NULL;
}


void SearchHandler_c::RunSubset ( int iStart, int iEnd )
{
	// prepare for descent
	float tmStart = sphLongTimer ();

	CSphQuery & tFirst = m_dQueries[iStart];

	m_dFailuresSet.SetSubset ( iStart, iEnd );
	for ( int iRes=iStart; iRes<=iEnd; iRes++ )
		m_dResults[iRes].m_iSuccesses = 0;

	////////////////////////////////////////////////////////////////
	// check for single-query, multi-queue optimization possibility
	////////////////////////////////////////////////////////////////

	bool bMultiQueue = ( iStart<iEnd );
	for ( int iCheck=iStart+1; iCheck<=iEnd; iCheck++ )
	{
		const CSphQuery & qFirst = m_dQueries[iStart];
		const CSphQuery & qCheck = m_dQueries[iCheck];

		// these parameters must be the same
		if (
			( qCheck.m_sQuery!=qFirst.m_sQuery ) || // query string
			( qCheck.m_iWeights!=qFirst.m_iWeights ) || // weights count
			( qCheck.m_pWeights && memcmp ( qCheck.m_pWeights, qFirst.m_pWeights, sizeof(int)*qCheck.m_iWeights ) ) || // weights
			( qCheck.m_eMode!=qFirst.m_eMode ) || // search mode
			( qCheck.m_eRanker!=qFirst.m_eRanker ) || // ranking mode
			( qCheck.m_iMinID!=qFirst.m_iMinID ) || // min-id filter
			( qCheck.m_iMaxID!=qFirst.m_iMaxID ) || // max-id filter
			( qCheck.m_dFilters.GetLength()!=qFirst.m_dFilters.GetLength() ) || // attr filters count
			( qCheck.m_iCutoff!=qFirst.m_iCutoff ) || // cutoff
			( qCheck.m_eSort==SPH_SORT_EXPR && qFirst.m_eSort==SPH_SORT_EXPR && qCheck.m_sSortBy!=qFirst.m_sSortBy ) || // sort expressions
			( qCheck.m_bGeoAnchor!=qFirst.m_bGeoAnchor ) || // geodist expression
			( qCheck.m_bGeoAnchor && qFirst.m_bGeoAnchor && ( qCheck.m_fGeoLatitude!=qFirst.m_fGeoLatitude || qCheck.m_fGeoLongitude!=qFirst.m_fGeoLongitude ) ) )  // some geodist cases
		{
			bMultiQueue = false;
			break;
		}

		// filters must be the same too
		assert ( qCheck.m_dFilters.GetLength()==qFirst.m_dFilters.GetLength() );
		ARRAY_FOREACH ( i, qCheck.m_dFilters )
			if ( qCheck.m_dFilters[i]!=qFirst.m_dFilters[i] )
		{
			bMultiQueue = false;
			break;
		}
		if ( !bMultiQueue )
			break;
	}

	////////////////////////////
	// build local indexes list
	////////////////////////////

	CSphVector<CSphString> dLocal;
	DistributedIndex_t * pDist = g_hDistIndexes ( tFirst.m_sIndexes );

	if ( !pDist )
	{
		// they're all local, build the list
		if ( tFirst.m_sIndexes=="*" )
		{
			// search through all local indexes
			g_hIndexes.IterateStart ();
			while ( g_hIndexes.IterateNext () )
				if ( g_hIndexes.IterateGet ().m_bEnabled )
					dLocal.Add ( g_hIndexes.IterateGetKey() );
		} else
		{
			// search through specified local indexes
			ParseIndexList ( tFirst.m_sIndexes, dLocal );
			ARRAY_FOREACH ( i, dLocal )
			{
				// check that it exists
				if ( !g_hIndexes(dLocal[i]) )
				{
					for ( int iRes=iStart; iRes<=iEnd; iRes++ )
						m_dResults[iRes].m_sError.SetSprintf ( "unknown local index '%s' in search request", dLocal[i].cstr() );
					return;
				}

				// if it exists but is not enabled, remove it from the list and force recheck
				if ( !g_hIndexes[dLocal[i]].m_bEnabled )
					dLocal.RemoveFast ( i-- );
			}
		}

		// sanity check
		if ( !dLocal.GetLength() )
		{
			for ( int iRes=iStart; iRes<=iEnd; iRes++ )
				m_dResults[iRes].m_sError.SetSprintf ( "no enabled local indexes to search" );
			return;
		}

	} else
	{
		// copy local indexes list from distributed definition, but filter out disabled ones
		ARRAY_FOREACH ( i, pDist->m_dLocal )
			if ( g_hIndexes[pDist->m_dLocal[i]].m_bEnabled )
				dLocal.Add ( pDist->m_dLocal[i] );
	}

	/////////////////////////////////////////////////////
	// optimize single-query, same-schema local searches
	/////////////////////////////////////////////////////

	ISphMatchSorter * pLocalSorter = NULL;
	while ( iStart==iEnd && dLocal.GetLength()>1 )
	{
		CSphString sError;

		// check if all schemas are equal
		bool bAllEqual = true;
		const CSphSchema * pFirstSchema = g_hIndexes [ dLocal[0] ].m_pSchema;
		for ( int i=1; i<dLocal.GetLength() && bAllEqual; i++ )
		{
			if ( !pFirstSchema->CompareTo ( *g_hIndexes [ dLocal[i] ].m_pSchema, sError ) )
				bAllEqual = false;
		}

		// we can reuse the very same sorter
		if ( bAllEqual )
			if ( FixupQuery ( &m_dQueries[iStart], pFirstSchema, "local-sorter", sError ) )
				pLocalSorter = sphCreateQueue ( &m_dQueries[iStart], *pFirstSchema, sError );
		break;
	}

	///////////////////////////////////////////////////////////
	// main query loop (with multiple retries for distributed)
	///////////////////////////////////////////////////////////

	tFirst.m_iRetryCount = Min ( Max ( tFirst.m_iRetryCount, 0 ), MAX_RETRY_COUNT ); // paranoid clamp
	if ( !pDist )
		tFirst.m_iRetryCount = 0;

	for ( int iRetry=0; iRetry<=tFirst.m_iRetryCount; iRetry++ )
	{
		////////////////////////
		// issue remote queries
		////////////////////////

		// delay between retries
		if ( iRetry>0 )
			sphUsleep ( tFirst.m_iRetryDelay );

		// connect to remote agents and query them, if required
		int iRemote = 0;
		if ( pDist )
		{
			m_dFailuresSet.SetIndex ( tFirst.m_sIndexes.cstr() );
			ConnectToRemoteAgents ( pDist->m_dAgents, iRetry!=0  );

			SearchRequestBuilder_t tReqBuilder ( m_dQueries, iStart, iEnd );
			iRemote = QueryRemoteAgents ( pDist->m_dAgents, pDist->m_iAgentConnectTimeout, tReqBuilder );
		}

		/////////////////////
		// run local queries
		//////////////////////

		// while the remote queries are running, do local searches
		// FIXME! what if the remote agents finish early, could they timeout?
		float tmQuery = 0.0f;
		if ( iRetry==0 )
		{
			if ( pDist && !iRemote && !dLocal.GetLength() )
			{
				for ( int iRes=iStart; iRes<=iEnd; iRes++ )
					m_dResults[iRes].m_sError = "all remote agents unreachable and no available local indexes found";
				return;
			}

			tmQuery = -sphLongTimer ();

			ARRAY_FOREACH ( iLocal, dLocal )
			{
				const ServedIndex_t & tServed = g_hIndexes [ dLocal[iLocal] ];
				assert ( tServed.m_pIndex );
				assert ( tServed.m_bEnabled );

				if ( bMultiQueue )
				{
					////////////////////////////////
					// run single multi-queue query
					////////////////////////////////

					CSphVector<int> dSorterIndexes;
					dSorterIndexes.Resize ( iEnd+1 );
					ARRAY_FOREACH ( j, dSorterIndexes )
						dSorterIndexes[j] = -1;

					CSphVector<ISphMatchSorter*> dSorters;

					for ( int iQuery=iStart; iQuery<=iEnd; iQuery++ )
					{
						CSphString sError;
						CSphQuery & tQuery = m_dQueries[iQuery];
						ISphMatchSorter * pSorter = sphCreateQueue ( &tQuery, *tServed.m_pSchema, sError );
						if ( !pSorter )
						{
							m_dFailuresSet[iQuery].SubmitEx ( dLocal[iLocal].cstr(), "%s", sError.cstr() );
							continue;
						}

						dSorterIndexes[iQuery] = dSorters.GetLength();
						dSorters.Add ( pSorter );
					}

					if ( dSorters.GetLength() )
					{
						AggrResult_t tStats;

						// set killlist
						CSphQuery * pQuery = &m_dQueries[iStart];

						int iNumFilters = pQuery->m_dFilters.GetLength ();
						for ( int i = iLocal + 1; i < dLocal.GetLength (); i++ )
						{
							const ServedIndex_t & tServed = g_hIndexes [ dLocal[i] ];
							if ( tServed.m_pIndex->GetKillListSize () )
							{
								CSphFilterSettings tKillListFilter;
								SetupKillListFilter ( tKillListFilter, tServed.m_pIndex->GetKillList (), tServed.m_pIndex->GetKillListSize () );
								pQuery->m_dFilters.Add ( tKillListFilter );
							}
						}

						if ( !tServed.m_pIndex->MultiQuery ( &m_dQueries[iStart], &tStats,
							dSorters.GetLength(), &dSorters[0] ) )
						{
							// failed
							for ( int iQuery=iStart; iQuery<=iEnd; iQuery++ )
								m_dFailuresSet[iQuery].SubmitEx ( dLocal[iLocal].cstr(), "%s", tServed.m_pIndex->GetLastError().cstr() );
						} else
						{
							// multi-query succeeded
							for ( int iQuery=iStart; iQuery<=iEnd; iQuery++ )
							{
								// but some of the sorters could had failed at "create sorter" stage
								if ( dSorterIndexes[iQuery]<0 )
									continue;

								// this one seems OK
								ISphMatchSorter * pSorter = dSorters [ dSorterIndexes[iQuery] ];
								AggrResult_t & tRes = m_dResults[iQuery];
								tRes.m_iSuccesses++;

								tRes.m_iTotalMatches += pSorter->GetTotalCount();
								tRes.m_iQueryTime += ( iQuery==iStart ) ? tStats.m_iQueryTime : 0;
								tRes.m_pMva = tStats.m_pMva;

								tRes.m_iNumWords = tStats.m_iNumWords;
								for ( int i=0; i<tRes.m_iNumWords; i++ )
									tRes.m_tWordStats[i] = tStats.m_tWordStats[i];

								tRes.m_tSchema = pSorter->m_tOutgoingSchema;

								// extract matches from sorter
								assert ( pSorter );

								if ( pSorter->GetLength() )
								{
									tRes.m_dMatchCounts.Add ( pSorter->GetLength() );
									tRes.m_dSchemas.Add ( tRes.m_tSchema );
									tRes.m_dIndexWeights.Add ( m_dQueries[iQuery].GetIndexWeight ( dLocal[iLocal].cstr() ) );
									tRes.m_dTag2MVA.Add ( tRes.m_pMva );
									sphFlattenQueue ( pSorter, &tRes, tRes.m_iTag++ );
								}
							}
						}

						pQuery->m_dFilters.Resize ( iNumFilters );

						ARRAY_FOREACH ( i, dSorters )
							SafeDelete ( dSorters[i] );
					}

				} else
				{
					////////////////////////////////
					// run local queries one by one
					////////////////////////////////

					for ( int iQuery=iStart; iQuery<=iEnd; iQuery++ )
					{
						CSphQuery & tQuery = m_dQueries[iQuery];
						CSphString sError;

						int iNumFilters = tQuery.m_dFilters.GetLength ();
						for ( int i = iLocal + 1; i < dLocal.GetLength (); i++ )
						{
							const ServedIndex_t & tServed = g_hIndexes [ dLocal[i] ];
							if ( tServed.m_pIndex->GetKillListSize () )
							{
								CSphFilterSettings tKillListFilter;
								SetupKillListFilter ( tKillListFilter, tServed.m_pIndex->GetKillList (), tServed.m_pIndex->GetKillListSize () );
								tQuery.m_dFilters.Add ( tKillListFilter );
							}
						}

						// create sorter, if needed
						ISphMatchSorter * pSorter = pLocalSorter;
						if ( !pLocalSorter )
						{
							// fixup old queries
							if ( !FixupQuery ( &tQuery, tServed.m_pSchema, dLocal[iLocal].cstr(), sError ) )
							{
								m_dFailuresSet[iQuery].SubmitEx ( dLocal[iLocal].cstr(), "%s", sError.cstr() );
								continue;
							}

							// create queue
							pSorter = sphCreateQueue ( &tQuery, *tServed.m_pSchema, sError );
							if ( !pSorter )
							{
								m_dFailuresSet[iQuery].SubmitEx ( dLocal[iLocal].cstr(), "%s", sError.cstr() );
								continue;
							}
						}

						// do query
						AggrResult_t & tRes = m_dResults[iQuery];
						if ( !tServed.m_pIndex->QueryEx ( &tQuery, &tRes, pSorter ) )
							m_dFailuresSet[iQuery].SubmitEx ( dLocal[iLocal].cstr(), "%s", tServed.m_pIndex->GetLastError().cstr() );
						else
							tRes.m_iSuccesses++;

						// extract my results and store schema
						if ( pSorter->GetLength() )
						{
							tRes.m_dMatchCounts.Add ( pSorter->GetLength() );
							tRes.m_dSchemas.Add ( tRes.m_tSchema );
							tRes.m_dIndexWeights.Add ( tQuery.GetIndexWeight ( dLocal[iLocal].cstr() ) );
							tRes.m_dTag2MVA.Add ( tRes.m_pMva );
							sphFlattenQueue ( pSorter, &tRes, tRes.m_iTag++ );
						}

						// throw away the sorter
						if ( !pLocalSorter )
							SafeDelete ( pSorter );

						tQuery.m_dFilters.Resize ( iNumFilters );
					}
				}
			}
			tmQuery += sphLongTimer ();
		}

		///////////////////////
		// poll remote queries
		///////////////////////

		// wait for remote queries to complete
		if ( iRemote )
		{
			m_dFailuresSet.SetIndex ( tFirst.m_sIndexes.cstr() );

			SearchReplyParser_t tParser ( iStart, iEnd );
			int iMsecLeft = pDist->m_iAgentQueryTimeout - int(tmQuery*1000.0f);
			int iReplys = WaitForRemoteAgents ( pDist->m_dAgents, Max(iMsecLeft,0), tParser );

			// check if there were valid (though might be 0-matches) replys, and merge them
			if ( iReplys )
				ARRAY_FOREACH ( iAgent, pDist->m_dAgents )
			{
				Agent_t & tAgent = pDist->m_dAgents[iAgent];
				if ( !tAgent.m_bSuccess )
					continue;

				// merge this agent's results
				for ( int iRes=iStart; iRes<=iEnd; iRes++ )
				{
					const CSphQueryResult & tRemoteResult = tAgent.m_dResults[iRes-iStart];

					// copy errors or warnings
					m_dFailuresSet[iRes].SetPrefix ( "agent %s: ", tAgent.GetName().cstr() );
					if ( !tRemoteResult.m_sError.IsEmpty() )
						m_dFailuresSet[iRes].Submit ( "remote query error: %s", tRemoteResult.m_sError.cstr() );
					if ( !tRemoteResult.m_sWarning.IsEmpty() )
						m_dFailuresSet[iRes].Submit ( "remote query warning: %s", tRemoteResult.m_sWarning.cstr() );

					if ( tRemoteResult.m_iSuccesses<=0 )
						continue;

					AggrResult_t & tRes = m_dResults[iRes];
					tRes.m_iSuccesses++;

					ARRAY_FOREACH ( i, tRemoteResult.m_dMatches )
					{
						tRes.m_dMatches.Add ( tRemoteResult.m_dMatches[i] );
						tRes.m_dMatches.Last().m_iTag = 0; // all remote MVA values go to special pool which is at index 0
					}

					tRes.m_dMatchCounts.Add ( tRemoteResult.m_dMatches.GetLength() );
					tRes.m_dSchemas.Add ( tRemoteResult.m_tSchema );
					// note how we do NOT add per-index weight here; remote agents are all tagged 0 (which contains weight 1)

					// merge this agent's stats
					tRes.m_iTotalMatches += tRemoteResult.m_iTotalMatches;
					tRes.m_iQueryTime += tRemoteResult.m_iQueryTime;

					// merge this agent's words
					if ( !tRes.m_iNumWords )
					{
						// nothing has been set yet; just copy
						tRes.m_iNumWords = tRemoteResult.m_iNumWords;
						for ( int i=0; i<tRemoteResult.m_iNumWords; i++ )
							tRes.m_tWordStats[i] = tRemoteResult.m_tWordStats[i];

					} else if ( tRes.m_iNumWords!=tRemoteResult.m_iNumWords )
					{
						// word count mismatch
						m_dFailuresSet[iRes].Submit ( "query words mismatch (%d local, %d remote)",
							tRes.m_iNumWords, tRemoteResult.m_iNumWords );

					} else
					{
						// check for word contents mismatch
						assert ( tRes.m_iNumWords>0 && tRes.m_iNumWords==tRemoteResult.m_iNumWords );

						int iMismatch = -1;
						for ( int i=0; i<tRemoteResult.m_iNumWords && iMismatch<0; i++ )
							if ( tRes.m_tWordStats[i].m_sWord!=tRemoteResult.m_tWordStats[i].m_sWord )
								iMismatch = i;

						if ( iMismatch<0 )
						{
							// everything matches, update stats
							for ( int i=0; i<tRemoteResult.m_iNumWords; i++ )
							{
								tRes.m_tWordStats[i].m_iDocs += tRemoteResult.m_tWordStats[i].m_iDocs;
								tRes.m_tWordStats[i].m_iHits += tRemoteResult.m_tWordStats[i].m_iHits;
							}
						} else
						{
							// there are mismatches, warn
							m_dFailuresSet[iRes].Submit ( "query words mismatch (word %d, '%s' local vs '%s' remote)",
								iMismatch, tRes.m_tWordStats[iMismatch].m_sWord.cstr(), tRemoteResult.m_tWordStats[iMismatch].m_sWord.cstr() );
						}
					}
				}

				// dismissed
				tAgent.m_dResults.Reset ();
				tAgent.m_bSuccess = false;
				tAgent.m_sFailure = "";
			}
		}

		// check if we need to retry again
		int iToRetry = 0;
		if ( pDist )
			ARRAY_FOREACH ( i, pDist->m_dAgents )
				if ( pDist->m_dAgents[i].m_eState==AGENT_RETRY )
					iToRetry++;
		if ( !iToRetry )
			break;
	}

	// submit failures from failed agents
	if ( pDist )
	{
		m_dFailuresSet.SetIndex ( tFirst.m_sIndexes.cstr() );
		ARRAY_FOREACH ( i, pDist->m_dAgents )
		{
			const Agent_t & tAgent = pDist->m_dAgents[i];
			if ( !tAgent.m_bSuccess && !tAgent.m_sFailure.IsEmpty() )
				m_dFailuresSet.Submit  ( "agent %s: %s", tAgent.GetName().cstr(), tAgent.m_sFailure.cstr() );
		}
	}

	ARRAY_FOREACH ( i, m_dResults )
		assert ( m_dResults[i].m_iTag==m_dResults[i].m_dTag2MVA.GetLength() );

	// cleanup
	SafeDelete ( pLocalSorter );

	/////////////////////
	// merge all results
	/////////////////////

	// warn about id64 server vs old clients
#if USE_64BIT
	if ( m_iClientVer<0x108 )
	{
		m_dFailuresSet.SetIndex ( "*" );
		m_dFailuresSet.SetPrefix ( "" );
		m_dFailuresSet.Submit ( "searchd is id64; resulting docids might be wrapped" );
	}
#endif

	for ( int iRes=iStart; iRes<=iEnd; iRes++ )
	{
		AggrResult_t & tRes = m_dResults[iRes];
		CSphQuery & tQuery = m_dQueries[iRes];

		// if there were no succesful searches at all, this is an error
		if ( !tRes.m_iSuccesses )
		{
			StrBuf_t sFailures;
			m_dFailuresSet[iRes].BuildReport ( sFailures );

			tRes.m_sError = sFailures.cstr();
			continue;
		}

		// minimize schema and remove dupes
		if ( tRes.m_dSchemas.GetLength() )
			tRes.m_tSchema = tRes.m_dSchemas[0];

		if ( tRes.m_iSuccesses>1 || tQuery.m_dItems.GetLength() )
			if ( !MinimizeAggrResult ( tRes, tQuery ) )
				return;

		if ( !m_dFailuresSet[iRes].IsEmpty() )
		{
			StrBuf_t sFailures;
			m_dFailuresSet[iRes].BuildReport ( sFailures );
			tRes.m_sWarning = sFailures.cstr();
		}

		////////////
		// finalize
		////////////

		tRes.m_iOffset = tQuery.m_iOffset;
		tRes.m_iCount = Max ( Min ( tQuery.m_iLimit, tRes.m_dMatches.GetLength()-tQuery.m_iOffset ), 0 );
		tRes.m_iQueryTime = int ( 1000.0f*( sphLongTimer() - tmStart ) );/* !COMMIT thats batch run time, not query */
	}
}


bool CheckCommandVersion ( int iVer, int iDaemonVersion, InputBuffer_c & tReq )
{
	if ( (iVer>>8)!=(iDaemonVersion>>8) )
	{
		tReq.SendErrorReply ( "major command version mismatch (expected v.%d.x, got v.%d.%d)",
			iDaemonVersion>>8, iVer>>8, iVer&0xff );
		return false;
	}
	if ( iVer>iDaemonVersion )
	{
		tReq.SendErrorReply ( "client version is higher than daemon version (client is v.%d.%d, daemon is v.%d.%d)",
			iVer>>8, iVer&0xff, iDaemonVersion>>8, iDaemonVersion&0xff );
		return false;
	}
	return true;
}


void HandleCommandSearch ( int iSock, int iVer, InputBuffer_c & tReq )
{
	if ( !CheckCommandVersion ( iVer, VER_COMMAND_SEARCH, tReq ) )
		return;

	/////////////////
	// parse request
	/////////////////

	int iQueries = 1;
	if ( iVer>=0x10D )
		iQueries = tReq.GetDword ();

	const int MAX_QUERIES = 32;
	if ( iQueries<=0 || iQueries>MAX_QUERIES )
	{
		tReq.SendErrorReply ( "bad multi-query count %d (must be in 1..%d range)", iQueries, MAX_QUERIES );
		return;
	}

	// create handler
	SearchHandler_c tHandler ( iQueries, iVer );

	// parse all queries to handler
	ARRAY_FOREACH ( i, tHandler.m_dQueries )
		if ( !ParseSearchQuery ( tReq, tHandler.m_dQueries[i], iVer ) )
			return;		

	///////////////////
	// run all queries
	///////////////////

	tHandler.RunQueries ();

	//////////////////////
	// serve the response
	//////////////////////

	NetOutputBuffer_c tOut ( iSock );
	int iReplyLen = 0;

	if ( iVer<=0x10C )
	{
		assert ( tHandler.m_dQueries.GetLength()==1 );
		assert ( tHandler.m_dResults.GetLength()==1 );
		const AggrResult_t & tRes = tHandler.m_dResults[0];

		if ( !tRes.m_sError.IsEmpty() )
		{
			tReq.SendErrorReply ( "%s", tRes.m_sError.cstr() );
			return;
		}

		iReplyLen = CalcResultLength ( iVer, &tRes, tRes.m_dTag2MVA );
		bool bWarning = ( iVer>=0x106 && !tRes.m_sWarning.IsEmpty() );

		// send it
		tOut.SendWord ( (WORD)( bWarning ? SEARCHD_WARNING : SEARCHD_OK ) );
		tOut.SendWord ( VER_COMMAND_SEARCH );
		tOut.SendInt ( iReplyLen );

		SendResult ( iVer, tOut, &tRes, tRes.m_dTag2MVA );

	} else
	{
		ARRAY_FOREACH ( i, tHandler.m_dQueries )
			iReplyLen += CalcResultLength ( iVer, &tHandler.m_dResults[i], tHandler.m_dResults[i].m_dTag2MVA );

		// send it
		tOut.SendWord ( (WORD)SEARCHD_OK );
		tOut.SendWord ( VER_COMMAND_SEARCH );
		tOut.SendInt ( iReplyLen );

		ARRAY_FOREACH ( i, tHandler.m_dQueries )
			SendResult ( iVer, tOut, &tHandler.m_dResults[i], tHandler.m_dResults[i].m_dTag2MVA );
	}

	tOut.Flush ();
	assert ( tOut.GetError()==true || tOut.GetSentCount()==iReplyLen+8 );

	////////////
	// clean up
	////////////

	ARRAY_FOREACH ( i, tHandler.m_dQueries )
		SafeDeleteArray ( tHandler.m_dQueries[i].m_pWeights );
}

/////////////////////////////////////////////////////////////////////////////
// EXCERPTS HANDLER
/////////////////////////////////////////////////////////////////////////////

void HandleCommandExcerpt ( int iSock, int iVer, InputBuffer_c & tReq )
{
	if ( !CheckCommandVersion ( iVer, VER_COMMAND_EXCERPT, tReq ) )
		return;

	/////////////////////////////
	// parse and process request
	/////////////////////////////

	const int EXCERPT_MAX_ENTRIES			= 1024;
	const int EXCERPT_FLAG_REMOVESPACES		= 1;
	const int EXCERPT_FLAG_EXACTPHRASE		= 2;
	const int EXCERPT_FLAG_SINGLEPASSAGE	= 4;
	const int EXCERPT_FLAG_USEBOUNDARIES	= 8;
	const int EXCERPT_FLAG_WEIGHTORDER		= 16;

	// v.1.0
	ExcerptQuery_t q;

	tReq.GetInt (); // mode field is for now reserved and ignored
	int iFlags = tReq.GetInt ();
	CSphString sIndex = tReq.GetString ();

	const ServedIndex_t * pIndex = g_hIndexes(sIndex);
	if ( !pIndex )
	{
		tReq.SendErrorReply ( "unknown local index '%s' in search request", sIndex.cstr() );
		return;
	}
	CSphDict * pDict = pIndex->m_pIndex->GetDictionary ();
	ISphTokenizer * pTokenizer = pIndex->m_pIndex->GetTokenizer ();

	q.m_sWords = tReq.GetString ();
	q.m_sBeforeMatch = tReq.GetString ();
	q.m_sAfterMatch = tReq.GetString ();
	q.m_sChunkSeparator = tReq.GetString ();
	q.m_iLimit = tReq.GetInt ();
	q.m_iAround = tReq.GetInt ();

	q.m_bRemoveSpaces = ( iFlags & EXCERPT_FLAG_REMOVESPACES )!=0;
	q.m_bExactPhrase = ( iFlags & EXCERPT_FLAG_EXACTPHRASE )!=0;
	q.m_bSinglePassage = ( iFlags & EXCERPT_FLAG_SINGLEPASSAGE )!=0;
	q.m_bUseBoundaries = ( iFlags & EXCERPT_FLAG_USEBOUNDARIES )!=0;
	q.m_bWeightOrder = ( iFlags & EXCERPT_FLAG_WEIGHTORDER )!=0;

	int iCount = tReq.GetInt ();
	if ( iCount<0 || iCount>EXCERPT_MAX_ENTRIES )
	{
		tReq.SendErrorReply ( "invalid entries count %d", iCount );
		return;
	}

	CSphVector<char*> dExcerpts;
	for ( int i=0; i<iCount; i++ )
	{
		q.m_sSource = tReq.GetString ();
		if ( tReq.GetError() )
		{
			tReq.SendErrorReply ( "invalid or truncated request" );
			return;
		}

		const CSphIndexSettings & tSettings = pIndex->m_pIndex->GetSettings ();
		if ( tSettings.m_bHtmlStrip )
		{
			CSphString sError;
			CSphHTMLStripper tStripper;
			if (
				!tStripper.SetIndexedAttrs ( tSettings.m_sHtmlIndexAttrs.cstr (), sError ) ||
				!tStripper.SetRemovedElements ( tSettings.m_sHtmlRemoveElements.cstr (), sError ) )
			{
				tReq.SendErrorReply ( "HTML stripper config error: %s", sError.cstr() );
				return;
			}
			tStripper.Strip ( (BYTE*)q.m_sSource.cstr() );
		}

		dExcerpts.Add ( sphBuildExcerpt ( q, pDict, pTokenizer ) );
	}

	////////////////
	// serve result
	////////////////

	int iRespLen = 0;
	ARRAY_FOREACH ( i, dExcerpts )
		iRespLen += 4 + strlen ( dExcerpts[i] );

	NetOutputBuffer_c tOut ( iSock );
	tOut.SendWord ( SEARCHD_OK );
	tOut.SendWord ( VER_COMMAND_EXCERPT );
	tOut.SendInt ( iRespLen );
	ARRAY_FOREACH ( i, dExcerpts )
	{
		tOut.SendString ( dExcerpts[i] );
		SafeDeleteArray ( dExcerpts[i] );
	}

	tOut.Flush ();
	assert ( tOut.GetError()==true || tOut.GetSentCount()==iRespLen+8 );
}

/////////////////////////////////////////////////////////////////////////////
// KEYWORDS HANDLER
/////////////////////////////////////////////////////////////////////////////
void HandleCommandKeywords ( int iSock, int iVer, InputBuffer_c & tReq )
{
	if ( !CheckCommandVersion ( iVer, VER_COMMAND_KEYWORDS, tReq ) )
		return;

	CSphString sQuery = tReq.GetString ();
	CSphString sIndex = tReq.GetString ();
	bool bGetStats = !!tReq.GetInt ();

	const ServedIndex_t * pIndex = g_hIndexes(sIndex);
	if ( !pIndex )
	{
		tReq.SendErrorReply ( "unknown local index '%s' in search request", sIndex.cstr() );
		return;
	}

	CSphVector < CSphKeywordInfo > dKeywords;
	dKeywords.Reserve ( SPH_MAX_QUERY_WORDS );

	if ( !pIndex->m_pIndex->GetKeywords ( dKeywords, sQuery.cstr (), bGetStats ) )
	{
		tReq.SendErrorReply ( "error generating keywords: %s", pIndex->m_pIndex->GetLastError ().cstr () );
		return;
	}

	int iRespLen = 4;
	ARRAY_FOREACH ( i, dKeywords )
	{
		iRespLen += 4 + strlen ( dKeywords [i].m_sTokenized.cstr () );
		iRespLen += 4 + strlen ( dKeywords [i].m_sNormalized.cstr () );
		if ( bGetStats )
			iRespLen += 8;
	}

	NetOutputBuffer_c tOut ( iSock );
	tOut.SendWord ( SEARCHD_OK );
	tOut.SendWord ( VER_COMMAND_KEYWORDS );
	tOut.SendInt ( iRespLen );
	tOut.SendInt ( dKeywords.GetLength () );
	ARRAY_FOREACH ( i, dKeywords )
	{
		tOut.SendString ( dKeywords [i].m_sTokenized.cstr () );
		tOut.SendString ( dKeywords [i].m_sNormalized.cstr () );
		if ( bGetStats )
		{
			tOut.SendInt ( dKeywords [i].m_iDocs );
			tOut.SendInt ( dKeywords [i].m_iHits );
		}
	}

	tOut.Flush ();
	assert ( tOut.GetError()==true || tOut.GetSentCount()==iRespLen+8 );
}


/////////////////////////////////////////////////////////////////////////////
// UPDATES HANDLER
/////////////////////////////////////////////////////////////////////////////

struct UpdateRequestBuilder_t : public IRequestBuilder_t
{
	UpdateRequestBuilder_t ( const CSphAttrUpdate & pUpd ) : m_tUpd ( pUpd ) {}
	virtual void BuildRequest ( const char * sIndexes, NetOutputBuffer_c & tOut ) const;

protected:
	const CSphAttrUpdate & m_tUpd;
};


struct UpdateReplyParser_t : public IReplyParser_t
{
	UpdateReplyParser_t ( int * pUpd )
		: m_pUpdated ( pUpd )
	{}

	virtual bool ParseReply ( MemInputBuffer_c & tReq, Agent_t & ) const
	{
		*m_pUpdated += tReq.GetDword ();
		return true;
	}

protected:
	int * m_pUpdated;
};


void UpdateRequestBuilder_t::BuildRequest ( const char * sIndexes, NetOutputBuffer_c & tOut ) const
{
	int iReqSize = 4+strlen(sIndexes); // indexes string
	iReqSize += 4; // attrs array len, data
	ARRAY_FOREACH ( i, m_tUpd.m_dAttrs )
		iReqSize += 8+strlen(m_tUpd.m_dAttrs[i].m_sName.cstr());
	iReqSize += 4; // number of updates
	iReqSize += 8*m_tUpd.m_dDocids.GetLength() + 4*m_tUpd.m_dPool.GetLength(); // 64bit ids, 32bit values

	// header
	tOut.SendDword ( SPHINX_SEARCHD_PROTO );
	tOut.SendWord ( SEARCHD_COMMAND_UPDATE );
	tOut.SendWord ( VER_COMMAND_UPDATE );
	tOut.SendInt ( iReqSize );

	tOut.SendString ( sIndexes );
	tOut.SendInt ( m_tUpd.m_dAttrs.GetLength() );
	ARRAY_FOREACH ( i, m_tUpd.m_dAttrs )
	{
		tOut.SendString ( m_tUpd.m_dAttrs[i].m_sName.cstr() );
		tOut.SendInt ( ( m_tUpd.m_dAttrs[i].m_eAttrType & SPH_ATTR_MULTI ) ? 1 : 0 );
	}
	tOut.SendInt ( m_tUpd.m_dDocids.GetLength() );

	ARRAY_FOREACH ( i, m_tUpd.m_dDocids )
	{
		int iHead = m_tUpd.m_dRowOffset[i];
		int iTail = ( (i+1)<m_tUpd.m_dDocids.GetLength() ) ? m_tUpd.m_dRowOffset[i+1] : m_tUpd.m_dPool.GetLength ();

		tOut.SendUint64 ( m_tUpd.m_dDocids[i] );
		for ( int j=iHead; j<iTail; j++ )
			tOut.SendDword ( m_tUpd.m_dPool[j] );
	}
}


template < typename T, typename U >
struct CSphPair
{
	T m_tFirst;
	U m_tSecond;
};


void HandleCommandUpdate ( int iSock, int iVer, InputBuffer_c & tReq, int iPipeFD )
{
	if ( !CheckCommandVersion ( iVer, VER_COMMAND_UPDATE, tReq ) )
		return;

	// parse request
	CSphString sIndexes = tReq.GetString ();
	CSphAttrUpdate tUpd;

	tUpd.m_dAttrs.Resize ( tReq.GetDword() ); // FIXME! check this
	ARRAY_FOREACH ( i, tUpd.m_dAttrs )
	{
		tUpd.m_dAttrs[i].m_sName = tReq.GetString ();
		tUpd.m_dAttrs[i].m_sName.ToLower ();

		tUpd.m_dAttrs[i].m_eAttrType = SPH_ATTR_INTEGER;
		if ( iVer>=0x102 )
			if ( tReq.GetDword() )
				tUpd.m_dAttrs[i].m_eAttrType |= SPH_ATTR_MULTI;
	}

	int iNumUpdates = tReq.GetInt (); // FIXME! check this
	tUpd.m_dDocids.Reserve ( iNumUpdates );
	tUpd.m_dRowOffset.Reserve ( iNumUpdates );

	for ( int i=0; i<iNumUpdates; i++ )
	{
		// v.1.0 always sends 32-bit ids; v.1.1+ always send 64-bit ones
		uint64_t uDocid = ( iVer>=0x101 ) ? tReq.GetUint64 () : tReq.GetDword ();

		tUpd.m_dDocids.Add ( (SphDocID_t)uDocid ); // FIXME! check this
		tUpd.m_dRowOffset.Add ( tUpd.m_dPool.GetLength() );

		ARRAY_FOREACH ( iAttr, tUpd.m_dAttrs )
		{
			DWORD uCount = 1;
			if ( tUpd.m_dAttrs[iAttr].m_eAttrType & SPH_ATTR_MULTI )
			{
				uCount = tReq.GetDword ();
				tUpd.m_dPool.Add ( uCount );
			}

			for ( DWORD j=0; j<uCount; j++ )
				tUpd.m_dPool.Add ( tReq.GetDword() );
		}
	}

	if ( tReq.GetError() )
	{
		tReq.SendErrorReply ( "invalid or truncated request" );
		return;
	}

	// check index names
	CSphVector<CSphString> dIndexNames;
	ParseIndexList ( sIndexes, dIndexNames );

	if ( !dIndexNames.GetLength() )
	{
		tReq.SendErrorReply ( "no valid indexes in update request" );
		return;
	}

	ARRAY_FOREACH ( i, dIndexNames )
	{
		if ( !g_hIndexes(dIndexNames[i]) && !g_hDistIndexes(dIndexNames[i]) )
		{
			tReq.SendErrorReply ( "unknown index '%s' in update request", dIndexNames[i].cstr() );
			return;
		}
	}

	// do update
	SearchFailuresLogset_c dFailuresSet;
	dFailuresSet.SetSize ( 1 );
	dFailuresSet.SetSubset ( 0, 0 );

	int iSuccesses = 0;
	int iUpdated = 0;
	CSphVector < CSphPair < CSphString, DWORD > > dUpdated;

	ARRAY_FOREACH ( iIdx, dIndexNames )
	{
		const char * sReqIndex = dIndexNames[iIdx].cstr();

		CSphVector<CSphString> dLocal;
		const CSphVector<CSphString> * pLocal = NULL;

		if ( g_hIndexes(sReqIndex) )
		{
			dLocal.Add ( sReqIndex );
			pLocal = &dLocal;
		} else
		{
			assert ( g_hDistIndexes(sReqIndex) );
			pLocal = &g_hDistIndexes[sReqIndex].m_dLocal;
		}

		// update local indexes
		assert ( pLocal );
		ARRAY_FOREACH ( i, (*pLocal) )
		{
			const char * sIndex = (*pLocal)[i].cstr();
			ServedIndex_t * pServed = g_hIndexes(sIndex);

			dFailuresSet.SetIndex ( sIndex );
			dFailuresSet.SetPrefix ( "" );

			if ( !pServed || !pServed->m_pIndex )
			{
				dFailuresSet.Submit ( "index not available" );
				continue;
			}

			DWORD uStatusDelta = pServed->m_pIndex->m_uAttrsStatus;
			int iUpd = pServed->m_pIndex->UpdateAttributes ( tUpd );
			uStatusDelta = pServed->m_pIndex->m_uAttrsStatus & ~uStatusDelta;

			if ( iUpd<0 )
			{
				dFailuresSet.Submit ( "%s", pServed->m_pIndex->GetLastError().cstr() );

			} else
			{
				iUpdated += iUpd;
				iSuccesses++;

				CSphPair<CSphString,DWORD> tAdd;
				tAdd.m_tFirst = sIndex;
				tAdd.m_tSecond = uStatusDelta;
				dUpdated.Add ( tAdd );
			}
		}

		// update remote agents
		if ( g_hDistIndexes(sReqIndex) )
		{
			DistributedIndex_t & tDist = g_hDistIndexes[sReqIndex];
			dFailuresSet.SetIndex ( sReqIndex );

			// connect to remote agents and query them
			ConnectToRemoteAgents ( tDist.m_dAgents, false );

			UpdateRequestBuilder_t tReqBuilder ( tUpd );
			int iRemote = QueryRemoteAgents ( tDist.m_dAgents, tDist.m_iAgentConnectTimeout, tReqBuilder );

			if ( iRemote )
			{
				UpdateReplyParser_t tParser ( &iUpdated );
				iSuccesses += WaitForRemoteAgents ( tDist.m_dAgents, tDist.m_iAgentQueryTimeout, tParser );
			}
		}
	}

	// notify head daemon of local updates
	if ( iPipeFD>=0 )
	{
		DWORD uTmp = SPH_PIPE_UPDATED_ATTRS;
		::write ( iPipeFD, &uTmp, sizeof(DWORD) );

		uTmp = dUpdated.GetLength();
		::write ( iPipeFD, &uTmp, sizeof(DWORD) );

		ARRAY_FOREACH ( i, dUpdated )
		{
			uTmp = strlen ( dUpdated[i].m_tFirst.cstr() );
			::write ( iPipeFD, &uTmp, sizeof(DWORD) );
			::write ( iPipeFD, dUpdated[i].m_tFirst.cstr(), uTmp );
			uTmp = dUpdated[i].m_tSecond;
			::write ( iPipeFD, &uTmp, sizeof(DWORD) );
		}
	}

	// serve reply to client
	StrBuf_t sReport;
	dFailuresSet[0].BuildReport ( sReport );

	if ( !iSuccesses )
	{
		tReq.SendErrorReply ( "%s", sReport.cstr() );
		return;
	}

	NetOutputBuffer_c tOut ( iSock );
	if ( dFailuresSet[0].IsEmpty() )
	{
		tOut.SendWord ( SEARCHD_OK );
		tOut.SendWord ( VER_COMMAND_UPDATE );
		tOut.SendInt ( 4 );
	} else
	{
		tOut.SendWord ( SEARCHD_WARNING );
		tOut.SendWord ( VER_COMMAND_UPDATE );
		tOut.SendInt ( 8+strlen(sReport.cstr()) );
		tOut.SendString ( sReport.cstr() );
	}
	tOut.SendInt ( iUpdated );
	tOut.Flush ();
}

/////////////////////////////////////////////////////////////////////////////
// GENERAL HANDLER
/////////////////////////////////////////////////////////////////////////////

void SafeClose ( int & iFD )
{
	if ( iFD>=0 )
		::close ( iFD );
	iFD = -1;
}


void HandleClient ( int iSock, const char * sClientIP, int iPipeFD )
{
	bool bPersist = false;
	int iTimeout = g_iReadTimeout;
	NetInputBuffer_c tBuf ( iSock );

	// send my version
	DWORD uServer = SPHINX_SEARCHD_PROTO;
	if ( sphSockSend ( iSock, (char*)&uServer, sizeof(DWORD) )!=sizeof(DWORD) )
	{
		sphWarning ( "failed to send server version (client=%s)", sClientIP );
		return;
	}

	// get client version and request
	tBuf.ReadFrom ( 4 ); // FIXME! magic
	tBuf.GetInt (); // client version is for now unused
	do
	{
		g_pCrashLog_LastQuery = NULL;
		
		tBuf.ReadFrom ( 8, iTimeout );
		int iCommand = tBuf.GetWord ();
		int iCommandVer = tBuf.GetWord ();
		int iLength = tBuf.GetInt ();
		if ( tBuf.GetError() )
		{
			// under high load, there can be pretty frequent accept() vs connect() timeouts
			// lets avoid agent log flood
			//
			// sphWarning ( "failed to receive client version and request (client=%s, error=%s)", sClientIP, sphSockError() );
			return;
		}

		if ( g_bCrashLog_Enabled )
			memcpy ( g_dCrashLog_LastHello + 4, tBuf.GetBufferPtr(), sizeof(g_dCrashLog_LastHello) - 4 );
		
		// check request
		if ( iCommand<0 || iCommand>=SEARCHD_COMMAND_TOTAL
			 || iLength<=0 || iLength>g_iMaxPacketSize )
		{
			// unknown command, default response header
			tBuf.SendErrorReply ( "unknown command (code=%d)", iCommand );

			// if request length is insane, low level comm is broken, so we bail out
			if ( iLength<=0 || iLength>g_iMaxPacketSize )
			{
				sphWarning ( "ill-formed client request (length=%d out of bounds)", iLength );
				return;
			}
		}
		
		// get request body
		assert ( iLength>0 && iLength<=g_iMaxPacketSize );
		if ( !tBuf.ReadFrom ( iLength ) )
		{
			sphWarning ( "failed to receive client request body (client=%s)", sClientIP );
			return;
		}

		if ( g_bCrashLog_Enabled )
		{
			g_pCrashLog_LastQuery = tBuf.GetBufferPtr();
			g_iCrashLog_LastQuerySize = iLength;
		}

		// handle known commands
		assert ( iCommand>=0 && iCommand<SEARCHD_COMMAND_TOTAL );
		switch ( iCommand )
		{
			case SEARCHD_COMMAND_SEARCH:	HandleCommandSearch ( iSock, iCommandVer, tBuf ); break;
			case SEARCHD_COMMAND_EXCERPT:	HandleCommandExcerpt ( iSock, iCommandVer, tBuf ); break;
			case SEARCHD_COMMAND_KEYWORDS:	HandleCommandKeywords ( iSock, iCommandVer, tBuf ); break;
			case SEARCHD_COMMAND_UPDATE:	HandleCommandUpdate ( iSock, iCommandVer, tBuf, iPipeFD ); break;
			case SEARCHD_COMMAND_PERSIST:
				bPersist = tBuf.GetInt() != 0;
				iTimeout = g_iClientTimeout;
				break;
			default:						assert ( 0 && "INTERNAL ERROR: unhandled command" ); break;
		}
	} while ( bPersist );
	SafeClose ( iPipeFD ); 
}

/////////////////////////////////////////////////////////////////////////////
// INDEX ROTATION
/////////////////////////////////////////////////////////////////////////////

bool TryRename ( const char * sIndex, const char * sPrefix, const char * sFromPostfix, const char * sToPostfix, bool bFatal )
{
	char sFrom [ SPH_MAX_FILENAME_LEN ];
	char sTo [ SPH_MAX_FILENAME_LEN ];

	snprintf ( sFrom, sizeof(sFrom), "%s%s", sPrefix, sFromPostfix );
	snprintf ( sTo, sizeof(sTo), "%s%s", sPrefix, sToPostfix );

#if USE_WINDOWS
	::unlink ( sTo );
#endif

	if ( rename ( sFrom, sTo ) )
	{
		if ( bFatal )
		{
			sphFatal ( "rotating index '%s': rollback rename '%s' to '%s' failed: %s",
				sIndex, sFrom, sTo, strerror(errno) );
		} else
		{
			sphWarning ( "rotating index '%s': rename '%s' to '%s' failed: %s",
				sIndex, sFrom, sTo, strerror(errno) );
		}
		return false;
	}

	return true;
}


bool HasFiles ( const ServedIndex_t & tIndex, const char ** dExts )
{
	char sFile [ SPH_MAX_FILENAME_LEN ];
	const char * sPath = tIndex.m_sIndexPath.cstr();

	for ( int i=0; i<EXT_COUNT; i++ )
	{
		snprintf ( sFile, sizeof(sFile), "%s%s", sPath, dExts [i] );
		if ( !sphIsReadable ( sFile ) )
			return false;
	}

	return true;
}

/// returns true if any version of the index (old or new one) has been preread
bool RotateIndexGreedy ( ServedIndex_t & tIndex, const char * sIndex )
{
	char sFile [ SPH_MAX_FILENAME_LEN ];
	const char * sPath = tIndex.m_sIndexPath.cstr();

	for ( int i=0; i<EXT_COUNT; i++ )
	{
		snprintf ( sFile, sizeof(sFile), "%s%s", sPath, g_dNewExts[i] );
		if ( !sphIsReadable ( sFile ) )
		{
			if ( i>0 )
			{
				if ( tIndex.m_bOnlyNew )
					sphWarning ( "rotating index '%s': '%s' unreadable: %s; NOT SERVING", sIndex, sFile, strerror(errno) );
				else
					sphWarning ( "rotating index '%s': '%s' unreadable: %s; using old index", sIndex, sFile, strerror(errno) );
			}
			return false;
		}
	}

	if ( !tIndex.m_bOnlyNew )
	{
		// rename current to old
		for ( int i=0; i<EXT_COUNT; i++ )
		{
			if ( TryRename ( sIndex, sPath, g_dCurExts[i], g_dOldExts[i], false ) )
				continue;

			// rollback
			for ( int j=0; j<i; j++ )
				TryRename ( sIndex, sPath, g_dOldExts[j], g_dCurExts[j], true );

			sphWarning ( "rotating index '%s': rename to .old failed; using old index", sIndex );
			return false;
		}
	}

	// rename new to current
	for ( int i=0; i<EXT_COUNT; i++ )
	{
		if ( TryRename ( sIndex, sPath, g_dNewExts[i], g_dCurExts[i], false ) )
			continue;

		// rollback new ones we already renamed
		for ( int j=0; j<i; j++ )
			TryRename ( sIndex, sPath, g_dCurExts[j], g_dNewExts[j], true );

		// rollback old ones
		for ( int j=0; j<EXT_COUNT; j++ )
			TryRename ( sIndex, sPath, g_dOldExts[j], g_dCurExts[j], true );

		return false;
	}

	bool bPreread = false;

	// try to use new index
	CSphString sWarning;
	ISphTokenizer * pTokenizer = tIndex.m_pIndex->LeakTokenizer ();
	CSphDict * pDictionary = tIndex.m_pIndex->LeakDictionary ();

	const CSphSchema * pNewSchema = tIndex.m_pIndex->Prealloc ( tIndex.m_bMlock, sWarning );
	if ( !pNewSchema || !tIndex.m_pIndex->Preread() )
	{
		if ( tIndex.m_bOnlyNew )
		{
			sphWarning ( "rotating index '%s': .new preload failed: %s; NOT SERVING", sIndex, tIndex.m_pIndex->GetLastError().cstr() );
			return false;
		}
		else
		{
			sphWarning ( "rotating index '%s': .new preload failed: %s", sIndex, tIndex.m_pIndex->GetLastError().cstr() );

			// try to recover
			for ( int j=0; j<EXT_COUNT; j++ )
			{
				TryRename ( sIndex, sPath, g_dCurExts[j], g_dNewExts[j], true );
				TryRename ( sIndex, sPath, g_dOldExts[j], g_dCurExts[j], true );
			}

			pNewSchema = tIndex.m_pIndex->Prealloc ( tIndex.m_bMlock, sWarning );
			if ( !pNewSchema || !tIndex.m_pIndex->Preread() )
			{
				sphWarning ( "rotating index '%s': .new preload failed; ROLLBACK FAILED; INDEX UNUSABLE", sIndex );
				tIndex.m_bEnabled = false;
			}
			else
			{
				tIndex.m_bEnabled = true;
				bPreread = true;

				sphWarning ( "rotating index '%s': .new preload failed; using old index", sIndex );
				if ( !sWarning.IsEmpty() )
					sphWarning ( "rotating index '%s': %s", sIndex, sWarning.cstr() );
			}

			if ( !tIndex.m_pIndex->GetTokenizer () )
				tIndex.m_pIndex->SetTokenizer ( pTokenizer );
			else
				SafeDelete ( pTokenizer );

			if ( !tIndex.m_pIndex->GetDictionary () )
				tIndex.m_pIndex->SetDictionary ( pDictionary );
			else
				SafeDelete ( pDictionary );
		}
	
		return bPreread;
	}
	else
	{
		bPreread = true;

		if ( !sWarning.IsEmpty() )
			sphWarning ( "rotating index '%s': %s", sIndex, sWarning.cstr() );
	}

	if ( !tIndex.m_pIndex->GetTokenizer () )
		tIndex.m_pIndex->SetTokenizer ( pTokenizer );
	else
		SafeDelete ( pTokenizer );

	if ( !tIndex.m_pIndex->GetDictionary () )
		tIndex.m_pIndex->SetDictionary ( pDictionary );
	else
		SafeDelete ( pDictionary );

	// unlink .old
	if ( g_bUnlinkOld && !tIndex.m_bOnlyNew )
		for ( int i=0; i<EXT_COUNT; i++ )
		{
			snprintf ( sFile, sizeof(sFile), "%s%s", sPath, g_dOldExts[i] );
			if ( ::unlink ( sFile ) )
				sphWarning ( "rotating index '%s': unable to unlink '%s': %s", sIndex, sFile, strerror(errno) );
		}

	// uff. all done
	tIndex.m_pSchema = pNewSchema;
	tIndex.m_bEnabled = true;
	tIndex.m_bOnlyNew = false;
	sphInfo ( "rotating index '%s': success", sIndex );
	return bPreread;
}

/////////////////////////////////////////////////////////////////////////////
// MAIN LOOP
/////////////////////////////////////////////////////////////////////////////

#if USE_WINDOWS

int CreatePipe ( bool, int )	{ return -1; }
int PipeAndFork ( bool, int )	{ return -1; }

#else

// open new pipe to be able to receive notifications from children
// adds read-end fd to g_dPipes; returns write-end fd for child
int CreatePipe ( bool bFatal, int iHandler )
{
	assert ( g_bHeadDaemon );
	int dPipe[2] = { -1, -1 };

	for ( ;; )
	{
		if ( pipe(dPipe) )
		{
			if ( bFatal )
				sphFatal ( "pipe() failed (error=%s)", strerror(errno) );
			else
				sphWarning ( "pipe() failed (error=%s)", strerror(errno) );
			break;
		}

		if ( fcntl ( dPipe[0], F_SETFL, O_NONBLOCK ) )
		{
			sphWarning ( "fcntl(O_NONBLOCK) on pipe failed (error=%s)", strerror(errno) );
			SafeClose ( dPipe[0] );
			SafeClose ( dPipe[1] );
			break;
		}

		PipeInfo_t tAdd;
		tAdd.m_iFD = dPipe[0];
		tAdd.m_iHandler = iHandler;
		g_dPipes.Add ( tAdd );
		break;
	}

	return dPipe[1];
}


/// create new worker child
/// creates a pipe to it, forks, and does some post-fork work
//
/// in child, returns write-end pipe fd (might be -1!) and sets g_bHeadDaemon to false
/// in parent, returns -1 and leaves g_bHeadDaemon unaffected
int PipeAndFork ( bool bFatal, int iHandler )
{
	int iChildPipe = CreatePipe ( bFatal, iHandler );
	switch ( fork() )
	{
		// fork() failed
		case -1:
			sphFatal ( "fork() failed (reason: %s)", strerror(errno) );

		// child process, handle client
		case 0:
			g_bHeadDaemon = false;
			sphSetProcessInfo ( false );
			ARRAY_FOREACH ( i, g_dPipes )
				SafeClose ( g_dPipes[i].m_iFD );
			break;

		// parent process, continue accept()ing
		default:
			g_iChildren++;
			SafeClose ( iChildPipe );
			break;
	}
	return iChildPipe;
}

#endif // !USE_WINDOWS


/// check and report if there were any leaks since last call
void CheckLeaks ()
{
#if SPH_DEBUG_LEAKS
	static int iHeadAllocs = sphAllocsCount ();
	static int iHeadCheckpoint = sphAllocsLastID ();

	if ( iHeadAllocs!=sphAllocsCount() )
	{
		lseek ( g_iLogFile, 0, SEEK_END );
		sphAllocsDump ( g_iLogFile, iHeadCheckpoint );

		iHeadAllocs = sphAllocsCount ();
		iHeadCheckpoint = sphAllocsLastID ();
	}
#endif
}


bool CheckIndex ( const CSphIndex * pIndex, CSphString & sError )
{
	const CSphIndexSettings & tSettings = pIndex->GetSettings ();

	if ( ( tSettings.m_iMinPrefixLen>0 || tSettings.m_iMinInfixLen>0 ) && !pIndex->GetStar () )
	{
		CSphDict * pDict = pIndex->GetDictionary ();
		assert ( pDict );
		if ( pDict->GetSettings ().HasMorphology () )
		{
			sError = "infixes and morphology are enabled, enable_star=0";
			return false;
		}
	}

	return true;
}


void SeamlessTryToForkPrereader ()
{
	// next in line
	const char * sPrereading = g_dRotating.Pop ();
	if ( !sPrereading || !g_hIndexes(sPrereading) )
	{
		sphWarning ( "INTERNAL ERROR: preread attempt on unknown index '%s'", sPrereading ? sPrereading : "(NULL)" );
		return;
	}
	const ServedIndex_t & tServed = g_hIndexes[sPrereading];

	// alloc buffer index (once per run)
	if ( !g_pPrereading )
		g_pPrereading = sphCreateIndexPhrase ( NULL ); // FIXME! check if it's ok

	g_pPrereading->SetStar ( tServed.m_bStar );
	g_pPrereading->SetPreopen ( tServed.m_bPreopen || g_bPreopenIndexes ); 
	g_pPrereading->SetWordlistPreload ( !tServed.m_bOnDiskDict && !g_bOnDiskDicts );

	// rebase buffer index
	char sNewPath [ SPH_MAX_FILENAME_LEN ];
	snprintf ( sNewPath, sizeof(sNewPath), "%s.new", tServed.m_sIndexPath.cstr() );
	g_pPrereading->SetBase ( sNewPath );

	// prealloc enough RAM and lock new index
	CSphString sWarn, sError;
	if ( !g_pPrereading->Prealloc ( tServed.m_bMlock, sWarn ) )
	{
		sphWarning ( "rotating index '%s': prealloc: %s; using old index", sPrereading, g_pPrereading->GetLastError().cstr() );
		return;
	}
	if ( !g_pPrereading->Lock() )
	{
		sphWarning ( "rotating index '%s': lock: %s; using old index", sPrereading, g_pPrereading->GetLastError().cstr() );
		g_pPrereading->Dealloc ();
		return;
	}

	if ( tServed.m_bOnlyNew && g_pCfg && g_pCfg->m_tConf.Exists ( "index" ) && g_pCfg->m_tConf["index"].Exists ( sPrereading ) )
		if ( !sphFixupIndexSettings ( g_pPrereading, g_pCfg->m_tConf["index"][sPrereading], sError ) )
		{
			sphWarning ( "rotating index '%s': fixup: %s; using old index", sPrereading, sError.cstr() );
			return;
		}

	if ( !CheckIndex ( g_pPrereading, sError ) )
	{
		sphWarning ( "rotating index '%s': check: %s; using old index", sPrereading, sError.cstr() );
		return;
	}

	// fork async reader
	g_sPrereading = sPrereading;
	int iPipeFD = PipeAndFork ( true, SPH_PIPE_PREREAD );

	// in parent, wait for prereader process to finish
	if ( g_bHeadDaemon )
		return;

	// in child, do preread
	bool bRes = g_pPrereading->Preread ();
	if ( !bRes )
		sphWarning ( "rotating index '%s': preread failed: %s; using old index", g_sPrereading, g_pPrereading->GetLastError().cstr() );

	// report and exit
	DWORD uTmp = SPH_PIPE_PREREAD;
	::write ( iPipeFD, &uTmp, sizeof(DWORD) );

	uTmp = bRes;
	::write ( iPipeFD, &uTmp, sizeof(DWORD) );

	::close ( iPipeFD );
	exit ( 0 );
}


void SeamlessForkPrereader ()
{
	// sanity checks
	if ( !g_bDoRotate )
	{
		sphWarning ( "INTERNAL ERROR: preread attempt not in rotate state" );
		return;
	}
	if ( g_sPrereading )
	{
		sphWarning ( "INTERNAL ERROR: preread attempt before previous completion" );
		return;
	}

	// try candidates one by one
	while ( g_dRotating.GetLength() && !g_sPrereading )
		SeamlessTryToForkPrereader ();

	// if there's no more candidates, and nothing in the works, we're done
	if ( !g_sPrereading && !g_dRotating.GetLength() )
	{
		g_bDoRotate = false;
		sphInfo ( "rotating finished" );
	}
}


/// simple wrapper to simplify reading from pipes
struct PipeReader_t
{
	PipeReader_t ( int iFD )
		: m_iFD ( iFD )
		, m_bError ( false )
	{
#if !USE_WINDOWS
		if ( fcntl ( iFD, F_SETFL, 0 )<0 )
			sphWarning ( "fcntl(0) on pipe failed (error=%s)", strerror(errno) );
#endif
	}

	~PipeReader_t ()
	{
		SafeClose ( m_iFD );
	}

	int GetFD () const
	{
		return m_iFD;
	}

	bool IsError () const
	{
		return m_bError;
	}

	int GetInt ()
	{
		int iTmp;
		if ( !GetBytes ( &iTmp, sizeof(iTmp) ) )
			iTmp = 0;
		return iTmp;
	}

	CSphString GetString ()
	{
		int iLen = GetInt ();
		CSphString sRes;
		sRes.Reserve ( iLen );
		if ( !GetBytes ( const_cast<char*>(sRes.cstr()), iLen ) )
			sRes = "";
		return sRes;
	}

protected:
	bool GetBytes ( void * pBuf, int iCount )
	{
		if ( m_bError )
			return false;
		
		if ( m_iFD<0 )
		{
			m_bError = true;
			sphWarning ( "invalid pipe fd" );
			return false;
		}

		for ( ;; )
		{
			int iRes = ::read ( m_iFD, pBuf, iCount );
			if ( iRes<0 && errno==EINTR )
				continue;

			if ( iRes!=iCount )
			{
				m_bError = true;
				sphWarning ( "pipe read failed (exp=%d, res=%d, error=%s)",
					iCount, iRes, iRes>0 ? "(none)" : strerror(errno) );
				return false;
			}
			return true;
		}
	}

protected:
	int			m_iFD;
	bool		m_bError;

};


/// handle pipe notifications from attribute updating
void HandlePipeUpdate ( PipeReader_t & tPipe, bool bFailure )
{
	if ( bFailure )
		return; // silently ignore errors

	++g_iUpdateTag;

	int iUpdIndexes = tPipe.GetInt ();
	for ( int i=0; i<iUpdIndexes; i++ )
	{
		// index name and status must follow
		CSphString sIndex = tPipe.GetString ();
		DWORD uStatus = tPipe.GetInt ();
		if ( tPipe.IsError() )
			break;

		ServedIndex_t * pServed = g_hIndexes(sIndex);
		if ( pServed )
		{
			pServed->m_iUpdateTag = g_iUpdateTag;
			pServed->m_pIndex->m_uAttrsStatus |= uStatus;
		} else
			sphWarning ( "INTERNAL ERROR: unknown index '%s' in HandlePipeUpdate()", sIndex.cstr() );
	}
}


/// handle pipe notifications from prereading
void HandlePipePreread ( PipeReader_t & tPipe, bool bFailure )
{
	if ( bFailure )
	{
		// clean up previous one and launch next one
		g_sPrereading = NULL;

		// in any case, buffer index should now be deallocated
		g_pPrereading->Dealloc ();
		g_pPrereading->Unlock ();

		// work next one
		SeamlessForkPrereader ();
		return;
	}

	assert ( g_bDoRotate && g_bSeamlessRotate && g_sPrereading );

	// whatever the outcome, we will be done with this one
	const char * sPrereading = g_sPrereading;
	g_sPrereading = NULL;

	// notice that this will block!
	int iRes = tPipe.GetInt();
	if ( !tPipe.IsError() && iRes )
	{
		// if preread was succesful, exchange served index and prereader buffer index
		ServedIndex_t & tServed = g_hIndexes[sPrereading];
		CSphIndex * pOld = tServed.m_pIndex;
		CSphIndex * pNew = g_pPrereading;

		char sOld [ SPH_MAX_FILENAME_LEN ];
		snprintf ( sOld, sizeof(sOld), "%s.old", tServed.m_sIndexPath.cstr() );

		if ( !tServed.m_bOnlyNew && !pOld->Rename ( sOld ) )
		{
			// FIXME! rollback inside Rename() call potentially fail
			sphWarning ( "rotating index '%s': cur to old rename failed: %s", sPrereading, pOld->GetLastError().cstr() );

		} else
		{
			// FIXME! at this point there's no cur lock file; ie. potential race
			if ( !pNew->Rename ( tServed.m_sIndexPath.cstr() ) )
			{
				sphWarning ( "rotating index '%s': new to cur rename failed: %s", sPrereading, pNew->GetLastError().cstr() );
				if ( !tServed.m_bOnlyNew && !pOld->Rename ( tServed.m_sIndexPath.cstr() ) )
				{
					sphWarning ( "rotating index '%s': old to cur rename failed: %s; INDEX UNUSABLE", sPrereading, pOld->GetLastError().cstr() );
					tServed.m_bEnabled = false;
				}
			} else
			{
				// all went fine; swap them
				if ( !g_pPrereading->GetTokenizer () )
					g_pPrereading->SetTokenizer ( tServed.m_pIndex->LeakTokenizer () );

				if ( !g_pPrereading->GetDictionary () )
					g_pPrereading->SetDictionary ( tServed.m_pIndex->LeakDictionary () );

				Swap ( tServed.m_pIndex, g_pPrereading );
				tServed.m_pSchema = tServed.m_pIndex->GetSchema ();
				tServed.m_bEnabled = true;

				// unlink .old
				if ( g_bUnlinkOld && !tServed.m_bOnlyNew )
				{
					char sFile [ SPH_MAX_FILENAME_LEN ];

					for ( int i=0; i<EXT_COUNT; i++ )
					{
						snprintf ( sFile, sizeof(sFile), "%s%s", sOld, g_dCurExts[i] );
						if ( ::unlink ( sFile ) )
							sphWarning ( "rotating index '%s': unable to unlink '%s': %s", sPrereading, sFile, strerror(errno) );
					}
				}

				tServed.m_bOnlyNew = false;
				sphInfo ( "rotating index '%s': success", sPrereading );
			}
		}

	} else
	{
		if ( tPipe.IsError() )
			sphWarning ( "rotating index '%s': pipe read failed" );
		else
			sphWarning ( "rotating index '%s': preread failure reported" );
	}

	// in any case, buffer index should now be deallocated
	g_pPrereading->Dealloc ();
	g_pPrereading->Unlock ();

	// work next one
	SeamlessForkPrereader ();
}


/// handle pipe notifications from attribute saving
void HandlePipeSave ( PipeReader_t & tPipe, bool bFailure )
{
	// in any case, we're no more flushing
	g_bFlushing = false;

	// silently ignore errors
	if ( bFailure )
		return;

	// handle response
	int iSavedIndexes = tPipe.GetInt ();
	for ( int i=0; i<iSavedIndexes; i++ )
	{
		// index name must follow
		CSphString sIndex = tPipe.GetString ();
		if ( tPipe.IsError() )
			break;

		ServedIndex_t * pServed = g_hIndexes(sIndex);
		if ( pServed )
		{
			if ( pServed->m_iUpdateTag<=g_iFlushTag )
				pServed->m_pIndex->m_uAttrsStatus = 0;
		} else
		{
			sphWarning ( "INTERNAL ERROR: unknown index '%s' in HandlePipeSave()", sIndex.cstr() );
		}
	}
}


/// check if there are any notifications from the children and handle them
void CheckPipes ()
{
	ARRAY_FOREACH ( i, g_dPipes )
	{
		// try to get status code
		DWORD uStatus;
		int iRes = ::read ( g_dPipes[i].m_iFD, &uStatus, sizeof(DWORD) );

		// no data yet?
		if ( iRes==-1 && errno==EAGAIN )
			continue;

		// either if there's eof, or error, or valid data - this pipe is over
		PipeReader_t tPipe ( g_dPipes[i].m_iFD );
		int iHandler = g_dPipes[i].m_iHandler;
		g_dPipes.Remove ( i-- );

		// check for eof/error
		bool bFailure = false;
		if ( iRes!=sizeof(DWORD) )
		{
			bFailure = true;

			if ( iHandler<0 )
				continue; // no handler; we're not expecting anything

			if ( iRes!=0 || iHandler>=0 )
				sphWarning ( "pipe status read failed (handler=%d)", iHandler );
		}

		// check for handler/status mismatch
		if ( !bFailure && ( iHandler>=0 && (int)uStatus!=iHandler ) )
		{
			bFailure = true;
			sphWarning ( "INTERNAL ERROR: pipe status mismatch (handler=%d, status=%d)", iHandler, uStatus );
		}

		// check for handler promotion (ie: we did not expect anything particular, but something happened anyway)
		if ( !bFailure && iHandler<0 )
			iHandler = (int)uStatus;

		// run the proper handler
		switch ( iHandler )
		{
			case SPH_PIPE_UPDATED_ATTRS:	HandlePipeUpdate ( tPipe, bFailure ); break;
			case SPH_PIPE_SAVED_ATTRS:		HandlePipeSave ( tPipe, bFailure ); break;
			case SPH_PIPE_PREREAD:			HandlePipePreread ( tPipe, bFailure ); break;
			default:						if ( !bFailure ) sphWarning ( "INTERNAL ERROR: unknown pipe handler (handler=%d, status=%d)", iHandler, uStatus ); break;
		}
	}
}


void ConfigureIndex ( ServedIndex_t & tIdx, const CSphConfigSection & hIndex )
{
	tIdx.m_bMlock =			( hIndex.GetInt ( "mlock", 0 )!=0 ) && !g_bOptConsole;
	tIdx.m_bStar =			hIndex.GetInt ( "enable_star", 0 )	!= 0;
	tIdx.m_bPreopen =		hIndex.GetInt ( "preopen", 0 )		!= 0;
	tIdx.m_bOnDiskDict =	hIndex.GetInt ( "ondisk_dict", 0 )	!= 0;
}


bool PrereadNewIndex ( ServedIndex_t & tIdx, const CSphConfigSection & hIndex, const char * szIndexName )
{
	CSphString sWarning;

	tIdx.m_pSchema = tIdx.m_pIndex->Prealloc ( tIdx.m_bMlock, sWarning );
	if ( !tIdx.m_pSchema || !tIdx.m_pIndex->Preread() )
	{
		sphWarning ( "index '%s': preload: %s; NOT SERVING", szIndexName, tIdx.m_pIndex->GetLastError().cstr() );
		return false;
	}

	if ( !sWarning.IsEmpty() )
		sphWarning ( "index '%s': %s", szIndexName, sWarning.cstr() );

	CSphString sError;
	if ( !sphFixupIndexSettings ( tIdx.m_pIndex, hIndex, sError ) )
	{
		sphWarning ( "index '%s': %s - NOT SERVING", szIndexName, sError.cstr() );
		return false;
	}

	// try to lock it
	if ( !g_bOptConsole && !tIdx.m_pIndex->Lock() )
	{
		sphWarning ( "index '%s': lock: %s; NOT SERVING", szIndexName, tIdx.m_pIndex->GetLastError().cstr() );
		return false;
	}

	return true;
}


bool ConfigureAgent ( Agent_t & tAgent, const CSphVariant * pAgent, const char * szIndexName, bool bBlackhole )
{
	// extract host name or path
	const char * p = pAgent->cstr();
	while ( sphIsAlpha(*p) || *p=='.' || *p=='-' || *p=='/' ) p++;
	if ( p==pAgent->cstr() )
	{
		sphWarning ( "index '%s': agent '%s': host name or path expected - SKIPPING AGENT",
			szIndexName, pAgent->cstr() );
		return false;
	}
	if ( *p++!=':' )
	{
		sphWarning ( "index '%s': agent '%s': colon expected near '%s' - SKIPPING AGENT",
			szIndexName, pAgent->cstr(), p );
		return false;
	}

	CSphString sSub = pAgent->SubString ( 0, p-1-pAgent->cstr() );
	if ( sSub.cstr()[0] == '/' )
	{
#if USE_WINDOWS
		sphWarning ( "index '%s': agent '%s': UNIX sockets are not supported on Windows - SKIPPING AGENT",
			szIndexName, pAgent->cstr() );
		return false;
#else
		if ( strlen ( sSub.cstr() ) + 1 > sizeof(((struct sockaddr_un *)0)->sun_path) )
		{
			sphWarning ( "index '%s': agent '%s': UNIX socket path is too long - SKIPPING AGENT",
				szIndexName, pAgent->cstr() );
			return false;
		}

		tAgent.m_iFamily = AF_UNIX;
		tAgent.m_sPath = sSub;
		p--;
#endif

	}
	else
	{
		tAgent.m_iFamily = AF_INET;
		tAgent.m_sHost = sSub;

		// extract port
		if ( !isdigit(*p) )
		{
			sphWarning ( "index '%s': agent '%s': port number expected near '%s' - SKIPPING AGENT",
				szIndexName, pAgent->cstr(), p );
			return false;
		}
		tAgent.m_iPort = atoi(p);

		if ( !IsPortInRange(tAgent.m_iPort) )
		{
			sphWarning ( "index '%s': agent '%s': invalid port number near '%s' - SKIPPING AGENT",
				szIndexName, pAgent->cstr(), p );
			return false;
		}

		while ( isdigit(*p) ) p++;
	}

	// extract index list
	if ( *p++!=':' )
	{
		sphWarning ( "index '%s': agent '%s': colon expected near '%s' - SKIPPING AGENT",
			szIndexName, pAgent->cstr(), p );
		return false;
	}
	while ( isspace(*p) )
		p++;
	const char * sIndexList = p;
	while ( sphIsAlpha(*p) || isspace(*p) || *p==',' )
		p++;
	if ( *p )
	{
		sphWarning ( "index '%s': agent '%s': index list expected near '%s' - SKIPPING AGENT",
			szIndexName, pAgent->cstr(), p );
		return false;
	}
	tAgent.m_sIndexes = sIndexList;

	// lookup address (if needed)
	if ( tAgent.m_iFamily == AF_INET )
	{
		tAgent.m_uAddr = sphGetAddress ( tAgent.m_sHost.cstr() );
		if ( tAgent.m_uAddr == 0 )
		{
			sphWarning ( "index '%s': agent '%s': failed to lookup host name '%s' (error=%s) - SKIPPING AGENT",
				szIndexName, pAgent->cstr(), tAgent.m_sHost.cstr(), sphSockError() );
			return false;
		}
	}

	tAgent.m_bBlackhole = bBlackhole;

	return true;
}


ESphAddIndex AddIndex ( const char * szIndexName, const CSphConfigSection & hIndex )
{
	if ( hIndex("type") && hIndex["type"]=="distributed" )
	{
		///////////////////////////////
		// configure distributed index
		///////////////////////////////

		DistributedIndex_t tIdx;

		// add local agents
		for ( CSphVariant * pLocal = hIndex("local"); pLocal; pLocal = pLocal->m_pNext )
		{
			if ( !g_hIndexes ( pLocal->cstr() ) )
			{
				sphWarning ( "index '%s': no such local index '%s' - SKIPPING LOCAL INDEX",
					szIndexName, pLocal->cstr() );
				continue;
			}
			tIdx.m_dLocal.Add ( pLocal->cstr() );
		}

		// add remote agents
		for ( CSphVariant * pAgent = hIndex("agent"); pAgent; pAgent = pAgent->m_pNext )
		{
			Agent_t tAgent;
			if ( ConfigureAgent ( tAgent, pAgent, szIndexName, false ) )
				tIdx.m_dAgents.Add ( tAgent );
		}

		for ( CSphVariant * pAgent = hIndex("agent_blackhole"); pAgent; pAgent = pAgent->m_pNext )
		{
			Agent_t tAgent;
			if ( ConfigureAgent ( tAgent, pAgent, szIndexName, true ) )
				tIdx.m_dAgents.Add ( tAgent );
		}

		// configure options
		if ( hIndex("agent_connect_timeout") )
		{
			if ( hIndex["agent_connect_timeout"].intval()<=0 )
				sphWarning ( "index '%s': connect_timeout must be positive, ignored", szIndexName );
			else
				tIdx.m_iAgentConnectTimeout = hIndex["agent_connect_timeout"].intval();
		}

		if ( hIndex("agent_query_timeout") )
		{
			if ( hIndex["agent_query_timeout"].intval()<=0 )
				sphWarning ( "index '%s': query_timeout must be positive, ignored", szIndexName );
			else
				tIdx.m_iAgentQueryTimeout = hIndex["agent_query_timeout"].intval();
		}

		// finally, check and add distributed index to global table
		if ( tIdx.m_dAgents.GetLength()==0 && tIdx.m_dLocal.GetLength()==0 )
		{
			sphWarning ( "index '%s': no valid local/remote indexes in distributed index - NOT SERVING",
				szIndexName );
			return ADD_ERROR;
		}
		else
		{
			if ( !g_hDistIndexes.Add ( tIdx, szIndexName ) )
			{
				sphWarning ( "index '%s': duplicate name in hash?! INTERNAL ERROR - NOT SERVING", szIndexName );
				return ADD_ERROR;
			}
		}

		return ADD_DISTR;
	}
	else
	{
		/////////////////////////
		// configure local index
		/////////////////////////

		ServedIndex_t tIdx;

		// check path
		if ( !hIndex.Exists ( "path" ) )
		{
			sphWarning ( "index '%s': key 'path' not found' - NOT SERVING", szIndexName );
			return ADD_ERROR;
		}

		// check name
		if ( g_hIndexes.Exists ( szIndexName ) )
		{
			sphWarning ( "index '%s': duplicate name in hash?! INTERNAL ERROR - NOT SERVING", szIndexName );
			return ADD_ERROR;
		}

		// configure memlocking, star
		ConfigureIndex ( tIdx, hIndex );

		// try to create index
		CSphString sWarning;
		tIdx.m_pIndex = sphCreateIndexPhrase ( hIndex["path"].cstr() );
		tIdx.m_pIndex->SetStar ( tIdx.m_bStar );
		tIdx.m_pIndex->SetPreopen ( tIdx.m_bPreopen || g_bPreopenIndexes );
		tIdx.m_pIndex->SetWordlistPreload ( !tIdx.m_bOnDiskDict && !g_bOnDiskDicts );
		tIdx.m_bEnabled = false;

		// done
		tIdx.m_sIndexPath = hIndex["path"];
		if ( !g_hIndexes.Add ( tIdx, szIndexName ) )
		{
			sphWarning ( "INTERNAL ERROR: index '%s': hash add failed - NOT SERVING", szIndexName );
			return ADD_ERROR;
		}

		tIdx.Reset (); // so that the dtor wouln't delete everything

		return ADD_LOCAL;
	}
}


bool CheckConfigChanges ()
{
	struct stat tStat;
	memset ( &tStat, 0, sizeof ( tStat ) );
	if ( stat ( g_sConfigFile.cstr (), &tStat ) < 0 )
		memset ( &tStat, 0, sizeof ( tStat ) );

	DWORD uCRC32 = 0;
	sphCalcFileCRC32 ( g_sConfigFile.cstr (), uCRC32 );

	if ( g_uCfgCRC32 == uCRC32 && tStat.st_mtime == g_tCfgStat.st_mtime && tStat.st_ctime == g_tCfgStat.st_ctime && tStat.st_size == g_tCfgStat.st_size )
		return false;

	g_uCfgCRC32 = uCRC32;
	g_tCfgStat = tStat;

	return true;
}


void ReloadIndexSettings ( CSphConfigParser * pCP )
{
	assert ( pCP );

	g_bDoDelete = false;

	g_hIndexes.IterateStart ();
	while ( g_hIndexes.IterateNext () )
		g_hIndexes.IterateGet ().m_bToDelete = true;

	g_hDistIndexes.IterateStart ();
	while ( g_hDistIndexes.IterateNext () )
		g_hDistIndexes.IterateGet ().m_bToDelete = true;
	
	if ( !pCP->Parse ( g_sConfigFile.cstr () ) )
		sphWarning ( "failed to parse config file '%s'", g_sConfigFile.cstr () );

	SetSignalHandlers ();

	int nTotalIndexes = g_hIndexes.GetLength () + g_hDistIndexes.GetLength ();
	int nChecked = 0;

	const CSphConfig & hConf = pCP->m_tConf;
	hConf["index"].IterateStart ();
	while ( hConf["index"].IterateNext() )
	{
		const CSphConfigSection & hIndex = hConf["index"].IterateGet();
		const char * sIndexName = hConf["index"].IterateGetKey().cstr();

		if ( g_hIndexes.Exists ( sIndexName ) )
		{
			ServedIndex_t & tIndex = g_hIndexes[sIndexName];
			ConfigureIndex ( tIndex, hIndex );
			tIndex.m_bToDelete = false;
			nChecked++;
		}
		else if ( g_hDistIndexes.Exists ( sIndexName ) )
		{
			DistributedIndex_t & tIndex = g_hDistIndexes[sIndexName];
			tIndex.m_bToDelete = false;
			nChecked++;
		}
		else if ( AddIndex ( sIndexName, hIndex ) == ADD_LOCAL )
		{
			ServedIndex_t & tIndex = g_hIndexes[sIndexName];
			tIndex.m_bOnlyNew = true;
		}
	}

	if ( nChecked < nTotalIndexes )
		g_bDoDelete = true;
}


struct IndexToDelete_t
{
	const CSphString *	m_pName;
	ServedIndex_t *		m_pIndex;
};


void CheckDelete ()
{
	if ( !g_bDoDelete )
		return;

	if ( g_iChildren )
		return;

	CSphVector<IndexToDelete_t> dToDelete;
	CSphVector<const CSphString *> dDistToDelete;
	dToDelete.Reserve ( 8 );
	dDistToDelete.Reserve ( 8 );

	g_hIndexes.IterateStart ();
	while ( g_hIndexes.IterateNext () )
	{
		ServedIndex_t & tIndex = g_hIndexes.IterateGet ();
		if ( tIndex.m_bToDelete )
		{
			IndexToDelete_t tToDelete;
			tToDelete.m_pName = &g_hIndexes.IterateGetKey ();
			tToDelete.m_pIndex = &tIndex;
			dToDelete.Add ( tToDelete );
		}
	}

	g_hDistIndexes.IterateStart ();
	while ( g_hDistIndexes.IterateNext () )
	{
		DistributedIndex_t & tIndex = g_hDistIndexes.IterateGet ();
		if ( tIndex.m_bToDelete )
			dDistToDelete.Add ( &g_hDistIndexes.IterateGetKey () );
	}

	ARRAY_FOREACH ( i, dToDelete )
	{
		dToDelete [i].m_pIndex->m_pIndex->Unlock();
		g_hIndexes.Delete ( *(dToDelete [i].m_pName) );
	}

	ARRAY_FOREACH ( i, dDistToDelete )
		g_hDistIndexes.Delete ( *dDistToDelete [i] );

	g_bDoDelete = false;
}


void CheckRotate ()
{
	// do we need to rotate now?
	if ( !g_bDoRotate )
		return;

	/////////////////////
	// RAM-greedy rotate
	/////////////////////

	if ( !g_bSeamlessRotate )
	{
		// wait until there's no running queries
		if ( g_iChildren )
			return;

		CSphConfigParser * pCP = NULL;

		if ( CheckConfigChanges () )
		{
			pCP = new CSphConfigParser;
			ReloadIndexSettings ( pCP );
		}

		g_hIndexes.IterateStart();
		while ( g_hIndexes.IterateNext() )
		{
			ServedIndex_t & tIndex = g_hIndexes.IterateGet();
			const char * sIndex = g_hIndexes.IterateGetKey().cstr();
			assert ( tIndex.m_pIndex );

			bool bWasAdded = tIndex.m_bOnlyNew;
			RotateIndexGreedy ( tIndex, sIndex );
			if ( bWasAdded && tIndex.m_bEnabled )
			{
				const CSphConfigType & hConf = pCP->m_tConf ["index"];
				if ( hConf.Exists ( sIndex ) )
				{
					CSphString sError;
					if ( !sphFixupIndexSettings ( tIndex.m_pIndex, hConf [sIndex], sError ) )
					{
						sphWarning ( "index '%s': %s - NOT SERVING", sIndex, sError.cstr() );
						tIndex.m_bEnabled = false;
					}

					if ( tIndex.m_bEnabled && !CheckIndex ( tIndex.m_pIndex, sError ) )
					{
						sphWarning ( "index '%s': %s - NOT SERVING", sIndex, sError.cstr() );
						tIndex.m_bEnabled = false;
					}
				}		
			}
		}

		SafeDelete ( pCP );

		g_bDoRotate = false;
		sphInfo ( "rotating finished" );
		return;
	}

	///////////////////
	// seamless rotate
	///////////////////

	assert ( g_bDoRotate && g_bSeamlessRotate );
	if ( g_dRotating.GetLength() || g_sPrereading )
		return; // rotate in progress already; will be handled in CheckPipes()

	SafeDelete ( g_pCfg );
	if ( CheckConfigChanges () )
	{
		g_pCfg = new CSphConfigParser;
		ReloadIndexSettings ( g_pCfg );
	}

	// check what indexes need to be rotated
	g_hIndexes.IterateStart();
	while ( g_hIndexes.IterateNext() )
	{
		const ServedIndex_t & tIndex = g_hIndexes.IterateGet();
		const char * sIndex = g_hIndexes.IterateGetKey().cstr();
		assert ( tIndex.m_pIndex );

		CSphString sNewPath;
		sNewPath.SetSprintf ( "%s.new", tIndex.m_sIndexPath.cstr() );

		// check if there's a .new index incoming
		// FIXME? move this code to index, and also check for exists-but-not-readable
		CSphString sTmp;
		sTmp.SetSprintf ( "%s.sph", sNewPath.cstr() );
		if ( !sphIsReadable ( sTmp.cstr() ) )
			continue;

		g_dRotating.Add ( sIndex );
	}

	SeamlessForkPrereader ();
}


void CheckReopen ()
{
	if ( !g_bGotSigusr1 )
		return;

	// reopen searchd log
	if ( g_iLogFile>=0 && !g_bLogTty )
	{
		int iFD = ::open ( g_sLogFile.cstr(), O_CREAT | O_RDWR | O_APPEND, S_IREAD | S_IWRITE );
		if ( iFD<0 )
		{
			sphWarning ( "failed to reopen log file '%s': %s", g_sLogFile.cstr(), strerror(errno) );
		} else
		{
			g_iLogFile = iFD;
			g_bLogTty = ( isatty(g_iLogFile)!=0 );
			sphInfo ( "log reopened" );
		}
	}

	// reopen query log
	if ( g_iQueryLogFile!=g_iLogFile && g_iQueryLogFile>=0 && !isatty(g_iQueryLogFile) )
	{
		int iFD = ::open ( g_sQueryLogFile.cstr(), O_CREAT | O_RDWR | O_APPEND, S_IREAD | S_IWRITE );
		if ( iFD<0 )
		{
			sphWarning ( "failed to reopen query log file '%s': %s", g_sQueryLogFile.cstr(), strerror(errno) );
		} else
		{
			g_iQueryLogFile = iFD;
			sphInfo ( "query log reopened" );
		}
	}

	g_bGotSigusr1 = false;
}


void CheckFlush ()
{
	if ( g_iAttrFlushPeriod<=0 || g_bFlushing )
		return;

	static float fLastCheck = -0.001f;
	float fNow = sphLongTimer ();

	if ( fLastCheck+float(g_iAttrFlushPeriod)>=fNow )
		return;

	fLastCheck = fNow;

	// check if there are dirty indexes
	bool bDirty = false;
	g_hIndexes.IterateStart ();
	while ( g_hIndexes.IterateNext () )
	{
		const ServedIndex_t & tServed = g_hIndexes.IterateGet ();
		if ( tServed.m_bEnabled && tServed.m_iUpdateTag>g_iFlushTag )
		{
			bDirty = true;
			break;
		}
	}
	if ( !bDirty )
		return;

	// launch the flush!
	g_bFlushing = true;
	int iPipeFD = PipeAndFork ( false, SPH_PIPE_SAVED_ATTRS ); // FIXME! gracefully handle fork() failures, Windows, etc
	if ( g_bHeadDaemon )
	{
		g_iFlushTag = g_iUpdateTag;
		return;
	}

	// child process, do the work
	CSphVector<CSphString> dSaved;

	g_hIndexes.IterateStart ();
	while ( g_hIndexes.IterateNext () )
	{
		const ServedIndex_t & tServed = g_hIndexes.IterateGet ();
		if ( tServed.m_bEnabled && tServed.m_iUpdateTag>g_iFlushTag )
			if ( tServed.m_pIndex->SaveAttributes () ) // FIXME? report errors somehow?
				dSaved.Add ( g_hIndexes.IterateGetKey() );
	}

	// report and exit
	DWORD uTmp = SPH_PIPE_SAVED_ATTRS;
	::write ( iPipeFD, &uTmp, sizeof(DWORD) );

	uTmp = dSaved.GetLength();
	::write ( iPipeFD, &uTmp, sizeof(DWORD) );

	ARRAY_FOREACH ( i, dSaved )
	{
		uTmp = strlen ( dSaved[i].cstr() );
		::write ( iPipeFD, &uTmp, sizeof(DWORD) );
		::write ( iPipeFD, dSaved[i].cstr(), uTmp );
	}

	::close ( iPipeFD );
	exit ( 0 );
}


#if !USE_WINDOWS
#define WINAPI
#else

SERVICE_STATUS			g_ss;
SERVICE_STATUS_HANDLE	g_ssHandle;


void MySetServiceStatus ( DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint )
{
	static DWORD dwCheckPoint = 1;

	if ( dwCurrentState == SERVICE_START_PENDING )
		g_ss.dwControlsAccepted = 0;
	else
		g_ss.dwControlsAccepted = SERVICE_ACCEPT_STOP;

	g_ss.dwCurrentState = dwCurrentState;
	g_ss.dwWin32ExitCode = dwWin32ExitCode;
	g_ss.dwWaitHint = dwWaitHint;

	if ( dwCurrentState==SERVICE_RUNNING || dwCurrentState==SERVICE_STOPPED )
		g_ss.dwCheckPoint = 0;
	else
		g_ss.dwCheckPoint = dwCheckPoint++;

	SetServiceStatus ( g_ssHandle, &g_ss );
}


void WINAPI ServiceControl ( DWORD dwControlCode )
{
	switch ( dwControlCode )
	{
		case SERVICE_CONTROL_STOP:
			MySetServiceStatus ( SERVICE_STOP_PENDING, NO_ERROR, 0 );
			g_bServiceStop = true;
			break;

		default:
			MySetServiceStatus ( g_ss.dwCurrentState, NO_ERROR, 0 );
			break;
	}
}


// warning! static buffer, non-reentrable
const char * WinErrorInfo ()
{
	static char sBuf[1024];

	DWORD uErr = ::GetLastError ();
	sprintf ( sBuf, "code=%d, error=", uErr );

	int iLen = strlen(sBuf);
	if ( !FormatMessage ( FORMAT_MESSAGE_FROM_SYSTEM, NULL, uErr, 0, sBuf+iLen, sizeof(sBuf)-iLen, NULL ) ) // FIXME? force US-english langid?
		strcpy ( sBuf+iLen, "(no message)" );

	return sBuf;
}


SC_HANDLE ServiceOpenManager ()
{
	SC_HANDLE hSCM = OpenSCManager (
		NULL,					// local computer
		NULL,					// ServicesActive database 
		SC_MANAGER_ALL_ACCESS );// full access rights

	if ( hSCM==NULL )
		sphFatal ( "OpenSCManager() failed: %s", WinErrorInfo() );

	return hSCM;
}


void strappend ( char * sBuf, const int iBufLimit, char * sAppend )
{
	int iLen = strlen(sBuf);
	int iAppend = strlen(sAppend);

	int iToCopy = Min ( iBufLimit-iLen-1, iAppend );
	memcpy ( sBuf+iLen, sAppend, iToCopy );
	sBuf[iLen+iToCopy] = '\0';
}


void ServiceInstall ( int argc, char ** argv )
{
	if ( g_bService )
		return;

	sphInfo ( "Installing service..." );

	char szPath[MAX_PATH];
	if( !GetModuleFileName ( NULL, szPath, MAX_PATH ) )
		sphFatal ( "GetModuleFileName() failed: %s", WinErrorInfo() );

	strappend ( szPath, sizeof(szPath), " --ntservice" );
	for ( int i=1; i<argc; i++ )
		if ( strcmp ( argv[i], "--install" ) )
	{
		strappend ( szPath, sizeof(szPath), " " );
		strappend ( szPath, sizeof(szPath), argv[i] );
	}

	SC_HANDLE hSCM = ServiceOpenManager ();
	SC_HANDLE hService = CreateService (
		hSCM,							// SCM database 
		g_sServiceName,					// name of service 
		g_sServiceName,					// service name to display 
		SERVICE_ALL_ACCESS,				// desired access 
		SERVICE_WIN32_OWN_PROCESS,		// service type 
		SERVICE_AUTO_START,				// start type 
		SERVICE_ERROR_NORMAL,			// error control type 
		szPath,							// path to service's binary 
		NULL,							// no load ordering group 
		NULL,							// no tag identifier 
		NULL,							// no dependencies 
		NULL,							// LocalSystem account 
		NULL );							// no password 

	if ( !hService ) 
	{
		CloseServiceHandle ( hSCM );
		sphFatal ( "CreateService() failed: %s", WinErrorInfo() );

	} else
	{
		sphInfo ( "Service '%s' installed succesfully.", g_sServiceName ); 
	}

	CSphString sDesc;
	sDesc.SetSprintf ( "%s-%s", g_sServiceName, SPHINX_VERSION );

	SERVICE_DESCRIPTION tDesc;
	tDesc.lpDescription = (LPSTR) sDesc.cstr();
	if ( !ChangeServiceConfig2 ( hService, SERVICE_CONFIG_DESCRIPTION, &tDesc ) )
		sphWarning ( "failed to set service description" );

	CloseServiceHandle ( hService ); 
	CloseServiceHandle ( hSCM );
}


void ServiceDelete ()
{
	if ( g_bService )
		return;

	sphInfo ( "Deleting service..." );

	// open manager
	SC_HANDLE hSCM = ServiceOpenManager ();

	// open service
	SC_HANDLE hService = OpenService ( hSCM, g_sServiceName, DELETE );
	if ( !hService )
	{ 
		CloseServiceHandle ( hSCM );
		sphFatal ( "OpenService() failed: %s", WinErrorInfo() );
	}

	// do delete
	bool bRes = !!DeleteService(hService);
	CloseServiceHandle ( hService );
	CloseServiceHandle ( hSCM );

	if ( !bRes ) 
		sphFatal ( "DeleteService() failed: %s", WinErrorInfo() );
	else
		sphInfo ( "Service '%s' deleted succesfully.", g_sServiceName ); 

}
#endif // USE_WINDOWS


void ShowHelp ()
{
	fprintf ( stdout,
		"Usage: searchd [OPTIONS]\n"
		"\n"
		"Options are:\n"
		"-h, --help\t\tdisplay this help message\n"
		"-c, -config <file>\tread configuration from specified file\n"
		"\t\t\t(default is sphinx.conf)\n"
		"--stop\t\t\tsend SIGTERM to currently running searchd\n"
		"\t\t\t(PID is taken from pid_file specified in config file)\n"
		"--iostats\t\tlog per-query io stats\n"
#if USE_WINDOWS
		"--install\t\tinstall as Windows service\n"
		"--delete\t\tdelete Windows service\n"
		"--servicename <name>\tuse given service name (default is 'searchd')\n"
#endif
		"\n"
		"Debugging options are:\n"
		"--console\t\trun in console mode (do not fork, do not log to files)\n"
		"-p, --port <port>\tlisten on given port (overrides config setting)\n"
		"-l, --listen <spec>\tlisten on given address, port or path (overrides\n"
		"\t\t\tconfig settings)\n"
		"-i, --index <index>\tonly serve one given index\n"
#if !USE_WINDOWS
		"--nodetach\t\tdo not detach into background\n"
#endif
		"\n"
		"Examples:\n"
		"searchd --config /usr/local/sphinx/etc/sphinx.conf\n"
#if USE_WINDOWS
		"searchd --install --config c:\\sphinx\\sphinx.conf\n"
#endif
		);
}


#if USE_WINDOWS
BOOL WINAPI CtrlHandler ( DWORD )
{
	if ( !g_bService )
		g_bGotSigterm = true;
	return TRUE;
}
#endif


/// check for incoming signals, and react on them
void CheckSignals ()
{
#if USE_WINDOWS
	if ( g_bService && g_bServiceStop )
	{
		Shutdown ();
		MySetServiceStatus ( SERVICE_STOPPED, NO_ERROR, 0 );
		exit ( 0 );
	}
#endif

	if ( g_bGotSighup )
	{
		sphInfo ( "rotating indices (seamless=%d)", (int)g_bSeamlessRotate ); // this might hang if performed from SIGHUP
		g_bGotSighup = false;
	}

	if ( g_bGotSigterm )
	{
		assert ( g_bHeadDaemon );
		sphInfo ( "caught SIGTERM, shutting down" );

		Shutdown ();
		exit ( 0 );
	}

#if !USE_WINDOWS
	if ( g_bGotSigchld )
	{
		while ( waitpid ( -1, NULL, WNOHANG ) > 0 )
			g_iChildren--;

		g_bGotSigchld = false;
	}
#endif

#if USE_WINDOWS
	BYTE dPipeInBuf [ WIN32_PIPE_BUFSIZE ];
	DWORD nBytesRead = 0;
	BOOL bSuccess = ReadFile ( g_hPipe, dPipeInBuf, WIN32_PIPE_BUFSIZE, &nBytesRead, NULL );
	if ( nBytesRead > 0 && bSuccess )
	{
		for ( DWORD i = 0; i < nBytesRead; i++ )
		{
			switch ( dPipeInBuf [i] )
			{
			case 0:
				g_bDoRotate = true;
				g_bGotSighup = true;
				break;

			case 1:
				g_bGotSigterm = true;
				if ( g_bService )
					g_bServiceStop = true;
				break;
			}

		}

		DisconnectNamedPipe ( g_hPipe );
		ConnectNamedPipe ( g_hPipe, NULL ); 
	}
#endif
}


int WINAPI ServiceMain ( int argc, char **argv )
{
	g_bLogTty = isatty ( g_iLogFile )!=0;

#if USE_WINDOWS
	CSphVector<char *> dArgs;
	if ( g_bService )
	{
		g_ssHandle = RegisterServiceCtrlHandler ( g_sServiceName, ServiceControl );
		if ( !g_ssHandle )
			sphFatal ( "failed to start service: RegisterServiceCtrlHandler() failed: %s", WinErrorInfo() );

		g_ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
		MySetServiceStatus ( SERVICE_START_PENDING, NO_ERROR, 4000 );

		if ( argc<=1 )
		{
			dArgs.Resize ( g_dArgs.GetLength() );
			ARRAY_FOREACH ( i, g_dArgs )
				dArgs[i] = (char*) g_dArgs[i].cstr();

			argc = g_dArgs.GetLength();
			argv = &dArgs[0];
		}
	}

	char szPipeName [64];
	sprintf ( szPipeName, "\\\\.\\pipe\\searchd_%d", getpid() );
	g_hPipe = CreateNamedPipe ( szPipeName, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, PIPE_UNLIMITED_INSTANCES, 0, WIN32_PIPE_BUFSIZE, NMPWAIT_NOWAIT, NULL );
	ConnectNamedPipe ( g_hPipe, NULL ); 
#endif

	if ( !g_bService )
		fprintf ( stdout, SPHINX_BANNER );

	//////////////////////
	// parse command line
	//////////////////////

	CSphConfig		conf;
	bool			bOptStop		= false;
	bool			bOptPIDFile		= false;
	const char *	sOptIndex		= NULL;
	
	int				iOptPort		= 0;
	bool			bOptPort		= false;
	
	CSphString		sOptListen;
	bool			bOptListen		= false;

	#define OPT(_a1,_a2)	else if ( !strcmp(argv[i],_a1) || !strcmp(argv[i],_a2) )
	#define OPT1(_a1)		else if ( !strcmp(argv[i],_a1) )

	int i;
	for ( i=1; i<argc; i++ )
	{
		// handle non-options
		if ( argv[i][0]!='-' )		break;

		// handle no-arg options
		OPT ( "-h", "--help" )		{ ShowHelp(); return 0; }
		OPT1 ( "--console" )		{ g_bOptConsole = true; g_bOptNoDetach = true; }
		OPT1 ( "--stop" )			bOptStop = true;
		OPT1 ( "--pidfile" )		bOptPIDFile = true;
		OPT1 ( "--iostats" )		g_bIOStats = true;
#if USE_WINDOWS
		OPT1 ( "--install" )		{ if ( !g_bService ) { ServiceInstall ( argc, argv ); return 0; } }
		OPT1 ( "--delete" )			{ if ( !g_bService ) { ServiceDelete (); return 0; } }
		OPT1 ( "--ntservice" )		; // it's valid but handled elsewhere
#else
		OPT1 ( "--nodetach" )		g_bOptNoDetach = true;
#endif

		// handle 1-arg options
		else if ( (i+1)>=argc )		break;
		OPT ( "-c", "--config" )	g_sConfigFile = argv[++i];
		OPT ( "-p", "--port" )		{ bOptPort = true; iOptPort = atoi ( argv[++i] ); }
		OPT ( "-l", "--listen" )	{ bOptListen = true; sOptListen = argv[++i]; }
		OPT ( "-i", "--index" )		sOptIndex = argv[++i];
#if USE_WINDOWS
		OPT1 ( "--servicename" )	++i; // it's valid but handled elsewhere
#endif

		// handle unknown options
		else break;
	}
	if ( i!=argc )
		sphFatal ( "malformed or unknown option near '%s'; use '-h' or '--help' to see available options.", argv[i] );

#if USE_WINDOWS
	if ( !g_bService )
	{
		sphWarning ( "forcing --console mode on Windows" );
		g_bOptConsole = g_bOptNoDetach = true;
	}

	// init WSA on Windows
	// we need to do it this early because otherwise gethostbyname() from config parser could fail
	WSADATA tWSAData;
	int iStartupErr = WSAStartup ( WINSOCK_VERSION, &tWSAData );
	if ( iStartupErr )
		sphFatal ( "failed to initialize WinSock2: %s", sphSockError(iStartupErr) );
#endif

	if ( !bOptPIDFile )
		bOptPIDFile = !g_bOptConsole;

	// check port and listen arguments early
	if ( !g_bOptConsole && ( bOptPort || bOptListen ) )
	{
		sphWarning ( "--listen and --port are only allowed in --console debug mode; switch ignored" );
		bOptPort = bOptListen = false;
	}

	if ( bOptPort )
	{
		if ( bOptListen )
			sphFatal ( "please specify either --port or --listen, not both" );

		CheckPort ( iOptPort );
	}

	/////////////////////
	// parse config file
	/////////////////////

	// fallback to defaults if there was no explicit config specified
	while ( !g_sConfigFile.cstr() )
	{
#ifdef SYSCONFDIR
		g_sConfigFile = SYSCONFDIR "/sphinx.conf";
		if ( sphIsReadable ( g_sConfigFile.cstr () ) )
			break;
#endif

		g_sConfigFile = "./sphinx.conf";
		if ( sphIsReadable ( g_sConfigFile.cstr () ) )
			break;

		g_sConfigFile = NULL;
		break;
	}

	if ( !g_sConfigFile.cstr () )
		sphFatal ( "no readable config file (looked in "
#ifdef SYSCONFDIR
			SYSCONFDIR "/sphinx.conf, "
#endif
			"./sphinx.conf)." );

	sphInfo ( "using config file '%s'...", g_sConfigFile.cstr () );

	CheckConfigChanges ();

	// do parse
	CSphConfigParser cp;
	if ( !cp.Parse ( g_sConfigFile.cstr () ) )
		sphFatal ( "failed to parse config file '%s'", g_sConfigFile.cstr () );

	const CSphConfig & hConf = cp.m_tConf;

	if ( !hConf.Exists ( "searchd" ) || !hConf["searchd"].Exists ( "searchd" ) )
		sphFatal ( "'searchd' config section not found in '%s'", g_sConfigFile.cstr () );

	const CSphConfigSection & hSearchd = hConf["searchd"]["searchd"];

	////////////////////////
	// stop running searchd
	////////////////////////

	if ( bOptStop )
	{
		if ( !hSearchd("pid_file") )
			sphFatal ( "stop: option 'pid_file' not found in '%s' section 'searchd'", g_sConfigFile.cstr () );

		const char * sPid = hSearchd["pid_file"].cstr(); // shortcut
		FILE * fp = fopen ( sPid, "r" );
		if ( !fp )
			sphFatal ( "stop: pid file '%s' does not exist or is not readable", sPid );

		char sBuf[16];
		int iLen = (int) fread ( sBuf, 1, sizeof(sBuf)-1, fp );
		sBuf[iLen] = '\0';
		fclose ( fp );

		int iPid = atoi(sBuf);
		if ( iPid<=0 )
			sphFatal ( "stop: failed to read valid pid from '%s'", sPid );

#if USE_WINDOWS
		bool bTerminatedOk = false;

		char szPipeName [64];
		sprintf ( szPipeName, "\\\\.\\pipe\\searchd_%d", iPid );

		HANDLE hPipe = INVALID_HANDLE_VALUE;

		while ( hPipe == INVALID_HANDLE_VALUE ) 
		{ 
			hPipe = CreateFile ( szPipeName, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL );

			if ( hPipe == INVALID_HANDLE_VALUE ) 
			{
				if ( GetLastError () != ERROR_PIPE_BUSY ) 
				{
					fprintf ( stdout, "WARNING: could not open pipe (GetLastError()=%d)\n", GetLastError () );
					break;
				}

				if ( !WaitNamedPipe ( szPipeName, 1000 ) ) 
				{ 
					fprintf ( stdout, "WARNING: could not open pipe (GetLastError()=%d)\n", GetLastError () );
					break;
				} 
			}
		} 

		if ( hPipe != INVALID_HANDLE_VALUE )
		{	
			DWORD uWritten = 0;
			BYTE uWrite = 1;
			BOOL bResult = WriteFile ( hPipe, &uWrite, 1, &uWritten, NULL );
			if ( !bResult )
				fprintf ( stdout, "WARNING: failed to send SIGHTERM to searchd (pid=%d, GetLastError()=%d)\n", iPid, GetLastError () );

			bTerminatedOk = !!bResult;

			CloseHandle ( hPipe );
		}

		if ( bTerminatedOk )
		{
			sphInfo ( "stop: succesfully terminated pid %d", iPid );
			exit ( 0 );
		}			
		else
			sphFatal ( "stop: error terminating pid %d", iPid );
#else
		if ( kill ( iPid, SIGTERM ) )
			sphFatal ( "stop: kill() on pid %d failed: %s", iPid, strerror(errno) );
		else
		{
			sphInfo ( "stop: succesfully sent SIGTERM to pid %d", iPid );
			exit ( 0 );
		}
#endif
	}

	/////////////////////
	// configure searchd
	/////////////////////

	if ( !hConf.Exists ( "index" ) )
		sphFatal ( "no indexes found in '%s'", g_sConfigFile.cstr () );

	#define CONF_CHECK(_hash,_key,_msg,_add) \
		if (!( _hash.Exists ( _key ) )) \
			sphFatal ( "mandatory option '%s' not found " _msg, _key, _add );

	if ( bOptPIDFile )
		CONF_CHECK ( hSearchd, "pid_file", "in 'searchd' section", "" );

	if ( hSearchd.Exists ( "read_timeout" ) && hSearchd["read_timeout"].intval()>=0 )
		g_iReadTimeout = hSearchd["read_timeout"].intval();

	if ( hSearchd.Exists ( "client_timeout" ) && hSearchd["client_timeout"].intval()>=0 )
		g_iClientTimeout = hSearchd["client_timeout"].intval();

	if ( hSearchd.Exists ( "max_children" ) && hSearchd["max_children"].intval()>=0 )
		g_iMaxChildren = hSearchd["max_children"].intval();

	g_bPreopenIndexes = hSearchd.GetInt ( "preopen_indexes", (int)g_bPreopenIndexes ) != 0;
	g_bOnDiskDicts = hSearchd.GetInt ( "ondisk_dict_default", (int)g_bOnDiskDicts ) != 0;
	g_bUnlinkOld = hSearchd.GetInt ( "unlink_old", (int)g_bUnlinkOld ) != 0;

	if ( hSearchd("max_matches") )
	{
		int iMax = hSearchd["max_matches"].intval();
		if ( iMax<0 || iMax>10000000 )
		{
			sphWarning ( "max_matches=%d out of bounds; using default 1000", iMax );
		} else
		{
			g_iMaxMatches = iMax;
		}
	}

	if ( hSearchd("seamless_rotate") )
		g_bSeamlessRotate = ( hSearchd["seamless_rotate"].intval()!=0 );

	if ( !g_bSeamlessRotate && g_bPreopenIndexes )
		sphWarning ( "preopen_indexes=1 has no effect with seamless_rotate=0" );

#if USE_WINDOWS
	if ( g_bSeamlessRotate )
	{
		sphWarning ( "seamless_rotate is not yet supported in windows; forcing seamless_rotate=0" );
		g_bSeamlessRotate = false;
	}
#endif

	g_iAttrFlushPeriod = hSearchd.GetInt ( "attr_flush_period", g_iAttrFlushPeriod );
	g_iMaxPacketSize = hSearchd.GetSize ( "max_packet_size", g_iMaxPacketSize );
	g_iMaxFilters = hSearchd.GetInt ( "max_filters", g_iMaxFilters );
	g_iMaxFilterValues = hSearchd.GetInt ( "max_filter_values", g_iMaxFilterValues );

	if ( g_iMaxPacketSize<128*1024 || g_iMaxPacketSize>128*1024*1024 )
		sphFatal ( "max_packet_size out of bounds (128K..128M)" );

	if ( g_iMaxFilters<1 || g_iMaxFilters>10240 )
		sphFatal ( "max_filters out of bounds (1..10240)" );

	if ( g_iMaxFilterValues<1 || g_iMaxFilterValues>1048576 )
		sphFatal ( "max_filter_values out of bounds (1..1048576)" );

	// create and lock pid
	if ( bOptPIDFile )
	{
		g_sPidFile = hSearchd["pid_file"].cstr();

		g_iPidFD = ::open ( g_sPidFile, O_CREAT | O_WRONLY, S_IREAD | S_IWRITE );
		if ( g_iPidFD<0 )
			sphFatal ( "failed to create pid file '%s': %s", g_sPidFile, strerror(errno) );

		if ( !sphLockEx ( g_iPidFD, false ) )
			sphFatal ( "failed to lock pid file '%s': %s (searchd already running?)", g_sPidFile, strerror(errno) );
	}

	if ( hSearchd("crash_log_path") )
	{
		g_sCrashLog_Path = hSearchd["crash_log_path"].cstr();
		g_bCrashLog_Enabled = true;

		char sPath[1024];
		snprintf ( sPath, sizeof(sPath), "%s.test", g_sCrashLog_Path );
		int iFd = open ( sPath, O_CREAT | O_WRONLY, 0644 );
		if ( iFd == -1 )
			sphWarning ( "unable to create files in crash_log_path: %s", strerror(errno) );
		else
		{
			close ( iFd );
			unlink ( sPath );
		}
	}

	////////////////////
	// network startup
	////////////////////

	// command line arguments override config (but only in --console)
	if ( bOptListen )
		g_dSockets.Add ( sphParseAndBind ( sOptListen ) );
	else if ( bOptPort )
		g_dSockets.Add ( sphCreateInetSocket ( htonl(INADDR_ANY), iOptPort ) );
	else 
	{
		// listen directives in configuration file
		for ( CSphVariant *v = hSearchd("listen"); v; v = v->m_pNext )
			g_dSockets.Add ( sphParseAndBind(*v) );
		
		// handle deprecated directives
		if ( hSearchd("port") )
		{
			DWORD uAddr = hSearchd.Exists("address") ?
				sphGetAddress ( hSearchd["address"].cstr(), GETADDR_STRICT ) : htonl(INADDR_ANY);

			int iPort = hSearchd["port"].intval();
			CheckPort(iPort);
			
			g_dSockets.Add ( sphCreateInetSocket ( uAddr, iPort ) );
		}
		
		// still nothing? listen on INADDR_ANY, default port
		if ( g_dSockets.GetLength() == 0 )
			g_dSockets.Add ( sphCreateInetSocket ( htonl(INADDR_ANY), SEARCHD_DEFAULT_PORT ) );
	}

#if !USE_WINDOWS
	// reserve an fd for clients
	int iDevNull = open ( "/dev/null", O_RDWR );
	int iClientFD = dup(iDevNull);
#endif

	//////////////////////
	// build indexes hash
	//////////////////////

	// configure and preload
	int iValidIndexes = 0;
	hConf["index"].IterateStart ();
	while ( hConf["index"].IterateNext() )
	{
		const CSphConfigSection & hIndex = hConf["index"].IterateGet();
		const char * sIndexName = hConf["index"].IterateGetKey().cstr();

		if ( g_bOptConsole && sOptIndex && strcasecmp ( sIndexName, sOptIndex )!=0 )
			continue;

		AddIndex ( sIndexName, hIndex );
		if ( g_hIndexes.Exists ( sIndexName ) )
		{
			ServedIndex_t & tIndex = g_hIndexes [sIndexName];

			if ( HasFiles ( tIndex, g_dNewExts ) )
			{
				tIndex.m_bOnlyNew = !HasFiles ( tIndex, g_dCurExts );
				if ( RotateIndexGreedy ( tIndex, sIndexName ) )
				{
					CSphString sError;
					if ( !sphFixupIndexSettings ( tIndex.m_pIndex, hIndex, sError ) )
					{
						sphWarning ( "index '%s': %s - NOT SERVING", sIndexName, sError.cstr() );
						tIndex.m_bEnabled = false;
					}
				}
				else
				{
					if ( PrereadNewIndex ( tIndex, hIndex, sIndexName ) )
						tIndex.m_bEnabled = true;
				}
			}
			else
			{
				tIndex.m_bOnlyNew = false;
				if ( PrereadNewIndex ( tIndex, hIndex, sIndexName ) )
					tIndex.m_bEnabled = true;
			}

			CSphString sError;
			if ( tIndex.m_bEnabled && !CheckIndex ( tIndex.m_pIndex, sError ) )
			{
				sphWarning ( "index '%s': %s - NOT SERVING", sIndexName, sError.cstr() );
				tIndex.m_bEnabled = false;
			}

			if ( !tIndex.m_bEnabled )
				continue;
		}

		iValidIndexes++;
	}
	if ( !iValidIndexes )
		sphFatal ( "no valid indexes to serve" );

	///////////
	// startup
	///////////

	// handle my signals
	SetSignalHandlers ();

	// setup mva updates arena
	sphArenaInit ( hSearchd.GetSize ( "mva_updates_pool", 1048576 ) );

	// create logs
	if ( !g_bOptConsole )
	{
		// create log
		const char * sLog = "searchd.log";
		if ( hSearchd.Exists ( "log" ) )
			sLog = hSearchd["log"].cstr();

		umask ( 066 );
		g_iLogFile = open ( sLog, O_CREAT | O_RDWR | O_APPEND, S_IREAD | S_IWRITE );
		if ( g_iLogFile<0 )
		{
			g_iLogFile = STDOUT_FILENO;
			sphFatal ( "failed to open log file '%s': %s", sLog, strerror(errno) );
		}

		g_bLogTty = isatty ( g_iLogFile )!=0;
		g_sLogFile = sLog;

		// create query log if required
		if ( hSearchd.Exists ( "query_log" ) )
		{
			g_iQueryLogFile = open ( hSearchd["query_log"].cstr(), O_CREAT | O_RDWR | O_APPEND, S_IREAD | S_IWRITE );
			if ( g_iQueryLogFile<0 )
				sphFatal ( "failed to open query log file '%s': %s", hSearchd["query_log"].cstr(), strerror(errno) );
			g_sQueryLogFile = hSearchd["query_log"].cstr();
		}
	}

	// almost ready, time to start listening
	ARRAY_FOREACH ( i, g_dSockets )
		if ( listen ( g_dSockets[i], SEARCHD_BACKLOG )==-1 )
			sphFatal ( "listen() failed: %s", sphSockError() );

	// prepare to detach
	if ( !g_bOptNoDetach )
	{
#if !USE_WINDOWS
		close ( STDIN_FILENO );
		close ( STDOUT_FILENO );
		close ( STDERR_FILENO );
		dup2 ( iDevNull, STDIN_FILENO );
		dup2 ( iDevNull, STDOUT_FILENO );
		dup2 ( iDevNull, STDERR_FILENO );
#endif
		g_bLogStdout = false;

		// explicitly unlock everything in parent immediately before fork
		//
		// there's a race in case another instance is started before
		// child re-acquires all locks; but let's hope that's rare
		g_hIndexes.IterateStart ();
		while ( g_hIndexes.IterateNext () )
		{
			ServedIndex_t & tServed = g_hIndexes.IterateGet ();
			if ( tServed.m_bEnabled )
				tServed.m_pIndex->Unlock();
		}
	}

	if ( bOptPIDFile )
		sphLockUn ( g_iPidFD );

#if !USE_WINDOWS
	if ( !g_bOptNoDetach )
	{
		switch ( fork() )
		{
			case -1:
				// error
				Shutdown ();
				sphFatal ( "fork() failed (reason: %s)", strerror ( errno ) );
				exit ( 1 );

			case 0:
				// daemonized child
				break;

			default:
				// tty-controlled parent
				sphSetProcessInfo ( false );
				exit ( 0 );
		}
	}
#endif

	if ( bOptPIDFile )
	{
#if !USE_WINDOWS
		// re-lock pid
		// FIXME! there's a potential race here
		if ( !sphLockEx ( g_iPidFD, true ) )
			sphFatal ( "failed to re-lock pid file '%s': %s", g_sPidFile, strerror(errno) );
#endif

		char sPid[16];
		snprintf ( sPid, sizeof(sPid), "%d\n", getpid() );
		int iPidLen = strlen(sPid);

		if ( ::write ( g_iPidFD, sPid, iPidLen )!=iPidLen )
			sphFatal ( "failed to write to pid file '%s': %s", g_sPidFile, strerror(errno) );
		ftruncate ( g_iPidFD, iPidLen );
	}

#if USE_WINDOWS
	SetConsoleCtrlHandler ( CtrlHandler, TRUE );
#endif

	if ( !g_bOptNoDetach )
	{
		// re-lock indexes
		g_hIndexes.IterateStart ();
		while ( g_hIndexes.IterateNext () )
		{
			ServedIndex_t & tServed = g_hIndexes.IterateGet ();
			if ( !tServed.m_bEnabled )
				continue;

			// obtain exclusive lock
			if ( !tServed.m_pIndex->Lock() )
			{
				sphWarning ( "index '%s': lock: %s; INDEX UNUSABLE", g_hIndexes.IterateGetKey().cstr(),
					tServed.m_pIndex->GetLastError().cstr() );
				tServed.m_bEnabled = false;
				continue;
			}

			// try to mlock again because mlock does not survive over fork
			if ( !tServed.m_pIndex->Mlock() )
			{
				sphWarning ( "index '%s': %s", g_hIndexes.IterateGetKey().cstr(),
					tServed.m_pIndex->GetLastError().cstr() );
			}
		}

	}

	// if we're running in console mode, dump queries to tty as well
	if ( g_bOptConsole )
		g_iQueryLogFile = g_iLogFile;

	/////////////////
	// serve clients
	/////////////////

	g_bHeadDaemon = true;
	sphInfo ( "accepting connections" );

#if USE_WINDOWS
	if ( g_bService )
		MySetServiceStatus ( SERVICE_RUNNING, NO_ERROR, 0 );
#endif

	sphSetInternalErrorCallback ( LogInternalError );

	fd_set fdsAccept;
	FD_ZERO ( &fdsAccept );

	int iNfds = 0;
	ARRAY_FOREACH ( i, g_dSockets )
		iNfds = Max ( iNfds, g_dSockets[i] );
	iNfds++;

	for ( ;; )
	{
		CheckSignals ();
		CheckLeaks ();
		CheckPipes ();
		CheckDelete ();
		CheckRotate ();
		CheckReopen ();
		CheckFlush ();
		sphLog ( LOG_INFO, NULL, NULL ); // flush dupes

		ARRAY_FOREACH ( i, g_dSockets )
			sphFDSet ( g_dSockets[i], &fdsAccept );

		struct timeval tvTimeout;
		tvTimeout.tv_sec = USE_WINDOWS ? 0 : 1;
		tvTimeout.tv_usec = USE_WINDOWS ? 50000 : 0;

		int iRes = select ( iNfds, &fdsAccept, NULL, NULL, &tvTimeout );
		if ( iRes == 0 )
			continue;
		if ( iRes == -1 )
		{
			int iErrno = sphSockGetErrno();
			if ( iErrno == EINTR || iErrno == EAGAIN || iErrno == EWOULDBLOCK )
				continue;
			
			static int iLastErrno = -1;
			if ( iLastErrno != iErrno )
				sphWarning ( "select() failed: %s", sphSockError(iErrno) );
			iLastErrno = iErrno;
			continue;
		}

		ARRAY_FOREACH ( i, g_dSockets )
		{
			if ( !FD_ISSET ( g_dSockets[i], &fdsAccept ) )
				continue;

			struct sockaddr_storage saStorage;
			socklen_t uLength = sizeof(saStorage);
			int iClientSock = accept ( g_dSockets[i], (struct sockaddr *)&saStorage, &uLength );

			if ( iClientSock == -1 )
			{
				const int iErrno = sphSockGetErrno();
				if ( iErrno==EINTR || iErrno==ECONNABORTED || iErrno==EAGAIN || iErrno==EWOULDBLOCK )
					continue;

				sphFatal ( "accept() failed: %s", sphSockError(iErrno) );
			}
			
			if ( ( g_iMaxChildren && g_iChildren>=g_iMaxChildren )
				 || ( g_bDoRotate && !g_bSeamlessRotate ) )
			{
				const char * sMessage = "server maxed out, retry in a second";
				int iRespLen = 4 + strlen(sMessage);
				
				NetOutputBuffer_c tOut ( iClientSock );
				tOut.SendInt ( SPHINX_SEARCHD_PROTO );
				tOut.SendWord ( SEARCHD_RETRY );
				tOut.SendWord ( 0 ); // version doesn't matter
				tOut.SendInt ( iRespLen );
				tOut.SendString ( sMessage );
				tOut.Flush ();
				
				sphWarning ( "maxed out, dismissing client" );
				sphSockClose ( iClientSock );
				break;
			}

			char sClientName[SPH_ADDRESS_SIZE];
			switch ( saStorage.ss_family )
			{
			case AF_INET:
				sphFormatIP ( sClientName, sizeof(sClientName), ((struct sockaddr_in *)&saStorage)->sin_addr.s_addr );
				break;

			case AF_UNIX:
				strncpy ( sClientName, "(local)", sizeof(sClientName) );
				break;

			default:
				sClientName[0] = '\0';
				break;
			}

			// handle the client
			if ( g_bOptConsole || g_bService )
			{
				#if !USE_WINDOWS
				if ( SPH_FDSET_OVERFLOW(iClientSock) )
					iClientSock = dup2 ( iClientSock, iClientFD );
				#endif
				HandleClient ( iClientSock, sClientName, -1 );
				sphSockClose ( iClientSock );
				continue;
			}

			#if !USE_WINDOWS
			int iChildPipe = PipeAndFork ( false, -1 );
			if ( !g_bHeadDaemon )
			{
				// child process, handle client
				if ( SPH_FDSET_OVERFLOW(iClientSock) )
					iClientSock = dup2 ( iClientSock, iClientFD );
				HandleClient ( iClientSock, sClientName, iChildPipe );
				sphSockClose ( iClientSock );
				exit ( 0 );
			} else
			{
				// parent process, continue accept()ing
				sphSockClose ( iClientSock );
			}
			#endif // !USE_WINDOWS
		}
	}
}


bool DieCallback ( const char * sMessage )
{
	sphLogFatal ( "%s", sMessage );
	return false; // caller should not log
}


int main ( int argc, char **argv )
{
	sphSetDieCallback ( DieCallback );

#if USE_WINDOWS
	int iNameIndex = -1;
	for ( int i=1; i<argc; i++ )
	{
		if ( strcmp ( argv[i], "--ntservice" )==0 )
			g_bService = true;

		if ( strcmp ( argv[i], "--servicename" )==0 && (i+1)<argc )
		{
			iNameIndex = i+1;
			g_sServiceName = argv[iNameIndex];
		}
	}

	if ( g_bService )
	{
		for ( int i=0; i<argc; i++ )
			g_dArgs.Add ( argv[i] );

		if ( iNameIndex>=0 )
			g_sServiceName = g_dArgs[iNameIndex].cstr ();

		SERVICE_TABLE_ENTRY dDispatcherTable[] =
		{
			{ (LPSTR) g_sServiceName, (LPSERVICE_MAIN_FUNCTION)ServiceMain },
			{ NULL, NULL }
		};
		if ( !StartServiceCtrlDispatcher ( dDispatcherTable ) )
			sphFatal ( "StartServiceCtrlDispatcher() failed: %s", WinErrorInfo() );
	} else
#endif

	return ServiceMain ( argc, argv );
}

//
// $Id: searchd.cpp 1564 2008-11-12 20:48:11Z xale $
//
