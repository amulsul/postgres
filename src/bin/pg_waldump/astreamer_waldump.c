/*-------------------------------------------------------------------------
 *
 * astreamer_waldump.c
 *		A generic facility for reading WAL data from tar archives via archive
 *		streamer.
 *
 * Portions Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/bin/pg_waldump/astreamer_waldump.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>

#include "access/xlog_internal.h"
#include "access/xlogdefs.h"
#include "common/logging.h"
#include "fe_utils/simple_list.h"
#include "pg_waldump.h"

/*
 * How many bytes should we try to read from a file at once?
 */
#define READ_CHUNK_SIZE				(128 * 1024)

typedef struct astreamer_waldump
{
	/* These fields don't change once initialized. */
	astreamer	base;
	XLogSegNo	startSegNo;
	XLogSegNo	endSegNo;
	XLogDumpPrivate *privateInfo;

	/* These fields change with archive member. */
	bool		skipThisSeg;
	XLogSegNo	nextSegNo;		/* Next expected segment to stream */
} astreamer_waldump;

static int	astreamer_archive_read(XLogDumpPrivate *privateInfo);
static void astreamer_waldump_content(astreamer *streamer,
									  astreamer_member *member,
									  const char *data, int len,
									  astreamer_archive_context context);
static void astreamer_waldump_finalize(astreamer *streamer);
static void astreamer_waldump_free(astreamer *streamer);

static bool member_is_relevant_wal(astreamer_member *member,
								   TimeLineID startTimeLineID,
								   XLogSegNo startSegNo,
								   XLogSegNo endSegNo,
								   XLogSegNo nextSegNo,
								   XLogSegNo *curSegNo,
								   TimeLineID *curSegTimeline);

static const astreamer_ops astreamer_waldump_ops = {
	.content = astreamer_waldump_content,
	.finalize = astreamer_waldump_finalize,
	.free = astreamer_waldump_free
};

/*
 * Copies WAL data from astreamer to readBuff; if unavailable, fetches more
 * from the tar archive via astreamer.
 */
int
astreamer_wal_read(char *readBuff, XLogRecPtr targetPagePtr, Size count,
				   XLogDumpPrivate *privateInfo)
{
	char	   *p = readBuff;
	Size		nbytes = count;
	XLogRecPtr	recptr = targetPagePtr;
	volatile StringInfo astreamer_buf = privateInfo->archive_streamer_buf;

	while (nbytes > 0)
	{
		char	   *buf = astreamer_buf->data;
		int			len = astreamer_buf->len;

		/* WAL record range that the buffer contains */
		XLogRecPtr	endPtr = privateInfo->archive_streamer_read_ptr;
		XLogRecPtr	startPtr = (endPtr > len) ? endPtr - len : 0;

		/*
		 * Ignore existing data if the required target page has not yet been
		 * read.
		 */
		if (recptr >= endPtr)
		{
			len = 0;

			/* Reset the buffer */
			resetStringInfo(astreamer_buf);
		}

		if (len > 0 && recptr > startPtr)
		{
			int			skipBytes = 0;

			/*
			 * The required offset is not at the start of the archive streamer
			 * buffer, so skip bytes until reaching the desired offset of the
			 * target page.
			 */
			skipBytes = recptr - startPtr;

			buf += skipBytes;
			len -= skipBytes;
		}

		if (len > 0)
		{
			int			readBytes = len >= nbytes ? nbytes : len;

			/*
			 * Ensure we are reading the correct page, unless we've received an
			 * invalid record pointer. In that specific case, it's acceptable
			 * to read any page.
			 */
			Assert(XLogRecPtrIsInvalid(recptr) ||
				   (recptr >= startPtr && recptr < endPtr));

			memcpy(p, buf, readBytes);

			/* Update state for read */
			nbytes -= readBytes;
			p += readBytes;
			recptr += readBytes;
		}
		else
		{
			/* Fetch more data */
			if (astreamer_archive_read(privateInfo) == 0)
				break;			/* No data remaining */
		}
	}

	return (count - nbytes) ? (count - nbytes) : -1;
}

/*
 * Reads the archive and passes it to the archive streamer for decompression.
 */
static int
astreamer_archive_read(XLogDumpPrivate *privateInfo)
{
	int			rc;
	char	   *buffer;

	buffer = pg_malloc(READ_CHUNK_SIZE * sizeof(uint8));

	/* Read more data from the tar file */
	rc = read(privateInfo->archive_fd, buffer, READ_CHUNK_SIZE);
	if (rc < 0)
		pg_fatal("could not read file \"%s\": %m",
				 privateInfo->archive_name);

	/*
	 * Decrypt (if required), and then parse the previously read contents of
	 * the tar file.
	 */
	if (rc > 0)
		astreamer_content(privateInfo->archive_streamer, NULL,
						  buffer, rc, ASTREAMER_UNKNOWN);
	pg_free(buffer);

	return rc;
}

/*
 * Create an astreamer that can read WAL from tar file.
 */
