/*
 * djffny.cpp - DJFFNY.exe, le lanceur tout-en-un de Def Jam: Fight for NY recompile.
 *
 * Un seul exe, a donner au joueur : il installe le jeu depuis son image disque (il telecharge la
 * release choisie, Clang tout compris ou MSVC + DLSS, puis lance installeur\installer.ps1), regle
 * defjam.ini, installe un mod (.mod, un zip), lance le jeu et signale les mises a jour.
 *
 * L'interface est une page HTML (lanceur/page.html, d'apres la maquette de Claude Design),
 * integree a l'exe et affichee par WebView2 (le moteur d'Edge, present sur Windows 10 et 11).
 * La page et l'exe se parlent par messages JSON { type, ... } : la page demande, l'exe repond.
 * Rien du jeu n'est dans cet exe : il peut etre distribue.
 *
 * Etat : %APPDATA%\DJFFNY\djffny.ini (dossier, paquet, version, image disque) ; le mod actif est
 * decrit dans <jeu>\game_files\mod_actif\ (liste de ses fichiers, sauvegarde de ce qu'il a remplace).
 * DJFFNY_PAQUET=<zip> remplace le telechargement par un paquet local (essais avant publication).
 * DJFFNY_OUTILS=1 ouvre les outils de developpement de la page.
 *
 * Langues (1.2.0) : francais, anglais, espagnol, allemand, portugais, d'apres la langue de Windows
 * (ou le choix fait dans Reglages, "langue" de djffny.ini). La page traduit tout ; l'exe ne lui
 * envoie que des cles (toasts, erreurs) et des nombres (tailles en octets).
 *
 * Mises a jour (1.2.0) : chaque release porte versions.json (paquet/fabriquer_release.ps1) :
 * - "lanceur" plus recent que cet exe : il telecharge le nouveau DJFFNY.exe, se remplace et
 *   redemarre (--apres-maj) ;
 * - "jeu" different de l'empreinte du manifest.json installe (ou "outils" pour la version Clang) :
 *   mise a jour incrementale, seuls les fichiers changes sont remplaces, puis installer.ps1
 *   -MiseAJour ne refait que ce qu'il faut (ni extraction du disque, ni traduction si inutile).
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WebView2.h"

using Microsoft::WRL::ComPtr;
using std::string;
using std::wstring;

#define VERSION_LANCEUR "1.2.0"
#define VERSION_LANCEUR_W L"1.2.0"
#define WM_RELANCER     (WM_APP + 3)
#define DEPOT           L"comoria08/defjam_recomp"
#define HOTE            L"https://djffny.local/"
#define WM_HOTE         (WM_APP + 1)        /* lParam : string* JSON a poster a la page */
#define WM_JEU_FINI     (WM_APP + 2)

static const char *SHA1_USA    = "4352DC8438BCE03E792FAB51E5CBF24EF492F770";
static const char *SHA1_EUROPE = "5D2A818CCCD7829E2E17FABA9962279CA547FCE5";
static const char *ETAPES[6]   = { "Téléchargement", "Vérification du disque", "Extraction",
                                   "Recompilation vers C", "Compilation", "Finitions" };

static HWND g_fen;
static HANDLE g_un;                          /* une seule fenetre du lanceur a la fois */
static string g_lang = "en";                 /* fr, en, es, de, pt */
static bool g_apres_maj;                     /* relance apres la mise a jour du lanceur */

static string langue_windows()
{
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_FRENCH: return "fr";
    case LANG_SPANISH: return "es";
    case LANG_GERMAN: return "de";
    case LANG_PORTUGUESE: return "pt";
    default: return "en";
    }
}
/* les lignes que l'exe ajoute au journal d'installation : francais ou anglais, comme installer.ps1 */
static const char *tr(const char *fr, const char *en) { return g_lang == "fr" ? fr : en; }
static ComPtr<ICoreWebView2Environment> g_env;
static ComPtr<ICoreWebView2Controller> g_ctrl;
static ComPtr<ICoreWebView2> g_vue;

/* ── Chaines ───────────────────────────────────────────────────────── */

static string u8(const wstring &w)
{
    if (w.empty()) return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static wstring w16(const string &s)
{
    if (s.empty()) return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static string jstr(const string &s)
{
    string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') {}
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}
static string jstr(const wstring &w) { return jstr(u8(w)); }

/* Un JSON minimal : les messages de la page (objets plats, un objet "settings"). */
struct Val {
    enum { NUL, NUM, STR, BOOL, OBJ, ARR } t = NUL;
    double n = 0; bool b = false; string s;
    std::map<string, Val> o; std::vector<Val> a;
    const Val &operator[](const char *k) const { static Val vide; auto i = o.find(k); return i == o.end() ? vide : i->second; }
    string str() const { return t == STR ? s : string(); }
};

static void blanc(const char *&p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; }

static Val lire(const char *&p)
{
    Val v;
    blanc(p);
    if (*p == '{') {
        v.t = Val::OBJ; p++; blanc(p);
        while (*p && *p != '}') {
            Val k = lire(p); blanc(p);
            if (*p == ':') p++;
            v.o[k.s] = lire(p); blanc(p);
            if (*p == ',') p++;
            blanc(p);
        }
        if (*p) p++;
    } else if (*p == '[') {
        v.t = Val::ARR; p++; blanc(p);
        while (*p && *p != ']') { v.a.push_back(lire(p)); blanc(p); if (*p == ',') p++; blanc(p); }
        if (*p) p++;
    } else if (*p == '"') {
        v.t = Val::STR; p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
                switch (*p) {
                case 'n': v.s += '\n'; break; case 't': v.s += '\t'; break; case 'r': break;
                case 'u': {
                    unsigned c = (unsigned)strtoul(string(p + 1, 4).c_str(), NULL, 16); p += 4;
                    if (c >= 0xD800 && c < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
                        unsigned d = (unsigned)strtoul(string(p + 3, 4).c_str(), NULL, 16);
                        c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); p += 6;
                    }
                    wstring w; if (c >= 0x10000) { c -= 0x10000; w += (wchar_t)(0xD800 + (c >> 10)); w += (wchar_t)(0xDC00 + (c & 0x3FF)); } else w += (wchar_t)c;
                    v.s += u8(w); break;
                }
                default: v.s += *p;
                }
                p++;
            } else v.s += *p++;
        }
        if (*p) p++;
    } else if (!strncmp(p, "true", 4)) { v.t = Val::BOOL; v.b = true; p += 4; }
    else if (!strncmp(p, "false", 5)) { v.t = Val::BOOL; p += 5; }
    else if (!strncmp(p, "null", 4)) { p += 4; }
    else { char *e; v.t = Val::NUM; v.n = strtod(p, &e); p = e == p ? p + 1 : e; }
    return v;
}

/* ── Fichiers et dossiers ──────────────────────────────────────────── */

static bool existe(const wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool est_dossier(const wstring &p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }

static wstring dossier_appdata()
{
    wchar_t *p = NULL, ev[MAX_PATH]; wstring d;
    if (GetEnvironmentVariableW(L"DJFFNY_DONNEES", ev, MAX_PATH)) d = ev;   /* essais : une config a part */
    else if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &p))) d = wstring(p) + L"\\DJFFNY";
    CoTaskMemFree(p);
    CreateDirectoryW(d.c_str(), NULL);
    return d;
}

static string lire_fichier(const wstring &p)
{
    FILE *f = _wfopen(p.c_str(), L"rb"); string s;
    if (!f) return s;
    char b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

static bool ecrire_fichier(const wstring &p, const string &s)
{
    FILE *f = _wfopen(p.c_str(), L"wb");
    if (!f) return false;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return true;
}

static void creer_dossiers(const wstring &p) { SHCreateDirectoryExW(NULL, p.c_str(), NULL); }

static bool supprimer_arbre(const wstring &p)
{
    if (!existe(p)) return true;
    wstring z = p + L'\0';
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE; op.pFrom = z.c_str(); op.fFlags = FOF_NO_UI;
    return SHFileOperationW(&op) == 0;
}

static unsigned long long taille_arbre(const wstring &d)
{
    unsigned long long t = 0;
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) t += taille_arbre(d + L"\\" + f.cFileName);
        else t += ((unsigned long long)f.nFileSizeHigh << 32) | f.nFileSizeLow;
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return t;
}

static string go(unsigned long long o)
{
    char b[32];
    if (o >= 1000000000000ull) snprintf(b, sizeof b, "%.1f To", o / 1e12);
    else if (o >= 1000000000ull) snprintf(b, sizeof b, "%.2f Go", o / 1e9);
    else snprintf(b, sizeof b, "%.0f Mo", o / 1e6);
    for (char *c = b; *c; c++) if (*c == '.') *c = ',';
    return b;
}

/* ── La configuration du lanceur (%APPDATA%\DJFFNY\djffny.ini) ─────── */

struct Config {
    std::map<string, string> v;
    wstring chemin() { return dossier_appdata() + L"\\djffny.ini"; }
    void charger() {
        v.clear();
        string s = lire_fichier(chemin()); size_t i = 0;
        while (i < s.size()) {
            size_t e = s.find('\n', i); if (e == string::npos) e = s.size();
            string l = s.substr(i, e - i); i = e + 1;
            if (!l.empty() && l.back() == '\r') l.pop_back();
            size_t q = l.find('=');
            if (q != string::npos && l[0] != '#') v[l.substr(0, q)] = l.substr(q + 1);
        }
    }
    void enregistrer() {
        string s = "# DJFFNY.exe : l'installation du jeu (ecrit par le lanceur)\r\n";
        for (auto &kv : v) s += kv.first + "=" + kv.second + "\r\n";
        ecrire_fichier(chemin(), s);
    }
    string get(const char *k) { auto i = v.find(k); return i == v.end() ? string() : i->second; }
};
static Config g_cfg;
static std::mutex g_cfg_mx;

