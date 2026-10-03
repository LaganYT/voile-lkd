# wine-4 : setsockopt(IPPROTO_IP, IP_RECVERR) -> ENOPROTOOPT casse toute résolution DNS de glibc

Demandeur : agent wine-linux. Bloquant pour HTTPS (EOS, bootstrapper EAC).

MESURE (logs/wine-linux-dns-001.log, LKD_TRACE=1) : `getent hosts www.epicgames.com` du rootfs Debian, code 2 (non trouvé). Séquence du résolveur glibc 2.41 (res_send, send_dg) pour chaque serveur de resolv.conf :
```
socket(AF_INET, SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0) = 3
setsockopt(3, IPPROTO_IP(0), IP_RECVERR(11), &1, 4) = -92 (ENOPROTOOPT)
close(3)
```
puis exit_group(2) : aucun paquet DNS n'est envoyé. glibc traite l'échec de IP_RECVERR comme fatal pour ce serveur. Côté Windows (Wine, WinHTTP) le même échec donne ERROR_WINHTTP_NAME_NOT_RESOLVED (12007), logs/wine-linux-https-001.log.
Demande : accepter IP_RECVERR (et IPV6_RECVERR, niveau 41 option 25, même code dans send_dg pour un serveur IPv6) sur les sockets UDP. Le minimum est de répondre 0 sans effet (les erreurs ICMP ne seront pas remontées par MSG_ERRQUEUE), ce qui suffit au résolveur.
Témoin : `run/wine-lkd-own.sh 30 /usr/bin/getent hosts www.epicgames.com` doit afficher une adresse.

## Traitée le 2026-10-02 (mainteneur d'intégration)

`IP_RECVERR` (niveau IPPROTO_IP, 11) et `IPV6_RECVERR` (niveau IPPROTO_IPV6, 25) sont acceptés sur les sockets AF_INET et AF_INET6 (`src/net/sockopt.c`). Le drapeau est mémorisé et relu par getsockopt ; le retirer vide la file. Une socket AF_UNIX garde ENOPROTOOPT.
File d'erreurs (`src/net/sockmsg.c`) : une erreur que l'hôte rapporte pour un datagramme (ECONNREFUSED, EHOSTUNREACH, ENETUNREACH, EHOSTDOWN) est aussi mise en file. `recvmsg(MSG_ERRQUEUE)` rend EAGAIN quand la file est vide, sinon 0 octet, l'adresse du pair, `MSG_ERRQUEUE` et un message de contrôle IP_RECVERR (`sock_extended_err` avec `SO_EE_ORIGIN_LOCAL`, type, code et émetteur ICMP inconnus : l'hôte ne donne que l'errno). poll annonce POLLERR tant que la file n'est pas vide.
Limites : une seule entrée en file (Linux en garde plusieurs), aucune charge utile du datagramme en échec.
MESURE : `test_net_inet` cas `ip_recverr` (port fermé sur la boucle locale : ECONNREFUSED puis l'entrée de file, IPv6, AF_UNIX). Témoin : `getent hosts www.epicgames.com` sous `build-integration/host/fexdarwin-run` (racine Debian) affiche 104.18.21.94 et 104.18.20.94, code 0. Avant : code 2 et `setsockopt level 0 option 11 not implemented`.
