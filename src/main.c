#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "mft.h"
#include "search.h"
#include "journal.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "ole32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2
#endif

#define WIN_W            1280
#define WIN_H            780

#define HEADER_H         64
#define FOOTER_H         32
#define LEFT_W           280
#define RIGHT_W          280
#define HPAD             16
#define ROW_H            28

#define COL_BG           RGB(18, 16, 26)
#define COL_PANEL        RGB(28, 24, 40)
#define COL_INPUT        RGB(24, 22, 36)
#define COL_BG_HOVER     RGB(45, 40, 65)
#define COL_BG_SEL       RGB(80, 60, 130)
#define COL_TEXT         RGB(232, 230, 245)
#define COL_TEXT_SEL     RGB(255, 255, 255)
#define COL_TEXT_DIM     RGB(155, 150, 175)
#define COL_TEXT_HEAD    RGB(195, 190, 215)
#define COL_DIVIDER      RGB(50, 45, 70)
#define COL_ACCENT       RGB(170, 130, 255)
#define COL_ACCENT_DIM   RGB(100, 80, 160)

#define ID_GLOBAL_EDIT 1001
#define ID_REFINE_EDIT 1002
#define ID_RESULTS     1003
#define ID_VOLUMES     1004
#define ID_BTN_MANAGE  1010
#define ID_BTN_SETTINGS 1011

#define ID_FT_PDF      1100
#define ID_FT_DOCX     1101
#define ID_FT_ZIP      1102
#define ID_FT_IMG      1103
#define ID_FT_VIDEO    1104

#define ID_DATE_RANGE  1110
#define ID_SIZE_INPUT  1120
#define ID_TAGS_INPUT  1130

#define HOTKEY_SUMMON  1

#define WM_TRAY         (WM_APP + 1)
#define WM_INDEX_DONE   (WM_APP + 2)
#define WM_INDEX_DIRTY  (WM_APP + 3)
#define WM_SEARCH_DONE  (WM_APP + 4)

#define IDM_TRAY_TOGGLE 2001
#define IDM_TRAY_QUIT   2002

#define IDM_CTX_OPEN     2100
#define IDM_CTX_COPYPATH 2101
#define IDM_CTX_SHOW     2102

#define IDM_SET_CACHE_DIR 2200
#define IDM_SET_PURGE     2201
#define IDM_SET_REBUILD   2202
#define IDM_SET_FORBIDDEN_EDIT   2203
#define IDM_SET_FORBIDDEN_RELOAD 2204

#define REFRESH_TIMER     1
#define REFRESH_DELAY_MS  80
#define SEARCH_TIMER      2
#define SEARCH_DEBOUNCE_MS 50

#define MAX_DRIVES 26

typedef struct {
    CoinIndex      idx;
    CoinJournalCtx jrn;
    HANDLE         thread;
    volatile LONG  ready;
    BOOL           enabled;
    wchar_t        label[64];
    wchar_t        fs[16];
    uint64_t       total_bytes;
    uint64_t       free_bytes;
    DWORD          start_ms;
    DWORD          ready_ms;
    BOOL           loaded_from_cache;
} CoinDrive;

static HWND g_main;
static HWND g_global_edit, g_refine_edit, g_results, g_volumes;
static HWND g_btn_manage, g_btn_settings;
static HWND g_lbl_volumes, g_lbl_brand;
static HWND g_lbl_filetype, g_lbl_date, g_lbl_size, g_lbl_tags;
static HWND g_btn_ft_pdf, g_btn_ft_docx, g_btn_ft_zip, g_btn_ft_img, g_btn_ft_video;
static HWND g_combo_date;
static HWND g_size_input, g_tags_input;

static HFONT g_font_brand, g_font_search, g_font_head, g_font_name, g_font_path, g_font_status, g_font_label;
static HBRUSH g_brush_bg, g_brush_panel, g_brush_input;

static CoinDrive g_drives[MAX_DRIVES];
static size_t g_drive_count = 0;
static CoinResults g_results_data;
static NOTIFYICONDATAW g_tray;
static BOOL g_hotkey_registered = FALSE;
static SYSTEMTIME g_last_sync_time;
static BOOL g_have_last_sync = FALSE;
static HANDLE g_singleton_mutex = NULL;
static UINT   g_msg_show = 0;
static BOOL   g_balloon_shown = FALSE;
static wchar_t       g_cache_dir[MAX_PATH] = L"";
static wchar_t       g_forbidden_path[MAX_PATH] = L"";
static volatile LONG g_skip_cache_save = 0;

typedef struct {
    LONG     version;
    unsigned flags;
    wchar_t  query[260];
    unsigned ext_mask;
    wchar_t  tags[256];
    int64_t  size_min;
    int64_t  size_max;
    BOOL     have_mtime_min;
    FILETIME mtime_min;
} CoinSearchTask;

static unsigned g_ft_mask = 0;
static int      g_date_choice = 0;

static volatile LONG g_search_version = 0;
static SRWLOCK       g_pending_lock = SRWLOCK_INIT;
static CoinResults   g_pending_results;
static BOOL          g_show_hidden_only = FALSE;
static RECT          g_hidden_link_rect = {0, 0, 0, 0};
static volatile LONG g_dirty_pending = 0;
static BOOL          g_in_background = FALSE;

#ifndef PROCESS_MODE_BACKGROUND_BEGIN
#define PROCESS_MODE_BACKGROUND_BEGIN 0x00100000
#endif
#ifndef PROCESS_MODE_BACKGROUND_END
#define PROCESS_MODE_BACKGROUND_END   0x00200000
#endif

static BOOL is_window_active(void) {
    return g_main && IsWindowVisible(g_main) && !IsIconic(g_main);
}

static void set_background_mode(BOOL on) {
    if (on == g_in_background) return;
    if (SetPriorityClass(GetCurrentProcess(),
                         on ? PROCESS_MODE_BACKGROUND_BEGIN
                            : PROCESS_MODE_BACKGROUND_END)) {
        g_in_background = on;
    }
}

static void populate_volumes_list(void);
static void rebuild_indexes_now(void);

static int default_cache_dir(wchar_t *out, size_t cap) {
    wchar_t appdata[MAX_PATH];
    DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) return 0;
    int n = swprintf(out, cap, L"%ls\\Coincidence", appdata);
    return n > 0 && (size_t)n < cap;
}

static int load_cache_dir_setting(wchar_t *out, size_t cap) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Coincidence", 0,
                      KEY_READ, &hk) != ERROR_SUCCESS) return 0;
    DWORD type = 0;
    DWORD bytes = (DWORD)(cap * sizeof(wchar_t));
    LONG r = RegQueryValueExW(hk, L"CachePath", NULL, &type,
                              (LPBYTE)out, &bytes);
    RegCloseKey(hk);
    if (r != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t)) return 0;
    size_t n = bytes / sizeof(wchar_t);
    if (n > 0 && out[n - 1] == 0) n--;
    if (n == 0 || n >= cap) return 0;
    out[n] = 0;
    return 1;
}

static int save_cache_dir_setting(const wchar_t *path) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Coincidence", 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL)
        != ERROR_SUCCESS) return 0;
    LONG r = RegSetValueExW(hk, L"CachePath", 0, REG_SZ,
                            (const BYTE*)path,
                            (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    return r == ERROR_SUCCESS;
}

static void resolve_cache_dir(void) {
    if (!load_cache_dir_setting(g_cache_dir, MAX_PATH)) {
        if (!default_cache_dir(g_cache_dir, MAX_PATH)) g_cache_dir[0] = 0;
    }
    if (g_cache_dir[0]) CreateDirectoryW(g_cache_dir, NULL);
}

static int make_cache_path(wchar_t drive, wchar_t *out, size_t cap) {
    if (g_cache_dir[0] == 0) return 0;
    int n = swprintf(out, cap, L"%ls\\index_%lc.bin", g_cache_dir, drive);
    return n > 0 && (size_t)n < cap;
}

static size_t discover_ntfs_fixed_drives(wchar_t *out_letters, size_t cap) {
    DWORD mask = GetLogicalDrives();
    size_t n = 0;
    for (int i = 0; i < 26 && n < cap; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        wchar_t fs_name[16];
        if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL,
                                   fs_name, ARRAYSIZE(fs_name))) continue;
        if (_wcsicmp(fs_name, L"NTFS") != 0) continue;
        out_letters[n++] = (wchar_t)(L'A' + i);
    }
    return n;
}

static void load_volume_metadata(CoinDrive *d) {
    wchar_t root[4] = { d->idx.drive_letter, L':', L'\\', 0 };
    GetVolumeInformationW(root, d->label, ARRAYSIZE(d->label), NULL, NULL, NULL,
                          d->fs, ARRAYSIZE(d->fs));
    if (d->label[0] == 0) wcscpy_s(d->label, ARRAYSIZE(d->label), L"Local Disk");
    ULARGE_INTEGER avail, total, free_b;
    if (GetDiskFreeSpaceExW(root, &avail, &total, &free_b)) {
        d->total_bytes = total.QuadPart;
        d->free_bytes  = free_b.QuadPart;
    }
}

static DWORD WINAPI worker_thread_proc(LPVOID param) {
    size_t di = (size_t)(uintptr_t)param;
    CoinDrive *d = &g_drives[di];
    d->start_ms = GetTickCount();

    if (!coin_mft_open(d->idx.drive_letter, &d->idx)) {
        d->ready_ms = GetTickCount();
        InterlockedExchange(&d->ready, 1);
        PostMessageW(g_main, WM_INDEX_DONE, (WPARAM)di, 0);
        return 1;
    }

    wchar_t cache_path[MAX_PATH];
    BOOL have_path = make_cache_path(d->idx.drive_letter, cache_path, MAX_PATH);
    DWORDLONG saved_jid = 0;
    USN saved_next = 0;
    BOOL loaded = FALSE;

    if (have_path && coin_mft_load_cache(&d->idx, cache_path, &saved_jid, &saved_next)) {
        if (saved_jid == d->idx.journal_id && saved_next >= d->idx.first_usn) {
            d->idx.next_usn = saved_next;
            loaded = TRUE;
        } else {
            coin_mft_clear_entries(&d->idx);
        }
    }
    if (!loaded) {
        coin_mft_enumerate(&d->idx);
        if (have_path) coin_mft_save_cache(&d->idx, cache_path);
    }
    d->loaded_from_cache = loaded;
    d->ready_ms = GetTickCount();

    InterlockedExchange(&d->ready, 1);
    PostMessageW(g_main, WM_INDEX_DONE, (WPARAM)di, 0);

    d->jrn.idx = &d->idx;
    d->jrn.notify_hwnd = g_main;
    d->jrn.notify_msg = WM_INDEX_DIRTY;
    d->jrn.stop = 0;
    return coin_journal_thread_proc((LPVOID)&d->jrn);
}

