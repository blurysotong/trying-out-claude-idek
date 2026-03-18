/*
 * Monarch ModKit — Project Wingman Mod Manager
 * Pure Win32 C — no dependencies, single .exe
 *
 * Architecture:
 *   - Mods stored in %APPDATA%\MonarchModKit\library\  (safe copies of your .pak files)
 *   - Profile saved as %APPDATA%\MonarchModKit\profile.json
 *   - Deploy: copies enabled mods into <game>\ProjectWingman\Content\Paks\~mods\
 *             with alphabetical prefixes (00_, 01_, ...) to enforce load order
 *   - Undeploy: removes only files we placed, tracked via deployed.json
 *
 * Build: see BUILD.bat (uses MinGW gcc, ships with MSYS2)
 */

/* UNICODE, _UNICODE, WIN32_LEAN_AND_MEAN passed via -D compiler flags */
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

/* ── Constants ───────────────────────────────────────────────────────────── */
#define APP_NAME        L"Monarch ModKit"
#define APP_VERSION     L"1.0.0"
#define MAX_MODS        256
#define MAX_PATH_LEN    1024
#define MAX_STR         256

/* Control IDs */
#define ID_LIST         1001
#define ID_BTN_ADD      1010
#define ID_BTN_DEPLOY   1011
#define ID_BTN_UNDEPLOY 1012
#define ID_BTN_SETTINGS 1013
#define ID_BTN_LAUNCH   1014
#define ID_BTN_ALL_ON   1015
#define ID_BTN_ALL_OFF  1016
#define ID_BTN_CONFLICTS 1017
#define ID_SEARCH       1020
#define ID_FILTER_ALL   1030
#define ID_FILTER_ON    1031
#define ID_FILTER_OFF   1032
#define ID_FILTER_GAMEPLAY 1033
#define ID_FILTER_VISUAL   1034
#define ID_FILTER_AUDIO    1035
#define ID_FILTER_UI       1036
#define ID_FILTER_MAP      1037
#define ID_STATUS_BAR   1050

/* Colours (COLORREF = BGR) */
#define COL_BG          RGB(8,  12, 10)
#define COL_BG2         RGB(13, 20, 16)
#define COL_BG3         RGB(17, 26, 20)
#define COL_GREEN       RGB(0, 255, 136)
#define COL_GREEN_DIM   RGB(0, 180, 100)
#define COL_AMBER       RGB(255, 183, 0)
#define COL_RED         RGB(255, 68, 68)
#define COL_BLUE        RGB(68, 170, 255)
#define COL_PURPLE      RGB(180, 127, 255)
#define COL_FG          RGB(200, 232, 208)
#define COL_FG_DIM      RGB(90, 138, 106)
#define COL_BORDER      RGB(26, 48, 32)

/* ── Mod structure ───────────────────────────────────────────────────────── */
typedef enum {
    CAT_GAMEPLAY = 0,
    CAT_VISUAL,
    CAT_AUDIO,
    CAT_UI,
    CAT_MAP,
    CAT_OTHER
} ModCategory;

typedef struct {
    int          id;
    BOOL         enabled;
    ModCategory  category;
    WCHAR        name[MAX_STR];
    WCHAR        author[MAX_STR];
    WCHAR        version[64];
    WCHAR        notes[MAX_STR];
    WCHAR        library_path[MAX_PATH_LEN]; /* path to our internal copy */
    WCHAR        added[64];                  /* ISO date string */
} Mod;

/* ── App state ───────────────────────────────────────────────────────────── */
typedef struct {
    Mod    mods[MAX_MODS];
    int    mod_count;
    int    next_id;
    WCHAR  game_path[MAX_PATH_LEN];
    WCHAR  data_dir[MAX_PATH_LEN];
    WCHAR  library_dir[MAX_PATH_LEN];
    WCHAR  profile_path[MAX_PATH_LEN];
    WCHAR  deployed_path[MAX_PATH_LEN];
} AppState;

static AppState g_state;
static HWND     g_hwnd;
static HWND     g_list;
static HWND     g_status;
static HWND     g_search;
static HFONT    g_font_mono;
static HFONT    g_font_sans;
static HFONT    g_font_title;
static HBRUSH   g_br_bg;
static HBRUSH   g_br_bg2;
static HBRUSH   g_br_bg3;
static int      g_filter = 0;  /* 0=all,1=on,2=off,3-7=category */

/* ── Forward declarations ────────────────────────────────────────────────── */
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void RefreshList(void);
static void SetStatus(const WCHAR *msg, COLORREF col);
void SaveProfile(void);
void LoadProfile(void);
void DeployMods(void);
void UndeployMods(void);

/* ══════════════════════════════════════════════════════════════════════════
   JSON helpers  (minimal hand-rolled, no external lib)
   ══════════════════════════════════════════════════════════════════════════ */

/* Write an escaped JSON string value */
static void json_write_str(FILE *f, const WCHAR *s) {
    fwprintf(f, L"\"");
    for (; *s; s++) {
        if      (*s == L'"')  fwprintf(f, L"\\\"");
        else if (*s == L'\\') fwprintf(f, L"\\\\");
        else if (*s == L'\n') fwprintf(f, L"\\n");
        else                  fwprintf(f, L"%c", *s);
    }
    fwprintf(f, L"\"");
}

static const WCHAR *cat_str(ModCategory c) {
    switch(c) {
        case CAT_GAMEPLAY: return L"gameplay";
        case CAT_VISUAL:   return L"visual";
        case CAT_AUDIO:    return L"audio";
        case CAT_UI:       return L"ui";
        case CAT_MAP:      return L"map";
        default:           return L"other";
    }
}

static ModCategory cat_from_str(const WCHAR *s) {
    if (wcscmp(s, L"gameplay") == 0) return CAT_GAMEPLAY;
    if (wcscmp(s, L"visual")   == 0) return CAT_VISUAL;
    if (wcscmp(s, L"audio")    == 0) return CAT_AUDIO;
    if (wcscmp(s, L"ui")       == 0) return CAT_UI;
    if (wcscmp(s, L"map")      == 0) return CAT_MAP;
    return CAT_OTHER;
}

