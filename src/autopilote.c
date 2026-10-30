/* autopilote.c - la manette 1 pilotee par un script, pour les essais sans
 * joueur (RECOMP_AUTOPILOTE). Eteint par defaut : quand l'utilisateur joue,
 * c'est lui qui tient la manette ; ce pilote ne sert qu'aux essais lances par
 * essai_auto.sh, qui met aussi ses sauvegardes a l'abri.
 *
 * Le script est une suite de mots separes par des espaces :
 *   wN        attendre N millisecondes
 *   eXXXXXXXX attendre que l'ecran courant ait cette vtable (hexadecimal ;
 *             2 minutes au plus, puis le pilote continue et le dit) ;
 *             eXXXXXXXX:START appuie START toutes les 2 s pendant l'attente
 *   A B X Y NOIR BLANC START BACK HAUT BAS GAUCHE DROITE GG GD
 *             appuyer 150 ms puis relacher 250 ms ; MOT*N le repete N fois
 *   F3        declencher le test de determinisme (determinisme.c)
 *   ALEA      jusqu'a la fin : des boutons et directions au hasard, changes
 *             toutes les 100 a 300 ms (graine fixe), jamais START ni BACK ;
 *             ALEA:N pendant N ms seulement, puis le script continue
 * Chaque etape est ecrite dans le journal ([AUTO]).
 *
 * Mesure du chemin (essai du 2026-10-18, captures) : intros, titre « Press
 * START », avertissement de sauvegarde automatique (A), menu principal sur
 * Story. Les menus sont un seul ecran Flash (vtable 0x0029D784).
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "autopilote.h"

#define MAX_ETAPES 256

typedef struct {
    char     quoi;          /* 'w' attente, 'e' ecran, 'b' bouton, 'f' F3, 'r' hasard */
    uint32_t val;           /* ms, vtable, ou masque (bas 16 : croix/start/back ; haut : analog) */
    int      fois;
    uint32_t pendant;       /* 'e' : bouton appuye toutes les 2 s pendant l'attente */
} etape_t;

static etape_t  g_et[MAX_ETAPES];
static int      g_n, g_cur, g_actif = -1;
static ULONGLONG g_t0;      /* debut de l'etape courante */
static int      g_rep;      /* repetitions faites de l'etape courante */
static volatile uint32_t g_ecran;
static volatile LONG g_f3;
static uint32_t g_graine = 0x2026u;

/* Les boutons : bits 0-7 du mot de la croix (HAUT 01, BAS 02, GAUCHE 04,
 * DROITE 08, START 10, BACK 20), puis un bit par bouton analogique :
 * A B X Y NOIR BLANC GG GD = analog[0..7]. */
static const struct { const char *nom; uint32_t masque; } k_boutons[] = {
    { "HAUT", 0x01 }, { "BAS", 0x02 }, { "GAUCHE", 0x04 }, { "DROITE", 0x08 },
    { "START", 0x10 }, { "BACK", 0x20 },
    { "A", 0x10000 }, { "B", 0x20000 }, { "X", 0x40000 }, { "Y", 0x80000 },
    { "NOIR", 0x100000 }, { "BLANC", 0x200000 }, { "GG", 0x400000 }, { "GD", 0x800000 },
};

static void lire_script(const char *s)
{
    char mot[64];
    while (*s && g_n < MAX_ETAPES) {
        int l = 0;
        while (*s == ' ' || *s == ',') s++;
        while (*s && *s != ' ' && *s != ',' && l < 63) mot[l++] = *s++;
        mot[l] = 0;
        if (!l) break;
        etape_t e = { 0, 0, 1, 0 };
        char *etoile = strchr(mot, '*');
        if (etoile) { *etoile = 0; e.fois = atoi(etoile + 1); if (e.fois < 1) e.fois = 1; }
        if (mot[0] == 'w' && mot[1] >= '0' && mot[1] <= '9') {
            e.quoi = 'w'; e.val = (uint32_t)atoi(mot + 1);
        } else if (mot[0] == 'e' && mot[1]) {
            char *deux = strchr(mot, ':');
            e.quoi = 'e'; e.val = (uint32_t)strtoul(mot + 1, NULL, 16);
            if (deux)
                for (size_t i = 0; i < sizeof k_boutons / sizeof k_boutons[0]; i++)
                    if (!strcmp(deux + 1, k_boutons[i].nom)) e.pendant = k_boutons[i].masque;
        } else if (!strcmp(mot, "F3")) {
            e.quoi = 'f';
        } else if (!strncmp(mot, "ALEA", 4) && (mot[4] == 0 || mot[4] == ':')) {
            e.quoi = 'r';
            e.val = mot[4] == ':' ? (uint32_t)atoi(mot + 5) : 0;   /* 0 : jusqu'a la fin */
        } else {
            for (size_t i = 0; i < sizeof k_boutons / sizeof k_boutons[0]; i++)
                if (!strcmp(mot, k_boutons[i].nom)) { e.quoi = 'b'; e.val = k_boutons[i].masque; }
            if (!e.quoi) {
                fprintf(stderr, "[AUTO] mot inconnu ignore : %s\n", mot);
                continue;
            }
        }
        g_et[g_n++] = e;
    }
}

