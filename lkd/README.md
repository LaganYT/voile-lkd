# LKD, Linux kernel on Darwin

Couche syscall Linux x86-64 pour un moteur CPU (FEXCore) hébergé dans un processus macOS arm64. Elle est appelée par le moteur à chaque `SYSCALL` invité. Décision : `reports/00-architecture-decision.md` (option O3). Aucune machine virtuelle, aucun Hypervisor.framework. Les réponses de `/proc`, de ptrace et des syscalls viennent de l'état réel du runtime ; une fonction absente répond `ENOSYS` de façon visible.

## Arborescence

```
CMakeLists.txt, build.sh, check.sh   construction et tests (cmake privé, ninja)
include/lkd/                         API publique (moteur <-> noyau)
  lkd.h        init, tâche principale, lkd_syscall, signaux, fork, exec, ptrace, services du moteur
  abi_x64.h    ABI Linux x86-64 (LX_*, struct lx_*), static_asserts de tailles et décalages
  xlate.h      traductions Linux <-> Darwin (errno, drapeaux, signaux, sockets, stat)
  syscall_nr.h généré par tools/gen_syscall_table.py (Linux v6.12, sha256 dedans)
src/include/                         contrats internes entre modules (task, fd, mm, vfs, sig, futex, ptrace, net)
src/core/                            dispatch, trace, usermem, bloquants, fork/exec, remplaçants faibles
src/<module>/                        mm fs procfs task sig net ptrace : un agent par dossier
tests/ltest.h, tests/support/        cadre de test et moteur factice
tests/test_<module>_*.c              un exécutable par fichier
docs/CONVENTIONS.md, OWNERS.md       règles et propriété des syscalls
docs/status/<module>.md              statut de chaque module, mis à jour à chaque jalon
docs/requests/<module>-<n>.md        demandes de changement d'un fichier partagé
tools/                               générateur de table, vérification des propriétaires, lanceur de tests
NOTICE                               attribution Apache-2.0
```

## Comment travaille un agent de module

1. Lire `OWNERS.md`, `include/lkd/lkd.h`, l'en-tête de son module dans `src/include/`, `docs/CONVENTIONS.md`.
2. N'écrire que dans `src/<module>/`, `tests/test_<module>_*.c`, `docs/status/<module>.md`, `docs/requests/<module>-*.md`.
3. Définir `lkd_<module>_init()` (version forte, le noyau a une version faible vide), y enregistrer ses syscalls avec `LKD_REGISTER(nom)`. Définir les symboles que l'en-tête du module annonce « OWNER: <module> » : les définitions faibles de `src/core/stubs_*.c` s'effacent d'elles-mêmes.
4. Chaque comportement a un test (`tests/test_<module>_<sujet>.c`) qui cite le comportement Linux modélisé.
5. Besoin d'un changement dans un fichier partagé : écrire `docs/requests/<module>-<n>.md` et travailler autour en local.
6. `./check.sh` après chaque jalon (compile, lance tous les tests, une ligne PASS ou FAIL par programme). Écrire `docs/status/<module>.md` à chaque jalon : ce qui existe, ce qui passe, la suite, les lacunes.

## Construire et tester

```
./build.sh          # configure et compile dans ./build (liblkd.a, tests)
./check.sh          # compile puis lance chaque tests/test_*.c sous 120 s, compte final
tools/runtest.sh 30 build/tests/test_core_usermem   # un seul programme avec limite de temps
python3 tools/owners_check.py --list-unowned         # contrôle OWNERS.md, liste les syscalls sans propriétaire
LKD_TRACE=1 build/tests/<test>   # trace les syscalls ; LKD_TRACE=2 ajoute les lignes d'entrée
```

Outils : cmake 4.4.3 privé (`tools/venv/bin/cmake`), ninja (`/opt/homebrew/bin/ninja`), clang 21, SDK macOS 27. Aucun `git commit`, `push`, branche ni stash.

## Intégration dans le moteur

Lier avec `-Wl,-force_load,liblkd.a`. Appeler `lkd_init_config` (facultatif), `lkd_init(&ops)`, `lkd_task_create_main(&spec)`, `lkd_set_current(t)`, puis `lkd_syscall` à chaque SYSCALL. Le contrat complet est en tête de `include/lkd/lkd.h` et dans `docs/CONVENTIONS.md` §5.
