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
/* DirectSound copies to and from the APU's register window, which is left
 * unmapped so each access faults into the emulated APU. The fault handler
 * decodes plain `mov` only, and the host memcpy moves 32 bytes at a time with
 * AVX -- so such a copy crashed the title in the main menu once sound effects
 * started. Those copies go a dword (then a byte) at a time instead. */
static int apu_window_hit(uint32_t va, uint32_t n)
{
    return (uint64_t)va + n > 0xFE800000u && va < 0xFE880000u;
}

static __declspec(noinline) void mmio_safe_move(uint32_t dst, uint32_t src, uint32_t n)
{
    volatile uint32_t w;
    volatile uint8_t b;
    uint32_t i = 0;
    if (dst > src && dst < src + n) {          /* overlapping: backwards */
        while (n--) {
            b = MEM8(src + n);
            MEM8(dst + n) = b;
        }
        return;
    }
    for (; i + 4 <= n; i += 4) {
        w = MEM32(src + i);
        MEM32(dst + i) = w;
    }
    for (; i < n; i++) {
        b = MEM8(src + i);
        MEM8(dst + i) = b;
    }
}

void sub_002016B0(void)
{
    /* The eax/esp aliases are only defined for generated code
     * (RECOMP_GENERATED_CODE); hand-written code names the globals. */
    uint32_t dst = MEM32(g_esp + 4);
    uint32_t src = MEM32(g_esp + 8);
    uint32_t n   = MEM32(g_esp + 12);

    if (n && (apu_window_hit(dst, n) || apu_window_hit(src, n)))
        mmio_safe_move(dst, src, n);
    else if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), n);
    g_eax = dst;
    g_esp += 4; return;
}

/* ── Audio DSP command acknowledgement ──────────────────────────
 *
 * DirectSound's DSP upload (sub_0025F394, DSOUND section) ends with:
 *
 *     mov  [ebx+0x810], eax     ; hand the command block to the GP DSP
 *     cmp  [ebx+0x810], 0
 *     jne  $-2                   ; spin until the DSP has taken it
 *
 * On hardware the audio DSP clears that word when it consumes the command.
 * xbox_apu emulates the voice processor but stubs both DSP cores, so nothing
 * ever clears it and the main thread spins there forever -- which is the hang
 * the toolkit's own note about RECOMP_AC97_READY predicts ("DirectSound hands
 * the audio DSP a command block in RAM and spins until the DSP clears it").
 *
 * Acknowledging it from the host is the same claim the NV2A busy-bit thread
 * makes for the GPU: the work is not queued anywhere, so reporting it as
 * consumed is the honest answer, and it is what lets audio init finish.
 *
 * The address is not fixed -- the block is allocated -- so it is read out of
 * the guest object on the way into the function, which is the only place the
 * chain is known.
 *
 * ponytail: no sound is produced by this path. The DSP program is discarded,
 * not run. Voices that DirectSound drives through APU MMIO still reach the
 * emulated voice processor.
 */
#define DSP_ACK_SLOTS 4
static volatile LONG  g_dsp_ack_count;
static volatile ULONG g_dsp_ack_self[DSP_ACK_SLOTS];  /* guest `this` pointers */
static volatile LONG  g_dsp_ack_started;

/* Guest reads from a polling thread have to be bounds-checked. The chain below
 * is followed while the title is still building it, so an intermediate word can
 * be anything at all, and MEM32 on a wild value faults in the host rather than
 * in the title.
 *
 * The two ranges are the only ones this chain can legitimately land in: main
 * RAM, and the contiguous window that mirrors it. Masking the top nibble
 * instead of testing the ranges accepted addresses like 0x40000000, which is
 * mapped nowhere. */
static int dsp_readable(uint32_t va)
{
    if (va >= 0x00010000u && (uint64_t)va + 4 <= 0x04000000u)
        return 1;
    if (va >= 0x80000000u && (uint64_t)va + 4 <= 0x84000000u)
        return 1;
    return 0;
}

/* The value sub_0025F394 writes to hand the block over:
 *
 *     push 3 ; pop eax ; ... ; mov [ebx+0x810], eax ; cmp [ebx+0x810],0 ; jne $-2
 *
 * Clearing only that exact value is what keeps this from corrupting the title.
 * An earlier version cleared any non-zero word the chain resolved to, and the
 * chain does not stay pointing at the same object -- so it zeroed live data and
 * produced a repeatable crash in D3D a few seconds later. */
