#include "search.h"
#include <stdlib.h>
#include <string.h>

static int contains_ci_lowered(const wchar_t *hay, size_t hay_len,
                                const wchar_t *needle_lower, size_t need_len,
                                wchar_t first_l, wchar_t first_u) {
    if (need_len == 0) return 1;
    if (need_len > hay_len) return 0;
    size_t last = hay_len - need_len;
    for (size_t i = 0; i <= last; ++i) {
        wchar_t c = hay[i];
        if (c != first_l && c != first_u) continue;
        size_t j = 1;
        for (; j < need_len; ++j) {
            wchar_t a = hay[i + j];
            if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + 32);
            if (a != needle_lower[j]) break;
        }
        if (j == need_len) return 1;
    }
    return 0;
}

typedef struct {
    wchar_t *buf;
    size_t  *offsets;
    size_t  *lens;
    size_t   count;
} ForbiddenList;

static ForbiddenList g_forbidden;
static SRWLOCK       g_forbidden_lock = SRWLOCK_INIT;

void coin_search_set_forbidden(const wchar_t *const *words, size_t count) {
    size_t total_chars = 0;
    for (size_t i = 0; i < count; ++i) total_chars += wcslen(words[i]);

    wchar_t *buf = NULL;
    size_t  *offsets = NULL;
    size_t  *lens = NULL;

    if (count > 0) {
        size_t alloc_chars = total_chars > 0 ? total_chars : 1;
        buf     = (wchar_t*)malloc(alloc_chars * sizeof(wchar_t));
        offsets = (size_t*)malloc(count * sizeof(size_t));
        lens    = (size_t*)malloc(count * sizeof(size_t));
        if (!buf || !offsets || !lens) {
            free(buf); free(offsets); free(lens);
            return;
        }
        size_t pos = 0;
        for (size_t i = 0; i < count; ++i) {
            size_t n = wcslen(words[i]);
            for (size_t k = 0; k < n; ++k) {
                wchar_t c = words[i][k];
                if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
                buf[pos + k] = c;
            }
            offsets[i] = pos;
            lens[i]    = n;
            pos += n;
        }
    }

    AcquireSRWLockExclusive(&g_forbidden_lock);
    free(g_forbidden.buf);
    free(g_forbidden.offsets);
    free(g_forbidden.lens);
    g_forbidden.buf     = buf;
    g_forbidden.offsets = offsets;
    g_forbidden.lens    = lens;
    g_forbidden.count   = count;
    ReleaseSRWLockExclusive(&g_forbidden_lock);
}

static int component_has_any_forbidden(const wchar_t *name, size_t name_len) {
    for (size_t f = 0; f < g_forbidden.count; ++f) {
        const wchar_t *w = g_forbidden.buf + g_forbidden.offsets[f];
        size_t wn = g_forbidden.lens[f];
        if (wn == 0 || wn > name_len) continue;
        wchar_t first_l = w[0];
        wchar_t first_u = (first_l >= L'a' && first_l <= L'z')
                          ? (wchar_t)(first_l - 32) : first_l;
        size_t last = name_len - wn;
        for (size_t i = 0; i <= last; ++i) {
            wchar_t c = name[i];
            if (c != first_l && c != first_u) continue;
            size_t j = 1;
            for (; j < wn; ++j) {
                wchar_t a = name[i + j];
                if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + 32);
                if (a != w[j]) break;
            }
            if (j == wn) return 1;
        }
    }
    return 0;
}

static int path_has_forbidden(const CoinIndex *idx, size_t entry_index) {
    enum { MAX_DEPTH = 64 };
    size_t cur = entry_index;
    for (int safety = 0; safety < MAX_DEPTH * 4; ++safety) {
        const CoinEntry *e = &idx->entries[cur];
        const wchar_t *name = idx->name_pool + e->name_offset;
        if (component_has_any_forbidden(name, e->name_len)) return 1;
        size_t parent = coin_mft_find(idx, e->parent_frn);
        if (parent == SIZE_MAX || parent == cur) break;
        cur = parent;
    }
    return 0;
}

