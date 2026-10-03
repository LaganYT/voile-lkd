# Réutilisation du coeur HyperLinux par un moteur FEXCore en processus

Date : 2026-10-02. Statut : version complète de la première moitié de l'étude. Elle couvre la mémoire invitée, les VMA, le vfs, procfs, les objets fd, l'identité de processus et la conversion d'ABI. Les threads, fork/exec, signaux, futex, ptrace et le réseau sont dans `reports/hl-reuse-process.md`.

Conventions. LU : lu dans le code, avec fichier:ligne. MESURÉ : obtenu en exécutant un programme ou un test sur cette machine, avec la commande ou le journal cités. INFÉRÉ : déduction non vérifiée par exécution. Abréviations de chemins :

- HL : `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928/hyper-linux/src`, copie de travail, HEAD 8048a96.
- FEX : `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928/probes/fex-implementation-feasibility-20260929/source/FEX-0df84d3844bcdb87bb7d3f5b8fb0959cd009c038/Source/Tools/LinuxEmulation/LinuxSyscalls`, arbre intact.
- EXP : `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-wine-fex-darwin-20261002`.
- XP : `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928`, dossier d'expérience précédent, lecture seule.

Machine : Apple M5 Max, macOS 27.0.1, pages hôte de 16 Kio (`getpagesize()` = 16384, MESURÉ, annexe A).

## Résumé

1. LU. HL est un noyau Linux ARM64 écrit autour d'une mémoire invitée HVF. Un seul tampon hôte `host_base` de 64 Gio ou 1 Tio, adresse virtuelle invitée = adresse physique invitée = décalage dans ce tampon, tables de pages de niveau 1 construites par l'hôte, pages invitées de 4 Kio sur un hôte à 16 Kio (`guest.h:6-15`, `guest.c:65-66`).
2. MESURÉ (`wc -l`, `nm -u`). 84 fichiers, 40 617 lignes. 25 des 39 objets compilés n'ont aucun symbole HVF non résolu ; l'API HVF utilisée tient en 20 fonctions ; les handlers de fichiers, d'I/O, de temps et de procfs n'ont besoin que de cinq fonctions d'accès à la mémoire invitée. Reprise quasi telle quelle : 3 835 lignes (9 %). À adapter, ABI et accès mémoire : 9 082 (22 %). À réécrire : 6 197 (15 %). À abandonner : 7 563 (19 %). Couche processus, traitée dans l'autre rapport : 13 940 (34 %).
3. MESURÉ (annexe C). Les vrais objets de HL, liés avec un shim de quinze lignes qui remplace les cinq accesseurs de mémoire invitée par l'identité, exécutent dans un processus macOS ordinaire : `openat` par le vfs rooted, `read`, `write`, `fstat`, `getdents64`, liens symboliques résolus dans la racine invitée, `/proc/self/maps` rendu depuis la table VMA avec des découpes à 4 Kio, réouverture `/proc/self/fd/N` d'un fichier supprimé avec un curseur indépendant. 23 contrôles sur 23. Aucune VM HVF n'est créée.
4. Les trois choses que le MMU invité donne gratuitement n'existent pas en processus : protection par page de 4 Kio, adresses invitées basses, faute sur page retirée. MESURÉ (annexe A) : sur cette machine `mmap`, `munmap` et `mprotect` arrondissent à 16 Kio, refusent une adresse non alignée sur 16 Kio (EINVAL) et refusent un offset de fichier de 0x1000 (EINVAL). L'équipe FEXCore mesure en plus qu'un `mmap` fixe à 0x400000 échoue (errno 12) et que `__PAGEZERO` ne se réduit pas.
5. La table `guest_vma_t` et le rendu de `/proc/self/maps` sont du C pur et se reprennent comme vue invitée à 4 Kio. La structure doit être refaite : 4 144 octets par entrée, `memmove` linéaire, 16 384 entrées au plus. Les blocs de support de 2 Mio, les alias de VA, les segments de débordement et le tampon noyau sont à jeter.
6. ABI. HL parle l'ABI ARM64 générique. Le passage à x86-64 change les numéros, `stat`, les drapeaux `O_*`, `epoll_event`, `clone`, les signaux et une trentaine d'appels hérités. `dirent64`, `statfs`, `rlimit`, `flock`, `sockaddr`, `msghdr`, `timespec` ont la même disposition. FEX envoie 241 appels x86-64 tels quels au noyau hôte, HL n'en implémente que 128 de ces 241 (MESURÉ par comparaison de noms).
7. Recommandation : une couche syscall x86-64 neuve écrite pour FEXCore, avec import sélectif de HL (vfs, fd, procfs, noms de tâches, traductions de drapeaux et d'errno) et de FEX (structures `x64`, suivi des VMA et du code automodifiable, accès mémoire protégé). Ne pas compiler `LinuxEmulation` de FEX sur une bibliothèque « noyau ARM64 ».
8. Lacunes face à Wine et EAC : seals de memfd absents, `/proc/<pid>/maps` des autres processus, `/proc/self/task` et `/proc/self/fd` en répertoire, `/proc/self/mem`, `/proc/self/environ`, `mremap`, `process_vm_readv`, `userfaultfd` absents. `madvise`, `mincore`, `msync`, `mlock` sont des stubs. Voir section 5.
9. Estimation INFÉRÉE : 8 000 à 11 000 lignes écrites ou modifiées, pour environ 10 500 lignes reprises presque sans changement.
10. Cinq risques : la granularité 4 Kio sur 16 Kio, les adresses basses, la sémantique réelle de `munmap`/`mprotect`/`mremap` en processus, la fidélité de `/proc` et de memfd pour EAC, le partage de mémoire entre processus sans noyau Linux.

## 0. Arbre lu, correctifs privés, banc

### 0.1 État de l'arbre

LU (`git status --short`, `git diff --stat`, lectures seules) : 33 fichiers suivis modifiés (4 475 insertions, 1 560 suppressions) et 11 fichiers source non suivis, soit 44 chemins utiles. Non suivis : `fault_dump.c/h`, `proc_names.c/h`, `proc_ptrace.c/h`, `signal_notify.h`, `syscall_filemap.c/h`, `syscall_reopen.c/h`. HEAD : 8048a96 « docs: link the public examples sibling ».

`README.md` et `CLAUDE.md` ne sont pas modifiés et retardent sur le code. Le README annonce « ~26,000 lines » et « MAP_SHARED treated as MAP_PRIVATE » ; `src/` compte 40 617 lignes et traite le MAP_SHARED de fichier par un vrai mapping hôte (`syscall.c:837-856`). Le code fait foi.

### 0.2 Ce que les correctifs privés ont changé, et pourquoi

Les lignes touchées sont insertions plus suppressions de `git diff --stat`. Les raisons viennent des commentaires du code, des noms de tests ajoutés et de `XP/README.md` ; je ne les ai pas rejouées.

| Sous-système | Fichiers (lignes touchées) | Raison |
| --- | --- | --- |
| Table des mappings invités exacts à 4 Kio, séparée des blocs de support | `guest.c` (562), `guest.h` (73), `syscall.c` (608), `proc_emulation.c` (716), test `test-page-table-pool.c` (240) | Le résolveur d'EAC cherche une ligne `/libc-` ou `libc.so` dans `/proc/self/maps`. L'ancien suivi montrait des blocs anonymes de 2 Mio (`XP/README.md`, section « Lecture de la carte mémoire par EAC »). |
| Mappings partagés de fichiers sur IPA alignée sur 16 Kio | `syscall_filemap.c/h` (nouveau, 466), `syscall_shm.c` (17), `guest.c` | `syscall_filemap.c:1-3` : les entrées L3 choisissent des pages de 4 Kio pour laisser intactes les pages privées voisines sur un hôte à 16 Kio. |
| Réouverture de `/proc/self/fd/N` avec une description de fichier indépendante | `syscall_reopen.c/h` (nouveau, 361), `proc_emulation.c`, `syscall_fs.c` (45), `syscall_io.c` (415) | La réouverture par `dup` partageait la position laissée en fin de fichier ; la lecture de l'en-tête ELF de 832 octets du memfd renvoyait zéro (`XP/README.md`, même section). |
| Registre des tâches partagé dans la famille de fork | `proc_names.c/h` (nouveau, 539), `syscall_proc.c` (991), `proc_emulation.c` | `/proc/<tid>/comm`, `status`, `cmdline`, `exe` d'un autre processus ; allocateur de PID commun (`syscall_proc.c:76-113`). |
| Résolution de chemins dans l'espace de noms invité | `vfs.c` (334), `syscall_fs.c` (45), test `test-vfs-unit.c` (136) | Un préfixe Wine contient des liens absolus qui doivent repartir de la racine invitée. Tests : « absolute interior link crosses guest root », « dotdot is evaluated after following an interior link », « forty-one interior links return ELOOP ». |
| Bas de l'espace invité rendu à l'invité | `hl.c` (60), `shim.S` (22), `stack.c` (8), `vdso.c` (7), `guest.c`, test `test-guard-page.c` (160) | Shim EL1 et pool de tables de pages déplacés en haut du tampon (`guest.c:83-89`, `guest.c:171-176`). Commentaire du diff : les MAP_FIXED invités peuvent utiliser le bas de l'espace « as Wine does for its DOS area ». |
| Pont ptrace x86, signaux, threads, fork | `proc_ptrace.c/h` (nouveau, 922), `fork_ipc.c` (457), `syscall_signal.c` (444), `thread.c` (392), `fault_dump.c/h`, `signal_notify.h` | Hors périmètre ici (`XP/STATUS.md`, `hl-reuse-process.md`). |
| Petits correctifs d'ABI | `stack.c` (UID/GID réels dans l'auxv), `syscall_fs.c` (types de verrous `fcntl` convertis vers les valeurs macOS) | Lu dans `git diff`. |

### 0.3 Banc réutilisé

MESURÉ. La tentative précédente a copié HL dans `EXP/scratch-hl/hyper-linux-copy` (manifeste sha256 identique avant et après, `EXP/evidence/hl-source-manifest-before.sha256` et `-after.sha256`). Le script `EXP/tools/build-hl-scratch.sh` compile les 39 sources C avec les drapeaux du Makefile amont : `EXP/logs/hl-compile.log` est vide (aucun avertissement, aucune erreur), 39 objets, `_build/hl` lié (488 944 octets, non signé, non lancé). Je n'ai créé aucune VM HVF et je n'ai lancé aucun invité.

Les huit tests purs hôte de `EXP/tools/build-run-host-tests.sh` ont été relancés pendant cette session depuis les binaires du scratch, sortie envoyée dans le répertoire temporaire de la session : `test-fd-object-lite`, `test-vfs-unit`, `test-app-open`, `test-oss-abi`, `test-fd-digest`, `test-aot-format`, `test-thread-vcpu-lifetime`, `test-page-table-pool`, tous code 0 et ligne finale `PASS`. Les journaux d'origine sont dans `EXP/logs/host-test-*.log`, leur contenu concorde. Le test `host-build-test-thread-vcpu-lifetime-asis.log` échoue à l'édition de liens (`_proc_trace_tracer_exit` non défini) quand on le construit comme le Makefile amont : la copie de travail n'est pas cohérente avec son propre test hôte, contournée dans le scratch par `scratch-hl/stub-trace.c`.

## 1. Architecture du coeur telle qu'implémentée

### 1.1 Mémoire invitée

LU. Un `mmap(MAP_ANON)` unique de 64 Gio (IPA 36 bits) ou 1 Tio (IPA 40 bits) sert de mémoire physique invitée, mappé d'un bloc en RWX dans le stage 2 (`guest.c:293-439`, `hv_vm_map` en 392-393). `GUEST_IPA_BASE` vaut 0 (`guest.h:95`) : adresse invitée = adresse physique invitée = décalage dans le tampon. Pour le fork COW, le tampon est remplacé par un fichier `mkstemp` mappé `MAP_SHARED|MAP_FIXED` (`guest.c:361-380`) et l'enfant le mappe `MAP_PRIVATE` (`guest.c:441-501`).

