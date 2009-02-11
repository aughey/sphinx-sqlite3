//
// $Id: sphinxsort.cpp 1521 2008-10-24 23:12:19Z shodan $
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

#include <time.h>
#include <math.h>

#if !USE_WINDOWS
#include <unistd.h>
#include <sys/time.h>
#endif

//////////////////////////////////////////////////////////////////////////
// TRAITS
//////////////////////////////////////////////////////////////////////////

/// helper to get i-th sorter attr from match
template<bool BITS>
inline SphAttr_t sphGetCompAttr ( const CSphMatchComparatorState & t, const CSphMatch & m, int i );

template<>
inline SphAttr_t sphGetCompAttr<false> ( const CSphMatchComparatorState & t, const CSphMatch & m, int i )
{
	return m.GetAttr ( t.m_tLocator[i] ); // FIXME! OPTIMIZE!!! we can go the short route here
}

template<>
inline SphAttr_t sphGetCompAttr<true> ( const CSphMatchComparatorState & t, const CSphMatch & m, int i )
{
	return m.GetAttr ( t.m_tLocator[i] );
}

//////////////////////////////////////////////////////////////////////////

/// groupby key type
typedef int64_t				SphGroupKey_t;


/// base grouper (class that computes groupby key)
class CSphGrouper
{
public:
	virtual					~CSphGrouper () {}
	virtual SphGroupKey_t	KeyFromValue ( SphAttr_t uValue ) const = 0;
	virtual SphGroupKey_t	KeyFromMatch ( const CSphMatch & tMatch ) const = 0;
	virtual void			GetLocator ( CSphAttrLocator & tOut ) const = 0;
	virtual DWORD			GetResultType () const = 0;
};


/// match-sorting priority queue traits
class CSphMatchQueueTraits : public ISphMatchSorter, ISphNoncopyable
{
protected:
	CSphMatch *					m_pData;
	int							m_iUsed;
	int							m_iSize;

	CSphMatchComparatorState	m_tState;
	const bool					m_bUsesAttrs;

public:
	/// ctor
	CSphMatchQueueTraits ( int iSize, bool bUsesAttrs )
		: m_iUsed ( 0 )
		, m_iSize ( iSize )
		, m_bUsesAttrs ( bUsesAttrs )
	{
		assert ( iSize>0 );
		m_pData = new CSphMatch [ iSize ];
		assert ( m_pData );

		m_tState.m_iNow = (DWORD) time ( NULL );
	}

	/// dtor
	~CSphMatchQueueTraits ()
	{
		SafeDeleteArray ( m_pData );
	}

public:
	int			GetLength () const										{ return m_iUsed; }
	void		SetState ( const CSphMatchComparatorState & tState )	{ m_tState = tState; m_tState.m_iNow = (DWORD) time ( NULL ); }
	bool		UsesAttrs ()											{ return m_bUsesAttrs; }
	CSphMatch *	First ()												{ return m_pData; }
};

//////////////////////////////////////////////////////////////////////////
// PLAIN SORTING QUEUE
//////////////////////////////////////////////////////////////////////////

/// normal match-sorting priority queue
template < typename COMP > class CSphMatchQueue : public CSphMatchQueueTraits
{
public:
	/// ctor
	CSphMatchQueue ( int iSize, bool bUsesAttrs )
		: CSphMatchQueueTraits ( iSize, bUsesAttrs )
	{}

	/// check if this sorter does groupby
	virtual bool IsGroupby ()
	{
		return false;
	}

	/// add entry to the queue
	virtual bool Push ( const CSphMatch & tEntry )
	{
		m_iTotal++;

		if ( m_iUsed==m_iSize )
		{
			// if it's worse that current min, reject it, else pop off current min
			if ( COMP::IsLess ( tEntry, m_pData[0], m_tState ) )
				return true;
			else
				Pop ();
		}

		// do add
		m_pData [ m_iUsed ] = tEntry;
		int iEntry = m_iUsed++;

		// sift up if needed, so that worst (lesser) ones float to the top
		while ( iEntry )
		{
			int iParent = ( iEntry-1 ) >> 1;
			if ( !COMP::IsLess ( m_pData[iEntry], m_pData[iParent], m_tState ) )
				break;

			// entry is less than parent, should float to the top
			Swap ( m_pData[iEntry], m_pData[iParent] );
			iEntry = iParent;
		}

		return true;
	}

	/// remove root (ie. top priority) entry
	virtual void Pop ()
	{
		assert ( m_iUsed );
		if ( !(--m_iUsed) ) // empty queue? just return
			return;

		// make the last entry my new root
		m_pData[0] = m_pData[m_iUsed];

		// sift down if needed
		int iEntry = 0;
		for ( ;; )
		{
			// select child
			int iChild = (iEntry<<1) + 1;
			if ( iChild>=m_iUsed )
				break;

			// select smallest child
			if ( iChild+1<m_iUsed )
				if ( COMP::IsLess ( m_pData[iChild+1], m_pData[iChild], m_tState ) )
					iChild++;

			// if smallest child is less than entry, do float it to the top
			if ( COMP::IsLess ( m_pData[iChild], m_pData[iEntry], m_tState ) )
			{
				Swap ( m_pData[iChild], m_pData[iEntry] );
				iEntry = iChild;
				continue;
			}

			break;
		}
	}

	/// store all entries into specified location in sorted order, and remove them from queue
	void Flatten ( CSphMatch * pTo, int iTag )
	{
		assert ( m_iUsed>=0 );
		pTo += m_iUsed;
		while ( m_iUsed>0 )
		{
			--pTo;
			pTo[0] = m_pData[0]; // OPTIMIZE? reset dst + swap?
			if ( iTag>=0 )
				pTo->m_iTag = iTag;
			Pop ();
		}
		m_iTotal = 0;
	}
};

//////////////////////////////////////////////////////////////////////////
// SORTING+GROUPING QUEUE
//////////////////////////////////////////////////////////////////////////

/// groupers
#define GROUPER_BEGIN(_name) \
	class _name : public CSphGrouper \
	{ \
	protected: \
		CSphAttrLocator m_tLocator; \
	public: \
		_name ( const CSphAttrLocator & tLoc ) : m_tLocator ( tLoc ) {} \
		virtual void GetLocator ( CSphAttrLocator & tOut ) const { tOut = m_tLocator; } \
		virtual DWORD GetResultType () const { return m_tLocator.m_iBitCount>8*(int)sizeof(DWORD) ? SPH_ATTR_BIGINT : SPH_ATTR_INTEGER; } \
		virtual SphGroupKey_t KeyFromMatch ( const CSphMatch & tMatch ) const { return KeyFromValue ( tMatch.GetAttr ( m_tLocator ) ); } \
		virtual SphGroupKey_t KeyFromValue ( SphAttr_t uValue ) const \
		{

#define GROUPER_END \
		} \
	};


#define GROUPER_BEGIN_SPLIT(_name) \
	GROUPER_BEGIN(_name) \
	time_t tStamp = (time_t)uValue; \
	struct tm * pSplit = localtime ( &tStamp );


GROUPER_BEGIN ( CSphGrouperAttr )
	return uValue;
GROUPER_END


GROUPER_BEGIN_SPLIT ( CSphGrouperDay )
	return (pSplit->tm_year+1900)*10000 + (1+pSplit->tm_mon)*100 + pSplit->tm_mday;
GROUPER_END


GROUPER_BEGIN_SPLIT ( CSphGrouperWeek )
	int iPrevSunday = (1+pSplit->tm_yday) - pSplit->tm_wday; // prev Sunday day of year, base 1
	int iYear = pSplit->tm_year+1900;
	if ( iPrevSunday<=0 ) // check if we crossed year boundary
	{
		// adjust day and year
		iPrevSunday += 365;
		iYear--;

		// adjust for leap years
		if ( iYear%4==0 && ( iYear%100!=0 || iYear%400==0 ) )
			iPrevSunday++;
	}
	return iYear*1000 + iPrevSunday;
GROUPER_END


GROUPER_BEGIN_SPLIT ( CSphGrouperMonth )
	return (pSplit->tm_year+1900)*100 + (1+pSplit->tm_mon);
GROUPER_END


GROUPER_BEGIN_SPLIT ( CSphGrouperYear )
	return (pSplit->tm_year+1900);
GROUPER_END

//////////////////////////////////////////////////////////////////////////

/// simple fixed-size hash
/// doesn't keep the order
template < typename T, typename KEY, typename HASHFUNC >
class CSphFixedHash : ISphNoncopyable
{
protected:
	static const int			HASH_LIST_END	= -1;
	static const int			HASH_DELETED	= -2;

	struct HashEntry_t
	{
		KEY		m_tKey;
		T		m_tValue;
		int		m_iNext;
	};

protected:
	CSphVector<HashEntry_t>		m_dEntries;		///< key-value pairs storage pool
	CSphVector<int>				m_dHash;		///< hash into m_dEntries pool

