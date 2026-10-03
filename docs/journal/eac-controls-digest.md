# Digest des contrôles EAC : pourquoi un environnement est accepté et les autres rejetés

Date de rédaction : 2026-10-02. Rapport complet, relu une fois contre les sources.

Conventions. **Mesuré** ou **Lu** : le fait figure dans un fichier cité. **Inféré** : déduction de ce rapport, non démontrée par une expérience. **Inconnu** : aucune donnée. Toutes les durées sont en secondes. Le « délai » est le temps entre la ligne `Starting Wine module mapping` du journal du bootstrapper et le callback (ou le résultat du launcher), sauf mention contraire. Aucune valeur d'authentification n'a été lue ni copiée. Les écarts à la règle sur les fichiers dont le nom évoque un secret (`.private`, `auth`, `token`, `session`, `credential`) sont listés en section 6.

Abréviations de chemins.

- `B` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928`
- `P` = `B/probes`, `LR` = `B/linux-reference`, `PERF` = `P/rocket-performance-20260930`, `FEXN` = `PERF/fex-native-hyperlinux-20261001`
- `DIAG` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/diagnostics`
- `CHR` = `/Users/mathieu/Documents/Github/Zagrios/sailover/docs/research/rocket-league-macos.md` (chronologie, numéros de ligne du fichier)
- `TECH` = `/Users/mathieu/Documents/Github/Zagrios/sailover/docs/research/rocket-league-eac-technologies-macos-2026-10-01.md`

## 0. Résumé pour l'architecture

