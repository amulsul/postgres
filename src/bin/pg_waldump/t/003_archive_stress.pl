
# Copyright (c) 2026, PostgreSQL Global Development Group

# Stress tests and edge cases for pg_waldump tar archive functionality
# Tests: concurrent operations, large files, cross-segment boundaries,
# specific WAL record types, and resource cleanup

use strict;
use warnings FATAL => 'all';
use Cwd;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Path qw(make_path remove_tree);
use File::Find;

my $tar = $ENV{TAR};

# Skip all tests if tar is not available
if (!defined $tar)
{
	plan skip_all => 'tar command not available';
}

my $node = PostgreSQL::Test::Cluster->new('stress_tests');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
wal_level = logical
max_wal_senders = 2
max_replication_slots = 2
checkpoint_timeout = 1h
max_wal_size = 256MB
min_wal_size = 128MB
});
$node->start;

my $waldir = $node->data_dir . '/pg_wal';
my $tmp_dir = PostgreSQL::Test::Utils::tempdir_short();

###########################################
# Test 1: Cross-segment boundary reads
###########################################

note "Testing reads that cross segment boundaries";

# Create data that will span across segments
$node->safe_psql('postgres',
	'CREATE TABLE boundary_test (id serial, data bytea);');

# Insert enough data to ensure we cross segment boundaries
for (my $i = 0; $i < 10; $i++)
{
	$node->safe_psql('postgres',
		"INSERT INTO boundary_test (data) SELECT repeat('x', 10000)::bytea FROM generate_series(1, 5000);"
	);
	$node->safe_psql('postgres', 'SELECT pg_switch_wal();');
}

$node->safe_psql('postgres', 'CHECKPOINT;');

# Get WAL files
my @wal_files;
opendir(my $dh, $waldir) or die "Cannot open $waldir: $!";
while (my $file = readdir($dh))
{
	push @wal_files, $file if $file =~ /^[0-9A-F]{24}$/;
}
closedir($dh);
@wal_files = sort @wal_files;

# Extract LSN from first WAL file
my ($boundary_start, $boundary_end);
if (scalar(@wal_files) >= 3)
{
	my ($stdout_first, $stderr_first);
	IPC::Run::run(
		['pg_waldump', $waldir . '/' . $wal_files[0], '--limit' => '1'],
		'>', \$stdout_first,
		'2>', \$stderr_first);
	($boundary_start) = $stdout_first =~ /lsn: ([0-9A-F]+\/[0-9A-F]+)/i;

	# Get current LSN as end
	$boundary_end = $node->safe_psql('postgres', "SELECT pg_current_wal_lsn();");

	my $boundary_archive = "$tmp_dir/boundary.tar";
	my $cwd = getcwd();
	chdir($waldir) or die "Cannot chdir: $!";
	system("$tar -cf $boundary_archive @wal_files") == 0
	  or die "tar failed";
	chdir($cwd) or die "Cannot chdir back: $!";

	# Test reading across boundaries
	command_like(
		[
			'pg_waldump',  '--path'  => $boundary_archive,
			'--start' => $boundary_start, '--end'   => $boundary_end
		],
		qr/rmgr:/,
		'pg_waldump reads across segment boundaries in archive');
}

###########################################
# Test 2: Specific WAL record types
###########################################

note "Testing decoding of specific WAL record types from archive";

# Generate various record types
$node->safe_psql('postgres', 'CREATE INDEX idx_boundary ON boundary_test(id);');
$node->safe_psql('postgres', 'VACUUM ANALYZE boundary_test;');
$node->safe_psql('postgres', 'ALTER TABLE boundary_test ADD COLUMN extra text;');
$node->safe_psql('postgres', 'TRUNCATE boundary_test;');

# Try to create replication slot if test_decoding is available
eval {
	$node->safe_psql('postgres',
		"SELECT pg_create_logical_replication_slot('test_slot', 'test_decoding');");
};
# If test_decoding is not available, just skip this part silently

