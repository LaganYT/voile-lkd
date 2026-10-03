# Port de FEXCore sur macOS arm64 : rapport vivant

Ce document est mis à jour à chaque jalon. Il sépare ce qui a été mesuré dans cette session de ce qui est déduit de la lecture du code. Les citations `fichier:ligne` pointent vers l'arbre intact `src/fex-pristine` (commit FEX 0df84d3844bcdb87bb7d3f5b8fb0959cd009c038), sauf mention contraire.

Dernière mise à jour : 2026-10-02 10:57 (jalon 3 : témoins a, b, d, e, h passent).

## Mesure de performance CPU (2026-10-02 14:57)

MESURE, à titre indicatif (un seul passage par configuration, deux pour le natif et Rosetta, M5 Max). Programme C portable `w12_bench.c` compilé en ELF x86-64 (`-O2`, zig cc) pour FEXCore, et en Mach-O x86_64 et arm64 (`clang -O2`) pour Rosetta et le natif. Durées en ms :

| Noyau | arm64 natif | x86_64 sous Rosetta | FEXCore, base 0 | FEXCore, base invitée 8 Tio |
| --- | --- | --- | --- | --- |
| entiers (LCG, 400 M itérations) | 411 / 400 | 379 / 386 | 378 | 394 |
| mémoire (8 Mio, 200 passages, pas de 64 octets) | 29 / 29 | 30 / 31 | 35 | 37 |
| flottants double (200 M) | 430 / 427 | 351 / 352 | 367 | 361 |
| appels (fib 32) | 3 / 3 | 3 / 3 | 5 | 5 |
| tri shell 100 k x 20 (branches) | 128 / 128 | 141 / 142 | 174 | 175 |

Les sommes de contrôle x86 (Rosetta et FEXCore) sont identiques ; la somme flottante native diffère (contraction FMA du compilateur arm64), ce qui n'est pas un défaut. Sur ces noyaux FEXCore est au niveau de Rosetta, de 0 à 23 % plus lent, et la base invitée coûte au plus 4 % (une addition par accès mémoire, l'adressage `[x24, xaddr]` sans instruction supplémentaire n'est pas encore utilisé). Ce n'est pas une mesure de jeu : le JIT ne voit ici ni Wine, ni appels système fréquents, ni code automodifiable. Journaux : `evidence/fexcore-darwin/bench/`.

## Jalon 7 : fidélité Linux des sélecteurs et des fautes d'exécution (2026-10-02 14:54)

