# Statut du module core (LKD)

Date : 2026-10-02. Jalons 1 à 4 atteints : squelette, contrats, tests, documentation. Le module core est livré aux sept autres agents.

## Ce qui existe (MESURE : `./check.sh`, 4 programmes, 4 PASS)

| Programme | Assertions | Cas | Contenu |
| --- | --- | --- | --- |
| `test_core_dispatch` | 155 | 16 | enregistrement et conflits, six arguments dans l'ordre x86-64, retours négatifs, ENOSYS compté (numéro libre, hors table, bit x32), dispositions, `ENOIOCTLCMD`, redémarrage (`ERESTARTSYS`, `ERESTARTNOHAND`, `RESTARTBLOCK`, `restart_syscall`), numéros de la table v6.12, trace (sans contenu mémoire, une ligne ENOSYS par numéro, niveau 2), attentes bloquantes (délai, interruption, réveil perdu), `lkd_fork_host` avec un vrai `fork` |
| `test_core_usermem` | 113 | 11 | copies exactes sur tous les alignements, pointeurs invalides, page de garde, `strn`, base invitée non nulle (0x7ffe0000), gestionnaire du moteur installé après (`lkd_fault_try_recover`), chaînage vers le gestionnaire précédent, quatre threads qui fautent en parallèle, crochet de contrôle de plage |
| `test_core_xlate` | 358 | 13 | errno, drapeaux `open`, `mmap`, signaux, `fcntl`, sockets, `MSG_*`, numéros de périphérique, `stat` octet par octet, `statx`, `statfs`, `d_type`, appels hôte réels avec les valeurs traduites |
| `test_core_contract` | 89 | 5 | ce que les en-têtes promettent aux modules : identifiants uniques à travers un `fork`, `tid == pid` du thread principal, threads d'un même processus, recherche dans le registre, table de descripteurs POSIX (plus petit libre, `dup2`, `FD_CLOEXEC`, partage CLONE_FILES) |

Autres contrôles lancés par `check.sh` : `tools/owners_check.py` (291 syscalls attribués, 84 sans propriétaire, 0 conflit) et `tools/verify_abi.py` (747 constantes `LX_*` comparées aux `#define` du uapi Linux v6.12 téléchargé dans `tools/data/uapi-v6.12`, 0 écart, 158 non vérifiables car énumérations ou macros à paramètres). Les tailles et décalages des structures sont des `_Static_assert` dans `abi_x64.h` (compilation C et C++ vérifiées une fois pour les en-têtes publics avec `clang++ -std=c++20`).

Stabilité : 15 exécutions consécutives de l'ensemble sans échec.

## Livrables

- `CMakeLists.txt`, `build.sh`, `check.sh`, `tools/run_tests.sh`, `tools/runtest.sh` (limite de temps par perl).
- `include/lkd/lkd.h` (API moteur et noyau), `abi_x64.h`, `xlate.h`, `syscall_nr.h` (généré par `tools/gen_syscall_table.py`, 375 syscalls, sha256 980ce311...).
- `src/include/` : `lkd_internal.h`, `task.h`, `fd.h`, `mm.h`, `vfs.h`, `sig.h`, `futex.h`, `ptrace.h`, `net.h`.
- `src/core/` : `api.c`, `dispatch.c`, `trace.c`, `usermem.c`, `block.c`, `hooks.c`, `weak_modules.c`, `xlate.c`, `stubs_{task,mm,fs,sig,ptrace,net}.c`.
- `tests/ltest.h`, `tests/support/fake_engine.{h,c}`.
- `OWNERS.md`, `docs/CONVENTIONS.md`, `README.md`, `NOTICE`.

## Choix de conception à connaître (tous documentés dans les en-têtes)

