#ifndef COINCIDENCE_MFT_H
#define COINCIDENCE_MFT_H

#include <stddef.h>
#include <stdint.h>
#include <windows.h>
#include <winioctl.h>

#define COIN_ENTRY_DELETED 0x0001u

#pragma pack(push, 4)
typedef struct {
    DWORDLONG frn;
    DWORDLONG parent_frn;
    DWORD     attrs;
    uint32_t  name_offset;
    uint16_t  name_len;
    uint16_t  flags;
} CoinEntry;
#pragma pack(pop)

typedef struct {
    CoinEntry *entries;
    size_t count;
    size_t capacity;

    wchar_t *name_pool;
    size_t name_pool_used;
    size_t name_pool_cap;

    uint32_t  *bucket_index;
    size_t bucket_count;

    size_t deleted_count;
    size_t orphan_chars;

    wchar_t drive_letter;

    HANDLE     volume;
    DWORDLONG  journal_id;
    USN        first_usn;
    USN        next_usn;

    SRWLOCK lock;
} CoinIndex;

int  coin_mft_open(wchar_t drive_letter, CoinIndex *out);
int  coin_mft_enumerate(CoinIndex *idx);
void coin_mft_close(CoinIndex *idx);

void coin_mft_apply_record(CoinIndex *idx, USN_RECORD_V2 *r);

size_t coin_mft_find(const CoinIndex *idx, DWORDLONG frn);
size_t coin_mft_path(const CoinIndex *idx, size_t entry_index, wchar_t *out_buf, size_t out_cap);

int  coin_mft_save_cache(const CoinIndex *idx, const wchar_t *path);
int  coin_mft_load_cache(CoinIndex *idx, const wchar_t *path,
                         DWORDLONG *out_saved_journal_id, USN *out_saved_next_usn);
void coin_mft_clear_entries(CoinIndex *idx);

int  coin_mft_compact(CoinIndex *idx);
size_t coin_mft_compact_waste(const CoinIndex *idx);
int  coin_mft_shrink_to_fit(CoinIndex *idx);

#endif