static void invalidate_chrome(void) {
    if (g_main) InvalidateRect(g_main, NULL, FALSE);
}

static uint64_t filetime_to_u64(const FILETIME *ft) {
    return ((uint64_t)ft->dwHighDateTime << 32) | (uint64_t)ft->dwLowDateTime;
}

static void stat_hits(CoinResults *r, LONG version) {
    for (size_t i = 0; i < r->count; ++i) {
        if (i % 32 == 0 && version != g_search_version) return;
        CoinHit *h = &r->hits[i];
        if (h->flags & COIN_HIT_STAT_DONE) continue;
        if (h->drive_idx >= g_drive_count) {
            h->flags |= COIN_HIT_STAT_DONE | COIN_HIT_STAT_FAILED;
            continue;
        }
        CoinIndex *idx = &g_drives[h->drive_idx].idx;

        wchar_t path[1024];
        AcquireSRWLockShared(&idx->lock);
        size_t plen = coin_mft_path(idx, h->entry_index, path, 1024);
        ReleaseSRWLockShared(&idx->lock);
        if (plen == 0) {
            h->flags |= COIN_HIT_STAT_DONE | COIN_HIT_STAT_FAILED;
            continue;
        }

        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
            h->flags |= COIN_HIT_STAT_DONE | COIN_HIT_STAT_FAILED;
            continue;
        }

        h->size  = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
        h->mtime = filetime_to_u64(&fad.ftLastWriteTime);
        h->flags |= COIN_HIT_STAT_DONE;
    }
}

static void apply_post_filter(const CoinSearchTask *task, CoinResults *r) {
    BOOL need_size  = (task->size_min > 0) || (task->size_max >= 0);
    BOOL need_mtime = task->have_mtime_min;
    if (!need_size && !need_mtime) return;

    uint64_t mtime_min_u64 = filetime_to_u64(&task->mtime_min);

    size_t out = 0;
    for (size_t i = 0; i < r->count; ++i) {
        const CoinHit *h = &r->hits[i];
        if (h->flags & COIN_HIT_STAT_FAILED) continue;
        if (!(h->flags & COIN_HIT_STAT_DONE)) continue;

        if (need_size) {
            if (task->size_min > 0 && h->size < task->size_min) continue;
            if (task->size_max >= 0 && h->size > task->size_max) continue;
        }
        if (need_mtime) {
            if (h->mtime < mtime_min_u64) continue;
        }

        if (out != i) r->hits[out] = *h;
        out++;
    }
    if (r->total_matched > out) r->total_matched = out;
    r->count = out;
}

static DWORD WINAPI search_thread_proc(LPVOID param) {
    CoinSearchTask *task = (CoinSearchTask*)param;

    CoinIndex *arr[MAX_DRIVES];
    size_t any_ready = 0;
    for (size_t i = 0; i < g_drive_count; ++i) {
        if (g_drives[i].ready && g_drives[i].enabled) {
            arr[i] = &g_drives[i].idx;
            any_ready++;
        } else {
            arr[i] = NULL;
        }
    }

    CoinPrefilter pre;
    pre.ext_mask = task->ext_mask;
    pre.tags     = task->tags[0] ? task->tags : NULL;

    CoinResults r;
    memset(&r, 0, sizeof(r));
    if (any_ready > 0) {
        coin_search_all(arr, g_drive_count, task->query, &pre, 1000, task->flags, &r);
        stat_hits(&r, task->version);
        apply_post_filter(task, &r);
    }

    AcquireSRWLockExclusive(&g_pending_lock);
    if (task->version == g_search_version) {
        coin_results_free(&g_pending_results);
        g_pending_results = r;
        ReleaseSRWLockExclusive(&g_pending_lock);
        PostMessageW(g_main, WM_SEARCH_DONE, (WPARAM)task->version, 0);
    } else {
        ReleaseSRWLockExclusive(&g_pending_lock);
        coin_results_free(&r);
    }

    free(task);
    return 0;
}

static int64_t parse_size_token(const wchar_t *s, size_t len) {
    if (len == 0) return -1;
    int64_t val = 0;
    size_t i = 0;
    int saw_digit = 0;
    while (i < len && s[i] >= L'0' && s[i] <= L'9') {
        val = val * 10 + (s[i] - L'0');
        i++;
        saw_digit = 1;
    }
    if (!saw_digit) return -1;
    while (i < len && (s[i] == L' ' || s[i] == L'\t')) i++;
    int64_t mult = 1;
    if (i < len) {
        wchar_t u = s[i];
        if (u >= L'A' && u <= L'Z') u = (wchar_t)(u + 32);
        if (u == L'k') mult = 1024;
        else if (u == L'm') mult = 1024 * 1024;
        else if (u == L'g') mult = (int64_t)1024 * 1024 * 1024;
        else if (u == L't') mult = (int64_t)1024 * 1024 * 1024 * 1024;
        else if (u == L'b') mult = 1;
        else return -1;
    }
    return val * mult;
}

static void parse_size_input_into_task(CoinSearchTask *task) {
    task->size_min = -1;
    task->size_max = -1;

    wchar_t buf[64];
    GetWindowTextW(g_size_input, buf, ARRAYSIZE(buf));
    wchar_t *p = buf;
    while (*p == L' ' || *p == L'\t') p++;
    if (!*p) return;

    int op_gt = 0, op_lt = 0;
    if (*p == L'>') { op_gt = 1; p++; if (*p == L'=') p++; }
    else if (*p == L'<') { op_lt = 1; p++; if (*p == L'=') p++; }
    while (*p == L' ' || *p == L'\t') p++;

    wchar_t *dash = NULL;
    for (wchar_t *q = p; *q; ++q) { if (*q == L'-') { dash = q; break; } }

    if (dash && !op_gt && !op_lt) {
        size_t lo_len = (size_t)(dash - p);
        int64_t lo = parse_size_token(p, lo_len);
        wchar_t *r = dash + 1;
        while (*r == L' ' || *r == L'\t') r++;
        size_t hi_len = wcslen(r);
        int64_t hi = parse_size_token(r, hi_len);
        if (lo >= 0) task->size_min = lo;
        if (hi >= 0) task->size_max = hi;
        return;
    }

    int64_t v = parse_size_token(p, wcslen(p));
    if (v < 0) return;
    if (op_lt) task->size_max = v;
    else       task->size_min = v;
}

static void parse_date_choice_into_task(CoinSearchTask *task, int choice) {
    task->have_mtime_min = FALSE;
    task->mtime_min.dwLowDateTime  = 0;
    task->mtime_min.dwHighDateTime = 0;
    if (choice <= 0) return;

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER nu;
    nu.LowPart = now.dwLowDateTime;
    nu.HighPart = now.dwHighDateTime;

    const ULONGLONG SEC = 10000000ULL;
    const ULONGLONG DAY = 24ULL * 3600ULL * SEC;
    ULONGLONG cutoff = nu.QuadPart;
    switch (choice) {
        case 1: cutoff -= DAY;        break;
        case 2: cutoff -= 7  * DAY;   break;
        case 3: cutoff -= 30 * DAY;   break;
        case 4: {
            SYSTEMTIME st;
            GetLocalTime(&st);
            SYSTEMTIME jan;
            memset(&jan, 0, sizeof(jan));
            jan.wYear = st.wYear;
            jan.wMonth = 1;
            jan.wDay = 1;
            FILETIME lf, uf;
            if (SystemTimeToFileTime(&jan, &lf) && LocalFileTimeToFileTime(&lf, &uf)) {
                task->mtime_min = uf;
                task->have_mtime_min = TRUE;
            }
            return;
        }
        default: return;
    }
    task->mtime_min.dwLowDateTime  = (DWORD)(cutoff & 0xFFFFFFFFu);
    task->mtime_min.dwHighDateTime = (DWORD)(cutoff >> 32);
    task->have_mtime_min = TRUE;
}

static void schedule_search(void) {
    CoinSearchTask *task = (CoinSearchTask*)malloc(sizeof(*task));
    if (!task) return;
    memset(task, 0, sizeof(*task));
    task->version = InterlockedIncrement(&g_search_version);
    task->flags = g_show_hidden_only ? COIN_SEARCH_HIDDEN_ONLY : 0;
    GetWindowTextW(g_global_edit, task->query, ARRAYSIZE(task->query));

    task->ext_mask = g_ft_mask;
    GetWindowTextW(g_tags_input, task->tags, ARRAYSIZE(task->tags));
    parse_size_input_into_task(task);
    parse_date_choice_into_task(task, g_date_choice);

    if (!QueueUserWorkItem(search_thread_proc, task, WT_EXECUTELONGFUNCTION)) {
        free(task);
    }
}

static void schedule_refresh(void) {
    SetTimer(g_main, REFRESH_TIMER, REFRESH_DELAY_MS, NULL);
}

static void apply_pending_results(LONG version) {
    if (version != g_search_version) return;

    AcquireSRWLockExclusive(&g_pending_lock);
    if (version == g_search_version) {
        coin_results_free(&g_results_data);
        g_results_data = g_pending_results;
        memset(&g_pending_results, 0, sizeof(g_pending_results));
    }
    ReleaseSRWLockExclusive(&g_pending_lock);

    SendMessageW(g_results, LVM_SETITEMCOUNT, (WPARAM)g_results_data.count, 0);
    InvalidateRect(g_results, NULL, TRUE);

    if (g_results_data.count > 0) {
        LVITEMW it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_STATE;
        it.state = LVIS_FOCUSED;
        it.stateMask = LVIS_FOCUSED;
        SendMessageW(g_results, LVM_SETITEMSTATE, 0, (LPARAM)&it);
    }

    invalidate_chrome();
}

static int active_volume_count(void) {
    int n = 0;
    for (size_t i = 0; i < g_drive_count; ++i) if (g_drives[i].enabled) n++;
    return n;
}