#define DSP_COMMAND_PENDING 3u

/* The other command on the same word. sub_0025F530 waits for the word to read
 * zero, then posts 2 and returns without waiting -- so the command is only
 * noticed by its next call, which spins forever if nothing clears it. Def Jam
 * posts it rarely: the first time is fine, the second (the end of a fight)
 * hung the game. It is cleared only at an address already proven to be the
 * command word during an upload, only while the chain still leads there, and
 * only when it holds exactly 2. */
#define DSP_COMMAND_UPDATE 2u

/* Resolve *(*(this + 8) + 0x10) + 0x810, the word sub_0025F394 spins on. */
static uint32_t dsp_command_word(uint32_t self)
{
    uint32_t p;

    if (!dsp_readable(self + 8))         return 0;
    p = MEM32(self + 8);
    if (!dsp_readable(p + 0x10))         return 0;
    p = MEM32(p + 0x10);
    if (!dsp_readable(p))                return 0;
    p = MEM32(p);
    if (!dsp_readable(p + 0x810))        return 0;
    return p + 0x810;
}

/* Non-zero only while a guest thread is inside sub_0025F394. Outside that
 * window there is no command to acknowledge and the object may be anything, so
 * the thread does not touch guest memory at all. */
static volatile LONG g_dsp_in_upload;

static DWORD WINAPI dsp_ack_thread(LPVOID unused)
{
    static uint32_t announced[DSP_ACK_SLOTS];
    ULONGLONG busy_until = 0;
    (void)unused;

    for (;;) {
        LONG n = g_dsp_ack_count, i;

        /* Outside an upload: the posted-and-forgotten command 2. */
        for (i = 0; i < n; i++) {
            uint32_t va = announced[i];
            if (va && dsp_command_word((uint32_t)g_dsp_ack_self[i]) == va
                    && MEM32(va) == DSP_COMMAND_UPDATE) {
                static int said;
                MEM32(va) = 0;
                busy_until = GetTickCount64() + 200;
                if (!said++) {
                    fprintf(stderr, "  [DSOUND] DSP update command at 0x%08X "
                            "acknowledged by the host\n", va);
                    fflush(stderr);
                }
            }
        }

        if (InterlockedCompareExchange(&g_dsp_in_upload, 0, 0) > 0) {
            for (i = 0; i < n; i++) {
                /* Re-resolved every tick rather than captured once. The chain
                 * is not complete when the function is entered -- the title
                 * fills it in as it builds the buffer -- and a single read at
                 * entry gave up on exactly the runs where the object was still
                 * being constructed, leaving the guest spinning forever. */
                uint32_t va = dsp_command_word((uint32_t)g_dsp_ack_self[i]);
                if (!va || MEM32(va) != DSP_COMMAND_PENDING)
                    continue;
                if (announced[i] != va) {
                    announced[i] = va;
                    fprintf(stderr, "  [DSOUND] DSP command word at 0x%08X "
                            "acknowledged by the host (no DSP emulation)\n", va);
                    fflush(stderr);
                }
                MEM32(va) = 0;
            }
            busy_until = GetTickCount64() + 200;
        }
        /* A waiter spins on another core, so answer fast while commands are
         * flowing; otherwise do not hold a core for nothing. */
        if (GetTickCount64() < busy_until)
            Sleep(0);
        else
            Sleep(1);
    }
}

static void dsp_ack_register(uint32_t self)
{
    LONG n, i;

    if (!self)
        return;
    n = g_dsp_ack_count;
    for (i = 0; i < n; i++)
        if ((uint32_t)g_dsp_ack_self[i] == self)
            return;                      /* already watched */
    if (n >= DSP_ACK_SLOTS)
        return;
    g_dsp_ack_self[n] = self;
    InterlockedIncrement(&g_dsp_ack_count);

    if (InterlockedCompareExchange(&g_dsp_ack_started, 1, 0) == 0)
        CreateThread(NULL, 0, dsp_ack_thread, NULL, 0, NULL);
}

/* DSOUND DSP upload at 0x0025F394. The generated body still runs -- it does
 * the relocation and the copies -- but the object it will spin on is handed to
 * the acknowledging thread first, so the spin ends. `this` arrives in ecx
 * (thiscall), which is all the thread needs: it walks the same chain the
 * function's own prologue walks, and keeps walking it until it resolves. */
