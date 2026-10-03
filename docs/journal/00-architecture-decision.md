# Décision d'architecture, 2 octobre 2026

Document de travail de la lignée Wine et FEX sur Darwin. Il fixe ce qui est établi, ce qui est choisi, et comment on le vérifie. Les rapports détaillés sont dans le même dossier. Chaque affirmation renvoie à un rapport ou à un journal ; ce qui n'est pas mesuré est dit.

## 1. Faits établis

1. Le runtime installé est entièrement x86_64 sous Rosetta. Aucun PE arm64, aucune tranche arm64 de D3DMetal (`wine-macos-bridge.md` §0 points 1 et 3).
2. Sous Rosetta, un `syscall` brut de numéro Linux lève SIGSYS. Wine macOS le redirige vers son dispatcher NT, et les numéros Linux recouvrent les identifiants NT (`wine-macos-bridge.md` §0 point 5). Le bootstrapper PE d'EAC émet lui-même des `syscall` Linux (319, 1) depuis son image. Le module EAC doit donc vivre dans un processus qui offre une vraie personnalité Linux.
3. Un processus arm64 macOS ne mappe rien sous 4 Gio. Réduire `__PAGEZERO` fait refuser l'exécution par le noyau. Wine a besoin de 0x7ffe0000 (`USER_SHARED_DATA`) et son préchargeur de 0x10000-0x68000000 et 0x7f000000-0x7fff0000 (`fexcore-darwin-port.md`, `wine-macos-bridge.md` §3). Un invité à adresse identique à l'hôte ne peut pas héberger Wine.
4. L'espace utilisateur arm64 va jusqu'à 0x7ffffe000000 ; une réservation `PROT_NONE` de 2^46 octets réussit, 2^47 échoue ; l'espace libre est surtout en haut (`fexcore-darwin-port.md`).
5. Pages hôte de 16 Kio ; `mmap`, `munmap` et `mprotect` arrondissent à 16 Kio et refusent une adresse non alignée ; un offset de fichier de 0x1000 donne EINVAL (`hl-reuse-core.md` annexe A). Seule une cible x86_64 sous Rosetta obtient des pages de 4 Kio (`posix_spawnattr_set_4k_page_size_np` répond EBADMACHO pour arm64, `darwin-memory-facts-a.md`).
6. `x18` est remis à zéro par le noyau : le JIT ne peut pas l'utiliser (`fexcore-darwin-port.md`).
7. FEXCore 0df84d3 se compile en bibliothèque statique sur macOS arm64 avec 13 fichiers modifiés (`fexcore-darwin-port.md`, jalon 1). Aucun témoin n'a encore été exécuté au moment de cette fiche.
8. HyperLinux compte 40 617 lignes de C. 25 objets sur 39 n'ont aucun symbole HVF. Un banc a exécuté ses vrais objets vfs, fd et procfs dans un processus macOS ordinaire : 23 contrôles sur 23, sans VM (`hl-reuse-core.md` résumé 3). Sa relation ptrace tracer/tracee en espace utilisateur (`proc_ptrace.c`) passe 79 contrôles sur 79 sur l'hôte (`hl-reuse-process.md` résumé 4).
9. Le `fork()` natif de macOS 27 tient dans un processus multithread avec `MAP_JIT` (17 PASS), avec des verrous orphelins à gérer (`hl-reuse-process.md` résumé 7).
10. Verdicts EAC : le seul environnement non physique accepté (code 301) est QEMU système x86 TCG, sans authentification, en 3,5 s ; toutes les piles ARM donnent 509/210 ou 502 « Cannot run under Virtual Machine ». L'absence d'authentification est une cause close. Un lancement du bootstrapper sans identifiants est donc un discriminateur valable (`eac-controls-digest.md` §0).
11. Contrat observé du module : fork depuis un thread worker, `PTRACE_TRACEME`, `SETOPTIONS(0x11)`, `PTRACE_SYSCALL`, `GETREGSET NT_PRSTATUS` de 216 octets, `GETSIGINFO`, exec du loader Wine par l'enfant, `INT3` à l'entrée de l'image, `POKE` et `SETREGSET` pour injecter un `dlopen("/proc/self/fd/N")`, `SIGSEGV` attendu à l'adresse 9 (`eac-probes-digest.md` §0 points 2 et 5, §6).
12. Wine 11 de voile-wine contient déjà la plomberie ARM64EC et une simulation de pages de 4 Kio sur hôte de 16 Kio ; l'interface `xtajit64` de Wine est identique, nom pour nom, à celle de `libarm64ecfex.dll` de FEX (`wine-macos-bridge.md` §4).