static wstring paquet() { return w16(g_cfg.get("paquet")); }
static wstring jeu() { return paquet() + L"\\defjam_recomp"; }
static wstring exe_jeu() { return jeu() + L"\\build\\Release\\defjam_recomp.exe"; }
static bool installe() { return !paquet().empty() && existe(exe_jeu()) && existe(jeu() + L"\\game_files\\default.xbe"); }
static bool dlss_construit() { return g_cfg.get("version_jeu_build") == "msvc" && existe(jeu() + L"\\tiers\\streamline\\include\\sl.h"); }

/* ── Messages vers la page (depuis n'importe quel fil) ─────────────── */

static void poster(const string &json)
{
    PostMessageW(g_fen, WM_HOTE, 0, (LPARAM)new string(json));
}
static void toast(const char *kind, const char *cle, const string &arg = string())
{
    poster("{\"type\":" + jstr(string(kind)) + ",\"key\":" + jstr(string(cle)) + ",\"arg\":" + jstr(arg) + "}");
}

/* ── defjam.ini : les reglages du jeu (le meme fichier que le lanceur du jeu, src/lanceur.c) ── */

struct Ini {
    struct L { string brut, cle, val; bool kv = false; };
    std::vector<L> l;
    bool charger(const wstring &p) {
        l.clear();
        string s = lire_fichier(p); if (s.empty() && !existe(p)) return false;
        size_t i = 0;
        while (i < s.size()) {
            size_t e = s.find('\n', i); if (e == string::npos) e = s.size();
            L x; x.brut = s.substr(i, e - i); i = e + 1;
            if (!x.brut.empty() && x.brut.back() == '\r') x.brut.pop_back();
            size_t a = x.brut.find_first_not_of(" \t"), q = x.brut.find('=');
            if (a != string::npos && x.brut[a] != '#' && q != string::npos && q > a) {
                x.kv = true; x.cle = x.brut.substr(a, q - a); x.val = x.brut.substr(q + 1);
                while (!x.cle.empty() && (x.cle.back() == ' ' || x.cle.back() == '\t')) x.cle.pop_back();
            }
            l.push_back(x);
        }
        return true;
    }
    string get(const char *k, const char *def) { for (auto &x : l) if (x.kv && x.cle == k) return x.val; return def; }
    void set(const char *k, const string &v) {
        for (auto &x : l) if (x.kv && x.cle == k) { x.val = v; return; }
        L x; x.kv = true; x.cle = k; x.val = v; l.push_back(x);
    }
    bool enregistrer(const wstring &p) {
        string s;
        for (auto &x : l) s += (x.kv ? x.cle + "=" + x.val : x.brut) + "\r\n";
        return ecrire_fichier(p, s);
    }
};

static bool on(Ini &ini, const char *k, const char *def) { return ini.get(k, def) != "0"; }

static string reglages_json()
{
    Ini ini; ini.charger(jeu() + L"\\defjam.ini");
    int res = atoi(ini.get("RECOMP_D3D11_SCALE", "3").c_str()); if (res < 1) res = 1; if (res > 6) res = 6;
    string pack = ini.get("RECOMP_TEX_PACK", "");
    auto b = [](bool x) { return x ? "true" : "false"; };
    string s = "{";
    s += "\"res\":" + std::to_string(res - 1);
    s += ",\"aspect\":" + string(on(ini, "RECOMP_WIDESCREEN", "1") ? "\"16:9\"" : "\"4:3\"");
    s += ",\"cutsceneFull\":" + string(b(on(ini, "RECOMP_WIDESCREEN_CINE", "1")));
    s += ",\"menus43\":" + string(b(on(ini, "RECOMP_UI_43", "0")));
    s += ",\"dlss\":" + string(dlss_construit() && on(ini, "RECOMP_DLSS", "0") ? "\"dlaa\"" : "\"off\"");
    s += ",\"fullscreen\":" + string(b(on(ini, "RECOMP_FULLSCREEN", "0")));
    s += ",\"bilinear\":" + string(b(on(ini, "RECOMP_TEX_FILTER", "1")));
    s += ",\"mips\":" + string(b(on(ini, "RECOMP_TEX_MIPS", "1")));
    s += ",\"effects\":" + string(b(on(ini, "RECOMP_COMBINERS", "1")));
    s += ",\"hdPack\":" + string(b(on(ini, "PACK_HD", pack.empty() ? "0" : "1") && !pack.empty()));
    {   /* defjam.ini est en ANSI (src/lanceur.c) */
        int n = MultiByteToWideChar(CP_ACP, 0, pack.c_str(), -1, NULL, 0);
        wstring w(n > 0 ? n - 1 : 0, 0);
        if (n > 1) MultiByteToWideChar(CP_ACP, 0, pack.c_str(), -1, &w[0], n);
        s += ",\"hdPath\":" + jstr(w);
    }
    s += ",\"mods\":" + string(b(on(ini, "RECOMP_MODS", "1")));
    s += ",\"unlockAll\":" + string(b(on(ini, "RECOMP_UNLOCK_ALL", "0")));
    s += ",\"xboxPad\":" + string(b(on(ini, "RECOMP_PAD_XBOX_LAYOUT", "0")));
    return s + "}";
}

static bool enregistrer_reglages(const Val &st)
{
    wstring p = jeu() + L"\\defjam.ini";
    Ini ini;
    if (!ini.charger(p)) {
        Ini::L x; x.brut = "# defjam.ini - reglages du jeu (DJFFNY.exe et le lanceur du jeu). Une ligne CLE=VALEUR."; ini.l.push_back(x);
    }
    auto b = [&](const char *k) { return st[k].t == Val::BOOL && st[k].b ? "1" : "0"; };
    ini.set("RECOMP_D3D11_SCALE", std::to_string((int)st["res"].n + 1));
    ini.set("RECOMP_WIDESCREEN", st["aspect"].str() == "16:9" ? "1" : "0");
    ini.set("RECOMP_WIDESCREEN_CINE", b("cutsceneFull"));
    ini.set("RECOMP_UI_43", b("menus43"));
    if (dlss_construit()) ini.set("RECOMP_DLSS", st["dlss"].str() == "dlaa" ? "1" : "0");
    ini.set("RECOMP_FULLSCREEN", b("fullscreen"));
    ini.set("RECOMP_TEX_FILTER", b("bilinear"));
    ini.set("RECOMP_TEX_MIPS", b("mips"));
    ini.set("RECOMP_TEX_ANISO", b("mips"));
    ini.set("RECOMP_COMBINERS", b("effects"));
    ini.set("PACK_HD", b("hdPack"));
    /* defjam.ini est lu en ANSI par le jeu (src/lanceur.c) */
    {
        wstring w = w16(st["hdPath"].str());
        int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
        string a(n > 0 ? n - 1 : 0, 0);
        if (n > 1) WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &a[0], n, NULL, NULL);
        ini.set("RECOMP_TEX_PACK", a);
    }
    ini.set("RECOMP_MODS", b("mods"));
    ini.set("RECOMP_UNLOCK_ALL", b("unlockAll"));
    ini.set("RECOMP_PAD_XBOX_LAYOUT", b("xboxPad"));
    return ini.enregistrer(p);
}

/* ── Processus : lancer, lire la sortie ligne a ligne, arreter tout l'arbre ── */

static HANDLE g_job_install;                 /* l'installation en cours (Annuler la tue) */
static std::atomic<bool> g_annule;

/* Une ligne de sortie en UTF-8 : telle quelle si elle en est, sinon lue dans la page de code
 * ANSI (Write-Host de Windows PowerShell 5.1 ignore [Console]::OutputEncoding quand la sortie
 * est un tube). */
static string en_utf8(const string &l)
{
    if (l.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, l.c_str(), (int)l.size(), NULL, 0)) return l;
    int n = MultiByteToWideChar(CP_ACP, 0, l.c_str(), (int)l.size(), NULL, 0);
    wstring w(n, 0);
    MultiByteToWideChar(CP_ACP, 0, l.c_str(), (int)l.size(), &w[0], n);
    return u8(w);
}

/* Lance cmd (dans dir) ; chaque ligne de sortie (en UTF-8) passe a ligne ; rend le code de sortie. */
static DWORD executer(const wstring &cmd, const wstring &dir, const std::function<void(const string &)> &ligne, HANDLE job)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 0)) return (DWORD)-1;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = w; si.hStdError = w; si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    wstring c = cmd;
    if (!CreateProcessW(NULL, &c[0], NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL,
                        dir.empty() ? NULL : dir.c_str(), &si, &pi)) {
        CloseHandle(r); CloseHandle(w); return (DWORD)-1;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(w);
    string tampon; char b[4096]; DWORD n;
    while (ReadFile(r, b, sizeof b, &n, NULL) && n) {
        tampon.append(b, n);
        size_t e;
        while ((e = tampon.find_first_of("\r\n")) != string::npos) {
            string l = tampon.substr(0, e); tampon.erase(0, e + 1);
            if (!l.empty() && ligne) ligne(en_utf8(l));
        }
    }
    if (!tampon.empty() && ligne) ligne(en_utf8(tampon));
    CloseHandle(r);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return code;
}

static wstring systeme(const wchar_t *exe)
{
    wchar_t d[MAX_PATH]; GetSystemDirectoryW(d, MAX_PATH);
    return wstring(L"\"") + d + L"\\" + exe + L"\"";
}

