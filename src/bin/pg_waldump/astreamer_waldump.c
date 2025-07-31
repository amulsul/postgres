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
#include "common/file_perm.h"
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
	TimeLineID  timeline;
	XLogSegNo	startSegNo;
	XLogSegNo	endSegNo;
	XLogDumpPrivate *privateInfo;

	/* These fields change with archive member. */
	bool		skipThisSeg;
	bool		writeThisSeg;
	FILE	   *segFp;
	SimpleStringList exportedSegList;	/* Temporary exported segment list */
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
								   char **curFname,
								   XLogSegNo *curSegNo,
								   TimeLineID *curSegTimeline);
static bool member_needs_temp_write(astreamer_waldump *mystreamer,
									const char *fname);

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
		XLogRecPtr	startPtr = endPtr - len;

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

			/* Ensure that we were reading the correct page */
			Assert(recptr >= startPtr && recptr < endPtr);

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

	return (count - nbytes);
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
		XLByteToSeg(startptr, streamer->startSegNo, WalSegSz);

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
				char	   *fname;
				XLogSegNo	segNo;
				TimeLineID	timeline;

				pg_log_debug("pg_waldump: reading \"%s\"", member->pathname);

				mystreamer->skipThisSeg = false;
				mystreamer->writeThisSeg = false;

				if (!member_is_relevant_wal(member,
											mystreamer->timeline,
											mystreamer->startSegNo,
											mystreamer->endSegNo,
											mystreamer->nextSegNo,
											&fname, &segNo, &timeline))
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

				/*
				 * When WAL segments are not archived sequentially, it becomes
				 * necessary to write out (or preserve) segments that might be
				 * required at a later point.
				 */
				if (mystreamer->nextSegNo != segNo &&
					member_needs_temp_write(mystreamer, fname))
				{
					mystreamer->writeThisSeg = true;
					break;
				}

				/*
				 * We are now streaming segment containt.
				 *
				 * We need to track the reading of WAL segment records using a
				 * pointer that's typically incremented by the length of the
				 * data read. However, we sometimes export the WAL file to
				 * temporary storage, allowing the decoding routine to read
				 * directly from there. This makes continuous pointer
				 * incrementing challenging, as file reads can occur from any
				 * offset, leading to potential errors. Therefore, we now
				 * reset the pointer when reading from a file for streaming.
				 * Also, if there's any existing data in the buffer, the next
				 * WAL record should logically follow it.
				 */
#ifdef USE_ASSERT_CHECKING
				Assert(!mystreamer->skipThisSeg);
				Assert(!mystreamer->writeThisSeg);

				if (privateInfo->archive_streamer_buf->len != 0)
				{
					XLogRecPtr	recPtr;

					XLogSegNoOffsetToRecPtr(segNo, 0, WalSegSz, recPtr);
					Assert(privateInfo->archive_streamer_read_ptr == recPtr);
				}
#endif

				/*
				 * Initialized to the beginning of the current segment being
				 * streamed through the buffer.
				 */
				XLogSegNoOffsetToRecPtr(segNo, 0, WalSegSz,
										privateInfo->archive_streamer_read_ptr);

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

			/* Or, write contents to file */
			if (mystreamer->writeThisSeg)
			{
				Assert(mystreamer->segFp != NULL);

				errno = 0;
				if (len > 0 && fwrite(data, len, 1, mystreamer->segFp) != 1)
				{
					char	   *fname;
					int			pathlen = strlen(member->pathname);

					Assert(pathlen >= XLOG_FNAME_LEN);

					fname = member->pathname + (pathlen - XLOG_FNAME_LEN);

					/*
					 * If write didn't set errno, assume problem is no disk
					 * space
					 */
					if (errno == 0)
						errno = ENOSPC;
					pg_fatal("could not write to file \"%s/%s\": %m",
							 privateInfo->tmpdir, fname);
				}
				break;
			}

			/* Or, copy contents to buffer */
			privateInfo->archive_streamer_read_ptr += len;
			astreamer_buffer_bytes(streamer, &data, &len, len);
			break;

		case ASTREAMER_MEMBER_TRAILER:
			if (mystreamer->segFp != NULL)
			{
				fclose(mystreamer->segFp);
				mystreamer->segFp = NULL;
			}
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
	astreamer_waldump *mystreamer;

	Assert(streamer->bbs_next == NULL);

	mystreamer = (astreamer_waldump *) streamer;
	if (mystreamer->segFp != NULL)
		fclose(mystreamer->segFp);

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
					   XLogSegNo nextSegNo, char **curFname,
					   XLogSegNo *curSegNo, TimeLineID *curSegTimeline)
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

	/*
	 * A corner case where we've already streamed the contents of an archived
	 * WAL segment with a similar name, so ignoring this duplicate.
	 */
	if (nextSegNo > segNo)
		return false;

	*curFname = fname;
	*curSegNo = segNo;
	*curSegTimeline = timeline;

	return true;
}

/*
 * Returns true and creates a temporary file if the given WAL segment needs to
 * be written to temporary space. This is required when the segment is not the
 * one currently being decoded. Conversely, if a temporary file for the
 * preceding segment already exists and the current segment is its direct
 * successor, then writing to temporary space is not necessary, and false is
 * returned.
 */
static bool
member_needs_temp_write(astreamer_waldump *mystreamer, const char *fname)
{
	bool		exists;
	XLogSegNo	segNo;
	TimeLineID	timeline;
	XLogDumpPrivate *privateInfo = mystreamer->privateInfo;

	/* Parse position from file */
	XLogFromFileName(fname, &timeline, &segNo, WalSegSz);

	/*
	 * If we find a file that was previously written to the temporary space,
	 * it indicates that the corresponding WAL segment request has already
	 * been fulfilled. In that case, we increment the nextSegNo counter and
	 * check again whether the current segment number matches the required WAL
	 * segment (i.e. nextSegNo). If it does, we allow it to stream normally
	 * through the buffer. Otherwise, we write it to the temporary space, from
	 * where the caller is expected to read it directly.
	 */
	do
	{
		char		segName[MAXFNAMELEN];

		XLogFileName(segName, timeline, mystreamer->nextSegNo, WalSegSz);

		/*
		 * If the WAL segment has already been exported, increment the counter
		 * and check for the next segment.
		 */
		exists = false;
		if (simple_string_list_member(&mystreamer->exportedSegList, segName))
		{
			mystreamer->nextSegNo += 1;
			exists = true;
		}
	} while (exists);

	/*
	 * Need to export this segment to disk; create an empty placeholder file
	 * to be written once its content is received.
	 */
	if (mystreamer->nextSegNo != segNo)
	{
		char		fpath[MAXPGPATH];

		snprintf(fpath, MAXPGPATH, "%s/%s", privateInfo->tmpdir, fname);

		mystreamer->segFp = fopen(fpath, PG_BINARY_W);
		if (mystreamer->segFp == NULL)
			pg_fatal("could not create file \"%s\": %m", fpath);

#ifndef WIN32
		if (chmod(fpath, pg_file_create_mode))
			pg_fatal("could not set permissions on file \"%s\": %m",
					 fpath);
#endif

		/* Record this segment's export to temporary space */
		simple_string_list_append(&mystreamer->exportedSegList, fname);
		return true;
	}

	return false;
}
