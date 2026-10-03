# Statut du module net (LKD)

Date : 2026-10-02. Jalons atteints : sockets AF_UNIX et AF_INET/AF_INET6, SCM_RIGHTS et SCM_CREDENTIALS, poll/select, epoll sur kqueue, eventfd partagé, timerfd, inotify (sous-ensemble), échange avec un processus hôte natif (besoins graphiques G4).

## Ce qui existe (src/net/, 5400 lignes, NOTICE listant l'import HL)

- `socket.c` : objet socket (descripteur hôte non bloquant, O_NONBLOCK invité dans l'objet, SO_NOSIGPIPE), socket, socketpair, bind, listen, accept, accept4, connect, getsockname, getpeername, shutdown, ioctl FIONREAD/SIOCOUTQ/SIOCATMARK, `sock_poll` (états Linux reconstruits de l'hôte).
- `sockmsg.c` : sendto, recvfrom, sendmsg, recvmsg, sendmmsg, recvmmsg ; MSG_PEEK, MSG_WAITALL, MSG_TRUNC, MSG_DONTWAIT, MSG_NOSIGNAL, MSG_CMSG_CLOEXEC, MSG_WAITFORONE ; SCM_RIGHTS dans les deux sens ; SCM_CREDENTIALS (voir plus bas).
- `sockopt.c` : getsockopt, setsockopt (SOL_SOCKET, TCP, IP, IPV6) ; SO_RCVTIMEO/SNDTIMEO tenus dans l'objet, SO_RCVBUF/SNDBUF doublés, SO_LINGER (secondes), SO_PEERCRED, SO_PASSCRED, booléens relus 0 ou 1.
- `unixaddr.c` : adresses Linux (u16 famille) et Darwin, chemins AF_UNIX par le vfs, espace abstrait par fichiers.
- `scm.c` : objets à travers SCM_RIGHTS (fichier, tube, socket, objet shm hôte, eventfd en trois descripteurs).
- `eventfd.c` : eventfd, eventfd2 (compteur en mémoire partagée MAP_SHARED, mises à jour sans verrou, tube de disponibilité).
- `timerfd.c` : timerfd_create, timerfd_settime, timerfd_gettime (expirations calculées de l'horloge, kqueue EVFILT_TIMER comme source d'attente).
- `epoll.c` : epoll_create, epoll_create1, epoll_ctl, epoll_wait, epoll_pwait, epoll_pwait2 ; epoll_ctl_old et epoll_wait_old rendent ENOSYS comme Linux.
- `inotify.c` : inotify_init, inotify_init1, inotify_add_watch, inotify_rm_watch sur EVFILT_VNODE.
- `wait.c` : poll, ppoll, select, pselect6, `lkd_net_wait_object`, `lkd_net_make_notify_pipe`, restart de poll.
- `net_init.c` : init, verrou du module (rang 71), crochets de fork qui reconstruisent les kqueue (timerfd et inotify d'abord, epoll ensuite), `lkd_net_object_released`.

Syscalls enregistrés (45) : socket socketpair bind listen accept accept4 connect getsockname getpeername sendto recvfrom sendmsg recvmsg sendmmsg recvmmsg shutdown setsockopt getsockopt poll ppoll select pselect6 epoll_create epoll_create1 epoll_ctl epoll_wait epoll_pwait epoll_pwait2 epoll_ctl_old epoll_wait_old eventfd eventfd2 timerfd_create timerfd_settime timerfd_gettime inotify_init inotify_init1 inotify_add_watch inotify_rm_watch (`tools/owners_check.py` : 0 problème). Tous les gestionnaires sont `static`.

## Tests (MESURE, construction privée cœur + fs + mm + sig + task + net, chaque programme lancé 3 fois sans échec)

| Programme | Assertions | Cas | Contenu |
| --- | --- | --- | --- |
| `test_net_unix` | 233 | 15 | AF_UNIX flux et datagramme, bind/listen/accept/connect, chemins longs, espace abstrait et autobind, SCM_RIGHTS (tube, fichier, socket, eventfd, troncature MSG_CTRUNC, entre deux processus), SCM_CREDENTIALS, options, poll, lecture bloquante et interruption, SIGPIPE |
| `test_net_epoll` | 352 | 15 | niveau, front, oneshot, retrait par fermeture, sockets, écoute, eventfd, attente bloquante et interrompue, epoll_pwait, imbrication et ELOOP, reconstruction après fork, 100 descripteurs |
| `test_net_inet` | 147 | 10 | TCP et UDP sur boucle locale, connect non bloquant, refus, SO_ERROR, options TCP, IPv6 ::1, sendmmsg/recvmmsg, échange de type DNS, poll de TCP |
| `test_net_events` | 199 | 16 | poll/ppoll/select/pselect6, eventfd (compteur, sémaphore, bloquant, partagé par fork), timerfd (une fois, périodique, absolu, epoll, fork), inotify |
| `test_net_host` | 48 | 2 | un fil hôte natif joue wsd/vtest : connexion invité vers un chemin lié à un répertoire hôte, `getpeereid` voit l'uid du runtime, objet shm hôte reçu puis `mmap(MAP_SHARED)` par mm avec écritures cohérentes dans les deux sens, tube de fence reçu et attendu par poll et epoll, tube invité envoyé à l'hôte |
| total | 979 | 58 | |

## Faits hôtes mesurés (macOS 27.0.1, ce Mac)

- AF_UNIX SOCK_SEQPACKET : `EPROTONOSUPPORT` sur socket et socketpair. LKD rend `ESOCKTNOSUPPORT` visible.
- `sun_path` fait 104 octets (Linux : 108). Un chemin hôte plus long est atteint par un descripteur de répertoire et la feuille, avec `pthread_fchdir_np` pour le fil seulement.
- Un socket de flux neuf, un socket après `shutdown(SHUT_WR)` d'un côté, un pair fermé : l'hôte annonce POLLHUP là où Linux annonce OUT, RDHUP ou rien (`tcp_poll`, `unix_poll`). `sock_poll` reconstruit les bits Linux à partir de la sonde hôte (send de 0 octet : EPIPE, ENOTCONN ; recv MSG_PEEK : 0 pour EOF).
- Un connect TCP refusé : `SO_ERROR` vaut `ECONNREFUSED` et le socket est HUP|ERR ; un deuxième connect donne `EINVAL` sur l'hôte.
- `connect(AF_UNSPEC)` sur UDP donne `EAFNOSUPPORT` mais dissout l'association ; sur TCP c'est refusé.
- Un descripteur kqueue ne peut pas partir par SCM_RIGHTS (EINVAL) : epoll, timerfd et inotify rendent `EOPNOTSUPP` à l'envoi.
- Un objet `shm_open` reçu n'a pas de bits de type de fichier dans `st_mode` : c'est ainsi que `scm.c` le reconnaît (et reconnaît l'eventfd par sa signature de 8 octets).
- kqueue sur un kqueue : lisible quand un événement est prêt (sert de source d'attente pour timerfd, inotify et epoll imbriqué).
- Un descripteur de socket reçu partage le drapeau O_NONBLOCK de l'émetteur : LKD passe alors `MSG_DONTWAIT` à chaque appel au lieu de changer la description partagée.

## Contrat avec Wine et glibc

- Sockets de Wine : le serveur et les clients utilisent socketpair AF_UNIX, SCM_RIGHTS (descripteurs de fichier, tubes), epoll, eventfd et poll ; tout cela est couvert par les tests. Le chemin AF_UNIX nommé et l'abstrait fonctionnent.
- AF_NETLINK : `socket(AF_NETLINK)` rend `EAFNOSUPPORT` visible (log une fois). glibc 2.41 : `getaddrinfo` avec `AI_ADDRCONFIG` appelle `__check_pf` qui ouvre une socket NETLINK_ROUTE ; en cas d'échec il suppose IPv4 et IPv6 présents (comportement de `check_pf.c`, LU, non mesuré ici) ; `getifaddrs` échoue (`ENOSYS`/`EAFNOSUPPORT`). Un NETLINK_ROUTE minimal reste à écrire si un témoin réel le réclame.
- DNS : glibc lit `/etc/resolv.conf`, `/etc/hosts`, `nsswitch.conf` dans la racine invitée (le fs les sert) et parle en UDP/TCP AF_INET au serveur. Testé : socket UDP non bloquant, connect, send, poll, recv, SO_ERROR. Stratégie : l'image du runtime fournit `resolv.conf` avec une adresse de serveur joignable depuis l'hôte (par exemple celle de `scutil --dns` ou 1.1.1.1) ; il n'y a pas de résolveur émulé. Les réponses ICMP d'erreur sur UDP (port fermé) ne sont pas toujours rapportées par l'hôte : l'appelant retombe sur la temporisation.
- SCM_CREDENTIALS : à la réception avec SO_PASSCRED, LKD ajoute les identifiants construits de `LOCAL_PEERPID` et `LOCAL_PEERCRED` du pair (état réel, pid hôte traduit en pid invité par le registre). À l'envoi, LKD vérifie que l'émetteur ne se prétend pas un autre (comme `scm_check_creds`) puis n'envoie rien : le récepteur obtient la même information de l'hôte. Pas de crédentiels pour un pair hors de la famille (pid 0, non rapporté).

## Besoins graphiques (G4) : état

1. Chemin AF_UNIX invité vers une socket hôte : ATTEINT (lié par `lkd_vfs_mount_bind`, test `test_net_host`). `getpeereid` côté hôte voit l'uid du compte du runtime.
2. SCM_RIGHTS dans les deux sens : ATTEINT. Un objet shm hôte devient un objet invité `host-shm` (taille et `fstat` hôtes, `read`/`write` refusés par ENXIO comme l'hôte, `mmap_fd` pour mm). Un tube hôte devient un objet fs ; un descripteur invité part en descripteur hôte dupliqué.
3. poll et epoll sur ces sockets et sur les tubes reçus : ATTEINT.
4. Test MAP_SHARED cohérent des deux côtés : ATTEINT (écritures du processus hôte vues par l'invité, et l'inverse, deux projections du même objet qui s'alias).

## Écarts connus (honnêtes)

1. AF_NETLINK, AF_PACKET, SOCK_RAW : refusés (EAFNOSUPPORT, EPERM).
2. SOCK_SEQPACKET AF_UNIX : ESOCKTNOSUPPORT.
3. Les noms de socket AF_UNIX renvoyés par `getpeername` d'un client sont ceux donnés au connect (mémorisés) ; `accept` et `recvfrom` retraduisent le chemin hôte vers un chemin invité par la table de montages (un chemin hors de la vue invitée est rendu tel quel).
4. SO_PEERSEC, SO_PEERGROUPS, SO_BINDTODEVICE, SO_TIMESTAMP*, IP_PKTINFO, MSG_ERRQUEUE : ENOPROTOOPT ou non pris en charge (log une fois).
5. Épuisement de la file d'attente des sockets : les tampons sont ceux de l'hôte.
6. epoll : EPOLLEXCLUSIVE est validé comme Linux puis traité comme une inscription normale (tous les ensembles se réveillent) ; EPOLLWAKEUP ignoré ; fichiers ordinaires, répertoires et memfd refusés par EPERM comme Linux. Un ensemble epoll n'est pas partagé après fork : l'enfant reconstruit sa copie (Linux partage l'ensemble), les deux ensembles divergent ensuite. Un epoll partagé entre processus par SCM_RIGHTS est impossible (EOPNOTSUPP). Front (EPOLLET) : les objets fondés sur un tube de disponibilité (eventfd, signalfd) signalent un front à chaque écriture.
7. timerfd : TFD_TIMER_CANCEL_ON_SET accepté mais ECANCELED n'arrive jamais (LKD ne voit pas un saut d'horloge) ; `_ALARM` : EPERM ; l'état est copié (pas partagé) après fork.
8. inotify : IN_ACCESS, IN_OPEN, IN_CLOSE_* jamais livrés ; les événements des enfants d'un répertoire demandent un descripteur par fichier régulier (1024 au plus) ; un rafale de créations et de modifications peut regrouper l'ordre ; un renommage qui sort du répertoire est vu comme DELETE et une entrée comme CREATE ; pas de récursion ; l'instance n'est pas partagée par fork (copie reconstruite).
9. Tube : l'extrémité d'écriture dont le lecteur est fermé est annoncée HUP par `fs_pipe_ops.poll` (Linux : EPOLLERR) ; demande dans `docs/requests/net-1.md`.
10. Les crochets de fork sont ceux de lkd_fork_host : un moteur qui forke lui-même doit appeler `lkd_fork_prepare` et `lkd_fork_finish` pour que les kqueue soient reconstruites.
11. Un appel `epoll_wait` dont les éléments ont une source d'attente qui ne peut pas signaler tous les événements demandés (EPOLLOUT sur un objet à tube de disponibilité) ne les voit qu'à l'entrée de l'appel.

## Suite possible

NETLINK_ROUTE minimal (RTM_GETADDR, RTM_GETLINK) si un témoin glibc le réclame ; IP_PKTINFO ; partage d'un epoll entre processus via famsvc ; test réel de `wineserver` sur LKD.

## Interfaces et NETLINK_ROUTE (2026-10-03, demande wine-8)

`src/net/ifaces.c` : ioctl SIOCGIF* et socket NETLINK_ROUTE (dumps de liens, d'adresses et de routes) depuis l'état réel de l'hôte, en lecture seule. L'écart 1 « AF_NETLINK refusé » ne vaut plus pour NETLINK_ROUTE. Détail et limites : `docs/requests/wine-8.md`.
