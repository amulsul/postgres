/*-------------------------------------------------------------------------
 *
 * pg_waldump.c - decode and display WAL
 *
 * Copyright (c) 2013-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_waldump/pg_waldump.c
 *-------------------------------------------------------------------------
 */

#define FRONTEND 1
#include "postgres.h"

#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "access/xlogstats.h"
#include "common/fe_memutils.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "common/relpath.h"
#include "fe_utils/astreamer.h"
#include "getopt_long.h"
#include "pg_waldump.h"
#include "rmgrdesc.h"
#include "storage/bufpage.h"

/*
 * NOTE: For any code change or issue fix here, it is highly recommended to
 * give a thought about doing the same in pg_walinspect contrib module as well.
 */

static const char *progname;

int			WalSegSz;
static volatile sig_atomic_t time_to_stop = false;

static const RelFileLocator emptyRelFileLocator = {0, 0, 0};

typedef struct XLogDumpConfig
{
	/* display options */
	bool		quiet;
	bool		bkp_details;
	int			stop_after_records;
	int			already_displayed_records;
	bool		follow;
	bool		stats;
	bool		stats_per_record;

	/* filter options */
	bool		filter_by_rmgr[RM_MAX_ID + 1];
	bool		filter_by_rmgr_enabled;
	TransactionId filter_by_xid;
	bool		filter_by_xid_enabled;
	RelFileLocator filter_by_relation;
	bool		filter_by_extended;
	bool		filter_by_relation_enabled;
	BlockNumber filter_by_relation_block;
	bool		filter_by_relation_block_enabled;
	ForkNumber	filter_by_relation_forknum;
	bool		filter_by_fpw;

	/* save options */
	char	   *save_fullpage_path;
} XLogDumpConfig;


/*
 * When sigint is called, just tell the system to exit at the next possible
 * moment.
 */
#ifndef WIN32

static void
sigint_handler(SIGNAL_ARGS)
{
	time_to_stop = true;
}
#endif

static void
print_rmgr_list(void)
{
	int			i;

	for (i = 0; i <= RM_MAX_BUILTIN_ID; i++)
	{
		printf("%s\n", GetRmgrDesc(i)->rm_name);
	}
}

/*
 * Check whether directory exists and whether we can open it. Keep errno set so
 * that the caller can report errors somewhat more accurately.
 */
static bool
verify_directory(const char *directory)
{
	DIR		   *dir = opendir(directory);

	if (dir == NULL)
		return false;
	closedir(dir);
	return true;
}

/*
 * Create the directory if it doesn't exist. Report an error if creation fails
 * or if an existing directory is not empty.
 */
static void
create_directory(char *path)
{
	int			ret;

	switch ((ret = pg_check_dir(path)))
	{
		case 0:
			/* Does not exist, so create it */
			if (pg_mkdir_p(path, pg_dir_create_mode) < 0)
				pg_fatal("could not create directory \"%s\": %m", path);
			break;
		case 1:
			/* Present and empty, so do nothing */
			break;
		case 2:
		case 3:
		case 4:
			/* Exists and not empty */
			pg_fatal("directory \"%s\" exists but is not empty", path);
			break;
		default:
			/* Trouble accessing directory */
			pg_fatal("could not access directory \"%s\": %m", path);
	}
}

/*
 * Split a pathname as dirname(1) and basename(1) would.
 *
 * XXX this probably doesn't do very well on Windows.  We probably need to
 * apply canonicalize_path(), at the very least.
 */
static void
split_path(const char *path, char **dir, char **fname)
{
	char	   *sep;

	/* split filepath into directory & filename */
	sep = strrchr(path, '/');

	/* directory path */
	if (sep != NULL)
	{
		*dir = pnstrdup(path, sep - path);
		*fname = pg_strdup(sep + 1);
	}
	/* local directory */
	else
	{
		*dir = NULL;
		*fname = pg_strdup(path);
	}
}

/*
 * Open the file in the valid target directory.
 *
 * return a read only fd
 */
static int
open_file_in_directory(const char *directory, const char *fname)
{
	int			fd = -1;
	char		fpath[MAXPGPATH];
	char	   *dir = directory ? (char *) directory : ".";

	snprintf(fpath, MAXPGPATH, "%s/%s", dir, fname);
	fd = open(fpath, O_RDONLY | PG_BINARY, 0);

	if (fd < 0 && errno != ENOENT)
		pg_fatal("could not open file \"%s\": %m", fname);
	return fd;
}

/*
 * Try to find fname in the given directory. Returns true if it is found,
 * false otherwise. If fname is NULL, search the complete directory for any
 * file with a valid WAL file name. If file is successfully opened, set the
 * wal segment size.
 */
static bool
search_directory(const char *directory, const char *fname)
{
	int			fd = -1;
	DIR		   *xldir;

	/* open file if valid filename is provided */
	if (fname != NULL)
		fd = open_file_in_directory(directory, fname);

	/*
	 * A valid file name is not passed, so search the complete directory.  If
	 * we find any file whose name is a valid WAL file name then try to open
	 * it.  If we cannot open it, bail out.
	 */
	else if ((xldir = opendir(directory)) != NULL)
	{
		struct dirent *xlde;

		while ((xlde = readdir(xldir)) != NULL)
		{
			if (IsXLogFileName(xlde->d_name))
			{
				fd = open_file_in_directory(directory, xlde->d_name);
				fname = pg_strdup(xlde->d_name);
				break;
			}
		}

		closedir(xldir);
	}

	/* set WalSegSz if file is successfully opened */
	if (fd >= 0)
	{
		PGAlignedXLogBlock buf;
		int			r;

		r = read(fd, buf.data, XLOG_BLCKSZ);
		if (r == XLOG_BLCKSZ)
		{
			XLogLongPageHeader longhdr = (XLogLongPageHeader) buf.data;

			WalSegSz = longhdr->xlp_seg_size;

			if (!IsValidWalSegSize(WalSegSz))
			{
				pg_log_error(ngettext("invalid WAL segment size in WAL file \"%s\" (%d byte)",
									  "invalid WAL segment size in WAL file \"%s\" (%d bytes)",
									  WalSegSz),
							 fname, WalSegSz);
				pg_log_error_detail("The WAL segment size must be a power of two between 1 MB and 1 GB.");
				exit(1);
			}
		}
		else if (r < 0)
			pg_fatal("could not read file \"%s\": %m",
					 fname);
		else
			pg_fatal("could not read file \"%s\": read %d of %d",
					 fname, r, XLOG_BLCKSZ);
		close(fd);
		return true;
	}

	return false;
}