	int							m_iFree;		///< free pairs count
	CSphVector<int>				m_dFree;		///< free pair indexes

public:
	/// ctor
	CSphFixedHash ( int iLength )
	{
		int iBuckets = 2<<sphLog2(iLength-1); // less than 50% bucket usage guaranteed
		assert ( iLength>0 );
		assert ( iLength<=iBuckets );

		m_dEntries.Resize ( iLength );
		m_dHash.Resize ( iBuckets );
		m_dFree.Resize ( iLength );

		Reset ();
	}

	/// cleanup
	void Reset ()
	{
		ARRAY_FOREACH ( i, m_dEntries )
			m_dEntries[i].m_iNext = HASH_DELETED;

		ARRAY_FOREACH ( i, m_dHash )
			m_dHash[i] = HASH_LIST_END;

		m_iFree = m_dFree.GetLength();
		ARRAY_FOREACH ( i, m_dFree )
			m_dFree[i] = i;
	}

	/// add new entry
	/// returns NULL on success
	/// returns pointer to value if already hashed
	T * Add ( const T & tValue, const KEY & tKey )
	{
		assert ( m_iFree>0 && "hash overflow" );

		// check if it's already hashed
		DWORD uHash = DWORD ( HASHFUNC::Hash ( tKey ) ) & ( m_dHash.GetLength()-1 );
		int iPrev = -1, iEntry;

		for ( iEntry=m_dHash[uHash]; iEntry>=0; iPrev=iEntry, iEntry=m_dEntries[iEntry].m_iNext )
			if ( m_dEntries[iEntry].m_tKey==tKey )
				return &m_dEntries[iEntry].m_tValue;
		assert ( iEntry!=HASH_DELETED );

		// if it's not, do add
		int iNew = m_dFree [ --m_iFree ];

		HashEntry_t & tNew = m_dEntries[iNew];
		assert ( tNew.m_iNext==HASH_DELETED );

		tNew.m_tKey = tKey;
		tNew.m_tValue = tValue;
		tNew.m_iNext = HASH_LIST_END;

		if ( iPrev>=0 )
		{
			assert ( m_dEntries[iPrev].m_iNext==HASH_LIST_END );
			m_dEntries[iPrev].m_iNext = iNew;
		} else
		{
			assert ( m_dHash[uHash]==HASH_LIST_END );
			m_dHash[uHash] = iNew;
		}
		return NULL;
	}

	/// remove entry from hash
	void Remove ( const KEY & tKey )
	{
		// check if it's already hashed
		DWORD uHash = DWORD ( HASHFUNC::Hash ( tKey ) ) & ( m_dHash.GetLength()-1 );
		int iPrev = -1, iEntry;

		for ( iEntry=m_dHash[uHash]; iEntry>=0; iPrev=iEntry, iEntry=m_dEntries[iEntry].m_iNext )
			if ( m_dEntries[iEntry].m_tKey==tKey )
		{
			// found, remove it
			assert ( m_dEntries[iEntry].m_iNext!=HASH_DELETED );
			if ( iPrev>=0 )
				m_dEntries[iPrev].m_iNext = m_dEntries[iEntry].m_iNext;
			else
				m_dHash[uHash] = m_dEntries[iEntry].m_iNext;

#ifndef NDEBUG
			m_dEntries[iEntry].m_iNext = HASH_DELETED;
#endif

			m_dFree [ m_iFree++ ] = iEntry;
			return;
		}
		assert ( iEntry!=HASH_DELETED );
	}

	/// get value pointer by key
	T * operator () ( const KEY & tKey ) const
	{
		DWORD uHash = DWORD ( HASHFUNC::Hash ( tKey ) ) & ( m_dHash.GetLength()-1 );
		int iEntry;

		for ( iEntry=m_dHash[uHash]; iEntry>=0; iEntry=m_dEntries[iEntry].m_iNext )
			if ( m_dEntries[iEntry].m_tKey==tKey )
				return (T*)&m_dEntries[iEntry].m_tValue;

		assert ( iEntry!=HASH_DELETED );
		return NULL;
	}
};


struct IdentityHash_fn
{
	static inline int64_t	Hash ( int64_t iValue )		{ return iValue; }
	static inline DWORD		Hash ( DWORD iValue )		{ return iValue; }
	static inline int		Hash ( int iValue )			{ return iValue; }
};

/////////////////////////////////////////////////////////////////////////////

/// (group,attrvalue) pair
struct SphGroupedValue_t
{
public:
	SphGroupKey_t	m_uGroup;
	SphAttr_t		m_uValue;

public:
	SphGroupedValue_t ()
	{}

	SphGroupedValue_t ( SphGroupKey_t uGroup, SphAttr_t uValue )
		: m_uGroup ( uGroup )
		, m_uValue ( uValue )
	{}

	inline bool operator < ( const SphGroupedValue_t & rhs ) const
	{
		if ( m_uGroup<rhs.m_uGroup ) return true;
		if ( m_uGroup>rhs.m_uGroup ) return false;
		return m_uValue<rhs.m_uValue;
	}

	inline bool operator == ( const SphGroupedValue_t & rhs ) const
	{
		return m_uGroup==rhs.m_uGroup && m_uValue==rhs.m_uValue;
	}
};


/// unique values counter
/// used for COUNT(DISTINCT xxx) GROUP BY yyy queries
class CSphUniqounter : public CSphVector<SphGroupedValue_t>
{
public:
#ifndef NDEBUG
					CSphUniqounter () : m_iCountPos ( 0 ), m_bSorted ( true ) { Reserve ( 16384 ); }
	void			Add ( const SphGroupedValue_t & tValue )	{ CSphVector<SphGroupedValue_t>::Add ( tValue ); m_bSorted = false; }
	void			Sort ()										{ CSphVector<SphGroupedValue_t>::Sort (); m_bSorted = true; }

#else
					CSphUniqounter () : m_iCountPos ( 0 ) {}
#endif

public:
	int				CountStart ( SphGroupKey_t * pOutGroup );	///< starting counting distinct values, returns count and group key (0 if empty)
	int				CountNext ( SphGroupKey_t * pOutGroup );	///< continues counting distinct values, returns count and group key (0 if done)
	void			Compact ( SphGroupKey_t * pRemoveGroups, int iRemoveGroups );

protected:
	int				m_iCountPos;

#ifndef NDEBUG
	bool			m_bSorted;
#endif
};


int CSphUniqounter::CountStart ( SphGroupKey_t * pOutGroup )
{
	m_iCountPos = 0;
	return CountNext ( pOutGroup );
}


int CSphUniqounter::CountNext ( SphGroupKey_t * pOutGroup )
{
	assert ( m_bSorted );
	if ( m_iCountPos>=m_iLength )
		return 0;

	SphGroupKey_t uGroup = m_pData[m_iCountPos].m_uGroup;
	SphAttr_t uValue = m_pData[m_iCountPos].m_uValue;
	*pOutGroup = uGroup;

	int iCount = 1;
	while ( m_iCountPos<m_iLength && m_pData[m_iCountPos].m_uGroup==uGroup )
	{
		if ( m_pData[m_iCountPos].m_uValue!=uValue )
			iCount++;
		uValue = m_pData[m_iCountPos].m_uValue;
		m_iCountPos++;
	}
	return iCount;
}


void CSphUniqounter::Compact ( SphGroupKey_t * pRemoveGroups, int iRemoveGroups )
{
	assert ( m_bSorted );
	if ( !m_iLength )
		return;

	sphSort ( pRemoveGroups, iRemoveGroups );

	SphGroupedValue_t * pSrc = m_pData;
	SphGroupedValue_t * pDst = m_pData;
	SphGroupedValue_t * pEnd = m_pData + m_iLength;

	// skip remove-groups which are not in my list
	while ( iRemoveGroups && (*pRemoveGroups)<pSrc->m_uGroup )
	{
		pRemoveGroups++;
		iRemoveGroups--;
	}

	for ( ; pSrc<pEnd; pSrc++ )
	{
		// check if this entry needs to be removed
		while ( iRemoveGroups && (*pRemoveGroups)<pSrc->m_uGroup )
		{
			pRemoveGroups++;
			iRemoveGroups--;
		}
		if ( iRemoveGroups && pSrc->m_uGroup==*pRemoveGroups )
			continue;

		// check if it's a dupe
		if ( pDst>m_pData && pDst[-1]==pSrc[0] )
			continue;

		*pDst++ = *pSrc;
	}

	assert ( pDst-m_pData<=m_iLength );
	m_iLength = pDst-m_pData;
}

/////////////////////////////////////////////////////////////////////////////

/// attribute magic
enum
{
	SPH_VATTR_ID			= -1,	///< tells match sorter to use doc id
	SPH_VATTR_RELEVANCE		= -2,	///< tells match sorter to use match weight
	SPH_VATTR_FLOAT			= 10000	///< tells match sorter to compare floats
};