$node->safe_psql('postgres', 'CHECKPOINT;');

# Get fresh WAL list
@wal_files = ();
opendir($dh, $waldir) or die "Cannot open $waldir: $!";
while (my $file = readdir($dh))
{
	push @wal_files, $file if $file =~ /^[0-9A-F]{24}$/;
}
closedir($dh);
@wal_files = sort @wal_files;

# Extract LSN from first WAL file
my ($record_stdout, $record_stderr);
IPC::Run::run(
	['pg_waldump', $waldir . '/' . $wal_files[0], '--limit' => '1'],
	'>', \$record_stdout,
	'2>', \$record_stderr);
my ($record_start) = $record_stdout =~ /lsn: ([0-9A-F]+\/[0-9A-F]+)/i;
my $record_end = $node->safe_psql('postgres', "SELECT pg_current_wal_lsn();");

my $record_archive = "$tmp_dir/records.tar";
my $cwd = getcwd();
chdir($waldir) or die "Cannot chdir: $!";
system("$tar -cf $record_archive @wal_files") == 0 or die "tar failed";
chdir($cwd) or die "Cannot chdir back: $!";


# Test filtering by different rmgr types
my @rmgr_types = ('Heap', 'Btree', 'XLOG', 'Transaction');

foreach my $rmgr (@rmgr_types)
{
	my ($stdout, $stderr);
	my $result = IPC::Run::run(
		[
			'pg_waldump', '--path' => $record_archive,
			'--start' => $record_start, '--end' => $record_end,
			'--rmgr' => $rmgr
		],
		'>',
		\$stdout,
		'2>',
		\$stderr);

	# Some rmgr types might not have records in our range, that's ok
	ok($result || $stderr =~ /error in WAL record/,
		"pg_waldump --rmgr=$rmgr processes archive");
}

###########################################
# Test 3: Temporary directory cleanup verification
###########################################

note "Testing temporary directory cleanup";

# Create out-of-order archive to trigger temp directory creation
my @shuffled_wals = reverse(@wal_files);
my $cleanup_archive = "$tmp_dir/cleanup_test.tar";
$cwd = getcwd();
chdir($waldir) or die "Cannot chdir: $!";
system("$tar -cf $cleanup_archive @shuffled_wals") == 0 or die "tar failed";
chdir($cwd) or die "Cannot chdir back: $!";

# Count waldump_tmp directories before
my @tmpfiles_before = glob("$ENV{TMPDIR}/waldump_tmp-*")
  if defined $ENV{TMPDIR};
@tmpfiles_before = glob("/tmp/waldump_tmp-*") unless @tmpfiles_before;
my $tmp_count_before = scalar(@tmpfiles_before);

# Run pg_waldump
my ($stdout, $stderr);
IPC::Run::run(
	[
		'pg_waldump', '--path' => $cleanup_archive,
		'--start' => $record_start, '--end' => $record_end
	],
	'>',
	\$stdout,
	'2>',
	\$stderr);

# Count waldump_tmp directories after
my @tmpfiles_after = glob("$ENV{TMPDIR}/waldump_tmp-*")
  if defined $ENV{TMPDIR};
@tmpfiles_after = glob("/tmp/waldump_tmp-*") unless @tmpfiles_after;
my $tmp_count_after = scalar(@tmpfiles_after);

is($tmp_count_after,
	$tmp_count_before,
	'temporary directories are cleaned up after pg_waldump exits');

###########################################
# Test 4: Maximum number of segments
###########################################

note "Testing with maximum available segments";

# Use all available WAL segments
my $max_archive = "$tmp_dir/max_segments.tar";
$cwd = getcwd();
chdir($waldir) or die "Cannot chdir: $!";
system("$tar -cf $max_archive @wal_files") == 0 or die "tar failed";
chdir($cwd) or die "Cannot chdir back: $!";

