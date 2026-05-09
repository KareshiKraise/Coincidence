#ifndef COINCIDENCE_JOURNAL_H
#define COINCIDENCE_JOURNAL_H

#include "mft.h"

typedef struct {
    CoinIndex *idx;
    HWND   notify_hwnd;
    UINT   notify_msg;
    volatile LONG stop;
} CoinJournalCtx;

DWORD WINAPI coin_journal_thread_proc(LPVOID param);
void coin_journal_signal_stop(CoinJournalCtx *ctx);

#endif
