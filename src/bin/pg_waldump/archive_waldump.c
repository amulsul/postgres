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

/*
 * Check if the start segment number is zero; this indicates a request to read
 * any WAL file.
 */
#define READ_ANY_WAL(privateInfo)	((privateInfo)->start_segno == 0)

/*
 * Hash entry representing a WAL segment retrieved from the archive.
 *
 * While WAL segments are typically read sequentially, individual entries
 * maintain their own buffers for the following reasons:
 *
 * 1. Boundary Handling: The archive streamer provides a continuous byte
 * stream. A single streaming chunk may contain the end of one WAL segment
 * and the start of the next. Separate buffers allow us to easily
 * partition and track these bytes by their respective segments.
 *
 * 2. Out-of-Order Support: Dedicated buffers simplify logic if segments
 * are ever archived or retrieved out of sequence.
 *
 * To minimize the memory footprint, entries and their associated buffers are
 * freed immediately once consumed. Since pg_waldump does not request the same
 * bytes twice, a segment is discarded as soon as it moves past it.
 */
typedef struct ArchivedWALFile
{
	uint32		status;			/* hash status */
	const char *fname;			/* hash key: WAL segment name */

	StringInfo	buf;			/* holds WAL bytes read from archive */

	int			read_len;		/* total bytes of a WAL read from archive */
} ArchivedWALFile;

static uint32 hash_string_pointer(const char *s);
#define SH_PREFIX				ArchivedWAL
#define SH_ELEMENT_TYPE			ArchivedWALFile
#define SH_KEY_TYPE				const char *
#define SH_KEY					fname
#define SH_HASH_KEY(tb, key)	hash_string_pointer(key)
#define SH_EQUAL(tb, a, b)		(strcmp(a, b) == 0)
#define SH_SCOPE				static inline
#define SH_RAW_ALLOCATOR		pg_malloc0
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

typedef struct astreamer_waldump
{
	astreamer	base;
	XLogDumpPrivate *privateInfo;
} astreamer_waldump;

static ArchivedWALFile *get_archive_wal_entry(const char *fname,
											  XLogDumpPrivate *privateInfo,
											  int WalSegSz);
static int	read_archive_file(XLogDumpPrivate *privateInfo, Size count);

static astreamer *astreamer_waldump_new(XLogDumpPrivate *privateInfo);
static void astreamer_waldump_content(astreamer *streamer,
									  astreamer_member *member,
									  const char *data, int len,
									  astreamer_archive_context context);
static void astreamer_waldump_finalize(astreamer *streamer);
static void astreamer_waldump_free(astreamer *streamer);

static bool member_is_wal_file(astreamer_waldump *mystreamer,
							   astreamer_member *member,
							   char **fname);

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

	/* Now, check the compression type of the tar */
	if (fname_len > 4 &&
		strcmp(fname + fname_len - 4, ".tar") == 0)
		*compression = PG_COMPRESSION_NONE;
	else if (fname_len > 4 &&
			 strcmp(fname + fname_len - 4, ".tgz") == 0)
		*compression = PG_COMPRESSION_GZIP;
	else if (fname_len > 7 &&
			 strcmp(fname + fname_len - 7, ".tar.gz") == 0)
		*compression = PG_COMPRESSION_GZIP;
	else if (fname_len > 8 &&
			 strcmp(fname + fname_len - 8, ".tar.lz4") == 0)
		*compression = PG_COMPRESSION_LZ4;
	else if (fname_len > 8 &&
			 strcmp(fname + fname_len - 8, ".tar.zst") == 0)
		*compression = PG_COMPRESSION_ZSTD;
	else
		return false;

	return true;
}

/*
 * Initializes the tar archive reader, creates a hash table for WAL entries,
 * checks for existing valid WAL segments in the archive file and retrieves the
 * segment size, and sets up filters for relevant entries.
 */