/// match comparator interface from group-by sorter point of view
struct ISphMatchComparator
{
	virtual ~ISphMatchComparator () {}
	virtual bool VirtualIsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & tState ) const = 0;
};


/// additional group-by sorter settings
struct CSphGroupSorterSettings
{
	CSphAttrLocator		m_tLocGroupby;		///< locator for @groupby
	CSphAttrLocator		m_tLocCount;		///< locator for @count
	CSphAttrLocator		m_tLocDistinct;		///< locator for @distinct
	CSphAttrLocator		m_tDistinctLoc;		///< locator for attribute to compute count(distinct) for
	int					m_iPresortRowitems;	///< how much raw (non-groupby) rowitems should the sorter expect
	int					m_iAddRowitems;		///< how much rowitems to add for non-grouped matches
	bool				m_bDistinct;		///< whether we need distinct
	bool				m_bMVA;				///< whether we're grouping by MVA attribute
	CSphGrouper *		m_pGrouper;			///< group key calculator

	CSphGroupSorterSettings ()
		: m_iPresortRowitems ( 0 )
		, m_iAddRowitems ( 0 )
		, m_bDistinct ( false )
		, m_bMVA ( false )
	{}
};


#if USE_WINDOWS
#pragma warning(disable:4127)
#endif


/// match sorter with k-buffering and group-by
template < typename COMPGROUP, bool DISTINCT >
class CSphKBufferGroupSorter : public CSphMatchQueueTraits
{
protected:
	const int		m_iRowitems;		///< normal match rowitems count (to distinguish already grouped matches)

	ESphGroupBy		m_eGroupBy;			///< group-by function
	CSphGrouper *	m_pGrouper;

	CSphFixedHash < CSphMatch *, SphGroupKey_t, IdentityHash_fn >	m_hGroup2Match;

protected:
	int				m_iLimit;		///< max matches to be retrieved

	CSphUniqounter	m_tUniq;
	bool			m_bSortByDistinct;

	CSphMatchComparatorState	m_tStateGroup;
	const ISphMatchComparator *	m_pComp;

	CSphGroupSorterSettings		m_tSettings;

	static const int			GROUPBY_FACTOR = 4;	///< allocate this times more storage when doing group-by (k, as in k-buffer)

public:
	/// ctor
	CSphKBufferGroupSorter ( const ISphMatchComparator * pComp, const CSphQuery * pQuery, const CSphGroupSorterSettings & tSettings ) // FIXME! make k configurable
		: CSphMatchQueueTraits ( pQuery->m_iMaxMatches*GROUPBY_FACTOR, true )
		, m_iRowitems		( tSettings.m_iPresortRowitems )
		, m_eGroupBy		( pQuery->m_eGroupFunc )
		, m_pGrouper		( tSettings.m_pGrouper )
		, m_hGroup2Match	( pQuery->m_iMaxMatches*GROUPBY_FACTOR )
		, m_iLimit			( pQuery->m_iMaxMatches )
		, m_bSortByDistinct	( false )
		, m_pComp			( pComp )
		, m_tSettings		( tSettings )
	{
		assert ( GROUPBY_FACTOR>1 );
		assert ( DISTINCT==false || tSettings.m_tDistinctLoc.m_iBitOffset>=0 );
	}

	/// dtor
	~CSphKBufferGroupSorter ()
	{
		SafeDelete ( m_pComp );
		SafeDelete ( m_pGrouper );
	}

	/// check if this sorter does groupby
	virtual bool IsGroupby ()
	{
		return true;
	}

	/// add entry to the queue
	virtual bool Push ( const CSphMatch & tEntry )
	{
		SphGroupKey_t uGroupKey = m_pGrouper->KeyFromMatch ( tEntry );
		return PushEx ( tEntry, uGroupKey );
	}

	/// add entry to the queue
	virtual bool PushEx ( const CSphMatch & tEntry, const SphGroupKey_t uGroupKey )
	{
		// check whether incoming match is already grouped
		assert ( tEntry.m_iRowitems==m_iRowitems || tEntry.m_iRowitems==m_iRowitems+m_tSettings.m_iAddRowitems );

		bool bGrouped = ( tEntry.m_iRowitems!=m_iRowitems );

		// if this group is already hashed, we only need to update the corresponding match
		CSphMatch ** ppMatch = m_hGroup2Match ( uGroupKey );
		if ( ppMatch )
		{
			CSphMatch * pMatch = (*ppMatch);
			assert ( pMatch );
			assert ( pMatch->GetAttr(m_tSettings.m_tLocGroupby)==uGroupKey );

			if ( bGrouped )
			{
				// it's already grouped match
				// sum grouped matches count
				assert ( pMatch->m_iRowitems==tEntry.m_iRowitems );
				pMatch->SetAttr ( m_tSettings.m_tLocCount, pMatch->GetAttr(m_tSettings.m_tLocCount) + tEntry.GetAttr(m_tSettings.m_tLocCount) ); // OPTIMIZE! AddAttr()?
				if ( DISTINCT )
					pMatch->SetAttr ( m_tSettings.m_tLocDistinct, pMatch->GetAttr(m_tSettings.m_tLocDistinct) + tEntry.GetAttr(m_tSettings.m_tLocDistinct) );

			} else
			{
				// it's a simple match
				// increase grouped matches count
				assert ( pMatch->m_iRowitems==tEntry.m_iRowitems+m_tSettings.m_iAddRowitems );
				pMatch->SetAttr ( m_tSettings.m_tLocCount, 1+pMatch->GetAttr(m_tSettings.m_tLocCount) ); // OPTIMIZE! IncAttr()?
			}

			// if new entry is more relevant, update from it
			if ( m_pComp->VirtualIsLess ( *pMatch, tEntry, m_tState ) )
			{
				pMatch->m_iDocID = tEntry.m_iDocID;
				pMatch->m_iWeight = tEntry.m_iWeight;
				pMatch->m_iTag = tEntry.m_iTag;

				for ( int i=0; i<pMatch->m_iRowitems-m_tSettings.m_iAddRowitems; i++ )
					pMatch->m_pRowitems[i] = tEntry.m_pRowitems[i];
			}
		}

		// submit actual distinct value in all cases
		if ( DISTINCT && !bGrouped )
			m_tUniq.Add ( SphGroupedValue_t ( uGroupKey, tEntry.GetAttr(m_tSettings.m_tDistinctLoc) ) ); // OPTIMIZE! use simpler locator here?

		// it's a dupe anyway, so we shouldn't update total matches count
		if ( ppMatch )
			return false;

		// if we're full, let's cut off some worst groups
		if ( m_iUsed==m_iSize )
			CutWorst ();

		// do add
		assert ( m_iUsed<m_iSize );
		CSphMatch & tNew = m_pData [ m_iUsed++ ];

		int iNewSize = tEntry.m_iRowitems + ( bGrouped ? 0 : m_tSettings.m_iAddRowitems );
		assert ( tNew.m_iRowitems==0 || tNew.m_iRowitems==iNewSize );

		if ( !tNew.m_iRowitems )
			tNew.Reset ( iNewSize );

		tNew.m_iDocID = tEntry.m_iDocID;
		tNew.m_iWeight = tEntry.m_iWeight;
		tNew.m_iTag = tEntry.m_iTag;

		memcpy ( tNew.m_pRowitems, tEntry.m_pRowitems, tEntry.m_iRowitems*sizeof(CSphRowitem) );

		if ( !bGrouped )
		{
			tNew.SetAttr ( m_tSettings.m_tLocGroupby, uGroupKey );
			tNew.SetAttr ( m_tSettings.m_tLocCount, 1 );
			if ( DISTINCT )
				tNew.SetAttr ( m_tSettings.m_tLocDistinct, 0 );
		}

		m_hGroup2Match.Add ( &tNew, uGroupKey );
		m_iTotal++;
		return true;
	}

	/// store all entries into specified location in sorted order, and remove them from queue
	void Flatten ( CSphMatch * pTo, int iTag )
	{
		CountDistinct ();
		SortGroups ();

		int iLen = GetLength ();
		for ( int i=0; i<iLen; i++, pTo++ )
		{
			pTo[0] = m_pData[i];
			if ( iTag>=0 )
				pTo->m_iTag = iTag;
		}

		m_iUsed = 0;
		m_iTotal = 0;

		m_hGroup2Match.Reset ();
		if ( DISTINCT )
			m_tUniq.Resize ( 0 );
	}

	/// get entries count
	int GetLength () const
	{
		return Min ( m_iUsed, m_iLimit );
	}

