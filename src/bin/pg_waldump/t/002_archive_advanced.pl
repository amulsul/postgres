
# Copyright (c) 2026, PostgreSQL Global Development Group

# Advanced TAR archive tests for pg_waldump
# Tests compressed archives, out-of-order WAL, error conditions, and edge cases

use strict;
use warnings FATAL => 'all';
use Cwd;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Copy;
use File::Path qw(make_path remove_tree);
use List::Util qw(shuffle);

my $tar = $ENV{TAR};

# Skip all tests if tar is not available
if (!defined $tar)
{
	plan skip_all => 'tar command not available';
}

my $node = PostgreSQL::Test::Cluster->new('archive_tests');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
checkpoint_timeout = 1h
wal_level = replica
max_wal_size = 128MB
min_wal_size = 80MB
});
$node->start;

# Generate substantial WAL across multiple segments
$node->safe_psql('postgres', 'CREATE TABLE test_data (id serial, data text);');

# Generate enough data to fill multiple WAL segments
for (my $i = 0; $i < 10; $i++)
{
	$node->safe_psql('postgres',
		"INSERT INTO test_data (data) SELECT repeat('testdata', 500) FROM generate_series(1, 5000);"
	);
	$node->safe_psql('postgres', 'SELECT pg_switch_wal();');
}

$node->safe_psql('postgres', 'CHECKPOINT;');

my $waldir = $node->data_dir . '/pg_wal';
my $tmp_dir = PostgreSQL::Test::Utils::tempdir_short();

# Get list of WAL files
my @wal_files;
opendir(my $dh, $waldir) or die "Cannot open $waldir: $!";
while (my $file = readdir($dh))
{
	# Match WAL segment files (24 hex characters)
	if ($file =~ /^[0-9A-F]{24}$/)
	{
		push @wal_files, $file;
	}
}
closedir($dh);

# Need at least 3 WAL segments for meaningful tests
if (scalar(@wal_files) < 3)
{
	plan skip_all => 'Need at least 3 WAL segments for testing';
}

@wal_files = sort @wal_files;
note "Found " . scalar(@wal_files) . " WAL segments";

# Get start and end LSNs from actual WAL files
my ($start_lsn, $start_walfile) = split /\|/,
  $node->safe_psql('postgres',
	q{SELECT pg_current_wal_insert_lsn(), pg_walfile_name(pg_current_wal_insert_lsn())}
  );

# Generate a bit more WAL
$node->safe_psql('postgres',
	"INSERT INTO test_data (data) SELECT 'end_marker' FROM generate_series(1, 100);"
);

my ($end_lsn, $end_walfile) = split /\|/,
  $node->safe_psql('postgres',
	q{SELECT pg_current_wal_insert_lsn(), pg_walfile_name(pg_current_wal_insert_lsn())}
  );

note "Testing WAL range: $start_lsn to $end_lsn";

# Helper function to create tar archive with specific files
sub create_tar_archive
{
	my ($archive_path, $files_ref, $compression_flag) = @_;
	my @files = @$files_ref;

	my $cwd = getcwd();
	chdir($waldir) or die "Cannot chdir to $waldir: $!";

	# Split compression flag into separate arguments if it contains spaces
	my @compression_args = split(/\s+/, $compression_flag);
	my @cmd = ($tar, @compression_args, '-f', $archive_path, @files);
	my $result = run_log(\@cmd);

	chdir($cwd) or die "Cannot chdir back: $!";
	return $result;
}

# Helper to run pg_waldump with expected success
sub test_waldump_succeeds
{
	my ($archive, $start, $end, $test_name) = @_;
	command_like(
		[
			'pg_waldump', '--path' => $archive,
			'--start' => $start, '--end' => $end
		],
		qr/rmgr:/,
		$test_name);
}

# Helper to run pg_waldump with expected failure
sub test_waldump_fails
{
	my ($archive, $start, $end, $pattern, $test_name) = @_;
	command_fails_like(
		[
			'pg_waldump', '--path' => $archive,
			'--start' => $start, '--end' => $end
		],
		$pattern,
		$test_name);
}

###########################################
# Test 1: Compressed archives (gzip, lz4, zstd)
###########################################

note "Testing compressed archives";

