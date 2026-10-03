# Réutilisation de la couche processus de HyperLinux par un moteur FEXCore en processus

Date : 2026-10-02. Statut : version complète relue sur les sources (brouillon 3). Seconde moitié de l'étude de réutilisation de HyperLinux : threads, fork, exec, wait, signaux, futex, ptrace, réseau, polling. La mémoire, les VMA, le vfs et procfs sont traités dans `reports/hl-reuse-core.md` ; je ne cite leurs symboles que pour mesurer le couplage.

Conventions. MESURÉ : sortie d'une commande ou d'un programme lancé dans cette session, avec le journal cité. LU : lecture d'une source, avec `fichier:ligne`. INFÉRÉ : déduction de ma part, non exécutée. Les chemins `src/...` désignent `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928/hyper-linux/src/` (HL). `EXP` désigne `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-wine-fex-darwin-20261002`. Les chemins `probes/...` désignent `rocket-hyperlinux-20260928/probes/...`.

## Résultat en bref

1. HyperLinux n'est pas un noyau réutilisable tel quel. Sa couche processus est soudée à la boucle `vcpu_run_loop` (HVC, ESR_EL1, registres HVF) et à un fork par `posix_spawn` plus transfert d'état. Sur 18 527 lignes de fichiers liés aux processus que j'ai classées par plages de lignes, j'estime 9 500 lignes à garder avec des changements mécaniques (accès mémoire invitée, registres), 2 300 à réécrire et 6 700 à abandonner (boucle HVC, IPC de fork, Rosetta, stub GDB). INFÉRÉ à plus ou moins 30 %.
2. Huit modules n'ont aucune référence HVF (comptage `grep`) : `futex.c`, `syscall_poll.c`, `syscall_net.c`, `syscall_inotify.c`, `syscall_fd.c`, `proc_ptrace.c`, `proc_names.c`, `trace.c`. Leur seule dépendance d'exécution au moteur est la copie de mémoire invitée (`guest_read`, `guest_write`, `guest_ptr`). Dépendance de compilation : onze en-têtes incluent `<Hypervisor/Hypervisor.h>` (dont `syscall.h`, `guest.h`, `thread.h`, `syscall_signal.h`, `syscall_proc.h`), que sept de ces huit modules incluent par transitivité (`trace.c` en est exempt) ; il faut les en purger (types `hv_vcpu_t` dans les prototypes de signaux et dans `guest_t`).
3. Le dispatch de HL est indexé par les numéros de syscall arm64 (`src/syscall.h:17-170`). Les 276 enregistrements de la couche Linux de FEX qui font un « passthrough » appellent le noyau hôte avec ces mêmes numéros arm64 par `svc #0` (`Syscalls/Passthrough.cpp:18-150`). HL peut donc jouer le rôle de noyau hôte derrière le front-end x86-64 de FEX, ce qui évite de refaire la conversion x86-64 (numéros, `epoll_event` packé, `stat`). La couverture par nom est de 128 syscalls sur 241 noms de passthrough FEX (53 %) ; 113 manquent.
4. `proc_ptrace.c` est déjà une émulation en espace utilisateur de la relation traceur/tracee entre deux processus du runtime, sans ptrace noyau : fichier d'état partagé, verrou `fcntl`, socket de notification, arrêt coopératif du tracee. Je l'ai recompilé dans la copie de travail : 79 contrôles PASS, 0 FAIL (`logs/host-test-proc-ptrace-host.log`). Son contenu est ARM64 (34 mots) : la vue x86 (216 octets, que demande EAC) est à écrire. FEXCore garde l'état x86 réel en mémoire ordinaire (`CPUState`), donc la vue x86 est exacte par construction, contrairement au contexte ARM de Rosetta.
5. Cinq lacunes cassent Wine avant même EAC : pas de signal vers un autre processus invité hors SIGKILL/SIGCONT d'un enfant (Wine server envoie SIGUSR1 aux clients, `voile-wine/server/thread.c:961`) ; futex locale au processus ; pas de SIGCHLD à la mort d'un enfant ; `fork` ignore `CLONE_CHILD_SETTID` ; `vfork` et `posix_spawn` mènent à un chemin vm-clone non qualifié.
6. MESURÉ sur cette machine : un descripteur kqueue ne passe pas par `SCM_RIGHTS` (`EINVAL`) et n'est pas hérité par `fork` (`EBADF` dans l'enfant) (`logs/host-test-kq-fork-scm.log`). Le fork de HL ferait échouer `clone` si un timerfd est ouvert. Epoll, timerfd et inotify doivent être reconstruits après `fork`.
7. Le `fork()` natif de macOS 27 tient dans un processus multithread avec `MAP_JIT` (journal du coéquipier `evidence/darwin-process/f1_fork_mt.log`, 17 PASS, 0 FAIL). Les 2 300 lignes de transfert d'état de `fork_ipc.c` deviennent inutiles. Le prix : les verrous tenus par les threads absents restent verrouillés dans l'enfant (mutex, `os_unfair_lock`, stdio), donc le schéma `LockBeforeFork`/`UnlockAfterFork` de FEX est obligatoire.
8. Infrastructure de test : l'arbre complet se compile sans avertissement en 2,9 s (39 objets, lien réussi, aucune exécution) ; 8 programmes de test hôte passent en 2,7 s sans HVF ; le test `test-thread-vcpu-lifetime` ne se lie pas avec le Makefile tel quel (symbole `proc_trace_tracer_exit`). 77 programmes invités x86-64 statiques sont déjà compilés (`test-binaries/amd64/bin`) et peuvent servir de tests d'acceptation d'un hôte FEX.
9. Constat annexe (hors périmètre mais mesuré) : `SYS_fgetxattr` vaut 16 et `SYS_fremovexattr` 18 dans `src/syscall.h:166,169`, alors que les numéros arm64 sont 10 et 16 (`FEX Arm64/SyscallsEnum.h:22,28`). Les journaux EAC contiennent 304 événements `unimplemented_syscall` pour le numéro 10.

## 0. Sources, état de l'arbre, méthode

Arbre lu : HL en état copie de travail, HEAD 8048a96 (« docs: link the public examples sibling »). MESURÉ (`git status --short`, `git diff --stat`, lecture seule) : 45 entrées, soit 33 fichiers suivis modifiés (4475 insertions, 1560 suppressions) et 12 non suivis (11 sources plus `.DS_Store`). Non suivis, donc entièrement privés : `src/fault_dump.c/h`, `src/proc_names.c/h`, `src/proc_ptrace.c/h`, `src/signal_notify.h`, `src/syscall_filemap.c/h`, `src/syscall_reopen.c/h`.

Intégrité : j'ai relancé `shasum -a 256 -c EXP/evidence/hl-source-manifest-after.sha256` depuis HL : 214 entrées, aucune ligne non OK. Je n'ai rien écrit dans HL, dans le jeu, dans le runtime installé ni dans les dépôts. Je n'ai lancé ni jeu, ni EAC, ni QEMU, ni UTM, ni HVF.

Lecture complète des fichiers : `thread.c/h`, `syscall_proc.c/h`, `syscall_signal.c/h`, `futex.c/h`, `fork_ipc.c/h`, `syscall_exec.c`, `proc_ptrace.c/h`, `syscall_internal.h`, `signal_notify.h`, `rosetta.h`, `gdb_stub.h`, `trace.h`, `crash_report.h`, `hv_util.h`, `proc_names.h`, `syscall_poll.h`, `syscall_net.h`, `syscall_inotify.h`. Lecture ciblée (fonctions, en-têtes, tables) : `syscall.c` (dispatch), `syscall_poll.c`, `syscall_net.c`, `syscall_inotify.c`, `syscall_fd.c`, `proc_names.c`, `trace.c`, `crash_report.c`, `fault_dump.c`, `hl.c`, `syscall_sys.c`. Documents lus : `proc-ptrace-module.md`, `ptrace-runtime.md`, `ptrace-options-contract.md`, `ptrace-syscall-contract.md`, `ptrace-regset-contract.md`, `regset-architecture.md`, le README de `ptrace-remote-setregset-runtime-20261001`, `ptrace-x86-feasibility-20260929.md`, `eac-ptrace-target-role-20260929.md`, les README des audits EAC, ainsi que `docs/research/rocket-league-eac-technologies-macos-2026-10-01.md`.

Hypothèse de moteur, reprise de la consigne : FEXCore dans un processus macOS arm64 natif, adresses invitées égales aux adresses hôtes, sans Hypervisor.framework, sans shim EL1, sans Rosetta. L'invité visé est un processus Linux x86-64 (Wine Linux, jeu Windows, module EAC Linux). Réserve : le rapport `reports/fexcore-darwin-port.md` mesure qu'un processus arm64 macOS ne peut pas réserver la mémoire sous 4 Gio ni au-dessus de 0x7ffffe000000, alors que `wine64-preloader` demande 0x10000-0x68000000 et 0x7f000000-0x7fff0000. L'identité parfaite des adresses ne pourra donc peut-être pas héberger Wine, et un décalage de base invité pourrait être nécessaire. Je recommande de garder une couche d'accès mémoire invitée explicite (section 3), ce qui rend ce décalage peu coûteux.

## 1. Architecture telle qu'implémentée

### 1.1 Un processus invité est un processus macOS avec sa propre VM HVF

LU : `src/fork_ipc.h:6-8` (HVF n'autorise qu'une VM par processus, donc fork lance un nouveau processus hl). Le PID invité est un entier d'un compteur partagé par toute la famille de fork : fichier `mkstemp` non lié d'une page, mappé `MAP_SHARED`, entier atomique `next` initialisé à 2, PID initial 1 (`src/syscall_proc.c:45-46,80-113`, `proc_alloc_pid` 516-529, export et import 531-562). Les PID restent sous `0x3fffffff`, limite du champ propriétaire des futex PI (`src/syscall_proc.c:77-81`). Le registre des tâches (`comm`, `exe`, `cmdline`, relation ptrace publiée) est un second fichier partagé indexé par TID, protégé par un verrou `fcntl` (`src/proc_names.c:1-80`, 479 lignes), transmis à l'enfant par descripteur.

### 1.2 vCPU contre threads invités

LU. Un thread invité est un `thread_entry_t` (`src/thread.h:28-102`) dans une table de `MAX_THREADS = 64` entrées (`src/thread.h:25`), sous un mutex global `thread_lock` de rang 5 (`src/thread.c:34`), avec un pointeur `_Thread_local current_thread` (`src/thread.c:45`). Chaque thread invité possède un `hv_vcpu_t` créé sur son propre pthread hôte, parce que HVF lie un vCPU à son thread créateur (`src/fork_ipc.c:1442-1445`). Il reçoit une pile EL1 de 4 Kio tirée d'un bitmap de 64 emplacements (`src/thread.c:36-40,373-393`). Le thread porte aussi : masque de signaux et pile alternative par thread (`src/thread.h:42-51`), état ptrace local de Rosetta (`53-77`), instantané GDB (`88-91`), état des clones `CLONE_VM` (`93-101`).

