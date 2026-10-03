# Statut du module fs (LKD)

Date : 2026-10-02. Jalons 1 et 2 atteints : vfs, table de descripteurs, objets fichier, tube, memfd et scellés, 100 syscalls enregistrés, 6 programmes de test, arbre Debian testé.

## Ce qui existe

Fichiers (`src/fs/`, 5500 lignes) : `vfs.c` (montages, fsctx, résolution), `fdtable.c`, `file.c` (fichiers, répertoires, O_PATH, flux hôtes, réouverture, contrôles mmap), `dir.c` (getdents), `pipe.c`, `memfd.c`, `tty.c`, `sys_open.c`, `sys_rw.c`, `sys_stat.c`, `sys_path.c`, `sys_misc.c`, `xattr.c`, `fs_init.c`, `fs_priv.h`, `lkd_fs.h` (services pour moteur, mm, procfs), `NOTICE`.

Imports HyperLinux : la boucle de résolution de `vfs.c` (`hl_vfs_resolve_at`) et la traduction termios de `syscall_io.c`, marqués « imported from HL » et listés dans `src/fs/NOTICE`. Le reste est écrit pour LKD.

Syscalls enregistrés (100, tous ceux de `OWNERS.md` sauf `tee`) : open openat openat2 creat close close_range read write readv writev pread64 pwrite64 preadv pwritev preadv2 pwritev2 lseek stat fstat lstat newfstatat statx statfs fstatfs ustat access faccessat faccessat2 readlink readlinkat getcwd chdir fchdir chroot rename renameat renameat2 mkdir mkdirat rmdir unlink unlinkat link linkat symlink symlinkat chmod fchmod fchmodat fchmodat2 chown fchown lchown fchownat umask truncate ftruncate fallocate fsync fdatasync sync syncfs getdents getdents64 dup dup2 dup3 fcntl flock ioctl pipe pipe2 utime utimes utimensat futimesat sendfile copy_file_range splice vmsplice memfd_create mknod mknodat les 12 xattr fadvise64 readahead sync_file_range name_to_handle_at open_by_handle_at.

## Tests (MESURE)

Construction complète de l'arbre (`cmake --build`, tous modules) : 24 programmes sur 24 passent. Programmes fs :

| Programme | Assertions | Cas |
| --- | --- | --- |
| `test_fs_vfs` | 135 | 11 |
| `test_fs_fd` | 101 | 9 |
| `test_fs_io` | 265 | 10 |
| `test_fs_pipe` | 71 | 7 |
| `test_fs_memfd` | 111 | 10 |
| `test_fs_glibc` | 64 | 6 |
| total | 747 | 53 |

Les mêmes tests passent aussi avec une construction privée cœur + fs (remplaçants faibles de sig, mm, task), ce qui vérifie que fs ne dépend pas du comportement d'un module particulier.

Couverture : racine invitée temporaire avec liens relatifs et absolus, ".." après un lien, boucles (40 liens acceptés, 41 refusés), évasion impossible, montages liés, répertoires synthétiques, montage virtuel avec liens magiques et réouverture indépendante, chroot, openat2 RESOLVE_* ; partage d'offset par dup et par fork, limites RLIMIT_NOFILE et fd jusqu'à 65535, close_range, CLOEXEC et exec, verrous POSIX vus d'un enfant avec le bon pid, flock ; disposition octet par octet de `struct stat` (144 octets) et de `statx` (256 octets) ; getdents et getdents64 (taille d'enregistrement, d_off, d_type, reprise après lseek, tampon trop petit) ; tubes (capacité 65536, EOF, EPIPE et SIGPIPE, attente et interruption, splice) ; memfd et scellés ; séquence ld.so de glibc 2.41 sur `rocket-hyperlinux-20260928/linux-root/rootfs` monté en lecture seule (`/etc/ld.so.cache` absent : ENOENT, `/lib/x86_64-linux-gnu/libc.so.6` ouvert, 832 octets lus, newfstatat AT_EMPTY_PATH taille 1995216, pread64 des en-têtes de programme, readlink de `/lib64/ld-linux-x86-64.so.2`, getcwd, 730 entrées de répertoire). L'arbre Debian n'a pas été modifié (MESURE : `find -newer` vide).

## Faits hôtes mesurés (MESURE, macOS 27, ce Mac)

- Capacité d'un tube hôte : 65536 octets (remplissage non bloquant), comme Linux.
- APFS de la racine invitée et de `/tmp` : insensible à la casse (`pathconf _PC_CASE_SENSITIVE` = 0), conservant la casse.
- Fichier supprimé : `F_GETPATH` rend encore l'ancien chemin, `st_nlink` vaut 0 ; `ftruncate` répété et `mmap MAP_SHARED` fonctionnent.
- `kern.maxfilesperproc` = 138240 ; `setrlimit(RLIMIT_NOFILE, 138240)` accepté.
- `F_SETNOSIGPIPE` fonctionne sur un tube ; l'écriture sans lecteur rend EPIPE sans tuer le processus.
- `flock` et les verrous `fcntl` POSIX d'un même fichier entrent en conflit sous macOS (Linux les garde indépendants).

## Écarts connus (honnêtes, documentés dans le code)

