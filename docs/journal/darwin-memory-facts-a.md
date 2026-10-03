# Faits macOS 27 arm64 sur l'espace d'adressage, la taille de page et les fonctions CPU (groupes A, B, E)

Etat du document: premier jet du 2026-10-02, a completer au fur et a mesure des mesures. Les sections marquees "a completer" n'ont pas encore de mesure.

## Contexte et machine

- Machine: Apple M5 Max, macOS 27.0.1 (build 26A434), noyau xnu-13432.1.9~1/RELEASE_ARM64_T6050, 36 GiB de RAM (hw.memsize 38654705664). MESURE (sysctl).
- `hw.pagesize` = 16384 et `vm.pagesize` = 16384. MESURE.
- Outils: Apple clang 21.0.0, SDK macOS 27. MESURE.
- Les programmes de test sont dans `witnesses/darwin-memory/` (prefixes a_, b_, e_), les journaux dans `evidence/darwin-memory/`.

## Resume des constats

A completer.

## A. Espace d'adressage d'un processus arm64

A completer.

## B. Taille de page

A completer.

## E. TSO et fonctions CPU

A completer.

## Implications for a 16 KiB-host FEXCore design

A completer.

## Limites, points non testables et questions ouvertes

A completer.

## Reproduction

A completer.

<!-- notes de travail (supprimees en fin de rapport) -->
- 2026-10-02 premiers constats: posix_spawnattr_set_4k_page_size_np existe dans spawn.h (macOS 26.0), accepte pour x86_64/Rosetta, EBADMACHO (88) pour tout arm64/arm64e. -pagezero_size < 4 Gio: ld l'accepte, le noyau refuse (EBADMACHO) tant que pagezero + glissement ASLR < 4 Gio. MH_PIE obligatoire (EBADEXEC 85 sinon). Glissement ASLR de l'executable: 0..80 Mio par pas de 16 Kio.
- FEX: Source/Common/HostFeatures.cpp lit les registres ID par mrs (Linux les emule); sous macOS il faudra les fabriquer depuis sysctl.
- FEX/AGENTS.md et CLAUDE.md: "AI must not be used to generate code for contributions to this project."
