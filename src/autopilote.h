/* autopilote.h - la manette 1 pilotee par un script, pour les essais sans
 * joueur. Voir autopilote.c. */
#ifndef AUTOPILOTE_H
#define AUTOPILOTE_H

#include <stdint.h>

/* Appele par XInputGetState pour le port 0 (fil d'entree du jeu). Rend 1 si le
 * pilote est actif : il a alors ecrit tout l'etat de la manette, et la vraie
 * manette comme le clavier sont ignores. */
int autopilote(uint16_t *boutons, uint8_t analog[8], int16_t sticks[4]);

/* L'ecran courant du jeu (vtable de l'ecran tenu par sub_00077040), note a
 * chaque mise a jour. */
void autopilote_ecran(uint32_t vtable);

/* 1 une seule fois quand le script atteint F3 : le test de determinisme se
 * declenche comme si la touche avait ete pressee. */
int autopilote_f3(void);

#endif
