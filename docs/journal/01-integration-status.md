# État d'intégration moteur et noyau, 2 octobre 2026

Document court, mis à jour à chaque jalon d'intégration. Détails du moteur dans `fexcore-darwin-port.md`, du noyau dans `src/lkd/docs/status/`.

## Moteur (FEXCore sur Darwin, `src/fexdarwin`)

MESURE. Code x86-64 exécuté par le JIT FEXCore dans un processus macOS arm64 natif, sans Rosetta ni virtualisation. Témoins possédés, tous rejoués avec une base invitée de 8 Tio : entiers 9524/9524, SIMD et x87 109/109 identiques à l'oracle Rosetta, TLS 25/25, mémoire 39/39, code automodifiable 20/20, syscalls 50/50, mémoire sous 4 Gio 19/19 (dont 0x7ffe0000), threads 13/13, sauts vers adresse non mappée 15/15, récursion profonde 4/4, débordement de pile récupéré 7/7. Performance sur noyaux C : au niveau de Rosetta (0 à 23 % plus lent), base invitée au plus 4 %.

## Noyau (LKD, `src/lkd`)

Coeur : dispatch, mémoire invitée protégée contre les fautes, traductions d'ABI, 599 assertions. Modules mm et fs en cours (agents), task, sig, procfs, net et ptrace à lancer.

## Raccordement

`src/fexdarwin/src/LkdGlue.cpp` implémente `lkd_engine_ops` sur FEXCore : registres au format `user_regs_struct`, état FP FXSAVE et XSAVE, interruption par signal hôte réservé, création de threads, invalidation de code, cadres `rt_sigframe`, verrous de fork. `LkdLoader.cpp` charge l'exécutable et son PT_INTERP par des appels système LKD (openat, pread64, mmap, mprotect), construit la pile initiale et l'auxv avec AT_PAGESZ = 4096. Mode activé par `FEXDARWIN_LKD=1`, racine invitée `FEXDARWIN_ROOT`.

MESURE (14:58) : `lkd_task_create_main` crée la tâche 2 ; le premier appel `mmap` du chargeur répond ENOSYS, attendu tant que le module mm n'est pas livré.

Ajouts de 15:20 (compilés, témoins hors LKD rejoués sans régression ; pas encore exercés en mode LKD faute des modules mm, task et sig) :

