# Chemin de lancement SailOver d'un jeu Epic Windows (Rocket League)

Date : 2026-10-02. Dépôt `sailover`, branche `main`, HEAD `04efbff` (seul `docs/` est non suivi). Tout a été lu ou mesuré en lecture seule : aucun jeu, aucun EAC, aucun SailOver, aucun Wine n'a été lancé pour ce rapport.

Conventions.
- LU : constaté dans le code ou un document cité (`fichier:ligne`).
- MESURÉ : constaté sur le disque de cette machine (listing, taille, hash, analyse statique d'un fichier sans l'exécuter).
- DÉDUIT : inférence de ma part, jamais présentée comme un fait.
- Abréviations de chemins : `GR` = `crates/game-runtime/src`, `EK` = `crates/storefronts/epic-kit/src`, `SC` = `crates/sailover-core/src`, `SN` = `crates/steam-native/src`, `LIB` = `~/Library/Application Support/com.zagrios.sailover`, `EXP` = `build/experiments/rocket-wine-fex-darwin-20261002`. Les citations `rocket-league-macos.md:N` désignent `docs/research/rocket-league-macos.md`. `voile/` et `voile-wine/` sont les dépôts frères de `Zagrios/`.

## 0. Limites de cette lecture

- Je n'ai pas lu `EpicSessions/`, `SteamSessions/`, `SteamCloud*`, les fichiers de registre du prefix (`system.reg`, `user.reg`, `userdef.reg`), ni les journaux EAC du prefix. Les « réglages de registre » de la section 4 viennent donc du code, pas du registre réel.
- Le classifieur de permissions a refusé la lecture du journal `~/Library/Logs/SailOver/Epic/wine-Sugar.log` (297 octets). Je n'ai pas contourné ce refus : son contenu est inconnu, seuls son nom, sa taille, son mode et sa date (liste de répertoire) sont connus.
- `docs/architecture.md` et `docs/epic-protocol.md` n'existent plus dans l'arbre : le commit `7d4d22f` (2026-09-24, « remove docs ») les a supprimés. Je les ai relus par `git show 7d4d22f^:docs/...` (lecture seule). Ils datent d'avant le câblage Wine du 26 septembre et portent l'ancien nom « Forge ».
- Contradictions relevées dans le dépôt : voir la section 8.1.

## 1. Les faits qui comptent pour l'architecture

1. Le lancement d'un jeu Epic Windows via Wine est déjà câblé de bout en bout à HEAD (`GR/epic/content.rs:357-416`, `GR/epic/windows.rs:10-44`, `GR/epic/launch.rs:258-274`, `GR/wine.rs:161-199`). Le paragraphe `README.md:74` qui dit le contraire est obsolète, comme le note déjà la chronologie (`docs/research/rocket-league-macos.md:15`). Les traces d'un lancement réel du 26 septembre existent sur disque (MESURÉ : `wine-Sugar.log`, `anticheatlauncher.log`, `service.log`, noms et dates seulement) et la chronologie rapporte qu'il atteint le bootstrapper EAC puis s'arrête sur le code launcher 206 (`rocket-league-macos.md:19-25`).
2. Trois processus se relaient : l'application (interface Swift et bibliothèque Rust dans le même processus), un superviseur détaché `sailover-steam-host epic-supervise`, puis un helper arm64 `sailover-steam-host game-exec` qui fait `exec` du binaire `bin/wine` (x86-64, sous Rosetta). Le contrat neutre entre superviseur et moteur est la structure `Target` (`GR/supervisor/mod.rs:41-50`) et le message `exec::Admission` (`GR/supervisor/exec.rs:13-22`).
3. Pour Rocket League, SailOver lance `Binaries/Win64/Launcher.exe` (MESURÉ dans le reçu), avec pour répertoire de travail `Binaries/Win64`. C'est un petit programme .NET de Psyonix (79 184 octets) qui, par défaut, lance `RocketLeague_EAC.exe` avec les mêmes arguments, puis attend (IL décodé, section 3.7). Il faut donc Wine Mono, ou lancer `RocketLeague_EAC.exe` directement.
4. Il y a deux codes d'échange Epic distincts : l'un ouvre la session Epic propre du superviseur (transmis par un tube privé), l'autre est le `-AUTH_PASSWORD=` de la ligne de commande du jeu (donc visible dans `argv` du processus Wine une fois lancé, section 3.5).
5. Il n'existe aucun CLI de lancement. Les coutures de test existantes sont le trait `Services` (`GR/epic/launch.rs:18-36`), le trait `Launch` (`GR/supervisor/session.rs:117-181`) et le message `Admission` du helper `game-exec` (exemple d'usage : `crates/steam-native/tests/exec_game.rs:15-33`).
6. Le point d'insertion le plus petit pour un chemin « userland Linux sous FEX » est la fonction qui produit un `Target` (aujourd'hui `wine::target`, `GR/wine.rs:181-199`), plus la préparation du prefix (`wine::prepare`, `GR/wine.rs:161-176`), plus le contrôle de la porte Rosetta (`GR/supervisor/session.rs:581`).
7. D3DMetal s'active par deux moyens combinés : les DLL d'Apple posées dans `lib/wine/x86_64-windows` du runtime et deux variables d'environnement que SailOver positionne seulement si `lib/external/libd3dshared.dylib` existe (`GR/wine.rs:80-83,134-140`). `SAILOVER_GRAPHICS` n'est lue par aucun code de SailOver : elle n'existe que dans `voile/scripts/wine-dev.sh`.
8. La bibliothèque installée contient `Voile/current` (runtime de développement, prioritaire, sans `runtime.json`) et `Voile/v26.3.0-4`, alors que le dépôt épingle `v26.3.0-6` (MESURÉ, `GR/voile.rs:65-73`, `voile-release.json`).

## 2. Pipeline de lancement, dans l'ordre réel

L'ordre réel diffère de l'ordre demandé : le prefix et les prérequis sont préparés AVANT toute authentification, parce qu'un prérequis peut durer des minutes et qu'un code d'échange est de courte durée (`GR/epic/windows.rs:7-9`, `GR/epic/content.rs:377-378`).

Arbre de processus d'un lancement (schéma de ma main : LU pour les trois premiers niveaux, DÉDUIT de l'IL de `Launcher.exe` et de la chronologie pour le dernier) :

```
SailOver.app                       interface Swift + bibliothèque Rust, un seul processus
 `- sailover-steam-host epic-supervise     détaché (setsid), env = PATH seul, un JSON Request sur stdin
     `- sailover-steam-host game-exec      arm64, env = Target.environment, un JSON Admission sur stdin
         = exec  <Voile>/bin/wine Launcher.exe <args>        x86-64, traduit par Rosetta
             `- wineserver, puis Launcher.exe (Mono) -> RocketLeague_EAC.exe -> RocketLeague.exe
