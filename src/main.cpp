// DriveScanner - portable disk usage viewer.
// Win32 + Direct2D, no runtime dependencies. See README.md for build steps.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winioctl.h>
#include <winnetwk.h>
#include <d2d1.h>
#include <dwrite.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <stdio.h>

typedef unsigned long long u64;
typedef long long s64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

static const u32 NONE = 0xFFFFFFFFu;
static const double PI = 3.14159265358979323846;

// ---------------------------------------------------------------- memory

static void* xalloc(size_t n) {
    void* p = HeapAlloc(GetProcessHeap(), 0, n);
    if (!p) { MessageBoxW(NULL, L"Out of memory.", L"DriveScanner", MB_ICONERROR); ExitProcess(1); }
    return p;
}
static void* xcalloc(size_t n) {
    void* p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n);
    if (!p) { MessageBoxW(NULL, L"Out of memory.", L"DriveScanner", MB_ICONERROR); ExitProcess(1); }
    return p;
}
static void* xrealloc(void* p, size_t n) {
    p = p ? HeapReAlloc(GetProcessHeap(), 0, p, n) : HeapAlloc(GetProcessHeap(), 0, n);
    if (!p) { MessageBoxW(NULL, L"Out of memory.", L"DriveScanner", MB_ICONERROR); ExitProcess(1); }
    return p;
}
static void xfree(void* p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static int fmtw(wchar_t* out, int cap, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(out, cap - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= cap - 1) n = cap - 1;
    out[n] = 0;
    return n;
}

static void fmt_size(u64 v, wchar_t* out, int cap) {
    static const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB" };
    double d = (double)v;
    int i = 0;
    while (d >= 1024.0 && i < 5) { d /= 1024.0; i++; }
    if (i == 0) fmtw(out, cap, L"%llu B", v);
    else fmtw(out, cap, d < 10 ? L"%.2f %ls" : d < 100 ? L"%.1f %ls" : L"%.0f %ls", d, units[i]);
}

static void fmt_num(u64 v, wchar_t* out) {
    wchar_t t[32];
    int n = fmtw(t, 32, L"%llu", v), k = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) out[k++] = L',';
        out[k++] = t[i];
    }
    out[k] = 0;
}

// ---------------------------------------------------------------- tree

enum { NF_DIR = 1, NF_LINK = 2 };

// Children are singly linked and sorted by size, largest first.
struct Node {
    u64 size;
    u32 parent, first, next, name, files;
    u16 nameLen, flags;
};

struct Tree {
    Node* n;
    u32 count, cap;
    wchar_t* names;
    u32 namesLen, namesCap;
    wchar_t letter;
    u64 volTotal, volFree;
    double seconds;
    u32 errors;
    bool mft;
};

static Tree* tree_new(wchar_t letter) {
    Tree* t = (Tree*)xcalloc(sizeof(Tree));
    t->cap = 1 << 16;
    t->n = (Node*)xalloc(t->cap * sizeof(Node));
    t->namesCap = 1 << 20;
    t->names = (wchar_t*)xalloc(t->namesCap * sizeof(wchar_t));
    t->letter = letter;
    return t;
}

static void tree_free(Tree* t) {
    if (!t) return;
    xfree(t->n);
    xfree(t->names);
    xfree(t);
}

static u32 tree_name(Tree* t, const wchar_t* s, u32 len) {
    if ((u64)t->namesLen + len > t->namesCap) {
        while ((u64)t->namesLen + len > t->namesCap) t->namesCap *= 2;
        t->names = (wchar_t*)xrealloc(t->names, (size_t)t->namesCap * sizeof(wchar_t));
    }
    u32 off = t->namesLen;
    memcpy(t->names + off, s, len * sizeof(wchar_t));
    t->namesLen += len;
    return off;
}

static u32 tree_add(Tree* t, u32 parent, u32 nameOff, u16 nameLen, u64 size, u16 flags) {
    if (t->count == t->cap) {
        t->cap *= 2;
        t->n = (Node*)xrealloc(t->n, (size_t)t->cap * sizeof(Node));
    }
    Node* x = &t->n[t->count];
    x->size = size;
    x->parent = parent;
    x->first = x->next = NONE;
    x->name = nameOff;
    x->nameLen = nameLen;
    x->flags = flags;
    x->files = (flags & NF_DIR) ? 0 : 1;
    return t->count++;
}

static Node* g_sortNodes;
static int cmp_size_desc(const void* a, const void* b) {
    u64 x = g_sortNodes[*(const u32*)a].size, y = g_sortNodes[*(const u32*)b].size;
    return x < y ? 1 : x > y ? -1 : 0;
}

// Both scanners add nodes so that every parent index is lower than its children.
static void tree_finalize(Tree* t) {
    Node* n = t->n;
    for (u32 i = t->count; i-- > 1;) {
        u32 p = n[i].parent;
        n[p].size += n[i].size;
        n[p].files += n[i].files;
        n[i].next = n[p].first;
        n[p].first = i;
    }
    u32 cap = 4096;
    u32* tmp = (u32*)xalloc(cap * sizeof(u32));
    g_sortNodes = n;
    for (u32 i = 0; i < t->count; i++) {
        u32 k = 0;
        for (u32 c = n[i].first; c != NONE; c = n[c].next) {
            if (k == cap) { cap *= 2; tmp = (u32*)xrealloc(tmp, cap * sizeof(u32)); }
            tmp[k++] = c;
        }
        if (k < 2) continue;
        qsort(tmp, k, sizeof(u32), cmp_size_desc);
        n[i].first = tmp[0];
        for (u32 j = 0; j + 1 < k; j++) n[tmp[j]].next = tmp[j + 1];
        n[tmp[k - 1]].next = NONE;
    }
    xfree(tmp);
}

// Writes "C:\dir\sub" (the root is "C:"). Returns the length.
static int tree_path(Tree* t, u32 idx, wchar_t* buf, int cap) {
    u32 chain[1024];
    int d = 0, len = 0;
    for (u32 i = idx; i != NONE && d < 1024; i = t->n[i].parent) chain[d++] = i;
    for (int k = d - 1; k >= 0; k--) {
        Node* x = &t->n[chain[k]];
        if (len + x->nameLen + 2 >= cap) break;
        if (k != d - 1) buf[len++] = L'\\';
        memcpy(buf + len, t->names + x->name, x->nameLen * sizeof(wchar_t));
        len += x->nameLen;
    }
    buf[len] = 0;
    return len;
}

// ---------------------------------------------------------------- MFT scan (admin, NTFS)

#pragma pack(push, 1)
struct MftRec {
    u64 size;
    u32 parent;
    u32 name;
    u16 nameLen;
    u8 ns;
    u8 flags;
};
#pragma pack(pop)
enum { MR_INUSE = 1, MR_DIR = 2, MR_NAMED = 4 };

struct MftRun { s64 lcn; u64 len; };

static bool mft_fixup(u8* r, u32 size) {
    u16 usaOff = *(u16*)(r + 4), usaCnt = *(u16*)(r + 6);
    if (usaCnt < 2 || (u32)usaOff + usaCnt * 2u > size) return false;
    u16* usa = (u16*)(r + usaOff);
    for (u32 i = 1; i < usaCnt; i++) {
        u32 pos = i * 512 - 2;
        if (pos + 2 > size) break;
        if (*(u16*)(r + pos) != usa[0]) return false;
        *(u16*)(r + pos) = usa[i];
    }
    return true;
}

static int mft_runs(u8* attr, MftRun** out) {
    u8* p = attr + *(u16*)(attr + 32);
    u8* end = attr + *(u32*)(attr + 4);
    int n = 0, cap = 16;
    MftRun* runs = (MftRun*)xalloc(cap * sizeof(MftRun));
    s64 lcn = 0;
    while (p < end && *p) {
        int lb = *p & 15, ob = *p >> 4;
        p++;
        if (lb == 0 || lb > 8 || ob > 8 || p + lb + ob > end) break;
        u64 len = 0;
        for (int i = 0; i < lb; i++) len |= (u64)p[i] << (8 * i);
        p += lb;
        s64 off = 0;
        for (int i = 0; i < ob; i++) off |= (s64)((u64)p[i] << (8 * i));
        if (ob && ob < 8 && (p[ob - 1] & 0x80)) off |= (s64)(~0ULL << (8 * ob));
        p += ob;
        if (!ob) continue;  // sparse run; not expected in $MFT
        lcn += off;
        if (n == cap) { cap *= 2; runs = (MftRun*)xrealloc(runs, cap * sizeof(MftRun)); }
        runs[n].lcn = lcn;
        runs[n].len = len;
        n++;
    }
    *out = runs;
    return n;
}

