
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test pg_verifybackup with WAL archives
# This tests the integration of pg_waldump's tar archive support
# with pg_verifybackup

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Path qw(remove_tree);

my $tar = $ENV{TAR};

# Skip all tests if tar is not available
if (!defined $tar)
{
	plan skip_all => 'tar command not available';
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(has_archiving => 1, allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', q{
wal_level = replica
max_wal_senders = 2
checkpoint_timeout = 1h
archive_mode = on
});
$primary->start;

# Create some data
$primary->safe_psql('postgres', 'CREATE TABLE test_data (id serial, data text);');
$primary->safe_psql('postgres',
	"INSERT INTO test_data (data) SELECT repeat('backup_test', 100) FROM generate_series(1, 1000);"
);

my $backup_path = $primary->backup_dir . '/test_backup';
my $tmp_dir = PostgreSQL::Test::Utils::tempdir_short();

###########################################
# Test 1: Plain format backup with WAL in tar
###########################################

note "Testing plain format backup with separate WAL tar archive";

# Take a plain format backup
$primary->command_ok(
	[
		'pg_basebackup', '-D', $backup_path,
		'-Fp', '--no-sync'
	],
	'plain format backup taken');

# Create more WAL after backup
$primary->safe_psql('postgres',
	"INSERT INTO test_data (data) SELECT 'post_backup' FROM generate_series(1, 500);"
);
$primary->safe_psql('postgres', 'CHECKPOINT;');

# Archive the WAL directory
my $wal_archive = "$tmp_dir/pg_wal.tar";
system("$tar -cf $wal_archive -C $backup_path pg_wal") == 0
  or die "Failed to create WAL archive";

# Verify backup with WAL path pointing to archive
command_ok(
	[
		'pg_verifybackup', '-e', $backup_path,
		'--wal-path', $wal_archive
	],
	'pg_verifybackup works with WAL tar archive (plain format)');

###########################################
# Test 2: Tar format backup with WAL archive
###########################################

note "Testing tar format backup with WAL verification";

my $tar_backup = "$tmp_dir/backup.tar";

# Take a tar format backup
$primary->command_ok(
	[ 'pg_basebackup', '-D', $tar_backup, '-Ft', '--no-sync' ],
	'tar format backup taken');

# Generate more WAL
$primary->safe_psql('postgres',
	"INSERT INTO test_data (data) SELECT 'tar_test' FROM generate_series(1, 500);"
);
$primary->safe_psql('postgres', 'CHECKPOINT;');

# The tar backup includes pg_wal.tar - test that pg_verifybackup can use it
command_ok(
	[ 'pg_verifybackup', '-e', $tar_backup ],
	'pg_verifybackup processes tar format backup with embedded WAL archive');

###########################################
# Test 3: Compressed tar backup with WAL
###########################################

SKIP:
{
	skip "gzip not supported", 1
	  unless check_pg_config("#define HAVE_LIBZ 1");

	note "Testing compressed tar backup with WAL archive";

	my $gz_backup = "$tmp_dir/backup.tar.gz";

	$primary->command_ok(
		[ 'pg_basebackup', '-D', $gz_backup, '-Ft', '-z', '--no-sync' ],
		'gzip compressed tar backup taken');

	# Generate more WAL
	$primary->safe_psql('postgres',
		"INSERT INTO test_data (data) SELECT 'gz_test' FROM generate_series(1, 500);"
	);
	$primary->safe_psql('postgres', 'CHECKPOINT;');

	command_ok(
		[ 'pg_verifybackup', '-e', $gz_backup ],
		'pg_verifybackup processes gzip compressed tar backup with WAL');
}

###########################################
# Test 4: Missing WAL segments in archive
###########################################

note "Testing error handling with incomplete WAL archive";

# Create a backup
my $incomplete_backup = "$tmp_dir/incomplete_backup";
$primary->command_ok(
	[
		'pg_basebackup', '-D', $incomplete_backup,
		'-Fp', '--no-sync'
	],
	'backup for incomplete test taken');

# Get list of WAL files
my $wal_dir = "$incomplete_backup/pg_wal";
my @wal_files;
opendir(my $dh, $wal_dir) or die "Cannot open $wal_dir: $!";
while (my $file = readdir($dh))
{
	push @wal_files, $file if $file =~ /^[0-9A-F]{24}$/;
}
closedir($dh);

# Create archive with only half the WAL files
my @partial_wal = @wal_files[ 0 .. int(scalar(@wal_files) / 2) ];
my $incomplete_archive = "$tmp_dir/incomplete_wal.tar";
system("$tar -cf $incomplete_archive -C $wal_dir @partial_wal") == 0
  or die "Failed to create incomplete archive";

# Remove the WAL directory and use the incomplete archive
remove_tree($wal_dir);

# This should fail due to missing WAL
command_fails(
	[
		'pg_verifybackup', '-e', $incomplete_backup,
		'--wal-path', $incomplete_archive
	],
	'pg_verifybackup detects missing WAL segments in archive');

