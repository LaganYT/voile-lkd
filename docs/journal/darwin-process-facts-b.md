# Faits Darwin sur l'inspection de processus, les exceptions Mach, le temps et l'identité (rapport B)

Rapport vivant, mis à jour à chaque jalon. Dernière mise à jour : 2026-10-02 10:20 (brouillon 1, reprise après interruption de la session).

Convention : MESURÉ = sortie d'un programme exécuté sur cette machine, journal cité. LU = lecture d'une source ou d'un en-tête. DÉDUIT = inférence, à qualifier. Une section marquée « à compléter » n'a pas encore de mesure.

Racine de travail : `build/experiments/rocket-wine-fex-darwin-20261002` (notée `$EXP`). Sources des témoins : `$EXP/witnesses/darwin-process/`. Journaux : `$EXP/evidence/darwin-process/`.

## État d'avancement

| Témoin | Sujet | État |
| --- | --- | --- |
| `h0_sysinfo.sh` | faits d'environnement (SIP, boot-args, version, Rosetta) | exécuté, `evidence/darwin-process/h0_sysinfo.log` |
| `j1_time_ident.c` | horloges, minuteries, identifiants, limites | source laissée par la tentative interrompue, journal partiel (arrêt dans J2), à relancer |
| `h1_*` task port, mémoire, états de thread | H | à écrire |
| `h2_*` ptrace | H | à écrire |
| `h3_*` proc_pidinfo, sysctl | H | à écrire |
| `h4_*` canal coopératif | H | à écrire |
| `i1_*` exceptions Mach contre signaux | I | à écrire |
| `i2_*` sonde `svc` | I | à écrire |
| `h5_*` transport de ptrace invité | K | à écrire |

## Faits d'environnement (H0, MESURÉ)

Source : `evidence/darwin-process/h0_sysinfo.log`, exécuté le 2026-10-02 10:11.

- `sw_vers` : macOS 27.0.1, build 26A434. `sysctl kern.osversion` : 26A434.
- `uname -a` : `Darwin 27.0.0 ... root:xnu-13432.1.9~1/RELEASE_ARM64_T6050 arm64`.
- Machine : `Mac17,7`, Apple M5 Max, 18 coeurs (2 niveaux de performance, 6 coeurs « Super »), 36 Gio, pages de 16384 octets, `hw.tbfrequency` 24000000.
- `kern.hv_support` : 1 (HVF disponible ; hors périmètre, noté pour mémoire).
- SIP : `System Integrity Protection status: enabled.`
- `boot-args` : `sysctl kern.bootargs` vide ; `nvram boot-args` et `csr-active-config` absents (aucun réglage personnalisé).
- Mode développeur : `DevToolsSecurity -status` répond « Developer mode is currently disabled ».
- Droits d'autorisation `system.privilege.taskport` et `system.privilege.taskport.debug` : classe `user`, groupe `_developer`, `authenticate-user` vrai, `allow-root` faux (donc un `task_for_pid` hors du chemin `get-task-allow` demanderait une authentification).
- Rosetta : présent (`/Library/Apple/usr/libexec/oah/libRosettaRuntime`, `RosettaLinux`, `translate_tool`, paquet `com.apple.pkg.RosettaUpdateAuto`). `sysctl.proc_translated` vaut 0 pour notre shell (processus natif arm64). Aucun `arch -x86_64` n'a été lancé.
- Utilisateur : uid 501, gid 20, membre de `_developer` et `admin`. Pas de root.
- Limites système : `kern.maxproc` 9000, `kern.maxprocperuid` 6000, `kern.num_taskthreads` 9216, `kern.num_threads` 46080, `kern.maxfilesperproc` 138240, `kern.argmax` 1048576. `ulimit -n` 1048576 dans ce shell, `ulimit -u` 6000.
- Outils : Apple clang 21.0.0, Xcode 27.0, SDK 27.0.

## H : inspecter un autre processus (résultats provisoires, h1 et h2 exécutés)

Témoins : `witnesses/darwin-process/h1_task_inspect.c`, `h2_ptrace.c`, journaux `evidence/darwin-process/h1_task_inspect.log` et `h2_ptrace.log`. Le débogué est le même binaire relancé avec l'argument `debuggee` (signé `com.apple.security.get-task-allow`, thread principal en boucle assembleur à registres connus, un thread bloqué dans `read(2)`, un thread serveur).

### H1, task port d'un enfant direct signé get-task-allow (MESURÉ, h1)