extern void sub_0025F394_gen(void);

void sub_0025F394(void)
{
    dsp_ack_register(g_ecx);
    InterlockedIncrement(&g_dsp_in_upload);
    sub_0025F394_gen();
    InterlockedDecrement(&g_dsp_in_upload);
}

/* IDirectSoundBuffer::GetCurrentPosition at 0x0026415B.
 *
 * It reads the buffer's voice object through [this+0x80] and then follows it:
 *
 *     mov ecx, [esi+0x80] ; mov ecx, [ecx+0x14]
 *
 * With audio initialisation now completing every run, the title reaches this
 * with [this+0x80] holding something that is not a guest pointer at all
 * (0xC2030000 in every observed case), and the runtime faults following it.
 * The value comes from CreateSoundBuffer, which takes it from the device
 * object, so the real defect is upstream in DirectSound's own construction --
 * this does not fix that.
 *
 * What it does is refuse to follow a pointer that cannot be one, report a
 * position of zero, and return success, which is what a buffer that has not
 * started playing would report anyway. The title carries on instead of the
 * process dying, and the bad pointer is logged once so the upstream bug stays
 * visible rather than being quietly papered over.
 *
 * Remove this the moment CreateSoundBuffer is understood.
 */
extern void sub_0026415B_gen(void);

