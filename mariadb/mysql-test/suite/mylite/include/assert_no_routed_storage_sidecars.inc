# ==== Purpose ====
#
# Check that a MyLite-routed MTR schema did not create native durable schema
# directories or table sidecars.
#
# ==== Usage ====
#
# --let $mylite_sidecar_schema= schema_name
# --source suite/mylite/include/assert_no_routed_storage_sidecars.inc

if (!$mylite_sidecar_schema)
{
  --die !!!ERROR IN TEST: mysqltest variable 'mylite_sidecar_schema' is not set
}

--let _MYLITE_SIDECAR_SCHEMA= $mylite_sidecar_schema

perl;
  use strict;
  use warnings;
  use File::Basename qw(basename dirname);

  my $schema = $ENV{'_MYLITE_SIDECAR_SCHEMA'}
      or die "_MYLITE_SIDECAR_SCHEMA is not set\n";
  my $vardir = $ENV{'MYSQLTEST_VARDIR'}
      or die "MYSQLTEST_VARDIR is not set\n";
  my $primary = $ENV{'MYLITE_MTR_PRIMARY_FILE'}
      or die "MYLITE_MTR_PRIMARY_FILE is not set\n";

  my @bad;
  my @schema_dirs = (
      "$vardir/mysqld.1/data/$schema",
      "$vardir/install.db/$schema",
  );
  for my $dir (@schema_dirs) {
      push @bad, $dir if -e $dir;
  }

  my $primary_dir = dirname($primary);
  my $primary_base = basename($primary);
  opendir(my $dh, $primary_dir)
      or die "Unable to open primary file directory '$primary_dir': $!\n";
  while (defined(my $name = readdir($dh))) {
      next if $name eq '.' || $name eq '..';
      next unless index($name, $primary_base) == 0;
      next if is_allowed_mylite_companion($name, $primary_base);
      push @bad, "$primary_dir/$name" if is_forbidden_sidecar_name($name);
  }
  closedir($dh) or die "Unable to close primary file directory '$primary_dir': $!\n";

  die "Forbidden MyLite routed-storage sidecar state:\n" .
      join("\n", @bad) . "\n" if @bad;

  sub is_allowed_mylite_companion {
      my ($name, $primary_base) = @_;
      return $name eq $primary_base ||
             $name eq "$primary_base-journal" ||
             $name eq "$primary_base-journal.rewrite" ||
             $name eq "$primary_base-transaction-journal" ||
             $name eq "$primary_base-transaction-journal.rewrite";
  }

  sub is_forbidden_sidecar_name {
      my ($name) = @_;
      return has_suffix($name, ".frm") ||
             has_suffix($name, ".par") ||
             has_suffix($name, ".ibd") ||
             has_suffix($name, ".MYD") ||
             has_suffix($name, ".MYI") ||
             has_suffix($name, ".MAI") ||
             has_suffix($name, ".MAD") ||
             has_prefix($name, "ibdata") ||
             has_prefix($name, "ib_logfile") ||
             has_prefix($name, "undo") ||
             has_prefix($name, "aria_log") ||
             has_prefix($name, "mysql-bin") ||
             has_prefix($name, "relay-log");
  }

  sub has_prefix {
      my ($value, $prefix) = @_;
      return index($value, $prefix) == 0;
  }

  sub has_suffix {
      my ($value, $suffix) = @_;
      return length($value) >= length($suffix) &&
             substr($value, -length($suffix)) eq $suffix;
  }
EOF