## 2. Options comparées

| Option | Principe | Verdict |
| --- | --- | --- |
| O1 | Wine macOS sous Rosetta, module ELF dans un processus FEX séparé | Rejetée. Le bootstrapper charge le module dans son propre processus par des `syscall` bruts qu'aucun pont externe ne voit, et le module doit tracer le vrai processus du jeu. |
| O2 | Wine ARM64EC natif sur macOS, FEX comme émulateur ARM64EC, Linux de poche dans le processus pour EAC | Gardée comme optimisation ultérieure. Meilleur chemin graphique et de performance, mais demande de porter Wine arm64 sur macOS (`x18`, zone basse, 16 Kio), de loger glibc dans le processus, et d'aiguiller deux personnalités. |
| O3 | Userland Linux x86-64 complet (Wine Linux, glibc, module EAC) sur FEXCore dans un processus macOS arm64, avec une couche noyau Linux-sur-Darwin | Retenue. Une seule personnalité, fidélité Linux par construction, preuve d'existence dans le chemin QEMU. |
| O4 | HyperLinux (Hypervisor.framework par processus) | Exclue tant que l'utilisateur n'a pas accepté la virtualisation par processus. Lue seulement pour ses contrats. |

## 3. Décision

O3, avec ces choix :

1. Moteur : FEXCore porté sur Darwin. Les accès mémoire du code invité ajoutent un registre de base : adresse hôte = `GUEST_BASE` + adresse invitée. L'hôte réserve la fenêtre invitée une fois (PROT_NONE, MAP_NORESERVE, au plus 2^46 octets) et la valide par morceaux. Tous les déréférencements de pointeurs invités par FEXCore et par la couche noyau passent par une fonction unique.
2. Pages : l'invité voit des pages de 4 Kio. Une table de VMA garde la vue exacte à 4 Kio ; la protection hôte d'une page de 16 Kio est le OU des protections de ses sous-pages ; les mappings de fichiers à offset non aligné sont copiés en mémoire anonyme pour le cas privé. FEXCore ignore le bit d'exécution invité, ce qui supprime le problème W^X des unions RWX.
3. Couche noyau (nom de travail LKD) : une couche syscall x86-64 neuve, appelée par le moteur à chaque `syscall` invité. Modules repris de HyperLinux : vfs enraciné, objets fd, procfs, noms de tâches, traductions de drapeaux et d'errno, relation ptrace tracer/tracee en espace utilisateur. Éléments repris de FEX : structures x64, cadres de signal x86-64, vDSO, suivi des VMA. On ne compile pas la couche `LinuxEmulation` de FEX sur une bibliothèque « noyau ARM64 » (recommandation de `hl-reuse-core.md`, contre l'avis du mode A de `hl-reuse-process.md` ; le mode A reste la repli si l'écriture des conversions x86-64 s'avère plus chère que prévu).
4. Processus : un processus invité est un processus hôte. `fork` invité = `fork` hôte avec le protocole de verrous de FEX. `exec` invité = réexécution du runtime. PID et TID viennent d'un compteur partagé de la famille, le TID du thread principal égale le PID.
5. Signaux : files logicielles par tâche et boîte aux lettres inter-processus ; un seul signal hôte réservé interrompt le code JIT ; les fautes synchrones sont traduites en signaux invités avec l'adresse exacte.
6. ptrace : émulation coopérative en espace utilisateur entre deux processus du runtime. L'état x86 réel se trouve dans `CPUState` de FEXCore, donc la vue de 216 octets est exacte par construction. Les écritures de code invalident le cache de traduction du tracee avant sa reprise.
7. Graphique : client Neptune dans Wine Linux, transport vtest, rendu dans un processus macOS séparé (D3DMetal-native sous Rosetta ou DXMT-native arm64). La comparaison des deux se fait avec des témoins avant le jeu.
8. Affichage, entrées, son : pilote Wine à écrire qui parle à l'hôte macOS par socket, ou serveur X minimal ; à trancher par les témoins de `graphics-display-audio.md`.
9. Aucune réponse fabriquée : chaque réponse de `/proc`, de ptrace ou de syscall provient d'un état réel du runtime. Les fonctions non implémentées répondent ENOSYS de façon visible.