MESURE. (1) CS=0x33 et SS=0x2b comme sous Linux x86-64 (`arch/x86/include/asm/segment.h`), au lieu de 0x30 et 0 : FEX choisit l'entrée GDT par `Selector >> 3`, les bits RPL ne changent rien à l'exécution (`w04_tls` affiche `cs=0x0033 ss=0x002b`). (2) Nouveau témoin `w11_badjump` 15/15 : `call *%rax` vers 9, `push 9; ret` (le motif de l'injection EAC), `jmp` vers une adresse haute non mappée. Linux livre SIGSEGV, `si_code` SEGV_MAPERR, `si_addr` = cible, RIP = cible, `trapno` 14, code d'erreur 0x14. FEX classait toute faute de chargement d'instruction en SEGV_ACCERR avec le bit « présent » (0x15) : l'hôte reclasse maintenant d'après la table des mappings (non mappé : MAPERR et 0x14 ; mappé sans exécution : ACCERR et 0x15).

Problème ouvert : `w07_fault` passe ses 99 contrôles puis boucle en fin de programme (saut vers une adresse de pile après le test de débordement de pile sur pile alternative, puis le gestionnaire du témoin renvoie toujours à la même étiquette). Cause non établie ; piste : pile d'appels fantôme de FEX (`REG_CALLRET_SP`) après la remise à zéro sur faute de garde.

## Jalon 6 : threads invités (2026-10-02 14:50)

MESURE. L'hôte prototype implémente `clone` pour les threads (CLONE_VM|FS|FILES|SIGHAND|THREAD avec SETTLS, PARENT_SETTID, CHILD_CLEARTID), `futex` WAIT/WAKE et leurs variantes BITSET sur `os_sync_wait_on_address`, et la sortie d'un thread (`exit` sans `exit_group`, avec remise à zéro et réveil de `clear_child_tid`). Témoin `w10_threads` 13/13 avec et sans base invitée : 8 threads invités exécutés en parallèle par le JIT, 1 600 000 `lock add` cohérents, base FS propre à chaque thread, 2000 passages de témoin par futex entre deux threads, attentes de fin par `CHILD_CLEARTID`. Les `clone` de type fork renvoient ENOSYS : ils relèvent de la couche LKD. Journaux `evidence/fexcore-darwin/runs/w10_threads-14494*.log`.

## Jalon 5 : SIMD et x87 identiques à l'oracle (2026-10-02 14:47)

MESURE. Nouveau témoin `w03_simd` : 109 opérations (SSE, SSE2, SSSE3, SSE4.1/4.2, AES, PCLMUL, AVX, AVX2, FMA, F16C, gather AVX2, `vmaskmovps`, x87 dont `fistp` sous deux modes d'arrondi, `fcompp`, `fldpi`, `fsin`, `f2xm1`, `fyl2x`, MXCSR arrondi vers le bas et FTZ/DAZ). Le même source compilé en Mach-O x86_64 tourne sous Rosetta comme oracle indépendant (`witnesses/elf-x86_64/oracle.sh`). Les transcriptions FEX et Rosetta sont identiques ligne pour ligne, 109/109, avec et sans base invitée (`evidence/fexcore-darwin/runs/w03_simd-144706.log`, `oracle/w03_simd-rosetta-144228.log`).

Défaut trouvé et corrigé en route : la passe `x87StackOptimizationPass` émet des `LoadMem`/`StoreMem` sur l'adresse hôte de l'état CPU (`FormContextAddress`) dans son chemin lent (après `fldcw`). Avec une base invitée ces accès étaient décalés et fautaient (SIGBUS hôte, adresse = état CPU + base). L'allocateur de registres insère des copies qui masquent l'origine de l'adresse, donc la détection dans le JIT (`IsHostAddress`) ne suffit pas : sous Apple la passe utilise maintenant `LoadContextIndexed`/`StoreContextIndexed`, qui ne portent pas de sémantique d'adresse invitée. Patch SHA-256 `53a6dcb0...`.

## Jalon 4 : décalage de base invité (2026-10-02 14:40)

MESURE. FEXCore exécute maintenant le code x86-64 avec une base invitée : adresse hôte = `FEXCore::GuestBase` + adresse invitée. Le registre x24 est réservé (`GUESTBASE`, `Arm64Emitter.h`), retiré du pool dynamique et rechargé par `FillStaticRegs` à chaque entrée du dispatcher. Les opérations mémoire du JIT passent par `GuestPtr` ou par la forme `[x24, adresse]` : chargements et stockages, variantes TSO, paires, vecteurs par élément, masqués et diffusés, push/pop sans écriture différée (`Push`, `PushTwo`, `Pop`, `PopTwo`), `MemSet`, `MemCpy`, atomiques (dont `CASPair`), lignes de cache, gather émulé, `ValidateCode`. Le décodeur lit les octets invités par `G2H` (`Frontend.cpp`, `AdjustAddrForSpecialRegion`). L'hôte prototype traduit tous ses pointeurs invités (chargeur ELF, pile initiale, syscalls, cadres de signaux, adresse de faute rendue en adresse invitée).

Résultats avec `FEXDARWIN_GUEST_BASE=0x80000000000` (8 Tio) : nouveau témoin `w09_lowmem` 12/12 (mmap fixe et accès à 0x7ffe0000 et 0x10000, xadd et cmpxchg verrouillés, `rep stos`/`rep movs`, appel de code placé à 0x14000, push, pop, call et ret sur une pile à 0x1c000) ; sans base invitée le même binaire meurt en SIGSEGV hôte (139), ce qui confirme que le témoin exerce bien la zone basse. Témoins précédents avec la base : `w02_int` 9524/9524, `w04_tls` 25/25, `w05_mem` 39/39, `w06_smc` 20/20, `w08_sys` 50/50, `w01_hello` OK. Journaux `evidence/fexcore-darwin/runs/*-1440*.log`. Patch `src/patches/fexcore-darwin.patch` SHA-256 `edc42aab...`, hôte `fexdarwin-run` SHA-256 `6998374b...`.

Limites : le coût de l'addition par accès n'est pas mesuré ; `LoadMem`/`StoreMem` pourraient utiliser l'adressage `[x24, xaddr]` sans instruction supplémentaire (optimisation à faire après les témoins de performance). Les chemins SVE ne sont pas couverts (pas de SVE sur ce CPU). Le cache disque de code et le désassembleur de débogage lisent encore le code invité sans `G2H` : désactivés ici.

## Etat au redémarrage (2026-10-02 10:55, après la deuxième interruption)

Relu sur disque : le patch FEXCore (`src/patches/fexcore-darwin.patch`) est à jour au dernier export ; l'hôte `src/fexdarwin` (11 fichiers sources et en-têtes, `scripts/build.sh`, `scripts/run-with-timeout.sh`, `scripts/export-patch.sh`) compile ; les témoins sont dans `witnesses/elf-x86_64` (`build.sh`, `run.sh`, `oracle.sh`, `src/`, `bin/`, `oracle/`, `gen/`). Témoins qui passent sur FEX dans cette session, journaux dans `evidence/fexcore-darwin/runs/` : (a) `w01_hello`, (b) `w02_int` 9524 sur 9524, (d) `w04_tls` 25 sur 25, (e) `w05_mem` 39 sur 39, (h) `w08_sys` 50 sur 50. Le témoin (f) `w06_smc` s'est d'abord bloqué après sa première exécution (délai dépassé, code 142, journal `w06_smc-105508.log`). Cause établie : macOS livre un SIGBUS de `si_code` 1 pour les fautes de permission et pour les fautes d'alignement, et mon gestionnaire prenait toute faute SIGBUS dans le JIT pour un accès atomique non aligné (`HandleUnalignedAccess`) ; la faute d'écriture sur une page protégée repartait donc indéfiniment. Correction : distinguer l'alignement par le syndrome ESR (`DFSC` = 0x21). Le témoin (f) passe ensuite. Les témoins (c), (g), (i) ne sont pas encore écrits. Rosetta 2 sert d'oracle indépendant pour les tables du témoin (b) (journal `evidence/fexcore-darwin/oracle/w02_int-rosetta-104410.log`, 9460 sur 9460). L'analyse du décalage de base invité est écrite plus bas et reste à implémenter.

## Etat courant en une phrase

Jalon 2 atteint : le témoin (a) passe. `fexdarwin-run` charge un ELF x86-64 statique, FEXCore compile son code en ARM64 avec le JIT dans un processus macOS arm64 natif (signature ad hoc de l'éditeur de liens, aucun droit particulier), le syscall `write` est émulé et affiche `hello from x86-64 under FEXCore on macOS arm64`, `exit_group` termine avec le code 0. Journal brut : `evidence/fexcore-darwin/runs/w01_hello-103618.log`. Les autres témoins ne sont pas encore écrits ou exécutés.

## Ce qui existe sur disque

Racine de travail : `build/experiments/rocket-wine-fex-darwin-20261002` (notée `$EXP`).

| Chemin | Contenu | Etat |
| --- | --- | --- |
| `$EXP/src/fex-pristine` | copie intacte des sources FEX, passée en lecture seule (`chmod -R a-w`) | fait |
| `$EXP/src/fex-darwin` | copie de travail patchée | 13 fichiers modifiés ou ajoutés |
| `$EXP/src/patches/fexcore-darwin.patch` | `diff -ruN fex-pristine fex-darwin`, 997 lignes | exporté à chaque jalon (dernier export 10:25) |
| `$EXP/src/fexdarwin/build/fexcore` | répertoire de build Ninja de FEXCore (hors de l'arbre patché) | compilé, bibliothèques produites |
| `$EXP/src/fexdarwin/` (sources de l'hôte) | `CMakeLists.txt`, `include/`, `src/`, `scripts/build.sh`, binaire `build/host/fexdarwin-run` | premier jet qui exécute le témoin (a) |
| `$EXP/logs/fexcore-configure-001.log`, `fexcore-configure-002.log` | configurations CMake (la seconde fixe la version FEX affichée) | gardés |
| `$EXP/logs/fexcore-build-001.log` à `fexcore-build-006.log` | six compilations successives : 57, 125, 6, 0 erreurs, puis bibliothèques annexes | gardés |
| `$EXP/witnesses/elf-x86_64` | `src/rt.h`, `src/w01_hello.c`, `build.sh`, `run.sh`, `bin/w01_hello` | témoin (a) construit et exécuté |
| `$EXP/evidence/fexcore-darwin` | sondes macOS (sources et journaux, en cours de reconstitution) | partiel |

Les fichiers `logs/hl-*`, `logs/host-*`, `tools/build-*.sh` et les répertoires `*darwin-memory`, `*darwin-process`, `scratch-hl` appartiennent à l'autre équipe et ne sont pas touchés.

Fichiers du patch actuel (extraits de l'en-tête de `fexcore-darwin.patch`) :

- `CMakeLists.txt` : accepte `Darwin`, positionne `FEX_DARWIN_CORE_ONLY`, ignore le script `aarch64_fit_native.py` qui lit `/proc/cpuinfo` (`CMakeLists.txt:514` dans l'arbre intact), saute `Source/`, `Data/binfmts`, `Data/AppConfig` et jemalloc glibc.
- `Data/CMake/LinkerGC.cmake` : `-dead_strip` à la place de `--gc-sections --strip-all --as-needed` sous Apple.
- `FEXCore/Source/CMakeLists.txt` : n'inclut pas `Utils/Allocator/64BitAllocator.cpp` (ligne 15 de l'arbre intact) ni la bibliothèque partagée sous Apple.
- `FEXCore/include/FEXCore/Utils/TypeDefines.h` : ajoute `FEX_HOST_PAGE_SIZE` (16384 sous Apple).
- `FEXCore/include/FEXCore/Utils/JITWriteScope.h` (nouveau) : garde RAII autour de `pthread_jit_write_protect_np`, avec compteur d'imbrication par thread.
- `FEXCore/include/FEXCore/Utils/AllocatorHooks.h` : `malloc/malloc.h`, `MAP_JIT` pour les allocations exécutables, `MADV_ZERO` à la place de `MADV_DONTNEED`, pas de THP.
- `FEXCore/include/FEXCore/Utils/PrctlUtils.h`, `SharedCodeBufferManager.cpp/.h`, `ThreadPoolAllocator.h`, `JIT.cpp`, `InternalThreadState.h`, `Dispatcher.cpp` : pages d'hôte de 16 Kio pour les pages de garde, la page d'interruption et les tampons temporaires.

Pas encore faits (liste des prochaines modifications) : `Allocator.cpp`, `AllocatorHooks.cpp`, `FEXHeaderUtils/Syscalls.h`, `Profiler.cpp`, `MiscOps.cpp` (`SYS_getcpu`), futex de `WritePriorityMutex.h` et `InterruptableConditionVariable.h`, `SignalScopeGuards.h`, `Threads.cpp`, retrait de `x18` des registres de l'allocateur de registres du JIT, bascule d'écriture JIT aux endroits qui modifient le code.

## Résultats mesurés sur cette machine

Machine : Apple M5 Max, macOS 27.0.1 (26A434), pages de 16 Kio (`sysctl hw.pagesize` = 16384), Xcode 27.0, Apple clang 21.0.0. Ces mesures viennent de petits programmes C lancés dans cette session. Leurs sources et journaux sont en cours d'archivage dans `$EXP/evidence/fexcore-darwin/probes/` (le répertoire temporaire de la session a été vidé par l'interruption, les programmes sont réécrits à l'identique).

| Question | Résultat mesuré | Conséquence |
| --- | --- | --- |
| `mmap` avec `MAP_JIT` sans droit particulier | réussit, binaire signé ad hoc par l'éditeur de liens (`flags=0x20002(adhoc,linker-signed)`), sans entitlement ni runtime durci | signature minimale suffisante pour l'instant |
| `mmap` RWX sans `MAP_JIT` | échoue, `errno` 13 | les tampons de code doivent être `MAP_JIT` |
| `mprotect` RW vers RX sur de la mémoire ordinaire | réussit et le code s'exécute | solution de repli possible, plus lente |
| Mémoire sous 4 Gio (par exemple 0x400000) | `mmap MAP_FIXED` échoue, `errno` 12 | un ELF x86-64 classique lié à 0x400000 ne peut pas être chargé |
| Réduire `__PAGEZERO` (`-Wl,-pagezero_size,0x4000` ou `0`) | le processus est tué au lancement (code de sortie 137) | pas de contournement par l'édition de liens |
| `mach_vm_deallocate(0, 4 Gio)` puis `mach_vm_allocate` à 0x400000 | la libération réussit, l'allocation échoue (`KERN_INVALID_ADDRESS`) | la limite est dans la carte VM, pas dans une région occupée |
| Plus haute adresse utilisateur | page 0x7ffffdffc000, fin à 0x7ffffe000000 | les adresses invitées font moins de 47 bits moins 32 Mio |
| Carte libre (par `mach_vm_allocate` fixe sans écrasement) | libre : 0x110000000 à 0x180000000, 0x7000000000 à 0x7780000000, 0x7d80000000 à 0x7ffffe000000 ; occupé entre | l'essentiel de l'espace libre est près du haut |
| Réservation `PROT_NONE` `MAP_NORESERVE` de 2^46 octets | réussit ; 2^47 échoue | une fenêtre invité très large est réservable |
| Mapping RW anonyme de 2^40 octets | réussit | la table de pages du cache de recherche de FEX est allouable |
| `MADV_DONTNEED` | ne remet pas la page à zéro (octet restant 0xcd) | FEX suppose un remplissage par zéro, `AllocatorHooks.h:128` doit changer |
| `MADV_ZERO` | remet à zéro sur du RW, `EPERM` sur du `PROT_READ` | utilisé comme équivalent, avec repli par remap |
| `MAP_32BIT` | échoue, `errno` 12 | pas d'aide pour la mémoire basse |
| `x18` sous charge et signaux | 6826 valeurs perdues sur 334453 itérations de 90 µs, valeur lue 0 | `x18` ne peut pas contenir d'état du JIT |

Fautes mémoire et exceptions, mesurées par la sonde `evidence/fexcore-darwin/probes/sig1.c` (journal `probe-sig1-001.log`) :

| Cas | Signal | `si_code` | Syndrome ESR |
| --- | --- | --- | --- |
| écriture dans une page `PROT_READ` | SIGBUS | 1 | 0x92000047 |
| lecture ou écriture dans une page `PROT_NONE` | SIGBUS | 1 | 0x92000007 ou 0x92000047 |
| accès à une page non mappée, à l'adresse 0, non canonique ou au-dessus de la limite haute | SIGSEGV | 2 | 0x92000004 à 0x92000007 |
| exécution d'une page non exécutable | SIGBUS | 1 | 0x8200000f |
| `ldxr`, `casal`, `ldadd`, `ldapr`, `stlr` à cheval sur 16 octets ou une ligne de cache | SIGBUS | 1 | 0x92000021 ou 0x92000061 (faute d'alignement) |
| `ldaxr` non aligné dans une granule de 16 octets, `ldapr` non aligné dans la granule, `ldr` ou `ldp` ordinaire non aligné | aucune faute | | |
| `hlt #0` ; `udf #0` ; `mrs ctr_el0` | SIGILL | 2 | |
| `brk #0` | SIGTRAP | 0 | |

Conséquences : `si_code` ne sépare pas permission et alignement, seul l'ESR le fait. Pour l'invité Linux, SIGBUS d'hôte (permission) doit devenir SIGSEGV avec `SEGV_ACCERR`, et SIGSEGV d'hôte (non mappé) doit devenir SIGSEGV avec `SEGV_MAPERR`, ce que la table des VMA de l'hôte permet de décider.

Capacités du processeur (`sysctl hw.optional.arm.*`) : AES, PMULL, SHA1, SHA256, SHA512, SHA3, CRC32, LSE, LSE2, LRCPC, LRCPC2, FlagM, FlagM2, FCMA, JSCVT, DotProd, I8MM, BF16, RPRES, AFP, CSSC, FRINTTS, ECV, HBC, WFxT, SME et SME2. Absents de la liste : SVE ordinaire, RNG (RNDR), MOPS. `hw.cachelinesize` = 128.

## Reconnaissance : dépendances Linux de FEXCore

La bibliothèque FEXCore elle-même a peu de dépendances au noyau. La couche Linux est dans `Source/Tools/LinuxEmulation`, hors périmètre de cette bibliothèque. Tableau des points trouvés :

| Catégorie | Emplacement | Problème sous macOS | Traitement |
| --- | --- | --- | --- |
| En-têtes absents | `AllocatorHooks.h:9` (`malloc.h`), `MiscOps.cpp:9` (`syscall.h`), `Profiler.cpp:5` (`linux/magic.h`) | n'existent pas dans le SDK | `malloc/malloc.h` (fait), reste à traiter |
| Appels système bruts | `MiscOps.cpp:270` (`SYS_getcpu`) | aucun appel `getcpu` | reste à traiter |
| Futex | `WritePriorityMutex.h:276-294`, `InterruptableConditionVariable.h:42,85` | pas de futex | `os_sync_wait_on_address` (à faire) |
| Masque de signaux | `SignalScopeGuards.h:161,174` (`rt_sigprocmask`) | pas d'appel brut | `pthread_sigmask` (à faire) |
| `prctl` | `SharedCodeBufferManager.cpp:72` (MDWE), `Allocator.cpp:63`, `AllocatorHooks.cpp:43,128` | pas de `prctl` | supprimé ou sans effet |
| `/proc/self/maps` | `Allocator.cpp:220` | n'existe pas | `mach_vm_region` (à faire) |
| `MAP_FIXED_NOREPLACE` | `Allocator.cpp:123,258` | flag absent | `mach_vm_allocate` fixe sans écrasement |
| `madvise` | `Allocator.cpp:263` (`MADV_DONTDUMP`), `AllocatorHooks.h:128,146` | sémantique ou flag absent | `MADV_ZERO`, sans effet pour THP |
| `memalign`, `malloc_usable_size` | `AllocatorHooks.cpp:70` et suivantes | absents | `posix_memalign`, `malloc_size` (à faire) |
| Mémoire exécutable | `SharedCodeBufferManager.cpp:21` | RWX refusé | `MAP_JIT` et `JITWriteScope` (fait pour l'allocation) |
| Taille de page | `TypeDefines.h:9` (4096), `SharedCodeBufferManager.cpp:25`, `InternalThreadState.h:90,125`, `Dispatcher.cpp:37`, `JIT.cpp:917,923` | pages d'hôte de 16 Kio | `FEX_HOST_PAGE_SIZE` (fait) |
| Pas de 12 bits codé en dur | `LookupCache.h:131,132,150,196,345,346,400,411`, `Dispatcher.cpp:158,195,204` | granularité de page invitée de 4 Kio, cohérente avec `FEX_PAGE_SIZE` (invité) | laissé à 4 Kio, voir l'analyse de pages |
| Registre de plateforme | `Arm64Emitter.cpp:79,86` (`r18` dans le pool dynamique) | macOS écrase `x18` | retirer `r18` (à faire) |
| Registres système | `CPUID.cpp:147` (`CNTFRQ_EL0`), `MiscOps.cpp` (`TPIDRRO_EL0`), `ALUOps.cpp:96,102` (`CNTVCT_EL0`) | `CNTVCT_EL0` lisible en EL0, `TPIDRRO_EL0` contient autre chose sous macOS | `SupportsCPUIndexInTPIDRRO=false` (à confirmer) |
| RDTSCP et RDPID | `OpcodeDispatcher.cpp:5076,5090` | `HostType != Linux` sans TPIDRRO : instruction non supportée | accepté, à documenter dans le témoin (h) |
| Détection des fonctions du CPU | `Source/Common/HostFeatures.cpp` (hors FEXCore) lit `ID_AA64*` par `mrs` | `mrs` des registres d'identification déclenche SIGILL en EL0 sous macOS | remplacer par `sysctlbyname("hw.optional.arm.*")` dans l'hôte |
| Réécriture de code en place | `JIT.cpp:535-646` (liens de blocs et délieurs), `JIT.cpp:1163` (copie du bloc), `Arm64.cpp:2106,2119-2160` (correctifs de gestionnaire SIGBUS) | écriture refusée sur `MAP_JIT` sans bascule | `JITWriteScope` (à poser) |
| `getrandom` | `Core.cpp:109-111`, `JIT.cpp:78` via `FHU::Syscalls::getrandom` | pas d'appel | `getentropy` (à faire) |

## Contraintes de Wine sur l'espace d'adressage (mesurées sur le vrai préchargeur)

Fichier lu : `rocket-hyperlinux-20260928/linux-root/rootfs/usr/lib/wine/wine64-preloader.static` (sha256 `fafbb248c5214f116191f8c9298e38067873b55e8795b9de88f7b4775e7a6aab`, 17640 octets, identique à `wine64-preloader`). Scripts et sorties : `evidence/fexcore-darwin/wine-preloader/` (pyelftools installé dans le venv).

- Le préchargeur de ce paquet Debian est un ELF `ET_DYN` x86-64 lié à l'adresse 0 (entrée 0x1000, quatre `PT_LOAD`, pas de `PT_INTERP`, une section `.dynamic`). Il n'est donc pas lié près de 0x7d400000 comme les anciens préchargeurs `ET_EXEC` : dans cette version, l'adresse de chargement est choisie par le chargeur au moment du lancement.
- Sa table `wine_preload_info` (section `.data`, quatre entrées de 16 octets suivies de deux entrées vides remplies à l'exécution pour la plage de l'exécutable PE) demande ces réservations :

| Entrée | Début | Fin | Taille |
| --- | --- | --- | --- |
| 0 | 0x10000 | 0x110000 | 1 Mio |
| 1 | 0x110000 | 0x68000000 | 1662,9 Mio |
| 2 | 0x7f000000 | 0x7fff0000 | 15,9 Mio (contient 0x7ffe0000, USER_SHARED_DATA) |
| 3 | 0x7ffffe000000 | 0x7fffffff0000 | 31,9 Mio |

- Conséquence mesurée : les entrées 0, 1 et 2 sont sous 4 Gio, donc impossibles à réserver dans un processus arm64 macOS (voir la table des mesures). L'entrée 3 commence à 0x7ffffe000000, exactement la première adresse au-dessus de la limite haute mesurée d'un processus macOS (la dernière page mappable finit à 0x7ffffe000000) : elle échouerait aussi. Le préchargeur affiche un avertissement `failed to reserve range` et continue pour les échecs de réservation, mais Wine a besoin de l'adresse 0x7ffe0000 pour USER_SHARED_DATA (déduction de la connaissance du code de Wine, non vérifiée ici sur ntdll).
- Donc l'identité invité = hôte ne peut pas héberger Wine. Il faut un décalage de base invité (analyse plus bas, section dédiée). L'identité reste utilisée pour le premier témoin parce qu'elle réduit le risque du portage du JIT.

## Mode « décalage de base invité » : analyse des sites et du coût (non implémenté)

But : adresse hôte = GUEST_BASE + adresse invitée, pour que l'invité dispose des adresses basses (0x10000, 0x110000 à 0x68000000, 0x7ffe0000) que macOS refuse à un processus arm64. Cette analyse a été faite par lecture du code (agent de lecture seule, puis vérification ponctuelle de ma part de `MemoryOps.cpp:587`, de la lecture GDT `OpcodeDispatcher.cpp:4210-4215`, de `FormContextAddress` et du CRC SMC `Core.cpp:645-650`). Les chiffres de coût sont des estimations, aucune mesure. Chemins relatifs à `FEXCore/Source/Interface/Core/` de l'arbre intact, `MO` = `JIT/MemoryOps.cpp`, `AO` = `JIT/AtomicOps.cpp`.

### Constats qui changent la conception

1. Des `LoadMem` et `StoreMem` de l'IR portent des adresses d'hôte : les emplacements de la pile x87 (`IR/Passes/x87StackOptimizationPass.cpp:439,475,622`, `MO:352`) et la lecture de la GDT (`OpcodeDispatcher.cpp:4215`). Une réécriture aveugle dans le backend les casserait. Il faut un indicateur IR ou les convertir en `LoadContextIndexed`.
2. Avec les défauts (`TSOEnabled=true`, `Config.json.in:577`), les accès entiers utilisent `ldapur`, `ldapr`, `ldar`, `stlur`, `stlr`, qui n'ont pas de forme registre plus registre. `VectorTSOEnabled` et `MemcpySetTSOEnabled` valent false. `SMCChecks=mtrack` désactive `ValidateCode` et `MonoBackpatcherWrite`.
3. Les cœurs Apple n'ont pas SVE : environ 34 lignes d'émission SVE sont mortes sur cette cible.
4. Il n'existe pas de registre MemBase malgré le commentaire `JIT.cpp:957` (périmé).

### Sites JIT (45 fonctions ou sites, 228 lignes d'émission mémoire)

| Groupe | Emetteurs | Forme actuelle | Avec la base | Lignes |
| --- | --- | --- | --- | --- |
| G1 LoadMem, StoreMem, Prefetch, moitiés vectorielles TSO | MO 704, 1639, 2466, 767, 1778 | `[Xn]`, `[Xn,#imm]`, `[Xn,Xm,ext]` | `[Xn]` nu devient `[B,Xn]` (gratuit), les autres +1 ADD | 29 |
| G2 TSO entier | MO 790-825, 1800-1826 | `ldapur/stlur [Xn,#imm9]`, `ldapr/ldar/stlr [Xn]` | impossible à plier, +1 ADD | 20 |
| G3 paires | MO 741, 1752 | `ldp/stp [Xn,#imm7]` | +1 | 10 |
| G4 Push, Pop | MO 1468, 1553, 1576, 1608 | pré ou post-index sur le RSP invité | pas de réécriture possible du pointeur, +1 ou +2, touche la fusion post-RA | 16 |
| G5 éléments vectoriels | MO 1349, 1387, 1421 | `ld1/st1` lane, `ld1r`, `ldr q` | +1 | 20 |
| G6 masqués et gather | MO 855, 959, 1057, 1170, 1268 | repli ASIMD par éléments | base pliée dans l'`add` existant, au plus +1 | 28 |
| G7 SVE contigu | MO 733, 843, 1682, 1843, 1690, 1721 | `[Xn,#imm,MUL VL]` | mort sur Apple | 12 |
| G8 atomiques | AO 13 à 298 (CASPair, CAS, Swap, Add, Sub, And, CLR, Or, Xor, Neg) | `caspal/casal/swpal/ldadd... [Xn]`, repli `ldaxr/stlxr` | impossible, +1 ADD avant la boucle LL/SC | 32 |
| G9 MemSet, MemCpy | MO 1851, 2086 | tout par TMP2 et TMP3 | le `mov` devient `add`, +0 | 43 |
| G10 lignes de cache | MO 2391, 2421, 2445 | `dc civac/cvac/zva` | +1 | 6 |
| G11 non temporel | MO 2504, 2530, 2546 | `stnp`, `stnt1b` | +1 | 7 |
| G12 ValidateCode | `BranchOps.cpp:323` | `ldr` en post-index | le `mov` devient `add`, +0 | 4 |
| G13 Thunk, MonoBackpatcherWrite | `BranchOps.cpp:293`, `MiscOps.cpp:351` | pointeur invité passé à du code hôte | ADD au `mov`, le travail est en C++ | 0 |
| G14 entrée de rappel JIT | `Dispatcher/Dispatcher.cpp:504` | `str x0,[x2]` | `[B,x2]` gratuit | 1 |

Les autres familles (ALUOps, ConversionOps, EncryptionOps, MoveOps, VectorOps hors masqués) n'accèdent pas à la mémoire invitée : elles touchent seulement STATE, des tables constantes et la pile hôte. `ExitFunction` et les consultations L1/L2 du dispatcher n'utilisent le RIP invité que comme clé.

### Opérande mémoire

`GenerateMemOperand` (`MO:587-613`) renvoie `[Xn]`, `[Xn,#imm]` ou `[Xn,Xm,SXTX|UXTW|SXTW #s]`. Côté IR, `Addressing.cpp:54-151` choisit imm9 ou imm12 mis à l'échelle, ou registre plus registre seulement en 64 bits sans TSO et sans décalage, sinon une chaîne d'`add`. Avec une base B : `[Xn]` nu devient `[B,Xn]` ; les formes imm et registre émettent `add T,B,Xn` puis utilisent T (TMP1 ou TMP2, libres chez les cinq appelants). Surcoût estimé par accès invité : 0 pour `[reg]` hors TSO ou vectoriel, +1 pour `[reg]` en TSO entier (cas par défaut), +1 pour `[reg+imm]`, +1 pour `[reg+reg<<s]`. Les bases fs et gs sont ajoutées dans l'IR (`OpcodeDispatcher.cpp:4101,4115`), donc les accès TEB coûtent +1 sauf pré-biaisage de `fs_cached`.

### Déréférencements C++ de pointeurs invités (18 sites, 4 obligatoires par défaut)

Lecture d'octets du décodeur (`Frontend.cpp:131-133`, `143-147`), `AdjustAddrForSpecialRegion` (`Frontend.cpp:1364,1370-1371`), conversions d'entrées vers `DecodeLoop` (`Core.cpp:518,546`, `DiskCache.cpp:561`), CRC de SMC complet (`Core.cpp:645-650`), cache disque (5 sites, désactivé par défaut), cache de code (2), `MonoBackpatcherWrite` (`Core.cpp:1178,1180`), lecture SHA de `ThunkOp` (`OpcodeDispatcher.cpp:66-73`), Zydis (débogage). Pas de changement pour le cache de recherche (indexé par adresse invitée seulement), la pile call-ret (mémoire hôte) ni les fonctions de repli de l'interpréteur. Le gestionnaire des accès non alignés (`Utils/ArchHelpers/Arm64.cpp`) lit des adresses d'hôte déjà dans les registres, mais il reconnaît des séquences JIT exactes (par exemple `HandleCASPAL_ARMv8` parcourt 9 instructions) : l'ADD de base doit se placer avant chaque séquence.

### Budget de registres

Utilisation actuelle (x86-64, hors ARM64EC) : x0 à x3 temporaires, x4 à x17 plus x19 et x29 pour les registres statiques, x26 et x27 pour PF et AF, x25 pile call-ret, x28 STATE, x20 à x24 et x30 pool dynamique (x18 retiré sur Apple). Meilleur candidat pour GUEST_BASE : x24, sauvegardé par l'appelé, donc conservé par l'ABI AAPCS64 et par `preserve_all`, à charger une fois après `PushCalleeSavedRegisters` et à l'entrée de rappel. Il n'est ni dans SRA ni dans `SignalDelegatorConfig`. Le pool dynamique passerait de 7 à 6 (sans x18) puis 5 (sans x24). Alternative sans registre réservé : `orr T,Xn,#2^44` si l'invité tient sous 2^44, même surcoût (+1) mais sans les cas gratuits `[B,Xn]`.

### Coût et risques

Environ 70 sites : 45 JIT, 18 C++, 3 IR, 5 configuration ou répartiteur. Facile : 13, moyen : 46, difficile : 5 (famille Push et Pop avec la fusion post-RA, 5e temporaire pour CASPair, distinction hôte ou invité des `LoadMem`, conversions h2g des gestionnaires de fautes, SMC et appels système, ABI des thunks). Estimation (non mesurée) : +1 opération ALU sur 75 à 90 % des accès mémoire, soit +10 à +15 % d'instructions dynamiques et +3 à +8 % de temps sur du code chargé en mémoire. Risques : écritures différées Push et Pop, formes `[Xn]` seul (LDP, LSE, exclusifs, DC ZVA), rigidité des séquences LL/SC, épuisement des temporaires de CASPair, pointeurs invités négatifs qui se replient sur B plus x (prévoir des pages de garde), B aligné sur 4 Gio.
Alternatives étudiées : TBI ne peut qu'ignorer des bits d'étiquette, un masquage ne déplace pas 0x10000 vers une adresse mappable, une vérification de plage par accès coûte au moins 3 instructions, un piège coûte des microsecondes (acceptable seulement pour des accès isolés comme 0x7ffe0000, pas pour 0x110000 à 0x68000000), `[B,Wn,UXTW]` est gratuit mais seulement pour un invité sous 4 Gio.

## Décisions de conception déjà prises

1. FEX utilise les adresses invitées comme adresses d'hôte (le code invité est lu par `reinterpret_cast<const uint8_t*>(GuestRIP)` dans `Core.cpp`). Les segments de l'ELF invité doivent donc être mappés à leur adresse virtuelle réelle dans le processus macOS. Les programmes de test seront liés à une adresse élevée, dans la fenêtre libre mesurée.
2. `FEX_PAGE_SIZE` reste 4096 : c'est la granularité de page invitée (cache de recherche, suivi des pages de code). `FEX_HOST_PAGE_SIZE` (16384) couvre ce que FEX protège lui-même sur l'hôte. Le suivi du code automodifiable travaillera au grain de 16 Kio et sur-invalidera (sûr).
3. Pas de portage de l'allocateur d'espace d'adressage 64 bits de FEX. L'hôte réserve la fenêtre invité (`PROT_NONE`) et gère les mappings invités lui-même.
4. Mémoire de code JIT : `MAP_JIT`, écriture sous `JITWriteScope`. La sortie du JIT est d'abord écrite dans un tampon temporaire ordinaire (`JIT.cpp:917-1163`), puis copiée dans le tampon partagé : un seul site d'écriture majeur pour la compilation.
5. Modèle mémoire : ordre mémoire x86 émulé par barrières logicielles (valeurs par défaut `TSOEnabled=true`, `HalfBarrierTSOEnabled=true`, `Config.json.in:577`), car le mode TSO matériel d'Apple n'est pas accessible à un processus ordinaire.

## Statut des témoins

| Témoin | Statut | Preuve (journal brut) |
| --- | --- | --- |
| (a) hello par write et exit_group | PASS | `runs/w01_hello-103618.log` |
| (b) entiers et drapeaux (9524 vérifications : ALU 8/16/32/64 bits, mul, div, décalages, rotations, bsf, bsr, popcnt, lzcnt, tzcnt, bt, chaînes, cmpxchg, xchg, lock) | PASS 9524/9524 | `runs/w02_int-104412.log` ; oracle Rosetta `oracle/w02_int-rosetta-104410.log` PASS 9460/9460 |
| (c) SSE, SSE2, AVX, AVX2, x87 | NON ECRIT | |
| (d) TLS par arch_prctl et %fs, %gs | PASS 25/25 | `runs/w04_tls-104605.log` |
| (e) mmap, mprotect, munmap, brk, madvise (multiples de 16 Kio) | PASS 39/39 | `runs/w05_mem-104605.log` |
| (f) code automodifiable (20 vérifications : réécriture entre appels, instruction du bloc en cours, mprotect RW/RX, munmap puis remap, MAP_FIXED sur du code traduit, boucle chaude) | PASS 20/20 en `--smc=mtrack` (défaut, 210 fautes d'écriture traitées) et en `--smc=full` ; en `--smc=none` le même binaire ÉCHOUE (12/20), ce qui prouve que le témoin détecte l'absence d'invalidation | `runs/w06_smc-105705.log`, `w06_smc-smc=mtrack-105723.log`, `w06_smc-smc=full-105723.log`, `w06_smc-smc=none-105714.log` |
| (g) faute invitée avec gestionnaire SIGSEGV | NON ECRIT (livraison de signaux invités à faire) | |
| (h) clobbers de syscall, cpuid, rdtsc, xgetbv | PASS 50/50 (la feuille cpuid 0xB renvoie des zéros dans FEX, consigné comme limite amont) | `runs/w08_sys-104648.log` |
| (i) clone et futex | NON ECRIT | |
| Mesures de performance | NON EXECUTE | |

## Prochaines étapes

1. Terminer les correctifs de compilation de FEXCore, produire `libFEXCore.a`, archiver les journaux.
2. Écrire l'hôte `fexdarwin-run` (chargeur ELF, pile initiale, appels système, délégué de signaux, détection des capacités).
3. Compiler les témoins avec une chaîne croisée privée et obtenir d'abord le témoin (a).
4. Elargir aux témoins suivants, mettre ce rapport à jour à chaque jalon.

## Note sur la licence d'usage du code FEX

Le fichier `CLAUDE.md` à la racine de l'arbre FEX contient la phrase : "AI must not be used to generate code for contributions to this project." Le patch produit ici est un correctif privé et local d'une copie de recherche. Il ne doit pas être soumis tel quel au dépôt FEX amont.

## Journal des jalons

- 2026-10-02 14:50 : jalon 6, threads invités par clone et futex (`w10_threads` 13/13).
- 2026-10-02 14:47 : jalon 5, `w03_simd` 109/109 identique à l'oracle Rosetta ; passe x87 corrigée pour la base invitée.

- 2026-10-02 14:40 : jalon 4, décalage de base invité dans le JIT et l'hôte ; `w09_lowmem` 12/12 et tous les témoins précédents passent avec une base de 8 Tio (repris par l'agent principal après l'arrêt de l'agent FEX).

- 2026-10-02 09:43 : copie de l'arbre FEX en `fex-pristine` (lecture seule) et `fex-darwin`.
- 2026-10-02 09:56 : configuration CMake réussie ; première compilation : 57 erreurs de 3 types.
- 2026-10-02 10:08 : premier export du patch (398 lignes).
- 2026-10-02 10:44 : jalon 3, témoin (b) entiers PASS 9524/9524 sur FEX, tables validées par Rosetta (9460/9460). Premier piège trouvé dans mes propres tests : en asm étendu clang numérote l'entrée liée d'un opérande `"+r"` après les entrées explicites, un `%2` mal placé lisait la destination (bsr).
- 2026-10-02 10:46 : (d), (e), (h) PASS. Mesures sur la sortie de (h) : cpuid vendor GenuineIntel, feuille maximale 22, marque "Apple Silicon", AVX, AVX2, FMA, F16C, AES, PCLMUL, BMI1, BMI2, ADX, SHA, FSGSBASE annoncés, AVX-512 et RDRAND non annoncés, XCR0 = 7.
- 2026-10-02 10:36 : jalon 2, premier témoin exécuté (hello). Mesure en passant : `mprotect` sur une région `MAP_JIT` échoue toujours avec `EACCES` (sonde `probe-jit2-001.log`), donc la page de garde finale du tampon de code partagé n'est pas armée (correctif dans `SharedCodeBufferManager.cpp`).
- 2026-10-02 10:25 : jalon 1, FEXCore et ses dépendances compilent (build-006). Empreintes des bibliothèques dans `evidence/fexcore-darwin/libs/sha256-libs-001.txt`.
