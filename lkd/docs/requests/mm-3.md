# Demande mm-3 (task) : région partagée de la famille pour la table System V

La table des segments SysV (`src/mm/shm.c`) vit dans une région `MAP_SHARED|MAP_ANON` créée par `lkd_mm_init`, héritée par fork. Elle ne survit pas à une réexécution hôte du runtime (execve invité) : un processus exécuté de nouveau ne voit plus les segments de la famille.

Demande : une fonction de task qui alloue un morceau nommé de la mémoire partagée de la famille (celle que `lkd_family_export`/`lkd_family_attach` transmettent), par exemple `void *lkd_family_region(const char *name, size_t size)`, qui rend la même région dans tout processus de la famille. mm y placera sa table (verrou : mot atomique avec pid du détenteur, repris si le détenteur est mort).


## Traitée le 2026-10-02 (mainteneur d'intégration)

Non fait : l'exec invité est fait dans le même processus hôte (`ExecImage`), la table SysV héritée par fork suffit. À reprendre si une ré-exécution hôte revient.
