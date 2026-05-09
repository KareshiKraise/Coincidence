#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winioctl.h>
#include <stdlib.h>
#include <string.h>

#include "journal.h"

#define JRN_BUF_BYTES (1u << 16)

DWORD WINAPI coin_journal_thread_proc(LPVOID param) {
    CoinJournalCtx *ctx = (CoinJournalCtx*)param;
    if (!ctx || !ctx->idx || !ctx->idx->volume) return 1;

    BYTE *buf = (BYTE*)malloc(JRN_BUF_BYTES);
    if (!buf) return 1;

    READ_USN_JOURNAL_DATA_V0 rd;
    memset(&rd, 0, sizeof(rd));
    rd.StartUsn         = ctx->idx->next_usn;
    rd.ReasonMask       = USN_REASON_FILE_CREATE
                        | USN_REASON_FILE_DELETE
                        | USN_REASON_RENAME_NEW_NAME
                        | USN_REASON_HARD_LINK_CHANGE
                        | USN_REASON_REPARSE_POINT_CHANGE;
    rd.ReturnOnlyOnClose = TRUE;
    rd.Timeout          = 0;
    rd.BytesToWaitFor   = 1;
    rd.UsnJournalID     = ctx->idx->journal_id;

    while (!ctx->stop) {
        DWORD bytes_returned = 0;
        BOOL ok = DeviceIoControl(
            ctx->idx->volume,
            FSCTL_READ_USN_JOURNAL,
            &rd, sizeof(rd),
            buf, JRN_BUF_BYTES,
            &bytes_returned, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_INVALID_HANDLE ||
                err == ERROR_OPERATION_ABORTED ||
                err == ERROR_HANDLE_EOF) break;
            if (err == ERROR_JOURNAL_ENTRY_DELETED || err == ERROR_JOURNAL_NOT_ACTIVE) break;
            Sleep(100);
            continue;
        }
        if (bytes_returned < sizeof(USN)) {
            Sleep(50);
            continue;
        }

        USN next_usn = *(USN*)buf;
        BYTE *p = buf + sizeof(USN);
        BYTE *end = buf + bytes_returned;

        AcquireSRWLockExclusive(&ctx->idx->lock);
        size_t applied = 0;
        while (p < end) {
            USN_RECORD_V2 *r = (USN_RECORD_V2*)p;
            if (r->RecordLength == 0 || p + r->RecordLength > end) break;
            if (r->MajorVersion == 2) {
                coin_mft_apply_record(ctx->idx, r);
                applied++;
            }
            p += r->RecordLength;
        }
        ctx->idx->next_usn = next_usn;
        ReleaseSRWLockExclusive(&ctx->idx->lock);

        rd.StartUsn = next_usn;

        if (applied && ctx->notify_hwnd) {
            PostMessageW(ctx->notify_hwnd, ctx->notify_msg, 0, 0);
        }
    }

    free(buf);
    return 0;
}

void coin_journal_signal_stop(CoinJournalCtx *ctx) {
    if (!ctx) return;
    InterlockedExchange(&ctx->stop, 1);
    if (ctx->idx && ctx->idx->volume) {
        HANDLE h = ctx->idx->volume;
        ctx->idx->volume = NULL;
        CloseHandle(h);
    }
}
