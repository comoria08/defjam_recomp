/*
 * lanceur.c - the launcher: the settings window before the game (point 141).
 *
 * Until now the settings lived in .env, read by jouer.bat: a text file to
 * edit by hand, and a batch file between the player and the game. The
 * launcher replaces both, inside the executable:
 *
 *   - the game folder is found wherever the exe was started from (the
 *     current folder, the exe's folder, or two levels up for build\Release);
 *   - the log goes to run_stderr.txt when nobody redirected it, the previous
 *     one kept as run_stderr_precedent.txt (the Log button opens it);
 *   - defjam.ini holds the settings, KEY=VALUE as .env did, and is created
 *     from .env the first time so nothing set there is lost;
 *   - a window offers what a player changes; everything else in the file
 *     (diagnostics, traces) is kept untouched and still applied;
 *   - on Play, each RECOMP_* setting becomes an environment variable, unless
 *     the environment already has it: a variable set by hand always wins.
 *
 * Plain Win32, no dependency: the toolkit is C and this stays C. French when
 * Windows is, English otherwise -- the port is meant to be shared.
 * RECOMP_LANCEUR=0 skips everything (the test scripts use it), LANCEUR=0 in
 * the file hides the window, Shift held at start shows it again.
 */
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lanceur.h"
#include "nv2a_dlss.h"                  /* nvdlss_built: DLSS offered only if built in */

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define INI_PATH  "defjam.ini"
#define ENV_PATH  ".env"
#define LOG_PATH  "run_stderr.txt"
#define LOG_PREV  "run_stderr_precedent.txt"

/* ── The settings file ─────────────────────────────────────────────── */

#define INI_MAX 256
typedef struct {
    char raw[640];                      /* the line as read, for what is not KEY=VALUE */
    char key[64];
    char value[576];
    int  is_kv;
} IniLine;

static IniLine s_ini[INI_MAX];
static int     s_nini;

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

static int ini_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    char line[640];

    if (!f)
        return 0;
    s_nini = 0;
    while (s_nini < INI_MAX && fgets(line, sizeof line, f)) {
        IniLine *l = &s_ini[s_nini++];
        char *eq, *p = line;
        memset(l, 0, sizeof *l);
        trim(line);
        snprintf(l->raw, sizeof l->raw, "%s", line);
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || !*p || !(eq = strchr(p, '=')) || eq == p)
            continue;
        *eq = 0;
        snprintf(l->key, sizeof l->key, "%s", p);
        trim(l->key);
        snprintf(l->value, sizeof l->value, "%s", eq + 1);
        l->is_kv = 1;
    }
    fclose(f);
    return 1;
}

static const char *ini_get(const char *key, const char *def)
{
    int i;
    for (i = 0; i < s_nini; i++)
        if (s_ini[i].is_kv && !strcmp(s_ini[i].key, key))
            return s_ini[i].value;
    return def;
}

static void ini_set(const char *key, const char *value)
{
    int i;
    for (i = 0; i < s_nini; i++)
        if (s_ini[i].is_kv && !strcmp(s_ini[i].key, key)) {
            snprintf(s_ini[i].value, sizeof s_ini[i].value, "%s", value);
            return;
        }
    if (s_nini < INI_MAX) {
        IniLine *l = &s_ini[s_nini++];
        memset(l, 0, sizeof *l);
        snprintf(l->key, sizeof l->key, "%s", key);
        snprintf(l->value, sizeof l->value, "%s", value);
        l->is_kv = 1;
    }
}

static int ini_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f)
        return 0;
    for (i = 0; i < s_nini; i++) {
        if (s_ini[i].is_kv)
            fprintf(f, "%s=%s\r\n", s_ini[i].key, s_ini[i].value);
        else
            fprintf(f, "%s\r\n", s_ini[i].raw);
    }
    fclose(f);
    return 1;
}

/* A new file: a header, then whatever .env held, so an existing setup moves
 * over whole -- diagnostics and comments included. */