###########################################
# Test 5: Corrupted WAL archive
###########################################

note "Testing error handling with corrupted WAL archive";

# Create a valid backup
my $corrupt_test_backup = "$tmp_dir/corrupt_test";
$primary->command_ok(
	[
		'pg_basebackup', '-D', $corrupt_test_backup,
		'-Fp', '--no-sync'
	],
	'backup for corruption test taken');

# Create WAL archive
my $corrupt_wal_archive = "$tmp_dir/corrupt_wal.tar";
system("$tar -cf $corrupt_wal_archive -C $corrupt_test_backup pg_wal") == 0
  or die "Failed to create WAL archive";

# Corrupt the archive
truncate($corrupt_wal_archive, -s $corrupt_wal_archive / 2);

# Remove original WAL directory
remove_tree("$corrupt_test_backup/pg_wal");

# This should fail gracefully
command_fails(
	[
		'pg_verifybackup', '-e', $corrupt_test_backup,
		'--wal-path', $corrupt_wal_archive
	],
	'pg_verifybackup handles corrupted WAL archive gracefully');

###########################################
# Test 6: Multiple tablespaces with tar WAL
###########################################

SKIP:
{
	skip "tablespace tests require filesystem support", 1
	  if $^O eq 'MSWin32';

	note "Testing backup with tablespaces and WAL archive";

	# Create tablespace
	my $ts_path = PostgreSQL::Test::Utils::tempdir_short();
	$primary->safe_psql('postgres',
		"CREATE TABLESPACE test_ts LOCATION '$ts_path';");

	# Create table in tablespace
	$primary->safe_psql('postgres',
		'CREATE TABLE ts_test (id serial, data text) TABLESPACE test_ts;');
	$primary->safe_psql('postgres',
		"INSERT INTO ts_test (data) SELECT 'tablespace_data' FROM generate_series(1, 1000);"
	);
	$primary->safe_psql('postgres', 'CHECKPOINT;');

	# Take backup
	my $ts_backup = "$tmp_dir/ts_backup";
	$primary->command_ok(
		[
			'pg_basebackup', '-D', $ts_backup,
			'-Fp', '--no-sync'
		],
		'backup with tablespace taken');

	# Create WAL archive
	my $ts_wal_archive = "$tmp_dir/ts_wal.tar";
	system("$tar -cf $ts_wal_archive -C $ts_backup pg_wal") == 0
	  or die "Failed to create WAL archive";

	# Verify with WAL archive
	command_ok(
		[
			'pg_verifybackup', '-e', $ts_backup,
			'--wal-path', $ts_wal_archive
		],
		'pg_verifybackup works with tablespaces and WAL archive');
}

###########################################
# Test 7: Out-of-order WAL in archive
###########################################

note "Testing WAL archive with out-of-order segments";

# Create a fresh backup
my $order_backup = "$tmp_dir/order_backup";
$primary->command_ok(
	[
		'pg_basebackup', '-D', $order_backup,
		'-Fp', '--no-sync'
	],
	'backup for order test taken');

# Get WAL files and shuffle them
@wal_files = ();
$wal_dir = "$order_backup/pg_wal";
opendir($dh, $wal_dir) or die "Cannot open $wal_dir: $!";
while (my $file = readdir($dh))
{
	push @wal_files, $file if $file =~ /^[0-9A-F]{24}$/;
}
closedir($dh);

# Create archive with reversed order
my @reversed_wal = reverse(@wal_files);
my $reversed_archive = "$tmp_dir/reversed_wal.tar";
system("$tar -cf $reversed_archive -C $wal_dir @reversed_wal") == 0
  or die "Failed to create reversed archive";

# Remove original WAL
remove_tree($wal_dir);

# Verify - should work despite reversed order
command_ok(
	[
		'pg_verifybackup', '-e', $order_backup,
		'--wal-path', $reversed_archive
	],
	'pg_verifybackup handles out-of-order WAL segments in archive');

###########################################
# Test 8: --no-parse-wal with archive
###########################################

note "Testing --no-parse-wal option";

my $no_parse_backup = "$tmp_dir/no_parse_backup";
$primary->command_ok(
	[
		'pg_basebackup', '-D', $no_parse_backup,
		'-Fp', '--no-sync'
	],
	'backup for no-parse test taken');

my $no_parse_archive = "$tmp_dir/no_parse_wal.tar";
system("$tar -cf $no_parse_archive -C $no_parse_backup pg_wal") == 0
  or die "Failed to create WAL archive";

# Even with WAL archive available, --no-parse-wal should skip parsing
command_ok(
	[
		'pg_verifybackup', '-e', $no_parse_backup,
		'--wal-path', $no_parse_archive, '--no-parse-wal'
	],
	'pg_verifybackup --no-parse-wal works with WAL archive');

###########################################
# Test 9: Backup with standby.signal
###########################################

note "Testing standby backup verification with WAL archive";

