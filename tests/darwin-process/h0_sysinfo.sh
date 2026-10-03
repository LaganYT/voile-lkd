#!/bin/sh
# H0: environment facts only (nothing here modifies the system). Output on stdout.
echo "INFO H0.date $(date '+%Y-%m-%d %H:%M:%S %z')"
echo "INFO H0.sw_vers $(sw_vers | tr '\n\t' '  ' | sed 's/  */ /g')"
echo "INFO H0.uname $(uname -a)"
echo "INFO H0.kern.osversion $(sysctl -n kern.osversion)"
echo "INFO H0.kern.osproductversion $(sysctl -n kern.osproductversion)"
echo "INFO H0.kern.version $(sysctl -n kern.version)"
echo "INFO H0.kern.hv_support $(sysctl -n kern.hv_support)"
echo "INFO H0.csrutil $(/usr/bin/csrutil status 2>&1 | tr '\n' ' ')"
echo "INFO H0.sysctl.kern.bootargs [$(sysctl -n kern.bootargs 2>&1)]"
echo "INFO H0.nvram.boot-args $(nvram boot-args 2>&1 | head -1)"
echo "INFO H0.nvram.csr-active-config $(nvram csr-active-config 2>&1 | head -1)"
echo "INFO H0.hw.machine $(sysctl -n hw.machine) model=$(sysctl -n hw.model) brand=$(sysctl -n machdep.cpu.brand_string)"
echo "INFO H0.hw.ncpu $(sysctl -n hw.ncpu) physicalcpu=$(sysctl -n hw.physicalcpu) memsize=$(sysctl -n hw.memsize)"
echo "INFO H0.hw.perflevels nperflevels=$(sysctl -n hw.nperflevels) p_logical=$(sysctl -n hw.perflevel0.logicalcpu 2>/dev/null) p_name=$(sysctl -n hw.perflevel0.name 2>/dev/null) l1d=$(sysctl -n hw.perflevel0.l1dcachesize 2>/dev/null)"
echo "INFO H0.hw.pagesize $(sysctl -n hw.pagesize) vm.pagesize=$(sysctl -n vm.pagesize 2>/dev/null) hw.cachelinesize=$(sysctl -n hw.cachelinesize) hw.tbfrequency=$(sysctl -n hw.tbfrequency) hw.cpufrequency=$(sysctl -n hw.cpufrequency 2>&1 | head -1)"
echo "INFO H0.proc_translated $(sysctl -n sysctl.proc_translated 2>&1)"
echo "INFO H0.hw.optional.arm $(sysctl -a 2>/dev/null | grep -E '^hw\.optional\.arm\.' | sed 's/^hw.optional.arm.//' | tr '\n' ' ')"
echo "INFO H0.hw.optional.misc $(sysctl -a 2>/dev/null | grep -E '^hw\.optional\.(AdvSIMD|floatingpoint|armv8|neon|amx|ucnormal|watchpoint|breakpoint)' | tr '\n' ' ')"
# Rosetta: facts from the file system only. 'arch -x86_64' is never run (it could start an installer prompt).
for p in /Library/Apple/usr/share/rosetta/rosetta /Library/Apple/usr/libexec/oah/libRosettaRuntime /Library/Apple/usr/libexec/oah/RosettaLinux /Library/Apple/usr/libexec/oah/translate_tool /Library/Apple/usr/libexec/oah/runtime; do
    if [ -e "$p" ]; then echo "INFO H0.rosetta.present $p size=$(stat -f %z "$p")"; else echo "INFO H0.rosetta.absent $p"; fi
done
echo "INFO H0.rosetta.pkg $(pkgutil --pkg-info com.apple.pkg.RosettaUpdateAuto 2>&1 | tr '\n' ' ')"
echo "INFO H0.devtools $(DevToolsSecurity -status 2>&1 | tr '\n' ' ')"
echo "INFO H0.id uid=$(id -u) gid=$(id -g) groups=$(id -Gn | tr ' ' ',')"
for r in system.privilege.taskport system.privilege.taskport.debug; do
    echo "INFO H0.authdb.$r $(security authorizationdb read $r 2>/dev/null | tr -d '\t\n' | sed -e 's/<?xml[^>]*?>//' -e 's/<!DOCTYPE[^>]*>//' -e 's/<key>comment<\/key><string>[^<]*<\/string>//' | cut -c1-420)"
done
echo "INFO H0.sysctl.limits maxproc=$(sysctl -n kern.maxproc) maxprocperuid=$(sysctl -n kern.maxprocperuid) maxfiles=$(sysctl -n kern.maxfiles) maxfilesperproc=$(sysctl -n kern.maxfilesperproc) num_taskthreads=$(sysctl -n kern.num_taskthreads) num_threads=$(sysctl -n kern.num_threads) num_tasks=$(sysctl -n kern.num_tasks) argmax=$(sysctl -n kern.argmax)"
echo "INFO H0.sysctl.ipc maxsockbuf=$(sysctl -n kern.ipc.maxsockbuf) somaxconn=$(sysctl -n kern.ipc.somaxconn) local.stream.sendspace=$(sysctl -n net.local.stream.sendspace) local.stream.recvspace=$(sysctl -n net.local.stream.recvspace) local.dgram.maxdgram=$(sysctl -n net.local.dgram.maxdgram) local.dgram.recvspace=$(sysctl -n net.local.dgram.recvspace)"
echo "INFO H0.sysctl.vm vm.pagesize=$(sysctl -n vm.pagesize) kern.ipc.maxsockbuf=$(sysctl -n kern.ipc.maxsockbuf) sysv.shmmax=$(sysctl -n kern.sysv.shmmax) sysv.shmall=$(sysctl -n kern.sysv.shmall) sysv.shmmni=$(sysctl -n kern.sysv.shmmni) posix.sem.max=$(sysctl -n kern.posix.sem.max 2>&1)"
echo "INFO H0.sysctl.kern.boottime $(sysctl -n kern.boottime | tr -d '\n')"
echo "INFO H0.ulimit $(ulimit -a | tr '\n' ';' | sed 's/  */ /g')"
echo "INFO H0.tools clang=$(clang --version | head -1) xcode=$(xcodebuild -version 2>/dev/null | head -1) sdk=$(xcrun --show-sdk-version)"