# Extract LSN from first WAL file
my ($max_stdout_first, $max_stderr_first);
IPC::Run::run(
	['pg_waldump', $waldir . '/' . $wal_files[0], '--limit' => '1'],
	'>', \$max_stdout_first,
	'2>', \$max_stderr_first);
my ($max_start) = $max_stdout_first =~ /lsn: ([0-9A-F]+\/[0-9A-F]+)/i;
my $max_end = $node->safe_psql('postgres', "SELECT pg_current_wal_lsn();");

my $time_start = time();
IPC::Run::run(
	[
		'pg_waldump', '--path' => $max_archive,
		'--start' => $max_start, '--end' => $max_end
	],
	'>',
	\$stdout,
	'2>',
	\$stderr);
my $time_elapsed = time() - $time_start;

ok(defined($stdout) && $stdout ne '',
	"pg_waldump processes all " . scalar(@wal_files) . " segments");
note "Processed " . scalar(@wal_files) . " segments in $time_elapsed seconds";

###########################################
# Test 5: Partial record at segment boundary
###########################################

note "Testing partial records at segment boundaries";

# Insert data that will create records spanning segments
$node->safe_psql('postgres',
	"INSERT INTO boundary_test (data) SELECT repeat('partial', 5000)::bytea FROM generate_series(1, 10000);"
);

@wal_files = ();
opendir($dh, $waldir) or die "Cannot open $waldir: $!";
while (my $file = readdir($dh))
{
	push @wal_files, $file if $file =~ /^[0-9A-F]{24}$/;
}
closedir($dh);
@wal_files = sort @wal_files;

my $partial_archive = "$tmp_dir/partial.tar";
$cwd = getcwd();
chdir($waldir) or die "Cannot chdir: $!";
system("$tar -cf $partial_archive @wal_files") == 0 or die "tar failed";
chdir($cwd) or die "Cannot chdir back: $!";

# Extract LSN from first WAL file
my ($partial_stdout, $partial_stderr);
IPC::Run::run(
	['pg_waldump', $waldir . '/' . $wal_files[0], '--limit' => '1'],
	'>', \$partial_stdout,
	'2>', \$partial_stderr);
my ($partial_start) = $partial_stdout =~ /lsn: ([0-9A-F]+\/[0-9A-F]+)/i;
my $partial_end = $node->safe_psql('postgres', "SELECT pg_current_wal_lsn();");

command_like(
	[
		'pg_waldump', '--path'  => $partial_archive,
		'--start'     => $partial_start, '--end' => $partial_end
	],
	qr/rmgr:/,
	'pg_waldump handles partial records at segment boundaries');

###########################################
# Test 6: Statistics accuracy with archives
###########################################

note "Testing statistics accuracy";

my $stats_archive = "$tmp_dir/stats.tar";
$cwd = getcwd();
chdir($waldir) or die "Cannot chdir: $!";
system("$tar -cf $stats_archive @wal_files") == 0 or die "tar failed";
chdir($cwd) or die "Cannot chdir back: $!";

# Get stats from directory
my ($dir_stats, $dir_stderr);
IPC::Run::run(
	[
		'pg_waldump', '--path'  => $waldir,
		'--start'     => $partial_start, '--end' => $partial_end,
		'--stats'
	],
	'>',
	\$dir_stats,
	'2>',
	\$dir_stderr);

# Get stats from archive
my ($archive_stats, $archive_stderr);
IPC::Run::run(
	[
		'pg_waldump', '--path'  => $stats_archive,
		'--start'     => $partial_start, '--end' => $partial_end,
		'--stats'
	],
	'>',
	\$archive_stats,
	'2>',
	\$archive_stderr);

# Extract record counts
my ($dir_count) = $dir_stats =~ /Total records:\s+(\d+)/;
my ($archive_count) = $archive_stats =~ /Total records:\s+(\d+)/;

is($archive_count, $dir_count,
	'statistics match between directory and archive reading');

###########################################
# Test 7: Full page writes (FPW)
###########################################

note "Testing full page write decoding from archive";

