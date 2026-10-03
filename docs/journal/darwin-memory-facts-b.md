# Faits macOS sur le JIT, les fautes et les signaux (groupes C et D)

Dernière mise à jour : 2026-10-02 (brouillon 3 : parties C et D mesurées sauf SMC et faute dans le handler). Les sections marquées `a completer` n'ont pas encore de mesure.

Ce rapport sépare ce qui a été MESURE (programmes C écrits et exécutés sur cette machine, journaux dans `evidence/darwin-memory/`) de ce qui est LU (code source, en-têtes du SDK) et de ce qui est DEDUIT. Racine de travail : `build/experiments/rocket-wine-fex-darwin-20261002`, notée `$EXP`.

## Machine de mesure

Mesuré : macOS 27.0.1 (build 26A434), noyau `xnu-13432.1.9~1/RELEASE_ARM64_T6050`, Apple M5 Max, 18 coeurs logiques, pages d'hôte de 16 Kio (`hw.pagesize` = 16384), 36 Gio de mémoire, SIP actif, Apple clang 21.0.0, SDK macOS 27.0. Aucune variable `boot-args` n'est lisible.

## Etat d'avancement

| Élément | Etat |
| --- | --- |
| `witnesses/darwin-memory/src/common.h` laissé par la tentative précédente | inspecté, réutilisé (copie dans `cd_common.h`) |
| Aucun fichier `c_*` ou `d_*` n'existait au démarrage de cette reprise | constaté |
| Programmes C (JIT) : `c_jit_basic.c`, `c_jit_threads.c`, `c_jit_cost.c` | écrits, exécutés (variante `plain`), toutes les variantes à relancer |
| Programmes D (fautes et signaux) | a completer |
| Scripts `build-cd.sh` et `run-cd.sh` | écrits |

## C. JIT

Mesures du brouillon 2 (variante `plain` : signature ad hoc sans runtime durci ni droit). Les journaux complets sont `evidence/darwin-memory/c_jit_basic.<variante>.log`, `c_jit_threads.<variante>.log` et `c_jit_cost.<variante>.log`. Détail à compléter dans la version finale.

### C.1 MAP_JIT et bascule d'écriture par thread (mesuré)

- Un binaire signé ad hoc sans droit particulier et sans runtime durci obtient `MAP_JIT` (`c_jit_basic.plain`, test `C02.mapjit`). Avec le runtime durci (`-o runtime`) et sans le droit `com.apple.security.cs.allow-jit`, `mmap(MAP_JIT)` échoue avec `EINVAL` (variante `rt`). Avec le runtime durci et `allow-jit` (variante `rtjit`), il réussit.
- `MAP_JIT|MAP_SHARED` et `MAP_JIT` sans `MAP_ANON` donnent `EINVAL`. `MAP_JIT|MAP_FIXED` donne `EINVAL`. Une région `MAP_JIT` de 4 Kio est arrondie à 16 Kio.
- L'état par défaut de chaque thread (le principal et tout thread créé, que le parent soit en mode écriture ou non) est le mode exécution : une écriture dans la page `MAP_JIT` lève `SIGBUS` (`si_code` = `BUS_ADRALN`, ESR EC=0x24, DFSC=0x0f, faute de permission), une lecture et un appel réussissent (`c_jit_threads` T01).
- `pthread_jit_write_protect_np(0)` : écriture et lecture possibles, l'exécution lève `SIGBUS` avec `si_addr` = `pc`. `pthread_jit_write_protect_np(1)` : lecture et exécution possibles, l'écriture lève `SIGBUS`. L'état est propre au thread appelant (T01.isolation, T02, T05).
- Coût d'une bascule : 11,6 ns par appel (11,7 ns même si l'état ne change pas), 23 ns pour une paire. `pthread_jit_write_with_callback_np` coûte 22,7 ns. Une retouche complète (bascule, un store, bascule) coûte 23 ns, et 102 ns avec `sys_icache_invalidate` de 4 octets (`c_jit_cost` K01, K02, K04).

### C.2 Cohérence du cache d'instructions (mesuré)

