/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <dbghelp.h>

/* Register model + MEM32/XBOX_PTR/PUSH32 macros, so hand-written functions
 * below read the guest stack exactly the way generated code does. */
#include "recomp/gen/recomp_types.h"
#include "autopilote.h"        /* manette 1 scriptee, essais sans joueur */
#include "dsound_hle.h"        /* DirectSound en haut niveau (toolkit) */

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern ptrdiff_t g_xbox_mem_offset;

/* ── A guarded call, for the few places that run title code the retail game
 * never runs ─────────────────────────────────────────────────────────────
 *
 * MSVC has __try/__except. Clang for MinGW accepts the syntax and catches
 * nothing (measured: the process dies on the fault). There, a vectored
 * handler registered last -- after main.c's, as an __except runs after it
 * under MSVC -- jumps back to the guard. The jmp_buf is taken with no frame,
 * _setjmp(buf, NULL), so longjmp restores registers without unwinding through
 * the exception dispatcher's frames, which are abandoned. Measured: 1000
 * faults out of 1000 caught, and a call that does not fault goes through. */
#if defined(_MSC_VER)
#  define GUARD_BEGIN  __try {
#  define GUARD_FAULT  } __except (EXCEPTION_EXECUTE_HANDLER) {
#  define GUARD_END    }
#else
#  include <setjmp.h>
static __thread jmp_buf *t_guard;