/*
 * Identify the target directory.
 *
 * Try to find the file in several places:
 * if directory != NULL:
 *	 directory /
 *	 directory / XLOGDIR /
 * else
 *	 .
 *	 XLOGDIR /
 *	 $PGDATA / XLOGDIR /
 *
 * The valid target directory is returned.
 */
static char *
identify_target_directory(char *directory, char *fname)
{
	char		fpath[MAXPGPATH];

	if (directory != NULL)
	{
		if (search_directory(directory, fname))
			return pg_strdup(directory);

		/* directory / XLOGDIR */
		snprintf(fpath, MAXPGPATH, "%s/%s", directory, XLOGDIR);
		if (search_directory(fpath, fname))
			return pg_strdup(fpath);
	}
	else
	{
		const char *datadir;

		/* current directory */
		if (search_directory(".", fname))
			return pg_strdup(".");
		/* XLOGDIR */
		if (search_directory(XLOGDIR, fname))
			return pg_strdup(XLOGDIR);

		datadir = getenv("PGDATA");
		/* $PGDATA / XLOGDIR */
		if (datadir != NULL)
		{
			snprintf(fpath, MAXPGPATH, "%s/%s", datadir, XLOGDIR);
			if (search_directory(fpath, fname))
				return pg_strdup(fpath);
		}
	}

	/* could not locate WAL file */
	if (fname)
		pg_fatal("could not locate WAL file \"%s\"", fname);
	else
		pg_fatal("could not find any WAL file");

	return NULL;				/* not reached */
}

/*
 * Set up a temporary directory to temporarily store WAL segments.
 */
static char *
setup_tmp_dir(char *waldir)
{
	char	   *tmpdir = waldir != NULL ? waldir : ".";

	tmpdir = psprintf("%s/pg_waldump_tmp_dir",
					  getenv("TMPDIR") ? getenv("TMPDIR") : tmpdir);

	create_directory(tmpdir);

	return tmpdir;
}

/*
 * Removes a directory along with its contents, if any.
 */
static void
remove_tmp_dir(char *tmpdir)
{
	DIR		   *dir;
	struct dirent *de;

	dir = opendir(tmpdir);
	while ((de = readdir(dir)) != NULL)
	{
		char		path[MAXPGPATH];

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		snprintf(path, MAXPGPATH, "%s/%s", tmpdir, de->d_name);
		unlink(path);
	}
	closedir(dir);

	if (rmdir(tmpdir) < 0)
		pg_log_error("could not remove directory \"%s\": %m",
					 tmpdir);
}

/*
 * Returns true if the given file is a tar archive and outputs its compression
 * algorithm.
 */
static bool
is_tar_file(const char *fname, pg_compress_algorithm *compression)
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
 * Creates an appropriate chain of archive streamers for reading the given
 * tar archive.
 */
static void
setup_astreamer(XLogDumpPrivate *private, pg_compress_algorithm compression,
				XLogRecPtr startptr, XLogRecPtr endptr)
{
	astreamer  *streamer = NULL;

	streamer = astreamer_waldump_content_new(NULL, startptr, endptr, private);

	/*
	 * Final extracted WAL data will reside in this streamer. However, since
	 * it sits at the bottom of the stack and isn't designed to propagate data
	 * upward, we need to hold a pointer to its data buffer in order to copy.
	 */
	private->archive_streamer_buf = &streamer->bbs_buffer;

	/* Before that we must parse the tar archive. */
	streamer = astreamer_tar_parser_new(streamer);

	/* Before that we must decompress, if archive is compressed. */
	if (compression == PG_COMPRESSION_GZIP)
		streamer = astreamer_gzip_decompressor_new(streamer);
	else if (compression == PG_COMPRESSION_LZ4)
		streamer = astreamer_lz4_decompressor_new(streamer);
	else if (compression == PG_COMPRESSION_ZSTD)
		streamer = astreamer_zstd_decompressor_new(streamer);

	private->archive_streamer = streamer;
}

/*---
 * Initial setup for reading the archive file:
 *	1. Opens the archive and stores its file descriptor in XLogDumpPrivate for
 *	   later use.
 *	2. Creates a temporary directory for writing WAL segments.
 *	3. Initializes the archive streamer to read from the tar archive.
 */
static void
init_tar_archive_reader(XLogDumpPrivate *private, char *waldir,
						pg_compress_algorithm compression)
{
	int			fd;

	/* Now, the tar archive and store its file descriptor */
	fd = open_file_in_directory(waldir, private->archive_name);

	if (fd < 0)
		pg_fatal("could not open file \"%s\"", private->archive_name);

	private->archive_fd = fd;

	/* Create temporary space for writing WAL segments. */
	private->tmpdir = setup_tmp_dir(waldir);

	/* Setup tar archive reading facility */
	setup_astreamer(private, compression, private->startptr, private->endptr);
}

/*
 * Release the archive streamer chain and close the archive file.
 */
static void
free_tar_archive_reader(XLogDumpPrivate *private)
{
	/*
	 * NB: Normally, astreamer_finalize() is called before astreamer_free() to
	 * flush any remaining buffered data or to ensure the end of the tar
	 * archive is reached. However, when decoding a WAL file, once we hit the
	 * end LSN, any remaining WAL data in the buffer or the tar archive's
	 * unreached end can be safely ignored.
	 */
	astreamer_free(private->archive_streamer);

	/* Close the file. */
	if (close(private->archive_fd) != 0)
		pg_log_error("could not close file \"%s\": %m",
					 private->archive_name);

	/* Remove temporary directory if any */
	if (private->tmpdir != NULL)
		remove_tmp_dir(private->tmpdir);
}

/*
 * Reads a WAL page from the archive and verifies WAL segment size.
 */
