# Graphismes, affichage, entrées et audio pour Wine Linux x86-64 émulé sur macOS : inventaire des options

Rapport vivant, mis à jour à chaque jalon. Il sépare ce qui a été LU ou MESURÉ dans cette session de ce qui est INFÉRÉ. Les citations `fichier:ligne` pointent vers les chemins absolus indiqués dans la section « Chemins et abréviations ». Tout ce qui est marqué « a completer » n'est pas encore établi.

Dernière mise à jour : 2026-10-02, 15 h 50 heure locale (session hôte graphique : jalons G1 à G4, section « Statut des jalons G1 à G4 »).

## Etat courant en une phrase

Côté hôte, la chaîne Metal est qualifiée par des témoins possédés sans aucun invité Wine : serveur vtest Neptune corrigé construit depuis les sources (G1), rendu D3D11 DXMT d'un triangle vérifié pixel par pixel et partagé entre deux processus par une shm `DMTX` (G2, avec le `dxmt-native` d'UTM ; la construction depuis les sources bute sur le compilateur Metal absent), serveur de fenêtres `wsd` qui présente sans copie à 60 et 120 Hz et transmet les entrées (G3), et conception du raccordement Neptune et du pilote Wine (G4).

## Reprise du 2026-10-02 après la troisième interruption (état des preuves)

Le journal de la tentative précédente (spike vtest) n'a PAS survécu : la copie de travail vivait dans un répertoire temporaire de session qui n'existe plus (`/private/tmp/claude-501/...` ne contient que deux répertoires de session vides, aucune copie de `virglrenderer`). Les chiffres du spike (section 1.6) restent donc des affirmations sans journal sur disque tant qu'ils ne sont pas rejoués : ils sont marqués « MESURE (tentative précédente, journal perdu) » et rejoués dans la section 1.7 de cette reprise.

Faits ajoutés dans cette reprise (chaque fait renvoie à son journal sous `EXP/logs/` ou `EXP/evidence/graphics-display-audio/`) :

