
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use Cwd;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Path qw(rmtree);
use List::Util qw(shuffle);

my $tar = $ENV{TAR};

program_help_ok('pg_waldump');
program_version_ok('pg_waldump');
program_options_handling_ok('pg_waldump');

# wrong number of arguments
command_fails_like([ 'pg_waldump', ], qr/error: no arguments/,
	'no arguments');
command_fails_like(
	[ 'pg_waldump', 'foo', 'bar', 'baz' ],
	qr/error: too many command-line arguments/,
	'too many arguments');

# invalid option arguments
command_fails_like(
	[ 'pg_waldump', '--block' => 'bad' ],
	qr/error: invalid block number/,
	'invalid block number');
command_fails_like(
	[ 'pg_waldump', '--fork' => 'bad' ],
	qr/error: invalid fork name/,
	'invalid fork name');
command_fails_like(
	[ 'pg_waldump', '--limit' => 'bad' ],
	qr/error: invalid value/,
	'invalid limit');
command_fails_like(
	[ 'pg_waldump', '--relation' => 'bad' ],
	qr/error: invalid relation/,
	'invalid relation specification');
command_fails_like(
	[ 'pg_waldump', '--rmgr' => 'bad' ],
	qr/error: resource manager .* does not exist/,
	'invalid rmgr name');
command_fails_like(
	[ 'pg_waldump', '--start' => 'bad' ],
	qr/error: invalid WAL location/,
	'invalid start LSN');
command_fails_like(
	[ 'pg_waldump', '--end' => 'bad' ],
	qr/error: invalid WAL location/,
	'invalid end LSN');

# rmgr list: If you add one to the list, consider also adding a test
# case exercising the new rmgr below.
command_like(
	[ 'pg_waldump', '--rmgr=list' ], qr/^XLOG
Transaction
Storage
CLOG
Database
Tablespace
MultiXact
RelMap
Standby
Heap2
Heap
Btree
Hash
Gin
Gist
Sequence
SPGist
BRIN
CommitTs
ReplicationOrigin
Generic
LogicalMessage$/,
	'rmgr list');


my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
checkpoint_timeout = 1h

# for standbydesc
archive_mode=on
archive_command=''

# for XLOG_HEAP_TRUNCATE
wal_level=logical
});
$node->start;

my ($start_lsn, $start_walfile) = split /\|/,
  $node->safe_psql('postgres',
	q{SELECT pg_current_wal_insert_lsn(), pg_walfile_name(pg_current_wal_insert_lsn())}
  );

$node->safe_psql(
	'postgres', q{
-- heap, btree, hash, sequence
CREATE TABLE t1 (a int GENERATED ALWAYS AS IDENTITY, b text);
CREATE INDEX i1a ON t1 USING btree (a);
CREATE INDEX i1b ON t1 USING hash (b);
INSERT INTO t1 VALUES (default, 'one'), (default, 'two');
DELETE FROM t1 WHERE b = 'one';
TRUNCATE t1;

-- abort
START TRANSACTION;
INSERT INTO t1 VALUES (default, 'three');
ROLLBACK;

-- unlogged/init fork
CREATE UNLOGGED TABLE t2 (x int);
CREATE INDEX i2 ON t2 USING btree (x);
INSERT INTO t2 SELECT generate_series(1, 10);

-- gin
CREATE TABLE gin_idx_tbl (id bigserial PRIMARY KEY, data jsonb);
CREATE INDEX gin_idx ON gin_idx_tbl USING gin (data);
INSERT INTO gin_idx_tbl
    WITH random_json AS (
        SELECT json_object_agg(key, trunc(random() * 10)) as json_data
            FROM unnest(array['a', 'b', 'c']) as u(key))
          SELECT generate_series(1,500), json_data FROM random_json;

-- gist, spgist
CREATE TABLE gist_idx_tbl (p point);
CREATE INDEX gist_idx ON gist_idx_tbl USING gist (p);
CREATE INDEX spgist_idx ON gist_idx_tbl USING spgist (p);
INSERT INTO gist_idx_tbl (p) VALUES (point '(1, 1)'), (point '(3, 2)'), (point '(6, 3)');

-- brin
CREATE TABLE brin_idx_tbl (col1 int, col2 text, col3 text );
CREATE INDEX brin_idx ON brin_idx_tbl USING brin (col1, col2, col3) WITH (autosummarize=on);
INSERT INTO brin_idx_tbl SELECT generate_series(1, 10000), 'dummy', 'dummy';
UPDATE brin_idx_tbl SET col2 = 'updated' WHERE col1 BETWEEN 1 AND 5000;
SELECT brin_summarize_range('brin_idx', 0);
SELECT brin_desummarize_range('brin_idx', 0);

VACUUM;

-- logical message
SELECT pg_logical_emit_message(true, 'foo', 'bar');

-- relmap
VACUUM FULL pg_authid;

-- database
CREATE DATABASE d1;
DROP DATABASE d1;
});

