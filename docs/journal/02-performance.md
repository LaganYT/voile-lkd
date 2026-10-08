# Performances du moteur Linux et couches à simplifier, 3 octobre 2026

Ce document mesure où passe le temps dans une image, dans la latence d'entrée et au démarrage de Rocket League sur le
moteur Linux. Il chiffre ensuite chaque couche de traduction qu'on pourrait retirer ou simplifier. C'est une analyse :
rien n'est décidé, et les correctifs proposés en fin de document ne sont pas écrits.

Machine : Apple M5 Max, macOS 27.0.1, écran jusqu'à 240 Hz. Runtime Voile `v26.3.0-8` (Obelisk `a853ad2`), avec
`fexdarwin-run` et wsd remplacés par les versions instrumentées décrites plus bas. Le jeu est limité à 60 i/s.

## Résumé

- **À 60 i/s, aucune pièce n'est saturée.** En Free Play, le fil qui présente les images attend 15,5 ms sur 16,7 ms.
  Le GPU est occupé à 27 %. Le serveur de rendu utilise 39 % d'un cœur. Le jeu lui-même en utilise 88 %, dont une
  bonne part en attente active.
- **Le jeu fait 660 000 appels système par seconde en Free Play** (480 000 au menu). Plus de la moitié viennent
  d'une attente active (`SwitchToThread`), que Wine traduit en `getrusage` + `sched_yield` + `getrusage`. Viennent
  ensuite environ 26 000 requêtes par seconde à wineserver, à environ 11 µs l'aller-retour.
- **Un appel à wineserver coûte 150 à 280 fois plus cher qu'avec le Wine macOS de Voile.** Ce n'est pas FEX : sans
  ntsync, chaque attente et chaque `SetEvent` font un aller-retour avec wineserver. Pour le calcul pur, memcpy et la
  lecture de fichiers, le moteur va aussi vite que Rosetta.
- **Le démarrage (31,6 s mesurés, 27,7 s sans profil) ne dépend pas du JIT.** La compilation FEX prend moins de
  0,1 s sur tout le lancement : un cache de code sur disque ne gagnerait presque rien. `InitOnlineSubsystem`
  (14,8 s) sature le fil principal avec une avalanche d'appels sur les fichiers. Wine relit tout un dossier à chaque
  fichier introuvable, parce que LKD ne dit pas que le volume est insensible à la casse. Un fichier introuvable coûte
  10,5 ms dans un dossier de 20 000 fichiers, contre 17 µs sous le Wine macOS.
- **Latence d'entrée.** Au menu, 58 ms jusqu'à l'envoi de l'image qui réagit (médiane). En Free Play, 33 ms. Il faut
  ajouter 15,6 ms jusqu'à l'affichage, alors que l'écran se rafraîchit toutes les 4,2 à 8,3 ms. Le pilote, lui,
  transmet une touche en 0,05 ms.
- **Ordre proposé** (détails à la fin) :
  1. dire à Wine que les dossiers sont insensibles à la casse (gros gain au démarrage, petit chantier) ;
  2. vérifier l'anomalie d'epoll de wineserver (jusqu'à un cœur de temps système) ;
  3. alléger `NtYieldExecution` ;
  4. compter les requêtes de wineserver par type, puis décider de ntsync dans LKD ;
  5. raccourcir le chemin de présentation de wsd.

  D3D11 dans le même processus, un wineserver natif, les parties Unix natives et le cache de code FEX ne se justifient
  pas d'après ces mesures.

## Outils de mesure (ajoutés pour ce rapport)

- **`FEXDARWIN_PROFILE=1`** (fexdarwin, `HitchLog.cpp`). Toutes les 10 s, ainsi qu'à la fin d'un processus et à un
  `exec`, chaque processus invité écrit :
  - son CPU utilisateur et système ;
  - ses appels système : nombre, et temps passé dans LKD (ce temps inclut les attentes bloquantes) ;
  - le temps de JIT de FEX, les fautes SMC et les invalidations ;
  - pour chaque thread, le CPU, nommé d'après le `comm` invité (les noms Windows des threads, par exemple
    `RenderingThread`) ;
  - les 12 appels système qui ont pris le plus de temps.

  Coût : deux lectures d'horloge par appel système. Avec le profil, le démarrage est passé de 27,7 à 31,6 s.
- **`tools/dev/engine-top.c`**. Il donne le CPU de chaque thread de wsd, de `virgl_render_server` et des
  `fexdarwin-run`, ainsi que l'occupation du GPU (`Device Utilization %` de l'IOAccelerator). Il lit ces chiffres par
  `proc_pidinfo` et IOKit, sans suspendre de thread, à la différence de `sample`.
- **Sonde de latence de wsd.** La première entrée de test (`wsdctl tap`) l'active maintenant, alors qu'il fallait
  avant `WSD_LATENCY_PROBE` dans l'environnement de wsd, que le lanceur d'Obelisk ne transmet pas.
- **`tests/bench/winbench.c`**. Coût par appel des traversées d'un jeu Windows : horloge, `SwitchToThread`, attente et
  signal d'événement, création d'objet, section critique, lecture de fichier, aller-retour entre deux threads, calcul,
  memcpy. `winbench N TEST` lance un seul test, pour que le profil ne voie que ses appels.
- **`tests/bench/missbench.c`**. Coût d'un `GetFileAttributesW` sur un fichier absent, et sur un fichier présent mais
  écrit avec une autre casse.

Session mesurée : lancement depuis Obelisk, puis menu (20 flèches haut/bas envoyées par `wsdctl`), puis Free Play
(environ 60 s de jeu, puis 8 bascules de caméra balle par la touche Espace), retour au menu et sortie par le menu.
Aucune partie en ligne. Journaux : dossier `session1` de l'espace de travail de la session (non versionné).

## Une image (60 i/s, budget de 16,7 ms)

| Pièce | Free Play | Menu | Remarque |
|---|---|---|---|
| Jeu, `RenderingThread` | 45 % d'un cœur (7,5 ms par image) | 21 % | 314 000 appels système/s en Free Play |
| Jeu, fil principal | 30 % (5,1 ms) | 21 % | 318 000 appels système/s |
| Jeu, autres fils | 13 % (2,2 ms) | 8 % | |
| wineserver | 12 % (1,9 ms) | 28 % (anomalie, voir plus bas) | 94 000 appels système/s en Free Play |
| Serveur de rendu, décodeur Neptune (`npt-ring-1`) | 25 % (4,2 ms) | 13 % | appels DXMT inclus |
| Serveur de rendu, `dxmt-encode-thread` | 6 % (1,0 ms) | 4 % | |
| Serveur de rendu, autres fils | 8 % (1,3 ms) | 5 % | |
| wsd | environ 3 % | 3 % | |
| GPU | 27 % | 26 % | occupation moyenne |
| Attente de la fence avant présentation | 0,5 ms (médiane) | | `fence wait` de nptunix |
| Attente de cadence (`NPT_WSD_PACING=after`) | 13 à 16,5 ms | 15 à 16 ms | marge inutilisée |

