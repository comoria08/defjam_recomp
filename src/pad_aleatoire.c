/*
 * pad_aleatoire.c - Y on the fighter select picks a fighter at random
 * (asked by the user, point 156).
 *
 * Y does nothing on that screen (measured with the user), and the cursor
 * stops at the edges of the grid instead of wrapping (same session): so a
 * known starting point can be reached by pushing against two edges, and a
 * random cell from there. The macro, on pad 1: up to the top row then down r
 * rows, left to the first column then right c columns, c and r drawn at
 * random, the two axes' steps shuffled together; then A.
 * The grid is 6 columns wide and scrolls (3 rows shown). How many rows it has
 * depends on what is unlocked: RECOMP_ALEA_RANGEES, 12 by default (counted by
 * the user, point 156).
 *
 * Which screen is up is told by what is drawn: Y means "random" only while at
 * least 3 of 5 textures that appear with the grid (fingerprints of the
 * texture pack, measured on the fighter select, point 156) were bound within
 * the last 6 presents. Y also hits in a fight, so elsewhere it passes through
 * untouched. RECOMP_ALEA=0 turns all of this off.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "nv2a_d3d11.h"
#include "pad_aleatoire.h"

/* Textures that came with the grid and its cards, not with a fighter. */
static const uint64_t s_grille[] = {
    0x8AAF509C2628E664ull,      /* 128x16 */
    0x00356C5B91180D7Bull,      /* 512x64 */
    0xB80FF618DF10FA6Cull,      /* 128x64 */
    0x406D9D4724ECCB69ull,      /* 64x128 */
    0x2905C4E903A98577ull,      /* 256x256 */
};
#define N_GRILLE (int)(sizeof s_grille / sizeof s_grille[0])

#define B_HAUT   0x01
#define B_BAS    0x02
#define B_GAUCHE 0x04
#define B_DROITE 0x08
#define B_A      0x100

#define MAX_ETAPES 160
static uint16_t  s_etape[MAX_ETAPES];
static int       s_n, s_i, s_appuye;
static ULONGLONG s_t;
static int       s_y_avant, s_dans_avant = -1;
/* Where the cursor is, followed from the moment the grid appears: it starts
 * at the top left (measured, point 156, both grids), and after each A the
 * next grid does too. Every D-pad press, the player's or the macro's, moves
 * it, stopping at the edges as the game does. */
static int       s_col, s_rang;
static uint16_t  s_dirs_avant;
static int       s_a_avant;

static int rangees(void)
{
    const char *e = getenv("RECOMP_ALEA_RANGEES");
    int r = e ? atoi(e) : 12;
    return r < 1 ? 1 : r > 60 ? 60 : r;
}

static void suivre(uint16_t dirs, int a)
{
    const uint16_t nouveau = (uint16_t)(dirs & ~s_dirs_avant);
    if (nouveau & B_HAUT)   s_rang = s_rang > 0 ? s_rang - 1 : 0;
    if (nouveau & B_BAS)    s_rang = s_rang < rangees() - 1 ? s_rang + 1 : s_rang;
    if (nouveau & B_GAUCHE) s_col = s_col > 0 ? s_col - 1 : 0;
    if (nouveau & B_DROITE) s_col = s_col < 5 ? s_col + 1 : 5;
    if (a && !s_a_avant) {                      /* a pick: the next grid, top left */
        s_col = 0;
        s_rang = 0;
    }
    s_dirs_avant = dirs;
    s_a_avant = a;
}

static int actif(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_ALEA");
        on = !(e && atoi(e) == 0);
    }
    return on;
}

static int grille_vue(void)
{
    int k, n = 0;
    for (k = 0; k < N_GRILLE; k++)
        n += nvd3d_texture_seen(s_grille[k], 6);
    return n;
}

