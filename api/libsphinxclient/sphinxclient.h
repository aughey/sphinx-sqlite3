//
// $Id: sphinxclient.h 1369 2008-07-15 13:30:36Z shodan $
//

//
// Copyright (c) 2008, Andrew Aksyonoff. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU Library General Public License. You should
// have received a copy of the LGPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#ifndef _sphinxclient_
#define _sphinxclient_

#ifdef	__cplusplus
extern "C" {
#endif

/// known searchd status codes
enum
{
	SEARCHD_OK				= 0,
	SEARCHD_ERROR			= 1,
	SEARCHD_RETRY			= 2,
	SEARCHD_WARNING			= 3
};

/// known match modes
enum
{
	SPH_MATCH_ALL			= 0,
	SPH_MATCH_ANY			= 1,
	SPH_MATCH_PHRASE		= 2,
	SPH_MATCH_BOOLEAN		= 3,
	SPH_MATCH_EXTENDED		= 4,
	SPH_MATCH_FULLSCAN		= 5,
	SPH_MATCH_EXTENDED2		= 6
};

/// known ranking modes (ext2 only)
enum
{
	SPH_RANK_PROXIMITY_BM25	= 0,
	SPH_RANK_BM25			= 1,
	SPH_RANK_NONE			= 2,
	SPH_RANK_WORDCOUNT		= 3
};

/// known sort modes
enum
{
	SPH_SORT_RELEVANCE		= 0,
	SPH_SORT_ATTR_DESC		= 1,
	SPH_SORT_ATTR_ASC		= 2,
	SPH_SORT_TIME_SEGMENTS	= 3,
	SPH_SORT_EXTENDED		= 4,
	SPH_SORT_EXPR			= 5
};

/// known filter types
enum
{	SPH_FILTER_VALUES		= 0,
	SPH_FILTER_RANGE		= 1,
	SPH_FILTER_FLOATRANGE	= 2
};

/// known attribute types
enum
{
	SPH_ATTR_INTEGER		= 1,
	SPH_ATTR_TIMESTAMP		= 2,
	SPH_ATTR_ORDINAL		= 3,
	SPH_ATTR_BOOL			= 4,
	SPH_ATTR_FLOAT			= 5,
	SPH_ATTR_MULTI			= 0x40000000UL
};

/// known grouping functions
enum
{	SPH_GROUPBY_DAY			= 0,
	SPH_GROUPBY_WEEK		= 1,
	SPH_GROUPBY_MONTH		= 2,
	SPH_GROUPBY_YEAR		= 3,
	SPH_GROUPBY_ATTR		= 4,
	SPH_GROUPBY_ATTRPAIR	= 5
};

//////////////////////////////////////////////////////////////////////////

#if defined(_MSC_VER)
typedef __int64				sphinx_int64_t;
typedef unsigned __int64	sphinx_uint64_t;
#else // !defined(_MSC_VER)
typedef long long			sphinx_int64_t;
typedef unsigned long long	sphinx_uint64_t;
#endif // !defined(_MSC_VER)


typedef int					sphinx_bool;
#define SPH_TRUE			1
#define SPH_FALSE			0

//////////////////////////////////////////////////////////////////////////

typedef struct st_sphinx_client	sphinx_client;

typedef struct st_sphinx_wordinfo
{
	const char *			word;
	int						docs;
	int						hits;
} sphinx_wordinfo;


typedef struct st_sphinx_result
{
	const char *			error;
	const char *			warning;
	int						status;

	int						num_fields;
	char **					fields;

	int						num_attrs;
	char **					attr_names;
	int *					attr_types;

	int						num_matches;
	void *					values_pool;

	int						total;
	int						total_found;
	int						time_msec;
	int						num_words;
	sphinx_wordinfo *		words;
} sphinx_result;


typedef struct st_sphinx_excerpt_options
{
	const char *			before_match;
	const char *			after_match;
	const char *			chunk_separator;

	int						limit;
	int						around;

	sphinx_bool				exact_phrase;
	sphinx_bool				single_passage;
	sphinx_bool				use_boundaries;
	sphinx_bool				weight_order;
} sphinx_excerpt_options;


typedef struct st_sphinx_keyword_info
{
	char *					tokenized;
	char *					normalized;
	int						num_docs;
	int						num_hits;
} sphinx_keyword_info;

//////////////////////////////////////////////////////////////////////////

sphinx_client *				sphinx_create	( sphinx_bool copy_args );
void						sphinx_destroy	( sphinx_client * client );

const char *				sphinx_error	( sphinx_client * client );
const char *				sphinx_warning	( sphinx_client * client );

sphinx_bool					sphinx_set_server				( sphinx_client * client, const char * host, int port );
sphinx_bool					sphinx_set_connect_timeout		( sphinx_client * client, float seconds );
sphinx_bool					sphinx_set_limits				( sphinx_client * client, int offset, int limit, int max_matches, int cutoff );
sphinx_bool					sphinx_set_max_query_time		( sphinx_client * client, int max_query_time );
sphinx_bool					sphinx_set_match_mode			( sphinx_client * client, int mode );
sphinx_bool					sphinx_set_ranking_mode			( sphinx_client * client, int ranker );
sphinx_bool					sphinx_set_sort_mode			( sphinx_client * client, int mode, const char * sortby );
sphinx_bool					sphinx_set_field_weights		( sphinx_client * client, int num_weights, const char ** field_names, const int * field_weights );
sphinx_bool					sphinx_set_index_weights		( sphinx_client * client, int num_weights, const char ** index_names, const int * index_weights );

sphinx_bool					sphinx_set_id_range				( sphinx_client * client, sphinx_uint64_t minid, sphinx_uint64_t maxid );
sphinx_bool					sphinx_add_filter				( sphinx_client * client, const char * attr, int num_values, const sphinx_uint64_t * values, sphinx_bool exclude );
sphinx_bool					sphinx_add_filter_range			( sphinx_client * client, const char * attr, sphinx_uint64_t umin, sphinx_uint64_t umax, sphinx_bool exclude );
sphinx_bool					sphinx_add_filter_float_range	( sphinx_client * client, const char * attr, float fmin, float fmax, sphinx_bool exclude );
sphinx_bool					sphinx_set_geoanchor			( sphinx_client * client, const char * attr_latitude, const char * attr_longitude, float latitude, float longitude );
sphinx_bool					sphinx_set_groupby				( sphinx_client * client, const char * attr, int groupby_func, const char * group_sort );
sphinx_bool					sphinx_set_groupby_distinct		( sphinx_client * client, const char * attr );
sphinx_bool					sphinx_set_retries				( sphinx_client * client, int count, int delay );

void						sphinx_reset_filters			( sphinx_client * client );
void						sphinx_reset_groupby			( sphinx_client * client );

sphinx_result *				sphinx_query					( sphinx_client * client, const char * query, const char * index_list, const char * comment );
int							sphinx_add_query				( sphinx_client * client, const char * query, const char * index_list, const char * comment );
sphinx_result *				sphinx_run_queries				( sphinx_client * client );

int							sphinx_get_num_results			( sphinx_client * client );
sphinx_uint64_t				sphinx_get_id					( sphinx_result * result, int match );
int							sphinx_get_weight				( sphinx_result * result, int match );
sphinx_uint64_t				sphinx_get_int					( sphinx_result * result, int match, int attr );
float						sphinx_get_float				( sphinx_result * result, int match, int attr );
unsigned int *				sphinx_get_mva					( sphinx_result * result, int match, int attr );

void						sphinx_init_excerpt_options		( sphinx_excerpt_options * opts );
char **						sphinx_build_excerpts			( sphinx_client * client, int num_docs, const char ** docs, const char * index, const char * words, sphinx_excerpt_options * opts );
int							sphinx_update_attributes		( sphinx_client * client, const char * index, int num_attrs, const char ** attrs, int num_docs, const sphinx_uint64_t * docids, const sphinx_uint64_t * values );
sphinx_keyword_info *		sphinx_build_keywords			( sphinx_client * client, const char * query, const char * index, sphinx_bool hits, int * out_num_keywords );

/////////////////////////////////////////////////////////////////////////////

#ifdef	__cplusplus
}
#endif

#endif // _sphinxclient_

//
// $Id: sphinxclient.h 1369 2008-07-15 13:30:36Z shodan $
//