- `task_for_pid(mach_task_self(), pid_enfant)` réussit sans invite : premier appel 1116 us, appels suivants p50 188 us (min 175 us, max 772 us). Le coût de 190 us par appel est celui d'un aller-retour avec taskgated : un traceur doit garder le port, pas le redemander. `pid_for_task` rend le bon pid.
- `task_name_for_pid`, `task_inspect_for_pid` et `task_read_for_pid` réussissent aussi sur cet enfant, et `mach_vm_read` marche à travers le port `task_read`.
- `mach_vm_read_overwrite` : 8 octets p50 0,62 us, 64 octets 0,62 us, 4 Kio 0,71 us, 64 Kio 2,33 us (28 Go/s), 1 Mio 28,9 us (36 Go/s). `mach_vm_read` plus `mach_vm_deallocate` de 4 Kio : 1,96 us. `mach_vm_write` de 8 octets : 0,62 us. Contenu relu égal au motif écrit par l'enfant sur 1 Mio.
- `mach_vm_remap(copy=FALSE)` d'un tampon anonyme de l'enfant dans le traceur réussit (prot courante 3 = rw, max 7) : le mapping est partagé (une écriture de l'enfant est vue par le traceur, une écriture du traceur est vue par l'enfant), et une lecture de 8 octets à travers ce mapping coûte moins que la résolution de l'horloge (41 ns). C'est le chemin le plus rapide pour un futur `/proc/<pid>/mem` ou PEEK en rafale.
- `mach_vm_write` directement sur une page en lecture seule échoue avec `KERN_INVALID_ADDRESS` (kr=1, pas `KERN_PROTECTION_FAILURE`). `mach_vm_protect(RW|VM_PROT_COPY)`, écriture, puis retour en lecture seule fonctionne (sémantique de POKE sur page non inscriptible).
- Écriture dans une page de code `__TEXT` du débogué : `mach_vm_protect(RW|COPY)`, `mach_vm_write` et le retour en R-X réussissent, mais l'enfant meurt de `SIGBUS` (Bus error: 10) dès qu'il exécute la page modifiée, avec ou sans `ptrace(PT_ATTACHEXC)` préalable. Un POKETEXT sur du code Mach-O signé n'est donc pas possible. Le code de l'exception reste à capturer (h6).
- `mach_vm_region` : 66 régions parcourues en 0,054 ms (0,8 us par appel). `mach_vm_region_recurse` : 192 régions feuilles en 18,4 ms (le sous-mapping du cache partagé coûte cher). `task_info(TASK_DYLD_INFO)` et `TASK_VM_INFO` fonctionnent (taille virtuelle du débogué : environ 466 Gio, taille de page 16384).
- `task_threads` rend 3 threads ; `THREAD_IDENTIFIER_INFO.thread_id` côté traceur est égal à `pthread_threadid_np` côté enfant ; `THREAD_EXTENDED_INFO.pth_name` rend le nom du thread (équivalent de `/proc/pid/task/tid/comm`) ; `THREAD_BASIC_INFO.run_state` donne l'état (1 running, 3 waiting).
- `thread_get_state(ARM_THREAD_STATE64)` sur le thread principal : les 26 registres constants x1..x17, x19..x26, x28 sont relus exactement, pc dans la boucle, x27 cohérent avec le compteur en mémoire (écart 1), `ARM_NEON_STATE64` v0..v7 exact, `ARM_EXCEPTION_STATE64` (esr 0x56000080 = dernier `svc #0x80`), `ARM_DEBUG_STATE64` (130 mots, mdscr_el1 = 0). `thread_get_state` marche aussi sur un thread qui tourne, sans `thread_suspend` (p50 0,67 us, p90 7,8 us).
- Latences : `thread_suspend` p50 0,38 us (p90 4,5), `thread_get_state` sur thread suspendu p50 0,62 us, `thread_resume` p50 0,38 us, cycle complet p50 1,46 us, p90 7,96 us, p99 11,7 us. `task_suspend` plus `task_resume` p50 0,79 us. Pendant `task_suspend` l'enfant ne répond plus ; après `task_resume` il répond.
- Un thread bloqué dans `read(2)` montre pc = `read`+8 (juste après le `svc #0x80`), x16 = 3 (numéro de syscall BSD) et x0 = le descripteur : l'état d'un thread dans un appel système est lisible sans coopération.
- `thread_set_state(pc = fonction, x0..x2)` sur un thread qui tourne le redirige : la fonction cible reçoit bien les trois arguments (équivalent de SETREGS plus CONT).

