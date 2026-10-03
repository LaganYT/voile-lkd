# Faits macOS sur les processus, l'IPC et la synchronisation (partie A)

Dernière mise à jour : 2026-10-02 (premier jet, reprise après interruption de la session). Toutes les sections marquées « a completer » n'ont pas encore de mesure.

Ce document sépare ce qui a été MESURE dans cette session (petits programmes C lancés sur cette machine) de ce qui est LU (en-têtes du SDK, pages de manuel) et de ce qui est DEDUIT. Les programmes sont dans `witnesses/darwin-process/` (préfixes `f_` et `g_`), les journaux dans `evidence/darwin-process/`. Chaque journal contient des lignes `PASS`, `FAIL`, `INFO` ou `SKIP` : `FAIL` signifie que l'attente écrite dans le programme n'a pas été vérifiée, ce qui est une donnée et pas forcément un bogue.

## Machine et limites mesurées

Mesuré (commandes `sw_vers`, `uname`, `sysctl`, `ulimit` de cette session) :

- macOS 27.0.1 (build 26A434), noyau `xnu-13432.1.9~1/RELEASE_ARM64_T6050`, Apple M5 Max, 18 coeurs logiques, 36 Gio de mémoire (`hw.memsize` = 38654705664).
- `hw.pagesize` = 16384.
- `kern.maxfiles` = 276480, `kern.maxfilesperproc` = 138240, `kern.maxproc` = 9000, `kern.maxprocperuid` = 6000.
- `ulimit -n` du shell de la session = 1048576 (valeur du terminal, pas celle d'une application lancée par le Finder : a completer).
- Utilisateur sans privilège (uid 501), pas de root, pas d'extension noyau, aucun dialogue d'autorisation déclenché.

## Etat d'avancement

| Fait | Programme | Etat |
| --- | --- | --- |
| F fork depuis un processus multi-thread, survie dans l'enfant | `f1_fork_mt` | mesuré, 17 PASS 0 FAIL |
| F coût de fork (64 Gio PROT_NONE + 1 Gio touché) | `f2_fork_cost` | mesuré |
| F posix_spawn et héritage des fd | `f3_spawn_fd` | mesuré, 32 PASS 2 FAIL (écarts POSIX) |
| F execve depuis un processus multi-thread | `f4_exec_mt` | mesuré (compile sans avertissement avec `-Wall -Wextra`) |
| F SCM_RIGHTS, identité du pair, fileport | `f5_scm_rights` | mesuré, 27 PASS 2 FAIL (écarts Linux) |
| F équivalents de memfd_create (shm, fichier, entrée mémoire Mach) | `f6_memfd` | mesuré, 15 PASS 0 FAIL |
| F mmap de fichier à des offsets non alignés sur 16 Kio | `f7_mmap_offset` | a completer |
| F alias de pages et équivalent de mremap | `f8_alias_remap` | a completer |
| G os_sync / ulock entre processus, latence, FUTEX | `g1_futex` | a completer |
| G kqueue pour epoll, eventfd, timerfd, signalfd, inotify | `g2_kqueue` | a completer |
| G sockets AF_UNIX | `g3_unix_sockets` | a completer |
| G pipe2, O_CLOEXEC, poll, limites de fd, sendfile, splice | `g4_fd_misc` | a completer |
| G prototype de epoll à déclenchement par niveau | `g5_epoll_proto` | a completer |

## F. Processus et mémoire partagée

### F1. fork() depuis un processus multi-thread (`f1_fork_mt`, journal `evidence/darwin-process/f1_fork_mt.log`)

Mesuré. Le programme crée 5 threads auxiliaires (boucle d'attente active, boucle malloc/free, un thread qui tient un `pthread_mutex_t` et un `os_unfair_lock` bloqué dans `read`, un thread qui tient le verrou stdio d'un `FILE`, un thread bloqué dans `read`) puis forke depuis le thread principal et depuis un thread secondaire. Résultat : 17 PASS, 0 FAIL.

| Question | Résultat mesuré |
| --- | --- |
| Threads dans l'enfant | 1 seul (`task_threads` renvoie 1). Les threads auxiliaires ne tournent plus (compteurs figés). |
| Thread qui a forké | Son `pthread_t`, ses valeurs `pthread_getspecific` et ses variables `__thread` survivent. Son identifiant (`pthread_threadid_np`) est nouveau (111808 contre 111798). `pthread_main_np()` vaut 1 si le fork vient du thread principal et 0 s'il vient d'un thread secondaire : contrairement à Linux, le thread qui fork ne devient pas « le thread principal ». |
| Ports Mach | Le droit de réception et d'envoi créé par le parent n'est pas hérité (`mach_port_type` renvoie `KERN_INVALID_NAME`). `mach_task_self()` et `mach_thread_self()` désignent bien le nouveau processus, mais les noms numériques changent (tâche 0x203 chez le parent, 0x103 chez l'enfant ; thread secondaire 0x1a03 chez le parent, 0x203 chez l'enfant). Tout nom de port mis en cache avant le fork est donc faux dans l'enfant. `bootstrap_look_up` d'un service système fonctionne dans l'enfant. Les ports d'exception de la tâche sont hérités (2 entrées). |
| Verrous tenus par un autre thread | `pthread_mutex_t` : reste verrouillé, `trylock` renvoie `EBUSY`, un `lock` bloquant reste suspendu (pendu après 700 ms, enfant tué). `FILE` verrouillé : `fputs` reste suspendu. `os_unfair_lock` : un `lock` ne pend pas, il fait mourir l'enfant. Le rapport d'incident du système (copié dans `evidence/darwin-process/os-crash-reports/f1_fork_mt-2026-10-02-102331.ips`) dit `BUG IN CLIENT OF LIBPLATFORM: os_unfair_lock is corrupt, or owner thread exited without unlocking`, `EXC_BREAKPOINT`, signal `SIGKILL`, avec l'annotation `crashed on child side of fork pre-exec`. |
| malloc dans l'enfant | Fonctionne (libmalloc enregistre ses gestionnaires atfork). 300 forks successifs pendant qu'un thread martèle malloc : 300 enfants terminés, 0 bloqué, 1,24 ms par fork+enfant+attente. |
| `pthread_create` dans l'enfant | Fonctionne. |
| libdispatch dans l'enfant | Si le parent n'a jamais utilisé GCD avant le fork : `dispatch_async` sur une file globale fonctionne dans l'enfant. Si le parent l'avait déjà utilisé : l'enfant meurt de `SIGSEGV` (journal `f1_fork_mt.run2-gcd-warm-crash.log`). |
| Signaux | Dispositions et masque du thread qui fork hérités ; signaux en attente non hérités ; `alarm` en cours non hérité. |
| `atfork` | `pthread_atfork` : prepare et parent s'exécutent dans le parent, child dans l'enfant. |
| `vfork` | Se comporte comme `fork` : une écriture de l'enfant dans une variable globale n'est pas visible du parent. `vfork` suivi de `execl` fonctionne. |

Mémoire après fork (mesuré, régions de 64 Mio) :

- `MAP_SHARED|MAP_ANON` : réellement partagée. L'écriture de l'enfant est vue par le parent et inversement. Deux processus ont fait 2 x 2 000 000 `atomic_fetch_add` sur le même mot : résultat exact 4 000 000.
- `MAP_PRIVATE|MAP_ANON` : copie sur écriture, isolation dans les deux sens.
- Fichier mappé `MAP_PRIVATE` : copie sur écriture, le fichier n'est jamais modifié. Fichier mappé `MAP_SHARED` : l'écriture de l'enfant est visible du parent et dans le fichier.
- `munmap` et `mprotect` faits par l'enfant sur sa vue ne touchent pas celle du parent.
- `minherit(VM_INHERIT_SHARE)` sur une région privée anonyme : après fork la région est partagée comme un `MAP_SHARED` (écritures visibles dans les deux sens). `minherit(VM_INHERIT_NONE)` : la région n'existe pas dans l'enfant (`mach_vm_region` ne la trouve pas). Ces deux appels réussissent et jouent le rôle de `MADV_DONTFORK` et de « rendre partagé après coup ».
- Coût d'un défaut de copie sur écriture (pages de 16 Kio, médiane de 3 séries sur 256 Mio) : premier accès à une page vierge 683 ns, accès suivant sans défaut 7 ns, première écriture de l'enfant après fork 1454 ns par page, première écriture du parent avec un enfant inactif 2252 ns par page, réécriture par le parent après la mort de l'enfant 378 ns par page (l'état de copie sur écriture n'est donc pas nettoyé tout de suite).
- Région `MAP_JIT` : le code compilé par le parent s'exécute dans l'enfant ; l'enfant peut le réécrire (bascule `pthread_jit_write_protect_np`) ; la copie du parent reste intacte. Le fork d'une région `MAP_JIT` coûte beaucoup plus cher qu'un fork ordinaire (voir F2).

### F2. Coût de fork() (`f2_fork_cost`, journal `evidence/darwin-process/f2_fork_cost.log`)

Mesuré, 30 forks par configuration, enfant qui fait `_exit(0)` aussitôt. Valeurs en microsecondes (médiane du régime établi, sauf mention). La machine n'était pas au repos (charge 1 à 4 pendant les mesures, autres équipes en train de compiler) : les maxima sont bruités.

| Configuration | `fork()` | enfant démarré | total jusqu'à l'attente |
| --- | --- | --- | --- |
| C0 processus nu | 610 | 721 | 920 |
| C1 réservation 64 Gio `PROT_NONE|MAP_NORESERVE` | 644 | 690 | 856 |
| C2 réservation 64 Gio + 1 Gio touché (privé) | 617 (premier fork : 2675) | 656 | 821 |
| C3 1 Gio `mprotect(RW)` + touché dans la réservation | 692 (premier : 3296) | 738 | 910 |
| C4 1 Gio RW non touché | 675 | 708 | 869 |
| C5 1 Gio touché `MAP_SHARED` | 750 (premier : 832) | 792 | 987 |
| C7 4 Gio touchés (privé) | 635 (premier : 8755) | 673 | 857 |
| C6 1 Gio fragmenté en 32 768 entrées VM | 4529 (premier : 15472) | 4511 | 9113 |
| 200 threads endormis | 767 | 735 | 1083 |
| C8b région `MAP_JIT` de 1 Gio, 0 Mio touché | 7744 | 7748 | 7953 |
| `MAP_JIT` 1 Gio, 16 / 64 / 256 Mio touchés | 8921 / 10948 / 21296 | | 9186 / 11321 / 23013 |
| `MAP_JIT` 1 Gio, 256 Mio touchés, `minherit(NONE)` | 678 | 677 | 875 |
| `MAP_JIT` 1 Gio, 256 Mio touchés, `minherit(SHARE)` | 593 | 567 | 766 |
| Mémoire anonyme ordinaire 1 Gio, 256 Mio touchés | 707 | 747 | 929 |

Autres chiffres mesurés :

- Réservation `PROT_NONE` seule : 256 Gio 851 us, 1 Tio 1398 us, 4 Tio 4285 us, 16 Tio 16 423 us, 64 Tio (2^46) 68 993 us. Le coût de `fork()` croît donc de façon linéaire avec la taille réservée, environ 1,07 ms par Tio, et le temps total (enfant détruit) à peu près le double (64 Tio : 142 ms). Une réservation de 64 Gio ne coûte rien de mesurable.
- Le premier fork d'un processus avec de la mémoire privée touchée paie la mise en lecture seule des pages du parent (1 Gio : environ 2 ms de plus, 4 Gio : environ 8 ms de plus) ; les forks suivants ne repaient pas ce coût tant que le parent n'écrit pas.
- Après le fork, le parent qui réécrit 1 Gio d'un coup (enfant vivant) met 127 à 182 ms (1944 à 2777 ns par page de 16 Kio, 5,5 à 7,9 Go/s).
- `posix_spawn(/usr/bin/true)` : l'appel rend la main en 90 à 106 us et le total jusqu'à l'attente est de 740 à 910 us, indépendamment de la taille de l'espace d'adressage du parent (il ne copie pas l'espace d'adressage). `fork` + `exec` : 1,5 à 1,65 ms.

Déduit : le coût dominant d'un fork émulé est donc (a) 0,6 ms de base, (b) environ 1,1 ms par Tio réservé, (c) environ 7,2 ms par Gio de région `MAP_JIT` mappée plus 54 us par Mio touché, sauf si la région est marquée `VM_INHERIT_NONE` ou `VM_INHERIT_SHARE`, (d) le coût différé des copies sur écriture. Les mesures de (c) montrent que le noyau copie ou parcourt page par page les régions `MAP_JIT` au fork, ce qu'il ne fait pas pour la mémoire anonyme ordinaire.

### F3. posix_spawn du même binaire et héritage des descripteurs (`f3_spawn_fd`, journal `evidence/darwin-process/f3_spawn_fd.log`)

Mesuré. 32 PASS, 2 FAIL (les deux FAIL sont des écarts avec POSIX, voir plus bas : ce sont des données).

- Mode par défaut : 0, 1, 2 et les descripteurs ordinaires sont hérités, un descripteur `FD_CLOEXEC` est fermé, un descripteur `FD_CLOEXEC|FD_CLOFORK` est fermé. Un descripteur qui n'a que `FD_CLOFORK` (drapeau « fermer au fork », nouveau dans le SDK 27, `O_CLOFORK` 0x08000000, `FD_CLOFORK` 2, `F_DUPFD_CLOFORK` 115, `sys/fcntl.h:158,354,361`) est lui aussi fermé dans l'enfant d'un `posix_spawn`.
- `POSIX_SPAWN_CLOEXEC_DEFAULT` sans action : tout est fermé dans l'enfant, 0, 1 et 2 compris. `posix_spawn_file_actions_addinherit_np(fd)` garde le descripteur ouvert, y compris s'il a `FD_CLOEXEC` (le drapeau est effacé dans l'enfant). `adddup2(f, 20)` crée le descripteur 20 et ferme `f` dans l'enfant. Un descripteur cité par aucune action est fermé. `addinherit_np` suivi de `addclose` laisse le descripteur fermé.
- Écart avec POSIX (2 FAIL) : `adddup2(h, h)` sur un descripteur `FD_CLOEXEC` ne le garde pas ouvert, ni avec `CLOEXEC_DEFAULT` ni en mode par défaut (le descripteur est fermé dans l'enfant). POSIX demande qu'un `dup2` sur soi-même efface `FD_CLOEXEC`. Sur macOS il faut `addinherit_np`.
- Limite silencieuse : avec `CLOEXEC_DEFAULT`, les descripteurs dont le numéro est supérieur ou égal à 10240 (`OPEN_MAX`) ne sont jamais hérités, même nommés par `addinherit_np`, et `posix_spawn` renvoie quand même 0. Sondé avec 1000, 5000, 10000, 10039, 10040, 10041, 10100, 10240, 12000, 12300 actions sur les descripteurs 200 à 12499 : l'enfant a toujours exactement les descripteurs 200 à 10239 (premier fermé : 10240). Hors `CLOEXEC_DEFAULT`, 20 000 descripteurs ouverts sont tous hérités. Le contournement est d'utiliser `adddup2(fd, petit_numero)` pour les descripteurs au-dessus de 10239 (non mesuré).
- Coût : l'appel `posix_spawn` rend la main en 106 à 150 us, le total jusqu'à l'attente d'un enfant qui sort aussitôt est de 1,14 à 1,42 ms ; 100 actions d'héritage ne changent rien de mesurable ; 4000 actions : 682 us pour l'appel ; 20 000 actions : 2,1 ms.
- Mémoire partagée avec un enfant lancé par `posix_spawn` : un descripteur `shm_open` (puis `shm_unlink`) ou `mkstemp` (puis `unlink`), hérité avec `CLOEXEC_DEFAULT` + `addinherit_np`, est mappé `MAP_SHARED` par l'enfant à une autre adresse (parent 0x10322c000, enfant 0x104788000). Le motif du parent est intact, l'écriture de l'enfant est visible du parent, et 2 x 100 000 incréments atomiques depuis les deux processus donnent exactement 200 000 pour les deux supports.
- Attributs : un signal ignoré reste ignoré, un gestionnaire repasse à l'action par défaut, le masque de signaux, `umask`, le répertoire courant et la limite `RLIMIT_NOFILE` sont hérités, l'environnement est celui qu'on passe. `POSIX_SPAWN_SETSIGMASK` et `SETSIGDEF` fonctionnent, `SETPGROUP` (pgroup 0) et `SETSID` font de l'enfant un chef de groupe et de session.
- `POSIX_SPAWN_START_SUSPENDED` : l'enfant démarre arrêté (`waitpid(WUNTRACED)` le voit arrêté, avec un numéro de signal 0), `SIGCONT` le relance.
- Erreurs renvoyées directement par `posix_spawn` : `ENOEXEC` pour un fichier texte sans `#!`, pour un en-tête ELF et pour un fichier vide ; `ENOENT` pour un interpréteur `#!` manquant et pour un chemin absent ; `EACCES` pour un fichier sans droit d'exécution et pour un répertoire.
- `fork` : les descripteurs `O_CLOFORK` sont fermés dans l'enfant du fork, les autres restent. `dup()` ne copie pas le drapeau `FD_CLOFORK`, `F_DUPFD_CLOFORK` le pose.

### F4. execve depuis un processus multi-thread (`f4_exec_mt`, journal `evidence/darwin-process/f4_exec_mt.log`)

Mesuré. Le programme crée un processus à 6 threads (principal, un thread actif, un endormi, un bloqué dans `read`, un bloqué dans `os_sync_wait_on_address`, un bloqué dans `kevent`) puis appelle `execve` depuis un thread secondaire (puis depuis le thread principal, puis avec `posix_spawn(POSIX_SPAWN_SETEXEC)`).

- Le pid, le pid parent, le groupe de processus et la date de démarrage du processus (`p_starttime`) sont inchangés. Il ne reste qu'un thread (le nouveau thread principal : `pthread_main_np()` vaut 1 même si l'`execve` venait d'un thread secondaire) ; le masque de signaux de ce thread est celui du thread qui a appelé `execve` (SIGTERM bloqué reste bloqué).
- Descripteurs : ordinaire conservé, `FD_CLOEXEC` fermé, `FD_CLOFORK` seul conservé (ce drapeau n'agit qu'au fork et au spawn). Signaux : ignoré reste ignoré, gestionnaire remis à l'action par défaut. `umask`, valeur de `nice`, limite `RLIMIT_NOFILE` conservés. Une alarme armée avant l'exec reste armée (60 s restantes). Un droit de réception Mach créé avant l'exec a disparu (le nom peut être recyclé par la nouvelle image : une première version du test s'est fait piéger, la version finale compare le contexte du port). Les ports d'exception de la tâche sont ceux de la nouvelle image (2 entrées). Une page `MAP_SHARED|MAP_ANON` de l'ancienne image n'existe plus.
- Latence jusqu'à la première ligne de `main` de la nouvelle image (médiane) : processus de 6 threads 2,1 ms ; `posix_spawn(SETEXEC)` 1,97 ms ; avec une réservation de 64 Gio et 1 Gio touché 15,1 ms ; avec une réservation `PROT_NONE` de 2^46 octets 83,4 ms. Le coût de démontage de l'ancien espace d'adressage est donc payé dans l'`execve` (environ 13 ms par Gio touché, environ 1,3 ms par Tio réservé).
- Table d'erreurs de `execve` : fichier texte sans `#!` `ENOEXEC`, en-tête ELF `ENOEXEC`, fichier vide `ENOEXEC`, MH_DYLIB arm64 `ENOEXEC`, Mach-O ARM 32 bits `EBADARCH` (86), interpréteur `#!` manquant `ENOENT`, chemin absent `ENOENT`, répertoire `EACCES`, fichier sans droit x `EACCES`, chemin de 2099 octets `ENAMETOOLONG`, boucle de liens `ELOOP`, arguments au-delà de `kern.argmax` (1 048 576) `E2BIG`. Un Mach-O arm64 aux commandes de chargement tronquées ne renvoie pas d'erreur : le processus est tué (l'`execve` a dépassé le point de non-retour). `execve(chemin, NULL, environ)` réussit comme sous Linux. Exécuter un fichier ouvert en écriture réussit (pas de `ETXTBSY` sur macOS).

### F5. SCM_RIGHTS, identité du pair et fileport (`f5_scm_rights`, journal `evidence/darwin-process/f5_scm_rights.log`)

Mesuré. 27 PASS, 2 FAIL (deux écarts avec Linux, détaillés ci-dessous). Le journal de la première exécution est conservé (`f5_scm_rights.run1.log`) : mêmes résultats, seul le libellé d'un test a été clarifié ensuite.

- Types de descripteurs qui passent par `SCM_RIGHTS` sans erreur : fichier ordinaire, les deux bouts d'un pipe, descripteur `shm_open` (déjà `shm_unlink`), `mkstemp` (déjà `unlink`), répertoire, socket AF_UNIX datagramme, `/dev/null`, `O_EVTONLY`, `O_SYMLINK`, socket AF_UNIX en écoute, socket TCP non connectée, maître et esclave de pty. À l'arrivée on a le même objet (même `st_mode`) et la même description de fichier ouverte : le curseur posé à 7 par la copie reçue est vu par l'émetteur. Un `connect()` sur le chemin d'une socket en écoute reçue par ce canal est accepté par la socket d'origine.
- Un descripteur `kqueue` est refusé par `sendmsg` avec `EINVAL` (22). C'est un FAIL du test (qui espérait le passage), et la donnée à retenir : un objet epoll émulé par kqueue ne peut pas être transmis à un autre processus. Ce résultat recoupe `logs/host-test-kq-fork-scm.log` de l'étude HyperLinux.
- Drapeaux : le descripteur reçu n'a aucun drapeau (`FD_CLOEXEC` et `FD_CLOFORK` de l'émetteur ne sont pas transmis). La valeur Linux de `MSG_CMSG_CLOEXEC` (0x40000000) est ignorée sans erreur et le descripteur reçu n'a pas `FD_CLOEXEC`. Le récepteur doit appeler `fcntl(F_SETFD)` lui-même.
- Nombre de descripteurs par message : 254 au plus (flux comme datagramme, longueur de contrôle 1028 octets), 255 donne `EINVAL`. Aucun descripteur n'a fui pendant le balayage (23 ouverts avant et après). La limite de 253 de Linux (`SCM_MAX_FD`) est une connaissance externe à cette session, pas une mesure.
- Écart avec Linux (le second FAIL) : si le récepteur ne donne de la place que pour 3 descripteurs et que 10 sont en vol, `recvmsg` renvoie 3 descripteurs avec `MSG_CTRUNC`, mais le noyau a installé les 10 dans la table du récepteur (ouverts : 23 puis 33, puis 30 après fermeture des 3 annoncés). Sept descripteurs restent ouverts sans que le programme en connaisse le numéro. Linux ferme ceux qui ne tiennent pas. Conséquence pour l'émulation : toujours fournir une zone de contrôle assez grande pour 254 descripteurs (1028 octets), jamais moins que ce que l'invité a demandé sans vérification, et balayer la table après un `MSG_CTRUNC`.
- Message sans octet de données avec un descripteur : accepté en flux comme en datagramme, `recvmsg` rend 0 octet et 1 descripteur.
- Un descripteur fermé par l'émetteur pendant qu'il est en vol arrive quand même et mappe la même mémoire partagée.
- Avec `fork` : l'enfant reçoit un descripteur `shm_open` par `SCM_RIGHTS`, le mappe `MAP_SHARED`, et le parent voit son écriture. L'enfant renvoie aussi un bout de pipe au parent.
- Identité du pair (`SO_PEERCRED` de Linux n'existe pas). `getsockopt(SOL_LOCAL, LOCAL_PEERCRED)` rend un `xucred` (version 0, uid 501, 16 groupes) sans pid. `LOCAL_PEERPID` et `LOCAL_PEEREPID` rendent le pid, `LOCAL_PEERUUID` l'UUID, `LOCAL_PEERTOKEN` un jeton d'audit de 32 octets, `getpeereid` uid et gid. Cela vaut pour un `socketpair` (les deux bouts) et pour une socket acceptée sur un chemin dont le client est un enfant forké. Pour un `socketpair` créé avant un `fork`, le pid rendu aux deux bouts est celui du créateur (43034 des deux côtés dans le journal), pas celui du processus qui tient l'autre bout. Il faudra donc traduire un pid hôte en pid invité, et le pair d'un `socketpair` hérité sera faux si l'émulation n'enregistre pas elle-même qui a fermé quoi. `sendmsg` avec `SCM_CREDS` sur AF_UNIX : `EINVAL` (pas de `SCM_CREDENTIALS`). `setsockopt(SO_TIMESTAMP)` est accepté sur AF_UNIX.
- `fileport` (API privée de libSystem) : `fileport_makeport(fd)` donne un port Mach qui porte la description de fichier ouverte, `fileport_makefd(port)` crée un nouveau descripteur sur le même objet (même curseur : posé à 5 par un descripteur, lu à 5 par l'autre ; un descripteur `shm_open` converti puis reconverti donne une vue qui alias les mêmes pages). Dans un enfant forké, `fileport_makefd` du port hérité « par son nom » échoue (`EINVAL`), parce que les noms de port ne sont pas hérités par `fork` (voir F1). F6 montre qu'un fileport envoyé par message Mach à un processus non apparenté fonctionne.

### F6. Équivalents de memfd_create (`f6_memfd`, journal `evidence/darwin-process/f6_memfd.log`)

Mesuré. 15 PASS, 0 FAIL après correction de deux erreurs du programme (voir plus bas). `memfd_create` n'existe pas (`dlsym` renvoie NULL). Trois candidats ont été comparés : B1 `shm_open`, B2 fichier temporaire ouvert puis détaché (`mkstemp` + `unlink`, APFS), B3 entrée de mémoire Mach.

| Question | B1 `shm_open` | B2 fichier détaché |
| --- | --- | --- |
| Longueur du nom | 31 caractères au plus, barre oblique initiale comprise ; 32 donne `ENAMETOOLONG` | sans objet |
| `ftruncate` | une seule fois : le premier appel réussit et la taille est arrondie à 16 Kio (100 donne `st_size` 16384) ; tout appel suivant, même à la même taille ou à 0, donne `EINVAL` | autant de fois qu'on veut, grandir ou rétrécir |
| `read`, `write`, `pread`, `pwrite`, `lseek` | `ENXIO` pour `read` et `write`, `ESPIPE` pour les autres : on ne fait que `mmap` | tout fonctionne |
| `mmap` `MAP_SHARED` lecture-écriture | oui | oui |
| `mmap` `MAP_PRIVATE` | `EINVAL` | oui (copie sur écriture, le fichier ne bouge pas) |
| `mmap` au-delà de la taille | `EINVAL` | accepté, l'accès après la fin donne SIGBUS ou SIGSEGV |
| `PROT_EXEC` | `mmap` accepté, `mprotect(R+X)` donne `EACCES` | `mmap(PROT_EXEC)` donne `EPERM`, `mprotect(R+X)` est accepté |
| Offset 4096 puis 16384 | 4096 donne `EINVAL`, 16384 accepté | pareil |
| Deux vues à deux adresses | alias dans un processus ; `MAP_FIXED` dans une réservation `PROT_NONE` fonctionne | pareil |
| Taille de 1 Gio à 1 Tio | `ftruncate` et `mmap` en quelques microsecondes, objet creux (`st_blocks` 0), premier et dernier octets écrits | pareil, quelques microsecondes de plus |
| Premier accès par page de 16 Kio (médiane de 256 Mio) | 570 à 719 ns | 969 à 1497 ns (la comparaison anonyme privée donne 629 à 683 ns, anonyme partagée 587 à 745 ns) |
| Comptabilité mémoire (`phys_footprint`) | +257 Mio, interne | +0, 256 Mio « externes » |
| Trous | `F_PUNCHHOLE` non applicable | `F_PUNCHHOLE` fonctionne : `st_blocks` 2048 devient 1536, la zone relit des zéros |
| `F_GETPATH` | `EBADF` | rend l'ancien chemin même après `unlink` (donc inutilisable pour émuler `/proc/self/fd/N` d'un memfd) |

Autres mesures : `mmap(MAP_SHARED)` de `/dev/zero` donne `ENODEV` (Linux rend de la mémoire anonyme partagée : à émuler par `MAP_ANON|MAP_SHARED`). Sur un objet `shm_open` mappé, `madvise(MADV_ZERO)` remet les octets à zéro immédiatement ; `MADV_FREE`, `MADV_DONTNEED`, `MADV_FREE_REUSABLE` et `MADV_FREE_REUSE` réussissent sans changer le contenu. Un fork d'un enfant qui hérite du descripteur (shm ou fichier détaché) mappe la même mémoire : données parent vers enfant, enfant vers parent et écriture tardive du parent sont toutes vues.

Entrée de mémoire Mach (B3) : `mach_make_memory_entry_64(MAP_MEM_NAMED_CREATE)` rend un droit d'envoi (type 0x10000), pas un descripteur. Taille arrondie à 16 Kio (5000 donne 16384), 256 Gio accepté. `mach_vm_map` de l'entrée deux fois donne deux vues qui s'alias ; une vue `copy=TRUE` faite après l'écriture reçoit le contenu et n'est pas touchée par les écritures ultérieures de la vue partagée. Au `fork`, `VM_INHERIT_SHARE` partage et `VM_INHERIT_COPY` isole l'écriture de l'enfant. Rendez-vous entre deux processus sans lien : en substituant le port d'amorçage de l'enfant avec `posix_spawnattr_setspecialport_np(TASK_BOOTSTRAP_PORT)`, l'enfant envoie un port de réception, le parent lui envoie l'entrée (et un fileport d'un descripteur `shm_open`) dans un message avec descripteur de port, l'enfant les mappe à une autre adresse et le parent voit ses écritures (les deux PASS), sans passer par `launchd`. `MAP_SHARED|MAP_ANON` n'a pas de poignée : seuls les enfants de `fork` en héritent.

Deux erreurs du programme corrigées pendant cette reprise (premier journal conservé : `f6_memfd.run1-mach-invalid-right-segv.log`) : `mach_vm_map` avec une protection maximale `VM_PROT_ALL` sur une entrée créée en lecture-écriture renvoie `KERN_INVALID_RIGHT` (17), ce qui faisait planter le test (`SIGSEGV`, code de sortie 139) ; une vue `copy=TRUE` faite avant l'écriture reçoit un instantané vide, et l'attente du test était fausse.

## G. Synchronisation et événements

a completer

## Implications for a Linux kernel emulation on Darwin

a completer (tableau fork, clone, exec, futex, epoll, memfd, SCM_RIGHTS : primitive Darwin, écart avec Linux, première expérience de qualification).

## Questions ouvertes et choses non testables

a completer
