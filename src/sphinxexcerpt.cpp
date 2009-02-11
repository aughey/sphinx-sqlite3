//
// $Id: sphinxexcerpt.cpp 1527 2008-10-30 00:17:12Z xale $
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
#include "sphinxexcerpt.h"
#include "sphinxutils.h"
#include <ctype.h>

/////////////////////////////////////////////////////////////////////////////
// THE EXCERPTS GENERATOR
/////////////////////////////////////////////////////////////////////////////

static const int MAX_HIGHLIGHT_WORDS = 256;

class ExcerptGen_c
{
public:
							ExcerptGen_c ();
							~ExcerptGen_c () {}

	char *					BuildExcerpt ( const ExcerptQuery_t & q, CSphDict * pDict, ISphTokenizer * pTokenizer );

public:
	enum Token_e
	{
		TOK_NONE = 0,		///< unspecified type, also used as the end marker
		TOK_WORD,			///< just a word
		TOK_SPACE,			///< whitespace chars seq
		TOK_BREAK			///< non-word chars seq which delimit a phrase part or boundary
	};

	struct Token_t
	{
		Token_e				m_eType;		///< token type
		int					m_iStart;		///< token start (index in codepoints array)
		int					m_iLengthCP;	///< token length (in codepoints)
		int					m_iLengthBytes;	///< token length (in bytes)
		int					m_iWeight;		///< token weight
		DWORD				m_uWords;		///< matching query words mask
		SphWordID_t			m_iWordID;		///< token word ID from dictionary
	};

	struct Passage_t
	{
		int					m_iStart;		///< start token index
		int					m_iTokens;		///< token count
		int					m_iCodes;		///< codepoints count
		DWORD				m_uWords;		///< matching query words mask
		int					m_iWordsWeight;	///< passage weight factor
		int					m_iWordCount;	///< passage weight factor
		int					m_iMaxLCS;		///< passage weight factor
		int					m_iMinGap;		///< passage weight factor

		void Reset ()
		{
			m_iStart = 0;
			m_iTokens = 0;
			m_iCodes = 0;
			m_uWords = 0;
			m_iWordsWeight = 0;
			m_iWordCount = 0;
			m_iMaxLCS = 0;
			m_iMinGap = 0;
		}

		inline int GetWeight () const
		{
			return m_iWordCount + m_iWordsWeight*m_iMaxLCS + m_iMinGap;
		}
	};

	enum KeywordStar_e
	{
		STAR_NONE	= 0,
		STAR_FRONT	= 1 << 0,
		STAR_BACK	= 1 << 1,
		STAR_BOTH	= STAR_FRONT | STAR_BACK
	};

	struct Keyword_t
	{
		int		m_uStar;
		int		m_iWord;
	};

protected:
	CSphVector<Token_t>		m_dTokens;		///< source text tokens
	CSphVector<Token_t>		m_dWords;		///< query words tokens

	CSphString				m_sBuffer;

	CSphVector<BYTE>		m_dResult;		///< result holder
	int						m_iResultLen;	///< result codepoints count

	CSphVector<Passage_t>	m_dPassages;	///< extracted passages

	bool					m_bExactPhrase;

protected:
	void					CalcPassageWeight ( const CSphVector<int> & dPassage, Passage_t & tPass, int iMaxWords, int iWordCountCoeff );
	bool					ExtractPassages ( const ExcerptQuery_t & q );
	bool					ExtractPhrases ( const ExcerptQuery_t & q );

	void					HighlightPhrase ( const ExcerptQuery_t & q, int iStart, int iEnd );
	void					HighlightAll ( const ExcerptQuery_t & q );
	void					HighlightStart ( const ExcerptQuery_t & q );
	bool					HighlightBestPassages ( const ExcerptQuery_t & q );

	void					ResultEmit ( const char * sLine );
	void					ResultEmit ( const Token_t & sTok );

	void					AddJunk ( int iStart, int iLength );
};

/////////////////////////////////////////////////////////////////////////////

