# fs -> procfs : comment servir /proc, /dev et /sys par le vfs

L'interface est celle de `src/include/vfs.h` ; les précisions de comportement sont dans `docs/requests/fs-1.md` §3. Le test `tests/test_fs_vfs.c` (montage `/vproc`) sert d'exemple complet : nœuds `fd/N` en liens magiques, lien ordinaire, boucle, listing.

## Montage

- `lkd_vfs_mount_virtual("/proc", &ops, priv)` depuis `lkd_procfs_init` (fs est initialisé avant procfs, la racine existe déjà). Idem pour `/sys` et `/dev`. Le module fs monte `/dev/shm` (répertoire hôte) : il est trouvé par le préfixe le plus long et listé sous `/dev` sans que procfs ait à le connaître.
- `fstype` : "proc", "sysfs", "devtmpfs" ; `statfs` en déduit le f_type Linux (0x9fa0, 0x62656572, 0x01021994).

## /proc/<pid>/fd et fdinfo

- Liste : `lkd_fd_list(t, fds, max)`.
- Nœud `fd/N` : `getattr` avec `magic_link = 1`, `mode = S_IFLNK | 0700` ; `readlink` rend `lkd_fs_describe(o, buf, cap)` ("/chemin", "/chemin (deleted)", "pipe:[ino]", "/memfd:nom (deleted)", ou le `describe` du module propriétaire) ; `open` rend `lkd_fdobj_reopen(t, o, flags, out)` (nouvelle description indépendante, propre offset).
- `fdinfo/N` : `lkd_fs_fdinfo_basic(t, o, &pos, &flags, &mnt_id)` pour les lignes `pos`, `flags` (octal), `mnt_id`, puis `o->ops->fdinfo` s'il existe.
- `/proc/<pid>/cwd` et `root` : chemin invité canonique de la tâche ; `lkd_vfs_getcwd` rend le texte de getcwd (avec "(unreachable)" hors racine). Pour la racine, une fonction n'existe pas encore dans vfs.h ; demander si besoin.

## /proc/mounts et mountinfo

`lkd_vfs_render_mounts(t, mountinfo, &len)` (malloc, à libérer). Source des montages hôtes : "hostfs", type : le nom du système de fichiers hôte (apfs).


## Traitée le 2026-10-02 (mainteneur d'intégration)

Information utilisée par procfs (tests procfs verts). Rien à changer.