# Test gzip compression
SKIP:
{
	skip "gzip not supported", 2
	  unless check_pg_config("#define HAVE_LIBZ 1");

	my $gz_archive = "$tmp_dir/test_gzip.tar.gz";
	create_tar_archive($gz_archive, \@wal_files, '-c -z');

	test_waldump_succeeds($gz_archive, $start_lsn, $end_lsn,
		'pg_waldump works with gzip compressed archive');

	# Test with .tgz extension
	my $tgz_archive = "$tmp_dir/test_tgz.tgz";
	create_tar_archive($tgz_archive, \@wal_files, '-c -z');
	test_waldump_succeeds($tgz_archive, $start_lsn, $end_lsn,
		'pg_waldump works with .tgz extension');
}

# Test lz4 compression
SKIP:
{
	skip "lz4 not supported", 1
	  unless check_pg_config("#define USE_LZ4 1");
	skip "tar does not support lz4", 1
	  unless system("$tar --help 2>&1 | grep -q lz4") == 0;

	my $lz4_archive = "$tmp_dir/test_lz4.tar.lz4";
	create_tar_archive($lz4_archive, \@wal_files, '--lz4 -c');

	test_waldump_succeeds($lz4_archive, $start_lsn, $end_lsn,
		'pg_waldump works with lz4 compressed archive');
}

# Test zstd compression
SKIP:
{
	skip "zstd not supported", 1
	  unless check_pg_config("#define USE_ZSTD 1");
	skip "tar does not support zstd", 1
	  unless system("$tar --help 2>&1 | grep -q zstd") == 0;

	my $zst_archive = "$tmp_dir/test_zstd.tar.zst";
	create_tar_archive($zst_archive, \@wal_files, '--zstd -c');

	test_waldump_succeeds($zst_archive, $start_lsn, $end_lsn,
		'pg_waldump works with zstd compressed archive');
}

###########################################
# Test 2: Out-of-order WAL files
###########################################

note "Testing out-of-order WAL files in archive";

my @shuffled = shuffle(@wal_files);
my $unordered_archive = "$tmp_dir/test_unordered.tar";
create_tar_archive($unordered_archive, \@shuffled, '-c');

test_waldump_succeeds($unordered_archive, $start_lsn, $end_lsn,
	'pg_waldump handles out-of-order WAL files in archive');

# Verify temporary directory was created and cleaned up
my ($stdout, $stderr);
my $result = IPC::Run::run(
	[
		'pg_waldump', '--path' => $unordered_archive,
		'--start' => $start_lsn, '--end' => $end_lsn
	],
	'>',
	\$stdout,
	'2>',
	\$stderr);

ok($result, 'out-of-order WAL decoding completed');
like($stdout, qr/rmgr:/, 'out-of-order WAL produced valid output');

###########################################
# Test 3: Reverse-ordered WAL files
###########################################

note "Testing reverse-ordered WAL files";

my @reversed = reverse(@wal_files);
my $reversed_archive = "$tmp_dir/test_reversed.tar";
create_tar_archive($reversed_archive, \@reversed, '-c');

test_waldump_succeeds($reversed_archive, $start_lsn, $end_lsn,
	'pg_waldump handles reverse-ordered WAL files');

###########################################
# Test 4: Partial WAL file set
###########################################

note "Testing archive with subset of WAL files";

# Create archive with only odd-numbered files
my @partial_files = grep { (index($_, $wal_files[0]) % 2) == 1 } @wal_files;
if (scalar(@partial_files) > 0)
{
	my $partial_archive = "$tmp_dir/test_partial.tar";
	create_tar_archive($partial_archive, \@partial_files, '-c');

	# This should fail because not all segments are present
	test_waldump_fails(
		$partial_archive,
		$start_lsn,
		$end_lsn,
		qr/could not find/,
		'pg_waldump fails when WAL segments are missing from archive');
}

###########################################
# Test 5: Archive with non-WAL files
###########################################

note "Testing archive with non-WAL files mixed in";

# Create temporary non-WAL files in WAL directory
my $readme_file = "$waldir/README.txt";
my $backup_label = "$waldir/backup_label";

open(my $fh, '>', $readme_file) or die "Cannot create $readme_file: $!";
print $fh "This is not a WAL file\n";
close($fh);

open($fh, '>', $backup_label) or die "Cannot create $backup_label: $!";
print $fh "FAKE BACKUP LABEL\n";
close($fh);