void
init_archive_reader(XLogDumpPrivate *privateInfo, const char *waldir,
					int *WalSegSz, pg_compress_algorithm compression)
{
	int			fd;
	astreamer  *streamer;
	ArchivedWALFile *entry = NULL;
	XLogLongPageHeader longhdr;
	XLogSegNo	segno;
	TimeLineID	timeline;

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

	/*
	 * Hash table storing WAL entries read from the archive with an arbitrary
	 * initial size
	 */
	privateInfo->archive_wal_htab = ArchivedWAL_create(8, NULL);

	/*
	 * Verify that the archive contains valid WAL files and fetch WAL segment
	 * size
	 */
	while (entry == NULL || entry->buf->len < XLOG_BLCKSZ)
	{
		if (read_archive_file(privateInfo, XLOG_BLCKSZ) == 0)
			pg_fatal("could not find WAL in archive \"%s\"",
					 privateInfo->archive_name);

		entry = privateInfo->cur_file;
	}

	/* Set WalSegSz if WAL data is successfully read */
	longhdr = (XLogLongPageHeader) entry->buf->data;

	if (!IsValidWalSegSize(longhdr->xlp_seg_size))
	{
		pg_log_error(ngettext("invalid WAL segment size in WAL file from archive \"%s\" (%d byte)",
							  "invalid WAL segment size in WAL file from archive \"%s\" (%d bytes)",
							  longhdr->xlp_seg_size),
					 privateInfo->archive_name, longhdr->xlp_seg_size);
		pg_log_error_detail("The WAL segment size must be a power of two between 1 MB and 1 GB.");
		exit(1);
	}

	*WalSegSz = longhdr->xlp_seg_size;

	/*
	 * With the WAL segment size available, we can now initialize the
	 * dependent start and end segment numbers.
	 */
	Assert(!XLogRecPtrIsInvalid(privateInfo->startptr));
	XLByteToSeg(privateInfo->startptr, privateInfo->start_segno, *WalSegSz);

	if (!XLogRecPtrIsInvalid(privateInfo->endptr))
		XLByteToSeg(privateInfo->endptr, privateInfo->end_segno, *WalSegSz);

	/*
	 * This WAL record was fetched before the filtering parameters
	 * (start_segno and end_segno) were fully initialized. Perform the
	 * relevance check against the user-provided range now; if the WAL falls
	 * outside this range, remove it from the hash table. Subsequent WAL will
	 * be filtered automatically by the archived streamer using the updated
	 * start_segno and end_segno values.
	 */
	XLogFromFileName(entry->fname, &timeline, &segno, privateInfo->segsize);
	if (privateInfo->timeline != timeline ||
		privateInfo->start_segno > segno ||
		privateInfo->end_segno < segno)
		free_archive_wal_entry(entry->fname, privateInfo);
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
					  Size count, char *readBuff, int WalSegSz)
{
	char	   *p = readBuff;
	Size		nbytes = count;
	XLogRecPtr	recptr = targetPagePtr;
	XLogSegNo	segno;
	char		fname[MAXFNAMELEN];
	ArchivedWALFile *entry;

	/* Identify the segment and locate its entry in the archive hash */
	XLByteToSeg(targetPagePtr, segno, WalSegSz);
	XLogFileName(fname, privateInfo->timeline, segno, WalSegSz);
	entry = get_archive_wal_entry(fname, privateInfo, WalSegSz);

	while (nbytes > 0)
	{
		char	   *buf = entry->buf->data;
		int			bufLen = entry->buf->len;
		XLogRecPtr	endPtr;
		XLogRecPtr	startPtr;

		/* Calculate the LSN range currently residing in the buffer */
		XLogSegNoOffsetToRecPtr(segno, entry->read_len, WalSegSz, endPtr);
		startPtr = endPtr - bufLen;

		/*
		 * Copy the requested WAL record if it exists in the buffer.
		 */
		if (bufLen > 0 && startPtr <= recptr && recptr < endPtr)
		{
			int			copyBytes;
			int			offset = recptr - startPtr;

			/*
			 * Given startPtr <= recptr < endPtr and a total buffer size
			 * 'bufLen', the offset (recptr - startPtr) will always be less
			 * than 'bufLen'.
			 */
			Assert(offset < bufLen);

			copyBytes = Min(nbytes, bufLen - offset);
			memcpy(p, buf + offset, copyBytes);

			/* Update state for read */
			recptr += copyBytes;
			nbytes -= copyBytes;
			p += copyBytes;
		}
		else
		{
			/*
			 * Before starting the actual decoding loop, pg_waldump tries to
			 * locate the first valid record from the user-specified start
			 * position, which might not be the start of a WAL record and
			 * could fall in the middle of a record that spans multiple pages.
			 * Consequently, the valid start position the decoder is looking
			 * for could be far away from that initial position.
			 *
			 * This may involve reading across multiple pages, and this
			 * pre-reading fetches data in multiple rounds from the archive
			 * streamer; normally, we would throw away existing buffer
			 * contents to fetch the next set of data, but that existing data
			 * might be needed once the main loop starts. Because previously
			 * read data cannot be re-read by the archive streamer, we delay
			 * resetting the buffer until the main decoding loop is entered.
			 *
			 * Once pg_waldump has entered the main loop, it may re-read the
			 * currently active page, but never an older one; therefore, any
			 * fully consumed WAL data preceding the current page can then be
			 * safely discarded.
			 */
			if (privateInfo->decoding_started)
			{
				resetStringInfo(entry->buf);

				/*
				 * Push back the partial page data for the current page to the
				 * buffer, ensuring it remains full page available for
				 * re-reading if requested.
				 */
				if (p > readBuff)
				{
					Assert((count - nbytes) > 0);
					appendBinaryStringInfo(entry->buf, readBuff, count - nbytes);
				}
			}

			/*
			 * Now, fetch more data; raise an error if it's not the current
			 * segment being read by the archive streamer or if reading of the
			 * archived file has finished.
			 */
			if (privateInfo->cur_file != entry ||
				read_archive_file(privateInfo, READ_CHUNK_SIZE) == 0)
				pg_fatal("could not read file \"%s\" from archive \"%s\": read %lld of %lld",
						 fname, privateInfo->archive_name,
						 (long long int) count - nbytes,
						 (long long int) nbytes);
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
 * Clears the buffer of a WAL entry that is being ignored.  This frees up memory
 * and prevents the accumulation of irrelevant WAL data.  Additionally,
 * conditionally setting cur_file within privateinfo to NULL ensures the
 * archive streamer skips unnecessary copy operations
 */
void
free_archive_wal_entry(const char *fname, XLogDumpPrivate *privateInfo)
{
	ArchivedWALFile *entry;

	entry = ArchivedWAL_lookup(privateInfo->archive_wal_htab, fname);

	if (entry == NULL)
		return;

	/* Destroy the buffer */
	destroyStringInfo(entry->buf);
	entry->buf = NULL;

	/* Set cur_file to NULL if it matches the entry being ignored */
	if (privateInfo->cur_file == entry)
		privateInfo->cur_file = NULL;

	ArchivedWAL_delete_item(privateInfo->archive_wal_htab, entry);
}

/*
 * Returns the archived WAL entry from the hash table if it exists.  Otherwise,
 * it invokes the routine to read the archived file, which then populates the
 * entry in the hash table if that WAL exists in the archive.
 */
static ArchivedWALFile *
get_archive_wal_entry(const char *fname, XLogDumpPrivate *privateInfo,
					  int WalSegSz)
{
	ArchivedWALFile *entry = NULL;

	/* Search hash table */
	entry = ArchivedWAL_lookup(privateInfo->archive_wal_htab, fname);

	if (entry != NULL)
		return entry;

	/*
	 * The requested WAL entry has not been read from the archive yet; invoke
	 * the archive streamer to read it.
	 */
	while (1)
	{
		/* Fetch more data */
		if (read_archive_file(privateInfo, READ_CHUNK_SIZE) == 0)
			break;				/* archive file ended */

		/*
		 * Archived streamer is reading a non-WAL file or an irrelevant WAL
		 * file.
		 */
		if (privateInfo->cur_file == NULL)
			continue;

		entry = privateInfo->cur_file;

		/* Found the required entry */
		if (strcmp(fname, entry->fname) == 0)
			return entry;

		/* WAL segments must be archived in order */
		pg_log_error("WAL files are not archived in sequential order");
		pg_log_error_detail("Expecting segment \"%s\" but found \"%s\".",
							fname, entry->fname);
		exit(1);
	}

	/* Requested WAL segment not found */
	pg_fatal("could not find WAL \"%s\" in archive \"%s\"",
			 fname, privateInfo->archive_name);
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

	buffer = pg_malloc(count * sizeof(uint8));

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
				char	   *fname = NULL;
				ArchivedWALFile *entry;
				bool		found;

				pg_log_debug("reading \"%s\"", member->pathname);

				if (!member_is_wal_file(mystreamer, member, &fname))
					break;

				/*
				 * Further checks are skipped if any WAL file can be read.
				 * This typically occurs during initial verification.
				 */
				if (!READ_ANY_WAL(privateInfo))
				{
					XLogSegNo	segno;
					TimeLineID	timeline;

					/*
					 * Skip the segment if the timeline does not match, if it
					 * falls outside the caller-specified range.
					 */
					XLogFromFileName(fname, &timeline, &segno, privateInfo->segsize);
					if (privateInfo->timeline != timeline ||
						privateInfo->start_segno > segno ||
						privateInfo->end_segno < segno)
					{
						free(fname);
						break;
					}
				}

				entry = ArchivedWAL_insert(privateInfo->archive_wal_htab,
										   fname, &found);

				/*
				 * Shouldn't happen, but if it does, simply ignore the
				 * duplicate WAL file.
				 */
				if (found)
				{
					pg_log_warning("ignoring duplicate WAL \"%s\" found in archive \"%s\"",
								   member->pathname, privateInfo->archive_name);
					break;
				}

				entry->buf = makeStringInfo();
				entry->read_len = 0;
				privateInfo->cur_file = entry;
			}
			break;

		case ASTREAMER_MEMBER_CONTENTS:
			if (privateInfo->cur_file)
			{
				appendBinaryStringInfo(privateInfo->cur_file->buf, data, len);
				privateInfo->cur_file->read_len += len;
			}
			break;

		case ASTREAMER_MEMBER_TRAILER:
			privateInfo->cur_file = NULL;
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
 * successful, it also outputs the WAL segment name.
 */
static bool
member_is_wal_file(astreamer_waldump *mystreamer, astreamer_member *member,
				   char **fname)
{
	int			pathlen;
	char		pathname[MAXPGPATH];
	char	   *filename;

	/* We are only interested in normal files. */
	if (member->is_directory || member->is_link)
		return false;

	if (strlen(member->pathname) < XLOG_FNAME_LEN)
		return false;

	/*
	 * For a correct comparison, we must remove any '.' or '..' components
	 * from the member pathname. Similar to member_verify_header(), we prepend
	 * './' to the path so that canonicalize_path() can properly resolve and
	 * strip these references from the tar member name
	 */
	snprintf(pathname, MAXPGPATH, "./%s", member->pathname);
	canonicalize_path(pathname);
	pathlen = strlen(pathname);

	/* WAL files from the top-level or pg_wal directory will be decoded */
	if (pathlen > XLOG_FNAME_LEN &&
		strncmp(pathname, XLOGDIR, strlen(XLOGDIR)) != 0)
		return false;

	/* WAL file could be with full path */
	filename = pathname + (pathlen - XLOG_FNAME_LEN);
	if (!IsXLogFileName(filename))
		return false;

	*fname = pnstrdup(filename, XLOG_FNAME_LEN);

	return true;
}

/*
 * Helper function for filemap hash table.
 */
static uint32
hash_string_pointer(const char *s)
{
	unsigned char *ss = (unsigned char *) s;

	return hash_bytes(ss, strlen(s));
}