static void
verify_tar_archive(XLogDumpPrivate *private, const char *waldir,
				   pg_compress_algorithm compression)
{
	PGAlignedXLogBlock buf;
	int			r;

	setup_astreamer(private, compression, InvalidXLogRecPtr, InvalidXLogRecPtr);

	/* Now, the tar archive and store its file descriptor */
	private->archive_fd = open_file_in_directory(waldir, private->archive_name);

	if (private->archive_fd < 0)
		pg_fatal("could not open file \"%s\"", private->archive_name);

	/* Read a wal page */
	r = astreamer_wal_read(buf.data, 0, XLOG_BLCKSZ, private);

	/* Set WalSegSz if WAL data is successfully read */
	if (r == XLOG_BLCKSZ)
	{
		XLogLongPageHeader longhdr = (XLogLongPageHeader) buf.data;

		WalSegSz = longhdr->xlp_seg_size;

		if (!IsValidWalSegSize(WalSegSz))
		{
			pg_log_error(ngettext("invalid WAL segment size in WAL file \"%s\" (%d byte)",
								  "invalid WAL segment size in WAL file \"%s\" (%d bytes)",
								  WalSegSz),
						 private->archive_name, WalSegSz);
			pg_log_error_detail("The WAL segment size must be a power of two between 1 MB and 1 GB.");
			exit(1);
		}
	}
	else
		pg_fatal("could not read WAL data from \"%s\" archive: read %d of %d",
				 private->archive_name, r, XLOG_BLCKSZ);

	free_tar_archive_reader(private);
}

/* Returns the size in bytes of the data to be read. */
static inline int
required_read_len(XLogDumpPrivate *private, XLogRecPtr targetPagePtr,
				  int reqLen)
{
	int			count = XLOG_BLCKSZ;

	if (private->endptr != InvalidXLogRecPtr)
	{
		if (targetPagePtr + XLOG_BLCKSZ <= private->endptr)
			count = XLOG_BLCKSZ;
		else if (targetPagePtr + reqLen <= private->endptr)
			count = private->endptr - targetPagePtr;
		else
		{
			private->endptr_reached = true;
			return -1;
		}
	}

	return count;
}

/* pg_waldump's XLogReaderRoutine->segment_open callback */
static void
WALDumpOpenSegment(XLogReaderState *state, XLogSegNo nextSegNo,
				   TimeLineID *tli_p)
{
	TimeLineID	tli = *tli_p;
	char		fname[MAXPGPATH];
	int			tries;

	XLogFileName(fname, tli, nextSegNo, state->segcxt.ws_segsize);

	/*
	 * In follow mode there is a short period of time after the server has
	 * written the end of the previous file before the new file is available.
	 * So we loop for 5 seconds looking for the file to appear before giving
	 * up.
	 */
	for (tries = 0; tries < 10; tries++)
	{
		state->seg.ws_file = open_file_in_directory(state->segcxt.ws_dir, fname);
		if (state->seg.ws_file >= 0)
			return;
		if (errno == ENOENT)
		{
			int			save_errno = errno;

			/* File not there yet, try again */
			pg_usleep(500 * 1000);

			errno = save_errno;
			continue;
		}
		/* Any other error, fall through and fail */
		break;
	}

	pg_fatal("could not find file \"%s\": %m", fname);
}

/*
 * pg_waldump's XLogReaderRoutine->segment_close callback.  Same as
 * wal_segment_close
 */
static void
WALDumpCloseSegment(XLogReaderState *state)
{
	close(state->seg.ws_file);
	/* need to check errno? */
	state->seg.ws_file = -1;
}

/* pg_waldump's XLogReaderRoutine->page_read callback */
static int
WALDumpReadPage(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
				XLogRecPtr targetPtr, char *readBuff)
{
	XLogDumpPrivate *private = state->private_data;
	int			count = required_read_len(private, targetPagePtr, reqLen);
	WALReadError errinfo;

	if (private->endptr_reached)
		return -1;

	if (!WALRead(state, readBuff, targetPagePtr, count, private->timeline,
				 &errinfo))
	{
		WALOpenSegment *seg = &errinfo.wre_seg;
		char		fname[MAXPGPATH];

		XLogFileName(fname, seg->ws_tli, seg->ws_segno,
					 state->segcxt.ws_segsize);

		if (errinfo.wre_errno != 0)
		{
			errno = errinfo.wre_errno;
			pg_fatal("could not read from file \"%s\", offset %d: %m",
					 fname, errinfo.wre_off);
		}
		else
			pg_fatal("could not read from file \"%s\", offset %d: read %d of %d",
					 fname, errinfo.wre_off, errinfo.wre_read,
					 errinfo.wre_req);
	}

	return count;
}

/*
 * pg_waldump's XLogReaderRoutine->segment_open callback to support dumping WAL
 * files from tar archives.
 */
static void
TarWALDumpOpenSegment(XLogReaderState *state, XLogSegNo nextSegNo,
					  TimeLineID *tli_p)
{
	/* No action needed */
}

/*
 * pg_waldump's XLogReaderRoutine->segment_close callback.
 */
static void
TarWALDumpCloseSegment(XLogReaderState *state)
{
	/* No action needed */
}

/*
 * pg_waldump's XLogReaderRoutine->page_read callback to support dumping WAL
 * files from tar archives.
 */