static LONG CALLBACK guard_veh(PEXCEPTION_POINTERS e)
{
    if (t_guard && e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        jmp_buf *j = t_guard;
        t_guard = NULL;
        longjmp(*j, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void guard_arm(jmp_buf *j)
{
    static volatile LONG installed;
    if (j && !InterlockedExchange(&installed, 1))
        AddVectoredExceptionHandler(0, guard_veh);
    t_guard = j;
}
#  define GUARD_BEGIN  { jmp_buf guard_jb_; \
                         if (!_setjmp(guard_jb_, NULL)) { guard_arm(&guard_jb_);
#  define GUARD_FAULT    guard_arm(NULL); } else {
#  define GUARD_END    } }
#endif

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
/* ── Hand-written replacements for generated bodies ────────────
 *
 * Each function below carries the sub_<VA> name of the guest function it
 * replaces. tools.recomp is run with --exclude-manual src/recomp_manual.c so
 * the generated body is skipped and this definition links in its place; the
 * dispatch table then resolves the VA to it for direct and indirect calls.
 *
 * Convention on entry (cdecl): MEM32(esp) is the return address pushed by the
 * caller, MEM32(esp + 4) the first argument. A function returns with
 * "esp += 4; return;" exactly like a lifted "ret".
 */

/* CRT memmove/memcpy at 0x002016B0 (LIBCMT 5849, identified by func_id).
 *
 * The lifted body is unusable: its copy tails are reached through
 * "jmp [edx*4 + 0x2017FC]" jump tables embedded in .text, and the lifter
 * turned those into indirect calls to mid-function addresses (0x00201770,
 * 0x00201954) that no dispatch entry can satisfy. Every memcpy whose size
 * hit one of those tails silently copied nothing, which is how the BIG4
 * archive loader went wrong right after opening assets\misc.viv. */
void sub_002016B0(void)
{
    /* The eax/esp aliases are only defined for generated code
     * (RECOMP_GENERATED_CODE); hand-written code names the globals. */
    uint32_t dst = MEM32(g_esp + 4);
    uint32_t src = MEM32(g_esp + 8);
    uint32_t n   = MEM32(g_esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), n);
    g_eax = dst;
    g_esp += 4; return;
}

/* Lire la memoire invitee en suivant des pointeurs que le titre est peut-etre
 * encore en train de construire : un mot intermediaire peut valoir n'importe
 * quoi, et MEM32 sur une valeur folle faute dans l'hote plutot que dans le
 * titre. Seules la RAM et sa fenetre contigue sont lisibles ; masquer le
 * quartet haut acceptait des adresses comme 0x40000000, mappees nulle part.
 * (Ne d'un acquittement des commandes du DSP, parti avec l'APU.) */
static int dsp_readable(uint32_t va)
{
    if (va >= 0x00010000u && (uint64_t)va + 4 <= 0x04000000u)
        return 1;
    if (va >= 0x80000000u && (uint64_t)va + 4 <= 0x84000000u)
        return 1;
    return 0;
}

/* ── DirectSound en haut niveau ─────────────────────────────────────────────
 *
 * Les 21 points d'entree de DSOUND que le jeu appelle (GUIDE_FR.md, points 166
 * et 167) vont a la couche DirectSound du toolkit (src/dsound_hle), qui rend
 * le son sans l'APU. Le reste de la bibliotheque d'origine ne tourne plus. */
void sub_0025C96E(void) { dsound_hle_DirectSoundCreate(); }
void sub_0025C9B5(void) { dsound_hle_DirectSoundCreateBuffer(); }
void sub_00256E61(void) { dsound_hle_IDirectSound_Release(); }
void sub_0025C620(void) { dsound_hle_IDirectSound_CreateSoundBuffer(); }
void sub_0025917D(void) { dsound_hle_IDirectSound_SetMixBinHeadroom(); }
void sub_002590D1(void) { dsound_hle_IDirectSound_DownloadEffectsImage(); }
void sub_0025C7A8(void) { dsound_hle_IDirectSound_SetI3DL2Listener(); }
void sub_00256E97(void) { dsound_hle_IDirectSoundBuffer_Release(); }
void sub_0025B15D(void) { dsound_hle_IDirectSoundBuffer_SetMixBins(); }
void sub_00259307(void) { dsound_hle_IDirectSoundBuffer_SetMixBinVolumes(); }
void sub_002592B3(void) { dsound_hle_IDirectSoundBuffer_SetEG(); }
void sub_002592CF(void) { dsound_hle_IDirectSoundBuffer_SetFilter(); }
void sub_0025B125(void) { dsound_hle_IDirectSoundBuffer_SetFrequency(); }
void sub_0025BB3D(void) { dsound_hle_IDirectSoundBuffer_SetBufferData(); }
void sub_002593E7(void) { dsound_hle_IDirectSoundBuffer_SetLoopRegion(); }
void sub_00259443(void) { dsound_hle_IDirectSoundBuffer_SetCurrentPosition(); }
void sub_00259323(void) { dsound_hle_IDirectSoundBuffer_Play(); }
void sub_002593A7(void) { dsound_hle_IDirectSoundBuffer_Pause(); }
void sub_00259383(void) { dsound_hle_IDirectSoundBuffer_StopEx(); }
void sub_00259407(void) { dsound_hle_IDirectSoundBuffer_GetStatus(); }
void sub_00259423(void) { dsound_hle_IDirectSoundBuffer_GetCurrentPosition(); }

static recomp_func_t son_hle_porte(uint32_t va)
{
    static const struct { uint32_t va; recomp_func_t f; } portes[] = {
        { 0x0025C96E, sub_0025C96E },   /* DirectSoundCreate */
        { 0x0025C9B5, sub_0025C9B5 },   /* DirectSoundCreateBuffer */
        { 0x00256E61, sub_00256E61 },   /* IDirectSound_Release */
        { 0x0025C620, sub_0025C620 },   /* IDirectSound_CreateSoundBuffer */
        { 0x0025917D, sub_0025917D },   /* IDirectSound_SetMixBinHeadroom */
        { 0x002590D1, sub_002590D1 },   /* IDirectSound_DownloadEffectsImage */
        { 0x0025C7A8, sub_0025C7A8 },   /* IDirectSound_SetI3DL2Listener */
        { 0x00256E97, sub_00256E97 },   /* IDirectSoundBuffer_Release */
        { 0x0025B15D, sub_0025B15D },   /* IDirectSoundBuffer_SetMixBins */
        { 0x00259307, sub_00259307 },   /* IDirectSoundBuffer_SetMixBinVolumes */
        { 0x002592B3, sub_002592B3 },   /* IDirectSoundBuffer_SetEG */
        { 0x002592CF, sub_002592CF },   /* IDirectSoundBuffer_SetFilter */
        { 0x0025B125, sub_0025B125 },   /* IDirectSoundBuffer_SetFrequency */
        { 0x0025BB3D, sub_0025BB3D },   /* IDirectSoundBuffer_SetBufferData */
        { 0x002593E7, sub_002593E7 },   /* IDirectSoundBuffer_SetLoopRegion */
        { 0x00259443, sub_00259443 },   /* IDirectSoundBuffer_SetCurrentPosition */
        { 0x00259323, sub_00259323 },   /* IDirectSoundBuffer_Play */
        { 0x002593A7, sub_002593A7 },   /* IDirectSoundBuffer_Pause */
        { 0x00259383, sub_00259383 },   /* IDirectSoundBuffer_StopEx */
        { 0x00259407, sub_00259407 },   /* IDirectSoundBuffer_GetStatus */
        { 0x00259423, sub_00259423 },   /* IDirectSoundBuffer_GetCurrentPosition */
    };
    unsigned i;
    for (i = 0; i < sizeof portes / sizeof portes[0]; i++)
        if (portes[i].va == va) return portes[i].f;
    return NULL;
}

/* The CRT thread-exit callback at 0x002091E3.
 *
 * It is not a detected function, and it cannot be: it sits in the middle of
 * sub_002091B9 (_endthread), immediately after a call to the thread
 * terminator that never returns. Nothing falls through to it -- the CRT
 * publishes its address (`mov [0x3CCA84], 0x2091E3` at 0x00203D22) and it is
 * only ever reached through that pointer. Seeding it as a function start is
 * not an option either: that would truncate sub_002091B9 at the seed.
 *
 * So it is written out here, instruction for instruction:
 *
 *     cmp [esp+4], 0 ; je .zero
 *     mov eax,[0x2FA244] ; test eax,eax ; je .ret ; call eax ; jmp .ret
 *   .zero:
 *     mov eax,[0x2FA248] ; test eax,eax ; je .after ; call eax
 *   .after:
 *     push 0 ; call 0x00203C67 ; pop ecx
 *   .ret:
 *     ret 4
 *
 * Unresolved, this call was skipped -- and it is skipped on the thread the
 * title spawns the instant it finishes reading its loading-screen assets.
 */
extern void sub_00203C67(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);

static recomp_func_t recomp_lookup_manual_fwd(uint32_t xbox_va);

/* Sonde sur la conversion flottant -> entier 16 bits du moteur audio
 * (`0x0012BE20`), sous `RECOMP_APU_TRACE`.
 *
 * Le grésillement est un trou de 3 à 5 échantillons par bloc rendu. Cette
 * sonde regarde l'accumulateur flottant *avant* la conversion : si le trou y
 * est déjà, il vient du mixage ou du décodeur, et sa position dans le bloc
 * dit lequel. Un trou en fin de bloc = le décodeur a rendu moins que demandé ;
 * un trou au début = les 16 premiers échantillons (la rampe de volume).
 *
 * Signature relevée dans le code généré : [esp+4] objet, [esp+8] nombre
 * d'échantillons, [esp+0xC] source flottante, [esp+0x10] destination 16 bits.
 */
/* Le décodeur audio du jeu (`0x0012C310`) et sa réserve de quatre échantillons.
 *
 * Le moteur l'appelle pour produire un bloc : (objet, nombre d'échantillons,
 * source, destination flottante), retour = nombre produit. Son contrat est un
 * recouvrement de quatre échantillons : il émet d'abord la réserve gardée dans
 * l'objet (index en `+0x2A`, données en `+0x2C`), demande au travailleur de
 * décodage `nombre - index + 4` échantillons, puis garde les quatre derniers
 * pour le bloc suivant.
 *
 * Chez nous, le travailleur (`sub_0012E560`) n'écrit pas ces quatre-là pour le
 * flux musical : la sauvegarde de fin range donc des zéros, l'index reste à 0,
 * et le bloc suivant commence par quatre échantillons muets. L'état s'entretient
 * tout seul, à raison d'un trou par bloc -- c'est le grésillement.
 *
 * En attendant de corriger le travailleur, on remet l'index à 4 (« rien à
 * émettre ») quand la réserve est vide. Le travailleur reçoit alors `nombre`
 * au lieu de `nombre + 4` et tout est consommé : aucun échantillon n'est perdu
 * ni ajouté, seul le recouvrement inutilisé disparaît. Mesure : 1816 trous sur
 * douze secondes avant, 0 après, à niveau RMS identique.
 *
 * `RECOMP_AUDIO_CARRY_FIX=0` le désactive pour retrouver le comportement brut.
 * Les statistiques s'affichent sous `RECOMP_APU_TRACE`.
 */

/* Sonde sur `QueryBootupCheck` (`0x00052990`), sous `RECOMP_SAVE_TRACE`.
 *
 * C'est le contrôle de démarrage du système de sauvegarde : l'interface Flash
 * appelle ce natif par son nom (la table de rappels est enregistrée par
 * `sub_00053700`, à côté de `QuerySave`, `Save`, `Load`, `BootupLoad`...).
 * C'est lui qui décide d'afficher « il manque N blocs », donc l'encadrer dit
 * quels appels noyau il fait et ce qu'il conclut. */
extern void sub_00052990_gen(void);

void sub_00052990(void)
{
    static int traced = -1;

    if (traced < 0)
        traced = getenv("RECOMP_SAVE_TRACE") ? 1 : 0;

    if (!traced) {
        sub_00052990_gen();
        return;
    }

    fprintf(stderr, "  [SAVE] QueryBootupCheck : debut\n");
    fflush(stderr);
    sub_00052990_gen();
    fprintf(stderr, "  [SAVE] QueryBootupCheck : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

/* Mêmes sondes sur les deux autres entrées susceptibles de parler au
 * démarrage. Un seul des trois affichera le dialogue ; c'est celui-là qu'il
 * faudra suivre. */
extern void sub_000524B0_gen(void);
extern void sub_00052EE0_gen(void);

static int save_traced(void)
{
    static int t = -1;
    if (t < 0)
        t = getenv("RECOMP_SAVE_TRACE") ? 1 : 0;
    return t;
}

void sub_000524B0(void)                      /* BootupLoad */
{
    if (!save_traced()) { sub_000524B0_gen(); return; }
    fprintf(stderr, "  [SAVE] BootupLoad : debut\n"); fflush(stderr);
    sub_000524B0_gen();
    fprintf(stderr, "  [SAVE] BootupLoad : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052B60_gen(void);

void sub_00052B60(void)                      /* QuerySave */
{
    if (!save_traced()) { sub_00052B60_gen(); return; }
    fprintf(stderr, "  [SAVE] QuerySave : debut\n"); fflush(stderr);
    sub_00052B60_gen();
    fprintf(stderr, "  [SAVE] QuerySave : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

void sub_00052EE0(void)                      /* LoadGlobal */
{
    if (!save_traced()) { sub_00052EE0_gen(); return; }
    fprintf(stderr, "  [SAVE] LoadGlobal : debut\n"); fflush(stderr);
    sub_00052EE0_gen();
    fprintf(stderr, "  [SAVE] LoadGlobal : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}


/* Sondes générées pour le reste des rappels de sauvegarde (voir la table
 * d'enregistrement du jeu). Toutes sous RECOMP_SAVE_TRACE. */

extern void sub_00052730_gen(void);
void sub_00052730(void)
{
    if (!save_traced()) { sub_00052730_gen(); return; }
    fprintf(stderr, "  [SAVE] ChooseCopySrcCB : debut\n"); fflush(stderr);
    sub_00052730_gen();
    fprintf(stderr, "  [SAVE] ChooseCopySrcCB : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052770_gen(void);
void sub_00052770(void)
{
    if (!save_traced()) { sub_00052770_gen(); return; }
    fprintf(stderr, "  [SAVE] ChooseCopyDst : debut\n"); fflush(stderr);
    sub_00052770_gen();
    fprintf(stderr, "  [SAVE] ChooseCopyDst : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052AB0_gen(void);
void sub_00052AB0(void)
{
    if (!save_traced()) { sub_00052AB0_gen(); return; }
    fprintf(stderr, "  [SAVE] ChooseCopyDstCB : debut\n"); fflush(stderr);
    sub_00052AB0_gen();
    fprintf(stderr, "  [SAVE] ChooseCopyDstCB : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052AF0_gen(void);
void sub_00052AF0(void)
{
    if (!save_traced()) { sub_00052AF0_gen(); return; }
    fprintf(stderr, "  [SAVE] ConfirmSaveCB : debut\n"); fflush(stderr);
    sub_00052AF0_gen();
    fprintf(stderr, "  [SAVE] ConfirmSaveCB : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052E70_gen(void);
void sub_00052E70(void)
{
    if (!save_traced()) { sub_00052E70_gen(); return; }
    fprintf(stderr, "  [SAVE] ChooseSaveDstCB : debut\n"); fflush(stderr);
    sub_00052E70_gen();
    fprintf(stderr, "  [SAVE] ChooseSaveDstCB : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052B40_gen(void);
void sub_00052B40(void)
{
    if (!save_traced()) { sub_00052B40_gen(); return; }
    fprintf(stderr, "  [SAVE] ConfirmLoadCB : debut\n"); fflush(stderr);
    sub_00052B40_gen();
    fprintf(stderr, "  [SAVE] ConfirmLoadCB : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052860_gen(void);
void sub_00052860(void)
{
    if (!save_traced()) { sub_00052860_gen(); return; }
    fprintf(stderr, "  [SAVE] ConfirmCardRemovedCB : debut\n"); fflush(stderr);
    sub_00052860_gen();
    fprintf(stderr, "  [SAVE] ConfirmCardRemovedCB : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_000528B0_gen(void);
void sub_000528B0(void)
{
    if (!save_traced()) { sub_000528B0_gen(); return; }
    fprintf(stderr, "  [SAVE] DeleteID : debut\n"); fflush(stderr);
    sub_000528B0_gen();
    fprintf(stderr, "  [SAVE] DeleteID : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052D70_gen(void);
void sub_00052D70(void)
{
    if (!save_traced()) { sub_00052D70_gen(); return; }
    fprintf(stderr, "  [SAVE] GetNumUsers : debut\n"); fflush(stderr);
    sub_00052D70_gen();
    fprintf(stderr, "  [SAVE] GetNumUsers : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052F90_gen(void);
void sub_00052F90(void)
{
    if (!save_traced()) { sub_00052F90_gen(); return; }
    fprintf(stderr, "  [SAVE] SetNewUserID : debut\n"); fflush(stderr);
    sub_00052F90_gen();
    fprintf(stderr, "  [SAVE] SetNewUserID : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052CE0_gen(void);
void sub_00052CE0(void)
{
    if (!save_traced()) { sub_00052CE0_gen(); return; }
    fprintf(stderr, "  [SAVE] GetUserData : debut\n"); fflush(stderr);
    sub_00052CE0_gen();
    fprintf(stderr, "  [SAVE] GetUserData : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052D10_gen(void);
void sub_00052D10(void)
{
    if (!save_traced()) { sub_00052D10_gen(); return; }
    fprintf(stderr, "  [SAVE] PopupResponse : debut\n"); fflush(stderr);
    sub_00052D10_gen();
    fprintf(stderr, "  [SAVE] PopupResponse : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052E40_gen(void);
void sub_00052E40(void)
{
    if (!save_traced()) { sub_00052E40_gen(); return; }
    fprintf(stderr, "  [SAVE] Save : debut\n"); fflush(stderr);
    sub_00052E40_gen();
    fprintf(stderr, "  [SAVE] Save : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052E60_gen(void);
void sub_00052E60(void)
{
    if (!save_traced()) { sub_00052E60_gen(); return; }
    fprintf(stderr, "  [SAVE] QueryLoad : debut\n"); fflush(stderr);
    sub_00052E60_gen();
    fprintf(stderr, "  [SAVE] QueryLoad : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052ED0_gen(void);
void sub_00052ED0(void)
{
    if (!save_traced()) { sub_00052ED0_gen(); return; }
    fprintf(stderr, "  [SAVE] Load : debut\n"); fflush(stderr);
    sub_00052ED0_gen();
    fprintf(stderr, "  [SAVE] Load : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052930_gen(void);
void sub_00052930(void)
{
    if (!save_traced()) { sub_00052930_gen(); return; }
    fprintf(stderr, "  [SAVE] ConfirmLoadGlobalCB : debut\n"); fflush(stderr);
    sub_00052930_gen();
    fprintf(stderr, "  [SAVE] ConfirmLoadGlobalCB : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_000528E0_gen(void);
void sub_000528E0(void)
{
    if (!save_traced()) { sub_000528E0_gen(); return; }
    fprintf(stderr, "  [SAVE] SetCurrentUserIndex : debut\n"); fflush(stderr);
    sub_000528E0_gen();
    fprintf(stderr, "  [SAVE] SetCurrentUserIndex : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052470_gen(void);
void sub_00052470(void)
{
    if (!save_traced()) { sub_00052470_gen(); return; }
    fprintf(stderr, "  [SAVE] SetBMCurrentUserIndex : debut\n"); fflush(stderr);
    sub_00052470_gen();
    fprintf(stderr, "  [SAVE] SetBMCurrentUserIndex : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052820_gen(void);
void sub_00052820(void)
{
    if (!save_traced()) { sub_00052820_gen(); return; }
    fprintf(stderr, "  [SAVE] BootupLoadCB : debut\n"); fflush(stderr);
    sub_00052820_gen();
    fprintf(stderr, "  [SAVE] BootupLoadCB : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052E20_gen(void);
void sub_00052E20(void)
{
    if (!save_traced()) { sub_00052E20_gen(); return; }
    fprintf(stderr, "  [SAVE] CreateNewUser : debut\n"); fflush(stderr);
    sub_00052E20_gen();
    fprintf(stderr, "  [SAVE] CreateNewUser : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052DE0_gen(void);
void sub_00052DE0(void)
{
    if (!save_traced()) { sub_00052DE0_gen(); return; }
    fprintf(stderr, "  [SAVE] MCOPComplete : debut\n"); fflush(stderr);
    sub_00052DE0_gen();
    fprintf(stderr, "  [SAVE] MCOPComplete : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052F30_gen(void);
void sub_00052F30(void)
{
    if (!save_traced()) { sub_00052F30_gen(); return; }
    fprintf(stderr, "  [SAVE] MCOPComplete2 : debut\n"); fflush(stderr);
    sub_00052F30_gen();
    fprintf(stderr, "  [SAVE] MCOPComplete2 : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052EF0_gen(void);
void sub_00052EF0(void)
{
    if (!save_traced()) { sub_00052EF0_gen(); return; }
    fprintf(stderr, "  [SAVE] RetryChooseMCDeviceCB : debut\n"); fflush(stderr);
    sub_00052EF0_gen();
    fprintf(stderr, "  [SAVE] RetryChooseMCDeviceCB : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052F00_gen(void);
void sub_00052F00(void)
{
    if (!save_traced()) { sub_00052F00_gen(); return; }
    fprintf(stderr, "  [SAVE] WantAutosaveFailedCB : debut\n"); fflush(stderr);
    sub_00052F00_gen();
    fprintf(stderr, "  [SAVE] WantAutosaveFailedCB : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_00052F10_gen(void);
void sub_00052F10(void)
{
    if (!save_traced()) { sub_00052F10_gen(); return; }
    fprintf(stderr, "  [SAVE] DeleteScreen : debut\n"); fflush(stderr);
    sub_00052F10_gen();
    fprintf(stderr, "  [SAVE] DeleteScreen : fin, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

extern void sub_0012C310_gen(void);

void sub_0012C310(void)
{
    static int fix = -1;
    static int traced = -1;
    uint32_t obj  = MEM32(g_esp + 4);
    uint32_t want = MEM32(g_esp + 8);
    uint32_t dst  = MEM32(g_esp + 0x10);
    int32_t carry = 0;
    uint32_t c0 = 0, c1 = 0;
    int readable = dsp_readable(obj + 0x3C);

    /* Off by default since point 78. This was the workaround for the missing
     * four-sample overlap: with an empty carry it set the index to 4, so the
     * worker was asked for `count` instead of `count + 4` and the unused
     * overlap simply vanished. The real cause is fixed now -- the lifter did
     * not translate pushal/popal, and this function's worker is built around
     * that pair -- so leaving it on would suppress an overlap the decoder now
     * produces correctly, and quietly falsify every measurement the [DEC]
     * probe below reports.
     *
     * Kept as opt-in rather than deleted: RECOMP_AUDIO_CARRY_FIX=1 puts the
     * old behaviour back, which is the cheapest way to tell a fresh overlap
     * problem from this one if the audio chain misbehaves again. */
    if (fix < 0) {
        const char *s = getenv("RECOMP_AUDIO_CARRY_FIX");
        fix = (s && *s) ? (atoi(s) != 0) : 0;
    }
    if (traced < 0)
        traced = getenv("RECOMP_APU_TRACE") ? 1 : 0;

    if (readable) {
        carry = (int32_t)(int16_t)(uint16_t)(MEM32(obj + 0x28) >> 16);
        c0 = MEM32(obj + 0x2C);
        c1 = MEM32(obj + 0x30);

        if (fix && carry == 0 && c0 == 0 && c1 == 0) {
            uint32_t w = MEM32(obj + 0x28);
            MEM32(obj + 0x28) = (w & 0x0000FFFFu) | (4u << 16);
        }
    }

    sub_0012C310_gen();

    if (traced) {
        static uint32_t next_tick;
        static unsigned calls, head_zero, overlap_missing;
        uint32_t got = g_eax;
        uint32_t now;

        /* Une source muette rend un bloc de zéros : elle noierait le compte. */
        if (dst && got > 16 && got <= 4096 && dsp_readable(dst + got * 4 + 12)) {
            uint32_t i, nonzero = 0;
            for (i = 0; i < got; i++)
                if ((MEM32(dst + i * 4) & 0x7FFFFFFFu) != 0)
                    nonzero++;
            if (nonzero * 4 > got * 3) {
                calls++;
                if (((MEM32(dst) & 0x7FFFFFFFu) == 0) &&
                    ((MEM32(dst + 4) & 0x7FFFFFFFu) == 0) &&
                    ((MEM32(dst + 8) & 0x7FFFFFFFu) == 0) &&
                    ((MEM32(dst + 12) & 0x7FFFFFFFu) == 0))
                    head_zero++;
                if (((MEM32(dst + got * 4) & 0x7FFFFFFFu) == 0) &&
                    ((MEM32(dst + got * 4 + 4) & 0x7FFFFFFFu) == 0) &&
                    ((MEM32(dst + got * 4 + 8) & 0x7FFFFFFFu) == 0) &&
                    ((MEM32(dst + got * 4 + 12) & 0x7FFFFFFFu) == 0))
                    overlap_missing++;
            }
        }

        now = (uint32_t)GetTickCount64();
        if (now >= next_tick) {
            next_tick = now + 1000;
            fprintf(stderr, "  [DEC] %u blocs sonores/s, %u commencent par"
                    " quatre zéros, %u sans recouvrement ; dernier : objet"
                    " 0x%08X, demandé %u, rendu %u, réserve %d (%08X %08X)\n",
                    calls, head_zero, overlap_missing, obj, want, g_eax,
                    carry, c0, c1);
            fflush(stderr);
            calls = head_zero = overlap_missing = 0;
        }
    }
}

extern void sub_0012BE20_gen(void);

void sub_0012BE20(void)
{
    static int traced = -1;

    if (traced < 0)
        traced = getenv("RECOMP_APU_TRACE") ? 1 : 0;

    if (traced) {
        uint32_t count = MEM32(g_esp + 8);
        uint32_t src   = MEM32(g_esp + 0xC);
        static unsigned holes, blocks, at_head, at_tail, in_middle;
        static unsigned head_len, mid_len;
        static uint32_t next_tick;
        uint32_t now = (uint32_t)GetTickCount64();

        if (count > 0 && count <= 4096 && src && dsp_readable(src + count * 4 - 4)) {
            unsigned run = 0, i, nonzero = 0;

            for (i = 0; i < count; i++)
                if ((MEM32(src + i * 4) & 0x7FFFFFFFu) != 0)
                    nonzero++;

            /* Un bloc muet n'est pas un trou : on ne regarde que ceux qui
             * portent vraiment du son. */
            if (nonzero * 4 > count * 3) {
                blocks++;
                for (i = 0; i < count; i++) {
                    uint32_t bits = MEM32(src + i * 4);
                    if ((bits & 0x7FFFFFFFu) == 0) {
                        run++;
                    } else {
                        if (run >= 3) {
                            holes++;
                            if (i - run == 0) {
                                at_head++;
                                head_len = run;   /* sa longueur nomme l'étage */
                            } else {
                                in_middle++;
                                mid_len = run;
                            }
                        }
                        run = 0;
                    }
                }
                if (run >= 3) {          /* le bloc se termine sur des zéros */
                    holes++;
                    at_tail++;
                }
            }
        }

        if (now >= next_tick) {
            next_tick = now + 1000;
            if (blocks)
                fprintf(stderr, "  [MIX] %u blocs/s, %u trous : %u au début"
                        " (long. %u), %u au milieu (long. %u), %u en fin"
                        " (bloc de %u échantillons)\n",
                        blocks, holes, at_head, head_len, in_middle, mid_len,
                        at_tail, count);
            fflush(stderr);
            blocks = holes = at_head = in_middle = at_tail = 0;
            head_len = mid_len = 0;
        }
    }

    sub_0012BE20_gen();
}

/* Call a guest function pointer the way generated code does: push the return
 * address the original would have pushed, then let the callee's own `ret`
 * consume it. A target nothing can translate is skipped rather than jumped
 * to, which is what the generated dispatch does too. */
static void guest_call(uint32_t target, uint32_t ret_va)
{
    recomp_func_t fn;

    if (!target)
        return;
    fn = recomp_lookup_manual_fwd(target);
    if (!fn)
        fn = recomp_lookup(target);
    if (!fn) {
        recomp_icall_fail_log(target);
        return;
    }
    PUSH32(g_esp, ret_va);
    fn();
}

void sub_002091E3(void)
{
    uint32_t arg = MEM32(g_esp + 4);      /* the single stack argument */

    if (arg != 0) {
        guest_call(MEM32(0x002FA244), 0x002091F5u);
    } else {
        guest_call(MEM32(0x002FA248), 0x00209202u);
        PUSH32(g_esp, 0);                  /* push 0 */
        PUSH32(g_esp, 0x00209209u);        /* call 0x00203C67 */
        sub_00203C67();
        g_esp += 4;                        /* pop ecx */
    }
    g_esp += 4 + 4;                        /* ret 4 */
}

/* Bring-up probe on the game's screen-state manager at 0x00077040.
 *
 * Its state lives at [this+0x24] and the screen it is showing at [this+0x18];
 * the transition out of a screen happens when that screen's vtable slot +0x14
 * ("am I finished?") returns true. The title sits on its loading screen
 * forever, so the question is which screen object that is and which method is
 * answering no. Logged once a second under RECOMP_STATE_TRACE, which keeps it
 * out of a normal run entirely.
 */
extern void sub_00077040_gen(void);
extern void xbox_FramebufferWindowCountTick(void);

/* Unlock every character and venue, under RECOMP_UNLOCK_ALL.
 *
 * The title carries the code already: sub_000158F0 walks its character and
 * venue collections, adds every entry to the profile's unlocked lists and
 * sets the reward points to 10000. The cheat screen cannot reach it -- the
 * code that recognises typed cheats (sub_000493D0) only knows the sixteen
 * music codes and the five point codes, and BIGBANG, MSGS and LQA are
 * compared in a branch the retail front end never takes.
 *
 * It needs a loaded profile: the front end's object lives at *(0x002FB1A8)
 * (its registry is at 0x002FB148, entry 0x18), the profile at +4 and the
 * collection database at +8. This runs from the screen-state probe, on the
 * game's own thread, once both are there -- and again on F7. Written into the
 * save by the title's own autosave.
 *
 * And again, by itself, whenever a profile is read from a save. The unlocked
 * lists live in the profile object (characters at +0x20, count at +0x24;
 * venues at +0x0C, count at +0x10), and choosing a profile at "SELECT USER ID"
 * replaces them with what the save holds. A character added by a mod is in no
 * save, so it came back locked until F7 -- measured (point 106): unlock at
 * boot, profile read, the selection grid built from the profile's list, the
 * newcomer at the end among the locked ones. The probe remembers how long our
 * unlock left each list and runs it again as soon as one is shorter, which is
 * well before the grid is built (the grid is a screen later).
 */
extern void sub_000158F0(void);
extern void sub_00012ED0(void);

/* Calling into the title from a probe clobbers the guest registers, and the
 * probe's own caller is still using them -- sub_00077040 is thiscall, so a
 * clobbered ecx sent the screen manager off a cliff one instruction later.
 * Everything is saved and put back. */
typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
} GuestRegs;

static void guest_regs_save(GuestRegs *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx; r->ebx = g_ebx;
    r->esp = g_esp; r->ebp = g_ebp; r->esi = g_esi; r->edi = g_edi;
}

static void guest_regs_restore(const GuestRegs *r)
{
    g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_ebx = r->ebx;
    g_esp = r->esp; g_ebp = r->ebp; g_esi = r->esi; g_edi = r->edi;
}


/* Is the collection with this name hash loaded? Calls the title's own lookup
 * (thiscall: the database in ecx, an iterator to fill and the hash on the
 * stack) on a scratch area of the guest stack. */
static int unlock_collection_ready(uint32_t db, uint32_t hash)
{
    uint32_t out, ok = 0;
    GuestRegs regs;

    guest_regs_save(&regs);

    g_esp -= 0x100;                    /* scratch above whatever the call uses */
    out = g_esp;
    MEM32(out + 4) = 0xFFFFFFFFu;
    GUARD_BEGIN
        PUSH32(g_esp, hash);
        PUSH32(g_esp, out);
        g_ecx = db;
        PUSH32(g_esp, 0);              /* dummy return address */
        sub_00012ED0();
        ok = MEM32(out + 4) != 0xFFFFFFFFu;
    GUARD_FAULT
        ok = 0;
    GUARD_END
    guest_regs_restore(&regs);
    return ok;
}

/* The profile object, or 0 while the front end has none. */
static uint32_t unlock_profile(void)
{
    uint32_t fe;
    if (!dsp_readable(0x002FB1A8u))
        return 0;
    fe = MEM32(0x002FB1A8u);
    if (!fe || !dsp_readable(fe + 4))
        return 0;
    return MEM32(fe + 4);
}

/* La liste des personnages debloques du profil n'a que 80 places (mesure le 2026-10-15 dans
 * Options : elle va des octets 4896 a 5215, suivie d'un bloc de 32 octets puis d'une liste de
 * 32 entiers et de son compte a 5396). sub_00016810 insere sans controle : avec les 44
 * personnages de Vendetta (121 en tout), le deblocage ecrivait 41 poignees par-dessus ces
 * donnees -- 9 personnages restaient bloques et le combat suivant se figeait au chargement.
 *
 * 1. Un personnage AJOUTE (rang >= 75 dans l'index des cles, poignee 0x07xxxxxx) est toujours
 *    debloque : sub_00017000 le trouve, donc personne ne l'insere dans la liste.
 * 2. Un profil deja deborde est repare une fois charge : compte ramene a 80, et la fin du bloc
 *    de sauvegarde (octets 5216 a 5403) remise a la valeur des sauvegardes saines (identique
 *    dans celles du 22 septembre et du point 105). L'autosave la reecrit, signature comprise. */
#define PROFIL_PLACES     80u
#define PROFIL_ORIGINAUX  75u
#define PROFIL_LISTE      4896u       /* decalage de la liste dans Options */
#define PROFIL_COMPTE     4772u
#define PROFIL_QUEUE      5216u
static const uint32_t profil_queue_saine[47] = {
    0x45411131, 0x2d514d49, 0x0d395955, 0x25015d1d, 0x69156561, 0x213d6d09, 0x71352905, 0x7d797519,
    0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31,
    0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x20, 0x101 };

static int personnage_ajoute(uint32_t h)
{
    return (h & 0xff000000u) == 0x07000000u && (h & 0x007fffffu) >= PROFIL_ORIGINAUX;
}

static void profil_reparer_tick(void)
{
    static int signale;
    uint32_t p = unlock_profile(), n, data, pc, base, k;
    if (!p || !dsp_readable(p + 0x2c))
        return;
    n = MEM32(p + 0x24);
    if (n <= PROFIL_PLACES)
        return;
    data = MEM32(p + 0x2c);
    pc = MEM32(p + 0x28);
    base = data - PROFIL_LISTE;
    if (!dsp_readable(base) || !dsp_readable(base + PROFIL_QUEUE + sizeof profil_queue_saine - 4)
        || pc != base + PROFIL_COMPTE) {
        if (!signale++) {
            fprintf(stderr, "  [PROFIL] %u personnages debloques, mais la liste n'est pas a sa place"
                    " (donnees 0x%08X, compte 0x%08X) : rien repare\n", n, data, pc);
            fflush(stderr);
        }
        return;
    }
    /* (Une premiere version cherchait aussi dans le profil une copie du compte de la liste
     * suivante, sur une supposition : elle a ecrase un pointeur a +0x48 et le jeu a plante.
     * On ne touche qu'au bloc de sauvegarde, mesure.) */
    for (k = 0; k < sizeof profil_queue_saine / 4; k++)
        MEM32(base + PROFIL_QUEUE + 4 * k) = profil_queue_saine[k];
    MEM32(p + 0x24) = PROFIL_PLACES;
    MEM32(pc) = PROFIL_PLACES;
    fprintf(stderr, "  [PROFIL] liste des debloques ramenee de %u a %u places, fin de la sauvegarde"
            " restauree (octets %u a %u)\n", n, PROFIL_PLACES, PROFIL_QUEUE,
            PROFIL_QUEUE + (unsigned)sizeof profil_queue_saine - 1);
    fflush(stderr);
}

/* La recherche dichotomique des listes du profil (base, nombre, &poignee) : un personnage
 * ajoute est toujours trouve, sur une copie de sa poignee. */
extern uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);
extern void sub_00017000_gen(void);
void sub_00017000(void)
{
    static uint32_t copie;
    uint32_t pp = MEM32(g_esp + 12);
    uint32_t h = (pp && dsp_readable(pp)) ? MEM32(pp) : 0;
    sub_00017000_gen();
    if (g_eax == 0 && personnage_ajoute(h)) {
        if (!copie)
            copie = xbox_ContiguousAlloc(16, 16);
        if (copie) {
            MEM32(copie) = h;
            g_eax = copie;
        }
    }
}

/* L'entree et la victoire d'un coreID clone (2026-10-15). sub_00087AF0 choisit l'animation
 * d'entree / de victoire d'un joueur : si le coreID du joueur (reglages du combat, this+0x2FF5C,
 * +0xA8 + joueur x 4) vaut 87 (0x57) ou plus, il le prend pour le heros du mode Story et tire une
 * entree 'HERO' au hasard, sinon il prend celle du modele (IntroExtro.xml). Mesure en jeu : Drake
 * (coreID 78) joue son entree, Opal et DMX (coreID 97 et 91) celles du heros. Les clones de
 * coreid.txt vont jusqu'a 99 : pendant l'appel, le coreID d'un clone est remplace par celui de son
 * donneur (sous 87), puis remis. Le heros, absent de coreid.txt, n'est pas touche.
 *
 * Les coreID 86 a 141 sont les jeux de coups du heros, un par combinaison de styles (point 177) :
 * depuis le 30 octobre, les clones vont au-dela, 142 a CORE_MAX - 1. Les coreID du jeu restent
 * 0 a 141 ; au-dela, il n'y a que des clones de coreid.txt. */
#define CORE_JEU 142                 /* coreID 0 a 141 : main_all.dat et wres_all d'origine */
#define CORE_MAX 256
int g_core_donneur[CORE_MAX];
int g_core_voix[CORE_MAX];

/* La table des voix (0x2DC5D0, 12 octets par coreID) n'a que 104 entrees, et ce qui la suit sert
 * ailleurs (2026-10-27). Au premier appel de sub_0003E170, son seul lecteur, elle est recopiee avec
 * CORE_MAX entrees -- les 142 premieres avec les octets d'apres compris, le heros (141) lit donc ce
 * qu'il lisait ; au-dela, l'entree 0 (inutilisee, voix -1) -- et les clones 104 et plus de
 * coreid.txt y recoivent l'entree de leur donneur et leur voix. La copie de sub_0003E170
 * (patch_gen.py) lit g_voix_va au lieu de 0x2DC5D0. */
uint32_t g_voix_va = 0x2DC5D0u;
extern void sub_0003E170_gen(void);
extern void sub_0003E170_patch(void);
void sub_0003E170(void)
{
    static int fait;
    if (!fait) {
        uint32_t va = xbox_ContiguousAlloc(12 * CORE_MAX, 16);
        int c;
        fait = 1;
        if (va) {
            memcpy((void *)XBOX_PTR(va), (void *)XBOX_PTR(0x2DC5D0u), 12 * CORE_JEU);
            for (c = CORE_JEU; c < CORE_MAX; c++)
                memcpy((void *)XBOX_PTR(va + 12 * c), (void *)XBOX_PTR(va), 12);
            for (c = 104; c < CORE_MAX; c++) {
                if (g_core_donneur[c] < 0)
                    continue;
                memcpy((void *)XBOX_PTR(va + 12 * c), (void *)XBOX_PTR(va + 12 * g_core_donneur[c]), 12);
                MEM32(va + 12 * c) = (uint32_t)g_core_voix[c];
            }
            g_voix_va = va;
        }
    }
    sub_0003E170_patch();
}

/* Les paires WPAR / WWAZ de main_all.dat au-dela de 141 (2026-10-30). sub_001A47B0 (eax = le
 * fichier) range les blocs du fichier, qui n'a pas de compte : six blocs, puis 141 paires dans un
 * tableau statique (0x3B8648, 16 octets par coreID - 1 : taille et adresse du WPAR, puis du WWAZ),
 * borne fixe 0x3B8F18 ; ce qui suit sert ailleurs. Ses lecteurs (sub_001A7F30, sub_001B2090,
 * sub_001B4930, sub_001B4BA0) indexent par le coreID - 1 du combattant (+0x2C) x 16. Ici : le
 * tableau recopie dans g_paires_va, CORE_MAX - 1 places, completees par les paires qui suivent la
 * 141e dans le fichier du mod ; les lecteurs le lisent la (patch_gen.py). Une place sans paire
 * garde celle du coreID 1 : pas d'adresse nulle. */
uint32_t g_paires_va = 0x3B8648u;
extern void sub_001A47B0_gen(void);
void sub_001A47B0(void)
{
    uint32_t p = g_eax, n, k;
    static uint32_t va;
    static int note;
    sub_001A47B0_gen();
    if (!va) {
        va = xbox_ContiguousAlloc(16 * (CORE_MAX - 1), 16);
        if (!va)
            return;
    }
    memcpy((void *)XBOX_PTR(va), (void *)XBOX_PTR(0x3B8648u), 16 * (CORE_JEU - 1));
    for (k = 0; k < 6 + 2 * (CORE_JEU - 1); k++)
        p += 8 + MEM32(p + 4);                     /* les blocs deja ranges par le jeu */
    n = CORE_JEU - 1;
    while (n < CORE_MAX - 1 && MEM32(p) == 0x52415057u /* WPAR */
           && MEM32(p + 8 + MEM32(p + 4)) == 0x5A415757u /* WWAZ */) {
        uint32_t q = p + 8 + MEM32(p + 4);
        MEM32(va + 16 * n) = MEM32(p + 4);
        MEM32(va + 16 * n + 4) = p + 8;
        MEM32(va + 16 * n + 8) = MEM32(q + 4);
        MEM32(va + 16 * n + 12) = q + 8;
        p = q + 8 + MEM32(q + 4);
        n++;
    }
    for (k = n; k < CORE_MAX - 1; k++)
        memcpy((void *)XBOX_PTR(va + 16 * k), (void *)XBOX_PTR(va), 16);
    g_paires_va = va;
    if (!note || n > CORE_JEU - 1) {
        fprintf(stderr, "  [MODS] main_all.dat : %u paires WPAR/WWAZ (coreID 1 a %u)\n", n, n);
        note = 1;
    }
}
extern void sub_001A7F30_gen(void);
extern void sub_001B2090_gen(void);
extern void sub_001B4930_gen(void);
extern void sub_001B4BA0_gen(void);
extern void sub_001A7F30_patch(void);
extern void sub_001B2090_patch(void);
extern void sub_001B4930_patch(void);
extern void sub_001B4BA0_patch(void);
void sub_001A7F30(void) { sub_001A7F30_patch(); }
void sub_001B2090(void) { sub_001B2090_patch(); }
void sub_001B4930(void) { sub_001B4930_patch(); }
void sub_001B4BA0(void) { sub_001B4BA0_patch(); }
/* L'activation du Blazin de Vendetta (2026-10-27). La cinematique d'activation de FFNY est la meme
 * pour tous : assets/Blazin.xml (nis.viv), 4 plans BT1_01, BT2_0x, BT3_0x, BT4C01. Chaque plan est
 * une requete du sous-systeme d'entree/victoire (objet de 0x1AC octets) : sub_00087AF0 la cree avec
 * la fiche du plan (joueur en +4), FUN_000a3e20 lit le XML (etat +0x19C = 4 pour Blazin.xml) et y
 * range le nom du plan (+0x61) et le nombre de plans (+0xD0) ; le numero du plan est en +0xD4 ;
 * FUN_0008b120 enchaine les plans tant que leur compte est sous +0xD0. mods\blazin_activation.txt
 * (<coreID> <archive>, outils de defjam_vendetta : activation_ffny.py) donne a un coreID son propre
 * plan unique : son archive (assets/v2ta/<archive>.viv, corps + cameras) au premier plan, et un
 * seul plan. */
#define ACTIVATIONS_MAX 64
static int activ_core[ACTIVATIONS_MAX];
static char activ_nom[ACTIVATIONS_MAX][16];
static char activ_fin[ACTIVATIONS_MAX][16];  /* archive des plans 2 a 4 (la derniere image, tenue) */
static int activ_n = -1;
static uint32_t activ_req[32];               /* requete -> coreID du joueur (sub_00087AF0) */
static uint32_t activ_req_core[32];
static char activ_req_nom[32][16];           /* l'archive Vendetta qu'on y a posee ("" : celle du jeu) */
static char activ_req_orig[32][3][32];       /* les noms du jeu : archive +0x61, corps, camera +0x1D */

static void activ_lire(void)
{
    FILE *f;
    char l[128];
    activ_n = 0;
    f = fopen("game_files\\mods\\blazin_activation.txt", "r");
    if (!f)
        return;
    while (fgets(l, sizeof l, f) && activ_n < ACTIVATIONS_MAX) {
        int c;
        char nom[16], fin[16] = "";
        if (l[0] == '#' || sscanf(l, "%d %15s %15s", &c, nom, fin) < 2)
            continue;
        activ_core[activ_n] = c;
        strcpy(activ_nom[activ_n], nom);
        strcpy(activ_fin[activ_n], fin);
        activ_n++;
    }
    fclose(f);
    fprintf(stderr, "  [BLAZIN] %d activation(s) propre(s) (blazin_activation.txt)\n", activ_n);
}

static void activ_noter(uint32_t req, uint32_t core)
{
    /* circulaire : chaque combat cree ses requetes (une dizaine par joueur) a des adresses neuves */
    static int suivante;
    int i;
    for (i = 0; i < 32; i++) {
        if (activ_req[i] == req) {
            activ_req_core[i] = core;
            activ_req_nom[i][0] = 0;      /* recreee : sub_000A3E20 y reposera l'archive voulue */
            return;
        }
    }
    activ_req[suivante] = req;
    activ_req_core[suivante] = core;
    activ_req_nom[suivante][0] = 0;
    activ_req_orig[suivante][0][0] = 0;
    suivante = (suivante + 1) % 32;
}

/* Les trois noms d'une requete de plan : archive (+0x61), corps (1er de la liste +0x90), camera
 * d'arene du plan 4 (+0x1D, vide sinon). lire : copie vers t ; sinon ecrit t (les vides sautes). */
static void activ_noms(uint32_t req, char t[3][32], int lire)
{
    char *p[3];
    int n;
    p[0] = (char *)XBOX_PTR(req + 0x61u);
    p[1] = (MEM32(req + 0x90u) && dsp_readable(MEM32(req + 0x90u))) ? (char *)XBOX_PTR(MEM32(req + 0x90u)) : NULL;
    p[2] = (char *)XBOX_PTR(req + 0x1Du);
    for (n = 0; n < 3; n++) {
        if (!p[n])
            continue;
        if (lire) {
            strncpy(t[n], p[n], 31);
            t[n][31] = 0;
        } else if (t[n][0] && (n < 2 || p[n][0]))
            strcpy(p[n], t[n]);
    }
}

/* L'archive Vendetta voulue pour un coreID et un numero de plan ("" : les plans du jeu). */
static const char *activ_voulu(uint32_t core, uint32_t plan)
{
    int j;
    if (activ_n < 0)
        activ_lire();
    for (j = 0; j < activ_n; j++)
        if ((uint32_t)activ_core[j] == core)
            return plan == 0 ? activ_nom[j] : activ_fin[j];
    return "";
}

extern void sub_000A3E20_gen(void);
void sub_000A3E20(void)
{
    uint32_t req = g_ecx;
    int i, j;
    sub_000A3E20_gen();
    if (!(g_eax & 0xFF) || !dsp_readable(req + 0x19Cu) || MEM32(req + 0x19Cu) != 4u)
        return;
    fprintf(stderr, "  [BLAZIN] plan %u / %u : %s (requete %08X, liste %s)\n", MEM32(req + 0xD4u) + 1,
            MEM32(req + 0xD0u), (char *)XBOX_PTR(req + 0x61u), req,
            MEM32(req + 0x90u) ? (char *)XBOX_PTR(MEM32(req + 0x90u)) : "-");
    if (activ_n < 0)
        activ_lire();
    for (i = 0; i < 32; i++)
        if (activ_req[i] == req)
            activ_noms(req, activ_req_orig[i], 1);       /* les noms du XML, pour les remettre */
    if (MEM32(req + 0xD4u) != 0) {
        /* Mesure du 27 octobre (combat automatique, journal) : a l'activation le jeu cree d'un coup
         * une requete par variante (plan 1, les 4 variantes du 2 et du 3, le 4) et enchaine les 4
         * plans quel que soit leur nombre (+0xD0). Les plans 2 a 4 prennent donc l'archive de fin
         * (la derniere image du plan unique, 2 images) : archive (+0x61), corps (+0x90) et camera
         * d'arene du plan 4 (+0x1D, chargee a part quand le drapeau 8 est leve). */
        for (i = 0; i < 32; i++)
            if (activ_req[i] == req)
                for (j = 0; j < activ_n; j++)
                    if ((uint32_t)activ_core[j] == activ_req_core[i] && activ_fin[j][0]) {
                        strcpy((char *)XBOX_PTR(req + 0x61u), activ_fin[j]);
                        if (MEM32(req + 0x90u) && dsp_readable(MEM32(req + 0x90u)))
                            strcpy((char *)XBOX_PTR(MEM32(req + 0x90u)), activ_fin[j]);
                        if (MEM8(req + 0x1Du))
                            strcpy((char *)XBOX_PTR(req + 0x1Du), activ_fin[j]);
                        strcpy(activ_req_nom[i], activ_fin[j]);
                    }
        return;
    }
    for (i = 0; i < 32; i++) {
        if (activ_req[i] != req)
            continue;
        for (j = 0; j < activ_n; j++) {
            if ((uint32_t)activ_core[j] == activ_req_core[i]) {
                char *nom = (char *)XBOX_PTR(req + 0x61u);
                fprintf(stderr, "  [BLAZIN] activation du coreID %u : %s au lieu de %s, 1 plan au lieu de %u\n",
                        activ_req_core[i], activ_nom[j], nom, MEM32(req + 0xD0u));
                strcpy(nom, activ_nom[j]);
                /* le nom de l'animation du corps : 1er personnage de la liste en +0x90 (0x68 octets
                 * chacun, FUN_000a3160) ; reste BT1_01, le corps n'etait pas trouve dans l'archive
                 * (position de repos, essai du 27 octobre) */
                if (MEM32(req + 0x90u) && dsp_readable(MEM32(req + 0x90u)))
                    strcpy((char *)XBOX_PTR(MEM32(req + 0x90u)), activ_nom[j]);
                MEM32(req + 0xD0u) = 1;
                strcpy(activ_req_nom[i], activ_nom[j]);
                return;
            }
        }
        return;
    }
}

/* Le plan applique au combattant qui active (FUN_00087e20(requete, fiche) : fiche[1] le joueur,
 * fiche[3] = 2 pour le Blazin, fiche[5] l'etape). Mesure du 27 octobre (combat automatique,
 * captures) : les requetes d'activation sont preparees une fois, en debut de combat, pour le joueur
 * 1, puis jouees sur celui qui active -- le CPU a joue l'activation Vendetta du joueur 1. Si
 * l'archive preparee n'est pas celle du combattant qui active, l'etat de chargement de la requete
 * est remis a zero (archive +0x5C, chargee +0x60, prete +0x81, camera d'arene +0x18 / +0x1C / +0x84 :
 * les champs que FUN_000a3fa0 initialise) avant de laisser le jeu continuer : FUN_000a3e80, rappele
 * a chaque image par l'enchaineur tant que le plan n'est pas pret, relit le XML (sub_000A3E20
 * choisit alors selon le coreID de celui qui active), rouvre l'archive et attend son chargement.
 * (Premier essai, fiche[5] remise a 0 pour recreer la requete : le jeu se figeait.) */
extern void sub_00087E20_gen(void);
extern void sub_000A3E80(void);
extern void sub_000A3130(void);
void sub_00087E20(void)
{
    uint32_t req = MEM32(g_esp + 4), fiche = MEM32(g_esp + 8);
    if (dsp_readable(fiche + 20) && MEM32(fiche + 12) == 2u && dsp_readable(req + 0xD4u)) {
        uint32_t joueur = MEM32(fiche + 4);
        uint32_t etat = dsp_readable(g_ecx + 0x2FF5Cu) ? MEM32(g_ecx + 0x2FF5Cu) : 0;
        if (etat && joueur < 8 && dsp_readable(etat + 0xA8u + 4u * joueur)) {
            uint32_t core = MEM32(etat + 0xA8u + 4u * joueur);
            const char *voulu = activ_voulu(core, MEM32(req + 0xD4u)), *pose = "";
            int i, k = -1;
            for (i = 0; i < 32; i++)
                if (activ_req[i] == req) {
                    pose = activ_req_nom[i];
                    k = i;
                }
            if (strcmp(voulu, pose) != 0) {
                fprintf(stderr, "  [BLAZIN] plan %u prepare avec '%s', active par le coreID %u ('%s') : rechargement\n",
                        MEM32(req + 0xD4u) + 1, pose, core, voulu);
                if (k < 0) {
                    activ_noter(req, core);
                    for (i = 0; i < 32; i++)
                        if (activ_req[i] == req)
                            k = i;
                }
                /* les noms ecrits ici : le rechargement ne relit pas le XML (essai du 27 octobre :
                 * l'archive etait rouverte sous l'ancien nom) */
                if (k >= 0) {
                    char t[3][32];
                    if (voulu[0]) {
                        strcpy(t[0], voulu); strcpy(t[1], voulu); strcpy(t[2], voulu);
                        activ_noms(req, t, 0);
                        if (MEM32(req + 0xD4u) == 0)
                            MEM32(req + 0xD0u) = 1;
                    } else if (activ_req_orig[k][0][0])
                        activ_noms(req, activ_req_orig[k], 0);
                    activ_req_core[k] = core;
                    strcpy(activ_req_nom[k], voulu);
                }
                MEM32(req + 0x5Cu) = 0xFFFFFFFFu;
                MEM8(req + 0x60u) = 0;
                MEM8(req + 0x81u) = 0;
                MEM32(req + 0x18u) = 0xFFFFFFFFu;
                MEM8(req + 0x1Cu) = 0;
                MEM32(req + 0x84u) = 0xFFFFFFFFu;
                /* Mesure du 27 octobre (journal du joueur, CPU en position de repos) : rien ne
                 * rouvrait l'archive -- les plans de Blazin sont prepares en debut de combat par
                 * sub_00088FF0 (etat 0 -> 1 : FUN_000a3e80 jusqu'a ce qu'il soit pret, puis
                 * FUN_000a3130, etat 2), et l'activation les joue tout de suite. On refait donc ici
                 * cette preparation, en attendant le chargement (3 s au plus). */
                {
                    uint32_t s[8] = { g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp, g_esp };
                    DWORD t0 = GetTickCount();
                    int pret = 0, n = 0;
                    do {
                        g_esp -= 4; MEM32(g_esp) = 0;
                        g_ecx = req;
                        sub_000A3E80();
                        n++;
                        if (g_eax & 0xFF) { pret = 1; break; }
                        Sleep(1);
                    } while (GetTickCount() - t0 < 3000);
                    if (pret) {
                        g_esp -= 4; MEM32(g_esp) = 0;
                        g_esp -= 4; MEM32(g_esp) = 0;
                        g_ecx = req;
                        sub_000A3130();
                    }
                    fprintf(stderr, "  [BLAZIN] plan %u : '%s' %s (%d appels, %lu ms)\n", MEM32(req + 0xD4u) + 1,
                            (char *)XBOX_PTR(req + 0x61u), pret ? "charge" : "PAS CHARGE", n,
                            (unsigned long)(GetTickCount() - t0));
                    g_eax = s[0]; g_ebx = s[1]; g_ecx = s[2]; g_edx = s[3];
                    g_esi = s[4]; g_edi = s[5]; g_ebp = s[6]; g_esp = s[7];
                }
            }
        }
    }
    sub_00087E20_gen();
}

/* La demande de cri d'activation (evenement 0x41 valeur 4 -> FUN_00032170 -> moteur d'EA,
 * categorie 3, evenement 1, la voix en argument sur la pile). Le moteur ne joue pas le cri a chaque fois :
 * le plan d'activation Vendetta le demande donc 3 fois (animtags.py) ; mais un cri deja parti est
 * relance par la demande suivante (le debut se repetait, signale par l'utilisateur le 27 octobre).
 * Mesure du meme jour : la valeur rendue vaut 1 a chaque demande, mais la banque Character de la voix
 * n'est ouverte que pour le cri accepte (kernel_path.c note l'heure). Le filtre est dans
 * sub_000379E0 (l'evenement entier) : ignorer ici la seule demande laissait l'appelant couper la voix
 * (FUN_00137390, l'aboiement commencait puis se coupait). */
extern volatile unsigned long g_xbox_speech_character_tick[1024];
extern void sub_00032170_gen(void);
void sub_00032170(void)
{
    sub_00032170_gen();
}

/* Le rangement des formes d'un film Apt (sub_0017F080, appele par sub_001813E0 seul : ebx = tableau
 * local de ecx = 0x200 places, argument = le film) : chaque forme va a la place atol(son nom), sans
 * borne -- une forme numerotee 512 ou plus ecrase la pile (le jeu se figeait apres le chargement de
 * la grille de selection avec une forme sepia par personnage Vendetta, point 107). L'appelant ne se
 * sert pas du numero de la place : il passe les 512 places et pose une matrice sur chaque forme. Le
 * corps genere range donc dans un tableau de 4096 places a part, puis les formes sont
 * recopiees a la suite dans le vrai tableau : la limite porte sur le nombre de formes (512), plus
 * sur leur numero. */
extern void sub_0017F080_gen(void);
void sub_0017F080(void)
{
    static uint32_t grand;
    uint32_t table = g_ebx, n = g_ecx, film = MEM32(g_esp + 4), ret = g_esp, i, k = 0;
    if (!grand)
        grand = xbox_ContiguousAlloc(4 * 0x1000, 16);
    if (!grand || n != 0x200) {
        sub_0017F080_gen();
        return;
    }
    g_esp -= 4; MEM32(g_esp) = film;
    g_esp -= 4; MEM32(g_esp) = 0;           /* adresse de retour, depilee par le corps */
    g_ebx = grand;
    g_ecx = 0x1000;
    sub_0017F080_gen();
    for (i = 0; i < n; i++)
        MEM32(table + 4 * i) = 0;
    for (i = 0; i < 0x1000; i++) {
        uint32_t f = MEM32(grand + 4 * i);
        if (f && k < n)
            MEM32(table + 4 * k++) = f;
    }
    g_ebx = table;
    g_ecx = n;
    g_esp = ret + 4;                        /* ret simple, comme le corps genere */
}

/* Le traitement d'un evenement d'animation (FUN_000379e0(evenement) : code en +0, valeur en +2,
 * combattant en +0x18 ; sa voix a 0x3403B4 + 100 x combattant). Un cri d'activation (0x41, valeur 4)
 * arrivant moins de 6 s apres l'ouverture de la banque Character de cette voix -- le cri est parti --
 * est ignore en entier. */
extern void sub_000379E0_gen(void);
void sub_000379E0(void)
{
    uint32_t ev = MEM32(g_esp + 4);
    if (dsp_readable(ev + 0x18) && MEM16(ev) == 0x41 && MEM16(ev + 2) == 4) {
        uint32_t c = MEM16(ev + 0x18), voix;
        if (c < 8 && dsp_readable(0x3403B4u + 100u * c)) {
            voix = MEM32(0x3403B4u + 100u * c);
            if (voix < 1024 && g_xbox_speech_character_tick[voix]
                && GetTickCount() - g_xbox_speech_character_tick[voix] < 6000) {
                g_eax = 0;
                g_esp += 4;               /* ret simple, comme le corps genere */
                return;
            }
        }
    }
    sub_000379E0_gen();
}

extern void sub_00087AF0_gen(void);
void sub_00087AF0(void)
{
    uint32_t etat = dsp_readable(g_ecx + 0x2FF5Cu) ? MEM32(g_ecx + 0x2FF5Cu) : 0;
    uint32_t p2 = MEM32(g_esp + 8);
    uint32_t joueur = dsp_readable(p2 + 4) ? MEM32(p2 + 4) : 0xFFFFFFFFu;
    uint32_t adr = 0, vieux = 0;
    if (etat && joueur < 8 && dsp_readable(etat + 0xA8u + 4u * joueur)) {
        adr = etat + 0xA8u + 4u * joueur;
        vieux = MEM32(adr);
        activ_noter(MEM32(g_esp + 4), vieux);
        if (MEM32(p2 + 12) == 2u)
            fprintf(stderr, "  [BLAZIN] requete %08X : joueur %u (coreID %u), type %u, plan %u\n",
                    MEM32(g_esp + 4), joueur, vieux, MEM32(p2 + 12), MEM32(p2 + 28));
        if (vieux >= 0x57u && vieux < CORE_MAX && g_core_donneur[vieux] >= 0)
            MEM32(adr) = (uint32_t)g_core_donneur[vieux];
        else
            adr = 0;
    }
    sub_00087AF0_gen();
    if (adr)
        MEM32(adr) = vieux;
}

/* Les Blazin des personnages ajoutes (2026-10-17, lu dans le code).
 *
 * sub_001A8260 donne a CBlazinAnimationData::Init (sub_000A0990) trois constantes : le premier
 * identifiant d'animation des Blazin (709), le nombre d'animations (164 = 82 paires) et un tableau
 * d'octets (0x3BE628), un drapeau << a charger >> par animation. sub_000A0C40 lit les noms de
 * blazin.mnl dans l'ordre (identifiant = 709 + rang) et ne charge que ceux dont le drapeau est mis ;
 * au-dela de 164, rien : les paires neuves du mod (82 et plus) n'existaient pas pour le jeu. Ici :
 * autant d'animations que de noms dans la liste du mod, et un tableau de drapeaux a nous, assez grand.
 *
 * sub_001A8180 met les drapeaux : pour chaque combattant, ses 4 Blazin (combattant + 0xB8C, +0xB90,
 * +0xB94, +0xB98 ; les combattants a 0x3B92E0, pas 0x12E8), drapeaux aux rangs 2p et 2p+1. Mais si
 * coreID - 1 >= 86 (coreID >= 87, le heros du mode Story), ses 4 Blazin sont d'abord remplaces par
 * ceux des reglages du joueur (0x3B928C + 16 x joueur : les Blazin achetes du heros) -- pour DMX
 * (clone 91) des zeros, d'ou la paire 0 (Crow) quoi qu'on ecrive dans main_all.dat. Meme fonction,
 * transcrite, avec deux changements : les clones de coreid.txt ne sont pas le heros, et les
 * drapeaux restent dans le tableau. */
#define BLAZIN_PREMIER   709u
#define BLAZIN_ORIGINE   164u
#define BLAZIN_MAX       512u
static uint32_t blazin_drapeaux;            /* adresse invitee du tableau de drapeaux */
static uint32_t blazin_nombre;              /* animations de Blazin (noms de blazin.mnl) */
static char blazin_noms[BLAZIN_MAX][16];    /* noms des animations du mod (rang 164 et plus) */
static uint32_t blazin_noms_va;             /* les memes, en memoire invitee (sub_001A8150) */

/* Blazin de dos, par rang de paire : le jeu teste << paire >= 57 >> (les 82 paires d'origine sont
 * rangees face N06 puis dos N07) ; les paires du mod suivent, de face ou de dos selon leur nom.
 * Lu par src/recomp_patch.c (outils/patch_gen.py). */
unsigned char g_blazin_dos[512];

static void blazin_preparer(void)
{
    static int dos_fait;
    if (!dos_fait) {
        uint32_t p;
        dos_fait = 1;
        for (p = 0; p < 512; p++)
            g_blazin_dos[p] = p >= 57;                 /* le test d'origine */
    }
    if (blazin_drapeaux)
        return;
    blazin_nombre = BLAZIN_ORIGINE;
    {
        FILE *f = fopen("game_files\\mods\\motion\\blazin\\blazin.mnl", "rb");
        uint32_t n = 0;
        if (f) {
            if (fread(&n, 4, 1, f) == 1 && n > BLAZIN_ORIGINE && n <= BLAZIN_MAX && !(n & 1)) {
                char nom[64];
                uint32_t k = 0, i = 0, zero;
                int c;
                blazin_nombre = n;
                if (fread(&zero, 4, 1, f) == 1) {
                    while ((c = fgetc(f)) != EOF && k < n) {
                        if (c == '.') {
                            nom[i] = 0;
                            if (!(k & 1) && k / 2 >= 82)       /* les paires d'origine gardent le test */
                                g_blazin_dos[k / 2] = strncmp(nom, "N07", 3) == 0;
                            if (k >= BLAZIN_ORIGINE && k < BLAZIN_MAX)
                                snprintf(blazin_noms[k], sizeof(blazin_noms[k]), "%s", nom);
                            k++;
                            i = 0;
                        } else if (i < sizeof(nom) - 1) {
                            nom[i++] = (char)c;
                        }
                    }
                }
            }
            fclose(f);
        }
    }
    blazin_drapeaux = xbox_ContiguousAlloc(BLAZIN_MAX, 16);
    if (blazin_drapeaux)
        memset((void *)XBOX_PTR(blazin_drapeaux), 0, BLAZIN_MAX);
    blazin_noms_va = xbox_ContiguousAlloc(sizeof(blazin_noms), 16);
    if (blazin_noms_va)
        memcpy((void *)XBOX_PTR(blazin_noms_va), blazin_noms, sizeof(blazin_noms));
    fprintf(stderr, "  [BLAZIN] %u animations (%u paires), drapeaux a 0x%08X\n",
            blazin_nombre, blazin_nombre / 2, blazin_drapeaux);
    fflush(stderr);
}

/* Les deux fonctions qui deduisent << de dos >> du rang (paire >= 57) : copies corrigees de
 * outils/patch_gen.py (src/recomp_patch.c), d'apres g_blazin_dos. RECOMP_BLAZIN_NATIF : le test
 * d'origine, pour comparer. */
extern void sub_001A8BC0_gen(void);
extern void sub_001A8BC0_patch(void);
extern void sub_001D0F80_gen(void);
extern void sub_001D0F80_patch(void);
static int blazin_natif(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("RECOMP_BLAZIN_NATIF") != NULL;
    return v;
}
void sub_001A8BC0(void)
{
    blazin_preparer();
    if (blazin_natif())
        sub_001A8BC0_gen();
    else
        sub_001A8BC0_patch();
}
void sub_001D0F80(void)
{
    blazin_preparer();
    if (blazin_natif())
        sub_001D0F80_gen();
    else
        sub_001D0F80_patch();
}

/* sub_001A8150(type, animation) : le nom d'une animation, lu dans deux tables de l'executable --
 * 2088 noms pour le type 1 (les coups), 873 sinon (dont les Blazin, 709 a 872) -- et '(none)'
 * au-dela. Le nom sert aux declencheurs de animtags.dat (cri, voix, bruitages) : les animations des
 * paires du mod (873 et plus) prennent leur nom dans blazin.mnl. */
extern void sub_001A8150_gen(void);
void sub_001A8150(void)
{
    uint32_t type = MEM32(g_esp + 4), anim = MEM32(g_esp + 8);
    blazin_preparer();
    if (type != 1 && anim >= BLAZIN_PREMIER + BLAZIN_ORIGINE && anim < BLAZIN_PREMIER + blazin_nombre
        && blazin_noms_va && blazin_noms[anim - BLAZIN_PREMIER][0]) {
        g_eax = blazin_noms_va + 16u * (anim - BLAZIN_PREMIER);
        g_esp += 4;
        return;
    }
    sub_001A8150_gen();
}

/* (int *premier, int *nombre, char **drapeaux) */
void sub_001A8260(void)
{
    uint32_t a = MEM32(g_esp + 4), b = MEM32(g_esp + 8), c = MEM32(g_esp + 12);
    blazin_preparer();
    MEM32(a) = BLAZIN_PREMIER;
    MEM32(b) = blazin_drapeaux ? blazin_nombre : BLAZIN_ORIGINE;
    MEM32(c) = blazin_drapeaux ? blazin_drapeaux : 0x3BE628u;
    g_esp += 4;
}

void sub_001A8180(void)
{
    uint32_t tab, nb, j;
    blazin_preparer();
    tab = blazin_drapeaux ? blazin_drapeaux : 0x3BE628u;
    nb = blazin_drapeaux ? blazin_nombre : BLAZIN_ORIGINE;
    memset((void *)XBOX_PTR(tab), 0, blazin_drapeaux ? BLAZIN_MAX : BLAZIN_ORIGINE);
    for (j = 0; j < 4; j++) {
        uint32_t f = 0x3B9E9Cu + 0x12E8u * j, s = 0x3B928Cu + 0x10u * j, k;
        int32_t ci;
        int heros;
        if (MEM32(f - 2964) == 0xFFFFFFFFu)
            continue;
        ci = (int32_t)MEM32(f - 2960);
        heros = ci >= 0x56 && !(ci + 1 < CORE_MAX && g_core_donneur[ci + 1] >= 0);
        for (k = 0; k < 4; k++) {
            uint32_t slot = f - 4 + 4 * k;
            int32_t p;
            if (heros)
                MEM32(slot) = MEM32(s - 4 + 4 * k);
            p = (int32_t)MEM32(slot);
            if (p >= 0 && (uint32_t)(2 * p + 1) < nb) {
                MEM8(tab + 2 * p) = 1;
                MEM8(tab + 2 * p + 1) = 1;
            }
        }
    }
    g_esp += 4;
}

static void unlock_all_tick(void)
{
    static int enabled = -1;
    static int done, tries;
    static ULONGLONG next_try;
    static uint32_t chars_after, venues_after;   /* list sizes our unlock left */
    uint32_t fe, profile, db;
    GuestRegs regs;
    int faulted = 0;

    if (enabled < 0)
        enabled = getenv("RECOMP_UNLOCK_ALL") != NULL;
    if (!enabled)
        return;
    if (done) {
        uint32_t p = unlock_profile();
        uint32_t chars = 0, venues = 0;
        int reloaded = 0;
        if (p && dsp_readable(p + 0x24)) {
            chars = MEM32(p + 0x24);
            venues = MEM32(p + 0x10);
            reloaded = chars < chars_after || venues < venues_after;
        }
        if (!reloaded && !(GetAsyncKeyState(VK_F7) & 1))
            return;
        if (reloaded) {
            fprintf(stderr, "  [UNLOCK] profile reloaded (%u characters and %u venues"
                    " unlocked, %u and %u before): unlocking again\n",
                    chars, venues, chars_after, venues_after);
            fflush(stderr);
        }
        tries = 0;                     /* F7 or a profile read: do it again */
        next_try = 0;
    }
    if (GetTickCount64() < next_try || tries > 30)
        return;
    next_try = GetTickCount64() + 3000;

    if (!dsp_readable(0x002FB1A8u))
        return;
    fe = MEM32(0x002FB1A8u);
    if (!fe || !dsp_readable(fe + 8))
        return;
    profile = MEM32(fe + 4);
    db      = MEM32(fe + 8);
    if (!profile || !db || !dsp_readable(profile + 0x50) || !dsp_readable(db))
        return;

    /* Both collections have to exist first. Unlocking venues while the venue
     * collection is still missing does not fault here -- it faults in the
     * title a moment later, when the front end reads the list it now has
     * entries in. sub_00012ED0 looks a collection up by name hash and writes
     * an iterator; a missing one gives {db, -1, -1, 0}. */
    if (!unlock_collection_ready(db, 0x4CE5B1B2u)       /* characters */
     || !unlock_collection_ready(db, 0xD7DCA628u))      /* venues */
        return;

    /* Retried under SEH anyway: this is code the retail front end never
     * calls, so it has never run before. Adding is idempotent -- the title
     * checks each entry before inserting it. */
    tries++;
    guest_regs_save(&regs);
    GUARD_BEGIN
        PUSH32(g_esp, 0);              /* dummy return address: cdecl, no args */
        sub_000158F0();
    GUARD_FAULT
        faulted = 1;
    GUARD_END
    guest_regs_restore(&regs);
    if (faulted) {
        fprintf(stderr, "  [UNLOCK] attempt %d too early (the title's lists are not"
                        " loaded yet); retrying\n", tries);
        fflush(stderr);
        return;
    }
    done = 1;
    chars_after = MEM32(profile + 0x24);
    venues_after = MEM32(profile + 0x10);
    {
        uint32_t screen = dsp_readable(g_ecx + 0x18) ? MEM32(g_ecx + 0x18) : 0;
        fprintf(stderr, "  [UNLOCK] fe=0x%08X profile=0x%08X db=0x%08X screen=0x%08X"
                " vtable=0x%08X state=%u\n", fe, profile, db, screen,
                (screen && dsp_readable(screen)) ? MEM32(screen) : 0,
                dsp_readable(g_ecx + 0x24) ? MEM32(g_ecx + 0x24) : 0);
    }
    fprintf(stderr, "  [UNLOCK] every character and venue unlocked (%u and %u),"
            " %u reward points\n", chars_after, venues_after,
            dsp_readable(profile + 0x50) ? MEM32(profile + 0x50) : 0);
    fflush(stderr);
}

void sub_00077040(void)
{
    static int enabled = -1;

    /* One call per game update: the "updates/s" in the window title. */
    xbox_FramebufferWindowCountTick();

    profil_reparer_tick();
    unlock_all_tick();

    /* L'ecran courant, pour le pilote automatique (attente d'un ecran). */
    {
        uint32_t ecran = dsp_readable(g_ecx + 0x18) ? MEM32(g_ecx + 0x18) : 0;
        uint32_t vt = ecran && dsp_readable(ecran) ? MEM32(ecran) : 0;
        autopilote_ecran(vt);
    }

    if (enabled < 0)
        enabled = getenv("RECOMP_STATE_TRACE") != NULL;

    if (enabled) {
        static ULONGLONG next_ms;
        static LONG ticks;
        ULONGLONG now = GetTickCount64();
        InterlockedIncrement(&ticks);
        if (now >= next_ms) {
            uint32_t self   = g_ecx;
            uint32_t state  = dsp_readable(self + 0x24) ? MEM32(self + 0x24) : 0xFFFFFFFF;
            uint32_t screen = dsp_readable(self + 0x18) ? MEM32(self + 0x18) : 0;
            uint32_t vtab   = (screen && dsp_readable(screen)) ? MEM32(screen) : 0;
            uint32_t ready  = (vtab && dsp_readable(vtab + 0x14)) ? MEM32(vtab + 0x14) : 0;
            uint32_t tick   = (vtab && dsp_readable(vtab + 0x0C)) ? MEM32(vtab + 0x0C) : 0;
            next_ms = now + 1000;
            ticks++;
            fprintf(stderr, "  [STATE] tick #%ld state=%u screen=0x%08X vtable=0x%08X "
                    "ready=sub_%08X tick=sub_%08X\n",
                    (long)ticks, state, screen, vtab, ready, tick);
            fflush(stderr);
        }
    }

    /* Every change of state or screen, with both screens' vtables. */
    static uint32_t last_key[4];
    if (enabled && dsp_readable(g_ecx + 0x24)
        && (MEM32(g_ecx + 0x24) != last_key[0] || MEM32(g_ecx + 0x18) != last_key[1]
            || MEM32(g_ecx + 0x1C) != last_key[2] || MEM8(g_ecx + 0x21) != last_key[3])) {
        uint32_t self = g_ecx;
        last_key[0] = MEM32(self + 0x24); last_key[1] = MEM32(self + 0x18);
        last_key[2] = MEM32(self + 0x1C); last_key[3] = MEM8(self + 0x21);
        fprintf(stderr, "  [STATE] change: state=%u flag21=%u\n", last_key[0], last_key[3]);
        for (int w = 0; w < 2; w++) {
            uint32_t obj = MEM32(self + (w ? 0x1C : 0x18));
            uint32_t vt = obj && dsp_readable(obj) ? MEM32(obj) : 0;
            char line[256];
            int o = snprintf(line, sizeof line, "  [STATE] transition %s screen 0x%08X vtable 0x%08X:",
                             w ? "new" : "old", obj, vt);
            for (int k = 0; k < 8 && vt && dsp_readable(vt + k * 4); k++)
                o += snprintf(line + o, sizeof line - o, " %08X", MEM32(vt + k * 4));
            fprintf(stderr, "%s\n", line);
        }
        fflush(stderr);
    }

    sub_00077040_gen();
}

/* Probe on D3D's timed-callback registration at 0x00216410.
 *
 * The title's input thread is woken by a callback it schedules here against a
 * TSC deadline; that thread is what feeds the game loop its ticks, so if the
 * registration is refused nothing ever updates. The function returns
 * 0x8876082A when the deadline is not far enough in the future, which is the
 * one outcome that would explain the title rendering forever without ticking.
 * Logged once a second under RECOMP_STATE_TRACE, and every refusal.
 *
 * It is also where a late deadline is forgiven. The title re-registers its
 * callback from inside the previous one at "previous deadline + period"; on
 * hardware that call happens well inside the period, but here a busy moment
 * (loading the next screen) delayed it past its own next deadline, D3D
 * refused it, and nothing ever registered the callback again: the game logic
 * stopped for good on the first screen transition. A deadline already due is
 * moved to one millisecond from now, which is what the hardware would have
 * made of it.
 */
extern uint64_t xbox_ReadTimeStampCounter(void);
extern void sub_00216410_gen(void);

#define TSC_PER_MS 733333u      /* the emulated Pentium III runs at 733 MHz */

void sub_00216410(void)
{
    static int enabled = -1;
    uint32_t lo = 0, hi = 0, cb = 0;
    uint64_t now = 0;

    if (enabled < 0)
        enabled = getenv("RECOMP_STATE_TRACE") != NULL;

    {
        uint64_t t = xbox_ReadTimeStampCounter();
        uint64_t deadline = ((uint64_t)MEM32(g_esp + 8) << 32) | MEM32(g_esp + 4);
        if (deadline < t + TSC_PER_MS) {
            static LONG forgiven;
            deadline = t + TSC_PER_MS;
            MEM32(g_esp + 4) = (uint32_t)deadline;
            MEM32(g_esp + 8) = (uint32_t)(deadline >> 32);
            if (enabled || InterlockedIncrement(&forgiven) <= 3)
                fprintf(stderr, "  [TIMERCB] late deadline moved to now + 1 ms\n");
        }
    }

    if (enabled) {
        lo  = MEM32(g_esp + 4);
        hi  = MEM32(g_esp + 8);
        cb  = MEM32(g_esp + 12);
        now = xbox_ReadTimeStampCounter();
    }

    {
        /* stdcall, four dwords: keep them to call again if refused. The call
         * itself can take longer than the margin, so a refusal is retried
         * with a deadline further out rather than accepted. */
        uint32_t ret = MEM32(g_esp), a[4];
        int tries;
        for (tries = 0; tries < 4; tries++)
            a[tries] = MEM32(g_esp + 4 + tries * 4);
        /* The function raises IRQL before it reads the TSC, and that can wait
         * on the timer thread for a long time while the renderer is busy, so
         * a few milliseconds of margin is not always enough. The margin grows
         * to a second: a late callback is a hitch, a lost one freezes the
         * game logic for good (seen loading a fight). */
        static const unsigned margin_ms[] = { 4, 16, 64, 250, 1000, 1000, 1000, 1000 };
        for (tries = 0; ; tries++) {
            sub_00216410_gen();
            if (g_eax != 0x8876082Au)
                break;
            if (tries == (int)(sizeof margin_ms / sizeof margin_ms[0])) {
                fprintf(stderr, "  [TIMERCB] refused %d times, giving up -- game logic will stall\n",
                        tries + 1);
                break;
            }
            {
                uint64_t d = xbox_ReadTimeStampCounter() + (uint64_t)TSC_PER_MS * margin_ms[tries];
                a[0] = (uint32_t)d;
                a[1] = (uint32_t)(d >> 32);
                g_esp -= 20;
                MEM32(g_esp) = ret;
                MEM32(g_esp + 4)  = a[0];
                MEM32(g_esp + 8)  = a[1];
                MEM32(g_esp + 12) = a[2];
                MEM32(g_esp + 16) = a[3];
                fprintf(stderr, "  [TIMERCB] refused, retrying %u ms out\n", margin_ms[tries]);
            }
        }
    }

    if (enabled) {
        static ULONGLONG next_ms;
        ULONGLONG t = GetTickCount64();
        if (t >= next_ms || g_eax != 0) {
            uint64_t deadline = ((uint64_t)hi << 32) | lo;
            if (g_eax == 0)
                next_ms = t + 1000;
            fprintf(stderr, "  [TIMERCB] cb=sub_%08X deadline=%llu now=%llu "
                    "delta=%lld -> eax=0x%08X\n",
                    cb, (unsigned long long)deadline,
                    (unsigned long long)now,
                    (long long)(deadline - now), g_eax);
            fflush(stderr);
        }
    }
}

/* Probe on the input thread's wake-up at 0x000D7EA0.
 *
 * D3D calls this from the timed callback the title registers each frame; it
 * signals the event the input sampling thread waits on, and that thread is
 * what produces the game's update ticks. Counting it says whether the chain
 * vblank -> ISR -> DPC -> timed callback ever reaches the title. Under
 * RECOMP_STATE_TRACE only.
 */
extern void sub_000D7EA0_gen(void);
extern uint32_t xbox_Nv2aPtimerFiredFor(void);

static volatile LONG g_inputwake_count;

/* When the callback above stops arriving, say what D3D's timer object and the
 * PTIMER registers look like. The object is *(0x00234768) + 0x1C28: +0 the
 * register base, +0x818 interrupt enabled, +0x820/+0x828 period and periodic
 * deadline, +0x830/+0x838 the two 64-bit slots, +0x840 the slot ALARM_0 was
 * armed for, +0x844 the one-shot slot, +0x848/+0x84C its callback and arg. */
static DWORD WINAPI timer_stall_thread(LPVOID unused)
{
    LONG last = -1;
    unsigned quiet = 0;
    (void)unused;
    for (;;) {
        LONG now;
        Sleep(1000);
        now = g_inputwake_count;
        if (now != last) {
            last = now;
            quiet = 0;
            continue;
        }
        if (++quiet < 2 || (quiet > 6 && quiet % 30))
            continue;
        {
            uint32_t base = MEM32(0x00234768u), t = base + 0x1C28u, k;
            fprintf(stderr, "[TIMERSTALL] %us without callback (count %ld): obj=0x%08X"
                    " PMC_INTR=0x%08X PMC_EN=0x%08X PT_INTR=0x%08X PT_EN=0x%08X"
                    " TIME=%08X:%08X ALARM=%08X fired_for=%08X\n",
                    quiet, (long)now, t,
                    MEM32(0xFD000100u), MEM32(0xFD000140u), MEM32(0xFD009100u),
                    MEM32(0xFD009140u), MEM32(0xFD009410u), MEM32(0xFD009400u),
                    MEM32(0xFD009420u), xbox_Nv2aPtimerFiredFor());
            if (base && dsp_readable(t + 0x84C)) {
                fprintf(stderr, "[TIMERSTALL] +818:");
                for (k = 0x818; k <= 0x84C; k += 4)
                    fprintf(stderr, " %08X", MEM32(t + k));
                fprintf(stderr, "\n");
            }
            /* The input timer's own run state: 0 started, 1 running,
             * 2 stop requested (sub_000D7D50 spins until the callback
             * answers 3), 3 stopped. */
            {
                uint32_t it = MEM32(0x003A0178u);
                if (it && dsp_readable(it + 0x1A9C))
                    fprintf(stderr, "[TIMERSTALL] input timer 0x%08X run=%u period=%08X:%08X"
                            " next=%08X:%08X\n", it, MEM32(it + 0x1A78),
                            MEM32(it + 0x1A84), MEM32(it + 0x1A80),
                            MEM32(it + 0x1A94), MEM32(it + 0x1A90));
            }
            fflush(stderr);
        }
    }
}

void sub_000D7EA0(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        enabled = getenv("RECOMP_STATE_TRACE") != NULL;
        if (enabled)
            CreateThread(NULL, 0, timer_stall_thread, NULL, 0, NULL);
    }

    if (enabled) {
        LONG n = InterlockedIncrement(&g_inputwake_count);
        if (n == 1 || (n % 300) == 0) {
            fprintf(stderr, "  [INPUTWAKE] input thread signalled %ld times\n", n);
            fflush(stderr);
        }
    }

    sub_000D7EA0_gen();
}

/* Two more probes on the callback chain, under RECOMP_STATE_TRACE.
 *
 * sub_002231D0 is D3D's vblank service, reached from the deferred routine the
 * ISR queues. sub_002166E0 is the walker that fires the timed callbacks the
 * title registers. Between them they say which half of
 * "vblank -> service -> dispatch -> callback" is missing.
 */
extern void sub_002231D0_gen(void);
extern void sub_002166E0_gen(void);

static void probe_count(const char *what, LONG *counter)
{
    LONG n = InterlockedIncrement(counter);
    if (n == 1 || (n % 300) == 0) {
        fprintf(stderr, "  [CHAIN] %s ran %ld times\n", what, n);
        fflush(stderr);
    }
}

static int chain_trace(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("RECOMP_STATE_TRACE") != NULL;
    return enabled;
}

void sub_002231D0(void)
{
    static LONG n;
    if (chain_trace()) probe_count("vblank service sub_002231D0", &n);
    sub_002231D0_gen();
}

void sub_002166E0(void)
{
    static LONG n;
    if (chain_trace()) probe_count("callback walker sub_002166E0", &n);
    sub_002166E0_gen();
}

/* The title's bytecode interpreter at 0x001931B0.
 *
 *     op = *pc++ ; handler = table[op] ; handler(ctx)
 *
 * with the table at *(*(ctx + 0x28) + 0x8C). Its handlers are reached only
 * through that table, so static analysis finds them one at a time, as the
 * running title happens to execute each opcode -- and every one it has not
 * found yet is a skipped instruction in a script, which surfaces later as a
 * crash somewhere unrelated.
 *
 * This reads the whole table the first time each distinct table is seen and
 * writes every entry that points into .text to interp_seeds.json, a seed file
 * for tools.disasm. One run then covers every opcode instead of one per
 * regeneration.
 */
extern void sub_001931B0_gen(void);

void sub_001931B0(void)
{
    static uint32_t tables[8];
    static unsigned n_tables;
    uint32_t ctx = MEM32(g_esp + 4);

    if (dsp_readable(ctx + 0x28)) {
        uint32_t holder = MEM32(ctx + 0x28);
        uint32_t table = dsp_readable(holder + 0x8C) ? MEM32(holder + 0x8C) : 0;
        unsigned i;

        for (i = 0; i < n_tables && tables[i] != table; i++) { }
        if (table && i == n_tables && n_tables < 8 && dsp_readable(table)) {
            static uint32_t seen[2048];
            static unsigned n_seen;
            unsigned op, added = 0;
            FILE *f;

            tables[n_tables++] = table;
            for (op = 0; op < 256 && dsp_readable(table + op * 4); op++) {
                uint32_t h = MEM32(table + op * 4);
                unsigned k;
                if (h < 0x00011000u || h >= 0x00210E00u)   /* .text only */
                    continue;
                for (k = 0; k < n_seen && seen[k] != h; k++) { }
                if (k == n_seen && n_seen < 2048) {
                    seen[n_seen++] = h;
                    added++;
                }
            }

            f = fopen("interp_seeds.json", "w");
            if (f) {
                fprintf(f, "[\n");
                for (i = 0; i < n_seen; i++)
                    fprintf(f, "  {\"start\": \"0x%08X\", \"source\": "
                            "\"bytecode-handler-table\"}%s\n",
                            seen[i], i + 1 < n_seen ? "," : "");
                fprintf(f, "]\n");
                fclose(f);
            }
            fprintf(stderr, "  [INTERP] handler table 0x%08X: %u new handlers,"
                    " %u in interp_seeds.json\n", table, added, n_seen);
            fflush(stderr);
        }
    }

    sub_001931B0_gen();
}

/* The screen loader thread, 0x000771E0.
 *
 *     wait(event) ; while (next) { next->load() ; state = 3 ; wait(event) }
 *
 * The game loop sits in its "loading" state until this sets state 3, so a
 * load() that never returns is a title stuck on its loading screen. The
 * existing watchdog only samples the main thread; this keeps the address of
 * the loader's own guest stack pointer (g_esp is thread-local, so &g_esp taken
 * on this thread names this thread's copy) and a monitor thread prints its
 * guest stack in the watchdog's "GS" format every RECOMP_LOADER_WATCH seconds,
 * which tools/stackwalk.py turns into a backtrace.
 */
extern void sub_000771E0_gen(void);
static uint32_t *volatile g_loader_esp;
static volatile uint32_t g_loader_mgr;   /* the screen-state manager it serves */

static DWORD WINAPI loader_watch_thread(LPVOID arg)
{
    DWORD every = (DWORD)(uintptr_t)arg;
    unsigned round = 0;

    for (;;) {
        uint32_t esp, i;
        Sleep(every * 1000);
        if (!g_loader_esp)
            continue;
        esp = *g_loader_esp;
        fprintf(stderr, "[LOADERWATCH] round %u, loader guest esp=0x%08X\n",
                ++round, esp);

        /* The object the load loop pumps until it reports done:
         *   screen = mgr->next (+0x1C), p = screen->[+0x10],
         *   done   = byte (p->[+8])->[+0x40], pumped only while byte p[1] != 0 */
        {
            uint32_t mgr = g_loader_mgr, scr = 0, p = 0, sub = 0;
            if (dsp_readable(mgr + 0x1C)) scr = MEM32(mgr + 0x1C);
            if (scr && dsp_readable(scr + 0x10)) p = MEM32(scr + 0x10);
            if (p && dsp_readable(p + 8)) sub = MEM32(p + 8);
            fprintf(stderr, "[LOADERWATCH] mgr=0x%08X next=0x%08X p=0x%08X "
                    "p[0]=%u p[1]=0x%08X sub=0x%08X done=%u\n",
                    mgr, scr, p,
                    p ? MEM32(p) : 0, p ? MEM32(p + 4) : 0, sub,
                    (sub && dsp_readable(sub + 0x40)) ? (unsigned)(MEM32(sub + 0x40) & 0xFF) : 999);
            /* The APT entries the movie requested (sub_00056520): its own at
             * +0x58, and one per sub-movie in the array at +0x54 (count +0x4C,
             * 0x44 bytes each, name at +0, entry at +0x40). An entry is
             * {refs, nameobj, state}; state 1 = queued for loading, 4 = loaded. */
            if (sub && dsp_readable(sub + 0x5C)) {
                uint32_t ents[5], n = 0, k;
                char names[5][48];
                ents[n] = MEM32(sub + 0x58);
                snprintf(names[n], sizeof names[n], "(movie)");
                n++;
                {
                    uint32_t arr = MEM32(sub + 0x54), cnt = MEM32(sub + 0x4C);
                    for (k = 0; k < cnt && n < 5 && dsp_readable(arr + k * 0x44 + 0x40); k++) {
                        uint32_t j;
                        for (j = 0; j < 47 && MEM8(arr + k * 0x44 + j); j++)
                            names[n][j] = (char)MEM8(arr + k * 0x44 + j);
                        names[n][j] = 0;
                        ents[n++] = MEM32(arr + k * 0x44 + 0x40);
                    }
                }
                for (k = 0; k < n; k++) {
                    uint32_t e = ents[k];
                    if (e && dsp_readable(e + 8))
                        fprintf(stderr, "[LOADERWATCH] request %s -> entry 0x%08X refs=%u "
                                "nameobj=0x%08X state=%u w3=0x%08X w4=0x%08X\n",
                                names[k], e, MEM32(e), MEM32(e + 4), MEM32(e + 8),
                                MEM32(e + 12), MEM32(e + 16));
                    else
                        fprintf(stderr, "[LOADERWATCH] request %s -> entry 0x%08X\n",
                                names[k], e);
                }
            }
            if (sub && dsp_readable(sub)) {
                unsigned k;
                fprintf(stderr, "[LOADERWATCH] sub:");
                for (k = 0; k < 24; k++) fprintf(stderr, " %08X", MEM32(sub + k * 4));
                fprintf(stderr, "\n");
            }
        }

        /* The APT movie loader's current slot (sub_00181D20): index in
         * 0x002EAB4C, record = *(0x003B37A0) + idx*0x110, async file handle at
         * record+0x108. -1 means it is not loading anything. */
        {
            uint32_t idx = MEM32(0x002EAB4Cu);
            uint32_t tab = MEM32(0x003B37A0u);
            /* APT core side (sub_00146050 -> sub_00148210 -> sub_001480B0):
             * the load list is walked only while 0x003B1D8C is zero; each node
             * is {entry, next}, each entry {refs, nameobj, state, ...}, and an
             * entry in state 1 is handed to the load-movie callback. */
            {
                uint32_t gate = MEM32(0x003B1D8Cu);
                uint32_t list = MEM32(0x003B1D84u);   /* ecx of sub_001480B0 */
                uint32_t node = (list && dsp_readable(list)) ? MEM32(list) : 0;
                unsigned n = 0;
                fprintf(stderr, "[LOADERWATCH] APT gate 0x3B1D8C=0x%08X list=0x%08X\n",
                        gate, list);
                while (node && dsp_readable(node + 4) && n < 8) {
                    uint32_t e = MEM32(node);
                    if (e && dsp_readable(e + 8)) {
                        uint32_t no = MEM32(e + 4);
                        char nm[64];
                        unsigned j = 0;
                        if (no && dsp_readable(no + 8))
                            for (; j < 63 && MEM8(no + 8 + j); j++) nm[j] = (char)MEM8(no + 8 + j);
                        nm[j] = 0;
                        fprintf(stderr, "[LOADERWATCH]   entry 0x%08X refs=%u state=%u name=%s\n",
                                e, MEM32(e), MEM32(e + 8), nm);
                    }
                    node = MEM32(node + 4);
                    n++;
                }
                if (!n)
                    fprintf(stderr, "[LOADERWATCH]   load list empty\n");
            }
            fprintf(stderr, "[LOADERWATCH] APT queue count=%d [0..3]=%d %d %d %d\n",
                    (int)MEM32(0x002EAB88u), (int)MEM32(0x002EAB8Cu),
                    (int)MEM32(0x002EAB90u), (int)MEM32(0x002EAB94u),
                    (int)MEM32(0x002EAB98u));
            if (tab && dsp_readable(tab + 255 * 0x110)) {
                unsigned k, shown = 0;
                for (k = 0; k < 256 && shown < 8; k++) {
                    uint32_t r = tab + k * 0x110;
                    if (MEM8(r + 8)) {
                        char nm[64];
                        unsigned j;
                        for (j = 0; j < 63 && MEM8(r + 8 + j); j++) nm[j] = (char)MEM8(r + 8 + j);
                        nm[j] = 0;
                        fprintf(stderr, "[LOADERWATCH] record #%u state=%u obj=0x%08X "
                                "name=%s handle=0x%08X\n", k, MEM32(r),
                                MEM32(r + 4), nm, MEM32(r + 0x108));
                        shown++;
                    }
                }
                if (!shown)
                    fprintf(stderr, "[LOADERWATCH] no record carries a name\n");
            }
            fprintf(stderr, "[LOADERWATCH] APT loader slot = %d", (int)idx);
            if (idx != 0xFFFFFFFFu && tab && dsp_readable(tab + idx * 0x110 + 0x10C)) {
                uint32_t r = tab + idx * 0x110;
                char name[64];
                unsigned k;
                for (k = 0; k < 63 && MEM8(r + 4 + k); k++) name[k] = (char)MEM8(r + 4 + k);
                name[k] = 0;
                fprintf(stderr, " state=%u name=%s handle=0x%08X +10C=0x%08X",
                        MEM32(r), name, MEM32(r + 0x108), MEM32(r + 0x10C));
            }
            fprintf(stderr, "\n");
        }

        /* What the front-end load is waiting for: sub_001811F0 reports ready
         * only once all 256 records (0x110 bytes each) of the table at
         * *(0x003B37A0) have a non-zero first word. */
        {
            uint32_t tab = MEM32(0x003B37A0u);
            unsigned k, zero = 0, first_zero = 256;
            if (tab && dsp_readable(tab) && dsp_readable(tab + 255 * 0x110)) {
                for (k = 0; k < 256; k++) {
                    if (MEM32(tab + k * 0x110) == 0) {
                        if (first_zero == 256) first_zero = k;
                        zero++;
                    }
                }
                fprintf(stderr, "[LOADERWATCH] table 0x%08X: %u of 256 records "
                        "empty, first empty #%u\n", tab, zero, first_zero);
                if (first_zero < 256) {
                    fprintf(stderr, "[LOADERWATCH] record #%u:", first_zero);
                    for (k = 0; k < 16; k++)
                        fprintf(stderr, " %08X",
                                MEM32(tab + first_zero * 0x110 + k * 4));
                    fprintf(stderr, "\n");
                }
            } else {
                fprintf(stderr, "[LOADERWATCH] table pointer 0x%08X not readable\n", tab);
            }
        }
        for (i = 0; i < 400; i++) {
            uint32_t a = esp + i * 4;
            if (!dsp_readable(a))
                break;
            fprintf(stderr, "    GS %08X %08X\n", a, MEM32(a));
        }
        fflush(stderr);
    }
}

void sub_000771E0(void)
{
    const char *w = getenv("RECOMP_LOADER_WATCH");

    g_loader_esp = &g_esp;
    g_loader_mgr = MEM32(g_esp + 4);
    if (w && atoi(w) > 0) {
        static LONG started;
        if (InterlockedCompareExchange(&started, 1, 0) == 0)
            CreateThread(NULL, 0, loader_watch_thread,
                         (LPVOID)(uintptr_t)atoi(w), 0, NULL);
    }
    sub_000771E0_gen();
}

/* APT movie-load trace, under RECOMP_APT_TRACE.
 *
 * The four places a movie request passes through, in order:
 *   0x00144CB0  AptLoadMovie(out, name)      creates the entry, state 1
 *   0x00069CC0  the title's load-movie hook  (APT calls it for state-1 entries)
 *   0x001819C0  enqueue(name, obj)           hands it to the async loader
 *   0x00146D60  destroy(entry)               the entry's last reference went
 * A request that never reaches the loader shows here as the point it stops.
 */
static int apt_trace_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_APT_TRACE") != NULL;
    return on;
}

static void apt_name(uint32_t va, char *out, unsigned cap)
{
    unsigned j = 0;
    if (va && dsp_readable(va))
        for (; j + 1 < cap && MEM8(va + j) >= 0x20 && MEM8(va + j) < 0x7F; j++)
            out[j] = (char)MEM8(va + j);
    out[j] = 0;
}

extern void sub_00144CB0_gen(void);
extern void sub_00069CC0_gen(void);
extern void sub_001819C0_gen(void);
extern void sub_00146D60_gen(void);

void sub_00144CB0(void)
{
    if (apt_trace_on()) {
        char nm[64];
        apt_name(MEM32(g_esp + 8), nm, sizeof nm);
        fprintf(stderr, "  [APT] AptLoadMovie(\"%s\")\n", nm);
    }
    uint32_t out = MEM32(g_esp + 4);
    sub_00144CB0_gen();
    if (apt_trace_on()) {
        uint32_t e = MEM32(out), head = MEM32(MEM32(0x003B1D84)), len = 0;
        int ok = e && dsp_readable(e);
        for (uint32_t n = head; n && dsp_readable(n) && len < 64; n = MEM32(n + 4))
            len++;
        fprintf(stderr, "  [APT]   -> out 0x%08X eax 0x%08X entry 0x%08X"
                " [%08X %08X %08X %08X %08X %08X] list head 0x%08X len %u\n",
                out, g_eax, e,
                ok ? MEM32(e) : 0, ok ? MEM32(e + 4) : 0, ok ? MEM32(e + 8) : 0,
                ok ? MEM32(e + 12) : 0, ok ? MEM32(e + 16) : 0, ok ? MEM32(e + 20) : 0,
                head, len);
    }
}

void sub_00069CC0(void)
{
    if (apt_trace_on()) {
        char nm[64];
        apt_name(MEM32(g_esp + 4), nm, sizeof nm);
        fprintf(stderr, "  [APT] load-movie hook(\"%s\")\n", nm);
    }
    sub_00069CC0_gen();
}

void sub_001819C0(void)
{
    if (apt_trace_on()) {
        char nm[64];
        apt_name(MEM32(g_esp + 4), nm, sizeof nm);
        fprintf(stderr, "  [APT] enqueue(\"%s\")\n", nm);
    }
    sub_001819C0_gen();
}

void sub_00146D60(void)
{
    if (apt_trace_on()) {
        uint32_t e = MEM32(g_esp + 4);
        char nm[64] = "";
        if (e && dsp_readable(e + 8)) {
            uint32_t no = MEM32(e + 4);
            if (no && dsp_readable(no + 8))
                apt_name(no + 8, nm, sizeof nm);
        }
        fprintf(stderr, "  [APT] destroy entry 0x%08X state=%u \"%s\"\n", e,
                e && dsp_readable(e + 8) ? MEM32(e + 8) : 0, nm);
    }
    sub_00146D60_gen();
}

extern void sub_00148210_gen(void);
extern void sub_001480B0_gen(void);

/* 0x00148210: APT per-frame file update; the first thing it does is walk the
 * load list (0x001480B0), which is where state-1 entries reach the hook. */
void sub_00148210(void)
{
    static LONG n;
    LONG k = InterlockedIncrement(&n);
    if (apt_trace_on() && (k <= 3 || (k & (k - 1)) == 0))
        fprintf(stderr, "  [APT] update #%ld gate=%u from 0x%08X\n", k,
                MEM32(0x003B1D8C), MEM32(g_esp));
    sub_00148210_gen();
}

void sub_001480B0(void)
{
    if (apt_trace_on()) {
        static LONG n;
        LONG k = InterlockedIncrement(&n);
        uint32_t node = MEM32(g_ecx), len = 0;
        char line[512];
        int o = 0;
        for (; node && dsp_readable(node) && len < 16; node = MEM32(node + 4), len++) {
            uint32_t e = MEM32(node);
            char nm[48] = "";
            if (e && dsp_readable(e + 8) && MEM32(e + 4) && dsp_readable(MEM32(e + 4) + 8))
                apt_name(MEM32(e + 4) + 8, nm, sizeof nm);
            if (o < 400)
                o += snprintf(line + o, sizeof line - o, " [%s st=%u r=%u]", nm,
                              e && dsp_readable(e + 8) ? MEM32(e + 8) : 0,
                              e && dsp_readable(e) ? MEM32(e) : 0);
        }
        line[o] = 0;
        if (len && (k <= 20 || (k & (k - 1)) == 0))
            fprintf(stderr, "  [APT] list #%ld len=%u%s\n", k, len, line);
    }
    sub_001480B0_gen();
}

/* 0x001E9AB0: the title's named allocator, (name, size, align, flags, heap).
 * Logged under RECOMP_ALLOC_TRACE when it is asked for a lot or fails. */
extern void sub_001E9AB0_gen(void);
void sub_001E9AB0(void)
{
    static int on = -1;
    uint32_t name = MEM32(g_esp + 4), size = MEM32(g_esp + 8);
    uint32_t align = MEM32(g_esp + 12), flags = MEM32(g_esp + 16), heap = MEM32(g_esp + 20);
    if (on < 0)
        on = getenv("RECOMP_ALLOC_TRACE") != NULL;
    sub_001E9AB0_gen();
    if (on && (size >= 0x10000 || g_eax < 0x10000)) {
        char nm[64];
        apt_name(name, nm, sizeof nm);
        fprintf(stderr, "  [ALLOC] \"%s\" size=0x%X align=0x%X flags=0x%X heap=0x%08X -> 0x%08X\n",
                nm, size, align, flags, heap, g_eax);
    }
}

/* ── Gamepad ────────────────────────────────────────────────────
 *
 * The front end pauses itself while `_level0._root.ActiveController` has no
 * pad (FUN_00078390 stops ticking the Flash movie), so without a controller the
 * legal screen stays frozen on the first frame of its intro animation.
 *
 * The title reaches pads through the XAPI functions linked into its XPP
 * section, all stdcall, all reading the gamepad device-type table 0x0027CE74:
 *
 *   0x0027DDAC  XGetDevices(type)
 *   0x0027DDCE  XGetDeviceChanges(type, *inserted, *removed)
 *   0x0027DE3B  XInputOpen(type, port, slot, params)
 *   0x0027DE91  XInputClose(handle)
 *   0x0027DE9D  XInputGetCapabilities(handle, *caps)
 *   0x0027E075  XInputGetState(handle, *state)
 *
 * They are answered here from the host rather than through the emulated USB
 * controller, whose descriptor walk does not exist yet. Port 0 is always
 * present: a host XInput pad drives it when one is plugged in, the keyboard
 * otherwise (arrows, Enter = START, Esc = BACK, Space/X/C/V = A/B/X/Y,
 * Q/E = black/white). Ports 1-3 follow host pads 1-3. Other device types
 * (memory units) still go to the title's own code. RECOMP_NO_PAD disables it.
 */
#include <xinput.h>

#define XPP_GAMEPAD_TYPE 0x0027CE74u
#define PAD_HANDLE_BASE  0x00FAD000u         /* opaque to the title */

static int pad_enabled(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_NO_PAD") == NULL;
    return on;
}

static uint32_t pad_host_mask(void)
{
    uint32_t mask = 1;                         /* port 0: pad or keyboard */
    DWORD i;
    for (i = 1; i < 4; i++) {
        XINPUT_STATE st;
        if (XInputGetState(i, &st) == ERROR_SUCCESS)
            mask |= 1u << i;
    }
    return mask;
}

static int pad_keyboard_focus(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (fg)
        GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static int key_down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

extern void sub_0027DDAC_gen(void);
extern void sub_0027DDCE_gen(void);
extern void sub_0027DE3B_gen(void);
extern void sub_0027DE91_gen(void);
extern void sub_0027DE9D_gen(void);
extern void sub_0027E075_gen(void);

void sub_0027DDAC(void)                      /* XGetDevices */
{
    uint32_t type = MEM32(g_esp + 4), mask;
    if (!pad_enabled() || type != XPP_GAMEPAD_TYPE) {
        sub_0027DDAC_gen();
        return;
    }
    mask = pad_host_mask();
    {
        static uint32_t logged = 0xFFFFFFFFu;
        if (mask != logged) {
            logged = mask;
            fprintf(stderr, "  [PAD] XGetDevices: ports 0x%X\n", mask);
        }
    }
    MEM32(type) = mask;                        /* CurrentConnected */
    MEM32(type + 4) = 0;                       /* ChangeConnected */
    MEM32(type + 8) = mask;                    /* PreviousConnected */
    g_eax = mask;
    g_esp += 8;
}

void sub_0027DDCE(void)                      /* XGetDeviceChanges */
{
    uint32_t type = MEM32(g_esp + 4);
    uint32_t pins = MEM32(g_esp + 8), prem = MEM32(g_esp + 12);
    uint32_t cur, prev;
    if (!pad_enabled() || type != XPP_GAMEPAD_TYPE) {
        sub_0027DDCE_gen();
        return;
    }
    cur = pad_host_mask();
    prev = MEM32(type + 8);
    if (cur != prev) {
        /* Point 139, the four-player check: which ports the title now sees,
         * and which host pads are really there (port 0 is the keyboard when
         * host pad 0 is not). Host pad n drives port n -- a pad Windows puts
         * at index 2 plays as the third player whatever else is plugged in. */
        XINPUT_STATE st0;
        fprintf(stderr, "  [PAD] ports seen by the title 0x%X (were 0x%X), host pad 0 %s\n",
                cur, prev, XInputGetState(0, &st0) == ERROR_SUCCESS ? "present" : "absent");
    }
    MEM32(pins) = cur & ~prev;
    MEM32(prem) = prev & ~cur;
    MEM32(type) = cur;
    MEM32(type + 4) = 0;
    MEM32(type + 8) = cur;
    g_eax = (cur != prev);
    g_esp += 16;
}

void sub_0027DE3B(void)                      /* XInputOpen */
{
    uint32_t type = MEM32(g_esp + 4), port = MEM32(g_esp + 8);
    if (!pad_enabled() || type != XPP_GAMEPAD_TYPE) {
        sub_0027DE3B_gen();
        return;
    }
    g_eax = port < 4 ? PAD_HANDLE_BASE + port : 0;
    fprintf(stderr, "  [PAD] XInputOpen(port %u) -> 0x%08X\n", port, g_eax);
    g_esp += 20;
}

static int pad_handle(uint32_t h)
{
    return pad_enabled() && h >= PAD_HANDLE_BASE && h < PAD_HANDLE_BASE + 4;
}

void sub_0027DE91(void)                      /* XInputClose */
{
    if (!pad_handle(MEM32(g_esp + 4))) {
        sub_0027DE91_gen();
        return;
    }
    g_esp += 8;
}

void sub_0027DE9D(void)                      /* XInputGetCapabilities */
{
    uint32_t h = MEM32(g_esp + 4), caps = MEM32(g_esp + 8);
    if (!pad_handle(h)) {
        sub_0027DE9D_gen();
        return;
    }
    memset((void *)XBOX_PTR(caps), 0, 24);
    MEM8(caps) = 1;                            /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
    memset((void *)XBOX_PTR(caps + 2), 0xFF, 2);       /* every button reported */
    memset((void *)XBOX_PTR(caps + 4), 0xFF, 8);
    g_eax = 0;
    g_esp += 12;
}

/* Le journal des roles, sous RECOMP_ANIM_ROLES=1 (remaster, 2026-09-27) : l'heure commune
 * aux lignes [PAD] (la manette 1, a chaque changement) et [ROLE] (les animations qui
 * demarrent, voir anim_roles). Aligner les deux dit quelle animation le jeu joue pour
 * chaque action du joueur, sans rien choisir a la main. */
static int roles_actif(void)
{
    static int actif = -1;
    if (actif < 0)
        actif = getenv("RECOMP_ANIM_ROLES") ? 1 : 0;
    return actif;
}

static double roles_ms(void)
{
    static LARGE_INTEGER freq, t0;
    LARGE_INTEGER now;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&now);
    return 1000.0 * (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
}

/* F9, sous RECOMP_F9_MEMOIRE=1 (remaster, 2026-09-28) : toute la memoire du jeu (64 Mo, de 0x10000)
 * dans travail\memoire_NNN.bin, et la fenetre du jeu dans travail\memoire_NNN.bmp, au meme instant.
 * Hors ligne, l'image dit ce que voit la camera et la memoire ou elle est (position, visee, champ),
 * ainsi que les lumieres de l'arene. F9, pas F10 : F10 est deja lu par la fenetre de capture, et
 * GetAsyncKeyState partage son bit « appuye depuis » entre les lecteurs. */
static void f9_capture_fenetre(const char *chemin)
{
    HWND h = GetForegroundWindow();
    DWORD pid = 0;
    RECT r;
    if (!h) return;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !GetClientRect(h, &r)) return;
    int l = r.right - r.left, H = r.bottom - r.top;
    if (l <= 0 || H <= 0) return;
    HDC dc = GetDC(h), mdc = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, l, H);
    HGDIOBJ ancien = SelectObject(mdc, bmp);
    PrintWindow(h, mdc, 3);                 /* PW_CLIENTONLY | PW_RENDERFULLCONTENT */
    BITMAPINFOHEADER bi = { sizeof bi, l, -H, 1, 32, BI_RGB, 0, 0, 0, 0, 0 };
    size_t n = (size_t)l * (size_t)H * 4;
    uint8_t *px = (uint8_t *)malloc(n);
    if (px && GetDIBits(mdc, bmp, 0, (UINT)H, px, (BITMAPINFO *)&bi, DIB_RGB_COLORS)) {
        FILE *fo = fopen(chemin, "wb");
        if (fo) {
            BITMAPFILEHEADER bf = { 0x4D42, (DWORD)(sizeof bf + sizeof bi + n), 0, 0, (DWORD)(sizeof bf + sizeof bi) };
            fwrite(&bf, sizeof bf, 1, fo);
            fwrite(&bi, sizeof bi, 1, fo);
            fwrite(px, 1, n, fo);
            fclose(fo);
        }
    }
    free(px);
    SelectObject(mdc, ancien);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(h, dc);
}

static DWORD WINAPI f9_fil_capture(LPVOID p)
{
    f9_capture_fenetre((const char *)p);
    free(p);
    return 0;
}

static void f9_vidage(void)
{
    static int actif = -1, avant = 0, numero = 0;
    if (actif < 0)
        actif = getenv("RECOMP_F9_MEMOIRE") ? 1 : 0;
    if (!actif)
        return;
    int bas = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (!bas || avant) { avant = bas; return; }
    avant = 1;
    HWND h = GetForegroundWindow();
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId())
        return;
    char chemin[128];
    ++numero;
    /* la capture dans un fil a part : PrintWindow attend le fil de la fenetre, et le jeu ne doit
     * pas l'attendre (risque d'interblocage) ; quelques millisecondes d'ecart avec la memoire */
    char *image = (char *)malloc(64);
    if (image) {
        snprintf(image, 64, "travail\\memoire_%03d.bmp", numero);
        HANDLE th = CreateThread(NULL, 0, f9_fil_capture, image, 0, NULL);
        if (th) CloseHandle(th); else free(image);
    }
    snprintf(chemin, sizeof chemin, "travail\\memoire_%03d.bin", numero);
    FILE *fo = fopen(chemin, "wb");
    if (!fo) return;
    static uint8_t zeros[0x10000];
    for (uint32_t va = 0x00010000u; va < 0x04010000u; va += 0x10000u) {
        if (dsp_readable(va) && dsp_readable(va + 0xFFFC))
            fwrite((const void *)XBOX_PTR(va), 1, 0x10000, fo);
        else
            fwrite(zeros, 1, 0x10000, fo);
    }
    fclose(fo);
    fprintf(stderr, "  [F9] %.1f ms memoire et fenetre -> travail\\memoire_%03d\n", roles_ms(), numero);
    fflush(stderr);
}

static void roles_manette(uint16_t buttons, const uint8_t *analog, const int16_t *thumbs)
{
    static uint32_t avant = 0xFFFFFFFFu;
    uint32_t bits = 0, etat;
    int sx, sy;
    if (!roles_actif())
        return;
    for (int k = 0; k < 8; k++)
        if (analog[k] > 30) bits |= 1u << k;
    /* le stick gauche, en huit directions : ce qui compte, c'est quand il change */
    sx = thumbs[0] > 12000 ? 1 : thumbs[0] < -12000 ? -1 : 0;
    sy = thumbs[1] > 12000 ? 1 : thumbs[1] < -12000 ? -1 : 0;
    etat = (uint32_t)buttons | (bits << 8) | ((uint32_t)(sx + 1) << 16) | ((uint32_t)(sy + 1) << 18);
    if (etat == avant)
        return;
    avant = etat;
    fprintf(stderr, "  [PAD] %.1f ms croix/start %02X boutons A.B.X.Y.noir.blanc.G.D %u%u%u%u%u%u%u%u stick %+d %+d\n",
            roles_ms(), buttons, bits & 1, (bits >> 1) & 1, (bits >> 2) & 1, (bits >> 3) & 1,
            (bits >> 4) & 1, (bits >> 5) & 1, (bits >> 6) & 1, (bits >> 7) & 1, sx, sy);
    fflush(stderr);
}

#include "pad_aleatoire.h"

void sub_0027E075(void)                      /* XInputGetState */
{
    uint32_t h = MEM32(g_esp + 4), st = MEM32(g_esp + 8);
    uint32_t port;
    XINPUT_STATE xs;
    uint16_t buttons = 0;
    uint8_t analog[8] = {0};
    int16_t thumbs[4] = {0};
    static uint32_t packet[4];
    static uint8_t last[4][22];
    uint8_t out[22];

    if (!pad_handle(h)) {
        sub_0027E075_gen();
        return;
    }
    port = h - PAD_HANDLE_BASE;

    if (XInputGetState(port, &xs) == ERROR_SUCCESS) {
        const XINPUT_GAMEPAD *g = &xs.Gamepad;
        buttons = g->wButtons & 0x00FF;        /* dpad, start, back, thumbs */
        analog[0] = (g->wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
        analog[1] = (g->wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
        analog[2] = (g->wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
        analog[3] = (g->wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
        /* The actions this game puts on the Xbox triggers sit on L1/R1 in its
         * PS2 version, so the shoulder buttons drive the triggers and the
         * modern triggers take black and white. RECOMP_PAD_XBOX_LAYOUT=1
         * keeps the one-to-one layout (LT/RT = triggers, RB/LB = black/white). */
        static int xbox_layout = -1;
        if (xbox_layout < 0)
            xbox_layout = getenv("RECOMP_PAD_XBOX_LAYOUT") != NULL;
        if (xbox_layout) {
            analog[4] = (g->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0; /* black */
            analog[5] = (g->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;  /* white */
            analog[6] = g->bLeftTrigger;
            analog[7] = g->bRightTrigger;
        } else {
            analog[4] = g->bRightTrigger > 30 ? 255 : 0;                         /* black */
            analog[5] = g->bLeftTrigger > 30 ? 255 : 0;                          /* white */
            analog[6] = (g->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;  /* left trigger */
            analog[7] = (g->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0; /* right trigger */
        }
        thumbs[0] = g->sThumbLX; thumbs[1] = g->sThumbLY;
        thumbs[2] = g->sThumbRX; thumbs[3] = g->sThumbRY;
    }
    if (port == 0 && pad_keyboard_focus()) {
        if (key_down(VK_UP))     buttons |= 0x01;
        if (key_down(VK_DOWN))   buttons |= 0x02;
        if (key_down(VK_LEFT))   buttons |= 0x04;
        if (key_down(VK_RIGHT))  buttons |= 0x08;
        if (key_down(VK_RETURN)) buttons |= 0x10;
        if (key_down(VK_ESCAPE)) buttons |= 0x20;
        if (key_down(VK_SPACE))  analog[0] = 255;
        if (key_down('X'))       analog[1] = 255;
        if (key_down('C'))       analog[2] = 255;
        if (key_down('V'))       analog[3] = 255;
        if (key_down('Q'))       analog[4] = 255;
        if (key_down('E'))       analog[5] = 255;
        if (key_down('A'))       analog[6] = 255;
        if (key_down('D'))       analog[7] = 255;
    }

    /* RECOMP_AUTOPILOTE : la manette 1 suit un script (autopilote.c), pour les
     * essais sans joueur ; la vraie manette et le clavier sont alors ignores. */
    if (port == 0)
        autopilote(&buttons, analog, thumbs);

    /* RECOMP_PAD_AUTOSTART=N: hold START for a quarter second every N
     * seconds, and A right after it -- for unattended test runs. */
    if (port == 0 && getenv("RECOMP_PAD_AUTOSTART")) {
        ULONGLONG period = (ULONGLONG)atoi(getenv("RECOMP_PAD_AUTOSTART")) * 1000;
        static ULONGLONG t0;
        ULONGLONG phase;
        if (!t0)
            t0 = GetTickCount64() - 1000;      /* first press at `period` */
        phase = period ? (GetTickCount64() - t0) % period : 0;
        if (period && phase < 250)
            buttons |= 0x10;
        else if (period && phase >= 500 && phase < 750)
            analog[0] = 255;
    }

    /* Y on the fighter select: a fighter at random (pad_aleatoire.c, point 156). */
    if (port == 0)
        pad_aleatoire(&buttons, analog);

    memset(out, 0, sizeof out);
    memcpy(out + 4, &buttons, 2);
    memcpy(out + 6, analog, 8);
    memcpy(out + 14, thumbs, 8);
    if (port == 0) {
        roles_manette(buttons, analog, thumbs);
        f9_vidage();
    }
    if (memcmp(out + 4, last[port] + 4, 18) != 0) {
        packet[port]++;
        memcpy(last[port], out, sizeof out);
    }
    memcpy(out, &packet[port], 4);
    memcpy((void *)XBOX_PTR(st), out, sizeof out);
    g_eax = 0;
    g_esp += 12;
}

/* 0x0027E0E8: XInputSetState(handle, *feedback) — la vibration.
 *
 * C'est la seule fonction manette de XAPI qui manquait à la liste ci-dessus,
 * et elle plantait le jeu : on lui passe *notre* handle (0x00FAD000 + port),
 * qu'elle déréférence comme un objet périphérique USB (`handle + 0xA3`), donc
 * sur un pointeur invalide. Le crash n'apparaissait qu'une fois les
 * sauvegardes visibles, parce que le jeu allait alors plus loin.
 *
 * L'appel est asynchrone sur Xbox : le titre lit `dwStatus` en tête de la
 * structure de retour pour savoir si c'est fini. On répond « terminé, sans
 * erreur » et on renvoie la consigne à la vraie manette.
 *
 * Réserve : la position des deux mots de vitesse (0x42 et 0x44, après un
 * en-tête de 66 octets) vient de la disposition du SDK Xbox, pas d'une
 * vérification en jeu. Si la vibration se révélait fausse, c'est la première
 * chose à revoir — elle ne peut rien casser d'autre.
 */
extern void sub_0027E0E8_gen(void);

void sub_0027E0E8(void)                      /* XInputSetState */
{
    uint32_t h  = MEM32(g_esp + 4);
    uint32_t fb = MEM32(g_esp + 8);
    uint32_t port;

    if (!pad_handle(h)) {
        sub_0027E0E8_gen();
        return;
    }
    port = h - PAD_HANDLE_BASE;

    if (fb && dsp_readable(fb + 0x44)) {
        XINPUT_VIBRATION v;
        v.wLeftMotorSpeed  = (WORD)(MEM32(fb + 0x42) & 0xFFFF);
        v.wRightMotorSpeed = (WORD)((MEM32(fb + 0x44) >> 0) & 0xFFFF);
        XInputSetState(port, &v);
        MEM32(fb) = 0;           /* dwStatus = ERROR_SUCCESS : requête finie */
    }

    g_eax = 0;                   /* ERROR_SUCCESS */
    g_esp += 12;                 /* ret 8 */
}

/* 0x00078390: front-end screen tick. It stops ticking the Flash movie while
 * [[this+0x20]+0x206] says the active controller is gone. */
extern void sub_00078390_gen(void);
void sub_00078390(void)
{
    if (apt_trace_on()) {
        static ULONGLONG next;
        ULONGLONG now = GetTickCount64();
        if (now >= next) {
            uint32_t self = g_ecx, ctl = MEM32(self + 0x20);
            next = now + 2000;
            fprintf(stderr, "  [SCREEN] tick this=0x%08X ctl=0x%08X +205=%u +206=%u"
                            " +20C=0x%08X +210=0x%08X paused=%u 346A58=%u\n",
                    self, ctl,
                    ctl && dsp_readable(ctl + 0x210) ? MEM8(ctl + 0x205) : 0xFF,
                    ctl && dsp_readable(ctl + 0x210) ? MEM8(ctl + 0x206) : 0xFF,
                    ctl && dsp_readable(ctl + 0x210) ? MEM32(ctl + 0x20C) : 0,
                    ctl && dsp_readable(ctl + 0x214) ? MEM32(ctl + 0x210) : 0,
                    MEM8(self + 0x24), MEM8(0x346A58));
        }
    }
    sub_00078390_gen();
}

/* 0x00146050: APT per-frame tick, argument = elapsed milliseconds. */
extern void sub_00146050_gen(void);
void sub_00146050(void)
{
    if (apt_trace_on()) {
        static LONG n;
        static LONG64 total;
        static ULONGLONG first;
        LONG k = InterlockedIncrement(&n);
        total += (int)MEM32(g_esp + 4);
        if (!first)
            first = GetTickCount64();
        if ((k & (k - 1)) == 0 && k >= 64)
            fprintf(stderr, "  [APT] %ld ticks: %lld ms given, %llu ms elapsed\n", k,
                    (long long)total, (unsigned long long)(GetTickCount64() - first));
        if (k <= 4 || (k & (k - 1)) == 0)
            fprintf(stderr, "  [APT] tick #%ld elapsed=%d from 0x%08X at %llu ms\n", k,
                    (int)MEM32(g_esp + 4), MEM32(g_esp),
                    (unsigned long long)GetTickCount64());
    }
    sub_00146050_gen();
}

/* 0x001DDCCA: EA raster conversion (dst, arg, src). Logged under
 * RECOMP_RASTER_TRACE to see which image overflows its destination. */
extern void sub_001DDCCA_gen(void);
void sub_001DDCCA(void)
{
    if (getenv("RECOMP_RASTER_TRACE")) {
        uint32_t d = MEM32(g_esp + 4), a = MEM32(g_esp + 8), s = MEM32(g_esp + 12);
        char line[256];
        int o = snprintf(line, sizeof line, "  [RASTER] dst 0x%08X arg 0x%08X src 0x%08X fmt '%c' |",
                         d, a, s, MEM8(0x3C86E8));
        for (int k = 0; k < 8 && dsp_readable(d + k * 4); k++)
            o += snprintf(line + o, sizeof line - o, " %08X", MEM32(d + k * 4));
        o += snprintf(line + o, sizeof line - o, " | src");
        for (int k = 0; k < 6 && s && dsp_readable(s + k * 4); k++)
            o += snprintf(line + o, sizeof line - o, " %08X", MEM32(s + k * 4));
        fprintf(stderr, "%s\n", line);
    }
    sub_001DDCCA_gen();
}

/* 0x001DDC11: EA raster row writer (?, src row, count), destination in edi. */
extern void sub_001DDC11_gen(void);
void sub_001DDC11(void)
{
    if (getenv("RECOMP_RASTER_TRACE")) {
        static LONG n;
        int32_t count = (int32_t)MEM32(g_esp + 12);
        LONG k = InterlockedIncrement(&n);
        if (k <= 6 || count <= 0 || count > 4096)
            fprintf(stderr, "  [ROW] #%ld a1 0x%08X src 0x%08X count %d edi 0x%08X ret 0x%08X\n",
                    k, MEM32(g_esp + 4), MEM32(g_esp + 8), count, g_edi, MEM32(g_esp));
    }
    sub_001DDC11_gen();
}

/* 0x0020144E: MSVC _CIfmod, x87 register convention: st1 = x, st0 = y,
 * result fmod(x, y) in st0 with one value popped.
 *
 * The CRT reaches the arithmetic through _trandisp2 (0x0020592C), which
 * classifies its arguments and dispatches through a table; lifted, that path
 * returns the wrong value. EA's APT uses fmod to fold the angle it recovers
 * from an element's matrix, so every tilted menu element was rebuilt at the
 * wrong rotation. Done natively here. */
void sub_0020144E(void)
{
    double y = g_fp_stack[g_fp_top & 7];
    double x = g_fp_stack[(g_fp_top + 1) & 7];
    g_fp_top = (g_fp_top + 1) & 7;
    g_fp_stack[g_fp_top] = fmod(x, y);
    g_esp += 4;
}

/* 0x000298F0: creates the title's base heap from (base, size). */
extern void sub_000298F0_gen(void);
void sub_000298F0(void)
{
    fprintf(stderr, "  [ALLOC] base heap at 0x%08X, %u bytes\n",
            MEM32(g_esp + 4), MEM32(g_esp + 8));
    sub_000298F0_gen();
}

/* ------------------------------------------------------------------ *
 * Sondes de la racine de chemin des sauvegardes (point 75).
 *
 * Le chemin demande est « ???004100420043\SaveMeta.xbx » : les trois
 * premiers octets, la racine, sont de la memoire non initialisee. Le
 * constructeur est maintenant identifie :
 *
 *   sub_0019C0B0(this, device)   monte le peripherique
 *       word[device] == 4  -> ecrit « U:\ » dans this+0x534
 *       sinon              -> XMountMU(port, slot, this+0x534)
 *     puis recopie this+0x534 dans this+0x538, et selon *(int*)(device+4)
 *     (1, 2 ou 3) appelle sub_001F7991(drive, titleid, this+0x538), qui
 *     remplace this+0x538 par la racine reellement montee.
 *
 *   sub_0019C320(this, ...)      ouvre un fichier de la sauvegarde
 *     sprintf(buf, "%s%s\%s", this+0x538, dossier, fichier)
 *
 * Les deux sondes disent donc : est-ce que le montage a lieu, sur quel
 * objet, avec quel type de peripherique, et ce que vaut la racine des
 * deux cotes. Si les deux « this » different, l'objet qui ouvre n'est pas
 * celui qu'on a monte. Sous RECOMP_SAVE_TRACE.
 * ------------------------------------------------------------------ */

/* Rend au plus 32 octets d'une chaine invitee, en hexa et en texte. */
#define SAVE_SHOW_MAX 32
static void save_show_root(const char *label, uint32_t va)
{
    char txt[SAVE_SHOW_MAX + 1];
    char hex[SAVE_SHOW_MAX * 3 + 1];
    int i, n = 0;

    if (!dsp_readable(va)) {
        fprintf(stderr, "%s <illisible 0x%08X>", label, va);
        return;
    }
    txt[0] = 0;
    for (i = 0; i < SAVE_SHOW_MAX; i++) {
        unsigned b = MEM8(va + i);
        n += snprintf(hex + n, sizeof hex - (size_t)n, "%02X ", b);
        txt[i] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        if (!b) break;
    }
    txt[i < SAVE_SHOW_MAX ? i : SAVE_SHOW_MAX] = 0;
    fprintf(stderr, "%s « %s » [%s]", label, txt, hex);
}

extern void sub_0019C0B0_gen(void);

void sub_0019C0B0(void)                      /* montage du peripherique */
{
    uint32_t self   = g_ecx;
    uint32_t device = MEM32(g_esp + 4);
    unsigned type = 0, slot = 0, mode = 0, tid = 0;

    if (!save_traced()) { sub_0019C0B0_gen(); return; }

    if (dsp_readable(device + 8)) {
        type = MEM32(device) & 0xFFFFu;
        slot = (MEM32(device) >> 16) & 0xFFFFu;
        mode = MEM32(device + 4);
        tid  = MEM32(device + 8);
    }
    fprintf(stderr, "  [ROOT] montage : this=0x%08X device=0x%08X "
            "type=%u slot=%u mode=%u titleid=0x%08X\n",
            self, device, type, slot, mode, tid);
    fflush(stderr);

    sub_0019C0B0_gen();

    fprintf(stderr, "  [ROOT] montage fini, eax=%u ;", g_eax);
    save_show_root(" +0x534", self + 0x534);
    save_show_root(" ; +0x538", self + 0x538);
    fprintf(stderr, "\n");
    fflush(stderr);
}

extern void sub_0019C320_gen(void);

void sub_0019C320(void)                      /* ouverture dans la sauvegarde */
{
    uint32_t self = g_ecx;

    if (!save_traced()) { sub_0019C320_gen(); return; }

    fprintf(stderr, "  [ROOT] ouverture (C320) : this=0x%08X ;", self);
    save_show_root(" racine +0x538", self + 0x538);
    save_show_root(" ; +0x534", self + 0x534);
    fprintf(stderr, "\n");
    fflush(stderr);

    sub_0019C320_gen();
}

/* La chaine reelle, mesuree (point 75) :
 *
 *   sub_0019CB40(this, dev, dossierW, "SaveMeta.xbx", &taille)
 *       -> sub_001F879A(this+0x538, dossierW, 3, 0, tampon, cch)   XCreateSaveGame
 *
 * XCreateSaveGame commence par recopier sa racine (param_1) dans son
 * tampon local, y appose le nom hexa du dossier, puis rend le tout dans
 * param_5. Les trois octets manquants sont exactement la longueur de
 * « U:\ », donc soit la racine d'entree est deja perdue, soit c'est cette
 * fonction qui la perd. Ces deux sondes tranchent : elles impriment la
 * racine a l'entree de chacune, et le tampon rendu a la sortie.
 */
extern void sub_0019CB40_gen(void);

void sub_0019CB40(void)                      /* ouvre un fichier de sauvegarde */
{
    uint32_t self = g_ecx;
    uint32_t file = MEM32(g_esp + 0xC);

    if (!save_traced()) { sub_0019CB40_gen(); return; }

    fprintf(stderr, "  [ROOT] ouverture : this=0x%08X fichier=0x%08X ;", self, file);
    save_show_root(" racine +0x538", self + 0x538);
    fprintf(stderr, "\n");
    fflush(stderr);

    sub_0019CB40_gen();
}

extern void sub_001F879A_gen(void);

void sub_001F879A(void)                      /* XCreateSaveGame */
{
    uint32_t root = MEM32(g_esp + 4);
    uint32_t out  = MEM32(g_esp + 0x14);

    if (!save_traced()) { sub_001F879A_gen(); return; }

    fprintf(stderr, "  [ROOT] XCreateSaveGame : racine=0x%08X", root);
    save_show_root(" ->", root);
    fprintf(stderr, " ; sortie=0x%08X\n", out);
    fflush(stderr);

    sub_001F879A_gen();

    fprintf(stderr, "  [ROOT] XCreateSaveGame fini, eax=%u ;", g_eax);
    save_show_root(" rendu", out);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* La pile de la trace [REL] designe celle-ci, pas XCreateSaveGame :
 * sub_001F85BD est le helper de XFindFirstSaveGame / XFindNextSaveGame.
 * Pour chaque entree de repertoire trouvee il construit
 *
 *     <racine><cFileName>\SaveMeta.xbx      dans pFindData + 0x140
 *
 * puis l'ouvre pour y lire le nom d'affichage de la sauvegarde. Les trois
 * octets perdus sont exactement la racine. Signature mesuree :
 *   sub_001F85BD(pFindData, racine, longueur_racine)
 * La sonde imprime les trois arguments, le tampon avant, et ce qui y est
 * ecrit apres : si la racine d'entree est bonne et le tampon faux, la
 * perte est dans cette fonction. */
extern void sub_001F85BD_gen(void);

void sub_001F85BD(void)
{
    uint32_t data = MEM32(g_esp + 4);
    uint32_t root = MEM32(g_esp + 8);
    uint32_t rlen = MEM32(g_esp + 0xC);
    uint32_t attr = dsp_readable(data) ? MEM32(data) : 0;

    if (!save_traced()) { sub_001F85BD_gen(); return; }

    fprintf(stderr, "  [ROOT] FindSaveGame : data=0x%08X attr=0x%08X rlen=%u racine=0x%08X",
            data, attr, rlen, root);
    save_show_root(" ->", root);
    save_show_root(" ; nom", data + 0x2C);
    fprintf(stderr, "\n");
    fflush(stderr);

    sub_001F85BD_gen();

    fprintf(stderr, "  [ROOT] FindSaveGame fini, eax=%u ;", g_eax);
    save_show_root(" chemin", data + 0x140);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* Un cran plus haut : sub_0019CE60 (FindFirst du gestionnaire de
 * sauvegarde) appelle sub_001F8ACD (XFindFirstSaveGame) avec
 * (this+0x538, this+0x1E0), et c'est cette derniere qui appelle le helper
 * pour chaque entree. Le helper recoit un pFindData nul : ces deux sondes
 * disent lequel des deux etages perd l'argument. */
extern void sub_0019CE60_gen(void);

void sub_0019CE60(void)
{
    uint32_t self = g_ecx;

    if (!save_traced()) { sub_0019CE60_gen(); return; }

    fprintf(stderr, "  [ROOT] FindFirst : this=0x%08X data=0x%08X racine=0x%08X",
            self, self + 0x1E0, self + 0x538);
    save_show_root(" ->", self + 0x538);
    fprintf(stderr, "\n");
    fflush(stderr);

    sub_0019CE60_gen();

    fprintf(stderr, "  [ROOT] FindFirst fini, eax=0x%08X ;", g_eax);
    save_show_root(" racine", self + 0x538);
    fprintf(stderr, "\n");
    fflush(stderr);
}

extern void sub_001F8ACD_gen(void);

void sub_001F8ACD(void)                      /* XFindFirstSaveGame */
{
    uint32_t root = MEM32(g_esp + 4);
    uint32_t data = MEM32(g_esp + 8);

    if (!save_traced()) { sub_001F8ACD_gen(); return; }

    fprintf(stderr, "  [ROOT] XFindFirst : racine=0x%08X data=0x%08X", root, data);
    save_show_root(" ->", root);
    fprintf(stderr, "\n");
    fflush(stderr);

    sub_001F8ACD_gen();

    fprintf(stderr, "  [ROOT] XFindFirst fini, eax=0x%08X\n", g_eax);
    fflush(stderr);
}

/* ------------------------------------------------------------------ *
 * Le mode Story : qui detruit esi dans l'interpreteur ActionScript ?
 *
 * sub_001596F0 (ActionCallMethod de la VM Flash) garde son objet VM dans
 * esi, charge une seule fois a l'entree et jamais reaffecte. Au plantage
 * esi vaut 0, et le prologue n'avait pas faute -- donc un appele l'a
 * detruit. Entre le dernier point ou esi etait sain (0x00159B99, meme
 * motif d'acces qui n'a pas faute) et l'instruction fautive (0x00159C08)
 * il n'y a que trois appels : deux fois sub_00203458 (_stricmp) et un
 * appel indirect. _stricmp preserve esi en assembleur, y compris par son
 * saut de queue vers sub_002082A0 -- reste a savoir si notre traduction
 * le fait aussi.
 *
 * Ces deux sondes ne coutent que trois comparaisons par appel et ne
 * parlent qu'en cas de violation, une fois par fonction.
 * ------------------------------------------------------------------ */
static void abi_watch(const char *who, uint32_t ebx0, uint32_t esi0,
                      uint32_t edi0)
{
    if (g_ebx == ebx0 && g_esi == esi0 && g_edi == edi0)
        return;
    {
        static int said[2];
        int slot = who[0] == 's' ? 0 : 1;
        if (said[slot]++ > 4)
            return;
    }
    fprintf(stderr, "  [ABI] %s ne preserve pas : "
            "ebx %08X->%08X  esi %08X->%08X  edi %08X->%08X\n",
            who, ebx0, g_ebx, esi0, g_esi, edi0, g_edi);
    fflush(stderr);
}

extern void sub_00203458_gen(void);

void sub_00203458(void)                      /* _stricmp */
{
    uint32_t b = g_ebx, s = g_esi, d = g_edi;
    sub_00203458_gen();
    abi_watch("sub_00203458", b, s, d);
}

extern void sub_002082A0_gen(void);

void sub_002082A0(void)                      /* le coeur de _stricmp */
{
    uint32_t b = g_ebx, s = g_esi, d = g_edi;
    sub_002082A0_gen();
    abi_watch("worker sub_002082A0", b, s, d);
}

/* ------------------------------------------------------------------ *
 * Mode Story : l'objet VM arrive-t-il deja nul ?
 *
 * La trace des appels indirects au plantage a corrige une deduction
 * fausse : les cinq derniers appels indirects sont tous des flottants
 * (0xBF6BEB20, 0xC0330000), donc tous sautes. Aucun appele n'a donc pu
 * detruire esi -- il etait nul avant. Le prologue de sub_001596F0 lit
 * pourtant [esi], [esi+8] et [edx+eax*4-4] sans fauter : avec esi nul
 * cela lit les adresses invitees 0 et 8, dont le contenu suffit a former
 * une adresse mappee. A 0x00159C02 le meme calcul part de 0x30 et 0x38,
 * qui valent zero, d'ou la lecture a 0xFFFFFFFC.
 *
 * Ces deux sondes disent qui passe un objet nul : l'appelee et son
 * appelant direct. Elles ne parlent que pour un objet nul ou illisible,
 * et impriment aussi les mots invites que le prologue va lire.
 * ------------------------------------------------------------------ */
extern void sub_001596F0_gen(void);

void sub_001596F0(void)                      /* ActionCallMethod de la VM */
{
    uint32_t vm = MEM32(g_esp + 4);

    if (vm == 0 || !dsp_readable(vm + 0x38)) {
        static int said;
        if (said++ < 8) {
            fprintf(stderr, "  [VM] CallMethod recoit un objet nul : "
                    "param_1=0x%08X param_2=0x%08X retour=0x%08X\n",
                    vm, MEM32(g_esp + 8), MEM32(g_esp));
            fprintf(stderr, "  [VM]   mots invites : [0]=0x%08X [8]=0x%08X "
                    "[0x30]=0x%08X [0x38]=0x%08X\n",
                    MEM32(0), MEM32(8), MEM32(0x30), MEM32(0x38));
            fflush(stderr);
        }
    }
    sub_001596F0_gen();
}

extern void sub_0015A2F0_gen(void);

void sub_0015A2F0(void)                      /* son appelant direct */
{
    uint32_t vm = MEM32(g_esp + 4);

    if (vm == 0 || !dsp_readable(vm + 0x38)) {
        static int said;
        if (said++ < 8) {
            fprintf(stderr, "  [VM] sub_0015A2F0 a deja un objet nul : "
                    "param_1=0x%08X retour=0x%08X\n", vm, MEM32(g_esp));
            fflush(stderr);
        }
    }
    sub_0015A2F0_gen();
}

/* Nomme chaque asset cherche dans une archive, sous RECOMP_ASSET_TRACE.
 *
 * C'est la sonde qui a tranche d'ou vient le modele de combat (voir mod.md) :
 * pas de fighter.xml mais de assets/djv2.sod. Elle vivait jusqu'ici collee a
 * la main dans le C genere, ou toute regeneration l'effacait ; elle est ici
 * maintenant, ou elle survit. Premier argument : le nom, en esp+4.
 */
extern void sub_001E46E0_gen(void);

void sub_001E46E0(void)                      /* recherche d'un asset */
{
    static int on = -1;

    if (on < 0)
        on = getenv("RECOMP_ASSET_TRACE") != NULL;
    if (on) {
        uint32_t va = MEM32(g_esp + 4);
        if (va && dsp_readable(va)) {
            const char *n = (const char *)XBOX_PTR(va);
            if ((unsigned char)n[0] >= 0x20 && (unsigned char)n[0] < 0x7F) {
                fprintf(stderr, "  [ASSET] %.80s\n", n);
                fflush(stderr);
            }
        }
    }
    sub_001E46E0_gen();
}

/* Le dossier mods\ : une entree d'archive remplacee par un fichier libre.
 *
 * Mesure (RECOMP_ASSET_TRACE, une partie complete) : le systeme de fichiers
 * d'EA cherche un fichier libre avant ses archives, mais seulement pour un
 * CHEMIN NU ("assets/blingtex.xsh", les XML, les flux audio). Une demande de la
 * forme "archive|entree" -- "assets\v2ip.viv|V2IP_121A.o" -- va droit dans
 * l'archive : 0 fichier libre essaye sur 495 demandes, et c'est la forme de
 * tout ce qu'on veut modder (personnages, arenes, armes, animations, polices).
 *
 * Quand le fichier existe dans game_files\mods, on reecrit donc la demande en
 * chemin nu, et le jeu l'ouvre lui-meme par son mecanisme "libre d'abord" ; le
 * calque du toolkit (kernel_path.c) le trouve dans mods. Correspondance :
 *
 *     assets\v2ip.viv|V2IP_121A.o          -> mods\assets\v2ip\V2IP_121A.o
 *     assets/ai/ai.viv|assets/ai/x.xml      -> mods\assets\ai\x.xml
 *
 * (une entree qui porte deja un chemin garde le sien). sub_001E5C00 est la
 * requete d'ouverture par nom, le seul point par ou passent les sept chemins
 * d'appel ; elle recopie le nom, donc le tampon n'a a vivre que pendant
 * l'appel. RECOMP_MODS=0 coupe tout. */
extern void sub_001E5C00_gen(void);
extern BOOL xbox_ModsHas(const char *disc_relative);
extern uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);

#define MODS_SLOTS    64
#define MODS_SLOT_LEN 256

static uint32_t mods_redirect(uint32_t va)
{
    static volatile LONG pool, next;
    const char *name, *bar;
    char rel[MODS_SLOT_LEN];
    size_t n;
    uint32_t slot;

    if (!va || !dsp_readable(va))
        return 0;
    name = (const char *)XBOX_PTR(va);
    bar = strchr(name, '|');
    if (!bar || bar - name >= MODS_SLOT_LEN / 2)
        return 0;

    if (strpbrk(bar + 1, "/\\")) {
        snprintf(rel, sizeof(rel), "%s", bar + 1);
    } else {
        const char *dot = NULL, *p;
        for (p = name; p < bar; p++)
            if (*p == '.') dot = p;
            else if (*p == '/' || *p == '\\') dot = NULL;
        n = (size_t)((dot ? dot : bar) - name);
        snprintf(rel, sizeof(rel), "%.*s/%s", (int)n, name, bar + 1);
    }
    for (char *p = rel; *p; p++)
        if (*p == '\\') *p = '/';

    if (!xbox_ModsHas(rel))
        return 0;

    if (!pool) {
        uint32_t a = xbox_ContiguousAlloc(MODS_SLOTS * MODS_SLOT_LEN, 16);
        if (!a)
            return 0;
        InterlockedCompareExchange(&pool, (LONG)a, 0);
    }
    slot = (uint32_t)pool
         + ((uint32_t)InterlockedIncrement(&next) % MODS_SLOTS) * MODS_SLOT_LEN;
    memcpy((void *)XBOX_PTR(slot), rel, strlen(rel) + 1);
    fprintf(stderr, "  [MODS] %.120s -> %s\n", name, rel);
    fflush(stderr);
    return slot;
}

void sub_001E5C00(void)                      /* ouverture d'un fichier par nom */
{
    static int on = -1, trace = -1;
    uint32_t esp0 = g_esp;
    uint32_t va = MEM32(esp0 + 4);
    uint32_t red = 0;

    if (on < 0) {
        const char *m = getenv("RECOMP_MODS");
        const char *t = getenv("RECOMP_ASSET_TRACE");
        on = !(m && *m == '0');
        trace = t && *t == '2';
    }
    /* RECOMP_ASSET_TRACE=2 : toutes les ouvertures par nom, pas seulement
     * celles que sub_001E46E0 voit passer -- c'est ce qui dit si une archive
     * est lue autrement que par la forme archive|entree. */
    if (trace && va && dsp_readable(va)) {
        fprintf(stderr, "  [OUVRE] %.120s\n", (const char *)XBOX_PTR(va));
        fflush(stderr);
    }
    if (on)
        red = mods_redirect(va);
    if (red)
        MEM32(esp0 + 4) = red;
    sub_001E5C00_gen();
    if (red)
        MEM32(esp0 + 4) = va;
}

/* Les textures des films EAGL, sous RECOMP_EAGL_TRACE (point 106).
 *
 * Une forme d'un film nomme sa texture par un symbole
 * "__EAGL::TAR:::RUNTIME_ALLOC::UID=n;SHAPENAME=97,1;...". Le constructeur
 * (sub_00102130) en fait la cle "shape_97", la cherche dans le registre global
 * (sub_00119470), puis dans le module avec le type SHAPE (sub_0010A440, qui
 * retombe sur un resolveur propre au module), et prend sinon une texture de
 * repli integree a l'executable (le damier 'banc', a 0x2E39A8) -- c'est le
 * carre orange qu'a donne le premier ajout de vignettes. Ces trois sondes
 * disent quelle cle est cherchee, ou, et ce qui revient. */
extern void sub_00102130_gen(void);
extern void sub_00119470_gen(void);
extern void sub_0010A440_gen(void);

static int eagl_trace_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_EAGL_TRACE") != NULL;
    return on;
}

static const char *eagl_str(uint32_t va)
{
    return (va && dsp_readable(va)) ? (const char *)XBOX_PTR(va) : "";
}

void sub_00102130(void)                      /* constructeur de TAR (cdecl) */
{
    uint32_t desc = MEM32(g_esp + 4), module = MEM32(g_esp + 8);
    sub_00102130_gen();
    if (eagl_trace_on() && strstr(eagl_str(desc), "SHAPENAME=")) {
        fprintf(stderr, "  [EAGL] TAR %.60s module=0x%08X -> 0x%08X\n",
                strstr(eagl_str(desc), "SHAPENAME="), module, g_eax);
        fflush(stderr);
    }
}

void sub_00119470(void)                      /* registre global : recherche */
{
    uint32_t reg = g_ecx, key = MEM32(g_esp + 4), found = MEM32(g_esp + 8);
    sub_00119470_gen();
    if (eagl_trace_on() && strncmp(eagl_str(key), "shape_", 6) == 0) {
        fprintf(stderr, "  [EAGL]   registre 0x%08X %-10s trouve=%u -> 0x%08X\n", reg,
                eagl_str(key), dsp_readable(found) ? MEM8(found) : 0xFF, g_eax);
        fflush(stderr);
    }
}

/* Le nom d'une image SHPX, lu dans son enregistrement 0x70 (au plus 8
 * enregistrements parcourus) ; "" si l'adresse n'en est pas une. */
static void eagl_shpx_name(uint32_t e, char *out, size_t n)
{
    int k;
    out[0] = 0;
    for (k = 0; k < 8 && e && dsp_readable(e); k++) {
        uint32_t w = MEM32(e), next = w >> 8;
        if ((w & 0xFF) == 0x70) {
            snprintf(out, n, "%.12s", (const char *)XBOX_PTR(e + 4));
            return;
        }
        if (!next || next > 0x400000)
            return;
        e += next;
    }
}

extern void sub_00101EB0_gen(void);

void sub_00101EB0(void)                      /* attacher une image a une texture (thiscall) */
{
    uint32_t tar = g_ecx, img = MEM32(g_esp + 4), ret = MEM32(g_esp);
    sub_00101EB0_gen();
    if (eagl_trace_on()) {
        char nom[16];
        eagl_shpx_name(img, nom, sizeof nom);
        fprintf(stderr, "  [EAGL] attache TAR 0x%08X <- image 0x%08X '%s' (appelant 0x%08X)\n",
                tar, img, nom, ret);
        fflush(stderr);
    }
}

void sub_0010A440(void)                      /* module : GetAddr(type, nom) */
{
    uint32_t mod = g_ecx, type = MEM32(g_esp + 4), key = MEM32(g_esp + 8);
    uint32_t out = MEM32(g_esp + 12);
    uint32_t cb = dsp_readable(mod + 0x1C) ? MEM32(mod + 0x1C) : 0;
    sub_0010A440_gen();
    if (eagl_trace_on() && strncmp(eagl_str(key), "shape_", 6) == 0) {
        fprintf(stderr, "  [EAGL]   module 0x%08X %s %-10s rappel=0x%08X trouve=%u -> 0x%08X\n",
                mod, eagl_str(type), eagl_str(key), cb, g_eax & 0xFF,
                dsp_readable(out) ? MEM32(out) : 0);
        fflush(stderr);
    }
}

/* La texture d'un combattant, sous RECOMP_EAGL_TRACE (point 108).
 *
 * Le TAR d'un modele de combattant ne trouve rien a sa construction et prend
 * le damier 'banc', comme celui d'un film ; la vraie image vient plus tard.
 * sub_000978D0 (fastcall, l'objet combattant en ecx) prend la premiere image
 * du .xsh charge en +0x870, en fait une texture, et la pose par
 * sub_00026290 sur le groupe de TAR du modele qui porte le NOM de cette image
 * (sub_00112450 : table de groupes [nom, n, n TAR, bits] en +0xBC). Ces
 * sondes disent quel nom est cherche, et quels groupes le modele a. */
extern void sub_000978D0_gen(void);
extern void sub_00112450_gen(void);

void sub_000978D0(void)                      /* texture du combattant (fastcall) */
{
    uint32_t self = g_ecx;
    uint32_t modele = dsp_readable(self + 8) ? MEM32(self + 8) : 0;
    uint32_t c = self + 0x870, img = 0;
    char nom[16] = "";
    if (dsp_readable(c) && MEM32(c) && dsp_readable(MEM32(c) + 0x14)) {
        img = MEM32(MEM32(c) + 0x14) + MEM32(c);
        eagl_shpx_name(img, nom, sizeof nom);
    }
    sub_000978D0_gen();
    if (eagl_trace_on()) {
        fprintf(stderr, "  [EAGL] combattant 0x%08X modele '%.12s' image0 0x%08X '%s' -> texture 0x%08X\n",
                self, eagl_str(modele), img, nom,
                dsp_readable(self + 0x880) ? MEM32(self + 0x880) : 0);
        fflush(stderr);
    }
}

void sub_00112450(void)                      /* groupe de TAR par nom (thiscall) */
{
    uint32_t self = g_ecx, out = MEM32(g_esp + 4), name = MEM32(g_esp + 8);
    sub_00112450_gen();
    if (eagl_trace_on() && name) {
        uint32_t n = dsp_readable(out + 4) ? MEM32(out + 4) : 0;
        fprintf(stderr, "  [EAGL]   groupe '%.16s' dans 0x%08X -> %u TAR\n", eagl_str(name), self, n);
        if (n == 0 && dsp_readable(self + 0xBC)) {
            uint32_t p = MEM32(self + 0xBC);
            int k;
            for (k = 0; k < 32 && p && dsp_readable(p) && MEM32(p); k++) {
                uint32_t cnt = MEM32(p + 4), bits = p + 8 + 4 * cnt, nb;
                fprintf(stderr, "  [EAGL]     a '%.16s' (%u TAR)\n", eagl_str(MEM32(p)), cnt);
                if (cnt > 4096 || !dsp_readable(bits))
                    break;
                nb = ((MEM16(bits) + 7u) >> 3) + 2u;
                p = bits + ((nb + 3u) & ~3u);
            }
        }
        fflush(stderr);
    }
}

/* Les classes d'animation, sous RECOMP_ANIM_TRACE (point 117).
 *
 * Une piste d'animation est un objet C++ : le jeu la demande a sa banque par son
 * nom (sub_00021890), puis appelle sa table de methodes -- +0x18 pour la poser
 * sur un squelette, +0x30 pour autre chose. Le premier mot de l'objet porte donc
 * la table, mais le FICHIER n'en dit rien : le chargeur l'y ecrit. Cette sonde
 * releve, pour les premieres pistes demandees, le nom, l'objet et sa table, ce
 * qui donne l'adresse du code qui sait lire les cles. */
extern void sub_00021890_gen(void);
static void anim_retenir_nom(uint32_t obj, uint32_t nom);

void sub_00021890(void)                      /* CAnimationBank::Get(nom) (thiscall) */
{
    uint32_t nom = MEM32(g_esp + 4);
    static int reste = -1;
    if (reste < 0)
        reste = getenv("RECOMP_ANIM_TRACE") ? 40 : 0;
    sub_00021890_gen();
    if (reste > 0) {
        uint32_t obj = g_eax;
        uint32_t vt = (obj && dsp_readable(obj)) ? MEM32(obj) : 0;
        reste--;
        fprintf(stderr, "  [ANIM] %-14s -> objet 0x%08X, table 0x%08X"
                        " (+0x18 = 0x%08X, +0x30 = 0x%08X)\n",
                eagl_str(nom), obj, vt,
                (vt && dsp_readable(vt + 0x18)) ? MEM32(vt + 0x18) : 0,
                (vt && dsp_readable(vt + 0x30)) ? MEM32(vt + 0x30) : 0);
        fflush(stderr);
    }
    anim_retenir_nom(g_eax, nom);
    if (roles_actif()) {
        /* une demande par nom, au plus une ligne par nom et par demi-seconde */
        static struct { uint32_t nom; double vu; } dem[256];
        double t = roles_ms();
        int i = (int)((nom >> 2) & 255);
        if (dem[i].nom != nom || t - dem[i].vu > 500.0) {
            fprintf(stderr, "  [ROLE] %.1f ms demande %s -> objet %08X\n", t, eagl_str(nom), g_eax);
            fflush(stderr);
        }
        dem[i].nom = nom;
        dem[i].vu = t;
    }
}

/* Les cles d'animation, sous RECOMP_ANIM_KEYS=<n> (point 117).
 *
 * La table 0x002B5850 est un composite : ses methodes +0x18 (0x000EDF90) et
 * +0x30 (0x000EE000) appellent la meme methode de chaque enfant, un par canal de
 * la piste. Le canal 0x13 est lu par la classe 0x002B5DE0 (init 0x000F4600,
 * echantillonnage 0x000F2580), qui ecrit un quaternion par os a
 * sortie + 48*rang + 0x10. Cette sonde releve, apres l'appel du composite, pour
 * chaque enfant : sa table, son canal (octets bruts, liste d'os, table des
 * temps) et les 48 octets de sortie de chacun de ses os -- de quoi comparer un
 * decodeur hors jeu a ce que le jeu calcule, sans passer par la peau. */
static int anim_cles_reste = -1;
static struct { uint32_t obj, piste; char nom[32]; } anim_noms[2048];
static int anim_noms_n;

static void anim_retenir_nom(uint32_t obj, uint32_t nom)
{
    static int racine = -1;
    if (anim_cles_reste < 0) {
        const char *s = getenv("RECOMP_ANIM_KEYS");
        anim_cles_reste = s ? atoi(s) : 0;
    }
    if (racine < 0)
        racine = getenv("RECOMP_ANIM_RACINE") ? 1 : 0;
    if ((anim_cles_reste <= 0 && !racine && !roles_actif()) || !obj || !dsp_readable(obj + 0xC))
        return;
    int i = anim_noms_n++ & 2047;
    anim_noms[i].obj = obj;
    anim_noms[i].piste = MEM32(obj + 0xC);
    snprintf(anim_noms[i].nom, sizeof anim_noms[i].nom, "%s", eagl_str(nom));
}

static const char *anim_nom_de(uint32_t obj, uint32_t piste)
{
    for (int k = 0; k < 2048; k++) {
        int i = (anim_noms_n - 1 - k) & 2047;
        if (k >= anim_noms_n) break;
        if (anim_noms[i].piste == piste || anim_noms[i].obj == obj)
            return anim_noms[i].nom;
    }
    return "?";
}

static void anim_hex(const char *tag, uint32_t va, uint32_t n)
{
    for (uint32_t o = 0; o < n; o += 32) {
        fprintf(stderr, "  [AK]   %s %08X:", tag, va + o);
        for (uint32_t k = o; k < n && k < o + 32; k++) {
            if (!dsp_readable(va + k)) { fprintf(stderr, " .."); continue; }
            fprintf(stderr, " %02X", MEM8(va + k));
        }
        fprintf(stderr, "\n");
    }
}

static void anim_cles(int meth, uint32_t self, uint32_t fbits, uint32_t out, uint32_t p3)
{
    static uint32_t derniere[64];
    static int vues[64];
    if (anim_cles_reste <= 0 || !dsp_readable(self + 0x14))
        return;
    uint32_t piste = MEM32(self + 0xC), enf = MEM32(self + 0x10);
    if (!dsp_readable(piste + 8) || !dsp_readable(enf))
        return;
    uint32_t n = MEM16(piste + 8), utile = 0;
    for (uint32_t i = 0; i < n && i < 8; i++) {
        uint32_t ch = MEM32(enf + 4 * i), c = dsp_readable(ch + 0xC) ? MEM32(ch + 0xC) : 0;
        uint32_t t = (c && dsp_readable(c)) ? MEM16(c) : 0;
        if (t == 0x12 || t == 0x13) utile = 1;
    }
    if (!utile)
        return;
    /* au plus trois appels de suite par piste, pour en voir beaucoup */
    int slot = (int)((piste >> 2) & 63);
    if (derniere[slot] != piste) { derniere[slot] = piste; vues[slot] = 0; }
    if (vues[slot]++ >= 3)
        return;
    anim_cles_reste--;
    float f; memcpy(&f, &fbits, 4);
    fprintf(stderr, "  [AK] meth +0x%02X piste %08X (%s) id %04X image %.6f sortie %08X p3 %08X"
                    " drapeau %u mult %u\n",
            meth, piste, anim_nom_de(self, piste), MEM16(piste + 2), f, out, p3,
            MEM8(self + 0x14), MEM8(self + 0x15));
    for (uint32_t i = 0; i < n && i < 8; i++) {
        uint32_t ch = MEM32(enf + 4 * i);
        uint32_t vt = dsp_readable(ch) ? MEM32(ch) : 0;
        uint32_t c = dsp_readable(ch + 0xC) ? MEM32(ch + 0xC) : 0;
        uint32_t t = (c && dsp_readable(c + 8)) ? MEM16(c) : 0;
        fprintf(stderr, "  [AK]  enfant %u table %08X canal %08X type %04X\n", i, vt, c, t);
        if (t != 0x12 && t != 0x13)
            continue;
        uint32_t nk = MEM16(c + 4), nb = MEM8(c + 6), tm = MEM32(c + 8), os = MEM32(c + 0xC);
        anim_hex("canal", c, 0x800);
        if (dsp_readable(os)) anim_hex("os", os, nb);
        if (tm && dsp_readable(tm)) anim_hex("temps", tm, nk * 2);
        if (!dsp_readable(os) || !dsp_readable(out))
            continue;
        for (uint32_t b = 0; b < nb; b++) {
            uint32_t r = MEM8(os + b), s = out + 48 * r;
            float v[12];
            for (int k = 0; k < 12; k++) {
                uint32_t w = dsp_readable(s + 4 * k) ? MEM32(s + 4 * k) : 0;
                memcpy(&v[k], &w, 4);
            }
            fprintf(stderr, "  [AK]   sortie os %2u : %.7g %.7g %.7g %.7g | %.7g %.7g %.7g %.7g"
                            " | %.7g %.7g %.7g %.7g\n", r,
                    v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
        }
    }
    fflush(stderr);
}

/* Des instantanes de la memoire des combattants, sous RECOMP_INSTANTANES=1 (remaster, 2026-09-27).
 *
 * Les objets d'animation des deux combattants sont a 0x00349xxx et 0x00354xxx (registre ebx, point
 * 10 du remaster) : un bloc d'environ 44 Ko par combattant. Toutes les 250 ms, les 128 Ko de
 * 0x00340000 a 0x00360000 vont dans travail\instantanes.bin (en-tete : "ISNS", heure en ms, base,
 * taille). Hors ligne, on y cherche la position des combattants (depart connu : gamecore.xml), leur
 * vitesse de marche et leur recul. */
static void roles_instantane(void)
{
    static int actif = -1;
    static FILE *fo;
    static double dernier = -1e9;
    const uint32_t base = 0x00340000u, taille = 0x00020000u;
    if (actif < 0) {
        actif = getenv("RECOMP_INSTANTANES") ? 1 : 0;
        if (actif) {
            fo = fopen("travail\\instantanes.bin", "wb");
            if (!fo) actif = 0;
        }
    }
    if (!actif)
        return;
    double t = roles_ms();
    if (t - dernier < 250.0 || !dsp_readable(base) || !dsp_readable(base + taille - 4))
        return;
    dernier = t;
    uint32_t tete[4] = { 0x534E5349u /* ISNS */, (uint32_t)t, base, taille };
    fwrite(tete, 4, 4, fo);
    fwrite((const void *)XBOX_PTR(base), 1, taille, fo);
    fflush(fo);
}

/* Le canal 0x0D, sous RECOMP_ANIM_0D=<n> (remaster, 2026-09-27, GUIDE_REMASTER.md point 11).
 *
 * Classe 0x002B5670 : echantillonnage 0x000ED300, decodage 0x000FC3F0. Pour chaque enfant 0x0D
 * d'une piste a plusieurs quaternions : l'en-tete du canal et ses indices de sortie, le flux
 * (en-tete, triplets de flottants, rangees de deltas), la table des temps, puis les quaternions que
 * le jeu vient d'ecrire (sortie + 4 * indice). Le decodeur du remaster
 * (D:\djffnyremaster\pipeline\verifier_0d.py) refait le calcul et compare. */
static void anim_0d(int meth, uint32_t self, uint32_t fbits, uint32_t out)
{
    static int reste = -1;
    static uint32_t derniere[64];
    static int vues[64];
    if (reste < 0) {
        const char *s = getenv("RECOMP_ANIM_0D");
        reste = s ? atoi(s) : 0;
    }
    if (reste <= 0 || !dsp_readable(self + 0x14))
        return;
    uint32_t piste = MEM32(self + 0xC), enf = MEM32(self + 0x10);
    if (!dsp_readable(piste + 8) || !dsp_readable(enf) || !dsp_readable(out))
        return;
    uint32_t n = MEM16(piste + 8), utile = 0;
    for (uint32_t i = 0; i < n && i < 8; i++) {
        uint32_t ch = MEM32(enf + 4 * i), c = dsp_readable(ch + 0xC) ? MEM32(ch + 0xC) : 0;
        uint32_t fl = (c && dsp_readable(c + 0x10)) ? MEM32(c + 4) : 0;
        if (MEM16(c) == 0x0D && fl && dsp_readable(fl + 4) && MEM16(fl) > 4) utile = 1;
    }
    if (!utile)
        return;
    int slot = (int)((piste >> 2) & 63);
    if (derniere[slot] != piste) { derniere[slot] = piste; vues[slot] = 0; }
    if (vues[slot]++ >= 3)
        return;
    reste--;
    float f; memcpy(&f, &fbits, 4);
    fprintf(stderr, "  [0D] meth +0x%02X piste %08X (%s) image %.6f sortie %08X\n",
            meth, piste, anim_nom_de(self, piste), f, out);
    for (uint32_t i = 0; i < n && i < 8; i++) {
        uint32_t ch = MEM32(enf + 4 * i), c = dsp_readable(ch + 0xC) ? MEM32(ch + 0xC) : 0;
        if (!c || !dsp_readable(c + 0x10) || MEM16(c) != 0x0D)
            continue;
        uint32_t fl = MEM32(c + 4), tm = MEM32(c + 8), nk = MEM16(c + 0xC);
        if (!fl || !dsp_readable(fl + 4))
            continue;
        uint32_t nv = MEM16(fl), nq = (nv + 3) / 4, bits = MEM8(fl + 2);
        uint32_t rangee = bits == 16 ? 2 * nv : bits == 4 ? (nv + 1) / 2 : nv;
        fprintf(stderr, "  [0D]  enfant %u canal %08X flux %08X temps %08X cles %u valeurs %u bits %u\n",
                i, c, fl, tm, nk, nv, bits);
        anim_hex("0Dcanal", c, 0x0E + 2 * nq);
        anim_hex("0Dflux", fl, 4 + 12 * nv + rangee * nk);
        if (tm && dsp_readable(tm)) anim_hex("0Dtemps", tm, 2 * nk);
        for (uint32_t q = 0; q < nq; q++) {
            uint32_t idx = MEM16(c + 0x0E + 2 * q), s = out + 4 * idx;
            float v[4];
            for (int k = 0; k < 4; k++) {
                uint32_t w = dsp_readable(s + 4 * k) ? MEM32(s + 4 * k) : 0;
                memcpy(&v[k], &w, 4);
            }
            fprintf(stderr, "  [0D]   q %2u indice %4u : %.9g %.9g %.9g %.9g\n", q, idx, v[0], v[1], v[2], v[3]);
        }
    }
    fflush(stderr);
}

/* La piste _t (deplacement de la racine), sous RECOMP_ANIM_RACINE=<n> (remaster, 2026-09-27).
 *
 * Meme composite que les pistes _s. Ses enfants de type 0x14 (classe 0x002B5D50,
 * echantillonnage 0x000F14F0) et 0x15 (classe 0x002B5CC8, 0x000EFD80) ecrivent
 * chaque valeur a sortie + 4 * indice (indices 8 a 10 dans les banques mesurees).
 * Cette sonde releve, apres l'appel, pour chaque enfant 0x14 ou 0x15 : les octets
 * bruts du canal, la liste des indices, la table des temps, puis les 16 premiers
 * flottants de la sortie -- de quoi comparer le decodeur hors jeu du remaster
 * (D:\djffnyremaster\pipeline\piste_t.py --verifier) a ce que le jeu ecrit. */
static void anim_hex_ar(const char *tag, uint32_t va, uint32_t n)
{
    for (uint32_t o = 0; o < n; o += 32) {
        fprintf(stderr, "  [AR]   %s %08X:", tag, va + o);
        for (uint32_t k = o; k < n && k < o + 32; k++) {
            if (!dsp_readable(va + k)) { fprintf(stderr, " .."); continue; }
            fprintf(stderr, " %02X", MEM8(va + k));
        }
        fprintf(stderr, "\n");
    }
}

static void anim_racine(int meth, uint32_t self, uint32_t fbits, uint32_t out)
{
    static int reste = -1;
    static uint32_t derniere[64];
    static int vues[64];
    if (reste < 0) {
        const char *s = getenv("RECOMP_ANIM_RACINE");
        reste = s ? atoi(s) : 0;
    }
    if (reste <= 0 || !dsp_readable(self + 0x14))
        return;
    uint32_t piste = MEM32(self + 0xC), enf = MEM32(self + 0x10);
    if (!dsp_readable(piste + 8) || !dsp_readable(enf))
        return;
    uint32_t n = MEM16(piste + 8), utile = 0;
    for (uint32_t i = 0; i < n && i < 8; i++) {
        uint32_t ch = MEM32(enf + 4 * i), c = dsp_readable(ch + 0xC) ? MEM32(ch + 0xC) : 0;
        uint32_t t = (c && dsp_readable(c)) ? MEM16(c) : 0;
        if (t == 0x14 || t == 0x15) utile = 1;
    }
    if (!utile)
        return;
    /* au plus trois appels de suite par piste, pour en voir beaucoup */
    int slot = (int)((piste >> 2) & 63);
    if (derniere[slot] != piste) { derniere[slot] = piste; vues[slot] = 0; }
    if (vues[slot]++ >= 3)
        return;
    reste--;
    float f; memcpy(&f, &fbits, 4);
    /* l'heure de l'appel : l'avancee de l'image entre deux appels, rapportee au temps
     * ecoule, donne la cadence des animations (30 ou 60 images/s, jamais mesuree) */
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    fprintf(stderr, "  [AR] meth +0x%02X piste %08X (%s) image %.6f sortie %08X t %.3f ms\n",
            meth, piste, anim_nom_de(self, piste), f, out,
            1000.0 * (double)now.QuadPart / (double)freq.QuadPart);
    for (uint32_t i = 0; i < n && i < 8; i++) {
        uint32_t ch = MEM32(enf + 4 * i);
        uint32_t c = dsp_readable(ch + 0xC) ? MEM32(ch + 0xC) : 0;
        uint32_t t = (c && dsp_readable(c + 0x12)) ? MEM16(c) : 0;
        fprintf(stderr, "  [AR]  enfant %u canal %08X type %04X\n", i, c, t);
        if (t != 0x14 && t != 0x15)
            continue;
        uint32_t nk = MEM16(c + 0xC), np = MEM16(c + 0xE), nc = MEM8(c + 0x11);
        uint32_t liste = MEM32(c + 4), tm = MEM32(c + 8);
        anim_hex_ar("canal", c, 0x800);
        if (liste && dsp_readable(liste)) anim_hex_ar("liste", liste, (np + nc) * 2);
        if (tm && dsp_readable(tm)) anim_hex_ar("temps", tm, nk * 2);
    }
    if (dsp_readable(out) && dsp_readable(out + 60)) {
        fprintf(stderr, "  [AR]  sortie :");
        for (int k = 0; k < 16; k++) {
            uint32_t w = MEM32(out + 4 * k);
            float v; memcpy(&v, &w, 4);
            fprintf(stderr, " %.7g", v);
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* Les animations qui demarrent, sous RECOMP_ANIM_ROLES=1 (voir roles_manette).
 *
 * Chaque appel du composite echantillonne une piste dans un tampon de pose (la sortie).
 * Quand une piste apparait sur une sortie ou elle n'avait pas ete echantillonnee depuis
 * 100 ms, une ligne [ROLE] : l'heure, la sortie, la methode, le nom et l'image. Une sortie
 * par combattant, c'est ce que le journal dira. (Mesure : non, les sorties sont des
 * tampons partages.) D'ou le contexte de l'appel en fin de ligne -- l'objet, l'adresse de
 * retour, ebx, esi, edi, ebp, p3 et quatre mots de pile -- pour trouver lequel dit le
 * combattant. */
static void anim_roles(int meth, uint32_t self, uint32_t fbits, uint32_t out, const uint32_t *ctx)
{
    static struct { uint32_t out, piste; double vu; } vus[1024];
    uint32_t piste;
    double t;
    int i, libre = -1, vieux = 0;
    if (!roles_actif() || !dsp_readable(self + 0x10))
        return;
    piste = MEM32(self + 0xC);
    t = roles_ms();
    for (i = 0; i < 1024; i++) {
        if (vus[i].out == out && vus[i].piste == piste) {
            double ecart = t - vus[i].vu;
            vus[i].vu = t;
            if (ecart < 100.0)
                return;
            break;
        }
        if (!vus[i].out && libre < 0)
            libre = i;
        if (vus[i].vu < vus[vieux].vu)
            vieux = i;
    }
    if (i == 1024) {
        i = libre >= 0 ? libre : vieux;
        vus[i].out = out;
        vus[i].piste = piste;
        vus[i].vu = t;
    }
    float f; memcpy(&f, &fbits, 4);
    fprintf(stderr, "  [ROLE] %.1f ms sortie %08X meth +%02X piste %08X %s image %.2f"
                    " ctx %08X %08X %08X %08X %08X %08X %08X | %08X %08X %08X %08X\n",
            t, out, meth, piste, anim_nom_de(self, piste), f,
            self, ctx[0], ctx[1], ctx[2], ctx[3], ctx[4], ctx[5], ctx[6], ctx[7], ctx[8], ctx[9]);
    fflush(stderr);
}

extern void sub_000EDF90_gen(void);
extern void sub_000EE000_gen(void);

void sub_000EDF90(void)                      /* composite d'animation, methode +0x18 */
{
    uint32_t self = g_ecx, f = MEM32(g_esp + 4), out = MEM32(g_esp + 8), p3 = MEM32(g_esp + 12);
    uint32_t ctx[10] = { MEM32(g_esp), g_ebx, g_esi, g_edi, g_ebp, p3,
                         MEM32(g_esp + 16), MEM32(g_esp + 20), MEM32(g_esp + 24), MEM32(g_esp + 28) };
    sub_000EDF90_gen();
    anim_cles(0x18, self, f, out, p3);
    anim_racine(0x18, self, f, out);
    anim_roles(0x18, self, f, out, ctx);
    anim_0d(0x18, self, f, out);
    roles_instantane();
}

void sub_000EE000(void)                      /* composite d'animation, methode +0x30 */
{
    uint32_t self = g_ecx, f = MEM32(g_esp + 4), out = MEM32(g_esp + 8), p3 = MEM32(g_esp + 12);
    uint32_t ctx[10] = { MEM32(g_esp), g_ebx, g_esi, g_edi, g_ebp, p3,
                         MEM32(g_esp + 16), MEM32(g_esp + 20), MEM32(g_esp + 24), MEM32(g_esp + 28) };
    sub_000EE000_gen();
    anim_cles(0x30, self, f, out, p3);
    anim_racine(0x30, self, f, out);
    anim_roles(0x30, self, f, out, ctx);
    anim_0d(0x30, self, f, out);
}

/* Le menu de debogage du titre, sous RECOMP_DEBUG_MENU (point 113).
 *
 * Le module CRunModuleAki dessine, a chaque image, l'une de HUIT pages de
 * debogage -- la premiere ecrit "DEBUG MENU -- %s" et le mode en cours
 * (sub_0002CA20) -- mais tout est garde par un octet en +0x21 que son
 * constructeur met a zero et que rien ne remet a 1. La page est choisie par le
 * mot en +0x2A0.
 *
 * La sonde force les deux a l'entree de la fonction de dessin (sub_0002DB00,
 * thiscall : le module en ecx). RECOMP_DEBUG_MENU=1 allume la page 0,
 * =4 la quatrieme, et ainsi de suite ; la page 2 n'existe pas dans le switch. */
extern void sub_0002DB00_gen(void);
extern void sub_00077B20_gen(void);

/* La boucle d'image du combat. Elle tient le module aki en +0x14 et l'appelle
 * pour dessiner ; c'est le seul endroit sur d'y arriver, puisque la sonde
 * posee sur le dessin lui-meme n'a jamais ete appelee (essai du 2026-09-23). */
void sub_00077B20(void)
{
    static int dit = 0;
    uint32_t self = g_ecx;
    uint32_t mod = dsp_readable(self + 0x14) ? MEM32(self + 0x14) : 0;
    const char *v = getenv("RECOMP_DEBUG_MENU");
    if (v && mod && dsp_readable(mod + 0x2A0)) {
        int page = *v ? atoi(v) - 1 : 0;
        if (page < 0)
            page = 0;
        MEM8(mod + 0x21) = 1;
        MEM32(mod + 0x2A0) = (uint32_t)page;
        if (!dit) {
            dit = 1;
            fprintf(stderr, "[DEBUG MENU] boucle de combat 0x%08X, module 0x%08X, page %d\n",
                    self, mod, page);
            fflush(stderr);
        }
    }
    sub_00077B20_gen();
}

void sub_0002DB00(void)                      /* CRunModuleAki::Draw (thiscall) */
{
    static int page = -2;
    if (page == -2) {
        const char *v = getenv("RECOMP_DEBUG_MENU");
        page = (v == NULL) ? -1 : (*v ? atoi(v) - 1 : 0);
        if (page < 0 && v != NULL)
            page = 0;
    }
    if (page >= 0 && dsp_readable(g_ecx + 0x2A0)) {
        static int dit = 0;
        uint32_t texte = MEM32(0x002FB148u + 4 * 7);
        if (!dit) {
            dit = 1;
            fprintf(stderr, "[DEBUG MENU] module 0x%08X, octet +0x21 = %u, mode %u -> page %d,"
                            " sous-systeme texte 0x%08X (drapeau %u)\n",
                    g_ecx, MEM8(g_ecx + 0x21), MEM32(g_ecx + 0x2A0), page, texte,
                    dsp_readable(texte + 1) ? MEM8(texte + 1) : 0xFF);
            fflush(stderr);
        }
        MEM8(g_ecx + 0x21) = 1;
        MEM32(g_ecx + 0x2A0) = (uint32_t)page;
    }
    sub_0002DB00_gen();
}

/* L'ecran large 16:9, sous RECOMP_WIDESCREEN.
 *
 * Le moteur EAGL porte deja le 16:9 : sub_0001A4E0 rend les parametres de
 * projection de la vue principale selon un mode range en this+0x10, et le
 * mode 1 ecrit 1,7778 au lieu de 1,3333. Rien ne le choisit jamais --
 * sub_0001A360, le setter du mode, n'est appele nulle part dans les 17 423
 * fonctions du titre, et le constructeur (sub_0001A610, depuis
 * CRenderSubsystem::Init) passe 0 en dur.
 *
 * Ce que les deux sorties valent, mesure dans sub_00103E10 :
 *
 *     m00 = cotan(fov/2) * A            fov est l'angle HORIZONTAL, en degres
 *     m11 = cotan(fov/2) * B * aspect   l'aspect n'agit que sur la verticale
 *
 * Donc toucher au seul aspect donne du Vert- : meme champ horizontal, champ
 * vertical resserre d'un quart. C'est geometriquement juste une fois l'image
 * etiree, et c'est ce que le titre ferait de lui-meme (son mode 1) -- mais a
 * l'ecran ca se voit comme un zoom avant, et l'utilisateur l'a dit des le
 * premier essai. Le patch grand ecran PCSX2 de la version PS2 fait l'inverse
 * et c'est la bonne reference : il met 0,75 sur le champ horizontal, donc il
 * elargit au lieu de rogner.
 *
 * Hor+ (le defaut, RECOMP_WIDESCREEN=1) : on garde la verticale de la version
 * 4:3 et on ouvre l'horizontale d'un tiers.
 *
 *     tan(fov'/2) = tan(fov/2) * 4/3        ->  m00' = 0,75 * m00
 *     aspect'     = 16/9                    ->  m11' = m11, inchange
 *
 * Les plans de culling se recalent tout seuls : sub_00103E10 les tire de
 * tan(fov'/2) pour l'horizontale et de tan(fov'/2)/aspect' pour la verticale,
 * et ce second rapport ne bouge pas. Sans ca la geometrie gagnee sur les
 * cotes serait calculee puis jetee.
 *
 * RECOMP_WIDESCREEN=2 garde l'ancien comportement (Vert-, cadrage rogne),
 * pour comparer deux passages sur la meme scene plutot que juger de memoire.
 *
 * Seul le mode 0 est touche -- celui que le titre utilise. Le mode 1
 * retrecit en plus le viewport a 3/4 de sa hauteur (letterbox dans une image
 * 4:3), ce qui rognerait l'interface 2D, et le mode 2 prend le rapport du
 * viewport : tous deux restent au titre. La camera hors ecran de
 * sub_000177E0 a son propre aspect et ne passe pas par ici.
 */
extern void sub_0001A4E0_gen(void);

/* 0 = eteint, 1 = Hor+ (defaut), 2 = Vert- */
int recomp_widescreen_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("RECOMP_WIDESCREEN");
        if (!e || !*e || *e == '0') mode = 0;
        else if (*e == '2')         mode = 2;
        else                        mode = 1;
    }
    return mode;
}

void sub_0001A4E0(void)                      /* PerspectiveViewport::GetParams */
{
    /* thiscall, ret 0x14 : le corps genere depile les arguments, donc ils
     * sont lus avant l'appel. */
    uint32_t mode    = MEM32(g_esp + 4);
    uint32_t p_fov   = MEM32(g_esp + 8);
    uint32_t p_asp   = MEM32(g_esp + 0xC);
    int      ws      = recomp_widescreen_mode();

    sub_0001A4E0_gen();

    if (mode != 0 || !ws || !p_asp || !dsp_readable(p_asp))
        return;

    MEM32(p_asp) = 0x3FE38E39u;              /* 16/9 */

    if (ws == 1 && p_fov && dsp_readable(p_fov)) {
        union { uint32_t u; float f; } v;
        v.u = MEM32(p_fov);
        if (v.f > 0.5f && v.f < 179.0f) {
            const double DEG = 3.14159265358979323846 / 180.0;
            double demi = tan((double)v.f * 0.5 * DEG) * (4.0 / 3.0);
            v.f = (float)(2.0 * atan(demi) / DEG);
            MEM32(p_fov) = v.u;
        }
    }

    {
        static int said;
        if (!said++) {
            union { uint32_t u; float f; } a, g;
            a.u = MEM32(p_asp);
            g.u = p_fov && dsp_readable(p_fov) ? MEM32(p_fov) : 0;
            fprintf(stderr, "  [16:9] vue principale : aspect %.4f, fov horizontal %.3f deg"
                    " (%s)\n", a.f, g.f,
                    ws == 1 ? "Hor+, champ horizontal elargi d'un tiers"
                            : "Vert-, champ vertical rogne d'un quart");
            fflush(stderr);
        }
    }
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * Overrides are matched here for indirect calls; direct calls reach the
     * definitions above through the link step (--exclude-manual).
     */
    if (xbox_va >= 0x00256E61u && xbox_va <= 0x0025C9B5u) {   /* DirectSound */
        recomp_func_t f = son_hle_porte(xbox_va);
        if (f) return f;
    }
    if (xbox_va == 0x002016B0) return sub_002016B0;   /* memmove */
    if (xbox_va == 0x002091E3) return sub_002091E3;   /* CRT thread-exit callback */
    if (xbox_va == 0x00077040) return sub_00077040;   /* screen-state probe */
    if (xbox_va == 0x00017000) return sub_00017000;   /* liste du profil : ajoutes debloques */
    if (xbox_va == 0x00087AF0) return sub_00087AF0;   /* entree des coreID clones */
    if (xbox_va == 0x0003E170) return sub_0003E170;   /* voix des coreID 104 et plus */
    if (xbox_va == 0x001A47B0) return sub_001A47B0;   /* main_all.dat : paires au-dela de 141 */
    if (xbox_va == 0x001A7F30) return sub_001A7F30;   /* lecteurs des paires (g_paires_va) */
    if (xbox_va == 0x001B2090) return sub_001B2090;
    if (xbox_va == 0x001B4930) return sub_001B4930;
    if (xbox_va == 0x001B4BA0) return sub_001B4BA0;
    if (xbox_va == 0x000A3E20) return sub_000A3E20;   /* activation du Blazin propre a un coreID */
    if (xbox_va == 0x00032170) return sub_00032170;   /* demande du cri d'activation (relais) */
    if (xbox_va == 0x000379E0) return sub_000379E0;   /* cri d'activation : pas de relance */
    if (xbox_va == 0x0017F080) return sub_0017F080;   /* formes d'un film Apt : plus de 512 numeros */
    if (xbox_va == 0x00087E20) return sub_00087E20;   /* activation : celle du combattant qui active */
    if (xbox_va == 0x001A8260) return sub_001A8260;   /* Blazin : nombre et drapeaux */
    if (xbox_va == 0x001A8180) return sub_001A8180;   /* Blazin : les clones ne sont pas le heros */
    if (xbox_va == 0x001A8150) return sub_001A8150;   /* Blazin : noms des animations du mod */
    if (xbox_va == 0x001A8BC0) return sub_001A8BC0;   /* Blazin : face ou dos selon le nom */
    if (xbox_va == 0x001D0F80) return sub_001D0F80;   /* Blazin : face ou dos selon le nom */
    if (xbox_va == 0x00216410) return sub_00216410;   /* D3D timed callback probe */
    if (xbox_va == 0x000D7EA0) return sub_000D7EA0;   /* input wake probe */
    if (xbox_va == 0x002231D0) return sub_002231D0;   /* vblank service probe */
    if (xbox_va == 0x002166E0) return sub_002166E0;   /* callback walker probe */
    if (xbox_va == 0x001931B0) return sub_001931B0;   /* bytecode interpreter */
    if (xbox_va == 0x000771E0) return sub_000771E0;   /* screen loader thread */
    if (xbox_va == 0x00144CB0) return sub_00144CB0;   /* APT trace */
    if (xbox_va == 0x00069CC0) return sub_00069CC0;
    if (xbox_va == 0x001819C0) return sub_001819C0;
    if (xbox_va == 0x00146D60) return sub_00146D60;
    if (xbox_va == 0x00148210) return sub_00148210;
    if (xbox_va == 0x001480B0) return sub_001480B0;
    if (xbox_va == 0x001E9AB0) return sub_001E9AB0;   /* named allocator trace */
    if (xbox_va == 0x000298F0) return sub_000298F0;
    if (xbox_va == 0x00146050) return sub_00146050;
    if (xbox_va == 0x0020144E) return sub_0020144E;   /* _CIfmod */
    if (xbox_va == 0x001DDCCA) return sub_001DDCCA;
    if (xbox_va == 0x001DDC11) return sub_001DDC11;
    if (xbox_va == 0x00078390) return sub_00078390;
    if (xbox_va == 0x0027DDAC) return sub_0027DDAC;   /* gamepad (XAPI) */
    if (xbox_va == 0x0027DDCE) return sub_0027DDCE;
    if (xbox_va == 0x0027DE3B) return sub_0027DE3B;
    if (xbox_va == 0x0027DE91) return sub_0027DE91;
    if (xbox_va == 0x0027DE9D) return sub_0027DE9D;
    if (xbox_va == 0x0027E075) return sub_0027E075;
    if (xbox_va == 0x0027E0E8) return sub_0027E0E8;   /* XInputSetState */
    if (xbox_va == 0x0012BE20) return sub_0012BE20;   /* sonde : float -> int16 */
    if (xbox_va == 0x0012C310) return sub_0012C310;   /* sonde : decodeur */
    if (xbox_va == 0x00052990) return sub_00052990;   /* sonde : QueryBootupCheck */
    if (xbox_va == 0x000524B0) return sub_000524B0;   /* sonde : BootupLoad */
    if (xbox_va == 0x00052EE0) return sub_00052EE0;   /* sonde : LoadGlobal */
    if (xbox_va == 0x00052B60) return sub_00052B60;   /* sonde : QuerySave */
    if (xbox_va == 0x00052730u) return sub_00052730;   /* ChooseCopySrcCB */
    if (xbox_va == 0x00052770u) return sub_00052770;   /* ChooseCopyDst */
    if (xbox_va == 0x00052AB0u) return sub_00052AB0;   /* ChooseCopyDstCB */
    if (xbox_va == 0x00052AF0u) return sub_00052AF0;   /* ConfirmSaveCB */
    if (xbox_va == 0x00052E70u) return sub_00052E70;   /* ChooseSaveDstCB */
    if (xbox_va == 0x00052B40u) return sub_00052B40;   /* ConfirmLoadCB */
    if (xbox_va == 0x00052860u) return sub_00052860;   /* ConfirmCardRemovedCB */
    if (xbox_va == 0x000528B0u) return sub_000528B0;   /* DeleteID */
    if (xbox_va == 0x00052D70u) return sub_00052D70;   /* GetNumUsers */
    if (xbox_va == 0x00052F90u) return sub_00052F90;   /* SetNewUserID */
    if (xbox_va == 0x00052CE0u) return sub_00052CE0;   /* GetUserData */
    if (xbox_va == 0x00052D10u) return sub_00052D10;   /* PopupResponse */
    if (xbox_va == 0x00052E40u) return sub_00052E40;   /* Save */
    if (xbox_va == 0x00052E60u) return sub_00052E60;   /* QueryLoad */
    if (xbox_va == 0x00052ED0u) return sub_00052ED0;   /* Load */
    if (xbox_va == 0x00052930u) return sub_00052930;   /* ConfirmLoadGlobalCB */
    if (xbox_va == 0x000528E0u) return sub_000528E0;   /* SetCurrentUserIndex */
    if (xbox_va == 0x00052470u) return sub_00052470;   /* SetBMCurrentUserIndex */
    if (xbox_va == 0x00052820u) return sub_00052820;   /* BootupLoadCB */
    if (xbox_va == 0x00052E20u) return sub_00052E20;   /* CreateNewUser */
    if (xbox_va == 0x00052DE0u) return sub_00052DE0;   /* MCOPComplete */
    if (xbox_va == 0x00052F30u) return sub_00052F30;   /* MCOPComplete2 */
    if (xbox_va == 0x00052EF0u) return sub_00052EF0;   /* RetryChooseMCDeviceCB */
    if (xbox_va == 0x00052F00u) return sub_00052F00;   /* WantAutosaveFailedCB */
    if (xbox_va == 0x00052F10u) return sub_00052F10;   /* DeleteScreen */
    if (xbox_va == 0x0019C0B0u) return sub_0019C0B0;   /* sonde : montage racine */
    if (xbox_va == 0x0019C320u) return sub_0019C320;   /* sonde : ouverture sauvegarde */
    if (xbox_va == 0x0019CB40u) return sub_0019CB40;   /* sonde : ouverture sauvegarde */
    if (xbox_va == 0x001F879Au) return sub_001F879A;   /* sonde : XCreateSaveGame */
    if (xbox_va == 0x001F85BDu) return sub_001F85BD;   /* sonde : helper FindSaveGame */
    if (xbox_va == 0x0019CE60u) return sub_0019CE60;   /* sonde : FindFirst du jeu */
    if (xbox_va == 0x001F8ACDu) return sub_001F8ACD;   /* sonde : XFindFirstSaveGame */
    if (xbox_va == 0x00203458u) return sub_00203458;   /* sonde ABI : _stricmp */
    if (xbox_va == 0x002082A0u) return sub_002082A0;   /* sonde ABI : coeur de _stricmp */
    if (xbox_va == 0x001596F0u) return sub_001596F0;   /* sonde : objet VM nul */
    if (xbox_va == 0x0015A2F0u) return sub_0015A2F0;   /* sonde : son appelant */
    if (xbox_va == 0x0001A4E0u) return sub_0001A4E0;   /* aspect de la vue principale (16:9) */
    if (xbox_va == 0x001E46E0u) return sub_001E46E0;   /* sonde : recherche d'asset */
    if (xbox_va == 0x001E5C00u) return sub_001E5C00;   /* dossier mods : ouverture par nom */
    if (xbox_va == 0x00102130u) return sub_00102130;   /* sonde : constructeur de TAR */
    if (xbox_va == 0x00119470u) return sub_00119470;   /* sonde : registre EAGL */
    if (xbox_va == 0x0010A440u) return sub_0010A440;   /* sonde : GetAddr d'un module */
    if (xbox_va == 0x00101EB0u) return sub_00101EB0;   /* sonde : image attachee a une texture */
    if (xbox_va == 0x000978D0u) return sub_000978D0;   /* sonde : texture d'un combattant */
    if (xbox_va == 0x00112450u) return sub_00112450;   /* sonde : groupe de TAR par nom */
    if (xbox_va == 0x0002DB00u) return sub_0002DB00;   /* menu de debogage du titre */
    if (xbox_va == 0x00077B20u) return sub_00077B20;   /* boucle d'image du combat */
    if (xbox_va == 0x00021890u) return sub_00021890;   /* sonde : classe d'une piste d'animation */
    if (xbox_va == 0x000EDF90u) return sub_000EDF90;   /* sonde : cles d'animation (+0x18) */
    if (xbox_va == 0x000EE000u) return sub_000EE000;   /* sonde : cles d'animation (+0x30) */

    return (recomp_func_t)0;
}

static recomp_func_t recomp_lookup_manual_fwd(uint32_t xbox_va)
{
    return recomp_lookup_manual(xbox_va);
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    /* The same failure tens of thousands of times says nothing new after the
     * first few, and a log that grows to a million lines hides everything
     * else. Report each target a handful of times, then only a running count. */
    enum { SLOTS = 32 };
    static uint32_t seen_va[SLOTS];
    static unsigned seen_n[SLOTS];
    static unsigned n_seen;
    unsigned k;

    for (k = 0; k < n_seen && seen_va[k] != va; k++) { }
    if (k == n_seen && n_seen < SLOTS)
        seen_va[n_seen++] = va;
    if (k < SLOTS) {
        unsigned c = ++seen_n[k];
        if (c > 4) {
            if ((c & (c - 1)) == 0)          /* 8, 16, 32, ... */
                fprintf(stderr, "[ICALL] 0x%08X unresolved %u times\n", va, c);
            return;
        }
    }

    /* The call site. Generated code pushes the guest return address before
     * dispatching, so at this point it is on top of the guest stack -- which
     * names the instruction that made the call, the one fact the target alone
     * does not give. */
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu),"
            " call site returns to 0x%08X\n",
            va, (unsigned long long)g_icall_count, MEM32(g_esp));

    /* The guest esp is not always in step with the generated code's view of
     * it, so name the native callers too: the translated function that made
     * the call is right there in the host stack. */
    {
        void *frames[6];
        USHORT n = CaptureStackBackTrace(1, 6, frames, NULL);
        char buf[sizeof(SYMBOL_INFO) + 128];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        for (USHORT f = 0; f < n; f++) {
            DWORD64 disp = 0;
            memset(buf, 0, sizeof buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 127;
            if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)frames[f], &disp, sym))
                fprintf(stderr, "  host caller %u: %s+0x%llX\n", f, sym->Name,
                        (unsigned long long)disp);
        }
    }

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

