# Demande task-2 aux modules sig, ptrace, procfs et fs

Date : 2026-10-02. Auteur : module task.

## sig

1. Un processus invité créé par `vfork` ou `posix_spawn` (clone avec `CLONE_VM` sans `CLONE_THREAD`) vit comme thread hôte dans le processus hôte de son parent jusqu'à son `execve` (`src/task/clone.c`, en-tête). S'il se termine avant, `lkd_hooks_proc_exit` est appelé pour lui dans le processus hôte du parent. `lkd_sig_proc_exit` ne doit alors pas appeler `lkd_famsvc_shutdown` : tester `lkd_task_shares_host(t)` (`src/task/tasks.c`, demande task-1 point 2). De même `lkd_task_release` rend 0 pour lui : le processus hôte continue.
2. Arrêts de contrôle de tâche : appeler `lkd_task_notify_stop(t, signo)` à l'entrée en arrêt de groupe et `lkd_task_notify_continue(t)` à la reprise par SIGCONT. task publie l'état et le rapport unique pour `wait4`/`waitid`. Le SIGCHLD avec `CLD_STOPPED`/`CLD_CONTINUED` reste à sig.
3. task appelle `lkd_sig_child_event(ppid, pid, uid, code, status)` seulement quand le signal de sortie de l'enfant est SIGCHLD. Pour un autre signal de sortie (clone avec un signal choisi), task envoie `lkd_sig_send_pid(NULL, ppid, sig, &info)` avec un siginfo `CLD_*` (`src/task/wait.c`, `notify_parent`). Le retour 1 de `lkd_sig_child_event` (parent qui ignore SIGCHLD ou `SA_NOCLDWAIT`) libère le zombie aussitôt.
4. Les minuteries `ITIMER_*` de task envoient `lkd_sig_send_task(cible, SIGALRM/SIGVTALRM/SIGPROF, &si, 0)` avec `si_code = SI_KERNEL` depuis un thread hôte auxiliaire sans tâche courante (`src/task/time.c`, `itimer_fire`).

## ptrace

5. Après un fork hôte, task appelle `lkd_ptrace_task_init(t, t, flags)` dans l'enfant, `t` ayant déjà l'identité de l'enfant et `t->ptrace` contenant la copie du lien du parent (voir demande task-1 point 3).
6. `lkd_exec_finish` appelle `lkd_ptrace_event(t, PTRACE_EVENT_EXEC, ancien_tid)` ; s'il rend 0 et que la tâche est tracée sans `SEIZE`, task envoie SIGTRAP (`SI_USER`) comme `exec_binprm`.
7. `PR_SET_PTRACER` (Yama) répond EINVAL dans task (noyau modélisé sans Yama). Si ptrace modélise Yama, dire quel comportement task doit rendre.

## procfs

8. `uname` : `release` vaut `6.12.0-lkd`, `version` `#1 SMP PREEMPT_DYNAMIC LKD`, `nodename` le nom de la famille (`obelisk` par défaut). Utiliser `lkd_task_uname` pour `/proc/sys/kernel/osrelease`, `/proc/sys/kernel/hostname` et `/proc/version`.
9. Noyau modélisé sans `CONFIG_SECCOMP` : `prctl(PR_GET_SECCOMP)` et `PR_SET_SECCOMP` répondent EINVAL, `seccomp(2)` ENOSYS. `/proc/<pid>/status` ne doit donc pas avoir de ligne `Seccomp:` ni `Seccomp_filters:`.
10. `lkd_proc_lookup` rafraîchit l'enregistrement d'un processus local avant de répondre. Pour un processus d'un autre processus hôte, `state` vaut ce qui a été publié en dernier : `S` pendant `wait4`, `T` en arrêt, `Z` après la mort, sinon `R`. `cmdline` est tronqué à 4096 octets dans le registre (le contenu complet est dans `proc->cmdline` pour un processus local).

## fs

11. `lkd_exec_prepare` résout par `lkd_vfs_stat`, `lkd_vfs_realpath`, `lkd_vfs_to_host` puis `open` hôte ; pour une monture virtuelle, `lkd_vfs_open` puis `host_fd` de l'objet. Pour `execveat(fd, "", AT_EMPTY_PATH)`, task lit `ops->fstat` et `host_fd` de l'objet. Rien à changer si ces fonctions gardent leur contrat (MESURE : `tests/test_task_exec.c` passe avec le module fs actuel).


## Traitée le 2026-10-02 (mainteneur d'intégration)

Point 1 : `lkd_sig_proc_exit` n'arrête famsvc que lorsque le compteur de processus invités du processus hôte tombe à 0 (`src/sig/state.c`), ce qui couvre déjà l'enfant de vfork cohébergé. Point 2 : sig appelle `lkd_task_notify_stop` à la fin de l'arrêt de groupe (`park`) et `lkd_task_notify_continue` au rapport CLD_CONTINUED (`src/sig/deliver.c`). Test : `test_task_proc` cas `sigstop_and_sigcont_reach_wait4` (SIGSTOP vrai, `wait4(WUNTRACED)` = 0x137f une fois, SIGCONT, `waitid(WCONTINUED)`). Avant : `wait4` ne rendait rien et l'enfant était tué par son alarme. Points 3 à 11 : informations, conformes au code lu (rien à changer). Point 7 : noyau sans Yama, EINVAL conservé.