static void ini_create(void)
{
    s_nini = 0;
    if (ini_load(ENV_PATH)) {
        /* .env's own header talks about jouer.bat; keep its lines but say where
         * they came from. */
        if (s_nini < INI_MAX) {
            memmove(&s_ini[1], &s_ini[0], (size_t)s_nini * sizeof s_ini[0]);
            s_nini++;
            memset(&s_ini[0], 0, sizeof s_ini[0]);
            snprintf(s_ini[0].raw, sizeof s_ini[0].raw,
                     "# defjam.ini - reglages du lanceur (repris de .env le premier lancement)");
        }
    } else {
        memset(&s_ini[0], 0, sizeof s_ini[0]);
        snprintf(s_ini[0].raw, sizeof s_ini[0].raw,
                 "# defjam.ini - reglages du lanceur. Une ligne CLE=VALEUR, # en tete = ignore.");
        s_nini = 1;
    }
}

/* ── Applying the settings ─────────────────────────────────────────── */

/* Variables whose "0" means something: they are on by default, so 0 turns
 * them off. Every other one is tested for presence alone -- exporting
 * RECOMP_UNLOCK_ALL=0 would unlock everything -- so its 0 is not exported. */
static int zero_means_off(const char *key)
{
    static const char *const keys[] = {
        "RECOMP_MODS", "RECOMP_TEX_FILTER", "RECOMP_TEX_MIPS", "RECOMP_TEX_ANISO",
        "RECOMP_COMBINERS", "RECOMP_WIDESCREEN_CINE", "RECOMP_VP_GPU",
        "RECOMP_WIDESCREEN", "RECOMP_FULLSCREEN", NULL
    };
    int i;
    for (i = 0; keys[i]; i++)
        if (!strcmp(keys[i], key))
            return 1;
    return 0;
}

static void export_env(void)
{
    /* Without these the game does not get past its initialisation. */
    static const char *const needed[] = {
        "RECOMP_AC97_READY", "RECOMP_VBLANK", "RECOMP_PTIMER", "RECOMP_PB_EXEC",
        "RECOMP_FB_WINDOW", "RECOMP_D3D11", NULL
    };
    const int pack = strcmp(ini_get("PACK_HD", "1"), "0") != 0;
    int i;

    for (i = 0; i < s_nini; i++) {
        const IniLine *l = &s_ini[i];
        if (!l->is_kv || strncmp(l->key, "RECOMP_", 7) != 0 || !l->value[0])
            continue;
        if (getenv(l->key))
            continue;                               /* set by hand: it wins */
        if (!strcmp(l->value, "0") && !zero_means_off(l->key))
            continue;
        if (!strcmp(l->key, "RECOMP_TEX_PACK") && !pack)
            continue;
        _putenv_s(l->key, l->value);
    }
    for (i = 0; needed[i]; i++)
        if (!getenv(needed[i]))
            _putenv_s(needed[i], "1");
}

/* ── The game folder and the log ───────────────────────────────────── */

static int has_game(const wchar_t *dir)
{
    wchar_t p[MAX_PATH + 64];
    _snwprintf(p, MAX_PATH + 63, L"%s\\game_files\\default.xbe", dir);
    p[MAX_PATH + 63] = 0;
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static void find_game_root(void)
{
    wchar_t dir[MAX_PATH];
    int i;

    if (GetCurrentDirectoryW(MAX_PATH, dir) && has_game(dir))
        return;
    if (!GetModuleFileNameW(NULL, dir, MAX_PATH))
        return;
    for (i = 0; i < 4; i++) {
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (!slash)
            return;
        *slash = 0;
        if (has_game(dir)) {
            SetCurrentDirectoryW(dir);
            return;
        }
    }
}

/* Started by double-click, a windows-subsystem program has no stderr, and the
 * whole of the runtime's diagnosis goes there. */
static void open_log(void)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN)
        return;                                     /* jouer.bat, a script, a terminal */
    MoveFileExA(LOG_PATH, LOG_PREV, MOVEFILE_REPLACE_EXISTING);
    freopen(LOG_PATH, "w", stderr);
}

/* ── The window ────────────────────────────────────────────────────── */

enum {
    ID_SCALE = 100, ID_FORMAT, ID_CINE, ID_FULL, ID_FILTER, ID_MIPS, ID_COMB,
    ID_PACK, ID_PACKDIR, ID_PACKBROWSE, ID_MODS, ID_UNLOCK, ID_PAD, ID_HIDE,
    ID_OPENMODS, ID_OPENLOG, ID_UI43, ID_DLSS
};

typedef struct { const wchar_t *fr, *en; } Text;
static int s_fr;
#define T(t) (s_fr ? (t).fr : (t).en)

