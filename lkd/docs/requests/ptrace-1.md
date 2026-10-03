# Demande ptrace-1 (sig, core, task, moteur)

Demandeur : ptrace. Date : 2026-10-02. Les points 1 a 3 sont des correctifs de comportement ; le module fonctionne sans eux mais avec les ecarts notes dans `docs/status/ptrace.md`.

## sig : `PTRACE_INTERRUPT` et `PTRACE_EVENT_STOP`

`lkd_sig_interrupted` lit deja `LKD_TF_TRACE_STOP`, mais `lkd_signal_next` ne l'honore pas : le drapeau reveille les attentes bloquantes sans rien consommer. En tete de la boucle de `lkd_signal_next` (avant `LKD_TF_GROUP_STOP`), ajouter :

```c
if (atomic_load(&t->flags) & LKD_TF_TRACE_STOP) {
    atomic_fetch_and(&t->flags, ~LKD_TF_TRACE_STOP);
    if (lkd_ptrace_trap_stop(t) < 0) return die(t);   /* declare dans ptrace.h */
    continue;
}
```

et definir `int lkd_sig_trace_stop_supported(void) { return 1; }` (le module ptrace en a une version faible qui rend 0). Tant que ce n'est pas fait, `PTRACE_INTERRUPT` s'arrete a la prochaine frontiere de syscall, de signal ou d'evenement, jamais pour un tracee bloque ou en code traduit.

## core : arret de sortie de syscall pour un contexte remplace

`lkd_syscall` n'appelle pas `lkd_ptrace_syscall_exit` quand `disp == LKD_CONTEXT_REPLACED` (execve, rt_sigreturn). Linux fait l'arret de sortie dans les deux cas (`syscall_exit_work`) : le premier arret apres un `execve` avec PTRACE_SYSCALL est la sortie d'execve (`orig_rax` 59, `rax` 0, RIP = entree de la nouvelle image), attendu par EAC. Contournement actuel : `lkd_ptrace_event(EXEC)` fait cet arret lui-meme. Demande : pour `LKD_CONTEXT_REPLACED`, appeler `lkd_ptrace_syscall_exit(t, &zero)` quand le drapeau etait pose ; ptrace sait alors que le contexte est remplace (rcx et r11 non touches) par `t->cur_nr`. Pour rt_sigreturn, meme appel (RAX reste celui du cadre restaure).

## moteur : `orig_rax`

`LkdHandleSyscall` pose `T->OrigRax = nr` et ne le remet jamais a -1. Linux le met a -1 a l'entree d'une exception ou d'une interruption. Demande : `OrigRax = -1` quand le moteur sort du code traduit pour une faute, un int3 ou un kick ; ptrace n'a pas d'autre moyen de le savoir (il le met a -1 pour un signal de faute d'apres `si_code`, ce qui couvre int3, SEGV, FPE).

## moteur : pas a pas

`set_single_step` est NULL : `PTRACE_SINGLESTEP` rend EIO (journalise). Quand le moteur saura executer une instruction, il pose `set_single_step`, et signale la fin du pas par `lkd_fault(t, SIGTRAP, TRAP_TRACE, rip, 0, 1)`. ptrace appelle `set_single_step(t, 0)` sur la tache du traceur comme sonde de disponibilite (idempotent).

## mm

Rien. `lkd_mm_remote_read/write` (PEEK/POKE) invalident le code traduit de la cible avant de repondre, comme prevu.

## core : declarations a ajouter a `src/include/ptrace.h`

Definies et fortes dans `src/ptrace` mais absentes de l'en-tete du noyau commun (les tests les declarent a la main) :

```c
int lkd_ptrace_trap_stop(lkd_task *t);                                   /* sig, PTRACE_INTERRUPT (voir plus haut) */
int lkd_ptrace_wait_has(lkd_task *waiter, int32_t sel_pid, unsigned options);   /* task, wait4 (ptrace-2) */
int lkd_sig_trace_stop_supported(void);   /* faible dans ptrace (rend 0), forte dans sig (rend 1) */
```

## moteur : variables d'environnement d'une re-execution hote

Si le moteur re-execute le processus hote pour un `execve` invite, il doit garder `LKD_PTRACE_FD` dans l'environnement et le descripteur qu'elle nomme ouvert (sans FD_CLOEXEC, comme `LKD_FAMSVC_FD` et `LKD_FAMILY_FD`) : c'est la table des liens traceur/tracee de toute la famille. Sans elle, un tracee perdrait son traceur a l'exec (le premier arret apres l'exec est justement ce que EAC attend). `lkd_ptrace_init` ratache la table au demarrage de l'image re-executee ; si le descripteur est absent il en cree une nouvelle et journalise (les liens existants sont alors perdus).


## Traitée le 2026-10-02 (mainteneur d'intégration)

sig : `lkd_signal_next` honore `LKD_TF_TRACE_STOP` (appel de `lkd_ptrace_trap_stop`) et définit `lkd_sig_trace_stop_supported() = 1`. Test : `test_ptrace_requests` cas `interrupt_reaches_a_blocked_tracee` (PTRACE_INTERRUPT d'un tracé dans nanosleep : arrêt 0x80057f en moins de 300 ms ; avant : aucun arrêt).
core : `lkd_syscall` appelle `lkd_ptrace_syscall_exit_replaced` (nouvelle, `ptrace.h`) pour `LKD_CONTEXT_REPLACED`, en relisant `LKD_PT_SYSCALL_TRACE` à la sortie (PTRACE_SYSCALL donné à l'arrêt d'exec). Le contournement dans `lkd_ptrace_event(EXEC)` est retiré. rt_sigreturn a maintenant un arrêt de sortie (rax restauré, rcx non modifié). Test : `test_ptrace_requests` cas `sigreturn_has_an_exit_stop`.
Déclarations ajoutées à `src/include/ptrace.h` : `lkd_ptrace_trap_stop`, `lkd_ptrace_wait_has`, `lkd_sig_trace_stop_supported` (redéclarations locales retirées).
Restent au moteur : `orig_rax = -1` à l'entrée d'une faute ou d'un kick (le chef l'a fait dans `fexdarwin_kick_safepoint`), le pas à pas, `LKD_PTRACE_FD` à la ré-exécution hôte.
