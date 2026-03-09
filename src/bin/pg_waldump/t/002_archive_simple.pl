
# Copyright (c) 2026, PostgreSQL Global Development Group

# Simplified tar archive tests for pg_waldump

use strict;
use warnings FATAL => 'all';
use Cwd;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Copy;

my $tar = $ENV{TAR};

if (!defined $tar)
{
	plan skip_all => 'tar command not available';
}

my $node = PostgreSQL::Test::Cluster->new('tar_test');
$node->init;
$node->start;

# Ensure we have WAL to work with
$node->safe_psql('postgres', 'CREATE TABLE test (id int, data text);');

# Generate substantial WAL across multiple segments
for (my $i = 0; $i < 5; $i++)
{
	$node->safe_psql('postgres',
		"INSERT INTO test SELECT generate_series(1, 5000), repeat('testdata', 200);");
	$node->safe_psql('postgres', 'SELECT pg_switch_wal();');
}

# Final checkpoint
$node->safe_psql('postgres', 'CHECKPOINT;');

my $waldir = $node->data_dir . '/pg_wal';
my $tmp_dir = PostgreSQL::Test::Utils::tempdir_short();

# Get ALL available WAL files
my @wal_files;
opendir(my $dh, $waldir) or die "opendir: $!";
while (my $file = readdir($dh))
{
	push @wal_files, $file if $file =~ /^[0-9A-F]{24}$/;
}
closedir($dh);

@wal_files = sort @wal_files;

ok(scalar(@wal_files) >= 2, 'generated multiple WAL segments: ' . scalar(@wal_files));

# Use pg_waldump to find the first valid record in the first WAL file
my $start_walfile = $wal_files[0];
my $end_walfile = $wal_files[-1];

# Get LSN of first record in first file
my ($stdout_first, $stderr_first);
IPC::Run::run(
	[
		'pg_waldump', $waldir . '/' . $start_walfile,
		'--limit' => '1'
	],
	'>',
	\$stdout_first,
	'2>',
	\$stderr_first);

# Extract LSN from output: "rmgr: name len (rec/tot): nn/nn, tx: nnn, lsn: X/X, ..."
my ($start_lsn) = $stdout_first =~ /lsn: ([0-9A-F]+\/[0-9A-F]+)/i;

# Get current WAL LSN as end point (this will span multiple segments)
my ($end_lsn) = split /\|/,
  $node->safe_psql('postgres',
	q{SELECT pg_current_wal_insert_lsn()});

ok(scalar(@wal_files) >= 2, 'generated multiple WAL segments: ' . scalar(@wal_files));
note "Testing LSN range: $start_lsn ($start_walfile) to $end_lsn ($end_walfile)";
note "WAL files available: " . join(', ', @wal_files);

###########################################
# Test 1: Basic uncompressed tar
###########################################

my $basic_tar = "$tmp_dir/basic.tar";
my $cwd = getcwd();
chdir($waldir) or die "chdir: $!";
system("$tar -cf $basic_tar @wal_files") == 0 or die "tar failed";
chdir($cwd) or die "chdir: $!";

command_like(
	[
		'pg_waldump',  '--path'  => $basic_tar,
		'--start' => $start_lsn, '--end'   => $end_lsn
	],
	qr/rmgr:/,
	'pg_waldump reads from uncompressed tar archive');

###########################################
# Test 2: Gzip compressed tar
###########################################

SKIP:
{
	skip "gzip not supported", 1
	  unless check_pg_config("#define HAVE_LIBZ 1");

	my $gz_tar = "$tmp_dir/compressed.tar.gz";
	chdir($waldir) or die "chdir: $!";
	system("$tar -czf $gz_tar @wal_files") == 0 or die "tar failed";
	chdir($cwd) or die "chdir: $!";

	command_like(
		[
			'pg_waldump',  '--path'  => $gz_tar,
			'--start' => $start_lsn, '--end'   => $end_lsn
		],
		qr/rmgr:/,
		'pg_waldump reads from gzip compressed tar archive');
}

