# Statut du module sig (LKD)

Date : 2026-10-02. Jalons 1 à 3 atteints : signaux, futex, signalfd, service IPC de famille (famsvc), tests mono et multi-processus. `./check.sh` : 26 programmes, 26 PASS (MESURE, dont les tests mm, fs, task et core contre le module sig réel).

## Fichiers

| Fichier | Contenu |
| --- | --- |
| `src/sig/sig_priv.h` | structures privées, ordre des verrous du module |
| `src/sig/state.c` | état paresseux par processus (`lkd_sigproc`) et par thread (`sig_task` sur `task->pending.queue`), objets d'actions, réveils hors verrou, crochets init/fork/exec/sortie, export et import pour la ré-exécution hôte |
| `src/sig/queue.c` | files de siginfo, génération (prepare_signal, legacy_queue, RLIMIT_SIGPENDING, complete_signal), retrait (next_signal, collect_signal), retarget au changement de masque, force_sig |
| `src/sig/deliver.c` | `lkd_signal_pending`, `lkd_sig_interrupted`, `lkd_signal_next`, `lkd_sig_restart_decision`, `lkd_fault`, `lkd_signal_returned`, masques temporaires, pile alternative, arrêt de groupe, `lkd_sig_status`, `lkd_sig_task_stopped` |
| `src/sig/send.c` | kill, tkill, tgkill, kill(0), kill(-pgrp), kill(-1), permissions, envoi inter-processus, SIGCHLD au parent, `lkd_sig_kill_all_threads` |
| `src/sig/syscalls.c` | appels système de signaux |
| `src/sig/signalfd.c` | objets signalfd (fd.h), lisibilité par tube de notification |
| `src/sig/futex.c` | futex, futex_waitv, futex2, PI, listes robustes, table privée et table partagée |
| `src/sig/famsvc.c`, `src/include/famsvc.h` | service IPC de famille (décision du chef), annoncé dans `docs/requests/sig-famsvc.md` |
| `src/sig/NOTICE` | aucun code importé de HyperLinux |

## Appels système enregistrés (21)

rt_sigaction, rt_sigprocmask, rt_sigreturn, rt_sigpending, rt_sigtimedwait, rt_sigqueueinfo, rt_sigsuspend, rt_tgsigqueueinfo, sigaltstack, kill, tkill, tgkill, pause, signalfd, signalfd4, pidfd_send_signal, futex, futex_waitv, futex_wake, futex_wait, futex_requeue. Tous ceux de `OWNERS.md` pour sig.

## Tests (MESURE, `tools/runtest.sh`)