static int name_ext_matches(const wchar_t *name, size_t name_len, unsigned ext_mask) {
    if (!ext_mask) return 1;
    size_t dot = name_len;
    for (size_t i = name_len; i > 0; --i) {
        if (name[i - 1] == L'.') { dot = i; break; }
        if (name[i - 1] == L'\\' || name[i - 1] == L'/') break;
    }
    if (dot >= name_len) return 0;
    size_t ext_len = name_len - dot;
    if (ext_len == 0 || ext_len > 5) return 0;
    wchar_t ext[6];
    for (size_t i = 0; i < ext_len; ++i) {
        wchar_t c = name[dot + i];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
        ext[i] = c;
    }
    ext[ext_len] = 0;

    static const struct { const wchar_t *e; unsigned bit; } table[] = {
        { L"pdf",  COIN_EXT_PDF },
        { L"doc",  COIN_EXT_DOC },
        { L"docx", COIN_EXT_DOC },
        { L"odt",  COIN_EXT_DOC },
        { L"rtf",  COIN_EXT_DOC },
        { L"txt",  COIN_EXT_DOC },
        { L"zip",  COIN_EXT_ZIP },
        { L"rar",  COIN_EXT_ZIP },
        { L"7z",   COIN_EXT_ZIP },
        { L"tar",  COIN_EXT_ZIP },
        { L"gz",   COIN_EXT_ZIP },
        { L"jpg",  COIN_EXT_IMG },
        { L"jpeg", COIN_EXT_IMG },
        { L"png",  COIN_EXT_IMG },
        { L"gif",  COIN_EXT_IMG },
        { L"bmp",  COIN_EXT_IMG },
        { L"webp", COIN_EXT_IMG },
        { L"tif",  COIN_EXT_IMG },
        { L"tiff", COIN_EXT_IMG },
        { L"mp4",  COIN_EXT_VIDEO },
        { L"mkv",  COIN_EXT_VIDEO },
        { L"avi",  COIN_EXT_VIDEO },
        { L"mov",  COIN_EXT_VIDEO },
        { L"webm", COIN_EXT_VIDEO },
        { L"wmv",  COIN_EXT_VIDEO },
        { L"flv",  COIN_EXT_VIDEO },
    };
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); ++i) {
        if (!(ext_mask & table[i].bit)) continue;
        if (wcscmp(ext, table[i].e) == 0) return 1;
    }
    return 0;
}

#define COIN_MAX_TAGS 8

typedef struct {
    wchar_t *buf;
    size_t   offsets[COIN_MAX_TAGS];
    size_t   lens[COIN_MAX_TAGS];
    size_t   count;
} TagList;

static void taglist_init(TagList *t, const wchar_t *src) {
    t->buf = NULL;
    t->count = 0;
    if (!src || !*src) return;
    size_t n = wcslen(src);
    t->buf = (wchar_t*)malloc(n * sizeof(wchar_t));
    if (!t->buf) return;
    size_t pos = 0;
    size_t i = 0;
    while (i < n && t->count < COIN_MAX_TAGS) {
        while (i < n && (src[i] == L' ' || src[i] == L'\t' ||
                         src[i] == L',' || src[i] == L';')) i++;
        if (i >= n) break;
        size_t start = pos;
        while (i < n && src[i] != L' ' && src[i] != L'\t' &&
                        src[i] != L',' && src[i] != L';') {
            wchar_t c = src[i++];
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
            t->buf[pos++] = c;
        }
        size_t len = pos - start;
        if (len > 0) {
            t->offsets[t->count] = start;
            t->lens[t->count] = len;
            t->count++;
        }
    }
}

static void taglist_free(TagList *t) {
    free(t->buf);
    t->buf = NULL;
    t->count = 0;
}

static int component_contains(const wchar_t *name, size_t name_len,
                              const wchar_t *tok_lower, size_t tok_len) {
    if (tok_len == 0 || tok_len > name_len) return 0;
    wchar_t first_l = tok_lower[0];
    wchar_t first_u = (first_l >= L'a' && first_l <= L'z')
                      ? (wchar_t)(first_l - 32) : first_l;
    size_t last = name_len - tok_len;
    for (size_t i = 0; i <= last; ++i) {
        wchar_t c = name[i];
        if (c != first_l && c != first_u) continue;
        size_t j = 1;
        for (; j < tok_len; ++j) {
            wchar_t a = name[i + j];
            if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + 32);
            if (a != tok_lower[j]) break;
        }
        if (j == tok_len) return 1;
    }
    return 0;
}

