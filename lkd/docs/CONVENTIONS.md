# Conventions LKD

Document de travail du noyau commun. Les modules (mm, fs, procfs, task, sig, net, ptrace) suivent ces règles. Ce qui n'est pas dans ce fichier est dans les commentaires des en-têtes (`include/lkd/lkd.h`, `src/include/*.h`), qui font foi. Une règle qui gêne se discute par une demande dans `docs/requests/<module>-<n>.md`, pas par un contournement.

## 1. Retours d'erreur

1. Un gestionnaire de syscall renvoie le contenu de RAX : une valeur, ou `-LX_E*` (erreur Linux négative). Jamais le `errno` de Darwin. Les constantes `LX_E*` ont les valeurs Linux (`EAGAIN` vaut 11 chez Linux et 35 chez Darwin).
2. Après un appel hôte qui échoue, écrire `return LKD_HOST_ERR();` (traduit `errno` par `lkd_errno_from_darwin`) ou `LKD_HOST_RET(r)` pour un `r < 0`. Un code Darwin sans équivalent Linux devient `LX_EIO` ou le plus proche honnête (table dans `src/core/xlate.c`).
3. Aucune réponse fabriquée. Une fonction non implémentée renvoie `-LX_ENOSYS` (ou l'erreur exacte de Linux pour un cas précis), jamais un succès. Un syscall que le module ne fournit pas reste non enregistré : le dispatcher répond `-ENOSYS`, compte l'appel (`lkd_enosys_count`) et le nomme une fois avec `LKD_LOG_ENOSYS=1`.
4. Les codes `-LX_ERESTARTSYS`, `-LX_ERESTARTNOINTR`, `-LX_ERESTARTNOHAND`, `-LX_ERESTART_RESTARTBLOCK` sont internes : ils ne sortent jamais vers l'invité (le dispatcher les résout en `-EINTR` ou en redémarrage). `-LX_ENOIOCTLCMD` devient `-LX_ENOTTY`.
5. Résultats spéciaux d'un gestionnaire : `LKD_RET_CONTEXT_REPLACED` (execve, rt_sigreturn : le contexte a été posé par `ops->set_regs`), `LKD_RET_EXIT_THREAD`, `LKD_RET_EXIT_GROUP`.

## 2. Pointeurs invités

1. Ne jamais déréférencer un pointeur invité autrement que par `lkd_user_read`, `lkd_user_write`, `lkd_user_strn`, `lkd_user_zero`. Elles valident par `lkd_mm_check_range` puis copient avec récupération de faute (`-LX_EFAULT`, jamais un plantage).
2. Chemin rapide autorisé : après `lkd_mm_check_range(gva, n, need_write) == 0`, donner `lkd_g2h(gva)` à un appel hôte (`read`, `write`, `recv`). Le noyau hôte renvoie `EFAULT` pour une page non mappée. Ne pas le faire pour un tampon qui peut recouvrir le fichier lu lui-même ou une page protégée par sous-pages de 4 Kio sans passer par la validation.
3. Ne jamais garder un pointeur hôte issu de `lkd_g2h` au-delà d'un appel bloquant, d'un `munmap` possible ou d'une mutation de la table de VMA. Recopier d'abord dans un tampon du noyau.
4. Structures contenant des pointeurs (`iovec`, `msghdr`, `sigaction`, `epoll_event`) : lire la structure par `lkd_user_read`, puis traiter chaque pointeur interne de la même façon. Les tailles et décalages sont dans `abi_x64.h`.
5. `lkd_guest_base` vaut 0 dans les tests : un pointeur invité est alors un pointeur hôte. Dans le moteur, l'adresse invitée `g` est à l'adresse hôte `lkd_guest_base + g`, la fenêtre fait au plus 2^46 octets, la page invitée est de 4 Kio et la page hôte de 16 Kio.
6. `lkd_mm_check_range` est consultatif : un `munmap` concurrent peut passer entre le contrôle et la copie. La récupération de faute est le filet de sécurité réel.

## 3. Ordre des verrous

Prendre dans l'ordre croissant, ne jamais prendre un rang plus bas en tenant un rang plus haut. Les constantes sont dans `lkd_internal.h` (`enum lkd_lock_rank`). La liste reprend celle de HyperLinux (`src/syscall_internal.h:10-38`) et l'étend.

| Rang | Verrou | Module |
| --- | --- | --- |
| 10 | `mmap_lock` (carte des VMA, brk, mmap) | mm |
| 11 | table SysV shm, prise sous `mmap_lock` | mm |
| 20 | table de montages vfs, `fsctx` (cwd, root, umask) | fs |
| 21 | registre des périphériques `/dev` | procfs |
| 30 | table de descripteurs | fs |
| 31 | verrou d'objet fd (`lkd_fdobj.lock`) | fs et tout propriétaire d'objet |
| 40 | état de signaux par processus (actions, pending de processus) | sig |
| 41 | file de siginfo par tâche | sig |
| 50 | registre des threads et liste des threads d'un processus | task |
| 51 | registre partagé des processus (verrou d'octet `fcntl`) | task |
| 60 | états d'attente des enfants (wait4) | task |
| 61 | état traceur/tracee | ptrace |
| 70 | seau de futex (par indice croissant si plusieurs) | sig |
| 71 | tables epoll, timerfd, inotify | net |
| 80 | feuille : journal, trace, compteurs | tous |
| 90 | verrou de lecture des VMA, pris par `lkd_mm_check_range` | mm |

Règles associées.

1. Le rang 90 est une feuille que `lkd_mm_check_range` prend en lecture, très brièvement : `lkd_user_*` est donc appelable sous n'importe quel autre verrou. Un module ne doit jamais appeler `lkd_user_*` en tenant ce rang 90 lui-même (c'est le seul interdit).
2. Aucun verrou n'est tenu pendant : un syscall invité, une attente bloquante, un callback du moteur marqué « sans verrou LKD », `lkd_signal_next`, `lkd_ptrace_stop`, ni pendant l'exécution de code invité.
3. Un module qui publie vers un autre module relâche son verrou avant l'appel. Le ptrace publie un arrêt (rang 61) puis relâche avant de réveiller wait4 (rang 60) : `lkd_ptrace_wait_event` est appelé avec le rang 60 tenu, le code de ptrace ne doit donc jamais prendre un rang inférieur à 61 en tenant le 61.
4. Verrous du moteur (2026-10-02, intégration). Les verrous du moteur passent AVANT tous ceux de LKD : `lkd_fork_prepare` appelle `ops->fork_prepare` (ThreadsLock, verrous de code de FEX) puis les crochets des modules. En contrepartie, LKD n'appelle jamais `ops->invalidate_code` (ni aucun rappel du moteur) en tenant un verrou LKD : mm collecte les plages dans un `struct mm_inval` et les invalide après avoir relâché le verrou mmap (`mm_inval_flush`), et le retrait d'une surveillance (`mm_unwatch_for_write`) invalide hors des verrous de surveillance et des VMA. Le gestionnaire SMC du moteur peut donc prendre ThreadsLock dans `invalidate_code` sans croiser un fork en cours.
5. Une primitive de synchronisation partagée entre processus (verrou de registre, compteur) doit survivre à la mort de son détenteur : verrou d'octet `fcntl` (relâché par le noyau à la mort), ou algorithme sans verrou. Un mutex `pthread` en mémoire partagée reste verrouillé si le processus meurt tenant : à éviter. `os_unfair_lock` ne se partage pas entre processus.

## 4. Appels bloquants et signaux

1. Tout syscall bloquant attend par `lkd_block_wait_fd`, `lkd_block_poll` ou `lkd_block_sleep` (ou une primitive qui suit le même protocole). Le test d'interruption `lkd_sig_interrupted(t)` a lieu avant de dormir, un `lkd_task_wake(t)` écrit dans le tube de réveil de la tâche : un signal en file avant l'attente n'est jamais perdu. Ne pas faire de `read` hôte bloquant sur un descripteur de pipe ou de socket : attendre la disponibilité (`lkd_block_wait_fd`), puis lire sans bloquer.
2. À l'interruption, l'attente rend `-LX_ERESTARTSYS`. Le syscall la rend telle quelle s'il est redémarrable, sinon la remplace par `-LX_EINTR` :
   - redémarrable (renvoyer `-LX_ERESTARTSYS`) : `read`, `readv`, `write`, `writev`, `ioctl`, `accept`, `accept4`, `recvfrom`, `recvmsg`, `sendto`, `sendmsg`, `connect` (bloquant), `flock`, `fcntl(F_SETLKW)`, `wait4`, `waitid`, `futex(FUTEX_WAIT)` sans délai, `open` sur FIFO, `msgrcv`. Linux ne redémarre pas si une temporisation `SO_RCVTIMEO` ou `SO_SNDTIMEO` est posée : renvoyer `-LX_EINTR`.
   - jamais redémarré (renvoyer `-LX_EINTR`) : `poll`, `ppoll`, `select`, `pselect6`, `epoll_wait`, `epoll_pwait`, `rt_sigtimedwait`, `rt_sigsuspend`, `sigsuspend`, `io_getevents`, `mq_timedreceive`.
   - `-LX_ERESTARTNOHAND` : `pause`, `rt_sigsuspend`, `epoll_pwait` avec masque (redémarre seulement si aucun gestionnaire ne s'exécute).
   - `-LX_ERESTART_RESTARTBLOCK` avec une suite stockée dans `t->restart` : `nanosleep`, `clock_nanosleep`, `futex(FUTEX_WAIT)` avec délai, `poll`/`ppoll` avec délai. Le syscall `restart_syscall` (core) exécute la suite ; le module `sig` met `t->restart.fn` à NULL quand un gestionnaire de signal s'exécute.
3. Le dispatcher décide du redémarrage par `lkd_sig_restart_decision` (règles de `arch/x86/kernel/signal.c`). Sur `LKD_RESTART`, le moteur recule RIP de 2, met RAX au numéro rendu dans `*rax`, garde les six registres d'argument, puis appelle `lkd_signal_next` : le cadre de signal sauvegarde le contexte déjà redémarré.
4. Un thread qui exécute du code JIT est interrompu par `ops->kick(t)` (un signal hôte réservé, choix du moteur). Le gestionnaire du moteur ne fait que lever un drapeau lu aux points sûrs, puis le thread appelle `lkd_signal_pending`, `lkd_signal_next`. Un thread bloqué dans LKD est réveillé par `lkd_task_wake` (tube), jamais par le signal hôte seul.
5. Les attentes qui ne passent pas par le tube (futex sur `os_sync_wait_on_address`) suivent le contrat de `futex.h` : s'enregistrer dans le seau, tester l'interruption, dormir, se retirer du seau, renvoyer `-LX_ERESTARTSYS`. Le module `sig` choisit le mécanisme de réveil.

## 5. Contrat avec le moteur (résumé)

1. `lkd_syscall` à chaque SYSCALL invité ; écrire RAX avec `*rax` pour `LKD_RETURN`. RCX et R11 sont posés par le moteur, jamais par LKD.
2. Après `lkd_syscall`, après un kick, après un rapport de faute : `while (lkd_signal_pending(t) && lkd_signal_next(t) > 0) {}` ; un retour `-1` veut dire que la tâche doit mourir (dérouler le thread hôte puis `lkd_task_release`).
3. `ops->get_regs` et `set_regs` sont valides pour la tâche appelante à un point sûr, et pour une tâche arrêtée (arrêt ptrace, sortie). Pour une tâche en cours d'exécution, elles renvoient `-LX_ESRCH`.
4. Le moteur appelle `lkd_set_current(t)` au début de chaque thread hôte invité.
5. Le moteur qui installe ses propres gestionnaires SIGSEGV/SIGBUS après `lkd_init` appelle `lkd_fault_try_recover(ucontext)` en premier.
6. Le moteur lie `liblkd.a` avec `-Wl,-force_load,liblkd.a`.

## 6. fork

macOS conserve seulement le thread appelant et laisse verrouillés les verrous tenus par les autres threads (mutex, `os_unfair_lock`, stdio). Le schéma est celui de FEX (`LockBeforeFork`/`UnlockAfterFork`).

1. `lkd_fork_host(t, flags)` fait : `ops->fork_prepare`, `lkd_<m>_fork_prepare` dans l'ordre mm, fs, procfs, sig, task, ptrace, net (rangs croissants), `fork()`, puis dans le parent `lkd_<m>_fork_parent` et `ops->fork_parent`, dans l'enfant `lkd_<m>_fork_child` et `ops->fork_child`, dans l'ordre inverse.
2. `fork_prepare` prend tous les verrous du module (dans l'ordre de ses propres rangs) et rend la main vite ; elle ne bloque pas longtemps (un détenteur absent est un défaut à signaler). `fork_parent` les relâche. `fork_child` les reconstruit par `lkd_fork_reinit_mutex` ou `lkd_fork_reinit_rwlock` (mémoire remise à zéro puis init), jamais par un `unlock` : si un autre thread attendait le verrou au moment du fork, le mot du verrou porte un état d'attente du noyau (psynch) que l'enfant n'a pas, et l'enfant reste bloqué au déverrouillage ou à la prise suivante (MESURE : témoin `w18_forkjit`, enfant bloqué dans `mm_wlock` ; `test_mm_fork` cas `fork_while_a_thread_churns_the_mm_locks`). Même règle pour les verrous que `fork_prepare` ne prend pas mais qu'un autre thread du parent peut tenir (verrous feuilles) : les reconstruire dans `fork_child`. Les `os_unfair_lock` sont remis à `OS_UNFAIR_LOCK_INIT`. `fork_child` oublie aussi les threads absents, relance les threads auxiliaires du module (thread de service de signaux, de transport ptrace) et reconstruit les objets que `fork` n'hérite pas : les descripteurs kqueue (MESURE : `EBADF` dans l'enfant) donc epoll, timerfd et inotify.
3. Hérité par `fork` : la mémoire privée (copie sur écriture), les descripteurs hôtes (avec leur offset et leurs drapeaux partagés), les mappings partagés (`MAP_SHARED`). Non hérité : kqueue, les autres threads, les verrous de threads absents.
4. mm : la mémoire anonyme partagée doit être un objet mémoire partagé pour rester partagée avec l'enfant. task : `lkd_fork_child` bascule `t` sur l'identité de l'enfant (nouveau tid et pid pris dans le compteur de la famille avant `fork()`, ppid, thread unique) ; l'état d'un thread absent est supprimé du registre local.
5. `CLONE_VM|CLONE_VFORK` n'est pas reproduit par un fork (limite documentée de FEX) : task le traite comme un fork suivi d'une attente de l'exec ou de la sortie de l'enfant, et le dit dans son statut.

## 7. exec

L'exec invité est une ré-exécution du runtime hôte. Ordre : `lkd_exec_prepare` (résolution par le vfs, droits, `#!`, copie de argv et envp ; aucune modification d'état), puis `lkd_exec_commit` (point de non-retour : hooks `lkd_<m>_exec_commit` dans l'ordre task, mm, fs, sig, ptrace, net, procfs), puis `ops->exec_image`. Si l'exec hôte perd la mémoire, l'état à conserver (registre de la famille, tables de descripteurs non CLOEXEC) passe par la mémoire partagée de la famille (`lkd_family_export`) et les descripteurs hôtes hérités. Le drapeau `FD_CLOEXEC` d'un descripteur invité est aussi posé sur le descripteur hôte. `lkd_exec_finish` rapporte l'arrêt `PTRACE_EVENT_EXEC`.

## 8. Ordre d'initialisation

`lkd_init(ops)` : copie des opérations, gestionnaires SIGSEGV/SIGBUS, variables d'environnement (`LKD_TRACE`, `LKD_LOG_ENOSYS`), syscalls du noyau commun (`restart_syscall`), puis `lkd_mm_init`, `lkd_fs_init`, `lkd_procfs_init`, `lkd_task_init`, `lkd_sig_init`, `lkd_net_init`, `lkd_ptrace_init`. `lkd_init` échoue avec `-LX_EEXIST` si deux modules enregistrent le même syscall. L'initialisation d'un module ne dépend que des modules qui le précèdent. Le premier thread invité est créé ensuite par `lkd_task_create_main`.

## 9. Ajouter un syscall en dix lignes

```c
/* src/task/ids.c */
#include "lkd_internal.h"
#include "task.h"

LKD_SYSCALL(gettid)                       /* static int64_t lkd_sys_gettid(lkd_task *t, const lkd_sysreq *rq) */
{
    return t->tid;
}

void lkd_task_init(void)                  /* strong definition; core has a weak no-op */
{
    LKD_REGISTER(gettid);                 /* nr taken from LKD_NR_gettid, name "gettid" */
}
```

Tout le reste (`tests/test_task_ids.c`) est détecté par `file(GLOB)` : relancer `./check.sh`. Les arguments sont `rq->a[0..5]` (macros `LKD_ARGI`, `LKD_ARGU`, `LKD_ARGP`). Le test appelle `lkd_syscall` avec `fake_make_task()` et `fake_engine_init()` (voir §10).

## 10. Tests

1. Un fichier `tests/test_<module>_<sujet>.c` est un exécutable ; `tests/support/*.c` (moteur factice) est lié à chacun. Cadre : `tests/ltest.h` (`LT_TEST`, `LT_RUN`, `LT_CHECK`, `LT_EQ`, `LT_ERRNO`, `LT_STREQ`, `LT_MEMEQ`, `lt_finish`). Un programme sans assertion échoue.
2. Chaque comportement a un test qui échoue avant et passe après, avec en commentaire le comportement Linux modélisé (page de manuel ou fichier source du noyau avec ligne). Les valeurs attendues sont écrites en littéraux, pas recalculées par le code testé.
3. Moteur factice : `fake_engine_init()` appelle `lkd_init` avec des opérations qui enregistrent les kicks, invalidations, constructions de cadre, spawns et événements de fork ; `fake_make_task()` crée une tâche publiée avec un CPU factice (`fake_cpu_of(t)->regs`) ; `fake_make_thread(t)` ajoute un thread du même processus. Les réglages `fake_spawn_result`, `fake_frame_build_result`, etc. simulent des échecs. Pour une extension du moteur factice, écrire une demande.
4. Les tests de `fork` forkent vraiment le processus de test et vérifient par tube ou mémoire partagée (`lt_run_child`). Les tests de threads créent de vrais threads, jamais plus de 300 par processus.
5. Sécurité de la machine : jamais plus de 300 threads dans un processus, 64 processus enfants simultanés, 8 Gio de mémoire touchée, aucune boucle de fork ou de thread sans borne. `check.sh` lance chaque programme sous une limite de 120 s (`LKD_TEST_TIMEOUT`). Un test laisse derrière lui ni processus ni fichier hors de son répertoire temporaire.
6. Les tests peuvent définir un symbole fort uniquement si aucun module ne le définit (pas de redéfinition d'un hook de module dans un test).
7. Ne jamais lancer le jeu, EAC, UTM, QEMU ou quoi que ce soit avec HVF.

## 11. Liaison et symboles faibles

`liblkd.a` contient des symboles faibles (hooks de `weak_modules.c`, remplaçants de `stubs_*.c`) et leurs définitions fortes dans les modules. Un lien d'archive ordinaire ne tire pas forcément le membre fort quand le faible a déjà résolu le symbole. Les tests et le moteur lient donc avec `-Wl,-force_load,<chemin>/liblkd.a` (déjà fait par `CMakeLists.txt` ; `LINK_DEPENDS` relie les tests quand la bibliothèque change). Un module qui ajoute un fichier source le dépose simplement dans `src/<module>/` : `file(GLOB_RECURSE)` le prend à la prochaine configuration (`./build.sh` reconfigure toujours).

## 12. Politique d'import de HyperLinux

HyperLinux (Apache-2.0, `rocket-hyperlinux-20260928/hyper-linux/src`) est en lecture seule. On copie un fichier ou une fonction dans `src/<module>/`, en l'adaptant (numéros de syscall x86-64, `lkd_user_*` à la place de `guest_read`, valeurs `LX_*`, pas de type HVF). Chaque bloc importé porte le commentaire `/* imported from HL <fichier> */`, l'en-tête SPDX et le copyright d'origine restent, et le module liste ses fichiers importés dans `src/<module>/NOTICE`. Le `NOTICE` racine contient le texte d'attribution. Jamais d'édition de l'original. Le CLAUDE.md de FEX interdit de soumettre du code généré par IA : cet arbre est une copie de recherche privée, rien n'est envoyé en amont.

## 13. Style

Commentaires de code en anglais, courts, sans décoration. Rapports et fichiers de statut en français, phrases simples, sans tiret cadratin, avec `fichier:ligne`, MESURE séparé de LU et INFERE. Noms : `lkd_<module>_*` pour les fonctions publiques d'un module, `lx_` et `LX_` pour l'ABI Linux, `LKD_` pour les macros du noyau.
