# Service IPC de famille (famsvc) : annonce et mode d'emploi

Demandeur : sig. Destinataires : mm, procfs, fs, ptrace, task, chef. Date : 2026-10-02.

Décision du chef : un seul service pour les quatre besoins inter-processus. Il est livré par le module sig dans `src/sig/famsvc.c`, contrat dans `src/include/famsvc.h` (nouveau fichier interne, possédé par sig, créé avec l'accord du chef).

## Ce que fait le service (LU dans le code, MESURE dans les tests)

- Chaque processus hôte de la famille a un thread de service hôte (pas un thread invité, `lkd_current()` vaut NULL, tous les signaux hôtes bloqués) qui écoute sur une socket AF_UNIX stream `<dir>/<pid hôte>.s`, dans un répertoire de famille en 0700 (`LKD_FAMSVC_DIR`, créé par `mkdtemp` au premier processus). Le pair est vérifié par `getpeereid` (même uid) et identifié par `LOCAL_PEERPID`.
- Appel synchrone avec délai : `lkd_famsvc_call(host_pid, &req, &resp, timeout_ms)` ou `lkd_famsvc_call_pid(guest_pid, ...)` (pid hôte pris dans le registre par `lkd_proc_lookup`). Charge utile jusqu'à 8 Mio dans chaque sens, jusqu'à 16 descripteurs par `SCM_RIGHTS` (reçus avec `FD_CLOEXEC`).
- Erreurs de transport : `-LX_ESRCH` (aucun service : processus parti), `-LX_ETIMEDOUT`, `-LX_EPROTO`, `-LX_E2BIG`, `-LX_EINVAL` (appel depuis le thread de service, opcode hors plage). Un opcode sans gestionnaire répond `-LX_ENOSYS`.
- fork : `lkd_sig_fork_child` ferme la socket héritée et les connexions, et démarre un service pour le nouveau pid hôte. exec hôte : la socket d'écoute n'est pas close-on-exec et `LKD_FAMSVC_FD="<fd>:<pid>"` la désigne ; la nouvelle image l'adopte (MESURE : `test_sig_famsvc` cas `exec_keeps_the_service`, ping servi par l'image ré-exécutée, compteur `adopted == 1`).
- Arrêt : `lkd_sig_proc_exit` (dernier processus invité du processus hôte) appelle `lkd_famsvc_shutdown`, qui supprime la socket et, s'il ne reste aucun membre vivant, le répertoire. Les répertoires `lkdfam.*` de plus d'une minute sans membre vivant (exécutions plantées) sont supprimés à la création d'une nouvelle famille.
- MESURE (`test_sig_famsvc`, 2000 appels PING de 4 octets, M5 Max, macOS 27) : aller-retour vers un autre processus p50 10,3 us, p90 13,9 us, p99 22 us ; vers soi p50 11,6 us.

## Opcodes réservés (ABI fixe entre processus d'une famille)

| Plage | Module | Déjà défini |
| --- | --- | --- |
| 1 | famsvc | PING (écho, statut = pid hôte) |
| 16..31 | sig | QUEUE_SIGNAL (16), CHILD_EVENT (17), SIG_STATUS (18) |
| 32..47 | mm | READ_MEM (32), WRITE_MEM (33), RENDER_MAPS (34) : à enregistrer par mm |
| 48..63 | procfs, fs | LIST_FDS (48), TASK_STATUS (49) : à enregistrer par procfs ou fs |
| 64..95 | ptrace | libre pour le transport traceur/tracee |
| 96..111 | task | libre |
| 120..127 | tests | réservé aux tests unitaires |

## Règles pour un gestionnaire

1. Enregistrer dans `lkd_<module>_init` : `lkd_famsvc_register(LKD_FAMSVC_OP_READ_MEM, handler)`.
2. Le gestionnaire tourne sur le thread de service, une requête à la fois par processus, sans verrou LKD tenu. Il répond vite, ne fait pas d'attente bloquante sur un thread invité, ne prend pas un verrou qu'un thread invité garde pendant une attente, et n'appelle jamais `lkd_famsvc_call` (deux services qui s'appellent finiraient au délai ; l'appel est refusé avec `-LX_EINVAL`).
3. Il lit `req->data/len`, garde un descripteur reçu en mettant `req->fds[i] = -1` (les autres sont fermés au retour), remplit `resp->status`, `resp->data` (malloc, libéré par famsvc) et `resp->fds` (fermés par famsvc après l'envoi).
4. Côté appelant, libérer la réponse avec `lkd_famsvc_msg_free(&resp)`.

Exemple pour mm (INFERE, à adapter) : `READ_MEM` reçoit `{uint64 gva; uint64 len}`, lit avec la récupération de faute de l'image locale, répond les octets lus (réponse courte au premier octet non mappé) ; `WRITE_MEM` appelle `ops->invalidate_code` dans le processus cible avant de répondre.

## Ce que le moteur doit garder

`LKD_FAMSVC_DIR` et `LKD_FAMSVC_FD` dans l'environnement de la ré-exécution hôte, et le descripteur nommé par `LKD_FAMSVC_FD` ouvert (pas de `closefrom` avant l'exec). Le thread de service n'a pas de `GuestThread` : le gestionnaire de kick du moteur doit l'ignorer (`LkdGlue.cpp` `KickHandler` le fait déjà en sortant si `CurrentGuestThread` est NULL ; de toute façon SIGUSR2 y est bloqué).


## Traitée le 2026-10-02 (mainteneur d'intégration)

Annonce. task utilise maintenant l'opcode 96 (`TASK_OP_WAKE_WAITERS`, `src/task/tasks.c`).