inline bool operator < ( const ExcerptGen_c::Token_t & a, const ExcerptGen_c::Token_t & b )
{
	if ( a.m_iLengthCP==b.m_iLengthCP )
		return a.m_iStart > b.m_iStart;
	return a.m_iLengthCP < b.m_iLengthCP;
}


inline bool operator < ( const ExcerptGen_c::Passage_t & a, const ExcerptGen_c::Passage_t & b )
{
	if ( a.GetWeight()==b.GetWeight() )
		return a.m_iCodes > b.m_iCodes;
	return a.GetWeight() > b.GetWeight();
}


ExcerptGen_c::ExcerptGen_c ()
{
	m_bExactPhrase = false;
}


void ExcerptGen_c::AddJunk ( int iStart, int iLength )
{
	int iChunkStart = iStart;
	
	for ( int i = iStart; i < iStart+iLength; i++ )
		if ( sphIsSpace ( m_sBuffer.cstr () [i] ) != sphIsSpace ( m_sBuffer.cstr () [iChunkStart] ) )
		{
			m_dTokens.Resize ( m_dTokens.GetLength () + 1 );
			Token_t & tLast = m_dTokens.Last ();
			tLast.m_eType   = TOK_SPACE;
			tLast.m_iStart	= iChunkStart;
			tLast.m_iLengthBytes = i - iChunkStart;
			tLast.m_iWordID = 0;
			tLast.m_uWords = 0;

			iChunkStart = i;
		}

	m_dTokens.Resize ( m_dTokens.GetLength () + 1 );
	Token_t & tLast = m_dTokens.Last ();
	tLast.m_eType   = TOK_SPACE;
	tLast.m_iStart	= iChunkStart;
	tLast.m_iLengthBytes = iStart + iLength - iChunkStart;
	tLast.m_iWordID = 0;
	tLast.m_uWords = 0;
}