my $tblspc_path = PostgreSQL::Test::Utils::tempdir_short();

$node->safe_psql(
	'postgres', qq{
CREATE TABLESPACE ts1 LOCATION '$tblspc_path';
DROP TABLESPACE ts1;
});

my ($end_lsn, $end_walfile) = split /\|/,
  $node->safe_psql('postgres',
	q{SELECT pg_current_wal_insert_lsn(), pg_walfile_name(pg_current_wal_insert_lsn())}
  );

my $default_ts_oid = $node->safe_psql('postgres',
	q{SELECT oid FROM pg_tablespace WHERE spcname = 'pg_default'});
my $postgres_db_oid = $node->safe_psql('postgres',
	q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
my $rel_t1_oid = $node->safe_psql('postgres',
	q{SELECT oid FROM pg_class WHERE relname = 't1'});
my $rel_i1a_oid = $node->safe_psql('postgres',
	q{SELECT oid FROM pg_class WHERE relname = 'i1a'});

$node->stop;


# various ways of specifying WAL range
command_fails_like(
	[ 'pg_waldump', 'foo', 'bar' ],
	qr/error: could not locate WAL file "foo"/,
	'start file not found');
command_like([ 'pg_waldump', $node->data_dir . '/pg_wal/' . $start_walfile ],
	qr/./, 'runs with start segment specified');
command_fails_like(
	[ 'pg_waldump', $node->data_dir . '/pg_wal/' . $start_walfile, 'bar' ],
	qr/error: could not open file "bar"/,
	'end file not found');
command_like(
	[
		'pg_waldump',
		$node->data_dir . '/pg_wal/' . $start_walfile,
		$node->data_dir . '/pg_wal/' . $end_walfile
	],
	qr/./,
	'runs with start and end segment specified');
command_like(
	[
		'pg_waldump', '--quiet',
		$node->data_dir . '/pg_wal/' . $start_walfile
	],
	qr/^$/,
	'no output with --quiet option');

# Test for: Display a message that we're skipping data if `from`
# wasn't a pointer to the start of a record.
{
	# Construct a new LSN that is one byte past the original
	# start_lsn.
	my ($part1, $part2) = split qr{/}, $start_lsn;
	my $lsn2 = hex $part2;
	$lsn2++;
	my $new_start = sprintf("%s/%X", $part1, $lsn2);

	my ($stdout, $stderr);

	my $result = IPC::Run::run [
		'pg_waldump',
		'--start' => $new_start,
		$node->data_dir . '/pg_wal/' . $start_walfile
	  ],
	  '>' => \$stdout,
	  '2>' => \$stderr;
	ok($result, "runs with start segment and start LSN specified");
	like($stderr, qr/first record is after/, 'info message printed');
}


# Helper function to test various options.  Pass options as arguments.
# Output lines are returned as array.
sub test_pg_waldump
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($path, @opts) = @_;

	my ($stdout, $stderr);

	my $result = IPC::Run::run [
		'pg_waldump',
		'--start' => $start_lsn,
		'--end' => $end_lsn,
		'--path' => $path,
		@opts
	  ],
	  '>' => \$stdout,
	  '2>' => \$stderr;
	ok($result, "pg_waldump @opts: runs ok");
	is($stderr, '', "pg_waldump @opts: no stderr");
	my @lines = split /\n/, $stdout;
	ok(@lines > 0, "pg_waldump @opts: some lines are output");
	return @lines;
}

# Create a tar archive, shuffling the file order
sub generate_archive
{
	my ($archive, $directory, $compression_flags) = @_;

	my @files;
	opendir my $dh, $directory or die "opendir: $!";
	while (my $entry = readdir $dh) {
		# Skip '.' and '..'
		next if $entry eq '.' || $entry eq '..';
		push @files, $entry;
	}
	closedir $dh;

	@files = shuffle @files;

	# move into the WAL directory before archiving files
	my $cwd = getcwd;
	chdir($directory) || die "chdir: $!";
	command_ok([$tar, $compression_flags, $archive, @files]);
	chdir($cwd) || die "chdir: $!";
}

