# Wine macOS : inventaire pour le pont Windows, Linux et macOS

Date : 2026-10-02. Statut : version 2, complète pour les cinq points de la consigne ; les inconnues sont listées en section 6. Rédigé par l'agent d'inventaire du côté Wine macOS.

Conventions. MESURÉ : sortie d'une commande ou d'un petit programme lancé aujourd'hui sur cette machine (M5 Max, macOS 27.0.1 build 26A434, Darwin 27.0.0 xnu-13432.1.9, pages hôte de 16 Kio, Rosetta présent). LU : citation de source avec `chemin:ligne`. DEDUIT : inférence à vérifier. EXTERNE : affirmation d'un tiers lue sur le web, non vérifiée localement.

Abréviations de chemins.
- `W` = `/Users/mathieu/Documents/Github/Zagrios/voile-wine` (branche `main`, HEAD `e82937c9`, `git describe` = `v26.3.0-2-14-ge82937c`, fichier `VERSION` = "Wine version 11.0", 22 commits Obelisk au-dessus de `ff44e00d` "CrossOver 26.3.0").
- `V` = `/Users/mathieu/Documents/Github/Zagrios/voile` (HEAD `fb27a63`, épingle voile-wine `e82937c9`).
- `RT` = `~/Library/Application Support/com.zagrios.obelisk/Voile/v26.3.0-4` (release installée, `runtime.json` : crossover 26.3.0, voile `6d2bbb44`, voile-wine `9861d8b4`, proton `5b89db94`, D3DMetal "Evaluation environment for Windows games 4.0 beta 2").
- `CUR` = `~/Library/Application Support/com.zagrios.obelisk/Voile/current` (runtime de développement installé par `install-dev.sh`, copie du 2026-09-26 19:26, sans `runtime.json`, binaires non strippés).
- `FEX` = `build/experiments/rocket-hyperlinux-20260928/probes/fex-implementation-feasibility-20260929/source/FEX-0df84d3844bcdb87bb7d3f5b8fb0959cd009c038` (même commit que `FEX-Emu/FEX` HEAD au 2026-10-02, vérifié par `git ls-remote`).
- Les sources des sondes et leurs sorties brutes sont dans le répertoire temporaire de session (`.../scratchpad/wmb/`, volatile). Les sources utiles sont reproduites en annexe A.

## 0. Synthèse pour la décision d'architecture

1. Le runtime installé est entièrement x86_64 et tourne sous Rosetta. `lipo -archs` donne `x86_64` pour `bin/wine`, `bin/wineserver`, les 34 Mach-O de `lib/wine/x86_64-unix`, `libd3dshared.dylib`, `D3DMetal.framework` et ses quatre dylib. Les 766 fichiers PE de `x86_64-windows` sont des PE32+ x86-64 et les 815 de `i386-windows` des PE32 i386. Il n'existe aucun PE arm64, arm64ec ni `xtajit` dans le runtime (MESURÉ, section 1).
2. Obelisk exécute `Voile/current` avant la release épinglée (`crates/game-runtime/src/voile.rs:59-73`). Sur cette machine le runtime effectif est donc le build de développement du 2026-09-26.
3. D3DMetal n'a aucune tranche arm64 (MESURÉ). Un processus arm64 natif ne peut pas le charger : le chemin Metal d'une conception "Wine arm64 plus FEX" ne peut pas reprendre D3DMetal tel quel.
4. Entrée PE vers Unix : les DLL de Wine appellent `callq *0x7ffe1000` (stub dans `include/wine/asm.h:287-301`, octets MESURÉS dans `ntdll.dll`), car `virtual_init_user_shared_data` force `SystemCall = 1` (`virtual.c:4457`). Aucune instruction `syscall` n'est exécutée par le code de Wine lui-même.
5. `syscall` brut : sous Rosetta, tout numéro de classe 0 (donc tout numéro Linux et tout identifiant NT) lève SIGSYS (MESURÉ sur 6 cas). Wine macOS installe `sigsys_handler` (`signal_x86_64.c:3062-3065`) qui renvoie vers le dispatcher. Il n'y a ni Syscall User Dispatch ni `prctl` dans l'arbre. Les numéros Linux (0 à environ 450) recouvrent les identifiants NT de la table 0 (259 services, 0x000 à 0x102) : la sémantique Linux ne peut être choisie que par l'adresse de l'appelant ou par un état de thread. Le cas réel est celui du bootstrapper PE d'EAC, qui émet lui-même des `syscall` Linux (319 `memfd_create`, 1 `write`) depuis son image à `0x14004b9ff` (`docs/research/rocket-league-macos.md:54-57`).
6. La TEB n'est pas la base GS sur macOS : Wine recopie `Tib.Self`, `ThreadLocalStoragePointer` et `Peb` dans le TSD du pthread (`gs:0x30`, `gs:0x58`, `gs:0x60`) et réécrit `localtime()` en mémoire. `ANNOUNCE.md:63-64` annonce un échange de GS dans le dispatcher en amont, mais le code CrossOver de cet arbre et le dispatcher installé (désassemblé) ne le font pas.
7. Rosetta donne à l'invité de vraies pages de 4 Kio et accepte un `__PAGEZERO` de 0x1000 (zone basse mappable, dont 0x7ffe0000). Un processus arm64 natif a des pages de 16 Kio, rien sous 4 Gio, est tué à l'exécution si son `__PAGEZERO` fait moins de 4 Gio, et perd `x18` à chaque entrée noyau (tout MESURÉ, section 3). Le schéma actuel de Wine (segments zerofill, USER_SHARED_DATA à 0x7ffe0000, TEB dans `x18`) ne se transpose donc pas tel quel dans un processus arm64.
8. voile-wine contient déjà la plomberie "Wine ARM64 plus émulateur" : ARM64EC (`signal_arm64ec.c`, `load_arm64ec_module`), stub `xtajit64.dll` à 20 exports (16 `stdcall` et 4 `extern`), clés `Wow64\amd64` et `Wow64\x86`, `KiUserEmulationDispatcher`, simulation de pages de 4 Kio sur hôte 16 Kio. Le build de Voile ne compile que `i386,x86_64` (`V/scripts/build.sh:33`).
9. L'interface `xtajit64` de Wine est identique, nom pour nom, à celle de `libarm64ecfex.dll` de FEX 0df84d38. Le syscall de FEX passe par l'exception `STATUS_EMULATION_SYSCALL` que `signal_arm64ec.c:1282` sait traiter, et FEX place la vraie TEB dans `gs` (`FEX/Source/Windows/ARM64EC/Module.cpp:361-367`). Aucun hack de TSD n'est nécessaire côté invité.
10. Trois candidats de premier témoin de franchissement sont décrits en section 5.3 : (T1) un PE qui charge un ELF statique et dont les `syscall` Linux sont aiguillés par Wine vers macOS, sous Rosetta ; (T2) un couple PE plus bibliothèque Unix Mach-O compilé hors arbre avec la recette de Voile ; (T3) un hôte arm64 autonome avec FEXCore exécutant les mêmes charges PE et ELF, qui sert de sosie de T1.

## 1. Matrice des composants et architecture du runtime macOS actuel

Sources : `lipo -archs`, `file`, `otool -L`, `otool -l`, `nm` sur `RT` (commandes en annexe B) ; en-têtes PE lus par script Python ; `CUR` donne les mêmes architectures. Version Wine : chaîne `wine-11.0` dans `ntdll.so`, `W/VERSION`, CrossOver 26.3.0 (`RT/runtime.json`).

### 1.1 Composants

