# wine-9 : FEXCore ASSERT « LookupCache: Failed to allocate PagePointer » au clone3 d'un nouveau fil -> le jeu meurt (SIGILL)

Demandeur : agent wine-linux. Bloquant : Rocket League s'arrête au menu après environ 135 s de jeu.

MESURE (evidence/eac-runs/20261003T003343, guest.log lignes 159-164) :
```
[FEXCore ASSERT] LookupCache.cpp:42, LookupCache: Failed to allocate PagePointer
[fexdarwin] FATAL unhandled fault: signal 4 (si_code 2) fault address 0x102d34648
[fexdarwin]   host pc=0x102d34648 sp=0x706241a660 (outside JIT code: host C++)
[fexdarwin]   guest rip=0x7f8275edc529 rax=0x1b3 rbx=0x7f8275e5e620 rcx=0x7f8275edc529 ...
```
rax = 0x1b3 = 435 = clone3, rcx = rip : le processus du jeu crée un fil et la construction du LookupCache du nouveau fil (Core.cpp:429, make_unique<LookupCache>) échoue : VirtualAlloc(TotalCacheSize) rend -1, ASSERT -> trap.
LU (LookupCache.cpp:27, Context.h:349, LookupCache.h:468) : TotalCacheSize = VirtualMemSize/4096*8 + CODE_SIZE + MAX_L1_SIZE = 128 Mio + 128 Mio + L1, soit environ 256 Mio de réservation hôte par fil invité.
INFERE : soit les LookupCache des fils terminés ne sont pas libérés (Rocket League et Wine créent et détruisent beaucoup de fils : workers, timers, threadpool), soit l'espace où l'allocateur hôte place ces réservations (adresses hôte 0x70_xxxx_xxxx) est plein. Deux runs précédents de 150 s n'ont pas eu l'assertion ; celui-ci est mort à 134 s de jeu, après 1560 Present (menu à 60 i/s, intervalle monté à 42 ms juste avant).
Demande : vérifier la libération du LookupCache à la sortie d'un fil et l'errno du mmap qui échoue ; un message avec errno et le nombre de fils vivants au lieu de l'assertion aiderait.

Deuxième occurrence (MESURE, evidence/eac-runs/20261003T004000) : même assertion, même rip invité 0x7f8275edc529, même clone3, mais à 112 s, pendant le chargement (96 Present seulement). Ce n'est donc pas lié à la durée de jeu.
LU (AllocatorHooks.h:128) : sur Darwin VirtualAlloc = mmap(NULL, environ 260 Mio, RW, MAP_PRIVATE|MAP_ANONYMOUS) ; c'est ce mmap hôte qui rend MAP_FAILED.
MESURE : les trois runs d'avant 00:30 (20261002T233036, 20261003T002238, 20261003T002813, jusqu'à 150 s et 1680 Present) n'ont pas l'assertion ; les deux suivants (00:34 et 00:40) l'ont. Fichiers LKD modifiés entre-temps : src/lkd/src/net/{ifaces,socket,sockopt,sockmsg}.c, net_priv.h (travail wine-8) ; build-lead/host/fexdarwin-run reconstruit à 00:40. INFERE : avec des interfaces réseau visibles, le jeu ou EAC démarre plus de fils (ou de fils réseau), ou le nouveau code réserve de l'espace hôte ; l'espace d'adresses hôte libre pour un mmap de 260 Mio d'un seul tenant est épuisé ou fragmenté (la fenêtre invitée de 2^47 occupe l'essentiel de l'espace hôte).
Conséquence : le test de jeu hors ligne (Free Play, 60 s) est bloqué.