static int
TarWALDumpReadPage(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
				   XLogRecPtr targetPtr, char *readBuff)
{
	XLogDumpPrivate *private = state->private_data;
	int			count = required_read_len(private, targetPagePtr, reqLen);
	XLogSegNo	nextSegNo;

	if (private->endptr_reached)
		return -1;

	/*
	 * If the target page is in a different segment, first check for the WAL
	 * segment's physical existence in the temporary directory.
	 */
	nextSegNo = state->seg.ws_segno;
	if (!XLByteInSeg(targetPagePtr, nextSegNo, WalSegSz) ||
		state->seg.ws_tli != private->timeline)
	{
		char		fname[MAXPGPATH];

		if (state->seg.ws_file >= 0)
		{
			char		fpath[MAXPGPATH];

			close(state->seg.ws_file);
			state->seg.ws_file = -1;

			/* Remove this file, as it is no longer needed. */
			XLogFileName(fname, state->seg.ws_tli, nextSegNo, WalSegSz);
			snprintf(fpath, MAXPGPATH, "%s/%s", private->tmpdir, fname);
			unlink(fpath);
		}

		XLByteToSeg(targetPagePtr, nextSegNo, WalSegSz);
		state->seg.ws_tli = private->timeline;
		state->seg.ws_segno = nextSegNo;

		/*
		 * If the next segment exists, open it and continue reading from there
		 */
		XLogFileName(fname, private->timeline, nextSegNo, WalSegSz);
		state->seg.ws_file = open_file_in_directory(private->tmpdir, fname);
	}

	/* Continue reading from the open WAL segment, if any */
	if (state->seg.ws_file >= 0)
		return WALDumpReadPage(state, targetPagePtr, reqLen, targetPtr,
							   readBuff);

	/* Otherwise, read the WAL page from the archive streamer */
	return astreamer_wal_read(readBuff, targetPagePtr, count, private);
}

/*
 * Boolean to return whether the given WAL record matches a specific relation
 * and optionally block.
 */
static bool
XLogRecordMatchesRelationBlock(XLogReaderState *record,
							   RelFileLocator matchRlocator,
							   BlockNumber matchBlock,
							   ForkNumber matchFork)
{
	int			block_id;

	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		RelFileLocator rlocator;
		ForkNumber	forknum;
		BlockNumber blk;

		if (!XLogRecGetBlockTagExtended(record, block_id,
										&rlocator, &forknum, &blk, NULL))
			continue;

		if ((matchFork == InvalidForkNumber || matchFork == forknum) &&
			(RelFileLocatorEquals(matchRlocator, emptyRelFileLocator) ||
			 RelFileLocatorEquals(matchRlocator, rlocator)) &&
			(matchBlock == InvalidBlockNumber || matchBlock == blk))
			return true;
	}

	return false;
}

/*
 * Boolean to return whether the given WAL record contains a full page write.
 */
static bool
XLogRecordHasFPW(XLogReaderState *record)
{
	int			block_id;

	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		if (!XLogRecHasBlockRef(record, block_id))
			continue;

		if (XLogRecHasBlockImage(record, block_id))
			return true;
	}

	return false;
}

/*
 * Function to externally save all FPWs stored in the given WAL record.
 * Decompression is applied to all the blocks saved, if necessary.
 */
static void
XLogRecordSaveFPWs(XLogReaderState *record, const char *savepath)
{
	int			block_id;

	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		PGAlignedBlock buf;
		Page		page;
		char		filename[MAXPGPATH];
		char		forkname[FORKNAMECHARS + 2];	/* _ + terminating zero */
		FILE	   *file;
		BlockNumber blk;
		RelFileLocator rnode;
		ForkNumber	fork;

		if (!XLogRecHasBlockRef(record, block_id))
			continue;

		if (!XLogRecHasBlockImage(record, block_id))
			continue;

		page = (Page) buf.data;

		/* Full page exists, so let's save it */
		if (!RestoreBlockImage(record, block_id, page))
			pg_fatal("%s", record->errormsg_buf);

		(void) XLogRecGetBlockTagExtended(record, block_id,
										  &rnode, &fork, &blk, NULL);

		if (fork >= 0 && fork <= MAX_FORKNUM)
			sprintf(forkname, "_%s", forkNames[fork]);
		else
			pg_fatal("invalid fork number: %u", fork);

		snprintf(filename, MAXPGPATH, "%s/%08X-%08X-%08X.%u.%u.%u.%u%s", savepath,
				 record->seg.ws_tli,
				 LSN_FORMAT_ARGS(record->ReadRecPtr),
				 rnode.spcOid, rnode.dbOid, rnode.relNumber, blk, forkname);

		file = fopen(filename, PG_BINARY_W);
		if (!file)
			pg_fatal("could not open file \"%s\": %m", filename);

		if (fwrite(page, BLCKSZ, 1, file) != 1)
			pg_fatal("could not write file \"%s\": %m", filename);

		if (fclose(file) != 0)
			pg_fatal("could not close file \"%s\": %m", filename);
	}
}

/*
 * Print a record to stdout
 */
static void
XLogDumpDisplayRecord(XLogDumpConfig *config, XLogReaderState *record)
{
	const char *id;
	const RmgrDescData *desc = GetRmgrDesc(XLogRecGetRmid(record));
	uint32		rec_len;
	uint32		fpi_len;
	uint8		info = XLogRecGetInfo(record);
	XLogRecPtr	xl_prev = XLogRecGetPrev(record);
	StringInfoData s;

	XLogRecGetLen(record, &rec_len, &fpi_len);

	printf("rmgr: %-11s len (rec/tot): %6u/%6u, tx: %10u, lsn: %X/%08X, prev %X/%08X, ",
		   desc->rm_name,
		   rec_len, XLogRecGetTotalLen(record),
		   XLogRecGetXid(record),
		   LSN_FORMAT_ARGS(record->ReadRecPtr),
		   LSN_FORMAT_ARGS(xl_prev));

	id = desc->rm_identify(info);
	if (id == NULL)
		printf("desc: UNKNOWN (%x) ", info & ~XLR_INFO_MASK);
	else
		printf("desc: %s ", id);

	initStringInfo(&s);
	desc->rm_desc(&s, record);
	printf("%s", s.data);

	resetStringInfo(&s);
	XLogRecGetBlockRefInfo(record, true, config->bkp_details, &s, NULL);
	printf("%s", s.data);
	pfree(s.data);
}

/*
 * Display a single row of record counts and sizes for an rmgr or record.
 */