astreamer *
astreamer_waldump_content_new(astreamer *next, XLogRecPtr startptr,
							  XLogRecPtr endPtr, XLogDumpPrivate *privateInfo)
{
	astreamer_waldump *streamer;

	streamer = palloc0(sizeof(astreamer_waldump));
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_waldump_ops;

	streamer->base.bbs_next = next;
	initStringInfo(&streamer->base.bbs_buffer);

	if (XLogRecPtrIsInvalid(startptr))
		streamer->startSegNo = 0;
	else
	{
		XLByteToSeg(startptr, streamer->startSegNo, WalSegSz);

		/*
		 * Initialize the record pointer to the beginning of the first
		 * segment; this pointer will track the WAL record reading status.
		 */
		XLogSegNoOffsetToRecPtr(streamer->startSegNo, 0, WalSegSz,
								privateInfo->archive_streamer_read_ptr);
	}

	if (XLogRecPtrIsInvalid(endPtr))
		streamer->endSegNo = UINT64_MAX;
	else
		XLByteToSeg(endPtr, streamer->endSegNo, WalSegSz);

	streamer->nextSegNo = streamer->startSegNo;
	streamer->privateInfo = privateInfo;

	return &streamer->base;
}

/*
 * Main entry point of the archive streamer for reading WAL from a tar file.
 */
static void
astreamer_waldump_content(astreamer *streamer, astreamer_member *member,
						  const char *data, int len,
						  astreamer_archive_context context)
{
	astreamer_waldump *mystreamer = (astreamer_waldump *) streamer;
	XLogDumpPrivate *privateInfo = mystreamer->privateInfo;

	Assert(context != ASTREAMER_UNKNOWN);

	switch (context)
	{
		case ASTREAMER_MEMBER_HEADER:
			{
				XLogSegNo	segNo;
				TimeLineID	timeline;

				pg_log_debug("pg_waldump: reading \"%s\"", member->pathname);

				mystreamer->skipThisSeg = false;

				if (!member_is_relevant_wal(member,
											privateInfo->timeline,
											mystreamer->startSegNo,
											mystreamer->endSegNo,
											mystreamer->nextSegNo,
											&segNo, &timeline))
				{
					mystreamer->skipThisSeg = true;
					break;
				}

				/*
				 * If nextSegNo is 0, the check is skipped, and any WAL file
				 * can be read -- this typically occurs during initial
				 * verification.
				 */
				if (mystreamer->nextSegNo == 0)
					break;

				/* WAL segments must be archived in order */
				if (mystreamer->nextSegNo != segNo)
				{
					pg_log_error("WAL files are not archived in sequential order");
					pg_log_error_detail("Expecting segment number " UINT64_FORMAT " but found " UINT64_FORMAT ".",
										mystreamer->nextSegNo, segNo);
					exit(1);
				}

				/*
				 * We track the reading of WAL segment records using a pointer
				 * that's continuously incremented by the length of the
				 * received data. This pointer is crucial for serving WAL page
				 * requests from the WAL decoding routine, so it must be
				 * accurate.
				 */
#ifdef USE_ASSERT_CHECKING
				if (mystreamer->nextSegNo != 0)
				{
					XLogRecPtr	recPtr;

					XLogSegNoOffsetToRecPtr(segNo, 0, WalSegSz, recPtr);
					Assert(privateInfo->archive_streamer_read_ptr == recPtr);
				}
#endif

				/* Save the timeline */
				privateInfo->timeline = timeline;

				/* Update the next expected segment number */
				mystreamer->nextSegNo += 1;
			}
			break;

		case ASTREAMER_MEMBER_CONTENTS:
			/* Skip this segment */
			if (mystreamer->skipThisSeg)
				break;

			/* Or, copy contents to buffer */
			privateInfo->archive_streamer_read_ptr += len;
			astreamer_buffer_bytes(streamer, &data, &len, len);
			break;

		case ASTREAMER_MEMBER_TRAILER:
			break;

		case ASTREAMER_ARCHIVE_TRAILER:
			break;

		default:
			/* Shouldn't happen. */
			pg_fatal("unexpected state while parsing tar file");
	}
}

/*
 * End-of-stream processing for a astreamer_waldump stream.
 */
static void
astreamer_waldump_finalize(astreamer *streamer)
{
	Assert(streamer->bbs_next == NULL);
}

/*
 * Free memory associated with a astreamer_waldump stream.
 */
static void
astreamer_waldump_free(astreamer *streamer)
{
	Assert(streamer->bbs_next == NULL);

	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
}

/*
 * Returns true if the archive member name matches the WAL naming format and
 * the corresponding WAL segment falls within the WAL decoding target range;
 * otherwise, returns false.
 */
static bool
member_is_relevant_wal(astreamer_member *member, TimeLineID startTimeLineID,
					   XLogSegNo startSegNo, XLogSegNo endSegNo,
					   XLogSegNo nextSegNo, XLogSegNo *curSegNo,
					   TimeLineID *curSegTimeline)
{
	int			pathlen;
	XLogSegNo	segNo;
	TimeLineID	timeline;
	char	   *fname;

	/* We are only interested in normal files. */
	if (member->is_directory || member->is_link)
		return false;

	pathlen = strlen(member->pathname);
	if (pathlen < XLOG_FNAME_LEN)
		return false;

	/* WAL file could be with full path */
	fname = member->pathname + (pathlen - XLOG_FNAME_LEN);
	if (!IsXLogFileName(fname))
		return false;

	/* Parse position from file */
	XLogFromFileName(fname, &timeline, &segNo, WalSegSz);

	/* Ignore the older timeline */
	if (startTimeLineID > timeline)
		return false;

	/* Skip if the current segment is not the desired one */
	if (startSegNo > segNo || endSegNo < segNo)
		return false;

	*curSegNo = segNo;
	*curSegTimeline = timeline;

	return true;
}
