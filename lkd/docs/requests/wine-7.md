# wine-7 : MAP_SHARED d'un memfd de 4 Kio à une adresse non congrue modulo 16 Kio -> EINVAL ; le module EAC du jeu boucle sans fin

Demandeur : agent wine-linux. Bloquant : RocketLeague.exe s'arrête avant l'initialisation de kernel32 (aucune fenêtre, aucun périphérique D3D).

MESURE (evidence/eac-runs/20261002T223742, LKD_TRACE=1, guest.trimmed.log ; la boucle a produit 49 millions de lignes, 3,4 Go, tronqué) :
- Processus du jeu (pid invité [71], juste après le chargement de ntdll à 0x6fffffd20000, pendant le mappage de kernel32) : boucle de
  `memfd_create(name, MFD_CLOEXEC)` ; `ftruncate(fd, 0x1000)` ; `mmap(addr, 0x1000, PROT_READ|PROT_EXEC, MAP_SHARED|MAP_FIXED_NOREPLACE, fd, 0)` pour addr = 0x6fffffd17000, 0x6fffffd27000, ... 0x6fffffdb7000 (pas de 64 Kio, ntdll - 0x9000 + k*64 Kio) ; `close(fd)`.
  Toutes les adresses rendent EEXIST (occupées par ntdll et ses voisins) sauf 0x6fffffd17000, la seule libre, qui rend EINVAL : `lkd: mm: MAP_SHARED at 0x6fffffd17000 offset 0x0: address and offset are not congruent modulo the 16 KiB host page`. 6970 tours de boucle en 100 s, sans fin.
- Ce code n'est pas Wine (Wine n'appelle jamais memfd_create avec un mmap fixe exécutable près de ntdll) : INFERE c'est le code Linux du module EAC dans le processus du jeu (« Starting Wine module mapping » du bootstrapper, puis 301). Sous Linux à pages de 4 Kio ce mmap réussit ; on ne doit pas changer ce que fait EAC, donc c'est à mm de l'accepter.
- Le memfd est neuf et fait 4 Kio ; la page hôte 0x6fffffd14000..0x6fffffd18000 n'a pas d'autre occupant d'après les EEXIST (à vérifier dans la table VMA).
Demande : accepter MAP_SHARED d'un objet dont (adresse - décalage) n'est pas multiple de 16 Kio, au moins pour un memfd créé par l'invité. Piste INFERE : LKD crée lui-même les memfd ; il peut placer le contenu de l'objet hôte avec un décalage interne choisi au premier mmap (ou réallouer l'objet hôte quand il n'est encore mappé nulle part et que sa taille est < 16 Kio), pour que la page hôte contienne l'objet à la bonne position ; les autres pages de la page hôte restent inaccessibles (PROT_NONE) selon la règle actuelle des voisins.

## Traitée le 2026-10-02, 22 h 55 (mainteneur d'intégration)

### 1. Phase d'un memfd (`src/fs/memfd.c`, `src/mm/sys.c`, `src/mm/mm.c`, `src/mm/remap.c`)