- MESURE `logs/graphics-voile-archs-001.log` : l'installation Voile contient DEUX arbres, `Voile/current/` (installé le 2026-09-26 19:26) et `Voile/v26.3.0-4/` (11:47). Les binaires D3DMetal sont identiques octet pour octet dans les deux (même SHA-256) et tous x86_64 seul ; les bibliothèques Unix de Wine (`winemac.so`, `winecoreaudio.so`, `winebus.so`, `ntdll.so`, `win32u.so`, `winevulkan.so`) diffèrent entre les deux arbres (tailles et SHA-256 différents) et sont toutes x86_64 seul. `runtime.json` n'existe que dans `v26.3.0-4/`.
- MESURE `logs/graphics-dlopen-d3dmetal-system-001.log` : sur macOS 27.0.1 (26A434), `dlopen("/System/Library/Frameworks/D3DMetal.framework/Versions/A/D3DMetal")` échoue en arm64 natif et en x86_64 sous Rosetta (« no such file, not in dyld cache »). Le nom d'installation du D3DMetal de Voile pointe pourtant sur ce chemin système : il n'y a donc aucun D3DMetal fourni par le système, seul celui de Voile ou d'UTM existe. Rosetta 2 est installé (`arch -x86_64 /usr/bin/true` rend 0 ; paquet `com.apple.pkg.RosettaUpdateAuto`).
- MESURE `logs/graphics-utm-archs-001.log` et `logs/graphics-native-exports-001.log` : les chiffres de la section 1.2 sont reconfirmés (D3DMetal d'UTM x86_64 seul, 5 263 904 o ; `d3dmetal-native` universel avec 69 symboles exportés en x86_64 et 0 en arm64 ; `dxmt-native` universel avec les mêmes 16 symboles exportés dans les deux tranches ; `render-server-real` universel).
- LU : un autre chantier (HyperLinux et FEX natif) a compilé aujourd'hui un serveur vtest macOS arm64 dans `RHL/probes/rocket-performance-20260930/fex-native-hyperlinux-20261001/native-neptune-vtest-runtime/` et a obtenu protocole 4, capset Neptune valide et contexte initialisé (`owned-protocol-20261002T081849820140Z/result.json`), puis un blob partagé de 49 152 octets relu (`shared-memory-qualified.json`). Ce chantier utilise un autre correctif de shm (`mkstemp` suivi de `unlink` dans `/private/tmp`) que mon spike (`shm_open`). Je ne l'ai pas modifié et je ne l'ai pas réutilisé comme preuve de ma part.

### Reprise du 2026-10-02 (quatrième session, 14h24 heure locale)

- MESURE `logs/graphics-spike-state-001.log` : la copie `EXP/src/graphics-spike/virgl/` (577 fichiers, créée à 11:03) est IDENTIQUE octet pour octet à `NEP/virgl` (`diff -rq` vide) et à `virgl-orig/`. Les correctifs de l'annexe A n'ont donc PAS été appliqués à cette copie (zéro occurrence de `out_fence_wr` dans `vtest/vtest_renderer.c`, zéro de `os_create_anonymous_file` dans `vtest/vtest_shm.c`). Aucun répertoire de build n'existe. Les chiffres de la section 1.6 restent donc « MESURE (tentative précédente, journal perdu) » jusqu'au rejeu documenté en 1.7.
- La section « Statut des jalons G1 à G4 » ci-dessous rapporte l'application des correctifs, la construction, l'exécution du serveur et des clients natifs, avec les journaux sous `EXP/logs/graphics-*`.
- Les sections 3 à 7 ont été rédigées dans cette session (jalons écrits sur disque au fur et à mesure).

## Statut des jalons G1 à G4 (session hôte graphique, 2026-10-02 après-midi)

Cette section remplace la « section 1.7 » annoncée plus haut, qui n'a jamais été écrite. `W` = `EXP/witnesses/graphics`, `G` = `EXP/src/graphics-spike`.

### G1. Renderer hôte : ATTEINT (avec `dxmt-native` emprunté)

MESURE :

- Annexe A appliquée à `G/virgl` par `G/patches/apply_annex_a.py` (remplacement exact de blocs, idempotent), `G/virgl-orig` reste intact. Diff obtenu : `G/patches/annex-a.diff`, 113 lignes, SHA-256 `07f711fb…d27a` (`logs/graphics-annexA-apply-001.log`).
- Construction : `G/build-virgl.sh` (libepoxy 1.5.10 sans GLX/EGL/X11 déjà installée dans `G/prefix`, puis virglrenderer `-Dneptune=true -Dvtest=true -Dvenus=false -Drender-server-mode=process`, `ninja -j8`) : 97 unités, `rc=0`, produit `G/prefix/bin/virgl_test_server` et `G/prefix/libexec/virgl_render_server` arm64 (`logs/graphics-virgl-build-001.log`).
- Témoin possédé `W/vtest_client.c` (construit par `W/build-vtest-client.sh`, lancé par `W/run-vtest-client.sh`, tout sous `run-with-timeout.sh`, socket dans un dossier `mktemp -d` 0700, chemin de 62 octets). Résultat `logs/graphics-vtest-client-003.log` : protocole 4, `MAX_TIMELINE_COUNT=64`, capset 7 valide de 60 octets, `wire_format_version=1`, `caps_flags=0x36`, `CONTEXT_INIT` suivi d'un `GET_PARAM` répondu, journal serveur « loaded D3D11 library … dxmt-native » et « created Neptune context 1 », `RESULT PASS`.
- Blobs `HOST3D_GUEST` reçus par SCM_RIGHTS, `mmap(MAP_SHARED)`, cohérence vérifiée par une seconde projection indépendante du même fd : 64 Kio, 12 288 o (le `st_size` devient 16 384), 1 Mio, 16 Mio. Latence création + `mmap` + écriture d'un octet par page de 16 Kio (n=50) : médiane 0,030 ms / p99 0,071 ms (64 Kio), 0,069 / 0,085 ms (1 Mio), 0,614 / 0,700 ms (16 Mio).
- `SUBMIT_CMD2` vide avec `RING_IDX|OUT_FENCE_FD` : 200/200 fds lisibles par anneau ; anneau 0 médiane 0,017 ms, p95 0,024, p99 0,056, max 0,059 ; anneau 1 médiane 0,017, p95 0,035, p99 0,062, max 0,095.
- Piège mesuré (`logs/graphics-vtest-client-001.log`, `-002.log`, `logs/graphics-dxmt-borrow-00{1,2,3}.log`) : une tranche arm64 extraite par `lipo -thin`, puis une copie nue du Mach-O universel (même SHA-256 `f7444460…6dfd`), échouent toutes deux au `dlopen` avec « code signature invalid (errno=1) » alors que le fichier en place se charge. La signature Developer ID de Turing Software lie l'`Info.plist` du bundle ; il faut copier le framework entier (`ditto`), ce qui est fait dans `G/third_party/utm-5.0.5/dxmt-native.framework` (provenance dans `PROVENANCE.txt`). Le premier passage du témoin rendait PASS alors que le worker n'avait chargé aucun backend (Neptune crée le contexte malgré l'échec) : le script de lancement exige donc l'absence de « failed to open NPT_ » dans le journal serveur.

LU : le worker journalise aussi « failed to load dxmt_open_existing_heap_from_fd » : ce binaire UTM 5.0.5 n'exporte pas l'import de heap ; sans effet sur ce témoin.

INFERE : le fd de fence devient lisible au retrait de la fence de timeline du worker, ce qui ne prouve pas la fin des écritures GPU (voir 1.6). Ce témoin ne soumet aucune commande Neptune sérialisée (aucun `D3D11CreateDevice` par l'anneau) : le rendu DXMT est qualifié séparément en G2.

Emprunté contre construit : virglrenderer, libepoxy et le témoin sont construits depuis les sources ; `dxmt-native` est le binaire d'UTM 5.0.5 (marchepied, à remplacer par la construction G2).

### G2. DXMT sur ce Mac : étape 1 (binaire emprunté) ATTEINTE, étape 2 (depuis les sources) BLOQUÉE par le compilateur Metal absent

MESURE, étape 1 (`logs/graphics-dxmt-triangle-borrowed-001.log`, binaire UTM 5.0.5 `f7444460…6dfd`) :

- Témoin `W/dxmt_triangle.m` (Objective-C++ : les en-têtes MinGW D3D11 fournis par DXMT ne définissent `__uuidof` et les interfaces COM qu'en C++), construit par `W/build-dxmt-triangle.sh`, charge `dxmt-native` par `dlopen` comme le worker Neptune.
- Shaders : DXBC SM 5.0 précompilés `vs_pass` et `ps_color`, copiés de `dxmt/tests/native/shaders_dxbc.h` au commit `bcdcf6da` (compilés hors ligne par fxc sous Windows d'après `tests/native/README.md`, puis `xxd -i`) ; copie et SHA-256 dans `W/dxbc/PROVENANCE.txt`. Ils n'ont pas été régénérés ici (pas de fxc sur macOS).
- Mode `offscreen` : `D3D11CreateDevice` rend le niveau `0xb100` (11_1), fournisseur `0x106b` ; `dlopen` 432 ms au premier lancement (3 ms ensuite), création du périphérique 12 à 32 ms. Triangle tricolore dans une cible RGBA8 256x256, relu par une texture STAGING et comparé pixel par pixel à la couleur barycentrique analytique (tolérance 3/255, pixels à moins d'environ 1,5 px d'une arête exclus) : 63 088 pixels jugés, 19 602 à l'intérieur, 0 écart, centroïde 85,85,86,255. Premier tracé jusqu'à la fin GPU (requête EVENT) 114 ms (compilation du pipeline), puis moyenne 0,316 ms et maximum 0,962 ms sur 20 tracés.
- Mode `shared` : texture BGRA8 `D3D11_RESOURCE_MISC_SHARED` ; `IDXGIResource::GetSharedHandle` rend un POD `DMTX` version 2 (fd 11, 256x256, format 87, `stride` 1024, `size` 262 144, `offset` 0, objet shm de 262 144 o). Après le tracé, trois lectures concordent sans écart : la copie STAGING, une projection `mmap` du fd dans le producteur, et un second processus lancé par `posix_spawn` qui reçoit le fd par SCM_RIGHTS sur une `socketpair`, le projette et vérifie les pixels ; CRC32 identiques (`473ce849`) ; 7,1 ms du `posix_spawn` à la réponse.

LU : la texture partagée native de DXMT est une texture linéaire Metal adossée à une shm POSIX anonyme créée par `shm_open(O_EXCL)` puis `shm_unlink`, projetée par `mmap(MAP_SHARED)` et enveloppée par `newBufferWithBytesNoCopy` (`dxmt/src/d3d11/d3d11_texture_shared_native.cpp:36-78, 140-194`) ; `GetSharedHandle` rend un pointeur vers le POD détenu par la texture (`d3d11_texture_device.cpp:306-330`) ; l'ouverture côté consommateur refait la même projection sur le fd reçu (`:600-635`). `SHARED_NTHANDLE` est refusé (`:415-420`).

INFERE : l'attente de fin GPU passe ici par une requête EVENT dans le même processus ; un consommateur distant n'a pas encore de signal de fin d'écriture (voir 1.6, fence partagée à qualifier en G3/G4).

Étape 2 : sources `utmapp/dxmt` clonées au commit `bcdcf6da89af6c9fedeb2fc92b4367b23d5d6ed6` (« native: shared-texture handle version 2 with a placement offset », 2026-08-15), sous-modules `include/native/directx` `9df86f23` et `external/nvapi` `d08488fc` (`logs/graphics-dxmt-fetch-001.log`) ; LLVM 15.0.7 source téléchargé (SHA-256 `8b5fcb24…eac6`, calculé localement, non comparé à une somme publiée) ; construction minimale de LLVM lancée par `G/build-llvm15.sh` (`logs/graphics-llvm15-build-001.log`).

MESURE, étape 2 : LLVM 15.0.7 minimal (aucune cible, aucun outil, arm64, `-j8`, 1 824 unités) construit et installé dans `G/llvm15` avec Apple clang 21 sans correctif (`LLVM15_DONE`, `logs/graphics-llvm15-build-001.log`). La configuration meson de DXMT (`G/build-dxmt.sh`, `-Dnative_llvm_path=G/llvm15`, macOS min 13.0) réussit, mais la construction s'arrête sur les 7 cibles `.air` (`air_msad.metal`, `air_samplepos.metal`, `air_tessellation.metal`, `dxmt_command.metal`) : « cannot execute tool 'metal' due to missing Metal Toolchain » (`logs/graphics-dxmt-build-001.log`). `xcodebuild -showComponent MetalToolchain` rend « Status: uninstalled » (`logs/graphics-metal-toolchain-001.log`). Le compilateur Metal hors ligne est un composant Xcode à télécharger (`xcodebuild -downloadComponent MetalToolchain`), installation globale que je n'ai pas faite sans accord : DEMANDE au lead. Un passage `ninja -k 0` (`logs/graphics-dxmt-build-002.log`) ne trouve pas d'autre erreur, mais il s'arrête à 25 unités sur 146 parce que presque tout dépend des `.air` : l'absence d'autres blocages n'est donc PAS établie.

### G3. Hôte de présentation et d'entrées (« wsd ») : ATTEINT, avec réserves

Fichiers : `EXP/src/wsd/wsd.m` (Objective-C, un seul fil applicatif : AppKit, E/S socket par sources GCD sur la file principale, Metal), `wsd_protocol.h` (format binaire, tailles vérifiées par `_Static_assert` pour être identiques côté Linux x86-64), `PROTOCOL.md`, `build.sh` (signature ad hoc, aucun entitlement : `codesign -d --entitlements -` vide). Témoins : `W/wsd_client.c` (`W/build-wsd-client.sh`), `W/run-wsd-client.sh` (dossier `mktemp -d` 0700, socket 0600, chemin de 69 octets, tout sous `run-with-timeout.sh`).

MESURE :

- Sécurité de la socket : `wsd` refuse un dossier qui n'est pas un vrai dossier 0700 du même uid, refuse un chemin de plus de 103 octets, vérifie chaque pair par `getpeereid` (uid 501 accepté, journal `client pid … uid 501 accepted`). Le chemin de refus d'un autre uid n'a PAS été exercé (pas de second compte).
- Présentation sans copie CPU côté serveur : fd shm reçu par SCM_RIGHTS, `mmap(MAP_SHARED)`, `newBufferWithBytesNoCopy`, texture linéaire `newTextureWithDescriptor:offset:bytesPerRow:`, une passe de rendu par tic de `CADisplayLink` vers le drawable du `CAMetalLayer` (BGRA8, 3 drawables, `displaySyncEnabled`), `EV_RELEASE` à la fin du tampon de commandes, `EV_PRESENTED` avec `presentedTime`. Le client CPU écrit ses pixels lui-même (rôle du producteur), le serveur n'en copie aucun.
- Écran de mesure : écran principal `XG27AQWMG` 2560x1440, échelle 1,0, `maximumFramesPerSecond` 240 (écran externe ; l'écran interne n'a pas été testé).
- Cadence et latence, client CPU 640x360 BGRA8 triple tampon, 4 s par cadence (`logs/graphics-wsd-client-003.log` et `-004.log`, deux passages) :

| Cadence demandée | Images à l'écran | Intervalle entre `presentedTime` p50 / p95 / p99 (ms) | Latence soumission -> `presentedTime` p50 / p95 / p99 (ms) | Remplacées (boîte aux lettres) |
| --- | --- | --- | --- | --- |
| 60 Hz, passage 003 | 239/240 | 16,669 / 16,669 / 20,836 | 20,666 / 21,011 / 24,781 | 0 |
| 60 Hz, passage 004 | 231/240 | 16,669 / 20,836 / 37,505 | 9,304 / 13,495 / 17,230 | 8 |
| 120 Hz, passage 003 | 468/480 | 8,334 / 12,502 / 16,669 | 18,587 / 18,806 / 22,788 | 12 |
| 120 Hz, passage 004 | 480/480 | 8,334 / 12,502 / 12,502 | 17,861 / 18,126 / 18,171 | 0 |

- Les intervalles sont des multiples de 4,167 ms (240 Hz) : le lien d'affichage demandé à 60 ou 120 Hz tombe sur une grille de 240 Hz et un retard d'un tic de 240 Hz apparaît au p95. La latence dépend de la phase entre l'horloge du client et le lien d'affichage (le client n'est pas calé sur la synchronisation verticale) : de 9 à 21 ms au p50.
- Fenêtre masquée : dans les passages `-001` (client CPU) et `logs/graphics-wsd-dxmt-001.log` (DXMT), presque tous les `presentedTime` valent 0, avec `occlusion_visible=0` dans le journal de `wsd` : la fenêtre était recouverte (l'application n'est pas activée par `[NSApp activate]` au lancement depuis un terminal, activation coopérative de macOS : `app_active=0`, `key=0` au moment de l'affichage). Correction : style `WSD_STYLE_TOPMOST` (`NSFloatingWindowLevel`, équivalent de `HWND_TOPMOST`) et `orderFrontRegardless` ; ensuite 0 ou 1 image non affichée par passage.
- Entrées : le scénario synthétique (`DEBUG_INJECT`, événements `NSEvent` passés à `-[NSWindow sendEvent:]`, marqués bit 31) livre 4/4 touches (`a` keycode 0 avec le caractère « a », Maj keycode 56 avec le bit de modificateur 0x2), 4/4 boutons à (10,20) px, 1 mouvement, 1 molette (dx 1, dy -3). Le mode relatif appelle réellement `CGAssociateMouseAndMouseCursorPosition(0)` (rend 0, aucune demande d'autorisation) puis rétablit 1 après environ 0,3 s ; un delta brut synthétique (5,-7) revient en `EV_RELATIVE`. Le clip rend (99,99) avec le bit « bridé » pour un mouvement hors du rectangle 100x100. Plein écran : un seul `EV_RESIZE` 2560x1440 après 2,9 ms, niveau de fenêtre 25, retour à 640x360 après 6,5 ms. Ping : p50 0,016 ms, p99 0,608 ms.
- Entrées réelles : 24 mouvements de souris réels (bit 31 absent) reçus pendant le passage `-002` quand le pointeur a traversé la fenêtre. Aucun clic, touche ou molette réels n'ont été mesurés.
- DXMT vers `wsd` (`logs/graphics-wsd-dxmt-002.log`, `W/dxmt_triangle.m` mode `wsd`) : trois textures partagées `DMTX` v2 256x256 BGRA8 rendues par DXMT (binaire emprunté) dont les fds sont attachés une fois à `wsd`, triangle en rotation, attente de fin GPU par requête EVENT avant `PRESENT`, réutilisation après `EV_RELEASE`. Zéro copie de bout en bout : `wsd` échantillonne les pages shm où DXMT a rendu. 60 Hz : 239/240 affichées, intervalle p50 16,669 / p95 20,836 / p99 20,836 ms, latence `PRESENT` -> affichage p50 25,7 ms ; rendu DXMT + fin GPU par image p50 0,536 ms. 120 Hz : 478/480, intervalle p50 8,334 / p95 12,502 / p99 12,502 ms, latence p50 15,8 ms, rendu p50 0,204 ms. Fenêtre 512x512 : mise à l'échelle x2 par l'échantillonneur.

LU : `CAMetalDrawable.presentedTime` vaut 0 si l'image n'a jamais été affichée (documentation Apple de `addPresentedHandler`), ce qui concorde avec l'occlusion mesurée.

INFERE et limites :

- Les entrées synthétiques valident l'encodage et le protocole, pas le chemin matériel (HID vers `NSEvent`) ; les clics, touches, molette et deltas bruts réels, le masquage effectif du curseur et le retour du pointeur par `CGWarpMouseCursorPosition` restent à vérifier par une personne devant l'écran. Le confinement est logiciel (bridage et retour du pointeur), sans `CGEventTap` : aucune permission Accessibilité requise, mais le pointeur peut sortir un instant avant le retour.
- Pas d'IME ni de `interpretKeyEvents` : le texte composé (accents morts, AZERTY) n'est pas traité ; `EV_KEY` donne le keycode macOS et `NSEvent.characters`.
- Pas encore de fence GPU partagée : le producteur DXMT attend sa propre requête EVENT avant `PRESENT`, ce qui sérialise le CPU du jeu sur le GPU. Avec Neptune, la fin d'écriture viendra du fd de fence de `SUBMIT_CMD2` (G1), qui ne prouve aujourd'hui que le retrait de la fence de timeline (voir 1.6).
- Le client ne se cale pas sur la synchronisation verticale ; un événement `EV_VBLANK` optionnel réduirait la latence d'environ une période.

### G4. Conception (rien de cela n'est construit) : présentation Neptune et pilote d'affichage Wine sur `wsd`

`N` = `NEP/mesa-triton/src/virtio/neptune`, `VW` = `/Users/mathieu/Documents/Github/Zagrios/voile-wine`. Tout ce qui suit est INFERE sauf les citations (LU).

#### G4.1 Neptune : remplacer DRI3/Present par `wsd`

LU, point de départ :

- `N/nptunix/npt_unix_wsi.c` (396 lignes) tient un état global unique `g_wsi` (`:39-59`, donc une seule chaîne d'échange présentée par processus), se connecte à X (`:80`), exige DRI3 et Present (`:88-120`), ouvre `/dev/dri/renderD128` pour les syncobj (`:104`), crée un pixmap par image avec `xcb_dri3_pixmap_from_buffer_checked` en supposant `size = width*height*4` et profondeur 24/32 (`:141-145`), présente avec `ASYNC|COPY` (`:246-247`), attend la fence en bloquant le fil appelant jusqu'à 30 s en chemin 2 (`:313-321`) et compte les `IDLE_NOTIFY` par image (`:197-215`).
- `N/nptunix/npt_unixlib.h:64-73` : `npt_unix_wsi_init_params` porte `x11_window`, `fds[4]`, `strides[4]`, `virgl_format`, `width`, `height`, sans décalage ni taille par image ; `:83-101` : `present` (index, fd de fence, drapeau « consommé ») et `drain` (compteurs de libération par image, pas un masque).
- `N/npt_renderer_wine_common.c:47-85` : `unixlib_wsi_init` lit `GetPropW(hwnd, L"__wine_x11_whole_window")` (`:64`) et échoue sans fenêtre X11.
- `N/npt_swapchain_common.c:378-409` : chaque image est un blob hôte réclamé par `create_host_blob` avec la taille arrondie à 4 096 (`:398-399`), la `pitch` du plan 0 sert de `stride` ; le décalage du plan (`exp.planes[0].offset`) est disponible mais n'est pas transmis. Côté hôte, `virgl/src/neptune/npt_shared.c:195-201` exporte le fd du POD `DMTX` et une taille `offset + size` : une texture placée dans un tas partagé commence à `offset` dans l'objet.
- Formats : `npt_shared_texture.c:109-120` rend 1 (BGRA8), 2 (BGRX8) ou 67 (RGBA8).

Modifications proposées :

1. `npt_unixlib.h:64-73` : remplacer `uint32_t x11_window` par `uint64_t host_window` (identifiant de fenêtre `wsd`) et ajouter `uint64_t offsets[NPT_WSI_MAX_IMAGES]` et `uint64_t sizes[NPT_WSI_MAX_IMAGES]`. Les deux côtés (PE et `.so`) sont construits ensemble, la structure peut changer.
2. `npt_swapchain_common.c:388-409` : remplir `offsets[i] = exp.planes[0].offset` et `sizes[i] = exp.allocation_size`.
3. `npt_renderer_wine_common.c:58-71` : remplacer la lecture de `__wine_x11_whole_window` par `GetPropW(hwnd, L"__wine_sailover_window")`, propriété posée par le pilote B sur chaque HWND qui a une fenêtre hôte (G4.2). Pour une fenêtre enfant, remonter à l'ancêtre de premier niveau et présenter dans son rectangle client (première version : fenêtres de premier niveau seulement).
4. Nouveau fichier `N/nptunix/npt_unix_wsi_wsd.c` qui garde les quatre points d'entrée (`npt_do_wsi_init/present/drain/destroy`) et l'ordre de l'interface `__wine_unix_call_funcs` :
   - `init` : obtenir la connexion du pilote dans le même processus par `dlopen("winesailover.so", RTLD_NOLOAD)` puis `dlsym("sailover_wsd_submit")` et `dlsym("sailover_wsd_set_present_listener")` ; pour chaque image, `BUFFER_ATTACH { buffer_id = 0x80000000 | (génération << 4) | i, width, height, stride, format, offset, size }` avec un `dup` du fd. Le format 2 (BGRX8) demande un format `wsd` de plus qui force alpha à 1 dans le shader.
   - `present` : version 1, attendre la fence côté client comme le chemin 2 actuel puis `PRESENT` ; version 2 (préférée) : `PRESENT_FENCED` qui transmet le fd de fence à `wsd`, lequel n'encode l'image qu'une fois ce fd lisible (source GCD sur le fd), sans bloquer le fil du jeu ; `wait_fence_consumed = 1` dans les deux cas.
   - `drain` : le fil lecteur du pilote appelle l'écouteur pour chaque `EV_RELEASE` dont `buffer_id` a le bit 31 ; l'écouteur incrémente `released_count[i]` sous un mutex et réveille une variable de condition ; `drain` attend au plus `timeout_ms`.
   - `destroy` : `BUFFER_DETACH` de chaque image.
   - Disparaissent : libxcb, xcb-dri3, xcb-present, libdrm, l'ouverture de `/dev/dri/renderD128`.
5. `N/nptunix/meson.build` : option `-Dnpt_wsi=x11|wsd` qui choisit le fichier et retire les dépendances X/DRM pour `wsd`.
6. Sorties DXGI : remplacer `npt_unix_enumerate_outputs` (`nptunix/npt_display_drm.c`) par une requête `wsd` `GET_OUTPUTS` (écrans, taille en pixels, fréquence maximale, échelle ; le `HELLO` actuel n'en donne qu'un).
7. Plein écran : Neptune fait déjà une fenêtre sans bordure `HWND_TOPMOST` qui couvre le moniteur (`npt_swapchain.c:520-560, 651-684`, section 2.1) ; le pilote B traduit cette géométrie en `WINDOW_FULLSCREEN` (mesuré en G3) et la mise à l'échelle se fait dans le shader de `wsd`.

Points de vigilance : la fin d'écriture GPU (la fence de G1 ne mesure que le retrait de la timeline) ; le singleton `g_wsi` (un jeu avec deux chaînes d'échange, ou un changement de fenêtre, réinitialise tout) ; `wsd` exige `stride` et décalage multiples de `minimumLinearTextureAlignmentForPixelFormat` (mesuré : 1 024 et 0 passent).

#### G4.2 Pilote d'affichage Wine « winesailover.drv » (option B), squelette

Modèle LU : `winewayland.drv` ne renseigne que 21 membres de `user_driver_funcs` (`VW/dlls/winewayland.drv/waylanddrv_main.c:36-58`) ; l'interface complète est `VW/include/wine/gdi_driver.h:364-446` (`WINE_GDI_DRIVER_VERSION 108` à `:221`). Les entrées passent par `NtUserSendHardwareInput(hwnd, 0, &input, 0)` (`wayland_pointer.c:167, 272, 319, 388`, `wayland_keyboard.c:644, 814, 840`) ; le mouvement relatif est un `MOUSEEVENTF_MOVE` sans `ABSOLUTE` (`wayland_pointer.c:377`) et les mouvements absolus sont ignorés en mode relatif (`:175-176`). Règle de capture (`wayland_pointer.c:883-888`) : rectangle de clip ou fenêtre couvrant l'écran virtuel avec curseur caché, ou forçage, donne un verrouillage du pointeur (mode relatif) ; clip avec curseur visible donne un confinement.

Attention : le Wine invité validé en QEMU est GE-Proton11-7 (section 2.0), pas voile-wine. Le pilote doit être compilé dans l'arbre exact du Wine invité ; la version de l'interface de `gdi_driver.h` de GE-Proton n'a pas été lue.

Structure :

- Partie PE `winesailover.drv` : comme `winewayland.drv/dllmain.c:30, 109`, un fil créé au chargement appelle l'unixlib `read_events` en boucle.
- Partie Unix `winesailover.so` (ELF Linux x86-64 sous FEX) : connexion unique à `wsd` (chemin dans `SAILOVER_WSD_SOCKET`), `HELLO`, table `user_driver_funcs`, fil lecteur qui décode les événements, exports `sailover_wsd_submit` et `sailover_wsd_set_present_listener` pour `nptunix.so`.
- `HKCU\Software\Wine\Drivers\Graphics = sailover` (`programs/explorer/desktop.c:43, 996-1030`, section 2.3).

Points d'entrée nécessaires pour Rocket League, et leur traduction `wsd` (les requêtes marquées « v2 » n'existent pas encore dans `wsd_protocol.h`) :

| Membre | Rôle pour Rocket League | Traduction |
| --- | --- | --- |
| `pCreateWindow`, `pDestroyWindow` | fenêtre du jeu, lanceur, boîtes de dialogue d'EAC | `WINDOW_CREATE`/`DESTROY` pour les fenêtres de premier niveau ; propriété `__wine_sailover_window` |
| `pWindowPosChanging`, `pWindowPosChanged` | taille, position, visibilité, passage en plein écran | `WINDOW_SHOW` ; `WINDOW_FULLSCREEN` quand le rectangle couvre le moniteur sans légende ; position et taille par `WINDOW_SET_FRAME` (v2) |
| `pShowWindow`, `pSysCommand` | minimiser, restaurer (Alt+Tab) | `WINDOW_MINIMIZE` (v2) |
| `pSetWindowStyle`, `pSetWindowText`, `pSetWindowIcons` | style, titre | `SET_TITLE` ; style au prochain `WINDOW_SET_FRAME` |
| `pActivateWindow` + `EV_FOCUS` | `WM_ACTIVATE`, pause du jeu hors focus | `EV_FOCUS` vers `NtUserSetForegroundWindow` ; activation demandée à `wsd` (`WINDOW_SHOW`) |
| `pCreateWindowSurface` | rendu GDI du lanceur et des dialogues EAC | surface en shm attachée par `BUFFER_ATTACH`, `PRESENT` au vidage de la surface (même présentateur que Neptune) |
| `pSetCursor` | curseur du menu, caché en jeu | `CURSOR_VISIBLE` (mesuré), image ARGB + point chaud par `CURSOR_SET_IMAGE` (v2) |
| `pGetCursorPos`, `pSetCursorPos` | recentrage de la caméra par certains moteurs | position en cache depuis `EV_MOTION` ; `CURSOR_WARP` (v2, `CGWarpMouseCursorPosition`) |
| `pClipCursor` | capture de la souris en jeu | règle de `winewayland` ci-dessus : clip + curseur caché -> `RELATIVE_MOUSE` (mesuré, `CGAssociateMouseAndMouseCursorPosition`), clip + visible -> `CURSOR_CLIP` (mesuré, bridage logiciel) |
| entrées souris | caméra `MouseX/MouseY`, raw input, `dinput8` | `EV_BUTTON`/`EV_MOTION`/`EV_WHEEL` -> `MOUSEINPUT` absolu ; `EV_RELATIVE` -> `MOUSEEVENTF_MOVE` relatif ; wineserver dérive le raw input de ces entrées matérielles (INFERE à vérifier avec `+rawinput`) |
| entrées clavier, `pKbdLayerDescriptor`, `pReleaseKbdTables`, `pMapVirtualKeyEx`, `pToUnicodeEx` | liaisons par touche, saisie du chat | table keycode macOS -> scancode/VK portée de `winemac.drv/keyboard.c` ; `KEYBDINPUT` avec `KEYEVENTF_SCANCODE` ; `pKbdLayerDescriptor` NULL donne `kbdus` au début (section 2.1) |
| `pUpdateDisplayDevices`, `pChangeDisplaySettings` | liste de modes, résolution plein écran | moniteurs et modes depuis `GET_OUTPUTS` (v2) ; changement de mode émulé (succès, mise à l'échelle Metal) |
| `pWindowMessage`, `pDesktopWindowProc`, `pThreadDetach`, `pBeep` | plomberie | comme `winewayland` |
| `pClipboardWindowProc`, `pUpdateClipboard` | presse-papiers | plus tard (non requis pour une partie) |
| `pVulkanInit`, `pOpenGLInit` | aucun (D3D11 par Neptune) | NULL, le pilote nul de `win32u` prend le relais |

Soit environ 25 membres, du même ordre que `winewayland` (21).

#### G4.3 Connexion du côté Unix (ELF x86-64 sous FEX) à la socket de `wsd`

- Chemin : `wsd` écoute sur un dossier hôte privé court (mesuré : `$TMPDIR/wsd.XXXXXX/wsd.sock` fait 69 octets, sous la limite de 103). La couche Linux expose ce dossier dans la racine invitée (par exemple `/run/sailover`) et le lanceur exporte `SAILOVER_WSD_SOCKET=/run/sailover/wsd.sock` pour Wine. Le module réseau de la couche traduit `connect(AF_UNIX, chemin invité)` en `connect` hôte sur le chemin hôte ; le chemin hôte, pas le chemin invité, doit tenir dans 103 octets.
- Authentification : le processus FEX tourne sous l'uid de l'utilisateur, donc `getpeereid` côté `wsd` voit le bon uid sans rien traduire ; `LOCAL_PEERPID` donne le pid hôte du processus FEX.
- SCM_RIGHTS : l'en-tête `cmsghdr` Linux x86-64 fait 16 octets (`size_t cmsg_len`, alignement 8) contre 12 sur Darwin (`socklen_t`, alignement 4) ; la couche doit réencoder les messages de contrôle et traduire les numéros de fd invités en fds hôtes par sa table de fds. `wsd` v1 n'envoie jamais de fd au client, seul le sens client -> serveur est requis.
- Mémoire partagée : les fds attachés (images Neptune, surfaces GDI) sont des objets shm hôtes reçus de vtest. Le `mmap(MAP_SHARED)` invité doit être une vraie projection partagée de l'objet hôte ; HyperLinux faisait une copie privée pour `MAP_SHARED` sur fichier (`RHL/graphics-notes.md:7-15`) : avec une copie, ni le rendu ni la présentation ne marchent. Les décalages de projection hôte doivent être des multiples de 16 Kio et `ftruncate` arrondit la taille à 16 Kio (mesuré en 1.6) alors que le client Neptune arrondit à 4 Kio (`npt_swapchain_common.c:398-399`).
- Format binaire : `wsd_protocol.h` n'utilise que des champs de taille fixe, alignés naturellement, avec des `_Static_assert` sur les tailles ; un en-tête identique compilé pour Linux x86-64 (LP64 comme Darwin arm64, petit-boutiste) donne les mêmes tailles. Les temps sont en `CLOCK_UPTIME_RAW` hôte : le pilote ne doit pas les comparer à l'horloge invitée sans conversion.

### Demandes au lead et lacunes connues (session hôte graphique)

- DEMANDE : autoriser `xcodebuild -downloadComponent MetalToolchain` (composant Xcode global, environ un téléchargement Apple, aucune écriture dans `/opt` ni `/usr/local`) pour finir la construction de DXMT depuis les sources ; sans lui, `dxmt-native` reste le binaire d'UTM 5.0.5. Variante sans installation : extraire les `.air` précompilés du binaire UTM, ce qui ne serait plus une construction depuis les sources.
- Lacunes : refus d'un pair d'un autre uid non exercé ; entrées réelles (clics, touches, molette, deltas bruts, retour du pointeur bridé) non mesurées, seules 24 positions de souris réelles l'ont été ; masquage effectif du curseur non vérifié ; écran interne (ProMotion) non mesuré, seul l'écran externe 240 Hz l'a été ; la fence vtest ne prouve pas la fin GPU ; somme SHA-256 de l'archive LLVM non comparée à une somme publiée ; tranche x86_64 / D3DMetal non exercée.

## Chemins et abréviations

- `EXP` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-wine-fex-darwin-20261002`
- `RHL` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928`
- `NEP` = `$RHL/probes/qemu-neptune-graphics-20260930`
- `DOC` = `/Users/mathieu/Documents/Github/Zagrios/sailover/docs/research/rocket-league-eac-technologies-macos-2026-10-01.md`
- `VOILE` = `/Users/mathieu/Library/Application Support/com.zagrios.sailover/Voile/`

## Acquis des documents existants (LU, non revérifié dans cette session)

- `$RHL/probes/gpu-paths.md:5` : aucun assemblage Linux vers Metal directement utilisable avec HyperLinux n'est confirmé ; le serveur vtest du fork UTM de virglrenderer (commit `71a67414…`) a deux obstacles de mémoire partagée : `vtest_shm.c:36-57` (memfd_create qui retourne -1) et `virglrenderer.c:1424-1447` (refus d'exporter un FD pour `VIRGL_RESOURCE_METAL_HEAP`).
- `$RHL/graphics-notes.md:7-15` : le chemin X11 de HyperLinux est réel (sockets Unix, SCM_RIGHTS, SysV SHM réel), `MAP_SHARED` sur fichier est une copie privée côté HyperLinux, et le chemin par défaut de hyper-linux-x11 présente un tampon CPU via NSImage.
- `$RHL/probes/hl-graphics-feasibility-20260929/README.md:13-25` : `winemac.so` et D3DMetal installés dans Voile sont des Mach-O x86_64 ; `d3d11.so` et `dxgi.so` du côté Unix sont des liens symboliques vers `libd3dshared.dylib`.
- `$DOC:150-183` : Neptune a deux transports (virtio-gpu et vtest) ; la présentation locale passe par X11 DRI3/Present dans `nptunix/npt_unix_wsi.c` ; le backend ARM de UTM `macos-next` choisit `libdxmt-native.dylib` pour `__APPLE__ && __aarch64__`.

## 1. Graphismes

### 1.1 Contraintes d'architecture de processus de D3DMetal

MESURE (lipo, otool, nm, file, 2026-10-02, lecture seule) :

| Fichier | Archs | Taille (octets) | SHA-256 | Remarque |
| --- | --- | --- | --- | --- |
| `VOILE/v26.3.0-4/lib/external/D3DMetal.framework/Versions/A/D3DMetal` | x86_64 seul | 7 578 032 | `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad` | GPTK « 4.0 beta 2 » selon `runtime.json` |
| `VOILE/v26.3.0-4/lib/external/libd3dshared.dylib` | x86_64 seul | 241 888 | `1582e7ceef7f495df4bebf7f06a49aef130233f8a2e9a8971e35affafeb76ec0` | colle CrossOver : ne lie que libc++, Foundation, CoreGraphics, libSystem, libobjc (il charge D3DMetal à l'exécution) |
| `VOILE/.../lib/wine/x86_64-unix/{d3d10,d3d11,d3d12,dxgi,nvapi64,nvngx-on-metalfx}.so` | liens symboliques vers `../../external/libd3dshared.dylib` | | | `winemac.so`, `winecoreaudio.so`, `winebus.so`, `winevulkan.so`, `ntdll.so` sont aussi x86_64 seul |
| `VOILE/.../lib/wine/x86_64-windows/{d3d11,dxgi,d3d12}.dll` | PE32+ x86-64 | | | côté Windows de la chaîne CrossOver |
| `UTM.app/Contents/Frameworks/D3DMetal.framework/Versions/A/D3DMetal` (UTM 5.0.5, `NEP/UTM.app`) | x86_64 seul | 5 263 904 | `287669456c09df4732e16a76baadc535724789a96bd4e5085f9614e6de6b1049` | version plus ancienne que celle de Voile |
| `UTM.app/.../d3dmetal-native.framework/Versions/A/d3dmetal-native` | universel x86_64 + arm64 | 510 240 | `4ff559614c5ecd53d5ad76c1e4ad893996fad9ea175bc65506ea01deddb0467a` | tranche x86_64 : exporte `D3D11CreateDevice`, `CreateDXGIFactory*`, `D3D12CreateDevice` et l'API embarqueur `dmn_*` (événements, fences partagées, fenêtres, partage mémoire, import de heap). Tranche arm64 : 0 symbole exporté, ne lie que libSystem. C'est une coquille vide. |

Les chemins `NEP`, `VOILE` sont définis plus haut.

Conséquences, par déduction directe des tranches ci-dessus et du commentaire de `NEP/virgl/server/render_worker.c:507-523` :

- Le chemin D3DMetal impose un processus hôte x86_64 sous Rosetta 2 (utilisateur, pas une VM). L'arm64 de `d3dmetal-native` ne contient rien.
- Un processus rendu x86_64 coexiste sans difficulté avec un Wine Linux x86-64 émulé par FEX en arm64 : les deux se parlent par socket Unix et mémoire partagée. Le coût de Rosetta s'applique au code du renderer (traduction D3D11 vers Metal, compilation de shaders), pas au jeu.
- Il faut vérifier que Rosetta 2 est installé sur la machine cible (à mesurer par un témoin `arch -x86_64`, voir section 5, témoin W0).
- D3DMetal est un binaire fermé d'Apple (framework, convertisseur Metal IR) : on peut modifier le wrapper `d3dmetal-native` et le transport, pas D3DMetal. La redistribution dépend de la licence GPTK (déjà notée dans `DOC:165`).

### 1.2 DXMT natif arm64

MESURE (même méthode) :

| Fichier | Archs | Taille | SHA-256 |
| --- | --- | --- | --- |
| `UTM.app/.../dxmt-native.framework/Versions/A/dxmt-native` | universel x86_64 + arm64 | 59 093 264 | `f7444460e48a8a4c8c4401385e1cd0bee9fbeceb76534cde4cda542fead76dfd` |
| `UTM.app/.../virglrenderer.1.framework/Versions/A/virglrenderer.1` | universel | 8 933 984 | `cdbe4531e7c4c4befb4316ed5a8e3a33dd27383d2f8e94c4a6a4cad4192c81c3` |
| `UTM.app/.../MoltenVK.framework/Versions/A/MoltenVK` | universel | 11 029 792 | `b96a76813884c04ebe7f13ddf4c4b305234224117a0688a56b81a398fc1641dd` |
| `UTM.app/.../vulkan_kosmickrisp.framework/Versions/A/vulkan_kosmickrisp` | universel | 23 902 384 | `3e438cca84029b162d273d98ac40ce5052ae710a2ac1e9aaae2b40b2c7ca99f6` |
| `UTM.app/.../epoxy.0.framework`, `EGL.framework`, `GLESv2.framework` (ANGLE) | universels | | |
| `NEP/render-server-real` (= `render-server-private`, même SHA-256 `d239f5926a9f0d89ae3cadcf60e3ae6610eb0dbcbe78f48e49f2240fe5d5cb39`) | universel x86_64 + arm64 | 7 947 824 | voir ci-contre |

- `dxmt-native` arm64 lie CoreFoundation, Metal, Foundation, CoreGraphics, ColorSync, MetalFX, libsqlite3, libSystem, libc++, AppKit, libobjc. Il ne dépend ni de D3DMetal ni de Rosetta. Exports mesurés (`nm -gU`) : `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2`, `D3D10CoreCreateDevice`, `D3D10CoreRegisterLayers`, `D3D11CoreCreateDevice`, `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`, `D3D11On12CreateDevice`, `DXGIGetDebugInterface1`, `dxmt_event_{clear,close,create,dup_fd,signal,wait}`. Il n'exporte pas `D3D12CreateDevice` : le bit de capacité D3D12 est donc faux avec DXMT (`NEP/virgl/src/neptune/npt_renderer.c:41-66`).
- Le serveur de rendu universel choisit la tranche par `posix_spawnattr_setarchpref_np` : x86_64 pour d3dmetal, arm64 pour dxmt (`NEP/virgl/server/render_worker.c:507-523`), variable `NPT_BACKEND=dxmt`.
- Binaires locaux utilisables pour un premier témoin sans rien compiler : la tranche arm64 de `render-server-real` et celle de `dxmt-native`. Les sources DXMT et d3dmetal-native ne sont PAS dans l'arbre local (section 1.5).
- `capset` DXMT (`npt_renderer.c:79-91`) : TBDR, MSAA_RTV_FORCED_SC1, MAP_DEFAULT_BUFFERS, SHADER_CACHE ; pas EXTENDED_RESOURCE_SHARING (DXMT échoue tout `SHARED_NTHANDLE`). D3DMetal : bit DXIL seul.

### 1.3 Chaîne client Neptune (DLL PE, bibliothèque Unix, vtest contre virtio-gpu)

LU dans `NEP/mesa-triton/src/virtio/neptune/` (arbre osy/mesa, branche neptune, `project mesa 26.1.0-devel` d'après `NEP/guest-meson-setup.log`) :

1. DLL PE `d3d11.dll`, `dxgi.dll`, `d3d12.dll` (meson.build:122-162), compilées en MinGW 10-posix dans un conteneur `ubuntu:22.04` amd64 (`NEP/Dockerfile.guest-build`, meson 1.9.2). Elles remplacent les DLL natives de Wine (`WINEDLLOVERRIDES='d3d11,dxgi=n;nptunix=b;...'`, `NEP/guest/guest-graphics.sh`). Tailles locales : 20 629 950 octets pour `d3d11.dll` et 20 591 698 pour `dxgi.dll` dans `NEP/client-original-44d3259/` (avec symboles de débogage). Les DLL sérialisent D3D11/DXGI par anneaux (`npt_ring.c`) et ne contiennent pas de rendu.
2. `nptunix.dll` (porteur PE, marqueur « Wine builtin DLL » patché à l'offset 0x40, `nptunix/meson.build:119-137`) et `nptunix.so` (ELF Linux x86-64 natif, chargé par le mécanisme unixlib de Wine). L'interface `__wine_unix_call_funcs` a 27 entrées (`nptunix/npt_unix.c:217-245`, énumération `npt_unixlib.h:222-251`) : connect, read, write, close, receive_fd (recvmsg + SCM_RIGHTS), mmap, munmap, wait_fd, 4 appels WSI, 9 appels virtgpu (DRM), enumerate_outputs, dup, 3 appels de singleton, alloc_obj_id. TOUT ce qui touche le noyau Linux passe par ce fichier, ce qui en fait la couture d'adaptation.
3. Choix du transport (`npt_device.c:279-292`) : virtgpu (`npt_renderer_virtgpu_wine.c` plus `nptunix/npt_unix_virtgpu.c`, ioctls DRM sur `/dev/dri/renderD*`) sauf si `NPT_DEBUG=force_vtest`, sinon vtest (`npt_renderer_vtest_wine.c`, 297 lignes). Hors VM, virtgpu échoue à l'ouverture et vtest prend le relais ; on peut forcer vtest.
4. Transport vtest (`npt_renderer_vtest_wine.c:221-296`, `npt_renderer_vtest_protocol.c`) : socket `AF_UNIX SOCK_STREAM` vers `$VTEST_SOCKET_NAME` (défaut `/tmp/.virgl_test`, `NEP/virgl/vtest/vtest_protocol.h:28` ; `sun_path` est limité à 104 octets sur Darwin), séquence `CREATE_RENDERER("neptune")`, `PING_PROTOCOL_VERSION`, version de protocole >= 3 exigée, `GET_PARAM(MAX_TIMELINE_COUNT)`, `GET_CAPSET(NPT_CAPSET_ID)` avec contrôle de `wire_format_version == NPT_PROTOCOL_WIRE_VERSION`, `CONTEXT_INIT`. Ensuite : `RESOURCE_CREATE_BLOB` (type `HOST3D_GUEST` mappable pour les SHM, `HOST3D` mappable plus partageable pour les blobs hôte), fd reçu par `recvmsg` SCM_RIGHTS puis `mmap(MAP_SHARED, offset 0)` ; `SUBMIT_CMD2` avec `RING_IDX` pour les commandes ; fence de présentation par un `SUBMIT_CMD2` vide avec `OUT_FENCE_FD` dont le fd revient par SCM_RIGHTS.
5. Présentation (WSI), seul point lié à X11 : `npt_renderer_wine_common.c:47-85` lit la propriété de fenêtre Wine `__wine_x11_whole_window` du HWND et échoue sans elle ; `nptunix/npt_unix_wsi.c` crée des pixmaps DRI3 à partir des fds dma-buf des blobs hôte (`xcb_dri3_pixmap_from_buffer_checked`, :141-145), présente par `xcb_present_pixmap[_synced]` avec options `ASYNC|COPY` (:246-247), lit `IDLE_NOTIFY` pour libérer les images (:197-215) et attend les fences par syncobj DRM (`/dev/dri/renderD128`, :104) ou par `poll` 30 s (:313-321). Le nombre d'images est limité à `NPT_WSI_MAX_IMAGES = 4` (`npt_unixlib.h:17`).
6. Mémoire : les images de la chaîne d'échange sont des blobs créés par l'hôte (`create_host_blob`) ; le client ne copie aucun pixel vers le serveur d'affichage. Taille arrondie à 4096 octets côté client (`npt_swapchain_common.c:396-399`).

INFERENCE : sur macOS le seul fichier à remplacer pour changer de plan de présentation est `nptunix/npt_unix_wsi.c` (396 lignes, 4 fonctions `wsi_init/present/drain/destroy`) plus la lecture de `__wine_x11_whole_window` dans `npt_renderer_wine_common.c`. Le reste (rings, blobs, fences, queries) est indépendant de X11. Voir sections 1.6 et 2.

### 1.4 Renderer hôte (code neptune de virglrenderer, d3dmetal-native, dxmt-native)

LU dans `NEP/virgl` (utmapp/virglrenderer, commit `482f9d8b8c2d2288efa10a027116216909d2c226` du 2026-09-01, `NEP/virgl-pin-commit.json` ; c'est le pin de `VIRGLRENDERER_COMMIT` dans `NEP/build_dependencies.sh` de UTM 5.0.5). La branche amont `macos-next` est plus récente (tête `5d26f605f50f8e22002ec6db5fb775e1992d4e96`, 2026-09-23, d'après la page de commits GitHub consultée ; elle ajoute au moins `0316f58` « darwin shared-texture POD version 2 », `6b52599` « pipe fallback for the fence eventfd (async fencing on macOS) », `8dfa5b5` canal SUBMIT_FENCE hors bande, `a2c0dba` placement des threads de ring).

Processus et rôles :

1. `virgl_test_server` (`NEP/virgl/vtest/vtest_server.c`, 5 021 lignes avec vtest, `vtest_renderer.c` seul : 3 073 lignes) accepte les connexions vtest. Avec `ENABLE_NEPTUNE` il force `VIRGL_RENDERER_NEPTUNE | VIRGL_RENDERER_RENDER_SERVER` (`vtest_server.c:315-318`).
2. `virgl_render_server` (`NEP/virgl/server/`, 3 265 lignes, plus `src/proxy/` 2 049 lignes) crée un worker par contexte. Sur Darwin le worker est lancé par `posix_spawn` (pas de `fork`, à cause des connexions XPC périmées qui cassent la compilation de shaders Metal, `server/render_worker.c:467-473`), avec préférence de tranche (arm64 pour `NPT_BACKEND=dxmt`, x86_64 sinon, `render_worker.c:509-524`).
3. Le worker contient le code Neptune hôte (`NEP/virgl/src/neptune/npt_*.c`, 10 793 lignes écrites à la main ; le décodeur de protocole est généré, environ 173 k lignes d'en-têtes). Il charge le backend par `dlopen` : `libdxmt-native.dylib` sur `__APPLE__ && __aarch64__`, `libd3dmetal-native.dylib` sur Apple x86 (`npt_library_names.h:25-32`), repère l'API embarqueur par le préfixe `dmn_` ou `dxmt_` (`npt_library.c:61-115`), et variables `NPT_D3D11_LIBRARY_PATH`, `NPT_DXGI_LIBRARY_PATH`, `NPT_D3D12_LIBRARY_PATH`, `NPT_BACKEND`, `NPT_WA_FLAGS`, `NPT_CAPSET_D3D12`, `D3DMETAL_FRAMEWORK_PATH` (les scripts `NEP/run-graphics.py:26-29` et `NEP/start-utm-private.py:8` les positionnent).
4. Le contexte Neptune (`npt_context.c`) offre : un anneau (`npt_ring.c`) pour les commandes sérialisées ; des ressources SHM créées côté hôte par `os_create_anonymous_file(size, "npt-shmem")` (`npt_context.c:1011` ; sur Apple c'est `shm_open(O_CREAT|O_EXCL)` avec un nom `/<tag>-<pid>-<nonce>-<i>` immédiatement `shm_unlink`, puis `ftruncate`, `NEP/virgl/src/mesa/util/anon_file.c:126-212`) ; des blobs « hôte » issus de textures partagées (`npt_shared.c`, descripteur POD `DMTX` version 2 : `fd`, largeur, hauteur, format DXGI, `stride`, `size`, `offset`) ; des événements Win32 émulés par `dmn_event_*` / `dxmt_event_*` (`npt_event.c:25-40, 117-130`) dont `*_event_dup_fd` donne un fd pollable ; des files de synchronisation (`npt_queue.c`) qui retirent les fences quand le GPU a fini ; des anneaux multiples avec threads de priorité différente (`npt_ring.c`).
5. Textures partagées sur macOS : d'après l'article UTM du 2026-07-24 (lu en ligne, non revérifié dans le code), une texture partagée est un fd `shm_open` mappé en `MTLBuffer` par `newBufferWithBytesNoCopy:` (mémoire unifiée d'Apple Silicon, textures linéaires seulement) et la fence partagée est une valeur de timeline écrite en mémoire partagée par `ClearUnorderedAccessViewUint`, sondée par le consommateur. Le README de `utmapp/dxmt` dit : « Win32-style events and cross-process shared textures over POSIX shm ». Localement, la structure `npt_darwin_shared_texture` (`npt_shared.c:76-90`) et `dxmt_shared_texture_handle` (récupérée dans `dxmt_native.h`) ont le même ABI (`'DMTX'` = `0x58544D44`, version 2).

Conséquence essentielle pour la présentation : le client reçoit pour chaque image de chaîne d'échange un fd de mémoire partagée linéaire (mappable) et non un dma-buf GPU. Le client peut donc LIRE les pixels (copie) ou transmettre le fd à un présentateur macOS, qui le mappe en MTLBuffer puis MTLTexture et le copie sur un `CAMetalLayer`, sans copie CPU.

Ce que l'hôte Neptune ne fait PAS : il ne crée aucune fenêtre. Aucun appel `dmn_window_*` ni `CAMetalLayer` dans `npt_*.c` (recherche textuelle) ; `dxmt_native.h` (commit `70aa109c`) n'exporte que des événements et le POD de texture partagée, pas de fenêtre. Dans le montage QEMU, la fenêtre est celle de QEMU/UTM (scanout virtio-gpu), alimentée par le serveur Xorg invité. Il n'y a donc pas aujourd'hui de code de présentation macOS réutilisable dans ces sources ; `dmn_window_create_for_view` et `dmn_window_get_metal_layer` existent dans `d3dmetal-native` (MESURE : symboles exportés de la tranche x86_64) mais Neptune ne les appelle pas.

Les sections suivantes de l'écosystème UTM décrivent des choix d'ingénierie utiles : `NPT_WA_FLAGS` active des contournements de convertisseur DXBC pour D3DMetal seul (`npt_library.c:240-250`), le capset annonce DXIL pour D3DMetal et TBDR, MSAA forcée, buffers par défaut mappables et cache de shaders pour DXMT (`npt_renderer.c:79-95`).

### 1.5 Ce qui existe localement en source contre en binaire, et comment cela a été construit

| Élément | Source locale | Binaire local | Construction |
| --- | --- | --- | --- |
| Client Neptune (`d3d11/dxgi/d3d12.dll`, `nptunix`) | OUI : `NEP/mesa-triton/src/virtio/neptune/` (client 23 495 lignes, `nptunix` 1 414 lignes, Triton UMD Windows 19 233 lignes inutile pour Wine, protocole généré 226 994 lignes) | OUI : `NEP/guest/client/d3d11.dll` (20 639 036 o), `dxgi.dll` (20 600 784 o), `libwinpthread-1.dll` (597 415 o), `NEP/guest/wine/x86_64-windows/nptunix.dll` (PE) et `x86_64-unix/nptunix.so` (ELF x86-64 avec symboles) | MinGW 10-posix dans `ubuntu:22.04` amd64 (`NEP/Dockerfile.guest-build`), meson 1.9.2, `NEP/build-win64.txt` ; builddirs `NEP/build-win64`, `NEP/build-triton` (sans `bin/`) |
| Hôte virglrenderer + Neptune + vtest + serveur de rendu | OUI : `NEP/virgl/` (commit `482f9d8b`) ; archive `NEP/virgl-source.tar.gz` | OUI : `NEP/render-server-real` (= `render-server-private`, universel x86_64+arm64, 7 947 824 o, construit par la CI de UTM : chaînes `/Users/runner/actions/runner-{1,4}/_work/UTM/UTM/sysroot-macOS-{arm64,x86_64}/libexec/virgl_render_server`) et `UTM.app/Contents/Frameworks/virglrenderer.1.framework` (universel) | recette UTM : `NEP/build_dependencies.sh:786` (`-Dtests=false -Dcheck-gl-errors=false -Dvenus=true -Dneptune=true -Dvulkan-dload=false -Drender-server-mode=process`) |
| `virgl_test_server` (vtest) pour macOS | source OUI (`NEP/virgl/vtest/`) | NON (aucun exécutable vtest trouvé ; UTM ne le livre pas) | non construit localement |
| D3DMetal | NON (fermé) | OUI : Voile (x86_64, 7 578 032 o) et copie privée dans `NEP/UTM.app` (x86_64, 5 263 904 o) | Apple GPTK |
| `d3dmetal-native` | NON (seul le README est copié : `hl-graphics-feasibility-20260929/d3dmetal-native-readme.md`) | OUI : `NEP/UTM.app/.../d3dmetal-native.framework` (tranche x86_64 utile, tranche arm64 vide) | `meson_build $D3DMETAL_REPO -Dtests=disabled`, x86_64 seulement (`build_dependencies.sh:900-905`), sinon dylib vide |
| DXMT natif | NON (pin UTM 5.0.5 : `bcdcf6da89af6c9fedeb2fc92b4367b23d5d6ed6` ; fork `utmapp/dxmt` `70aa109c…` du 2026-09-02) | OUI : `NEP/UTM.app/.../dxmt-native.framework` (universel, 59 093 264 o) | LLVM 15 minimal (`cmake -DLLVM_TARGETS_TO_BUILD=""`) puis `meson_darwin_build $DXMT_REPO -Dnative_llvm_path="$PREFIX"`, SDK min 13.0 (`build_dependencies.sh:876-899`) |
| MoltenVK, KosmicKrisp, ANGLE, libepoxy, loader Vulkan | NON | OUI (frameworks universels dans `NEP/UTM.app`) | `build_dependencies.sh` (inutiles pour un rendu D3D11 seul si epoxy est rendu optionnel, voir 1.6) |
| Outils de test D3D11 possédés | OUI : `NEP/d3d11-control.c`, `d3d11-present-control.c`, `d3d11-query-control.c`, `d3d11-view-resource-control.c`, `drm-caps-control.c`, `view-control-variants/*.c` | OUI : `NEP/guest/client/d3d11-*.exe` (PE) | MinGW |
| Spécification du protocole Neptune | généré dans les deux arbres (copies identiques vérifiées par `NEP/pinned-protocol-comparison.json`, 5 fichiers) | | amont `osy/neptune-protocol` (Apache-2.0, 25 commits d'après la page GitHub ; générateur Python/Mako, doc `docs/neptune_command_serialization.txt`) |

Notes de provenance : le client local est le commit `44d3259f888b65369fafb33ed68f7a5ec62966b0` du 2026-08-31 de `osy/mesa` (branche neptune), `NEP/selected-guest-revision.txt` et `NEP/mesa-sep1-commit.json` ; UTM 5.0.5 épingle `MESA_COMMIT=79bc850d884a1307356ff61c017e58901b90c7e2` (`NEP/dependency-pins.txt`). Les deux commits diffèrent : la différence entre le client réellement utilisé dans la session validée (`44d3259` plus le correctif GetData, voir `NEP/client-original-44d3259/sha256.json` et `NEP/view-resource-fix-evidence.json`) et le pin UTM n'a pas été mesurée ici.

### 1.6 Etapes exactes pour construire un processus renderer macOS autonome qui accepte une connexion vtest

MESURE : un spike jetable a été fait dans le scratchpad de la session (rien n'a été écrit dans `EXP`, ni dans les dépôts, ni dans le runtime installé ; ni UTM, ni QEMU, ni le jeu, ni EAC lancés ; seul `dlopen` du `dxmt-native` arm64 copié dans `NEP/UTM.app`). Outils : Xcode 27.0 (27A266a), Apple clang 21.0.0 (clang-2100.3.34.2), SDK MacOSX27.0, meson 1.12.1, ninja 1.13.2, pkg-config 3.0.7, PyYAML 6.0.3 installé dans `EXP/tools/venv`. Les sources étaient une copie de `NEP/virgl` (commit `482f9d8b`).

Résultats, dans l'ordre :

1. Le serveur vtest, le serveur de rendu et `libvirglrenderer` se compilent sur macOS 27 arm64 avec `-Dneptune=true -Dvtest=true` en environ 6 s (97 unités), sans EGL, sans GLX, sans Vulkan, sans ANGLE ; seul `libepoxy` est exigé par `meson.build:88` et se construit en quelques secondes (libepoxy 1.5.10, archive SHA-256 `a7ced37f4102b745ac86d6a70a9da399cc139ff168ba6b8002b4d8d43c900c15`, `-Dglx=no -Degl=no -Dx11=false`). Ce n'est donc pas la chaîne ANGLE/WebKit/MoltenVK de `build_dependencies.sh` qui est nécessaire pour un hôte Neptune seul.
2. Le serveur NON modifié accepte la connexion mais est inutilisable par le client Neptune : `vtest_shm_check()` échoue (`memfd_create` absent, `vtest_shm.c:33-63`), la négociation répond « protocol version 0 » (`vtest_renderer.c:884-926`) alors que le client exige >= 3 (`npt_renderer_vtest_wine.c:249-254`), et un blob `HOST3D_GUEST` échoue avec `vtest_new_shm called memfd_create which failed` puis `VTEST_CLIENT_ERROR_COMMAND_DISPATCH`.
3. Deux correctifs bornés (annexe A, 41 et 70 lignes de diff) suffisent pour la poignée de main complète : `vtest_shm.c` réutilise `os_create_anonymous_file` (POSIX shm) sous `__APPLE__` ; `vtest_renderer.c` annonce `MAX_TIMELINE_COUNT = 64` et remplace le fd de fence « déjà signalé » (`eventfd`, absent) par un `pipe` dont l'extrémité d'écriture est écrite quand la fence du contexte se retire.
4. Avec ces correctifs, un client macOS natif de 150 lignes (scratch, non livré) a obtenu : protocole 4, `MAX_TIMELINE_COUNT=64`, capset Neptune `wire_format_version=1`, `caps_flags=0x36` (= TBDR, MSAA_RTV_FORCED_SC1, MAP_DEFAULT_BUFFERS, SHADER_CACHE, identique au calcul de `npt_renderer.c:79-91` pour DXMT), création du contexte Neptune dans un worker arm64 qui a chargé `dxmt-native` (log : `npt: created Neptune context 1`), blob `HOST3D_GUEST` de 64 Kio reçu par SCM_RIGHTS puis `mmap(MAP_SHARED)` et écriture réussis.
5. Latences mesurées (processus natifs, aucune émulation) : 200 requêtes `SUBMIT_CMD2` vides avec `RING_IDX|OUT_FENCE_FD` : 200/200 fds pollables devenus lisibles, minimum 0,017 ms, médiane 0,027 ms, p95 0,039 ms, maximum 0,079 ms ; création de blob + SCM_RIGHTS + `mmap` + écriture + `munmap` + `RESOURCE_UNREF` : médiane 0,034 ms (64 Kio), 0,031 ms (1 Mio), 0,049 ms (16 Mio).
6. NON mesuré : création de périphérique D3D11 par le flux d'anneau, rendu, tranche x86_64 et D3DMetal, présentation, client Wine, FEX.

Pièges macOS mesurés par un petit programme C (scratch) et importants pour la couche mémoire de FEX (pages hôte de 16 Kio, `hw.pagesize=16384`) :

| Opération | Résultat mesuré |
| --- | --- |
| `shm_open` avec un nom de 40 caractères | `ENAMETOOLONG` (limite 31, `PSHMNAMLEN`) |
| `ftruncate(fd, 12288)` sur un objet shm | accepté, `st_size` devient 16384 |
| second `ftruncate` sur le même objet | `EINVAL` (redimensionnement unique) |
| `mmap(len=12288, off=0)` | accepté, adresse alignée sur 16 Kio, écriture à l'octet 12287 correcte |
| `mmap(len=8192, off=4096)` | `EINVAL` (un décalage de mapping doit être multiple de 16 Kio) |
| SysV SHM : `kern.sysv.shmmax=4194304`, `shmall=1024`, `shmseg=8`, `shmmni=32` | segment maximal 4 Mio, total 1024 pages : un framebuffer 1080p (8,3 Mo) ne passe pas |

Étapes à suivre pour reproduire et durcir (la partie « exécution » a été faite ; le reste est la recette) :

```sh
# 0. Prérequis (déjà présents) : Xcode 27, meson, ninja, pkg-config. Python : PyYAML dans le venv privé.
EXP=/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-wine-fex-darwin-20261002
PFX=<préfixe privé>            # jamais /opt, jamais brew install

# 1. libepoxy sans plateformes GL
meson setup build libepoxy-1.5.10 --prefix=$PFX -Dtests=false -Dglx=no -Degl=no -Dx11=false --buildtype=release
ninja -C build && meson install -C build

# 2. virglrenderer épinglé (copie de NEP/virgl ; ne pas modifier l'original) puis annexe A
PATH=$EXP/tools/venv/bin:$PATH meson setup build-arm64 virgl --prefix=$PFX \
  --pkg-config-path=$PFX/lib/pkgconfig -Dneptune=true -Dvenus=false -Dvtest=true \
  -Drender-server-mode=process -Drender-server-worker=process -Dtests=false \
  -Dcheck-gl-errors=false --buildtype=release
ninja -C build-arm64            # produit vtest/virgl_test_server, server/virgl_render_server, src/libvirglrenderer.1.dylib

# 3. Exécution (socket dans un dossier privé 0700, chemin < 104 octets)
RENDER_SERVER_EXEC_PATH=$PWD/build-arm64/server/virgl_render_server NPT_BACKEND=dxmt \
NPT_D3D11_LIBRARY_PATH=<libdxmt-native> NPT_DXGI_LIBRARY_PATH=<libdxmt-native> NPT_D3D12_LIBRARY_PATH=<libdxmt-native> \
  build-arm64/vtest/virgl_test_server --no-virgl --no-fork --socket-path $TMPDIR/gda/s

# 4. Tranche D3DMetal (NON exécutée ici ; recette de l'article UTM du 2026-07-24 et de build_dependencies.sh:900-905)
#    cross-file clang -arch x86_64 ; epoxy x86_64 statique ; -Dvenus=false -Dneptune=true -Dvtest=false -Ddefault_library=static ;
#    lipo -create server arm64 + x86_64 -> RENDER_SERVER_EXEC_PATH universel ; NPT_BACKEND=d3dmetal ;
#    D3DMETAL_FRAMEWORK_PATH=<D3DMetal.framework> ; libd3dmetal-native.dylib x86_64 (meson, -Dtests=disabled).
```

Pour un processus hôte « autonome » du produit, le spike montre qu'on peut partir de `virgl_test_server` corrigé. Ce qu'il faut encore décider ou écrire (INFERENCE, estimations en section 2.5) :

- Remplacer `virgl_test_server` par un serveur de la plateforme qui garde les 9 commandes vtest utilisées par le client (CREATE_RENDERER, PING_PROTOCOL_VERSION, PROTOCOL_VERSION, GET_PARAM, GET_CAPSET, CONTEXT_INIT, RESOURCE_CREATE_BLOB, RESOURCE_UNREF, SUBMIT_CMD2) et y ajouter le canal de présentation. Le serveur vtest actuel fait 5 021 lignes dont une grande part est du GL/virgl inutile ici.
- Authentifier le pair sur la socket (`LOCAL_PEERCRED` / `getpeereid`), un thread par connexion et non `fork` (les connexions XPC héritées cassent Metal, `render_worker.c:467-473`), cycle de vie : arrêt à la fin du client, nettoyage des shm.
- Fences : le serveur vtest ne donne pas de vraie fence GPU sans correctif (le chemin d'origine renvoie un fd déjà signalé quand le contexte ne partage pas de fence, `vtest_renderer.c:2378-2392`) ; avec le correctif de l'annexe A le fd devient lisible quand la fence de timeline se retire, ce qui reflète la progression du worker mais PAS encore l'achèvement GPU des écritures dans la texture partagée. Pour une présentation sans déchirure, la fin d'écriture GPU doit être établie par l'événement D3D11 apparié (`npt_queue.c`, `npt_event.c`) ou par une fence partagée en mémoire (article UTM) : à qualifier par le témoin W5 (section 5).
- Signature : binaires arm64 signés ad hoc suffisent pour le spike ; le worker x86_64 de D3DMetal tourne sous Rosetta sans entitlement particulier d'après la session QEMU du 2026-09-30 (INFERENCE, non revérifiée ici).

## 2. Présentation, affichage et entrées pour « Wine Linux x86-64 sous FEX sur macOS »

### 2.0 Ce que le guest QEMU validé utilisait (Xorg, openbox, curseur)

LU et MESURE dans `NEP/guest/guest-graphics.sh`, `NEP/guest/xorg.conf`, `NEP/guest/openbox.xml`, `NEP/guest/guest-game.sh`, `NEP/SailOver-EAC-Graphics.utm/config.plist` et dans les sorties non privées de `NEP/game-query-fix-20260930T211549Z/` (les fichiers `*.private.log` n'ont pas été ouverts) :

| Élément | Valeur | Source |
| --- | --- | --- |
| Noyau et distribution invités | Debian 13, noyau `6.12.107+deb13-amd64`, modules d'entrée évdev/usbhid/xhci/psmouse préparés | `NEP/input-modules.json`, `NEP/prepare-input-modules.py` |
| Serveur X | Xorg 21.1.16 (`xserver-xorg-core 2:21.1.16-1.3+deb13u4`), écran 1280x720 profondeur 24, `AutoAddDevices false`, lancé en racine sur `:89` avec cookie `-auth` | `guest-graphics.sh:51-53`, `xorg.conf` |
| Pilote | `modesetting` sur `/dev/dri/card0` (virtio-gpu), `AccelMethod glamor`, `DRI 3`, journal : « glamor X acceleration enabled on virgl » | `xorg.conf`, `outputs/xorg.log` |
| Curseur | `SWcursor false` (curseur matériel, taille maximale 64x64) ; le défaut précédent dessinait un rectangle blanc autour du pointeur, corrigé et validé par un témoin possédé de 120 images | `NEP/validation.json` (« cursor_fix »), `NEP/cursor-hardware-control-20260930T211146Z/result.json` |
| Entrées | `evdev` explicite : clavier `AT Translated Set 2 keyboard` lié à `/dev/input/sailover-keyboard` (XkbLayout `us`), pointeur `QEMU QEMU USB Tablet` (absolu) lié à `/dev/input/sailover-pointer` ; une souris PS/2 relative est aussi présente | `xorg.conf`, `outputs/kernel-input-devices.txt`, `guest-graphics.sh:12-25` |
| Gestionnaire de fenêtres | openbox 3.6.1-12 (`libobt2v5_3.6.1-12`), `focusNew yes`, `followMouse yes`, `focusDelay 0`, `raiseOnFocus no`, `decor no` pour toutes les fenêtres ; menu absent (« Unable to find a valid menu file »). Le script note « A real X11 window manager supplies focus and WM_TAKE_FOCUS delivery » ; après lancement, la fenêtre focalisée est `0x1200001` (`revert 2`) | `openbox.xml`, `guest-graphics.sh:82-92`, `outputs/x11-focus.json`, `outputs/openbox.log` |
| Mesa | 25.0.7 (`libgl1-mesa-dri`, `libegl-mesa0`, `mesa-libgallium`), uniquement pour glamor ; le rendu du jeu ne passe pas par Mesa | `NEP/xorg-packages/` |
| Repli sans GPU | `Xvfb :89 -screen 0 640x480x24` | `guest-graphics.sh:58` |
| Wine | GE-Proton11-7 x86_64 (`RHL/ge-proton/GE-Proton11-7-x86_64`) ; `WINEDLLOVERRIDES='d3d11,dxgi=n;nptunix=b;...'` ; contrôles graphiques avec `WINEESYNC=0 WINEFSYNC=0 WINE_DISABLE_FAST_SYNC=1`, jeu avec `WINEFSYNC=1` | `guest-graphics.sh:33-34`, `guest-game.sh` |
| Mode de fenêtre du jeu | `-windowed -ResX=800 -ResY=600` : ni plein écran ni capture exclusive de la souris n'ont été validés dans la session de 91,54 s | `guest-launch.py` (arguments ajoutés au lancement) |
| Audio | aucun périphérique (`Sound = []` dans `config.plist`, aucun mot-clé audio dans les scripts invités) ; la table des modules du jeu montre pourtant `winealsa.so`, `libasound.so.2.0.0`, `libpulse.so.0.24.3`, `mmdevapi.dll`, `xaudio2_9.dll`, `dsound.dll`, `winmm.dll` | `outputs/game-process-maps.txt` |
| Manette | aucune manette présente ; le jeu a quand même chargé `xinput1_3.dll` (Wine), `dinput8.dll`, `hid.dll`, `setupapi.dll` | `outputs/game-process-maps.txt` |
| Bibliothèques X chargées par le processus du jeu | libX11, libXi, libXrandr, libXrender, libXcursor, libXfixes, libXcomposite, libXinerama, libXxf86vm, libXext, libxcb-dri3, libxcb-present | `outputs/game-process-maps.txt` |

Résultat d'ensemble de ce guest : matchmaking public, session protégée de 91,54 s, sortie volontaire, limites déclarées « glyphes manquants dans l'interface, hitches sous QEMU TCG » (`NEP/game-query-fix-20260930T211549Z/validation.json`). Le chemin d'affichage était donc : jeu D3D11 -> client Neptune -> hôte Metal -> texture partagée -> pixmap DRI3 dans Xorg invité -> composition Xorg/openbox -> scanout virtio-gpu -> fenêtre de QEMU. La seule partie de cette chaîne qui existe aussi sans VM est le début (jusqu'à la texture partagée).

### 2.1 Besoins de Rocket League (plein écran, capture souris, raw input, manette)

MESURE (installation en lecture seule, noms de fichiers et lignes de configuration par défaut) :

- `Binaries/Win64/` contient `XAudio2_7.dll`, `X3DAudio1_7.dll`, `XAPOFX1_5.dll`, `xaudio2_9redist.dll`, `xinput1_3.dll`, `d3dcompiler_47.dll`, `discord_game_sdk.dll`, `EOSSDK-Win64-Shipping.dll` (liste de `ls`, aucun fichier ouvert).
- `TAGame/Config/DefaultSystemSettings.ini` : `Fullscreen=False` par défaut ; `DefaultInput.ini` : liaisons clavier/souris avec les axes `MouseX`/`MouseY` (caméra, vue libre), `MouseScrollUp/Down`, boutons de souris, `bEnableMouseSmoothing=false`, et manettes de types `IPT_360`, `IPT_PS4`, `IPT_XBOX_ONE` avec zones mortes.
- Dans la session QEMU, le processus du jeu a chargé `dinput8.dll`, `xinput1_3.dll`, `hid.dll`, `setupapi.dll`, `dsound.dll`, `xaudio2_9.dll`, `mmdevapi.dll` (voir 2.0).

INFERENCE (à confirmer par un journal Wine `+rawinput,+cursor,+keyboard,+dinput,+xinput` lors d'un essai de jeu) :

| Besoin | Pourquoi | Conséquence de conception |
| --- | --- | --- |
| Fenêtre fenêtrée redimensionnable, 800x600 à la résolution native | défaut du jeu, validé à 800x600 | un hôte de fenêtres qui supporte création, déplacement, redimensionnement, ordre Z, focus, titre |
| Plein écran | option du jeu, côté Neptune c'est une fenêtre sans bordure en `HWND_TOPMOST` couvrant le rectangle du moniteur (`npt_swapchain.c:520-560, 651-684`), sans changement de mode matériel | pas de changement de mode nécessaire pour un premier plein écran ; le présentateur doit pouvoir couvrir l'écran (niveau de fenêtre au-dessus de la barre de menus, Dock masqué) et mettre à l'échelle |
| Liste de modes DXGI | `IDXGIOutput::GetDisplayModeList` est fabriquée côté client à partir de DRM, sinon une sortie synthétique 1920x1080 (`npt_overrides_dxgi_adapter.c:66-98`, `nptunix/npt_display_drm.c`) | remplacer `npt_unix_enumerate_outputs` par un appel à l'hôte qui renvoie les vrais modes macOS (ou une liste choisie) |
| Souris relative et capture | axes `MouseX/MouseY` pour la caméra ; `dinput8` chargé ; typiquement `ClipCursor` et recentrage par `SetCursorPos`, ou `RegisterRawInputDevices` | l'hôte doit fournir des deltas non accélérés (par exemple `GCMouse` ou `CGEventGetIntegerValueField(kCGMouseEventDeltaX)` avec `CGAssociateMouseAndMouseCursorPosition(false)`) et gérer le piège connu du saut de curseur (`winemac.drv/cocoa_cursorclipping.m:31-60, 301-370` utilise un `CGEventTap`, qui exige la permission Accessibilité de macOS, ou `-[NSWindow setMouseConfinementRect:]` en variante) |
| Entrées claviers par scancode | UE3 lie des touches ; AZERTY/QWERTZ fréquents | l'hôte envoie des scancodes physiques ; la disposition Windows vient des tables `KBDTABLES` de win32u (retour NULL de `pKbdLayerDescriptor` donne `kbdus`, `input.c:1100`) ou d'une table construite depuis la disposition macOS (section 4) |
| Manettes XInput et PS4 | `xinput1_3`, `hid`, `setupapi` chargés ; types IPT_PS4/360/ONE | pont HID vers `winebus.sys` (section 4) |
| Audio XAudio2 | `xaudio2_9`, `mmdevapi` chargés | pilote audio mmdevapi distant ou null (section 3) ; absent de la validation QEMU |
| Curseur | curseur système et personnalisé, masqué en jeu | `pSetCursor` / `pClipCursor` / `pSetCursorPos` / `pGetCursorPos` transmis à l'hôte |
| Focus et activation | `WM_ACTIVATE`, `WM_TAKE_FOCUS` côté X11 ; le script QEMU insiste sur un vrai gestionnaire de fenêtres | `pActivateWindow` et événements de focus de l'hôte |

### 2.2 Option A : serveur X11 sur macOS

Principe : Wine Linux x86-64 sous FEX garde `winex11.drv` ; la socket X est celle d'un serveur natif macOS. C'est la seule option où le côté Wine reste celui du montage QEMU validé (même `winex11.so`, mêmes bibliothèques X, même openbox si on le garde). Seul `nptunix/npt_unix_wsi.c` doit changer, parce que DRI3 n'existe pas sur macOS.

Serveurs candidats, avec ce qui est établi :

| Variante | Établi | Limite |
| --- | --- | --- |
| A1 XQuartz 2.8.6 visible | paquet officiel notarisé, SHA-256 `9ac35a505095bfbd3009c3b4772f0c6421e2f79c4210ab908459270d1c447909`, extrait sans installateur (`RHL/x11/README.md:5-12`) | MESURE : plantage à l'initialisation de GLX (`CGLQueryRendererInfo` dans `OpenGL.framework`, `RHL/x11/XQuartz-visible-crash-summary.json`) ; avec `-extension GLX` le processus a dépassé l'initialisation mais a signalé des problèmes de clavier et de connexion `pbproxy`, puis a été arrêté ; aucun client visible validé |
| A2 Xvfb (même distribution, X.Org 21.1.23) | MESURE : serveur privé relocalisé, `xdpyinfo` natif et Linux sous HyperLinux OK, `vkcube` lavapipe capturé (`RHL/x11/cube-shared-io-1.png`) ; 21 extensions (liste ci-dessous) | écran hors écran : il faut une visionneuse macOS qui lit le framebuffer et injecte les entrées par XTEST ; pas de DRI3, pas de GLX |
| A3 serveur X écrit sur mesure | aucun | `winex11.drv` appelle 174 points d'entrée Xlib/extensions distincts (MESURE par `grep` sur `dlls/winex11.drv/*.c`, sur-approximation) ; un serveur fiable les couvre tous plus XKB, XI2, RANDR, RENDER, SHAPE, XFIXES, COMPOSITE, MIT-SHM, EWMH. Non recommandé. |
| A4 serveur X avec DRI3/Present sur shm | aucun | variante de A2 ou A1 : ajouter DRI3 (`PixmapFromBuffer` adossé à la shm linéaire) et laisser `nptunix.so` inchangé ; le code générique DRI3/Present d'Xorg existe, le DDX doit fournir les rappels |

Extensions X : MESURE sur Xvfb de XQuartz 2.8.6 (`RHL/x11/xdpyinfo-private-89.txt`) : BIG-REQUESTS, Composite, DAMAGE, DOUBLE-BUFFER, Generic Event Extension, MIT-SCREEN-SAVER, MIT-SHM, Present, RANDR, RECORD, RENDER, SHAPE, SYNC, X-Resource, XC-MISC, XFIXES, XINERAMA, XInputExtension, XKEYBOARD, XTEST, XVideo. Absentes : GLX, DRI3, XFree86-VidModeExtension, DPMS. Ce que `winex11.drv` (Wine 11.0) en fait, LU :

| Extension | Usage dans `winex11.drv` | Si absente |
| --- | --- | --- |
| noyau + BIG-REQUESTS | tout | inutilisable |
| XKEYBOARD | `XkbUseExtension`, `XkbKeycodeToKeysym` (`keyboard.c`, `x11drv_main.c:679, 758`) | repli sur le mappage noyau, dispositions dégradées |
| XInputExtension 2.x | souris brute (`mouse.c:1812-1816` : `XIQueryVersion`, `xinput2_available`) | repli sur événements noyau, pas de deltas bruts |
| RANDR >= 1.2 / 1.4 | moniteurs et modes (`xrandr.c:1242-1683`) | repli Xinerama puis écran unique |
| RENDER | texte et alpha GDI (`xrender.c:353`) | GDI logiciel via `XPutImage` |
| XFIXES | surveillance du presse-papiers (`clipboard.c:2131-2137`) | pas de notification de presse-papiers |
| Composite, Xinerama, XVidMode, SHAPE, MIT-SHM | options (`x11drv_main.c:553`, `xinerama.c:91`, `xvidmode.c`, `bitblt.c:1574-1657`) | dégradations locales |
| EWMH/ICCCM par un gestionnaire de fenêtres | `_NET_ACTIVE_WINDOW`, `_NET_SUPPORTED`, `WM_TAKE_FOCUS`, `_NET_WM_STATE_FULLSCREEN` (`event.c:642, 738, 888`, `window.c:1296, 1405, 1883`) | focus par `XSetInputFocus` direct, plein écran sans cadre à gérer soi-même |
| DRI3 + Present | uniquement via `nptunix` (`npt_unix_wsi.c:88-120`) | présentation Neptune impossible sans remplacement |

MIT-SHM de `winex11.drv` utilise SysV (`shmget(IPC_PRIVATE, ...)`, `bitblt.c:1614`). MESURE sur cette machine : `kern.sysv.shmmax=4194304`, `shmall=1024`, `shmseg=8` : un tampon 1080p (8,3 Mo) échoue et Wine retombe sur `XPutImage`.

Présentation Neptune sous A : (a) A-lite : `nptunix` mappe la shm de l'image (déjà accessible, c'est un fd POSIX) et pousse les pixels par `xcb_put_image` (copie CPU : 1920x1080x4 x 60 i/s = 498 Mo/s dans la socket X, plus la copie du serveur) ; utilisable pour valider la fonction et en 720p, pas comme solution finale ; (b) A4 ; (c) pont vers un présentateur macOS externe : impossible de relier l'identifiant de fenêtre X à une `NSWindow` d'XQuartz sans hook dans le serveur.

Besoins de Rocket League sous A : plein écran = `_NET_WM_STATE_FULLSCREEN` donc un gestionnaire (openbox x86-64 sous FEX, déjà utilisé en QEMU) ; capture de souris = `XGrabPointer`/`XIConfineTo` + deltas XI2 bruts, dont la fidélité sous XQuartz n'est pas établie (non testé) ; la manette ne passe pas par X (section 4).

Estimation (INFERENCE, une personne connaissant Wine et X) : A-lite sur XQuartz ou Xvfb + visionneuse : 2 à 4 semaines pour une fenêtre visible avec entrées ; A4 : 8 à 12 semaines ; A3 : 12 à 20 semaines. Risques : stabilité d'XQuartz sur macOS 27, isolation des préférences (le test de RHL a créé `~/Library/Preferences/org.xquartz.X11.plist`, supprimé ensuite, `RHL/x11/README.md:44`), latence d'entrée, plein écran sans vrai changement de mode, absence de contrôle fin du curseur.

### 2.3 Option B : pilote d'affichage Wine sur mesure parlant à un hôte macOS par socket

Principe : un pilote `wine<nom>.drv` (sélectionné par `HKCU\Software\Wine\Drivers\Graphics=<nom>`, liste par défaut `mac,x11,wayland` dans `programs/explorer/desktop.c:43`, lecture du registre et `LoadLibraryW(L"wine%s.drv")` dans `load_graphics_driver`, `desktop.c:996-1030`) dont la partie Unix est un ELF x86-64 sous FEX qui parle par socket Unix à un processus hôte macOS (« hôte d'affichage ») propriétaire des `NSWindow`, du curseur, des événements et du presse-papiers. `win32u` remplace tout membre NULL par la fonction du pilote nul (`SET_USER_FUNC`, `dlls/win32u/driver.c:1350-1420`). Interface : `struct user_driver_funcs` avec `WINE_GDI_DRIVER_VERSION 108` (`include/wine/gdi_driver.h:221, 364-446` de voile-wine, identique à `wine-mirror/wine` tag `wine-11.0`, vérifié en ligne) : 64 membres dont 2 ajouts CrossOver (`pSet/GetCurrentProcessExplicitAppUserModelID`), et seulement 21 sont renseignés par `winewayland.drv` (`waylanddrv_main.c:36-58`).

Modèle d'événements à copier : `winewayland.drv` n'utilise pas `pProcessEvents` ; il lance à l'initialisation du module PE un thread (`dllmain.c:30, 109`) qui appelle l'unixlib `read_events` (boucle bloquante sur la socket, `waylanddrv_main.c:105-113`) et injecte les entrées dans win32u. Le pilote Option B suit ce modèle.

Éléments à écrire :

| Composant | Contenu | Référence de taille (MESURE) |
| --- | --- | --- |
| Pilote Wine (PE + unixlib ELF) | fenêtres (`pCreateWindow`, `pDestroyWindow`, `pWindowPosChanging/Changed`, `pShowWindow`, `pSetWindowStyle`, `pSetWindowText`, `pActivateWindow`, `pSetCapture`), surface GDI en shm (`pCreateWindowSurface`), curseur (`pSetCursor`, `pGetCursorPos`, `pSetCursorPos`, `pClipCursor`), moniteurs et modes (`pUpdateDisplayDevices`, `pChangeDisplaySettings`), clavier (`pKbdLayerDescriptor`, `pReleaseKbdTables`), presse-papiers (`pClipboardWindowProc`, `pUpdateClipboard`, optionnel au début), transport RPC et thread de lecture | `winewayland.drv` : 8 249 lignes dont `wayland_surface.c` 1 315 (propre à Wayland), `wayland_pointer.c` 1 096, `wayland_keyboard.c` 992, `wayland_data_device.c` 943 (presse-papiers), `window.c` 876, `window_surface.c` 531, `display.c` 308, OpenGL 278 + Vulkan 121 (inutiles ici) |
| Hôte d'affichage macOS (Swift/Objective-C) | une `NSWindow` + `NSView` + `CAMetalLayer` par HWND de premier niveau, ordre Z, focus, niveau plein écran, modes (`CGDisplay*`), événements souris/clavier/molette convertis en scancodes et deltas, curseur (`NSCursor`), confinement/capture, `NSPasteboard`, présentateur Metal (section ci-dessous) | `winemac.drv` côté Cocoa : `cocoa_window.m` 4 209, `cocoa_app.m` 3 067, `cocoa_display.m` 1 050, `cocoa_event.m` 795, `cocoa_cursorclipping.m` 507, `cocoa_clipboard.m` 248 ; la frontière C/Cocoa de `winemac.drv` compte 90 fonctions `macdrv_*` (`macdrv_cocoa.h`, 617 lignes) |
| Canal d'entrée/sortie | socket Unix privée (dossier 0700, chemin < 104 octets, authentification du pair), messages binaires bornés, un fd `SCM_RIGHTS` par image partagée | transport vtest mesuré en 1.6 : médiane 27 µs par aller-retour de fence en natif |

Réutilisation de `winemac.drv` : la moitié Cocoa est prévue pour être appelée par la moitié C via `macdrv_cocoa.h`. Deux façons de l'exploiter : (i) copier des morceaux de `cocoa_*.m` dans l'hôte (capture de souris, résolution des pièges d'`NSEvent`, `CGEventTap`, gestion des modes) ; (ii) compiler la moitié C de `winemac.drv` pour Linux en remplaçant `macdrv_*` par des appels RPC. (ii) demande de retirer les types CoreFoundation/CoreGraphics (`CFArrayRef`, `CFDataRef`, `CGRect`) des fichiers C (`window.c` 2 254, `keyboard.c` 1 958, `display.c` 1 156, `clipboard.c` 1 526, `mouse.c` 964, `event.c` 538) ; aucune mesure n'a été faite de l'effort réel de ce portage.

Présentation Neptune sous B (c'est le point que l'option contrôle entièrement) :

1. `npt_renderer_wine_common.c:47-85` lit aujourd'hui la propriété `__wine_x11_whole_window`. Sous B, le pilote publie à la place une propriété analogue (identifiant de fenêtre hôte) ou `nptunix` interroge directement le pilote.
2. `nptunix.so` et la partie Unix du pilote vivent dans le même processus : `nptunix.so` peut résoudre par `dlsym` (après `dlopen(..., RTLD_NOLOAD)`) un point d'entrée exporté par la bibliothèque Unix du pilote et réutiliser sa connexion, sans seconde socket ni seconde authentification, et avec un ordre cohérent entre événements de fenêtre et images (INFERENCE de conception, non essayée).
3. Messages minimaux : `WSI_INIT{hwnd, n, width, height, format, strides[n]}` + `n` fds `SCM_RIGHTS` ; `WSI_PRESENT{hwnd, index, serial, sync_interval}` + fd de fence optionnel ; `WSI_DESTROY` ; retour hôte -> client : `RELEASE{hwnd, index}` (équivalent de `IDLE_NOTIFY`, compté par image comme l'exige `npt_unixlib.h:90-101`), `OCCLUDED`, `DISPLAY_LINK{timestamp}` pour le rythme.
4. Hôte : chaque fd shm est mappé (`mmap`), enveloppé par `newBufferWithBytesNoCopy:` (adresse et longueur multiples de 16 Kio) puis `newTextureWithDescriptor:offset:bytesPerRow:` (lignes de `dxmt-native` confirmées par `strings`), copié par un passage de rendu vers le drawable du `CAMetalLayer` avec mise à l'échelle ; `RELEASE` est émis quand le tampon de commandes est terminé. Cette API n'a pas été exercée ici.
5. Le même chemin « présenter une image shm sur une fenêtre » sert aux surfaces GDI du pilote (fenêtres du lanceur, boîtes de dialogue d'EAC) : un seul présentateur pour deux usages.
6. Mode de présentation : `SyncInterval` du jeu devient `CAMetalLayer.displaySyncEnabled`, `maximumDrawableCount` 2 ou 3 ; la version QEMU forçait `ASYNC|COPY` (`npt_unix_wsi.c:246-247`) sans vrai vsync.

Besoins de Rocket League sous B : plein écran par fenêtre sans bordure couvrant l'écran (déjà conçu côté Neptune) avec mise à l'échelle Metal et modes DXGI fournis par l'hôte ; souris relative non accélérée par `GCMouse` ou `CGEvent` (la permission Accessibilité n'est exigée que pour le `CGEventTap` de confinement) ; clavier par scancodes physiques ; manette par pont HID (section 4) ; tout le curseur (forme, masqué, saut) passe par le même canal.

Estimation (INFERENCE ; hypothèse : une personne connaissant Wine et AppKit) : pilote Wine 4 000 à 5 500 lignes, hôte 4 000 à 6 000 lignes ; 4 à 6 semaines pour fenêtre unique + entrées + curseur + présentation Metal ; 8 à 12 semaines avec plein écran, multi-moniteurs, presse-papiers, surfaces GDI, ergonomie. Risques : surface de code neuve (bogues de synchronisation focus/ordre Z), dépendance à des API AppKit sensibles au fil principal (D3DMetal le dit dans ses chaînes : `dmn_window_create_for_view called off the main thread`), permission Accessibilité pour le confinement par `CGEventTap`, comportement de FEX sur les sockets et `SCM_RIGHTS` (traduction des structures `msghdr`/`cmsghdr` Linux vers Darwin, qui diffèrent).

### 2.4 Option C : winewayland.drv avec un compositeur Wayland minimal sur macOS

Principe : `winewayland.drv` (Wine 11.0, présent dans `voile-wine/dlls/winewayland.drv`, 8 249 lignes) est utilisé tel quel dans le Wine Linux émulé ; la socket Wayland est servie par un compositeur macOS.

Dépendances de `winewayland.drv` d'après `Makefile.in` de `wine-mirror/wine` tag `wine-11.0` : libwayland-client, wayland-egl, EGL, xkbcommon, xkbregistry ; protocoles inclus : cursor-shape-v1, pointer-constraints-unstable-v1, relative-pointer-unstable-v1, text-input-unstable-v3, viewporter, wlr-data-control-unstable-v1, xdg-output-unstable-v1, xdg-shell, xdg-toplevel-icon-v1. Le compositeur doit au minimum offrir `wl_compositor`, `wl_shm`, `wl_seat`, `wl_output`, `xdg_wm_base`, `zxdg_output_manager_v1`, `wp_viewporter`, `zwp_relative_pointer_manager_v1` et `zwp_pointer_constraints_v1` (ces deux derniers pour la souris brute et le confinement) ; une recherche sur le Web (moteur de recherche, non vérifiée dans le code) cite aussi `zxdg_decoration_manager_v1` pour les décorations serveur.

Compositeurs macOS existants (sources primaires lues partiellement) : `J-x-Z/cocoa-way` (Rust/Smithay, rendu Metal, « damage-aware SHM uploads », GPL-3.0, mentionne un relais audio CoreAudio sans en préciser le mécanisme, liste de protocoles non fournie par le README) ; `jwinarske/wayland-darwin` (libwayland via epoll-shim, backend Cocoa de wlroots, rendu Metal/IOSurface, d'après le résultat de recherche ; le README n'a pas pu être récupéré, 404 sur `main/README.md`) ; `owl-compositor/owl` (Objective-C, ancien). Aucun n'a été construit ni essayé ici ; la licence GPL-3.0 de cocoa-way est à examiner avant toute réutilisation.

Présentation Neptune sous C : l'image partagée de Neptune EST déjà un tampon `wl_shm` (fd + offset + stride + format). `wl_buffer.release` joue le rôle de `IDLE_NOTIFY` ; `wl_surface.attach/damage/commit` est le `present`. Écart : `winewayland.drv` garde `wl_display` et `wl_surface` en interne (`process_wayland`) ; `nptunix.so` doit donc passer par un point d'entrée exporté de `winewayland.so` (même processus, `dlsym` sur `RTLD_NOLOAD`), ce qui modifie le pilote amont (300 à 500 lignes estimées). La synchronisation GPU-avant-lecture se fait côté client (attendre le fd de fence avant `commit`, comme le « chemin 2 » de `npt_unix_wsi.c:302-321`).

Besoins de Rocket League sous C : souris brute et confinement par les deux protocoles ci-dessus ; plein écran par `xdg_toplevel.set_fullscreen` ; changements de mode émulés par le compositeur (pas de mode matériel) ; manette hors Wayland (section 4).

Estimation (INFERENCE) : compositeur minimal à partir de `libwayland-server` via epoll-shim ou Smithay : 6 à 10 semaines (6 000 à 10 000 lignes) avant d'être fiable avec Wine ; réutiliser `wayland-darwin` ou `cocoa-way` peut réduire ce chiffre mais ajoute une dépendance dont l'état n'est pas mesuré. Côté Wine : 0 ligne pour le pilote, environ 400 pour le point de présentation. Risques : conformité de protocole (le détail des comportements de `winewayland.drv` n'est pas contractuel, par exemple les versions de `xdg_wm_base` exigées ; un rapport public lu en ligne signale des échecs d'initialisation dus à une version trop ancienne), traduction par FEX des appels `memfd_create`, `fcntl(F_ADD_SEALS)` et `SCM_RIGHTS` de libwayland, fonctionnement de libwayland-client et des protocoles de pointeur dans l'émulation.

### 2.5 Tableau comparatif

| Critère | A : serveur X11 | B : pilote sur mesure + hôte | C : winewayland + compositeur |
| --- | --- | --- | --- |
| Code Wine à écrire | 0 (hors WSI) | 4 000 à 5 500 lignes | 0 (hors WSI, environ 400) |
| Code macOS à écrire ou adopter | serveur X existant (XQuartz/Xvfb) + visionneuse ou DDX | hôte 4 000 à 6 000 lignes | compositeur 6 000 à 10 000 lignes ou projet tiers |
| Présentation Neptune | A-lite par `xcb_put_image` ; A4 par DRI3 | contrôlée, présentateur Metal | wl_shm + point d'entrée à ajouter |
| Mesures disponibles | Xvfb OK en headless, XQuartz visible plante (GLX) | aucune pour le pilote ; Cocoa de winemac comme référence | aucune |
| Souris relative / capture | incertain sous XQuartz | contrôle total | protocoles relatifs/contrainte à fournir |
| Plein écran | WM nécessaire, pas de mode réel | fenêtre couvrante + échelle Metal | `xdg_toplevel` fullscreen + échelle |
| Risque principal | serveur X instable, SHM SysV 4 Mio, pas de DRI3 | surface de code neuve | conformité du compositeur, GPL éventuelle |
| Estimation d'effort (INFERENCE) | 2 à 4 semaines (A-lite) / 8 à 12 (A4) | 4 à 6 semaines (minimal) / 8 à 12 (complet) | 6 à 10 semaines |

Recommandation (INFERENCE, à confirmer par le lead) : viser B comme chemin produit, parce qu'il est la seule option où la fenêtre, le curseur, la capture, les modes d'écran et la présentation Metal relèvent d'un seul canal que nous maîtrisons, et que le point où les options A et C doivent contourner Neptune (l'identité de fenêtre) y est trivial. Garder A2 ou A-lite comme échafaudage de première heure si l'on veut voir des fenêtres avant d'avoir le pilote B ; garder C comme alternative si l'équipe préfère ne pas maintenir un pilote Wine propre et accepte de maintenir un compositeur. Le pilote B peut démarrer par le modèle de `winewayland.drv` (21 membres de la table, thread de lecture d'événements) et réutiliser la logique Cocoa de `winemac.drv` côté hôte.

## 3. Audio

a completer

## 4. Manette et disposition clavier

a completer

## 5. Plan classé de témoins graphiques possédés

a completer

## 6. Inconnues et questions ouvertes

a completer

## 7. Journal des commandes et sources externes

a completer

## Annexe A : correctifs du spike vtest macOS (appliqués à une copie de `NEP/virgl`, commit 482f9d8b)

Ces diffs sont reproduits ici parce que le spike a vécu dans le scratchpad de la session (hors `EXP`). Ils s'appliquent depuis la racine de la copie de `virgl/` (chemins relatifs `vtest/...`).

`vtest/vtest_shm.c` (POSIX shm sous `__APPLE__`) :

```diff
--- a/vtest/vtest_shm.c
+++ b/vtest/vtest_shm.c
@@ -45,8 +45,33 @@
 #endif
 }
 
+#ifdef __APPLE__
+/* Darwin has no memfd_create.  Reuse the POSIX shm path of os_create_anonymous_file
+ * (shm_open + shm_unlink + ftruncate, anon_file.c).  The tag must stay short:
+ * shm names are limited to PSHMNAMLEN (31) characters on macOS. */
+#include "mesa/util/anon_file.h"
+
 int vtest_new_shm(uint32_t handle, size_t size)
 {
+   int fd = os_create_anonymous_file((off_t)size, "vt");
+   (void)handle;
+   if (fd < 0)
+      return report_failed_call("os_create_anonymous_file", -errno);
+   return fd;
+}
+
+int vtest_shm_check(void)
+{
+   int fd = os_create_anonymous_file(16384, "vt");
+   if (fd >= 0) {
+      close(fd);
+      return 1;
+   }
+   return 0;
+}
+#else
+int vtest_new_shm(uint32_t handle, size_t size)
+{
    int fd, ret;
    char name[32];
 
@@ -75,3 +100,4 @@
 
     return 0;
 }
+#endif
```

`vtest/vtest_renderer.c` (nombre de timelines, fence de sortie par pipe) :

```diff
--- a/vtest/vtest_renderer.c
+++ b/vtest/vtest_renderer.c
@@ -105,6 +105,10 @@
    uint32_t count;
    struct vtest_sync **syncs;
    uint64_t *values;
+
+   /* Darwin: write end of the pipe whose read end was sent as OUT_FENCE_FD.
+    * One byte is written when the context fence retires (-1 if unused). */
+   int out_fence_wr;
 };
 
 struct vtest_sync_wait {
@@ -331,6 +335,8 @@
    uint32_t i;
    for (i = 0; i < submit->count; i++)
       vtest_unref_sync(submit->syncs[i]);
+   if (submit->out_fence_wr >= 0)
+      close(submit->out_fence_wr);
    free(submit);
 }
 
@@ -961,7 +967,7 @@
    case VCMD_PARAM_MAX_TIMELINE_COUNT:
       resp[0] = true;
       /* TODO until we have a timerfd */
-#ifdef HAVE_EVENTFD_H
+#if defined(HAVE_EVENTFD_H) || defined(__APPLE__)
       if (!getenv("VIRGL_DISABLE_MT"))
          resp[1] = VTEST_MAX_TIMELINE_COUNT;
 #endif
@@ -1974,6 +1980,11 @@
       for (i = 0; i < submit->count; i++) {
          vtest_signal_sync(submit->syncs[i], submit->values[i]);
          vtest_unref_sync(submit->syncs[i]);
+      }
+      if (submit->out_fence_wr >= 0) {
+         char one = 1;
+         (void)write(submit->out_fence_wr, &one, 1);
+         close(submit->out_fence_wr);
       }
       free(submit);
 
@@ -2330,6 +2341,7 @@
       submit->count = batch->sync_count;
       submit->syncs = (void *)&submit[1];
       submit->values = (void *)&submit->syncs[batch->sync_count];
+      submit->out_fence_wr = -1;
    }
 
    for (i = 0; i < batch->sync_count; i++) {
@@ -2384,6 +2396,18 @@
             fence_fd = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
             if (fence_fd < 0)
                report_failed_call("eventfd(signalled)", -errno);
+#elif defined(__APPLE__)
+            /* No eventfd/sync_file on Darwin: hand out a pipe read end that becomes
+             * readable when the context fence retires (vtest_signal_timeline). */
+            int pfd[2];
+            if (!pipe(pfd)) {
+               fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
+               fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
+               fence_fd = pfd[0];
+               submit->out_fence_wr = pfd[1];
+            } else {
+               report_failed_call("pipe(out fence)", -errno);
+            }
 #endif
          }
          if (batch->flags & VCMD_SUBMIT_CMD2_FLAG_OUT_FENCE_FD)
```

Les numéros de ligne des hunks sont ceux de la copie d'origine ; ils ont été appliqués par script Python (remplacement exact de blocs) et non par `patch`, donc un `patch -p1` direct n'a pas été testé.