1. Deux environnements seulement ont produit un launcher 301 suivi d'une session EAC protégée : un PC Linux x86 physique (Bazzite, 72,82 s) et QEMU système x86 en TCG avec un noyau Linux x86 réel (91,54 s, sortie volontaire). Aucune pile à noyau ARM ou à traducteur ARM n'a produit 301.
2. La paire de contrôle la plus serrée oppose QEMU TCG (301 en 3,50 s, sans authentification) à HyperLinux avec Rosetta (509/210) : mêmes fichiers Wine (ntdll `e6ad055d...`), même préfixe d'origine, même Steam Runtime 4, même composant EAC Heroic, même charge utile EAC de 13 825 695 octets, mêmes quatre binaires du jeu, mêmes variables de synchronisation, aucune authentification. Le delta est la pile noyau et CPU.
3. Le 509/210 « Unexpected error. (#1) » apparaît aussi sur un vrai noyau ARM Linux (OrbStack) avec Rosetta (trois builds de Wine) et avec FEX non modifié (deux essais). Ce n'est donc pas un défaut propre à HyperLinux. Sur cette pile réelle, les 68 appels ptrace d'EAC visibles au PLT retournent 0 et l'enfant auxiliaire se termine quand même par `exit_group(1)`, sans écriture de registres observée par cet audit. Sous HyperLinux avec FEX natif, l'enfant se termine aussi par le code 1 (statut brut 256).
4. Quatre familles de verdicts distinctes ne doivent pas être fusionnées : 507/206 (résolveur libc de la bootstrapper), 502/208 « Launch Error: 256 » (Wine lancé comme PID 1), 502/208 « Cannot run under Virtual Machine. » (refus explicite), 509/210 (verdict résiduel de toutes les piles ARM qui atteignent l'initialisation du module).
5. Causes closes : ordre de la libc dans `/proc/self/maps` (507), PID 1 et métadonnées du parent (502/256), boucle locale (505), version de Wine, version de la charge utile, configuration produit, absence d'authentification. Cause ouverte : le 509. L'hypothèse ptrace x86 fidèle est la mieux étayée par la structure du module (21 sites d'appel à `ptrace`, 66 lectures de registres avant l'échec), mais aucun essai ne l'a testée avec tout le contrat réuni et rien n'exclut d'autres facteurs propres à l'architecture x86 (CPUID, `cpuinfo`, vecteur auxiliaire).
6. Le dernier refus connu sous FEX natif est `PTRACE_SETOPTIONS(0x11)` (TRACESYSGOOD|TRACEEXEC) qui retourne -95 (`FEXN/README.md:333-335`). Aucune trace des appels ptrace d'EAC n'existe dans un environnement accepté.
7. Le verdict « Cannot run under Virtual Machine. » est sélectif : WSL2 et Linux ARM sous HVF avec FEX l'ont reçu, pas QEMU TCG, pas OrbStack, pas HyperLinux. Les signaux utilisés sont inconnus.
8. La session protégée QEMU n'a duré que 91,54 s, sans reconnexion ni trace ptrace. La checklist Epic vérifiée en ligne exige plus de 10 minutes connecté puis la jonction d'une autre session protégée. Aucun environnement du dépôt n'a atteint ce critère.

## 1. Matrice des environnements

### 1.1 Familles de verdicts

| Famille | Callback / launcher | Texte | Lecture |
| --- | --- | --- | --- |
| F0 | 301 / terminaison 0 | `Easy Anti-Cheat successfully loaded in-game` | Module initialisé, jeu lancé protégé |
| F1 | 507 / 206 | `Failed to map the anti-cheat module.` puis `Failed to load the anti-cheat module.` | La bootstrapper n'a pas pu charger le module (résolveur libc, dlopen) |
| F2 | 502 / 208 | `Launch Error: 256` | Échec de lancement générique ; reproduit avec Wine PID 1 |
| F3 | 502 / 208 | `Cannot run under Virtual Machine.` | Refus explicite de VM |
| F4 | 509 / 210 | `Unexpected error. (#1)` | Verdict du module après chargement ; cause inconnue |
| F5 | 505 | échec DNS du CDN, client nul | Prérequis réseau local manquant (boucle locale) |

La bootstrapper ne construit pas le texte `(#1)` ni la valeur 256 : elle transmet le pointeur de message reçu du module, et aucun de ces textes n'existe en littéral dans son PE (`LR/eac-static-210/analysis.md:16,19`, `LR/eac-static-208/analysis.md:13-14,20`). Les causes de F2 et F4 ne se lisent donc pas dans le code de la bootstrapper.

**Mesuré.** Dans le run `real-parent` (F4, FEX natif), l'enfant auxiliaire 82 se termine avec le code 1 et le `wait4` hôte enregistre le statut brut 256 (`FEXN/real-parent-control/eac-result-summary.json`, `child_exit_1_actual_host_wait_records`). **Inféré, non démontré.** Le détail « 256 » de F2 et le « (#1) » de F4 sont tous deux compatibles avec « l'enfant s'est terminé avec le code 1 » (statut brut 256, code 1). Le run F2 sous FEX natif n'a pas de statut 256 enregistré (`FEXN/topdown-sparse-diagnostic-runtime/README.md`, troisième paragraphe) : l'hypothèse n'est donc pas confirmée pour F2. Si elle l'était, F2 et F4 seraient le même événement (échec de l'enfant) classé différemment selon le contexte de lancement.

### 1.2 Table

Les identifiants `S1` à `S16` (avec `S6b` et `S6c`) renvoient à la liste de sources qui suit la table.

| # | Environnement | Noyau et CPU | Wine et runtime | Auth. | Verdict | Session protégée | Délai | Sources |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | PC Linux x86 physique (Bazzite) | 6.18.52-ogc1.1.fc44, i7-10750H, `systemd-detect-virt` = none | Proton-CachyOS `1784681208` (Wine 11.0), UMU 1.4.4, steamrt4, Heroic | oui | F0 (essais 01 et 02) | oui, 72,82 s, arrêt volontaire | mapping vers 301 : 0,520 (essai 01, HTTP 200) et 0,262 (essai 02, HTTP 304) | S1 |
| 2 | QEMU TCG x86, bootstrapper seul | Debian 6.12.107+deb13-amd64 sur QEMU TCG `-cpu max`, 2 vCPU | même Proton-CachyOS (Wine 11.0), root Steam Runtime 4, EAC Heroic v2 | non | F0 | non applicable | 3,502 | S2 |
| 3 | QEMU TCG x86, jeu complet | même version de noyau (6.12.107+deb13-amd64, `xorg.log` dans `serial.log`), UTM 5.0.5 (QEMU 10.0.12), `-cpu max`, 4 vCPU, 8 GiB, Neptune | idem, `Launcher.exe` via plan d'authentification | oui | F0 (sept lancements authentifiés le 30 sept., de 19:04 à 21:15 UTC, tous avec `ROCKET_EAC_SUCCESSFULLY_LOADED_IN_GAME=1`) | oui une fois : 91,54 s (233,73 à 325,27), sortie volontaire ; les six autres s'arrêtent pour des raisons étrangères à EAC | EAC actif à 144,93 (dernier run) | S3, S3b |
| 4 | QEMU TCG x86, `lo` éteinte (premiers essais) | idem | idem | non | F5 (505, cURL 6) | non | non relevé | S4 |
| 5 | Windows natif (PC x86) | Windows, module EAC Windows | non applicable | oui | 301 Windows | oui, 15,84 s visibles | non applicable | S16 |
| 6 | WSL2 (Hyper-V), x86 | 6.18.33.2-microsoft-standard-WSL2 | WineHQ 10.0.0.0~noble-1, Ubuntu 24.04, ancien module | non | F3 | non | 0,019 (502), 0,188 (208) | S5 |
| 7 | OrbStack, Rosetta, Debian Wine 10, ancien module (28 sept.) | 7.0.14-orbstack (ARM) + Rosetta | Wine 10.0~repack-6, sysroot Debian trixie | non | F4 | non | 36,838 à 37,539 | S6 |
| 8 | idem, nouveau module probable (29 sept.) | idem | idem, avec bibliothèque d'audit PLT | non | F4 | non | 0,077 et 0,080 | S6b |
| 9 | OrbStack, Rosetta, Wine lancé comme PID 1, ancien module (28 sept.) | idem | Wine 10.0 Debian | non | F2 | non | 0,039 | S7 |
| 10 | OrbStack, Rosetta, GE-Proton11-7 (Wine 11) | idem | GE-Proton11-7 direct puis via UMU et Steam Runtime 4, ancien module | non | F4 | non | 79,287 (direct), 77,669 (UMU) | S8 |
| 11 | OrbStack, Rosetta, Proton-CachyOS 1784681208 via UMU, avec et sans `PROTON_EAC_RUNTIME` | idem | même runner que Bazzite, nouveau module | non | F4 dans les deux cas | non | observation 30,303 et 30,927 (préparation UMU incluse) | S9 |
| 12 | OrbStack, FEX ARM Linux non modifié, ptrace invité de FEX désactivé (`FEX_GUEST_PTRACE` non posée) | noyau ARM 7.0.14 réel, FEX ARM64 | Cachy Wine (copie readonly-section), `PROTON_EAC_RUNTIME` posée, rootfs x86 de FEX, conteneur `--platform linux/arm64` | non | F4 (deux essais) | non | 0,074 et 2,618 | S14 |
| 13 | HyperLinux + Rosetta (28 sept. au 1er oct.) | syscalls Linux sur Darwin, Hypervisor.framework par processus, Rosetta | Wine 10.0~repack-6 puis Cachy Wine + steamrt4 | non | F1, puis F2, puis F4 | non | F2 : 0,027 à 0,035 ; F4 : 0,731 à 0,914 (essais du 29 et 30 sept.) ; avec Cachy Wine le 1er oct., 37,3 à 42,3 observés depuis le lancement d'EAC, non ramenés au mapping | S10 |
| 14 | HyperLinux + FEX natif (1er au 2 oct.) | idem avec FEX ARM64 d'origine (SHA-256 `637943d0...`), puis un clone de diagnostic numérique (`670825e8...`) ; ptrace invité désactivé jusqu'au run `real-parent` inclus, activé à partir du 2 oct. | Cachy Wine, x86 sysroot | non | F1 (0,011 à 0,012), puis F2 (0,061), puis F4 (3,432 à 7,024) | non | voir colonne verdict | S11 |
| 15 | HyperLinux + FEX natif, plan authentifié (`Launcher.exe`) | idem | idem | oui | launcher 208, erreur GUI 256, aucun libellé VM | non | 393,734 (total) | S15 |
| 16 | Linux ARM 6.12.111 sous QEMU HVF + FEX + Wine | `-accel hvf -cpu host -machine virt`, pages 4 KiB | Wine via FEX, racine x86 de l'image Neptune (inféré) | oui | F3 (208) | non | totaux 94,5 ; 94,7 ; 245,2 | S12 |
| 17 | Voile, Wine Darwin 11.0 | macOS, D3DMetal | Wine 11.0 de Voile | non | F1 (206) | non | 3,89 et 4,91 (totaux) | S13 |

Remarques sur les cellules.

- Ligne 3 : la durée « 91,54 » est l'écart entre `protected_session_start_elapsed_s` et `protected_session_end_elapsed_s` de `validation.json`. « EAC actif à 144,93 » est `eac_enabled_elapsed_s`, mesuré depuis le début de l'observateur, pas depuis le début du mapping.
- Ligne 7 et 8 : le délai passe de 37 s à 0,08 s sur la même pile OrbStack entre le 28 et le 29 septembre. Le CDN d'EAC a changé de module le 29 septembre à 11:37:15Z (S6c). Le module de la ligne 8 n'est pas haché, d'où « probable ».
- Ligne 13 : la progression F1 vers F2 puis F4 est dans l'ordre chronologique (CHR:142, 146, 182-184, 195).
- Ligne 14 : F1 = trois essais (19:02, 22:04, 22:45 UTC le 1er oct.), F2 = un essai (23:22), F4 = trois essais (23:45, 00:46 et 01:57).
- Ligne 16 : le libellé VM est attesté par `FEXN/README.md:170` et par une recherche de fichiers (sans affichage du contenu) qui désigne le journal EAC du troisième essai (`arm-game-20260930T233737Z`). Les deux premiers essais (codes de sortie du lanceur 255 et 195) n'ont pas de verdict EAC lisible dans les fichiers publics.

### 1.3 Sources de la table

- S1 : `DIAG/rocket-bazzite-return-20260929/ANALYSE.md:17-21` (essais, session t=80,53 à 153,35), `:31-33` (hôte et runner), `:39` (charge utile) ; `CHR:276` ; journaux reçus `DIAG/rocket-bazzite-return-20260929/received/journaux/essai-01/eac-bootstrapper.log` (mapping 11:51:52.026, 301 à 11:51:52.546) et `essai-02/eac-bootstrapper.log` (11:54:03.610 et 11:54:03.872).
- S2 : `P/qemu-fullsystem-eac-20260930/run-20260930T161531680277Z-eac/eac-journal-extracted.txt:149` (mapping 16:15:50.085), `:155` (301 à 16:15:53.587), `eac-evidence.json` (`launcher_301`, `bootstrapper_reports_module_initialized`, `executable_memfd_observed`) ; `result.json` (`VM_KERNEL=6.12.107+deb13-amd64`, `-accel tcg -cpu max -machine pc -m 4096M -smp 2`).
- S3 : `P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/validation.json` ; `serial.log:893-902` ; `config.plist` (CPU `max`, 4 cœurs, 8192 MiB, TCG) ; `P/qemu-neptune-graphics-20260930/version.log` (QEMU 10.0.12) ; `PERF/baseline-20260930T214616Z/manifest.json` (empreintes critiques, `online_validation`).
- S3b : `serial.log` (marqueur `ROCKET_EAC_SUCCESSFULLY_LOADED_IN_GAME=1`) de `P/qemu-neptune-graphics-20260930/authenticated-game-crash-20260930T190429Z`, `corrected-client-game-attempt-20260930T192209Z`, `gui-decoder-fatal-attempt-20260930T194508Z`, `eac-game-title-auth-expired-20260930T200739Z`, `cached-9p-auth-expired-20260930T202603Z`, `block-disk-auth-success-render-stall-20260930T205551Z` et `game-query-fix-20260930T211549Z` ; causes d'arrêt des six premiers dans leurs `finding.json` quand ils existent (expiration du code d'échange `EOS_Auth_ExchangeCodeNotFound`, blocage du rendu) ou dans `serial.log` (`ROCKET_GAME_PROCESS_GONE=1`).
- S4 : `P/qemu-fullsystem-eac-20260930/static-dns-loopback-path.md` (analyse statique et premier essai) ; `eac-evidence.json` (`loopback_down_reproduced`, `loopback_up_control_passed`) ; `guest-run.sh:38,69`.
- S5 : `DIAG/rocket-wsl-eac-return-20260929/ANALYSE.md:3,23-27,37-38`.
- S6 : `LR/eac-static-210/analysis.md:7` (12:58:10.848 à 12:58:47.922), `:24` (13:07:06.530 à 13:07:43.415) ; `LR/seal-comparison/README.md:11` (37,539 et 36,838) ; `LR/eac-linux-environment-inventory.md` (Docker 29.4.0, noyau 7.0.14-orbstack, `/mnt/rv/[rosetta]`).
- S6b : `P/eac-ptrace-audit-linux-20260929/run-20260929T142857833442Z/anticheatlauncher-0.log` (14:29:02.522 à 14:29:02.599) et `P/eac-ptrace-audit-regset-linux-20260929/run-20260929T144112227623Z/anticheatlauncher-0.log` (14:41:16.423 à 14:41:16.503) ; `CHR:332,347`.
- S6c : `P/eac-current-payload-20260929/README.md` (premier paragraphe : 13 825 695 octets, `Last-Modified` 11:37:15 UTC le 29 septembre) ; ancien module 9 681 573 octets, SHA-256 `ce28ef66...` (`LR/README.md`).
- S7 : `LR/pid1-control/README.md:12-15,25-27`.
- S8 : `LR/eac-ge-120s-analysis.md:9-13` ; `CHR:164` ; `LR/umu/ab-diagnosis.md`.
- S9 : `P/eac-heroic-bridge-ab-20260929/RESULTATS-HEROIC.md` (tableau et durées).
- S10 : `CHR:124,142,146,182-184,195,213,219,229-233,241,278,505-507` ; `P/eac-current-payload-20260929/README.md` ; `P/eac-remote-poke-20261001/run-20261001T061958296093Z/result.json` (Cachy Wine direct, 37,33 s observés), `P/eac-setregset-diff-20261001/run-20261001T063442877259Z/result.json` (42,30 s), `P/eac-jit-invalidation-20261001/run-20261001T084938884685Z/result.json` (38,71 s).
- S11 : `FEXN/README.md:3,11,317,321,333-335` ; journaux propres au run : `FEXN/eac-fex-native-<run>/prefix/drive_c/users/steamuser/AppData/Roaming/EasyAntiCheat/e6bcca5b37d0457ca881aec508205542/da32ae9c12ae40e8a112c52e1f17f3ba/anticheatlauncher.log` pour `anonymous-shared-20261001T185648813375Z`, `proc-root-alias-20261001T215909862880Z`, `libc-maps-diagnostic-20261001T223956127723Z`, `topdown-sparse-diagnostic-20261001T231755792951Z`, `real-parent-20261001T234249771714Z`, `arm-remote-regset-20261002T004602387151Z`, `traceexec-20261002T015655495806Z`. Les copies trouvées sous `inherited-private-logs/` sont des journaux hérités, à ne pas confondre avec les journaux frais.
- S12 : `PERF/arm-game-20260930T233737Z/result.json` (`eac_loaded: False`, `eac_initialized: False`, `wall_s` 245,234, QEMU `-accel hvf -cpu host -machine virt -smp 4 -m 8G`) ; `PERF/arm-game-20260930T232623Z/result.json` et `PERF/arm-game-20260930T233320Z/result.json` (94,47 et 94,68) ; `FEXN/README.md:170` ; `PERF/arm-hvf-witness-20260930T230613Z/result.json` (witnesses FEX : CPU 0, ptrace 199/199, point d'arrêt 44/44 ; les quatre essais antérieurs sortent en 11).
- S13 : `CHR:20-27`.
- S14 : `FEXN/eac-linux-fex-mapping-reference-20261001T191700585805Z/result.json` et `...T200811099559Z/result.json` (`launcher_codes [210]`, `callback_codes [509]`, `vm_rejection_literal false`) ; journaux sous `prefix/...` : 19:17:12.837 à 19:17:12.911 et 20:08:24.217 à 20:08:26.835.
- S15 : `FEXN/eac-fex-native-epic-fork-fd-backpressure-20261002T073702835631Z/public-terminal-summary.json` (`launcher_codes [208]`, `original_eac_gui_error 256`, `eac_vm_rejection_literal false`).
- S16 : `DIAG/rocket-x86-return-20260929/ANALYSE.md` (Windows : 301, session visible 15,84 s ; WSL2 : sonde GETREGSET 13/13).

### 1.4 Lecture de la matrice

- **Mesuré.** Toutes les lignes F4 partagent un hôte ARM64 qui traduit du x86-64 (Rosetta ou FEX) ; toutes les lignes F0 partagent un noyau x86-64 sur CPU x86 réel ou émulé complet.
- **Mesuré.** Le 301 sous QEMU est reproductible : huit lancements sur huit le 30 septembre (un non authentifié, sept authentifiés). Les arrêts de ces journées ont d'autres causes que EAC : code d'échange expiré, blocage de rendu, disparition du processus du jeu (S3b). Un seul de ces runs a produit une session protégée.
- **Mesuré.** Le délai du 509 dépend de la version du module, pas de la pile : 37 s à 79 s avec l'ancien module sur OrbStack, 0,08 s avec le nouveau sur la même pile (S6, S6b, S8). Un module qui échoue en 0,08 s sur un CPU rapide et réussit en 3,5 s sur un CPU émulé en logiciel (TCG) montre que le verdict ne tient pas à un dépassement de délai.
- **Mesuré.** La ligne 2 donne 301 sans authentification. Toutes les lignes F4 sont aussi sans authentification. **Inféré.** L'authentification Epic n'est donc pas ce qui sépare 301 de 509 pour l'initialisation du module. Le contrôle Bazzite sans authentification (consigne `docs/research/rocket-bazzite-unauthenticated-control-prompt.md`) n'a jamais reçu de retour dans `DIAG/` ; il est devenu redondant avec la ligne 2.
- **Inféré.** Les lignes 13 à 15 mélangent des états du runtime qui ont changé entre chaque essai. Elles ne se lisent pas comme un runtime unique qui aurait passé tous les contrôles (consigne de `TECH:45`).

## 2. Builds Wine, rootfs, variables d'environnement, lancement, delta

### 2.1 Builds Wine

| Build | Où | Version et identité | Résultats |
| --- | --- | --- | --- |
| Debian Wine 10 | OrbStack (lignes 7 à 9), HyperLinux + Rosetta (ligne 13) | paquets Debian trixie `wine`, `wine64`, `wine64-preloader`, `libwine` 10.0~repack-6 ; sysroot de 235 paquets avec `libc6 2.41-12+deb13u4` (`B/linux-root/README.md`, `B/linux-root/lock.json`) | F4 sur OrbStack, F1 puis F2 puis F4 sur HyperLinux |
| GE-Proton11-7 | OrbStack (ligne 10) | Wine 11, `WINEARCH=wow64`, lancé directement puis via UMU 1.4.4 et Steam Runtime 4 (`LR/eac-linux-environment-inventory.md`, `LR/umu/ab-diagnosis.md`) | F4 à 77,669 et 79,287 s (ancien module) |
| Proton-CachyOS 11.0 | Bazzite (ligne 1), QEMU (lignes 2 à 4), OrbStack (ligne 11), HyperLinux (lignes 13, 14), FEX sur OrbStack (ligne 12) | `1784681208 cachyos-11.0-20260703-slr`, Wine 11.0 ; copie « readonly-section » du dépôt avec ntdll `e6ad055d8bf9542c3ac8ad520c701c78c3de7b3df40c05a6d3e71a6075916b41` (`P/qemu-fullsystem-eac-20260930/staging-manifest.json`, clé `cachy_ntdll_sha256`) ; archive officielle vérifiée `62ff4b27...` (`P/bazzite-cachyos-runtime-20260929/provenance.json`) | F0 sur x86, F4 sur ARM |
| WineHQ 10.0.0.0~noble-1 | WSL2 (ligne 6) | paquets WineHQ sous Ubuntu 24.04 ; pas le même ensemble que Debian 10.0~repack-6 | F3 |
| Wine 11.0 de Voile | macOS (ligne 17) | CrossOver-based, Mach-O, pas d'ABI Linux | F1 (206) |

Le chemin `/opt/ge-proton` dans la VM QEMU est un nom historique : son contenu est le Proton-CachyOS ci-dessus (`/opt/ge-proton/version` = `1784681208 cachyos-11.0-20260703-slr`, journal du bootstrapper `Wine version: 11.0`).

### 2.2 Composition des racines et des noyaux

- **QEMU (lignes 2 à 4).** Noyau Debian `6.12.107+deb13-amd64` (SHA-256 `2b2358b37674d2505350528875bb17afae2a36522a9e8a9417eaca65a7da0e08`, netboot officiel Debian trixie ; `P/qemu-x86-regset-control-20260929/README.md`), initramfs maison (`init.c`), racine `root.ext4` en lecture seule construite depuis `root-stage` (`/etc/os-release` : « Steam Runtime 4 »), `/opt/ge-proton` (Proton-CachyOS), `/opt/heroic-eac-runtime` (composant v2, six fichiers identiques à Bazzite), `/x11` (Xvfb et xkbcomp), `/game` (sous-ensemble Win64 de 78 Mio pour le bootstrapper seul ; jeu complet en ext4 lecture seule pour la session protégée), préfixe `prefix.ext4` copié de `P/eac-heroic-bridge-ab-20260929/results-cachyos-current-001/B/prefix` (`staging-manifest.json`, `source_paths`).
- **OrbStack (lignes 7 à 12).** Docker 29.4.0 sur noyau ARM `7.0.14-orbstack-00380-ga7e0a2dc9535`, conteneur `--platform linux/amd64 --user 501:20` sur l'image Debian `sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c`, `/usr/lib`, `/usr/share/wine` etc. montés en lecture seule depuis le sysroot Debian du dépôt, seccomp Docker par défaut, aucune capacité ajoutée (`LR/eac.json`, `LR/eac-linux-environment-inventory.md`). Rosetta : `/mnt/rv/[rosetta]` dans les maps.
- **HyperLinux (lignes 13 à 15).** Pas de noyau Linux invité : un petit shim EL1 sous Hypervisor.framework par processus traduit les syscalls Linux (HEAD amont `8048a96...`, patches privés). Racine Debian (ligne 13) puis arbre Steam Runtime 4 restauré (`restore-steamrt-tree.py`) pour Cachy Wine. Ligne 14 : FEX ARM64 et FEXServer non modifiés (SHA-256 FEX `637943d0185ca3774cbc261c6c7152be2c54758a71e237769d5a27a2dea3b33f`, FEXServer `89e286c41c844ad596ed714a4449e895d9f00bda3cacfbf991f3df2a56d8d0b3`), sysroot x86 avec `ld-linux-x86-64.so.2` (`FEXN/handoff-native-fex-20261002-traceexec.md`).
- **Linux ARM sous HVF (ligne 16).** Noyau Debian arm64 `6.12.111+deb13-arm64`, pages de 4 KiB (`PERF/arm-hvf-witness-20260930T230613Z/serial.log`), `arm-root.ext4`, image x86 `native-display-root.qcow2` de la base QEMU montée sous `/mnt/x86root` (**inféré** d'après les étapes `/mnt/x86root/*` du `serial.log` et de la ligne de commande de `result.json`), FEX privé avec ptrace invité.
- **WSL2 (ligne 6).** Ubuntu 24.04.5, runner Python vivant comme parent de Wine, montage du jeu en EROFS lecture seule (`DIAG/rocket-wsl-eac-return-20260929/ANALYSE.md:15`).

### 2.3 Variables d'environnement pertinentes

QEMU, bootstrapper seul (`P/qemu-fullsystem-eac-20260930/guest-run.sh:99-112`) :

```
WINEARCH=win64
WINESERVER=/opt/ge-proton/files/bin/wineserver
WINELOADER=/opt/ge-proton/files/bin/wine
WINEDLLPATH=/opt/ge-proton/files/lib/wine
LD_LIBRARY_PATH=/opt/ge-proton/files/lib:/opt/ge-proton/files/lib/wine/x86_64-unix:/usr/lib/x86_64-linux-gnu:/usr/lib
WINEESYNC=0 WINEFSYNC=0 WINE_DISABLE_FAST_SYNC=1
PROTON_EAC_RUNTIME=/opt/heroic-eac-runtime
WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d'
WINEDEBUG='err+all,warn+winstation,fixme-all'
DISPLAY=:89 XAUTHORITY=/xauth XDG_RUNTIME_DIR=/run/user/501 XDG_CACHE_HOME=/tmp/cache
HOME=/home/test USER=wine LOGNAME=wine LANG=C.UTF-8
WINEPREFIX=/prefix
```

QEMU, session protégée (`PERF/baseline-20260930T214616Z/guest-game.sh:21-33`) : `WINEPREFIX=/prefix`, `PROTON_EAC_RUNTIME=/opt/heroic-eac-runtime`, `WINEFSYNC=1` (futex_waitv du noyau invité), `XDG_CACHE_HOME=/tmp/cache-wine`, `WINEDEBUG='err+all,warn+module,fixme-all,+seh'`, `FONTCONFIG_FILE` et `FONTCONFIG_PATH` vers `/run/graphics`. Les DLL `d3d11.dll`, `dxgi.dll`, `libwinpthread-1.dll` et `nptunix.dll` du client Neptune sont copiées dans `system32` (sans effet attendu sur EAC).

OrbStack, Debian Wine 10 (`LR/run-inside.sh:3-14`) : `HOME=/reference WINEPREFIX=/reference/prefix WINEARCH=win64 WINELOADER=/usr/lib/wine/wine64 WINESERVER=/usr/lib/wine/wineserver64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG='err+all,fixme-all' DISPLAY=:90`, `LD_LIBRARY_PATH` Debian ; variante GE : `WINEPREFIX=/reference/prefix-ge WINEARCH=wow64 WINELOADER=/opt/ge-proton/files/bin/wine`. `PROTON_EAC_RUNTIME` n'est pas posée dans ces essais du 28 septembre.

HyperLinux natif avec FEX (profil du 2 octobre, `FEXN/handoff-native-fex-20261002-traceexec.md`) : `HL_MMAP_TOPDOWN=1`, `HL_PROC_SELF_EXEC=1`, `HL_SYSROOT_NO_BASENAME_FALLBACK=1`, accès distant MAPS/PEEK/POKE actif, `HL_PROC_REMOTE_SETREGSET=1`, `FEX_GUEST_PTRACE=1` pour les contrôles possédés ; côté Wine `WINELOADERNOEXEC=1`, `LD_BIND_NOW=1` (`FEXN/README.md:88-93`). Un seul run à la fois dans l'espace de noms privé.

### 2.4 Forme des commandes de lancement d'EAC

Sans credential, les deux formes sont :

1. Bootstrapper seul, non authentifié (lignes 2, 4, 6 à 14) : `<wine> Z:\game\Binaries\Win64\RocketLeague_EAC.exe --verbose`, répertoire courant `Binaries/Win64`, utilisateur non root (UID 501 sous QEMU et OrbStack), un parent réel vivant sauf dans le contrôle PID 1. QEMU : `setpriv --reuid=501 --regid=501 --clear-groups timeout 600s "$WINELOADER" 'Z:\game\Binaries\Win64\RocketLeague_EAC.exe' --verbose` (`guest-run.sh:276-278`). OrbStack : `timeout -k 3 <durée> "$WINELOADER" 'Z:\game\Binaries\Win64\RocketLeague_EAC.exe' --verbose` (`LR/run-inside.sh:53-55`).
2. Plan authentifié (lignes 3, 15, 16) : `<wine> Z:\...\Binaries\Win64\Launcher.exe <arguments fournis par un courtier côté hôte> [-epicovt=Z:<fichier temporaire contenant le jeton de propriété>] -windowed -ResX=800 -ResY=600` (`PERF/baseline-20260930T214616Z/guest-launch.py:35-36,49` ; `serial.log:893` : `ROCKET_PUBLISHER_EXECUTABLE=Binaries/Win64/Launcher.exe`). Le plan est reçu par un petit serveur HTTP local ; aucun secret n'est copié dans ce rapport ni lu pour l'écrire.

Le bootstrapper est lancé avec `--verbose` pour émettre les lignes `[EAC Callback] Code: N` (`LR/eac-static-210/analysis.md:24`) ; sans cette option, les journaux contiennent le résultat du launcher mais pas le callback (cas du plan authentifié, ligne 15).

### 2.5 Delta exact entre le succès QEMU et les échecs

La paire la plus serrée à tous égards sauf la pile noyau et CPU :

| Facteur | QEMU, ligne 2 (301) | HyperLinux + Rosetta, ligne 13 (509), essais du 1er oct. | Identique ? |
| --- | --- | --- | --- |
| Wine | Proton-CachyOS 1784681208, ntdll `e6ad055d...` | idem (`cachy_selection.ntdll_sha256` dans `P/eac-remote-poke-20261001/run-20261001T061958296093Z/result.json`) | oui |
| Bibliothèques | Steam Runtime 4 | « Direct Cachy Wine with Steamrt4 libraries; no UMU or pressure-vessel » (même `result.json`) | oui (même famille) |
| Préfixe d'origine | `.../eac-heroic-bridge-ab-20260929/results-cachyos-current-001/B/prefix` | `prefix_origin` identique | oui |
| Composant EAC Heroic v2 | présent, `PROTON_EAC_RUNTIME` posée | hash enregistrés (`heroic_before`) | oui |
| Synchronisation | `WINEESYNC=0 WINEFSYNC=0 WINE_DISABLE_FAST_SYNC=1` | `sync_overrides` identique | oui |
| Quatre binaires du jeu | `903e7aba...`, `ca3a9075...`, `40d1bb2d...`, `ea35d2ef...` | mêmes empreintes avant et après | oui |
| Charge utile EAC | 13 825 695 octets | 13 825 695 octets | oui (taille et HTTP 200 ; le corps du bootstrapper n'est pas haché dans ces runs) |
| Authentification | aucune | aucune | oui |
| Parent de Wine | `timeout`, shell d'initialisation | parent réel (PID > 1) | oui |
| Boucle locale | `lo` activée (`guest-run.sh:69`) | fonctionnelle dans ces essais (HTTP 200 et module chargé) ; un défaut antérieur d'AFD_CONNECT lié au refus de `tgkill` entre processus est documenté (`P/cachyos-loopback-controls-20260930/README.md`) | oui à la date des essais |
| **Noyau** | Linux x86-64 6.12.107 réel | couche de syscalls HyperLinux sur Darwin | **non** |
| **CPU** | x86-64 émulé complet (TCG `-cpu max`) | CPU ARM Apple + Rosetta | **non** |

**Inféré.** Pour le verdict d'initialisation non authentifié, le facteur discriminant est la pile noyau et CPU. La ligne 12 (noyau ARM Linux réel, FEX non modifié, 509 en 0,074 s et 2,618 s) retire en plus la couche de syscalls HyperLinux de la liste des suspects pour le 509 : les faits Linux (procfs, memfd, scellement, ptrace natif ARM) y sont ceux d'un vrai noyau. Restent les faits propres à l'architecture x86 que le traducteur doit reproduire : vue ptrace x86, CPUID, feuilles `cpuinfo`, vecteurs auxiliaires, trames de signaux.

**Différences connues non neutralisées entre la paire.** Racine QEMU en lecture seule et initramfs maison contre sysroot HyperLinux monté ; pages de 4 KiB dans l'invité contre pages de 16 KiB de l'hôte ; Xvfb dans l'invité contre Xvfb sur macOS. Aucune ne s'est montrée liée au verdict.

## 3. Hypothèses vivantes et ce que chaque contrôle a exclu

Statuts : **exclue**, **close pour un code précis**, **vivante**, **non testée**. La colonne « Expérience restante » indique si une expérience discriminante reste à lancer.

### 3.1 Les sept hypothèses demandées

| # | Hypothèse | Contrôles et mesures | Statut | Expérience restante |
| --- | --- | --- | --- | --- |
| H1 | Libc ARM et x86 mélangées dans `/proc/self/maps` | Résolveur statique : première ligne dont le chemin contient `/libc-` ou `libc.so`, adresse = début de plage + offset fichier, puis ELF `e_machine=62`, `ET_DYN`, `e_phentsize=56` (`LR/eac-static-210/maps-resolver.md`). Sous FEX natif, libc ARM listée en premier (machine 183, adresse 8 593 539 072) contre libc x86 plus haut (19 105 316 864) : 507/206 en 0,011 s (`FEXN/libc-maps-diagnostic-runtime/README.md`, `FEXN/proc-libc-layout-control/qualified-order-difference.json`). Une allocation descendante place la libc x86 d'abord : 507 devient 502/208 (`FEXN/topdown-sparse-diagnostic-runtime/eac-result-summary.json`, comparaison `prior_callback 507` puis `current_callback 502`). FEX sur Linux ARM réel liste la libc x86 d'abord (adresse 140 737 350 791 168). | Close pour F1, exclue pour F4 : avec la libc x86 en premier et la libc ARM toujours présente plus loin, le 509 apparaît (`FEXN/real-parent-control/eac-result-summary.json`, `first_machine 62`, `x86_headers 1`, `arm_headers 1`) | non, mais garder la vue invitée sans objet ARM |
| H2 | Vues ptrace traduites ou infidèles | Rosetta renvoie 272 octets ARM64 pour NT_PRSTATUS, une demande de 216 en donne le préfixe X0..X26 (CHR:176, 313, 345). Sur OrbStack, l'audit PLT voit 68 appels `ptrace` d'EAC, tous à 0 : TRACEME 1, SETOPTIONS 1, GETREGSET 66 (CHR:332 ; `P/eac-ptrace-audit-regset-linux-20260929/README.md`). L'audit ne voit pas les requêtes émises par instruction `syscall` directe : aucun `PTRACE_SYSCALL` n'y apparaît alors qu'il est observé sous HyperLinux (CHR:229). Les 66 arrêts lus montrent, en X8 (numéro de syscall ARM64), 51 `mmap`, 1 `memfd_create`, 1 `write` de 7 757 608 octets (0x765f28), 2 `fcntl`, 1 `openat`, 2 `fstat`, 2 `lseek`, 2 `read`, 2 `close`, 1 `munmap` et 1 `exit_group` avec l'argument 1 (`P/eac-ptrace-audit-regset-linux-20260929/run-20260929T144112227623Z/regset-memory-report.json`, clé `word8_values` ; `P/eac-ptrace-target-role-20260929.md`, CHR:345). Sous HyperLinux le même enfant lit en plus `RocketLeague.exe` avant `exit_group(1)` (CHR:241, 536). Sous HyperLinux avec vue x86 par RSP, trois contextes x86 corrects (RAX -38 à l'entrée, RCX = RIP, R11 = EFLAGS) : 509 inchangé (CHR:505-507). Sous HyperLinux + Rosetta (runtime diagnostique), EAC émet aussi un SETREGSET de 216 octets où un seul mot diffère, RIP moins 1 (`P/eac-setregset-diff-20261001/run-20261001T063442877259Z/setregset-diff-summary.json`), et interroge `newfstatat("/proc/<enfant>/maps")`, qui retournait ENOENT avant correction (`P/eac-remote-maps-finding-20260930/README.md`). Sous FEX natif : TRACEME réussit, `SETOPTIONS(0x11)` retourne -95 (`FEXN/README.md:333-335`). FEX avec ptrace invité désactivé sur noyau ARM réel (EPERM pour tout sauf six requêtes, `P/fex-eac-feasibility.md`) : 509 (ligne 12). Les témoins possédés sous FEX passent 199/199 et 44/44 (S12). | Vivante. Aucun essai ne combine une vue x86 fidèle complète et tout le reste du contrat. Sur OrbStack aucun refus du noyau n'est observé et l'enfant sort de lui-même avec 1 : la vue de registres ne peut agir que par le parent (décision de tuer l'enfant ou de rapporter l'échec) ou par un échange non observé. Le motif de ce `exit_group(1)` de l'enfant est inconnu. | **oui** (X1, X2 et X6, section 3.3) |
| H3 | Identité CPU et `cpuid` | FEX (source épinglée `0df84d38...`) met par défaut le bit hyperviseur à 1 sauf `HideHypervisorBit()` et répond `FEXIFEXIEMU` à la feuille `0x40000000` (`FEXCore/Source/Interface/Core/CPUID.cpp:451-452,984` dans `P/fex-implementation-feasibility-20260929/source/FEX-0df84d3844bcdb87bb7d3f5b8fb0959cd009c038`). Aucun essai n'a modifié le CPUID (« FEX identity and CPUID remain unchanged », `FEXN/README.md:63`). Sous HyperLinux + FEX, aucun libellé VM (`vm_rejection_observed false`, `FEXN/eac-fex-native-first-mapping-result-20261001.json`). WSL2 et Linux ARM sous HVF + FEX reçoivent le libellé VM, QEMU TCG et OrbStack non. La feuille 1 ECX bit 31 et la feuille hyperviseur de QEMU TCG ne sont pas mesurées. | Vivante, partiellement exclue pour le seul libellé VM : le CPUID par défaut de FEX n'a pas déclenché F3 sous HyperLinux. Rien ne dit quelles feuilles EAC interroge. | **oui** (X3) |
| H4 | Temporisation | Délais 0,011 à 7,024 s sur FEX, 0,07 à 0,91 s sur Rosetta avec le nouveau module, 36,9 à 79,3 s avec l'ancien module, 3,50 s pour le succès TCG. Un fil actif à 100 % pendant 79 s sur OrbStack/GE (`LR/eac-ge-120s-analysis.md`, section fils). | Exclue comme cause globale : le succès a lieu sur un CPU émulé en logiciel et le délai du 509 suit la version du module. Les contrôles fins (rdtsc, horloges) ne sont pas testés. | non (faible valeur) |
| H5 | Acceptation côté serveur Epic | Le verdict 301 vs 509 se produit avant toute session de compte et sans authentification (lignes 2 et 11 à 14). Les deux environnements F0 ont obtenu une session protégée authentifiée (lignes 1 et 3). Les piles ARM n'ont jamais initialisé le module : le serveur n'a pas été exercé. Plan authentifié sous FEX natif : launcher 208 sans que le jeu ni le matchmaking soient atteints (S15). | Non testée pour les piles ARM, sans lien avec les rejets observés | non, avant l'initialisation du module |
| H6 | Configuration produit | Mêmes `ProductId e6bcca5b...`, `SandboxId 9773aa1a...`, `DeploymentId da32ae9c...` dans tous les journaux (`P/qemu-fullsystem-eac-20260930/run-20260930T161531680277Z-eac/eac-journal-extracted.txt:11,13,15` ; `LR/eac-bootstrapper-first.log:6-8`) ; module Linux actif pour le produit : CDN `.../linux64` en HTTP 200 partout, sessions protégées sur Bazzite et QEMU. | Exclue | non |
| H7 | Version de Wine | Debian Wine 10.0, GE-Proton11-7, Proton-CachyOS 11.0 donnent tous 509 sur OrbStack/Rosetta ; Proton-CachyOS 11.0 donne 301 sur Bazzite et sur QEMU. | Exclue comme discriminant. Cachy sous OrbStack passe par UMU, pas Cachy direct, mais Cachy direct donne aussi 509 sous HyperLinux. | non |

### 3.2 Hypothèses supplémentaires

| # | Hypothèse | Mesures | Statut | Expérience restante |
| --- | --- | --- | --- | --- |
| H8 | Wine lancé comme PID 1, ou métadonnées du parent absentes | `pid1-control` sur noyau réel : 502/208 « Launch Error: 256 » en 0,039 s (`LR/pid1-control/README.md:12-15,25-27`). EAC lit `getppid`, `/proc/<ppid>/comm`, `exe`, `cmdline`, `status` et ouvre `/tmp/eac.flag` (`P/eac-parent-proc-comparison.md`). HyperLinux : 502/208 devient 509/210 quand ces entrées existent (CHR:195) ; FEX natif : PID 1 donne 502/208 (0,061 s), un parent réel PID 2 donne 509/210 (`FEXN/real-parent-control/eac-result-summary.json`, `comparison`). | Close pour les essais non authentifiés : PID 1 suffit à produire F2 sur un noyau réel, et un parent réel supprime F2 dans les deux runtimes HyperLinux. Le F2 du plan authentifié (ligne 15) n'est pas expliqué. | non, garde-fou de régression |
| H9 | VM explicite | Libellé F3 : WSL2 en 0,019 s ; Linux ARM sous HVF avec FEX (S12). Absent : QEMU TCG, OrbStack, HyperLinux. La doc Epic dit que l'interface client ne prend pas en charge les VM (section 5). HyperLinux a vu `ENOENT` sur des chemins DMI et PCI sans recevoir F3 (CHR:197). | Vivante : signaux utilisés inconnus | **oui** (X3, mesure sans modification) |
| H10 | Version du module EAC | Ancien module 9 681 573 octets (SHA-256 du fichier `ce28ef66...`, ELF décodé `c6bf6c7d...`), nouveau 13 825 695 octets (SHA-256 `74b6cab9...`, ELF décodé `1b9faa54...`), changement du CDN le 29 sept. à 11:37:15Z (S6c). 509 avant et après sur ARM ; 301 seulement avec le nouveau module, sur x86. Le délai de F4 passe de 37 s à 0,08 s. | Exclue comme discriminant du résultat, mais variable de dérive : Epic active des modules sans préavis | non ; épingler une copie |
| H11 | Boucle locale et DNS | QEMU : `lo` éteinte donne 505 avant tout `getaddrinfo` (paire de sockets locale sur 127.0.0.1 construite avant le thread DNS, `static-dns-loopback-path.md`). `lo` allumée donne 301. HyperLinux avait des connexions locales en échec (`P/eac-cachyos-dns-trace-20260929/README.md`) ; la chaîne documentée est un refus de `tgkill` entre processus, l'APC de complétion d'AFD_CONNECT n'arrive pas (`P/cachyos-loopback-controls-20260930/README.md`). Les essais du 1er octobre obtiennent HTTP 200 et chargent le module. | Close | non |
| H12 | Scellement mémoire (`F_ADD_SEALS`) | Paire autorisé/refusé sur OrbStack : 509 des deux côtés (37,539 et 36,838 s). Sous HyperLinux et FEX natif, EINVAL (errno 22). | Non conclusive : pas d'effet isolé, mais l'enfant fait le `fcntl` juste avant `exit_group(1)` | non, à inclure dans le contrat complet |
| H13 | Invalidation JIT et code modifié par ptrace | Candidat Rosetta SETREGSET/POKE : 74/76 contre 76/76 pour le noyau x86 de référence ; Rosetta réutilise une traduction périmée après restauration du code (`P/ptrace-remote-setregset-runtime-20261001/README.md`). Pour FEX : `InvalidateGuestCodeRange` à utiliser (`TECH:129`). | Spécifique à Rosetta ; à re-contrôler sous FEX | oui, pour FEX seulement |
| H14 | Conteneur, seccomp, espaces de noms | OrbStack utilise le seccomp Docker par défaut, aucune capacité ajoutée ; QEMU sans conteneur ; Bazzite avec pressure-vessel. Aucun `EPERM` EAC observé (`LR/eac-linux-environment-inventory.md`). | Non discriminante | non |

### 3.3 Expériences discriminantes restantes

Classées par rapport décroissant entre information obtenue et coût. Aucune ne masque une identité, n'altère EAC ou n'invente une réponse.

- **X1. Séquence ptrace et syscalls d'EAC dans un environnement accepté.** Rejouer la bibliothèque d'audit PLT déjà qualifiée (`P/eac-ptrace-audit-linux-20260929`, `P/eac-ptrace-audit-regset-linux-20260929`) dans le bootstrapper sous QEMU TCG (ou sous Bazzite). Sorties attendues : requêtes ptrace et valeurs, nombre de GETREGSET, présence de POKE et SETREGSET, statut de sortie de l'enfant (0 ou 1), sorties de `waitpid`. C'est la seule façon de savoir si l'enfant sort aussi avec 1 dans un environnement accepté. Contrainte : exige QEMU ou un PC Linux physique, hors périmètre de cet effort sans accord explicite.
- **X2. Contrat ptrace complet dans le moteur cible.** Réaliser `SETOPTIONS(0x11)` (événements exec compris), les arrêts SYSCALL, GETREGSET de 216 octets au format x86, PEEK/POKE avec invalidation du JIT, SETREGSET (RIP moins 1 à un INT3), `/proc/<enfant>/maps`, `F_ADD_SEALS`, puis relancer le bootstrapper non authentifié. Critère : 509 devient 301, ou le verdict change et pointe l'étape suivante. C'est le chantier en cours sous FEX natif (`FEXN/handoff-native-fex-20261002-traceexec.md`).
- **X3. Relevé des signaux VM et CPU, sans modification.** Une sonde possédée lit CPUID (feuille 1 ECX bit 31, feuilles `0x40000000`+, marque), `/proc/cpuinfo`, `/sys/class/dmi/id/*`, `/sys/bus/pci/devices/*/vendor`, `/sys/hypervisor`, `/proc/device-tree`, `uname`, `AT_HWCAP` et `AT_PLATFORM`. À exécuter sous QEMU TCG (accepté), Linux ARM sous HVF + FEX (refus VM), OrbStack (509), HyperLinux + FEX (509), WSL2 (refus VM ; exige le PC de l'utilisateur). Résultat : la liste des signaux qui séparent « refusé » de « non refusé ». Plus : compteur numérique dans le gestionnaire CPUID d'une copie privée de FEX pour enregistrer les feuilles interrogées par le module.
- **X4. Vérification procfs sur la pile cible.** Sonde possédée : première ligne libc avec offset 0 et en-tête machine 62, `/proc/<enfant>/maps` lisible par le traceur, `/proc/<ppid>/{comm,exe,cmdline,status}`, `readlinkat(/proc/self/exe)`, noms `/memfd:<nom> (deleted)`.
- **X5. Qualification Epic.** Après X2 seulement : plus de 10 minutes dans une session protégée puis jonction d'une autre session protégée (section 5).
- **X6. Motif du `exit_group(1)` de l'enfant, sur un noyau réel.** OrbStack avec Rosetta ou avec FEX donne le même 509 que HyperLinux, avec tous les faits Linux authentiques. Pour savoir ce que l'enfant compare avant de sortir, enregistrer sans rien modifier la suite de ses appels système (chemin du `openat`, longueur du `read`, offset du `lseek`, résultat de `F_ADD_SEALS`, hachage du contenu écrit dans le memfd) avec l'outil d'audit validé. La limite connue est que cet audit ne voit que les appels passant par le PLT. Cette expérience répond à « pourquoi 1 » indépendamment de la vue ptrace.

## 4. Ce que le succès QEMU implique pour un nouveau runtime

### 4.1 Contrat observable par le module

Liste des faits que le module a été vu consommer sur les piles ARM (où il progresse jusqu'au 509), complétée par les faits du noyau qui a donné le succès TCG. La session QEMU n'a pas été tracée : aucun élément n'est confirmé comme requis par une trace dans l'environnement accepté.

1. **Noyau.** QEMU expose un vrai Linux x86-64 `6.12.107` (`uname -m` = `x86_64`, `VM_MACHINE=x86_64`). Le noyau de la VM est celui qui fait aussi passer le témoin ptrace x86 : 199/199 contrôles, 152/152 comparaisons de champs, 216 octets exactement pour NT_PRSTATUS quelle que soit la longueur demandée (216 ou 272), `RAX = -ENOSYS` à l'entrée d'un syscall, `RCX = RIP`, `R11 = EFLAGS`, `orig_rax` conservé (`P/qemu-x86-regset-control-20260929/README.md`, `semantic-evaluation.json`).
2. **Appels système exercés par le chargeur de la bootstrapper** (lus dans le code, et observés jusqu'au chargement sur OrbStack) : `getpid`, ouverture et lecture de `/proc/<pid>/maps`, `memfd_create` avec le drapeau 2, `write` du module préparé, `dlopen` via `/proc/self/fd/N` puis `dlsym` (`LR/eac-linux-environment-inventory.md`, section « Syscalls actually supported »).
3. **Appels du module au chargement** (observés sur les piles ARM, pas sous QEMU) : fork de l'enfant auxiliaire (`clone` avec `0x1200011`), `PTRACE_TRACEME`, `SIGSTOP`, `SETOPTIONS(0x11)`, `PTRACE_SYSCALL`, GETREGSET de 216 octets, `waitpid` avec statut `0x857f` aux arrêts de syscall, `tgkill`, `kill SIGKILL`, `fcntl(F_ADD_SEALS, 0xf)`, `readlinkat(/proc/self/exe)` (37 octets), `newfstatat(/proc/<enfant>/maps)`, PEEK/POKE et SETREGSET avec RIP moins 1 à un INT3 (`P/eac-ptrace-target-role-20260929.md`, `P/eac-remote-maps-finding-20260930/README.md`, `P/eac-setregset-diff-20261001/...`).
4. **Réseau.** `127.0.0.1` en TCP (listen, connect, accept, send, recv) construit avant le thread DNS ; sans cela 505 (`static-dns-loopback-path.md`). Puis DNS, TLS et HTTP 200 vers `modules-cdn.eac-prod.on.epicgames.com`.
5. **Processus.** Wine non PID 1, avec un parent réel lisible dans `/proc`. Utilisateur non root. `/tmp/eac.flag` absent n'est pas bloquant (ENOENT dans tous les essais).

### 4.2 CPU

- **Mesuré.** Le modèle de CPU demandé à QEMU est `-cpu max` en TCG (`result.json`, `config.plist`, `run-graphics.py`).
- **Lu.** Selon la lecture du code QEMU 10.1 consignée dans `P/qemu-system-x86-witness.md`, ce modèle expose sous TCG SSE2, FXSR, XSAVE, AVX et d'autres extensions. Aucune sonde dans l'invité ne l'a vérifié. Les valeurs CPUID réelles de l'invité QEMU (bit hyperviseur, marque) ne sont pas relevées.
- **Inconnu.** Les feuilles CPUID que le module interroge. Aucun relevé.
- **Inféré.** Le bit hyperviseur de FEX n'a pas déclenché F3 sous HyperLinux, donc il n'est pas à lui seul une condition suffisante de F3. La condition suffisante n'est pas identifiée (H9). Un runtime Darwin avec FEXCore n'est pas une machine virtuelle au sens du noyau invité, mais FEXCore annoncera par défaut le bit hyperviseur et la marque `FEXIFEXIEMU` (code lu, H3). Cette valeur se mesure (X3) avant toute décision.
- **Règle.** Aucun masquage de l'identité hyperviseur n'est proposé ici.

### 4.3 Contenu de procfs

Le module a été vu ouvrant ou lisant : `/proc/self/maps` (parseur : au moins six champs, `début-fin` en hexadécimal, offset en hexadécimal, chemin à partir du champ 5 ; première ligne libc ; adresse = début + offset ; ELF machine 62, `ET_DYN`, `e_phentsize 56`), `/proc/<ppid>/comm`, `exe`, `cmdline`, `status`, `/proc/self/exe` (lien de 37 octets `.../wine64-preloader.static` dans la racine Debian), `/proc/self/fd/N` (module en memfd), `/proc/<enfant>/maps`, chemins DMI et PCI (ENOENT sous HyperLinux, sans verdict VM), le certificat `runtime.conf` du jeu (`LR/eac-static-210/maps-resolver.md` ; CHR:197 ; `P/eac-remote-maps-finding-20260930/README.md` ; `P/eac-parent-proc-comparison.md`).

Dans la session protégée QEMU, la vue `/proc/<jeu>/maps` du processus `RocketLeague.exe` (pid 347), copiée par l'observateur à la fin, montre (**mesuré**, `P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/outputs/game-process-maps.txt:1089-1098`) : la première ligne dont le chemin contient `libc.so` est la ligne 1098, `/x11/usr/lib/x86_64-linux-gnu/libc.so.6` à l'offset 0 (`r--p`), précédée de `libcap`, `libcups` et `libcom_err` qui ne correspondent pas au motif `libc.so` ; un objet mappé depuis `/memfd:5a9729b5-d83d-5dd7-3b14-3d69f8 (deleted)` (inode 1026, segments `r--p`, `r-xp`, `r--p`, `---p`, `r--p`, `rw-p`, puis un second segment `r-xp` de 5,3 Mio à l'offset 0x21f000) précède cette libc de quelques pages ; une recherche de `rosetta`, `aarch64`, `arm64`, `FEX` et `/mnt/rv` dans ce fichier ne trouve rien. **Inféré** : cet objet memfd est le module EAC (aucun hachage n'a été pris dans cette session ; le premier essai QEMU non authentifié a observé des mappings memfd exécutables, `eac-evidence.json`). La capture date de l'instant 684 s de l'observateur, soit environ 6 minutes après la fin de la session protégée (325 s) : le module est resté mappé.

### 4.4 Ce que la session protégée a réellement exercé

- **Durée et jalons (mesuré).** Matchmaking public `TryToPlayOnlineWithAntiCheat PlaylistId=3`, carte `TrainStation_Dawn_P`, accueil serveur à 224,42 s, jonction à 232,64 s, début de session protégée à 233,73 s, fin à 325,27 s, soit 91,54 s ; sortie volontaire de l'utilisateur ; aucune partie complète observée ; micro-saccades et glyphes manquants (`validation.json`).
- **Charge (mesuré).** Un processus `RocketLeague.exe` de 39 fils, `VmRSS` 4 311 204 kB (4,11 Gio), `futex_wait_multiple` dominant (`WINEFSYNC=1`), fils réseau (`HttpManagerThread`, `LibwebsocketsThread`, `RtcSignalingThread`) actifs (`outputs/cpu-task-status.json`). Observateur à 684 s : jeu vivant, `launcher_code 0` (`outputs/process-status.json`).
- **Activité ptrace (inconnu).** Aucune trace ptrace ni audit de syscalls n'a été enregistré pendant la session ni pendant les amorçages QEMU. Les scripts n'enregistrent que listes de processus, statistiques des fils, maps, journaux EAC (`guest-launch.py`). Le témoin ptrace x86 sur le même noyau (199/199) prouve la capacité du noyau, pas l'usage qu'en fait EAC.
- **Durée requise par Epic (section 5).** 91,54 s est très inférieur à « plus de 10 minutes » et aucune seconde session n'a été jointe.

### 4.5 Conséquences pour une exécution Darwin avec FEXCore

Chaque point est une exigence tirée des contrôles ; la preuve que son absence cause le 509 manque pour la plupart.

1. Un ABI Linux x86-64 cohérent pour Wine et le module dans un même processus : ptrace x86 complet (voir X2), memfd avec scellement, `clone`, `waitpid`, signaux, boucle locale.
2. Une table de VMA invitée exacte et ordonnée, sans objet de l'hôte, avec noms de fichiers invités, offsets exacts et `/memfd:<nom> (deleted)` pour les memfd. FEX Linux avait déjà la libc x86 d'abord ; HyperLinux l'a obtenue par allocation descendante.
3. Un modèle de processus avec un parent réel, un PID invité différent de 1 pour Wine et des entrées `/proc/<pid>` lisibles pour le parent et l'enfant.
4. Pages invitées de 4 KiB sur hôte de 16 KiB : non vu par EAC, mais exigé par Wine et FEX.
5. Les rejets F3 sont explicites et ne se contournent pas. Mesurer X3 (signaux réellement visibles) et, si possible, X1 (séquence d'EAC dans l'environnement accepté) avant de fixer les valeurs que le runtime synthétise pour `/proc`, `/sys` et CPUID.

## 5. Exigence de qualification Epic et déclarations sur VM, traduction et Wine hors Linux

La vérification en ligne a été faite le 2026-10-02 par `curl` sur les pages publiques (texte extrait des données de la page, sans exécuter de script). Les pages d'aide `epicgames.com/help/...` et la page Rocket League renvoient HTTP 403 à un client automatique : leur contenu n'est connu que par les paraphrases du dépôt.

### 5.1 Qualification (checklist Epic)

Page : https://dev.epicgames.com/docs/epic-online-services/trust-and-safety/anti-cheat-interfaces/anti-cheat-integration-check-list?lang=en-US

Critères de succès répétés à plusieurs étapes de la checklist (démarrage avec le bootstrapper, avec la protection client seule, avec toutes les protections), citation exacte :

- « Your game should function normally. »
- « The player should be able to join protected multiplayer game sessions with other players successfully and remain connected for more than 10 minutes. »
- « The player should be able to disconnect from one protected multiplayer game session and join a different one successfully. »

**Lu.** Le dépôt paraphrase en « plus de dix minutes de session protégée puis une reconnexion » (`TECH:222`). Le texte source parle d'une jonction réussie d'une session protégée différente après déconnexion, pas d'une reconnexion à la même session. **Inféré.** La checklist est un test de pré-publication pour l'intégrateur du jeu, pas une exigence adressée à des runtimes tiers ; le dépôt l'utilise comme barre de réussite. Elle ne dit rien des VM, de Wine ni de la traduction.

### 5.2 Déclarations sur les machines virtuelles

Page : https://dev.epicgames.com/docs/epic-online-services/trust-and-safety/anti-cheat-interfaces/anti-cheat-interfaces?lang=en-US (déjà citée par `TECH:88`).

- « Note: The two Anti-Cheat Interfaces support virtual machines differently: The Anti-Cheat Server Interface does support virtual machines (VM). The Anti-Cheat Client Interface does not support virtual machines (VM). »
- Page d'aide citée par le dépôt (`TECH:36`, `CHR:73`) : https://www.epicgames.com/help/c-32735058/c-37477814/a21516107 (HTTP 403 pour curl ; sa paraphrase dans le dépôt est cohérente avec la phrase ci-dessus).

**Mesuré.** Le refus explicite du libellé F3 existe et a été reçu dans deux environnements (WSL2, Linux ARM sous HVF + FEX). Il n'a pas été reçu dans QEMU TCG, OrbStack ni HyperLinux. Epic décrit une absence de support, pas une liste de VM refusées. Le verdict n'est donc pas généralisable aux VM (`P/runtime-alternatives-20260929/README.md:14`).

### 5.3 Déclarations sur Wine, Proton et les hôtes non Linux

- Annonce (datée de 2021 par `TECH:86`) : https://onlineservices.epicgames.com/news/epic-online-services-launches-anti-cheat-support-for-linux-mac-and-steam-deck. Citations : « Today, we extend support to Linux and Mac for developers who maintain full native builds of their games for these platforms. » et « support for the Wine and Proton compatibility layers on Linux is included ». Rien sur Wine sur macOS.
- Documentation actuelle (`anti-cheat-interfaces`) : « Linux Wine and Proton Support: It is possible to run many Windows games on Linux using the Wine or Proton compatibility layers and the anti-cheat client protection can support this configuration. To enable support for your game, you must be using SDK version 1.14 or greater and activate a client module for the Linux platform [...] Players running the game using Wine or Proton will use the Linux client module [...] ».
- Page `using-anti-cheat` (https://dev.epicgames.com/docs/epic-online-services/trust-and-safety/anti-cheat-interfaces/using-anti-cheat) : l'erreur NullClient se produit quand « The player is launching the game on a platform where no client module has been activated, for example using Wine/Proton when no Linux module has been activated. » Et : « At each game start, the anti-cheat bootstrapper fetches the latest configured anti-cheat client module for your game » ; « A new anti-cheat module might be activated automatically if a major compatibility or user security issue is discovered. » Ces deux phrases expliquent la dérive de module du 29 septembre (H10).
- Traduction : la même page signale que les modules 2508.3, 2509.2 et suivants contiennent « a compatibility update for xtajit64se.dll which is the latest implementation of Microsoft's x86/x64 emulation layer for Windows on ARM ». La page des interfaces écrit : « Players must have Secure Boot, TPM, and HVCI enabled when they use the Anti-Cheat Interfaces on the following versions of Windows for x86-64 » suivi d'une liste qui comprend « Windows on Arm », puis « These security features are enabled by default on Windows on Arm, and if disabled, players are prompted to enable them. » **Lu.** Epic adapte donc explicitement son module à la couche d'émulation x86 de Microsoft sur Windows sur ARM, sous contraintes de sécurité du système. **Inconnu.** Aucune déclaration trouvée sur Rosetta, FEX, Box64 ou une traduction sur Linux ou macOS.
- Apple silicon : https://dev.epicgames.com/docs/epic-online-services/platforms/mac?lang=en-US dit « From EOS SDK version 1.16.4, the anti-cheat module supports Apple silicon. » La version coréenne (`?lang=ko`) dit encore que le module ne le prend pas en charge ; elle est périmée. Cela résout la contradiction signalée par `TECH:94`. Cela concerne les jeux avec une version Mac native ; Rocket League n'en a pas.
- Linux ARM64 : `TECH:88` et `P/fex-eac-feasibility.md` affirment qu'Epic exclut Linux ARM64. Cette phrase n'a pas été retrouvée dans le texte actuel des pages `anti-cheat-interfaces` et `using-anti-cheat` (recherches de « ARM », « arm64 », « aarch64 »). **Non vérifié** : à recontrôler dans une autre page ou une version antérieure avant de s'en servir.
- Rocket League : EAC obligatoire sur PC depuis le 28 avril 2026, Linux et Steam Deck annoncés, macOS non pris en charge (`TECH:84`, https://www.rocketleague.com/news/easy-anti-cheat-comes-to-rocket-league-on-pc-today?lang=en et https://www.epicgames.com/help/c-37599050/a13980741 ; non relus en ligne, HTTP 403).
- CodeWeavers (paraphrase du dépôt, non relue en ligne) : le support dépend de l'éditeur et ne couvre pas CrossOver Mac (`TECH:90`, `CHR:71`) : https://www.codeweavers.com/blog/mjohnson/2026/8/31/why-do-most-games-with-anti-cheat-not-work-with-crossover-mac .

## 6. Inconnues, limites et notes de méthode

Inconnues.

1. Les appels ptrace et le statut de sortie de l'enfant auxiliaire d'EAC dans un environnement accepté (QEMU, Bazzite).
2. La condition précise qui déclenche F3 et la raison de F4. Le chemin de code du module qui produit « (#1) » n'est pas localisé (`P/eac-current-module-static-20260929/README.md`).
3. L'identité exacte du module chargé dans chaque run (seul un run tardif d'HyperLinux a le SHA mappé `1b9faa54...`, `P/eac-remote-maps-finding-20260930/README.md`).
4. La seconde où le libellé VM apparaît dans l'essai ARM sous HVF (le journal EAC est un fichier `.private`, non lu) ; le verdict des deux premiers essais ARM sous HVF.
5. Le résultat du contrôle Bazzite sans authentification (aucun retour reçu).
6. Le risque de sanction de compte pour une exécution en VM avec EAC actif : aucune donnée dans le dépôt.
7. La cause exacte de l'erreur GUI 256 de la ligne 15 (processus et parent non analysés).

Limites.

- Les durées de la ligne 11 incluent la préparation UMU. Elles ne se comparent pas aux délais mapping vers callback.
- Les lignes 13 à 15 reposent sur des runtimes privés non qualifiés (matrice Nix quatre modes indisponible, `FEXN/README.md`).
- La vérification des pages Epic donne leur texte du 2026-10-02 ; ces pages changent. La checklist peut être différente à la date de la prochaine lecture.

Notes de méthode et écarts.

- **Fichiers dont le nom évoque un secret.** (a) Un `grep -rIl` (qui ne liste que les noms de fichiers) a désigné `PERF/arm-game-20260930T233737Z/outputs/eac.private.log` comme contenant « Cannot run under Virtual » ; le contenu n'a pas été affiché ni copié. La même recherche a pu parcourir d'autres fichiers `.private` sans afficher leur contenu. (b) Deux fichiers au nom contenant `auth` ont été lus en entier : `PERF/arm-game-20260930T233737Z/auth-broker.log` (quatre lignes d'état `EPIC_BROKER_*=1`, aucune valeur) et `FEXN/authorized-epic-helper-path.json` (chemin d'un assistant et son SHA-256). (c) La consigne `docs/research/rocket-bazzite-unauthenticated-control-prompt.md` a été lue ; c'est un texte de procédure sans secret. (d) Les fichiers `plan.json`, `launch-record.private.json`, `epic-launch-plan*.private.json`, `result.json` des runs authentifiés sous FEX natif et les journaux `*.private.log` n'ont pas été ouverts. Ont été ouverts les `result.json` des trois essais ARM sous HVF (ligne de commande QEMU et statuts) et les résumés publics `public-terminal-summary.json` et `public-live-summary.json`.
- Une tentative accidentelle d'exécution de `--version` sur le fichier `UTM.app/.../qemu-x86_64-softmmu` a échoué immédiatement (`exec format error`) : aucun processus QEMU n'a été lancé.
- Aucun jeu, EAC, UTM ni QEMU n'a été lancé. Aucune écriture hors de ce rapport ; les copies de pages Epic et le script d'extraction sont dans le répertoire temporaire de la session. Les lectures de pages Epic par `curl` sont des GET anonymes de pages publiques.