static wstring ps_quote(const wstring &s)
{
    wstring o = L"'";
    for (wchar_t c : s) { if (c == L'\'') o += L"''"; else o += c; }
    return o + L"'";
}

/* ── HTTPS (WinHTTP) ───────────────────────────────────────────────── */

/* GET url ; vers fichier (si chemin) ou en memoire ; progres(recu, total). */
static bool http_get(const wstring &url, string *mem, const wstring &fichier,
                     const std::function<bool(unsigned long long, unsigned long long)> &progres, string *err)
{
    URL_COMPONENTS uc = { sizeof uc };
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { if (err) *err = "bad URL"; return false; }
    HINTERNET s = WinHttpOpen(L"DJFFNY/" VERSION_LANCEUR_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!s) { if (err) *err = "WinHTTP unavailable"; return false; }
    DWORD proto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &proto, sizeof proto);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    bool ok = false;
    if (r && WinHttpSendRequest(r, L"Accept: */*\r\n", (DWORD)-1, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
        wchar_t lg[32] = L"0"; DWORD lsz = sizeof lg;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, NULL, lg, &lsz, NULL);
        unsigned long long total = _wtoi64(lg), recu = 0;
        if (code != 200) { if (err) *err = "HTTP " + std::to_string(code); }
        else {
            FILE *f = fichier.empty() ? NULL : _wfopen(fichier.c_str(), L"wb");
            if (!fichier.empty() && !f) { if (err) *err = "cannot write " + u8(fichier); }
            else {
                ok = true;
                char b[65536]; DWORD n;
                for (;;) {
                    if (!WinHttpReadData(r, b, sizeof b, &n)) { ok = false; if (err) *err = "connection lost"; break; }
                    if (!n) break;
                    if (f) fwrite(b, 1, n, f); else mem->append(b, n);
                    recu += n;
                    if (progres && !progres(recu, total)) { ok = false; if (err) *err = "cancelled"; break; }
                }
                if (f) fclose(f);
            }
        }
    } else if (err && err->empty()) *err = "cannot reach " + u8(host);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

/* ── L'image disque : XDVDFS, default.xbe et son empreinte ─────────── */

/* Le systeme de fichiers des disques Xbox : en-tete au secteur 32 de la partition de jeu, qui
 * commence a 0 dans un .iso extrait (xiso) ou plus loin dans une image complete du disque. */
static bool iso_default_xbe(const wstring &iso, unsigned long long *pos, unsigned *taille)
{
    HANDLE h = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    auto lire_a = [&](unsigned long long o, void *b, DWORD n) {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)o; DWORD lu = 0;
        return SetFilePointerEx(h, li, NULL, FILE_BEGIN) && ReadFile(h, b, n, &lu, NULL) && lu == n;
    };
    static const unsigned long long bases[] = { 0, 0x18300000ull, 0x2080000ull, 0xFD90000ull };
    bool trouve = false;
    for (unsigned long long base : bases) {
        unsigned char e[0x20];
        if (!lire_a(base + 0x10000, e, sizeof e) || memcmp(e, "MICROSOFT*XBOX*MEDIA", 20)) continue;
        unsigned rs = *(unsigned *)(e + 20), rz = *(unsigned *)(e + 24);
        if (!rz || rz > (1u << 24)) break;
        std::vector<unsigned char> t(rz);
        if (!lire_a(base + (unsigned long long)rs * 2048, t.data(), rz)) break;
        /* arbre binaire d'entrees : gauche/droite en mots de 4 octets */
        unsigned o = 0;
        for (int garde = 0; garde < 4096 && o + 14 <= rz; garde++) {
            unsigned short g = *(unsigned short *)&t[o], d = *(unsigned short *)&t[o + 2];
            unsigned lng = t[o + 13];
            if (o + 14 + lng > rz) break;
            string nom((char *)&t[o + 14], lng);
            int c = _stricmp("default.xbe", nom.c_str());
            if (!c) {
                *pos = base + (unsigned long long)*(unsigned *)&t[o + 4] * 2048;
                *taille = *(unsigned *)&t[o + 8];
                trouve = true; break;
            }
            unsigned suiv = c < 0 ? g : d;
            if (!suiv || suiv == 0xFFFF) break;
            o = suiv * 4u;
        }
        break;
    }
    CloseHandle(h);
    return trouve;
}

static string sha1_morceau(const wstring &iso, unsigned long long pos, unsigned taille)
{
    HANDLE h = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return string();
    std::vector<unsigned char> b(taille);
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)pos; DWORD lu = 0;
    bool ok = SetFilePointerEx(h, li, NULL, FILE_BEGIN) && ReadFile(h, b.data(), taille, &lu, NULL) && lu == taille;
    CloseHandle(h);
    if (!ok) return string();
    BCRYPT_ALG_HANDLE alg; unsigned char d[20]; string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0)) return string();
    BCryptHash(alg, NULL, 0, b.data(), taille, d, sizeof d);
    BCryptCloseAlgorithmProvider(alg, 0);
    for (unsigned char x : d) { char t[3]; snprintf(t, 3, "%02X", x); hex += t; }
    return hex;
}

static void verifier_iso(const wstring &iso)
{
    std::thread([iso] {
        WIN32_FILE_ATTRIBUTE_DATA a; unsigned long long sz = 0;
        if (GetFileAttributesExW(iso.c_str(), GetFileExInfoStandard, &a)) sz = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        unsigned long long pos; unsigned t;
        string statut = "unreadable";
        if (iso_default_xbe(iso, &pos, &t) && t > 0 && t < (64u << 20)) {
            string h = sha1_morceau(iso, pos, t);
            statut = h == SHA1_USA ? "valid" : h == SHA1_EUROPE ? "europe" : h.empty() ? "unreadable" : "other";
        }
        poster("{\"type\":\"isoChecked\",\"status\":" + jstr(statut) + ",\"file\":" + jstr(iso) + ",\"bytes\":" + std::to_string(sz) + "}");
    }).detach();
}

/* ── Visual Studio ─────────────────────────────────────────────────── */

static string g_vs;                          /* "Visual Studio Community 2022 17.12", vide sinon */

static void chercher_vs()
{
    wchar_t pf[MAX_PATH];
    if (!ExpandEnvironmentStringsW(L"%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe", pf, MAX_PATH) || !existe(pf)) return;
    string nom, ver;
    executer(L"\"" + wstring(pf) + L"\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property displayName -utf8",
             L"", [&](const string &l) { if (nom.empty()) nom = l; }, NULL);
    executer(L"\"" + wstring(pf) + L"\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property catalog_productDisplayVersion -utf8",
             L"", [&](const string &l) { if (ver.empty()) ver = l; }, NULL);
    if (!nom.empty()) g_vs = nom + (ver.empty() ? "" : " " + ver);
}

/* ── Le mod actif ──────────────────────────────────────────────────── */

static wstring dossier_mod() { return jeu() + L"\\game_files\\mod_actif"; }

static string mod_json()
{
    if (!installe()) return "null";
    Ini m; if (!m.charger(dossier_mod() + L"\\mod.ini")) return "null";
    string couv = existe(dossier_mod() + L"\\couverture") ? "https://djffny.local/couverture?" + std::to_string(GetTickCount()) : "";
    return "{\"name\":" + jstr(m.get("nom", "Mod")) + ",\"version\":" + jstr(m.get("version", "1.0")) +
           ",\"desc\":" + jstr(m.get("description", "")) + ",\"files\":" + jstr(m.get("fichiers", "0")) +
           ",\"date\":" + jstr(m.get("date", "")) + ",\"cover\":" + jstr(couv) + "}";
}

/* Retire le mod actif : ses fichiers, puis ce qu'il avait remplace. */
static void retirer_mod()
{
    wstring d = dossier_mod(), mods = jeu() + L"\\game_files\\mods";
    string liste = lire_fichier(d + L"\\fichiers.txt");
    size_t i = 0;
    while (i < liste.size()) {
        size_t e = liste.find('\n', i); if (e == string::npos) e = liste.size();
        string l = liste.substr(i, e - i); i = e + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.empty()) continue;
        wstring rel = w16(l);
        for (auto &c : rel) if (c == L'/') c = L'\\';
        DeleteFileW((mods + L"\\" + rel).c_str());
        wstring sauve = d + L"\\remplaces\\" + rel;
        if (existe(sauve)) MoveFileExW(sauve.c_str(), (mods + L"\\" + rel).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    }
    supprimer_arbre(d);
}

static std::atomic<bool> g_mod_occupe;