void sub_0026415B(void)
{
    uint32_t self = g_ecx;
    uint32_t voice = dsp_readable(self + 0x80) ? MEM32(self + 0x80) : 0;

    /* Experiment, opt-in through RECOMP_DS_FAKE_CURSOR.
     *
     * The APU's DSP cores are stubbed, so nothing consumes a sound buffer and
     * its play cursor never moves. A title that waits for playback to progress
     * would wait forever, and this says whether Def Jam is one: it reports a
     * cursor advancing at CD rate instead of asking the buffer. If the title
     * gets further with this on, the answer is to make the APU actually play;
     * if it does not, audio progress was never what it was waiting for.
     *
     * Not a fix, and deliberately not on by default: a title that trusts this
     * cursor to know how much data to write would be misled by it. */
    if (voice && dsp_readable(voice + 0xC0) && getenv("RECOMP_DS_FAKE_CURSOR")) {
        uint32_t size = MEM32(voice + 0xC0);
        if (size >= 0x400u && size <= 0x1000000u) {
            uint32_t play_out  = MEM32(g_esp + 4);
            uint32_t write_out = MEM32(g_esp + 8);
            /* 176 bytes per millisecond is 44.1 kHz, 16-bit, stereo. */
            uint32_t pos = (uint32_t)((uint64_t)GetTickCount64() * 176u % size);
            if (dsp_readable(play_out))  MEM32(play_out)  = pos;
            if (dsp_readable(write_out)) MEM32(write_out) = (pos + 2048u) % size;
            g_eax = 0;
            g_esp += 4 + 8;
            return;
        }
    }

    if (!voice || !dsp_readable(voice + 0x14)) {
        uint32_t play_out  = MEM32(g_esp + 4);
        uint32_t write_out = MEM32(g_esp + 8);
        static uint32_t seen[4];
        static unsigned n_seen;
        unsigned i;

        for (i = 0; i < n_seen && seen[i] != voice; i++) { }
        if (i == n_seen && n_seen < 4) {
            seen[n_seen++] = voice;
            fprintf(stderr, "  [DSOUND] GetCurrentPosition: buffer 0x%08X has "
                    "voice 0x%08X, which is not a guest pointer -- reporting "
                    "position 0\n", self, voice);
            fflush(stderr);
        }
        if (dsp_readable(play_out))  MEM32(play_out)  = 0;
        if (dsp_readable(write_out)) MEM32(write_out) = 0;
        g_eax = 0;                  /* DS_OK */
        g_esp += 4 + 8;             /* ret 8 */
        return;
    }

    /* What the title is told about playback progress, once a second.
     *
     * It decides where it may write from this. The APU prints the voice's real
     * read offset at the same cadence, so the two lines together say whether
     * the title is writing into the stretch we are reading -- which is what a
     * click in the middle of a buffer sounds like. */
    {
        uint32_t play_out  = MEM32(g_esp + 4);
        uint32_t write_out = MEM32(g_esp + 8);

        sub_0026415B_gen();

        if (getenv("RECOMP_APU_TRACE")) {
            /* One line per buffer per second: the title runs several, and
             * reporting whichever happened to ask last says nothing. */
            static struct { uint32_t self; uint32_t next_tick; } seen[8];
            uint32_t now = (uint32_t)GetTickCount64();
            unsigned i;

            for (i = 0; i < 8 && seen[i].self && seen[i].self != self; i++) { }
            if (i < 8) {
                seen[i].self = self;
                if (now >= seen[i].next_tick) {
                    /* Why the position comes back as zero.
                     *
                     * FUN_0026302e only reads the voice's current offset when
                     * the buffer's flag bit 0 is set and the voice is no
                     * longer marked NEW_VOICE. The voice register array base
                     * lives at 0x0027CCB0, each voice is 0x80 bytes, PAR_STATE
                     * is at +0x54 and PAR_OFFSET (CBO) at +0x58. */
                    uint32_t regs = dsp_readable(0x0027CCB0)
                                        ? MEM32(0x0027CCB0) : 0;
                    uint32_t handle = dsp_readable(self + 0xC)
                                        ? (MEM32(self + 0xC) & 0xFFFF) : 0;
                    uint32_t flags = dsp_readable(self + 0x10)
                                        ? ((MEM32(self + 0x10) >> 16) & 0xFF) : 0;
                    uint32_t state = 0, cbo = 0;

                    if (regs && dsp_readable(regs + handle * 0x80 + 0x58)) {
                        state = MEM32(regs + handle * 0x80 + 0x54);
                        cbo = MEM32(regs + handle * 0x80 + 0x58) & 0x00FFFFFF;
                    }

                    seen[i].next_tick = now + 1000;
                    fprintf(stderr, "  [DSOUND] buffer 0x%08X voice 0x%08X:"
                            " play cursor %u, write cursor %u"
                            " | handle %u flags 0x%02X regs 0x%08X"
                            " state 0x%08X (new_voice %d) cbo %u\n",
                            self, voice,
                            dsp_readable(play_out) ? MEM32(play_out) : 0u,
                            dsp_readable(write_out) ? MEM32(write_out) : 0u,
                            handle, flags, regs, state,
                            (state & (1u << 20)) ? 1 : 0, cbo);
                    fflush(stderr);
                }
            }
        }
    }
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

    unlock_all_tick();

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
static struct { uint32_t obj, piste; char nom[32]; } anim_noms[256];
static int anim_noms_n;

static void anim_retenir_nom(uint32_t obj, uint32_t nom)
{
    if (anim_cles_reste < 0) {
        const char *s = getenv("RECOMP_ANIM_KEYS");
        anim_cles_reste = s ? atoi(s) : 0;
    }
    if (anim_cles_reste <= 0 || !obj || !dsp_readable(obj + 0xC))
        return;
    int i = anim_noms_n++ & 255;
    anim_noms[i].obj = obj;
    anim_noms[i].piste = MEM32(obj + 0xC);
    snprintf(anim_noms[i].nom, sizeof anim_noms[i].nom, "%s", eagl_str(nom));
}

static const char *anim_nom_de(uint32_t obj, uint32_t piste)
{
    for (int k = 0; k < 256; k++) {
        int i = (anim_noms_n - 1 - k) & 255;
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

extern void sub_000EDF90_gen(void);
extern void sub_000EE000_gen(void);

void sub_000EDF90(void)                      /* composite d'animation, methode +0x18 */
{
    uint32_t self = g_ecx, f = MEM32(g_esp + 4), out = MEM32(g_esp + 8), p3 = MEM32(g_esp + 12);
    sub_000EDF90_gen();
    anim_cles(0x18, self, f, out, p3);
}

void sub_000EE000(void)                      /* composite d'animation, methode +0x30 */
{
    uint32_t self = g_ecx, f = MEM32(g_esp + 4), out = MEM32(g_esp + 8), p3 = MEM32(g_esp + 12);
    sub_000EE000_gen();
    anim_cles(0x30, self, f, out, p3);
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
    if (xbox_va == 0x002016B0) return sub_002016B0;   /* memmove */
    if (xbox_va == 0x0025F394) return sub_0025F394;   /* DSOUND DSP upload */
    if (xbox_va == 0x0026415B) return sub_0026415B;   /* DSOUND GetCurrentPosition */
    if (xbox_va == 0x002091E3) return sub_002091E3;   /* CRT thread-exit callback */
    if (xbox_va == 0x00077040) return sub_00077040;   /* screen-state probe */
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