```

| # | Étape | Fonction | Fichier:ligne |
|---|---|---|---|
| 0 | Clic Launch côté interface | `GameInstallations.launch` appelle `core.launch(installation:verify:)` | `apps/macos/SailOver/Models/GameInstallations.swift:507-508`, pont `crates/sailover-bridge/src/installations.rs:506-508` |
| 1 | Admission : la Reservation est prise avant toute attente | `Installations::launch` puis `Sessions::admit` | `GR/installations.rs:841-865`, `GR/sessions.rs:590-612` |
| 2 | Vérification des fichiers (réseau Epic), seulement si `verify` ou si le reçu l'exige | `shared.perform(&verification)` ; la requête vient de `Epic::update_request` | `GR/installations.rs:849,897-901`, `GR/epic/mod.rs:303-305,343-352` |
| 3 | Remise au superviseur | `Content::launch` puis `Admission::hand_off(Handover)` | `GR/epic/content.rs:248-266`, `GR/sessions.rs:841-864` |
| 4 | Sélection et installation du runtime Voile | `Handover::voile()` (Some si build Windows) puis `Canceller::install_runtime` ; résolution par `voile::runtime` | `GR/epic/content.rs:369-374`, `GR/sessions.rs:784-810`, `GR/voile.rs:61-73,116-186` |
| 5 | Préparation du prefix : programme valide, `wineboot`, Mono, dossiers, prérequis | `Handover::prepare` appelle `prepare_windows` puis `wine::prepare` (thread bloquant) | `GR/epic/content.rs:375-392`, `GR/epic/windows.rs:10-44`, `GR/wine.rs:161-176` |
| 6 | Authentification côté interface : compte puis code d'échange puis recontrôle du compte | `account_id()`, `exchange_code()`, `account_id()` | `GR/epic/content.rs:393-399` |
| 7 | Sauvegarde des identifiants renouvelés avant que le jeu parte | `Handover::before_begin` | `GR/epic/content.rs:403-406`, `GR/sessions.rs:843,432-434` |
| 8 | Création du superviseur et remise de la requête par socketpair privé | `epic::start` puis `Client::start_with` | `GR/epic/client.rs:8-43`, `GR/supervisor/client.rs:384-450` |
| 9 | Le superviseur devient un processus détaché | `sailover-steam-host epic-supervise` (`setsid`) puis `epic::serve` | `SN/bin/sailover-steam-host.rs:18,90-95`, `SN/native.rs:243-264`, `GR/epic/launch.rs:301-303` |
| 10 | Ouverture du journal, du socket de contrôle, du verrou propriétaire | `lifecycle::serve` | `GR/supervisor/session.rs:404-492` |
| 11 | Porte Begin : attend le feu vert de l'interface (120 s max) | `Session::begun` | `GR/supervisor/session.rs:36-37,357-372,569-571` |
| 12 | Authentification du superviseur : sa propre session depuis le code d'échange | `Launch::authenticate` puis `Client::from_exchange_code` | `GR/epic/launch.rs:136-140`, `EK/client.rs:78-82`, `EK/auth.rs:137-146` |
| 13 | Cloud EGS : ouverture et préparation avant le jeu | `Launch::cloud` puis `CloudSync::prepare` | `GR/epic/launch.rs:142-156`, `GR/supervisor/session.rs:573-575` |
| 14 | Plan de lancement : second code d'échange, jeton de propriété éventuel, arguments Epic | `Launch::target` puis `launch_plan` puis `windows_target` | `GR/epic/launch.rs:158-173,258-274`, `EK/client/launch.rs:150-304` |
| 15 | Porte Rosetta (build aarch64 et architecture Intel) | `wait_for_rosetta` | `GR/supervisor/session.rs:577-594`, `GR/supervisor/architecture.rs:143-172` |
| 16 | Préparation de l'hôte (helper, sous-commande, environnement) | `Launch::prepare_host` | `GR/epic/launch.rs:175-190` |
| 17 | Création du helper et admission : un seul message JSON sur son stdin | spawn `game-exec`, `exec::send` | `GR/supervisor/session.rs:596-657`, `GR/supervisor/exec.rs:27-35` |
| 18 | Le helper devient le jeu (`exec` de `bin/wine`) | `game_exec` | `GR/supervisor/exec.rs:73-86`, `SN/bin/sailover-steam-host.rs:19-23` |
| 19 | Confirmation d'exec, identité du processus, phase Running, présence Epic | `confirm_execution`, `wait_identity`, `Presence::start` | `GR/supervisor/child.rs:28-118`, `GR/supervisor/session.rs:675-695` |
| 20 | Supervision : journal, socket de contrôle, observation, arrêt, révocation | `listen`, `respond`, `Session::update` | `GR/supervisor/session.rs:731-889,334-345` |
| 21 | Sortie : attente du processus, phase Finalizing, synchro Cloud de sortie, phase finale | `wait_for_exit`, `LaunchCloud::finished`, `update` | `GR/supervisor/session.rs:696-728`, `GR/supervisor/child.rs:16-24` |
| 22 | Nettoyage : présence arrêtée, session Epic fermée, jeton de propriété supprimé, socket et verrou libérés | `Presence::stop`, `Launch::close`, drop de `Host` | `GR/supervisor/session.rs:523-557`, `GR/epic/launch.rs:88-91,206-210` |

Détails qui pèsent sur un nouveau chemin.

- Sélection du runtime (`GR/voile.rs:61-73`) : `LIB/Voile/current` s'il contient `bin/wine` (installé à la main par `voile/scripts/install-dev.sh`, il prime) ; sinon `LIB/Voile/<version épinglée>` si `bin/wine` existe ET si son `runtime.json` porte cette version ; sinon aucun runtime (`WineUnavailable`). Si aucun runtime n'existe, `install_runtime` télécharge l'archive épinglée (`voile-release.json` : URL, taille, SHA-256) en HTTPS avec contrôle de taille et de SHA-256, l'extrait par `/usr/bin/tar -xJf` dans un dossier caché, vérifie `bin/wine` et la version, renomme, puis supprime les autres versions sauf `current` (`GR/voile.rs:116-226`). Une build Debug lit `SAILOVER_VOILE_ARCHIVE` (copie locale de l'archive, SHA-256 contrôlé) à la place du téléchargement (`GR/sessions.rs:813-822`).
- La Reservation (`Sessions::admit`, `GR/sessions.rs:590-612`) est exclusive par storefront : un seul superviseur Epic à la fois, gardé aussi par `start.lock` et `owner.lock` (`GR/supervisor/client.rs:394-403`, `GR/supervisor/session.rs:412-419`).
- L'ordre des étapes du superviseur est dicté par le trait `Launch` (`GR/supervisor/session.rs:112-181`) : authentification, Cloud, cible et architecture, porte Rosetta (puis cible de nouveau après installation de Rosetta), préparation de l'hôte, admission d'exec.
- La confirmation d'exec réussit à l'EOF du tube CLOEXEC du helper, ou, à défaut, quand l'exécutable du processus inspecté est égal au `target.executable` canonique après 3 s (`GR/supervisor/child.rs:72-79`). Un nouveau moteur dont le processus final n'a pas ce chemin d'exécutable sera donc confirmé par l'EOF du tube ou pas du tout.
- Le code de sortie enregistré est celui du processus `wine` suivi (`GR/supervisor/child.rs:16-24`). Si l'exec n'a jamais été confirmé, la session se termine `Failed` (`GR/supervisor/session.rs:715-727`).
- Arrêt : SIGTERM au superviseur (quand SailOver se termine) appelle `Session::terminated` (`GR/supervisor/session.rs:279-289,497-506`) : `kill -TERM` du jeu, `wineserver -k` du prefix, 5 s de grâce, `kill -KILL` (`GR/supervisor/session.rs:294-320`), puis `wine::end_orphans` (`GR/wine.rs:466-490`) qui tue par `lsof` tout processus qui exécute un fichier sous le dossier donné : le dossier du runtime à l'arrêt (`GR/supervisor/session.rs:286-288`), le dossier `Voile` au démarrage de l'application (`AppLifecycle.swift:27`).
- DÉDUIT, risque de supervision : `Settings.json` d'EAC contient `"wait_for_game_process_exit": "false"` (MESURÉ). Si le bootstrapper rend la main avant la fin du jeu, le processus `wine` suivi par SailOver (celui de `Launcher.exe`) se termine avant le jeu. La session passerait alors à Finalizing/Finished alors que `RocketLeague.exe` tourne encore dans le prefix. À vérifier lors du premier essai protégé.

## 3. Forme exacte du lancement d'un jeu Epic Windows

### 3.1 Processus, arguments, répertoire de travail

LU, assemblé de `GR/wine.rs:181-199`, `GR/epic/launch.rs:258-274`, `GR/supervisor/session.rs:596-657`, `GR/supervisor/exec.rs:77-86`.

```
helper  : <app>/Contents/Helpers/SteamNative/sailover-steam-host game-exec   (arm64, non traduit)
          env_clear, puis exactement Target.environment ; stdin = socketpair ; stdout = tube ; stderr = /dev/null
