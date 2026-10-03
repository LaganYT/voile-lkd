# Demande procfs-2 au module mm

Date : 2026-10-02. Auteur : module procfs.

1. `/dev/zero` (`drivers/char/mem.c` `mmap_zero`) : un mmap du fichier est de la mémoire anonyme (privé : pages à zéro copiées à l'écriture ; partagé : objet partagé anonyme). procfs fournit `int lkd_procfs_dev_is_zero(lkd_fdobj *o)` (`src/procfs/lkd_procfs.h`). Aujourd'hui `lkd_fs_mmap_check` répond `-ENODEV` pour cet objet (pas de fd hôte), donc `mmap(/dev/zero)` échoue. Demande : dans le chemin mmap de fichier, si `lkd_procfs_dev_is_zero(file)` alors traiter comme `MAP_ANONYMOUS` avec l'objet en nom `/dev/zero (deleted)` dans maps (Linux : `/dev/zero (deleted)`). Aucun test procfs ne l'exige (c'est un test mm).
2. Compteurs de crête. `/proc/<pid>/status` affiche `VmPeak` et `VmHWM`. `lkd_mm_stats` ne les donne pas : procfs affiche la valeur courante (borne inférieure de la crête, dit dans le statut). Demande : ajouter `hiwater_vm` et `hiwater_rss` à `struct lkd_mm_stats` (mise à jour à chaque map et à chaque `munmap` qui réduit `total_vm`).
3. `lkd_mm_remote_*` en mode fenêtre identité (tests) : le service n'est pas démarré par `lkd_mm_create` (`mm.c:128`). `tests/test_procfs_other.c` appelle `mm_remote_start()` lui-même. Rien à changer si c'est voulu.
4. `THP_enabled` de status est fixé à 1 (INFERE : les THP ne sont qu'un drapeau dans mm). Si mm retient `PR_SET_THP_DISABLE`, dire où le lire.


## Traitée le 2026-10-02 (mainteneur d'intégration)

Point 1 : `do_mmap` (`src/mm/sys.c`) traite un objet `/dev/zero` (`lkd_procfs_dev_is_zero`, faible dans core) comme de la mémoire anonyme : privé « /dev/zero » dans maps, partagé objet partagé anonyme « /dev/zero (deleted) » ; EACCES pour un partagé inscriptible sur un fd en lecture seule. Test : `test_procfs_global` cas `dev_nodes`. Avant : ENODEV.
Point 2 : `hiwater_vm` et `hiwater_rss` dans `struct lkd_mm_stats`. `hiwater_vm` est exact (avant chaque démappage, comme `update_hiwater_vm`). `hiwater_rss` est relevé à chaque lecture et avant les démappages de 2 Mio ou plus : c'est une borne inférieure. Remise à zéro à l'exec. procfs les affiche dans VmPeak et VmHWM. Test : `test_mm_misc` cas `high_water_marks`.
Point 3 : voulu (service distant démarré seulement pour une fenêtre de production). Point 4 : mm ne retient pas `PR_SET_THP_DISABLE` ; `THP_enabled` reste 1.
