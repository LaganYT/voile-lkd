# fs -> core : demandes sur les fichiers partagés

## 1. Exposer `lkd_config_get`

`lkd_fs_init` a besoin de `lkd_config.guest_root`, et `file.c` de l'uid/gid invités pour présenter le propriétaire des fichiers hôtes. `lkd_config_get` n'est déclaré que dans `src/core/core_priv.h`. Contournement local : `src/fs/fs_priv.h` le redéclare. Demande : le déclarer dans `src/include/lkd_internal.h`.

## 2. En-tête `lkd_fs.h` visible des autres modules

Les services fs pour le moteur, procfs, mm et task sont dans `src/fs/lkd_fs.h` (je ne peux écrire que sous `src/fs`). Demande : soit ajouter `src/fs` au chemin d'inclusion de `liblkd` et des tests, soit créer `src/include/lkd_fs.h` qui fait `#include "../fs/lkd_fs.h"`. En attendant, les tests incluent `"../src/fs/lkd_fs.h"`.

## 3. Précisions de `vfs.h` (interface de montage virtuel, sans changement de signature)

Ce que le module fs implémente et que procfs peut considérer comme stable :

- `readdir` ne rend ni "." ni ".." : le vfs les ajoute (il ignore ces noms s'ils sont rendus). Le vfs ajoute aussi les points de montage situés juste sous le répertoire listé (par exemple `shm` sous `/dev`).
- `getattr` est appelé avec `nofollow = 1` pendant la marche. Un lien ordinaire est lu par `readlink` et résolu dans l'espace invité (cible absolue depuis la racine de la tâche). Un nœud `magic_link` final suivi est ouvert par `open(rel)` ; `stat` qui suit un lien magique fait `open(rel, O_PATH)` puis `fstat` de l'objet. Un lien magique intérieur n'est suivi que si son texte est un chemin absolu, sinon `-ENOTDIR`.
- `open` est appelé pour un nœud non répertoire et pour un lien magique final ; un répertoire virtuel est ouvert par le vfs lui-même (objet `LKD_FD_DIR` sans fd hôte, `getdents` passe par `readdir`).
- Un montage n'est jamais démonté ; remonter au même point un montage hôte le remplace, un second montage virtuel au même point rend `-EBUSY`.

## 4. Commentaire de `fd.h` sur la réouverture des tubes

`fd.h` dit qu'un tube ne se rouvre pas par procfs. Linux le permet. Le module fs le fait pour la même extrémité (duplication du fd hôte) et rend `-ENXIO` pour l'autre extrémité, que l'objet ne détient pas. Demande : corriger le commentaire.

## 5. Symboles de gestionnaires dans les tests du noyau

`tests/test_core_dispatch.c` définit ses propres `lkd_sys_read`, `lkd_sys_write`, `lkd_sys_open`, `lkd_sys_ioctl` par `LKD_SYSCALL`, ce que CONVENTIONS §10.6 interdit dès qu'un module définit ces noms : l'édition de liens échouait (symboles dupliqués, MESURE). Contournement côté fs : tous les gestionnaires fs sont `static LKD_SYSCALL(...)`. Demande : soit déclarer `static` dans `LKD_SYSCALL` lui-même (les gestionnaires ne sont référencés que par `LKD_REGISTER` du même fichier), soit renommer les gestionnaires du test.

Construction complète (MESURE, 2026-10-02 fin de session) : `cmake --build` sans erreur, 24 programmes de test sur 24 passent.

## 6. Moteur factice

`fake_make_task` ne crée ni table de descripteurs ni fsctx ; les tests fs les posent eux-mêmes (`tests/test_fs_util.h`). Suggestion : les créer dans `fake_make_task` comme `lkd_task_create_main`.


## Traitée le 2026-10-02 (mainteneur d'intégration)

Points 1 et 2 : faits (lead). Point 3 : contrat recopié en tête de `src/include/vfs.h`. Point 4 : commentaire de `lkd_fdobj_reopen` corrigé dans `fd.h`. Point 5 : `LKD_SYSCALL` est `static` (voir task-1 point 5). Point 6 : non fait ; les tests posent fdt et fsctx eux-mêmes, et les créer dans `fake_make_task` doublerait les leurs.