/* ── Save profile.json ───────────────────────────────────────────────────── */
void SaveProfile(void) {
    FILE *f = _wfopen(g_state.profile_path, L"w, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"{\n");
    fwprintf(f, L"  \"next_id\": %d,\n", g_state.next_id);
    fwprintf(f, L"  \"game_path\": ");
    json_write_str(f, g_state.game_path);
    fwprintf(f, L",\n");
    fwprintf(f, L"  \"mods\": [\n");
    for (int i = 0; i < g_state.mod_count; i++) {
        Mod *m = &g_state.mods[i];
        fwprintf(f, L"    {\n");
        fwprintf(f, L"      \"id\": %d,\n", m->id);
        fwprintf(f, L"      \"enabled\": %s,\n", m->enabled ? L"true" : L"false");
        fwprintf(f, L"      \"category\": \"%s\",\n", cat_str(m->category));
        fwprintf(f, L"      \"name\": ");      json_write_str(f, m->name);      fwprintf(f, L",\n");
        fwprintf(f, L"      \"author\": ");    json_write_str(f, m->author);    fwprintf(f, L",\n");
        fwprintf(f, L"      \"version\": ");   json_write_str(f, m->version);   fwprintf(f, L",\n");
        fwprintf(f, L"      \"notes\": ");     json_write_str(f, m->notes);     fwprintf(f, L",\n");
        fwprintf(f, L"      \"library_path\": "); json_write_str(f, m->library_path); fwprintf(f, L",\n");
        fwprintf(f, L"      \"added\": ");     json_write_str(f, m->added);     fwprintf(f, L"\n");
        fwprintf(f, L"    }%s\n", (i < g_state.mod_count-1) ? L"," : L"");
    }
    fwprintf(f, L"  ]\n}\n");
    fclose(f);
}

/* ── Minimal JSON field extractor ────────────────────────────────────────── */
static BOOL json_get_str(const WCHAR *json, const WCHAR *key, WCHAR *out, int out_len) {
    WCHAR pattern[MAX_STR];
    swprintf(pattern, MAX_STR, L"\"%s\"", key);
    const WCHAR *p = wcsstr(json, pattern);
    if (!p) return FALSE;
    p += wcslen(pattern);
    while (*p == L' ' || *p == L':' || *p == L' ') p++;
    if (*p != L'"') return FALSE;
    p++;
    int i = 0;
    while (*p && *p != L'"' && i < out_len-1) {
        if (*p == L'\\' && *(p+1)) { p++; }
        out[i++] = *p++;
    }
    out[i] = 0;
    return TRUE;
}

static int json_get_int(const WCHAR *json, const WCHAR *key) {
    WCHAR pattern[MAX_STR];
    swprintf(pattern, MAX_STR, L"\"%s\"", key);
    const WCHAR *p = wcsstr(json, pattern);
    if (!p) return 0;
    p += wcslen(pattern);
    while (*p == L' ' || *p == L':') p++;
    return _wtoi(p);
}

static BOOL json_get_bool(const WCHAR *json, const WCHAR *key) {
    WCHAR pattern[MAX_STR];
    swprintf(pattern, MAX_STR, L"\"%s\"", key);
    const WCHAR *p = wcsstr(json, pattern);
    if (!p) return FALSE;
    p += wcslen(pattern);
    while (*p == L' ' || *p == L':') p++;
    return (wcsncmp(p, L"true", 4) == 0);
}

/* ── Load profile.json ───────────────────────────────────────────────────── */
void LoadProfile(void) {
    g_state.mod_count = 0;
    g_state.next_id   = 1;
    g_state.game_path[0] = 0;

    FILE *f = _wfopen(g_state.profile_path, L"r, ccs=UTF-8");
    if (!f) return;

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 1024*1024) { fclose(f); return; }

    WCHAR *buf = (WCHAR*)malloc((sz+2) * sizeof(WCHAR));
    if (!buf) { fclose(f); return; }
    int nch = (int)fread(buf, sizeof(WCHAR), sz/sizeof(WCHAR), f);
    buf[nch] = 0;
    fclose(f);

    g_state.next_id = json_get_int(buf, L"next_id");
    if (g_state.next_id < 1) g_state.next_id = 1;
    json_get_str(buf, L"game_path", g_state.game_path, MAX_PATH_LEN);

    /* Parse mods array — find each object between { } */
    const WCHAR *mods_start = wcsstr(buf, L"\"mods\"");
    if (!mods_start) { free(buf); return; }
    const WCHAR *arr = wcsstr(mods_start, L"[");
    if (!arr) { free(buf); return; }

    const WCHAR *p = arr + 1;
    while (*p && g_state.mod_count < MAX_MODS) {
        /* Find next { */
        while (*p && *p != L'{' && *p != L']') p++;
        if (!*p || *p == L']') break;

        /* Find matching } — naive single-level scan */
        const WCHAR *obj_start = p;
        int depth = 0;
        const WCHAR *obj_end = p;
        while (*obj_end) {
            if (*obj_end == L'{') depth++;
            else if (*obj_end == L'}') { depth--; if (depth == 0) { obj_end++; break; } }
            obj_end++;
        }

        /* Copy object to temp buffer */
        int obj_len = (int)(obj_end - obj_start);
        WCHAR *obj = (WCHAR*)malloc((obj_len+1)*sizeof(WCHAR));
        if (!obj) break;
        wcsncpy(obj, obj_start, obj_len);
        obj[obj_len] = 0;

        Mod *m = &g_state.mods[g_state.mod_count];
        memset(m, 0, sizeof(Mod));
        m->id      = json_get_int(obj, L"id");
        m->enabled = json_get_bool(obj, L"enabled");
        WCHAR cat_s[32] = {0};
        json_get_str(obj, L"category", cat_s, 32);
        m->category = cat_from_str(cat_s);
        json_get_str(obj, L"name",         m->name,         MAX_STR);
        json_get_str(obj, L"author",       m->author,       MAX_STR);
        json_get_str(obj, L"version",      m->version,      64);
        json_get_str(obj, L"notes",        m->notes,        MAX_STR);
        json_get_str(obj, L"library_path", m->library_path, MAX_PATH_LEN);
        json_get_str(obj, L"added",        m->added,        64);

        if (m->name[0]) g_state.mod_count++;
        free(obj);
        p = obj_end;
    }
    free(buf);
}

/* ══════════════════════════════════════════════════════════════════════════
   File utilities
   ══════════════════════════════════════════════════════════════════════════ */

static void ensure_dir(const WCHAR *path) {
    SHCreateDirectoryExW(NULL, path, NULL);
}

static void sanitize_filename(const WCHAR *in, WCHAR *out, int out_len) {
    int j = 0;
    for (int i = 0; in[i] && j < out_len-1; i++) {
        WCHAR c = in[i];
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
            (c >= L'0' && c <= L'9') || c == L'-' || c == L'.') {
            out[j++] = c;
        } else {
            out[j++] = L'_';
        }
    }
    out[j] = 0;
}

static BOOL copy_file_to_library(const WCHAR *src_path, int mod_id,
                                  const WCHAR *mod_name, WCHAR *out_path) {
    WCHAR safe_name[MAX_STR];
    sanitize_filename(mod_name, safe_name, MAX_STR);
    swprintf(out_path, MAX_PATH_LEN, L"%s\\mod_%04d_%s.pak",
             g_state.library_dir, mod_id, safe_name);
    return CopyFileW(src_path, out_path, FALSE);
}

static void get_today(WCHAR *out, int len) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(out, len, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
}

/* ══════════════════════════════════════════════════════════════════════════
   Deploy / Undeploy
   ══════════════════════════════════════════════════════════════════════════ */

static void get_mods_folder(WCHAR *out, int len) {
    swprintf(out, len, L"%s\\ProjectWingman\\Content\\Paks\\~mods",
             g_state.game_path);
}

