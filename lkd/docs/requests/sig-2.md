# Demandes de sig au module task

Demandeur : sig. Date : 2026-10-02.

## 1. Arrêts et reprises de contrôle de travaux pour wait4/waitid

sig fait l'arrêt de groupe (SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU par défaut) et la reprise (SIGCONT). Au dernier thread garé il appelle `lkd_sig_child_event(ppid, pid, uid, CLD_STOPPED, signo)` puis `lkd_task_wake_waiters(ppid)` ; à la reprise, `CLD_CONTINUED` (statut SIGCONT) de la même façon (`src/sig/deliver.c` `park`, `src/sig/queue.c` `prepare_signal`, `src/sig/send.c` `sig_notify_parent`). Le parent vit souvent dans un autre processus hôte : son wait4 doit lire l'état dans le registre partagé.

Demande : une fonction task `void lkd_registry_jobctl(int32_t pid, int code, int signo)` qui enregistre dans l'entrée partagée du processus « arrêté par signo, non encore rapporté » ou « repris, non encore rapporté », consommée par wait4 avec WUNTRACED/WSTOPPED (`(signo << 8) | 0x7f`) et WCONTINUED (`0xffff`), WNOWAIT respecté. sig l'appellera aux deux endroits ci-dessus dès qu'elle existe. Pour l'état 'T' de `/proc`, `lkd_registry_publish(t)` peut demander `lkd_sig_task_stopped(t)` (déclaration demandée à core, sig-1.md).

## 2. pidfd

`pidfd_send_signal` (sig) reconnaît un pidfd par `o->ops->name == "pidfd"` et lit le pid dans le premier champ `int32_t` de `o->priv`. Demande : que l'objet créé par `pidfd_open` (task) suive cette forme, ou indiquer une autre convention. Sans pidfd, l'appel répond `EBADF` comme Linux pour un descripteur qui n'est pas un pidfd.

## 3. Horloges invitées

Les délais absolus des futex sur CLOCK_MONOTONIC sont convertis en supposant que l'horloge monotone invitée est `lkd_host_mono_ns()` (CLOCK_MONOTONIC_RAW de l'hôte) ; CLOCK_REALTIME est `clock_gettime(CLOCK_REALTIME)` de l'hôte (`src/sig/futex.c` `abs_deadline`). Demande : confirmer que `clock_gettime` invité utilise les mêmes bases, ou exposer `int64_t lkd_guest_clock_ns(int clockid)` que sig utilisera.

## 4. État des signaux à travers une ré-exécution hôte

Linux garde à l'exec le masque bloqué, les dispositions ignorées et les signaux en attente. Une ré-exécution hôte perd la mémoire. sig fournit `lkd_sig_exec_export(t, buf, cap)` et `lkd_sig_exec_import(t, buf, len)` (testés dans `test_sig_basic`, cas `exec_state_survives_host_reexec`). Demande : que le chemin d'exec de task place ce blob dans la mémoire partagée de la famille avant `ops->exec_image` et l'importe dans la tâche recréée par la nouvelle image.

## 5. Ce que sig attend du registre (LU dans sig)

- `lkd_proc_lookup(pid).host_pid` et `.state` ('Z' : un kill vers un zombie répond 0 après le contrôle de permission, comme Linux), `.uid`, `.euid`, `.sid` ;
- `lkd_tid_lookup(tid).tgid` pour tkill/tgkill ;
- `lkd_proc_list` pour kill(-1) et kill(-pgrp) (sig lit au plus 512 processus).


## Traitée le 2026-10-02 (mainteneur d'intégration)

Point 1 : traité par `lkd_task_notify_stop`/`lkd_task_notify_continue` (voir task-2) au lieu d'une nouvelle fonction de registre. Point 2 : pas de pidfd (`pidfd_open` non enregistré, EBADF conservé). Point 3 : `lkd_guest_clock_ns(clockid)` (task, déclarée dans `task.h`) ; `abs_deadline` de `src/sig/futex.c` convertit avec l'horloge invitée (MONOTONIC invité = `CLOCK_UPTIME_RAW` de Darwin, et non `CLOCK_MONOTONIC_RAW`). MESURE : sur ce Mac les deux horloges ne diffèrent pas en ce moment (0,000 s), l'écart n'apparaît qu'après une mise en veille. Test : `test_sig_futex` `bitset_and_absolute_timeouts` (échéance prise sur l'horloge invitée). Point 4 : sans objet tant que l'exec est dans le même processus hôte (`ExecImage` du moteur) ; à reprendre si une ré-exécution hôte revient.
