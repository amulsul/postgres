/*-------------------------------------------------------------------------
 *
 * astreamer_verify.c
 *
 * Extend fe_utils/astreamer.h archive streaming facility to verify TAR
 * backup.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/bin/pg_verifybackup/astreamer_verify.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/logging.h"
#include "pg_verifybackup.h"

typedef struct astreamer_verify
{
	astreamer	base;
	verifier_context *context;
	char	   *archive_name;
	Oid			tblspc_oid;
	pg_checksum_context *checksum_ctx;

	/* Hold information for a member file verification */
	manifest_file *mfile;
	int64		received_bytes;
	bool		verify_checksum;
	bool		verify_control_data;
} astreamer_verify;

static void astreamer_verify_content(astreamer *streamer,
									 astreamer_member *member,
									 const char *data, int len,
									 astreamer_archive_context context);
static void astreamer_verify_finalize(astreamer *streamer);
static void astreamer_verify_free(astreamer *streamer);

static void verify_member_header(astreamer *streamer, astreamer_member *member);
static void verify_member_contents(astreamer *streamer,
								   astreamer_member *member,
								   const char *data, int len);
static void verify_content_checksum(astreamer *streamer,
									astreamer_member *member,
									const char *buffer, int buffer_len);
static void verify_controldata(astreamer *streamer,
							   astreamer_member *member,
							   const char *data, int len);
static void reset_member_info(astreamer *streamer);

static const astreamer_ops astreamer_verify_ops = {
	.content = astreamer_verify_content,
	.finalize = astreamer_verify_finalize,
	.free = astreamer_verify_free
};

/*
 * Create a astreamer that can verifies content of a TAR file.
 */
astreamer *
astreamer_verify_content_new(astreamer *next, verifier_context *context,
							 char *archive_name, Oid tblspc_oid)
{
	astreamer_verify *streamer;

	streamer = palloc0(sizeof(astreamer_verify));
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_verify_ops;

	streamer->base.bbs_next = next;
	streamer->context = context;
	streamer->archive_name = archive_name;
	streamer->tblspc_oid = tblspc_oid;
	initStringInfo(&streamer->base.bbs_buffer);

	if (!context->skip_checksums)
		streamer->checksum_ctx = pg_malloc(sizeof(pg_checksum_context));

	return &streamer->base;
}

/*
 * It verifies each TAR member entry against the manifest data and performs
 * checksum verification if enabled. Additionally, it validates the backup's
 * system identifier against the backup_manifest.
 */
static void
astreamer_verify_content(astreamer *streamer, astreamer_member *member,
						 const char *data, int len,
						 astreamer_archive_context context)
{
	Assert(context != ASTREAMER_UNKNOWN);

	switch (context)
	{
		case ASTREAMER_MEMBER_HEADER:

			/*
			 * Perform the initial check and setup verification steps.
			 */
			verify_member_header(streamer, member);
			break;

		case ASTREAMER_MEMBER_CONTENTS:

			/*
			 * Peform the required contents verification.
			 */
			verify_member_contents(streamer, member, data, len);
			break;

		case ASTREAMER_MEMBER_TRAILER:

			/*
			 * Reset the temporary information stored for the verification.
			 */
			reset_member_info(streamer);
			break;

		case ASTREAMER_ARCHIVE_TRAILER:
			break;

		default:
			/* Shouldn't happen. */
			pg_fatal("unexpected state while parsing tar archive");
	}
}

/*
 * End-of-stream processing for a astreamer_verify stream.
 */
static void
astreamer_verify_finalize(astreamer *streamer)
{
	Assert(streamer->bbs_next == NULL);
}

/*
 * Free memory associated with a astreamer_verify stream.
 */
static void
astreamer_verify_free(astreamer *streamer)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;

	if (mystreamer->checksum_ctx)
		pfree(mystreamer->checksum_ctx);

	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
}

/*
 * Verify the entry if it is a member file in the backup manifest. If the
 * archive being processed is a tablespace, prepare the required file path for
 * subsequent operations. Finally, check if it needs to perform checksum
 * verification and control data verification during file content processing.
 */
static void
verify_member_header(astreamer *streamer, astreamer_member *member)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;
	manifest_file *m;

	/* We are only interested in files that are not in the ignore list. */
	if (member->is_directory || member->is_link ||
		should_ignore_relpath(mystreamer->context, member->pathname))
		return;

	/*
	 * The backup_manifest stores a relative path to the base directory for
	 * files belong tablespace, whereas <tablespaceoid>.tar doesn't. Prepare
	 * the required path, otherwise, the manfiest entry verification will
	 * fail.
	 */
	if (OidIsValid(mystreamer->tblspc_oid))
	{
		char		temp[MAXPGPATH];

		/* Copy original name at temporary space */
		memcpy(temp, member->pathname, MAXPGPATH);

		snprintf(member->pathname, MAXPGPATH, "%s/%d/%s",
				 "pg_tblspc", mystreamer->tblspc_oid, temp);
	}

	/* Check the manifest entry */
	m = verify_manifest_entry(mystreamer->context, member->pathname,
							  member->size);
	mystreamer->mfile = (void *) m;

	/*
	 * Prepare for checksum and control data verification.
	 *
	 * We could have these checks while receiving contents. However, since
	 * contents are received in multiple iterations, this would result in
	 * these lengthy checks being performed multiple times. Instead, having a
	 * single flag would be more efficient.
	 */
	mystreamer->verify_checksum =
		(!mystreamer->context->skip_checksums && should_verify_checksum(m));
	mystreamer->verify_control_data =
		should_verify_control_data(mystreamer->context->manifest, m);

	/* Initialize the context required for checksum verification. */
	if (mystreamer->verify_checksum &&
		pg_checksum_init(mystreamer->checksum_ctx, m->checksum_type) < 0)
	{
		report_backup_error(mystreamer->context,
							"%s: could not initialize checksum of file \"%s\"",
							mystreamer->archive_name, m->pathname);

		/*
		 * Checksum verification cannot be performed without proper context
		 * initialization.
		 */
		mystreamer->verify_checksum = false;
	}
}