static void format_size_short(int64_t n, wchar_t *out, size_t cap) {
    if (n < 0)            { swprintf(out, cap, L"—"); return; }
    if (n < 1024)         { swprintf(out, cap, L"%lld B", (long long)n); return; }
    double v = (double)n;
    static const wchar_t *units[] = { L"KB", L"MB", L"GB", L"TB", L"PB" };
    int u = -1;
    while (v >= 1024.0 && u < (int)(sizeof(units)/sizeof(units[0])) - 1) {
        v /= 1024.0;
        u++;
    }
    if (u < 0) { swprintf(out, cap, L"%lld B", (long long)n); return; }
    if (v >= 100.0)      swprintf(out, cap, L"%.0f %ls", v, units[u]);
    else if (v >= 10.0)  swprintf(out, cap, L"%.1f %ls", v, units[u]);
    else                 swprintf(out, cap, L"%.2f %ls", v, units[u]);
}

static void format_mtime_short(uint64_t mt, wchar_t *out, size_t cap) {
    if (mt == 0) { swprintf(out, cap, L"—"); return; }
    FILETIME ut;
    ut.dwLowDateTime  = (DWORD)(mt & 0xFFFFFFFFu);
    ut.dwHighDateTime = (DWORD)(mt >> 32);
    FILETIME lt;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ut, &lt) || !FileTimeToSystemTime(&lt, &st)) {
        swprintf(out, cap, L"—");
        return;
    }
    swprintf(out, cap, L"%04d-%02d-%02d %02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

static size_t parent_dir_len(const wchar_t *path, size_t plen) {
    while (plen > 0 && path[plen - 1] != L'\\' && path[plen - 1] != L'/') plen--;
    if (plen > 1 && (path[plen - 1] == L'\\' || path[plen - 1] == L'/')
        && !(plen == 3 && path[1] == L':')) {
        plen--;
    }
    return plen;
}

static LRESULT on_results_custom_draw(NMLVCUSTOMDRAW *cd) {
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;

        case CDDS_ITEMPREPAINT: {
            HDC hdc = cd->nmcd.hdc;
            int idx = (int)cd->nmcd.dwItemSpec;
            RECT rc = cd->nmcd.rc;
            BOOL selected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;

            HBRUSH bg = CreateSolidBrush(selected ? COL_BG_SEL : COL_PANEL);
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);

            if (idx < 0 || (size_t)idx >= g_results_data.count) return CDRF_SKIPDEFAULT;

            const CoinHit *hit = &g_results_data.hits[idx];
            if (hit->drive_idx >= g_drive_count) return CDRF_SKIPDEFAULT;
            CoinIndex *cidx = &g_drives[hit->drive_idx].idx;

            if (selected) {
                RECT bar = { rc.left, rc.top + 3, rc.left + 3, rc.bottom - 3 };
                HBRUSH a = CreateSolidBrush(COL_ACCENT);
                FillRect(hdc, &bar, a);
                DeleteObject(a);
            }

            wchar_t name_buf[260], path_buf[1024];
            name_buf[0] = 0; path_buf[0] = 0;
            size_t plen = 0;

            AcquireSRWLockShared(&cidx->lock);
            if (hit->entry_index < cidx->count) {
                const CoinEntry *e = &cidx->entries[hit->entry_index];
                size_t name_n = e->name_len < 259 ? e->name_len : 259;
                memcpy(name_buf, cidx->name_pool + e->name_offset, name_n * sizeof(wchar_t));
                name_buf[name_n] = 0;
                plen = coin_mft_path(cidx, hit->entry_index, path_buf, 1024);
            }
            ReleaseSRWLockShared(&cidx->lock);

            size_t parent_len = parent_dir_len(path_buf, plen);
            wchar_t parent_buf[1024];
            if (parent_len >= ARRAYSIZE(parent_buf)) parent_len = ARRAYSIZE(parent_buf) - 1;
            memcpy(parent_buf, path_buf, parent_len * sizeof(wchar_t));
            parent_buf[parent_len] = 0;

            wchar_t size_buf[32], date_buf[32];
            format_size_short(hit->size, size_buf, ARRAYSIZE(size_buf));
            format_mtime_short(hit->mtime, date_buf, ARRAYSIZE(date_buf));

            int col_w[4];
            for (int c = 0; c < 4; ++c) col_w[c] = (int)SendMessageW(g_results, LVM_GETCOLUMNWIDTH, c, 0);

            SetBkMode(hdc, TRANSPARENT);
            HFONT old_font = (HFONT)SelectObject(hdc, g_font_name);
            SetTextColor(hdc, selected ? COL_TEXT_SEL : COL_TEXT);

            int x = rc.left;
            const int pad_left = 12;
            const int pad_right = 12;

            RECT name_rc = { x + pad_left, rc.top, x + col_w[0] - pad_right, rc.bottom };
            DrawTextW(hdc, name_buf, -1, &name_rc,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER | DT_NOPREFIX);
            x += col_w[0];

            SelectObject(hdc, g_font_path);
            SetTextColor(hdc, selected ? COL_TEXT_SEL : COL_TEXT_DIM);

            RECT path_rc = { x + pad_left, rc.top, x + col_w[1] - pad_right, rc.bottom };
            DrawTextW(hdc, parent_buf, -1, &path_rc,
                      DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_VCENTER | DT_NOPREFIX);
            x += col_w[1];

            RECT size_rc = { x + pad_left, rc.top, x + col_w[2] - pad_right, rc.bottom };
            DrawTextW(hdc, size_buf, -1, &size_rc,
                      DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            x += col_w[2];

            RECT date_rc = { x + pad_left, rc.top, x + col_w[3] - pad_right, rc.bottom };
            DrawTextW(hdc, date_buf, -1, &date_rc,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER | DT_NOPREFIX);

            SelectObject(hdc, old_font);
            return CDRF_SKIPDEFAULT;
        }
    }
    return CDRF_DODEFAULT;
}

static LRESULT on_volumes_custom_draw(NMLVCUSTOMDRAW *cd) {
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:     return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
            int i = (int)cd->nmcd.dwItemSpec;
            BOOL selected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;
            cd->clrTextBk = selected ? COL_BG_HOVER : COL_PANEL;
            cd->clrText   = (i >= 0 && (size_t)i < g_drive_count && g_drives[i].enabled)
                             ? COL_TEXT : COL_TEXT_DIM;
            return CDRF_NEWFONT;
        }
    }
    return CDRF_DODEFAULT;
}