char * ExcerptGen_c::BuildExcerpt ( const ExcerptQuery_t & q, CSphDict * pDict, ISphTokenizer * pTokenizer )
{
	m_dTokens.Reserve ( 1024 );
	m_sBuffer = q.m_sSource;

	// tokenize query words
	int iWordsLength = strlen ( q.m_sWords.cstr() );

	CSphVector<char> dKwBuffer ( iWordsLength );
	CSphVector<Keyword_t> dKeywords;
	dKeywords.Reserve ( MAX_HIGHLIGHT_WORDS );

	BYTE * sWord;
	int iKwIndex = 0;

	pTokenizer->SetBuffer ( (BYTE*)q.m_sWords.cstr(), iWordsLength );
	while ( ( sWord = pTokenizer->GetToken() ) != NULL )
	{
		SphWordID_t iWord = pDict->GetWordID ( sWord );
		if ( iWord )
		{
			m_dWords.Resize ( m_dWords.GetLength () + 1 );
			Token_t & tLast = m_dWords.Last ();
			tLast.m_eType = TOK_WORD;
			tLast.m_iWordID = iWord;
			tLast.m_iLengthCP = sphUTF8Len ( (const char *)sWord );
			tLast.m_iLengthBytes = strlen ( (const char *)sWord );

			// store keyword
			dKeywords.Resize( dKeywords.GetLength() + 1 );
			Keyword_t & kwLast = dKeywords.Last ();

			// find stars
			bool bStarBack = *pTokenizer->GetTokenEnd() == '*';
			bool bStarFront = ( pTokenizer->GetTokenStart() != pTokenizer->GetBufferPtr() ) &&
				pTokenizer->GetTokenStart()[-1] == '*';
			kwLast.m_uStar = ( bStarFront ? STAR_FRONT : 0 ) | ( bStarBack ? STAR_BACK : 0 );

			// store token
			const int iEndIndex = iKwIndex + tLast.m_iLengthBytes + 1;
			dKwBuffer.Resize ( iEndIndex );
			kwLast.m_iWord = iKwIndex;
			strcpy ( &dKwBuffer [ iKwIndex ], (const char *)sWord );
			iKwIndex = iEndIndex;

			if ( m_dWords.GetLength() == MAX_HIGHLIGHT_WORDS )
				break;
		}
	}

	// tokenize document
	pTokenizer->SetBuffer ( (BYTE*)q.m_sSource.cstr (), strlen ( q.m_sSource.cstr () ) );
	
	const char * pStartPtr = pTokenizer->GetBufferPtr ();
	const char * pLastTokenEnd = pStartPtr;

	while ( ( sWord = pTokenizer->GetToken() ) != NULL )
	{
		const char * pTokenStart = pTokenizer->GetTokenStart ();

		if ( pTokenStart != pStartPtr )
			AddJunk ( pLastTokenEnd - pStartPtr, pTokenStart - pLastTokenEnd );

		if ( pTokenizer->GetBoundary() )
		{
			m_dTokens.Resize ( m_dTokens.GetLength () + 1 );
			Token_t & tLast = m_dTokens.Last ();
			tLast.m_eType   = TOK_BREAK;
			tLast.m_iStart  = 0;
			tLast.m_iLengthBytes = 0;
			tLast.m_iWordID = 0;
			tLast.m_uWords = 0;
		}

		SphWordID_t iWord = pDict->GetWordID ( sWord );

		pLastTokenEnd = pTokenizer->GetTokenEnd ();

		m_dTokens.Resize ( m_dTokens.GetLength () + 1 );
		Token_t & tLast = m_dTokens.Last ();
		tLast.m_eType	= iWord ? TOK_WORD : TOK_SPACE;
		tLast.m_iStart  = pTokenStart - pStartPtr;
		tLast.m_iLengthBytes = pLastTokenEnd - pTokenStart;
		tLast.m_iWordID = iWord;
		tLast.m_uWords = 0;

		// fill word mask
		if ( iWord )
		{
			bool bMatch = false;
			int iOffset;

			ARRAY_FOREACH ( nWord, m_dWords )
			{
				const char * keyword = &dKwBuffer [ dKeywords[nWord].m_iWord ];
				const Token_t & token = m_dWords[nWord];
				
				switch ( dKeywords[nWord].m_uStar )
				{
				case STAR_NONE:
					bMatch = iWord == token.m_iWordID;
					break;

				case STAR_FRONT:					
					iOffset = tLast.m_iLengthBytes - token.m_iLengthBytes;
					bMatch = (iOffset >= 0) &&
						( memcmp( keyword, sWord + iOffset, token.m_iLengthBytes ) == 0 );
					break;

				case STAR_BACK:
					bMatch = ( tLast.m_iLengthBytes >= token.m_iLengthBytes ) &&
						( memcmp( keyword, sWord, token.m_iLengthBytes ) == 0 );
					break;

				case STAR_BOTH:
					bMatch = strstr( (const char *)sWord, keyword ) != NULL;
					break;
				}

				if ( bMatch )
					tLast.m_uWords |= (1UL << nWord);
			}
		}
	}

	// last space if any
	if ( pLastTokenEnd != pTokenizer->GetBufferEnd () )
		AddJunk ( pLastTokenEnd - pStartPtr, pTokenizer->GetBufferEnd () - pLastTokenEnd );

	m_dTokens.Resize ( m_dTokens.GetLength () + 1 );
	Token_t & tLast = m_dTokens.Last ();
	tLast.m_eType   = TOK_NONE;
	tLast.m_iStart  = 0;
	tLast.m_iLengthBytes = 0;
	tLast.m_iWordID = 0;
	tLast.m_uWords = 0;

	// sum token lengths
	int iSourceCodes = 0;
	ARRAY_FOREACH ( i, m_dTokens )
	{
		m_dTokens [i].m_iWeight = 0;

		if ( m_dTokens [i].m_iLengthBytes )
		{
			m_dTokens [i].m_iLengthCP = sphUTF8Len ( m_sBuffer.SubString ( m_dTokens [i].m_iStart, m_dTokens [i].m_iLengthBytes ).cstr () );
			iSourceCodes += m_dTokens [i].m_iLengthCP;
		}
		else
			m_dTokens [i].m_iLengthCP = 0;
	}

	m_bExactPhrase = q.m_bExactPhrase && ( m_dWords.GetLength()>1 );

	// assign word weights
	ARRAY_FOREACH ( i, m_dWords )
		m_dWords[i].m_iWeight = m_dWords[i].m_iLengthCP; // FIXME! should obtain freqs from dict

	// reset result
	m_dResult.Reserve ( 16384 );
	m_dResult.Resize ( 0 );
	m_iResultLen = 0;

	// do highlighting
	if ( q.m_iLimit<=0 || q.m_iLimit>iSourceCodes )
	{
		HighlightAll ( q );

	} else
	{
		if ( !( ExtractPassages ( q ) && HighlightBestPassages ( q ) ) )
			HighlightStart ( q );
	}

	// alloc, fill and return the result
	m_dResult.Add ( 0 );
	char * pRes = new char [ m_dResult.GetLength() ];
	memcpy ( pRes, &m_dResult[0], m_dResult.GetLength() );
	m_dResult.Reset ();

	return pRes;
}