Interruption : `hv_vcpus_exit` sous le verrou de table (`src/thread.c:283-296`, `454-472`). Un signal invité ou `exit_group` réveille tous les vCPU ; la boucle voit `HV_EXIT_REASON_CANCELED` (`src/syscall_proc.c:2073-2136`). Les appels hôte bloquants ne sont interruptibles que sur le chemin `read`, `write`, `pread`, `pwrite` (`interruptible_iov_io`, `src/syscall_io.c:304-324`) : le thread publie son pthread, un thread « waker » lui envoie SIGUSR2 hôte toutes les 2 ms tant que le réveil est en attente (`src/thread.c:47-140`). `ppoll`, `pselect6` et `epoll_pwait` sans délai se réveillent par le pipe `wakeup_pipe` et l'époque d'interruption (`src/syscall_poll.c:221-265,436-497,846-911`) ; `wait4` boucle par pas de 1 ms et `futex` par pas de 100 ms. En revanche `nanosleep` et `clock_nanosleep` appellent le `nanosleep` hôte sans condition de sortie (`src/syscall_time.c`), et `syscall_net.c` ne contient ni `signal_pending`, ni `EINTR`, ni `poll(` : `accept`, `connect`, `recv*` et `send*` bloquent sur l'hôte sans être interrompus par un signal invité. INFÉRÉ de ces absences, non exécuté.

Fin de thread : écriture de 0 dans `clear_child_tid` puis `futex_wake_one`, après destruction du vCPU (`src/fork_ipc.c:1515-1541`). `exit_group` positionne `exit_group_requested`, réveille tout (`thread_interrupt_all`, pipe de réveil) et joint les workers pendant au plus 50 ms avant de les détacher (`src/syscall_proc.c:125-138`, `src/thread.c:405-439`).

### 1.3 Variantes de clone

LU : le dispatcher `sys_clone` (`src/fork_ipc.c:2623-2642`) n'a que trois branches.

| Flags | Chemin | Effet |
|---|---|---|
| `CLONE_THREAD` (pthread_create glibc, musl) | `sys_clone_thread`, `src/fork_ipc.c:1290-1440` | Nouveau pthread hôte de 2 Mio, nouveau vCPU, mêmes tables de pages. Copie des 31 GPR et des registres système du parent, X0=0, SP=`child_stack`, TPIDR_EL0=`tls` si `CLONE_SETTLS` (1484-1488). `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID` traités (1384-1407). |
| `CLONE_VM` sans `CLONE_THREAD` | `sys_clone_vm`, `src/fork_ipc.c:1568-1701` | Un thread hôte détaché avec `is_vm_clone=1`, TID propre, attendable par wait4 via la table de threads (`src/thread.c:783-869`). Conçu pour le JIT à deux processus de Rosetta. `CLONE_VFORK` n'est pas honoré ; si `child_stack` vaut 0 le « enfant » reprend la pile du parent (1633-1634). La sortie du dernier vm-clone déclenche `exit_group` (1795-1808). |
| Tout le reste, dont `fork()` glibc (flags 0x1200011, observé pour EAC) | `sys_clone_fork` puis `sys_clone_fork_inner`, `src/fork_ipc.c:1841-2617` | `posix_spawn` d'un `hl --fork-child <fd>` (1873-1930) puis transfert d'état (1.4). Seul `CLONE_VFORK` y est lu : le parent attend le `waitpid` de l'enfant (2595). |

Constats LUS :

- `sys_clone_fork(vcpu, g, flags, verbose)` ne reçoit ni `child_stack`, ni `ptid`, ni `tls`, ni `ctid` (`src/fork_ipc.c:2612-2642`). Un `fork()` glibc passe `CLONE_CHILD_SETTID` avec `ctid = &THREAD_SELF->tid` : l'enfant ne reçoit donc pas son TID dans cette variable. INFÉRÉ : tout code glibc qui lit le TID mis en cache dans l'enfant voit celui du parent (mutex PI, `pthread_kill(pthread_self())`).
- `vfork()` et `posix_spawn()` de glibc utilisent `CLONE_VM|CLONE_VFORK` (0x4111 avec SIGCHLD). INFÉRÉ : ils tombent dans `sys_clone_vm`, le parent n'est pas suspendu, et un `execve` de l'« enfant » appelle `guest_reset(g)` sur la mémoire invitée partagée (`src/syscall_exec.c:373`). Aucun test HL n'exerce vfork ni posix_spawn (`grep` sur `test/test-*.c` : seul `test-shm.c:216` en parle, dans un commentaire).
- `clone3` renvoie ENOSYS (`src/syscall.c:2417-2420`) ; glibc retombe sur `clone`. `unshare`, `setns`, `pidfd_open` sont absents du dispatch.

### 1.4 Comment fork copie la mémoire invitée et les trous de fork corrigés

LU : deux chemins, IPC version 18 (`src/fork_ipc.c:57`).

1. Chemin COW. Si la mémoire invitée est un fichier `mkstemp` `MAP_SHARED` (`g->shm_fd >= 0`) et hors Rosetta, le parent envoie le descripteur par `SCM_RIGHTS` et l'enfant le mappe `MAP_PRIVATE` (`src/fork_ipc.c:575-592,1954-1964,2041-2047`). Zéro octet copié. Le parent ne remappe jamais, car HVF met en cache la correspondance VA hôte vers PA (1966-1978).
2. Chemin copie. Rosetta et repli : énumération des régions utilisées, blocs de 1 Mio sur le socket (`src/fork_ipc.c:2080-2135`).

État envoyé dans l'ordre : en-tête `ipc_header_t` (brk, pile, mmap, TTBR0/1, placement Rosetta, 62-91), descripteur de l'allocateur de PID, descripteur des noms, deux descripteurs ptrace avec ACK de l'enfant (2014-2038), registres `ipc_registers_t` (93-108), mémoire, table de FD par `SCM_RIGHTS` (dup de chaque FD hôte, 2251-2261), objets OSS recréés vides, états « reopen » procfd, états epoll (les kqueue ne passent pas, 161-163 et 243-250), cwd et umask, instantané VFS, segments SysV shm, vues de fichiers partagées, sysroot, chemin ELF, chemin hl, cmdline, régions sémantiques, plages VA hautes, VMA, alias VA, régions pré-annoncées, état de signaux, blob du shim, sentinelle.

Trous de fork corrigés par les correctifs privés (LU, motifs dans les commentaires) : identité de tâche et noms partagés dans la famille (`src/syscall_proc.c:76-81` : un compteur local donnait au premier pthread de l'enfant le PID de l'enfant) ; ACK d'import des descripteurs ptrace (sans lui, Darwin signalait un EOF sur le socket reçu pendant que l'alias émetteur se fermait, faux décès du parent mesuré 3 fois sur 100 ; `src/fork_ipc.c:313-336,2029-2038`, `probes/ptrace-scm-diagnosis.md`) ; kqueue d'epoll recréés dans l'enfant (`src/fork_ipc.c:164-243`, `src/syscall_poll.c:742-810`) ; états procfd rouverts sans fermer d'alias, car fermer un alias libère les verrous POSIX (343-446) ; mappings `MAP_SHARED` de fichiers conservés partagés (448-504, `src/syscall_filemap.c`) ; segments SysV rattachés (2431-2452) ; page `[vvar]` du vDSO, dont le thread publieur n'existe pas dans l'enfant (1233-1243) ; masque de signaux et pile alternative restaurés après `thread_register_main` (1119-1125, 1226-1231) ; tampons FD sur le tas parce que la pile d'un worker macOS fait 512 Kio (1832-1840, 2140-2150).

Trous de fork restants (LUS ou INFÉRÉS) :

- MESURÉ avec `EXP/scratch-hl/probe-src/kq_fork_scm.c` : `dup(kqueue)` réussit, `sendmsg(SCM_RIGHTS)` d'un fd kqueue échoue avec `EINVAL`, et après `fork()` le fd kqueue n'existe pas dans l'enfant (`fcntl` et `kevent` répondent `EBADF`), le kqueue du parent restant intact (`logs/host-test-kq-fork-scm.log`).
- `FD_TIMERFD` a pour fd hôte le kqueue lui-même (`src/syscall_fd.c:79-89`) et passe par le chemin générique `dup` puis `SCM_RIGHTS` (`src/fork_ipc.c:2251-2261`). INFÉRÉ : `send_fds` échoue (2320-2334) et `clone` renvoie ENOMEM tant qu'un timerfd est ouvert. Non exécuté.
- `FD_EVENTFD`, `FD_SIGNALFD` et `FD_INOTIFY` ont un pipe comme fd hôte, mais leur état (compteur, masque, kqueue) vit dans des tableaux locaux au processus (`src/syscall_fd.c:389-397,626`, `src/syscall_inotify.c:69-94`). `src/fork_ipc.c` ne contient aucune occurrence de ces trois types : l'enfant a le fd mais pas l'état.
- Un eventfd passé par `SCM_RIGHTS` à un autre processus perd aussi son compteur. C'est le schéma d'esync dans Wine (non activé par défaut).

### 1.5 exec

LU (`src/syscall_exec.c:70-764`). Le runtime ne se relance pas : exec se fait dans le même processus macOS. Séquence : lecture de path, argv, envp dans la mémoire invitée (limites 256 arguments, 4096 variables, 256 Kio, lignes 100-102), résolution VFS, chargement ELF ou shebang (133-250), pré-validation de l'interpréteur ou de Rosetta (251-308), point de non-retour (310), fermeture des FD `O_CLOEXEC` avec nettoyage des sous-systèmes eventfd, signalfd, timerfd, inotify (316-370), `guest_reset(g)` qui met toute la mémoire invitée à zéro (373), remise à zéro de `exit_group_requested` (379), `signal_reset_for_exec` (384), rechargement du shim, des segments et de l'interpréteur (386-421), reconstruction des tables de pages (460-575), nouvelle pile (625-684), écriture de TTBR0, ELR, SP_EL0, SPSR, TPIDR_EL0 et mise à zéro des 31 GPR (696-736), puis arrêt ptrace d'exec (`proc_trace_exec_stop`, 746-757).

Limites LUES : aucun appel qui arrête les autres threads (le fichier ne référence que `current_thread` et `exit_group_requested`) ; l'exec depuis un thread non-leader (de_thread) n'est pas géré, ce que confirme `probes/ptrace-syscall-contract.md:33` (« full nonleader exec ... remain outside this lot »). Le lien ptrace survit à l'exec (`src/proc_ptrace.h:67-69`).

### 1.6 wait4, waitid, SIGCHLD

LU (`src/syscall_proc.c:1077-1196`). `proc_wait_event` interroge en boucle : les événements ptrace distants (`proc_trace_wait`), la table de threads locale (`thread_ptrace_wait`, clones Rosetta) et la table des enfants `proc_table[64]` avec un `wait4(WNOHANG)` hôte par enfant. Sans `WNOHANG` il dort 1 ms (`nanosleep`, 1150-1151) et teste `signal_pending` et `exit_group`. Les statuts d'arrêt sont recodés en numéros Linux (1121-1128). `waitid` accepte `P_ALL` et `P_PID` seulement (1170), avec `WSTOPPED`, `WEXITED`, `WCONTINUED`, `WNOWAIT`. Les options `__WALL` et `__WCLONE` sont ignorées.

Constats LUS :

- Aucun SIGCHLD n'est mis en file à la mort d'un enfant ordinaire. `LINUX_SIGCHLD` n'apparaît que dans la notification d'arrêt ptrace (`src/syscall_signal.c:300-318`), le champ `exit_signal` des vm-clones (jamais livré) et la valeur du `waitid` (`src/syscall_proc.c:1179`). `SA_NOCLDWAIT` et l'auto-récolte ne sont pas gérés.
- Un processus invité tué par un signal par défaut sort avec le code `128 + signum` (`src/syscall_signal.c:1002-1007`), donc le parent lit `WIFEXITED` avec ce code, pas `WIFSIGNALED`. INFÉRÉ à partir de ce code ; non exécuté.
- Pas de reparentage ni de sous-reaper ; `getppid` renvoie le PID du parent d'origine ; `setpgid` et `setsid` sont des stubs (`src/syscall.c:2088-2093`).