Le fichier hôte d'un memfd peut stocker l'octet x du fichier invité à l'offset hôte x + B, B valant 0, 4, 8 ou 12 Kio (la « phase »).
- Quand la phase est fixée : au premier mmap du memfd (tout processus de la famille, toute description). Ce mmap passe tous ses contrôles d'abord, ensuite la phase est fixée, et le contenu est déplacé si B n'est pas 0. Elle ne change plus ensuite.
- Valeur choisie : pour un MAP_SHARED fixe, B = (adresse - offset) mod 16 Kio. Pour toute autre vue, la phase déjà fixée, ou 0.
- Stockage : bits 16 à 18 du mot de scellés, qui est partagé par fork et par les réouvertures. F_GET_SEALS ne les montre pas.
- Toutes les opérations de fs ajoutent B : read et write (offset propre ou offset de l'hôte), pread et pwrite, readv et writev (RWF_APPEND compris), sendfile, splice, copy_file_range, ftruncate, fallocate, lseek (SEEK_END, DATA, HOLE), fstat, et la limite de F_SEAL_GROW.
- Côté mm : `mm_backing.hbias` s'applique au contrôle de congruence, à la projection hôte partagée, à la projection directe et aux copies `pread` des vues privées, à mremap, et à la recherche d'adresse (une vue non fixe reçoit une adresse congrue à offset + B).
- `/proc/<pid>/maps` montre l'offset invité.
- process_vm passe par la mémoire invitée, il n'est pas touché.

Ce qui reste refusé, de façon visible (message une fois dans le journal) :
- Une vue MAP_SHARED fixe qui demande une autre phase rend EINVAL.
- Un fichier hôte ordinaire ne peut pas être décalé : la règle de congruence existante s'applique, avec EINVAL.

Usages bruts du descripteur hôte : `lkd_fs_pin_raw` fixe la phase à 0, ou refuse avec EOPNOTSUPP si elle vaut déjà autre chose. Il est appelé dans trois cas :
- SCM_RIGHTS (`src/net/scm.c`) ;
- exec par descripteur (`src/task/exec.c`) ;
- réouverture `/proc/<pid>/fd/N` servie à un autre processus (`src/procfs/remote.c`).

Considéré mais non fait :
- Mémoire anonyme partagée : elle n'a pas d'offset, toute adresse de 4 Kio convient déjà.
- Segments SysV : shmat à une adresse de 4 Kio non congrue rend toujours EINVAL. Un objet `shm_open` ne se lit ni ne s'écrit sous macOS, donc déplacer le contenu demanderait de le projeter ; non fait faute d'utilisateur connu.

Lacune : un autre processus qui lit ou écrit le memfd pendant le premier mmap qui fixe une phase non nulle peut voir le contenu en cours de déplacement.

MESURE : nouveau cas `phased_shared_mapping` de `test_fs_memfd`. Il couvre :
- la séquence d'EAC (RX, MAP_SHARED|MAP_FIXED_NOREPLACE, 0x3000 dans la page hôte, offset 0) sur un memfd qui avait déjà du contenu ;
- fstat, lseek et pread inchangés pour l'invité ;
- une vue RW choisie par mm à une adresse congrue à 0x3000, avec les écritures partagées dans les deux sens et avec pwrite ;
- EINVAL pour une autre phase, et une même phase acceptée à un autre offset ;
- une vue privée qui lit les bons octets ;
- EOPNOTSUPP de `lkd_fs_pin_raw`.
Le premier mmap de ce cas rendait EINVAL avant (message cité dans la demande).

### 2. Voisines

LU dans `evidence/eac-runs/20261002T223742/guest.trimmed.log` :
- Les adresses essayées en boucle (0x6fffe4b57000, 0x700017f77000, 0x6fffed4b7000…, pas de 64 Kio, toujours 0x3000 dans la page hôte) sont toutes libres : MAP_FIXED_NOREPLACE n'a pas rendu EEXIST.
- L'EINVAL venait seulement de la congruence.
- Dans la première phase de la boucle (0x7f8275fb8000…, positions congrues), les adresses étaient occupées : EEXIST, comme sous Linux.

INFERE : dans les pages de 16 Kio de ces candidats, les trois autres pages de 4 Kio sont non mappées. La règle des voisines est donc satisfaite et le premier essai réussit maintenant. Ce n'est pas vérifié sur un vrai lancement : la table des VMA du processus n'est pas dans le journal tronqué.

Voisine accessible privée : refusée, et il n'y a pas de solution exacte. Une page hôte de 16 Kio a une seule projection hôte : soit l'objet partagé, soit de la mémoire privée.
- Les voisines d'une vue partagée lisent donc forcément des octets de l'objet partagé.
- Avec une phase B, ce sont les octets de remplissage [0, B) du fichier hôte, eux-mêmes partagés avec toute autre vue de cette page hôte (autre processus, enfant de fork, autre vue du même processus).
- Y loger des données privées les ferait voir et modifier par ces vues : c'est l'aliasing interdit.

L'EINVAL visible reste la réponse.

### 3. Code aliasé (`src/mm/watch.c`)

Une surveillance de code JIT couvre maintenant toutes les pages hôtes du processus qui projettent les mêmes octets d'un objet partagé (`mm_alias_pages`).
- Elles perdent PROT_WRITE ensemble.
- Une faute d'écriture sur l'une les libère toutes et invalide la plage de chacune.
- Une vue créée après hérite de la surveillance (`mm_watch_alias_sync`).
- Une écriture par le descripteur (write, pwrite, kio) invalide aussi les vues surveillées (`lkd_mm_file_written`).

Lacune : les vues d'un AUTRE processus de la famille ne sont pas couvertes (comme avant pour MAP_SHARED).

MESURE :
- `test_mm_watch` cas `aliased_views_share_the_watch` : échoue sans le code d'alias (la vue A n'était pas surveillée), passe avec.
- Témoin invité `witnesses/elf-x86_64/src/w19_memfd_alias.c` (`EXEC=1 ./build.sh src/w19_memfd_alias.c`), sous `build-integration/host/fexdarwin-run`, mode SMC mtrack par défaut. Il fait : vue RX à 0x3000 dans sa page hôte, offset 0 ; vue RW choisie par le noyau ; code écrit par A, exécuté par B, réécrit par A (2 fois, puis 100 tours), puis pwrite par le descripteur.
  - Avec la bibliothèque corrigée : PASS 8/8.
  - Avec le code d'alias désactivé : FAILED 5/8 (B exécute encore l'ancien code).

Non-régression :
- 41/41 programmes, 6154 assertions.
- Témoins w01, w05, w06_smc, w10, w17, w18 : sortie 0.
- `wine cmd /c echo hi` : affiche hi, code 0.
