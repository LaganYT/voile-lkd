# Demande mm-1 (core) : lire la configuration depuis un module

Le module mm doit lire `guest_window_bytes` et `guest_window_base` de `lkd_config`. La seule fonction qui les donne est `lkd_config_get()` déclarée dans `src/core/core_priv.h`, réservée au noyau commun. En attendant, `src/mm/mm_priv.h` redéclare le prototype `const lkd_config *lkd_config_get(void);`.

Demande : déclarer `lkd_config_get()` dans `src/include/lkd_internal.h` (lecture seule pour les modules).


## Traitée le 2026-10-02 (mainteneur d'intégration)

Fait (lead, `lkd_config_get` dans `lkd_internal.h`).
