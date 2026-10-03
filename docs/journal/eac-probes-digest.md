# Digest des sondes EAC : bootstrapper et module Linux genuine sous Wine

Rédigé le 2026-10-02. Lecture seule des sondes existantes. Aucun lancement du jeu, d'EAC, de QEMU ou d'UTM. Une seule écriture : ce fichier.

## Conventions

Chemins abrégés :

- `P/` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928/probes`
- `R/` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928`
- `D/` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/diagnostics` (retours du PC Bazzite, de WSL2 et du PC Windows)
- `GR` = `P/eac-general-regset-20261001/run-20261001T102102834594Z/eac.log` : la trace la plus avancée du module (runtime HyperLinux `f3f7ca30`, 2026-10-01 10:21Z, 63 463 lignes). Les numéros de ligne `GR:n` y renvoient.
- `LOG-REGSET` et `LOG-STATUS` = `R/run/logs/eac-parent-ptrace-regset.log` et `R/run/logs/eac-parent-proc-status.log` (traces syscall complètes de HyperLinux, 35 Mo chacune, numérotation ARM64 des syscalls).

Étiquettes de statut (les quatre imposées par la tâche) :

- `observed on real Linux x86` : deux sources distinctes, toujours qualifiées. (a) PC physique : Dell XPS 15 9500 sous Bazzite, noyau 6.18.52, `systemd-detect-virt` = none (`D/rocket-bazzite-return-20260929/ANALYSE.md:31`). (b) Noyau Linux x86 6.12.107+deb13-amd64 sous QEMU TCG : sûr pour la sémantique du noyau, ce n'est pas du matériel x86 (`P/ptrace-x86-full-context-control-20260929/README.md:7`).
- `observed under HyperLinux/Rosetta` : trace HyperLinux (HL) ou Rosetta sur noyau Linux ARM64 réel d'OrbStack (RLX). Dans ce second cas le noyau est réel mais la vue ptrace est celle d'un ARM64.
- `observed under FEX` : aucune observation de ce type n'existe dans ces sondes. FEX n'y est lu que statiquement (`P/fex-eac-feasibility.md`).
- `inferred` : déduction de l'auteur de ce digest, avec les éléments qui la fondent.

Fait structurant : aucune trace des syscalls d'EAC n'a été prise sur un Linux x86 réel. Sur Linux x86 réel on possède (1) les journaux du bootstrapper et du jeu (PC physique), (2) les cartes mémoire et la liste des processus de la session protégée de 91,54 s sous QEMU TCG, (3) des témoins possédés dont la sémantique ptrace a été mesurée sous QEMU TCG et, pour deux sondes, sur le PC physique. L'ordre exact des requêtes d'EAC n'est connu que par les traces HyperLinux/Rosetta, jusqu'au point où EAC abandonne. Tout ce qui suit cet abandon est inféré.

## 0. Lecture rapide pour la décision d'architecture

1. Le bootstrapper PE (`RocketLeague_EAC.exe` 1.9.3) choisit `linux64` sous Wine, télécharge un module encodé, le décode dans un memfd, retrouve libc en analysant `/proc/<pid>/maps`, appelle `dlopen("/proc/self/fd/N")` dans son propre processus, puis laisse le module ELF x86-64 (glibc, 230 imports libc) faire le reste (`R/linux-reference/eac-static-210/analysis.md:15-19`, `maps-resolver.md:5-9`).
2. Le module forke depuis un thread worker. L'enfant s'auto-trace (`PTRACE_TRACEME`), crée un memfd de 7 757 608 octets sans `CLOEXEC`, relit `RocketLeague.exe`, puis ré-exécute le loader Wine (cible de `readlink("/proc/self/exe")`). Le thread parent trace l'exec, suit les syscalls du préloader jusqu'à `prctl(PR_SET_NAME)`, pose un `INT3` sur le point d'entrée du programme principal, lit la mémoire du tracee pour résoudre une fonction de libc, écrit la chaîne `/proc/self/fd/34`, réécrit les registres (rdi, rsi=1, rdx=0, rsp, rip) et reprend avec une adresse de retour égale à 9. Lecture : injection d'un `dlopen` du second ELF dans le futur processus du jeu (`inferred`, voir §3.4).
3. Sur le PC physique le flux complet dure environ 0,5 s : `Starting Wine module mapping` à 11:51:52.026, launcher 301 à 11:51:52.546, `Terminating with: 0` à 11:51:52.570. Le bootstrapper sort ensuite ; le jeu est réparenté à `pv-adverb` (`D/rocket-bazzite-return-20260929/received/journaux/essai-01/eac-bootstrapper.log:22-27`, `received/chronologie/process-tree-during-match.json`, `received/chronologie/lifecycle.jsonl:1`). Sous QEMU TCG le processus du jeu contient le memfd de 7,76 Mo mappé comme ELF (`P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/outputs/game-process-maps.txt:1089-1096`). La relation ptrace est donc de durée courte, limitée au lancement (`inferred` pour la durée exacte).
4. EAC demande `NT_PRSTATUS` avec `iov_len` = 216, soit 27 mots de 8 octets : la taille de `struct user_regs_struct` x86-64. Sous Rosetta sur noyau ARM, le noyau renvoie le préfixe de 216 octets d'un contexte ARM64 de 272 octets (`P/regset-architecture.md:3,9`). Il faut donc une vue x86-64 exacte, avec RIP, RSP, FS_BASE, RFLAGS, `orig_rax` et RCX/R11 justes (`P/ptrace-x86-feasibility-20260929.md:80-89`).
5. Écriture de registres obligatoire : EAC fait `SETREGSET` (RIP moins 1 après `INT3`, puis injection d'appel) et lit aussi `NT_PRFPREG` (512 octets). Écriture mémoire obligatoire : `POKEDATA` de code exécutable (`INT3`, puis restauration) dans le tracee, avec invalidation du cache de traduction avant reprise. Rosetta échoue ici (traduction périmée), c'est le blocage le plus récent de HyperLinux (`P/ptrace-remote-setregset-runtime-20261001/README.md:32-45`).
6. EAC lit `/proc/<tracee>/maps` d'un autre processus (stat, open, read) et, avant cela, les fichiers `/proc` de son propre parent. Un Wine sans parent (PID 1, PPid 0) produit `502 / Launch Error: 256` en 39 ms même sur noyau réel (`R/linux-reference/pid1-control/README.md`).
7. Le module refuse explicitement une VM : WSL2 renvoie `502 / Cannot run under Virtual Machine.` 19 ms après le début du mapping (`D/rocket-wsl-eac-return-20260929/ANALYSE.md:3,21-27`). QEMU TCG (`-cpu max`, machine `pc`, TCG) est accepté. Les critères de détection sont inconnus.
8. Aucun run n'a dépassé 509/210 hors PC physique. Le dernier état HL : l'injection est préparée et lancée, le second arrêt arrive comme `SIGTRAP` au lieu d'un `SIGSEGV` attendu à l'adresse 9 (inféré), EAC tue le tracee (`P/ptrace-general-regset-runtime-20261001/README.md:9`).
9. Réutilisable sans QEMU : une trentaine de témoins freestanding x86-64 (ELF statiques déjà compilés, source C ou assembleur) et les sorties de l'oracle Linux x86 enregistrées (§5).
10. FEX Linux refuse `TRACEME`, `SETOPTIONS`, `SYSCALL`, `GETREGSET` (EPERM) et ne transmet que PEEK/POKE/ATTACH/DETACH (`P/fex-eac-feasibility.md:7,13`, révision `0df84d3`). Un port Darwin devra fournir toute la relation de traçage x86 (§6).

## 1. Flux de lancement du bootstrapper genuine sous Wine sur Linux

### 1.1 Qui fait quoi

| Composant | Rôle observé ou lu | Preuve | Statut |
| --- | --- | --- | --- |
| Bootstrapper PE `RocketLeague_EAC.exe` 1.9.3 (3 975 920 octets, SHA-256 `903e7aba…14d1`) | Lit `EasyAntiCheat\Settings.json`, journalise `System name: 'linux64'`, télécharge `https://modules-cdn.eac-prod.on.epicgames.com/modules/<ProductId>/<DeploymentId>/linux64`, décode, charge le module, relaie les callbacks. | `R/linux-reference/eac-static-208/anticheatlauncher.log:2-8,30-33,94-100` | observed under HyperLinux/Rosetta (RLX), identique sur PC (`D/rocket-bazzite-return-20260929/received/journaux/essai-01/eac-bootstrapper.log:17-27`) : observed on real Linux x86 (PC) |
| Pont SysV du PE | Adapte l'ABI SysV vers Windows pour le callback (`0x14004ba20`) ; `0x14001cc10` écrit les octets dans un descripteur Linux, formate `/proc/self/fd/%i` et appelle le loader natif via `__libc_dlopen_mode`/`dlopen` puis `__libc_dlsym`/`dlsym`. Le code PE exécute donc des appels SysV natifs dans le même processus. | `R/linux-reference/eac-static-210/analysis.md:15-18` | inferred (lecture statique du PE) |
| Résolveur libc du PE | `getpid`, lit `/proc/%i/maps` par blocs de 4096 octets jusqu'à 5 Mio, découpe lignes et champs, retient la première ligne contenant `/libc-` ou `libc.so` (sans casse), calcule `début + offset fichier`, exige l'en-tête ELF (`e_machine=62`, `e_type=3`, `e_phentsize=56`) et un `PT_DYNAMIC`. Cache initialisé une fois. | `R/linux-reference/eac-static-210/maps-resolver.md:5-9` | inferred (statique) ; échec observé sous HL (507/206) tant que libc manquait de `/proc/self/maps` (`docs/research/rocket-league-macos.md:142`) |
| Module téléchargé | Fichier CDN non ELF. Relation `encoded[i] = (plain[i] + plain[i+1] + 3*i) mod 256` (octet suivant nul après la fin). Ancien : 9 681 573 octets, SHA `ce28ef66…`, ELF décodé SHA `c6bf6c7d…`. Courant : 13 825 695 octets, SHA `74b6cab9…`, ELF décodé SHA `1b9faa54…` (ET_DYN x86-64, 11 PT_LOAD, 33 sections, exports `a b c d e`). | `P/eac-current-module-static-20260929/README.md:7-11` ; `P/eac-native-module-capture-plan.md:7-9` | observed on real Linux x86 (PC : 13 825 695 octets reçus) ; décodage vérifié hors exécution |
| Module ELF dans le processus Wine du bootstrapper | Thread worker (TID 76 dans `GR`, 71/72 dans les runs de 2026-09-28/29). Sondes d'environnement, fork, rôle de traceur, réseau. | `P/eac-parent-status-transition/README.md:49-56` ; `GR` | observed under HyperLinux/Rosetta (HL) |
| Enfant forké avant exec | `prlimit64`, `prctl(PR_SET_DUMPABLE,0)`, `PTRACE_TRACEME`, `tgkill(self,self,SIGSTOP)`, crée et remplit un memfd, relit le jeu, `readlink`, `execve` du loader Wine. | §2.4, §2.5 | observed under HyperLinux/Rosetta (HL) |
| Processus après exec | Nouveau processus Wine (`wine-preloader` puis `wine64`), tracé, qui deviendra `RocketLeague.ex`. | `GR:61377-61403` ; `D/rocket-bazzite-return-20260929/received/rapport.md:72` (nom `wine64` puis `RocketLeague.ex`) | observed under HyperLinux/Rosetta (HL) pour l'exec ; observed on real Linux x86 (PC) pour le nom final |
| Second ELF (memfd 7 757 608 octets) | Mappé comme ELF dans le jeu : segments r--p, r-xp, r--p, garde, r--p, rw-p, puis r-xp à l'offset 0x21f000 (fin 0x764000). | `P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/outputs/game-process-maps.txt:1089-1096` | observed on real Linux x86 (QEMU TCG) pour le mapping ; inferred pour l'origine (même taille que l'écriture de l'enfant : 0x765f28 ≈ 0x764000 + queue de fichier) |
| Pont ELF Proton/Heroic (`easyanticheat_x64.so`, `PROTON_EAC_RUNTIME`) | Absent du bootstrapper EOS 1.9.3 (chaînes introuvables en ASCII et UTF-16). Les A/B avec et sans composant Heroic donnent tous deux 509/210. Aucun chargement observé. | `P/eac-proton-runtime.md:7` ; `P/eac-heroic-bridge-ab-20260929/RESULTATS-HEROIC.md:3-12,28` | observed under HyperLinux/Rosetta (RLX) |

### 1.2 Chronologie observée

Les horodatages viennent du journal du bootstrapper ; les déroulés ptrace viennent de `GR`.

