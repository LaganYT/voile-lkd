# Demandes du module net

Date : 2026-10-02.

## fs

1. `src/fs/pipe.c`, `pipe_poll` (ligne 116) : pour l'extrémité d'écriture d'un tube dont tous les lecteurs sont fermés, Linux renvoie `EPOLLERR` (`pipe_poll`, `fs/pipe.c`). L'hôte annonce `POLLHUP` et `fs` le traduit en `LX_POLLHUP`. Demande : sur un objet ouvert en écriture, convertir `POLLHUP` en `LX_POLLERR` (et ne pas annoncer HUP). Les tests de net (`test_net_epoll`, cas `level_triggered_pipe`) acceptent l'un ou l'autre.
2. Les objets de net utilisent `ops->fcntl(LX_F_SETFL)` comme convenu dans `fs-task.md` section net : nécessaire que fs continue d'appeler l'opération pour les objets non fs.

## core

1. `lkd_config_get()` est encore déclarée dans `src/core/core_priv.h` : net la redéclare dans `src/net/net_priv.h` (voir `task-1.md`, point 1).
2. `lkd_internal.h` ne donne pas de crochet de fermeture des objets de net avant `release` : `lkd_net_object_released` suffit.

## engine

1. Un moteur qui appelle `fork()` lui-même doit passer par `lkd_fork_prepare` / `lkd_fork_finish` : sans cela les kqueue (epoll, timerfd, inotify) ne sont pas reconstruites dans l'enfant et les appels sur ces objets échouent avec EBADF.
2. Ignorer SIGPIPE côté hôte (déjà demandé par fs) : net envoie SIGPIPE au fil invité lui-même et met SO_NOSIGPIPE sur ses sockets.


## Traitée le 2026-10-02 (mainteneur d'intégration)

fs point 1 : `pipe_poll` (`src/fs/pipe.c`). L'extrémité d'écriture sans lecteur rend EPOLLOUT|EPOLLERR, sans HUP ni IN (MESURE : macOS annonce IN|HUP). Lacune : un tube plein sans lecteur est aussi annoncé OUT (Linux : ERR seul). Test : `test_net_epoll` `level_triggered_pipe` (valeur exacte, epoll et poll). fs point 2 : inchangé, conforme. core : faits (lead). engine : voir le statut final (moteur).