## 4. Interfaces

Frontière étroite entre moteur et noyau, reprise de `hl-reuse-process.md` §3.2 : accès mémoire invitée (`user_read`, `user_write`, `user_strn`), entrée de syscall, livraison de signaux aux points sûrs, rapport de fautes, services du moteur (registres, FP, invalidation de code, spawn de thread, construction et restauration de cadre), points de fork et d'exec, arrêts ptrace. Le fichier d'en-tête de référence est `src/lkd/include/lkd/lkd.h`.

## 5. Échelle de qualification

Chaque palier a des programmes possédés et des oracles enregistrés (jamais QEMU relancé). Un palier n'est franchi que si la sortie correspond à l'oracle.

1. ELF x86-64 statique sans libc : chargement, syscalls directs, TLS par `arch_prctl`, code automodifiable, faute invitée.
2. ELF statique avec libc (musl puis glibc) : malloc, threads, signaux, fichiers.
3. Dynamique glibc depuis le sysroot Debian : `ld.so`, `/bin/sh`, outils de base.
4. Processus : fork, exec, wait, pipes, sockets Unix, futex partagé, signaux entre processus.
5. Wine `--version`, puis `wineboot --init`, puis `cmd /c echo`, puis un PE possédé : premier témoin de franchissement PE, ELF, macOS.
6. ptrace x86 : les témoins existants (76 contrôles de l'oracle Linux x86) puis le contrat EAC.
7. Bootstrapper EAC sans identifiants : code 301 contre 509/210.
8. Graphique, entrées, son : témoins séparés avant le jeu.
9. Jeu protégé : session authentifiée, partie complète, reconnexion, FPS et temps de frame.

## 6. Risques

1. Décalage de base dans le JIT : modification de nombreux sites mémoire de FEXCore (atomiques, vecteurs, `rep movs`). Lever : estimation par l'agent FEX, puis implémentation derrière une option.
2. 4 Kio sur 16 Kio : garde-fous Windows et mappings fichiers à offsets de 4 Kio. Lever : témoins de pages sur Wine réel.
3. Signaux asynchrones et boîte aux lettres inter-processus (Wine server envoie SIGUSR1 aux clients).
4. Futex partagé entre processus, eventfd et epoll sur kqueue (kqueue n'est pas héritée par fork).
5. ptrace coopératif et cohérence du cache de traduction après POKE.
6. Verdict EAC : ARM n'a jamais produit 301 ; les critères de refus de VM sont inconnus. Lever : mesurer CPUID, `cpuinfo`, DMI et vecteur auxiliaire de l'environnement, sans masquage de l'identité de FEX.
7. Performances : FEX sans TSO matériel. Mesurer tôt sur des microprogrammes puis sur Wine.

## 7. Questions pour l'utilisateur

1. Si le mur des 4 Kio ou des 16 Kio bloquait Wine de façon irréparable, accepte-t-il Hypervisor.framework par processus comme repli ? Non demandé tant qu'aucun témoin n'établit ce blocage.
2. Jeton Epic : le classifieur de permissions a refusé la lecture du trousseau. Il n'est requis qu'à la fin (session protégée). L'utilisateur peut écrire lui-même le fichier avec la commande `!` indiquée dans la conversation, ou lancer le jeu depuis SailOver avec un moteur de laboratoire.