static uint32_t hasard(void)
{
    g_graine = g_graine * 1103515245u + 12345u;
    return g_graine >> 8;
}

static void appliquer(uint32_t masque, uint16_t *boutons, uint8_t analog[8])
{
    *boutons = (uint16_t)(masque & 0xFF);
    for (int i = 0; i < 8; i++)
        analog[i] = (masque >> (16 + i)) & 1 ? 255 : 0;
}

void autopilote_ecran(uint32_t vtable) { g_ecran = vtable; }

int autopilote_f3(void)
{
    return InterlockedExchange(&g_f3, 0) != 0;
}

int autopilote(uint16_t *boutons, uint8_t analog[8], int16_t sticks[4])
{
    ULONGLONG t = GetTickCount64();

    if (g_actif < 0) {
        const char *s = getenv("RECOMP_AUTOPILOTE");
        g_actif = s && *s;
        if (g_actif) {
            lire_script(s);
            g_t0 = t;
            fprintf(stderr, "[AUTO] pilote actif : %d etapes\n", g_n);
        }
    }
    if (!g_actif)
        return 0;

    appliquer(0, boutons, analog);
    memset(sticks, 0, 4 * sizeof *sticks);

    while (g_cur < g_n) {
        etape_t *e = &g_et[g_cur];
        ULONGLONG ecoule = t - g_t0;
        int fini = 0;

        switch (e->quoi) {
        case 'w':
            fini = ecoule >= e->val;
            break;
        case 'e':
            if (g_ecran == e->val) fini = 1;
            else if (e->pendant && ecoule % 2000 >= 1000 && ecoule % 2000 < 1150) {
                appliquer(e->pendant, boutons, analog);
                return 1;
            }
            else if (ecoule >= 120000) {
                fprintf(stderr, "[AUTO] ecran %08X pas atteint en 2 min (ecran %08X), on continue\n",
                        e->val, g_ecran);
                fini = 1;
            }
            break;
        case 'b':
            if (ecoule < 150) { appliquer(e->val, boutons, analog); return 1; }
            if (ecoule < 400) return 1;
            if (++g_rep < e->fois) { g_t0 = t; return 1; }
            fini = 1;
            break;
        case 'f':
            InterlockedExchange(&g_f3, 1);
            fini = 1;
            break;
        case 'r': {
            static ULONGLONG prochain;
            static uint32_t masque;
            if (e->val && ecoule >= e->val) { fini = 1; break; }
            if (t >= prochain) {
                uint32_t h = hasard();
                masque = (h & 0x0F) & ~((h & 0x03) == 0x03 ? 0x03u : 0u);  /* pas haut+bas */
                masque &= ~((masque & 0x0C) == 0x0C ? 0x0Cu : 0u);         /* ni gauche+droite */
                masque |= ((h >> 4) & 0xFF) << 16;                          /* A..GD */
                prochain = t + 100 + (hasard() % 200);
            }
            appliquer(masque, boutons, analog);
            return 1;
        }
        }
        if (!fini)
            return 1;
        g_cur++;
        g_rep = 0;
        g_t0 = t;
        if (g_cur < g_n) {
            fprintf(stderr, "[AUTO] etape %d/%d (%c %08X x%d), ecran %08X\n", g_cur + 1, g_n,
                    g_et[g_cur].quoi, g_et[g_cur].val, g_et[g_cur].fois, g_ecran);
            fflush(stderr);
        }
    }
    return 1;
}
