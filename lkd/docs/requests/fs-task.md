# fs -> task et net : points de coopération

## task

1. Références : chaque thread tient une référence sur `t->fdt` et `t->fsctx`. Le crochet `lkd_fs_task_exit` les rend (et met les champs à NULL) ; `lkd_fs_proc_exit` fait de même si elles sont encore là. Le module task ne doit pas les rendre une seconde fois après ces crochets.
2. exec : `lkd_fs_exec_commit` ferme les descripteurs CLOEXEC ; task n'a pas à appeler `lkd_fd_close_on_exec`.
3. `RLIMIT_NOFILE` : fs lit `proc->rlimits[LX_RLIMIT_NOFILE].rlim_cur` sous `proc->lock` à chaque allocation. `setrlimit`/`prlimit64` doivent refuser `rlim_max > 1048576` avec `-EPERM` (fs.nr_open de Linux, la taille maximale de la table fs).
4. Exec par ré-exécution de l'hôte : la table de descripteurs invités (numéros, CLOEXEC, objets non hôtes) ne survit pas d'elle-même. fs peut fournir un export/import de la table (numéro invité -> fd hôte hérité, drapeaux, chemin invité) ; à demander quand le chemin d'exec sera fixé.

## net

1. `F_SETFL` et `FIONBIO`/`FIOASYNC` sur un objet de net : fs met à jour `o->status_flags` (O_APPEND, O_NONBLOCK, O_DIRECT, O_NOATIME) puis appelle `o->ops->fcntl(t, o, LX_F_SETFL, nouveaux_drapeaux)` ; une réponse `-EINVAL` est ignorée, toute autre erreur négative est rendue. O_ASYNC est refusé (`-ENOSYS`) tant que SIGIO n'existe pas.
2. `readv`/`writev` sur une socket appellent `ops->read`/`ops->write` segment par segment (pas de regroupement) : net peut préférer un chemin `sendmsg` interne, à discuter.
3. Objets fs utilisables par epoll : `fs_pipe_ops.poll` et `fs_stream_ops.poll` existent, les fd hôtes portent la disponibilité (`o->host_fd`).


## Traitée le 2026-10-02 (mainteneur d'intégration)

Points 1 à 3 : conformes (task ne rend pas fdt et fsctx deux fois ; `setrlimit(NOFILE)` au-delà de 1048576 rend EPERM, `src/task/sys.c:76`). Point 4 : sans objet tant que l'exec reste dans le processus hôte. net : conforme.
