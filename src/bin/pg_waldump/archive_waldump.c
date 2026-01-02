/*-------------------------------------------------------------------------
 *
 * archive_waldump.c
 *		A generic facility for reading WAL data from tar archives via archive
 *		streamer.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/bin/pg_waldump/archive_waldump.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>

#include "access/xlog_internal.h"
#include "common/hashfn.h"
#include "common/logging.h"
#include "fe_utils/simple_list.h"
#include "pg_waldump.h"

/*
 * How many bytes should we try to read from a file at once?
 */
#define READ_CHUNK_SIZE				(128 * 1024)

/* Hash entry structure for holding WAL segment data read from the archive */
typedef struct ArchivedWALEntry
{
	uint32		status;			/* hash status */
	XLogSegNo	segno;			/* hash key: WAL segment number */
	TimeLineID	timeline;		/* timeline of this wal file */

	StringInfoData buf;
	bool		tmpseg_exists;	/* spill file exists? */

	int			total_read;		/* total read of archived WAL segment */
} ArchivedWALEntry;

#define SH_PREFIX				ArchivedWAL
#define SH_ELEMENT_TYPE			ArchivedWALEntry
#define SH_KEY_TYPE				XLogSegNo
#define SH_KEY					segno
#define SH_HASH_KEY(tb, key)	murmurhash64((uint64) key)
#define SH_EQUAL(tb, a, b)		(a == b)
#define SH_GET_HASH(tb, a)		a->hash
#define SH_SCOPE				static inline
#define SH_RAW_ALLOCATOR		pg_malloc0
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

static ArchivedWAL_hash *ArchivedWAL_HTAB = NULL;

typedef struct astreamer_waldump
{
	astreamer	base;
	XLogDumpPrivate *privateInfo;
} astreamer_waldump;

static int	read_archive_file(XLogDumpPrivate *privateInfo, Size count);
static ArchivedWALEntry *get_archive_wal_entry(XLogSegNo segno,
											   XLogDumpPrivate *privateInfo);

static astreamer *astreamer_waldump_new(XLogDumpPrivate *privateInfo);
static void astreamer_waldump_content(astreamer *streamer,
									  astreamer_member *member,
									  const char *data, int len,
									  astreamer_archive_context context);
static void astreamer_waldump_finalize(astreamer *streamer);
static void astreamer_waldump_free(astreamer *streamer);

static bool member_is_wal_file(astreamer_waldump *mystreamer,
							   astreamer_member *member,
							   XLogSegNo *curSegNo,
							   TimeLineID *curTimeline);

static const astreamer_ops astreamer_waldump_ops = {
	.content = astreamer_waldump_content,
	.finalize = astreamer_waldump_finalize,
	.free = astreamer_waldump_free
};

/*
 * Returns true if the given file is a tar archive and outputs its compression
 * algorithm.
 */
bool
is_archive_file(const char *fname, pg_compress_algorithm *compression)
{
	int			fname_len = strlen(fname);
	pg_compress_algorithm compress_algo;

	/* Now, check the compression type of the tar */
	if (fname_len > 4 &&
		strcmp(fname + fname_len - 4, ".tar") == 0)
		compress_algo = PG_COMPRESSION_NONE;
	else if (fname_len > 4 &&
			 strcmp(fname + fname_len - 4, ".tgz") == 0)
		compress_algo = PG_COMPRESSION_GZIP;
	else if (fname_len > 7 &&
			 strcmp(fname + fname_len - 7, ".tar.gz") == 0)
		compress_algo = PG_COMPRESSION_GZIP;
	else if (fname_len > 8 &&
			 strcmp(fname + fname_len - 8, ".tar.lz4") == 0)
		compress_algo = PG_COMPRESSION_LZ4;
	else if (fname_len > 8 &&
			 strcmp(fname + fname_len - 8, ".tar.zst") == 0)
		compress_algo = PG_COMPRESSION_ZSTD;
	else
		return false;

	*compression = compress_algo;

	return true;
}

/*
 * Initializes the tar archive reader, creates a hash table for WAL entries,
 * checks for existing valid WAL segments in the archive file and retrieves the
 * segment size, and sets up filters for relevant entries.
 */