	/// set group comparator state
	void SetGroupState ( const CSphMatchComparatorState & tState )
	{
		m_tStateGroup = tState;

		if ( DISTINCT && m_tSettings.m_tDistinctLoc.m_iBitOffset>=0 )
			for ( int i=0; i<CSphMatchComparatorState::MAX_ATTRS; i++ )
				if ( m_tStateGroup.m_tLocator[i].m_iBitOffset==m_tSettings.m_tDistinctLoc.m_iBitOffset )
			{
				m_bSortByDistinct = true;
				break;
			}
	}

protected:
	/// count distinct values if necessary
	void CountDistinct ()
	{
		if ( DISTINCT )
		{
			m_tUniq.Sort ();
			SphGroupKey_t uGroup;
			for ( int iCount = m_tUniq.CountStart(&uGroup); iCount; iCount = m_tUniq.CountNext(&uGroup) )
			{
				CSphMatch ** ppMatch = m_hGroup2Match(uGroup);
				if ( ppMatch )
					(*ppMatch)->SetAttr ( m_tSettings.m_tLocDistinct, iCount );
			}
		}
	}

	/// cut worst N groups off the buffer tail
	void CutWorst ()
	{
		// sort groups
		if ( m_bSortByDistinct )
			CountDistinct ();
		SortGroups ();

		// cut groups
		int iCut = m_iLimit * (int)(GROUPBY_FACTOR/2);
		m_iUsed -= iCut;

		// cleanup unused distinct stuff
		if ( DISTINCT )
		{
			// build kill-list
			CSphVector<SphGroupKey_t> dRemove;
			dRemove.Resize ( iCut );
			for ( int i=0; i<iCut; i++ )
				dRemove[i] = m_pData[m_iUsed+i].GetAttr ( m_tSettings.m_tLocGroupby );

			// sort and compact
			if ( !m_bSortByDistinct )
				m_tUniq.Sort ();
			m_tUniq.Compact ( &dRemove[0], iCut );
		}

		// rehash
		m_hGroup2Match.Reset ();
		for ( int i=0; i<m_iUsed; i++ )
			m_hGroup2Match.Add ( m_pData+i, m_pData[i].GetAttr ( m_tSettings.m_tLocGroupby ) );
	}

	/// sort groups buffer
	void SortGroups ()
	{
		CSphMatch * pData = m_pData;
		int iCount = m_iUsed;

		int st0[32], st1[32], a, b, k, i, j;
		CSphMatch x;

		k = 1;
		st0[0] = 0;
		st1[0] = iCount-1;
		while ( k )
		{
			k--;
			i = a = st0[k];
			j = b = st1[k];
			x = pData [ (a+b)/2 ]; // FIXME! add better median at least
			while ( a<b )
			{
				while ( i<=j )
				{

					while ( COMPGROUP::IsLess ( x, pData[i], m_tStateGroup ) ) i++;
					while ( COMPGROUP::IsLess ( pData[j], x, m_tStateGroup ) ) j--;
					if (i <= j) { Swap ( pData[i], pData[j] ); i++; j--; }
				}

				if ( j-a>=b-i )
				{
					if ( a<j ) { st0[k] = a; st1[k] = j; k++; }
					a = i;
				} else
				{
					if ( i<b ) { st0[k] = i; st1[k] = b; k++; }
					b = j;
				}
			}
		}
	}
};


/// match sorter with k-buffering and group-by for MVAs
template < typename COMPGROUP, bool DISTINCT >
class CSphKBufferMVAGroupSorter : public CSphKBufferGroupSorter < COMPGROUP, DISTINCT >
{
protected:
	const DWORD *		m_pMva;		///< pointer to MVA pool for incoming matches
	CSphAttrLocator		m_tMvaLocator;

public:
	/// ctor
	CSphKBufferMVAGroupSorter ( const ISphMatchComparator * pComp, const CSphQuery * pQuery, const CSphGroupSorterSettings & tSettings )
		: CSphKBufferGroupSorter < COMPGROUP, DISTINCT > ( pComp, pQuery, tSettings )
		, m_pMva ( NULL )
	{
		this->m_pGrouper->GetLocator ( m_tMvaLocator );
	}

	/// check if this sorter does groupby
	virtual bool IsGroupby ()
	{
		return true;
	}

	/// set MVA pool for subsequent matches
	void SetMVAPool ( const DWORD * pMva )
	{
		m_pMva = pMva;
	}

	/// add entry to the queue
	virtual bool Push ( const CSphMatch & tEntry )
	{
		// check whether incoming match is already grouped
		if ( tEntry.m_iRowitems!=this->m_iRowitems )
		{
			// it must be pre-grouped; well, just re-group it based on the group key
			// (first 'this' is for icc; second 'this' is for gcc)
			return this->PushEx ( tEntry, tEntry.GetAttr(this->m_tSettings.m_tLocGroupby) );
		}

		// ungrouped match
		if ( !m_pMva )
			return false;

		// get that list
		// FIXME! OPTIMIZE! use simpler locator than full bits/count here
		// FIXME! hardcoded MVA type, so here's MVA_DOWNSIZE marker for searching
		const DWORD * pValues = tEntry.GetAttrMVA ( this->m_tMvaLocator, m_pMva ); // (this pointer is for gcc; it doesn't work otherwise)
		if ( !pValues )
			return false;

		DWORD iValues = *pValues++;

		bool bRes = false;
		while ( iValues-- )
		{
			SphGroupKey_t uGroupkey = this->m_pGrouper->KeyFromValue ( *pValues++ );
			bRes |= this->PushEx ( tEntry, uGroupkey );
		}
		return bRes;
	}
};


#if USE_WINDOWS
#pragma warning(default:4127)
#endif

//////////////////////////////////////////////////////////////////////////
// PLAIN SORTING FUNCTORS 
//////////////////////////////////////////////////////////////////////////

/// match sorter
template < bool BITS >
struct MatchRelevanceLt_fn : public ISphMatchComparator
{
	virtual bool VirtualIsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t ) const
	{
		return IsLess ( a, b, t );
	}

	static bool IsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & )
	{
		if ( a.m_iWeight!=b.m_iWeight )
			return a.m_iWeight < b.m_iWeight;

		return a.m_iDocID > b.m_iDocID;
	};
};


/// match sorter
template < bool BITS >
struct MatchAttrLt_fn : public ISphMatchComparator
{
	virtual bool VirtualIsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t ) const
	{
		return IsLess ( a, b, t );
	}

	static inline bool IsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t )
	{
		SphAttr_t aa = sphGetCompAttr<BITS> ( t, a, 0 );
		SphAttr_t bb = sphGetCompAttr<BITS> ( t, b, 0 );
		if ( aa!=bb )
			return aa<bb;

		if ( a.m_iWeight!=b.m_iWeight )
			return a.m_iWeight < b.m_iWeight;

		return a.m_iDocID > b.m_iDocID;
	};
};


/// match sorter
template < bool BITS >
struct MatchAttrGt_fn : public ISphMatchComparator
{
	virtual bool VirtualIsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t ) const
	{
		return IsLess ( a, b, t );
	}

	static inline bool IsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t )
	{
		SphAttr_t aa = sphGetCompAttr<BITS> ( t, a, 0 );
		SphAttr_t bb = sphGetCompAttr<BITS> ( t, b, 0 );
		if ( aa!=bb )
			return aa>bb;

		if ( a.m_iWeight!=b.m_iWeight )
			return a.m_iWeight < b.m_iWeight;

		return a.m_iDocID > b.m_iDocID;
	};
};


/// match sorter
template < bool BITS >
struct MatchTimeSegments_fn : public ISphMatchComparator
{
	virtual bool VirtualIsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t ) const
	{
		return IsLess ( a, b, t );
	}

	static inline bool IsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t )
	{
		SphAttr_t aa = sphGetCompAttr<BITS> ( t, a, 0 );
		SphAttr_t bb = sphGetCompAttr<BITS> ( t, b, 0 );
		int iA = GetSegment ( aa, t.m_iNow );
		int iB = GetSegment ( bb, t.m_iNow );
		if ( iA!=iB )
			return iA > iB;

		if ( a.m_iWeight!=b.m_iWeight )
			return a.m_iWeight < b.m_iWeight;

		if ( aa!=bb )
			return aa<bb;

		return a.m_iDocID > b.m_iDocID;
	};

protected:
	static inline int GetSegment ( SphAttr_t iStamp, SphAttr_t iNow )
	{
		if ( iStamp>=iNow-3600 ) return 0; // last hour
		if ( iStamp>=iNow-24*3600 ) return 1; // last day
		if ( iStamp>=iNow-7*24*3600 ) return 2; // last week
		if ( iStamp>=iNow-30*24*3600 ) return 3; // last month
		if ( iStamp>=iNow-90*24*3600 ) return 4; // last 3 months
		return 5; // everything else
	}
};


/// match sorter
struct MatchExpr_fn : public ISphMatchComparator
{
	virtual bool VirtualIsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t ) const
	{
		return IsLess ( a, b, t );
	}

	static inline bool IsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t )
	{
		float aa = a.GetAttrFloat ( t.m_tLocator[0] ); // FIXME! OPTIMIZE!!! simplified (dword-granular) getter could be used here
		float bb = b.GetAttrFloat ( t.m_tLocator[0] );
		if ( aa!=bb )
			return aa<bb;
		return a.m_iDocID>b.m_iDocID;
	}
};

