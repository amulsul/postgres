/*-------------------------------------------------------------------------
 *
 * pg_waldump.h - decode and display WAL
 *
 * Copyright (c) 2013-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_waldump/pg_waldump.h
 *-------------------------------------------------------------------------
 */
#ifndef PG_WALDUMP_H
#define PG_WALDUMP_H

#include "access/xlogdefs.h"
#include "fe_utils/astreamer.h"
#include "fe_utils/simple_list.h"
#include "lib/stringinfo.h"

#define TEMP_FILE_EXT	"waldump.tmp"

extern int	WalSegSz;
extern char *TmpWalSegDir;
extern SimpleStringList TmpWalSegList;

/* Contains the necessary information to drive WAL decoding */
typedef struct XLogDumpPrivate
{
	TimeLineID	timeline;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	bool		endptr_reached;

	/* Fields required to read WAL from archive */
	char	   *archive_name;	/* Tar archive name */
	int			archive_fd;		/* File descriptor for the open tar file */

	astreamer  *archive_streamer;
	StringInfo	archive_streamer_buf;	/* Buffer for receiving WAL data */
	XLogRecPtr	archive_streamer_read_ptr;	/* Populate the buffer with
											 * records until this record
											 * pointer */
	XLogSegNo	curSegNo;		/* Current segment being read */
} XLogDumpPrivate;

/*
 * Generate the temporary WAL file path.
 *
 * Note that the caller is responsible to pfree it.
 */
static inline char *
get_tmp_wal_file_path(const char *fname)
{
	char	   *fpath = (char *) palloc(MAXPGPATH);

	snprintf(fpath, MAXPGPATH, "%s/%s.%s", TmpWalSegDir, fname,
			 TEMP_FILE_EXT);

	return fpath;
}

extern astreamer *astreamer_waldump_new(XLogRecPtr startptr, XLogRecPtr endptr,
										XLogDumpPrivate *privateInfo);
extern int astreamer_wal_read(char *readBuff, XLogRecPtr startptr, Size count,
							  XLogDumpPrivate *privateInfo);
extern int astreamer_archive_read(XLogDumpPrivate *privateInfo);

#endif							/* end of PG_WALDUMP_H */
