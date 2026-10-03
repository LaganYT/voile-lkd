# Contrat d'interruption (kick) entre le moteur et LKD, vu par sig

Demandeur : sig. Destinataires : chef, moteur (`src/fexdarwin/src/LkdGlue.cpp`). Date : 2026-10-02.

## Ce que sig fait

1. Quand un signal est mis en file pour une tâche qui peut le prendre (`complete_signal`), sig pose `LKD_TF_SIGPENDING`, puis, verrous relâchés : `lkd_task_wake(t)` (tube de réveil, réveille toute attente LKD), sonne l'attente futex partagée éventuelle (`sig_ring_futex`), et appelle `ops->kick(t)` si `t` n'est pas la tâche courante. Un signal fatal (SIGKILL, ou action par défaut terminante) marque tous les threads `LKD_TF_FATAL` et fait de même pour chacun.
2. Une attente bloquante dans LKD ne compte jamais sur le signal hôte pour se terminer : tube de réveil, puis test de `lkd_sig_interrupted` avant de dormir. Le kick ne sert qu'au code JIT.
3. Les attentes futex sur clé partagée dorment dans `os_sync_wait_on_address(SHARED)` par tranches de 50 ms ; le kick (SIGUSR2 sans SA_RESTART) les fait revenir avec EINTR (MESURE darwin-memory-facts-b.md:96), et sig les sonne directement.
4. `lkd_signal_pending` est vrai aussi quand un masque temporaire (sigsuspend, ppoll, pselect6) attend d'être restauré : la boucle du moteur `while (lkd_signal_pending(t)) lkd_signal_next(t)` le restaure.

## Ce que le moteur doit faire (déjà fait d'après la lecture de LkdGlue.cpp:118-197)

1. SIGUSR2 réservé au kick, installé sans SA_RESTART, jamais utilisé pour autre chose ; le gestionnaire ne fait que lever un drapeau et, en code JIT, rediriger vers un point sûr qui appelle `lkd_signal_next`.
2. Après chaque `lkd_syscall`, après un kick, après `lkd_fault` : boucler sur `lkd_signal_next` tant que `lkd_signal_pending` ; sur -1, dérouler le thread et appeler `lkd_task_release`.
3. `lkd_fault` : SIGSEGV avec SEGV_MAPERR/SEGV_ACCERR et l'adresse exacte, SIGBUS, SIGILL, SIGFPE/FPE_INTDIV avec l'adresse de l'instruction, SIGTRAP de int3 avec SI_KERNEL (sig ne remplit alors pas si_addr, comme `force_sig`), pas à pas avec TRAP_TRACE et l'adresse. err et trapno vont dans le sigcontext construit par le moteur.
4. Limite documentée : un thread en code JIT n'est atteint qu'à son prochain syscall tant que la page d'interruption n'existe pas ; le kick à 1 ms de relance du moteur couvre les zones non sûres.
5. Garder `LKD_FAMSVC_DIR` et `LKD_FAMSVC_FD` (et le descripteur) à travers la ré-exécution hôte (voir sig-famsvc.md).