/////////////////////////////////////////////////////////////////////////////

#define SPH_TEST_PAIR(_aa,_bb,_idx ) \
	if ( (_aa)!=(_bb) ) \
		return ( (t.m_uAttrDesc>>(_idx))&1 ) ^ ( (_aa)>(_bb) );


#define SPH_TEST_KEYPART(_idx) \
	switch ( t.m_iAttr[_idx] ) \
	{ \
		case SPH_VATTR_ID:			SPH_TEST_PAIR ( a.m_iDocID, b.m_iDocID, _idx ); break; \
		case SPH_VATTR_RELEVANCE:	SPH_TEST_PAIR ( a.m_iWeight, b.m_iWeight, _idx ); break; \
		case SPH_VATTR_FLOAT: \
		{ \
			register float aa = a.GetAttrFloat ( t.m_tLocator[_idx] ); \
			register float bb = b.GetAttrFloat ( t.m_tLocator[_idx] ); \
			SPH_TEST_PAIR ( aa, bb, _idx ) \
			break; \
		} \
		default: \
		{ \
			register SphAttr_t aa = sphGetCompAttr<BITS> ( t, a, _idx ); \
			register SphAttr_t bb = sphGetCompAttr<BITS> ( t, b, _idx ); \
			SPH_TEST_PAIR ( aa, bb, _idx ); \
			break; \
		} \
	}


template < bool BITS >
struct MatchGeneric2_fn : public ISphMatchComparator
{
	virtual bool VirtualIsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t ) const
	{
		return IsLess ( a, b, t );
	}

	static inline bool IsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t )
	{
		SPH_TEST_KEYPART(0);
		SPH_TEST_KEYPART(1);
		return false;
	};
};


template < bool BITS >
struct MatchGeneric3_fn : public ISphMatchComparator
{
	virtual bool VirtualIsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t ) const
	{
		return IsLess ( a, b, t );
	}

	static inline bool IsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t )
	{
		SPH_TEST_KEYPART(0);
		SPH_TEST_KEYPART(1);
		SPH_TEST_KEYPART(2);
		return false;
	};
};


template < bool BITS >
struct MatchGeneric4_fn : public ISphMatchComparator
{
	virtual bool VirtualIsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t ) const
	{
		return IsLess ( a, b, t );
	}

	static inline bool IsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t )
	{
		SPH_TEST_KEYPART(0);
		SPH_TEST_KEYPART(1);
		SPH_TEST_KEYPART(2);
		SPH_TEST_KEYPART(3);
		return false;
	};
};


template < bool BITS >
struct MatchGeneric5_fn : public ISphMatchComparator
{
	virtual bool VirtualIsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t ) const
	{
		return IsLess ( a, b, t );
	}

	static inline bool IsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t )
	{
		SPH_TEST_KEYPART(0);
		SPH_TEST_KEYPART(1);
		SPH_TEST_KEYPART(2);
		SPH_TEST_KEYPART(3);
		SPH_TEST_KEYPART(4);
		return false;
	};
};

//////////////////////////////////////////////////////////////////////////

template < bool BITS >
struct MatchCustom_fn : public ISphMatchComparator
{
	virtual bool VirtualIsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t ) const
	{
		return IsLess ( a, b, t );
	}

	// setup sorting state
	static bool SetupAttr ( const CSphSchema & tSchema, CSphMatchComparatorState & tState, CSphString & sError, int iIdx, const char * sAttr )
	{
		if ( iIdx>=CSphMatchComparatorState::MAX_ATTRS )
		{
			sError.SetSprintf ( "custom sort: too many attributes declared" );
			return false;
		}

		int iAttr = tSchema.GetAttrIndex(sAttr);
		if ( iAttr<0 )
		{
			sError.SetSprintf ( "custom sort: attr '%s' not found in schema", sAttr );
			return false;
		}

		const CSphColumnInfo & tAttr = tSchema.GetAttr(iAttr);
		tState.m_iAttr[iIdx] = iAttr;
		tState.m_tLocator[iIdx] = tAttr.m_tLocator;
		return true;
	}

	// setup sorting state
	static bool Setup ( const CSphSchema & tSchema, CSphMatchComparatorState & tState, CSphString & sError )
	{
		float fTmp;
		int iAttr = 0;

#define MATCH_FUNCTION				fTmp
#define MATCH_WEIGHT				1.0f
#define MATCH_NOW					1.0f
#define MATCH_ATTR(_idx)			1.0f
#define MATCH_DECLARE_ATTR(_name)	if ( !SetupAttr ( tSchema, tState, sError, iAttr++, _name ) ) return false;
#include "sphinxcustomsort.inl"
;

		return true;
	}

	// calc function and compare matches
	// OPTIMIZE? could calc once per match on submit
	static inline bool IsLess ( const CSphMatch & a, const CSphMatch & b, const CSphMatchComparatorState & t )
	{
#undef MATCH_DECLARE_ATTR
#undef MATCH_WEIGHT
#undef MATCH_NOW
#undef MATCH_ATTR
#define MATCH_DECLARE_ATTR(_name)	;
#define MATCH_WEIGHT				float(MATCH_VAR.m_iWeight)
#define MATCH_NOW					float(t.m_iNow)
#define MATCH_ATTR(_idx)			float(sphGetCompAttr<BITS>(t,MATCH_VAR,_idx))

		float aa, bb;

#undef MATCH_FUNCTION
#undef MATCH_VAR
#define MATCH_FUNCTION aa
#define MATCH_VAR a
#include "sphinxcustomsort.inl"
;

#undef MATCH_FUNCTION
#undef MATCH_VAR
#define MATCH_FUNCTION bb
#define MATCH_VAR b
#include "sphinxcustomsort.inl"
;

		return aa<bb;
	}
};

//////////////////////////////////////////////////////////////////////////
// SORT CLAUSE PARSER
//////////////////////////////////////////////////////////////////////////

static const int MAX_SORT_FIELDS = 5; // MUST be in sync with CSphMatchComparatorState::m_iAttr


enum ESphSortFunc
{
	FUNC_REL_DESC,
	FUNC_ATTR_DESC,
	FUNC_ATTR_ASC,
	FUNC_TIMESEGS,
	FUNC_GENERIC2,
	FUNC_GENERIC3,
	FUNC_GENERIC4,
	FUNC_GENERIC5,
	FUNC_CUSTOM,
	FUNC_EXPR
};


enum ESortClauseParseResult
{
	SORT_CLAUSE_OK,
	SORT_CLAUSE_ERROR,
	SORT_CLAUSE_RANDOM
};


static ESortClauseParseResult sphParseSortClause ( const char * sClause, const CSphSchema & tSchema,
	ESphSortFunc & eFunc, CSphMatchComparatorState & tState, CSphString & sError )
{
	// mini parser
	CSphScopedPtr<ISphTokenizer> pTokenizer ( sphCreateSBCSTokenizer () );
	if ( !pTokenizer.Ptr() )
	{
		sError.SetSprintf ( "INTERNAL ERROR: failed to create tokenizer when parsing sort clause" );
		return SORT_CLAUSE_ERROR;
	}

	CSphString sTmp;
	pTokenizer->SetCaseFolding ( "0..9, A..Z->a..z, _, a..z, @", sTmp );

	CSphString sSortClause ( sClause );
	pTokenizer->SetBuffer ( (BYTE*)sSortClause.cstr(), strlen(sSortClause.cstr())  );

	bool bField = false; // whether i'm expecting field name or sort order
	int iField = 0;

	for ( const char * pTok=(char*)pTokenizer->GetToken(); pTok; pTok=(char*)pTokenizer->GetToken() )
	{
		bField = !bField;

		// special case, sort by random
		if ( iField==0 && bField && strcmp ( pTok, "@random" )==0 )
			return SORT_CLAUSE_RANDOM;

		// special case, sort by custom
		if ( iField==0 && bField && strcmp ( pTok, "@custom" )==0 )
		{
			eFunc = FUNC_CUSTOM;
			return MatchCustom_fn<false>::Setup ( tSchema, tState, sError ) ? SORT_CLAUSE_OK : SORT_CLAUSE_ERROR;
		}

		// handle sort order
		if ( !bField )
		{
			// check
			if ( strcmp ( pTok, "desc" ) && strcmp ( pTok, "asc" ) )
			{
				sError.SetSprintf ( "invalid sorting order '%s'", pTok );
				return SORT_CLAUSE_ERROR;
			}

			// set
			if ( !strcmp ( pTok, "desc" ) )
				tState.m_uAttrDesc |= ( 1<<iField );

			iField++;
			continue;
		}

		// handle field name
		if ( iField==MAX_SORT_FIELDS )
		{
			sError.SetSprintf ( "too much sort-by fields; maximum count is %d", MAX_SORT_FIELDS );
			return SORT_CLAUSE_ERROR;
		}

		if ( !strcasecmp ( pTok, "@relevance" )
			|| !strcasecmp ( pTok, "@rank" )
			|| !strcasecmp ( pTok, "@weight" ) )
		{
			tState.m_iAttr[iField] = SPH_VATTR_RELEVANCE;

		} else if ( !strcasecmp ( pTok, "@id" ) )
		{
			tState.m_iAttr[iField] = SPH_VATTR_ID;

		} else
		{
			if ( !strcasecmp ( pTok, "@group" ) )
				pTok = "@groupby";

			int iAttr = tSchema.GetAttrIndex ( pTok );
			if ( iAttr<0 )
			{
				sError.SetSprintf ( "sort-by attribute '%s' not found", pTok );
				return SORT_CLAUSE_ERROR;
			}

			tState.m_iAttr[iField] = ( tSchema.GetAttr(iAttr).m_eAttrType==SPH_ATTR_FLOAT ) ? SPH_VATTR_FLOAT : iAttr;
			tState.m_tLocator[iField] = tSchema.GetAttr(iAttr).m_tLocator;
		}
	}

	if ( iField==0 )
	{
		sError.SetSprintf ( "no sort order defined" );
		return SORT_CLAUSE_ERROR;
	}

	if ( iField==1 )
		tState.m_iAttr[iField++] = SPH_VATTR_ID; // add "id ASC"

	switch ( iField )
	{
		case 2:		eFunc = FUNC_GENERIC2; break;
		case 3:		eFunc = FUNC_GENERIC3; break;
		case 4:		eFunc = FUNC_GENERIC4; break;
		case 5:		eFunc = FUNC_GENERIC5; break;
		default:	sError.SetSprintf ( "INTERNAL ERROR: %d fields in sphParseSortClause()", iField ); return SORT_CLAUSE_ERROR;
	}
	return SORT_CLAUSE_OK;
}

