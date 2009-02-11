//
// $Id: sphinxexcerpt.h 1042 2007-12-31 22:25:36Z shodan $
//

//
// Copyright (c) 2001-2008, Andrew Aksyonoff. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#ifndef _sphinxexcerpt_
#define _sphinxexcerpt_

#include "sphinx.h"

/// a query to generate an excerpt
/// everything string is expected to be UTF-8
struct ExcerptQuery_t
{
	CSphString		m_sSource;			///< source text
	CSphString		m_sWords;			///< words themselves
	CSphString		m_sBeforeMatch;		///< string to insert before each match
	CSphString		m_sAfterMatch;		///< string to insert after each match
	CSphString		m_sChunkSeparator;	///< string to insert between matching chunks (in limited mode only)
	int				m_iLimit;			///< max chars in excerpt (0 if unlimited)
	int				m_iAround;			///< how much words to highlight around each match
	bool			m_bRemoveSpaces;	///< whether to collapse whitespace
	bool			m_bExactPhrase;		///< whether to highlight exact phrase matches only
	bool			m_bSinglePassage;	///< whether to return single best passage only
	bool			m_bUseBoundaries;	///< whether to extract passages by phrase boundaries setup in tokenizer
	bool			m_bWeightOrder;		///< whether to order best passages in document (default) or weight order

public:
	ExcerptQuery_t ()
		: m_sBeforeMatch ( "<b>" )
		, m_sAfterMatch ( "</b>" )
		, m_sChunkSeparator ( " ... " )
		, m_iLimit ( 256 )
		, m_iAround ( 5 )
		, m_bRemoveSpaces ( false )
		, m_bExactPhrase ( false )
		, m_bSinglePassage ( false )
		, m_bUseBoundaries ( false )
		, m_bWeightOrder ( false )
	{
	}
};

/// an excerpt generator
/// returns a newly allocated UTF-8 string
char *				sphBuildExcerpt ( const ExcerptQuery_t & q, CSphDict * pDict, ISphTokenizer * pTokenizer );

#endif // _sphinxexcerpt_

//
// $Id: sphinxexcerpt.h 1042 2007-12-31 22:25:36Z shodan $
//