my $tmp_dir = PostgreSQL::Test::Utils::tempdir_short();

my @scenario = (
	{
		'path' => $node->data_dir,
		'is_archive' => 0,
		'enabled' => 1
	},
	{
		'path' => "$tmp_dir/pg_wal.tar",
		'compression_method' => 'none',
		'compression_flags' => '-cf',
		'is_archive' => 1,
		'enabled' => 1
	},
	{
		'path' => "$tmp_dir/pg_wal.tar.gz",
		'compression_method' => 'gzip',
		'compression_flags' => '-czf',
		'is_archive' => 1,
		'enabled' => check_pg_config("#define HAVE_LIBZ 1")
	});

for my $scenario (@scenario)
{
	my $path = $scenario->{'path'};

	SKIP:
	{
		skip "tar command is not available", 3
		  if !defined $tar;
		skip "$scenario->{'compression_method'} compression not supported by this build", 3
		  if !$scenario->{'enabled'} && $scenario->{'is_archive'};

		  # create pg_wal archive
		  if ($scenario->{'is_archive'})
		  {
			  generate_archive($path,
				  $node->data_dir . '/pg_wal',
				  $scenario->{'compression_flags'});
		  }

		command_fails_like(
			[ 'pg_waldump', '--path' => $path ],
			qr/error: no start WAL location given/,
			'path option requires start location');
		command_like(
			[
				'pg_waldump',
				'--path' => $path,
				'--start' => $start_lsn,
				'--end' => $end_lsn,
			],
			qr/./,
			'runs with path option and start and end locations');
		command_fails_like(
			[
				'pg_waldump',
				'--path' => $path,
				'--start' => $start_lsn,
			],
			qr/error: error in WAL record at/,
			'falling off the end of the WAL results in an error');

		# pg_waldump fails to clear its temporary directory after an error.
		rmtree($tmp_dir.'/pg_waldump_tmp_dir/') if $scenario->{'is_archive'};

		command_fails_like(
			[
				'pg_waldump', '--quiet',
				'--path' => $path,
				'--start' => $start_lsn
			],
			qr/error: error in WAL record at/,
			'errors are shown with --quiet');

		# pg_waldump fails to clear its temporary directory after an error.
		rmtree($tmp_dir.'/pg_waldump_tmp_dir/') if $scenario->{'is_archive'};

		my @lines;
		@lines = test_pg_waldump($path);
		is(grep(!/^rmgr: \w/, @lines), 0, 'all output lines are rmgr lines');

		@lines = test_pg_waldump($path, '--limit' => 6);
		is(@lines, 6, 'limit option observed');

		@lines = test_pg_waldump($path, '--fullpage');
		is(grep(!/^rmgr:.*\bFPW\b/, @lines), 0, 'all output lines are FPW');

		@lines = test_pg_waldump($path, '--stats');
		like($lines[0], qr/WAL statistics/, "statistics on stdout");
		is(grep(/^rmgr:/, @lines), 0, 'no rmgr lines output');

		@lines = test_pg_waldump($path, '--stats=record');
		like($lines[0], qr/WAL statistics/, "statistics on stdout");
		is(grep(/^rmgr:/, @lines), 0, 'no rmgr lines output');

		@lines = test_pg_waldump($path, '--rmgr' => 'Btree');
		is(grep(!/^rmgr: Btree/, @lines), 0, 'only Btree lines');

		@lines = test_pg_waldump($path, '--fork' => 'init');
		is(grep(!/fork init/, @lines), 0, 'only init fork lines');

		@lines = test_pg_waldump($path,
			'--relation' => "$default_ts_oid/$postgres_db_oid/$rel_t1_oid");
		is(grep(!/rel $default_ts_oid\/$postgres_db_oid\/$rel_t1_oid/, @lines),
			0, 'only lines for selected relation');

		@lines = test_pg_waldump($path,
			'--relation' => "$default_ts_oid/$postgres_db_oid/$rel_i1a_oid",
			'--block' => 1);
		is(grep(!/\bblk 1\b/, @lines), 0, 'only lines for selected block');

		# Cleanup.
		unlink $path if $scenario->{'is_archive'};
	}
}

done_testing();