Le MMU invité est celui du vCPU : tables de pages de niveau 1 écrites par l'hôte dans un pool de 16 Mio à la fin du tampon (`guest.h:36`, `guest.c:171-176`), descripteurs de bloc de 2 Mio par défaut (`guest_build_page_tables`, `guest.c:1588-1689`), éclatés en 512 descripteurs L3 de 4 Kio quand les droits diffèrent (`split_l2_block`, `guest.c:2007`, `guest_update_perms`, `guest.c:2314`, `guest_map_pages`, `guest.c:2071`). HVF impose W^X, d'où deux arènes : RX à 0x10000000 et RW à 0x200000000 (`guest.h:41-61`, `syscall.c:997-1048`). Le shim EL1 et les pages de pile EL1 sont 32 Mio sous la fin du tampon (`guest.c:83-89`).

4 Kio invités sur 16 Kio hôte. `PAGE_SIZE` est redéfini à 4096 dans `guest.c:65-66` et l'auxv annonce `AT_PAGESZ` = 4096 (`stack.c:257`). Les droits que l'invité voit sont ceux des PTE de 4 Kio ; le noyau hôte ne voit qu'un seul gros mapping RWX. La taille de page hôte n'intervient que pour les mémoires externes branchées par `hv_vm_map` : l'IPA et la taille doivent être multiples de 16 Kio (`syscall_filemap.c:220-224`).

Traduction d'un pointeur invité (`gva_resolve`, `guest.c:821-878`), dans l'ordre : résolveurs externes (SysV SHM et mappings de fichiers, 8 au plus, `guest.c:795-819`) ; chemin rapide `host_base + gva` ; fenêtre du tampon noyau de Rosetta (`KBUF_VA_BASE`, `guest.h:76-88`) ; rejet de l'espace non canonique ; marche des tables de pages (`guest_walk_pt`, `guest.c:748-787`) ; segments de débordement (`guest_overflow_resolve`, `guest.c:590-597`). `guest_read`, `guest_write` et `guest_read_str` découpent la copie en segments de page (`guest.c:888-948`). MESURÉ (`grep -o`, appels de fonction seulement) : 269 appels à `guest_ptr`, `guest_ptr_avail`, `guest_read`, `guest_write`, `guest_read_str` ou `guest_gpa_ptr` dans 20 fichiers hors `guest.c` ; 57 dans `syscall_fs.c`, 40 dans `syscall_net.c`, 26 dans `syscall_io.c`, 25 dans `audio_oss.c`, 18 dans `syscall_proc.c`, 17 dans `syscall.c`. Les handlers n'accèdent à la mémoire invitée que par ces fonctions, ce qui rend possible un remplacement par des copies protégées sans toucher aux sites d'appel. Les références directes à `host_base` ou `gva_resolve` (lignes de grep) sont dans le code de chargement et de mémoire : `hl.c` 9, `syscall.c` 8 (mmap, brk, munmap), `syscall_exec.c` 6, `vdso.c` 5, et 3 chacun dans `fork_ipc.c`, `rosetta.c`, `syscall_filemap.c`, `syscall_shm.c`. Aucun handler de fichiers, de réseau ou d'I/O n'en contient.

