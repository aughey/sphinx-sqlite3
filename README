Thanks to Andrew Aksyonoff for providing the initial patch.

The configure scripts do not work right for this yet.  sphinx.h
forces sqlite3 support to be built and the -lsqlite3 link lines
are not added to the Makefile.

To build this, run 

  ./configure --without-mysql

and then edit src/Makefile.  Look for the LIBS line and add -lsqlite3
to the link line.  I realize this is not ideal, but it's work in progress.