void ExcerptGen_c::HighlightPhrase ( const ExcerptQuery_t & q, int iTok, int iEnd )
{
	while ( iTok<=iEnd )
	{
		while ( iTok<=iEnd && !m_dTokens[iTok].m_uWords )
			ResultEmit ( m_dTokens[iTok++] );
		
		if ( iTok>iEnd )
			break;
		
		bool bMatch = true;
		int iWord = 0;
		int iStart = iTok;
		while ( iWord<m_dWords.GetLength() )
		{
			if ( ( iTok > iEnd ) ||
				 !( m_dTokens[iTok].m_eType==TOK_SPACE || m_dTokens[iTok].m_uWords == ( 1UL<<iWord++ ) ) )
			{
				bMatch = false;
				break;
			}
			iTok++;
		}
		
		if ( !bMatch )
		{
			ResultEmit ( m_dTokens[iStart] );
			iTok = iStart + 1;
			continue;
		}
		
		ResultEmit ( q.m_sBeforeMatch.cstr() );
		while ( iStart<iTok )
			ResultEmit ( m_dTokens [ iStart++ ] );
		ResultEmit ( q.m_sAfterMatch.cstr() );
	}
}


void ExcerptGen_c::HighlightAll ( const ExcerptQuery_t & q )
{
	bool bOpen = false;
	const int iMaxTok = m_dTokens.GetLength()-1; // skip last one, it's TOK_NONE

	if ( m_bExactPhrase )
		HighlightPhrase ( q, 0, iMaxTok-1 );
	else
	{
		// bag of words
		for ( int iTok=0; iTok<iMaxTok; iTok++ )
		{
			if ( ( m_dTokens[iTok].m_uWords!=0 ) ^ bOpen )
			{
				ResultEmit ( bOpen ? q.m_sAfterMatch.cstr() : q.m_sBeforeMatch.cstr() );
				bOpen = !bOpen;
			}
			ResultEmit ( m_dTokens[iTok] );
		}
		if ( bOpen )
			ResultEmit ( q.m_sAfterMatch.cstr() );
	}
}


void ExcerptGen_c::HighlightStart ( const ExcerptQuery_t & q )
{
	// no matches found. just show the starting tokens
	int i = 0;
	while ( m_iResultLen+m_dTokens[i].m_iLengthCP < q.m_iLimit )
	{
		ResultEmit ( m_dTokens[i++] );
		if ( i>=m_dTokens.GetLength() )
			break;
	}
	ResultEmit ( q.m_sChunkSeparator.cstr() );
}

void ExcerptGen_c::ResultEmit ( const char * sLine )
{
	while ( *sLine )
	{
		assert ( (*(BYTE*)sLine)<128 );
		m_dResult.Add ( *sLine++ );
		m_iResultLen++;
	}
}


