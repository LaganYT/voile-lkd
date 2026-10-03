# Demande procfs-1 au noyau commun (core)

Date : 2026-10-02. Auteur : module procfs.

1. Chemin d'inclusion. `src/procfs/lkd_procfs.h` (API pour le moteur et les tests : fournisseur CPUID, backend de test, `lkd_procfs_dev_is_zero`) est inclus par chemin relatif (`"../src/procfs/lkd_procfs.h"`). Demande : ajouter `src/procfs` au chemin d'inclusion de `liblkd` et des tests, ou créer `src/include/lkd_procfs.h` qui fait `#include "../procfs/lkd_procfs.h"`. Même chose que `docs/requests/fs-1.md` point 2 pour `lkd_fs.h`.
2. `lkd_task_uname` est définie par task (`src/task/sys.c:25`) mais déclarée nulle part (demande `task-1.md` point 2). procfs la redéclare dans `src/procfs/procfs_priv.h`. Demande : la déclarer dans `task.h`.
3. `lkd_config_get` : même demande que `fs-1.md` point 1 (procfs la redéclare).
4. `fake_make_task` ne crée ni table de descripteurs, ni fsctx, ni mm. Les tests procfs les posent (`tests/test_procfs_util.h`, `procfs_fixture`). Si core les ajoute, les tests restent valables.
5. Ordre des crochets : procfs n'a aucun crochet (aucun verrou, aucun état par processus). Rien à changer.


## Traitée le 2026-10-02 (mainteneur d'intégration)

Point 1 : `src/include/lkd_procfs.h` renvoie à `src/procfs/lkd_procfs.h` ; tests et net incluent maintenant `"lkd_fs.h"` et `"lkd_procfs.h"`. Point 2 : `lkd_task_uname` déclarée dans `task.h`, redéclaration de `procfs_priv.h` retirée. Point 3 : fait (lead). Point 4 : non fait (même raison que fs-1 point 6). Point 5 : rien.