admission (un JSON sur le stdin du helper) :
  executable        = <runtime>/bin/wine                       (x86-64, donc Rosetta)
  arguments         = [ <chemin Mac canonique de Launcher.exe>,
                        <args du reçu : launch_command>, <args catalogue : AdditionalCommandLine>,
                        -AUTH_LOGIN=unused, -AUTH_PASSWORD=<code>, -AUTH_TYPE=exchangecode,
                        -epicapp=<app_name>, -epicenv=Prod, -EpicPortal,
                        -epicusername=<nom affiché>, -epicuserid=<id de compte>,
                        -epiclocale=<locale>, -epicsandboxid=<namespace>,
                        [-epicdeploymentid=<id>], [-epicovt=Z:\...\ownership.bin] ]
  working_directory = <install>/Binaries/Win64                 (parent de l'exécutable du reçu)
  log               = ~/Library/Logs/SailOver/Epic/wine-<app_name>.log
puis le helper fait exec(executable, arguments) avec cwd = working_directory,
  stdin = /dev/null, stdout = /dev/null, stderr = le log (créé, tronqué, mode 0600)
```

Précisions.
- L'argument 1 de Wine est le chemin Unix canonique du `.exe`, vérifié comme situé dans l'installation et de format PE (`GR/wine.rs:202-211,555-566`). Wine le traduit en `Z:\Users\...`. Le chemin contient une espace (`Application Support`).
- L'ordre des arguments est : exécutable, arguments du reçu, arguments du catalogue, les neuf arguments Epic fixes, `-epicdeploymentid` s'il existe, puis `-epicovt` ajouté en dernier par `windows_target` (`EK/client/launch.rs:194-217`, `GR/epic/launch.rs:266-272`).
- Le répertoire de travail et l'exécutable sont confinés à l'installation, liens symboliques résolus (`SC/launch.rs:119-128`).
- Le helper `game-exec` refuse de tourner traduit (`sysctl.proc_translated`) et exige son état arm64 (`SN/bin/sailover-steam-host.rs:11-13`, `SN/native.rs:186-198`). Il porte les droits `allow-dyld-environment-variables` et `disable-library-validation` (MESURÉ par `codesign -d --entitlements` sur le build Debug de `build/xcode/...`).

### 3.2 Environnement complet du processus Wine

LU : base `native::environment(home)` (`GR/native.rs:69-102`) puis `Config::environment()` (`GR/wine.rs:115-142`), cette dernière écrasant la base (`GR/wine.rs:190-191`).

| Variable | Valeur | Origine |
|---|---|---|
| `HOME` | dossier personnel du joueur (`Locations.home`) | `GR/native.rs:75` |
| `PATH` | `/usr/bin:/bin:/usr/sbin:/sbin` | `GR/native.rs:76-79` |
| `TMPDIR` | `TMPDIR` de l'interface, sinon `std::env::temp_dir()` | `GR/native.rs:80-83` |
| `USER`, `LOGNAME` | repris de l'interface s'ils existent | `GR/native.rs:85-88` |
| `LANG`, `LC_ALL`, `LC_CTYPE` | repris s'ils existent et ne sont pas vides, sinon `LANG=en_US.UTF-8` | `GR/native.rs:89-100` |
| `WINEPREFIX` | `LIB/Prefixes/<nom du dossier d'installation>` | `GR/wine.rs:52-55,117-120` |
| `WINEDLLOVERRIDES` | `mshtml=;winemenubuilder.exe=d` (runtime avec Wine Mono), sinon `mscoree,mshtml=;winemenubuilder.exe=d` | `GR/wine.rs:97-103` |
| `WINEDEBUG` | première ligne de `<runtime>/winedebug`, sinon `err+all,fixme-all` | `GR/wine.rs:107-113` |
| `WINEMSYNC` | `1` (Mach semaphores ; le serveur et tous les processus du prefix doivent être d'accord) | `GR/wine.rs:126-129` |
| `WINE_LARGE_ADDRESS_AWARE` | `1` | `GR/wine.rs:130-132` |
| `CX_APPLEGPTK_LIBD3DSHARED_PATH` | `<runtime>/lib/external/libd3dshared.dylib`, seulement si ce fichier existe | `GR/wine.rs:80-83,134-138` |
| `CX_ACTIVE_GRAPHICS_BACKEND` | `d3dmetal`, seulement si ce fichier existe | `GR/wine.rs:139` |

Aucune variable `DYLD_*` n'est posée pour Wine. Le superviseur lui-même démarre avec `env_clear` et `PATH=/usr/bin:/bin` seulement (`GR/supervisor/client.rs:371-378`), d'où `std::env::temp_dir()` = `/tmp` dans le superviseur (DÉDUIT : `TMPDIR` absent). La requête `Request.environment` est construite dans le processus de l'interface (`GR/epic/client.rs:24`).

MESURÉ sur cette machine : ni `LIB/Voile/current/winedebug` ni `LIB/Voile/v26.3.0-4/winedebug` n'existent, donc `WINEDEBUG=err+all,fixme-all` est la valeur effective. Les deux runtimes ont `lib/external/libd3dshared.dylib`, donc les variables `CX_*` sont posées.

### 3.3 Canaux de debug Wine

- Par défaut `err+all,fixme-all` (`GR/wine.rs:112`). Un fichier `winedebug` à la racine du runtime remplace cette valeur pour TOUTES les commandes Wine de SailOver (jeu, `wineboot`, prérequis), car `environment()` le relit à chaque appel (`GR/wine.rs:107-113`). Documenté dans `voile/README.md` (exemple `err+all,+steamclient`).
- Pour un run avec identifiants, ne pas activer `+process`, `+relay`, `+server` ni `+all` : voile-wine trace la ligne de commande sous le canal `process` (`voile-wine/dlls/kernelbase/process.c:529`, `voile-wine/dlls/ntdll/unix/process.c:1248`). DÉDUIT : `+relay` et `+server` peuvent aussi l'afficher.
- Sortie : seul le stderr du processus du jeu va dans le journal ; celui de `wineboot` est jeté (`GR/wine.rs:247-250`) ; celui des prérequis va dans `<prefix>/sailover-prerequisites.log` (`GR/wine.rs:380-427`).

### 3.4 Arguments Epic, un par un

LU dans `EK/client/launch.rs` sauf mention contraire. Aucune valeur n'est reproduite ici.

| Argument | D'où vient la valeur | Ligne |
|---|---|---|
| exécutable | champ `launch_executable` du reçu `.sailover-install.json`, issu du manifeste de build Epic (MESURÉ : `Binaries/Win64/Launcher.exe`) | 129-147 |
| arguments du reçu | champ `launch_command`, découpé selon les règles Windows (MESURÉ : vide pour Rocket League) | 139-145, `SC/launch.rs:164-211` |
| arguments du catalogue | attribut personnalisé `AdditionalCommandLine` du catalogue Epic, lu sur le réseau (inconnu pour Rocket League) | 255-258, 194-197 |
| `-AUTH_LOGIN=unused` | constante | 199 |
| `-AUTH_PASSWORD=<code>` | code d'échange : `GET account/api/oauth/exchange` avec le jeton d'accès de la session propre du superviseur ; 1 à 2048 caractères `[A-Za-z0-9_-]` | 200, 272-283 |
| `-AUTH_TYPE=exchangecode` | constante | 201 |
| `-epicapp=<app_name>` | `app_name` du reçu (nom d'application Epic, public) | 202 |
| `-epicenv=Prod` | constante | 203 |
| `-EpicPortal` | constante | 204 |
| `-epicusername=<nom affiché>` | `display_name` des identifiants de la session (réponse OAuth), après un recontrôle du compte | 205, 190-193 |
| `-epicuserid=<id de compte>` | `account_id` des identifiants de la session ; doit égaler celui du reçu | 206, 172-180 |
| `-epiclocale=<locale>` | `Configuration.language` fourni par l'interface (`_` devient `-`), sinon `en` | 207, 313-325, `GR/epic/mod.rs:105` |
| `-epicsandboxid=<namespace>` | `namespace` du reçu (identifiant de bac à sable public, le même que `sandboxid` d'EAC, section 3.7) | 208 |
| `-epicdeploymentid=<id>` | `deployment_id` du reçu, seulement s'il existe (MESURÉ : absent) | 210-217 |
| `-epicovt=Z:\...\ownership.bin` | chemin Windows d'un fichier qui contient les octets du jeton de propriété, ajouté seulement si le catalogue porte `OwnershipToken=true` (inconnu pour Rocket League) | `GR/epic/launch.rs:242-272`, `EK/client/launch.rs:259-271` |

### 3.5 Obtention et transmission du code d'échange et du jeton de propriété

Code d'échange n° 1, celui de la session du superviseur.
- L'interface demande `account_id` puis `exchange_code` (`GET account/api/oauth/exchange`) avec sa propre session, dont les identifiants renouvelés sont sauvegardés (`GR/epic/content.rs:393-406`, `EK/client.rs:84-104`).
- Il traverse un socketpair privé : stdin du superviseur reçoit le `Request` JSON, stderr renvoie un accusé unique, et le commentaire du code précise que ni l'un ni l'autre n'atteint un fichier de journal (`GR/supervisor/client.rs:407-418`). Selon le commentaire de `GR/epic/mod.rs:221-235`, le `Request` n'est jamais persisté, jamais formaté avec `Debug`, jamais mis dans `argv`.
- Le superviseur l'échange contre sa propre session (`grant_type=exchange_code`) et vérifie que le compte est le bon, sinon il révoque (`EK/auth.rs:137-146`). Cette session est distincte du jeton mémorisé de l'interface (`GR/epic/launch.rs:21`).

Code d'échange n° 2 et jeton de propriété, ceux du jeu.
- Le plan de lancement se fait dans le superviseur : contrôle de propriété, lecture du catalogue, puis `try_join!` du code d'échange et, si exigé, du jeton de propriété (`POST ecommerceintegration .../identities/<compte>/ownershipToken`, corps `nsCatalogItemId=<namespace>:<catalogItemId>`) (`EK/client/launch.rs:235-283`, `EK/auth.rs:228-257`).
- Le code n° 2 entre dans `LaunchPlan.target.arguments`. `LaunchPlan` n'a volontairement pas de `Debug` (`EK/client/launch.rs:17-26`). Il voyage ensuite dans le message `Admission` (JSON, socketpair du helper, `GR/supervisor/exec.rs:27-35`), puis devient `argv` du processus Wine. Il est donc présent dans `argv` du processus Wine pendant la vie du jeu (DÉDUIT du fait qu'il fait partie de `Target.arguments`) ; je ne sais pas qui peut le lire sous macOS 27.
- Le jeton de propriété est écrit dans un dossier `sailover-epic-ownership-*` en 0700, fichier `ownership.bin` (`GR/epic/launch.rs:242-256`). Seul son chemin Windows `Z:\...` est dans `argv`. Le dossier vit tant que `Host` vit : `run` le garde jusqu'à son retour, donc après la sortie du jeu et la synchro Cloud de sortie (`GR/epic/launch.rs:88-91,182-189`, `GR/supervisor/session.rs:599`).
- Aucun des deux ne figure dans un reçu (`.sailover-install.json` : 12 clés, aucune ne porte un jeton ou un code ; `account_id` est un identifiant de compte, MESURÉ) ni dans le journal de session `Record` (`GR/supervisor/mod.rs:111-135`).

### 3.6 Ce qui se fait sans aucun identifiant

La préparation du prefix et des prérequis n'utilise aucun identifiant Epic : `prepare_windows` ne lit que le reçu local (`installed_launch`, `EK/client/launch.rs:63-99`, appelé par `GR/epic/windows.rs:25-31`). Un moteur de laboratoire peut donc préparer et tester un prefix sans compte.

### 3.7 Exécutable lancé pour Rocket League

MESURÉ.
- Reçu `.sailover-install.json` du dossier d'installation (champs non secrets seulement) : `app_name=Sugar`, `platform=Windows`, `build_version=++Prime+Update60-CL-528869-pchotfix4`, `launch_executable=Binaries/Win64/Launcher.exe`, `launch_command` vide, pas de `deployment_id`, prérequis `ids=["EAC_Service"]`, `path=Binaries/Win64/EasyAntiCheat/EasyAntiCheat_EOS_Setup.exe`, `arguments="install e6bcca5b37d0457ca881aec508205542"`.
- SHA-256 inchangés depuis le début de l'effort : `RocketLeague.exe` `ca3a9075...38fa`, `RocketLeague_EAC.exe` `903e7aba...14d1` (comparés à `evidence/game-sha256-start.txt`). `Launcher.exe` : `40d1bb2d02989907894d29d59769f51975ea9b930255696e49be1f61fd515eb9` (non présent dans le fichier de départ).
- `EasyAntiCheat/Settings.json` : `executable=RocketLeague.exe`, `productid=e6bcca5b37d0457ca881aec508205542`, `sandboxid=9773aa1aa54f4f7b80e44bef04986cea`, `deploymentid=da32ae9c12ae40e8a112c52e1f17f3ba`, `wait_for_game_process_exit=false`, `hide_bootstrapper=false`, `hide_gui=false`.

Donc SailOver lance `Launcher.exe`, jamais `RocketLeague.exe` ni `RocketLeague_EAC.exe` directement, depuis `Binaries/Win64`.

Analyse statique de `Launcher.exe` (assembly .NET 1.0.0.0, société « Psyonix LLC »), IL décodé avec `dnfile` et `dncil` installés dans `EXP/tools/venv`, sans exécution. Logique de `Main` (RVA 8512) et de l'itérateur `LaunchOptions` (RVA 8880) :
1. Le répertoire courant devient le dossier de l'assembly (`Environment.CurrentDirectory = dirname(Assembly.GetEntryAssembly().Location)`).
2. Liste ordonnée des options : `("-eac","RocketLeague_EAC.exe")`, `("-noeac","RocketLeague.exe")`, `("-offline","RocketLeague.exe")`, filtrée par `File.Exists`. Si aucune n'existe, code de retour 1.
3. Pour chaque option dans l'ordre, si un argument égale le drapeau (`OrdinalIgnoreCase`), lance l'exécutable associé avec les arguments restants (le drapeau retiré).
4. Sans drapeau, lance le premier : `RocketLeague_EAC.exe` avec TOUS les arguments (`Main` 042c-043c).
5. `Launch(exe, args)` : `Process.Start(new ProcessStartInfo(exe, string.Join(" ", args)) { UseShellExecute=false, RedirectStandardOutput=false })`, `WaitForExit()`, retourne le code de sortie (`Launch` 02f0-032f). Les arguments sont rejoints par une espace sans guillemets (conséquence DÉDUITE : un `-epicusername=` avec une espace serait scindé en deux arguments pour l'enfant).
6. Une exception écrit `Launcher.log` dans le répertoire courant (`Main` 0446-0452).

Conséquences. Le chemin protégé par défaut est `Launcher.exe` puis `RocketLeague_EAC.exe` (bootstrapper EAC 1.9.3 selon le journal cité par `rocket-league-macos.md:12`), qui lit `Settings.json`, télécharge le module `linux64` depuis `modules-cdn.eac-prod.on.epicgames.com/modules/{{productid}}/{{deploymentid}}/{{system}}` (chaînes du binaire) et le charge par un chargeur propre à Wine (`wine_get_version`, `/proc/self/fd/%i`, `/proc/%i/maps`), puis lance `RocketLeague.exe`. Sous Wine macOS cette étape échoue avec le code launcher 206 (`rocket-league-macos.md:19-27,47-61`). `-noeac` et `-offline` lancent le jeu sans EAC : hors objectif.

## 4. Prefix Wine

### 4.0 Bibliothèque installée (listing en lecture seule)

`LIB` (`drwxr-xr-x`), noms et tailles (`du -sk`, 1 Ko = 1024 octets) :

| Entrée | Mode | Taille | Remarque |
|---|---|---|---|
| `EpicSessions/` | 0700 | non mesurée | journaux et sockets des sessions Epic ; contenu non lu |
| `SteamSessions/`, `SteamCloud/`, `SteamCloudValidation/` | 0700 | non mesurées | non lus |
| `SteamCloud-sdk/` | 0755 | non mesurée | non lu |
| `Games/Epic/` | 0755 | `Games/` : 103 290 020 Ko au total | 6 installations : 3 `-Mac`, 3 `-Windows` ; Rocket League est `epic-52edd447ec86748fc2afecc6cd67acc7748a5461-Windows`, 42 690 576 Ko (reçu : 43 667 660 596 octets) |
| `Games/Steam/` | 0755 | inclus ci-dessus | non détaillé |
| `Prefixes/` | 0755 | 8 095 316 Ko | 7 prefix, 0700 chacun : 2 Epic Windows, 5 Steam ; de 1 086 360 à 1 256 688 Ko chacun ; celui de Rocket League : 1 086 672 Ko |
| `Voile/current/` | 0755 | 1 396 744 Ko | runtime de développement : `bin`, `lib`, `share`, `steam`, sans `runtime.json` ni `licenses` ; Wine Mono `wine-mono-10.4.1` dans `share/wine/mono` |
| `Voile/v26.3.0-4/` | 0755 | 523 760 Ko | archive de release : `bin`, `lib`, `licenses`, `runtime.json`, `share`, `steam` ; pas de Wine Mono dans `share/wine/mono` |
| `epic-installations-<hex>.json`, `steam-installations-<id>.json`, `library.lock` | 0644 | 1 841 / 1 719 / 0 octets | reçus de compte ; non lus |

`runtime.json` de `Voile/v26.3.0-4` (MESURÉ) : `version v26.3.0-4`, `voile 6d2bbb44...`, `crossover 26.3.0`, `voile-wine 9861d8b4...`, `proton 5b89db94...`, `d3dmetal "Evaluation environment for Windows games 4.0 beta 2"`. `Voile/current` n'a pas de manifeste : sa version exacte n'est pas identifiable par fichier.

### 4.1 Création et mise à jour

LU, `GR/wine.rs`.
- Chemin : `LIB/Prefixes/<nom du dossier d'installation>` (`GR/wine.rs:52-55`, `GR/locations.rs:44`). Pour Rocket League : `epic-52edd447ec86748fc2afecc6cd67acc7748a5461-Windows` (MESURÉ), mode 0700 (`GR/files.rs:66-77`, appelé par `GR/wine.rs:219`).
- `boot` (`GR/wine.rs:215-237`) : runtime absent donne `WineUnavailable` ; création du dossier ; `wineboot --init` si `system.reg` manque ; si le runtime porte Wine Mono et que le témoin `sailover-mono` diffère, `wineboot --update` (sauf création) puis écriture du témoin ; enfin `unlink_home_folders` transforme en dossiers vides les liens vers le home Mac laissés par d'anciens runtimes (`GR/wine.rs:266-282`).
- `wineboot` : `<runtime>/bin/wine wineboot --init|--update`, `env_clear`, environnement natif + Wine, stdio nuls ; puis `wineserver -w` avec le seul `WINEPREFIX` (`GR/wine.rs:241-260`). Échec si l'un des deux n'est pas un succès.
- Le prefix est « préparé avant chaque lancement » (`CONTEXT.md`, entrée Wine prefix) ; `prepare` crée aussi le dossier des journaux (`GR/wine.rs:172-175`).
- Suppression : `Installation::remove` fait `wineserver -k` puis `remove_dir_all`, sans suivre de lien (`GR/wine.rs:337-356`).
- Profil Windows : `drive_c/users/crossover`, nom fixé par CrossOver (`GR/wine.rs:22-39`). Il sert de racine aux sauvegardes Cloud (`GR/epic/mod.rs:120-129`).

### 4.2 Contenu mesuré du prefix de Rocket League (listing seulement)

`LIB/Prefixes/epic-52edd447ec86748fc2afecc6cd67acc7748a5461-Windows` : 1 086 672 Ko (1,04 Gio), créé le 26 septembre à 22:37.
- Racine : `.update-timestamp` (12 octets), `dosdevices/`, `drive_c/`, `sailover-mono`, `sailover-prerequisites`, `sailover-prerequisites.log`, `system.reg` (3,3 Mo), `user.reg` (66 Ko), `userdef.reg` (4 Ko).
- `dosdevices/` : `c:` vers `../drive_c`, `z:` vers `/`, plus `d:` vers `/Volumes/VirtualBuddy_v2.2-417` et `d::` vers `/dev/rdisk13s1` (lecteurs détectés par `wineboot` au moment de la création, sans rapport avec le jeu).
- `drive_c/` : `Program Files` (Common Files, Internet Explorer, Windows Media Player, Windows NT), `Program Files (x86)` (Common Files, `EasyAntiCheat_EOS` avec `EasyAntiCheat_EOS.exe`, Internet Explorer, Windows Media Player, Windows NT), `ProgramData/Microsoft`, `users/` (`Public`, `crossover`), `windows/`.
- `drive_c/users/crossover/` : `AppData` (`Local` avec `Microsoft` et `Temp`, `LocalLow`, `Roaming` avec `EasyAntiCheat` et `Microsoft`), `Contacts`, `Desktop`, `Documents`, `Downloads`, `Favorites`, `Links`, `Music`, `Pictures`, `Saved Games`, `Searches`, `Videos`. Ce sont des dossiers réels, non des liens ; tous sont vides sauf `Pictures`, qui contient un sous-dossier `Screenshots`. Il n'y a pas de `Documents/My Games` : DÉDUIT, le prefix n'a jamais exécuté le jeu jusqu'au bout.
- `AppData/Roaming/EasyAntiCheat/` : `service.log` et `e6bcca5b37d0457ca881aec508205542/da32ae9c12ae40e8a112c52e1f17f3ba/anticheatlauncher.log` (noms seulement ; contenu non lu).
- Contenu des témoins SailOver (non secrets) : `sailover-mono` = `wine-mono-10.4.1` ; `sailover-prerequisites` = `epic:EAC_Service` ; `sailover-prerequisites.log` = trois lignes (`msync: bootstrapped mach port on wine-164e476-msync.`, `msync: up and running.`, `err:ntoskrnl:ZwLoadDriver failed to create driver ... winebth: c00000e5`).

### 4.3 Prérequis

- Pour Epic, le prérequis vient du reçu : un programme sous l'installation, avec une ligne de commande, identifié `epic:<ids joints par virgule>`, ou `epic:<chemin>` si `ids` est vide (`GR/epic/windows.rs:52-77`). Un chemin qui sort de l'installation est ignoré.
- Pour Rocket League : `Binaries/Win64/EasyAntiCheat/EasyAntiCheat_EOS_Setup.exe install e6bcca5b37d0457ca881aec508205542`, lancé par `wine <programme> install <productid>` avec pour répertoire de travail le dossier du programme (`GR/wine.rs:396-437`). Un `.cmd` ou `.bat` passerait par `wine cmd /c Z:\...`.
- Chaque id ne tourne qu'une fois par prefix (témoin `sailover-prerequisites`, une ligne par id). Codes de réussite 0, 1638, 3010 ; délai 10 minutes puis `wineserver -k` ; un échec rejoue au lancement suivant et le jeu démarre quand même (`GR/wine.rs:25-33,362-393`). Un `wineserver -w` final attend la fin des processus de l'installeur.
- MESURÉ : le service EAC est installé dans ce prefix (`Program Files (x86)/EasyAntiCheat_EOS`), id `epic:EAC_Service` enregistré.

### 4.4 Surcharges de DLL et registre

- SailOver n'écrit aucune clé de registre elle-même : un grep de `crates/game-runtime/src` (hors tests) pour `reg`, `regedit`, `HKEY`, `winetricks` ne trouve que le test d'existence de `system.reg` (`GR/wine.rs:220`). Les réglages passent par des variables d'environnement. Pour Epic, `furnish` est le comportement par défaut du trait, qui ne fait rien (`GR/wine.rs:151-153`) ; Steam, lui, copie `steamclient.dll` et `steamclient64.dll` dans `Program Files (x86)/Steam` du prefix (`GR/steam/windows.rs:63-77`).
- `WINEDLLOVERRIDES` : `winemenubuilder.exe=d` (pas d'entrées de menu) ; `mshtml=` (pas d'invite Gecko) ; `mscoree` n'est neutralisé que si le runtime n'a pas de Wine Mono (`GR/wine.rs:94-103`).
- Les DLL que Wine remplace ou ajoute viennent du runtime : voir 4.5.
- Registre réel du prefix : non lu (section 0). DÉDUIT : `wineboot` enregistre le .NET de Wine Mono (`GR/wine.rs:224-226`) et l'installeur EAC enregistre son service.

### 4.5 D3DMetal

- Dans le runtime (MESURÉ, `LIB/Voile/current/lib/wine`) : `x86_64-windows/d3d10.dll`, `d3d11.dll`, `d3d12.dll`, `dxgi.dll`, `nvapi64.dll` datés du 10 juillet (DLL d'Apple), à côté des `wined3d.dll` de Wine datés du 26 septembre ; côté Unix, `x86_64-unix/d3d10.so`, `d3d11.so`, `d3d12.so`, `dxgi.so`, `nvapi64.so`, `nvngx-on-metalfx.so` sont des liens vers `../../external/libd3dshared.dylib` ; `lib/external/D3DMetal.framework` est présent. La partie `i386-windows` reste celle de Wine (D3DMetal ne couvre que le 64 bits).
- Dans SailOver : `Config::d3dmetal()` teste `lib/external/libd3dshared.dylib` ; s'il existe, `CX_APPLEGPTK_LIBD3DSHARED_PATH` pointe dessus et `CX_ACTIVE_GRAPHICS_BACKEND=d3dmetal` (`GR/wine.rs:78-83,134-140`). Côté Wine, `voile-wine/dlls/ntdll/unix/loader.c:1443` lit la première, `dlls/win32u/d3dkmt.c:719` et `dlls/win32u/vulkan.c:3007` lisent la seconde.
- `SAILOVER_GRAPHICS` n'est lue que par `voile/scripts/wine-dev.sh` (valeurs `wined3d` par défaut ou `d3dmetal`, qui superpose D3DMetal à un clone du runtime). Elle n'existe nulle part dans le code de SailOver (grep sur `crates`, `apps`, `scripts`, `packaging`). DÉDUIT : SailOver ne sait pas choisir wined3d à la place de D3DMetal, puisque les DLL sont déjà superposées dans le runtime installé.
- Le build Wine de Voile est x86-64 (`--enable-archs=i386,x86_64 --without-x --without-vulkan`, `voile/scripts/build.sh:33-34`) et fonctionne sous Rosetta : `bin/wine` et `bin/wineserver` sont des Mach-O x86_64 (MESURÉ, `lipo -archs`).

### 4.6 Rôle des scripts et du test de Voile

LU dans `voile/scripts/*` et `voile/tests/d3d11-smoke.c`. Voile ne crée pas les prefix des jeux : c'est SailOver (`wine::boot`). Voile fournit le runtime et un moyen de le tester.
- `build.sh` : construit Wine pour x86-64 avec `--enable-archs=i386,x86_64 --disable-tests --without-x --without-gstreamer --without-sdl --without-vulkan` (lignes 33-34), exige Rosetta (`arch -x86_64 /usr/bin/true`, ligne 43), produit `build/stage`.
- `install-dev.sh` : copie `build/stage` dans `LIB/Voile/current` (ou `$SAILOVER_LIBRARY_DIRECTORY/Voile/current`), pose les DLL de D3DMetal par-dessus `lib/` et copie `steamclient.dll` et `steamclient64.dll` dans `steam/`.
- `package.sh <version>` : construit, retire les infos de debug, ajoute D3DMetal après le `strip` (licence d'Apple), écrit `runtime.json`, crée `voile-<version>.tar.xz` et son SHA-256, puis vérifie l'archive extraite : `wineboot --init` dans un prefix neuf puis `cmd.exe` 64 bits et 32 bits avec `ver`, sous `WINEDEBUG=-all` et `WINEDLLOVERRIDES=mscoree,mshtml=;winemenubuilder.exe=d`. Refuse les modifications non commitées.
- `publish.sh` : crée la release GitHub de la version. `import-d3dmetal.sh` : extrait les Redistributables de D3DMetal du disque du Game Porting Toolkit.
- `wine-dev.sh` : lance le Wine de `build/stage` avec `WINEPREFIX=build/prefix` par défaut ; `SAILOVER_GRAPHICS=d3dmetal` superpose D3DMetal à un clone APFS du runtime et pose `CX_APPLEGPTK_LIBD3DSHARED_PATH` et `CX_ACTIVE_GRAPHICS_BACKEND=d3dmetal`. Il ne pose pas les autres variables de 3.2.
- `tests/d3d11-smoke.c` : crée un périphérique et une chaîne d'échange D3D11, présente 120 images et affiche l'adaptateur et le niveau de fonctionnalités ; se compile avec `x86_64-w64-mingw32-clang`. D'après le `README` de Voile, wined3d s'arrête à 10_1 et D3DMetal annonce « AMD Compatibility Mode » à 11_0. C'est un test d'acceptation graphique réutilisable pour un nouveau chemin, sans EAC ni identifiants.
- Il n'y a pas d'autre test dans `voile/tests/`.

## 5. Journaux, lancement sans interface, identifiants

### 5.1 Où vont les journaux

| Quoi | Où | Source |
|---|---|---|
| stderr de Wine pendant le jeu | `~/Library/Logs/SailOver/Epic/wine-<app_name>.log` (Rocket League : `wine-Sugar.log`, 297 octets, mode 0600, 26 sept 22:38 ; contenu non lu) | `GR/epic/windows.rs:19-24`, `GR/locations.rs:42`, `GR/supervisor/exec.rs:57-69` |
| sortie des installeurs de prérequis | `<prefix>/sailover-prerequisites.log` (réécrit à chaque lancement qui en exécute un) | `GR/wine.rs:29,380` |
| journal EAC | `<prefix>/drive_c/users/crossover/AppData/Roaming/EasyAntiCheat/<productid>/<deploymentid>/anticheatlauncher.log` et `service.log` (MESURÉ : fichiers présents) | `rocket-league-macos.md:19-25` |
| journal de `Launcher.exe` | `Launcher.log` dans `Binaries/Win64`, seulement sur exception (IL) | section 3.7 |
| journal de session | `LIB/EpicSessions/` (`journal::write`), socket de contrôle dans `/tmp/sailover-session-*/control` (0700) | `GR/supervisor/session.rs:420-447` |
| manifeste Epic de diagnostic | `~/Library/Logs/SailOver/Epic/manifest-Sugar-Windows.json` et `.manifest` | MESURÉ (liste) |

Le dossier `~/Library/Logs/SailOver/Epic` est en 0700, les journaux Wine en 0600. Le stdout du jeu est jeté (`GR/supervisor/exec.rs:82-84`).

### 5.2 Lancer sans l'interface : ce qui existe et ce qui manque

LU : il n'existe aucun CLI de lancement. Les binaires du dépôt sont `uniffi-bindgen` (`crates/sailover-bridge/src/bin/uniffi-bindgen.rs`), `sailover-steam-runtime-package` (`crates/game-runtime/Cargo.toml:34-36`, outil de paquetage), le helper `sailover-steam-host` (`crates/steam-native/src/bin/sailover-steam-host.rs`) et un exemple de sonde Cloud Steam (`crates/steam-native/examples/cloud_storage_probe.rs`, variables `SAILOVER_CLOUD_PROBE_*`). Les scripts `scripts/*.sh` construisent, testent, signent ou ouvrent l'application (`scripts/run.sh` fait `open` de `SailOver.app`) ; aucun ne lance un jeu. `README.md:98` : les tests « ne lancent pas de jeux ».

Quatre voies existent, de la plus fidèle à la plus légère.
1. **L'application elle-même**, avec une bibliothèque isolée : `SAILOVER_LIBRARY_DIRECTORY` est lue dans `apps/macos/SailOver/App/AppIdentity.swift:11` (et ne change pas l'identifiant du Trousseau, `README.md:129`). Les emplacements sont dérivés par `sailoverLocations(library:bundle:home:)` (`AppIdentity.swift:21`, `GR/locations.rs:30-48`). C'est la seule voie qui exerce les étapes 1 à 22 sans code nouveau.
2. **Le helper seul** : `sailover-steam-host game-exec` lit un `Admission` JSON sur stdin et fait `exec` (`crates/steam-native/tests/exec_game.rs:15-33` montre comment `Admission` est construit et envoyé avec `exec::send`). Il suffit de fournir l'environnement exact de 3.2 (le helper le transmet tel quel) et un `Admission` avec `executable=<runtime>/bin/wine`. Le build Debug est à `build/xcode/Build/Products/Debug/SailOver.app/Contents/Helpers/SteamNative/sailover-steam-host` (arm64, MESURÉ). Le helper doit tourner en arm64 natif, pas sous Rosetta.
3. **Le superviseur avec des services factices** : `lifecycle::serve::<Launch<S>>` est générique sur `Services` ; `FakeEpic` dans `GR/epic/launch/tests.rs:96-140` montre comment fournir un `LaunchPlan` sans réseau. Le harnais `EpicHarness` ré-entre le binaire de test comme superviseur jetable (`GR/supervisor/session/tests.rs:135-162,268-275`). Une implémentation `Services` « de laboratoire » est la couture prévue pour un lancement hors interface (section 7).
4. **Wine seul** : `voile/scripts/wine-dev.sh <programme>` (runtime `voile/build/stage`, prefix `voile/build/prefix` ou `WINEPREFIX`, `SAILOVER_GRAPHICS=d3dmetal`) ou l'appel direct du `bin/wine` installé. Il faut alors reproduire à la main les variables de 3.2, car `wine-dev.sh` ne pose ni `WINEMSYNC`, ni `WINEDLLOVERRIDES`, ni `WINEDEBUG`. C'est la voie qu'ont utilisée les essais sans authentification (`rocket-league-macos.md:27-45`, copies APFS du prefix).

Ce qu'une exécution fidèle hors interface doit fournir : un `Locations` (bibliothèque, home, bundle), un reçu installé dont le compte est celui de la session, un runtime (`Voile/current` ou version épinglée), le helper arm64 et le bundle Rosetta (`Contents/Helpers/SailOver Rosetta.app`, MESURÉ présent dans le build Debug ; `GR/epic/launch.rs:229-238` le localise comme le grand-parent de `config.helper`, `GR/supervisor/rosetta.rs:7-9` ajoute `SailOver Rosetta.app/Contents/MacOS/SailOverRosetta`), et un code d'échange frais pour un compte qui possède le jeu.

### 5.3 Comment les identifiants restent hors des journaux

LU.
- Chemins sans `argv` : `Request` par socketpair stdin/stderr, jamais persisté ni `Debug` (`GR/epic/mod.rs:221-235`, `GR/supervisor/client.rs:407-418`). `Admission` par socketpair, taille bornée à 1 Mio (`GR/supervisor/exec.rs:24-54`).
- Superviseur sans environnement hérité (`env_clear`, `PATH` seulement), sans stdout, stderr dédié à l'accusé (`GR/supervisor/client.rs:371-378,412-417`). Helper `game-exec` avec stderr vers `/dev/null` (`GR/supervisor/session.rs:608-610`).
- `LaunchPlan` sans `Debug` (`EK/client/launch.rs:17-18`). Les erreurs sont des clés de localisation `backend.epic.*` (`GR/epic/mod.rs:21-50`).
- Jeton de propriété dans un dossier 0700, écriture atomique (`GR/epic/launch.rs:242-256`). Zéroïsation pour les identifiants Steam (`GR/protocol.rs:1-56`).
- Journal Wine 0600, créé et tronqué à chaque lancement (`GR/supervisor/exec.rs:57-69`).

Limite : rien n'empêche Wine de journaliser une ligne de commande si l'on active les canaux de 3.3, et le journal EAC peut contenir ce que l'éditeur y écrit (inconnu). Le code d'échange n° 2 est dans `argv` du processus Wine.

## 6. Points d'entrée Rust pour un chemin « userland Linux sous FEX »

### 6.1 Ce qui est neutre et réutilisable tel quel

- `Target { executable, arguments, working_directory, environment, log }` (`GR/supervisor/mod.rs:40-50`) : description de spawn indépendante du moteur.
- `exec::Admission` et `game_exec` (`GR/supervisor/exec.rs`) : le helper arm64 fait `exec` de n'importe quel `executable` absolu, avec le répertoire de travail, l'environnement hérité du helper et un stderr redirigé vers `log`.
- Le trait `Launch` (`GR/supervisor/session.rs:117-181`) et tout le cycle `run` : authentification, Cloud, cible, porte Rosetta, hôte, admission, confirmation, supervision, sortie. `Prepared { host, helper, subcommand, environment }` (`GR/supervisor/session.rs:54-62`) permet déjà de choisir un autre helper et une autre sous-commande.
- L'authentification Epic et le plan de lancement (`EK/client/launch.rs`), le jeton de propriété (`GR/epic/launch.rs:242-256`), `native::environment` (`GR/native.rs:69-102`), le journal et la Reservation.
- Les contrôles de chemin (`SC/launch.rs`) et `portable_executable` (`GR/wine.rs:555-566`).
- La couture Wine est déjà partagée par deux storefronts : Steam appelle aussi `wine::prepare` et `wine::target`, et insère seulement `C:\windows\system32\steam.exe` en tête des arguments (`GR/steam.rs:88`, `GR/steam/windows.rs:79-90`). Un second moteur peut donc se brancher à cet endroit pour les deux.

### 6.2 Ce qui est propre à Wine sous macOS et Rosetta

| Élément | Fichier:ligne | Pourquoi c'est spécifique |
|---|---|---|
| `ARCHITECTURE = Intel` et la porte Rosetta | `GR/wine.rs:20-21`, `GR/supervisor/session.rs:581`, `GR/supervisor/architecture.rs:143-172`, `GR/supervisor/rosetta.rs` | `bin/wine` est x86-64 ; la porte teste la traduction et propose l'installation de Rosetta |
| `USER = "crossover"`, `profile()` | `GR/wine.rs:22-39` | nom de profil des prefix CrossOver ; sert aussi de racine Cloud (`GR/epic/mod.rs:120-129`) |
| `WINEMSYNC`, `WINE_LARGE_ADDRESS_AWARE`, `CX_*`, `libd3dshared.dylib` | `GR/wine.rs:80-83,115-142` | Mach semaphores, CrossOver, D3DMetal : rien de tout cela n'existe sous Linux |
| `windows_path` = `"Z:" + chemin` | `GR/wine.rs:296-298`, `GR/epic/launch.rs:268-271` | `Z:` est la racine du Mac ; dans un userland Linux, `Z:` serait la racine du Linux invité, donc le jeton de propriété et l'installation doivent y être visibles |
| `unlink_home_folders` | `GR/wine.rs:262-282` | liens vers le home macOS laissés par d'anciens runtimes |
| `wineboot`, `wineserver -w/-k` lancés en commandes hôte | `GR/wine.rs:241-260,455-459,512-522` | à exécuter dans l'invité pour un moteur Linux |
| `end_orphans` par `lsof` + `kill` sur les fichiers sous `Voile` | `GR/wine.rs:466-508`, appelé depuis `GR/supervisor/session.rs:286-288` (dossier du runtime) et `crates/sailover-bridge/src/locations.rs:52-57` (dossier `Voile`, au démarrage de l'application, `AppLifecycle.swift:27`) | repose sur des Mach-O mappés depuis `Voile/` |
| `Installation::of` (plateforme `windows`) et `runtime()` | `GR/wine.rs:311-332` | le sélecteur de runtime est ici, figé sur `locations.voile` |
| Installation, version et checksum de Voile | `GR/voile.rs`, `voile-release.json`, `GR/sessions.rs:784-822` | archive `tar.xz` épinglée, `Voile/current` de développement |
| `Request.wine`, `Configuration.prefix`, `runs_in`, `validate` | `GR/epic/mod.rs:93-164,233-234` | `Request.wine` est le seul champ transporté qui dit « ce build Windows tourne dans Wine » ; `Configuration.prefix` le double côté Cloud |
| `Handover::voile/prepare` | `GR/epic/content.rs:369-401` | installe Voile et prépare le prefix avant le code d'échange |
| `Launch::wine()` et les arrêts qui en dépendent | `GR/supervisor/session.rs:134-136,286-288,312-314,482` | `stop_game` appelle `wine::stop` |
| Droits du helper (`allow-dyld-environment-variables`, `disable-library-validation`) et refus d'être traduit | `packaging/steam-host.entitlements`, `SN/bin/sailover-steam-host.rs:11-13` | contraintes du binaire qui fait `exec` |

### 6.3 Proposition d'interface minimale

Les trois points demandés se placent ainsi : le sélecteur de runtime est `Installation::runtime` avec `voile::runtime` (`GR/wine.rs:330-332`, `GR/voile.rs:61-73`) ; la fonction de spawn est `wine::target` (`GR/wine.rs:181-199`) ; l'assemblage de l'environnement est `native::environment` plus `Config::environment` (`GR/native.rs:69-102`, `GR/wine.rs:115-142`), fusionnés dans `wine::target` et réutilisés par `wineboot` et les prérequis.

Objectif : un seul point d'insertion entre le pipeline (inchangé) et le moteur, sans toucher au superviseur, au Cloud ni à Epic. Les noms sont proposés ; chaque ligne indique la fonction actuelle qui devient l'implémentation Wine.

```rust
// crates/game-runtime/src/engine.rs (nouveau)
pub(crate) trait Engine: Send + Sync {
    /// Architecture du binaire que le helper fera exécuter. `None` : pas de porte Rosetta.
    fn architecture(&self) -> Option<Architecture>;            // wine::ARCHITECTURE
    /// Moteur installé et complet ? (runtime ou rootfs présent)
    fn installed(&self) -> bool;                               // Config::installed
    /// Prépare le prefix : création, mise à jour, prérequis. Bloquant, peut durer des minutes.
    fn prepare(&self, home: &Path, install: &Path, target: &LaunchTarget,
               storefront: &dyn Storefront) -> Result<(), Error>;   // wine::prepare
    /// Variables propres au moteur, fusionnées sur `native::environment(home)` pour le jeu,
    /// pour la préparation du prefix et pour les prérequis.
    fn environment(&self) -> HashMap<String, String>;           // Config::environment
    /// Forme de spawn du jeu : le helper arm64 fera exec de `Target.executable`.
    fn target(&self, install: &Path, target: &LaunchTarget,
              environment: HashMap<String, String>) -> Result<Target, Error>;   // wine::target
    /// Chemin tel que le programme Windows le voit (Z:\... aujourd'hui).
    fn guest_path(&self, host: &Path) -> String;                // wine::windows_path
    /// Racine du profil Windows côté hôte (Cloud, sauvegardes).
    fn profile(&self) -> PathBuf;                               // wine::profile(prefix)
    /// Termine tous les processus du prefix.
    fn stop(&self);                                             // wine::stop
    /// Termine les processus qu'un SailOver mort a laissés. Retourne leur nombre.
    fn end_orphans(&self) -> usize;                             // wine::end_orphans
}

#[derive(Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub(crate) enum EngineConfig {
    WineRosetta(wine::Config),        // l'existant
    LinuxFex(fex::Config),            // nouveau : rootfs, hôte FEX, prefix dans l'invité, journal
}
```

Branchements, dans l'ordre où je les ferais.
1. `Request.wine: Option<wine::Config>` (`GR/epic/mod.rs:233-234`) devient `engine: Option<EngineConfig>` ; `Configuration.prefix` reste (racine Cloud, `runs_in` compare avec `engine.prefix()`).
2. Sélecteur de runtime : `Installation::runtime` (`GR/wine.rs:330-332`) retourne un `EngineConfig`. Choix proposé : un témoin dans le prefix (`sailover-engine`) ou, en développement, un dossier `Engines/linux-fex/current` qui prime, sur le modèle de `Voile/current` (`GR/voile.rs:21,65-73`). Ajouter `engines` à `Locations` (`GR/locations.rs:7-28`) et à son miroir UniFFI (`crates/sailover-bridge/src/locations.rs:7-28,31-50,59-77`), puis régénérer les liaisons Swift (`scripts/build-rust.sh`).
3. `Handover::prepare` et `voile()` (`GR/epic/content.rs:369-401`) appellent `Engine::prepare` et l'installation du moteur correspondant ; `Canceller::install_runtime` (`GR/sessions.rs:784-810`) devient générique sur le moteur.
4. `windows_target` (`GR/epic/launch.rs:258-274`) appelle `Engine::target` et `Engine::guest_path` pour `-epicovt`. Il retourne `engine.architecture()`.
5. `Launch::wine()` (`GR/supervisor/session.rs:134-136`) devient `engine()` ; `stop_game` et `terminated` appellent `Engine::stop` et `end_orphans`.
6. Le test de la porte Rosetta (`GR/supervisor/session.rs:581`) est déjà conditionné par `architecture == Some(Intel)` : un moteur FEX qui retourne `Some(Arm64)` ou `None` la contourne sans autre changement.

Pour `Engine::target` d'un moteur FEX, le contrat à respecter est celui du helper : `executable` absolu, existant, dont le chemin canonique sera comparé à l'identité du processus (`GR/supervisor/session.rs:611-614`, `GR/supervisor/child.rs:94-118`). Il peut être arm64 (hôte FEX natif) ou x86-64 (cas actuel, traduit par Rosetta) ; seul le helper lui-même doit rester arm64 natif. L'assemblage de l'environnement de l'invité (variables Wine côté Linux, variables FEX) est à définir par l'équipe FEX : le `HashMap` de `Target.environment` est transmis tel quel au processus final (`GR/supervisor/exec.rs:13-14`). Je n'ai pas de mesure sur le futur hôte FEX Darwin ; ces contraintes sont DÉDUITES du code lu.

Chemin le plus court pour un essai de laboratoire (sans toucher au superviseur) : produire un `Target` avec une fonction analogue à `wine::target` et l'envoyer au helper `game-exec` (voie 2 de 5.2). Le trait `Engine` ne devient nécessaire que pour brancher le moteur dans le flux de l'application.

### 6.4 Autres points de vigilance pour le branchement

- Wine Mono : `Launcher.exe` est managé. Soit l'invité embarque Wine Mono (équivalent de `Voile/current/share/wine/mono/wine-mono-10.4.1`, `GR/wine.rs:86-92`), soit l'essai lance `RocketLeague_EAC.exe` directement avec les arguments qu'aurait passés `Launcher.exe` (section 3.7, reproduit sa logique par défaut sans modifier de binaire ; décision à prendre par l'équipe).
- Visibilité des fichiers dans l'invité : l'installation (`Games/Epic/...`), le fichier de jeton de propriété (`/tmp/sailover-epic-ownership-*/ownership.bin`) et le prefix doivent être joignables par le jeu. Aujourd'hui `Z:` les expose via la racine du Mac.
- Sauvegardes Cloud : la synchro EGS lit le profil Windows côté hôte (`GR/epic/mod.rs:120-129`). Un prefix dans un rootfs invité doit rester accessible à l'hôte, ou la synchro doit être désactivée par un `Launch::cloud` qui retourne `None` (`GR/supervisor/session.rs:144-147`).
- Pages mémoire de 16 Kio de l'hôte : sujet de l'équipe FEX/Darwin, non traité ici.

### 6.5 Ce que disaient les anciens documents sur le choix du moteur

Lu par `git show 7d4d22f^:...` (documents supprimés, antérieurs au câblage Wine, ancien nom « Forge »). Ils confirment l'intention d'un sélecteur, que le code n'a pas encore.
- `docs/architecture.md:86-92` (« Compatibility engines ») : le catalogue stocke des références de jeux, pas une version de Wine ; une future implémentation de lancement Windows « peut choisir un moteur et un profil pour chaque jeu » ; la configuration d'un moteur reste séparée des fichiers de l'utilisateur et permet de revenir à une version précédente.
- `docs/research-game-runtime.md:43,47,51` : garder remplaçables le traducteur CPU, le moteur graphique et le mode de synchronisation ; un profil de lancement doit enregistrer le prefix Windows, les exigences du client de boutique, l'exécutable et ses arguments, le moteur de rendu, le traducteur CPU, le mode de synchronisation, les dépendances, les surcharges de DLL et l'environnement ; prévoir Wine ARM64EC avec un traducteur CPU macOS plutôt que de supposer Rosetta pour toujours.
- `docs/epic-protocol.md:119-127` (« Launching ») : décrit le plan de lancement que le code actuel implémente (code d'échange frais, jeton de propriété écrit dans un fichier privé et passé par `-epicovt`, répertoire de travail = parent de l'exécutable) et affirme que les codes d'échange et jetons ne sont jamais stockés dans les reçus ni journalisés.

## 7. Démarrer une session EAC protégée en ligne de commande, sans identifiants dans les commandes ni les rapports

Mécanisme décrit seulement : rien n'a été lancé ni écrit. Objectif : un essai contrôlé avec EAC intact, où aucune valeur secrète ne passe par une commande, un fichier du dépôt, un journal ou un rapport.

### 7.1 Ce que le lancement exige (LU)

- Un compte Epic qui possède Rocket League et dont l'identifiant est celui du reçu (`EK/client/launch.rs:172-180`).
- Une session Epic vivante pour ce compte, d'où le code d'échange n° 2 et, si le catalogue l'exige, le jeton de propriété (`EK/client/launch.rs:183-188`).
- Une porte Begin ouverte si l'on passe par le superviseur (`GR/supervisor/session.rs:569-571`).

### 7.2 Mécanisme A (recommandé pour la première session) : SailOver comme courtier d'identifiants

L'application SailOver, lancée par le joueur, détient déjà la session (Trousseau, jeton de rafraîchissement). Le moteur de laboratoire est branché comme en 6.3 et choisi par un témoin ou un dossier `Engines/.../current`. Le joueur clique Launch. Aucune valeur secrète n'apparaît dans une commande, un fichier de l'essai ou un rapport ; la boîte de dialogue du Trousseau, s'il y en a une, est celle du joueur. L'essai ne collecte que des faits non secrets : phase de session, code de sortie, codes du lanceur EAC (206, 208, 210, ...), durées, journal du moteur. Inconvénient : pas de scénarisation en ligne de commande.

### 7.3 Mécanisme B : lanceur de laboratoire lisant un fichier d'environnement

Un petit binaire (exemple ou test de `game-runtime`, jamais ajouté à l'application) qui réutilise `Launch<S>` avec un `Services` de laboratoire, ou qui compose directement `Target` puis `Admission` pour `game-exec`.

Tous les noms de variables et de clés ci-dessous sont des propositions : rien de tel n'existe dans le dépôt.

1. Entrée : le CHEMIN d'un fichier d'environnement, passé par variable (par exemple `SAILOVER_LAB_ENV_FILE`) ou par argument : le chemin, jamais les valeurs. Le fichier est écrit par le joueur, jamais par un agent, hors du dépôt et hors de `EXP` (par exemple sous `~/.config/sailover-lab/`), mode 0600.
2. Contenu (noms de clés seulement) : `EPIC_AUTH_CODE` (code d'autorisation à usage unique obtenu par le joueur sur la page de connexion Epic, que `Client::sign_in` accepte déjà en entrée, `EK/client.rs:63-68`), ou `EPIC_ACCOUNT_ID` + jeton de rafraîchissement pour `Client::restore` (`EK/client.rs:70-74`). Le répertoire d'installation et le moteur sont des arguments non secrets.
3. Contrôles avant lecture : le lanceur refuse si le fichier a des bits groupe ou autres, s'il est dans une copie de travail git, ou s'il n'appartient pas à l'utilisateur courant. Même esprit que le contrôle du dossier de socket (`GR/supervisor/client.rs:453-461`).
4. Flux : lecture du fichier en mémoire (zéroïsée ensuite), `sign_in` ou `restore`, puis `launch_plan` (`EK/client/launch.rs:150-225`) qui produit `LaunchPlan` avec `-AUTH_PASSWORD=` et le jeton de propriété ; `Engine::target` ; écriture de l'`Admission` sur le stdin du helper `game-exec` par un tube, jamais sur sa ligne de commande.
5. Consommation : le fichier est vidé ou renommé dès la lecture (un code d'autorisation est à usage unique, un code d'échange de courte durée). Rien de secret ne reste au repos.
6. Journaux : le lanceur n'écrit jamais `argv`. S'il doit l'écrire, il remplace les valeurs de `-AUTH_PASSWORD=`, `-epicusername=`, `-epicuserid=` et `-epicovt=`. Il impose un `WINEDEBUG` sans `+process`, `+relay`, `+server`, `+all`, et refuse un fichier `winedebug` qui les contient (section 3.3).
7. Contrôle « canari » après l'essai : le lanceur cherche les chaînes secrètes qu'il a en mémoire dans les dossiers de journaux de l'essai (journal du moteur, journaux EAC copiés) et ne rapporte qu'un nombre d'occurrences. Si le nombre est non nul, les journaux restent en dehors des preuves archivées.
8. Rapport : exit code, phase, codes EAC, durées. Aucun identifiant ni nom affiché.

### 7.4 Précautions communes

- Travailler sur un clone APFS du prefix et utiliser une bibliothèque isolée (`SAILOVER_LIBRARY_DIRECTORY`), pour ne pas toucher `LIB/Prefixes/...` ni les verrous `EpicSessions/` d'un SailOver en cours.
- Ne jamais passer `-noeac` ni `-offline` (hors objectif) ; ne pas modifier `Settings.json`, `RocketLeague_EAC.exe` ni `Launcher.exe` ; vérifier les hash avant et après (`evidence/game-sha256-start.txt`).
- Le code d'échange n° 2 est dans `argv` du processus Wine pendant la vie du jeu : pendant l'essai, ne lancer aucun outil de diagnostic qui liste ou journalise les lignes de commande des processus (`ps`, `lsof -p`, traces `dtruss`, rapports de plantage joints) et les traiter comme sensibles s'ils existent.
- Les chaînes `-epicusername=` et `-epicuserid=` identifient le joueur : à traiter comme sensibles dans les rapports.

## 8. Inconnues, risques, contradictions

### 8.1 Contradictions dans le dépôt

- `README.md:74` : « Epic's Windows games remain downloadable, but launching them through Wine is not wired yet ». Faux à HEAD (sections 1 et 2). `rocket-league-macos.md:15` le dit aussi.
- `GR/voile.rs:179-181` : « only Steam launches Windows games ». Périmé : Epic aussi.
- `voile-release.json` épingle `v26.3.0-6`, alors que la bibliothèque contient `v26.3.0-4` et un `current`. `Voile/current` (sans `runtime.json`) prime (`GR/voile.rs:65-73`) : la version réellement exécutée par les essais est donc celle de `current`, non identifiable par manifeste. `runtime.json` de `v26.3.0-4` : voile `6d2bbb44...`, voile-wine `9861d8b4...`, proton `5b89db94...`, D3DMetal « Evaluation environment for Windows games 4.0 beta 2 ». HEAD de voile-wine : `e82937c9` (26 sept), HEAD de voile : `fb27a63`.
- La chronologie dit « Wine 11.0 » pour `Voile/current` (`rocket-league-macos.md:11`) ; je ne l'ai pas revérifié.

### 8.2 Inconnues

- Contenu de `wine-Sugar.log` (lecture refusée) et des journaux EAC : non lus.
- Si le catalogue Epic de Rocket League porte `OwnershipToken=true` et un `AdditionalCommandLine` : inconnu hors réseau. Le code gère les deux cas.
- Registre réel du prefix : non lu.
- Comment `RocketLeague_EAC.exe` utilise les arguments `-AUTH_*` et `-epic*` (il reçoit les mêmes arguments que `Launcher.exe`, d'après l'IL) : non analysé. Les chaînes du binaire montrent `-anticheat_settings=` et `Settings.json`.
- Si `Launcher.exe` sous Mono suffit : il utilise `Process.Start`, `Path`, `File`, `Assembly` : rien d'exotique, mais non exécuté ici.
- Comportement de supervision quand le bootstrapper rend la main avant le jeu (section 2).
- Pages de 16 Kio et FEX : hors de ma tâche.

### 8.3 Risques pour un nouveau chemin

- Le code d'échange n° 2 expire vite : tout temps passé entre `launch_plan` et l'exec réduit la marge. Dans le flux actuel, `prepare` (prefix, prérequis) est fait AVANT pour cette raison (`GR/epic/windows.rs:7-9`) ; un moteur dont la préparation serait longue doit rester avant l'authentification.
- Une seule session Epic à la fois et un seul propriétaire de `EpicSessions/` : un essai parallèle avec la bibliothèque réelle serait refusé (`Busy`) ou, pire, gênerait l'application.
- `end_orphans` tue par `lsof` tout processus qui exécute un fichier sous `Voile/` : un nouveau runtime hors de ce dossier ne serait pas nettoyé.
- Signature et droits du binaire final : le helper impose arm64 natif pour lui-même ; les droits dont un hôte FEX Darwin (JIT) aurait besoin ne sont pas étudiés ici.

## Annexe : mesures exécutées (toutes en lecture seule)

`ls`/`du` de `LIB`, `LIB/Voile`, `LIB/Prefixes`, `LIB/Games/Epic` et du prefix de Rocket League ; `cat` de `Voile/v26.3.0-4/runtime.json` et des témoins `sailover-*` ; `file`/`lipo -archs` sur `bin/wine`, `bin/wineserver`, `libd3dshared.dylib`, helper ; `codesign -d --entitlements` sur le helper du build Debug ; `shasum -a 256` sur `Launcher.exe`, `RocketLeague.exe`, `RocketLeague_EAC.exe` ; lecture des champs non secrets de `.sailover-install.json` par script (le champ `account_id` n'est pas imprimé) ; lecture de `EasyAntiCheat/Settings.json` ; `strings` et extraction des chaînes UTF-16 de `Launcher.exe` ; décodage IL de `Launcher.exe` avec `dnfile` et `dncil` (`pip install` dans `EXP/tools/venv`, seul changement fait sous `EXP` ; le script de décodage et sa sortie sont restés dans le scratchpad de session) ; `git show 7d4d22f^:docs/...` et `git ls-tree` pour les anciens documents ; `grep` du code de SailOver, de voile et de voile-wine. Aucun `git commit`, `push`, `branch` ni `stash` ; `git status` du dépôt `sailover` reste `?? docs/` ; `voile` et `voile-wine` sont propres. Aucune sortie contenant un secret n'a été produite.