static const Text TX_TITLE    = { L"Def Jam: Fight for NY", L"Def Jam: Fight for NY" };
static const Text TX_SUB      = { L"Recompilation statique \u2014 r\u00e9glages",
                                  L"Static recompilation \u2014 settings" };
static const Text TX_PICTURE  = { L"Image", L"Picture" };
static const Text TX_SCALE    = { L"R\u00e9solution interne", L"Internal resolution" };
static const Text TX_FORMAT   = { L"Format", L"Aspect ratio" };
static const Text TX_43       = { L"4:3 (d'origine)", L"4:3 (original)" };
static const Text TX_169      = { L"16:9 (\u00e9cran large)", L"16:9 (widescreen)" };
static const Text TX_CINE     = { L"Cin\u00e9matiques sur toute la hauteur (16:9)",
                                  L"Full-height cutscenes (16:9)" };
static const Text TX_DLSS     = { L"DLSS (NVIDIA RTX)", L"DLSS (NVIDIA RTX)" };
static const Text TX_DLSS_OFF = { L"D\u00e9sactiv\u00e9", L"Off" };
static const Text TX_DLSS_AA  = { L"DLAA (anticr\u00e9nelage)", L"DLAA (anti-aliasing)" };
static const Text TX_DLSS_NO  = { L"Non disponible dans cette version", L"Not available in this build" };
static const Text TX_UI43     = { L"Interface et menus en 4:3, non \u00e9tir\u00e9s",
                                  L"Interface and menus in 4:3, not stretched" };
static const Text TX_FULL     = { L"Plein \u00e9cran au d\u00e9marrage (F11 bascule)",
                                  L"Start in full screen (F11 toggles)" };
static const Text TX_FILTER   = { L"Filtrage bilin\u00e9aire des textures", L"Bilinear texture filtering" };
static const Text TX_MIPS     = { L"Mipmaps et filtrage anisotrope 16x", L"Mipmaps and 16x anisotropic filtering" };
static const Text TX_COMB     = { L"Effets de rendu (reflets, \u00e9clairages)",
                                  L"Rendering effects (highlights, lighting)" };
static const Text TX_PACK     = { L"Pack de textures HD :", L"HD texture pack:" };
static const Text TX_GAME     = { L"Jeu", L"Game" };
static const Text TX_MODS     = { L"Mods (dossier game_files\\mods)", L"Mods (game_files\\mods folder)" };
static const Text TX_UNLOCK   = { L"Tout d\u00e9bloquer (personnages, lieux, 10 000 points)",
                                  L"Unlock everything (fighters, venues, 10,000 points)" };
static const Text TX_PAD      = { L"Disposition d'origine de la manette Xbox",
                                  L"Original Xbox controller layout" };
static const Text TX_HIDE     = { L"Ne plus afficher ce lanceur (Maj au d\u00e9marrage pour le revoir)",
                                  L"Don't show this launcher again (hold Shift at start to see it)" };
static const Text TX_PLAY     = { L"Jouer", L"Play" };
static const Text TX_QUIT     = { L"Quitter", L"Quit" };
static const Text TX_OPENMODS = { L"Dossier des mods", L"Mods folder" };
static const Text TX_OPENLOG  = { L"Journal", L"Log" };
static const Text TX_BROWSE   = { L"Choisir le dossier du pack de textures",
                                  L"Choose the texture pack folder" };
static const Text TX_NOLOG    = { L"Pas encore de journal : il est \u00e9crit pendant une partie.",
                                  L"No log yet: it is written while the game runs." };

static HWND  s_wnd;
static HFONT s_font, s_font_title;
static UINT  s_dpi = 96;
static int   s_result = -1;                     /* -1 open, 0 quit, 1 play */

static int px(int v) { return MulDiv(v, (int)s_dpi, 96); }

static HWND ctl(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             px(x), px(y), px(w), px(h), s_wnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}

