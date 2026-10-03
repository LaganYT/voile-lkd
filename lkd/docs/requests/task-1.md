# Demande task-1 au noyau commun (core)

Date : 2026-10-02. Auteur : module task.

1. `lkd_config_get()` est déclarée dans `src/core/core_priv.h`, réservé au core. Le module task en a besoin (uid, gid, first_pid, hostname). Il la déclare localement dans `src/task/task_priv.h:22`. Demande : la déclarer dans `lkd_internal.h`.

2. Déclarer dans `src/include/task.h` les fonctions que task fournit déjà (définitions dans `src/task/tasks.c` et `src/task/sys.c`) :
   - `void lkd_task_notify_stop(lkd_task *t, int signo);` et `void lkd_task_notify_continue(lkd_task *t);` : le module sig les appelle quand un processus entre en arrêt de contrôle de tâche ou en repart. Elles publient l'état `T` et l'événement que `wait4(WUNTRACED)` et `waitid(WCONTINUED)` rapportent une fois (MESURE : `tests/test_task_proc.c` cas `stop_and_continue_reports`).
   - `int lkd_task_uname(struct lx_utsname *out);` : la même chaîne `release` pour `uname(2)` et pour procfs (`/proc/sys/kernel/osrelease`, `/proc/version`).
   - `int lkd_task_shares_host(const lkd_task *t);` : vrai pour un processus invité qui vit dans le processus hôte d'un autre (enfant de vfork ou de posix_spawn avant son exec). Les crochets `proc_exit` ne doivent alors arrêter aucun service global au processus hôte.

3. Ordre des crochets de fork dans l'enfant (`src/core/hooks.c`, `lkd_fork_finish`) : l'ordre inverse donne net, ptrace, task, sig, procfs, fs, mm. ptrace et net voient donc encore l'identité du parent dans l'enfant. Demande : appeler `lkd_task_fork_child` en premier dans l'enfant, puis les autres dans l'ordre inverse actuel. En attendant, task appelle `lkd_ptrace_task_init(t, t, flags)` après le basculement d'identité (`src/task/clone.c`, `clone_fork`), avec la copie du lien ptrace du parent encore dans `t->ptrace`.

4. `src/core/block.c` : poser `LKD_TF_IN_BLOCK` pendant `lkd_block_poll` et le retirer après. Le registre montre alors `S` pour une tâche bloquée dans un `read` ou un `poll` (EAC lit `/proc/<ppid>/status`). task le fait déjà pour wait4, nanosleep et clock_nanosleep.

5. `tests/test_core_dispatch.c` définit des gestionnaires non statiques (`lkd_sys_getpid`, `lkd_sys_read`, `lkd_sys_write`, `lkd_sys_open`, `lkd_sys_ioctl`...). MESURE : le lien échoue dans la construction commune par symbole dupliqué avec le module fs (`./build.sh -k 0` : `FAILED: tests/test_core_dispatch`). task a rendu ses gestionnaires `static`. Demande : `static` dans le test, ou une macro `LKD_SYSCALL_STATIC`.


## Traitée le 2026-10-02 (mainteneur d'intégration)

Point 1 : fait (lead, 16 h 20). Point 2 : `lkd_task_notify_stop`, `lkd_task_notify_continue`, `lkd_task_uname`, `lkd_task_shares_host` déclarées dans `task.h`, remplaçants faibles dans `stubs_task.c`, redéclarations locales retirées. Point 3 : non changé. Le crochet de fork de ptrace lit le lien du parent avec l'identité du parent (`lkd_ptrace_fork_child`) ; passer task en premier casserait l'auto-attachement. Le défaut que cela causait (tid du parent pris pour un ancien tid à l'exec, cluster de sondes EAC) est corrigé dans `lkd_ptrace_exec_commit`. Point 4 : `lkd_block_poll` et `lkd_block_wait_wake` posent `LKD_TF_IN_BLOCK` (imbrication conservée). Test : `test_task_proc` cas `blocked_thread_shows_sleeping`. Point 5 : `LKD_SYSCALL` déclare maintenant le gestionnaire `static` (tous les gestionnaires de l'arbre et des tests le sont, 269 définitions) ; `test_core_dispatch` se lie avec tous les modules et garde ses numéros, car il remet la table à zéro (`lkd_dispatch_reset`).
