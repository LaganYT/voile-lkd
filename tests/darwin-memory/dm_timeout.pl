#!/usr/bin/perl
# dm_timeout.pl SECONDS CMD [ARGS...]
# macOS has no timeout(1).  Run CMD in its own process group, kill the group
# after SECONDS, exit 124 on timeout, otherwise propagate the exit status
# (128+signal when the child died from a signal).
use strict;
use warnings;
use POSIX ":sys_wait_h";

my $t = shift @ARGV;
die "usage: dm_timeout.pl SECONDS CMD [ARGS...]\n" unless defined $t && @ARGV;
my $pid = fork();
die "fork: $!" unless defined $pid;
if ($pid == 0) {
    setpgrp(0, 0);
    exec { $ARGV[0] } @ARGV;
    die "exec: $!";
}
my $deadline = time + $t;
while (1) {
    my $r = waitpid($pid, WNOHANG);
    if ($r == $pid) {
        my $st = $?;
        if ($st & 127) {
            print STDERR "dm_timeout: child killed by signal " . ($st & 127) . "\n";
            exit(128 + ($st & 127));
        }
        exit($st >> 8);
    }
    if (time >= $deadline) {
        kill 'TERM', -$pid;
        select(undef, undef, undef, 1.0);
        kill 'KILL', -$pid;
        waitpid($pid, 0);
        print STDERR "dm_timeout: TIMEOUT after $t s\n";
        exit(124);
    }
    select(undef, undef, undef, 0.05);
}