# Create a standby
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'test_backup',
	has_streaming => 1,
	has_restoring => 1);
$standby->start;

# Wait for standby to catch up
$primary->wait_for_catchup($standby);

# Take backup from standby
my $standby_backup = "$tmp_dir/standby_backup";
$standby->command_ok(
	[
		'pg_basebackup', '-D', $standby_backup,
		'-Fp', '--no-sync'
	],
	'backup from standby taken');

# Create WAL archive from standby backup
my $standby_wal_archive = "$tmp_dir/standby_wal.tar";
system("$tar -cf $standby_wal_archive -C $standby_backup pg_wal") == 0
  or die "Failed to create standby WAL archive";

# Verify standby backup with WAL archive
command_ok(
	[
		'pg_verifybackup', '-e', $standby_backup,
		'--wal-path', $standby_wal_archive
	],
	'pg_verifybackup works with standby backup and WAL archive');

###########################################
# Test 10: Error in manifest but WAL is OK
###########################################

note "Testing manifest errors with valid WAL archive";

my $manifest_backup = "$tmp_dir/manifest_backup";
$primary->command_ok(
	[
		'pg_basebackup', '-D', $manifest_backup,
		'-Fp', '--no-sync'
	],
	'backup for manifest test taken');

# Create WAL archive (should be valid)
my $manifest_wal_archive = "$tmp_dir/manifest_wal.tar";
system("$tar -cf $manifest_wal_archive -C $manifest_backup pg_wal") == 0
  or die "Failed to create WAL archive";

# Corrupt the manifest
my $manifest_file = "$manifest_backup/backup_manifest";
open(my $fh, '>>', $manifest_file) or die "Cannot open manifest: $!";
print $fh "CORRUPTED_DATA\n";
close($fh);

# Remove original WAL directory
remove_tree("$manifest_backup/pg_wal");

# Verification should fail due to manifest corruption, not WAL issues
command_fails(
	[
		'pg_verifybackup', '-e', $manifest_backup,
		'--wal-path', $manifest_wal_archive
	],
	'pg_verifybackup detects manifest corruption with valid WAL archive');

###########################################
# Test 11: Large backup with compressed WAL archive
###########################################

SKIP:
{
	skip "gzip not supported", 1
	  unless check_pg_config("#define HAVE_LIBZ 1");

	note "Testing large backup with compressed WAL archive";

	# Generate substantial data
	$primary->safe_psql('postgres',
		"INSERT INTO test_data (data) SELECT repeat('large_backup', 500) FROM generate_series(1, 5000);"
	);
	$primary->safe_psql('postgres', 'CHECKPOINT;');

	my $large_backup = "$tmp_dir/large_backup";
	$primary->command_ok(
		[
			'pg_basebackup', '-D', $large_backup,
			'-Fp', '--no-sync'
		],
		'large backup taken');

	# Create compressed WAL archive
	my $large_wal_gz = "$tmp_dir/large_wal.tar.gz";
	system("$tar -czf $large_wal_gz -C $large_backup pg_wal") == 0
	  or die "Failed to create compressed WAL archive";

	# Remove original WAL
	remove_tree("$large_backup/pg_wal");

	# Verify with compressed archive
	command_ok(
		[
			'pg_verifybackup', '-e', $large_backup,
			'--wal-path', $large_wal_gz
		],
		'pg_verifybackup works with large compressed WAL archive');
}

###########################################
# Test 12: Concurrent backups with WAL archives
###########################################

note "Testing verification of multiple backups with separate WAL archives";

my $backup1 = "$tmp_dir/concurrent1";
my $backup2 = "$tmp_dir/concurrent2";

# Take first backup
$primary->command_ok(
	[ 'pg_basebackup', '-D', $backup1, '-Fp', '--no-sync' ],
	'first concurrent backup taken');

# Generate more WAL
$primary->safe_psql('postgres',
	"INSERT INTO test_data (data) SELECT 'between_backups' FROM generate_series(1, 500);"
);
$primary->safe_psql('postgres', 'CHECKPOINT;');

# Take second backup
$primary->command_ok(
	[ 'pg_basebackup', '-D', $backup2, '-Fp', '--no-sync' ],
	'second concurrent backup taken');

# Create separate WAL archives
my $wal1 = "$tmp_dir/wal1.tar";
my $wal2 = "$tmp_dir/wal2.tar";
system("$tar -cf $wal1 -C $backup1 pg_wal") == 0
  or die "Failed to create first WAL archive";
system("$tar -cf $wal2 -C $backup2 pg_wal") == 0
  or die "Failed to create second WAL archive";

# Verify both backups with their respective WAL archives
command_ok(
	[ 'pg_verifybackup', '-e', $backup1, '--wal-path', $wal1 ],
	'first backup verifies with its WAL archive');

command_ok(
	[ 'pg_verifybackup', '-e', $backup2, '--wal-path', $wal2 ],
	'second backup verifies with its WAL archive');

$standby->stop;
$primary->stop;

done_testing();