- Sans invalidation, un thread qui réécrit un mot d'instruction puis l'appelle exécute l'ancienne version 199 900 fois sur 200 000. `dsb ish; isb` seul et `dc cvau` seul ne suffisent pas (même taux). `ic ivau` suivi de `dsb ish; isb` suffit, sans `dc cvau`. `sys_icache_invalidate` et `__builtin___clear_cache` suffisent (0 exécution périmée) (`C03.same-thread`).
- Entre threads : un écrivain qui fait bascule, écriture, bascule, `sys_icache_invalidate`, puis publie par un store release ne produit aucune exécution périmée chez trois lecteurs sans `isb` (T02, 60 000 réécritures par configuration, lignes de 128 octets partagées entre le code exécuté et le code réécrit). Sans invalidation : 69 149 exécutions périmées sur 69 250.
- Patch d'un `b` en place pendant qu'un autre thread boucle : avec invalidation le lecteur sort de la boucle 125 ns (médiane) après le store, 250 ns au p99 (T03). Sans invalidation, la médiane est de 6,8 ms et 9 essais sur 300 n'ont pas quitté la boucle en 30 ms.
- Un mot d'instruction alterné 11 millions de fois entre deux valeurs est lu entier par deux lecteurs concurrents (11,9 millions d'exécutions, 0 valeur intermédiaire, 0 faute) (T06).
- Un pointeur littéral `ldr x16, lit; blr x16` retouché par un store 64 bits aligné en mode écriture est vu par le lecteur en 42 ns (médiane), sans maintenance de cache (T04).
- Un thread qui bascule et écrit en continu ne ralentit pas quatre threads qui exécutent du code (0,72 ns par appel avec ou sans écrivain, même page de 16 Kio comprise) (T05).
- `CTR_EL0` n'est pas lisible depuis l'espace utilisateur (`SIGILL`) ; `hw.cachelinesize` vaut 128, L1I 128 Kio, L1D 64 Kio. `sys_icache_invalidate` coûte 82 ns jusqu'à 128 octets, 652 ns pour 16 Kio, 35 µs pour 1 Mio (environ 25 à 30 Go/s) (K03). Elle fonctionne en mode écriture comme en mode exécution, et lève `SIGSEGV` sur une plage non mappée.

### C.3 Mappages RWX sans MAP_JIT (mesuré)

- `mmap(RWX)` anonyme sans `MAP_JIT` : `EACCES` dans toutes les variantes. `mprotect(RWX)` sur n'importe quel mappage anonyme : `EACCES`.
- Variante `plain` (sans runtime durci) : `mmap(RW)`, écriture, `mprotect(RX)`, appel : réussit. Variante `rtjit` (runtime durci + `allow-jit`) : le même enchaînement est tué par `SIGKILL` à l'exécution. Variante `rtjitx` (`allow-jit` + `allow-unsigned-executable-memory`) : réussit.
- Fichier non signé mappé `PROT_EXEC` : `EPERM`.
- La paire `mprotect(RX)` + `mprotect(RW)` coûte 540 ns pour 16 Kio, 650 ns pour 128 Kio, 1,6 µs pour 1 Mio, 680 ns pour 16 Kio avec 8 threads occupés, et agit sur tout le processus (K05).

### C.4 Tailles et nombre de régions (mesuré)

- Une réservation `MAP_JIT` RWX unique de 1 Gio, 4 Gio, 64 Gio, 1 Tio, 127 Tio réussit en 1 à 3 µs sans toucher de page ; 128 Tio donne `ENOMEM`. Les pages première et dernière sont utilisables.
- 100 000 régions `MAP_JIT` de 16 Kio (0,5 µs chacune, aucune fusion des entrées VM), 20 000 de 1 Mio et 4 096 de 1 Gio ont été créées sans échec (limite fixée par le test, pas par le noyau).
- Une région `MAP_JIT` créée avec la protection `---`, `r--`, `rw-` ou `r-x` peut être promue par `mprotect` vers n'importe quelle protection, dont RWX. Une fois RWX (créée ainsi ou promue), toute nouvelle `mprotect` donne `EACCES` : la promotion est à sens unique. Une région créée RWX refuse tout `mprotect`, y compris vers RWX. `mach_vm_protect(set_maximum)` donne `KERN_PROTECTION_FAILURE`.
- Un indice d'adresse est respecté pour `MAP_JIT` comme pour `mmap` ordinaire quand l'adresse est libre (0x120000000, 0x7000000000, 0x10000000000 respectés ; les adresses occupées sont déplacées).
- `MAP_FIXED|MAP_ANON` sans `MAP_JIT` par-dessus des pages `MAP_JIT` réussit, mais les pages perdent définitivement le statut JIT (`mprotect(RWX)` : `EACCES`, `MAP_FIXED|MAP_JIT` : `EINVAL`). `munmap` d'un trou puis `mmap(MAP_JIT)` à la même adresse (indice) réussit. `mach_vm_remap` d'une région `MAP_JIT` donne `KERN_PROTECTION_FAILURE` : pas de double mappage.
- `madvise` sur pages `MAP_JIT` : `MADV_ZERO` donne `EPERM` (il réussit sur de la mémoire anonyme ordinaire), `MADV_DONTNEED`, `MADV_FREE`, `MADV_FREE_REUSABLE` retournent 0 sans remettre à zéro (le contenu 0xa5 reste lisible), `MADV_PAGEOUT` donne `ENOTSUP`.
- Coût de la première touche d'une page `MAP_JIT` fraîche de 16 Kio (écriture, invalidation, premier appel) : 2,26 µs, contre 259 ns pour une page déjà résidente et 615 ns pour une page anonyme ordinaire (K06). `munmap` d'une réservation `MAP_JIT` non touchée : 1,8 µs pour 4 Mio, 4,0 µs pour 1 Gio.