void
init_archive_reader(XLogDumpPrivate *privateInfo, const char *waldir,
					pg_compress_algorithm compression)
{
	int			fd;
	astreamer  *streamer;
	ArchivedWALEntry *entry = NULL;
	XLogLongPageHeader longhdr;

	/* Open tar archive and store its file descriptor */
	fd = open_file_in_directory(waldir, privateInfo->archive_name);

	if (fd < 0)
		pg_fatal("could not open file \"%s\"", privateInfo->archive_name);

	privateInfo->archive_fd = fd;

	streamer = astreamer_waldump_new(privateInfo);

	/* Before that we must parse the tar archive. */
	streamer = astreamer_tar_parser_new(streamer);

	/* Before that we must decompress, if archive is compressed. */
	if (compression == PG_COMPRESSION_GZIP)
		streamer = astreamer_gzip_decompressor_new(streamer);
	else if (compression == PG_COMPRESSION_LZ4)
		streamer = astreamer_lz4_decompressor_new(streamer);
	else if (compression == PG_COMPRESSION_ZSTD)
		streamer = astreamer_zstd_decompressor_new(streamer);

	privateInfo->archive_streamer = streamer;

	/* Hash table storing WAL entries read from the archive */
	ArchivedWAL_HTAB = ArchivedWAL_create(16, NULL);

	/*
	 * Verify that the archive contains valid WAL files and fetch WAL segment
	 * size
	 */
	while (entry == NULL || entry->buf.len < XLOG_BLCKSZ)
	{
		if (read_archive_file(privateInfo, XLOG_BLCKSZ) == 0)
			pg_fatal("could not find WAL in \"%s\" archive",
					 privateInfo->archive_name);

		entry = privateInfo->cur_wal;
	}

	/* Set WalSegSz if WAL data is successfully read */
	longhdr = (XLogLongPageHeader) entry->buf.data;

	WalSegSz = longhdr->xlp_seg_size;

	if (!IsValidWalSegSize(WalSegSz))
	{
		pg_log_error(ngettext("invalid WAL segment size in WAL file from archive \"%s\" (%d byte)",
							  "invalid WAL segment size in WAL file from archive \"%s\" (%d bytes)",
							  WalSegSz),
					 privateInfo->archive_name, WalSegSz);
		pg_log_error_detail("The WAL segment size must be a power of two between 1 MB and 1 GB.");
		exit(1);
	}

	/*
	 * With the WAL segment size available, we can now initialize the
	 * dependent start and end segment numbers.
	 */
	Assert(!XLogRecPtrIsInvalid(privateInfo->startptr));
	XLByteToSeg(privateInfo->startptr, privateInfo->startSegNo, WalSegSz);

	if (XLogRecPtrIsInvalid(privateInfo->endptr))
		privateInfo->endSegNo = UINT64_MAX;
	else
		XLByteToSeg(privateInfo->endptr, privateInfo->endSegNo, WalSegSz);
}

/*
 * Release the archive streamer chain and close the archive file.
 */
void
free_archive_reader(XLogDumpPrivate *privateInfo)
{
	/*
	 * NB: Normally, astreamer_finalize() is called before astreamer_free() to
	 * flush any remaining buffered data or to ensure the end of the tar
	 * archive is reached. However, when decoding a WAL file, once we hit the
	 * end LSN, any remaining WAL data in the buffer or the tar archive's
	 * unreached end can be safely ignored.
	 */
	astreamer_free(privateInfo->archive_streamer);

	/* Close the file. */
	if (close(privateInfo->archive_fd) != 0)
		pg_log_error("could not close file \"%s\": %m",
					 privateInfo->archive_name);
}

/*
 * Copies WAL data from astreamer to readBuff; if unavailable, fetches more
 * from the tar archive via astreamer.
 */
