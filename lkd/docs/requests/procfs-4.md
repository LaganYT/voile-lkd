# Demande procfs-4 aux modules task, fs, net, ptrace

Date : 2026-10-02. Auteur : module procfs.

## fs
1. `fs_host_io`, `fs_stat_from_host` et `fs_get_root` (définis non statiques dans `src/fs`) sont utilisés par procfs (objet sur descripteur reçu d'un autre processus, lien `/proc/<pid>/root`), déclarés dans `src/procfs/procfs_priv.h`. Demande : les déclarer dans `src/fs/lkd_fs.h` sous les noms `lkd_fs_host_io`, `lkd_fs_stat_from_host`, `lkd_fs_get_root`, ou garantir qu'ils gardent leur nom et leur signature.
2. `lkd_vfs_getcwd` rend le texte de getcwd (avec `(unreachable)` hors racine) : utilisé tel quel pour `/proc/<pid>/cwd`. Rien d'autre.

## net
3. Limites inotify. procfs publie `/proc/sys/fs/inotify/{max_user_instances 128, max_user_watches 65536, max_queued_events 16384}` seulement si `inotify_init1` a un propriétaire. Si net applique d'autres limites, dire lesquelles (ou exporter une fonction `lkd_net_inotify_limits`).

## ptrace
4. `lkd_ptrace_check_access` rend `-EPERM` ; procfs traduit tout refus en `-EACCES` pour maps, environ, auxv, mem (`mm_access`, `kernel/fork.c`) et pour exe, cwd, root, fd/N (`proc_fd_access_allowed`). Pas de Yama : `/proc/sys/kernel/yama` est absent.
5. `TracerPid` : `lkd_ptrace_tracer_of(tid)` (mesuré dans `test_procfs_other` : PTRACE_TRACEME de l'enfant, le parent lit le tid du traceur).

## task
6. Pour `stat` : utime et stime d'un thread autre que l'appelant ne sont pas disponibles côté hôte (pas de port Mach des autres threads invités). procfs rend 0 pour la vue par thread d'un autre thread. Si task garde le port Mach du thread (`mach_thread_self` pris au spawn) dans la structure de tâche, procfs pourrait le lire (`thread_info THREAD_BASIC_INFO`).
7. `processes` de `/proc/stat` (créations depuis le démarrage) : task n'expose pas le compteur de pid de la famille ; la ligne est omise. Une fonction `uint32_t lkd_pid_peek(void)` suffirait.
8. `processor` (champ 39 de stat) : valeur de `pthread_cpu_number_np` pour le thread appelant, 0 pour un autre.


## Traitée le 2026-10-02 (mainteneur d'intégration)

Point 1 : `fs_get_root`, `fs_host_io` et `fs_stat_from_host` sont déclarées dans `lkd_fs.h` sous leurs noms actuels, qui restent stables, et retirées de `procfs_priv.h`. Point 3 : `lkd_net_inotify_limits` (`net.h`) ; procfs publie les limites réelles de net (128, 8192, 16384 ; avant, 65536 surveillances annoncées pour 8192 appliquées). Test : `test_procfs_global` `proc_sys_values`. Point 7 : `lkd_pid_peek` (`task.h`) ; `/proc/stat` imprime `processes` (identifiants distribués par la famille). Test : `test_procfs_global`. Points 2, 4, 5, 8 : informations, rien à changer. Point 6 (port Mach des autres threads) : non fait, lacune conservée.
