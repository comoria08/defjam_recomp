/*
 * lanceur.h - the launcher, before the game starts (point 141).
 *
 * Finds the game folder, opens the log when nobody redirected it, reads the
 * settings in defjam.ini (imported from the old .env the first time), shows
 * the settings window, and turns the settings into the RECOMP_* environment
 * variables the runtime reads. Variables already in the environment win, so a
 * test or a diagnosis can still set anything by hand. RECOMP_LANCEUR=0 skips
 * all of it and leaves the environment exactly as it was given.
 */
#ifndef LANCEUR_H
#define LANCEUR_H

/* 1: start the game. 0: the player closed the launcher, quit. */
int lanceur_run(void);

#endif