void ExcerptGen_c::ResultEmit ( const Token_t & sTok )
{
	for ( int i=0; i<sTok.m_iLengthBytes; i++ )
		m_dResult.Add ( m_sBuffer.cstr () [ i+sTok.m_iStart ] );

	m_iResultLen += sTok.m_iLengthCP;
}

/////////////////////////////////////////////////////////////////////////////

void ExcerptGen_c::CalcPassageWeight ( const CSphVector<int> & dPassage, Passage_t & tPass, int iMaxWords, int iWordCountCoeff )
{
	DWORD uLast = 0;
	int iLCS = 1;
	tPass.m_iMaxLCS = 1;

	// calc everything
	tPass.m_uWords = 0;
	tPass.m_iMinGap = iMaxWords-1;

	ARRAY_FOREACH ( i, dPassage )
	{
		Token_t & tTok = m_dTokens[dPassage[i]];
		assert ( tTok.m_eType==TOK_WORD );

		// update mask
		tPass.m_uWords |= tTok.m_uWords;

		// update LCS
		uLast = tTok.m_uWords & ( uLast<<1 );
		if ( uLast )
		{
			iLCS++;
			tPass.m_iMaxLCS = Max ( iLCS, tPass.m_iMaxLCS );
		} else
		{
			iLCS = 1;
			uLast = tTok.m_uWords;
		}

		// update min gap
		if ( tTok.m_uWords )
		{
			tPass.m_iMinGap = Min ( tPass.m_iMinGap, i );
			tPass.m_iMinGap = Min ( tPass.m_iMinGap, dPassage.GetLength()-1-i );
		}
	}
	assert ( tPass.m_iMinGap>=0 );

	// calc final weight
	tPass.m_iWordsWeight = 0;
	tPass.m_iWordCount = 0;

	DWORD uWords = tPass.m_uWords;
	for ( int iWord=0; uWords; uWords>>=1, iWord++ )
		if ( uWords & 1 )
	{
		tPass.m_iWordsWeight += m_dWords[iWord].m_iWeight;
		tPass.m_iWordCount++;
	}

	tPass.m_iMaxLCS *= iMaxWords;
	tPass.m_iWordCount *= iWordCountCoeff;
}