### H2, ptrace entre un processus et ses enfants (MESURÉ, h2)

- `ptrace(PT_ATTACHEXC)` et `ptrace(10 = PT_ATTACH)` d'un parent sur son enfant signé get-task-allow réussissent sans invite (21 us) ; `waitpid(WUNTRACED)` rapporte un arrêt `SIGSTOP`.
- Requêtes inexistantes sur arm64 : `PT_READ_I/D/U` et `PT_WRITE_I/D/U` rendent `EINVAL`, de même `PT_THUPDATE` avec mes arguments et toutes les requêtes 15 à 29 et 32 à 40 ; `PT_FORCEQUOTA` rend `EPERM`. Il n'existe ni GETREGS, ni SETREGS, ni PEEK, ni POKE, ni SYSCALL, ni SEIZE, ni options d'événements : tout cela doit passer par Mach ou par un canal coopératif.
- Mode classique (`PT_TRACE_ME` dans l'enfant, puis un signal) : arrêt de signal rapporté par `waitpid` avec le numéro du signal, tous les threads de l'enfant sont arrêtés, `PT_CONTINUE` avec `data=0` supprime le signal, avec `data=signal` il est délivré (l'enfant meurt de SIGUSR1). L'`exec` produit un arrêt `SIGTRAP`, sans code d'événement.
- `PT_STEP` en mode classique fonctionne : le pc avance de 4 octets par pas ; 5000 pas consécutifs, aller-retour `PT_STEP` plus `waitpid` p50 10,75 us, p90 14,1 us, p99 20,8 us. Le pas porte sur le premier thread (thread principal).
- Mode exception (`PT_ATTACHEXC`) : l'arrêt `SIGSTOP` du début n'arrête que le thread qui a pris le signal (le thread serveur continue de répondre) ; un signal ultérieur est rapporté à `waitpid` mais `PT_CONTINUE` et `PT_DETACH` rendent ensuite `ESRCH`, et l'enfant reste bloqué. Ce mode exige un gestionnaire d'exceptions Mach côté traceur (h6).
- `PT_DENY_ATTACH` dans l'enfant : un `PT_ATTACHEXC` ultérieur rend `EBUSY` et le noyau envoie `SIGSEGV` au traceur, qui meurt. `task_for_pid` continue à réussir sur ce débogué.
- Un frère (pas le parent) peut attacher : `ppid` du traceur devient le pid du frère pendant la trace (style BSD, le noyau reparente le tracé), le parent d'origine reçoit `ECHILD` sur `waitpid`, le `ppid` revient après `PT_DETACH`.

### H3, proc_pidinfo et sysctl sans task port (MESURÉ, h3)

Témoin `h3_pidinfo.c`, journal `h3_pidinfo.log`. Cible : l'enfant débogué (8 threads), le processus parent (même uid, non débogable) et le pid 1 (root).

- Sur l'enfant et sur le parent, tous les flavors essayés réussissent sans task port : `PROC_PIDLISTFDS`, `TASKALLINFO`, `TBSDINFO`, `TASKINFO`, `LISTTHREADS`, `REGIONINFO`, `REGIONPATHINFO`, `VNODEPATHINFO`, `T_SHORTBSDINFO`, `UNIQIDENTIFIERINFO`, `ARCHINFO`, `LISTTHREADIDS` (28), `PLATFORMINFO` (30), plus `proc_pidpath`. Sur le pid 1 (autre uid) seuls `T_SHORTBSDINFO`, `UNIQIDENTIFIERINFO`, `PLATFORMINFO` et `proc_pidpath` répondent, le reste rend `EPERM`.
- `PROC_PIDLISTTHREADIDS` rend les identifiants 64 bits des threads, égaux aux `pthread_threadid_np` de l'enfant (3 correspondances sur 3 connues) ; `PROC_PIDLISTTHREADS` rend des poignées de threads (pointeurs), pas des identifiants. `PROC_PIDTHREADID64INFO(tid)` rend nom, état (`pth_run_state` 1 running, 3 waiting), priorité, temps utilisateur et système : base de `/proc/pid/task/tid/{comm,stat}`.
- `PROC_PIDREGIONPATHINFO` parcourt les 76 régions de l'enfant en 1,2 ms (16 us par région, chemins de fichier inclus pour 4 régions) sans task port, et `PROC_PIDREGIONINFO` parcourt les 61 régions du processus parent (non débogable, même uid) : un `/proc/<pid>/maps` pour tout processus de l'utilisateur est possible sans droits, la fin du parcours rend `EINVAL`.
- `PROC_PIDVNODEPATHINFO` donne le répertoire courant, `proc_pidpath` le chemin de l'exécutable, `PROC_PIDLISTFDS` plus `proc_pidfdinfo(PROC_PIDFDVNODEPATHINFO)` les descripteurs et leurs chemins, `KERN_PROCARGS2` argc, chemin et arguments (aussi sur le parent ; sur le pid 1 `EINVAL`). `kinfo_proc` donne comm, état, ppid, pgid, uid, nombre de groupes.
- Latences (p50) : `PROC_PIDTBSDINFO` 0,17 us, `PIDTHREADID64INFO` 0,17 us, liste des identifiants de threads 0,21 us, `PIDLISTFDS` 0,25 us, `PIDREGIONINFO` 0,42 us, `PIDTASKINFO` 0,46 us, `proc_pidpath` 0,88 us, `KERN_PROCARGS2` 2,67 us, `sysctl KERN_PROC_PID` 7,2 us.
- Limite : il n'y a pas de notion de `Tgid` ni de `PPid` namespacés, et `pbi_status` (SRUN=2) ne distingue pas les états Linux R, S, D, T ; ceux-ci viennent de `pth_run_state` par thread.

### H4, canal coopératif contre task port (MESURÉ, h4)

Témoin `h4_coop_channel.c`. La cible est un binaire lancé par exec qui possède 4 Mio de mémoire privée ; le traceur veut `len` octets à une adresse de cette mémoire. Temps de la requête à l'arrivée des octets dans le tampon du traceur, p50 en us :

| Méthode | 8 o | 4 Kio | 64 Kio | 1 Mio |
| --- | --- | --- | --- | --- |
| lecture directe d'un mapping partagé (shm) | 0,00 | 0,04 | 0,54 | 10,8 |
| `mach_vm_remap` de la mémoire privée de la cible (lecture par memcpy) | 0,00 | 0,04 | 0,50 | 10,8 |
| `mach_vm_read_overwrite` (task port) | 0,75 | 0,75 | 22,6 | 68,9 |
| requête/réponse, attente active sur mémoire partagée | 0,12 | 0,46 | 1,79 | 25,5 |
| pipe, notification seule (données en mémoire partagée) | 4,21 | 4,29 | 7,58 | 44,6 |
| pipe, données dans la réponse | 4,29 | 4,42 | 7,12 | 121,7 |
| sémaphore POSIX nommé, deux sens | 4,17 | 4,21 | 7,04 | 44,4 |
| socketpair AF_UNIX stream | 4,79 | 5,58 | 7,54 | 44,3 |
| socketpair AF_UNIX dgram | 5,12 | 5,83 | 7,42 | 44,2 |
| message Mach (données en ligne dans la réponse) | 6,29 | 6,79 | 20,25 | non mesuré |
| futex `os_sync_wait_on_address` avec `SHARED`, deux sens | 6,08 | 6,25 | 7,29 | 45,6 |
| mutex et variable de condition `PTHREAD_PROCESS_SHARED` | 7,79 | 7,58 | 8,08 | 44,9 |

- Aucun effet mesurable de la classe de QoS (par défaut contre `USER_INTERACTIVE`) sur ces temps. p99 des chemins bloquants : 9 à 12 us.
- Un réveil inter-processus bloquant coûte environ 4 us (pipe, sémaphore) à 6 us (futex partagé, message Mach). Le futex partagé n'est pas plus rapide qu'un pipe. L'attente active descend à 0,12 us mais occupe un coeur.
- `shm_open` plus `ftruncate` plus `mmap(MAP_SHARED)` et `sem_open` nommés fonctionnent sans droit particulier ; les mutex et conditions `PTHREAD_PROCESS_SHARED` fonctionnent dans la page partagée.
- `mach_vm_read_overwrite` du task port a un coût fixe d'environ 0,6 us (aller-retour noyau) puis monte avec la taille (36 Go/s en gros transfert) : c'est le meilleur chemin sans coopération de mapping, 5 à 10 fois plus rapide qu'un aller-retour par socket ou pipe.

### H5, passage de ports Mach sans launchd et sans task_for_pid (MESURÉ, h6_port_inherit)

Témoin `h6_port_inherit.c`.

- L'enfant peut donner son propre `mach_task_self()` au parent par message (`MACH_MSG_TYPE_COPY_SEND` dans un descripteur de port) : le port est déplaçable, et le parent obtient `pid_for_task`, `mach_vm_read`, `task_threads` et `thread_get_state` qui réussissent. C'est vrai pour l'enfant signé get-task-allow et aussi pour l'enfant signé sans aucun entitlement (copie `.noent`, signature ad hoc non durcie). Pas d'invite. L'attachement coopératif fournit donc le droit d'inspection sans `task_for_pid`, et sans get-task-allow, pour nos propres binaires Mach-O non durcis. À ne pas généraliser à un binaire à runtime durci ou de plateforme : non testé.
- Moyen de transporter le premier port : l'emplacement spécial `TASK_BOOTSTRAP_PORT`, hérité par `fork` (le parent y place temporairement son port, puis le restaure) ou fixé par `posix_spawnattr_setspecialport_np(TASK_BOOTSTRAP_PORT)`. Les deux fonctionnent pour un enfant forké et pour un enfant exec. `TASK_ACCESS_PORT` fait échouer `posix_spawn` avec `EINVAL`. `mach_ports_register` plus `mach_ports_lookup` rendent un port à l'enfant, mais les messages envoyés dessus n'arrivent pas chez le parent : inutilisable.
- `bootstrap_register` depuis un outil signé ad hoc réussit (`kr=0`), `bootstrap_look_up` retrouve le nom, `bootstrap_check_in` rend 1103 (service actif) : un rendez-vous par nom dans la session de l'utilisateur est disponible sans plist launchd, le nom disparaît avec le port.
- Un port d'exception posé au niveau tâche est hérité par `fork` et par `fork` plus `exec`. Un traceur qui l'a posé reçoit les fautes de ses descendants (avec leurs ports de contrôle dans le message), l'équivalent de `PTRACE_O_TRACEFORK` pour les exceptions.

### H6, le modèle débogueur par exceptions Mach sur une tâche étrangère (MESURÉ, h7_foreign_exc)

Témoin `h7_foreign_exc.c`. Le traceur pose `task_set_exception_ports` sur la tâche de son enfant get-task-allow et sert les exceptions dans un thread gestionnaire.

- `brk #0x77` : `EXC_BREAKPOINT`, codes `[1, pc]`, `pc` pointant sur le `brk` ; `udf #0x1234` : `EXC_BAD_INSTRUCTION`, codes `[1, mot d'instruction]` ; `ldr` en 0x10 : `EXC_BAD_ACCESS`, codes `[1 (KERN_INVALID_ADDRESS), adresse]`. Le gestionnaire réécrit x0 et `pc+4`, répond `KERN_SUCCESS` et l'enfant reprend avec l'état réécrit, pour les trois comportements (`EXCEPTION_STATE_IDENTITY`, `EXCEPTION_STATE`, `EXCEPTION_DEFAULT`). Les messages `DEFAULT` et `STATE_IDENTITY` transportent le port de contrôle de la tâche fautive (utilisable par `mach_vm_read`).
- Coût d'une exception inter-processus : commande par pipe plus exception plus réponse, p50 11,7 us (STATE_IDENTITY), 10,7 us (STATE), 12,0 us (DEFAULT), contre 5,9, 4,7 et 6,0 us pour la même commande sans exception : environ 6 us par exception.
- Écriture sur page en lecture seule : `EXC_BAD_ACCESS`, codes `[2 (KERN_PROTECTION_FAILURE), adresse]`. Le traceur rend la page inscriptible par `mach_vm_protect` dans l'enfant et répond : l'instruction est rejouée et réussit (p50 14,0 us pour la commande entière, environ 8 us par faute). C'est le mécanisme de protection en écriture à la granularité de page hôte (16 Kio).
- Exécution de la page de code modifiée de h1 : `EXC_BAD_ACCESS` avec `KERN_PROTECTION_FAILURE` sur la recherche d'instruction (adresse = pc), puis `SIGBUS` : une page signée modifiée n'est plus exécutable, même après `mach_vm_protect(R-X)`.
- Pas matériel : `thread_set_state(ARM_DEBUG_STATE64)` avec `mdscr_el1` bit 0 donne un `EXC_BREAKPOINT` (codes `[1, 0]`) par instruction. Le noyau efface le bit à chaque pas : le gestionnaire doit le réarmer avant de répondre. 20 000 pas à 9,3 us par pas ; 3,1 % des exceptions sont répétées au même pc sans progression (un client de pas doit le tolérer) et toutes les autres avancent d'exactement une instruction (compteur de boucle exact).
- Points d'arrêt et points de surveillance matériels : `bvr0`/`bcr0` (valeur 0x1e5) sur `hi_target_fn` donne `EXC_BREAKPOINT` `[1, pc]` ; `wvr0`/`wcr0` (valeur 0x1ff5, écriture sur 8 octets) donne `EXC_BREAKPOINT` `[0x102, adresse de donnée]`. `hw.optional.breakpoint` vaut 6 et `hw.optional.watchpoint` vaut 4 sur cette machine : de quoi émuler les 4 registres de débogage x86 DR0 à DR3 sous réserve de la différence de largeur.
- `PT_ATTACHEXC` plus port d'exception : le `SIGSTOP` d'attache et les signaux ultérieurs arrivent comme `EXC_SOFTWARE`, codes `[0x10003 (EXC_SOFT_SIGNAL), numéro de signal]`. Répondre seulement `KERN_SUCCESS` laisse le signal être délivré (l'enfant meurt de SIGUSR1) ; `ptrace(PT_THUPDATE, pid, port_du_thread, 0)` avant de répondre supprime le signal (l'enfant survit) ; avec `data=SIGUSR1` il le délivre. C'est un arrêt de livraison de signal avec choix du signal injecté, comme `PTRACE_CONT` avec signal.

## I : exceptions Mach et pièges (à compléter)

À compléter : ports d'exception pour soi, état réécrit, comparaison signaux contre Mach, sonde `svc #0` en processus enfant.

## J : temps et identité

Résultats partiels de la tentative interrompue (`evidence/darwin-process/j1_time_ident.log`, 2026-10-02 09:59, arrêt pendant J2). À refaire, la relance complète est en cours.

- MESURÉ `mach_timebase_info` : 125/3, soit 41,67 ns par tick (24 MHz).
- MESURÉ résolution : `CLOCK_REALTIME` et `CLOCK_MONOTONIC` avancent par pas de 1000 ns (microseconde). `CLOCK_MONOTONIC_RAW`, `CLOCK_UPTIME_RAW` et `mach_absolute_time` avancent par pas de 41 ns.
- MESURÉ coût d'un appel : `CLOCK_MONOTONIC_RAW_APPROX` 4,8 ns, `mach_absolute_time` 5,0 ns, `CLOCK_REALTIME` 12,8 ns, `CLOCK_MONOTONIC` 18,3 ns, `CLOCK_THREAD_CPUTIME_ID` 133 ns, `CLOCK_PROCESS_CPUTIME_ID` 469 ns.
- MESURÉ `CLOCK_MONOTONIC` retarde de 5,893 s sur `CLOCK_UPTIME_RAW` après 177 906 s d'activité, et vaut le temps de l'horloge murale depuis `kern.boottime` (177 901,0 s). DÉDUIT : `CLOCK_MONOTONIC` est discipliné sur l'heure murale (dérive d'environ 33 ppm de l'oscillateur brut), alors que `CLOCK_MONOTONIC_RAW` égale `mach_absolute_time`. À confirmer à la relance.
- MESURÉ identifiants d'horloge 1, 2, 3, 7, 10, 11, 13 à 15, 17 à 19 : `EINVAL`. Il n'existe pas de `CLOCK_BOOTTIME`, `CLOCK_REALTIME_COARSE`, `CLOCK_MONOTONIC_COARSE` ni d'identifiants dynamiques de type Linux.
- MESURÉ minuteries au QoS par défaut, dépassement de la durée demandée : `nanosleep` 20 us : p50 6,7 us ; 100 us : p50 28,0 us ; 1000 us : p50 255,8 us (p99 264,5 us).
- ANOMALIE à vérifier : `mrs cntfrq_el0` rend 1 000 000 000 et `mrs cntvct_el0` rend 41 alors que `mach_absolute_time` vaut 4,27e12 (24 MHz). Le registre de compteur lu en EL0 n'est donc pas la source de `mach_absolute_time` sur cette machine. Conséquence potentielle pour l'émulation de RDTSC : lire `mach_absolute_time` et non `cntvct_el0`. À mesurer proprement à la relance.

## K : transport de ptrace invité coopératif (à compléter)

À compléter : prototype mémoire partagée + socket + primitive de type futex, allers-retours d'arrêt et de reprise en microsecondes.

## Implications pour ptrace, procfs et signaux sur Darwin (à compléter)

À compléter après les mesures H, I, J et K. Aucune conclusion n'est tirée avant.

## Questions ouvertes

- À compléter.
