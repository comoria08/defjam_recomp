/* pad_aleatoire.h - Y on the fighter select picks a fighter at random. See
 * pad_aleatoire.c. Called from XInputGetState for pad 1, on the state about
 * to be handed to the title: buttons (D-pad bits 0x01-0x08) and the eight
 * analog buttons (A is analog[0], Y analog[3]). */
#ifndef PAD_ALEATOIRE_H
#define PAD_ALEATOIRE_H

#include <stdint.h>

void pad_aleatoire(uint16_t *buttons, uint8_t analog[8]);

#endif
