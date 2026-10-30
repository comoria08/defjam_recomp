r"""Copies corrigees de fonctions du C genere (src/recomp_patch.c).

On ne peut pas corriger une instruction dans src/recomp/gen (il est regenere). Pour une fonction
sub_X a corriger : recomp_manual.c definit sub_X (qui appelle sub_X_patch, et garde sub_X_gen
pour comparer) ; cet outil recopie le corps genere sub_X_gen sous le nom sub_X_patch en appliquant
des substitutions (expressions regulieres), et s'arrete si une substitution ne trouve pas sa cible
le nombre de fois attendu. A relancer apres chaque regeneration :

    py -3 outils/patch_gen.py

Corrections (2026-10-17) :

  Blazin de face / de dos. Les 82 paires de blazin.mnl sont rangees face (N06, 0 a 56) puis dos
  (N07, 57 a 81) et le jeu deduit le type du rang : << paire + 0x279E >= 0x27D7 >> (paire >= 57),
  dans sub_001A8BC0 (le Blazin ne part que depuis une prise du meme cote) et sub_001D0F80. Les
  paires du mod (82 et plus) passaient toutes pour des Blazin de dos. BLAZIN_CLE rend une valeur
  qui donne le meme resultat de comparaison, d'apres le nom (g_blazin_dos, rempli par
  recomp_manual.c depuis blazin.mnl) : pour les 82 paires d'origine, exactement l'ancien test.

Correction (2026-10-27) :

  La table des voix des coreID (0x2DC5D0, 12 octets par coreID, 104 entrees) : sub_0003E170, son
  seul lecteur, la lit a g_voix_va, une copie agrandie a 142 entrees (recomp_manual.c), pour les
  coreID clones 104 a 140.

Correction (2026-10-30) :

  Les paires WPAR / WWAZ de main_all.dat (0x3B8648, 16 octets par coreID - 1, 141 places fixes) :
  leurs quatre lecteurs les lisent a g_paires_va, un tableau agrandi a CORE_MAX - 1 places et
  complete par les paires qui suivent la 141e dans le fichier (recomp_manual.c, sub_001A47B0), pour
  les clones 142 et plus : les coreID 86 a 141 restent au heros du mode Story.
"""
import glob
import os
import re

ICI = os.path.dirname(os.path.abspath(__file__))
RACINE = os.path.dirname(ICI)

CLE = (r'_fa = \(uint32_t\)\((e[a-d]x|e[sd]i|ebx|ebp)\) & 0xFFFFFFFFu; _fb = \(uint32_t\)\(0x27D7\)',
       r'_fa = (uint32_t)(BLAZIN_CLE(\1)) & 0xFFFFFFFFu; _fb = (uint32_t)(0x27D7)')
PAIRES_WPAR = (r'0x3B864C(?![0-9A-Fa-f])', r'(g_paires_va + 0x4u)')
PAIRES_WWAZ = (r'0x3B8654(?![0-9A-Fa-f])', r'(g_paires_va + 0xCu)')
CORRECTIONS = {
    '001A8BC0': [CLE + (2,)],
    '001D0F80': [CLE + (2,)],
    '0003E170': [(r'0x2DC5D([0-6])(?![0-9A-Fa-f])', r'(g_voix_va + 0x\1u)', 6)],
    '001A7F30': [PAIRES_WWAZ + (3,)],
    '001B2090': [PAIRES_WPAR + (1,)],
    '001B4930': [PAIRES_WWAZ + (3,)],
    '001B4BA0': [PAIRES_WWAZ + (1,)],
}
ENTETE = r'''/* Genere par outils/patch_gen.py -- ne pas modifier a la main. */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_funcs.h"
#include <math.h>

/* rang de paire de Blazin -> 1 si Blazin de dos (nom N07 dans blazin.mnl), rempli par recomp_manual.c */
extern unsigned char g_blazin_dos[512];
/* la table des voix des coreID, agrandie (recomp_manual.c, sub_0003E170) */
extern uint32_t g_voix_va;
/* les paires WPAR / WWAZ de main_all.dat, agrandies (recomp_manual.c, sub_001A47B0) */
extern uint32_t g_paires_va;
/* x = paire + 0x279E ; rend 0x27D7 (>= seuil) pour un Blazin de dos, 0x279E sinon */
#define BLAZIN_CLE(x) ((uint32_t)(x) - 0x279Eu < 512u \
    ? (g_blazin_dos[(uint32_t)(x) - 0x279Eu] ? 0x27D7u : 0x279Eu) : (uint32_t)(x))
'''


def extraire(nom):
    for f in sorted(glob.glob(os.path.join(RACINE, 'src', 'recomp', 'gen', 'recomp_*.c'))):
        s = open(f, encoding='utf-8', errors='surrogateescape').read()
        m = re.search(r'\nvoid sub_%s_gen\(void\)\n\{' % nom, s)
        if m:
            fin = s.index('\n}\n', m.start()) + 3
            return s[m.start() + 1:fin]
    raise SystemExit('sub_%s_gen introuvable : le wrapper de recomp_manual.c manque, ou regenerer' % nom)


def main():
    out = [ENTETE]
    for nom, subs in CORRECTIONS.items():
        corps = extraire(nom).replace('sub_%s_gen(void)' % nom, 'sub_%s_patch(void)' % nom, 1)
        for motif, rempl, attendu in subs:
            corps, n = re.subn(motif, rempl, corps)
            print('sub_%s : %d substitution(s) (attendu %d)' % (nom, n, attendu))
            if n != attendu:
                raise SystemExit('nombre inattendu : le code genere a change, verifier')
        out.append(corps)
    open(os.path.join(RACINE, 'src', 'recomp_patch.c'), 'w', encoding='utf-8', newline='\n').write('\n'.join(out))


if __name__ == '__main__':
    main()