| # | Composant | Chemin dans `RT` | Format et architecture (MESURÉ) | Chargé par | Processus et architecture d'exécution | Rôle |
|---|---|---|---|---|---|---|
| 1 | Lanceur `wine` | `bin/wine` (9808 o) | Mach-O exécutable x86_64, `__PAGEZERO` de 4 Gio, `__TEXT` à 0x100000000 | Obelisk (Rust, arm64) par `Command::new` (`wine.rs:242,406`) | Rosetta x86_64 | `W/tools/wine/wine.c:20-65` : `dlopen("ntdll.so")` puis `__wine_main` ; ce processus ré-exécute ensuite le chargeur (ligne 2) |
| 2 | Chargeur `wine` | `lib/wine/x86_64-unix/wine` (14704 o) | Mach-O exécutable x86_64 ; `__PAGEZERO` 0x1000, `WINE_RESERVE` 0x1000+0x1fffff000, `WINE_TOP_DOWN` 0x7ff000000000+0x1ff0000, `__TEXT` 0x200000000 | `reexec_loader` (`W/dlls/ntdll/unix/loader.c:2658-2683`) puis à chaque nouveau processus Windows par `exec_wineloader` (`loader.c:709-729`) | Rosetta | `W/loader/main.c` : réserve la mémoire basse avant tout, charge `ntdll.so`, appelle `__wine_main` |
| 3 | `ntdll.so` | `lib/wine/x86_64-unix/ntdll.so` (623200 o ; `CUR` 699888 o) | Mach-O dylib x86_64 ; IOKit, CoreFoundation, CoreServices, libSystem | `dlopen` par les lignes 1 et 2 | Rosetta | moitié Unix de ntdll : dispatcher de syscalls, signaux, mémoire virtuelle, protocole wineserver, msync, chargement des PE |
| 4 | `ntdll.dll` | `lib/wine/x86_64-windows/ntdll.dll`, `i386-windows/ntdll.dll` | PE32+ x86-64 ("Wine builtin DLL"), ImageBase 0x170000000 ; i386 : PE32 | `load_ntdll()` puis `load_wow64_ntdll()` (`loader.c:2371-2372`) | Rosetta (code PE traduit) | stubs de syscalls, Rtl, Ldr, KiUser*Dispatcher |
| 5 | `win32u.so` | `lib/wine/x86_64-unix/win32u.so` (1896224 o) | Mach-O dylib x86_64 ; CoreText, AppKit, CoreFoundation | `dlopen` lors du premier appel de `win32u.dll` | Rosetta | moitié Unix de win32u ; enregistre la table de syscalls 1 (`W/dlls/win32u/syscall.c:103`) |
| 6 | `win32u.dll` | `lib/wine/x86_64-windows/win32u.dll` | PE32+ x86-64, ImageBase 0x180000000 | ntdll / user32 | Rosetta | stubs `NtUser*` et `NtGdi*` (identifiants 0x1000 et plus) |
| 7 | `winemac.so` et `winemac.drv` | `lib/wine/x86_64-unix/winemac.so` (511936 o) ; `lib/wine/x86_64-windows/winemac.drv` | `.so` : Mach-O x86_64 ; AppKit, Carbon, CoreVideo, Foundation, IOKit, Metal, OpenGL, QuartzCore, Security ; `.drv` : PE32+ x86-64 | win32u (pilote graphique) | Rosetta | pilote d'affichage macOS : fenêtres, entrées, OpenGL ; sources `d3dmetal.c` et `d3dmetal_objc.m` pour la présentation de D3DMetal (`W/dlls/winemac.drv/Makefile.in`) |
| 8 | `wineserver` | `bin/wineserver` (675120 o ; `CUR` 845840 o) | Mach-O exécutable x86_64 ; libSystem seul | `exec_wineserver` (`loader.c:737-756`) ou Obelisk (`wine.rs:251,513`) | processus Rosetta séparé, un par préfixe | objets NT, registre ; `W/server/mach.c` (port bootstrap, ports de tâche, `mach_vm_read_overwrite`, `mach_vm_write`, état de debug) ; `W/server/msync.c` |
| 9 | `libd3dshared.dylib` | `lib/external/libd3dshared.dylib` (241888 o) | Mach-O dylib x86_64 ; libc++, Foundation, CoreGraphics, libobjc. Exporte `___wine_unix_call_funcs`, `_register_non_native_code_region`, `_supports_non_native_code_regions`, `KeUserModeCallback`, `D3DMThreadInit`, `Win32DispatchInit`, `GetMacDRVFunctions`, `PatchUnixCallTable`, `PatchWin32DispatchFunctions` | `dlopen` par `init_non_native_support` si `CX_APPLEGPTK_LIBD3DSHARED_PATH` est défini (`loader.c:1441-1473` ; Obelisk le définit, `wine.rs:134-140`) et par les six liens ci-dessous | Rosetta | colle D3DMetal à Wine : recherche `NtCreateEvent`, `NtCreateSemaphore`, `ntdll_get_unix_file_name`, `macdrv_functions` ; patche les tables d'appel Unix |
| 10 | `d3d10.so`, `d3d11.so`, `d3d12.so`, `dxgi.so`, `nvapi64.so`, `nvngx-on-metalfx.so` | `lib/wine/x86_64-unix/` | six liens symboliques vers `../../external/libd3dshared.dylib` | `get_builtin_unix_funcs` (`virtual.c:837-860`) quand la DLL PE de même nom interroge `MemoryWineUnixFuncs` | Rosetta | bibliothèques Unix des enveloppes D3DMetal |
| 11 | `D3DMetal.framework` | `lib/external/D3DMetal.framework/Versions/A/D3DMetal` (7578032 o) | Mach-O dylib x86_64 seul (aucune tranche arm64) ; nom d'installation `/System/Library/Frameworks/D3DMetal.framework/...` ; Metal, MetalFX, MetalKit, AppKit, CoreAnalytics (privé) | `dlopen` par `libd3dshared` (chaînes `@rpath/D3DMetal.framework/D3DMetal`, `D3DMETAL_FRAMEWORK_PATH`) | Rosetta | traducteur Direct3D 10/11/12 vers Metal. Le chemin système `/System/Library/Frameworks/D3DMetal.framework` n'existe pas sur cette machine |
| 12 | `libmetalirconverter`, `libdxcompiler`, `libdxilconv`, `libdxccontainer` | `.../D3DMetal.framework/Versions/A/Resources/` | Mach-O dylib x86_64 (30,5 Mo ; 29 Mo ; 2,1 Mo ; 0,1 Mo) | D3DMetal | Rosetta | conversion DXIL vers Metal IR |
| 13 | Enveloppes PE de D3DMetal | `lib/wine/x86_64-windows/d3d11.dll` (139264 o), `dxgi.dll` (122880), `d3d12.dll` (196608), `nvapi64.dll` (188416), `nvngx-on-metalfx.dll` (135632) | PE32+ x86-64 ; imports MESURÉS : `NtQueryVirtualMemory` (et `GetProcAddress`, `LoadLibraryA`, `VirtualProtect`) | le jeu | Rosetta | appellent `NtQueryVirtualMemory(MemoryWineUnixFuncs)` puis `__wine_unix_call` (export de ntdll, `ntdll.spec:1773`, "CW HACK 22435", `W/dlls/ntdll/loader.c:4091-4098`) |
| 14 | `wow64.dll`, `wow64cpu.dll`, `wow64win.dll` | `lib/wine/x86_64-windows/` | PE32+ x86-64 | `init_wow64` (`W/dlls/ntdll/loader.c:5016-5050`) | Rosetta | nouveau WoW64 : un processus 64 bits exécute le code 32 bits par changement de mode CPU ; `wow64cpu/cpu.c:55-70` contient un contournement de Rosetta ("rosetta2_workaround") |
| 15 | Pont Steam | `lib/wine/x86_64-unix/lsteamclient.so` (2028928 o), `lsteamclient.dll`, `steam.exe`, `steam/steamclient.dll` (PE32 i386), `steam/steamclient64.dll` (PE32+ x86-64) | Mach-O x86_64 et PE | Obelisk / jeu | Rosetta | pont Proton vers le moteur Steam macOS ; les exports de `steamclient*.dll` de Valve sont réécrits en `mov rax,imm64 ; jmp rax` (`loader.c:1139-1253`) |
| 16 | Bibliothèques tierces | `lib/wine/x86_64-unix/libfreetype.6.dylib`, `libgnutls.30.dylib` | Mach-O x86_64 | `dlopen` par leur nom de feuille (`V/scripts/build.sh:216-219,265-267`) | Rosetta | polices ; TLS de Windows |
| 17 | Autres bibliothèques Unix | `avicap32, bcrypt, crypt32, ctapi32, dnsapi, dwrite, kerberos, localspl, mountmgr, msv1_0, netapi32, nsiproxy, odbc32, opencl, opengl32 (1193520 o), qcap, secur32, winebth, winebus, winecoreaudio, winedmo, wineps, winevulkan (645520 o), winscard, winspool, wpcap, ws2_32` | Mach-O x86_64 | leurs DLL PE | Rosetta | services Unix de Wine. `V/scripts/build.sh:33-34` compile `--without-vulkan` ; le runtime ne contient pas MoltenVK |