1. Le PE charge `Settings.json`, affiche `Loader initializing with bootstrapper version 1.9.3, architecture: x86-64`, puis se connecte au CDN (`eac-static-208/anticheatlauncher.log:30-33`). PC : HTTP 200, 13 825 695 octets en 534 ms (`essai-01/eac-bootstrapper.log:26`). `observed on real Linux x86 (PC)`.
2. Wine charge `secur32`, `kerberos`, `netapi32`, `dnsapi`, `iphlpapi`, `nsi`, `rsaenh`, `schannel` côté Unix (`GR:59092-61336`) pour la pile TLS du téléchargement. `observed under HyperLinux/Rosetta (HL)`.
3. Le PE exécute dans cet ordre (trace complète HL) : `memfd_create(name, 2)` (ligne 1011659), `write(fd, buf, 9 681 573)` du module ancien (1011712), `getpid` (1011722), ouverture de `/proc/<pid>/maps` et lecture jusqu'à EOF (1011726 et suivantes ; `R/linux-reference/eac-static-210/maps-resolver.md:5`), tous dans `LOG-REGSET`. `observed under HyperLinux/Rosetta (HL)`.
4. `dlopen("/proc/self/fd/N")` : lien de carte `link map` de base 0x7fffef6c2000, taille 0x93d650, 12 program headers, `calling init`, ouverture réussie (`R/linux-reference/loader-trace/ge-eac.14:965-973`, décrit dans `eac-ge-loader-analysis.md:9-14`). L'en-tête de 64 octets du memfd commence par `7f454c46 02 01 01`, type 3, machine 62. `observed under HyperLinux/Rosetta (RLX)`.
5. Le PE résout les exports `a`, `b`, `c`, `d`, passe un pointeur de callback (`0x140034530`) et appelle l'export natif (`0x1400347b8`) (`eac-static-210/analysis.md:18`). `inferred` (statique).
6. Sondes d'environnement du module dans le processus (§2.3), puis `fork` (`clone` 0x1200011).
7. Le tracer attend le stop initial `SIGSTOP`, installe les options 0x11 et suit les syscalls de l'enfant (§3.2). Fenêtre mesurée entre le début du mapping et le callback sous HL : 0,7 à 0,9 s (`P/eac-ptrace-syscall-transition/report.md:26`, `P/eac-ptrace-regset-transition/report.md:43`).
8. PC : `Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game'`, 520 ms après le début du mapping, puis `waiting for game window`, `Terminating with: 0` 24 ms plus tard (`essai-01/eac-bootstrapper.log:22-27`). `observed on real Linux x86 (PC)`.
9. Échecs observés ailleurs : 507/206 (libc non trouvée), 502/208 `Launch Error: 256` (parent illisible), 502/208 `Cannot run under Virtual Machine.` (WSL2), 509/210 `Unexpected error. (#1)` (tous les runs Mac). Table des événements : le commutateur `0x140030c10` soustrait 501, 507 vers 206, 509 vers 210 (`eac-static-210/analysis.md:15-16`).

### 1.3 Arbre de processus selon l'environnement

| Environnement | Arbre | Preuve | Statut |
| --- | --- | --- | --- |
| HL, run `GR` | PID 1 = shell `dash` (parent de Wine), PID 2 = Wine du bootstrapper (`wine-preloader`), TID 76 = thread traceur, PID 80 = tracee (feuille, leader), wineserver et services en PID 3 à 70. Parent = worker, enfant = leader. | `GR:61379` (stat `/usr/bin/dash`), `P/eac-cachyos-tgkill-20260930/next-controls-source-provenance.md` (section Séquence actuelle) | observed under HyperLinux/Rosetta (HL) |
| RLX Cachy, 509/210 | bootstrapper PID 2863, `RocketLeague.ex` PID 2881 de parent 2863 ; ce processus exécute le code de démarrage du jeu (noms de thread `PoolThread`). | `P/eac-cachyos-process-trace-20260929/RESULTATS.md:9-21` | observed under HyperLinux/Rosetta (RLX) |
| PC Bazzite | `pv-adverb` (9065) est parent à la fois de `wineserver` (9161) et de `RocketLeague.ex` (9268). Le bootstrapper (8539) était déjà sorti. | `D/rocket-bazzite-return-20260929/received/chronologie/process-tree-during-match.json`, `lifecycle.jsonl:1` | observed on real Linux x86 (PC) |
| QEMU TCG, session protégée | Un seul `RocketLeague.ex` (PID 347) à 684 s avec `wineserver`, `services.exe`, `winedevice.exe`, `explorer.exe`. `launcher_code: 0`. | `P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/outputs/process-status.json` | observed on real Linux x86 (QEMU TCG) |

Conséquence (inferred) : le tracee devient indépendant quand le bootstrapper sort. Aucune trace ne montre de lien ptrace persistant pendant la partie ; aucune lecture de `TracerPid` du jeu n'existe.

### 1.4 Ce qui n'est pas établi

- L'ordre des requêtes ptrace d'EAC sur Linux x86 réel (jamais tracé).
- Le contenu du second ELF avant écriture dans le memfd ; l'argv et l'environnement transmis à l'`execve` (non journalisés).
- Le mécanisme de détection de VM, la raison de 509/210, la fonction exacte appelée par l'appel injecté (identifiée seulement par adresse).
- La hypothèse selon laquelle le nom du loader décide de la suite : voir §3.7.

## 2. Opérations observées, par composant

Convention : colonne `Oracle` = fichier qui contient la sémantique du noyau Linux x86 pour cette opération, quand elle existe (sinon `aucun`). Les numéros de syscall sont ceux de Linux x86-64. Les traces HL sont en numérotation ARM64 ; la correspondance a été faite ici.

### 2.1 Bootstrapper PE, étape de chargement du module

| # | Opération | Motif d'arguments | Résultat attendu ou observé | Composant | Statut | Preuve et oracle |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | `newfstatat`/ouvertures de chemin | `/game/Binaries/Win64/RocketLeague.exe.eac` (puis la variante `…exe.eac?`) et le répertoire `/game/Binaries/Win64` | résultat non journalisé ; aucun fichier `.eac` n'est conservé dans le préfixe (`payloads: []`) | PE | observed under HyperLinux/Rosetta (HL) | `GR:59071-59089` ; `P/eac-current-payload-20260929/README.md:20`. Nature (cache du module) : inferred |
| 1 | `getpid` (39) | aucun | pid du processus Wine | PE | observed under HyperLinux/Rosetta (HL) | `LOG-REGSET:1011722` ; `maps-resolver.md:5`. Oracle : aucun |
| 2 | `openat` (257) | `AT_FDCWD`, `/proc/<pid>/maps`, `O_RDONLY` | fd ; contenu Linux (≥6 champs par ligne, champ 0 `début-fin`, champ 2 offset hexadécimal, chemin dès le champ 5) | PE | observed under HyperLinux/Rosetta (HL) | `LOG-REGSET:1011726-1011727` (63 226 octets rendus par HL) ; `maps-resolver.md:5-9`. Oracle : sémantique de maps distante dans `P/ptrace-remote-maps-control-20261001/qemu-20261001T000419470178Z` |
| 3 | `read` (0) puis `close` (3) | blocs de 4096 octets, plafond 5 Mio | lit jusqu'à EOF ; libc en premier segment (offset 0) | PE | inferred (statique) | `maps-resolver.md:5,7` |
| 4 | `memfd_create` (319) | nom aléatoire, flags `2` (`MFD_ALLOW_SEALING`) | fd ≥ 3 (32 dans cette trace) | PE | observed under HyperLinux/Rosetta (HL) | `LOG-REGSET:1011659` (`syscall 279(…, 0x2, …)`) |
| 5 | `write` (1) | fd du memfd, buffer décodé, 9 681 573 octets (ancien) ou 13 825 695 (courant) | octets écrits en une fois | PE | observed under HyperLinux/Rosetta (HL) | `LOG-REGSET:1011712` (`syscall 64(0x20, …, 0x93baa5)`) ; `GR:61343` (`mapped_elf_sha256`, `file_size` 13 825 695) |
| 6 | `dlopen("/proc/self/fd/N")` | `RTLD_*` non journalisé | libc résout le lien magique, `openat` + `fstat` + `mmap` PROT_EXEC privé du memfd, `mprotect`, init | PE via libc | observed under HyperLinux/Rosetta (RLX) | `R/linux-reference/loader-trace/ge-eac.14:965-973`. `GR:61343` : fd invité 34, hôte 56, offset 0, `nlink` 0 |
| 7 | `dlsym` (aucun syscall) | `a`, `b`, `c`, `d` | adresses d'exports | PE | inferred (statique) | `eac-static-210/analysis.md:18` |

### 2.2 Module, premières sondes d'environnement (processus principal du bootstrapper)

Toutes les lignes de cette table sont `observed under HyperLinux/Rosetta (HL)`. Les fichiers procfs de HL sont synthétisés par l'émulateur ; leurs octets ne sont pas ceux d'un noyau.