static HWND item(int id) { return GetDlgItem(s_wnd, id); }
static int  checked(int id) { return SendMessageW(item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void check(int id, int on) { SendMessageW(item(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
static int  on(const char *key, const char *def) { return strcmp(ini_get(key, def), "0") != 0; }

static void update_enabled(void)
{
    const int wide = SendMessageW(item(ID_FORMAT), CB_GETCURSEL, 0, 0) == 1;
    EnableWindow(item(ID_CINE), wide);
    EnableWindow(item(ID_UI43), wide);
    EnableWindow(item(ID_PACKDIR), checked(ID_PACK));
    EnableWindow(item(ID_PACKBROWSE), checked(ID_PACK));
}

static void load_controls(void)
{
    int scale = atoi(ini_get("RECOMP_D3D11_SCALE", "3"));
    wchar_t w[576];

    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    SendMessageW(item(ID_SCALE), CB_SETCURSEL, (WPARAM)(scale - 1), 0);
    SendMessageW(item(ID_FORMAT), CB_SETCURSEL, on("RECOMP_WIDESCREEN", "1") ? 1 : 0, 0);
    check(ID_CINE, on("RECOMP_WIDESCREEN_CINE", "1"));
    check(ID_UI43, on("RECOMP_UI_43", "0"));
    SendMessageW(item(ID_DLSS), CB_SETCURSEL, (nvdlss_built() && on("RECOMP_DLSS", "0")) ? 1 : 0, 0);
    check(ID_FULL, on("RECOMP_FULLSCREEN", "0"));
    check(ID_FILTER, on("RECOMP_TEX_FILTER", "1"));
    check(ID_MIPS, on("RECOMP_TEX_MIPS", "1"));
    check(ID_COMB, on("RECOMP_COMBINERS", "1"));
    check(ID_PACK, on("PACK_HD", ini_get("RECOMP_TEX_PACK", "")[0] ? "1" : "0"));
    MultiByteToWideChar(CP_ACP, 0, ini_get("RECOMP_TEX_PACK", ""), -1, w, 576);
    SetWindowTextW(item(ID_PACKDIR), w);
    check(ID_MODS, on("RECOMP_MODS", "1"));
    check(ID_UNLOCK, on("RECOMP_UNLOCK_ALL", "0"));
    check(ID_PAD, on("RECOMP_PAD_XBOX_LAYOUT", "0"));
    check(ID_HIDE, !on("LANCEUR", "1"));
    update_enabled();
}

static void save_controls(void)
{
    char v[576];
    wchar_t w[576];

    snprintf(v, sizeof v, "%d", (int)SendMessageW(item(ID_SCALE), CB_GETCURSEL, 0, 0) + 1);
    ini_set("RECOMP_D3D11_SCALE", v);
    ini_set("RECOMP_WIDESCREEN", SendMessageW(item(ID_FORMAT), CB_GETCURSEL, 0, 0) == 1 ? "1" : "0");
    ini_set("RECOMP_WIDESCREEN_CINE", checked(ID_CINE) ? "1" : "0");
    ini_set("RECOMP_UI_43", checked(ID_UI43) ? "1" : "0");
    if (nvdlss_built())                         /* left as it was otherwise */
        ini_set("RECOMP_DLSS", SendMessageW(item(ID_DLSS), CB_GETCURSEL, 0, 0) == 1 ? "1" : "0");
    ini_set("RECOMP_FULLSCREEN", checked(ID_FULL) ? "1" : "0");
    ini_set("RECOMP_TEX_FILTER", checked(ID_FILTER) ? "1" : "0");
    ini_set("RECOMP_TEX_MIPS", checked(ID_MIPS) ? "1" : "0");
    ini_set("RECOMP_TEX_ANISO", checked(ID_MIPS) ? "1" : "0");
    ini_set("RECOMP_COMBINERS", checked(ID_COMB) ? "1" : "0");
    ini_set("PACK_HD", checked(ID_PACK) ? "1" : "0");
    GetWindowTextW(item(ID_PACKDIR), w, 576);
    WideCharToMultiByte(CP_ACP, 0, w, -1, v, sizeof v, NULL, NULL);
    ini_set("RECOMP_TEX_PACK", v);
    ini_set("RECOMP_MODS", checked(ID_MODS) ? "1" : "0");
    ini_set("RECOMP_UNLOCK_ALL", checked(ID_UNLOCK) ? "1" : "0");
    ini_set("RECOMP_PAD_XBOX_LAYOUT", checked(ID_PAD) ? "1" : "0");
    ini_set("LANCEUR", checked(ID_HIDE) ? "0" : "1");
}

static void browse_pack(void)
{
    BROWSEINFOW bi;
    wchar_t name[MAX_PATH];
    PIDLIST_ABSOLUTE pidl;

    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = s_wnd;
    bi.pszDisplayName = name;
    bi.lpszTitle = T(TX_BROWSE);
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            SetWindowTextW(item(ID_PACKDIR), path);
            check(ID_PACK, 1);
            update_enabled();
        }
        CoTaskMemFree(pidl);
    }
}

static void build(void)
{
    static const wchar_t *const scales[] = {
        L"640 x 480 (x1)", L"1280 x 960 (x2)", L"1920 x 1440 (x3)",
        L"2560 x 1920 (x4)", L"3200 x 2400 (x5)", L"3840 x 2880 (x6)"
    };
    HWND c;
    int i, y;

    c = ctl(L"STATIC", T(TX_TITLE), SS_LEFT, 16, 12, 440, 28, -1);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font_title, TRUE);
    ctl(L"STATIC", T(TX_SUB), SS_LEFT, 16, 42, 440, 18, -1);

    ctl(L"BUTTON", T(TX_PICTURE), BS_GROUPBOX, 12, 68, 452, 294, -1);
    ctl(L"STATIC", T(TX_SCALE), SS_LEFT, 26, 92, 160, 18, -1);
    c = ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 200, 88, 250, 200, ID_SCALE);
    for (i = 0; i < 6; i++)
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)scales[i]);
    ctl(L"STATIC", T(TX_FORMAT), SS_LEFT, 26, 122, 160, 18, -1);
    c = ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 200, 118, 250, 100, ID_FORMAT);
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)T(TX_43));
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)T(TX_169));
    /* DLSS (point 155): DLAA for now. Greyed when the exe was built without
     * the Streamline SDK; whether the GPU can is only known in game (log). */
    ctl(L"STATIC", T(TX_DLSS), SS_LEFT, 26, 152, 160, 18, -1);
    c = ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 200, 148, 250, 100, ID_DLSS);
    if (nvdlss_built()) {
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)T(TX_DLSS_OFF));
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)T(TX_DLSS_AA));
    } else {
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)T(TX_DLSS_NO));
        EnableWindow(c, FALSE);
    }
    y = 180;
    ctl(L"BUTTON", T(TX_CINE),   BS_AUTOCHECKBOX | WS_TABSTOP, 26, y, 424, 20, ID_CINE);   y += 24;
    ctl(L"BUTTON", T(TX_UI43),   BS_AUTOCHECKBOX | WS_TABSTOP, 26, y, 424, 20, ID_UI43);   y += 24;
    ctl(L"BUTTON", T(TX_FULL),  BS_AUTOCHECKBOX | WS_TABSTOP, 26, y, 424, 20, ID_FULL);   y += 24;
    ctl(L"BUTTON", T(TX_FILTER), BS_AUTOCHECKBOX | WS_TABSTOP, 26, y, 424, 20, ID_FILTER); y += 24;
    ctl(L"BUTTON", T(TX_MIPS),   BS_AUTOCHECKBOX | WS_TABSTOP, 26, y, 424, 20, ID_MIPS);   y += 24;
    ctl(L"BUTTON", T(TX_COMB),   BS_AUTOCHECKBOX | WS_TABSTOP, 26, y, 424, 20, ID_COMB);   y += 28;
    ctl(L"BUTTON", T(TX_PACK),   BS_AUTOCHECKBOX | WS_TABSTOP, 26, y, 150, 20, ID_PACK);
    ctl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 180, y - 1, 236, 22, ID_PACKDIR);
    ctl(L"BUTTON", L"...", BS_PUSHBUTTON | WS_TABSTOP, 420, y - 1, 30, 22, ID_PACKBROWSE);

    ctl(L"BUTTON", T(TX_GAME), BS_GROUPBOX, 12, 370, 452, 100, -1);
    y = 392;
    ctl(L"BUTTON", T(TX_MODS),   BS_AUTOCHECKBOX | WS_TABSTOP, 26, y, 424, 20, ID_MODS);   y += 24;
    ctl(L"BUTTON", T(TX_UNLOCK), BS_AUTOCHECKBOX | WS_TABSTOP, 26, y, 424, 20, ID_UNLOCK); y += 24;
    ctl(L"BUTTON", T(TX_PAD),    BS_AUTOCHECKBOX | WS_TABSTOP, 26, y, 424, 20, ID_PAD);

    ctl(L"BUTTON", T(TX_HIDE), BS_AUTOCHECKBOX | WS_TABSTOP, 16, 480, 448, 20, ID_HIDE);

    ctl(L"BUTTON", T(TX_OPENMODS), BS_PUSHBUTTON | WS_TABSTOP, 12, 512, 120, 30, ID_OPENMODS);
    ctl(L"BUTTON", T(TX_OPENLOG),  BS_PUSHBUTTON | WS_TABSTOP, 138, 512, 80, 30, ID_OPENLOG);
    ctl(L"BUTTON", T(TX_QUIT), BS_PUSHBUTTON | WS_TABSTOP, 264, 512, 96, 30, IDCANCEL);
    c = ctl(L"BUTTON", T(TX_PLAY), BS_DEFPUSHBUTTON | WS_TABSTOP, 368, 512, 96, 30, IDOK);
    SetFocus(c);
}