### C.5 Transferts de contrôle (mesuré)

- Le code JIT appelle une fonction C de l'hôte par `ldr x16, lit; blr x16` et revient ; `bl` entre deux régions JIT voisines (16 Kio d'écart) fonctionne ; `ldr x16, lit; br x16` vers une région à 443 Gio fonctionne ; 1 000 appels hôte vers JIT via pointeur de fonction fonctionnent.
- Un `blr` vers une adresse JIT sans instruction `bti` d'arrivée s'exécute sans faute (FEAT_BTI est présent, mais non appliqué aux pages `MAP_JIT` d'un binaire arm64 simple).
- Un appel indirect d'un stub JIT coûte 0,48 à 0,56 ns, contre 0,47 ns pour une fonction hôte (K08).

## D. Fautes et signaux

Mesures du brouillon 3 (variante `plain`), journaux `evidence/darwin-memory/d_*.plain.log`. Les volets "coût d'un aller-retour de faute" (SMC) et "faute dans le handler" sont à compléter.

### D.1 Classification des fautes (mesuré, `d_faults`)

- Page non mappée (lecture, écriture, saut) et adresses non canoniques : `SIGSEGV`, `si_code` = `SEGV_ACCERR` (2) et jamais `SEGV_MAPERR`, `si_addr` = FAR = adresse fautive, ESR EC=0x24 (donnée) ou 0x20 (instruction), DFSC=0x04 à 0x07 (translation).
- Page mappée mais `PROT_NONE`, lecture seule en écriture, `PROT_READ|PROT_WRITE` en exécution, MAP_JIT en mode exécution (écriture) ou en mode écriture (appel), fichier au-delà de l'EOF, fichier partagé en lecture seule en écriture : `SIGBUS`, `si_code` = `BUS_ADRALN` (1) dans tous les cas, `si_addr` = FAR. L'ESR distingue : DFSC=0x0f (permission) pour une page protégée en écriture ou non exécutable, DFSC=0x07 (translation) pour `PROT_NONE` et le fichier tronqué.
- Donc sur Darwin : non mappé = `SIGSEGV`, mappé avec protection insuffisante = `SIGBUS`. Le `si_code` ne permet pas de distinguer les cas ; l'ESR du contexte (`uc_mcontext->__es.__esr`) le permet. Le champ `__exception` du contexte vaut toujours 0.
- Accès non alignés : `ldr`, `ldp`, `ldr q`, `ldar` et les exclusifs 32 et 64 bits à l'intérieur d'une fenêtre alignée de 16 octets ne lèvent aucune faute. Un `ldar`, `stlr`, `ldaxr`, `ldadd`, `swp`, `cas` qui franchit une frontière de 16 octets lève `SIGBUS`/`BUS_ADRALN`, ESR DFSC=0x21 (alignement), `si_addr` = adresse de début de l'accès. FEAT_LSE2 est présent (`hw.optional.arm.FEAT_LSE2` = 1).
- Un `pc` non aligné (2 mod 4) : `SIGBUS`, ESR EC=0x22, `si_addr` = cible. Un `sp` non aligné utilisé comme base : `SIGBUS`, ESR EC=0x26, `si_addr` = 0.
- Accès à cheval sur une frontière de page dont la seconde page est inaccessible : l'adresse de faute (`si_addr` et FAR) est la première adresse de la seconde page, pas le début de l'accès ; aucune écriture partielle n'a eu lieu (les 4 octets de la première page sont restés intacts dans les trois variantes).
- `brk #0` : `SIGTRAP`, `si_code` = 0, `si_addr` = pc de l'instruction, ESR EC=0x3c. `udf`, `mrs ttbr0_el1`, `hvc`, `msr daifset` : `SIGILL`/`ILL_ILLTRP` (2), ESR EC=0x00 sauf `msr` (EC=0x18).
- Les bits d'activation des exceptions flottantes de FPCR (IOE, DZE, OFE, UFE, IXE, IDE) se relisent tels qu'écrits et les exceptions sont réellement levées : `1.0/0.0` avec DZE donne `SIGILL`/`ILL_ILLTRP`, ESR EC=0x2c, `si_addr` = instruction fautive. La division entière par zéro ne lève rien (résultat 0).
- Top-byte-ignore est actif pour les accès de données : un pointeur dont l'octet haut vaut 0xab accède à la même page.
- Débordement de pile : thread principal sans pile alternative, mort par `SIGSEGV` ; thread `pthread` sans pile alternative, mort par `SIGILL` (le noyau ne peut pas construire le cadre). Avec `sigaltstack` et `SA_ONSTACK` le handler s'exécute : `SIGSEGV` (`SEGV_ACCERR`) sur le thread principal, `SIGBUS` (`BUS_ADRALN`) sur un thread `pthread` (page de garde mappée `PROT_NONE`).

### D.2 Contexte arm64 (mesuré, `d_context`)

- `sizeof(siginfo_t)` = 104, `ucontext_t` = 56 octets (champs `uc_onstack` 0, `uc_sigmask` 4, `uc_stack` 8, `uc_link` 32, `uc_mcsize` 40, `uc_mcontext` 48), le `mcontext` de 816 octets suit immédiatement (`uc_mcontext` pointe à `uc + 56`). `sigset_t` fait 4 octets et `NSIG` vaut 32.
- Dans `struct __darwin_mcontext64` : `__es` (16 octets : `__far` 0, `__esr` 8, `__exception` 12), `__ss` à 16 (x0 à 16, x28 à 240, fp 248, lr 256, sp 264, pc 272, cpsr 280, 272 octets), `__ns` à 288 (v0 288, v31 784, fpsr 800, fpcr 804, 528 octets).
- Lecture : le handler voit exactement x0 à x28, fp, lr, sp, v0 à v31, NZCV, fpsr et fpcr posés avant un `brk`, sur la pile du thread comme sur une pile alternative. Écriture : x0 à x28, fp, lr, v0 à v31, NZCV, fpsr, fpcr, sp et pc modifiés par le handler sont ceux que le code repris observe après `sigreturn` (sp changé vers une autre pile, pc vers une autre fonction).
- Exception : **x18 est remis à 0 par le noyau**. La valeur écrite dans le contexte n'est pas restaurée (relu à 0 après `sigreturn`) ; x18 est aussi remis à 0 à chaque retour d'appel système (2 000 000 `svc #0x80` sur 2 000 000, et `getpid()` de libSystem) et pendant une boucle sans signal (1 500 à 5 000 changements par seconde, interruptions et ordonnancement). x18 est donc inutilisable par du code JIT.
- Cadre de signal : environ 1 120 octets au-dessus de la pile (siginfo à top-1120, ucontext à top-1016) ; la pile alternative minimale acceptée est `MINSIGSTKSZ` = 32 768 (`ENOMEM` en dessous), `SIGSTKSZ` = 131 072. Un thread créé n'hérite pas de la pile alternative de son créateur (`SS_DISABLE`).

### D.3 Interruption de threads (mesuré, `d_thread_signals`)

Les durées sont en microsecondes ; elles sont mesurées avec `cntvct_el0` normalisé par le `CNTFRQ_EL0` du processus (voir D.6).

- `pthread_kill` vers un thread qui boucle dans une région `MAP_JIT` : entrée du handler 3,1 µs (médiane), 3,6 µs (p90), 9,6 µs (p99) ; le handler voit un pc dans la boucle (4000 sur 4000) ; l'appel `pthread_kill` lui-même dure 0,71 µs. Le handler redirige le pc vers un stub dans la même région, dans une autre région `MAP_JIT`, ou dans le texte de l'hôte : 12 000 retours corrects sur 12 000 ; thread revenu en C après `sigreturn` : 3,4 µs (médiane), 10 µs (p99).
- Thread bloqué : `read` sur un tube, `os_sync_wait_on_address`, `usleep`, `nanosleep` reviennent tous avec `EINTR` (500 sur 500, sans `SA_RESTART`). Entrée du handler : 6,8 µs, 16,6 µs, 11,3 µs, 15,2 µs (médianes).
- Arrêt et reprise par signal (le handler attend sur `os_sync_wait_on_address`) : `pthread_kill` vers handler parqué 3,4 µs (médiane), 28,8 µs (p99) ; réveil vers thread de nouveau en cours d'exécution 5,9 µs ; cycle complet 9,5 µs (médiane), 62 µs (p99). Le thread est réellement arrêté dans 3000 cycles sur 3000.
- Arrêt par Mach (`thread_suspend`, `thread_get_state`, `thread_resume`) sans droit particulier : `thread_suspend` revient en 6,9 µs, `thread_get_state` 1,2 µs, `thread_resume` vers reprise 5,3 µs, cycle 13,6 µs (médiane) ; le pc lu est dans la boucle JIT dans 3000 cas sur 3000. Redirection par `thread_set_state` du pc vers un stub JIT : 2000 réussites sur 2000, 12,4 µs de la suspension au retour en C.
- Masques : un signal dirigé vers le processus (`kill(getpid(), ...)`) est toujours remis au thread principal s'il ne le bloque pas (200 sur 200), sinon au premier thread débloqué. Dix envois du même signal pendant qu'il est bloqué donnent un seul appel de handler. Un thread hérite du masque de son créateur. Dans un handler, le signal remis et les signaux de `sa_mask` sont bloqués ; au retour, le masque est celui de `uc_sigmask` en fin de handler : une modification de `uc_sigmask` est appliquée, les appels `pthread_sigmask` faits dans le handler sont écrasés.
- Il n'y a pas de signaux temps réel (`SIGRTMIN` absent), pas de `sigqueue`, `sigtimedwait`, `sigwaitinfo`, `signalfd`, `timer_create`, `tgkill`.

### D.4 Mode d'écriture JIT et signaux (mesuré, `d_jit_signal_state`)

- À l'entrée d'un handler le thread est toujours en mode exécution, quel que soit son mode au moment de l'interruption (signal asynchrone, `pthread_kill` sur soi-même ou `brk`) : un store dans une page JIT lève `SIGBUS`, un appel de stub JIT réussit.
- Après `sigreturn` : si le thread était en mode écriture à l'arrivée du signal, il est en mode écriture, quoi que le handler ait fait. S'il était en mode exécution, il garde le mode que le handler lui a laissé (donc en mode écriture si le handler a appelé `pthread_jit_write_protect_np(0)` et ne l'a pas annulé).
- Conséquence mesurée : un handler qui redirige le pc vers du code JIT échoue (`SIGBUS` à la reprise) si le thread était en mode écriture, ou si le handler a laissé le mode écriture ; il réussit si le thread était en mode exécution et que le handler n'a rien changé.
- Une faute synchrone levée par du code qui s'exécute dans une page JIT : le handler démarre en mode exécution ; un mode écriture laissé par le handler persiste après `sigreturn`.

### D.5 À compléter

- Aller-retour faute et reprise (détection de code auto-modifiant par protection des pages), faute dans le handler.

### D.6 Compteur de temps (mesuré)

- `CNTFRQ_EL0` vaut 1 000 000 000 dans un processus lancé par `exec` ou `posix_spawn` (le compteur avance de 1,00 tick par ns), et 24 000 000 dans un processus créé par `fork` sans `exec` (0,02 tick par ns). `mach_absolute_time` avance à 24 MHz (rapport 125/3) dans les deux cas. Aucune valeur brute de `cntvct_el0` ne doit être comparée entre ces deux types de processus.

## Implications for FEXCore on Darwin

a completer

## Ce qui n'est pas testable ici

a completer

## Fichiers

a completer
