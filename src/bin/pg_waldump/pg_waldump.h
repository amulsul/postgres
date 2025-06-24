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

extern int WalSegSz;

/* Contains the necessary information to drive WAL decoding */
typedef struct XLogDumpPrivate
{
	TimeLineID	timeline;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	bool		endptr_reached;
} XLogDumpPrivate;

#endif		/* end of PG_WALDUMP_H */
