# Statut du module mm (LKD)

Date : 2026-10-02. Jalon 4 (dernier de cette session) : le module couvre toute sa liste prioritaire. 8 programmes de test passent, soit 563 assertions.

## Ce qui existe

`src/mm/` (3705 lignes) :

- `vma.c` : table des VMA en arbre AVL trié par adresse de début. Recherche, insertion et retrait en O(log n), environ 100 octets par entrée, écart maximal par sous-arbre pour la recherche de trou (comme l'arbre augmenté de Linux). Remplace le tableau trié de HyperLinux (memmove linéaire, 4144 octets par entrée).
- `window.c` : fenêtre hôte. `mm_window_reserve` (`window.c:36`) teste d'abord la base voulue avec `mach_vm_allocate(VM_FLAGS_FIXED)`, qui échoue au lieu d'écraser. Puis il essaie 8 Tio, 16 Tio, 32 Tio, 0x7d80000000, puis n'importe où aligné sur 1 Gio, et pose `lkd_guest_base`.
- `lkd_mm_find_nowait` (`mm.c`) : `lkd_mm_find` sans attente, -EBUSY si la table des VMA est en cours de modification. Pour le rapport de faute fatale de fexdarwin, écrit depuis un gestionnaire de signal dont le fil peut tenir ce verrou. Cas `find_nowait` de `test_mm_map`.
- `mm.c` : espaces d'adressage ; `mm_map_locked` (`mm.c:440`), `mm_unmap_locked` (`mm.c:344`), `mm_host_cleanup` (`mm.c:380`) ; recherche de trou descendante de Linux avec indice, `MAP_32BIT`, `MAP_ABOVE4G`, alignement THP de 2 Mio pour l'anonyme (`mm.c:612`) ; `lkd_mm_check_range` fort (`mm.c:681`) ; `/proc/<pid>/maps` exact (`mm.c:848`, `mm.c:878`) ; smaps, stats, clés futex ; crochets de fork (`mm.c:1092`), d'exec et de sortie de thread.
- `sys.c` : mmap (ordre des erreurs de `do_mmap`, `sys.c:101`), munmap, mprotect, pkey_mprotect, pkey_alloc, pkey_free, map_shadow_stack, brk (`sys.c:318`), madvise, mincore (`sys.c:524`), msync, mlock, mlock2, munlock, mlockall, munlockall.
- `remap.c` : mremap (MAYMOVE, FIXED, DONTUNMAP, agrandissement sur place, déplacement par `mach_vm_remap`, `remap.c:133`).
- `shm.c` : shmget, shmat, shmdt, shmctl (IPC_STAT, IPC_SET, IPC_RMID, IPC_INFO, SHM_INFO, SHM_STAT, SHM_STAT_ANY, SHM_LOCK, SHM_UNLOCK). Les segments sont des objets `shm_open` ; la table est partagée par fork.
- `remote.c` : service de mémoire entre processus (un fil par processus, socket AF_UNIX) ; `lkd_mm_remote_read/write/render_maps` avec la règle FOLL_FORCE et l'invalidation du code de la cible ; process_vm_readv/writev (`remote.c:320`).
- `lkd_mm_ext.h` : entrées pas encore dans `mm.h` (voir `docs/requests/mm-2.md`). `NOTICE` : un bloc importé de HyperLinux (rendu d'une ligne de maps).

Modèle mémoire (LU, `mm_priv.h`) :

- I1 : une page hôte de 16 Kio sans VMA est une réservation neuve (PROT_NONE, zéro).
- I2 : sa protection est l'union de ses sous-pages. Elle est lisible si une sous-page est accessible, inscriptible si une sous-page l'est, jamais exécutable.
- I3 : une page hôte d'un mapping partagé (MAP_SHARED anonyme, SysV, fichier partagé) n'appartient qu'à ce mapping.

Fichiers privés : une page hôte entière est projetée directement (copie sur écriture de l'hôte) si son offset de fichier est aligné sur 16 Kio et si elle commence avant la fin du fichier. Les autres pages sont copiées par `pread`. La recherche de trou préfère une adresse congrue à l'offset modulo 16 Kio, pour permettre la projection directe.

## MAP_SHARED plus petit qu'une page hôte (demande du chef, premier lancement de Wine)

Cas déclencheur : le client Wine mappe le memfd de 4 Kio de wineserver (KUSER_SHARED_DATA) en `MAP_FIXED|MAP_SHARED|PROT_READ` sur sa réservation privée de 4 Kio à 0x7ffe0000 ; le serveur met la page à jour toutes les quelques ms, elle doit être réellement partagée. Avant : EINVAL. Maintenant : réussi.

Ce qui est géré (MESURE, `test_mm_shpart` 85 assertions, `test_mm_window` cas `wine_kuser_shared_data_production_window`) :

- Un VMA MAP_SHARED (fichier, memfd, anonyme, SysV) peut couvrir 4, 8, 12 Kio ou toute taille de 4 Kio. La ou les pages hôtes de 16 Kio qu'il touche sont mappées depuis l'objet partagé (`MAP_SHARED`, `MAP_FIXED`, décalage de fichier = décalage du VMA moins sa position dans la page hôte). Les pages invitées voisines de la page hôte (les « soeurs ») aliasent les octets de l'objet à cet endroit.
- Condition d'adresse : l'adresse et le décalage de fichier doivent avoir la même position dans une page de 16 Kio (`(adresse - décalage) % 16 Kio == 0`). Un mapping non fixe choisit lui-même une telle adresse (4 Kio à l'offset 4 Kio tombe sur une adresse ≡ 0x1000 mod 16 Kio). Un mapping FIXE à une adresse alignée sur 16 Kio avec l'offset 4 Kio, ou avec un offset plus petit que la position dans la page, rend EINVAL avec un message visible. La mémoire anonyme partagée n'a pas d'offset : toute adresse de 4 Kio convient.
- Règle de sûreté à la création : les soeurs doivent être non mappées, ou PROT_NONE privées, ou d'autres morceaux du MÊME objet à la même position relative. Sinon EINVAL avec message (« siblings must be unmapped or PROT_NONE »). Un autre objet dans la même page hôte : EINVAL.
- Règle ensuite : un `mmap` privé accessible dans une soeur rend EINVAL ; un `mprotect` qui rendrait une soeur accessible rend ENOMEM, avec un message visible. Rien n'est jamais écrit dans les octets de l'objet depuis les soeurs (le remplissage et la remise à zéro des pages privées sautent les pages hôtes partagées).
- La vue 4 Kio du VMA reste exacte : les protections sont par VMA, la page hôte prend l'union (une page partagée en lecture seule avec soeurs PROT_NONE est PROT_READ côté hôte). Les copies du noyau (`lkd_user_*`) et la classification du moteur par `lkd_mm_find` voient la vue exacte (EFAULT sur une soeur PROT_NONE).
- munmap / mprotect d'un morceau : le VMA se découpe ; une page hôte dont le dernier VMA partagé a disparu mais qui garde des soeurs PROT_NONE redevient de la mémoire privée neuve (zéros) : l'objet ne reste pas visible derrière elles, et un `mprotect` ultérieur de ces soeurs réussit et lit des zéros. Les réservations autour restent privées PROT_NONE.
- fork : le fils garde le mapping comme vrai mapping hôte partagé (test : le fils écrit par un second mapping, le père le voit ; les règles de soeurs restent valables dans le fils). mremap d'un morceau partagé : déplacement possible si la position dans la page hôte est conservée.

Limite à connaître (ne peut pas être corrigée par mm) : la vue exacte n'est pas imposée par le matériel. Un accès du CODE INVITÉ à une soeur PROT_NONE ne fautera pas, car la page hôte est lisible (protection hôte = union) : il lit les octets de l'objet. Le SEGV_ACCERR n'est levé par le moteur que si l'hôte fautes. Les accès du noyau LKD sont, eux, refusés exactement. Aucun accès privé accessible ne peut aliaser l'objet, donc rien n'est écrit par erreur.

## Pages de code surveillées par le JIT (`src/mm/watch.c`, `src/mm/lkd_mm_watch.h`)

API (autonome, utilisable en C++ par le moteur) :

- `int lkd_mm_watch_code(uint64_t gva, uint64_t len)` : toute page hôte de 16 Kio touchant la plage perd PROT_WRITE, même si le VMA invité est inscriptible. Idempotent, sans verrou quand tout est déjà surveillé. Un bit par page hôte, dans un bitmap `MAP_NORESERVE` (une fenêtre de 2^46 octets coûte 512 Mio d'espace d'adressage, seuls les mots touchés sont alloués).
- `int lkd_mm_code_write_fault(uint64_t gva)` : appelé par le gestionnaire SIGSEGV/SIGBUS du moteur pour un store fautif. Si la page est surveillée et si le VMA autorise l'écriture : `ops->invalidate_code(page, 16 Kio)` une seule fois, la surveillance est levée, la page redevient inscriptible, retour 1 (rejouer le store). Sinon 0 (vraie faute invitée). Un retour 1 sur une page déjà redevenue inscriptible par un autre thread est borné à 4 réponses consécutives par thread et par page, ensuite 0 : un store qui échoue pour une autre raison ne boucle pas.
- `int lkd_mm_code_watched(uint64_t gva)` pour les tests.

MESURE importante pour le moteur : un store sur une page en lecture seule NON RÉSIDENTE donne une faute de TRADUCTION (ESR 0x92000046, DFSC 0x06), pas une faute de permission (0x0f). Le gestionnaire du moteur ne doit pas filtrer sur DFSC : le critère est « abort de données (EC 0x24) avec WnR = 1 ». Un cas de test le couvre (`store_to_nonresident_watched_page`).

Verrou de surveillance : mutex récursif, pris avant le verrou de lecture des VMA, jamais tenu pendant du code invité, pendant `ops->invalidate_code` ni pendant une copie `lkd_user_*`. Il protège la cohérence entre les bits et les protections hôtes (`mm_host_refresh`, fenêtres d'écriture temporaires de mm, les deux fonctions de l'API). Le gestionnaire de signal prend donc un mutex : acceptable parce que le code interrompu est du code invité ou `lkd_uc_copy`, jamais du code de mm. `ops->invalidate_code` est appelé depuis ce gestionnaire et ne doit prendre aucun verrou que du code invité pourrait tenir.

Cohérence :

- `mprotect` recalcule les protections sans redonner l'écriture aux pages surveillées. `mremap` : la plage déplacée n'est pas surveillée à sa nouvelle adresse (le code traduit à l'ancienne est invalidé). `munmap` et reset d'exec lèvent la surveillance des pages hôtes libérées ; un `munmap` partiel garde celle d'une page encore utilisée par un autre VMA. Le fork : l'enfant efface tous les bits et rend leur protection aux pages (il n'hérite d'aucune traduction), le parent garde les siens.
- Copies du noyau : `lkd_mm_check_range` (donc `lkd_user_write` et toutes les copies) lève la surveillance et invalide avant la copie. `lkd_mm_remote_write` et `process_vm_writev` font de même. MADV_POPULATE_WRITE ne réécrit pas une page surveillée. MADV_DONTNEED sur une page surveillée garde la protection.
- MAP_SHARED et SysV : même règle dans ce processus. Limite : une écriture d'un AUTRE processus dans une page partagée traduite ici n'est pas vue (aucune faute dans ce processus).

Récupération de faute des copies (core) : sans correctif, une copie qui fautes sur une page surveillée après le contrôle (un autre thread surveille de nouveau entre le contrôle et la copie) rend -EFAULT. MESURE dans le scratchpad : `lkd_user_write` rend -14 sans correctif, 0 avec une invalidation avec le correctif. Demande et diff exact : `docs/requests/mm-5.md`.

Tests : `test_mm_watch` 104 assertions (store qui faute puis rejoué, invalidation unique avec la bonne plage, `lkd_user_write` et `process_vm_writev` et POKE forcé, page hôte partagée avec un VMA de données, mprotect qui garde la surveillance, munmap/mremap/exec qui la lèvent, MAP_SHARED, fork, page non résidente, bornage des réessais).

## Verrou de surveillance dans l'enfant d'un fork (signalé par l'agent sig)

Un blocage de `lkd_mm_fork_child -> mm_host_refresh -> mm_watch_lock` venait d'une version intermédiaire de `watch.c` : l'enfant faisait `mm_watch_unlock()` sur le mutex récursif pris par `fork_prepare`, et macOS refuse ce déverrouillage depuis le thread de l'enfant (MESURE : `test_mm_fork` bloquait). Le verrou est maintenant reconstruit par `mm_watch_fork_child()`, et ce appel est la toute première opération de `lkd_mm_fork_child`. Le nouveau cas `fork_watch_lock_in_child` de `test_mm_fork` (35 assertions au total) fait fork trois fois avec une page surveillée, utilise le verrou dans l'enfant (mmap, surveillance, mprotect, madvise, munmap), puis fork depuis un thread secondaire ; chaque enfant a un délai de 10 s et le test un délai de 90 s. `test_task_bigwin` : 525 ms, passe. Si un binaire plus ancien de liblkd.a bloque encore, il faut le reconstruire.

## Correctif task-4 : parcours de la fenêtre en O(VMA) (2026-10-02, soir)

- Cause (LU, demande `docs/requests/task-4.md`) : `mm_host_cleanup` avançait d'une page hôte de 16 Kio à la fois. Sur toute la fenêtre de production (2^46 octets), cela fait 2^32 recherches dans l'arbre, à chaque execve (`lkd_mm_reset_for_exec`) et à chaque fin de processus (`mm_destroy`).
- Correctif : `mm_host_cleanup` saute à la page du prochain VMA et fait un seul `mmap` de remise à zéro par intervalle vide. Le même défaut existait ailleurs et a été corrigé :
  - `mm_host_refresh` parcourt maintenant par VMA et par plage, avec un seul `mprotect` pour un vide ou pour l'intérieur d'un VMA ;
  - la boucle de remplissage de `mm_map_locked` ne traite plus que les pages hôtes de bord et celles qui portent des données de fichier ;
  - MADV_DONTNEED, WIPEONFORK et l'ancien intervalle de mremap DONTUNMAP passent par `mm_discard_range`, qui fait une seule remise à zéro pour l'intérieur ;
  - la résidence de smaps et de stats est lue par région hôte (`mach_vm_region`) pour un VMA de plus de 256 Mio ;
  - les bits de surveillance d'un intervalle vide sont effacés par pages entières de bitmap.
  Les autres boucles restantes ne dépassent pas la taille demandée par l'appel (mincore, populate, copies).
- `lkd_mm_window_size()` est défini.
- MESURE : `test_task_bigwin` (module task) passe en 521 ms dans un build privé sans net, contre 20 114 ms d'après task-4. Le nouveau cas `whole_window_operations_are_fast` de `test_mm_window` fait sous 2 s : réservation de 1 Tio PROT_NONE, smaps, stats, DONTNEED de 1 Tio, munmap de toute la fenêtre, reset d'exec, destruction. Le programme entier tourne en 0,85 s.

## Ce qui passe (MESURE)

Mêmes résultats dans le build privé core + mm (script du scratchpad) et dans la construction commune (`./build.sh -k 0`, liblkd.a complet avec les autres modules).

| Programme | Assertions | Contenu |
| --- | --- | --- |
| `test_mm_vma` | 32 | arbre : recherches, écarts, 20 000 opérations aléatoires comparées à un tableau de référence |
| `test_mm_map` | 144 | mmap/munmap/mprotect sur vraie mémoire, accès hôtes sondés par write/read sur un tube, vraie faute SIGBUS/SIGSEGV dans un enfant, MAP_FIXED, NOREPLACE, indice, MAP_32BIT, THP, MAP_SHARED anonyme partagé avec un enfant, invalidation JIT |
| `test_mm_file` | 70 | fichier hôte réel, par `lkd_mm_map` (chemin du chargeur ELF) et par le syscall : projection directe, offsets de 4 Kio, motif de segments de ld.so, copie sur écriture, écriture MAP_SHARED visible dans le fichier (msync), règles EACCES/ENODEV/EOPNOTSUPP, « (deleted) » |
| `test_mm_misc` | 159 | brk, mremap, madvise, mincore, msync, mlock*, pkeys, format exact de maps (colonne 73, espace final), clés futex |
| `test_mm_fork` | 23 | vrai fork par `lkd_fork_host` : copie privée, partage MAP_SHARED, DONTFORK, WIPEONFORK, verrous mémoire non hérités ; remise à zéro à l'exec |
| `test_mm_shm` | 71 | SysV : création, attache par un autre processus, nattch, IPC_RMID différé, droits, IPC_INFO, SHM_STAT |
| `test_mm_remote` | 45 | process_vm_readv/writev (comptes partiels, erreurs), POKE forcé dans du code en lecture seule avec invalidation, service entre deux vrais processus hôtes |
| `test_mm_watch` | 104 | pages de code surveillées (voir plus haut) |
| `test_mm_window` | 37 | fenêtre de production de 2^46 à 8 Tio, opérations sur toute la fenêtre en moins de 2 s : adresses invitées 0x10000 et 0x7ffe0000, EPERM sous 0x10000, recherche sous 2^46 - 128 Mio, service démarré puis supprimé, repli sur 16 Tio quand 8 Tio est occupé |

Défauts trouvés par les tests et corrigés :

- `discard` remettait à zéro toute la page hôte pour un MADV_DONTNEED d'une seule sous-page.
- L'enfant d'un fork restait bloqué, car macOS refuse de réinitialiser un rwlock tenu (EBUSY). L'enfant relâche maintenant les verrous.

Suite complète (MESURE, `tools/run_tests.sh`) : les échecs restants sont `test_fs_pipe`, `test_sig_basic` et `test_task_time` (livraison de signaux, autres modules), ainsi que le lien de `test_core_dispatch` (symboles en double entre core et fs). Aucun ne touche mm.

## Syscalls enregistrés par mm

brk mmap munmap mprotect pkey_mprotect pkey_alloc pkey_free map_shadow_stack mremap madvise mincore msync mlock mlock2 munlock mlockall munlockall shmget shmat shmdt shmctl process_vm_readv process_vm_writev.

Non enregistrés (ENOSYS visible et compté) : process_madvise, membarrier, mbind, set_mempolicy, get_mempolicy, migrate_pages, move_pages, set_mempolicy_home_node, mseal, remap_file_pages.

## Limites connues (documentées)

1. Sous-pages de 4 Kio dans une page hôte de 16 Kio : une sous-page PROT_NONE ou en lecture seule, voisine d'une sous-page accessible, n'est pas protégée contre le code invité, car l'hôte applique l'union. Les copies du noyau (`lkd_user_*`) respectent la vue exacte. Conséquence (INFERE) : une page de garde de 4 Kio ne faute que si elle couvre une page hôte entière. Sont concernées la pile de thread de glibc (AT_PAGESZ 4096) et les gardes de Wine.
2. MAP_SHARED (fichier, SysV) exige adresse et décalage de même position dans une page de 16 Kio, sinon EINVAL ; un mapping privé accessible ne partage jamais une page hôte avec un mapping partagé (voir la section précédente). Un mapping partagé anonyme ou SysV ne grandit pas par mremap au-delà de son objet (ENOMEM).
3. Fichiers privés copiés : les pages copiées ne voient pas les écritures faites ensuite dans le fichier, alors que Linux les montre tant que la page n'a pas été écrite. Les pages entièrement au-delà de la fin du fichier lisent des zéros au lieu de lever SIGBUS.
4. MAP_GROWSDOWN rend EINVAL (pas d'extension de pile). Pas d'ASLR : mmap_base est fixe (haut de la fenêtre moins 128 Mio). RLIMIT_AS ne s'applique pas à mmap, RLIMIT_DATA ne s'applique qu'à brk.
5. MAP_POPULATE ne préfaute pas. MADV_WILLNEED, COLD, PAGEOUT et FREE ne font rien, ce que leur sémantique permet. MADV_REMOVE sur un fichier partagé rend EOPNOTSUPP, MADV_COLLAPSE rend EINVAL. KSM et THP ne sont que des drapeaux. Dans smaps, Pss vaut Rss et la résidence est connue par page hôte. SHM_LOCK n'est qu'un drapeau, shm_rss vaut 0.
6. mlock câble les pages hôtes (`mlock`). Un refus de l'hôte rend EAGAIN ou ENOMEM, jamais un faux succès.
7. La table SysV ne survit pas à une réexécution hôte (`docs/requests/mm-3.md`). nattch fuit si un processus meurt sans passer par les crochets de sortie.
8. Avec une fenêtre de 2^46, le TASK_SIZE invité est plus petit que celui de Linux (`docs/requests/mm-2.md` point 3).
9. Chemins d'erreur sans test : échec de `mmap` hôte pendant une projection directe, échec de `mach_vm_remap` dans mremap.
10. Coût du service entre processus (une connexion par requête) non mesuré.

## Suivant

Mesurer le coût de mmap/munmap et du service distant. Appliquer RLIMIT_AS. Ajouter membarrier (Wine s'en sert, avec un repli) et une politique NUMA à un nœud. Traiter les pages surveillées par le JIT (question de `docs/requests/mm-4.md` point 4).

## Phase des memfd et code aliasé (2026-10-02, 22 h 55, demande wine-7)

`mm_backing.hbias` : un memfd peut avoir une phase hôte (offset hôte = offset du fichier + phase), fixée par son premier mmap, pour qu'un MAP_SHARED à une adresse non congrue à l'offset modulo 16 Kio soit possible. Les surveillances de code JIT couvrent toutes les vues d'un même objet partagé dans le processus (`mm_alias_pages`). Détail, mesures et limites : `docs/requests/wine-7.md`.

## Coût de VmRSS (2026-10-03, profil du menu du jeu)

Le RSS de `lkd_mm_stats` :
- Compte par région hôte quand la région est entièrement dans la VMA : une seule requête `mach_vm_region`, MESURE même total que mincore (270180 kB des deux façons sur le témoin).
- Compte par mincore pour les pages de bord.
- Est gardé en cache 100 ms (`MM_RSS_TTL_NS`) tant qu'aucune projection n'est ajoutée ni retirée.

Précision : le RSS lu peut avoir 100 ms de retard sur des pages touchées ou libérées sans changement de projection. VmHWM n'est relevé qu'à ces lectures.

MESURE (témoin `w20_statbench` : 1000 petites VMA et 256 Mio touchés, moteur réel) :

| Opération | Avant | Après |
| --- | --- | --- |
| lecture de `/proc/self/status` | 4,45 ms | 24 à 26 us |
| parcours complet, sans cache | 4,5 ms | 1,26 ms |

Test : `test_mm_misc` cas `rss_counting_and_cache`.
