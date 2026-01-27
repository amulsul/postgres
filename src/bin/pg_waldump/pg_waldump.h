/*-------------------------------------------------------------------------
 *
 * pg_waldump.h - decode and display WAL
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_waldump/pg_waldump.h
 *-------------------------------------------------------------------------
 */
#ifndef PG_WALDUMP_H
#define PG_WALDUMP_H

#include "access/xlogdefs.h"
#include "fe_utils/astreamer.h"

/* Forward declaration */
struct ArchivedWALFile;
struct ArchivedWAL_hash;

/* Temporary directory */
extern char *TmpWalSegDir;

/* Contains the necessary information to drive WAL decoding */
typedef struct XLogDumpPrivate
{
	TimeLineID	timeline;
	int			segsize;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	bool		endptr_reached;
	bool		decoding_started;

	/* Fields required to read WAL from archive */
	char	   *archive_name;	/* Tar archive name */
	int			archive_fd;		/* File descriptor for the open tar file */

	astreamer  *archive_streamer;

	/* What the archive streamer is currently reading */
	struct ArchivedWALFile *cur_file;

	/*
	 * Hash table of all WAL files that the archive stream has read, including
	 * the one currently in progress.
	 */
	struct ArchivedWAL_hash *archive_wal_htab;

	/*
	 * Although these values can be easily derived from startptr and endptr,
	 * doing so repeatedly for each archived member would be inefficient, as
	 * it would involve recalculating and filtering out irrelevant WAL
	 * segments.
	 */
	XLogSegNo	start_segno;
	XLogSegNo	end_segno;
} XLogDumpPrivate;

extern int	open_file_in_directory(const char *directory, const char *fname);

extern bool is_archive_file(const char *fname,
							pg_compress_algorithm *compression);
extern void init_archive_reader(XLogDumpPrivate *privateInfo,
								const char *waldir, int *WalSegSz,
								pg_compress_algorithm compression);
extern void free_archive_reader(XLogDumpPrivate *privateInfo);
extern int	read_archive_wal_page(XLogDumpPrivate *privateInfo,
								  XLogRecPtr targetPagePtr,
								  Size count, char *readBuff,
								  int WalSegSz);
extern void free_archive_wal_entry(const char *fname,
								   XLogDumpPrivate *privateInfo);

#endif							/* end of PG_WALDUMP_H */
