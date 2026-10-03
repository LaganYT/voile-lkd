# wine-3 : fchdir sur un fd de répertoire reçu par SCM_RIGHTS -> ENOTDIR

Demandeur : agent wine-linux. Bloquant : wineboot ne crée pas les fichiers factices de system32 (kernel32.dll, ...), donc tout `wine <programme>` suivant échoue avec `could not load kernel32.dll, status c0000135`.

MESURE (logs/wine-linux-m2-wineboot-005.trace.log, -006.log) : NtQueryDirectoryFile sur Z:\opt\wine\lib\wine\x86_64-windows. wineserver fait `openat(dir, O_RDONLY|O_NONBLOCK) = 72`, `fstat = 0` (le serveur le classe FD_TYPE_DIR), puis l'envoie par sendmsg. Le client reçoit l'fd par recvmsg puis appelle `fchdir(fd) = -20 (ENOTDIR)` (dlls/ntdll/unix/file.c NtQueryDirectoryFile, `fchdir( fd )` avant opendir(".")). 124 échecs de ce type dans un seul wineboot. Les fchdir des processus fils au démarrage (fchdir vers le répertoire courant reçu du serveur) échouent de la même façon.

Témoin minimal : src/wine-linux/tests/dirfd.c (statique musl), lancé avec ROOT Debian et /opt/wine lié :
```
open /usr -> 3
local fchdir -> 0 errno 0
child received fd 6, fchdir -> -1 errno 20
```
Même résultat pour /opt/wine/lib/wine (lien en lecture seule). Dans le processus qui a ouvert l'fd, fchdir réussit ; après SCM_RIGHTS vers un autre processus, il échoue.
INFERE : l'objet fd reçu ne garde pas le chemin du répertoire ou sa nature de répertoire dans la table VFS du processus récepteur.
Demande : qu'un fd de répertoire reçu par SCM_RIGHTS accepte fchdir, et aussi getdents64 et openat(fd, ...) relatif (Wine utilise les deux).

## Traitée le 2026-10-02 (mainteneur d'intégration)

Cause (LU) : à la réception, `net_scm_import` passe le descripteur hôte à `lkd_fs_wrap_host_fd` sans chemin invité ; `fs_wrap_host_fd` (`src/fs/file.c`) créait un objet `LKD_FD_DIR` sans `gpath` ni montage, et `fs_dirobj_gpath` répond ENOTDIR sans `gpath` (fchdir, openat relatif).
Correctif (`src/fs/file.c`, `fs_wrap_host_fd`) : pour un répertoire ou un fichier ordinaire reçu sans chemin invité, le chemin hôte (`F_GETPATH`) est ramené à un chemin invité par la table de montages (`lkd_fs_host_to_guest`), le montage est retrouvé (`fs_mount_find`), le drapeau `O_DIRECTORY` est posé pour un répertoire. Un descripteur hors de toute vue invitée garde l'ancien comportement.
MESURE : nouveau cas `scm_rights_directory_across_fork` de `tests/test_net_unix.c` (deux vrais processus : fstat S_IFDIR, openat(fd, "inner.txt"), getdents64, fchdir, getcwd = "/sub", `/proc/self/fd/N` = "/sub") : échoue sans le correctif (étape 3, openat relatif), passe avec. Témoin `src/wine-linux/tests/dirfd` sous `build-integration/host/fexdarwin-run` (racine Debian, `/usr`) : `child received fd 6, fchdir -> 0 errno 0`. Tests fs (6) et net (sauf `test_net_host`, échec antérieur sans rapport) passent.
Limite : un répertoire dont le chemin hôte n'est sous aucun montage hôte (hors vue invitée) reste sans chemin invité (ENOTDIR à fchdir).
