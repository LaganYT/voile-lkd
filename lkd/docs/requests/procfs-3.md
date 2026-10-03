# Demande procfs-3 au chef de projet (moteur fexdarwin)

Date : 2026-10-02. Auteur : module procfs.

1. Fournisseur CPUID. `/proc/cpuinfo` et `/sys/devices/system/cpu/cpuN/cache/` sont construits à partir du CPUID que l'invité exécute, par une fonction que le moteur enregistre une fois avant le premier code invité :
   `lkd_procfs_set_cpuid(fn)` avec `int fn(uint32_t leaf, uint32_t subleaf, int cpu, struct lkd_cpuid_regs *out)`. Dans `LkdGlue.cpp` : `CTX->RunCPUIDFunction(leaf, subleaf)` pour tous les niveaux, et `CTX->RunCPUIDFunctionName(leaf, subleaf, cpu)` pour 0x80000002 à 0x80000004 (chaîne de marque par CPU). Les niveaux lus : 0, 1, 4 (sous-niveaux jusqu'à type 0), 6, 7 (0 et 1), 0xD (1), 0x10, 0x15, 0x80000001, 0x80000007, 0x80000008, 0x8000000A, 0x8000001F. Tant que le fournisseur manque, `open("/proc/cpuinfo")` répond `-ENOSYS` (journalisé une fois) et `/sys/.../cache` et `clocksource0` n'existent pas : aucun contenu inventé.
   FEX n'est pas caché : le bit `hypervisor` du CPUID s'affiche dans `flags` (FEX l'efface avec `HideHypervisorBit`), la marque vient du MIDR.
2. Disposition après exec. `ops->exec_image` doit appeler `lkd_proc_set_layout` (demande `task-3.md` point 1) : sans elle `/proc/<pid>/auxv` est vide et `start_code`, `start_stack`, `arg_start`... de `stat` valent 0.
3. `/proc/self/exe` rend `proc->exe_path` (chemin invité posé par task à l'exec). `execve("/proc/self/exe")` direct passe par `lkd_vfs_to_host`, qui répond `-ENOENT` ou `-EXDEV` pour un lien magique : non vérifié avec le moteur (task).
4. Service famille. procfs enregistre `LKD_FAMSVC_OP_TASK_STATUS` (49). Le moteur doit garder `LKD_FAMSVC_DIR` et `LKD_FAMSVC_FD` à l'exec hôte (déjà demandé par sig).