//////////////////////////////////////////////////////////////////////////
// SORTING+GROUPING INSTANTIATION
//////////////////////////////////////////////////////////////////////////

template < typename COMPGROUP >
static ISphMatchSorter * sphCreateSorter3rd ( const ISphMatchComparator * pComp, const CSphQuery * pQuery, const CSphGroupSorterSettings & tSettings )
{
	if ( tSettings.m_bMVA==true )
	{
		if ( tSettings.m_bDistinct==true )
			return new CSphKBufferMVAGroupSorter<COMPGROUP,true> ( pComp, pQuery, tSettings);
		else
			return new CSphKBufferMVAGroupSorter<COMPGROUP,false> ( pComp, pQuery, tSettings );
	} else
	{
		if ( tSettings.m_bDistinct==true )
			return new CSphKBufferGroupSorter<COMPGROUP,true> ( pComp, pQuery, tSettings );
		else
			return new CSphKBufferGroupSorter<COMPGROUP,false> ( pComp, pQuery, tSettings );
	}
}


static ISphMatchSorter * sphCreateSorter2nd ( ESphSortFunc eGroupFunc, bool bGroupBits, const ISphMatchComparator * pComp, const CSphQuery * pQuery, const CSphGroupSorterSettings & tSettings )
{
	if ( bGroupBits )
	{
		switch ( eGroupFunc )
		{
			case FUNC_GENERIC2:		return sphCreateSorter3rd<MatchGeneric2_fn<true> >	( pComp, pQuery, tSettings ); break;
			case FUNC_GENERIC3:		return sphCreateSorter3rd<MatchGeneric3_fn<true> >	( pComp, pQuery, tSettings ); break;
			case FUNC_GENERIC4:		return sphCreateSorter3rd<MatchGeneric4_fn<true> >	( pComp, pQuery, tSettings ); break;
			case FUNC_GENERIC5:		return sphCreateSorter3rd<MatchGeneric5_fn<true> >	( pComp, pQuery, tSettings ); break;
			case FUNC_CUSTOM:		return sphCreateSorter3rd<MatchCustom_fn<true>   >	( pComp, pQuery, tSettings ); break;
			default:				return NULL;
		}
	} else
	{
		switch ( eGroupFunc )
		{
			case FUNC_GENERIC2:		return sphCreateSorter3rd<MatchGeneric2_fn<false> >	( pComp, pQuery, tSettings ); break;
			case FUNC_GENERIC3:		return sphCreateSorter3rd<MatchGeneric3_fn<false> >	( pComp, pQuery, tSettings ); break;
			case FUNC_GENERIC4:		return sphCreateSorter3rd<MatchGeneric4_fn<false> >	( pComp, pQuery, tSettings ); break;
			case FUNC_GENERIC5:		return sphCreateSorter3rd<MatchGeneric5_fn<false> >	( pComp, pQuery, tSettings ); break;
			case FUNC_CUSTOM:		return sphCreateSorter3rd<MatchCustom_fn<false>   >	( pComp, pQuery, tSettings ); break;
			case FUNC_EXPR:			return sphCreateSorter3rd<MatchExpr_fn>				( pComp, pQuery, tSettings ); break;
			default:				return NULL;
		}
	}
}


static ISphMatchSorter * sphCreateSorter1st ( ESphSortFunc eMatchFunc, bool bMatchBits, ESphSortFunc eGroupFunc, bool bGroupBits, const CSphQuery * pQuery, const CSphGroupSorterSettings & tSettings )
{
	ISphMatchComparator * pComp = NULL;
	if ( bMatchBits )
	{
		switch ( eMatchFunc )
		{
			case FUNC_REL_DESC:		pComp = new MatchRelevanceLt_fn<true>(); break;
			case FUNC_ATTR_DESC:	pComp = new MatchAttrLt_fn<true>(); break;
			case FUNC_ATTR_ASC:		pComp = new MatchAttrGt_fn<true>(); break;
			case FUNC_TIMESEGS:		pComp = new MatchTimeSegments_fn<true>(); break;
			case FUNC_GENERIC2:		pComp = new MatchGeneric2_fn<true>(); break;
			case FUNC_GENERIC3:		pComp = new MatchGeneric3_fn<true>(); break;
			case FUNC_GENERIC4:		pComp = new MatchGeneric4_fn<true>(); break;
			case FUNC_GENERIC5:		pComp = new MatchGeneric5_fn<true>(); break;
			case FUNC_CUSTOM:		pComp = new MatchCustom_fn<true>(); break;
			case FUNC_EXPR:			pComp = NULL; break; // expr can't be bitfield
		}
	} else
	{
		switch ( eMatchFunc )
		{
			case FUNC_REL_DESC:		pComp = new MatchRelevanceLt_fn<false>(); break;
			case FUNC_ATTR_DESC:	pComp = new MatchAttrLt_fn<false>(); break;
			case FUNC_ATTR_ASC:		pComp = new MatchAttrGt_fn<false>(); break;
			case FUNC_TIMESEGS:		pComp = new MatchTimeSegments_fn<false>(); break;
			case FUNC_GENERIC2:		pComp = new MatchGeneric2_fn<false>(); break;
			case FUNC_GENERIC3:		pComp = new MatchGeneric3_fn<false>(); break;
			case FUNC_GENERIC4:		pComp = new MatchGeneric4_fn<false>(); break;
			case FUNC_GENERIC5:		pComp = new MatchGeneric5_fn<false>(); break;
			case FUNC_CUSTOM:		pComp = new MatchCustom_fn<false>(); break;
			case FUNC_EXPR:			pComp = new MatchExpr_fn(); break; // only for non-bitfields, obviously
		}
	}

	return pComp
		? sphCreateSorter2nd ( eGroupFunc, bGroupBits, pComp, pQuery, tSettings )
		: NULL;
}

//////////////////////////////////////////////////////////////////////////
// GEODIST
//////////////////////////////////////////////////////////////////////////

struct ExprGeodist_t : public ISphExpr
{
public:
						ExprGeodist_t () {}
	bool				Setup ( const CSphQuery * pQuery, const CSphSchema & tSchema, CSphString & sError );
	virtual float		Eval ( const CSphMatch & tMatch ) const;

protected:
	CSphAttrLocator		m_tGeoLatLoc;
	CSphAttrLocator		m_tGeoLongLoc;
	float				m_fGeoAnchorLat;
	float				m_fGeoAnchorLong;
};


bool ExprGeodist_t::Setup ( const CSphQuery * pQuery, const CSphSchema & tSchema, CSphString & sError )
{
	if ( !pQuery->m_bGeoAnchor )
	{
		sError.SetSprintf ( "INTERNAL ERROR: no geoanchor, can not create geodist evaluator" );
		return false;
	}

	int iLat = tSchema.GetAttrIndex ( pQuery->m_sGeoLatAttr.cstr() );
	if ( iLat<0 )
	{
		sError.SetSprintf ( "unknown latitude attribute '%s'", pQuery->m_sGeoLatAttr.cstr() );
		return false;
	}

	int iLong = tSchema.GetAttrIndex ( pQuery->m_sGeoLongAttr.cstr() );
	if ( iLong<0 )
	{
		sError.SetSprintf ( "unknown latitude attribute '%s'", pQuery->m_sGeoLongAttr.cstr() );
		return false;
	}

	m_tGeoLatLoc = tSchema.GetAttr(iLat).m_tLocator;
	m_tGeoLongLoc = tSchema.GetAttr(iLong).m_tLocator;
	m_fGeoAnchorLat = pQuery->m_fGeoLatitude;
	m_fGeoAnchorLong = pQuery->m_fGeoLongitude;
	return true;
}