int
read_archive_wal_page(XLogDumpPrivate *privateInfo, XLogRecPtr targetPagePtr,
					  Size count, char *readBuff)
{
	char	   *p = readBuff;
	Size		nbytes = count;
	XLogRecPtr	recptr = targetPagePtr;
	XLogSegNo	segno;
	ArchivedWALEntry *entry;

	XLByteToSeg(targetPagePtr, segno, WalSegSz);
	entry = get_archive_wal_entry(segno, privateInfo);

	while (nbytes > 0)
	{
		char	   *buf = entry->buf.data;
		int			len = entry->buf.len;

		/* WAL record range that the buffer contains */
		XLogRecPtr	endPtr;
		XLogRecPtr	startPtr;

		XLogSegNoOffsetToRecPtr(entry->segno, entry->total_read,
								WalSegSz, endPtr);
		startPtr = endPtr - len;

		/*
		 * pg_waldump may request to re-read the currently active page, but
		 * never a page older than the current one. Therefore, any fully
		 * consumed WAL data preceding the current page can be safely
		 * discarded.
		 */
		if (recptr >= endPtr)
		{
			/* Discard the buffered data */
			resetStringInfo(&entry->buf);
			len = 0;

			/*
			 * Push back the partial page data for the current page to the
			 * buffer, ensuring it remains full page available for re-reading
			 * if requested.
			 */
			if (p > readBuff)
			{
				Assert((count - nbytes) > 0);
				appendBinaryStringInfo(&entry->buf, readBuff, count - nbytes);
			}
		}

		if (len > 0 && recptr > startPtr)
		{
			int			skipBytes = 0;

			/*
			 * The required offset is not at the start of the buffer, so skip
			 * bytes until reaching the desired offset of the target page.
			 */
			skipBytes = recptr - startPtr;

			buf += skipBytes;
			len -= skipBytes;
		}

		if (len > 0)
		{
			int			readBytes = len >= nbytes ? nbytes : len;

			/* Ensure the reading page is in the buffer */
			Assert(recptr >= startPtr && recptr < endPtr);

			memcpy(p, buf, readBytes);

			/* Update state for read */
			nbytes -= readBytes;
			p += readBytes;
			recptr += readBytes;
		}
		else
		{
			/*
			 * Fetch more data; raise an error if it's not the current segment
			 * being read by the archive streamer or if reading of the
			 * archived file has finished.
			 */
			if (privateInfo->cur_wal != entry ||
				read_archive_file(privateInfo, READ_CHUNK_SIZE) == 0)
			{
				char		fname[MAXFNAMELEN];

				XLogFileName(fname, privateInfo->timeline, entry->segno,
							 WalSegSz);
				pg_fatal("could not read file \"%s\" from archive \"%s\": read %lld of %lld",
						 fname, privateInfo->archive_name,
						 (long long int) count - nbytes,
						 (long long int) nbytes);
			}
		}
	}

	/*
	 * Should have either have successfully read all the requested bytes or
	 * reported a failure before this point.
	 */
	Assert(nbytes == 0);

	/*
	 * NB: We return the fixed value provided as input. Although we could
	 * return a boolean since we either successfully read the WAL page or
	 * raise an error, but the caller expects this value to be returned. The
	 * routine that reads WAL pages from the physical WAL file follows the
	 * same convention.
	 */
	return count;
}

/*
 * Reads the archive file and passes it to the archive streamer for
 * decompression.
 */
static int
read_archive_file(XLogDumpPrivate *privateInfo, Size count)
{
	int			rc;
	char	   *buffer;

	buffer = pg_malloc(READ_CHUNK_SIZE * sizeof(uint8));

	rc = read(privateInfo->archive_fd, buffer, count);
	if (rc < 0)
		pg_fatal("could not read file \"%s\": %m",
				 privateInfo->archive_name);

	/*
	 * Decompress (if required), and then parse the previously read contents
	 * of the tar file.
	 */
	if (rc > 0)
		astreamer_content(privateInfo->archive_streamer, NULL,
						  buffer, rc, ASTREAMER_UNKNOWN);
	pg_free(buffer);

	return rc;
}

/*
 * Returns the archived WAL entry from the hash table if it exists. Otherwise,
 * it invokes the routine to read the archived file, which then populates the
 * entry in the hash table.
 */