1. Récupération de faute des copies invitées : plage de PC (routine assembleur `lkd_uc_copy`, le gestionnaire met x0 = -1 et PC = LR), pas de point de reprise par thread. Pas de coût par appel, pas d'état par thread. arm64 seulement (arm64e non géré). Le moteur qui installe son gestionnaire après `lkd_init` appelle `lkd_fault_try_recover` en premier.
2. `lkd_signal_next` appelle `ops->frame_build` lui-même (le moteur ne reçoit pas une description à construire). `rt_sigreturn` passe par `ops->frame_restore` dans le module sig. `lkd_signal_returned` existe pour un moteur qui restaure le cadre seul.
3. Redémarrage des syscalls : décidé dans `lkd_syscall` par `lkd_sig_restart_decision` (module sig). Le moteur reçoit `LKD_RESTART` avec dans `*rax` le numéro à remettre (le numéro d'origine ou `restart_syscall`), recule RIP de 2.
4. Attentes bloquantes : tube de réveil par tâche plus test d'interruption avant de dormir (`lkd_block_wait_fd`), pas de signal hôte pour les attentes que LKD maîtrise. Le futex (sig) suit un contrat séparé (`futex.h`).
5. fork : `lkd_fork_host` orchestre les crochets dans l'ordre des rangs de verrous. exec : `lkd_exec_prepare` (task), `lkd_exec_commit` (core, crochets), `ops->exec_image` (moteur), `lkd_exec_finish`.
6. Symboles faibles : les crochets de `weak_modules.c` et les remplaçants de `stubs_*.c` ne sont remplacés que si la bibliothèque est liée avec `-force_load` (fait par `CMakeLists.txt`, avec `LINK_DEPENDS` pour que les tests se relient). MESURE : une définition forte dans `src/mm/` et un test dans `tests/test_mm_*.c` ont été ajoutés puis retirés pour vérifier la prise en compte par `file(GLOB)`, le remplacement de `lkd_mm_check_range` et l'enregistrement de `brk` avec propriétaire `mm`.
7. Les remplaçants faibles sont réels là où les autres modules en ont besoin pour leurs tests : registre local de tâches avec compteur d'identifiants en mémoire partagée (hérité par `fork`), table de descripteurs, objets fd. Le vfs, le mm, les signaux et ptrace y sont des coquilles (-ENOSYS, rien en attente, un accès ptrace refusé hors de soi-même).

## Lacunes connues

1. Pas de service d'appel inter-processus commun. Quatre besoins l'exigent : copie mémoire d'un autre processus (mm), boîte aux lettres de signaux (sig), transport ptrace, listage de `/proc/<pid>/fd` d'un autre processus (fs/procfs). Chaque module choisit son transport ; si les quatre divergent, un service commun dans core est à envisager (INFERE : un thread de service par processus et un socket par pid, avec codes d'opération enregistrés par module).
2. Le listage des descripteurs d'un autre processus n'a aucun support (le registre partagé ne contient pas la table). procfs devra répondre honnêtement (ENOENT ou EACCES) tant qu'aucun transport n'existe.
3. Le pas à pas (`ops->set_single_step`) est facultatif : NULL ou `-ENOSYS` rend `PTRACE_SINGLESTEP` honnêtement indisponible.
4. Taille de l'état XSAVE : fixée par le moteur (`LKD_FP_XSAVE`, taille rendue dans `*len`).
5. `lkd_mm_check_range` n'est validé qu'avec le remplaçant permissif ; la version stricte viendra avec le module mm. Les tests qui utilisent de la mémoire hôte brute comme mémoire invitée doivent appeler `lkd_usermem_set_check_fn` (déclaré dans `lkd_internal.h`).
6. Aucun module n'existe encore : les tests de contrat de `test_core_contract` tournent contre les remplaçants et devront passer contre les modules.
7. Le champ `exe` et `cmdline` du registre sont de taille fixe (512 et 4096 octets, tronqués au-delà).

## Suite

Les sept agents de module démarrent. Le noyau commun ne change plus sans demande (`docs/requests/<module>-<n>.md`). A traiter à la réception des demandes : ajustements d'en-têtes, extensions du moteur factice.

## Demandes traitées par le chef de projet (2026-10-02 16:20)

- fs-1 point 1, mm-1, task-1 (en partie) : `lkd_config_get` est déclaré dans `src/include/lkd_internal.h` ; les redéclarations locales des modules restent valides (même prototype) et peuvent être retirées.
- fs-1 point 2 : `src/include/lkd_fs.h` renvoie à `src/fs/lkd_fs.h`.
- En attente : `test_core_dispatch` enregistre ses propres `lkd_sys_read/write/open/ioctl` alors que tous les modules sont liés (`-force_load`) ; il faut le réécrire avec des gestionnaires `static` et des numéros non pris, ou le lier contre core seul. Les autres points de task-1 (notifications d'arrêt, `LKD_TF_IN_BLOCK`, ordre des crochets de fork) et les demandes sig-* restent à traiter.

## Intégration (2026-10-02, 19 h 40 à 21 h 45, mainteneur d'intégration)

Construction : `EXP/build-integration` (cmake Ninja, Debug, `nice -n 10`, `-j 4`). Lanceur : `build-integration/run_all.sh [motif]` (chaque programme sous `src/fexdarwin/scripts/run-with-timeout.sh 120`, contrôle `pgrep` après chacun). Hôte moteur lié à cette bibliothèque : `build-integration/host/fexdarwin-run`.

### Totaux (MESURE)

| Moment | Programmes | PASS | Assertions | Cas |
| --- | --- | --- | --- | --- |
| avant | 41 | 40 (échec : `test_net_host`) | 5960 | |
| après | 41 | 41, deux exécutions complètes de suite | 6108 | 403 |

`tools/owners_check.py` : 291 attribués, 84 sans propriétaire, 0 problème. `tools/verify_abi.py` : 749 vérifiés, 0 écart. Tous les gestionnaires de syscall sont `static` (la macro `LKD_SYSCALL` le garantit). Aucun processus de test restant.

Sondes EAC x86-64 (`witnesses/eac-probes/run-probes.py`) : 1824 contrôles et 232 échecs avant, 4376 contrôles et 38 échecs après (20261002T214053). Détail dans `docs/status/ptrace.md`.

### Changements par module (chacun avec un test qui échoue avant et passe après, sauf mention)

- core : `lkd_block_wait_wake` (ptrace-2) ; `LKD_TF_IN_BLOCK` pendant `lkd_block_poll` (task-1) ; arrêt de sortie de syscall pour `LKD_CONTEXT_REPLACED`, drapeau relu à la sortie (ptrace-1) ; `lkd_user_cmpxchg32` récupérable (sig-1) ; crochet `lkd_sig_wake_hook` dans `lkd_task_wake` ; `LKD_BUG` avec fichier et ligne ; `LKD_SYSCALL` statique ; `lkd_fork_reinit_mutex/rwlock` ; déclarations ajoutées à `ptrace.h`, `sig.h`, `task.h`, `mm.h`, `net.h`, `lkd_internal.h` avec leurs remplaçants faibles ; `src/include/lkd_procfs.h` ; contrat de montage virtuel dans `vfs.h` ; commentaires de `fd.h` et de `lkd_config.guest_window_base` ; CONVENTIONS §3.4 (verrous du moteur avant ceux de LKD, aucun rappel du moteur sous verrou LKD) et §6.2 (reconstruction des verrous dans l'enfant d'un fork).
- task : `wait4` se réveille au premier `lkd_task_wake` (11 ms par arrêt ptrace avant) ; `lkd_task_wake_waiters` atteint un autre processus hôte par famsvc (opcode 96) ; ECHILD selon le sélecteur (`lkd_ptrace_wait_has`) ; `si_status` complet pour un arrêt ptrace ; `lkd_guest_clock_ns`, `lkd_pid_peek`.
- sig : `LKD_TF_TRACE_STOP` honoré (PTRACE_INTERRUPT d'un tracé bloqué) ; arrêt et reprise de contrôle de travaux rapportés à `wait4` ; CAS futex récupérable ; échéances absolues sur l'horloge invitée ; correction d'un réveil futex perdu (raccourci « pas d'attente » de `do_wake` retiré, `mutex_contention` bloquait environ une fois sur 20, 0 sur 60 après).
- ptrace : `lkd_ptrace_syscall_exit_replaced` (exec et rt_sigreturn), contournement de l'exec retiré ; `lkd_ptrace_exec_commit` ne réécrit que les liens du même groupe de threads (cluster de sondes EAC : ESRCH après l'arrêt d'exec) ; attente bornée du lien d'un enfant auto-attaché avant l'arrêt PTRACE_EVENT_FORK.
- mm : POKE forcé dans une page de code surveillée partagée avec des données ; `/dev/zero` mappable (procfs-2) ; `hiwater_vm` et `hiwater_rss` ; `mprotect(PROT_WRITE)` d'un memfd scellé refusé (fs-mm) ; reconstruction des verrous dans l'enfant d'un fork (témoin `w18_forkjit` : blocage avant, 3 passages sur 3 après) ; invalidations collectées hors du verrou mmap.
- 2026-10-10 : `lkd_fd_ops.export_fds` et les enregistrements (`fd.h`, `src/core/fdrec.c`) : une description sans descripteur hôte part par SCM_RIGHTS comme un objet shm POSIX de 16 Kio (signature `LKDFREC1`, type, drapeaux d'état, données du module) ; chaque module enregistre l'importeur de son type (`lkd_fdrec_register`). Voir `net.md`.
- fs : extrémité d'écriture d'un tube sans lecteur annoncée OUT|ERR (net-1) ; descripteur de répertoire ou de fichier reçu par SCM_RIGHTS retrouve chemin invité et montage (wine-3) ; déclarations pour procfs dans `lkd_fs.h`.
- net : `IP_RECVERR`/`IPV6_RECVERR` et `MSG_ERRQUEUE` (wine-4, DNS de glibc dans l'invité) ; `lkd_net_inotify_limits`.
- procfs : VmPeak et VmHWM réels ; limites inotify de net ; ligne `processes` de `/proc/stat`.
- tests : `test_net_host` mis à jour (mm accepte maintenant un MAP_SHARED de 4 Kio à l'offset 4096) ; les tests d'exec de ptrace simulent la fin de `lkd_syscall`.

### Lacunes connues après l'intégration

1. Ordre des crochets de fork dans l'enfant inchangé (task-1 point 3) : ptrace lit le lien du parent avec l'identité du parent.
2. `hiwater_rss` est une borne inférieure (relevé aux lectures et aux grands démappages).
3. File d'erreurs IP_RECVERR d'une seule entrée, sans détail ICMP.
4. Sondes `peek/poke/poke-commit` : MAP_FIXED à 0x7fff... hors de la fenêtre invitée de 2^46 octets (mm-2 point 3).
5. Tube plein sans lecteur annoncé OUT en plus de ERR.
6. mm-3 (table SysV pour une ré-exécution hôte) et sig-2 point 4 : sans objet tant que l'exec reste dans le processus hôte.