| Programme | Assertions | Cas | Contenu |
| --- | --- | --- | --- |
| `test_sig_basic` | 380 | 25 | rt_sigaction (drapeaux UAPI, KILL/STOP), masques, livraison et cadre (SI_USER, masques old/new, NODEFER, RESETHAND), signaux ignorés, coalescence et file temps réel FIFO, sigqueueinfo EPERM, arguments de kill/tkill/tgkill, permissions (uid, suid, SIGCONT même session, CAP_KILL), choix du thread, règles de redémarrage, fautes (si_code, si_addr, SI_KERNEL, force_sig sur action bloquée ou ignorée, statut core), signal fatal de groupe, sigaltstack (ENOMEM, EINVAL, EPERM, SS_ONSTACK, SA_ONSTACK, SS_AUTODISARM, restauration), échec du cadre puis SIGSEGV, rt_sigtimedwait bloquant, rt_sigsuspend, pause, arrêt et reprise de groupe, SIGCHLD (NOCLDSTOP, NOCLDWAIT, SIG_IGN), kill de groupe, signalfd, exec, export/import de ré-exécution |
| `test_sig_more` | 83 | 8 | SIGKILL pendant un arrêt, tgkill(SIGKILL) tue le groupe, SIGPIPE, limite RLIMIT_SIGPENDING (EAGAIN, perte silencieuse), clone des actions (CLONE_SIGHAND, CLONE_CLEAR_SIGHAND), retarget au blocage et à la sortie d'un thread, état de l'enfant d'un vrai fork, 4 émetteurs concurrents x 500 signaux temps réel (aucun perdu, FIFO par émetteur), chemin de w15 (`lkd_task_create_main`, `rt_sigaction`, `clone` réel du module task, `tgkill`, livraison sur le nouveau thread) |
| `test_sig_futex` | 147 | 13 | erreurs de WAIT, WAIT/WAKE avec 8 threads (nr = 0 réveille un), bitset, délais absolus MONOTONIC et REALTIME, REQUEUE et CMP_REQUEUE, WAKE_OP (5 opérations, OPARG_SHIFT, ENOSYS), protocole PI (EDEADLK, EWOULDBLOCK, EPERM, passage de propriété, ESRCH, délai, OWNER_DIED), interruption par signal (ERESTARTSYS, ERESTART_RESTARTBLOCK et restart_syscall jusqu'à l'échéance d'origine), futex_waitv, appels futex2, liste robuste (OWNER_DIED, list_op_pending, cycle borné), mutex futex à 16 threads, futex partagé entre deux processus (fork), PI partagé |
| `test_sig_famsvc` | 93 | 9 | PING, charges de 3 et 6 Mio, passage de descripteur, identité de l'appelant, opcode inconnu ENOSYS, appel imbriqué refusé, ESRCH, délai puis reprise, service de l'enfant d'un fork, signaux enfant vers parent (kill, tgkill) et parent vers enfant, EPERM entre processus, adoption de la socket après une ré-exécution hôte réelle (`execl` du binaire de test), arrêt et suppression du répertoire |

Total : 703 assertions, 55 cas, dont des cas multi-thread (jusqu'à 16 threads) et multi-processus (fork réel par `lkd_fork_host`, exec réel).

## Mesures (MESURE, M5 Max, macOS 27)

- famsvc, PING de 4 octets, 2000 appels : vers un autre processus p50 8,4 à 10,4 us, p99 22 à 32 us ; vers soi p50 8,4 à 12,7 us. Première version avec `poll` : p50 19 us ; passage à kqueue côté service et sockets bloquantes avec `SO_RCVTIMEO` côté client.
- Attente futex sur clé partagée interrompue par un signal envoyé à 20 ms : retour à 22,5 à 25 ms (la sonnerie directe évite la tranche de 50 ms).
- Mutex futex privé, 16 threads x 3000 sections avec `sched_yield` dedans : 7,4 ms, 1579 appels FUTEX_WAIT.

## Témoins du moteur (MESURE, 19:25, détail dans `docs/requests/sig-4.md`)

Reproduction : liblkd.a construite dans un répertoire de travail, hôte `fexdarwin-run` construit contre elle (recette du chef), `FEXDARWIN_LKD=1`, `--trace-signals`, `run-with-timeout.sh`.

1. Cause de la mort de w15_async (statut 138, `lkd_signal_next = -1`). Une trace temporaire (retirée) dans `rt_sigaction` et dans `complete_signal` montre que le partage CLONE_SIGHAND est correct : le tid 2 et le tid 3 voient le même objet d'actions. L'action lue dans la mémoire invitée est `handler = 0x0, flags = 0x4000004` : le gestionnaire vaut 0 AVANT d'entrer dans LKD. Les binaires de `witnesses/elf-x86_64/bin` (datés 15:17, inchangés à 19:23) sont des PIE statiques (`ET_DYN`, `PT_DYNAMIC`, pas de `PT_INTERP`) dont les relocations `R_X86_64_RELATIVE` ne sont jamais appliquées (ni par le chargeur du moteur, ni par `rt.h`, ni par un `_dlstart`, absent avec `-nostdlib`). Relocations non appliquées : w15_async 2 (0x99a0 gestionnaire, 0x99b0 restaurateur), w02_int 4, w11_badjump 2, w14_overflow 3, w07_fault 1, w10_threads 0. Les structures `ksigaction` initialisées avec `(u64)handler` sont lues avec le champ à 0, donc SIG_DFL, donc SIGUSR1 tue le processus. Remède côté témoins ou chargeur : appliquer les relocations RELATIVE (base + addend) au chargement, ou lier en ET_EXEC. Le test `clone_thread_shares_handler` (`tests/test_sig_more.c`) reproduit le chemin de w15 avec le module task réel et passe.
2. Avec des copies relogées pour la base 0x2aaaaaaaa000 (octets 8 de chaque RELATIVE écrits par un script Python, copies hors de `witnesses/`) : w02_int PASS 9524/9524, w14_overflow PASS 7/7 (débordement de pile rattrapé sur pile alternative, deux fois), w10_threads PASS 13/13, w07_fault passe jusqu'au cas « non-exec fetch ».
3. w11_badjump et la fin de w07_fault : FEXCore répète `[FEXCore E] NoExec instruction in entry block: 9` (ou 3FFFF7FF0000) sans que `lkd_fault` soit appelé (aucune ligne de signal dans la trace). Côté moteur : `GuestSignals.cpp:112` n'est jamais atteint pour un saut vers une adresse non exécutable. Le journal atteint des centaines de Mio par minute : toujours borner la sortie.
4. w15_async relogué : 15/16 ou 14/16 selon l'exécution. Livraison correcte (`lkd_signal_next = 1`, gestionnaire sur le thread visé, SI_TKILL et SI_USER justes, masque restauré), mais « spinner registers intact » et parfois « made progress » échouent : registres ou xmm du thread interrompu en code traduit mal sauvés ou restaurés par `FrameBuild` et `FrameRestore` (INFERE, côté moteur : LKD ne touche à aucun registre).
5. Hors sig : `test_task_bigwin` se bloque dans le processus enfant de `lkd_fork_host` (MESURE par `sample` : `lkd_mm_fork_child` -> `mm_host_refresh` (mm.c:305) -> `mm_watch_lock` (watch.c:50) -> `__psynch_mutexwait`). Le verrou récursif de `watch.c` est pris par `lkd_mm_fork_prepare` dans le parent ; l'enfant le réinitialise dans `mm_watch_fork_child`, mais `mm_host_refresh` le prend avant cet appel dans `lkd_mm_fork_child`. Défaut du module mm en cours de modification, signalé au chef. Les tests sig ne sont pas touchés (aucun n'utilise la fenêtre de production).

## Choix de conception (LU dans le code)

1. Un seul verrou par processus (`lkd_sigproc.lock`, rang 40) pour toutes les files ; actions modifiées sous ce verrou et celui de l'objet d'actions. Les réveils (tube, sonnerie futex, `ops->kick`) sont faits verrous relâchés.
2. Génération selon Linux : un signal ignoré est jeté à la génération sauf s'il est bloqué (ou attendu par rt_sigtimedwait, `real_blocked`) ou si la tâche est tracée ; un signal standard déjà en attente n'est pas remis en file ; un signal fatal par défaut (hors core) marque tout le groupe `LKD_TF_FATAL` immédiatement ; le premier thread au point sûr appelle `lkd_task_do_exit_group` une seule fois.
3. Ordre de retrait : signaux synchrones (SEGV, BUS, ILL, TRAP, FPE, SYS) d'abord, puis plus petit numéro, file du thread avant celle du processus.
4. Statut « core » : bit 0x80 si RLIMIT_CORE souple non nul et processus dumpable. Aucun fichier core n'est écrit.
5. Arrêt de groupe : le premier thread qui retire un signal d'arrêt marque tous les threads ; chacun se gare à son point sûr ; le dernier rapporte CLD_STOPPED au parent ; SIGCONT relâche, rapporte CLD_CONTINUED (ou CLD_STOPPED si l'arrêt n'était pas fini, comme Linux) au prochain passage dans `lkd_signal_next`. Une tâche tracée passe par `lkd_ptrace_stop(LKD_STOP_GROUP)`.
6. Futex : table privée par processus (verrous `os_unfair_lock`) et table partagée dans le fichier `futex.shm` du répertoire de famille (verrous par mot contenant le pid hôte, repris si le détenteur est mort). Les attentes privées dorment sur le tube de réveil de la tâche ; une attente avec une clé partagée dort sur `os_sync_wait_on_address(SHARED)` par tranches de 50 ms, sonnée par les signaux. Le REQUEUE entre table privée et table partagée réveille au lieu de déplacer (réveil fallacieux permis).
7. famsvc : voir `docs/requests/sig-famsvc.md`.

## Lacunes connues

1. WAIT_REQUEUE_PI et CMP_REQUEUE_PI : `-ENOSYS` (glibc 2.25 et plus ne les utilise plus pour les conditions). Pas d'héritage de priorité. La mort d'un propriétaire PI sans liste robuste n'est pas détectée (Linux le fait par l'état PI du noyau).
2. Opérations atomiques sur les mots invités (WAKE_OP, PI, robuste) : contrôle de plage puis atomique hôte ; un `munmap` concurrent ferait planter (demande sig-1.md, point 1).
3. wait4 ne voit pas encore les arrêts et reprises de contrôle de travaux d'un enfant (demande sig-2.md, point 1) ; le SIGCHLD et le réveil des attentes sont envoyés.
4. Groupe de processus orphelin non détecté : SIGTSTP, SIGTTIN, SIGTTOU arrêtent toujours.
5. `si_utime`/`si_stime` de SIGCHLD à 0. RLIMIT_SIGPENDING compté par processus (Linux : par utilisateur), plafonné à 16384.
6. signalfd : `fstat` non fourni (NULL, l'erreur par défaut du module fs) ; état non partagé à travers un fork (fd.h).
7. Les entrées de la table futex partagée d'un processus mort brutalement restent (réveils comptés à tort) ; les directoires de famille d'exécutions plantées sont nettoyés après une minute.
8. Interruption du code JIT : seulement au prochain syscall tant que le moteur n'a pas de page d'interruption (limite moteur, sig-3.md).
9. Les délais REALTIME absolus sont convertis une fois : un changement d'heure ultérieur ne les déplace pas.

## Demandes ouvertes

`docs/requests/sig-famsvc.md` (annonce du service, opcodes de mm, procfs, ptrace), `sig-1.md` (core : CAS invité avec récupération de faute, crochet de réveil, déclarations dans sig.h, macro LKD_BUG), `sig-2.md` (task : état d'arrêt pour wait4, pidfd, horloges, transport de l'état des signaux à la ré-exécution), `sig-3.md` (contrat de kick avec le moteur).