Constats.
- Un seul modèle d'exécution : tout est x86_64 et le code PE est exécuté par la traduction de Rosetta dans le même processus que les bibliothèques Unix. Il n'y a pas de frontière d'architecture entre PE et Unix (MESURÉ : tout est x86_64). C'est ce qui rend le dispatcher de `signal_x86_64.c` possible.
- `cxcompatdb.so` est recherché au démarrage (`loader.c:2381-2394`) mais n'est pas livré : avertissement seulement.
- Les 766 PE de `x86_64-windows` portent tous la signature "Wine builtin DLL" ; aucune machine PE `0xaa64` (ARM64) ni `0xa641` (ARM64EC) n'existe. `wow64cpu.dll` est présente mais `xtajit.dll` et `xtajit64.dll` ne le sont pas.
- `RT` et `CUR` ont les mêmes `libd3dshared.dylib` et `D3DMetal` (empreintes identiques). Les fichiers Wine diffèrent (`CUR` n'est pas strippé).

### 1.2 Processus et frontières

- Obelisk (arm64) lance `bin/wineserver` et `bin/wine` avec `WINEPREFIX`, `WINEDLLOVERRIDES`, `WINEDEBUG`, `WINEMSYNC=1`, `WINE_LARGE_ADDRESS_AWARE=1`, et, si `lib/external/libd3dshared.dylib` existe, `CX_APPLEGPTK_LIBD3DSHARED_PATH` et `CX_ACTIVE_GRAPHICS_BACKEND=d3dmetal` (`wine.rs:115-142`).
- Chaque processus Windows est un processus Rosetta distinct, créé par exec du chargeur avec `WINEPRELOADRESERVE=début-fin` (la plage de l'image PE) et `WINESERVERSOCKET` (`loader.c:709-729`).
- wineserver et les processus Windows échangent par sockets Unix, par mémoire partagée (section USER_SHARED_DATA mappée depuis un descripteur, `virtual.c:4400-4421`) et par Mach : chaque processus envoie son port de tâche au serveur enregistré dans le bootstrap sous le nom `server_dir` (`W/dlls/ntdll/unix/server.c:1473-1519`, `W/server/mach.c:96-170`) ; msync remplace les événements et sémaphores par des sémaphores Mach (`WINEMSYNC=1`, `W/dlls/ntdll/unix/msync.c`).
- Le fil principal du processus est réservé à AppKit : `apple_main_thread` (`loader.c:2556-2588`) le parque dans une boucle CFRunLoop et `start_main_thread` s'exécute dans un second pthread (`loader.c:2522-2545`).

## 2. Entrée d'un thread PE x86-64 dans le côté Unix de Wine macOS

### 2.1 Génération des stubs de syscall

- `W/tools/winebuild` ne génère pas les stubs dans cette version. `parser.c:72,259-305,495-502` analyse seulement le drapeau `syscall` et `syscall=N` (intervalle 0-0x3fff), `import.c:1040,1146` et `utils.c:675` ignorent ces entrées dans les thunks d'import. Le fichier `W/dlls/ntdll/unix/syscall.c` cité par la consigne n'existe pas ; `W/dlls/win32u/syscall.c` existe et enregistre la table 1.
- Le corps des stubs vient de la macro `__ASM_SYSCALL_FUNC` (`W/include/wine/asm.h:225-301`), appliquée à la liste `ALL_SYSCALLS` générée par `tools/make_specfiles` (`W/dlls/ntdll/ntsyscalls.h`, `W/dlls/win32u/win32syscalls.h`) dans `W/dlls/ntdll/signal_x86_64.c:43-48` (côté PE ; le même fichier compilé pour `arm64ec_x64` utilise la variante `asm.h:268-280`).
- Forme x86_64 (`asm.h:281-301`) : `mov r10,rcx ; mov eax,id ; test byte [0x7ffe0308],1 ; jne +3 ; syscall ; ret ; jmp +1 ; ret ; call [0x7ffe1000] ; ret`. MESURÉ dans `RT/lib/wine/x86_64-windows/ntdll.dll` (export `NtClose`, RVA 0x55930) : `4c 8b d1 b8 0f 00 00 00 f6 04 25 08 03 fe 7f 01 75 03 0f 05 c3 eb 01 c3 ff 14 25 00 10 fe 7f c3` ; `NtAllocateVirtualMemory` : identifiant 0x18, mêmes octets ; `win32u.dll` : `NtUserGetForegroundWindow` 0x13fa et `NtGdiBitBlt` 0x1097 (table 1).
- Numérotation : bits 0-11 numéro, bits 12-13 table (0 ntdll, 1 win32u, 2 et 3 libres). Le dispatcher lit `(id >> 12) & 3` (`signal_x86_64.c:3338-3341`) dans `KeServiceDescriptorTable[4]` (`loader.c:158-162`). `KeAddSystemServiceTable(funcs, counters, limit, args, index)` (`loader.c:787-799`) accepte les index 0 à 3 ; seul 1 est utilisé (win32u).

### 2.2 Chemin normal : `callq *0x7ffe1000`

- `virtual_init_user_shared_data` écrit `data->SystemCall = 1` (`virtual.c:4457`) dans la section USER_SHARED_DATA partagée. Le test `testb $1,0x7ffe0308` du stub est donc vrai pour tout le code de Wine, et l'exécution passe par `jmp +1 ; call [0x7ffe1000]`.
- `signal_init_process` (`signal_x86_64.c:3007-3010`) mappe une page à `user_shared_data + page_size` (0x7ffe1000) et y écrit `__wine_syscall_dispatcher`. `NtSetInformationProcess(ProcessInstrumentationCallback)` permute ce pointeur entre `__wine_syscall_dispatcher` et `__wine_syscall_dispatcher_instrumentation` (`signal_x86_64.c:542-553`).
- Le pointeur `__wine_syscall_dispatcher` de la `ntdll.dll` PE est une variable (`W/dlls/ntdll/loader.c:80`, `ntdll.spec:1752`) remplie par la bibliothèque Unix (`loader.c:2043`), comme `__wine_unix_call_dispatcher` (`loader.c:2036,2051`) et `__wine_unixlib_handle` (`loader.c:2044`). Ces trois noms sont les seuls liens binaires entre la moitié PE et la moitié Unix de ntdll.

### 2.3 Le dispatcher (`signal_x86_64.c:3226-3523`)

Séquence lue, confirmée par le désassemblage du `ntdll.so` installé (`otool -tvV`, début identique : `movq %gs:0x30,%rcx ; movq 0x378(%rcx),%rcx` à `___wine_syscall_dispatcher` = +0x348d0) :
1. `rcx` ← TEB (`gs:0x30`) puis `rcx` ← `thread_data->syscall_frame` (TEB+0x378 ; sur Linux c'est `gs:0x378` directement, `signal_x86_64.c:3227-3234`).
2. Dépile l'adresse de retour dans `frame->rip`, sauve rflags, rbx, rdx, rsi, rdi, r12-r15, cs, rsp, ss, rbp, `syscall_id` (eax) ; `subq $0xb,frame->rip` (contournement documenté pour Legends of Runeterra, lignes 3266-3268).
3. Sauve l'état FPU/AVX par `xsavec64`, `xsave64` ou `fxsave64` selon `0x7ffe0285`, `0x7ffe03d8`, `0x7ffe03ec` (lignes 3269-3292).
4. Passe sur la pile noyau (`movq %rcx,%rsp`, la pile noyau commence juste sous le cadre), `ldmxcsr` du MXCSR Unix (`TEB+0x33c`), calcule la table `(id >> 8) & 0x30`, borne `ServiceLimit` (sinon `bad_syscall` renvoie `STATUS_INVALID_SYSTEM_SERVICE` 0xc000001c, lignes 3519-3522), copie les arguments de pile au-delà de 4 registres, charge `r10, rdx, r8, r9` dans `rdi, rsi, rdx, rcx` et appelle l'implémentation (convention SysV).
5. Retour (`__wine_syscall_dispatcher_return`, lignes 3369-3487) : restaure FPU selon `restore_flags`, puis retour par `ret` (chemin rapide), `iretq` (CONTEXT_CONTROL ou INTEGER) ou rappel d'instrumentation.
- Le cadre `struct syscall_frame` (`signal_x86_64.c:463-492`, taille 0x300, XSAVE à 0xc0) et `struct amd64_thread_data` (`signal_x86_64.c:498-518`, dans `TEB.GdiTebBatch` à partir de 0x2f0 : dr0-dr7, `pthread_teb` 0x320, `frame_size` 0x328, `instrumentation_callback` 0x330, `fs` 0x338, `mxcsr` 0x33c) plus `struct ntdll_thread_data` (`unix_private.h:102-119` : `syscall_table` 0x370, `syscall_frame` 0x378, `syscall_trace` 0x380) forment l'ABI interne de la TEB. Les décalages sont figés par `C_ASSERT`.

### 2.4 Appels unixlib : `__wine_unix_call`

- Une DLL PE builtin avec bibliothèque Unix obtient le tableau de fonctions de sa `.so` par `NtQueryVirtualMemory(module, MemoryWineUnixFuncs)` (`virtual.c:6200-6212`) qui fait `dlopen(unix_path)` puis `dlsym("__wine_unix_call_funcs")` (`virtual.c:837-860`). Elle appelle ensuite `__wine_unix_call_dispatcher(handle, code, args)` (`W/include/wine/unixlib.h:280-300`).
- `__wine_unix_call_dispatcher` (`signal_x86_64.c:3552-3654`) est une variante allégée du dispatcher : elle sauve le cadre incomplet (`restore_flags = RESTORE_FLAGS_INCOMPLETE_FRAME_CONTEXT`), passe sur la pile noyau et fait `callq *(%r10,%rdx,8)` sur le tableau. Ce chemin ne passe pas par une instruction `syscall` ni par un signal. C'est celui de winemac, de D3DMetal (via `ntdll.__wine_unix_call`) et de lsteamclient.
- Le tableau interne de ntdll (`loader.c:1492-1506`) contient `load_so_dll`, `unwind_builtin_dll`, `wine_dbg_write`, `wine_server_call`, `wine_server_fd_to_handle`, `wine_server_handle_to_fd`, `wine_spawnvp`, `system_time_precise`, `steamclient_setup_trampolines` et, sur x86_64 seulement, `pe_module_loaded` (qui enregistre les régions de code PE auprès de `libd3dshared`, "CW HACK 22434", `loader.c:1419-1489`).

### 2.5 Retour Unix vers PE

`load_ntdll_functions` (`loader.c:2011-2053`) résout dans la `ntdll.dll` PE `KiUserExceptionDispatcher`, `KiUserApcDispatcher`, `KiUserCallbackDispatcher`, `LdrInitializeThunk`, `RtlUserThreadStart`, `__wine_ctrl_routine`, `DbgUiRemoteBreakin`, `KiRaiseUserExceptionDispatcher` et, en ARM64EC, `KiUserEmulationDispatcher` (`loader.c:105-116`). Les exceptions sont livrées en construisant `exc_stack_layout` (`signal_x86_64.c:419-430`) sur la pile utilisateur. D3DMetal appelle `KeUserModeCallback` (exporté par `libd3dshared`) pour rappeler du code PE.

### 2.6 TEB, FS/GS et TLS sur macOS

- LU (`init_syscall_frame`, `signal_x86_64.c:3099-3107`) : sur macOS il n'y a pas d'`arch_prctl`. Wine écrit `teb->Tib.Self` à `gs:0x30`, `teb->ThreadLocalStoragePointer` à `gs:0x58` et `teb->Peb` à `gs:0x60`, c'est-à-dire dans le tableau TSD du pthread, retient la base GS de ce pthread dans `thread_data->pthread_teb` (`mac_thread_gsbase`, lignes 2975-2988, par `thread_info(THREAD_IDENTIFIER_INFO)`) et la copie dans `teb->Instrumentation[0]`. Le code PE lit donc `gs:[0x30]`, `gs:[0x58]` et `gs:[0x60]` correctement ; les autres décalages de la TEB lus directement par `gs:` (par exemple `gs:[0x48]` TID, `gs:[0x68]` LastError, `gs:[0x08]` et `gs:[0x10]` limites de pile) tombent sur d'autres cases du TSD de libpthread. DEDUIT : c'est un risque connu des programmes qui lisent la TEB sans passer par `gs:[0x30]`.
- LU (`loader.c:2375-2379`, `2316-2348`) : la case `gs:0x60` est utilisée par `localtime()` de libSystem. `start_main_thread` réécrit le début de `localtime` en `jmp *` d'une adresse stockée 8 octets plus loin (`my_localtime`) après un `mprotect` RWX du texte de libSystem. `pthread_exit_wrapper` met `gs:0x60` à NULL avant la sortie du thread pour que macOS ne libère pas la PEB (`W/dlls/ntdll/unix/thread.c:1113-1124`).
- LU (`ANNOUNCE.md:63-64`, texte amont) : "On macOS, the `%gs` register is swapped in the syscall dispatcher". MESURÉ : le dispatcher du `ntdll.so` installé ne contient pas d'échange de GS. DEDUIT : CrossOver 26.3.0 garde le schéma TSD, probablement parce qu'un échange de base GS coûte un appel système machdep par transition sous Rosetta ; non vérifié.
- Rosetta ne fournit ni FS ni FSGSBASE à l'invité (MESURÉ, annexe A.4) : `%fs:0` lève SIGSEGV à l'adresse 0, `wrfsbase`, `rdfsbase` et `wrgsbase` lèvent SIGILL, `CPUID` feuille 7 EBX bit 0 = 0. Le TLS d'un ELF glibc (basé sur `%fs`) ne peut donc pas tourner sous Rosetta ; seul `%gs` (TSD) est utilisable. Pour le code 32 bits, Wine utilise la LDT (`i386_set_ldt`, `signal_x86_64.c:2898-2899`, `3019-3020`, `2944-2958`).
- Côté FEX (LU, `FEX/Source/Windows/ARM64EC/Module.cpp:361-367`) : `State.gs_cached = NtCurrentTeb()` et `fs_cached = 0`. FEX porte les bases de segments dans son état d'invité, donc `gs:[x]` atteint la vraie TEB et `%fs` peut être n'importe quelle base (utile pour du TLS ELF).

### 2.7 Signaux et exceptions Mach

- LU (`signal_x86_64.c:3043-3066`) : `sigaction` avec `SA_SIGINFO|SA_RESTART|SA_ONSTACK` et `sa_mask = server_block_set` pour SIGINT, SIGFPE, SIGABRT, SIGQUIT, SIGUSR1 (suspension), SIGTRAP, SIGSEGV, SIGILL, SIGBUS et, sur Apple seulement, SIGSYS. Le numéro d'exception x86 vient de `__es.__trapno` du contexte (`TRAP_sig`, ligne 327).
- Aucun port d'exception Mach : `grep` de `task_set_exception_ports`, `thread_set_exception_ports` et `EXC_BAD_ACCESS` dans `W/dlls/ntdll/unix/` ne trouve rien ; `mach_port_allocate` n'apparaît que dans `msync.c` (sémaphores) et le Mach de `server.c` sert à envoyer le port de tâche au serveur. Les fautes sont donc traitées par les signaux BSD, que Rosetta livre à l'invité.
- Particularités Rosetta codées en dur : `is_rosetta2` lu par `sysctl.proc_translated` (`signal_x86_64.c:3026-3041`) ; MXCSR incorrect dans les contextes de signal (CW Hack 24256, `signal_x86_64.c:1003-1008`, `2856-2875`) ; fautes d'écriture rapportées comme lectures (`virtual.c:4556-4562`) ; fautes sur pages W|X et X (CW Hacks 24945 et 25719, `virtual.c:4564-4582`) ; réécriture de code d'un autre processus par `NtWriteVirtualMemory` non vue par Rosetta, d'où une bascule du bit exécutable (CW HACK 18947, `virtual.c:6836-6900`) ; émulation de `xgetbv` et des NOP CET (`signal_x86_64.c:1956-2035`, `2534-2539`) ; wineserver renvoie des registres de debug à zéro pour un processus traduit (`W/server/mach.c:192-197`).
- Faux succès existants dans Wine macOS : `NtSetContextThread` (`signal_x86_64.c:1115`, bloc aux lignes 1149-1154) et `set_thread_wow64_context` (ligne 1372, bloc aux lignes 1395-1400) transforment un `STATUS_UNSUCCESSFUL` du serveur en `STATUS_SUCCESS` quand des registres de debug sont demandés (CW HACK 22131, message "Setting debug registers is not supported under Rosetta, faking success", présent dans les chaînes du `ntdll.so` installé). Ce n'est pas une modification d'EAC, mais c'est une réponse de succès fabriquée à une API que des anticheats utilisent ; à relire avant tout test avec EAC (section 6).

### 2.8 `syscall` brut avec un numéro Linux : qui décide

MESURÉ (annexe A.1, programme `rawsys`, binaire x86_64 lancé sous Rosetta, `proc_translated = 1`, un cas par processus, avec gestionnaires pour SIGSYS, SIGILL, SIGSEGV, SIGBUS, SIGTRAP) :

| Cas | `rax` | Résultat |
|---|---|---|
| 0 | `0x2000014` (classe BSD, getpid) | retour = pid réel, pas de signal |
| 1 | 39 (getpid Linux, classe 0) | SIGSYS (12), `si_code = 0`, `si_addr = rip`, `rip` pointe juste après les octets `0f 05` |
| 2 | 0x18 (identifiant NT, classe 0) | SIGSYS, même forme |
| 3 | 1 (write Linux, classe 0) | SIGSYS, rien n'est écrit |
| 4 | `0x4000001` (classe 4 invalide) | SIGSYS |
| 5 | `0x3000003` (classe machdep 3, `thread_fast_set_cthread_self`) | TIMEOUT du processus de sonde (arrêt par le pilote au bout de 8 s). DEDUIT, non vérifié : la classe machdep 3 est `thread_fast_set_cthread_self`, qui fixe la base GS, ici à 1, et le gestionnaire de signal ne peut plus lire le TSD |
| 6 | `0x1000014` (classe Mach) | retour `0x10000003`, pas de signal |
| 7 | 0x1018 (identifiant de la table 1, classe 0) | SIGSYS |

Lecture. Darwin ou Rosetta décide, pas Wine : la classe du numéro (bits 24-31) choisit le sous-système, et la classe 0 donne SIGSYS sur macOS 27.0.1 (MESURÉ). Le commentaire de Wine dit "Only called on macOS 14 Sonoma and later" (`signal_x86_64.c:2831-2833`) ; le comportement des versions intermédiaires n'a pas été vérifié. Les registres `rcx` et `r11` ne contiennent pas l'adresse de retour ni rflags dans le contexte livré (valeurs sans rapport dans le cas 1) : `sigsys_handler` les reconstruit lui-même (`frame->rcx = RIP`, `R11 = eflags`, lignes 2842-2848).

Ce que fait Wine (LU, `signal_x86_64.c:2834-2876`) : `frame->rip = RIP + 0xb` (l'instruction `ret` de la fin du stub standard), `frame->eflags`, `RCX = frame`, `RIP = __wine_syscall_dispatcher_prolog_end_ptr` ; le gestionnaire n'exécute rien lui-même, il renvoie le fil au dispatcher qui travaille hors du contexte du signal, sur la pile noyau. Un identifiant NT hors de la table (`bad_syscall`) donne `STATUS_INVALID_SYSTEM_SERVICE`. Il n'y a aucun Syscall User Dispatch : `grep` de `SYSCALL_USER_DISPATCH`, `PR_SET_SYSCALL`, `SYS_USER_DISPATCH` dans `W/dlls/ntdll`, `W/server`, `W/include`, `W/configure.ac` ne trouve que les trois lignes SIGSYS ci-dessus. Sous Linux, ce Wine ne piège donc pas un `syscall` brut (seuls les stubs de Wine passent par `0x7ffe1000`).

Cas réel du bootstrapper EAC. Le diagnostic local (`docs/research/rocket-league-macos.md:47-61`) montre que le bootstrapper Windows intact, quand il détecte Wine, prend la branche `linux64`. Son image PE (base `0x140000000`) prépare le numéro Linux 319 (`memfd_create`, adresse `0x14001cc82`), puis 1 (`write`), construit `/proc/self/fd/%i`, exécute l'instruction `syscall` d'un pont d'appel (`0x14004b9ff`), cherche `libc.so` dans `/proc/%i/maps` (`0x14001c850`), valide un ELF x86-64 de type bibliothèque partagée et résout `__libc_dlopen_mode` ou `dlopen` puis `dlsym` (`0x14001c470`) pour charger le module Linux dans le même processus. DEDUIT de la lecture du code de Wine (non exécuté) : dans Wine macOS non modifié, le `syscall` 319 donne SIGSYS puis le dispatcher, qui compare 319 à la limite de la table 0 (259 services, identifiants 0x000 à 0x102, `ARRAY_SIZE(syscalls)` de `loader.c:158-162`) et renvoie `STATUS_INVALID_SYSTEM_SERVICE` (`0xc000001c`) dans `rax` ; le `syscall` 1 (`write`) exécuterait l'entrée NT 0x0001, `NtWorkerFactoryWorkerReady`, qui est un stub (`ntsyscalls.h:539`) et lève `EXCEPTION_WINE_STUB` (`loader.c:118-142`). Dans les deux cas la réponse n'est pas celle d'un noyau Linux. Le franchissement du PE vers la sémantique Linux est donc le premier obstacle d'EAC et non un cas marginal : l'appelant est du code PE, pas seulement de l'ELF.

### 2.9 Où insérer la sémantique Linux dans une conception qui garde Wine macOS

DEDUIT, à partir des faits ci-dessus.
1. Point d'entrée : `sigsys_handler` (`signal_x86_64.c:2834`). Il reçoit déjà tout `syscall` brut, qu'il vienne d'un ELF ou, comme dans le cas EAC, de code PE. Il faut y décider "NT ou Linux", et les numéros se recouvrent (NT table 0 : 0x000-0x102 ; Linux x86-64 : 0 à environ 450), donc le discriminant ne peut pas être le numéro. Candidats : (a) l'adresse de l'appelant (`RIP_sig`) contre une liste de plages enregistrée par un appel unixlib, une classe de `NtSetInformationProcess` ou une variable d'environnement ; la liste doit pouvoir désigner des plages ELF et aussi une fonction ou une image PE précise (le pont `0x14004b9ff` du bootstrapper), sans modifier le binaire ; (b) un drapeau de personnalité par thread, ce qui exige des points d'entrée et de sortie que le code d'EAC ne fournit pas ; (c) la combinaison de (a) et (b). Le choix (a) est déterministe et testable.
2. Exécution : ne pas travailler dans le signal. Réutiliser le motif de Wine : le gestionnaire redirige `RIP` vers un second point d'entrée assembleur (copie du prologue de `__wine_syscall_dispatcher`) qui sauve le cadre, convertit les arguments Linux (`rdi, rsi, rdx, r10, r8, r9`) vers la convention interne (le dispatcher NT attend le premier argument dans `r10`) et appelle un tableau de services Linux sur la pile noyau. Variante moins invasive : enregistrer une table de services d'index 2 avec `KeAddSystemServiceTable` (`loader.c:787-799`) et réécrire `eax = 0x2000 | nr` ; la conversion des arguments reste à faire.
3. Mémoire : les segments ELF doivent être des vues connues de `virtual.c` (allocation par `NtAllocateVirtualMemory` ou enregistrement) pour que `virtual_handle_fault` et le suivi `pages_vprot` restent cohérents.
4. TLS : un ELF glibc a besoin de `%fs`. Impossible sous Rosetta (MESURÉ) ; possible sous FEX (bases de segment virtuelles). Un témoin sous Rosetta doit donc être un ELF statique sans glibc, ou utiliser `%gs`.
5. Signaux et `clone`/`futex`/`mmap` Linux : à traduire vers pthread, `os_sync_wait_on_address`, `mach_vm_*` ; hors de portée du premier témoin.
6. Les `syscall` NT de code tiers (anticheat, Chromium, protections) arrivent par le même SIGSYS : le test d'adresse doit laisser passer les autres plages PE vers le chemin NT inchangé.
7. Variante FEX : le point d'insertion équivalent est `ECSyscallHandler::HandleSyscall` (`FEX/Source/Windows/ARM64EC/Module.cpp:520-536`), qui reçoit le `CpuStateFrame` (donc `rax` et le RIP invité) avant de lever `STATUS_EMULATION_SYSCALL`. Il peut aiguiller un `syscall` Linux vers un gestionnaire macOS sans signal et sans passer par le dispatcher de Wine.
8. Le bootstrapper cherche une libc ELF dans `/proc/%i/maps` et appelle son `dlopen` : il faut donc un `/proc/self/maps` synthétique qui expose une image ELF fournissant `dlopen`, `dlsym` ou `__libc_dlopen_mode`, ainsi qu'un `memfd` et `/proc/self/fd/N`. Ce n'est pas traité ici ; voir `docs/research/rocket-league-eac-technologies-macos-2026-10-01.md`, section "Pourquoi une greffe Linux dans Wine Darwin serait un gros portage".

### 2.10 Ce qui, dans `loader.c` et `signal_x86_64.c`, entre en conflit avec FEXCore dans le même processus

Remarque préalable (DEDUIT) : un processus Rosetta ne peut pas exécuter la sortie d'un JIT arm64. FEXCore dans le processus de Wine implique donc un Wine dont la moitié Unix est arm64 (Mach-O arm64), et tout `signal_x86_64.c` devient inutilisable tel quel ; il sert de spécification de ce qu'un équivalent doit faire. Les conflits sont :
1. Propriété des signaux. Wine installe SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGSYS, SIGUSR1, SIGINT, SIGQUIT, SIGABRT pour tout le processus (`signal_x86_64.c:3043-3066`). FEXCore a besoin de SIGSEGV/SIGBUS (détection du code automodifiable, fautes invitées) et de SIGILL. Un seul gestionnaire par signal : il faut chaîner. La conception ARM64EC évite le conflit : FEX tourne comme DLL PE, ses fautes arrivent par `KiUserExceptionDispatcher` et `pResetToConsistentState` (`signal_arm64ec.c:56,1280-1290`).
2. Registre de TEB. `include/winnt.h:2499` lit la TEB dans `x18` et tout `signal_arm64.c` suppose `x18` (lignes 852-877, 1548-1549, 1599, 1639, 1704-1752). MESURÉ : macOS arm64 met `x18` à zéro (section 3.4).
3. Réécriture de libSystem : `hook(localtime, my_localtime)` (`loader.c:2316-2348,2375-2379`) est du code x86-64 (octets `ff 25`).
4. Assembleur x86-64 sans équivalent : dispatcher, `__restore_mxcsr_thunk`, `GPT_ABI_WRAPPER` (`unix_private.h:601-638`, `msthunk_NtClose`, `msthunk_NtCreateEvent`, `msthunk_NtCreateThreadEx`, etc.), `pe_module_loaded` (`loader.c:1503-1505`), états de debug x86 (`x86_debug_state_t`) de `server/mach.c` (`#if defined(__i386__) || defined(__x86_64__)`, lignes 179-262).
5. Réservations par segments zerofill (`loader/main.c:52-95`) : un exécutable arm64 dont le `__PAGEZERO` fait moins de 4 Gio est tué (MESURÉ). Le chargeur actuel, qui est ré-exécuté à chaque processus Windows (`loader.c:709-729`), ne peut pas rester tel quel.
6. `apple_main_thread` (`loader.c:2556-2588`) parque le fil principal en CFRunLoop : compatible avec FEX, mais le fil Wine est un second pthread (la pile du fil principal ne porte pas le code invité).
7. Constantes de page : `page_size = 0x1000` (`unix_private.h:152`), `host_page_size` constante 0x1000 hors aarch64 (`virtual.c:177-183`). Un build aarch64 les rend dynamiques (`virtual.c:3658-3662`) mais plusieurs fonctions refusent les libérations partielles non alignées sur la page hôte (`virtual.c:2550-2555,2594-2607`).
8. Débogage matériel : le wineserver renvoie des zéros pour les registres de debug des processus traduits ; sous FEX il n'y aurait aucune implémentation.

## 3. Réservations d'espace d'adressage et pages

### 3.1 Édition de liens et réservations de Wine macOS

- `W/configure.ac:948` : `WINELOADER_LDFLAGS="-Wl,-segalign,0x1000,-pagezero_size,0x1000,-sectcreate,__TEXT,__info_plist,loader/wine_info.plist"`.
- `configure.ac:954-957` : si l'éditeur de liens connaît `-Wl,-no_huge` (Xcode 15.3 et suivants), pas de préchargeur et `-Wl,-no_pie,-image_base,0x200000000,-no_huge,-no_fixup_chains,-segalign,0x1000,-segaddr,WINE_RESERVE,0x1000,-segaddr,WINE_TOP_DOWN,0x7ff000000000`. `configure.ac:962-977` décrit le repli par préchargeur (non utilisé ici).
- `loader/main.c:59-72` : deux sections zerofill `WINE_RESERVE` (0x1fffff000 octets : de 0x1000 à 0x200000000, "low 8GB") et `WINE_TOP_DOWN` (0x1ff0000 octets à 0x7ff000000000 : allocations descendantes et tas virtuel). `init_reserved_areas` (lignes 74-84) les remappe en `PROT_NONE|MAP_NORESERVE|MAP_FIXED`. La table `wine_main_preload_info` est lue par `virtual_init` (`virtual.c:3647-3677`).
- MESURÉ (`otool -l RT/lib/wine/x86_64-unix/wine`) : `__PAGEZERO` 0x0 taille 0x1000 ; `__TEXT` 0x200000000 taille 0x2000 ; `__DATA` 0x200002000 ; `WINE_RESERVE` 0x1000 taille 0x1fffff000 ; `WINE_TOP_DOWN` 0x7ff000000000 taille 0x1ff0000. Le lanceur `bin/wine`, lui, a un `__PAGEZERO` de 4 Gio et aucun de ces segments : c'est pourquoi `__wine_main` ré-exécute le chargeur (`loader.c:2658-2683`).
- Autres réservations (LU, `virtual.c:749-757`, seulement sans informations de préchargement) : 0x10000-0x68000000, 0x7f000000-0x7fff0000, 0x7ffffe000000-0x7fffffff0000. Sur macOS la variante `reserve_area` (`virtual.c:630-701`) balaye la carte par `mach_vm_region` et réserve les trous par `mach_vm_map(VM_FLAGS_FIXED, PROT_NONE)`. `anon_mmap_tryfixed` utilise `mach_vm_map` faute de `MAP_FIXED_NOREPLACE` (`virtual.c:602-616`).
- Limite haute : `get_host_addr_space_limit` (`virtual.c:2762-2785`) sonde par `mmap` à 2^63 puis en divisant par deux. MESURÉ (annexe A.3) : même réponse en arm64 natif et sous Rosetta, première puissance de deux acceptée 0x400000000000, d'où une limite d'environ 0x7fffffff0000 ; `mach_vm_allocate` fixe réussit jusqu'à 2^46 et échoue à 2^47 ; `MACH_VM_MAX_ADDRESS` = 0x7ffffe000000 (arm64) et 0x7fffffe00000 (x86_64).

### 3.2 Zone 0x7ffe0000 et page du dispatcher

- `user_shared_data = 0x7ffe0000` (`virtual.c:206`) ; mappée depuis un descripteur de section fourni par le serveur (`virtual.c:4400-4421`, `MAP_SHARED|MAP_FIXED` de `page_size` octets) ; initialisée une fois (`virtual.c:4429-4473`) avec `SystemCall = 1`, `NativeProcessorArchitecture` selon `native_machine`. Le stub de syscall lit en dur `0x7ffe0308`, le dispatcher lit `0x7ffe0285`, `0x7ffe03d8`, `0x7ffe03dc`, `0x7ffe03ec` (et `0x7ffe028a` dans les blocs `#ifdef __linux__` seulement).
- La page 0x7ffe1000 contient le pointeur de dispatcher (section 2.2).
- MESURÉ : sous Rosetta avec `__PAGEZERO` 0x1000, `mach_vm_allocate` fixe réussit à 0x400000, 0x10000000, 0x7ffe0000 (1 et 4 pages) et 0xfffff000 ; en arm64 natif les mêmes adresses donnent `KERN_INVALID_ADDRESS`.

### 3.3 Taille de page dans `virtual.c`

- LU : `page_shift = 12`, `page_mask = 0xfff`, `granularity_mask = 0xffff` (`virtual.c:173-175`) ; `page_size = 0x1000` constant (`unix_private.h:152`). `host_page_size` est la constante 0x1000, sauf `#ifdef __aarch64__` où elle vaut `sysconf(_SC_PAGESIZE)` lu à `virtual_init` (`virtual.c:177-183,3658-3662`).
- Voile compile x86_64 : `host_page_size` = 0x1000. Sous Rosetta cela est vrai de bout en bout. MESURÉ (annexe A.3, programme x86_64) : `getpagesize()` = 4096, `sysconf(_SC_PAGESIZE)` = 4096, `vm_page_size` = 4096 mais `vm_kernel_page_size` = 16384 ; deux `mmap` de 4 Kio consécutifs sont espacés de 4096 ; `mprotect(PROT_NONE)` sur une page de 4 Kio au milieu d'un mapping de 64 Kio réussit. Rosetta fournit donc une vraie granularité 4 Kio à l'invité, ce que Wine suppose partout.
- Simulation de 4 Kio sur hôte 16 Kio (build aarch64, annoncée dans `ANNOUNCE.md:78-82` : "works for simple applications ... Using a 4K-page kernel is strongly recommended") : un tableau `pages_vprot` garde la protection par page de 4 Kio ; la protection hôte d'une page de 16 Kio est le OU des protections des sous-pages (`get_host_page_vprot`, `virtual.c:1095-1113`) ; les gardes de pile, le suivi d'écriture, les vues et les `mprotect` sont arrondis à `host_page_size` (`virtual.c:4518-4528,4591-4610`) ; la granularité de suivi d'écriture passe à `host_page_size / page_size` (`virtual.c:367`). Limites explicites : une libération partielle qui n'est pas alignée sur la page hôte échoue avec `STATUS_CONFLICTING_ADDRESSES` et un `ERR "unaligned partial free"` (`virtual.c:2550-2555,2594-2607`) ; il n'y a pas de découpage des mappings, donc deux pages de 4 Kio de protections incompatibles dans une même page hôte donnent la protection la plus permissive.
- ARM64EC : une carte d'un bit par page de 4 Kio (`alloc_arm64ec_map`, `virtual.c:2789-2805`, drapeau `VPROT_ARM64EC` ligne 144).

### 3.4 Mesures arm64 natif contre Rosetta (cette machine)

| Mesure | arm64 natif | x86_64 sous Rosetta |
|---|---|---|
| Taille de page vue (`getpagesize`) | 16384 | 4096 |
| `__PAGEZERO` du binaire réduit à 0x1000 | l'éditeur de liens l'arrondit à 0x4000 et le processus est tué (SIGKILL) à l'exécution | accepté |
| Adresses 0x1000, 0x10000, 0x400000, 0x10000000, 0x7ffe0000, 0xfffff000 avec `__PAGEZERO` par défaut (4 Gio) | `KERN_INVALID_ADDRESS` | `KERN_INVALID_ADDRESS` |
| Mêmes adresses avec `__PAGEZERO` 0x1000 | non exécutable (tué) | 0x400000, 0x10000000, 0x7ffe0000, 0xfffff000 : succès ; 0x1000 et 0x10000 : `KERN_NO_SPACE` (déjà occupé, par quoi non identifié) |
| `x18` après un `svc #0x80` réel, une trappe Mach, une faute de données avec gestionnaire, un `usleep` | remis à 0 à chaque fois ; le contexte de signal contient l'ancienne valeur mais `sigreturn` ne la restaure pas ; 309 à 320 pertes sur 20 millions de lectures par thread pendant des `usleep(1)` | sans objet |
| Fichier `x18probe2` | 5 cas sur 5 : `x18` = 0 dès le retour du noyau | |

Le rapport frère `reports/fexcore-darwin-port.md` (lignes 45-60) obtient les mêmes résultats par d'autres programmes (zone basse refusée, `__PAGEZERO` réduit = code de sortie 137, `x18` perdu). Conséquences (DEDUIT) pour une conception arm64 :
- USER_SHARED_DATA à 0x7ffe0000, le DOS area, les images PE 32 bits ou liées sous 4 Gio et les allocations `MEM_TOP_DOWN` sous 4 Gio ne peuvent pas être hébergées 1:1 dans l'espace d'un processus arm64 macOS. Il faut une fenêtre d'adresses invitée avec base décalée dans FEXCore. EXTERNE : le fork `willfaust/FEX` (PR #2) ajoute une "guest window" avec registres réservés `x19` (base) et `x24` (temporaire) pour les invités 32 bits sur iOS, pour la même raison.
- `x18` ne peut contenir ni la TEB de Windows ARM64 ni de l'état de JIT. EXTERNE : Madeira (iOS) constate que `x18` n'est pas la TEB et se lit comme 0, obtient la TEB par `TPIDRRO_EL0` et un emplacement TSD, avec une passe de réécriture binaire après édition de liens (`virtual_ios.c`, PR #60).
- Les pages de 16 Kio imposent la simulation de 4 Kio de Wine et un suivi du code automodifiable à la granularité de 16 Kio (EXTERNE : `willfaust/FEX` PR #1).

## 4. Crochets arm64, arm64ec, wow64 et émulateur dans voile-wine

### 4.1 Ce qui existe dans l'arbre (LU)

| Élément | Emplacement | Contenu |
|---|---|---|
| Stub d'émulateur x64 pour ARM64EC | `W/dlls/xtajit64/cpu.c`, `xtajit64.spec`, `Makefile.in` | 20 exports (`xtajit64.spec` : 16 `stdcall` et 4 `extern` : `DispatchJump`, `RetToEntryThunk`, `ExitToX64`, `BeginSimulation`) ; toutes les entrées de simulation affichent "x64 emulation not implemented" et terminent le processus (`cpu.c:39-77`) ; `UpdateProcessorInformation` force `PROCESSOR_ARCHITECTURE_AMD64` (`cpu.c:235-240`) |
| Activation | `W/configure.ac:2413-2423` | `enable_xtajit64=${enable_xtajit64:-arm64ec}` quand `PE_ARCHS` est non vide ; `arm64ec` ajoute `x86_64` comme architecture supplémentaire (`configure.ac:397-402`), compilateur `arm64ec-w64-mingw32-clang` (`configure.ac:422-424`) |
| Sélection de l'émulateur | `W/dlls/ntdll/loader.c:4958-4992` (`load_arm64ec_module`) | lit la valeur par défaut de `HKLM\Software\Microsoft\Wow64\amd64` (défaut `xtajit64.dll`), charge la DLL puis appelle `arm64ec_process_init` (`signal_arm64ec.c:180-230`) |
| Valeurs de registre | `W/loader/wine.inf.in:400-402,467,497-498` | `Wow64\amd64 = xtajit64.dll`, `Wow64\x86 = xtajit.dll`, KnownDLLs `xtajit`, `_xtajit`, `xtajit64` |
| Interface résolue par ntdll | `W/dlls/ntdll/signal_arm64ec.c:42-59,186-209` | `ExitToX64`, `DispatchJump`, `RetToEntryThunk` (variables `__os_arm64x_*`, lignes 186-188) et 17 pointeurs de fonction (`BTCpu64FlushInstructionCache`, `BTCpu64IsProcessorFeaturePresent`, `BTCpu64NotifyMemoryDirty`, `BTCpu64NotifyReadFile`, `BeginSimulation`, `FlushInstructionCacheHeavy`, `NotifyMapViewOfSection`, `NotifyMemoryAlloc`, `NotifyMemoryFree`, `NotifyMemoryProtect`, `NotifyUnmapViewOfSection`, `ProcessInit`, `ProcessTerm`, `ResetToConsistentState`, `ThreadInit`, `ThreadTerm`, `UpdateProcessorInformation`, lignes 190-209) |
| Syscalls depuis l'émulateur | `signal_arm64ec.c:1280-1290` | `STATUS_EMULATION_SYSCALL` → `dispatch_syscall(arm_ctx)` ; `KiUserExceptionDispatcher` ARM64EC avec détection de patch (`KiUserExceptionDispatcher_orig`) |
| Rappels Unix vers l'émulateur | `W/dlls/ntdll/unix/loader.c:110,2038-2050` | `pKiUserEmulationDispatcher`, `__wine_unix_call_dispatcher_arm64ec` permutés avec `__wine_unix_call_dispatcher` quand `is_arm64ec()` (`unix_private.h:95-99`) ; `include/wine/unixlib.h:287-291` |
| Mémoire | `W/dlls/ntdll/unix/virtual.c:144,2789-2805` | `VPROT_ARM64EC`, carte ARM64EC d'un bit par page |
| WoW64 sur ARM64 | `W/dlls/wow64/syscall.c:767` | la DLL de CPU 32 bits est `xtajit.dll` si `native_machine == ARM64`, sinon `wow64cpu.dll` |
| WoW64 x86_64 | `W/dlls/wow64cpu/cpu.c` | interface `BTCpu*` (`BTCpuSimulate`, `BTCpuProcessInit`, `BTCpuGetBopCode`, `__wine_get_unix_opcode`, `BTCpuTurboThunkControl`) avec contournement Rosetta |
| Version | `W/ANNOUNCE.md` (Wine 11.0) | nouveau WoW64 complet ; simulation 4 Kio sur ARM64 ; échange de GS sur macOS (amont) |

Aucun symbole FEX, box64, Hangover ou `libarm64ecfex` n'existe dans l'arbre (`grep -ri` sur `dlls`, `include`, `programs`, `loader`, `server`, `tools`, `configure.ac` : seuls `xtajit` et les mots "FEX" de tests sans rapport).

### 4.2 Ce qui n'existe pas dans le runtime installé (MESURÉ)

`RT/lib/wine` ne contient que `i386-windows`, `x86_64-windows` et `x86_64-unix`. Pas de `aarch64-windows`, pas de `aarch64-unix`, pas de `xtajit64.dll`. `V/scripts/build.sh:33` compile `--enable-archs=i386,x86_64` avec `CC="clang -arch x86_64"` ; la chaîne PE est llvm-mingw 20260922 universel (`V/sources.json:6-9`), qui sait en principe cibler `arm64ec-w64-mingw32` (EXTERNE, voir 4.4).

### 4.3 Comparaison de l'interface avec FEX (LU des deux côtés)

Les 20 exports de `W/dlls/xtajit64/xtajit64.spec` et ceux de `FEX/Source/Windows/ARM64EC/libarm64ecfex.def` (20 noms également, `DispatchJump`, `RetToEntryThunk`, `ExitToX64` et `BeginSimulation` marqués `DATA`) sont les mêmes noms (`BTCpu64FlushInstructionCache`, `BTCpu64IsProcessorFeaturePresent`, `BTCpu64NotifyMemoryDirty`, `BTCpu64NotifyReadFile`, `DispatchJump`, `RetToEntryThunk`, `ExitToX64`, `BeginSimulation`, `FlushInstructionCacheHeavy`, `NotifyMapViewOfSection`, `NotifyMemoryAlloc`, `NotifyMemoryFree`, `NotifyMemoryProtect`, `NotifyUnmapViewOfSection`, `ProcessInit`, `ProcessTerm`, `ResetToConsistentState`, `ThreadInit`, `ThreadTerm`, `UpdateProcessorInformation`). Le module ARM64EC de FEX :
- gère `syscall` côté invité en levant `STATUS_EMULATION_SYSCALL` via `KiUserExceptionDispatcher` (`FEX/Source/Windows/ARM64EC/Module.cpp:516-536`) ;
- appelle le dispatcher de Wine par `__wine_syscall_dispatcher` pour les variantes natives de `NtContinue`, `NtAllocateVirtualMemory`, `NtProtectVirtualMemory`, `NtRaiseException` (`Module.cpp:85-95,225-285`, `Module.S:106-130`) ;
- reçoit ses services Unix par `GetProcAddress(ntdll, "__wine_unix_call_dispatcher")` puis `MemoryWineLoadUnixLibByName` ou, en repli, `MemoryWineUnixFuncs` sur sa propre image (`FEX/Source/Windows/Common/FEXUnixLib.cpp:44-87`). voile-wine ne définit que `MemoryWineUnixFuncs` et `MemoryWineUnixWow64Funcs` (`W/include/winternl.h:2439-2440`) : le repli s'applique, il faut une bibliothèque Unix compagnon (nom attendu par convention : `libarm64ecfex.so`) ;
- demande à cette bibliothèque Unix des services Linux (`prctl(PR_SET_MEM_MODEL)`, `PR_ARM64_SET_UNALIGN_ATOMIC`, `PR_SET_VMA`, `shm_open`, `madvise`, `FEX/Source/Windows/UnixLib/FEXUnixLib.cpp:29-110`) qui doivent être remplacés sur macOS.

### 4.4 Sources externes (consultées le 2026-10-02)

- Hangover, https://github.com/AndreRH/hangover, HEAD `56e385b7ff490edd281b288f420677093563560b` (README brut lu par `curl`). Citations : "we break out of emulation at the win32 syscall or wine unix call level for performance reasons", "everything Unix related is never emulated", "For 64-Bit we're using the ARM64EC ABI combined with an emulator (FEX)", "Current main focus is to run x86_64 and i386 Windows applications on ARM64 Linux". Variables : `HODLL64` (`libarm64ecfex.dll` par défaut, ou `xtajit64.dll`) et `HODLL` (`wowbox64.dll` par défaut, `libwow64fex.dll`, `wow64cpu.dll`). Le README ne contient aucun des mots macOS, Darwin ou Apple (grep). Compilation : `docs/COMPILE.md`.
- Wiki FEX, https://wiki.fex-emu.com/index.php/Development:ARM64EC (page lue par `curl`) : "Unlike Linux FEX, the Windows FEX modules purely handle CPU emulation, with syscall translation being done on either the wine side (WOW64) or ABI (ARM64EC). This is similarly the case for wine's unixcalls and as a result native ARM64 variants can be used for all .so libraries (Vulkan, wine, etc.) dropping the need for an x86 rootfs and FEX-side thunking." ; "To actually use FEX create an empty registry key at HKLM\Software\Microsoft\Wow64\amd64 and set its value to libarm64ecfex.dll" ; "wow64 replacement with FEX on Windows doesn't work and causes all 32-bit applications to hang" ; FEX ne peut dépendre que de `ntdll` au chargement. Commande de configuration de Wine : `./configure --enable-archs=arm64ec,aarch64,i386 --with-mingw=clang` (fork `https://github.com/bylaws/wine`, branche `upstream-arm64ec`, HEAD `a6844d10622fc1a973ec1f22fc4f78a0fcd6cb29`) ; FEX compilé deux fois avec `-DCMAKE_TOOLCHAIN_FILE=../Data/CMake/toolchain_mingw.cmake` et `-DMINGW_TRIPLE=arm64ec-w64-mingw32` (64 bits) ou `aarch64-w64-mingw32` (WOW64), llvm-mingw de `bylaws/llvm-mingw`.
- Annonce Wine 10.0, https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/ANNOUNCE.md (lue par `curl`, tag `wine-10.0`, commit pelé `b073859675060c9211fcbccfd90e4e87520dc2c2`), section ARM64 : "The ARM64EC architecture is fully supported, with feature parity with the ARM64 support." ; "The 64-bit x86 emulation interface is implemented. This takes advantage of the ARM64EC support to run all of the Wine code as native, with only the application's x86-64 code requiring emulation." ; "No emulation library is provided with Wine at this point, but an external library that exports the emulation interface can be used, by specifying its name in the `HKLM\Software\Microsoft\Wow64\amd64` registry key. The FEX emulator implements this interface when built as ARM64EC." ; "ARM64 support requires the system page size to be 4K, since that is what the Windows ABI specifies. Running on kernels with 16K or 64K pages is not supported at this point." L'annonce de Wine 11.0 (tag `wine-11.0`, commit pelé `db11d0fe6a169c457e23d007e20404643d067aa8`, copie locale `W/ANNOUNCE.md:78-82`) ajoute la simulation de 4 Kio. Les pages `gitlab.winehq.org` et `www.winehq.org` ont refusé les requêtes automatiques (Anubis, 403).
- Madeira (iOS : Wine 11.4 en ARM64EC, FEX, DXMT), https://github.com/willfaust/Madeira, HEAD `ca3183ea3dfb0fd706aff1bea2abb871b5d27aec` ; forks `https://github.com/willfaust/wine` (HEAD `abf22e09603014b8f55bb79bd1cf6badf96c34ca`) et `https://github.com/willfaust/FEX` (HEAD `26859e184ad90f0e811d7f8bbd943a4b1573a2c3`). Pages de pull requests lues par un outil de résumé : un seul processus Mach avec wineserver en thread, JIT sous débogueur ; FEX #1 (suivi du code automodifiable sur pages de 16 Kio, double mapping du JIT, transitions ARM64EC qui passaient par des exceptions Mach), FEX #2 (fenêtre invité 32 bits avec registre de base `x19`), Madeira #60 (réécriture binaire de `x18` vers `TPIDRRO_EL0` plus un emplacement TSD, dans `virtual_ios.c`). EXTERNE, non reproduit ici. C'est une preuve d'existence sur iOS, pas sur macOS.

## 5. Options de franchissement entre Windows, Linux et macOS

### 5.1 Table

Légende : P = code PE x86-64 (Windows), E = code ELF x86-64 (Linux), M = code macOS (Mach-O, frameworks). "Rosetta" = conception actuelle, "FEX" = futur hôte arm64.

| # | Franchissement | Mécanisme (preuve) | Etat | Avantages | Inconvénients |
|---|---|---|---|---|---|
| F1 | P vers M, service NT | stub NT, `call [0x7ffe1000]`, dispatcher (`asm.h:287-301`, `signal_x86_64.c:3226`) | existe (Rosetta) | pas de piège, coût d'un appel, sémantique NT complète, tables extensibles (index 2 et 3) | assembleur x86-64 ; hors de portée d'un hôte arm64 (remplacé par `signal_arm64.c` fondé sur `x18`) |
| F2 | P vers M, `syscall` brut de code tiers | SIGSYS puis `sigsys_handler` (`signal_x86_64.c:2834`) | existe (Rosetta ; mesuré sur macOS 27.0.1, la source parle de macOS 14 et plus) | capte les syscalls directs d'anticheats et de runtimes | un signal par appel, bidouilles MXCSR (CW 24256), disparaît sous FEX où `syscall` est un point du JIT |
| F3 | P vers M, table unixlib | `NtQueryVirtualMemory(MemoryWineUnixFuncs)` puis `__wine_unix_call` (`virtual.c:837-860,6200`, `signal_x86_64.c:3552`) | existe | sans piège, par DLL, utilisé par winemac, D3DMetal, lsteamclient | exige une DLL PE builtin avec `.so` compagnon ; pile noyau et cadre incomplet à chaque appel |
| F4 | M vers P, rappels | `KiUserCallbackDispatcher`, `KiUserExceptionDispatcher`, `KiUserApcDispatcher`, `KeUserModeCallback` (`loader.c:2026-2041`) | existe | modèle Windows exact | pile utilisateur manipulée ; conventions différentes entre x86-64 et arm64ec |
| F5 | P vers P, redirection d'exports | réécriture `mov rax,imm64 ; jmp rax` ou piège d'exécution (`loader.c:1139-1253`, `WINESTEAMNOEXEC`) | existe (steamclient) | trivial | mprotect RWX sur du code ; fragile avec Rosetta (CW 18947) et avec le suivi de code d'un JIT |
| F6 | P vers E, appel de fonction direct | thunk MS x64 vers SysV dans le même espace d'adressage ; modèle `GPT_ABI_WRAPPER` (`unix_private.h:601-638`, test d'adresse de l'appelant) | à écrire | coût d'un appel ; le modèle de thunk existe déjà | ABI différentes (registres, alignement de pile, `xmm6-15` non volatils côté MS) ; TLS ELF absent sous Rosetta |
| F7 | P ou E vers M, `syscall` Linux | SIGSYS (Rosetta, MESURÉ) ou décodage JIT (FEX, `HandleSyscall`) vers un tableau de services Linux (section 2.9). L'appelant réel est le bootstrapper PE d'EAC (319, 1, `/proc`), puis le module ELF | à écrire | sémantique Linux sans VM ; point d'insertion unique | numéros recouverts avec NT : aiguillage par adresse d'appelant obligatoire ; `clone`, `futex`, `mmap`, `ptrace`, `/proc` à traduire ; chaque appel coûte un signal sous Rosetta |
| F8 | E vers P, rappels | thunk SysV vers MS, pile préparée comme pour `KiUserCallbackDispatcher` | à écrire | | mêmes ABI que F6 ; exceptions : le déroulement de pile mélange DWARF (ELF) et `.pdata` (PE) |
| F9 | P/E (émulés) vers M (natif arm64), syscall d'un invité FEX | `STATUS_EMULATION_SYSCALL` via `KiUserExceptionDispatcher` (`signal_arm64ec.c:1282`, `FEX Module.cpp:516-536`) | existe côté Wine et FEX, jamais exécuté sur macOS | sans signal Unix, TEB réelle dans `gs`, `%fs` libre | exige Wine arm64 sur macOS : `x18`, zone basse, pages 16 Kio, `server/mach.c` x86 seulement |
| F10 | invité FEX vers natif arm64 (appel direct) | `__os_arm64x_dispatch_call_no_redirect`, `DispatchJump`, `ExitToX64` (`xtajit64.spec`) | existe (interface) | appels natifs sans émuler les bibliothèques (Metal, Cocoa) | thunks par fonction ; données partagées entre ABI |
| F11 | processus Windows vers wineserver | socket Unix, mémoire partagée USD, ports Mach et msync (`server.c:1473-1519`, `server/mach.c`, `msync.c`) | existe | isole l'état NT | `mach_vm_*` sur un autre processus ; registres de debug faux sous Rosetta ; à porter pour arm64 |
| F12 | pont par processus auxiliaire | un processus Rosetta ou FEX distinct héberge E (ou D3DMetal) et parle par mémoire partagée et sockets | à écrire | contourne les contraintes d'architecture (D3DMetal x86_64 seul, zone basse, `%fs`) | latence, synchronisation, duplication d'espace d'adressage ; EAC attend un seul processus |
| F13 | fautes par exceptions Mach au lieu de signaux | `thread_set_exception_ports` | non utilisé par Wine | granularité fine, évite l'interaction avec les signaux de FEX | non fournies par Wine ; EXTERNE : Madeira l'emploie pour les transitions ARM64EC |
| F14 | moteur Hypervisor.framework par processus (HyperLinux) | lecture seule des sources pour les contrats d'ABI | exclu par la consigne | | non retenu |

### 5.2 Constat transverse

- Le seul franchissement déjà complet et exercé en production est F1+F3+F4 sous Rosetta. Toute conception arm64 doit remplacer le noyau de F1/F2 par F9 et reconstruire F11.
- Les numéros Linux et NT se recouvrent : F2/F7 ne coexistent que par un aiguillage par adresse. Le dispatcher de Wine contient déjà la structure qu'il faut (table de services par index, cadre de syscall) ; il manque la plage et la conversion d'arguments.
- Le TLS ELF (`%fs`) est impossible sous Rosetta et naturel sous FEX. Les ELF des tests initiaux doivent être sans libc ou n'utiliser aucun TLS.

### 5.3 Les trois candidats les plus prometteurs pour le premier témoin

Contrat commun de sortie : le PE écrit, par ordre, une ligne `PE`, une ligne `ELF` (écrite par l'ELF avec un `write` Linux), une ligne `MAC` (donnée obtenue du système macOS, vérifiable de l'extérieur) et une ligne finale `PE`. Le validateur extérieur compare la valeur `MAC` à une commande macOS (`ps`, `sysctl`) et exige l'ordre des lignes.

**T1. Un PE qui émet des `syscall` Linux puis charge un ELF statique qui en émet aussi, aiguillés vers macOS (Rosetta, runtime actuel plus un correctif de `ntdll.so`)**
- Programme : `crossing.exe` (llvm-mingw, x86_64). Étape 1 : comme le bootstrapper d'EAC, il émet lui-même des `syscall` Linux depuis une fonction de pont d'appel : `getpid` (39), `write` (1), `memfd_create` (319) suivi d'un `write` du module dans le memfd. Étape 2 : il lit un ELF `ET_EXEC` statique x86-64 sans libc (clang `--target=x86_64-linux-gnu -ffreestanding -nostdlib -static`, lié à une adresse haute, sans TLS), le mappe par `VirtualAlloc` et `VirtualProtect`, prépare une pile et saute à l'entrée. L'ELF exécute `write(1, ...)`, `getpid`, `uname`, puis revient au PE. Les deux sources de `syscall` Linux, PE et ELF, sont couvertes.
- Correctif Wine : plages enregistrées (fonction de pont du PE de test et segments de l'ELF) par une variable d'environnement ou un nouvel appel unixlib ; `sigsys_handler` aiguille ces `syscall` vers un point d'entrée Linux et laisse tous les autres au chemin NT (section 2.9). Un second PE `control.exe`, qui émet un `syscall` NT direct (`NtClose` avec ses propres octets de stub), doit continuer à passer par le chemin NT : c'est le test de non-régression de l'aiguillage.
- Preuve : ligne `PE` (`NtCurrentTeb()->ClientId`, machine PE, pid Windows), ligne `ELF` (écrite par l'ELF avec un `write` Linux ; son `getpid` Linux renvoie le pid macOS réel, différent du pid Windows), ligne `MAC` (valeur de `uname`/`sysctl` rendue par le noyau macOS via la table Linux, comparée de l'extérieur à `ps -p <pid>`). Les trois couches sont distinctes par construction : pid Windows, pid Unix, valeur du noyau.
- Avantages : tout existe sauf un correctif de quelques centaines de lignes ; exerce exactement le piège SIGSYS mesuré et le scénario d'EAC (le PE émet lui-même des `syscall` Linux) ; sert de test de non-régression pour les `syscall` NT directs de code tiers. Limites : Rosetta, pas FEX ; ELF sans glibc ; pas de `/proc/self/maps` synthétique ; une partie "macOS" en lecture seule.

**T2. Un couple PE builtin plus bibliothèque Unix Mach-O compilé hors arbre avec la recette de Voile**
- Programme : `crossing.dll` (PE builtin) et `crossing.so` (Mach-O x86_64) construits comme `lsteamclient` (`V/scripts/build.sh:297-324` : Makefile de Wine réutilisé avec `UNIX_LIBS` et makedep) ; `crossing.exe` appelle `WINE_UNIX_CALL`. Côté Unix, la fonction appelle `sysctlbyname("machdep.cpu.brand_string")`, `sysctl.proc_translated`, `proc_pidpath`, `mach_absolute_time`, puis (étape 2) charge et exécute le même ELF que T1 depuis la bibliothèque Unix.
- Preuve : la ligne `MAC` affiche des valeurs que seul le noyau macOS fournit à un processus traduit : `sysctl.proc_translated=1`, `hw.optional.arm64=1`, `machdep.cpu.brand_string=Apple M5 Max`, `hw.model=Mac17,7`, `kern.osversion=26A434` (MESURÉ en annexe A.5 : un processus Rosetta voit ces valeurs hôtes, alors que `hw.machine` et `uname -m` y valent `x86_64` et `hw.cachelinesize` 64), tandis que la ligne `PE` annonce AMD64 ; l'appel traverse `NtQueryVirtualMemory(MemoryWineUnixFuncs)` puis `__wine_unix_call_dispatcher`, vérifiable dans `WINEDEBUG=+module,+syscall`.
- Avantages : aucun changement de Wine, mécanisme déjà employé par winemac et D3DMetal, c'est la meilleure manière de produire le décor macOS de T1. Limites : ne couvre pas à lui seul la sémantique Linux.

**T3. Un hôte arm64 autonome avec FEXCore qui exécute les mêmes charges PE et ELF (sans Wine)**
- Programme : `fexdarwin-run` (en cours de construction par l'autre équipe, `reports/fexcore-darwin-port.md`) charge deux blobs x86-64 liés à des adresses hautes libres (mesurées : 0x110000000-0x180000000, 0x7000000000-0x7780000000, 0x7d80000000-0x7ffffe000000) : un PE minimal dont les appels de service sont des `syscall` à identifiants NT, et le même ELF que T1. L'hôte traduit chaque `syscall` selon la plage de l'appelant, écrit par `write(2)` macOS et affiche les mêmes quatre lignes.
- Preuve : sortie octet pour octet identique à T1 (les lignes `MAC` mises à part si elles dépendent du pid) ; le `getpid` obtenu est celui du processus arm64 hôte, ce qui prouve que l'ELF et le PE sont exécutés par le JIT dans un processus macOS natif.
- Avantages : premier contact de FEXCore avec un `syscall` invité sur macOS, sans Wine ; sert de sosie de T1 (même charge, deux moteurs). Limites : pas de Wine, donc pas de TEB réelle ni de D3DMetal ; la zone basse (USER_SHARED_DATA) est contournée par construction.

Ordre recommandé : T2 d'abord (le décor macOS), T1 ensuite (aiguillage Linux), T3 en parallèle dès que l'hôte FEXCore tourne, pour comparer les sorties.

## 6. Inconnues, risques et questions ouvertes

1. Non mesuré : coût d'un aller-retour SIGSYS sous Rosetta (latence par appel) ; indispensable avant de dimensionner F2/F7.
2. Non mesuré : comportement de `thread_fast_set_cthread_self` (classe machdep 3) pour poser la TEB comme base GS sous Rosetta ; le cas 5 de la sonde a bloqué, probablement parce que la base GS valait 1 (non vérifié). Un échange de GS à la manière amont est peut-être possible sous Rosetta, mais ni son fonctionnement ni son coût ne sont connus.
3. Non vérifié : si les DLL PE `d3d10.dll`, `d3d10core.dll` et `d3d12core.dll` sont aussi des enveloppes D3DMetal (imports mesurés seulement pour `d3d11`, `dxgi`, `d3d12`, `nvapi64`).
4. Non exécuté : aucun Wine, aucun jeu, aucun EAC n'a été lancé par cette tâche (consigne). Les faits sur le dispatcher viennent de la lecture et du désassemblage, pas d'une trace `WINEDEBUG=+syscall`.
5. Risque EAC : Wine macOS fabrique déjà des réponses (registres de debug acceptés sans effet sous Rosetta, `signal_x86_64.c:1149-1154,1395-1400` ; zéros renvoyés par `server/mach.c:192-197`) et le commit Obelisk `bfd51d7f` ("Read cached debug registers when no breakpoint is enabled", venu de Proton) le constate dans son message : "under Rosetta the server only ever answers zeroes" ; il répond depuis une copie locale des registres tant qu'aucun point d'arrêt matériel n'est actif, et il écarte volontairement le changement serveur 2105dac8b57e parce que "setting another thread's debug registers fails" sous Rosetta. La consigne interdit les réponses de succès fabriquées pour EAC : ces chemins sont à auditer avant tout test.
6. `Voile/current` (build de développement) et `v26.3.0-4` différent ; la décision d'architecture doit citer lequel est la base. Le `current` n'a pas de `runtime.json` ; son commit exact n'est pas enregistré (DEDUIT : proche de `e82937c9`).
7. Le désaccord entre `ANNOUNCE.md:63-64` (échange de GS sur macOS) et le code CrossOver est à clarifier si la conception veut reprendre le correctif amont.
8. Les sources externes de la section 4.4 sont des pages GitHub et wiki lues par un outil de résumé ; Wine (gitlab et winehq.org) a refusé les requêtes automatiques (403). Les citations de l'annonce 10.0 viennent du miroir GitHub.
9. Questions pour l'architecte : (a) la conception cible-t-elle un Wine arm64 natif (Mach-O arm64) plus FEXCore dans le processus, ou garde-t-elle Rosetta pour Wine et déporte-t-elle ELF et FEX dans un processus auxiliaire ? (b) quel remplaçant de D3DMetal (x86_64 seul) pour Metal ? (c) la TEB dans `TPIDRRO_EL0`/TSD ou un registre réservé du JIT ? (d) fenêtre d'adresses invitée décalée (donc FEXCore modifié) ou processus Rosetta pour tout ce qui exige la zone basse ?

## Annexe A. Sondes (sources utiles et résultats)

Toutes compilées avec `clang -arch ... -isysroot $(xcrun --sdk macosx --show-sdk-path)` dans le répertoire temporaire de session, lancées par un pilote Python (un cas par processus, délai 8 s).

### A.1 `rawsys.c` (x86_64, Rosetta) : voir tableau de la section 2.8

```c
static void on_signal(int sig, siginfo_t *si, void *ctx_) {
    ucontext_t *uc = ctx_; unsigned long long rip = uc->uc_mcontext->__ss.__rip;
    const unsigned char *p = (const unsigned char *)(rip - 2);
    fprintf(stderr, "sig=%d si_code=%d si_addr=%p rip=%llx bytes_before_rip=%02x%02x rax=%llx rcx=%llx r11=%llx\n",
            sig, si->si_code, si->si_addr, rip, p[0], p[1], uc->uc_mcontext->__ss.__rax,
            uc->uc_mcontext->__ss.__rcx, uc->uc_mcontext->__ss.__r11);
    uc->uc_mcontext->__ss.__rax = 0xdead0000ULL | (unsigned)sig;   /* reprend après l'instruction */
}
__attribute__((noinline)) static long do_syscall(long nr, long a1, long a2, long a3, int *cf) {
    long ret; unsigned char c;
    __asm__ volatile("syscall\n\tsetc %1" : "=a"(ret), "=q"(c) : "a"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory", "cc");
    *cf = c; return ret;
}
```

Sortie type (cas 1) : `HANDLER sig=12 si_code=0 si_addr=0x1007557d6 si_errno=0 rip=1007557d6 bytes_before_rip=0f05 rax=27 rcx=1007559d0 r11=20084b890`.

### A.2 `x18probe2.c` (arm64 natif)

```c
static inline unsigned long get_x18(void){unsigned long v;__asm__ volatile("mov %0, x18":"=r"(v));return v;}
static inline void set_x18(unsigned long v){__asm__ volatile("mov x18, %0"::"r"(v));}
static long raw_getpid(void){register long x16 __asm__("x16")=20;register long x0 __asm__("x0");
    __asm__ volatile("svc #0x80":"=r"(x0):"r"(x16):"memory","cc");return x0;}
/* set_x18(0x1122334455667788); raw_getpid(); get_x18() == 0 : les trois essais, la trappe Mach et la faute */
```

### A.3 `addrprobe.c` (arm64 et x86_64) et pages

Sorties : voir section 3.4. Programme x86_64, cas 99 de `rawsys` : `proc_translated=1 getpagesize=4096 sysconf=4096 vm_page_size=4096 vm_kernel_page_size=16384` ; `two 4K mmaps: a=0x10824d000 b=0x10824e000 diff=4096` ; `mprotect(4K sub-page of 64K map, PROT_NONE) rc=0`.

### A.4 `fsgs.c` (x86_64, Rosetta)

Cas 0 : `%gs:0` lisible. Cas 1 : `%fs:0` SIGSEGV `si_addr=0x0`. Cas 2, 3, 4 : `wrfsbase`, `rdfsbase`, `wrgsbase` SIGILL. Cas 5 : `CPUID.7.0:EBX = 0`.

### A.5 `sysq.c` (même source compilée arm64 et x86_64)

| Clé | arm64 natif | x86_64 sous Rosetta |
|---|---|---|
| `sysctl.proc_translated` | 0 | 1 |
| `hw.machine` et `uname.machine` | arm64 | x86_64 |
| `hw.cachelinesize` | 128 | 64 |
| `hw.optional.arm64` | 1 | 1 |
| `machdep.cpu.brand_string` | Apple M5 Max | Apple M5 Max |
| `hw.model` | Mac17,7 | Mac17,7 |
| `kern.osproductversion`, `kern.osversion`, `kern.osrelease` | 27.0.1, 26A434, 27.0.0 | identiques |
| `hw.ncpu`, `hw.memsize` | 18, 38654705664 | identiques |

## Annexe B. Commandes de lecture du runtime (aucune écriture, aucune exécution de Wine)

- `lipo -archs`, `file`, `otool -L`, `otool -l`, `nm -gU`, `nm -u`, `otool -tvV -p ___wine_syscall_dispatcher` sur les fichiers de `RT` et `CUR`.
- `strings -a` sur `bin/wine`, `lib/wine/x86_64-unix/wine`, `ntdll.so`, `libd3dshared.dylib`.
- Scripts Python de session : lecture des en-têtes PE (machine, magie, signature "Wine builtin DLL") de `lib/wine/x86_64-windows` (766 fichiers) et `i386-windows` (815), des octets des stubs d'export et des tables d'import.
- `git ls-remote` pour les empreintes de commit des dépôts externes ; `git -C W log`, `describe`, `show --stat c6e20da3`.
- Aucun fichier de session, de jeton, de registre ou de données utilisateur n'a été ouvert ; seuls les listings de dossiers et `runtime.json` ont été lus.
