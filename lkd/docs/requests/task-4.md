# Demande task-4 au module mm : remise à zéro de l'espace d'adressage en temps proportionnel aux VMA

Date : 2026-10-02. Auteur : module task, à la suite du blocage signalé par le chef de projet (`/bin/sh -c '/bin/true; echo status=$?'` sous le vrai moteur).

## Constat (MESURE)

1. Sous le vrai moteur (fenêtre de 2^46 octets), `sample` du processus hôte de l'enfant de vfork (pid invité 3) pendant le blocage : `do_execve -> lkd_exec_commit -> lkd_mm_exec_commit -> lkd_mm_reset_for_exec -> mm_unmap_locked -> mm_host_cleanup -> page_has_vma -> vt_first_end_after`, état `R` (il calcule, il n'est pas bloqué). L'enfant ne sort donc jamais et le shell attend à juste titre dans `wait4`. Avec `LKD_TRACE=1`, l'enfant finit, mais le shell reste ensuite dans son propre `exit_group` (`lkd_mm_task_exit -> lkd_mm_put -> mm_destroy`, même parcours). Le témoin `w02_int` lancé par l'agent sig montrait la même pile (`lkd_mm_task_exit -> lkd_mm_put`).
2. Cause (LU, `src/mm/mm.c`, `mm_host_cleanup`) : `mm_unmap_locked(mm, g_lo, g_hi)` appelle `mm_host_cleanup` sur toute la fenêtre, dont la boucle intérieure `while (q < pe && !page_has_vma(mm, q)) q += mm_hps;` avance d'une page hôte de 16 Kio à la fois : 2^46 / 2^14 = 2^32 itérations, chacune avec une recherche dans l'arbre.
3. Correctif essayé dans une COPIE de l'arbre (rien n'a été modifié dans `src/mm`) : sauter directement à la page du prochain VMA :
   ```c
   mm_vma *nv = vt_first_end_after(&mm->vt, p);
   uint64_t q = nv ? HP_DOWN(nv->start) : pe;
   if (q > pe) q = pe;
   if (q <= p) q = p + mm_hps;
   ```
   Avec cette copie, le moteur exécute `/bin/sh -c '/bin/true; echo status=$?'` en 1 s, avec ou sans `LKD_TRACE`, affiche `status=0` et termine par `exit_group(0)`.
4. Test de non-régression ajouté : `tests/test_task_bigwin.c` (vfork, execve, wait4 puis exit_group avec une fenêtre de 2^46 octets à 8 Tio). Arbre actuel : 20 114 ms, ÉCHEC (borne 5 s). Copie corrigée : 524 ms, PASSE.

## Demande

Rendre `mm_host_cleanup` (et tout parcours de la fenêtre entière : `mm_destroy`, `lkd_mm_reset_for_exec`) proportionnel au nombre de VMA et de pages hôtes réellement utilisées, par exemple avec le saut ci-dessus. `mm_host_reset` sur un grand intervalle vide reste un seul appel.

Remarque : `lkd_mm_window_size()` n'est pas défini par mm (le remplaçant du core rend 0) ; le test utilise `lkd_guest_base != 0` pour savoir si la fenêtre est réservée.


## Traitée le 2026-10-02 (mainteneur d'intégration)

Corrigé par mm (`mm_host_cleanup` en O(VMA)). MESURE : `test_task_bigwin` passe en moins d'une seconde dans `build-integration`.
