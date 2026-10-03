# fs -> mm : mappings de fichiers et scellés memfd

## Ce que fs fournit

- `o->ops->mmap_fd(o)` : fd hôte mappable (fichiers ordinaires, memfd), absent pour les tubes, répertoires, O_PATH et fichiers virtuels.
- `lkd_fs_mmap_check(o, prot, flags)` (dans `src/fs/lkd_fs.h`) : contrôles de `do_mmap` propres au fichier, à appeler avant de mapper : `-EBADF` (O_PATH), `-ENODEV` (rien à mapper), `-EACCES` (pas ouvert en lecture ; MAP_SHARED|PROT_WRITE sans O_RDWR ou avec O_APPEND), `-EPERM` (MAP_SHARED|PROT_WRITE d'un memfd scellé F_SEAL_WRITE ou F_SEAL_FUTURE_WRITE). Testé dans `tests/test_fs_memfd.c`.
- `lkd_fs_memfd_seals(o)` : scellés d'un memfd, -1 sinon.

## Ce que fs attend de mm

1. `lkd_mm_has_writable_shared_mapping(mm, file)` : rendre 1 s'il existe dans `mm` un mapping MAP_SHARED de ce fichier qui peut devenir inscriptible, c'est-à-dire, comme `i_mmap_writable` de Linux, tout mapping partagé fait depuis une description O_RDWR (VM_SHARED avec VM_MAYWRITE), même s'il est en lecture seule à cet instant. Comparer l'objet ou, mieux, l'inode (`o->dev`, `o->ino`), parce qu'une réouverture par /proc crée un autre objet pour le même memfd. fs appelle la fonction par le pointeur `lkd_fs_wsm_query` (les tests le remplacent).
2. Après F_SEAL_WRITE ou F_SEAL_FUTURE_WRITE : refuser `mprotect(PROT_WRITE)` d'un mapping partagé de ce memfd (`-EACCES`, VM_MAYWRITE retiré par `seal_check_write`).
3. Limite à documenter côté mm : la requête ne voit que l'espace d'adressage de l'appelant. Un mapping inscriptible partagé fait par un autre processus de la famille (après fork) n'est pas vu ; Linux le verrait. C'est la seule écriture qu'un scellé F_SEAL_WRITE ne peut pas bloquer ici.


## Traitée le 2026-10-02 (mainteneur d'intégration)

Point 1 : déjà implanté par mm (`lkd_mm_has_writable_shared_mapping`, tests `test_mm_file`). Point 2 : `mprotect(PROT_WRITE)` d'un mapping partagé d'un memfd scellé (F_SEAL_WRITE ou F_SEAL_FUTURE_WRITE) rend EACCES (`src/mm/mm.c`, `lkd_fs_memfd_seals` faible dans core). Test : `test_fs_memfd` cas `sealed_memfd_mprotect`. Point 3 : limite conservée.