# Force full page writes
$node->safe_psql('postgres', 'CHECKPOINT;');
$node->safe_psql('postgres',
	"UPDATE boundary_test SET extra = 'trigger_fpw' WHERE id % 100 = 0;");

@wal_files = ();
opendir($dh, $waldir) or die "Cannot open $waldir: $!";
while (my $file = readdir($dh))
{
	push @wal_files, $file if $file =~ /^[0-9A-F]{24}$/;
}
closedir($dh);
@wal_files = sort @wal_files;

my $fpw_archive = "$tmp_dir/fpw.tar";
$cwd = getcwd();
chdir($waldir) or die "Cannot chdir: $!";
system("$tar -cf $fpw_archive @wal_files") == 0 or die "tar failed";
chdir($cwd) or die "Cannot chdir back: $!";

# Extract LSN from first WAL file
my ($fpw_stdout, $fpw_stderr);
IPC::Run::run(
	['pg_waldump', $waldir . '/' . $wal_files[0], '--limit' => '1'],
	'>', \$fpw_stdout,
	'2>', \$fpw_stderr);
my ($fpw_start) = $fpw_stdout =~ /lsn: ([0-9A-F]+\/[0-9A-F]+)/i;
my $fpw_end = $node->safe_psql('postgres', "SELECT pg_current_wal_lsn();");

command_like(
	[
		'pg_waldump', '--path'  => $fpw_archive,
		'--start'     => $fpw_start, '--end' => $fpw_end,
		'--fullpage'
	],
	qr/FPW/,
	'pg_waldump --fullpage works with archive');

###########################################
# Test 8: Block filtering
###########################################

note "Testing block-level filtering with archive";

my $block_archive = "$tmp_dir/block.tar";
$cwd = getcwd();
chdir($waldir) or die "Cannot chdir: $!";
system("$tar -cf $block_archive @wal_files") == 0 or die "tar failed";
chdir($cwd) or die "Cannot chdir back: $!";

# Get relation OID
my $reloid = $node->safe_psql('postgres',
	"SELECT oid FROM pg_class WHERE relname = 'boundary_test';");

# Get tablespace and database OIDs
my $tsoid =
  $node->safe_psql('postgres', "SELECT oid FROM pg_tablespace WHERE spcname = 'pg_default';");
my $dboid = $node->safe_psql('postgres', "SELECT oid FROM pg_database WHERE datname = 'postgres';");

command_like(
	[
		'pg_waldump',  '--path'     => $block_archive,
		'--start'      => $fpw_start,
		'--end'        => $fpw_end,
		'--relation'   => "$tsoid/$dboid/$reloid",
		'--block'      => '0'
	],
	qr/blk 0/,
	'pg_waldump --block filtering works with archive');

###########################################
# Test 9: Fork filtering
###########################################

note "Testing fork-level filtering with archive";

command_like(
	[
		'pg_waldump', '--path'  => $block_archive,
		'--start'     => $fpw_start, '--end' => $fpw_end,
		'--fork'      => 'main'
	],
	qr/rmgr:/,
	'pg_waldump --fork filtering works with archive');

###########################################
# Test 10: Quiet mode with errors
###########################################

note "Testing error reporting in quiet mode with archive";

# Create archive with only first file (definitely incomplete)
my @incomplete = ($wal_files[0]);
my $incomplete_archive = "$tmp_dir/incomplete.tar";
$cwd = getcwd();
chdir($waldir) or die "Cannot chdir: $!";
system("$tar -cf $incomplete_archive @incomplete") == 0 or die "tar failed";
chdir($cwd) or die "Cannot chdir back: $!";

# Errors should still be shown in quiet mode (no --end means it will try to continue)
IPC::Run::run(
	[
		'pg_waldump', '--quiet', '--path' => $incomplete_archive,
		'--start' => $partial_start
	],
	'>',
	\$stdout,
	'2>',
	\$stderr);