static int path_matches_all_tags(const CoinIndex *idx, size_t entry_index,
                                 const TagList *tags) {
    if (tags->count == 0) return 1;
    unsigned all = (tags->count >= 32) ? 0xFFFFFFFFu : ((1u << tags->count) - 1u);
    unsigned found = 0;

    enum { MAX_DEPTH = 64 };
    size_t cur = entry_index;
    for (int safety = 0; safety < MAX_DEPTH * 4; ++safety) {
        const CoinEntry *e = &idx->entries[cur];
        const wchar_t *name = idx->name_pool + e->name_offset;
        for (size_t k = 0; k < tags->count; ++k) {
            unsigned bit = 1u << k;
            if (found & bit) continue;
            if (component_contains(name, e->name_len,
                                   tags->buf + tags->offsets[k],
                                   tags->lens[k])) {
                found |= bit;
            }
        }
        if (found == all) return 1;
        size_t parent = coin_mft_find(idx, e->parent_frn);
        if (parent == SIZE_MAX || parent == cur) break;
        cur = parent;
    }
    return found == all;
}

void coin_search_all(CoinIndex **indices, size_t n_indices,
                     const wchar_t *needle,
                     const CoinPrefilter *pre,
                     size_t max_results,
                     unsigned flags,
                     CoinResults *out) {
    out->hits = NULL;
    out->count = 0;
    out->capacity = 0;
    out->total_matched = 0;
    out->total_hidden = 0;
    int hidden_only = (flags & COIN_SEARCH_HIDDEN_ONLY) != 0;

    if (max_results == 0) return;

    wchar_t needle_lower[260];
    size_t need_len = 0;
    while (needle[need_len] && need_len < 259) {
        wchar_t c = needle[need_len];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
        needle_lower[need_len] = c;
        need_len++;
    }
    needle_lower[need_len] = 0;

    wchar_t first_l = need_len ? needle_lower[0] : 0;
    wchar_t first_u = first_l;
    if (first_l >= L'a' && first_l <= L'z') first_u = (wchar_t)(first_l - 32);

    out->hits = (CoinHit*)malloc(max_results * sizeof(CoinHit));
    if (!out->hits) return;
    out->capacity = max_results;

    unsigned ext_mask = pre ? pre->ext_mask : 0u;
    TagList tags;
    taglist_init(&tags, pre ? pre->tags : NULL);

    AcquireSRWLockShared(&g_forbidden_lock);
    int has_forbidden = (g_forbidden.count > 0);

    for (size_t d = 0; d < n_indices; ++d) {
        CoinIndex *idx = indices[d];
        if (!idx) continue;
        AcquireSRWLockShared(&idx->lock);
        for (size_t i = 0; i < idx->count; ++i) {
            const CoinEntry *e = &idx->entries[i];
            if (e->flags & COIN_ENTRY_DELETED) continue;
            const wchar_t *name = idx->name_pool + e->name_offset;
            if (!contains_ci_lowered(name, e->name_len,
                                     needle_lower, need_len,
                                     first_l, first_u)) continue;
            if (ext_mask && !name_ext_matches(name, e->name_len, ext_mask)) continue;
            if (tags.count && !path_matches_all_tags(idx, i, &tags)) continue;
            int forbidden = has_forbidden && path_has_forbidden(idx, i);
            if (hidden_only) {
                if (!forbidden) continue;
            } else if (forbidden) {
                out->total_hidden++;
                continue;
            }
            out->total_matched++;
            if (out->count < out->capacity) {
                CoinHit *h = &out->hits[out->count++];
                h->entry_index = (uint32_t)i;
                h->drive_idx   = (uint16_t)d;
                h->flags       = 0;
                h->size        = -1;
                h->mtime       = 0;
            }
        }
        ReleaseSRWLockShared(&idx->lock);
    }

    ReleaseSRWLockShared(&g_forbidden_lock);
    taglist_free(&tags);
}

void coin_results_free(CoinResults *r) {
    free(r->hits);
    r->hits = NULL;
    r->count = 0;
    r->capacity = 0;
    r->total_matched = 0;
    r->total_hidden = 0;
}
