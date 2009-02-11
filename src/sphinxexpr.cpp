//
// $Id$
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
#include "sphinxexpr.h"
#include <time.h>
#include <math.h>

#if !USE_WINDOWS
#include <unistd.h>
#include <sys/time.h>
#endif

//////////////////////////////////////////////////////////////////////////

#ifndef M_LOG2E
#define M_LOG2E    1.44269504088896340736
#endif

#ifndef M_LOG10E
#define M_LOG10E   0.434294481903251827651
#endif

//////////////////////////////////////////////////////////////////////////
// EVALUATION ENGINE
//////////////////////////////////////////////////////////////////////////

struct Expr_GetInt_c : public ISphExpr
{
	CSphAttrLocator m_tLocator;
	Expr_GetInt_c ( const CSphAttrLocator & tLocator ) : m_tLocator ( tLocator ) {}
	virtual float Eval ( const CSphMatch & tMatch ) const { return (float) tMatch.GetAttr ( m_tLocator ); } // FIXME! OPTIMIZE!!! we can go the short route here
	virtual int IntEval ( const CSphMatch & tMatch ) const { return (int)tMatch.GetAttr ( m_tLocator ); }
	virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return (int64_t)tMatch.GetAttr ( m_tLocator ); }
};


struct Expr_GetBits_c : public ISphExpr
{
	CSphAttrLocator m_tLocator;
	Expr_GetBits_c ( const CSphAttrLocator & tLocator ) : m_tLocator ( tLocator ) {}
	virtual float Eval ( const CSphMatch & tMatch ) const { return (float) tMatch.GetAttr ( m_tLocator ); }
	virtual int IntEval ( const CSphMatch & tMatch ) const { return (int)tMatch.GetAttr ( m_tLocator ); }
	virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return (int64_t)tMatch.GetAttr ( m_tLocator ); }
};


struct Expr_GetFloat_c : public ISphExpr
{
	CSphAttrLocator m_tLocator;
	Expr_GetFloat_c ( const CSphAttrLocator & tLocator ) : m_tLocator ( tLocator ) {}
	virtual float Eval ( const CSphMatch & tMatch ) const { return tMatch.GetAttrFloat ( m_tLocator ); }
};


struct Expr_GetConst_c : public ISphExpr
{
	float m_fValue;
	Expr_GetConst_c ( float fValue ) : m_fValue ( fValue ) {}
	virtual float Eval ( const CSphMatch & ) const { return m_fValue; }
};


struct Expr_GetIntConst_c : public ISphExpr
{
	int m_iValue;
	Expr_GetIntConst_c ( int iValue ) : m_iValue ( iValue ) {}
	virtual float Eval ( const CSphMatch & ) const { return (float) m_iValue; } // no assert() here cause generic float Eval() needs to work even on int-evaluator tree
	virtual int IntEval ( const CSphMatch & ) const { return m_iValue; }
	virtual int64_t Int64Eval ( const CSphMatch & ) const { return m_iValue; }
};


struct Expr_GetInt64Const_c : public ISphExpr
{
	int64_t m_iValue;
	Expr_GetInt64Const_c ( int64_t iValue ) : m_iValue ( iValue ) {}
	virtual float Eval ( const CSphMatch & ) const { return (float) m_iValue; } // no assert() here cause generic float Eval() needs to work even on int-evaluator tree
	virtual int IntEval ( const CSphMatch & ) const { assert ( 0 ); return (int)m_iValue; }
	virtual int64_t Int64Eval ( const CSphMatch & ) const { return m_iValue; }
};


struct Expr_GetId_c : public ISphExpr
{
	virtual float Eval ( const CSphMatch & tMatch ) const { return (float)tMatch.m_iDocID; }
	virtual int IntEval ( const CSphMatch & tMatch ) const { return (int)tMatch.m_iDocID; }
	virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return (int64_t)tMatch.m_iDocID; }
};


struct Expr_GetWeight_c : public ISphExpr
{
	virtual float Eval ( const CSphMatch & tMatch ) const { return (float)tMatch.m_iWeight; }
	virtual int IntEval ( const CSphMatch & tMatch ) const { return (int)tMatch.m_iWeight; }
	virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return (int64_t)tMatch.m_iWeight; }
};

//////////////////////////////////////////////////////////////////////////

struct Expr_Arglist_c : public ISphExpr
{
	CSphVector<ISphExpr *> m_dArgs;

	Expr_Arglist_c ( ISphExpr * pLeft, ISphExpr * pRight )
	{
		AddArgs ( pLeft );
		AddArgs ( pRight );
	}

	~Expr_Arglist_c ()
	{
		ARRAY_FOREACH ( i, m_dArgs )
			SafeRelease ( m_dArgs[i] );
	}

	void AddArgs ( ISphExpr * pExpr )
	{
		// not an arglist? just add it
		if ( !pExpr->IsArglist() )
		{
			m_dArgs.Add ( pExpr );
			return;
		}

		// arglist? take ownership of its args, and dismiss it
		Expr_Arglist_c * pArgs = (Expr_Arglist_c *) pExpr;
		ARRAY_FOREACH ( i, pArgs->m_dArgs )
		{
			m_dArgs.Add ( pArgs->m_dArgs[i] );
			pArgs->m_dArgs[i] = NULL;
		}
		SafeRelease ( pExpr );
	}

	virtual bool IsArglist () const
	{
		return true;
	}

	virtual float Eval ( const CSphMatch & ) const
	{
		assert ( 0 && "internal error: Eval() must not be explicitly called on arglist" );
		return 0.0f;
	}
};

//////////////////////////////////////////////////////////////////////////

#define FIRST	m_pFirst->Eval(tMatch)
#define SECOND	m_pSecond->Eval(tMatch)
#define THIRD	m_pThird->Eval(tMatch)

#define INTFIRST	m_pFirst->IntEval(tMatch)
#define INTSECOND	m_pSecond->IntEval(tMatch)
#define INTTHIRD	m_pThird->IntEval(tMatch)

#define INT64FIRST	m_pFirst->Int64Eval(tMatch)
#define INT64SECOND	m_pSecond->Int64Eval(tMatch)
#define INT64THIRD	m_pThird->Int64Eval(tMatch)

