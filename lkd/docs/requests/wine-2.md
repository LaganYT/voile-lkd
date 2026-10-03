# wine-2 : le garde « more than 200000 VM regions » tue tout lancement de Wine (build-lead/host)

Demandeur : agent wine-linux. Bloquant pour M2.

MESURE (logs/wine-linux-m2-cmd-001.trace.log, -002, -004) : `run/wine-lkd-own.sh 90 /opt/wine/bin/wine cmd /c echo hi` avec EXP/build-lead/host/fexdarwin-run. Code 134 en moins de 2 s, trois fois `[fexdarwin] more than 200000 VM regions in this process: ending it to protect the kernel` (wine, wineserver et un troisième processus). La trace LKD de [2] s'arrête juste après `clone3(...) = 3` (lancement de wineserver), sans autre syscall lourd : en tout une trentaine de mmap/mprotect/munmap, les plus gros étant des réservations PROT_NONE (0x10000+0x67ff0000, 0x7f000000+0xff0000, 0x7fde0000+0x200000).
MESURE : `vmmap -summary` du processus wine 0,1 s après le lancement : 1070 régions. Le même binaire Wine tournait plusieurs dizaines de secondes avec l'hôte construit avant la panique (logs 012, 013).
INFERE : 1070 régions ne deviennent pas 200000 en moins de 2 s avec ces appels. Hypothèse à vérifier dans CountOwnRegions (LkdGlue.cpp) : une région de taille nulle ou un `Addr += Size` qui déborde à 0 ferait recompter jusqu'à la limite (faux positif). Autre hypothèse : la copie des VMA au fork crée une région hôte par page.
Demande : vérifier le compteur (dernière région, débordement, taille 0) ou donner une variable pour lire le nombre réel au moment de la décision (par exemple écrire N et la dernière adresse dans le message).

## Témoin décisif (MESURE)
src/wine-linux/tests/reserve.c (statique musl) : un mmap qui échoue puis `sleep(3)`, rien d'autre. `FEXDARWIN_LKD=1 run-with-timeout.sh 20 build-lead/host/fexdarwin-run $PWD/src/wine-linux/tests/reserve 0x1000000` : imprime `mmap ... -> 0xffffffffffffffff`, puis `more than 200000 VM regions`, sans jamais atteindre `alive`. Tout processus invité vivant plus de ~0,5 s est donc tué : le garde est un faux positif, indépendant de Wine. Les tests courts (dash) passent parce qu'ils finissent avant la première mesure.

## Mesure du nombre réel (MESURE, hôte relinké après wine-1)
Même témoin, `vmmap` du processus 0,15 s après le lancement : TOTAL 332 régions (dont 10 de la mémoire écrite). Le processus est tout de même tué par le garde ~0,5 s plus tard. Le compteur CountOwnRegions est donc faux d'au moins trois ordres de grandeur ; aucun lancement de Wine n'est possible tant qu'il n'est pas corrigé.
