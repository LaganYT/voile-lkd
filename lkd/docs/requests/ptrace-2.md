# Demande ptrace-2 (task, core) : wait4 ne se reveille pas sur un arret ptrace

Demandeur : ptrace. Date : 2026-10-02. Destinataire principal : task (`src/task/wait.c`), accessoirement core (`src/core/block.c`).

## Constat (MESURE, `tests/test_ptrace_transport.c`)

Un `wait4` bloquant qui attend un arret ptrace dure 11,1 ms en p50 (p99 11,3 ms), alors que le meme aller-retour (PTRACE_SYSCALL puis arret suivant) avec un `wait4(WNOHANG)` en boucle dure 42 us entre deux processus hotes. EAC fait environ 150 arrets de syscall : 1,7 s perdues sur ce seul mecanisme.

Cause (LU) : `do_wait` (`src/task/wait.c`, boucle `for (i < 10 && fam_seq() == seq)`) dort par `lkd_block_sleep(t, now + 10 ms)`. `lkd_block_poll` (`src/core/block.c:51-78`) vide le tube de reveil sur un `lkd_task_wake` mais, si la tache n'est pas interrompue, repart dormir jusqu'a l'echeance (c'est la semantique ecrite dans `lkd_internal.h`, "re-evalue"). Le notifieur fait pourtant son travail : ptrace appelle `lkd_task_wake_waiters(tracer_pid)` (qui incremente le compteur de famille et ecrit dans le tube du `wait4` marque `LT_IN_WAIT`) quelques dizaines de microsecondes apres l'arret, dans le processus du traceur (service de famille). Le reveil n'a aucun effet. Meme chose pour la mort d'un enfant d'un autre processus hote, `lkd_task_notify_stop` et les SIGCHLD : tous passent par `lkd_task_wake_waiters`.

## Correctif propose (MESURE sur une copie de `wait.c` dans le scratchpad, sans toucher a l'arbre)

Dans la boucle de `do_wait`, remplacer l'appel a `lkd_block_sleep` par une attente sur le tube de reveil lui-meme :

```c
for (int i = 0; i < 10 && fam_seq() == seq; i++) {
    if (lkd_sig_interrupted(t)) { interrupted = 1; break; }
    struct pollfd pw = { .fd = t->wake_rd, .events = POLLIN };
    if (poll(&pw, 1, 10) > 0) { char junk[64]; while (read(t->wake_rd, junk, sizeof junk) > 0) { } }
}
```

Resultat avec ce correctif : `wait4` bloquant, tracee dans un autre processus hote, p50 43 us, p90 53 us, p99 73 us (n = 1500). Sans lui : p50 11 110 us. Une variante plus propre est une fonction de core `int lkd_block_wait_wake(lkd_task *t, int64_t deadline_ns)` qui rend 0 des qu'un `lkd_task_wake` est consomme (ou -LX_ERESTARTSYS si interrompue), a utiliser par `do_wait`. Piege mesure : `lkd_block_wait_fd(t, t->wake_rd, POLLIN, ...)` ne convient pas, car le meme descripteur est passe deux fois a `poll` et macOS n'en signale qu'une entree.

## Autre point pour task : `lkd_ptrace_has_tracees` et le selecteur

`scan()` rend 0 (attendre) des que `lkd_ptrace_has_tracees(t)` est vrai, meme si `wait4(pid)` vise un pid qui n'est ni un enfant ni un tracee. Linux rend ECHILD dans ce cas (`do_wait` ne trouve aucun enfant qui satisfait le selecteur). Pour que ptrace puisse repondre exactement, task peut appeler a la place `int lkd_ptrace_wait_has(lkd_task *waiter, int32_t sel_pid, unsigned options)` (definie dans `src/ptrace/life.c`, meme selecteur que `lkd_ptrace_wait_event`, a declarer dans `ptrace.h`). Il faut aussi que `lkd_ptrace_wait_event` rende toujours `CLD_TRAPPED` pour un arret ptrace, y compris un arret de groupe (`why = ptrace ? CLD_TRAPPED : CLD_STOPPED` dans `wait_task_stopped`) : c'est fait.

Aussi `waitid` : pour un arret ptrace le `si_status` de `waitid` doit etre le code d'arret complet (`exit_code`, par exemple 0x405 pour PTRACE_EVENT_EXEC, 0x80 | 5 pour un arret de syscall), pas `(status >> 8) & 0xff` (`src/task/wait.c`, `scan`, branche ptrace). C'est `ev.status >> 8` en entier (kernel/exit.c v6.12 ligne 1358, `wait_task_stopped` : `infop->status = exit_code`).


## Traitée le 2026-10-02 (mainteneur d'intégration)

Point 1 (réveil) : nouvelle attente de core `lkd_block_wait_wake` (`src/core/block.c`), qui rend la main dès qu'un octet de réveil est consommé ; `do_wait` (`src/task/wait.c`) l'utilise au lieu de `lkd_block_sleep`. `lkd_task_wake_waiters` (`src/task/tasks.c`) atteint aussi les attentes d'un AUTRE processus hôte par famsvc (opcode task 96), pour la mort, l'arrêt et la reprise d'un enfant. MESURE : `test_ptrace_life` cas `blocking_wait4_wakes_at_once` (20 arrêts sous 100 ms ; avant : blocage, le test ne finit pas en 100 s) ; `test_task_proc` cas `wait4_wakes_at_once_across_host_processes` (10 tours sous 65 ms ; avant : environ 100 ms) ; `test_ptrace_transport` passe de 18 s à 1 s.
Point 2 (sélecteur) : `scan()` appelle `lkd_ptrace_wait_has` (déclarée dans `ptrace.h`, faible dans core). ECHILD pour un pid qui n'est ni enfant ni tracé, et pour `__WNOTHREAD` depuis un thread sans tracé. Pour que `wait4(nouveau pid)` juste après PTRACE_EVENT_FORK ne rende pas ECHILD, `lkd_ptrace_event` attend (borné à 2 s) que l'enfant forké ait créé son lien. Tests : `test_ptrace_life` `wait_selectors_see_tracees`, `wait_semantics`.
Point 3 (si_status) : code d'arrêt complet (`status >> 8`). Test : `test_ptrace_life` `group_stop_of_a_traced_task` (0x8013).