| # | Opération | Motif d'arguments | Résultat observé | Preuve |
| --- | --- | --- | --- | --- |
| 8 | `getppid` (110) | aucun | 1 | `P/eac-parent-status-transition/after-status-context.txt:1014804-1014805` |
| 9 | `openat` + `read` + `close` | `/proc/<pid propre>/status`, lecture de 4096 octets | 166 octets rendus par HL | `P/eac-parent-proc-comparison-after-status-final/windows.txt` (ligne d'origine 1062313-1062322) |
| 10 | idem | `/proc/<ppid>/comm`, lecture de 31 octets | 3 octets | idem, 1062383-1062387 |
| 11 | `readlinkat` (267) | `/proc/self/exe`, tampon de 4096 | 37 octets (Debian : `/usr/lib/wine/wine64-preloader.static`) ou 56 octets (GE : `…/x86_64-unix/wine-preloader`) | idem, 1062446-1062448 ; `P/eac-readlink-boundary-20260929/README.md:3` |
| 12 | `readlinkat` | `/proc/<ppid>/exe` puis `/proc/<pid>/exe`, puis `newfstatat` (262) sur chaque cible | 13 octets (`/usr/bin/dash`) ; 37 ou 56 octets ; stat 0 | idem, 1062453-1062467 ; `GR:61379` |
| 13 | `openat` + `read` + `close` | `/proc/<ppid>/cmdline`, 31 octets | 31 octets (ENOENT avant le correctif bb2f) | idem, 1062476-1062480 ; `P/eac-parent-proc-comparison.md:11-17` |
| 14 | `openat` + `read` + `close` | `/proc/<ppid>/status`, 31 octets | 31 octets (ENOENT avant le correctif de 'status') | `after-status-context.txt:1014815-1014819` ; `windows.txt` 1062511 |
| 15 | `openat` | `/tmp/eac.flag` | ENOENT, sans effet connu | `windows.txt` 1062538 ; `P/eac-parent-proc-comparison.md:18,22` |
| 16 | `openat` + `close` | `/proc/self/maps` | fd 131 puis fermé ; 67 510 octets générés, aucune lecture vue sur cet open | `after-status-context.txt:1015491-1015495` |
| 17 | `newfstatat` | chemin non journalisé | ENOENT | `after-status-context.txt:1015549-1015550` |
| 18 | `openat` | `/sys/devices/virtual/dmi/id/board_name` | ENOENT | `after-status-context.txt:1015616` |
| 19 | `openat` | `/sys/devices/virtual/dmi/id/product_name` | ENOENT | `after-status-context.txt:1015646` |
| 20 | `openat` | `/proc/bus/pci/devices` | ENOENT | `after-status-context.txt:1015664` |
| 21 | parcours de chemin | `stat` de `/opt`, `/opt/ge-proton`, …, `…/wine-preloader`, `readlink` des composants (EINVAL attendu pour un non-lien) | succès | `GR:61387-61399` ; `P/eac-parent-status-transition/README.md:25-26` (`LOG-STATUS:1015725-1015756`) |
| 22 | `openat` | `/proc/mounts` (attribution au module non établie : Wine lit aussi ce fichier) | ouvert | `GR:61385` |
| 23 | `openat` | `/usr/lib/x86_64-linux-gnu/gconv/UTF-32.so` (conversion `iconv`) | ouvert | `GR:61401` |
| 24 | `openat` | `<dirname de /proc/self/exe>/EasyAntiCheat\Certificates\runtime.conf` (séparateurs backslash littéraux) | ENOENT sous HL ; ouvert avec un overlay du fichier du jeu, sans changer 509/210 | `LOG-STATUS:1016121` ; `GR:61403` ; `P/eac-runtime-conf-availability-20260929/README.md:15-24` |
| 25 | `newfstatat` | `/game/Binaries/Win64/RocketLeague.exe` | succès | `GR:61345` |

Les chaînes `hidepid`, `debugger` et `VM` existent dans le module sans preuve que ces branches soient prises (`R/linux-reference/umu/eac-native-elf-capture/README.md:43`).

### 2.3 Fork et enfant avant exec

| # | Opération | Motif d'arguments | Résultat observé | Composant | Statut | Preuve et oracle |
| --- | --- | --- | --- | --- | --- | --- |
| 26 | `clone` (56) = `fork` | flags `0x1200011` (`CHILD_SETTID` + `CHILD_CLEARTID` + `SIGCHLD`) | pid enfant (75, 80, selon le run) | MOD, thread worker | observed under HyperLinux/Rosetta (HL) | `P/eac-ptrace-target-role-20260929.md:9` ; `P/eac-ptrace-transition/report.md:9` |
| 27 | `wait4` (61) | `(enfant, &status, 0, NULL)` appelé avant que l'enfant n'ait démarré | bloque jusqu'au stop `0x137f` | MOD | observed under HyperLinux/Rosetta (HL) | `P/eac-ptrace-transition/report.md:9` ligne 1014380 ; oracle : `P/ptrace-signal-two-int3-control-20260930/qemu-20260930T142310221768Z/manifest.json` (`first real SIGSTOP` = 4991) |
| 28 | `prlimit64` (302) | non décodé | 0 | enfant | observed under HyperLinux/Rosetta (HL) | `P/eac-parent-status-transition/README.md:61-62` |
| 29 | `prctl` (157) | `PR_SET_DUMPABLE` (4), 0 | 0 | enfant | observed under HyperLinux/Rosetta (HL) | `P/eac-parent-status-transition/README.md:62-63` ; sémantique dumpable : `P/ptrace-peek-control-20260930/README.md` (QEMU 375/375) |
| 30 | `ptrace` (101) `PTRACE_TRACEME` | `(0,0,0,0)` | 0 | enfant | observed under HyperLinux/Rosetta (HL) | `GR:61407` ; oracle : tous les `ptrace-*-control-*` |
| 31 | `gettid` (186), `getpid` (39) | aucun | tid, pid de l'enfant | enfant | observed under HyperLinux/Rosetta (HL) | `P/eac-x86-ptrace-exec-diagnostic-20260929/README.md:7` ; `GR` (clobber seq 6-7) |
| 32 | `tgkill` (234) | `(pid, tid, SIGSTOP=19)` sur soi-même | 0 ; l'enfant s'arrête, stop de livraison de signal, statut `0x137f` | enfant | observed under HyperLinux/Rosetta (HL) | `P/eac-ptrace-options-transition/report.md:11` ; oracle : `P/ptrace-x86-tgkill-context-control-20260929/results-20260929T165852960827Z` (272/272, cinq stops, aucun stop de sortie de tgkill avant le premier stop de syscall) |
| 33 | `mmap` (9) | `NULL, 0x766000, PROT_READ\|PROT_WRITE, MAP_PRIVATE\|MAP_ANONYMOUS, -1, 0` | tampon pour 7 757 608 octets arrondis | enfant | observed under HyperLinux/Rosetta (HL) | `GR` regset stop_seq 2 (rsi 0x766000, rdx 3, r10 0x22) |
| 34 | `memfd_create` | nom, flags `2` | fd 33, 34 ou 35 selon le run | enfant | observed under HyperLinux/Rosetta (HL) | `GR` stop_seq 4/5 (rax 34) ; `P/eac-ptrace-regset-transition/report.md:21` |
| 35 | `write` | fd, tampon, 7 757 608 (0x765f28) | écriture complète | enfant | observed under HyperLinux/Rosetta (HL) et (RLX) | `P/eac-ptrace-target-role-20260929.md:7` (RLX, `write(fd=3, 0x765f28)`) ; `P/eac-x86-ptrace-tgkill-20260929/README.md:15` (HL) |
| 36 | `fcntl` (72) | `F_ADD_SEALS` (1033), `0xf` (SEAL \| SHRINK \| GROW \| WRITE) | EINVAL sous HL. Sur noyau réel l'appel d'EAC n'est pas tracé ; le noyau implémente les seals et le contrôle A/B qui autorise ou refuse la commande par seccomp donne 509/210 dans les deux cas | enfant | observed under HyperLinux/Rosetta (HL) pour EINVAL ; résultat sur noyau réel : non observé | `P/eac-ptrace-regset-transition/report.md:23,35-37` ; `docs/research/rocket-league-macos.md:270-272` |
| 37 | `fcntl` | `F_GETFD` (1) sur le memfd | 0 (pas de `FD_CLOEXEC`, donc héritage par exec) | enfant | observed under HyperLinux/Rosetta (HL) | `P/eac-ptrace-regset-transition/report.md:24` |
| 38 | `openat` | `/game/Binaries/Win64/RocketLeague.exe`, `O_RDONLY` | fd | enfant | observed under HyperLinux/Rosetta (HL) | `P/eac-ptrace-regset-transition/report.md:25` ; `GR` (fs openat) |
| 39 | `fstat`, `lseek`, `read`, `close` | `fstat` ×2 ; `lseek(fd, 0x2512000)` puis `read` de 2488 octets ; `lseek(fd, 0)` puis `read` de 4096 octets | lectures réussies. 0x2512000 + 2488 = 0x25129b8 = taille du fichier (38 873 528 octets) : lecture de la fin de fichier | enfant | observed under HyperLinux/Rosetta (HL) | `P/eac-ptrace-regset-transition/report.md:26-27` ; taille mesurée dans `R/game/Binaries/Win64/RocketLeague.exe` ; la table Authenticode du PE débute à 0x2510000 (10 680 octets). Interprétation (vérification de signature) : inferred |
| 40 | `readlinkat` (267) | `AT_FDCWD`, `/proc/self/exe`, tampon de 256 | chemin du loader Wine, 56 octets (GE) ou 37 octets (Debian), non tronqué | enfant | observed under HyperLinux/Rosetta (HL) | `GR` (`readlink_boundary`) ; `P/eac-readlink-boundary-20260929/README.md:3` |
| 41 | `newfstatat` (262) | `<dirname du loader>/wine64` | 0 (GE) | enfant | observed under HyperLinux/Rosetta (HL) | `GR` fs pathop `…/x86_64-unix/wine64` (juste avant l'exec) |
| 42 | `execve` (59) | chemin du loader Wine ; argv/envp non capturés | événement exec ptrace, statut `0x4057f` | enfant | observed under HyperLinux/Rosetta (HL) | `GR` (clobber seq 24, fs resolve `wine-preloader`) ; oracle : `P/ptrace-postexec-exit-control-20260930/qemu-20260930T084834023780Z/manifest.json` |
| 42b | chemin Debian : `close(memfd)`, `munmap`, `exit_group` (231) code 1 | après le `readlink` de 37 octets | l'enfant sort sans exec | enfant | observed under HyperLinux/Rosetta (HL) | `P/eac-readlink-boundary-20260929/README.md:9` ; `P/eac-cachyos-tgkill-20260930/next-controls-source-provenance.md` (tableau, ligne « Suite enfant ») |

### 2.4 Tracee après exec (loader Wine puis ld.so)

Ces appels sont ceux d'un Wine ordinaire ; ils sont listés parce qu'EAC les observe un à un avec `PTRACE_SYSCALL`.

| # | Séquence x86-64 | Preuve | Statut |
| --- | --- | --- | --- |
| 43 | Préloader : `arch_prctl` (158), `mmap` ×4, `open` (2), `read`, `mmap`, `mprotect` (10), `mmap` ×3, `close`, `open`, `read`, `mmap`, `mprotect`, `mmap` ×4, `close`, `prctl(PR_SET_NAME)` (157) : 23 syscalls | `P/eac-postexec-exit-comparison-20260930/README.md:19` ; `GR` (clobber génération 3, seq 1-23) | observed under HyperLinux/Rosetta (HL) |
| 44 | ld.so : `brk` (12), `access`, 12 `openat` entrecoupés de 11 `newfstatat` (recherche de bibliothèques), `read`, `pread64`, `fstat`, `mmap`, `arch_prctl`, `set_tid_address`, `set_robust_list`, `rseq`, `mprotect`, `prlimit64`, `getrandom`, `brk` ×2, `openat`/`read`/`mmap` des bibliothèques, `futex` | `GR` (clobber génération 3, seq 24-111) | observed under HyperLinux/Rosetta (HL) |

### 2.5 Thread traceur (module), requêtes ptrace et attentes

Le détail ordonné est en §3.2. Le tableau donne les formes.

| # | Opération | Motif d'arguments | Résultat | Statut | Preuve et oracle |
| --- | --- | --- | --- | --- | --- |
| 45 | `ptrace(SETOPTIONS 0x4200)` | `(pid, 0, 0x11)` = `TRACESYSGOOD` \| `TRACEEXEC` | 0 | observed under HyperLinux/Rosetta (HL) | `GR:61417` ; `P/ptrace-options-contract.md:3` ; oracle `P/ptrace-options/results/` (14/14) |
| 46 | `ptrace(SYSCALL 24)` | `(pid, 0, 0)` ; signal 0 | 0 ; stops `0x857f` | observed under HyperLinux/Rosetta (HL) | `GR:61838` ; `P/ptrace-syscall-contract.md:7-13` |
| 47 | `ptrace(GETREGSET 0x4204)` | `(pid, NT_PRSTATUS=1, &iovec{base, len=216})` | 216 octets copiés ; 27 mots x86-64 | observed under HyperLinux/Rosetta (HL) | `GR:62027` ; oracle : `P/ptrace-x86-full-context-control-20260929/results-20260929T161941227190Z/manifest.json` (272/272) et sonde PC `D/rocket-bazzite-return-20260929/received/sondes/results-host/results.json` (13/13, demande de 4096, retour 216) |
| 48 | `ptrace(GETREGSET)` | `NT_PRFPREG` (2), iovec de 512 octets | FXSAVE de 512 octets (EINVAL avant le candidat `77450112`) | observed under HyperLinux/Rosetta (HL) | `GR:62868` ; `P/ptrace-fpregset-runtime-20261001/README.md:5-7` ; oracle : `P/ptrace-fpregset-control-20261001/qemu-*` (43/43) |
| 49 | `ptrace(SETREGSET 0x4205)` | `NT_PRSTATUS`, 216 octets | 0 sous le candidat ; EIO ou EINVAL sinon | observed under HyperLinux/Rosetta (HL) | `GR:62033,62878` ; oracle : `P/ptrace-setregset-control-20261001/qemu-20261001T063637525656Z/manifest.json` (76/76 par mode) |
| 50 | `ptrace(GETSIGINFO 0x4202)` | `(pid, 0, &siginfo)` après l'événement exec | 128 octets ; EAC l'appelle une fois | observed under HyperLinux/Rosetta (HL) | `GR:61492` ; oracle PC : `D/rocket-bazzite-return-20260929/ANALYSE.md:49` (30/30) ; QEMU : `P/ptrace-signal-two-int3-control-20260930/…manifest.json` (signo 5, code 128) |
| 51 | `ptrace(PEEKDATA 2)` | `(pid, addr, &mot)` ; 164 appels dans `GR` | un mot de 8 octets copié vers `data`, retour brut 0 | observed under HyperLinux/Rosetta (HL) | `GR:61858` et suivants ; oracle : `P/ptrace-peek-control-20260930/qemu-20260930T110720674039Z/manifest.json` (375/375) |
| 52 | `ptrace(POKEDATA 5)` | `(pid, addr, mot de 8 octets)` ; 7 appels | 0 ; écriture dans une page exécutable du tracee | observed under HyperLinux/Rosetta (HL) | `GR:61908` ; oracle : `P/ptrace-poke-int3-control-20260930/qualification.json` (37/37) ; `P/ptrace-remote-poke-control-20261001/qemu-20261001T061756029742Z` |
| 53 | `ptrace(CONT 7)` | `(pid, 0, sig)` ; sig 0 sauf une fois 5 (SIGTRAP transmis) | 0 | observed under HyperLinux/Rosetta (HL) | `GR:61913,62884,63027` |
| 54 | `kill` (62) | `(pid tracee, SIGKILL)` deux fois | 0 | observed under HyperLinux/Rosetta (HL) | `GR:63046,63053` ; `P/eac-remote-maps-finding-20260930/README.md:3` |
| 55 | `wait4` (61) | `(pid, &status, 0, NULL)` | statuts : `0x137f` (4991), `0x857f` (34175), `0x4057f` (263551), `0x57f` | observed under HyperLinux/Rosetta (HL) | `GR` (événements `wait4`, `status`) ; oracle : `P/ptrace-tracer-window-control-20260930/README.md:5` |
| 56 | `newfstatat`, `openat`, `read` ×2, `close` | `/proc/<tracee>/maps`, lecture par blocs de 4096 octets, deux relectures après l'`INT3` | 13 octets de chemin (`/proc/80/maps`), contenu des VMA du tracee | observed under HyperLinux/Rosetta (HL) | `GR:61848,62050,62061` ; `P/eac-remote-maps-finding-20260930/README.md:3-7` ; oracle : `P/ptrace-remote-maps-control-20261001/qemu-20261001T000419470178Z` (268/268) |

### 2.6 Thread du module après l'échec : réseau, fichiers, threads

Ces opérations viennent des 783 événements `tracer_syscall_window` de `GR` (entrées et sorties ; champ `record` impair = entrée ; lignes 61838 à 63461) et sont `observed under HyperLinux/Rosetta (HL)`. Elles suivent le `kill` du tracee (records 405 et 407). On ne sait pas si le chemin de succès les exécute aussi.

| # | Opération | Motif | Remarque |
| --- | --- | --- | --- |
| 57 | `clock_gettime` (228), `rt_sigprocmask` (14) | blocs d'encadrement de chaque message de file interne | Les messages internes passent par une paire de descripteurs 26/27 : `write(27, buf, 64)` et `read(26, buf, 64)` entre `SIG_BLOCK` et `SIG_SETMASK`. `GR:63057-63461` (records 409 et suivants) |
| 58 | `uname` (63) ×3 | tampon utilisateur | valeurs non journalisées. `GR` (records 447, 449, 451) |
| 59 | `openat`, `fstat`, `lseek`, `read` (435 octets), `close` | chemin non journalisé | petit fichier texte (nature inconnue). records 421-433 |
| 60 | `newfstatat` | `/sys/class/dmi/id/sys_vendor`, `/sys/class/dmi/id/product_name`, `/run/host/container-manager`, `/.flatpak-info`, `/run/.containerenv`, `/.dockerenv`, `/etc/resolv.conf`, `/etc/nsswitch.conf` | détection de conteneur et de plate-forme. `GR:63084-63123` (`tracer_stat_path`) ; `GR:63095-63127` |
| 61 | `openat` | `/etc/hosts` | résolution de noms. `GR:63127` |
| 62 | `socket` (41) | `AF_INET` datagramme non bloquant, puis `AF_NETLINK` (16) brut, puis `AF_INET` flux | DNS UDP, netlink pour `getaddrinfo`, TCP. enregistrements 471-507 |
| 63 | `connect` (42), `poll` (7), `sendto` (44), `recvfrom` (45), `ioctl FIONREAD` (0x541b), `setsockopt` (54), `getsockopt` (55), `fcntl` | requête DNS de 35 octets, ClientHello de 182 octets, échanges TLS | pile réseau du module, via libc. enregistrements 473-627 |
| 64 | `epoll_create` (213), `epoll_ctl` (233), `epoll_wait` (232) | 50 descripteurs max, délai 2500 ms | enregistrements 517-523 |
| 65 | `getrandom` (318), `time` (201) | | TLS |
| 66 | `munmap` | libère des tampons de 13 828 096 et 14 221 312 octets | enregistrements 631, 701 |
| 67 | `exit` (60) | fin du thread | enregistrement 783 |

### 2.7 Signaux

| Signal | Où | Preuve | Statut |
| --- | --- | --- | --- |
| `SIGSTOP` (19) auto-envoyé par `tgkill` | enfant, avant tout `PTRACE_SYSCALL` ; statut 4991 | §2.3 #32 ; `GR:61416` | observed under HyperLinux/Rosetta (HL) |
| `SIGTRAP` (5), `si_code` 128 (`SI_KERNEL`), `si_addr` 0, statut `0x57f` | `INT3` posé par le traceur | oracle `P/ptrace-signal-two-int3-control-20260930/qemu-20260930T142310221768Z/manifest.json` ; PC `D/rocket-bazzite-return-20260929/ANALYSE.md:49` (si_code 128) | observed on real Linux x86 (QEMU TCG et PC) pour la sémantique |
| `SIGKILL` (9) | traceur contre le tracee, après échec, deux fois | §2.5 #54 ; `GR:63046,63053` | observed under HyperLinux/Rosetta (HL) |
| `SIGTRAP` transmis par `CONT(…, 5)` | traceur, après le second arrêt | `GR:63027` | observed under HyperLinux/Rosetta (HL) |
| `SIGSEGV`, statut 2943 (`0xb7f`) | tracee, après le `SIGTRAP` transmis ; sous HL seulement | `GR:63033` | observed under HyperLinux/Rosetta (HL) |
| `SIGSEGV` à l'adresse 9 attendu au retour de l'appel injecté | tracee | adresse de retour écrite par `POKEDATA` (valeur 9) à `GR:62873` | inferred |
| `SIGCHLD` | implicite, arrêts de l'enfant ; le module ne fait que `wait4` | aucune mesure | inferred |

## 3. ptrace : rôles, séquence et exigences

### 3.1 Rôles et raisons

- Tracee : enfant forké par le module (PID 80 dans `GR`, leader). Il s'auto-trace avec `PTRACE_TRACEME`. Linux attache alors le parent réel, c'est-à-dire le thread qui a créé l'enfant.
- Tracer : le thread worker du module (TID 76 de PID 2). Seul ce thread peut émettre les requêtes. HyperLinux a dû implémenter cette règle : `TRACEME` lie l'enfant au TID créateur, un autre thread du même processus ne peut pas le reprendre (`P/ptrace-runtime.md:21`).
- Raison (inferred) : lancer le jeu comme enfant tracé du bootstrapper, pour injecter le second ELF dans un processus Wine neuf avant son code utilisateur, sans `LD_PRELOAD`. Fondements : memfd hérité par l'exec (§2.3 #37), `POKEDATA` de la chaîne `/proc/self/fd/34` (§3.2 étape 21), mapping du memfd comme ELF dans le jeu sous QEMU TCG (§1.1).
- Options : exactement `0x11`. Ni `PTRACE_O_EXITKILL` ni `TRACECLONE` n'est demandé (`GR:61417`, `P/ptrace-options-contract.md:3`).
- `GETEVENTMSG` n'est jamais appelé par EAC dans ces traces (0 occurrence sur 265 appels `ptrace`). `GETSIGINFO` l'est une fois, `SEIZE`/`INTERRUPT`/`ATTACH`/`DETACH`/`SINGLESTEP`/`GETREGS`/`PEEKUSER`/`POKEUSER` jamais. Comptage par requête dans `GR` : `PEEKDATA` 164, `SYSCALL` 52, `GETREGSET` 32, `POKEDATA` 7, `CONT` 4, `SETREGSET` 3, `TRACEME` 1, `SETOPTIONS` 1, `GETSIGINFO` 1. Les autres runs du 2026-09-30 et 10-01 n'ajoutent aucune requête hors de cette liste.

### 3.2 Séquence mesurée (run `GR`, tracee PID 80)

Chaque ligne cite la ligne de `GR` (ou le champ `stop_seq` de l'événement `ptrace_regset`). Statut de tout le tableau : `observed under HyperLinux/Rosetta (HL)`, sauf mention. Les valeurs de registres viennent des 27 mots renvoyés à EAC (événements `ptrace_regset`), les arguments des requêtes de `ptrace_provenance` (`x86_args`) et de `tracer_syscall_window`.

| Étape | Requête et contexte | Détail mesuré | Ligne |
| --- | --- | --- | --- |
| 1 | `TRACEME` dans l'enfant | arguments tous nuls | 61407 |
| 2 | `wait4` rend le stop initial | statut 4991 (`0x137f`, SIGSTOP) | 61416 |
| 3 | `SETOPTIONS` | data = 17 (`0x11`) | 61417 |
| 4 | `SYSCALL` puis `GETREGSET` | premier stop de syscall (statut 34175 = `0x857f`) : `mmap` ENTRY, `rax` = -38, `orig_rax` = 9, RCX = RIP, `rsi` = `0x766000` | stop_seq 2 ; wait4 61424 |
| 5 | `SYSCALL` ×2, `GETREGSET` ×2 | `memfd_create` ENTRY (stop_seq 4), puis EXIT (stop_seq 5, `rax` = 34) | stop_seq 4-5 |
| 6 | `CONT` (sig 0) | l'enfant exécute `write`, `fcntl` ×2, `openat`, …, `readlinkat`, `newfstatat`, `execve` sans stop de syscall | |
| 7 | `wait4` | statut 263551 (`0x4057f`, événement exec) | 61491 |
| 8 | `GETSIGINFO` | un seul appel dans tout le run | 61492 |
| 9 | `SYSCALL` puis `GETREGSET` | premier stop après exec = sortie d'exec : `orig_rax` = 59, `rax` = 0, RIP = `0x7fffffffa000`, `eflags` = `0x202`, `fs_base` = `gs_base` = 0. C'est l'ordre de l'oracle Linux. | stop_seq 7 ; `P/eac-postexec-exit-comparison-20260930/README.md:9-17` |
| 10 | boucle `SYSCALL` ×2 + `GETREGSET` | un `GETREGSET` par syscall, à la sortie (phase 2), jusqu'à `prctl(PR_SET_NAME)` (`rdi` = 15, `orig_rax` = 157, stop_seq 53) | wait4 61505-61833 |
| 11 | `newfstatat`, `openat`, `read` ×2, `close` | `/proc/80/maps`, lu d'un trait | 61847-61856 (`tracer_stat_path` 61848) |
| 12 | `PEEKDATA` ×8 | 64 octets à `0x7ffffdffb000` : l'en-tête ELF (taille 64) du programme principal ; `e_entry` = `0x11f0` | 61858-61893 |
| 13 | `PEEKDATA` ×2, `POKEDATA` | adresse `0x7ffffdffc1f0` = base + `e_entry`. Valeur écrite `0x89485ed18949edcc` : octet bas `0xcc` (INT3) ; octets originaux `31 ed 49 89 d1 5e 48 89` (`xor ebp,ebp; mov r9,rdx; pop rsi; mov rdx,rsp…` : début du `_start` d'un programme) | 61898-61908 |
| 14 | `CONT` (sig 0), `wait4` | statut 1407 (`0x57f`, SIGTRAP) | 61913 ; wait4 62025 |
| 15 | `GETREGSET` | RIP = adresse INT3 + 1 (`0x7ffffdffc1f1`), `orig_rax` = -1, `eflags` = `0x246` | 62027 |
| 16 | `SETREGSET` | un seul mot change : RIP (indice 16) de `0x7ffffdffc1f1` à `0x7ffffdffc1f0` | 62033 ; événement `setregset_requested_difference` |
| 17 | `PEEKDATA`, `POKEDATA` | restaure le mot original `0x89485ed18949ed31` | 62039, 62044 |
| 18 | relecture de maps (×2) | deux séries `newfstatat`, `openat`, `read` ×2, `close` sur `/proc/80/maps` | 62049-62069 |
| 19 | `PEEKDATA` ×153 | résolution d'un symbole dans une bibliothèque du tracee : 113 mots (904 octets = en-tête ELF de 64 octets + 15 program headers de 56 octets) à `0x7ffffddcd000` ; 16 mots à `0x7ffffdfb1940` ; 3 mots à `…1528`, `…152c`, `…1530` (début de table de hachage GNU) ; 1 mot à `…30a4` et à `…58b0` ; 3 mots à `0x7ffffdde3e40` (une `Elf64_Sym` de 24 octets) ; 16 mots non alignés à `0x7ffffddeb14f` (comparaison de nom) | 62071-62831 |
| 20 | `GETREGSET` | état à sauvegarder | 62836 |
| 21 | `POKEDATA` ×4 | sur la pile du tracee à `0x7fffffff8200`, `…8208`, `…8210`, `…8218` : `0x65732f636f72702f`, `0x34332f64662f666c`, 0, 0 = `/proc/self/fd/34` suivi de zéros | 62842-62857 |
| 22 | `GETREGSET` `NT_PRSTATUS` puis `NT_PRFPREG` | sauvegarde des registres généraux et flottants | 62862, 62868 |
| 23 | `POKEDATA` | `0x7fffffff81f8` reçoit la valeur 9 | 62873 |
| 24 | `SETREGSET` | cinq mots changent : `rdx` (12) = 0, `rsi` (13) = 1, `rdi` (14) = `0x7fffffff8200`, RIP (16) = `0x7ffffde5bfc0`, `rsp` (19) = `0x7fffffff81f8` (ancienne valeur `0x7fffffff82a0`) ; `rsp` + 8 est aligné sur 16 | 62878 ; `setregset_requested_difference` |
| 25 | `CONT` (sig 0) | l'appel injecté doit s'exécuter | 62884 |
| 26 | `wait4` | statut 1407 (`0x57f`) : sous HL le second arrêt arrive comme SIGTRAP (détour interne de Rosetta, X28 = 9, X22 = 0) | 62920 ; wait4 63025 |
| 27 | `CONT` avec sig = 5 | EAC transmet le signal reçu, sans lire de registres | 63027 |
| 28 | `wait4`, `GETREGSET`, `SETREGSET` | statut 2943 (`0xb7f`, SIGSEGV) ; puis `GETREGSET` et `SETREGSET` rendent EIO | 63033 ; 63035, 63040 |
| 29 | `kill(80, SIGKILL)` ×2, puis chemin d'échec (§2.6) | callback 509, launcher 210 | 63046, 63053 |

Lecture des étapes 19 à 24 (`inferred`) : les octets écrits, les registres et l'adresse de retour sont mesurés. Sont déduits : (a) l'image lue à l'étape 19 est celle de libc (15 program headers, grande taille, adresse inférieure à celle de ld.so, RIP cible à l'offset `0x8efc0` de cette image) ; (b) la fonction appelée est `dlopen` (arguments `(chaîne, 1, 0)` : `RTLD_LAZY`) ; (c) l'objet chargé est le memfd de 7,76 Mo (le numéro 34 est celui retourné à l'étape 5). La pile est alignée comme à l'entrée d'une fonction System V, avec retour à l'adresse 9.

Suite sur Linux x86 réel (`inferred`) : le retour à l'adresse 9 fait un défaut de page ; le noyau livre `SIGSEGV` (stop `0xb7f`, `si_addr` = 9) au premier `wait4` de l'étape 26 ; EAC y lit le résultat (`rax`), restaure les registres sauvegardés à l'étape 22 (flottants compris), supprime le signal avec `CONT(0)` et laisse l'enfant poursuivre. Cette lecture s'appuie sur le comportement observé de EAC à l'étape 27 : face à un stop différent de celui attendu, il transmet le signal et n'en lit aucun registre ; ce n'est qu'au stop SIGSEGV de l'étape 28 qu'il redemande les registres.

### 3.3 Formats et tailles

| Objet | Contenu exigé | Preuve |
| --- | --- | --- |
| `NT_PRSTATUS` (1) | 27 mots x86-64 dans l'ordre `struct user_regs_struct` : r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx, rsi, rdi, orig_rax, rip, cs, eflags, rsp, ss, fs_base, gs_base, ds, es, fs, gs. 216 octets. Une demande de 4096 octets rend 216 (PC et QEMU TCG). Les longueurs non multiples de 8 (215, 7, 1) donnent EINVAL sur le noyau ARM64 (`P/regset-architecture.md:9`) ; la règle de `ptrace_regset` est générique, donc probable pour x86 (inferred, non mesuré en x86) | `P/ptrace-x86-tgkill-context-control-20260929/README.md:12` ; `D/rocket-bazzite-return-20260929/ANALYSE.md:48` |
| `NT_PRFPREG` (2) | 512 octets, disposition `FXSAVE` (x87, 16 registres XMM, MXCSR) | `P/ptrace-fpregset-runtime-20261001/README.md:5` |
| iovec | 16 octets lus d'abord ; `iov_len` réécrit seul (pas `iov_base`) ; la longueur est tronquée à la taille du jeu ; copie partielle possible avant une faute | `P/remote-getregset-contract.md:44-46` (mesures sur Linux ARM) ; oracle x86 : `P/ptrace-signal-two-int3-control-20260930/…manifest.json` (`NT_PRSTATUS buffer canaries`) |
| Valeurs au stop `INT3` | `orig_rax` = -1, `cs` = 0x33, `ss` = 0x2b, `eflags` = 0x246, RIP = adresse INT3 + 1 | `P/ptrace-signal-two-int3-control-20260930/…manifest.json` (rip 4198615 pour un site à 4198614) |
| Valeurs aux stops de syscall | ENTRY : `rax` = -38 (ENOSYS), `orig_rax` = numéro, RCX = RIP après `syscall`, R11 = eflags ; EXIT : `rax` = résultat | `GR` stop_seq 2-5 ; oracle `P/ptrace-x86-full-context-control-20260929/README.md:3` |
| Valeurs à la sortie d'exec | RIP = entrée de la nouvelle image, `rax` = 0, `orig_rax` = 59 (même pour `execveat`, 322), `eflags` = 0x202, autres registres nuls | `P/ptrace-tracer-window-control-20260930/README.md:5,23` |

### 3.4 Séquence INT3, arrêt et reprise

1. Le traceur écrit `0xcc` dans l'octet de poids faible d'un mot de 8 octets d'une page exécutable du tracee (`POKEDATA` avec écriture forcée : cible en lecture seule autorisée sur un mapping privé, comme `FOLL_FORCE`). Oracle : `P/ptrace-poke-int3-control-20260930/README.md` (37/37) ; règles de permissions : `P/ptrace-poke-design-20260930/DESIGN.md:9-11,40-44`.
2. `CONT` : le tracee exécute l'INT3. Stop de signal-delivery, statut `0x57f`, `siginfo.si_code` = 128 (`SI_KERNEL`), RIP = INT3 + 1.
3. Le traceur lit les registres, restaure l'octet original par `POKEDATA`, puis `SETREGSET` avec RIP = INT3. Linux ne rembobine jamais RIP seul. Le traceur le fait explicitement.
4. `CONT(0)` : l'instruction d'origine s'exécute. Le témoin exige que le tracee exécute réellement l'instruction restaurée (exit 77) : la traduction du code posée dans le cache avec l'INT3 doit avoir disparu.
5. Dans EAC, l'étape 4 n'est pas faite à ce stop : après la restauration (§3.2 étape 17), EAC lit la mémoire du tracee, prépare l'appel injecté et n'émet aucun `CONT` avant l'étape 25.

Point d'implémentation majeur : toute écriture ptrace dans le code d'un processus doit invalider les traductions de ce processus avant la reprise. Rosetta échoue ici : deux fragments de traduction recouvrent l'octet (le fragment d'origine et un fragment `INT3` d'un octet) et son cache par thread réinsère une traduction `BRK` périmée après effacement (`P/ptrace-remote-setregset-runtime-20261001/README.md:32-45`, `P/ptrace-jit-invalidation-runtime-20261001/README.md:5`). Avec FEX, le tracee et le traceur sont deux processus macOS ; l'invalidation doit donc traverser les processus (`inferred`, point de conception, voir §6).

### 3.5 Registres qui doivent être exacts

- Lecture : les 27 mots à chaque stop de lecture, en particulier RIP, RSP, `orig_rax`, `rax`, RCX, R11, `eflags`, `fs_base`, `gs_base` et les sélecteurs. Les valeurs doivent suivre le comportement du noyau : RCX = RIP suivant et R11 = eflags à `syscall` (`P/ptrace-x86-feasibility-20260929.md:45-47,80-89`), `fs_base` et `gs_base` non nuls dans le témoin (`ptrace-x86-full-context-control-20260929/README.md:5`).
- Écriture : tout mot non modifié par EAC doit être accepté tel qu'il a été lu (aller-retour exact). Les mots modifiés par EAC sont RIP, `rsp`, `rdi`, `rsi`, `rdx`. Les registres appelés doivent conserver `rbx`, `rbp`, `r12` à `r15`, `fs_base` (TLS de glibc pour `errno` et le canari de pile), `eflags` (DF = 0), MXCSR et x87 (par `NT_PRFPREG`). La candidature HL n'accepte que des écritures « sûres » et refuse (EIO) les changements de sélecteurs, bases, flags et R11 ambigus (`P/ptrace-general-regset-runtime-20261001/README.md:3`).
- `orig_rax` = -1 aux stops de signal ; valeur du syscall aux stops de syscall. Écrire `-1` ne doit pas redémarrer de syscall.

### 3.6 Événements exec et fork, statuts, siginfo, tgkill

- Statuts `wait4` à décoder avec les valeurs Linux, non avec les macros Darwin (le stop `0x137f` est lu comme « continué » par `WIFCONTINUED` Darwin) : `P/proc-ptrace-module.md:26`.
- Stop initial : `0x137f` (SIGSTOP). Stop de syscall avec `TRACESYSGOOD` : `0x857f`, `GETEVENTMSG` 1 (ENTRY) ou 2 (EXIT) (`P/ptrace-syscall-contract.md:9`). Événement exec avec `TRACEEXEC` : `0x4057f` (`P/ptrace-options-contract.md:5`). `INT3` : `0x57f`. Sortie d'enfant : `0x4d00` pour exit 77 (témoin).
- Le premier arrêt après l'exec est la sortie d'exec, pas l'entrée du premier syscall de la nouvelle image : statut `0x857f`, `GETEVENTMSG` 2, `GETREGSET` réussi (`P/ptrace-tracer-window-control-20260930/README.md:5`). HL a longtemps manqué ce stop (`P/eac-postexec-exit-comparison-20260930/README.md:9`).
- Le `tgkill` auto-adressé est suivi d'un stop de signal et d'aucun stop de sortie de `tgkill` avant les stops de syscall (5 stops dans l'oracle : getpid ENTRY/EXIT, write ENTRY/EXIT, exit ENTRY, phases 1,2,1,2,1). Un stop supplémentaire `orig_rax` = 234 fait diverger HL (`P/ptrace-x86-tgkill-context-control-20260929/README.md:5-12`).
- Après `TRACEEXEC`, un signal ordinaire passé à `CONT` à cet événement est ignoré ; un signal passé à un stop de syscall est remis en file comme `SI_KERNEL` (`P/ptrace-options-contract.md:7`, `P/ptrace-syscall-contract.md:13`).
- `GETSIGINFO` (128 octets) : pour le `SIGSTOP` auto-envoyé, `si_signo` 19 et `si_code` `SI_TKILL` (-6) avec pid et uid de l'enfant ; pour un stop de syscall `TRACESYSGOOD`, `si_signo` 5 et `si_code` 0x85 ; pour l'événement exec, 5 et 0x405 ; pour l'INT3, 5 et 128 (`SI_KERNEL`), `si_addr` 0. Valeurs attendues par `P/ptrace-exec-siginfo-control-20260930/README.md` et `P/ptrace-siginfo-contract-control-20260930/README.md` (fixtures préparés : `ptrace-exec-siginfo-control` a un baseline HL rouge dans `baseline-evaluation.json` ; `ptrace-siginfo-contract-control` n'avait été ni exécuté ni comparé à un Linux natif à sa préparation) ; seul le cas INT3 (128) a un oracle x86 : QEMU TCG et PC (`D/rocket-bazzite-return-20260929/ANALYSE.md:49`). Les autres codes sont `inferred` d'après l'ABI Linux.

### 3.7 Échelle de progression sous HyperLinux et divergences connues

Chaque ligne donne le premier refus attribuable à un runtime donné ; le verdict final reste 509/210 à toutes les étapes (sauf 506/207, 502/208 aux étapes 1 et 2). Heures en UTC.

| Étape | Runtime (SHA court) | Premier refus attribuable | Preuve |
| --- | --- | --- | --- |
| 1 | HL, 2026-09-28 | libc absente de `/proc/self/maps` : 507/206 | `docs/research/rocket-league-macos.md:142` |
| 2 | HL jusqu'à `bb2f` | `/proc/<ppid>/exe`, `cmdline` ENOENT : 502/208 `Launch Error: 256` | `P/eac-parent-proc-comparison.md:9-24` |
| 3 | HL `3c1babf6` | `/proc/<ppid>/status` fourni : 502 devient 509/210 ; l'enfant reçoit EINVAL à `TRACEME` | `P/eac-parent-status-transition/README.md:3-6,58-69` |
| 4 | HL `361882f5` | `SETOPTIONS(0x11)` EINVAL | `P/eac-ptrace-transition/report.md:5` |
| 5 | HL `a4178585` | `PTRACE_SYSCALL` EINVAL | `P/eac-ptrace-options-transition/report.md:5` |
| 6 | HL `0701cfc1` | `GETREGSET` ESRCH (le tracee est un autre processus hôte) | `P/eac-ptrace-syscall-transition/report.md:5,20` |
| 7 | HL `6601c6ec` | 66 `GETREGSET` réussissent avec des registres ARM64 (216 octets = x0 à x26) ; `F_ADD_SEALS` EINVAL ; l'enfant fait `exit_group(1)` | `P/eac-ptrace-regset-transition/report.md:3` |
| 8 | RLX (noyau réel + Rosetta, Wine Debian) | mêmes 66 captures ARM ; l'enfant fait aussi `memfd_create`, `write`, `fcntl`, puis `exit_group(1)` sans exec ; le retour de `fcntl` n'est pas tracé | `P/eac-ptrace-target-role-20260929.md:5-7` ; `P/eac-ptrace-audit-regset-linux-20260929/README.md:36-50` |
| 9 | HL `3d37afab` (pont x86 de Rosetta, Wine Debian) | trois `GETREGSET` x86 corrects (`mmap`, `memfd_create` ENTRY et EXIT), `readlink` de 37 octets, fermeture du memfd, `exit_group(1)` | `P/eac-x86-ptrace-tgkill-20260929/README.md:7-15,28` ; `P/eac-readlink-boundary-20260929/README.md:3-9` |
| 10 | HL `a8829b17` (09-30 07:43, Wine Cachy 11) | l'enfant arrive à l'exec ; `GETSIGINFO` ESRCH | `P/eac-cachyos-tgkill-20260930/run-20260930T074315115989Z/README.md` ; `next-controls-source-provenance.md` |
| 11 | HL `872a6f2a` (08:00), `44d98bdc` (08:13) | `GETSIGINFO` ok ; 26 `GETREGSET` ; premier stop après exec = entrée de `arch_prctl` avec `rax` = -38 pour EAC (entrée de `prctl` pour le témoin possédé), donc stop de sortie d'exec absent ; après la sortie de `prctl(PR_SET_NAME)` le parent envoie deux `SIGKILL` | `P/ptrace-tracer-window-control-20260930/README.md:5` ; `P/eac-process-lifecycle-20260930/run-20260930T081347056221Z/README.md` |
| 12 | HL `7cadfcc4` (09:14) | sortie d'exec correcte ; 27 `GETREGSET` ; entrée de `brk`, puis deux `SIGKILL` | `P/eac-postexec-exit-comparison-20260930/README.md:5-21` |
| 13 | HL `b797c823` (09:35), `7b7133a4` (09:51) | dernier appel du traceur avant les `SIGKILL` : `newfstatat("/proc/80/maps")` = ENOENT (chemin capturé à 09:51) | `P/eac-tracer-window-20260930/run-20260930T093526586977Z/tracer-window-summary.json` ; `P/eac-remote-maps-finding-20260930/README.md:3` |
| 14 | HL `1f842509` (10:23) | maps distante fournie (stat 0, open, 2580 octets, EOF) ; `PEEKDATA(0x7ffffdffb000)` EINVAL | `P/eac-proc-remote-maps-20260930/run-20260930T102349870211Z/progress-finding.json` |
| 15 | HL `48a7ee6a` (11:25) | 10 `PEEKDATA` ok ; `POKEDATA(0x7ffffdffc1f0, 0x89485ed18949edcc)` EINVAL | `P/eac-ptrace-peek-20260930/run-20260930T112510372128Z/progress-finding.json` ; `P/ptrace-poke-design-20260930/DESIGN.md:3` |
| 16 | HL `173201a1` (13:39) | `POKEDATA` ok ; `CONT` ; stop SIGTRAP `0x57f` ; `GETREGSET` EIO ; l'octet d'origine est restauré ; deux `SIGKILL` | `P/eac-ptrace-poke-20260930/run-20260930T133949334669Z/progress-finding.json` (champ `observed_delta`) |
| 17 | HL `29649335`, `80953d6e`, `fb965df1` (10-01 00:05 à 06:19) | VMA, `PEEKDATA` et `POKEDATA` réels remplacent les approximations précédentes | `P/ptrace-remote-maps-runtime-20261001/README.md` ; `P/ptrace-remote-peek-runtime-20261001/README.md` ; `P/ptrace-remote-poke-runtime-20261001/README.md` |
| 18 | HL `e2f12c45` (06:31), `cf568d78` (06:34) | registres x86 réels au stop INT3 ; `SETREGSET` demandé observé : un seul mot, RIP moins 1 | `P/ptrace-signal-regs-runtime-20261001/README.md` ; `P/eac-setregset-diff-20261001/run-20261001T063442877259Z/setregset-diff-summary.json` |
| 19 | HL `acc0844f` (08:49) | `SETREGSET` accepté ; `GETREGSET NT_PRFPREG` EINVAL ; aucun `CONT` avant les `SIGKILL` | `P/ptrace-jit-invalidation-runtime-20261001/README.md:11` |
| 20 | HL `77450112` (10:09, 10:12) | `NT_PRFPREG` ok ; le second `SETREGSET` (écriture générale) rend EIO | `P/eac-fpregset-20261001/validation-summary.json` |
| 21 | HL `f3f7ca30` (10:21, 10:25) | écriture générale acceptée, `CONT`, second stop SIGTRAP au détour de Rosetta (X28 = 9, X22 = 0), snapshot indisponible (EIO), signal transmis, SIGSEGV, `SIGKILL` | `P/ptrace-general-regset-runtime-20261001/README.md:9` ; `GR:63025-63053` |

Divergence suspecte (`inferred`, non testée isolément) : à l'étape 9 l'enfant referme le memfd et sort quand `readlink(/proc/self/exe)` rend un chemin se terminant par `wine64-preloader.static` (Debian 10, runtime `2cd7375c`/`3d37afab`) ; à l'étape 10, avec `…/x86_64-unix/wine-preloader` (GE-Proton, Cachy 11, runtime `a8829b17`), il fait `newfstatat(<dir>/wine64)` puis `execve`. Les deux runs diffèrent par le runtime, la version de Wine et le nom du loader. `P/eac-cachyos-tgkill-20260930/next-controls-source-provenance.md` note la différence sans l'attribuer et écrit que les noms différents « ne prouvent pas une erreur ». Aucun test ne change uniquement ce nom.

Observation (`inferred`) : `Launch Error: 256` vaut 0x100, le statut brut `wait4` d'un `exit(1)`. Cette égalité n'est pas démontrée ; `R/linux-reference/eac-static-208/analysis.md:28-32` écarte toute lecture de 256 comme errno, statut de sortie ou verdict.

## 4. Faits d'environnement vérifiés par le module

N'y figure que ce qui est évidencé.

| Fait | Évidence | Statut |
| --- | --- | --- |
| Un parent avec `/proc` lisible est exigé : PID 1 / PPid 0 donne 502 `Launch Error: 256` en 39 ms, même sur noyau réel. Pas de parent fabriqué | `R/linux-reference/pid1-control/README.md` | observed under HyperLinux/Rosetta (RLX) |
| Lit `comm`, `exe`, `cmdline`, `status` du parent et `status`, `exe` de soi ; 31 octets sur le parent ; échec ENOENT de `exe`/`cmdline` donne 502/256 ; échec de `status` donne 502/256 | `P/eac-parent-proc-comparison.md:9-24` ; `P/eac-parent-status-transition/README.md:3-6` | observed under HyperLinux/Rosetta (HL) |
| Refuse une VM : WSL2 donne 502/208 `Cannot run under Virtual Machine.` à +19 ms, avant tout ptrace. Critère inconnu | `D/rocket-wsl-eac-return-20260929/ANALYSE.md:3,21-29,45` | observed on real Linux x86 (WSL2 est une VM x86 ; ce n'est pas un noyau natif) |
| Accepté sans refus de VM : PC physique (Bazzite) ; QEMU TCG `-cpu max -machine pc,vmport=off,hpet=off -accel tcg`, 4 vCPU, 8 Gio, sans flag ajouté ou retiré ; HyperLinux (pas de noyau). Rosetta sur noyau ARM64 (VM OrbStack) donne 509, pas le message VM | `P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/config.plist:170-215` ; `D/rocket-bazzite-return-20260929/ANALYSE.md:31` | observed on real Linux x86 (QEMU TCG, PC) |
| Les sondes DMI (`board_name`, `product_name`, `sys_vendor`) et PCI (`/proc/bus/pci/devices`) sont lues ; leur absence n'arrête pas le module | `P/eac-parent-status-transition/after-status-context.txt:1015616-1015664` ; `GR:63084-63087` | observed under HyperLinux/Rosetta (HL) |
| Détection de conteneur par `stat` de `/.flatpak-info`, `/run/.containerenv`, `/.dockerenv`, `/run/host/container-manager` (après l'échec dans `GR`, rôle exact inconnu). Sur le PC le jeu tourne sous Heroic Flatpak + pressure-vessel (`Seccomp` 2, `NoNewPrivs` 1) et EAC répond 301 ; la présence de ces fichiers dans l'espace de noms du bootstrapper n'est pas mesurée | `GR:63090-63105` ; `D/rocket-bazzite-return-20260929/received/chronologie/process-tree-during-match.json` ; `D/rocket-bazzite-return-20260929/received/rapport.md:37` | observed under HyperLinux/Rosetta (HL) pour les stats ; observed on real Linux x86 (PC) pour l'acceptation sous conteneur |
| Chemin du loader : `readlink(/proc/self/exe)` doit nommer un fichier relançable ; `<dir>/wine64` doit exister (GE) ; `runtime.conf` est cherché à `<dir>\EasyAntiCheat\Certificates\runtime.conf` (nom littéral avec backslashes) | §2.2 #24 et §2.3 #40-41 | observed under HyperLinux/Rosetta (HL) |
| Uname : trois appels à `uname`, valeurs non journalisées ; `gnu_get_libc_version` importé | `GR` (enregistrements 447-451) ; `P/eac-current-module-static-20260929/dynamic-symbols.txt` | observed under HyperLinux/Rosetta (HL) pour l'appel ; inferred pour l'usage |
| `ptrace_scope` : 0 sur le PC (SELinux Enforcing) ; sonde WSL à 1 en root ; EAC n'utilise que `TRACEME` | `D/rocket-bazzite-return-20260929/ANALYSE.md:31` ; `D/rocket-x86-return-20260929/ANALYSE.md:31` | observed on real Linux x86 (PC) |
| Dumpable : l'enfant met `PR_SET_DUMPABLE` à 0 avant `TRACEME` ; l'exec le remet à 1, ce qui permet `PEEK` et l'ouverture de `/proc/<pid>/maps`. Avec dumpable = 0 : `PEEK` donne EIO, l'ouverture de maps EACCES | `P/ptrace-peek-control-20260930/README.md` (QEMU 375/375) | observed on real Linux x86 (QEMU TCG) pour la sémantique ; EAC : observed under HyperLinux/Rosetta (HL) |
| Seccomp : `prctl(PR_SET_SECCOMP)` EINVAL ×27 apparaît dans le contrôle Windows et le lancement EAC (émetteur non attribué ; aucune cause de 509 démontrée) | `P/eac-cachyos-process-trace-20260929/RESULTATS.md:27` | observed under HyperLinux/Rosetta (RLX) |
| `/tmp/eac.flag` est sondé | §2.2 #15 | observed under HyperLinux/Rosetta (HL) |
| `process_vm_readv`/`writev` : aucun import nommé ; ENOSYS sous HL sans effet connu | `R/linux-reference/umu/eac-native-elf-capture/README.md:35` ; `P/ptrace-x86-feasibility-20260929.md:18` | observed under HyperLinux/Rosetta (HL) |
| CPUID, `/proc/cpuinfo`, cgroups, `LD_PRELOAD`, `mmap_min_addr`, `personality`, ASLR, `TracerPid` : aucune lecture ni test établi dans ces sondes. Notes : l'adresse de retour 9 doit rester inaccessible (donc `mmap_min_addr` > 9) ; l'ASLR est actif dans la carte mesurée du jeu sous QEMU (`game-process-maps.txt`) | | inferred, sauf l'ASLR (mesuré) |
| Imports libc du module courant (230) : `fork`, `execv`, `execvp`, `waitpid`, `ptrace`, `syscall`, `prctl`, `kill`, `getpid`, `uname`, `gnu_get_libc_version`, `sysconf`, `getpwuid`, `popen`/`pclose`, `dlopen`/`dlsym`/`dladdr`/`dl_iterate_phdr`, `shmget`/`shmat`/`shmdt`/`shm_open`/`shm_unlink`, `mmap`/`mprotect`/`madvise`/`mlock`, `getcontext`/`makecontext`/`setcontext`, `epoll_*`, `socket` et la pile sockets, `getaddrinfo`, `iconv_*`, `pthread_*` (rwlock, cond), `sigaction`, `signal`, `raise`, `alarm`, `select`, `ioctl`, `tcgetattr`, `glob`, `realpath`, `getentropy` | `P/eac-current-module-static-20260929/dynamic-symbols.txt` | inferred (table d'imports statique) |

## 5. Témoins et oracles réutilisables sans QEMU

### 5.1 Principe

Les témoins sont des ELF x86-64 statiques sans libc, compilés avec Apple clang (`-target x86_64-linux-gnu -ffreestanding -fno-pie`) et liés avec `ld.lld -static --no-pie -e _start --image-base=0x400000 -z separate-code -z max-page-size=4096`. Le linker employé est celui du NDK Android (`/Users/mathieu/Library/Android/sdk/ndk/28.2.13676358/…/ld.lld`). Les binaires déjà liés (`probe-x86_64`, `target-x86_64`, `target-flags-x86_64`) sont dans chaque répertoire et leurs SHA-256 dans `build.json`. Pour qualifier un nouveau runtime il suffit de lancer `probe-x86_64` (il forke ses cibles) puis de comparer. Les sorties sont des lignes JSON : `kernel`, `mount_proc`, `check{name, actual, expected, pass}`, `regset{stop, result, length, rip, hex}`, `siginfo{signo, code, addr}`, `result{passed, total}`. La comparaison se fait par noms de contrôles ordonnés, résultat `pass` et relation résultat/attendu.

Les runners fournis (`run-hl.py`) sont propres à HyperLinux : ils vérifient les SHA des entrées, exécutent le runtime avec `--bind`, interdisent le réseau par `sandbox-exec`, bornent le temps (25 s) et le journal (1 Mio) et lisent `--oracle native-oracle-flags.json`. Un `run-darwin.py` serait à écrire sur ce modèle : lancer `probe-x86_64` dans le nouveau runtime, capturer stdout et comparer à `qemu-*/manifest.json` (champ `events`).

Reconstruction : `python3 <dir>/build.py` (vérifie les SHA attendus). Oracle : `run-qemu.py` (réutilise une image Docker ARM64 en cache et le noyau 6.12.107+deb13-amd64 en cache, SHA `2b2358b3…`, sans réseau ni disque). Cette dernière étape n'est pas à rejouer : les manifestes enregistrés suffisent.

### 5.2 Tableau des témoins

| Répertoire (sous `P/`) | Ce qu'il teste | Sortie Linux x86 enregistrée | Lecture |
| --- | --- | --- | --- |
| `ptrace-x86-full-context-control-20260929` | 27 champs de `NT_PRSTATUS`, tailles 216 et 272, cinq stops exacts (getpid, write, exit), vrais RCX/R11, FS/GS non nuls, enfant exit 42 | `results-20260929T161941227190Z/manifest.json` : 272/272 (QEMU TCG) | observed on real Linux x86 (QEMU TCG) |
| `ptrace-x86-tgkill-context-control-20260929` | même témoin avec `tgkill(self,self,SIGSTOP)` ; ELF SHA `2a3f6544…` | `results-20260929T165852960827Z` : 272/272 ; `eac-order-readonly.json`, `evaluation.json` | idem |
| `ptrace-x86-semantic-control-20260929` | contexte à un `INT3` ; paquet `portable/` pour PC physique (`run-native.py`) | `semantic-evaluation.json` ; Rosetta et HL : 0/36 champs justes (vue ARM) | Rosetta : observed under HyperLinux/Rosetta ; le résultat natif n'a pas été collecté dans ce répertoire |
| `ptrace-x86-syscall-semantic-control-20260929` | contexte aux stops `PTRACE_SYSCALL` | `semantic-evaluation.json` (Rosetta et HL identiques, ARM X8/X24/X28) | observed under HyperLinux/Rosetta (RLX, HL) |
| `ptrace-signal-regset-control-20260930` | `SIGSTOP` puis `INT3`, `GETREGSET` 216, `GETSIGINFO` | `qemu-20260930T135510823911Z/manifest.json` : 15/15 | observed on real Linux x86 (QEMU TCG) |
| `ptrace-signal-two-int3-control-20260930` | deux sites INT3, `CONT(0)` après chacun, exit 77 | `qemu-20260930T142310221768Z/manifest.json` : 22/22 | idem |
| `ptrace-signal-regs-control-20261001`, `-qualified-` | registres x86 au `SIGTRAP` INT3, cinq modes | `native-oracle-flags.json` ; 60/60 sur Linux x86 | idem |
| `ptrace-breakpoint-control-20261001`, `ptrace-setregset-control-20261001`, `-two-trap-`, `-deliver-` | INT3 + `SETREGSET` (RIP moins 1) + restauration de code + reprise ; modes plain, cont, syscall, pre-syscall, execveat | `ptrace-setregset-control-20261001/qemu-20261001T063637525656Z/manifest.json` : 76/76 par mode ; `ptrace-breakpoint-control-20261001/qemu-20261001T062322664795Z` et `…062540698072Z` | idem. HL : 74/76 (traduction périmée). JIT invalidation : 76/76 |
| `ptrace-general-regset-control-20261001` | écriture générale de registres (nouvelle pile, appel de fonction x86, exit 37) | `candidate.json`, `red.json` ; Linux x86 76/76 | idem |
| `ptrace-fpregset-control-20261001` | `NT_PRFPREG` : x87, 16 XMM, arrondi, FTZ, exceptions SSE ; 43/43 (bord) | `qemu-20261001T095005943638Z`, `…095620328583Z`, `…100141654400Z`, `…101654707738Z` ; `fp-*.json` | idem |
| `ptrace-postexec-exit-control-20260930` (+ `-candidate-`, `ptrace-postexec-prctl-control`, `ptrace-mapped-elf-postexec-control`, `ptrace-tracer-window-control`) | ordre `TRACEME`/SIGSTOP, `SETOPTIONS`, `CONT`, EXEC, `SYSCALL` ; sortie d'exec ; `GETEVENTMSG`, `GETSIGINFO` ; erreurs EFAULT | `qemu-20260930T084834023780Z/manifest.json` ; `native-oracle-flags.json` | idem. Les 5 modes valent 12/12 à 162/162 |
| `ptrace-peek-control-20260930`, `ptrace-peek-observation-control`, `ptrace-peek-extended-diagnostic-control-20261001` | `PEEKTEXT/PEEKDATA` : mot tout à 1, non aligné, traversée de pages, `PROT_NONE`, EIO, EFAULT, ESRCH, dumpable | `qemu-20260930T110720674039Z/manifest.json` : 375/375 ; `native-oracle.json` | idem |
| `ptrace-remote-peek-control-20261001`, `ptrace-remote-maps-control-20261001`, `ptrace-remote-poke-control-20261001`, `ptrace-stat-window-control-20260930`, `ptrace-tracer-stat-control-20261001` | lecture/écriture mémoire d'un autre processus, `/proc/<pid>/maps` distant (stat, open, read, mprotect, munmap), chemin `newfstatat` capturé | `qemu-20261001T001541805339Z` (289/289), `…T000419470178Z` (268/268), `…T061756029742Z` et `…T061912553710Z` (54/54) | idem |
| `ptrace-poke-int3-control-20260930`, `-exec-`, `-commit-`, `-pte-toggle-`, `-translation-`, `-translation-clflush-`, `ptrace-poke-design-20260930` | `POKE` d'un octet de code exécutable, `INT3` posé et retiré, droits de page, translation | `qualification.json` (37/37 pour INT3) | idem |
| `ptrace-options`, `ptrace-options-controls` | `SETOPTIONS 0x11`, `GETEVENTMSG`, EXEC event, WNOWAIT | `ptrace-options/results/`, `ptrace-options-controls/results/` (sur Linux ARM64 réel et Rosetta, pas QEMU) | observed under HyperLinux/Rosetta (RLX) |
| `ptrace-syscall*`, `ptrace-traceme`, `ptrace-worker`, `ptrace-lifecycle`, `ptrace-regset*` | `PTRACE_SYSCALL`, injection de signal, timers, cycle de vie, 272 octets ARM64 | `results/` de chaque répertoire (références ARM64 et x64/Rosetta) | observed under HyperLinux/Rosetta (RLX) |
| `ptrace-exec-siginfo-control-20260930` (+ `-postexec-`), `ptrace-siginfo-contract-control-20260930`, `ptrace-siginfo-control-20260929` | `GETSIGINFO` : `SIGSTOP` par `tgkill` (code -6), événement exec (0x405), stop de syscall (0x85), refus EFAULT/ESRCH ; version BRK ARM64 | `baseline-evaluation.json` (baseline HL rouge) ; le contrôle `ptrace-siginfo-control` a une référence Linux ARM64 23/23 | aucun oracle x86 natif enregistré pour ces fixtures (sauf INT3 : PC et QEMU) |
| `ptrace-audit-control-20260929`, `ptrace-audit-memory-control-20260929` | observateurs `LD_AUDIT` x86-64 glibc pour les appels `ptrace@plt` : `ptrace-audit.so` (SHA `32d2ab71…`, 8 contrôles) et `ptrace-audit-memory.so` (SHA `cb2ae09e…`, lit l'iovec et le tampon de `GETREGSET`, 8 cas) | `results-20260929T141302293656Z/manifest.json`, `results-20260929T143930143296Z/manifest.json` | observed under HyperLinux/Rosetta (RLX, HL). Outil réutilisable sur tout runtime glibc pour tracer les 21 sites `ptrace` du module ; sa présence (variable `LD_AUDIT`) change l'environnement du processus (effet sur EAC non établi) |
| `ptrace-fpreg-control-20260929`, `ptrace-fpreg-set-control-20260929`, `ptrace-hwdebug-*`, `ptrace-sigchld-control-20260929`, `ptrace-notify-runtime-20260929` | `NT_PRFPREG` ARM64 (528 octets), `NT_ARM_HW_WATCH`, notification `SIGCHLD` du débogueur de Rosetta | `results-*` de chaque répertoire | Internes à Rosetta et au runtime HL ; hors contrat EAC, sans intérêt pour un moteur FEX |
| PC physique : `D/rocket-bazzite-return-20260929/received/sondes/` | `linux-regset-probe.c` (SHA `ac04f0be…`, 13/13) et `linux-siginfo-probe.c` (30/30) | `results-host/results.json`, `linux-regset-probe.txt`, `linux-siginfo-probe.txt` | observed on real Linux x86 (PC) |
| WSL2 : `D/rocket-x86-return-20260929/received/linux/` | même sonde regset (13/13, 216 octets, RAX d'un getpid) | `probe-result.json`, `probe.stdout.txt` | observed on real Linux x86 (VM) |

### 5.3 Autres références réutilisables

- Statique : `P/eac-current-module-static-20260929/` (ELF décodé `module-current.elf`, désassemblage, `verified-ptrace-call-sites.json` : 21 appels à `ptrace@plt` dans le segment exécutable non sectionné `[0xa7e000, 0xd33cc8)`, 7 `waitpid`), `R/linux-reference/eac-static-208|210/`.
- Dynamique sur noyau réel : `R/linux-reference/loader-trace/ge-eac*.` (LD_DEBUG) et `ge-eac-process-maps.txt`.
- Session protégée : `P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/outputs/` (`game-process-maps.txt`, `process-status.json`, `cpu-task-status.json`) ; les fichiers `*.private.log` n'ont pas été ouverts.
- Runners EAC non authentifiés : `P/eac-process-lifecycle-20260930/run-current.py`, `P/eac-general-regset-20261001/run-eac-minimal.py` ; ils dépendent de HyperLinux, de Rosetta et d'un Wine Cachy préparé. Ce sont des modèles de protocole, non des outils pour Darwin.

## 6. ABI Linux minimale pour le nouveau runtime, par priorité

Chaque entrée dit ce qui est exigé, pourquoi, et l'évidence. « Exigé » signifie : EAC s'arrête ou diverge si ce n'est pas fourni (sauf mention).

### P0 : sans elles, pas de callback au-delà de 206/208

1. Un processus Linux x86-64 avec glibc réelle et ld.so en mémoire, où `/proc/<pid>/maps` liste, dans l'ordre des adresses, les mappings réels avec noms de fichiers invités, offsets exacts et la colonne dev/inode ; la première ligne portant `libc.so` ou `/libc-` doit être le segment d'offset 0 qui contient l'en-tête ELF et un `PT_DYNAMIC` exploitable ; `dlopen` et `dlsym` (ou `__libc_dlopen_mode`) doivent être exportés (`maps-resolver.md:5-9`). Sans ce point le résultat est 206 ou 507, comme Voile.
2. `memfd_create` avec `MFD_ALLOW_SEALING`, `write`, `lseek`, `fstat`, `pread`, `mmap` privé exécutable d'un memfd, et `openat("/proc/self/fd/N")` qui rouvre le memfd (lien magique vers l'objet) (`§2.1`).
3. Identité de processus et procfs de soi et du parent : `getpid`, `gettid`, `getppid` ≠ 0 ; `/proc/<pid>/{status, comm, cmdline, exe, maps}` avec la sémantique Linux : `exe` est un lien lisible vers un fichier exécutable ; `cmdline` en NUL ; `comm` ; lecture partielle de 31 octets ; pas de nom inventé (`P/proc-exe-cmdline-contract.md:5-17`). Wine ne doit pas être PID 1.
4. `fork()` d'un processus Wine multi-thread (clone `0x1200011`) : l'enfant ne conserve que le thread appelant ; copie de tout l'espace d'adressage (les mappings Wine, les piles, le contexte du traducteur). Le thread parent doit rester un thread non leader.
5. ptrace, vue x86-64, à relation `TRACEME` entre l'enfant et le thread créateur : `TRACEME`, `SETOPTIONS(0x11)`, `PTRACE_SYSCALL`, `CONT`, `GETREGSET`/`SETREGSET` de `NT_PRSTATUS` (216 octets), `GETSIGINFO`, et `wait4` avec les statuts Linux ; `kill(SIGKILL)` ; `tgkill(SIGSTOP)` auto-adressé avec l'ordre exact des stops (§3.6).
6. `execve` de l'enfant tracé avec événement `PTRACE_EVENT_EXEC` (`TRACEEXEC`), héritage d'un fd sans `CLOEXEC`, `dumpable` remis à 1, première trace après exec = sortie d'exec (`0x857f`, `GETEVENTMSG` 2) avec RIP égal à l'entrée de la nouvelle image (§3.6).
7. `prctl(PR_SET_DUMPABLE)` avec sa conséquence sur PEEK et sur l'ouverture de maps distante (`ptrace-peek-control` README).

### P1 : nécessaires pour la suite connue de l'injection

8. `PEEKDATA` et `POKEDATA` d'un autre processus, avec écriture forcée sur pages exécutables privées ; EIO partiel ; `ESRCH` ; ordre des erreurs (`P/ptrace-poke-design-20260930/DESIGN.md:12-48`). Invalidation des traductions du tracee avant sa reprise.
9. `/proc/<tracee>/maps` lisible par le traceur (stat, open, read), avec VMA réels du tracee et mise à jour après `mprotect`/`munmap`.
10. `SETREGSET` complet : aller-retour exact des 27 mots ; modification de RIP, RSP, RDI, RSI, RDX ; reprise correcte. `NT_PRFPREG` en lecture (512 octets) et, par symétrie avec la finalité (inferred), en écriture.
11. Livraison correcte de `SIGSEGV` (stop `0xb7f`, `si_addr` = 9) quand le tracee saute à l'adresse 9, lisible par `GETSIGINFO`/`GETREGSET`, avec `CONT(0)` qui supprime le signal et `CONT(sig)` qui le délivre. Sous Rosetta il arrive comme `SIGTRAP` par un détour interne (inferred).
12. `fcntl(F_ADD_SEALS, 0xf)` et `F_GET_SEALS` réels sur memfd (échec EINVAL sous HL non fatal pour l'exec, mais le comportement correct est exigé : aucune réussite factice). Le A/B qui refuse les seals sur noyau réel garde 509 (`docs/research/rocket-league-macos.md:270-272`), donc ce n'est pas la cause connue de 509.
13. Chemin du loader : `readlink(/proc/self/exe)` renvoie un chemin que le module peut `execve`, avec `wine64` voisin ; le nom `wine-preloader` fonctionne (GE/Cachy). Le nom `wine64-preloader.static` (Debian) mène à la sortie 1 de l'enfant (étape 10 du §3.7).

### P2 : requis par le module hors chemin ptrace

14. Threads (clone avec `CLONE_VM`), futex, signaux (`rt_sigaction`, `rt_sigprocmask`), `pipe`/`eventfd`, `epoll`, `poll`, `select`, `ioctl FIONREAD`.
15. Réseau : `socket(AF_INET/AF_INET6/AF_NETLINK)`, `connect`, `sendto`, `recvfrom`, `setsockopt`, `getsockopt`, `getrandom`, `time`, `clock_gettime`, résolution de noms par `getaddrinfo` (fichiers `/etc/resolv.conf`, `/etc/hosts`, `/etc/nsswitch.conf`).
16. `uname`, `sysconf`, `getpwuid`, `popen`/`pclose`, `iconv` avec modules gconv (`UTF-32.so`), `shm_open` et SysV shm (Wine utilise aussi shm pour X11).
17. Fichiers sondés avec absence tolérée : `/sys/devices/virtual/dmi/id/{board_name,product_name,sys_vendor}`, `/proc/bus/pci/devices`, `/tmp/eac.flag`, `/.flatpak-info`, `/run/.containerenv`, `/.dockerenv`, `/run/host/container-manager`. ENOENT n'a pas arrêté le module sous HL ; ne pas fabriquer de contenu.

### P3 : à décider ou à mesurer

18. Critère de refus de VM : inconnu. La présence de `hypervisor` dans CPUID/cpuinfo et les chaînes DMI ne sont pas démontrées comme critères (QEMU TCG accepté, WSL2 refusé). Mesurer avec un témoin qui reproduit CPUID, `/proc/cpuinfo`, DMI et sysfs du nouveau runtime.
19. `process_vm_readv`/`process_vm_writev` : non importés par le module, ENOSYS sous HL sans conséquence connue ; à fournir si trivial.
20. Persistance du tracee après la sortie du bootstrapper : détacher automatiquement à la mort du traceur (comportement Linux) ; le jeu doit continuer et être adopté par un ancêtre.

### Points propres à FEX et à Darwin

- FEX Linux transmet `PEEKTEXT`, `PEEKDATA`, `POKETEXT`, `POKEDATA`, `ATTACH`, `DETACH` au noyau hôte et répond EPERM à tout le reste (`P/fex-eac-feasibility.md:13`). Sous Darwin il n'y a aucun noyau Linux : toute la relation tracer/tracee, les statuts `wait4`, les signaux, `/proc`, le fork copy-on-write et l'exec doivent être émulés dans le runtime.
- Les points de modification de FEX sont listés dans `docs/research/rocket-league-eac-technologies-macos-2026-10-01.md:124-130` : `CpuStateFrame`, frontière `HandleSyscall`, `SignalDelegator`, `InvalidateGuestCodeRange`, `Thread.cpp`.
- Le POKE d'un processus dans le code d'un autre doit atteindre le cache de traduction du tracee ; avec deux processus hôtes distincts cela exige un canal de commande entre les deux instances FEX ou un arrêt coopératif du tracee (la leçon de HyperLinux : mémoire distante, snapshot au thread propriétaire, jamais d'accès direct au vCPU d'un autre processus ; `docs/research/rocket-league-eac-technologies-macos-2026-10-01.md:114`).
- Les pages Darwin de 16 Kio contre 4 Kio invité, la double mapping RW/RX de pages mémoire (voir ci-dessous) et la politique W^X de macOS pour le JIT sont des contraintes séparées, hors de ce digest.
- Le jeu en session protégée contient aussi des pages `memfd` de 1 page en `r-xs` avec alias `rw-s` (mappings partagés exécutables à `6fffffe82000`, `6ffffff00000`, `6ffffff0f000`, `7febb1da8000`, alias `rw-s` à `7febae934000` et suivants), proches de `kernel32.dll`/`ntdll.dll` (`game-process-maps.txt:571-577,1033-1035,1125`). Origine non établie (Wine ou module). Elles exigent `memfd` + `mmap(MAP_SHARED, PROT_EXEC)` avec un second mapping RW du même objet (`inferred` pour la nécessité).

## 7. Inconnues et pièges d'interprétation

- Aucun run n'atteint 301 en dehors du PC physique. Les 509/210 sont un verdict global : « les ptrace réussissent » ne prouve pas que EAC accepte l'environnement (aveu répété dans `P/eac-ptrace-regset-transition/report.md:45`).
- Le contenu du second ELF, l'argv de l'exec, la fonction appelée (`dlopen` seulement par déduction) et la raison du code `(#1)` restent inconnus. Le module est fortement obfusqué (dispatcheur indirect, constantes opaques) : l'analyse statique ne relie pas `509` aux appels `ptrace` (`P/eac-regset-static/README.md:3`, `P/eac-current-module-static-20260929/eio-return-path-20260930.md` « Limite de causalité »).
- La sémantique des registres a été mesurée sur un noyau Linux x86 sous QEMU TCG et deux sondes sur PC, pas sur le module EAC.
- Les journaux HL mélangent plusieurs processus et threads sans préfixe de PID. Ne pas apparier des lignes voisines (`P/eac-parent-status-transition/README.md:90-97`).
- Trois runs récents de `GR` donnent des totaux de requêtes `ptrace` presque identiques (183, 183, 185 selon le run) ; les écarts à 65 contre 66 `GETREGSET` entre anciens runs ne sont pas attribués (`P/eac-runtime-conf-availability-20260929/README.md:22`).
- Chemin DMI : l'analyse statique de l'ancien module dit ne pas trouver `/sys/bus/pci/devices` en clair, la trace montre `/proc/bus/pci/devices`. Les chaînes du module sont obfusquées ; seule la trace d'exécution fait foi.

## Annexe A : constantes et empreintes utiles

| Objet | Valeur |
| --- | --- |
| Bootstrapper | `RocketLeague_EAC.exe`, 3 975 920 octets, SHA-256 `903e7aba292448d68976597efe3f0dc6f5cc66ef1889e3c1efcaf3eaf4e614d1` |
| Jeu | `RocketLeague.exe`, 38 873 528 octets (0x25129b8), SHA-256 `ca3a9075422f6c79c0cd3ef8c4edd5c9ae1cc48f51b1077d8e18fb495f9738fa` ; table Authenticode à 0x2510000 (10 680 octets) |
| Module ancien | CDN 9 681 573 octets `ce28ef66…`, ELF `c6bf6c7d…` |
| Module courant | CDN 13 825 695 octets `74b6cab9…`, ELF `1b9faa54…`, premier export `a` à 0x44f00, 21 appels `ptrace`, segment RX non sectionné `[0xa7e000, 0xd33cc8)` |
| Second ELF | 7 757 608 octets (0x765f28) pour le module courant, 6 618 885 pour l'ancien |
| Options ptrace | 0x11 = `TRACESYSGOOD` (1) \| `TRACEEXEC` (0x10) |
| Requêtes | `TRACEME` 0, `PEEKDATA` 2, `POKEDATA` 5, `CONT` 7, `SYSCALL` 24, `SETOPTIONS` 0x4200, `GETSIGINFO` 0x4202, `GETREGSET` 0x4204, `SETREGSET` 0x4205 |
| Statuts `wait4` | `0x137f` SIGSTOP, `0x57f` SIGTRAP, `0x857f` stop de syscall, `0x4057f` exec, `0xb7f` SIGSEGV |
| `clone` | `0x1200011` |
| Oracle noyau | Linux 6.12.107+deb13-amd64 sous QEMU 10.0.13 TCG ; image Docker `sha256:cefd9cb7…` ; noyau SHA `2b2358b3…` |

## Annexe B : répertoires `eac-*` lus

Pour les répertoires de runs, le verdict est celui du journal du launcher (`anticheatlauncher-0.log` ou `result.json`) et le SHA est celui du runtime HyperLinux. « 509/210 » = `Unexpected error. (#1)`.

| Répertoire (sous `P/`) | Run et runtime | Verdict | Apport |
| --- | --- | --- | --- |
| `eac-cachyos-dns-trace-20260929` | `2cd7375c` | pas de verdict (120 s) | erreur curl 6 ; deux `connect` locaux avec statut erroné |
| `eac-cachyos-hyperlinux-20260929` | `2cd7375c` | arrêt à 32 Mio de journal | échec de la création du bureau Wine 11 |
| `eac-cachyos-process-trace-20260929` | RLX, branche B | 509/210 | `RocketLeague.exe` exécute son démarrage (PID 2881, parent 2863) |
| `eac-cachyos-readonly-section-20260929` | `2cd7375c` + ntdll `e6ad055d` | pas de verdict | `linux64`, puis erreur curl 6 |
| `eac-cachyos-tgkill-20260930` | `a8829b17` (07:43), `872a6f2a` (08:00) | 509/210 | l'enfant atteint l'exec ; `GETSIGINFO` ESRCH puis réussi (26 `GETREGSET`) ; stop de sortie d'exec absent |
| `eac-current-module-static-20260929` | statique | n/a | module courant décodé ; 21 appels `ptrace` ; imports ; code dans un segment non sectionné |
| `eac-current-payload-20260929` | `6601c6ec` | 509/210 | payload 13 825 695 octets ; 66 `GETREGSET` ; pont Heroic distinct |
| `eac-fpregset-20261001` | `77450112` | 509/210 | `NT_PRFPREG` accepté ; deuxième `SETREGSET` EIO |
| `eac-general-regset-20261001` | `f3f7ca30` | 509/210 | trace la plus avancée (`GR`) |
| `eac-heroic-bridge-ab-20260929` | Cachy, branches A et B | 509/210 des deux côtés | le pont Heroic ne change rien ; deux essais préalables écartés (assertion Rosetta `xfeatures`) |
| `eac-jit-invalidation-20261001` | `acc0844f` | 509/210 | `SETREGSET` accepté ; `NT_PRFPREG` EINVAL |
| `eac-notify-fpset-20260929` | `02c77f34`, `62ed4284` | 509/210 | 66 captures ; rôle de l'enfant inconnu |
| `eac-parent-capture-fault` | n/a | n/a | plantage Rosetta d'un pointeur de contexte corrompu, non EAC |
| `eac-parent-proc-comparison*` (4 répertoires) | extraits de `LOG-*` | n/a | fenêtres des lectures `/proc` du parent |
| `eac-parent-shell` | script | n/a | lance `wine64 …RocketLeague_EAC.exe --verbose` sous un vrai shell parent |
| `eac-parent-status-transition` | `3c1babf6` | 502 puis 509/210 | `/proc/1/status` ; TRACEME EINVAL de l'enfant |
| `eac-peek-extended-diagnostic-20261001` | `73272999` | 509/210 | fenêtres élargies (8192 enregistrements) ; 10 `PEEKDATA` puis 1 `POKEDATA` |
| `eac-postexec-comparison-review-20260930` | doc | n/a | préparation de la comparaison après exec |
| `eac-postexec-exit-20260930` | `7cadfcc4` | 509/210 | sortie d'exec correcte (RIP `0x7fffffffa000`, `eflags` `0x202`) ; entrée de `brk` puis deux `SIGKILL` |
| `eac-postexec-exit-comparison-20260930` | comparaison | 509/210 | 27 `GETREGSET` valides ; 23 syscalls identiques ; kill à l'entrée de `brk` |
| `eac-proc-remote-maps-20260930` | `1f842509` | 509/210 | maps distante lue (stat 0, fd 131, 2580 octets, EOF) ; `PEEKDATA(0x7ffffdffb000)` EINVAL |
| `eac-process-lifecycle-20260930` | `44d98bdc` | 509/210 | après la sortie de `prctl(PR_SET_NAME)`, le parent TID 76 appelle deux fois `kill(80, SIGKILL)` (`README.md` du run) |
| `eac-proton-runtime.md` | doc | n/a | pas de preuve que `PROTON_EAC_RUNTIME` soit en cause |
| `eac-ptrace-audit-linux-20260929` | RLX, LD_AUDIT | 509/210 | 68 appels : TRACEME, SETOPTIONS(150,0x11), 66 GETREGSET, tous 0 |
| `eac-ptrace-audit-regset-linux-20260929` | RLX | 509/210 | 66 requêtes de 216 octets ; registres ARM64 |
| `eac-ptrace-callsite-audit-20260929` | `62ed4284` | assertion Rosetta | aucun appel `ptrace` capturé |
| `eac-ptrace-options-transition` | `a4178585` | 509/210 | SETOPTIONS ok ; SYSCALL EINVAL |
| `eac-ptrace-peek-20260930` | `48a7ee6a` | 509/210 | 10 `PEEKDATA` réussis, puis `POKEDATA` EINVAL ; fenêtre de 128 enregistrements saturée (`progress-finding.json`) |
| `eac-ptrace-poke-20260930` | `173201a1` | 509/210 | `POKEDATA` ok, `CONT`, SIGTRAP `0x57f`, `GETREGSET` EIO, octet d'origine restauré, deux `SIGKILL` (`progress-finding.json`, `observed_delta`) |
| `eac-ptrace-regset-transition` | `6601c6ec` | 509/210 | 66 GETREGSET ; F_ADD_SEALS |
| `eac-ptrace-syscall-transition` | `0701cfc1` | 509/210 | SYSCALL ok ; GETREGSET ESRCH |
| `eac-ptrace-transition` | `361882f5` | 509/210 | TRACEME ok ; SETOPTIONS EINVAL |
| `eac-read-regsets-20260929` | `a5453fd7` | 509/210 | relance du runner `eac-current-payload` avec le runtime « read-regsets » ; 66 copies `ptrace regset-copied format=arm64 requested=216` |
| `eac-readlink-boundary-20260929` | `2cd7375c` | 509/210 | `readlinkat` capturé à la frontière x86 |
| `eac-readlink-observation-20260929` | `a864312b` | 509/210 | aucun événement hôte (Rosetta court-circuite) |
| `eac-regset-diagnostic` | `50385ea2` | 509/210 | `iov_len` = 216 |
| `eac-regset-static` | statique | n/a | aucun lecteur x64 identifié |
| `eac-remote-maps-20261001` | `29649335` | 509/210 | maps distante réelle (stat 0, open 131, 2580 octets, EOF) ; `PEEKDATA` suivant EINVAL |
| `eac-remote-maps-finding-20260930` | analyse | 509/210 | ENOENT sur `/proc/80/maps` ; SHA du module mappé mesuré |
| `eac-remote-peek-20261001` | `80953d6e` | 509/210 | maps et `PEEKDATA` réels réussissent ; fenêtre bornée à 16 enregistrements |
| `eac-remote-poke-20261001` | `fb965df1` | 509/210 | `POKEDATA` réel ; 442 enregistrements de fenêtre |
| `eac-runtime-conf-availability-20260929` | `62ed4284` | 509/210 | `runtime.conf` accessible : aucun effet |
| `eac-runtime-conf-research.md` | doc | n/a | aucune source externe n'explique l'ouverture |
| `eac-setregset-diff-20261001` | `cf568d78` | 509/210 | différence demandée : RIP moins 1 |
| `eac-signal-regs-20261001` | `e2f12c45` | 509/210 | registres x86 réels au SIGTRAP INT3 (`stops_after_breakpoint`: seq 58, statut 1407) |
| `eac-stat-window-20260930` | `7b7133a4` | 509/210 | chemin `/proc/80/maps` capturé ; SHA du module mappé mesuré (`1b9faa54…`) |
| `eac-tracer-stat-20261001` | `f3ee91bd` | 509/210 | `newfstatat /proc/80/maps` ENOENT juste avant les deux `SIGKILL` (`stat-window-summary.json`) |
| `eac-tracer-window-20260930` | `b797c823` | 509/210 | `newfstatat` ENOENT juste avant deux `SIGKILL`, chemin encore inconnu (`tracer-window-summary.json`) |
| `eac-x86-ptrace-20260929` | `4c719659` | crash wineserver | première tentative du pont x86 |
| `eac-x86-ptrace-exec-diagnostic-20260929` | `cf7ad808` | 509/210 | 13 `ptrace` x86 qualifiés |
| `eac-x86-ptrace-tgkill-20260929` | `3d37afab` | 509/210 | 3 `GETREGSET` x86 réels |
| `eac-native-module-capture-plan.md` | doc | n/a | plan de capture du memfd |
