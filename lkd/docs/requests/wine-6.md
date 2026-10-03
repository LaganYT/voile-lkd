# wine-6 : l'espace d'adresses invité s'arrête à 2^46 ; le bootstrapper EAC alloue à 0x7001_0000_0000 (Linux : 2^47)

Demandeur : agent wine-linux. Bloquant pour le bootstrapper EAC (cause de l'arrêt « Failed to initialize Wine helper buffer »).

MESURE :
- La boîte de dialogue que le bootstrapper affiche dans la fenêtre 290x146 (capture de la seule fenêtre wsd : evidence/wine-linux/eac-splash/t2-win583.png) dit « Easy Anti-Cheat Launch Error / Failed to initialize Wine helper buffer ». Code de sortie 201 (evidence/eac-runs/20261002T215740, 215925, 220301).
- WINEDEBUG +virtual (evidence/eac-runs/20261002T220659/guest.log, fil 0024 = RocketLeague_EAC.exe), juste avant l'erreur, dix appels :
  `NtAllocateVirtualMemory(GetCurrentProcess(), 0x700100000000, size 0x1000, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE)` puis 0x700200000000 ... 0x700a00000000 (pas de 2^40). Tous échouent : Wine journalise au démarrage `host addr space limit: 0x3fffffff0000` (sonde mmap de virtual.c), donc aucune adresse au-dessus de 2^46 n'est disponible.
- Sous Linux x86-64 (QEMU, GE-Proton, essais du 30 septembre) l'espace utilisateur va jusqu'à 0x7fffffffffff (47 bits) et ces adresses sont libres ; Windows x64 aussi (128 Tio).
LU : Wine (virtual.c) ne limite pas user_space_limit à 2^46 ; il plafonne au host_addr_space_limit mesuré par mmap. C'est l'espace invité de fexdarwin/LKD (fenêtre de 2^46, cf. message du lead sur les 524288 entrées) qui borne.
Demande : une fenêtre invitée de 2^47 (0x0000_0000_0000 .. 0x7fff_ffff_ffff), au minimum que 0x7000_0000_0000 .. 0x7fff_0000_0000 soit allouable. Les réservations de mm étant maintenant à protection maximale nulle, la taille de la fenêtre ne devrait plus coûter d'entrées noyau (à confirmer par le lead).
Témoin : petit programme Windows qui fait VirtualAlloc((void*)0x700100000000, 4096, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE) et imprime le résultat ; sous Linux natif il réussit.

Témoin exécuté (MESURE, logs/wine-linux-highalloc-001.log, src/wine-linux/tests/highalloc.c, dans notre Wine sous LKD) : VirtualAlloc RWX à adresse fixe réussit à 0x100000000000, 0x200000000000, 0x300000000000 et échoue (487 ERROR_INVALID_ADDRESS) à 0x400000000000 et au-delà, dont 0x700100000000 .. 0x700a00000000, les adresses du bootstrapper.