bool ExcerptGen_c::ExtractPassages ( const ExcerptQuery_t & q )
{
	m_dPassages.Reserve ( 256 );
	m_dPassages.Resize ( 0 );

	if ( q.m_bUseBoundaries )
		return ExtractPhrases ( q );

	// my current passage
	CSphVector<int> dPass;
	Passage_t tPass;
	tPass.Reset ();

	int iMaxWords = 2*q.m_iAround+1;
	int iLCSThresh = m_bExactPhrase ? m_dWords.GetLength()*iMaxWords : 0;

	ARRAY_FOREACH ( iTok, m_dTokens )
	{
		// skip starting whitespace
		if ( tPass.m_iTokens==0 && m_dTokens[iTok].m_eType!=TOK_WORD )
		{
			tPass.m_iStart++;
			continue;
		}

		// got token, update passage
		tPass.m_iTokens++;
		tPass.m_iCodes += m_dTokens[iTok].m_iLengthCP;

		if ( m_dTokens[iTok].m_eType!=TOK_WORD )
			continue;

		// got word
		dPass.Add ( iTok );
		tPass.m_uWords |= m_dTokens[iTok].m_uWords;

		// check if we're done
		if ( dPass.GetLength()>=iMaxWords )
			break;
	}

	if ( tPass.m_uWords )
	{
		CalcPassageWeight ( dPass, tPass, iMaxWords, 0 );
		if ( tPass.m_iMaxLCS>=iLCSThresh )
			m_dPassages.Add ( tPass );
	}

	// my lovely update-and-submit loop
	for ( ;; )
	{
		int iAdd = tPass.m_iStart + tPass.m_iTokens;
		if ( iAdd>=m_dTokens.GetLength() )
			break;

		// remove opening word
		assert ( m_dTokens[tPass.m_iStart].m_eType==TOK_WORD );
		tPass.m_iTokens--;
		tPass.m_iCodes -= m_dTokens [ tPass.m_iStart ].m_iLengthCP;
		tPass.m_iStart++;
		dPass.Remove ( 0 ); // FIXME! OPTIMIZE! make this a cyclic buffer

		// remove opening non-words
		while ( m_dTokens[tPass.m_iStart].m_eType!=TOK_WORD )
		{
			tPass.m_iTokens--;
			tPass.m_iCodes -= m_dTokens [ tPass.m_iStart ].m_iLengthCP;
			tPass.m_iStart++;
		}

		// add trailing non-words
		assert ( tPass.m_iStart+tPass.m_iTokens==iAdd );
		while ( iAdd<m_dTokens.GetLength() && m_dTokens[iAdd].m_eType!=TOK_WORD )
		{
			tPass.m_iTokens++;
			tPass.m_iCodes += m_dTokens[iAdd].m_iLengthCP;
			iAdd++;
		}
		if ( iAdd>=m_dTokens.GetLength() )
			break;

		// add trailing word
		assert ( tPass.m_iStart+tPass.m_iTokens==iAdd );
		assert ( m_dTokens[iAdd].m_eType==TOK_WORD );
		tPass.m_iTokens++;
		tPass.m_iCodes += m_dTokens[iAdd].m_iLengthCP;
		dPass.Add ( iAdd );

		// re-weight current passage, and check if it matches
		CalcPassageWeight ( dPass, tPass, iMaxWords, 0 );
		if ( !tPass.m_uWords || tPass.m_iMaxLCS<iLCSThresh )
			continue;

		// if it's the very first one, do add
		if ( !m_dPassages.GetLength() )
		{
			m_dPassages.Add ( tPass );
			continue;
		}

		// check if it's new or better
		Passage_t & tLast = m_dPassages.Last();
		if ( tLast.m_uWords!=tPass.m_uWords
			|| tLast.m_iStart+tLast.m_iTokens-1 < tPass.m_iStart )
		{
			// new
			m_dPassages.Add ( tPass );
		} else
		{
			// better
			if ( tLast.GetWeight()<tPass.GetWeight() )
				tLast = tPass;
		}
	}

	return m_dPassages.GetLength()!=0;
}


bool ExcerptGen_c::ExtractPhrases ( const ExcerptQuery_t & )
{
	int iStart = 0;
	DWORD uWords = 0;

	ARRAY_FOREACH ( iTok, m_dTokens )
	{
		// phrase boundary found, go flush
		if ( m_dTokens[iTok].m_eType==TOK_BREAK || m_dTokens[iTok].m_eType==TOK_NONE )
		{
			// where's my ending token
			int iEnd = iTok;
			if ( m_dTokens[iTok].m_eType==TOK_NONE )
				iEnd--;

			// emit non-empty phrases with matching words as passages
			if ( iStart<iEnd && uWords!=0 )
			{
				Passage_t tPass;
				tPass.Reset ();

				tPass.m_iStart = iStart;
				tPass.m_iTokens = iEnd-iStart+1;

				CSphVector<int> dPass;
				for ( int i=iStart; i<=iEnd; i++ )
				{
					tPass.m_iCodes += m_dTokens[i].m_iLengthCP;
					if ( m_dTokens[i].m_eType==TOK_WORD )
						dPass.Add ( i );
				}

				CalcPassageWeight ( dPass, tPass, 100, 10000 );
				m_dPassages.Add ( tPass );
			}

			// skip until first word or EOF
			while ( m_dTokens[iTok].m_eType!=TOK_WORD && m_dTokens[iTok].m_eType!=TOK_NONE )
				iTok++;
			if ( m_dTokens[iTok].m_eType==TOK_NONE )
				break;

			assert ( m_dTokens[iTok].m_eType==TOK_WORD );
			iStart = iTok;
			uWords = 0;
		}

		// just an incoming token
		if ( m_dTokens[iTok].m_eType==TOK_WORD )
			uWords |= m_dTokens[iTok].m_uWords;
	}

	return m_dPassages.GetLength()!=0;
}