static void mft_record(Tree* t, MftRec* recs, u64 recCount, u8* r, u32 recSize, u64 recNo) {
    if (memcmp(r, "FILE", 4) != 0 || !mft_fixup(r, recSize)) return;
    u16 hflags = *(u16*)(r + 22);
    if (!(hflags & 1)) return;
    u64 base = *(u64*)(r + 32) & 0xFFFFFFFFFFFFULL;
    u64 idx = base ? base : recNo;
    if (idx >= recCount) return;
    MftRec* m = &recs[idx];
    if (!base) m->flags |= MR_INUSE | ((hflags & 2) ? MR_DIR : 0);
    u32 used = *(u32*)(r + 24);
    if (used > recSize) used = recSize;
    u32 off = *(u16*)(r + 20);
    while (off + 16 <= used) {
        u8* a = r + off;
        u32 type = *(u32*)a;
        if (type == 0xFFFFFFFF) break;
        u32 len = *(u32*)(a + 4);
        if (len < 16 || off + len > used) break;
        u8 nonRes = a[8];
        if (type == 0x30 && !nonRes) {
            u8* v = a + *(u16*)(a + 20);
            if (v + 66 <= a + len) {
                u8 nl = v[64], ns = v[65];
                bool better = !(m->flags & MR_NAMED) || (m->ns == 2 && ns != 2);
                if (better && v + 66 + nl * 2 <= a + len) {
                    m->parent = (u32)(*(u64*)v & 0xFFFFFFFFFFFFULL);
                    m->name = tree_name(t, (wchar_t*)(v + 66), nl);
                    m->nameLen = nl;
                    m->ns = ns;
                    m->flags |= MR_NAMED;
                }
            }
        } else if (type == 0x80 && nonRes && len >= 64) {
            // Size on disk of every data stream: allocated size, or the
            // total allocated size when the stream is compressed or sparse.
            if (*(u64*)(a + 16) == 0) {
                u16 aflags = *(u16*)(a + 12);
                m->size += ((aflags & 0x8001) && len >= 72) ? *(u64*)(a + 64) : *(u64*)(a + 40);
            }
        }
        off += len;
    }
}