void DeployMods(void) {
    if (!g_state.game_path[0]) {
        MessageBoxW(g_hwnd,
            L"Game path is not set.\nGo to Settings and set your Project Wingman folder.",
            L"Deploy Failed", MB_ICONERROR);
        return;
    }

    WCHAR mods_folder[MAX_PATH_LEN];
    get_mods_folder(mods_folder, MAX_PATH_LEN);
    ensure_dir(mods_folder);

    /* Remove previously deployed files */
    FILE *dt = _wfopen(g_state.deployed_path, L"r, ccs=UTF-8");
    if (dt) {
        fseek(dt, 0, SEEK_END);
        long sz = ftell(dt); rewind(dt);
        if (sz > 0 && sz < 512*1024) {
            WCHAR *dbuf = (WCHAR*)malloc((sz+2)*sizeof(WCHAR));
            if (dbuf) {
                int n = (int)fread(dbuf, sizeof(WCHAR), sz/sizeof(WCHAR), dt);
                dbuf[n] = 0;
                /* Each deployed path on its own line after "files":[ */
                const WCHAR *fp = wcsstr(dbuf, L"[");
                if (fp) {
                    fp++;
                    while (*fp) {
                        while (*fp && *fp != L'"' && *fp != L']') fp++;
                        if (!*fp || *fp == L']') break;
                        fp++;
                        WCHAR fpath[MAX_PATH_LEN]; int fi = 0;
                        while (*fp && *fp != L'"' && fi < MAX_PATH_LEN-1)
                            fpath[fi++] = *fp++;
                        fpath[fi] = 0;
                        if (fpath[0]) DeleteFileW(fpath);
                        if (*fp == L'"') fp++;
                    }
                }
                free(dbuf);
            }
        }
        fclose(dt);
    }

    /* Deploy enabled mods in order */
    int deployed = 0, skipped = 0;
    WCHAR deployed_paths[MAX_MODS][MAX_PATH_LEN];

    for (int i = 0; i < g_state.mod_count; i++) {
        Mod *m = &g_state.mods[i];
        if (!m->enabled) continue;

        if (!PathFileExistsW(m->library_path)) { skipped++; continue; }

        WCHAR safe[MAX_STR];
        sanitize_filename(m->name, safe, MAX_STR);

        /* Ensure name ends in _P */
        WCHAR dest_name[MAX_PATH_LEN];
        swprintf(dest_name, MAX_PATH_LEN, L"%s\\%02d_%s_P.pak",
                 mods_folder, deployed, safe);

        if (CopyFileW(m->library_path, dest_name, FALSE)) {
            wcscpy(deployed_paths[deployed], dest_name);
            deployed++;
        } else { skipped++; }
    }

    /* Write deployed tracker */
    FILE *df = _wfopen(g_state.deployed_path, L"w, ccs=UTF-8");
    if (df) {
        fwprintf(df, L"{ \"files\": [");
        for (int i = 0; i < deployed; i++) {
            fwprintf(df, L"\n  ");
            json_write_str(df, deployed_paths[i]);
            if (i < deployed-1) fwprintf(df, L",");
        }
        fwprintf(df, L"\n] }\n");
        fclose(df);
    }

    WCHAR msg[512];
    if (skipped > 0)
        swprintf(msg, 512, L"Deployed %d mod(s) to ~mods\\\n(%d skipped — files missing).", deployed, skipped);
    else
        swprintf(msg, 512, L"Successfully deployed %d mod(s) to:\n%s", deployed, mods_folder);

    SetStatus(L"Mods deployed! Launch the game now.", COL_GREEN);
    MessageBoxW(g_hwnd, msg, L"Deploy Complete", MB_ICONINFORMATION);
}

