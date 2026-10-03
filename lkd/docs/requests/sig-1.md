# Demandes de sig au noyau commun (core)

Demandeur : sig. Date : 2026-10-02.

## 1. Opérations atomiques sur un mot invité avec récupération de faute

Besoin : FUTEX_WAKE_OP, LOCK_PI, UNLOCK_PI et la liste robuste font un compare-and-swap sur un mot invité. Aujourd'hui `src/sig/futex.c` (`cmpxchg_word`) valide par `lkd_mm_check_range(gva, 4, 1)` puis fait l'atomique hôte sur `lkd_g2h(gva)`. Un `munmap` concurrent entre les deux ferait planter le processus hôte (le contrôle est consultatif, CONVENTIONS 2.6).

Demande : dans `usermem.c`, dans la plage de PC récupérable, `int lkd_user_cmpxchg32(uint64_t gva, uint32_t *expected, uint32_t desired)` qui rend 0 (échangé), 1 (valeur différente, `*expected` mis à jour) ou `-LX_EFAULT`. sig remplacera `cmpxchg_word` par cet appel.

## 2. Crochet de réveil pour les attentes futex partagées

Besoin : une attente futex sur une clé partagée dort sur `os_sync_wait_on_address(SHARED)` (seul moyen de réveil depuis un autre processus). `lkd_task_wake` n'écrit que dans le tube de la tâche, qui ne réveille pas cette attente. sig sonne lui-même la tâche quand il met un signal en file (`sig_ring_futex`), mais un réveil venu d'un autre module (ptrace, task) n'est vu qu'au bout d'une tranche de 50 ms.

Demande : que `lkd_task_wake(t)` appelle aussi `lkd_sig_wake_hook(t)`, faible et vide dans core, fort dans sig (il sonnera `sig_ring_futex`). Async-signal-safe : une écriture atomique et `os_sync_wake_by_address_all`.

## 3. Ajouts à `src/include/sig.h`

Fonctions déjà définies (fortes) dans sig, à déclarer pour les autres modules :

```c
/* 'T' (LKD_PS_STOPPED) while t is parked in a job-control stop, else 0. ANY, lock-free. task uses it
 * for the registry state, procfs for /proc/<pid>/stat. */
int lkd_sig_task_stopped(lkd_task *t);
/* Host re-exec: serialize what execve keeps (blocked mask, ignored dispositions, pending signals with
 * their siginfo). Returns the size needed; writes only when cap is enough. import returns 0 or -EINVAL. */
size_t lkd_sig_exec_export(lkd_task *t, void *buf, size_t cap);
int lkd_sig_exec_import(lkd_task *t, const void *buf, size_t len);
```

Correction de commentaire : `lkd_sig_exec_reset` dit « pending cleared of ignored ». Linux garde les signaux en attente à l'exec (`flush_signal_handlers` ne touche que les actions) ; sig suit Linux.

## 4. Défaut dans `LKD_BUG`

`lkd_internal.h` : `lkd_log("BUG %s:%d: " __VA_ARGS__)` n'ajoute ni `__FILE__` ni `__LINE__`, d'où l'avertissement `-Wformat-insufficient-args` et un affichage indéfini. sig n'utilise plus la macro.


## Traitée le 2026-10-02 (mainteneur d'intégration)

Point 1 : `lkd_user_cmpxchg32` (`src/core/usermem.c`, routine LL/SC dans la plage de PC récupérable, déclarée dans `lkd_internal.h`) ; `cmpxchg_word` de sig l'utilise. Test : `test_core_usermem` cas `cmpxchg32_swaps_and_recovers_faults` (PROT_NONE et lecture seule : EFAULT sans plantage). Point 2 : `lkd_task_wake` appelle `lkd_sig_wake_hook` (faible dans core, sig sonne l'attente futex partagée). Test : `test_sig_futex` (réveil par `lkd_task_wake` d'une attente partagée en 21 ms au lieu de la tranche de 50 ms). Point 3 : déclarations ajoutées à `sig.h`, commentaire de `lkd_sig_exec_reset` corrigé. Point 4 : `LKD_BUG(cond, fmt, ...)` imprime fichier et ligne. Test : `test_core_usermem` cas `bug_macro_prints_file_and_line`.
Au passage (MESURE) : `test_sig_futex` cas `mutex_contention` bloquait environ une fois sur 20 (réveil perdu). Cause : le raccourci « pas d'attente dans le seau » de `do_wake` lisait le compteur sans verrou alors que l'attente lit le mot avant d'être comptée. Raccourci retiré : 0 blocage sur 60 exécutions.
