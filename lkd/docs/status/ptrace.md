# Statut du module ptrace (LKD)

Date : 2026-10-02, 20 h 40 (apres le redemarrage de 20 h 21). Jalon 3 : la sequence EAC passe de bout en bout entre deux vrais processus hotes, les requetes, les options, les evenements, la mort du traceur et du tracee sont testees. 4 programmes de test, 852 assertions (MESURE, build prive core + mm + fs + task + sig + ptrace dans `build-ptrace-agent/` (projet `proj/`, `-j 4`, `nice`), chaque test sous `src/fexdarwin/scripts/run-with-timeout.sh`, aucun processus restant apres les executions ; 5 executions de suite sans echec avant le redemarrage, une apres).

## Ce qui existe

`src/ptrace/` (NOTICE : blocs importes de HyperLinux `proc_ptrace.c`, Apache-2.0) :

- `transport.c` : table des liens partagee par toute la famille. Un fichier temporaire non lie est projete `MAP_SHARED` (512 emplacements de 3,4 Kio), cree par le premier processus, herite par `fork`, nomme par `LKD_PTRACE_FD` pour une re-execution hote. Exclusion mutuelle : mutex local (rang 61) plus verrou d'octet `fcntl` sur le fichier (le noyau hote le relache a la mort du detenteur : MESURE, cas `lock_is_released_when_the_holder_dies`). Reveil du tracee et notification du traceur par `famsvc` (opcodes 64 et 65 de la plage ptrace). Aucun autre transport.
- `stop.c` : cote tracee. Un arret est publie dans l'emplacement (code Linux, message, siginfo, vue des registres, images FXSAVE et XSAVE, masque de signaux) par le thread hote du tracee, qui se gare sur son tube de reveil. Les ecritures du traceur (SETREGS, SETFPREGS, SETSIGINFO, SETSIGMASK, signal de reprise, mode, pas) sont des commandes que le tracee applique lui-meme a la reprise, en une seule section critique ; une ecriture apres la reprise echoue (ESRCH). Crochets : `lkd_ptrace_syscall_enter/exit`, `lkd_ptrace_signal_stop`, `lkd_ptrace_event`, `lkd_ptrace_stop`, `lkd_ptrace_trap_stop`.
- `request.c` : l'appel `ptrace(2)` dans l'ordre d'erreurs du noyau.
- `life.c` : `wait4`/`waitid` (`lkd_ptrace_wait_event`, `lkd_ptrace_has_tracees`, `lkd_ptrace_wait_has`), `lkd_ptrace_check_access`, `lkd_ptrace_tracer_of`, mort d'un traceur (detachement, EXITKILL), mort d'un tracee (arret `PTRACE_EVENT_EXIT`, rapport au traceur, processus hote disparu), auto-attachement fork/vfork/clone, crochets de module (init, fork, exec, sortie).
- `regs.c` : `getreg/putreg`, selecteurs, `FLAG_MASK`, bases FS/GS, registres de debogage, MXCSR, en-tete XSAVE.

## Requetes implementees

TRACEME, ATTACH, SEIZE, INTERRUPT, DETACH, KILL, CONT, SYSCALL, SYSEMU, SINGLESTEP et SYSEMU_SINGLESTEP (EIO tant que le moteur n'a pas `set_single_step`), SINGLEBLOCK (EIO), PEEKTEXT/PEEKDATA, POKETEXT/POKEDATA (par `lkd_mm_remote_*`, qui invalide le code traduit de la cible), PEEKUSER/POKEUSER (registres, registres de debogage a l'offset 848, zone vide en zero), GETREGS/SETREGS, GETFPREGS/SETFPREGS, GETREGSET/SETREGSET (NT_PRSTATUS 216, NT_PRFPREG 512, NT_X86_XSTATE de la taille du moteur, NT_386_IOPERM : ENXIO en lecture et EOPNOTSUPP en ecriture), GETSIGINFO/SETSIGINFO, GETEVENTMSG, SETOPTIONS (masque 0x3000ff ; SUSPEND_SECCOMP = EINVAL), GETSIGMASK/SETSIGMASK, GET_SYSCALL_INFO (24, 80 ou 33 octets), ARCH_PRCTL. EIO journalise : LISTEN, PEEKSIGINFO, SECCOMP_GET_*, GET_RSEQ_CONFIGURATION, user dispatch, GET/SET_THREAD_AREA, GET/SETFPXREGS, toute requete inconnue.