static bool mft_scan(Tree* t, volatile LONG* cancel, volatile LONG* progress, volatile LONG* phase) {
    wchar_t vol[8] = { L'\\', L'\\', L'.', L'\\', t->letter, L':', 0 };
    HANDLE h = CreateFileW(vol, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    NTFS_VOLUME_DATA_BUFFER vd;
    DWORD br;
    if (!DeviceIoControl(h, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &vd, sizeof(vd), &br, NULL)) {
        CloseHandle(h);
        return false;
    }
    InterlockedExchange(phase, 0);
    u32 recSize = vd.BytesPerFileRecordSegment, clus = vd.BytesPerCluster;
    u64 recCount = (u64)vd.MftValidDataLength.QuadPart / recSize;
    const u32 CHUNK = 8 << 20;
    u8* buf = (u8*)VirtualAlloc(NULL, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    u8* rec = (u8*)xalloc(recSize);
    MftRun* runs = NULL;
    MftRec* recs = NULL;
    bool ok = false;

    auto read_at = [&](u64 off, u32 n) -> bool {
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)off;
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        return ReadFile(h, buf, n, &got, &ov) && got == n;
    };

    do {
        // Record 0 is $MFT itself; its $DATA runs say where the rest of the table lives.
        u32 first = recSize > clus ? recSize : clus;
        if (!buf || !read_at((u64)vd.MftStartLcn.QuadPart * clus, first)) break;
        memcpy(rec, buf, recSize);
        if (memcmp(rec, "FILE", 4) != 0 || !mft_fixup(rec, recSize)) break;
        int nruns = 0;
        u32 off = *(u16*)(rec + 20);
        while (off + 16 <= recSize) {
            u8* a = rec + off;
            u32 type = *(u32*)a, len = *(u32*)(a + 4);
            if (type == 0xFFFFFFFF || len < 16 || off + len > recSize) break;
            if (type == 0x80 && a[8] && a[9] == 0) { nruns = mft_runs(a, &runs); break; }
            off += len;
        }
        u64 covered = 0;
        for (int i = 0; i < nruns; i++) covered += runs[i].len * clus;
        if (!nruns || covered < recCount * recSize) break;

        recs = (MftRec*)VirtualAlloc(NULL, recCount * sizeof(MftRec), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!recs) break;
        u64 recNo = 0;
        u32 fill = 0;
        bool failed = false;
        for (int i = 0; i < nruns && recNo < recCount && !failed; i++) {
            u64 pos = (u64)runs[i].lcn * clus, left = runs[i].len * clus;
            while (left && recNo < recCount) {
                if (*cancel) { failed = true; break; }
                u32 n = left > CHUNK ? CHUNK : (u32)left;
                if (!read_at(pos, n)) { failed = true; break; }
                u32 p = 0;
                if (fill) {
                    u32 take = recSize - fill < n ? recSize - fill : n;
                    memcpy(rec + fill, buf, take);
                    fill += take;
                    p = take;
                    if (fill == recSize) { mft_record(t, recs, recCount, rec, recSize, recNo++); fill = 0; }
                }
                while (p + recSize <= n && recNo < recCount) {
                    mft_record(t, recs, recCount, buf + p, recSize, recNo++);
                    p += recSize;
                }
                if (p < n && recNo < recCount) { memcpy(rec, buf + p, n - p); fill = n - p; }
                pos += n;
                left -= n;
                InterlockedExchange(progress, (LONG)recNo);
            }
        }
        if (failed) break;

        // Link records to parents, then walk from the root (record 5) so parents get lower node indices.
        u32* firstC = (u32*)VirtualAlloc(NULL, recCount * 4 * 3, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!firstC) break;
        u32* nextC = firstC + recCount;
        u32* queue = nextC + recCount;
        memset(firstC, 0xFF, recCount * 4);
        for (u64 i = recCount; i-- > 0;) {
            MftRec* m = &recs[i];
            if ((m->flags & (MR_INUSE | MR_NAMED)) != (MR_INUSE | MR_NAMED) || i == 5) continue;
            u32 p = m->parent;
            if (p >= recCount || !(recs[p].flags & MR_INUSE) || !(recs[p].flags & MR_DIR)) continue;
            nextC[i] = firstC[p];
            firstC[p] = (u32)i;
        }
        // queue holds record numbers; node index = position in BFS order + 1 offset handled via map below
        u32 qh = 0, qt = 0;
        u32* nodeOf = (u32*)xalloc(recCount * 4);
        nodeOf[5] = 0;
        t->n[0].size = recs[5].size;
        queue[qt++] = 5;
        while (qh < qt) {
            u32 r = queue[qh++];
            u32 nd = nodeOf[r];
            for (u32 c = firstC[r]; c != NONE; c = nextC[c]) {
                MftRec* m = &recs[c];
                u16 fl = (m->flags & MR_DIR) ? NF_DIR : 0;
                nodeOf[c] = tree_add(t, nd, m->name, m->nameLen, m->size, fl);
                if (fl) queue[qt++] = c;
            }
        }
        xfree(nodeOf);
        VirtualFree(firstC, 0, MEM_RELEASE);
        ok = true;
    } while (0);

    if (recs) VirtualFree(recs, 0, MEM_RELEASE);
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    xfree(rec);
    xfree(runs);
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------- directory scan (any drive)

struct DirScan {
    Tree* t;
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    u32* q;
    u32 qLen, qCap;
    int active;
    volatile LONG* cancel;
    volatile LONG* progress;
    u64* ids;  // hard links share a file id; count each file once
    u32 idCap, idCount;
};

static bool id_insert(DirScan* s, u64 k) {
    if (!k) return true;
    if (s->idCount * 2 >= s->idCap) {
        u32 oldCap = s->idCap;
        u64* old = s->ids;
        s->idCap = oldCap ? oldCap * 2 : 1 << 16;
        s->ids = (u64*)xcalloc((size_t)s->idCap * sizeof(u64));
        s->idCount = 0;
        for (u32 i = 0; i < oldCap; i++) if (old[i]) id_insert(s, old[i]);
        xfree(old);
    }
    u32 mask = s->idCap - 1, i = (u32)((k * 0x9E3779B97F4A7C15ULL) >> 32) & mask;
    while (s->ids[i]) {
        if (s->ids[i] == k) return false;
        i = (i + 1) & mask;
    }
    s->ids[i] = k;
    s->idCount++;
    return true;
}

struct DirEnt { u64 size, id; u32 name; u16 nameLen, flags; };

static DWORD WINAPI dir_worker(void* param) {
    DirScan* s = (DirScan*)param;
    Tree* t = s->t;
    const u32 BUFSZ = 64 * 1024;
    u8* buf = (u8*)xalloc(BUFSZ);
    wchar_t* path = (wchar_t*)xalloc(33000 * sizeof(wchar_t));
    u32 entCap = 1024, nbCap = 32768;
    DirEnt* ents = (DirEnt*)xalloc(entCap * sizeof(DirEnt));
    wchar_t* nb = (wchar_t*)xalloc(nbCap * sizeof(wchar_t));

    for (;;) {
        AcquireSRWLockExclusive(&s->lock);
        while (s->qLen == 0 && s->active > 0 && !*s->cancel)
            SleepConditionVariableSRW(&s->cv, &s->lock, 100, 0);
        if (s->qLen == 0 || *s->cancel) {
            WakeAllConditionVariable(&s->cv);
            ReleaseSRWLockExclusive(&s->lock);
            break;
        }
        u32 dir = s->q[--s->qLen];
        s->active++;
        memcpy(path, L"\\\\?\\", 4 * sizeof(wchar_t));
        int len = 4 + tree_path(t, dir, path + 4, 32700);
        ReleaseSRWLockExclusive(&s->lock);
        if (len == 6) { path[len++] = L'\\'; path[len] = 0; }  // "\\?\C:" would open the volume

        u32 nents = 0, nbLen = 0;
        bool err = false;
        HANDLE h = CreateFileW(path, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            err = true;
        } else {
            BOOL first = TRUE;
            while (GetFileInformationByHandleEx(h, first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
                                                buf, BUFSZ)) {
                first = FALSE;
                u8* p = buf;
                for (;;) {
                    FILE_ID_BOTH_DIR_INFO* e = (FILE_ID_BOTH_DIR_INFO*)p;
                    u32 nl = e->FileNameLength / 2;
                    bool dot = (nl == 1 && e->FileName[0] == L'.') ||
                               (nl == 2 && e->FileName[0] == L'.' && e->FileName[1] == L'.');
                    if (!dot && nl) {
                        if (nents == entCap) { entCap *= 2; ents = (DirEnt*)xrealloc(ents, entCap * sizeof(DirEnt)); }
                        if (nbLen + nl > nbCap) {
                            while (nbLen + nl > nbCap) nbCap *= 2;
                            nb = (wchar_t*)xrealloc(nb, nbCap * sizeof(wchar_t));
                        }
                        DirEnt* d = &ents[nents++];
                        bool isDir = (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                        bool link = (e->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                        d->size = isDir ? 0 : (u64)e->AllocationSize.QuadPart;
                        d->id = isDir ? 0 : (u64)e->FileId.QuadPart;
                        d->flags = (isDir ? NF_DIR : 0) | (isDir && link ? NF_LINK : 0);
                        d->name = nbLen;
                        d->nameLen = (u16)nl;
                        memcpy(nb + nbLen, e->FileName, nl * sizeof(wchar_t));
                        nbLen += nl;
                    }
                    if (!e->NextEntryOffset) break;
                    p += e->NextEntryOffset;
                }
            }
            CloseHandle(h);
        }

        AcquireSRWLockExclusive(&s->lock);
        if (err) t->errors++;
        u32 pushed = 0;
        for (u32 i = 0; i < nents; i++) {
            DirEnt* d = &ents[i];
            if (!(d->flags & NF_DIR) && !id_insert(s, d->id)) continue;
            u32 nm = tree_name(t, nb + d->name, d->nameLen);
            u32 id = tree_add(t, dir, nm, d->nameLen, d->size, d->flags);
            if ((d->flags & NF_DIR) && !(d->flags & NF_LINK)) {
                if (s->qLen == s->qCap) { s->qCap *= 2; s->q = (u32*)xrealloc(s->q, s->qCap * sizeof(u32)); }
                s->q[s->qLen++] = id;
                pushed++;
            }
        }
        s->active--;
        InterlockedExchange(s->progress, (LONG)t->count);
        if (pushed || (s->qLen == 0 && s->active == 0)) WakeAllConditionVariable(&s->cv);
        ReleaseSRWLockExclusive(&s->lock);
    }
    xfree(buf);
    xfree(path);
    xfree(ents);
    xfree(nb);
    return 0;
}

static void dir_scan(Tree* t, volatile LONG* cancel, volatile LONG* progress) {
    DirScan s = {};
    s.t = t;
    InitializeSRWLock(&s.lock);
    InitializeConditionVariable(&s.cv);
    s.qCap = 4096;
    s.q = (u32*)xalloc(s.qCap * sizeof(u32));
    s.q[s.qLen++] = 0;
    s.cancel = cancel;
    s.progress = progress;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int threads = (int)si.dwNumberOfProcessors * 2;
    if (threads < 4) threads = 4;
    if (threads > 16) threads = 16;
    HANDLE th[16];
    for (int i = 0; i < threads; i++) th[i] = CreateThread(NULL, 256 * 1024, dir_worker, &s, 0, NULL);
    WaitForMultipleObjects(threads, th, TRUE, INFINITE);
    for (int i = 0; i < threads; i++) CloseHandle(th[i]);
    xfree(s.q);
    xfree(s.ids);
}

// ---------------------------------------------------------------- scan job

#define WM_SCANDONE (WM_APP + 1)

struct ScanJob {
    u32 id;
    wchar_t letter;
    HWND notify;
    volatile LONG cancel, progress, phase;  // phase: 0 file table, 1 folders, 2 sorting
    Tree* result;
};

static DWORD WINAPI scan_thread(void* param) {
    ScanJob* j = (ScanJob*)param;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    wchar_t drv[3] = { j->letter, L':', 0 };
    wchar_t root[4] = { j->letter, L':', L'\\', 0 };
    Tree* t = tree_new(j->letter);
    tree_add(t, NONE, tree_name(t, drv, 2), 2, 0, NF_DIR);
    ULARGE_INTEGER avail, total, freeb;
    if (GetDiskFreeSpaceExW(root, &avail, &total, &freeb)) {
        t->volTotal = total.QuadPart;
        t->volFree = freeb.QuadPart;
    }
    t->mft = mft_scan(t, &j->cancel, &j->progress, &j->phase);
    if (!t->mft && !j->cancel) {
        t->count = 1;
        t->namesLen = 2;
        t->n[0].size = 0;
        InterlockedExchange(&j->phase, 1);
        dir_scan(t, &j->cancel, &j->progress);
    }
    if (j->cancel) { tree_free(t); return 0; }
    InterlockedExchange(&j->phase, 2);
    tree_finalize(t);
    QueryPerformanceCounter(&t1);
    t->seconds = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    j->result = t;
    PostMessageW(j->notify, WM_SCANDONE, j->id, 0);
    return 0;
}

// ---------------------------------------------------------------- UI state

struct Seg { u32 node; int depth; float a0, a1; ID2D1PathGeometry* geo; };
struct Crumb { u32 node; D2D1_RECT_F rc; };
struct Row { u32 node; int depth; };

static const float HDR = 48.0f;      // header strip in the view (DIPs)
static const float LIST_HDR = 30.0f;
static const float ROW_H = 26.0f;
static const int LEVELS = 6;
static const int TOOLBAR = 48;        // toolbar height in the main window (DIPs)

enum { ID_COMBO = 100, ID_RESCAN, ID_LIST, ID_FREE, ID_ABOUT };

static const wchar_t APP_VERSION[] = L"1.0.0";  // keep in sync with DriveScanner.rc

static struct {
    HINSTANCE inst;
    HWND main, view, combo, rescan, listChk, freeChk, status;
    HFONT uiFont, iconFont;
    HWND about, tip;
    UINT dpi;

    ID2D1Factory* d2d;
    IDWriteFactory* dw;
    ID2D1HwndRenderTarget* rt;
    ID2D1SolidColorBrush* br;
    IDWriteTextFormat *fText, *fBold, *fRight, *fSmall, *fCenter, *fTitle, *fIcon;

    Tree* tree;
    u32 root;
    bool listMode, showFree;

    ScanJob* job;
    HANDLE scanThread;
    u32 nextJobId;
    DWORD scanStart;

    Seg* segs;
    u32 segCount, segCap;
    bool layoutDirty;
    float cx, cy, r0, ringW, hueScale;

    u32 hover;
    bool hoverCenter;
    int hoverCrumb;
    float mx, my;
    bool mouseIn, hand;

    Crumb crumbs[64];
    int crumbCount;

    u8* expanded;
    Row* rows;
    u32 rowCount, rowCap;
    int scroll;
    bool rowsDirty;
    int hoverRow;

    wchar_t driveLetter[26];
    int driveCount;
} G;

static int S(int v) { return MulDiv(v, (int)G.dpi, 96); }

static D2D1_COLOR_F rgb(u32 hex, float a = 1.0f) {
    D2D1_COLOR_F c = { ((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a };
    return c;
}

static D2D1_COLOR_F hsl(float h, float s, float l) {
    h = fmodf(h, 360.0f) / 60.0f;
    if (h < 0) h += 6.0f;
    float c = (1 - fabsf(2 * l - 1)) * s, x = c * (1 - fabsf(fmodf(h, 2.0f) - 1)), m = l - c / 2;
    float r, g, b;
    switch ((int)h) {
    case 0: r = c; g = x; b = 0; break;
    case 1: r = x; g = c; b = 0; break;
    case 2: r = 0; g = c; b = x; break;
    case 3: r = 0; g = x; b = c; break;
    case 4: r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }
    D2D1_COLOR_F o = { r + m, g + m, b + m, 1.0f };
    return o;
}

static const u32 C_BG = 0xFFFFFF, C_TEXT = 0x1B1B1B, C_MUTED = 0x6E6E6E, C_LINE = 0xE6E6E6,
                 C_ACCENT = 0x2563EB, C_HOVER = 0xEEF3FD, C_TRACK = 0xE9ECF1, C_CENTER = 0xF4F5F7;

static int node_name(u32 i, wchar_t* out, int cap) {
    Node* x = &G.tree->n[i];
    int len = x->nameLen < cap - 2 ? x->nameLen : cap - 2;
    memcpy(out, G.tree->names + x->name, len * sizeof(wchar_t));
    if (i == 0) out[len++] = L'\\';
    out[len] = 0;
    return len;
}

static void node_path(u32 i, wchar_t* out, int cap) {
    int len = tree_path(G.tree, i, out, cap - 1);
    if (len == 2) { out[2] = L'\\'; out[3] = 0; }
}

static const u32 FREE_SPACE = 0xFFFFFFFEu;  // pseudo-node: the drive's free space slice in the rings

static bool has_children(u32 i) { return i < G.tree->count && G.tree->n[i].first != NONE; }

// ---------------------------------------------------------------- Direct2D helpers

static void fill_rect(float l, float t, float r, float b, D2D1_COLOR_F c) {
    G.br->SetColor(c);
    G.rt->FillRectangle(D2D1::RectF(l, t, r, b), G.br);
}

static void draw_text(const wchar_t* s, int len, IDWriteTextFormat* f, float l, float t, float r, float b,
                      D2D1_COLOR_F c) {
    if (r <= l) return;
    G.br->SetColor(c);
    G.rt->DrawText(s, len, f, D2D1::RectF(l, t, r, b), G.br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static float text_width(const wchar_t* s, int len, IDWriteTextFormat* f) {
    IDWriteTextLayout* lay;
    if (FAILED(G.dw->CreateTextLayout(s, len, f, 10000.0f, 100.0f, &lay))) return 0;
    DWRITE_TEXT_METRICS m;
    lay->GetMetrics(&m);
    lay->Release();
    return m.widthIncludingTrailingWhitespace;
}

static ID2D1PathGeometry* make_arc(float cx, float cy, float r0, float r1, float a0, float a1) {
    if (a1 - a0 > 2 * (float)PI - 0.0005f) a1 = a0 + 2 * (float)PI - 0.0005f;
    ID2D1PathGeometry* g;
    if (FAILED(G.d2d->CreatePathGeometry(&g))) return NULL;
    ID2D1GeometrySink* s;
    g->Open(&s);
    auto P = [&](float r, float a) { return D2D1::Point2F(cx + r * sinf(a), cy - r * cosf(a)); };
    D2D1_ARC_SIZE big = (a1 - a0 > (float)PI) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
    s->BeginFigure(P(r1, a0), D2D1_FIGURE_BEGIN_FILLED);
    s->AddArc(D2D1::ArcSegment(P(r1, a1), D2D1::SizeF(r1, r1), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, big));
    s->AddLine(P(r0, a1));
    s->AddArc(D2D1::ArcSegment(P(r0, a0), D2D1::SizeF(r0, r0), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, big));
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    s->Release();
    return g;
}

static IDWriteTextFormat* make_format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w,
                                      DWRITE_TEXT_ALIGNMENT align) {
    IDWriteTextFormat* f = NULL;
    G.dw->CreateTextFormat(family, NULL, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"",
                           &f);
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    IDWriteInlineObject* sign = NULL;
    G.dw->CreateEllipsisTrimmingSign(f, &sign);
    f->SetTrimming(&trim, sign);
    if (sign) sign->Release();
    return f;
}

static void discard_target() {
    if (G.br) { G.br->Release(); G.br = NULL; }
    if (G.rt) { G.rt->Release(); G.rt = NULL; }
}

static bool ensure_target() {
    if (G.rt) return true;
    RECT rc;
    GetClientRect(G.view, &rc);
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties();
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp =
        D2D1::HwndRenderTargetProperties(G.view, D2D1::SizeU(rc.right, rc.bottom));
    if (FAILED(G.d2d->CreateHwndRenderTarget(rp, hp, &G.rt))) return false;
    G.rt->SetDpi((float)G.dpi, (float)G.dpi);
    G.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    G.rt->CreateSolidColorBrush(rgb(0), &G.br);
    return true;
}

static D2D1_SIZE_F view_size() {
    RECT rc;
    GetClientRect(G.view, &rc);
    return D2D1::SizeF(rc.right * 96.0f / G.dpi, rc.bottom * 96.0f / G.dpi);
}

// ---------------------------------------------------------------- rings

static void clear_segs() {
    for (u32 i = 0; i < G.segCount; i++) if (G.segs[i].geo) G.segs[i].geo->Release();
    G.segCount = 0;
}

static void add_seg(u32 node, int depth, double a0, double a1) {
    if (G.segCount == G.segCap) {
        G.segCap = G.segCap ? G.segCap * 2 : 1024;
        G.segs = (Seg*)xrealloc(G.segs, G.segCap * sizeof(Seg));
    }
    Seg* s = &G.segs[G.segCount++];
    s->node = node;
    s->depth = depth;
    s->a0 = (float)a0;
    s->a1 = (float)a1;
    float inner = G.r0 + G.ringW * (depth - 1) + 1.0f, outer = G.r0 + G.ringW * depth;
    s->geo = make_arc(G.cx, G.cy, inner, outer, s->a0, s->a1);
}

static void build_segs(u32 node, int depth, double a0, double a1) {
    Node* n = G.tree->n;
    double total = (double)n[node].size;
    if (total <= 0) return;
    double minA = 1.2 / (G.r0 + G.ringW * (depth - 0.5));  // skip slivers under ~1 DIP wide
    double a = a0;
    for (u32 c = n[node].first; c != NONE; c = n[c].next) {
        double span = (a1 - a0) * (double)n[c].size / total;
        if (span < minA) break;
        add_seg(c, depth, a, a + span);
        if (depth < LEVELS && n[c].first != NONE) build_segs(c, depth + 1, a, a + span);
        a += span;
    }
}

static void layout_rings(D2D1_SIZE_F sz) {
    clear_segs();
    G.layoutDirty = false;
    float h = sz.height - HDR;
    G.cx = sz.width / 2;
    G.cy = HDR + h / 2;
    float R = (sz.width < h ? sz.width : h) / 2 - 16;
    if (R < 60) { G.r0 = G.ringW = 0; return; }
    G.r0 = R * 0.2f;
    G.ringW = (R - G.r0) / LEVELS;
    // At the drive level, the inner ring also shows free space so used vs. free is to scale.
    double used = (double)G.tree->n[0].size, freeb = (double)G.tree->volFree;
    if (G.showFree && G.root == 0 && freeb > 0 && used + freeb > 0) {
        double split = 2 * PI * used / (used + freeb);
        G.hueScale = (float)(2 * PI / split);
        build_segs(G.root, 1, 0, split);
        add_seg(FREE_SPACE, 1, split, 2 * PI);
    } else {
        G.hueScale = 1.0f;
        build_segs(G.root, 1, 0, 2 * PI);
    }
}

static D2D1_COLOR_F seg_color(Seg* s, bool hot) {
    if (s->node == FREE_SPACE) return rgb(hot ? 0xCDD2D9 : 0xE4E7EB);
    bool dir = (G.tree->n[s->node].flags & NF_DIR) != 0;
    float hue = (s->a0 + s->a1) * 0.5f * G.hueScale * 180.0f / (float)PI;  // full rainbow over the used span
    float l = (dir ? 0.56f : 0.74f) + 0.045f * (s->depth - 1);
    if (l > 0.88f) l = 0.88f;
    if (hot) l -= 0.14f;
    return hsl(hue, dir ? 0.62f : 0.22f, l);
}

static u32 hit_rings(float x, float y, bool* center) {
    *center = false;
    if (G.ringW <= 0) return NONE;
    float dx = x - G.cx, dy = y - G.cy, r = sqrtf(dx * dx + dy * dy);
    if (r < G.r0) { *center = true; return NONE; }
    int depth = (int)((r - G.r0) / G.ringW) + 1;
    if (depth > LEVELS) return NONE;
    float a = atan2f(dx, -dy);
    if (a < 0) a += 2 * (float)PI;
    for (u32 i = 0; i < G.segCount; i++) {
        Seg* s = &G.segs[i];
        if (s->depth == depth && a >= s->a0 && a < s->a1) return s->node;
    }
    return NONE;
}

static void draw_seg_label(Seg* s) {
    float mid = (s->a0 + s->a1) * 0.5f, rMid = G.r0 + G.ringW * (s->depth - 0.5f);
    float arc = (s->a1 - s->a0) * rMid;
    float sn = fabsf(sinf(mid)), cs = fabsf(cosf(mid));
    float wr = G.ringW / (sn > 0.01f ? sn : 0.01f), wa = arc / (cs > 0.01f ? cs : 0.01f);
    float hr = G.ringW / (cs > 0.01f ? cs : 0.01f), ha = arc / (sn > 0.01f ? sn : 0.01f);
    float w = (wr < wa ? wr : wa) * 0.86f, h = (hr < ha ? hr : ha) * 0.86f;
    if (w > 150) w = 150;
    if (w < 36 || h < 15) return;
    wchar_t name[300];
    int len = s->node == FREE_SPACE ? fmtw(name, 300, L"Free") : node_name(s->node, name, 300);
    float px = G.cx + rMid * sinf(mid), py = G.cy - rMid * cosf(mid);
    draw_text(name, len, G.fCenter, px - w / 2, py - 9, px + w / 2, py + 9, rgb(0x202020));
}

static void draw_tooltip(u32 node, D2D1_SIZE_F sz) {
    Node* n = G.tree->n;
    wchar_t name[300], line2[128], sizeStr[32], files[32];
    int nlen, l2;
    if (node == FREE_SPACE) {
        nlen = fmtw(name, 300, L"Free space");
        fmt_size(G.tree->volFree, sizeStr, 32);
        double pct = G.tree->volTotal ? 100.0 * (double)G.tree->volFree / (double)G.tree->volTotal : 0;
        l2 = fmtw(line2, 128, L"%ls  \x00B7  %.1f%% of drive", sizeStr, pct);
    } else {
        nlen = node_name(node, name, 300);
        fmt_size(n[node].size, sizeStr, 32);
        u64 psize = n[node].parent != NONE ? n[n[node].parent].size : n[node].size;
        double pct = psize ? 100.0 * (double)n[node].size / (double)psize : 100.0;
        if (n[node].flags & NF_DIR) {
            fmt_num(n[node].files, files);
            l2 = fmtw(line2, 128, L"%ls  \x00B7  %.1f%% of parent  \x00B7  %ls files", sizeStr, pct, files);
        } else {
            l2 = fmtw(line2, 128, L"%ls  \x00B7  %.1f%% of parent", sizeStr, pct);
        }
    }
    float w1 = text_width(name, nlen, G.fBold), w2 = text_width(line2, l2, G.fText);
    float w = (w1 > w2 ? w1 : w2) + 24;
    if (w > 420) w = 420;
    float h = 52, x = G.mx + 16, y = G.my + 18;
    if (x + w > sz.width - 4) x = G.mx - w - 12;
    if (y + h > sz.height - 4) y = G.my - h - 12;
    if (x < 4) x = 4;
    D2D1_ROUNDED_RECT rr = { D2D1::RectF(x, y, x + w, y + h), 6, 6 };
    G.br->SetColor(rgb(0xFFFFFF));
    G.rt->FillRoundedRectangle(rr, G.br);
    G.br->SetColor(rgb(0xC9CDD4));
    G.rt->DrawRoundedRectangle(rr, G.br, 1.0f);
    draw_text(name, nlen, G.fBold, x + 12, y + 6, x + w - 12, y + 26, rgb(C_TEXT));
    draw_text(line2, l2, G.fText, x + 12, y + 26, x + w - 12, y + 46, rgb(C_MUTED));
}

static void draw_rings(D2D1_SIZE_F sz) {
    if (G.layoutDirty) layout_rings(sz);
    if (G.ringW <= 0) return;
    Node* n = G.tree->n;
    for (u32 i = 0; i < G.segCount; i++) {
        Seg* s = &G.segs[i];
        if (!s->geo) continue;
        G.br->SetColor(seg_color(s, s->node == G.hover));
        G.rt->FillGeometry(s->geo, G.br);
    }
    for (u32 i = 0; i < G.segCount; i++) if (G.segs[i].depth <= 2) draw_seg_label(&G.segs[i]);

    // center: the folder being viewed; click to go up
    D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(G.cx, G.cy), G.r0 - 2, G.r0 - 2);
    G.br->SetColor(rgb(G.hoverCenter && G.root != 0 ? 0xE6EAF0 : C_CENTER));
    G.rt->FillEllipse(e, G.br);
    wchar_t name[300], sizeStr[32], files[32], line[64];
    int len = node_name(G.root, name, 300);
    fmt_size(n[G.root].size, sizeStr, 32);
    fmt_num(n[G.root].files, files);
    float w = G.r0 * 1.7f;
    if (G.root == 0 && G.tree->volTotal) {
        // Drive level: used and total space of the volume, like the header.
        wchar_t used[32], total[32];
        fmt_size(G.tree->volTotal - G.tree->volFree, used, 32);
        fmt_size(G.tree->volTotal, total, 32);
        draw_text(name, len, G.fTitle, G.cx - w / 2, G.cy - 38, G.cx + w / 2, G.cy - 16, rgb(C_TEXT));
        int l2 = fmtw(line, 64, L"%ls used", used);
        draw_text(line, l2, G.fCenter, G.cx - w / 2, G.cy - 16, G.cx + w / 2, G.cy + 2, rgb(C_TEXT));
        l2 = fmtw(line, 64, L"of %ls", total);
        draw_text(line, l2, G.fCenter, G.cx - w / 2, G.cy + 1, G.cx + w / 2, G.cy + 19, rgb(C_TEXT));
        l2 = fmtw(line, 64, L"%ls files", files);
        draw_text(line, l2, G.fCenter, G.cx - w / 2, G.cy + 20, G.cx + w / 2, G.cy + 38, rgb(C_MUTED));
    } else {
        draw_text(name, len, G.fTitle, G.cx - w / 2, G.cy - 30, G.cx + w / 2, G.cy - 8, rgb(C_TEXT));
        draw_text(sizeStr, (int)wcslen(sizeStr), G.fCenter, G.cx - w / 2, G.cy - 8, G.cx + w / 2, G.cy + 10,
                  rgb(C_TEXT));
        int l3 = fmtw(line, 64, L"%ls files", files);
        draw_text(line, l3, G.fCenter, G.cx - w / 2, G.cy + 8, G.cx + w / 2, G.cy + 26, rgb(C_MUTED));
    }
    if (G.root != 0 && G.r0 > 48)
        draw_text(L"\xE74A  Up", 5, G.fCenter, G.cx - w / 2, G.cy + 28, G.cx + w / 2, G.cy + 46,
                  rgb(G.hoverCenter ? C_ACCENT : C_MUTED));

    if (G.hover != NONE && G.mouseIn) draw_tooltip(G.hover, sz);
}

// ---------------------------------------------------------------- list

static void rows_add(u32 node, int depth) {
    Node* n = G.tree->n;
    for (u32 c = n[node].first; c != NONE; c = n[c].next) {
        if (G.rowCount == G.rowCap) {
            G.rowCap = G.rowCap ? G.rowCap * 2 : 1024;
            G.rows = (Row*)xrealloc(G.rows, G.rowCap * sizeof(Row));
        }
        G.rows[G.rowCount].node = c;
        G.rows[G.rowCount].depth = depth;
        G.rowCount++;
        if (G.expanded[c]) rows_add(c, depth + 1);
    }
}

static int list_visible() {
    int v = (int)((view_size().height - HDR - LIST_HDR) / ROW_H);
    return v < 1 ? 1 : v;
}

static void update_scrollbar() {
    if (!G.listMode || !G.tree) {
        ShowScrollBar(G.view, SB_VERT, FALSE);
        return;
    }
    int vis = list_visible();
    int maxScroll = (int)G.rowCount - vis;
    if (G.scroll > maxScroll) G.scroll = maxScroll;
    if (G.scroll < 0) G.scroll = 0;
    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL, 0,
                      G.rowCount ? (int)G.rowCount - 1 : 0, (UINT)vis, G.scroll, 0 };
    ShowScrollBar(G.view, SB_VERT, TRUE);
    SetScrollInfo(G.view, SB_VERT, &si, TRUE);
}

static void rebuild_rows() {
    G.rowCount = 0;
    rows_add(G.root, 0);
    G.rowsDirty = false;
    update_scrollbar();
}

struct Cols { float name0, name1, bar0, pct0, size0, size1, files0, files1; };

static Cols list_cols(float W) {
    Cols c;
    c.files1 = W - 16;
    c.files0 = c.files1 - 90;
    c.size1 = c.files0 - 12;
    c.size0 = c.size1 - 90;
    c.pct0 = c.size0 - 60;
    c.bar0 = c.pct0 - 128;
    c.name0 = 16;
    c.name1 = c.bar0 - 16;
    return c;
}

static void draw_list(D2D1_SIZE_F sz) {
    if (G.rowsDirty) rebuild_rows();
    Node* n = G.tree->n;
    Cols c = list_cols(sz.width);
    float y = HDR;
    draw_text(L"Name", 4, G.fSmall, c.name0, y, c.name1, y + LIST_HDR, rgb(C_MUTED));
    draw_text(L"% of parent", 11, G.fSmall, c.bar0, y, c.pct0 + 56, y + LIST_HDR, rgb(C_MUTED));
    draw_text(L"Size", 4, G.fRight, c.size0, y, c.size1, y + LIST_HDR, rgb(C_MUTED));
    draw_text(L"Files", 5, G.fRight, c.files0, y, c.files1, y + LIST_HDR, rgb(C_MUTED));
    fill_rect(0, y + LIST_HDR - 1, sz.width, y + LIST_HDR, rgb(C_LINE));
    y += LIST_HDR;
    if (!G.rowCount) {
        draw_text(L"This folder is empty", 20, G.fText, c.name0, y, c.name1, y + ROW_H, rgb(C_MUTED));
        return;
    }
    for (int r = G.scroll; r < (int)G.rowCount && y < sz.height; r++, y += ROW_H) {
        Row* row = &G.rows[r];
        Node* x = &n[row->node];
        if (r == G.hoverRow) fill_rect(0, y, sz.width, y + ROW_H, rgb(C_HOVER));
        float ix = c.name0 + row->depth * 18.0f;
        if (x->first != NONE)
            draw_text(G.expanded[row->node] ? L"\xE70D" : L"\xE76C", 1, G.fIcon, ix, y, ix + 16, y + ROW_H,
                      rgb(C_MUTED));
        ix += 18;
        bool dir = (x->flags & NF_DIR) != 0;
        draw_text(dir ? L"\xE8B7" : L"\xE8A5", 1, G.fIcon, ix, y, ix + 16, y + ROW_H,
                  rgb(dir ? 0xD9A21B : 0x8A8F98));
        ix += 22;
        wchar_t name[300];
        int len = node_name(row->node, name, 300);
        draw_text(name, len, G.fText, ix, y, c.name1, y + ROW_H, rgb(C_TEXT));

        u64 psize = x->parent != NONE ? n[x->parent].size : x->size;
        double pct = psize ? (double)x->size / (double)psize : 0;
        float by = y + ROW_H / 2 - 3;
        fill_rect(c.bar0, by, c.bar0 + 118, by + 6, rgb(C_TRACK));
        if (pct > 0) fill_rect(c.bar0, by, c.bar0 + (float)(118 * pct < 1 ? 1 : 118 * pct), by + 6, rgb(C_ACCENT));
        wchar_t t[32];
        int tl = fmtw(t, 32, L"%.1f%%", pct * 100.0);
        draw_text(t, tl, G.fRight, c.pct0, y, c.pct0 + 50, y + ROW_H, rgb(C_MUTED));
        fmt_size(x->size, t, 32);
        draw_text(t, (int)wcslen(t), G.fRight, c.size0, y, c.size1, y + ROW_H, rgb(C_TEXT));
        if (dir) {
            fmt_num(x->files, t);
            draw_text(t, (int)wcslen(t), G.fRight, c.files0, y, c.files1, y + ROW_H, rgb(C_MUTED));
        }
    }
}

static int hit_list(float y) {
    if (y < HDR + LIST_HDR) return -1;
    int r = G.scroll + (int)((y - HDR - LIST_HDR) / ROW_H);
    return r < (int)G.rowCount ? r : -1;
}

// ---------------------------------------------------------------- header, scanning screen

static void draw_header(D2D1_SIZE_F sz) {
    Node* n = G.tree->n;
    u32 chain[256];
    int d = 0;
    for (u32 i = G.root; i != NONE && d < 256; i = n[i].parent) chain[d++] = i;
    float x = 16, maxX = sz.width * 0.58f;
    G.crumbCount = 0;
    for (int k = d - 1; k >= 0; k--) {
        wchar_t name[300];
        int len = node_name(chain[k], name, 300);
        bool last = k == 0;
        IDWriteTextFormat* f = last ? G.fBold : G.fText;
        float w = text_width(name, len, f);
        if (x + w > maxX) w = maxX - x;
        if (w <= 8) break;
        bool hot = !last && G.hoverCrumb == G.crumbCount;
        draw_text(name, len, f, x, 0, x + w, HDR, rgb(last ? C_TEXT : hot ? C_ACCENT : C_MUTED));
        if (!last && G.crumbCount < 64) {
            G.crumbs[G.crumbCount].node = chain[k];
            G.crumbs[G.crumbCount].rc = D2D1::RectF(x, 10, x + w, HDR - 10);
            G.crumbCount++;
        }
        x += w;
        if (!last) {
            draw_text(L"\xE76C", 1, G.fIcon, x + 4, 0, x + 20, HDR, rgb(0xA0A4AB));
            x += 24;
        }
    }

    wchar_t s[256], a[32], b[32], c[32];
    int len;
    if (G.root == 0 && G.tree->volTotal) {
        fmt_size(G.tree->volTotal - G.tree->volFree, a, 32);
        fmt_size(G.tree->volTotal, b, 32);
        fmt_size(G.tree->volFree, c, 32);
        len = fmtw(s, 256, L"%ls used of %ls  \x00B7  %ls free", a, b, c);
    } else {
        fmt_size(n[G.root].size, a, 32);
        fmt_num(n[G.root].files, b);
        double pct = n[0].size ? 100.0 * (double)n[G.root].size / (double)n[0].size : 0;
        len = fmtw(s, 256, L"%ls  \x00B7  %ls files  \x00B7  %.1f%% of drive", a, b, pct);
    }
    draw_text(s, len, G.fRight, maxX + 16, 0, sz.width - 16, HDR, rgb(C_MUTED));
    fill_rect(0, HDR - 1, sz.width, HDR, rgb(C_LINE));
}

static void draw_scanning(D2D1_SIZE_F sz) {
    float cx = sz.width / 2, cy = sz.height / 2 - 20;
    float a = (float)fmod(GetTickCount() / 1000.0 * 5.0, 2 * PI);
    ID2D1PathGeometry* track = make_arc(cx, cy, 24, 28, 0, 2 * (float)PI);
    ID2D1PathGeometry* arc = make_arc(cx, cy, 24, 28, a, a + 1.6f);
    if (track) { G.br->SetColor(rgb(C_TRACK)); G.rt->FillGeometry(track, G.br); track->Release(); }
    if (arc) { G.br->SetColor(rgb(C_ACCENT)); G.rt->FillGeometry(arc, G.br); arc->Release(); }
    wchar_t t[128], num[32];
    int len = fmtw(t, 128, L"Scanning %lc:\\", G.job->letter);
    draw_text(t, len, G.fTitle, 0, cy + 40, sz.width, cy + 64, rgb(C_TEXT));
    fmt_num((u64)G.job->progress, num);
    static const wchar_t* phases[] = { L"Reading file table", L"Scanning folders", L"Sorting" };
    LONG ph = G.job->phase;
    len = fmtw(t, 128, L"%ls  \x00B7  %ls items  \x00B7  %.1f s", phases[ph < 0 || ph > 2 ? 1 : ph], num,
               (GetTickCount() - G.scanStart) / 1000.0);
    draw_text(t, len, G.fCenter, 0, cy + 64, sz.width, cy + 86, rgb(C_MUTED));
}

static void paint() {
    if (!ensure_target()) return;
    D2D1_SIZE_F sz = G.rt->GetSize();
    G.rt->BeginDraw();
    G.rt->Clear(rgb(C_BG));
    if (G.job) draw_scanning(sz);
    else if (G.tree) {
        draw_header(sz);
        if (G.listMode) draw_list(sz);
        else draw_rings(sz);
    }
    if (G.rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) discard_target();
}

// ---------------------------------------------------------------- actions

static void set_root(u32 node) {
    if (!G.tree || node == G.root) return;
    G.root = node;
    G.layoutDirty = true;
    G.rowsDirty = true;
    G.scroll = 0;
    G.hover = NONE;
    G.hoverRow = -1;
    if (G.listMode) rebuild_rows();
    InvalidateRect(G.view, NULL, FALSE);
}

static void go_up() {
    if (G.tree && G.root != 0) set_root(G.tree->n[G.root].parent);
}

static void set_status(const wchar_t* s) { SendMessageW(G.status, SB_SETTEXTW, 0, (LPARAM)s); }

static void cancel_scan() {
    if (!G.job) return;
    InterlockedExchange(&G.job->cancel, 1);
    WaitForSingleObject(G.scanThread, INFINITE);
    CloseHandle(G.scanThread);
    if (G.job->result) tree_free(G.job->result);
    xfree(G.job);
    G.job = NULL;
    KillTimer(G.view, 1);
}

static void start_scan(wchar_t letter) {
    cancel_scan();
    clear_segs();
    tree_free(G.tree);
    G.tree = NULL;
    xfree(G.expanded);
    G.expanded = NULL;
    G.rowCount = 0;
    update_scrollbar();
    G.job = (ScanJob*)xcalloc(sizeof(ScanJob));
    G.job->id = ++G.nextJobId;
    G.job->letter = letter;
    G.job->notify = G.main;
    G.job->phase = 1;
    G.scanStart = GetTickCount();
    G.scanThread = CreateThread(NULL, 0, scan_thread, G.job, 0, NULL);
    SetTimer(G.view, 1, 40, NULL);
    wchar_t s[64];
    fmtw(s, 64, L"Scanning %lc:\\ ...", letter);
    set_status(s);
    InvalidateRect(G.view, NULL, FALSE);
}

static void scan_done(u32 id) {
    if (!G.job || G.job->id != id) return;
    WaitForSingleObject(G.scanThread, INFINITE);
    CloseHandle(G.scanThread);
    KillTimer(G.view, 1);
    G.tree = G.job->result;
    xfree(G.job);
    G.job = NULL;
    G.root = 0;
    G.expanded = (u8*)xcalloc(G.tree->count);
    G.layoutDirty = G.rowsDirty = true;
    G.scroll = 0;
    G.hover = NONE;
    G.hoverRow = -1;
    if (G.listMode) rebuild_rows();

    wchar_t s[512], files[32];
    fmt_num(G.tree->n[0].files, files);
    int len = fmtw(s, 512, L"  Scanned %lc:\\ in %.1f s  \x00B7  %ls files  \x00B7  %ls", G.tree->letter,
                   G.tree->seconds, files, G.tree->mft ? L"fast scan (file table)" : L"standard scan");
    if (G.tree->errors) {
        wchar_t e[32];
        fmt_num(G.tree->errors, e);
        len += fmtw(s + len, 512 - len, L"  \x00B7  %ls folders couldn't be read", e);
    }
    if (!G.tree->mft) fmtw(s + len, 512 - len, L"  \x00B7  run as administrator for a faster, complete scan");
    set_status(s);
    InvalidateRect(G.view, NULL, FALSE);
}

static void context_menu(u32 node, int sx, int sy) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Open in Explorer");
    AppendMenuW(m, MF_STRING, 2, L"Copy path");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, G.view, NULL);
    DestroyMenu(m);
    if (!cmd) return;
    wchar_t path[33000];
    node_path(node, path, 32768);
    if (cmd == 1) {
        if (G.tree->n[node].flags & NF_DIR) {
            ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
        } else {
            wchar_t args[33100];
            fmtw(args, 33100, L"/select,\"%ls\"", path);
            ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
        }
    } else if (cmd == 2 && OpenClipboard(G.main)) {
        EmptyClipboard();
        size_t bytes = (wcslen(path) + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            memcpy(GlobalLock(h), path, bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        CloseClipboard();
    }
}

// ---------------------------------------------------------------- view window

static void update_hover(float x, float y) {
    u32 hover = NONE;
    bool center = false;
    int crumb = -1, row = -1;
    for (int i = 0; i < G.crumbCount; i++) {
        D2D1_RECT_F r = G.crumbs[i].rc;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) crumb = i;
    }
    if (G.tree && !G.job) {
        if (G.listMode) row = hit_list(y);
        else hover = hit_rings(x, y, &center);
    }
    bool changed = hover != G.hover || center != G.hoverCenter || crumb != G.hoverCrumb || row != G.hoverRow;
    G.hover = hover;
    G.hoverCenter = center;
    G.hoverCrumb = crumb;
    G.hoverRow = row;
    G.hand = crumb >= 0 || (center && G.root != 0) || (hover != NONE && has_children(hover)) ||
             (row >= 0 && has_children(G.rows[row].node));
    if (changed || hover != NONE) InvalidateRect(G.view, NULL, FALSE);
}

static LRESULT CALLBACK view_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (G.rt) {
            G.rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            G.rt->SetDpi((float)G.dpi, (float)G.dpi);
        }
        G.layoutDirty = true;
        if (G.listMode) update_scrollbar();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_TIMER:
        if (G.job) InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        G.mx = GET_X_LPARAM(lp) * 96.0f / G.dpi;
        G.my = GET_Y_LPARAM(lp) * 96.0f / G.dpi;
        if (!G.mouseIn) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            G.mouseIn = true;
        }
        update_hover(G.mx, G.my);
        return 0;
    }
    case WM_MOUSELEAVE:
        G.mouseIn = false;
        G.hover = NONE;
        G.hoverCenter = false;
        G.hoverCrumb = G.hoverRow = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, G.hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        return 0;
    case WM_LBUTTONUP: {
        if (!G.tree || G.job) return 0;
        if (G.hoverCrumb >= 0) { set_root(G.crumbs[G.hoverCrumb].node); return 0; }
        if (G.listMode) {
            if (G.hoverRow >= 0) {
                u32 node = G.rows[G.hoverRow].node;
                if (has_children(node)) {
                    G.expanded[node] ^= 1;
                    rebuild_rows();
                    InvalidateRect(h, NULL, FALSE);
                }
            }
        } else if (G.hoverCenter) {
            go_up();
        } else if (G.hover != NONE && has_children(G.hover)) {
            set_root(G.hover);
        }
        update_hover(G.mx, G.my);
        return 0;
    }
    case WM_RBUTTONUP: {
        if (!G.tree || G.job) return 0;
        u32 node = G.listMode ? (G.hoverRow >= 0 ? G.rows[G.hoverRow].node : NONE)
                              : (G.hoverCenter ? G.root : G.hover);
        if (node == NONE || node == FREE_SPACE) return 0;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(h, &pt);
        context_menu(node, pt.x, pt.y);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (G.listMode && G.tree) {
            G.scroll -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 3;
            update_scrollbar();
            update_hover(G.mx, G.my);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(h, SB_VERT, &si);
        int vis = list_visible();
        switch (LOWORD(wp)) {
        case SB_LINEUP: G.scroll--; break;
        case SB_LINEDOWN: G.scroll++; break;
        case SB_PAGEUP: G.scroll -= vis; break;
        case SB_PAGEDOWN: G.scroll += vis; break;
        case SB_TOP: G.scroll = 0; break;
        case SB_BOTTOM: G.scroll = (int)G.rowCount; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: G.scroll = si.nTrackPos; break;
        }
        update_scrollbar();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- main window

static void fill_drives() {
    SendMessageW(G.combo, CB_RESETCONTENT, 0, 0);
    G.driveCount = 0;
    int select = 0;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        UINT type = GetDriveTypeW(root);
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) continue;
        wchar_t label[MAX_PATH + 1] = L"", text[512], sizeStr[32] = L"";
        if (type == DRIVE_REMOTE) {
            wchar_t local[3] = { root[0], L':', 0 }, remote[MAX_PATH];
            DWORD n = MAX_PATH;
            if (WNetGetConnectionW(local, remote, &n) == NO_ERROR) fmtw(label, MAX_PATH, L"%ls", remote);
            else fmtw(label, MAX_PATH, L"Network drive");
        } else {
            if (!GetVolumeInformationW(root, label, MAX_PATH + 1, NULL, NULL, NULL, NULL, 0)) {
                if (type == DRIVE_CDROM || type == DRIVE_REMOVABLE) continue;  // no media
            }
            ULARGE_INTEGER a, t, f;
            if (GetDiskFreeSpaceExW(root, &a, &t, &f)) fmt_size(t.QuadPart, sizeStr, 32);
            if (!label[0]) fmtw(label, MAX_PATH, type == DRIVE_REMOVABLE ? L"Removable drive" : L"Local disk");
        }
        if (sizeStr[0]) fmtw(text, 512, L"%lc:   %ls   (%ls)", root[0], label, sizeStr);
        else fmtw(text, 512, L"%lc:   %ls", root[0], label);
        SendMessageW(G.combo, CB_ADDSTRING, 0, (LPARAM)text);
        if (root[0] == L'C') select = G.driveCount;
        G.driveLetter[G.driveCount++] = root[0];
    }
    SendMessageW(G.combo, CB_SETCURSEL, select, 0);
}

static wchar_t selected_drive() {
    int i = (int)SendMessageW(G.combo, CB_GETCURSEL, 0, 0);
    return i >= 0 && i < G.driveCount ? G.driveLetter[i] : L'C';
}

static void update_fonts() {
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, G.dpi);
    if (G.uiFont) DeleteObject(G.uiFont);
    G.uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
    HWND ctrls[] = { G.combo, G.rescan, G.listChk, G.freeChk, G.status };
    for (HWND c : ctrls) SendMessageW(c, WM_SETFONT, (WPARAM)G.uiFont, TRUE);
    if (G.iconFont) DeleteObject(G.iconFont);
    G.iconFont = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                             L"Segoe MDL2 Assets");
    SendMessageW(G.about, WM_SETFONT, (WPARAM)G.iconFont, TRUE);
}

