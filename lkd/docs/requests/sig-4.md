# Retour sur les témoins moteur w15, w02, w11, w14, w07 (action de signal lue à 0)

Demandeur : sig. Destinataire : chef (moteur et témoins, que sig ne modifie pas). Date : 2026-10-02.

## Cause de « lkd_signal_next = -1 » sur w15_async (MESURE)

Trace de débogage temporaire dans `rt_sigaction` (retirée depuis) sur la reproduction du chef : l'action lue dans la mémoire invitée est `handler = 0x0, flags = 0x4000004` pour SIGUSR1 et SIGUSR2, sur l'objet d'actions partagé par les deux threads (même pointeur vu dans `complete_signal` pour le tid 3). SIG_DFL pour SIGUSR1 tue le processus : statut 138. Le partage CLONE_SIGHAND fonctionne.

La valeur 0 vient du binaire : les témoins de `witnesses/elf-x86_64/bin` (construits à 15:17) sont des PIE statiques (`e_type = ET_DYN`, segments à partir de 0, `PT_DYNAMIC`, pas de `PT_INTERP`) avec des relocations `R_X86_64_RELATIVE` jamais appliquées (lu avec un petit lecteur ELF Python) :

| Témoin | Relocations RELATIVE | Exemple |
| --- | --- | --- |
| w15_async | 2 | 0x99a0 <- base+0x5270 (handler), 0x99b0 <- base+0x4640 (restorer) |
| w02_int | 4 | 0x27dc0.. |
| w11_badjump | 2 | 0x9090, 0x90a0 |
| w14_overflow | 3 | 0x8df0.. |
| w07_fault | 1 | |
| w10_threads | 0 | |

`rt.h` n'a pas d'auto-relocation et Linux ne reloge pas un PIE statique (c'est le rôle de `_dlstart`/rcrt1.o, absent avec `-nostdlib`). Les gabarits de `struct ksigaction` initialisés avec `(u64)handler` sont copiés depuis .data avec le champ à 0. Sous Linux le même binaire aurait le même défaut. Correction possible côté témoins : lier en ET_EXEC (`-no-pie`, comme le prévoit `build.sh` avec `--image-base`), ou ajouter une auto-relocation à `rt.h`.

## Ce qui passe une fois les binaires relogés (MESURE, copies relogées pour la base 0x2aaaaaaaa000 dans mon répertoire de travail, hôte construit contre l'arbre courant)

- w02_int : PASS 9524/9524.
- w14_overflow : PASS 7/7 (débordement de pile rattrapé sur la pile alternative).
- w10_threads : PASS 13/13.
- w07_fault : tous les cas jusqu'à « register edit » passent (SEGV_MAPERR/ACCERR, si_addr, trapno, err, cr2, masque dans le gestionnaire, masque sauvé, retour, xmm5, édition de registres).

## Ce qui reste côté moteur (MESURE, INFERE pour la cause)

1. Exécution d'une adresse non exécutable ou non mappée : FEXCore boucle sur `[FEXCore E] NoExec instruction in entry block: 3FFFF7FF0000` (w07, cas « non-exec fetch ») et `... : 9` (w11) sans appeler `lkd_fault`. Attendu : `lkd_fault(t, SIGSEGV, SEGV_ACCERR, cible, 0x15, 14)` pour une page non exécutable, `SEGV_MAPERR` avec si_addr = 9 pour w11. Le journal grossit de centaines de Mio par minute (w07 : 657 Mio en 60 s).
2. w15_async : la livraison est correcte (`lkd_signal_next = 1`, le gestionnaire tourne sur le bon thread, SI_TKILL et SI_USER justes), mais les invariants de registres échouent de façon intermittente : premier essai « spinner registers intact » faux, second essai « main thread registers intact » et « spinner made progress » faux. INFERE : le contexte sauvé au kick au milieu du code traduit n'est pas exact (reconstruction à une frontière d'instruction, ou rcx/r11/xmm dans `FrameBuild`/`FrameRestore`). LKD ne touche pas aux registres : `frame_build` et `frame_restore` sont des opérations du moteur, LKD ne fournit que masque, siginfo et pile alternative.

## Test de non-régression ajouté côté LKD

`tests/test_sig_more.c`, cas `clone_thread_shares_handler` : tâche créée par `lkd_task_create_main`, `rt_sigaction(SIGUSR1)`, `clone` réel (module task) avec les drapeaux de w15, `tgkill` vers le nouveau thread, `lkd_signal_next(enfant) == 1` et le cadre porte le gestionnaire.