### 1.7 Signaux vers l'invité

LU (`src/syscall_signal.c/.h`).

- Les actions (`actions[64]`) sont uniques par processus, sous `sig_lock` (rang 4). Les signaux dirigés (tkill, tgkill, `rt_tgsigqueueinfo`) ont des ensembles en attente par thread `directed_pending[MAX_THREADS]` (`src/syscall_signal.c:37-47,165-189`) avec un `siginfo` complet. Signaux standards coalescés ; signaux temps réel 32..64 en file de 32 (`RT_SIGQUEUE_MAX`, `src/syscall_signal.h:177`).
- Livraison seulement à des points de la boucle : après chaque syscall (`src/syscall_proc.c:1380-1385`) ou après annulation du vCPU (2124-2131). `signal_queue_info` réveille tous les vCPU, les E/S hôte du thread visé, signalfd et le pipe de réveil (`src/syscall_signal.c:284-289`).
- Cadre : `linux_rt_sigframe_t` arm64 sur la pile invitée (siginfo de 128 octets, `ucontext`, `sigcontext` avec 31 GPR, SP, PC, PSTATE), contexte FPSIMD (magic `0x46508001`, 528 octets, 843-899), contexte ESR pour les fautes synchrones, restaurateur toujours égal à la page vDSO (`src/syscall_signal.c:1153-1156`, `sa_restorer` ignoré comme le noyau arm64).
- Masques et pile alternative par thread. `SA_ONSTACK`, `SA_NODEFER`, `SA_RESETHAND`, `SA_SIGINFO`, `SA_RESTART` traités. Le redémarrage `SA_RESTART` sauvegarde x0..x5, x8 et le PC avant chaque appel redémarrable (`signal_syscall_begin`, `src/syscall_signal.c:389-396`, liste `src/syscall.c:1185-1211`) puis recule le PC de 4 octets (403, 1037).
- `sigsuspend` renvoie toujours EINTR sans dormir (`src/syscall_signal.c:727-777`) : une boucle `while (!flag) sigsuspend()` tourne à vide.
- `ITIMER_REAL` invité est émulé à la main parce que l'`alarm()` hôte sert au chien de garde vCPU (80-90, 541-621).
- Signaux entre processus : `kill` n'accepte que soi-même, le « groupe » supposé égal à soi, ou un enfant enregistré avec SIGKILL ou SIGCONT seulement (`src/syscall.c:1982-2001`, `src/syscall_proc.c:641-665` ; tout autre signal renvoie EOPNOTSUPP). `tgkill` avec un `tgid` différent du PID courant renvoie ESRCH (`src/syscall.c:2025-2028`). `probes/ptrace-runtime.md:51` le confirme.
- Fautes synchrones : le shim EL1 les transmet par HVC #11 (abort, instruction indéfinie) et HVC #10 (BRK) ; la boucle fabrique `si_code`, `si_addr` et l'ESR (`src/syscall_proc.c:1692-1887`).

### 1.8 futex

LU (`src/futex.c`, 905 lignes). Table de 64 seaux indexés par l'adresse virtuelle invitée, attendeur alloué sur la pile de l'appelant, une condvar par attendeur (91-137). Opérations : WAIT, WAKE, WAIT_BITSET, WAKE_BITSET, REQUEUE, CMP_REQUEUE, WAKE_OP, LOCK_PI, UNLOCK_PI, TRYLOCK_PI (857-893) ; le reste renvoie ENOSYS (895-898).

- La table est locale au processus. Aucune attente ni réveil entre processus, même sur une mémoire `MAP_SHARED`. C'est l'écart principal pour Wine, dont les processus partagent des sections.
- `FUTEX_WAIT` sans délai se réveille toutes les 100 ms et renvoie EINTR après 1 s (`futex.c:230-269`) : substitut de livraison de signal, pas la sémantique Linux.
- `FUTEX_WAIT_BITSET` utilise `pthread_cond_timedwait` en CLOCK_REALTIME et traite le délai absolu comme temps réel, que `FUTEX_CLOCK_REALTIME` soit posé ou non (`futex.c:143-165`) ; Linux utilise CLOCK_MONOTONIC par défaut. INFÉRÉ : `pthread_cond_clockwait` et `sem_clockwait` de glibc se comportent mal.
- PI : propriété du mot sous le verrou du seau, récupération d'un propriétaire mort par `thread_tid_alive`, pas d'héritage de priorité, pas d'`OWNER_DIED` (`futex.c:78-87,596-771`).
- `set_robust_list` renvoie 0 sans rien faire (`src/syscall.c:1810-1814`). Absents : `get_robust_list`, `futex_waitv` (449), `futex_wake/wait/requeue` (454-456), `FUTEX_CMP_REQUEUE_PI`, `FUTEX_WAIT_REQUEUE_PI`, `FUTEX_LOCK_PI2`.

### 1.9 poll, select, epoll, eventfd, timerfd, signalfd

LU. `ppoll` et `pselect6` : `src/syscall_poll.c:117-553`. epoll par kqueue : `EVFILT_READ` et `EVFILT_WRITE`, `EV_CLEAR` pour ET, `EV_ONESHOT` pour ONESHOT, table de 256 kevents par appel, EPOLLEXCLUSIVE non pris en charge (`syscall_poll.c:554-990`, commentaire 561). Le réveil des attentes indéfinies passe par un pipe global `wakeup_pipe` (`src/syscall_poll.h:46-52`). `epoll_event` suit la disposition arm64 de 16 octets (`syscall_poll.c:583-588`).

`src/syscall_fd.c` (814 lignes) : eventfd = pipe non bloquant plus compteur dans un tableau de 32 entrées (`:384-444`) ; timerfd = kqueue `EVFILT_TIMER` à usage unique réarmé à la lecture, 32 entrées, `TFD_TIMER_ABSTIME` ignoré (`:30-60,121-126`) ; signalfd = pipe plus masque, 16 entrées (`:626,686-720`). Aucun de ces objets n'est partageable entre processus.

### 1.10 ptrace tel qu'il existe aujourd'hui

Deux implémentations disjointes derrière `sys_ptrace` (`src/syscall_proc.c:870-1044`).

A. Locale, propre à Rosetta (`src/thread.c:577-869`, `src/syscall_proc.c:909-1039`) : SEIZE, CONT, INTERRUPT, GETSIGINFO, GETREGSET et SETREGSET `NT_PRSTATUS`, plus `NT_PRFPREG` et `NT_ARM_HW_WATCH`. Le traceur et le tracee sont deux threads du même processus (clones `CLONE_VM`). Le tracee s'arrête de lui-même sur BRK (HVC #10) ou sur INTERRUPT, copie ses registres ARM dans `ptrace_regs` et dort sur une condvar (`src/thread.c:607-683`). Registres ARM64 seulement. À abandonner.

B. Distante, module `proc_ptrace.c` (807 lignes, non suivi) : relation parent vers enfant créée au fork (1.11).

Requêtes traitées pour la relation distante (`src/syscall_proc.c:873-893`) : TRACEME (0), SETOPTIONS (0x4200, masque 0x11 uniquement), GETEVENTMSG (0x4201), CONT (7), DETACH (17), SYSCALL (24), GETREGSET `NT_PRSTATUS` (0x4204, 34 mots ARM64). Tout le reste renvoie EINVAL ou ESRCH.

Usage réel par EAC (LU dans `probes/`) : TRACEME dans l'enfant PID 148, puis dans le parent SETOPTIONS(148, 0x11) et 66 GETREGSET de 216 octets (`probes/eac-ptrace-audit-regset-linux-20260929/README.md`) ; PTRACE_SYSCALL (24) 146 fois (`eac-ptrace-regset-transition`) ; dans le run `eac-ptrace-poke-20260930/run-20260930T133949334669Z/progress-finding.json` : POKEDATA (5) d'un mot dont l'octet bas est 0xCC, CONT (7), wait4 avec statut 1407 (0x57f, SIGTRAP), GETREGSET, PEEKDATA (2), POKEDATA de restauration, kill ; puis un SETREGSET de 216 octets qui recule RIP d'un octet (`probes/ptrace-remote-setregset-runtime-20261001/README.md:3-9`). Le module EAC contient 21 sites d'appel statiques à `ptrace` (`probes/eac-current-module-static-20260929/verified-ptrace-call-sites.json`) ; seules les requêtes ci-dessus sont observées à l'exécution, les autres sites sont inconnus. La taille 216 est celle du `user_regs_struct` x86-64 (27 mots) : EAC demande une vue x86, que Rosetta remplit avec les 27 premiers registres ARM (`probes/regset-architecture.md:9`).

### 1.11 La relation traceur/tracee entre deux processus, en espace utilisateur pur

LU (`src/proc_ptrace.c/h`, `probes/proc-ptrace-module.md`). Cette partie n'utilise aucun ptrace noyau.

- Au fork, le parent crée un fichier de métadonnées `mkstemp` non lié (`trace_shared_t`, `src/proc_ptrace.c:28-40`), mappé `MAP_SHARED` dans les deux processus, plus une paire de sockets Unix de notification. Ces descripteurs ne figurent jamais dans la table de FD invitée et ne vont qu'à l'enfant direct (`:321-389`, `src/fork_ipc.c:2014-2038`).
- Exclusion mutuelle entre processus : verrou POSIX `fcntl(F_SETLKW)` sur le premier octet plus un mutex local par processus (`:117-131`). Le verrou se libère à la mort du propriétaire.
- Le tracee s'arrête lui-même, sur son thread, à trois points : livraison de signal (`signal_deliver` vers `proc_trace_stop`, `src/syscall_signal.c:937-985`), entrée et sortie de syscall (`syscall_trace_boundary`, `src/syscall.c:1213-1235,1257,2496`), événement d'exec (`src/syscall_exec.c:746-757`). Il capture alors ses registres par le rappel `proc_trace_capture_regs` (34 mots, `src/syscall_proc.c:152-183`), les publie dans le fichier partagé avec un numéro de séquence d'arrêt (`src/proc_ptrace.c:620-691`) et attend la reprise par `poll` du socket avec un délai de 200 ms (`:698-715`).
- Le traceur lit la séquence d'arrêt dans wait4 et waitid (`proc_trace_wait`, `:582-618`), contrôle par CONT, DETACH, SYSCALL (`proc_trace_control`, `:459-492`) et lit les registres capturés (`proc_trace_getregs`, `:533-563`). Seul le TID traceur exact peut contrôler (`:469-471`).
- Mort d'un pair : EOF du socket de notification (`peer_closed`, `:193-206`) ; la mort du traceur détache et reprend le tracee avec le signal supprimé (`:735-755`).

Limites (LUES, `probes/ptrace-runtime.md:49-55`, `proc-ptrace-module.md:9`) : pas d'ATTACH ni de SEIZE d'un non-enfant, pas d'accès mémoire distant (`process_vm_readv` répond ENOSYS), pas de PEEK ni POKE, pas d'écriture de registres, pas d'arrêts de groupe, pas de reparentage, pas de SIGCHLD asynchrone pour un traceur qui n'attend pas.