static void show_about() {
    static const wchar_t license[] =
        L"MIT License\n\nCopyright (c) 2026 Jayson Brush\n\n"
        L"Permission is hereby granted, free of charge, to any person obtaining a copy of this software and "
        L"associated documentation files (the \"Software\"), to deal in the Software without restriction, "
        L"including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, "
        L"and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, "
        L"subject to the following conditions:\n\n"
        L"The above copyright notice and this permission notice shall be included in all copies or substantial "
        L"portions of the Software.\n\n"
        L"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT "
        L"LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO "
        L"EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER "
        L"IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR "
        L"THE USE OR OTHER DEALINGS IN THE SOFTWARE.";
    wchar_t title[64];
    fmtw(title, 64, L"DriveScanner %ls", APP_VERSION);
    TASKDIALOGCONFIG tc = { sizeof(tc) };
    tc.hwndParent = G.main;
    tc.hInstance = G.inst;
    tc.dwFlags = TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    tc.pszWindowTitle = L"About DriveScanner";
    tc.hMainIcon = (HICON)LoadImageW(G.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(32), S(32), 0);
    tc.pszMainInstruction = title;
    tc.pszContent = L"A small, portable disk usage viewer for Windows.\n\n"
                    L"Copyright \x00A9 2026 Jayson Brush\nReleased under the MIT License.";
    tc.pszExpandedInformation = license;
    tc.pszCollapsedControlText = L"Show license";
    tc.pszExpandedControlText = L"Hide license";
    TaskDialogIndirect(&tc, NULL, NULL, NULL);
    if (tc.hMainIcon) DestroyIcon(tc.hMainIcon);
}

