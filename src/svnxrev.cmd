@echo off
if exist %1\.svn (
	svn info --xml %1 | perl %1\src\svnxrev.pl %1\src\sphinxversion.h
)