###########################################
# Test 3: Out-of-order WAL files
###########################################

my @reversed = reverse(@wal_files);
my $reversed_tar = "$tmp_dir/reversed.tar";
chdir($waldir) or die "chdir: $!";
system("$tar -cf $reversed_tar @reversed") == 0 or die "tar failed";
chdir($cwd) or die "chdir: $!";

command_like(
	[
		'pg_waldump',  '--path'  => $reversed_tar,
		'--start' => $start_lsn, '--end'   => $end_lsn
	],
	qr/rmgr:/,
	'pg_waldump handles reversed WAL files in archive');

###########################################
# Test 4: Stats option with archive
###########################################

command_like(
	[
		'pg_waldump',  '--path'  => $basic_tar,
		'--start' => $start_lsn, '--end'   => $end_lsn,
		'--stats'
	],
	qr/WAL statistics/,
	'pg_waldump --stats works with archive');

###########################################
# Test 5: Limit option with archive
###########################################

my ($stdout, $stderr);
my $result = IPC::Run::run(
	[
		'pg_waldump',  '--path'  => $basic_tar,
		'--start' => $start_lsn, '--end'   => $end_lsn,
		'--limit'     => '10'
	],
	'>',
	\$stdout,
	'2>',
	\$stderr);

ok($result, 'pg_waldump --limit works with archive');
my @lines = split /\n/, $stdout;
my $record_count = grep { /^rmgr:/ } @lines;
cmp_ok($record_count, '<=', 10, '--limit honored (got ' . $record_count . ' records)');

###########################################
# Test 6: Empty archive (error case)
###########################################

my $empty_tar = "$tmp_dir/empty.tar";
system("$tar -cf $empty_tar -T /dev/null 2>/dev/null") == 0
  or die "tar failed";

command_fails_like(
	[
		'pg_waldump',  '--path'  => $empty_tar,
		'--start' => $start_lsn
	],
	qr/could not find WAL/,
	'pg_waldump fails with empty archive');

###########################################
# Test 7: Corrupted archive (error case)
###########################################

my $corrupt_tar = "$tmp_dir/corrupt.tar";
chdir($waldir) or die "chdir: $!";
system("$tar -cf $corrupt_tar $wal_files[0]") == 0 or die "tar failed";
chdir($cwd) or die "chdir: $!";

# Truncate to a very small size to break tar format itself
truncate($corrupt_tar, 100) or die "truncate failed";

command_fails(
	[
		'pg_waldump',  '--path'  => $corrupt_tar,
		'--start' => $start_lsn, '--end'   => $end_lsn
	],
	'pg_waldump handles corrupted archive');

###########################################
# Test 8: Missing WAL segment (error case)
###########################################

# Create archive with only first file, pg_waldump will try to continue reading
# and fail when it can't find the next segment
my @partial_files = ($wal_files[0]);
my $partial_tar = "$tmp_dir/partial.tar";
chdir($waldir) or die "chdir: $!";
system("$tar -cf $partial_tar @partial_files") == 0 or die "tar failed";
chdir($cwd) or die "chdir: $!";

# Don't specify --end, so pg_waldump will try to read as far as possible
# pg_waldump will either fail to find next segment or hit end of valid data
command_fails(
	[
		'pg_waldump',  '--path'  => $partial_tar,
		'--start' => $start_lsn
	],
	'pg_waldump fails when WAL segments are missing');

###########################################
# Test 9: Rmgr filtering with archive
###########################################

command_like(
	[
		'pg_waldump',  '--path'  => $basic_tar,
		'--start' => $start_lsn, '--end'   => $end_lsn,
		'--rmgr'      => 'XLOG'
	],
	qr/rmgr: XLOG/,
	'pg_waldump --rmgr filter works with archive');

$node->stop;

done_testing();