static inline double sphSqr ( double v )
{
	return v*v;
}


float ExprGeodist_t::Eval ( const CSphMatch & tMatch ) const
{
	const double R = 6384000;
	float plat = tMatch.GetAttrFloat ( m_tGeoLatLoc );
	float plon = tMatch.GetAttrFloat ( m_tGeoLongLoc );
	double dlat = plat - m_fGeoAnchorLat;
	double dlon = plon - m_fGeoAnchorLong;
	double a = sphSqr(sin(dlat/2)) + cos(plat)*cos(m_fGeoAnchorLat)*sphSqr(sin(dlon/2));
	double c = 2*asin ( Min ( 1, sqrt(a) ) );
	return float(R*c);
}

//////////////////////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS (FACTORY AND FLATTENING)
//////////////////////////////////////////////////////////////////////////

static bool SetupGroupbySettings ( const CSphQuery * pQuery, const CSphSchema & tSchema, CSphGroupSorterSettings & tSettings, CSphString & sError )
{
	tSettings.m_tDistinctLoc.m_iBitOffset = -1;

	if ( pQuery->m_sGroupBy.IsEmpty() )
		return true;

	if ( pQuery->m_eGroupFunc==SPH_GROUPBY_ATTRPAIR )
	{
		sError.SetSprintf ( "SPH_GROUPBY_ATTRPAIR is not supported any more (just group on 'bigint' attribute)" );
		return false;
	}

	// setup groupby attr
	int iGroupBy = tSchema.GetAttrIndex ( pQuery->m_sGroupBy.cstr() );
	if ( iGroupBy<0 )
	{
		sError.SetSprintf ( "group-by attribute '%s' not found", pQuery->m_sGroupBy.cstr() );
		return false;
	}

	CSphAttrLocator tLoc = tSchema.GetAttr(iGroupBy).m_tLocator;
	switch ( pQuery->m_eGroupFunc )
	{
		case SPH_GROUPBY_DAY:		tSettings.m_pGrouper = new CSphGrouperDay ( tLoc ); break;
		case SPH_GROUPBY_WEEK:		tSettings.m_pGrouper = new CSphGrouperWeek ( tLoc ); break;
		case SPH_GROUPBY_MONTH:		tSettings.m_pGrouper = new CSphGrouperMonth ( tLoc ); break;
		case SPH_GROUPBY_YEAR:		tSettings.m_pGrouper = new CSphGrouperYear ( tLoc ); break;
		case SPH_GROUPBY_ATTR:		tSettings.m_pGrouper = new CSphGrouperAttr ( tLoc ); break;
		default:
			sError.SetSprintf ( "invalid group-by mode (mode=%d)", pQuery->m_eGroupFunc );
			return false;
	}

	tSettings.m_bMVA = ( tSchema.GetAttr(iGroupBy).m_eAttrType & SPH_ATTR_MULTI )!=0;

	// setup distinct attr
	if ( !pQuery->m_sGroupDistinct.IsEmpty() )
	{
		int iDistinct = tSchema.GetAttrIndex ( pQuery->m_sGroupDistinct.cstr() );
		if ( iDistinct<0 )
		{
			sError.SetSprintf ( "group-count-distinct attribute '%s' not found", pQuery->m_sGroupDistinct.cstr() );
			return false;
		}

		tSettings.m_tDistinctLoc = tSchema.GetAttr(iDistinct).m_tLocator;
	}

	return true;
}


