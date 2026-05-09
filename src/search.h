#ifndef COINCIDENCE_SEARCH_H
#define COINCIDENCE_SEARCH_H

#include "mft.h"

#define COIN_HIT_STAT_DONE   0x1u
#define COIN_HIT_STAT_FAILED 0x2u

typedef struct {
    uint32_t entry_index;
    uint16_t drive_idx;
    uint16_t flags;
    int64_t  size;
    uint64_t mtime;
} CoinHit;

typedef struct {
    CoinHit *hits;
    size_t count;
    size_t capacity;
    size_t total_matched;
    size_t total_hidden;
} CoinResults;

#define COIN_SEARCH_HIDDEN_ONLY 0x1u

#define COIN_EXT_PDF   0x01u
#define COIN_EXT_DOC   0x02u
#define COIN_EXT_ZIP   0x04u
#define COIN_EXT_IMG   0x08u
#define COIN_EXT_VIDEO 0x10u

typedef struct {
    unsigned ext_mask;
    const wchar_t *tags;
} CoinPrefilter;

void coin_search_all(CoinIndex **indices, size_t n_indices,
                     const wchar_t *needle,
                     const CoinPrefilter *pre,
                     size_t max_results,
                     unsigned flags,
                     CoinResults *out);
void coin_results_free(CoinResults *r);

void coin_search_set_forbidden(const wchar_t *const *words, size_t count);

#endif