static void open_path(const wchar_t *path) {
    if (!path || !path[0]) return;
    ShellExecuteW(g_main, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

static int build_path(int item, wchar_t *out, size_t cap) {
    if (item < 0 || (size_t)item >= g_results_data.count) return 0;
    const CoinHit *hit = &g_results_data.hits[item];
    if (hit->drive_idx >= g_drive_count) return 0;
    CoinIndex *cidx = &g_drives[hit->drive_idx].idx;
    AcquireSRWLockShared(&cidx->lock);
    size_t len = coin_mft_path(cidx, hit->entry_index, out, cap);
    ReleaseSRWLockShared(&cidx->lock);
    return len > 0;
}

static void open_result(int item) {
    wchar_t path[1024];
    if (build_path(item, path, 1024)) open_path(path);
}

static void show_in_explorer(int item) {
    wchar_t path[1024];
    if (!build_path(item, path, 1024)) return;
    wchar_t arg[1100];
    swprintf(arg, 1100, L"/select,\"%ls\"", path);
    ShellExecuteW(g_main, L"open", L"explorer.exe", arg, NULL, SW_SHOWNORMAL);
}

static void copy_path_to_clipboard(int item) {
    wchar_t path[1024];
    if (!build_path(item, path, 1024)) return;
    if (!OpenClipboard(g_main)) return;
    EmptyClipboard();
    size_t n = wcslen(path) + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n * sizeof(wchar_t));
    if (h) {
        wchar_t *p = (wchar_t*)GlobalLock(h);
        if (p) {
            memcpy(p, path, n * sizeof(wchar_t));
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

static void show_results_context_menu(int item, POINT *screen_pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_CTX_OPEN, L"Open");
    AppendMenuW(m, MF_STRING, IDM_CTX_SHOW, L"Show in Explorer");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_CTX_COPYPATH, L"Copy Path");
    SetForegroundWindow(g_main);
    int cmd = (int)TrackPopupMenu(m,
        TPM_RIGHTBUTTON | TPM_RETURNCMD,
        screen_pt->x, screen_pt->y, 0, g_main, NULL);
    DestroyMenu(m);
    switch (cmd) {
        case IDM_CTX_OPEN:     open_result(item); break;
        case IDM_CTX_SHOW:     show_in_explorer(item); break;
        case IDM_CTX_COPYPATH: copy_path_to_clipboard(item); break;
    }
}

static int CALLBACK browse_init_proc(HWND hwnd, UINT msg, LPARAM lp, LPARAM data) {
    (void)lp;
    if (msg == BFFM_INITIALIZED && data) {
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    }
    return 0;
}

static void change_cache_dir(void) {
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_main;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    bi.lpszTitle = L"Choose where Coincidence stores its index cache";
    bi.lpfn = browse_init_proc;
    bi.lParam = (LPARAM)g_cache_dir;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;

    wchar_t path[MAX_PATH];
    BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree((LPVOID)pidl);
    if (!ok || path[0] == 0) return;

    if (_wcsicmp(path, g_cache_dir) == 0) return;

    wcscpy_s(g_cache_dir, MAX_PATH, path);
    CreateDirectoryW(g_cache_dir, NULL);
    save_cache_dir_setting(g_cache_dir);

    wchar_t msg[MAX_PATH + 256];
    swprintf(msg, MAX_PATH + 256,
             L"Coincidence will store indexes in:\n\n%ls\n\n"
             L"New caches will be written here on exit. Existing caches in the "
             L"previous folder are no longer used — use Settings → Purge cache "
             L"files (after pointing back at the old folder) to delete them.",
             g_cache_dir);
    MessageBoxW(g_main, msg, L"Coincidence", MB_OK | MB_ICONINFORMATION);
}

static void purge_cache_files(void) {
    if (g_cache_dir[0] == 0) return;

    wchar_t prompt[MAX_PATH + 384];
    swprintf(prompt, MAX_PATH + 384,
             L"Delete all cached index files from:\n\n%ls\n\n"
             L"Coincidence will not write caches again until you restart. "
             L"In-memory indexes keep working for this session.",
             g_cache_dir);
    if (MessageBoxW(g_main, prompt, L"Coincidence — Purge cache",
                    MB_YESNO | MB_ICONWARNING) != IDYES) return;

    wchar_t pattern[MAX_PATH + 32];
    swprintf(pattern, MAX_PATH + 32, L"%ls\\index_*.bin", g_cache_dir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    int deleted = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t fp[MAX_PATH + 64];
            swprintf(fp, MAX_PATH + 64, L"%ls\\%ls", g_cache_dir, fd.cFileName);
            if (DeleteFileW(fp)) deleted++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    InterlockedExchange(&g_skip_cache_save, 1);

    wchar_t msg[256];
    swprintf(msg, 256,
             L"Deleted %d cache file%ls.\n\n"
             L"Caches will be rebuilt the next time you start Coincidence.",
             deleted, deleted == 1 ? L"" : L"s");
    MessageBoxW(g_main, msg, L"Coincidence", MB_OK | MB_ICONINFORMATION);
}

static int build_forbidden_path(void) {
    if (g_cache_dir[0] == 0) { g_forbidden_path[0] = 0; return 0; }
    int n = swprintf(g_forbidden_path, MAX_PATH, L"%ls\\forbidden.txt", g_cache_dir);
    return n > 0 && (size_t)n < MAX_PATH;
}

static void load_forbidden_words(void) {
    if (!build_forbidden_path()) {
        coin_search_set_forbidden(NULL, 0);
        return;
    }

    HANDLE h = CreateFileW(g_forbidden_path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        coin_search_set_forbidden(NULL, 0);
        return;
    }

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0
        || sz.QuadPart > (LONGLONG)(4 * 1024 * 1024)) {
        CloseHandle(h);
        coin_search_set_forbidden(NULL, 0);
        return;
    }

    DWORD len = (DWORD)sz.QuadPart;
    char *bytes = (char*)malloc(len + 1);
    if (!bytes) { CloseHandle(h); coin_search_set_forbidden(NULL, 0); return; }

    DWORD got = 0;
    BOOL ok = ReadFile(h, bytes, len, &got, NULL);
    CloseHandle(h);
    if (!ok) { free(bytes); coin_search_set_forbidden(NULL, 0); return; }
    bytes[got] = 0;

    char *p = bytes;
    if (got >= 3
        && (unsigned char)p[0] == 0xEF
        && (unsigned char)p[1] == 0xBB
        && (unsigned char)p[2] == 0xBF) {
        p += 3; got -= 3;
    }

    if (got == 0) {
        free(bytes);
        coin_search_set_forbidden(NULL, 0);
        return;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, p, (int)got, NULL, 0);
    if (wlen <= 0) {
        free(bytes);
        coin_search_set_forbidden(NULL, 0);
        return;
    }
    wchar_t *wbuf = (wchar_t*)malloc((wlen + 1) * sizeof(wchar_t));
    if (!wbuf) { free(bytes); coin_search_set_forbidden(NULL, 0); return; }
    MultiByteToWideChar(CP_UTF8, 0, p, (int)got, wbuf, wlen);
    wbuf[wlen] = 0;
    free(bytes);

    enum { MAX_WORDS = 256 };
    wchar_t *words[MAX_WORDS];
    size_t count = 0;
    wchar_t *line_start = wbuf;
    for (wchar_t *cur = wbuf; ; ++cur) {
        if (*cur == 0 || *cur == L'\n' || *cur == L'\r') {
            wchar_t saved = *cur;
            *cur = 0;
            wchar_t *s = line_start;
            while (*s == L' ' || *s == L'\t') s++;
            wchar_t *e = cur;
            while (e > s && (e[-1] == L' ' || e[-1] == L'\t')) e--;
            *e = 0;
            if (*s != 0 && *s != L'#' && count < MAX_WORDS) {
                words[count++] = s;
            }
            if (saved == 0) break;
            line_start = cur + 1;
        }
    }

    coin_search_set_forbidden((const wchar_t *const *)words, count);
    free(wbuf);
}

static void edit_forbidden_words(void) {
    if (!build_forbidden_path()) {
        MessageBoxW(g_main,
            L"Cache folder is not set, so the forbidden word list has nowhere to live. "
            L"Pick a cache folder from Settings first.",
            L"Coincidence", MB_OK | MB_ICONERROR);
        return;
    }

    if (GetFileAttributesW(g_forbidden_path) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(g_forbidden_path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            const char *header =
                "\xEF\xBB\xBF"
                "# Coincidence forbidden word list\r\n"
                "# Files whose path contains any of these words (case-insensitive)\r\n"
                "# will never appear in search results. Useful for parental controls.\r\n"
                "# One word or phrase per line. Lines starting with '#' are comments.\r\n"
                "# Blank lines are ignored. After saving, choose 'Reload forbidden\r\n"
                "# words' from the Settings menu to apply changes.\r\n"
                "\r\n";
            DWORD wrote = 0;
            WriteFile(h, header, (DWORD)strlen(header), &wrote, NULL);
            CloseHandle(h);
        }
    }

    ShellExecuteW(g_main, L"open", L"notepad.exe", g_forbidden_path, NULL, SW_SHOWNORMAL);

    MessageBoxW(g_main,
        L"After saving the file, choose \"Reload forbidden words\" from the "
        L"Settings menu to apply your changes.",
        L"Coincidence", MB_OK | MB_ICONINFORMATION);
}

static void reload_forbidden_words(void) {
    load_forbidden_words();
    schedule_search();
}

static void show_settings_menu(void) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SET_CACHE_DIR, L"Change cache folder…");
    AppendMenuW(m, MF_STRING, IDM_SET_REBUILD,   L"Rebuild indexes now");
    AppendMenuW(m, MF_STRING, IDM_SET_PURGE,     L"Purge cache files now");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_SET_FORBIDDEN_EDIT,   L"Edit forbidden words…");
    AppendMenuW(m, MF_STRING, IDM_SET_FORBIDDEN_RELOAD, L"Reload forbidden words");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    wchar_t cur[MAX_PATH + 32];
    swprintf(cur, MAX_PATH + 32, L"Cache: %ls",
             g_cache_dir[0] ? g_cache_dir : L"(unset)");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, cur);

    RECT br;
    GetWindowRect(g_btn_settings, &br);
    SetForegroundWindow(g_main);
    int cmd = (int)TrackPopupMenu(m,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_RIGHTALIGN,
        br.right, br.bottom + 2, 0, g_main, NULL);
    DestroyMenu(m);

    switch (cmd) {
        case IDM_SET_CACHE_DIR:        change_cache_dir(); break;
        case IDM_SET_REBUILD:          rebuild_indexes_now(); break;
        case IDM_SET_PURGE:            purge_cache_files(); break;
        case IDM_SET_FORBIDDEN_EDIT:   edit_forbidden_words(); break;
        case IDM_SET_FORBIDDEN_RELOAD: reload_forbidden_words(); break;
    }
}

static void show_and_focus(void) {
    if (!IsWindowVisible(g_main)) ShowWindow(g_main, SW_SHOW);
    if (IsIconic(g_main)) ShowWindow(g_main, SW_RESTORE);

    set_background_mode(FALSE);

    DWORD fg_thread = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    DWORD my_thread = GetCurrentThreadId();
    if (fg_thread && fg_thread != my_thread) {
        AttachThreadInput(my_thread, fg_thread, TRUE);
        BringWindowToTop(g_main);
        SetForegroundWindow(g_main);
        AttachThreadInput(my_thread, fg_thread, FALSE);
    } else {
        BringWindowToTop(g_main);
        SetForegroundWindow(g_main);
    }
    SetFocus(g_global_edit);
    SendMessageW(g_global_edit, EM_SETSEL, 0, -1);

    if (InterlockedExchange(&g_dirty_pending, 0)) {
        schedule_search();
        invalidate_chrome();
    }
}

static DWORD WINAPI compact_drive_proc(LPVOID p) {
    size_t di = (size_t)(uintptr_t)p;
    if (di >= g_drive_count) return 0;
    CoinDrive *d = &g_drives[di];
    if (!d->ready) return 0;

    if (!coin_mft_compact(&d->idx)) return 0;

    if (g_skip_cache_save) return 0;
    wchar_t cache_path[MAX_PATH];
    if (!make_cache_path(d->idx.drive_letter, cache_path, MAX_PATH)) return 0;
    AcquireSRWLockShared(&d->idx.lock);
    coin_mft_save_cache(&d->idx, cache_path);
    ReleaseSRWLockShared(&d->idx.lock);
    return 0;
}

static void schedule_compact_if_wasteful(void) {
    for (size_t i = 0; i < g_drive_count; ++i) {
        CoinDrive *d = &g_drives[i];
        if (!d->ready || !d->enabled) continue;
        size_t waste = coin_mft_compact_waste(&d->idx);
        if (waste < (size_t)(2 * 1024 * 1024)) continue;
        QueueUserWorkItem(compact_drive_proc,
                          (LPVOID)(uintptr_t)i,
                          WT_EXECUTELONGFUNCTION);
    }
}

static void hide_to_tray(void) {
    ShowWindow(g_main, SW_HIDE);
    set_background_mode(TRUE);
    schedule_compact_if_wasteful();
}

static void toggle_window(void) {
    BOOL visible = IsWindowVisible(g_main) && !IsIconic(g_main);
    if (visible && GetForegroundWindow() == g_main) {
        hide_to_tray();
    } else {
        show_and_focus();
    }
}

static void install_tray(HWND hwnd) {
    memset(&g_tray, 0, sizeof(g_tray));
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = hwnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    const wchar_t *tip = L"Coincidence — Ctrl+Alt+J";
    size_t n = wcslen(tip);
    if (n >= ARRAYSIZE(g_tray.szTip)) n = ARRAYSIZE(g_tray.szTip) - 1;
    memcpy(g_tray.szTip, tip, n * sizeof(wchar_t));
    g_tray.szTip[n] = 0;
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}

