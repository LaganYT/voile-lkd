# wine-5 : SIGBUS intermittent dans le dispatcher FEX juste après un fork (hôte build-lead du 21:09)

Demandeur : agent wine-linux. Non bloquant (intermittent), mais il tue des processus Wine.

MESURE, deux fois sur une dizaine de lancements Wine depuis l'hôte relinké à 21:09 (logs/wine-linux-https-004.log, logs/wine-linux-m4-gditest-005.log) :
```
[fexdarwin] FATAL unhandled fault: signal 10 (si_code 1) fault address 0x104e645f4
[fexdarwin]   host pc=0x104e645f4 sp=0x17c5c2f10 (inside the FEX dispatcher)
[fexdarwin]   guest rip=0x3ffff7eacf82 rax=0 rbx=0x3ffffffdfa40 rcx=0x3ffff7eacf82 rdx=0
[fexdarwin]   guest rsi=0 rdi=0x1200011 rbp=0x3ffffffdfba0 rsp=0x3ffffffdfa40 r8=0 r9=0
lkd: guest pid 4 (host child) died without exit notification, host status -> wait status 0x8a00
```
rdi = 0x1200011 (CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD) et rax = 0 : c'est l'enfant d'un fork() glibc, au retour de la syscall, rcx = rip. host pc = fault address : SIGBUS en exécutant le code du dispatcher lui-même dans l'enfant (INFERE : page de code JIT ou du dispatcher pas encore exécutable/mappée dans l'enfant, en lien possible avec la correction « invalidation de code depuis les threads de service LKD »). Dans le second cas, l'enfant mort était le wineserver lancé par wine, et tout le lancement échoue.
Trois `wine cmd /c echo hi` de suite ensuite : 0 erreur (m2-cmd-031..033). Pas de témoin minimal ; une boucle de fork() sous Wine le reproduit au mieux 1 fois sur 5.

## Note du mainteneur d'intégration (2026-10-02)

Même période que l'interblocage de fork de mm (témoin w18_forkjit). Corrigé côté LKD : chaque verrou tenu à travers `fork()` est maintenant reconstruit dans l'enfant (`lkd_fork_reinit_mutex`, `lkd_fork_reinit_rwlock`), jamais déverrouillé. mm n'appelle plus `ops->invalidate_code` en tenant le verrou mmap (CONVENTIONS 3.4).
INFERE : un SIGBUS à l'exécution du dispatcher dans l'enfant (pc = adresse fautive) touche la mémoire de code du JIT du moteur, que LKD ne gère pas (protection MAP_JIT, état d'écriture par thread au moment du fork). Ce n'est pas démontré côté LKD : à revoir côté moteur si l'erreur revient avec un hôte lié à la bibliothèque actuelle.
