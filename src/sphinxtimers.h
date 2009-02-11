//
// $Id: sphinxtimers.h 1042 2007-12-31 22:25:36Z shodan $
//

//
// Copyright (c) 2001-2008, Andrew Aksyonoff. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

DECLARE_TIMER ( collect_hits )
DECLARE_TIMER ( sort_hits )
DECLARE_TIMER ( write_hits )
DECLARE_TIMER ( invert_hits )
DECLARE_TIMER ( read_hits )

DECLARE_TIMER ( src_document )
DECLARE_TIMER ( src_sql )
DECLARE_TIMER ( src_xmlpipe )

DECLARE_TIMER ( query_init )
DECLARE_TIMER ( query_load_dir )
DECLARE_TIMER ( query_load_words )
DECLARE_TIMER ( query_match )
DECLARE_TIMER ( query_sort )

DECLARE_TIMER ( debug1 )
DECLARE_TIMER ( debug2 )
DECLARE_TIMER ( debug3 )

//
// $Id: sphinxtimers.h 1042 2007-12-31 22:25:36Z shodan $
//