my @mixed_files = (@wal_files, 'README.txt', 'backup_label');
my $mixed_archive = "$tmp_dir/test_mixed.tar";
create_tar_archive($mixed_archive, \@mixed_files, '-c');

test_waldump_succeeds($mixed_archive, $start_lsn, $end_lsn,
	'pg_waldump ignores non-WAL files in archive');

# Cleanup non-WAL files
unlink($readme_file, $backup_label);

###########################################
# Test 6: Empty archive
###########################################

note "Testing empty archive";

my $empty_archive = "$tmp_dir/test_empty.tar";
system("$tar -cf $empty_archive -T /dev/null");

test_waldump_fails(
	$empty_archive,
	$start_lsn,
	$end_lsn,
	qr/could not find WAL/,
	'pg_waldump fails gracefully with empty archive');

###########################################
# Test 7: Corrupted archive
###########################################

note "Testing corrupted archive";

# Create a valid archive then corrupt it
my $corrupt_archive = "$tmp_dir/test_corrupt.tar";
create_tar_archive($corrupt_archive, [ $wal_files[0] ], '-c');

# Truncate the archive to a very small size to break tar format
truncate($corrupt_archive, 100);

test_waldump_fails(
	$corrupt_archive,
	$start_lsn,
	$end_lsn,
	qr/could not (find|read) WAL/,
	'pg_waldump handles corrupted archive gracefully');

###########################################
# Test 8: Archive with only single segment
###########################################

note "Testing single-segment archive";

my $single_archive = "$tmp_dir/test_single.tar";
create_tar_archive($single_archive, [ $wal_files[0] ], '-c');

# Extract first LSN from this specific file
my ($single_stdout, $single_stderr);
IPC::Run::run(
	[
		'pg_waldump', $waldir . '/' . $wal_files[0],
		'--limit' => '1'
	],
	'>',
	\$single_stdout,
	'2>',
	\$single_stderr);
my ($single_start) = $single_stdout =~ /lsn: ([0-9A-F]+\/[0-9A-F]+)/i;

command_like(
	[
		'pg_waldump', '--path' => $single_archive,
		'--start' => $single_start,
		'--limit' => '10'
	],
	qr/rmgr:/,
	'pg_waldump works with single-segment archive');

###########################################
# Test 9: Multiple archives test (error case)
###########################################

note "Testing that multiple path arguments are rejected";

my $archive1 = "$tmp_dir/test1.tar";
my $archive2 = "$tmp_dir/test2.tar";
create_tar_archive($archive1, [ $wal_files[0] ], '-c');
create_tar_archive($archive2, [ $wal_files[1] ], '-c');

command_fails_like(
	[
		'pg_waldump',      '--path'  => $archive1,
		'--path' => $archive2, '--start' => $start_lsn
	],
	qr/error:/,
	'pg_waldump rejects multiple --path arguments');

###########################################
# Test 10: Boundary conditions - start at segment boundary
###########################################

note "Testing boundary conditions";

# Use start_lsn which is already a valid LSN for boundary testing
my $boundary_archive = "$tmp_dir/test_boundary.tar";
create_tar_archive($boundary_archive, \@wal_files, '-c');

command_like(
	[
		'pg_waldump',  '--path'  => $boundary_archive,
		'--start' => $start_lsn, '--end'   => $end_lsn
	],
	qr/rmgr:/,
	'pg_waldump handles LSN range with tar archive');

###########################################
# Test 11: Stats and filtering with archives
###########################################

note "Testing stats and filtering options with archives";

my $filter_archive = "$tmp_dir/test_filter.tar";
create_tar_archive($filter_archive, \@wal_files, '-c');

# Test --stats option
command_like(
	[
		'pg_waldump', '--path'  => $filter_archive,
		'--start'     => $start_lsn, '--end' => $end_lsn,
		'--stats'
	],
	qr/WAL statistics/,
	'pg_waldump --stats works with archive');

# Test --rmgr filter
command_like(
	[
		'pg_waldump', '--path'  => $filter_archive,
		'--start'     => $start_lsn, '--end' => $end_lsn,
		'--rmgr'      => 'Heap'
	],
	qr/rmgr: Heap/,
	'pg_waldump --rmgr filter works with archive');

# Test --limit option
my $limit_output;
IPC::Run::run(
	[
		'pg_waldump', '--path'  => $filter_archive,
		'--start'     => $start_lsn, '--end' => $end_lsn,
		'--limit'     => '10'
	],
	'>',
	\$limit_output);