ISphMatchSorter * sphCreateQueue ( const CSphQuery * pQuery, const CSphSchema & tSchema, CSphString & sError, bool bComputeItems )
{
	// prepare for descent
	ISphMatchSorter * pTop = NULL;
	CSphMatchComparatorState tStateMatch, tStateGroup;

	sError = "";

	///////////////////////////////////////
	// build incoming and outgoing schemas
	///////////////////////////////////////

	// incoming schema adds computed expressions on top of the original index schema
	CSphSchema tInSchema = tSchema;

	// setup @geodist
	if ( pQuery->m_bGeoAnchor && tInSchema.GetAttrIndex ( "@geodist" )<0 )
	{
		ExprGeodist_t * pExpr = new ExprGeodist_t ();
		if ( !pExpr->Setup ( pQuery, tSchema, sError ) )
		{
			pExpr->Release ();
			return NULL;
		}

		CSphColumnInfo tCol ( "@geodist", SPH_ATTR_FLOAT );
		tCol.m_pExpr = pExpr; // takes ownership, no need to for explicit pExpr release
		tInSchema.AddAttr ( tCol );
	}

	// setup @expr
	if ( pQuery->m_eSort==SPH_SORT_EXPR && tInSchema.GetAttrIndex ( "@expr" )<0 )
	{
		CSphColumnInfo tCol ( "@expr", SPH_ATTR_FLOAT ); // enforce float type for backwards compatibility (ie. too lazy to fix those tests right now)
		tCol.m_pExpr = sphExprParse ( pQuery->m_sSortBy.cstr(), tSchema, NULL, sError );
		if ( !tCol.m_pExpr )
			return NULL;
		tCol.m_bLateCalc = true;
		tInSchema.AddAttr ( tCol );
	}

	// expressions from select items
	if ( bComputeItems )
		ARRAY_FOREACH ( i, pQuery->m_dItems )
	{
		const CSphQueryItem & tItem = pQuery->m_dItems[i];
		const CSphString & sExpr = tItem.m_sExpr;

		// for now, just always pass "plain" attrs from index to sorter; they will be filtered on searchd level
		if ( sExpr=="*" || tSchema.GetAttrIndex(sExpr.cstr())>=0 )
			continue;

		// not an attribute? must be an expression, and must be aliased
		if ( tItem.m_sAlias.IsEmpty() )
		{
			sError.SetSprintf ( "expression '%s' must be aliased (use 'expr AS alias' syntax)", tItem.m_sExpr.cstr() );
			return NULL;
		}

		// tricky part
		// we might be we're fed with precomputed matches, but it's all or nothing
		// the incoming match either does not have anything computed, or it has everything
		if ( tSchema.GetAttrsCount()==tInSchema.GetAttrsCount() )
		{
			// so far we had everything, so we might be precomputed, and the alias just might already exist
			int iSuspect = tSchema.GetAttrIndex(tItem.m_sAlias.cstr());
			if ( iSuspect>=0 )
			{
				// however, let's ensure that it was an expression
				if ( tSchema.GetAttr(iSuspect).m_pExpr.Ptr()!=NULL )
					continue;

				// otherwise we're not precomputed, *and* have a duplicate name
				sError.SetSprintf ( "alias '%s' must be unique (conflicts with an index attribute)", tItem.m_sAlias.cstr() );
				return NULL;
			}
		} else
		{
			// we are adding stuff, must not be precomputed, check for both kinds of dupes
			if ( tSchema.GetAttrIndex(tItem.m_sAlias.cstr())>=0 )
			{
				sError.SetSprintf ( "alias '%s' must be unique (conflicts with an index attribute)", tItem.m_sAlias.cstr() );
				return NULL;
			}
			if ( tInSchema.GetAttrIndex(tInSchema.m_sName.cstr())>=0 )
			{
				sError.SetSprintf ( "alias '%s' must be unique (conflicts with another alias)", tItem.m_sAlias.cstr() );
				return NULL;
			}
		}

		// a new and shiny expression, lets parse
		CSphColumnInfo tExprCol ( tItem.m_sAlias.cstr(), SPH_ATTR_NONE );
		tExprCol.m_pExpr = sphExprParse ( sExpr.cstr(), tSchema, &tExprCol.m_eAttrType, sError );
		if ( !tExprCol.m_pExpr )
		{
			sError.SetSprintf ( "parse error: %s", sError.cstr() );
			return NULL;
		}

		// do add
		tInSchema.AddAttr ( tExprCol );
	}

	////////////////////////////////////////////
	// setup groupby settings, create shortcuts
	////////////////////////////////////////////

	CSphGroupSorterSettings tSettings;
	if ( !SetupGroupbySettings ( pQuery, tInSchema, tSettings, sError ) )
		return NULL;

	const bool bGotGroupby = !pQuery->m_sGroupBy.IsEmpty(); // or else, check in SetupGroupbySettings() would already fail
	const bool bGotDistinct = ( tSettings.m_tDistinctLoc.m_iBitOffset>=0 );

	// outgoing schema adds @groupby etc if needed on top of incoming schema
	CSphSchema tOutSchema = tInSchema;

	if ( bGotGroupby && tOutSchema.GetAttrIndex ( "@groupby" )<0 )
	{
		CSphColumnInfo tGroupby ( "@groupby", tSettings.m_pGrouper->GetResultType() );
		CSphColumnInfo tCount ( "@count", SPH_ATTR_INTEGER );
		CSphColumnInfo tDistinct ( "@distinct", SPH_ATTR_INTEGER );

		tOutSchema.AddAttr ( tGroupby );
		tOutSchema.AddAttr ( tCount );
		if ( bGotDistinct )
			tOutSchema.AddAttr ( tDistinct );
	}

	int iGroupby = tOutSchema.GetAttrIndex ( "@groupby" );
	if ( iGroupby>=0 )
	{
		tSettings.m_iPresortRowitems = tOutSchema.GetAttr ( iGroupby ).m_tLocator.CalcRowitem(); // how much raw (non-groupby) rowitems should the sorter expect
		tSettings.m_iAddRowitems = tOutSchema.GetRowSize() - tSettings.m_iPresortRowitems; // how much groupby rowitems the sorter should add for non-grouped matches
		tSettings.m_bDistinct = bGotDistinct;

		tSettings.m_tLocGroupby = tOutSchema.GetAttr ( iGroupby ).m_tLocator;

		int iCount = tOutSchema.GetAttrIndex ( "@count" );
		if ( iCount>=0 )
			tSettings.m_tLocCount = tOutSchema.GetAttr ( iCount ).m_tLocator;

		int iDistinct = tOutSchema.GetAttrIndex ( "@distinct" );
		if ( iDistinct>=0 )
			tSettings.m_tLocDistinct = tOutSchema.GetAttr ( iDistinct ).m_tLocator;
	}

	////////////////////////////////////
	// choose and setup sorting functor
	////////////////////////////////////

	ESphSortFunc eMatchFunc = FUNC_REL_DESC;
	ESphSortFunc eGroupFunc = FUNC_REL_DESC;
	bool bUsesAttrs = false;
	bool bRandomize = false;

	// matches sorting function
	if ( pQuery->m_eSort==SPH_SORT_EXTENDED )
	{
		ESortClauseParseResult eRes = sphParseSortClause ( pQuery->m_sSortBy.cstr(), tInSchema, eMatchFunc, tStateMatch, sError );

		if ( eRes==SORT_CLAUSE_ERROR )
			return NULL;

		if ( eRes==SORT_CLAUSE_RANDOM )
			bRandomize = true;

		for ( int i=0; i<CSphMatchComparatorState::MAX_ATTRS; i++ )
			if ( tStateMatch.m_iAttr[i]>=0 )
				bUsesAttrs = true;

	} else if ( pQuery->m_eSort==SPH_SORT_EXPR )
	{
		tStateMatch.m_iAttr[0] = tInSchema.GetAttrIndex ( "@expr" );
		tStateMatch.m_tLocator[0] = tInSchema.GetAttr ( tStateMatch.m_iAttr[0] ).m_tLocator;
		tStateMatch.m_iAttr[1] = SPH_VATTR_ID;
		tStateMatch.m_uAttrDesc = 1;
		eMatchFunc = FUNC_EXPR;
		bUsesAttrs = true;

	} else
	{
		// check sort-by attribute
		if ( pQuery->m_eSort!=SPH_SORT_RELEVANCE )
		{
			tStateMatch.m_iAttr[0] = tSchema.GetAttrIndex ( pQuery->m_sSortBy.cstr() );
			if ( tStateMatch.m_iAttr[0]<0 )
			{
				sError.SetSprintf ( "sort-by attribute '%s' not found", pQuery->m_sSortBy.cstr() );
				return NULL;
			}

			const CSphColumnInfo & tAttr = tSchema.GetAttr ( tStateMatch.m_iAttr[0] );
			tStateMatch.m_tLocator[0] = tAttr.m_tLocator;
		}

		// find out what function to use and whether it needs attributes
		bUsesAttrs = true;
		switch ( pQuery->m_eSort )
		{
			case SPH_SORT_ATTR_DESC:		eMatchFunc = FUNC_ATTR_DESC; break;
			case SPH_SORT_ATTR_ASC:			eMatchFunc = FUNC_ATTR_ASC; break;
			case SPH_SORT_TIME_SEGMENTS:	eMatchFunc = FUNC_TIMESEGS; break;
			case SPH_SORT_RELEVANCE:		eMatchFunc = FUNC_REL_DESC; bUsesAttrs = false; break;
			default:
				sError.SetSprintf ( "unknown sorting mode %d", pQuery->m_eSort );
				return NULL;
		}
	}

	// groups sorting function
	if ( bGotGroupby )
	{
		ESortClauseParseResult eRes = sphParseSortClause ( pQuery->m_sGroupSortBy.cstr(), tOutSchema, eGroupFunc, tStateGroup, sError );

		if ( eRes==SORT_CLAUSE_ERROR || eRes==SORT_CLAUSE_RANDOM )
		{
			if ( eRes==SORT_CLAUSE_RANDOM )
				sError.SetSprintf ( "groups can not be sorted by @random" );
			return NULL;
		}
	}

	///////////////////
	// spawn the queue
	///////////////////

	bool bMatchBits = tStateMatch.UsesBitfields ();
	bool bGroupBits = tStateGroup.UsesBitfields ();

	if ( !bGotGroupby )
	{
		if ( bMatchBits )
		{
			switch ( eMatchFunc )
			{
				case FUNC_REL_DESC:	pTop = new CSphMatchQueue<MatchRelevanceLt_fn<true> >	( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_ATTR_DESC:pTop = new CSphMatchQueue<MatchAttrLt_fn<true> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_ATTR_ASC:	pTop = new CSphMatchQueue<MatchAttrGt_fn<true> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_TIMESEGS:	pTop = new CSphMatchQueue<MatchTimeSegments_fn<true> >	( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_GENERIC2:	pTop = new CSphMatchQueue<MatchGeneric2_fn<true> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_GENERIC3:	pTop = new CSphMatchQueue<MatchGeneric3_fn<true> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_GENERIC4:	pTop = new CSphMatchQueue<MatchGeneric4_fn<true> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_GENERIC5:	pTop = new CSphMatchQueue<MatchGeneric5_fn<true> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_CUSTOM:	pTop = new CSphMatchQueue<MatchCustom_fn<true> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				default:			pTop = NULL;
			}
		} else
		{
			switch ( eMatchFunc )
			{
				case FUNC_REL_DESC:	pTop = new CSphMatchQueue<MatchRelevanceLt_fn<false> >	( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_ATTR_DESC:pTop = new CSphMatchQueue<MatchAttrLt_fn<false> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_ATTR_ASC:	pTop = new CSphMatchQueue<MatchAttrGt_fn<false> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_TIMESEGS:	pTop = new CSphMatchQueue<MatchTimeSegments_fn<false> >	( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_GENERIC2:	pTop = new CSphMatchQueue<MatchGeneric2_fn<false> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_GENERIC3:	pTop = new CSphMatchQueue<MatchGeneric3_fn<false> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_GENERIC4:	pTop = new CSphMatchQueue<MatchGeneric4_fn<false> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_GENERIC5:	pTop = new CSphMatchQueue<MatchGeneric5_fn<false> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_CUSTOM:	pTop = new CSphMatchQueue<MatchCustom_fn<false> >		( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				case FUNC_EXPR:		pTop = new CSphMatchQueue<MatchExpr_fn>					( pQuery->m_iMaxMatches, bUsesAttrs ); break;
				default:			pTop = NULL;
			}
		}

	} else
	{
		pTop = sphCreateSorter1st ( eMatchFunc, bMatchBits, eGroupFunc, bGroupBits, pQuery, tSettings );
	}

	if ( !pTop )
	{
		sError.SetSprintf ( "internal error: unhandled sorting mode (match-sort=%d, group=%d, group-sort=%d)",
			eMatchFunc, bGotGroupby, eGroupFunc );
		return NULL;
	}

	assert ( pTop );
	pTop->SetState ( tStateMatch );
	pTop->SetGroupState ( tStateGroup );
	pTop->m_bRandomize = bRandomize;
	pTop->m_tIncomingSchema = tInSchema;
	pTop->m_tOutgoingSchema = tOutSchema;
	return pTop;
}


void sphFlattenQueue ( ISphMatchSorter * pQueue, CSphQueryResult * pResult, int iTag )
{
	if ( pQueue && pQueue->GetLength() )
	{
		int iOffset = pResult->m_dMatches.GetLength ();
		pResult->m_dMatches.Resize ( iOffset + pQueue->GetLength() );
		pQueue->Flatten ( &pResult->m_dMatches[iOffset], iTag );
	}
}

//
// $Id: sphinxsort.cpp 1521 2008-10-24 23:12:19Z shodan $
//