void UndeployMods(void) {
    if (MessageBoxW(g_hwnd,
            L"Remove all deployed mod files from the ~mods folder?\n"
            L"Your mod list will be kept — you can re-deploy anytime.",
            L"Undeploy All", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    int removed = 0;
    FILE *dt = _wfopen(g_state.deployed_path, L"r, ccs=UTF-8");
    if (dt) {
        fseek(dt, 0, SEEK_END);
        long sz = ftell(dt); rewind(dt);
        if (sz > 0 && sz < 512*1024) {
            WCHAR *dbuf = (WCHAR*)malloc((sz+2)*sizeof(WCHAR));
            if (dbuf) {
                int n = (int)fread(dbuf, sizeof(WCHAR), sz/sizeof(WCHAR), dt);
                dbuf[n] = 0;
                const WCHAR *fp = wcsstr(dbuf, L"[");
                if (fp) {
                    fp++;
                    while (*fp) {
                        while (*fp && *fp != L'"' && *fp != L']') fp++;
                        if (!*fp || *fp == L']') break;
                        fp++;
                        WCHAR fpath[MAX_PATH_LEN]; int fi = 0;
                        while (*fp && *fp != L'"' && fi < MAX_PATH_LEN-1)
                            fpath[fi++] = *fp++;
                        fpath[fi] = 0;
                        if (fpath[0] && DeleteFileW(fpath)) removed++;
                        if (*fp == L'"') fp++;
                    }
                }
                free(dbuf);
            }
        }
        fclose(dt);
        DeleteFileW(g_state.deployed_path);
    }
    WCHAR msg[256];
    swprintf(msg, 256, L"Removed %d deployed mod file(s).", removed);
    SetStatus(msg, COL_AMBER);
    MessageBoxW(g_hwnd, msg, L"Undeploy Complete", MB_ICONINFORMATION);
}

/* ══════════════════════════════════════════════════════════════════════════
   Add Mod Dialog
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    WCHAR name[MAX_STR];
    WCHAR author[MAX_STR];
    WCHAR version[64];
    WCHAR notes[MAX_STR];
    WCHAR pak_path[MAX_PATH_LEN];
    int   category;
    BOOL  enabled;
    BOOL  is_edit;
    int   edit_id;
} DialogData;

static DialogData g_dlg;

#define DLG_NAME     2001
#define DLG_AUTHOR   2002
#define DLG_VERSION  2003
#define DLG_NOTES    2004
#define DLG_CAT      2005
#define DLG_ENABLED  2006
#define DLG_PAKPATH  2007
#define DLG_BROWSE   2008
#define DLG_OK       2009
#define DLG_CANCEL   2010

static void dlg_set_fonts(HWND hwnd, HFONT font) {
    /* Apply font to all children */
    HWND child = GetWindow(hwnd, GW_CHILD);
    while (child) {
        SendMessage(child, WM_SETFONT, (WPARAM)font, TRUE);
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

LRESULT CALLBACK AddModDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&g_dlg);

        WCHAR title[64];
        swprintf(title, 64, L"%s — %s", APP_NAME,
                 g_dlg.is_edit ? L"Edit Mod" : L"Add Mod");
        SetWindowTextW(hwnd, title);

        int x = 14, y = 14, lw = 110, ew = 330, h = 24, gap = 32;

        /* Helper lambdas as macros */
        #define LBL(t,Y)  CreateWindowW(L"STATIC",t,WS_CHILD|WS_VISIBLE|SS_LEFT,\
                              x,Y+4,lw,20,hwnd,NULL,NULL,NULL)
        #define EDT(id,Y,ro) CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",\
                              WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|(ro?ES_READONLY:0),\
                              x+lw+6,Y,ew,h,hwnd,(HMENU)(UINT_PTR)(id),NULL,NULL)

        LBL(L"MOD NAME *",    y);
        HWND eName = EDT(DLG_NAME, y, 0);
        SetWindowTextW(eName, g_dlg.name); y += gap;

        LBL(L"AUTHOR",       y);
        HWND eAuthor = EDT(DLG_AUTHOR, y, 0);
        SetWindowTextW(eAuthor, g_dlg.author); y += gap;

        LBL(L"VERSION",      y);
        HWND eVer = EDT(DLG_VERSION, y, 0);
        SetWindowTextW(eVer, g_dlg.version); y += gap;

        LBL(L"NOTES",        y);
        HWND eNotes = EDT(DLG_NOTES, y, 0);
        SetWindowTextW(eNotes, g_dlg.notes); y += gap;

        LBL(L"CATEGORY",     y);
        HWND cb = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            x+lw+6, y, 180, 200, hwnd, (HMENU)(UINT_PTR)DLG_CAT, NULL, NULL);
        const WCHAR *cats[] = {L"Gameplay",L"Visual / Skins",L"Audio / Sound",
                                L"UI / HUD",L"Map / Mission",L"Other"};
        for (int i=0;i<6;i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)cats[i]);
        SendMessageW(cb, CB_SETCURSEL, g_dlg.category, 0);
        y += gap;

        LBL(L"START AS",     y);
        HWND cb2 = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            x+lw+6, y, 180, 100, hwnd, (HMENU)(UINT_PTR)DLG_ENABLED, NULL, NULL);
        SendMessageW(cb2, CB_ADDSTRING, 0, (LPARAM)L"Active (ON)");
        SendMessageW(cb2, CB_ADDSTRING, 0, (LPARAM)L"Inactive (OFF)");
        SendMessageW(cb2, CB_SETCURSEL, g_dlg.enabled ? 0 : 1, 0);
        y += gap;

        if (!g_dlg.is_edit) {
            LBL(L"PAK / ZIP FILE *", y);
            HWND ePak = EDT(DLG_PAKPATH, y, 1);
            SetWindowTextW(ePak, g_dlg.pak_path);
            CreateWindowW(L"BUTTON", L"Browse...",
                WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                x+lw+6+ew+6, y, 80, h, hwnd, (HMENU)(UINT_PTR)DLG_BROWSE, NULL, NULL);
            y += gap;
        }

        y += 8;
        CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            x+lw+6+ew-80-86, y, 80, 28, hwnd, (HMENU)(UINT_PTR)DLG_CANCEL, NULL, NULL);
        CreateWindowW(L"BUTTON", g_dlg.is_edit ? L"Save Changes" : L"Add Mod",
            WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
            x+lw+6+ew-80, y, 86, 28, hwnd, (HMENU)(UINT_PTR)DLG_OK, NULL, NULL);

        dlg_set_fonts(hwnd, g_font_sans);

        /* Resize window to content */
        RECT r = {0, 0, x+lw+6+ew+100, y+50};
        AdjustWindowRect(&r, WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, FALSE);
        SetWindowPos(hwnd, NULL, 0, 0, r.right-r.left, r.bottom-r.top,
                     SWP_NOMOVE|SWP_NOZORDER);

        /* Centre on parent */
        RECT pr; GetWindowRect(g_hwnd, &pr);
        RECT wr; GetWindowRect(hwnd, &wr);
        int ww = wr.right-wr.left, wh = wr.bottom-wr.top;
        SetWindowPos(hwnd, NULL,
            pr.left + (pr.right-pr.left-ww)/2,
            pr.top  + (pr.bottom-pr.top-wh)/2,
            0, 0, SWP_NOSIZE|SWP_NOZORDER);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case DLG_BROWSE: {
            OPENFILENAMEW ofn = {0};
            WCHAR buf[MAX_PATH_LEN] = {0};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hwnd;
            ofn.lpstrFilter = L"Mod files (*.pak, *.zip)\0*.pak;*.zip\0PAK files\0*.pak\0ZIP files\0*.zip\0All\0*.*\0";
            ofn.lpstrFile   = buf;
            ofn.nMaxFile    = MAX_PATH_LEN;
            ofn.Flags       = OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                SetDlgItemTextW(hwnd, DLG_PAKPATH, buf);
                /* Auto-fill name from filename */
                WCHAR auto_name[MAX_STR];
                GetDlgItemTextW(hwnd, DLG_NAME, auto_name, MAX_STR);
                if (!auto_name[0]) {
                    WCHAR stem[MAX_STR];
                    wcscpy(stem, PathFindFileNameW(buf));
                    /* Strip extension and _P suffix */
                    WCHAR *dot = wcsrchr(stem, L'.');
                    if (dot) *dot = 0;
                    int slen = (int)wcslen(stem);
                    if (slen >= 2 && (stem[slen-2]==L'_') &&
                        (stem[slen-1]==L'P'||stem[slen-1]==L'p'))
                        stem[slen-2] = 0;
                    /* Replace underscores with spaces */
                    for (WCHAR *c=stem; *c; c++) if (*c==L'_') *c=L' ';
                    SetDlgItemTextW(hwnd, DLG_NAME, stem);
                }
            }
            return 0;
        }
        case DLG_OK: {
            GetDlgItemTextW(hwnd, DLG_NAME,    g_dlg.name,    MAX_STR);
            GetDlgItemTextW(hwnd, DLG_AUTHOR,  g_dlg.author,  MAX_STR);
            GetDlgItemTextW(hwnd, DLG_VERSION, g_dlg.version, 64);
            GetDlgItemTextW(hwnd, DLG_NOTES,   g_dlg.notes,   MAX_STR);
            if (!g_dlg.is_edit)
                GetDlgItemTextW(hwnd, DLG_PAKPATH, g_dlg.pak_path, MAX_PATH_LEN);
            g_dlg.category = (int)SendDlgItemMessageW(hwnd, DLG_CAT, CB_GETCURSEL, 0, 0);
            g_dlg.enabled  = (SendDlgItemMessageW(hwnd, DLG_ENABLED, CB_GETCURSEL, 0, 0) == 0);

            if (!g_dlg.name[0]) {
                MessageBoxW(hwnd, L"Please enter a mod name.", L"Required", MB_ICONWARNING);
                SetFocus(GetDlgItem(hwnd, DLG_NAME));
                return 0;
            }
            if (!g_dlg.is_edit && !g_dlg.pak_path[0]) {
                MessageBoxW(hwnd, L"Please select a .pak or .zip file.", L"Required", MB_ICONWARNING);
                return 0;
            }
            EndDialog(hwnd, IDOK);
            return 0;
        }
        case DLG_CANCEL:
            EndDialog(hwnd, IDCANCEL);
            return 0;
        }
        break;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, COL_FG);
        SetBkColor(hdc, COL_BG2);
        return (LRESULT)g_br_bg2;
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, COL_BG);
        return (LRESULT)g_br_bg;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int ShowAddModDialog(HWND parent, BOOL is_edit, int edit_id) {
    memset(&g_dlg, 0, sizeof(g_dlg));
    g_dlg.is_edit = is_edit;
    g_dlg.edit_id = edit_id;
    g_dlg.enabled = TRUE;
    wcscpy(g_dlg.version, L"1.0.0");
    wcscpy(g_dlg.author,  L"Unknown");

    if (is_edit) {
        for (int i = 0; i < g_state.mod_count; i++) {
            if (g_state.mods[i].id == edit_id) {
                Mod *m = &g_state.mods[i];
                wcscpy(g_dlg.name,    m->name);
                wcscpy(g_dlg.author,  m->author);
                wcscpy(g_dlg.version, m->version);
                wcscpy(g_dlg.notes,   m->notes);
                g_dlg.category = m->category;
                g_dlg.enabled  = m->enabled;
                break;
            }
        }
    }

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = AddModDlgProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"MonarchAddDlg";
    wc.hbrBackground = g_br_bg;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc); /* ok if fails (already registered) */

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_TOPMOST,
        L"MonarchAddDlg", APP_NAME,
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        100, 100, 500, 400, parent, NULL, GetModuleHandleW(NULL), NULL);
    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    MSG m;
    while (IsWindow(dlg) && GetMessageW(&m, NULL, 0, 0)) {
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return g_dlg.name[0] ? IDOK : IDCANCEL;
}