static void show_tray_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_TRAY_TOGGLE,
                IsWindowVisible(hwnd) ? L"Hide" : L"Show");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_TRAY_QUIT, L"Quit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
                   pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK edit_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                            UINT_PTR uId, DWORD_PTR ref) {
    (void)uId; (void)ref;
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) { hide_to_tray(); return 0; }
        if (wp == VK_RETURN) {
            int sel = (int)SendMessageW(g_results, LVM_GETNEXTITEM,
                                         (WPARAM)-1, LVNI_SELECTED);
            if (sel < 0 && g_results_data.count > 0) sel = 0;
            if (sel >= 0) open_result(sel);
            return 0;
        }
        if (wp == VK_DOWN && g_results_data.count > 0) { SetFocus(g_results); return 0; }
    }
    if (msg == WM_CHAR && (wp == VK_ESCAPE || wp == VK_RETURN)) return 0;
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK list_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                            UINT_PTR uId, DWORD_PTR ref) {
    (void)uId; (void)ref;
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) { hide_to_tray(); return 0; }
        if (wp == VK_RETURN) {
            int sel = (int)SendMessageW(hwnd, LVM_GETNEXTITEM,
                                         (WPARAM)-1, LVNI_SELECTED);
            if (sel >= 0) open_result(sel);
            return 0;
        }
        if (wp >= 0x20 && wp <= 0x7E) {
            SetFocus(g_global_edit);
            SendMessageW(g_global_edit, WM_KEYDOWN, wp, lp);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static unsigned ft_bit_for_id(UINT id) {
    switch (id) {
        case ID_FT_PDF:   return COIN_EXT_PDF;
        case ID_FT_DOCX:  return COIN_EXT_DOC;
        case ID_FT_ZIP:   return COIN_EXT_ZIP;
        case ID_FT_IMG:   return COIN_EXT_IMG;
        case ID_FT_VIDEO: return COIN_EXT_VIDEO;
        default:          return 0;
    }
}

static void draw_owner_button(DRAWITEMSTRUCT *di) {
    HDC hdc = di->hDC;
    RECT rc = di->rcItem;
    BOOL momentary = (di->itemState & ODS_SELECTED) != 0;
    BOOL disabled  = (di->itemState & ODS_DISABLED) != 0;
    BOOL focused   = (di->itemState & ODS_FOCUS) != 0;

    unsigned ft_bit = ft_bit_for_id(di->CtlID);
    BOOL toggled = ft_bit && (g_ft_mask & ft_bit);
    BOOL pressed = momentary || toggled;

    COLORREF bg;
    if (disabled)      bg = COL_BG;
    else if (pressed)  bg = COL_BG_SEL;
    else               bg = COL_PANEL;

    HBRUSH bgB = CreateSolidBrush(bg);
    FillRect(hdc, &rc, bgB);
    DeleteObject(bgB);

    HPEN pen = CreatePen(PS_SOLID, 1, focused ? COL_ACCENT : COL_DIVIDER);
    HBRUSH null_b = (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN oldp = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldb = (HBRUSH)SelectObject(hdc, null_b);
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);

    wchar_t text[128];
    int n = GetWindowTextW(di->hwndItem, text, 128);
    if (n > 0) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, disabled ? COL_TEXT_DIM : COL_TEXT);
        DrawTextW(hdc, text, -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

static void draw_owner_combo(DRAWITEMSTRUCT *di) {
    HDC hdc = di->hDC;
    RECT rc = di->rcItem;
    BOOL selected = (di->itemState & ODS_SELECTED) != 0;
    BOOL editPart = (di->itemState & ODS_COMBOBOXEDIT) != 0;
    BOOL disabled = (di->itemState & ODS_DISABLED) != 0;

    COLORREF bg = editPart ? COL_INPUT : (selected ? COL_BG_SEL : COL_PANEL);
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(hdc, &rc, b);
    DeleteObject(b);

    if ((LONG)di->itemID < 0) return;

    wchar_t text[128];
    SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)text);
    SetBkMode(hdc, TRANSPARENT);
    COLORREF tc = disabled ? COL_TEXT_DIM
                : (selected && !editPart ? COL_TEXT_SEL : COL_TEXT);
    SetTextColor(hdc, tc);
    rc.left += 8;
    DrawTextW(hdc, text, -1, &rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void paint_chrome(HDC hdc, RECT *crc) {
    FillRect(hdc, crc, g_brush_bg);

    int body_top   = HEADER_H;
    int footer_top = crc->bottom - FOOTER_H;
    int W = crc->right;

    HPEN pen = CreatePen(PS_SOLID, 1, COL_DIVIDER);
    HPEN old_pen = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, 0, body_top - 1, NULL);
    LineTo(hdc, W, body_top - 1);
    MoveToEx(hdc, 0, footer_top, NULL);
    LineTo(hdc, W, footer_top);
    MoveToEx(hdc, LEFT_W, body_top, NULL);
    LineTo(hdc, LEFT_W, footer_top);
    MoveToEx(hdc, W - RIGHT_W, body_top, NULL);
    LineTo(hdc, W - RIGHT_W, footer_top);
    SelectObject(hdc, old_pen);
    DeleteObject(pen);

    RECT center_title = { LEFT_W + HPAD, body_top + HPAD,
                          W - RIGHT_W - HPAD, body_top + HPAD + 28 };
    SetBkMode(hdc, TRANSPARENT);
    HFONT old_f = (HFONT)SelectObject(hdc, g_font_head);
    SetTextColor(hdc, COL_TEXT);

    int active = active_volume_count();
    wchar_t letters[160] = L"";
    int letter_off = 0;
    for (size_t i = 0; i < g_drive_count && letter_off < 150; ++i) {
        if (!g_drives[i].enabled) continue;
        if (letter_off > 0) letters[letter_off++] = L',', letters[letter_off++] = L' ';
        letters[letter_off++] = g_drives[i].idx.drive_letter;
        letters[letter_off++] = L':';
    }
    letters[letter_off] = 0;

    SetRectEmpty(&g_hidden_link_rect);

    if (g_show_hidden_only) {
        wchar_t hidden_label[200];
        swprintf(hidden_label, 200,
                 L"Showing %zu hidden entr%ls  ·  click to return",
                 g_results_data.total_matched,
                 g_results_data.total_matched == 1 ? L"y" : L"ies");
        SetTextColor(hdc, COL_ACCENT);
        SIZE hsz;
        GetTextExtentPoint32W(hdc, hidden_label, (int)wcslen(hidden_label), &hsz);
        DrawTextW(hdc, hidden_label, -1, &center_title,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        int rt = center_title.left + hsz.cx;
        if (rt > center_title.right) rt = center_title.right;
        g_hidden_link_rect.left   = center_title.left;
        g_hidden_link_rect.top    = center_title.top;
        g_hidden_link_rect.right  = rt;
        g_hidden_link_rect.bottom = center_title.bottom;
    } else {
        wchar_t title_main[256];
        swprintf(title_main, 256,
                 L"Found %zu file%ls in %d active volume%ls (%ls)",
                 g_results_data.total_matched,
                 g_results_data.total_matched == 1 ? L"" : L"s",
                 active,
                 active == 1 ? L"" : L"s",
                 letter_off ? letters : L"none");
        SetTextColor(hdc, COL_TEXT);
        DrawTextW(hdc, title_main, -1, &center_title,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (g_results_data.total_hidden > 0) {
            SIZE psz;
            GetTextExtentPoint32W(hdc, title_main, (int)wcslen(title_main), &psz);

            wchar_t link_text[160];
            swprintf(link_text, 160,
                     L"  ·  hiding %zu entr%ls due to forbidden words",
                     g_results_data.total_hidden,
                     g_results_data.total_hidden == 1 ? L"y" : L"ies");
            SIZE lsz;
            GetTextExtentPoint32W(hdc, link_text, (int)wcslen(link_text), &lsz);

            RECT link_rc = { center_title.left + psz.cx, center_title.top,
                             center_title.right, center_title.bottom };
            SetTextColor(hdc, COL_ACCENT);
            DrawTextW(hdc, link_text, -1, &link_rc,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

            int rt = center_title.left + psz.cx + lsz.cx;
            if (rt > center_title.right) rt = center_title.right;
            g_hidden_link_rect.left   = center_title.left + psz.cx;
            g_hidden_link_rect.top    = center_title.top;
            g_hidden_link_rect.right  = rt;
            g_hidden_link_rect.bottom = center_title.bottom;
        }
    }

    SelectObject(hdc, g_font_status);
    SetTextColor(hdc, COL_TEXT_DIM);
    RECT footer = { HPAD, footer_top + 4, W - HPAD, crc->bottom - 4 };

    size_t total = 0, ready = 0;
    for (size_t i = 0; i < g_drive_count; ++i) {
        total += g_drives[i].idx.count;
        if (g_drives[i].ready) ready++;
    }

    wchar_t left[200];
    if (g_drive_count == 0) {
        swprintf(left, 200, L"No NTFS volumes found");
    } else if (ready < g_drive_count) {
        int pct = (int)((100.0 * ready) / (double)g_drive_count);
        swprintf(left, 200, L"Indexing: %d%%  ·  %zu files indexed (%zu/%zu volumes)",
                 pct, total, ready, g_drive_count);
    } else {
        swprintf(left, 200, L"Up to date  ·  %zu files indexed across %zu volume%ls",
                 total, g_drive_count, g_drive_count == 1 ? L"" : L"s");
    }
    DrawTextW(hdc, left, -1, &footer, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    if (g_have_last_sync) {
        wchar_t right_text[160];
        swprintf(right_text, 160, L"Last sync: %02d:%02d:%02d  ·  Esc hide  ·  Enter open",
                 g_last_sync_time.wHour, g_last_sync_time.wMinute, g_last_sync_time.wSecond);
        DrawTextW(hdc, right_text, -1, &footer, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    } else {
        DrawTextW(hdc, L"Esc hide  ·  Enter open  ·  Ctrl+Alt+J summon", -1, &footer,
                  DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }

    SelectObject(hdc, old_f);
}

static void layout_controls(int W, int H) {
    int body_top = HEADER_H;
    int body_bot = H - FOOTER_H;

    SetWindowPos(g_lbl_brand, NULL, HPAD, 18, 200, 28, SWP_NOZORDER);
    int sx = LEFT_W + HPAD;
    int sw = (W - RIGHT_W - LEFT_W) - 2 * HPAD;
    SetWindowPos(g_global_edit, NULL, sx, 16, sw, 32, SWP_NOZORDER);

    SetWindowPos(g_btn_settings, NULL, W - HPAD - 110, 18, 110, 28, SWP_NOZORDER);

    SetWindowPos(g_lbl_volumes, NULL, HPAD, body_top + HPAD - 4, 200, 22, SWP_NOZORDER);
    int vol_top = body_top + HPAD + 22;
    int vol_bot_pad = 96;
    SetWindowPos(g_volumes, NULL, HPAD, vol_top,
                 LEFT_W - 2 * HPAD, body_bot - vol_top - vol_bot_pad, SWP_NOZORDER);
    SetWindowPos(g_btn_manage, NULL, HPAD, body_bot - 60, LEFT_W - 2 * HPAD, 32, SWP_NOZORDER);

    int cleft = LEFT_W + HPAD;
    int cright = W - RIGHT_W - HPAD;
    int ctop = body_top + HPAD + 28 + 8;
    SetWindowPos(g_refine_edit, NULL, cleft, ctop, cright - cleft, 28, SWP_NOZORDER);
    int results_top = ctop + 28 + 12;
    int results_w = (cright - cleft) + 8;
    SetWindowPos(g_results, NULL, cleft - 4, results_top,
                 results_w, body_bot - results_top - HPAD, SWP_NOZORDER);

    int size_w = 100;
    int date_w = 150;
    int avail  = results_w - size_w - date_w;
    if (avail < 200) avail = 200;
    int name_w = avail * 4 / 10;
    int path_w = avail - name_w;
    if (name_w < 120) name_w = 120;
    if (path_w < 120) path_w = 120;
    SendMessageW(g_results, LVM_SETCOLUMNWIDTH, 0, name_w);
    SendMessageW(g_results, LVM_SETCOLUMNWIDTH, 1, path_w);
    SendMessageW(g_results, LVM_SETCOLUMNWIDTH, 2, size_w);
    SendMessageW(g_results, LVM_SETCOLUMNWIDTH, 3, date_w);

    int rx = W - RIGHT_W + HPAD;
    int rw = RIGHT_W - 2 * HPAD;
    int ry = body_top + HPAD;

    SetWindowPos(g_lbl_filetype, NULL, rx, ry, rw, 22, SWP_NOZORDER); ry += 28;
    int btn_w = (rw - 12) / 5;
    SetWindowPos(g_btn_ft_pdf,  NULL, rx + 0*(btn_w + 3), ry, btn_w, 28, SWP_NOZORDER);
    SetWindowPos(g_btn_ft_docx, NULL, rx + 1*(btn_w + 3), ry, btn_w, 28, SWP_NOZORDER);
    SetWindowPos(g_btn_ft_zip,  NULL, rx + 2*(btn_w + 3), ry, btn_w, 28, SWP_NOZORDER);
    SetWindowPos(g_btn_ft_img,  NULL, rx + 3*(btn_w + 3), ry, btn_w, 28, SWP_NOZORDER);
    SetWindowPos(g_btn_ft_video,NULL, rx + 4*(btn_w + 3), ry, btn_w, 28, SWP_NOZORDER);
    ry += 36 + 12;

    SetWindowPos(g_lbl_date, NULL, rx, ry, rw, 22, SWP_NOZORDER); ry += 28;
    SetWindowPos(g_combo_date, NULL, rx, ry, rw, 28, SWP_NOZORDER); ry += 36 + 12;

    SetWindowPos(g_lbl_size, NULL, rx, ry, rw, 22, SWP_NOZORDER); ry += 28;
    SetWindowPos(g_size_input, NULL, rx, ry, rw, 28, SWP_NOZORDER); ry += 36 + 12;

    SetWindowPos(g_lbl_tags, NULL, rx, ry, rw, 22, SWP_NOZORDER); ry += 28;
    SetWindowPos(g_tags_input, NULL, rx, ry, rw, 28, SWP_NOZORDER);
}

static void format_volume_status(const CoinDrive *d, wchar_t *out, size_t cap) {
    wchar_t status_buf[48];
    if (!d->ready) {
        wcscpy_s(status_buf, ARRAYSIZE(status_buf), L"Indexing…");
    } else if (d->idx.count == 0) {
        wcscpy_s(status_buf, ARRAYSIZE(status_buf), L"Empty");
    } else {
        DWORD elapsed = d->ready_ms - d->start_ms;
        if (d->loaded_from_cache) {
            swprintf(status_buf, ARRAYSIZE(status_buf), L"Loaded %u ms · %zu", elapsed, d->idx.count);
        } else if (elapsed >= 1000) {
            swprintf(status_buf, ARRAYSIZE(status_buf), L"Indexed %.1fs · %zu", elapsed / 1000.0, d->idx.count);
        } else {
            swprintf(status_buf, ARRAYSIZE(status_buf), L"Indexed %u ms · %zu", elapsed, d->idx.count);
        }
    }
    if (d->total_bytes) {
        int pct = (int)(100.0 *
            ((double)(d->total_bytes - d->free_bytes) / (double)d->total_bytes));
        swprintf(out, cap, L"%ls · %d%% full", status_buf, pct);
    } else {
        swprintf(out, cap, L"%ls", status_buf);
    }
}

static void populate_volumes_list(void) {
    SendMessageW(g_volumes, LVM_DELETEALLITEMS, 0, 0);
    for (size_t i = 0; i < g_drive_count; ++i) {
        const CoinDrive *d = &g_drives[i];
        wchar_t col0[96], col1[80];
        swprintf(col0, 96, L"%lc:  %.40ls", d->idx.drive_letter, d->label);
        format_volume_status(d, col1, 80);

        LVITEMW it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.iSubItem = 0;
        it.pszText = col0;
        SendMessageW(g_volumes, LVM_INSERTITEMW, 0, (LPARAM)&it);
        ListView_SetItemText(g_volumes, (int)i, 1, col1);

        ListView_SetCheckState(g_volumes, (int)i, d->enabled ? TRUE : FALSE);
    }
}

static void refresh_volume_status(size_t i) {
    if (i >= g_drive_count) return;
    const CoinDrive *d = &g_drives[i];
    wchar_t col0[96], col1[80];
    swprintf(col0, 96, L"%lc:  %.40ls", d->idx.drive_letter, d->label);
    format_volume_status(d, col1, 80);
    ListView_SetItemText(g_volumes, (int)i, 0, col0);
    ListView_SetItemText(g_volumes, (int)i, 1, col1);
}

static void rebuild_indexes_now(void) {
    if (g_drive_count == 0) return;

    wchar_t prompt[MAX_PATH + 384];
    swprintf(prompt, MAX_PATH + 384,
             L"Rebuild indexes now?\n\n"
             L"This drops the in-memory index, deletes cached index files in:\n%ls\n\n"
             L"and re-enumerates the MFT for every volume. Search will return "
             L"no results until indexing finishes.",
             g_cache_dir[0] ? g_cache_dir : L"(unset)");
    if (MessageBoxW(g_main, prompt, L"Coincidence — Rebuild indexes",
                    MB_YESNO | MB_ICONWARNING) != IDYES) return;

    HCURSOR old_cursor = SetCursor(LoadCursorW(NULL, IDC_WAIT));

    for (size_t i = 0; i < g_drive_count; ++i) {
        InterlockedExchange(&g_drives[i].jrn.stop, 1);
        if (g_drives[i].thread) CancelSynchronousIo(g_drives[i].thread);
        coin_journal_signal_stop(&g_drives[i].jrn);
    }

    HANDLE pending[MAX_DRIVES];
    DWORD npending = 0;
    for (size_t i = 0; i < g_drive_count; ++i) {
        if (g_drives[i].thread) pending[npending++] = g_drives[i].thread;
    }
    if (npending > 0) {
        WaitForMultipleObjects(npending, pending, TRUE, 10000);
    }

    for (size_t i = 0; i < g_drive_count; ++i) {
        InterlockedExchange(&g_drives[i].ready, 0);
    }
    InterlockedIncrement(&g_search_version);

    for (size_t i = 0; i < g_drive_count; ++i) {
        AcquireSRWLockExclusive(&g_drives[i].idx.lock);
        ReleaseSRWLockExclusive(&g_drives[i].idx.lock);
    }

    wchar_t letters[MAX_DRIVES];
    for (size_t i = 0; i < g_drive_count; ++i) {
        letters[i] = g_drives[i].idx.drive_letter;
        if (g_drives[i].thread) {
            CloseHandle(g_drives[i].thread);
            g_drives[i].thread = NULL;
        }
        coin_mft_close(&g_drives[i].idx);
    }

    coin_results_free(&g_results_data);
    SendMessageW(g_results, LVM_SETITEMCOUNT, 0, 0);
    InvalidateRect(g_results, NULL, TRUE);

    if (g_cache_dir[0]) {
        wchar_t pattern[MAX_PATH + 32];
        swprintf(pattern, MAX_PATH + 32, L"%ls\\index_*.bin", g_cache_dir);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                wchar_t fp[MAX_PATH + 64];
                swprintf(fp, MAX_PATH + 64, L"%ls\\%ls", g_cache_dir, fd.cFileName);
                DeleteFileW(fp);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    InterlockedExchange(&g_skip_cache_save, 0);

    for (size_t i = 0; i < g_drive_count; ++i) {
        g_drives[i].idx.drive_letter = letters[i];
        InitializeSRWLock(&g_drives[i].idx.lock);
        g_drives[i].start_ms = 0;
        g_drives[i].ready_ms = 0;
        g_drives[i].loaded_from_cache = FALSE;
        memset(&g_drives[i].jrn, 0, sizeof(g_drives[i].jrn));
    }

    populate_volumes_list();
    invalidate_chrome();

    for (size_t i = 0; i < g_drive_count; ++i) {
        g_drives[i].thread = CreateThread(NULL, 0, worker_thread_proc,
                                          (LPVOID)(uintptr_t)i, 0, NULL);
    }

    SetCursor(old_cursor);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_msg_show != 0 && msg == g_msg_show) {
        show_and_focus();
        return 0;
    }
    switch (msg) {
        case WM_CREATE: {
            INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
            InitCommonControlsEx(&icc);

            g_brush_bg    = CreateSolidBrush(COL_BG);
            g_brush_panel = CreateSolidBrush(COL_PANEL);
            g_brush_input = CreateSolidBrush(COL_INPUT);

            g_font_brand  = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_font_search = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_font_head   = CreateFontW(17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_font_name   = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_font_path   = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_font_status = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_font_label  = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            g_lbl_brand = CreateWindowExW(0, L"STATIC", L"Coincidence",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            SendMessageW(g_lbl_brand, WM_SETFONT, (WPARAM)g_font_brand, TRUE);

            g_global_edit = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_GLOBAL_EDIT, NULL, NULL);
            SendMessageW(g_global_edit, WM_SETFONT, (WPARAM)g_font_search, TRUE);
            SendMessageW(g_global_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(10, 10));
            SendMessageW(g_global_edit, EM_SETCUEBANNER, TRUE,
                         (LPARAM)L"Search all volumes…");
            SetWindowSubclass(g_global_edit, edit_subclass_proc, 1, 0);

            g_btn_settings = CreateWindowExW(0, L"BUTTON", L"Settings",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_BTN_SETTINGS, NULL, NULL);
            SendMessageW(g_btn_settings, WM_SETFONT, (WPARAM)g_font_label, TRUE);

            g_lbl_volumes = CreateWindowExW(0, L"STATIC", L"VOLUMES & SOURCES",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            SendMessageW(g_lbl_volumes, WM_SETFONT, (WPARAM)g_font_label, TRUE);

            g_volumes = CreateWindowExW(0, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_VOLUMES, NULL, NULL);
            SendMessageW(g_volumes, LVM_SETEXTENDEDLISTVIEWSTYLE,
                         LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES,
                         LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
            SetWindowTheme(g_volumes, L"DarkMode_Explorer", NULL);
            ListView_SetBkColor(g_volumes, COL_PANEL);
            ListView_SetTextBkColor(g_volumes, COL_PANEL);
            ListView_SetTextColor(g_volumes, COL_TEXT);

            LVCOLUMNW vcol;
            memset(&vcol, 0, sizeof(vcol));
            vcol.mask = LVCF_WIDTH | LVCF_TEXT;
            vcol.cx = 140; vcol.pszText = L"Drive";
            SendMessageW(g_volumes, LVM_INSERTCOLUMNW, 0, (LPARAM)&vcol);
            vcol.cx = 120; vcol.pszText = L"Status";
            SendMessageW(g_volumes, LVM_INSERTCOLUMNW, 1, (LPARAM)&vcol);

            g_btn_manage = CreateWindowExW(0, L"BUTTON", L"Manage Indexing…",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_BTN_MANAGE, NULL, NULL);
            SendMessageW(g_btn_manage, WM_SETFONT, (WPARAM)g_font_label, TRUE);

            g_refine_edit = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_REFINE_EDIT, NULL, NULL);
            SendMessageW(g_refine_edit, WM_SETFONT, (WPARAM)g_font_search, TRUE);
            SendMessageW(g_refine_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(8, 8));
            SendMessageW(g_refine_edit, EM_SETCUEBANNER, TRUE,
                         (LPARAM)L"Filter results (coming soon)");
            EnableWindow(g_refine_edit, FALSE);

            g_results = CreateWindowExW(0, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA |
                LVS_SHOWSELALWAYS,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_RESULTS, NULL, NULL);
            SendMessageW(g_results, LVM_SETEXTENDEDLISTVIEWSTYLE,
                         LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                         LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            SetWindowTheme(g_results, L"DarkMode_Explorer", NULL);
            HWND hdr = (HWND)SendMessageW(g_results, LVM_GETHEADER, 0, 0);
            if (hdr) SetWindowTheme(hdr, L"ItemsView", NULL);
            ListView_SetBkColor(g_results, COL_PANEL);
            ListView_SetTextBkColor(g_results, COL_PANEL);
            ListView_SetTextColor(g_results, COL_TEXT);
            SetWindowSubclass(g_results, list_subclass_proc, 1, 0);

            HIMAGELIST il = ImageList_Create(1, ROW_H, ILC_COLOR32, 1, 1);
            SendMessageW(g_results, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)il);

            LVCOLUMNW rcol;
            memset(&rcol, 0, sizeof(rcol));
            rcol.mask = LVCF_WIDTH | LVCF_TEXT;
            rcol.cx = 280; rcol.pszText = L"Name";
            SendMessageW(g_results, LVM_INSERTCOLUMNW, 0, (LPARAM)&rcol);
            rcol.cx = 380; rcol.pszText = L"Path";
            SendMessageW(g_results, LVM_INSERTCOLUMNW, 1, (LPARAM)&rcol);
            rcol.cx = 90;  rcol.pszText = L"Size"; rcol.mask |= LVCF_FMT; rcol.fmt = LVCFMT_RIGHT;
            SendMessageW(g_results, LVM_INSERTCOLUMNW, 2, (LPARAM)&rcol);
            rcol.cx = 140; rcol.pszText = L"Date modified"; rcol.fmt = LVCFMT_LEFT;
            SendMessageW(g_results, LVM_INSERTCOLUMNW, 3, (LPARAM)&rcol);

            g_lbl_filetype = CreateWindowExW(0, L"STATIC", L"FILE TYPES",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            SendMessageW(g_lbl_filetype, WM_SETFONT, (WPARAM)g_font_label, TRUE);

            g_btn_ft_pdf   = CreateWindowExW(0, L"BUTTON", L"PDF",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_FT_PDF, NULL, NULL);
            g_btn_ft_docx  = CreateWindowExW(0, L"BUTTON", L"DOCX",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_FT_DOCX, NULL, NULL);
            g_btn_ft_zip   = CreateWindowExW(0, L"BUTTON", L"ZIP",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_FT_ZIP, NULL, NULL);
            g_btn_ft_img   = CreateWindowExW(0, L"BUTTON", L"IMG",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_FT_IMG, NULL, NULL);
            g_btn_ft_video = CreateWindowExW(0, L"BUTTON", L"VIDEO",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_FT_VIDEO, NULL, NULL);
            HWND ft_buttons[5] = { g_btn_ft_pdf, g_btn_ft_docx, g_btn_ft_zip,
                                    g_btn_ft_img, g_btn_ft_video };
            for (int k = 0; k < 5; ++k) {
                SendMessageW(ft_buttons[k], WM_SETFONT, (WPARAM)g_font_label, TRUE);
            }

            g_lbl_date = CreateWindowExW(0, L"STATIC", L"DATE RANGE",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            SendMessageW(g_lbl_date, WM_SETFONT, (WPARAM)g_font_label, TRUE);
            g_combo_date = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_DATE_RANGE, NULL, NULL);
            SendMessageW(g_combo_date, WM_SETFONT, (WPARAM)g_font_label, TRUE);
            SendMessageW(g_combo_date, CB_ADDSTRING, 0, (LPARAM)L"Any time");
            SendMessageW(g_combo_date, CB_ADDSTRING, 0, (LPARAM)L"Last 24 hours");
            SendMessageW(g_combo_date, CB_ADDSTRING, 0, (LPARAM)L"Last 7 days");
            SendMessageW(g_combo_date, CB_ADDSTRING, 0, (LPARAM)L"Last 30 days");
            SendMessageW(g_combo_date, CB_ADDSTRING, 0, (LPARAM)L"This year");
            SendMessageW(g_combo_date, CB_SETCURSEL, 0, 0);

            g_lbl_size = CreateWindowExW(0, L"STATIC", L"SIZE",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            SendMessageW(g_lbl_size, WM_SETFONT, (WPARAM)g_font_label, TRUE);
            g_size_input = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_SIZE_INPUT, NULL, NULL);
            SendMessageW(g_size_input, WM_SETFONT, (WPARAM)g_font_label, TRUE);
            SendMessageW(g_size_input, EM_SETCUEBANNER, TRUE, (LPARAM)L">10MB or 1MB-50MB");

            g_lbl_tags = CreateWindowExW(0, L"STATIC", L"TAGS",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            SendMessageW(g_lbl_tags, WM_SETFONT, (WPARAM)g_font_label, TRUE);
            g_tags_input = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_TAGS_INPUT, NULL, NULL);
            SendMessageW(g_tags_input, WM_SETFONT, (WPARAM)g_font_label, TRUE);
            SendMessageW(g_tags_input, EM_SETCUEBANNER, TRUE, (LPARAM)L"e.g. report, q3");

            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
            DWORD corners = DWMWCP_ROUND;
            DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
            int backdrop = DWMSBT_MAINWINDOW;
            DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

            install_tray(hwnd);
            g_hotkey_registered = RegisterHotKey(hwnd, HOTKEY_SUMMON,
                                                 MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                                                 'J');

            wchar_t letters[MAX_DRIVES];
            g_drive_count = discover_ntfs_fixed_drives(letters, MAX_DRIVES);
            for (size_t i = 0; i < g_drive_count; ++i) {
                memset(&g_drives[i], 0, sizeof(g_drives[i]));
                g_drives[i].idx.drive_letter = letters[i];
                g_drives[i].enabled = TRUE;
                InitializeSRWLock(&g_drives[i].idx.lock);
                load_volume_metadata(&g_drives[i]);
            }
            populate_volumes_list();
            for (size_t i = 0; i < g_drive_count; ++i) {
                g_drives[i].thread = CreateThread(NULL, 0, worker_thread_proc,
                                                  (LPVOID)(uintptr_t)i, 0, NULL);
            }

            SetFocus(g_global_edit);
            return 0;
        }

        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            layout_controls(w, h);
            int rw_col = w - LEFT_W - RIGHT_W - 2 * HPAD + 8;
            if (rw_col < 200) rw_col = 200;
            SendMessageW(g_results, LVM_SETCOLUMNWIDTH, 0, (LPARAM)rw_col);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            paint_chrome(hdc, &rc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT*)lp;
            if (di->CtlType == ODT_BUTTON)        { draw_owner_button(di); return TRUE; }
            else if (di->CtlType == ODT_COMBOBOX) { draw_owner_combo(di);  return TRUE; }
            return FALSE;
        }

        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT*)lp;
            if (mi->CtlType == ODT_COMBOBOX) { mi->itemHeight = 24; return TRUE; }
            return FALSE;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, COL_TEXT);
            SetBkColor(hdc, COL_INPUT);
            SetBkMode(hdc, OPAQUE);
            return (LRESULT)g_brush_input;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wp;
            HWND ctl = (HWND)lp;
            wchar_t cls[16];
            cls[0] = 0;
            GetClassNameW(ctl, cls, 16);
            if (_wcsicmp(cls, L"Edit") == 0) {
                SetTextColor(hdc, COL_TEXT_DIM);
                SetBkColor(hdc, COL_INPUT);
                SetBkMode(hdc, OPAQUE);
                return (LRESULT)g_brush_input;
            }
            SetTextColor(hdc, COL_TEXT_HEAD);
            SetBkColor(hdc, COL_BG);
            SetBkMode(hdc, OPAQUE);
            return (LRESULT)g_brush_bg;
        }

        case WM_CTLCOLORBTN: {
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, COL_TEXT);
            SetBkColor(hdc, COL_PANEL);
            SetBkMode(hdc, OPAQUE);
            return (LRESULT)g_brush_panel;
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, COL_TEXT);
            SetBkColor(hdc, COL_PANEL);
            SetBkMode(hdc, OPAQUE);
            return (LRESULT)g_brush_panel;
        }

        case WM_COMMAND: {
            WORD code = HIWORD(wp);
            WORD id = LOWORD(wp);
            if (code == EN_CHANGE && id == ID_GLOBAL_EDIT) {
                SetTimer(hwnd, SEARCH_TIMER, SEARCH_DEBOUNCE_MS, NULL);
                return 0;
            }
            if (code == EN_CHANGE && (id == ID_SIZE_INPUT || id == ID_TAGS_INPUT)) {
                SetTimer(hwnd, SEARCH_TIMER, SEARCH_DEBOUNCE_MS, NULL);
                return 0;
            }
            if (code == CBN_SELCHANGE && id == ID_DATE_RANGE) {
                int sel = (int)SendMessageW(g_combo_date, CB_GETCURSEL, 0, 0);
                if (sel < 0) sel = 0;
                g_date_choice = sel;
                schedule_search();
                return 0;
            }
            if (code == BN_CLICKED) {
                unsigned bit = ft_bit_for_id(id);
                if (bit) {
                    g_ft_mask ^= bit;
                    HWND btn = (HWND)lp;
                    if (btn) InvalidateRect(btn, NULL, TRUE);
                    schedule_search();
                    return 0;
                }
            }
            if (code == 0 || code == BN_CLICKED) {
                switch (id) {
                    case IDM_TRAY_TOGGLE: toggle_window(); break;
                    case IDM_TRAY_QUIT:   DestroyWindow(hwnd); break;
                    case ID_BTN_SETTINGS:
                        show_settings_menu();
                        break;
                    case ID_BTN_MANAGE: {
                        wchar_t mmsg[MAX_PATH + 256];
                        swprintf(mmsg, MAX_PATH + 256,
                            L"Manage Indexing — toggle volumes in the list to enable or disable them.\n\n"
                            L"Cache folder:\n%ls\n\n"
                            L"Use Settings (top-right) to change the folder or purge cache files.",
                            g_cache_dir[0] ? g_cache_dir : L"(unset)");
                        MessageBoxW(hwnd, mmsg, L"Coincidence", MB_OK | MB_ICONINFORMATION);
                        break;
                    }
                }
            }
            return 0;
        }

        case WM_HOTKEY:
            if (wp == HOTKEY_SUMMON) toggle_window();
            return 0;

        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (!IsRectEmpty(&g_hidden_link_rect) && PtInRect(&g_hidden_link_rect, pt)) {
                g_show_hidden_only = !g_show_hidden_only;
                schedule_search();
                invalidate_chrome();
                return 0;
            }
            break;
        }

        case WM_SETCURSOR: {
            if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT
                && !IsRectEmpty(&g_hidden_link_rect)) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (PtInRect(&g_hidden_link_rect, pt)) {
                    SetCursor(LoadCursorW(NULL, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        }

        case WM_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
                toggle_window();
            } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                show_tray_menu(hwnd);
            }
            return 0;

        case WM_NOTIFY: {
            NMHDR *nm = (NMHDR*)lp;
            if (nm->idFrom == ID_RESULTS) {
                if (nm->code == NM_CUSTOMDRAW) {
                    return on_results_custom_draw((NMLVCUSTOMDRAW*)lp);
                }
                if (nm->code == LVN_GETDISPINFOW) {
                    NMLVDISPINFOW *di = (NMLVDISPINFOW*)lp;
                    di->item.pszText = L"";
                }
                if (nm->code == NM_DBLCLK) {
                    NMITEMACTIVATE *a = (NMITEMACTIVATE*)lp;
                    open_result(a->iItem);
                }
                if (nm->code == NM_RCLICK) {
                    NMITEMACTIVATE *a = (NMITEMACTIVATE*)lp;
                    if (a->iItem >= 0) {
                        POINT pt = a->ptAction;
                        ClientToScreen(g_results, &pt);
                        show_results_context_menu(a->iItem, &pt);
                    }
                }
            } else if (nm->idFrom == ID_VOLUMES) {
                if (nm->code == NM_CUSTOMDRAW) {
                    return on_volumes_custom_draw((NMLVCUSTOMDRAW*)lp);
                }
                if (nm->code == LVN_ITEMCHANGED) {
                    NMLISTVIEW *lv = (NMLISTVIEW*)lp;
                    if (lv->uChanged & LVIF_STATE) {
                        UINT old_img = (lv->uOldState & LVIS_STATEIMAGEMASK) >> 12;
                        UINT new_img = (lv->uNewState & LVIS_STATEIMAGEMASK) >> 12;
                        if (old_img != new_img && lv->iItem >= 0
                            && (size_t)lv->iItem < g_drive_count) {
                            BOOL on = (new_img == 2);
                            if (g_drives[lv->iItem].enabled != on) {
                                g_drives[lv->iItem].enabled = on;
                                schedule_search();
                                invalidate_chrome();
                            }
                        }
                    }
                }
            }
            return 0;
        }

        case WM_INDEX_DONE: {
            size_t di = (size_t)wp;
            if (di < g_drive_count) {
                refresh_volume_status(di);
                GetLocalTime(&g_last_sync_time);
                g_have_last_sync = TRUE;
            }
            if (is_window_active()) {
                schedule_search();
                invalidate_chrome();
            } else {
                InterlockedExchange(&g_dirty_pending, 1);
            }
            return 0;
        }

        case WM_INDEX_DIRTY:
            if (is_window_active()) {
                schedule_refresh();
            } else {
                InterlockedExchange(&g_dirty_pending, 1);
            }
            return 0;

        case WM_SEARCH_DONE:
            apply_pending_results((LONG)wp);
            return 0;

        case WM_TIMER:
            if (wp == REFRESH_TIMER) {
                KillTimer(hwnd, REFRESH_TIMER);
                schedule_search();
                GetLocalTime(&g_last_sync_time);
                g_have_last_sync = TRUE;
                invalidate_chrome();
            } else if (wp == SEARCH_TIMER) {
                KillTimer(hwnd, SEARCH_TIMER);
                schedule_search();
            }
            return 0;

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            set_background_mode(TRUE);
            if (!g_balloon_shown) {
                g_balloon_shown = TRUE;
                NOTIFYICONDATAW nb;
                memset(&nb, 0, sizeof(nb));
                nb.cbSize = sizeof(nb);
                nb.hWnd = hwnd;
                nb.uID = 1;
                nb.uFlags = NIF_INFO;
                nb.dwInfoFlags = NIIF_INFO;
                wcscpy_s(nb.szInfoTitle, ARRAYSIZE(nb.szInfoTitle), L"Coincidence");
                wcscpy_s(nb.szInfo, ARRAYSIZE(nb.szInfo),
                         L"Still running in the tray. Right-click the icon and choose Quit "
                         L"to exit fully. Press Ctrl+Alt+J to summon.");
                Shell_NotifyIconW(NIM_MODIFY, &nb);
            }
            return 0;

        case WM_DESTROY: {
            if (g_hotkey_registered) UnregisterHotKey(hwnd, HOTKEY_SUMMON);
            Shell_NotifyIconW(NIM_DELETE, &g_tray);

            for (size_t i = 0; i < g_drive_count; ++i) {
                InterlockedExchange(&g_drives[i].jrn.stop, 1);
                if (g_drives[i].thread) CancelSynchronousIo(g_drives[i].thread);
                coin_journal_signal_stop(&g_drives[i].jrn);
            }

            HANDLE pending[MAX_DRIVES];
            DWORD npending = 0;
            for (size_t i = 0; i < g_drive_count; ++i) {
                if (g_drives[i].thread) pending[npending++] = g_drives[i].thread;
            }
            if (npending > 0) {
                WaitForMultipleObjects(npending, pending, TRUE, 2000);
            }
            for (size_t i = 0; i < g_drive_count; ++i) {
                if (g_drives[i].thread) {
                    CloseHandle(g_drives[i].thread);
                    g_drives[i].thread = NULL;
                }
            }

            if (!g_skip_cache_save) {
                for (size_t i = 0; i < g_drive_count; ++i) {
                    if (!g_drives[i].ready || g_drives[i].idx.count == 0) continue;
                    wchar_t cp[MAX_PATH];
                    if (make_cache_path(g_drives[i].idx.drive_letter, cp, MAX_PATH)) {
                        coin_mft_save_cache(&g_drives[i].idx, cp);
                    }
                }
            }

            if (g_singleton_mutex) {
                ReleaseMutex(g_singleton_mutex);
                CloseHandle(g_singleton_mutex);
                g_singleton_mutex = NULL;
            }

            coin_results_free(&g_pending_results);

            if (g_brush_bg)    DeleteObject(g_brush_bg);
            if (g_brush_panel) DeleteObject(g_brush_panel);
            if (g_brush_input) DeleteObject(g_brush_input);
            if (g_font_brand)  DeleteObject(g_font_brand);
            if (g_font_search) DeleteObject(g_font_search);
            if (g_font_head)   DeleteObject(g_font_head);
            if (g_font_name)   DeleteObject(g_font_name);
            if (g_font_path)   DeleteObject(g_font_path);
            if (g_font_status) DeleteObject(g_font_status);
            if (g_font_label)  DeleteObject(g_font_label);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    resolve_cache_dir();
    load_forbidden_words();

    g_msg_show = RegisterWindowMessageW(L"CoincidenceShowMessage_v1");
    g_singleton_mutex = CreateMutexW(NULL, TRUE, L"Local\\CoincidenceSingleton_v1");
    DWORD mutex_err = GetLastError();
    if (g_singleton_mutex == NULL || mutex_err == ERROR_ALREADY_EXISTS) {
        if (g_msg_show) {
            PostMessageW(HWND_BROADCAST, g_msg_show, 0, 0);
        }
        if (g_singleton_mutex) {
            CloseHandle(g_singleton_mutex);
            g_singleton_mutex = NULL;
        }
        return 0;
    }

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"CoincidenceWnd";
    RegisterClassW(&wc);

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - WIN_W) / 2;
    int y = wa.top  + ((wa.bottom - wa.top) - WIN_H) / 2;

    g_main = CreateWindowExW(
        0,
        L"CoincidenceWnd", L"Coincidence — Universal File Search",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
        x, y, WIN_W, WIN_H,
        NULL, NULL, hi, NULL);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    coin_results_free(&g_results_data);
    for (size_t i = 0; i < g_drive_count; ++i) {
        coin_mft_close(&g_drives[i].idx);
    }
    CoUninitialize();
    return 0;
}
