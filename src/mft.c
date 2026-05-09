#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mft.h"

#define USN_BUF_BYTES (1u << 16)

static const char COIN_CACHE_MAGIC[8] = { 'C','O','I','N','I','D','X','1' };
#define COIN_CACHE_VERSION 1u

#pragma pack(push, 1)
typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t entry_size;
    DWORDLONG journal_id;
    int64_t  next_usn;
    uint32_t drive_letter;
    uint32_t reserved;
    uint64_t count;
    uint64_t name_pool_used;
} CoinCacheHeader;
#pragma pack(pop)

static int ensure_entries_cap(CoinIndex *idx, size_t needed) {
    if (idx->capacity >= needed) return 1;
    size_t cap = idx->capacity ? idx->capacity * 2 : 65536;
    while (cap < needed) cap *= 2;
    CoinEntry *p = (CoinEntry*)realloc(idx->entries, cap * sizeof(CoinEntry));
    if (!p) return 0;
    idx->entries = p;
    idx->capacity = cap;
    return 1;
}

static int ensure_name_pool(CoinIndex *idx, size_t need_wchars) {
    if (idx->name_pool_used + need_wchars <= idx->name_pool_cap) return 1;
    size_t cap = idx->name_pool_cap ? idx->name_pool_cap * 2 : (1u << 20);
    while (cap < idx->name_pool_used + need_wchars) cap *= 2;
    wchar_t *p = (wchar_t*)realloc(idx->name_pool, cap * sizeof(wchar_t));
    if (!p) return 0;
    idx->name_pool = p;
    idx->name_pool_cap = cap;
    return 1;
}

static size_t mix64(DWORDLONG x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (size_t)x;
}

static int rebuild_table(CoinIndex *idx) {
    size_t buckets = 16;
    size_t target = idx->count + idx->count / 2 + 64;
    while (buckets < target) buckets *= 2;

    uint32_t *bi = (uint32_t*)malloc(buckets * sizeof(uint32_t));
    if (!bi) return 0;

    for (size_t i = 0; i < buckets; ++i) bi[i] = UINT32_MAX;
    size_t mask = buckets - 1;

    for (size_t i = 0; i < idx->count; ++i) {
        if (idx->entries[i].flags & COIN_ENTRY_DELETED) continue;
        DWORDLONG frn = idx->entries[i].frn;
        size_t h = mix64(frn) & mask;
        while (bi[h] != UINT32_MAX) h = (h + 1) & mask;
        bi[h] = (uint32_t)i;
    }

    free(idx->bucket_index);
    idx->bucket_index = bi;
    idx->bucket_count = buckets;
    return 1;
}

static int ensure_table_room(CoinIndex *idx) {
    if (idx->bucket_count == 0) return rebuild_table(idx);
    if ((idx->count + 1) * 10 > idx->bucket_count * 7) {
        return rebuild_table(idx);
    }
    return 1;
}

static void hashtable_insert(CoinIndex *idx, DWORDLONG frn, uint32_t entry_idx) {
    size_t mask = idx->bucket_count - 1;
    size_t h = mix64(frn) & mask;
    while (idx->bucket_index[h] != UINT32_MAX) {
        if (idx->entries[idx->bucket_index[h]].frn == frn) {
            idx->bucket_index[h] = entry_idx;
            return;
        }
        h = (h + 1) & mask;
    }
    idx->bucket_index[h] = entry_idx;
}

static int query_journal_state(HANDLE h, DWORDLONG *jid, USN *first_usn, USN *next_usn) {
    USN_JOURNAL_DATA_V0 jd;
    DWORD bytes = 0;
    if (!DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL,
                         NULL, 0, &jd, sizeof(jd), &bytes, NULL)) return 0;
    *jid = jd.UsnJournalID;
    *first_usn = jd.FirstUsn;
    *next_usn = jd.NextUsn;
    return 1;
}

