# Demande mm-2 (core) : mode de la fenêtre invitée et points d'entrée du chargeur dans mm.h

## 1. Sens de `lkd_config.guest_window_base` (lkd.h, section 4)

`lkd.h` dit que mm réserve la fenêtre et pose `lkd_guest_base` dans `lkd_init()`, et `CONVENTIONS.md` 2.5 dit que `lkd_guest_base` vaut 0 dans les tests. Les deux ne tiennent ensemble que si un réglage distingue le moteur des tests. Comportement implémenté (`src/mm/mm.c` lkd_mm_init, `src/mm/window.c`) :

- `guest_window_base != 0` (moteur) : `lkd_init` réserve `guest_window_bytes` (défaut 2^46) à cette base, sinon au premier candidat libre (8 Tio, 16 Tio, 32 Tio, 0x7d80000000), sinon n'importe où aligné sur 1 Gio, et pose `lkd_guest_base`. Le moteur copie `lkd_guest_base` dans `FEXCore::GuestBase` APRÈS `lkd_init`.
- `guest_window_base == 0` (tests) : mode identité, `lkd_guest_base` reste 0, la fenêtre (au plus 4 Gio) est réservée par le premier `lkd_mm_create()` et les adresses invitées sont les adresses hôtes de la fenêtre.

Demande : écrire cette règle dans le commentaire de `lkd_config.guest_window_base` (« 0 = mode test identité ; le moteur passe une base, 0x80000000000 recommandé ») au lieu de « 0 = module's choice ».

## 2. Fonctions à ajouter à `src/include/mm.h`

Elles existent (déclarées dans `src/mm/lkd_mm_ext.h`) :

```c
void lkd_mm_set_brk_start(lkd_mm *mm, uint64_t start_brk);   /* le chargeur ELF après exec */
void lkd_mm_set_stack_start(lkd_mm *mm, uint64_t start_stack); /* adresse qui nomme [stack] */
void lkd_mm_set_mmap_base(lkd_mm *mm, uint64_t base);          /* haut de la recherche descendante */
uint64_t lkd_mm_window_guest_lo(void);                         /* 0 en production */
uint64_t lkd_mm_window_guest_hi(void);                         /* TASK_SIZE effectif = taille de fenêtre */
```

Sans `lkd_mm_set_brk_start`, `brk` lit `t->proc->layout->start_brk` (posé par `lkd_proc_set_layout`) au premier appel ; si rien n'est connu, `brk` rend 0 (tas absent, glibc passe alors par mmap).

## 3. Taille de l'espace invité

Avec une fenêtre de 2^46, l'invité voit TASK_SIZE = 0x400000000000 et non 0x7ffffffff000. La zone haute que le préchargeur Wine x86-64 tente de réserver (0x7ffffe000000) n'existe donc pas pour l'invité. LU, non mesuré avec Wine : le préchargeur ignore les réservations qui échouent. A vérifier au premier lancement de Wine ; une fenêtre plus grande (jusqu'à 0x77fffe000000 octets avec la base 8 Tio) est possible si `guest_window_bytes` le demande.


## Traitée le 2026-10-02 (mainteneur d'intégration)

Point 1 : commentaire de `guest_window_base` réécrit dans `include/lkd/lkd.h`. Point 2 : les cinq fonctions sont déclarées dans `src/include/mm.h` (remplaçants faibles dans `stubs_mm.c`) et retirées de `lkd_mm_ext.h`. Point 3 : limite confirmée par la campagne de sondes EAC (MAP_FIXED à 0x7ffff7ff0000 impossible, voir `docs/status/ptrace.md`).
