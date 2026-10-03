# fs -> moteur : services pour le chargeur ELF et la mise en route

Déclarés dans `src/fs/lkd_fs.h` (à inclure par `"../fs/lkd_fs.h"` tant que `src/fs` n'est pas dans le chemin d'inclusion, voir `fs-1.md`). Tous testés par `tests/test_fs_glibc.c` contre la racine Debian.

## Objet fichier pour `lkd_mm_map`

- `int lkd_fs_open_host_file(const char *guest_path, int host_fd, lkd_fdobj **out)` : à partir de `img->host_fd` rendu par `lkd_exec_prepare`. Le descripteur hôte est dupliqué (CLOEXEC), celui de l'appelant reste à lui. L'objet est un fichier ordinaire en lecture seule dont le chemin invité (pour `/proc/<pid>/maps`) est `guest_path`. Erreurs : `-EBADF` (fd invalide), `-EACCES` (pas un fichier ordinaire, comme execve). Libérer par `lkd_fdobj_put`. Appelable depuis n'importe quel thread.
- `int lkd_fs_open_guest_file(lkd_task *t, const char *guest_path, lkd_fdobj **out)` : même chose par un chemin invité résolu par le vfs de `t` (liens symboliques, racine invitée).
- `o->ops->mmap_fd(o)` donne le descripteur hôte à mapper ; `lkd_fs_mmap_check(o, prot, flags)` fait les contrôles de `do_mmap` qui dépendent du fichier.

## Descripteurs standard

- `int lkd_fs_install_stdio(lkd_task *t)` : duplique les fd hôtes 0, 1, 2 et les installe comme fd invités 0, 1, 2. Un tty devient un objet « stream » (TCGETS, TIOCGWINSZ traduits), un tube ou une socket hérités reçoivent `F_SETNOSIGPIPE`.
- `int lkd_fs_wrap_host_fd(int host_fd, uint32_t lx_flags, const char *guest_path, lkd_fdobj **out)` pour tout autre descripteur hôte (prend possession).

## Ce que `lkd_init` change dans le processus hôte (via `lkd_fs_init`)

- `RLIMIT_NOFILE` hôte monté à `min(rlim_max, kern.maxfilesperproc)` (MESURE : 138240 sur ce Mac).
- `umask(0)` hôte : LKD applique lui-même l'umask invité à chaque création.
- Répertoires hôtes `/tmp` et `/dev/shm` invités : `$LKD_HOST_TMP`, `$LKD_HOST_SHM`, sinon `<DARWIN_USER_TEMP_DIR>/lkd-<uid>/{tmp,shm}` (mode 01777). Ils ne sont montés que si `lkd_config.guest_root` est fourni.

## Demande au moteur

- Ignorer `SIGPIPE` dans le processus hôte (ou au moins ne jamais le laisser tuer le runtime). LKD pose `F_SETNOSIGPIPE` sur ses tubes et sur les tubes et sockets qu'il enveloppe, mais un descripteur hôte que le moteur garde pour lui n'est pas couvert.
- Le chemin invité passé à `lkd_fs_open_host_file` doit être le chemin canonique (`lkd_exec_image.guest_path`), c'est le texte de `/proc/<pid>/maps`.