static void installer_mod(const wstring &fichier)
{
    if (g_mod_occupe.exchange(true)) return;
    std::thread([fichier] {
        wstring tar = systeme(L"tar.exe"), mods = jeu() + L"\\game_files\\mods", d = dossier_mod();
        string info;
        executer(tar + L" -x -O -f \"" + fichier + L"\" mod.ini", L"", [&](const string &l) { info += l + "\n"; }, NULL);
        std::vector<string> noms;
        DWORD c = executer(tar + L" -t -f \"" + fichier + L"\"", L"", [&](const string &l) { noms.push_back(l); }, NULL);
        if (c || noms.empty() || info.empty()) {
            toast("error", "notMod");
            poster("{\"type\":\"mod\",\"mod\":" + mod_json() + "}"); g_mod_occupe = false; return;
        }
        Ini mi; { wstring tmp = dossier_appdata() + L"\\mod.ini"; ecrire_fichier(tmp, info); mi.charger(tmp); DeleteFileW(tmp.c_str()); }
        string nom = mi.get("nom", "Mod");
        poster("{\"type\":\"modProgress\",\"name\":" + jstr(nom) + ",\"pct\":0}");
        if (existe(d)) retirer_mod();
        creer_dossiers(d + L"\\remplaces"); creer_dossiers(mods);
        /* ce que le mod va remplacer est mis de cote */
        string liste; unsigned fichiers = 0;
        for (auto &n : noms) {
            if (n.empty() || n.back() == '/' || n == "mod.ini" || !_strnicmp(n.c_str(), "couverture.", 11)) continue;
            wstring rel = w16(n); for (auto &ch : rel) if (ch == L'/') ch = L'\\';
            wstring cible = mods + L"\\" + rel;
            if (existe(cible)) {
                wstring s = d + L"\\remplaces\\" + rel;
                creer_dossiers(s.substr(0, s.find_last_of(L'\\')));
                CopyFileW(cible.c_str(), s.c_str(), FALSE);
            }
            liste += n + "\n"; fichiers++;
        }
        ecrire_fichier(d + L"\\fichiers.txt", liste);
        unsigned vus = 0; DWORD t0 = GetTickCount();
        c = executer(tar + L" -x -v -f \"" + fichier + L"\" -C \"" + mods + L"\" --exclude mod.ini --exclude \"couverture.*\"", L"",
                     [&](const string &) {
                         vus++;
                         if (GetTickCount() - t0 > 120) { t0 = GetTickCount(); char p[32]; snprintf(p, sizeof p, "%.3f", fichiers ? (double)vus / fichiers : 1.0);
                             poster("{\"type\":\"modProgress\",\"name\":" + jstr(nom) + ",\"pct\":" + p + "}"); }
                     }, NULL);
        /* la couverture, si le mod en a une */
        for (const char *ext : { "png", "jpg" }) {
            string n = string("couverture.") + ext;
            for (auto &x : noms) if (x == n) {
                executer(tar + L" -x -f \"" + fichier + L"\" -C \"" + d + L"\" " + w16(n), L"", nullptr, NULL);
                MoveFileExW((d + L"\\" + w16(n)).c_str(), (d + L"\\couverture").c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
        SYSTEMTIME st; GetLocalTime(&st); char date[16]; snprintf(date, sizeof date, "%02d/%02d/%04d", st.wDay, st.wMonth, st.wYear);
        mi.set("fichiers", std::to_string(fichiers)); mi.set("date", date); mi.set("source", u8(fichier));
        mi.enregistrer(d + L"\\mod.ini");
        if (c) toast("error", "modIncomplete", std::to_string(c));
        else toast("info", "modInstalled", nom);
        poster("{\"type\":\"mod\",\"mod\":" + mod_json() + "}");
        g_mod_occupe = false;
    }).detach();
}

/* ── L'installation du jeu ─────────────────────────────────────────── */

static std::atomic<bool> g_install_en_cours;

struct Avance {
    DWORD t0 = GetTickCount();
    int etape = 0; double pas = 0;
    std::mutex mx;
    /* part de chaque etape dans la barre, et duree typique (s) pour avancer celles sans mesure */
    const double poids[6] = { 0.08, 0.02, 0.10, 0.30, 0.45, 0.05 };
    const double duree[6] = { 30, 15, 120, 240, 420, 10 };
    DWORD t_etape = GetTickCount();
    void aller(int e) { std::lock_guard<std::mutex> l(mx); if (e > etape) { etape = e; pas = 0; t_etape = GetTickCount(); } }
    void envoyer(const string &ligne, const char *niveau = "info") {
        std::lock_guard<std::mutex> l(mx);
        double el = (GetTickCount() - t0) / 1000.0;
        double sp = pas > 0 ? pas : 1 - exp(-((GetTickCount() - t_etape) / 1000.0) / duree[etape]);
        if (sp > 0.97) sp = 0.97;
        double pct = 0; for (int i = 0; i < etape; i++) pct += poids[i];
        pct += poids[etape] * sp;
        char b[160]; snprintf(b, sizeof b, "{\"type\":\"installProgress\",\"step\":%d,\"stepPct\":%.3f,\"pct\":%.4f,\"elapsed\":%.1f", etape, sp, pct, el);
        string m = b;
        if (!ligne.empty()) m += ",\"line\":" + jstr(ligne) + ",\"level\":" + jstr(string(niveau));
        poster(m + "}");
    }
};

static wstring nom_paquet(const string &build)
{
    return build == "msvc" ? L"DJFFNY-Recomp-MSVC-DLSS.zip" : L"DJFFNY-Recomp-Clang-complet.zip";
}

/* Le dossier du paquet extrait : celui qui contient defjam_recomp\installeur\installer.ps1. */
static wstring trouver_paquet(const wstring &d)
{
    WIN32_FIND_DATAW f; wstring r;
    HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return r;
    do {
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || f.cFileName[0] == L'.') continue;
        wstring p = d + L"\\" + f.cFileName;
        if (existe(p + L"\\defjam_recomp\\installeur\\installer.ps1")) { r = p; break; }
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return r;
}

static void raccourci(const wstring &exe)
{
    wchar_t *bureau = NULL;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, NULL, &bureau))) return;
    ComPtr<IShellLinkW> sl;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        sl->SetPath(exe.c_str());
        sl->SetWorkingDirectory(exe.substr(0, exe.find_last_of(L'\\')).c_str());
        sl->SetDescription(L"Def Jam: Fight for NY — Recompiled");
        ComPtr<IPersistFile> pf;
        if (SUCCEEDED(sl.As(&pf))) pf->Save((wstring(bureau) + L"\\Def Jam Fight for NY.lnk").c_str(), TRUE);
    }
    CoTaskMemFree(bureau);
}

static bool installe();
static string versions_json();

/* Copie src vers dst, dossiers compris. */
static bool copier(const wstring &src, const wstring &dst)
{
    creer_dossiers(dst.substr(0, dst.find_last_of(L'\\')));
    return CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0;
}

/* Telecharge (ou prend DJFFNY_PAQUET) un paquet et l'extrait dans tmp ; rend le dossier du paquet. */
static wstring paquet_extrait(Avance &av, const wstring &dir, const wstring &nom, const wstring &tmp, string &e, string &a)
{
    wstring zip = dir + L"\\" + nom, local;
    wchar_t ev[MAX_PATH]; if (GetEnvironmentVariableW(L"DJFFNY_PAQUET", ev, MAX_PATH)) local = ev;
    if (!local.empty()) {
        av.envoyer(tr("paquet local : ", "local package: ") + u8(local));
        if (!CopyFileW(local.c_str(), zip.c_str(), FALSE)) { e = "localCopy"; return L""; }
    } else {
        wstring url = wstring(L"https://github.com/") + DEPOT + L"/releases/latest/download/" + nom;
        av.envoyer("GET " + u8(url));
        DWORD dern = 0; string err;
        if (!http_get(url, NULL, zip, [&](unsigned long long r, unsigned long long t) {
                if (GetTickCount() - dern > 200) { dern = GetTickCount();
                    { std::lock_guard<std::mutex> l(av.mx); av.pas = t ? (double)r / t : 0; }
                    av.envoyer(""); }
                return !g_annule.load(); }, &err)) { e = "download"; a = err; DeleteFileW(zip.c_str()); return L""; }
    }
    supprimer_arbre(tmp); creer_dossiers(tmp);
    av.envoyer(tr("extraction du paquet", "extracting the package"));
    DWORD c = executer(systeme(L"tar.exe") + L" -x -f \"" + zip + L"\" -C \"" + tmp + L"\"", L"", nullptr, g_job_install);
    DeleteFileW(zip.c_str());
    wstring src = c ? L"" : trouver_paquet(tmp);
    if (src.empty()) e = "badPackage";
    return src;
}

/* La mise a jour incrementale : le manifeste du nouveau paquet (empreinte de chaque fichier)
 * contre celui de l'installation ; seuls les fichiers changes sont copies, les disparus retires,
 * et le dossier outils (version Clang) n'est remplace que si son empreinte a change. Les dates
 * des fichiers inchanges ne bougent pas : la compilation qui suit ne reprend que le reste. */
static bool mettre_a_jour_fichiers(Avance &av, const wstring &pq, const string &build, string &e, string &a)
{
    wstring dir = pq.substr(0, pq.find_last_of(L'\\')), tmp = dir + L"\\maj_extraction";
    wstring src = paquet_extrait(av, dir, nom_paquet("msvc"), tmp, e, a);   /* le paquet leger : sans outils */
    if (src.empty()) { supprimer_arbre(tmp); return false; }
    string jn = lire_fichier(src + L"\\manifest.json"), ja = lire_fichier(pq + L"\\manifest.json");
    const char *pn = jn.c_str(), *pa = ja.c_str();
    Val nouv = lire(pn), anc = lire(pa);
    if (nouv["fichiers"].t != Val::OBJ) { e = "badPackage"; supprimer_arbre(tmp); return false; }
    unsigned changes = 0, retires = 0;
    for (auto &kv : nouv["fichiers"].o) {
        wstring rel = w16(kv.first); for (auto &c : rel) if (c == L'/') c = L'\\';
        if (anc["fichiers"][kv.first.c_str()].str() == kv.second.str() && existe(pq + L"\\" + rel)) continue;
        if (!copier(src + L"\\" + rel, pq + L"\\" + rel)) { e = "placeFailed"; a = kv.first; supprimer_arbre(tmp); return false; }
        changes++;
    }
    for (auto &kv : anc["fichiers"].o) {
        if (nouv["fichiers"].o.count(kv.first)) continue;
        wstring rel = w16(kv.first); for (auto &c : rel) if (c == L'/') c = L'\\';
        if (DeleteFileW((pq + L"\\" + rel).c_str())) retires++;
    }
    copier(src + L"\\manifest.json", pq + L"\\manifest.json");
    supprimer_arbre(tmp);
    char b[160]; snprintf(b, sizeof b, tr("%u fichier(s) mis à jour, %u retiré(s)", "%u file(s) updated, %u removed"), changes, retires);
    av.envoyer(b);
    if (build == "clang") {
        string vj = versions_json(); const char *pv = vj.c_str();
        string voulu = lire(pv)["outils"].str(), actuel;
        { std::lock_guard<std::mutex> l(g_cfg_mx); actuel = g_cfg.get("outils"); }
        if (!voulu.empty() && voulu != actuel) {
            av.envoyer(tr("les outils ont changé : téléchargement du paquet complet", "the tools changed: downloading the full package"));
            wstring t2 = dir + L"\\maj_outils";
            wstring s2 = paquet_extrait(av, dir, nom_paquet("clang"), t2, e, a);
            if (s2.empty() || !existe(s2 + L"\\outils")) { if (e.empty()) e = "badPackage"; supprimer_arbre(t2); return false; }
            supprimer_arbre(pq + L"\\outils");
            if (!MoveFileExW((s2 + L"\\outils").c_str(), (pq + L"\\outils").c_str(), MOVEFILE_COPY_ALLOWED)) { e = "placeFailed"; a = "outils"; supprimer_arbre(t2); return false; }
            supprimer_arbre(t2);
            std::lock_guard<std::mutex> l(g_cfg_mx); g_cfg.v["outils"] = voulu;
        }
    }
    return true;
}