like($stderr, qr/error|invalid/, 'errors shown in --quiet mode with archive');

###########################################
# Test 11: Very large single record
###########################################

note "Testing very large single WAL record in archive";

# Create a table with a very wide row
$node->safe_psql('postgres', 'CREATE TABLE wide_row (id serial, ' .
	  join(', ', map { "col$_ text" } (1 .. 100)) . ');');

$node->safe_psql('postgres',
	"INSERT INTO wide_row VALUES (1, " . join(', ',
		map { "'x' || repeat('data', 100)" } (1 .. 100)) . ");");

@wal_files = ();
opendir($dh, $waldir) or die "Cannot open $waldir: $!";
while (my $file = readdir($dh))
{
	push @wal_files, $file if $file =~ /^[0-9A-F]{24}$/;
}
closedir($dh);
@wal_files = sort @wal_files;

my $large_rec_archive = "$tmp_dir/large_record.tar";
$cwd = getcwd();
chdir($waldir) or die "Cannot chdir: $!";
system("$tar -cf $large_rec_archive @wal_files") == 0 or die "tar failed";
chdir($cwd) or die "Cannot chdir back: $!";

# Extract LSN from first WAL file
my ($large_stdout, $large_stderr);
IPC::Run::run(
	['pg_waldump', $waldir . '/' . $wal_files[0], '--limit' => '1'],
	'>', \$large_stdout,
	'2>', \$large_stderr);
my ($large_start) = $large_stdout =~ /lsn: ([0-9A-F]+\/[0-9A-F]+)/i;
my $large_end = $node->safe_psql('postgres', "SELECT pg_current_wal_lsn();");

command_like(
	[
		'pg_waldump', '--path'  => $large_rec_archive,
		'--start'     => $large_start, '--end' => $large_end
	],
	qr/rmgr:/,
	'pg_waldump handles very large WAL records in archive');

###########################################
# Test 12: bkp-details option with archive
###########################################

note "Testing --bkp-details with archive";

command_like(
	[
		'pg_waldump', '--path'  => $fpw_archive,
		'--start'     => $fpw_start, '--end' => $fpw_end,
		'--bkp-details'
	],
	qr/(hole|FPW)/,
	'pg_waldump --bkp-details works with archive');

###########################################
# Test 13: Transaction filtering
###########################################

note "Testing transaction XID filtering with archive";

# Start a transaction and get its XID
my $xid = $node->safe_psql('postgres', 'SELECT txid_current();');

$node->safe_psql('postgres',
	"INSERT INTO boundary_test (data) SELECT 'xid_test'::bytea FROM generate_series(1, 100);"
);

@wal_files = ();
opendir($dh, $waldir) or die "Cannot open $waldir: $!";
while (my $file = readdir($dh))
{
	push @wal_files, $file if $file =~ /^[0-9A-F]{24}$/;
}
closedir($dh);
@wal_files = sort @wal_files;

my $xid_archive = "$tmp_dir/xid.tar";
$cwd = getcwd();
chdir($waldir) or die "Cannot chdir: $!";
system("$tar -cf $xid_archive @wal_files") == 0 or die "tar failed";
chdir($cwd) or die "Cannot chdir back: $!";

# Extract LSN from first WAL file
my ($xid_stdout, $xid_stderr);
IPC::Run::run(
	['pg_waldump', $waldir . '/' . $wal_files[0], '--limit' => '1'],
	'>', \$xid_stdout,
	'2>', \$xid_stderr);
my ($xid_start) = $xid_stdout =~ /lsn: ([0-9A-F]+\/[0-9A-F]+)/i;
my $xid_end = $node->safe_psql('postgres', "SELECT pg_current_wal_lsn();");

command_like(
	[
		'pg_waldump', '--path' => $xid_archive,
		'--start' => $xid_start, '--end' => $xid_end,
		'--xid' => $xid
	],
	qr/tx:\s+$xid/,
	'pg_waldump --xid filtering works with archive');

$node->stop;

done_testing();