static void layout_main() {
    RECT rc;
    GetClientRect(G.main, &rc);
    SendMessageW(G.status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(G.status, &sr);
    int statusH = sr.bottom - sr.top, tb = S(TOOLBAR);
    int y = S(10), h = S(28);
    MoveWindow(G.combo, S(12), y, S(320), S(400), TRUE);
    MoveWindow(G.rescan, S(342), y, S(90), h, TRUE);
    MoveWindow(G.listChk, S(448), y, S(100), h, TRUE);
    MoveWindow(G.freeChk, S(552), y, S(110), h, TRUE);
    MoveWindow(G.about, rc.right - S(12) - h, y, h, h, TRUE);
    MoveWindow(G.view, 0, tb, rc.right, rc.bottom - tb - statusH, TRUE);
}

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        G.main = h;
        G.dpi = GetDpiForWindow(h);
        G.combo = CreateWindowExW(0, WC_COMBOBOXW, NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                  0, 0, 0, 0, h, (HMENU)ID_COMBO, G.inst, NULL);
        G.rescan = CreateWindowExW(0, WC_BUTTONW, L"Rescan", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 0, 0, h, (HMENU)ID_RESCAN, G.inst, NULL);
        G.listChk = CreateWindowExW(0, WC_BUTTONW, L"List view", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    0, 0, 0, 0, h, (HMENU)ID_LIST, G.inst, NULL);
        G.freeChk = CreateWindowExW(0, WC_BUTTONW, L"Free space", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    0, 0, 0, 0, h, (HMENU)ID_FREE, G.inst, NULL);
        G.showFree = true;
        SendMessageW(G.freeChk, BM_SETCHECK, BST_CHECKED, 0);
        G.about = CreateWindowExW(0, WC_BUTTONW, L"\xE946", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  0, 0, 0, 0, h, (HMENU)ID_ABOUT, G.inst, NULL);
        G.tip = CreateWindowExW(0, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0, h, NULL, G.inst,
                                NULL);
        TTTOOLINFOW ti = { sizeof(ti) };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = h;
        ti.uId = (UINT_PTR)G.about;
        ti.lpszText = (LPWSTR)L"About";
        SendMessageW(G.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        G.status = CreateWindowExW(0, STATUSCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                   0, 0, 0, 0, h, NULL, G.inst, NULL);
        G.view = CreateWindowExW(0, L"DriveScannerView", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                 0, 0, 0, 0, h, NULL, G.inst, NULL);
        update_fonts();
        fill_drives();
        return 0;
    }
    case WM_SIZE:
        layout_main();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        RECT line = { 0, S(TOOLBAR) - 1, rc.right, S(TOOLBAR) };
        HBRUSH b = CreateSolidBrush(RGB(0xE6, 0xE6, 0xE6));
        FillRect(dc, &line, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DPICHANGED: {
        G.dpi = HIWORD(wp);
        RECT* r = (RECT*)lp;
        update_fonts();
        discard_target();
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout_main();
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        if (G.dpi) { mm->ptMinTrackSize.x = S(640); mm->ptMinTrackSize.y = S(460); }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_COMBO && HIWORD(wp) == CBN_SELCHANGE) {
            start_scan(selected_drive());
            SetFocus(G.view);
        } else if (LOWORD(wp) == ID_RESCAN) {
            start_scan(selected_drive());
            SetFocus(G.view);
        } else if (LOWORD(wp) == ID_ABOUT) {
            show_about();
            SetFocus(G.view);
        } else if (LOWORD(wp) == ID_FREE) {
            G.showFree = SendMessageW(G.freeChk, BM_GETCHECK, 0, 0) == BST_CHECKED;
            G.hover = NONE;
            G.layoutDirty = true;
            InvalidateRect(G.view, NULL, FALSE);
            SetFocus(G.view);
        } else if (LOWORD(wp) == ID_LIST) {
            G.listMode = SendMessageW(G.listChk, BM_GETCHECK, 0, 0) == BST_CHECKED;
            G.hover = NONE;
            G.hoverRow = -1;
            G.layoutDirty = true;
            if (G.listMode && G.tree) rebuild_rows();
            else update_scrollbar();
            InvalidateRect(G.view, NULL, FALSE);
            SetFocus(G.view);
        }
        return 0;
    case WM_MOUSEWHEEL:
        return SendMessageW(G.view, msg, wp, lp);
    case WM_SCANDONE:
        scan_done((u32)wp);
        return 0;
    case WM_DESTROY:
        cancel_scan();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    G.inst = inst;
    G.hover = NONE;
    G.hoverCrumb = G.hoverRow = -1;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &G.d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&G.dw))) {
        MessageBoxW(NULL, L"Direct2D is not available.", L"DriveScanner", MB_ICONERROR);
        return 1;
    }
    const wchar_t* ui = L"Segoe UI";
    G.fText = make_format(ui, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    G.fBold = make_format(ui, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    G.fRight = make_format(ui, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    G.fSmall = make_format(ui, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    G.fCenter = make_format(ui, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    G.fTitle = make_format(ui, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    G.fIcon = make_format(L"Segoe MDL2 Assets", 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = view_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"DriveScannerView";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = main_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"DriveScannerMain";
    RegisterClassExW(&wc);

    HWND h = CreateWindowExW(0, L"DriveScannerMain", L"DriveScanner", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1100, 780, NULL, NULL, inst, NULL);
    SetWindowPos(h, NULL, 0, 0, S(1100), S(780), SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(h, show);
    UpdateWindow(h);
    start_scan(selected_drive());
    SetFocus(G.view);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (m.message == WM_KEYDOWN) {
            if (m.wParam == VK_F5) { start_scan(selected_drive()); continue; }
            if (m.wParam == VK_BACK && m.hwnd != G.combo) { go_up(); continue; }
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