int coin_mft_open(wchar_t drive_letter, CoinIndex *out) {
    memset(out, 0, sizeof(*out));
    out->drive_letter = drive_letter;
    InitializeSRWLock(&out->lock);

    wchar_t volume_path[8];
    swprintf(volume_path, 8, L"\\\\.\\%c:", drive_letter);

    HANDLE h = CreateFileW(
        volume_path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    out->volume = h;

    DWORDLONG jid = 0;
    USN first_usn = 0, next_usn = 0;
    if (!query_journal_state(h, &jid, &first_usn, &next_usn)) {
        DWORD err = GetLastError();
        if (err == ERROR_JOURNAL_NOT_ACTIVE || err == ERROR_INVALID_FUNCTION) {
            CREATE_USN_JOURNAL_DATA cd;
            memset(&cd, 0, sizeof(cd));
            DWORD bytes = 0;
            if (!DeviceIoControl(h, FSCTL_CREATE_USN_JOURNAL,
                                 &cd, sizeof(cd), NULL, 0, &bytes, NULL)) {
                CloseHandle(h); out->volume = NULL; return 0;
            }
            if (!query_journal_state(h, &jid, &first_usn, &next_usn)) {
                CloseHandle(h); out->volume = NULL; return 0;
            }
        } else {
            CloseHandle(h); out->volume = NULL; return 0;
        }
    }
    out->journal_id = jid;
    out->first_usn  = first_usn;
    out->next_usn   = next_usn;
    return 1;
}

int coin_mft_enumerate(CoinIndex *idx) {
    if (!idx->volume) return 0;

    MFT_ENUM_DATA_V1 med;
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = MAXLONGLONG;
    med.MinMajorVersion = 2;
    med.MaxMajorVersion = 2;

    BYTE *buf = (BYTE*)malloc(USN_BUF_BYTES);
    if (!buf) return 0;

    for (;;) {
        DWORD bytes_returned = 0;
        BOOL ok = DeviceIoControl(
            idx->volume, FSCTL_ENUM_USN_DATA,
            &med, sizeof(med),
            buf, USN_BUF_BYTES,
            &bytes_returned, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) break;
            free(buf);
            return 0;
        }
        if (bytes_returned < sizeof(DWORDLONG)) break;

        DWORDLONG next_frn = *(DWORDLONG*)buf;
        BYTE *p = buf + sizeof(DWORDLONG);
        BYTE *end = buf + bytes_returned;
        while (p < end) {
            USN_RECORD_V2 *r = (USN_RECORD_V2*)p;
            if (r->RecordLength == 0 || p + r->RecordLength > end) break;

            wchar_t *name = (wchar_t*)((BYTE*)r + r->FileNameOffset);
            uint16_t name_wchars = (uint16_t)(r->FileNameLength / sizeof(wchar_t));

            if (!ensure_entries_cap(idx, idx->count + 1)) goto fail;
            if (!ensure_name_pool(idx, name_wchars)) goto fail;

            CoinEntry *e = &idx->entries[idx->count++];
            e->frn = r->FileReferenceNumber;
            e->parent_frn = r->ParentFileReferenceNumber;
            e->attrs = r->FileAttributes;
            e->name_offset = (uint32_t)idx->name_pool_used;
            e->name_len = name_wchars;
            e->flags = 0;
            memcpy(idx->name_pool + idx->name_pool_used, name,
                   (size_t)name_wchars * sizeof(wchar_t));
            idx->name_pool_used += name_wchars;

            p += r->RecordLength;
        }
        med.StartFileReferenceNumber = next_frn;
    }

    free(buf);
    if (!rebuild_table(idx)) return 0;
    coin_mft_shrink_to_fit(idx);
    return 1;

fail:
    free(buf);
    return 0;
}

int coin_mft_shrink_to_fit(CoinIndex *idx) {
    if (!idx) return 0;
    if (idx->capacity > idx->count) {
        size_t want = idx->count ? idx->count : 1;
        CoinEntry *p = (CoinEntry*)realloc(idx->entries, want * sizeof(CoinEntry));
        if (p) { idx->entries = p; idx->capacity = idx->count; }
    }
    if (idx->name_pool_cap > idx->name_pool_used) {
        size_t want = idx->name_pool_used ? idx->name_pool_used : 1;
        wchar_t *p = (wchar_t*)realloc(idx->name_pool, want * sizeof(wchar_t));
        if (p) { idx->name_pool = p; idx->name_pool_cap = idx->name_pool_used; }
    }
    return 1;
}