static ArchivedWALEntry *
get_archive_wal_entry(XLogSegNo segno, XLogDumpPrivate *privateInfo)
{
	ArchivedWALEntry *entry = NULL;
	char		fname[MAXFNAMELEN];

	/* Search hash table */
	entry = ArchivedWAL_lookup(ArchivedWAL_HTAB, segno);

	if (entry != NULL)
		return entry;

	/* Needed WAL yet to be decoded from archive, do the same */
	while (1)
	{
		entry = privateInfo->cur_wal;

		/* Fetch more data */
		if (read_archive_file(privateInfo, READ_CHUNK_SIZE) == 0)
			break;			/* archive file ended */

		/*
		 * Either, here for the first time, or the archived streamer is
		 * reading a non-WAL file or an irrelevant WAL file.
		 */
		if (entry == NULL)
			continue;

		/* Found the required entry */
		if (entry->segno == segno)
			return entry;

		/*
		 * Ignore if the timeline is different or the current segment is not
		 * the desired one.
		 */
		if (privateInfo->timeline != entry->timeline ||
			privateInfo->startSegNo > entry->segno ||
			privateInfo->endSegNo < entry->segno)
		{
			privateInfo->cur_wal = NULL;
			continue;
		}

		/*
		 * XXX: If the segment being read not the requested one, the data must
		 * be buffered, as we currently lack the mechanism to write it to a
		 * temporary file. This is a known limitation that will be fixed in the
		 * next patch, as the buffer could grow up to the full WAL segment
		 * size.
		 */
		if (segno > entry->segno)
			continue;

		/* WAL segments must be archived in order */
		pg_log_error("WAL files are not archived in sequential order");
		pg_log_error_detail("Expecting segment number " UINT64_FORMAT " but found " UINT64_FORMAT ".",
							segno, entry->segno);
		exit(1);
	}

	/* Requested WAL segment not found */
	XLogFileName(fname, privateInfo->timeline, segno, WalSegSz);
	pg_fatal("could not find file \"%s\" in archive", fname);
}

/*
 * Create an astreamer that can read WAL from a tar file.
 */
static astreamer *
astreamer_waldump_new(XLogDumpPrivate *privateInfo)
{
	astreamer_waldump *streamer;

	streamer = palloc0(sizeof(astreamer_waldump));
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_waldump_ops;

	streamer->privateInfo = privateInfo;

	return &streamer->base;
}

/*
 * Main entry point of the archive streamer for reading WAL data from a tar
 * file. If a member is identified as a valid WAL file, a hash entry is created
 * for it, and its contents are copied into that entry's buffer, making them
 * accessible to the decoding routine.
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
				XLogSegNo	segno;
				TimeLineID	timeline;
				ArchivedWALEntry *entry;
				bool		found;

				pg_log_debug("reading \"%s\"", member->pathname);

				if (!member_is_wal_file(mystreamer, member,
										&segno, &timeline))
					break;

				entry = ArchivedWAL_insert(ArchivedWAL_HTAB, segno, &found);

				/*
				 * Shouldn't happen, but if it does, simply ignore the
				 * duplicate WAL file.
				 */
				if (found)
				{
					pg_log_warning("ignoring duplicate WAL file found in archive: \"%s\"",
								   member->pathname);
					break;
				}

				initStringInfo(&entry->buf);
				entry->timeline = timeline;
				entry->total_read = 0;

				privateInfo->cur_wal = entry;
			}
			break;

		case ASTREAMER_MEMBER_CONTENTS:
			if (privateInfo->cur_wal)
			{
				appendBinaryStringInfo(&privateInfo->cur_wal->buf, data, len);
				privateInfo->cur_wal->total_read += len;
			}
			break;

		case ASTREAMER_MEMBER_TRAILER:
			privateInfo->cur_wal = NULL;
			break;

		case ASTREAMER_ARCHIVE_TRAILER:
			break;

		default:
			/* Shouldn't happen. */
			pg_fatal("unexpected state while parsing tar file");
	}
}

/*
 * End-of-stream processing for an astreamer_waldump stream.
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
	pfree(streamer);
}

/*
 * Returns true if the archive member name matches the WAL naming format. If
 * successful, it also outputs the WAL segment number, and timeline.
 */
static bool
member_is_wal_file(astreamer_waldump *mystreamer, astreamer_member *member,
				   XLogSegNo *curSegNo, TimeLineID *curTimeline)
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

	/*
	 * XXX: On some systems (e.g., OpenBSD), the tar utility includes
	 * PaxHeaders when creating an archive. These are special entries that
	 * store extended metadata for the file entry immediately following them,
	 * and they share the exact same name as that file.
	 */
	if (strstr(member->pathname, "PaxHeaders."))
		return false;

	/* Parse position from file */
	XLogFromFileName(fname, &timeline, &segNo, WalSegSz);

	*curSegNo = segNo;
	*curTimeline = timeline;

	return true;
}