static void tirer(int vues)
{
    const int nr = rangees();
    int k, c, r, essai;
    static int graine;

    if (!graine) {
        graine = 1;
        srand((unsigned)GetTickCount64());
    }
    c = rand() % 6;
    r = rand() % nr;
    /* From where the cursor is (followed, see suivre) straight to the drawn
     * cell, the moves in a random order, with one to three detours (a move
     * and its opposite) for a cursor that wanders: the user wanted "up left
     * down right right up", not every up then every left (point 156). Only
     * orders that stay inside the grid are kept, so no press is lost against
     * an edge and the arrival is exact. */
    for (essai = 0; essai < 200; essai++) {
        uint16_t m[MAX_ETAPES];
        int n = 0, x = s_col, y = s_rang, ok = 1, detours = 1 + rand() % 3;
        for (k = 0; k < c - s_col; k++) m[n++] = B_DROITE;
        for (k = 0; k < s_col - c; k++) m[n++] = B_GAUCHE;
        for (k = 0; k < r - s_rang; k++) m[n++] = B_BAS;
        for (k = 0; k < s_rang - r; k++) m[n++] = B_HAUT;
        for (k = 0; k < detours && n + 2 < MAX_ETAPES - 1; k++) {
            if (rand() & 1) { m[n++] = B_DROITE; m[n++] = B_GAUCHE; }
            else            { m[n++] = B_BAS;    m[n++] = B_HAUT; }
        }
        for (k = n - 1; k > 0; k--) {           /* shuffle */
            const int j = rand() % (k + 1);
            const uint16_t t = m[k]; m[k] = m[j]; m[j] = t;
        }
        for (k = 0; k < n && ok; k++) {
            x += (m[k] == B_DROITE) - (m[k] == B_GAUCHE);
            y += (m[k] == B_BAS) - (m[k] == B_HAUT);
            ok = x >= 0 && x <= 5 && y >= 0 && y < nr;
        }
        if (!ok && essai < 199)
            continue;
        if (!ok) {                              /* never: then the plain path */
            n = 0;
            for (k = 0; k < c - s_col; k++) m[n++] = B_DROITE;
            for (k = 0; k < s_col - c; k++) m[n++] = B_GAUCHE;
            for (k = 0; k < r - s_rang; k++) m[n++] = B_BAS;
            for (k = 0; k < s_rang - r; k++) m[n++] = B_HAUT;
        }
        memcpy(s_etape, m, (size_t)n * sizeof m[0]);
        s_n = n;
        break;
    }
    s_etape[s_n++] = B_A;
    s_i = 0;
    s_appuye = 1;
    s_t = GetTickCount64();
    fprintf(stderr, "  [ALEA] Y sur la grille (%d/%d textures) : de colonne %d rangee %d vers"
                    " colonne %d rangee %d sur %d, %d appuis\n", vues, N_GRILLE, s_col + 1,
            s_rang + 1, c + 1, r + 1, nr, s_n);
    fflush(stderr);
}

void pad_aleatoire(uint16_t *buttons, uint8_t analog[8])
{
    const int y = analog[3] > 30;
    ULONGLONG now;

    if (!actif())
        return;
    now = GetTickCount64();

    if (s_n) {
        /* The macro owns the pad's directions, A and Y until it is done. */
        const char *e = getenv("RECOMP_ALEA_MS");
        /* 35 ms pressed, 35 released: two of the title's reads at 60 a second
         * each way (60 was slow to watch, point 156). */
        const ULONGLONG ms = e ? (ULONGLONG)atoi(e) : 35;
        *buttons &= (uint16_t)~(B_HAUT | B_BAS | B_GAUCHE | B_DROITE);
        analog[0] = 0;
        analog[3] = 0;
        if (s_appuye) {
            const uint16_t b = s_etape[s_i];
            *buttons |= (uint16_t)(b & 0x0F);
            if (b & B_A)
                analog[0] = 255;
            if (now - s_t >= ms) {
                s_appuye = 0;
                s_t = now;
            }
        } else if (now - s_t >= ms) {
            if (++s_i >= s_n) {
                s_n = 0;
                fprintf(stderr, "  [ALEA] fait\n");
            } else {
                s_appuye = 1;
                s_t = now;
            }
        }
        /* The cursor follows the macro's presses as it follows the player's. */
        suivre((uint16_t)(*buttons & 0x0F), analog[0] > 30);
        s_y_avant = 1;                          /* the Y that started it is spent */
        return;
    }

    {
        const int vues = grille_vue();
        const int dans = vues >= 3;
        if (dans != s_dans_avant) {
            fprintf(stderr, "  [ALEA] grille de selection %s (%d/%d textures)\n",
                    dans ? "a l'ecran" : "quittee", vues, N_GRILLE);
            s_dans_avant = dans;
            if (dans) {                         /* it appears with the cursor top left */
                s_col = 0;
                s_rang = 0;
                s_dirs_avant = (uint16_t)(*buttons & 0x0F);
                s_a_avant = analog[0] > 30;
            }
        }
        if (dans)
            suivre((uint16_t)(*buttons & 0x0F), analog[0] > 30);
        if (y && !s_y_avant) {
            if (dans) {
                tirer(vues);
                analog[3] = 0;                  /* the title never sees this Y */
            }
        }
    }
    s_y_avant = y;
}