size_t coin_mft_compact_waste(const CoinIndex *idx) {
    if (!idx) return 0;
    size_t entry_waste = idx->deleted_count * sizeof(CoinEntry);
    size_t pool_waste  = idx->orphan_chars * sizeof(wchar_t);
    return entry_waste + pool_waste;
}

int coin_mft_compact(CoinIndex *idx) {
    if (!idx) return 0;
    AcquireSRWLockExclusive(&idx->lock);

    size_t live_count = 0, live_chars = 0;
    for (size_t i = 0; i < idx->count; ++i) {
        const CoinEntry *e = &idx->entries[i];
        if (e->flags & COIN_ENTRY_DELETED) continue;
        live_count++;
        live_chars += e->name_len;
    }

    if (live_count == idx->count && live_chars == idx->name_pool_used) {
        idx->deleted_count = 0;
        idx->orphan_chars  = 0;
        ReleaseSRWLockExclusive(&idx->lock);
        return 1;
    }

    CoinEntry *new_entries = NULL;
    wchar_t   *new_pool    = NULL;
    if (live_count) {
        new_entries = (CoinEntry*)malloc(live_count * sizeof(CoinEntry));
        if (!new_entries) { ReleaseSRWLockExclusive(&idx->lock); return 0; }
    }
    if (live_chars) {
        new_pool = (wchar_t*)malloc(live_chars * sizeof(wchar_t));
        if (!new_pool) {
            free(new_entries);
            ReleaseSRWLockExclusive(&idx->lock);
            return 0;
        }
    }

    size_t out_idx = 0, out_pool = 0;
    for (size_t i = 0; i < idx->count; ++i) {
        const CoinEntry *e = &idx->entries[i];
        if (e->flags & COIN_ENTRY_DELETED) continue;
        CoinEntry *o = &new_entries[out_idx++];
        *o = *e;
        if (e->name_len) {
            memcpy(new_pool + out_pool,
                   idx->name_pool + e->name_offset,
                   (size_t)e->name_len * sizeof(wchar_t));
        }
        o->name_offset = (uint32_t)out_pool;
        out_pool += e->name_len;
    }

    free(idx->entries);
    free(idx->name_pool);
    idx->entries        = new_entries;
    idx->capacity       = live_count;
    idx->count          = live_count;
    idx->name_pool      = new_pool;
    idx->name_pool_cap  = live_chars;
    idx->name_pool_used = live_chars;
    idx->deleted_count  = 0;
    idx->orphan_chars   = 0;

    int ok = rebuild_table(idx);
    ReleaseSRWLockExclusive(&idx->lock);
    return ok;
}

size_t coin_mft_find(const CoinIndex *idx, DWORDLONG frn) {
    if (idx->bucket_count == 0) return SIZE_MAX;
    size_t mask = idx->bucket_count - 1;
    size_t h = mix64(frn) & mask;
    while (idx->bucket_index[h] != UINT32_MAX) {
        uint32_t ei = idx->bucket_index[h];
        if (idx->entries[ei].frn == frn) return ei;
        h = (h + 1) & mask;
    }
    return SIZE_MAX;
}

size_t coin_mft_path(const CoinIndex *idx, size_t entry_index, wchar_t *out_buf, size_t out_cap) {
    if (entry_index >= idx->count || out_cap < 4) return 0;

    enum { MAX_DEPTH = 64 };
    size_t stack[MAX_DEPTH];
    int depth = 0;
    size_t cur = entry_index;

    for (int safety = 0; safety < MAX_DEPTH * 4 && depth < MAX_DEPTH; ++safety) {
        stack[depth++] = cur;
        DWORDLONG parent_frn = idx->entries[cur].parent_frn;
        size_t parent_idx = coin_mft_find(idx, parent_frn);
        if (parent_idx == SIZE_MAX || parent_idx == cur) break;
        cur = parent_idx;
    }

    size_t pos = 0;
    if (pos + 3 >= out_cap) return 0;
    out_buf[pos++] = idx->drive_letter;
    out_buf[pos++] = L':';
    out_buf[pos++] = L'\\';

    for (int i = depth - 1; i >= 0; --i) {
        const CoinEntry *e = &idx->entries[stack[i]];
        const wchar_t *name = idx->name_pool + e->name_offset;
        if (pos + (size_t)e->name_len + 1 >= out_cap) return 0;
        memcpy(out_buf + pos, name, (size_t)e->name_len * sizeof(wchar_t));
        pos += e->name_len;
        if (i > 0) out_buf[pos++] = L'\\';
    }
    out_buf[pos] = 0;
    return pos;
}