static void
XLogDumpStatsRow(const char *name,
				 uint64 n, uint64 total_count,
				 uint64 rec_len, uint64 total_rec_len,
				 uint64 fpi_len, uint64 total_fpi_len,
				 uint64 tot_len, uint64 total_len)
{
	double		n_pct,
				rec_len_pct,
				fpi_len_pct,
				tot_len_pct;

	n_pct = 0;
	if (total_count != 0)
		n_pct = 100 * (double) n / total_count;

	rec_len_pct = 0;
	if (total_rec_len != 0)
		rec_len_pct = 100 * (double) rec_len / total_rec_len;

	fpi_len_pct = 0;
	if (total_fpi_len != 0)
		fpi_len_pct = 100 * (double) fpi_len / total_fpi_len;

	tot_len_pct = 0;
	if (total_len != 0)
		tot_len_pct = 100 * (double) tot_len / total_len;

	printf("%-27s "
		   "%20" PRIu64 " (%6.02f) "
		   "%20" PRIu64 " (%6.02f) "
		   "%20" PRIu64 " (%6.02f) "
		   "%20" PRIu64 " (%6.02f)\n",
		   name, n, n_pct, rec_len, rec_len_pct, fpi_len, fpi_len_pct,
		   tot_len, tot_len_pct);
}


/*
 * Display summary statistics about the records seen so far.
 */
static void
XLogDumpDisplayStats(XLogDumpConfig *config, XLogStats *stats)
{
	int			ri,
				rj;
	uint64		total_count = 0;
	uint64		total_rec_len = 0;
	uint64		total_fpi_len = 0;
	uint64		total_len = 0;
	double		rec_len_pct,
				fpi_len_pct;

	/*
	 * Leave if no stats have been computed yet, as tracked by the end LSN.
	 */
	if (XLogRecPtrIsInvalid(stats->endptr))
		return;

	/*
	 * Each row shows its percentages of the total, so make a first pass to
	 * calculate column totals.
	 */

	for (ri = 0; ri <= RM_MAX_ID; ri++)
	{
		if (!RmgrIdIsValid(ri))
			continue;

		total_count += stats->rmgr_stats[ri].count;
		total_rec_len += stats->rmgr_stats[ri].rec_len;
		total_fpi_len += stats->rmgr_stats[ri].fpi_len;
	}
	total_len = total_rec_len + total_fpi_len;

	printf("WAL statistics between %X/%08X and %X/%08X:\n",
		   LSN_FORMAT_ARGS(stats->startptr), LSN_FORMAT_ARGS(stats->endptr));

	/*
	 * 27 is strlen("Transaction/COMMIT_PREPARED"), 20 is strlen(2^64), 8 is
	 * strlen("(100.00%)")
	 */

	printf("%-27s %20s %8s %20s %8s %20s %8s %20s %8s\n"
		   "%-27s %20s %8s %20s %8s %20s %8s %20s %8s\n",
		   "Type", "N", "(%)", "Record size", "(%)", "FPI size", "(%)", "Combined size", "(%)",
		   "----", "-", "---", "-----------", "---", "--------", "---", "-------------", "---");

	for (ri = 0; ri <= RM_MAX_ID; ri++)
	{
		uint64		count,
					rec_len,
					fpi_len,
					tot_len;
		const RmgrDescData *desc;

		if (!RmgrIdIsValid(ri))
			continue;

		desc = GetRmgrDesc(ri);

		if (!config->stats_per_record)
		{
			count = stats->rmgr_stats[ri].count;
			rec_len = stats->rmgr_stats[ri].rec_len;
			fpi_len = stats->rmgr_stats[ri].fpi_len;
			tot_len = rec_len + fpi_len;

			if (RmgrIdIsCustom(ri) && count == 0)
				continue;

			XLogDumpStatsRow(desc->rm_name,
							 count, total_count, rec_len, total_rec_len,
							 fpi_len, total_fpi_len, tot_len, total_len);
		}
		else
		{
			for (rj = 0; rj < MAX_XLINFO_TYPES; rj++)
			{
				const char *id;

				count = stats->record_stats[ri][rj].count;
				rec_len = stats->record_stats[ri][rj].rec_len;
				fpi_len = stats->record_stats[ri][rj].fpi_len;
				tot_len = rec_len + fpi_len;

				/* Skip undefined combinations and ones that didn't occur */
				if (count == 0)
					continue;

				/* the upper four bits in xl_info are the rmgr's */
				id = desc->rm_identify(rj << 4);
				if (id == NULL)
					id = psprintf("UNKNOWN (%x)", rj << 4);

				XLogDumpStatsRow(psprintf("%s/%s", desc->rm_name, id),
								 count, total_count, rec_len, total_rec_len,
								 fpi_len, total_fpi_len, tot_len, total_len);
			}
		}
	}

	printf("%-27s %20s %8s %20s %8s %20s %8s %20s\n",
		   "", "--------", "", "--------", "", "--------", "", "--------");

	/*
	 * The percentages in earlier rows were calculated against the column
	 * total, but the ones that follow are against the row total. Note that
	 * these are displayed with a % symbol to differentiate them from the
	 * earlier ones, and are thus up to 9 characters long.
	 */

	rec_len_pct = 0;
	if (total_len != 0)
		rec_len_pct = 100 * (double) total_rec_len / total_len;

	fpi_len_pct = 0;
	if (total_len != 0)
		fpi_len_pct = 100 * (double) total_fpi_len / total_len;

	printf("%-27s "
		   "%20" PRIu64 " %-9s"
		   "%20" PRIu64 " %-9s"
		   "%20" PRIu64 " %-9s"
		   "%20" PRIu64 " %-6s\n",
		   "Total", stats->count, "",
		   total_rec_len, psprintf("[%.02f%%]", rec_len_pct),
		   total_fpi_len, psprintf("[%.02f%%]", fpi_len_pct),
		   total_len, "[100%]");
}