struct PassageOrder_fn
{
	inline bool operator () ( const ExcerptGen_c::Passage_t & a, const ExcerptGen_c::Passage_t & b ) const
	{
		return a.m_iStart < b.m_iStart;
	}
};


bool ExcerptGen_c::HighlightBestPassages ( const ExcerptQuery_t & q )
{
	///////////////////////////
	// select the ones to show
	///////////////////////////

	CSphVector<Passage_t> dShow;
	int iLeft = q.m_iLimit;

	if ( ( q.m_bUseBoundaries || iLeft>0 ) && m_dPassages.GetLength() )
	{
		// initial heapify
		for ( int i=1; i<m_dPassages.GetLength(); i++ )
		{
			// everything upto i-th is heapified; sift up i-th element
			for ( int j=i; j!=0 && ( m_dPassages[j] < m_dPassages[j>>1] ); j=j>>1 )
				Swap ( m_dPassages[j>>1], m_dPassages[j] );
		}

		// best passage extraction loop
		DWORD uNotShown = 1UL << ( m_dWords.GetLength()-1 );
		while ( m_dPassages.GetLength() )
		{
			// this is our hero
			Passage_t & tPass = m_dPassages[0];

			// emit this passage, if we can
			DWORD uShownWords = 0;
			if ( tPass.m_iCodes<=iLeft || q.m_bUseBoundaries )
			{
				// add it to the show
				dShow.Add ( tPass );
				iLeft -= tPass.m_iCodes;
				uShownWords = tPass.m_uWords;

				// sometimes we need only one best one
				if ( q.m_bSinglePassage )
					break;
			}

			// promote tail, retire head
			m_dPassages.RemoveFast ( 0 );

			// sift down former tail
			int iEntry = 0;
			for ( ;; )
			{
				// select child
				int iChild = (iEntry<<1) + 1;
				if ( iChild>=m_dPassages.GetLength() )
					break;

				// select smallest child
				if ( iChild+1<m_dPassages.GetLength() )
					if ( m_dPassages[iChild+1] < m_dPassages[iChild] )
						iChild++;

				// if smallest child is less than entry, exchange and continue
				if (!( m_dPassages[iChild]<m_dPassages[iEntry] ))
					break;
				Swap ( m_dPassages[iChild], m_dPassages[iEntry] );
				iEntry = iChild;
			}

			// we now show some of the query words,
			// so displaying other passages containing those is less significant,
			// so let's update all the other weights (and word masks, to avoid updating twice)
			// and sift up
			if ( uNotShown )
				for ( int i=0; i<m_dPassages.GetLength(); i++ )
			{
				if ( m_dPassages[i].m_uWords & uShownWords )
				{
					// update this passage
					DWORD uWords = uShownWords;
					for ( int iWord=0; uWords; iWord++, uWords>>=1 )
						if ( ( uWords & 1 ) && ( m_dPassages[i].m_uWords & ( 1UL<<iWord ) ) )
							m_dPassages[i].m_iWordsWeight -= m_dWords[iWord].m_iWeight;

					m_dPassages[i].m_uWords &= ~uShownWords;
					assert ( m_dPassages[i].m_iWordsWeight>=0 );
				}

				// every entry above this is both already updated and properly heapified
				// we only need to sift up, but we need to sift up *every* entry
				// because its parent might had been updated this time, breaking heap property
				for ( int j=i; j!=0 && ( m_dPassages[j] < m_dPassages[j>>1] ); j=j>>1 )
					Swap ( m_dPassages[j>>1], m_dPassages[j] );
			}
			uNotShown &= ~uShownWords;
		}
	}

	if ( !dShow.GetLength() )
		return false;

	///////////
	// do show
	///////////

	// sort the passaged in the document order
	if ( !q.m_bWeightOrder )
		dShow.Sort ( PassageOrder_fn() );

	// estimate length, and grow it up to the limit
	int iLast = -1;
	int iLength = 0;
	ARRAY_FOREACH ( i, dShow )
	{
		int iEnd = dShow[i].m_iStart + dShow[i].m_iTokens - 1;
		for ( int iTok = dShow[i].m_iStart; iTok<=iEnd; iTok++ )
			if ( iTok>iLast )
				iLength += m_dTokens[iTok].m_iLengthCP;
		iLast = iEnd;
	}
	if ( iLength<q.m_iLimit && !q.m_bUseBoundaries )
	{
		// word id is no longer needed; we'll use it to store index into dShow
		ARRAY_FOREACH ( i, m_dTokens )
			m_dTokens[i].m_iWordID = 0;

		ARRAY_FOREACH ( i, dShow )
			for ( int iTok = dShow[i].m_iStart; iTok < dShow[i].m_iStart+dShow[i].m_iTokens; iTok++ )
				if ( m_dTokens[iTok].m_iWordID==0 )
					m_dTokens[iTok].m_iWordID = i;

		int iLeft = q.m_iLimit - iLength;
		int iLastLeft = 0;
		while ( iLeft>0 && iLeft!=iLastLeft )
		{
			iLastLeft = iLeft;
			for ( int iShow=0; iShow<dShow.GetLength() && iLeft>0; iShow++ )
			{
				Passage_t & tPass = dShow [ iShow ];

				// the first one
				int iTok = tPass.m_iStart - 1;
				if ( iTok>=0
					&& m_dTokens[iTok].m_iWordID==0
					&& iLeft>=m_dTokens[iTok].m_iLengthCP )
				{
					iLeft -= m_dTokens [ iTok ].m_iLengthCP;
					m_dTokens [ iTok ].m_iWordID = iShow;
					tPass.m_iStart--;
					tPass.m_iTokens++;
				}

				// the last one
				iTok = tPass.m_iStart + tPass.m_iTokens;
				if ( iTok<m_dTokens.GetLength()
					&& m_dTokens[iTok].m_iWordID==0
					&& iLeft>=m_dTokens[iTok].m_iLengthCP )
				{
					iLeft -= m_dTokens [ iTok ].m_iLengthCP;
					m_dTokens [ iTok ].m_iWordID = iShow;
					tPass.m_iTokens++;
				}
			}
		}
	}

	// show everything
	iLast = -1;
	ARRAY_FOREACH ( i, dShow )
	{
		int iTok = dShow[i].m_iStart;
		int iEnd = iTok + dShow[i].m_iTokens - 1;

		if ( iTok>1+iLast || q.m_bWeightOrder )
			ResultEmit ( q.m_sChunkSeparator.cstr() );

		if ( m_bExactPhrase )
		{
			if ( q.m_bWeightOrder )
				iTok = iLast + 1;

			HighlightPhrase ( q, iTok, iEnd );
		}
		else // !m_bExactPhrase
		{
			while ( iTok<=iEnd )
			{
				if ( iTok>iLast || q.m_bWeightOrder )
				{
					if ( m_dTokens[iTok].m_uWords )
					{
						ResultEmit ( q.m_sBeforeMatch.cstr() );
						ResultEmit ( m_dTokens[iTok] );
						ResultEmit ( q.m_sAfterMatch.cstr() );
					}
					else
						ResultEmit ( m_dTokens[iTok] );
				}
				iTok++;
			}
		}

		iLast = iEnd;
	}
	if ( iLast != m_dTokens.GetLength()-1 )
		ResultEmit ( q.m_sChunkSeparator.cstr() );

	return true;
}

/////////////////////////////////////////////////////////////////////////////

char * sphBuildExcerpt ( const ExcerptQuery_t & q, CSphDict * pDict, ISphTokenizer * pTokenizer )
{
	ExcerptGen_c tGen;
	return tGen.BuildExcerpt ( q, pDict, pTokenizer );
}

//
// $Id: sphinxexcerpt.cpp 1527 2008-10-30 00:17:12Z xale $
//