1. Casse : un chemin invité d'une autre casse trouve le fichier (Linux : ENOENT) ; deux noms qui ne diffèrent que par la casse ne peuvent pas coexister. Rien n'est simulé ; `test_fs_vfs` enregistre ce comportement.
2. Chemins hôtes limités à 1024 octets (PATH_MAX de macOS) : un chemin invité valide plus long rend ENAMETOOLONG.
3. Course entre la marche et l'appel hôte : un lien posé entre les deux fait échouer l'appel (O_NOFOLLOW), sans évasion.
4. Répertoire ouvert puis renommé : les *at relatifs utilisent le chemin invité mémorisé à l'ouverture.
5. Tubes créés par LKD : le fd hôte est toujours non bloquant, O_NONBLOCK invité est dans l'objet ; un F_SETFL O_NONBLOCK n'est donc pas vu par un processus forké qui partage le tube. Atomicité PIPE_BUF : macOS ne garantit que 512 octets pour une écriture non bloquante.
6. Flux hôtes partagés (tty, tube du lanceur) : attente interruptible puis lecture ; un autre lecteur peut prendre les données entre les deux et la lecture hôte bloque alors sans être interruptible.
7. FIFO : un lecteur bloquant n'attend pas l'arrivée d'un écrivain à l'ouverture.
8. flock et verrous POSIX d'un même fichier se gênent (voir mesure). Verrous bloquants (F_SETLKW, flock) : essais répétés toutes les 5 ms, interruptibles, sans détection d'interblocage EDEADLK.
9. pipe2 O_DIRECT (mode paquet) : EINVAL. F_SETPIPE_SZ au-delà de 65536 : EPERM (fs.pipe-max-size vaut 65536 ici). splice, sendfile et copy_file_range copient par un tampon noyau (pas de zéro copie) ; tee n'est pas enregistré (ENOSYS compté).
10. SIGIO : O_ASYNC refusé par ENOSYS sur tubes, flux et objets des autres modules (accepté sur fichiers ordinaires, où Linux ne fait rien non plus). F_SETOWN, F_SETSIG mémorisés ; F_SETLEASE (autre que F_UNLCK) et F_NOTIFY rendent EINVAL comme un noyau où leases et dnotify sont désactivés.
11. memfd : scellés partagés entre processus forkés par une zone MAP_SHARED, perdus lors d'une ré-exécution hôte. Les écritures par un mapping partagé inscriptible d'un AUTRE processus de la famille ne sont pas vues par la requête EBUSY (elle ne connaît que l'espace d'adressage de l'appelant). MFD_HUGETLB : EINVAL. Attributs étendus user.* d'un memfd : non conservés (EOPNOTSUPP).
12. Réouverture par /proc d'un fichier supprimé : duplication avec offset privé ; le mode d'accès ne peut pas dépasser celui de la description d'origine (EACCES). Tube : seule la même extrémité se rouvre (ENXIO sinon).
13. O_TMPFILE : EOPNOTSUPP (glibc retombe sur un fichier nommé). name_to_handle_at : EOPNOTSUPP ; open_by_handle_at : EPERM sans capacité.
14. Propriétaire : le compte hôte du runtime est présenté comme l'identité invitée (1000 par défaut), root reste 0, tout autre propriétaire devient 65534. chown vers un autre utilisateur : EPERM. Un invité root (euid 0) reste soumis aux droits hôtes de l'utilisateur.
15. fsync ne force pas l'écriture du cache du disque (pas de F_FULLFSYNC) ; fallocate réserve au mieux (F_PREALLOCATE), APFS peut encore rendre ENOSPC plus tard ; PUNCH_HOLE écrit des zéros quand F_PUNCHHOLE refuse la plage.
16. Ré-exécution hôte (execve) : la table de descripteurs invités n'est pas encore exportée (voir `docs/requests/fs-task.md` §4).
17. Positions de répertoire et état des objets sans fd hôte : propres au processus, non partagés après fork (règle de `fd.h`).

## Demandes écrites

- `docs/requests/fs-1.md` (core) : exposer `lkd_config_get`, rendre `lkd_fs.h` incluable, précisions de `vfs.h`, commentaire de `fd.h` sur les tubes, symboles `lkd_sys_*` du test du noyau, `fake_make_task`.
- `docs/requests/fs-engine.md` : `lkd_fs_open_host_file`, `lkd_fs_open_guest_file`, `lkd_fs_install_stdio`, `lkd_fs_wrap_host_fd`, effets de `lkd_init` sur l'hôte, ignorer SIGPIPE hôte.
- `docs/requests/fs-procfs.md` : usage de l'interface de montage, liens magiques fd/N, fdinfo, mounts.
- `docs/requests/fs-mm.md` : `lkd_fs_mmap_check`, sémantique attendue de `lkd_mm_has_writable_shared_mapping`.
- `docs/requests/fs-task.md` : références fdt/fsctx, RLIMIT_NOFILE, exec ; coopération F_SETFL avec net.

## Suite possible

Export/import de la table pour l'exec hôte ; pid de ".." et inode synthétiques ; `tee` ; mesure de performance de la marche (un lstat par composant) sur un lancement réel de Wine.