Le temps des fils du jeu comprend l'attente active. Le jeu fait environ 118 000 `SwitchToThread` par seconde en
Free Play, soit environ 2 000 par image. Chacun coûte trois appels système, environ 1,6 µs au total : l'attente active
prend donc environ 0,2 cœur, soit 3 ms de CPU par image sur les deux fils principaux.

Les appels système du jeu en Free Play, par seconde :

| Appel | Nombre/s | Origine |
|---|---|---|
| `getrusage` | 237 000 | `NtYieldExecution` de Wine (deux par `SwitchToThread`) |
| `clock_gettime` | 142 000 | `QueryPerformanceCounter` (150 ns l'appel en tout) |
| `sched_yield` | 118 000 | `NtYieldExecution` |
| `rt_sigprocmask` | 77 000 | deux par requête à wineserver |
| `read` | 42 000 | réponses de wineserver, socket wsd, etc. |
| `write` + `writev` | 39 000 | requêtes à wineserver, présentations, son |
| `futex` | 1 500 | |

wineserver lit environ 26 000 requêtes par seconde en Free Play (7 700 au menu), dont presque toutes viennent du jeu.
Avec environ 11 µs de latence par appel, cela fait jusqu'à 0,3 s de fil bloqué par seconde, réparties entre les fils
du jeu. Le type de ces requêtes n'est pas encore mesuré (voir « Mesures suivantes »).

La cadence tient : sur 14 820 présentations, l'intervalle est de 16,6 à 16,7 ms. Il y a eu 23 images tenues plus de
50 ms (surtout aux chargements), et 17 « frame back » en environ 4 minutes.

## Latence d'entrée

Méthode. `wsdctl tap` envoie une touche à wsd, qui la date. La sonde compare ensuite chaque image reçue à celle
d'avant l'entrée, et date la première qui change nettement. Le pilote `wineobelisk.drv` date de son côté la lecture
de l'évènement et son envoi à Wine (`WINE_OBELISK_LATENCY=1`).

| Étape | Menu | Free Play |
|---|---|---|
| wsd → lecture par le pilote → envoi à Wine | 0,05 ms | 0,05 ms (90e centile 3,5 ms) |
| Entrée → soumission de l'image qui réagit (jeu, rendu, GPU, fence) | 58 ms en médiane (de 21 à 101 ms, 20 essais) | 33 ms en médiane (de 27 à 48 ms, 8 essais) |
| Soumission → affichage (`presentedTime`) | 15,6 ms (médiane) | 15,6 ms |
| **Total** | **environ 73 ms** | **environ 49 ms** |

Les chiffres du menu sont moins sûrs : le surlignage du menu s'anime en fondu, et la sonde le détecte un peu tard. En
Free Play, la bascule de caméra change toute l'image, ce qui donne une mesure nette.

Pour les vraies touches du joueur, 3,1 ms en médiane séparent l'heure de l'évènement macOS (le matériel) de sa
lecture par le pilote. C'est le trajet de macOS jusqu'à l'application, avant wsd.

De l'entrée à la soumission, il faut environ deux images en Free Play : le fil de jeu lit l'entrée, puis le fil de
rendu dessine une image plus tard. Ce délai est celui du moteur du jeu, pas celui de notre chaîne.

La soumission → affichage, en revanche, est à nous. Une image attend :
- le prochain tick du display link de wsd ;
- le dessin dans le drawable ;
- puis le compositeur de macOS, puisque la fenêtre est en mode fenêtré.

Au total, cela fait 15,6 ms, alors que les intervalles affichés tombent sur des multiples de 4,17 ms. On peut sans
doute gagner 4 à 10 ms ici, à mesurer plus finement (voir la couche « Présentation »).

## Démarrage

| Étape (`Launch.log`) | Sans profil (17:22) | Avec profil (20:49) | Ce qui se passe (profil) |
|---|---|---|---|
| `PreInit` | 2,3 s | 2,6 s | |
| `LoadScriptPackages` | 9,9 s | 10,5 s | le fil principal tourne en attente active ; deux autres fils chargent, sans saturer |
| `InitOnlineSubsystem` | 12,4 s | 14,8 s | le fil principal sature un cœur sur les appels de fichiers |
| Autres étapes | 3,1 s | 3,7 s | |
| **Total** | **27,7 s** | **31,6 s** | |

Détail du profil, par tranche de 10 s, pour le processus du jeu :
- **De 0 à 10 s.** Le fil principal prend 8,7 s de CPU pour 6,5 millions d'appels système : 4,2 millions de
  `getrusage` et 2,1 millions de `sched_yield`. C'est l'attente active d'un chargement qui tourne sur d'autres fils :
  le fil 117 prend 3,6 s de CPU, et `AsyncIOSystem` 1,7 s avec 957 000 appels système. Ce qui borne cette phase n'est
  pas établi, puisqu'aucun fil n'y est saturé (voir « Mesures suivantes »).
- **De 10 à 30 s.** Le fil principal prend 8,3 puis 10,2 s de CPU, dont plus de la moitié en temps système. Sur les
  20 s, il fait :
  - 592 000 `newfstatat` ;
  - 304 000 `openat` ;
  - 304 000 `getdents64` ;
  - 255 000 `getxattr` ;
  - 240 000 `close` ;
  - 120 000 `fstat` ;
  - 75 000 `fstatfs`.

  C'est la recherche insensible à la casse de Wine sous Linux (`lookup_unix_name`, `dlls/ntdll/unix/file.c`). Quand
  un nom n'existe pas tel quel, Wine appelle `get_dir_case_sensitivity` : il ouvre le dossier et demande
  `EXT2_IOC_GETFLAGS`. Si le drapeau `EXT4_CASEFOLD_FL` n'est pas là, il relit tout le dossier. LKD ne connaît pas cet
  ioctl, alors que le volume APFS de l'hôte est réellement insensible à la casse.
- **Le JIT de FEX** a pris 27, 14 puis 5 ms sur ces trois tranches, soit moins de 0,1 s sur tout le démarrage. Un
  échantillonnage de wineboot (hors jeu) confirme : les fonctions de compilation de FEX y sont presque absentes, et les
  fils attendent dans le noyau ou dans des appels de fichiers.

Coût d'une recherche de fichier (`missbench`, 200 à 2 000 appels de `GetFileAttributesW`) :

| Cas | Moteur Linux | Wine macOS de Voile |
|---|---|---|
| Fichier absent, dossier de 20 fichiers | 139 µs | 17 µs |
| Fichier absent, dossier de 20 000 fichiers | 10 518 µs | 17 µs |
| Fichier présent, autre casse | 50 µs | 9 µs |

## Coûts unitaires (`winbench`)

Même programme sur le moteur Linux et sous le Wine macOS de Voile (x86-64 sous Rosetta), dans un préfixe de test.

| Appel | Moteur Linux | Wine macOS, msync (réglage d'Obelisk) | Wine macOS sans msync |
|---|---|---|---|
| `QueryPerformanceCounter` | 150 ns | 48 ns | 48 ns |
| `SwitchToThread` | 1 256 ns | 226 ns | 230 ns |
| `WaitForSingleObject(ev, 0)` | 11 839 ns | 66 ns | 8 423 ns |
| `SetEvent` | 10 681 ns | 39 ns | 7 928 ns |
| `CreateEvent` + `CloseHandle` | 20 946 ns | 18 862 ns | 17 156 ns |
| Section critique, sans contention | 3,7 ns | 3,4 ns | 3,3 ns |
| `SetFilePointer` + `ReadFile` de 4 Kio | 957 ns | 1 027 ns | 1 013 ns |
| Aller-retour d'évènement entre deux fils | 34 715 ns | 5 851 ns | 19 650 ns |
| Boucle de calcul (par itération) | 1,2 ns | 1,2 ns | 1,2 ns |
| memcpy de 1 Mio | 16,3 µs | 18,5 µs | 18,5 µs |

Une requête à wineserver se décompose ainsi d'après le profil :
- côté client, quatre appels système : `rt_sigprocmask` deux fois, `write` de la requête, `read` de la réponse ;
- côté serveur, environ cinq : `epoll_pwait2`, `read`, `write`, `gettimeofday`, `clock_gettime` ;
- au total, environ 16 µs de CPU répartis entre les deux processus.

Un appel système LKD de base coûte environ 0,15 µs (`clock_gettime`). Les appels qui touchent le noyau Darwin coûtent
1 à 2 µs (`write` sur un tube, `sched_yield`).

## Les couches, une par une

| Couche | Ce qu'elle coûte (mesuré) | Pour la retirer ou la simplifier | Risque | Gain attendu |
|---|---|---|---|---|
| **Recherche insensible à la casse (Wine sous Linux sur APFS)** | Fil principal saturé pendant `InitOnlineSubsystem` (environ 1,9 million d'appels de fichiers en 20 s). 10,5 ms par fichier absent dans un grand dossier. | LKD répond à `FS_IOC_GETFLAGS` (`EXT2_IOC_GETFLAGS`) sur un dossier avec `EXT4_CASEFOLD_FL` quand le volume hôte est insensible à la casse (`pathconf(_PC_CASE_SENSITIVE)`). Pas de changement de Wine. | Faible : c'est une description exacte du volume. Un volume APFS sensible à la casse continue de répondre « sensible ». | Estimé de −8 à −12 s ; mesuré **−4,2 s** (27,7 → 23,5 s, voir « Après le correctif de casse »). |
| **Attente active `NtYieldExecution`** | 118 000 yields/s en Free Play, chacun = 3 appels système (environ 1,6 µs), soit environ 0,2 cœur. Au démarrage, le fil principal y passe 8,7 s de CPU sur les 10 premières secondes. `thread_rusage` de LKD renvoie toujours 0 changement de contexte, donc `SwitchToThread` répond toujours FALSE (« pas de yield »). | Sous LKD : un `NtYieldExecution` sans `getrusage` dans voile-wine, ou bien un `getrusage(RUSAGE_THREAD)` qui renvoie un vrai compteur. Rendre `sched_yield` de LKD aussi bon marché que celui de Darwin (1,26 µs contre 0,23 µs). | Faible, mais la valeur de retour de `SwitchToThread` change le comportement des boucles d'attente du jeu. À vérifier. | De −2 à −3 appels système par yield. Environ 0,1 à 0,2 cœur en jeu ; moins de chauffe sur un portable. Peu d'effet sur la cadence, puisque ce temps est de l'attente. |
| **wineserver : temps système dans `epoll_pwait2`** | Par tranches de 10 s au menu, wineserver prend jusqu'à 10,5 s de CPU (8 s en système), pour le même nombre d'appels que dans les tranches calmes (0,9 s). Ce temps est caché dans `epoll_pwait2`. | Compter les tours de la boucle `do_wait` de `lkd/src/net/epoll.c` (kqueue lisible alors que `ep_collect` ne rend rien). Corriger si c'est bien une boucle de réveil. | Faible | Jusqu'à un cœur au menu. Peut-être aussi de la latence de wineserver. |
| **ntsync dans LKD (`/dev/ntsync`)** | Attente ou signal = environ 11 µs et un aller-retour avec wineserver. Aller-retour entre deux fils : 35 µs (contre 5,9 µs avec msync sous macOS). Environ 26 000 requêtes/s en Free Play, de types inconnus. | Un périphérique `/dev/ntsync` dans LKD. Wine et wineserver sont déjà compilés avec ntsync (`HAVE_LINUX_NTSYNC_H`). Les objets doivent être partagés entre processus hôtes : état en mémoire partagée, fds passés par SCM_RIGHTS, WAIT_ALL atomique par instance du périphérique, attentes interruptibles par les signaux LKD. | Moyen. Le point délicat est le multiprocessus, et il ne doit rien changer à ce que voit EAC. | Si la plupart des requêtes sont des waits, des sets et des resets : environ 10 µs par appel. Jusqu'à 0,2 à 0,3 s de fil bloqué par seconde en moins, et une partie des 12 à 28 % de CPU de wineserver. À décider après le comptage des requêtes. |
| **Présentation (wsd)** | 15,6 ms entre soumission et affichage, sur un écran de 120/240 Hz. | D'abord dater chaque étape dans wsd : tick du display link, encodage, `presentedTime`. Pistes : présenter dès la fin de la fence sans attendre le tick, n'utiliser que 2 drawables, et vérifier si le plein écran natif (sans compositeur) est possible pour une fenêtre plein écran. | Faible à moyen (cadence, image en arrière) | Estimé de 4 à 10 ms de latence. |
| **D3D11 dans le même processus (thunk FEX vers DXMT)** | Le serveur de rendu prend 6,5 ms de CPU par image (décodage et appels DXMT). Ce coût serait déplacé, pas supprimé. Le passage entre processus coûte surtout l'attente de fence (0,5 ms en médiane) et les réveils de l'anneau. | `SetThunkHandler` et des stubs `0F 3F` dans fexdarwin (aucun gestionnaire n'est installé aujourd'hui : un `0F 3F` planterait le JIT). Il faudrait aussi charger le décodeur Neptune, DXMT et Metal dans chaque `fexdarwin-run`, et les rendre sûrs vis-à-vis des forks. La version la moins chère garde l'anneau Neptune et remplace seulement le transport vtest. | Élevé : Metal dans un processus qui fait des forks hôtes, plantage commun avec le jeu, et 813 commandes générées dont le générateur n'est pas versionné. | Faible à 60 i/s : moins d'une milliseconde de latence par image. Utile seulement si un jour le serveur de rendu sature (cadence au-delà de 60 i/s). |
| **Parties Unix natives (`nptunix`, `wineobelisk`) par thunk** | Non isolé dans le profil, mais petit. Le pilote transmet une touche en 0,05 ms. Les tempêtes SMC de `nptunix` sont réglées depuis le relinkage en pages de 16 Kio. | Les mêmes thunks que ci-dessus. | Moyen | Faible. |
| **wineserver natif arm64** | 12 % d'un cœur en Free Play, 28 % au menu (anomalie comprise), soit environ 5 µs de serveur par requête. | Porter wineserver sur Darwin en gardant la sémantique des fds et de la mémoire partagée de LKD vue par les invités. Gros chantier. | Élevé | Faible une fois ntsync en place, qui supprime la plupart des requêtes. Ne se justifie pas. |
| **Cache de code FEX sur disque** | JIT de moins de 0,1 s sur tout un démarrage, et environ 0,5 ms/s en jeu. | Le `DiskCache` de FEX existe, désactivé par défaut. Pour Darwin, il lit la mémoire invitée sans `G2H` (`DiskCache.cpp:566-578`, `Core.cpp:1029`). | Moyen | Presque nul. À abandonner. |
| **Assertions de FEXCore** (`-DENABLE_ASSERTIONS=ON` dans `fexdarwin/scripts/build.sh`) | Le passage `IRValidation` tourne à chaque compilation. Le JIT est négligeable, donc le coût aussi. | `OFF` en Release. | Faible, mais on perd des contrôles. | Négligeable. |
| **D3DMetal à la place de DXMT** | Déjà écarté : x86-64 seulement, code fermé, pont depuis l'invité nécessaire (`graphics-display-audio.md` §1.1, §1.4). | | | |

## Ordre proposé

1. **Indiquer la casse à Wine** (LKD, `FS_IOC_GETFLAGS`). Petit, sans risque, et c'est le seul gain chiffré en
   secondes. On mesure avant et après avec `Launch.log` (`Startup_*`) et `missbench`.
2. **Vérifier l'anomalie d'epoll de wineserver** : un compteur dans `do_wait`, puis un correctif s'il s'agit d'une
   boucle de réveil.
3. **Alléger `NtYieldExecution` sous LKD**, en vérifiant ce que change la valeur de retour de `SwitchToThread`.
4. **Compter les requêtes de wineserver par type** (compteur par code de requête dans le wineserver de voile-wine,
   affiché toutes les 10 s), puis décider de ntsync dans LKD.
5. **Dater le chemin de présentation de wsd**, puis le raccourcir.

Les thunks, le wineserver natif et le cache de code ne se justifient pas avec ces mesures. On les réexaminera si le
jeu dépasse 60 i/s et que le serveur de rendu devient la limite.

## Après le correctif de casse (21:19)

Correctifs : LKD `ad12111` (`FS_IOC_GETFLAGS` répond casefold pour un dossier d'un volume insensible à la casse) et
voile-wine `87a1c39b` (sur un tel dossier, un nom 8.3 absent ne fait plus relire le dossier). Le lancement se fait
depuis Obelisk, sans profil, comme la mesure de référence de 17:22.

| Étape | Avant (17:22) | Après (21:19) |
|---|---|---|
| `LoadScriptPackages` | 9,9 s | 9,5 s |
| `InitOnlineSubsystem` | 12,4 s | 8,7 s |
| **Total jusqu'au menu** | **27,7 s** | **23,5 s** |

`missbench` (fichier absent, dossier de 20 000 fichiers) : nom long 10,5 ms → 86 µs, nom 8.3 11,3 ms → 77 µs. Le
coût ne dépend plus de la taille du dossier.

Le gain est de 4,2 s, deux fois moins que l'estimation (8 à 12 s). Les relectures de dossiers n'expliquaient donc pas
tout le temps passé dans `InitOnlineSubsystem`.

Le coût d'une recherche reste de 50 à 90 µs, contre 9 à 17 µs sous le Wine macOS. Ce reste vient des appels de chemin
de LKD :
- `newfstatat` : 8 µs ;
- `openat` : 15 µs ;
- `getxattr` : 17 µs.

En natif, chacun coûte 1 à 3 µs, et une recherche Wine en fait 5 à 7. C'est une piste à profiler, avec le reste
d'`InitOnlineSubsystem` (8,7 s).

## `LoadScriptPackages` et `InitOnlineSubsystem` en détail (21:30)

Profil toutes les 2 s (`FEXDARWIN_PROFILE_MS=2000`), avec, pour chaque thread, les appels système qui lui ont pris le
plus de temps. Ce lancement a pris 23,7 s, avec le correctif de casse : la mesure perturbe peu. `LoadScriptPackages`
couvre de 21:30:30,4 à 21:30:40,2, et `InitOnlineSubsystem` de 21:30:41,8 à 21:30:50,6.

| Tranche | Durée | Ce qui la limite |
|---|---|---|
| 30,4–31,6 | 1,2 s | fil principal : appels de fichiers (24 000 `getxattr`, 15 000 `newfstatat` en 2 s) |
| 31,6–35,7 | 4,1 s | chargement asynchrone. `AsyncIOSystem` passe 95 % de son temps bloqué dans `read`, en attente des réponses de wineserver (environ 45 000 `read` et autant d'écritures par seconde, pour 4 500 vraies lectures de fichier, `pread64`). Le fil 117 fait 4 500 `mmap` et 4 500 `mprotect` par seconde (30 µs le `mmap`). wineserver traite environ 50 000 requêtes/s, à 37 % d'un cœur. Le fil principal attend en tournant (900 000 `sched_yield` en 2 s). |
| 35,7–37,9 | 2,2 s | fil principal : calcul pur (100 % d'un cœur, presque sans appels système) |
| 37,9–40,2 | 2,3 s | fil principal : appels de fichiers (75 000 `getxattr`, 67 000 `newfstatat`, 9 600 `openat` en 2 s) |
| `InitOnlineSubsystem`, 41,8–50,6 | 8,8 s | fil principal à 100 %, environ 20 000 fichiers absents par seconde. Chaque seconde : 68 000 `newfstatat` (9 µs), 20 000 `openat` (18 µs), 20 000 `ioctl` de casse, 20 000 `close`. |

Deux causes se partagent donc environ 15 s des 23,7 s :

- **Le coût des appels de chemin dans LKD (environ 10 s).** `fs_resolve` (`lkd/src/fs/vfs.c`) parcourt le chemin
  composant par composant, et `lookup` fait un `lstat` hôte sur le chemin complet de chaque préfixe. Un chemin de
  12 composants coûte donc 12 `lstat`, chacun sur un chemin long.

  Un échantillonnage de `missbench` (pas du jeu) le confirme : `lstat` fait 60 % des échantillons actifs,
  `open` 17 %, `readlink` 7 %.

  Piste : tenter d'abord le reste du chemin en un seul `fstatat(..., AT_SYMLINK_NOFOLLOW_ANY)`. macOS fait alors
  échouer l'appel dès qu'un composant est un lien symbolique, et on ne revient à la marche composant par composant
  que dans ce cas, par exemple pour le lien `dosdevices/c:` de Wine. Pour un nom absent, on vérifie le parent de la
  même façon. Il n'y a ni cache ni invalidation, donc la sémantique ne change pas.

  Estimation : `newfstatat` de 9 à 3 µs environ, `openat` de 18 à 6 µs. Cela ferait environ −4 s sur
  `InitOnlineSubsystem` et −1,5 s sur `LoadScriptPackages`, à vérifier avec `missbench`.
- **Les allers-retours avec wineserver pendant le chargement asynchrone (environ 4 s).** Il faut savoir quelles
  requêtes sont en cause (compteur par type de requête) avant de choisir entre ntsync dans LKD et un chemin d'E/S
  asynchrones plus direct.

Le reste est du calcul (2,2 s, au niveau de Rosetta d'après `winbench`), plus `PreInit` (2,3 s) et les autres
étapes.

## Après les correctifs de fichiers (21:56)

Correctifs : LKD `daa6775` (dossiers ouverts en `O_SEARCH`, 1,6 µs au lieu de 7,3 µs sur macOS), LKD `fcf56ae`
(`getxattr` consulte d'abord `listxattr`, 1 µs au lieu de 5,6 µs pour un attribut absent), voile-wine `c803a925`
(une seule vérification de casse par recherche, au lieu de deux avec la recherche `nom?` des points d'analyse).

| Mesure | 17:22 | 21:19 (casse) | 21:56 |
|---|---|---|---|
| `PreInit` | 2,3 s | 2,6 s | 1,6 s |
| `LoadScriptPackages` | 9,9 s | 9,5 s | 8,7 s |
| `InitOnlineSubsystem` | 12,4 s | 8,7 s | 6,3 s |
| **Total jusqu'au menu (`Launch.log`)** | **27,7 s** | **23,5 s** | **19,2 s** |
| Icône du Dock → fenêtre du jeu | | environ 17,8 s | environ 14,9 s |
| Icône du Dock → menu | | environ 27,5 s | environ 22,2 s |

Bancs d'essai (`missbench`, `enumbench`) :
- fichier absent : 87–101 µs → 57–64 µs ;
- entrée de dossier listée : 24,2 µs → 7,4 µs (Wine macOS : 4,4 µs).

Ce que le jeu cherche en vain (échantillon d'un chemin sur 200, `FEXDARWIN_PROFILE_MISSES`) : environ 240 000 fois
les mêmes fichiers de localisation (`TAGame.INT`, `ProductSlots.INT`…), dans des dossiers de surcharge qui
n'existent pas :
- `Documents/My Games/Rocket League/Engine/Localization/INT` ;
- `…/ProjectX/Localization/INT` ;
- `Engine/Localization` dans le dossier du jeu.

Un essai écarté : résoudre en un seul `fstatat(AT_SYMLINK_NOFOLLOW_ANY)` les dossiers d'un chemin sans lien
symbolique. Le gain n'était que de 0 à 9 %, parce que les chemins de Wine passent par `dosdevices/c:` ou
`dosdevices/z:` puis par un changement de montage. Le test `deep_paths` est resté.

Ce qui reste avant la fenêtre :
- le chargement asynchrone, limité par les allers-retours avec wineserver (environ 4 s, à compter par type de
  requête) ;
- le calcul pur du jeu (environ 2,2 s) ;
- le démarrage de Wine et d'EAC (environ 3 s).

## Requêtes à wineserver par type, et Free Play sans limite (22:03 à 22:18)

Compteur de requêtes de wineserver (voile-wine, branche `wineserver-request-stats`, `WINESERVER_REQUEST_STATS`)
et cadence débridée côté moteur (`NPT_WSD_PACING=off`, `NPT_PRESENT_QUEUE=0`), puis limite d'i/s du jeu au maximum.

| Phase | Requêtes par seconde | Remarque |
|---|---|---|
| Chargement asynchrone (avant la fenêtre) | environ 45 000 : `event_op` 17 000, `close_handle` et `create_event` 8 000 chacune, `select` 6 000, `add_fd_completion` 5 000 | environ 6 allers-retours par lecture de fichier, tous sur le fil `AsyncIOSystem` |
| Fermeture du jeu (15,7 s entre « AK Audio shut down » et « Object subsystem closed ») | environ 55 000 : `select` et `event_op` à parts égales | le fil principal réveille le fil de rendu environ 26 000 fois par seconde |
| Free Play limité à 60 i/s | `event_op` environ 19 000, `select` environ 16 000 | |
| Free Play débridé (environ 130 i/s) | `event_op` environ 41 000, `select` environ 8 000, `create_event` et `close_handle` environ 2 500 chacune | |

Free Play débridé : environ 130 i/s (6 à 9 ms par image).
- Fil de rendu : 82 % d'un cœur, dont environ 20 % en attente active.
- Fil principal : 56 % de son temps bloqué dans des allers-retours avec wineserver (environ 33 000 par seconde,
  environ 250 par image).
- Serveur de rendu : décodeur à 52 %. GPU à 43 %.

ntsync dans LKD rendrait les `event_op` et les `select` locaux. C'est la piste qui sert à la fois au chargement, à la
fermeture et à la cadence au-delà de 60 i/s.

**Incident EAC.** Avec ce wineserver instrumenté et son compteur actif, le montage du module EAC dans le jeu (« Wine
module mapping ») a pris 1,4 s puis 2,4 s, au lieu de 0,5 s sur tous les autres lancements de la journée. Au second
lancement, le jeu a démarré avant la fin du montage et s'est lancé sans EAC (`bAntiCheatEnabled=(False)`). Avec le
wineserver d'origine, tout est revenu : 0,5 s et EAC actif.

La cause n'est pas comprise. Ce compteur reste donc hors de `main`, et ne doit plus servir pendant un lancement
protégé avant d'avoir été compris.

Cet incident montre aussi une course : le jeu peut démarrer avant la fin du montage d'EAC quand celui-ci est lent.

## ntsync dans LKD (23:00)

`/dev/ntsync` dans LKD (`f90924f`, `lkd/docs/status/ntsync.md`). Wine 11 l'utilise sans changement : les attentes et
les signaux d'événements, de mutex et de sémaphores restent dans le processus.

| Mesure | Sans ntsync | Avec ntsync |
|---|---|---|
| `WaitForSingleObject(0)` (`winbench`) | 11,8 µs | 0,35 µs |
| `SetEvent` | 10,7 µs | 0,16 µs |
| Aller-retour d'événement entre deux fils | 34,7 µs | 8,7 µs |
| Rocket League, jusqu'au menu (sans profil) | 19,1 à 19,2 s | 18,6 s |
| Rocket League, « Quitter » → fin du processus | 17,6 à 18,1 s | 10,8 à 11,2 s |
| Free Play débridé | environ 130 i/s | environ 150 i/s |
| EAC (montage du module, `bAntiCheatEnabled`) | 0,5 s, True | 0,5 s, True |

En Free Play débridé avec ntsync :
- le fil principal n'attend plus wineserver ; il attend le fil de rendu (attentes ntsync) ;
- le fil de rendu est la limite (88 % d'un cœur, dont environ 20 % dans l'attente active de `NtYieldExecution`).

Frame back en Free Play à 60 i/s : 4,5/min (20:52, sans ntsync, profil actif), 23/min (23:05, avec ntsync),
39/min (23:11, sans ntsync, `LKD_NTSYNC=0`, même binaire). Ils varient beaucoup d'une session à l'autre, et ntsync
n'en est pas la cause. Leur cause connue reste à corriger : le signal « GPU terminé » de la copie de présentation
arrive avant que le contenu soit en place.

## Frame back : cause et correctif (4 octobre, 00:05)

À chaque `Present`, le pilote invité Neptune arme un événement à usage unique. Cet événement est lié à la fence
signalée après la copie dans l'image du swapchain. Son enregistrement (REGISTER) passait par le flux de commandes du
renderer, alors que l'armement (ARM) et le `SetEventOnCompletion` passent par le ring de méthodes. Quand l'hôte
décodait le REGISTER en dernier :
- le proxy créé par l'ARM n'avait que la référence de l'armement ;
- la fence de présentation relâchait cette référence en s'appariant ;
- l'événement DXMT se fermait avant que le GPU le signale ;
- la fermeture de son pipe (`POLLHUP` sans jeton) retirait la fence avant la copie.

wsd montrait alors l'image avec son contenu de 2 ou 3 présentations plus tôt.

Ce qui l'a montré :
- **La sonde de wsd** : au moment de l'encodage comme après le GPU, la mémoire contient encore les anciens pixels.
  La copie n'avait donc pas eu lieu ; ce n'était pas un cache GPU.
- **Le serveur de rendu instrumenté** : 189 fences retirées sur une fd morte en 71 s, toutes des jetons de swapchain.
  Le proxy enregistré pour le jeton à ce moment n'avait jamais créé de pipe : c'était un second proxy, créé par le
  REGISTER arrivé en retard.

Correctif : Neptune `22d94ed`. Le REGISTER et le RELEASE de ces jetons passent par le ring de méthodes, comme les
événements Win32 de `npt_event_arm`.

| Free Play à 60 i/s | Avant (23:58) | Après (00:04) |
|---|---|---|
| Fences retirées sur une fd morte | 189 en 71 s | 0 en 2 min |
| Frame back vus par wsd | 45 en environ 65 s (39/min) | 2 en 2 min |

Les 2 restants tombent la même seconde, pendant des images tenues 50 à 70 ms au chargement. La sonde de wsd note
maintenant aussi ce que contient la mémoire à l'encodage et après le GPU.

## Présentation de wsd et `NtYieldExecution` : mesurés, laissés tels quels (4 octobre, 00:30)

### Où passent les 15,6 ms entre soumission et affichage

Pour cette mesure, wsd datait chaque image :
- réception ;
- tick du display link, avec son `timestamp` et son `targetTimestamp` ;
- `nextDrawable` et commit ;
- début et fin du GPU ;
- `presentedTime`.

Toutes les 120 images, il écrivait les médianes. Cette datation n'est pas restée dans wsd. Conditions :
- Free Play à 60 i/s, cadence `after` ;
- écran externe à 240 Hz (ASUS XG27AQWMG, un rafraîchissement toutes les 4,17 ms) ;
- les mêmes binaires que dans « Frame back ».

| Segment (médianes par lot de 120 images) | Free Play |
|---|---|
| Envoi nptunix → wsd | 0,02 ms |
| Attente du tick | 3,7 à 4,0 ms |
| Retard du tick sur le vsync | 0,3 à 1,2 ms |
| `nextDrawable` + encodage | 0,1 ms |
| Commit → début du GPU | 0,55 ms |
| GPU (copie dans le drawable) | 0,05 ms |
| Fin du GPU → affichage | 10,8 à 11,5 ms |
| **Soumission → affichage** | **15,6 à 16,0 ms** (11,9 ms quand la phase est favorable) |

- **La composition coûte deux rafraîchissements.** 97 % des images s'affichent 3 rafraîchissements (12,5 ms) après le
  vsync de leur tick, environ 1 % un plus tôt et environ 1 % un plus tard. Le GPU a pourtant fini 1,5 ms après ce
  vsync. Le reste est la composition de macOS : la fenêtre a deux couches, celle de la fenêtre et celle du jeu
  par-dessus.
- **L'image attend presque un rafraîchissement entier avant d'être encodée.** Le jeu se cale sur l'évènement
  « affichée » et renvoie son image juste après un tick. Quand l'image arrive juste avant un tick, l'attente tombe à
  0,3 ou 0,5 ms, et le total à 11,9 ms.
- **Sonde, 8 essais :** de l'entrée à la soumission, 40,5 ms en médiane (de 27,4 à 42,6 ms). Le total est donc
  d'environ 56 ms.
- nptunix attend 15,8 ms par `Present`. Aucun frame back pendant la mesure.

Pistes chiffrées, non faites : l'utilisateur a jugé le gain trop faible.

| Piste | Gain attendu à 240 Hz |
|---|---|
| Présenter dès l'arrivée quand aucune image du plan n'attend l'affichage, au lieu d'attendre le tick | De 0 à 4 ms, selon l'heure limite du compositeur, inconnue |
| Affichage direct sans compositeur (une seule couche plein écran) | Jusqu'à 2 rafraîchissements (8,3 ms) si macOS l'accepte, ce qui n'est pas établi |
| 2 drawables au lieu de 3 | Rien : avec la cadence `after`, une seule image est en vol |

Sur un écran à 120 Hz, ces durées devraient doubler, mais ce n'est pas mesuré. L'estimation de 4 à 10 ms du tableau
des couches supposait un tel écran.

### `NtYieldExecution` : pas de changement

- **Le jeu ignore la valeur de retour.** `RocketLeague.exe` appelle `SwitchToThread` depuis environ 70 endroits.
  - La plupart sont des verrous en attente active : `lock cmpxchg` sur un octet, puis `pause` 1, 2, 4, 8 et 16 fois,
    puis `SwitchToThread` à chaque nouvel essai.
  - Les autres sont des boucles sur un drapeau global.
  - Après chaque appel, `eax` est remis à zéro ou rechargé : la valeur de retour n'est jamais lue.
- **Un yield moins cher n'économiserait pas de CPU.** Un fil dans ces boucles tourne jusqu'à ce que le verrou se
  libère, quel que soit le coût d'un tour. Un yield moins cher donne plus de tours, pas moins de CPU. L'estimation
  « environ 0,1 à 0,2 cœur » du tableau des couches était donc fausse : on gagnerait 2 appels système par yield, et
  une reprise de verrou plus rapide de moins d'une microseconde.
- **Décision de l'utilisateur : on ne touche à rien.** D'autres jeux peuvent lire la valeur de retour de
  `SwitchToThread`. `thread_rusage` de LKD continue de renvoyer 0 changement de contexte, donc `SwitchToThread`
  répond toujours FALSE sous le moteur.

### Mesures retirées

À la demande de l'utilisateur, aucune mesure ne tourne plus à chaque image dans wsd. Le détecteur de frame back
(copie GPU de 64 pixels et deux lectures CPU de 64 pixels par image) est retiré : le correctif de Neptune `22d94ed`
a supprimé ce qu'il cherchait. Pour s'en servir à nouveau, il faut reprendre wsd à `ff71f31`. La sonde de latence
reste, car elle ne fait rien tant qu'aucune entrée `wsdctl` ne l'active.

## Release Voile v26.3.0-9 (4 octobre, 02:00)

Contenu, pour le moteur Linux :
- ntsync ;
- les correctifs de fichiers ;
- le correctif des frame backs ;
- wsd sans son détecteur ;
- FEXCore sans ses assertions (`e20b270`).

voile-lkd et Neptune sont maintenant des sous-modules de Voile, comme voile-wine. `build-linux.sh` les compile ; les
archives `lkd-*` et `neptune-*` ne servent plus.

| Mesure | v26.3.0-9 |
|---|---|
| Archive | 400 Mo (406,5 Mo pour la -8) |
| Rocket League, EAC | `bAntiCheatEnabled=(True)` sur tous les lancements qui ont atteint le jeu |
| Programmes témoins x86-64, FEX sans assertions | mêmes codes de sortie qu'avec assertions (21 sur 21) |

**Retrait des tables de symboles : essayé, puis abandonné.** Voile retirait déjà les infos de débogage ; il a
essayé d'enlever aussi les tables de symboles :
- PE : `--strip-all` ;
- Mach-O : `strip -x` pour les bibliothèques, `strip` pour les programmes ;
- ELF : `--strip-all`.

Le gain était de 32 Mo. À la demande de l'utilisateur, Voile est revenu à `strip -S` et `--strip-debug`, comme pour
la -8. Pendant les essais, deux incidents :
- **Premier lancement après l'installation** (moteur sans symboles), donc avec mise à jour du préfixe par
  wineboot :
  - Rocket League s'est arrêté au démarrage sur des données corrompues (`Bad name index 359464960/2678:
    Core.upk`) ;
  - puis le lanceur d'EAC a planté (liste au pointeur `0xffffffffffffd8f0`).
- **Lancement avec mise à jour du préfixe forcée** (`.update-timestamp`), même moteur : écran noir. Deux fils du
  jeu tournaient à 99 %, et une section critique était bloquée depuis plus de 60 s.

Les lancements suivants ont tous marché, sans les symboles comme avec : un moteur avec assertions, le moteur sans
assertions non élagué, puis élagué, puis le paquet final. La cause n'est pas trouvée. Les deux incidents tombent sur
une mise à jour du préfixe, quand beaucoup de processus Windows démarrent ensemble ; ntsync est une piste.

Ce qu'il faut faire au prochain incident :
- avoir `FEXDARWIN_DUMP_DIR` dans `engine/fexdarwin.env` ;
- toucher `dump-request` pendant le blocage ;
- pour reproduire, forcer la mise à jour (`printf '1\n' > <prefix>/.update-timestamp`).

**Bug de wsd sur deux écrans.** Le `HELLO` de wsd donne la taille de `NSScreen.mainScreen`, l'écran de la fenêtre
active. Avec l'écran du MacBook allumé, Rocket League a reçu 3024x1964 alors que sa fenêtre était sur l'écran à
2560x1440, et son image dépassait. Corrigé dans `6a197ac` : wsd donne l'écran de la barre des menus, qui est
déjà l'origine de ses coordonnées. Vérifié avec Obelisk sur l'écran du MacBook : le jeu reçoit 2560x1440.

## Mesures suivantes

- **Appels système par thread et par type.** Le profil donne les totaux par thread et les types par processus, mais
  pas les deux ensemble. C'est nécessaire pour savoir quel fil fait les requêtes à wineserver et ce qui borne
  `LoadScriptPackages`.
- **Requêtes de wineserver par type** (point 4 ci-dessus).
- **Temps d'image sans limite** (`NPT_WSD_PACING=off`, limite d'i/s du jeu au maximum), pour connaître la vraie marge
  et la pièce qui sature en premier.

## Reproduire

- Profil : `FEXDARWIN_PROFILE=1` (et `FEXDARWIN_HITCH_LOG=1`, au choix) dans `engine/fexdarwin.env` ; ajouter
  `guest WINE_OBELISK_LATENCY=1` pour les heures du pilote. Les lignes vont dans `~/Library/Logs/Obelisk/Epic/wine-<jeu>.log`.
- Fils natifs et GPU : `clang -O2 -o engine-top tools/dev/engine-top.c -framework IOKit -framework CoreFoundation`,
  puis `engine-top 5`.
- Latence : au menu, `wsdctl $TMPDIR/obelisk-engine-<pid>/wsd.sock tap 56` (Maj, active la sonde), puis
  `tap 125`/`tap 126`. En Free Play, `tap 49` (caméra balle). Lignes `wsd: probe:` dans le journal.
- Bancs d'essai : compiler avec `x86_64-w64-mingw32-clang -O2` (llvm-mingw de Voile). Les lancer par
  `crates/game-runtime/src/linux_engine/launch.sh` d'Obelisk, sous `run-with-timeout.sh`, puis vérifier
  `pgrep -x fexdarwin-run`.

## Cadence sur l'écran du MacBook et wineserver qui tourne à vide (7 octobre)

Machine : la même, mais sur l'écran intégré (ProMotion, 120 Hz), sur secteur. Runtime Voile `v26.3.0-11`. Scène :
Free Play, Forbidden Temple, caméra balle, voiture à l'arrêt, fenêtre 2560x1408, VSync décochée. Les lancements se
font depuis Obelisk, piloté par l'accessibilité, et le jeu est conduit au clavier et à la souris par `wsdctl`.

### Mesures

| Mesure | v26.3.0-11 | Cadence `latch` | `latch` + correctif epoll |
|---|---|---|---|
| Images affichées, Free Play | 30,5 i/s | 120 i/s | 120 i/s |
| Images affichées, menu | 33 i/s | 120 i/s | 120 i/s |
| Entrée → image qui réagit soumise (12 essais) | 62 à 98 ms, médiane 77 | 18 à 30 ms, médiane 20 | 17,5 à 23,6 ms, médiane 21 |
| Soumission → affichage | 33 ms | 32 ms | 32 ms |
| CPU du moteur (engine-top), Free Play | 0,55 cœur | 2,22 cœurs | 1,46 cœur |
| wineserver | | 78 % d'un cœur | 8 % d'un cœur |
| Free Play débridé (cadence `off`, `UncappedFramerate=True`) | | 184 à 196 i/s, 3,0 cœurs | 185 à 210 i/s, 2,4 cœurs |
| Démarrage (`EULA sync response`) | 20,5 s | | 20,7 s |
| EAC (`bAntiCheatEnabled`) | True | True | True à chaque lancement |

### Cadence : attendre la prise de l'image, plus son affichage

Avec la cadence `after`, nptunix attendait l'évènement `EV_PRESENTED` de chaque image, donc la composition de macOS
(3 à 4 rafraîchissements sur l'écran intégré, 25 à 33 ms) faisait partie de chaque image. Sur un écran à 120 Hz, le
jeu plafonnait à 30 i/s. Les intervalles affichés tombaient sur 25 et 33,3 ms, des multiples du tick de 8,33 ms : le
display link tournait bien à 120 Hz.

La nouvelle cadence `latch` (par défaut) attend `EV_RELEASE` de l'image envoyée, reconnue par son numéro de série :
wsd l'a copiée dans un drawable à un tick, ou l'a remplacée. Le jeu reste calé sur la fréquence de l'écran avec une
seule image en file, et la composition se superpose aux images suivantes. `after`, `before` et `off` restent
disponibles par `NPT_WSD_PACING`.

Vérifié : VSync activée puis désactivée, passage en sans bordure puis retour en fenêtré, fenêtre minimisée (environ
16 i/s, attentes bornées à 100 ms) puis restaurée (retour à 118 i/s en quelques secondes).

Essai écarté : demander au display link la fréquence maximale de l'écran (`preferredFrameRateRange`). Aucun effet,
le display link tournait déjà à 120 Hz.

### epoll de LKD : un filtre qui ne surveille que la fermeture tournait en boucle

C'est l'anomalie d'epoll de wineserver notée le 3 octobre. Un compteur dans `do_wait` a montré environ 300 000 tours
à vide par seconde, toujours sur une socket. wineserver met `events` à 0 sur une socket de client qu'il ne veut pas
lire tout de suite. LKD enregistrait quand même un filtre kqueue de lecture, déclenché par niveau, pour voir la
fermeture. Tant que la socket gardait des données non lues, kqueue la signalait à chaque attente, `ep_collect` ne
trouvait rien de demandé, et `epoll_wait` repartait aussitôt.

Correctif : quand l'élément ne demande ni `EPOLLIN` ni `EPOLLRDNORM` (`events` à 0, ou `EPOLLRDHUP` seul), le filtre
de lecture est déclenché par front (`EV_CLEAR`). Il réveille une fois par nouvelle donnée ou à la fermeture. Quand le
mode du filtre change, LKD le retire puis le remet. Test `hangup_only_watch_does_not_spin` dans `test_net_epoll` :
il échoue avant le correctif (le temps CPU de l'attente) et passe après. Les six suites réseau passent.

Deux constats sans rapport avec ce correctif :
- LKD ne signale pas `EPOLLHUP` sur une socket dont le pair a fermé tant qu'elle garde des données non lues (même
  résultat avant et après).
- `test_sig_futex` bloque dans le cas `waitv` (même résultat avant et après).

### Profil du fil de rendu (Free Play débridé)

Un échantillonneur temporaire (suspension de chaque fil invité, PC hôte traduit en RIP invité par
`RestoreRIPFromHostPC`, retiré depuis) donne :
- code du jeu 15 à 17 % ;
- `d3d11.dll` de Neptune 13 à 15 %, dont 5 % dans la soumission au ring (verrou, `sfence`, barrière complète,
  échange atomique à chaque commande) ;
- `memmove` et `memcmp` d'ucrtbase 5 % ;
- l'attente active du jeu (`NtYieldExecution` : deux `getrusage` et un `sched_yield`) environ 30 % ;
- des allers-retours avec wineserver 6 à 14 % : `NtCreateEvent`, puis `get_inproc_sync` qui va chercher le
  descripteur ntsync de chaque nouvel évènement ;
- le reste en attentes ntsync.

Le fil principal attend surtout le fil de rendu (ntsync) et fait des ioctls de socket (13 %). Fautes SMC : 4 en 11 s,
JIT : 4 ms. Dans le serveur de rendu, environ 9 % du décodeur part dans `dxmt::guid::make_guid_helper` :
`__uuidof` y relit le texte du GUID à chaque `QueryInterface`.

### Essais écartés

| Essai | Résultat |
|---|---|
| `TSOEnabled=0` | Le jeu plante dans `d3d11.dll` (le ring de Neptune dépend de l'ordre mémoire x86). |
| QoS user-interactive pour tous les fils invités | 189 à 211 i/s débridé (moyenne +6 %), mais un lancement sur six sans EAC. |
| D3DMetal (serveur de rendu x86_64 sous Rosetta) | Image correcte, 119,4 i/s, 212 i/s débridé (comme DXMT), démarrage 22,5 s. Premier lancement sans EAC. |
| Métadonnées volatiles des PE (TSO désactivé hors accès volatils) | Aucun des jeux installés n'en a. |

### EAC : la course du montage du module

Sur tous les journaux de lancement de la journée, EAC est absent exactement quand « Wine module mapping » a duré
2,31 s (d'habitude 0,37 à 0,41 s ; un lancement à 1,08 s est resté actif). Les deux cas : un lancement avec la QoS
user-interactive, et le premier lancement de D3DMetal, pendant que Rosetta traduisait le serveur de rendu. Toute
charge forte au moment du lancement peut donc désactiver EAC. C'est à corriger pour lui-même.

Suite (8 octobre). Les deux échecs durent 2,305 et 2,306 s : c'est un délai fixe d'environ 2 s dans la bootstrapper,
pas une lenteur quelconque. Les deux lancements ratés démarraient aussi le jeu bien plus lentement (préinitialisation
de 5,4 s contre 1,4 s avec D3DMetal). Un lancement avec les 18 cœurs saturés par 18 boucles `yes` pendant 25 s à partir
du clic : montage en 0,386 s, EAC actif. Une charge CPU ordinaire ne suffit donc pas. Aucun échec sur plus de 45
lancements avec la configuration retenue. Ce qu'attend la bootstrapper pendant ces 2 s reste inconnu.

## La cadence suit la VSync du jeu (8 octobre)

Le `SyncInterval` de chaque `Present` va maintenant de Neptune (D3D11 et D3D12) jusqu'à nptunix (Neptune `144f613`,
voile-wine `5ec9624`). Un `Present` avec VSync attend la prise de l'image par wsd (`latch`), un `Present` sans VSync
n'attend rien, comme sur Windows. `NPT_WSD_PACING` impose toujours un mode pour toutes les images. Obelisk recopie le
`d3d11.dll` du runtime dans le préfixe avant chaque lancement, donc le pilote et nptunix ont toujours la même structure
de paramètres.

| Lancement (Free Play) | Écran | `SyncInterval` reçu | Images soumises | Images affichées | EAC |
|---|---|---|---|---|---|
| VSync décochée, FPS illimités | MacBook 120 Hz | 0 | 190 i/s (menu 385) | 120 i/s | True |
| VSync décochée, FPS limités (`UncappedFramerate=False`) | MacBook 120 Hz | 0 | 194 i/s | 120 i/s | True |
| `UseVsync=True` + `UncappedFramerate=True` | 240 Hz, fenêtré et plein écran | 0 sur 20 270 et 15 062 images | 179 à 187 i/s | 173 à 179 i/s | True |
| VSync cochée dans le jeu (`UncappedFramerate=False`) | 240 Hz | ≥ 1 (attente de cadence 3,9 à 5,6 ms) | 176 i/s | 176 i/s | True |

Rocket League n'envoie la VSync que si les FPS ne sont pas illimités. `UncappedFramerate=False` ne limite rien
ici. Démarrage inchangé (20,7 s). En plein écran, soumission → affichage tombe à 12,6 ms (24 ms en fenêtré).

À voir : sur l'écran externe à 240 Hz (XG27AQWMG), wsd n'affiche pas plus de 173 à 179 i/s, avec ou sans cadence,
même quand le jeu en soumet davantage.