static void installer(Val m)
{
    if (g_install_en_cours.exchange(true)) return;
    g_annule = false;
    std::thread([m] {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        Avance av;
        string raison = m["reason"].str(), build = m["build"].str() == "msvc" ? "msvc" : "clang";
        wstring dir = w16(m["dir"].str()), iso = w16(m["iso"].str());
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        {
            std::lock_guard<std::mutex> l(g_cfg_mx);
            if (raison != "install" && raison != "retry") { dir = w16(g_cfg.get("dossier")); if (iso.empty()) iso = w16(g_cfg.get("iso")); }
        }
        wstring ancien = paquet();
        bool reparer = raison == "repair" && !ancien.empty() && existe(ancien);
        g_job_install = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = {}; jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job_install, JobObjectExtendedLimitInformation, &jl, sizeof jl);

        bool maj = raison == "update" && installe();
        string erreur, arg; int etape_err = 0;
        auto echec = [&](const char *cle, const string &a = string()) { erreur = cle; arg = a; etape_err = av.etape; };
        wstring pq = reparer || maj ? ancien : L"";

        /* une installation, une reparation ou un changement de version refont tout depuis l'image
         * disque : si elle a disparu, la page la redemande (une mise a jour n'en a pas besoin) */
        if (!maj && !dir.empty() && !existe(iso)) {
            poster("{\"type\":\"needIso\",\"file\":" + jstr(iso) + "}");
            CloseHandle(g_job_install); g_job_install = NULL;
            g_install_en_cours = false;
            CoUninitialize();
            return;
        }
        if (dir.empty()) echec("noDir");
        creer_dossiers(dir);

        /* 1 bis. mise a jour : seuls les fichiers changes, et les outils s'ils ont change */
        if (erreur.empty() && maj) {
            string e, a;
            if (!mettre_a_jour_fichiers(av, pq, build, e, a)) echec(e.c_str(), a);
        }

        /* 1. le paquet : telecharge (ou DJFFNY_PAQUET), puis extrait a cote */
        if (erreur.empty() && !reparer && !maj) {
            wstring zip = dir + L"\\" + nom_paquet(build), local;
            wchar_t ev[MAX_PATH]; if (GetEnvironmentVariableW(L"DJFFNY_PAQUET", ev, MAX_PATH)) local = ev;
            if (!local.empty()) {
                av.envoyer(tr("paquet local : ", "local package: ") + u8(local));
                if (!CopyFileW(local.c_str(), zip.c_str(), FALSE)) echec("localCopy");
            } else {
                wstring url = wstring(L"https://github.com/") + DEPOT + L"/releases/latest/download/" + nom_paquet(build);
                av.envoyer("GET " + u8(url));
                DWORD dern = 0;
                if (!http_get(url, NULL, zip, [&](unsigned long long r, unsigned long long t) {
                        if (GetTickCount() - dern > 200) { dern = GetTickCount();
                            { std::lock_guard<std::mutex> l(av.mx); av.pas = t ? (double)r / t : 0; }
                            av.envoyer(""); }
                        return !g_annule.load(); }, &erreur))
                    echec("download", erreur);
                else {
                    WIN32_FILE_ATTRIBUTE_DATA a; unsigned long long z = 0;
                    if (GetFileAttributesExW(zip.c_str(), GetFileExInfoStandard, &a)) z = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
                    av.envoyer(tr("téléchargé : ", "downloaded: ") + u8(nom_paquet(build)) + " (" + go(z) + ")");
                }
            }
            if (erreur.empty() && !g_annule) {
                wstring tmp = dir + L"\\extraction";
                supprimer_arbre(tmp); creer_dossiers(tmp);
                av.envoyer(tr("extraction du paquet", "extracting the package"));
                DWORD c = executer(systeme(L"tar.exe") + L" -x -f \"" + zip + L"\" -C \"" + tmp + L"\"", L"", nullptr, g_job_install);
                wstring trouve = c ? L"" : trouver_paquet(tmp);
                if (trouve.empty()) echec("badPackage");
                else {
                    wstring nom = trouve.substr(trouve.find_last_of(L'\\') + 1), cible = dir + L"\\" + nom;
                    if (cible == ancien) { cible += L"-neuf"; }
                    supprimer_arbre(cible);
                    if (!MoveFileExW(trouve.c_str(), cible.c_str(), 0)) echec("placeFailed", u8(dir));
                    else {
                        pq = cible;
                        string mf = lire_fichier(pq + L"\\manifest.json"); const char *q = mf.c_str();
                        std::lock_guard<std::mutex> l(g_cfg_mx); g_cfg.v["outils"] = lire(q)["outils"].str();
                    }
                }
                supprimer_arbre(tmp);
            }
            DeleteFileW(zip.c_str());
        }

        /* 2. l'installateur du paquet : outils, disque, extraction, C, compilation, finitions */
        if (erreur.empty() && !g_annule) {
            string derniere_erreur;
            wstring ps1 = pq + L"\\defjam_recomp\\installeur\\installer.ps1";
            wstring cmd = systeme(L"WindowsPowerShell\\v1.0\\powershell.exe") +
                L" -NoProfile -ExecutionPolicy Bypass -Command \"[Console]::OutputEncoding=[Text.Encoding]::UTF8; & " +
                ps_quote(ps1) + (maj ? wstring(L" -MiseAJour") : L" -Iso " + ps_quote(iso)) + L" -Oui -Langue " + (g_lang == "fr" ? L"fr" : L"en") +
                (build == "clang" ? L" -SansDLSS" : L"") + L"; exit $LASTEXITCODE\"";
            std::atomic<bool> fini(false);
            std::thread tic([&] { while (!fini) { Sleep(500); if (!fini) av.envoyer(""); } });
            DWORD c = executer(cmd, pq + L"\\defjam_recomp", [&](const string &l) {
                size_t a = l.find("=== [");
                if (a != string::npos) {
                    int n = atoi(l.c_str() + a + 5);
                    static const int vers[7] = { 0, 0, 1, 2, 3, 4, 5 };
                    if (n >= 1 && n <= 6) av.aller(vers[n]);
                }
                bool err = l.find("ÉCHEC") != string::npos || l.find("FAILED") != string::npos;
                if (err) { derniere_erreur = l.substr(l.find(':') != string::npos ? l.find(':') + 2 : 0); }
                av.envoyer(l, err ? "error" : "info");
            }, g_job_install);
            fini = true; tic.join();
            if (g_annule) {}
            else if (c) { if (derniere_erreur.empty()) echec("installerStopped", std::to_string(c)); else echec("installer", derniere_erreur); }
        }

        /* 3. finitions : reglages et mod de l'ancienne installation, configuration, raccourci */
        if (erreur.empty() && !g_annule) {
            av.aller(5); av.envoyer(tr("finitions", "finishing"));
            if (!ancien.empty() && ancien != pq && existe(ancien)) {
                wstring a = ancien + L"\\defjam_recomp", n = pq + L"\\defjam_recomp";
                CopyFileW((a + L"\\defjam.ini").c_str(), (n + L"\\defjam.ini").c_str(), FALSE);
                if (existe(a + L"\\game_files\\mods")) { supprimer_arbre(n + L"\\game_files\\mods"); MoveFileExW((a + L"\\game_files\\mods").c_str(), (n + L"\\game_files\\mods").c_str(), MOVEFILE_COPY_ALLOWED); }
                if (existe(a + L"\\game_files\\mod_actif")) MoveFileExW((a + L"\\game_files\\mod_actif").c_str(), (n + L"\\game_files\\mod_actif").c_str(), MOVEFILE_COPY_ALLOWED);
                av.envoyer(tr("réglages et mod repris de l'installation précédente", "settings and mod kept from the previous install"));
                supprimer_arbre(ancien);
                if (pq.size() > 5 && pq.compare(pq.size() - 5, 5, L"-neuf") == 0) {
                    wstring propre = pq.substr(0, pq.size() - 5);
                    if (MoveFileExW(pq.c_str(), propre.c_str(), 0)) pq = propre;
                }
            }
            /* le lanceur s'installe a cote du jeu, avec un raccourci sur le Bureau */
            wchar_t moi[MAX_PATH]; GetModuleFileNameW(NULL, moi, MAX_PATH);
            wstring copie = dir + L"\\DJFFNY.exe";
            if (_wcsicmp(moi, copie.c_str())) CopyFileW(moi, copie.c_str(), FALSE);
            if (!GetEnvironmentVariableW(L"DJFFNY_PAQUET", NULL, 0)) raccourci(copie);   /* pas pendant un essai */
            string tag;
            { string j; if (http_get(wstring(L"https://api.github.com/repos/") + DEPOT + L"/releases/latest", &j, L"", nullptr, NULL)) {
                const char *p = j.c_str(); Val v = lire(p); tag = v["tag_name"].str(); } }
            SYSTEMTIME st; GetLocalTime(&st); char date[16]; snprintf(date, sizeof date, "%02d/%02d/%04d", st.wDay, st.wMonth, st.wYear);
            unsigned long long sz = taille_arbre(pq);
            {
                std::lock_guard<std::mutex> l(g_cfg_mx);
                g_cfg.v["dossier"] = u8(dir); g_cfg.v["paquet"] = u8(pq); if (!maj) g_cfg.v["iso"] = u8(iso);
                g_cfg.v["version_jeu_build"] = build; g_cfg.v["date"] = date; g_cfg.v["octets"] = std::to_string(sz);
                g_cfg.v.erase("taille");
                if (!tag.empty()) g_cfg.v["version_jeu"] = tag;
                g_cfg.enregistrer();
            }
            poster(string("{\"type\":\"installDone\",\"ok\":true,\"bytes\":") + std::to_string(sz) + ",\"build\":" + jstr(build) +
                   ",\"dlssBuilt\":" + (dlss_construit() ? "true" : "false") + "}");
        } else if (g_annule) {
            if (!reparer && !maj && !pq.empty() && pq != ancien) supprimer_arbre(pq);
            poster("{\"type\":\"installCancelled\"}");
        } else {
            av.envoyer(string(tr("ÉCHEC : ", "FAILED: ")) + erreur + (arg.empty() ? "" : " " + arg), "error");
            char b[64]; snprintf(b, sizeof b, ",\"step\":%d}", etape_err);
            poster("{\"type\":\"installDone\",\"ok\":false,\"errKey\":" + jstr(erreur) + ",\"errArg\":" + jstr(arg) + b);
        }
        CloseHandle(g_job_install); g_job_install = NULL;
        g_install_en_cours = false;
        CoUninitialize();
    }).detach();
}