void coin_mft_apply_record(CoinIndex *idx, USN_RECORD_V2 *r) {
    DWORD reason = r->Reason;
    DWORDLONG frn = r->FileReferenceNumber;

    const DWORD relevant =
        USN_REASON_FILE_CREATE |
        USN_REASON_FILE_DELETE |
        USN_REASON_RENAME_NEW_NAME |
        USN_REASON_HARD_LINK_CHANGE |
        USN_REASON_REPARSE_POINT_CHANGE;
    if (!(reason & relevant)) return;

    size_t i = coin_mft_find(idx, frn);

    if (reason & USN_REASON_FILE_DELETE) {
        if (i != SIZE_MAX) {
            CoinEntry *e = &idx->entries[i];
            if (!(e->flags & COIN_ENTRY_DELETED)) {
                e->flags |= COIN_ENTRY_DELETED;
                idx->deleted_count++;
                idx->orphan_chars += e->name_len;
            }
        }
        return;
    }

    wchar_t *name = (wchar_t*)((BYTE*)r + r->FileNameOffset);
    uint16_t name_wchars = (uint16_t)(r->FileNameLength / sizeof(wchar_t));

    if (i == SIZE_MAX) {
        if (!ensure_entries_cap(idx, idx->count + 1)) return;
        if (!ensure_name_pool(idx, name_wchars)) return;
        if (!ensure_table_room(idx)) return;

        uint32_t new_idx = (uint32_t)idx->count++;
        CoinEntry *e = &idx->entries[new_idx];
        e->frn = frn;
        e->parent_frn = r->ParentFileReferenceNumber;
        e->attrs = r->FileAttributes;
        e->name_offset = (uint32_t)idx->name_pool_used;
        e->name_len = name_wchars;
        e->flags = 0;
        memcpy(idx->name_pool + idx->name_pool_used, name,
               (size_t)name_wchars * sizeof(wchar_t));
        idx->name_pool_used += name_wchars;

        hashtable_insert(idx, frn, new_idx);
    } else {
        if (!ensure_name_pool(idx, name_wchars)) return;
        CoinEntry *e = &idx->entries[i];
        if (e->flags & COIN_ENTRY_DELETED) {
            if (idx->deleted_count) idx->deleted_count--;
            if (idx->orphan_chars >= e->name_len) idx->orphan_chars -= e->name_len;
            else idx->orphan_chars = 0;
        } else {
            idx->orphan_chars += e->name_len;
        }
        e->parent_frn = r->ParentFileReferenceNumber;
        e->attrs = r->FileAttributes;
        e->name_offset = (uint32_t)idx->name_pool_used;
        e->name_len = name_wchars;
        e->flags = (uint16_t)(e->flags & (uint16_t)(~COIN_ENTRY_DELETED & 0xFFFFu));
        memcpy(idx->name_pool + idx->name_pool_used, name,
               (size_t)name_wchars * sizeof(wchar_t));
        idx->name_pool_used += name_wchars;
    }
}

void coin_mft_close(CoinIndex *idx) {
    if (idx->volume) { CloseHandle(idx->volume); idx->volume = NULL; }
    free(idx->entries);
    free(idx->name_pool);
    free(idx->bucket_index);
    memset(idx, 0, sizeof(*idx));
}

void coin_mft_clear_entries(CoinIndex *idx) {
    free(idx->entries);      idx->entries = NULL;
    free(idx->name_pool);    idx->name_pool = NULL;
    free(idx->bucket_index); idx->bucket_index = NULL;
    idx->count = 0;
    idx->capacity = 0;
    idx->name_pool_used = 0;
    idx->name_pool_cap = 0;
    idx->bucket_count = 0;
    idx->deleted_count = 0;
    idx->orphan_chars = 0;
}