- Interruption d'un thread qui tourne dans le code traduit. Le signal hôte réservé (SIGUSR2) arrive dans le gestionnaire ; si le PC est dans le code du JIT, hors section à signaux différés et hors séquence d'appel système, les registres invités sont déversés (même méthode que FEX sur Linux pour les signaux asynchrones) et le thread repart sur un trampoline qui appelle `lkd_signal_next` sur la pile ordinaire du thread (il peut donc s'y bloquer pour un arrêt ptrace), puis rentre dans la boucle du répartiteur. Dans les autres cas (répartiteur, C++ hôte), un thread de relance renvoie le signal toutes les millisecondes tant que la demande tient et que le thread n'est pas dans LKD. Témoin `w15_async` écrit : tgkill vers un thread qui boucle sans appel système, kill vers le processus pendant que le thread principal boucle, contrôle des registres entiers et SSE après cinq interruptions.
- Fautes synchrones en mode LKD : `lkd_fault` puis le même trampoline, au lieu de l'ancienne table de signaux interne.
- `exec_image` : chargement en place après le point de non-retour (code traduit invalidé sur toute la fenêtre, registres d'un processus neuf, EFLAGS 0x202, FPU remise à zéro, AT_EXECFN = chemin passé à execve). Le chargeur passe par `lkd_dispatch` (appel interne au noyau) et non `lkd_syscall` : un traceur en PTRACE_SYSCALL ne doit voir que l'entrée et la sortie d'execve, pas les mmap du chargeur.
- (16:10) Les deux restes de cette liste sont faits : classement MAPERR/ACCERR et `[stack]` (par `start_stack` du module mm), voir plus bas.

## Constat qui a orienté le travail

Annoncer des pages de 16 Kio à glibc fait échouer ld.so (« ELF load command address/offset not page-aligned » sur libc.so.6, mesuré avec l'ancien hôte). La vue logique de 4 Kio du module mm est donc obligatoire, comme prévu par la fiche de décision.


## Premier passage de bout en bout sur LKD (16:00 à 16:15)

MESURE, hôte `fexdarwin-run` lié à un instantané privé de `liblkd.a` (modules core, mm, fs, task livrés ; sig en cours), journaux sous `evidence/fexcore-darwin/runs-lkd/` :

- Neuf témoins sur treize passent en mode LKD : `w01_hello`, `w03_simd`, `w04_tls`, `w05_mem` (39/39), `w06_smc` (20/20, mode SMC complet), `w08_sys`, `w09_lowmem`, `w10_threads`, `w13_deeprec`. Les quatre qui échouent (`w02_int`, `w11_badjump`, `w14_overflow`, `w15_async`) passent tous par la livraison de signaux : `lkd_signal_next` rend -1 malgré un gestionnaire installé (module sig en cours, reproduction transmise à son agent). Côté moteur, l'interruption fonctionne : le thread qui boucle sans appel système est arrêté à `rip 0x2aaaaaaaf7ea` et atteint le point sûr.
- Programmes glibc Debian 2.41, dynamiques, par `ld.so`, racine Debian montée en lecture seule : `/usr/bin/true`, `/bin/ls /`, `/bin/uname -a` (`Linux obelisk 6.12.0-lkd ... x86_64`), `dash -c` avec substitution de commande (fork réel). `vfork` puis `execve` puis `exit_group` du fils, SIGCHLD livré au gestionnaire de dash (cadre construit, `rt_sigreturn`), `wait4` rend le fils et `status=0` s'affiche ; ensuite le processus ne sort pas (attente perdue dans `wait4` entre deux processus hôtes, reproduction transmise à l'agent task).
- Wine GE-Proton11-7 : `/opt/ge-proton/files/bin/wine --version` affiche `wine-11.0 (Staging)` et sort avec 0. Les appels `socket(AF_UNIX)` répondent ENOSYS (module net en cours) ; c'est la condition suivante pour wineserver et wineboot.

Corrections faites pour y arriver :
- Moteur : base de fenêtre de 8 Tio passée à LKD (sinon mode identité des tests) ; FEXCore créé après la réservation de la fenêtre, parce que le répartiteur grave la base invitée comme constante ; mode SMC complet par défaut en LKD et CRC du mode complet calculé sur la copie hôte (`Core.cpp`) ; requêtes de plage exécutable et classement MAPERR/ACCERR lus dans la table des VMA de LKD (`LkdShim.c`, seul fichier qui inclut des en-têtes internes de LKD) ; stdio invité ; début du tas et de la pile communiqués à mm ; pile initiale dans l'ordre de Linux et disposition du processus pour `/proc` et `PR_GET_AUXV` ; adresses de chargement à la manière de Linux (`ELF_ET_DYN_BASE` = 2/3 de la fenêtre, interpréteur par mmap descendant) ; `fork_child`, fin du seul thread principal, SIGPIPE ignoré ; montages `FEXDARWIN_BIND` et racine en lecture seule `FEXDARWIN_ROOT_RO=1`.
- Témoin `w05_mem` : l'assertion d'alignement de 16 Kio venait de l'ancien hôte ; Linux garantit 4 Kio, l'assertion suit Linux.

## Signaux, SMC rapide et témoins complets sur LKD (19:00 à 19:40)

MESURE, journaux `evidence/fexcore-darwin/runs-lkd/` :

- Les treize témoins du lot passent en mode LKD (`w01` à `w06`, `w08` à `w11`, `w13` à `w15`) avec les binaires ET_EXEC de `witnesses/elf-x86_64/bin-exec/` (`EXEC=1 ./build.sh`). Les échecs de `w02`, `w11`, `w14`, `w15` venaient de nos binaires : des PIE statiques dont personne n'appliquait les relocations RELATIVE (le gestionnaire de signal lu dans la structure valait 0, soit SIG_DFL). Un noyau Linux ne les applique pas non plus ; l'ancien hôte le faisait à sa place. Trouvé par l'agent sig (`docs/status/sig.md`, `docs/requests/sig-4.md`).
- Le chargeur oubliait la page d'un segment PT_LOAD sans données de fichier dont le bss tient dans une page (`filesz == 0`) ; Linux la mappe (`elf_load` appelle `vm_brk_flags` depuis le début de page). Corrigé dans `LkdLoader.cpp`.
- `w07_fault` imprime `PASS 99/99` puis `ret` de `run` saute à une adresse de pile (stack corrompue après le dernier test de débordement) ; même symptôme que sur l'ancien hôte, donc pas propre à LKD. Non élucidé ; `w14` couvre le même scénario et passe. À reprendre si un programme réel montre le même défaut.
- Interruption asynchrone exacte. `w15_async` échouait 3 fois sur 8 (invariants de registres) : FEX reconstruit RIP depuis une table qui ne contient que les instructions à effets de bord et au mieux « as close as FEX can get » (`Core.cpp`, `RestoreRIPFromHostPC`) ; une interruption au milieu de la traduction d'une instruction x86 la rejoue après le gestionnaire. Corrigé dans FEXCore : une entrée de table pour chaque instruction invitée, et `IsHostPCAtGuestInstructionBoundary` (PC hôte exactement au premier octet de la traduction d'une instruction) ; le gestionnaire du signal de coup de pied n'interrompt que dans ce cas, sinon le fil de relance renvoie le signal une milliseconde plus tard. Résultat : 30 exécutions de `w15_async` sur 30 réussies (16 contrôles), latence maximale de livraison 16 ms. Enjeu réel : Wine suspend et lit le contexte des threads avec SIGUSR1 ; une reprise inexacte corromprait le jeu.
- Détection du code automodifiable rapide (`mtrack`) rétablie : le module mm retire l'écriture des pages hôtes qui contiennent du code traduit (`lkd_mm_watch_code`), la première écriture revient par le gestionnaire (`lkd_mm_code_write_fault`, qui appelle `invalidate_code`), y compris pour les copies du noyau (patch `usermem.c`, demande mm-5). `w06_smc` 20/20 dans ce mode. Détail à ne pas filtrer : macOS rapporte l'écriture sur une page en lecture seule non résidente comme faute de traduction (DFSC 0x06), pas de permission.
- Performance (`w12_bench` sous LKD, deux exécutions) : entiers 393/403 ms, mémoire 38/37, flottants 371/369, tri 183/185 ; Rosetta 379/30/351/141. La table de RIP complète et le mode LKD coûtent environ 4 % sur les entiers.
- Module sig terminé (703 assertions), mm terminé (685 assertions + `test_task_bigwin`), task (616), fs (747). net, procfs et ptrace en cours de reprise après deux coupures par la limite de session de l'API.

## Incident machine du 2026-10-02, 20:21 : panique noyau, cause et garde-fous

MESURE (`/Library/Logs/DiagnosticReports/panic-full-2026-10-02-202119.0002.panic`) : « zone map exhausted while allocating from zone [VM map entries] ... (10G, 177893865 elements allocated) », 332 processus `fexdarwin-run` vivants. Le dernier essai de Wine (`logs/wine-linux-m2-version-013.log`, Wine construit depuis voile-wine, de 20:18:46 à 20:20:23) a lancé des processus Windows en boucle (identifiants 0x2c à 0x3e4, « failed to create desktop window »), chacun étant un processus hôte ; des exécutions dont l'enveloppe de délai avait été tuée laissaient leurs processus vivants. MESURE après redémarrage : un processus invité ordinaire a environ 400 régions VM (dash 400, client Wine 380 à 410, wineserver 460).

Garde-fous ajoutés (`src/fexdarwin/src/LkdGlue.cpp`, `scripts/run-with-timeout.sh`, `src/lkd/src/task/task_priv.h`) :
- chaque processus hôte suit `FEXDARWIN_SUPERVISOR_PID` (l'enveloppe, ou à défaut le premier processus de la famille) et s'arrête en 0,5 s quand il disparaît ; MESURE : `kill -9` de l'enveloppe, les 5 processus invités disparaissent en moins de 2 s ;
- chaque processus s'arrête de lui-même au-delà de 200 000 régions VM ;
- au plus 96 processus vivants par famille (512 avant) ;
- règles de travail : jamais d'invité hors `run-with-timeout.sh`, au plus deux agents qui lancent des invités en même temps, comptage des `fexdarwin-run` restants après chaque essai.

### Cause réelle (20:40) et campagne de sondes EAC

MESURE (`build-lead/chunk.c`) : XNU découpe un mapping anonyme dont la protection maximale n'est pas nulle en entrées de 128 Mio (`ANON_CHUNK_SIZE`). La fenêtre invitée de 2^46 octets réservée par `mmap(PROT_NONE, MAP_NORESERVE)` coûtait 524 288 entrées de la table du noyau par processus ; `vmmap` les fusionne à l'affichage (320 régions affichées). 332 processus × 524 288 = 174 millions, ce que la panique rapporte (177 millions). La même fenêtre réservée par `mach_vm_map` avec protection maximale nulle est une seule entrée. Correctif dans `src/lkd/src/mm/window.c` (réservation et remise à vide en protection maximale nulle, recréation en mémoire anonyme avant tout accès, région par région) : 10 programmes de test mm et `test_task_bigwin` passent ; un processus invité compte maintenant 120 à 160 entrées (compteur `FEXDARWIN_REGION_LOG=1`). Le garde-fou de 200 000 régions reste en place ; il avait arrêté proprement, avant correction, la sonde qui l'atteignait.

Sondes EAC (`witnesses/eac-probes/run-probes.py`, 39 sondes x86-64 de la campagne HyperLinux, valeurs attendues du vrai Linux, `evidence/eac-probes/20261002T204027/`) : 1 592 contrôles sur 1 824 passent. Passent entièrement : cycle de vie de `/proc/<pid>`, jeux de registres généraux et FP, contrat siginfo, arrêts sur signaux et double INT3, tgkill entre processus et entre threads, état brk. Échecs groupés : POKEDATA dans du code en lecture seule (EIO au lieu de l'écriture forcée de Linux) et code réécrit non réexécuté ; arrêt d'événement d'exec (GETEVENTMSG, GETSIGINFO) ; quelques sondes qui sortent avant leur premier contrôle. Transmis à l'agent d'intégration.

### Sondes EAC après intégration (21:00)

MESURE (`evidence/eac-probes/20261002T205719/`, 40 sondes, 4 376 contrôles, les sondes vont maintenant jusqu'au bout) : 38 échecs (232 sur 1 824 avant). Corrections : POKE forcé dans du code en lecture seule partageant sa page hôte avec des données (`src/mm/remote.c`) ; lien traceur réécrit à tort après l'exec d'un fils de fork (`src/ptrace/life.c`, GETEVENTMSG et GETSIGINFO en ESRCH sur 14 sondes) ; invalidation du code traduit demandée par le fil de service de mm (moteur : `InvalidateCode` sans thread invité). Les 38 restants : 31 contrôles de sondes qui font un `MAP_FIXED` à 0x7ffff7ff0000 ou 0x7ffffdffb000, au-delà de la fenêtre invitée de 2^46 octets (limite d'architecture : avec une base décalée, l'espace utilisateur macOS s'arrête à 0x7ffffe000000 et la zone de nano malloc occupe 0x600000000000, donc l'espace Linux complet de 128 Tio n'est pas atteignable ; Wine et EAC ne demandent pas ces adresses fixes, les adresses qu'EAC lit viennent de `/proc/<pid>/maps`) ; 6 contrôles où `CONT(SIGTRAP)` à un arrêt de livraison de SIGTRAP sans gestionnaire tue le traceé avec le statut 5, comportement de Linux selon notre lecture (valeur attendue sans oracle Linux dans le répertoire de la sonde, ancienne exécution HyperLinux en échec aussi).

## EAC 301 sur la pile fexdarwin + LKD + Wine Linux (22:12)

MESURE (`evidence/eac-runs/20261002T221224/`, lancement authentifié par `run/eac-run.py`, journal EAC expurgé `files/.../anticheatlauncher.log`) : le bootstrapper EAC 1.9.3 choisit `linux64`, télécharge le module (13 825 695 octets, HTTP 200 depuis modules-cdn.eac-prod.on.epicgames.com), écrit « Starting Wine module mapping, Wine version: 11.0 » puis, 2,4 s plus tard, « Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game' », « Successfully initialized the Easy Anti-Cheat module, waiting for game window to become visible », « Terminating with: 0 ». `RocketLeague.exe` est chargé dans un nouveau processus. Le traceur montre la séquence connue (retour vers l'adresse 9 dans le JIT, « NoExec instruction in entry block: 9 »). Empreintes du jeu d'origine inchangées. Référence : PC Linux réel 301 en 0,5 s, QEMU TCG 301 en 3,5 s ; toutes les piles ARM précédentes 509/210 ou 502.

Ce qui a débloqué : (1) le bootstrapper demande dix allocations RWX à adresse fixe de 0x7001'0000'0000 à 0x700a'0000'0000 et affichait « Failed to initialize Wine helper buffer » avec une fenêtre invitée de 2^46 (trouvé par l'agent Wine avec `+virtual`) ; la fenêtre invitée va maintenant de la base hôte choisie au démarrage (fin de la plus haute région occupée au-dessus de 0x7d80000000, MESURE : une réservation de 12,8 Gio placée par l'ASLR atteint parfois 0x7cb9400000) jusqu'à 0x7ffffe000000, soit environ 127,5 Tio d'adresses invitées, une seule entrée noyau ; pile en haut de fenêtre, exécutables PIE à 0x555555554000 comme Linux ; 16 témoins passent, 20 démarrages sur 20 sans échec de réservation. (2) le pilote d'affichage présente la boîte de dialogue d'erreur (elle restait invisible) ; (3) la persistance du registre du préfixe (attente de wineserver).

Étape suivante : le processus du jeu (fenêtre, D3D11 par Neptune vers Metal). Défauts vus dans ce même essai : `MAP_SHARED` refusé à 0x6fffffd17000 (adresse et décalage non congruents modulo la page hôte de 16 Kio), allocations de Wine au sommet de l'espace (0x7fffffb50000 et au-dessus, hors fenêtre), AF_NETLINK absent, option IPv6 62 non gérée.

## Jeu au menu principal, connecté à Psyonix avec EAC actif (22:54)

MESURE (`evidence/eac-runs/20261002T225400/`, `files/.../Launch.log` et `anticheatlauncher.log` expurgés), après le correctif wine-7 (phase des memfd et vues de code partagées) : EAC 301 ; `RocketLeague.exe` charge `EOSSDK-Win64-Shipping.dll`, nos `dxgi.dll`/`d3d11.dll` Neptune, `xaudio2_9redist.dll` ; « Found D3D11 adapter 0: Apple M5 Max », « Shader platform (RHI): PC-D3D-SM5 », 466 923 shaders chargés ; requêtes HTTPS Psyonix 200 ; connexion EOS par code d'échange, « Platform auth ticket completed with success 'True' », « PsyNet auth RPC succeeded », « Login successful », WebSocket PsyNet connecté, carte `MENU_Main_p` chargée et mise en jeu ; le rendu Neptune crée une chaîne d'échange de 3 images 2560x1440 présentée par wsd en plein écran. Défaut restant : aucune image n'atteint l'écran (wsd `presented=0`). Durées : menu à 76 s, connexion Psyonix à 100 s (chargement du cache de shaders 30 s).

## Premier affichage du jeu par Metal (23:07)

MESURE (`evidence/eac-runs/20261002T230701/`, capture de la seule fenêtre du jeu `evidence/wine-linux/game-keep/rl-title-20261002T230701.png`) : l'écran titre « ROCKET LEAGUE SEASON 24 » est rendu par le client Neptune D3D11, le serveur de rendu DXMT sur Metal, et présenté par wsd dans une fenêtre 1280x720 (mode fenêtré réglé dans la configuration utilisateur du préfixe, `TASystemSettings.ini`, jamais dans l'installation). 96 présentations, 95 affichées ; attente de fence moyenne 2,3 ms. Cadence pendant le chargement : 148 ms par image en moyenne sur les 60 premières (environ 7 images par seconde). Défauts ouverts : les présentations s'arrêtent après 96 images (le journal du jeu s'arrête à 113,9 s) ; le serveur de rendu ignore des appels VSSetShader/PSSetShader (« argument handle is unknown ») et signale des objets non enregistrés de types 61 et 65.

## Menu principal stable à 60 images par seconde (2026-10-03, 00:30)

MESURE (`evidence/eac-runs/20261002T233036/`, capture de la fenêtre du jeu `evidence/wine-linux/game/m2-win1084.png`, qui montre le nom d'affichage Epic du joueur) : menu principal atteint et stable, 1 680 présentations en 150 s. Le blocage après 96 images venait de Wine : `gethostbyname` passe par nsiproxy, qui lisait la liste des interfaces par `if_nameindex()` ; sans AF_NETLINK dans LKD elle rendait NULL, nsiproxy le déréférençait en tenant son verrou, et tous les appels suivants restaient bloqués. Corrigé dans Wine (repli `SIOCGIFCONF`, `GetAdaptersAddresses` sans interface rend ERROR_NO_DATA comme Windows) ; demande LKD wine-8 pour des interfaces réelles. Temps d'image au menu : 16,1 à 16,4 ms (affichage à 60 Hz), latence de soumission à l'affichage 12 à 16 ms. Profil processeur du jeu au menu, attentes exclues : environ 93 % dans des appels système LKD (`getrusage` en boucle 1,4 s sur 5 s ; `mincore` pour VmRSS de `/proc/<pid>/status` 0,4 s), 5 % code JIT, 1,5 % C++ de FEX et LKD. Restent : 14 paires de shaders jamais créées côté hôte (28 commandes rejetées sur des milliers, menu visuellement correct), entrées pour jouer, essai de jeu hors ligne, puis partie publique.

## Jeu en 3D hors ligne et sortie propre (2026-10-03, 01:01)

MESURE (`evidence/eac-runs/20261003T005434/`, capture `evidence/wine-linux/play/d1-win1362.png`) : navigation écran titre, menu, Training, Free Play par entrées injectées (wsdctl), carte `CHN_Stadium_Day_P` en Free Play (hors ligne, sans autre joueur), conduite 70 s. Temps d'image en 3D : 23,0 ms en moyenne sur 2 160 images (environ 43 i/s), fence 2,4 ms ; wsd 22,5 ms en moyenne, maximum 129 ms, latence d'affichage 13,7 ms ; menu 16,1 ms. Processeur pendant la conduite : processus du jeu, part active, JIT 43 %, appels système 42 % (sockets), C++ hôte 15 % ; serveur de rendu 86 % d'un cœur, surtout des copies mémoire (memmove). Shaders : 0 identifiant inconnu au menu et en 3D. Sortie par les menus du jeu : code 0 en 441,9 s, « Exit: Exiting. », EAC « Launcher finished with: 301 » puis « Terminating with: 0 », aucun processus restant, empreintes inchangées. Défauts avant une partie publique : environ 1 s entre une entrée et son effet ; texte de l'interface (Scaleform) dessiné en partie ; cadence 3D limitée par les copies du serveur de rendu.

## Régression « bloqué sur l'écran d'initialisation » corrigée (2026-10-03, 02:35)

Symptôme (MESURE, essais `20261003T014348` à `022545`, 8 essais sur 8) : 0 à 3 images présentées puis plus rien ; le jeu continue son chargement jusqu'à ~80 s puis tout s'arrête. Le dernier essai bon était `013654`.

Cause (MESURE) :
- Vidage des fils invités, sans rien changer pour l'invité : nouvel outil `FEXDARWIN_DUMP_DIR` dans `LkdGlue.cpp`. Il donne par fil l'appel système en cours, ses arguments, le mot futex et le propriétaire du mutex glibc, ainsi que les adresses de retour avec module et décalage.
- Résultat : le fil de présentation (`npt_unix_wsi_wsd.c:277`) attend `g_lock` alors qu'il en est lui-même propriétaire (`__owner` = son tid). Le drain (ligne 322) et le fil d'événements du pilote (`on_presented`) attendent derrière lui.
- Aucune source ne prend ce verrou deux fois. Le déverrouillage a été perdu.
- La recompilation de nptunix à 01:43 a déplacé `.plt` (0x4020–0x41d0) dans la même page hôte de 16 Kio que `.got.plt`, `.data` et `.bss` (où se trouve `g_lock` à 0x65a0).
- Chaque appel par la PLT retraduit ce code et reprotège la page (SMC mtrack). Chaque écriture dans `g_lock` passe donc par `lkd_mm_code_write_fault`.
- Sous contention, la borne `MAX_RETRIES` (4 réponses « réessayer » consécutives, état « surveillé » lu seulement avant le traitement) refusait des écritures légitimes. L'invité recevait SIGSEGV.
- Wine abandonne alors l'appel unix en cours et revient côté PE avec le mutex tenu. La présentation suivante se bloque.

Correctif (LKD, `src/lkd/src/mm/watch.c`) :
- `mm_unwatch_for_write` distingue « page trouvée surveillée ou protection changée » (2) de « déjà inscriptible, rien fait » (1).
- Seul le cas 1 compte vers la borne, maintenant 1000.
- Une écriture Linux dans une VMA inscriptible ne doit jamais échouer à cause de la surveillance du code.

Témoins (`witnesses/glibc-x86_64/`) :
- w23_codedatapage reproduit le défaut (SIGSEGV, rc 139 avant ; PASS après) ;
- w22_mutexsig (mutex sous signaux) passe ;
- w24 (`cond_timedwait`) passe ;
- non-régression : w06, w10, w15, w17, w18, w19 et w21 passent.

Vérification (MESURE, `evidence/eac-runs/20261003T023459/`) :
- EAC initialisé ;
- menu atteint, musique lancée à 77 s ;
- 3 540 images présentées en 150 s, environ 16 ms par image au menu ;
- empreintes du jeu inchangées.

Le vidage de copie de texture mappée (texte Scaleform) est réactivé par défaut (`NPT_FLUSH_MAPPED_COPY=0` le coupe). Il avait été accusé à tort.

À faire (performance) : la page mixte PLT+données provoque une tempête d'invalidations SMC dans toute bibliothèque dont les données partagent une page hôte avec du code. Pistes : surveillance plus fine, ou édition des liens de nos .so à 16 Kio (`-z max-page-size=16384`).

## Texte Scaleform qui disparaît : corrigé (2026-10-03, 02:56)

Cause (MESURE, trace client `NPT_TRACE_STAGING`, essais `20261003T024536` et `025108`) :
- Le cache de glyphes de Rocket League copie son atlas (texture GPU 1024x1024 R8) dans une texture STAGING (accès CPU en écriture seule), puis la mappe en `D3D11_MAP_WRITE`.
- Il ajoute ensuite les nouveaux glyphes et envoie toute l'image mappée à l'atlas par `UpdateSubresource`.
- Le renderer Neptune (`src/graphics-spike/virgl/src/neptune/npt_resource.c`, map synchrone) ne recopiait le contenu dans l'emplacement partagé que pour un accès READ.
- Le jeu voyait donc une image vide hors des glyphes du moment : chaque envoi effaçait les anciens. Mesure : octets non nuls envoyés 17 617, 24 879, 7 313, 946… au lieu d'une croissance.
- Le correctif client de 01:40 (copie depuis une texture mappée) visait une séquence que le jeu n'emploie pas : la trace ne l'a jamais vue.

Correctif :
- Pour tout Map D3D11 qui n'est pas DISCARD (WRITE, READ_WRITE, NO_OVERWRITE), le renderer recopie le contenu courant dans l'emplacement, comme le prévoit la sémantique D3D11.
- Le chemin D3D12 est inchangé.

Vérification (MESURE, essai `20261003T025436`, capture `evidence/wine-linux/play/t2-menu-win1846.png`) :
- l'atlas croît sans jamais redescendre (17 617 → 117 736 octets non nuls) ;
- tous les textes du menu sont affichés : boutons, défis, nom du joueur, rang.

## Session joueur du 2026-10-03 (02:57–03:57) : affichage perdu en entrant en Free Play

MESURE (`evidence/eac-runs/20261003T025713/`) :
- Lancement authentifié, EAC actif, menu avec texte complet. Free Play chargé à 151 s.
- Juste après, le pilote reçoit `WSD_RELATIVE_MOUSE` (souris capturée, dernière ligne de `wsd.log` : « relative mouse on »).
- Puis les deux connexions du jeu à wsd sont perdues (« lost the connection to the window server wsd »). wsd disparaît sans message de sortie, sans rapport de plantage et sans « signal 14 » : c'est la signature d'un arrêt forcé ou d'un Quitter via le Dock.
- Le jeu continue sans affichage jusqu'à la limite d'une heure (journal jusqu'à 3 542 s, rc 142). Empreintes inchangées.

Cause probable (LU, `src/wsd/wsd.m`) :
- En mode relatif, wsd appelle `CGAssociateMouseAndMouseCursorPosition(false)`, qui agit sur tout le système.
- La perte du focus (Cmd+Tab, notification) ne rendait pas le curseur : il restait figé partout.
- Arrêter wsd était alors la seule issue.
- Reproduction automatique (60 000 mouvements relatifs à 1000 Hz en Free Play, `run/mouse-stress.sh`, essai `20261003T035930`) : wsd tient. Mais l'écran était en veille (fenêtre occultée) et le scénario de focus n'a pas pu être rejoué.

Correctifs (wsd) :
- perte de focus : curseur rendu et mode relatif suspendu ;
- retour du focus : mode relatif repris ;
- un Quitter (Dock ou menu) est maintenant journalisé ;
- `wsdctl move COUNT HZ` (entrée de test) injecte des mouvements relatifs.

Non validé à l'écran : à vérifier avec l'utilisateur.

## Clavier et son (2026-10-03, 10:35)

Clavier :
- Cause : les fenêtres de jeu sont créées sans bordure, car Wine dessine lui-même le cadre. Une `NSWindow` sans bordure refuse le statut de fenêtre clé, donc aucun `keyDown` n'arrivait (`key=0` dans tous les états de fenêtre). Les touches injectées marchaient parce qu'elles étaient envoyées directement à la fenêtre.
- Correctif : sous-classe `WsdNSWindow` (`canBecomeKeyWindow`/`canBecomeMainWindow` → YES) dans `src/wsd/wsd.m`.
- Mesure (`evidence/eac-runs/20261003T103502/wsd.log`) : fenêtre du jeu `key=1 app_active=1`.

Son :
- Cause : Wine construit sans pilote audio Linux, et aucun serveur de son côté hôte (« No driver from pulse,alsa,oss,coreaudio »).
- Correctif : nouveau pilote Wine `wineobeliskaudio.drv`, dérivé de wineoss pour la logique de temps.
  - Les trames vont dans un anneau partagé (memfd, `struct wsd_audio_ring`), passé à wsd par SCM_RIGHTS.
  - wsd les joue par une AudioUnit CoreAudio par flux, dont le rappel lit l'anneau directement : `WSD_AUDIO_OPEN/START/STOP/RESET/CLOSE` dans `wsd_protocol.h`.
  - L'ouverture est synchrone (PING/PONG) : un refus de wsd fait échouer le flux proprement.
  - `obeliskaudio` est en tête de la liste par défaut de mmdevapi. Pas de MIDI ni d'aux. Pas de capture pour l'instant.
- Mesures :
  - client natif : 48 000 trames lues en 1,12 s ;
  - WASAPI partagé par événements (`src/wine-linux/tests/wasapitone.c`) : position 2,000 s après 2,154 s, en float 48 kHz stéréo ;
  - le jeu ouvre son flux (format 4 = float, 48 kHz, 2 canaux).
- Limites : le volume par flux de Wine n'est pas appliqué (seul le silence total l'est, comme wineoss) ; pas de micro.

## Chargements de cartes : de 26–38 s à 2–5 s (2026-10-03, 14:57)

MESURE (profil `sample` de chaque processus invité, déclenché par chaque `LoadMap:` de `Launch.log`, lancements par Obelisk) :
- Avant : Free Play 37,8 s, retour au menu 18,4 s, démarrage jusqu'au menu 80,6 s. Le fil principal du jeu passait 69 % du chargement bloqué dans `read` sur un tube (attentes Wine), 10 % dans `getdents64`, 10 % dans le code JIT. Le serveur de rendu et wineserver étaient presque inactifs.
- `getdents` : `snap_add` comparait chaque nom à tous les précédents. `CookedPCConsole` contient 20 746 fichiers, soit environ 215 millions de `strcmp` et 357 ms par listage. Wine liste le dossier à chaque recherche insensible à la casse qui échoue. Les noms d'un `readdir` hôte sont uniques : seuls les points de montage et les dossiers virtuels gardent la vérification. Listage : 6,4 ms (test `getdents_large_directory`).
- LKD était compilé en Debug (sans optimisation) par défaut, et c'est ce binaire qui partait dans l'archive `lkd`. `lkd/build.sh` compile désormais en Release. Toute la suite passe en Release (le test de borne de `mm/watch.c` attendait encore l'ancienne borne de 4).
- Après les deux correctifs : Free Play 4,6 s, menu 1,6 à 2,3 s, démarrage 28,3 s, arène en ligne 3,6 s. Le match en ligne a tenu jusqu'à la sortie du joueur, sans « Error: 42 ». Pendant le chargement Free Play, aucun processus n'est saturé (wineserver à environ 6 %).

## Pics de lag et image qui revient en arrière (2026-10-03, 15:55)

Symptômes rapportés par le joueur : de gros pics de lag en jeu, le menu qui reste lent après un match, et l'image entière qui revient en arrière le temps d'une image.

MESURE, en trois temps :
- Les profils `sample` de l'après-midi faussaient les sessions. Ils suspendent chaque thread environ mille fois par seconde, et Free Play tombait de 60 à environ 25 i/s tant qu'ils tournaient. Sans profil, Free Play tient 60 i/s. D'où le journal interne `FEXDARWIN_HITCH_LOG` (temps de traduction FEX par thread, fautes SMC et invalidations, fenêtres de 50 ms, pages fautives toutes les 10 s) et le fichier `fexdarwin.env`, qui l'active sans toucher au lanceur d'Obelisk.
- Tempête SMC : environ 19 000 fautes d'écriture par seconde dans le processus du jeu pendant toute la partie, 0,9 s de gestion par tranche de 10 s, presque toutes sur `nptunix.so+0x3000`. La fin de son code et ses données (`.got`, `.data`, `.bss`) partageaient une page hôte de 16 Kio. En reliant les 27 `.so` Unix de voile-wine avec `-z max-page-size=16384` (Voile, `build-linux.sh`), on tombe à 4 fautes par 10 s, et les pics ont disparu du ressenti du joueur.
- Image en arrière : wsd fait copier par son GPU 64 pixels du tampon qu'il dessine. L'image N est parfois dessinée avec les pixels de N−3, qui sont le contenu précédent du même tampon de la chaîne d'échange. Relu par le CPU 2 à 8 ms plus tard, le tampon contient la bonne image : la copie arrive, mais après le signal « GPU terminé ». Correction annexe dans vtest (neptune) : un retrait de fence sur un anneau d'événements Neptune emportait les soumissions précédentes du même anneau. C'était un vrai défaut (signal prématuré, puis accès après libération au retrait suivant), mais ce n'était pas la cause : après correction, il reste environ une image en arrière par minute. Piste suivante : ordonner la présentation sur la fin réelle du tampon de commandes DXMT (requête d'événement D3D11 côté serveur de rendu) plutôt que sur l'événement partagé.


## PID hôte pour Steam : `lkd_host_pid` et `steam-hostpid.so` (2026-10-10, 08:20)

Contexte : chemin ELDEN RING (Steam) sur le moteur Linux. Le `steamclient.so` Linux de Valve, dans l'invité, parle en TCP au moteur Steam arm64 d'Obelisk et joint `getpid()` à chaque message IPC. Le moteur coupe le tuyau (RST) 2 à 3 s après la connexion si ce PID n'est pas un processus macOS vivant. Les PID invités de LKD (2, 3...) n'en sont pas.

- LKD : `/proc/<pid>/lkd_host_pid` (et `task/<tid>/`, `/proc/thread-self/`) donne le PID macOS du processus hôte, lu dans le registre de la famille. Il reste juste après un fork, un exec et la migration d'un enfant de vfork. Il n'apparaît pas dans le listage de `/proc/<pid>`. Détails : `lkd/docs/status/procfs.md`.
- `guest/steam-hostpid.so` (bibliothèque x86-64, zig, glibc 2.36) : `getpid()` rend ce PID hôte seulement quand l'appelant est dans `steamclient.so` (adresse de retour, `_dl_find_object`). Pour tout autre appelant, il rend le PID invité.
  - Pourquoi filtrer par appelant : ntdll envoie `getpid()` à wineserver (`unix_pid` de `init_first_thread`), qui signale les threads par `tgkill(unix_pid, unix_tid)` et lit leur mémoire par `process_vm_readv` (`server/ptrace.c`). Un PID hôte y donnerait ESRCH. Limiter le préchargement à l'arbre du jeu ne suffit pas : le processus du jeu est lui-même un processus Wine.
  - Les 22 appels de `getpid` dans `steamclient.so` (build 1788652215) sont des `call` par la PLT, sans saut terminal : l'adresse de retour est bien dans le `.so`.
  - Le résultat est mis en cache, avec le PID invité comme clé : un fork (PID invité jamais réutilisé) relit le fichier.
- MESURE, témoin `tests/glibc-x86_64/hostpid.sh` (w25) : thread principal, thread, fork, exec, `posix_spawn`, puis la bibliothèque appelée depuis un faux `steamclient.so`, depuis un thread et après un fork. Chaque PID annoncé est un `fexdarwin-run` vivant, avec le bon parent macOS (`ps`). PASS.
- MESURE, sonde Steam en direct (session Obelisk d'ELDEN RING, port `Steam3Master` seul lu) :
  - avec la bibliothèque : session complète (tuyau, utilisateur, licence, ticket chiffré `eResult=1`), 25 s stables, sortie 0 ;
  - sans la bibliothèque (témoin) : RST environ 2 s après la connexion, assertion fatale, sortie 1.
- Pour Voile et Obelisk : livrer `guest/bin/steam-hostpid.so` dans le runtime Linux, et le précharger (`LD_PRELOAD`) dans l'invité du lanceur. Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/05-lkd-pid-hote.md`.

## Orphelins : un parent vivant et lisible (2026-10-10, 10:50)

Contexte : ELDEN RING sur le moteur Linux. Son bootstrapper EAC, lancé par `steam.exe`, finissait toujours en `208, 'Launch Error: 256'` (rapport sailover 07, § 4). EAC lit `getppid()` puis `/proc/<ppid>/{comm,exe,cmdline,status}` (digest EAC, H8). Or Wine crée chaque processus par un double fork (`ntdll/unix/process.c`, `spawn_process`) : le processus intermédiaire sort aussitôt et le processus créé devient orphelin.

Ce que faisait LKD (MESURE, sonde `ppid-probe.sh` : `ping.exe` lancé par `cmd.exe`) :
- sans sous-moissonneur, l'orphelin partait au pid 1. Aucune famille LKD n'a de pid 1 (les pids commencent à 2) : `getppid()` rendait 1, et `/proc/1` n'existait pas ;
- `/proc/<pid>/status` faisait pire. Il montrait le processus intermédiaire mort (`ping.exe` : `PPid 51` ; `winedevice.exe`, `plugplay.exe`, `svchost.exe` : 12, 22, 28 ou 34), parce que procfs rendait la copie locale du ppid, que le processus ne relit que s'il appelle lui-même `getppid` ;
- un parent dont le processus hôte meurt sans passer par sa sortie (SIGKILL hôte) gardait ses enfants : leur ppid restait sur lui.

Correctif dans lkd (`task/wait.c`, `task/family.c`, `task/tasks.c`, `procfs/pid.c`), sur le modèle de `kernel/exit.c` v6.12 :
- l'init de la famille est son premier processus enregistré sans parent dans la famille, celui que le moteur lance (chez Obelisk, le `sh` de `launch.sh`). Le registre garde son pid dans son en-tête (version 2). Comme le `child_reaper` d'un espace de pid Linux, il reçoit les orphelins qui n'ont aucun sous-moissonneur vivant parmi leurs ancêtres ;
- `PR_SET_CHILD_SUBREAPER` et `PR_GET_CHILD_SUBREAPER` existaient déjà. Le sous-moissonneur garde la priorité, et l'indicateur ne passe pas aux enfants ;
- l'orphelin prend SIGCHLD comme signal de sortie (`reparent_leader`). Un zombie reparenté est annoncé au nouveau parent, qui peut le récolter par `wait4` ;
- le même reparentage a lieu quand le processus hôte d'un parent meurt sans sa sortie. Le signal de mort du parent part alors avec SI_USER et le pid du parent mort, comme `SEND_SIG_NOINFO` ;
- `/proc/<pid>/status` et `stat` relisent ppid, pgid, sid et le signal de sortie dans le registre avant le rendu.

Le premier processus ne devient pas le pid 1. Raisons :
- EAC exige un parent vivant et lisible, pas le pid 1. Sur le PC Bazzite, le jeu a pour parent `pv-adverb`, un sous-moissonneur ; sous HyperLinux, un `dash` de pid 1 passait aussi le contrôle du parent (digest EAC, § 1.3 et H8) ;
- Wine en pid 1 (PPid 0) donne précisément 256 sur un vrai noyau (`pid1-control`). Avec le premier processus numéroté 1, tout lancement où Wine est ce premier processus y serait exposé ;
- un vrai pid 1 a aussi la protection `SIGNAL_UNKILLABLE`, l'exclusion de `kill(-1)`, et sa mort arrête tout l'espace de pid. LKD n'a aucune de ces règles : numéroter le premier processus 1 sans elles inventerait un init à moitié ;
- le bootstrapper de Rocket League a déjà ce `sh` (pid 2) pour parent et passe en 301. Rien ne change pour lui ;
- LKD ne donne pas le pid 1 par choix documenté (`first_pid` vaut 2 par défaut, statut task, point 1). `first_pid = 1` reste possible par configuration, et l'init de la famille est alors le pid 1.

MESURE :
- suite LKD : 42 programmes passent ; `test_sig_futex` dépasse son délai comme sur l'arbre intact, au même cas (`waitv`). `test_task_proc` compte 21 cas (260 assertions) et `test_procfs_other` 6 cas. Les cinq nouveaux cas échouent sans le correctif ;
- témoin invité `w26_reaper` : 8 échecs avec le `fexdarwin-run` du runtime -13-dev, PASS avec le nouveau ;
- sonde ppid : avant, `ping.exe ppid=51 parent_comm=UNREADABLE` ; après, `ppid=2 parent_comm=sh`, comme tous les processus de Wine ;
- ELDEN RING, bootstrapper derrière `steam.exe` (`er-direct.sh` en mode B, runtime et préfixe copiés, `/opt/game` en lecture seule, binds Steam d'Obelisk) : avant, `208, 'Launch Error: 256'` 0,9 s après le début du mapping ; après, le mapping dure 89 s et finit en `210, 'Unexpected error. (#1)'`. C'est le verdict du lancement direct (mode A) du rapport 07, un autre chantier ;
- Rocket League (`rl-direct.sh`) : `301, 'Easy Anti-Cheat successfully loaded in-game'` 0,39 s après le début du mapping.

Reste ouvert : l'init de la famille n'a pas les règles propres au pid 1 (voir plus haut), et le premier processus garde `ppid` 1 sans `/proc/1`. Linux donnerait 0 pour un parent hors de l'espace de pid. Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/08-lkd-orphelins.md`.
