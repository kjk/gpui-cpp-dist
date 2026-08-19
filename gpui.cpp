#include "gpui.h"

#include <cctype>
#include <climits>
#include <cstdarg>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#line 1 "src/Base.cpp"

namespace gpui {

template <typename T, size_t N>
char (&DimofSizeHelper(T (&array)[N]))[N];
#define dimof(array) (sizeof(DimofSizeHelper(array)))

static bool StrEqNI(Str s1, Str s2, int n);
static int VsnprintfUtf8(Str buf, const char* fmt, va_list args);

void* AllocZero(int count, int size) {
    return calloc(count, size);
}

static_assert(sizeof(Arena) <= kArenaHeaderSize,
              "Arena header must fit in reserved header bytes");

using ArenaFlags = uint64_t;
enum : ArenaFlags {
    ArenaFlagNoChain = 1ull << 0,
    ArenaFlagLargePages = 1ull << 1,
};

struct ArenaParams {
    ArenaFlags flags = 0;
    uint64_t reserveSize = 0;
    uint64_t commitSize = 0;
    void* optionalBackingBuffer = nullptr;
    const char* allocationSiteFile = nullptr;
    int allocationSiteLine = 0;
    const char* name = nullptr;
};

static uint64_t gArenaDefaultReserveSize = 64ull * 1024ull * 1024ull;
static uint64_t gArenaDefaultCommitSize = 64ull * 1024ull;
static ArenaFlags gArenaDefaultFlags = 0;

static uint64_t ArenaAlignPow2(uint64_t value, uint64_t align) {
    if (align <= 1) {
        return value;
    }
    return (value + align - 1) & ~(align - 1);
}

static uint64_t ArenaMin(uint64_t a, uint64_t b) {
    return (a < b) ? a : b;
}

static uint64_t ArenaMax(uint64_t a, uint64_t b) {
    return (a > b) ? a : b;
}

static uint64_t ArenaClampTop(uint64_t value, uint64_t maxValue) {
    return (value < maxValue) ? value : maxValue;
}

static uint64_t ArenaClampBot(uint64_t minValue, uint64_t value) {
    return (value > minValue) ? value : minValue;
}

static Arena* ArenaAlloc(const ArenaParams& params);

static void ArenaRelease(Arena* arena) {
    PlatMemRelease(arena, arena->reserved);
}

static void* ArenaPushLocked(Arena* arena, uint64_t size, uint64_t align,
                             bool zero) {
    if (!arena) {
        return nullptr;
    }
    if (align == 0) {
        align = 1;
    }

    Arena* current = arena->current;
    uint64_t posPre = ArenaAlignPow2(current->pos, align);
    uint64_t posPost = posPre + size;

    uint64_t sizeToZero = 0;
    if (zero && current->committed > posPre) {
        sizeToZero = ArenaMin(current->committed, posPost) - posPre;
    }

    if (current->reserved < posPost && !(arena->flags & ArenaFlagNoChain)) {
        uint64_t reserveChunkSize = current->reserveChunkSize;
        uint64_t commitChunkSize = current->commitChunkSize;
        if (size + kArenaHeaderSize > reserveChunkSize) {
            reserveChunkSize = ArenaAlignPow2(size + kArenaHeaderSize,
                                              ArenaMax(align, PlatPageSize()));
            commitChunkSize = reserveChunkSize;
        }

        ArenaParams newParams = {};
        newParams.flags = current->flags;
        newParams.reserveSize = reserveChunkSize;
        newParams.commitSize = commitChunkSize;
        newParams.allocationSiteFile = current->allocationSiteFile;
        newParams.allocationSiteLine = current->allocationSiteLine;
        newParams.name = current->name;

        Arena* newBlock = ArenaAlloc(newParams);
        if (!newBlock) {
            return nullptr;
        }

        newBlock->basePos = current->basePos + current->reserved;
        newBlock->prev = current;
        arena->current = newBlock;
        current = newBlock;
        posPre = ArenaAlignPow2(current->pos, align);
        posPost = posPre + size;
        sizeToZero = 0;
    }

    if (current->committed < posPost) {
        if (current->flags & ArenaFlagLargePages) {
            return nullptr;
        }

        uint64_t commitEnd = ArenaAlignPow2(posPost, current->commitChunkSize);
        uint64_t commitClamped = ArenaClampTop(commitEnd, current->reserved);
        uint64_t commitSize = commitClamped - current->committed;
        void* commitPtr = (char*)current + current->committed;
        if (!PlatMemCommit(commitPtr, commitSize, false)) {
            return nullptr;
        }
        current->committed = commitClamped;
    }

    if (current->committed < posPost) {
        return nullptr;
    }

    void* result = (char*)current + posPre;
    current->pos = posPost;

    arena->nAllocsLifetime++;
    arena->nAllocsSinceReset++;
    uint64_t used = current->basePos + posPost;
    arena->peakBytesLifetime = std::max(used, arena->peakBytesLifetime);
    arena->peakBytesSinceReset = std::max(used, arena->peakBytesSinceReset);

    if (sizeToZero) {
        memset(result, 0, (size_t)sizeToZero);
    }
    return result;
}

static ArenaParams ArenaDefaultParams() {
    ArenaParams params = {};
    params.flags = gArenaDefaultFlags;
    params.reserveSize = gArenaDefaultReserveSize;
    params.commitSize = gArenaDefaultCommitSize;
    return params;
}

Arena* ArenaNew() {
    return ArenaAlloc(ArenaDefaultParams());
}

static Arena* ArenaAlloc(const ArenaParams& srcParams) {
    ArenaParams params = srcParams;
    if (params.reserveSize == 0) {
        params.reserveSize = gArenaDefaultReserveSize;
    }
    if (params.commitSize == 0) {
        params.commitSize = gArenaDefaultCommitSize;
    }

    bool useLargePages = (params.flags & ArenaFlagLargePages) != 0;
    const uint64_t pageSize =
        useLargePages ? PlatLargePageSize() : PlatPageSize();
    uint64_t reserveSize = ArenaAlignPow2(
        ArenaMax(params.reserveSize, kArenaHeaderSize), pageSize);
    uint64_t commitSize =
        ArenaAlignPow2(ArenaMax(params.commitSize, kArenaHeaderSize), pageSize);
    commitSize = ArenaClampTop(commitSize, reserveSize);

    void* base = params.optionalBackingBuffer;
    bool usesExternalBuffer = (base != nullptr);
    ArenaFlags actualFlags = params.flags;

    if (!usesExternalBuffer) {
        if (useLargePages) {
            base = PlatMemReserveCommit(reserveSize, true);
            if (base) {
                commitSize = reserveSize;
            } else {
                actualFlags &= ~ArenaFlagLargePages;
                useLargePages = false;
                reserveSize = ArenaAlignPow2(reserveSize, PlatPageSize());
                commitSize = ArenaAlignPow2(commitSize, PlatPageSize());
            }
        }

        if (!base) {
            base = PlatMemReserve(reserveSize);
            if (base && !PlatMemCommit(base, commitSize, false)) {
                PlatMemRelease(base, reserveSize);
                base = nullptr;
            }
        }
    } else {
        commitSize = reserveSize;
    }

    if (!base) {
        return nullptr;
    }

    memset(base, 0, (size_t)std::min<uint64_t>(commitSize, kArenaHeaderSize));
    Arena* arena = (Arena*)base;
    arena->prev = nullptr;
    arena->current = arena;
    arena->flags = actualFlags;
    arena->commitChunkSize = useLargePages ? reserveSize : commitSize;
    arena->reserveChunkSize = reserveSize;
    arena->basePos = 0;
    arena->pos = kArenaHeaderSize;
    arena->committed = commitSize;
    arena->reserved = reserveSize;
    arena->allocationSiteFile = params.allocationSiteFile;
    arena->allocationSiteLine = params.allocationSiteLine;
    arena->name = params.name;
    arena->usesExternalBuffer = usesExternalBuffer;
    arena->nAllocsLifetime = 0;
    arena->peakBytesLifetime = 0;
    arena->nAllocsSinceReset = 0;
    arena->peakBytesSinceReset = 0;
    return arena;
}

void ArenaDelete(Arena* arena) {
    if (!arena) {
        return;
    }

    Arena* node = arena->current;
    while (node) {
        Arena* prev = node->prev;
        if (!node->usesExternalBuffer) {
            ArenaRelease(node);
        }
        node = prev;
    }
}

void* Arena::Push(uint64_t size, uint64_t align, bool zero) {
    lock.Lock();
    void* mem = ArenaPushLocked(this, size, align, zero);
    lock.Unlock();
    return mem;
}

void Arena::PopTo(uint64_t popPos) {
    Arena* arena = this;
    lock.Lock();

    uint64_t bigPos = ArenaClampBot(kArenaHeaderSize, popPos);
    Arena* node = arena->current;
    while (node && node->basePos >= bigPos) {
        Arena* prevNode = node->prev;
        if (!node->usesExternalBuffer) {
            ArenaRelease(node);
        } else {
            node->pos = kArenaHeaderSize;
        }
        node = prevNode;
    }

    if (!node) {
        lock.Unlock();
        return;
    }

    arena->current = node;
    uint64_t newPos = bigPos - node->basePos;
    node->pos = newPos;
    lock.Unlock();
}

void* Arena::Alloc(int size) {
    if (size <= 0) {
        return nullptr;
    }
    return Push((uint64_t)size, 8, false);
}

void Arena::Reset() {
    PopTo(0);
    nAllocsSinceReset = 0;
    peakBytesSinceReset = 0;
}

void* Alloc(Arena* arena, int size) {
    if (size <= 0) {
        return nullptr;
    }
    if (!arena) {
        return malloc(size);
    }
    return arena->Alloc(size);
}

void Free(Arena* arena, void* mem) {

    if (arena) return;
    free(mem);
}

static void* Alloc(Arena* arena, size_t size) {
    if (size == 0) {
        return nullptr;
    }
    if (!arena) {
        return malloc(size);
    }
    return arena->Push((uint64_t)size, 8, false);
}

static void* Realloc(Arena* arena, void* mem, size_t newSize, size_t copySize) {
    if (!arena) {
        return realloc(mem, newSize);
    }

    if (newSize == 0) {
        return nullptr;
    }
    void* newMem = arena->Push((uint64_t)newSize, 8, false);
    if (newMem && mem && copySize > 0) {

        size_t n = copySize;
        n = std::min(n, newSize);
        memmove(newMem, mem, n);
    }
    return newMem;
}

static void* MemDup(Arena* arena, const void* mem, size_t size,
                    size_t extraBytes = 0) {
    void* newMem = Alloc(arena, size + extraBytes);
    if (!newMem) {
        return nullptr;
    }
    if (mem && size) {
        memcpy(newMem, mem, size);
    }

    if (extraBytes > 0) {
        memset((char*)newMem + size, 0, extraBytes);
    }
    return newMem;
}

static thread_local Arena* gTempArena = nullptr;

Arena* GetTempArena() {
    if (!gTempArena) {
        gTempArena = ArenaNew();
    }
    return gTempArena;
}

void ResetTempArena() {
    if (gTempArena) {
        gTempArena->Reset();
    }
}

void DestroyTempArena() {
    ArenaDelete(gTempArena);
    gTempArena = nullptr;
}

Str AllocStrTemp(int size) {
    if (size == 0) {
        return {};
    }
    Arena* arena = GetTempArena();
    char* res = (char*)arena->Push((uint64_t)size + 1, 1, false);
    res[size] = 0;
    return Str(res, size);
}

GPUI_NOINLINE bool VecRealloc(Arena* a, void** els, int len, int* cap,
                              int newCap, int elSize) {

    if (elSize <= 0 || newCap < 0 || newCap > INT_MAX - 1) {
        return false;
    }
    int newElCount = newCap + 1;
    if (newElCount > INT_MAX / elSize) {
        return false;
    }

    int keep = len;
    keep = std::max(keep, 0);
    keep = std::min(keep, newCap);
    int oldSize = keep * elSize;
    int allocSize = newElCount * elSize;

    void* newEls = Realloc(a, *els, (size_t)allocSize, (size_t)oldSize);
    if (!newEls) {
        return false;
    }
    int tail = allocSize - oldSize;
    if (tail > 0) {
        memset((char*)newEls + oldSize, 0, (size_t)tail);
    }
    *els = newEls;
    *cap = newCap;
    return true;
}

static bool StrIsNull(const Str& s) {
    return !s.s;
}

static Str WrapAllocated(char* s, int cch = -1) {
    if (!s) {
        return {};
    }
    if (cch < 0) {
        return Str(s);
    }
    return Str(s, cch);
}

Str StrDup(Arena* a, Str s) {
    if (StrIsNull(s) || s.len < 0) {
        return {};
    }
    int cch = s.len;
    return WrapAllocated(
        (char*)MemDup(a, s.s, (size_t)cch * sizeof(char), sizeof(char)), cch);
}

Str StrDup(Str s) {
    return StrDup(nullptr, s);
}

void StrFree(Str s) {
    free(s.s);
}

static bool DateParseIso(const char* s, LocalDate* out) {
    int part[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        if (i > 0) {
            if (*s != '-') {
                return false;
            }
            s++;
        }
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            part[i] = part[i] * 10 + (*s - '0');
            s++;
            digits++;
        }
        if (digits == 0 || digits > 4) {
            return false;
        }
    }
    if (*s != 0) {
        return false;
    }
    if (part[0] < 1 || part[1] < 1 || part[1] > 12 || part[2] < 1 ||
        part[2] > 31) {
        return false;
    }
    out->year = part[0];
    out->month = part[1];
    out->day = part[2];
    return true;
}

static bool gTodayChecked = false;
static LocalDate gTodayPinned = {};

LocalDate DateToday() {
    if (!gTodayChecked) {
        gTodayChecked = true;
        const char* env = getenv("GPUI_TODAY");
        if (env) {
            LocalDate pinned;
            if (DateParseIso(env, &pinned)) {
                gTodayPinned = pinned;
            }
        }
    }
    if (gTodayPinned.year != 0) {
        return gTodayPinned;
    }
    LocalDate out;
    time_t now = time(nullptr);
    struct tm* lt = localtime(&now);
    if (!lt) {
        return out;
    }
    out.year = lt->tm_year + 1900;
    out.month = lt->tm_mon + 1;
    out.day = lt->tm_mday;
    return out;
}

LocalDate DateAddDays(LocalDate base, int days) {
    struct tm t = {};
    t.tm_year = base.year - 1900;
    t.tm_mon = base.month - 1;
    t.tm_mday = base.day + days;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    time_t stamp = mktime(&t);
    if (stamp == (time_t)-1) {
        return base;
    }
    LocalDate out;
    out.year = t.tm_year + 1900;
    out.month = t.tm_mon + 1;
    out.day = t.tm_mday;
    return out;
}

void StrLowerAscii(char* s) {
    if (!s) {
        return;
    }
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s - 'A' + 'a');
        }
    }
}

bool StrEqI(Str s1, Str s2) {
    if (s1.s == s2.s) {
        return true;
    }
    if (s1.len != s2.len) {
        return false;
    }
    if (s1.len == 0) {
        return true;
    }
    if (StrIsNull(s1) || StrIsNull(s2)) {
        return false;
    }
    return 0 == StrCmpNI(s1.s, s2.s, s1.len);
}

bool StrContainsI(Str s, Str sub) {
    if (!s || !sub || sub.len <= 0) {
        return false;
    }
    for (int off = 0; off + sub.len <= s.len; off++) {
        if (StrEqNI(Str(s.s + off, s.len - off), sub, sub.len)) {
            return true;
        }
    }
    return false;
}

static bool StrEqNI(Str s1, Str s2, int n) {
    if (s1.s == s2.s) {
        return true;
    }
    if (!s1 || !s2 || n == 0) {
        return n == 0;
    }
    if (s1.len < n || s2.len < n) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (tolower(s1.s[i]) != tolower(s2.s[i])) {
            return false;
        }
    }
    return true;
}

static bool IsDigit(char c) {
    return ('0' <= c) && (c <= '9');
}

static constexpr int kPadding = 1;

static bool IsExternalOrEmpty(const StrBuilder* s) {
    return !s->els || (s->buf.s && s->els == s->buf.s);
}

static char* EnsureCap(StrBuilder* s, int needed) {

    if (IsExternalOrEmpty(s) && s->buf.s && needed + kPadding <= s->buf.len) {
        s->els = s->buf.s;
        return s->els;
    }

    int capacityHint = s->cap;

    if (IsExternalOrEmpty(s)) {
        s->cap = 0;
    }

    if (s->els && s->cap >= needed) {
        return s->els;
    }

    int newCap = s->cap * 2;
    newCap = std::max(needed, newCap);
    newCap = std::max(newCap, capacityHint);

    int newElCount = newCap + kPadding;

    int allocSize = newElCount;
    char* newEls;
    if (IsExternalOrEmpty(s)) {
        newEls = (char*)Alloc(s->a, allocSize);
        if (newEls && s->els && s->len > 0) {
            memcpy(newEls, s->els, (size_t)s->len + 1);
        } else if (newEls) {
            newEls[0] = 0;
        }
    } else {
        newEls = (char*)Realloc(s->a, s->els, (size_t)allocSize,
                                (size_t)s->len + kPadding);
    }
    if (!newEls) {
        return nullptr;
    }
    s->els = newEls;
    s->cap = newCap;
    return newEls;
}

static char* MakeSpaceAt(StrBuilder* s, int idx, int count) {
    int newLen = std::max(s->len, idx) + count;
    char* buf = EnsureCap(s, newLen);
    if (!buf) {
        return nullptr;
    }
    buf[newLen] = 0;
    char* res = &(buf[idx]);
    if (s->len > idx) {

        char* src = buf + idx;
        char* dst = buf + idx + count;
        memmove(dst, src, (size_t)(s->len - idx));
    }
    s->len = newLen;

    return res;
}

static void StrBuilderReset(StrBuilder* s) {
    s->len = 0;

    if (!s->els || (s->buf.s && s->els == s->buf.s)) {
        s->els = s->buf.s;
    }
    if (s->els) {
        s->els[0] = 0;
    }
}

static void StrBuilderFree(StrBuilder* s) {
    if (s->els && !(s->buf.s && s->els == s->buf.s)) {
        Free(s->a, s->els);
    }
    s->len = 0;
    s->cap = 0;
    s->els = s->buf.s;
    if (s->els) {
        s->els[0] = 0;
    }
}

void StrBuilder::Reset(Str s) {
    StrBuilderReset(this);
    Append(s);
}

StrBuilder::StrBuilder(Str externalBuf) {
    this->buf = externalBuf;
    Reset();
}

StrBuilder::~StrBuilder() {
    StrBuilderFree(this);
}

bool StrBuilder::InsertAt(int idx, char el) {
    char* p = MakeSpaceAt(this, idx, 1);
    if (!p) {
        return false;
    }
    p[0] = el;
    return true;
}

bool StrBuilder::AppendChar(char c) {
    return InsertAt(len, c);
}

bool StrBuilder::Append(Str src) {
    if (StrIsNull(src) || 0 == src.len) {
        return true;
    }
    char* dst = MakeSpaceAt(this, len, src.len);
    if (!dst) {
        return false;
    }
    memcpy(dst, src.s, (size_t)src.len);
    return true;
}

Str StrBuilder::TakeStr() {
    int n = len;
    char* res = els;
    if (!els || n == 0) {
        Reset();
        return Str{};
    }
    if (buf.s && els == buf.s) {

        res = (char*)MemDup(this->a, els, (size_t)n + kPadding);
        els = buf.s;
    } else {

        els = buf.s;
    }

    Reset();
    return Str(res, n);
}

struct Inst {
    FmtArg::Kind t = FmtArg::Kind::None;
    int argNo = 0;
    int rawOff = 0;

    int sLen = 0;

    char conv = 0;
    int intBits = 0;
    int fwpOff = 0;
    int fwpLen = 0;
    int width = 0;
    int prec = -1;
    bool leftJust = false;
};

struct Fmt {
    Fmt() = default;
    ~Fmt() = default;

    bool Eval(const FmtArg** args, int nArgs);

    bool isOk =
        true;

    Str format;
    Inst instructions[32]{};
    int nInst = 0;

    int currArgNo = 0;
    int currPercArgNo = 0;
    StrBuilder res;

    char buf[256] = {};
};

static void addRawStr(Fmt& fmt, int off, size_t n) {
    if (n == 0) {
        return;
    }
    auto& i = fmt.instructions[fmt.nInst++];
    i.t = FmtArg::Kind::RawStr;
    i.rawOff = off;
    i.sLen = (int)n;
    i.argNo = -1;
}

static int parseArgDefBrace(Fmt& fmt, int off) {
    off++;
    int n = 0;
    bool positional = false;

    while (off < fmt.format.len && fmt.format.s[off] != '}') {
        if (!IsDigit(fmt.format.s[off])) {
            fmt.isOk = false;
            return off;
        }
        n = (n * 10) + (fmt.format.s[off] - '0');
        positional = true;
        off++;
    }
    if (off >= fmt.format.len) {
        fmt.isOk = false;
        return off;
    }
    if (fmt.nInst >= (int)dimof(fmt.instructions)) {
        fmt.isOk = false;
        return off;
    }
    auto& i = fmt.instructions[fmt.nInst++];
    i.t = FmtArg::Kind::Any;

    i.argNo = positional ? n : fmt.currPercArgNo++;
    return off + 1;
}

static FmtArg::Kind typeFromConv(char c) {
    switch (c) {
        case 'c':
            return FmtArg::Kind::Char;
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            return FmtArg::Kind::Int;
        case 'p':
            return FmtArg::Kind::Ptr;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            return FmtArg::Kind::Float;
        case 's':
        case 'S':
            return FmtArg::Kind::Str;
        case 'v':
            return FmtArg::Kind::Any;
    }
    return FmtArg::Kind::None;
}

static bool startsWith(Str s, int off, const char* prefix) {
    int i = 0;
    while (prefix[i]) {
        if (off + i >= s.len || s.s[off + i] != prefix[i]) {
            return false;
        }
        i++;
    }
    return true;
}

static int parseArgDefPerc(Fmt& fmt, int off) {
    Str f = fmt.format;
    off++;
    int fwpStart = off;
    bool leftJust = false;

    while (off < f.len &&
           (f.s[off] == '-' || f.s[off] == '+' || f.s[off] == ' ' ||
            f.s[off] == '0' || f.s[off] == '#')) {
        if (f.s[off] == '-') {
            leftJust = true;
        }
        off++;
    }

    int width = 0;
    while (off < f.len && IsDigit(f.s[off])) {
        width = (width * 10) + (f.s[off] - '0');
        off++;
    }

    int prec = -1;
    if (off < f.len && f.s[off] == '.') {
        off++;
        prec = 0;
        while (off < f.len && IsDigit(f.s[off])) {
            prec = (prec * 10) + (f.s[off] - '0');
            off++;
        }
    }
    int fwpEnd = off;

    int bits = 32;
    char lenMod = (off < f.len) ? f.s[off] : 0;
    bool is32BitLenMod =
        lenMod == 'l' || lenMod == 'h' || lenMod == 'L' || lenMod == 'w';

    bool is64BitLenMod =
        lenMod == 'z' || lenMod == 'j' || lenMod == 't' || lenMod == 'I';
    if (startsWith(f, off, "I64")) {
        bits = 64;
        off += 3;
    } else if (startsWith(f, off, "I32")) {
        off += 3;
    } else if (startsWith(f, off, "ll")) {
        bits = 64;
        off += 2;
    } else if (startsWith(f, off, "hh")) {
        off += 2;
    } else if (is32BitLenMod) {
        off++;
    } else if (is64BitLenMod) {
        bits = 64;
        off++;
    }
    char conv = (off < f.len) ? f.s[off] : 0;
    off++;

    auto& i = fmt.instructions[fmt.nInst++];
    i.t = typeFromConv(conv);
    i.argNo = fmt.currPercArgNo++;
    i.conv = conv;
    i.intBits = bits;
    i.fwpOff = fwpStart;
    i.fwpLen = fwpEnd - fwpStart;
    i.width = width;
    i.prec = prec;
    i.leftJust = leftJust;
    return off;
}

static bool hasInstructionWithArgNo(Inst* insts, int nInst, int argNo) {
    for (int i = 0; i < nInst; i++) {
        if (insts[i].argNo == argNo) {
            return true;
        }
    }
    return false;
}

static bool isIntLike(FmtArg::Kind t) {
    return t == FmtArg::Kind::Char || t == FmtArg::Kind::Int ||
           t == FmtArg::Kind::Ptr;
}

static bool validArgTypes(FmtArg::Kind instType, FmtArg::Kind argType) {
    if (instType == FmtArg::Kind::Any || instType == FmtArg::Kind::RawStr) {
        return true;
    }

    if (instType == FmtArg::Kind::Char || instType == FmtArg::Kind::Int ||
        instType == FmtArg::Kind::Ptr) {
        return isIntLike(argType);
    }
    if (instType == FmtArg::Kind::Float) {
        return argType == FmtArg::Kind::Float ||
               argType == FmtArg::Kind::Double;
    }
    if (instType == FmtArg::Kind::Str) {
        return argType == FmtArg::Kind::Str;
    }
    return false;
}

static bool ParseFormat(Fmt& o, Str fmtStr) {
    o.format = fmtStr;
    o.nInst = 0;
    o.currPercArgNo = 0;
    o.currArgNo = 0;
    o.res.Reset();

    int start = 0;
    int off = 0;
    while (off < fmtStr.len && fmtStr.s[off]) {
        char c = fmtStr.s[off];
        if ('%' == c) {

            if (off + 1 < fmtStr.len && '%' == fmtStr.s[off + 1]) {
                addRawStr(o, start, off - start);
                start = off + 1;
                off += 2;
                continue;
            }
            addRawStr(o, start, off - start);
            if (off + 1 < fmtStr.len && '{' == fmtStr.s[off + 1]) {
                off = parseArgDefBrace(o, off + 1);
            } else {
                off = parseArgDefPerc(o, off);
            }
            start = off;
            continue;
        }
        off++;
    }
    addRawStr(o, start, off - start);

    int maxArgNo = -1;

    for (int i = 0; i < o.nInst; i++) {
        if (o.instructions[i].t == FmtArg::Kind::RawStr) {
            continue;
        }
        maxArgNo = std::max(o.instructions[i].argNo, maxArgNo);
    }

    for (int i = 0; i <= maxArgNo; i++) {
        bool isOk = hasInstructionWithArgNo(o.instructions, o.nInst, i);
        if (!isOk) {
            return false;
        }
    }
    return true;
}

static void bufFmt(Str buf, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    VsnprintfUtf8(buf, fmt, args);
    va_end(args);
    buf.s[buf.len - 1] = 0;
}

static void evalDefault(Fmt& fmt, const FmtArg& arg) {
    TempStr s;
    Str buf(fmt.buf, (int)dimof(fmt.buf));
    switch (arg.t) {
        case FmtArg::Kind::Char:
            fmt.res.AppendChar(arg.c);
            break;
        case FmtArg::Kind::Int:
            bufFmt(buf, "%lld", (long long)arg.i);
            fmt.res.Append(fmt.buf);
            break;
        case FmtArg::Kind::Ptr:
            bufFmt(buf, "%p", arg.ptr);
            fmt.res.Append(fmt.buf);
            break;
        case FmtArg::Kind::Float:

            bufFmt(buf, "%G", (double)arg.f);
            fmt.res.Append(fmt.buf);
            break;
        case FmtArg::Kind::Double:
            bufFmt(buf, "%G", arg.d);
            fmt.res.Append(fmt.buf);
            break;
        case FmtArg::Kind::Str:
            fmt.res.Append(arg.str);
            break;
        default:
            break;
    }
}

static int64_t argToI64(const FmtArg& arg) {
    switch (arg.t) {
        case FmtArg::Kind::Char:
            return (int64_t)arg.c;
        case FmtArg::Kind::Ptr:
            return (int64_t)(intptr_t)arg.ptr;
        default:
            return arg.i;
    }
}

static void evalPercInst(Fmt& fmt, const Inst& inst, const FmtArg& arg) {
    char* buf = fmt.buf;
    Str bufS(fmt.buf, (int)dimof(fmt.buf));

    if (inst.conv == 's' || inst.conv == 'S') {
        Str sv = arg.str;
        int slen = sv.len;
        if (inst.prec >= 0 && inst.prec < slen) {
            slen = inst.prec;
        }
        int pad = inst.width - slen;
        pad = std::max(pad, 0);
        if (!inst.leftJust) {
            for (int j = 0; j < pad; j++) {
                fmt.res.AppendChar(' ');
            }
        }
        fmt.res.Append(Str(sv.s, slen));
        if (inst.leftJust) {
            for (int j = 0; j < pad; j++) {
                fmt.res.AppendChar(' ');
            }
        }
        return;
    }

    char fbuf[64];
    int k = 0;
    fbuf[k++] = '%';
    for (int j = 0; j < inst.fwpLen && k < (int)dimof(fbuf) - 5; j++) {
        fbuf[k++] = fmt.format.s[inst.fwpOff + j];
    }
    char conv = inst.conv;
    int64_t ival = argToI64(arg);
    switch (conv) {
        case 'd':
        case 'i':
            if (inst.intBits == 64) {
                fbuf[k++] = 'l';
                fbuf[k++] = 'l';
                fbuf[k++] = 'd';
                fbuf[k] = 0;
                bufFmt(bufS, fbuf, (long long)ival);
            } else {
                fbuf[k++] = 'd';
                fbuf[k] = 0;
                bufFmt(bufS, fbuf, (int)ival);
            }
            fmt.res.Append(buf);
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            if (inst.intBits == 64) {
                fbuf[k++] = 'l';
                fbuf[k++] = 'l';
                fbuf[k++] = conv;
                fbuf[k] = 0;
                bufFmt(bufS, fbuf, (unsigned long long)ival);
            } else {
                fbuf[k++] = conv;
                fbuf[k] = 0;
                bufFmt(bufS, fbuf, (unsigned int)(unsigned long long)ival);
            }
            fmt.res.Append(buf);
            break;
        case 'c':
            fbuf[k++] = 'c';
            fbuf[k] = 0;
            bufFmt(bufS, fbuf, (int)ival);
            fmt.res.Append(buf);
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            fbuf[k++] = conv;
            fbuf[k] = 0;
            double dv = (arg.t == FmtArg::Kind::Double) ? arg.d : (double)arg.f;
            bufFmt(bufS, fbuf, dv);
            fmt.res.Append(buf);
        } break;
        case 'p': {

            const void* pv = (arg.t == FmtArg::Kind::Ptr)
                                 ? arg.ptr
                                 : (const void*)(intptr_t)ival;
            bufFmt(bufS, "%p", pv);
            fmt.res.Append(buf);
        } break;
        default:
            break;
    }
}

bool Fmt::Eval(const FmtArg** args, int nArgs) {
    if (!isOk) {

        return false;
    }

    for (int n = 0; n < nInst; n++) {
        auto& inst = instructions[n];

        if (inst.t == FmtArg::Kind::RawStr) {
            res.Append(Str(format.s + inst.rawOff, inst.sLen));
            continue;
        }

        int argNo = inst.argNo;
        if (argNo < 0 || argNo >= nArgs) {
            isOk = false;
            return false;
        }

        const FmtArg& arg = *args[argNo];
        isOk = validArgTypes(inst.t, arg.t);
        if (!isOk) {
            return false;
        }

        if (inst.t == FmtArg::Kind::Any) {
            evalDefault(*this, arg);
        } else {
            evalPercInst(*this, inst, arg);
        }
    }
    return true;
}

static Str FormatArgs(Arena* a, const char* fmt, const FmtArg** args,
                      int nArgs) {

    while (nArgs > 0 && args[nArgs - 1]->t == FmtArg::Kind::None) {
        nArgs--;
    }

    if (nArgs == 0) {

        bool hasDirective = false;
        for (const char* p = fmt; p && *p; p++) {
            if (*p == '%') {
                hasDirective = true;
                break;
            }
        }
        if (!hasDirective) {
            return StrDup(a, Str(fmt));
        }
    }

    Fmt f;

    f.res.a = a;
    bool ok = ParseFormat(f, fmt);
    if (!ok) {
        return {};
    }
    ok = f.Eval(args, nArgs);
    if (!ok) {
        return {};
    }
    return f.res.TakeStr();
}

TempStr FormatTempArgs(const char* fmt, const FmtArg** args, int nArgs) {
    return FormatArgs(GetTempArena(), fmt, args, nArgs);
}

#if defined(_MSC_VER)
static _locale_t GetUtf8FormatLocale() {

    struct Locale {
        _locale_t loc = _create_locale(LC_ALL, ".UTF-8");
        ~Locale() {
            if (loc) {
                _free_locale(loc);
                loc = nullptr;
            }
        }
    };
    static Locale l;
    return l.loc;
}
#endif

static int VsnprintfUtf8(Str buf, const char* fmt, va_list args) {
#if defined(_MSC_VER)
    _locale_t loc = GetUtf8FormatLocale();
    if (loc) {
        return _vsnprintf_l(buf.s, (size_t)buf.len, fmt, loc, args);
    }
#endif
    return vsnprintf(buf.s, (size_t)buf.len, fmt, args);
}
}

#line 1 "src/gpui/Assets.cpp"

namespace gpui {

#if GPUI_OS_WINDOWS
static const char kSep = '\\';
#else
static const char kSep = '/';
#endif

static const int kMaxRoots = 12;
static char gRoots[kMaxRoots][kMaxPath];
static int gRootN = 0;

void AssetsClear() {
    gRootN = 0;
}

static void AddRootRaw(const char* dir) {
    if (!dir || !dir[0] || gRootN >= kMaxRoots) {
        return;
    }
    for (int i = 0; i < gRootN; i++) {
        if (StrCmpI(gRoots[i], dir) == 0) {
            return;
        }
    }
    if (!PlatDirExists(dir)) {
        return;
    }
    StrCopyZ(gRoots[gRootN], kMaxPath, dir);
    gRootN++;
}

void AssetsAddRoot(Str dir) {
    if (!dir.s || dir.len <= 0) {
        return;
    }
    char buf[kMaxPath];
    int n = dir.len < kMaxPath - 1 ? dir.len : kMaxPath - 1;
    memcpy(buf, dir.s, (size_t)n);
    buf[n] = 0;
    AddRootRaw(buf);
}

static void JoinPath(char* dst, int dstN, const char* a, const char* b) {
    if (!a || !a[0]) {
        StrCopyZ(dst, dstN, b ? b : "");
        return;
    }
    if (!b || !b[0]) {
        StrCopyZ(dst, dstN, a);
        return;
    }

    StrCopyZ(dst, dstN, a);
    int n = (int)strlen(dst);
    if (n + 1 < dstN) {
        dst[n++] = kSep;
        dst[n] = 0;
        StrCopyZ(dst + n, dstN - n, b);
    }
}

static void ParentDir(char* path) {
    int n = (int)strlen(path);
    while (n > 0 && (path[n - 1] == '\\' || path[n - 1] == '/')) {
        path[--n] = 0;
    }
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') {
        path[--n] = 0;
    }
    while (n > 0 && (path[n - 1] == '\\' || path[n - 1] == '/')) {
        path[--n] = 0;
    }
}

static void ToNativeSep(char* s) {
    for (; *s; s++) {
        if (*s == '/' || *s == '\\') {
            *s = kSep;
        }
    }
}

void AssetsAddDefaultRoots(Str exampleName) {
    char cwd[kMaxPath] = {};
    PlatGetCwd(cwd, kMaxPath);

    char exe[kMaxPath] = {};
    PlatGetExeDir(exe, kMaxPath);

    char sub[kMaxPath];
    if (exampleName.s && exampleName.len > 0) {
        snprintf(sub, kMaxPath, "assets%c%s", kSep, exampleName.s);
    } else {
        StrCopyZ(sub, kMaxPath, "assets");
    }

    char p[kMaxPath];
    JoinPath(p, kMaxPath, cwd, sub);
    AddRootRaw(p);
    JoinPath(p, kMaxPath, exe, sub);
    AddRootRaw(p);

    char walk[kMaxPath];
    for (int src = 0; src < 2; src++) {
        StrCopyZ(walk, kMaxPath, src == 0 ? cwd : exe);
        for (int up = 0; up < 6; up++) {
            JoinPath(p, kMaxPath, walk, sub);
            AddRootRaw(p);
            if (exampleName.s) {

                char rust[kMaxPath];
                snprintf(rust, kMaxPath, "examples%c%s%cassets", kSep,
                         exampleName.s, kSep);
                JoinPath(p, kMaxPath, walk, rust);
                AddRootRaw(p);
                snprintf(rust, kMaxPath,
                         ".work%cgpui-component%cexamples%c%s%cassets", kSep,
                         kSep, kSep, exampleName.s, kSep);
                JoinPath(p, kMaxPath, walk, rust);
                AddRootRaw(p);
            }
            char prev[kMaxPath];
            StrCopyZ(prev, kMaxPath, walk);
            ParentDir(walk);
            if (!walk[0] || StrCmpI(prev, walk) == 0) {
                break;
            }
        }
    }
}

static bool ReadFileAll(const char* path, Vec<uint8_t>* out) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size < 0 || size > 8 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    rewind(f);
    out->Reset();
    int n = (int)size;
    if (n == 0) {
        fclose(f);
        return true;
    }
    uint8_t* buf = out->AppendBlanks(n);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if ((int)got != n) {
        out->Reset();
        return false;
    }
    return true;
}

bool AssetsLoad(Str relPath, Vec<uint8_t>* out) {
    if (!relPath.s || relPath.len <= 0 || !out) {
        return false;
    }
    char rel[kMaxPath];
    int n = relPath.len < kMaxPath - 1 ? relPath.len : kMaxPath - 1;
    memcpy(rel, relPath.s, (size_t)n);
    rel[n] = 0;
    ToNativeSep(rel);

    for (int i = 0; i < gRootN; i++) {
        char full[kMaxPath];
        JoinPath(full, kMaxPath, gRoots[i], rel);
        if (ReadFileAll(full, out)) {
            return true;
        }
    }
    return false;
}

TempStr AssetsLoadTextTemp(Str relPath) {
    Vec<uint8_t> buf;
    if (!AssetsLoad(relPath, &buf) || buf.len <= 0) {
        return {};
    }
    Str s = AllocStrTemp(buf.len);
    if (!s.s) {
        return {};
    }
    memcpy(s.s, buf.els, (size_t)buf.len);
    s.s[buf.len] = 0;
    return s;
}

bool AssetsExists(Str relPath) {
    Vec<uint8_t> buf;
    return AssetsLoad(relPath, &buf);
}
}

#line 1 "src/gpui/Entity.cpp"

namespace gpui {

EntityId EntityNewRaw(App* app, void* ptr, RenderFn render, DropFn drop) {
    EntityId id;
    if (!app || !ptr) {
        return id;
    }
    int32_t ix;
    if (app->freeSlots.len > 0) {
        ix = app->freeSlots[app->freeSlots.len - 1];
        app->freeSlots.len--;
    } else {
        EntitySlot fresh = {};
        app->entities.Append(fresh);
        ix = (int32_t)(app->entities.len - 1);
    }
    EntitySlot& s = app->entities[ix];
    s.ptr = ptr;
    s.render = render;
    s.drop = drop;
    if (s.gen == 0) {
        s.gen = 1;
    }
    id.index = ix;
    id.gen = s.gen;
    return id;
}

void* EntityGet(App* app, EntityId id) {
    if (!app || !id.IsValid() || id.index >= app->entities.len) {
        return nullptr;
    }
    EntitySlot& s = app->entities[id.index];
    if (s.gen != id.gen || !s.ptr) {
        return nullptr;
    }
    return s.ptr;
}

void EntityDrop(App* app, EntityId id) {
    if (!app || !id.IsValid() || id.index >= app->entities.len) {
        return;
    }
    EntitySlot& s = app->entities[id.index];
    if (s.gen != id.gen || !s.ptr) {
        return;
    }
    if (s.drop) {
        s.drop(s.ptr);
    }
    s.ptr = nullptr;
    s.render = nullptr;
    s.drop = nullptr;

    s.gen++;
    if (s.gen == 0) {
        s.gen = 1;
    }
    app->freeSlots.Append(id.index);
}

void EntityDropAll(App* app) {
    if (!app) {
        return;
    }
    for (int i = 0; i < app->entities.len; i++) {
        EntitySlot& s = app->entities[i];
        if (s.ptr && s.drop) {
            s.drop(s.ptr);
        }
        s.ptr = nullptr;
        s.render = nullptr;
        s.drop = nullptr;
    }
    app->entities.Reset();
    app->freeSlots.Reset();
}

El* EntityRender(App* app, Window* win, Arena* a, EntityId id) {
    if (!app || !id.IsValid() || id.index >= app->entities.len) {
        return nullptr;
    }
    EntitySlot& s = app->entities[id.index];
    if (s.gen != id.gen || !s.ptr || !s.render) {
        return nullptr;
    }
    Ctx cx;
    cx.app = app;
    cx.win = win;
    cx.a = a;
    cx.self = id;
    return s.render(s.ptr, &cx);
}

const Theme& Ctx::theme() const {
    return themeMode() == ThemeMode::Dark ? ThemeDark() : ThemeLight();
}

ThemeMode Ctx::themeMode() const {
    return app ? app->themeMode : ThemeGet();
}

void NotifyApp(App* app) {
    if (!app) {
        return;
    }
    for (int i = 0; i < app->windows.len; i++) {
        AppInvalidate(app->windows[i]);
    }
}

void Notify(Ctx* cx) {
    if (!cx) {
        return;
    }
    if (cx->win) {
        AppInvalidate(cx->win);
        return;
    }
    NotifyApp(cx->app);
}

void ListenerCall(App* app, Window* win, const Listener& l, const void* ev) {
    if (!l.fn) {
        return;
    }
    void* self = EntityGet(app, l.view);
    if (!self) {

        return;
    }
    Ctx cx;
    cx.app = app;
    cx.win = win;
    cx.a = win ? win->frameArena : nullptr;
    cx.self = l.view;
    if (l.hasArg) {
        ((ListenerArgFn)l.fn)(self, &cx, ev, l.arg);
    } else {
        ((ListenerFn)l.fn)(self, &cx, ev);
    }
}

void WindowOnKey(Window* win, Listener l) {
    if (win) {
        win->onKey = l;
    }
}

void WindowOnScrollWheel(Window* win, Listener l) {
    if (win) {
        win->onScrollWheel = l;
    }
}

WinSize WindowSize(Window* win) {
    WinSize ws = {};
    if (!win) {
        return ws;
    }
    ws.dipW = win->paint.viewW;
    ws.dipH = win->paint.viewH;
    ws.pxW = DipToPx(&win->paint, ws.dipW);
    ws.pxH = DipToPx(&win->paint, ws.dipH);
    return ws;
}

void WindowOnUnhandledClick(Window* win, Listener l) {
    if (win) {
        win->onClick = l;
    }
}

void WindowOnMouseDown(Window* win, Listener l) {
    if (win) {
        win->onMouseDown = l;
    }
}

void WindowOnMouseUp(Window* win, Listener l) {
    if (win) {
        win->onMouseUp = l;
    }
}

void WindowOnMouseMove(Window* win, Listener l) {
    if (win) {
        win->onMouseMove = l;
    }
}

void WindowOnMouseExit(Window* win, Listener l) {
    if (win) {
        win->onMouseExit = l;
    }
}

static int WindowArmTimer(Window* win, int ms, Listener l, bool repeat) {
    if (!win || ms <= 0 || !l.IsValid()) {
        return 0;
    }
    TimerSub t;
    t.id = win->nextTimerId++;
    t.ms = ms;
    t.dueAt = TimeNow() + (double)ms / 1000.0;
    t.repeat = repeat;
    t.l = l;
    win->timers.Append(t);
    PlatSetTimer(win, WindowTimerMs(win));
    return t.id;
}

int WindowSetInterval(Window* win, int ms, Listener l) {
    return WindowArmTimer(win, ms, l, true);
}

int WindowSetTimeout(Window* win, int ms, Listener l) {
    return WindowArmTimer(win, ms, l, false);
}

void WindowCancelTimer(Window* win, int id) {
    if (!win || id <= 0) {
        return;
    }
    for (int i = 0; i < win->timers.len; i++) {
        if (win->timers[i].id != id) {
            continue;
        }
        for (int j = i + 1; j < win->timers.len; j++) {
            win->timers[j - 1] = win->timers[j];
        }
        win->timers.len--;
        break;
    }
    PlatSetTimer(win, WindowTimerMs(win));
}

void* WindowKeyedState(Window* win, uint32_t key, int size, DropFn drop) {
    if (!win || size <= 0) {
        return nullptr;
    }
    for (int i = 0; i < win->keyed.len; i++) {
        if (win->keyed[i].key == key) {
            return win->keyed[i].ptr;
        }
    }
    KeyedSlot s = {};
    s.key = key;
    s.ptr = AllocZero(1, size);
    s.drop = drop;
    win->keyed.Append(s);
    return s.ptr;
}

void WindowKeyedFree(Window* win) {
    if (!win) {
        return;
    }
    for (int i = 0; i < win->keyed.len; i++) {

        Free(nullptr, win->keyed[i].ptr);
    }
    win->keyed.Reset();
}

}

#line 1 "src/gpui/Fps.cpp"

namespace gpui {

const FpsStyle& FpsStyleDark() {

    static FpsStyle style = {
        RgbaHsla(0.f, 0.f, 0.04f, 0.92f),
        RgbaHsla(0.f, 0.f, 0.98f, 1.f),
        RgbaHsla(0.f, 0.f, 0.62f, 1.f),
        RgbaHsla(0.41f, 0.95f, 0.56f, 1.f),
        RgbaHsla(0.11f, 0.95f, 0.6f, 1.f),
        RgbaHsla(0.99f, 0.9f, 0.62f, 1.f),
    };
    return style;
}

Rgba FpsLevelColor(const FpsStyle& style, float frameSecs, float budgetSecs) {
    if (frameSecs <= budgetSecs) {
        return style.good;
    }
    if (frameSecs <= budgetSecs * 2.f) {
        return style.warn;
    }
    return style.bad;
}

static const double kFpsWindow = 1.0;

void FrameSamplerSetCapacity(FrameSampler* s, int capacity) {
    if (capacity < 1) {
        capacity = 1;
    }
    if (capacity > kFpsCapacity) {
        capacity = kFpsCapacity;
    }
    s->capacity = capacity;
    if (s->n > capacity) {
        int drop = s->n - capacity;
        memmove(s->draws, s->draws + drop, sizeof(float) * (size_t)capacity);
        s->n = capacity;
    }
}

static void FrameSamplerPush(FrameSampler* s, float drawSecs, double now) {
    if (s->n == s->capacity) {
        memmove(s->draws, s->draws + 1, sizeof(float) * (size_t)(s->n - 1));
        s->n--;
    }
    s->draws[s->n++] = drawSecs;

    if (s->nArrivals == kFpsArrivals) {
        memmove(s->arrivals, s->arrivals + 1,
                sizeof(double) * (size_t)(s->nArrivals - 1));
        s->nArrivals--;
    }
    s->arrivals[s->nArrivals++] = now;
}

void FrameSamplerIngest(FrameSampler* s, const float* drawSecs, int n,
                        double now) {
    if (!s) {
        return;
    }
    if (s->capacity < 1 || s->capacity > kFpsCapacity) {
        s->capacity = kFpsCapacity;
    }
    for (int i = 0; i < n; i++) {
        FrameSamplerPush(s, drawSecs[i], now);
    }

    int drop = 0;
    while (drop < s->nArrivals && now - s->arrivals[drop] > kFpsWindow) {
        drop++;
    }
    if (drop > 0) {
        memmove(s->arrivals, s->arrivals + drop,
                sizeof(double) * (size_t)(s->nArrivals - drop));
        s->nArrivals -= drop;
    }
}

void FrameSamplerTick(FrameSampler* s, Window* win) {
    if (!s || !win) {
        return;
    }
    FrameTiming timings[kFrameTraceCap];
    int n = WindowCollectFrames(win, &s->cursor, timings, kFrameTraceCap);
    float draws[kFrameTraceCap];
    for (int i = 0; i < n; i++) {
        draws[i] = timings[i].drawSecs;
    }
    FrameSamplerIngest(s, draws, n, TimeNow());
}

float FrameSamplerFps(const FrameSampler* s) {
    if (s->nArrivals < 2) {
        return 0;
    }
    double span = s->arrivals[s->nArrivals - 1] - s->arrivals[0];
    if (span <= 0) {
        return 0;
    }
    return (float)((double)(s->nArrivals - 1) / span);
}

float FrameSamplerMeanDraw(const FrameSampler* s) {
    if (s->n <= 0) {
        return 0;
    }
    double total = 0;
    for (int i = 0; i < s->n; i++) {
        total += s->draws[i];
    }
    return (float)(total / (double)s->n);
}

float FrameSamplerPeakDraw(const FrameSampler* s) {
    float peak = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->draws[i] > peak) {
            peak = s->draws[i];
        }
    }
    return peak;
}

float FrameSamplerOverBudget(const FrameSampler* s, float budgetSecs) {
    if (s->n <= 0) {
        return 0;
    }
    int over = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->draws[i] > budgetSecs) {
            over++;
        }
    }
    return (float)over / (float)s->n;
}

bool ResourceProbeSample(ResourceProbe* probe, ResourceSample* out) {
    if (!probe || !out) {
        return false;
    }
    if (!probe->primed) {
        probe->cores = (float)PlatCoreCount();
    }
    uint64_t cpu100ns = 0;
    uint64_t memBytes = 0;
    if (!PlatSelfUsage(&cpu100ns, &memBytes)) {
        return false;
    }
    double now = TimeNow();

    bool primed = probe->primed;
    double elapsed = now - probe->prevAt;
    uint64_t delta = cpu100ns - probe->prevCpu100ns;
    probe->prevCpu100ns = cpu100ns;
    probe->prevAt = now;
    probe->primed = true;
    if (!primed || elapsed <= 0) {
        return false;
    }

    float cpu = (float)((double)delta / (elapsed * 1e7 * probe->cores) * 100.);
    out->cpuPercent = cpu > 100.f ? 100.f : cpu;
    out->memoryBytes = memBytes;
    return true;
}

static const float kAxisDecay = 0.04f;

static const float kHudWidth = 172.f;
static const float kCompactFigureWidth = 25.f;

static const float kTextSize = 10.f;

static const float kTraceOpacity = 0.35f;

static const float kHeadlineHeight = 35.f;

static const float kFigureSize = 28.f;
static const float kFigureWidth = 70.f;

static const float kUnitWidth = 22.f;

static const double kReadoutInterval = 0.5;

static const float kFpsTolerance = 0.95f;

static const float kOverlayMargin = 12.f;

static Rgba FpsColor(float fps, float budgetSecs, const FpsStyle& style) {
    if (fps <= 0) {
        return style.muted;
    }
    float target = 1.f / budgetSecs;
    if (fps >= target * kFpsTolerance) {
        return style.good;
    }
    if (fps >= target * 0.5f) {
        return style.warn;
    }
    return style.bad;
}

static TempStr FpsFormatBytes(uint64_t bytes) {
    const double kMib = 1024. * 1024.;
    const double kGib = kMib * 1024.;
    double v = (double)bytes;
    if (v >= kGib) {
        return fmt("%.2f GB", v / kGib);
    }
    return fmt("%.0f MB", v / kMib);
}

static void UpdateReadout(FpsMonitor* self) {
    double now = TimeNow();
    if (self->readoutAt >= 0 && now - self->readoutAt < kReadoutInterval) {
        return;
    }
    self->readout.fps = FrameSamplerFps(&self->sampler);

    self->readout.frameMillis = FrameSamplerMeanDraw(&self->sampler) * 1000.f;
    self->readout.droppedPercent =
        FrameSamplerOverBudget(&self->sampler, self->frameBudget) * 100.f;
    self->readoutAt = now;
}

static void UpdateAxis(FpsMonitor* self) {
    float floorSecs = self->frameBudget * 2.f;
    float target = FrameSamplerPeakDraw(&self->sampler);
    if (target < floorSecs) {
        target = floorSecs;
    }
    self->axisMax = target > self->axisMax
                        ? target
                        : self->axisMax + (target - self->axisMax) * kAxisDecay;
}

static void UpdateResources(FpsMonitor* self) {
    if (!self->showResources) {
        return;
    }
    double now = TimeNow();
    if (self->resourcesAt >= 0 &&
        now - self->resourcesAt < (double)self->resourceInterval) {
        return;
    }
    self->resourcesAt = now;
    ResourceSample sample;
    if (ResourceProbeSample(&self->probe, &sample)) {
        self->resources = sample;
        self->hasResources = true;
    }
}

static bool SameRgba(Rgba a, Rgba b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static void PaintFpsTrace(PaintCtx* ctx, El* e, void* user) {
    auto* self = (FpsMonitor*)user;
    const FrameSampler* s = &self->sampler;
    if (!ctx->rt || s->n < 2 || e->w <= 0 || e->h <= 0) {
        return;
    }
    const FpsStyle& style = FpsStyleDark();
    float axisMax = self->axisMax > 1e-6f ? self->axisMax : 1e-6f;
    float slot = e->w / (float)s->capacity;

    int leading = s->capacity - s->n;
    if (leading < 0) {
        leading = 0;
    }

    float px[kFpsCapacity];
    float py[kFpsCapacity];
    Rgba colors[kFpsCapacity];
    for (int i = 0; i < s->n; i++) {
        float secs = s->draws[i];
        float ratio = secs / axisMax;
        if (ratio < 0) {
            ratio = 0;
        }
        if (ratio > 1) {
            ratio = 1;
        }
        px[i] = e->x + slot * (float)(leading + i) + slot * 0.5f;
        py[i] = e->y + e->h * (1.f - ratio);
        colors[i] = RgbaOpacity(FpsLevelColor(style, secs, self->frameBudget),
                                kTraceOpacity);
    }

    int start = 0;
    while (start + 1 < s->n) {

        Rgba color = colors[start + 1];
        int end = start + 1;
        while (end < s->n && SameRgba(colors[end], color)) {
            CanvasLine(ctx, px[end - 1], py[end - 1], px[end], py[end], 1.f,
                       color);
            end++;
        }

        start = end - 1;
    }
}

static El* FpsPair(Ctx* cx, Str label, Str value, const FpsStyle& style) {
    return Div(cx->a)
        ->FlexRow()
        ->Gap(4)
        ->Child(TextEl(cx->a, label)->Fg(style.muted))
        ->Child(TextEl(cx->a, value)->Fg(style.foreground));
}

static El* FpsReading(Ctx* cx, Str label, Str value, Rgba valueColor,
                      const FpsStyle& style) {
    return Div(cx->a)
        ->FlexRow()
        ->W(kFill)
        ->JustifyBetween()
        ->Gap(8)
        ->PadY(1)
        ->Child(TextEl(cx->a, label)->Fg(style.muted))
        ->Child(TextEl(cx->a, value)->Fg(valueColor));
}

static El* FpsHeadline(Ctx* cx, FpsMonitor* self, float fps, Rgba color,
                       const FpsStyle& style) {
    El* trace = Div(cx->a)->Absolute()->Top(0)->Left(0)->SizeFull();
    trace->customPaint = PaintFpsTrace;
    trace->customUser = self;

    El* figure = Div(cx->a)
                     ->W(kFigureWidth)
                     ->H(kFigureSize)
                     ->FlexRow()
                     ->ItemsCenter()
                     ->JustifyCenter()
                     ->Child(TextEl(cx->a, fmt("%.0f", fps))
                                 ->Font(kFigureSize)
                                 ->LineHeight(1.f)
                                 ->Fg(color));

    return Div(cx->a)
        ->ClipY()
        ->W(kFill)
        ->H(kHeadlineHeight)
        ->Child(trace)
        ->Child(Div(cx->a)
                    ->FlexRow()
                    ->SizeFull()
                    ->ItemsEnd()
                    ->JustifyCenter()
                    ->Gap(4)

                    ->Child(Div(cx->a)->W(kUnitWidth)->H(kTextSize))
                    ->Child(figure)
                    ->Child(Div(cx->a)
                                ->W(kUnitWidth)
                                ->Child(TextEl(cx->a, StrL("FPS"))
                                            ->Fg(style.muted))));
}

void FpsMonitor::OnToggleCompact(FpsMonitor* self, Ctx* cx, const ClickEvent*) {
    self->compact = !self->compact;
    Notify(cx);
}

El* FpsMonitor::Render(FpsMonitor* self, Ctx* cx) {
    FrameSamplerTick(&self->sampler, cx->win);
    UpdateReadout(self);
    UpdateAxis(self);
    UpdateResources(self);

    if (self->continuous && cx->win && !cx->win->anim) {
        AppRequestAnim(cx->win, true);
    }

    const FpsStyle& style = FpsStyleDark();
    FpsReadout r = self->readout;
    Rgba fpsColor = FpsColor(r.fps, self->frameBudget, style);

    El* hud = Div(cx->a)
                  ->Click(HashClickId(StrL("gpui-fps-hud")))
                  ->FlexRow()
                  ->Bg(style.background)
                  ->Mono()
                  ->Font(kTextSize)
                  ->OnClick(Listen(cx, &FpsMonitor::OnToggleCompact));

    if (self->compact) {

        return hud->ItemsCenter()
            ->Gap(4)
            ->PadX(6)
            ->PadY(2)
            ->Radius(3)
            ->Child(
                Div(cx->a)
                    ->W(kCompactFigureWidth)
                    ->FlexRow()
                    ->JustifyEnd()
                    ->Child(TextEl(cx->a, fmt("%.0f", r.fps))->Fg(fpsColor)))
            ->Child(TextEl(cx->a, StrL("FPS"))->Fg(style.muted));
    }

    hud->FlexCol()
        ->W(kHudWidth)
        ->PadX(8)
        ->PadY(6)
        ->Radius(4)
        ->Child(FpsHeadline(cx, self, r.fps, fpsColor, style))
        ->Child(FpsReading(cx, StrL("FRAME"), fmt("%.1f ms", r.frameMillis),
                           style.foreground, style))
        ->Child(FpsReading(
            cx, StrL("DROP"), fmt("%.1f%%", r.droppedPercent),
            FpsLevelColor(style, r.droppedPercent > 0 ? 1.f : 0.f, 0.5f),
            style));
    if (self->showResources && self->hasResources) {

        hud->Child(
            Div(cx->a)
                ->FlexRow()
                ->W(kFill)
                ->JustifyBetween()
                ->Gap(8)
                ->PadY(1)
                ->Child(FpsPair(cx, StrL("CPU"),
                                fmt("%.1f%%", self->resources.cpuPercent),
                                style))
                ->Child(FpsPair(cx, StrL("MEM"),
                                FpsFormatBytes(self->resources.memoryBytes),
                                style)));
    }
    return hud;
}

El* FpsOverlayEl(Ctx* cx, Entity<FpsMonitor> monitor, FpsAnchor anchor) {
    El* hud = EntityRender(cx->app, cx->win, cx->a, monitor.id);
    if (!hud) {
        return Div(cx->a);
    }

    El* box = Div(cx->a)->Absolute()->FlexRow();
    float m = kOverlayMargin;
    switch (anchor) {
        case FpsAnchor::TopLeft:
            box->Top(m)->Left(m);
            break;
        case FpsAnchor::TopRight:
            box->Top(m)->Right(m);
            break;
        case FpsAnchor::BottomLeft:
            box->Bottom(m)->Left(m);
            break;
        case FpsAnchor::BottomRight:
            box->Bottom(m)->Right(m);
            break;
        case FpsAnchor::TopCenter:
            box->Top(m)->Left(0)->W(kFill)->JustifyCenter();
            break;
        case FpsAnchor::BottomCenter:
            box->Bottom(m)->Left(0)->W(kFill)->JustifyCenter();
            break;
        case FpsAnchor::LeftCenter:
            box->Left(m)->Top(0)->H(kFill)->ItemsCenter();
            break;
        case FpsAnchor::RightCenter:
            box->Right(m)->Top(0)->H(kFill)->ItemsCenter();
            break;
    }
    return box->Child(hud);
}

El* FpsMonitorEl(Ctx* cx) {

    auto* slot = KeyedState<Entity<FpsMonitor>>(
        cx, (uint32_t)HashClickId(StrL("gpui-fps-monitor")));
    if (!slot) {
        return Div(cx->a);
    }
    if (!slot->IsValid()) {
        *slot = EntityNew<FpsMonitor>(cx);
    }
    return FpsOverlayEl(cx, *slot, FpsAnchor::TopRight);
}

}

#line 1 "src/gpui/Gpui.cpp"

namespace gpui {

Rgba RgbaOpacity(Rgba c, float a01) {
    if (a01 < 0) {
        a01 = 0;
    }
    if (a01 > 1) {
        a01 = 1;
    }
    c.a = (uint8_t)(c.a * a01 + 0.5f);
    return c;
}

Rgba RgbaMix(Rgba a, Rgba b, float t) {
    if (t < 0) {
        t = 0;
    }
    if (t > 1) {
        t = 1;
    }
    Rgba o;
    o.r = (uint8_t)(a.r * t + b.r * (1 - t) + 0.5f);
    o.g = (uint8_t)(a.g * t + b.g * (1 - t) + 0.5f);
    o.b = (uint8_t)(a.b * t + b.b * (1 - t) + 0.5f);
    o.a = (uint8_t)(a.a * t + b.a * (1 - t) + 0.5f);
    return o;
}

static float Clamp01(float v) {
    if (v < 0) {
        return 0;
    }
    return v > 1 ? 1 : v;
}

Rgba RgbaHsla(float h, float s, float l, float a01) {
    h = h - floorf(h);
    s = Clamp01(s);
    l = Clamp01(l);
    float c = (1.f - fabsf(2.f * l - 1.f)) * s;
    float hp = h * 6.f;
    float x = c * (1.f - fabsf(fmodf(hp, 2.f) - 1.f));
    float r = 0, g = 0, b = 0;
    if (hp < 1.f) {
        r = c;
        g = x;
    } else if (hp < 2.f) {
        r = x;
        g = c;
    } else if (hp < 3.f) {
        g = c;
        b = x;
    } else if (hp < 4.f) {
        g = x;
        b = c;
    } else if (hp < 5.f) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }
    float m = l - c * 0.5f;
    return Rgba{(uint8_t)(Clamp01(r + m) * 255.f + 0.5f),
                (uint8_t)(Clamp01(g + m) * 255.f + 0.5f),
                (uint8_t)(Clamp01(b + m) * 255.f + 0.5f),
                (uint8_t)(Clamp01(a01) * 255.f + 0.5f)};
}

const Theme& ThemeDark() {
    static Theme t;
    static bool init = false;
    if (!init) {
        t.background = Rgb(0x0a, 0x0a, 0x0a);
        t.foreground = Rgb(0xfa, 0xfa, 0xfa);
        t.border = Rgb(0x26, 0x26, 0x26);
        t.mutedFg = Rgb(0xa3, 0xa3, 0xa3);
        t.inputBorder = Rgb(0x2f, 0x2f, 0x2f);
        t.inputBg = Rgba8(0x2f, 0x2f, 0x2f, 0xb3);
        t.ring = Rgb(0x73, 0x73, 0x73);
        t.caret = Rgb(0xfa, 0xfa, 0xfa);
        t.titleBar = Rgb(0x17, 0x17, 0x17);
        t.titleBarBorder = Rgb(0x26, 0x26, 0x26);
        t.tabBar = Rgb(0x17, 0x17, 0x17);
        t.tabActiveBg = Rgb(0x0a, 0x0a, 0x0a);
        t.tabActiveFg = Rgb(0xfa, 0xfa, 0xfa);
        t.tabFg = Rgb(0xd4, 0xd4, 0xd4);
        t.tableBg = Rgb(0x0a, 0x0a, 0x0a);
        t.tableHead = Rgba8(0x17, 0x17, 0x17, 0x66);
        t.tableHeadFg = Rgb(0x52, 0x52, 0x52);
        t.tableRowBorder = Rgba8(0x26, 0x26, 0x26, 0xb3);
        t.tableEven = Rgba8(0x17, 0x17, 0x17, 0x66);
        t.progress = Rgb(0xf5, 0xf5, 0xf5);
        t.red = Rgb(0xf8, 0x71, 0x71);
        t.green = Rgb(0x4a, 0xde, 0x80);
        t.blue = Rgb(0x60, 0xa5, 0xfa);
        t.yellow = Rgb(0xfa, 0xcc, 0x15);
        t.cyan = Rgb(0x22, 0xd3, 0xee);
        t.magenta = Rgb(0xc0, 0x84, 0xfc);
        t.danger = Rgb(0xf8, 0x71, 0x71);
        t.dangerFg = Rgb(0xdc, 0x26, 0x26);
        t.secondaryHover = Rgb(0x29, 0x29, 0x29);
        t.secondaryActive = Rgb(0x21, 0x21, 0x21);
        t.secondaryFg = Rgb(0xfa, 0xfa, 0xfa);
        t.secondary = Rgb(0x26, 0x26, 0x26);
        t.muted = Rgb(0x26, 0x26, 0x26);
        t.accent = Rgb(0x26, 0x26, 0x26);
        t.primary = Rgb(0xfa, 0xfa, 0xfa);
        t.primaryFg = Rgb(0x17, 0x17, 0x17);
        t.sidebar = Rgb(0x0a, 0x0a, 0x0a);
        t.sidebarFg = Rgb(0xf5, 0xf5, 0xf5);
        t.sidebarPrimary = Rgb(0xf5, 0xf5, 0xf5);
        t.sidebarPrimaryFg = Rgb(0x0a, 0x0a, 0x0a);
        t.scrollbarThumb = Rgba8(0x52, 0x52, 0x52, 0xe6);
        t.info = Rgb(0x22, 0xd3, 0xee);
        t.infoFg = Rgb(0xfa, 0xfa, 0xfa);
        t.success = Rgb(0x4a, 0xde, 0x80);
        t.successFg = Rgb(0x0a, 0x0a, 0x0a);
        t.warning = Rgb(0xfa, 0xcc, 0x15);
        t.warningFg = Rgb(0x0a, 0x0a, 0x0a);
        t.skeleton = Rgb(0x26, 0x26, 0x26);
        t.overlay = Rgba8(0, 0, 0, 0x33);
        t.groupBox = Rgb(0x0a, 0x0a, 0x0a);
        t.groupBoxFg = Rgb(0xfa, 0xfa, 0xfa);
        t.descListLabel = Rgb(0x17, 0x17, 0x17);
        t.descListLabelFg = Rgb(0xf5, 0xf5, 0xf5);
        t.radius = 6;
        t.radiusLg = 8;
        init = true;
    }
    return t;
}

const Theme& ThemeLight() {
    static Theme t;
    static bool init = false;
    if (!init) {
        t.background = Rgb(0xff, 0xff, 0xff);
        t.foreground = Rgb(0x0a, 0x0a, 0x0a);
        t.border = Rgb(0xe5, 0xe5, 0xe5);
        t.mutedFg = Rgb(0x73, 0x73, 0x73);
        t.inputBorder = Rgb(0xe5, 0xe5, 0xe5);
        t.inputBg = Rgb(0xff, 0xff, 0xff);
        t.ring = Rgb(0xa3, 0xa3, 0xa3);
        t.caret = Rgb(0x0a, 0x0a, 0x0a);
        t.titleBar = Rgb(0xf8, 0xf8, 0xf8);
        t.titleBarBorder = Rgb(0xe5, 0xe5, 0xe5);
        t.tabBar = Rgb(0xf5, 0xf5, 0xf5);
        t.tabActiveBg = Rgb(0xff, 0xff, 0xff);
        t.tabActiveFg = Rgb(0x17, 0x17, 0x17);
        t.tabFg = Rgb(0x40, 0x40, 0x40);
        t.tableBg = Rgb(0xff, 0xff, 0xff);
        t.tableHead = Rgb(0xfa, 0xfa, 0xfa);
        t.tableHeadFg = Rgb(0x73, 0x73, 0x73);
        t.tableRowBorder = Rgba8(0xe5, 0xe5, 0xe5, 0xb3);
        t.tableEven = Rgb(0xfa, 0xfa, 0xfa);
        t.progress = Rgb(0x17, 0x17, 0x17);
        t.red = Rgb(0xdc, 0x26, 0x26);
        t.green = Rgb(0x16, 0xa3, 0x4a);
        t.blue = Rgb(0x25, 0x63, 0xeb);
        t.yellow = Rgb(0xca, 0x8a, 0x04);
        t.cyan = Rgb(0x08, 0x91, 0xb2);
        t.magenta = Rgb(0x93, 0x33, 0xea);
        t.danger = Rgb(0xef, 0x44, 0x44);
        t.dangerFg = Rgb(0xfa, 0xfa, 0xfa);
        t.secondaryHover = Rgb(0xe5, 0xe5, 0xe5);
        t.secondaryActive = Rgb(0xd4, 0xd4, 0xd4);
        t.secondaryFg = Rgb(0x17, 0x17, 0x17);
        t.secondary = Rgb(0xe5, 0xe5, 0xe5);
        t.muted = Rgb(0xf5, 0xf5, 0xf5);
        t.accent = Rgb(0xf5, 0xf5, 0xf5);
        t.primary = Rgb(0x17, 0x17, 0x17);
        t.primaryFg = Rgb(0xfa, 0xfa, 0xfa);
        t.sidebar = Rgb(0xfa, 0xfa, 0xfa);
        t.sidebarFg = Rgb(0x17, 0x17, 0x17);
        t.sidebarPrimary = Rgb(0x17, 0x17, 0x17);
        t.sidebarPrimaryFg = Rgb(0xfa, 0xfa, 0xfa);
        t.scrollbarThumb = Rgba8(0xa3, 0xa3, 0xa3, 0xe6);
        t.info = Rgb(0x06, 0xb6, 0xd4);
        t.infoFg = Rgb(0xfa, 0xfa, 0xfa);
        t.success = Rgb(0x22, 0xc5, 0x5e);
        t.successFg = Rgb(0xfa, 0xfa, 0xfa);
        t.warning = Rgb(0xea, 0xb3, 0x08);
        t.warningFg = Rgb(0x17, 0x17, 0x17);
        t.skeleton = Rgb(0xf5, 0xf5, 0xf5);
        t.overlay = Rgba8(0, 0, 0, 0x0d);
        t.groupBox = Rgb(0xf5, 0xf5, 0xf5);
        t.groupBoxFg = Rgb(0x17, 0x17, 0x17);
        t.descListLabel = Rgb(0xfa, 0xfa, 0xfa);
        t.descListLabelFg = Rgb(0x17, 0x17, 0x17);
        t.radius = 6;
        t.radiusLg = 8;
        init = true;
    }
    return t;
}

static ThemeMode gThemeMode = ThemeMode::Light;

void ThemeSet(App* app, ThemeMode mode) {
    if (app) {
        app->themeMode = mode;
    }

    gThemeMode = mode;
}

ThemeMode ThemeGet() {
    return gThemeMode;
}

const Theme& ThemeNow() {
    return gThemeMode == ThemeMode::Dark ? ThemeDark() : ThemeLight();
}

static El* NewEl(Arena* a, ElKind k) {
    El* e = ArenaNew<El>(a);
    e->kind = k;
    return e;
}

El* Div(Arena* a) {
    return NewEl(a, ElKind::Div);
}

El* TextEl(Arena* a, Str s) {
    El* e = NewEl(a, ElKind::Text);
    e->text = s;
    return e;
}

El* IconEl(Arena* a, IconName name) {
    return IconEl(a, name, 16.f);
}

El* IconEl(Arena* a, IconName name, float size) {
    El* e = NewEl(a, ElKind::Icon);
    e->icon = name;
    e->iconPath = IconNamePath(name);
    e->style.width = size;
    e->style.height = size;
    e->style.flexShrink = 0;
    return e;
}

El* ButtonEl(Arena* a, int clickId, Str label, BtnKind kind) {
    return ButtonSmall(a, clickId, label, kind, false);
}

El* ButtonSmall(Arena* a, int clickId, Str label, BtnKind kind, bool selected) {
    const Theme& th = ThemeNow();
    El* b = Div(a)
                ->ItemsCenter()
                ->JustifyCenter()
                ->Radius(th.radius)
                ->Click(clickId)
                ->FocusId(clickId);
    if (kind == BtnKind::Primary) {
        b->PadX(16)
            ->PadY(8)
            ->Bg(th.primary)
            ->HoverBg(RgbaMix(th.primary, th.foreground, 0.85f));
        b->Child(TextEl(a, label)->Font(14)->Fg(th.primaryFg));
    } else if (kind == BtnKind::Outline) {
        b->PadX(16)->PadY(8)->Border(1, th.border)->HoverBg(th.muted);
        b->Child(TextEl(a, label)->Font(14)->Fg(th.foreground));
    } else {
        b->PadX(12)
            ->PadY(6)
            ->Bg(selected ? th.secondaryActive : th.secondary)
            ->HoverBg(th.secondaryHover);
        b->Child(
            TextEl(a, label)->Font(selected ? 13.f : 14.f)->Fg(th.secondaryFg));
    }
    return b;
}

El* ProgressEl(Arena* a, float value01to100, float barW, float barH) {
    El* e = NewEl(a, ElKind::Progress);
    e->progress = value01to100;
    if (e->progress < 0) {
        e->progress = 0;
    }
    if (e->progress > 100) {
        e->progress = 100;
    }
    e->style.width = barW;
    e->style.height = barH;
    e->style.flexShrink = 0;
    e->style.radius = barH * 0.5f;
    return e;
}

El* ChartEl(Arena* a, const float* ys, int n, Rgba stroke, Rgba fillTop,
            Rgba fillBot, int tickMargin) {
    El* e = NewEl(a, ElKind::Chart);
    e->chart.ys = ys;
    e->chart.n = n;
    e->chart.stroke = stroke;
    e->chart.fillTop = fillTop;
    e->chart.fillBot = fillBot;
    e->chart.tickMargin = tickMargin > 0 ? tickMargin : 15;
    e->style.flexGrow = 1;
    e->style.height = kFill;
    e->style.minH = 80;
    return e;
}

El* El::FlexRow() {
    style.dir = FlexDir::Row;
    return this;
}
El* El::FlexCol() {
    style.dir = FlexDir::Col;
    return this;
}
El* El::FlexWrap() {
    style.flexWrap = true;
    return this;
}
El* El::Grow(float g) {
    style.flexGrow = g;
    return this;
}
El* El::Shrink0() {
    style.flexShrink = 0;
    return this;
}
El* El::W(float v) {
    style.width = v;
    return this;
}
El* El::WFrac(float f) {
    style.widthFrac = f;
    return this;
}
El* El::H(float v) {
    style.height = v;
    return this;
}
El* El::SizeFull() {
    style.width = kFill;
    style.height = kFill;
    style.flexGrow = 1;
    return this;
}
El* El::MinH(float v) {
    style.minH = v;
    return this;
}
El* El::MinW(float v) {
    style.minW = v;
    return this;
}
El* El::MaxW(float v) {
    style.maxW = v;
    return this;
}
El* El::MaxH(float v) {
    style.maxH = v;
    return this;
}
El* El::Gap(float v) {
    style.gap = v;
    return this;
}
El* El::Pad(float v) {
    style.pad = {v, v, v, v};
    return this;
}
El* El::PadX(float v) {
    style.pad.left = style.pad.right = v;
    return this;
}
El* El::PadY(float v) {
    style.pad.top = style.pad.bottom = v;
    return this;
}
El* El::PadL(float v) {
    style.pad.left = v;
    return this;
}
El* El::PadR(float v) {
    style.pad.right = v;
    return this;
}
El* El::PadT(float v) {
    style.pad.top = v;
    return this;
}
El* El::PadB(float v) {
    style.pad.bottom = v;
    return this;
}
El* El::ItemsCenter() {
    style.align = Align::Center;
    return this;
}
El* El::ItemsStart() {
    style.align = Align::Start;
    return this;
}
El* El::ItemsEnd() {
    style.align = Align::End;
    return this;
}
El* El::JustifyBetween() {
    style.justify = Justify::SpaceBetween;
    return this;
}
El* El::JustifyCenter() {
    style.justify = Justify::Center;
    return this;
}
El* El::JustifyEnd() {
    style.justify = Justify::End;
    return this;
}
El* El::JustifyStart() {
    style.justify = Justify::Start;
    return this;
}
El* El::Bg(Rgba c) {
    style.bg = c;
    style.hasBg = true;
    return this;
}
El* El::Border(float width, Rgba c) {
    style.border = width;
    style.borderColor = c;
    return this;
}
El* El::BorderT(float width, Rgba c) {
    style.borderT = width;
    style.borderColor = c;
    return this;
}
El* El::BorderB(float width, Rgba c) {
    style.borderB = width;
    style.borderColor = c;
    return this;
}
El* El::BorderL(float width, Rgba c) {
    style.borderL = width;
    style.borderColor = c;
    return this;
}
El* El::BorderR(float width, Rgba c) {
    style.borderR = width;
    style.borderColor = c;
    return this;
}
El* El::DashArray(float on, float off) {
    style.dashOn = on;
    style.dashOff = off;
    return this;
}
El* El::Radius(float r) {
    style.radius = r;
    return this;
}
El* El::Fg(Rgba c) {
    style.color = c;
    style.hasColor = true;
    return this;
}
El* El::Font(float px) {
    style.fontSize = px;
    return this;
}
El* El::LineHeight(float mult) {
    style.lineHeight = mult;
    return this;
}
El* El::Truncate() {
    style.truncate = true;
    return this;
}
El* El::ClipY() {
    style.overflowY = OverflowY::Hidden;
    return this;
}
El* El::ScrollY(float off) {
    style.overflowY = OverflowY::Scroll;
    scrollY = off;
    return this;
}
El* El::ScrollId(int v) {
    scrollId = v;
    return this;
}
El* El::Click(int v) {
    clickId = v;
    return this;
}
El* El::OnClick(Func0 fn) {
    onClick = fn;
    return this;
}
El* El::OnClick(Listener l) {
    listener = l;
    return this;
}
El* El::OnMouseDown(Listener l) {
    onMouseDown = l;
    return this;
}
El* El::OnMouseUp(Listener l) {
    onMouseUp = l;
    return this;
}
El* El::OnDragMove(Listener l) {
    onDragMove = l;
    return this;
}
El* El::BindSlider(SliderState* s, Axis axis) {
    slider = s;
    sliderAxis = axis;
    return this;
}
El* El::BindSliderBounds(SliderState* s) {
    sliderBounds = s;
    return this;
}

int HashClickId(Str s) {
    uint32_t h = 2166136261u;
    if (s.s) {
        for (int i = 0; i < s.len; i++) {
            h ^= (uint8_t)s.s[i];
            h *= 16777619u;
        }
    }
    int id = (int)(h & 0x3fffffff);
    if (id < 1000) {
        id += 1000;
    }
    return id;
}
El* El::Bold() {
    style.fontBold = true;
    return this;
}
El* El::Semibold() {
    style.fontSemibold = true;
    return this;
}
El* El::Medium() {
    style.fontMedium = true;
    return this;
}
El* El::Mono() {
    style.fontMono = true;
    return this;
}
El* El::Underline() {
    style.underline = true;
    return this;
}
El* El::Italic() {
    style.italic = true;
    return this;
}
El* El::Selectable() {
    selectable = true;
    return this;
}
El* El::Wrap() {
    style.wrap = true;
    return this;
}
El* El::Dashed() {
    style.borderDashed = true;
    return this;
}
El* El::Absolute() {
    style.absolute = true;
    return this;
}
El* El::Fixed() {
    style.absolute = true;
    style.fixed = true;
    return this;
}
El* El::Deferred() {
    style.deferred = true;
    return this;
}
El* El::AnchorBelow(float gap) {
    style.absolute = true;
    style.anchorBelow = true;
    style.anchorGap = gap;
    return this;
}
El* El::AnchorAbove(float gap) {
    style.absolute = true;
    style.anchorAbove = true;
    style.anchorGap = gap;
    return this;
}
El* El::AnchorCenterX() {
    style.absolute = true;
    style.anchorCenterX = true;
    return this;
}
El* El::Top(float v) {
    style.absTop = v;
    return this;
}
El* El::Left(float v) {
    style.absLeft = v;
    return this;
}
El* El::Bottom(float v) {
    style.absBottom = v;
    return this;
}
El* El::Right(float v) {
    style.absRight = v;
    return this;
}
El* El::HoverBg(Rgba c) {
    style.hoverBg = c;
    style.hasHoverBg = true;
    return this;
}
El* El::HoverFg(Rgba c) {
    style.hoverFg = c;
    style.hasHoverFg = true;
    return this;
}
El* El::FocusId(int v) {
    style.focusId = v;
    return this;
}
El* El::TrapId(int v) {
    style.trapId = v;
    return this;
}
El* El::Tip(Str s) {
    style.tooltip = s;
    return this;
}
El* El::Id(Str s) {
    id = s;
    return this;
}
El* El::Child(El* c) {
    if (!c) {
        return this;
    }
    c->next = nullptr;
    if (last) {
        last->next = c;
    } else {
        first = c;
    }
    last = c;
    return this;
}

float PxToDip(PaintCtx* ctx, int px) {
    return px * 96.f / (ctx->dpi > 0 ? ctx->dpi : 96.f);
}
int DipToPx(PaintCtx* ctx, float dip) {
    return (int)(dip * (ctx->dpi > 0 ? ctx->dpi : 96.f) / 96.f + 0.5f);
}

static float MeasKeyMaxW(float maxW, bool wrap) {
    if (!wrap || maxW <= 0) {
        return 0;
    }
    return floorf(maxW + 0.5f);
}

static float MeasKeyFont(float fontSize) {
    if (fontSize <= 0) {
        return 16.f;
    }
    return floorf(fontSize * 4.f + 0.5f) / 4.f;
}

static bool memeq(const void* s1, const void* s2, int n) {
    return 0 == memcmp(s1, s2, (size_t)n);
}

static uint32_t MurmurHash2(const void* key, int n) {
    if (n <= 0) {
        return 0;
    }
    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    uint32_t h = 5381u ^ (uint32_t)n;
    const uint8_t* data = (const uint8_t*)key;
    while (n >= 4) {
        uint32_t k = *(uint32_t*)data;
        k *= m;
        k ^= k >> r;
        k *= m;
        h *= m;
        h ^= k;
        data += 4;
        n -= 4;
    }
    switch (n) {
        case 3:
            h ^= data[2] << 16;
            [[fallthrough]];
        case 2:
            h ^= data[1] << 8;
            [[fallthrough]];
        case 1:
            h ^= data[0];
            h *= m;
    }
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return h;
}

static uint32_t MurmurHash2(Str s) {
    return MurmurHash2(s.s, s.len);
}

struct TextMeasSlot {
    char* text = nullptr;
    int len = 0;
    uint32_t hash = 0;
    float fontSize = 0;
    float maxW = 0;

    float lineH = 0;
    float w = 0;
    float h = 0;
    uint32_t lastUsed = 0;
    TextLayout* layout = nullptr;
    uint8_t wrap = 0;
    uint8_t bold = 0;
    uint8_t occupied = 0;
};

static uint32_t TextMeasHash(Str s, float fontSize, float maxW, bool wrap,
                             uint8_t weight, float lineH) {
    uint32_t h = MurmurHash2(s);
    uint32_t fs = 0;
    uint32_t mw = 0;
    uint32_t lh = 0;
    memcpy(&fs, &fontSize, sizeof(fs));
    memcpy(&mw, &maxW, sizeof(mw));
    memcpy(&lh, &lineH, sizeof(lh));
    h ^= fs * 0x9e3779b9u;
    h ^= mw * 0x85ebca6bu;
    h ^= lh * 0xc2b2ae35u;
    if (wrap) {
        h ^= 0x165667b1u;
    }
    if (weight) {
        h ^= 0x27d4eb2fu * (uint32_t)weight;
    }
    return h;
}

static bool TextMeasKeyEq(const TextMeasSlot* sl, uint32_t hash, Str s,
                          float fontSize, float maxW, bool wrap, uint8_t weight,
                          float lineH) {
    if (!sl->occupied || sl->hash != hash || sl->len != s.len) {
        return false;
    }
    if (sl->fontSize != fontSize || sl->maxW != maxW || sl->lineH != lineH ||
        sl->wrap != (wrap ? 1 : 0) || sl->bold != weight) {
        return false;
    }
    return memeq(sl->text, s.s, s.len);
}

static uint8_t ElTextWeight(const El* e) {
    uint8_t w = kFontWeightNormal;
    if (e->style.fontBold) {
        w = kFontWeightBold;
    } else if (e->style.fontSemibold) {
        w = kFontWeightSemibold;
    } else if (e->style.fontMedium) {
        w = kFontWeightMedium;
    }
    if (e->style.fontMono) {
        w |= kFontMono;
    }
    if (e->style.underline) {
        w |= kFontUnderline;
    }
    if (e->style.italic) {
        w |= kFontItalic;
    }
    return w;
}

static void TextMeasFreeSlot(TextMeasSlot* sl) {
    if (!sl) {
        return;
    }
    if (sl->text) {
        StrFree(Str{sl->text, sl->len});
        sl->text = nullptr;
    }
    if (sl->layout) {
        TextLayoutRelease(sl->layout);
        sl->layout = nullptr;
    }
    sl->occupied = 0;
    sl->len = 0;
}

static TextMeasSlot* TextMeasFind(TextMeasCache* c, Str s, float fontSize,
                                  float maxW, bool wrap, uint8_t weight,
                                  float lineH, uint32_t* outHash) {
    float keyFont = MeasKeyFont(fontSize);
    float keyMaxW = MeasKeyMaxW(maxW, wrap);
    uint32_t hash = TextMeasHash(s, keyFont, keyMaxW, wrap, weight, lineH);
    if (outHash) {
        *outHash = hash;
    }
    if (!c->slots || c->cap <= 0) {
        return nullptr;
    }
    int mask = c->cap - 1;
    int i = (int)(hash & (uint32_t)mask);
    for (int n = 0; n < c->cap; n++) {
        TextMeasSlot* sl = &((TextMeasSlot*)c->slots)[i];
        if (!sl->occupied) {
            return nullptr;
        }
        if (TextMeasKeyEq(sl, hash, s, keyFont, keyMaxW, wrap, weight, lineH)) {
            return sl;
        }
        i = (i + 1) & mask;
    }
    return nullptr;
}

static void TextMeasInsertMove(TextMeasCache* c, TextMeasSlot* src);

static void TextMeasGrow(TextMeasCache* c, int minCap) {
    int cap = c->cap > 0 ? c->cap : 256;
    while (cap < minCap) {
        cap *= 2;
    }
    TextMeasSlot* old = (TextMeasSlot*)c->slots;
    int oldCap = c->cap;
    TextMeasSlot* neu = AllocArray<TextMeasSlot>(cap);
    if (!neu) {
        return;
    }
    c->slots = neu;
    c->cap = cap;
    c->used = 0;
    if (old) {
        for (int i = 0; i < oldCap; i++) {
            if (old[i].occupied) {
                TextMeasInsertMove(c, &old[i]);
            }
        }
        Free(nullptr, old);
    }
}

static void TextMeasInsertMove(TextMeasCache* c, TextMeasSlot* src) {
    if (!c->slots || c->cap <= 0) {
        return;
    }
    int mask = c->cap - 1;
    int i = (int)(src->hash & (uint32_t)mask);
    for (int n = 0; n < c->cap; n++) {
        TextMeasSlot* sl = &((TextMeasSlot*)c->slots)[i];
        if (!sl->occupied) {
            *sl = *src;
            sl->occupied = 1;
            c->used++;
            src->text = nullptr;
            src->layout = nullptr;
            src->occupied = 0;
            return;
        }
        i = (i + 1) & mask;
    }
    TextMeasFreeSlot(src);
}

static TextMeasSlot* TextMeasInsert(PaintCtx* ctx, Str s, float fontSize,
                                    float maxW, bool wrap, uint8_t weight,
                                    float lineH, float w, float h,
                                    TextLayout* layout) {
    TextMeasCache* c = &ctx->textCache;
    float keyFont = MeasKeyFont(fontSize);
    float keyMaxW = MeasKeyMaxW(maxW, wrap);
    uint32_t hash = TextMeasHash(s, keyFont, keyMaxW, wrap, weight, lineH);
    if (c->cap == 0 || (c->used + 1) * 10 > c->cap * 6) {
        TextMeasGrow(c, c->cap > 0 ? c->cap * 2 : 256);
    }
    if (!c->slots || c->cap <= 0) {
        return nullptr;
    }
    int mask = c->cap - 1;
    int i = (int)(hash & (uint32_t)mask);
    TextMeasSlot* sl = nullptr;
    for (int n = 0; n < c->cap; n++) {
        TextMeasSlot* cand = &((TextMeasSlot*)c->slots)[i];
        if (!cand->occupied) {
            sl = cand;
            break;
        }
        if (TextMeasKeyEq(cand, hash, s, keyFont, keyMaxW, wrap, weight,
                          lineH)) {
            sl = cand;
            break;
        }
        i = (i + 1) & mask;
    }
    if (!sl) {
        return nullptr;
    }
    if (!sl->occupied) {
        Str copy = StrDup(s);
        if (!copy.s) {
            return nullptr;
        }
        sl->text = copy.s;
        sl->len = copy.len;
        sl->hash = hash;
        sl->fontSize = keyFont;
        sl->maxW = keyMaxW;
        sl->lineH = lineH;
        sl->wrap = wrap ? 1 : 0;
        sl->bold = weight;
        sl->occupied = 1;
        c->used++;
    }
    sl->w = w;
    sl->h = h;
    sl->lastUsed = c->frame;
    if (layout && sl->layout != layout) {
        if (sl->layout) {
            TextLayoutRelease(sl->layout);
        }
        TextLayoutAddRef(layout);
        sl->layout = layout;
    }
    return sl;
}

void TextMeasBeginFrame(PaintCtx* ctx) {
    if (!ctx) {
        return;
    }
    ctx->textCache.frame++;
    if (ctx->textCache.frame == 0) {
        ctx->textCache.frame = 1;
    }
}

void TextMeasEndFrame(PaintCtx* ctx) {
    if (!ctx) {
        return;
    }
    TextMeasCache* c = &ctx->textCache;
    if (!c->slots || c->cap <= 0) {
        return;
    }
    uint32_t frame = c->frame;
    TextMeasSlot* old = (TextMeasSlot*)c->slots;
    int oldCap = c->cap;
    int keep = 0;
    for (int i = 0; i < oldCap; i++) {
        if (old[i].occupied && old[i].lastUsed + 1 >= frame) {
            keep++;
        }
    }
    int newCap = c->cap;
    if (keep * 4 < newCap && newCap > 256) {
        newCap = 256;
        while (newCap < keep * 2) {
            newCap *= 2;
        }
    }
    TextMeasSlot* neu = AllocArray<TextMeasSlot>(newCap);
    if (!neu) {
        return;
    }
    c->slots = neu;
    c->cap = newCap;
    c->used = 0;
    for (int i = 0; i < oldCap; i++) {
        if (!old[i].occupied) {
            continue;
        }
        if (old[i].lastUsed + 1 < frame) {
            TextMeasFreeSlot(&old[i]);
            continue;
        }
        TextMeasInsertMove(c, &old[i]);
    }
    Free(nullptr, old);
}

void TextMeasClear(PaintCtx* ctx) {
    if (!ctx) {
        return;
    }
    TextMeasCache* c = &ctx->textCache;
    TextMeasSlot* slots = (TextMeasSlot*)c->slots;
    if (slots) {
        for (int i = 0; i < c->cap; i++) {
            if (slots[i].occupied) {
                TextMeasFreeSlot(&slots[i]);
            }
        }
        Free(nullptr, slots);
    }
    c->slots = nullptr;
    c->cap = 0;
    c->used = 0;
    c->frame = 0;
}

static TextLayout* TextMeasLayout(PaintCtx* ctx, Str s, float fontSize,
                                  float maxW, bool wrap, uint8_t weight,
                                  float lineH, Size* outSize,
                                  bool* outCached = nullptr) {
    if (outCached) {
        *outCached = false;
    }
    if (outSize) {
        outSize->w = 0;
        outSize->h =
            fontSize > 0 ? fontSize * (lineH > 0 ? lineH : kLineHeight) : 16.f;
    }
    if (!ctx || !ctx->pa || !s.s || s.len <= 0) {
        return nullptr;
    }
    TextMeasCache* c = &ctx->textCache;
    TextMeasSlot* hit =
        TextMeasFind(c, s, fontSize, maxW, wrap, weight, lineH, nullptr);
    if (hit && hit->layout) {
        hit->lastUsed = c->frame;
        if (outCached) {
            *outCached = true;
        }
        if (outSize) {
            outSize->w = hit->w;
            outSize->h = hit->h;
        }
        TextLayoutAddRef(hit->layout);
        return hit->layout;
    }
    Size size = {};
    TextLayout* layout =
        TextLayoutNew(ctx, s, fontSize, maxW, wrap, weight, lineH, &size);
    if (!layout) {
        return nullptr;
    }
    if (outSize) {
        *outSize = size;
    }
    TextMeasSlot* sl = TextMeasInsert(ctx, s, fontSize, maxW, wrap, weight,
                                      lineH, size.w, size.h, layout);
    if (outCached) {
        *outCached = sl != nullptr;
    }
    return layout;
}

Size MeasureText(PaintCtx* ctx, Str s, float fontSize, float maxW, bool wrap,
                 int weight, float lineH) {
    Size size = {};
    TextLayout* layout = TextMeasLayout(ctx, s, fontSize, maxW, wrap,
                                        (uint8_t)weight, lineH, &size);
    if (layout) {
        TextLayoutRelease(layout);
    }
    return size;
}

int TextIndexAt(PaintCtx* ctx, Str s, float fontSize, float maxW, bool wrap,
                float relX, float relY) {
    TextLayout* layout =
        TextMeasLayout(ctx, s, fontSize, maxW, wrap, 0, 0, nullptr);
    if (!layout) {
        return 0;
    }
    int off = TextLayoutHitPoint(layout, s, relX, relY);
    TextLayoutRelease(layout);
    return off;
}

void PaintTextRange(PaintCtx* ctx, Str s, float fontSize, float maxW, bool wrap,
                    float x, float y, int u8a, int u8b, Rgba color) {
    if (!ctx || !ctx->rt || color.a == 0) {
        return;
    }
    if (u8a > u8b) {
        int t = u8a;
        u8a = u8b;
        u8b = t;
    }
    if (u8a == u8b) {
        return;
    }
    TextLayout* layout =
        TextMeasLayout(ctx, s, fontSize, maxW, wrap, 0, 0, nullptr);
    if (!layout) {
        return;
    }

    Bounds rects[32] = {};
    int n = TextLayoutRangeRects(layout, s, u8a, u8b, rects, 32);
    for (int i = 0; i < n; i++) {
        CanvasFillRect(ctx, x + rects[i].x, y + rects[i].y, rects[i].w,
                       rects[i].h, color);
    }
    TextLayoutRelease(layout);
}

static float Clamp(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static float ResolveSize(float spec, float avail, float growAsFill) {
    (void)growAsFill;
    if (spec == kFill) {
        return avail;
    }
    if (spec == kAuto) {
        return -1.f;
    }
    if (spec >= 0) {
        return spec;
    }
    return -1.f;
}

static void LayoutChildren(PaintCtx* ctx, El* e, float inheritFont,
                           Rgba inheritFg);

static void TranslateSubtree(El* e, float dx, float dy) {
    for (El* c = e->first; c; c = c->next) {
        c->x += dx;
        c->y += dy;
        TranslateSubtree(c, dx, dy);
    }
}

static void MoveEl(El* c, float cx, float cy) {
    float dx = cx - c->x;
    float dy = cy - c->y;
    if (dx == 0 && dy == 0) {
        return;
    }
    c->x = cx;
    c->y = cy;
    TranslateSubtree(c, dx, dy);
}

static bool RgbaEq(Rgba a, Rgba b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static void StampFg(El* e, Rgba c) {
    for (El* ch = e->first; ch; ch = ch->next) {
        if (ch->style.hasColor) {
            continue;
        }
        ch->style.color = c;
        ch->style.hasColor = true;
        StampFg(ch, c);
    }
}

static void LayoutMemoStore(El* e, float availW, float availH,
                            float inheritFont, Rgba inheritFg) {
    e->memoAvailW = availW;
    e->memoAvailH = availH;
    e->memoFont = inheritFont;
    e->memoFg = inheritFg;
    e->memoW = e->w;
    e->memoH = e->h;
    e->memoContentW = e->contentW;
    e->memoContentH = e->contentH;
    e->memoValid = true;
}

void LayoutEl(PaintCtx* ctx, El* e, float x, float y, float availW,
              float availH, float inheritFont, Rgba inheritFg) {
    if (!e) {
        return;
    }

    if (e->memoValid && e->memoAvailW == availW && e->memoAvailH == availH &&
        e->memoFont == inheritFont && RgbaEq(e->memoFg, inheritFg)) {
        e->w = e->memoW;
        e->h = e->memoH;
        e->contentW = e->memoContentW;
        e->contentH = e->memoContentH;
        MoveEl(e, x, y);
        return;
    }
    float font = e->style.fontSize > 0 ? e->style.fontSize : inheritFont;
    Rgba fg = e->style.hasColor ? e->style.color : inheritFg;

    if (e->style.hasHoverFg && e->clickId && e->clickId == ctx->hoverId) {
        fg = e->style.hoverFg;
        StampFg(e, fg);
    }

    if (e->style.fontMono) {
        for (El* c = e->first; c; c = c->next) {
            c->style.fontMono = true;
        }
    }

    float wSpec = ResolveSize(e->style.width, availW, e->style.flexGrow);
    float hSpec = ResolveSize(e->style.height, availH, e->style.flexGrow);

    e->x = x;
    e->y = y;

    if (e->kind == ElKind::Text) {
        float boxW = wSpec > 0 ? wSpec : availW;
        if (boxW > e->style.maxW) {
            boxW = e->style.maxW;
        }
        bool constrain = e->style.wrap || e->style.truncate;
        float measW = constrain && boxW > 0 ? boxW : 0;
        e->laidFont = font;
        e->laidMaxW = measW;
        Size text = {};

        bool cached = false;
        TextLayout* tl = TextMeasLayout(ctx, e->text, font, measW,
                                        e->style.wrap, (uint8_t)ElTextWeight(e),
                                        e->style.lineHeight, &text, &cached);
        e->laidLayout = cached ? tl : nullptr;
        if (tl) {
            TextLayoutRelease(tl);
        }
        e->w = wSpec > 0 ? wSpec : Clamp(text.w, e->style.minW, e->style.maxW);
        e->h = hSpec > 0 ? hSpec : Clamp(text.h, e->style.minH, e->style.maxH);
        LayoutMemoStore(e, availW, availH, inheritFont, inheritFg);
        return;
    }
    if (e->kind == ElKind::Icon) {
        e->w = wSpec > 0 ? wSpec : 16;
        e->h = hSpec > 0 ? hSpec : 16;
        LayoutMemoStore(e, availW, availH, inheritFont, inheritFg);
        return;
    }
    if (e->kind == ElKind::Progress) {
        e->w = wSpec > 0 ? wSpec : 48;
        e->h = hSpec > 0 ? hSpec : 8;
        LayoutMemoStore(e, availW, availH, inheritFont, inheritFg);
        return;
    }

    float padX = e->style.pad.Horizontal();
    float padY = e->style.pad.Vertical();
    float innerW = (wSpec > 0 ? wSpec : availW) - padX;
    float innerH = (hSpec > 0 ? hSpec : availH) - padY;
    if (innerW < 0) {
        innerW = 0;
    }
    if (innerH < 0) {
        innerH = 0;
    }

    e->w = wSpec > 0 ? wSpec : availW;
    e->h = hSpec > 0 ? hSpec : availH;
    LayoutChildren(ctx, e, font, fg);

    bool wrapW = (wSpec < 0 && e->style.flexGrow <= 0);
    bool wrapH = (hSpec < 0);
    bool resized = false;
    if (wrapW) {
        float needed = e->contentW + padX;
        float nw = Clamp(needed, e->style.minW, e->style.maxW);
        if (availW > 0 && nw > availW) {
            nw = availW;
        }
        if (nw != e->w) {
            e->w = nw;
            resized = true;
        }
    }

    if (wrapH && e->style.overflowY != OverflowY::Scroll) {
        float needed = e->contentH + padY;
        float nh = Clamp(needed, e->style.minH, e->style.maxH);
        if (availH > 0 && nh > availH) {
            nh = availH;
        }
        if (nh != e->h) {
            e->h = nh;
            resized = true;
        }
    }
    if (resized) {
        LayoutChildren(ctx, e, font, fg);
    }
    float prevW = e->w;
    float prevH = e->h;
    e->w = Clamp(e->w, e->style.minW, e->style.maxW);
    e->h = Clamp(e->h, e->style.minH, e->style.maxH);
    if (e->w != prevW || e->h != prevH) {
        LayoutChildren(ctx, e, font, fg);
    }
    LayoutMemoStore(e, availW, availH, inheritFont, inheritFg);
}

static void PlaceOutOfFlow(PaintCtx* ctx, El* parent, El* c, float inheritFont,
                           Rgba inheritFg) {
    float ax;
    float ay;
    float aw;
    float ah;
    if (c->style.fixed) {
        ax = c->style.absLeft != kAuto ? c->style.absLeft : 0;
        ay = c->style.absTop != kAuto ? c->style.absTop : 0;
        if (c->style.width == kFill) {
            aw = ctx->viewW;
        } else if (c->style.width >= 0) {
            aw = c->style.width;
        } else {
            aw = ctx->viewW > 0 ? ctx->viewW : 10000.f;
        }
        if (c->style.height == kFill) {
            ah = ctx->viewH;
        } else if (c->style.height >= 0) {
            ah = c->style.height;
        } else {
            ah = ctx->viewH > 0 ? ctx->viewH : 10000.f;
        }
        LayoutEl(ctx, c, ax, ay, aw, ah, inheritFont, inheritFg);
        if (c->style.absRight != kAuto) {
            ax = ctx->viewW - c->style.absRight - c->w;
        }
        if (c->style.absBottom != kAuto) {
            ay = ctx->viewH - c->style.absBottom - c->h;
        }
        c->x = ax;
        c->y = ay;
        if (c->first) {
            LayoutEl(ctx, c, ax, ay, c->w, c->h, inheritFont, inheritFg);
        }
        return;
    }
    Bounds inner = parent->Bounds().Inset(parent->style.pad);
    ax = inner.x;
    ay = inner.y;
    float innerW = inner.w;
    float innerH = inner.h;
    if (innerW < 0) {
        innerW = 0;
    }
    if (innerH < 0) {
        innerH = 0;
    }
    if (c->style.width == kFill) {
        aw = innerW;
    } else if (c->style.width >= 0) {
        aw = c->style.width;
    } else {
        aw = 10000.f;
    }
    if (c->style.height == kFill) {
        ah = innerH;
    } else if (c->style.height >= 0) {
        ah = c->style.height;
    } else {
        ah = 10000.f;
    }
    LayoutEl(ctx, c, ax, ay, aw, ah, inheritFont, inheritFg);
    if (c->style.absLeft != kAuto) {
        ax = parent->x + c->style.absLeft;
    }
    if (c->style.absTop != kAuto) {
        ay = parent->y + c->style.absTop;
    }
    if (c->style.absRight != kAuto) {
        ax = parent->x + parent->w - c->style.absRight - c->w;
    }
    if (c->style.absBottom != kAuto) {
        ay = parent->y + parent->h - c->style.absBottom - c->h;
    }
    if (c->style.anchorBelow) {
        ay = parent->y + parent->h + c->style.anchorGap;
    }
    if (c->style.anchorAbove) {
        ay = parent->y - c->h - c->style.anchorGap;
    }
    if (c->style.anchorCenterX) {
        ax = parent->x + (parent->w - c->w) * 0.5f;
    }
    MoveEl(c, ax, ay);
}

static void LayoutChildren(PaintCtx* ctx, El* e, float inheritFont,
                           Rgba inheritFg) {
    float padL = e->style.pad.left;
    float padT = e->style.pad.top;
    float innerW = e->w - e->style.pad.Horizontal();
    float innerH = e->h - e->style.pad.Vertical();
    if (innerW < 0) {
        innerW = 0;
    }
    if (innerH < 0) {
        innerH = 0;
    }

    int n = 0;
    for (El* c = e->first; c; c = c->next) {
        if (!c->style.absolute) {
            n++;
        }
    }
    if (n == 0) {
        e->contentW = 0;
        e->contentH = 0;
        for (El* c = e->first; c; c = c->next) {
            if (c->style.absolute) {
                PlaceOutOfFlow(ctx, e, c, inheritFont, inheritFg);
            }
        }
        return;
    }

    if (innerW > 0) {
        for (El* c = e->first; c; c = c->next) {
            if (c->style.widthFrac > 0) {
                c->style.width = innerW * c->style.widthFrac;
                c->style.widthFrac = 0;
            }
        }
    }

    bool row = e->style.dir == FlexDir::Row;
    float mainAvail = row ? innerW : innerH;
    float crossAvail = row ? innerH : innerW;
    float gap = e->style.gap;
    float gaps = gap * (n - 1);

    bool shrinkWrapH = e->style.height == kAuto && e->style.flexGrow <= 0 &&
                       e->style.overflowY != OverflowY::Scroll;
    bool unconstrH = e->style.overflowY == OverflowY::Scroll || shrinkWrapH;
    float childCross0 = (unconstrH && row) ? 0.f : crossAvail;
    float childMain0 = (unconstrH && !row) ? 0.f : mainAvail;

    float used = 0;
    float growSum = 0;
    for (El* c = e->first; c; c = c->next) {
        if (c->style.absolute) {
            continue;
        }
        growSum += c->style.flexGrow;
        if (c->style.flexGrow > 0) {
            continue;
        }
        if (row) {
            LayoutEl(ctx, c, 0, 0, childMain0, childCross0, inheritFont,
                     inheritFg);
        } else {
            LayoutEl(ctx, c, 0, 0, childCross0, childMain0, inheritFont,
                     inheritFg);
        }
        used += row ? c->w : c->h;
    }
    used += gaps;
    float remain = mainAvail - used;
    if (remain < 0) {
        remain = 0;
    }
    for (El* c = e->first; c; c = c->next) {
        if (c->style.absolute || c->style.flexGrow <= 0) {
            continue;
        }
        float growMain = (row && c->style.wrap) ? remain : 0.f;
        if (row) {
            LayoutEl(ctx, c, 0, 0, growMain, childCross0, inheritFont,
                     inheritFg);
        } else {
            LayoutEl(ctx, c, 0, 0, childCross0, growMain, inheritFont,
                     inheritFg);
        }
        used += row ? c->w : c->h;
    }

    float leftover = mainAvail - used;
    if (leftover < 0) {
        leftover = 0;
    }

    if (growSum > 0 && leftover > 0) {
        for (El* c = e->first; c; c = c->next) {
            if (c->style.absolute || c->style.flexGrow <= 0) {
                continue;
            }
            float extra = leftover * (c->style.flexGrow / growSum);
            if (row) {
                float w = c->w + extra;
                if (w < c->style.minW) {
                    w = c->style.minW;
                }
                LayoutEl(ctx, c, 0, 0, w, crossAvail, inheritFont, inheritFg);
                c->w = w;
            } else {
                float h = c->h + extra;
                if (h < c->style.minH) {
                    h = c->style.minH;
                }
                LayoutEl(ctx, c, 0, 0, crossAvail, h, inheritFont, inheritFg);
                c->h = h;
            }
        }
    }

    if (row && e->style.flexWrap && mainAvail > 0) {
        enum {
            kMaxWrapItems = 256
        };
        El* items[kMaxWrapItems];
        int nItems = 0;
        bool tooMany = false;
        for (El* c = e->first; c; c = c->next) {
            if (c->style.absolute) {
                continue;
            }
            if (nItems >= kMaxWrapItems) {
                tooMany = true;
                break;
            }
            items[nItems++] = c;
        }
        if (!tooMany) {
            float lineY = 0;
            float widest = 0;
            int i = 0;
            while (i < nItems) {
                int j = i;
                float lineW = 0;
                float lineH = 0;
                while (j < nItems) {
                    float next =
                        j > i ? lineW + gap + items[j]->w : items[j]->w;
                    if (j > i && next > mainAvail) {
                        break;
                    }
                    lineW = next;
                    if (items[j]->h > lineH) {
                        lineH = items[j]->h;
                    }
                    j++;
                }
                float x = 0;
                if (e->style.justify == Justify::Center) {
                    x = (mainAvail - lineW) * 0.5f;
                } else if (e->style.justify == Justify::End) {
                    x = mainAvail - lineW;
                }
                if (x < 0) {
                    x = 0;
                }
                for (int k = i; k < j; k++) {
                    El* c = items[k];
                    float cross = 0;
                    if (e->style.align == Align::Center) {
                        cross = (lineH - c->h) * 0.5f;
                    } else if (e->style.align == Align::End) {
                        cross = lineH - c->h;
                    }
                    float cx = e->x + padL + x;
                    float cy = e->y + padT + lineY + cross - e->scrollY;
                    MoveEl(c, cx, cy);
                    x += c->w + gap;
                }
                if (lineW > widest) {
                    widest = lineW;
                }
                lineY += lineH + gap;
                i = j;
            }
            e->contentW = widest;
            e->contentH = lineY > 0 ? lineY - gap : 0;
            for (El* c = e->first; c; c = c->next) {
                if (c->style.absolute) {
                    PlaceOutOfFlow(ctx, e, c, inheritFont, inheritFg);
                }
            }
            return;
        }
    }

    float cursor = 0;
    if (e->style.justify == Justify::Center) {
        float total = -gap;
        for (El* c = e->first; c; c = c->next) {
            if (c->style.absolute) {
                continue;
            }
            total += (row ? c->w : c->h) + gap;
        }
        cursor = (mainAvail - total) * 0.5f;
        if (cursor < 0) {
            cursor = 0;
        }
    } else if (e->style.justify == Justify::End) {
        float total = -gap;
        for (El* c = e->first; c; c = c->next) {
            if (c->style.absolute) {
                continue;
            }
            total += (row ? c->w : c->h) + gap;
        }
        cursor = mainAvail - total;
        if (cursor < 0) {
            cursor = 0;
        }
    }

    float betweenExtra = 0;
    if (e->style.justify == Justify::SpaceBetween && n > 1) {
        float total = 0;
        for (El* c = e->first; c; c = c->next) {
            if (c->style.absolute) {
                continue;
            }
            total += row ? c->w : c->h;
        }

        float free = mainAvail - total - gaps;
        if (free > 0) {
            betweenExtra = free / (n - 1);
        }
    }

    float maxCross = 0;
    for (El* c = e->first; c; c = c->next) {
        if (c->style.absolute) {
            continue;
        }
        float cw = c->w;
        float ch = c->h;
        float cross = 0;
        if (e->style.align == Align::Center) {
            cross = ((row ? innerH : innerW) - (row ? ch : cw)) * 0.5f;
        } else if (e->style.align == Align::End) {
            cross = (row ? innerH : innerW) - (row ? ch : cw);
        } else if (e->style.align == Align::Stretch) {

            if (row && e->style.height != kAuto) {
                ch = innerH;
                c->h = ch;
            } else if (!row && e->style.width != kAuto) {
                cw = innerW;
                c->w = cw;
            }
        }
        if (cross < 0) {
            cross = 0;
        }

        float cx, cy;
        if (row) {
            cx = e->x + padL + cursor;
            cy = e->y + padT + cross - e->scrollY;
        } else {
            cx = e->x + padL + cross;
            cy = e->y + padT + cursor - e->scrollY;
        }

        MoveEl(c, cx, cy);

        float step = (row ? c->w : c->h) + gap + betweenExtra;
        cursor += step;
        float cr = row ? c->h : c->w;
        if (cr > maxCross) {
            maxCross = cr;
        }
    }

    if (row && maxCross > 0) {
        for (El* c = e->first; c; c = c->next) {
            if (c->style.absolute) {
                continue;
            }
            bool fillCross =
                c->style.height == kFill ||
                (e->style.align == Align::Stretch && c->style.height == kAuto);
            if (fillCross && maxCross > c->h) {
                c->h = maxCross;
            }
        }
    }

    float intrinsicMain = 0;
    int inN = 0;
    for (El* c = e->first; c; c = c->next) {
        if (c->style.absolute) {
            continue;
        }
        if (inN) {
            intrinsicMain += gap;
        }
        intrinsicMain += row ? c->w : c->h;
        inN++;
    }
    e->contentW = row ? intrinsicMain : maxCross;
    e->contentH = row ? maxCross : intrinsicMain;

    for (El* c = e->first; c; c = c->next) {
        if (!c->style.absolute) {
            continue;
        }
        PlaceOutOfFlow(ctx, e, c, inheritFont, inheritFg);
    }
}

static void FillRound(PaintCtx* ctx, float x, float y, float w, float h,
                      float r, Rgba c) {
    CanvasFillRound(ctx, x, y, w, h, r, c);
}

static void DrawRoundStroke(PaintCtx* ctx, float x, float y, float w, float h,
                            float r, float stroke, Rgba c) {
    CanvasStrokeRound(ctx, x, y, w, h, r, stroke, c);
}

static float EdgeLine(PaintCtx* ctx, float v) {
    float scale = ctx->dpi > 0 ? (float)ctx->dpi / 96.f : 1.f;
    float px = v * scale;
    return (floorf(px) + 0.5f) / scale;
}

static float EdgeEnd(PaintCtx* ctx, float v) {
    float scale = ctx->dpi > 0 ? (float)ctx->dpi / 96.f : 1.f;
    return floorf(v * scale + 0.5f) / scale;
}

static void DrawLine(PaintCtx* ctx, float x1, float y1, float x2, float y2,
                     float stroke, Rgba c) {
    CanvasLine(ctx, x1, y1, x2, y2, stroke, c);
}

static void DrawTextAt(PaintCtx* ctx, Str s, float x, float y, float w, float h,
                       float fontSize, Rgba c, bool truncate, bool wrap = false,
                       float measMaxW = -1.f, int weight = 0, float lineH = 0) {
    if (!s.s || s.len <= 0 || !ctx->pa) {
        return;
    }
    (void)w;
    (void)h;
    float keyW = wrap ? (measMaxW >= 0 ? measMaxW : (w > 0 ? w : 0)) : 0;
    TextLayout* layout = TextMeasLayout(ctx, s, fontSize, keyW, wrap,
                                        (uint8_t)weight, lineH, nullptr);
    if (!layout) {
        return;
    }
    TextLayoutDraw(ctx, layout, x, y, c, truncate);
    TextLayoutRelease(layout);
}

static void DrawIcon(PaintCtx* ctx, IconName name, float x, float y, float s,
                     Rgba c) {
    float sw = 1.6f;

    auto PX = [&](float u) { return x + u * s / 24.f; };
    auto PY = [&](float v) { return y + v * s / 24.f; };
    auto line = [&](float x1, float y1, float x2, float y2) {
        CanvasLine(ctx, PX(x1), PY(y1), PX(x2), PY(y2), sw, c);
    };
    auto dot = [&](float u, float v, float r) {
        CanvasEllipse(ctx, PX(u), PY(v), r, r, 0, c);
    };
    auto ring = [&](float u, float v, float rx, float ry) {
        CanvasEllipse(ctx, PX(u), PY(v), rx, ry, sw, c);
    };
    switch (name) {
        case IconName::WindowMinimize:
            line(5, 16, 19, 16);
            break;
        case IconName::WindowMaximize:
            DrawRoundStroke(ctx, x + s * 0.22f, y + s * 0.22f, s * 0.56f,
                            s * 0.56f, 1, sw, c);
            break;
        case IconName::WindowRestore:
            DrawRoundStroke(ctx, x + s * 0.32f, y + s * 0.18f, s * 0.46f,
                            s * 0.46f, 1, sw, c);
            DrawRoundStroke(ctx, x + s * 0.18f, y + s * 0.32f, s * 0.46f,
                            s * 0.46f, 1, sw, c);
            break;
        case IconName::WindowClose:
            line(6, 6, 18, 18);
            line(18, 6, 6, 18);
            break;
        case IconName::Cpu:
            DrawRoundStroke(ctx, x + s * 0.17f, y + s * 0.17f, s * 0.66f,
                            s * 0.66f, s * 0.08f, sw, c);
            DrawRoundStroke(ctx, x + s * 0.33f, y + s * 0.33f, s * 0.34f,
                            s * 0.34f, s * 0.04f, sw, c);
            line(12, 2, 12, 4);
            line(12, 20, 12, 22);
            line(7, 2, 7, 4);
            line(7, 20, 7, 22);
            line(17, 2, 17, 4);
            line(17, 20, 17, 22);
            line(2, 7, 4, 7);
            line(20, 7, 22, 7);
            line(2, 12, 4, 12);
            line(20, 12, 22, 12);
            line(2, 17, 4, 17);
            line(20, 17, 22, 17);
            break;
        case IconName::MemoryStick:
            DrawRoundStroke(ctx, x + s * 0.25f, y + s * 0.12f, s * 0.5f,
                            s * 0.76f, s * 0.08f, sw, c);
            line(9, 7, 15, 7);
            line(9, 11, 15, 11);
            line(9, 15, 15, 15);
            break;
        case IconName::HardDrive:
            DrawRoundStroke(ctx, x + s * 0.12f, y + s * 0.38f, s * 0.76f,
                            s * 0.36f, s * 0.08f, sw, c);
            dot(8, 14, 1.2f);
            break;
        case IconName::Battery:
        case IconName::BatteryMedium:
        case IconName::BatteryFull:
        case IconName::BatteryCharging: {
            DrawRoundStroke(ctx, x + s * 0.08f, y + s * 0.32f, s * 0.72f,
                            s * 0.36f, s * 0.06f, sw, c);
            FillRound(ctx, x + s * 0.80f, y + s * 0.42f, s * 0.08f, s * 0.16f,
                      1, c);
            float fill = 0.35f;
            if (name == IconName::BatteryFull) {
                fill = 0.85f;
            } else if (name == IconName::BatteryMedium) {
                fill = 0.5f;
            } else if (name == IconName::BatteryCharging) {
                fill = 0.6f;
            }
            FillRound(ctx, x + s * 0.14f, y + s * 0.38f, s * 0.60f * fill,
                      s * 0.24f, 1, c);
            if (name == IconName::BatteryCharging) {
                line(13, 8, 10, 13);
                line(10, 13, 14, 13);
                line(14, 13, 11, 18);
            }
            break;
        }
        case IconName::Info:
            ring(12, 12, s * 0.38f, s * 0.38f);
            line(12, 10, 12, 16);
            dot(12, 8, 1.2f);
            break;
        case IconName::X:
        case IconName::CircleX:
            if (name == IconName::CircleX) {
                ring(12, 12, s * 0.38f, s * 0.38f);
            }

            if (name == IconName::CircleX) {
                line(9, 9, 15, 15);
                line(15, 9, 9, 15);
            } else {
                line(6, 6, 18, 18);
                line(18, 6, 6, 18);
            }
            break;
        case IconName::CircleCheck:
            ring(12, 12, s * 0.38f, s * 0.38f);
            line(8, 12, 11, 15);
            line(11, 15, 16, 9);
            break;
        case IconName::TriangleAlert:
            line(12, 5, 20, 19);
            line(20, 19, 4, 19);
            line(4, 19, 12, 5);
            line(12, 10, 12, 14);
            dot(12, 17, 1.1f);
            break;
        case IconName::Loader:
            line(12, 4, 12, 8);
            line(12, 16, 12, 20);
            line(4, 12, 8, 12);
            line(16, 12, 20, 12);
            line(6, 6, 9, 9);
            line(15, 15, 18, 18);
            line(18, 6, 15, 9);
            line(9, 15, 6, 18);
            break;
        case IconName::ChevronDown:
            line(6, 9, 12, 15);
            line(12, 15, 18, 9);
            break;
        case IconName::ChevronLeft:
            line(15, 6, 9, 12);
            line(9, 12, 15, 18);
            break;
        case IconName::ChevronRight:
            line(9, 6, 15, 12);
            line(15, 12, 9, 18);
            break;
        case IconName::ChevronUp:
            line(6, 15, 12, 9);
            line(12, 9, 18, 15);
            break;
        case IconName::Check:
            line(6, 12, 10, 16);
            line(10, 16, 18, 8);
            break;
        case IconName::Search:
            ring(10, 10, s * 0.22f, s * 0.22f);
            line(14, 14, 20, 20);
            break;
        case IconName::Minus:
            line(6, 12, 18, 12);
            break;
        case IconName::Plus:
            line(6, 12, 18, 12);
            line(12, 6, 12, 18);
            break;
        case IconName::Copy:

            DrawRoundStroke(ctx, x + s * (8.f / 24.f), y + s * (8.f / 24.f),
                            s * (13.f / 24.f), s * (13.f / 24.f), s * 0.08f, sw,
                            c);
            line(5, 15, 4, 15);
            line(4, 15, 4, 4);
            line(4, 4, 15, 4);
            line(15, 4, 15, 5);
            break;
        case IconName::Bell:
            line(12, 4, 12, 5);
            DrawRoundStroke(ctx, x + s * 0.29f, y + s * 0.25f, s * 0.42f,
                            s * 0.42f, s * 0.18f, sw, c);
            line(7, 16, 17, 16);
            dot(12, 19, 1.2f);
            break;
        case IconName::Star:
            line(12, 4, 14, 10);
            line(14, 10, 20, 10);
            line(20, 10, 15, 14);
            line(15, 14, 17, 20);
            line(17, 20, 12, 16);
            line(12, 16, 7, 20);
            line(7, 20, 9, 14);
            line(9, 14, 4, 10);
            line(4, 10, 10, 10);
            line(10, 10, 12, 4);
            break;
        case IconName::Eye:
            ring(12, 12, s * 0.38f, s * 0.22f);
            ring(12, 12, s * 0.12f, s * 0.12f);
            break;
        case IconName::Heart:
            ring(8.5f, 9, s * 0.16f, s * 0.16f);
            ring(15.5f, 9, s * 0.16f, s * 0.16f);
            line(5, 11, 12, 20);
            line(19, 11, 12, 20);
            break;
        case IconName::ArrowLeft:
            line(18, 12, 6, 12);
            line(10, 7, 6, 12);
            line(10, 17, 6, 12);
            break;
        case IconName::Building2:
            DrawRoundStroke(ctx, x + s * 0.18f, y + s * 0.18f, s * 0.38f,
                            s * 0.64f, 1, sw, c);
            DrawRoundStroke(ctx, x + s * 0.48f, y + s * 0.32f, s * 0.32f,
                            s * 0.50f, 1, sw, c);
            line(10, 22, 10, 18);
            line(8, 10, 10, 10);
            line(8, 14, 10, 14);
            line(16, 14, 18, 14);
            line(16, 18, 18, 18);
            break;
        case IconName::Asterisk:
            line(12, 5, 12, 19);
            line(6, 8, 18, 16);
            line(18, 8, 6, 16);
            break;
        case IconName::Sun:
            ring(12, 12, s * 0.16f, s * 0.16f);
            line(12, 3, 12, 6);
            line(12, 18, 12, 21);
            line(3, 12, 6, 12);
            line(18, 12, 21, 12);
            line(6, 6, 8, 8);
            line(16, 16, 18, 18);
            line(18, 6, 16, 8);
            line(8, 16, 6, 18);
            break;
        case IconName::Maximize:
            DrawRoundStroke(ctx, x + s * 0.22f, y + s * 0.22f, s * 0.56f,
                            s * 0.56f, 1, sw, c);
            break;
        default:
            break;
    }
}

static void DrawChart(PaintCtx* ctx, El* e) {
    const Theme& th = ThemeNow();
    float x = e->x;
    float y = e->y;
    float w = e->w;
    float h = e->h;
    const float axisGap = 18.f;
    float plotH = h - axisGap;
    if (plotH < 8 || w < 8) {
        return;
    }

    if (!e->chart.overlay) {
        const float kGridDash[2] = {4.f, 2.f};
        for (int i = 0; i <= 3; i++) {
            float gy = y + plotH * (i / 4.f);
            CanvasLine(ctx, x, gy, x + w, gy, 1.f, th.border, kGridDash);
        }
        DrawLine(ctx, x, y + plotH, x + w, y + plotH, 1.f, th.border);
    }

    int n = e->chart.n;
    const float* ys = e->chart.ys;
    if (!ys || n <= 0) {
        return;
    }

    auto Xat = [&](int i) -> float {
        if (n <= 1) {
            return x + w * 0.5f;
        }
        return x + (w * (float)i / (float)(n - 1));
    };
    auto Yat = [&](float v) -> float {
        if (v < 0) {
            v = 0;
        }
        if (v > 100) {
            v = 100;
        }
        return y + 10.f + (1.f - v / 100.f) * (plotH - 10.f);
    };

    Path* area = PathNew(ctx, true);
    if (area) {
        PathMoveTo(area, Xat(0), y + plotH);
        PathLineTo(area, Xat(0), Yat(ys[0]));
        for (int i = 1; i < n; i++) {
            PathLineTo(area, Xat(i), Yat(ys[i]));
        }
        PathLineTo(area, Xat(n - 1), y + plotH);
        PathClose(area);
        PathFillGradientV(ctx, area, y, y + plotH, e->chart.fillTop,
                          e->chart.fillBot);
        PathFree(area);
    }

    if (n == 1) {
        DrawLine(ctx, x, Yat(ys[0]), x + w, Yat(ys[0]), 2.f, e->chart.stroke);
    }
    for (int i = 1; i < n; i++) {
        DrawLine(ctx, Xat(i - 1), Yat(ys[i - 1]), Xat(i), Yat(ys[i]), 2.f,
                 e->chart.stroke);
    }

    int step = e->chart.tickMargin;
    if (step < 1) {
        step = 15;
    }
    if (e->chart.overlay) {
        return;
    }
    for (int i = 0; i < n; i += step) {
        if (e->chart.labels) {
            DrawTextAt(ctx, Str(e->chart.labels[i]), Xat(i) - 16, y + plotH + 2,
                       60, 16, 10, th.mutedFg, false);
        } else {
            DrawTextAt(ctx, fmt("%ds", i), Xat(i) - 16, y + plotH + 2, 60, 16,
                       10, th.mutedFg, false);
        }
    }
}

static void PaintElNode(PaintCtx* ctx, El* e, bool skipOverlay);

static bool IsOverlay(El* e) {
    return e->style.fixed || e->style.deferred;
}

static void PaintOverlays(PaintCtx* ctx, El* e) {
    if (!e) {
        return;
    }
    if (IsOverlay(e)) {
        PaintElNode(ctx, e, false);
        return;
    }
    for (El* c = e->first; c; c = c->next) {
        PaintOverlays(ctx, c);
    }
}

void PaintEl(PaintCtx* ctx, El* e) {
    PaintElNode(ctx, e, true);
    PaintOverlays(ctx, e);
}

static void PaintElNode(PaintCtx* ctx, El* e, bool skipOverlay) {
    if (!e || !ctx->rt) {
        return;
    }
    if (skipOverlay && IsOverlay(e)) {
        return;
    }

    if (e->sliderBounds) {
        SliderSetBounds(e->sliderBounds, e->Bounds());
    }
    if (e->clickId || e->onClick.IsValid() || e->listener.IsValid() ||
        e->onMouseDown.IsValid() || e->onMouseUp.IsValid() ||
        e->onDragMove.IsValid() || e->slider) {
        HitRect hr;
        hr.id = e->clickId;
        hr.bounds = e->Bounds();
        hr.onClick = e->onClick;
        hr.listener = e->listener;
        hr.onMouseDown = e->onMouseDown;
        hr.onMouseUp = e->onMouseUp;
        hr.onDragMove = e->onDragMove;
        hr.slider = e->slider;
        hr.sliderAxis = e->sliderAxis;
        ctx->hits.Append(hr);
    }
    if (e->style.overflowY == OverflowY::Scroll) {
        ScrollRect sr;
        sr.id = e->scrollId;
        sr.bounds = e->Bounds();
        sr.contentH = e->contentH;
        ctx->scrolls.Append(sr);
    }

    if (e->style.hasHoverBg && e->clickId && e->clickId == ctx->hoverId) {
        FillRound(ctx, e->x, e->y, e->w, e->h, e->style.radius,
                  e->style.hoverBg);
    } else if (e->style.hasBg) {
        FillRound(ctx, e->x, e->y, e->w, e->h, e->style.radius, e->style.bg);
    }
    if (e->style.border > 0) {
        if (e->style.borderDashed) {

            const float dash[2] = {e->style.dashOn, e->style.dashOff};
            float half = e->style.border * 0.5f;
            if (e->style.radius <= 0) {

                float l = EdgeLine(ctx, e->x + half);
                float r = EdgeLine(ctx, e->x + e->w - half);
                float t = EdgeLine(ctx, e->y + half);
                float b = EdgeLine(ctx, e->y + e->h - half);
                float x0 = EdgeEnd(ctx, e->x);
                float x1 = EdgeEnd(ctx, e->x + e->w);
                float y0 = EdgeEnd(ctx, e->y);
                float y1 = EdgeEnd(ctx, e->y + e->h);
                Rgba bc = e->style.borderColor;
                float bw = e->style.border;
                CanvasLine(ctx, x0, t, x1, t, bw, bc, dash);
                CanvasLine(ctx, x0, b, x1, b, bw, bc, dash);
                CanvasLine(ctx, l, y0, l, y1, bw, bc, dash);
                CanvasLine(ctx, r, y0, r, y1, bw, bc, dash);
            } else {
                CanvasStrokeRound(ctx, e->x, e->y, e->w, e->h, e->style.radius,
                                  e->style.border, e->style.borderColor, dash);
            }
        } else {
            DrawRoundStroke(ctx, e->x, e->y, e->w, e->h, e->style.radius,
                            e->style.border, e->style.borderColor);
        }
    }

    if (e->style.borderT > 0) {
        float y = EdgeLine(ctx, e->y + e->style.borderT * 0.5f);
        DrawLine(ctx, e->x, y, e->x + e->w, y, e->style.borderT,
                 e->style.borderColor);
    }
    if (e->style.borderB > 0) {
        float y = EdgeLine(ctx, e->y + e->h - e->style.borderB * 0.5f);
        DrawLine(ctx, e->x, y, e->x + e->w, y, e->style.borderB,
                 e->style.borderColor);
    }
    if (e->style.borderL > 0) {
        float x = EdgeLine(ctx, e->x + e->style.borderL * 0.5f);
        DrawLine(ctx, x, e->y, x, e->y + e->h, e->style.borderL,
                 e->style.borderColor);
    }
    if (e->style.borderR > 0) {
        float x = EdgeLine(ctx, e->x + e->w - e->style.borderR * 0.5f);
        DrawLine(ctx, x, e->y, x, e->y + e->h, e->style.borderR,
                 e->style.borderColor);
    }

    bool clip = e->style.overflowY != OverflowY::Visible;
    if (clip) {
        CanvasPushClip(ctx, e->x, e->y, e->w, e->h);
    }

    if (e->kind == ElKind::Text) {
        float font = e->laidFont > 0
                         ? e->laidFont
                         : (e->style.fontSize > 0 ? e->style.fontSize : 14.f);
        Rgba c = e->style.hasColor ? e->style.color : ThemeNow().foreground;
        int lo = e->selLo;
        int hi = e->selHi;
        if (e->selectable && e->text.s) {
            int docOff = ctx->textDocLen;
            TextHit th;
            th.bounds = e->Bounds();
            th.text = e->text;
            th.font = font;
            th.maxW = e->laidMaxW > 0 ? e->laidMaxW : e->w;
            th.wrap = e->style.wrap;
            th.docOff = docOff;
            ctx->texts.Append(th);
            ctx->textDocLen += e->text.len + 1;
            int a = ctx->selA;
            int b = ctx->selB;
            if (a >= 0 && b >= 0 && a != b) {
                if (a > b) {
                    int t = a;
                    a = b;
                    b = t;
                }
                int tlo = a > docOff ? a : docOff;
                int thi = b < docOff + e->text.len ? b : docOff + e->text.len;
                if (tlo < thi) {
                    lo = tlo - docOff;
                    hi = thi - docOff;
                }
            }
        }
        if (lo >= 0 && hi > lo) {
            PaintTextRange(ctx, e->text, font,
                           e->laidMaxW > 0 ? e->laidMaxW : e->w, e->style.wrap,
                           e->x, e->y, lo, hi, Rgba8(0x6b, 0xb3, 0xf0, 90));
        }
        if (e->laidLayout) {
            TextLayoutDraw(ctx, e->laidLayout, e->x, e->y, c,
                           e->style.truncate);
        } else {
            DrawTextAt(ctx, e->text, e->x, e->y, e->w, e->h, font, c,
                       e->style.truncate, e->style.wrap, e->laidMaxW,
                       ElTextWeight(e), e->style.lineHeight);
        }
    } else if (e->kind == ElKind::Icon) {
        Rgba c = e->style.hasColor ? e->style.color : ThemeNow().foreground;
        float s = e->w > 0 ? e->w : 16;
        Str path = e->iconPath.s ? e->iconPath : IconNamePath(e->icon);
        if (!SvgDraw(ctx, path, e->x, e->y, s, c)) {
            DrawIcon(ctx, e->icon, e->x, e->y, s, c);
        }
    } else if (e->kind == ElKind::Progress) {
        const Theme& th = ThemeNow();
        Rgba track = RgbaOpacity(th.progress, 0.2f);
        FillRound(ctx, e->x, e->y, e->w, e->h, e->style.radius, track);
        float fw = e->w * (e->progress / 100.f);
        if (fw > 0) {
            FillRound(ctx, e->x, e->y, fw, e->h, e->style.radius, th.progress);
        }
    } else if (e->kind == ElKind::Chart) {
        DrawChart(ctx, e);
    }
    if (e->customPaint) {
        e->customPaint(ctx, e, e->customUser);
    }

    for (El* c = e->first; c; c = c->next) {
        PaintElNode(ctx, c, skipOverlay);
    }

    if (clip) {
        CanvasPopClip(ctx);
    }

    if (e->style.overflowY == OverflowY::Scroll && e->contentH > e->h + 1.f &&
        e->h > 0) {
        float view = e->h;
        float content = e->contentH;
        float thumbH = view * (view / content);
        if (thumbH < 48.f) {
            thumbH = 48.f;
        }
        if (thumbH > view) {
            thumbH = view;
        }
        float maxScroll = content - view;
        if (maxScroll < 1.f) {
            maxScroll = 1.f;
        }
        float t = e->scrollY / maxScroll;
        if (t < 0) {
            t = 0;
        }
        if (t > 1) {
            t = 1;
        }
        float thumbW = 6.f;
        float thumbX = e->x + e->w - thumbW - 4.f;
        float thumbY = e->y + t * (view - thumbH);
        FillRound(ctx, thumbX, thumbY, thumbW, thumbH, 3.f,
                  ThemeNow().scrollbarThumb);
    }

    if (e->style.trapId && e->style.focusId &&
        e->style.focusId == ctx->focusId) {

        Bounds ring = e->Bounds().Inset(-2.f);
        DrawRoundStroke(ctx, ring.x, ring.y, ring.w, ring.h,
                        e->style.radius + 2, 2, ThemeNow().blue);
    }
    if (e->style.tooltip.s && e->clickId && e->clickId == ctx->hoverId) {
        const Theme& th = ThemeNow();
        Size tip = MeasureText(ctx, e->style.tooltip, 12, 280);

        Positioned at = PositionSide(e->Bounds(), {tip.w + 16, tip.h + 10},
                                     {ctx->viewW, ctx->viewH}, kPopupMargin,
                                     nullptr, PopupAlign::Center, 0);
        FillRound(ctx, at.bounds.x, at.bounds.y, at.bounds.w, at.bounds.h, 6,
                  th.foreground);
        DrawTextAt(ctx, e->style.tooltip, at.bounds.x + 8, at.bounds.y + 5,
                   tip.w + 4, tip.h, 12, th.background, false);
    }
}

const HitRect* HitTestRect(PaintCtx* ctx, float x, float y) {
    for (int i = ctx->hits.len - 1; i >= 0; i--) {
        const HitRect& h = ctx->hits[i];
        if (h.bounds.Contains({x, y})) {
            return &ctx->hits[i];
        }
    }
    return nullptr;
}

int HitTest(PaintCtx* ctx, float x, float y) {
    const HitRect* h = HitTestRect(ctx, x, y);
    return h ? h->id : 0;
}

const ScrollRect* HitScrollRect(PaintCtx* ctx, float x, float y) {
    for (int i = ctx->scrolls.len - 1; i >= 0; i--) {
        const ScrollRect& s = ctx->scrolls[i];
        if (s.bounds.Contains({x, y})) {
            return &ctx->scrolls[i];
        }
    }
    return nullptr;
}

static float DistToInterval(float v, float lo, float hi) {
    if (v < lo) {
        return lo - v;
    }
    if (v > hi) {
        return v - hi;
    }
    return 0.f;
}

static const TextHit* TextHitFind(PaintCtx* ctx, float x, float y, bool nearest,
                                  Point* outRel) {
    if (!ctx) {
        return nullptr;
    }
    const TextHit* best = nullptr;
    float bestScore = 1e9f;
    for (int i = ctx->texts.len - 1; i >= 0; i--) {
        const TextHit& h = ctx->texts[i];
        if (h.bounds.Contains({x, y})) {
            best = &h;
            nearest = false;
            break;
        }
        if (!nearest) {
            continue;
        }
        float dy = DistToInterval(y, h.bounds.y, h.bounds.Bottom());
        float dx = DistToInterval(x, h.bounds.x, h.bounds.Right());
        float score = dy * 1000.f + dx;
        if (score < bestScore) {
            bestScore = score;
            best = &h;
        }
    }
    if (!best || !best->text.s) {
        return nullptr;
    }
    Point rel = {x - best->bounds.x, y - best->bounds.y};
    if (nearest) {
        if (rel.x < 0) {
            rel.x = 0;
        }
        if (rel.y < 0) {
            rel.y = 0;
        }
        if (rel.x > best->bounds.w) {
            rel.x = best->bounds.w;
        }
        if (rel.y > best->bounds.h) {
            rel.y = best->bounds.h;
        }
    }
    *outRel = rel;
    return best;
}

static int TextHitLocal(PaintCtx* ctx, const TextHit* h, Point rel) {
    int local =
        TextIndexAt(ctx, h->text, h->font, h->maxW > 0 ? h->maxW : h->bounds.w,
                    h->wrap, rel.x, rel.y);
    if (local < 0) {
        local = 0;
    }
    if (local > h->text.len) {
        local = h->text.len;
    }
    return local;
}

int TextHitOffsetAt(PaintCtx* ctx, float x, float y, bool nearest) {
    Point rel = {};
    const TextHit* h = TextHitFind(ctx, x, y, nearest, &rel);
    if (!h) {
        return -1;
    }
    return h->docOff + TextHitLocal(ctx, h, rel);
}

enum class CharKind : uint8_t {
    Word,
    Whitespace,
    Newline,
    Other
};

static CharKind KindOf(uint32_t c) {
    bool word = c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||

                (c >= 0x00C0 && c <= 0x024F) || (c >= 0x0300 && c <= 0x036F) ||
                (c >= 0x0400 && c <= 0x04FF) || (c >= 0x1E00 && c <= 0x1EFF);
    if (word) {
        return CharKind::Word;
    }
    if (c == '\n' || c == '\r') {
        return CharKind::Newline;
    }

    bool space = c == ' ' || c == '\t' || c == 0x0B || c == 0x0C || c == 0x85 ||
                 c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
                 c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
                 c == 0x3000;
    return space ? CharKind::Whitespace : CharKind::Other;
}

static int Utf8At(Str s, int i, uint32_t* out) {
    const uint8_t* p = (const uint8_t*)s.s + i;
    uint8_t c = p[0];
    if (c < 0x80) {
        *out = c;
        return 1;
    }
    int n = (c & 0xE0) == 0xC0   ? 2
            : (c & 0xF0) == 0xE0 ? 3
            : (c & 0xF8) == 0xF0 ? 4
                                 : 1;
    if (n == 1 || i + n > s.len) {
        *out = c;
        return 1;
    }
    uint32_t cp = (uint32_t)(c & (0xFF >> (n + 1)));
    for (int k = 1; k < n; k++) {
        if ((p[k] & 0xC0) != 0x80) {
            *out = c;
            return 1;
        }
        cp = (cp << 6) | (uint32_t)(p[k] & 0x3F);
    }
    *out = cp;
    return n;
}

static int Utf8Prev(Str s, int i) {
    int j = i - 1;
    while (j > 0 && ((uint8_t)s.s[j] & 0xC0) == 0x80) {
        j--;
    }
    return j < 0 ? 0 : j;
}

static int Utf8ClipLeft(Str s, int off) {
    if (off > s.len) {
        off = s.len;
    }
    if (off < 0) {
        off = 0;
    }
    while (off > 0 && off < s.len && ((uint8_t)s.s[off] & 0xC0) == 0x80) {
        off--;
    }
    return off;
}

static const int kWordScanMax = 128;

bool TextWordRangeAt(Str s, int off, int* outA, int* outB) {
    if (!s.s || s.len <= 0) {
        return false;
    }
    off = Utf8ClipLeft(s, off);
    if (off >= s.len) {
        return false;
    }
    uint32_t c = 0;
    int clen = Utf8At(s, off, &c);
    CharKind kind = KindOf(c);
    bool joins = kind == CharKind::Word || kind == CharKind::Whitespace;
    int a = off;
    int b = off + clen;
    for (int i = 0; joins && a > 0 && i < kWordScanMax; i++) {
        int prev = Utf8Prev(s, a);
        uint32_t pc = 0;
        Utf8At(s, prev, &pc);
        if (KindOf(pc) != kind) {
            break;
        }
        a = prev;
    }
    for (int i = 0; joins && b < s.len && i < kWordScanMax; i++) {
        uint32_t nc = 0;
        int nlen = Utf8At(s, b, &nc);
        if (KindOf(nc) != kind) {
            break;
        }
        b += nlen;
    }
    *outA = a;
    *outB = b;
    return true;
}

void TextLineRangeAt(Str s, int off, int* outA, int* outB) {
    *outA = 0;
    *outB = 0;
    if (!s.s || s.len <= 0) {
        return;
    }
    off = Utf8ClipLeft(s, off);
    int a = 0;
    for (int i = off - 1; i >= 0; i--) {
        if (s.s[i] == '\n') {
            a = i + 1;
            break;
        }
    }
    int b = s.len;
    for (int i = off; i < s.len; i++) {
        if (s.s[i] == '\n') {
            b = i;
            break;
        }
    }
    *outA = a;
    *outB = b;
}

bool TextMultiClickRange(PaintCtx* ctx, float x, float y, int clickCount,
                         int* outA, int* outB) {
    if (clickCount < 2) {
        return false;
    }
    Point rel = {};
    const TextHit* h = TextHitFind(ctx, x, y, false, &rel);
    if (!h) {
        return false;
    }
    int local = TextHitLocal(ctx, h, rel);
    int a = 0;
    int b = 0;
    if (clickCount == 2) {
        if (!TextWordRangeAt(h->text, local, &a, &b)) {
            return false;
        }
    } else {
        TextLineRangeAt(h->text, local, &a, &b);
    }
    if (a >= b) {
        return false;
    }
    *outA = h->docOff + a;
    *outB = h->docOff + b;
    return true;
}

int CopyTextHits(PaintCtx* ctx, int a, int b, char* out, int cap) {
    if (!out || cap <= 0) {
        return 0;
    }
    out[0] = 0;
    if (!ctx || a < 0 || b < 0 || a == b) {
        return 0;
    }
    if (a > b) {
        int t = a;
        a = b;
        b = t;
    }
    int n = 0;
    for (int i = 0; i < ctx->texts.len && n < cap - 1; i++) {
        const TextHit& t = ctx->texts[i];
        int pos = t.docOff;
        int plen = t.text.len;
        int lo = a > pos ? a : pos;
        int hi = b < pos + plen ? b : pos + plen;
        if (lo < hi && t.text.s) {
            int take = hi - lo;
            if (n + take > cap - 1) {
                take = cap - 1 - n;
            }
            memcpy(out + n, t.text.s + (lo - pos), (size_t)take);
            n += take;
        }
        int gap = pos + plen;
        if (i + 1 < ctx->texts.len && a <= gap && b > gap && n < cap - 1) {
            out[n++] = '\n';
        }
    }
    out[n] = 0;
    return n;
}

static void CollectFocus(El* e, Window* win) {
    if (!e) {
        return;
    }
    if (e->style.focusId) {
        FocusRect fr;
        fr.id = e->style.focusId;
        fr.trapId = e->style.trapId;
        fr.bounds = e->Bounds();
        win->focusEls.Append(fr);
    }
    for (El* c = e->first; c; c = c->next) {
        CollectFocus(c, win);
    }
}

void FocusCollect(Window* win, El* root) {
    win->focusEls.Clear();
    CollectFocus(root, win);
}

int FocusNext(Window* win, int trapId, bool backward) {
    int n = win->focusEls.len;
    if (n == 0) {
        return 0;
    }
    int cur = -1;
    for (int i = 0; i < n; i++) {
        if (win->focusEls[i].id == win->focusId) {
            cur = i;
            break;
        }
    }
    int step = backward ? -1 : 1;
    int i = cur;
    for (int k = 0; k < n; k++) {
        i = (i + step + n) % n;
        if (trapId && win->focusEls[i].trapId != trapId) {
            continue;
        }
        if (!trapId && win->focusEls[i].trapId) {

            if (cur < 0 || win->focusEls[cur].trapId == 0) {
                continue;
            }
            if (win->focusEls[i].trapId != win->focusEls[cur].trapId) {
                continue;
            }
        }
        win->focusId = win->focusEls[i].id;
        return win->focusId;
    }
    return win->focusId;
}
}

#line 1 "src/gpui/Positioner.cpp"

namespace gpui {

static float MaxF(float a, float b) {
    return a > b ? a : b;
}

static Placement ResolvePlacement(Bounds trigger, Size popup, Size view,
                                  float margin, const Placement* preferred) {
    float rightLimit = MaxF(view.w - margin, margin);
    float bottomLimit = MaxF(view.h - margin, margin);
    float availLeft = MaxF(trigger.x - margin, 0.f);
    float availRight = MaxF(rightLimit - trigger.Right(), 0.f);
    float availAbove = MaxF(trigger.y - margin, 0.f);
    float availBelow = MaxF(bottomLimit - trigger.Bottom(), 0.f);

    if (preferred && *preferred == Placement::Right) {
        if (popup.w <= availRight) {
            return Placement::Right;
        }
        if (popup.w <= availLeft) {
            return Placement::Left;
        }
        return availRight >= availLeft ? Placement::Right : Placement::Left;
    }
    if (preferred && *preferred == Placement::Left) {
        if (popup.w <= availLeft) {
            return Placement::Left;
        }
        if (popup.w <= availRight) {
            return Placement::Right;
        }
        return availLeft >= availRight ? Placement::Left : Placement::Right;
    }
    if (preferred && *preferred == Placement::Bottom) {
        if (popup.h <= availBelow) {
            return Placement::Bottom;
        }
        if (popup.h <= availAbove) {
            return Placement::Top;
        }
        return availBelow >= availAbove ? Placement::Bottom : Placement::Top;
    }

    if (popup.h <= availAbove) {
        return Placement::Top;
    }
    if (popup.h <= availBelow) {
        return Placement::Bottom;
    }
    return availBelow >= availAbove ? Placement::Bottom : Placement::Top;
}

static Bounds SideOrigin(Bounds trigger, Size popup, Placement placement,
                         PopupAlign align, float offset) {
    float alignedX = trigger.x;
    float alignedY = trigger.y;
    if (align == PopupAlign::Center) {
        alignedX = trigger.CenterX() - popup.w * 0.5f;
        alignedY = trigger.CenterY() - popup.h * 0.5f;
    } else if (align == PopupAlign::End) {
        alignedX = trigger.Right() - popup.w;
        alignedY = trigger.Bottom() - popup.h;
    }

    Bounds b = {0, 0, popup.w, popup.h};
    switch (placement) {
        case Placement::Top:
            b.x = alignedX;
            b.y = trigger.y - popup.h - offset;
            break;
        case Placement::Bottom:
            b.x = alignedX;
            b.y = trigger.Bottom() + offset;
            break;
        case Placement::Left:
            b.x = trigger.x - popup.w - offset;
            b.y = alignedY;
            break;
        case Placement::Right:
            b.x = trigger.Right() + offset;
            b.y = alignedY;
            break;
    }
    return b;
}

static Bounds ClampToViewport(Bounds b, Size view, float margin) {
    float rightLimit = MaxF(view.w - margin, margin);
    float bottomLimit = MaxF(view.h - margin, margin);
    if (b.Right() > rightLimit) {
        b.x -= b.Right() - rightLimit;
    }
    if (b.x < margin) {
        b.x = margin;
    }
    if (b.Bottom() > bottomLimit) {
        b.y -= b.Bottom() - bottomLimit;
    }
    if (b.y < margin) {
        b.y = margin;
    }
    return b;
}

Positioned PositionSide(Bounds trigger, Size popup, Size view, float margin,
                        const Placement* preferred, PopupAlign align,
                        float offset) {
    Placement placement =
        ResolvePlacement(trigger, popup, view, margin, preferred);
    Bounds b = SideOrigin(trigger, popup, placement, align, offset);
    Positioned out;
    out.bounds = ClampToViewport(b, view, margin);
    out.placement = placement;
    out.hasPlacement = true;
    return out;
}

Positioned PositionCorner(Anchor anchor, Point at, Size popup, Size view,
                          float margin) {

    Bounds b = BoundsAt(at, popup);
    if (anchor == Anchor::TopRight || anchor == Anchor::BottomRight) {
        b.x = at.x - popup.w;
    }
    if (anchor == Anchor::BottomLeft || anchor == Anchor::BottomRight) {
        b.y = at.y - popup.h;
    }
    Positioned out;
    out.bounds = ClampToViewport(b, view, margin);
    out.hasPlacement = false;
    return out;
}

}

#line 1 "src/gpui/Svg.cpp"

namespace gpui {

static const int kMaxOps = 128;
static const int kMaxCache = 24;

enum SvgCmd : uint8_t {
    kMove = 0,
    kLine = 1,
    kCubic = 2,
    kClose = 3
};

struct SvgOp {
    uint8_t cmd = kMove;
    float x = 0, y = 0;
    float x1 = 0, y1 = 0;
    float x2 = 0, y2 = 0;
};

struct SvgIcon {
    float vbX = 0, vbY = 0, vbW = 24, vbH = 24;
    float strokeW = 2;

    bool filled = false;
    int nOps = 0;
    SvgOp ops[kMaxOps];
};

struct SvgCache {
    char path[128];
    SvgIcon icon;
    bool ok = false;
};

static SvgCache gCache[kMaxCache];
static int gCacheN = 0;

static void AddOp(SvgIcon* ic, SvgOp op) {
    if (ic->nOps < kMaxOps) {
        ic->ops[ic->nOps++] = op;
    }
}

static void AddMove(SvgIcon* ic, float x, float y) {
    SvgOp o;
    o.cmd = kMove;
    o.x = x;
    o.y = y;
    AddOp(ic, o);
}
static void AddLine(SvgIcon* ic, float x, float y) {
    SvgOp o;
    o.cmd = kLine;
    o.x = x;
    o.y = y;
    AddOp(ic, o);
}
static void AddCubic(SvgIcon* ic, float x1, float y1, float x2, float y2,
                     float x, float y) {
    SvgOp o;
    o.cmd = kCubic;
    o.x1 = x1;
    o.y1 = y1;
    o.x2 = x2;
    o.y2 = y2;
    o.x = x;
    o.y = y;
    AddOp(ic, o);
}
static void AddClose(SvgIcon* ic) {
    SvgOp o;
    o.cmd = kClose;
    AddOp(ic, o);
}

static void AddRoundRect(SvgIcon* ic, float x, float y, float w, float h,
                         float rx) {
    if (rx < 0) {
        rx = 0;
    }
    if (rx > w * 0.5f) {
        rx = w * 0.5f;
    }
    if (rx > h * 0.5f) {
        rx = h * 0.5f;
    }
    if (rx <= 0.01f) {
        AddMove(ic, x, y);
        AddLine(ic, x + w, y);
        AddLine(ic, x + w, y + h);
        AddLine(ic, x, y + h);
        AddClose(ic);
        return;
    }

    float k = rx * 0.55228475f;
    float x1 = x + rx, x2 = x + w - rx;
    float y1 = y + rx, y2 = y + h - rx;
    AddMove(ic, x1, y);
    AddLine(ic, x2, y);
    AddCubic(ic, x2 + k, y, x + w, y1 - k, x + w, y1);
    AddLine(ic, x + w, y2);
    AddCubic(ic, x + w, y2 + k, x2 + k, y + h, x2, y + h);
    AddLine(ic, x1, y + h);
    AddCubic(ic, x1 - k, y + h, x, y2 + k, x, y2);
    AddLine(ic, x, y1);
    AddCubic(ic, x, y1 - k, x1 - k, y, x1, y);
    AddClose(ic);
}

struct PathScan {
    const char* p;
    const char* end;
};

static void SkipWs(PathScan* s) {
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' ||
                             *s->p == '\r' || *s->p == ',')) {
        s->p++;
    }
}

static bool ParseNum(PathScan* s, float* out) {
    SkipWs(s);
    if (s->p >= s->end) {
        return false;
    }
    char* endp = nullptr;
    float v = strtof(s->p, &endp);
    if (endp == s->p) {
        return false;
    }
    *out = v;
    s->p = endp;
    return true;
}

static float Angle(float ux, float uy, float vx, float vy) {
    float dot = ux * vx + uy * vy;
    float nu = sqrtf(ux * ux + uy * uy);
    float nv = sqrtf(vx * vx + vy * vy);
    float c = (nu > 0 && nv > 0) ? dot / (nu * nv) : 1;
    if (c < -1) {
        c = -1;
    }
    if (c > 1) {
        c = 1;
    }
    float a = acosf(c);
    if (ux * vy - uy * vx < 0) {
        a = -a;
    }
    return a;
}

static void AddArc(SvgIcon* ic, float x1, float y1, float rx, float ry,
                   float phiDeg, bool large, bool sweep, float x2, float y2) {
    rx = fabsf(rx);
    ry = fabsf(ry);
    if (rx < 1e-6f || ry < 1e-6f) {
        AddLine(ic, x2, y2);
        return;
    }
    float phi = phiDeg * kPi / 180.f;
    float cosP = cosf(phi);
    float sinP = sinf(phi);
    float dx = (x1 - x2) * 0.5f;
    float dy = (y1 - y2) * 0.5f;
    float x1p = cosP * dx + sinP * dy;
    float y1p = -sinP * dx + cosP * dy;
    float rx2 = rx * rx, ry2 = ry * ry;
    float x1p2 = x1p * x1p, y1p2 = y1p * y1p;
    float lam = x1p2 / rx2 + y1p2 / ry2;
    if (lam > 1) {
        float sc = sqrtf(lam);
        rx *= sc;
        ry *= sc;
        rx2 = rx * rx;
        ry2 = ry * ry;
    }
    float num = rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2;
    float den = rx2 * y1p2 + ry2 * x1p2;
    float csq = (den > 0) ? num / den : 0;
    if (csq < 0) {
        csq = 0;
    }
    float c = sqrtf(csq);
    if (large == sweep) {
        c = -c;
    }
    float cxp = c * rx * y1p / ry;
    float cyp = c * -ry * x1p / rx;
    float cx = cosP * cxp - sinP * cyp + (x1 + x2) * 0.5f;
    float cy = sinP * cxp + cosP * cyp + (y1 + y2) * 0.5f;
    float theta1 = Angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
    float dtheta = Angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx,
                         (-y1p - cyp) / ry);
    if (!sweep && dtheta > 0) {
        dtheta -= 2 * kPi;
    }
    if (sweep && dtheta < 0) {
        dtheta += 2 * kPi;
    }
    int segs = (int)ceilf(fabsf(dtheta) / (kPi * 0.5f + 1e-6f));
    if (segs < 1) {
        segs = 1;
    }
    if (segs > 8) {
        segs = 8;
    }
    float dt = dtheta / (float)segs;
    for (int i = 0; i < segs; i++) {
        float t0 = theta1 + dt * (float)i;
        float t1 = t0 + dt;
        float e0x = rx * cosf(t0), e0y = ry * sinf(t0);
        float e1x = rx * cosf(t1), e1y = ry * sinf(t1);
        float q = tanf(dt * 0.5f);
        float alpha = sinf(dt) * (sqrtf(4 + 3 * q * q) - 1) / 3.f;
        float d0x = -rx * sinf(t0), d0y = ry * cosf(t0);
        float d1x = -rx * sinf(t1), d1y = ry * cosf(t1);
        float p0x = cx + cosP * e0x - sinP * e0y;
        float p0y = cy + sinP * e0x + cosP * e0y;
        (void)p0x;
        (void)p0y;
        float p1x = cx + cosP * e1x - sinP * e1y;
        float p1y = cy + sinP * e1x + cosP * e1y;
        float c1x =
            cx + cosP * (e0x + alpha * d0x) - sinP * (e0y + alpha * d0y);
        float c1y =
            cy + sinP * (e0x + alpha * d0x) + cosP * (e0y + alpha * d0y);
        float c2x =
            cx + cosP * (e1x - alpha * d1x) - sinP * (e1y - alpha * d1y);
        float c2y =
            cy + sinP * (e1x - alpha * d1x) + cosP * (e1y - alpha * d1y);
        AddCubic(ic, c1x, c1y, c2x, c2y, p1x, p1y);
    }
}

static void ParsePathD(SvgIcon* ic, Str d) {
    if (!d.s || d.len <= 0) {
        return;
    }
    PathScan s{d.s, d.s + d.len};
    char cmd = 0;
    float cx = 0, cy = 0, sx = 0, sy = 0;
    float pcx = 0, pcy = 0;
    bool hasPrevC = false;
    while (s.p < s.end) {
        SkipWs(&s);
        if (s.p >= s.end) {
            break;
        }
        char c = *s.p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            cmd = c;
            s.p++;
        } else if (!cmd) {
            s.p++;
            continue;
        }
        bool rel = cmd >= 'a';
        char op = rel ? (char)(cmd - 32) : cmd;
        if (op == 'Z') {
            AddClose(ic);
            cx = sx;
            cy = sy;
            hasPrevC = false;
            continue;
        }
        if (op == 'M') {
            float x, y;
            if (!ParseNum(&s, &x) || !ParseNum(&s, &y)) {
                break;
            }
            if (rel) {
                x += cx;
                y += cy;
            }
            AddMove(ic, x, y);
            cx = sx = x;
            cy = sy = y;
            hasPrevC = false;

            cmd = rel ? 'l' : 'L';
            continue;
        }
        if (op == 'L') {
            float x, y;
            if (!ParseNum(&s, &x) || !ParseNum(&s, &y)) {
                break;
            }
            if (rel) {
                x += cx;
                y += cy;
            }
            AddLine(ic, x, y);
            cx = x;
            cy = y;
            hasPrevC = false;
            continue;
        }
        if (op == 'H') {
            float x;
            if (!ParseNum(&s, &x)) {
                break;
            }
            if (rel) {
                x += cx;
            }
            AddLine(ic, x, cy);
            cx = x;
            hasPrevC = false;
            continue;
        }
        if (op == 'V') {
            float y;
            if (!ParseNum(&s, &y)) {
                break;
            }
            if (rel) {
                y += cy;
            }
            AddLine(ic, cx, y);
            cy = y;
            hasPrevC = false;
            continue;
        }
        if (op == 'C') {
            float x1, y1, x2, y2, x, y;
            if (!ParseNum(&s, &x1) || !ParseNum(&s, &y1) ||
                !ParseNum(&s, &x2) || !ParseNum(&s, &y2) || !ParseNum(&s, &x) ||
                !ParseNum(&s, &y)) {
                break;
            }
            if (rel) {
                x1 += cx;
                y1 += cy;
                x2 += cx;
                y2 += cy;
                x += cx;
                y += cy;
            }
            AddCubic(ic, x1, y1, x2, y2, x, y);
            pcx = x2;
            pcy = y2;
            hasPrevC = true;
            cx = x;
            cy = y;
            continue;
        }
        if (op == 'S') {
            float x2, y2, x, y;
            if (!ParseNum(&s, &x2) || !ParseNum(&s, &y2) || !ParseNum(&s, &x) ||
                !ParseNum(&s, &y)) {
                break;
            }
            if (rel) {
                x2 += cx;
                y2 += cy;
                x += cx;
                y += cy;
            }
            float x1 = hasPrevC ? (2 * cx - pcx) : cx;
            float y1 = hasPrevC ? (2 * cy - pcy) : cy;
            AddCubic(ic, x1, y1, x2, y2, x, y);
            pcx = x2;
            pcy = y2;
            hasPrevC = true;
            cx = x;
            cy = y;
            continue;
        }
        if (op == 'Q') {
            float x1, y1, x, y;
            if (!ParseNum(&s, &x1) || !ParseNum(&s, &y1) || !ParseNum(&s, &x) ||
                !ParseNum(&s, &y)) {
                break;
            }
            if (rel) {
                x1 += cx;
                y1 += cy;
                x += cx;
                y += cy;
            }

            float c1x = cx + 2.f / 3.f * (x1 - cx);
            float c1y = cy + 2.f / 3.f * (y1 - cy);
            float c2x = x + 2.f / 3.f * (x1 - x);
            float c2y = y + 2.f / 3.f * (y1 - y);
            AddCubic(ic, c1x, c1y, c2x, c2y, x, y);
            pcx = x1;
            pcy = y1;
            hasPrevC = true;
            cx = x;
            cy = y;
            continue;
        }
        if (op == 'T') {
            float x, y;
            if (!ParseNum(&s, &x) || !ParseNum(&s, &y)) {
                break;
            }
            if (rel) {
                x += cx;
                y += cy;
            }
            float x1 = hasPrevC ? (2 * cx - pcx) : cx;
            float y1 = hasPrevC ? (2 * cy - pcy) : cy;
            float c1x = cx + 2.f / 3.f * (x1 - cx);
            float c1y = cy + 2.f / 3.f * (y1 - cy);
            float c2x = x + 2.f / 3.f * (x1 - x);
            float c2y = y + 2.f / 3.f * (y1 - y);
            AddCubic(ic, c1x, c1y, c2x, c2y, x, y);
            pcx = x1;
            pcy = y1;
            hasPrevC = true;
            cx = x;
            cy = y;
            continue;
        }
        if (op == 'A') {
            float rx, ry, rot, x, y;
            float fA, fS;
            if (!ParseNum(&s, &rx) || !ParseNum(&s, &ry) ||
                !ParseNum(&s, &rot) || !ParseNum(&s, &fA) ||
                !ParseNum(&s, &fS) || !ParseNum(&s, &x) || !ParseNum(&s, &y)) {
                break;
            }
            if (rel) {
                x += cx;
                y += cy;
            }
            AddArc(ic, cx, cy, rx, ry, rot, fA != 0, fS != 0, x, y);
            cx = x;
            cy = y;
            hasPrevC = false;
            continue;
        }

        s.p++;
    }
}

static void ParsePolyline(SvgIcon* ic, Str pts, bool close) {
    PathScan s{pts.s, pts.s + pts.len};
    bool first = true;
    float x, y;
    while (ParseNum(&s, &x) && ParseNum(&s, &y)) {
        if (first) {
            AddMove(ic, x, y);
            first = false;
        } else {
            AddLine(ic, x, y);
        }
    }
    if (close && !first) {
        AddClose(ic);
    }
}

static bool StartsWithI(const char* p, const char* end, const char* lit) {
    int n = (int)strlen(lit);
    if (p + n > end) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        char a = p[i], b = lit[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + 32);
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool gpui_Svg_IsIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static bool GetAttr(Str tag, const char* name, char* out, int outN) {
    int nlen = (int)strlen(name);
    const char* p = tag.s;
    const char* end = tag.s + tag.len;
    while (p + nlen + 2 < end) {
        bool bound = (p == tag.s) || !gpui_Svg_IsIdentChar(p[-1]);
        if (bound && StrCmpNI(p, name, nlen) == 0 && p[nlen] == '=') {
            p += nlen + 1;
            char q = 0;
            if (*p == '"' || *p == '\'') {
                q = *p++;
            }
            int i = 0;
            while (p < end && *p != q && *p != '>' && i < outN - 1) {
                out[i++] = *p++;
            }
            out[i] = 0;
            return i > 0;
        }
        p++;
    }
    return false;
}

static float AttrF(Str tag, const char* name, float def) {
    char buf[64];
    if (!GetAttr(tag, name, buf, 64)) {
        return def;
    }
    return (float)atof(buf);
}

static void ParseSvg(Str xml, SvgIcon* ic) {
    *ic = SvgIcon{};
    ic->vbW = 24;
    ic->vbH = 24;
    ic->strokeW = 2;
    ic->filled = false;
    if (!xml.s || xml.len <= 0) {
        return;
    }
    const char* p = xml.s;
    const char* end = xml.s + xml.len;
    while (p < end) {
        if (*p != '<') {
            p++;
            continue;
        }
        p++;
        if (p < end && *p == '/') {
            while (p < end && *p != '>') {
                p++;
            }
            if (p < end) {
                p++;
            }
            continue;
        }
        if (p < end && *p == '!') {

            while (p + 2 < end &&
                   !(p[0] == '-' && p[1] == '-' && p[2] == '>')) {
                p++;
            }
            p += 3;
            continue;
        }
        const char* tagStart = p;
        while (p < end && *p != '>') {
            p++;
        }
        if (p >= end) {
            break;
        }
        Str tag(tagStart, (int)(p - tagStart));
        p++;

        if (StartsWithI(tagStart, end, "svg")) {
            char vb[64];
            if (GetAttr(tag, "viewBox", vb, 64)) {
                PathScan s{vb, vb + strlen(vb)};
                float a = 0, b = 0, c = 24, d = 24;
                ParseNum(&s, &a);
                ParseNum(&s, &b);
                ParseNum(&s, &c);
                ParseNum(&s, &d);
                ic->vbX = a;
                ic->vbY = b;
                ic->vbW = c > 0 ? c : 24;
                ic->vbH = d > 0 ? d : 24;
            }
            float sw = AttrF(tag, "stroke-width", 0);
            if (sw > 0) {
                ic->strokeW = sw;
            }
            char fill[64];
            if (GetAttr(tag, "fill", fill, 64)) {
                ic->filled = !StrEqI(Str(fill), StrL("none"));
            }
            continue;
        }
        if (StartsWithI(tagStart, end, "path")) {
            char d[2048];
            if (GetAttr(tag, "d", d, 2048)) {
                ParsePathD(ic, Str(d));
            }
            continue;
        }
        if (StartsWithI(tagStart, end, "rect")) {
            float x = AttrF(tag, "x", 0);
            float y = AttrF(tag, "y", 0);
            float w = AttrF(tag, "width", 0);
            float h = AttrF(tag, "height", 0);
            float rx = AttrF(tag, "rx", 0);
            AddRoundRect(ic, x, y, w, h, rx);
            continue;
        }
        if (StartsWithI(tagStart, end, "polyline")) {
            char pts[1024];
            if (GetAttr(tag, "points", pts, 1024)) {
                ParsePolyline(ic, Str(pts), false);
            }
            continue;
        }
        if (StartsWithI(tagStart, end, "polygon")) {
            char pts[1024];
            if (GetAttr(tag, "points", pts, 1024)) {
                ParsePolyline(ic, Str(pts), true);
            }
            continue;
        }
        if (StartsWithI(tagStart, end, "line")) {
            float x1 = AttrF(tag, "x1", 0);
            float y1 = AttrF(tag, "y1", 0);
            float x2 = AttrF(tag, "x2", 0);
            float y2 = AttrF(tag, "y2", 0);
            AddMove(ic, x1, y1);
            AddLine(ic, x2, y2);
            continue;
        }
        if (StartsWithI(tagStart, end, "circle")) {
            float cx = AttrF(tag, "cx", 0);
            float cy = AttrF(tag, "cy", 0);
            float r = AttrF(tag, "r", 0);
            AddRoundRect(ic, cx - r, cy - r, r * 2, r * 2, r);
            continue;
        }
    }
}

static const SvgIcon* GetIcon(Str assetPath) {
    if (!assetPath.s || assetPath.len <= 0) {
        return nullptr;
    }
    for (int i = 0; i < gCacheN; i++) {
        if (gCache[i].ok &&
            StrCmpNI(gCache[i].path, assetPath.s, assetPath.len) == 0 &&
            gCache[i].path[assetPath.len] == 0) {
            return &gCache[i].icon;
        }
    }
    TempStr xml = AssetsLoadTextTemp(assetPath);
    if (!xml.s) {
        return nullptr;
    }
    if (gCacheN >= kMaxCache) {
        gCacheN = 0;
    }
    SvgCache* e = &gCache[gCacheN++];
    int n = assetPath.len < 127 ? assetPath.len : 127;
    memcpy(e->path, assetPath.s, (size_t)n);
    e->path[n] = 0;
    ParseSvg(xml, &e->icon);
    e->ok = e->icon.nOps > 0;
    return e->ok ? &e->icon : nullptr;
}

bool SvgDraw(PaintCtx* ctx, Str assetPath, float x, float y, float size,
             Rgba color) {
    if (!ctx || !ctx->rt || size <= 0) {
        return false;
    }
    const SvgIcon* ic = GetIcon(assetPath);
    if (!ic) {
        return false;
    }

    float sx = size / (ic->vbW > 0 ? ic->vbW : 24.f);
    float sy = size / (ic->vbH > 0 ? ic->vbH : 24.f);
    auto TX = [&](float u) { return x + (u - ic->vbX) * sx; };
    auto TY = [&](float v) { return y + (v - ic->vbY) * sy; };

    Path* path = PathNew(ctx, true);
    if (!path) {
        return false;
    }
    for (int i = 0; i < ic->nOps; i++) {
        const SvgOp& o = ic->ops[i];
        if (o.cmd == kMove) {
            PathMoveTo(path, TX(o.x), TY(o.y));
        } else if (o.cmd == kLine) {
            PathLineTo(path, TX(o.x), TY(o.y));
        } else if (o.cmd == kCubic) {
            PathCubicTo(path, TX(o.x1), TY(o.y1), TX(o.x2), TY(o.y2), TX(o.x),
                        TY(o.y));
        } else if (o.cmd == kClose) {
            PathClose(path);
        }
    }

    if (ic->filled) {
        PathFill(ctx, path, color);
    }

    float strokeScale = (sx + sy) * 0.5f;
    PathStroke(ctx, path, (ic->strokeW > 0 ? ic->strokeW : 2.f) * strokeScale,
               color, true);
    PathFree(path);
    return true;
}

Str IconNamePath(IconName name) {
    switch (name) {
        case IconName::ArrowLeft:
            return StrL("icons/arrow-left.svg");
        case IconName::Asterisk:
            return StrL("icons/asterisk.svg");
        case IconName::Bell:
            return StrL("icons/bell.svg");
        case IconName::Building2:
            return StrL("icons/building-2.svg");
        case IconName::Eye:
            return StrL("icons/eye.svg");
        case IconName::Heart:
            return StrL("icons/heart.svg");
        case IconName::HeartOff:
            return StrL("icons/heart-off.svg");
        case IconName::Maximize:
            return StrL("icons/maximize.svg");
        case IconName::Minimize:
            return StrL("icons/minimize.svg");
        case IconName::Star:
            return StrL("icons/star.svg");
        case IconName::StarFill:
            return StrL("icons/star-fill.svg");
        case IconName::Sun:
            return StrL("icons/sun.svg");
        case IconName::Map:
            return StrL("icons/map.svg");
        case IconName::Globe:
            return StrL("icons/globe.svg");
        case IconName::Github:
            return StrL("icons/github.svg");
        case IconName::Inbox:
            return StrL("icons/inbox.svg");
        case IconName::Bot:
            return StrL("icons/bot.svg");
        case IconName::Cpu:
            return StrL("icons/cpu.svg");
        case IconName::MemoryStick:
            return StrL("icons/memory-stick.svg");
        case IconName::HardDrive:
            return StrL("icons/hard-drive.svg");
        case IconName::Battery:
            return StrL("icons/battery.svg");
        case IconName::BatteryCharging:
            return StrL("icons/battery-charging.svg");
        case IconName::BatteryMedium:
            return StrL("icons/battery-medium.svg");
        case IconName::BatteryFull:
            return StrL("icons/battery-full.svg");
        case IconName::WindowMinimize:
            return StrL("icons/window-minimize.svg");
        case IconName::WindowMaximize:
            return StrL("icons/window-maximize.svg");
        case IconName::WindowRestore:
            return StrL("icons/window-restore.svg");
        case IconName::WindowClose:
            return StrL("icons/window-close.svg");
        case IconName::LayoutDashboard:
            return StrL("icons/layout-dashboard.svg");
        case IconName::Calendar:
            return StrL("icons/calendar.svg");
        case IconName::Folder:
            return StrL("icons/folder.svg");
        case IconName::Settings:
            return StrL("icons/settings.svg");
        case IconName::GalleryVerticalEnd:
            return StrL("icons/gallery-vertical-end.svg");
        case IconName::CircleUser:
            return StrL("icons/circle-user.svg");
        case IconName::User:
            return StrL("icons/user.svg");
        case IconName::PanelLeft:
            return StrL("icons/panel-left.svg");
        case IconName::Info:
            return StrL("icons/info.svg");
        case IconName::X:
            return StrL("icons/x.svg");
        case IconName::CircleCheck:
            return StrL("icons/circle-check.svg");
        case IconName::TriangleAlert:
            return StrL("icons/triangle-alert.svg");
        case IconName::CircleX:
            return StrL("icons/circle-x.svg");
        case IconName::Loader:
            return StrL("icons/loader.svg");
        case IconName::LoaderCircle:
            return StrL("icons/loader-circle.svg");
        case IconName::Ellipsis:
            return StrL("icons/ellipsis.svg");
        case IconName::ChevronsUpDown:
            return StrL("icons/chevrons-up-down.svg");
        case IconName::SquareTerminal:
            return StrL("icons/square-terminal.svg");
        case IconName::BookOpen:
            return StrL("icons/book-open.svg");
        case IconName::Settings2:
            return StrL("icons/settings-2.svg");
        case IconName::Frame:
            return StrL("icons/frame.svg");
        case IconName::ChartPie:
            return StrL("icons/chart-pie.svg");
        case IconName::File:
            return StrL("icons/file.svg");
        case IconName::FolderOpen:
            return StrL("icons/folder-open.svg");
        case IconName::ChevronDown:
            return StrL("icons/chevron-down.svg");
        case IconName::ChevronLeft:
            return StrL("icons/chevron-left.svg");
        case IconName::ChevronRight:
            return StrL("icons/chevron-right.svg");
        case IconName::ChevronUp:
            return StrL("icons/chevron-up.svg");
        case IconName::Check:
            return StrL("icons/check.svg");
        case IconName::Search:
            return StrL("icons/search.svg");
        case IconName::Minus:
            return StrL("icons/minus.svg");
        case IconName::Plus:
            return StrL("icons/plus.svg");
        case IconName::Copy:
            return StrL("icons/copy.svg");
        default:
            return {};
    }
}
}

#line 1 "src/gpui/WindowCommon.cpp"

namespace gpui {

int WindowCollectFrames(Window* win, uint64_t* cursor, FrameTiming* out,
                        int max) {
    if (!win || !cursor || !out || max <= 0) {
        return 0;
    }
    uint64_t from = *cursor;

    if (win->frameSeq > (uint64_t)kFrameTraceCap &&
        from < win->frameSeq - (uint64_t)kFrameTraceCap) {
        from = win->frameSeq - (uint64_t)kFrameTraceCap;
    }
    if (from + (uint64_t)max < win->frameSeq) {
        from = win->frameSeq - (uint64_t)max;
    }
    int n = 0;
    for (uint64_t i = from; i < win->frameSeq; i++) {
        out[n++] = win->frameTrace[i % (uint64_t)kFrameTraceCap];
    }
    *cursor = win->frameSeq;
    return n;
}

void WindowDrawFrame(Window* win, void* native, int pxW, int pxH, float dipW,
                     float dipH) {
    if (!win) {
        return;
    }
    double drawStart = TimeNow();
    if (!PaintTargetBegin(&win->paint, native, pxW, pxH)) {
        return;
    }

    if (win->frameArena) {
        win->frameArena->Reset();
    } else {
        win->frameArena = ArenaNew();
    }
    ResetTempArena();
    win->paint.hits.Clear();
    win->paint.scrolls.Clear();
    win->paint.texts.Clear();
    win->paint.textDocLen = 0;
    win->paint.selA = -1;
    win->paint.selB = -1;
    win->paint.hoverId = win->hoverId;
    win->paint.focusId = win->focusId;
    win->paint.viewW = dipW;
    win->paint.viewH = dipH;
    TextMeasBeginFrame(&win->paint);

    El* root = EntityRender(win->app, win, win->frameArena, win->root);

    if (win->input != win->prevInput) {
        if (win->prevInput) {
            BlinkStop(win->app, win, &win->prevInput->blink);
        }
        if (win->input) {
            BlinkStart(win->app, win, &win->input->blink);
        }
        win->prevInput = win->input;
    }

    const Theme& th = ThemeNow();
    CanvasClear(&win->paint, th.background);
    if (root) {
        LayoutEl(&win->paint, root, 0, 0, dipW, dipH, 16.f, th.foreground);
        FocusCollect(win, root);
        PaintEl(&win->paint, root);
    }

    PaintTargetEnd(&win->paint);
    TextMeasEndFrame(&win->paint);

    FrameTiming timing;
    timing.drawSecs = (float)(TimeNow() - drawStart);
    win->frameTrace[win->frameSeq % (uint64_t)kFrameTraceCap] = timing;
    win->frameSeq++;
}

static const HitRect* HitRectById(Window* win, int id) {
    if (!win || !id) {
        return nullptr;
    }
    for (int i = win->paint.hits.len - 1; i >= 0; i--) {
        if (win->paint.hits[i].id == id) {
            return &win->paint.hits[i];
        }
    }
    return nullptr;
}

void WindowKeyDown(Window* win, int key, bool shift, bool ctrl, bool alt) {
    if (!win) {
        return;
    }
    if (key == KeyTab) {
        int trap = 0;
        for (int i = 0; i < win->focusEls.len; i++) {
            if (win->focusEls[i].id == win->focusId) {
                trap = win->focusEls[i].trapId;
                break;
            }
        }
        FocusNext(win, trap, shift);
        AppInvalidate(win);
        return;
    }

    if (win->input && (key == KeyLeft || key == KeyRight || key == KeyUp ||
                       key == KeyDown || key == KeyHome || key == KeyEnd ||
                       key == KeyBack || key == KeyDelete)) {
        BlinkPause(win->app, win, &win->input->blink);
    }
    if (win->onKey.IsValid()) {
        KeyEvent ev = {};
        ev.vk = key;
        ev.down = true;
        ev.shift = shift;
        ev.ctrl = ctrl;
        ev.alt = alt;
        ListenerCall(win->app, win, win->onKey, &ev);
    }

    if (key == KeyReturn && win->focusId && !win->eatReturn) {
        const HitRect* focused = HitRectById(win, win->focusId);

        ClickEvent ev = {0, 0, MouseButton::Left, win->focusId};
        ev.keyboard = true;
        if (focused) {
            ev.x = focused->bounds.CenterX();
            ev.y = focused->bounds.CenterY();
            ev.el = focused->bounds;
        }
        if (focused && focused->listener.IsValid()) {
            ListenerCall(win->app, win, focused->listener, &ev);
        } else if (win->onClick.IsValid()) {
            ListenerCall(win->app, win, win->onClick, &ev);
        }
    }
    win->eatReturn = false;
    AppInvalidate(win);
}

void WindowChar(Window* win, uint32_t ch, bool ctrl, bool alt) {
    if (!win) {
        return;
    }
    if (win->onKey.IsValid() && ch >= 32) {
        KeyEvent ev = {};
        ev.ch = ch;
        ev.down = true;
        ev.ctrl = ctrl;
        ev.alt = alt;
        ListenerCall(win->app, win, win->onKey, &ev);
    }
    if (win->input && win->input->focused) {
        LineInput* in = win->input;
        bool changed = false;
        if (ch == 8) {
            if (in->len > 0) {
                in->len--;
                in->buf[in->len] = 0;
                in->cursor = in->len;
                changed = true;
            }
        } else if (ch >= 32 && ch < 127 && in->len < 511) {
            in->buf[in->len++] = (char)ch;
            in->buf[in->len] = 0;
            in->cursor = in->len;
            changed = true;
        }
        if (changed) {

            BlinkPause(win->app, win, &in->blink);
        }

        if (changed && in->onChange.IsValid()) {
            InputEvent ev = {InputEventKind::Change};
            ListenerCall(win->app, win, in->onChange, &ev);
        }
    }
    AppInvalidate(win);
}

static void SliderEmit(Window* win, SliderState* s, SliderEventKind kind) {
    if (!s->onChange.IsValid()) {
        return;
    }
    SliderEvent ev = {kind, s->value};
    ListenerCall(win->app, win, s->onChange, &ev);
}

static void SliderPress(Window* win, const HitRect* hit, Point at) {
    SliderState* s = hit->slider;

    if (s->bounds.w <= 0 || s->bounds.h <= 0) {
        SliderSetBounds(s, hit->bounds);
    }
    s->dragStart = SliderIsStartAt(s, hit->sliderAxis, at);
    if (SliderUpdateByPosition(s, hit->sliderAxis, at, s->dragStart)) {
        SliderEmit(win, s, SliderEventKind::Change);
    }
    AppInvalidate(win);
}

static void SliderDrag(Window* win, const HitRect* hit, Point at) {
    SliderState* s = hit->slider;
    if (SliderUpdateByPosition(s, hit->sliderAxis, at, s->dragStart)) {
        SliderEmit(win, s, SliderEventKind::Change);
        AppInvalidate(win);
    }
}

static void SliderRelease(Window* win) {
    for (int i = 0; i < win->paint.hits.len; i++) {
        SliderState* s = win->paint.hits[i].slider;
        if (s && SliderHandleRelease(s)) {
            SliderEmit(win, s, SliderEventKind::Release);
            AppInvalidate(win);
        }
    }
}

static void DispatchMouseMove(Window* win, const MouseMoveEvent& in) {
    float x = in.x;
    float y = in.y;
    win->mouseX = x;
    win->mouseY = y;

    CursorKind want = TextHitOffsetAt(&win->paint, x, y, false) >= 0
                          ? CursorKind::IBeam
                          : CursorKind::Arrow;
    if (want != win->cursor) {
        win->cursor = want;
        PlatSetCursor(win, want);
    }
    int id = HitTest(&win->paint, x, y);
    if (id != win->hoverId) {
        win->hoverId = id;
        AppInvalidate(win);
    }
    if (win->onMouseMove.IsValid()) {
        ListenerCall(win->app, win, win->onMouseMove, &in);
    }

    const HitRect* pressed = HitRectById(win, win->pressedId);
    if (pressed && pressed->onDragMove.IsValid()) {
        ListenerCall(win->app, win, pressed->onDragMove, &in);
    }
    if (pressed && pressed->slider) {
        SliderDrag(win, pressed, {x, y});
    }
    if (win->mouseDown) {
        AppInvalidate(win);
    }
}

static const float kClickSlop = 4;

static const float kCaptionH = 34;

int WindowClickCount(Window* win, float x, float y, MouseButton button) {
    if (!win) {
        return 1;
    }
    double now = TimeNow();
    float dx = x - win->lastDownX;
    float dy = y - win->lastDownY;
    bool sameRun = win->clickRun > 0 && button == win->lastDownButton &&
                   now - win->lastDownAt <= PlatDoubleClickMs() / 1000.0 &&
                   dx * dx + dy * dy <= kClickSlop * kClickSlop;
    win->clickRun = sameRun ? win->clickRun + 1 : 1;
    win->lastDownAt = now;
    win->lastDownX = x;
    win->lastDownY = y;
    win->lastDownButton = button;
    return win->clickRun;
}

int WindowCurrentClickCount(Window* win) {
    return win && win->clickRun > 0 ? win->clickRun : 1;
}

static void DispatchMouseDown(Window* win, const MouseDownEvent& in) {
    float x = in.x;
    float y = in.y;
    if (win->onMouseDown.IsValid()) {
        ListenerCall(win->app, win, win->onMouseDown, &in);
    }

    if (!in.IsFocusing()) {
        AppInvalidate(win);
        return;
    }
    const HitRect* hit = HitTestRect(&win->paint, x, y);
    int id = hit ? hit->id : 0;
    win->mouseDown = true;
    win->pressedId = id;
    if (id) {
        win->focusId = id;
    }

    if (hit && hit->onMouseDown.IsValid()) {
        ListenerCall(win->app, win, hit->onMouseDown, &in);
    }
    if (hit && hit->slider) {
        SliderPress(win, hit, {x, y});
    }
    ClickEvent ev = {x, y, in.button, id};
    ev.clickCount = in.clickCount;
    ev.modifiers = in.modifiers;
    if (hit) {
        ev.el = hit->bounds;
    }
    if (hit && hit->listener.IsValid()) {
        ListenerCall(win->app, win, hit->listener, &ev);
    } else if (win->onClick.IsValid() && !(hit && hit->slider)) {

        ListenerCall(win->app, win, win->onClick, &ev);
    }
    if (hit && hit->onClick.IsValid()) {
        hit->onClick.Call();
    }

    bool caption = id == ClickWinCaption ||
                   (id == 0 && win->opts.clientTitleBar && y < kCaptionH);
    if (in.clickCount == 2 && caption) {
        AppToggleMaximize(win);
    }
    AppInvalidate(win);
}

static void DispatchMouseUp(Window* win, const MouseUpEvent& in) {
    win->mouseDown = false;
    if (win->onMouseUp.IsValid()) {
        ListenerCall(win->app, win, win->onMouseUp, &in);
    }

    const HitRect* hit = HitTestRect(&win->paint, in.x, in.y);
    if (hit && hit->onMouseUp.IsValid()) {
        ListenerCall(win->app, win, hit->onMouseUp, &in);
    }
    SliderRelease(win);
    win->pressedId = 0;
    AppInvalidate(win);
}

static void DispatchMouseExited(Window* win, const MouseExitEvent& in) {
    win->hoverId = 0;
    if (win->onMouseExit.IsValid()) {
        ListenerCall(win->app, win, win->onMouseExit, &in);
    }
    AppInvalidate(win);
}

static void DispatchScrollWheel(Window* win, const ScrollWheelEvent& in) {
    if (win->onScrollWheel.IsValid()) {
        ListenerCall(win->app, win, win->onScrollWheel, &in);
    }
    AppInvalidate(win);
}

void WindowDispatchInput(Window* win, const PlatformInput* input) {
    if (!win || !input) {
        return;
    }
    switch (input->kind) {
        case PlatformInputKind::MouseDown:
            DispatchMouseDown(win, input->mouseDown);
            break;
        case PlatformInputKind::MouseUp:
            DispatchMouseUp(win, input->mouseUp);
            break;
        case PlatformInputKind::MouseMove:
            DispatchMouseMove(win, input->mouseMove);
            break;
        case PlatformInputKind::MouseExited:
            DispatchMouseExited(win, input->mouseExited);
            break;
        case PlatformInputKind::ScrollWheel:
            DispatchScrollWheel(win, input->scrollWheel);
            break;
    }
}

PlatformInput InputMouseDown(MouseButton button, float x, float y,
                             Modifiers modifiers, int clickCount,
                             bool firstMouse) {
    PlatformInput in = {};
    in.kind = PlatformInputKind::MouseDown;
    in.mouseDown.button = button;
    in.mouseDown.x = x;
    in.mouseDown.y = y;
    in.mouseDown.modifiers = modifiers;
    in.mouseDown.clickCount = clickCount;
    in.mouseDown.firstMouse = firstMouse;
    return in;
}

PlatformInput InputMouseUp(MouseButton button, float x, float y,
                           Modifiers modifiers, int clickCount) {
    PlatformInput in = {};
    in.kind = PlatformInputKind::MouseUp;
    in.mouseUp.button = button;
    in.mouseUp.x = x;
    in.mouseUp.y = y;
    in.mouseUp.modifiers = modifiers;
    in.mouseUp.clickCount = clickCount;
    return in;
}

PlatformInput InputMouseMove(float x, float y, bool pressed,
                             MouseButton pressedButton, Modifiers modifiers) {
    PlatformInput in = {};
    in.kind = PlatformInputKind::MouseMove;
    in.mouseMove.x = x;
    in.mouseMove.y = y;
    in.mouseMove.pressed = pressed;
    in.mouseMove.pressedButton = pressedButton;
    in.mouseMove.modifiers = modifiers;
    return in;
}

PlatformInput InputMouseExited(float x, float y, bool pressed,
                               MouseButton pressedButton, Modifiers modifiers) {
    PlatformInput in = {};
    in.kind = PlatformInputKind::MouseExited;
    in.mouseExited.x = x;
    in.mouseExited.y = y;
    in.mouseExited.pressed = pressed;
    in.mouseExited.pressedButton = pressedButton;
    in.mouseExited.modifiers = modifiers;
    return in;
}

PlatformInput InputScrollWheel(float x, float y, float deltaX, float deltaY,
                               bool precise, Modifiers modifiers,
                               TouchPhase phase) {
    PlatformInput in = {};
    in.kind = PlatformInputKind::ScrollWheel;
    in.scrollWheel.x = x;
    in.scrollWheel.y = y;
    in.scrollWheel.deltaX = deltaX;
    in.scrollWheel.deltaY = deltaY;
    in.scrollWheel.precise = precise;
    in.scrollWheel.modifiers = modifiers;
    in.scrollWheel.phase = phase;
    return in;
}

static const int kBlinkIntervalMs = 500;
static const int kBlinkPauseMs = 300;

static BlinkCursor* BlinkGet(App* app, EntityId handle) {
    return (BlinkCursor*)EntityGet(app, handle);
}

void BlinkCursor::OnFlip(BlinkCursor* self, Ctx* cx, const TickEvent*) {
    if (self->paused) {
        return;
    }
    self->visible = !self->visible;
    Notify(cx);
}

void BlinkCursor::OnResume(BlinkCursor* self, Ctx* cx, const TickEvent*) {

    self->paused = false;
    self->visible = true;
    Listener flip;
    flip.fn = (void*)&BlinkCursor::OnFlip;
    flip.view = cx->self;
    self->timer = WindowSetInterval(cx->win, kBlinkIntervalMs, flip);
    Notify(cx);
}

static Listener BlinkListener(EntityId handle, void* fn) {
    Listener l;
    l.fn = fn;
    l.view = handle;
    return l;
}

void BlinkStart(App* app, Window* win, EntityId* handle) {
    if (!app || !win || !handle) {
        return;
    }
    if (!handle->IsValid()) {

        *handle = EntityNewRaw(app, new BlinkCursor(), nullptr,
                               &EntityDropT<BlinkCursor>);
    }
    BlinkCursor* b = BlinkGet(app, *handle);
    if (!b || b->timer) {
        return;
    }
    b->paused = false;

    b->visible = true;
    b->timer =
        WindowSetInterval(win, kBlinkIntervalMs,
                          BlinkListener(*handle, (void*)&BlinkCursor::OnFlip));
    AppInvalidate(win);
}

void BlinkStop(App* app, Window* win, EntityId* handle) {
    if (!app || !win || !handle || !handle->IsValid()) {
        return;
    }
    BlinkCursor* b = BlinkGet(app, *handle);
    if (!b) {
        return;
    }
    WindowCancelTimer(win, b->timer);
    b->timer = 0;
    b->paused = false;
    b->visible = false;
    AppInvalidate(win);
}

void BlinkPause(App* app, Window* win, EntityId* handle) {
    if (!app || !win || !handle || !handle->IsValid()) {
        return;
    }
    BlinkCursor* b = BlinkGet(app, *handle);
    if (!b || !b->timer) {
        return;
    }
    WindowCancelTimer(win, b->timer);
    b->paused = true;
    b->visible = true;
    b->timer =
        WindowSetTimeout(win, kBlinkPauseMs,
                         BlinkListener(*handle, (void*)&BlinkCursor::OnResume));
    AppInvalidate(win);
}

bool BlinkVisible(App* app, EntityId handle) {
    BlinkCursor* b = BlinkGet(app, handle);
    if (!b || !b->timer) {
        return false;
    }

    return b->paused || b->visible;
}

void WindowTimerTick(Window* win) {
    if (!win) {
        return;
    }
    double now = TimeNow();
    bool repaint = false;

    int n = win->timers.len;
    for (int i = 0; i < n && i < win->timers.len; i++) {
        TimerSub& t = win->timers[i];
        if (t.dueAt > now) {
            continue;
        }
        Listener l = t.l;
        int ms = t.ms;
        if (t.repeat) {
            t.dueAt = now + (double)ms / 1000.0;
        } else {
            t.dueAt = 0;
        }
        TickEvent ev = {ms};
        ListenerCall(win->app, win, l, &ev);
        repaint = true;
    }

    int keep = 0;
    for (int i = 0; i < win->timers.len; i++) {
        const TimerSub& t = win->timers[i];
        bool dead = t.dueAt <= 0 ||
                    (t.l.view.IsValid() && !EntityGet(win->app, t.l.view));
        if (dead) {
            continue;
        }
        win->timers[keep++] = win->timers[i];
    }
    win->timers.len = keep;

    if (win->anim || repaint) {
        AppInvalidate(win);
    }
    PlatSetTimer(win, WindowTimerMs(win));
}

int WindowChromeHit(Window* win, float x, float y) {
    if (!win) {
        return 0;
    }
    int id = HitTest(&win->paint, x, y);
    if (id == ClickWinMin || id == ClickWinMax || id == ClickWinClose ||
        id == ClickWinCaption) {
        return id;
    }
    return 0;
}

int WindowTimerMs(Window* win) {
    if (!win) {
        return 0;
    }

    double now = TimeNow();
    double soonest = -1;
    if (win->anim || win->opts.anim) {
        soonest = now + 0.016;
    }
    for (int i = 0; i < win->timers.len; i++) {
        double due = win->timers[i].dueAt;
        if (due > 0 && (soonest < 0 || due < soonest)) {
            soonest = due;
        }
    }
    if (soonest < 0) {
        return 0;
    }
    int ms = (int)((soonest - now) * 1000.0 + 0.5);
    return ms > 0 ? ms : 1;
}

Window* WindowAlloc(App* app, WinOpts opts) {
    if (!app) {
        return nullptr;
    }
    Window* win = new Window();
    win->app = app;
    win->opts = opts;
    win->anim = opts.anim;

    win->paint.pa = app->paint;
    app->windows.Append(win);
    return win;
}

bool AppAnyWindowOpen(App* app) {
    if (!app) {
        return false;
    }
    for (int i = 0; i < app->windows.len; i++) {
        if (app->windows[i]->plat) {
            return true;
        }
    }
    return false;
}

void WindowClosed(Window* win) {
    if (!win) {
        return;
    }
    PaintTargetFree(&win->paint);
    win->plat = nullptr;
    win->running = false;
}

App* AppNew() {
    App* app = new App();
    app->paint = PaintAppNew();
    if (!app->paint) {
        delete app;
        return nullptr;
    }
    if (!PlatInit(app)) {
        PaintAppFree(app->paint);
        delete app;
        return nullptr;
    }
    return app;
}

void AppFree(App* app) {
    if (!app) {
        return;
    }
    EntityDropAll(app);
    for (int i = 0; i < app->windows.len; i++) {
        Window* w = app->windows[i];
        if (w->frameArena) {
            ArenaDelete(w->frameArena);
        }
        TextMeasClear(&w->paint);
        PaintTargetFree(&w->paint);
        w->timers.Reset();
        WindowKeyedFree(w);
        delete w;
    }
    app->windows.Reset();
    PaintAppFree(app->paint);
    app->paint = nullptr;
    PlatShutdown(app);
    delete app;
    DestroyTempArena();
}

void AppRequestAnim(Window* win, bool on) {
    if (!win) {
        return;
    }
    win->anim = on;
    win->opts.anim = on;

    PlatSetTimer(win, WindowTimerMs(win));
}

static bool gGeomAsked = false;
static int gGeom[4] = {0, 0, 0, 0};

static bool ParseGeom(const char* s, int out[4]) {
    for (int i = 0; i < 4; i++) {
        if (i > 0) {
            if (*s != ',') {
                return false;
            }
            s++;
        }
        bool neg = false;
        if (*s == '-') {
            neg = true;
            s++;
        }
        int digits = 0;
        int v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            s++;
            digits++;
            if (digits > 6) {
                return false;
            }
        }
        if (digits == 0) {
            return false;
        }
        out[i] = neg ? -v : v;
    }
    return *s == 0 && out[2] > 0 && out[3] > 0;
}

bool WindowGeomRequested(int* x, int* y, int* w, int* h) {
    if (!gGeomAsked) {
        return false;
    }
    *x = gGeom[0];
    *y = gGeom[1];
    *w = gGeom[2];
    *h = gGeom[3];
    return true;
}

int GpuiTakeRuntimeArgs(int argc, char** argv) {
    const char* kGeom = "-gpui-window=";
    int keep = 0;
    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        size_t kGeomLen = strlen(kGeom);
        if (i > 0 && a && strncmp(a, kGeom, kGeomLen) == 0) {
            int g[4];
            if (ParseGeom(a + kGeomLen, g)) {
                gGeomAsked = true;
                for (int k = 0; k < 4; k++) {
                    gGeom[k] = g[k];
                }
            }
            continue;
        }
        argv[keep++] = argv[i];
    }
    for (int i = keep; i <= argc; i++) {
        argv[i] = nullptr;
    }
    return keep;
}

void WindowClampToDisplay(int* dipW, int* dipH, int screenW, int screenH) {
    if (screenW > 0 && *dipW > (int)(screenW * 0.85f)) {
        *dipW = (int)(screenW * 0.85f);
    }
    if (screenH > 0 && *dipH > (int)(screenH * 0.85f)) {
        *dipH = (int)(screenH * 0.85f);
    }
}

Window* WindowOpenView(App* app, Str title, int dipW, int dipH, EntityId root,
                       WinOpts opts) {
    Window* win = WindowOpen(app, title, dipW, dipH, opts);
    if (win) {
        win->root = root;
        AppInvalidate(win);
    }
    return win;
}

int AppRunView(Str title, int dipW, int dipH, EntityId root, App* app,
               WinOpts opts) {
    if (!WindowOpenView(app, title, dipW, dipH, root, opts)) {
        return 1;
    }
    int rc = AppRun(app);
    AppFree(app);
    return rc;
}

void AppClose(Window* win) {
    AppQuit(win);
}

bool AppIsMaximized(Window* win) {
    return win && win->maximized;
}

}

#line 1 "src/ui/Accordion.cpp"

namespace gpui {

El* Accordion::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}

El* AccordionTrigger::New(Ctx* cx, Str id, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, id, clickId);
}

El* AccordionHeader::New(Ctx* cx, El* trigger) {
    Arena* a = cx->a;
    return Div(a)->Child(trigger);
}

El* AccordionPanel::New(Ctx* cx) {
    Arena* a = cx->a;
    return Div(a);
}

AccordionItem* AccordionItem::New(Ctx* cx) {
    Arena* a = cx->a;
    AccordionItem* item = ArenaNew<AccordionItem>(a);
    item->root = Div(a)->FlexCol();
    return item;
}

AccordionItem* AccordionItem::Open(bool v) {
    open = v;
    return this;
}

AccordionItem* AccordionItem::Header(El* header) {
    root->Child(header);
    return this;
}

AccordionItem* AccordionItem::Panel(El* panel) {
    if (open && panel) {
        root->Child(panel);
    }
    return this;
}

El* AccordionItem::IntoEl() {
    return root;
}
}

#line 1 "src/ui/AlertDialog.cpp"

namespace gpui {

El* AlertDialogBackdrop::New(Ctx* cx) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("alert-dialog-backdrop"), 0);
}
El* AlertDialogPopup::New(Ctx* cx) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("alert-dialog-popup"), 0);
}
El* AlertDialogTitle::New(Ctx* cx) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("alert-dialog-title"), 0);
}
El* AlertDialogDescription::New(Ctx* cx) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("alert-dialog-description"), 0);
}
El* AlertDialogCancel::New(Ctx* cx) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("alert-dialog-cancel"), 0);
}
El* AlertDialogAction::New(Ctx* cx) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("alert-dialog-action"), 0);
}

AlertDialog* AlertDialog::New(Ctx* cx) {
    Arena* a = cx->a;
    AlertDialog* d = ArenaNew<AlertDialog>(a);

    d->root = Div(a)->Fixed()->Top(0)->Left(0)->W(kFill)->H(kFill)->FlexCol();
    return d;
}

AlertDialog* AlertDialog::Backdrop(El* backdrop) {
    if (backdrop) {
        root->Child(backdrop);
    }
    return this;
}

AlertDialog* AlertDialog::Popup(El* popup) {
    if (popup) {
        root->Child(popup);
    }
    return this;
}

El* AlertDialog::IntoEl() {
    return root;
}
}

#line 1 "src/ui/Avatar.cpp"

namespace gpui {

Avatar* Avatar::New(Ctx* cx) {
    Arena* a = cx->a;
    Avatar* v = ArenaNew<Avatar>(a);
    v->root = Div(a);
    return v;
}

Avatar* Avatar::Size(float px) {
    root->W(px)->H(px);
    return this;
}

Avatar* Avatar::Fallback(El* fb) {
    fallback = fb;
    return this;
}

El* Avatar::IntoEl() {
    if (fallback) {
        root->Child(fallback);
    }
    return root;
}

El* AvatarFallback::New(Ctx* cx) {
    Arena* a = cx->a;
    return Div(a);
}
}

#line 1 "src/ui/Button.cpp"

namespace gpui {

El* Button::New(Ctx* cx, Str id, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, id, clickId);
}
}

#line 1 "src/ui/Calendar.cpp"

namespace gpui {

El* Calendar::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
El* CalendarItem::New(Ctx* cx, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("calendar-item"), clickId);
}
}

#line 1 "src/ui/Checkbox.cpp"

namespace gpui {

El* Checkbox::New(Ctx* cx, Str id, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, id, clickId);
}

El* CheckboxIndicator::New(Ctx* cx) {
    Arena* a = cx->a;
    return Div(a);
}
}

#line 1 "src/ui/Collapsible.cpp"

namespace gpui {

Collapsible* Collapsible::New(Ctx* cx) {
    Arena* a = cx->a;
    Collapsible* c = ArenaNew<Collapsible>(a);
    c->root = Div(a)->FlexCol();
    return c;
}

Collapsible* Collapsible::Open(bool v) {
    open = v;
    return this;
}

Collapsible* Collapsible::Child(El* e) {
    root->Child(e);
    return this;
}

Collapsible* Collapsible::Content(El* e) {
    if (open && e) {
        root->Child(e);
    }
    return this;
}

El* Collapsible::IntoEl() {
    return root;
}
}

#line 1 "src/ui/ColorPicker.cpp"

namespace gpui {

El* ColorPicker::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
El* ColorSwatch::New(Ctx* cx, Str id, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, id, clickId);
}
}

#line 1 "src/ui/Combobox.cpp"

namespace gpui {

El* Combobox::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
}

#line 1 "src/ui/DatePicker.cpp"

namespace gpui {

El* DatePicker::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
}

#line 1 "src/ui/Dialog.cpp"

namespace gpui {

El* DialogBackdrop::New(Ctx* cx) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("dialog-backdrop"), 0);
}
El* DialogPopup::New(Ctx* cx) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("dialog-popup"), 0);
}
El* DialogTitle::New(Ctx* cx) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("dialog-title"), 0);
}
El* DialogDescription::New(Ctx* cx) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("dialog-description"), 0);
}
El* DialogClose::New(Ctx* cx, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("dialog-close"), clickId);
}

Dialog* Dialog::New(Ctx* cx) {
    Arena* a = cx->a;
    Dialog* d = ArenaNew<Dialog>(a);
    d->root = Div(a)->Fixed()->Top(0)->Left(0)->W(kFill)->H(kFill);
    return d;
}

Dialog* Dialog::Backdrop(El* backdrop) {
    if (backdrop) {
        root->Child(backdrop);
    }
    return this;
}

Dialog* Dialog::Popup(El* popup) {
    if (popup) {
        root->Child(popup);
    }
    return this;
}

El* Dialog::IntoEl() {
    return root;
}
}

#line 1 "src/ui/HoverCard.cpp"

namespace gpui {

HoverCard* HoverCard::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    HoverCard* h = ArenaNew<HoverCard>(a);
    h->root = UiRoot(a, id, 0);
    return h;
}

HoverCard* HoverCard::Trigger(El* trigger) {
    if (trigger) {
        root->Child(trigger);
    }
    return this;
}

HoverCard* HoverCard::Content(El* content) {
    if (content) {

        if (!content->style.absolute) {
            content->Absolute()->Top(22)->Left(0);
        }
        root->Child(content);
    }
    return this;
}

El* HoverCard::IntoEl() {
    return root;
}
}

#line 1 "src/ui/Input.cpp"

namespace gpui {

El* InputBase::New(Ctx* cx, Str id, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, id, clickId);
}

El* Input::New(Ctx* cx, LineInput* state) {
    return New(cx, state, InputEditorStyle{});
}

El* Input::New(Ctx* cx, LineInput* state, const InputEditorStyle& style) {
    Arena* a = cx->a;
    if (!state) {
        return TextEl(a, Str{});
    }
    float font = style.fontSize > 0 ? style.fontSize : 12.f;

    const float kInputLineH = 20.f;
    float lineMult = kInputLineH / font;
    float caretH = font + 2.f;
    Rgba fg = state->len > 0 ? style.foreground : style.mutedForeground;
    bool caret = state->focused && BlinkVisible(cx, state->blink);
    El* bar = Div(a)->W(2)->H(caretH)->Bg(style.caret);
    El* slot = caret ? bar : Div(a)->W(2)->H(caretH);
    El* row = Div(a)->FlexRow()->ItemsCenter()->H(kInputLineH);
    if (style.align == 1) {
        row->W(kFill)->JustifyCenter();
    } else if (style.align == 2) {
        row->W(kFill)->JustifyEnd();
    }
    if (state->len <= 0) {

        if (caret) {
            row->Child(bar->Absolute()->Left(0)->Top(1));
        }
        return row->Child(TextEl(a, Str(state->placeholder))
                              ->Font(font)
                              ->LineHeight(lineMult)
                              ->Fg(fg));
    }
    if (style.mask) {

        int chars = 0;
        for (int i = 0; i < state->len; i++) {
            if (((unsigned char)state->buf[i] & 0xc0) != 0x80) {
                chars++;
            }
        }
        char* dots = (char*)Alloc(a, chars * 3 + 1);
        int n = 0;
        for (int i = 0; i < chars; i++) {
            memcpy(dots + n, "\xE2\x80\xA2", 3);
            n += 3;
        }
        dots[n] = 0;
        row->Child(
            TextEl(a, Str(dots, n))->Font(font)->LineHeight(lineMult)->Fg(fg));
        if (state->focused) {
            row->Child(slot);
        }
        return row;
    }
    int cur = state->cursor;
    if (cur < 0) {
        cur = 0;
    }
    if (cur > state->len) {
        cur = state->len;
    }
    if (cur > 0) {
        row->Child(TextEl(a, Str(state->buf, cur))
                       ->Font(font)
                       ->LineHeight(lineMult)
                       ->Fg(fg));
    }
    if (state->focused) {
        row->Child(slot);
    }
    if (cur < state->len) {
        row->Child(TextEl(a, Str(state->buf + cur, state->len - cur))
                       ->Font(font)
                       ->LineHeight(lineMult)
                       ->Fg(fg));
    }
    return row;
}

El* Textarea::New(Ctx* cx, const char* text, bool caret) {
    InputEditorStyle style;
    return New(cx, text, style, caret);
}

El* Textarea::New(Ctx* cx, const char* text, const InputEditorStyle& style,
                  bool caret, bool softWrap) {
    Arena* a = cx->a;
    El* col = Div(a)->FlexCol();
    if (!text) {
        text = "";
    }
    int i = 0;
    for (;;) {
        int start = i;
        while (text[i] && text[i] != '\n') {
            i++;
        }
        char tmp[512];
        int n = i - start;
        if (n > 511) {
            n = 511;
        }
        memcpy(tmp, text + start, (size_t)n);
        tmp[n] = 0;
        bool last = text[i] == 0;
        float font = style.fontSize > 0 ? style.fontSize : 12.f;

        El* line = TextEl(a, StrDup(a, Str(tmp)))
                       ->Font(font)
                       ->LineHeight(20.f / font)
                       ->Fg(style.foreground);
        if (softWrap) {
            line->Wrap();
        }
        if (last && caret) {
            El* row = Div(a)->FlexRow()->ItemsCenter()->H(20);
            row->Child(line);
            row->Child(Div(a)->W(2)->H(14)->Bg(style.caret));
            col->Child(row);
        } else {
            col->Child(line);
        }
        if (last) {
            break;
        }
        i++;
    }
    return col;
}

static bool ui_Input_IsIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static bool TokEq(const char* s, int n, const char* kw) {
    int k = (int)strlen(kw);
    return n == k && memcmp(s, kw, (size_t)n) == 0;
}

static Rgba EditorTokColor(const char* s, int n) {
    static const char* kws[] = {"use",  "struct", "fn",    "impl",  "let",
                                "mut",  "pub",    "self",  "Self",  "as",
                                "in",   "for",    "if",    "else",  "return",
                                "true", "false",  "crate", "super", nullptr};
    for (int i = 0; kws[i]; i++) {
        if (TokEq(s, n, kws[i])) {
            return Rgb(0x25, 0x63, 0xeb);
        }
    }
    static const char* tys[] = {"usize", "isize",   "i32",  "u32",
                                "u64",   "i64",     "str",  "String",
                                "bool",  "HashMap", nullptr};
    for (int i = 0; tys[i]; i++) {
        if (TokEq(s, n, tys[i])) {
            return Rgb(0x25, 0x63, 0xeb);
        }
    }
    return Rgb(0x17, 0x17, 0x17);
}

static void EditorSpan(Arena* a, El* row, const char* s, int n, Rgba c) {
    if (n <= 0) {
        return;
    }
    row->Child(TextEl(a, StrDup(a, Str(s, n)))->Font(12)->Fg(c));
}

static void EditorCaret(Arena* a, El* row, bool on) {
    El* slot = Div(a)->W(2)->H(14);
    if (on) {
        slot->Bg(Rgb(0x17, 0x17, 0x17));
    }
    row->Child(slot);
}

static void EditorRun(Arena* a, El* row, const char* s, int n, Rgba c, int* col,
                      int caretCol, bool caretOn) {
    if (n <= 0) {
        return;
    }
    int start = *col;
    int end = start + n;
    if (caretCol < start || caretCol > end) {
        EditorSpan(a, row, s, n, c);
        *col = end;
        return;
    }
    int left = caretCol - start;
    if (left > 0) {
        EditorSpan(a, row, s, left, c);
    }
    EditorCaret(a, row, caretOn);
    if (n - left > 0) {
        EditorSpan(a, row, s + left, n - left, c);
    }
    *col = end;
}

static void HighlightEditorLine(Arena* a, El* row, const char* line, int n,
                                int caretCol, bool caretOn) {
    int i = 0;
    int col = 0;
    while (i < n && line[i] == ' ') {
        if (caretCol == col) {
            EditorCaret(a, row, caretOn);
        }

        EditorSpan(a, row, " ", 1, Rgb(0xa3, 0xa3, 0xa3));
        col++;
        i++;
    }
    while (i < n) {
        if (line[i] == '/' && i + 1 < n && line[i + 1] == '/') {
            EditorRun(a, row, line + i, n - i, Rgb(0x73, 0x73, 0x73), &col,
                      caretCol, caretOn);
            i = n;
            break;
        }
        if (line[i] == '#') {
            int j = i + 1;
            while (j < n && line[j] != ']') {
                j++;
            }
            if (j < n) {
                j++;
            }
            EditorRun(a, row, line + i, j - i, Rgb(0x7c, 0x3a, 0xed), &col,
                      caretCol, caretOn);
            i = j;
            continue;
        }
        if (line[i] == '"') {
            int j = i + 1;
            while (j < n && line[j] != '"') {
                j++;
            }
            if (j < n) {
                j++;
            }
            EditorRun(a, row, line + i, j - i, Rgb(0x16, 0xa3, 0x4a), &col,
                      caretCol, caretOn);
            i = j;
            continue;
        }
        if (ui_Input_IsIdentChar(line[i]) && (line[i] < '0' || line[i] > '9')) {
            int j = i + 1;
            while (j < n && ui_Input_IsIdentChar(line[j])) {
                j++;
            }
            EditorRun(a, row, line + i, j - i, EditorTokColor(line + i, j - i),
                      &col, caretCol, caretOn);
            i = j;
            continue;
        }
        EditorRun(a, row, line + i, 1, Rgb(0x17, 0x17, 0x17), &col, caretCol,
                  caretOn);
        i++;
    }
    if (caretCol == col) {
        EditorCaret(a, row, caretOn);
    }
}

El* Editor::New(Ctx* cx, const char* text) {
    return New(cx, text, -1, false);
}

El* Editor::New(Ctx* cx, const char* text, int cursor, bool caret) {
    Arena* a = cx->a;
    El* col = Div(a)->FlexCol();
    if (!text) {
        text = "";
    }
    bool blink = caret;
    int i = 0;
    int pos = 0;
    int lineNo = 1;
    for (;;) {
        int start = i;
        while (text[i] && text[i] != '\n') {
            i++;
        }
        int n = i - start;
        int caretCol = -1;
        if (cursor >= pos && cursor <= pos + n) {
            caretCol = cursor - pos;
        }
        El* row = Div(a)->FlexRow()->H(20)->Gap(8)->ItemsCenter();
        row->Child(Div(a)->W(20)->JustifyEnd()->Child(
            TextEl(a, StrDup(a, fmt("%d", lineNo)))
                ->Font(11)
                ->Fg(Rgb(0xa3, 0xa3, 0xa3))));
        El* code = Div(a)->FlexRow()->ItemsCenter();
        HighlightEditorLine(a, code, text + start, n, caretCol, blink);
        row->Child(code);
        col->Child(row);
        lineNo++;
        if (!text[i]) {
            break;
        }
        i++;
        pos = i;
        if (lineNo > 80) {
            break;
        }
    }
    return col;
}
}

#line 1 "src/ui/Link.cpp"

namespace gpui {

El* Link::New(Ctx* cx, Str id, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, id, clickId);
}
}

#line 1 "src/ui/NumberInput.cpp"

namespace gpui {

El* NumberInput::New(Ctx* cx) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("example-number"), 0);
}
}

#line 1 "src/ui/OtpInput.cpp"

namespace gpui {

El* OtpInput::New(Ctx* cx, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("example-otp"), clickId);
}
}

#line 1 "src/ui/Pagination.cpp"

namespace gpui {

El* Pagination::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
El* PaginationItem::New(Ctx* cx, Str id, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, id, clickId);
}
}

#line 1 "src/ui/Popover.cpp"

namespace gpui {

Popover* Popover::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    Popover* p = ArenaNew<Popover>(a);
    p->root = UiRoot(a, id, 0);
    return p;
}

Popover* Popover::Trigger(El* trigger) {
    if (trigger) {
        root->Child(trigger);
    }
    return this;
}

Popover* Popover::Content(El* content) {
    if (content) {
        content->Absolute()->Top(28)->Left(0);
        root->Child(content);
    }
    return this;
}

El* Popover::IntoEl() {
    return root;
}
}

#line 1 "src/ui/Popup.cpp"

namespace gpui {

Popup* Popup::New(Ctx* cx, Str id, El* trigger) {
    Arena* a = cx->a;
    Popup* p = ArenaNew<Popup>(a);

    p->root = UiRoot(a, id, 0);
    if (trigger) {
        p->root->Child(trigger);
    }
    return p;
}

Popup* Popup::AnchorRight(bool on) {
    anchorRight = on;
    return this;
}

Popup* Popup::Content(El* content) {
    if (content) {

        if (!content->style.absolute) {
            content->AnchorBelow(4);
            if (anchorRight) {
                content->Right(0);
            } else {
                content->Left(0);
            }
        }

        content->Deferred();
        root->Child(content);
    }
    return this;
}

El* Popup::IntoEl() {
    return root;
}
}

#line 1 "src/ui/Progress.cpp"

namespace gpui {

El* Progress::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}

El* ProgressTrack::New(Ctx* cx) {
    Arena* a = cx->a;
    return Div(a);
}

El* ProgressIndicator::New(Ctx* cx) {
    Arena* a = cx->a;
    return Div(a);
}
}

#line 1 "src/ui/Radio.cpp"

namespace gpui {

El* Radio::New(Ctx* cx, Str id, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, id, clickId);
}

El* RadioGroup::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
}

#line 1 "src/ui/Resizable.cpp"

namespace gpui {

El* Resizable::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
El* ResizablePanel::New(Ctx* cx) {
    Arena* a = cx->a;
    return Div(a);
}
}

#line 1 "src/ui/Scrollbar.cpp"

namespace gpui {

El* Scrollbar::New(Ctx* cx) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("example-scroll-region"), 0);
}
}

#line 1 "src/ui/Select.cpp"

namespace gpui {

El* Select::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
}

#line 1 "src/ui/Sheet.cpp"

namespace gpui {

Sheet* Sheet::New(Ctx* cx) {
    Arena* a = cx->a;
    Sheet* s = ArenaNew<Sheet>(a);
    s->root = Div(a)->Fixed()->Top(0)->Left(0)->W(kFill)->H(kFill);
    return s;
}

Sheet* Sheet::Overlay(El* overlay) {
    if (overlay) {
        root->Child(overlay);
    }
    return this;
}

Sheet* Sheet::Surface(El* surface) {
    if (surface) {
        root->Child(surface);
    }
    return this;
}

El* Sheet::IntoEl() {
    return root;
}
}

#line 1 "src/ui/Slider.cpp"

namespace gpui {

static float ClampF(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    return v > hi ? hi : v;
}

SliderValue SliderValueClamp(SliderValue v, float min, float max) {
    v.hi = ClampF(v.hi, min, max);
    if (v.range) {
        v.lo = ClampF(v.lo, min, max);
    }
    return v;
}

void SliderValueSetStart(SliderValue* v, float value) {
    if (v->range) {
        v->lo = value < v->hi ? value : v->hi;
    } else {
        v->hi = value;
    }
}

void SliderValueSetEnd(SliderValue* v, float value) {
    if (v->range) {
        v->hi = value > v->lo ? value : v->lo;
    } else {
        v->hi = value;
    }
}

static void FixLogLimits(SliderState* s) {
    if (s->scale != SliderScale::Logarithmic) {
        return;
    }
    if (s->min <= 0) {
        s->min = 0.0001f;
    }
    if (s->max <= s->min) {
        s->max = s->min * 2.f;
    }
}

SliderState SliderStateNew(float min, float max, SliderValue value, float step,
                           SliderScale scale) {
    SliderState s = {};
    s.min = min;
    s.max = max;
    s.step = step;
    s.scale = scale;
    FixLogLimits(&s);
    s.value = value;
    SliderUpdateThumbPos(&s);
    return s;
}

void SliderSetLimits(SliderState* s, float min, float max) {
    s->min = min;
    s->max = max;
    FixLogLimits(s);
    SliderUpdateThumbPos(s);
}

void SliderSetStep(SliderState* s, float step) {
    s->step = step;
}

void SliderSetScale(SliderState* s, SliderScale scale) {
    s->scale = scale;
    FixLogLimits(s);
    SliderUpdateThumbPos(s);
}

void SliderSetValue(SliderState* s, SliderValue v) {
    s->value = v;
    SliderUpdateThumbPos(s);
}

float SliderPctToValue(const SliderState* s, float pct) {
    if (s->scale == SliderScale::Linear) {
        return s->min + (s->max - s->min) * pct;
    }

    float base = s->max / s->min;
    return ClampF(powf(base, pct) * s->min, s->min, s->max);
}

float SliderValueToPct(const SliderState* s, float value) {
    if (s->scale == SliderScale::Linear) {
        float range = s->max - s->min;
        if (range <= 0) {
            return 0;
        }
        return (value - s->min) / range;
    }

    float base = s->max / s->min;
    float logBase = ::logf(base);
    if (logBase == 0 || value <= 0) {
        return 0;
    }
    return ClampF(::logf(value / s->min) / logBase, 0.f, 1.f);
}

void SliderUpdateThumbPos(SliderState* s) {
    if (!s->value.range) {
        s->pctLo = 0;
        s->pctHi = SliderValueToPct(s, ClampF(s->value.hi, s->min, s->max));
        return;
    }
    s->pctLo = SliderValueToPct(s, ClampF(s->value.lo, s->min, s->max));
    s->pctHi = SliderValueToPct(s, ClampF(s->value.hi, s->min, s->max));
}

static float PctAt(const SliderState* s, Axis axis, Point pos) {
    float inner = AxisIsHorizontal(axis) ? pos.x - s->bounds.x
                                         : s->bounds.Bottom() - pos.y;
    float total = AxisIsHorizontal(axis) ? s->bounds.w : s->bounds.h;
    if (total <= 0) {
        return 0;
    }
    return ClampF(inner, 0.f, total) / total;
}

bool SliderIsStartAt(const SliderState* s, Axis axis, Point pos) {
    if (!s->value.range) {
        return false;
    }
    float center = (s->pctHi - s->pctLo) * 0.5f + s->pctLo;
    return PctAt(s, axis, pos) < center;
}

bool SliderUpdateByPosition(SliderState* s, Axis axis, Point pos,
                            bool isStart) {
    s->dragging = true;
    float pct = PctAt(s, axis, pos);
    pct = isStart ? ClampF(pct, 0.f, s->pctHi) : ClampF(pct, s->pctLo, 1.f);

    float value = SliderPctToValue(s, pct);
    if (s->step > 0) {
        value = roundf(value / s->step) * s->step;
    }

    SliderValue before = s->value;
    if (isStart) {
        s->pctLo = pct;
        SliderValueSetStart(&s->value, value);
    } else {
        s->pctHi = pct;
        SliderValueSetEnd(&s->value, value);
    }
    return s->value.lo != before.lo || s->value.hi != before.hi;
}

bool SliderHandleRelease(SliderState* s) {
    if (!s->dragging) {
        return false;
    }
    s->dragging = false;
    return true;
}

El* Slider::New(Ctx* cx, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("example-slider"), clickId);
}
El* SliderTrack::New(Ctx* cx, SliderState* state, Axis axis) {
    Arena* a = cx->a;
    El* e = Div(a);
    if (state) {
        e->BindSlider(state, axis);
    }
    return e;
}
El* SliderIndicator::New(Ctx* cx, SliderState* state) {
    Arena* a = cx->a;
    El* e = Div(a);
    if (state) {
        e->BindSliderBounds(state);
    }
    return e;
}
El* SliderThumb::New(Ctx* cx) {
    Arena* a = cx->a;
    return Div(a);
}
}

#line 1 "src/ui/Switch.cpp"

namespace gpui {

El* Switch::New(Ctx* cx, Str id, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, id, clickId);
}

El* SwitchTrack::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}

El* SwitchThumb::New(Ctx* cx) {
    Arena* a = cx->a;
    return Div(a);
}
}

#line 1 "src/ui/Table.cpp"

namespace gpui {

El* Table::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
El* TableHeader::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
El* TableBody::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
El* TableRow::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
El* TableHead::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
El* TableCell::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
}

#line 1 "src/ui/Tabs.cpp"

namespace gpui {

El* Tabs::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
El* Tab::New(Ctx* cx, Str id, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, id, clickId);
}
}

#line 1 "src/ui/TextSelection.cpp"

namespace gpui {

El* TextSelection::New(Ctx* cx, Str id, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, id, clickId);
}
}

#line 1 "src/ui/Toast.cpp"

namespace gpui {

El* Toast::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
}

#line 1 "src/ui/Toggle.cpp"

namespace gpui {

El* Toggle::New(Ctx* cx, Str id, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, id, clickId);
}

El* ToggleGroup::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
}

#line 1 "src/ui/Tooltip.cpp"

namespace gpui {

El* Tooltip::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
}

#line 1 "src/ui/Tree.cpp"

namespace gpui {

El* Tree::New(Ctx* cx) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("example-tree"), 0);
}
El* TreeItem::New(Ctx* cx, int clickId) {
    Arena* a = cx->a;
    return UiRoot(a, StrL("tree-item"), clickId);
}
}

#line 1 "src/ui/VirtualList.cpp"

namespace gpui {

El* VirtualList::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    return UiRoot(a, id, 0);
}
}

#line 1 "src/component/Accordion.cpp"

namespace gpui {

namespace component {

Accordion* Accordion::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    Accordion* acc = ArenaNew<Accordion>(a);
    acc->a = a;
    acc->cx = cx;
    acc->id = id;
    return acc;
}

Accordion* Accordion::Multiple(bool v) {
    multiple = v;
    return this;
}
Accordion* Accordion::Bordered(bool v) {
    bordered = v;
    return this;
}
Accordion* Accordion::Disabled(bool v) {
    disabled = v;
    return this;
}
Accordion* Accordion::WithSize(UiSize s) {
    size = s;
    return this;
}
Accordion* Accordion::Item(Str title, Str body, bool open) {
    if (nItems < 8) {
        items[nItems].title = title;
        items[nItems].body = body;
        items[nItems].open = open;
        nItems++;
    }
    return this;
}
Accordion* Accordion::SettingsItem(Str title, Str body, bool open,
                                   IconName icon, Str tag) {
    if (nItems < 8) {
        items[nItems].title = title;
        items[nItems].body = body;
        items[nItems].open = open;
        items[nItems].icon = icon;
        items[nItems].tag = tag;
        items[nItems].settings = true;
        nItems++;
    }
    return this;
}
Accordion* Accordion::OnToggle(Listener fn) {
    onToggle = fn;
    return this;
}

El* Accordion::IntoEl() {
    const Theme& th = cx->theme();

    El* root =
        gpui::Accordion::New(cx, id)->FlexCol()->W(kFill)->Bg(th.background);
    if (bordered) {
        root->Border(1, th.border)->Radius(th.radiusLg)->ClipY();
    }
    for (int i = 0; i < nItems; i++) {
        float font = UiFontPx(size);
        El* trig = AccordionTrigger::New(
            cx, items[i].title, disabled ? 0 : HashClickId(items[i].title));

        trig->FlexRow()->ItemsCenter()->JustifyBetween()->PadX(12)->PadY(8)->W(
            kFill);
        if (items[i].settings) {
            El* left = Div(a)->FlexRow()->Gap(8)->ItemsCenter()->Grow();
            if (items[i].icon != IconName::None) {
                left->Child(
                    Div(a)
                        ->W(32)
                        ->H(32)
                        ->Radius(8)
                        ->Bg(RgbaOpacity(th.secondary, 0.5f))
                        ->ItemsCenter()
                        ->JustifyCenter()
                        ->Shrink0()
                        ->Child(IconEl(a, items[i].icon, 16)->Fg(th.mutedFg)));
            }
            left->Child(TextEl(a, items[i].title)
                            ->Font(font)
                            ->Fg(th.foreground)
                            ->Semibold());
            if (items[i].tag.s) {
                left->Child(Tag::New(cx, items[i].tag)
                                ->Success()
                                ->Outline()
                                ->WithSize(UiSize::Small)
                                ->IntoEl());
            }
            trig->Child(left);
        } else {
            trig->Child(TextEl(a, items[i].title)
                            ->Font(font)
                            ->Fg(th.foreground)
                            ->Semibold());
        }
        trig->Child(
            IconEl(a,
                   items[i].open ? IconName::ChevronUp : IconName::ChevronDown,
                   14)
                ->Fg(th.mutedFg));
        if (onToggle.IsValid() && !disabled) {
            trig->OnClick(ListenerArg(onToggle, i));
        }
        gpui::AccordionItem* it =
            gpui::AccordionItem::New(cx)
                ->Open(items[i].open)
                ->Header(gpui::AccordionHeader::New(cx, trig));
        El* panel = gpui::AccordionPanel::New(cx);
        if (items[i].settings) {
            panel->PadL(52)->PadR(8)->PadT(0)->PadB(12);
        } else {

            panel->PadX(12)->PadT(0)->PadB(8);
        }
        it->Panel(panel->Child(
            TextEl(a, items[i].body)->Font(font)->Fg(th.mutedFg)->Wrap()));

        El* itEl = it->IntoEl();
        if (i + 1 < nItems) {
            itEl->BorderB(1, th.border);
        }
        root->Child(itEl);
    }
    return root;
}

}
}

#line 1 "src/component/Alert.cpp"

namespace gpui {

namespace component {

Alert* Alert::New(Ctx* cx, Str id, Str message) {
    Arena* a = cx->a;
    Alert* al = ArenaNew<Alert>(a);
    al->a = a;
    al->cx = cx;
    al->id = id;
    al->message = message;
    return al;
}

Alert* Alert::Info(Ctx* cx, Str id, Str message) {
    Alert* al = New(cx, id, message);
    al->variant = AlertVariant::Info;
    al->icon = IconName::Info;
    return al;
}
Alert* Alert::Success(Ctx* cx, Str id, Str message) {
    Alert* al = New(cx, id, message);
    al->variant = AlertVariant::Success;
    al->icon = IconName::CircleCheck;
    return al;
}
Alert* Alert::Warning(Ctx* cx, Str id, Str message) {
    Alert* al = New(cx, id, message);
    al->variant = AlertVariant::Warning;
    al->icon = IconName::TriangleAlert;
    return al;
}
Alert* Alert::Error(Ctx* cx, Str id, Str message) {
    Alert* al = New(cx, id, message);
    al->variant = AlertVariant::Error;
    al->icon = IconName::CircleX;
    return al;
}

Alert* Alert::Title(Str s) {
    title = s;
    return this;
}
Alert* Alert::Icon(IconName n) {
    icon = n;
    return this;
}
Alert* Alert::Content(El* e) {
    content = e;
    return this;
}
Alert* Alert::Banner() {
    banner = true;
    return this;
}
Alert* Alert::Visible(bool v) {
    visible = v;
    return this;
}
Alert* Alert::OnClose(Listener fn) {
    onClose = fn;
    return this;
}
Alert* Alert::WithSize(UiSize s) {
    size = s;
    return this;
}

El* Alert::IntoEl() {
    if (!visible) {
        return Div(a);
    }
    const Theme& th = cx->theme();
    Rgba fg = th.foreground, bg = th.background, bd = th.border;
    switch (variant) {
        case AlertVariant::Info:
            fg = th.info;
            bg = RgbaOpacity(th.info, 0.04f);
            bd = RgbaOpacity(th.info, 0.3f);
            break;
        case AlertVariant::Success:
            fg = th.success;
            bg = RgbaOpacity(th.success, 0.04f);
            bd = RgbaOpacity(th.success, 0.3f);
            break;
        case AlertVariant::Warning:
            fg = th.warning;
            bg = RgbaOpacity(th.warning, 0.04f);
            bd = RgbaOpacity(th.warning, 0.3f);
            break;
        case AlertVariant::Error:
            fg = th.danger;
            bg = RgbaOpacity(th.danger, 0.04f);
            bd = RgbaOpacity(th.danger, 0.3f);
            break;
        default:
            break;
    }
    El* row = Div(a)
                  ->FlexRow()
                  ->Gap(8)
                  ->Pad(banner ? 8.f : 12.f)
                  ->ItemsStart()
                  ->Bg(bg);
    if (!banner) {
        row->Border(1, bd)->Radius(th.radius);
    }
    row->Child(IconEl(a, icon, 16)->Fg(fg)->Shrink0());
    El* col = Div(a)->FlexCol()->Gap(4)->Grow();
    if (title.s && !banner) {
        col->Child(TextEl(a, title)->Font(14)->Semibold()->Fg(fg));
    }
    if (content) {
        col->Child(content);
    } else {
        col->Child(TextEl(a, message)->Font(UiFontPx(size))->Fg(fg)->Wrap());
    }
    row->Child(col);
    if (onClose.IsValid()) {

        El* x = Div(a)
                    ->Pad(2)
                    ->Radius(th.radius)
                    ->ItemsCenter()
                    ->JustifyCenter()
                    ->Shrink0()
                    ->Child(IconEl(a, IconName::X, 16)->Fg(fg));
        BindClick(x, StrL("alert-close"), onClose);
        row->Child(x);
    }
    return row;
}

}
}

#line 1 "src/component/Avatar.cpp"

namespace gpui {

namespace component {

Avatar* Avatar::New(Ctx* cx) {
    Arena* a = cx->a;
    Avatar* v = ArenaNew<Avatar>(a);
    v->a = a;
    v->cx = cx;
    return v;
}

Avatar* Avatar::Initials(Str s) {
    initials = s;
    return this;
}
Avatar* Avatar::Bg(Rgba c) {
    bg = c;
    hasBg = true;
    return this;
}
Avatar* Avatar::Size(float v) {
    size = v;
    return this;
}

float AvatarSizePx(UiSize s) {
    switch (s) {
        case UiSize::XSmall:
            return 16;
        case UiSize::Small:
            return 24;
        case UiSize::Large:
            return 80;
        default:
            return 48;
    }
}

static float AvatarTextPx(UiSize s) {
    switch (s) {
        case UiSize::XSmall:
            return 10.4f;
        case UiSize::Small:
            return 12;
        case UiSize::Large:
            return 30;
        default:
            return 14;
    }
}

Avatar* Avatar::WithSize(UiSize s) {
    size = AvatarSizePx(s);
    textPx = AvatarTextPx(s);
    return this;
}
Avatar* Avatar::Radius(float v) {
    radius = v;
    return this;
}
Avatar* Avatar::Border(float w, Rgba c) {
    borderW = w;
    borderC = c;
    hasBorderC = true;
    return this;
}
Avatar* Avatar::Placeholder(IconName n) {
    placeholder = n;
    return this;
}

static Rgba AvatarHue(Str initials) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < initials.len; i++) {
        h ^= (uint8_t)initials.s[i];
        h *= 16777619u;
    }
    static const Rgba kCols[] = {
        Rgb(0x3b, 0x82, 0xf6), Rgb(0x22, 0xc5, 0x5e), Rgb(0xa8, 0x55, 0xf7),
        Rgb(0xf9, 0x73, 0x16), Rgb(0x06, 0xb6, 0xd4), Rgb(0xec, 0x48, 0x99),
        Rgb(0xe1, 0x1d, 0x48), Rgb(0x65, 0x43, 0xd9),
    };
    return kCols[h % (sizeof(kCols) / sizeof(kCols[0]))];
}

El* Avatar::IntoEl() {
    const Theme& th = cx->theme();
    float r = radius >= 0 ? radius : size * 0.5f;

    float inset = borderW > 0 ? borderW : 0;
    float innerSize = size - inset * 2;
    bool named = initials.s && initials.len > 0;
    Rgba fill = th.secondary;
    Rgba fg = th.mutedFg;
    if (hasBg) {
        fill = bg;
        fg = th.foreground;
    } else if (named) {
        Rgba hue = AvatarHue(initials);
        fill = RgbaOpacity(hue, 0.2f);
        fg = hue;
    }
    float txt = textPx > 0 ? textPx : size * 0.35f;
    El* inner = named ? TextEl(a, initials)->Font(txt)->Fg(fg)->Semibold()
                      : IconEl(a, placeholder, size * 0.6f)->Fg(fg);
    El* fb = AvatarFallback::New(cx)
                 ->W(innerSize)
                 ->H(innerSize)
                 ->ItemsCenter()
                 ->JustifyCenter()
                 ->Bg(fill)
                 ->Radius(r - inset)
                 ->Child(inner);

    El* el = gpui::Avatar::New(cx)
                 ->Size(size)
                 ->Fallback(fb)
                 ->IntoEl()
                 ->Radius(r)
                 ->Bg(th.secondary);
    Rgba bd = hasBorderC ? borderC : th.border;
    if (borderW > 0) {
        el->Pad(inset)->Border(borderW, bd);
    }
    return el;
}

}
}

#line 1 "src/component/Badge.cpp"

namespace gpui {

namespace component {

Badge* Badge::New(Ctx* cx) {
    Arena* a = cx->a;
    Badge* b = ArenaNew<Badge>(a);
    b->a = a;
    b->cx = cx;
    return b;
}

Badge* Badge::Count(int n) {
    count = n;
    return this;
}
Badge* Badge::Max(int n) {
    max = n;
    return this;
}
Badge* Badge::Dot() {
    kind = BadgeKind::Dot;
    return this;
}
Badge* Badge::Icon(IconName n) {
    icon = n;
    kind = BadgeKind::Icon;
    return this;
}
Badge* Badge::Color(Rgba c) {
    color = c;
    hasColor = true;
    return this;
}
Badge* Badge::WithSize(UiSize s) {
    size = s;
    return this;
}
Badge* Badge::Child(El* c) {
    child = c;
    return this;
}

El* Badge::IntoEl() {
    const Theme& th = cx->theme();
    bool visible = kind != BadgeKind::Number || count > 0;
    El* root = Div(a);
    if (child) {
        root->Child(child);
    }
    if (!visible) {
        return root;
    }
    float box = 16;
    float font = 10;
    if (size == UiSize::Large) {
        box = 24;
        font = 14;
    } else if (size == UiSize::Small || size == UiSize::XSmall) {
        box = 10;
        font = 8;
    }
    Rgba bg = hasColor ? color : th.danger;
    El* mark = Div(a)
                   ->Absolute()
                   ->Top(-box * 0.35f)
                   ->Right(-box * 0.35f)
                   ->Bg(bg)
                   ->ItemsCenter()
                   ->JustifyCenter();
    if (kind == BadgeKind::Dot) {
        mark->W(8)->H(8)->Radius(4);
    } else if (kind == BadgeKind::Icon) {
        mark->W(box)
            ->H(box)
            ->Radius(box * 0.5f)
            ->Child(IconEl(a, icon, box * 0.6f)->Fg(th.dangerFg));
    } else {
        int shown = count > max ? max : count;
        Str txt = count > max ? StrDup(a, fmt("%d+", shown))
                              : StrDup(a, fmt("%d", shown));
        mark->MinW(box)
            ->H(box)
            ->PadX(4)
            ->Radius(box * 0.5f)
            ->Child(TextEl(a, txt)->Font(font)->Fg(th.dangerFg));
    }
    root->Child(mark);
    return root;
}

}
}

#line 1 "src/component/Breadcrumb.cpp"

namespace gpui {

namespace component {

Breadcrumb* Breadcrumb::New(Ctx* cx) {
    Arena* a = cx->a;
    Breadcrumb* b = ArenaNew<Breadcrumb>(a);
    b->a = a;
    b->cx = cx;
    return b;
}
Breadcrumb* Breadcrumb::Item(Str s) {
    if (n < 8) {
        items[n++] = s;
    }
    return this;
}
Breadcrumb* Breadcrumb::OnClick(Listener fn) {
    onClick = fn;
    return this;
}

El* Breadcrumb::IntoEl() {
    const Theme& th = cx->theme();
    El* row = Div(a)->FlexRow()->ItemsCenter()->Gap(6);
    for (int i = 0; i < n; i++) {
        if (i) {
            row->Child(IconEl(a, IconName::ChevronRight, 12)->Fg(th.mutedFg));
        }
        bool last = i == n - 1;
        El* t = TextEl(a, items[i])
                    ->Font(13)
                    ->Fg(last ? th.foreground : th.mutedFg);
        if (onClick.IsValid()) {
            El* hit = Div(a)->Child(t);
            BindClick(hit, items[i], ListenerArg(onClick, i));
            row->Child(hit);
        } else {
            row->Child(t);
        }
    }
    return row;
}

}
}

#line 1 "src/component/Button.cpp"

namespace gpui {

namespace component {

Button* Button::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    Button* b = ArenaNew<Button>(a);
    b->a = a;
    b->cx = cx;
    b->id = id;
    return b;
}

Button* Button::Label(Str s) {
    label = s;
    return this;
}
Button* Button::Icon(IconName n) {
    icon = n;
    return this;
}
Button* Button::Primary() {
    variant = ButtonVariant::Primary;
    return this;
}
Button* Button::Secondary() {
    variant = ButtonVariant::Secondary;
    return this;
}
Button* Button::Danger() {
    variant = ButtonVariant::Danger;
    return this;
}
Button* Button::Warning() {
    variant = ButtonVariant::Warning;
    return this;
}
Button* Button::Success() {
    variant = ButtonVariant::Success;
    return this;
}
Button* Button::Info() {
    variant = ButtonVariant::Info;
    return this;
}
Button* Button::Ghost() {
    variant = ButtonVariant::Ghost;
    return this;
}
Button* Button::Link() {
    variant = ButtonVariant::Link;
    return this;
}
Button* Button::Text() {
    variant = ButtonVariant::Text;
    return this;
}
Button* Button::Outline() {
    outline = true;
    return this;
}
Button* Button::Compact() {
    compact = true;
    return this;
}
Button* Button::Selected(bool v) {
    selected = v;
    return this;
}
Button* Button::DropdownCaret(bool v) {
    dropdown = v;
    return this;
}
Button* Button::Custom(Rgba c) {
    custom = c;
    hasCustom = true;
    return this;
}
Button* Button::Extra(El* e) {
    extra = e;
    return this;
}
Button* Button::Loading(bool v) {
    loading = v;
    return this;
}
Button* Button::Disabled(bool v) {
    disabled = v;
    return this;
}
Button* Button::WithSize(UiSize s) {
    size = s;
    return this;
}
Button* Button::Tooltip(Str s) {
    tooltip = s;
    return this;
}
Button* Button::OnClick(Listener l) {
    onClick = l;
    return this;
}

El* Button::IntoEl() {
    const Theme& th = cx->theme();

    Rgba bg = th.background, fg = th.foreground,
         hover = RgbaOpacity(th.inputBorder, 0.5f), bd = th.inputBorder;

    Rgba accent = {};
    bool hasAccent = false;
    switch (variant) {
        case ButtonVariant::Secondary:
            bg = th.secondary;
            fg = th.secondaryFg;
            hover = th.secondaryHover;
            bd = th.border;
            break;
        case ButtonVariant::Primary:
            bg = th.primary;
            fg = th.primaryFg;
            hover = RgbaMix(th.primary, th.foreground, 0.85f);
            bd = th.primary;
            break;
        case ButtonVariant::Danger:
            accent = th.danger;
            hasAccent = true;
            break;
        case ButtonVariant::Success:
            accent = th.success;
            hasAccent = true;
            break;
        case ButtonVariant::Warning:
            accent = th.warning;
            hasAccent = true;
            break;
        case ButtonVariant::Info:
            accent = th.info;
            hasAccent = true;
            break;
        case ButtonVariant::Ghost:
        case ButtonVariant::Text:
            bg = Rgba8(0, 0, 0, 0);
            fg = th.foreground;
            hover = th.muted;
            bd = Rgba8(0, 0, 0, 0);
            break;
        case ButtonVariant::Link:
            bg = Rgba8(0, 0, 0, 0);
            fg = th.blue;
            hover = th.muted;
            bd = Rgba8(0, 0, 0, 0);
            break;
        default:
            break;
    }
    if (hasAccent) {
        bg = RgbaOpacity(accent, 0.2f);
        fg = accent;
        hover = RgbaOpacity(accent, 0.3f);
        bd = bg;
    }
    if (hasCustom) {
        fg = custom;
        bd = custom;
        bg = outline ? th.background : RgbaOpacity(custom, 0.12f);
        hover = RgbaOpacity(custom, 0.2f);
    }
    if (outline && !hasCustom) {
        if (hasAccent) {
            bg = RgbaOpacity(accent, 0.1f);
            bd = RgbaOpacity(accent, 0.6f);
            hover = RgbaOpacity(accent, 0.2f);
        } else if (variant == ButtonVariant::Primary) {
            bg = RgbaOpacity(th.primary, 0.1f);
            fg = th.primary;
            hover = RgbaOpacity(th.primary, 0.2f);
        } else {
            bg = th.background;
            hover = th.muted;
        }
    }
    if (selected) {
        bg = th.secondaryActive;
        hover = th.secondaryHover;
    }
    if (disabled) {
        fg = th.mutedFg;
    }

    float h = 32.f;
    float padX = compact ? 8.f : 10.f;
    if (size == UiSize::XSmall) {
        h = 20.f;
        padX = 4.f;
    } else if (size == UiSize::Small) {
        h = 24.f;
        padX = compact ? 6.f : 8.f;
    } else if (size == UiSize::Large) {
        padX = compact ? 8.f : 12.f;
    }
    if (variant == ButtonVariant::Text || variant == ButtonVariant::Link) {
        padX = 0;
        h = 0;
    }
    El* e = gpui::Button::New(cx, id, disabled ? 0 : HashClickId(id))
                ->H(h > 0 ? h : kAuto)
                ->PadX(padX)
                ->ItemsCenter()
                ->JustifyCenter()
                ->Gap(6)
                ->Radius(th.radius);
    if (bd.a) {
        e->Border(1, bd);
    }
    if (bg.a) {
        e->Bg(bg);
    }
    if (!disabled) {
        e->HoverBg(hover);
        if (onClick.IsValid()) {
            e->OnClick(onClick);
        }
    }
    if (tooltip.s) {
        e->Tip(tooltip);
    }
    if (extra) {
        e->Child(extra);
    } else if (loading) {
        e->Child(IconEl(a, IconName::Loader, 14)->Fg(fg));
    } else if (icon != IconName::None) {
        e->Child(IconEl(a, icon, 14)->Fg(fg));
    }
    if (label.s) {

        float fontPx = size == UiSize::XSmall  ? 12.f
                       : size == UiSize::Small ? 14.f
                                               : 16.f;
        e->Child(TextEl(a, label)->Font(fontPx)->Fg(fg));
    }
    if (dropdown) {

        if (bd.a) {
            e->Child(Div(a)->W(1)->H(kFill)->Bg(bd));
        }
        e->Child(IconEl(a, IconName::ChevronDown, 12)->Fg(fg));
    }
    return e;
}

}
}

#line 1 "src/component/Calendar.cpp"

namespace gpui {

namespace component {

Calendar* Calendar::New(Ctx* cx) {
    Arena* a = cx->a;
    Calendar* c = ArenaNew<Calendar>(a);
    c->a = a;
    c->cx = cx;
    return c;
}
Calendar* Calendar::Year(int y) {
    year = y;
    return this;
}
Calendar* Calendar::Month(int m) {
    month = m;
    return this;
}
Calendar* Calendar::Day(int d) {
    day = d;
    return this;
}
Calendar* Calendar::OnDay(Listener fn) {
    onDay = fn;
    return this;
}
Calendar* Calendar::OnPrev(Listener fn) {
    onPrev = fn;
    return this;
}
Calendar* Calendar::OnNext(Listener fn) {
    onNext = fn;
    return this;
}

static int Dim(int y, int m) {
    static const int k[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) {
        return 29;
    }
    return k[m];
}

static int Dow(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) {
        y -= 1;
    }
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void PrevMonth(int y, int m, int* py, int* pm) {
    *pm = m - 1;
    *py = y;
    if (*pm < 1) {
        *pm = 12;
        (*py)--;
    }
}

El* Calendar::IntoEl() {
    const Theme& th = cx->theme();
    static const char* mon[] = {"",        "January",   "February", "March",
                                "April",   "May",       "June",     "July",
                                "August",  "September", "October",  "November",
                                "December"};
    static const char* wd[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};

    const float kCell = 32.f;
    El* root = gpui::Calendar::New(cx, StrL("calendar"))
                   ->FlexCol()
                   ->W(288)
                   ->Pad(12)
                   ->Gap(2)
                   ->Border(1, th.border)
                   ->Radius(th.radiusLg);

    El* nav = Div(a)->FlexRow()->W(kFill)->JustifyBetween()->ItemsCenter();
    El* prev =
        Div(a)
            ->W(kCell)
            ->H(kCell)
            ->ItemsCenter()
            ->JustifyCenter()
            ->Radius(th.radius)
            ->HoverBg(th.secondaryHover)
            ->Child(IconEl(a, IconName::ChevronLeft, 16)->Fg(th.foreground));
    BindClick(prev, StrL("cal-prev"), onPrev);
    El* next =
        Div(a)
            ->W(kCell)
            ->H(kCell)
            ->ItemsCenter()
            ->JustifyCenter()
            ->Radius(th.radius)
            ->HoverBg(th.secondaryHover)
            ->Child(IconEl(a, IconName::ChevronRight, 16)->Fg(th.foreground));
    BindClick(next, StrL("cal-next"), onNext);
    El* toggles = Div(a)->FlexRow()->Grow()->JustifyCenter()->Gap(16);
    toggles->Child(Div(a)->PadX(8)->H(kCell)->ItemsCenter()->Child(
        TextEl(a, Str(mon[month]))->Font(14)->Semibold()->Fg(th.foreground)));
    toggles->Child(Div(a)->PadX(8)->H(kCell)->ItemsCenter()->Child(
        TextEl(a, StrDup(a, fmt("%d", year)))
            ->Font(14)
            ->Semibold()
            ->Fg(th.foreground)));
    nav->Child(prev)->Child(toggles)->Child(next);
    root->Child(nav);

    El* head = Div(a)->FlexRow()->W(kFill);
    for (int i = 0; i < 7; i++) {
        head->Child(
            Div(a)->W(kCell)->H(kCell)->ItemsCenter()->JustifyCenter()->Child(
                TextEl(a, Str(wd[i]))->Font(12)->Fg(th.mutedFg)));
    }
    root->Child(head);

    LocalDate now = DateToday();
    int dim = Dim(year, month);
    int lead = Dow(year, month, 1);
    int prevY = 0, prevM = 0;
    PrevMonth(year, month, &prevY, &prevM);
    int prevDim = Dim(prevY, prevM);

    El* grid = Div(a)->FlexCol()->Gap(2);
    for (int week = 0; week < 6; week++) {
        El* row = Div(a)->FlexRow();
        for (int col = 0; col < 7; col++) {
            int cellIx = week * 7 + col;
            int d = cellIx - lead + 1;
            bool muted = d < 1 || d > dim;
            int shown = d;
            if (d < 1) {
                shown = prevDim + d;
            } else if (d > dim) {
                shown = d - dim;
            }
            bool active = !muted && d == day;
            bool today = !muted && year == now.year && month == now.month &&
                         d == now.day;
            El* cell = CalendarItem::New(
                           cx, HashClickId(StrDup(a, fmt("d%d-%d", month, d))))
                           ->W(kCell)
                           ->H(kCell)
                           ->ItemsCenter()
                           ->JustifyCenter()
                           ->Radius(th.radius);
            Rgba fg = muted ? th.mutedFg : th.foreground;
            if (active) {
                cell->Bg(th.primary);
                fg = th.primaryFg;
            } else if (today) {
                cell->Bg(th.accent);
                fg = th.foreground;
            } else if (!muted) {
                cell->HoverBg(th.secondaryHover);
            }
            cell->Child(
                TextEl(a, StrDup(a, fmt("%d", shown)))->Font(14)->Fg(fg));
            if (!muted && onDay.IsValid()) {
                cell->OnClick(ListenerArg(onDay, d));
            }
            row->Child(cell);
        }
        grid->Child(row);
    }
    root->Child(grid);
    return root;
}

}
}

#line 1 "src/component/Chart.cpp"

namespace gpui {

namespace component {

AreaChart* AreaChart::New(Ctx* cx, const float* ys, int n) {
    Arena* a = cx->a;
    AreaChart* c = ArenaNew<AreaChart>(a);
    c->a = a;
    c->cx = cx;
    c->ys = ys;
    c->n = n;
    c->stroke = cx->theme().blue;
    c->fill = RgbaOpacity(cx->theme().blue, 0.25f);
    return c;
}
AreaChart* AreaChart::Stroke(Rgba c) {
    stroke = c;
    return this;
}
AreaChart* AreaChart::Fill(Rgba c) {
    fill = c;
    return this;
}

AreaChart* AreaChart::Labels(const char* const* l) {
    labels = l;
    return this;
}
AreaChart* AreaChart::TickMargin(int t) {
    tickMargin = t;
    return this;
}
AreaChart* AreaChart::Overlay(bool v) {
    overlay = v;
    return this;
}

El* AreaChart::IntoEl() {
    El* e =
        ChartEl(a, ys, n, stroke, fill, RgbaOpacity(fill, 0.0f), tickMargin);
    e->chart.labels = labels;
    e->chart.overlay = overlay;
    return e;
}

PieChart* PieChart::New(Ctx* cx) {
    Arena* a = cx->a;
    PieChart* p = ArenaNew<PieChart>(a);
    p->a = a;
    p->cx = cx;
    return p;
}
PieChart* PieChart::Slice(float value, Rgba color, float outerInset) {
    if (n < 12) {
        slices[n].value = value;
        slices[n].color = color;
        slices[n].outerInset = outerInset;
        n++;
    }
    return this;
}
PieChart* PieChart::OuterRadius(float r) {
    outerRadius = r;
    return this;
}
PieChart* PieChart::InnerRadius(float r) {
    innerRadius = r;
    return this;
}
PieChart* PieChart::PadAngle(float radians) {
    padAngle = radians;
    return this;
}

static void PaintPie(PaintCtx* ctx, El* e, void* user) {
    auto* p = (PieChart*)user;
    if (!p || !ctx->rt || p->n == 0) {
        return;
    }
    float cx = e->x + e->w * 0.5f;
    float cy = e->y + e->h * 0.5f;
    float total = 0;
    for (int i = 0; i < p->n; i++) {
        total += p->slices[i].value;
    }
    if (total <= 0) {
        return;
    }
    float angle = -kPi * 0.5f;
    for (int i = 0; i < p->n; i++) {
        const PieSlice& s = p->slices[i];
        float sweep = 2.f * kPi * (s.value / total) - p->padAngle;
        if (sweep <= 0) {
            angle += 2.f * kPi * (s.value / total);
            continue;
        }
        float ro = p->outerRadius - s.outerInset;
        float ri = p->innerRadius;
        float a0 = angle, a1 = angle + sweep;
        Path* wedge = PathNew(ctx, true);
        if (wedge) {
            PathArcTo(wedge, cx, cy, ro, a0, a1, true);
            if (ri > 0) {

                PathArcTo(wedge, cx, cy, ri, a1, a0, false);
            } else {
                PathLineTo(wedge, cx, cy);
            }
            PathClose(wedge);
            PathFill(ctx, wedge, s.color);
            PathFree(wedge);
        }
        angle += 2.f * kPi * (s.value / total);
    }
}

El* PieChart::IntoEl() {
    float d = outerRadius * 2;
    El* e = Div(a)->W(d)->H(d);
    e->customPaint = PaintPie;
    e->customUser = this;
    return e;
}

}
}

#line 1 "src/component/Checkbox.cpp"

namespace gpui {

namespace component {

Checkbox* Checkbox::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    Checkbox* c = ArenaNew<Checkbox>(a);
    c->a = a;
    c->cx = cx;
    c->id = id;
    return c;
}

Checkbox* Checkbox::Label(Str s) {
    label = s;
    return this;
}
Checkbox* Checkbox::Hint(Str s) {
    hint = s;
    return this;
}
Checkbox* Checkbox::Checked(bool v) {
    checked = v;
    return this;
}
Checkbox* Checkbox::Disabled(bool v) {
    disabled = v;
    return this;
}
Checkbox* Checkbox::WithSize(UiSize s) {
    size = s;
    return this;
}
Checkbox* Checkbox::W(float v) {
    w = v;
    return this;
}
Checkbox* Checkbox::Tooltip(Str s) {
    tooltip = s;
    return this;
}
Checkbox* Checkbox::OnClick(Listener fn) {
    onClick = fn;
    return this;
}

El* Checkbox::IntoEl() {
    const Theme& th = cx->theme();
    float box = size == UiSize::Small ? 14.f : 16.f;

    Rgba mark = checked ? th.primary : th.inputBorder;
    if (disabled) {
        mark = RgbaOpacity(mark, 0.5f);
    }
    float radius = th.radius < 4.f ? th.radius : 4.f;
    El* ind = CheckboxIndicator::New(cx)
                  ->W(box)
                  ->H(box)
                  ->Shrink0()
                  ->ItemsCenter()
                  ->JustifyCenter()
                  ->Border(1, mark)
                  ->Radius(radius);
    if (checked) {
        Rgba tick = disabled ? RgbaOpacity(th.primaryFg, 0.5f) : th.primaryFg;
        ind->Bg(mark)->Child(IconEl(a, IconName::Check, box - 4)->Fg(tick));
    }
    El* row = gpui::Checkbox::New(cx, id, disabled ? 0 : HashClickId(id))
                  ->FlexRow()
                  ->ItemsCenter()
                  ->Gap(8);
    if (onClick.IsValid() && !disabled) {
        row->OnClick(ListenerArg(onClick, !checked));
    }
    if (tooltip.s) {
        row->Tip(tooltip);
    }
    row->Child(ind);
    if (w > 0) {
        row->W(w);
    }
    if (label.s || hint.s) {
        El* col = Div(a)->FlexCol()->Gap(2);
        if (label.s) {
            col->Child(TextEl(a, label)
                           ->Font(UiFontPx(size))
                           ->Fg(disabled ? th.mutedFg : th.foreground)
                           ->Wrap());
        }
        if (hint.s) {
            col->Child(TextEl(a, hint)->Font(12)->Fg(th.mutedFg)->Wrap());
        }
        row->Child(col);
    }
    return row;
}

}
}

#line 1 "src/component/Clipboard.cpp"

namespace gpui {

namespace component {

Clipboard* Clipboard::New(Ctx* cx, Str value) {
    Arena* a = cx->a;
    Clipboard* c = ArenaNew<Clipboard>(a);
    c->a = a;
    c->cx = cx;
    c->value = value;
    return c;
}
Clipboard* Clipboard::OnCopy(Listener fn) {
    onCopy = fn;
    return this;
}

El* Clipboard::IntoEl() {

    Button* btn = Button::New(cx, StrL("clipboard"))
                      ->Icon(IconName::Copy)
                      ->Ghost()
                      ->WithSize(UiSize::XSmall)
                      ->Tooltip(StrL("Copy"));
    if (onCopy.IsValid()) {
        btn->OnClick(onCopy);
    }
    return btn->IntoEl();
}

}
}

#line 1 "src/component/Collapsible.cpp"

namespace gpui {

namespace component {

Collapsible* Collapsible::New(Ctx* cx) {
    Arena* a = cx->a;
    Collapsible* c = ArenaNew<Collapsible>(a);
    c->a = a;
    c->cx = cx;
    return c;
}

Collapsible* Collapsible::Open(bool v) {
    open = v;
    return this;
}
Collapsible* Collapsible::Trigger(El* e) {
    trigger = e;
    return this;
}
Collapsible* Collapsible::Content(El* e) {
    content = e;
    return this;
}

El* Collapsible::IntoEl() {
    return gpui::Collapsible::New(cx)
        ->Open(open)
        ->Child(trigger)
        ->Content(content)
        ->IntoEl();
}

}
}

#line 1 "src/component/ColorPicker.cpp"

namespace gpui {

namespace component {

ColorPicker* ColorPicker::New(Ctx* cx) {
    Arena* a = cx->a;
    ColorPicker* c = ArenaNew<ColorPicker>(a);
    c->a = a;
    c->cx = cx;
    return c;
}
ColorPicker* ColorPicker::Hex(uint32_t h) {
    hex = h;
    hasValue = true;
    return this;
}
ColorPicker* ColorPicker::Label(Str s) {
    label = s;
    return this;
}
ColorPicker* ColorPicker::WithSize(UiSize s) {
    size = s;
    return this;
}
ColorPicker* ColorPicker::Open(bool v) {
    open = v;
    return this;
}
ColorPicker* ColorPicker::OnChange(Listener fn) {
    onChange = fn;
    return this;
}
ColorPicker* ColorPicker::OnToggle(Listener fn) {
    onToggle = fn;
    return this;
}

El* ColorPicker::IntoEl() {
    const Theme& th = cx->theme();
    Rgba c = Rgb((uint8_t)((hex >> 16) & 0xff), (uint8_t)((hex >> 8) & 0xff),
                 (uint8_t)(hex & 0xff));

    float sq = 32;
    if (size == UiSize::Large) {
        sq = 44;
    } else if (size == UiSize::Small) {
        sq = 20;
    } else if (size == UiSize::XSmall) {
        sq = 16;
    }
    El* swatch = Div(a)
                     ->W(sq)
                     ->H(sq)
                     ->Radius(th.radius)
                     ->Bg(hasValue ? c : th.background)

                     ->Border(1, hasValue ? RgbaMix(c, Rgb(0, 0, 0), 0.3f)
                                          : th.inputBorder);
    El* trigger = Div(a)->FlexRow()->Gap(8)->ItemsCenter()->Child(swatch);
    if (label.s) {
        trigger->Child(TextEl(a, label)->Font(16)->Fg(th.foreground));
    }
    BindClick(trigger, StrL("color-trigger"), onToggle);
    El* pop = nullptr;
    if (open) {
        static const uint32_t sw[] = {0xdc2626, 0xd97706, 0x16a34a, 0x2563eb,
                                      0x7c3aed};
        pop = Div(a)
                  ->FlexRow()
                  ->Gap(4)
                  ->Pad(8)
                  ->Border(1, th.foreground)
                  ->Bg(th.background);
        for (int i = 0; i < 5; i++) {
            Rgba sc =
                Rgb((uint8_t)((sw[i] >> 16) & 0xff),
                    (uint8_t)((sw[i] >> 8) & 0xff), (uint8_t)(sw[i] & 0xff));
            El* cell = ColorSwatch::New(cx, StrDup(a, fmt("sw%d", i)))
                           ->W(24)
                           ->H(24)
                           ->Bg(sc);
            if (onChange.IsValid()) {
                BindClick(cell, StrDup(a, fmt("sw%d", i)),
                          ListenerArg(onChange, (intptr_t)sw[i]));
            }
            pop->Child(cell);
        }
    }
    El* root = gpui::ColorPicker::New(cx, StrL("color-picker"))->Child(trigger);
    return Popup::New(cx, StrL("color-pop"), root)->Content(pop)->IntoEl();
}

}
}

#line 1 "src/component/Combobox.cpp"

namespace gpui {

namespace component {

Combobox* Combobox::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    Combobox* c = ArenaNew<Combobox>(a);
    c->a = a;
    c->cx = cx;
    c->id = id;
    return c;
}
Combobox* Combobox::Option(Str s) {
    if (n < 12) {
        options[n++] = s;
    }
    return this;
}
Combobox* Combobox::Options(const char* const* items, int count) {
    for (int i = 0; i < count; i++) {
        Option(Str(items[i]));
    }
    return this;
}
Combobox* Combobox::Selected(Str s) {
    selected = s;
    return this;
}
Combobox* Combobox::Placeholder(Str s) {
    placeholder = s;
    return this;
}
Combobox* Combobox::SearchPlaceholder(Str s) {
    searchPlaceholder = s;
    return this;
}
Combobox* Combobox::Icon(IconName i) {
    icon = i;
    return this;
}
Combobox* Combobox::W(float v) {
    width = v;
    return this;
}
Combobox* Combobox::Open(bool v) {
    open = v;
    return this;
}
Combobox* Combobox::Query(LineInput* q) {
    query = q;
    return this;
}
Combobox* Combobox::OnChange(Listener fn) {
    onChange = fn;
    return this;
}
Combobox* Combobox::OnToggle(Listener fn) {
    onToggle = fn;
    return this;
}

El* Combobox::IntoEl() {
    const Theme& th = cx->theme();

    bool hasValue = selected.s != nullptr;
    El* trigger = Div(a)
                      ->FlexRow()
                      ->W(width)
                      ->H(32)
                      ->PadX(10)
                      ->Gap(8)
                      ->ItemsCenter()
                      ->JustifyBetween()
                      ->Radius(th.radius)
                      ->Bg(th.inputBg)
                      ->Border(1, open ? th.ring : th.inputBorder);
    El* title = Div(a)->FlexRow()->Gap(8)->ItemsCenter();

    if (icon != IconName::None && hasValue) {
        title->Child(IconEl(a, icon, 14)->Fg(th.mutedFg));
    }
    title->Child(TextEl(a, hasValue ? selected : placeholder)
                     ->Font(14)
                     ->Fg(hasValue ? th.foreground : th.mutedFg));
    trigger->Child(title);
    trigger->Child(IconEl(a, IconName::ChevronDown, 16)->Fg(th.mutedFg));
    BindClick(trigger, id, onToggle);

    El* pop = nullptr;
    if (open) {
        pop = Div(a)
                  ->FlexCol()
                  ->W(width)
                  ->Pad(4)
                  ->Gap(2)
                  ->Radius(th.radiusLg)
                  ->Border(1, th.border)
                  ->Bg(th.background);
        if (query) {
            El* search = Div(a)
                             ->FlexRow()
                             ->W(kFill)
                             ->H(32)
                             ->PadX(8)
                             ->Gap(8)
                             ->ItemsCenter()
                             ->BorderB(1, th.border);
            search->Child(IconEl(a, IconName::Search, 14)->Fg(th.mutedFg));
            search->Child(Div(a)->Grow()->Child(gpui::Input::New(cx, query)));
            pop->Child(search);
        }
        for (int i = 0; i < n; i++) {
            El* row = Div(a)
                          ->FlexRow()
                          ->W(kFill)
                          ->H(28)
                          ->PadX(8)
                          ->ItemsCenter()
                          ->JustifyBetween()
                          ->Radius(th.radius)
                          ->HoverBg(th.accent);
            row->Child(TextEl(a, options[i])->Font(14)->Fg(th.foreground));
            if (selected.s && StrEqI(options[i], selected)) {
                row->Child(IconEl(a, IconName::Check, 14)->Fg(th.foreground));
            }
            if (onChange.IsValid()) {
                BindClick(row, StrDup(a, fmt("%s-opt%d", id, i)),
                          ListenerArg(onChange, i));
            }
            pop->Child(row);
        }
    }
    El* root = gpui::Combobox::New(cx, id)->W(width)->Child(trigger);
    return Popup::New(cx, StrDup(a, fmt("%s-pop", id)), root)
        ->Content(pop)
        ->IntoEl();
}

}
}

#line 1 "src/component/DatePicker.cpp"

namespace gpui {

namespace component {

DatePicker* DatePicker::New(Ctx* cx) {
    Arena* a = cx->a;
    DatePicker* d = ArenaNew<DatePicker>(a);
    d->a = a;
    d->cx = cx;
    return d;
}
DatePicker* DatePicker::Year(int y) {
    year = y;
    return this;
}
DatePicker* DatePicker::Month(int m) {
    month = m;
    return this;
}
DatePicker* DatePicker::Day(int d) {
    day = d;
    return this;
}
DatePicker* DatePicker::RangeEnd(int y, int m, int d) {
    year2 = y;
    month2 = m;
    day2 = d;
    return this;
}
DatePicker* DatePicker::Format(DateFormat f) {
    format = f;
    return this;
}
DatePicker* DatePicker::W(float v) {
    width = v;
    return this;
}
DatePicker* DatePicker::Cleanable(bool v) {
    cleanable = v;
    return this;
}
DatePicker* DatePicker::Appearance(bool v) {
    appearance = v;
    return this;
}
DatePicker* DatePicker::OnClear(Listener fn) {
    onClear = fn;
    return this;
}
DatePicker* DatePicker::Placeholder(Str s) {
    placeholder = s;
    return this;
}
DatePicker* DatePicker::Open(bool v) {
    open = v;
    return this;
}
DatePicker* DatePicker::OnToggle(Listener fn) {
    onToggle = fn;
    return this;
}
DatePicker* DatePicker::OnDay(Listener fn) {
    onDay = fn;
    return this;
}

static Str FormatDate(Arena* a, DateFormat f, int y, int m, int d) {
    const char* sep = f == DateFormat::Dash ? "-" : "/";
    return StrDup(a, fmt("%d%s%02d%s%02d", y, Str(sep), m, Str(sep), d));
}

El* DatePicker::IntoEl() {
    const Theme& th = cx->theme();
    bool hasDate = day > 0;
    Str title;
    if (!hasDate) {
        title = placeholder.s ? placeholder : StrL("Select date");
    } else if (year2 > 0) {
        title =
            StrDup(a, fmt("%s - %s", FormatDate(a, format, year, month, day),
                          FormatDate(a, format, year2, month2, day2)));
    } else {
        title = FormatDate(a, format, year, month, day);
    }

    El* trigger = Div(a)
                      ->FlexRow()
                      ->W(width)
                      ->H(32)
                      ->PadX(10)
                      ->Gap(4)
                      ->ItemsCenter()
                      ->JustifyBetween();
    if (appearance) {
        trigger->Radius(th.radius)->Bg(th.inputBg)->Border(1, th.inputBorder);
    }
    trigger->Child(
        TextEl(a, title)->Font(14)->Fg(hasDate ? th.foreground : th.mutedFg));
    if (cleanable && hasDate) {
        trigger->Child(Button::New(cx, StrL("date-clean"))
                           ->Text()
                           ->WithSize(UiSize::XSmall)
                           ->Icon(IconName::X)
                           ->OnClick(onClear)
                           ->IntoEl());
    } else {
        trigger->Child(IconEl(a, IconName::Calendar, 12)->Fg(th.mutedFg));
    }
    BindClick(trigger, StrL("date"), onToggle);
    El* cal = nullptr;
    if (open) {
        cal = Calendar::New(cx)
                  ->Year(year)
                  ->Month(month)
                  ->Day(day)
                  ->OnDay(onDay)
                  ->IntoEl();
    }
    return gpui::DatePicker::New(cx, StrL("date-picker"))
        ->W(width)
        ->Child(
            Popup::New(cx, StrL("date-pop"), trigger)->Content(cal)->IntoEl());
}

}
}

#line 1 "src/component/DescriptionList.cpp"

namespace gpui {

namespace component {

DescriptionList* DescriptionList::New(Ctx* cx) {
    Arena* a = cx->a;
    DescriptionList* d = ArenaNew<DescriptionList>(a);
    d->a = a;
    d->cx = cx;
    return d;
}

DescriptionList* DescriptionList::Item(Str label, Str value, int span) {
    return ItemEl(label, TextEl(a, value)->Font(14)->Wrap(), span);
}

DescriptionList* DescriptionList::ItemEl(Str label, El* value, int span) {
    if (n < 16) {
        items[n].label = label;
        items[n].value = value;
        items[n].span = span < 1 ? 1 : span;
        n++;
    }
    return this;
}

DescriptionList* DescriptionList::Separator() {
    if (n < 16) {
        items[n].separator = true;
        n++;
    }
    return this;
}

DescriptionList* DescriptionList::Columns(int v) {
    columns = v < 1 ? 1 : v;
    return this;
}
DescriptionList* DescriptionList::LabelWidth(float w) {
    labelWidth = w;
    return this;
}
DescriptionList* DescriptionList::Bordered(bool v) {
    bordered = v;
    return this;
}
DescriptionList* DescriptionList::WithSize(UiSize s) {
    size = s;
    return this;
}

El* DescriptionList::IntoEl() {
    const Theme& th = cx->theme();

    float padX = 8, padY = 4;
    if (size == UiSize::Small || size == UiSize::XSmall) {
        padX = 4;
        padY = 2;
    } else if (size == UiSize::Large) {
        padX = 12;
        padY = 6;
    }
    float gap = bordered ? 0.f : padY;
    if (!bordered) {
        padX = 0;
        padY = 0;
    }

    El* root = Div(a)->FlexCol()->W(kFill)->Gap(gap)->ClipY();
    if (bordered) {
        root->Border(1, th.border)->Radius(padX);
    }

    int i = 0;
    while (i < n) {
        int used = 0;
        int count = 0;
        while (i < n && used < columns) {
            if (items[i].separator) {
                i++;
                break;
            }
            if (used + items[i].span > columns && count > 0) {
                break;
            }
            count++;
            used += items[i].span;
            i++;
        }
        if (count == 0) {
            continue;
        }
        int first = i - count;
        El* row = Div(a)->FlexRow()->W(kFill);
        bool last = i >= n;
        if (bordered && !last) {
            row->BorderB(1, th.border);
        }
        for (int k = 0; k < count; k++) {
            const DescriptionItem& it = items[first + k];
            El* cell = Div(a)->FlexRow()->Grow((float)it.span);
            El* label = Div(a)
                            ->W(labelWidth)
                            ->Shrink0()
                            ->PadX(padX)
                            ->PadY(padY)
                            ->Child(TextEl(a, it.label)
                                        ->Font(14)
                                        ->Wrap()
                                        ->Fg(th.descListLabelFg));
            if (bordered) {
                label->Bg(th.descListLabel)->Border(1, th.border);
            }
            El* value = Div(a)->Grow()->PadX(padX)->PadY(padY)->ClipY();
            if (it.value) {
                value->Child(it.value);
            }
            cell->Child(label)->Child(value);
            row->Child(cell);
        }
        root->Child(row);
    }
    return root;
}

}
}

#line 1 "src/component/Dialog.cpp"

namespace gpui {

namespace component {

Dialog* Dialog::New(Ctx* cx) {
    Arena* a = cx->a;
    Dialog* d = ArenaNew<Dialog>(a);
    d->a = a;
    d->cx = cx;
    return d;
}
Dialog* Dialog::Title(Str s) {
    title = s;
    return this;
}
Dialog* Dialog::Description(Str s) {
    description = s;
    return this;
}
Dialog* Dialog::Open(bool v) {
    open = v;
    return this;
}
Dialog* Dialog::Body(El* e) {
    body = e;
    return this;
}
Dialog* Dialog::W(float px) {
    width = px;
    return this;
}
Dialog* Dialog::Overlay(bool v) {
    overlay = v;
    return this;
}
Dialog* Dialog::Icon(IconName n, Rgba color, float size) {
    icon = n;
    iconColor = color;
    hasIconColor = true;
    iconSize = size;
    return this;
}
Dialog* Dialog::HeaderCentered(bool v) {
    headerCentered = v;
    return this;
}
Dialog* Dialog::OkText(Str s) {
    okText = s;
    return this;
}
Dialog* Dialog::CancelText(Str s) {
    cancelText = s;
    return this;
}
Dialog* Dialog::OkVariant(ButtonVariant v, bool outline) {
    okVariant = v;
    okOutline = outline;
    return this;
}
Dialog* Dialog::ShowCancel(bool v) {
    showCancel = v;
    return this;
}
Dialog* Dialog::CloseButton(bool v) {
    closeButton = v;
    return this;
}
Dialog* Dialog::Footer(El* e) {
    footer = e;
    return this;
}
Dialog* Dialog::FooterVertical(bool v) {
    footerVertical = v;
    return this;
}
Dialog* Dialog::FooterStretch(bool v) {
    footerStretch = v;
    return this;
}
Dialog* Dialog::FooterMuted(bool v) {
    footerMuted = v;
    return this;
}
Dialog* Dialog::FooterDivider(bool v) {
    footerDivider = v;
    return this;
}
Dialog* Dialog::OnClose(Listener fn) {
    onClose = fn;
    return this;
}
Dialog* Dialog::OnCancel(Listener fn) {
    onCancel = fn;
    return this;
}
Dialog* Dialog::OnOk(Listener fn) {
    onOk = fn;
    return this;
}

El* Dialog::Header() {
    const Theme& th = cx->theme();
    El* head = Div(a)->FlexCol()->W(kFill)->Pad(16)->Gap(8);
    El* ic = nullptr;
    if (icon != IconName::None) {
        ic = IconEl(a, icon, iconSize)->Shrink0();
        if (hasIconColor) {
            ic->Fg(iconColor);
        }
    }
    if (headerCentered) {
        head->ItemsCenter();
        if (ic) {
            head->Child(ic);
        }
        ic = nullptr;
    }
    if (title.s && title.len > 0) {
        El* text = TextEl(a, title)->Font(16)->Semibold()->Fg(th.foreground);
        El* line = text;
        if (ic) {
            line = Div(a)->FlexRow()->Gap(8)->ItemsCenter()->Child(ic)->Child(
                text);
        }
        head->Child(DialogTitle::New(cx)->Child(line));
    } else if (ic) {
        head->Child(ic);
    }
    if (description.s && description.len > 0) {
        head->Child(DialogDescription::New(cx)->Child(TextEl(a, description)
                                                          ->Font(13)
                                                          ->Fg(th.mutedFg)
                                                          ->Wrap()
                                                          ->W(kFill)));
    }
    if (body) {
        head->Child(body);
    }
    return head;
}

El* Dialog::Actions() {
    const Theme& th = cx->theme();
    El* row = Div(a)->W(kFill)->Pad(16)->Gap(8);
    if (footerVertical) {
        row->FlexCol();
    } else {
        row->FlexRow()->JustifyEnd();
    }
    if (footerMuted) {
        row->Bg(th.muted);
    }
    if (footerDivider) {
        row->BorderT(1, th.border);
    }
    if (footer) {
        row->Child(footer);
        return row;
    }

    El* cancel = nullptr;
    if (showCancel) {
        cancel = Button::New(cx, StrL("dialog-cancel"))
                     ->Label(cancelText.s ? cancelText : StrL("Cancel"))
                     ->Outline()
                     ->OnClick(onCancel.IsValid() ? onCancel : onClose)
                     ->IntoEl();
    }
    Button* okBtn = Button::New(cx, StrL("dialog-ok"))
                        ->Label(okText.s ? okText : StrL("OK"))
                        ->OnClick(onOk);
    switch (okVariant) {
        case ButtonVariant::Danger:
            okBtn->Danger();
            break;
        case ButtonVariant::Default:
            break;
        default:
            okBtn->Primary();
            break;
    }
    if (okOutline) {
        okBtn->Outline();
    }
    El* ok = okBtn->IntoEl();

    if (footerVertical) {
        row->Child(ok->W(kFill));
        if (cancel) {
            row->Child(cancel->W(kFill));
        }
        return row;
    }
    if (cancel) {
        row->Child(footerStretch ? cancel->Grow() : cancel);
    }
    row->Child(footerStretch ? ok->Grow() : ok);
    return row;
}

El* Dialog::IntoEl(WinSize size) {
    if (!open) {
        return Div(a);
    }
    const Theme& th = cx->theme();

    El* panel = Div(a)
                    ->W(width)
                    ->FlexCol()
                    ->Bg(th.background)
                    ->Border(1, th.border)
                    ->Radius(th.radius)
                    ->ClipY();
    panel->Child(Header());
    panel->Child(Actions());
    if (closeButton) {
        El* x = Div(a)
                    ->Absolute()
                    ->Top(12)
                    ->Right(12)
                    ->W(20)
                    ->H(20)
                    ->ItemsCenter()
                    ->JustifyCenter()
                    ->Radius(th.radius)
                    ->HoverBg(th.secondaryHover)
                    ->Child(IconEl(a, IconName::X, 14)->Fg(th.mutedFg));
        BindClick(x, StrL("dialog-close-x"), onClose);
        panel->Child(x);
    }

    El* backdrop =
        DialogBackdrop::New(cx)->Fixed()->Top(0)->Left(0)->W(kFill)->H(kFill);
    if (overlay) {
        backdrop->Bg(th.overlay);
    }
    if (onClose.IsValid()) {
        backdrop->OnClick(onClose)->Click(HashClickId(StrL("dialog-backdrop")));
    }

    El* popup = DialogPopup::New(cx)
                    ->Fixed()
                    ->Top(0)
                    ->Left(0)
                    ->W(kFill)
                    ->H(kFill)
                    ->FlexCol()
                    ->ItemsCenter()
                    ->PadT(size.dipH * 0.1f)
                    ->Child(panel);
    return gpui::Dialog::New(cx)->Backdrop(backdrop)->Popup(popup)->IntoEl();
}

}
}

#line 1 "src/component/Dock.cpp"

namespace gpui {

namespace component {

Dock* Dock::New(Ctx* cx) {
    Arena* a = cx->a;
    Dock* d = ArenaNew<Dock>(a);
    d->a = a;
    d->cx = cx;
    return d;
}
Dock* Dock::Left(El* e) {
    left = e;
    return this;
}
Dock* Dock::Center(El* e) {
    center = e;
    return this;
}
Dock* Dock::Right(El* e) {
    right = e;
    return this;
}

El* Dock::IntoEl() {
    El* row = Div(a)->FlexRow()->SizeFull();
    if (left) {
        row->Child(left);
    }
    if (center) {
        row->Child(Div(a)->Grow()->H(kFill)->Child(center));
    }
    if (right) {
        row->Child(right);
    }
    return row;
}

}
}

#line 1 "src/component/Form.cpp"

namespace gpui {

namespace component {

Form* Form::New(Ctx* cx) {
    Arena* a = cx->a;
    Form* f = ArenaNew<Form>(a);
    f->a = a;
    f->cx = cx;
    return f;
}
Form* Form::Field(Str label, El* control) {
    if (n < 12) {
        fields[n].label = label;
        fields[n].control = control;
        n++;
    }
    return this;
}

Form* Form::Required(bool v) {
    if (n > 0) {
        fields[n - 1].required = v;
    }
    return this;
}
Form* Form::Description(Str s) {
    if (n > 0) {
        fields[n - 1].description = s;
    }
    return this;
}
Form* Form::SpanAll(bool v) {
    if (n > 0) {
        fields[n - 1].spanAll = v;
    }
    return this;
}

Form* Form::Horizontal(bool v) {
    horizontal = v;
    return this;
}
Form* Form::Columns(int c) {
    columns = c < 1 ? 1 : c;
    return this;
}
Form* Form::LabelWidth(float w) {
    labelWidth = w;
    return this;
}

El* Form::IntoEl() {
    const Theme& th = cx->theme();

    const float kGap = 8;
    const float kFieldGap = 4;
    float inner = horizontal ? kFieldGap : kFieldGap * 0.5f;
    float lw = labelWidth > 0 ? labelWidth : (columns > 1 ? 100.f : 140.f);

    El* col = Div(a)->FlexCol()->W(kFill)->Gap(kGap);
    El* row = nullptr;
    int inRow = 0;
    for (int i = 0; i < n; i++) {
        const FormField& fld = fields[i];
        El* f = Div(a)->FlexCol()->W(kFill)->Gap(kFieldGap * 0.5f);

        El* head = Div(a)->W(kFill)->Gap(inner);
        if (horizontal) {
            head->FlexRow()->ItemsCenter();
        } else {
            head->FlexCol();
        }
        if (fld.label.s) {
            El* label = Div(a)->FlexRow()->W(lw)->Gap(4)->ItemsCenter();
            label->Child(
                TextEl(a, fld.label)->Font(14)->Medium()->Fg(th.foreground));
            if (fld.required) {
                label->Child(TextEl(a, StrL("*"))->Font(14)->Fg(th.danger));
            }
            head->Child(label);
        }
        El* control = Div(a)->W(kFill)->Grow();
        if (fld.control) {
            control->Child(fld.control);
        }
        head->Child(control);
        f->Child(head);

        El* desc = Div(a)->FlexRow()->W(kFill)->Gap(inner);
        if (fld.description.s) {

            if (horizontal && fld.label.s) {
                desc->Child(Div(a)->W(lw));
            }
            desc->Child(TextEl(a, fld.description)->Font(12)->Fg(th.mutedFg));
        }
        f->Child(desc);

        if (columns <= 1) {
            col->Child(f);
            continue;
        }

        if (fld.spanAll) {
            row = nullptr;
            inRow = 0;
            col->Child(f);
            continue;
        }
        if (!row) {
            row = Div(a)->FlexRow()->W(kFill)->Gap(kGap * 3)->ItemsStart();
            col->Child(row);
            inRow = 0;
        }
        row->Child(Div(a)->Grow()->Child(f));
        inRow++;
        if (inRow >= columns) {
            row = nullptr;
        }
    }

    if (row) {
        for (int i = inRow; i < columns; i++) {
            row->Child(Div(a)->Grow());
        }
    }
    return col;
}

}
}

#line 1 "src/component/GroupBox.cpp"

namespace gpui {

namespace component {

GroupBox* GroupBox::New(Ctx* cx, Str title) {
    Arena* a = cx->a;
    GroupBox* g = ArenaNew<GroupBox>(a);
    g->a = a;
    g->cx = cx;
    g->title = title;
    return g;
}
GroupBox* GroupBox::Child(El* e) {
    child = e;
    return this;
}
GroupBox* GroupBox::Outline() {
    outline = true;
    filled = false;
    return this;
}
GroupBox* GroupBox::Filled(bool v) {
    filled = v;
    return this;
}
GroupBox* GroupBox::TitleSemibold(bool v) {
    titleSemibold = v;
    return this;
}
GroupBox* GroupBox::TitlePadX(float px) {
    titlePadX = px;
    return this;
}
GroupBox* GroupBox::ContentBg(Rgba c) {
    contentBg = c;
    hasContentBg = true;
    return this;
}
GroupBox* GroupBox::ContentRadius(float px) {
    contentRadius = px;
    return this;
}
GroupBox* GroupBox::ContentPad(float px) {
    contentPad = px;
    return this;
}
GroupBox* GroupBox::ContentBorder(float px) {
    contentBorder = px;
    return this;
}

El* GroupBox::IntoEl() {
    const Theme& th = cx->theme();

    bool padded = filled || outline;
    El* box = Div(a)->FlexCol()->W(kFill)->Gap(padded ? 12.f : 16.f);
    if (title.s && title.len > 0) {
        El* text = TextEl(a, title)->Font(14)->Fg(th.mutedFg)->LineHeight(1.f);
        if (titleSemibold) {
            text->Semibold();
        }
        box->Child(titlePadX > 0 ? Div(a)->PadX(titlePadX)->Child(text) : text);
    }
    El* content = Div(a)->FlexCol()->W(kFill)->Gap(16)->Fg(th.groupBoxFg);
    content->Radius(contentRadius >= 0 ? contentRadius : th.radius);
    if (filled) {
        content->Bg(th.groupBox);
    }
    if (outline) {
        content->Border(1, th.border);
    }
    if (padded) {
        content->Pad(16);
    }
    if (hasContentBg) {
        content->Bg(contentBg);
    }
    if (contentPad >= 0) {
        content->Pad(contentPad);
    }
    if (contentBorder >= 0) {
        content->Border(contentBorder, th.border);
    }
    if (child) {
        content->Child(child);
    }
    box->Child(content);
    return box;
}

}
}

#line 1 "src/component/Highlighter.cpp"

namespace gpui {

namespace component {

Highlighter* Highlighter::New(Ctx* cx, const char* text) {
    Arena* a = cx->a;
    Highlighter* h = ArenaNew<Highlighter>(a);
    h->a = a;
    h->cx = cx;
    h->text = text;
    return h;
}

El* Highlighter::IntoEl() {
    return gpui::Editor::New(cx, text);
}

}
}

#line 1 "src/component/History.cpp"

namespace gpui {

namespace component {

void History::Push(Str s) {
    if (n < 32) {
        cursor++;
        n = cursor + 1;
        items[cursor] = s;
    }
}
bool History::CanUndo() const {
    return cursor > 0;
}
bool History::CanRedo() const {
    return cursor + 1 < n;
}
Str History::Undo() {
    if (CanUndo()) {
        cursor--;
    }
    return cursor >= 0 ? items[cursor] : Str{};
}
Str History::Redo() {
    if (CanRedo()) {
        cursor++;
    }
    return cursor >= 0 ? items[cursor] : Str{};
}

}
}

#line 1 "src/component/HoverCard.cpp"

namespace gpui {

namespace component {

HoverCard* HoverCard::New(Ctx* cx) {
    Arena* a = cx->a;
    HoverCard* h = ArenaNew<HoverCard>(a);
    h->a = a;
    h->cx = cx;
    return h;
}
HoverCard* HoverCard::Trigger(El* e) {
    trigger = e;
    return this;
}
HoverCard* HoverCard::Content(El* e) {
    content = e;
    return this;
}
HoverCard* HoverCard::Open(bool v) {
    open = v;
    return this;
}
HoverCard* HoverCard::New(Ctx* cx, Str id) {
    HoverCard* h = New(cx);
    h->id = id;
    return h;
}
HoverCard* HoverCard::Anchor(HoverCardAnchor v) {
    anchor = v;
    return this;
}

El* HoverCard::IntoEl() {
    El* card = open ? content : nullptr;
    if (card) {

        const float kGap = 4.f;
        switch (anchor) {
            case HoverCardAnchor::BottomCenter:
                card->AnchorBelow(kGap)->AnchorCenterX();
                break;
            case HoverCardAnchor::BottomRight:
                card->AnchorBelow(kGap)->Right(0);
                break;
            case HoverCardAnchor::TopLeft:
                card->AnchorAbove(kGap)->Left(0);
                break;
            case HoverCardAnchor::TopCenter:
                card->AnchorAbove(kGap)->AnchorCenterX();
                break;
            case HoverCardAnchor::TopRight:
                card->AnchorAbove(kGap)->Right(0);
                break;
            default:
                card->AnchorBelow(kGap)->Left(0);
                break;
        }
        card->Deferred();
    }
    return gpui::HoverCard::New(cx, id.s ? id : StrL("hover-card"))
        ->Trigger(trigger)
        ->Content(card)
        ->IntoEl();
}

}
}

#line 1 "src/component/Icon.cpp"

namespace gpui {

namespace component {

Icon* Icon::New(Ctx* cx, IconName name) {
    Arena* a = cx->a;
    Icon* i = ArenaNew<Icon>(a);
    i->a = a;
    i->cx = cx;
    i->name = name;
    return i;
}
Icon* Icon::Size(float v) {
    size = v;
    return this;
}
Icon* Icon::Color(Rgba c) {
    color = c;
    hasColor = true;
    return this;
}

El* Icon::IntoEl() {
    El* e = IconEl(a, name, size);
    if (hasColor) {
        e->Fg(color);
    }
    return e;
}

}
}

#line 1 "src/component/Input.cpp"

namespace gpui {

namespace component {

Input* Input::New(Ctx* cx, Str id, LineInput* state) {
    Arena* a = cx->a;
    Input* i = ArenaNew<Input>(a);
    i->a = a;
    i->cx = cx;
    i->id = id;
    i->state = state;
    return i;
}
Input* Input::Label(Str s) {
    label = s;
    return this;
}
Input* Input::Prefix(El* el) {
    prefix = el;
    return this;
}
Input* Input::Suffix(El* el) {
    suffix = el;
    return this;
}
Input* Input::W(float v) {
    width = v;
    return this;
}
Input* Input::OnChange(Listener fn) {
    onChange = fn;
    return this;
}

Input* Input::OnFocus(Listener fn) {
    onFocus = fn;
    return this;
}
Input* Input::WithSize(UiSize s) {
    size = s;
    return this;
}
Input* Input::Align(InputAlign v) {
    align = v;
    return this;
}
Input* Input::Disabled(bool v) {
    disabled = v;
    return this;
}
Input* Input::Cleanable(bool v) {
    cleanable = v;
    return this;
}
Input* Input::Masked(bool v) {
    masked = v;
    return this;
}
Input* Input::MaskToggle(bool v) {
    maskToggle = v;
    return this;
}
Input* Input::Appearance(bool v) {
    appearance = v;
    return this;
}
Input* Input::TextColor(Rgba c) {
    textColor = c;
    hasTextColor = true;
    return this;
}
Input* Input::OnClear(Listener fn) {
    onClear = fn;
    return this;
}
Input* Input::OnToggleMask(Listener fn) {
    onToggleMask = fn;
    return this;
}

static const float kInputHeight = 32;
static const float kInputPadX = 10;
static const float kInputPadY = 8;
static const float kInputGap = 6;
static const float kInputTextSize = 14;

El* Input::IntoEl() {
    const Theme& th = cx->theme();
    El* col = Div(a)->FlexCol()->Gap(4);
    if (label.s) {
        col->Child(TextEl(a, label)->Font(12)->Fg(th.foreground));
    }
    bool focused = state && state->focused && !disabled;

    float h = kInputHeight, padX = kInputPadX, padY = kInputPadY,
          font = kInputTextSize;
    if (size == UiSize::Large) {
        h = 44;
        padX = 12;
        padY = 10;
        font = 16;
    } else if (size == UiSize::Small) {
        h = 24;
        padX = 8;
        padY = 2;
    } else if (size == UiSize::XSmall) {
        h = 20;
        padX = 4;
        padY = 0;
        font = 12;
    }
    InputEditorStyle editor;
    editor.foreground = hasTextColor ? textColor : th.foreground;
    editor.mutedForeground = th.mutedFg;
    editor.caret = th.caret;
    editor.fontSize = font;
    editor.mask = masked;
    editor.align = align == InputAlign::Center  ? 1
                   : align == InputAlign::Right ? 2
                                                : 0;
    if (disabled) {
        editor.foreground = th.mutedFg;
    }
    El* field = InputBase::New(cx, id, disabled ? 0 : HashClickId(id))
                    ->FlexRow()
                    ->W(width)
                    ->H(h)
                    ->PadX(padX)
                    ->PadY(padY)
                    ->Gap(kInputGap)
                    ->ItemsCenter();
    if (appearance) {
        field->Radius(th.radius)
            ->Bg(disabled ? th.muted : th.inputBg)

            ->Border(1, focused ? th.ring : th.inputBorder);
    }
    if (prefix) {

        field->PadL(0)->Child(prefix);
    }
    bool hasValue = state && state->len > 0;
    bool trailing = suffix || (cleanable && hasValue) || maskToggle;
    if (prefix || trailing) {
        field
            ->Child(Div(a)->Grow()->Child(gpui::Input::New(cx, state, editor)));
    } else {
        field->Child(gpui::Input::New(cx, state, editor));
    }
    if (maskToggle) {
        field->Child(Button::New(cx, StrDup(a, fmt("%s-mask", id)))
                         ->Text()
                         ->WithSize(UiSize::XSmall)
                         ->Icon(IconName::Eye)
                         ->OnClick(onToggleMask)
                         ->IntoEl());
    }
    if (cleanable && hasValue && !disabled) {
        field->Child(Button::New(cx, StrDup(a, fmt("%s-clean", id)))
                         ->Text()
                         ->WithSize(UiSize::XSmall)
                         ->Icon(IconName::X)
                         ->OnClick(onClear)
                         ->IntoEl());
    }
    if (suffix) {
        field->Child(suffix);
    }
    if (!disabled) {
        if (onFocus.IsValid()) {
            field->OnClick(onFocus);
        } else if (onChange.IsValid()) {
            field->OnClick(onChange);
        }
    }
    col->Child(field);
    return col;
}

Textarea* Textarea::New(Ctx* cx, Str id, const char* text) {
    Arena* a = cx->a;
    Textarea* t = ArenaNew<Textarea>(a);
    t->a = a;
    t->cx = cx;
    t->id = id;
    t->text = text;
    return t;
}
Textarea* Textarea::Rows(int n) {
    rows = n;
    return this;
}
Textarea* Textarea::H(float px) {
    height = px;
    return this;
}
Textarea* Textarea::Placeholder(Str s) {
    placeholder = s;
    return this;
}
Textarea* Textarea::SoftWrap(bool v) {
    softWrap = v;
    return this;
}
Textarea* Textarea::OnFocus(Listener fn) {
    onFocus = fn;
    return this;
}

El* Textarea::IntoEl() {
    const Theme& th = cx->theme();
    InputEditorStyle editor;
    editor.foreground = th.foreground;
    editor.mutedForeground = th.mutedFg;
    editor.caret = th.caret;
    editor.fontSize = kInputTextSize;

    float h = height > 0 ? height
              : rows > 0 ? (float)rows * 20.f + 2 * 8 + 2
                         : 64;
    bool empty = !text || !text[0];
    const char* body = empty && placeholder.s ? placeholder.s : text;
    if (empty && placeholder.s) {
        editor.foreground = th.mutedFg;
    }
    El* box =
        InputBase::New(cx, id, HashClickId(id))
            ->W(kFill)
            ->H(h)
            ->Pad(8)
            ->ClipY()
            ->Radius(th.radius)
            ->Bg(th.inputBg)
            ->Border(1, th.inputBorder)
            ->Child(gpui::Textarea::New(cx, body, editor, false, softWrap));
    if (onFocus.IsValid()) {
        box->OnClick(onFocus);
    }
    return box;
}

NumberInput* NumberInput::New(Ctx* cx, LineInput* state) {
    Arena* a = cx->a;
    NumberInput* n = ArenaNew<NumberInput>(a);
    n->a = a;
    n->cx = cx;
    n->state = state;
    return n;
}
NumberInput* NumberInput::New(Ctx* cx, Str id, LineInput* state) {
    NumberInput* n = New(cx, state);
    n->id = id;
    return n;
}
NumberInput* NumberInput::WithSize(UiSize s) {
    size = s;
    return this;
}
NumberInput* NumberInput::Disabled(bool v) {
    disabled = v;
    return this;
}
NumberInput* NumberInput::Appearance(bool v) {
    appearance = v;
    return this;
}
NumberInput* NumberInput::Suffix(El* el) {
    suffix = el;
    return this;
}
NumberInput* NumberInput::Bg(Rgba c) {
    bg = c;
    hasBg = true;
    return this;
}
NumberInput* NumberInput::TextColor(Rgba c) {
    textColor = c;
    hasTextColor = true;
    return this;
}
NumberInput* NumberInput::OnFocus(Listener fn) {
    onFocus = fn;
    return this;
}
NumberInput* NumberInput::W(float v) {
    width = v;
    return this;
}
NumberInput* NumberInput::OnInc(Listener fn) {
    onInc = fn;
    return this;
}
NumberInput* NumberInput::OnDec(Listener fn) {
    onDec = fn;
    return this;
}
El* NumberInput::IntoEl() {
    const Theme& th = cx->theme();
    float h = 32, btn = 32, font = 14;
    if (size == UiSize::Large) {
        h = 44;
        btn = 32;
        font = 16;
    } else if (size == UiSize::Small) {
        h = 24;
        btn = 24;
    } else if (size == UiSize::XSmall) {
        h = 20;
        btn = 24;
        font = 12;
    }
    Rgba border = disabled ? RgbaOpacity(th.inputBorder, 0.5f) : th.inputBorder;
    El* frame = gpui::NumberInput::New(cx)->FlexRow()->W(width)->H(h);
    if (appearance) {
        frame->Radius(th.radius)
            ->Bg(hasBg ? bg : (disabled ? th.muted : th.inputBg))
            ->Border(1, border);
    } else if (hasBg) {
        frame->Radius(th.radius)->Bg(bg);
    }

    Rgba stepFg = disabled ? RgbaOpacity(th.secondaryFg, 0.5f) : th.secondaryFg;
    El* dec = Div(a)->W(btn)->H(kFill)->ItemsCenter()->JustifyCenter()->Child(
        IconEl(a, IconName::Minus, font)->Fg(stepFg));
    El* inc = Div(a)->W(btn)->H(kFill)->ItemsCenter()->JustifyCenter()->Child(
        IconEl(a, IconName::Plus, font)->Fg(stepFg));
    if (!disabled) {
        dec->HoverBg(RgbaOpacity(th.inputBorder, 0.4f));
        inc->HoverBg(RgbaOpacity(th.inputBorder, 0.4f));
        BindClick(dec, StrDup(a, fmt("%s-dec", id.s ? id : StrL("number"))),
                  onDec);
        BindClick(inc, StrDup(a, fmt("%s-inc", id.s ? id : StrL("number"))),
                  onInc);
    }
    frame->Child(dec);

    Input* editor = Input::New(cx, id.s ? id : StrL("number"), state)
                        ->WithSize(size)
                        ->Align(InputAlign::Center)
                        ->Appearance(false)
                        ->Disabled(disabled)
                        ->OnFocus(onFocus);
    if (hasTextColor) {
        editor->TextColor(textColor);
    }
    if (suffix) {
        editor->Suffix(suffix);
    }
    frame->Child(Div(a)->Grow()->H(kFill)->Child(editor->IntoEl()));
    frame->Child(inc);
    return frame;
}

OtpInput* OtpInput::New(Ctx* cx, const char* value, int len) {
    Arena* a = cx->a;
    OtpInput* o = ArenaNew<OtpInput>(a);
    o->a = a;
    o->cx = cx;
    o->value = value;
    o->len = len;
    return o;
}
OtpInput* OtpInput::Id(Str s) {
    id = s;
    return this;
}
OtpInput* OtpInput::Slots(int n) {
    slots = n;
    return this;
}
OtpInput* OtpInput::Groups(int n) {
    groups = n;
    return this;
}
OtpInput* OtpInput::Masked(bool v) {
    masked = v;
    return this;
}
OtpInput* OtpInput::Disabled(bool v) {
    disabled = v;
    return this;
}
OtpInput* OtpInput::WithSize(UiSize s) {
    size = s;
    return this;
}
OtpInput* OtpInput::CellSize(float px) {
    cellPx = px;
    return this;
}
OtpInput* OtpInput::OnFocus(Listener fn) {
    onFocus = fn;
    return this;
}

El* OtpInput::IntoEl() {
    const Theme& th = cx->theme();
    float cell = 32, text = 16;
    if (cellPx > 0) {
        cell = cellPx;
        text = cellPx * 0.5f;
    } else if (size == UiSize::Large) {
        cell = 44;
        text = 18;
    } else if (size == UiSize::Small || size == UiSize::XSmall) {
        cell = 24;
        text = 14;
    }
    int nGroups = groups < 1 ? 1 : (groups > slots ? slots : groups);
    int per = (slots + nGroups - 1) / nGroups;
    if (per < 1) {
        per = 1;
    }
    Rgba fg = disabled ? th.mutedFg : th.secondaryFg;

    El* row = gpui::OtpInput::New(cx, HashClickId(id.s ? id : StrL("otp")))
                  ->FlexRow()
                  ->ItemsCenter()
                  ->Gap(20);
    if (onFocus.IsValid() && !disabled) {
        row->OnClick(onFocus);
    }
    El* group = nullptr;
    for (int i = 0; i < slots; i++) {
        if (i % per == 0) {
            group = Div(a)->FlexRow()->ItemsCenter()->Gap(4);
            row->Child(group);
        }
        El* box = Div(a)
                      ->W(cell)
                      ->H(cell)
                      ->ItemsCenter()
                      ->JustifyCenter()
                      ->Radius(th.radius)
                      ->Bg(disabled ? th.muted : th.inputBg)
                      ->Border(1, th.inputBorder);
        if (value && i < len) {
            if (masked) {
                box->Child(IconEl(a, IconName::Asterisk, text)->Fg(fg));
            } else {
                char ch[2] = {value[i], 0};
                box->Child(TextEl(a, StrDup(a, Str(ch)))
                               ->Font(text)
                               ->LineHeight(1.f)
                               ->Fg(fg));
            }
        }
        group->Child(box);
    }
    return row;
}

}
}

#line 1 "src/component/Kbd.cpp"

namespace gpui {

namespace component {

Kbd* Kbd::New(Ctx* cx, Str stroke) {
    Arena* a = cx->a;
    Kbd* k = ArenaNew<Kbd>(a);
    k->a = a;
    k->cx = cx;
    k->stroke = stroke;
    return k;
}

Kbd* Kbd::Appearance(bool v) {
    appearance = v;
    return this;
}

Kbd* Kbd::Outline() {
    outline = true;
    return this;
}

El* Kbd::IntoEl() {
    const Theme& th = cx->theme();
    if (!appearance) {
        return TextEl(a, stroke)->Font(12)->Fg(th.mutedFg);
    }

    El* e = Div(a)
                ->PadX(4)
                ->PadY(2)
                ->MinW(20)
                ->ItemsCenter()
                ->JustifyCenter()
                ->Radius(th.radius * 0.5f)
                ->Bg(th.muted);
    if (outline) {
        e->Bg(th.background)->Border(1, th.border);
    }
    e->Child(TextEl(a, stroke)->Font(12)->LineHeight(1.f)->Fg(th.mutedFg));
    return e;
}

}
}

#line 1 "src/component/Label.cpp"

namespace gpui {

namespace component {

Label* Label::New(Ctx* cx, Str text) {
    Arena* a = cx->a;
    Label* l = ArenaNew<Label>(a);
    l->a = a;
    l->cx = cx;
    l->text = text;
    return l;
}

Label* Label::Secondary(Str s) {
    secondary = s;
    return this;
}

Label* Label::Masked(bool v) {
    masked = v;
    return this;
}
Label* Label::Semibold() {
    semibold = true;
    return this;
}
Label* Label::Font(float px) {
    font = px;
    return this;
}

El* Label::IntoEl() {
    const Theme& th = cx->theme();
    Str shown = text;
    if (masked && text.len > 0) {
        char buf[64];
        int n = text.len < 63 ? text.len : 63;

        for (int i = 0; i < n; i++) {
            buf[i] = '*';
        }
        buf[n] = 0;
        shown = StrDup(a, Str(buf, n));
    }
    El* row = Div(a)->FlexRow()->ItemsCenter()->Gap(6);
    El* primary = TextEl(a, shown)->Font(font)->Fg(th.foreground);
    if (semibold) {
        primary->Semibold();
    }
    row->Child(primary);
    if (secondary.s) {
        row->Child(TextEl(a, secondary)->Font(14)->Fg(th.mutedFg));
    }
    return row;
}

}
}

#line 1 "src/component/Link.cpp"

namespace gpui {

namespace component {

Link* Link::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    Link* l = ArenaNew<Link>(a);
    l->a = a;
    l->cx = cx;
    l->id = id;
    return l;
}

Link* Link::Href(Str s) {
    href = s;
    return this;
}
Link* Link::Text(Str s) {
    text = s;
    return this;
}
Link* Link::Disabled(bool v) {
    disabled = v;
    return this;
}
Link* Link::OnOpen(Listener fn) {
    onOpen = fn;
    return this;
}

El* Link::IntoEl() {
    const Theme& th = cx->theme();
    El* e = gpui::Link::New(cx, id, disabled ? 0 : HashClickId(id));
    if (onOpen.IsValid() && !disabled) {
        e->OnClick(onOpen);
    }

    e->Child(TextEl(a, text.s ? text : href)
                 ->Font(14)
                 ->Underline()
                 ->Fg(disabled ? th.mutedFg : th.blue));
    return e;
}

}
}

#line 1 "src/component/List.cpp"

namespace gpui {

namespace component {

List* List::New(Ctx* cx) {
    Arena* a = cx->a;
    List* l = ArenaNew<List>(a);
    l->a = a;
    l->cx = cx;
    return l;
}
List* List::Item(Str s) {
    if (n < 32) {
        items[n++] = s;
    }
    return this;
}
List* List::Selected(int i) {
    selected = i;
    return this;
}
List* List::OnSelect(Listener fn) {
    onSelect = fn;
    return this;
}

El* List::IntoEl() {
    const Theme& th = cx->theme();
    El* col = Div(a)->FlexCol()->Border(1, th.border);
    for (int i = 0; i < n; i++) {
        El* row = Div(a)->H(32)->PadX(10)->ItemsCenter()->HoverBg(th.muted);
        if (i == selected) {
            row->Bg(th.accent);
        }
        row->Child(TextEl(a, items[i])->Font(13)->Fg(th.foreground));
        if (onSelect.IsValid()) {
            BindClick(row, items[i], ListenerArg(onSelect, i));
        }
        col->Child(row);
    }
    return col;
}

}
}

#line 1 "src/component/Menu.cpp"

namespace gpui {

namespace component {

Menu* Menu::New(Ctx* cx) {
    Arena* a = cx->a;
    Menu* m = ArenaNew<Menu>(a);
    m->a = a;
    m->cx = cx;
    return m;
}
Menu* Menu::Item(Str s) {
    if (n < 8) {
        items[n++] = s;
    }
    return this;
}
Menu* Menu::OnClick(Listener fn) {
    onClick = fn;
    return this;
}

El* Menu::IntoEl() {
    const Theme& th = cx->theme();
    El* col = Div(a)
                  ->FlexCol()
                  ->W(180)
                  ->Border(1, th.border)
                  ->Bg(th.background)
                  ->Radius(th.radius);
    for (int i = 0; i < n; i++) {
        El* row =
            Div(a)->H(28)->PadX(10)->ItemsCenter()->HoverBg(th.muted)->Child(
                TextEl(a, items[i])->Font(13)->Fg(th.foreground));
        if (onClick.IsValid()) {
            BindClick(row, items[i], ListenerArg(onClick, i));
        }
        col->Child(row);
    }
    return col;
}

}
}

#line 1 "src/component/NativeMenu.cpp"

namespace gpui {

namespace component {

El* NativeMenu::New(Ctx* cx, Menu* menu) {
    Arena* a = cx->a;
    return menu ? menu->IntoEl() : Div(a);
}

}
}

#line 1 "src/component/Notification.cpp"

namespace gpui {

namespace component {

Notification* Notification::New(Ctx* cx, Str title, Str message) {
    Arena* a = cx->a;
    Notification* n = ArenaNew<Notification>(a);
    n->a = a;
    n->cx = cx;
    n->title = title;
    n->message = message;
    return n;
}
Notification* Notification::Kind(NotificationKind k) {
    kind = k;
    return this;
}
Notification* Notification::Action(El* e) {
    action = e;
    return this;
}
Notification* Notification::Content(El* e) {
    content = e;
    return this;
}
Notification* Notification::Placement(NotificationAnchor p) {
    anchor = p;
    return this;
}
Notification* Notification::OnClose(Listener fn) {
    onClose = fn;
    return this;
}
Notification* Notification::OnClick(Listener fn) {
    onClick = fn;
    return this;
}

El* Notification::IntoEl() {
    AlertVariant v = AlertVariant::Info;
    if (kind == NotificationKind::Success) {
        v = AlertVariant::Success;
    } else if (kind == NotificationKind::Warning) {
        v = AlertVariant::Warning;
    } else if (kind == NotificationKind::Error) {
        v = AlertVariant::Error;
    }
    Alert* al = Alert::New(cx, StrL("notification"), message)
                    ->Title(title)
                    ->OnClose(onClose);
    al->variant = v;

    if (content || action) {
        El* extra = Div(a)->FlexCol()->W(kFill)->Gap(8);
        if (content) {
            extra->Child(content);
        } else if (message.s && message.len > 0) {
            extra->Child(TextEl(a, message)
                             ->Font(14)
                             ->Fg(cx->theme().foreground)
                             ->Wrap()
                             ->W(kFill));
        }
        if (action) {
            extra->Child(Div(a)->FlexRow()->Child(action));
        }
        al->Content(extra);
    }
    El* card = al->IntoEl();

    if (onClick.IsValid()) {
        BindClick(card, StrL("notification-body"), onClick);
    }
    if (anchor == NotificationAnchor::None) {
        return card;
    }

    card->W(width);
    El* layer = Div(a)
                    ->Fixed()
                    ->Top(0)
                    ->Left(0)
                    ->W(kFill)
                    ->H(kFill)
                    ->FlexCol()
                    ->Pad(16)
                    ->Child(card);
    switch (anchor) {
        case NotificationAnchor::TopLeft:
            layer->JustifyStart()->ItemsStart();
            break;
        case NotificationAnchor::TopCenter:
            layer->JustifyStart()->ItemsCenter();
            break;
        case NotificationAnchor::TopRight:
            layer->JustifyStart()->ItemsEnd();
            break;
        case NotificationAnchor::LeftCenter:
            layer->JustifyCenter()->ItemsStart();
            break;
        case NotificationAnchor::RightCenter:
            layer->JustifyCenter()->ItemsEnd();
            break;
        case NotificationAnchor::BottomLeft:
            layer->JustifyEnd()->ItemsStart();
            break;
        case NotificationAnchor::BottomCenter:
            layer->JustifyEnd()->ItemsCenter();
            break;
        default:
            layer->JustifyEnd()->ItemsEnd();
            break;
    }
    return layer;
}

}
}

#line 1 "src/component/Pagination.cpp"

namespace gpui {

namespace component {

Pagination* Pagination::New(Ctx* cx, int page, int total) {
    Arena* a = cx->a;
    Pagination* p = ArenaNew<Pagination>(a);
    p->a = a;
    p->cx = cx;
    p->page = page;
    p->total = total;
    return p;
}
Pagination* Pagination::Id(Str s) {
    id = s;
    return this;
}
Pagination* Pagination::VisiblePages(int n) {
    visiblePages = n;
    return this;
}
Pagination* Pagination::Compact(bool v) {
    compact = v;
    return this;
}
Pagination* Pagination::Disabled(bool v) {
    disabled = v;
    return this;
}
Pagination* Pagination::WithSize(UiSize s) {
    size = s;
    return this;
}
Pagination* Pagination::OnChange(Listener fn) {
    onChange = fn;
    return this;
}

struct PageItem {
    int page = 0;
    int from = 0;
    int to = 0;
};

static int PageItems(int current, int total, int maxVisible, PageItem* out,
                     int cap) {
    int n = 0;
    if (total <= 1) {
        return 0;
    }
    if (maxVisible < 5) {
        maxVisible = 5;
    }
    if (total <= maxVisible) {
        for (int i = 1; i <= total && n < cap; i++) {
            out[n].page = i;
            n++;
        }
        return n;
    }
    out[n].page = 1;
    n++;
    int side = (maxVisible - 3) / 2;
    int start = current <= side + 1          ? 2
                : current > total - side - 1 ? total - side - 1
                                             : current - side;
    if (start > 2 && n < cap) {
        out[n].page = 0;
        out[n].from = 2;
        out[n].to = start - 1;
        n++;
    }
    int end = current >= total - side ? total - 1
              : current <= side + 1   ? side + 2
                                      : current + side;
    for (int i = start; i <= end && n < cap; i++) {
        out[n].page = i;
        n++;
    }
    if (end < total - 1 && n < cap) {
        out[n].page = 0;
        out[n].from = end + 1;
        out[n].to = total - 1;
        n++;
    }
    if (n < cap) {
        out[n].page = total;
        n++;
    }
    return n;
}

El* Pagination::IntoEl() {
    Str base = id.s ? id : StrL("pagination");
    El* row = gpui::Pagination::New(cx, base)
                  ->FlexRow()
                  ->PadX(8)
                  ->PadY(8)
                  ->Gap(4)
                  ->ItemsCenter();

    bool hasPrev = page > 1 && !disabled;
    bool hasNext = page < total && !disabled;
    Button* prev = Button::New(cx, StrDup(a, fmt("%s-prev", base)))
                       ->Ghost()
                       ->Compact()
                       ->WithSize(size)
                       ->Disabled(!hasPrev);
    Button* next = Button::New(cx, StrDup(a, fmt("%s-next", base)))
                       ->Ghost()
                       ->Compact()
                       ->WithSize(size)
                       ->Disabled(!hasNext);
    if (compact) {
        prev->Icon(IconName::ChevronLeft);
        next->Icon(IconName::ChevronRight);
    } else {
        prev->Icon(IconName::ChevronLeft)->Label(StrL("Previous"));
        next->Label(StrL("Next"))->Extra(IconEl(a, IconName::ChevronRight, 16));
    }
    if (hasPrev && onChange.IsValid()) {
        prev->OnClick(ListenerArg(onChange, page - 1));
    }
    if (hasNext && onChange.IsValid()) {
        next->OnClick(ListenerArg(onChange, page + 1));
    }
    row->Child(prev->IntoEl());
    if (!compact) {
        PageItem items[32];
        int n = PageItems(page, total, visiblePages, items, 32);
        for (int i = 0; i < n; i++) {
            if (items[i].page == 0) {
                row->Child(
                    Button::New(cx, StrDup(a, fmt("%s-ellipsis-%d", base, i)))
                        ->Ghost()
                        ->Compact()
                        ->WithSize(size)
                        ->Disabled(disabled)
                        ->Icon(IconName::Ellipsis)
                        ->IntoEl());
                continue;
            }
            bool selected = items[i].page == page;
            Button* b =
                Button::New(cx,
                            StrDup(a, fmt("%s-page-%d", base, items[i].page)))
                    ->Label(StrDup(a, fmt("%d", items[i].page)))
                    ->Compact()
                    ->WithSize(size)
                    ->Disabled(disabled);
            if (selected) {
                b->Outline();
            } else {
                b->Ghost();
            }
            if (!selected && !disabled && onChange.IsValid()) {
                b->OnClick(ListenerArg(onChange, items[i].page));
            }
            row->Child(b->IntoEl());
        }
    }
    row->Child(next->IntoEl());
    return row;
}

}
}

#line 1 "src/component/Plot.cpp"

namespace gpui {

namespace component {

static void MinMax(const float* v, int n, float* outMin, float* outMax) {
    if (n <= 0) {
        *outMin = 0;
        *outMax = 0;
        return;
    }
    float lo = v[0];
    float hi = v[0];
    for (int i = 1; i < n; i++) {
        if (v[i] < lo) {
            lo = v[i];
        }
        if (v[i] > hi) {
            hi = v[i];
        }
    }
    *outMin = lo;
    *outMax = hi;
}

static int FirstIndexOf(const float* v, int n, float want) {
    for (int i = 0; i < n; i++) {
        if (v[i] == want) {
            return i;
        }
    }
    return 0;
}

ScaleLinear ScaleLinear::New(const float* domain, int domainN,
                             const float* range, int rangeN) {
    float domainMin = 0, domainMax = 0;
    MinMax(domain, domainN, &domainMin, &domainMax);

    float rangeMin = 0, rangeMax = 0;
    MinMax(range, rangeN, &rangeMin, &rangeMax);
    float rangeFrom = rangeMin;
    float rangeTo = rangeMax;
    if (rangeN > 0) {

        if (FirstIndexOf(range, rangeN, rangeMin) >
            FirstIndexOf(range, rangeN, rangeMax)) {
            rangeFrom = rangeMax;
            rangeTo = rangeMin;
        }
    }

    ScaleLinear s;
    s.domainLen = domainN;
    s.domainStart = domainMin;
    s.domainDiff = domainMax - domainMin;
    s.rangeStart = rangeFrom;
    s.rangeDiff = rangeTo - rangeFrom;
    return s;
}

bool ScaleLinear::Tick(float value, float* out) const {
    if (domainDiff == 0) {
        return false;
    }
    float ratio = (value - domainStart) / domainDiff;
    *out = ratio * rangeDiff + rangeStart;
    return true;
}

void ScaleLinear::LeastIndexWithDomain(float tick, const float* domain,
                                       int domainN, int* outIndex,
                                       float* outTick) const {
    *outIndex = 0;
    *outTick = 0;
    if (domainLen == 0 || domainN <= 0) {
        return;
    }

    int seen = 0;
    bool any = false;
    float bestDist = 0;
    for (int i = 0; i < domainN; i++) {
        float t = 0;
        if (!Tick(domain[i], &t)) {
            continue;
        }
        float dist = t - tick;
        if (dist < 0) {
            dist = -dist;
        }
        if (!any || dist < bestDist) {
            any = true;
            bestDist = dist;
            *outIndex = seen;
            *outTick = t;
        }
        seen++;
    }
}

ScalePoint ScalePoint::New(const float* domain, int domainN, const float* range,
                           int rangeN) {
    ScalePoint s;
    s.domain = domain;
    s.domainLen = domainN;
    if (domainN == 0) {
        return s;
    }
    float rangeMin = 0, rangeMax = 0;
    MinMax(range, rangeN, &rangeMin, &rangeMax);
    float diff = rangeMax - rangeMin;
    s.rangeStart = rangeMin;
    s.rangeTick = domainN == 1 ? diff : diff / (float)(domainN - 1);
    return s;
}

bool ScalePoint::Tick(float value, float* out) const {
    int index = -1;
    for (int i = 0; i < domainLen; i++) {
        if (domain[i] == value) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        return false;
    }

    *out = domainLen == 1 ? rangeStart + rangeTick * 0.5f
                          : rangeStart + (float)index * rangeTick;
    return true;
}

int ScalePoint::LeastIndex(float tick) const {
    if (domainLen <= 0 || rangeTick == 0) {
        return 0;
    }

    float index = roundf((tick - rangeStart) / rangeTick);
    if (index < 0) {
        return 0;
    }
    if (index > (float)(domainLen - 1)) {
        return domainLen - 1;
    }
    return (int)index;
}

int ScaleOrdinal::Map(int domainIndex) const {
    if (domainIndex < 0) {
        return unknown;
    }
    if (rangeLen <= 0) {
        return -1;
    }
    return domainIndex % rangeLen;
}

}
}

#line 1 "src/component/Popover.cpp"

namespace gpui {

namespace component {

Popover* Popover::New(Ctx* cx) {
    Arena* a = cx->a;
    Popover* p = ArenaNew<Popover>(a);
    p->a = a;
    p->cx = cx;
    return p;
}
Popover* Popover::Trigger(El* e) {
    trigger = e;
    return this;
}
Popover* Popover::Content(El* e) {
    content = e;
    return this;
}
Popover* Popover::Open(bool v) {
    open = v;
    return this;
}

El* Popover::IntoEl() {
    return gpui::Popover::New(cx, StrL("popover"))
        ->Trigger(trigger)
        ->Content(open ? content : nullptr)
        ->IntoEl();
}

}
}

#line 1 "src/component/Progress.cpp"

namespace gpui {

namespace component {

Progress* Progress::New(Ctx* cx) {
    Arena* a = cx->a;
    Progress* p = ArenaNew<Progress>(a);
    p->a = a;
    p->cx = cx;
    return p;
}

Progress* Progress::Value(float v) {
    value = v;
    if (value < 0) {
        value = 0;
    }
    if (value > 100) {
        value = 100;
    }
    return this;
}
Progress* Progress::W(float v) {
    w = v;
    return this;
}
Progress* Progress::H(float v) {
    h = v;
    return this;
}

El* Progress::IntoEl() {
    const Theme& th = cx->theme();
    return gpui::Progress::New(cx, StrL("progress"))
        ->W(w)
        ->Child(gpui::ProgressTrack::New(cx)
                    ->W(w)
                    ->H(h)
                    ->Radius(h * 0.5f)
                    ->Bg(RgbaOpacity(th.progress, 0.2f))

                    ->Child(gpui::ProgressIndicator::New(cx)
                                ->WFrac(value / 100.f)
                                ->H(h)
                                ->Radius(h * 0.5f)
                                ->Bg(th.progress)));
}

ProgressCircle* ProgressCircle::New(Ctx* cx) {
    Arena* a = cx->a;
    ProgressCircle* p = ArenaNew<ProgressCircle>(a);
    p->a = a;
    p->cx = cx;
    return p;
}
ProgressCircle* ProgressCircle::Value(float v) {
    value = v;
    return this;
}
ProgressCircle* ProgressCircle::Size(float v) {
    size = v;
    return this;
}
ProgressCircle* ProgressCircle::Color(Rgba c) {
    color = c;
    hasColor = true;
    return this;
}
ProgressCircle* ProgressCircle::Label(bool v) {
    showLabel = v;
    return this;
}

static void PaintCircleProgress(PaintCtx* ctx, El* e, void* user) {
    auto* p = (ProgressCircle*)user;
    if (!p || !ctx->rt) {
        return;
    }
    float cx = e->x + e->w * 0.5f;
    float cy = e->y + e->h * 0.5f;
    float r = (e->w < e->h ? e->w : e->h) * 0.42f;
    if (r < 3) {
        return;
    }
    float sw = r * 0.22f;
    if (sw < 1.5f) {
        sw = 1.5f;
    }
    Rgba col = p->hasColor ? p->color : ThemeNow().foreground;
    CanvasEllipse(ctx, cx, cy, r, r, sw, RgbaOpacity(col, 0.2f));
    float v = p->value;
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    if (v <= 0) {
        return;
    }
    float start = -kPi * 0.5f;
    float sweep = 2.f * kPi * (v / 100.f);
    Path* arc = PathNew(ctx, true);
    if (arc) {
        PathArcTo(arc, cx, cy, r, start, start + sweep, true);
        PathStroke(ctx, arc, sw, col);
        PathFree(arc);
    }
}

El* ProgressCircle::IntoEl() {
    El* e = Div(a)->W(size)->H(size)->ItemsCenter()->JustifyCenter();
    e->customPaint = PaintCircleProgress;
    e->customUser = this;
    if (showLabel && size >= 28) {
        e->Child(TextEl(a, StrDup(a, fmt("%.0f%%", value)))
                     ->Font(size * 0.22f)
                     ->Fg(hasColor ? color : cx->theme().foreground));
    }
    return e;
}

}
}

#line 1 "src/component/Radio.cpp"

namespace gpui {

namespace component {

Radio* Radio::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    Radio* r = ArenaNew<Radio>(a);
    r->a = a;
    r->cx = cx;
    r->id = id;
    return r;
}

Radio* Radio::Label(Str s) {
    label = s;
    return this;
}
Radio* Radio::Hint(Str s) {
    hint = s;
    return this;
}
Radio* Radio::Checked(bool v) {
    checked = v;
    return this;
}
Radio* Radio::Disabled(bool v) {
    disabled = v;
    return this;
}
Radio* Radio::WithSize(UiSize s) {
    size = s;
    return this;
}
Radio* Radio::OnClick(Listener fn) {
    onClick = fn;
    return this;
}

El* Radio::IntoEl() {
    const Theme& th = cx->theme();

    float box = size == UiSize::Small    ? 14.f
                : size == UiSize::XSmall ? 12.f
                : size == UiSize::Large  ? 18.f
                                         : 16.f;
    Rgba border = checked ? th.primary : th.inputBorder;
    Rgba fill = checked ? th.primary : th.inputBg;
    if (disabled) {
        border = RgbaOpacity(border, 0.5f);
        fill = RgbaOpacity(fill, 0.5f);
    }
    El* dot = Div(a)
                  ->W(box)
                  ->H(box)
                  ->Radius(box * 0.5f)
                  ->Border(1, border)
                  ->Bg(fill)
                  ->ItemsCenter()
                  ->JustifyCenter()
                  ->Shrink0();
    if (checked) {
        Rgba tick = disabled ? RgbaOpacity(th.primaryFg, 0.5f) : th.primaryFg;
        dot->Child(IconEl(a, IconName::Check, box - 5)->Fg(tick));
    }
    El* row = gpui::Radio::New(cx, id, disabled ? 0 : HashClickId(id))
                  ->FlexRow()
                  ->ItemsStart()
                  ->Gap(8);
    if (onClick.IsValid() && !disabled) {
        row->OnClick(ListenerArg(onClick, checked ? 0 : 1));
    }
    row->Child(dot);
    if (label.s || hint.s) {

        El* col = Div(a)->FlexCol()->Gap(4);
        if (label.s) {

            float fontPx = size == UiSize::XSmall  ? 12.f
                           : size == UiSize::Small ? 14.f
                           : size == UiSize::Large ? 18.f
                                                   : 16.f;
            col->Child(TextEl(a, label)->Font(fontPx)->LineHeight(1.f)->Fg(
                disabled ? th.mutedFg : th.foreground));
        }
        if (hint.s) {
            col->Child(TextEl(a, hint)
                           ->Font(12)
                           ->LineHeight(1.2f)
                           ->Fg(th.mutedFg)
                           ->Wrap());
        }
        row->Child(col);
    }
    return row;
}

}
}

#line 1 "src/component/Rating.cpp"

namespace gpui {

namespace component {

Rating* Rating::New(Ctx* cx) {
    Arena* a = cx->a;
    Rating* r = ArenaNew<Rating>(a);
    r->a = a;
    r->cx = cx;
    return r;
}
Rating* Rating::Value(int v) {
    value = v;
    return this;
}
Rating* Rating::Max(int v) {
    max = v;
    return this;
}
Rating* Rating::Disabled(bool v) {
    disabled = v;
    return this;
}
Rating* Rating::Color(Rgba c) {
    color = c;
    hasColor = true;
    return this;
}
Rating* Rating::WithSize(UiSize s) {
    size = s;
    return this;
}
Rating* Rating::OnChange(Listener fn) {
    onChange = fn;
    return this;
}

El* Rating::IntoEl() {
    const Theme& th = cx->theme();
    El* row = Div(a)->FlexRow()->Gap(4);
    int n = max > 8 ? 8 : max;
    for (int i = 1; i <= n; i++) {
        bool on = i <= value;
        Rgba onC = hasColor ? color : th.warning;

        El* star = IconEl(a, on ? IconName::StarFill : IconName::Star,
                          UiSizePx(size) * 0.6f)
                       ->Fg(on ? onC : th.mutedFg);
        El* hit = Div(a)->Child(star);
        if (onChange.IsValid() && !disabled) {
            BindClick(hit, StrDup(a, fmt("star-%d", i)),
                      ListenerArg(onChange, i));
        }
        row->Child(hit);
    }
    return row;
}

}
}

#line 1 "src/component/Root.cpp"

namespace gpui {

namespace component {

Root* Root::New(Ctx* cx) {
    Arena* a = cx->a;
    Root* r = ArenaNew<Root>(a);
    r->a = a;
    r->cx = cx;
    return r;
}
Root* Root::Bordered(bool v) {
    bordered = v;
    return this;
}
Root* Root::Child(El* e) {
    child = e;
    return this;
}

El* Root::IntoEl() {
    const Theme& th = cx->theme();
    El* e = Div(a)->FlexCol()->SizeFull()->Bg(th.background);
    if (bordered) {
        e->Border(1, th.border);
    }
    if (child) {
        e->Child(child);
    }
    return e;
}

}
}

#line 1 "src/component/Scroll.cpp"

namespace gpui {

namespace component {

Scrollable* Scrollable::New(Ctx* cx) {
    Arena* a = cx->a;
    Scrollable* s = ArenaNew<Scrollable>(a);
    s->a = a;
    s->cx = cx;
    return s;
}
Scrollable* Scrollable::Child(El* e) {
    child = e;
    return this;
}
Scrollable* Scrollable::ScrollY(float v) {
    scrollY = v;
    return this;
}
Scrollable* Scrollable::H(float v) {
    h = v;
    return this;
}

El* Scrollable::IntoEl() {
    El* box = Scrollbar::New(cx)->H(h)->ClipY()->ScrollY(scrollY)->W(kFill);
    if (child) {
        box->Child(child);
    }
    return box;
}

}
}

#line 1 "src/component/SearchableList.cpp"

namespace gpui {

namespace component {

SearchableList* SearchableList::New(Ctx* cx, LineInput* query) {
    Arena* a = cx->a;
    SearchableList* s = ArenaNew<SearchableList>(a);
    s->a = a;
    s->cx = cx;
    s->query = query;
    return s;
}
SearchableList* SearchableList::Item(Str s) {
    if (n < 32) {
        items[n++] = s;
    }
    return this;
}
SearchableList* SearchableList::OnSelect(Listener fn) {
    onSelect = fn;
    return this;
}

El* SearchableList::IntoEl() {
    List* list = List::New(cx);
    const char* q = query && query->len ? query->buf : "";
    for (int i = 0; i < n; i++) {
        if (q[0] && items[i].s && !strstr(items[i].s, q)) {
            continue;
        }
        list->Item(items[i]);
    }
    list->OnSelect(onSelect);
    return Div(a)
        ->FlexCol()
        ->Gap(8)
        ->Child(Input::New(cx, StrL("search"), query)->IntoEl())
        ->Child(list->IntoEl());
}

}
}

#line 1 "src/component/Select.cpp"

namespace gpui {

namespace component {

Select* Select::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    Select* s = ArenaNew<Select>(a);
    s->a = a;
    s->cx = cx;
    s->id = id;
    return s;
}
Select* Select::Option(Str s) {
    if (n < 24) {
        options[n++] = s;
    }
    return this;
}
Select* Select::Options(const char* const* items, int count) {
    for (int i = 0; i < count; i++) {
        Option(Str(items[i]));
    }
    return this;
}
Select* Select::Selected(int i) {
    selected = i;
    return this;
}
Select* Select::Placeholder(Str s) {
    placeholder = s;
    return this;
}
Select* Select::TitlePrefix(Str s) {
    titlePrefix = s;
    return this;
}
Select* Select::Empty(Str s) {
    empty = s;
    return this;
}
Select* Select::W(float v) {
    width = v;
    return this;
}
Select* Select::MenuWidth(float v) {
    menuWidth = v;
    return this;
}
Select* Select::MenuMaxH(float v) {
    menuMaxH = v;
    return this;
}
Select* Select::WithSize(UiSize s) {
    size = s;
    return this;
}
Select* Select::Icon(IconName i) {
    icon = i;
    return this;
}
Select* Select::Disabled(bool v) {
    disabled = v;
    return this;
}
Select* Select::Cleanable(bool v) {
    cleanable = v;
    return this;
}
Select* Select::Appearance(bool v) {
    appearance = v;
    return this;
}
Select* Select::Open(bool v) {
    open = v;
    return this;
}
Select* Select::OnChange(Listener fn) {
    onChange = fn;
    return this;
}
Select* Select::OnToggle(Listener fn) {
    onToggle = fn;
    return this;
}
Select* Select::OnClear(Listener fn) {
    onClear = fn;
    return this;
}

El* Select::IntoEl() {
    const Theme& th = cx->theme();

    float h = 32, padX = 10, font = 14, caret = 16;
    if (size == UiSize::Large) {
        h = 44;
        padX = 12;
        font = 16;
    } else if (size == UiSize::Small) {
        h = 24;
        padX = 8;
        caret = 14;
    } else if (size == UiSize::XSmall) {
        h = 20;
        padX = 4;
        font = 12;
        caret = 12;
    }
    bool hasValue = selected >= 0 && selected < n;
    Str title = hasValue
                    ? options[selected]
                    : (placeholder.s ? placeholder : StrL("Please select"));
    El* trigger = Div(a)
                      ->FlexRow()
                      ->W(width)
                      ->H(h)
                      ->PadX(padX)
                      ->Gap(4)
                      ->ItemsCenter()
                      ->JustifyBetween();
    if (appearance) {
        trigger->Radius(th.radius)
            ->Bg(disabled ? th.muted : th.inputBg)
            ->Border(1, open ? th.ring : th.inputBorder);
    }
    Rgba fg = disabled ? th.mutedFg : th.foreground;
    if (hasValue && titlePrefix.s) {
        title = StrDup(a, fmt("%s%s", titlePrefix, title));
    }
    trigger
        ->Child(TextEl(a, title)->Font(font)->Fg(hasValue ? fg : th.mutedFg));
    if (cleanable && hasValue && !disabled) {
        trigger->Child(Button::New(cx, StrDup(a, fmt("%s-clean", id)))
                           ->Text()
                           ->WithSize(UiSize::XSmall)
                           ->Icon(IconName::X)
                           ->OnClick(onClear)
                           ->IntoEl());
    } else if (icon != IconName::None) {

        trigger->Child(IconEl(a, icon, 12)->Fg(th.mutedFg));
    } else {
        trigger->Child(IconEl(a, IconName::ChevronDown, caret)->Fg(th.mutedFg));
    }
    if (!disabled) {
        BindClick(trigger, id, onToggle);
    }

    El* menu = nullptr;
    if (open && !disabled) {
        menu = Div(a)
                   ->FlexCol()
                   ->W(menuWidth > 0 ? menuWidth : width)
                   ->Pad(4)
                   ->Gap(1)
                   ->Radius(th.radiusLg)
                   ->Border(1, th.border)
                   ->Bg(th.background);
        if (menuMaxH > 0) {
            menu->H(menuMaxH)->ClipY();
        }
        if (n == 0) {

            menu->Child(
                Div(a)->H(96)->W(kFill)->ItemsCenter()->JustifyCenter()->Child(
                    TextEl(a, empty.s ? empty : StrL("No Data"))
                        ->Font(font)
                        ->Fg(th.mutedFg)));
        }
        for (int i = 0; i < n; i++) {
            El* row = Div(a)
                          ->FlexRow()
                          ->W(kFill)
                          ->H(28)
                          ->PadX(8)
                          ->Gap(8)
                          ->ItemsCenter()
                          ->JustifyBetween()
                          ->Radius(th.radius)
                          ->HoverBg(th.accent);
            row->Child(TextEl(a, options[i])->Font(font)->Fg(th.foreground));
            if (i == selected) {
                row->Child(IconEl(a, IconName::Check, 14)->Fg(th.foreground));
            }
            if (onChange.IsValid()) {
                BindClick(row, StrDup(a, fmt("%s-opt%d", id, i)),
                          ListenerArg(onChange, i));
            }
            menu->Child(row);
        }
    }
    El* root = gpui::Select::New(cx, id)->W(width)->Child(trigger);
    return Popup::New(cx, StrDup(a, fmt("%s-popup", id)), root)
        ->Content(menu)
        ->IntoEl();
}

}
}

#line 1 "src/component/Separator.cpp"

namespace gpui {

namespace component {

Separator* Separator::Vertical(Ctx* cx) {
    Arena* a = cx->a;
    Separator* s = ArenaNew<Separator>(a);
    s->a = a;
    s->cx = cx;
    s->vertical = true;
    return s;
}

Separator* Separator::Horizontal(Ctx* cx) {
    Arena* a = cx->a;
    Separator* s = ArenaNew<Separator>(a);
    s->a = a;
    s->cx = cx;
    return s;
}

Separator* Separator::Dashed() {
    line = SeparatorStyle::Dashed;
    return this;
}

Separator* Separator::Label(Str s) {
    label = s;
    return this;
}

Separator* Separator::Color(Rgba c) {
    color = c;
    hasColor = true;
    return this;
}

El* Separator::IntoEl() {
    const Theme& th = cx->theme();
    Rgba c = hasColor ? color : th.border;
    El* root = Div(a)->ItemsCenter()->JustifyCenter()->Shrink0();
    if (vertical) {
        root->H(kFill)->W(label.s ? 24.f : 1.f);

        El* lineEl = Div(a)->W(1)->H(kFill);
        if (line == SeparatorStyle::Dashed) {

            lineEl->Dashed()->DashArray(4, 2)->Border(1, c);
        } else {
            lineEl->Bg(c);
        }
        root->Child(lineEl);
    } else {
        root->W(kFill)->H(label.s ? 24.f : 1.f);
        El* lineEl = Div(a)->H(1)->W(kFill);
        if (line == SeparatorStyle::Dashed) {

            lineEl->Dashed()->DashArray(4, 2)->Border(1, c);
        } else {
            lineEl->Bg(c);
        }
        root->Child(lineEl);
    }
    if (label.s) {
        root->Child(TextEl(a, label)
                        ->Font(12)
                        ->Fg(th.mutedFg)
                        ->Bg(th.background)
                        ->PadX(8));
    }
    return root;
}

}
}

#line 1 "src/component/Setting.cpp"

namespace gpui {

namespace component {

Setting* Setting::New(Ctx* cx, Str title) {
    Arena* a = cx->a;
    Setting* s = ArenaNew<Setting>(a);
    s->a = a;
    s->cx = cx;
    s->title = title;
    return s;
}
Setting* Setting::Item(Str label, El* control) {
    if (n < 12) {
        items[n].label = label;
        items[n].control = control;
        n++;
    }
    return this;
}

El* Setting::IntoEl() {
    const Theme& th = cx->theme();

    El* col = Div(a)->FlexCol()->Gap(12)->W(kFill);
    col->Child(TextEl(a, title)->Font(16)->Semibold()->Fg(th.foreground));
    for (int i = 0; i < n; i++) {
        col->Child(
            Div(a)
                ->FlexRow()
                ->W(kFill)
                ->ItemsCenter()
                ->JustifyBetween()
                ->Child(TextEl(a, items[i].label)->Font(13)->Fg(th.foreground))
                ->Child(items[i].control ? items[i].control : Div(a)));
    }
    return col;
}

}
}

#line 1 "src/component/Sheet.cpp"

namespace gpui {

namespace component {

Sheet* Sheet::New(Ctx* cx) {
    Arena* a = cx->a;
    Sheet* s = ArenaNew<Sheet>(a);
    s->a = a;
    s->cx = cx;
    return s;
}
Sheet* Sheet::Title(Str s) {
    title = s;
    return this;
}
Sheet* Sheet::Placement(SheetPlacement p) {
    placement = p;
    return this;
}
Sheet* Sheet::Size(float px) {
    size = px;
    return this;
}
Sheet* Sheet::Overlay(bool v) {
    overlay = v;
    return this;
}
Sheet* Sheet::Open(bool v) {
    open = v;
    return this;
}
Sheet* Sheet::Body(El* e) {
    body = e;
    return this;
}
Sheet* Sheet::OnClose(Listener fn) {
    onClose = fn;
    return this;
}

El* Sheet::IntoEl(WinSize win) {
    if (!open) {
        return Div(a);
    }
    const Theme& th = cx->theme();
    bool horizontal =
        placement == SheetPlacement::Left || placement == SheetPlacement::Right;
    El* surface = Div(a)
                      ->Absolute()
                      ->W(horizontal ? size : win.dipW)
                      ->H(horizontal ? win.dipH : size)
                      ->Pad(16)
                      ->FlexCol()
                      ->Gap(12)
                      ->Bg(th.background)
                      ->Border(1, th.border);
    switch (placement) {
        case SheetPlacement::Left:
            surface->Top(0)->Left(0);
            break;
        case SheetPlacement::Top:
            surface->Top(0)->Left(0);
            break;
        case SheetPlacement::Bottom:
            surface->Top(win.dipH - size)->Left(0);
            break;
        default:
            surface->Top(0)->Left(win.dipW - size);
            break;
    }
    El* head = Div(a)->FlexRow()->W(kFill)->ItemsCenter()->JustifyBetween();
    head->Child(TextEl(a, title)->Font(16)->Semibold()->Fg(th.foreground));
    head->Child(Button::New(cx, StrL("sheet-close"))
                    ->Text()
                    ->WithSize(UiSize::XSmall)
                    ->Icon(IconName::X)
                    ->OnClick(onClose)
                    ->IntoEl());
    surface->Child(head);
    if (body) {
        surface->Child(body);
    }
    El* backdrop = nullptr;
    if (overlay) {
        backdrop =
            Div(a)->Absolute()->Top(0)->Left(0)->W(win.dipW)->H(win.dipH)->Bg(
                Rgba8(0, 0, 0, 40));
        if (onClose.IsValid()) {
            backdrop->OnClick(onClose)
                ->Click(HashClickId(StrL("sheet-overlay")));
        }
    }
    return gpui::Sheet::New(cx)->Overlay(backdrop)->Surface(surface)->IntoEl();
}

}
}

#line 1 "src/component/Sidebar.cpp"

namespace gpui {

namespace component {

Sidebar* Sidebar::New(Ctx* cx) {
    Arena* a = cx->a;
    Sidebar* s = ArenaNew<Sidebar>(a);
    s->a = a;
    s->cx = cx;
    return s;
}
Sidebar* Sidebar::Title(Str s) {
    title = s;
    return this;
}
Sidebar* Sidebar::Item(Str s) {
    if (n < 8) {
        items[n++] = s;
    }
    return this;
}
Sidebar* Sidebar::Selected(int i) {
    selected = i;
    return this;
}
Sidebar* Sidebar::Collapsed(bool v) {
    collapsed = v;
    return this;
}
Sidebar* Sidebar::OnSelect(Listener fn) {
    onSelect = fn;
    return this;
}

El* Sidebar::IntoEl() {
    const Theme& th = cx->theme();
    El* col = Div(a)
                  ->FlexCol()
                  ->W(collapsed ? 48.f : 220.f)
                  ->H(kFill)
                  ->Pad(8)
                  ->Gap(4)
                  ->Bg(th.sidebar);
    if (title.s && !collapsed) {
        col->Child(TextEl(a, title)->Font(14)->Semibold()->Fg(th.sidebarFg));
    }
    for (int i = 0; i < n; i++) {
        El* row = Div(a)->H(32)->PadX(8)->ItemsCenter()->Radius(6)->HoverBg(
            th.secondaryHover);
        if (i == selected) {
            row->Bg(th.accent);
        }
        row->Child(TextEl(a, collapsed ? StrL("•") : items[i])
                       ->Font(13)
                       ->Fg(th.sidebarFg));
        if (onSelect.IsValid()) {
            BindClick(row, items[i], ListenerArg(onSelect, i));
        }
        col->Child(row);
    }
    return col;
}

}
}

#line 1 "src/component/Skeleton.cpp"

namespace gpui {

namespace component {

Skeleton* Skeleton::New(Ctx* cx) {
    Arena* a = cx->a;
    Skeleton* s = ArenaNew<Skeleton>(a);
    s->a = a;
    s->cx = cx;
    return s;
}

Skeleton* Skeleton::Secondary() {
    secondary = true;
    return this;
}

Skeleton* Skeleton::W(float v) {
    w = v;
    return this;
}

Skeleton* Skeleton::H(float v) {
    h = v;
    return this;
}

El* Skeleton::IntoEl() {
    const Theme& th = cx->theme();
    Rgba bg = th.skeleton;
    if (secondary) {
        bg = RgbaOpacity(bg, 0.5f);
    }
    return Div(a)->W(w)->H(h)->Bg(bg)->Radius(4);
}

}
}

#line 1 "src/component/Slider.cpp"

namespace gpui {

namespace component {

Slider* Slider::New(Ctx* cx, Str id, SliderState* state) {
    Arena* a = cx->a;
    Slider* s = ArenaNew<Slider>(a);
    s->a = a;
    s->cx = cx;
    s->id = id;
    s->state = state;
    return s;
}
Slider* Slider::Reverse(bool v) {
    reverse = v;
    return this;
}
Slider* Slider::Vertical(bool v) {
    axis = v ? Axis::Vertical : Axis::Horizontal;
    return this;
}
Slider* Slider::WithAxis(Axis v) {
    axis = v;
    return this;
}
Slider* Slider::Disabled(bool v) {
    disabled = v;
    return this;
}
Slider* Slider::W(float px) {
    width = px;
    return this;
}
Slider* Slider::OnChange(Listener fn) {
    onChange = fn;
    return this;
}

static float Clamp01f(float v) {
    if (v < 0) {
        return 0;
    }
    return v > 1 ? 1 : v;
}

El* Slider::IntoEl() {
    const Theme& th = cx->theme();

    float low = state ? Clamp01f(state->pctLo) : 0.f;
    float hi = state ? Clamp01f(state->pctHi) : 0.f;
    bool range = state && state->value.range;
    if (low > hi) {
        float t = low;
        low = hi;
        hi = t;
    }

    if (state && !disabled) {
        state->onChange = onChange;
    }
    const float kBar = 4.f;
    const float kThumb = 14.f;
    const float kH = 20.f;
    float w = width;
    float mid = (kH - kBar) * 0.5f;

    Rgba railBg = th.secondary;
    Rgba fillBg = disabled ? RgbaOpacity(th.primary, 0.5f) : th.primary;
    Rgba thumbBorder = disabled ? RgbaOpacity(th.primary, 0.5f) : th.primary;
    SliderState* bind = disabled ? nullptr : state;

    if (axis == Axis::Vertical) {

        El* vtrack = SliderTrack::New(cx, bind, axis)
                         ->W(kH)
                         ->H(w)
                         ->Click(HashClickId(id.s ? id : StrL("slider-v")));
        vtrack->Child(SliderIndicator::New(cx, bind)
                          ->Absolute()
                          ->Left(mid)
                          ->Top(0)
                          ->W(kBar)
                          ->H(w)
                          ->Radius(kBar * 0.5f)
                          ->Bg(railBg));
        float vFrom = reverse ? hi : low;
        float vTo = reverse ? 1.f : hi;
        vtrack->Child(Div(a)
                          ->Absolute()
                          ->Left(mid)
                          ->Top(w * (1.f - vTo))
                          ->W(kBar)
                          ->H(w * (vTo - vFrom))
                          ->Radius(kBar * 0.5f)
                          ->Bg(fillBg));
        for (int i = 0; i < (range ? 2 : 1); i++) {
            float at = (range && i == 0) ? low : hi;
            vtrack->Child(SliderThumb::New(cx)
                              ->Absolute()
                              ->Left((kH - kThumb) * 0.5f)
                              ->Top(w * (1.f - at) - kThumb * 0.5f)
                              ->W(kThumb)
                              ->H(kThumb)
                              ->Radius(kThumb * 0.5f)
                              ->Bg(th.background)
                              ->Border(1, thumbBorder));
        }
        return gpui::Slider::New(cx)->W(kH)->H(w)->Child(vtrack);
    }

    El* track = SliderTrack::New(cx, bind, axis)
                    ->W(w)
                    ->H(kH)
                    ->Click(HashClickId(id.s ? id : StrL("slider")));
    track->Child(SliderIndicator::New(cx, bind)
                     ->Absolute()
                     ->Top(mid)
                     ->Left(0)
                     ->W(w)
                     ->H(kBar)
                     ->Radius(kBar * 0.5f)
                     ->Bg(railBg));

    float fillFrom = reverse ? hi : low;
    float fillTo = reverse ? 1.f : hi;
    track->Child(Div(a)
                     ->Absolute()
                     ->Top(mid)
                     ->Left(w * fillFrom)
                     ->W(w * (fillTo - fillFrom))
                     ->H(kBar)
                     ->Radius(kBar * 0.5f)
                     ->Bg(fillBg));
    for (int i = 0; i < (range ? 2 : 1); i++) {
        float at = (range && i == 0) ? low : hi;
        track->Child(SliderThumb::New(cx)
                         ->Absolute()
                         ->Top((kH - kThumb) * 0.5f)
                         ->Left(w * at - kThumb * 0.5f)
                         ->W(kThumb)
                         ->H(kThumb)
                         ->Radius(kThumb * 0.5f)
                         ->Bg(th.background)
                         ->Border(1, thumbBorder));
    }
    return gpui::Slider::New(cx)->W(w)->H(kH)->Child(track);
}

}
}

#line 1 "src/component/Spinner.cpp"

namespace gpui {

namespace component {

Spinner* Spinner::New(Ctx* cx) {
    Arena* a = cx->a;
    Spinner* s = ArenaNew<Spinner>(a);
    s->a = a;
    s->cx = cx;
    return s;
}

Spinner* Spinner::WithSize(UiSize s) {
    size = s;
    return this;
}
Spinner* Spinner::Size(float v) {
    px = v;
    return this;
}

Spinner* Spinner::Icon(IconName n) {
    icon = n;
    return this;
}

Spinner* Spinner::Color(Rgba c) {
    color = c;
    hasColor = true;
    return this;
}

El* Spinner::IntoEl() {
    const Theme& th = cx->theme();
    float dim = px > 0 ? px : UiSizePx(size);
    El* ic = IconEl(a, icon, dim);
    if (hasColor) {
        ic->Fg(color);
    } else {
        ic->Fg(th.mutedFg);
    }
    return Div(a)->W(dim)->H(dim)->ItemsCenter()->JustifyCenter()->Child(ic);
}

}
}

#line 1 "src/component/StatusBar.cpp"

namespace gpui {

namespace component {

StatusBar* StatusBar::New(Ctx* cx) {
    Arena* a = cx->a;
    StatusBar* s = ArenaNew<StatusBar>(a);
    s->a = a;
    s->cx = cx;
    return s;
}
StatusBar* StatusBar::Left(Str s) {
    left = s;
    hasLeft = true;
    return this;
}
StatusBar* StatusBar::Center(Str s) {
    center = s;
    hasCenter = true;
    return this;
}
StatusBar* StatusBar::Right(Str s) {
    right = s;
    hasRight = true;
    return this;
}

El* StatusBar::IntoEl() {
    const Theme& th = cx->theme();
    El* bar = Div(a)
                  ->FlexRow()
                  ->W(kFill)
                  ->H(28)
                  ->PadX(12)
                  ->ItemsCenter()
                  ->Bg(th.titleBar)
                  ->BorderT(1, th.border);

    if (hasLeft) {
        bar->Child(TextEl(a, left)->Font(12)->Fg(th.mutedFg));
    }
    El* mid = Div(a)->FlexRow()->Grow()->ItemsCenter();
    if (hasLeft && hasRight) {
        mid->JustifyCenter();
    } else if (hasLeft) {
        mid->JustifyEnd();
    }
    if (hasCenter) {
        mid->Child(TextEl(a, center)->Font(12)->Fg(th.mutedFg));
    }
    bar->Child(mid);
    if (hasRight) {
        bar->Child(TextEl(a, right)->Font(12)->Fg(th.mutedFg));
    }
    return bar;
}

}
}

#line 1 "src/component/Stepper.cpp"

namespace gpui {

namespace component {

Stepper* Stepper::New(Ctx* cx) {
    Arena* a = cx->a;
    Stepper* s = ArenaNew<Stepper>(a);
    s->a = a;
    s->cx = cx;
    return s;
}
Stepper* Stepper::Step(Str s) {
    if (n < 8) {
        steps[n++] = s;
    }
    return this;
}
Stepper* Stepper::Step(Str s, IconName icon) {
    if (n < 8) {
        icons[n] = icon;
    }
    return Step(s);
}
Stepper* Stepper::W(float px) {
    width = px;
    return this;
}
Stepper* Stepper::Current(int i) {
    current = i;
    return this;
}
Stepper* Stepper::OnChange(Listener fn) {
    onChange = fn;
    return this;
}

El* Stepper::IntoEl() {
    const Theme& th = cx->theme();

    El* root = Div(a)->FlexRow()->W(width)->ItemsStart();
    const float kDot = 24.f;
    for (int i = 0; i < n; i++) {
        bool on = i == current;
        bool done = i < current;
        Rgba fill = on || done ? th.primary : th.secondary;
        Rgba fg = on || done ? th.primaryFg : th.mutedFg;
        El* dot = Div(a)
                      ->W(kDot)
                      ->H(kDot)
                      ->Shrink0()
                      ->Radius(kDot * 0.5f)
                      ->ItemsCenter()
                      ->JustifyCenter()
                      ->Bg(fill);
        if (icons[i] != IconName::None) {
            dot->Child(IconEl(a, icons[i], 14)->Fg(fg));
        } else {
            dot->Child(TextEl(a, StrDup(a, fmt("%d", i + 1)))
                           ->Font(12)
                           ->LineHeight(1.f)
                           ->Fg(fg));
        }
        El* col = Div(a)->FlexCol()->ItemsCenter()->Gap(8)->Shrink0();
        col->Child(dot);
        col->Child(
            TextEl(a, steps[i])->Font(14)->Fg(on ? th.foreground : th.mutedFg));
        if (onChange.IsValid()) {
            BindClick(col, steps[i], ListenerArg(onChange, i));
        }
        root->Child(col);
        if (i + 1 < n) {
            root->Child(Div(a)
                            ->Grow()
                            ->H(kDot)
                            ->MinW(24)
                            ->FlexRow()
                            ->ItemsCenter()
                            ->Child(Div(a)->W(kFill)->H(1)->Bg(
                                done ? th.primary : th.border)));
        }
    }
    return root;
}

}
}

#line 1 "src/component/Switch.cpp"

namespace gpui {

namespace component {

Switch* Switch::New(Ctx* cx, Str id) {
    Arena* a = cx->a;
    Switch* s = ArenaNew<Switch>(a);
    s->a = a;
    s->cx = cx;
    s->id = id;
    return s;
}

Switch* Switch::Label(Str s) {
    label = s;
    return this;
}
Switch* Switch::Checked(bool v) {
    checked = v;
    return this;
}
Switch* Switch::Disabled(bool v) {
    disabled = v;
    return this;
}
Switch* Switch::WithSize(UiSize s) {
    size = s;
    return this;
}
Switch* Switch::Color(Rgba c) {
    color = c;
    hasColor = true;
    return this;
}
Switch* Switch::OnClick(Listener fn) {
    onClick = fn;
    return this;
}

El* Switch::IntoEl() {
    const Theme& th = cx->theme();
    Rgba on = hasColor ? color : th.primary;
    float trackW = 36;
    float trackH = 20;
    float thumb = 16;
    if (size == UiSize::Small || size == UiSize::XSmall) {
        trackW = 28;
        trackH = 16;
        thumb = 12;
    } else if (size == UiSize::Large) {
        trackW = 44;
        trackH = 24;
        thumb = 20;
    }

    Rgba trackBg = checked ? on : th.secondary;
    if (disabled && checked) {
        trackBg = RgbaOpacity(trackBg, 0.5f);
    }
    Rgba thumbBg = disabled ? RgbaOpacity(th.background, 0.35f) : th.background;
    El* track = SwitchTrack::New(cx, id)
                    ->W(trackW)
                    ->H(trackH)
                    ->Pad(2)
                    ->Radius(trackH * 0.5f)
                    ->Bg(trackBg)
                    ->ItemsCenter();
    if (checked) {
        track->JustifyEnd();
    } else {
        track->JustifyStart();
    }
    track->Child(SwitchThumb::New(cx)
                     ->W(thumb)
                     ->H(thumb)
                     ->Radius(thumb * 0.5f)
                     ->Bg(thumbBg));
    El* root = gpui::Switch::New(cx, id, disabled ? 0 : HashClickId(id))
                   ->FlexRow()
                   ->ItemsCenter()
                   ->Gap(8);
    if (onClick.IsValid() && !disabled) {
        root->OnClick(ListenerArg(onClick, !checked));
    }
    root->Child(track);
    if (label.s) {
        root->Child(TextEl(a, label)->Font(14)->Fg(disabled ? th.mutedFg
                                                            : th.foreground));
    }
    return root;
}

}
}

#line 1 "src/component/Tab.cpp"

namespace gpui {

namespace component {

Tabs* Tabs::New(Ctx* cx) {
    Arena* a = cx->a;
    Tabs* t = ArenaNew<Tabs>(a);
    t->a = a;
    t->cx = cx;
    return t;
}
Tabs* Tabs::Tab(Str label) {
    if (n < 8) {
        labels[n++] = label;
    }
    return this;
}
Tabs* Tabs::Selected(int i) {
    selected = i;
    return this;
}
Tabs* Tabs::OnChange(Listener fn) {
    onChange = fn;
    return this;
}

El* Tabs::IntoEl() {
    const Theme& th = cx->theme();
    El* bar = gpui::Tabs::New(cx, StrL("tabs"))
                  ->FlexRow()
                  ->Gap(4)
                  ->BorderB(1, th.border);
    for (int i = 0; i < n; i++) {
        bool on = i == selected;
        El* tab =
            gpui::Tab::New(cx, labels[i], HashClickId(labels[i]))
                ->H(28)
                ->PadX(8)
                ->ItemsCenter()
                ->BorderB(2, on ? th.foreground : th.background)
                ->Child(TextEl(a, labels[i])->Font(13)->Fg(th.foreground));
        if (on) {
            tab->first->style.fontSemibold = true;
        }
        if (onChange.IsValid()) {
            tab->OnClick(ListenerArg(onChange, i));
        }
        bar->Child(tab);
    }
    return bar;
}

}
}

#line 1 "src/component/Table.cpp"

namespace gpui {

namespace component {

Table* Table::New(Ctx* cx) {
    Arena* a = cx->a;
    Table* t = ArenaNew<Table>(a);
    t->a = a;
    t->cx = cx;
    return t;
}
Table* Table::Heads(const char** h, int n) {
    heads = h;
    nHeads = n;
    return this;
}
Table* Table::Rows(const char*** r, int n) {
    rows = r;
    nRows = n;
    return this;
}

El* Table::IntoEl() {
    const Theme& th = cx->theme();
    El* t =
        gpui::Table::New(cx, StrL("table"))->FlexCol()->Border(1, th.border);
    El* head =
        TableHeader::New(cx, StrL("th"))
            ->Child(TableRow::New(cx, StrL("hr"))->FlexRow()->Bg(th.muted));
    for (int i = 0; i < nHeads; i++) {
        head->first->Child(
            TableHead::New(cx, Str(heads[i]))
                ->Pad(8)
                ->Grow()
                ->Child(TextEl(a, Str(heads[i]))->Font(12)->Fg(th.mutedFg)));
    }
    t->Child(head);
    El* body = TableBody::New(cx, StrL("tb"))->FlexCol();
    for (int r = 0; r < nRows; r++) {
        El* row = TableRow::New(cx, StrDup(a, fmt("r%d", r)))
                      ->FlexRow()
                      ->BorderT(1, th.border);
        for (int c = 0; c < nHeads; c++) {
            row->Child(TableCell::New(cx, StrDup(a, fmt("c%d", c)))
                           ->Pad(8)
                           ->Grow()
                           ->Child(TextEl(a, Str(rows[r][c]))
                                       ->Font(13)
                                       ->Fg(th.foreground)));
        }
        body->Child(row);
    }
    t->Child(body);
    return t;
}

}
}

#line 1 "src/component/Tag.cpp"

namespace gpui {

namespace component {

Tag* Tag::New(Ctx* cx, Str text) {
    Arena* a = cx->a;
    Tag* t = ArenaNew<Tag>(a);
    t->a = a;
    t->cx = cx;
    t->text = text;
    return t;
}

Tag* Tag::Primary() {
    variant = TagVariant::Primary;
    return this;
}
Tag* Tag::Secondary() {
    variant = TagVariant::Secondary;
    return this;
}
Tag* Tag::Danger() {
    variant = TagVariant::Danger;
    return this;
}
Tag* Tag::Success() {
    variant = TagVariant::Success;
    return this;
}
Tag* Tag::Warning() {
    variant = TagVariant::Warning;
    return this;
}
Tag* Tag::Info() {
    variant = TagVariant::Info;
    return this;
}
Tag* Tag::Outline() {
    outline = true;
    return this;
}
Tag* Tag::WithSize(UiSize s) {
    size = s;
    return this;
}
Tag* Tag::Radius(float v) {
    radius = v;
    return this;
}
Tag* Tag::Custom(Rgba bg, Rgba fg) {
    customBg = bg;
    customFg = fg;
    hasCustom = true;
    return this;
}

El* Tag::IntoEl() {
    const Theme& th = cx->theme();
    Rgba bg = th.secondary, fg = th.secondaryFg, bd = th.border;
    switch (variant) {
        case TagVariant::Primary:
            bg = th.primary;
            fg = th.primaryFg;
            bd = th.primary;
            break;
        case TagVariant::Danger:
            bg = th.danger;
            fg = th.dangerFg;
            bd = th.danger;
            break;
        case TagVariant::Success:
            bg = th.success;
            fg = th.successFg;
            bd = th.success;
            break;
        case TagVariant::Warning:
            bg = th.warning;
            fg = th.warningFg;
            bd = th.warning;
            break;
        case TagVariant::Info:
            bg = th.info;
            fg = th.infoFg;
            bd = th.info;
            break;
        default:
            break;
    }
    if (hasCustom) {
        bg = customBg;
        fg = customFg;
        bd = customFg;
    }
    if (outline) {
        fg = bd;
        bg = th.background;
    }
    float r = radius >= 0 ? radius : th.radius * 0.5f;
    return Div(a)
        ->PadX(size == UiSize::Small ? 6.f : 8.f)
        ->PadY(2)
        ->Radius(r)
        ->Bg(bg)
        ->Border(1, bd)
        ->ItemsCenter()
        ->Child(TextEl(a, text)->Font(UiFontPx(size) - 2)->Fg(fg));
}

}
}

#line 1 "src/component/Text.cpp"

namespace gpui {

namespace component {

struct MdBuild {
    Arena* a = nullptr;
    MdNode* cur = nullptr;

    uint8_t marks = 0;
    Str href = {};

    bool inHead = false;
};

static MdNode* component_Text_Push(MdBuild* b, MdKind k) {
    MdNode* n = ArenaNew<MdNode>(b->a);
    n->kind = k;
    n->parent = b->cur;
    if (b->cur->last) {
        b->cur->last->next = n;
    } else {
        b->cur->first = n;
    }
    b->cur->last = n;
    b->cur = n;
    return n;
}

static void Pop(MdBuild* b) {
    if (b->cur->parent) {
        b->cur = b->cur->parent;
    }
}

static void AddText(MdBuild* b, Str s) {
    if (s.len <= 0) {
        return;
    }
    MdNode* n = b->cur;
    MdRun* r = n->runLast;
    if (r && r->marks == b->marks && r->href.s == b->href.s &&
        r->text.s + r->text.len == s.s) {
        r->text.len += s.len;
        return;
    }
    r = ArenaNew<MdRun>(b->a);
    r->text = s;
    r->marks = b->marks;
    r->href = b->href;
    if (n->runLast) {
        n->runLast->next = r;
    } else {
        n->runFirst = r;
    }
    n->runLast = r;
}

static int Utf8Encode(char* out, uint32_t cp) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

struct NamedEntity {
    const char* name;
    uint32_t cp;
};

static const NamedEntity kEntities[] = {
    {"amp", '&'},      {"lt", '<'},        {"gt", '>'},
    {"quot", '"'},     {"apos", '\''},     {"nbsp", 0xa0},
    {"copy", 0xa9},    {"reg", 0xae},      {"trade", 0x2122},
    {"deg", 0xb0},     {"hellip", 0x2026}, {"mdash", 0x2014},
    {"ndash", 0x2013}, {"lsquo", 0x2018},  {"rsquo", 0x2019},
    {"ldquo", 0x201c}, {"rdquo", 0x201d},  {"bull", 0x2022},
    {"middot", 0xb7},  {"times", 0xd7},    {"rarr", 0x2192},
    {"larr", 0x2190},  {"check", 0x2713},  {"dagger", 0x2020},
};

static uint32_t ParseHex(Str s) {
    uint32_t v = 0;
    for (int i = 0; i < s.len; i++) {
        char c = s.s[i];
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return 0;
        }
        v = v * 16 + (uint32_t)d;
    }
    return v;
}

static uint32_t ParseDec(Str s) {
    uint32_t v = 0;
    for (int i = 0; i < s.len; i++) {
        if (s.s[i] < '0' || s.s[i] > '9') {
            return 0;
        }
        v = v * 10 + (uint32_t)(s.s[i] - '0');
    }
    return v;
}

static Str DecodeEntity(Arena* a, Str e) {
    if (e.len < 3 || e.s[0] != '&' || e.s[e.len - 1] != ';') {
        return e;
    }
    Str body((char*)e.s + 1, e.len - 2);
    uint32_t cp = 0;
    if (body.len > 1 && body.s[0] == '#') {
        if (body.s[1] == 'x' || body.s[1] == 'X') {
            cp = ParseHex(Str((char*)body.s + 2, body.len - 2));
        } else {
            cp = ParseDec(Str((char*)body.s + 1, body.len - 1));
        }
    } else {
        for (const NamedEntity& ne : kEntities) {
            int n = (int)strlen(ne.name);
            if (n == body.len && memcmp(ne.name, body.s, (size_t)n) == 0) {
                cp = ne.cp;
                break;
            }
        }
    }
    if (cp == 0) {
        return e;
    }
    char* out = (char*)Alloc(a, 5);
    if (!out) {
        return e;
    }
    int n = Utf8Encode(out, cp);
    out[n] = 0;
    return Str(out, n);
}

static Str Attr(Arena* a, const MD_ATTRIBUTE* at) {
    if (!at || !at->text || at->size == 0) {
        return {};
    }
    return StrDup(a, Str((char*)at->text, (int)at->size));
}

static int OnEnterBlock(MD_BLOCKTYPE type, void* detail, void* ud) {
    MdBuild* b = (MdBuild*)ud;
    switch (type) {
        case MD_BLOCK_QUOTE:
            component_Text_Push(b, MdKind::Quote);
            break;
        case MD_BLOCK_UL:
            component_Text_Push(b, MdKind::List);
            break;
        case MD_BLOCK_OL: {
            MD_BLOCK_OL_DETAIL* d = (MD_BLOCK_OL_DETAIL*)detail;
            MdNode* n = component_Text_Push(b, MdKind::List);
            n->ordered = true;
            n->start = d ? (int)d->start : 1;
            break;
        }
        case MD_BLOCK_LI:
            component_Text_Push(b, MdKind::Item);
            break;
        case MD_BLOCK_HR:
            component_Text_Push(b, MdKind::Rule);
            break;
        case MD_BLOCK_H: {
            MD_BLOCK_H_DETAIL* d = (MD_BLOCK_H_DETAIL*)detail;
            MdNode* n = component_Text_Push(b, MdKind::Heading);
            n->level = d ? (uint8_t)d->level : 1;
            break;
        }
        case MD_BLOCK_CODE: {
            MD_BLOCK_CODE_DETAIL* d = (MD_BLOCK_CODE_DETAIL*)detail;
            MdNode* n = component_Text_Push(b, MdKind::Code);
            n->lang = d ? Attr(b->a, &d->lang) : Str{};
            break;
        }
        case MD_BLOCK_HTML:
            component_Text_Push(b, MdKind::Html);
            break;
        case MD_BLOCK_P:
            component_Text_Push(b, MdKind::Paragraph);
            break;
        case MD_BLOCK_TABLE:
            component_Text_Push(b, MdKind::Table);
            break;
        case MD_BLOCK_THEAD:
            b->inHead = true;
            break;
        case MD_BLOCK_TBODY:
            b->inHead = false;
            break;
        case MD_BLOCK_TR: {
            MdNode* n = component_Text_Push(b, MdKind::Row);
            n->head = b->inHead;
            break;
        }
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            MD_BLOCK_TD_DETAIL* d = (MD_BLOCK_TD_DETAIL*)detail;
            MdNode* n = component_Text_Push(b, MdKind::Cell);
            n->align = d ? (uint8_t)d->align : 0;
            break;
        }
        default:
            break;
    }
    return 0;
}

static int OnLeaveBlock(MD_BLOCKTYPE type, void* detail, void* ud) {
    (void)detail;
    MdBuild* b = (MdBuild*)ud;
    switch (type) {
        case MD_BLOCK_DOC:
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
            break;
        default:
            Pop(b);
            break;
    }
    return 0;
}

static uint8_t SpanMark(MD_SPANTYPE type) {
    switch (type) {
        case MD_SPAN_EM:
            return MdItalic;
        case MD_SPAN_STRONG:
            return MdBold;
        case MD_SPAN_CODE:
            return MdCode;
        case MD_SPAN_DEL:
            return MdDel;
        case MD_SPAN_U:
            return MdUnderline;
        case MD_SPAN_A:
        case MD_SPAN_WIKILINK:
            return MdLink;
        default:
            return 0;
    }
}

static int OnEnterSpan(MD_SPANTYPE type, void* detail, void* ud) {
    MdBuild* b = (MdBuild*)ud;
    if (type == MD_SPAN_A) {
        MD_SPAN_A_DETAIL* d = (MD_SPAN_A_DETAIL*)detail;
        b->href = d ? Attr(b->a, &d->href) : Str{};
    }

    b->marks = (uint8_t)(b->marks | SpanMark(type));
    return 0;
}

static int OnLeaveSpan(MD_SPANTYPE type, void* detail, void* ud) {
    (void)detail;
    MdBuild* b = (MdBuild*)ud;
    if (type == MD_SPAN_A) {
        b->href = {};
    }
    b->marks = (uint8_t)(b->marks & ~SpanMark(type));
    return 0;
}

static int OnText(MD_TEXTTYPE type, const MD_CHAR* txt, MD_SIZE size,
                  void* ud) {
    MdBuild* b = (MdBuild*)ud;
    Str s((char*)txt, (int)size);
    switch (type) {
        case MD_TEXT_NULLCHAR:
            AddText(b, StrL("\xEF\xBF\xBD"));
            break;
        case MD_TEXT_BR:
            AddText(b, StrL("\n"));
            break;
        case MD_TEXT_SOFTBR:
            AddText(b, StrL(" "));
            break;
        case MD_TEXT_ENTITY:
            AddText(b, DecodeEntity(b->a, s));
            break;
        case MD_TEXT_HTML:

            break;
        default:
            AddText(b, s);
            break;
    }
    return 0;
}

MdNode* MdParse(Arena* a, Str source) {
    MdNode* doc = ArenaNew<MdNode>(a);
    doc->kind = MdKind::Doc;
    if (!source.s || source.len <= 0) {
        return doc;
    }
    MdBuild b;
    b.a = a;
    b.cur = doc;

    MD_PARSER p = {};
    p.abi_version = 0;
    p.flags = MD_DIALECT_GITHUB;
    p.enter_block = OnEnterBlock;
    p.leave_block = OnLeaveBlock;
    p.enter_span = OnEnterSpan;
    p.leave_span = OnLeaveSpan;
    p.text = OnText;
    md_parse(source.s, (MD_SIZE)source.len, &p, &b);
    return doc;
}

struct MdCacheSlot {
    Arena* a = nullptr;
    Str source = {};
    MdNode* doc = nullptr;
    uint64_t used = 0;
};

constexpr int kMdCacheSlots = 8;

struct MdCache {
    MdCacheSlot slots[kMdCacheSlots] = {};
    uint64_t clock = 0;

    ~MdCache() {
        for (int i = 0; i < kMdCacheSlots; i++) {
            if (slots[i].a) {
                ArenaDelete(slots[i].a);
            }
        }
    }
};

static bool MdSourceEq(Str a, Str b) {
    if (a.len != b.len) {
        return false;
    }
    return a.len == 0 || memcmp(a.s, b.s, (size_t)a.len) == 0;
}

static MdNode* MdParseCached(Ctx* cx, Arena* frame, Str source) {
    MdCache* c = nullptr;
    if (cx && cx->win) {
        auto* slot = KeyedState<Entity<MdCache>>(
            cx, (uint32_t)HashClickId(StrL("gpui-md-parse-cache")));
        if (slot) {
            if (!slot->IsValid()) {
                *slot = EntityNewState<MdCache>(cx->app);
            }
            c = slot->Get(cx);
        }
    }
    if (!c) {
        return MdParse(frame, source);
    }

    c->clock++;
    MdCacheSlot* lru = &c->slots[0];
    for (int i = 0; i < kMdCacheSlots; i++) {
        MdCacheSlot* s = &c->slots[i];
        if (s->used != 0 && MdSourceEq(s->source, source)) {
            s->used = c->clock;
            return s->doc;
        }
        if (s->used < lru->used) {
            lru = s;
        }
    }

    if (!lru->a) {
        lru->a = ArenaNew();
    } else {
        lru->a->Reset();
    }
    lru->source = StrDup(lru->a, source);
    lru->doc = MdParse(lru->a, lru->source);
    lru->used = c->clock;
    return lru->doc;
}

static float HeadingScale(int level) {
    switch (level) {
        case 1:
            return 2.f;
        case 2:
            return 1.5f;
        case 3:
            return 1.25f;
        case 4:
            return 1.125f;
        default:
            return 1.f;
    }
}

static int HeadingWeight(int level) {
    if (level == 1) {
        return 3;
    }
    if (level >= 6) {
        return 1;
    }
    return 2;
}

static El* ApplyWeight(El* t, int weight) {
    if (weight >= 3) {
        return t->Bold();
    }
    if (weight == 2) {
        return t->Semibold();
    }
    if (weight == 1) {
        return t->Medium();
    }
    return t;
}

static Str Bullet(int depth) {
    switch (depth) {
        case 0:
            return StrL("\xE2\x80\xA2 ");
        case 1:
            return StrL("\xE2\x97\xA6 ");
        case 2:
            return StrL("\xE2\x96\xAA ");
        case 3:
            return StrL("\xE2\x80\xA3 ");
        default:
            return StrL("\xE2\x81\x83 ");
    }
}

static Str OrderedMarker(Arena* a, int n, int depth) {
    char buf[24];
    if (depth == 0) {
        snprintf(buf, sizeof(buf), "%d. ", n);
        return StrDup(a, Str(buf));
    }

    int ix = n > 0 ? (n - 1) % 26 : 0;
    snprintf(buf, sizeof(buf), "%c. ", (depth == 1 ? 'A' : 'a') + ix);
    return StrDup(a, Str(buf));
}

static El* MdWord(Arena* a, const Theme& th, Str w, float font, Rgba color,
                  uint8_t marks, int weight, bool selectable) {
    Rgba c = color;
    if (marks & MdLink) {
        c = th.primary;
    } else if (marks & MdDel) {

        c = th.mutedFg;
    }
    float px = (marks & MdCode) ? font - 1 : font;
    El* t = TextEl(a, w)->Font(px)->Fg(c);
    ApplyWeight(t, (marks & MdBold) ? (weight > 2 ? weight : 2) : weight);
    if (marks & MdItalic) {
        t->Italic();
    }
    if (marks & (MdLink | MdUnderline)) {
        t->Underline();
    }
    if (marks & MdCode) {

        t->Mono()->Bg(th.accent);
    }
    if (selectable) {
        t->Selectable();
    }
    return t;
}

static bool IsPlainRun(MdRun* r) {
    if (!r || r->next || r->marks != 0) {
        return false;
    }
    for (int i = 0; i < r->text.len; i++) {
        if (r->text.s[i] == '\n') {
            return false;
        }
    }
    return true;
}

El* TextView::Inline(MdNode* n, float font, Rgba color, int weight) {
    const Theme& th = cx->theme();

    if (IsPlainRun(n->runFirst)) {
        El* t = TextEl(a, n->runFirst->text)->Font(font)->Fg(color)->Wrap();
        ApplyWeight(t, weight);
        if (selectable) {
            t->Selectable();
        }
        return t->W(kFill);
    }

    El* col = Div(a)->FlexCol()->W(kFill);
    El* row = Div(a)->FlexRow()->FlexWrap();
    char word[512];
    int len = 0;
    uint8_t marks = 0;
    auto flush = [&]() {
        if (len <= 0) {
            return;
        }
        row->Child(MdWord(a, th, StrDup(a, Str(word, len)), font, color, marks,
                          weight, selectable));
        len = 0;
    };
    for (MdRun* r = n->runFirst; r; r = r->next) {
        flush();
        marks = r->marks;
        for (int i = 0; i < r->text.len; i++) {
            char c = r->text.s[i];
            if (c == '\n') {
                flush();
                col->Child(row);
                row = Div(a)->FlexRow()->FlexWrap();
                continue;
            }
            if (len < (int)sizeof(word) - 1) {
                word[len++] = c;
            }
            if (c == ' ') {
                flush();
            }
        }
    }
    flush();
    col->Child(row);
    return col;
}

static int RunsLen(MdNode* n) {
    int len = 0;
    for (MdRun* r = n->runFirst; r; r = r->next) {
        len += r->text.len;
    }
    return len;
}

El* TextView::CodeBlock(MdNode* n) {
    const Theme& th = cx->theme();
    El* box =
        Div(a)->FlexCol()->W(kFill)->Pad(12)->Radius(th.radius)->Bg(th.muted);

    int len = RunsLen(n);
    while (len > 0 && n->runLast && n->runLast->text.len > 0 &&
           n->runLast->text.s[n->runLast->text.len - 1] == '\n') {

        n->runLast->text.len--;
        len--;
    }
    char* buf = (char*)Alloc(a, len + 1);
    if (!buf) {
        return box;
    }
    int at = 0;
    for (MdRun* r = n->runFirst; r; r = r->next) {
        memcpy(buf + at, r->text.s, (size_t)r->text.len);
        at += r->text.len;
    }
    buf[at] = 0;
    El* t = TextEl(a, Str(buf, at))->Font(codeFont)->Fg(th.foreground)->Mono();
    if (selectable) {
        t->Selectable();
    }
    box->Child(t);
    return box;
}

El* TextView::Table(MdNode* n) {
    enum {
        kMaxCols = 32,

        kMaxLen = 150
    };
    const Theme& th = cx->theme();
    int colLen[kMaxCols] = {};
    int nCols = 0;
    for (MdNode* r = n->first; r; r = r->next) {
        int ix = 0;
        for (MdNode* c = r->first; c && ix < kMaxCols; c = c->next, ix++) {
            int len = RunsLen(c);
            if (len > kMaxLen) {
                len = kMaxLen;
            }
            if (len > colLen[ix]) {
                colLen[ix] = len;
            }
            if (ix + 1 > nCols) {
                nCols = ix + 1;
            }
        }
    }
    float total = 0;
    for (int i = 0; i < nCols; i++) {

        if (colLen[i] < 4) {
            colLen[i] = 4;
        }
        total += (float)colLen[i];
    }
    if (total <= 0) {
        return Div(a);
    }

    El* table =
        Div(a)->FlexCol()->W(kFill)->Border(1, th.border)->Radius(th.radius);
    for (MdNode* r = n->first; r; r = r->next) {
        El* row = Div(a)->FlexRow()->W(kFill);
        if (r->next) {
            row->BorderB(1, th.border);
        }
        int ix = 0;
        for (MdNode* c = r->first; c; c = c->next, ix++) {
            float frac = ix < nCols ? (float)colLen[ix] / total : 1.f / total;
            El* cell = Div(a)->WFrac(frac)->MinW(tableColW)->PadX(8)->PadY(4);
            if (c->next) {
                cell->BorderR(1, th.border);
            }
            cell->Child(Inline(c, baseFont, th.foreground, r->head ? 2 : 0));
            row->Child(cell);
        }
        table->Child(row);
    }
    return table;
}

El* TextView::Item(MdNode* n, Str marker, int depth) {
    const Theme& th = cx->theme();
    El* content = Div(a)->FlexCol()->Grow();

    if (n->runFirst) {
        content->Child(Inline(n, baseFont, th.foreground, 0));
    }
    Blocks(content, n, depth, true);
    return Div(a)
        ->FlexRow()
        ->W(kFill)
        ->ItemsStart()
        ->Child(TextEl(a, marker)->Font(baseFont)->Fg(th.mutedFg)->Shrink0())
        ->Child(content);
}

El* TextView::Blocks(El* into, MdNode* n, int depth, bool inList) {
    for (MdNode* c = n->first; c; c = c->next) {
        El* e = Block(c, depth, inList, c->next == nullptr);
        if (e) {
            into->Child(e);
        }
    }
    return into;
}

El* TextView::Block(MdNode* n, int depth, bool inList, bool isLast) {
    const Theme& th = cx->theme();

    float mb = (inList || isLast) ? 0.f : paragraphGap;
    switch (n->kind) {
        case MdKind::Paragraph:
            return Div(a)->W(kFill)->PadB(mb)->Child(
                Inline(n, baseFont, th.foreground, 0));
        case MdKind::Heading: {
            float font = headingFont * HeadingScale(n->level);

            return Div(a)->W(kFill)->PadB(5)->Child(
                Inline(n, font, th.foreground, HeadingWeight(n->level)));
        }
        case MdKind::Rule:
            return Div(a)->W(kFill)->PadB(mb)->Child(
                Div(a)->H(2)->W(kFill)->Bg(th.border));
        case MdKind::Quote: {
            El* inner = Div(a)
                            ->FlexCol()
                            ->W(kFill)
                            ->Fg(th.mutedFg)
                            ->BorderL(3, th.secondaryActive)
                            ->PadX(16);
            Blocks(inner, n, depth, false);
            return Div(a)->W(kFill)->PadB(mb)->Child(inner);
        }
        case MdKind::List: {
            El* list = Div(a)->FlexCol()->W(kFill)->MinW(0)->PadB(mb);
            int ix = n->start;
            for (MdNode* c = n->first; c; c = c->next) {
                if (c->kind != MdKind::Item) {
                    continue;
                }
                Str marker =
                    n->ordered ? OrderedMarker(a, ix, depth) : Bullet(depth);
                list->Child(Item(c, marker, depth + 1));
                ix++;
            }
            return list;
        }
        case MdKind::Code:
            return Div(a)->W(kFill)->PadB(mb)->Child(CodeBlock(n));
        case MdKind::Table:
            return Div(a)->W(kFill)->PadB(mb)->Child(Table(n));
        case MdKind::Item: {

            El* box = Div(a)->FlexCol()->W(kFill)->PadB(mb);
            if (n->runFirst) {
                box->Child(Inline(n, baseFont, th.foreground, 0));
            }
            return Blocks(box, n, depth, inList);
        }
        case MdKind::Html:
        case MdKind::Doc:
        case MdKind::Row:
        case MdKind::Cell:
            return nullptr;
    }
    return nullptr;
}

El* TextView::IntoEl() {
    MdNode* doc = MdParseCached(cx, a, source);
    return Blocks(Div(a)->FlexCol()->W(kFill), doc, 0, false);
}

TextView* TextView::New(Ctx* cx, Str source) {
    Arena* a = cx->a;
    TextView* t = ArenaNew<TextView>(a);
    t->a = a;
    t->cx = cx;
    t->source = source;
    return t;
}

TextView* TextView::Font(float px) {
    baseFont = px;
    return this;
}

TextView* TextView::HeadingFont(float px) {
    headingFont = px;
    return this;
}

TextView* TextView::Selectable(bool on) {
    selectable = on;
    return this;
}

TextView* TextView::TableColumnWidth(float px) {
    tableColW = px;
    return this;
}

TextView* TextView::ParagraphGap(float px) {
    paragraphGap = px;
    return this;
}

}
}

#line 1 "src/component/TitleBar.cpp"

namespace gpui {

namespace component {

#if !GPUI_OS_MAC

static El* ControlIcon(Ctx* cx, IconName icon, int clickId) {
    Arena* a = cx->a;
    const Theme& th = cx->theme();
    bool isClose = clickId == ClickWinClose;

    return Div(a)
        ->W(kTitleBarHeight)
        ->H(kFill)
        ->Shrink0()
        ->ItemsCenter()
        ->JustifyCenter()
        ->Click(clickId)
        ->HoverBg(isClose ? th.danger : th.secondaryHover)
        ->HoverFg(isClose ? th.dangerFg : th.secondaryFg)
        ->Child(IconEl(a, icon, UiIconPx(UiSize::Small)));
}

static El* WindowControls(Ctx* cx) {
    Arena* a = cx->a;
    Window* win = cx->win;
    bool maximized = win && win->maximized;
    return Div(a)
        ->FlexRow()
        ->H(kFill)
        ->ItemsCenter()
        ->Shrink0()
        ->Child(ControlIcon(cx, IconName::WindowMinimize, ClickWinMin))
        ->Child(ControlIcon(
            cx, maximized ? IconName::WindowRestore : IconName::WindowMaximize,
            ClickWinMax))
        ->Child(ControlIcon(cx, IconName::WindowClose, ClickWinClose));
}
#endif

TitleBar* TitleBar::New(Ctx* cx) {
    Arena* a = cx->a;
    const Theme& th = cx->theme();
    TitleBar* t = ArenaNew<TitleBar>(a);
    t->a = a;
    t->cx = cx;

    Rgba mixed = RgbaMix(th.titleBar, th.background, 0.55f);
    t->content =
        Div(a)->FlexRow()->H(kFill)->Grow()->ItemsCenter()->JustifyBetween();
    t->bar = Div(a)
                 ->FlexRow()
                 ->W(kFill)
                 ->H(kTitleBarHeight)
                 ->Shrink0()
                 ->PadL(kTitleBarLeftPad)
                 ->ItemsCenter()
                 ->Bg(mixed)
                 ->BorderB(1, th.titleBarBorder)
                 ->Click(ClickWinCaption)
                 ->Child(t->content);
    return t;
}

TitleBar* TitleBar::Child(El* e) {
    content->Child(e);
    return this;
}

El* TitleBar::IntoEl() {
#if !GPUI_OS_MAC
    bar->Child(WindowControls(cx));
#endif
    return bar;
}

}
}

#line 1 "src/component/Tooltip.cpp"

namespace gpui {

namespace component {

Tooltip* Tooltip::New(Ctx* cx, Str text) {
    Arena* a = cx->a;
    Tooltip* t = ArenaNew<Tooltip>(a);
    t->a = a;
    t->cx = cx;
    t->text = text;
    return t;
}

El* Tooltip::IntoEl() {
    const Theme& th = cx->theme();
    return gpui::Tooltip::New(cx, StrL("tooltip"))
        ->PadX(8)
        ->H(28)
        ->ItemsCenter()
        ->Radius(6)
        ->Bg(th.foreground)
        ->Child(TextEl(a, text)->Font(12)->Fg(th.background));
}

}
}

#line 1 "src/component/Tree.cpp"

namespace gpui {

namespace component {

Tree* Tree::New(Ctx* cx) {
    Arena* a = cx->a;
    Tree* t = ArenaNew<Tree>(a);
    t->a = a;
    t->cx = cx;
    return t;
}
Tree* Tree::Node(Str label, int parent, bool folder, bool open) {
    if (n < 16) {
        nodes[n].label = label;
        nodes[n].parent = parent;
        nodes[n].folder = folder;
        nodes[n].open = open;
        n++;
    }
    return this;
}
Tree* Tree::Selected(int i) {
    selected = i;
    return this;
}
Tree* Tree::OnSelect(Listener fn) {
    onSelect = fn;
    return this;
}

static bool Visible(Tree* t, int i) {
    int p = t->nodes[i].parent;
    while (p >= 0) {
        if (!t->nodes[p].open) {
            return false;
        }
        p = t->nodes[p].parent;
    }
    return true;
}

static int Depth(Tree* t, int i) {
    int d = 0;
    int p = t->nodes[i].parent;
    while (p >= 0) {
        d++;
        p = t->nodes[p].parent;
    }
    return d;
}

El* Tree::IntoEl() {
    const Theme& th = cx->theme();
    El* list = Div(a)->FlexCol();
    for (int i = 0; i < n; i++) {
        if (!Visible(this, i)) {
            continue;
        }
        El* row = TreeItem::New(cx, HashClickId(nodes[i].label))
                      ->H(28)
                      ->PadX(8)
                      ->ItemsCenter()
                      ->Gap(4);
        if (i == selected) {
            row->Bg(th.muted);
        }
        int d = Depth(this, i);
        if (d) {
            row->Child(Div(a)->W((float)d * 12));
        }
        if (nodes[i].folder) {
            row->Child(IconEl(a,
                              nodes[i].open ? IconName::ChevronDown
                                            : IconName::ChevronRight,
                              12)
                           ->Fg(th.mutedFg));
        } else {
            row->Child(Div(a)->W(12));
        }
        row->Child(TextEl(a, nodes[i].label)->Font(13)->Fg(th.foreground));
        if (onSelect.IsValid()) {
            row->OnClick(ListenerArg(onSelect, i));
        }
        list->Child(row);
    }
    return gpui::Tree::New(cx)
        ->W(256)
        ->H(192)
        ->Border(1, th.border)
        ->ClipY()
        ->Child(list);
}

}
}

#line 1 "src/component/VirtualList.cpp"

namespace gpui {

namespace component {

VirtualList* VirtualList::New(Ctx* cx, int count) {
    Arena* a = cx->a;
    VirtualList* v = ArenaNew<VirtualList>(a);
    v->a = a;
    v->cx = cx;
    v->count = count;
    return v;
}
VirtualList* VirtualList::RowH(float v) {
    rowH = v;
    return this;
}
VirtualList* VirtualList::ViewH(float v) {
    viewH = v;
    return this;
}
VirtualList* VirtualList::ScrollY(float v) {
    scrollY = v;
    return this;
}
VirtualList* VirtualList::Row(El* (*fn)(Arena*, int)) {
    row = fn;
    return this;
}

El* VirtualList::IntoEl() {
    const Theme& th = cx->theme();
    int first = (int)(scrollY / rowH);
    if (first < 0) {
        first = 0;
    }
    int visible = (int)(viewH / rowH) + 2;
    El* list = Div(a)->FlexCol();
    if (first > 0) {
        list->Child(Div(a)->H((float)first * rowH));
    }
    for (int i = 0; i < visible; i++) {
        int ix = first + i;
        if (ix >= count) {
            break;
        }
        if (row) {
            list->Child(row(a, ix));
        } else {
            list->Child(Div(a)->H(rowH)->PadX(8)->ItemsCenter()->Child(
                TextEl(a, StrDup(a, fmt("Item %d", ix)))
                    ->Font(12)
                    ->Fg(th.foreground)));
        }
    }
    return gpui::VirtualList::New(cx, StrL("vlist"))
        ->H(viewH)
        ->ClipY()
        ->Child(list);
}

}
}

#line 1 "src/component/WindowBorder.cpp"

namespace gpui {

namespace component {

WindowBorder* WindowBorder::New(Ctx* cx) {
    Arena* a = cx->a;
    WindowBorder* w = ArenaNew<WindowBorder>(a);
    w->a = a;
    w->cx = cx;
    return w;
}
WindowBorder* WindowBorder::Child(El* e) {
    child = e;
    return this;
}

El* WindowBorder::IntoEl() {
    El* e = Div(a)->SizeFull()->Border(1, cx->theme().border);
    if (child) {
        e->Child(child);
    }
    return e;
}

}
}

#line 1 "src/sys/SysInfo.cpp"

namespace gpui {

static int CmpStrI(const char* a, const char* b) {
    return StrCmpI(a, b);
}

struct SortCtx {
    ProcessSort field;
    bool desc;
};

static int CmpProc(const ProcessInfo* a, const ProcessInfo* b,
                   ProcessSort field, bool desc) {
    int c = 0;
    switch (field) {
        case ProcessSort::Pid:
            c = (a->pid > b->pid) - (a->pid < b->pid);
            break;
        case ProcessSort::Name:
            c = CmpStrI(a->name, b->name);
            break;
        case ProcessSort::Cpu:
            c = (a->cpu > b->cpu) - (a->cpu < b->cpu);
            if (c == 0) {
                c = (a->memory > b->memory) - (a->memory < b->memory);
            }
            break;
        case ProcessSort::Memory:
            c = (a->memory > b->memory) - (a->memory < b->memory);
            break;
    }
    return desc ? -c : c;
}

static ProcessSort gSortField = ProcessSort::Cpu;
static bool gSortDesc = true;

static int QsortProc(const void* x, const void* y) {
    return CmpProc((const ProcessInfo*)x, (const ProcessInfo*)y, gSortField,
                   gSortDesc);
}

void SysSortProcesses(SysState* s, ProcessSort field, bool descending,
                      int keepTop) {
    if (s->procs.len <= 1) {
        return;
    }
    gSortField = field;
    gSortDesc = descending;
    qsort(s->procs.els, (size_t)s->procs.len, sizeof(ProcessInfo), QsortProc);
    if (keepTop > 0 && s->procs.len > keepTop) {
        s->procs.len = keepTop;
    }
}

TempStr FormatBytes(uint64_t bytes) {
    const uint64_t KB = 1024;
    const uint64_t MB = KB * 1024;
    const uint64_t GB = MB * 1024;
    if (bytes >= GB) {
        return fmt("%.1f GB", (double)bytes / (double)GB);
    }
    if (bytes >= MB) {
        return fmt("%.1f MB", (double)bytes / (double)MB);
    }
    if (bytes >= KB) {
        return fmt("%.1f KB", (double)bytes / (double)KB);
    }
    return fmt("%d B", (int)bytes);
}

TempStr FormatPct(float v, int decimals) {
    if (decimals <= 0) {
        return fmt("%.0f%%", v);
    }
    return fmt("%.1f%%", v);
}
}

#if GPUI_OS_LINUX
#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <dirent.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

#if GPUI_OS_MAC
#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

#if GPUI_OS_WINDOWS
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <ole2.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#endif

#if GPUI_OS_LINUX || GPUI_OS_MAC
#include <dirent.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if GPUI_OS_LINUX
#line 1 "src/gpui/Paint_linux.cpp"

namespace gpui {

struct PaintApp {

    PangoContext* pango = nullptr;
};

struct PaintTarget {
    cairo_t* cr = nullptr;
};

PaintApp* PaintAppNew() {
    auto* pa = new PaintApp();
    PangoFontMap* map = pango_cairo_font_map_get_default();
    if (!map) {
        delete pa;
        return nullptr;
    }
    pa->pango = pango_font_map_create_context(map);
    if (!pa->pango) {
        delete pa;
        return nullptr;
    }

    cairo_font_options_t* fo = cairo_font_options_create();
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options(pa->pango, fo);
    cairo_font_options_destroy(fo);

    pango_cairo_context_set_resolution(pa->pango, 96.0);
    return pa;
}

void PaintAppFree(PaintApp* pa) {
    if (!pa) {
        return;
    }
    if (pa->pango) {
        g_object_unref(pa->pango);
    }
    delete pa;
}

void PaintTargetFree(PaintCtx* ctx) {
    if (!ctx || !ctx->rt) {
        return;
    }
    if (ctx->rt->cr) {
        cairo_destroy(ctx->rt->cr);
    }
    delete ctx->rt;
    ctx->rt = nullptr;
}

bool PaintTargetBegin(PaintCtx* ctx, void* native, int pxW, int pxH) {
    (void)pxW;
    (void)pxH;
    if (!ctx || !ctx->pa || !native) {
        return false;
    }
    PaintTargetFree(ctx);
    auto* t = new PaintTarget();
    t->cr = cairo_create((cairo_surface_t*)native);
    if (!t->cr || cairo_status(t->cr) != CAIRO_STATUS_SUCCESS) {
        if (t->cr) {
            cairo_destroy(t->cr);
        }
        delete t;
        return false;
    }
    ctx->rt = t;
    cairo_set_antialias(t->cr, CAIRO_ANTIALIAS_DEFAULT);
    return true;
}

bool PaintTargetEnd(PaintCtx* ctx) {
    if (!ctx || !ctx->rt || !ctx->rt->cr) {
        return false;
    }
    cairo_surface_t* surf = cairo_get_target(ctx->rt->cr);
    if (surf) {
        cairo_surface_flush(surf);
    }
    PaintTargetFree(ctx);
    return true;
}

static cairo_t* Cr(PaintCtx* ctx) {
    return (ctx && ctx->rt) ? ctx->rt->cr : nullptr;
}

static void SetColor(cairo_t* cr, Rgba c) {
    cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0,
                          c.a / 255.0);
}

static void SetDash(cairo_t* cr, const float* dash, float stroke) {
    if (!dash) {
        cairo_set_dash(cr, nullptr, 0, 0);
        return;
    }
    double d[2] = {dash[0] * stroke, dash[1] * stroke};
    cairo_set_dash(cr, d, 2, 0);
}

static void RoundRectPath(cairo_t* cr, float x, float y, float w, float h,
                          float r) {
    float rmax = (w < h ? w : h) * 0.5f;
    if (r > rmax) {
        r = rmax;
    }
    if (r <= 0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -kPi / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, kPi / 2);
    cairo_arc(cr, x + r, y + h - r, r, kPi / 2, kPi);
    cairo_arc(cr, x + r, y + r, r, kPi, 3 * kPi / 2);
    cairo_close_path(cr);
}

void CanvasClear(PaintCtx* ctx, Rgba c) {
    cairo_t* cr = Cr(ctx);
    if (!cr) {
        return;
    }
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    SetColor(cr, c);
    cairo_paint(cr);
    cairo_restore(cr);
}

void CanvasFillRect(PaintCtx* ctx, float x, float y, float w, float h, Rgba c) {
    cairo_t* cr = Cr(ctx);
    if (!cr || w <= 0 || h <= 0 || c.a == 0) {
        return;
    }
    SetColor(cr, c);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

void CanvasFillRound(PaintCtx* ctx, float x, float y, float w, float h, float r,
                     Rgba c) {
    cairo_t* cr = Cr(ctx);
    if (!cr || w <= 0 || h <= 0 || c.a == 0) {
        return;
    }
    SetColor(cr, c);
    RoundRectPath(cr, x, y, w, h, r);
    cairo_fill(cr);
}

void CanvasStrokeRound(PaintCtx* ctx, float x, float y, float w, float h,
                       float r, float stroke, Rgba c, const float* dash) {
    cairo_t* cr = Cr(ctx);
    if (!cr || stroke <= 0 || w <= 0 || h <= 0) {
        return;
    }
    SetColor(cr, c);
    cairo_set_line_width(cr, stroke);
    SetDash(cr, dash, stroke);

    RoundRectPath(cr, x + stroke * 0.5f, y + stroke * 0.5f, w - stroke,
                  h - stroke, r);
    cairo_stroke(cr);
    cairo_set_dash(cr, nullptr, 0, 0);
}

void CanvasLine(PaintCtx* ctx, float x1, float y1, float x2, float y2,
                float stroke, Rgba c, const float* dash) {
    cairo_t* cr = Cr(ctx);
    if (!cr) {
        return;
    }
    SetColor(cr, c);
    cairo_set_line_width(cr, stroke);
    SetDash(cr, dash, stroke);
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x2, y2);
    cairo_stroke(cr);
    cairo_set_dash(cr, nullptr, 0, 0);
}

void CanvasEllipse(PaintCtx* ctx, float cx, float cy, float rx, float ry,
                   float stroke, Rgba c) {
    cairo_t* cr = Cr(ctx);
    if (!cr || rx <= 0 || ry <= 0) {
        return;
    }
    SetColor(cr, c);
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, rx, ry);
    cairo_new_sub_path(cr);
    cairo_arc(cr, 0, 0, 1, 0, 2 * kPi);
    cairo_restore(cr);
    if (stroke > 0) {
        cairo_set_line_width(cr, stroke);
        cairo_stroke(cr);
    } else {
        cairo_fill(cr);
    }
}

void CanvasPushClip(PaintCtx* ctx, float x, float y, float w, float h) {
    cairo_t* cr = Cr(ctx);
    if (!cr) {
        return;
    }
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
}

void CanvasPopClip(PaintCtx* ctx) {
    cairo_t* cr = Cr(ctx);
    if (cr) {
        cairo_restore(cr);
    }
}

enum PathCmd : uint8_t {
    kPathMove,
    kPathLine,
    kPathCubic,
    kPathArc,
    kPathClose
};

struct PathOp {
    PathCmd cmd = kPathMove;
    bool clockwise = false;
    float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
};

struct Path {
    Vec<PathOp> ops;
    bool winding = true;
    bool fig = false;
};

Path* PathNew(PaintCtx* ctx, bool winding) {
    if (!ctx) {
        return nullptr;
    }
    auto* p = new Path();
    p->winding = winding;
    return p;
}

void PathFree(Path* p) {
    delete p;
}

static void gpui_Paint_linux_Push(Path* p, const PathOp& op) {
    if (p) {
        p->ops.Append(op);
    }
}

void PathMoveTo(Path* p, float x, float y) {
    if (!p) {
        return;
    }
    PathOp op;
    op.cmd = kPathMove;
    op.a = x;
    op.b = y;
    gpui_Paint_linux_Push(p, op);
    p->fig = true;
}

void PathLineTo(Path* p, float x, float y) {
    if (!p) {
        return;
    }
    if (!p->fig) {
        PathMoveTo(p, x, y);
        return;
    }
    PathOp op;
    op.cmd = kPathLine;
    op.a = x;
    op.b = y;
    gpui_Paint_linux_Push(p, op);
}

void PathCubicTo(Path* p, float x1, float y1, float x2, float y2, float x,
                 float y) {
    if (!p) {
        return;
    }
    if (!p->fig) {
        PathMoveTo(p, x, y);
        return;
    }
    PathOp op;
    op.cmd = kPathCubic;
    op.a = x1;
    op.b = y1;
    op.c = x2;
    op.d = y2;
    op.e = x;
    op.f = y;
    gpui_Paint_linux_Push(p, op);
}

void PathArcTo(Path* p, float cx, float cy, float r, float a0, float a1,
               bool clockwise) {
    if (!p) {
        return;
    }
    PathOp op;
    op.cmd = kPathArc;
    op.clockwise = clockwise;
    op.a = cx;
    op.b = cy;
    op.c = r;
    op.d = a0;
    op.e = a1;
    gpui_Paint_linux_Push(p, op);

    p->fig = true;
}

void PathClose(Path* p) {
    if (!p || !p->fig) {
        return;
    }
    PathOp op;
    op.cmd = kPathClose;
    gpui_Paint_linux_Push(p, op);
    p->fig = false;
}

static bool Replay(cairo_t* cr, Path* p) {
    if (!cr || !p || p->ops.len == 0) {
        return false;
    }
    cairo_new_path(cr);
    cairo_set_fill_rule(
        cr, p->winding ? CAIRO_FILL_RULE_WINDING : CAIRO_FILL_RULE_EVEN_ODD);
    for (int i = 0; i < p->ops.len; i++) {
        const PathOp& o = p->ops[i];
        switch (o.cmd) {
            case kPathMove:
                cairo_move_to(cr, o.a, o.b);
                break;
            case kPathLine:
                cairo_line_to(cr, o.a, o.b);
                break;
            case kPathCubic:
                cairo_curve_to(cr, o.a, o.b, o.c, o.d, o.e, o.f);
                break;
            case kPathArc:
                if (o.clockwise) {
                    cairo_arc(cr, o.a, o.b, o.c, o.d, o.e);
                } else {
                    cairo_arc_negative(cr, o.a, o.b, o.c, o.d, o.e);
                }
                break;
            case kPathClose:
                cairo_close_path(cr);
                break;
        }
    }
    return true;
}

void PathFill(PaintCtx* ctx, Path* p, Rgba c) {
    cairo_t* cr = Cr(ctx);
    if (!Replay(cr, p)) {
        return;
    }
    SetColor(cr, c);
    cairo_fill(cr);
}

void PathFillGradientV(PaintCtx* ctx, Path* p, float y0, float y1, Rgba top,
                       Rgba bot) {
    cairo_t* cr = Cr(ctx);
    if (!Replay(cr, p)) {
        return;
    }
    cairo_pattern_t* pat = cairo_pattern_create_linear(0, y0, 0, y1);
    if (!pat) {
        SetColor(cr, top);
        cairo_fill(cr);
        return;
    }
    cairo_pattern_add_color_stop_rgba(pat, 0, top.r / 255.0, top.g / 255.0,
                                      top.b / 255.0, top.a / 255.0);
    cairo_pattern_add_color_stop_rgba(pat, 1, bot.r / 255.0, bot.g / 255.0,
                                      bot.b / 255.0, bot.a / 255.0);
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
}

void PathStroke(PaintCtx* ctx, Path* p, float stroke, Rgba c, bool roundCaps) {
    cairo_t* cr = Cr(ctx);
    if (!Replay(cr, p)) {
        return;
    }
    SetColor(cr, c);
    cairo_set_line_width(cr, stroke);
    cairo_set_line_cap(cr,
                       roundCaps ? CAIRO_LINE_CAP_ROUND : CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(
        cr, roundCaps ? CAIRO_LINE_JOIN_ROUND : CAIRO_LINE_JOIN_MITER);
    cairo_stroke(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
}

struct TextLayout {
    PangoLayout* layout = nullptr;
    int refs = 1;

    float box = 0;
    float natural = 0;
    int lines = 1;
};

static const char* kSans = "Sans";
static const char* kMono = "Monospace";

static PangoWeight PangoWeightFor(uint8_t weight, float fontSize) {
    switch (weight & kFontWeightMask) {
        case kFontWeightBold:
            return PANGO_WEIGHT_BOLD;
        case kFontWeightSemibold:
            return PANGO_WEIGHT_SEMIBOLD;
        case kFontWeightMedium:
            return PANGO_WEIGHT_MEDIUM;
        default:
            break;
    }

    return fontSize >= 18.f ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL;
}

TextLayout* TextLayoutNew(PaintCtx* ctx, Str s, float fontSize, float maxW,
                          bool wrap, uint8_t weight, float lineH,
                          Size* outSize) {
    if (!ctx || !ctx->pa || !ctx->pa->pango || !s.s || s.len <= 0) {
        return nullptr;
    }
    if (fontSize <= 0) {
        fontSize = 16.f;
    }
    PangoLayout* l = pango_layout_new(ctx->pa->pango);
    if (!l) {
        return nullptr;
    }
    PangoFontDescription* fd = pango_font_description_new();
    pango_font_description_set_family(fd, (weight & kFontMono) ? kMono : kSans);
    pango_font_description_set_weight(fd, PangoWeightFor(weight, fontSize));
    if (weight & kFontItalic) {
        pango_font_description_set_style(fd, PANGO_STYLE_ITALIC);
    }
    pango_font_description_set_absolute_size(fd,
                                             (double)fontSize * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);

    if (weight & kFontUnderline) {
        PangoAttrList* attrs = pango_attr_list_new();
        pango_attr_list_insert(
            attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
        pango_layout_set_attributes(l, attrs);
        pango_attr_list_unref(attrs);
    }

    pango_layout_set_text(l, s.s, s.len);
    if (wrap && maxW > 0) {
        pango_layout_set_width(l, (int)(maxW * PANGO_SCALE));
        pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    } else {
        pango_layout_set_width(l, -1);
    }

    auto* tl = new TextLayout();
    tl->layout = l;
    tl->lines = pango_layout_get_line_count(l);
    if (tl->lines < 1) {
        tl->lines = 1;
    }
    int pw = 0, ph = 0;
    pango_layout_get_pixel_size(l, &pw, &ph);
    tl->natural = (float)ph / (float)tl->lines;
    tl->box = fontSize * (lineH > 0 ? lineH : kLineHeight);

    if (tl->lines > 1) {
        pango_layout_set_spacing(l,
                                 (int)((tl->box - tl->natural) * PANGO_SCALE));
        pango_layout_get_pixel_size(l, &pw, &ph);
    }
    if (outSize) {
        outSize->w = (float)pw;
        outSize->h = tl->box * (float)tl->lines;
    }
    return tl;
}

void TextLayoutAddRef(TextLayout* tl) {
    if (tl) {
        tl->refs++;
    }
}

void TextLayoutRelease(TextLayout* tl) {
    if (!tl) {
        return;
    }
    if (--tl->refs > 0) {
        return;
    }
    if (tl->layout) {
        g_object_unref(tl->layout);
    }
    delete tl;
}

static float BoxPad(TextLayout* tl) {
    return (tl->box - tl->natural) * 0.5f;
}

void TextLayoutDraw(PaintCtx* ctx, TextLayout* tl, float x, float y, Rgba c,
                    bool clip) {
    cairo_t* cr = Cr(ctx);
    if (!cr || !tl || !tl->layout) {
        return;
    }
    if (clip) {
        int pw = 0, ph = 0;
        pango_layout_get_pixel_size(tl->layout, &pw, &ph);
        int w = pango_layout_get_width(tl->layout);
        float boxW = w > 0 ? (float)w / PANGO_SCALE : (float)pw;
        cairo_save(cr);
        cairo_rectangle(cr, x, y, boxW, tl->box * (float)tl->lines);
        cairo_clip(cr);
    }
    SetColor(cr, c);
    cairo_move_to(cr, x, y + BoxPad(tl));
    pango_cairo_show_layout(cr, tl->layout);
    if (clip) {
        cairo_restore(cr);
    }
}

int TextLayoutHitPoint(TextLayout* tl, Str s, float relX, float relY) {
    if (!tl || !tl->layout) {
        return 0;
    }
    int index = 0;
    int trailing = 0;
    pango_layout_xy_to_index(tl->layout, (int)(relX * PANGO_SCALE),
                             (int)((relY - BoxPad(tl)) * PANGO_SCALE), &index,
                             &trailing);

    const char* text = pango_layout_get_text(tl->layout);
    while (trailing > 0 && text && text[index]) {
        index = (int)(g_utf8_next_char(text + index) - text);
        trailing--;
    }
    if (index < 0) {
        index = 0;
    }
    if (index > s.len) {
        index = s.len;
    }
    return index;
}

int TextLayoutRangeRects(TextLayout* tl, Str s, int u8a, int u8b, Bounds* out,
                         int max) {
    if (!tl || !tl->layout || !out || max <= 0 || u8a >= u8b) {
        return 0;
    }
    (void)s;
    float pad = BoxPad(tl);
    int n = 0;
    PangoLayoutIter* iter = pango_layout_get_iter(tl->layout);
    if (!iter) {
        return 0;
    }
    do {
        PangoLayoutLine* line = pango_layout_iter_get_line_readonly(iter);
        if (!line) {
            continue;
        }
        int lineStart = line->start_index;
        int lineEnd = lineStart + line->length;
        int lo = u8a > lineStart ? u8a : lineStart;
        int hi = u8b < lineEnd ? u8b : lineEnd;
        if (lo >= hi) {
            continue;
        }
        int x0 = 0, x1 = 0;
        pango_layout_line_index_to_x(line, lo, FALSE, &x0);
        pango_layout_line_index_to_x(line, hi, FALSE, &x1);
        int y0 = 0, y1 = 0;
        pango_layout_iter_get_line_yrange(iter, &y0, &y1);
        float left = (float)x0 / PANGO_SCALE;
        float right = (float)x1 / PANGO_SCALE;
        if (right < left) {
            float t = left;
            left = right;
            right = t;
        }
        out[n].x = left;
        out[n].y = (float)y0 / PANGO_SCALE + pad;
        out[n].w = right - left;
        out[n].h = (float)(y1 - y0) / PANGO_SCALE;
        n++;
    } while (n < max && pango_layout_iter_next_line(iter));
    pango_layout_iter_free(iter);
    return n;
}

}

#endif

#if GPUI_OS_MAC
#line 1 "src/gpui/Paint_mac.cpp"

namespace gpui {

struct FontSlot {
    float size = 0;
    uint8_t weight = 0;
    CTFontRef font = nullptr;
};

enum {
    kFontCacheCap = 32
};

struct PaintApp {
    FontSlot fonts[kFontCacheCap] = {};
    int nFonts = 0;
};

struct PaintTarget {
    CGContextRef cg = nullptr;
};

PaintApp* PaintAppNew() {
    return new PaintApp();
}

void PaintAppFree(PaintApp* pa) {
    if (!pa) {
        return;
    }
    for (int i = 0; i < pa->nFonts; i++) {
        if (pa->fonts[i].font) {
            CFRelease(pa->fonts[i].font);
        }
    }
    delete pa;
}

void PaintTargetFree(PaintCtx* ctx) {
    if (!ctx || !ctx->rt) {
        return;
    }
    delete ctx->rt;
    ctx->rt = nullptr;
}

bool PaintTargetBegin(PaintCtx* ctx, void* native, int pxW, int pxH) {
    (void)pxW;
    (void)pxH;
    if (!ctx || !ctx->pa || !native) {
        return false;
    }
    PaintTargetFree(ctx);
    auto* t = new PaintTarget();
    t->cg = (CGContextRef)native;
    ctx->rt = t;
    CGContextSetShouldAntialias(t->cg, true);

    CGContextSetTextMatrix(t->cg, CGAffineTransformMakeScale(1, -1));
    return true;
}

bool PaintTargetEnd(PaintCtx* ctx) {
    if (!ctx || !ctx->rt) {
        return false;
    }
    PaintTargetFree(ctx);
    return true;
}

static CGContextRef Cg(PaintCtx* ctx) {
    return (ctx && ctx->rt) ? ctx->rt->cg : nullptr;
}

static void SetFill(CGContextRef cg, Rgba c) {
    CGContextSetRGBFillColor(cg, c.r / 255.0, c.g / 255.0, c.b / 255.0,
                             c.a / 255.0);
}

static void SetStroke(CGContextRef cg, Rgba c) {
    CGContextSetRGBStrokeColor(cg, c.r / 255.0, c.g / 255.0, c.b / 255.0,
                               c.a / 255.0);
}

static void SetDash(CGContextRef cg, const float* dash, float stroke) {
    if (!dash) {
        CGContextSetLineDash(cg, 0, nullptr, 0);
        return;
    }
    CGFloat d[2] = {dash[0] * stroke, dash[1] * stroke};
    CGContextSetLineDash(cg, 0, d, 2);
}

static CGPathRef RoundRectPath(float x, float y, float w, float h, float r) {
    CGRect rect = CGRectMake(x, y, w, h);
    float rmax = (w < h ? w : h) * 0.5f;
    if (r > rmax) {
        r = rmax;
    }
    if (r <= 0) {
        return CGPathCreateWithRect(rect, nullptr);
    }
    return CGPathCreateWithRoundedRect(rect, r, r, nullptr);
}

void CanvasClear(PaintCtx* ctx, Rgba c) {
    CGContextRef cg = Cg(ctx);
    if (!cg) {
        return;
    }
    SetFill(cg, c);
    CGContextFillRect(cg, CGContextGetClipBoundingBox(cg));
}

void CanvasFillRect(PaintCtx* ctx, float x, float y, float w, float h, Rgba c) {
    CGContextRef cg = Cg(ctx);
    if (!cg || w <= 0 || h <= 0 || c.a == 0) {
        return;
    }
    SetFill(cg, c);
    CGContextFillRect(cg, CGRectMake(x, y, w, h));
}

void CanvasFillRound(PaintCtx* ctx, float x, float y, float w, float h, float r,
                     Rgba c) {
    CGContextRef cg = Cg(ctx);
    if (!cg || w <= 0 || h <= 0 || c.a == 0) {
        return;
    }
    CGPathRef path = RoundRectPath(x, y, w, h, r);
    SetFill(cg, c);
    CGContextAddPath(cg, path);
    CGContextFillPath(cg);
    CGPathRelease(path);
}

void CanvasStrokeRound(PaintCtx* ctx, float x, float y, float w, float h,
                       float r, float stroke, Rgba c, const float* dash) {
    CGContextRef cg = Cg(ctx);
    if (!cg || stroke <= 0 || w <= 0 || h <= 0) {
        return;
    }

    CGPathRef path = RoundRectPath(x + stroke * 0.5f, y + stroke * 0.5f,
                                   w - stroke, h - stroke, r);
    SetStroke(cg, c);
    CGContextSetLineWidth(cg, stroke);
    SetDash(cg, dash, stroke);
    CGContextAddPath(cg, path);
    CGContextStrokePath(cg);
    CGContextSetLineDash(cg, 0, nullptr, 0);
    CGPathRelease(path);
}

void CanvasLine(PaintCtx* ctx, float x1, float y1, float x2, float y2,
                float stroke, Rgba c, const float* dash) {
    CGContextRef cg = Cg(ctx);
    if (!cg) {
        return;
    }
    SetStroke(cg, c);
    CGContextSetLineWidth(cg, stroke);
    SetDash(cg, dash, stroke);
    CGContextBeginPath(cg);
    CGContextMoveToPoint(cg, x1, y1);
    CGContextAddLineToPoint(cg, x2, y2);
    CGContextStrokePath(cg);
    CGContextSetLineDash(cg, 0, nullptr, 0);
}

void CanvasEllipse(PaintCtx* ctx, float cx, float cy, float rx, float ry,
                   float stroke, Rgba c) {
    CGContextRef cg = Cg(ctx);
    if (!cg || rx <= 0 || ry <= 0) {
        return;
    }
    CGRect box = CGRectMake(cx - rx, cy - ry, rx * 2, ry * 2);
    CGContextBeginPath(cg);
    CGContextAddEllipseInRect(cg, box);
    if (stroke > 0) {
        SetStroke(cg, c);
        CGContextSetLineWidth(cg, stroke);
        CGContextStrokePath(cg);
    } else {
        SetFill(cg, c);
        CGContextFillPath(cg);
    }
}

void CanvasPushClip(PaintCtx* ctx, float x, float y, float w, float h) {
    CGContextRef cg = Cg(ctx);
    if (!cg) {
        return;
    }
    CGContextSaveGState(cg);
    CGContextClipToRect(cg, CGRectMake(x, y, w, h));
}

void CanvasPopClip(PaintCtx* ctx) {
    CGContextRef cg = Cg(ctx);
    if (cg) {
        CGContextRestoreGState(cg);
    }
}

struct Path {
    CGMutablePathRef path = nullptr;
    bool winding = true;
    bool fig = false;
    float cx = 0, cy = 0;
};

Path* PathNew(PaintCtx* ctx, bool winding) {
    if (!ctx) {
        return nullptr;
    }
    auto* p = new Path();
    p->path = CGPathCreateMutable();
    p->winding = winding;
    return p;
}

void PathFree(Path* p) {
    if (!p) {
        return;
    }
    if (p->path) {
        CGPathRelease(p->path);
    }
    delete p;
}

void PathMoveTo(Path* p, float x, float y) {
    if (!p) {
        return;
    }
    CGPathMoveToPoint(p->path, nullptr, x, y);
    p->fig = true;
    p->cx = x;
    p->cy = y;
}

void PathLineTo(Path* p, float x, float y) {
    if (!p) {
        return;
    }
    if (!p->fig) {
        PathMoveTo(p, x, y);
        return;
    }
    CGPathAddLineToPoint(p->path, nullptr, x, y);
    p->cx = x;
    p->cy = y;
}

void PathCubicTo(Path* p, float x1, float y1, float x2, float y2, float x,
                 float y) {
    if (!p) {
        return;
    }
    if (!p->fig) {
        PathMoveTo(p, x, y);
        return;
    }
    CGPathAddCurveToPoint(p->path, nullptr, x1, y1, x2, y2, x, y);
    p->cx = x;
    p->cy = y;
}

void PathArcTo(Path* p, float cx, float cy, float r, float a0, float a1,
               bool clockwise) {
    if (!p) {
        return;
    }

    CGPathAddArc(p->path, nullptr, cx, cy, r, a0, a1, !clockwise);
    p->fig = true;
    p->cx = cx + r * cosf(a1);
    p->cy = cy + r * sinf(a1);
}

void PathClose(Path* p) {
    if (!p || !p->fig) {
        return;
    }
    CGPathCloseSubpath(p->path);
    p->fig = false;
}

void PathFill(PaintCtx* ctx, Path* p, Rgba c) {
    CGContextRef cg = Cg(ctx);
    if (!cg || !p || CGPathIsEmpty(p->path)) {
        return;
    }
    SetFill(cg, c);
    CGContextAddPath(cg, p->path);
    if (p->winding) {
        CGContextFillPath(cg);
    } else {
        CGContextEOFillPath(cg);
    }
}

void PathFillGradientV(PaintCtx* ctx, Path* p, float y0, float y1, Rgba top,
                       Rgba bot) {
    CGContextRef cg = Cg(ctx);
    if (!cg || !p || CGPathIsEmpty(p->path)) {
        return;
    }
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGFloat comps[8] = {top.r / 255.0, top.g / 255.0, top.b / 255.0,
                        top.a / 255.0, bot.r / 255.0, bot.g / 255.0,
                        bot.b / 255.0, bot.a / 255.0};
    CGFloat stops[2] = {0.0, 1.0};
    CGGradientRef grad =
        CGGradientCreateWithColorComponents(space, comps, stops, 2);
    CGColorSpaceRelease(space);
    if (!grad) {
        PathFill(ctx, p, top);
        return;
    }
    CGContextSaveGState(cg);
    CGContextAddPath(cg, p->path);
    if (p->winding) {
        CGContextClip(cg);
    } else {
        CGContextEOClip(cg);
    }
    CGContextDrawLinearGradient(
        cg, grad, CGPointMake(0, y0), CGPointMake(0, y1),
        kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    CGContextRestoreGState(cg);
    CGGradientRelease(grad);
}

void PathStroke(PaintCtx* ctx, Path* p, float stroke, Rgba c, bool roundCaps) {
    CGContextRef cg = Cg(ctx);
    if (!cg || !p || CGPathIsEmpty(p->path)) {
        return;
    }
    SetStroke(cg, c);
    CGContextSetLineWidth(cg, stroke);
    CGContextSetLineCap(cg, roundCaps ? kCGLineCapRound : kCGLineCapButt);
    CGContextSetLineJoin(cg, roundCaps ? kCGLineJoinRound : kCGLineJoinMiter);
    CGContextAddPath(cg, p->path);
    CGContextStrokePath(cg);
    CGContextSetLineCap(cg, kCGLineCapButt);
    CGContextSetLineJoin(cg, kCGLineJoinMiter);
}

static int Utf8Decode(const char* s, int len, uint32_t* out) {
    if (len <= 0) {
        return 0;
    }
    auto b = (const unsigned char*)s;
    if (b[0] < 0x80) {
        *out = b[0];
        return 1;
    }
    if ((b[0] & 0xe0) == 0xc0 && len >= 2) {
        *out = ((uint32_t)(b[0] & 0x1f) << 6) | (b[1] & 0x3f);
        return 2;
    }
    if ((b[0] & 0xf0) == 0xe0 && len >= 3) {
        *out = ((uint32_t)(b[0] & 0x0f) << 12) |
               ((uint32_t)(b[1] & 0x3f) << 6) | (b[2] & 0x3f);
        return 3;
    }
    if ((b[0] & 0xf8) == 0xf0 && len >= 4) {
        *out = ((uint32_t)(b[0] & 0x07) << 18) |
               ((uint32_t)(b[1] & 0x3f) << 12) |
               ((uint32_t)(b[2] & 0x3f) << 6) | (b[3] & 0x3f);
        return 4;
    }
    *out = 0xfffd;
    return 1;
}

static int Utf8OffToU16(Str s, int u8off) {
    if (!s.s || u8off <= 0) {
        return 0;
    }
    if (u8off > s.len) {
        u8off = s.len;
    }
    int i = 0;
    int u16 = 0;
    while (i < u8off) {
        uint32_t cp = 0;
        int adv = Utf8Decode(s.s + i, s.len - i, &cp);
        if (adv <= 0) {
            break;
        }
        i += adv;
        u16 += cp >= 0x10000 ? 2 : 1;
    }
    return u16;
}

static int U16OffToUtf8(Str s, int u16off) {
    if (!s.s || u16off <= 0) {
        return 0;
    }
    int i = 0;
    int u16 = 0;
    while (i < s.len && u16 < u16off) {
        uint32_t cp = 0;
        int adv = Utf8Decode(s.s + i, s.len - i, &cp);
        if (adv <= 0) {
            break;
        }
        i += adv;
        u16 += cp >= 0x10000 ? 2 : 1;
    }
    return i;
}

struct MacLine {
    CTLineRef line = nullptr;
    int start = 0;
    int len = 0;
    float width = 0;
};

struct TextLayout {
    int refs = 1;
    CFAttributedStringRef attr = nullptr;
    MacLine* lines = nullptr;
    int nLines = 0;

    float box = 0;
    float baseline = 0;
    float width = 0;
};

static NSFontWeight WeightFor(uint8_t weight, float fontSize) {
    switch (weight & kFontWeightMask) {
        case kFontWeightBold:
            return NSFontWeightBold;
        case kFontWeightSemibold:
            return NSFontWeightSemibold;
        case kFontWeightMedium:
            return NSFontWeightMedium;
        default:
            break;
    }

    return fontSize >= 18.f ? NSFontWeightSemibold : NSFontWeightRegular;
}

static CTFontRef FontFor(PaintApp* pa, float fontSize, uint8_t weight) {
    for (int i = 0; i < pa->nFonts; i++) {
        if (pa->fonts[i].size == fontSize && pa->fonts[i].weight == weight) {
            return pa->fonts[i].font;
        }
    }
    NSFontWeight w = WeightFor(weight, fontSize);
    NSFont* font = nil;
    if (weight & kFontMono) {
        if (@available(macOS 10.15, *)) {
            font = [NSFont monospacedSystemFontOfSize:fontSize weight:w];
        }
        if (!font) {
            font = [NSFont fontWithName:@"Menlo" size:fontSize];
        }
    } else {
        font = [NSFont systemFontOfSize:fontSize weight:w];
    }
    if (!font) {
        font = [NSFont systemFontOfSize:fontSize];
    }
    if ((weight & kFontItalic) && font) {
        NSFont* italic =
            [[NSFontManager sharedFontManager] convertFont:font
                                               toHaveTrait:NSItalicFontMask];
        if (italic) {
            font = italic;
        }
    }
    if (!font) {
        return nullptr;
    }
    auto ct = (CTFontRef)CFBridgingRetain(font);
    if (pa->nFonts >= kFontCacheCap) {

        CFRelease(pa->fonts[0].font);
        for (int i = 1; i < kFontCacheCap; i++) {
            pa->fonts[i - 1] = pa->fonts[i];
        }
        pa->nFonts = kFontCacheCap - 1;
    }
    pa->fonts[pa->nFonts].size = fontSize;
    pa->fonts[pa->nFonts].weight = weight;
    pa->fonts[pa->nFonts].font = ct;
    pa->nFonts++;
    return ct;
}

TextLayout* TextLayoutNew(PaintCtx* ctx, Str s, float fontSize, float maxW,
                          bool wrap, uint8_t weight, float lineH,
                          Size* outSize) {
    if (!ctx || !ctx->pa || !s.s || s.len <= 0) {
        return nullptr;
    }
    if (fontSize <= 0) {
        fontSize = 16.f;
    }
    CTFontRef font = FontFor(ctx->pa, fontSize, weight);
    if (!font) {
        return nullptr;
    }
    CFStringRef text = CFStringCreateWithBytes(
        nullptr, (const UInt8*)s.s, s.len, kCFStringEncodingUTF8, false);
    if (!text) {
        return nullptr;
    }
    CFIndex u16Len = CFStringGetLength(text);
    if (u16Len <= 0) {
        CFRelease(text);
        return nullptr;
    }

    CFMutableDictionaryRef attrs =
        CFDictionaryCreateMutable(nullptr, 3, &kCFTypeDictionaryKeyCallBacks,
                                  &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCTFontAttributeName, font);

    CFDictionarySetValue(attrs, kCTForegroundColorFromContextAttributeName,
                         kCFBooleanTrue);
    if (weight & kFontUnderline) {
        int32_t style = kCTUnderlineStyleSingle;
        CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &style);
        CFDictionarySetValue(attrs, kCTUnderlineStyleAttributeName, n);
        CFRelease(n);
    }
    CFAttributedStringRef attr = CFAttributedStringCreate(nullptr, text, attrs);
    CFRelease(attrs);
    CFRelease(text);
    if (!attr) {
        return nullptr;
    }

    CTTypesetterRef ts = CTTypesetterCreateWithAttributedString(attr);
    if (!ts) {
        CFRelease(attr);
        return nullptr;
    }

    auto* lines = (MacLine*)AllocArray<MacLine>((int)u16Len + 2);
    int nLines = 0;
    float width = 0;
    CFIndex pos = 0;
    UniChar buf[1];
    while (pos < u16Len) {
        CFIndex paraEnd = pos;
        while (paraEnd < u16Len) {
            CFStringGetCharacters(CFAttributedStringGetString(attr),
                                  CFRangeMake(paraEnd, 1), buf);
            if (buf[0] == '\n') {
                break;
            }
            paraEnd++;
        }
        CFIndex lineStart = pos;
        do {
            CFIndex count = paraEnd - lineStart;
            if (wrap && maxW > 0 && count > 0) {
                CFIndex fits =
                    CTTypesetterSuggestLineBreak(ts, lineStart, maxW);
                if (fits > 0 && fits < count) {
                    count = fits;
                }
            }
            CTLineRef line =
                CTTypesetterCreateLine(ts, CFRangeMake(lineStart, count));
            if (line) {
                CGFloat asc = 0, desc = 0, lead = 0;
                double w = CTLineGetTypographicBounds(line, &asc, &desc, &lead);
                lines[nLines].line = line;
                lines[nLines].start = (int)lineStart;
                lines[nLines].len = (int)count;
                lines[nLines].width = (float)w;
                if ((float)w > width) {
                    width = (float)w;
                }
                nLines++;
            }
            lineStart += count;

        } while (lineStart < paraEnd);
        pos = paraEnd + 1;
    }
    CFRelease(ts);

    if (nLines == 0) {
        Free(nullptr, lines);
        CFRelease(attr);
        return nullptr;
    }

    auto* tl = new TextLayout();
    tl->attr = attr;
    tl->lines = lines;
    tl->nLines = nLines;
    tl->width = width;
    tl->box = fontSize * (lineH > 0 ? lineH : kLineHeight);

    CGFloat ascent = CTFontGetAscent(font);
    CGFloat descent = CTFontGetDescent(font);
    tl->baseline = (float)ascent + (tl->box - (float)(ascent + descent)) * 0.5f;

    if (outSize) {
        outSize->w = width;
        outSize->h = tl->box * (float)nLines;
    }
    return tl;
}

void TextLayoutAddRef(TextLayout* tl) {
    if (tl) {
        tl->refs++;
    }
}

void TextLayoutRelease(TextLayout* tl) {
    if (!tl) {
        return;
    }
    if (--tl->refs > 0) {
        return;
    }
    for (int i = 0; i < tl->nLines; i++) {
        if (tl->lines[i].line) {
            CFRelease(tl->lines[i].line);
        }
    }
    Free(nullptr, tl->lines);
    if (tl->attr) {
        CFRelease(tl->attr);
    }
    delete tl;
}

void TextLayoutDraw(PaintCtx* ctx, TextLayout* tl, float x, float y, Rgba c,
                    bool clip) {
    CGContextRef cg = Cg(ctx);
    if (!cg || !tl) {
        return;
    }
    if (clip) {
        CGContextSaveGState(cg);
        CGContextClipToRect(
            cg, CGRectMake(x, y, tl->width, tl->box * (float)tl->nLines));
    }
    SetFill(cg, c);
    CGContextSetTextMatrix(cg, CGAffineTransformMakeScale(1, -1));
    for (int i = 0; i < tl->nLines; i++) {
        CGContextSetTextPosition(cg, x, y + (float)i * tl->box + tl->baseline);
        CTLineDraw(tl->lines[i].line, cg);
    }
    if (clip) {
        CGContextRestoreGState(cg);
    }
}

int TextLayoutHitPoint(TextLayout* tl, Str s, float relX, float relY) {
    if (!tl || tl->nLines == 0) {
        return 0;
    }
    int row = tl->box > 0 ? (int)(relY / tl->box) : 0;
    if (row < 0) {
        row = 0;
    }
    if (row >= tl->nLines) {
        row = tl->nLines - 1;
    }
    const MacLine& ml = tl->lines[row];
    CFIndex idx =
        CTLineGetStringIndexForPosition(ml.line, CGPointMake(relX, 0));
    if (idx == kCFNotFound) {
        idx = ml.start + ml.len;
    }
    return U16OffToUtf8(s, (int)idx);
}

int TextLayoutRangeRects(TextLayout* tl, Str s, int u8a, int u8b, Bounds* out,
                         int max) {
    if (!tl || !out || max <= 0 || u8a >= u8b) {
        return 0;
    }
    int a = Utf8OffToU16(s, u8a);
    int b = Utf8OffToU16(s, u8b);
    int n = 0;
    for (int i = 0; i < tl->nLines && n < max; i++) {
        const MacLine& ml = tl->lines[i];
        int lo = a > ml.start ? a : ml.start;
        int hi = b < ml.start + ml.len ? b : ml.start + ml.len;
        if (lo >= hi) {
            continue;
        }
        CGFloat x0 = CTLineGetOffsetForStringIndex(ml.line, lo, nullptr);
        CGFloat x1 = CTLineGetOffsetForStringIndex(ml.line, hi, nullptr);
        if (x1 < x0) {
            CGFloat t = x0;
            x0 = x1;
            x1 = t;
        }
        out[n].x = (float)x0;
        out[n].y = (float)i * tl->box;
        out[n].w = (float)(x1 - x0);
        out[n].h = tl->box;
        n++;
    }
    return n;
}

}

#endif

#if GPUI_OS_WINDOWS
#line 1 "src/gpui/Paint_win.cpp"

namespace gpui {

static inline D2D1_COLOR_F ToD2D(Rgba c) {
    return D2D1::ColorF(c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f);
}

template <typename T>
static void Rel(T** p) {
    if (p && *p) {
        (*p)->Release();
        *p = nullptr;
    }
}

struct PaintApp {
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dwrite = nullptr;
    IDWriteTextFormat* font12 = nullptr;
    IDWriteTextFormat* font14 = nullptr;
    IDWriteTextFormat* font16 = nullptr;
    IDWriteTextFormat* font20 = nullptr;
    IDWriteTextFormat* font24 = nullptr;
    IDWriteTextFormat* fontMono = nullptr;
};

struct PaintTarget {
    ID2D1DCRenderTarget* dcRt = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
};

static void MakeFontFamily(PaintApp* pa, const wchar_t* family, float px,
                           int weight, IDWriteTextFormat** out) {
    DWRITE_FONT_WEIGHT w =
        weight ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    pa->dwrite->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, px, L"en-us", out);
    if (*out) {
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
}

PaintApp* PaintAppNew() {
    auto* pa = new PaintApp();
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pa->d2d);
    if (FAILED(hr)) {
        delete pa;
        return nullptr;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             __uuidof(IDWriteFactory), (IUnknown**)&pa->dwrite);
    if (FAILED(hr)) {
        Rel(&pa->d2d);
        delete pa;
        return nullptr;
    }
    MakeFontFamily(pa, L"Segoe UI", 12.f, 0, &pa->font12);
    MakeFontFamily(pa, L"Segoe UI", 14.f, 0, &pa->font14);
    MakeFontFamily(pa, L"Segoe UI", 16.f, 0, &pa->font16);
    MakeFontFamily(pa, L"Segoe UI", 20.f, 1, &pa->font20);
    MakeFontFamily(pa, L"Segoe UI", 24.f, 1, &pa->font24);

    MakeFontFamily(pa, L"Consolas", 12.f, 0, &pa->fontMono);
    return pa;
}

void PaintAppFree(PaintApp* pa) {
    if (!pa) {
        return;
    }
    Rel(&pa->font12);
    Rel(&pa->font14);
    Rel(&pa->font16);
    Rel(&pa->font20);
    Rel(&pa->font24);
    Rel(&pa->fontMono);
    Rel(&pa->dwrite);
    Rel(&pa->d2d);
    delete pa;
}

void PaintTargetFree(PaintCtx* ctx) {
    if (!ctx || !ctx->rt) {
        return;
    }
    Rel(&ctx->rt->brush);
    Rel(&ctx->rt->dcRt);
    delete ctx->rt;
    ctx->rt = nullptr;
}

bool PaintTargetBegin(PaintCtx* ctx, void* native, int pxW, int pxH) {
    if (!ctx || !ctx->pa) {
        return false;
    }
    if (!ctx->rt) {
        auto* t = new PaintTarget();
        D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_IGNORE),
            96.f, 96.f);
        HRESULT hr = ctx->pa->d2d->CreateDCRenderTarget(&rtp, &t->dcRt);
        if (FAILED(hr)) {
            logf("CreateDCRenderTarget failed %08x", (unsigned)hr);
            delete t;
            return false;
        }
        t->rt = t->dcRt;
        hr = t->rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &t->brush);
        if (FAILED(hr)) {
            Rel(&t->dcRt);
            delete t;
            return false;
        }
        ctx->rt = t;
    }
    RECT rc = {0, 0, pxW, pxH};
    HRESULT hr = ctx->rt->dcRt->BindDC((HDC)native, &rc);
    if (FAILED(hr)) {
        logf("BindDC failed %08x", (unsigned)hr);
        PaintTargetFree(ctx);
        return false;
    }
    ctx->rt->rt->BeginDraw();
    ctx->rt->rt->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

bool PaintTargetEnd(PaintCtx* ctx) {
    if (!ctx || !ctx->rt) {
        return false;
    }
    HRESULT hr = ctx->rt->rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        PaintTargetFree(ctx);
        return false;
    }
    return true;
}

static ID2D1SolidColorBrush* Brush(PaintCtx* ctx, Rgba c) {
    if (!ctx || !ctx->rt || !ctx->rt->brush) {
        return nullptr;
    }
    ctx->rt->brush->SetColor(ToD2D(c));
    return ctx->rt->brush;
}

static ID2D1StrokeStyle* DashStyle(PaintCtx* ctx, const float* dash,
                                   bool roundCaps) {
    if (!ctx || !ctx->pa) {
        return nullptr;
    }
    if (!dash && !roundCaps) {
        return nullptr;
    }
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
    if (roundCaps) {
        sp.startCap = D2D1_CAP_STYLE_ROUND;
        sp.endCap = D2D1_CAP_STYLE_ROUND;
        sp.dashCap = D2D1_CAP_STYLE_ROUND;
        sp.lineJoin = D2D1_LINE_JOIN_ROUND;
    }
    ID2D1StrokeStyle* ss = nullptr;
    if (dash) {
        sp.dashStyle = D2D1_DASH_STYLE_CUSTOM;
        ctx->pa->d2d->CreateStrokeStyle(sp, dash, 2, &ss);
    } else {
        ctx->pa->d2d->CreateStrokeStyle(sp, nullptr, 0, &ss);
    }
    return ss;
}

void CanvasClear(PaintCtx* ctx, Rgba c) {
    if (ctx && ctx->rt) {
        ctx->rt->rt->Clear(ToD2D(c));
    }
}

void CanvasFillRect(PaintCtx* ctx, float x, float y, float w, float h, Rgba c) {
    if (w <= 0 || h <= 0 || c.a == 0) {
        return;
    }
    ID2D1SolidColorBrush* b = Brush(ctx, c);
    if (b) {
        ctx->rt->rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), b);
    }
}

void CanvasFillRound(PaintCtx* ctx, float x, float y, float w, float h, float r,
                     Rgba c) {
    if (w <= 0 || h <= 0 || c.a == 0) {
        return;
    }
    ID2D1SolidColorBrush* b = Brush(ctx, c);
    if (!b) {
        return;
    }
    D2D1_ROUNDED_RECT rr;
    rr.rect = D2D1::RectF(x, y, x + w, y + h);
    rr.radiusX = r;
    rr.radiusY = r;
    ctx->rt->rt->FillRoundedRectangle(rr, b);
}

void CanvasStrokeRound(PaintCtx* ctx, float x, float y, float w, float h,
                       float r, float stroke, Rgba c, const float* dash) {
    if (stroke <= 0 || w <= 0 || h <= 0) {
        return;
    }
    ID2D1SolidColorBrush* b = Brush(ctx, c);
    if (!b) {
        return;
    }

    D2D1_ROUNDED_RECT rr;
    rr.rect = D2D1::RectF(x + stroke * 0.5f, y + stroke * 0.5f,
                          x + w - stroke * 0.5f, y + h - stroke * 0.5f);
    rr.radiusX = r;
    rr.radiusY = r;
    ID2D1StrokeStyle* ss = DashStyle(ctx, dash, false);
    ctx->rt->rt->DrawRoundedRectangle(rr, b, stroke, ss);
    Rel(&ss);
}

void CanvasLine(PaintCtx* ctx, float x1, float y1, float x2, float y2,
                float stroke, Rgba c, const float* dash) {
    ID2D1SolidColorBrush* b = Brush(ctx, c);
    if (!b) {
        return;
    }
    ID2D1StrokeStyle* ss = DashStyle(ctx, dash, false);
    ctx->rt->rt
        ->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, stroke, ss);
    Rel(&ss);
}

void CanvasEllipse(PaintCtx* ctx, float cx, float cy, float rx, float ry,
                   float stroke, Rgba c) {
    ID2D1SolidColorBrush* b = Brush(ctx, c);
    if (!b) {
        return;
    }
    D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry);
    if (stroke > 0) {
        ctx->rt->rt->DrawEllipse(e, b, stroke);
    } else {
        ctx->rt->rt->FillEllipse(e, b);
    }
}

void CanvasPushClip(PaintCtx* ctx, float x, float y, float w, float h) {
    if (ctx && ctx->rt) {
        ctx->rt->rt->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h),
                                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
}

void CanvasPopClip(PaintCtx* ctx) {
    if (ctx && ctx->rt) {
        ctx->rt->rt->PopAxisAlignedClip();
    }
}

struct Path {
    ID2D1PathGeometry* geom = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    bool fig = false;
    bool sealed = false;
    float mx = 0, my = 0;
};

Path* PathNew(PaintCtx* ctx, bool winding) {
    if (!ctx || !ctx->pa) {
        return nullptr;
    }
    auto* p = new Path();
    if (FAILED(ctx->pa->d2d->CreatePathGeometry(&p->geom)) || !p->geom) {
        delete p;
        return nullptr;
    }
    if (FAILED(p->geom->Open(&p->sink)) || !p->sink) {
        Rel(&p->geom);
        delete p;
        return nullptr;
    }
    p->sink->SetFillMode(winding ? D2D1_FILL_MODE_WINDING
                                 : D2D1_FILL_MODE_ALTERNATE);
    return p;
}

void PathFree(Path* p) {
    if (!p) {
        return;
    }
    if (p->sink) {
        if (p->fig) {
            p->sink->EndFigure(D2D1_FIGURE_END_OPEN);
        }
        if (!p->sealed) {
            p->sink->Close();
        }
        p->sink->Release();
    }
    Rel(&p->geom);
    delete p;
}

void PathMoveTo(Path* p, float x, float y) {
    if (!p || !p->sink) {
        return;
    }
    if (p->fig) {
        p->sink->EndFigure(D2D1_FIGURE_END_OPEN);
    }

    p->sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
    p->fig = true;
    p->mx = x;
    p->my = y;
}

void PathLineTo(Path* p, float x, float y) {
    if (!p || !p->sink) {
        return;
    }
    if (!p->fig) {
        PathMoveTo(p, x, y);
        return;
    }
    p->sink->AddLine(D2D1::Point2F(x, y));
}

void PathCubicTo(Path* p, float x1, float y1, float x2, float y2, float x,
                 float y) {
    if (!p || !p->sink) {
        return;
    }
    if (!p->fig) {
        PathMoveTo(p, x, y);
        return;
    }
    D2D1_BEZIER_SEGMENT b;
    b.point1 = D2D1::Point2F(x1, y1);
    b.point2 = D2D1::Point2F(x2, y2);
    b.point3 = D2D1::Point2F(x, y);
    p->sink->AddBezier(b);
}

void PathArcTo(Path* p, float cx, float cy, float r, float a0, float a1,
               bool clockwise) {
    if (!p || !p->sink) {
        return;
    }
    float sx = cx + r * cosf(a0);
    float sy = cy + r * sinf(a0);
    float ex = cx + r * cosf(a1);
    float ey = cy + r * sinf(a1);
    if (!p->fig) {
        PathMoveTo(p, sx, sy);
    } else {
        p->sink->AddLine(D2D1::Point2F(sx, sy));
    }
    float sweep = a1 - a0;
    if (sweep < 0) {
        sweep = -sweep;
    }
    D2D1_ARC_SEGMENT arc = {};
    arc.point = D2D1::Point2F(ex, ey);
    arc.size = D2D1::SizeF(r, r);
    arc.rotationAngle = 0;
    arc.sweepDirection = clockwise ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                                   : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
    arc.arcSize =
        sweep > 3.14159265f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
    p->sink->AddArc(arc);
}

void PathClose(Path* p) {
    if (!p || !p->sink || !p->fig) {
        return;
    }
    p->sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    p->fig = false;
}

static ID2D1PathGeometry* PathSeal(Path* p) {
    if (!p || !p->geom) {
        return nullptr;
    }
    if (!p->sealed) {
        if (p->fig) {
            p->sink->EndFigure(D2D1_FIGURE_END_OPEN);
            p->fig = false;
        }
        p->sink->Close();
        p->sealed = true;
    }
    return p->geom;
}

void PathFill(PaintCtx* ctx, Path* p, Rgba c) {
    ID2D1PathGeometry* g = PathSeal(p);
    ID2D1SolidColorBrush* b = Brush(ctx, c);
    if (g && b) {
        ctx->rt->rt->FillGeometry(g, b, nullptr);
    }
}

void PathFillGradientV(PaintCtx* ctx, Path* p, float y0, float y1, Rgba top,
                       Rgba bot) {
    ID2D1PathGeometry* g = PathSeal(p);
    if (!g || !ctx || !ctx->rt) {
        return;
    }
    D2D1_GRADIENT_STOP gs[2];
    gs[0].position = 0.f;
    gs[0].color = ToD2D(top);
    gs[1].position = 1.f;
    gs[1].color = ToD2D(bot);
    ID2D1GradientStopCollection* stops = nullptr;
    ctx->rt->rt->CreateGradientStopCollection(gs, 2, &stops);
    bool filled = false;
    if (stops) {
        ID2D1LinearGradientBrush* gb = nullptr;
        ctx->rt->rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, y0),
                                                D2D1::Point2F(0, y1)),
            stops, &gb);
        if (gb) {
            ctx->rt->rt->FillGeometry(g, gb);
            gb->Release();
            filled = true;
        }
        stops->Release();
    }
    if (!filled) {
        PathFill(ctx, p, top);
    }
}

void PathStroke(PaintCtx* ctx, Path* p, float stroke, Rgba c, bool roundCaps) {
    ID2D1PathGeometry* g = PathSeal(p);
    ID2D1SolidColorBrush* b = Brush(ctx, c);
    if (!g || !b) {
        return;
    }
    ID2D1StrokeStyle* ss = DashStyle(ctx, nullptr, roundCaps);
    ctx->rt->rt->DrawGeometry(g, b, stroke, ss);
    Rel(&ss);
}

static IDWriteTextFormat* FontFor(PaintApp* pa, float fontSize,
                                  uint8_t weight) {
    if ((weight & kFontMono) && pa->fontMono) {
        return pa->fontMono;
    }
    if (fontSize >= 22.f && pa->font24) {
        return pa->font24;
    }
    if (fontSize >= 18.f && pa->font20) {
        return pa->font20;
    }
    if (fontSize <= 13.f) {
        return pa->font12;
    }
    if (fontSize <= 15.f) {
        return pa->font14;
    }
    return pa->font16;
}

static DWRITE_FONT_WEIGHT DwriteWeight(uint8_t weight) {
    switch (weight & kFontWeightMask) {
        case kFontWeightBold:
            return DWRITE_FONT_WEIGHT_BOLD;
        case kFontWeightSemibold:
            return DWRITE_FONT_WEIGHT_SEMI_BOLD;
        case kFontWeightMedium:
            return DWRITE_FONT_WEIGHT_MEDIUM;
        default:
            return DWRITE_FONT_WEIGHT_NORMAL;
    }
}

static int Utf8ToWideN(Str s, WCHAR* wbuf, int cap) {
    if (!s.s || s.len <= 0 || cap < 2) {
        if (wbuf && cap > 0) {
            wbuf[0] = 0;
        }
        return 0;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.s, s.len, wbuf, cap - 1);
    if (n < 0) {
        n = 0;
    }
    wbuf[n] = 0;
    return n;
}

static int Utf8OffToWide(Str s, int u8off) {
    if (u8off <= 0 || !s.s) {
        return 0;
    }
    if (u8off > s.len) {
        u8off = s.len;
    }
    return MultiByteToWideChar(CP_UTF8, 0, s.s, u8off, nullptr, 0);
}

static int WideOffToUtf8(Str s, int woff) {
    if (woff <= 0 || !s.s) {
        return 0;
    }
    WCHAR wbuf[2048];
    int wn = Utf8ToWideN(s, wbuf, 2048);
    if (woff > wn) {
        woff = wn;
    }
    return WideCharToMultiByte(CP_UTF8, 0, wbuf, woff, nullptr, 0, nullptr,
                               nullptr);
}

static void ApplyLineHeight(IDWriteTextLayout* layout, float fontSize,
                            float mult) {
    if (!layout || fontSize <= 0) {
        return;
    }
    DWRITE_LINE_METRICS lm = {};
    UINT32 n = 0;

    layout->GetLineMetrics(&lm, 1, &n);
    if (n == 0 || lm.height <= 0) {
        return;
    }
    float box = fontSize * (mult > 0 ? mult : kLineHeight);
    float baseline = lm.baseline + (box - lm.height) * 0.5f;
    layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, box, baseline);
}

static IDWriteTextLayout* Dw(TextLayout* tl) {
    return (IDWriteTextLayout*)tl;
}

TextLayout* TextLayoutNew(PaintCtx* ctx, Str s, float fontSize, float maxW,
                          bool wrap, uint8_t weight, float lineH,
                          Size* outSize) {
    if (!ctx || !ctx->pa || !ctx->pa->dwrite || !s.s || s.len <= 0) {
        return nullptr;
    }
    IDWriteTextFormat* fmt = FontFor(ctx->pa, fontSize, weight);
    if (!fmt) {
        return nullptr;
    }
    WCHAR wbuf[2048];
    int n = Utf8ToWideN(s, wbuf, 2048);
    if (n <= 0) {
        return nullptr;
    }
    IDWriteTextLayout* layout = nullptr;
    float layoutW = maxW > 0 ? maxW : 10000.f;
    HRESULT hr = ctx->pa->dwrite->CreateTextLayout(wbuf, (UINT32)n, fmt,
                                                   layoutW, 4000.f, &layout);
    if (FAILED(hr) || !layout) {
        return nullptr;
    }
    DWRITE_TEXT_RANGE range = {0, (UINT32)n};
    if (fontSize > 0) {
        layout->SetFontSize(fontSize, range);
    }
    if (weight & kFontWeightMask) {
        layout->SetFontWeight(DwriteWeight(weight), range);
    }
    if (weight & kFontUnderline) {
        layout->SetUnderline(TRUE, range);
    }
    if (weight & kFontItalic) {
        layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
    }
    layout->SetWordWrapping(wrap && maxW > 0 ? DWRITE_WORD_WRAPPING_WRAP
                                             : DWRITE_WORD_WRAPPING_NO_WRAP);
    ApplyLineHeight(layout, fontSize, lineH);
    DWRITE_TEXT_METRICS m = {};
    layout->GetMetrics(&m);
    if (outSize) {
        outSize->w = m.widthIncludingTrailingWhitespace;
        outSize->h = m.height;
    }
    return (TextLayout*)layout;
}

void TextLayoutAddRef(TextLayout* tl) {
    if (tl) {
        Dw(tl)->AddRef();
    }
}

void TextLayoutRelease(TextLayout* tl) {
    if (tl) {
        Dw(tl)->Release();
    }
}

void TextLayoutDraw(PaintCtx* ctx, TextLayout* tl, float x, float y, Rgba c,
                    bool clip) {
    ID2D1SolidColorBrush* b = Brush(ctx, c);
    if (!tl || !b) {
        return;
    }
    D2D1_DRAW_TEXT_OPTIONS opt =
        clip ? D2D1_DRAW_TEXT_OPTIONS_CLIP : D2D1_DRAW_TEXT_OPTIONS_NONE;
    ctx->rt->rt->DrawTextLayout(D2D1::Point2F(x, y), Dw(tl), b, opt);
}

int TextLayoutHitPoint(TextLayout* tl, Str s, float relX, float relY) {
    if (!tl) {
        return 0;
    }
    WCHAR wbuf[2048];
    int wn = Utf8ToWideN(s, wbuf, 2048);
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS m = {};
    Dw(tl)->HitTestPoint(relX, relY, &trailing, &inside, &m);
    int wpos = (int)m.textPosition;
    if (trailing) {
        wpos += (int)m.length;
    }
    if (wpos < 0) {
        wpos = 0;
    }
    if (wpos > wn) {
        wpos = wn;
    }
    return WideOffToUtf8(s, wpos);
}

int TextLayoutRangeRects(TextLayout* tl, Str s, int u8a, int u8b, Bounds* out,
                         int max) {
    if (!tl || !out || max <= 0 || u8a >= u8b) {
        return 0;
    }
    IDWriteTextLayout* layout = Dw(tl);
    int wa = Utf8OffToWide(s, u8a);
    int wb = Utf8OffToWide(s, u8b);
    if (wa > wb) {
        int t = wa;
        wa = wb;
        wb = t;
    }
    DWRITE_TEXT_METRICS tm = {};
    layout->GetMetrics(&tm);
    UINT32 lineCount = tm.lineCount;
    if (lineCount == 0) {
        return 0;
    }
    DWRITE_LINE_METRICS lines[32] = {};
    if (lineCount > 32) {
        lineCount = 32;
    }
    UINT32 actual = 0;
    layout->GetLineMetrics(lines, lineCount, &actual);
    UINT32 pos = 0;
    int n = 0;
    for (UINT32 i = 0; i < actual && n < max; i++) {
        int lineStart = (int)pos;
        int lineEnd = lineStart + (int)lines[i].length;
        int visEnd = lineEnd - (int)lines[i].newlineLength;
        pos = (UINT32)lineEnd;
        int lo = wa > lineStart ? wa : lineStart;
        int hi = wb < visEnd ? wb : visEnd;
        if (lo >= hi) {
            continue;
        }
        FLOAT x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        DWRITE_HIT_TEST_METRICS a = {}, b = {};
        layout->HitTestTextPosition((UINT32)lo, FALSE, &x0, &y0, &a);
        layout->HitTestTextPosition((UINT32)hi, FALSE, &x1, &y1, &b);
        float left = x0;
        float right = x1;
        if (right < left) {
            float tmp = left;
            left = right;
            right = tmp;
        }

        if (hi == visEnd && lo < visEnd) {
            right = tm.layoutWidth;
            if (x1 > 0 && x1 + 1.f < tm.layoutWidth) {
                right = x1;
            }
        }
        out[n].x = left;
        out[n].y = y0;
        out[n].w = right - left;
        out[n].h = lines[i].height;
        n++;
    }
    return n;
}

}

#endif

#if GPUI_OS_LINUX
#line 1 "src/gpui/Window_linux.cpp"

namespace gpui {

using XWindow = ::Window;

struct PlatWindow {
    XWindow xwin = 0;
    cairo_surface_t* surf = nullptr;
    cairo_surface_t* back = nullptr;
    XIC xic = nullptr;
    int pxW = 0;
    int pxH = 0;
    bool dirty = true;

    double nextTick = 0;

    int edge = -1;
    CursorKind edgeUnder = CursorKind::Arrow;
};

static Display* gDpy = nullptr;
static int gScreen = 0;
static XWindow gRoot = 0;
static XIM gXim = nullptr;
static Str gClipboard = {};

static Atom aWmDeleteWindow, aWmProtocols, aNetWmName, aUtf8String;
static Atom aNetWmState, aNetWmStateMaxVert, aNetWmStateMaxHorz;
static Atom aNetWmMoveResize, aMotifWmHints, aGtkShowWindowMenu;
static Atom aClipboard, aTargets;

double TimeNow() {
    static bool started = false;
    static struct timespec start = {};
    struct timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!started) {
        start = now;
        started = true;
    }
    return (double)(now.tv_sec - start.tv_sec) +
           (double)(now.tv_nsec - start.tv_nsec) / 1e9;
}

static Window* FindWindow(App* app, XWindow xwin) {
    if (!app) {
        return nullptr;
    }
    for (int i = 0; i < app->windows.len; i++) {
        Window* w = app->windows[i];
        if (w->plat && w->plat->xwin == xwin) {
            return w;
        }
    }
    return nullptr;
}

static void EnsureSurfaces(Window* win) {
    PlatWindow* pw = win->plat;
    if (!pw || pw->pxW <= 0 || pw->pxH <= 0) {
        return;
    }
    if (!pw->surf) {
        pw->surf = cairo_xlib_surface_create(
            gDpy, pw->xwin, DefaultVisual(gDpy, gScreen), pw->pxW, pw->pxH);
    } else {
        cairo_xlib_surface_set_size(pw->surf, pw->pxW, pw->pxH);
    }
    if (pw->back) {
        if (cairo_image_surface_get_width(pw->back) != pw->pxW ||
            cairo_image_surface_get_height(pw->back) != pw->pxH) {
            cairo_surface_destroy(pw->back);
            pw->back = nullptr;
        }
    }
    if (!pw->back) {
        pw->back =
            cairo_image_surface_create(CAIRO_FORMAT_RGB24, pw->pxW, pw->pxH);
    }
}

static void Redraw(Window* win) {
    PlatWindow* pw = win->plat;
    if (!pw) {
        return;
    }
    pw->dirty = false;
    EnsureSurfaces(win);
    if (!pw->surf || !pw->back) {
        return;
    }
    win->paint.dpi = 96;
    WindowDrawFrame(win, pw->back, pw->pxW, pw->pxH, (float)pw->pxW,
                    (float)pw->pxH);

    cairo_t* cr = cairo_create(pw->surf);
    cairo_set_source_surface(cr, pw->back, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(pw->surf);
    XFlush(gDpy);
}

static bool ReadMaximized(Window* win) {
    PlatWindow* pw = win->plat;
    if (!pw) {
        return false;
    }
    Atom type = 0;
    int format = 0;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(gDpy, pw->xwin, aNetWmState, 0, 32, False, XA_ATOM,
                           &type, &format, &n, &after, &data) != Success) {
        return false;
    }
    bool vert = false;
    bool horz = false;
    if (data) {
        auto* atoms = (Atom*)data;
        for (unsigned long i = 0; i < n; i++) {
            if (atoms[i] == aNetWmStateMaxVert) {
                vert = true;
            }
            if (atoms[i] == aNetWmStateMaxHorz) {
                horz = true;
            }
        }
        XFree(data);
    }
    return vert && horz;
}

static void SendWmState(Window* win, Atom a, Atom b, int action) {
    PlatWindow* pw = win->plat;
    if (!pw) {
        return;
    }
    XEvent ev = {};
    ev.type = ClientMessage;
    ev.xclient.window = pw->xwin;
    ev.xclient.message_type = aNetWmState;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = action;
    ev.xclient.data.l[1] = (long)a;
    ev.xclient.data.l[2] = (long)b;
    ev.xclient.data.l[3] = 1;
    XSendEvent(gDpy, gRoot, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(gDpy);
}

static void SetUndecorated(XWindow xwin) {

    struct MotifHints {
        unsigned long flags;
        unsigned long functions;
        unsigned long decorations;
        long input_mode;
        unsigned long status;
    };
    MotifHints hints = {};
    hints.flags = 2;
    hints.decorations = 0;
    XChangeProperty(gDpy, xwin, aMotifWmHints, aMotifWmHints, 32,
                    PropModeReplace, (unsigned char*)&hints, 5);
}

static int KeyFor(KeySym ks) {
    switch (ks) {
        case XK_BackSpace:
            return KeyBack;
        case XK_Tab:
        case XK_ISO_Left_Tab:
            return KeyTab;
        case XK_Return:
        case XK_KP_Enter:
            return KeyReturn;
        case XK_Shift_L:
        case XK_Shift_R:
            return KeyShift;
        case XK_Control_L:
        case XK_Control_R:
            return KeyControl;
        case XK_Alt_L:
        case XK_Alt_R:
            return KeyMenu;
        case XK_Escape:
            return KeyEscape;
        case XK_space:
            return KeySpace;
        case XK_Prior:
            return KeyPageUp;
        case XK_Next:
            return KeyPageDown;
        case XK_End:
            return KeyEnd;
        case XK_Home:
            return KeyHome;
        case XK_Left:
            return KeyLeft;
        case XK_Up:
            return KeyUp;
        case XK_Right:
            return KeyRight;
        case XK_Down:
            return KeyDown;
        case XK_Delete:
            return KeyDelete;
        default:
            break;
    }

    if (ks >= XK_a && ks <= XK_z) {
        return (int)(ks - XK_a) + 'A';
    }
    if (ks >= XK_A && ks <= XK_Z) {
        return (int)(ks - XK_A) + 'A';
    }
    if (ks >= XK_0 && ks <= XK_9) {
        return (int)(ks - XK_0) + '0';
    }
    return 0;
}

static int Utf8Next(const char* s, int len, uint32_t* out) {
    if (len <= 0) {
        return 0;
    }
    auto b = (const unsigned char*)s;
    if (b[0] < 0x80) {
        *out = b[0];
        return 1;
    }
    if ((b[0] & 0xe0) == 0xc0 && len >= 2) {
        *out = ((uint32_t)(b[0] & 0x1f) << 6) | (b[1] & 0x3f);
        return 2;
    }
    if ((b[0] & 0xf0) == 0xe0 && len >= 3) {
        *out = ((uint32_t)(b[0] & 0x0f) << 12) |
               ((uint32_t)(b[1] & 0x3f) << 6) | (b[2] & 0x3f);
        return 3;
    }
    if ((b[0] & 0xf8) == 0xf0 && len >= 4) {
        *out = ((uint32_t)(b[0] & 0x07) << 18) |
               ((uint32_t)(b[1] & 0x3f) << 12) |
               ((uint32_t)(b[2] & 0x3f) << 6) | (b[3] & 0x3f);
        return 4;
    }
    *out = b[0];
    return 1;
}

static void OnKeyPress(Window* win, XKeyEvent* ke) {
    PlatWindow* pw = win->plat;
    char buf[64] = {};
    KeySym ks = 0;
    int n = 0;
    if (pw && pw->xic) {
        Status st = 0;
        n = Xutf8LookupString(pw->xic, ke, buf, (int)sizeof(buf) - 1, &ks, &st);
        if (st == XLookupNone) {
            return;
        }
        if (st != XLookupChars && st != XLookupBoth) {
            n = 0;
        }
    } else {
        n = XLookupString(ke, buf, (int)sizeof(buf) - 1, &ks, nullptr);
    }
    bool shift = (ke->state & ShiftMask) != 0;
    bool ctrl = (ke->state & ControlMask) != 0;
    bool alt = (ke->state & Mod1Mask) != 0;

    int key = KeyFor(ks);
    if (key) {
        WindowKeyDown(win, key, shift, ctrl, alt);
    }

    if (key == KeyBack) {
        WindowChar(win, 8, ctrl, alt);
        return;
    }

    if (n <= 0 || ctrl || alt || key == KeyReturn || key == KeyTab ||
        key == KeyEscape) {
        return;
    }
    int i = 0;
    while (i < n) {
        uint32_t cp = 0;
        int adv = Utf8Next(buf + i, n - i, &cp);
        if (adv <= 0) {
            break;
        }
        i += adv;
        if (cp >= 32 && cp != 127) {
            WindowChar(win, cp, ctrl, alt);
        }
    }
}

static const int kMoveResizeMove = 8;

static void StartMoveResize(Window* win, int rootX, int rootY, int dir) {
    PlatWindow* pw = win->plat;
    if (!pw) {
        return;
    }
    XUngrabPointer(gDpy, CurrentTime);
    XEvent ev = {};
    ev.type = ClientMessage;
    ev.xclient.window = pw->xwin;
    ev.xclient.message_type = aNetWmMoveResize;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = rootX;
    ev.xclient.data.l[1] = rootY;
    ev.xclient.data.l[2] = dir;
    ev.xclient.data.l[3] = Button1;
    ev.xclient.data.l[4] = 1;
    XSendEvent(gDpy, gRoot, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(gDpy);
}

static void StartMoveDrag(Window* win, int rootX, int rootY) {
    StartMoveResize(win, rootX, rootY, kMoveResizeMove);
}

static void ShowWindowMenu(Window* win, int rootX, int rootY) {
    PlatWindow* pw = win->plat;
    if (!pw) {
        return;
    }
    XUngrabPointer(gDpy, CurrentTime);
    XEvent ev = {};
    ev.type = ClientMessage;
    ev.xclient.window = pw->xwin;
    ev.xclient.message_type = aGtkShowWindowMenu;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 0;
    ev.xclient.data.l[1] = rootX;
    ev.xclient.data.l[2] = rootY;
    XSendEvent(gDpy, gRoot, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(gDpy);
}

static const int kResizeBand = 6;

static bool ClientDecorated(Window* win) {
    return win->opts.clientTitleBar || win->opts.borderless;
}

static int ResizeEdge(Window* win, int x, int y) {
    PlatWindow* pw = win->plat;
    if (!pw || !ClientDecorated(win) || win->maximized) {
        return -1;
    }
    bool l = x < kResizeBand;
    bool r = x >= pw->pxW - kResizeBand;
    bool t = y < kResizeBand;
    bool b = y >= pw->pxH - kResizeBand;
    if (t) {
        return l ? 0 : r ? 2 : 1;
    }
    if (b) {
        return l ? 6 : r ? 4 : 5;
    }
    if (l) {
        return 7;
    }
    return r ? 3 : -1;
}

static void SetEdgeCursor(Window* win, int dir) {
    PlatWindow* pw = win->plat;
    if (!pw || (pw->edge == dir && pw->edgeUnder == win->cursor)) {
        return;
    }
    pw->edge = dir;
    pw->edgeUnder = win->cursor;
    if (dir < 0) {

        PlatSetCursor(win, win->cursor);
        return;
    }
    static const unsigned kShapes[8] = {XC_top_left_corner,     XC_top_side,
                                        XC_top_right_corner,    XC_right_side,
                                        XC_bottom_right_corner, XC_bottom_side,
                                        XC_bottom_left_corner,  XC_left_side};

    static ::Cursor cache[8] = {};
    if (!cache[dir]) {
        cache[dir] = XCreateFontCursor(gDpy, kShapes[dir]);
    }
    XDefineCursor(gDpy, pw->xwin, cache[dir]);
    XFlush(gDpy);
}

void PlatSetCursor(Window* win, CursorKind kind) {
    if (!win || !win->plat || !gDpy) {
        return;
    }

    static ::Cursor arrow = 0;
    static ::Cursor ibeam = 0;
    if (!arrow) {
        arrow = XCreateFontCursor(gDpy, XC_left_ptr);
    }
    if (!ibeam) {
        ibeam = XCreateFontCursor(gDpy, XC_xterm);
    }
    XDefineCursor(gDpy, win->plat->xwin,
                  kind == CursorKind::IBeam ? ibeam : arrow);
    XFlush(gDpy);
}

int PlatDoubleClickMs() {

    return 400;
}

void ClipboardSetText(Window* win, Str text) {
    if (!win || !win->plat || !text.s || text.len <= 0) {
        return;
    }
    if (gClipboard.s) {
        StrFree(gClipboard);
    }
    gClipboard = StrDup(text);
    XSetSelectionOwner(gDpy, aClipboard, win->plat->xwin, CurrentTime);
    XFlush(gDpy);
}

static void OnSelectionRequest(XSelectionRequestEvent* req) {
    XEvent resp = {};
    resp.xselection.type = SelectionNotify;
    resp.xselection.requestor = req->requestor;
    resp.xselection.selection = req->selection;
    resp.xselection.target = req->target;
    resp.xselection.time = req->time;
    resp.xselection.property = None;

    Atom prop = req->property ? req->property : req->target;
    if (req->target == aTargets) {
        Atom targets[2] = {aTargets, aUtf8String};
        XChangeProperty(gDpy, req->requestor, prop, XA_ATOM, 32,
                        PropModeReplace, (unsigned char*)targets, 2);
        resp.xselection.property = prop;
    } else if ((req->target == aUtf8String || req->target == XA_STRING) &&
               gClipboard.s) {
        XChangeProperty(gDpy, req->requestor, prop, req->target, 8,
                        PropModeReplace, (unsigned char*)gClipboard.s,
                        gClipboard.len);
        resp.xselection.property = prop;
    }
    XSendEvent(gDpy, req->requestor, False, 0, &resp);
    XFlush(gDpy);
}

static void DestroyPlatWindow(Window* win) {
    PlatWindow* pw = win->plat;
    if (!pw) {
        return;
    }
    if (pw->back) {
        cairo_surface_destroy(pw->back);
    }
    if (pw->surf) {
        cairo_surface_destroy(pw->surf);
    }
    if (pw->xic) {
        XDestroyIC(pw->xic);
    }
    XWindow xwin = pw->xwin;
    delete pw;
    WindowClosed(win);
    if (xwin) {
        XDestroyWindow(gDpy, xwin);
    }
}

static Modifiers ModsOf(unsigned state) {
    Modifiers m;
    m.control = (state & ControlMask) != 0;
    m.alt = (state & Mod1Mask) != 0;
    m.shift = (state & ShiftMask) != 0;
    m.platform = (state & Mod4Mask) != 0;
    return m;
}

static bool ButtonOf(unsigned b, MouseButton* out) {
    switch (b) {
        case Button1:
            *out = MouseButton::Left;
            return true;
        case Button2:
            *out = MouseButton::Middle;
            return true;
        case Button3:
            *out = MouseButton::Right;
            return true;
        case 8:
            *out = MouseButton::NavigateBack;
            return true;
        case 9:
            *out = MouseButton::NavigateForward;
            return true;
        default:
            return false;
    }
}

static bool PressedButton(unsigned state, MouseButton* out) {
    if (state & Button1Mask) {
        *out = MouseButton::Left;
        return true;
    }
    if (state & Button2Mask) {
        *out = MouseButton::Middle;
        return true;
    }
    if (state & Button3Mask) {
        *out = MouseButton::Right;
        return true;
    }
    return false;
}

static void PressButton(Window* win, MouseButton button, float x, float y,
                        Modifiers mods) {
    PlatformInput in = InputMouseDown(
        button, x, y, mods, WindowClickCount(win, x, y, button), false);
    WindowDispatchInput(win, &in);
}

static void HandleEvent(App* app, XEvent* ev) {
    if (ev->type == SelectionRequest) {
        OnSelectionRequest(&ev->xselectionrequest);
        return;
    }
    if (ev->type == SelectionClear) {
        if (gClipboard.s) {
            StrFree(gClipboard);
            gClipboard = {};
        }
        return;
    }
    XWindow xwin = ev->xany.window;
    Window* win = FindWindow(app, xwin);
    if (!win) {
        return;
    }
    PlatWindow* pw = win->plat;
    switch (ev->type) {
        case Expose:
            if (ev->xexpose.count == 0) {
                pw->dirty = true;
            }
            break;
        case ConfigureNotify:
            if (ev->xconfigure.width != pw->pxW || ev->xconfigure
                                                           .height != pw->pxH) {
                pw->pxW = ev->xconfigure.width;
                pw->pxH = ev->xconfigure.height;
                pw->dirty = true;
            }
            break;
        case PropertyNotify:
            if (ev->xproperty.atom == aNetWmState) {
                win->maximized = ReadMaximized(win);
            }
            break;
        case KeyPress:
            OnKeyPress(win, &ev->xkey);
            break;
        case MotionNotify: {
            MouseButton held = MouseButton::Left;
            bool any = PressedButton(ev->xmotion.state, &held);
            PlatformInput in =
                InputMouseMove((float)ev->xmotion.x, (float)ev->xmotion.y, any,
                               held, ModsOf(ev->xmotion.state));
            WindowDispatchInput(win, &in);
            SetEdgeCursor(win, ResizeEdge(win, ev->xmotion.x, ev->xmotion.y));
            break;
        }
        case LeaveNotify: {
            MouseButton held = MouseButton::Left;
            bool any = PressedButton(ev->xcrossing.state, &held);
            PlatformInput in =
                InputMouseExited((float)ev->xcrossing.x, (float)ev->xcrossing.y,
                                 any, held, ModsOf(ev->xcrossing.state));
            WindowDispatchInput(win, &in);

            pw->edge = -1;
            break;
        }
        case ButtonPress: {
            float x = (float)ev->xbutton.x;
            float y = (float)ev->xbutton.y;
            unsigned b = ev->xbutton.button;
            Modifiers mods = ModsOf(ev->xbutton.state);

            if (b >= Button4 && b <= 7) {
                float dx = b == 6 ? 48.f : b == 7 ? -48.f : 0.f;
                float dy = b == Button4 ? 48.f : b == Button5 ? -48.f : 0.f;
                PlatformInput in = InputScrollWheel(x, y, dx, dy, false, mods,
                                                    TouchPhase::Moved);
                WindowDispatchInput(win, &in);
                break;
            }
            if (b == Button3) {

                if (WindowChromeHit(win, x, y) == ClickWinCaption) {
                    ShowWindowMenu(win, ev->xbutton.x_root, ev->xbutton.y_root);
                    break;
                }
                PressButton(win, MouseButton::Right, x, y, mods);
                break;
            }
            if (b == Button2) {
                PressButton(win, MouseButton::Middle, x, y, mods);
                break;
            }

            if (b == 8 || b == 9) {
                PressButton(win,
                            b == 8 ? MouseButton::NavigateBack
                                   : MouseButton::NavigateForward,
                            x, y, mods);
                break;
            }
            if (b != Button1) {
                break;
            }

            int clicks = WindowClickCount(win, x, y, MouseButton::Left);

            int edge = ResizeEdge(win, ev->xbutton.x, ev->xbutton.y);
            if (edge >= 0) {
                StartMoveResize(win, ev->xbutton.x_root, ev->xbutton.y_root,
                                edge);
                break;
            }
            int chrome = WindowChromeHit(win, x, y);
            if (chrome == ClickWinMin) {
                AppMinimize(win);
                break;
            }
            if (chrome == ClickWinMax) {
                AppToggleMaximize(win);
                break;
            }
            if (chrome == ClickWinClose) {
                AppClose(win);
                break;
            }
            if (chrome == ClickWinCaption) {
                if (clicks == 2) {
                    AppToggleMaximize(win);
                } else {
                    StartMoveDrag(win, ev->xbutton.x_root, ev->xbutton.y_root);
                }
                break;
            }
            PlatformInput in =
                InputMouseDown(MouseButton::Left, x, y, mods, clicks, false);
            WindowDispatchInput(win, &in);
            break;
        }
        case ButtonRelease: {
            MouseButton button = MouseButton::Left;
            if (!ButtonOf(ev->xbutton.button, &button)) {
                break;
            }
            PlatformInput in = InputMouseUp(
                button, (float)ev->xbutton.x, (float)ev->xbutton.y,
                ModsOf(ev->xbutton.state), WindowCurrentClickCount(win));
            WindowDispatchInput(win, &in);
            break;
        }
        case ClientMessage:
            if (ev->xclient.message_type == aWmProtocols &&
                (Atom)ev->xclient.data.l[0] == aWmDeleteWindow) {
                DestroyPlatWindow(win);
            }
            break;
        default:
            break;
    }
}

void AppQuit(Window* win) {
    if (win && win->plat) {
        DestroyPlatWindow(win);
    }
}

void AppInvalidate(Window* win) {
    if (win && win->plat) {
        win->plat->dirty = true;
    }
}

void AppMinimize(Window* win) {
    if (win && win->plat) {
        XIconifyWindow(gDpy, win->plat->xwin, gScreen);
        XFlush(gDpy);
    }
}

void AppToggleMaximize(Window* win) {
    if (win && win->plat) {
        SendWmState(win, aNetWmStateMaxVert, aNetWmStateMaxHorz, 2);
    }
}

void AppDrag(Window* win) {
    if (!win || !win->plat) {
        return;
    }
    XWindow child = 0;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned mask = 0;
    XWindow rootRet = 0;
    XQueryPointer(gDpy, win->plat->xwin, &rootRet, &child, &rx, &ry, &wx, &wy,
                  &mask);
    StartMoveDrag(win, rx, ry);
}

void AppSetTitle(Window* win, Str title) {
    if (!win || !win->plat || !title.s) {
        return;
    }
    XWindow xwin = win->plat->xwin;

    XChangeProperty(gDpy, xwin, aNetWmName, aUtf8String, 8, PropModeReplace,
                    (unsigned char*)title.s, title.len);
    Str z = StrDup(title);
    if (z.s) {
        XStoreName(gDpy, xwin, z.s);
        StrFree(z);
    }
    XFlush(gDpy);
}

void PlatSetTimer(Window* win, int ms) {
    if (!win || !win->plat) {
        return;
    }
    win->plat->nextTick = ms > 0 ? TimeNow() + ms / 1000.0 : 0;
}

bool PlatInit(App* app) {
    (void)app;
    if (gDpy) {
        return true;
    }
    setlocale(LC_ALL, "");
    XSetLocaleModifiers("");
    gDpy = XOpenDisplay(nullptr);
    if (!gDpy) {
        logf("XOpenDisplay failed: no DISPLAY?");
        return false;
    }
    gScreen = DefaultScreen(gDpy);
    gRoot = RootWindow(gDpy, gScreen);
    gXim = XOpenIM(gDpy, nullptr, nullptr, nullptr);

    aWmProtocols = XInternAtom(gDpy, "WM_PROTOCOLS", False);
    aWmDeleteWindow = XInternAtom(gDpy, "WM_DELETE_WINDOW", False);
    aNetWmName = XInternAtom(gDpy, "_NET_WM_NAME", False);
    aUtf8String = XInternAtom(gDpy, "UTF8_STRING", False);
    aNetWmState = XInternAtom(gDpy, "_NET_WM_STATE", False);
    aNetWmStateMaxVert =
        XInternAtom(gDpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    aNetWmStateMaxHorz =
        XInternAtom(gDpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    aNetWmMoveResize = XInternAtom(gDpy, "_NET_WM_MOVERESIZE", False);
    aMotifWmHints = XInternAtom(gDpy, "_MOTIF_WM_HINTS", False);
    aGtkShowWindowMenu = XInternAtom(gDpy, "_GTK_SHOW_WINDOW_MENU", False);
    aClipboard = XInternAtom(gDpy, "CLIPBOARD", False);
    aTargets = XInternAtom(gDpy, "TARGETS", False);
    return true;
}

void PlatShutdown(App* app) {
    (void)app;
    if (gClipboard.s) {
        StrFree(gClipboard);
        gClipboard = {};
    }
    if (gXim) {
        XCloseIM(gXim);
        gXim = nullptr;
    }
    if (gDpy) {
        XCloseDisplay(gDpy);
        gDpy = nullptr;
    }
}

Window* WindowOpen(App* app, Str title, int dipW, int dipH, WinOpts opts) {
    if (!gDpy) {
        return nullptr;
    }
    Window* win = WindowAlloc(app, opts);
    if (!win) {
        return nullptr;
    }
    int sw = DisplayWidth(gDpy, gScreen);
    int sh = DisplayHeight(gDpy, gScreen);
    WindowClampToDisplay(&dipW, &dipH, sw, sh);

    auto* pw = new PlatWindow();
    pw->pxW = dipW;
    pw->pxH = dipH;

    int x = (sw - dipW) / 2;
    int y = (sh - dipH) / 2;

    XSetWindowAttributes attrs = {};
    attrs.background_pixel = BlackPixel(gDpy, gScreen);
    attrs.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                       LeaveWindowMask | StructureNotifyMask |
                       PropertyChangeMask | FocusChangeMask;
    pw->xwin = XCreateWindow(gDpy, gRoot, x, y, (unsigned)dipW, (unsigned)dipH,
                             0, CopyFromParent, InputOutput, CopyFromParent,
                             CWBackPixel | CWEventMask, &attrs);
    if (!pw->xwin) {
        delete pw;
        return nullptr;
    }
    win->plat = pw;

    XSetWMProtocols(gDpy, pw->xwin, &aWmDeleteWindow, 1);
    if (opts.borderless || opts.clientTitleBar) {
        SetUndecorated(pw->xwin);
    }
    AppSetTitle(win, title);

    if (gXim) {
        pw->xic = XCreateIC(
            gXim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
            XNClientWindow, pw->xwin, XNFocusWindow, pw->xwin, nullptr);
    }

    XMapWindow(gDpy, pw->xwin);
    XFlush(gDpy);
    PlatSetTimer(win, WindowTimerMs(win));
    return win;
}

int AppRun(App* app) {
    if (!app || !gDpy) {
        return 1;
    }
    int fd = ConnectionNumber(gDpy);
    while (AppAnyWindowOpen(app)) {
        while (XPending(gDpy) > 0) {
            XEvent ev = {};
            XNextEvent(gDpy, &ev);
            if (XFilterEvent(&ev, None)) {
                continue;
            }
            HandleEvent(app, &ev);
        }
        if (!AppAnyWindowOpen(app)) {
            break;
        }

        for (int i = 0; i < app->windows.len; i++) {
            Window* w = app->windows[i];
            if (w->plat && w->plat->dirty) {
                Redraw(w);
            }
        }

        double now = TimeNow();
        double waitS = 1.0;
        bool anyDirty = false;
        for (int i = 0; i < app->windows.len; i++) {
            Window* w = app->windows[i];
            if (!w->plat) {
                continue;
            }
            if (w->plat->dirty) {
                anyDirty = true;
            }
            if (w->plat->nextTick > 0) {
                double d = w->plat->nextTick - now;
                if (d < waitS) {
                    waitS = d;
                }
            }
        }
        if (!anyDirty && XPending(gDpy) == 0) {
            int timeoutMs = waitS <= 0 ? 0 : (int)(waitS * 1000.0);
            struct pollfd pfd = {fd, POLLIN, 0};
            poll(&pfd, 1, timeoutMs);
        }

        now = TimeNow();
        for (int i = 0; i < app->windows.len; i++) {
            Window* w = app->windows[i];
            if (!w->plat || w->plat->nextTick <= 0) {
                continue;
            }
            if (now >= w->plat->nextTick) {

                WindowTimerTick(w);
            }
        }
    }
    return app->exitCode;
}

}

int main(int argc, char** argv) {

    argc = gpui::GpuiTakeRuntimeArgs(argc, argv);
    return GpuiMain(argc, argv);
}

#endif

#if GPUI_OS_MAC
#line 1 "src/gpui/Window_mac.cpp"

@class GpuiView;
@class GpuiWindowDelegate;

namespace gpui {

struct PlatWindow {
    NSWindow* window = nil;
    GpuiView* view = nil;

    GpuiWindowDelegate* delegate = nil;
    bool dirty = true;

    double nextTick = 0;
};

double TimeNow() {
    static bool started = false;
    static struct timespec start = {};
    struct timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!started) {
        start = now;
        started = true;
    }
    return (double)(now.tv_sec - start.tv_sec) +
           (double)(now.tv_nsec - start.tv_nsec) / 1e9;
}

void WindowMacKeyDown(Window* win, NSEvent* event);

}

namespace gpui {

static Modifiers ModsOf(NSEvent* event) {
    NSEventModifierFlags f = [event modifierFlags];
    Modifiers m;
    m.control = (f & NSEventModifierFlagControl) != 0;
    m.alt = (f & NSEventModifierFlagOption) != 0;
    m.shift = (f & NSEventModifierFlagShift) != 0;
    m.platform = (f & NSEventModifierFlagCommand) != 0;
    m.function = (f & NSEventModifierFlagFunction) != 0;
    return m;
}

static MouseButton MouseButtonOf(NSInteger number) {
    switch (number) {
        case 0:
            return MouseButton::Left;
        case 1:
            return MouseButton::Right;
        case 2:
            return MouseButton::Middle;
        case 3:
            return MouseButton::NavigateBack;
        default:
            return MouseButton::NavigateForward;
    }
}

static bool PressedButton(MouseButton* out) {
    NSUInteger mask = [NSEvent pressedMouseButtons];
    for (NSInteger i = 0; i < 5; i++) {
        if (mask & (1u << i)) {
            *out = MouseButtonOf(i);
            return true;
        }
    }
    return false;
}

}

@interface GpuiView : NSView {
  @public
    gpui::Window* win;
}
@end

@implementation GpuiView

- (BOOL)isFlipped {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}
- (BOOL)mouseDownCanMoveWindow {

    return NO;
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    if (!win) {
        return;
    }
    CGContextRef cg =
        (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
    NSRect b = [self bounds];
    NSRect px = [self convertRectToBacking:b];
    win->paint.dpi = 96;
    gpui::WindowDrawFrame(win, cg, (int)px.size.width, (int)px.size.height,
                          (float)b.size.width, (float)b.size.height);
}

- (void)updateTrackingAreas {
    for (NSTrackingArea* a in [self trackingAreas]) {
        [self removeTrackingArea:a];
    }
    NSTrackingAreaOptions opts =
        NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
        NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect;
    NSTrackingArea* area = [[NSTrackingArea alloc] initWithRect:[self bounds]
                                                        options:opts
                                                          owner:self
                                                       userInfo:nil];
    [self addTrackingArea:area];
    [super updateTrackingAreas];
}

- (NSPoint)gpuiPoint:(NSEvent*)event {
    return [self convertPoint:[event locationInWindow] fromView:nil];
}

- (void)mouseMoved:(NSEvent*)event {
    NSPoint p = [self gpuiPoint:event];
    gpui::MouseButton held = gpui::MouseButton::Left;
    bool any = gpui::PressedButton(&held);
    gpui::PlatformInput in = gpui::InputMouseMove((float)p.x, (float)p.y, any,
                                                  held, gpui::ModsOf(event));
    gpui::WindowDispatchInput(win, &in);
}
- (void)mouseDragged:(NSEvent*)event {
    [self mouseMoved:event];
}
- (void)rightMouseDragged:(NSEvent*)event {
    [self mouseMoved:event];
}
- (void)otherMouseDragged:(NSEvent*)event {
    [self mouseMoved:event];
}
- (void)mouseExited:(NSEvent*)event {
    NSPoint p = [self gpuiPoint:event];
    gpui::MouseButton held = gpui::MouseButton::Left;
    bool any = gpui::PressedButton(&held);
    gpui::PlatformInput in = gpui::InputMouseExited((float)p.x, (float)p.y, any,
                                                    held, gpui::ModsOf(event));
    gpui::WindowDispatchInput(win, &in);
}

- (void)mouseDown:(NSEvent*)event {
    NSPoint p = [self gpuiPoint:event];
    float x = (float)p.x;
    float y = (float)p.y;

    int clicks = gpui::WindowClickCount(win, x, y, gpui::MouseButton::Left);

    int chrome = gpui::WindowChromeHit(win, x, y);
    if (chrome == gpui::ClickWinMin) {
        gpui::AppMinimize(win);
        return;
    }
    if (chrome == gpui::ClickWinMax) {
        gpui::AppToggleMaximize(win);
        return;
    }
    if (chrome == gpui::ClickWinClose) {
        gpui::AppClose(win);
        return;
    }
    if (chrome == gpui::ClickWinCaption) {
        if (clicks == 2) {
            gpui::AppToggleMaximize(win);
            return;
        }
        [[self window] performWindowDragWithEvent:event];
        return;
    }

    gpui::PlatformInput in = gpui::InputMouseDown(
        gpui::MouseButton::Left, x, y, gpui::ModsOf(event), clicks, false);
    gpui::WindowDispatchInput(win, &in);
}

- (void)press:(NSEvent*)event button:(gpui::MouseButton)button {
    NSPoint p = [self gpuiPoint:event];
    float x = (float)p.x;
    float y = (float)p.y;
    gpui::PlatformInput in =
        gpui::InputMouseDown(button, x, y, gpui::ModsOf(event),
                             gpui::WindowClickCount(win, x, y, button), false);
    gpui::WindowDispatchInput(win, &in);
}

- (void)release:(NSEvent*)event button:(gpui::MouseButton)button {
    NSPoint p = [self gpuiPoint:event];
    gpui::PlatformInput in =
        gpui::InputMouseUp(button, (float)p.x, (float)p.y, gpui::ModsOf(event),
                           gpui::WindowCurrentClickCount(win));
    gpui::WindowDispatchInput(win, &in);
}

- (void)mouseUp:(NSEvent*)event {
    [self release:event button:gpui::MouseButton::Left];
}

- (void)rightMouseDown:(NSEvent*)event {
    [self press:event button:gpui::MouseButton::Right];
}

- (void)rightMouseUp:(NSEvent*)event {
    [self release:event button:gpui::MouseButton::Right];
}

- (void)otherMouseDown:(NSEvent*)event {
    [self press:event button:gpui::MouseButtonOf([event buttonNumber])];
}

- (void)otherMouseUp:(NSEvent*)event {
    [self release:event button:gpui::MouseButtonOf([event buttonNumber])];
}

- (void)scrollWheel:(NSEvent*)event {
    NSPoint p = [self gpuiPoint:event];

    bool precise = [event hasPreciseScrollingDeltas];
    float scale = precise ? 1.f : 48.f;
    float dx = (float)[event scrollingDeltaX] * scale;
    float dy = (float)[event scrollingDeltaY] * scale;
    gpui::TouchPhase phase = gpui::TouchPhase::Moved;
    NSEventPhase ph = [event phase];
    if (ph & NSEventPhaseBegan) {
        phase = gpui::TouchPhase::Started;
    } else if (ph & NSEventPhaseEnded) {
        phase = gpui::TouchPhase::Ended;
    } else if (ph & NSEventPhaseCancelled) {
        phase = gpui::TouchPhase::Cancelled;
    }
    gpui::PlatformInput in = gpui::InputScrollWheel(
        (float)p.x, (float)p.y, dx, dy, precise, gpui::ModsOf(event), phase);
    gpui::WindowDispatchInput(win, &in);
}

- (void)keyDown:(NSEvent*)event {
    gpui::WindowMacKeyDown(win, event);
}

- (void)keyUp:(NSEvent*)event {
    (void)event;
}

- (BOOL)performKeyEquivalent:(NSEvent*)event {
    (void)event;
    return NO;
}

@end

@interface GpuiWindowDelegate : NSObject <NSWindowDelegate> {
  @public
    gpui::Window* win;
}
@end

@implementation GpuiWindowDelegate

- (void)windowWillClose:(NSNotification*)note {
    (void)note;
    gpui::WindowClosed(win);
}

- (void)windowDidResize:(NSNotification*)note {
    (void)note;
    gpui::AppInvalidate(win);
}

- (void)windowDidChangeBackingProperties:(NSNotification*)note {
    (void)note;
    gpui::AppInvalidate(win);
}

@end

namespace gpui {

static int KeyFor(unichar c) {
    switch (c) {
        case NSUpArrowFunctionKey:
            return KeyUp;
        case NSDownArrowFunctionKey:
            return KeyDown;
        case NSLeftArrowFunctionKey:
            return KeyLeft;
        case NSRightArrowFunctionKey:
            return KeyRight;
        case NSHomeFunctionKey:
            return KeyHome;
        case NSEndFunctionKey:
            return KeyEnd;
        case NSPageUpFunctionKey:
            return KeyPageUp;
        case NSPageDownFunctionKey:
            return KeyPageDown;
        case NSDeleteFunctionKey:
            return KeyDelete;
        case 0x7f:
            return KeyBack;
        case '\r':
        case 0x03:
            return KeyReturn;
        case '\t':
        case 0x19:
            return KeyTab;
        case 0x1b:
            return KeyEscape;
        case ' ':
            return KeySpace;
        default:
            break;
    }
    if (c >= 'a' && c <= 'z') {
        return (int)(c - 'a') + 'A';
    }
    if (c >= 'A' && c <= 'Z') {
        return (int)c;
    }
    if (c >= '0' && c <= '9') {
        return (int)c;
    }
    return 0;
}

void WindowMacKeyDown(Window* win, NSEvent* event) {
    if (!win) {
        return;
    }
    NSEventModifierFlags mods = [event modifierFlags];
    bool shift = (mods & NSEventModifierFlagShift) != 0;

    bool ctrl =
        (mods & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0;
    bool alt = (mods & NSEventModifierFlagOption) != 0;

    NSString* bare = [event charactersIgnoringModifiers];
    unichar first = [bare length] > 0 ? [bare characterAtIndex:0] : 0;
    int key = KeyFor(first);
    if (key) {
        WindowKeyDown(win, key, shift, ctrl, alt);
    }

    if (key == KeyBack) {
        WindowChar(win, 8, ctrl, alt);
        return;
    }
    if (ctrl || alt || key == KeyReturn || key == KeyTab || key == KeyEscape) {
        return;
    }
    NSString* text = [event characters];
    NSUInteger n = [text length];
    for (NSUInteger i = 0; i < n; i++) {
        unichar u = [text characterAtIndex:i];
        uint32_t cp = u;

        if (u >= 0xd800 && u <= 0xdbff && i + 1 < n) {
            unichar lo = [text characterAtIndex:i + 1];
            if (lo >= 0xdc00 && lo <= 0xdfff) {
                cp = 0x10000 + ((uint32_t)(u - 0xd800) << 10) + (lo - 0xdc00);
                i++;
            }
        }
        if (cp >= 32 && cp != 0x7f) {
            WindowChar(win, cp, ctrl, alt);
        }
    }
}

static void Redraw(Window* win) {
    PlatWindow* pw = win->plat;
    if (!pw || !pw->view) {
        return;
    }
    pw->dirty = false;
    win->maximized = [pw->window isZoomed] ? true : false;
    [pw->view display];
}

void AppQuit(Window* win) {
    if (win && win->plat && win->plat->window) {

        [win->plat->window close];
    }
}

void AppInvalidate(Window* win) {
    if (win && win->plat) {
        win->plat->dirty = true;
    }
}

void AppMinimize(Window* win) {
    if (win && win->plat) {
        [win->plat->window miniaturize:nil];
    }
}

void AppToggleMaximize(Window* win) {
    if (win && win->plat) {
        [win->plat->window zoom:nil];
        win->maximized = [win->plat->window isZoomed] ? true : false;
    }
}

void AppDrag(Window* win) {
    if (!win || !win->plat) {
        return;
    }
    NSEvent* ev = [NSApp currentEvent];
    if (ev) {
        [win->plat->window performWindowDragWithEvent:ev];
    }
}

void AppSetTitle(Window* win, Str title) {
    if (!win || !win->plat || !title.s) {
        return;
    }
    NSString* s = [[NSString alloc] initWithBytes:title.s
                                           length:(NSUInteger)title.len
                                         encoding:NSUTF8StringEncoding];
    if (s) {
        [win->plat->window setTitle:s];
    }
}

void PlatSetTimer(Window* win, int ms) {
    if (!win || !win->plat) {
        return;
    }
    win->plat->nextTick = ms > 0 ? TimeNow() + ms / 1000.0 : 0;
}

void PlatSetCursor(Window* win, CursorKind kind) {
    (void)win;
    if (kind == CursorKind::IBeam) {
        [[NSCursor IBeamCursor] set];
    } else {
        [[NSCursor arrowCursor] set];
    }
}

int PlatDoubleClickMs() {

    return (int)([NSEvent doubleClickInterval] * 1000);
}

void ClipboardSetText(Window* win, Str text) {
    (void)win;
    if (!text.s || text.len <= 0) {
        return;
    }
    NSString* s = [[NSString alloc] initWithBytes:text.s
                                           length:(NSUInteger)text.len
                                         encoding:NSUTF8StringEncoding];
    if (!s) {
        return;
    }
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:s forType:NSPasteboardTypeString];
}

bool PlatInit(App* app) {
    (void)app;
    @autoreleasepool {
        [NSApplication sharedApplication];

        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
    }
    return true;
}

void PlatShutdown(App* app) {
    (void)app;
}

Window* WindowOpen(App* app, Str title, int dipW, int dipH, WinOpts opts) {
    Window* win = WindowAlloc(app, opts);
    if (!win) {
        return nullptr;
    }
    @autoreleasepool {
        NSWindowStyleMask style =
            NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        if (opts.clientTitleBar) {
            style |= NSWindowStyleMaskFullSizeContentView;
        }
        if (opts.borderless) {
            style = NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable |
                    NSWindowStyleMaskMiniaturizable;
        }

        NSRect screen = [[NSScreen mainScreen] frame];
        WindowClampToDisplay(&dipW, &dipH, (int)screen.size.width,
                             (int)screen.size.height);
        NSRect frame = NSMakeRect(0, 0, dipW, dipH);
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:frame
                                        styleMask:style
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window) {
            return nullptr;
        }
        if (opts.clientTitleBar) {
            [window setTitleVisibility:NSWindowTitleHidden];
            [window setTitlebarAppearsTransparent:YES];
            [window setTitlebarSeparatorStyle:NSTitlebarSeparatorStyleNone];
            [window setMovableByWindowBackground:NO];
        }
        GpuiView* view = [[GpuiView alloc] initWithFrame:frame];
        view->win = win;
        GpuiWindowDelegate* del = [[GpuiWindowDelegate alloc] init];
        del->win = win;

        auto* pw = new PlatWindow();
        pw->window = window;
        pw->view = view;
        pw->delegate = del;
        win->plat = pw;

        [window setContentView:view];
        [window makeFirstResponder:view];
        [window setAcceptsMouseMovedEvents:YES];
        [window setReleasedWhenClosed:NO];
        [window setDelegate:del];
        [window center];

        AppSetTitle(win, title);
        [window makeKeyAndOrderFront:nil];

        [NSApp activateIgnoringOtherApps:YES];
        PlatSetTimer(win, WindowTimerMs(win));
    }
    return win;
}

int AppRun(App* app) {
    if (!app) {
        return 1;
    }
    while (AppAnyWindowOpen(app)) {
        @autoreleasepool {

            for (;;) {
                NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                 untilDate:nil
                                                    inMode:NSDefaultRunLoopMode
                                                   dequeue:YES];
                if (!ev) {
                    break;
                }
                [NSApp sendEvent:ev];
            }
            if (!AppAnyWindowOpen(app)) {
                break;
            }

            for (int i = 0; i < app->windows.len; i++) {
                Window* w = app->windows[i];
                if (w->plat && w->plat->dirty) {
                    Redraw(w);
                }
            }

            double now = TimeNow();
            double waitS = 1.0;
            bool anyDirty = false;
            for (int i = 0; i < app->windows.len; i++) {
                Window* w = app->windows[i];
                if (!w->plat) {
                    continue;
                }
                if (w->plat->dirty) {
                    anyDirty = true;
                }
                if (w->plat->nextTick > 0) {
                    double d = w->plat->nextTick - now;
                    if (d < waitS) {
                        waitS = d;
                    }
                }
            }
            if (!anyDirty) {
                NSDate* deadline =
                    waitS <= 0 ? [NSDate distantPast]
                               : [NSDate dateWithTimeIntervalSinceNow:waitS];
                NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                 untilDate:deadline
                                                    inMode:NSDefaultRunLoopMode
                                                   dequeue:YES];
                if (ev) {
                    [NSApp sendEvent:ev];
                }
            }

            now = TimeNow();
            for (int i = 0; i < app->windows.len; i++) {
                Window* w = app->windows[i];
                if (!w->plat || w->plat->nextTick <= 0) {
                    continue;
                }
                if (now >= w->plat->nextTick) {

                    WindowTimerTick(w);
                }
            }
        }
    }
    return app->exitCode;
}

}

int main(int argc, char** argv) {

    argc = gpui::GpuiTakeRuntimeArgs(argc, argv);
    return GpuiMain(argc, argv);
}

#endif

#if GPUI_OS_WINDOWS
#line 1 "src/gpui/Window_win.cpp"

namespace gpui {

static const wchar_t* kWndClass = L"Gpui2SystemMonitor";

struct PlatWindow {
    HWND hwnd = nullptr;

    HCURSOR cursor = nullptr;

    bool firstMouse = false;
};

static HWND Hwnd(Window* win) {
    return (win && win->plat) ? win->plat->hwnd : nullptr;
}

double TimeNow() {
    static LARGE_INTEGER freq = {};
    static LARGE_INTEGER start = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
}

typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);

static GetDpiForWindowFn kNoDpiFn = (GetDpiForWindowFn)1;

static float HostDpi(HWND hwnd) {
    static GetDpiForWindowFn getDpiForWindow = nullptr;
    if (!getDpiForWindow) {
        HMODULE user = GetModuleHandleW(L"user32.dll");
        if (user) {
            getDpiForWindow =
                (GetDpiForWindowFn)GetProcAddress(user, "GetDpiForWindow");
        }
        if (!getDpiForWindow) {
            getDpiForWindow = kNoDpiFn;
        }
    }
    UINT dpi = getDpiForWindow != kNoDpiFn ? getDpiForWindow(hwnd) : 96;
    if (dpi == 0) {
        dpi = 96;
    }
    return (float)dpi;
}

static void RenderFrame(Window* win, HDC hdc) {
    HWND hwnd = Hwnd(win);
    if (!hwnd) {
        return;
    }
    RECT rc = {};
    GetClientRect(hwnd, &rc);

    win->paint.dpi = 96;
    WINDOWPLACEMENT wp = {sizeof(wp)};
    GetWindowPlacement(hwnd, &wp);
    win->maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    int pxW = rc.right - rc.left;
    int pxH = rc.bottom - rc.top;
    WindowDrawFrame(win, hdc, pxW, pxH, (float)pxW, (float)pxH);
}

static int BorderPx() {
    return GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
}

static int BorderYPx() {
    return GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
}

static bool ClientDecorated(Window* win) {
    return win->opts.clientTitleBar || win->opts.borderless;
}

static bool ShiftDown() {
    return (GetKeyState(VK_SHIFT) & 0x8000) != 0;
}
static bool CtrlDown() {
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
}
static bool AltDown() {
    return (GetKeyState(VK_MENU) & 0x8000) != 0;
}

static Modifiers ModsNow() {
    Modifiers m;
    m.control = CtrlDown();
    m.alt = AltDown();
    m.shift = ShiftDown();
    m.platform = (GetKeyState(VK_LWIN) & 0x8000) != 0 ||
                 (GetKeyState(VK_RWIN) & 0x8000) != 0;
    return m;
}

static bool PressedButton(MouseButton* out) {
    struct {
        int vk;
        MouseButton button;
    } kButtons[] = {
        {VK_LBUTTON, MouseButton::Left},
        {VK_RBUTTON, MouseButton::Right},
        {VK_MBUTTON, MouseButton::Middle},
        {VK_XBUTTON1, MouseButton::NavigateBack},
        {VK_XBUTTON2, MouseButton::NavigateForward},
    };
    for (const auto& b : kButtons) {
        if (GetKeyState(b.vk) & 0x8000) {
            *out = b.button;
            return true;
        }
    }
    return false;
}

static void MouseDown(Window* win, MouseButton button, LPARAM lParam) {
    win->paint.dpi = HostDpi(Hwnd(win));
    float x = PxToDip(&win->paint, GET_X_LPARAM(lParam));
    float y = PxToDip(&win->paint, GET_Y_LPARAM(lParam));
    bool first = win->plat->firstMouse;
    win->plat->firstMouse = false;
    PlatformInput in = InputMouseDown(
        button, x, y, ModsNow(), WindowClickCount(win, x, y, button), first);
    WindowDispatchInput(win, &in);
}

static void MouseUp(Window* win, MouseButton button, LPARAM lParam) {
    float x = PxToDip(&win->paint, GET_X_LPARAM(lParam));
    float y = PxToDip(&win->paint, GET_Y_LPARAM(lParam));
    PlatformInput in =
        InputMouseUp(button, x, y, ModsNow(), WindowCurrentClickCount(win));
    WindowDispatchInput(win, &in);
}

static void MouseMove(Window* win, float x, float y) {
    MouseButton pressed = MouseButton::Left;
    bool any = PressedButton(&pressed);
    PlatformInput in = InputMouseMove(x, y, any, pressed, ModsNow());
    WindowDispatchInput(win, &in);
}

static void MouseExited(Window* win) {
    MouseButton pressed = MouseButton::Left;
    bool any = PressedButton(&pressed);
    PlatformInput in =
        InputMouseExited(win->mouseX, win->mouseY, any, pressed, ModsNow());
    WindowDispatchInput(win, &in);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
    Window* win = (Window*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lParam;
        win = (Window*)cs->lpCreateParams;
        win->plat->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)win);
    }
    if (!win) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_CREATE: {
            PlatSetTimer(win, WindowTimerMs(win));
            return 0;
        }
        case WM_KEYDOWN:
            WindowKeyDown(win, (int)wParam, ShiftDown(), CtrlDown(), AltDown());
            return 0;
        case WM_CHAR:
            WindowChar(win, (uint32_t)wParam, CtrlDown(), AltDown());
            return 0;
        case WM_MOUSEACTIVATE:

            win->plat->firstMouse = true;
            break;
        case WM_RBUTTONDOWN:
            MouseDown(win, MouseButton::Right, lParam);
            return 0;
        case WM_RBUTTONUP:
            MouseUp(win, MouseButton::Right, lParam);
            return 0;
        case WM_MBUTTONDOWN:
            MouseDown(win, MouseButton::Middle, lParam);
            return 0;
        case WM_MBUTTONUP:
            MouseUp(win, MouseButton::Middle, lParam);
            return 0;

        case WM_XBUTTONDOWN:
            MouseDown(win,
                      GET_XBUTTON_WPARAM(wParam) == XBUTTON1
                          ? MouseButton::NavigateBack
                          : MouseButton::NavigateForward,
                      lParam);
            return TRUE;
        case WM_XBUTTONUP:
            MouseUp(win,
                    GET_XBUTTON_WPARAM(wParam) == XBUTTON1
                        ? MouseButton::NavigateBack
                        : MouseButton::NavigateForward,
                    lParam);
            return TRUE;
        case WM_NCCALCSIZE: {

            if (!ClientDecorated(win) || wParam == 0) {
                break;
            }
            auto* p = (NCCALCSIZE_PARAMS*)lParam;
            LONG top = p->rgrc[0].top;
            LRESULT r = DefWindowProcW(hwnd, msg, wParam, lParam);
            p->rgrc[0].top = top;
            if (IsZoomed(hwnd)) {
                p->rgrc[0].top += BorderYPx();
            }
            return r;
        }
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
            if (hit != HTCLIENT) {
                return hit;
            }
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);

            if (ClientDecorated(win) && !IsZoomed(hwnd) && pt.y < BorderYPx()) {
                RECT rc = {};
                GetClientRect(hwnd, &rc);
                if (pt.x < BorderPx()) {
                    return HTTOPLEFT;
                }
                if (pt.x >= rc.right - BorderPx()) {
                    return HTTOPRIGHT;
                }
                return HTTOP;
            }
            float dipX = PxToDip(&win->paint, pt.x);
            float dipY = PxToDip(&win->paint, pt.y);
            switch (WindowChromeHit(win, dipX, dipY)) {
                case ClickWinMin:
                    return HTMINBUTTON;
                case ClickWinMax:
                    return HTMAXBUTTON;
                case ClickWinClose:
                    return HTCLOSE;
                case ClickWinCaption:
                    return HTCAPTION;
                default:
                    return HTCLIENT;
            }
        }
        case WM_SIZE:
            win->paint.dpi = HostDpi(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            auto* r = (RECT*)lParam;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            PaintTargetFree(&win->paint);
            return 0;
        }
        case WM_TIMER:
            WindowTimerTick(win);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RenderFrame(win, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            win->paint.dpi = HostDpi(hwnd);
            float x = PxToDip(&win->paint, GET_X_LPARAM(lParam));
            float y = PxToDip(&win->paint, GET_Y_LPARAM(lParam));
            MouseMove(win, x, y);
            TRACKMOUSEEVENT tme = {sizeof(tme)};
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            MouseExited(win);
            return 0;
        case WM_NCMOUSEMOVE: {

            if (wParam != HTCAPTION && wParam != HTMINBUTTON &&
                wParam != HTMAXBUTTON && wParam != HTCLOSE) {
                break;
            }
            win->paint.dpi = HostDpi(hwnd);
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            MouseMove(win, PxToDip(&win->paint, pt.x),
                      PxToDip(&win->paint, pt.y));
            TRACKMOUSEEVENT tme = {sizeof(tme)};
            tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            break;
        }
        case WM_NCMOUSELEAVE:
            MouseExited(win);
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            MouseDown(win, MouseButton::Left, lParam);
            return 0;
        case WM_LBUTTONUP:
            MouseUp(win, MouseButton::Left, lParam);
            return 0;
        case WM_NCLBUTTONDOWN:
            if (wParam == HTMINBUTTON) {
                AppMinimize(win);
                return 0;
            }
            if (wParam == HTMAXBUTTON) {
                AppToggleMaximize(win);
                return 0;
            }
            if (wParam == HTCLOSE) {
                AppClose(win);
                return 0;
            }
            break;

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            float x = PxToDip(&win->paint, pt.x);
            float y = PxToDip(&win->paint, pt.y);
            float delta = (float)GET_WHEEL_DELTA_WPARAM(wParam) /
                          (float)WHEEL_DELTA * 48.f;
            bool horizontal = msg == WM_MOUSEHWHEEL;
            PlatformInput in = InputScrollWheel(x, y, horizontal ? -delta : 0.f,
                                                horizontal ? 0.f : delta, false,
                                                ModsNow(), TouchPhase::Moved);
            WindowDispatchInput(win, &in);
            return 0;
        }
        case WM_SETCURSOR:

            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(win->plat->cursor ? win->plat->cursor
                                            : LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY: {
            KillTimer(hwnd, 1);
            App* app = win->app;
            delete win->plat;
            WindowClosed(win);

            if (!AppAnyWindowOpen(app)) {
                PostQuitMessage(0);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void AppQuit(Window* win) {
    HWND hwnd = Hwnd(win);
    if (hwnd) {
        DestroyWindow(hwnd);
    }
}

void AppInvalidate(Window* win) {
    HWND hwnd = Hwnd(win);
    if (hwnd) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void AppMinimize(Window* win) {
    HWND hwnd = Hwnd(win);
    if (hwnd) {
        ShowWindow(hwnd, SW_MINIMIZE);
    }
}

void AppToggleMaximize(Window* win) {
    HWND hwnd = Hwnd(win);
    if (!hwnd) {
        return;
    }
    WINDOWPLACEMENT wp = {sizeof(wp)};
    GetWindowPlacement(hwnd, &wp);
    ShowWindow(hwnd, wp.showCmd == SW_SHOWMAXIMIZED ? SW_RESTORE : SW_MAXIMIZE);
}

void AppDrag(Window* win) {
    HWND hwnd = Hwnd(win);
    if (hwnd) {
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
}

void AppSetTitle(Window* win, Str title) {
    HWND hwnd = Hwnd(win);
    if (hwnd) {
        SetWindowTextW(hwnd, ToCWstrTemp(title));
    }
}

void PlatSetTimer(Window* win, int ms) {
    HWND hwnd = Hwnd(win);
    if (!hwnd) {
        return;
    }
    if (ms > 0) {
        SetTimer(hwnd, 1, (UINT)ms, nullptr);
    } else {
        KillTimer(hwnd, 1);
    }
}

void PlatSetCursor(Window* win, CursorKind kind) {
    if (!win || !win->plat) {
        return;
    }
    LPCWSTR name = kind == CursorKind::IBeam ? IDC_IBEAM : IDC_ARROW;
    win->plat->cursor = LoadCursorW(nullptr, name);
    SetCursor(win->plat->cursor);
}

int PlatDoubleClickMs() {
    return (int)GetDoubleClickTime();
}

void ClipboardSetText(Window* win, Str text) {
    HWND hwnd = Hwnd(win);
    if (!text.s || text.len <= 0) {
        return;
    }
    WCHAR* w = ToCWstrTemp(text);
    int wn = (int)wcslen(w);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(wn + 1) * sizeof(WCHAR));
    if (!h) {
        return;
    }
    auto* dst = (WCHAR*)GlobalLock(h);
    if (!dst) {
        GlobalFree(h);
        return;
    }
    memcpy(dst, w, (size_t)(wn + 1) * sizeof(WCHAR));
    GlobalUnlock(h);
    if (!OpenClipboard(hwnd)) {
        GlobalFree(h);
        return;
    }
    EmptyClipboard();

    SetClipboardData(CF_UNICODETEXT, h);
    CloseClipboard();
}

bool PlatInit(App* app) {
    (void)app;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL(WINAPI * SetDpiFn)(HANDLE);
        auto setDpi =
            (SetDpiFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setDpi) {
            setDpi((HANDLE)-4);
        }
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);

        wc.hCursor = nullptr;
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = kWndClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    return true;
}

void PlatShutdown(App* app) {
    (void)app;
    CoUninitialize();
}

Window* WindowOpen(App* app, Str title, int dipW, int dipH, WinOpts opts) {
    Window* win = WindowAlloc(app, opts);
    if (!win) {
        return nullptr;
    }
    win->plat = new PlatWindow();

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (ClientDecorated(win)) {

        style = WS_OVERLAPPEDWINDOW & ~WS_CAPTION;
        style |= WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    }
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    WindowClampToDisplay(&dipW, &dipH, sx, sy);
    RECT wr = {0, 0, dipW, dipH};
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    int pxW = wr.right - wr.left;
    int pxH = wr.bottom - wr.top;
    int x = (sx - pxW) / 2;
    int y = (sy - pxH) / 2;

    WindowGeomRequested(&x, &y, &pxW, &pxH);

    HWND hwnd =
        CreateWindowExW(0, kWndClass, ToCWstrTemp(title), style, x, y, pxW, pxH,
                        nullptr, nullptr, GetModuleHandleW(nullptr), win);
    if (!hwnd) {
        delete win->plat;
        win->plat = nullptr;
        return nullptr;
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20  , &dark,
                          sizeof(dark));
    if (ClientDecorated(win)) {

        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE);
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return win;
}

int AppRun(App* app) {
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (app) {
        app->exitCode = (int)msg.wParam;
    }
    return (int)msg.wParam;
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv || argc <= 0) {
        char* argv0 = (char*)"gpui";
        char* argv[2] = {argv0, nullptr};
        return GpuiMain(1, argv);
    }

    auto** argv = (char**)calloc((size_t)argc + 1, sizeof(char*));
    if (!argv) {
        return 1;
    }
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0,
                                    nullptr, nullptr);
        if (n <= 0) {
            n = 1;
        }
        auto* buf = (char*)calloc((size_t)n, 1);
        if (!buf) {
            return 1;
        }
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, buf, n, nullptr, nullptr);
        argv[i] = buf;
    }
    LocalFree(wargv);
    argc = gpui::GpuiTakeRuntimeArgs(argc, argv);
    return GpuiMain(argc, argv);
}

#endif

#if GPUI_OS_LINUX
#line 1 "src/sys/SysInfo_linux.cpp"

namespace gpui {

static uint64_t TickTo100ns() {
    static uint64_t per = 0;
    if (per == 0) {
        long hz = sysconf(_SC_CLK_TCK);
        if (hz <= 0) {
            hz = 100;
        }
        per = 10000000ull / (uint64_t)hz;
    }
    return per;
}

void SysStateInit(SysState* s) {
    s->prevProcs.Reset();
    s->procs.Reset();
    s->cpu = 0;
    s->mem = 0;
    s->memTotal = 0;
    s->memUsed = 0;
    ZeroStruct(&s->prevCpu);
    ZeroStruct(&s->disk);
    ZeroStruct(&s->battery);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    s->ncpu = n > 0 ? (int)n : 1;
}

void SysStateFree(SysState* s) {
    s->prevProcs.Reset();
    s->procs.Reset();
}

static int ReadSmallFile(const char* path, char* buf, int cap) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    size_t n = fread(buf, 1, (size_t)cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

static void RefreshCpu(SysState* s) {
    char buf[512];
    if (ReadSmallFile("/proc/stat", buf, (int)sizeof(buf)) <= 0) {
        return;
    }

    unsigned long long v[10] = {};
    int n = sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                   &v[8], &v[9]);
    if (n < 4) {
        return;
    }
    uint64_t idle = v[3] + v[4];
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        total += v[i];
    }
    SysTimes cur;
    cur.idle = idle;

    cur.kernel = total;
    cur.user = 0;
    cur.valid = true;
    if (s->prevCpu.valid) {
        uint64_t idleD = cur.idle - s->prevCpu.idle;
        uint64_t totalD = cur.kernel - s->prevCpu.kernel;
        if (totalD > 0) {
            double used = (double)(totalD - idleD) / (double)totalD;
            s->cpu = (float)(used * 100.0);
        }
    }
    s->prevCpu = cur;
}

static uint64_t MeminfoKb(const char* text, const char* key) {
    const char* p = strstr(text, key);
    if (!p) {
        return 0;
    }
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '\t') {
        p++;
    }
    return strtoull(p, nullptr, 10) * 1024ull;
}

static void RefreshMemory(SysState* s) {
    char buf[4096];
    if (ReadSmallFile("/proc/meminfo", buf, (int)sizeof(buf)) <= 0) {
        return;
    }
    uint64_t total = MeminfoKb(buf, "MemTotal");
    uint64_t avail = MeminfoKb(buf, "MemAvailable");
    if (total == 0) {
        return;
    }
    if (avail == 0) {

        avail = MeminfoKb(buf, "MemFree") + MeminfoKb(buf, "Cached") +
                MeminfoKb(buf, "Buffers");
    }
    if (avail > total) {
        avail = total;
    }
    s->memTotal = total;
    s->memUsed = total - avail;
    s->mem = (float)((double)s->memUsed * 100.0 / (double)total);
}

static void RefreshDisk(SysState* s) {
    struct statvfs st = {};
    if (statvfs("/", &st) != 0 || st.f_blocks == 0) {
        return;
    }
    uint64_t unit = st.f_frsize ? st.f_frsize : st.f_bsize;
    s->disk.total = (uint64_t)st.f_blocks * unit;
    uint64_t avail = (uint64_t)st.f_bavail * unit;
    s->disk.used = s->disk.total > avail ? s->disk.total - avail : 0;
    if (s->disk.total > 0) {
        s->disk.usedPct =
            (float)((double)s->disk.used * 100.0 / (double)s->disk.total);
    }
}

static void RefreshBattery(SysState* s) {
    s->battery = {};
    DIR* d = opendir("/sys/class/power_supply");
    if (!d) {
        return;
    }
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char path[512];
        char buf[128];
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type",
                 ent->d_name);
        if (ReadSmallFile(path, buf, (int)sizeof(buf)) <= 0) {
            continue;
        }
        if (StrCmpNI(buf, "Battery", 7) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity",
                 ent->d_name);
        if (ReadSmallFile(path, buf, (int)sizeof(buf)) <= 0) {
            continue;
        }
        s->battery.present = true;
        s->battery.pct = (float)atoi(buf);
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status",
                 ent->d_name);
        if (ReadSmallFile(path, buf, (int)sizeof(buf)) > 0) {
            s->battery.charging = StrCmpNI(buf, "Charging", 8) == 0;
        }
        break;
    }
    closedir(d);
}

static uint64_t FindPrevCpu(const Vec<ProcSample>& prev, uint32_t pid) {
    for (int i = 0; i < prev.len; i++) {
        if (prev[i].pid == pid) {
            return prev[i].cpu100ns;
        }
    }
    return 0;
}

static bool ReadProcStat(const char* text, ProcessInfo* pi, uint64_t* cpu) {
    const char* open = strchr(text, '(');
    const char* close = strrchr(text, ')');
    if (!open || !close || close < open) {
        return false;
    }
    int nameLen = (int)(close - open - 1);
    if (nameLen > (int)sizeof(pi->name) - 1) {
        nameLen = (int)sizeof(pi->name) - 1;
    }
    if (nameLen > 0) {
        memcpy(pi->name, open + 1, (size_t)nameLen);
    }
    pi->name[nameLen > 0 ? nameLen : 0] = 0;

    const char* p = close + 1;
    unsigned long long utime = 0, stime = 0;
    long long rss = 0;
    int field = 2;
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        field++;
        if (field == 14) {
            utime = strtoull(p, nullptr, 10);
        } else if (field == 15) {
            stime = strtoull(p, nullptr, 10);
        } else if (field == 24) {
            rss = strtoll(p, nullptr, 10);
            break;
        }
        while (*p && *p != ' ') {
            p++;
        }
    }
    *cpu = (utime + stime) * TickTo100ns();
    long page = sysconf(_SC_PAGESIZE);
    pi->memory =
        rss > 0 ? (uint64_t)rss * (uint64_t)(page > 0 ? page : 4096) : 0;
    return true;
}

static void RefreshProcesses(SysState* s) {
    DIR* d = opendir("/proc");
    if (!d) {
        return;
    }
    Vec<ProcessInfo> next;
    Vec<ProcSample> samples;

    static uint64_t sPrevWall = 0;
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now =
        (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)(ts.tv_nsec / 100);
    uint64_t wallDelta = (sPrevWall && now > sPrevWall) ? (now - sPrevWall) : 0;
    sPrevWall = now;

    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
            continue;
        }
        uint32_t pid = (uint32_t)strtoul(ent->d_name, nullptr, 10);
        if (pid == 0) {
            continue;
        }
        char path[64];
        char buf[2048];
        snprintf(path, sizeof(path), "/proc/%u/stat", pid);
        if (ReadSmallFile(path, buf, (int)sizeof(buf)) <= 0) {
            continue;
        }
        ProcessInfo pi;
        pi.pid = pid;
        uint64_t cpu = 0;
        if (!ReadProcStat(buf, &pi, &cpu)) {
            continue;
        }
        uint64_t prev = FindPrevCpu(s->prevProcs, pid);
        if (prev && wallDelta > 0) {
            uint64_t delta = cpu >= prev ? cpu - prev : 0;
            pi.cpu = (float)((double)delta * 100.0 /
                             ((double)wallDelta * (double)s->ncpu));
        }
        ProcSample sm;
        sm.pid = pid;
        sm.cpu100ns = cpu;
        samples.Append(sm);
        next.Append(pi);
    }
    closedir(d);

    s->prevProcs.Reset();
    s->prevProcs = samples;
    s->procs.Reset();
    s->procs = next;
}

void SysRefresh(SysState* s) {
    RefreshCpu(s);
    RefreshMemory(s);
    RefreshDisk(s);
    RefreshBattery(s);
    RefreshProcesses(s);
}

}

#endif

#if GPUI_OS_MAC
#line 1 "src/sys/SysInfo_mac.cpp"

namespace gpui {

void SysStateInit(SysState* s) {
    s->prevProcs.Reset();
    s->procs.Reset();
    s->cpu = 0;
    s->mem = 0;
    s->memTotal = 0;
    s->memUsed = 0;
    ZeroStruct(&s->prevCpu);
    ZeroStruct(&s->disk);
    ZeroStruct(&s->battery);
    s->ncpu = PlatCoreCount();
}

void SysStateFree(SysState* s) {
    s->prevProcs.Reset();
    s->procs.Reset();
}

static void RefreshCpu(SysState* s) {
    host_cpu_load_info_data_t load = {};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&load, &count) != KERN_SUCCESS) {
        return;
    }
    uint64_t idle = load.cpu_ticks[CPU_STATE_IDLE];
    uint64_t total = 0;
    for (int i = 0; i < CPU_STATE_MAX; i++) {
        total += load.cpu_ticks[i];
    }
    SysTimes cur;
    cur.idle = idle;

    cur.kernel = total;
    cur.user = 0;
    cur.valid = true;
    if (s->prevCpu.valid) {
        uint64_t idleD = cur.idle - s->prevCpu.idle;
        uint64_t totalD = cur.kernel - s->prevCpu.kernel;
        if (totalD > 0) {
            double used = (double)(totalD - idleD) / (double)totalD;
            s->cpu = (float)(used * 100.0);
        }
    }
    s->prevCpu = cur;
}

static void RefreshMemory(SysState* s) {
    uint64_t total = 0;
    size_t len = sizeof(total);
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    if (sysctl(mib, 2, &total, &len, nullptr, 0) != 0 || total == 0) {
        return;
    }
    vm_statistics64_data_t vm = {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm,
                          &count) != KERN_SUCCESS) {
        return;
    }

    uint64_t page = (uint64_t)getpagesize();
    uint64_t avail = ((uint64_t)vm.free_count + (uint64_t)vm.purgeable_count +
                      (uint64_t)vm.external_page_count) *
                     page;
    if (avail > total) {
        avail = total;
    }
    s->memTotal = total;
    s->memUsed = total - avail;
    s->mem = (float)((double)s->memUsed * 100.0 / (double)total);
}

static void RefreshDisk(SysState* s) {
    struct statfs st = {};
    if (statfs("/", &st) != 0 || st.f_blocks == 0) {
        return;
    }
    uint64_t unit = st.f_bsize;
    s->disk.total = (uint64_t)st.f_blocks * unit;
    uint64_t avail = (uint64_t)st.f_bavail * unit;
    s->disk.used = s->disk.total > avail ? s->disk.total - avail : 0;
    if (s->disk.total > 0) {
        s->disk.usedPct =
            (float)((double)s->disk.used * 100.0 / (double)s->disk.total);
    }
}

static void RefreshBattery(SysState* s) {
    s->battery = {};
    CFTypeRef blob = IOPSCopyPowerSourcesInfo();
    if (!blob) {
        return;
    }
    CFArrayRef list = IOPSCopyPowerSourcesList(blob);
    if (!list) {
        CFRelease(blob);
        return;
    }
    CFIndex n = CFArrayGetCount(list);
    for (CFIndex i = 0; i < n; i++) {
        CFDictionaryRef desc = IOPSGetPowerSourceDescription(
            blob, CFArrayGetValueAtIndex(list, i));
        if (!desc) {
            continue;
        }
        auto type =
            (CFStringRef)CFDictionaryGetValue(desc, CFSTR(kIOPSTypeKey));
        if (!type || CFStringCompare(type, CFSTR(kIOPSInternalBatteryType),
                                     0) != kCFCompareEqualTo) {
            continue;
        }
        auto cur = (CFNumberRef)CFDictionaryGetValue(
            desc, CFSTR(kIOPSCurrentCapacityKey));
        auto max =
            (CFNumberRef)CFDictionaryGetValue(desc, CFSTR(kIOPSMaxCapacityKey));
        int curV = 0;
        int maxV = 0;
        if (cur) {
            CFNumberGetValue(cur, kCFNumberIntType, &curV);
        }
        if (max) {
            CFNumberGetValue(max, kCFNumberIntType, &maxV);
        }
        if (maxV <= 0) {
            continue;
        }
        s->battery.present = true;
        s->battery.pct = (float)((double)curV * 100.0 / (double)maxV);
        auto state = (CFStringRef)CFDictionaryGetValue(
            desc, CFSTR(kIOPSPowerSourceStateKey));
        s->battery.charging =
            state && CFStringCompare(state, CFSTR(kIOPSACPowerValue), 0) ==
                         kCFCompareEqualTo;
        break;
    }
    CFRelease(list);
    CFRelease(blob);
}

static uint64_t FindPrevCpu(const Vec<ProcSample>& prev, uint32_t pid) {
    for (int i = 0; i < prev.len; i++) {
        if (prev[i].pid == pid) {
            return prev[i].cpu100ns;
        }
    }
    return 0;
}

static uint64_t MachTicksTo100ns(uint64_t ticks) {
    static mach_timebase_info_data_t timebase = {};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }

    long double ns = (long double)ticks * (long double)timebase.numer /
                     (long double)timebase.denom;
    return (uint64_t)(ns / 100.0L);
}

static void RefreshProcesses(SysState* s) {
    int cap = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (cap <= 0) {
        return;
    }

    int nPids = cap / (int)sizeof(pid_t) + 32;
    Vec<pid_t> pids;
    pid_t* buf = pids.AppendBlanks(nPids);
    if (!buf) {
        return;
    }
    int got =
        proc_listpids(PROC_ALL_PIDS, 0, buf, (int)(nPids * (int)sizeof(pid_t)));
    if (got <= 0) {
        return;
    }
    int count = got / (int)sizeof(pid_t);

    Vec<ProcessInfo> next;
    Vec<ProcSample> samples;

    static uint64_t sPrevWall = 0;
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now =
        (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)(ts.tv_nsec / 100);
    uint64_t wallDelta = (sPrevWall && now > sPrevWall) ? (now - sPrevWall) : 0;
    sPrevWall = now;

    for (int i = 0; i < count; i++) {
        pid_t pid = buf[i];
        if (pid <= 0) {
            continue;
        }
        struct proc_taskallinfo info = {};
        int n = proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, &info, sizeof(info));
        if (n < (int)sizeof(info)) {
            continue;
        }
        ProcessInfo pi;
        pi.pid = (uint32_t)pid;
        StrCopyZ(
            pi.name, (int)sizeof(pi.name),
            info.pbsd.pbi_name[0] ? info.pbsd.pbi_name : info.pbsd.pbi_comm);
        pi.memory = info.ptinfo.pti_resident_size;
        uint64_t cpu = MachTicksTo100ns(info.ptinfo.pti_total_user +
                                        info.ptinfo.pti_total_system);
        uint64_t prev = FindPrevCpu(s->prevProcs, pi.pid);
        if (prev && wallDelta > 0) {
            uint64_t delta = cpu >= prev ? cpu - prev : 0;
            pi.cpu = (float)((double)delta * 100.0 /
                             ((double)wallDelta * (double)s->ncpu));
        }
        ProcSample sm;
        sm.pid = pi.pid;
        sm.cpu100ns = cpu;
        samples.Append(sm);
        next.Append(pi);
    }

    s->prevProcs.Reset();
    s->prevProcs = samples;
    s->procs.Reset();
    s->procs = next;
}

void SysRefresh(SysState* s) {
    RefreshCpu(s);
    RefreshMemory(s);
    RefreshDisk(s);
    RefreshBattery(s);
    RefreshProcesses(s);
}

}

#endif

#if GPUI_OS_WINDOWS
#line 1 "src/sys/SysInfo_win.cpp"

namespace gpui {

static uint64_t FtToU64(FILETIME ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

void SysStateInit(SysState* s) {
    s->prevProcs.Reset();
    s->procs.Reset();
    s->cpu = 0;
    s->mem = 0;
    s->memTotal = 0;
    s->memUsed = 0;
    ZeroStruct(&s->prevCpu);
    ZeroStruct(&s->disk);
    ZeroStruct(&s->battery);
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    s->ncpu = (int)si.dwNumberOfProcessors;
    if (s->ncpu < 1) {
        s->ncpu = 1;
    }
}

void SysStateFree(SysState* s) {
    s->prevProcs.Reset();
    s->procs.Reset();
}

static void RefreshCpu(SysState* s) {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        return;
    }
    SysTimes cur;
    cur.idle = FtToU64(idle);
    cur.kernel = FtToU64(kernel);
    cur.user = FtToU64(user);
    cur.valid = true;
    if (s->prevCpu.valid) {
        uint64_t idleD = cur.idle - s->prevCpu.idle;
        uint64_t totalD =
            (cur.kernel - s->prevCpu.kernel) + (cur.user - s->prevCpu.user);

        if (totalD > 0) {
            double used = (double)(totalD - idleD) / (double)totalD;
            s->cpu = (float)(used * 100.0);
        }
    }
    s->prevCpu = cur;
}

static void RefreshMemory(SysState* s) {
    MEMORYSTATUSEX ms = {sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) {
        s->memTotal = ms.ullTotalPhys;
        s->memUsed = ms.ullTotalPhys - ms.ullAvailPhys;
        s->mem = (float)ms.dwMemoryLoad;
    }
}

static void RefreshDisk(SysState* s) {
    WCHAR drives[512];
    DWORD n = GetLogicalDriveStringsW(511, drives);
    if (n == 0 || n > 511) {
        return;
    }
    WCHAR* p = drives;
    while (*p) {
        UINT type = GetDriveTypeW(p);
        if (type == DRIVE_FIXED) {
            ULARGE_INTEGER freeBytes, total, totalFree;
            if (GetDiskFreeSpaceExW(p, &freeBytes, &total, &totalFree)) {
                s->disk.total = total.QuadPart;
                s->disk.used = total.QuadPart - freeBytes.QuadPart;
                if (s->disk.total > 0) {
                    s->disk.usedPct = (float)((double)s->disk.used * 100.0 /
                                              (double)s->disk.total);
                }
                return;
            }
        }
        p += wcslen(p) + 1;
    }
}

static void RefreshBattery(SysState* s) {
    SYSTEM_POWER_STATUS ps = {};
    s->battery = {};
    if (!GetSystemPowerStatus(&ps)) {
        return;
    }
    if (ps.BatteryFlag == 128 || ps.BatteryLifePercent == 255) {
        return;
    }
    s->battery.present = true;
    s->battery.charging = (ps.ACLineStatus == 1) && (ps.BatteryFlag & 8);
    s->battery.pct = (float)ps.BatteryLifePercent;
}

static uint64_t FindPrevCpu(const Vec<ProcSample>& prev, uint32_t pid) {
    for (int i = 0; i < prev.len; i++) {
        if (prev[i].pid == pid) {
            return prev[i].cpu100ns;
        }
    }
    return 0;
}

static void RefreshProcesses(SysState* s) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }

    Vec<ProcessInfo> next;
    Vec<ProcSample> samples;
    PROCESSENTRY32W pe = {sizeof(pe)};
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    static uint64_t sPrevWall = 0;
    uint64_t now = FtToU64(nowFt);
    uint64_t wallDelta = (sPrevWall && now > sPrevWall) ? (now - sPrevWall) : 0;
    sPrevWall = now;

    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0) {
                continue;
            }
            ProcessInfo pi;
            pi.pid = pe.th32ProcessID;
            int n =
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, pi.name,
                                    (int)sizeof(pi.name) - 1, nullptr, nullptr);
            if (n <= 0) {
                pi.name[0] = 0;
            }

            HANDLE h =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                            FALSE, pe.th32ProcessID);
            if (!h) {
                h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                pe.th32ProcessID);
            }
            if (h) {
                PROCESS_MEMORY_COUNTERS pmc = {sizeof(pmc)};
                if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
                    pi.memory = pmc.WorkingSetSize;
                }
                FILETIME c, e, k, u;
                if (GetProcessTimes(h, &c, &e, &k, &u)) {
                    uint64_t cpu = FtToU64(k) + FtToU64(u);
                    uint64_t prev = FindPrevCpu(s->prevProcs, pi.pid);
                    if (prev && wallDelta > 0) {
                        uint64_t d = cpu >= prev ? cpu - prev : 0;
                        pi.cpu = (float)((double)d * 100.0 /
                                         ((double)wallDelta * (double)s->ncpu));
                    }
                    ProcSample sm;
                    sm.pid = pi.pid;
                    sm.cpu100ns = cpu;
                    samples.Append(sm);
                }
                CloseHandle(h);
            }
            next.Append(pi);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    s->prevProcs.Reset();
    s->prevProcs = samples;
    s->procs.Reset();
    s->procs = next;
}

void SysRefresh(SysState* s) {
    RefreshCpu(s);
    RefreshMemory(s);
    RefreshDisk(s);
    RefreshBattery(s);
    RefreshProcesses(s);
}

}

#endif

#if GPUI_OS_LINUX
#line 1 "src/Base_linux.cpp"

namespace gpui {

void PlatDirNameInPlace(char* path);

void PlatGetExeDir(char* out, int cap) {
    if (!out || cap <= 0) {
        return;
    }
    out[0] = 0;
    ssize_t n = readlink("/proc/self/exe", out, (size_t)cap - 1);
    if (n <= 0) {
        return;
    }
    out[n] = 0;
    PlatDirNameInPlace(out);
}

bool PlatSelfUsage(uint64_t* cpu100ns, uint64_t* memBytes) {

    struct rusage ru = {};
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        return false;
    }
    if (cpu100ns) {
        uint64_t us = (uint64_t)ru.ru_utime.tv_sec * 1000000ull +
                      (uint64_t)ru.ru_utime.tv_usec +
                      (uint64_t)ru.ru_stime.tv_sec * 1000000ull +
                      (uint64_t)ru.ru_stime.tv_usec;
        *cpu100ns = us * 10ull;
    }
    if (memBytes) {
        *memBytes = 0;
        FILE* f = fopen("/proc/self/statm", "rb");
        if (f) {
            unsigned long total = 0, resident = 0;
            if (fscanf(f, "%lu %lu", &total, &resident) == 2) {
                long page = sysconf(_SC_PAGESIZE);
                *memBytes =
                    (uint64_t)resident * (uint64_t)(page > 0 ? page : 4096);
            }
            fclose(f);
        }
    }
    return true;
}

}

#endif

#if GPUI_OS_MAC
#line 1 "src/Base_mac.cpp"

namespace gpui {

void PlatDirNameInPlace(char* path);

void PlatGetExeDir(char* out, int cap) {
    if (!out || cap <= 0) {
        return;
    }
    out[0] = 0;
    uint32_t n = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &n) != 0) {
        out[0] = 0;
        return;
    }
    out[cap - 1] = 0;
    PlatDirNameInPlace(out);
}

bool PlatSelfUsage(uint64_t* cpu100ns, uint64_t* memBytes) {

    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    task_basic_info_data_t basic = {};
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&basic,
                  &count) != KERN_SUCCESS) {
        return false;
    }
    if (memBytes) {
        *memBytes = (uint64_t)basic.resident_size;
    }
    if (cpu100ns) {
        uint64_t us = (uint64_t)basic.user_time.seconds * 1000000ull +
                      (uint64_t)basic.user_time.microseconds +
                      (uint64_t)basic.system_time.seconds * 1000000ull +
                      (uint64_t)basic.system_time.microseconds;
        count = TASK_THREAD_TIMES_INFO_COUNT;
        task_thread_times_info_data_t threads = {};
        if (task_info(mach_task_self(), TASK_THREAD_TIMES_INFO,
                      (task_info_t)&threads, &count) == KERN_SUCCESS) {
            us += (uint64_t)threads.user_time.seconds * 1000000ull +
                  (uint64_t)threads.user_time.microseconds +
                  (uint64_t)threads.system_time.seconds * 1000000ull +
                  (uint64_t)threads.system_time.microseconds;
        }
        *cpu100ns = us * 10ull;
    }
    return true;
}

}

#endif

#if GPUI_OS_LINUX || GPUI_OS_MAC
#line 1 "src/Base_posix.cpp"

namespace gpui {

uint64_t PlatPageSize() {
    static uint64_t pageSize = 0;
    if (pageSize == 0) {
        long n = sysconf(_SC_PAGESIZE);
        pageSize = n > 0 ? (uint64_t)n : 4096;
    }
    return pageSize;
}

uint64_t PlatLargePageSize() {
    return 2ull * 1024ull * 1024ull;
}

void* PlatMemReserve(uint64_t size) {
    if (size == 0) {
        return nullptr;
    }
    void* p = mmap(nullptr, (size_t)size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

bool PlatMemCommit(void* base, uint64_t size, bool largePages) {
    (void)largePages;
    if (size == 0) {
        return true;
    }
    if (!base) {
        return false;
    }

    uint64_t page = PlatPageSize();
    uintptr_t start = (uintptr_t)base & ~(uintptr_t)(page - 1);
    uintptr_t end =
        ((uintptr_t)base + (uintptr_t)size + page - 1) & ~(uintptr_t)(page - 1);
    return mprotect((void*)start, (size_t)(end - start),
                    PROT_READ | PROT_WRITE) == 0;
}

void* PlatMemReserveCommit(uint64_t size, bool largePages) {
    (void)largePages;
    if (size == 0) {
        return nullptr;
    }
    void* p = mmap(nullptr, (size_t)size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

void PlatMemRelease(void* base, uint64_t size) {
    if (base && size > 0) {
        munmap(base, (size_t)size);
    }
}

int StrCmpI(const char* a, const char* b) {
    return strcasecmp(a ? a : "", b ? b : "");
}

int StrCmpNI(const char* a, const char* b, int n) {
    if (n <= 0) {
        return 0;
    }
    return strncasecmp(a ? a : "", b ? b : "", (size_t)n);
}

void StrCopyZ(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) {
        return;
    }
    if (!src) {
        dst[0] = 0;
        return;
    }
    int n = (int)strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(dst, src, (size_t)n);
    dst[n] = 0;
}

bool PlatDirExists(const char* path) {
    if (!path || !path[0]) {
        return false;
    }
    struct stat st = {};
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void PlatGetCwd(char* out, int cap) {
    if (!out || cap <= 0) {
        return;
    }
    out[0] = 0;
    if (!getcwd(out, (size_t)cap)) {
        out[0] = 0;
    }
}

void PlatDirNameInPlace(char* path) {
    if (!path) {
        return;
    }
    int i = (int)strlen(path);
    while (i > 0 && path[i - 1] != '/') {
        path[--i] = 0;
    }
    while (i > 1 && path[i - 1] == '/') {
        path[--i] = 0;
    }
}

int PlatListDir(const char* dir, DirEntry* out, int max) {
    if (!dir || !out || max <= 0) {
        return 0;
    }
    DIR* d = opendir(dir);
    if (!d) {
        return 0;
    }
    int n = 0;
    struct dirent* ent = nullptr;
    while (n < max && (ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        DirEntry& e = out[n];
        StrCopyZ(e.name, (int)sizeof(e.name), ent->d_name);
        if (ent->d_type == DT_DIR) {
            e.isDir = true;
        } else if (ent->d_type == DT_UNKNOWN) {

            char full[kMaxPath];
            snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
            e.isDir = PlatDirExists(full);
        } else {
            e.isDir = false;
        }
        n++;
    }
    closedir(d);
    return n;
}

int PlatCoreCount() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

}

#endif

#if GPUI_OS_WINDOWS
#line 1 "src/Base_win.cpp"

namespace gpui {

uint64_t PlatPageSize() {
    static uint64_t pageSize = 0;
    if (pageSize == 0) {
        SYSTEM_INFO info = {};
        GetSystemInfo(&info);
        pageSize = info.dwPageSize;
    }
    return pageSize;
}

uint64_t PlatLargePageSize() {
    static uint64_t largePageSize = 0;
    if (largePageSize == 0) {
        SIZE_T size = GetLargePageMinimum();
        largePageSize = size ? (uint64_t)size : PlatPageSize();
    }
    return largePageSize;
}

bool PlatMemCommit(void* base, uint64_t size, bool largePages) {
    if (size == 0) {
        return true;
    }
    DWORD flags = MEM_COMMIT;
    if (largePages) {
        flags |= MEM_LARGE_PAGES;
    }
    return VirtualAlloc(base, (SIZE_T)size, flags, PAGE_READWRITE) != nullptr;
}

void* PlatMemReserve(uint64_t size) {
    return VirtualAlloc(nullptr, (SIZE_T)size, MEM_RESERVE, PAGE_READWRITE);
}

void* PlatMemReserveCommit(uint64_t size, bool largePages) {
    DWORD flags = MEM_RESERVE | MEM_COMMIT;
    if (largePages) {
        flags |= MEM_LARGE_PAGES;
    }
    return VirtualAlloc(nullptr, (SIZE_T)size, flags, PAGE_READWRITE);
}

void PlatMemRelease(void* base, uint64_t size) {
    (void)size;
    VirtualFree(base, 0, MEM_RELEASE);
}

int StrCmpI(const char* a, const char* b) {
    return _stricmp(a ? a : "", b ? b : "");
}

int StrCmpNI(const char* a, const char* b, int n) {
    if (n <= 0) {
        return 0;
    }
    return _strnicmp(a ? a : "", b ? b : "", (size_t)n);
}

void StrCopyZ(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) {
        return;
    }
    strncpy_s(dst, (size_t)cap, src ? src : "", _TRUNCATE);
}

WCHAR* ToCWstrTemp(Str s) {
    Arena* arena = GetTempArena();
    int n = 0;
    if (s.s && s.len > 0) {
        n = MultiByteToWideChar(CP_UTF8, 0, s.s, s.len, nullptr, 0);
        if (n < 0) {
            n = 0;
        }
    }
    auto res = (WCHAR*)arena->Push((uint64_t)(n + 1) * sizeof(WCHAR),
                                   alignof(WCHAR), false);
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s.s, s.len, res, n);
    }
    res[n] = 0;
    return res;
}

bool PlatDirExists(const char* path) {
    if (!path || !path[0]) {
        return false;
    }
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void PlatGetCwd(char* out, int cap) {
    if (!out || cap <= 0) {
        return;
    }
    out[0] = 0;
    GetCurrentDirectoryA((DWORD)cap, out);
}

void PlatGetExeDir(char* out, int cap) {
    if (!out || cap <= 0) {
        return;
    }
    out[0] = 0;
    GetModuleFileNameA(nullptr, out, (DWORD)cap);
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/') {
        out[--n] = 0;
    }
    while (n > 0 && (out[n - 1] == '\\' || out[n - 1] == '/')) {
        out[--n] = 0;
    }
}

int PlatListDir(const char* dir, DirEntry* out, int max) {
    if (!dir || !out || max <= 0) {
        return 0;
    }
    char pattern[kMaxPath];
    _snprintf_s(pattern, kMaxPath, _TRUNCATE, "%s\\*", dir);
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(ToCWstrTemp(Str(pattern)), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int n = 0;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 ||
             (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
            continue;
        }
        DirEntry& e = out[n];
        int got = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, e.name,
                                      (int)sizeof(e.name), nullptr, nullptr);
        if (got <= 0) {
            continue;
        }
        e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        n++;
    } while (n < max && FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

int PlatCoreCount() {
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

bool PlatSelfUsage(uint64_t* cpu100ns, uint64_t* memBytes) {
    HANDLE self = GetCurrentProcess();
    FILETIME creation = {}, exit = {}, kernel = {}, user = {};
    if (!GetProcessTimes(self, &creation, &exit, &kernel, &user)) {
        return false;
    }
    ULARGE_INTEGER k = {}, u = {};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    if (cpu100ns) {
        *cpu100ns = k.QuadPart + u.QuadPart;
    }
    PROCESS_MEMORY_COUNTERS mem = {};
    mem.cb = sizeof(mem);
    if (!GetProcessMemoryInfo(self, &mem, sizeof(mem))) {
        return false;
    }
    if (memBytes) {
        *memBytes = (uint64_t)mem.WorkingSetSize;
    }
    return true;
}

}

#endif

#if defined(_MSC_VER)
#pragma warning(push, 0)
#pragma warning(disable : 4701 4702)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#undef MIN
#undef MAX
#line 1 "ext/md4c/md4c.c"

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199409L

    #if defined __GNUC__
        #define inline __inline__
    #elif defined _MSC_VER
        #define inline __inline
    #else
        #define inline
    #endif
#endif

#if !defined MD4C_USE_ASCII && !defined MD4C_USE_UTF8 && !defined MD4C_USE_UTF16
    #define MD4C_USE_UTF8
#endif

#ifdef _T
    #undef _T
#endif
#if defined MD4C_USE_UTF16
    #define _T(x)           L##x
#else
    #define _T(x)           x
#endif

#define SIZEOF_ARRAY(a)     (sizeof(a) / sizeof(a[0]))

#define STRINGIZE_(x)       #x
#define STRINGIZE(x)        STRINGIZE_(x)

#define MAX(a,b)            ((a) > (b) ? (a) : (b))
#define MIN(a,b)            ((a) < (b) ? (a) : (b))

#ifndef TRUE
    #define TRUE            1
    #define FALSE           0
#endif

#define MD_LOG(msg)                                                     \
    do {                                                                \
        if(ctx->parser.debug_log != NULL)                               \
            ctx->parser.debug_log((msg), ctx->userdata);                \
    } while(0)

#ifdef DEBUG
    #define MD_ASSERT(cond)                                             \
            do {                                                        \
                if(!(cond)) {                                           \
                    MD_LOG(__FILE__ ":" STRINGIZE(__LINE__) ": "        \
                           "Assertion '" STRINGIZE(cond) "' failed.");  \
                    exit(1);                                            \
                }                                                       \
            } while(0)

    #define MD_UNREACHABLE()        MD_ASSERT(1 == 0)
#else
    #ifdef __GNUC__
        #define MD_ASSERT(cond)     do { if(!(cond)) __builtin_unreachable(); } while(0)
        #define MD_UNREACHABLE()    do { __builtin_unreachable(); } while(0)
    #elif defined _MSC_VER  &&  _MSC_VER > 120
        #define MD_ASSERT(cond)     do { __assume(cond); } while(0)
        #define MD_UNREACHABLE()    do { __assume(0); } while(0)
    #else
        #define MD_ASSERT(cond)     do {} while(0)
        #define MD_UNREACHABLE()    do {} while(0)
    #endif
#endif

#if defined __clang__ && __clang_major__ >= 12
    #define MD_FALLTHROUGH()        __attribute__((fallthrough))
#elif defined __GNUC__ && __GNUC__ >= 7
    #define MD_FALLTHROUGH()        __attribute__((fallthrough))
#else
    #define MD_FALLTHROUGH()        ((void)0)
#endif

#define MD_UNUSED(x)                ((void)x)

#define CODESPAN_MARK_MAXLEN    32

#define TABLE_MAXCOLCOUNT       128

#define CHAR    MD_CHAR
#define SZ      MD_SIZE
#define OFF     MD_OFFSET

typedef struct MD_MARK_tag MD_MARK;
typedef struct MD_BLOCK_tag MD_BLOCK;
typedef struct MD_CONTAINER_tag MD_CONTAINER;
typedef struct MD_REF_DEF_tag MD_REF_DEF;

typedef struct MD_MARKSTACK_tag MD_MARKSTACK;
struct MD_MARKSTACK_tag {
    int top;
};

typedef struct MD_CTX_tag MD_CTX;
struct MD_CTX_tag {

    const CHAR* text;
    SZ size;
    MD_PARSER parser;
    void* userdata;

    int doc_ends_with_newline;

    CHAR* buffer;
    unsigned alloc_buffer;

    MD_REF_DEF* ref_defs;
    int n_ref_defs;
    int alloc_ref_defs;
    void** ref_def_hashtable;
    int ref_def_hashtable_size;
    SZ max_ref_def_output;

    MD_MARK* marks;
    int n_marks;
    int alloc_marks;

#if defined MD4C_USE_UTF16
    char mark_char_map[128];
#else
    char mark_char_map[256];
#endif

    MD_MARKSTACK opener_stacks[16];
#define ASTERISK_OPENERS_oo_mod3_0      (ctx->opener_stacks[0])
#define ASTERISK_OPENERS_oo_mod3_1      (ctx->opener_stacks[1])
#define ASTERISK_OPENERS_oo_mod3_2      (ctx->opener_stacks[2])
#define ASTERISK_OPENERS_oc_mod3_0      (ctx->opener_stacks[3])
#define ASTERISK_OPENERS_oc_mod3_1      (ctx->opener_stacks[4])
#define ASTERISK_OPENERS_oc_mod3_2      (ctx->opener_stacks[5])
#define UNDERSCORE_OPENERS_oo_mod3_0    (ctx->opener_stacks[6])
#define UNDERSCORE_OPENERS_oo_mod3_1    (ctx->opener_stacks[7])
#define UNDERSCORE_OPENERS_oo_mod3_2    (ctx->opener_stacks[8])
#define UNDERSCORE_OPENERS_oc_mod3_0    (ctx->opener_stacks[9])
#define UNDERSCORE_OPENERS_oc_mod3_1    (ctx->opener_stacks[10])
#define UNDERSCORE_OPENERS_oc_mod3_2    (ctx->opener_stacks[11])
#define TILDE_OPENERS_1                 (ctx->opener_stacks[12])
#define TILDE_OPENERS_2                 (ctx->opener_stacks[13])
#define BRACKET_OPENERS                 (ctx->opener_stacks[14])
#define DOLLAR_OPENERS                  (ctx->opener_stacks[15])

    MD_MARKSTACK ptr_stack;

    int n_table_cell_boundaries;
    int table_cell_boundaries_head;
    int table_cell_boundaries_tail;

    int unresolved_link_head;
    int unresolved_link_tail;

    OFF html_comment_horizon;
    OFF html_proc_instr_horizon;
    OFF html_decl_horizon;
    OFF html_cdata_horizon;

    void* block_bytes;
    MD_BLOCK* current_block;
    int n_block_bytes;
    int alloc_block_bytes;

    MD_CONTAINER* containers;
    int n_containers;
    int alloc_containers;

    unsigned code_indent_offset;

    SZ code_fence_length;
    int html_block_type;
    int last_line_has_list_loosening_effect;
    int last_list_item_starts_with_two_blank_lines;
};

enum MD_LINETYPE_tag {
    MD_LINE_BLANK,
    MD_LINE_HR,
    MD_LINE_ATXHEADER,
    MD_LINE_SETEXTHEADER,
    MD_LINE_SETEXTUNDERLINE,
    MD_LINE_INDENTEDCODE,
    MD_LINE_FENCEDCODE,
    MD_LINE_HTML,
    MD_LINE_TEXT,
    MD_LINE_TABLE,
    MD_LINE_TABLEUNDERLINE
};
typedef enum MD_LINETYPE_tag MD_LINETYPE;

typedef struct MD_LINE_ANALYSIS_tag MD_LINE_ANALYSIS;
struct MD_LINE_ANALYSIS_tag {
    MD_LINETYPE type;
    unsigned data;
    int enforce_new_block;
    OFF beg;
    OFF end;
    unsigned indent;
};

typedef struct MD_LINE_tag MD_LINE;
struct MD_LINE_tag {
    OFF beg;
    OFF end;
};

typedef struct MD_VERBATIMLINE_tag MD_VERBATIMLINE;
struct MD_VERBATIMLINE_tag {
    OFF beg;
    OFF end;
    OFF indent;
};

#define CH(off)                 (ctx->text[(off)])
#define STR(off)                (ctx->text + (off))

#define ISIN_(ch, ch_min, ch_max)       ((ch_min) <= (unsigned)(ch) && (unsigned)(ch) <= (ch_max))
#define ISANYOF_(ch, palette)           ((ch) != _T('\0')  &&  md_strchr((palette), (ch)) != NULL)
#define ISANYOF2_(ch, ch1, ch2)         ((ch) == (ch1) || (ch) == (ch2))
#define ISANYOF3_(ch, ch1, ch2, ch3)    ((ch) == (ch1) || (ch) == (ch2) || (ch) == (ch3))
#define ISASCII_(ch)                    ((unsigned)(ch) <= 127)
#define ISBLANK_(ch)                    (ISANYOF2_((ch), _T(' '), _T('\t')))
#define ISNEWLINE_(ch)                  (ISANYOF2_((ch), _T('\r'), _T('\n')))
#define ISWHITESPACE_(ch)               (ISBLANK_(ch) || ISANYOF2_((ch), _T('\v'), _T('\f')))
#define ISCNTRL_(ch)                    ((unsigned)(ch) <= 31 || (unsigned)(ch) == 127)
#define ISPUNCT_(ch)                    (ISIN_(ch, 33, 47) || ISIN_(ch, 58, 64) || ISIN_(ch, 91, 96) || ISIN_(ch, 123, 126))
#define ISUPPER_(ch)                    (ISIN_(ch, _T('A'), _T('Z')))
#define ISLOWER_(ch)                    (ISIN_(ch, _T('a'), _T('z')))
#define ISALPHA_(ch)                    (ISUPPER_(ch) || ISLOWER_(ch))
#define ISDIGIT_(ch)                    (ISIN_(ch, _T('0'), _T('9')))
#define ISXDIGIT_(ch)                   (ISDIGIT_(ch) || ISIN_(ch, _T('A'), _T('F')) || ISIN_(ch, _T('a'), _T('f')))
#define ISALNUM_(ch)                    (ISALPHA_(ch) || ISDIGIT_(ch))

#define ISANYOF(off, palette)           ISANYOF_(CH(off), (palette))
#define ISANYOF2(off, ch1, ch2)         ISANYOF2_(CH(off), (ch1), (ch2))
#define ISANYOF3(off, ch1, ch2, ch3)    ISANYOF3_(CH(off), (ch1), (ch2), (ch3))
#define ISASCII(off)                    ISASCII_(CH(off))
#define ISBLANK(off)                    ISBLANK_(CH(off))
#define ISNEWLINE(off)                  ISNEWLINE_(CH(off))
#define ISWHITESPACE(off)               ISWHITESPACE_(CH(off))
#define ISCNTRL(off)                    ISCNTRL_(CH(off))
#define ISPUNCT(off)                    ISPUNCT_(CH(off))
#define ISUPPER(off)                    ISUPPER_(CH(off))
#define ISLOWER(off)                    ISLOWER_(CH(off))
#define ISALPHA(off)                    ISALPHA_(CH(off))
#define ISDIGIT(off)                    ISDIGIT_(CH(off))
#define ISXDIGIT(off)                   ISXDIGIT_(CH(off))
#define ISALNUM(off)                    ISALNUM_(CH(off))

#if defined MD4C_USE_UTF16
    #define md_strchr wcschr
#else
    #define md_strchr strchr
#endif

static inline int
md_ascii_case_eq(const CHAR* s1, const CHAR* s2, SZ n)
{
    OFF i;
    for(i = 0; i < n; i++) {
        CHAR ch1 = s1[i];
        CHAR ch2 = s2[i];

        if(ISLOWER_(ch1))
            ch1 += ('A'-'a');
        if(ISLOWER_(ch2))
            ch2 += ('A'-'a');
        if(ch1 != ch2)
            return FALSE;
    }
    return TRUE;
}

static inline int
md_ascii_eq(const CHAR* s1, const CHAR* s2, SZ n)
{
    return memcmp(s1, s2, n * sizeof(CHAR)) == 0;
}

static int
md_text_with_null_replacement(MD_CTX* ctx, MD_TEXTTYPE type, const CHAR* str, SZ size)
{
    OFF off = 0;
    int ret = 0;

    while(1) {
        while(off < size  &&  str[off] != _T('\0'))
            off++;

        if(off > 0) {
            ret = ctx->parser.text(type, str, off, ctx->userdata);
            if(ret != 0)
                return ret;

            str += off;
            size -= off;
            off = 0;
        }

        if(off >= size)
            return 0;

        ret = ctx->parser.text(MD_TEXT_NULLCHAR, _T(""), 1, ctx->userdata);
        if(ret != 0)
            return ret;
        off++;
    }
}

#define MD_CHECK(func)                                                      \
    do {                                                                    \
        ret = (func);                                                       \
        if(ret != 0)                                                        \
            goto abort;                                                     \
    } while(0)

#define MD_TEMP_BUFFER(sz)                                                  \
    do {                                                                    \
        if(sz > ctx->alloc_buffer) {                                        \
            CHAR* new_buffer;                                               \
            SZ new_size = ((sz) + (sz) / 2 + 128) & ~127;                   \
                                                                            \
            new_buffer = (CHAR*) realloc(ctx->buffer, new_size);                    \
            if(new_buffer == NULL) {                                        \
                MD_LOG("realloc() failed.");                                \
                ret = -1;                                                   \
                goto abort;                                                 \
            }                                                               \
                                                                            \
            ctx->buffer = new_buffer;                                       \
            ctx->alloc_buffer = new_size;                                   \
        }                                                                   \
    } while(0)

#define MD_ENTER_BLOCK(type, arg)                                           \
    do {                                                                    \
        ret = ctx->parser.enter_block((type), (arg), ctx->userdata);        \
        if(ret != 0) {                                                      \
            MD_LOG("Aborted from enter_block() callback.");                 \
            goto abort;                                                     \
        }                                                                   \
    } while(0)

#define MD_LEAVE_BLOCK(type, arg)                                           \
    do {                                                                    \
        ret = ctx->parser.leave_block((type), (arg), ctx->userdata);        \
        if(ret != 0) {                                                      \
            MD_LOG("Aborted from leave_block() callback.");                 \
            goto abort;                                                     \
        }                                                                   \
    } while(0)

#define MD_ENTER_SPAN(type, arg)                                            \
    do {                                                                    \
        ret = ctx->parser.enter_span((type), (arg), ctx->userdata);         \
        if(ret != 0) {                                                      \
            MD_LOG("Aborted from enter_span() callback.");                  \
            goto abort;                                                     \
        }                                                                   \
    } while(0)

#define MD_LEAVE_SPAN(type, arg)                                            \
    do {                                                                    \
        ret = ctx->parser.leave_span((type), (arg), ctx->userdata);         \
        if(ret != 0) {                                                      \
            MD_LOG("Aborted from leave_span() callback.");                  \
            goto abort;                                                     \
        }                                                                   \
    } while(0)

#define MD_TEXT(type, str, size)                                            \
    do {                                                                    \
        if(size > 0) {                                                      \
            ret = ctx->parser.text((type), (str), (size), ctx->userdata);   \
            if(ret != 0) {                                                  \
                MD_LOG("Aborted from text() callback.");                    \
                goto abort;                                                 \
            }                                                               \
        }                                                                   \
    } while(0)

#define MD_TEXT_INSECURE(type, str, size)                                   \
    do {                                                                    \
        if(size > 0) {                                                      \
            ret = md_text_with_null_replacement(ctx, type, str, size);      \
            if(ret != 0) {                                                  \
                MD_LOG("Aborted from text() callback.");                    \
                goto abort;                                                 \
            }                                                               \
        }                                                                   \
    } while(0)

static const MD_LINE*
md_lookup_line(OFF off, const MD_LINE* lines, MD_SIZE n_lines, MD_SIZE* p_line_index)
{
    MD_SIZE lo, hi;
    MD_SIZE pivot;
    const MD_LINE* line;

    lo = 0;
    hi = n_lines - 1;
    while(lo <= hi) {
        pivot = (lo + hi) / 2;
        line = &lines[pivot];

        if(off < line->beg) {
            if(hi == 0  ||  lines[hi-1].end < off) {
                if(p_line_index != NULL)
                    *p_line_index = pivot;
                return line;
            }
            hi = pivot - 1;
        } else if(off > line->end) {
            lo = pivot + 1;
        } else {
            if(p_line_index != NULL)
                *p_line_index = pivot;
            return line;
        }
    }

    return NULL;
}

typedef struct MD_UNICODE_FOLD_INFO_tag MD_UNICODE_FOLD_INFO;
struct MD_UNICODE_FOLD_INFO_tag {
    unsigned codepoints[3];
    unsigned n_codepoints;
};

#if defined MD4C_USE_UTF16 || defined MD4C_USE_UTF8

    static int
    md_unicode_bsearch__(unsigned codepoint, const unsigned* map, size_t map_size)
    {
        int beg, end;
        int pivot_beg, pivot_end;

        beg = 0;
        end = (int) map_size-1;
        while(beg <= end) {

            pivot_beg = pivot_end = (beg + end) / 2;
            if(map[pivot_end] & 0x40000000)
                pivot_end++;
            if(map[pivot_beg] & 0x80000000)
                pivot_beg--;

            if(codepoint < (map[pivot_beg] & 0x00ffffff))
                end = pivot_beg - 1;
            else if(codepoint > (map[pivot_end] & 0x00ffffff))
                beg = pivot_end + 1;
            else
                return pivot_beg;
        }

        return -1;
    }

    static int
    md_is_unicode_whitespace__(unsigned codepoint)
    {
#define R(cp_min, cp_max)   ((cp_min) | 0x40000000), ((cp_max) | 0x80000000)
#define S(cp)               (cp)

        static const unsigned WHITESPACE_MAP[] = {
            S(0x0020), S(0x00a0), S(0x1680), R(0x2000,0x200a), S(0x202f), S(0x205f), S(0x3000)
        };
#undef R
#undef S

        if(codepoint <= 0x7f)
            return ISWHITESPACE_(codepoint);

        return (md_unicode_bsearch__(codepoint, WHITESPACE_MAP, SIZEOF_ARRAY(WHITESPACE_MAP)) >= 0);
    }

    static int
    md_is_unicode_punct__(unsigned codepoint)
    {
#define R(cp_min, cp_max)   ((cp_min) | 0x40000000), ((cp_max) | 0x80000000)
#define S(cp)               (cp)

        static const unsigned PUNCT_MAP[] = {
            R(0x0021,0x002f), R(0x003a,0x0040), R(0x005b,0x0060), R(0x007b,0x007e), R(0x00a1,0x00a9),
            R(0x00ab,0x00ac), R(0x00ae,0x00b1), S(0x00b4), R(0x00b6,0x00b8), S(0x00bb), S(0x00bf), S(0x00d7),
            S(0x00f7), R(0x02c2,0x02c5), R(0x02d2,0x02df), R(0x02e5,0x02eb), S(0x02ed), R(0x02ef,0x02ff), S(0x0375),
            S(0x037e), R(0x0384,0x0385), S(0x0387), S(0x03f6), S(0x0482), R(0x055a,0x055f), R(0x0589,0x058a),
            R(0x058d,0x058f), S(0x05be), S(0x05c0), S(0x05c3), S(0x05c6), R(0x05f3,0x05f4), R(0x0606,0x060f),
            S(0x061b), R(0x061d,0x061f), R(0x066a,0x066d), S(0x06d4), S(0x06de), S(0x06e9), R(0x06fd,0x06fe),
            R(0x0700,0x070d), R(0x07f6,0x07f9), R(0x07fe,0x07ff), R(0x0830,0x083e), S(0x085e), S(0x0888),
            R(0x0964,0x0965), S(0x0970), R(0x09f2,0x09f3), R(0x09fa,0x09fb), S(0x09fd), S(0x0a76), R(0x0af0,0x0af1),
            S(0x0b70), R(0x0bf3,0x0bfa), S(0x0c77), S(0x0c7f), S(0x0c84), S(0x0d4f), S(0x0d79), S(0x0df4), S(0x0e3f),
            S(0x0e4f), R(0x0e5a,0x0e5b), R(0x0f01,0x0f17), R(0x0f1a,0x0f1f), S(0x0f34), S(0x0f36), S(0x0f38),
            R(0x0f3a,0x0f3d), S(0x0f85), R(0x0fbe,0x0fc5), R(0x0fc7,0x0fcc), R(0x0fce,0x0fda), R(0x104a,0x104f),
            R(0x109e,0x109f), S(0x10fb), R(0x1360,0x1368), R(0x1390,0x1399), S(0x1400), R(0x166d,0x166e),
            R(0x169b,0x169c), R(0x16eb,0x16ed), R(0x1735,0x1736), R(0x17d4,0x17d6), R(0x17d8,0x17db),
            R(0x1800,0x180a), S(0x1940), R(0x1944,0x1945), R(0x19de,0x19ff), R(0x1a1e,0x1a1f), R(0x1aa0,0x1aa6),
            R(0x1aa8,0x1aad), R(0x1b4e,0x1b4f), R(0x1b5a,0x1b6a), R(0x1b74,0x1b7f), R(0x1bfc,0x1bff),
            R(0x1c3b,0x1c3f), R(0x1c7e,0x1c7f), R(0x1cc0,0x1cc7), S(0x1cd3), S(0x1fbd), R(0x1fbf,0x1fc1),
            R(0x1fcd,0x1fcf), R(0x1fdd,0x1fdf), R(0x1fed,0x1fef), R(0x1ffd,0x1ffe), R(0x2010,0x2027),
            R(0x2030,0x205e), R(0x207a,0x207e), R(0x208a,0x208e), R(0x20a0,0x20c4), R(0x2100,0x2101),
            R(0x2103,0x2106), R(0x2108,0x2109), S(0x2114), R(0x2116,0x2118), R(0x211e,0x2123), S(0x2125), S(0x2127),
            S(0x2129), S(0x212e), R(0x213a,0x213b), R(0x2140,0x2144), R(0x214a,0x214d), S(0x214f), R(0x218a,0x218b),
            R(0x2190,0x2429), R(0x2440,0x244a), R(0x249c,0x24e9), R(0x2500,0x2775), R(0x2794,0x2b73),
            R(0x2b76,0x2bff), R(0x2ce5,0x2cea), R(0x2cf9,0x2cfc), R(0x2cfe,0x2cff), S(0x2d70), R(0x2e00,0x2e2e),
            R(0x2e30,0x2e5d), R(0x2e60,0x2e63), R(0x2e80,0x2e99), R(0x2e9b,0x2ef3), R(0x2f00,0x2fd5),
            R(0x2ff0,0x2fff), R(0x3001,0x3004), R(0x3008,0x3020), S(0x3030), R(0x3036,0x3037), R(0x303d,0x303f),
            R(0x309b,0x309c), S(0x30a0), S(0x30fb), R(0x3190,0x3191), R(0x3196,0x319f), R(0x31c0,0x31e5), S(0x31ef),
            R(0x3200,0x321e), R(0x322a,0x3247), S(0x3250), R(0x3260,0x327f), R(0x328a,0x32b0), R(0x32c0,0x33ff),
            R(0x4dc0,0x4dff), R(0xa490,0xa4c6), R(0xa4fe,0xa4ff), R(0xa60d,0xa60f), S(0xa673), S(0xa67e),
            R(0xa6f2,0xa6f7), R(0xa700,0xa716), R(0xa720,0xa721), R(0xa789,0xa78a), R(0xa828,0xa82b),
            R(0xa836,0xa839), R(0xa874,0xa877), R(0xa8ce,0xa8cf), R(0xa8f8,0xa8fa), S(0xa8fc), R(0xa92e,0xa92f),
            S(0xa95f), R(0xa9c1,0xa9cd), R(0xa9de,0xa9df), R(0xaa5c,0xaa5f), R(0xaa77,0xaa79), R(0xaade,0xaadf),
            R(0xaaf0,0xaaf1), S(0xab5b), R(0xab6a,0xab6b), S(0xabeb), S(0xfb29), R(0xfbb2,0xfbd2), R(0xfd3e,0xfd4f),
            R(0xfd90,0xfd91), R(0xfdc8,0xfdcf), R(0xfdfc,0xfdff), R(0xfe10,0xfe19), R(0xfe30,0xfe52),
            R(0xfe54,0xfe66), R(0xfe68,0xfe6b), R(0xff01,0xff0f), R(0xff1a,0xff20), R(0xff3b,0xff40),
            R(0xff5b,0xff65), R(0xffe0,0xffe6), R(0xffe8,0xffee), R(0xfffc,0xfffd), R(0x10100,0x10102),
            R(0x10137,0x1013f), R(0x10179,0x10189), R(0x1018c,0x1018e), R(0x10190,0x1019c), S(0x101a0),
            R(0x101d0,0x101fc), S(0x1039f), S(0x103d0), S(0x1056f), S(0x10857), R(0x10877,0x10878), S(0x1091f),
            S(0x1093f), R(0x10a50,0x10a58), S(0x10a7f), S(0x10ac8), R(0x10af0,0x10af6), R(0x10b39,0x10b3f),
            R(0x10b99,0x10b9c), S(0x10d6e), R(0x10d8e,0x10d8f), S(0x10ead), R(0x10ec9,0x10eca), R(0x10ed0,0x10ed8),
            R(0x10f55,0x10f59), R(0x10f86,0x10f89), R(0x11047,0x1104d), R(0x110bb,0x110bc), R(0x110be,0x110c1),
            R(0x11140,0x11143), R(0x11174,0x11175), R(0x111c5,0x111c8), S(0x111cd), S(0x111db), R(0x111dd,0x111df),
            R(0x11238,0x1123d), S(0x112a9), R(0x113d4,0x113d5), R(0x113d7,0x113d8), R(0x1144b,0x1144f),
            R(0x1145a,0x1145b), S(0x1145d), S(0x114c6), R(0x115c1,0x115d7), R(0x11641,0x11643), R(0x11660,0x1166c),
            S(0x116b9), R(0x1173c,0x1173f), S(0x1183b), R(0x11944,0x11946), S(0x119e2), R(0x11a3f,0x11a46),
            R(0x11a9a,0x11a9c), R(0x11a9e,0x11aa2), R(0x11b00,0x11b09), S(0x11be1), R(0x11c41,0x11c45),
            R(0x11c70,0x11c71), R(0x11ef7,0x11ef8), R(0x11f43,0x11f4f), R(0x11fd5,0x11ff1), S(0x11fff),
            R(0x12470,0x12474), R(0x12ff1,0x12ff2), R(0x16a6e,0x16a6f), S(0x16af5), R(0x16b37,0x16b3f),
            R(0x16b44,0x16b45), R(0x16d6d,0x16d6f), R(0x16e97,0x16e9a), S(0x16fe2), S(0x1bc9c), S(0x1bc9f),
            R(0x1cc00,0x1ccef), R(0x1ccfa,0x1ccfc), R(0x1cd00,0x1ceb3), R(0x1ceba,0x1ced0), R(0x1ced2,0x1ced4),
            R(0x1cedd,0x1cefd), R(0x1cf50,0x1cfc3), R(0x1d000,0x1d0f5), R(0x1d100,0x1d126), R(0x1d129,0x1d164),
            R(0x1d16a,0x1d16c), R(0x1d183,0x1d184), R(0x1d18c,0x1d1a9), R(0x1d1ae,0x1d241), S(0x1d245),
            R(0x1d253,0x1d25a), R(0x1d25d,0x1d25e), R(0x1d260,0x1d27f), R(0x1d300,0x1d356), S(0x1d6c1), S(0x1d6db),
            S(0x1d6fb), S(0x1d715), S(0x1d735), S(0x1d74f), S(0x1d76f), S(0x1d789), S(0x1d7a9), S(0x1d7c3),
            R(0x1d800,0x1d9ff), R(0x1da37,0x1da3a), R(0x1da6d,0x1da74), R(0x1da76,0x1da83), R(0x1da85,0x1da8b),
            R(0x1db00,0x1db1c), S(0x1e14f), S(0x1e2ff), S(0x1e5ff), R(0x1e95e,0x1e95f), S(0x1ecac), S(0x1ecb0),
            S(0x1ed2e), R(0x1eef0,0x1eef1), R(0x1f000,0x1f02b), R(0x1f030,0x1f093), R(0x1f0a0,0x1f0ae),
            R(0x1f0b1,0x1f0bf), R(0x1f0c1,0x1f0cf), R(0x1f0d1,0x1f0f5), R(0x1f10d,0x1f1ae), R(0x1f1e6,0x1f202),
            R(0x1f210,0x1f23b), R(0x1f240,0x1f248), R(0x1f250,0x1f251), R(0x1f260,0x1f265), R(0x1f300,0x1f6d9),
            R(0x1f6dc,0x1f6ec), R(0x1f6f0,0x1f6fc), R(0x1f700,0x1f7db), R(0x1f7e0,0x1f7eb), R(0x1f7f0,0x1f80b),
            R(0x1f810,0x1f847), R(0x1f850,0x1f859), R(0x1f860,0x1f887), R(0x1f890,0x1f8ad), R(0x1f8b0,0x1f8bb),
            R(0x1f8c0,0x1f8c1), R(0x1f8d0,0x1f8d8), R(0x1f900,0x1fa57), R(0x1fa60,0x1fa6d), R(0x1fa70,0x1fa7c),
            R(0x1fa80,0x1fac6), S(0x1fac8), R(0x1facc,0x1fadd), R(0x1fadf,0x1faeb), R(0x1faef,0x1fafa),
            R(0x1fb00,0x1fb92), R(0x1fb94,0x1fbef), S(0x1fbfa)
        };
#undef R
#undef S

        if(codepoint <= 0x7f)
            return ISPUNCT_(codepoint);

        return (md_unicode_bsearch__(codepoint, PUNCT_MAP, SIZEOF_ARRAY(PUNCT_MAP)) >= 0);
    }

    static void
    md_get_unicode_fold_info(unsigned codepoint, MD_UNICODE_FOLD_INFO* info)
    {
#define R(cp_min, cp_max)   ((cp_min) | 0x40000000), ((cp_max) | 0x80000000)
#define S(cp)               (cp)

        static const unsigned FOLD_MAP_1[] = {
            R(0x0041,0x005a), S(0x00b5), R(0x00c0,0x00d6), R(0x00d8,0x00de), R(0x0100,0x012e), R(0x0132,0x0136),
            R(0x0139,0x0147), R(0x014a,0x0176), S(0x0178), R(0x0179,0x017d), S(0x017f), S(0x0181), S(0x0182),
            S(0x0184), S(0x0186), S(0x0187), S(0x0189), S(0x018a), S(0x018b), S(0x018e), S(0x018f), S(0x0190),
            S(0x0191), S(0x0193), S(0x0194), S(0x0196), S(0x0197), S(0x0198), S(0x019c), S(0x019d), S(0x019f),
            R(0x01a0,0x01a4), S(0x01a6), S(0x01a7), S(0x01a9), S(0x01ac), S(0x01ae), S(0x01af), S(0x01b1), S(0x01b2),
            S(0x01b3), S(0x01b5), S(0x01b7), S(0x01b8), S(0x01bc), S(0x01c4), S(0x01c5), S(0x01c7), S(0x01c8),
            S(0x01ca), R(0x01cb,0x01db), R(0x01de,0x01ee), S(0x01f1), S(0x01f2), S(0x01f4), S(0x01f6), S(0x01f7),
            R(0x01f8,0x021e), S(0x0220), R(0x0222,0x0232), S(0x023a), S(0x023b), S(0x023d), S(0x023e), S(0x0241),
            S(0x0243), S(0x0244), S(0x0245), R(0x0246,0x024e), S(0x0345), S(0x0370), S(0x0372), S(0x0376), S(0x037f),
            S(0x0386), R(0x0388,0x038a), S(0x038c), S(0x038e), S(0x038f), R(0x0391,0x03a1), R(0x03a3,0x03ab),
            S(0x03c2), S(0x03cf), S(0x03d0), S(0x03d1), S(0x03d5), S(0x03d6), R(0x03d8,0x03ee), S(0x03f0), S(0x03f1),
            S(0x03f4), S(0x03f5), S(0x03f7), S(0x03f9), S(0x03fa), R(0x03fd,0x03ff), R(0x0400,0x040f),
            R(0x0410,0x042f), R(0x0460,0x0480), R(0x048a,0x04be), S(0x04c0), R(0x04c1,0x04cd), R(0x04d0,0x052e),
            R(0x0531,0x0556), R(0x10a0,0x10c5), S(0x10c7), S(0x10cd), R(0x13f8,0x13fd), S(0x1c80), S(0x1c81),
            S(0x1c82), S(0x1c83), S(0x1c84), S(0x1c85), S(0x1c86), S(0x1c87), S(0x1c88), S(0x1c89), R(0x1c90,0x1cba),
            R(0x1cbd,0x1cbf), R(0x1e00,0x1e94), S(0x1e9b), R(0x1ea0,0x1efe), R(0x1f08,0x1f0f), R(0x1f18,0x1f1d),
            R(0x1f28,0x1f2f), R(0x1f38,0x1f3f), R(0x1f48,0x1f4d), S(0x1f59), S(0x1f5b), S(0x1f5d), S(0x1f5f),
            R(0x1f68,0x1f6f), S(0x1fb8), S(0x1fb9), S(0x1fba), S(0x1fbb), S(0x1fbe), R(0x1fc8,0x1fcb), S(0x1fd8),
            S(0x1fd9), S(0x1fda), S(0x1fdb), S(0x1fe8), S(0x1fe9), S(0x1fea), S(0x1feb), S(0x1fec), S(0x1ff8),
            S(0x1ff9), S(0x1ffa), S(0x1ffb), S(0x2126), S(0x212a), S(0x212b), S(0x2132), R(0x2160,0x216f), S(0x2183),
            R(0x24b6,0x24cf), R(0x2c00,0x2c2f), S(0x2c60), S(0x2c62), S(0x2c63), S(0x2c64), R(0x2c67,0x2c6b),
            S(0x2c6d), S(0x2c6e), S(0x2c6f), S(0x2c70), S(0x2c72), S(0x2c75), S(0x2c7e), S(0x2c7f), R(0x2c80,0x2ce2),
            S(0x2ceb), S(0x2ced), S(0x2cf2), R(0xa640,0xa66c), R(0xa680,0xa69a), R(0xa722,0xa72e), R(0xa732,0xa76e),
            S(0xa779), S(0xa77b), S(0xa77d), R(0xa77e,0xa786), S(0xa78b), S(0xa78d), S(0xa790), S(0xa792),
            R(0xa796,0xa7a8), S(0xa7aa), S(0xa7ab), S(0xa7ac), S(0xa7ad), S(0xa7ae), S(0xa7b0), S(0xa7b1), S(0xa7b2),
            S(0xa7b3), R(0xa7b4,0xa7c2), S(0xa7c4), S(0xa7c5), S(0xa7c6), S(0xa7c7), S(0xa7c9), S(0xa7cb),
            R(0xa7cc,0xa7da), S(0xa7dc), S(0xa7dd), S(0xa7e2), S(0xa7f5), S(0xab6c), S(0xab6d), R(0xab70,0xabbf),
            R(0xff21,0xff3a), R(0x10400,0x10427), R(0x104b0,0x104d3), R(0x10570,0x1057a), R(0x1057c,0x1058a),
            R(0x1058c,0x10592), S(0x10594), S(0x10595), R(0x10c80,0x10cb2), R(0x10d50,0x10d65), R(0x118a0,0x118bf),
            R(0x16e40,0x16e5f), R(0x16ea0,0x16eb8), S(0x1df40), S(0x1df48), S(0x1df4a), S(0x1df4d), S(0x1df51),
            R(0x1df68,0x1df6e), R(0x1df72,0x1df7e), R(0x1e900,0x1e921)
        };
        static const unsigned FOLD_MAP_1_DATA[] = {
            0x0061, 0x007a, 0x03bc, 0x00e0, 0x00f6, 0x00f8, 0x00fe, 0x0101, 0x012f, 0x0133, 0x0137, 0x013a, 0x0148,
            0x014b, 0x0177, 0x00ff, 0x017a, 0x017e, 0x0073, 0x0253, 0x0183, 0x0185, 0x0254, 0x0188, 0x0256, 0x0257,
            0x018c, 0x01dd, 0x0259, 0x025b, 0x0192, 0x0260, 0x0263, 0x0269, 0x0268, 0x0199, 0x026f, 0x0272, 0x0275,
            0x01a1, 0x01a5, 0x0280, 0x01a8, 0x0283, 0x01ad, 0x0288, 0x01b0, 0x028a, 0x028b, 0x01b4, 0x01b6, 0x0292,
            0x01b9, 0x01bd, 0x01c6, 0x01c6, 0x01c9, 0x01c9, 0x01cc, 0x01cc, 0x01dc, 0x01df, 0x01ef, 0x01f3, 0x01f3,
            0x01f5, 0x0195, 0x01bf, 0x01f9, 0x021f, 0x019e, 0x0223, 0x0233, 0x2c65, 0x023c, 0x019a, 0x2c66, 0x0242,
            0x0180, 0x0289, 0x028c, 0x0247, 0x024f, 0x03b9, 0x0371, 0x0373, 0x0377, 0x03f3, 0x03ac, 0x03ad, 0x03af,
            0x03cc, 0x03cd, 0x03ce, 0x03b1, 0x03c1, 0x03c3, 0x03cb, 0x03c3, 0x03d7, 0x03b2, 0x03b8, 0x03c6, 0x03c0,
            0x03d9, 0x03ef, 0x03ba, 0x03c1, 0x03b8, 0x03b5, 0x03f8, 0x03f2, 0x03fb, 0x037b, 0x037d, 0x0450, 0x045f,
            0x0430, 0x044f, 0x0461, 0x0481, 0x048b, 0x04bf, 0x04cf, 0x04c2, 0x04ce, 0x04d1, 0x052f, 0x0561, 0x0586,
            0x2d00, 0x2d25, 0x2d27, 0x2d2d, 0x13f0, 0x13f5, 0x0432, 0x0434, 0x043e, 0x0441, 0x0442, 0x0442, 0x044a,
            0x0463, 0xa64b, 0x1c8a, 0x10d0, 0x10fa, 0x10fd, 0x10ff, 0x1e01, 0x1e95, 0x1e61, 0x1ea1, 0x1eff, 0x1f00,
            0x1f07, 0x1f10, 0x1f15, 0x1f20, 0x1f27, 0x1f30, 0x1f37, 0x1f40, 0x1f45, 0x1f51, 0x1f53, 0x1f55, 0x1f57,
            0x1f60, 0x1f67, 0x1fb0, 0x1fb1, 0x1f70, 0x1f71, 0x03b9, 0x1f72, 0x1f75, 0x1fd0, 0x1fd1, 0x1f76, 0x1f77,
            0x1fe0, 0x1fe1, 0x1f7a, 0x1f7b, 0x1fe5, 0x1f78, 0x1f79, 0x1f7c, 0x1f7d, 0x03c9, 0x006b, 0x00e5, 0x214e,
            0x2170, 0x217f, 0x2184, 0x24d0, 0x24e9, 0x2c30, 0x2c5f, 0x2c61, 0x026b, 0x1d7d, 0x027d, 0x2c68, 0x2c6c,
            0x0251, 0x0271, 0x0250, 0x0252, 0x2c73, 0x2c76, 0x023f, 0x0240, 0x2c81, 0x2ce3, 0x2cec, 0x2cee, 0x2cf3,
            0xa641, 0xa66d, 0xa681, 0xa69b, 0xa723, 0xa72f, 0xa733, 0xa76f, 0xa77a, 0xa77c, 0x1d79, 0xa77f, 0xa787,
            0xa78c, 0x0265, 0xa791, 0xa793, 0xa797, 0xa7a9, 0x0266, 0x025c, 0x0261, 0x026c, 0x026a, 0x029e, 0x0287,
            0x029d, 0xab53, 0xa7b5, 0xa7c3, 0xa794, 0x0282, 0x1d8e, 0xa7c8, 0xa7ca, 0x0264, 0xa7cd, 0xa7db, 0x019b,
            0x0277, 0x027c, 0xa7f6, 0xab4b, 0xab4c, 0x13a0, 0x13ef, 0xff41, 0xff5a, 0x10428, 0x1044f, 0x104d8,
            0x104fb, 0x10597, 0x105a1, 0x105a3, 0x105b1, 0x105b3, 0x105b9, 0x105bb, 0x105bc, 0x10cc0, 0x10cf2,
            0x10d70, 0x10d85, 0x118c0, 0x118df, 0x16e60, 0x16e7f, 0x16ebb, 0x16ed3, 0x1df41, 0x1df49, 0x1df4b,
            0x1df4e, 0x1df52, 0x1df69, 0x1df6f, 0x1df73, 0x1df7f, 0x1e922, 0x1e943
        };
        static const unsigned FOLD_MAP_2[] = {
            S(0x00df), S(0x0130), S(0x0149), S(0x01f0), S(0x0587), S(0x1e96), S(0x1e97), S(0x1e98), S(0x1e99),
            S(0x1e9a), S(0x1e9e), S(0x1f50), R(0x1f80,0x1f87), R(0x1f88,0x1f8f), R(0x1f90,0x1f97), R(0x1f98,0x1f9f),
            R(0x1fa0,0x1fa7), R(0x1fa8,0x1faf), S(0x1fb2), S(0x1fb3), S(0x1fb4), S(0x1fb6), S(0x1fbc), S(0x1fc2),
            S(0x1fc3), S(0x1fc4), S(0x1fc6), S(0x1fcc), S(0x1fd6), S(0x1fe4), S(0x1fe6), S(0x1ff2), S(0x1ff3),
            S(0x1ff4), S(0x1ff6), S(0x1ffc), S(0xfb00), S(0xfb01), S(0xfb02), S(0xfb05), S(0xfb06), S(0xfb13),
            S(0xfb14), S(0xfb15), S(0xfb16), S(0xfb17), S(0x1df95)
        };
        static const unsigned FOLD_MAP_2_DATA[] = {
            0x0073,0x0073, 0x0069,0x0307, 0x02bc,0x006e, 0x006a,0x030c, 0x0565,0x0582, 0x0068,0x0331, 0x0074,0x0308,
            0x0077,0x030a, 0x0079,0x030a, 0x0061,0x02be, 0x0073,0x0073, 0x03c5,0x0313, 0x1f00,0x03b9, 0x1f07,0x03b9,
            0x1f00,0x03b9, 0x1f07,0x03b9, 0x1f20,0x03b9, 0x1f27,0x03b9, 0x1f20,0x03b9, 0x1f27,0x03b9, 0x1f60,0x03b9,
            0x1f67,0x03b9, 0x1f60,0x03b9, 0x1f67,0x03b9, 0x1f70,0x03b9, 0x03b1,0x03b9, 0x03ac,0x03b9, 0x03b1,0x0342,
            0x03b1,0x03b9, 0x1f74,0x03b9, 0x03b7,0x03b9, 0x03ae,0x03b9, 0x03b7,0x0342, 0x03b7,0x03b9, 0x03b9,0x0342,
            0x03c1,0x0313, 0x03c5,0x0342, 0x1f7c,0x03b9, 0x03c9,0x03b9, 0x03ce,0x03b9, 0x03c9,0x0342, 0x03c9,0x03b9,
            0x0066,0x0066, 0x0066,0x0069, 0x0066,0x006c, 0x0073,0x0074, 0x0073,0x0074, 0x0574,0x0576, 0x0574,0x0565,
            0x0574,0x056b, 0x057e,0x0576, 0x0574,0x056d, 0x0073,0x0073
        };
        static const unsigned FOLD_MAP_3[] = {
            S(0x0390), S(0x03b0), S(0x1f52), S(0x1f54), S(0x1f56), S(0x1fb7), S(0x1fc7), S(0x1fd2), S(0x1fd3),
            S(0x1fd7), S(0x1fe2), S(0x1fe3), S(0x1fe7), S(0x1ff7), S(0xfb03), S(0xfb04)
        };
        static const unsigned FOLD_MAP_3_DATA[] = {
            0x03b9,0x0308,0x0301, 0x03c5,0x0308,0x0301, 0x03c5,0x0313,0x0300, 0x03c5,0x0313,0x0301,
            0x03c5,0x0313,0x0342, 0x03b1,0x0342,0x03b9, 0x03b7,0x0342,0x03b9, 0x03b9,0x0308,0x0300,
            0x03b9,0x0308,0x0301, 0x03b9,0x0308,0x0342, 0x03c5,0x0308,0x0300, 0x03c5,0x0308,0x0301,
            0x03c5,0x0308,0x0342, 0x03c9,0x0342,0x03b9, 0x0066,0x0066,0x0069, 0x0066,0x0066,0x006c
        };
#undef R
#undef S
        static const struct {
            const unsigned* map;
            const unsigned* data;
            size_t map_size;
            unsigned n_codepoints;
        } FOLD_MAP_LIST[] = {
            { FOLD_MAP_1, FOLD_MAP_1_DATA, SIZEOF_ARRAY(FOLD_MAP_1), 1 },
            { FOLD_MAP_2, FOLD_MAP_2_DATA, SIZEOF_ARRAY(FOLD_MAP_2), 2 },
            { FOLD_MAP_3, FOLD_MAP_3_DATA, SIZEOF_ARRAY(FOLD_MAP_3), 3 }
        };

        int i;

        if(codepoint <= 0x7f) {
            info->codepoints[0] = codepoint;
            if(ISUPPER_(codepoint))
                info->codepoints[0] += 'a' - 'A';
            info->n_codepoints = 1;
            return;
        }

        for(i = 0; i < (int) SIZEOF_ARRAY(FOLD_MAP_LIST); i++) {
            int index;

            index = md_unicode_bsearch__(codepoint, FOLD_MAP_LIST[i].map, FOLD_MAP_LIST[i].map_size);
            if(index >= 0) {

                unsigned n_codepoints = FOLD_MAP_LIST[i].n_codepoints;
                const unsigned* map = FOLD_MAP_LIST[i].map;
                const unsigned* codepoints = FOLD_MAP_LIST[i].data + (index * n_codepoints);

                memcpy(info->codepoints, codepoints, sizeof(unsigned) * n_codepoints);
                info->n_codepoints = n_codepoints;

                if(FOLD_MAP_LIST[i].map[index] != codepoint) {

                    if((map[index] & 0x00ffffff)+1 == codepoints[0]) {

                        info->codepoints[0] = codepoint + ((codepoint & 0x1) == (map[index] & 0x1) ? 1 : 0);
                    } else {

                        info->codepoints[0] += (codepoint - (map[index] & 0x00ffffff));
                    }
                }

                return;
            }
        }

        info->codepoints[0] = codepoint;
        info->n_codepoints = 1;
    }
#endif

#if defined MD4C_USE_UTF16
    #define IS_UTF16_SURROGATE_HI(word)     (((WORD)(word) & 0xfc00) == 0xd800)
    #define IS_UTF16_SURROGATE_LO(word)     (((WORD)(word) & 0xfc00) == 0xdc00)
    #define UTF16_DECODE_SURROGATE(hi, lo)  (0x10000 + ((((unsigned)(hi) & 0x3ff) << 10) | (((unsigned)(lo) & 0x3ff) << 0)))

    static unsigned
    md_decode_utf16le__(const CHAR* str, SZ str_size, SZ* p_size)
    {
        if(IS_UTF16_SURROGATE_HI(str[0])) {
            if(1 < str_size && IS_UTF16_SURROGATE_LO(str[1])) {
                if(p_size != NULL)
                    *p_size = 2;
                return UTF16_DECODE_SURROGATE(str[0], str[1]);
            }
        }

        if(p_size != NULL)
            *p_size = 1;
        return str[0];
    }

    static unsigned
    md_decode_utf16le_before__(MD_CTX* ctx, OFF off)
    {
        if(off > 2 && IS_UTF16_SURROGATE_HI(CH(off-2)) && IS_UTF16_SURROGATE_LO(CH(off-1)))
            return UTF16_DECODE_SURROGATE(CH(off-2), CH(off-1));

        return CH(off-1);
    }

    #define ISUNICODEWHITESPACE_(codepoint) md_is_unicode_whitespace__(codepoint)
    #define ISUNICODEWHITESPACE(off)        md_is_unicode_whitespace__(CH(off))
    #define ISUNICODEWHITESPACEBEFORE(off)  md_is_unicode_whitespace__(CH((off)-1))

    #define ISUNICODEPUNCT(off)             md_is_unicode_punct__(md_decode_utf16le__(STR(off), ctx->size - (off), NULL))
    #define ISUNICODEPUNCTBEFORE(off)       md_is_unicode_punct__(md_decode_utf16le_before__(ctx, off))

    static inline int
    md_decode_unicode(const CHAR* str, OFF off, SZ str_size, SZ* p_char_size)
    {
        return md_decode_utf16le__(str+off, str_size-off, p_char_size);
    }
#elif defined MD4C_USE_UTF8
    #define IS_UTF8_LEAD1(byte)     ((unsigned char)(byte) <= 0x7f)
    #define IS_UTF8_LEAD2(byte)     (((unsigned char)(byte) & 0xe0) == 0xc0)
    #define IS_UTF8_LEAD3(byte)     (((unsigned char)(byte) & 0xf0) == 0xe0)
    #define IS_UTF8_LEAD4(byte)     (((unsigned char)(byte) & 0xf8) == 0xf0)
    #define IS_UTF8_TAIL(byte)      (((unsigned char)(byte) & 0xc0) == 0x80)

    static unsigned
    md_decode_utf8__(const CHAR* str, SZ str_size, SZ* p_size)
    {
        if(!IS_UTF8_LEAD1(str[0])) {
            if(IS_UTF8_LEAD2(str[0])) {
                if(1 < str_size && IS_UTF8_TAIL(str[1])) {
                    if(p_size != NULL)
                        *p_size = 2;

                    return (((unsigned int)str[0] & 0x1f) << 6) |
                           (((unsigned int)str[1] & 0x3f) << 0);
                }
            } else if(IS_UTF8_LEAD3(str[0])) {
                if(2 < str_size && IS_UTF8_TAIL(str[1]) && IS_UTF8_TAIL(str[2])) {
                    if(p_size != NULL)
                        *p_size = 3;

                    return (((unsigned int)str[0] & 0x0f) << 12) |
                           (((unsigned int)str[1] & 0x3f) << 6) |
                           (((unsigned int)str[2] & 0x3f) << 0);
                }
            } else if(IS_UTF8_LEAD4(str[0])) {
                if(3 < str_size && IS_UTF8_TAIL(str[1]) && IS_UTF8_TAIL(str[2]) && IS_UTF8_TAIL(str[3])) {
                    if(p_size != NULL)
                        *p_size = 4;

                    return (((unsigned int)str[0] & 0x07) << 18) |
                           (((unsigned int)str[1] & 0x3f) << 12) |
                           (((unsigned int)str[2] & 0x3f) << 6) |
                           (((unsigned int)str[3] & 0x3f) << 0);
                }
            }
        }

        if(p_size != NULL)
            *p_size = 1;
        return (unsigned) str[0];
    }

    static unsigned
    md_decode_utf8_before__(MD_CTX* ctx, OFF off)
    {
        if(!IS_UTF8_LEAD1(CH(off-1))) {
            if(off > 1 && IS_UTF8_LEAD2(CH(off-2)) && IS_UTF8_TAIL(CH(off-1)))
                return (((unsigned int)CH(off-2) & 0x1f) << 6) |
                       (((unsigned int)CH(off-1) & 0x3f) << 0);

            if(off > 2 && IS_UTF8_LEAD3(CH(off-3)) && IS_UTF8_TAIL(CH(off-2)) && IS_UTF8_TAIL(CH(off-1)))
                return (((unsigned int)CH(off-3) & 0x0f) << 12) |
                       (((unsigned int)CH(off-2) & 0x3f) << 6) |
                       (((unsigned int)CH(off-1) & 0x3f) << 0);

            if(off > 3 && IS_UTF8_LEAD4(CH(off-4)) && IS_UTF8_TAIL(CH(off-3)) && IS_UTF8_TAIL(CH(off-2)) && IS_UTF8_TAIL(CH(off-1)))
                return (((unsigned int)CH(off-4) & 0x07) << 18) |
                       (((unsigned int)CH(off-3) & 0x3f) << 12) |
                       (((unsigned int)CH(off-2) & 0x3f) << 6) |
                       (((unsigned int)CH(off-1) & 0x3f) << 0);
        }

        return (unsigned) CH(off-1);
    }

    #define ISUNICODEWHITESPACE_(codepoint) md_is_unicode_whitespace__(codepoint)
    #define ISUNICODEWHITESPACE(off)        md_is_unicode_whitespace__(md_decode_utf8__(STR(off), ctx->size - (off), NULL))
    #define ISUNICODEWHITESPACEBEFORE(off)  md_is_unicode_whitespace__(md_decode_utf8_before__(ctx, off))

    #define ISUNICODEPUNCT(off)             md_is_unicode_punct__(md_decode_utf8__(STR(off), ctx->size - (off), NULL))
    #define ISUNICODEPUNCTBEFORE(off)       md_is_unicode_punct__(md_decode_utf8_before__(ctx, off))

    static inline unsigned
    md_decode_unicode(const CHAR* str, OFF off, SZ str_size, SZ* p_char_size)
    {
        return md_decode_utf8__(str+off, str_size-off, p_char_size);
    }
#else
    #define ISUNICODEWHITESPACE_(codepoint) ISWHITESPACE_(codepoint)
    #define ISUNICODEWHITESPACE(off)        ISWHITESPACE(off)
    #define ISUNICODEWHITESPACEBEFORE(off)  ISWHITESPACE((off)-1)

    #define ISUNICODEPUNCT(off)             ISPUNCT(off)
    #define ISUNICODEPUNCTBEFORE(off)       ISPUNCT((off)-1)

    static inline void
    md_get_unicode_fold_info(unsigned codepoint, MD_UNICODE_FOLD_INFO* info)
    {
        info->codepoints[0] = codepoint;
        if(ISUPPER_(codepoint))
            info->codepoints[0] += 'a' - 'A';
        info->n_codepoints = 1;
    }

    static inline unsigned
    md_decode_unicode(const CHAR* str, OFF off, SZ str_size, SZ* p_size)
    {
        *p_size = 1;
        return (unsigned) str[off];
    }
#endif

static void
md_merge_lines(MD_CTX* ctx, OFF beg, OFF end, const MD_LINE* lines, MD_SIZE n_lines,
               CHAR line_break_replacement_char, CHAR* buffer, SZ* p_size)
{
    CHAR* ptr = buffer;
    int line_index = 0;
    OFF off = beg;

    MD_UNUSED(n_lines);

    while(1) {
        const MD_LINE* line = &lines[line_index];
        OFF line_end = line->end;
        if(end < line_end)
            line_end = end;

        while(off < line_end) {
            *ptr = CH(off);
            ptr++;
            off++;
        }

        if(off >= end) {
            *p_size = (MD_SIZE)(ptr - buffer);
            return;
        }

        *ptr = line_break_replacement_char;
        ptr++;

        line_index++;
        off = lines[line_index].beg;
    }
}

static int
md_merge_lines_alloc(MD_CTX* ctx, OFF beg, OFF end, const MD_LINE* lines, MD_SIZE n_lines,
                    CHAR line_break_replacement_char, CHAR** p_str, SZ* p_size)
{
    CHAR* buffer;

    buffer = (CHAR*) malloc(sizeof(CHAR) * (end - beg));
    if(buffer == NULL) {
        MD_LOG("malloc() failed.");
        return -1;
    }

    md_merge_lines(ctx, beg, end, lines, n_lines,
                line_break_replacement_char, buffer, p_size);

    *p_str = buffer;
    return 0;
}

static OFF
md_skip_unicode_whitespace(const CHAR* label, OFF off, SZ size)
{
    SZ char_size;
    unsigned codepoint;

    while(off < size) {
        codepoint = md_decode_unicode(label, off, size, &char_size);
        if(!ISUNICODEWHITESPACE_(codepoint)  &&  !ISNEWLINE_(label[off]))
            break;
        off += char_size;
    }

    return off;
}

static int
md_is_html_tag(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines, OFF beg, OFF max_end, OFF* p_end)
{
    int attr_state;
    OFF off = beg;
    OFF line_end = (n_lines > 0) ? lines[0].end : ctx->size;
    MD_SIZE line_index = 0;

    MD_ASSERT(CH(beg) == _T('<'));

    if(off + 1 >= line_end)
        return FALSE;
    off++;

    attr_state = 0;

    if(CH(off) == _T('/')) {

        attr_state = -1;
        off++;
    }

    if(off >= line_end  ||  !ISALPHA(off))
        return FALSE;
    off++;
    while(off < line_end  &&  (ISALNUM(off)  ||  CH(off) == _T('-')))
        off++;

    while(1) {
        while(off < line_end  &&  !ISNEWLINE(off)) {
            if(attr_state > 40) {
                if(attr_state == 41 && (ISBLANK(off) || ISANYOF(off, _T("\"'=<>`")))) {
                    attr_state = 0;
                    off--;
                } else if(attr_state == 42 && CH(off) == _T('\'')) {
                    attr_state = 0;
                } else if(attr_state == 43 && CH(off) == _T('"')) {
                    attr_state = 0;
                }
                off++;
            } else if(ISWHITESPACE(off)) {
                if(attr_state == 0)
                    attr_state = 1;
                off++;
            } else if(attr_state <= 2 && CH(off) == _T('>')) {

                goto done;
            } else if(attr_state <= 2 && CH(off) == _T('/') && off+1 < line_end && CH(off+1) == _T('>')) {

                off++;
                goto done;
            } else if((attr_state == 1 || attr_state == 2) && (ISALPHA(off) || CH(off) == _T('_') || CH(off) == _T(':'))) {
                off++;

                while(off < line_end && (ISALNUM(off) || ISANYOF(off, _T("_.:-"))))
                    off++;
                attr_state = 2;
            } else if(attr_state == 2 && CH(off) == _T('=')) {

                off++;
                attr_state = 3;
            } else if(attr_state == 3) {

                if(CH(off) == _T('"'))
                    attr_state = 43;
                else if(CH(off) == _T('\''))
                    attr_state = 42;
                else if(!ISANYOF(off, _T("\"'=<>`"))  &&  !ISNEWLINE(off))
                    attr_state = 41;
                else
                    return FALSE;
                off++;
            } else {

                return FALSE;
            }
        }

        if(n_lines == 0)
            return FALSE;

        line_index++;
        if(line_index >= n_lines)
            return FALSE;

        off = lines[line_index].beg;
        line_end = lines[line_index].end;

        if(attr_state == 0  ||  attr_state == 41)
            attr_state = 1;

        if(off >= max_end)
            return FALSE;
    }

done:
    if(off >= max_end)
        return FALSE;

    *p_end = off+1;
    return TRUE;
}

static int
md_scan_for_html_closer(MD_CTX* ctx, const MD_CHAR* str, MD_SIZE len,
                        const MD_LINE* lines, MD_SIZE n_lines,
                        OFF beg, OFF max_end, OFF* p_end,
                        OFF* p_scan_horizon)
{
    OFF off = beg;
    MD_SIZE line_index = 0;

    if(off < *p_scan_horizon  &&  *p_scan_horizon >= max_end - len) {

        return FALSE;
    }

    while(TRUE) {
        while(off + len <= lines[line_index].end  &&  off + len <= max_end) {
            if(md_ascii_eq(STR(off), str, len)) {

                *p_end = off + len;
                return TRUE;
            }
            off++;
        }

        line_index++;
        if(off >= max_end  ||  line_index >= n_lines) {

            *p_scan_horizon = off;
            return FALSE;
        }

        off = lines[line_index].beg;
    }
}

static int
md_is_html_comment(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines, OFF beg, OFF max_end, OFF* p_end)
{
    OFF off = beg;

    MD_ASSERT(CH(beg) == _T('<'));

    if(off + 4 >= lines[0].end)
        return FALSE;
    if(CH(off+1) != _T('!')  ||  CH(off+2) != _T('-')  ||  CH(off+3) != _T('-'))
        return FALSE;

    off += 2;

    return md_scan_for_html_closer(ctx, _T("-->"), 3,
                lines, n_lines, off, max_end, p_end, &ctx->html_comment_horizon);
}

static int
md_is_html_processing_instruction(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines, OFF beg, OFF max_end, OFF* p_end)
{
    OFF off = beg;

    if(off + 2 >= lines[0].end)
        return FALSE;
    if(CH(off+1) != _T('?'))
        return FALSE;
    off += 2;

    return md_scan_for_html_closer(ctx, _T("?>"), 2,
                lines, n_lines, off, max_end, p_end, &ctx->html_proc_instr_horizon);
}

static int
md_is_html_declaration(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines, OFF beg, OFF max_end, OFF* p_end)
{
    OFF off = beg;

    if(off + 2 >= lines[0].end)
        return FALSE;
    if(CH(off+1) != _T('!'))
        return FALSE;
    off += 2;

    if(off >= lines[0].end  ||  !ISALPHA(off))
        return FALSE;
    off++;
    while(off < lines[0].end  &&  ISALPHA(off))
        off++;

    return md_scan_for_html_closer(ctx, _T(">"), 1,
                lines, n_lines, off, max_end, p_end, &ctx->html_decl_horizon);
}

static int
md_is_html_cdata(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines, OFF beg, OFF max_end, OFF* p_end)
{
    static const CHAR open_str[] = _T("<![CDATA[");
    static const SZ open_size = SIZEOF_ARRAY(open_str) - 1;

    OFF off = beg;

    if(off + open_size >= lines[0].end)
        return FALSE;
    if(memcmp(STR(off), open_str, open_size) != 0)
        return FALSE;
    off += open_size;

    return md_scan_for_html_closer(ctx, _T("]]>"), 3,
                lines, n_lines, off, max_end, p_end, &ctx->html_cdata_horizon);
}

static int
md_is_html_any(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines, OFF beg, OFF max_end, OFF* p_end)
{
    MD_ASSERT(CH(beg) == _T('<'));
    return (md_is_html_tag(ctx, lines, n_lines, beg, max_end, p_end)  ||
            md_is_html_comment(ctx, lines, n_lines, beg, max_end, p_end)  ||
            md_is_html_processing_instruction(ctx, lines, n_lines, beg, max_end, p_end)  ||
            md_is_html_declaration(ctx, lines, n_lines, beg, max_end, p_end)  ||
            md_is_html_cdata(ctx, lines, n_lines, beg, max_end, p_end));
}

static int
md_is_hex_entity_contents(MD_CTX* ctx, const CHAR* text, OFF beg, OFF max_end, OFF* p_end)
{
    OFF off = beg;
    MD_UNUSED(ctx);

    while(off < max_end  &&  ISXDIGIT_(text[off])  &&  off - beg <= 8)
        off++;

    if(1 <= off - beg  &&  off - beg <= 6) {
        *p_end = off;
        return TRUE;
    } else {
        return FALSE;
    }
}

static int
md_is_dec_entity_contents(MD_CTX* ctx, const CHAR* text, OFF beg, OFF max_end, OFF* p_end)
{
    OFF off = beg;
    MD_UNUSED(ctx);

    while(off < max_end  &&  ISDIGIT_(text[off])  &&  off - beg <= 8)
        off++;

    if(1 <= off - beg  &&  off - beg <= 7) {
        *p_end = off;
        return TRUE;
    } else {
        return FALSE;
    }
}

static int
md_is_named_entity_contents(MD_CTX* ctx, const CHAR* text, OFF beg, OFF max_end, OFF* p_end)
{
    OFF off = beg;
    MD_UNUSED(ctx);

    if(off < max_end  &&  ISALPHA_(text[off]))
        off++;
    else
        return FALSE;

    while(off < max_end  &&  ISALNUM_(text[off])  &&  off - beg <= 48)
        off++;

    if(2 <= off - beg  &&  off - beg <= 48) {
        *p_end = off;
        return TRUE;
    } else {
        return FALSE;
    }
}

static int
md_is_entity_str(MD_CTX* ctx, const CHAR* text, OFF beg, OFF max_end, OFF* p_end)
{
    int is_contents;
    OFF off = beg;

    MD_ASSERT(text[off] == _T('&'));
    off++;

    if(off+2 < max_end  &&  text[off] == _T('#')  &&  (text[off+1] == _T('x') || text[off+1] == _T('X')))
        is_contents = md_is_hex_entity_contents(ctx, text, off+2, max_end, &off);
    else if(off+1 < max_end  &&  text[off] == _T('#'))
        is_contents = md_is_dec_entity_contents(ctx, text, off+1, max_end, &off);
    else
        is_contents = md_is_named_entity_contents(ctx, text, off, max_end, &off);

    if(is_contents  &&  off < max_end  &&  text[off] == _T(';')) {
        *p_end = off+1;
        return TRUE;
    } else {
        return FALSE;
    }
}

static inline int
md_is_entity(MD_CTX* ctx, OFF beg, OFF max_end, OFF* p_end)
{
    return md_is_entity_str(ctx, ctx->text, beg, max_end, p_end);
}

typedef struct MD_ATTRIBUTE_BUILD_tag MD_ATTRIBUTE_BUILD;
struct MD_ATTRIBUTE_BUILD_tag {
    CHAR* text;
    MD_TEXTTYPE* substr_types;
    OFF* substr_offsets;
    int substr_count;
    int substr_alloc;
    MD_TEXTTYPE trivial_types[1];
    OFF trivial_offsets[2];
};

#define MD_BUILD_ATTR_NO_ESCAPES    0x0001

static int
md_build_attr_append_substr(MD_CTX* ctx, MD_ATTRIBUTE_BUILD* build,
                            MD_TEXTTYPE type, OFF off)
{
    if(build->substr_count >= build->substr_alloc) {
        MD_TEXTTYPE* new_substr_types;
        OFF* new_substr_offsets;

        build->substr_alloc = (build->substr_alloc > 0
                ? build->substr_alloc + build->substr_alloc / 2
                : 8);
        new_substr_types = (MD_TEXTTYPE*) realloc(build->substr_types,
                                    build->substr_alloc * sizeof(MD_TEXTTYPE));
        if(new_substr_types == NULL) {
            MD_LOG("realloc() failed.");
            return -1;
        }

        new_substr_offsets = (OFF*) realloc(build->substr_offsets,
                                    (build->substr_alloc+1) * sizeof(OFF));
        if(new_substr_offsets == NULL) {
            MD_LOG("realloc() failed.");
            free(new_substr_types);
            return -1;
        }

        build->substr_types = new_substr_types;
        build->substr_offsets = new_substr_offsets;
    }

    build->substr_types[build->substr_count] = type;
    build->substr_offsets[build->substr_count] = off;
    build->substr_count++;
    return 0;
}

static void
md_free_attribute(MD_CTX* ctx, MD_ATTRIBUTE_BUILD* build)
{
    MD_UNUSED(ctx);

    if(build->substr_alloc > 0) {
        free(build->text);
        free(build->substr_types);
        free(build->substr_offsets);
    }
}

static int
md_build_attribute(MD_CTX* ctx, const CHAR* raw_text, SZ raw_size,
                   unsigned flags, MD_ATTRIBUTE* attr, MD_ATTRIBUTE_BUILD* build)
{
    OFF raw_off, off;
    int is_trivial;
    int ret = 0;

    memset(build, 0, sizeof(MD_ATTRIBUTE_BUILD));

    is_trivial = TRUE;
    for(raw_off = 0; raw_off < raw_size; raw_off++) {
        if(ISANYOF3_(raw_text[raw_off], _T('\\'), _T('&'), _T('\0'))) {
            is_trivial = FALSE;
            break;
        }
    }

    if(is_trivial) {
        build->text = (CHAR*) (raw_size ? raw_text : NULL);
        build->substr_types = build->trivial_types;
        build->substr_offsets = build->trivial_offsets;
        build->substr_count = 1;
        build->substr_alloc = 0;
        build->trivial_types[0] = MD_TEXT_NORMAL;
        build->trivial_offsets[0] = 0;
        build->trivial_offsets[1] = raw_size;
        off = raw_size;
    } else {
        build->text = (CHAR*) malloc(raw_size * sizeof(CHAR));
        if(build->text == NULL) {
            MD_LOG("malloc() failed.");
            goto abort;
        }

        raw_off = 0;
        off = 0;

        while(raw_off < raw_size) {
            if(raw_text[raw_off] == _T('\0')) {
                MD_CHECK(md_build_attr_append_substr(ctx, build, MD_TEXT_NULLCHAR, off));
                memcpy(build->text + off, raw_text + raw_off, 1);
                off++;
                raw_off++;
                continue;
            }

            if(raw_text[raw_off] == _T('&')) {
                OFF ent_end;

                if(md_is_entity_str(ctx, raw_text, raw_off, raw_size, &ent_end)) {
                    MD_CHECK(md_build_attr_append_substr(ctx, build, MD_TEXT_ENTITY, off));
                    memcpy(build->text + off, raw_text + raw_off, ent_end - raw_off);
                    off += ent_end - raw_off;
                    raw_off = ent_end;
                    continue;
                }
            }

            if(build->substr_count == 0  ||  build->substr_types[build->substr_count-1] != MD_TEXT_NORMAL)
                MD_CHECK(md_build_attr_append_substr(ctx, build, MD_TEXT_NORMAL, off));

            if(!(flags & MD_BUILD_ATTR_NO_ESCAPES)  &&
               raw_text[raw_off] == _T('\\')  &&  raw_off+1 < raw_size  &&
               (ISPUNCT_(raw_text[raw_off+1]) || ISNEWLINE_(raw_text[raw_off+1])))
                raw_off++;

            build->text[off++] = raw_text[raw_off++];
        }
        build->substr_offsets[build->substr_count] = off;
    }

    attr->text = build->text;
    attr->size = off;
    attr->substr_offsets = build->substr_offsets;
    attr->substr_types = build->substr_types;
    return 0;

abort:
    md_free_attribute(ctx, build);
    return -1;
}

#define MD_FNV1A_BASE       2166136261U
#define MD_FNV1A_PRIME      16777619U

static inline unsigned
md_fnv1a(unsigned base, const void* data, size_t n)
{
    const unsigned char* buf = (const unsigned char*) data;
    unsigned hash = base;
    size_t i;

    for(i = 0; i < n; i++) {
        hash ^= buf[i];
        hash *= MD_FNV1A_PRIME;
    }

    return hash;
}

struct MD_REF_DEF_tag {
    CHAR* label;
    CHAR* title;
    unsigned hash;
    SZ label_size;
    SZ title_size;
    OFF dest_beg;
    OFF dest_end;
    unsigned char label_needs_free : 1;
    unsigned char title_needs_free : 1;
};

static unsigned
md_link_label_hash(const CHAR* label, SZ size)
{
    unsigned hash = MD_FNV1A_BASE;
    OFF off;
    unsigned codepoint;
    int is_whitespace = FALSE;

    off = md_skip_unicode_whitespace(label, 0, size);
    while(off < size) {
        SZ char_size;

        codepoint = md_decode_unicode(label, off, size, &char_size);
        is_whitespace = ISUNICODEWHITESPACE_(codepoint) || ISNEWLINE_(label[off]);

        if(is_whitespace) {
            codepoint = ' ';
            hash = md_fnv1a(hash, &codepoint, sizeof(unsigned));
            off = md_skip_unicode_whitespace(label, off, size);
        } else {
            MD_UNICODE_FOLD_INFO fold_info;

            md_get_unicode_fold_info(codepoint, &fold_info);
            hash = md_fnv1a(hash, fold_info.codepoints, fold_info.n_codepoints * sizeof(unsigned));
            off += char_size;
        }
    }

    return hash;
}

static OFF
md_link_label_cmp_load_fold_info(const CHAR* label, OFF off, SZ size,
                                 MD_UNICODE_FOLD_INFO* fold_info)
{
    unsigned codepoint;
    SZ char_size;

    if(off >= size) {

        goto whitespace;
    }

    codepoint = md_decode_unicode(label, off, size, &char_size);
    off += char_size;
    if(ISUNICODEWHITESPACE_(codepoint)) {

        goto whitespace;
    }

    md_get_unicode_fold_info(codepoint, fold_info);
    return off;

whitespace:
    fold_info->codepoints[0] = _T(' ');
    fold_info->n_codepoints = 1;
    return md_skip_unicode_whitespace(label, off, size);
}

static int
md_link_label_cmp(const CHAR* a_label, SZ a_size, const CHAR* b_label, SZ b_size)
{
    OFF a_off;
    OFF b_off;
    MD_UNICODE_FOLD_INFO a_fi = { { 0 }, 0 };
    MD_UNICODE_FOLD_INFO b_fi = { { 0 }, 0 };
    OFF a_fi_off = 0;
    OFF b_fi_off = 0;
    int cmp;

    a_off = md_skip_unicode_whitespace(a_label, 0, a_size);
    b_off = md_skip_unicode_whitespace(b_label, 0, b_size);
    while(a_off < a_size || a_fi_off < a_fi.n_codepoints ||
          b_off < b_size || b_fi_off < b_fi.n_codepoints)
    {

        if(a_fi_off >= a_fi.n_codepoints) {
            a_fi_off = 0;
            a_off = md_link_label_cmp_load_fold_info(a_label, a_off, a_size, &a_fi);
        }
        if(b_fi_off >= b_fi.n_codepoints) {
            b_fi_off = 0;
            b_off = md_link_label_cmp_load_fold_info(b_label, b_off, b_size, &b_fi);
        }

        cmp = b_fi.codepoints[b_fi_off] - a_fi.codepoints[a_fi_off];
        if(cmp != 0)
            return cmp;

        a_fi_off++;
        b_fi_off++;
    }

    return 0;
}

typedef struct MD_REF_DEF_LIST_tag MD_REF_DEF_LIST;
struct MD_REF_DEF_LIST_tag {
    int n_ref_defs;
    int alloc_ref_defs;
    MD_REF_DEF* ref_defs[];
};

static int
md_ref_def_cmp(const void* a, const void* b)
{
    const MD_REF_DEF* a_ref = *(const MD_REF_DEF**)a;
    const MD_REF_DEF* b_ref = *(const MD_REF_DEF**)b;

    if(a_ref->hash < b_ref->hash)
        return -1;
    else if(a_ref->hash > b_ref->hash)
        return +1;
    else
        return md_link_label_cmp(a_ref->label, a_ref->label_size, b_ref->label, b_ref->label_size);
}

static int
md_ref_def_cmp_for_sort(const void* a, const void* b)
{
    int cmp;

    cmp = md_ref_def_cmp(a, b);

    if(cmp == 0) {
        const MD_REF_DEF* a_ref = *(const MD_REF_DEF**)a;
        const MD_REF_DEF* b_ref = *(const MD_REF_DEF**)b;

        if(a_ref < b_ref)
            cmp = -1;
        else if(a_ref > b_ref)
            cmp = +1;
        else
            cmp = 0;
    }

    return cmp;
}

static int
md_build_ref_def_hashtable(MD_CTX* ctx)
{
    int i, j;

    if(ctx->n_ref_defs == 0)
        return 0;

    ctx->ref_def_hashtable_size = (ctx->n_ref_defs * 5) / 4;
    ctx->ref_def_hashtable = (void**) malloc(ctx->ref_def_hashtable_size * sizeof(void*));
    if(ctx->ref_def_hashtable == NULL) {
        MD_LOG("malloc() failed.");
        goto abort;
    }
    memset(ctx->ref_def_hashtable, 0, ctx->ref_def_hashtable_size * sizeof(void*));

    for(i = 0; i < ctx->n_ref_defs; i++) {
        MD_REF_DEF* def = &ctx->ref_defs[i];
        void* bucket;
        MD_REF_DEF_LIST* list;

        def->hash = md_link_label_hash(def->label, def->label_size);
        bucket = ctx->ref_def_hashtable[def->hash % ctx->ref_def_hashtable_size];

        if(bucket == NULL) {

            ctx->ref_def_hashtable[def->hash % ctx->ref_def_hashtable_size] = def;
            continue;
        }

        if(ctx->ref_defs <= (MD_REF_DEF*) bucket  &&  (MD_REF_DEF*) bucket < ctx->ref_defs + ctx->n_ref_defs) {

            MD_REF_DEF* old_def = (MD_REF_DEF*) bucket;

            if(md_link_label_cmp(def->label, def->label_size, old_def->label, old_def->label_size) == 0) {

                continue;
            }

            list = (MD_REF_DEF_LIST*) malloc(sizeof(MD_REF_DEF_LIST) + 2 * sizeof(MD_REF_DEF*));
            if(list == NULL) {
                MD_LOG("malloc() failed.");
                goto abort;
            }
            list->ref_defs[0] = old_def;
            list->ref_defs[1] = def;
            list->n_ref_defs = 2;
            list->alloc_ref_defs = 2;
            ctx->ref_def_hashtable[def->hash % ctx->ref_def_hashtable_size] = list;
            continue;
        }

        list = (MD_REF_DEF_LIST*) bucket;
        if(list->n_ref_defs >= list->alloc_ref_defs) {
            int alloc_ref_defs = list->alloc_ref_defs + list->alloc_ref_defs / 2;
            MD_REF_DEF_LIST* list_tmp = (MD_REF_DEF_LIST*) realloc(list,
                        sizeof(MD_REF_DEF_LIST) + alloc_ref_defs * sizeof(MD_REF_DEF*));
            if(list_tmp == NULL) {
                MD_LOG("realloc() failed.");
                goto abort;
            }
            list = list_tmp;
            list->alloc_ref_defs = alloc_ref_defs;
            ctx->ref_def_hashtable[def->hash % ctx->ref_def_hashtable_size] = list;
        }

        list->ref_defs[list->n_ref_defs] = def;
        list->n_ref_defs++;
    }

    for(i = 0; i < ctx->ref_def_hashtable_size; i++) {
        void* bucket = ctx->ref_def_hashtable[i];
        MD_REF_DEF_LIST* list;

        if(bucket == NULL)
            continue;
        if(ctx->ref_defs <= (MD_REF_DEF*) bucket  &&  (MD_REF_DEF*) bucket < ctx->ref_defs + ctx->n_ref_defs)
            continue;

        list = (MD_REF_DEF_LIST*) bucket;
        qsort(list->ref_defs, list->n_ref_defs, sizeof(MD_REF_DEF*), md_ref_def_cmp_for_sort);

        for(j = 1; j < list->n_ref_defs; j++) {
            if(md_ref_def_cmp(&list->ref_defs[j-1], &list->ref_defs[j]) == 0)
                list->ref_defs[j] = list->ref_defs[j-1];
        }
    }

    return 0;

abort:
    return -1;
}

static void
md_free_ref_def_hashtable(MD_CTX* ctx)
{
    if(ctx->ref_def_hashtable != NULL) {
        int i;

        for(i = 0; i < ctx->ref_def_hashtable_size; i++) {
            void* bucket = ctx->ref_def_hashtable[i];
            if(bucket == NULL)
                continue;
            if(ctx->ref_defs <= (MD_REF_DEF*) bucket  &&  (MD_REF_DEF*) bucket < ctx->ref_defs + ctx->n_ref_defs)
                continue;
            free(bucket);
        }

        free(ctx->ref_def_hashtable);
    }
}

static const MD_REF_DEF*
md_lookup_ref_def(MD_CTX* ctx, const CHAR* label, SZ label_size)
{
    unsigned hash;
    void* bucket;

    if(ctx->ref_def_hashtable_size == 0)
        return NULL;

    hash = md_link_label_hash(label, label_size);
    bucket = ctx->ref_def_hashtable[hash % ctx->ref_def_hashtable_size];

    if(bucket == NULL) {
        return NULL;
    } else if(ctx->ref_defs <= (MD_REF_DEF*) bucket  &&  (MD_REF_DEF*) bucket < ctx->ref_defs + ctx->n_ref_defs) {
        const MD_REF_DEF* def = (MD_REF_DEF*) bucket;

        if(md_link_label_cmp(def->label, def->label_size, label, label_size) == 0)
            return def;
        else
            return NULL;
    } else {
        MD_REF_DEF_LIST* list = (MD_REF_DEF_LIST*) bucket;
        MD_REF_DEF key_buf;
        const MD_REF_DEF* key = &key_buf;
        const MD_REF_DEF** ret;

        key_buf.label = (CHAR*) label;
        key_buf.label_size = label_size;
        key_buf.hash = md_link_label_hash(key_buf.label, key_buf.label_size);

        ret = (const MD_REF_DEF**) bsearch(&key, list->ref_defs,
                    list->n_ref_defs, sizeof(MD_REF_DEF*), md_ref_def_cmp);
        if(ret != NULL)
            return *ret;
        else
            return NULL;
    }
}

typedef struct MD_LINK_ATTR_tag MD_LINK_ATTR;
struct MD_LINK_ATTR_tag {
    OFF dest_beg;
    OFF dest_end;

    CHAR* title;
    SZ title_size;
    int title_needs_free;
};

static int
md_is_link_label(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines, OFF beg,
                 OFF* p_end, MD_SIZE* p_beg_line_index, MD_SIZE* p_end_line_index,
                 OFF* p_contents_beg, OFF* p_contents_end)
{
    OFF off = beg;
    OFF contents_beg = 0;
    OFF contents_end = 0;
    MD_SIZE line_index = 0;
    int len = 0;

    *p_beg_line_index = 0;

    if(CH(off) != _T('['))
        return FALSE;
    off++;

    while(1) {
        OFF line_end = lines[line_index].end;

        while(off < line_end) {
            if(CH(off) == _T('\\')  &&  off+1 < ctx->size  &&  (ISPUNCT(off+1) || ISNEWLINE(off+1))) {
                if(contents_end == 0) {
                    contents_beg = off;
                    *p_beg_line_index = line_index;
                }
                contents_end = off + 2;
                off += 2;
            } else if(CH(off) == _T('[')) {
                return FALSE;
            } else if(CH(off) == _T(']')) {
                if(contents_beg < contents_end) {

                    *p_contents_beg = contents_beg;
                    *p_contents_end = contents_end;
                    *p_end = off+1;
                    *p_end_line_index = line_index;
                    return TRUE;
                } else {

                    return FALSE;
                }
            } else {
                unsigned codepoint;
                SZ char_size;

                codepoint = md_decode_unicode(ctx->text, off, ctx->size, &char_size);
                if(!ISUNICODEWHITESPACE_(codepoint)) {
                    if(contents_end == 0) {
                        contents_beg = off;
                        *p_beg_line_index = line_index;
                    }
                    contents_end = off + char_size;
                }

                off += char_size;
            }

            len++;
            if(len > 999)
                return FALSE;
        }

        line_index++;
        len++;
        if(line_index < n_lines)
            off = lines[line_index].beg;
        else
            break;
    }

    return FALSE;
}

static int
md_is_link_destination_A(MD_CTX* ctx, OFF beg, OFF max_end, OFF* p_end,
                         OFF* p_contents_beg, OFF* p_contents_end)
{
    OFF off = beg;

    if(off >= max_end  ||  CH(off) != _T('<'))
        return FALSE;
    off++;

    while(off < max_end) {
        if(CH(off) == _T('\\')  &&  off+1 < max_end  &&  ISPUNCT(off+1)) {
            off += 2;
            continue;
        }

        if(ISNEWLINE(off)  ||  CH(off) == _T('<'))
            return FALSE;

        if(CH(off) == _T('>')) {

            *p_contents_beg = beg+1;
            *p_contents_end = off;
            *p_end = off+1;
            return TRUE;
        }

        off++;
    }

    return FALSE;
}

static int
md_is_link_destination_B(MD_CTX* ctx, OFF beg, OFF max_end, OFF* p_end,
                         OFF* p_contents_beg, OFF* p_contents_end)
{
    OFF off = beg;
    int parenthesis_level = 0;

    while(off < max_end) {
        if(CH(off) == _T('\\')  &&  off+1 < max_end  &&  ISPUNCT(off+1)) {
            off += 2;
            continue;
        }

        if(ISWHITESPACE(off) || ISCNTRL(off))
            break;

        if(CH(off) == _T('(')) {
            parenthesis_level++;
            if(parenthesis_level > 32)
                return FALSE;
        } else if(CH(off) == _T(')')) {
            if(parenthesis_level == 0)
                break;
            parenthesis_level--;
        }

        off++;
    }

    if(parenthesis_level != 0  ||  off == beg)
        return FALSE;

    *p_contents_beg = beg;
    *p_contents_end = off;
    *p_end = off;
    return TRUE;
}

static inline int
md_is_link_destination(MD_CTX* ctx, OFF beg, OFF max_end, OFF* p_end,
                       OFF* p_contents_beg, OFF* p_contents_end)
{
    if(CH(beg) == _T('<'))
        return md_is_link_destination_A(ctx, beg, max_end, p_end, p_contents_beg, p_contents_end);
    else
        return md_is_link_destination_B(ctx, beg, max_end, p_end, p_contents_beg, p_contents_end);
}

static int
md_is_link_title(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines, OFF beg,
                 OFF* p_end, MD_SIZE* p_beg_line_index, MD_SIZE* p_end_line_index,
                 OFF* p_contents_beg, OFF* p_contents_end)
{
    OFF off = beg;
    CHAR closer_char;
    MD_SIZE line_index = 0;

    while(off < lines[line_index].end  &&  ISWHITESPACE(off))
        off++;
    if(off >= lines[line_index].end) {
        line_index++;
        if(line_index >= n_lines)
            return FALSE;
        off = lines[line_index].beg;
    }
    if(off == beg)
        return FALSE;

    *p_beg_line_index = line_index;

    switch(CH(off)) {
        case _T('"'):   closer_char = _T('"'); break;
        case _T('\''):  closer_char = _T('\''); break;
        case _T('('):   closer_char = _T(')'); break;
        default:        return FALSE;
    }
    off++;

    *p_contents_beg = off;

    while(line_index < n_lines) {
        OFF line_end = lines[line_index].end;

        while(off < line_end) {
            if(CH(off) == _T('\\')  &&  off+1 < ctx->size  &&  (ISPUNCT(off+1) || ISNEWLINE(off+1))) {
                off++;
            } else if(CH(off) == closer_char) {

                *p_contents_end = off;
                *p_end = off+1;
                *p_end_line_index = line_index;
                return TRUE;
            } else if(closer_char == _T(')')  &&  CH(off) == _T('(')) {

                return FALSE;
            }

            off++;
        }

        line_index++;
    }

    return FALSE;
}

static int
md_is_link_reference_definition(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines)
{
    OFF label_contents_beg;
    OFF label_contents_end;
    MD_SIZE label_contents_line_index;
    int label_is_multiline = FALSE;
    OFF dest_contents_beg;
    OFF dest_contents_end;
    OFF title_contents_beg;
    OFF title_contents_end;
    MD_SIZE title_contents_line_index;
    int title_is_multiline = FALSE;
    OFF off;
    MD_SIZE line_index = 0;
    MD_SIZE tmp_line_index;
    MD_REF_DEF* def = NULL;
    int ret = 0;

    if(!md_is_link_label(ctx, lines, n_lines, lines[0].beg,
                &off, &label_contents_line_index, &line_index,
                &label_contents_beg, &label_contents_end))
        return FALSE;
    label_is_multiline = (label_contents_line_index != line_index);

    if(off >= lines[line_index].end  ||  CH(off) != _T(':'))
        return FALSE;
    off++;

    while(off < lines[line_index].end  &&  ISWHITESPACE(off))
        off++;
    if(off >= lines[line_index].end) {
        line_index++;
        if(line_index >= n_lines)
            return FALSE;
        off = lines[line_index].beg;
    }

    if(!md_is_link_destination(ctx, off, lines[line_index].end,
                &off, &dest_contents_beg, &dest_contents_end))
        return FALSE;

    if(md_is_link_title(ctx, lines + line_index, n_lines - line_index, off,
                &off, &title_contents_line_index, &tmp_line_index,
                &title_contents_beg, &title_contents_end)
        &&  off >= lines[line_index + tmp_line_index].end)
    {
        title_is_multiline = (tmp_line_index != title_contents_line_index);
        title_contents_line_index += line_index;
        line_index += tmp_line_index;
    } else {

        title_is_multiline = FALSE;
        title_contents_beg = off;
        title_contents_end = off;
        title_contents_line_index = 0;
    }

    if(off < lines[line_index].end)
        return FALSE;

    if(ctx->n_ref_defs >= ctx->alloc_ref_defs) {
        MD_REF_DEF* new_defs;

        ctx->alloc_ref_defs = (ctx->alloc_ref_defs > 0
                ? ctx->alloc_ref_defs + ctx->alloc_ref_defs / 2
                : 16);
        new_defs = (MD_REF_DEF*) realloc(ctx->ref_defs, ctx->alloc_ref_defs * sizeof(MD_REF_DEF));
        if(new_defs == NULL) {
            MD_LOG("realloc() failed.");
            goto abort;
        }

        ctx->ref_defs = new_defs;
    }
    def = &ctx->ref_defs[ctx->n_ref_defs];
    memset(def, 0, sizeof(MD_REF_DEF));

    if(label_is_multiline) {
        MD_CHECK(md_merge_lines_alloc(ctx, label_contents_beg, label_contents_end,
                    lines + label_contents_line_index, n_lines - label_contents_line_index,
                    _T(' '), &def->label, &def->label_size));
        def->label_needs_free = TRUE;
    } else {
        def->label = (CHAR*) STR(label_contents_beg);
        def->label_size = label_contents_end - label_contents_beg;
    }

    if(title_is_multiline) {
        MD_CHECK(md_merge_lines_alloc(ctx, title_contents_beg, title_contents_end,
                    lines + title_contents_line_index, n_lines - title_contents_line_index,
                    _T('\n'), &def->title, &def->title_size));
        def->title_needs_free = TRUE;
    } else {
        def->title = (CHAR*) STR(title_contents_beg);
        def->title_size = title_contents_end - title_contents_beg;
    }

    def->dest_beg = dest_contents_beg;
    def->dest_end = dest_contents_end;

    ctx->n_ref_defs++;
    return line_index + 1;

abort:

    if(def != NULL  &&  def->label_needs_free)
        free(def->label);
    if(def != NULL  &&  def->title_needs_free)
        free(def->title);
    return ret;
}

static int
md_is_link_reference(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines,
                     OFF beg, OFF end, MD_LINK_ATTR* attr)
{
    const MD_REF_DEF* def;
    const MD_LINE* beg_line;
    int is_multiline;
    CHAR* label;
    SZ label_size;
    int ret = FALSE;

    MD_ASSERT(CH(beg) == _T('[') || CH(beg) == _T('!'));
    MD_ASSERT(CH(end-1) == _T(']'));

    if(ctx->max_ref_def_output == 0)
        return FALSE;

    beg += (CH(beg) == _T('!') ? 2 : 1);
    end--;

    beg_line = md_lookup_line(beg, lines, n_lines, NULL);
    is_multiline = (end > beg_line->end);

    if(is_multiline) {
        MD_CHECK(md_merge_lines_alloc(ctx, beg, end, beg_line,
                 (int)(n_lines - (beg_line - lines)), _T(' '), &label, &label_size));
    } else {
        label = (CHAR*) STR(beg);
        label_size = end - beg;
    }

    def = md_lookup_ref_def(ctx, label, label_size);
    if(def != NULL) {
        attr->dest_beg = def->dest_beg;
        attr->dest_end = def->dest_end;
        attr->title = def->title;
        attr->title_size = def->title_size;
        attr->title_needs_free = FALSE;
    }

    if(is_multiline)
        free(label);

    if(def != NULL) {

        MD_SIZE output_size_estimation = def->label_size + def->title_size + def->dest_end - def->dest_beg;
        if(output_size_estimation < ctx->max_ref_def_output) {
            ctx->max_ref_def_output -= output_size_estimation;
            ret = TRUE;
        } else {
            MD_LOG("Too many link reference definition instantiations.");
            ctx->max_ref_def_output = 0;
        }
    }

abort:
    return ret;
}

static int
md_is_inline_link_spec(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines,
                       OFF beg, OFF* p_end, MD_LINK_ATTR* attr)
{
    MD_SIZE line_index = 0;
    MD_SIZE tmp_line_index;
    OFF title_contents_beg;
    OFF title_contents_end;
    MD_SIZE title_contents_line_index;
    int title_is_multiline;
    OFF off = beg;
    int ret = FALSE;

    md_lookup_line(off, lines, n_lines, &line_index);

    MD_ASSERT(CH(off) == _T('('));
    off++;

    while(off < lines[line_index].end  &&  ISWHITESPACE(off))
        off++;
    if(off >= lines[line_index].end  &&  (off >= ctx->size  ||  ISNEWLINE(off))) {
        line_index++;
        if(line_index >= n_lines)
            return FALSE;
        off = lines[line_index].beg;
    }

    if(off < ctx->size  &&  CH(off) == _T(')')) {
        attr->dest_beg = off;
        attr->dest_end = off;
        attr->title = NULL;
        attr->title_size = 0;
        attr->title_needs_free = FALSE;
        off++;
        *p_end = off;
        return TRUE;
    }

    if(!md_is_link_destination(ctx, off, lines[line_index].end,
                        &off, &attr->dest_beg, &attr->dest_end))
        return FALSE;

    if(md_is_link_title(ctx, lines + line_index, n_lines - line_index, off,
                &off, &title_contents_line_index, &tmp_line_index,
                &title_contents_beg, &title_contents_end))
    {
        title_is_multiline = (tmp_line_index != title_contents_line_index);
        title_contents_line_index += line_index;
        line_index += tmp_line_index;
    } else {

        title_is_multiline = FALSE;
        title_contents_beg = off;
        title_contents_end = off;
        title_contents_line_index = 0;
    }

    while(off < lines[line_index].end  &&  ISWHITESPACE(off))
        off++;
    if(off >= lines[line_index].end) {
        line_index++;
        if(line_index >= n_lines)
            return FALSE;
        off = lines[line_index].beg;
    }
    if(CH(off) != _T(')'))
        goto abort;
    off++;

    if(title_contents_beg >= title_contents_end) {
        attr->title = NULL;
        attr->title_size = 0;
        attr->title_needs_free = FALSE;
    } else if(!title_is_multiline) {
        attr->title = (CHAR*) STR(title_contents_beg);
        attr->title_size = title_contents_end - title_contents_beg;
        attr->title_needs_free = FALSE;
    } else {
        MD_CHECK(md_merge_lines_alloc(ctx, title_contents_beg, title_contents_end,
                    lines + title_contents_line_index, n_lines - title_contents_line_index,
                    _T('\n'), &attr->title, &attr->title_size));
        attr->title_needs_free = TRUE;
    }

    *p_end = off;
    ret = TRUE;

abort:
    return ret;
}

static void
md_free_ref_defs(MD_CTX* ctx)
{
    int i;

    for(i = 0; i < ctx->n_ref_defs; i++) {
        MD_REF_DEF* def = &ctx->ref_defs[i];

        if(def->label_needs_free)
            free(def->label);
        if(def->title_needs_free)
            free(def->title);
    }

    free(ctx->ref_defs);
}

struct MD_MARK_tag {
    union {
        struct {
            OFF beg;
            OFF end;
        };
        void* pointer;
    };

    int prev;
    int next;
    CHAR ch;
    unsigned char flags;
};

#define MD_MARK_POTENTIAL_OPENER            0x01
#define MD_MARK_POTENTIAL_CLOSER            0x02
#define MD_MARK_OPENER                      0x04
#define MD_MARK_CLOSER                      0x08
#define MD_MARK_RESOLVED                    0x10

#define MD_MARK_EMPH_OC                     0x20
#define MD_MARK_EMPH_MOD3_0                 0x40
#define MD_MARK_EMPH_MOD3_1                 0x80
#define MD_MARK_EMPH_MOD3_2                 (0x40 | 0x80)
#define MD_MARK_EMPH_MOD3_MASK              (0x40 | 0x80)
#define MD_MARK_AUTOLINK                    0x20
#define MD_MARK_AUTOLINK_MISSING_MAILTO     0x40
#define MD_MARK_VALIDPERMISSIVEAUTOLINK     0x20
#define MD_MARK_HASNESTEDBRACKETS           0x20

static MD_MARKSTACK*
md_emph_stack(MD_CTX* ctx, MD_CHAR ch, unsigned flags)
{
    MD_MARKSTACK* stack;

    switch(ch) {
        case '*':   stack = &ASTERISK_OPENERS_oo_mod3_0; break;
        case '_':   stack = &UNDERSCORE_OPENERS_oo_mod3_0; break;
        default:    MD_UNREACHABLE();
    }

    if(flags & MD_MARK_EMPH_OC)
        stack += 3;

    switch(flags & MD_MARK_EMPH_MOD3_MASK) {
        case MD_MARK_EMPH_MOD3_0:   stack += 0; break;
        case MD_MARK_EMPH_MOD3_1:   stack += 1; break;
        case MD_MARK_EMPH_MOD3_2:   stack += 2; break;
        default:                    MD_UNREACHABLE();
    }

    return stack;
}

static MD_MARKSTACK*
md_opener_stack(MD_CTX* ctx, int mark_index)
{
    MD_MARK* mark = &ctx->marks[mark_index];

    switch(mark->ch) {
        case _T('*'):
        case _T('_'):   return md_emph_stack(ctx, mark->ch, mark->flags);

        case _T('~'):   return (mark->end - mark->beg == 1) ? &TILDE_OPENERS_1 : &TILDE_OPENERS_2;

        case _T('!'):
        case _T('['):   return &BRACKET_OPENERS;

        default:        MD_UNREACHABLE();
    }
}

static MD_MARK*
md_add_mark(MD_CTX* ctx)
{
    if(ctx->n_marks >= ctx->alloc_marks) {
        MD_MARK* new_marks;

        ctx->alloc_marks = (ctx->alloc_marks > 0
                ? ctx->alloc_marks + ctx->alloc_marks / 2
                : 64);
        new_marks = (MD_MARK*) realloc(ctx->marks, ctx->alloc_marks * sizeof(MD_MARK));
        if(new_marks == NULL) {
            MD_LOG("realloc() failed.");
            return NULL;
        }

        ctx->marks = new_marks;
    }

    return &ctx->marks[ctx->n_marks++];
}

#define ADD_MARK_()                                                     \
        do {                                                            \
            mark = md_add_mark(ctx);                                    \
            if(mark == NULL) {                                          \
                ret = -1;                                               \
                goto abort;                                             \
            }                                                           \
        } while(0)

#define ADD_MARK(ch_, beg_, end_, flags_)                               \
        do {                                                            \
            ADD_MARK_();                                                \
            mark->beg = (beg_);                                         \
            mark->end = (end_);                                         \
            mark->prev = -1;                                            \
            mark->next = -1;                                            \
            mark->ch = (char)(ch_);                                     \
            mark->flags = (flags_);                                     \
        } while(0)

static inline void
md_mark_stack_push(MD_CTX* ctx, MD_MARKSTACK* stack, int mark_index)
{
    ctx->marks[mark_index].next = stack->top;
    stack->top = mark_index;
}

static inline int
md_mark_stack_pop(MD_CTX* ctx, MD_MARKSTACK* stack)
{
    int top = stack->top;
    if(top >= 0)
        stack->top = ctx->marks[top].next;
    return top;
}

static inline void
md_mark_store_ptr(MD_CTX* ctx, int mark_index, void* ptr)
{
    MD_ASSERT(ctx->marks[mark_index].ch == 'D');
    ctx->marks[mark_index].pointer = ptr;
}

static inline void*
md_mark_get_ptr(MD_CTX* ctx, int mark_index)
{
    MD_ASSERT(ctx->marks[mark_index].ch == 'D');
    return ctx->marks[mark_index].pointer;
}

static inline void
md_resolve_range(MD_CTX* ctx, int opener_index, int closer_index)
{
    MD_MARK* opener = &ctx->marks[opener_index];
    MD_MARK* closer = &ctx->marks[closer_index];

    opener->next = closer_index;
    closer->prev = opener_index;

    opener->flags |= MD_MARK_OPENER | MD_MARK_RESOLVED;
    closer->flags |= MD_MARK_CLOSER | MD_MARK_RESOLVED;
}

static void
md_pop_openers(MD_CTX* ctx, int opener_index)
{
    int i;

    for(i = 0; i < (int) SIZEOF_ARRAY(ctx->opener_stacks); i++) {
        MD_MARKSTACK* stack = &ctx->opener_stacks[i];
        while(stack->top >= opener_index)
            md_mark_stack_pop(ctx, stack);
    }
}

static void
md_disable_marks(MD_CTX* ctx, int mark_index0, int mark_index1)
{
    int i;

    for(i = mark_index0; i < mark_index1; i++) {
        ctx->marks[i].ch = 'D';
        ctx->marks[i].flags &= ~MD_MARK_RESOLVED;
    }
}

static void
md_build_mark_char_map(MD_CTX* ctx)
{
    memset(ctx->mark_char_map, 0, sizeof(ctx->mark_char_map));

    ctx->mark_char_map['\\'] = 1;
    ctx->mark_char_map['*'] = 1;
    ctx->mark_char_map['_'] = 1;
    ctx->mark_char_map['`'] = 1;
    ctx->mark_char_map['&'] = 1;
    ctx->mark_char_map[';'] = 1;
    ctx->mark_char_map['<'] = 1;
    ctx->mark_char_map['>'] = 1;
    ctx->mark_char_map['['] = 1;
    ctx->mark_char_map['!'] = 1;
    ctx->mark_char_map[']'] = 1;
    ctx->mark_char_map['\0'] = 1;

    if(ctx->parser.flags & MD_FLAG_STRIKETHROUGH)
        ctx->mark_char_map['~'] = 1;

    if(ctx->parser.flags & MD_FLAG_LATEXMATHSPANS)
        ctx->mark_char_map['$'] = 1;

    if(ctx->parser.flags & MD_FLAG_PERMISSIVEEMAILAUTOLINKS)
        ctx->mark_char_map['@'] = 1;

    if(ctx->parser.flags & MD_FLAG_PERMISSIVEURLAUTOLINKS)
        ctx->mark_char_map[':'] = 1;

    if(ctx->parser.flags & MD_FLAG_PERMISSIVEWWWAUTOLINKS)
        ctx->mark_char_map['.'] = 1;

    if((ctx->parser.flags & MD_FLAG_TABLES) || (ctx->parser.flags & MD_FLAG_WIKILINKS))
        ctx->mark_char_map['|'] = 1;

    if(ctx->parser.flags & MD_FLAG_COLLAPSEWHITESPACE) {
        int i;

        for(i = 0; i < (int) sizeof(ctx->mark_char_map); i++) {
            if(ISWHITESPACE_(i))
                ctx->mark_char_map[i] = 1;
        }
    }
}

static int
md_is_code_span(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines, OFF beg,
                MD_MARK* opener, MD_MARK* closer,
                OFF last_potential_closers[CODESPAN_MARK_MAXLEN],
                int* p_reached_paragraph_end)
{
    OFF opener_beg = beg;
    OFF opener_end;
    OFF closer_beg;
    OFF closer_end;
    SZ mark_len;
    OFF line_end;
    int has_space_after_opener = FALSE;
    int has_eol_after_opener = FALSE;
    int has_space_before_closer = FALSE;
    int has_eol_before_closer = FALSE;
    int has_only_space = TRUE;
    MD_SIZE line_index = 0;

    line_end = lines[0].end;
    opener_end = opener_beg;
    while(opener_end < line_end  &&  CH(opener_end) == _T('`'))
        opener_end++;
    has_space_after_opener = (opener_end < line_end && CH(opener_end) == _T(' '));
    has_eol_after_opener = (opener_end == line_end);

    opener->end = opener_end;

    mark_len = opener_end - opener_beg;
    if(mark_len > CODESPAN_MARK_MAXLEN)
        return FALSE;

    if(last_potential_closers[mark_len-1] >= lines[n_lines-1].end  ||
       (*p_reached_paragraph_end  &&  last_potential_closers[mark_len-1] < opener_end))
        return FALSE;

    closer_beg = opener_end;
    closer_end = opener_end;

    while(TRUE) {
        while(closer_beg < line_end  &&  CH(closer_beg) != _T('`')) {
            if(CH(closer_beg) != _T(' '))
                has_only_space = FALSE;
            closer_beg++;
        }
        closer_end = closer_beg;
        while(closer_end < line_end  &&  CH(closer_end) == _T('`'))
            closer_end++;

        if(closer_end - closer_beg == mark_len) {

            has_space_before_closer = (closer_beg > lines[line_index].beg && CH(closer_beg-1) == _T(' '));
            has_eol_before_closer = (closer_beg == lines[line_index].beg);
            break;
        }

        if(closer_end - closer_beg > 0) {

            has_only_space = FALSE;

            if(closer_end - closer_beg < CODESPAN_MARK_MAXLEN) {
                if(closer_beg > last_potential_closers[closer_end - closer_beg - 1])
                    last_potential_closers[closer_end - closer_beg - 1] = closer_beg;
            }
        }

        if(closer_end >= line_end) {
            line_index++;
            if(line_index >= n_lines) {

                *p_reached_paragraph_end = TRUE;
                return FALSE;
            }

            line_end = lines[line_index].end;
            closer_beg = lines[line_index].beg;
        } else {
            closer_beg = closer_end;
        }
    }

    if(!has_only_space  &&
       (has_space_after_opener || has_eol_after_opener)  &&
       (has_space_before_closer || has_eol_before_closer))
    {
        if(has_space_after_opener)
            opener_end++;
        else
            opener_end = lines[1].beg;

        if(has_space_before_closer)
            closer_beg--;
        else {

            closer_beg = lines[line_index-1].end;

            while(closer_beg < ctx->size  &&  ISBLANK(closer_beg))
                closer_beg++;
        }
    }

    opener->ch = _T('`');
    opener->beg = opener_beg;
    opener->end = opener_end;
    opener->flags = MD_MARK_POTENTIAL_OPENER;
    closer->ch = _T('`');
    closer->beg = closer_beg;
    closer->end = closer_end;
    closer->flags = MD_MARK_POTENTIAL_CLOSER;
    return TRUE;
}

static int
md_is_autolink_uri(MD_CTX* ctx, OFF beg, OFF max_end, OFF* p_end)
{
    OFF off = beg+1;

    MD_ASSERT(CH(beg) == _T('<'));

    if(off >= max_end  ||  !ISASCII(off))
        return FALSE;
    off++;
    while(1) {
        if(off >= max_end)
            return FALSE;
        if(off - beg > 32)
            return FALSE;
        if(CH(off) == _T(':')  &&  off - beg >= 3)
            break;
        if(!ISALNUM(off) && CH(off) != _T('+') && CH(off) != _T('-') && CH(off) != _T('.'))
            return FALSE;
        off++;
    }

    while(off < max_end  &&  CH(off) != _T('>')) {
        if(ISWHITESPACE(off) || ISCNTRL(off) || CH(off) == _T('<'))
            return FALSE;
        off++;
    }

    if(off >= max_end)
        return FALSE;

    MD_ASSERT(CH(off) == _T('>'));
    *p_end = off+1;
    return TRUE;
}

static int
md_is_autolink_email(MD_CTX* ctx, OFF beg, OFF max_end, OFF* p_end)
{
    OFF off = beg + 1;
    int label_len;

    MD_ASSERT(CH(beg) == _T('<'));

    while(off < max_end  &&  (ISALNUM(off) || ISANYOF(off, _T(".!#$%&'*+/=?^_`{|}~-"))))
        off++;
    if(off <= beg+1)
        return FALSE;

    if(off >= max_end  ||  CH(off) != _T('@'))
        return FALSE;
    off++;

    label_len = 0;
    while(off < max_end) {
        if(ISALNUM(off))
            label_len++;
        else if(CH(off) == _T('-')  &&  label_len > 0)
            label_len++;
        else if(CH(off) == _T('.')  &&  label_len > 0  &&  CH(off-1) != _T('-'))
            label_len = 0;
        else
            break;

        if(label_len > 63)
            return FALSE;

        off++;
    }

    if(label_len <= 0  || off >= max_end  ||  CH(off) != _T('>') ||  CH(off-1) == _T('-'))
        return FALSE;

    *p_end = off+1;
    return TRUE;
}

static int
md_is_autolink(MD_CTX* ctx, OFF beg, OFF max_end, OFF* p_end, int* p_missing_mailto)
{
    if(md_is_autolink_uri(ctx, beg, max_end, p_end)) {
        *p_missing_mailto = FALSE;
        return TRUE;
    }

    if(md_is_autolink_email(ctx, beg, max_end, p_end)) {
        *p_missing_mailto = TRUE;
        return TRUE;
    }

    return FALSE;
}

static int
md_collect_marks(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines, int table_mode)
{
    MD_SIZE line_index;
    int ret = 0;
    MD_MARK* mark;
    OFF codespan_last_potential_closers[CODESPAN_MARK_MAXLEN] = { 0 };
    int codespan_scanned_till_paragraph_end = FALSE;

    for(line_index = 0; line_index < n_lines; line_index++) {
        const MD_LINE* line = &lines[line_index];
        OFF off = line->beg;

        while(TRUE) {
            CHAR ch;

#ifdef MD4C_USE_UTF16

    #define IS_MARK_CHAR(off)   ((CH(off) < SIZEOF_ARRAY(ctx->mark_char_map))  &&  \
                                (ctx->mark_char_map[(unsigned char) CH(off)]))
#else

    #define IS_MARK_CHAR(off)   (ctx->mark_char_map[(unsigned char) CH(off)])
#endif

            while(off + 3 < line->end  &&  !IS_MARK_CHAR(off+0)  &&  !IS_MARK_CHAR(off+1)
                                       &&  !IS_MARK_CHAR(off+2)  &&  !IS_MARK_CHAR(off+3))
                off += 4;
            while(off < line->end  &&  !IS_MARK_CHAR(off+0))
                off++;

            if(off >= line->end)
                break;

            ch = CH(off);

            if(ch == _T('\\')  &&  off+1 < ctx->size  &&  (ISPUNCT(off+1) || ISNEWLINE(off+1))) {

                if(!ISNEWLINE(off+1)  ||  line_index+1 < n_lines)
                    ADD_MARK(ch, off, off+2, MD_MARK_RESOLVED);
                off += 2;
                continue;
            }

            if(ch == _T('*')  ||  ch == _T('_')) {
                OFF tmp = off+1;
                int left_level;
                int right_level;

                while(tmp < line->end  &&  CH(tmp) == ch)
                    tmp++;

                if(off == line->beg  ||  ISUNICODEWHITESPACEBEFORE(off))
                    left_level = 0;
                else if(ISUNICODEPUNCTBEFORE(off))
                    left_level = 1;
                else
                    left_level = 2;

                if(tmp == line->end  ||  ISUNICODEWHITESPACE(tmp))
                    right_level = 0;
                else if(ISUNICODEPUNCT(tmp))
                    right_level = 1;
                else
                    right_level = 2;

                if(ch == _T('_')  &&  left_level == 2  &&  right_level == 2) {
                    left_level = 0;
                    right_level = 0;
                }

                if(left_level != 0  ||  right_level != 0) {
                    unsigned flags = 0;

                    if(left_level > 0  &&  left_level >= right_level)
                        flags |= MD_MARK_POTENTIAL_CLOSER;
                    if(right_level > 0  &&  right_level >= left_level)
                        flags |= MD_MARK_POTENTIAL_OPENER;
                    if(flags == (MD_MARK_POTENTIAL_OPENER | MD_MARK_POTENTIAL_CLOSER))
                        flags |= MD_MARK_EMPH_OC;

                    switch((tmp - off) % 3) {
                        case 0: flags |= MD_MARK_EMPH_MOD3_0; break;
                        case 1: flags |= MD_MARK_EMPH_MOD3_1; break;
                        case 2: flags |= MD_MARK_EMPH_MOD3_2; break;
                    }

                    ADD_MARK(ch, off, tmp, flags);

                    off++;
                    while(off < tmp) {
                        ADD_MARK('D', off, off, 0);
                        off++;
                    }
                    continue;
                }

                off = tmp;
                continue;
            }

            if(ch == _T('`')) {
                MD_MARK opener;
                MD_MARK closer;
                int is_code_span;

                is_code_span = md_is_code_span(ctx, line, n_lines - line_index, off,
                            &opener, &closer, codespan_last_potential_closers,
                            &codespan_scanned_till_paragraph_end);
                if(is_code_span) {
                    ADD_MARK(opener.ch, opener.beg, opener.end, opener.flags);
                    ADD_MARK(closer.ch, closer.beg, closer.end, closer.flags);
                    md_resolve_range(ctx, ctx->n_marks-2, ctx->n_marks-1);
                    off = closer.end;

                    if(off > line->end)
                        line = md_lookup_line(off, lines, n_lines, &line_index);
                    continue;
                }

                off = opener.end;
                continue;
            }

            if(ch == _T('&')) {
                ADD_MARK(ch, off, off+1, MD_MARK_POTENTIAL_OPENER);
                off++;
                continue;
            }

            if(ch == _T(';')) {

                if(ctx->n_marks > 0  &&  ctx->marks[ctx->n_marks-1].ch == _T('&'))
                    ADD_MARK(ch, off, off+1, MD_MARK_POTENTIAL_CLOSER);

                off++;
                continue;
            }

            if(ch == _T('<')) {
                int is_autolink;
                OFF autolink_end;
                int missing_mailto;

                if(!(ctx->parser.flags & MD_FLAG_NOHTMLSPANS)) {
                    int is_html;
                    OFF html_end;

                    is_html = md_is_html_any(ctx, line, n_lines - line_index, off,
                                    lines[n_lines-1].end, &html_end);
                    if(is_html) {
                        ADD_MARK(_T('<'), off, off, MD_MARK_OPENER | MD_MARK_RESOLVED);
                        ADD_MARK(_T('>'), html_end, html_end, MD_MARK_CLOSER | MD_MARK_RESOLVED);
                        ctx->marks[ctx->n_marks-2].next = ctx->n_marks-1;
                        ctx->marks[ctx->n_marks-1].prev = ctx->n_marks-2;
                        off = html_end;

                        if(off > line->end)
                            line = md_lookup_line(off, lines, n_lines, &line_index);
                        continue;
                    }
                }

                is_autolink = md_is_autolink(ctx, off, lines[n_lines-1].end,
                                    &autolink_end, &missing_mailto);
                if(is_autolink) {
                    unsigned flags = MD_MARK_RESOLVED | MD_MARK_AUTOLINK;
                    if(missing_mailto)
                        flags |= MD_MARK_AUTOLINK_MISSING_MAILTO;

                    ADD_MARK(_T('<'), off, off+1, MD_MARK_OPENER | flags);
                    ADD_MARK(_T('>'), autolink_end-1, autolink_end, MD_MARK_CLOSER | flags);
                    ctx->marks[ctx->n_marks-2].next = ctx->n_marks-1;
                    ctx->marks[ctx->n_marks-1].prev = ctx->n_marks-2;
                    off = autolink_end;
                    continue;
                }

                off++;
                continue;
            }

            if(ch == _T('[')  ||  (ch == _T('!') && off+1 < line->end && CH(off+1) == _T('['))) {
                OFF tmp = (ch == _T('[') ? off+1 : off+2);
                ADD_MARK(ch, off, tmp, MD_MARK_POTENTIAL_OPENER);
                off = tmp;

                ADD_MARK('D', off, off, 0);
                ADD_MARK('D', off, off, 0);
                continue;
            }
            if(ch == _T(']')) {
                ADD_MARK(ch, off, off+1, MD_MARK_POTENTIAL_CLOSER);
                off++;
                continue;
            }

            if(ch == _T('@')) {
                if(line->beg + 1 <= off  &&  ISALNUM(off-1)  &&
                    off + 3 < line->end  &&  ISALNUM(off+1))
                {
                    ADD_MARK(ch, off, off+1, MD_MARK_POTENTIAL_OPENER);

                    ADD_MARK('D', line->beg, line->end, 0);
                }

                off++;
                continue;
            }

            if(ch == _T(':')) {
                static const struct {
                    const CHAR* scheme;
                    SZ scheme_size;
                    const CHAR* suffix;
                    SZ suffix_size;
                } scheme_map[] = {

                    { _T("http"), 4,    _T("//"), 2 },
                    { _T("https"), 5,   _T("//"), 2 },
                    { _T("ftp"), 3,     _T("//"), 2 }
                };
                int scheme_index;

                for(scheme_index = 0; scheme_index < (int) SIZEOF_ARRAY(scheme_map); scheme_index++) {
                    const CHAR* scheme = scheme_map[scheme_index].scheme;
                    const SZ scheme_size = scheme_map[scheme_index].scheme_size;
                    const CHAR* suffix = scheme_map[scheme_index].suffix;
                    const SZ suffix_size = scheme_map[scheme_index].suffix_size;

                    if(line->beg + scheme_size <= off  &&  md_ascii_eq(STR(off-scheme_size), scheme, scheme_size)  &&
                        off + 1 + suffix_size < line->end  &&  md_ascii_eq(STR(off+1), suffix, suffix_size))
                    {
                        ADD_MARK(ch, off-scheme_size, off+1+suffix_size, MD_MARK_POTENTIAL_OPENER);

                        ADD_MARK('D', line->beg, line->end, 0);
                        off += 1 + suffix_size;
                        break;
                    }
                }

                off++;
                continue;
            }

            if(ch == _T('.')) {
                if(line->beg + 3 <= off  &&  md_ascii_eq(STR(off-3), _T("www"), 3)  &&
                   (off-3 == line->beg || ISUNICODEWHITESPACEBEFORE(off-3) || ISUNICODEPUNCTBEFORE(off-3)))
                {
                    ADD_MARK(ch, off-3, off+1, MD_MARK_POTENTIAL_OPENER);

                    ADD_MARK('D', line->beg, line->end, 0);
                    off++;
                    continue;
                }

                off++;
                continue;
            }

            if((table_mode || ctx->parser.flags & MD_FLAG_WIKILINKS) && ch == _T('|')) {
                ADD_MARK(ch, off, off+1, 0);
                off++;
                continue;
            }

            if(ch == _T('$') || ch == _T('~')) {
                OFF tmp = off+1;

                while(tmp < line->end && CH(tmp) == ch)
                    tmp++;

                if(tmp - off <= 2) {
                    unsigned flags = MD_MARK_POTENTIAL_OPENER | MD_MARK_POTENTIAL_CLOSER;

                    if(off > line->beg  &&  !ISUNICODEWHITESPACEBEFORE(off)  &&  !ISUNICODEPUNCTBEFORE(off))
                        flags &= ~MD_MARK_POTENTIAL_OPENER;
                    if(tmp < line->end  &&  !ISUNICODEWHITESPACE(tmp)  &&  !ISUNICODEPUNCT(tmp))
                        flags &= ~MD_MARK_POTENTIAL_CLOSER;
                    if(flags != 0)
                        ADD_MARK(ch, off, tmp, flags);
                }

                off = tmp;
                continue;
            }

            if(ISWHITESPACE_(ch)) {
                OFF tmp = off+1;

                while(tmp < line->end  &&  ISWHITESPACE(tmp))
                    tmp++;

                if(tmp - off > 1  ||  ch != _T(' '))
                    ADD_MARK(ch, off, tmp, MD_MARK_RESOLVED);

                off = tmp;
                continue;
            }

            if(ch == _T('\0')) {
                ADD_MARK(ch, off, off+1, MD_MARK_RESOLVED);
                off++;
                continue;
            }

            off++;
        }
    }

    ADD_MARK(127, ctx->size, ctx->size, MD_MARK_RESOLVED);

abort:
    return ret;
}

static void
md_analyze_bracket(MD_CTX* ctx, int mark_index)
{

    MD_MARK* mark = &ctx->marks[mark_index];

    if(mark->flags & MD_MARK_POTENTIAL_OPENER) {
        if(BRACKET_OPENERS.top >= 0)
            ctx->marks[BRACKET_OPENERS.top].flags |= MD_MARK_HASNESTEDBRACKETS;

        md_mark_stack_push(ctx, &BRACKET_OPENERS, mark_index);
        return;
    }

    if(BRACKET_OPENERS.top >= 0) {
        int opener_index = md_mark_stack_pop(ctx, &BRACKET_OPENERS);
        MD_MARK* opener = &ctx->marks[opener_index];

        opener->next = mark_index;
        mark->prev = opener_index;

        if(ctx->unresolved_link_tail >= 0)
            ctx->marks[ctx->unresolved_link_tail].prev = opener_index;
        else
            ctx->unresolved_link_head = opener_index;
        ctx->unresolved_link_tail = opener_index;
        opener->prev = -1;
    }
}

static void md_analyze_link_contents(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines,
                                     int mark_beg, int mark_end);

static int
md_resolve_links(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines)
{
    int opener_index = ctx->unresolved_link_head;
    OFF last_link_beg = 0;
    OFF last_link_end = 0;
    OFF last_img_beg = 0;
    OFF last_img_end = 0;

    while(opener_index >= 0) {
        MD_MARK* opener = &ctx->marks[opener_index];
        int closer_index = opener->next;
        MD_MARK* closer = &ctx->marks[closer_index];
        int next_index = opener->prev;
        MD_MARK* next_opener;
        MD_MARK* next_closer;
        MD_LINK_ATTR attr;
        int is_link = FALSE;

        if(next_index >= 0) {
            next_opener = &ctx->marks[next_index];
            next_closer = &ctx->marks[next_opener->next];
        } else {
            next_opener = NULL;
            next_closer = NULL;
        }

        if((opener->beg < last_link_beg  &&  closer->end < last_link_end)  ||
           (opener->beg < last_img_beg  &&  closer->end < last_img_end)  ||
           (opener->beg < last_link_end  &&  opener->ch == '['))
        {
            opener_index = next_index;
            continue;
        }

        if ((ctx->parser.flags & MD_FLAG_WIKILINKS) &&
            (opener->end - opener->beg == 1) &&
            next_opener != NULL &&
            next_opener->ch == '[' &&
            (next_opener->beg == opener->beg - 1) &&
            (next_opener->end - next_opener->beg == 1) &&
            next_closer != NULL &&
            next_closer->ch == ']' &&
            (next_closer->beg == closer->beg + 1) &&
            (next_closer->end - next_closer->beg == 1))
        {
            MD_MARK* delim = NULL;
            int delim_index;
            OFF dest_beg, dest_end;

            is_link = TRUE;

            delim_index = opener_index + 1;
            while(delim_index < closer_index) {
                MD_MARK* m = &ctx->marks[delim_index];
                if(m->ch == '|') {
                    delim = m;
                    break;
                }
                if(m->ch != 'D') {
                    if(m->beg - opener->end > 100)
                        break;
                    if(m->ch != 'D'  &&  (m->flags & MD_MARK_OPENER))
                        delim_index = m->next;
                }
                delim_index++;
            }

            dest_beg = opener->end;
            dest_end = (delim != NULL) ? delim->beg : closer->beg;
            if(dest_end - dest_beg == 0 || dest_end - dest_beg > 100)
                is_link = FALSE;

            if(is_link) {
                OFF off;
                for(off = dest_beg; off < dest_end; off++) {
                    if(ISNEWLINE(off)) {
                        is_link = FALSE;
                        break;
                    }
                }
            }

            if(is_link) {
                md_pop_openers(ctx, opener_index);

                if(delim != NULL) {
                    if(delim->end < closer->beg) {
                        md_disable_marks(ctx, delim_index+1, delim_index);
                        delim->flags |= MD_MARK_RESOLVED;
                        opener->end = delim->beg;
                    } else {

                        md_disable_marks(ctx, opener_index+1, closer_index);
                        closer->beg = delim->beg;
                    }
                }

                opener->beg = next_opener->beg;
                opener->next = closer_index;
                opener->flags |= MD_MARK_OPENER | MD_MARK_RESOLVED;

                closer->end = next_closer->end;
                closer->prev = opener_index;
                closer->flags |= MD_MARK_CLOSER | MD_MARK_RESOLVED;

                last_link_beg = opener->beg;
                last_link_end = closer->end;

                if(delim != NULL)
                    md_analyze_link_contents(ctx, lines, n_lines, delim_index+1, closer_index);

                opener_index = next_opener->prev;
                continue;
            }
        }

        if(next_opener != NULL  &&  next_opener->beg == closer->end) {
            if(next_closer->beg > closer->end + 1) {

                if(!(next_opener->flags & MD_MARK_HASNESTEDBRACKETS))
                    is_link = md_is_link_reference(ctx, lines, n_lines, next_opener->beg, next_closer->end, &attr);
            } else {

                if(!(opener->flags & MD_MARK_HASNESTEDBRACKETS))
                    is_link = md_is_link_reference(ctx, lines, n_lines, opener->beg, closer->end, &attr);
            }

            if(is_link < 0)
                return -1;

            if(is_link) {

                closer->end = next_closer->end;

                next_index = ctx->marks[next_index].prev;
            }
        } else {
            if(closer->end < ctx->size  &&  CH(closer->end) == _T('(')) {

                OFF inline_link_end = UINT_MAX;
                int following_mark_index = closer_index + 1;

                is_link = md_is_inline_link_spec(ctx, lines, n_lines, closer->end, &inline_link_end, &attr);
                if(is_link < 0)
                    return -1;

                if(is_link) {
                    while(following_mark_index < ctx->n_marks) {
                        MD_MARK* mark = &ctx->marks[following_mark_index];

                        if(mark->beg >= inline_link_end)
                            break;
                        if((mark->flags & (MD_MARK_OPENER | MD_MARK_RESOLVED)) == (MD_MARK_OPENER | MD_MARK_RESOLVED)) {
                            if(ctx->marks[mark->next].beg >= inline_link_end) {

                                if(attr.title_needs_free)
                                    free(attr.title);
                                is_link = FALSE;
                                break;
                            }

                            following_mark_index = mark->next + 1;
                        } else {
                            following_mark_index++;
                        }
                    }
                }

                if(is_link) {

                    closer->end = inline_link_end;
                    md_disable_marks(ctx, closer_index+1, following_mark_index);
                }
            }

            if(!is_link) {

                if(!(opener->flags & MD_MARK_HASNESTEDBRACKETS))
                    is_link = md_is_link_reference(ctx, lines, n_lines, opener->beg, closer->end, &attr);
                if(is_link < 0)
                    return -1;
            }
        }

        if(is_link) {

            opener->flags |= MD_MARK_OPENER | MD_MARK_RESOLVED;
            closer->flags |= MD_MARK_CLOSER | MD_MARK_RESOLVED;

            MD_ASSERT(ctx->marks[opener_index+1].ch == 'D');
            ctx->marks[opener_index+1].beg = attr.dest_beg;
            ctx->marks[opener_index+1].end = attr.dest_end;

            MD_ASSERT(ctx->marks[opener_index+2].ch == 'D');
            md_mark_store_ptr(ctx, opener_index+2, attr.title);

            if(attr.title_needs_free)
                md_mark_stack_push(ctx, &ctx->ptr_stack, opener_index+2);
            ctx->marks[opener_index+2].prev = attr.title_size;

            if(opener->ch == '[') {
                last_link_beg = opener->beg;
                last_link_end = closer->end;
            } else {
                last_img_beg = opener->beg;
                last_img_end = closer->end;
            }

            md_analyze_link_contents(ctx, lines, n_lines, opener_index+1, closer_index);

            if(ctx->parser.flags & MD_FLAG_PERMISSIVEAUTOLINKS) {
                MD_MARK* first_nested;
                MD_MARK* last_nested;

                first_nested = opener + 1;
                while(first_nested->ch == _T('D')  &&  first_nested < closer)
                    first_nested++;

                last_nested = closer - 1;
                while(first_nested->ch == _T('D')  &&  last_nested > opener)
                    last_nested--;

                if((first_nested->flags & MD_MARK_RESOLVED)  &&
                   first_nested->beg == opener->end  &&
                   ISANYOF_(first_nested->ch, _T("@:."))  &&
                   first_nested->next == (last_nested - ctx->marks)  &&
                   last_nested->end == closer->beg)
                {
                    first_nested->ch = _T('D');
                    first_nested->flags &= ~MD_MARK_RESOLVED;
                    last_nested->ch = _T('D');
                    last_nested->flags &= ~MD_MARK_RESOLVED;
                }
            }
        }

        opener_index = next_index;
    }

    return 0;
}

static void
md_analyze_entity(MD_CTX* ctx, int mark_index)
{
    MD_MARK* opener = &ctx->marks[mark_index];
    MD_MARK* closer;
    OFF off;

    if(mark_index + 1 >= ctx->n_marks)
        return;
    closer = &ctx->marks[mark_index+1];
    if(closer->ch != ';')
        return;

    if(md_is_entity(ctx, opener->beg, closer->end, &off)) {
        MD_ASSERT(off == closer->end);

        md_resolve_range(ctx, mark_index, mark_index+1);
        opener->end = closer->end;
    }
}

static void
md_analyze_table_cell_boundary(MD_CTX* ctx, int mark_index)
{
    MD_MARK* mark = &ctx->marks[mark_index];
    mark->flags |= MD_MARK_RESOLVED;
    mark->next = -1;

    if(ctx->table_cell_boundaries_head < 0)
        ctx->table_cell_boundaries_head = mark_index;
    else
        ctx->marks[ctx->table_cell_boundaries_tail].next = mark_index;
    ctx->table_cell_boundaries_tail = mark_index;
    ctx->n_table_cell_boundaries++;
}

static int
md_split_emph_mark(MD_CTX* ctx, int mark_index, SZ n)
{
    MD_MARK* mark = &ctx->marks[mark_index];
    int new_mark_index = mark_index + (mark->end - mark->beg - n);
    MD_MARK* dummy = &ctx->marks[new_mark_index];

    MD_ASSERT(mark->end - mark->beg > n);
    MD_ASSERT(dummy->ch == 'D');

    memcpy(dummy, mark, sizeof(MD_MARK));
    mark->end -= n;
    dummy->beg = mark->end;

    return new_mark_index;
}

static void
md_analyze_emph(MD_CTX* ctx, int mark_index)
{
    MD_MARK* mark = &ctx->marks[mark_index];

    if(mark->flags & MD_MARK_POTENTIAL_CLOSER) {
        MD_MARK* opener = NULL;
        int opener_index = 0;
        MD_MARKSTACK* opener_stacks[6];
        int i, n_opener_stacks;
        unsigned flags = mark->flags;

        n_opener_stacks = 0;

        opener_stacks[n_opener_stacks++] = md_emph_stack(ctx, mark->ch, MD_MARK_EMPH_MOD3_0 | MD_MARK_EMPH_OC);
        if((flags & MD_MARK_EMPH_MOD3_MASK) != MD_MARK_EMPH_MOD3_2)
            opener_stacks[n_opener_stacks++] = md_emph_stack(ctx, mark->ch, MD_MARK_EMPH_MOD3_1 | MD_MARK_EMPH_OC);
        if((flags & MD_MARK_EMPH_MOD3_MASK) != MD_MARK_EMPH_MOD3_1)
            opener_stacks[n_opener_stacks++] = md_emph_stack(ctx, mark->ch, MD_MARK_EMPH_MOD3_2 | MD_MARK_EMPH_OC);
        opener_stacks[n_opener_stacks++] = md_emph_stack(ctx, mark->ch, MD_MARK_EMPH_MOD3_0);
        if(!(flags & MD_MARK_EMPH_OC)  ||  (flags & MD_MARK_EMPH_MOD3_MASK) != MD_MARK_EMPH_MOD3_2)
            opener_stacks[n_opener_stacks++] = md_emph_stack(ctx, mark->ch, MD_MARK_EMPH_MOD3_1);
        if(!(flags & MD_MARK_EMPH_OC)  ||  (flags & MD_MARK_EMPH_MOD3_MASK) != MD_MARK_EMPH_MOD3_1)
            opener_stacks[n_opener_stacks++] = md_emph_stack(ctx, mark->ch, MD_MARK_EMPH_MOD3_2);

        for(i = 0; i < n_opener_stacks; i++) {
            if(opener_stacks[i]->top >= 0) {
                int m_index = opener_stacks[i]->top;
                MD_MARK* m = &ctx->marks[m_index];

                if(opener == NULL  ||  m->end > opener->end) {
                    opener_index = m_index;
                    opener = m;
                }
            }
        }

        if(opener != NULL) {
            SZ opener_size = opener->end - opener->beg;
            SZ closer_size = mark->end - mark->beg;
            MD_MARKSTACK* stack = md_opener_stack(ctx, opener_index);

            if(opener_size > closer_size) {
                opener_index = md_split_emph_mark(ctx, opener_index, closer_size);
                md_mark_stack_push(ctx, stack, opener_index);
            } else if(opener_size < closer_size) {
                md_split_emph_mark(ctx, mark_index, closer_size - opener_size);
            }

            md_pop_openers(ctx, opener_index);
            md_resolve_range(ctx, opener_index, mark_index);
            return;
        }
    }

    if(mark->flags & MD_MARK_POTENTIAL_OPENER)
        md_mark_stack_push(ctx, md_emph_stack(ctx, mark->ch, mark->flags), mark_index);
}

static void
md_analyze_tilde(MD_CTX* ctx, int mark_index)
{
    MD_MARK* mark = &ctx->marks[mark_index];
    MD_MARKSTACK* stack = md_opener_stack(ctx, mark_index);

    if((mark->flags & MD_MARK_POTENTIAL_CLOSER)  &&  stack->top >= 0) {
        int opener_index = stack->top;

        md_pop_openers(ctx, opener_index);
        md_resolve_range(ctx, opener_index, mark_index);
        return;
    }

    if(mark->flags & MD_MARK_POTENTIAL_OPENER)
        md_mark_stack_push(ctx, stack, mark_index);
}

static void
md_analyze_dollar(MD_CTX* ctx, int mark_index)
{
    MD_MARK* mark = &ctx->marks[mark_index];

    if((mark->flags & MD_MARK_POTENTIAL_CLOSER)  &&  DOLLAR_OPENERS.top >= 0) {

        MD_MARK* opener = &ctx->marks[DOLLAR_OPENERS.top];
        int opener_index = DOLLAR_OPENERS.top;
        MD_MARK* closer = mark;
        int closer_index = mark_index;

        if(opener->end - opener->beg == closer->end - closer->beg) {

            md_pop_openers(ctx, opener_index);
            md_disable_marks(ctx, opener_index+1, closer_index);
            md_resolve_range(ctx, opener_index, closer_index);

            DOLLAR_OPENERS.top = -1;
            return;
        }
    }

    if(mark->flags & MD_MARK_POTENTIAL_OPENER)
        md_mark_stack_push(ctx, &DOLLAR_OPENERS, mark_index);
}

static MD_MARK*
md_scan_left_for_resolved_mark(MD_CTX* ctx, MD_MARK* mark_from, OFF off, MD_MARK** p_cursor)
{
    MD_MARK* mark;

    for(mark = mark_from; mark >= ctx->marks; mark--) {
        if(mark->ch == 'D'  ||  mark->beg > off)
            continue;
        if(mark->beg <= off  &&  off < mark->end  &&  (mark->flags & MD_MARK_RESOLVED)) {
            if(p_cursor != NULL)
                *p_cursor = mark;
            return mark;
        }
        if(mark->end <= off)
            break;
    }

    if(p_cursor != NULL)
        *p_cursor = mark;
    return NULL;
}

static MD_MARK*
md_scan_right_for_resolved_mark(MD_CTX* ctx, MD_MARK* mark_from, OFF off, MD_MARK** p_cursor)
{
    MD_MARK* mark;

    for(mark = mark_from; mark < ctx->marks + ctx->n_marks; mark++) {
        if(mark->ch == 'D'  ||  mark->end <= off)
            continue;
        if(mark->beg <= off  &&  off < mark->end  &&  (mark->flags & MD_MARK_RESOLVED)) {
            if(p_cursor != NULL)
                *p_cursor = mark;
            return mark;
        }
        if(mark->beg > off)
            break;
    }

    if(p_cursor != NULL)
        *p_cursor = mark;
    return NULL;
}

static void
md_analyze_permissive_autolink(MD_CTX* ctx, int mark_index)
{
    static const struct {
        const MD_CHAR start_char;
        const MD_CHAR delim_char;
        const MD_CHAR* allowed_nonalnum_chars_inside;
        const MD_CHAR* allowed_nonalnum_chars_anywhere;
        int min_components;
        const MD_CHAR optional_end_char;
    } URL_MAP[] = {
        { _T('\0'), _T('.'),  _T(".-_"),      _T(""),   2, _T('\0') },
        { _T('/'),  _T('/'),  _T("/._"),      _T("+-"), 0, _T('/') },
        { _T('?'),  _T('&'),  _T("&.-+_=()"), _T(""),   1, _T('\0') },
        { _T('#'),  _T('\0'), _T(".-+_") ,    _T(""),   1, _T('\0') }
    };

    MD_MARK* opener = &ctx->marks[mark_index];
    MD_MARK* closer = &ctx->marks[mark_index + 1];
    OFF line_beg = closer->beg;
    OFF line_end = closer->end;
    OFF beg = opener->beg;
    OFF end = opener->end;
    MD_MARK* left_cursor = opener;
    int left_boundary_ok = FALSE;
    MD_MARK* right_cursor = opener;
    int right_boundary_ok = FALSE;
    unsigned i;

    MD_ASSERT(closer->ch == 'D');

    if(opener->ch == '@') {
        MD_ASSERT(CH(opener->beg) == _T('@'));

        while(beg > line_beg) {
            if(ISALNUM(beg-1))
                beg--;
            else if(beg >= line_beg+2  &&  ISALNUM(beg-2)  &&
                        ISANYOF(beg-1, _T(".-_+"))  &&
                        md_scan_left_for_resolved_mark(ctx, left_cursor, beg-1, &left_cursor) == NULL  &&
                        ISALNUM(beg))
                beg--;
            else
                break;
        }
        if(beg == opener->beg)
            return;
    }

    if(beg == line_beg  ||  ISUNICODEWHITESPACEBEFORE(beg)  ||  ISANYOF(beg-1, _T("({["))) {
        left_boundary_ok = TRUE;
    } else if(ISANYOF(beg-1, _T("*_~"))) {
        MD_MARK* left_mark;

        left_mark = md_scan_left_for_resolved_mark(ctx, left_cursor, beg-1, &left_cursor);
        if(left_mark != NULL  &&  (left_mark->flags & MD_MARK_OPENER))
            left_boundary_ok = TRUE;
    }
    if(!left_boundary_ok)
        return;

    for(i = 0; i < SIZEOF_ARRAY(URL_MAP); i++) {
        int n_components = 0;
        int n_open_brackets = 0;
        int component_len = 0;

        if(URL_MAP[i].start_char != _T('\0')) {
            if(end >= line_end  ||  CH(end) != URL_MAP[i].start_char)
                continue;
            if(URL_MAP[i].min_components > 0  &&  (end+1 >= line_end  ||  !ISALNUM(end+1)))
                continue;
            end++;
        }

        while(end < line_end) {
            if(ISALNUM(end)  ||  ISANYOF(end, URL_MAP[i].allowed_nonalnum_chars_anywhere)) {
                if(n_components == 0)
                    n_components++;
                component_len++;
                end++;
            } else if(component_len > 0  &&  CH(end) == URL_MAP[i].delim_char  &&  end+1 < line_end  &&
                      (ISALNUM(end+1)  ||  ISANYOF(end+1, URL_MAP[i].allowed_nonalnum_chars_anywhere))) {
                n_components++;
                component_len = 0;
                end++;
            } else if(ISANYOF(end, URL_MAP[i].allowed_nonalnum_chars_inside)  &&
                      md_scan_right_for_resolved_mark(ctx, right_cursor, end, &right_cursor) == NULL  &&
                      ((end > line_beg && (ISALNUM(end-1) || CH(end-1) == _T(')')))  ||  CH(end) == _T('('))  &&
                      ((end+1 < line_end && (ISALNUM(end+1) || CH(end+1) == _T('(')))  ||  CH(end) == _T(')')))
            {

                if(CH(end) == _T('(')) {
                    n_open_brackets++;
                } else if(CH(end) == _T(')')) {
                    if(n_open_brackets <= 0)
                        break;
                    n_open_brackets--;
                }

                component_len++;
                end++;
            } else {
                break;
            }
        }

        if(end < line_end  &&  URL_MAP[i].optional_end_char != _T('\0')  &&
                CH(end) == URL_MAP[i].optional_end_char)
            end++;

        if(n_components < URL_MAP[i].min_components  ||  n_open_brackets != 0)
            return;

        if(opener->ch == '@')
            break;
    }

    if(end == line_end  ||  ISUNICODEWHITESPACE(end)  ||  ISANYOF(end, _T(")}].!?,;"))) {
        right_boundary_ok = TRUE;
    } else {
        MD_MARK* right_mark;

        right_mark = md_scan_right_for_resolved_mark(ctx, right_cursor, end, &right_cursor);
        if(right_mark != NULL  &&  (right_mark->flags & MD_MARK_CLOSER))
            right_boundary_ok = TRUE;
    }
    if(!right_boundary_ok)
        return;

    opener->beg = beg;
    opener->end = beg;
    closer->beg = end;
    closer->end = end;
    closer->ch = opener->ch;
    md_resolve_range(ctx, mark_index, mark_index + 1);
}

#define MD_ANALYZE_NOSKIP_EMPH  0x01

static inline void
md_analyze_marks(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines,
                 int mark_beg, int mark_end, const CHAR* mark_chars, unsigned flags)
{
    int i = mark_beg;
    OFF last_end = lines[0].beg;

    MD_UNUSED(lines);
    MD_UNUSED(n_lines);

    while(i < mark_end) {
        MD_MARK* mark = &ctx->marks[i];

        if(mark->flags & MD_MARK_RESOLVED) {
            if((mark->flags & MD_MARK_OPENER)  &&
               !((flags & MD_ANALYZE_NOSKIP_EMPH) && ISANYOF_(mark->ch, "*_~")))
            {
                MD_ASSERT(i < mark->next);
                i = mark->next + 1;
            } else {
                i++;
            }
            continue;
        }

        if(!ISANYOF_(mark->ch, mark_chars)) {
            i++;
            continue;
        }

        if(mark->beg < last_end) {
            i++;
            continue;
        }

        switch(mark->ch) {
            case '[':
            case '!':
            case ']':   md_analyze_bracket(ctx, i); break;
            case '&':   md_analyze_entity(ctx, i); break;
            case '|':   md_analyze_table_cell_boundary(ctx, i); break;
            case '_':
            case '*':   md_analyze_emph(ctx, i); break;
            case '~':   md_analyze_tilde(ctx, i); break;
            case '$':   md_analyze_dollar(ctx, i); break;
            case '.':
            case ':':
            case '@':   md_analyze_permissive_autolink(ctx, i); break;
        }

        if(mark->flags & MD_MARK_RESOLVED) {
            if(mark->flags & MD_MARK_OPENER)
                last_end = ctx->marks[mark->next].end;
            else
                last_end = mark->end;
        }

        i++;
    }
}

static int
md_analyze_inlines(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines, int table_mode)
{
    int ret;

    ctx->n_marks = 0;

    MD_CHECK(md_collect_marks(ctx, lines, n_lines, table_mode));

    md_analyze_marks(ctx, lines, n_lines, 0, ctx->n_marks, _T("[]!"), 0);
    MD_CHECK(md_resolve_links(ctx, lines, n_lines));
    BRACKET_OPENERS.top = -1;
    ctx->unresolved_link_head = -1;
    ctx->unresolved_link_tail = -1;

    if(table_mode) {

        MD_ASSERT(n_lines == 1);
        ctx->n_table_cell_boundaries = 0;
        md_analyze_marks(ctx, lines, n_lines, 0, ctx->n_marks, _T("|"), 0);
        return ret;
    }

    md_analyze_link_contents(ctx, lines, n_lines, 0, ctx->n_marks);

abort:
    return ret;
}

static void
md_analyze_link_contents(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines,
                         int mark_beg, int mark_end)
{
    int i;

    md_analyze_marks(ctx, lines, n_lines, mark_beg, mark_end, _T("&"), 0);
    md_analyze_marks(ctx, lines, n_lines, mark_beg, mark_end, _T("*_~$"), 0);

    if((ctx->parser.flags & MD_FLAG_PERMISSIVEAUTOLINKS) != 0) {

        md_analyze_marks(ctx, lines, n_lines, mark_beg, mark_end, _T("@:."), MD_ANALYZE_NOSKIP_EMPH);
    }

    for(i = 0; i < (int) SIZEOF_ARRAY(ctx->opener_stacks); i++)
        ctx->opener_stacks[i].top = -1;
}

static int
md_enter_leave_span_a(MD_CTX* ctx, int enter, MD_SPANTYPE type,
                      const CHAR* dest, SZ dest_size, int is_autolink,
                      const CHAR* title, SZ title_size)
{
    MD_ATTRIBUTE_BUILD href_build = { 0 };
    MD_ATTRIBUTE_BUILD title_build = { 0 };
    MD_SPAN_A_DETAIL det;
    int ret = 0;

    memset(&det, 0, sizeof(MD_SPAN_A_DETAIL));
    MD_CHECK(md_build_attribute(ctx, dest, dest_size,
                    (is_autolink ? MD_BUILD_ATTR_NO_ESCAPES : 0),
                    &det.href, &href_build));
    MD_CHECK(md_build_attribute(ctx, title, title_size, 0, &det.title, &title_build));
    det.is_autolink = is_autolink;
    if(enter)
        MD_ENTER_SPAN(type, &det);
    else
        MD_LEAVE_SPAN(type, &det);

abort:
    md_free_attribute(ctx, &href_build);
    md_free_attribute(ctx, &title_build);
    return ret;
}

static int
md_enter_leave_span_wikilink(MD_CTX* ctx, int enter, const CHAR* target, SZ target_size)
{
    MD_ATTRIBUTE_BUILD target_build = { 0 };
    MD_SPAN_WIKILINK_DETAIL det;
    int ret = 0;

    memset(&det, 0, sizeof(MD_SPAN_WIKILINK_DETAIL));
    MD_CHECK(md_build_attribute(ctx, target, target_size, 0, &det.target, &target_build));

    if (enter)
        MD_ENTER_SPAN(MD_SPAN_WIKILINK, &det);
    else
        MD_LEAVE_SPAN(MD_SPAN_WIKILINK, &det);

abort:
    md_free_attribute(ctx, &target_build);
    return ret;
}

static int
md_process_inlines(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines)
{
    MD_TEXTTYPE text_type;
    const MD_LINE* line = lines;
    MD_MARK* prev_mark = NULL;
    MD_MARK* mark;
    OFF off = lines[0].beg;
    OFF end = lines[n_lines-1].end;
    OFF tmp;
    int enforce_hardbreak = 0;
    int ret = 0;

    mark = ctx->marks;
    while(!(mark->flags & MD_MARK_RESOLVED))
        mark++;

    text_type = MD_TEXT_NORMAL;

    while(1) {

        tmp = (line->end < mark->beg ? line->end : mark->beg);
        if(tmp > off) {
            MD_TEXT(text_type, STR(off), tmp - off);
            off = tmp;
        }

        if(off >= mark->beg) {
            switch(mark->ch) {
                case '\\':
                    if(ISNEWLINE(mark->beg+1))
                        enforce_hardbreak = 1;
                    else
                        MD_TEXT(text_type, STR(mark->beg+1), 1);
                    break;

                case ' ':
                    MD_TEXT(text_type, _T(" "), 1);
                    break;

                case '`':
                    if(mark->flags & MD_MARK_OPENER) {
                        MD_ENTER_SPAN(MD_SPAN_CODE, NULL);
                        text_type = MD_TEXT_CODE;
                    } else {
                        MD_LEAVE_SPAN(MD_SPAN_CODE, NULL);
                        text_type = MD_TEXT_NORMAL;
                    }
                    break;

                case '_':
                    if(ctx->parser.flags & MD_FLAG_UNDERLINE) {
                        if(mark->flags & MD_MARK_OPENER) {
                            while(off < mark->end) {
                                MD_ENTER_SPAN(MD_SPAN_U, NULL);
                                off++;
                            }
                        } else {
                            while(off < mark->end) {
                                MD_LEAVE_SPAN(MD_SPAN_U, NULL);
                                off++;
                            }
                        }
                        break;
                    }
                    MD_FALLTHROUGH();

                case '*':
                    if(mark->flags & MD_MARK_OPENER) {
                        if((mark->end - off) % 2) {
                            MD_ENTER_SPAN(MD_SPAN_EM, NULL);
                            off++;
                        }
                        while(off + 1 < mark->end) {
                            MD_ENTER_SPAN(MD_SPAN_STRONG, NULL);
                            off += 2;
                        }
                    } else {
                        while(off + 1 < mark->end) {
                            MD_LEAVE_SPAN(MD_SPAN_STRONG, NULL);
                            off += 2;
                        }
                        if((mark->end - off) % 2) {
                            MD_LEAVE_SPAN(MD_SPAN_EM, NULL);
                            off++;
                        }
                    }
                    break;

                case '~':
                    if(mark->flags & MD_MARK_OPENER)
                        MD_ENTER_SPAN(MD_SPAN_DEL, NULL);
                    else
                        MD_LEAVE_SPAN(MD_SPAN_DEL, NULL);
                    break;

                case '$':
                    if(mark->flags & MD_MARK_OPENER) {
                        MD_ENTER_SPAN((mark->end - off) % 2 ? MD_SPAN_LATEXMATH : MD_SPAN_LATEXMATH_DISPLAY, NULL);
                        text_type = MD_TEXT_LATEXMATH;
                    } else {
                        MD_LEAVE_SPAN((mark->end - off) % 2 ? MD_SPAN_LATEXMATH : MD_SPAN_LATEXMATH_DISPLAY, NULL);
                        text_type = MD_TEXT_NORMAL;
                    }
                    break;

                case '[':
                case '!':
                case ']':
                {
                    const MD_MARK* opener = (mark->ch != ']' ? mark : &ctx->marks[mark->prev]);
                    const MD_MARK* closer = &ctx->marks[opener->next];
                    const MD_MARK* dest_mark;
                    const MD_MARK* title_mark;

                    if ((opener->ch == '[' && closer->ch == ']') &&
                        opener->end - opener->beg >= 2 &&
                        closer->end - closer->beg >= 2)
                    {
                        int has_label = (opener->end - opener->beg > 2);
                        SZ target_sz;

                        if(has_label)
                            target_sz = opener->end - (opener->beg+2);
                        else
                            target_sz = closer->beg - opener->end;

                        MD_CHECK(md_enter_leave_span_wikilink(ctx, (mark->ch != ']'),
                                 has_label ? STR(opener->beg+2) : STR(opener->end),
                                 target_sz));

                        break;
                    }

                    dest_mark = opener+1;
                    MD_ASSERT(dest_mark->ch == 'D');
                    title_mark = opener+2;
                    MD_ASSERT(title_mark->ch == 'D');

                    MD_CHECK(md_enter_leave_span_a(ctx, (mark->ch != ']'),
                                (opener->ch == '!' ? MD_SPAN_IMG : MD_SPAN_A),
                                STR(dest_mark->beg), dest_mark->end - dest_mark->beg, FALSE,
                                (const CHAR*) md_mark_get_ptr(ctx, (int)(title_mark - ctx->marks)),
								title_mark->prev));

                    if(mark->ch == ']') {
                        while(mark->end > line->end)
                            line++;
                    }

                    break;
                }

                case '<':
                case '>':
                    if(!(mark->flags & MD_MARK_AUTOLINK)) {

                        if(mark->flags & MD_MARK_OPENER)
                            text_type = MD_TEXT_HTML;
                        else
                            text_type = MD_TEXT_NORMAL;
                        break;
                    }

                    MD_FALLTHROUGH();

                case '@':
                case ':':
                case '.':
                {
                    MD_MARK* opener = ((mark->flags & MD_MARK_OPENER) ? mark : &ctx->marks[mark->prev]);
                    MD_MARK* closer = &ctx->marks[opener->next];
                    const CHAR* dest = STR(opener->end);
                    SZ dest_size = closer->beg - opener->end;

                    if(mark->flags & MD_MARK_OPENER)
                        closer->flags |= MD_MARK_VALIDPERMISSIVEAUTOLINK;

                    if(opener->ch == '@' || opener->ch == '.' ||
                        (opener->ch == '<' && (opener->flags & MD_MARK_AUTOLINK_MISSING_MAILTO)))
                    {
                        dest_size += 7;
                        MD_TEMP_BUFFER(dest_size * sizeof(CHAR));
                        memcpy(ctx->buffer,
                                (opener->ch == '.' ? _T("http://") : _T("mailto:")),
                                7 * sizeof(CHAR));
                        memcpy(ctx->buffer + 7, dest, (dest_size-7) * sizeof(CHAR));
                        dest = ctx->buffer;
                    }

                    if(closer->flags & MD_MARK_VALIDPERMISSIVEAUTOLINK)
                        MD_CHECK(md_enter_leave_span_a(ctx, (mark->flags & MD_MARK_OPENER),
                                    MD_SPAN_A, dest, dest_size, TRUE, NULL, 0));
                    break;
                }

                case '&':
                    MD_TEXT(MD_TEXT_ENTITY, STR(mark->beg), mark->end - mark->beg);
                    break;

                case '\0':
                    MD_TEXT(MD_TEXT_NULLCHAR, _T(""), 1);
                    break;

                case 127:
                    goto abort;
            }

            off = mark->end;

            prev_mark = mark;
            mark++;
            while(!(mark->flags & MD_MARK_RESOLVED)  ||  mark->beg < off)
                mark++;
        }

        if(off >= line->end) {

            if(off >= end)
                break;

            if(text_type == MD_TEXT_CODE || text_type == MD_TEXT_LATEXMATH) {
                MD_ASSERT(prev_mark != NULL);
                MD_ASSERT(ISANYOF2_(prev_mark->ch, '`', '$')  &&  (prev_mark->flags & MD_MARK_OPENER));
                MD_ASSERT(ISANYOF2_(mark->ch, '`', '$')  &&  (mark->flags & MD_MARK_CLOSER));

                tmp = off;
                while(off < ctx->size  &&  ISBLANK(off))
                    off++;
                if(off > tmp)
                    MD_TEXT(text_type, STR(tmp), off-tmp);

                if(off == line->end)
                    MD_TEXT(text_type, _T(" "), 1);
            } else if(text_type == MD_TEXT_HTML) {

                tmp = off;
                while(tmp < end  &&  ISBLANK(tmp))
                    tmp++;
                if(tmp > off)
                    MD_TEXT(MD_TEXT_HTML, STR(off), tmp - off);
                MD_TEXT(MD_TEXT_HTML, _T("\n"), 1);
            } else {

                MD_TEXTTYPE break_type = MD_TEXT_SOFTBR;

                if(text_type == MD_TEXT_NORMAL) {
                    if(enforce_hardbreak  ||  (ctx->parser.flags & MD_FLAG_HARD_SOFT_BREAKS)) {
                        break_type = MD_TEXT_BR;
                    } else {
                        while(off < ctx->size  &&  ISBLANK(off))
                            off++;
                        if(off >= line->end + 2  &&  CH(off-2) == _T(' ')  &&  CH(off-1) == _T(' ')  &&  ISNEWLINE(off))
                            break_type = MD_TEXT_BR;
                    }
                }

                MD_TEXT(break_type, _T("\n"), 1);
            }

            line++;
            off = MAX(off, line->beg);

            enforce_hardbreak = 0;
        }
    }

abort:
    return ret;
}

static void
md_analyze_table_alignment(MD_CTX* ctx, OFF beg, OFF end, MD_ALIGN* align, int n_align)
{
    static const MD_ALIGN align_map[] = { MD_ALIGN_DEFAULT, MD_ALIGN_LEFT, MD_ALIGN_RIGHT, MD_ALIGN_CENTER };
    OFF off = beg;

    while(n_align > 0) {
        int index = 0;

        while(CH(off) != _T('-'))
            off++;
        if(off > beg  &&  CH(off-1) == _T(':'))
            index |= 1;
        while(off < end  &&  CH(off) == _T('-'))
            off++;
        if(off < end  &&  CH(off) == _T(':'))
            index |= 2;

        *align = align_map[index];
        align++;
        n_align--;
    }

}

static int md_process_normal_block_contents(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines);

static int
md_process_table_cell(MD_CTX* ctx, MD_BLOCKTYPE cell_type, MD_ALIGN align, OFF beg, OFF end)
{
    MD_LINE line;
    MD_BLOCK_TD_DETAIL det;
    int ret = 0;

    while(beg < end  &&  ISWHITESPACE(beg))
        beg++;
    while(end > beg  &&  ISWHITESPACE(end-1))
        end--;

    det.align = align;
    line.beg = beg;
    line.end = end;

    MD_ENTER_BLOCK(cell_type, &det);
    MD_CHECK(md_process_normal_block_contents(ctx, &line, 1));
    MD_LEAVE_BLOCK(cell_type, &det);

abort:
    return ret;
}

static int
md_process_table_row(MD_CTX* ctx, MD_BLOCKTYPE cell_type, OFF beg, OFF end,
                     const MD_ALIGN* align, int col_count)
{
    MD_LINE line;
    OFF* pipe_offs = NULL;
    int i, j, k, n;
    int ret = 0;

    line.beg = beg;
    line.end = end;

    MD_CHECK(md_analyze_inlines(ctx, &line, 1, TRUE));

    n = ctx->n_table_cell_boundaries + 2;
    pipe_offs = (OFF*) malloc(n * sizeof(OFF));
    if(pipe_offs == NULL) {
        MD_LOG("malloc() failed.");
        ret = -1;
        goto abort;
    }
    j = 0;
    pipe_offs[j++] = beg;
    for(i = ctx->table_cell_boundaries_head; i >= 0; i = ctx->marks[i].next) {
        MD_MARK* mark = &ctx->marks[i];
        pipe_offs[j++] = mark->end;
    }
    pipe_offs[j++] = end+1;

    MD_ENTER_BLOCK(MD_BLOCK_TR, NULL);
    k = 0;
    for(i = 0; i < j-1  &&  k < col_count; i++) {
        if(pipe_offs[i] < pipe_offs[i+1]-1)
            MD_CHECK(md_process_table_cell(ctx, cell_type, align[k++], pipe_offs[i], pipe_offs[i+1]-1));
    }

    while(k < col_count)
        MD_CHECK(md_process_table_cell(ctx, cell_type, align[k++], 0, 0));
    MD_LEAVE_BLOCK(MD_BLOCK_TR, NULL);

abort:
    free(pipe_offs);

    ctx->table_cell_boundaries_head = -1;
    ctx->table_cell_boundaries_tail = -1;

    return ret;
}

static int
md_process_table_block_contents(MD_CTX* ctx, int col_count, const MD_LINE* lines, MD_SIZE n_lines)
{
    MD_ALIGN* align;
    MD_SIZE line_index;
    int ret = 0;

    MD_ASSERT(n_lines >= 2);

    align = (MD_ALIGN*) malloc(col_count * sizeof(MD_ALIGN));
    if(align == NULL) {
        MD_LOG("malloc() failed.");
        ret = -1;
        goto abort;
    }

    md_analyze_table_alignment(ctx, lines[1].beg, lines[1].end, align, col_count);

    MD_ENTER_BLOCK(MD_BLOCK_THEAD, NULL);
    MD_CHECK(md_process_table_row(ctx, MD_BLOCK_TH,
                        lines[0].beg, lines[0].end, align, col_count));
    MD_LEAVE_BLOCK(MD_BLOCK_THEAD, NULL);

    if(n_lines > 2) {
        MD_ENTER_BLOCK(MD_BLOCK_TBODY, NULL);
        for(line_index = 2; line_index < n_lines; line_index++) {
            MD_CHECK(md_process_table_row(ctx, MD_BLOCK_TD,
                     lines[line_index].beg, lines[line_index].end, align, col_count));
        }
        MD_LEAVE_BLOCK(MD_BLOCK_TBODY, NULL);
    }

abort:
    free(align);
    return ret;
}

#define MD_BLOCK_CONTAINER_OPENER   0x01
#define MD_BLOCK_CONTAINER_CLOSER   0x02
#define MD_BLOCK_CONTAINER          (MD_BLOCK_CONTAINER_OPENER | MD_BLOCK_CONTAINER_CLOSER)
#define MD_BLOCK_LOOSE_LIST         0x04
#define MD_BLOCK_SETEXT_HEADER      0x08

struct MD_BLOCK_tag {
    MD_BLOCKTYPE type  :  8;
    unsigned flags     :  8;

    unsigned data      : 16;

    MD_SIZE n_lines;
};

struct MD_CONTAINER_tag {
    CHAR ch;
    unsigned is_loose    : 8;
    unsigned is_task     : 8;
    unsigned start;
    unsigned mark_indent;
    unsigned contents_indent;
    OFF block_byte_off;
    OFF task_mark_off;
};

static int
md_process_normal_block_contents(MD_CTX* ctx, const MD_LINE* lines, MD_SIZE n_lines)
{
    int i;
    int ret;

    MD_CHECK(md_analyze_inlines(ctx, lines, n_lines, FALSE));
    MD_CHECK(md_process_inlines(ctx, lines, n_lines));

abort:

    for(i = ctx->ptr_stack.top; i >= 0; i = ctx->marks[i].next)
        free(md_mark_get_ptr(ctx, i));
    ctx->ptr_stack.top = -1;

    return ret;
}

static int
md_process_verbatim_block_contents(MD_CTX* ctx, MD_TEXTTYPE text_type, const MD_VERBATIMLINE* lines, MD_SIZE n_lines)
{
    static const CHAR indent_chunk_str[] = _T("                ");
    static const SZ indent_chunk_size = SIZEOF_ARRAY(indent_chunk_str) - 1;

    MD_SIZE line_index;
    int ret = 0;

    for(line_index = 0; line_index < n_lines; line_index++) {
        const MD_VERBATIMLINE* line = &lines[line_index];
        int indent = line->indent;

        MD_ASSERT(indent >= 0);

        while(indent > (int) indent_chunk_size) {
            MD_TEXT(text_type, indent_chunk_str, indent_chunk_size);
            indent -= indent_chunk_size;
        }
        if(indent > 0)
            MD_TEXT(text_type, indent_chunk_str, indent);

        MD_TEXT_INSECURE(text_type, STR(line->beg), line->end - line->beg);

        MD_TEXT(text_type, _T("\n"), 1);
    }

abort:
    return ret;
}

static int
md_process_code_block_contents(MD_CTX* ctx, int is_fenced, const MD_VERBATIMLINE* lines, MD_SIZE n_lines)
{
    if(is_fenced) {

        lines++;
        n_lines--;
    } else {

        while(n_lines > 0  &&  lines[0].beg == lines[0].end) {
            lines++;
            n_lines--;
        }
        while(n_lines > 0  &&  lines[n_lines-1].beg == lines[n_lines-1].end) {
            n_lines--;
        }
    }

    if(n_lines == 0)
        return 0;

    return md_process_verbatim_block_contents(ctx, MD_TEXT_CODE, lines, n_lines);
}

static int
md_setup_fenced_code_detail(MD_CTX* ctx, const MD_BLOCK* block, MD_BLOCK_CODE_DETAIL* det,
                            MD_ATTRIBUTE_BUILD* info_build, MD_ATTRIBUTE_BUILD* lang_build)
{
    const MD_VERBATIMLINE* fence_line = (const MD_VERBATIMLINE*)(block + 1);
    OFF beg = fence_line->beg;
    OFF end = fence_line->end;
    OFF lang_end;
    CHAR fence_ch = CH(fence_line->beg);
    int ret = 0;

    while(beg < ctx->size  &&  CH(beg) == fence_ch)
        beg++;

    while(beg < ctx->size  &&  CH(beg) == _T(' '))
        beg++;

    while(end > beg  &&  CH(end-1) == _T(' '))
        end--;

    MD_CHECK(md_build_attribute(ctx, STR(beg), end - beg, 0, &det->info, info_build));

    lang_end = beg;
    while(lang_end < end  &&  !ISWHITESPACE(lang_end))
        lang_end++;
    MD_CHECK(md_build_attribute(ctx, STR(beg), lang_end - beg, 0, &det->lang, lang_build));

    det->fence_char = fence_ch;

abort:
    return ret;
}

static int
md_process_leaf_block(MD_CTX* ctx, const MD_BLOCK* block)
{
    union {
        MD_BLOCK_H_DETAIL header;
        MD_BLOCK_CODE_DETAIL code;
        MD_BLOCK_TABLE_DETAIL table;
    } det;
    MD_ATTRIBUTE_BUILD info_build = { 0 };
    MD_ATTRIBUTE_BUILD lang_build = { 0 };
    int is_in_tight_list;
    int clean_fence_code_detail = FALSE;
    int ret = 0;

    memset(&det, 0, sizeof(det));

    if(ctx->n_containers == 0)
        is_in_tight_list = FALSE;
    else
        is_in_tight_list = !ctx->containers[ctx->n_containers-1].is_loose;

    switch(block->type) {
        case MD_BLOCK_H:
            det.header.level = block->data;
            break;

        case MD_BLOCK_CODE:

            if(block->data != 0) {
                memset(&det.code, 0, sizeof(MD_BLOCK_CODE_DETAIL));
                clean_fence_code_detail = TRUE;
                MD_CHECK(md_setup_fenced_code_detail(ctx, block, &det.code, &info_build, &lang_build));
            }
            break;

        case MD_BLOCK_TABLE:
            det.table.col_count = block->data;
            det.table.head_row_count = 1;
            det.table.body_row_count = block->n_lines - 2;
            break;

        default:

            break;
    }

    if(!is_in_tight_list  ||  block->type != MD_BLOCK_P)
        MD_ENTER_BLOCK(block->type, (void*) &det);

    switch(block->type) {
        case MD_BLOCK_HR:

            break;

        case MD_BLOCK_CODE:
            MD_CHECK(md_process_code_block_contents(ctx, (block->data != 0),
                            (const MD_VERBATIMLINE*)(block + 1), block->n_lines));
            break;

        case MD_BLOCK_HTML:
            MD_CHECK(md_process_verbatim_block_contents(ctx, MD_TEXT_HTML,
                            (const MD_VERBATIMLINE*)(block + 1), block->n_lines));
            break;

        case MD_BLOCK_TABLE:
            MD_CHECK(md_process_table_block_contents(ctx, block->data,
                            (const MD_LINE*)(block + 1), block->n_lines));
            break;

        default:
            MD_CHECK(md_process_normal_block_contents(ctx,
                            (const MD_LINE*)(block + 1), block->n_lines));
            break;
    }

    if(!is_in_tight_list  ||  block->type != MD_BLOCK_P)
        MD_LEAVE_BLOCK(block->type, (void*) &det);

abort:
    if(clean_fence_code_detail) {
        md_free_attribute(ctx, &info_build);
        md_free_attribute(ctx, &lang_build);
    }
    return ret;
}

static int
md_process_all_blocks(MD_CTX* ctx)
{
    int byte_off = 0;
    int ret = 0;

    ctx->n_containers = 0;

    while(byte_off < ctx->n_block_bytes) {
        MD_BLOCK* block = (MD_BLOCK*)((char*)ctx->block_bytes + byte_off);
        union {
            MD_BLOCK_UL_DETAIL ul;
            MD_BLOCK_OL_DETAIL ol;
            MD_BLOCK_LI_DETAIL li;
        } det;

        switch(block->type) {
            case MD_BLOCK_UL:
                det.ul.is_tight = (block->flags & MD_BLOCK_LOOSE_LIST) ? FALSE : TRUE;
                det.ul.mark = (CHAR) block->data;
                break;

            case MD_BLOCK_OL:
                det.ol.start = block->n_lines;
                det.ol.is_tight =  (block->flags & MD_BLOCK_LOOSE_LIST) ? FALSE : TRUE;
                det.ol.mark_delimiter = (CHAR) block->data;
                break;

            case MD_BLOCK_LI:
                det.li.is_task = (block->data != 0);
                det.li.task_mark = (CHAR) block->data;
                det.li.task_mark_offset = (OFF) block->n_lines;
                break;

            default:

                break;
        }

        if(block->flags & MD_BLOCK_CONTAINER) {
            if(block->flags & MD_BLOCK_CONTAINER_CLOSER) {
                MD_LEAVE_BLOCK(block->type, &det);

                if(block->type == MD_BLOCK_UL || block->type == MD_BLOCK_OL || block->type == MD_BLOCK_QUOTE)
                    ctx->n_containers--;
            }

            if(block->flags & MD_BLOCK_CONTAINER_OPENER) {
                MD_ENTER_BLOCK(block->type, &det);

                if(block->type == MD_BLOCK_UL || block->type == MD_BLOCK_OL) {
                    ctx->containers[ctx->n_containers].is_loose = (block->flags & MD_BLOCK_LOOSE_LIST);
                    ctx->n_containers++;
                } else if(block->type == MD_BLOCK_QUOTE) {

                    ctx->containers[ctx->n_containers].is_loose = TRUE;
                    ctx->n_containers++;
                }
            }
        } else {
            MD_CHECK(md_process_leaf_block(ctx, block));

            if(block->type == MD_BLOCK_CODE || block->type == MD_BLOCK_HTML)
                byte_off += block->n_lines * sizeof(MD_VERBATIMLINE);
            else
                byte_off += block->n_lines * sizeof(MD_LINE);
        }

        byte_off += sizeof(MD_BLOCK);
    }

    ctx->n_block_bytes = 0;

abort:
    return ret;
}

static void*
md_push_block_bytes(MD_CTX* ctx, int n_bytes)
{
    void* ptr;

    if(ctx->n_block_bytes + n_bytes > ctx->alloc_block_bytes) {
        void* new_block_bytes;

        ctx->alloc_block_bytes = (ctx->alloc_block_bytes > 0
                ? ctx->alloc_block_bytes + ctx->alloc_block_bytes / 2
                : 512);
        new_block_bytes = realloc(ctx->block_bytes, ctx->alloc_block_bytes);
        if(new_block_bytes == NULL) {
            MD_LOG("realloc() failed.");
            return NULL;
        }

        if(ctx->current_block != NULL) {
            OFF off_current_block = (OFF) ((char*) ctx->current_block - (char*) ctx->block_bytes);
            ctx->current_block = (MD_BLOCK*) ((char*) new_block_bytes + off_current_block);
        }

        ctx->block_bytes = new_block_bytes;
    }

    ptr = (char*)ctx->block_bytes + ctx->n_block_bytes;
    ctx->n_block_bytes += n_bytes;
    return ptr;
}

static int
md_start_new_block(MD_CTX* ctx, const MD_LINE_ANALYSIS* line)
{
    MD_BLOCK* block;

    MD_ASSERT(ctx->current_block == NULL);

    block = (MD_BLOCK*) md_push_block_bytes(ctx, sizeof(MD_BLOCK));
    if(block == NULL)
        return -1;

    switch(line->type) {
        case MD_LINE_HR:
            block->type = MD_BLOCK_HR;
            break;

        case MD_LINE_ATXHEADER:
        case MD_LINE_SETEXTHEADER:
            block->type = MD_BLOCK_H;
            break;

        case MD_LINE_FENCEDCODE:
        case MD_LINE_INDENTEDCODE:
            block->type = MD_BLOCK_CODE;
            break;

        case MD_LINE_TEXT:
            block->type = MD_BLOCK_P;
            break;

        case MD_LINE_HTML:
            block->type = MD_BLOCK_HTML;
            break;

        case MD_LINE_BLANK:
        case MD_LINE_SETEXTUNDERLINE:
        case MD_LINE_TABLEUNDERLINE:
        default:
            MD_UNREACHABLE();
            break;
    }

    block->flags = 0;
    block->data = line->data;
    block->n_lines = 0;

    ctx->current_block = block;
    return 0;
}

static int
md_consume_link_reference_definitions(MD_CTX* ctx)
{
    MD_LINE* lines = (MD_LINE*) (ctx->current_block + 1);
    MD_SIZE n_lines = ctx->current_block->n_lines;
    MD_SIZE n = 0;

    while(n < n_lines) {
        int n_link_ref_lines;

        n_link_ref_lines = md_is_link_reference_definition(ctx,
                                    lines + n, n_lines - n);

        if(n_link_ref_lines == 0)
            break;

        if(n_link_ref_lines < 0)
            return -1;

        n += n_link_ref_lines;
    }

    if(n > 0) {
        if(n == n_lines) {

            ctx->n_block_bytes -= n * sizeof(MD_LINE);
            ctx->n_block_bytes -= sizeof(MD_BLOCK);
            ctx->current_block = NULL;
        } else {

            memmove(lines, lines + n, (n_lines - n) * sizeof(MD_LINE));
            ctx->current_block->n_lines -= n;
            ctx->n_block_bytes -= n * sizeof(MD_LINE);
        }
    }

    return 0;
}

static int
md_end_current_block(MD_CTX* ctx)
{
    int ret = 0;

    if(ctx->current_block == NULL)
        return ret;

    if(ctx->current_block->type == MD_BLOCK_P  ||
       (ctx->current_block->type == MD_BLOCK_H  &&  (ctx->current_block->flags & MD_BLOCK_SETEXT_HEADER)))
    {
        MD_LINE* lines = (MD_LINE*) (ctx->current_block + 1);
        if(lines[0].beg < ctx->size  &&  CH(lines[0].beg) == _T('[')) {
            MD_CHECK(md_consume_link_reference_definitions(ctx));
            if(ctx->current_block == NULL)
                return ret;
        }
    }

    if(ctx->current_block->type == MD_BLOCK_H  &&  (ctx->current_block->flags & MD_BLOCK_SETEXT_HEADER)) {
        MD_SIZE n_lines = ctx->current_block->n_lines;

        if(n_lines > 1) {

            ctx->current_block->n_lines--;
            ctx->n_block_bytes -= sizeof(MD_LINE);
        } else {

            ctx->current_block->type = MD_BLOCK_P;
            return 0;
        }
    }

    ctx->current_block = NULL;

abort:
    return ret;
}

static int
md_add_line_into_current_block(MD_CTX* ctx, const MD_LINE_ANALYSIS* analysis)
{
    MD_ASSERT(ctx->current_block != NULL);

    if(ctx->current_block->type == MD_BLOCK_CODE || ctx->current_block->type == MD_BLOCK_HTML) {
        MD_VERBATIMLINE* line;

        line = (MD_VERBATIMLINE*) md_push_block_bytes(ctx, sizeof(MD_VERBATIMLINE));
        if(line == NULL)
            return -1;

        line->indent = analysis->indent;
        line->beg = analysis->beg;
        line->end = analysis->end;
    } else {
        MD_LINE* line;

        line = (MD_LINE*) md_push_block_bytes(ctx, sizeof(MD_LINE));
        if(line == NULL)
            return -1;

        line->beg = analysis->beg;
        line->end = analysis->end;
    }
    ctx->current_block->n_lines++;

    return 0;
}

static int
md_push_container_bytes(MD_CTX* ctx, MD_BLOCKTYPE type, unsigned start,
                        unsigned data, unsigned flags)
{
    MD_BLOCK* block;
    int ret = 0;

    MD_CHECK(md_end_current_block(ctx));

    block = (MD_BLOCK*) md_push_block_bytes(ctx, sizeof(MD_BLOCK));
    if(block == NULL)
        return -1;

    block->type = type;
    block->flags = flags;
    block->data = data;
    block->n_lines = start;

abort:
    return ret;
}

static int
md_is_hr_line(MD_CTX* ctx, OFF beg, OFF* p_end, OFF* p_killer)
{
    OFF off = beg + 1;
    int n = 1;

    while(off < ctx->size  &&  (CH(off) == CH(beg) || CH(off) == _T(' ') || CH(off) == _T('\t'))) {
        if(CH(off) == CH(beg))
            n++;
        off++;
    }

    if(n < 3) {
        *p_killer = off;
        return FALSE;
    }

    if(off < ctx->size  &&  !ISNEWLINE(off)) {
        *p_killer = off;
        return FALSE;
    }

    *p_end = off;
    return TRUE;
}

static int
md_is_atxheader_line(MD_CTX* ctx, OFF beg, OFF* p_beg, OFF* p_end, unsigned* p_level)
{
    int n;
    OFF off = beg + 1;

    while(off < ctx->size  &&  CH(off) == _T('#')  &&  off - beg < 7)
        off++;
    n = off - beg;

    if(n > 6)
        return FALSE;
    *p_level = n;

    if(!(ctx->parser.flags & MD_FLAG_PERMISSIVEATXHEADERS)  &&  off < ctx->size  &&
       !ISBLANK(off)  &&  !ISNEWLINE(off))
        return FALSE;

    while(off < ctx->size  &&  ISBLANK(off))
        off++;
    *p_beg = off;
    *p_end = off;
    return TRUE;
}

static int
md_is_setext_underline(MD_CTX* ctx, OFF beg, OFF* p_end, unsigned* p_level)
{
    OFF off = beg + 1;

    while(off < ctx->size  &&  CH(off) == CH(beg))
        off++;

    while(off < ctx->size  &&  ISBLANK(off))
        off++;

    if(off < ctx->size  &&  !ISNEWLINE(off))
        return FALSE;

    *p_level = (CH(beg) == _T('=') ? 1 : 2);
    *p_end = off;
    return TRUE;
}

static int
md_is_table_underline(MD_CTX* ctx, OFF beg, OFF* p_end, unsigned* p_col_count)
{
    OFF off = beg;
    int found_pipe = FALSE;
    unsigned col_count = 0;

    if(off < ctx->size  &&  CH(off) == _T('|')) {
        found_pipe = TRUE;
        off++;
        while(off < ctx->size  &&  ISWHITESPACE(off))
            off++;
    }

    while(1) {
        int delimited = FALSE;

        if(off < ctx->size  &&  CH(off) == _T(':'))
            off++;
        if(off >= ctx->size  ||  CH(off) != _T('-'))
            return FALSE;
        while(off < ctx->size  &&  CH(off) == _T('-'))
            off++;
        if(off < ctx->size  &&  CH(off) == _T(':'))
            off++;

        col_count++;
        if(col_count > TABLE_MAXCOLCOUNT) {
            MD_LOG("Suppressing table (column_count >" STRINGIZE(TABLE_MAXCOLCOUNT) ")");
            return FALSE;
        }

        while(off < ctx->size  &&  ISWHITESPACE(off))
            off++;
        if(off < ctx->size  &&  CH(off) == _T('|')) {
            delimited = TRUE;
            found_pipe =  TRUE;
            off++;
            while(off < ctx->size  &&  ISWHITESPACE(off))
                off++;
        }

        if(off >= ctx->size  ||  ISNEWLINE(off))
            break;

        if(!delimited)
            return FALSE;
    }

    if(!found_pipe)
        return FALSE;

    *p_end = off;
    *p_col_count = col_count;
    return TRUE;
}

static int
md_is_opening_code_fence(MD_CTX* ctx, OFF beg, OFF* p_end)
{
    OFF off = beg;

    while(off < ctx->size && CH(off) == CH(beg))
        off++;

    if(off - beg < 3)
        return FALSE;

    ctx->code_fence_length = off - beg;

    while(off < ctx->size  &&  CH(off) == _T(' '))
        off++;

    while(off < ctx->size  &&  !ISNEWLINE(off)) {

        if(CH(beg) == _T('`')  &&  CH(off) == _T('`'))
            return FALSE;
        off++;
    }

    *p_end = off;
    return TRUE;
}

static int
md_is_closing_code_fence(MD_CTX* ctx, CHAR ch, OFF beg, OFF* p_end)
{
    OFF off = beg;
    int ret = FALSE;

    while(off < ctx->size  &&  CH(off) == ch)
        off++;
    if(off - beg < ctx->code_fence_length)
        goto out;

    while(off < ctx->size  &&  ISANYOF2(off, _T(' '), _T('\t')))
        off++;

    if(off < ctx->size  &&  !ISNEWLINE(off))
        goto out;

    ret = TRUE;

out:

    *p_end = off;
    return ret;
}

typedef struct TAG_tag TAG;
struct TAG_tag {
    const CHAR* name;
    unsigned len    : 8;
};

#ifdef X
    #undef X
#endif
#define X(name)     { _T(name), (sizeof(name)-1) / sizeof(CHAR) }
#define Xend        { NULL, 0 }

static const TAG t1[] = { X("pre"), X("script"), X("style"), X("textarea"), Xend };

static const TAG a6[] = { X("address"), X("article"), X("aside"), Xend };
static const TAG b6[] = { X("base"), X("basefont"), X("blockquote"), X("body"), Xend };
static const TAG c6[] = { X("caption"), X("center"), X("col"), X("colgroup"), Xend };
static const TAG d6[] = { X("dd"), X("details"), X("dialog"), X("dir"),
                          X("div"), X("dl"), X("dt"), Xend };
static const TAG f6[] = { X("fieldset"), X("figcaption"), X("figure"), X("footer"),
                          X("form"), X("frame"), X("frameset"), Xend };
static const TAG h6[] = { X("h1"), X("h2"), X("h3"), X("h4"), X("h5"), X("h6"),
                          X("head"), X("header"), X("hr"), X("html"), Xend };
static const TAG i6[] = { X("iframe"), Xend };
static const TAG l6[] = { X("legend"), X("li"), X("link"), Xend };
static const TAG m6[] = { X("main"), X("menu"), X("menuitem"), Xend };
static const TAG n6[] = { X("nav"), X("noframes"), Xend };
static const TAG o6[] = { X("ol"), X("optgroup"), X("option"), Xend };
static const TAG p6[] = { X("p"), X("param"), Xend };
static const TAG s6[] = { X("search"), X("section"), X("summary"), Xend };
static const TAG t6[] = { X("table"), X("tbody"), X("td"), X("tfoot"), X("th"),
                          X("thead"), X("title"), X("tr"), X("track"), Xend };
static const TAG u6[] = { X("ul"), Xend };
static const TAG xx[] = { Xend };

#undef X
#undef Xend

static int
md_is_html_block_start_condition(MD_CTX* ctx, OFF beg)
{

    static const TAG* map6[26] = {
        a6, b6, c6, d6, xx, f6, xx, h6, i6, xx, xx, l6, m6,
        n6, o6, p6, xx, xx, s6, t6, u6, xx, xx, xx, xx, xx
    };
    OFF off = beg + 1;
    int i;

    for(i = 0; t1[i].name != NULL; i++) {
        if(off + t1[i].len <= ctx->size) {
            if(md_ascii_case_eq(STR(off), t1[i].name, t1[i].len))
                return 1;
        }
    }

    if(off + 3 < ctx->size  &&  CH(off) == _T('!')  &&  CH(off+1) == _T('-')  &&  CH(off+2) == _T('-'))
        return 2;

    if(off < ctx->size  &&  CH(off) == _T('?'))
        return 3;

    if(off < ctx->size  &&  CH(off) == _T('!')) {

        if(off + 1 < ctx->size  &&  ISASCII(off+1))
            return 4;

        if(off + 8 < ctx->size) {
            if(md_ascii_eq(STR(off), _T("![CDATA["), 8))
                return 5;
        }
    }

    if(off + 1 < ctx->size  &&  (ISALPHA(off) || (CH(off) == _T('/') && ISALPHA(off+1)))) {
        int slot;
        const TAG* tags;

        if(CH(off) == _T('/'))
            off++;

        slot = (ISUPPER(off) ? CH(off) - 'A' : CH(off) - 'a');
        tags = map6[slot];

        for(i = 0; tags[i].name != NULL; i++) {
            if(off + tags[i].len <= ctx->size) {
                if(md_ascii_case_eq(STR(off), tags[i].name, tags[i].len)) {
                    OFF tmp = off + tags[i].len;
                    if(tmp >= ctx->size)
                        return 6;
                    if(ISBLANK(tmp) || ISNEWLINE(tmp) || CH(tmp) == _T('>'))
                        return 6;
                    if(tmp+1 < ctx->size && CH(tmp) == _T('/') && CH(tmp+1) == _T('>'))
                        return 6;
                    break;
                }
            }
        }
    }

    if(off + 1 < ctx->size) {
        OFF end;

        if(md_is_html_tag(ctx, NULL, 0, beg, ctx->size, &end)) {

            while(end < ctx->size  &&  ISWHITESPACE(end))
                end++;
            if(end >= ctx->size  ||  ISNEWLINE(end))
                return 7;
        }
    }

    return FALSE;
}

static int
md_line_contains(MD_CTX* ctx, OFF beg, const CHAR* what, SZ what_len, OFF* p_end)
{
    OFF i;
    for(i = beg; i + what_len < ctx->size; i++) {
        if(ISNEWLINE(i))
            break;
        if(memcmp(STR(i), what, what_len * sizeof(CHAR)) == 0) {
            *p_end = i + what_len;
            return TRUE;
        }
    }

    *p_end = i;
    return FALSE;
}

static int
md_is_html_block_end_condition(MD_CTX* ctx, OFF beg, OFF* p_end)
{
    switch(ctx->html_block_type) {
        case 1:
        {
            OFF off = beg;
            int i;

            while(off+1 < ctx->size  &&  !ISNEWLINE(off)) {
                if(CH(off) == _T('<')  &&  CH(off+1) == _T('/')) {
                    for(i = 0; t1[i].name != NULL; i++) {
                        if(off + 2 + t1[i].len < ctx->size) {
                            if(md_ascii_case_eq(STR(off+2), t1[i].name, t1[i].len)  &&
                               CH(off+2+t1[i].len) == _T('>'))
                            {
                                *p_end = off+2+t1[i].len+1;
                                return TRUE;
                            }
                        }
                    }
                }
                off++;
            }
            *p_end = off;
            return FALSE;
        }

        case 2:
            return (md_line_contains(ctx, beg, _T("-->"), 3, p_end) ? 2 : FALSE);

        case 3:
            return (md_line_contains(ctx, beg, _T("?>"), 2, p_end) ? 3 : FALSE);

        case 4:
            return (md_line_contains(ctx, beg, _T(">"), 1, p_end) ? 4 : FALSE);

        case 5:
            return (md_line_contains(ctx, beg, _T("]]>"), 3, p_end) ? 5 : FALSE);

        case 6:
        case 7:
            if(beg >= ctx->size  ||  ISNEWLINE(beg)) {

                *p_end = beg;
                return ctx->html_block_type;
            }
            return FALSE;

        default:
            MD_UNREACHABLE();
    }
    return FALSE;
}

static int
md_is_container_compatible(const MD_CONTAINER* pivot, const MD_CONTAINER* container)
{

    if(container->ch == _T('>'))
        return FALSE;

    if(container->ch != pivot->ch)
        return FALSE;
    if(container->mark_indent > pivot->contents_indent)
        return FALSE;

    return TRUE;
}

static int
md_push_container(MD_CTX* ctx, const MD_CONTAINER* container)
{
    if(ctx->n_containers >= ctx->alloc_containers) {
        MD_CONTAINER* new_containers;

        ctx->alloc_containers = (ctx->alloc_containers > 0
                ? ctx->alloc_containers + ctx->alloc_containers / 2
                : 16);
        new_containers = (MD_CONTAINER*) realloc(ctx->containers, ctx->alloc_containers * sizeof(MD_CONTAINER));
        if(new_containers == NULL) {
            MD_LOG("realloc() failed.");
            return -1;
        }

        ctx->containers = new_containers;
    }

    memcpy(&ctx->containers[ctx->n_containers++], container, sizeof(MD_CONTAINER));
    return 0;
}

static int
md_enter_child_containers(MD_CTX* ctx, int n_children)
{
    int i;
    int ret = 0;

    for(i = ctx->n_containers - n_children; i < ctx->n_containers; i++) {
        MD_CONTAINER* c = &ctx->containers[i];
        int is_ordered_list = FALSE;

        switch(c->ch) {
            case _T(')'):
            case _T('.'):
                is_ordered_list = TRUE;
                MD_FALLTHROUGH();

            case _T('-'):
            case _T('+'):
            case _T('*'):

                md_end_current_block(ctx);
                c->block_byte_off = ctx->n_block_bytes;

                MD_CHECK(md_push_container_bytes(ctx,
                                (is_ordered_list ? MD_BLOCK_OL : MD_BLOCK_UL),
                                c->start, c->ch, MD_BLOCK_CONTAINER_OPENER));
                MD_CHECK(md_push_container_bytes(ctx, MD_BLOCK_LI,
                                c->task_mark_off,
                                (c->is_task ? CH(c->task_mark_off) : 0),
                                MD_BLOCK_CONTAINER_OPENER));
                break;

            case _T('>'):
                MD_CHECK(md_push_container_bytes(ctx, MD_BLOCK_QUOTE, 0, 0, MD_BLOCK_CONTAINER_OPENER));
                break;

            default:
                MD_UNREACHABLE();
                break;
        }
    }

abort:
    return ret;
}

static int
md_leave_child_containers(MD_CTX* ctx, int n_keep)
{
    int ret = 0;

    while(ctx->n_containers > n_keep) {
        MD_CONTAINER* c = &ctx->containers[ctx->n_containers-1];
        int is_ordered_list = FALSE;

        switch(c->ch) {
            case _T(')'):
            case _T('.'):
                is_ordered_list = TRUE;
                MD_FALLTHROUGH();

            case _T('-'):
            case _T('+'):
            case _T('*'):
                MD_CHECK(md_push_container_bytes(ctx, MD_BLOCK_LI,
                                c->task_mark_off, (c->is_task ? CH(c->task_mark_off) : 0),
                                MD_BLOCK_CONTAINER_CLOSER));
                MD_CHECK(md_push_container_bytes(ctx,
                                (is_ordered_list ? MD_BLOCK_OL : MD_BLOCK_UL), 0,
                                c->ch, MD_BLOCK_CONTAINER_CLOSER));
                break;

            case _T('>'):
                MD_CHECK(md_push_container_bytes(ctx, MD_BLOCK_QUOTE, 0,
                                0, MD_BLOCK_CONTAINER_CLOSER));
                break;

            default:
                MD_UNREACHABLE();
                break;
        }

        ctx->n_containers--;
    }

abort:
    return ret;
}

static int
md_is_container_mark(MD_CTX* ctx, unsigned indent, OFF beg, OFF* p_end, MD_CONTAINER* p_container)
{
    OFF off = beg;
    OFF max_end;

    if(off >= ctx->size  ||  indent >= ctx->code_indent_offset)
        return FALSE;

    if(CH(off) == _T('>')) {
        off++;
        p_container->ch = _T('>');
        p_container->is_loose = FALSE;
        p_container->is_task = FALSE;
        p_container->mark_indent = indent;
        p_container->contents_indent = indent + 1;
        *p_end = off;
        return TRUE;
    }

    if(ISANYOF(off, _T("-+*"))  &&  (off+1 >= ctx->size || ISBLANK(off+1) || ISNEWLINE(off+1))) {
        p_container->ch = CH(off);
        p_container->is_loose = FALSE;
        p_container->is_task = FALSE;
        p_container->mark_indent = indent;
        p_container->contents_indent = indent + 1;
        *p_end = off+1;
        return TRUE;
    }

    max_end = off + 9;
    if(max_end > ctx->size)
        max_end = ctx->size;
    p_container->start = 0;
    while(off < max_end  &&  ISDIGIT(off)) {
        p_container->start = p_container->start * 10 + CH(off) - _T('0');
        off++;
    }
    if(off > beg  &&
       off < ctx->size  &&
       (CH(off) == _T('.') || CH(off) == _T(')'))  &&
       (off+1 >= ctx->size || ISBLANK(off+1) || ISNEWLINE(off+1)))
    {
        p_container->ch = CH(off);
        p_container->is_loose = FALSE;
        p_container->is_task = FALSE;
        p_container->mark_indent = indent;
        p_container->contents_indent = indent + off - beg + 1;
        *p_end = off+1;
        return TRUE;
    }

    return FALSE;
}

static unsigned
md_line_indentation(MD_CTX* ctx, unsigned total_indent, OFF beg, OFF* p_end)
{
    OFF off = beg;
    unsigned indent = total_indent;

    while(off < ctx->size  &&  ISBLANK(off)) {
        if(CH(off) == _T('\t'))
            indent = (indent + 4) & ~3;
        else
            indent++;
        off++;
    }

    *p_end = off;
    return indent - total_indent;
}

static const MD_LINE_ANALYSIS md_dummy_blank_line = { MD_LINE_BLANK, 0, 0, 0, 0, 0 };

static int
md_analyze_line(MD_CTX* ctx, OFF beg, OFF* p_end,
                const MD_LINE_ANALYSIS* pivot_line, MD_LINE_ANALYSIS* line)
{
    unsigned total_indent = 0;
    int n_parents = 0;
    int n_brothers = 0;
    int n_children = 0;
    MD_CONTAINER container = { 0 };
    int prev_line_has_list_loosening_effect = ctx->last_line_has_list_loosening_effect;
    OFF off = beg;
    OFF hr_killer = 0;
    int ret = 0;

    line->indent = md_line_indentation(ctx, total_indent, off, &off);
    total_indent += line->indent;
    line->beg = off;
    line->enforce_new_block = FALSE;

    while(n_parents < ctx->n_containers) {
        MD_CONTAINER* c = &ctx->containers[n_parents];

        if(c->ch == _T('>')  &&  line->indent < ctx->code_indent_offset  &&
            off < ctx->size  &&  CH(off) == _T('>'))
        {

            off++;
            total_indent++;
            line->indent = md_line_indentation(ctx, total_indent, off, &off);
            total_indent += line->indent;

            if(line->indent > 0)
                line->indent--;

            line->beg = off;

        } else if(c->ch != _T('>')  &&  line->indent >= c->contents_indent) {

            line->indent -= c->contents_indent;
        } else {
            break;
        }

        n_parents++;
    }

    if(off >= ctx->size  ||  ISNEWLINE(off)) {

        if(n_brothers + n_children == 0) {
            while(n_parents < ctx->n_containers  &&  ctx->containers[n_parents].ch != _T('>'))
                n_parents++;
        }
    }

    while(TRUE) {

        if(pivot_line->type == MD_LINE_FENCEDCODE) {
            line->beg = off;

            if(line->indent < ctx->code_indent_offset) {
                if(md_is_closing_code_fence(ctx, CH(pivot_line->beg), off, &off)) {
                    line->type = MD_LINE_BLANK;
                    ctx->last_line_has_list_loosening_effect = FALSE;
                    break;
                }
            }

            if(n_parents == ctx->n_containers) {
                if(line->indent > pivot_line->indent)
                    line->indent -= pivot_line->indent;
                else
                    line->indent = 0;

                line->type = MD_LINE_FENCEDCODE;
                break;
            }
        }

        if(pivot_line->type == MD_LINE_HTML  &&  ctx->html_block_type > 0) {
            if(n_parents < ctx->n_containers) {

                ctx->html_block_type = 0;
            } else {
                int html_block_type;

                html_block_type = md_is_html_block_end_condition(ctx, off, &off);
                if(html_block_type > 0) {
                    MD_ASSERT(html_block_type == ctx->html_block_type);

                    ctx->html_block_type = 0;

                    if(html_block_type == 6 || html_block_type == 7) {
                        line->type = MD_LINE_BLANK;
                        line->indent = 0;
                        break;
                    }
                }

                line->type = MD_LINE_HTML;
                n_parents = ctx->n_containers;
                break;
            }
        }

        if(off >= ctx->size  ||  ISNEWLINE(off)) {
            if(pivot_line->type == MD_LINE_INDENTEDCODE  &&  n_parents == ctx->n_containers) {
                line->type = MD_LINE_INDENTEDCODE;
                if(line->indent > ctx->code_indent_offset)
                    line->indent -= ctx->code_indent_offset;
                else
                    line->indent = 0;
                ctx->last_line_has_list_loosening_effect = FALSE;
            } else {
                line->type = MD_LINE_BLANK;
                ctx->last_line_has_list_loosening_effect = (n_parents > 0  &&
                        n_brothers + n_children == 0  &&
                        ctx->containers[n_parents-1].ch != _T('>'));

    #if 1

                if(n_parents > 0  &&  ctx->containers[n_parents-1].ch != _T('>')  &&
                   n_brothers + n_children == 0  &&  ctx->current_block == NULL  &&
                   ctx->n_block_bytes > (int) sizeof(MD_BLOCK))
                {
                    MD_BLOCK* top_block = (MD_BLOCK*) ((char*)ctx->block_bytes + ctx->n_block_bytes - sizeof(MD_BLOCK));
                    if(top_block->type == MD_BLOCK_LI)
                        ctx->last_list_item_starts_with_two_blank_lines = TRUE;
                }
    #endif
            }
            break;
        } else {
    #if 1

            if(ctx->last_list_item_starts_with_two_blank_lines) {
                if(n_parents > 0  &&  n_parents == ctx->n_containers  &&
                   ctx->containers[n_parents-1].ch != _T('>')  &&
                   n_brothers + n_children == 0  &&  ctx->current_block == NULL  &&
                   ctx->n_block_bytes > (int) sizeof(MD_BLOCK))
                {
                    MD_BLOCK* top_block = (MD_BLOCK*) ((char*)ctx->block_bytes + ctx->n_block_bytes - sizeof(MD_BLOCK));
                    if(top_block->type == MD_BLOCK_LI) {
                        n_parents--;

                        line->indent = total_indent;
                        if(n_parents > 0)
                            line->indent -= MIN(line->indent, ctx->containers[n_parents-1].contents_indent);
                    }
                }

                ctx->last_list_item_starts_with_two_blank_lines = FALSE;
            }
    #endif
            ctx->last_line_has_list_loosening_effect = FALSE;
        }

        if(line->indent < ctx->code_indent_offset  &&  pivot_line->type == MD_LINE_TEXT
            &&  off < ctx->size  &&  ISANYOF2(off, _T('='), _T('-'))
            &&  (n_parents == ctx->n_containers))
        {
            unsigned level;

            if(md_is_setext_underline(ctx, off, &off, &level)) {
                line->type = MD_LINE_SETEXTUNDERLINE;
                line->data = level;
                break;
            }
        }

        if(line->indent < ctx->code_indent_offset
            &&  off < ctx->size  &&  off >= hr_killer
            &&  ISANYOF(off, _T("-_*")))
        {
            if(md_is_hr_line(ctx, off, &off, &hr_killer)) {
                line->type = MD_LINE_HR;
                break;
            }
        }

        if(n_parents < ctx->n_containers  &&  n_brothers + n_children == 0) {
            OFF tmp;

            if(md_is_container_mark(ctx, line->indent, off, &tmp, &container)  &&
               md_is_container_compatible(&ctx->containers[n_parents], &container))
            {
                pivot_line = &md_dummy_blank_line;

                off = tmp;

                total_indent += container.contents_indent - container.mark_indent;
                line->indent = md_line_indentation(ctx, total_indent, off, &off);
                total_indent += line->indent;
                line->beg = off;

                if(off >= ctx->size || ISNEWLINE(off)) {
                    container.contents_indent++;
                } else if(line->indent <= ctx->code_indent_offset) {
                    container.contents_indent += line->indent;
                    line->indent = 0;
                } else {
                    container.contents_indent += 1;
                    line->indent--;
                }

                ctx->containers[n_parents].mark_indent = container.mark_indent;
                ctx->containers[n_parents].contents_indent = container.contents_indent;

                n_brothers++;
                continue;
            }
        }

        if(line->indent >= ctx->code_indent_offset  &&  (pivot_line->type != MD_LINE_TEXT)) {
            line->type = MD_LINE_INDENTEDCODE;
            line->indent -= ctx->code_indent_offset;
            line->data = 0;
            break;
        }

        if(line->indent < ctx->code_indent_offset  &&
           md_is_container_mark(ctx, line->indent, off, &off, &container))
        {
            if(pivot_line->type == MD_LINE_TEXT  &&  n_parents == ctx->n_containers  &&
                        (off >= ctx->size || ISNEWLINE(off))  &&  container.ch != _T('>'))
            {

            } else if(pivot_line->type == MD_LINE_TEXT  &&  n_parents == ctx->n_containers  &&
                        ISANYOF2_(container.ch, _T('.'), _T(')'))  &&  container.start != 1)
            {

            } else {
                total_indent += container.contents_indent - container.mark_indent;
                line->indent = md_line_indentation(ctx, total_indent, off, &off);
                total_indent += line->indent;

                line->beg = off;
                line->data = container.ch;

                if(off >= ctx->size || ISNEWLINE(off)) {
                    container.contents_indent++;
                } else if(line->indent <= ctx->code_indent_offset) {
                    container.contents_indent += line->indent;
                    line->indent = 0;
                } else {
                    container.contents_indent += 1;
                    line->indent--;
                }

                if(n_brothers + n_children == 0)
                    pivot_line = &md_dummy_blank_line;

                if(n_children == 0)
                    MD_CHECK(md_leave_child_containers(ctx, n_parents + n_brothers));

                n_children++;
                MD_CHECK(md_push_container(ctx, &container));
                continue;
            }
        }

        if(pivot_line->type == MD_LINE_TABLE  &&  n_parents == ctx->n_containers) {
            line->type = MD_LINE_TABLE;
            break;
        }

        if(line->indent < ctx->code_indent_offset  &&
                off < ctx->size  &&  CH(off) == _T('#'))
        {
            unsigned level;

            if(md_is_atxheader_line(ctx, off, &line->beg, &off, &level)) {
                line->type = MD_LINE_ATXHEADER;
                line->data = level;
                break;
            }
        }

        if(line->indent < ctx->code_indent_offset  &&
                off < ctx->size  &&  ISANYOF2(off, _T('`'), _T('~')))
        {
            if(md_is_opening_code_fence(ctx, off, &off)) {
                line->type = MD_LINE_FENCEDCODE;
                line->data = 1;
                line->enforce_new_block = TRUE;
                break;
            }
        }

        if(off < ctx->size  &&  CH(off) == _T('<')
            &&  !(ctx->parser.flags & MD_FLAG_NOHTMLBLOCKS))
        {
            ctx->html_block_type = md_is_html_block_start_condition(ctx, off);

            if(ctx->html_block_type == 7  &&  pivot_line->type == MD_LINE_TEXT)
                ctx->html_block_type = 0;

            if(ctx->html_block_type > 0) {

                if(md_is_html_block_end_condition(ctx, off, &off) == ctx->html_block_type) {

                    ctx->html_block_type = 0;
                }

                line->enforce_new_block = TRUE;
                line->type = MD_LINE_HTML;
                break;
            }
        }

        if((ctx->parser.flags & MD_FLAG_TABLES)  &&  pivot_line->type == MD_LINE_TEXT
            &&  off < ctx->size  &&  ISANYOF3(off, _T('|'), _T('-'), _T(':'))
            &&  n_parents == ctx->n_containers)
        {
            unsigned col_count;

            if(ctx->current_block != NULL  &&  ctx->current_block->n_lines == 1  &&
                md_is_table_underline(ctx, off, &off, &col_count))
            {
                line->data = col_count;
                line->type = MD_LINE_TABLEUNDERLINE;
                break;
            }
        }

        line->type = MD_LINE_TEXT;
        if(pivot_line->type == MD_LINE_TEXT  &&  n_brothers + n_children == 0) {

            n_parents = ctx->n_containers;
        }

        if((ctx->parser.flags & MD_FLAG_TASKLISTS)  &&  n_brothers + n_children > 0  &&
           ISANYOF_(ctx->containers[ctx->n_containers-1].ch, _T("-+*.)")))
        {
            OFF tmp = off;

            while(tmp < ctx->size  &&  tmp < off + 3  &&  ISBLANK(tmp))
                tmp++;
            if(tmp + 2 < ctx->size  &&  CH(tmp) == _T('[')  &&
               ISANYOF(tmp+1, _T("xX "))  &&  CH(tmp+2) == _T(']')  &&
               (tmp + 3 == ctx->size  ||  ISBLANK(tmp+3)  ||  ISNEWLINE(tmp+3)))
            {
                MD_CONTAINER* task_container = (n_children > 0 ? &ctx->containers[ctx->n_containers-1] : &container);
                task_container->is_task = TRUE;
                task_container->task_mark_off = tmp + 1;
                off = tmp + 3;
                while(off < ctx->size  &&  ISWHITESPACE(off))
                    off++;
                line->beg = off;
            }
        }

        break;
    }

    while(off + 3 < ctx->size  &&  !ISNEWLINE(off+0)  &&  !ISNEWLINE(off+1)
                               &&  !ISNEWLINE(off+2)  &&  !ISNEWLINE(off+3))
        off += 4;
    while(off < ctx->size  &&  !ISNEWLINE(off))
        off++;

    line->end = off;

    if(line->type == MD_LINE_ATXHEADER) {
        OFF tmp = line->end;
        while(tmp > line->beg && ISBLANK(tmp-1))
            tmp--;
        while(tmp > line->beg && CH(tmp-1) == _T('#'))
            tmp--;
        if(tmp == line->beg || ISBLANK(tmp-1) || (ctx->parser.flags & MD_FLAG_PERMISSIVEATXHEADERS))
            line->end = tmp;
    }

    if(line->type != MD_LINE_INDENTEDCODE  &&  line->type != MD_LINE_FENCEDCODE  && line->type != MD_LINE_HTML) {
        while(line->end > line->beg && ISBLANK(line->end-1))
            line->end--;
    }

    if(off < ctx->size && CH(off) == _T('\r'))
        off++;
    if(off < ctx->size && CH(off) == _T('\n'))
        off++;

    *p_end = off;

    if(prev_line_has_list_loosening_effect  &&  line->type != MD_LINE_BLANK  &&  n_parents + n_brothers > 0) {
        MD_CONTAINER* c = &ctx->containers[n_parents + n_brothers - 1];
        if(c->ch != _T('>')) {
            MD_BLOCK* block = (MD_BLOCK*) (((char*)ctx->block_bytes) + c->block_byte_off);
            block->flags |= MD_BLOCK_LOOSE_LIST;
        }
    }

    if(n_children == 0  &&  n_parents + n_brothers < ctx->n_containers)
        MD_CHECK(md_leave_child_containers(ctx, n_parents + n_brothers));

    if(n_brothers > 0) {
        MD_ASSERT(n_brothers == 1);
        MD_CHECK(md_push_container_bytes(ctx, MD_BLOCK_LI,
                    ctx->containers[n_parents].task_mark_off,
                    (ctx->containers[n_parents].is_task ? CH(ctx->containers[n_parents].task_mark_off) : 0),
                    MD_BLOCK_CONTAINER_CLOSER));
        MD_CHECK(md_push_container_bytes(ctx, MD_BLOCK_LI,
                    container.task_mark_off,
                    (container.is_task ? CH(container.task_mark_off) : 0),
                    MD_BLOCK_CONTAINER_OPENER));
        ctx->containers[n_parents].is_task = container.is_task;
        ctx->containers[n_parents].task_mark_off = container.task_mark_off;
    }

    if(n_children > 0)
        MD_CHECK(md_enter_child_containers(ctx, n_children));

abort:
    return ret;
}

static int
md_process_line(MD_CTX* ctx, const MD_LINE_ANALYSIS** p_pivot_line, MD_LINE_ANALYSIS* line)
{
    const MD_LINE_ANALYSIS* pivot_line = *p_pivot_line;
    int ret = 0;

    if(line->type == MD_LINE_BLANK) {
        MD_CHECK(md_end_current_block(ctx));
        *p_pivot_line = &md_dummy_blank_line;
        return 0;
    }

    if(line->enforce_new_block)
        MD_CHECK(md_end_current_block(ctx));

    if(line->type == MD_LINE_HR || line->type == MD_LINE_ATXHEADER) {
        MD_CHECK(md_end_current_block(ctx));

        MD_CHECK(md_start_new_block(ctx, line));
        MD_CHECK(md_add_line_into_current_block(ctx, line));
        MD_CHECK(md_end_current_block(ctx));
        *p_pivot_line = &md_dummy_blank_line;
        return 0;
    }

    if(line->type == MD_LINE_SETEXTUNDERLINE) {
        MD_ASSERT(ctx->current_block != NULL);
        ctx->current_block->type = MD_BLOCK_H;
        ctx->current_block->data = line->data;
        ctx->current_block->flags |= MD_BLOCK_SETEXT_HEADER;
        MD_CHECK(md_add_line_into_current_block(ctx, line));
        MD_CHECK(md_end_current_block(ctx));
        if(ctx->current_block == NULL) {
            *p_pivot_line = &md_dummy_blank_line;
        } else {

            line->type = MD_LINE_TEXT;
            *p_pivot_line = line;
        }
        return 0;
    }

    if(line->type == MD_LINE_TABLEUNDERLINE) {
        MD_ASSERT(ctx->current_block != NULL);
        MD_ASSERT(ctx->current_block->n_lines == 1);
        ctx->current_block->type = MD_BLOCK_TABLE;
        ctx->current_block->data = line->data;
        MD_ASSERT(pivot_line != &md_dummy_blank_line);
        ((MD_LINE_ANALYSIS*)pivot_line)->type = MD_LINE_TABLE;
        MD_CHECK(md_add_line_into_current_block(ctx, line));
        return 0;
    }

    if(line->type != pivot_line->type)
        MD_CHECK(md_end_current_block(ctx));

    if(ctx->current_block == NULL) {
        MD_CHECK(md_start_new_block(ctx, line));
        *p_pivot_line = line;
    }

    MD_CHECK(md_add_line_into_current_block(ctx, line));

abort:
    return ret;
}

static int
md_process_doc(MD_CTX *ctx)
{
    const MD_LINE_ANALYSIS* pivot_line = &md_dummy_blank_line;
    MD_LINE_ANALYSIS line_buf[2];
    MD_LINE_ANALYSIS* line = &line_buf[0];
    OFF off = 0;
    int ret = 0;

    MD_ENTER_BLOCK(MD_BLOCK_DOC, NULL);

    while(off < ctx->size) {
        if(line == pivot_line)
            line = (line == &line_buf[0] ? &line_buf[1] : &line_buf[0]);

        MD_CHECK(md_analyze_line(ctx, off, &off, pivot_line, line));
        MD_CHECK(md_process_line(ctx, &pivot_line, line));
    }

    md_end_current_block(ctx);

    MD_CHECK(md_build_ref_def_hashtable(ctx));

    MD_CHECK(md_leave_child_containers(ctx, 0));
    MD_CHECK(md_process_all_blocks(ctx));

    MD_LEAVE_BLOCK(MD_BLOCK_DOC, NULL);

abort:

#if 0

    {
        char buffer[256];
        sprintf(buffer, "Alloced %u bytes for block buffer.",
                    (unsigned)(ctx->alloc_block_bytes));
        MD_LOG(buffer);

        sprintf(buffer, "Alloced %u bytes for containers buffer.",
                    (unsigned)(ctx->alloc_containers * sizeof(MD_CONTAINER)));
        MD_LOG(buffer);

        sprintf(buffer, "Alloced %u bytes for marks buffer.",
                    (unsigned)(ctx->alloc_marks * sizeof(MD_MARK)));
        MD_LOG(buffer);

        sprintf(buffer, "Alloced %u bytes for aux. buffer.",
                    (unsigned)(ctx->alloc_buffer * sizeof(MD_CHAR)));
        MD_LOG(buffer);
    }
#endif

    return ret;
}

int
md_parse(const MD_CHAR* text, MD_SIZE size, const MD_PARSER* parser, void* userdata)
{
    MD_CTX ctx;
    int i;
    int ret;

    if(parser->abi_version != 0) {
        if(parser->debug_log != NULL)
            parser->debug_log("Unsupported abi_version.", userdata);
        return -1;
    }

    memset(&ctx, 0, sizeof(MD_CTX));
    ctx.text = text;
    ctx.size = size;
    memcpy(&ctx.parser, parser, sizeof(MD_PARSER));
    ctx.userdata = userdata;
    ctx.code_indent_offset = (ctx.parser.flags & MD_FLAG_NOINDENTEDCODEBLOCKS) ? (OFF)(-1) : 4;
    md_build_mark_char_map(&ctx);
    ctx.doc_ends_with_newline = (size > 0  &&  ISNEWLINE_(text[size-1]));
    ctx.max_ref_def_output = 16 * MIN(size, (MD_SIZE)(1024 * 1024 / 16));

    for(i = 0; i < (int) SIZEOF_ARRAY(ctx.opener_stacks); i++)
        ctx.opener_stacks[i].top = -1;
    ctx.ptr_stack.top = -1;
    ctx.unresolved_link_head = -1;
    ctx.unresolved_link_tail = -1;
    ctx.table_cell_boundaries_head = -1;
    ctx.table_cell_boundaries_tail = -1;

    ret = md_process_doc(&ctx);

    md_free_ref_defs(&ctx);
    md_free_ref_def_hashtable(&ctx);
    free(ctx.buffer);
    free(ctx.marks);
    free(ctx.block_bytes);
    free(ctx.containers);

    return ret;
}