static int write_all(HANDLE h, const void *buf, size_t bytes) {
    const BYTE *p = (const BYTE*)buf;
    while (bytes) {
        DWORD chunk = (bytes > (1u << 30)) ? (1u << 30) : (DWORD)bytes;
        DWORD wrote = 0;
        if (!WriteFile(h, p, chunk, &wrote, NULL) || wrote != chunk) return 0;
        p += wrote;
        bytes -= wrote;
    }
    return 1;
}

static int read_all(HANDLE h, void *buf, size_t bytes) {
    BYTE *p = (BYTE*)buf;
    while (bytes) {
        DWORD chunk = (bytes > (1u << 30)) ? (1u << 30) : (DWORD)bytes;
        DWORD got = 0;
        if (!ReadFile(h, p, chunk, &got, NULL) || got != chunk) return 0;
        p += got;
        bytes -= got;
    }
    return 1;
}

int coin_mft_save_cache(const CoinIndex *idx, const wchar_t *path) {
    wchar_t tmp_path[MAX_PATH];
    int n = swprintf(tmp_path, MAX_PATH, L"%ls.tmp", path);
    if (n <= 0 || n >= MAX_PATH) return 0;

    HANDLE h = CreateFileW(tmp_path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    CoinCacheHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, COIN_CACHE_MAGIC, 8);
    hdr.version        = COIN_CACHE_VERSION;
    hdr.entry_size     = (uint32_t)sizeof(CoinEntry);
    hdr.journal_id     = idx->journal_id;
    hdr.next_usn       = idx->next_usn;
    hdr.drive_letter   = (uint32_t)idx->drive_letter;
    hdr.count          = (uint64_t)idx->count;
    hdr.name_pool_used = (uint64_t)idx->name_pool_used;

    int ok = write_all(h, &hdr, sizeof(hdr));
    if (ok && idx->count) {
        ok = write_all(h, idx->entries, idx->count * sizeof(CoinEntry));
    }
    if (ok && idx->name_pool_used) {
        ok = write_all(h, idx->name_pool, idx->name_pool_used * sizeof(wchar_t));
    }
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp_path); return 0; }

    if (!MoveFileExW(tmp_path, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp_path);
        return 0;
    }
    return 1;
}

int coin_mft_load_cache(CoinIndex *idx, const wchar_t *path,
                        DWORDLONG *out_saved_journal_id, USN *out_saved_next_usn) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    CoinCacheHeader hdr;
    if (!read_all(h, &hdr, sizeof(hdr))) { CloseHandle(h); return 0; }
    if (memcmp(hdr.magic, COIN_CACHE_MAGIC, 8) != 0 ||
        hdr.version != COIN_CACHE_VERSION ||
        hdr.entry_size != (uint32_t)sizeof(CoinEntry) ||
        (wchar_t)hdr.drive_letter != idx->drive_letter) {
        CloseHandle(h);
        return 0;
    }

    size_t count = (size_t)hdr.count;
    size_t pool  = (size_t)hdr.name_pool_used;

    CoinEntry *entries = NULL;
    wchar_t   *name_pool = NULL;
    if (count) {
        entries = (CoinEntry*)malloc(count * sizeof(CoinEntry));
        if (!entries) { CloseHandle(h); return 0; }
    }
    if (pool) {
        name_pool = (wchar_t*)malloc(pool * sizeof(wchar_t));
        if (!name_pool) { free(entries); CloseHandle(h); return 0; }
    }

    int ok = 1;
    if (count) ok = read_all(h, entries, count * sizeof(CoinEntry));
    if (ok && pool) ok = read_all(h, name_pool, pool * sizeof(wchar_t));
    CloseHandle(h);
    if (!ok) { free(entries); free(name_pool); return 0; }

    coin_mft_clear_entries(idx);
    idx->entries        = entries;
    idx->capacity       = count;
    idx->count          = count;
    idx->name_pool      = name_pool;
    idx->name_pool_cap  = pool;
    idx->name_pool_used = pool;

    if (!rebuild_table(idx)) {
        coin_mft_clear_entries(idx);
        return 0;
    }

    if (out_saved_journal_id) *out_saved_journal_id = hdr.journal_id;
    if (out_saved_next_usn)   *out_saved_next_usn   = (USN)hdr.next_usn;
    return 1;
}
