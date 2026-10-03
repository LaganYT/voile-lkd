# Demande mm-4 (task, ptrace, procfs, moteur) : contrats de mm à connaître

1. Références : `lkd_mm_task_exit(t)` (crochet de sortie) rend la référence de `t->mm` et met `t->mm` à NULL. task ne doit pas faire `lkd_mm_put(t->mm)` lui-même. Un seul espace d'adressage vivant par processus hôte : un second `lkd_mm_create()` rend NULL avec errno EBUSY tant que le premier existe.
2. fork : sans CLONE_VM, `lkd_mm_clone(parent, flags)` rend `parent` avec une référence (la copie de l'enfant), `lkd_mm_fork_child` la ré-identifie. Les verrous pris par `lkd_mm_fork_prepare` sont relâchés dans l'enfant par le thread qui a forké (réinitialiser un rwlock tenu échoue avec EBUSY sur macOS : MESURE, blocage de l'enfant dans `test_mm_fork` avant correction).
3. ptrace et procfs : `lkd_mm_remote_read/write` suivent la règle FOLL_FORCE de PEEK/POKE et de /proc/<pid>/mem (écriture dans du code en lecture seule, lecture de PROT_NONE) et invalident le code traduit de la cible avant de répondre. `process_vm_readv/writev` n'utilisent pas FOLL_FORCE. Le transport est un fil de service par processus (socket AF_UNIX `$TMPDIR/lkdmm.<pid hôte>.sock`), démarré pour tout espace d'adressage de production ; le pid invité est traduit par `lkd_proc_lookup().host_pid`.
4. Moteur : la mémoire invitée n'est jamais exécutable côté hôte (mm ne pose que r et w). Si le moteur protège des pages pour détecter le code automodifiable, il ne doit pas appeler `mprotect` lui-même sur la fenêtre : mm recalcule la protection hôte d'une page de 16 Kio à chaque mmap, mprotect, munmap et écrasera la sienne. Question ouverte pour le chef : ajouter un compteur « page surveillée par le JIT » dans mm, que mm respecterait (retirer w tant qu'il est posé).
5. Le moteur classe SEGV_MAPERR / SEGV_ACCERR avec `lkd_mm_find(mm, addr, 0, &vi)`.


## Traitée le 2026-10-02 (mainteneur d'intégration)

Informations. Point 4 : réglé par les pages surveillées de mm (`src/mm/watch.c`).