/* ══════════════════════════════════════════════════════════════════════════
   Settings dialog (inline)
   ══════════════════════════════════════════════════════════════════════════ */

static HWND g_settings_path_edit;

LRESULT CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        int x=14, y=14;
        CreateWindowW(L"STATIC",
            L"PROJECT WINGMAN INSTALL FOLDER",
            WS_CHILD|WS_VISIBLE|SS_LEFT, x, y, 440, 20, hwnd, NULL, NULL, NULL);
        y += 22;
        CreateWindowW(L"STATIC",
            L"e.g.  C:\\SteamLibrary\\steamapps\\common\\Project Wingman",
            WS_CHILD|WS_VISIBLE|SS_LEFT, x, y, 440, 18, hwnd, NULL, NULL, NULL);
        y += 26;
        g_settings_path_edit = CreateWindowExW(WS_EX_CLIENTEDGE,
            L"EDIT", g_state.game_path,
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            x, y, 370, 24, hwnd, (HMENU)(UINT_PTR)3001, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Browse...",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            x+376, y, 80, 24, hwnd, (HMENU)(UINT_PTR)3002, NULL, NULL);
        y += 36;
        CreateWindowW(L"BUTTON", L"Validate Path",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            x, y, 120, 24, hwnd, (HMENU)(UINT_PTR)3003, NULL, NULL);
        y += 44;
        CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            x+350, y, 80, 28, hwnd, (HMENU)(UINT_PTR)3004, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Save",
            WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
            x+440, y, 80, 28, hwnd, (HMENU)(UINT_PTR)3005, NULL, NULL);
        dlg_set_fonts(hwnd, g_font_sans);
        RECT r = {0,0,544,y+56};
        AdjustWindowRect(&r, WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, FALSE);
        SetWindowPos(hwnd,NULL,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER);
        RECT pr; GetWindowRect(g_hwnd,&pr);
        RECT wr; GetWindowRect(hwnd,&wr);
        SetWindowPos(hwnd,NULL,
            pr.left+(pr.right-pr.left-(wr.right-wr.left))/2,
            pr.top+(pr.bottom-pr.top-(wr.bottom-wr.top))/2,
            0,0,SWP_NOSIZE|SWP_NOZORDER);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 3002: { /* Browse */
            BROWSEINFOW bi = {0};
            bi.hwndOwner = hwnd;
            bi.lpszTitle = L"Select Project Wingman install folder";
            bi.ulFlags = BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pid = SHBrowseForFolderW(&bi);
            if (pid) {
                WCHAR path[MAX_PATH_LEN];
                if (SHGetPathFromIDListW(pid, path))
                    SetWindowTextW(g_settings_path_edit, path);
                CoTaskMemFree(pid);
            }
            return 0;
        }
        case 3003: { /* Validate */
            WCHAR path[MAX_PATH_LEN];
            GetWindowTextW(g_settings_path_edit, path, MAX_PATH_LEN);
            WCHAR exe[MAX_PATH_LEN];
            swprintf(exe, MAX_PATH_LEN, L"%s\\ProjectWingman.exe", path);
            if (PathFileExistsW(exe)) {
                WCHAR mods[MAX_PATH_LEN];
                swprintf(mods, MAX_PATH_LEN,
                    L"Game found!\n\nMods will be deployed to:\n%s\\ProjectWingman\\Content\\Paks\\~mods", path);
                MessageBoxW(hwnd, mods, L"Valid Path", MB_ICONINFORMATION);
            } else {
                MessageBoxW(hwnd,
                    L"ProjectWingman.exe not found in that folder.\n\n"
                    L"Select the root folder, e.g.:\n"
                    L"C:\\SteamLibrary\\steamapps\\common\\Project Wingman",
                    L"Path Not Found", MB_ICONWARNING);
            }
            return 0;
        }
        case 3004: EndDialog(hwnd, IDCANCEL); return 0;
        case 3005: {
            GetWindowTextW(g_settings_path_edit, g_state.game_path, MAX_PATH_LEN);
            SaveProfile();
            EndDialog(hwnd, IDOK);
            SetStatus(L"Settings saved.", COL_GREEN);
            RefreshList();
            return 0;
        }
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc=(HDC)wp; SetTextColor(hdc,COL_FG); SetBkColor(hdc,COL_BG2);
        return (LRESULT)g_br_bg2;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN: {
        HDC hdc=(HDC)wp; SetBkColor(hdc,COL_BG); return (LRESULT)g_br_bg;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowSettings(void) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = SettingsDlgProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"MonarchSettings";
    wc.hbrBackground = g_br_bg;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_TOPMOST,
        L"MonarchSettings", L"Settings — Monarch ModKit",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        100,100,400,200,g_hwnd,NULL,GetModuleHandleW(NULL),NULL);
    EnableWindow(g_hwnd, FALSE);
    ShowWindow(dlg, SW_SHOW);
    MSG m;
    while (IsWindow(dlg) && GetMessageW(&m,NULL,0,0)) {
        if (!IsDialogMessageW(dlg,&m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);
}

/* ══════════════════════════════════════════════════════════════════════════
   ListView helpers
   ══════════════════════════════════════════════════════════════════════════ */

static void RefreshList(void) {
    ListView_DeleteAllItems(g_list);

    WCHAR search_buf[MAX_STR] = {0};
    GetWindowTextW(g_search, search_buf, MAX_STR);
    CharLowerW(search_buf);

    const WCHAR *cat_labels[] = {L"GAMEPLAY",L"VISUAL",L"AUDIO",L"UI/HUD",L"MAP/MISSION",L"OTHER"};
    int row = 0;
    int total = 0, active = 0;

    for (int i = 0; i < g_state.mod_count; i++) {
        Mod *m = &g_state.mods[i];
        total++;
        if (m->enabled) active++;

        /* Filter */
        if (g_filter == 1 && !m->enabled) continue;
        if (g_filter == 2 &&  m->enabled) continue;
        if (g_filter >= 3 && m->category != g_filter-3) continue;

        /* Search */
        if (search_buf[0]) {
            WCHAR hay[MAX_STR*3];
            swprintf(hay, MAX_STR*3, L"%s %s %s", m->name, m->author, m->notes);
            CharLowerW(hay);
            if (!wcsstr(hay, search_buf)) continue;
        }

        LVITEMW lvi = {0};
        lvi.mask     = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem    = row;
        lvi.iSubItem = 0;
        lvi.lParam   = (LPARAM)m->id;

        /* Col 0: order */
        WCHAR ord[8]; swprintf(ord, 8, L"%02d", i+1);
        lvi.pszText = ord;
        ListView_InsertItem(g_list, &lvi);

        /* Col 1: status */
        ListView_SetItemText(g_list, row, 1, m->enabled ? L"ON" : L"OFF");

        /* Col 2: name */
        ListView_SetItemText(g_list, row, 2, m->name);

        /* Col 3: category */
        ListView_SetItemText(g_list, row, 3, (WCHAR*)cat_labels[m->category]);

        /* Col 4: author */
        ListView_SetItemText(g_list, row, 4, m->author);

        /* Col 5: version */
        ListView_SetItemText(g_list, row, 5, m->version);

        /* Col 6: notes */
        ListView_SetItemText(g_list, row, 6, m->notes);

        row++;
    }

    /* Status bar */
    WCHAR sb[256];
    int inactive = total - active;
    swprintf(sb, 256, L"  Total: %d   Active: %d   Inactive: %d   |   "
             L"Data: %s",
             total, active, inactive, g_state.data_dir);
    SetWindowTextW(g_status, sb);

    /* Header count */
    WCHAR title[128];
    swprintf(title, 128, L"%s v%s  —  %d mod(s)  |  %d active",
             APP_NAME, APP_VERSION, total, active);
    SetWindowTextW(g_hwnd, title);
}

static void SetStatus(const WCHAR *msg, COLORREF col) {
    /* Prepend to statusbar */
    WCHAR cur[512], combined[768];
    GetWindowTextW(g_status, cur, 512);
    swprintf(combined, 768, L"  %s   |%s", msg, wcschr(cur, L'|') ? wcschr(cur, L'|') : L"");
    SetWindowTextW(g_status, combined);
    (void)col;
}

/* ══════════════════════════════════════════════════════════════════════════
   Main window proc
   ══════════════════════════════════════════════════════════════════════════ */

static void OnAddMod(void) {
    if (ShowAddModDialog(g_hwnd, FALSE, 0) != IDOK) return;

    int mod_id = g_state.next_id++;
    Mod *m = &g_state.mods[g_state.mod_count];
    memset(m, 0, sizeof(Mod));
    m->id       = mod_id;
    m->enabled  = g_dlg.enabled;
    m->category = (ModCategory)g_dlg.category;
    wcscpy(m->name,    g_dlg.name);
    wcscpy(m->author,  g_dlg.author[0] ? g_dlg.author : L"Unknown");
    wcscpy(m->version, g_dlg.version[0] ? g_dlg.version : L"1.0.0");
    wcscpy(m->notes,   g_dlg.notes);
    get_today(m->added, 64);

    /* Copy pak into library */
    WCHAR out_path[MAX_PATH_LEN];
    if (!copy_file_to_library(g_dlg.pak_path, mod_id, g_dlg.name, out_path)) {
        WCHAR err[512];
        swprintf(err, 512, L"Could not copy file to library:\n%s\n\n"
                 L"Make sure the file exists and you have permission to copy it.",
                 g_dlg.pak_path);
        MessageBoxW(g_hwnd, err, L"Import Error", MB_ICONERROR);
        return;
    }
    wcscpy(m->library_path, out_path);
    g_state.mod_count++;
    SaveProfile();
    RefreshList();
    SetStatus(L"Mod added. Click Deploy to apply.", COL_GREEN);
}

static void OnEditMod(int mod_id) {
    if (ShowAddModDialog(g_hwnd, TRUE, mod_id) != IDOK) return;
    for (int i = 0; i < g_state.mod_count; i++) {
        if (g_state.mods[i].id == mod_id) {
            Mod *m = &g_state.mods[i];
            wcscpy(m->name,    g_dlg.name);
            wcscpy(m->author,  g_dlg.author[0] ? g_dlg.author : L"Unknown");
            wcscpy(m->version, g_dlg.version[0] ? g_dlg.version : L"1.0.0");
            wcscpy(m->notes,   g_dlg.notes);
            m->category = (ModCategory)g_dlg.category;
            m->enabled  = g_dlg.enabled;
            break;
        }
    }
    SaveProfile(); RefreshList();
    SetStatus(L"Mod updated.", COL_GREEN);
}

static void OnToggleMod(int mod_id) {
    for (int i = 0; i < g_state.mod_count; i++) {
        if (g_state.mods[i].id == mod_id) {
            g_state.mods[i].enabled = !g_state.mods[i].enabled;
            break;
        }
    }
    SaveProfile(); RefreshList();
    SetStatus(L"Mod toggled. Remember to Deploy.", COL_AMBER);
}

static void OnDeleteMod(int mod_id) {
    WCHAR msg[256];
    Mod *found = NULL;
    for (int i=0;i<g_state.mod_count;i++) if (g_state.mods[i].id==mod_id) { found=&g_state.mods[i]; break; }
    if (!found) return;
    swprintf(msg,256,L"Remove \"%s\" from your mod list?",found->name);
    if (MessageBoxW(g_hwnd,msg,L"Remove Mod",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
    DeleteFileW(found->library_path);
    for (int i=0;i<g_state.mod_count;i++) {
        if (g_state.mods[i].id==mod_id) {
            memmove(&g_state.mods[i],&g_state.mods[i+1],(g_state.mod_count-i-1)*sizeof(Mod));
            g_state.mod_count--; break;
        }
    }
    SaveProfile(); RefreshList();
    SetStatus(L"Mod removed.", COL_AMBER);
}

static void OnMoveUp(int mod_id) {
    for (int i=1;i<g_state.mod_count;i++) {
        if (g_state.mods[i].id==mod_id) {
            Mod tmp=g_state.mods[i]; g_state.mods[i]=g_state.mods[i-1]; g_state.mods[i-1]=tmp;
            break;
        }
    }
    SaveProfile(); RefreshList();
}

static void OnMoveDown(int mod_id) {
    for (int i=0;i<g_state.mod_count-1;i++) {
        if (g_state.mods[i].id==mod_id) {
            Mod tmp=g_state.mods[i]; g_state.mods[i]=g_state.mods[i+1]; g_state.mods[i+1]=tmp;
            break;
        }
    }
    SaveProfile(); RefreshList();
}

static int GetSelectedModId(void) {
    int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (sel < 0) return -1;
    LVITEMW lvi = {0}; lvi.mask=LVIF_PARAM; lvi.iItem=sel;
    ListView_GetItem(g_list, &lvi);
    return (int)lvi.lParam;
}

/* Context menu IDs */
#define CM_TOGGLE   3100
#define CM_EDIT     3101
#define CM_MOVE_UP  3102
#define CM_MOVE_DN  3103
#define CM_DELETE   3104

static void ShowContextMenu(HWND hwnd, int x, int y) {
    int id = GetSelectedModId();
    if (id < 0) return;
    HMENU hm = CreatePopupMenu();
    AppendMenuW(hm, MF_STRING, CM_TOGGLE,  L"Toggle ON/OFF");
    AppendMenuW(hm, MF_STRING, CM_EDIT,    L"Edit Details...");
    AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hm, MF_STRING, CM_MOVE_UP, L"Move Up (lower priority)");
    AppendMenuW(hm, MF_STRING, CM_MOVE_DN, L"Move Down (higher priority)");
    AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hm, MF_STRING, CM_DELETE,  L"Remove Mod...");
    int cmd = TrackPopupMenu(hm, TPM_RETURNCMD|TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);
    DestroyMenu(hm);
    switch(cmd) {
        case CM_TOGGLE:  OnToggleMod(id);  break;
        case CM_EDIT:    OnEditMod(id);    break;
        case CM_MOVE_UP: OnMoveUp(id);     break;
        case CM_MOVE_DN: OnMoveDown(id);   break;
        case CM_DELETE:  OnDeleteMod(id);  break;
    }
}

/* ListView custom draw — colour rows by state */
static LRESULT OnCustomDraw(LPNMLVCUSTOMDRAW cd) {
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        int id = (int)cd->nmcd.lItemlParam;
        Mod *m = NULL;
        for (int i=0;i<g_state.mod_count;i++) if (g_state.mods[i].id==id) { m=&g_state.mods[i]; break; }
        if (!m) return CDRF_DODEFAULT;
        if (m->enabled) {
            cd->clrText   = COL_FG;
            cd->clrTextBk = COL_BG3;
        } else {
            cd->clrText   = COL_FG_DIM;
            cd->clrTextBk = COL_BG2;
        }
        return CDRF_NEWFONT;
    }
    }
    return CDRF_DODEFAULT;
}

