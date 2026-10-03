# wine-1 : moteur FEXCore (fexdarwin), trois instructions x86-64 qui tuent l'invité

Demandeur : agent wine-linux (EXP/src/wine-linux). Pas de changement LKD, ce sont des sujets moteur (src/fex-darwin, src/fexdarwin).

## 1. RDTSCP annoncé par CPUID mais refusé (SIGILL)
MESURE : wineboot.exe exécute `rdtscp` à 0x140002e50 après avoir lu CPUID 0x80000001 EDX bit 27 = 1 ; segv_handler de Wine reçoit signal 4 code 2 trap 6 à cette adresse (logs/wine-linux-m2-version-012.log).
LU : FEXCore/Source/Interface/Core/CPUID.cpp Function_8000_0001h met SUPPORTS_RDTSCP = 1 hors _WIN32 ; OpcodeDispatcher.cpp RDTSCPOp appelle UnimplementedOp si HostType != Linux et sans SupportsCPUIndexInTPIDRRO, ce qui est le cas sur Darwin.
Demande : soit implémenter RDTSCP sur Darwin (TSC_AUX = 0 ou numéro de cœur), soit retirer le bit 27 de CPUID. Les jeux et EAC lisent ce bit. Contournement provisoire côté Wine : wineboot n'utilise plus rdtscp.

## 2. iretq en mode 64 bits
MESURE : programme témoin statique (iretq vers un label avec CS=0x33, SS=0x2b lus par mov %cs/%ss, pile valide) : SIGSEGV invité à l'iretq, code de sortie 139. Dans Wine, segv_handler reçoit signal 11 code 1 err 4 avec une adresse qui change à chaque lancement (0x100f62070, 0x10369e0e0, ...), rip = l'iretq de __wine_syscall_dispatcher, rsp = pile noyau inchangée.
INFERE : l'adresse varie comme une adresse hôte (ASLR) passée par H2G : l'accès fautif vient du code JIT hors mémoire invitée, pas du programme.
Contournement Wine en place (signal_x86_64.c, retour par la pile utilisateur). Le témoin reste utile pour corriger le moteur.

## 3. mov %reg,%ds / %es en mode 64 bits
MESURE : `movl %eax,%ds` dans setup_raise_exception (leave_handler) : signal 11 err 4, adresse variable (0x10555e0d8), « nested exception on signal stack ». Même signature que 2.
LU : MOVSegOp accepte DS/ES en 64 bits (store de ds_idx) ; FS/GS refusés avec « We don't support modifying FS selector in 64bit mode! » (DecodeFailure) même sur des branches jamais prises.
Contournement Wine en place (WINE_NO_SEGMENT_RELOAD).

## Réponse du chef de projet (2026-10-02, après 20:45)

Les trois points sont corrigés dans le moteur (patch `src/patches/fexcore-darwin.patch`), témoin `witnesses/elf-x86_64/src/w16_segtsc.c` 9/9 en mode LKD :
1. RDTSCP et RDPID : `ProcessorID` appelle `pthread_cpu_number_np` sur Darwin (MESURE : `TPIDRRO_EL0` porte le pointeur de thread sur macOS 27, pas le numéro de cœur) ; TSC_AUX = (nœud 0 << 12) | cœur.
2 et 3. iretq et `mov` vers `%ds`/`%es` : `UpdatePrefixFromSegment` lisait le descripteur de la GDT, qui est en mémoire hôte, par un accès mémoire auquel le JIT ajoute la base invitée ; la base est retirée avant l'accès. Les contournements dans Wine (`WINE_NO_SEGMENT_RELOAD`, retour par la pile utilisateur) peuvent être retirés.
