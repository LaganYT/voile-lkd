# Demande task-3 au chef de projet (moteur fexdarwin)

Date : 2026-10-02. Auteur : module task. LU : `src/fexdarwin/src/LkdGlue.cpp`, `src/fexdarwin/src/main.cpp:168-237`.

1. `ops->exec_image` répond `-ENOSYS` (`LkdGlue.cpp`, `ExecImage`). task l'appelle après le point de non-retour (modules remis à zéro par `lkd_exec_commit`), comme Linux : un échec à cet endroit tue le processus par SIGSEGV (`src/task/exec.c`, `do_execve`). Tant que l'opération manque, tout `execve` réussi jusque-là tue donc le processus. Contrat attendu :
   - entrée : `img->host_fd` (lecture seule, CLOEXEC, fermé par LKD après l'appel), `img->guest_path` (cible de `/proc/self/exe`), `img->argv`, `img->envp`, `img->original_path` (pour `AT_EXECFN`). Le fichier est un ELF64 x86-64 `ET_EXEC` ou `ET_DYN` déjà vérifié ; `PT_INTERP` reste à charger par le moteur.
   - charger les segments par `lkd_mm_map` dans le mm déjà vidé, construire pile, auxv et `AT_RANDOM`, appeler `lkd_proc_set_layout(t, &layout)` (sert à `PR_GET_AUXV` et procfs), poser les registres par `ops->set_regs` sur le modèle de `start_thread` (RIP = point d'entrée, RSP, tous les autres à 0, `fs_base` 0), invalider le code traduit de tout l'espace, rendre 0.
   - un exec par ré-exécution de l'hôte doit appeler `lkd_family_export()` avant, garder `LKD_FAMILY_FD` (et `LKD_FAMSVC_DIR`, `LKD_FAMSVC_FD`), puis dans la nouvelle image appeler `lkd_task_create_main` avec `spec.pid` = le pid invité : task reprend alors l'enregistrement existant (même pid hôte, ppid, pgid, sid, limites).

2. Fin du thread principal invité seul (`exit` et non `exit_group`, d'autres threads vivants) : `lkd_task_release` rend 0, le moteur fait `pthread_exit`, puis `main.cpp:233-236` voit `pthread_join` revenir et termine le processus avec 70. Linux laisse le groupe continuer. Demande : ne pas sortir de `RunWithLkd` à ce moment (attendre indéfiniment ; la fin vient de `lkd_task_release` == 1 dans un autre thread).

3. Après un fork, l'enfant garde le même `GuestThread` : son champ `Tid` vaut encore le tid du parent. task a déjà basculé l'identité quand `ops->fork_child` est appelé : y faire `GT(t)->Tid = lkd_task_tid(t)`.

4. `exit_group` : `lkd_task_release` rend 1 pour le thread qui l'appelle (et pour tout thread qui sort ensuite) ; le moteur peut appeler `_exit` sans attendre les autres threads. Le code rendu est le code de sortie, ou 128 + numéro de signal pour une mort par signal (le statut exact est dans le registre pour le parent).

5. `de_thread` (exec depuis un processus à plusieurs threads) attend que chaque autre thread passe par `lkd_signal_next` == -1 puis `lkd_task_release`. Un thread qui boucle dans du code traduit sans syscall n'est pas atteint tant que le kick ne passe pas par la page d'interruption de FEX (`LkdGlue.cpp`, commentaire de `KickHandler`). L'exec attend alors (journal toutes les 2 s).

6. Processus partageant la mémoire (vfork, posix_spawn) : `spawn_thread` est appelé pour une tâche d'un AUTRE processus invité (pid différent) qui vit dans le même processus hôte. Le moteur ne doit pas supposer `lkd_task_pid(child) == lkd_task_pid(parent)`. À son `execve`, task fait un fork hôte : dans l'ancien processus hôte le syscall rend `LKD_EXIT_THREAD` et `lkd_task_release` rend 0 ; dans le nouveau, l'exec continue normalement.

7. Limites de sûreté appliquées par task : 300 threads par processus, 64 enfants vivants par processus, 512 processus vivants par famille (`src/task/task_priv.h`, `TASK_MAX_*`) ; au-delà `-EAGAIN`. À relever seulement avec une preuve de besoin.