/* ── Auto-detect Steam ───────────────────────────────────────────────────── */
static void AutoDetectGame(void) {
    if (g_state.game_path[0]) return;
    const WCHAR *guesses[] = {
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Project Wingman",
        L"C:\\Program Files\\Steam\\steamapps\\common\\Project Wingman",
        L"D:\\SteamLibrary\\steamapps\\common\\Project Wingman",
        L"E:\\SteamLibrary\\steamapps\\common\\Project Wingman",
        L"D:\\Steam\\steamapps\\common\\Project Wingman",
        NULL
    };
    for (int i=0; guesses[i]; i++) {
        WCHAR exe[MAX_PATH_LEN];
        swprintf(exe, MAX_PATH_LEN, L"%s\\ProjectWingman.exe", guesses[i]);
        if (PathFileExistsW(exe)) {
            wcscpy(g_state.game_path, guesses[i]);
            SaveProfile();
            SetStatus(L"Game auto-detected!", COL_GREEN);
            return;
        }
    }
}

/* ── Main window ─────────────────────────────────────────────────────────── */
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        g_hwnd = hwnd;
        g_br_bg  = CreateSolidBrush(COL_BG);
        g_br_bg2 = CreateSolidBrush(COL_BG2);
        g_br_bg3 = CreateSolidBrush(COL_BG3);

        /* Fonts */
        g_font_mono  = CreateFontW(-13,0,0,0,FW_NORMAL,0,0,0,ANSI_CHARSET,
                           OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                           FIXED_PITCH|FF_MODERN, L"Courier New");
        g_font_sans  = CreateFontW(-13,0,0,0,FW_NORMAL,0,0,0,ANSI_CHARSET,
                           OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                           DEFAULT_PITCH|FF_SWISS, L"Segoe UI");
        g_font_title = CreateFontW(-18,0,0,0,FW_BOLD,0,0,0,ANSI_CHARSET,
                           OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                           DEFAULT_PITCH|FF_SWISS, L"Segoe UI");

        /* Toolbar buttons */
        int bx = 8, by = 8, bh = 28;
        struct { const WCHAR *t; int id; } btns[] = {
            {L"+ ADD MOD",    ID_BTN_ADD},
            {L"DEPLOY MODS",  ID_BTN_DEPLOY},
            {L"UNDEPLOY ALL", ID_BTN_UNDEPLOY},
            {L"SETTINGS",     ID_BTN_SETTINGS},
            {L"LAUNCH GAME",  ID_BTN_LAUNCH},
            {L"ALL ON",       ID_BTN_ALL_ON},
            {L"ALL OFF",      ID_BTN_ALL_OFF},
            {NULL, 0}
        };
        for (int i=0; btns[i].t; i++) {
            int bw = (int)wcslen(btns[i].t)*9 + 20;
            HWND b = CreateWindowW(L"BUTTON", btns[i].t,
                WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                bx, by, bw, bh, hwnd, (HMENU)(UINT_PTR)btns[i].id, NULL, NULL);
            SendMessage(b, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
            bx += bw + 6;
        }

        /* Filter buttons */
        struct { const WCHAR *t; int id; } filts[] = {
            {L"ALL",      ID_FILTER_ALL},
            {L"ACTIVE",   ID_FILTER_ON},
            {L"INACTIVE", ID_FILTER_OFF},
            {L"GAMEPLAY", ID_FILTER_GAMEPLAY},
            {L"VISUAL",   ID_FILTER_VISUAL},
            {L"AUDIO",    ID_FILTER_AUDIO},
            {L"UI/HUD",   ID_FILTER_UI},
            {L"MAP",      ID_FILTER_MAP},
            {NULL, 0}
        };
        bx = 8; by = 44;
        for (int i=0; filts[i].t; i++) {
            int bw = (int)wcslen(filts[i].t)*8 + 20;
            HWND b = CreateWindowW(L"BUTTON", filts[i].t,
                WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                bx, by, bw, 22, hwnd, (HMENU)(UINT_PTR)filts[i].id, NULL, NULL);
            SendMessage(b, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
            bx += bw + 4;
        }

        /* Search box */
        CreateWindowW(L"STATIC", L"Search:", WS_CHILD|WS_VISIBLE|SS_LEFT,
            bx+6, by+3, 56, 18, hwnd, NULL, NULL, NULL);
        g_search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            bx+66, by, 200, 22, hwnd, (HMENU)(UINT_PTR)ID_SEARCH, NULL, NULL);
        SendMessage(g_search, WM_SETFONT, (WPARAM)g_font_mono, TRUE);

        /* ListView */
        RECT rc; GetClientRect(hwnd, &rc);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SHOWSELALWAYS,
            0, 76, rc.right, rc.bottom-100, hwnd,
            (HMENU)(UINT_PTR)ID_LIST, NULL, NULL);
        SendMessage(g_list, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
        SendMessage(g_list, LVM_SETBKCOLOR,   0, (LPARAM)COL_BG2);
        SendMessage(g_list, LVM_SETTEXTCOLOR,  0, (LPARAM)COL_FG);
        SendMessage(g_list, LVM_SETTEXTBKCOLOR,0, (LPARAM)COL_BG2);
        ListView_SetExtendedListViewStyle(g_list,
            LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);

        /* Columns */
        struct { const WCHAR *h; int w; } cols[] = {
            {L"#",        36}, {L"Status",  60}, {L"Mod Name",     280},
            {L"Category", 100},{L"Author",  120}, {L"Version", 70},
            {L"Notes",    200}, {NULL, 0}
        };
        for (int i=0; cols[i].h; i++) {
            LVCOLUMNW lvc = {0};
            lvc.mask    = LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
            lvc.cx      = cols[i].w;
            lvc.pszText = (WCHAR*)cols[i].h;
            lvc.iSubItem = i;
            ListView_InsertColumn(g_list, i, &lvc);
        }

        /* Status bar */
        g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
            WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_STATUS_BAR, NULL, NULL);
        SendMessage(g_status, WM_SETFONT, (WPARAM)g_font_mono, TRUE);

        AutoDetectGame();
        LoadProfile();
        RefreshList();
        return 0;
    }

    case WM_SIZE: {
        if (!g_list || !g_status) return 0;
        int w = LOWORD(lp), h = HIWORD(lp);
        SetWindowPos(g_list,   NULL, 0, 76,    w, h-76-22, SWP_NOZORDER);
        SetWindowPos(g_status, NULL, 0, h-22,  w, 22,      SWP_NOZORDER);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case ID_BTN_ADD:      OnAddMod(); break;
        case ID_BTN_DEPLOY:   DeployMods(); break;
        case ID_BTN_UNDEPLOY: UndeployMods(); break;
        case ID_BTN_SETTINGS: ShowSettings(); break;
        case ID_BTN_LAUNCH: {
            if (!g_state.game_path[0]) {
                MessageBoxW(hwnd, L"Set game path in Settings first.", L"No Path", MB_ICONWARNING);
                break;
            }
            WCHAR exe[MAX_PATH_LEN];
            swprintf(exe, MAX_PATH_LEN, L"%s\\ProjectWingman.exe", g_state.game_path);
            if (PathFileExistsW(exe))
                ShellExecuteW(hwnd, L"open", exe, NULL, g_state.game_path, SW_SHOWNORMAL);
            else
                ShellExecuteW(hwnd, L"open", L"steam://rungameid/895870", NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        case ID_BTN_ALL_ON:
            for (int i=0;i<g_state.mod_count;i++) g_state.mods[i].enabled=TRUE;
            SaveProfile(); RefreshList(); SetStatus(L"All mods activated.", COL_GREEN); break;
        case ID_BTN_ALL_OFF:
            for (int i=0;i<g_state.mod_count;i++) g_state.mods[i].enabled=FALSE;
            SaveProfile(); RefreshList(); SetStatus(L"All mods deactivated.", COL_AMBER); break;
        case ID_FILTER_ALL:      g_filter=0; RefreshList(); break;
        case ID_FILTER_ON:       g_filter=1; RefreshList(); break;
        case ID_FILTER_OFF:      g_filter=2; RefreshList(); break;
        case ID_FILTER_GAMEPLAY: g_filter=3; RefreshList(); break;
        case ID_FILTER_VISUAL:   g_filter=4; RefreshList(); break;
        case ID_FILTER_AUDIO:    g_filter=5; RefreshList(); break;
        case ID_FILTER_UI:       g_filter=6; RefreshList(); break;
        case ID_FILTER_MAP:      g_filter=7; RefreshList(); break;
        case ID_SEARCH:
            if (HIWORD(wp)==EN_CHANGE) RefreshList();
            break;
        }
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR *hdr = (NMHDR*)lp;
        if (hdr->idFrom == ID_LIST) {
            switch (hdr->code) {
            case NM_DBLCLK: {
                int id = GetSelectedModId();
                if (id >= 0) OnEditMod(id);
                return 0;
            }
            case NM_RCLICK: {
                POINT pt; GetCursorPos(&pt);
                ShowContextMenu(hwnd, pt.x, pt.y);
                return 0;
            }
            case NM_CUSTOMDRAW:
                return OnCustomDraw((LPNMLVCUSTOMDRAW)lp);
            }
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc=(HDC)wp; SetTextColor(hdc,COL_FG_DIM); SetBkColor(hdc,COL_BG);
        return (LRESULT)g_br_bg;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc=(HDC)wp; SetTextColor(hdc,COL_FG); SetBkColor(hdc,COL_BG2);
        return (LRESULT)g_br_bg2;
    }
    case WM_CTLCOLORBTN: {
        HDC hdc=(HDC)wp; SetBkColor(hdc,COL_BG); return (LRESULT)g_br_bg;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_br_bg);
        return 1;
    }

    case WM_DESTROY:
        SaveProfile();
        DeleteObject(g_font_mono); DeleteObject(g_font_sans); DeleteObject(g_font_title);
        DeleteObject(g_br_bg); DeleteObject(g_br_bg2); DeleteObject(g_br_bg3);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ══════════════════════════════════════════════════════════════════════════
   WinMain
   ══════════════════════════════════════════════════════════════════════════ */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd;

    InitCommonControls();
    CoInitialize(NULL);

    /* Set up data dirs */
    WCHAR appdata[MAX_PATH_LEN];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata);
    swprintf(g_state.data_dir,    MAX_PATH_LEN, L"%s\\MonarchModKit",          appdata);
    swprintf(g_state.library_dir, MAX_PATH_LEN, L"%s\\MonarchModKit\\library", appdata);
    swprintf(g_state.profile_path,MAX_PATH_LEN, L"%s\\MonarchModKit\\profile.json", appdata);
    swprintf(g_state.deployed_path,MAX_PATH_LEN,L"%s\\MonarchModKit\\deployed.json",appdata);
    ensure_dir(g_state.data_dir);
    ensure_dir(g_state.library_dir);

    /* Register window class */
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"MonarchModKit";
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"MonarchModKit",
        APP_NAME L" v" APP_VERSION L" — Project Wingman Mod Manager",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1080, 640,
        NULL, NULL, hInst, NULL);

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    CoUninitialize();
    return (int)m.wParam;
}
