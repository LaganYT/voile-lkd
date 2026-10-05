# wine-8 : aucune interface réseau visible (AF_NETLINK absent, SIOCGIFCONF -> ENOTTY) ; c'était l'arrêt du jeu après 96 images

Demandeur : agent wine-linux.

MESURE :
- Le fil principal de RocketLeague.exe (0130) s'arrête toujours après `gethostname() -> "obelisk"`, `gethostbyname("obelisk")`, puis l'ioctl nsiproxy 0x121000 (IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, ifinfo) qui ne revient jamais (WINEDEBUG +winsock, evidence/eac-runs/20261002T232124 ; Launch.log s'arrête au même moment, plus aucun Present).
- Témoin src/wine-linux/tests/hostent.c (gethostbyname du nom de la machine puis GetAdaptersAddresses), logs/wine-linux-hostent-001.log : même blocage, `nsi_ioctl ioctl 121000` puis `ifinfo_enumerate_all` sans retour.
- Cause côté Wine (corrigée) : nsiproxy appelle if_nameindex() ; glibc la construit par netlink ; LKD répond `AF_NETLINK is not implemented (-EAFNOSUPPORT)`, if_nameindex rend NULL, Wine déréférençait NULL en tenant if_list_lock, le fil mourait et tous les appels suivants restaient bloqués sur le verrou. Correctif dans dlls/nsiproxy.sys/ndis.c (NULL vérifié, repli SIOCGIFCONF + SIOCGIFINDEX). MESURE après correctif : gethostbyname rend en 22 ms (NULL : aucune adresse).
- Il reste qu'aucune interface n'existe : le repli SIOCGIFCONF rend `Inappropriate ioctl for device` (ENOTTY) sous LKD (logs/wine-linux-hostent-003.log). Sous Windows il y a toujours au moins la boucle locale et l'interface active ; gethostbyname(nom de la machine) réussit toujours, et GetAdaptersAddresses avec zéro interface fait planter iphlpapi de Wine (cas jamais vu sous Linux).
Demande, au choix :
1. NETLINK_ROUTE minimal (socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE) ; RTM_GETLINK et RTM_GETADDR en dump ; RTM_GETROUTE serait utile à nsiproxy pour les routes) : rend getifaddrs/if_nameindex de glibc et nsiproxy (iphlpapi) fonctionnels. C'est ce que font tous les programmes Linux.
2. À défaut, les ioctl SIOCGIFCONF, SIOCGIFINDEX, SIOCGIFFLAGS, SIOCGIFADDR, SIOCGIFNETMASK, SIOCGIFHWADDR, SIOCGIFMTU sur une socket AF_INET, à partir des interfaces de l'hôte (getifaddrs de macOS : lo0 -> « lo », en0 -> « eth0 » par exemple).

## Traitée le 2026-10-03 (mainteneur d'intégration)

Nouveau fichier `src/net/ifaces.c`. Tout est en lecture seule et lu dans l'état réel de l'hôte (getifaddrs, sysctl des routes) à chaque requête.

Interfaces vues par l'invité :
- `lo` (lo0 de l'hôte, index 1, MTU 65536 comme Linux) ;
- puis chaque interface hôte UP et RUNNING qui porte une adresse IPv4 ou une adresse IPv6 non locale au lien, dans l'ordre de l'hôte.
- Noms Linux : Ethernet (en*) donne `ethN`, point à point (utun de VPN) donne `tunN`, le reste donne `netN`.
- Sur ce Mac : `lo` et `eth0` (en0, 192.168.1.167, deux IPv6 globales). Les utun sans adresse globale, awdl, llw, bridge, anpi et en* inactives ne sont pas montrées.

ioctl sur toute socket : SIOCGIFCONF (IPv4, ifreq entières seulement, sonde de taille avec tampon NULL), SIOCGIFNAME, SIOCGIFINDEX, SIOCGIFFLAGS, SIOCGIFADDR, SIOCGIFNETMASK, SIOCGIFBRDADDR, SIOCGIFMTU, SIOCGIFHWADDR, SIOCGIFTXQLEN. Les SIOCSIF* rendent EPERM (pas de CAP_NET_ADMIN). Un nom inconnu rend ENODEV.

NETLINK_ROUTE (`socket(AF_NETLINK, SOCK_RAW ou SOCK_DGRAM, 0)`) :
- dumps RTM_GETLINK, RTM_GETADDR et RTM_GETROUTE, plus RTM_GETLINK d'une seule interface (par index ou IFLA_IFNAME) ;
- NLMSG_DONE et NLMSG_ERROR, avec le seq et le port id de la requête ;
- bind et autobind (port id = tgid), getsockname, adresse source {AF_NETLINK, 0} ;
- options SOL_NETLINK acceptées sans effet ;
- RTM_NEW*, RTM_DEL* et RTM_SET* rendent EPERM, les autres types EOPNOTSUPP.
- Transport : une paire AF_UNIX datagramme de l'hôte, la réponse étant écrite par LKD, un message netlink par datagramme. recv, poll, epoll, MSG_PEEK, MSG_TRUNC et O_NONBLOCK marchent sans code particulier.
- Routes : un préfixe connecté par adresse (proto kernel, scope link) et les routes par défaut de l'hôte (passerelle, sur une interface que l'invité voit).

Les autres protocoles netlink rendent EPROTONOSUPPORT.

MESURE :
- `tests/test_net_ifaces.c` (nouveau, 46 assertions) : ioctl sur lo (index 1, flags, 127.0.0.1/8, MTU 65536, ARPHRD_LOOPBACK), SIOCGIFNAME, ENODEV, EPERM, SIOCGIFCONF comparé à getifaddrs de l'hôte ; dumps netlink (lo en tête avec IFLA_IFNAME, adresses, routes), seq et port id vérifiés, NLMSG_ERROR -EPERM, EAGAIN.
- `test_net_unix` mis à jour (NETLINK_ROUTE accepté, autres protocoles EPROTONOSUPPORT).
- Témoin glibc 2.41 `witnesses/glibc/ifaces.c`, lié dynamiquement par zig (x86_64-linux-gnu), lancé dans la racine Debian sous `build-integration/host/fexdarwin-run` :
  - avant : getifaddrs et if_nameindex rendent EAFNOSUPPORT, SIOCGIFCONF rend ENOTTY, 0 interface ;
  - après : getifaddrs donne lo et eth0 (AF_PACKET, 127.0.0.1, ::1, fe80::1, 192.168.1.167, IPv6 de eth0) ; if_nameindex donne 1 lo et 2 eth0 ; SIOCGIFCONF puis SIOCGIFINDEX sont cohérents ; PASS.
- `ip` (iproute2) n'est pas dans la racine Debian : non testé.

Limites :
1. L'adresse MAC est celle que macOS donne au processus : 02:00:00:00:00:00, valeur de confidentialité. MESURE : getifaddrs et sysctl NET_RT_IFLIST2 de l'hôte rendent la même valeur pour en0, alors que `ifconfig` montre la vraie.
2. Pas de notifications multicast (changement d'adresse ou de lien).
3. Pas de RTM_GETROUTE d'une seule destination.
4. Pas de statistiques d'interface (IFLA_STATS).
5. Une socket netlink envoyée par SCM_RIGHTS arrive comme socket AF_UNIX.