/* ── Le jeu ────────────────────────────────────────────────────────── */

static HANDLE g_jeu_proc;

static void lancer_jeu()
{
    if (g_jeu_proc) return;
    if (!installe()) { toast("error", "notInstalled"); return; }
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    wstring cmd = L"\"" + exe_jeu() + L"\" --sans-fenetre";
    if (!CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, 0, NULL, jeu().c_str(), &si, &pi)) {
        toast("error", "launchRefused", std::to_string(GetLastError()));
        return;
    }
    CloseHandle(pi.hThread);
    g_jeu_proc = pi.hProcess;
    poster("{\"type\":\"gameStarted\"}");
    HANDLE h = pi.hProcess; DWORD t0 = GetTickCount();
    std::thread([h, t0] {
        WaitForSingleObject(h, INFINITE);
        DWORD code = 0; GetExitCodeProcess(h, &code);
        PostMessageW(g_fen, WM_JEU_FINI, code, (LPARAM)(GetTickCount() - t0));
    }).detach();
}

/* ── Mises a jour ──────────────────────────────────────────────────── */

/* versions.json de la derniere release ("" si introuvable : une release d'avant la 1.2.0) */
static string versions_json()
{
    string j;
    wchar_t ev[MAX_PATH];
    if (GetEnvironmentVariableW(L"DJFFNY_VERSIONS", ev, MAX_PATH)) return lire_fichier(ev);   /* essais */
    if (!http_get(wstring(L"https://github.com/") + DEPOT + L"/releases/latest/download/versions.json", &j, L"", nullptr, NULL)) return string();
    return j;
}