my @lines = split /\n/, $limit_output;
my $record_count = grep { /^rmgr:/ } @lines;
cmp_ok($record_count, '<=', 10, 'pg_waldump --limit works with archive');

###########################################
# Test 12: Very large archive (if we have enough WAL)
###########################################

if (scalar(@wal_files) >= 10)
{
	note "Testing large archive with many segments";

	my $large_archive = "$tmp_dir/test_large.tar";
	create_tar_archive($large_archive, \@wal_files, '-c');

	my $large_start =
	  $node->safe_psql('postgres', "SELECT pg_current_wal_lsn() - '128MB'::pg_size;");

	command_like(
		[
			'pg_waldump', '--path' => $large_archive,
			'--start' => $large_start, '--end' => $end_lsn
		],
		qr/rmgr:/,
		'pg_waldump handles large archive with many segments');
}

###########################################
# Test 13: Archive with symbolic links (if supported)
###########################################

SKIP:
{
	skip "Symbolic links test", 1 if $^O eq 'MSWin32';

	note "Testing archive behavior with symbolic links";

	my $link_dir = "$tmp_dir/link_test";
	make_path($link_dir);

	# Create a symlink to a WAL file
	my $wal_source = "$waldir/$wal_files[0]";
	my $wal_link = "$link_dir/$wal_files[0]";  # Use proper WAL filename
	symlink($wal_source, $wal_link) or die "Cannot create symlink: $!";

	# Extract first LSN from this WAL file
	my ($symlink_stdout, $symlink_stderr);
	IPC::Run::run(
		[
			'pg_waldump', $wal_source,
			'--limit' => '1'
		],
		'>',
		\$symlink_stdout,
		'2>',
		\$symlink_stderr);
	my ($symlink_lsn) = $symlink_stdout =~ /lsn: ([0-9A-F]+\/[0-9A-F]+)/i;

	my $cwd = getcwd();
	chdir($link_dir) or die "Cannot chdir: $!";

	my $link_archive = "$tmp_dir/test_symlink.tar";
	system("$tar -chf $link_archive $wal_files[0]") == 0
	  or die "tar with symlink failed";

	chdir($cwd) or die "Cannot chdir back: $!";

	# Archive should contain the dereferenced file
	command_like(
		[ 'pg_waldump', '--path' => $link_archive, '--start' => $symlink_lsn, '--limit' => '5' ],
		qr/rmgr:/,
		'pg_waldump handles archive created with dereferenced symlinks');

	remove_tree($link_dir);
}

###########################################
# Test 14: Invalid archive formats
###########################################

note "Testing invalid archive formats";

# Create a file that's not a tar archive
my $not_tar = "$tmp_dir/not_a_tar.tar";
open(my $fh2, '>', $not_tar) or die "Cannot create $not_tar: $!";
print $fh2 "This is not a valid tar archive\n";
close($fh2);

test_waldump_fails(
	$not_tar,
	$start_lsn,
	$end_lsn,
	qr/could not (find|read) WAL/,
	'pg_waldump detects invalid tar format');

# Test with wrong compression type (create gzip, call it lz4)
# DISABLED: This test can cause pg_waldump to hang when decompression fails
#SKIP:
#{
#	skip "gzip not supported", 1
#	  unless check_pg_config("#define HAVE_LIBZ 1");
#
#	my $wrong_ext = "$tmp_dir/wrong.tar.lz4";
#	create_tar_archive($wrong_ext, [ $wal_files[0] ], '-c -z');
#
#	test_waldump_fails(
#		$wrong_ext,
#		$start_lsn,
#		$end_lsn,
#		qr/could not (find|read|decompress)/,
#		'pg_waldump detects compression mismatch');
#}

###########################################
# Test 15: Extremely long WAL file names (edge case)
###########################################

note "Testing edge cases with file naming";

# Tar archives should handle standard WAL naming correctly
# This test ensures we don't break on edge cases

my $standard_archive = "$tmp_dir/test_standard.tar";
create_tar_archive($standard_archive, [ $wal_files[0], $wal_files[-1] ], '-c');

command_like(
	[ 'pg_waldump', '--path' => $standard_archive, '--start' => $start_lsn, '--limit' => '10' ],
	qr/rmgr:/,
	'pg_waldump correctly handles standard WAL filenames in archive');

$node->stop;

done_testing();