static void
usage(void)
{
	printf(_("%s decodes and displays PostgreSQL write-ahead logs for debugging.\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [STARTSEG [ENDSEG]]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -b, --bkp-details      output detailed information about backup blocks\n"));
	printf(_("  -B, --block=N          with --relation, only show records that modify block N\n"));
	printf(_("  -e, --end=RECPTR       stop reading at WAL location RECPTR\n"));
	printf(_("  -f, --follow           keep retrying after reaching end of WAL\n"));
	printf(_("  -F, --fork=FORK        only show records that modify blocks in fork FORK;\n"
			 "                         valid names are main, fsm, vm, init\n"));
	printf(_("  -n, --limit=N          number of records to display\n"));
	printf(_("  -p, --path=PATH        tar archive or a directory in which to find WAL segment files or\n"
			 "                         a directory with a ./pg_wal that contains such files\n"
			 "                         (default: current directory, ./pg_wal, $PGDATA/pg_wal)\n"));
	printf(_("  -q, --quiet            do not print any output, except for errors\n"));
	printf(_("  -r, --rmgr=RMGR        only show records generated by resource manager RMGR;\n"
			 "                         use --rmgr=list to list valid resource manager names\n"));
	printf(_("  -R, --relation=T/D/R   only show records that modify blocks in relation T/D/R\n"));
	printf(_("  -s, --start=RECPTR     start reading at WAL location RECPTR\n"));
	printf(_("  -t, --timeline=TLI     timeline from which to read WAL records\n"
			 "                         (default: 1 or the value used in STARTSEG)\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -w, --fullpage         only show records with a full page write\n"));
	printf(_("  -x, --xid=XID          only show records with transaction ID XID\n"));
	printf(_("  -z, --stats[=record]   show statistics instead of records\n"
			 "                         (optionally, show per-record statistics)\n"));
	printf(_("  --save-fullpage=DIR    save full page images to DIR\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

int
main(int argc, char **argv)
{
	uint32		xlogid;
	uint32		xrecoff;
	XLogReaderState *xlogreader_state;
	XLogDumpPrivate private;
	XLogDumpConfig config;
	XLogStats	stats;
	XLogRecord *record;
	XLogRecPtr	first_record;
	char	   *waldir = NULL;
	char	   *walpath = NULL;
	char	   *errormsg;
	bool		is_tar = false;
	XLogReaderRoutine *routine = NULL;
	pg_compress_algorithm compression;

	static struct option long_options[] = {
		{"bkp-details", no_argument, NULL, 'b'},
		{"block", required_argument, NULL, 'B'},
		{"end", required_argument, NULL, 'e'},
		{"follow", no_argument, NULL, 'f'},
		{"fork", required_argument, NULL, 'F'},
		{"fullpage", no_argument, NULL, 'w'},
		{"help", no_argument, NULL, '?'},
		{"limit", required_argument, NULL, 'n'},
		{"path", required_argument, NULL, 'p'},
		{"quiet", no_argument, NULL, 'q'},
		{"relation", required_argument, NULL, 'R'},
		{"rmgr", required_argument, NULL, 'r'},
		{"start", required_argument, NULL, 's'},
		{"timeline", required_argument, NULL, 't'},
		{"xid", required_argument, NULL, 'x'},
		{"version", no_argument, NULL, 'V'},
		{"stats", optional_argument, NULL, 'z'},
		{"save-fullpage", required_argument, NULL, 1},
		{NULL, 0, NULL, 0}
	};

	int			option;
	int			optindex = 0;

#ifndef WIN32
	pqsignal(SIGINT, sigint_handler);
#endif

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_waldump"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_waldump (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	memset(&private, 0, sizeof(XLogDumpPrivate));
	memset(&config, 0, sizeof(XLogDumpConfig));
	memset(&stats, 0, sizeof(XLogStats));

	private.timeline = 1;
	private.startptr = InvalidXLogRecPtr;
	private.endptr = InvalidXLogRecPtr;
	private.endptr_reached = false;

	config.quiet = false;
	config.bkp_details = false;
	config.stop_after_records = -1;
	config.already_displayed_records = 0;
	config.follow = false;
	/* filter_by_rmgr array was zeroed by memset above */
	config.filter_by_rmgr_enabled = false;
	config.filter_by_xid = InvalidTransactionId;
	config.filter_by_xid_enabled = false;
	config.filter_by_extended = false;
	config.filter_by_relation_enabled = false;
	config.filter_by_relation_block_enabled = false;
	config.filter_by_relation_forknum = InvalidForkNumber;
	config.filter_by_fpw = false;
	config.save_fullpage_path = NULL;
	config.stats = false;
	config.stats_per_record = false;

	stats.startptr = InvalidXLogRecPtr;
	stats.endptr = InvalidXLogRecPtr;

	if (argc <= 1)
	{
		pg_log_error("no arguments specified");
		goto bad_argument;
	}

	while ((option = getopt_long(argc, argv, "bB:e:fF:n:p:qr:R:s:t:wx:z",
								 long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'b':
				config.bkp_details = true;
				break;
			case 'B':
				if (sscanf(optarg, "%u", &config.filter_by_relation_block) != 1 ||
					!BlockNumberIsValid(config.filter_by_relation_block))
				{
					pg_log_error("invalid block number: \"%s\"", optarg);
					goto bad_argument;
				}
				config.filter_by_relation_block_enabled = true;
				config.filter_by_extended = true;
				break;
			case 'e':
				if (sscanf(optarg, "%X/%08X", &xlogid, &xrecoff) != 2)
				{
					pg_log_error("invalid WAL location: \"%s\"",
								 optarg);
					goto bad_argument;
				}
				private.endptr = (uint64) xlogid << 32 | xrecoff;
				break;
			case 'f':
				config.follow = true;
				break;
			case 'F':
				config.filter_by_relation_forknum = forkname_to_number(optarg);
				if (config.filter_by_relation_forknum == InvalidForkNumber)
				{
					pg_log_error("invalid fork name: \"%s\"", optarg);
					goto bad_argument;
				}
				config.filter_by_extended = true;
				break;
			case 'n':
				if (sscanf(optarg, "%d", &config.stop_after_records) != 1)
				{
					pg_log_error("invalid value \"%s\" for option %s", optarg, "-n/--limit");
					goto bad_argument;
				}
				break;
			case 'p':
				walpath = pg_strdup(optarg);
				break;
			case 'q':
				config.quiet = true;
				break;
			case 'r':
				{
					int			rmid;

					if (pg_strcasecmp(optarg, "list") == 0)
					{
						print_rmgr_list();
						exit(EXIT_SUCCESS);
					}

					/*
					 * First look for the generated name of a custom rmgr, of
					 * the form "custom###". We accept this form, because the
					 * custom rmgr module is not loaded, so there's no way to
					 * know the real name. This convention should be
					 * consistent with that in rmgrdesc.c.
					 */
					if (sscanf(optarg, "custom%03d", &rmid) == 1)
					{
						if (!RmgrIdIsCustom(rmid))
						{
							pg_log_error("custom resource manager \"%s\" does not exist",
										 optarg);
							goto bad_argument;
						}
						config.filter_by_rmgr[rmid] = true;
						config.filter_by_rmgr_enabled = true;
					}
					else
					{
						/* then look for builtin rmgrs */
						for (rmid = 0; rmid <= RM_MAX_BUILTIN_ID; rmid++)
						{
							if (pg_strcasecmp(optarg, GetRmgrDesc(rmid)->rm_name) == 0)
							{
								config.filter_by_rmgr[rmid] = true;
								config.filter_by_rmgr_enabled = true;
								break;
							}
						}
						if (rmid > RM_MAX_BUILTIN_ID)
						{
							pg_log_error("resource manager \"%s\" does not exist",
										 optarg);
							goto bad_argument;
						}
					}
				}
				break;
			case 'R':
				if (sscanf(optarg, "%u/%u/%u",
						   &config.filter_by_relation.spcOid,
						   &config.filter_by_relation.dbOid,
						   &config.filter_by_relation.relNumber) != 3 ||
					!OidIsValid(config.filter_by_relation.spcOid) ||
					!RelFileNumberIsValid(config.filter_by_relation.relNumber))
				{
					pg_log_error("invalid relation specification: \"%s\"", optarg);
					pg_log_error_detail("Expecting \"tablespace OID/database OID/relation filenode\".");
					goto bad_argument;
				}
				config.filter_by_relation_enabled = true;
				config.filter_by_extended = true;
				break;
			case 's':
				if (sscanf(optarg, "%X/%08X", &xlogid, &xrecoff) != 2)
				{
					pg_log_error("invalid WAL location: \"%s\"",
								 optarg);
					goto bad_argument;
				}
				else
					private.startptr = (uint64) xlogid << 32 | xrecoff;
				break;
			case 't':

				/*
				 * This is like option_parse_int() but needs to handle
				 * unsigned 32-bit int.  Also, we accept both decimal and
				 * hexadecimal specifications here.
				 */
				{
					char	   *endptr;
					unsigned long val;

					errno = 0;
					val = strtoul(optarg, &endptr, 0);

					while (*endptr != '\0' && isspace((unsigned char) *endptr))
						endptr++;

					if (*endptr != '\0')
					{
						pg_log_error("invalid value \"%s\" for option %s",
									 optarg, "-t/--timeline");
						goto bad_argument;
					}

					if (errno == ERANGE || val < 1 || val > UINT_MAX)
					{
						pg_log_error("%s must be in range %u..%u",
									 "-t/--timeline", 1, UINT_MAX);
						goto bad_argument;
					}

					private.timeline = val;

					break;
				}
			case 'w':
				config.filter_by_fpw = true;
				break;
			case 'x':
				if (sscanf(optarg, "%u", &config.filter_by_xid) != 1)
				{
					pg_log_error("invalid transaction ID specification: \"%s\"",
								 optarg);
					goto bad_argument;
				}
				config.filter_by_xid_enabled = true;
				break;
			case 'z':
				config.stats = true;
				config.stats_per_record = false;
				if (optarg)
				{
					if (strcmp(optarg, "record") == 0)
						config.stats_per_record = true;
					else if (strcmp(optarg, "rmgr") != 0)
					{
						pg_log_error("unrecognized value for option %s: %s",
									 "--stats", optarg);
						goto bad_argument;
					}
				}
				break;
			case 1:
				config.save_fullpage_path = pg_strdup(optarg);
				break;
			default:
				goto bad_argument;
		}
	}

	if (config.filter_by_relation_block_enabled &&
		!config.filter_by_relation_enabled)
	{
		pg_log_error("option %s requires option %s to be specified",
					 "-B/--block", "-R/--relation");
		goto bad_argument;
	}

	if ((optind + 2) < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind + 2]);
		goto bad_argument;
	}

	if (walpath != NULL)
	{
		/* validate path points to tar archive */
		if (is_tar_file(walpath, &compression))
		{
			char	   *fname = NULL;

			split_path(walpath, &waldir, &fname);

			private.archive_name = fname;
			is_tar = true;
		}
		/* validate path points to directory */
		else if (!verify_directory(walpath))
		{
			pg_log_error("could not open directory \"%s\": %m", waldir);
			goto bad_argument;
		}
	}

	/*
	 * Create if necessary the directory storing the full-page images
	 * extracted from the WAL records read.
	 */
	if (config.save_fullpage_path != NULL)
		create_directory(config.save_fullpage_path);

	/* parse files as start/end boundaries, extract path if not specified */
	if (optind < argc)
	{
		char	   *directory = NULL;
		char	   *fname = NULL;
		int			fd;
		XLogSegNo	segno;

		split_path(argv[optind], &directory, &fname);

		if (walpath == NULL && directory != NULL)
		{
			walpath = directory;

			if (!verify_directory(walpath))
				pg_fatal("could not open directory \"%s\": %m", waldir);
		}

		if (fname != NULL && is_tar_file(fname, &compression))
		{
			private.archive_name = fname;
			waldir = walpath;
			is_tar = true;
		}
		else
		{
			waldir = identify_target_directory(walpath, fname);

			fd = open_file_in_directory(waldir, fname);
			if (fd < 0)
				pg_fatal("could not open file \"%s\"", fname);
			close(fd);

			/* parse position from file */
			XLogFromFileName(fname, &private.timeline, &segno, WalSegSz);

			if (XLogRecPtrIsInvalid(private.startptr))
				XLogSegNoOffsetToRecPtr(segno, 0, WalSegSz, private.startptr);
			else if (!XLByteInSeg(private.startptr, segno, WalSegSz))
			{
				pg_log_error("start WAL location %X/%08X is not inside file \"%s\"",
							 LSN_FORMAT_ARGS(private.startptr),
							 fname);
				goto bad_argument;
			}

			/* no second file specified, set end position */
			if (!(optind + 1 < argc) && XLogRecPtrIsInvalid(private.endptr))
				XLogSegNoOffsetToRecPtr(segno + 1, 0, WalSegSz, private.endptr);

			/* parse ENDSEG if passed */
			if (optind + 1 < argc)
			{
				XLogSegNo	endsegno;

				/* ignore directory, already have that */
				split_path(argv[optind + 1], &directory, &fname);

				fd = open_file_in_directory(waldir, fname);
				if (fd < 0)
					pg_fatal("could not open file \"%s\"", fname);
				close(fd);

				/* parse position from file */
				XLogFromFileName(fname, &private.timeline, &endsegno, WalSegSz);

				if (endsegno < segno)
					pg_fatal("ENDSEG %s is before STARTSEG %s",
							 argv[optind + 1], argv[optind]);

				if (XLogRecPtrIsInvalid(private.endptr))
					XLogSegNoOffsetToRecPtr(endsegno + 1, 0, WalSegSz,
											private.endptr);

				/* set segno to endsegno for check of --end */
				segno = endsegno;
			}


			if (!XLByteInSeg(private.endptr, segno, WalSegSz) &&
				private.endptr != (segno + 1) * WalSegSz)
			{
				pg_log_error("end WAL location %X/%08X is not inside file \"%s\"",
							 LSN_FORMAT_ARGS(private.endptr),
							 argv[argc - 1]);
				goto bad_argument;
			}
		}
	}
	else if (!is_tar)
		waldir = identify_target_directory(walpath, NULL);

	/* Verify that the archive contains valid WAL files */
	if (is_tar)
		verify_tar_archive(&private, waldir, compression);

	/* we don't know what to print */
	if (XLogRecPtrIsInvalid(private.startptr))
	{
		pg_log_error("no start WAL location given");
		goto bad_argument;
	}

	/* done with argument parsing, do the actual work */

	/* we have everything we need, start reading */
	if (is_tar)
	{
		/* Set up for reading tar file */
		init_tar_archive_reader(&private, waldir, compression);

		/* Routine to decode WAL files in tar archive */
		routine = XL_ROUTINE(.page_read = TarWALDumpReadPage,
							 .segment_open = TarWALDumpOpenSegment,
							 .segment_close = TarWALDumpCloseSegment);
	}
	else
	{
		/* Routine to decode WAL files */
		routine = XL_ROUTINE(.page_read = WALDumpReadPage,
							 .segment_open = WALDumpOpenSegment,
							 .segment_close = WALDumpCloseSegment);
	}

	xlogreader_state =
		XLogReaderAllocate(WalSegSz, waldir, routine,
						   &private);
	if (!xlogreader_state)
		pg_fatal("out of memory while allocating a WAL reading processor");

	/* first find a valid recptr to start from */
	first_record = XLogFindNextRecord(xlogreader_state, private.startptr);

	if (first_record == InvalidXLogRecPtr)
		pg_fatal("could not find a valid record after %X/%08X",
				 LSN_FORMAT_ARGS(private.startptr));

	/*
	 * Display a message that we're skipping data if `from` wasn't a pointer
	 * to the start of a record and also wasn't a pointer to the beginning of
	 * a segment (e.g. we were used in file mode).
	 */
	if (first_record != private.startptr &&
		XLogSegmentOffset(private.startptr, WalSegSz) != 0)
		pg_log_info(ngettext("first record is after %X/%08X, at %X/%08X, skipping over %u byte",
							 "first record is after %X/%08X, at %X/%08X, skipping over %u bytes",
							 (first_record - private.startptr)),
					LSN_FORMAT_ARGS(private.startptr),
					LSN_FORMAT_ARGS(first_record),
					(uint32) (first_record - private.startptr));

	if (config.stats == true && !config.quiet)
		stats.startptr = first_record;

	for (;;)
	{
		if (time_to_stop)
		{
			/* We've been Ctrl-C'ed, so leave */
			break;
		}

		/* try to read the next record */
		record = XLogReadRecord(xlogreader_state, &errormsg);
		if (!record)
		{
			if (!config.follow || private.endptr_reached)
				break;
			else
			{
				pg_usleep(1000000L);	/* 1 second */
				continue;
			}
		}

		/* apply all specified filters */
		if (config.filter_by_rmgr_enabled &&
			!config.filter_by_rmgr[record->xl_rmid])
			continue;

		if (config.filter_by_xid_enabled &&
			config.filter_by_xid != record->xl_xid)
			continue;

		/* check for extended filtering */
		if (config.filter_by_extended &&
			!XLogRecordMatchesRelationBlock(xlogreader_state,
											config.filter_by_relation_enabled ?
											config.filter_by_relation :
											emptyRelFileLocator,
											config.filter_by_relation_block_enabled ?
											config.filter_by_relation_block :
											InvalidBlockNumber,
											config.filter_by_relation_forknum))
			continue;

		if (config.filter_by_fpw && !XLogRecordHasFPW(xlogreader_state))
			continue;

		/* perform any per-record work */
		if (!config.quiet)
		{
			if (config.stats == true)
			{
				XLogRecStoreStats(&stats, xlogreader_state);
				stats.endptr = xlogreader_state->EndRecPtr;
			}
			else
				XLogDumpDisplayRecord(&config, xlogreader_state);
		}

		/* save full pages if requested */
		if (config.save_fullpage_path != NULL)
			XLogRecordSaveFPWs(xlogreader_state, config.save_fullpage_path);

		/* check whether we printed enough */
		config.already_displayed_records++;
		if (config.stop_after_records > 0 &&
			config.already_displayed_records >= config.stop_after_records)
			break;
	}

	if (config.stats == true && !config.quiet)
		XLogDumpDisplayStats(&config, &stats);

	if (time_to_stop)
		exit(0);

	if (errormsg)
		pg_fatal("error in WAL record at %X/%08X: %s",
				 LSN_FORMAT_ARGS(xlogreader_state->ReadRecPtr),
				 errormsg);

	XLogReaderFree(xlogreader_state);

	if (is_tar)
		free_tar_archive_reader(&private);

	return EXIT_SUCCESS;

bad_argument:
	pg_log_error_hint("Try \"%s --help\" for more information.", progname);
	return EXIT_FAILURE;
}