static bool plus_recente(const string &a, const string &b)      /* a > b, versions x.y.z */
{
    int x[3] = { 0 }, y[3] = { 0 };
    sscanf(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b.c_str(), "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

static void verifier_maj()
{
    std::thread([] {
        string j = versions_json();
        if (j.empty()) return;
        const char *p = j.c_str(); Val v = lire(p);
        string lanceur = v["lanceur"].str(), jeu_ = v["jeu"].str(), outils = v["outils"].str();
        if (plus_recente(lanceur, VERSION_LANCEUR)) {
            poster("{\"type\":\"update\",\"kind\":\"lanceur\",\"version\":" + jstr(lanceur) + "}");
            return;
        }
        if (!installe() || jeu_.empty()) return;
        string mf = lire_fichier(paquet() + L"\\manifest.json"); const char *q = mf.c_str();
        string empreinte = lire(q)["empreinte"].str(), build, outils_inst;
        { std::lock_guard<std::mutex> l(g_cfg_mx); build = g_cfg.get("version_jeu_build"); outils_inst = g_cfg.get("outils"); }
        if (empreinte != jeu_ || (build == "clang" && !outils.empty() && outils != outils_inst))
            poster("{\"type\":\"update\",\"kind\":\"jeu\",\"version\":" + jstr(lanceur) + "}");
    }).detach();
}

/* Le lanceur se remplace par celui de la derniere release, puis redemarre (WM_RELANCER). L'exe en
 * cours peut etre renomme mais pas ecrase : il devient DJFFNY.exe.ancien, efface au demarrage. */
static void maj_lanceur()
{
    std::thread([] {
        wchar_t moi[MAX_PATH]; GetModuleFileNameW(NULL, moi, MAX_PATH);
        wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t);
        wstring neuf = wstring(t) + L"DJFFNY_nouveau.exe", ancien = wstring(moi) + L".ancien";
        string err;
        wchar_t ev[MAX_PATH];
        bool ok = GetEnvironmentVariableW(L"DJFFNY_LANCEUR", ev, MAX_PATH)          /* essais : un exe local */
            ? CopyFileW(ev, neuf.c_str(), FALSE) != 0
            : http_get(wstring(L"https://github.com/") + DEPOT + L"/releases/latest/download/DJFFNY.exe", NULL, neuf, nullptr, &err);
        string tete = ok ? lire_fichier(neuf).substr(0, 2) : string();
        if (!ok || tete != "MZ") { DeleteFileW(neuf.c_str()); toast("error", "launcherUpdateFailed", err); return; }
        DeleteFileW(ancien.c_str());
        if (!MoveFileExW(moi, ancien.c_str(), MOVEFILE_REPLACE_EXISTING) || !CopyFileW(neuf.c_str(), moi, FALSE)) {
            MoveFileExW(ancien.c_str(), moi, MOVEFILE_REPLACE_EXISTING);
            DeleteFileW(neuf.c_str()); toast("error", "launcherUpdateFailed", "rename"); return;
        }
        /* la copie installee a cote du jeu aussi, si ce n'est pas elle qui tourne */
        string d; { std::lock_guard<std::mutex> l(g_cfg_mx); d = g_cfg.get("dossier"); }
        wstring copie = w16(d) + L"\\DJFFNY.exe";
        if (!d.empty() && _wcsicmp(copie.c_str(), moi) && existe(copie)) CopyFileW(neuf.c_str(), copie.c_str(), FALSE);
        DeleteFileW(neuf.c_str());
        PostMessageW(g_fen, WM_RELANCER, 0, 0);
    }).detach();
}

/* ── Dialogues ─────────────────────────────────────────────────────── */

static wstring choisir(bool dossier, const wchar_t *filtre_nom, const wchar_t *filtre)
{
    ComPtr<IFileOpenDialog> d; wstring r;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return r;
    DWORD o; d->GetOptions(&o);
    d->SetOptions(o | FOS_FORCEFILESYSTEM | (dossier ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!dossier) { COMDLG_FILTERSPEC f[2] = { { filtre_nom, filtre }, { L"Tous les fichiers", L"*.*" } }; d->SetFileTypes(2, f); }
    if (SUCCEEDED(d->Show(g_fen))) {
        ComPtr<IShellItem> it; wchar_t *p = NULL;
        if (SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { r = p; CoTaskMemFree(p); }
    }
    return r;
}

static string infos_disque(const wstring &chemin)
{
    wstring racine = chemin;
    while (!racine.empty() && !existe(racine)) { size_t s = racine.find_last_of(L'\\'); if (s == wstring::npos) break; racine.resize(s); }
    if (racine.size() == 2) racine += L"\\";
    ULARGE_INTEGER libre, total;
    if (racine.empty() || !GetDiskFreeSpaceExW(racine.c_str(), &libre, &total, NULL)) return "\"free\":0,\"drive\":\"\",\"used\":\"50%\"";
    char u[16]; snprintf(u, sizeof u, "%.0f%%", 100.0 * (1.0 - (double)libre.QuadPart / total.QuadPart));
    return "\"free\":" + std::to_string(libre.QuadPart) + ",\"drive\":" + jstr(u8(racine.substr(0, 2))) + ",\"used\":" + jstr(string(u));
}

/* Le chemin d'un fichier depose sur la page (postMessageWithAdditionalObjects). */
static wstring fichier_depose(ICoreWebView2WebMessageReceivedEventArgs *args)
{
    ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> a2; ComPtr<ICoreWebView2ObjectCollectionView> objs;
    UINT32 n = 0; wstring r;
    if (FAILED(args->QueryInterface(IID_PPV_ARGS(&a2))) || FAILED(a2->get_AdditionalObjects(&objs)) || !objs || FAILED(objs->get_Count(&n)) || !n) return r;
    ComPtr<IUnknown> u; ComPtr<ICoreWebView2File> f; LPWSTR p = NULL;
    if (SUCCEEDED(objs->GetValueAtIndex(0, &u)) && SUCCEEDED(u.As(&f)) && SUCCEEDED(f->get_Path(&p))) { r = p; CoTaskMemFree(p); }
    return r;
}

/* ── Les messages de la page ───────────────────────────────────────── */

static string etat_json()
{
    std::lock_guard<std::mutex> l(g_cfg_mx);
    bool ok = installe();
    string dir = g_cfg.get("dossier");
    if (dir.empty()) dir = "C:\\Jeux\\DefJam-FFNY";
    string s = "{\"installed\":" + string(ok ? "true" : "false");
    s += ",\"installDir\":" + jstr(dir) + "," + infos_disque(w16(dir));
    s += ",\"build\":" + jstr(g_cfg.get("version_jeu_build").empty() ? string("clang") : g_cfg.get("version_jeu_build"));
    s += ",\"vs\":" + jstr(g_vs) + ",\"dlssBuilt\":" + (ok && dlss_construit() ? "true" : "false");
    s += ",\"version\":\"" VERSION_LANCEUR "\",\"gameVersion\":" + jstr(g_cfg.get("version_jeu"));
    s += ",\"builtOn\":" + jstr(g_cfg.get("date")) + ",\"gameBytes\":" + std::to_string(_atoi64(g_cfg.get("octets").c_str()));
    s += ",\"lang\":" + jstr(g_lang) + ",\"langWindows\":" + jstr(langue_windows()) + ",\"langPref\":" + jstr(g_cfg.get("langue").empty() ? string("auto") : g_cfg.get("langue"));
    s += ",\"apresMaj\":" + string(g_apres_maj ? "true" : "false");
    wstring iso = w16(g_cfg.get("iso"));
    s += ",\"isoName\":" + jstr(iso.empty() ? string() : u8(iso.substr(iso.find_last_of(L'\\') + 1)));
    if (ok) s += ",\"settings\":" + reglages_json() + ",\"mod\":" + mod_json();
    return s + "}";
}

static void ouvrir(const wstring &p) { ShellExecuteW(g_fen, L"open", p.c_str(), NULL, NULL, SW_SHOWNORMAL); }

static void message(const string &json, ICoreWebView2WebMessageReceivedEventArgs *args)
{
    const char *p = json.c_str();
    Val m = lire(p);
    string t = m["type"].str();
    if (t == "getState") {
        poster("{\"type\":\"state\",\"state\":" + etat_json() + "}");
        verifier_maj();
    } else if (t == "setLang") {
        string l = m["lang"].str();
        {
            std::lock_guard<std::mutex> g(g_cfg_mx);
            if (l == "auto" || l.empty()) g_cfg.v.erase("langue"); else g_cfg.v["langue"] = l;
            g_cfg.enregistrer();
        }
        g_lang = (l == "auto" || l.empty()) ? langue_windows() : l;
    } else if (t == "updateLauncher") {
        maj_lanceur();
    } else if (t == "browseFolder") {
        wstring d = choisir(true, NULL, NULL);
        if (!d.empty()) poster("{\"type\":\"folderPicked\",\"path\":" + jstr(d) + "," + infos_disque(d) + "}");
    } else if (t == "checkFolder") {
        poster("{\"type\":\"folderPicked\",\"path\":" + jstr(m["path"].str()) + "," + infos_disque(w16(m["path"].str())) + "}");
    } else if (t == "pickIso") {
        wchar_t ev[8]; bool outils = GetEnvironmentVariableW(L"DJFFNY_OUTILS", ev, 8) > 0;
        wstring f = outils && !m["path"].str().empty() ? w16(m["path"].str())          /* essais pilotes */
                  : m["dropped"].b ? fichier_depose(args) : choisir(false, L"Image disque Xbox (*.iso)", L"*.iso");
        if (f.empty()) poster("{\"type\":\"isoChecked\"}"); else verifier_iso(f);
    } else if (t == "startInstall") {
        installer(m);
    } else if (t == "cancelInstall") {
        g_annule = true;
        if (g_job_install) TerminateJobObject(g_job_install, 1);
    } else if (t == "launch") {
        lancer_jeu();
    } else if (t == "saveSettings") {
        if (enregistrer_reglages(m["settings"])) poster("{\"type\":\"settingsSaved\"}");
        else toast("error", "settingsNotSaved");
    } else if (t == "browseHdPack") {
        wstring d = choisir(true, NULL, NULL);
        if (!d.empty()) poster("{\"type\":\"hdPicked\",\"path\":" + jstr(d) + "}");
    } else if (t == "installMod") {
        if (!installe()) return;
        wchar_t ev[8]; bool outils = GetEnvironmentVariableW(L"DJFFNY_OUTILS", ev, 8) > 0;
        wstring f = outils && !m["path"].str().empty() ? w16(m["path"].str()) : fichier_depose(args);   /* essais pilotes */
        if (f.empty()) f = choisir(false, L"Mod Def Jam (*.mod)", L"*.mod;*.zip");
        if (!f.empty()) installer_mod(f);
    } else if (t == "removeMod") {
        std::thread([] { retirer_mod(); toast("info", "modRemoved"); poster("{\"type\":\"mod\",\"mod\":null}"); }).detach();
    } else if (t == "openLog") {
        wstring a = jeu() + L"\\run_stderr.txt", b = jeu() + L"\\installation.log";
        FILETIME fa = {}, fb = {}; WIN32_FILE_ATTRIBUTE_DATA d;
        if (GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &d)) fa = d.ftLastWriteTime;
        if (GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &d)) fb = d.ftLastWriteTime;
        if (paquet().empty() || (!existe(a) && !existe(b))) {
            /* une installation ratee : son journal est dans le paquet en cours */
            toast("info", "noLog");
        } else ouvrir(CompareFileTime(&fa, &fb) >= 0 && existe(a) ? a : b);
    } else if (t == "openFolder") {
        string w = m["what"].str();
        if (w == "mods") { creer_dossiers(jeu() + L"\\game_files\\mods"); ouvrir(jeu() + L"\\game_files\\mods"); }
        else if (w == "game") ouvrir(w16(g_cfg.get("dossier")));
        else if (w == "licences") ouvrir(existe(paquet() + L"\\xboxrecomp\\LICENSES") ? paquet() + L"\\xboxrecomp\\LICENSES" : wstring(L"https://github.com/") + DEPOT);
    } else if (t == "openUrl") {
        string u = m["url"].str();
        if (u.rfind("https://github.com/comoria08/", 0) == 0) ouvrir(w16(u));
    } else if (t == "uninstall") {
        std::thread([] {
            wstring pq = paquet();
            bool ok = supprimer_arbre(pq);
            {
                std::lock_guard<std::mutex> l(g_cfg_mx);
                wstring dir = w16(g_cfg.get("dossier"));
                g_cfg.v.erase("paquet"); g_cfg.v.erase("version_jeu"); g_cfg.v.erase("octets"); g_cfg.v.erase("date"); g_cfg.v.erase("outils");
                g_cfg.enregistrer();
                (void)dir;
            }
            if (ok) poster("{\"type\":\"uninstalled\"}");
            else toast("error", "uninstallIncomplete");
        }).detach();
    }
}

/* Les deux textes des boites de message de Windows (la page n'est pas la pour les traduire). */
static const wchar_t *natif(int quoi)
{
    static const wchar_t *t[2][5] = {
        { L"Le moteur WebView2 de Microsoft est introuvable.\n\nInstallez « Microsoft Edge WebView2 Runtime » (https://go.microsoft.com/fwlink/p/?LinkId=2124703), puis relancez DJFFNY.exe.",
          L"Microsoft's WebView2 engine was not found.\n\nInstall \"Microsoft Edge WebView2 Runtime\" (https://go.microsoft.com/fwlink/p/?LinkId=2124703), then run DJFFNY.exe again.",
          L"No se encontró el motor WebView2 de Microsoft.\n\nInstala «Microsoft Edge WebView2 Runtime» (https://go.microsoft.com/fwlink/p/?LinkId=2124703) y vuelve a abrir DJFFNY.exe.",
          L"Die WebView2-Engine von Microsoft wurde nicht gefunden.\n\nInstalliere „Microsoft Edge WebView2 Runtime“ (https://go.microsoft.com/fwlink/p/?LinkId=2124703) und starte DJFFNY.exe neu.",
          L"O mecanismo WebView2 da Microsoft não foi encontrado.\n\nInstale o «Microsoft Edge WebView2 Runtime» (https://go.microsoft.com/fwlink/p/?LinkId=2124703) e abra o DJFFNY.exe novamente." },
        { L"Une installation est en cours. Quitter l'arrête.\n\nQuitter quand même ?",
          L"An installation is in progress. Quitting stops it.\n\nQuit anyway?",
          L"Hay una instalación en curso. Salir la detiene.\n\n¿Salir de todos modos?",
          L"Eine Installation läuft. Beenden bricht sie ab.\n\nTrotzdem beenden?",
          L"Há uma instalação em andamento. Sair a interrompe.\n\nSair mesmo assim?" } };
    int i = g_lang == "fr" ? 0 : g_lang == "es" ? 2 : g_lang == "de" ? 3 : g_lang == "pt" ? 4 : 1;
    return t[quoi][i];
}

/* ── Les fichiers servis a la page (https://djffny.local/...) ──────── */

static bool ressource(const wchar_t *nom, const void **d, DWORD *n)
{
    HRSRC r = FindResourceW(NULL, nom, RT_RCDATA);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    if (!g) return false;
    *d = LockResource(g); *n = SizeofResource(NULL, r);
    return *d != NULL;
}

static void servir(ICoreWebView2WebResourceRequestedEventArgs *args)
{
    ComPtr<ICoreWebView2WebResourceRequest> req; LPWSTR uri = NULL;
    args->get_Request(&req); req->get_Uri(&uri);
    wstring u = uri; CoTaskMemFree(uri);
    wstring chemin = u.substr(wcslen(HOTE)); chemin = chemin.substr(0, chemin.find(L'?'));
    const void *d = NULL; DWORD n = 0; string fichier; const wchar_t *type = L"application/octet-stream";
    if (chemin == L"page.html" || chemin.empty()) { ressource(L"PAGE", &d, &n); type = L"text/html; charset=utf-8"; }
    else if (chemin == L"icone.png") { ressource(L"ICONE_PNG", &d, &n); type = L"image/png"; }
    else if (chemin == L"logo.png") { ressource(L"LOGO", &d, &n); type = L"image/png"; }
    else if (chemin == L"accueil.jpg") { ressource(L"ACCUEIL", &d, &n); type = L"image/jpeg"; }
    else if (chemin == L"mod_defaut.jpg") { ressource(L"MOD_DEFAUT", &d, &n); type = L"image/jpeg"; }
    else if (chemin == L"aucun_mod.png") { ressource(L"AUCUN_MOD", &d, &n); type = L"image/png"; }
    else if (chemin == L"installation.jpg") { ressource(L"FOND", &d, &n); type = L"image/jpeg"; }
    else if (chemin == L"couverture") { fichier = lire_fichier(dossier_mod() + L"\\couverture"); d = fichier.data(); n = (DWORD)fichier.size();
        type = fichier.size() > 2 && (unsigned char)fichier[0] == 0xFF && (unsigned char)fichier[1] == 0xD8 ? L"image/jpeg" : L"image/png"; }
    ComPtr<ICoreWebView2WebResourceResponse> rep;
    ComPtr<IStream> s;
    if (d && n) s.Attach(SHCreateMemStream((const BYTE *)d, n));
    wstring tete = wstring(L"Content-Type: ") + type + L"\r\nCache-Control: no-store";
    g_env->CreateWebResourceResponse(s.Get(), s ? 200 : 404, s ? L"OK" : L"Not Found", tete.c_str(), &rep);
    args->put_Response(rep.Get());
}

/* ── WebView2 : des rappels COM portes par des lambdas ─────────────── */

template <class I, class... A>
struct Rappel : I {
    std::function<HRESULT(A...)> f; ULONG refs = 1;
    explicit Rappel(std::function<HRESULT(A...)> g) : f(std::move(g)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
        if (id == IID_IUnknown || id == __uuidof(I)) { *p = static_cast<I *>(this); AddRef(); return S_OK; }
        *p = NULL; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE Invoke(A... a) override { return f(a...); }
};
template <class I, class... A>
static ComPtr<I> rappel(std::function<HRESULT(A...)> f) { ComPtr<I> p; p.Attach(new Rappel<I, A...>(std::move(f))); return p; }

static void ajuster()
{
    if (!g_ctrl) return;
    RECT r; GetClientRect(g_fen, &r);
    g_ctrl->put_Bounds(r);
}

static void demarrer_webview()
{
    wstring donnees = dossier_appdata() + L"\\WebView2";
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(NULL, donnees.c_str(), NULL,
        rappel<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment *>(
        [](HRESULT r, ICoreWebView2Environment *env) -> HRESULT {
            if (FAILED(r) || !env) {
                MessageBoxW(g_fen, natif(0), L"Def Jam: Fight for NY", MB_ICONERROR);
                PostQuitMessage(1); return S_OK;
            }
            g_env = env;
            return env->CreateCoreWebView2Controller(g_fen,
                rappel<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller *>(
                [](HRESULT r2, ICoreWebView2Controller *c) -> HRESULT {
                    if (FAILED(r2) || !c) return S_OK;
                    g_ctrl = c; c->get_CoreWebView2(&g_vue);
                    ComPtr<ICoreWebView2Controller2> c2;
                    if (SUCCEEDED(g_ctrl.As(&c2))) { COREWEBVIEW2_COLOR noir = { 255, 11, 11, 11 }; c2->put_DefaultBackgroundColor(noir); }
                    ComPtr<ICoreWebView2Settings> st; g_vue->get_Settings(&st);
                    wchar_t ev[8]; bool outils = GetEnvironmentVariableW(L"DJFFNY_OUTILS", ev, 8) > 0;
                    st->put_AreDevToolsEnabled(outils); st->put_AreDefaultContextMenusEnabled(outils);
                    st->put_IsStatusBarEnabled(FALSE); st->put_IsZoomControlEnabled(FALSE);
                    ComPtr<ICoreWebView2Settings3> st3;
                    if (SUCCEEDED(st.As(&st3))) st3->put_AreBrowserAcceleratorKeysEnabled(outils);
                    EventRegistrationToken tok;
                    g_vue->add_WebMessageReceived(rappel<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *>(
                        [](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *a) -> HRESULT {
                            LPWSTR j = NULL;
                            if (SUCCEEDED(a->get_WebMessageAsJson(&j))) { message(u8(j), a); CoTaskMemFree(j); }
                            return S_OK; }).Get(), &tok);
                    g_vue->AddWebResourceRequestedFilter(HOTE L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    g_vue->add_WebResourceRequested(rappel<ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *>(
                        [](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *a) -> HRESULT { servir(a); return S_OK; }).Get(), &tok);
                    /* un lien suivi dans la page s'ouvre dans le navigateur, pas dans le lanceur */
                    g_vue->add_NewWindowRequested(rappel<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *>(
                        [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *a) -> HRESULT { a->put_Handled(TRUE); return S_OK; }).Get(), &tok);
                    ajuster();
                    g_vue->Navigate(HOTE L"page.html");
                    return S_OK;
                }).Get());
        }).Get());
    if (FAILED(hr)) {
        MessageBoxW(g_fen, natif(0), L"Def Jam: Fight for NY", MB_ICONERROR);
        PostQuitMessage(1);
    }
}

/* ── La fenetre ────────────────────────────────────────────────────── */

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: ajuster(); return 0;
    case WM_GETMINMAXINFO: {
        UINT dpi = GetDpiForWindow(h);
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = MulDiv(960 + 16, dpi, 96);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = MulDiv(600 + 39, dpi, 96);
        return 0;
    }
    case WM_HOTE: {
        string *s = (string *)lp;
        if (g_vue) g_vue->PostWebMessageAsJson(w16(*s).c_str());
        delete s;
        return 0;
    }
    case WM_JEU_FINI: {
        if (g_jeu_proc) { CloseHandle(g_jeu_proc); g_jeu_proc = NULL; }
        poster("{\"type\":\"gameExited\"}");
        DWORD code = (DWORD)wp, duree = (DWORD)lp;
        if (code && duree < 10000) {
            char b[16]; snprintf(b, sizeof b, "0x%08X", (unsigned)code);
            toast("error", "launchFailed", b);
        }
        return 0;
    }
    case WM_CLOSE:
        if (g_install_en_cours && MessageBoxW(h, natif(1), L"Def Jam: Fight for NY", MB_YESNO | MB_ICONWARNING) != IDYES)
            return 0;
        if (g_job_install) TerminateJobObject(g_job_install, 1);
        DestroyWindow(h);
        return 0;
    case WM_RELANCER: {
        /* le nouvel exe est en place : liberer le verrou "une seule fenetre", le lancer, partir */
        wchar_t moi[MAX_PATH]; GetModuleFileNameW(NULL, moi, MAX_PATH);
        wstring cmd = L"\"" + wstring(moi) + L"\" --apres-maj";
        if (g_un) { ReleaseMutex(g_un); CloseHandle(g_un); g_un = NULL; }
        STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
        if (CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        DestroyWindow(h);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    /* une seule fenetre du lanceur a la fois */
    g_apres_maj = wcsstr(GetCommandLineW(), L"--apres-maj") != NULL;
    if (g_apres_maj) Sleep(800);              /* l'ancien lanceur finit de se fermer */
    g_un = CreateMutexW(NULL, TRUE, L"DJFFNY_Lanceur");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND autre = FindWindowW(L"DJFFNY", NULL);
        if (autre) { ShowWindow(autre, SW_RESTORE); SetForegroundWindow(autre); }
        return 0;
    }
    g_cfg.charger();
    g_lang = g_cfg.get("langue").empty() ? langue_windows() : g_cfg.get("langue");
    {   /* l'exe remplace par la derniere mise a jour du lanceur */
        wchar_t moi[MAX_PATH]; GetModuleFileNameW(NULL, moi, MAX_PATH);
        DeleteFileW((wstring(moi) + L".ancien").c_str());
    }
    std::thread(chercher_vs).detach();

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"DJFFNY";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(11, 11, 11));
    wc.hIcon = (HICON)LoadImageW(inst, L"ICONE", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.hIconSm = (HICON)LoadImageW(inst, L"ICONE", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    RECT r = { 0, 0, MulDiv(1100, dpi, 96), MulDiv(700, dpi, 96) };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    g_fen = CreateWindowExW(0, wc.lpszClassName, L"Def Jam: Fight for NY — Recompiled", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    /* fenetre sombre, comme la page */
    BOOL sombre = TRUE;
    {
        typedef HRESULT(WINAPI * F)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        F f = dwm ? (F)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
        if (f) f(g_fen, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &sombre, sizeof sombre);
    }
    ShowWindow(g_fen, show);
    demarrer_webview();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    g_vue.Reset(); g_ctrl.Reset(); g_env.Reset();
    CoUninitialize();
    return 0;
}