/*
 * Process the member content according to the flags set by the member header
 * processing routine for checksum and control data verification.
 */
static void
verify_member_contents(astreamer *streamer, astreamer_member *member,
					   const char *data, int len)

{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;

	/* Verify the checksums */
	if (mystreamer->verify_checksum)
		verify_content_checksum(streamer, member, data, len);

	/* Verify pg_control information */
	if (mystreamer->verify_control_data)
		verify_controldata(streamer, member, data, len);
}

/*
 * Similar to verify_file_checksum() but this function computes the checksum
 * incrementally for the received file content. Unlike a normal backup
 * directory, TAR format files do not allow random access, so checksum
 * verification occurs progressively. Additionally, the function calls the
 * routine for control data verification if the flags indicate that it is
 * required.
 *
 * Caller should pass correctly initialised checksum_ctx, which will be used
 * for incremental checksum calculation. Once the complete file content is
 * received (tracked using the received_bytes), the routine that performs the
 * final checksum verification is called
 */
static void
verify_content_checksum(astreamer *streamer, astreamer_member *member,
						const char *buffer, int buffer_len)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;
	pg_checksum_context *checksum_ctx = mystreamer->checksum_ctx;
	verifier_context *context = mystreamer->context;
	manifest_file *m = mystreamer->mfile;
	const char *relpath = m->pathname;
	uint8		checksumbuf[PG_CHECKSUM_MAX_LENGTH];

	/*
	 * Mark it false to avoid unexpected re-entrance for the same file content
	 * (e.g. returned in error should not be revisited).
	 */
	Assert(mystreamer->verify_checksum);
	mystreamer->verify_checksum = false;

	/* Should have came for the right file */
	Assert(strcmp(member->pathname, relpath) == 0);

	/*
	 * The checksum context should match the type noted in the backup
	 * manifest.
	 */
	Assert(checksum_ctx->type == m->checksum_type);

	/* Update the total count of computed checksum bytes. */
	mystreamer->received_bytes += buffer_len;

	if (pg_checksum_update(checksum_ctx, (uint8 *) buffer, buffer_len) < 0)
	{
		report_backup_error(context, "could not update checksum of file \"%s\"",
							relpath);
		return;
	}

	/* Yet to receive the full content of the file. */
	if (mystreamer->received_bytes < m->size)
	{
		mystreamer->verify_checksum = true;
		return;
	}

	/* Do the final computation and verification. */
	verify_checksum(context, m, checksum_ctx, checksumbuf);
}

/*
 * Prepare the control data from the received file contents, which are supposed
 * to be from the pg_control file, including CRC calculation. Then, call the
 * routines that perform the final verification of the control file information.
 */
static void
verify_controldata(astreamer *streamer, astreamer_member *member,
				   const char *data, int len)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;
	manifest_data *manifest = mystreamer->context->manifest;
	ControlFileData control_file;
	pg_crc32c	crc;
	bool		crc_ok;

	/* Should be here only for control file */
	Assert(strcmp(member->pathname, "global/pg_control") == 0);
	Assert(manifest->version != 1);

	/* Mark it as false to avoid unexpected re-entrance */
	Assert(mystreamer->verify_control_data);
	mystreamer->verify_control_data = false;

	/* Should have whole control file data. */
	if (!astreamer_buffer_until(streamer, &data, &len, sizeof(ControlFileData)))
	{
		mystreamer->verify_control_data = true;
		return;
	}

	pg_log_debug("%s: reading \"%s\"", mystreamer->archive_name,
				 member->pathname);

	if (streamer->bbs_buffer.len != sizeof(ControlFileData))
		report_fatal_error("%s: could not read control file: read %d of %zu",
						   mystreamer->archive_name, streamer->bbs_buffer.len,
						   sizeof(ControlFileData));

	memcpy(&control_file, streamer->bbs_buffer.data,
		   sizeof(ControlFileData));

	/* Check the CRC. */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc,
				(char *) (&control_file),
				offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);

	crc_ok = EQ_CRC32C(crc, control_file.crc);

	/* Do the final control data verification. */
	verify_control_data(&control_file, member->pathname, crc_ok,
						manifest->system_identifier);
}

/*
 * Reset flags and free memory allocations for member file verification.
 */
static void
reset_member_info(astreamer *streamer)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;

	mystreamer->mfile = NULL;
	mystreamer->received_bytes = 0;
	mystreamer->verify_checksum = false;
	mystreamer->verify_control_data = false;
}