static void open_path(const wchar_t *path)
{
    ShellExecuteW(s_wnd, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            save_controls();
            s_result = 1;
            DestroyWindow(h);
            return 0;
        case IDCANCEL:
            s_result = 0;
            DestroyWindow(h);
            return 0;
        case ID_FORMAT:
            if (HIWORD(wp) == CBN_SELCHANGE)
                update_enabled();
            return 0;
        case ID_PACK:
            update_enabled();
            return 0;
        case ID_PACKBROWSE:
            browse_pack();
            return 0;
        case ID_OPENMODS:
            CreateDirectoryW(L"game_files\\mods", NULL);
            open_path(L"game_files\\mods");
            return 0;
        case ID_OPENLOG:
            if (GetFileAttributesW(L"" LOG_PREV) != INVALID_FILE_ATTRIBUTES)
                open_path(L"" LOG_PREV);
            else if (GetFileAttributesW(L"" LOG_PATH) != INVALID_FILE_ATTRIBUTES)
                open_path(L"" LOG_PATH);
            else
                MessageBoxW(h, T(TX_NOLOG), T(TX_TITLE), MB_ICONINFORMATION);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CLOSE:
        s_result = 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static int show_window(void)
{
    WNDCLASSW wc;
    RECT r;
    MSG m;
    int cw, ch;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    s_dpi = GetDpiForSystem();
    s_fr = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);      /* the folder picker */

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = L"DefJamLanceur";
    RegisterClassW(&wc);

    s_font = CreateFontW(-MulDiv(9, (int)s_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_font_title = CreateFontW(-MulDiv(15, (int)s_dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                               0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    cw = px(476); ch = px(554);
    r.left = 0; r.top = 0; r.right = cw; r.bottom = ch;
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, s_dpi);
    s_wnd = CreateWindowExW(0, wc.lpszClassName, L"Def Jam: Fight for NY",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                            NULL, NULL, wc.hInstance, NULL);
    if (!s_wnd)
        return 1;                                        /* no window: play with the file */
    {
        /* Centred on the work area of the primary monitor. */
        RECT wa, wr;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        GetWindowRect(s_wnd, &wr);
        SetWindowPos(s_wnd, NULL,
                     wa.left + ((wa.right - wa.left) - (wr.right - wr.left)) / 2,
                     wa.top + ((wa.bottom - wa.top) - (wr.bottom - wr.top)) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    build();
    load_controls();
    ShowWindow(s_wnd, SW_SHOW);
    SetForegroundWindow(s_wnd);

    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (s_wnd && IsWindow(s_wnd) && IsDialogMessageW(s_wnd, &m))
            continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    s_wnd = NULL;
    DeleteObject(s_font);
    DeleteObject(s_font_title);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return s_result == 1;
}

/* ── Entry ─────────────────────────────────────────────────────────── */

int lanceur_run(void)
{
    const char *e = getenv("RECOMP_LANCEUR");
    const int forced = GetAsyncKeyState(VK_SHIFT) & 0x8000
                    || strstr(GetCommandLineA(), "--lanceur") != NULL;

    if (e && *e == '0')
        return 1;                                        /* scripts: environment only */
    find_game_root();
    open_log();
    if (!ini_load(INI_PATH)) {
        ini_create();
        ini_save(INI_PATH);
    }
    if (on("LANCEUR", "1") || forced) {
        if (!show_window())
            return 0;
        ini_save(INI_PATH);
    }
    export_env();
    fprintf(stderr, "[LANCEUR] %s lu, dossier du jeu pret, reglages appliques\n", INI_PATH);
    return 1;
}