Zéro et libération. Un `mmap` anonyme fait un `memset` du tampon (`syscall.c:1103-1105`, `syscall.c:972-973`) parce que `munmap` ne rend rien à l'hôte : il met à zéro les régions non PROT_NONE, retire la région et laisse les PTE du bas de l'espace valides (`syscall.c:1385-1515`, commentaire de l'auteur `syscall.c:1458-1470`). Un accès après `munmap` lit donc des zéros au lieu de lever SIGSEGV dans le bas de l'espace. `mprotect(PROT_NONE)` invalide les PTE (`guest_invalidate_ptes`, `syscall.c:1593-1596`).

Fichiers. Un `mmap` de fichier `MAP_PRIVATE` est un `pread` dans le tampon (`syscall.c:1112-1117`, `syscall.c:974-982`). Seul le `MAP_SHARED` de fichier fait un vrai `mmap` hôte, branché par `hv_vm_map` dans une fenêtre d'IPA entre `mmap_limit + 2 Gio` et `interp_base`, puis sélectionné page par page par des PTE (`syscall_filemap.c:210-298`). Un `MAP_SHARED|MAP_ANONYMOUS` est traité comme privé (`syscall.c:837`, `syscall.c:1120-1121`).

### 1.2 VMA et suivi côté hôte

Trois structures indépendantes dans `guest_t` (`guest.h:259-264`) :

1. `regions[]` (`guest_region_t`, 4096 entrées triées, 112 octets). Allocateur de blocs de support en espace physique : `find_free_gap` (`syscall.c:519-570`), fusion des voisines anonymes (`regions_mergeable`, `guest.c:1247`). Alimente aussi `VmSize` de `/proc/self/status` et le zéro-fill de `guest_reset`. Propre au modèle physique.
2. `va_ranges[]` (4096 entrées). Réservations exactes de VA hautes, créées pour les mappings de Rosetta à 128 Tio (`guest_track_high_mapping`, `guest.c:130-146`).
3. `vmas` (`guest_vma_t`, tableau dynamique, 16 384 entrées au plus). Mappings exacts visibles par l'invité : début, fin, offset, inode, dev, droits, drapeaux, chemin de 4096 octets (`guest.h:203-211`, 4 144 octets par entrée). Opérations `guest_vma_replace`, `guest_vma_remove`, `guest_vma_protect` avec découpe, et `vma_merge` des voisines compatibles (`guest.c:1107-1205`). Chaque `mmap` réussi appelle `record_mmap_vma` (`syscall.c:1163-1179`), `munmap` appelle `guest_vma_remove` (`syscall.c:1511`), `mprotect` appelle `guest_vma_protect` (`syscall.c:1635`).

Autres tables : `va_aliases[16]`, `overflow[4]` de 1 Gio, `preannounced[16]`, `pt_free_list[256]`, 256 supports et 2 048 vues de fichiers dans `syscall_filemap.h:7-8`. Ces plafonds fixes viennent du modèle physique.

### 1.3 vfs et racine de l'invité

LU, `vfs.c` (751 lignes) et `vfs.h` :

- Table de montages de 64 entrées `guest_prefix` vers `host_path`, drapeaux RO, NOEXEC, NOSUID, VIRTUAL (`vfs.h:17-39`). Le préfixe le plus long gagne (`find_mount`, `vfs.c:264-282`). Deux modes : `legacy` (aucune réécriture) et `rooted` (`vfs.h:21-24`).
- CWD virtuel, jamais de `chdir` hôte en mode rooted (`vfs.c:646-670`). Carte inverse hôte vers invité (`hl_vfs_host_to_guest`, `vfs.c:610-644`). Racine et ancêtres de montages synthétiques (`vfs.c:293-302`, `vfs.c:362-415`).
- Le résolveur avance composant par composant. Un lien symbolique intérieur est résolu dans l'espace de noms invité (un lien absolu repart de `/` invité), avec 40 liens au plus et ELOOP (`vfs.c:511-608`). Les noms trop longs donnent ENAMETOOLONG au lieu d'être tronqués (`vfs.c:284-291`). Le confinement canonicalise le parent avec `realpath` (`vfs.c:315-353`) et `sys_openat` ouvre ensuite sous la racine du montage avec `O_RESOLVE_BENEATH` (`syscall_fs.c:485-594`, drapeau défini dans le SDK macOS, `sys/fcntl.h:128`).
- La sysroot est un champ séparé (`vfs.h:47`) avec un repli par nom de base pour les chemins Nix (`resolve_sysroot_path`, `syscall_fs.c:107-134`). Le lanceur de l'expérience monte la racine Debian en lecture seule sur `/`, puis `prefix`, `tmp`, `home`, un répertoire hôte `proc` sur `/proc` et `/run/user/UID` (`XP/run-linux.py`).
- `/dev` et `/proc` : `sys_openat` essaie d'abord le registre de périphériques, la réouverture procfd et `proc_intercept_open` sur la chaîne du chemin, avant toute résolution (`syscall_fs.c:451-473`). `hl_vfs_apply_app_profile` déclare en plus `/dev` et `/proc` comme montages VIRTUAL (`vfs.c:732-733`).
- Aucun appel HVF, aucun pointeur invité. Seule dépendance : `syscall.h` pour les constantes `LINUX_*` et `fd_table` (`vfs.c:438-446`), et `syscall.h` inclut `Hypervisor.h` (`syscall.h:9`), dépendance d'en-tête.
- Non vérifié : APFS est insensible à la casse par défaut et le résolveur ne replie pas la casse. Wine sait faire sa propre recherche insensible à la casse, mais la sysroot est sur le volume de l'utilisateur ; à mesurer.

### 1.4 Objets fd

LU. Trois niveaux :

- `fd_table[1024]` (`syscall.h:601`, `syscall.c:82`), entrée `{type, host_fd, linux_flags, dir, of, desc}` (`syscall.h:623-630`). Allocation du plus petit numéro libre par bitmap (`syscall.c:88-96`, `syscall.c:149-171`). Les fichiers procfd sont alloués à partir de 128 (`syscall_fs.c:466`), ce qui s'écarte de la règle Linux du plus petit descripteur libre.
- `hl_descriptor_t` par numéro de descripteur : compteur, `host_fd`, drapeaux CLOEXEC, marque `removed` (`fd_object.h:111-119`).
- `hl_open_file_t` par description ouverte : compteur, genre, table d'opérations, `status_flags` atomique, état, `mount_id`, `guest_path_hint` (`fd_object.h:98-107`). Table `hl_fd_ops_t` : `read, write, readv, writev, ioctl, lseek, fstat, fcntl_getfl/setfl, poll_host_fd, destroy, fork_export, fork_import` (`fd_object.h:72-96`). `hl_fd_get` épingle la description pour la durée d'un appel (`fd_object.h:121-128`). `dup`, `dup3` et `F_DUPFD` partagent la description (`fd_object.c:339-440`).

Genres : stdio, fichier, répertoire, pipe, socket, epoll, timerfd, eventfd, signalfd, inotify, répertoire virtuel, OSS dsp et mixer, périphérique (`syscall.h:603-614`). Pipes et sockets sont des descripteurs hôte. Émulations : eventfd par pipe et compteur, timerfd par `kqueue` EVFILT_TIMER, signalfd par lectures synthétiques (`syscall_fd.c:1-8`), epoll par `kqueue` (`syscall_poll.c:1-8`), inotify par `kqueue` EVFILT_VNODE (`syscall_inotify.c:1-20`). Leur état est indexé par le numéro invité, d'où `fd_special_subsystem_close` pour chaque chemin qui libère un numéro (`syscall_fs.c:649-657`).

memfd : `mkstemp`, `unlink`, `fd_alloc(FD_REGULAR)`, seul `MFD_CLOEXEC` est lu, le nom est ignoré (`syscall.c:2385-2398`). Pas de scellement : `grep -i seal` ne trouve rien dans `src/`. Réouverture procfd : `syscall_reopen.c` crée une description indépendante dont le curseur vit dans un fichier d'état partagé et verrouillé (`reopen_shared_t`, `syscall_reopen.c:20-30`), exportable au fork par `hl_reopen_export_state_fd` (`syscall_reopen.h:16-20`).

Verrous : `fd_lock` de rang 3 après `mmap_lock` et `pt_lock` (`syscall_internal.h:10-38`).

### 1.5 Identité de processus

LU.

- Le processus racine a le PID 1 et le PPID 0 (`proc_init`, `syscall_proc.c:355-361`). Le TID du thread principal est le PID. `gettid` renvoie `current_thread->guest_tid` (`syscall.c:1362-1364`), `getpgid` renvoie le PID (`syscall.c:1823-1825`), `getuid` et `getgid` renvoient ceux de l'hôte (`syscall.c:1665-1676`).
- PID et TID viennent du même compteur. Il vit dans une page `MAP_SHARED` d'un fichier `mkstemp` supprimé, initialisé à 2, incrémenté par `compare_exchange`, plafond `GUEST_PID_MAX` = 0x3fffffff (sous le bit propriétaire de futex), sans rebouclage (`syscall_proc.c:76-113`, `syscall_proc.c:516-529`). Le descripteur est exporté vers l'enfant au fork (`fork_ipc.c:1847`) et importé avant tout thread invité (`fork_ipc.c:539`, `syscall_proc.c:538-562`). Les trois formes de clone appellent `proc_alloc_pid` (`fork_ipc.c:1295`, `1573`, `2614`).
- Un enfant de fork reçoit `proc_set_identity(child_pid, parent_pid)` (`fork_ipc.c:530`, `1219`). Le PID ne change pas à l'exec ; l'allocateur et le registre survivent (`proc_names.h:15-17`).
- Le registre `proc_names` est un fichier partagé avec verrous POSIX, en-tête versionné 4, un enregistrement par tâche `{tid, tgid, ppid, comm[16]}`, un instantané d'exec (chemin absolu et octets de `cmdline`), la relation de traçage et l'état zombie (`proc_names.h:8-60`, `proc_names.c:15-60`). Il sert `/proc/<tid>/{comm,status,cmdline,exe}` d'un autre processus (`proc_emulation.c:84-183`, `402-446`).
- Correspondance PID invité et PID hôte : `proc_table[64]` `{host_pid, guest_pid, exited, exit_status}` (`syscall_proc.h:99-106`), donc 64 enfants vivants au plus par processus.
- `prctl(PR_SET_NAME)` écrit dans le registre (`syscall.c:1834-1848`). `uname` annonce `Linux`, nœud `hl`, version `6.8.0-101-generic`, machine `aarch64` (`syscall_sys.c:28-45`).

### 1.6 Couplage mesuré par symboles non résolus

MESURÉ. `nm -u` sur les 39 objets de `EXP/scratch-hl/hyper-linux-copy/_build/obj` (compilés sans exécution, voir 0.3). Cette mesure remplace l'impression que donnent les mentions dans le texte.

- 25 objets sur 39 n'ont aucun symbole `_hv_*` non résolu : `app_open`, `audio`, `audio_coreaudio`, `audio_oss`, `device`, `elf`, `fd_object`, `futex`, `proc_emulation`, `proc_names`, `proc_ptrace`, `stack`, `syscall_fd`, `syscall_fs`, `syscall_inotify`, `syscall_io`, `syscall_net`, `syscall_poll`, `syscall_reopen`, `syscall_stats`, `syscall_sys`, `syscall_time`, `trace`, `vdso`, `vfs`.
- Les 14 autres appellent HVF : `guest` (9 fonctions), `thread` (8), `gdb_stub` (7), `syscall_proc` (7), `syscall_signal` (6), `fork_ipc` (5), `hl` (4), `syscall_exec` (4), `syscall` (3), `crash_report`, `fault_dump`, `syscall_filemap`, `syscall_shm` (2 chacun), `rosetta` (1). L'API HVF utilisée tient en 20 fonctions distinctes : `hv_vcpu_get_sys_reg`, `hv_vcpu_get_reg`, `hv_vcpu_set_sys_reg`, `hv_vcpu_set_reg`, `hv_vm_map`, `hv_vm_unmap`, `hv_vcpu_get/set_simd_fp_reg`, `hv_vcpus_exit`, `hv_vcpu_create`, `hv_vcpu_destroy`, `hv_vcpu_run`, `hv_vm_create`, `hv_vm_destroy`, `hv_vm_config_*` (4), `hv_vcpu_set_vtimer_mask`, `hv_vcpu_set_trap_debug_exceptions`.
- Sept objets n'ont aucun symbole non résolu de HVF, de mémoire invitée (`guest_*`), de `proc_*`, de `thread_*` ni de `signal_*` : `vfs`, `fd_object`, `device`, `proc_names`, `syscall_reopen`, `trace`, `syscall_stats`. Ce sont les plus sûrs à reprendre. `syscall_time` reste classé « reprise » : il demande seulement deux accès à la mémoire invitée et `signal_get_itimer` avec `signal_set_itimer`.
- Les objets de la classe « adapter » (`syscall_fs`, `syscall_io`, `syscall_fd`, `syscall_sys`, `syscall_time`, `proc_emulation`, `stack`, `syscall_net`, `syscall_poll`, `syscall_inotify`) n'ont besoin, côté mémoire invitée, que de cinq fonctions : `guest_write` (10 objets), `guest_read` (9), `guest_read_str` (2), `guest_ptr_avail` (2), `guest_ptr` (2). C'est l'interface minimale du shim en processus.
- Leurs dépendances vers la couche processus sont courtes. `syscall_fs` : `proc_get_sysroot`, `proc_set_sysroot` et les cinq entrées de `proc_emulation`. `syscall_io` : `proc_get_hl_path`, `proc_get_pid`, `signal_pending`, `signal_queue`, `thread_host_io_begin`, `thread_host_io_end`. `proc_emulation` : `current_thread`, `proc_get_cmdline`, `proc_get_elf_path`, `proc_get_pid`, `proc_get_ppid`, `proc_names_*`, `thread_active_count`. `syscall_fd` : `proc_get_pid`, `signal_consume`, `signal_get_state`.
- Le reste de `guest.c` est utilisé par 56 symboles au total hors de lui : 5 d'accès aux données, puis les régions (`guest_region_add` dans 6 objets, `guest_region_remove` 3, `guest_region_set_prot`), les PTE (`guest_invalidate_ptes` 4, `guest_update_perms` 3, `guest_map_pages` 2, `guest_extend_page_tables`, `guest_clear_pages`), les mappings hauts (`guest_map_va_range` 2, `guest_track_high_mapping`), le shim (`guest_shim_base` 2, `guest_shim_data_base` 3), `kbuf` (4), la VMA (`guest_vma_reserve` 2, `guest_vma_replace`, `guest_vma_remove`, `guest_vma_protect`) et les points d'entrée `guest_init`, `guest_init_from_shm`, `guest_destroy`, `guest_reset`, `guest_get_used_regions`. Parmi ces groupes, seule la VMA est à conserver ; tout le reste est le modèle de mémoire physique.

### 1.7 Témoin en processus : les handlers de HL tournent sans HVF (MESURÉ)

Pour vérifier la thèse « cinq fonctions suffisent » par exécution et non par lecture, j'ai lié les objets réels du scratch (tous sauf `hl.o` et `guest.o`) avec un shim de quinze lignes qui remplace `guest_ptr`, `guest_ptr_avail`, `guest_read`, `guest_write` et `guest_read_str` par l'identité et `memcpy` (annexe C). Les symboles HVF non résolus ne sont jamais appelés (`-Wl,-undefined,dynamic_lookup`). Trois pilotes C, compilés dans le répertoire temporaire de la session, appellent directement les fonctions `sys_*` avec de vrais pointeurs de l'hôte comme « adresses invitées » :

- Pilote 1, 12 contrôles passés : montage du répertoire temporaire sur `/data` en mode rooted, `sys_openat(O_CREAT|O_RDWR)`, `sys_write`, `sys_lseek`, `sys_read`, `sys_fstat` (structure `linux_stat_t` de 128 octets), `sys_close`, `sys_getdents64`, lien symbolique hôte à cible absolue `/data/hello.txt` résolu dans la racine invitée, `ENOENT` renvoyé sous forme `-2`.
- Pilote 2, 5 contrôles passés : les vraies fonctions `guest_vma_replace`, `guest_vma_protect` et `guest_vma_remove` (extraites de `guest.c:1107-1205` et compilées seules), puis `sys_openat("/proc/self/maps")` et `sys_read`. Le rendu montre les découpes exactes à 4 Kio et le chemin hôte reconverti par le bind :
  `7f0000001000-7f0000002000 r-xp 00000000 00:00 0   /usr/lib/x86_64-linux-gnu/libc.so.6` puis `7f0000002000-7f0000003000 r--p 00001000 …`.
- Pilote 3, 6 contrôles passés : le chemin de l'amorceur EAC. Écriture dans un fichier, `unlink`, puis `sys_openat("/proc/self/fd/A")` : la description rouverte lit le contenu depuis l'offset 0 pendant que le descripteur d'origine garde son curseur en fin de fichier, et une seconde réouverture en `O_RDWR` a son propre curseur à 0.

Contraintes découvertes pendant ce témoin : `sys_read` et `sys_write` exigent un `current_thread` enregistré (`thread_host_io_begin` renvoie EAGAIN sans lui, `thread.c:100-106`) ; le pilote 1 a d'abord renvoyé `-11` pour cette raison. C'est une dépendance vers la couche processus : l'interruption des E/S bloquantes repose sur un signal privé par thread. Limites : l'ABI testée est ARM64 (`linux_stat_t` de 128 octets), les trois pilotes ne couvrent ni `mmap`, ni le dispatch, ni les sockets, et aucun code invité ne s'exécute.

## 2. Table par fichier

LOC : `wc -l` sur la copie de travail. Colonne couplage : symboles concrets. Verdict : reprise (telle quelle ou presque), adapter, réécrire, abandonner.

| Fichier | LOC | Sous-système | Couplage HVF, EL1, Rosetta, mémoire physique | ABI invitée supposée | Verdict |
| --- | --- | --- | --- | --- | --- |
| `guest.c` | 2402 | mémoire invitée, stage 1, régions, VMA | `hv_vm_create`, `hv_vm_config_*`, `hv_vm_map`, `hv_vm_unmap`, `hv_vm_destroy`, `hv_vcpu_destroy`, `guest_walk_pt`, `ttbr0`, `ttbr1`, `PT_*`, `MEM_PERM_EL1`, `need_tlbi`, `kbuf_*`, `va_aliases`, `overflow`, `is_rosetta` | neutre pour la VMA (l.1107-1235), ARM64 stage 1 ailleurs | réécrire ; extraire 130 lignes de VMA (l.1107-1205) |
| `guest.h` | 516 | structures mémoire | `Hypervisor/Hypervisor.h`, `hv_vcpu_t`, `hv_vcpu_exit_t` dans `guest_t`, constantes `ROSETTA_*`, `KBUF_*` | `guest_vma_t` neutre | réécrire ; garder `guest_vma_t` après réduction de `name[4096]` |
| `syscall.c` | 2512 | table fd, errno, drapeaux, mmap et brk, dispatch | 14 `hv_*`, `hv_vcpu_get_reg(HV_REG_X0-X8)` (l.1249-1255), `hv_vcpu_set_reg(X8)` pour le TLBI (l.2502-2507), `g->regions`, `kbuf`, `ttbr` | ARM64 générique, X8 et X0-X5 | scinder : l.70-494 adapter, l.495-1180 mémoire à réécrire, l.1181-2512 dispatch à reclasser en x86-64 |
| `syscall.h` | 645 | numéros, constantes, structures `linux_*` | `Hypervisor.h`, `hv_vcpu_t` dans `syscall_dispatch` | ARM64 : numéros l.17-200, `O_*` l.352-356, `linux_stat_t` l.402 | adapter (numéros et `stat`) |
| `syscall_internal.h` | 132 | ordre des verrous, externes | aucun | neutre | reprise |
| `syscall_fs.c` | 2397 | stat, open, close, répertoires, fcntl, xattr, dirfd | 58 appels `guest_*`, `F_GETPATH`, `O_RESOLVE_BENEATH`, `fdopendir` | ARM64 : `linux_stat_t` (l.59-77), `O_*` | adapter |
| `syscall_fs.h` | 108 | déclarations | aucun | neutre | reprise |
| `syscall_io.c` | 1836 | read, write, ioctl tty, splice, sendfile | 29 `guest_*` ; l.638-1224 : serveur `rosettad`, cache AOT, `posix_spawn` | ARM64 (termios et ioctl identiques à x86-64) | adapter 1 249 lignes, abandonner 587 |
| `syscall_io.h` | 63 | déclarations | `rosetta.h` | neutre | adapter |
| `syscall_fd.c` | 814 | eventfd, timerfd, signalfd | `kqueue`, `signal_*` | `signalfd_siginfo` | adapter |
| `syscall_fd.h` | 55 | déclarations | aucun | neutre | reprise |
| `syscall_sys.c` | 301 | uname, getrandom, sysinfo, prlimit64, rusage | `mach/mach.h`, `sysctl` | `machine` = `aarch64` (l.37) | adapter |
| `syscall_time.c` | 299 | horloges, nanosleep, itimer | `translate_clockid` | `timespec` identique | reprise |
| `syscall_stats.c` | 439 | profil des appels | aucun | numéros ARM64 | reprise, optionnel |
| `vfs.c` | 751 | montages, CWD virtuel, résolveur | `fd_table` (par `syscall.h`) | neutre | reprise |
| `vfs.h` | 124 | API vfs | aucun | neutre | reprise |
| `fd_object.c` | 492 | descriptions et descripteurs | aucun | neutre | reprise |
| `fd_object.h` | 195 | modèle fd | `guest_t` en déclaration avancée | neutre | reprise |
| `device.c` | 226 | registre /dev | `audio_oss.h` | neutre | reprise ; retirer OSS |
| `proc_emulation.c` | 1078 | /proc, /dev, /etc synthétiques | `guest_t` (`vmas`, `regions`), `thread_active_count`, `sysctl`, cpuinfo ARM (l.501-518) | cpuinfo ARM, reste neutre | adapter |
| `proc_names.c` | 479 | registre de tâches partagé | verrous POSIX | neutre | reprise |
| `syscall_reopen.c` | 324 | description indépendante de procfd | `mmap` d'un fichier d'état | neutre | reprise |
| `syscall_filemap.c` | 426 | `MAP_SHARED` de fichier | `hv_vm_map`, `hv_vm_unmap`, `guest_map_pages`, `guest_set_extra_resolve`, `g->host_base`, `vm_page_size` | 4 Kio sur 16 Kio | réécrire (environ 250 lignes) |
| `syscall_shm.c` | 915 | SysV SHM | `hv_vm_map`, `guest_map_va_range`, IPA volée dans le stage 2 | idem | réécrire (environ 300 lignes) |
| `syscall_shm.h`, `syscall_filemap.h`, `syscall_reopen.h`, `proc_emulation.h`, `proc_names.h` | 50, 40, 37, 35, 60 | déclarations | `guest.h` | neutre | suivre le fichier source |
| `elf.c` | 331 | chargeur ELF | `elf_map_segments(guest_base, guest_size, load_base)` copie par `fread` dans le tampon | x86-64 et ARM64 | réécrire ; utiliser le chargeur de FEX |
| `elf.h` | 145 | structures ELF | aucun | neutre | reprise |
| `stack.c` | 296 | pile initiale et auxv | `guest_write` ; `AT_PLATFORM` `aarch64` (l.173-176), HWCAP ARM (l.56-130), `AT_PAGESZ` 4096 (l.257) | ARM64 | adapter ou utiliser le chargeur de FEX |
| `stack.h` | 53 | API pile | aucun | neutre | reprise |
| `vdso.c` | 507 | vDSO ARM64 et page [vvar] | `host_base`, pages fixes 0xE000 et 0xF000, publication toutes les 1 ms | ARM64 | abandonner ; `VDSO_Emulation.cpp` de FEX |
| `vdso.h`, `vdso_guest.S` | 69, 110 | idem | idem | ARM64 | abandonner |
| `hl.c` | 1463 | CLI, VM, profil par défaut | 25 `hv_*`, 55 mentions Rosetta, `shim_blob` | ARM64 et x86-64 par Rosetta | abandonner |
| `shim.S` | 656 | shim EL1 | vecteurs d'exception, HVC, MMU, DC ZVA | ARM64 EL1 | abandonner |
| `hv_util.h` | 121 | macros HVF | `HV_CHECK`, `SCTLR_*` | ARM64 EL1 | abandonner |
| `rosetta.c`, `rosetta.h`, `aot_format.h`, `fd_digest.h` | 301, 136, 49, 50 | Rosetta et cache AOT | `ROSETTA_PATH`, `guest_add_va_alias` | ARM64 | abandonner |
| `gdb_stub.c`, `gdb_stub.h` | 1732, 78 | stub GDB | `hv_vcpu_get_reg` | ARM64 | abandonner |
| `crash_report.c`, `crash_report.h`, `fault_dump.c`, `fault_dump.h` | 195, 37, 162, 8 | diagnostics | `hv_vcpu_get_reg` | ARM64 | abandonner ; refaire en 100 lignes |
| `trace.c`, `trace.h` | 209, 68 | traces par catégorie | aucun | neutre | reprise |
| `audio.c`, `audio.h`, `audio_oss.c`, `audio_oss.h`, `audio_coreaudio.c`, `linux_oss_abi.h`, `app_open.c`, `app_open.h` | 639, 129, 701, 27, 255, 150, 60, 27 | OSS, Core Audio, ouverture d'application | Core Audio, `guest_*` (25 appels dans `audio_oss.c`) | OSS | abandonner pour Wine et EAC |
| `thread.h` (détail, car lu pour cette étude) | 226 | table des threads invités | `hv_vcpu_t vcpu`, `hv_vcpu_exit_t *vexit`, `sp_el1`, `linux_user_pt_regs_t`, `linux_user_fpsimd_state_t`, `gdb_reg_snapshot[788]`, `_Thread_local current_thread` | registres ARM64 | réécrire ; ne garder que `guest_tid`, `clear_child_tid`, masque de signaux et pile alternative ; `proc_emulation.c` lit `current_thread` et `thread_active_count`, `syscall_io.c` exige `thread_host_io_begin` (voir 1.7) |
| Couche processus : `thread.*`, `futex.*`, `syscall_proc.*`, `syscall_signal.*`, `syscall_exec.*`, `fork_ipc.*`, `proc_ptrace.*`, `signal_notify.h`, `syscall_net.*`, `syscall_poll.*`, `syscall_inotify.*` | 13 940 au total | threads, fork, exec, signaux, ptrace, réseau, polling | 72 `hv_*` dans `fork_ipc.c`, 87 dans `syscall_proc.c`, 34 dans `syscall_signal.c`, 30 dans `thread.c` | ARM64 | voir `hl-reuse-process.md` ; seul l'allocateur de PID (`syscall_proc.c:76-113`, `516-562`, environ 90 lignes) est repris ici |

Totaux par classe, MESURÉS par somme de `wc -l` : reprise 3 835 lignes (`vfs`, `fd_object`, `syscall_reopen`, `proc_names`, `device`, `trace`, `syscall_stats`, `syscall_time` et leurs en-têtes) ; adapter 9 082 (`syscall_fs`, `syscall_io` hors `rosettad`, `syscall_fd`, `syscall_sys`, `proc_emulation`, `syscall.c` hors mémoire, `syscall.h`, `syscall_internal.h`, `stack`) ; réécrire 6 197 (`guest`, mémoire de `syscall.c`, `syscall_filemap`, `syscall_shm`, `elf`, `vdso`) ; abandonner 7 563 ; processus 13 940. Somme 40 617.

## 3. Logique VMA indépendante de HVF, réutilisable avec adresse invitée = adresse hôte sur 16 Kio

### 3.1 Ce qui se reprend

LU, sans appel Mach ni HVF :

- `guest_vma_t` et ses opérations : `guest_vma_replace` (retrait des chevauchements, insertion triée, fusion), `guest_vma_remove` (découpe en deux), `guest_vma_protect` (découpe aux bornes), règles de fusion de `vma_merge` (mêmes droits, drapeaux, inode, dev, nom, offset contigu pour un fichier), ajustement d'offset à la découpe de `vma_trim_start` (`guest.c:1107-1205`). Les règles sont exactes à 4 Kio : `& ~4095` partout.
- `record_mmap_vma` (`syscall.c:1163-1179`) : nom par `fcntl(F_GETPATH)`, inode et dev par `fstat`, dev découpé en majeur et mineur.
- `proc_maps_fd` (`proc_emulation.c:222-288`) : tri, colonne de nom à 73, chemin invité par `hl_vfs_host_to_guest`, échappement du retour ligne. Il ne lit que `nvmas`, `vmas`, `preannounced` et `verbose` de `guest_t`.
- Le test `test-page-table-pool.c` contient des oracles utiles pour les découpes (« exact shared/private pages, protection restore, replacement and clear »).

Défauts de structure à ne pas reprendre. Chaque entrée porte 4 096 octets de chemin (4 144 octets), le tableau monte à 16 384 entrées soit environ 65 Mio, et chaque insertion ou retrait fait un `memmove` de tout le suffixe (`guest.c:1127-1131`). L'ouverture de `/proc/self/maps` copie tout le tableau avant de le trier (`proc_emulation.c:224-228`). Il faut un arbre ordonné et un chemin interné.

FEX a déjà un suivi à 4 Kio : `VMATracking` (`FEX/SyscallsVMATracking.h:102`, 594 + 171 lignes), une `std::map` indexée par adresse, avec droits, partage, offset et ressource `MRID` (dev, inode) ; il sert au code automodifiable et au cache de code. Deux écarts avec HL. Le nom de fichier n'est gardé que pour un fichier régulier exécutable (`FEX/SyscallsSMCTracking.cpp:595-609`, test `S_IXUSR`), alors que `/proc/self/maps` doit nommer toute DLL ou bibliothèque, y compris les DLL de Wine non exécutables. Les mappings anonymes partagés ont un identifiant, pas de nom (`SyscallsSMCTracking.cpp:699`). Recommandation : un seul suivi, celui de FEX étendu d'un chemin interné et du dev/inode complet, avec le rendu de `/proc/self/maps` repris de HL. Garder deux suivis en parallèle crée des divergences au premier `MAP_FIXED` sur un mapping existant.

### 3.2 Ce qui doit changer sur un hôte à 16 Kio avec GVA = HVA

MESURÉ (annexe A, M5 Max, pages de 16 Kio) :

| Opération hôte | Résultat |
| --- | --- |
| `mmap(MAP_FIXED)` à `base+0x1000` (longueur 0x1000) et à `base+0x2000` (longueur 0x4000) | échec EINVAL dans les deux cas |
| `mmap(MAP_FIXED)` à `base+0x4000`, longueur 0x1000 | réussit et remplace 16 Kio : les octets de `+0x4000` à `+0x7fff` sont mis à zéro |
| `mprotect(base+0x9000, 0x1000)` | EINVAL |
| `mprotect(base+0x8000, 0x1000, PROT_READ)` | réussit et s'applique à 16 Kio (région de 0x4000 octets, droits 1) |
| `munmap(base+0x1000, 0x1000)` | EINVAL |
| `munmap(base+0x10000, 0x1000)` | réussit et retire 16 Kio |
| `mmap` de fichier, offset 0x1000, `MAP_PRIVATE` ou `MAP_SHARED` | EINVAL |
| `mmap` de fichier, offset 0x4000, longueur 0x4000, `MAP_SHARED` | réussit |
| `mach_vm_protect(a, 0x1000)` avec `a` aligné sur 16 Kio | réussit et protège 16 Kio (région de 0x4000 octets). `mach_vm_protect(a+0x1000, 0x1000)` réussit aussi, l'adresse est arrondie, son effet exact n'a pas été isolé |

Conséquences, point par point.

- Granularité des droits. L'hôte ne sait pas protéger 4 Kio. La table VMA reste la vérité de ce que l'invité attend (codes d'erreur, `/proc/self/maps`) ; les droits hôte sont une enveloppe sur 16 Kio. Une page de garde de 4 Kio dans une région lisible et inscriptible ne peut pas faire de faute. Trois voies, à trancher : annoncer 16 Kio à l'invité (`AT_PAGESZ`, `sysconf`) et faire tourner Wine en pages de 16 Kio (les sections PE sont alignées sur 4 Kio, Wine doit le gérer) ; émuler les droits de 4 Kio par fautes et table fantôme (coût de chaque faute) ; garder l'étage 1 d'HL, ce qui est exclu ici. Aucune n'a été testée. FEX suppose 4 096 (`FEXCore/include/FEXCore/Utils/TypeDefines.h:6-7`).
- Blocs de support. `regions[]`, `va_ranges[]`, le débordement, les alias et `kbuf` disparaissent. `find_free_gap` est remplacé par le choix d'adresse de l'hôte (`mmap(NULL, …)` dans la fenêtre réservée) ou par une recherche dans la table VMA. L'équipe FEXCore réserve la fenêtre invitée en `PROT_NONE` (`reports/fexcore-darwin-port.md`, décision 3).
- Mappings de fichiers à offset non aligné sur 16 Kio. HL les absorbe : copie par `pread` pour `MAP_PRIVATE`, `delta` dans une IPA alignée pour `MAP_SHARED` (`syscall_filemap.c:220-224`). En processus, un `MAP_PRIVATE` devient anonyme plus `pread` (comme `syscall.c:1112-1117`), avec le coût de mémoire résidente et la perte du partage de cache ; la VMA garde le nom et l'offset. Un `MAP_SHARED` n'est possible que si l'adresse et l'offset sont tous deux alignés sur 16 Kio et congrus ; sinon refuser (EINVAL) ou accepter une copie incohérente, à décider. Les vues de Wine suivent la granularité d'allocation de 64 Kio (INFÉRÉ de la conception de Wine, non vérifié ici).
- Mappings partagés entre processus et fork. HL n'a pas de mémoire anonyme partagée (`MAP_SHARED|MAP_ANONYMOUS` devient privé, `syscall.c:837`). Pour les fichiers, il exporte par SCM_RIGHTS les descripteurs de support et les triplets (va, longueur, offset) et réimporte dans l'enfant (`syscall_filemap.c:334-426`) : ce schéma de 90 lignes se reprend tel quel pour un fork qui n'est pas un vrai `fork()`. Avec un vrai `fork()` hôte le partage est natif, mais la sûreté de `fork()` dans un processus multithread est du ressort de l'équipe processus.
- `MAP_FIXED` sur un mapping existant. HL retire les régions, puis réécrit les PTE (`syscall.c:938-993`). En processus, `mmap(MAP_FIXED)` remplace atomiquement côté noyau, mais il faut mettre à jour la table VMA et invalider les traductions JIT sous le même verrou (modèle : `FEX/SyscallsSMCTracking.cpp:302-350`, `GuestMmap`). `MAP_FIXED_NOREPLACE` n'existe pas dans le SDK macOS (grep de `sys/mman.h`) : le test de chevauchement passe par la table VMA, comme `guest_mapping_overlaps` (`guest.c:103-119`).
- `mremap`. Absent de HL (ENOSYS par le cas par défaut, `syscall.c:2451-2462`) et du SDK macOS. FEX appelle `::mremap` puis suit (`FEX/SyscallsSMCTracking.cpp:390-412`). À émuler : réduction par `munmap` de la queue ; agrandissement sur place si la table VMA montre un trou, par `mmap(MAP_FIXED)` ; déplacement par `mach_vm_remap` à 16 Kio puis mise à jour de la table et invalidation JIT. glibc retombe sur malloc, copie, free quand `mremap` échoue (INFÉRÉ de mémoire du code de `realloc`).
- `munmap` et `mprotect`. En processus ils doivent être exacts : vrai `munmap`, vrai `PROT_NONE`. L'invité observera enfin un SIGSEGV après `munmap`, ce que HL ne produit pas dans le bas de l'espace.
- Zéro. HL fait un `memset` à chaque `mmap` anonyme ; un `mmap` hôte neuf est déjà à zéro. Mais `madvise(MADV_DONTNEED)` ne remet pas la page à zéro sur Darwin (mesure de l'équipe FEXCore, `fexcore-darwin-port.md`), alors qu'il le fait sous Linux pour du privé anonyme.
- Code automodifiable. Le suivi de FEX protège le code invité par `mprotect(PROT_READ)` en unités de 4 Kio (`SyscallsSMCTracking.cpp:136-196`, `FEX_PAGE_SIZE`). À 16 Kio il sur-invalidera, ce qui est sûr (décision 2 de `fexcore-darwin-port.md`).

## 4. Question d'ABI : ARM64 Linux générique contre x86-64 Linux

### 4.1 Comment HL représente les arguments et les structures

LU. `syscall_dispatch` lit X8 et X0-X5 dans le vCPU par `hv_vcpu_get_reg` (`syscall.c:1249-1255`), un `switch` sur le numéro ARM64 appelle des fonctions `sys_*(guest_t *g, …)` qui reçoivent des adresses invitées en `uint64_t` et les lisent par `guest_read` et `guest_write`, et le résultat retourne dans X0 (`syscall.c:2495`). Les structures sont des copies des structures noyau ARM64 : `linux_stat_t` de 128 octets (`syscall.h:402-423`), `linux_statfs_t` (`syscall.h:450-463`), `linux_epoll_event_t` de 16 octets non empaqueté (`syscall_poll.c:583-588`), `linux_sigaction_t` de 32 octets (`syscall_signal.h:80-85`). Chaque handler traduit entre la structure noyau Linux et l'API Darwin : `translate_stat` (`syscall_fs.c:59-77`) avec ré-encodage du `dev_t` (`syscall_fs.c:52-56`), errno (`linux_errno`, `syscall.c:330-414`), `AT_*` (`syscall.c:421-444`), `O_*` (`syscall.c:453-473`), identifiants de clocks, `RLIMIT_*` (`syscall_sys.c:228-242`), `sockaddr` avec `sa_len` BSD, `cmsghdr` (16 contre 12 octets d'en-tête, `SOL_SOCKET` 1 contre 0xFFFF).

### 4.2 Écarts ARM64 générique contre x86-64

| Objet | ARM64 générique (HL) | x86-64 | Ce que fait FEX | État dans HL |
| --- | --- | --- | --- | --- |
| Numéros | asm-generic, `read`=63, `openat`=56, `mmap`=222 (`syscall.h:17-200`) | `read`=0, `write`=1, `open`=2, `mmap`=9 | environ 467 entrées x86-64 (`x64/SyscallsEnum.h`) vers environ 469 entrées ARM64 (`Arm64/SyscallsEnum.h`) par `SYSCALL_DEF` (`Syscalls.h:54`, 400 usages) | à refaire |
| Registres | X8, X0-X5 | rax ; rdi, rsi, rdx, r10, r8, r9 | `CpuStateFrame`, handlers typés | à refaire |
| Appels hérités absents d'ARM64 | sans objet | `open`, `stat`, `lstat`, `access`, `pipe`, `dup2`, `poll`, `select`, `rename`, `mkdir`, `rmdir`, `link`, `unlink`, `symlink`, `readlink`, `chmod`, `chown`, `creat`, `getdents`, `utime`, `time`, `fork`, `vfork`, `arch_prctl` | appelle les fonctions glibc (`Syscalls/FS.cpp:23-48`, `x64/FD.cpp:122-128`), émulation de `getdents` par `getdents64` (`GetDentsEmulation`) | à écrire (environ 30 appels) |
| `struct stat` | 128 octets (`syscall.h:402`) | 144 octets : `st_dev`, `st_ino`, `st_nlink` (8), `st_mode`, `st_uid`, `st_gid`, `__pad0`, `st_rdev`, `st_size`, `st_blksize` (8), `st_blocks`, 3 horodatages, 3 mots libres | `guest_stat`, `static_assert` 144 (`x64/Types.h:112-215`), utilisé en `x64/FD.cpp:65-103` | nouvelle fonction de 25 lignes ; `mac_to_linux_dev` se reprend |
| `dirent64` | en-tête de 19 octets, `d_ino`, `d_off`, `d_reclen`, `d_type` (`syscall_fs.c:97-103`) | identique | `linux_dirent_64`, `static_assert` 19 (`x64/Types.h:213-222`) | reprise |
| `statfs` | `linux_statfs_t` (`syscall.h:450`) | même disposition 64 bits | passe `FM.Statfs` directement (`x64/FD.cpp:135-139`) | reprise |
| `rlimit` | 16 octets, `RLIM_INFINITY` = 2^64-1 (`syscall_sys.c:244-299`) | identique | passthrough | reprise |
| `flock` (`fcntl`) | 32 octets | identique | passthrough | reprise ; la conversion F_RDLCK 0, F_WRLCK 1, F_UNLCK 2 vers 1, 3, 2 de macOS est un correctif privé de `syscall_fs.c` |
| `sockaddr`, `msghdr`, `cmsghdr` | LP64 Linux | identique | passthrough | reprise (`syscall_net.c`, autre rapport) |
| `timespec`, `timeval`, `iovec`, `pollfd`, `sysinfo`, `rusage` | `linux_timespec_t` etc. (`syscall.h:437-545`) | identiques | passthrough | reprise |
| `epoll_event` | 16 octets, non empaqueté (`syscall_poll.c:583`) | 12 octets empaquetés | `epoll_event_x86`, `static_assert` 12 (`Types.h:56-72`) et copie par vecteur (`x64/EPoll.cpp:24-94`) | à adapter (20 lignes) |
| `O_*` | `O_DIRECTORY` 0x4000, `O_NOFOLLOW` 0x8000, `O_DIRECT` 0x10000, `O_LARGEFILE` 0x20000 (`syscall.h:352-356`) | `O_DIRECT` 040000, `O_LARGEFILE` 0100000, `O_DIRECTORY` 0200000, `O_NOFOLLOW` 0400000 | `RemapFromX86Flags` et `RemapToX86Flags` (`Syscalls.h:490-562`) | à adapter ; `translate_open_flags` (`syscall.c:453-473`) ignore `O_DIRECT`, `O_SYNC`, `O_DSYNC`, `O_NOATIME`, `O_PATH`, `O_TMPFILE` |
| `sigaction` noyau | 32 octets : handler, flags, restorer, mask | 32 octets, même ordre | `GuestSigAction` (`Types.h:97-106`) | même taille ; la structure ARM64 réelle n'a pas de `restorer` d'après mes souvenirs des en-têtes noyau, non revérifié ici (équipe signaux) |
| Trame de signal, `ucontext`, `rt_sigreturn` | trame ARM64 construite sur la pile invitée, restaurateur dans le vDSO (`vdso.h:34-38`) | trame x86-64 avec `fpstate` xsave, restaurateur fourni par la libc (`SA_RESTORER`) | `SignalDelegator` (1 352 + 856 lignes) | hors périmètre ; ne pas réutiliser |
| `clone` | ordre des arguments ARM64 | `flags, stack, parent_tid, child_tid, tls` (les deux derniers sont inversés par rapport à ARM64) | `x64/Thread.cpp` | équipe processus |
| `uname` | `machine` `aarch64` (`syscall_sys.c:37`) | `x86_64` | sans objet | une ligne |
| `mmap` | mêmes drapeaux, pas de `MAP_32BIT` | `MAP_32BIT` 0x40 | `Get32BitAllocator()` pour les adresses sous 4 Gio (`SyscallsSMCTracking.cpp:316-325`) | inutile sur Darwin, l'espace sous 4 Gio n'existe pas |
| `getrandom`, `futex`, `ppoll`, `pselect6`, `wait4`, `ioctl` (hors tty) | mêmes arguments | identiques | passthrough | reprise |

### 4.3 Comment FEX convertit les appels x86-64 vers l'ABI hôte ARM64

LU dans FEX. Chaque appel x86-64 est enregistré par `REGISTER_SYSCALL_IMPL_X64(nom, lambda)` ou `REGISTER_SYSCALL_IMPL(nom, lambda)` (les deux architectures). Quatre mécanismes :

1. Passthrough. 168 appels communs (`RegisterCommon`, `Syscalls/Passthrough.cpp:212-414`) plus 73 propres à x86-64 (`Passthrough.cpp:416-501`), soit 241 appels envoyés tels quels. Sur un hôte ARM64 Linux, un `svc #0` en assembleur en ligne avec le numéro ARM64 (`Passthrough.cpp:17-150`) ; sur les autres hôtes, `::syscall` (l.152-210). La liste inclut `read`, `write`, `ioctl`, `readv`, `writev`, `sendmsg`, `recvmsg`, `setsockopt`, `getsockopt`, `futex`, `wait4`, `ppoll`, `pselect6`, `clock_*`, `timerfd_*`, `preadv`, `pwritev`, `process_vm_readv`, `process_vm_writev`, `memfd_create`, `getrandom`, `futex_waitv`.
2. Conversion de structure ou de drapeaux, environ 100 handlers : `stat` en `guest_stat` (`x64/FD.cpp:65-103`), `epoll_*` (`x64/EPoll.cpp`), `fcntl` avec remappage de `O_*` (`x64/FD.cpp:45-55`), `rt_sigaction` vers le délégué de signaux (`x64/Signals.cpp:21-28`), `getdents` hérité, `clone`.
3. Appels hérités implémentés par les fonctions glibc (`::rename`, `::mkdir`, `::unlink`…), qui prennent le chemin invité tel quel.
4. Mémoire : `GuestMmap`, `GuestMunmap`, `GuestMremap`, `GuestMprotect` appellent `::mmap`, `::munmap`, `::mremap`, `::mprotect` de l'hôte, puis mettent à jour `VMATracking` et invalident le code JIT (`SyscallsSMCTracking.cpp:302-514`). Les chemins passent par `FileManager`, qui superpose une racine (`RootFSFD`) et émule quelques fichiers : `/proc/cpuinfo`, `/proc/version`, `/proc/sys/kernel/osrelease`, `/proc/self/auxv`, `/sys/devices/system/cpu/online`, `/proc/self/exe` (`FEX/EmulatedFiles/EmulatedFiles.cpp:522-592`, `FEX/FileManagement.h`). Tout le reste de `/proc` est celui du vrai noyau Linux, ce qui n'existe pas sur Darwin.

MESURÉ (comptage). Le répertoire `LinuxEmulation` hors `x32` fait 21 705 lignes dans 75 fichiers, `x32` 10 793. Hors `x32`, `grep` compte 137 lignes `::syscall(`, 400 usages de `SYSCALL_DEF`, 19 lignes `asm volatile` dans 5 fichiers, 39 inclusions d'en-têtes `<linux/…>` ou `<asm/…>` (le SDK macOS n'a aucun répertoire `usr/include/linux`) et 25 inclusions de 14 en-têtes `<sys/…>` absents du SDK : `auxv`, `epoll`, `eventfd`, `fsuid`, `inotify`, `klog`, `personality`, `prctl`, `sendfile`, `signalfd`, `statfs`, `swap`, `sysinfo`, `timerfd`.

MESURÉ (comparaison de noms, script jetable). Sur les 241 appels passthrough, HL en implémente 128 et n'en a pas 113. Parmi les absents : `process_vm_readv`, `process_vm_writev`, `futex_waitv`, `timer_create` et cinq autres `timer_*`, `capget`, `capset`, `getsid`, `times`, `settimeofday`, `rt_sigqueueinfo`, `sched_*` (neuf appels), `semget`, `semop`, `semtimedop`, `msgget`, `msgsnd`, `msgrcv`, `msgctl`, `mq_*`, `io_*` (sept appels), `pidfd_*`, `readahead`, `sync_file_range`, `syncfs`, `mlockall`, `mlock2`, `getcpu`, `setgroups`, `setfsuid`, `setfsgid`, `syslog`, `unshare`, `setns`, `kcmp`.

### 4.4 Comparaison des deux compositions

Composition A : `LinuxEmulation` de FEX compilé pour Darwin sur une bibliothèque « noyau ARM64 sur Darwin » dérivée des handlers de HL. Composition B : une couche x86-64 neuve écrite directement pour FEXCore.

Pour A, il faut :

- Remplacer les 7 gabarits de passthrough (`Passthrough.cpp:17-210`) et les 137 sites `::syscall` par un appel `hl_syscall(nr, a0..a5)`. Mécanique, environ 150 sites.
- Fournir une libc « Linux » : FEX appelle directement `::open`, `::stat`, `::fcntl`, `::select`, `::rename`, `::mkdir`, `::epoll_*`… avec des valeurs de drapeaux Linux (`RemapToX86Flags` suppose des `O_*` ARM64 Linux, `Syscalls.h:528-562`). Sur Darwin ces appels renverraient des valeurs Darwin. Environ 100 enveloppes, plus l'isolation des en-têtes : 64 inclusions visent des en-têtes absents du SDK macOS (39 UAPI Linux et 25 `<sys/…>`) et, INFÉRÉ, les en-têtes UAPI importés risquent de redéfinir des types que le SDK définit aussi (`struct stat`, `timespec`).
- Étendre la bibliothèque de 128 à environ 240 appels pour tenir le passthrough, ou marquer ENOSYS les appels que Wine et EAC n'utilisent pas.
- Convertir deux fois : Darwin vers ARM64 dans la bibliothèque, ARM64 vers x86-64 dans FEX (`stat`, `epoll`, drapeaux). `st_dev` aurait deux encodages à enchaîner.
- Porter ce qui reste Linux dans FEX de toute façon : création de threads par appels système bruts en assembleur (`Utils/Threads.cpp:128-173`), délégué de signaux sur `ucontext` ARM64 Linux (`ArchHelpers/MContext.h`, 409 lignes, `UContext.h`, 439), `execve` réalisé par `execveat(…, "/proc/self/exe", …)` (`Syscalls.cpp:443`), client FEXServer (`SyscallsSMCTracking.cpp:434-466`), allocateur 32 bits (`LinuxAllocator.cpp`), `GdbServer`.
- Garder deux gestionnaires de chemins en conflit : le `FileManager` à racine de FEX et le vfs à montages de HL.

Pour B, il faut :

- Une table de dispatch x86-64 sur `CpuStateFrame` : environ 150 à 200 appels, les 103 numéros ARM64 vus dans les traces Wine plus EAC (annexe B) étant la base.
- Les convertisseurs x86-64 : `stat`, `epoll_event`, drapeaux `O_*`, `getdents`, `uname`, environ 30 appels hérités.
- Les services « Linux sur Darwin » de HL tels quels : vfs, fd, procfs, noms de tâches, traductions d'errno et de drapeaux, `kqueue` pour epoll, timerfd, inotify.

Raisons de choisir B :

1. MESURÉ : 241 appels de FEX sont des passthrough et HL n'en couvre que 128. A oblige à faire de HL un noyau ARM64 presque complet, B oblige à servir ce que Wine et EAC appellent. Les traces de l'expérience précédente (annexe B) donnent 103 appels distincts, 101 présents dans le dispatch de HL.
2. La conversion se fait une fois, de Darwin vers x86-64.
3. HL canalise les accès des handlers à la mémoire invitée par cinq fonctions (269 sites hors `guest.c`). En processus, `guest_ptr(g, gva)` devient l'identité et `guest_read`/`guest_write` des copies protégées (modèle : `FEX/FaultSafeUserMemAccess.cpp`, 183 lignes). Les 269 sites ne changent pas.
4. Les parties de FEX qui valent la peine d'être importées sont couplées au CPU, pas au noyau, et s'importent dans B sans A : `SyscallsVMATracking.*` (765 lignes), `SyscallsSMCTracking.cpp` (816), `x64/Types.h` (222), `Types.h` (108), `FaultSafeUserMemAccess.cpp` (183), le générateur de `/proc/cpuinfo` (`EmulatedFiles.cpp:522-534`), `VDSO_Emulation.cpp` (906). Licences : FEX est MIT et HL Apache-2.0, toutes deux permissives. L'équipe FEXCore signale la règle de `CLAUDE.md` de FEX sur les contributions générées par IA (`fexcore-darwin-port.md`, dernière section) ; elle vise les contributions amont, pas ce banc privé.
5. L'équipe FEXCore écrit déjà un hôte `fexdarwin-run` avec son chargeur ELF, sa pile initiale et son délégué de signaux (`fexcore-darwin-port.md`, prochaines étapes).

Quantité de glue, INFÉRÉE de la table de la section 2 et des comptages ci-dessus : B demande environ 2 600 à 3 400 lignes pour la table et les convertisseurs, plus 1 800 à 2 400 lignes de retouches des handlers repris. A demande à peu près B plus 3 000 à 4 500 lignes : 2 000 à 3 000 de retouches dans FEX, 800 à 1 200 de libc Linux, 1 500 à 2 500 pour sortir HL de `guest_t`. Réserve : l'avantage de A est de suivre FEX en amont ; il est faible ici parce que le dépôt FEX est déjà forké pour Darwin.

## 5. Lacunes du coeur face aux besoins de Wine et EAC

État dans la copie de travail. Les besoins viennent des traces de l'expérience précédente (`XP/README.md`, `XP/STATUS.md`) : l'amorceur EAC écrit de 6,6 à 9,7 Mo dans un memfd selon les essais, lit la carte mémoire pour trouver `libc`, rouvre le memfd par `/proc/self/fd/N`, tente `F_ADD_SEALS`, appelle `ptrace`, `readlinkat(/proc/self/exe)`.

| Élément | État | Détail |
| --- | --- | --- |
| `/proc/self/maps` | présent | Rendu depuis la table VMA, noms `[heap]`, `[stack]`, `[sysvshm]` (`hl.c:891-903`, `syscall_shm.c:584`) ; `[vdso]` et `[vvar]` seulement en mode Rosetta (`rosetta.c:238-241`). Le chemin invité est recalculé par `hl_vfs_host_to_guest`. |
| `/proc/<pid>/maps`, autre processus | absent | Seul l'alias strict du PID courant est réécrit en `/proc/self` (`proc_emulation.c:60-68`). Cet alias a corrigé l'ouverture de `/proc/1/maps` par l'amorceur EAC quand son propre PID est 1 (`XP/README.md`, ligne 109). Aucune lecture de la carte d'un autre PID n'a été vue dans les traces citées. |
| `/proc/self/status` | partiel | Name, State, Tgid, Pid, PPid, TracerPid, Uid, Gid, VmPeak, VmSize, VmRSS, Threads (`proc_emulation.c:521-576`). VmSize et VmRSS viennent des blocs de support. Absents : VmData, VmStk, VmExe, VmLib, Seccomp, NoNewPrivs, Cap*, Sig*, Cpus_allowed. |
| `/proc/<tid>/status` d'un autre processus | partiel | Name, State (Z ou t), Tgid, Pid, PPid, TracerPid, Uid, Gid (`proc_emulation.c:133-166`). |
| `/proc/self/stat` | partiel | 52 champs, seuls pid, comm, état `R`, ppid, pgrp, session, utime, stime, threads, vsize, rss sont remplis (`proc_emulation.c:804-855`). |
| `/proc/self/task` | partiel | Ouverture de `/proc/<tgid>/task/<tid>/comm` et `status` (`proc_emulation.c:99-111`). Pas de listage de `/proc/self/task` ni de `/proc/self/task/<tid>/` : l'ouverture d'un répertoire sous un montage virtuel renvoie ENOENT (`syscall_fs.c:549`). |
| `/proc/self/fd` | partiel | Ouverture, `readlink` et `stat` de `/proc/self/fd/N` (`proc_emulation.c:293-395`, `1053-1075`, `997-1007`) ; pas de listage du répertoire. Pas de `/proc/self/fdinfo`. |
| `/proc/self/exe` | présent | `readlink` en chemin invité par `realpath` et `hl_vfs_host_to_guest` (`proc_emulation.c:1026-1031`), ouverture du binaire (`493-498`). Autres tâches : instantané d'exec. |
| `/proc/self/cmdline` | présent | Octets NUL séparés, instantané d'exec pour les autres tâches (`proc_emulation.c:579-584`). |
| `/proc/self/environ` | absent | Aucune occurrence. |
| `/proc/self/mem` | absent | Aucune occurrence. |
| `/proc/cpuinfo` | partiel | Champs ARM seulement : « CPU implementer 0x61 » (`proc_emulation.c:501-518`). Faux pour un invité x86-64 ; reprendre le générateur de FEX. |
| `/proc/meminfo` | partiel | Rapports fixes : 50 % libre, 75 % disponible (`proc_emulation.c:730-782`). |
| `/proc/self/smaps`, `pagemap`, `limits`, `auxv` | absent | Aucune occurrence dans `src/`. FEX fournit `auxv` (`FEX/EmulatedFiles/EmulatedFiles.cpp:589-592`). |
| `/proc/self/io` | partiel | Fichier de zéros (`proc_emulation.c:787-797`). |
| `/dev/null`, `/dev/zero`, `/dev/urandom`, `/dev/random`, `/dev/tty` | présent | Ouverture du nœud hôte (`device.c:47-105`, `proc_emulation.c:449-471`). |
| `/dev/stdin`, `/dev/stdout`, `/dev/stderr`, `/dev/fd/N` | présent | `proc_emulation.c:473-487`. |
| `/dev/shm` | absent | Aucun nœud ni montage. À contourner par un montage d'un répertoire hôte, à tester avec le chemin MAP_SHARED de fichier. |
| `/dev/full`, `/dev/ptmx`, `/dev/pts` | absent | Aucune occurrence. |
| `memfd_create` | partiel | Fichier temporaire supprimé, `MFD_CLOEXEC` seulement, nom ignoré (`syscall.c:2385-2398`). `readlink /proc/self/fd/N` ne donne pas `/memfd:nom (deleted)`. |
| Seals (`F_ADD_SEALS`, `F_GET_SEALS`) | absent | Aucune occurrence de « seal ». `F_ADD_SEALS(0xf)` renvoie EINVAL (`XP/README.md`, ligne 209). Sonde `XP/probes/memfd-seals` : 8 contrôles sur 29 passent sous HL, 29 sur 29 sous Linux. L'essai allow contre deny sur un vrai noyau Linux garde 509/210 dans les deux cas (`XP/linux-reference/seal-comparison/README.md`), donc le lien avec le rejet d'EAC n'est pas établi. Sur Darwin, aucun contrôle ne bloque les écritures par un mapping existant : un scellement exact est impossible, il faudra décider d'un scellement approché et le documenter. |
| `mremap` | absent | ENOSYS (`syscall.c:2451-2462`). |
| `madvise` | partiel (stub) | Renvoie 0 sans rien faire (`syscall.c:1692-1694`) ; `MADV_DONTNEED` ne vide pas la page. |
| `mincore` | absent (ENOSYS explicite) | ENOSYS (`syscall.c:2409-2411`). |
| `mlock`, `munlock`, `msync` | partiel (stub) | Renvoient 0 (`syscall.c:2399-2408`). |
| `process_vm_readv`, `process_vm_writev` | absent | ENOSYS. `XP/STATUS.md` note sept lectures `process_vm_readv` en ENOSYS dans le témoin complexe. Sur Darwin la lecture dans un autre processus passe par `mach_vm_read_overwrite` sur le port de tâche du pair ; son obtention est du ressort de l'équipe processus. |
| `userfaultfd` | absent | ENOSYS. Aucun équivalent Darwin ; je n'ai trouvé aucun usage dans les traces. |
| Descripteurs au-delà de 1 024 | absent | `FD_TABLE_SIZE` = 1024 (`syscall.h:601`). |
| Famille xattr, numéros | défaut | `SYS_fgetxattr` est 16 et `SYS_fremovexattr` 18 dans `syscall.h:166` et `169` ; les vrais numéros ARM64 sont 10 et 16. Un appel `fgetxattr` (numéro 10) tombe dans « unimplemented syscall 10 » (19, 36 et 22 fois dans les trois traces EAC) et un `fremovexattr` (16) irait au handler de `fgetxattr`. Défaut présent aussi à HEAD (`git show HEAD:src/syscall.h`). Contrôle systématique : sur les 177 `#define SYS_*` de `syscall.h`, 175 coïncident avec la table ARM64 que j'ai reconstituée de mémoire (script jetable) et ces deux seuls sont faux. |
| `O_TMPFILE`, `O_PATH`, `O_SYNC`, `O_DSYNC`, `O_NOATIME` | absent | Ignorés sans erreur par `translate_open_flags` (`syscall.c:453-473`). |

Défauts relevés en lisant le code et à ne pas reproduire : `MAP_SHARED` anonyme traité comme privé (`syscall.c:837`, `1120`) ; `munmap` ne libère rien et conserve les PTE du bas de l'espace (`syscall.c:1458-1470`) ; fichiers procfs alloués à partir du descripteur 128 (`syscall_fs.c:466`) ; table VMA de 65 Mio au pire.

## 6. Estimation en lignes et cinq risques

### 6.1 Estimation INFÉRÉE pour un hôte FEXCore

Base : sections 2 et 4. Marge : plus ou moins 35 %.

| Poste | Lignes | Origine |
| --- | --- | --- |
| Reprise sans changement notable | environ 10 500 | 3 835 de classe reprise moins 150 à 300 retouchées, plus 6 700 à 7 300 des 9 082 de classe adapter qui ne changent pas |
| Retouches de la classe adapter (numéros, `stat`, `O_*`, `epoll_event`, `machine`, retrait des types `hv_`, shim `guest_ptr`) | 1 800 à 2 400 | 20 à 25 % de 9 082 |
| Retouches de la classe reprise (séparer `syscall.h` des constantes, retirer l'audio) | 150 à 300 | `vfs.c`, `fd_object.c`, `device.c` incluent `syscall.h` qui inclut `Hypervisor.h` |
| Gestionnaire d'espace d'adressage : mmap, munmap, mprotect, MAP_FIXED, gestion 4 Kio sur 16 Kio, VMA, crochets d'invalidation JIT | 1 300 à 1 900 | remplace 2 918 lignes de `guest.*` et 686 de `syscall.c` |
| Table de dispatch x86-64, appels hérités, convertisseurs | 2 600 à 3 400 | section 4.4 |
| Mappings partagés, SysV SHM, memfd et scellement approché | 700 à 1 100 | `syscall_filemap`, `syscall_shm` réécrits (550) plus seals (250 à 500) |
| Compléments de procfs : `task`, `fd`, `environ`, `mem`, `/proc/<pid>/maps`, `smaps`, cpuinfo x86, status complet | 700 à 1 100 | section 5 |
| `mremap`, `madvise`, `mincore`, `process_vm_readv` et `writev` | 600 à 900 | section 5 |
| Total écrit ou modifié | 8 000 à 11 000 | |

### 6.2 Les cinq éléments les plus risqués

1. Granularité 4 Kio invitée sur pages hôte de 16 Kio avec adresse invitée = adresse hôte. MESURÉ : `mmap` et `mprotect` hôtes arrondissent à 16 Kio, un `MAP_FIXED` de 4 Kio détruit 12 Kio voisins, un offset de fichier à 0x1000 est refusé. HL résout cela avec son MMU invité, qui n'existe plus. Choix à faire tôt, car il conditionne Wine (taille de page annoncée, gardes de pile, `VirtualProtect`, sections PE) : annoncer 16 Kio, ou émuler par fautes. Aucune des deux n'a été testée.
2. Adresses invitées basses. MESURÉ par l'équipe FEXCore : aucun `mmap` fixe sous 4 Gio, `__PAGEZERO` ne peut pas être réduit (`reports/fexcore-darwin-port.md`, tableau des mesures). HL l'évite (guest.h:95, `GUEST_IPA_BASE` = 0) et ses correctifs privés libèrent le bas de l'espace pour la zone DOS de Wine. Un ELF x86-64 lié à 0x400000 ou un préchargeur de Wine à adresse fixe basse ne se chargent pas en processus ; seule une traduction d'adresse dans le JIT ou des invités liés plus haut sortent de l'impasse.
3. Sémantique exacte de `munmap`, `mprotect`, `mremap` et `madvise` et cohérence avec le JIT. HL triche : `munmap` ne libère rien, `madvise` et `mincore` sont des stubs. En processus l'invité observe les vraies fautes, `mremap` n'existe pas sur Darwin et `MADV_DONTNEED` ne remet pas à zéro. Toute écriture dans la table VMA doit être atomique avec l'invalidation des traductions (modèle : `GuestMmap` de FEX). Une course ici produit du code obsolète exécuté, difficile à diagnostiquer.
4. Fidélité de `/proc` et de memfd pour EAC. L'amorceur ouvre `/proc/<son pid>/maps` pour localiser `libc` (ligne `/libc-` ou `libc.so`), rouvre le memfd par `/proc/self/fd/N` avec un curseur indépendant et tente `F_ADD_SEALS` (`XP/README.md`, section « Lecture de la carte mémoire par EAC »). Les seals ne sont pas imitables à l'identique sur Darwin. Chaque écart de `/proc` est un échec silencieux possible, sans référence autre que le Linux réel de comparaison.
5. Partage de mémoire et identité entre processus sans noyau Linux. L'anonyme partagé n'existe pas dans HL, les vues de fichiers ne passent un fork que par export de descripteurs, le PID invité est découplé du PID hôte, `kill` et `wait4` passent par `proc_table[64]`, `/proc/<pid>` repose sur un fichier partagé verrouillé par POSIX. Un wineserver séparé qui lit la mémoire d'un client demande un port de tâche (équipe processus). Le risque est l'incohérence entre ces trois tables (VMA, descripteurs hôte, registre de noms) après `exec` ou crash.

## 7. Tests hôte déjà exécutés

MESURÉ, relancés pendant cette session (voir 0.3) :

- `test-fd-object-lite` : 12 lignes `OK` puis `PASS` (création, pipe, installation, `dup` partage la description, fermeture).
- `test-vfs-unit` : 15 lignes `OK` puis `PASS` (résolution relative, liens internes, ELOOP à 41 liens, bind le plus long, refus des cibles hôte absolues).
- `test-page-table-pool` : 6 lignes `OK` puis `PASS` (pool hors de l'arène, 512 blocs de découpe, réservations basses de Wine, trous exacts de fork, VA haute, droits). Il teste du code du stage 1 qui sera abandonné ; ses oracles sur les découpes de VMA restent utiles.
- `test-aot-format`, `test-app-open`, `test-fd-digest`, `test-oss-abi`, `test-thread-vcpu-lifetime` : `PASS`, sans rapport direct avec le coeur FEX.

## 8. Points non établis

- Aucun test n'a exécuté de code invité. Les verdicts de la section 2 et les estimations de la section 6 sont des jugements sur lecture de code.
- Les traces de l'annexe B s'arrêtent à l'erreur EAC 509 : le jeu d'appels d'une session complète de Rocket League est plus large (audio, réseau, GPU).
- Taille de page annoncée à l'invité, fork réel contre export de mappings, partage de ports de tâche entre processus : décisions ouvertes, pas de mesure.
- Le comportement de `fcntl(F_ADD_SEALS)` d'EAC lui-même n'a pas été enregistré sous Linux (`XP/linux-reference/seal-comparison/README.md`, « Limites et décision »).

## Annexe A. Sonde de granularité de page (MESURÉ)

Programme C compilé avec `clang -O0` et exécuté avec `perl -e 'alarm 20; exec @ARGV' ./pg2`, dans le répertoire temporaire de la session (non conservé hors de ce rapport). Il crée 256 Kio anonymes alignés sur 16 Kio, essaie les opérations puis lit la région par `mach_vm_region_recurse`.

```c
#define TRY(label, expr) do { errno = 0; long _r = (long)(expr); int _e = errno; \
    printf("%-64s -> %s errno=%d (%s)\n", label, (_r == -1 || (void*)_r == MAP_FAILED) ? "FAIL" : "OK  ", \
           _e, _e ? strerror(_e) : "-"); } while (0)
char *base = mmap(NULL, 0x40000, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
TRY("mmap(MAP_FIXED|ANON, base+0x1000, 0x1000)", mmap(base+0x1000, 0x1000, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0));
TRY("mmap(MAP_FIXED|ANON, base+0x2000, 0x4000)", mmap(base+0x2000, 0x4000, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0));
memset(base, 0xAA, 0x40000);
TRY("mmap(MAP_FIXED|ANON, base+0x4000, 0x1000)", mmap(base+0x4000, 0x1000, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0));
TRY("mprotect(base+0x9000, 0x1000, PROT_NONE)", mprotect(base+0x9000, 0x1000, PROT_NONE));
TRY("mprotect(base+0x8000, 0x1000, PROT_READ)", mprotect(base+0x8000, 0x1000, PROT_READ));
TRY("munmap(base+0x1000, 0x1000)", munmap(base+0x1000, 0x1000));
TRY("munmap(base+0x10000, 0x1000)", munmap(base+0x10000, 0x1000));
/* fichier de 64 Kio : mmap offset 0x1000 MAP_PRIVATE et MAP_SHARED, offset 0x4000 longueur 0x4000 MAP_SHARED */
```

Sortie (extraits, M5 Max, macOS 27.0.1) :

```
getpagesize=16384 sysconf=16384 vm_page_size=16384
mmap(MAP_FIXED|ANON, addr=base+0x1000, len=0x1000)               -> FAIL errno=22 (Invalid argument)
mmap(MAP_FIXED|ANON, addr=base+0x2000, len=0x4000)               -> FAIL errno=22 (Invalid argument)
mmap(MAP_FIXED|ANON, addr=base+0x4000, len=0x1000)               -> OK   errno=0 (-)
   after: +0x3fff=aa +0x4000=00 +0x5000=00 +0x7fff=00 +0x8000=aa  (00 where the 16K page was replaced)
mprotect(base+0x9000, 0x1000, PROT_NONE)                         -> FAIL errno=22 (Invalid argument)
mprotect(base+0x8000, 0x1000, PROT_READ) (16K aligned, 4K len)   -> OK   errno=0 (-)
   region(after mprotect 4K len @0x102948000): kr=0 start=0x102948000 size=0x4000 prot=1 maxprot=7
munmap(base+0x1000, 0x1000)                                      -> FAIL errno=22 (Invalid argument)
munmap(base+0x10000, 0x1000) (16K aligned, 4K len)               -> OK   errno=0 (-)
   region(after munmap of 4K len at +0x10000 @0x102950000): kr=0 start=0x102954000 size=0x2c000 prot=3 maxprot=7
mmap(file, off=0x1000, len=0x1000, MAP_PRIVATE)                  -> FAIL errno=22 (Invalid argument)
mmap(file, off=0x1000, len=0x1000, MAP_SHARED)                   -> FAIL errno=22 (Invalid argument)
mmap(file, off=0x4000, len=0x4000, MAP_SHARED)                   -> OK   errno=0 (-)
mmap(file, off=0x4000, len=0x1000, MAP_SHARED|MAP_FIXED at base+0x1000) -> FAIL errno=22 (Invalid argument)
mach_vm_protect(a+0x1000, 0x1000)                                -> kr=0 ((os/kern) successful)
mach_vm_protect(a, 0x1000) 16K aligned, 4K len                   -> kr=0 ((os/kern) successful)
   region(after mach_vm_protect 4K len @0x102960000): kr=0 start=0x102960000 size=0x4000 prot=1 maxprot=7
```

Une première version de la sonde a planté sur une écriture dans la page de 16 Kio que `mprotect` venait de passer en lecture seule : le plantage confirme l'arrondi.

Vérifications de SDK (`grep` dans `usr/include/sys/mman.h` et `fcntl.h` du SDK macOS) : `MAP_FIXED_NOREPLACE`, `mremap`, `memfd_create`, `epoll_create`, `eventfd` absents ; `MAP_32BIT` (0x8000), `MAP_JIT` (0x0800), `MADV_ZERO` (11), `O_RESOLVE_BENEATH` (0x1000) présents.

## Annexe B. Jeu d'appels vu dans les traces Wine et EAC de l'expérience précédente (MESURÉ)

Commande : `grep -a -o '^hl: syscall [0-9]*(' <trace> | sed … | sort -n | uniq -c`, sur `XP/run/logs/eac-syscall-trace.log` (282 379 appels, 102 numéros distincts) et `XP/run/logs/eac-parent-ptrace-regset.log` (251 693 appels, 103 numéros). Ce sont des appels ARM64 émis par Rosetta pour le compte de Wine et d'EAC, plus ceux de Rosetta lui-même, jusqu'à l'erreur EAC 509. Union des deux traces, 103 numéros (numéro et nom ARM64) :

8 getxattr, 10 fgetxattr, 17 getcwd, 19 eventfd2, 20 epoll_create1, 21 epoll_ctl, 22 epoll_pwait, 23 dup, 24 dup3, 25 fcntl, 29 ioctl, 32 flock, 34 mkdirat, 35 unlinkat, 38 renameat, 43 statfs, 44 fstatfs, 46 ftruncate, 48 faccessat, 49 chdir, 50 fchdir, 53 fchmodat, 56 openat, 57 close, 59 pipe2, 61 getdents64, 62 lseek, 63 read, 64 write, 66 writev, 67 pread64, 68 pwrite64, 72 pselect6, 73 ppoll, 78 readlinkat, 79 newfstatat, 80 fstat, 85 timerfd_create, 93 exit, 94 exit_group, 96 set_tid_address, 98 futex, 99 set_robust_list, 113 clock_gettime, 114 clock_getres, 115 clock_nanosleep, 117 ptrace, 122 sched_setaffinity, 123 sched_getaffinity, 124 sched_yield, 129 kill, 131 tgkill, 134 rt_sigaction, 135 rt_sigprocmask, 139 rt_sigreturn, 148 getresuid, 150 getresgid, 157 setsid, 160 uname, 163 getrlimit, 165 getrusage, 166 umask, 167 prctl, 169 gettimeofday, 172 getpid, 173 getppid, 174 getuid, 175 geteuid, 176 getgid, 177 getegid, 178 gettid, 179 sysinfo, 194 shmget, 195 shmctl, 196 shmat, 198 socket, 199 socketpair, 200 bind, 201 listen, 202 accept, 203 connect, 204 getsockname, 205 getpeername, 207 recvfrom, 208 setsockopt, 209 getsockopt, 210 shutdown, 211 sendmsg, 212 recvmsg, 215 munmap, 220 clone, 221 execve, 222 mmap, 223 fadvise64, 226 mprotect, 240 rt_tgsigqueueinfo, 260 wait4, 261 prlimit64, 264 name_to_handle_at, 269 sendmmsg, 278 getrandom, 279 memfd_create, 291 statx.

Les plus fréquents dans la seconde trace : `read` 56 887, `rt_sigprocmask` 50 532, `write` 24 806, `clock_gettime` 22 401, `epoll_pwait` 20 658, `writev` 17 010, `mmap` 10 358, `mprotect` 6 001, `recvmsg` 4 798, `openat` 4 497, `readlinkat` 4 351, `newfstatat` 4 066.

Parmi les 103, seuls deux n'ont pas de `case` par numéro dans le dispatch de HL : 10 (`fgetxattr`, voir le défaut de numérotation en section 5) et 264 (`name_to_handle_at`, 2 appels). Les lignes « unimplemented syscall » des traces ne contiennent que ces deux numéros. Ni `mremap`, ni `madvise`, ni `mincore`, ni `process_vm_readv`, ni `userfaultfd` n'apparaissent dans ces traces, ce qui ne prouve pas que la suite de la session ne les utilise pas.

## Annexe C. Témoin en processus (MESURÉ)

Les trois pilotes et le shim sont dans le répertoire temporaire de la session (`scratchpad/inproc`), non conservé hors de ce rapport. Empreintes SHA-256 des sources, 16 premiers caractères : `drv.c` b5ed6aa5886ee9d8, `drv2.c` fc8c3d444ea55dee, `drv3.c` f47ff861c37ea3e3, `shim.c` 27182628a6de137f, `vma_only.c` 232ba06c0cb12564.

Shim, la totalité de l'interface mémoire invitée requise par les handlers de fichiers, d'I/O, de temps et de procfs :

```c
void *guest_ptr(const guest_t *g, uint64_t gva) { (void)g; return (void *)(uintptr_t)gva; }
void *guest_ptr_avail(const guest_t *g, uint64_t gva, uint64_t *avail) { (void)g; if (avail) *avail = 1ULL << 30; return (void *)(uintptr_t)gva; }
int guest_read(const guest_t *g, uint64_t gva, void *dst, size_t len) { (void)g; memcpy(dst, (void *)(uintptr_t)gva, len); return 0; }
int guest_write(guest_t *g, uint64_t gva, const void *src, size_t len) { (void)g; memcpy((void *)(uintptr_t)gva, src, len); return 0; }
int guest_read_str(const guest_t *g, uint64_t gva, char *dst, size_t max) {
    (void)g; if (!max) return -1; const char *s = (const char *)(uintptr_t)gva; size_t i = 0;
    for (; i + 1 < max && s[i]; i++) dst[i] = s[i];
    dst[i] = 0; return s[i] ? -1 : (int)i;
}
```

Ce shim n'est pas sûr face à un pointeur invalide : il fait un `memcpy` brut. En production il doit être remplacé par des copies protégées qui renvoient `-EFAULT` (modèle : `FEX/FaultSafeUserMemAccess.cpp`).

Édition de liens (bash, sans exécution de HVF) : tous les `EXP/scratch-hl/hyper-linux-copy/_build/obj/*.o` sauf `hl.o` et `guest.o`, plus `drv.o` (ou `drv2.o` avec `vma_only.o`, ou `drv3.o`) et `shim.o` :

```
clang -arch arm64 -o drv drv.o shim.o $OBJS -framework AudioToolbox -framework CoreAudio \
      -framework Hypervisor -Wl,-undefined,dynamic_lookup
```

Sorties, lignes finales : pilote 1 `PASSED (0 failures)` avec 12 lignes `OK`, pilote 2 `PASSED (0 failures)` avec 5 `OK`, pilote 3 `PASSED (0 failures)` avec 6 `OK`. Premier essai du pilote 1 : `write` renvoyait `-11` faute de `current_thread` (voir 1.7), corrigé par `thread_init(0x100000)` et `thread_register_main(0, NULL, 1, 0x100000)`. Premier essai du pilote 2 : la ligne de chemin montrait `/tmp/…` au lieu de `/usr/…`, parce que mon nom de VMA n'était pas canonique alors que `hl_vfs_add_bind` canonicalise la racine du bind (`/tmp` est un lien vers `/private/tmp` sur macOS, `vfs.c:139-172`) ; en production `fcntl(F_GETPATH)` renvoie le chemin canonique, ce que le pilote simule par `realpath`.
