/*-------------------------------------------------------------------------
 *
 * astreamer_verify.c
 *
 * Add a new archive streamer to verify tar content parsed using the
 * fe_utils/astreamer.h facility.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/bin/pg_verifybackup/astreamer_verify.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/logging.h"
#include "fe_utils/astreamer.h"
#include "pg_verifybackup.h"

typedef struct astreamer_verify
{
	astreamer	base;
	verifier_context *context;
	char	   *archive_name;
	Oid			tblspc_oid;

	manifest_file *mfile;
	size_t		received_bytes;
	bool		verify_checksums;
	bool		verify_sysid;
	pg_checksum_context *checksum_ctx;
} astreamer_verify;

static void astreamer_verify_content(astreamer *streamer,
									 astreamer_member *member,
									 const char *data, int len,
									 astreamer_archive_context context);
static void astreamer_verify_finalize(astreamer *streamer);
static void astreamer_verify_free(astreamer *streamer);

const astreamer_ops astreamer_verify_ops = {
	.content = astreamer_verify_content,
	.finalize = astreamer_verify_finalize,
	.free = astreamer_verify_free
};

/*
 * Create a astreamer that can verifies content of a tar data.
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

	return &streamer->base;
}

/*
 * It verifies the tar member and the backup system identifier against the
 * backup manifest. Additionally, it performs checksum verification if enabled.
 */
static void
astreamer_verify_content(astreamer *streamer,
						 astreamer_member *member, const char *data,
						 int len, astreamer_archive_context context)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;

	Assert(context != ASTREAMER_UNKNOWN);

	switch (context)
	{
		case ASTREAMER_MEMBER_HEADER:
			if (!member->is_directory && !member->is_link &&
				!should_ignore_relpath(mystreamer->context, member->pathname))
			{
				manifest_file *m;

				/*
				 * The backup_manifest stores a relative path to the base
				 * directory for files belong tablespace, whereas
				 * <tablespaceoid>.tar doesn't. Prepare the required path,
				 * otherwise, the manfiest entry verification will fail.
				 */
				if (OidIsValid(mystreamer->tblspc_oid))
				{
					char		temp[MAXPGPATH];

					/* Copy original name at temporary space */
					memcpy(temp, member->pathname, MAXPGPATH);

					snprintf(member->pathname, MAXPGPATH, "%s/%d/%s",
							 "pg_tblspc", mystreamer->tblspc_oid, temp);
				}

				/*
				 * Do the manifest entry and checksum verification right away
				 * since the archive formal wouldn't have random access to
				 * files like a normal backup directory, where these happen at
				 * different points.
				 */
				m = verify_manifest_entry(mystreamer->context, member->pathname,
										  member->size);
				mystreamer->mfile = (void *) m;

				/*
				 * Prepare for checksum and manifest system identifier
				 * verification.
				 *
				 * We could have this and subsequent check for the system
				 * identifier directly while receiving contents.  However,
				 * since contents are received in multiple iterations, this
				 * would result in these lengthy checks being performed
				 * multiple times. Instead, having a single flag would be more
				 * efficient.
				 */
				mystreamer->verify_checksums = (!skip_checksums && m != NULL &&
												should_verify_checksum(m));

				/*
				 * Validate the manifest system identifier, which isn't
				 * present in manifest version 1.  This validation should be
				 * carried out only if the manifest entry validation is
				 * completed without any errors.
				 */
				mystreamer->verify_sysid =
					(mystreamer->context->manifest->version != 1 &&
					 strcmp(member->pathname, "global/pg_control") == 0 &&
					 m != NULL && m->matched && !m->bad);

			}
			break;

		case ASTREAMER_MEMBER_CONTENTS:

			/* Do the checksum verification */
			if (mystreamer->verify_checksums)
			{
				manifest_file *m = mystreamer->mfile;
				char	   *relpath = m->pathname;

				/* If we were first time for this file */
				if (!mystreamer->checksum_ctx)
				{
					mystreamer->checksum_ctx = pg_malloc(sizeof(pg_checksum_context));

					if (pg_checksum_init(mystreamer->checksum_ctx, m->checksum_type) < 0)
					{
						report_backup_error(mystreamer->context,
											"%s: could not initialize checksum of file \"%s\"",
											mystreamer->archive_name, relpath);
						mystreamer->verify_checksums = false;
						return;
					}
				}

				/* Compute and do the checksum validation */
				mystreamer->verify_checksums =
					verify_content_checksum(mystreamer->context,
											mystreamer->checksum_ctx,
											m, (uint8 *) data, len,
											&mystreamer->received_bytes);
			}

			/* Do the manifest system identifier verification */
			if (mystreamer->verify_sysid)
			{
				ControlFileData control_file;
				uint64		manifest_system_identifier;
				pg_crc32c	crc;
				bool		crc_ok;

				/* Should be here only for control file */
				Assert(strcmp(member->pathname, "global/pg_control") == 0);
				Assert(mystreamer->context->manifest->version != 1);

				/* Should have whole control file data. */
				if (!astreamer_buffer_until(streamer, &data, &len,
											sizeof(ControlFileData)))
					return;

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

				manifest_system_identifier =
					mystreamer->context->manifest->system_identifier;

				verify_control_file_data(&control_file, member->pathname,
										 crc_ok, manifest_system_identifier);
			}
			break;

		case ASTREAMER_MEMBER_TRAILER:
			if (mystreamer->checksum_ctx)
				pfree(mystreamer->checksum_ctx);
			mystreamer->checksum_ctx = NULL;
			mystreamer->mfile = NULL;
			mystreamer->received_bytes = 0;
			mystreamer->verify_checksums = false;
			mystreamer->verify_sysid = false;
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
	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
}