#define DECLARE_UNARY_TRAITS(_classname,_expr) \
	struct _classname : public ISphExpr \
	{ \
		ISphExpr * m_pFirst; \
		_classname ( ISphExpr * pFirst ) : m_pFirst ( pFirst ) {}; \
		~_classname () { SafeRelease ( m_pFirst ); } \
		virtual float Eval ( const CSphMatch & tMatch ) const { return _expr; } \

#define DECLARE_UNARY_FLT(_classname,_expr) \
		DECLARE_UNARY_TRAITS(_classname,_expr) \
	};

#define DECLARE_UNARY_INT(_classname,_expr,_expr2,_expr3) \
		DECLARE_UNARY_TRAITS(_classname,_expr) \
		virtual int IntEval ( const CSphMatch & tMatch ) const { return _expr2; } \
		virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return _expr3; } \
	};

#define IABS(_arg) ( (_arg)>0 ? (_arg) : (-_arg) )

DECLARE_UNARY_INT ( Expr_Neg_c,		-FIRST,			-INTFIRST,		-INT64FIRST )
DECLARE_UNARY_INT ( Expr_Abs_c,		fabs(FIRST),	IABS(INTFIRST),	IABS(INT64FIRST) )
DECLARE_UNARY_FLT ( Expr_Ceil_c,	float(ceil(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Floor_c,	float(floor(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Sin_c,		float(sin(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Cos_c,		float(cos(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Ln_c,		float(log(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Log2_c,	float(log(FIRST)*M_LOG2E) )
DECLARE_UNARY_FLT ( Expr_Log10_c,	float(log(FIRST)*M_LOG10E) )
DECLARE_UNARY_FLT ( Expr_Exp_c,		float(exp(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Sqrt_c,	float(sqrt(FIRST)) )

//////////////////////////////////////////////////////////////////////////

#define DECLARE_BINARY_TRAITS(_classname,_expr) \
	struct _classname : public ISphExpr \
	{ \
		ISphExpr * m_pFirst; \
		ISphExpr * m_pSecond; \
		_classname ( ISphExpr * pFirst, ISphExpr * pSecond ) : m_pFirst ( pFirst ), m_pSecond ( pSecond ) {} \
		~_classname () { SafeRelease ( m_pFirst ); SafeRelease ( m_pSecond ); } \
		virtual float Eval ( const CSphMatch & tMatch ) const { return _expr; } \

#define DECLARE_BINARY_FLT(_classname,_expr) \
		DECLARE_BINARY_TRAITS(_classname,_expr) \
	};

#define DECLARE_BINARY_INT(_classname,_expr,_expr2,_expr3) \
		DECLARE_BINARY_TRAITS(_classname,_expr) \
		virtual int IntEval ( const CSphMatch & tMatch ) const { return _expr2; } \
		virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return _expr3; } \
	};

#define IFFLT(_expr)	( (_expr) ? 1.0f : 0.0f )
#define IFINT(_expr)	( (_expr) ? 1 : 0 )

DECLARE_BINARY_INT ( Expr_Add_c,	FIRST + SECOND,						INTFIRST + INTSECOND,				INT64FIRST + INT64SECOND )
DECLARE_BINARY_INT ( Expr_Sub_c,	FIRST - SECOND,						INTFIRST - INTSECOND,				INT64FIRST - INT64SECOND )
DECLARE_BINARY_INT ( Expr_Mul_c,	FIRST * SECOND,						INTFIRST * INTSECOND,				INT64FIRST * INT64SECOND )
DECLARE_BINARY_FLT ( Expr_Div_c,	FIRST / SECOND )														
DECLARE_BINARY_INT ( Expr_Idiv_c,	(float)(int(FIRST)/int(SECOND)),	INTFIRST / INTSECOND,				INT64FIRST / INT64SECOND )
DECLARE_BINARY_INT ( Expr_Lt_c,		IFFLT ( FIRST < SECOND ),			IFINT ( INTFIRST < INTSECOND ),		IFINT ( INT64FIRST < INT64SECOND ) )
DECLARE_BINARY_INT ( Expr_Gt_c,		IFFLT ( FIRST > SECOND ),			IFINT ( INTFIRST > INTSECOND ),		IFINT ( INT64FIRST > INT64SECOND ) )
DECLARE_BINARY_INT ( Expr_Lte_c,	IFFLT ( FIRST <= SECOND ),			IFINT ( INTFIRST <= INTSECOND ),	IFINT ( INT64FIRST <= INT64SECOND ) )
DECLARE_BINARY_INT ( Expr_Gte_c,	IFFLT ( FIRST >= SECOND ),			IFINT ( INTFIRST >= INTSECOND ),	IFINT ( INT64FIRST >= INT64SECOND ) )
DECLARE_BINARY_INT ( Expr_Eq_c,		IFFLT ( fabs(FIRST-SECOND)<=1e-6 ),	IFINT ( INTFIRST == INTSECOND ),	IFINT ( INT64FIRST == INT64SECOND ) )
DECLARE_BINARY_INT ( Expr_Ne_c,		IFFLT ( fabs(FIRST-SECOND)>1e-6 ),	IFINT ( INTFIRST != INTSECOND ),	IFINT ( INT64FIRST != INT64SECOND ) )
																											
DECLARE_BINARY_INT ( Expr_Min_c,	Min(FIRST,SECOND),					Min(INTFIRST,INTSECOND),			Min(INT64FIRST,INT64SECOND) )
DECLARE_BINARY_INT ( Expr_Max_c,	Max(FIRST,SECOND),					Max(INTFIRST,INTSECOND),			Max(INT64FIRST,INT64SECOND) )
DECLARE_BINARY_FLT ( Expr_Pow_c,	float(pow(FIRST,SECOND)) )

//////////////////////////////////////////////////////////////////////////

#define DECLARE_TERNARY(_classname,_expr,_expr2,_expr3) \
	struct _classname : public ISphExpr \
	{ \
		ISphExpr * m_pFirst; \
		ISphExpr * m_pSecond; \
		ISphExpr * m_pThird; \
		_classname ( ISphExpr * pFirst, ISphExpr * pSecond, ISphExpr * pThird ) : m_pFirst ( pFirst ), m_pSecond ( pSecond ), m_pThird ( pThird ) {} \
		~_classname () { SafeRelease ( m_pFirst ); SafeRelease ( m_pSecond ); SafeRelease ( m_pThird ); } \
		virtual float Eval ( const CSphMatch & tMatch ) const { return _expr; } \
		virtual int IntEval ( const CSphMatch & tMatch ) const { return _expr2; } \
		virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return _expr3; } \
	};

DECLARE_TERNARY ( Expr_If_c,	( FIRST!=0.0f ) ? SECOND : THIRD,	INTFIRST ? INTSECOND : INTTHIRD,	INT64FIRST ? INT64SECOND : INT64THIRD )
DECLARE_TERNARY ( Expr_Madd_c,	FIRST*SECOND+THIRD,					INTFIRST*INTSECOND + INTTHIRD,		INT64FIRST*INT64SECOND + INT64THIRD )
DECLARE_TERNARY ( Expr_Mul3_c,	FIRST*SECOND*THIRD,					INTFIRST*INTSECOND*INTTHIRD,		INT64FIRST*INT64SECOND*INT64THIRD )

//////////////////////////////////////////////////////////////////////////
// PARSER INTERNALS
//////////////////////////////////////////////////////////////////////////

/// known docinfo entries list
enum Docinfo_e
{
	DI_ID,
	DI_WEIGHT
};

#include "sphinxexpryy.hpp"

//////////////////////////////////////////////////////////////////////////

/// known functions
enum Func_e
{
	FUNC_NOW,

	FUNC_ABS,
	FUNC_CEIL,
	FUNC_FLOOR,
	FUNC_SIN,
	FUNC_COS,
	FUNC_LN,
	FUNC_LOG2,
	FUNC_LOG10,
	FUNC_EXP,
	FUNC_SQRT,
	FUNC_BIGINT,

	FUNC_MIN,
	FUNC_MAX,
	FUNC_POW,
	FUNC_IDIV,

	FUNC_IF,
	FUNC_MADD,
	FUNC_MUL3,

	FUNC_INTERVAL,
	FUNC_IN
};


struct FuncDesc_t
{
	const char *	m_sName;
	int				m_iArgs;
	Func_e			m_eFunc;
};


static FuncDesc_t g_dFuncs[] =
{
	{ "now",	0,	FUNC_NOW },

	{ "abs",	1,	FUNC_ABS },
	{ "ceil",	1,	FUNC_CEIL },
	{ "floor",	1,	FUNC_FLOOR },
	{ "sin",	1,	FUNC_SIN },
	{ "cos",	1,	FUNC_COS },
	{ "ln",		1,	FUNC_LN },
	{ "log2",	1,	FUNC_LOG2 },
	{ "log10",	1,	FUNC_LOG10 },
	{ "exp",	1,	FUNC_EXP },
	{ "sqrt",	1,	FUNC_SQRT },
	{ "bigint",	1,	FUNC_BIGINT },	// type-enforcer special as-if-function

	{ "min",	2,	FUNC_MIN },
	{ "max",	2,	FUNC_MAX },
	{ "pow",	2,	FUNC_POW },
	{ "idiv",	2,	FUNC_IDIV },

	{ "if",		3,	FUNC_IF },
	{ "madd",	3,	FUNC_MADD },
	{ "mul3",	3,	FUNC_MUL3 },

	{ "interval",	-2,	FUNC_INTERVAL },
	{ "in",			-2, FUNC_IN }
};

//////////////////////////////////////////////////////////////////////////

/// expression tree node
struct ExprNode_t
{
	int				m_iToken;	///< token type, including operators
	CSphAttrLocator	m_tLocator;	///< attribute locator, for TOK_ATTR type
	union
	{
		int64_t		m_iConst;		///< constant value, for TOK_CONST_INT type
		float		m_fConst;		///< constant value, for TOK_CONST_FLOAT type
		int			m_iFunc;		///< built-in function id, for TOK_FUNC type
		Docinfo_e	m_eDocinfo;		///< docinfo field id, for TOK_DOCINFO type
		int			m_iArgs;		///< args count, for arglist (token==',') type
	};
	int				m_iLeft;
	int				m_iRight;

	ExprNode_t () : m_iToken ( 0 ), m_iLeft ( -1 ), m_iRight ( -1 ) {}
};

/// expression parser
class ExprParser_t
{
	friend int				yylex ( YYSTYPE * lvalp, ExprParser_t * pParser );
	friend int				yyparse ( ExprParser_t * pParser );
	friend void				yyerror ( ExprParser_t * pParser, const char * sMessage );

public:	
							ExprParser_t () {}
							~ExprParser_t () {}

	ISphExpr *				Parse ( const char * sExpr, const CSphSchema & tSchema, DWORD & uAttrType, CSphString & sError );

protected:
	int						m_iParsed;	///< filled by yyparse() at the very end
	CSphString				m_sLexerError;
	CSphString				m_sParserError;

protected:
	int						AddNodeInt ( int64_t iValue );
	int						AddNodeFloat ( float fValue );
	int						AddNodeAttr ( int iTokenType, int iAttrLocator );
	int						AddNodeDocinfo ( Docinfo_e eDocinfo );
	int						AddNodeOp ( int iOp, int iLeft, int iRight );
	int						AddNodeFunc ( int iFunc, int iLeft );

private:
	const char *			m_sExpr;
	const char *			m_pCur;
	const char *			m_pLastTokenStart;
	const CSphSchema *		m_pSchema;
	CSphVector<ExprNode_t>	m_dNodes;

	int						m_iConstNow;

private:
	int						GetToken ( YYSTYPE * lvalp );

	void					GatherArgTypes ( int iNode, CSphVector<int> & dTypes );
	bool					CheckForConstSet ( int iArgsNode );

	DWORD					DeduceType ( int iNode );
	void					Optimize ( int iNode, DWORD uAttrType );

	ISphExpr *				CreateTree ( int iNode, DWORD uAttrType );
	ISphExpr *				CreateIntervalNode ( int iArgsNode, CSphVector<ISphExpr *> & dArgs, DWORD uAttrType );
	ISphExpr *				CreateInNode ( int iArgsNode, CSphVector<ISphExpr *> & dArgs, DWORD uAttrType );
};

//////////////////////////////////////////////////////////////////////////

/// parse that numeric constant
static int ParseNumeric ( YYSTYPE * lvalp, const char ** ppStr )
{
	assert ( lvalp && ppStr && *ppStr );

	// try float route
	char * pEnd = NULL;
	float fRes = (float) strtod ( *ppStr, &pEnd );

	// try int route
	int64_t iRes = 0;
	bool bInt = true;
	for ( const char * p=(*ppStr); p<pEnd; p++ && bInt )
	{
		if ( isdigit(*p) )
			iRes = iRes*10 + (int)( (*p)-'0' ); // FIXME! missing overflow check, missing octal/hex handling
		else
			bInt = false;
	}

	// choose your destiny
	*ppStr = pEnd;
	if ( bInt )
	{
		lvalp->iConst = iRes;
		return TOK_CONST_INT;
	} else
	{
		lvalp->fConst = fRes;
		return TOK_CONST_FLOAT;
	}
}


static bool IsNumericAttrType ( DWORD eType )
{
	return eType==SPH_ATTR_INTEGER
		|| eType==SPH_ATTR_TIMESTAMP
		|| eType==SPH_ATTR_BOOL
		|| eType==SPH_ATTR_FLOAT
		|| eType==SPH_ATTR_BIGINT;
}


/// a lexer of my own
/// returns token id and fills lvalp on success
/// returns -1 and fills sError on failure
int ExprParser_t::GetToken ( YYSTYPE * lvalp )
{
	// skip whitespace, check eof
	while ( isspace(*m_pCur) ) m_pCur++;
	m_pLastTokenStart = m_pCur;
	if ( !*m_pCur ) return 0;

	// check for constant
	if ( isdigit(*m_pCur) )
		return ParseNumeric ( lvalp, &m_pCur );

	// check for field, function, or magic name
	if ( sphIsAttr(m_pCur[0]) 
		|| ( m_pCur[0]=='@' && sphIsAttr(m_pCur[1]) && !isdigit(m_pCur[1]) ) )
	{
		// get token
		const char * pStart = m_pCur++;
		while ( sphIsAttr(*m_pCur) ) m_pCur++;

		CSphString sTok;
		sTok.SetBinary ( pStart, m_pCur-pStart );
		sTok.ToLower ();

		// check for magic name
		if ( sTok=="@id" )		{ lvalp->eDocinfo = DI_ID; return TOK_DOCINFO; }
		if ( sTok=="@weight" )	{ lvalp->eDocinfo = DI_WEIGHT; return TOK_DOCINFO; }

		// check for attribute
		int iAttr = m_pSchema->GetAttrIndex ( sTok.cstr() );
		if ( iAttr>=0 )
		{
			// check attribute type and width
			const CSphColumnInfo & tCol = m_pSchema->GetAttr ( iAttr );
			if ( IsNumericAttrType(tCol.m_eAttrType) )
			{
				lvalp->iAttrLocator = ( tCol.m_tLocator.m_iBitOffset<<16 ) + tCol.m_tLocator.m_iBitCount;

				if ( tCol.m_eAttrType==SPH_ATTR_FLOAT )
					return TOK_ATTR_FLOAT;
				else
					return tCol.m_tLocator.IsBitfield() ? TOK_ATTR_BITS : TOK_ATTR_INT;

			} else
			{
				if ( tCol.m_eAttrType & SPH_ATTR_MULTI )
					m_sLexerError.SetSprintf ( "attribute '%s' is MVA, can not be used in expressions", sTok.cstr() );
				else
					m_sLexerError.SetSprintf ( "attribute '%s' is of unsupported type (type=%d)", sTok.cstr(), tCol.m_eAttrType );
				return -1;
			}
		}

		// check for function
		sTok.ToLower();
		for ( int i=0; i<int(sizeof(g_dFuncs)/sizeof(g_dFuncs[0])); i++ )
			if ( sTok==g_dFuncs[i].m_sName )
		{
			lvalp->iFunc = i;
			return TOK_FUNC;
		}

		m_sLexerError.SetSprintf ( "unknown identifier '%s' (not an attribute, not a function)", sTok.cstr() );
		return -1;
	}

	// check for known operators, then
	switch ( *m_pCur )
	{
		case '+':
		case '-':
		case '*':
		case '/':
		case '(':
		case ')':
		case ',':
			return *m_pCur++;

		case '<':
			m_pCur++;
			if ( *m_pCur=='>' ) { m_pCur++; return TOK_NE; }
			if ( *m_pCur=='=' ) { m_pCur++; return TOK_LTE; }
			return '<';

		case '>':
			m_pCur++;
			if ( *m_pCur=='=' ) { m_pCur++; return TOK_GTE; }
			return '>';

		case '=':
			m_pCur++;
			if ( *m_pCur=='=' ) m_pCur++;
			return TOK_EQ;
	}

	m_sLexerError.SetSprintf ( "unknown operator '%c' near '%s'", *m_pCur, m_pCur );
	return -1;
}

/// is arithmetic?
static inline bool IsAri ( int iTok )
{
	return iTok=='+' || iTok=='-' || iTok=='*' || iTok=='/';
}

/// is constant?
static inline bool IsConst ( int iTok )
{
	return iTok==TOK_CONST_INT || iTok==TOK_CONST_FLOAT;
}

/// optimize subtree
void ExprParser_t::Optimize ( int iNode, DWORD uAttrType )
{
	if ( iNode<0 )
		return;

	Optimize ( m_dNodes[iNode].m_iLeft, uAttrType );
	Optimize ( m_dNodes[iNode].m_iRight, uAttrType );

	ExprNode_t * pRoot = &m_dNodes[iNode];

	// madd, mul3
	if ( ( pRoot->m_iToken=='+' || pRoot->m_iToken=='*' )
		&& ( m_dNodes[pRoot->m_iLeft].m_iToken=='*' || m_dNodes[pRoot->m_iRight].m_iToken=='*' ) )
	{
		if ( m_dNodes[pRoot->m_iLeft].m_iToken!='*' )
			Swap ( pRoot->m_iLeft, pRoot->m_iRight );
		assert ( m_dNodes[pRoot->m_iLeft].m_iToken=='*' );

		m_dNodes.Resize ( m_dNodes.GetLength()+1 );
		pRoot = &m_dNodes[iNode];

		m_dNodes[pRoot->m_iLeft].m_iToken = ',';

		m_dNodes.Last().m_iToken = ',';
		m_dNodes.Last().m_iLeft = pRoot->m_iLeft;
		m_dNodes.Last().m_iRight = pRoot->m_iRight;

		pRoot->m_iFunc = ( pRoot->m_iToken=='+' ) ? FUNC_MADD : FUNC_MUL3;
		pRoot->m_iToken = TOK_FUNC;
		assert ( g_dFuncs[pRoot->m_iFunc].m_eFunc==pRoot->m_iFunc );

		pRoot->m_iLeft = m_dNodes.GetLength()-1;
		pRoot->m_iRight = -1;
		return;
	}

	// constant arithmetic expression
	if ( IsAri ( pRoot->m_iToken ) )
	{
		const ExprNode_t & tLeft = m_dNodes[pRoot->m_iLeft];
		const ExprNode_t & tRight = m_dNodes[pRoot->m_iRight];

		if ( IsConst(tLeft.m_iToken) && IsConst(tRight.m_iToken) )
		{
			if ( tLeft.m_iToken==TOK_CONST_INT && tRight.m_iToken==TOK_CONST_INT && pRoot->m_iToken!='/' )
			{
				switch ( pRoot->m_iToken )
				{
					case '+':	pRoot->m_iConst = tLeft.m_iConst + tRight.m_iConst; break;
					case '-':	pRoot->m_iConst = tLeft.m_iConst - tRight.m_iConst; break;
					case '*':	pRoot->m_iConst = tLeft.m_iConst * tRight.m_iConst; break;
					default:	assert ( 0 && "internal error: unhandled arithmetic token during const-int optimization" );
				}
				pRoot->m_iToken = TOK_CONST_INT; 

			} else
			{
				float fLeft = ( tLeft.m_iToken==TOK_CONST_FLOAT ) ? tLeft.m_fConst : float(tLeft.m_iConst);
				float fRight = ( tRight.m_iToken==TOK_CONST_FLOAT ) ? tRight.m_fConst : float(tRight.m_iConst);
				switch ( pRoot->m_iToken )
				{
					case '+':	pRoot->m_fConst = fLeft + fRight; break;
					case '-':	pRoot->m_fConst = fLeft - fRight; break;
					case '*':	pRoot->m_fConst = fLeft * fRight; break;
					case '/':	pRoot->m_fConst = fLeft / fRight; break;
					default:	assert ( 0 && "internal error: unhandled arithmetic token during const-float optimization" );
				}
				pRoot->m_iToken = TOK_CONST_FLOAT; 
			}
			return;
		}
	}

	// division by a constant (replace with multiplication by inverse)
	if ( pRoot->m_iToken=='/' && m_dNodes[pRoot->m_iRight].m_iToken==TOK_CONST_FLOAT )
	{
		m_dNodes[pRoot->m_iRight].m_fConst = 1.0f / m_dNodes[pRoot->m_iRight].m_fConst;
		pRoot->m_iToken = '*';
		return;
	}

	// unary function from a constant
	if ( pRoot->m_iToken==TOK_FUNC && g_dFuncs[pRoot->m_iFunc].m_iArgs==1 )
	{
		const ExprNode_t & tArg = m_dNodes[pRoot->m_iLeft];
		if ( tArg.m_iToken==TOK_CONST_FLOAT || tArg.m_iToken==TOK_CONST_INT )
		{
			float fArg = tArg.m_iToken==TOK_CONST_FLOAT ? tArg.m_fConst : float(tArg.m_iConst);
			switch ( g_dFuncs[pRoot->m_iFunc].m_eFunc )
			{
				case FUNC_ABS:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = fabs(fArg); break;
				case FUNC_CEIL:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(ceil(fArg)); break;
				case FUNC_FLOOR:	pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(floor(fArg)); break;
				case FUNC_SIN:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(sin(fArg)); break;
				case FUNC_COS:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(cos(fArg)); break;
				case FUNC_LN:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(log(fArg)); break;
				case FUNC_LOG2:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(log(fArg)*M_LOG2E); break;
				case FUNC_LOG10:	pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(log(fArg)*M_LOG10E); break;
				case FUNC_EXP:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(exp(fArg)); break;
				case FUNC_SQRT:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_fConst = float(sqrt(fArg)); break;
				default:			break;
			}
			return;
		}
	}

	// constant function (such as NOW())
	if ( pRoot->m_iToken==TOK_FUNC && pRoot->m_iFunc==FUNC_NOW )
	{
		pRoot->m_iToken = TOK_CONST_INT;
		pRoot->m_iConst = m_iConstNow;
		return;
	}
}

/// fold nodes subtree into opcodes
ISphExpr * ExprParser_t::CreateTree ( int iNode, DWORD uAttrType )
{
	if ( iNode<0 )
		return NULL;

	const ExprNode_t & tNode = m_dNodes[iNode];
	ISphExpr * pLeft = CreateTree ( tNode.m_iLeft, uAttrType );
	ISphExpr * pRight = CreateTree ( tNode.m_iRight, uAttrType );

	switch ( tNode.m_iToken )
	{
		case TOK_ATTR_INT:		return new Expr_GetInt_c ( tNode.m_tLocator );
		case TOK_ATTR_BITS:		return new Expr_GetBits_c ( tNode.m_tLocator );
		case TOK_ATTR_FLOAT:	return new Expr_GetFloat_c ( tNode.m_tLocator );
		case TOK_CONST_FLOAT:	return new Expr_GetConst_c ( tNode.m_fConst );
		case TOK_CONST_INT:
			if ( uAttrType==SPH_ATTR_INTEGER )
				return new Expr_GetIntConst_c ( (int)tNode.m_iConst );
			else if ( uAttrType==SPH_ATTR_BIGINT )
				return new Expr_GetInt64Const_c ( tNode.m_iConst );
			else
				return new Expr_GetConst_c ( float(tNode.m_iConst) );
			break;
		case TOK_DOCINFO:
			switch ( tNode.m_eDocinfo )
			{
				case DI_ID:		return new Expr_GetId_c ();
				case DI_WEIGHT:	return new Expr_GetWeight_c ();
				default:		assert ( 0 && "unhandled docinfo element id" ); break;
			}
			break;

		case '+':				return new Expr_Add_c ( pLeft, pRight ); break;
		case '-':				return new Expr_Sub_c ( pLeft, pRight ); break;
		case '*':				return new Expr_Mul_c ( pLeft, pRight ); break;
		case '/':				return new Expr_Div_c ( pLeft, pRight ); break;
		case '<':				return new Expr_Lt_c ( pLeft, pRight ); break;
		case '>':				return new Expr_Gt_c ( pLeft, pRight ); break;
		case TOK_LTE:			return new Expr_Lte_c ( pLeft, pRight ); break;
		case TOK_GTE:			return new Expr_Gte_c ( pLeft, pRight ); break;
		case TOK_EQ:			return new Expr_Eq_c ( pLeft, pRight ); break;
		case TOK_NE:			return new Expr_Ne_c ( pLeft, pRight ); break;

		case ',':				return new Expr_Arglist_c ( pLeft, pRight ); break;
		case TOK_NEG:			assert ( pRight==NULL ); return new Expr_Neg_c ( pLeft ); break;
		case TOK_FUNC:
			{
				// fold arglist to array
				CSphVector<ISphExpr *> dArgs;
				if ( pLeft && pLeft->IsArglist() )
				{
					assert ( !pRight );
					Expr_Arglist_c * pArgs = (Expr_Arglist_c *) pLeft;

					dArgs = pArgs->m_dArgs;
					pArgs->m_dArgs.Reset ();
					SafeRelease ( pLeft );

				} else
				{
					dArgs.Add ( pLeft );
				}

				// spawn proper function
				assert ( tNode.m_iFunc>=0 && tNode.m_iFunc<int(sizeof(g_dFuncs)/sizeof(g_dFuncs[0])) );
				assert (
					( g_dFuncs[tNode.m_iFunc].m_iArgs>=0 && g_dFuncs[tNode.m_iFunc].m_iArgs==dArgs.GetLength() ) || // arg count matches,
					( g_dFuncs[tNode.m_iFunc].m_iArgs<0 && -g_dFuncs[tNode.m_iFunc].m_iArgs<=dArgs.GetLength() ) ); // or min vararg count reached

				switch ( g_dFuncs[tNode.m_iFunc].m_eFunc )
				{
					case FUNC_NOW:		assert ( 0 ); break; // prevent gcc bitching

					case FUNC_ABS:		return new Expr_Abs_c ( dArgs[0] );
					case FUNC_CEIL:		return new Expr_Ceil_c ( dArgs[0] );
					case FUNC_FLOOR:	return new Expr_Floor_c ( dArgs[0] );
					case FUNC_SIN:		return new Expr_Sin_c ( dArgs[0] );
					case FUNC_COS:		return new Expr_Cos_c ( dArgs[0] );
					case FUNC_LN:		return new Expr_Ln_c ( dArgs[0] );
					case FUNC_LOG2:		return new Expr_Log2_c ( dArgs[0] );
					case FUNC_LOG10:	return new Expr_Log10_c ( dArgs[0] );
					case FUNC_EXP:		return new Expr_Exp_c ( dArgs[0] );
					case FUNC_SQRT:		return new Expr_Sqrt_c ( dArgs[0] );
					case FUNC_BIGINT:	return dArgs[0];

					case FUNC_MIN:		return new Expr_Min_c ( dArgs[0], dArgs[1] );
					case FUNC_MAX:		return new Expr_Max_c ( dArgs[0], dArgs[1] );
					case FUNC_POW:		return new Expr_Pow_c ( dArgs[0], dArgs[1] );
					case FUNC_IDIV:		return new Expr_Idiv_c ( dArgs[0], dArgs[1] );

					case FUNC_IF:		return new Expr_If_c ( dArgs[0], dArgs[1], dArgs[2] );
					case FUNC_MADD:		return new Expr_Madd_c ( dArgs[0], dArgs[1], dArgs[2] );
					case FUNC_MUL3:		return new Expr_Mul3_c ( dArgs[0], dArgs[1], dArgs[2] );

					case FUNC_INTERVAL:	return CreateIntervalNode ( tNode.m_iLeft, dArgs, uAttrType );
					case FUNC_IN:		return CreateInNode ( tNode.m_iLeft, dArgs, uAttrType );
				}
				assert ( 0 && "unhandled function id" );
				break;
			}

		default:				assert ( 0 && "unhandled token type" ); break;
	}

	// fire exit
	SafeRelease ( pLeft );
	SafeRelease ( pRight );
	return NULL;
}

//////////////////////////////////////////////////////////////////////////

/// arg-vs-set function (currently, IN or INTERVAL) evaluator traits
template < typename T >
class Expr_ArgVsSet_c : public ISphExpr
{
protected:
	ISphExpr *			m_pArg;

public:
	Expr_ArgVsSet_c ( ISphExpr * pArg ) : m_pArg ( pArg ) {}
	~Expr_ArgVsSet_c () { SafeRelease ( m_pArg ); }

	virtual int IntEval ( const CSphMatch & tMatch ) const = 0;
	virtual float Eval ( const CSphMatch & tMatch ) const { return (float) IntEval ( tMatch ); }
	virtual int64_t Int64Eval ( const CSphMatch & tMatch ) const { return IntEval ( tMatch ); }

protected:
	T ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const;
};

template<> int Expr_ArgVsSet_c<int>::ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const			{ return pArg->IntEval ( tMatch ); }
template<> float Expr_ArgVsSet_c<float>::ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const		{ return pArg->Eval ( tMatch ); }
template<> int64_t Expr_ArgVsSet_c<int64_t>::ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const	{ return pArg->Int64Eval ( tMatch ); }


/// arg-vs-constant-set
template < typename T >
class Expr_ArgVsConstSet_c : public Expr_ArgVsSet_c<T>
{
protected:
	CSphVector<T> m_dValues;

public:
	/// take ownership of arg, pre-evaluate and dismiss turn points
	Expr_ArgVsConstSet_c ( CSphVector<ISphExpr *> & dArgs )
		: Expr_ArgVsSet_c<T> ( dArgs[0] )
	{
		CSphMatch tDummy;
		for ( int i=1; i<dArgs.GetLength(); i++ )
		{
			m_dValues.Add ( Expr_ArgVsSet_c<T>::ExprEval ( dArgs[i], tDummy ) );
			SafeRelease ( dArgs[i] );
		}
	}
};

//////////////////////////////////////////////////////////////////////////

/// INTERVAL() evaluator for constant turn point values case
template < typename T >
class Expr_IntervalConst_c : public Expr_ArgVsConstSet_c<T>
{
public:
	/// take ownership of arg, pre-evaluate and dismiss turn points
	Expr_IntervalConst_c ( CSphVector<ISphExpr *> & dArgs )
		: Expr_ArgVsConstSet_c<T> ( dArgs )
	{}

	/// evaluate arg, return interval id
	virtual int IntEval ( const CSphMatch & tMatch ) const
	{
		T val = ExprEval ( this->m_pArg, tMatch ); // 'this' fixes gcc braindamage
		ARRAY_FOREACH ( i, this->m_dValues ) // FIXME! OPTIMIZE! perform binary search here
			if ( val<this->m_dValues[i] )
				return i;
		return this->m_dValues.GetLength();
	}
};


/// INTERVAL() evaluator for generic case
template < typename T >
class Expr_Interval_c : public Expr_ArgVsSet_c<T>
{
protected:
	CSphVector<ISphExpr *> m_dTurnPoints;

public:
	/// take ownership of arg and turn points
	Expr_Interval_c ( const CSphVector<ISphExpr *> & dArgs )
		: Expr_ArgVsSet_c<T> ( dArgs[0] )
	{
		for ( int i=1; i<dArgs.GetLength(); i++ )
			m_dTurnPoints.Add ( dArgs[i] );
	}

	/// evaluate arg, return interval id
	virtual int IntEval ( const CSphMatch & tMatch ) const
	{
		T val = ExprEval ( this->m_pArg, tMatch ); // 'this' fixes gcc braindamage
		ARRAY_FOREACH ( i, m_dTurnPoints )
			if ( val < Expr_ArgVsSet_c<T>::ExprEval ( m_dTurnPoints[i], tMatch ) )
				return i;
		return m_dTurnPoints.GetLength();
	}
};

//////////////////////////////////////////////////////////////////////////

/// IN() evaluator for constant values set
template < typename T >
class Expr_In_c : public Expr_ArgVsConstSet_c<T>
{
public:
	/// pre-sort values for binary search
	Expr_In_c ( CSphVector<ISphExpr *> & dArgs ) :
		Expr_ArgVsConstSet_c<T> ( dArgs )
	{
		this->m_dValues.Sort();
	}

	/// evaluate arg, return interval id
	virtual int IntEval ( const CSphMatch & tMatch ) const
	{
		T val = ExprEval ( this->m_pArg, tMatch ); // 'this' fixes gcc braindamage
		return this->m_dValues.BinarySearch ( val )!=NULL;
	}
};

//////////////////////////////////////////////////////////////////////////

void ExprParser_t::GatherArgTypes ( int iNode, CSphVector<int> & dTypes )
{
	if ( iNode<0 )
		return;

	const ExprNode_t & tNode = m_dNodes[iNode];
	if ( tNode.m_iToken==',' )
	{
		GatherArgTypes ( tNode.m_iLeft, dTypes );
		GatherArgTypes ( tNode.m_iRight, dTypes );
	} else
	{
		dTypes.Add ( tNode.m_iToken );
	}
}


bool ExprParser_t::CheckForConstSet ( int iArgsNode )
{
	assert ( m_dNodes[iArgsNode].m_iToken==',' );

	CSphVector<int> dTypes;
	GatherArgTypes ( iArgsNode, dTypes );

	for ( int i=1; i<dTypes.GetLength(); i++ )
		if ( dTypes[i]!=TOK_CONST_INT && dTypes[i]!=TOK_CONST_FLOAT )
			return false;
	return true;
}


ISphExpr * ExprParser_t::CreateIntervalNode ( int iArgsNode, CSphVector<ISphExpr *> & dArgs, DWORD uAttrType )
{
	assert ( dArgs.GetLength()>=2 );

	bool bConst = CheckForConstSet ( iArgsNode );
	if ( bConst )
	{
		switch ( uAttrType )
		{
			case SPH_ATTR_INTEGER:	return new Expr_IntervalConst_c<int> ( dArgs ); break;
			case SPH_ATTR_BIGINT:	return new Expr_IntervalConst_c<int64_t> ( dArgs ); break;
			default:				return new Expr_IntervalConst_c<float> ( dArgs ); break;
		}
	} else
	{
		switch ( uAttrType )
		{
			case SPH_ATTR_INTEGER:	return new Expr_Interval_c<int> ( dArgs ); break;
			case SPH_ATTR_BIGINT:	return new Expr_Interval_c<int64_t> ( dArgs ); break;
			default:				return new Expr_Interval_c<float> ( dArgs ); break;
		}
	}
}


ISphExpr * ExprParser_t::CreateInNode ( int iArgsNode, CSphVector<ISphExpr *> & dArgs, DWORD uAttrType )
{
	assert ( dArgs.GetLength()>=2 );
	assert ( CheckForConstSet(iArgsNode) );

	// spawn that evaluator
	switch ( uAttrType )
	{
		case SPH_ATTR_INTEGER:	return new Expr_In_c<int> ( dArgs ); break;
		case SPH_ATTR_BIGINT:	return new Expr_In_c<int64_t> ( dArgs ); break;
		default:				return new Expr_In_c<float> ( dArgs ); break;
	}
}

//////////////////////////////////////////////////////////////////////////

int yylex ( YYSTYPE * lvalp, ExprParser_t * pParser )
{
	return pParser->GetToken ( lvalp );
}

void yyerror ( ExprParser_t * pParser, const char * sMessage )
{
	pParser->m_sParserError.SetSprintf ( "%s near '%s'", sMessage, pParser->m_pLastTokenStart );
}

#if USE_WINDOWS
#pragma warning(push,1)
#endif

#include "sphinxexpryy.cpp"

#if USE_WINDOWS
#pragma warning(pop)
#endif

//////////////////////////////////////////////////////////////////////////

int ExprParser_t::AddNodeInt ( int64_t iValue )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_CONST_INT;
	tNode.m_iConst = iValue;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeFloat ( float fValue )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_CONST_FLOAT;
	tNode.m_fConst = fValue;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeAttr ( int iTokenType, int iAttrLocator )
{
	assert ( iTokenType==TOK_ATTR_INT || iTokenType==TOK_ATTR_BITS || iTokenType==TOK_ATTR_FLOAT );
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = iTokenType;
	tNode.m_tLocator.m_iBitOffset = iAttrLocator>>16;
	tNode.m_tLocator.m_iBitCount = iAttrLocator&0xffff;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeDocinfo ( Docinfo_e eDocinfo )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_DOCINFO;
	tNode.m_eDocinfo = eDocinfo;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeOp ( int iOp, int iLeft, int iRight )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = iOp;
	tNode.m_iArgs = 0;
	if ( iOp==',' )
	{
		if ( iLeft>=0 )		tNode.m_iArgs += ( m_dNodes[iLeft].m_iToken==',' ) ? m_dNodes[iLeft].m_iArgs : 1;
		if ( iRight>=0 )	tNode.m_iArgs += ( m_dNodes[iRight].m_iToken==',' ) ? m_dNodes[iRight].m_iArgs : 1;
	}
	tNode.m_iLeft = iLeft;
	tNode.m_iRight = iRight;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeFunc ( int iFunc, int iArgsNode )
{
	// check args count
	assert ( iFunc>=0 && iFunc<int(sizeof(g_dFuncs)/sizeof(g_dFuncs[0])) );
	int iExpectedArgc = g_dFuncs[iFunc].m_iArgs;
	int iArgc = 0;
	if ( iArgsNode>=0 )
		iArgc = ( m_dNodes[iArgsNode].m_iToken==',' ) ? m_dNodes[iArgsNode].m_iArgs : 1;
	if ( iExpectedArgc<0 )
	{
		if ( iArgc<-iExpectedArgc )
		{
			m_sParserError.SetSprintf ( "%s() called with %d args, at least %d args expected", g_dFuncs[iFunc].m_sName, iArgc, -iExpectedArgc );
			return -1;
		}
	} else if ( iArgc!=iExpectedArgc )
	{
		m_sParserError.SetSprintf ( "%s() called with %d args, %d args expected", g_dFuncs[iFunc].m_sName, iArgc, iExpectedArgc );
		return -1;
	}

	// check for set of constants, if needed
	if ( iFunc==FUNC_IN && !CheckForConstSet ( iArgsNode ) )
	{
		m_sParserError.SetSprintf ( "%s() requires constant 2nd and further args", g_dFuncs[iFunc].m_sName );
		return -1;
	}

	// do add
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_FUNC;
	tNode.m_iFunc = iFunc;
	tNode.m_iLeft = iArgsNode;
	return m_dNodes.GetLength()-1;
}


DWORD ExprParser_t::DeduceType ( int iNode )
{
	const ExprNode_t & tNode = m_dNodes[iNode];
	switch ( tNode.m_iToken )
	{
		// variables with unconditionally integer result
		case TOK_ATTR_INT:
		case TOK_ATTR_BITS:
			if ( tNode.m_tLocator.m_iBitCount>32 )
				return SPH_ATTR_BIGINT;
			else
				return SPH_ATTR_INTEGER;

		case TOK_CONST_INT:	
			if ( tNode.m_iConst>=(int64_t)INT_MIN && tNode.m_iConst<=(int64_t)INT_MAX )
				return SPH_ATTR_INTEGER;
			else
				return SPH_ATTR_BIGINT;

		case TOK_DOCINFO:
			return USE_64BIT ? SPH_ATTR_BIGINT : SPH_ATTR_INTEGER;

		// binary ops with unconditionally float result
		case TOK_ATTR_FLOAT:
		case TOK_CONST_FLOAT:
		case '/':
			return SPH_ATTR_FLOAT;

		// unary ops that can be evaluated in both modes
		case TOK_NEG:
			return DeduceType ( tNode.m_iLeft );

		// binary ops that can be evaluated in both modes
		case TOK_LTE:
		case TOK_GTE:
		case TOK_EQ:
		case TOK_NE:
		case '<':
		case '>':
		case '+':
		case '-':
		case '*':
		case ',':
			{
				DWORD iLeftType = DeduceType ( tNode.m_iLeft );
				DWORD iRightType = DeduceType ( tNode.m_iRight );

				if ( iLeftType==SPH_ATTR_INTEGER && iRightType==SPH_ATTR_INTEGER )
				{
					// both types are int32? can compute in int32
					return SPH_ATTR_INTEGER;
				} else if (
					( iLeftType==SPH_ATTR_INTEGER || iLeftType==SPH_ATTR_BIGINT ) &&
					( iRightType==SPH_ATTR_INTEGER || iRightType==SPH_ATTR_BIGINT ) )
				{
					// both types are int32 or int64, but not both int32? can compute in int64
					return SPH_ATTR_BIGINT;
				} else
				{
					// there must had been a float floating around
					return SPH_ATTR_FLOAT;
				}
			}

		// function calls
		case TOK_FUNC:
			{
				assert ( tNode.m_iFunc>=0 && tNode.m_iFunc<int(sizeof(g_dFuncs)/sizeof(g_dFuncs[0])) );
				switch ( g_dFuncs[tNode.m_iFunc].m_eFunc )
				{
					case FUNC_NOW:
						return SPH_ATTR_INTEGER;

					case FUNC_MIN:
					case FUNC_MAX:
					case FUNC_IF:
					case FUNC_MADD:
					case FUNC_MUL3:
					case FUNC_IDIV:
					case FUNC_ABS:
					case FUNC_INTERVAL:
					case FUNC_IN:
						return DeduceType ( tNode.m_iLeft );

					case FUNC_BIGINT:
					{
						DWORD uArgType = DeduceType ( tNode.m_iLeft );
						if ( uArgType==SPH_ATTR_FLOAT )
							return uArgType; // FIXME! silently ignores BIGINT() on floats; should warn or raise an error
						else
							return SPH_ATTR_BIGINT;
					}

					default:
						return SPH_ATTR_FLOAT; // by default, functions are either on floats and/or return floats
				}
			}

		// unknown tokens
		default:
			return SPH_ATTR_FLOAT;
	}
}


ISphExpr * ExprParser_t::Parse ( const char * sExpr, const CSphSchema & tSchema, DWORD & uAttrType, CSphString & sError )
{
	// setup lexer
	m_sExpr = sExpr;
	m_pCur = sExpr;
	m_pSchema = &tSchema;

	// setup constant functions
	m_iConstNow = (int) time ( NULL );

	// build tree
	m_iParsed = -1;
	yyparse ( this );

	// handle errors
	if ( m_iParsed<0 || !m_sLexerError.IsEmpty() || !m_sParserError.IsEmpty() )
	{
		sError = !m_sLexerError.IsEmpty() ? m_sLexerError : m_sParserError;
		if ( sError.IsEmpty() ) sError = "general parsing error";
		return NULL;
	}

	// deduce return type
	uAttrType = DeduceType ( m_iParsed );

	// perform optimizations
	Optimize ( m_iParsed, uAttrType );

	// create evaluator
	ISphExpr * pRes = CreateTree ( m_iParsed, uAttrType );
	if ( !pRes )
		sError.SetSprintf ( "empty expression" );
	return pRes;
}

//////////////////////////////////////////////////////////////////////////
// PUBLIC STUFF
//////////////////////////////////////////////////////////////////////////

/// parser entry point
ISphExpr * sphExprParse ( const char * sExpr, const CSphSchema & tSchema, DWORD * pAttrType, CSphString & sError )
{
	// parse into opcodes
	DWORD uTmp;
	ExprParser_t tParser;
	return tParser.Parse ( sExpr, tSchema, pAttrType ? (*pAttrType) : uTmp, sError );
}

//
// $Id$
//