## 2. Table par fichier

LOC mesurées par `wc -l`. Colonnes A, R, D : lignes à adapter ou garder (A), à réécrire sur le même principe (R), à abandonner (D), estimées par plages de lignes que j'ai lues (calcul : script Python dans l'historique de cette session, plages citées dans les verdicts). Total des 37 fichiers du tableau : 18 527 lignes, A 9 483, R 2 316, D 6 728.

| Fichier | LOC | Sous-système | Couplage concret HVF, EL1, Rosetta | ABI invitée supposée | A | R | D | Verdict |
|---|---|---|---|---|---|---|---|---|
| thread.c | 885 | table de threads, E/S hôte interruptibles, ptrace local | `hv_vcpu_t`, `hv_vcpus_exit` (283-296, 454-472), `hv_vcpu_destroy` (276), `sp_el1` (373-393), `HV_SYS_REG_DBGW*` (508-552), `vcpu_get_gpr` (607-616) | `linux_user_pt_regs_t` ARM64 | 406 | 45 | 434 | adapt (table, `host_io_waker`, join) ; drop (ptrace local 473-885) ; s'appuyer sur le ThreadManager de FEX |
| thread.h | 226 | idem | `Hypervisor.h`, `hv_vcpu_exit_t`, `ptrace_*` | ARM64 | 109 | 27 | 90 | adapt |
| syscall_proc.c | 2164 | PID, wait, boucle vCPU, sys_ptrace | `vcpu_run_loop` (1223-2164), HVC 0/2/4/5/7/9/10/11/12, `ESR_EL1`, `KBUF_VA_BASE`, `dump_rosetta_state` (242-351), surcharge `ID_AA64*` (1482-1540) | x0-x8, ESR, ARM64 | 619 | 409 | 1136 | adapt (PID, wait, table d'enfants) ; rewrite (`sys_ptrace`, capture) ; drop (boucle HVC) |
| syscall_proc.h | 171 | idem | `Hypervisor.h`, `vcpu_run_loop` | ARM64 | 125 | 0 | 46 | adapt |
| syscall_signal.c | 1255 | signaux | `hv_vcpu_get/set_reg`, `HV_SYS_REG_ELR_EL1`, `HV_REG_FPSR`, Q0-Q31 (858-899, 1023-1165, 1190-1250) | cadre arm64, FPSIMD, ESR, restaurateur vDSO | 958 | 297 | 0 | adapt (état, files, itimer, sigaction) ; rewrite (cadre, sigreturn) |
| syscall_signal.h | 314 | idem | `Hypervisor.h` | `linux_sigcontext_t`, `linux_ucontext_t` ARM64 | 274 | 40 | 0 | adapt ; rewrite (structures 133-172) |
| futex.c / .h | 905 / 42 | futex locale | aucun HVF ; `guest_ptr` (7 sites) | générique | 947 | 0 | 0 | adapt (mémoire invitée) ; étendre (section 4) |
| fork_ipc.c / .h | 2642 / 28 | clone, fork par IPC | `posix_spawn` (1919), `hv_vcpu_create` (1167, 1454), TTBR0/1, SCTLR (1197), `guest_init_from_shm` (586), 109 références HVF | ARM64, `ipc_registers_t` | 0 | 330 | 2340 | drop (IPC 130-1255, 1561-2617) ; rewrite (clone thread 1256-1560, dispatcher) |
| syscall_exec.c / .h | 764 / 24 | execve | `guest_reset` (373), `guest_build_page_tables` (569), `rosetta_prepare/finalize` (479, 636), `hv_vcpu_set_sys_reg` (697-713) | ARM64, Rosetta | 336 | 138 | 314 | adapt (lecture argv, shebang, CLOEXEC) ; drop (rebuild des tables de pages) ; rewrite (hook d'exec) |
| syscall_poll.c / .h | 1002 / 52 | poll, select, epoll | aucun HVF ; `guest_*` (16 sites) | `epoll_event` 16 octets (583-588) | 863 | 191 | 0 | adapt ; rewrite (attente epoll, layout x86) |
| syscall_net.c / .h | 1701 / 122 | sockets | aucun HVF ; `guest_*` (41 sites) ; `rosettad_is_socket` | `msghdr` LP64 identique x86-64 | 1823 | 0 | 0 | adapt |
| syscall_inotify.c / .h | 667 / 44 | inotify sur kqueue vnode | aucun HVF ; `guest_*` (4 sites) | `inotify_event` identique | 711 | 0 | 0 | adapt |
| syscall_fd.c / .h | 814 / 55 | eventfd, timerfd, signalfd | aucun HVF ; `guest_*` (8 sites) | identique | 432 | 437 | 0 | adapt ; rewrite (états partageables entre processus) |
| proc_ptrace.c / .h | 807 / 115 | ptrace distant, espace utilisateur | `PROC_TRACE_REG_WORDS = 34` ARM64 (`:41`), rappel `proc_trace_capture_regs` | NT_PRSTATUS ARM64 | 922 | 0 | 0 | adapt (tampon de registres arch-neutre, version 5) |
| proc_names.c / .h | 479 / 60 | registre familial des tâches | aucun | générique | 539 | 0 | 0 | adapt (tel quel pour l'essentiel) |
| signal_notify.h | 10 | notification d'arrêt ptrace | aucun | générique | 10 | 0 | 0 | as-is |
| rosetta.c / .h | 301 / 136 | Rosetta | tout (`TCR_EL1_VALUE`, VZ ioctl, rosettad) | ARM64 | 0 | 0 | 437 | drop |
| gdb_stub.c / .h | 1732 / 78 | GDB RSP AArch64 | `HV_SYS_REG_DBG*`, XML AArch64 | ARM64 | 0 | 0 | 1810 | drop ; FEX a son GdbServer (`LinuxEmulation/GdbServer`, 1523 lignes) |
| syscall_internal.h | 132 | rangs de verrous, FD | aucun | générique | 132 | 0 | 0 | as-is |
| trace.c / .h | 209 / 68 | traces par catégorie | aucun (libc pure) | générique | 277 | 0 | 0 | as-is (déjà lié par les tests hôte) |
| crash_report.c / .h | 195 / 37 | rapport de plantage | 11 références HVF | ARM64 | 0 | 232 | 0 | rewrite (facultatif) |
| fault_dump.c / .h | 162 / 8 | diagnostic de faute | `mach_vm_read`, VMA, `hv_vcpu_t` | ARM64 | 0 | 170 | 0 | rewrite (facultatif) |
| hv_util.h | 121 | macros HVF | `HV_CHECK`, SCTLR | ARM64 | 0 | 0 | 121 | drop |
| syscall.c / .h (hors total) | 2512 / 645 | dispatch et constantes | x0-x5, x8, `hv_vcpu_set_reg(X0)` (2495), `SYS_*` arm64 (17-170) | ARM64 | n/a | n/a | n/a | le squelette de dispatch (1181-1303, 2463-2512) et environ 600 lignes de cas processus restent ; le reste appartient au coéquipier |

### 2.1 Couplage mesuré par fichier

MESURÉ par `grep -c` sur HL/src : nombre de lignes qui touchent la mémoire invitée (`guest_read`, `guest_write`, `guest_ptr`, `guest_read_str`, `guest_ptr_avail`), HVF (`hv_vcpu`, `hv_vm`, `HV_REG`, `HV_SYS_REG`, `HV_CHECK`, `vcpu_get`, `vcpu_set`) et `guest_t`.

| Fichier | accès mémoire invitée | références HVF | `guest_t` |
|---|---|---|---|
| thread.c | 0 | 48 | 21 |
| syscall_proc.c | 19 | 82 | 22 |
| syscall_signal.c | 11 | 32 | 11 |
| syscall_poll.c | 16 | 0 | 4 |
| syscall_net.c | 41 | 0 | 19 |
| syscall_inotify.c | 4 | 0 | 2 |
| syscall_exec.c | 3 | 15 | 6 |
| fork_ipc.c | 7 | 109 | 23 |
| futex.c | 7 | 0 | 13 |
| proc_ptrace.c | 0 | 0 | 0 |
| syscall_fd.c | 8 | 0 | 7 |
| syscall.c | 15 | 15 | 19 |
| syscall_io.c | 29 | 0 | 20 |
| syscall_time.c | 12 | 0 | 7 |
| syscall_sys.c | 12 | 0 | 8 |
| syscall_fs.c | 58 | 0 | 33 |

### 2.2 Limites fixes dans le code (LUES)

`FD_TABLE_SIZE` 1024 (`src/syscall.h:601`) ; `MAX_THREADS` 64 (`src/thread.h:25`) ; `PROC_TABLE_SIZE` 64 enfants (`src/syscall_proc.h:92`) ; `FUTEX_BUCKETS` 64 ; `EVENTFD_MAX` 32, `TIMERFD_MAX` 32, `SIGNALFD_MAX` 16 (`src/syscall_fd.c`), `INOTIFY_MAX` 16 ; 64 verrous de noms abstraits AF_UNIX (`src/syscall_net.c:156`) ; 256 kevents par `epoll_pwait`. Je n'ai pas mesuré le nombre de threads de Rocket League sous Wine : 64 threads est une limite dure inconnue face à ce jeu.

## 3. Interface minimale entre un moteur CPU et l'émulation du noyau

### 3.1 Deux modes d'intégration

Mode A (recommandé, INFÉRÉ). Garder le front-end x86-64 de FEX (`Source/Tools/LinuxEmulation/LinuxSyscalls`, environ 30 000 lignes, dont 406 enregistrements de syscall, 276 en passthrough pur) et remplacer le « noyau hôte » qu'il appelle. FEX appelle ce noyau de deux façons : par les 7 gabarits `SyscallPassthroughN<numéro arm64>` qui contiennent un `svc #0` en assembleur en ligne (`Syscalls/Passthrough.cpp:18-150`), et par 64 appels `::syscall(` plus quelques appels libc directs (`::poll` `Syscalls/FD.cpp:36`, `::select` et `::fcntl` `x64/FD.cpp:41-55`). Remplacer les 7 gabarits par un appel à une fonction `hlk_host_syscall(nr, a0..a5)` indexée par les numéros arm64 donne exactement le dispatch de HL. Les conversions x86-64 vers disposition hôte (par exemple `epoll_event_x86` packé vers l'`epoll_event` de 16 octets, `x64/EPoll.cpp:25-90`) restent faites par FEX. Les appels libc directs de FEX supposent des constantes Linux : ils doivent aussi passer par la couche (par exemple `RemapFromX86Flags` suppose les valeurs `O_*` d'un hôte Linux).

Mode B. Garder le dispatch de HL tel quel, l'indexer par les numéros x86-64 et jeter la couche Linux de FEX. Il faut alors refaire dans HL la table x86-64 (environ 330 numéros dont ~100 historiques), toutes les structures x86 (`stat`, `epoll_event`, `sigaction`, cadres de signal), le vDSO x86-64 (FEX en a un, `VDSO_Emulation.cpp`, 906 lignes) et la vue ptrace. À éviter.

Dans les deux modes, FEX traite lui-même quatre zones qui supposent un noyau Linux hôte (LU, source FEX intacte) : clone et fork (`Syscalls/Thread.cpp:179-399`, qui appelle `::syscall(SYSCALL_DEF(clone), ...)` à la ligne 258 et émule `vfork` par un fork suivi d'une attente, 284-299) ; les signaux (`SignalDelegator` garde les actions invitées, mais `kill`, `tkill`, `tgkill` sont des passthroughs vers le noyau hôte, `Passthrough.cpp:232,295,305`) ; exec (`x64/Thread.cpp:286-315` puis `Syscalls.cpp:199-440` : FEX relance l'hôte sur lui-même avec les arguments du chargeur, ce qui correspond naturellement à une ré-exécution du runtime sous Darwin) ; ptrace (`Syscalls/Info.cpp:96-113` : PEEKTEXT, PEEKDATA, POKETEXT, POKEDATA, ATTACH, DETACH en passthrough « pour que Wine lance le launcher Ubisoft », tout le reste renvoie EPERM). `prctl` envoie toutes les options sauf SECCOMP et GET_AUXV à la libc hôte (`Syscalls/Thread.cpp:479-516`), `personality` est mémorisé par thread (`Syscalls/Info.cpp:72`) et `seccomp` est émulé par un `SeccompEmulator` en espace utilisateur (`Info.cpp:93`). Ce sont ces zones que la couche processus de HL doit remplacer ; les écarts HL sur `personality` et `seccomp` sont donc sans objet en mode A.

### 3.2 Esquisse d'API C (frontière étroite)

Principe : le moteur possède le CPU, la pile du thread hôte et le cadre de signal ; la couche noyau possède l'identité, les tables, les signaux en attente, les arrêts ptrace et les objets noyau. Sept points d'entrée suffisent.

```c
/* hlk.h : frontière entre un moteur CPU (hôte FEXCore) et la couche noyau Linux-sur-Darwin */
typedef struct hlk_task hlk_task;          /* un thread invité : TID, masque, pile alternative, lien ptrace */

/* 1. mémoire invitée. Adresse invitée = adresse hôte (ou base + adresse, voir 3.3). Jamais de déréférencement direct dans les gestionnaires. */
int     hlk_user_read (uint64_t gva, void *dst, size_t n);        /* 0 ou -EFAULT, sûr face aux fautes */
int     hlk_user_write(uint64_t gva, const void *src, size_t n);
int64_t hlk_user_strn (uint64_t gva, char *dst, size_t max);      /* longueur, -EFAULT ou -ENAMETOOLONG */

/* 2. entrée de syscall : appelée par le moteur à chaque SYSCALL invité, thread propriétaire */
typedef struct { uint64_t nr, a[6]; } hlk_sysreq;                  /* RAX, RDI, RSI, RDX, R10, R8, R9 */
typedef enum { HLK_RETURN, HLK_CONTEXT_REPLACED, HLK_EXIT_THREAD,
               HLK_EXIT_GROUP, HLK_RESTART } hlk_disp;
hlk_disp hlk_syscall(hlk_task *t, const hlk_sysreq *rq, int64_t *rax);
   /* RETURN: écrire rax. CONTEXT_REPLACED: exec ou sigreturn ont déjà posé le contexte par engine->set_regs.
      RESTART: SA_RESTART, le moteur recule RIP de 2 et remet RAX = nr. */

/* 3. signaux asynchrones : le moteur appelle hlk_signal_next aux points sûrs (retour de syscall, après kick, après faute) */
typedef struct { int signo; uint8_t info[128]; uint64_t handler, flags, restorer, new_mask;
                 uint64_t alt_sp, alt_size; int use_alt; } hlk_delivery;
int  hlk_signal_next(hlk_task *t, hlk_delivery *d);               /* 1 livrer, 0 rien, -1 le processus meurt */
void hlk_signal_returned(hlk_task *t, uint64_t mask, uint64_t sp);/* après le sigreturn du moteur */

/* 4. fautes synchrones : le moteur traduit sa faute en (signo, si_code, adresse, err, trapno) */
void hlk_fault(hlk_task *t, int signo, int si_code, uint64_t addr, uint64_t err, uint64_t trapno);

/* 5. services du moteur appelés par la couche noyau (enregistrés une fois) */
typedef struct hlk_engine_ops {
  int  (*get_regs)(hlk_task*, hlk_x86_regs *r);      /* user_regs_struct, valable à un point sûr ou thread arrêté */
  int  (*set_regs)(hlk_task*, const hlk_x86_regs *r);
  int  (*get_fp)(hlk_task*, void *xsave, size_t *n);  int (*set_fp)(hlk_task*, const void*, size_t);
  void (*kick)(hlk_task*);                            /* atteindre un point sûr bientôt : pthread_kill sur le signal réservé */
  int  (*spawn)(hlk_task *child, const hlk_x86_regs *start, uint64_t fs_base);
  void (*invalidate_code)(uint64_t start, uint64_t len); /* POKE, mprotect, écriture dans une page de code */
  int  (*frame_build)(hlk_task*, const hlk_delivery*);   /* pousse rt_sigframe x86-64, règle RIP/RSP/RDI/RSI/RDX */
  int  (*frame_restore)(hlk_task*, uint64_t *mask, uint64_t *sp);
  void (*reset_for_exec)(void);                          /* ou ré-exécution de l'hôte */
} hlk_engine_ops;

/* 6. fork : le moteur appelle fork() lui-même entre ces deux appels */
int  hlk_fork_prepare(hlk_task *t, uint64_t clone_flags);  /* verrous, registre familial, kqueue à reconstruire */
void hlk_fork_finish (hlk_task *t, pid_t host_child);      /* host_child == 0 dans l'enfant */

/* 7. exec */
int  hlk_exec_prepare(hlk_task *t, const char *path, char *const argv[], char *const envp[], hlk_image *img);
void hlk_exec_commit (hlk_task *t, const hlk_image *img); /* image installée : arrêt TRACEEXEC, remise à zéro des signaux */

/* + arrêt ptrace (appelé par le moteur aux points d'arrêt) */
typedef enum { HLK_STOP_SYSCALL_ENTRY, HLK_STOP_SYSCALL_EXIT, HLK_STOP_SIGNAL, HLK_STOP_EXEC, HLK_STOP_FAULT } hlk_stop;
int  hlk_ptrace_stop(hlk_task *t, hlk_stop kind, int signo, int *resume_signo);
```

### 3.3 Règles de propriété et de verrous

- Seul le thread propriétaire appelle `hlk_syscall`, `hlk_signal_next` et `hlk_ptrace_stop`. Un thread arrêté pour ptrace a son état x86 dans la mémoire ordinaire du processus (`FEXCore::Core::CPUState`, `CoreState.h`, `gregs[16]`, `rip`, `xmm`, `flags[48]`) : un thread auxiliaire du même processus peut le lire sans le réveiller. La contrainte HVF « seul le vCPU propriétaire lit ses registres » (`src/thread.c:607-616`) disparaît.
- Ordre des verrous à conserver (`src/syscall_internal.h:10-38`) : mmap (1), fd (3), signal (4), table de threads (5), pid (6), seaux de futex (7). Le moteur ne doit jamais tenir un verrou du noyau pendant qu'il exécute du code invité.
- Mémoire invitée : garder les trois fonctions `hlk_user_*` comme seul accès. Avec l'identité elles font une copie protégée contre les fautes (FEX fournit `FaultSafeUserMemAccess` : `CopyFromUser` et `CopyToUser` en assembleur arm64 avec un PC de faute connu, `FaultSafeUserMemAccess.cpp:1-80`). Si un décalage de base invité devient nécessaire pour Wine (section 0), seule cette couche change, plus les pointeurs imbriqués dans les structures (`iovec`, `msghdr`, `sigaction`), pour lesquels HL a déjà des convertisseurs (`append_guest_iov`, `src/syscall_io.c:279-301`).
- Signaux : un seul signal hôte réservé sert de « kick » (SIGUSR2 est déjà pris par les E/S hôte de HL, `src/thread.c:52`) ; FEX réserve ses propres signaux, il faut un plan d'allocation. Darwin n'a pas de signaux temps réel ni de `tgkill` ni de `rt_sigqueueinfo` : les signaux invités doivent rester des files logicielles (comme `src/syscall_signal.c:37-47`), le signal hôte ne servant qu'à interrompre.
- Identité : PID et TID virtuels du compteur familial, pas des identifiants macOS. Le TID du thread principal doit égaler le PID (attente de Linux, que les identifiants Mach ne satisfont pas).

### 3.4 Ce qui dépend d'ARM64 Linux (liste des conversions x86-64)

1. Numéros et registres du syscall : `src/syscall.h:17-170`, `case SYS_*` dans `src/syscall.c:1277-2463` (182 lignes), lecture de x0-x5 et x8 (`src/syscall.c:1249-1255`), résultat dans X0 et abus de X8 comme drapeau TLBI du shim (2495-2507, sans objet en x86-64). x86-64 : RAX, RDI, RSI, RDX, R10, R8, R9 ; RCX reçoit le RIP suivant et R11 les RFLAGS.
2. Ordre des arguments de `clone` : arm64 `(flags, stack, ptid, tls, ctid)` (`src/syscall.c:2227`, `src/fork_ipc.c:1290-1293`) ; x86-64 `(flags, stack, ptid, ctid, tls)`.
3. Redémarrage de syscall : PC moins 4 (`src/syscall_signal.c:403,1037`) ; x86-64 : RIP moins 2 et RAX restauré depuis `orig_rax`, qui n'existe pas dans HL.
4. Cadre de signal : `linux_rt_sigframe_t`, FPSIMD et ESR (`src/syscall_signal.h:133-172`, `.c:841-899,1021-1186,1190-1255`). x86-64 : `rt_sigframe` avec `pretcode` pris dans `sa_restorer` (pas de restaurateur vDSO), saut de la zone rouge de 128 octets, alignement de pile, `sigcontext` x86 (r8..r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags, cs/gs/fs, err, trapno, cr2), zone xsave avec magic `FP_XSTATE_MAGIC1`. FEX l'a écrit (`SignalDelegator/GuestFramesManagement.cpp`, 856 lignes).
5. Traduction des fautes : décodage d'ESR_EL1, FSC, ACCERR/MAPERR (`src/syscall_proc.c:1767-1887`). x86-64 : #PF (code d'erreur, cr2), #UD (SIGILL), #DE (SIGFPE, `FPE_INTDIV`), `int3` (SIGTRAP avec `si_code = SI_KERNEL`, RIP après l'octet), pas à pas (`TRAP_TRACE`).
6. Pointeur de thread : TPIDR_EL0 (`src/fork_ipc.c:1484-1489`). x86-64 : base FS posée par `CLONE_SETTLS` ou `arch_prctl(ARCH_SET_FS)` ; base GS pour le TEB de Wine.
7. Vues ptrace : `linux_user_pt_regs_t` (34 mots, `src/syscall.h:215-220`), `linux_user_fpsimd_state_t`, `NT_ARM_HW_WATCH`, convention X7 aux arrêts syscall (`probes/regset-architecture.md:18`). x86-64 : `user_regs_struct` 27 mots, `user_fpregs_struct` 512 octets, `NT_X86_XSTATE`, registres de debug par PEEKUSER et POKEUSER (Wine server en dépend, `voile-wine/server/ptrace.c:643-721`), requêtes GETREGS, SETREGS, GETFPREGS, SETFPREGS, `PTRACE_ARCH_PRCTL`, et à l'arrêt de syscall : `rax = -ENOSYS`, `orig_rax` = numéro, `rcx` et `r11` selon la sémantique de `syscall`.
8. `struct epoll_event` : 16 octets en arm64 (`src/syscall_poll.c:583-588`), 12 octets packés en x86-64.
9. Valeurs de `O_*`, `struct stat`, `statfs`, `AT_*` et auxv (vfs et chargeur, coéquipier) : `O_DIRECTORY`, `O_NOFOLLOW`, `O_DIRECT`, `O_LARGEFILE` diffèrent (`CLAUDE.md` de HL, section « aarch64-linux Open Flag Values »).
10. Syscalls x86-64 sans équivalent arm64 : une centaine d'appels historiques (`fork` 57, `vfork` 58, `pause` 34, `alarm` 37, `poll` 7, `select` 23, `dup2` 33, `pipe` 22, `epoll_create` 213, `epoll_wait` 232, `eventfd` 284, `signalfd` 282, `inotify_init` 253, `getpgrp` 111, `time` 201) et propres à x86 (`arch_prctl` 158, `modify_ldt` 154, `iopl`, `ioperm`). `fork` et `vfork` x86 passent par la même `ForkGuest` que dans FEX (`Syscalls/Thread.cpp:401-436`).

## 4. Écarts face à Wine et EAC

Légende : présent (P), partiel (PA), absent (A). « Statut » concerne l'arbre HL en l'état.

### 4.1 Ce que Wine et EAC utilisent réellement

MESURÉ par script sur 18 journaux `eac.log`/`control.log` de 8 répertoires d'essais EAC (`probes/eac-*`) : événements `unimplemented_syscall` du runtime privé, avec un budget de 5 par processus (champ `budget`) donc non exhaustifs. Numéros arm64 : 10 (`fgetxattr`, 304 événements, 7 runs, voir 8), 282 (`userfaultfd`, 120, 7 runs, ENOSYS légitime), 153 (`times`, 26, 7 runs). Le même run `eac-ptrace-poke-20260930` journalise 24 `sys_exit_group`, 13 `exec_shim_diagnostic`, 65 `wait4` : plusieurs dizaines de processus invités.

Wine (LU dans `voile-wine/server`, utilisé comme proxy du code serveur de Wine 10 Debian que les essais HyperLinux exécutaient ; identité des deux sources non vérifiée) : `send_thread_signal` envoie SIGUSR1 aux clients par `tkill(unix_pid, unix_tid, sig)` pour suspendre, interrompre et terminer des threads (`thread.c:961,1451,1468`, `ptrace.c:258-266`), SIGQUIT à la mort violente (`thread.c:1628`), et `kill(process->unix_pid, signal)` (`process.c:787`). La lecture et l'écriture mémoire inter-processus utilisent d'abord `process_vm_readv` et `process_vm_writev`, puis, si ENOSYS ou EPERM, ptrace ATTACH, waitpid(SIGSTOP, `__WALL`), PEEKDATA, POKEDATA, DETACH (`ptrace.c:182-300,336-416,474-492`). Les registres de debug passent par ATTACH, PEEKUSER, POKEUSER, DETACH (`ptrace.c:625-725`). Donc, avant EAC, Wine exige déjà le `tgkill` inter-processus et soit `process_vm_*`, soit ptrace sur des non-enfants.

### 4.2 ptrace

| Requête | Valeur | Preuve d'usage | Statut distant HL | Remarque pour une vue x86 |
|---|---|---|---|---|
| TRACEME | 0 | EAC (enfant 148) | P | lien créé au fork seulement |
| PEEKTEXT, PEEKDATA | 1, 2 | EAC (PEEKDATA, run poke) ; Wine server (repli) | A (EINVAL) | agent coopératif ou tâche Mach du tracee |
| PEEKUSER, POKEUSER | 3, 6 | Wine server (DR0-DR7) | A | `struct user` x86, offsets des registres de debug |
| POKETEXT, POKEDATA | 4, 5 | EAC (octet 0xCC) ; Wine server (repli) | A | écriture dans une page RX, puis invalidation du cache de traduction |
| CONT | 7 | EAC | P | |
| KILL | 8 | non observé (EAC utilise kill) | A | |
| SINGLESTEP | 9 | non observé (défini dans `voile-wine/server/ptrace.c:64-65`, jamais appelé) | A | TF et `TRAP_TRACE` |
| GETREGS, SETREGS | 12, 13 | non observé | A | requêtes x86 |
| GETFPREGS, SETFPREGS | 14, 15 | non observé | A | 512 octets |
| ATTACH | 16 | Wine server | A (non-enfant) | registre familial des processus |
| DETACH | 17 | Wine server, EAC possible | P (enfant) | |
| SYSCALL | 24 | EAC (146 fois) | P | arrêts aux frontières FEX |
| SETOPTIONS | 0x4200 | EAC (0x11) | PA (masque 0x11 seul) | TRACECLONE, TRACEFORK, TRACEEXIT, EXITKILL absents |
| GETEVENTMSG | 0x4201 | EAC possible | P | |
| GETSIGINFO, SETSIGINFO | 0x4202, 0x4203 | non observé | A distant (local Rosetta seulement) | |
| GETREGSET | 0x4204 | EAC (66 fois, 216 octets) | PA (272 octets ARM64) | 216 octets = `user_regs_struct` x86 |
| SETREGSET | 0x4205 | EAC (RIP moins 1) | A dans l'arbre (candidat non qualifié, 74/76) | |
| SEIZE, INTERRUPT | 0x4206, 0x4207 | non observé en distant | A distant | |
| `PTRACE_GET_SYSCALL_INFO` | 0x420e | non observé | A | |

### 4.3 Drapeaux de clone et création de processus

| Élément | Statut | Détail |
|---|---|---|
| `pthread_create` glibc (0x3d0f00 plus SETTLS, PARENT_SETTID, CHILD_CLEARTID) | P | `sys_clone_thread` |
| `fork()` glibc (0x1200011) | PA | pas d'écriture de `ctid`, pas de SIGCHLD, pas de `SETTLS` |
| `vfork`, `posix_spawn` (`CLONE_VM|CLONE_VFORK`) | A (chemin vm-clone) | INFÉRÉ cassé, non testé |
| `CLONE_PTRACE`, `CLONE_UNTRACED`, `CLONE_PARENT`, `CLONE_PIDFD`, `CLONE_NEW*` | A | ignorés sans erreur |
| `clone3` | A | ENOSYS, repli glibc |
| Limite de threads | PA | 64 par processus |

### 4.4 Signaux, kill, tgkill

| Élément | Statut | Détail |
|---|---|---|
| `rt_sigaction`, `rt_sigprocmask`, `sigaltstack`, `rt_sigpending`, `rt_sigreturn` | P | cadre ARM64 |
| `rt_sigsuspend` | PA | renvoie EINTR sans dormir |
| `rt_sigtimedwait`, `rt_sigqueueinfo` | A | |
| `rt_tgsigqueueinfo` | PA | intra-processus |
| `kill`, `tkill`, `tgkill` intra-processus | P | `SI_TKILL` honoré |
| `kill` vers un enfant | PA | SIGKILL et SIGCONT seulement |
| `kill`, `tgkill` vers un autre processus | A | requis par Wine server |
| SIGCHLD à la mort d'un enfant | A | |
| Signaux temps réel 32 à 64 | P | file de 32 |
| `signalfd4` | PA | 16 emplacements, local |
| ITIMER_REAL | P | émulé ; ITIMER_VIRTUAL et PROF non |
| Attentes interrompues par un signal invité | PA | oui pour read et write, poll, select, epoll sans délai, wait4, futex ; non pour `nanosleep`, `clock_nanosleep`, `accept`, `connect`, `recv*`, `send*` (1.2) |

### 4.5 prctl, rlimits, personality, seccomp, io_uring, pidfd

| Élément | Statut | Détail |
|---|---|---|
| `prctl` SET_NAME, GET_NAME | P | registre des noms |
| `prctl` SET_PDEATHSIG | PA | stub qui renvoie 0 |
| `prctl` NO_NEW_PRIVS, DUMPABLE | PA | stubs (`src/syscall.c:1857-1882`) |
| `prctl` SET_MEM_MODEL | PA | EINVAL, voulu pour Rosetta |
| `prctl` SECCOMP, PTRACER (Yama), CHILD_SUBREAPER, TIMERSLACK, SYSCALL_USER_DISPATCH, CAPBSET | A | ENOSYS (Linux renvoie EINVAL pour une option inconnue) |
| `getrlimit`, `setrlimit`, `prlimit64` | PA | ressources 0 à 9 vers les limites macOS ; `pid` ignoré ; 10 à 15 (LOCKS, SIGPENDING, MSGQUEUE, NICE, RTPRIO, RTTIME) renvoient EINVAL (`syscall_sys.c`) |
| `personality` | A dans HL | émulé par FEX lui-même (`Info.cpp:72`) en mode A |
| `seccomp` (syscall) et `prctl(PR_SET_SECCOMP)` | A dans HL | émulés par le `SeccompEmulator` de FEX (`Info.cpp:93`, `Thread.cpp:485-492`) en mode A |
| `io_uring_*` | A | FEX répond aussi « unimplemented safe » (`Passthrough.cpp:360-362`) |
| `pidfd_open`, `pidfd_send_signal`, `pidfd_getfd` | A | |
| `process_vm_readv`, `process_vm_writev` | A | première option de Wine server et d'EAC |
| `unshare`, `setns`, namespaces | A | |
| `sched_setscheduler`, `sched_getscheduler`, `sched_getparam`, `sched_get_priority_*` | A | `sched_setaffinity` PA (stub 0), `sched_getaffinity` P |
| `getcpu`, `times`, `capget`, `capset`, `setgroups`, `getsid` | A | `times` observé dans les journaux EAC |

### 4.6 futex

| Opération | Statut |
|---|---|
| WAIT, WAKE, REQUEUE, CMP_REQUEUE, WAKE_OP | P (locale au processus) |
| WAIT_BITSET, WAKE_BITSET | PA (horloge absolue traitée en temps réel) |
| LOCK_PI, UNLOCK_PI, TRYLOCK_PI | PA (pas d'héritage de priorité, pas d'OWNER_DIED) |
| WAIT_REQUEUE_PI, CMP_REQUEUE_PI, LOCK_PI2 | A |
| `FUTEX_PRIVATE_FLAG` | PA (ignoré ; tout est privé) |
| futex partagé entre processus | A |
| `set_robust_list` | PA (stub) ; `get_robust_list` A |
| `futex_waitv`, `futex_wake`, `futex_wait`, `futex_requeue` (449, 454-456) | A |

Primitive Darwin candidate pour le partagé : `os_sync_wait_on_address` avec le drapeau « shared » (le coéquipier la qualifie dans `g1_futex`, non exécuté à ce jour).

### 4.7 Polling et objets de notification

| Élément | Statut | Détail |
|---|---|---|
| `ppoll`, `pselect6` | P | |
| epoll IN, OUT, ERR, HUP, RDHUP, ET, ONESHOT | P | kqueue |
| epoll PRI, EXCLUSIVE, WAKEUP, `epoll_pwait2` | A | |
| eventfd (compteur, sémaphore, nonblock) | PA | local, 32 max, pas partageable |
| timerfd | PA | kqueue, `ABSTIME` ignoré, 32 max, non héritable |
| signalfd | PA | local, 16 max |
| inotify | PA | kqueue vnode, 16 instances, pas de cookie de renommage, noms de répertoire parfois absents, pas de récursion (`src/syscall_inotify.c:1-20`) |

### 4.8 Sockets

| Élément | Statut | Détail |
|---|---|---|
| AF_UNIX flux et datagramme, `socketpair` | P | `SCM_RIGHTS` avec traduction fd invité vers hôte et disposition `cmsghdr` (`src/syscall_net.c:1296-1352,1564-1600`) |
| AF_UNIX abstrait | PA | chemin dérivé par hachage FNV plus verrou `flock` (`:60-200`) |
| AF_INET, AF_INET6, TCP, UDP | P | numéros d'option traduits (`src/syscall_net.h:36-62`) |
| DNS | PA | simples sockets UDP et TCP vers le serveur de `resolv.conf` du vfs ; aucune résolution émulée |
| `SOCK_SEQPACKET` | PA | simulé en `socketpair` flux pour rosettad seulement (`:538-570`) |
| `SO_PEERCRED`, `SCM_CREDENTIALS`, `SO_PASSCRED` | A | |
| AF_NETLINK, AF_PACKET | A | « best effort passthrough » (`:40-48`) : les numéros Linux 16 et 17 désignent `AF_APPLETALK` et `AF_ROUTE` dans le SDK de macOS (`sys/socket.h:383-384`), il n'existe pas d'`AF_NETLINK` ; `getifaddrs` de glibc passe par netlink |
| options de socket | PA | 17 options `SOL_SOCKET`, 4 options TCP, 3 options IP et IPv6 traduites (`src/syscall_net.h:36-62`) |

### 4.9 Couverture des syscalls en passthrough de FEX

INFÉRÉ par comparaison de noms (script : noms de `Passthrough.cpp` contre les `SYS_*` de `src/syscall.c` et `.h`) : sur 241 noms de passthrough, 128 sont couverts, 113 non couverts. Non couverts, les plus pertinents pour Wine et EAC : `capget`, `capset`, `futex_waitv`, `futex_wait`, `futex_wake`, `futex_requeue`, `get_robust_list`, `getcpu`, `getsid`, `mlock2`, `mlockall`, `pidfd_getfd`, `pidfd_open`, `pidfd_send_signal`, `process_vm_readv`, `process_vm_writev`, `rt_sigqueueinfo`, `sched_get_priority_max`, `sched_get_priority_min`, `sched_getparam`, `sched_getscheduler`, `sched_setscheduler`, `sched_rr_get_interval`, `setgroups`, `settimeofday`, `clock_settime`, `times`, `timer_create`, `timer_settime`, `timer_gettime`, `timer_delete`, `unshare`, `setns`, `msgget`, `msgsnd`, `msgrcv`, `msgctl`, `semget`, `semop`, `semtimedop`, `io_setup`, `io_submit`, `io_getevents`, `ioprio_set`, `ioprio_get`, `syncfs`, `sync_file_range`, `readahead`, `fanotify_init`. La liste complète est dans mon historique de commande ; le total dépend de la normalisation des noms (par exemple `fadvise64`).

## 5. Infrastructure de test

### 5.1 Compiler

- Ce que fait le Makefile de HL : `HL_SRCS` (39 sources), CFLAGS `-O2 -std=c11 -Werror=vla -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wimplicit-fallthrough -Wundef -Wnull-dereference -Wno-unused-parameter` (`Makefile:74-77`), shim assemblé par `as -arch arm64` puis extrait en `shim_blob.h`, signature avec `entitlements.plist` (`Makefile:30,169`). Les tests invités demandent une chaîne `aarch64-unknown-linux-musl-` ou des binaires prébuilts (`GUEST_TEST_BINARIES`) fournis par `nix develop` ; `nix` n'est pas installé ici (`probes/ptrace-remote-setregset-runtime-20261001/README.md:29-30`).
- Compilation seule dans la copie de travail, sans exécution : `bash EXP/tools/build-hl-scratch.sh`. MESURÉ : 39 objets, 0 échec, 0 avertissement, 0 erreur, lien réussi, 2,9 s (`logs/hl-compile.log` vide). Le binaire `_build/hl` n'est pas signé avec l'entitlement HVF et n'a pas été lancé.

### 5.2 Tests purs hôte (sans HVF)

Lancer : `bash EXP/tools/build-run-host-tests.sh` (2,7 s). MESURÉ, rejoué dans cette session (`logs/host-run-all-rerun.log`) :

| Programme | Résultat | Lien avec la couche processus |
|---|---|---|
| test-fd-object-lite | PASS, 13 lignes OK | objets fd |
| test-vfs-unit | PASS, 16 lignes OK | vfs (coéquipier) |
| test-app-open | PASS | sans rapport |
| test-oss-abi | PASS | sans rapport |
| test-fd-digest | PASS, 9 | sans rapport |
| test-aot-format | PASS, 7 | Rosetta, sans rapport |
| test-thread-vcpu-lifetime | PASS, 7, avec `scratch-hl/stub-trace.c` | `thread.c`, interruption et destruction de vCPU |
| test-page-table-pool | PASS, 7 | mémoire (coéquipier) |
| proc-ptrace-host (source `probes/proc-ptrace-host.c`, compilé par moi dans `EXP/scratch-hl/probe-bin`) | 79 PASS, 0 FAIL (`logs/host-test-proc-ptrace-host.log`) | `proc_ptrace.c` complet : 23 contrôles de base, options, syscalls, registres, arrêts concurrents |
| kq_fork_scm (écrit par moi, `EXP/scratch-hl/probe-src/kq_fork_scm.c`) | voir 1.4 (`logs/host-test-kq-fork-scm.log`) | kqueue, fork, `SCM_RIGHTS` |

Échec connu : avec le Makefile tel quel, `test-thread-vcpu-lifetime` ne se lie pas (`_proc_trace_tracer_exit` indéfini ; `thread.c:248` appelle `proc_trace_tracer_exit`, que la recette `test-host-units` ne lie pas, `Makefile:409`), voir `logs/host-build-test-thread-vcpu-lifetime-asis.log`. Le contournement est un stub dans le seul répertoire de travail. Non exécutés : les quatre tests audio (CoreAudio) et `test-diagnostics.sh` (demande un invité).

Les tests `test/test-*.c` (69 programmes, plus 6 `helper-*.c` et 15 scripts) sont des programmes invités : ils demandent `hl` signé et HVF, donc ils ne se lancent pas ici. Pour une couche FEX : 77 binaires x86-64 statiques musl sont déjà compilés dans `test-binaries/amd64/bin` (liste complète : `ls`), dont les tests de processus `test-fork`, `test-cow-fork`, `test-fork-exec`, `test-exec`, `test-pthread`, `test-thread`, `test-futex-pi`, `test-signal`, `test-signal-thread`, `test-signalfd`, `test-timerfd`, `test-eventfd`, `test-epoll`, `test-poll`, `test-poll-ready`, `test-poll-wakeup`, `test-socket`, `test-net`, `test-inotify`, `test-tgkill-target`, `test-sigpipe-survival`, `test-sigill`, `test-stress`. La matrice de référence (43 programmes choisis par architecture, `test-binaries/TEST-REPORT.md`) est pilotée par `run-unit-matrix.sh` et `bounded-hl.py` (15 s par invité, groupe de processus propre) ; remplacer l'argument `HL` par un exécutable `fexdarwin` suffit. Politique d'échecs attendus : `test/matrix-xfails.sh` (`test-signal` et `test-signal-thread` échouent sous Rosetta à cause de `SA_RESETHAND` ; ne s'appliquera pas à FEX).

### 5.3 Oracles

Aucun oracle Linux n'est exécutable ici sans VM. Oracles enregistrés réutilisables hors ligne : références Linux ARM64 et x64 sous OrbStack (`probes/ptrace-lifecycle/results`, 7/7, 13/13, 9/9, 40/40, 24/24 selon la fixture), références x86 natives sous QEMU TCG (`probes/ptrace-x86-full-context-control-20260929`, 272/272 ; `probes/ptrace-setregset-control-20261001/qemu-*`, 76/76 en cinq modes). Les sources des fixtures sont dans `probes/ptrace-*/`. Compilation locale d'invités x86-64 : ni `zig`, ni `x86_64-linux-gnu-gcc`, ni `ld.lld` ne sont dans le `PATH` (mesuré par `which`) et l'éditeur de liens d'Apple ne produit pas d'ELF, mais le venv partagé contient `ziglang 0.16.0` (`EXP/tools/venv`, lecture seule de ma part). MESURÉ : `"$PY" -m ziglang cc -target x86_64-linux-musl -static -O2 -I<hyper-linux/test> <test>.c -o <sortie> -lpthread` produit un ELF x86-64 statique pour 5 programmes sur 5 essayés (`test-fork`, `test-pthread`, `test-futex-pi`, `test-signal-thread`, `test-epoll`), sortie dans `EXP/scratch-hl/guest-x86_64/`, journaux `logs/host-build-guest-*.log`, sans Docker. Aucun de ces binaires n'a été exécuté (pas de runtime). Les 77 binaires prébuilts (conteneur Debian, `test-binaries/build-guests.sh`) restent la référence de comparaison avec la matrice existante. Les journaux d'oracle restent utilisables comme valeurs attendues.

## 6. Estimation en lignes de code et cinq risques

### 6.1 Lignes

INFÉRÉ, plus ou moins 30 %, calculé depuis les plages de lignes de la section 2 et la lecture des fichiers.

| Poste | Lignes |
|---|---|
| À garder ou adapter mécaniquement (A) | 9 500 |
| À réécrire sur le même principe (R) | 2 300 |
| À abandonner (D) | 6 700 |
| Neuf : colle fork, clone, exec sur FEX (fork natif, threads, ré-exécution hôte, émulation de vfork, réinitialisation du registre dans l'enfant) | 900 à 1 300 |
| Neuf : signaux (boîte aux lettres inter-processus, SIGCHLD, `sigsuspend` bloquant, `rt_sigtimedwait`, `rt_sigqueueinfo`, pont de cadre x86) | 1 100 à 1 600 |
| Neuf : wait et statut (registre des statuts de sortie Linux, `WIFSIGNALED`, groupes et sessions minimaux) | 400 à 700 |
| Neuf : ptrace x86 (GETREGS/SETREGS/REGSET prstatus, fpregs, xstate, PEEK/POKE/USER, agent coopératif, invalidation, SINGLESTEP, KILL, ATTACH/SEIZE/INTERRUPT, événements CLONE/FORK/EXIT) | 2 200 à 3 200 |
| Neuf : futex partagé, robust list, waitv, horloges | 500 à 900 |
| Neuf : objets spéciaux partageables et sûrs au fork | 300 à 500 |
| Neuf : syscalls manquants (sched_*, getcpu, times, capget, process_vm_*, pidfd_*, personality) | 600 à 1 000 |
| Neuf : tests hôte des modules neufs | 1 200 à 2 000 |
| Total neuf | 7 200 à 11 200 |

Ces chiffres supposent le mode A (section 3.1) et ne comptent pas le portage de la couche Linux de FEX sur Darwin (clone, signaux hôte, allocateur), qui relève du rapport FEXCore.

### 6.2 Les cinq risques les plus lourds

1. ptrace x86 coopératif entre deux processus, avec cohérence du cache de traduction (EAC). Le seul risque qui dépend d'une relation ptrace noyau : EAC appelle le ptrace du monde invité, et macOS n'offre pas de relation équivalente (le `ptrace(2)` de Darwin ne voit que l'état arm64 hôte, et `task_for_pid` demande une autorisation que la consigne interdit de déclencher). Il faut donc l'émulation décrite en 6.3. Points durs : reconstruire un contexte x86 exact à des arrêts qui se trouvent dans du code JIT ou dans un gestionnaire (`ReconstructCompactedEFLAGS`, `RestoreRIPFromHostPC` dans `Context.h`) ; la sémantique x86 de RCX, R11 et `orig_rax` aux arrêts syscall (mesurée sur Rosetta : `probes/ptrace-x86-feasibility-20260929.md:13,47-51`) ; l'invalidation du cache de traduction du tracee après un POKE (échec constaté avec Rosetta : traduction périmée après restauration de l'octet, 74/76, `probes/ptrace-remote-setregset-runtime-20261001/README.md:32-44`) ; l'accès mémoire du tracee depuis un autre processus.
2. Livraison asynchrone des signaux invités sur Darwin. Pas de signaux temps réel, pas de `tgkill`, pas de `rt_sigqueueinfo`, un seul signal hôte réservé disponible pour interrompre du code JIT à un point exact. Wine server suspend, interrompt et termine des threads avec SIGUSR1 inter-processus, glibc utilise des signaux temps réel pour l'annulation et `setxid`. Il faut des files logicielles, une boîte aux lettres inter-processus, et des attentes (`futex`, `poll`, `read`, `accept`, `wait4`) toutes interruptibles avec la bonne sémantique d'EINTR et de redémarrage. Ne dépend pas d'un ptrace noyau.
3. Synchronisation partagée entre processus : futex sur sections partagées, eventfd d'esync, wait et SIGCHLD. HL n'a rien de partagé (futex, compteurs d'eventfd, tables d'enfants). Candidat : `os_sync_wait_on_address` en mode partagé, à qualifier. Ne dépend pas d'un ptrace noyau.
4. fork, exec et vfork sur un processus qui contient un JIT vivant. Verrous détenus par des threads absents (MESURÉ par le coéquipier : mutex, `os_unfair_lock` et stdio restent verrouillés dans l'enfant et bloquent un petit-enfant, `evidence/darwin-process/f1_fork_mt.log`), kqueue non hérités (MESURÉ ici), registre familial à faire survivre à un exec hôte, sémantique de `CLONE_VM|CLONE_VFORK` qu'un fork ne reproduit pas (l'erreur d'exec que glibc renvoie au parent par la pile partagée ne lui parviendrait jamais ; FEX a la même limite, `Syscalls/Thread.cpp:293-294`). Ne dépend pas d'un ptrace noyau.
5. Fidélité de Linux dans polling et sockets, et limites dures. kqueue au lieu d'epoll (EXCLUSIVE, PRI), `epoll_event` packé, AF_NETLINK absent, `SO_PEERCRED` absent, 64 threads, 1024 fd, objets spéciaux limités à 16 ou 32, `SIGCHLD` absent. Chacun est petit, la somme décide si Wine démarre. Ne dépend pas d'un ptrace noyau.

### 6.3 Émulation en espace utilisateur de la relation traceur/tracee entre deux processus du runtime

Principe repris de HL (déjà testé hôte : 79/79) et adapté à FEX.

1. À la création d'un enfant par `fork`, le parent crée un fichier d'état non lié, de taille fixe, mappé `MAP_SHARED`, et une paire de sockets Unix. L'enfant en hérite, personne d'autre (`proc_trace_create`, `proc_trace_import`). Un registre familial par TID (comme `proc_names.c`) permet à un tiers de retrouver une relation ; il faudra l'étendre pour ATTACH et SEIZE d'un non-enfant (nouveau : rendez-vous par socket nommé dans un répertoire de famille).
2. Exclusion mutuelle : verrou d'octet `fcntl` plus mutex local. Il se libère à la mort du propriétaire ; MESURÉ par le test hôte.
3. TRACEME, SETOPTIONS, CONT, DETACH, SYSCALL, GETEVENTMSG sont des écritures dans ce fichier. Le traceur ne touche jamais au tracee directement.
4. Arrêt : le tracee atteint un point sûr de FEX (retour de `HandleSyscall` entrée ou sortie, livraison d'un signal par le `SignalDelegator`, faute SIGTRAP d'un `int3`, fin d'exec). Il reconstruit son contexte x86 depuis `CpuStateFrame` et, aux points asynchrones, depuis le contexte hôte du gestionnaire de signal. Il publie dans le fichier : numéro de séquence d'arrêt, type, signal, `user_regs_struct` (27 mots), et au besoin les fpregs. Il notifie le socket puis attend la reprise (`poll`), sans verrou tenu.
5. Le traceur attend par wait4 ou waitid : le statut `(événement << 16) | (signal << 8) | 0x7f` est reconstruit depuis la séquence (`proc_trace_wait`).
6. GETREGSET, GETREGS, GETFPREGS sont des lectures du fichier. SETREGSET, SETREGS, POKEUSER sont des commandes : le traceur écrit la commande dans le fichier avec la séquence d'arrêt visée et notifie ; le tracee, ou un thread auxiliaire du même processus, exécute et publie le résultat, comme le candidat SETREGSET de HL qui vérifie la séquence pour rejeter les écritures périmées.
7. PEEK et POKE : le tracee (ou son thread auxiliaire) fait la lecture ou l'écriture dans son espace d'adressage, avec `vm_protect` temporaire pour une page RX, puis appelle l'invalidation de code du moteur (FEX : `InvalidateGuestCodeRange` / `InvalidateThreadCachedCodeRange` de `Context.h`) avant de répondre. Ce choix évite à la fois `task_for_pid` et l'incohérence du cache. Alternative à qualifier par le coéquipier (série H) : obtenir le port de tâche du tracee par un message Mach envoyé par le tracee lui-même, sans autorisation `task_for_pid`.
8. Mort d'un pair : EOF du socket (`peer_closed`), détachement avec signal supprimé, comme sur Linux (références `probes/ptrace-lifecycle`).

Ce que ce schéma ne donne pas gratuitement : l'arrêt d'un thread qui s'exécute (PTRACE_INTERRUPT, SIGSTOP envoyé par le traceur) demande le « kick » du risque 2 ; l'arrêt de groupe ; le reparentage.

## 7. Pourquoi les correctifs privés existent

LU (diff, commentaires des hunks, `STATUS.md`, `probes/ptrace-*.md`). Lignes changées = insertions plus suppressions de `git diff --stat`.

| Sous-système | Fichiers (lignes changées) | Pourquoi |
|---|---|---|
| Threads | `thread.c/h` (392 plus 47) | Interrompre les E/S hôte bloquantes par un signal privé pour que `tgkill` ne soit pas perdu (`src/thread.c:47-140`) ; pile EL1 relogée (le shim passe en haut de la mémoire pour laisser la zone basse à Wine) ; arrêt ptrace avec siginfo, points de surveillance, FP, interruption explicite ; sémantique `WNOWAIT` et `waitid` ; nettoyage du traceur et des noms à la sortie |
| Processus | `syscall_proc.c/h` (991 plus 31) | Allocateur de PID familial, registre des noms, `wait4` et `waitid` réécrits (arrêts, continuation, `WNOWAIT`, redémarrage `SA_RESTART`, minuterie pendant l'attente), `sys_ptrace` distant, capture de registres avec provenance (`proc_regset_capture_context`) |
| Signaux | `syscall_signal.c/h` (444 plus 16) | Signaux dirigés par thread, `siginfo` complet, redémarrage `SA_RESTART` par arguments sauvegardés, arrêt ptrace avant la disposition, sémantique de fork (en attente effacés), SIGCHLD d'arrêt ptrace |
| Dispatch | `syscall.c/h` (608 plus 21) | Points d'arrêt ptrace ENTRY et EXIT (`syscall_trace_boundary`), liste des appels redémarrables, `tkill`/`tgkill` avec `SI_TKILL` (glibc `setxid`, musl annulation), `rt_tgsigqueueinfo` (Rosetta renvoie ses fautes), plus la mémoire (coéquipier) |
| Fork | `fork_ipc.c` (457) | IPC version 18 : liens ptrace avec ACK, allocateur de PID, noms, états reopen, reconstruction d'epoll, vues de fichiers, SHM, vDSO |
| Exec | `syscall_exec.c` (70) | Shebang avec argv intact, résolution VFS, vDSO mappé, événement TRACEEXEC |
| Réseau | `syscall_net.c` (123) | Tampons invités dispersés sur plusieurs mappings hôtes, introduits par les vues de fichiers partagées |
| Nouveaux modules | `proc_ptrace.c/h`, `proc_names.c/h`, `signal_notify.h`, `fault_dump.c/h` | Besoins EAC : `/proc/<pid>` d'un autre processus, relation ptrace entre processus, notification SIGCHLD d'arrêt, diagnostic de faute |
| Démarrage | `hl.c` (60), `shim.S` (22), `guest.c` (562), `vdso.c`, `stack.c` | Shim déplacé en haut de la mémoire invitée pour que Wine puisse mapper la zone basse (« Wine's DOS area », commentaire de `src/vdso.c:279`), vDSO mappé explicitement |
| GDB | `gdb_stub.c/h` (7) | `gdb_stub_owns_debug_regs` : GDB possède les registres de debug, ptrace n'y touche pas |

## 8. Constats annexes

- `SYS_fgetxattr` vaut 16 et `SYS_fremovexattr` 18 dans `src/syscall.h:166,169` ; les numéros arm64 sont 10 et 16 (`Arm64/SyscallsEnum.h:22,28` dans FEX). Effet : l'appel 10 n'est pas implémenté (304 événements dans les journaux EAC) et l'appel 16 (fremovexattr) serait traité comme fgetxattr. Le dispatch utilise ces constantes (`src/syscall.c:2328-2339`).
- La recette `test-host-units` du Makefile ne lie pas `test-thread-vcpu-lifetime` sur la copie de travail (5.2).
- `CLAUDE.md` de HL est en retard sur le code : il décrit `sys_ptrace` comme limité à Rosetta et les futex PI comme non implémentés, alors que `futex.c` les implémente et que `proc_ptrace.c` existe.
- `FUTEX_WAIT_BITSET` traite toujours l'horloge comme temps réel (1.8).
- `fork` ne propage pas `CLONE_CHILD_SETTID` (1.3).

## 9. Inconnues et mesures non faites

- Aucune exécution d'invité : tous les comportements de HL cités viennent du code, de journaux existants ou de tests hôte. Les points INFÉRÉS (échec de `clone` avec un timerfd ouvert, vfork, WIFSIGNALED, TID en cache de glibc) n'ont pas été exécutés.
- Je n'ai pas lu en entier `syscall_io.c`, `syscall_fs.c`, `syscall_time.c`, `guest.c`, `vdso.c`, `proc_emulation.c`, `stack.c`, `shim.S`. Mes comptes de couplage portent sur leurs lignes `grep`, pas sur leur lecture.
- Je n'ai pas mesuré le nombre de threads de Rocket League sous Wine ; la limite de 64 est donc un risque non quantifié.
- Le comportement de FEX aux arrêts de syscall (RCX, R11, RFLAGS, `orig_rax`) n'est pas vérifié ; il relève du coéquipier FEX (témoin (h)).
- Je n'ai pas qualifié `os_sync_wait_on_address` en mode partagé, ni l'échange de port de tâche Mach entre processus coopérants ; ils figurent dans les séries G et H du coéquipier Darwin.
- Les 21 sites d'appel ptrace d'EAC ne sont pas tous associés à une requête : l'ensemble des requêtes nécessaires peut dépasser la liste observée.