Statuts de wait : `(exit_code << 8) | 0x7f` : 0x137f (SIGSTOP), 0x57f, 0x857f (TRACESYSGOOD), 0x4057f (EXEC), 0x1057f (FORK), 0x6057f (EXIT), 0x80057f (PTRACE_EVENT_STOP), 0x80137f (arret de groupe d'un tracee SEIZE). `waitid` rend CLD_TRAPPED pour tout arret ptrace (`wait_task_stopped`, ligne 1345 de kernel/exit.c v6.12).

## Ce qui passe (MESURE)

| Programme | Assertions | Contenu |
| --- | --- | --- |
| `test_ptrace_requests` | 368 | 17 cas, tracee dans un thread : ATTACH et regles d'acces, SEIZE, ESRCH selon le traceur et l'etat, formats de regsets, SETREGSET et putreg, zone USER et registres de debogage, FP et XSTATE, arrets de syscall et GET_SYSCALL_INFO, modification de numero, d'arguments et de resultat, `orig_rax` = -1, SYSEMU, signaux (CONT(0), CONT(sig), signal remplace, SETSIGINFO, signal donne a un arret de syscall remis en file en SI_KERNEL), pas a pas refuse, INTERRUPT, masque, ARCH_PRCTL, requetes non modelisees, PEEK/POKE et invalidation |
| `test_ptrace_eac` | 134 | la sequence EAC entre deux processus hotes (voir plus bas) |
| `test_ptrace_life` | 302 | 16 cas : mort du traceur (detachement, signal supprime, EXITKILL), mort du tracee (statut, TRACEEXIT), KILL, DETACH avec signal, wait4/waitid (WNOWAIT, selection, WNOTHREAD), arret de groupe (non SEIZE : EINVAL pour GETSIGINFO ; SEIZE : evenement 128), exec avec et sans TRACEEXEC, TRACEME et thread createur, fork avec TRACEFORK et auto-attachement (3 processus), options des evenements, matrice d'auto-attachement, selecteurs, tracee tue de l'exterieur (processus hote disparu), traceur tue de l'exterieur (le tracee gare reprend 594 ms apres, MESURE), recopie des ids a l'exec d'un thread non chef |
| `test_ptrace_transport` | 48 | table pleine (EAGAIN a 513), verrou relache a la mort, table partagee entre processus, reveil entre processus, mesures |

Sequence EAC (`test_ptrace_eac`, fork reel, enfant joue le moteur) : TRACEME, tgkill(SIGSTOP) -> 0x137f avec `waitid(WNOWAIT)` puis `wait4`, SETOPTIONS 0x11, PTRACE_SYSCALL x4 avec GETREGSET de 216 octets (entree : rax = -38, orig_rax, rcx = rip, r11 = rflags ; sortie : rax = resultat ; message 1 et 2), CONT, evenement exec 0x4057f (message = ancien tid, siginfo 5/0x405), PTRACE_SYSCALL -> sortie d'execve 0x857f (message 2, orig_rax 59, rax 0, rip = entree, eflags 0x202, fs_base = 0, rcx = 0), PEEK puis POKE de l'INT3 dans une page de code en lecture seule du tracee (le tracee voit 0xcc et `invalidate_code` a ete appele une fois avant sa reprise), CONT, SIGTRAP 0x57f (si_code 128, si_addr 0, rip = int3 + 1, orig_rax = -1, cs 0x33, ss 0x2b, eflags 0x246), SETREGSET (rip - 1), POKE de restauration, sauvegarde NT_PRFPREG, POKE du chemin et de l'adresse de retour 9 sur la pile, SETREGSET (rdi, rsi, rdx, rip, rsp), CONT, SIGSEGV 0xb7f (si_addr 9, rax = resultat), restauration des registres et du FP, CONT(0) (le tracee voit les valeurs restaurees), second SIGSEGV et CONT(11) : le tracee meurt, `wait4` rend 11.

Cout d'un arret et d'une reprise (MESURE, M5 Max, `test_ptrace_transport`, 1500 tours, PTRACE_SYSCALL + attente du stop suivant) :

| Mode | p50 | p90 | p99 |
| --- | --- | --- | --- |
| tracee dans un autre processus hote, `wait4(WNOHANG)` en boucle | 42 us | 63 us | 120 us |
| tracee dans un thread du meme processus (la boucle du tracee dort 300 us) | 367 us | 423 us | 481 us |
| tracee dans un autre processus, `wait4` bloquant (module task actuel) | 11 110 us puis 36 us (deux executions) | 11 203 us puis 80 us | 11 253 us puis 10 757 us |
| tracee dans un thread, `wait4` bloquant (module task actuel) | 11 127 us | 11 529 us | 11 587 us |
| meme, avec le correctif de `wait.c` de la demande ptrace-2 (copie de scratchpad) | 43 us | 53 us | 73 us |
| GETREGSET de 216 octets (lecture du cliche, sans aller-retour) | 2 us | 3 us | 3 us |

Le `wait4` bloquant perd jusqu'a 11 ms par arret (selon que le reveil tombe avant ou pendant la tranche de 10 ms) a cause de la boucle d'attente de task : voir `docs/requests/ptrace-2.md` (correctif de quatre lignes, mesure).

## Ecarts connus

1. `PTRACE_INTERRUPT` ne s'arrete qu'a la prochaine frontiere de syscall, de signal ou d'evenement tant que `lkd_signal_next` n'honore pas `LKD_TF_TRACE_STOP` (demande ptrace-1). Poser ce drapeau sans cela ferait tourner a vide les attentes bloquantes du tracee. Un tracee en code traduit pur ou bloque dans un syscall n'est pas interrompu.
2. Pas de pas a pas : le moteur n'a pas `set_single_step` (EIO, journalise). Aucun arret n'est fabrique.
3. `orig_rax` d'un arret de signal : -1 pour un signal de faute (`si_code > 0` ou `SI_KERNEL`), sinon la valeur du moteur (le dernier syscall). Un signal asynchrone devrait aussi donner -1 (demande ptrace-1, moteur).
4. L'arret de sortie d'execve est emis dans `lkd_ptrace_event(EXEC)`, car `lkd_syscall` n'appelle pas `lkd_ptrace_syscall_exit` pour `LKD_CONTEXT_REPLACED` (demande ptrace-1). Meme limite pour `rt_sigreturn` : pas d'arret de sortie.
5. Yama absent (noyau modele sans Yama, comme `PR_SET_PTRACER` = EINVAL dans task) : seules les regles classiques de `__ptrace_may_access` s'appliquent. Le registre ne garde pas suid et sgid d'une cible : euid et egid les remplacent.
6. Tracee qui n'est pas enfant du traceur : le parent reel apprend la mort par le module task tout de suite, pas apres la collecte par le traceur (pas de reparentage ptrace). Si le traceur est le parent, ptrace ne rapporte pas la mort (task le fait).
7. Un tracee dont le processus hote disparait sans passer par la sortie est rapporte comme tue par SIGKILL (statut reel inconnu).
8. `PTRACE_LISTEN`, `PEEKSIGINFO`, seccomp, rseq, user dispatch, `GET/SET_THREAD_AREA` : EIO journalise. `SETREGSET` lit tout le tampon utilisateur avant d'appliquer (Linux peut appliquer un prefixe puis fautter).
9. `wait4` bloquant : latence de 11 ms (ptrace-2). `wait4(pid)` d'un pid qui n'est pas un enfant attend au lieu de rendre ECHILD tant que le traceur a un tracee (`lkd_ptrace_wait_has` existe, task doit l'appeler). `waitid` : `si_status` d'un arret ptrace est tronque a 8 bits par task.
10. Exec depuis un thread non chef : le tid du tracee et celui du traceur sont recopies dans la table par `lkd_ptrace_exec_commit` (teste en simulant le changement de tid, pas avec un vrai de_thread).

## Demandes ouvertes

`docs/requests/ptrace-1.md` (sig : TRACE_STOP ; core : arret de sortie pour contexte remplace ; moteur : orig_rax, pas a pas), `docs/requests/ptrace-2.md` (task : reveil de wait4, selecteur, si_status).

## Suite possible

Tests de bout en bout avec le vrai moteur FEX (le chef lance les temoins x86 : `probe-x86_64`, `target-x86_64`, `target-flags-x86_64`), arret asynchrone par `kick` dans du code traduit, pas a pas, reparentage ptrace, `PTRACE_LISTEN`.

## Incident du 2026-10-02 20 h 05

Quatre rapports de plantage `test_ptrace_life-2026-10-02-2005*.ips` : tous dans `traceme_rules`, `test_ptrace_life.c:365`, lecture de `x->proc` apres `lkd_task_release(x)` dans une impression de debogage temporaire du test (utilisation apres liberation dans le test, pas dans le module). Corrige a 20 h 10 : le test garde le tid avant la liberation. MESURE apres le redemarrage : `test_ptrace_life` 302 assertions, 0 echec.

## Campagne de sondes EAC x86-64 (2026-10-02, 20 h 44 à 20 h 58, mainteneur d'intégration)

Lanceur : `witnesses/eac-probes/run-probes.py`, hôte `build-integration/host/fexdarwin-run` lié à `build-integration/lib/liblkd.a`.

MESURE :

| Exécution | Sondes | Contrôles | Échecs |
| --- | --- | --- | --- |
| avant (20261002T204415, identique à celle du chef 20261002T204027) | 39 | 1824 | 232 |
| après les trois correctifs LKD (20261002T205456) | 39 | 3257 | 7 |
| après correction du lanceur (20261002T205719) | 40 | 4376 | 38 |

Le nombre de contrôles augmente parce que les sondes qui s'arrêtaient au premier échec vont maintenant jusqu'au bout.

Correctifs LKD, chacun avec un test qui échoue avant et passe après :

1. POKE forcé dans du code en lecture seule dont la page hôte de 16 Kio contient aussi des données (cold_INT3_POKEDATA : EIO). Cause : `mm_local_rw` (`src/mm/remote.c`) déduisait la protection hôte de l'union des VMA (RW), alors que la page surveillée par le JIT était encore en lecture seule après le retrait de la surveillance (`mm_unwatch_for_write` laisse l'ouverture à l'écrivain FOLL_FORCE). Correctif : quand la surveillance vient d'être retirée, la page est ouverte explicitement. Test : `test_mm_watch` cas `host_page_shared_with_data`.
2. Exec d'un tracé issu d'un fork (cluster GETEVENTMSG et GETSIGINFO après l'arrêt d'exec, 14 sondes). Cause : `lkd_ptrace_exec_commit` (`src/ptrace/life.c`) suivait le changement de tid de `de_thread` à partir de `tid_seen`, qui vaut encore le tid du parent dans l'enfant d'un fork (le crochet de fork de ptrace passe avant le changement d'identité). Le `tracer_tid` du lien devenait le tid du tracé et toute requête suivante rendait ESRCH. Correctif : ne réécrire que les liens du même groupe de threads. Test : `test_ptrace_life` cas `exec_of_a_forked_tracee_keeps_its_tracer`.
3. Code chaud après POKE (warm_INT3, « warm repeat execution after write ») : invalidation appelée depuis le fil de service mm, sans thread invité, ignorée par le moteur. Corrigé côté moteur par le chef (InvalidateCode sans thread). Le mécanisme de rejeu ajouté un temps dans LKD a été retiré.

Lanceur (`run-probes.py`) : la ligne de commande de la sonde reprend maintenant celle de son `run-hl.py` (cible, mode ou phase `pre`/`post` seulement s'ils y figurent ; avant, `peek`, `poke`, `poke-commit`, `proc-remote-maps*` sortaient avec 87 ou 89). La sonde tourne dans une copie privée et inscriptible de son répertoire, parce que les sondes créent des fichiers témoins dans leur répertoire courant (`/`). Avant : EROFS sur `owned marker O_EXCL creation`.

Échecs restants (MESURE, 20261002T205719) :

- `ptrace-peek-control`, `ptrace-poke-control` (9 chacun), `ptrace-poke-commit` pre et post (7 chacun) : l'enfant mappe en MAP_FIXED à 0x7ffff7ff0000 et 0x7ffffdffb000. Cette adresse dépasse la fenêtre invitée de 2^46 octets (TASK_SIZE de LKD, `docs/requests/mm-2.md` point 3). L'enfant sort avec 70 avant l'arrêt. LU : une fenêtre de 2^47 ne tient pas dans l'espace d'adresses hôte avec la base de 8 Tio. Limite de conception, pas un défaut de ptrace.
- `ptrace-setregset-deliver` (2) et `ptrace-setregset-two-trap` (4) : `CONT(pid, SIGTRAP)` à l'arrêt de délivrance d'un SIGTRAP, sans gestionnaire. LKD délivre SIGTRAP et le tracé meurt (statut 5). INFERE : c'est le comportement de Linux (`get_signal`, action par défaut coredump, RLIMIT_CORE 0). L'attendu 0 du probe n'a pas d'oracle natif dans son répertoire, et l'exécution HyperLinux enregistrée échouait aussi. À confirmer sur un vrai Linux avant tout changement.

## Mise à jour d'intégration (2026-10-02, 21 h 45)

Écarts 1 (INTERRUPT d'un tracé bloqué), 4 (arrêt de sortie pour contexte remplacé, rt_sigreturn compris) et 9 (wait4 : latence de 11 ms, ECHILD, si_status) corrigés, avec les tests `interrupt_reaches_a_blocked_tracee`, `sigreturn_has_an_exit_stop`, `blocking_wait4_wakes_at_once` et `wait_selectors_see_tracees`. Voir les demandes ptrace-1 et ptrace-2 (sections « Traitée »).
