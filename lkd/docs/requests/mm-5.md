# Demande mm-5 (core) : la récupération de faute des copies LKD doit passer par mm pour une page surveillée

Contexte : pages de code traduit protégées en écriture (`docs/status/mm.md`, `src/mm/lkd_mm_watch.h`). `lkd_user_write` passe déjà par `lkd_mm_check_range`, que mm a renforcé : il déprotège la page surveillée avant la copie. Une course reste : un autre thread du moteur peut traduire du code et surveiller de nouveau la page entre le contrôle et la copie. La copie fautes alors dans `lkd_uc_copy`.

## MESURE (scratch, sans toucher à src/core)

Copie sur une page surveillée sans contrôle préalable (aucune tâche liée) :

- `src/core/usermem.c` actuel : `lkd_user_write` rend -14 (EFAULT), 0 invalidation, page toujours surveillée.
- Avec le correctif ci-dessous : `lkd_user_write` rend 0, 1 invalidation, page déprotégée, la valeur est écrite.

Constat important pour le moteur (MESURE, ESR 0x92000046) : un store sur une page en lecture seule qui n'est pas encore résidente donne une faute de TRADUCTION (DFSC 0x06), pas une faute de permission (0x0f). Aucun filtre sur DFSC ne doit être appliqué : le filtre correct est « abort de données (EC 0x24) avec WnR = 1 ».

## Correctif exact (src/core/usermem.c)

```diff
--- a/src/core/usermem.c
+++ b/src/core/usermem.c
@@ -17,6 +17,7 @@
 
 #include "lkd_internal.h"
 #include "mm.h"
+int lkd_mm_code_write_fault(uint64_t gva); /* mm.h after the patch; weak 0 in stubs_mm.c */
 
 uint64_t lkd_guest_base;
 
@@ -74,6 +75,16 @@
     ucontext_t *uc = ctx;
     uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
     if (pc >= (uintptr_t)lkd_uc_copy_begin && pc < (uintptr_t)lkd_uc_copy_end) {
+        /* A store into a page whose write permission mm removed because it holds translated code
+         * (src/mm/watch.c): mm drops the translations and restores the permission; return without
+         * touching the context so the store is executed again. Only data aborts with WnR set (a store).
+         * The fault status is NOT filtered: macOS reports a store to a non-resident read-only page as a
+         * translation fault (DFSC 0x06, MESURE), a resident one as a permission fault (0x0f). */
+        uint32_t esr = uc->uc_mcontext->__es.__esr;
+        uint32_t ec = esr >> 26;
+        if ((ec == 0x24 || ec == 0x25) && (esr & (1u << 6)) &&
+            lkd_mm_code_write_fault(lkd_h2g((const void *)(uintptr_t)uc->uc_mcontext->__es.__far)) == 1)
+            return 1;
         void *lr = (void *)arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
         arm_thread_state64_set_pc_fptr(uc->uc_mcontext->__ss, lr);
         uc->uc_mcontext->__ss.__x[0] = (uint64_t)-1;
```

## Autres changements demandés

1. `src/include/mm.h` : ajouter `int lkd_mm_code_write_fault(uint64_t gva);` (puis retirer la déclaration locale du correctif) et `#include "../../src/mm/lkd_mm_watch.h"` n'est pas nécessaire : le moteur peut inclure `src/mm/lkd_mm_watch.h`, autonome et compatible C++.
2. `src/core/stubs_mm.c` : `LKD_WEAK int lkd_mm_code_write_fault(uint64_t gva) { (void)gva; return 0; }` et de même `lkd_mm_watch_code` (rend 0) pour les tests des autres modules. mm les définit fortes dans `src/mm/watch.c`.
3. Verrous : `lkd_mm_code_write_fault` prend le verrou de surveillance (mutex récursif, jamais tenu pendant du code invité, pendant `ops->invalidate_code` ni pendant une copie `lkd_user_*`) et le verrou de lecture des VMA. Le gestionnaire de signal appelle donc un mutex : acceptable parce que le code interrompu est du code invité ou `lkd_uc_copy` (jamais un code qui tient ces verrous). `ops->invalidate_code` est appelé depuis le gestionnaire : il ne doit prendre aucun verrou que du code invité interrompu pourrait tenir.


## Traitée le 2026-10-02 (mainteneur d'intégration)

Correctif présent dans `src/core/usermem.c` (`lkd_fault_try_recover`) et déclarations présentes. Vérifié par `test_mm_watch` et `test_core_usermem`.
