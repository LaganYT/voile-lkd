#!/bin/sh
# usage: runtest.sh <timeout-seconds> <program> [args...]
# Runs a program with a wall-clock limit (no GNU timeout on macOS). Exit 124 on timeout.
t="$1"; shift
exec /usr/bin/perl -e '
  my $t = shift; my $pid = fork();
  if ($pid == 0) { setpgrp(0, 0); exec @ARGV or exit 127; }
  $SIG{ALRM} = sub { kill "KILL", -$pid; kill "KILL", $pid; waitpid($pid, 0); exit 124; };
  alarm $t; waitpid($pid, 0);
  my $st = $?; exit(($st & 127) ? 128 + ($st & 127) : $st >> 8);
' "$t" "$@"
