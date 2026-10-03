#!/bin/sh
# Usage: run-with-timeout.sh SECONDS command [args...]
# macOS has no timeout(1). The command runs in its own process group; after SECONDS the whole group gets SIGALRM, then
# SIGKILL 2 s later, and whatever is left of the group when the command ends is killed too (guest processes are host
# fork()s that an alarm on the first process would not reach). RLIMIT_CPU, inherited by every fork, caps a spinning
# process. FEXDARWIN_SUPERVISOR_PID names this wrapper: every fexdarwin-run process of the run ends itself within 0.5 s
# once the wrapper is gone, even if the wrapper is killed with SIGKILL. Exit status: the command's, or 128 + signal.
secs="$1"; shift
ulimit -t $((secs * 16)) 2>/dev/null
FEXDARWIN_SUPERVISOR_PID=$$
export FEXDARWIN_SUPERVISOR_PID
exec perl -e '
  my $secs = shift;
  my $pid = fork();
  die "fork failed: $!" unless defined $pid;
  if ($pid == 0) { setpgrp(0, 0); exec @ARGV or die "exec failed: $!"; }
  $SIG{ALRM} = sub { kill "ALRM", -$pid; sleep 2; kill "KILL", -$pid; };
  alarm $secs;
  waitpid($pid, 0);
  my $st = $?;
  alarm 0;
  kill "KILL", -$pid;
  exit(($st & 127) ? 128 + ($st & 127) : ($st >> 8));
' "$secs" "$@"
