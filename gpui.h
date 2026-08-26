#ifndef GPUI_H_
#define GPUI_H_
#ifndef GPUI_AMALGAM
#define GPUI_AMALGAM 1
#endif
#define GPUI_MARKDOWN_FULL 1
#define GPUI_MARKDOWN_MINI 0

#line 1 "src/base.h"

#ifndef UNICODE
#define UNICODE
#endif

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <algorithm>
#include <utility>

#if defined(__EMSCRIPTEN__)
#define GPUI_OS_WINDOWS 0
#define GPUI_OS_LINUX 0
#define GPUI_OS_MAC 0
#define GPUI_OS_WASM 1
#elif defined(_WIN32)
#define GPUI_OS_WINDOWS 1
#define GPUI_OS_LINUX 0
#define GPUI_OS_MAC 0
#define GPUI_OS_WASM 0
#elif defined(__APPLE__)
#define GPUI_OS_WINDOWS 0
#define GPUI_OS_LINUX 0
#define GPUI_OS_MAC 1
#define GPUI_OS_WASM 0
#elif defined(__linux__)
#define GPUI_OS_WINDOWS 0
#define GPUI_OS_LINUX 1
#define GPUI_OS_MAC 0
#define GPUI_OS_WASM 0
#else
#error "unsupported platform: gpui builds on Windows, Linux, macOS and wasm"
#endif

#define GPUI_OS_POSIX (!GPUI_OS_WINDOWS)

#if GPUI_OS_WINDOWS
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#else
#include <pthread.h>
#include <limits.h>
#endif

namespace base {

enum {
    kMaxPath = 1024
};

struct Arena;

struct Str {
    char* s;
    int len;

    Str() : s(nullptr), len(0) {}

    explicit Str(const char* s_) : s((char*)s_), len(0) {
        len = s_ ? (int)strlen(s_) : 0;
    }
    explicit Str(const char* s_, int len_) : s((char*)s_), len(len_) {}
    explicit Str(char* s_) : s(s_), len(0) { len = s ? (int)strlen(s) : 0; }
    explicit Str(char* s_, int len_) : s(s_), len(len_) {}

    explicit operator bool() const { return len > 0 && s; }
};

void log(Str s);

using TempStr = Str;

#define StrL(lit) ::base::Str((char*)(lit), (int)(sizeof(lit) - 1))

Str AllocStrTemp(int size);

#if GPUI_OS_WINDOWS

WCHAR* ToCWstrTemp(Str s);
#endif

uint64_t PlatPageSize();
uint64_t PlatLargePageSize();

uint64_t PlatArenaReserveSize();

void* PlatMemReserve(uint64_t size);
bool PlatMemCommit(void* base, uint64_t size, bool largePages);
void* PlatMemReserveCommit(uint64_t size, bool largePages);
void PlatMemRelease(void* base, uint64_t size);

int StrCmpI(const char* a, const char* b);
int StrCmpNI(const char* a, const char* b, int n);

void StrCopyZ(char* dst, int cap, const char* src);

bool PlatDirExists(const char* path);

bool PlatFileExists(const char* path);
void PlatGetCwd(char* out, int cap);

void PlatGetExeDir(char* out, int cap);

struct DirEntry {
    char name[260] = {};
    bool isDir = false;
};

int PlatListDir(const char* dir, DirEntry* out, int max);

int PlatCoreCount();

bool PlatSelfUsage(uint64_t* cpu100ns, uint64_t* memBytes);

void* AllocZero(int count, int size);

template <typename T>
inline T* AllocArray(int n) {
    return (T*)AllocZero(n, (int)sizeof(T));
}

template <typename T>
inline void ZeroStruct(T* s) {
    memset((void*)s, 0, sizeof(T));
}

struct Func0 {

    static constexpr uintptr_t kFuncNoArg = ~(uintptr_t)1;

    void* fn = nullptr;
    uintptr_t userData = 0;

    Func0() = default;

    bool IsValid() const { return fn != nullptr; }
    void Call() const {
        if (!fn) {
            return;
        }
        if (userData == kFuncNoArg) {
            auto func = (void (*)())fn;
            func();
            return;
        }
        auto func = (void (*)(uintptr_t))fn;
        func(userData);
    }
};

template <typename T>
Func0 MkFunc0(void (*fn)(T*), T* d) {
    auto res = Func0{};
    res.fn = (void*)fn;
    res.userData = (uintptr_t)d;
    return res;
}

inline Func0 MkFunc0Void(void (*fn)()) {
    auto res = Func0{};
    res.fn = (void*)fn;
    res.userData = Func0::kFuncNoArg;
    return res;
}

template <typename T>
struct Func1 {

    static constexpr uintptr_t kDropsArgBit = 1;
    static constexpr uintptr_t kFuncNoArg = Func0::kFuncNoArg;

    void* fn = nullptr;
    uintptr_t userData = 0;

    Func1() = default;

    Func1(const Func0& that) {
        this->fn = that.fn;
        this->SetData(that.userData, true);
    }

    void SetData(uintptr_t d, bool dropsArg) {
        userData = d | (dropsArg ? kDropsArgBit : 0);
    }
    bool IsValid() const { return fn != nullptr; }
    void Call(T arg) const {
        if (!fn) {
            return;
        }
        uintptr_t d = userData & ~kDropsArgBit;
        if (userData & kDropsArgBit) {
            if (d == kFuncNoArg) {
                auto func = (void (*)())fn;
                func();
            } else {
                auto func = (void (*)(uintptr_t))fn;
                func(d);
            }
            return;
        }
        if (d == kFuncNoArg) {
            auto func = (void (*)(T))fn;
            func(arg);
            return;
        }
        auto func = (void (*)(uintptr_t, T))fn;
        func(d, arg);
    }
};

template <typename T1, typename T2>
Func1<T2> MkFunc1(void (*fn)(T1*, T2), T1* d) {
    auto res = Func1<T2>{};
    res.fn = (void*)fn;
    res.SetData((uintptr_t)d, false);
    return res;
}

template <typename T2>
Func1<T2> MkFunc1Void(void (*fn)(T2)) {
    auto res = Func1<T2>{};
    res.fn = (void*)fn;
    res.SetData(Func1<T2>::kFuncNoArg, false);
    return res;
}

struct Mutex {
#if GPUI_OS_WINDOWS
    SRWLOCK lock = SRWLOCK_INIT;
    void Lock() { AcquireSRWLockExclusive(&lock); }
    void Unlock() { ReleaseSRWLockExclusive(&lock); }
#else
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    void Lock() { pthread_mutex_lock(&lock); }
    void Unlock() { pthread_mutex_unlock(&lock); }
#endif
    Mutex() = default;
    ~Mutex() = default;
};

struct CondVar {
#if GPUI_OS_WINDOWS
    CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
    void Wait(Mutex* m, int timeoutMs) {
        DWORD t = timeoutMs < 0 ? INFINITE : (DWORD)timeoutMs;
        SleepConditionVariableSRW(&cv, &m->lock, t, 0);
    }
    void WakeOne() { WakeConditionVariable(&cv); }
    void WakeAll() { WakeAllConditionVariable(&cv); }
#else
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    void Wait(Mutex* m, int timeoutMs);
    void WakeOne() { pthread_cond_signal(&cv); }
    void WakeAll() { pthread_cond_broadcast(&cv); }
#endif
    CondVar() = default;
    ~CondVar() = default;
};

bool PlatThreadRun(Func0 f);

uint64_t PlatThreadId();
void PlatSleepMs(int ms);

static const uint64_t kArenaHeaderSize = 256;

struct Arena {
    Arena* prev;
    Arena* current;
    uint64_t flags;
    uint64_t commitChunkSize;
    uint64_t reserveChunkSize;
    uint64_t basePos;
    uint64_t pos;
    uint64_t committed;
    uint64_t reserved;
    const char* allocationSiteFile;
    int allocationSiteLine;
    const char* name;
    bool usesExternalBuffer;
    Mutex lock;
    uint64_t nAllocsLifetime;
    uint64_t peakBytesLifetime;
    uint64_t nAllocsSinceReset;
    uint64_t peakBytesSinceReset;

    void* Alloc(int size);
    void Reset();
    void* Push(uint64_t size, uint64_t align = 8, bool zero = true);
    void PopTo(uint64_t pos);

    Arena() = delete;
    ~Arena() = delete;
};

Arena* ArenaNew();
void ArenaDelete(Arena* arena);

uint64_t ArenaUsed(Arena* arena);

int VarintSize(uint32_t v);

int VarintPut(char* dst, uint32_t v);

int VarintGet(const char* src, uint32_t* out);

using ArenaStr = uint32_t;

constexpr ArenaStr kArenaStrNone = 0;

constexpr bool ArenaStrIsSet(ArenaStr s) {
    return s != kArenaStrNone;
}

ArenaStr ArenaStrDup(Arena* a, Str src);

uint32_t ArenaStrLen(Arena* a, ArenaStr s);

ArenaStr ArenaStrAppend(Arena* a, ArenaStr s, Str more);

Str ArenaStrGet(Arena* a, ArenaStr s);

constexpr uint32_t kArenaPtrNone = 0;

uint32_t ArenaOffsetOf(Arena* a, const void* p);

inline void* ArenaAtOffset(Arena* a, uint32_t off) {
    if (off == kArenaPtrNone || !a) {
        return nullptr;
    }
    Arena* node = a->current;

    if (node && node->basePos <= (uint64_t)off) {
        return (char*)node + ((uint64_t)off - node->basePos);
    }
    while (node && node->basePos > (uint64_t)off) {
        node = node->prev;
    }
    return node ? (char*)node + ((uint64_t)off - node->basePos) : nullptr;
}

template <typename T>
struct ArenaPtr {
    uint32_t off = kArenaPtrNone;

    bool IsSet() const { return off != kArenaPtrNone; }
    bool operator==(const ArenaPtr<T>& o) const { return off == o.off; }
    bool operator!=(const ArenaPtr<T>& o) const { return off != o.off; }
};

template <typename T>
ArenaPtr<T> ArenaPtrOf(Arena* a, const T* p) {
    return ArenaPtr<T>{ArenaOffsetOf(a, p)};
}

template <typename T>
T* ArenaPtrGet(Arena* a, ArenaPtr<T> p) {
    return (T*)ArenaAtOffset(a, p.off);
}

Arena* GetTempArena();
void ResetTempArena();
void DestroyTempArena();

void* Alloc(struct Arena* arena, int size);
void Free(struct Arena* arena, void* mem);

template <typename T, typename... Args>
T* ArenaNew(Arena* arena, Args&&... args) {
    void* mem = Alloc(arena, (int)sizeof(T));
    return new (mem) T(std::forward<Args>(args)...);
}

#if GPUI_OS_WINDOWS
#define GPUI_NOINLINE __declspec(noinline)
#else
#define GPUI_NOINLINE __attribute__((noinline))
#endif

void* ArenaVecAlloc(struct Arena* a, int count, int elSize, int align,
                    int hdrSize = 0);

GPUI_NOINLINE bool VecRealloc(struct Arena* a, void** els, int len, int* cap,
                              int newCap, int elSize);

#if defined(DEBUG)
int VecDbgBirth(const char* file, int line, const char* func, char kind,
                int elSize);
void VecDbgGrow(int id, int len, int oldCap, int needed, int newCap);
void VecDbgSegment(int id, int len, int want, int lastSegCap, int newSegCap,
                   int totalCap, bool reused);
void VecDbgDeath(int id, int len, int cap);
void VecDbgArenaDeath(int id, int len, int totalCap, int segCount);

#define GPUI_VEC_DBG_ARGS0                                            \
    const char *dbgF = __builtin_FILE(), int dbgL = __builtin_LINE(), \
               const char *dbgFn = __builtin_FUNCTION()
#define GPUI_VEC_DBG_ARGS , GPUI_VEC_DBG_ARGS0
#define GPUI_VEC_DBG_INIT(kind) \
    : dbgId(VecDbgBirth(dbgF, dbgL, dbgFn, kind, (int)sizeof(T)))
#else
#define GPUI_VEC_DBG_ARGS0
#define GPUI_VEC_DBG_ARGS
#define GPUI_VEC_DBG_INIT(kind)
#endif

template <typename T>
struct Vec;

template <typename T>
bool VecReserve(Arena* arena, T& v, int wantedSize);

template <typename T>
inline T* VecReserve(Vec<T>& v, int capNeeded);

template <typename T>
T* VecInsertSpace(Vec<T>& v, int idx, int count);

template <typename T>
struct Vec {
    int len = 0;

    int cap = 0;
    T* els = nullptr;
#if defined(DEBUG)
    int dbgId = 0;
#endif

    int Cap() const { return cap < 0 ? -cap : cap; }

    void FreeEls() {
        if (els) {
            if (cap > 0) {
                Free(nullptr, (void*)els);
            } else {

                cap = 0;
            }
            els = nullptr;
        }
    }

    void Reset() {
        FreeEls();
        len = 0;
        cap = 0;
    }

    void Clear() {
        len = 0;
        if (els && Cap() > 0) {
            memset((void*)els, 0, (size_t)Cap() * sizeof(T));
        }
    }

    explicit Vec(GPUI_VEC_DBG_ARGS0) GPUI_VEC_DBG_INIT('V') {}

    Vec(const Vec& other GPUI_VEC_DBG_ARGS) GPUI_VEC_DBG_INIT('V') {
        VecReserve(*this, other.len);
        len = other.len;
        if (other.len > 0 && other.els && els) {
            memcpy((void*)els, (const void*)other.els,
                   sizeof(T) * (size_t)other.len);
        }
    }

    Vec& operator=(const Vec& other) {
        if (this == &other) {
            return *this;
        }
        Reset();
        VecReserve(*this, other.len);
        len = other.len;
        if (other.len > 0) {
            memcpy((void*)els, (const void*)other.els, sizeof(T) * (size_t)len);
            memset((void*)(els + len), 0, sizeof(T) * (size_t)(Cap() - len));
        }
        return *this;
    }

    ~Vec() {
#if defined(DEBUG)
        VecDbgDeath(dbgId, len, Cap());
#endif
        FreeEls();
    }

    T& operator[](int idx) const { return els[idx]; }

    bool InsertAt(int idx, const T& el) {
        T* p = VecInsertSpace(*this, idx, 1);
        if (!p) {
            return false;
        }
        p[0] = el;
        return true;
    }

    bool Append(const T& el) { return InsertAt(len, el); }

    T* AppendBlanks(int count) { return VecInsertSpace(*this, len, count); }
};

inline int VecNextCap(int cap, int wanted, int elSize) {
    if (cap == 0) {
        int floorCap = elSize == 1 ? 8 : elSize <= 1024 ? 4 : 1;
        return std::max(floorCap, wanted);
    }
    return std::max(cap * 2, wanted);
}

template <typename T>
bool VecReserve(Arena* arena, T& v, int wantedSize) {

    int elSize = (int)sizeof(*v.els);
    int curCap = v.cap < 0 ? -v.cap : v.cap;
    if (wantedSize <= curCap) {
        return true;
    }
    int newCap = VecNextCap(curCap, wantedSize, elSize);
    if (v.cap < 0) {

        auto* borrowed = v.els;
        v.els = nullptr;
        v.cap = 0;
        if (!VecRealloc(arena, (void**)&v.els, 0, &v.cap, newCap, elSize)) {
            v.els = borrowed;
            v.cap = -curCap;
            return false;
        }
        if (v.len > 0) {
            memcpy((void*)v.els, (const void*)borrowed,
                   (size_t)v.len * (size_t)elSize);
        }
        return true;
    }
    return VecRealloc(arena, (void**)&v.els, v.len, &v.cap, newCap, elSize);
}

template <typename T, int N>
inline void VecUseInline(Vec<T>& v, T (&buf)[N]) {
    v.els = buf;
    v.cap = -N;
    v.len = 0;
}

template <typename T>
inline T* VecReserve(Vec<T>& v, int capNeeded) {
#if defined(DEBUG)

    if (capNeeded > v.Cap()) {
        VecDbgGrow(v.dbgId, v.len, v.Cap(), capNeeded,
                   VecNextCap(v.Cap(), capNeeded, (int)sizeof(T)));
    }
#endif
    if (!VecReserve(nullptr, v, capNeeded)) {
        return nullptr;
    }
    return v.els;
}

template <typename T>
T* VecInsertSpace(Vec<T>& v, int idx, int count) {
    int newLen = std::max(v.len, idx) + count;
    T* ok = VecReserve(v, newLen);
    if (!ok) {
        return nullptr;
    }
    T* res = &(v.els[idx]);
    if (v.len > idx) {
        T* src = v.els + idx;
        T* dst = v.els + idx + count;
        memmove((void*)dst, (const void*)src,
                (size_t)(v.len - idx) * sizeof(T));
    }
    v.len = newLen;
    return res;
}

template <typename T>
struct ArenaVecSegment {

    ArenaPtr<ArenaVecSegment<T>> next;

    int base;
    int len;

    int cap;

    static constexpr int HeaderSize() {
        return ((int)sizeof(ArenaVecSegment<T>) + (int)alignof(T) - 1) &
               ~((int)alignof(T) - 1);
    }

    T* Els() const { return (T*)((char*)(void*)this + HeaderSize()); }
};

constexpr int kArenaVecCap0 = 4;
constexpr int kArenaVecCap1 = 16;
constexpr int kArenaVecCap2 = 64;
constexpr int kArenaVecBytes0 = 64;
constexpr int kArenaVecBytes1 = 256;
constexpr int kArenaVecBytes2 = 1024;

template <typename T>
struct ArenaVec {
    using Segment = ArenaVecSegment<T>;

    Arena* a = nullptr;
    ArenaPtr<Segment> first = {};
    ArenaPtr<Segment> last = {};
    int len = 0;
#if defined(DEBUG)

    int dbgId = 0;

    int dbgTotalCap = 0;
    int dbgSegs = 0;

    ArenaVec(GPUI_VEC_DBG_ARGS0) GPUI_VEC_DBG_INIT('A') {}

    ~ArenaVec() { VecDbgArenaDeath(dbgId, len, dbgTotalCap, dbgSegs); }
#endif

    T& operator[](int idx) const {

        Segment* seg = ArenaPtrGet(a, first);
        if (first == last) {
            return seg->Els()[idx];
        }
        while (idx >= seg->base + seg->len) {
            seg = ArenaPtrGet(a, seg->next);
        }
        return seg->Els()[idx - seg->base];
    }

    struct Iter {

        Arena* a;

        Segment* seg;
        int idx;

        void Normalize() {
            while (seg && idx >= seg->len) {
                seg = ArenaPtrGet(a, seg->next);
                idx = 0;
            }
        }
        T& operator*() const { return seg->Els()[idx]; }
        T* operator->() const { return &seg->Els()[idx]; }
        Iter& operator++() {
            idx++;
            if (idx >= seg->len) {
                seg = ArenaPtrGet(a, seg->next);
                idx = 0;
                Normalize();
            }
            return *this;
        }
        bool operator!=(const Iter& o) const {
            return seg != o.seg || idx != o.idx;
        }
    };

    Iter begin() const {
        Iter it = {a, ArenaPtrGet(a, first), 0};
        it.Normalize();
        return it;
    }
    Iter end() const { return Iter{a, nullptr, 0}; }

    bool Append(Arena* arena, const T& el) {
        Segment* seg = ArenaPtrGet(a, last);
        if (!seg || seg->len >= seg->cap) {
            seg = NextSegment(arena, 1);
            if (!seg) {
                return false;
            }
        }
        seg->Els()[seg->len++] = el;
        len++;
        return true;
    }

    bool AppendMany(Arena* arena, const T* src, int n) {
        while (n > 0) {
            Segment* seg = ArenaPtrGet(a, last);
            if (!seg || seg->len >= seg->cap) {
                seg = NextSegment(arena, n);
                if (!seg) {
                    return false;
                }
            }
            int room = seg->cap - seg->len;
            int take = n < room ? n : room;
            for (int i = 0; i < take; i++) {
                seg->Els()[seg->len + i] = src[i];
            }
            seg->len += take;
            len += take;
            src += take;
            n -= take;
        }
        return true;
    }

    bool Reserve(Arena* arena, int n) {
        Segment* seg = ArenaPtrGet(a, last);
        if (seg && seg->cap - seg->len >= n) {
            return true;
        }
        return NextSegment(arena, n) != nullptr;
    }

    void Truncate(int newLen) {
        if (newLen < 0) {
            newLen = 0;
        }
        if (newLen >= len) {
            return;
        }
        ArenaPtr<Segment> at = first;
        Segment* seg = ArenaPtrGet(a, at);
        while (seg && seg->base + seg->len <= newLen) {
            at = seg->next;
            seg = ArenaPtrGet(a, at);
        }
        if (seg) {
            seg->len = newLen - seg->base;
            last = at;
            for (Segment* s = ArenaPtrGet(a, seg->next); s;
                 s = ArenaPtrGet(a, s->next)) {
                s->len = 0;
            }
        }
        len = newLen;
    }

    void Pop() { Truncate(len - 1); }

    T* Flatten(Arena* into) const {
        if (len == 0) {
            return nullptr;
        }
        if (first == last) {
            return ArenaPtrGet(a, first)->Els();
        }
        T* out = (T*)ArenaVecAlloc(into, len, (int)sizeof(T), (int)alignof(T));
        if (!out) {
            return nullptr;
        }
        int at = 0;
        for (const T& el : *this) {
            out[at++] = el;
        }
        return out;
    }

    static constexpr int CapFor(int count, int bytes) {
        return bytes / (int)sizeof(T) < count
                   ? (bytes / (int)sizeof(T) > 0 ? bytes / (int)sizeof(T) : 1)
                   : count;
    }

    static int NextCap(int prevCap) {
        if (prevCap < CapFor(kArenaVecCap0, kArenaVecBytes0)) {
            return CapFor(kArenaVecCap0, kArenaVecBytes0);
        }
        if (prevCap < CapFor(kArenaVecCap1, kArenaVecBytes1)) {
            return CapFor(kArenaVecCap1, kArenaVecBytes1);
        }
        if (prevCap < CapFor(kArenaVecCap2, kArenaVecBytes2)) {
            return CapFor(kArenaVecCap2, kArenaVecBytes2);
        }
        return prevCap * 2;
    }

    GPUI_NOINLINE Segment* NextSegment(Arena* arena, int want) {
        a = arena;
#if defined(DEBUG)
        if (dbgId == 0) {
            dbgId = VecDbgBirth("<no-constructor>", 0, "", 'A', (int)sizeof(T));
        }
#endif
        Segment* lastSeg = ArenaPtrGet(a, last);
#if defined(DEBUG)
        int dbgPrevCap = lastSeg ? lastSeg->cap : 0;
#endif
        ArenaPtr<Segment> reuseAt =
            lastSeg ? lastSeg->next : ArenaPtr<Segment>{};
        Segment* reuse = ArenaPtrGet(a, reuseAt);
        if (reuse && reuse->cap >= want) {
            reuse->len = 0;
            reuse->base = len;
            last = reuseAt;
#if defined(DEBUG)
            VecDbgSegment(dbgId, len, want, dbgPrevCap, reuse->cap, dbgTotalCap,
                          true);
#endif
            return reuse;
        }
        int cap = NextCap(lastSeg ? lastSeg->cap : 0);
        if (cap < want) {
            cap = want;
        }
        int align = (int)alignof(T) > 8 ? (int)alignof(T) : 8;
        void* mem =
            ArenaVecAlloc(a, cap, (int)sizeof(T), align, Segment::HeaderSize());
        if (!mem) {
            return nullptr;
        }
        Segment* seg = (Segment*)mem;
        seg->next = {};
        seg->base = len;
        seg->len = 0;
        seg->cap = cap;
        ArenaPtr<Segment> at = ArenaPtrOf(a, seg);
        if (lastSeg) {
            lastSeg->next = at;
        } else {
            first = at;
        }
        last = at;
#if defined(DEBUG)
        dbgTotalCap += cap;
        dbgSegs++;
        VecDbgSegment(dbgId, len, want, dbgPrevCap, cap, dbgTotalCap, false);
#endif
        return seg;
    }
};

struct PointF {
    float x = 0.0f;
    float y = 0.0f;

    static constexpr PointF Zero() { return {0.0f, 0.0f}; }
};

constexpr bool operator==(PointF a, PointF b) {
    return a.x == b.x && a.y == b.y;
}

constexpr bool operator!=(PointF a, PointF b) {
    return !(a == b);
}

constexpr PointF operator+(PointF a, PointF b) {
    return {a.x + b.x, a.y + b.y};
}

struct SizeF {
    float w = 0.0f;
    float h = 0.0f;

    static constexpr SizeF Zero() { return {0.0f, 0.0f}; }
};

constexpr bool operator==(SizeF a, SizeF b) {
    return a.w == b.w && a.h == b.h;
}

constexpr bool operator!=(SizeF a, SizeF b) {
    return !(a == b);
}

constexpr SizeF operator+(SizeF a, SizeF b) {
    return {a.w + b.w, a.h + b.h};
}

constexpr SizeF operator-(SizeF a, SizeF b) {
    return {a.w - b.w, a.h - b.h};
}

struct RectF {
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;

    static constexpr RectF Zero() { return {0.0f, 0.0f, 0.0f, 0.0f}; }
    static constexpr RectF New(float l, float r, float t, float b) {
        return {l, r, t, b};
    }

    constexpr float HorizontalAxisSum() const { return left + right; }
    constexpr float VerticalAxisSum() const { return top + bottom; }
    constexpr SizeF SumAxes() const { return {left + right, top + bottom}; }
};

constexpr bool operator==(RectF a, RectF b) {
    return a.left == b.left && a.right == b.right && a.top == b.top &&
           a.bottom == b.bottom;
}

constexpr bool operator!=(RectF a, RectF b) {
    return !(a == b);
}

constexpr RectF operator+(RectF a, RectF b) {
    return {a.left + b.left, a.right + b.right, a.top + b.top,
            a.bottom + b.bottom};
}

struct LocalDate {
    int year = 0;
    int month = 0;
    int day = 0;
};

LocalDate DateToday();

LocalDate DateAddDays(LocalDate base, int days);

void StrFree(Str s);
void StrFree(const char*) = delete;

Str StrDup(Arena*, Str str);
Str StrDup(Str s);

bool StrEqI(Str s1, Str s2);
bool StrContainsI(Str s, Str sub);

using SeqStrings = const char*;

Str SeqStrAt(SeqStrings strs, int off);

bool SeqStrAdvance(SeqStrings strs, int& off, int* idxInOut = nullptr);

int SeqStrIndex(SeqStrings strs, Str toFind);
int SeqStrIndexIS(SeqStrings strs, Str toFind);

Str SeqStrByIndex(SeqStrings strs, int idx);

int SeqStrCount(SeqStrings strs);

void StrLowerAscii(char* s);

struct StrBuilder {
    Arena* a = nullptr;
    char* els = nullptr;
    int len = 0;
    int cap = 0;
    Str buf;

    explicit StrBuilder(Str externalBuf = {});
    StrBuilder(const StrBuilder&) = delete;
    StrBuilder& operator=(const StrBuilder&) = delete;
    ~StrBuilder();

    void Reset(Str s = {});
    bool InsertAt(int idx, char el);
    bool AppendChar(char c);
    bool Append(Str src);
    Str TakeStr();
};

struct FmtArg {
    enum class Kind {
        Char,
        Int,
        Ptr,
        Float,
        Double,
        Str,
        RawStr,
        Any,
        None,
    };

    Kind t{Kind::None};
    union {
        Str str;
        char c;
        int64_t i;
        float f;
        double d;
        const void* ptr;
    };

    FmtArg() : i{0} {}
    explicit FmtArg(char c_) : t{Kind::Char}, c{c_} {}
    explicit FmtArg(int arg) : t{Kind::Int}, i{(int64_t)arg} {}
    explicit FmtArg(unsigned int arg) : t{Kind::Int}, i{(int64_t)arg} {}
    explicit FmtArg(long arg) : t{Kind::Int}, i{(int64_t)arg} {}
    explicit FmtArg(unsigned long arg) : t{Kind::Int}, i{(int64_t)arg} {}
    explicit FmtArg(long long arg) : t{Kind::Int}, i{(int64_t)arg} {}
    explicit FmtArg(unsigned long long arg) : t{Kind::Int}, i{(int64_t)arg} {}
    explicit FmtArg(float f_) : t{Kind::Float}, f{f_} {}
    explicit FmtArg(double d_) : t{Kind::Double}, d{d_} {}
    explicit FmtArg(Str arg) : t{Kind::Str}, str{arg} {}
    explicit FmtArg(const void* p) : t{Kind::Ptr}, ptr{p} {}
    FmtArg(char*) = delete;
    FmtArg(const char*) = delete;
    FmtArg(wchar_t*) = delete;
    FmtArg(const wchar_t*) = delete;
};

TempStr FormatTempArgs(const char* fmt, const FmtArg** args, int nArgs);

inline TempStr FormatTemp(const char* fmt) {
    return FormatTempArgs(fmt, nullptr, 0);
}

template <typename... TArgs>
TempStr FormatTemp(const char* fmt, const TArgs&... args) {
    const FmtArg argv[] = {FmtArg(args)...};
    const FmtArg* argp[sizeof...(TArgs)];
    int n = (int)sizeof...(TArgs);
    for (int i = 0; i < n; i++) {
        argp[i] = &argv[i];
    }
    return FormatTempArgs(fmt, argp, n);
}

template <typename... TArgs>
inline TempStr fmt(const char* format, const TArgs&... args) {
    return FormatTemp(format, args...);
}

template <typename... TArgs>
inline void logf(const char* format, const TArgs&... args) {
    log(FormatTemp(format, args...));
}
}

#line 1 "src/taffy/geometry.h"

#ifndef GPUI_TAFFY_GEOMETRY_H_
#define GPUI_TAFFY_GEOMETRY_H_

#include <cmath>
#include <cfloat>

namespace taffy {

using base::Arena;
using base::PointF;
using base::RectF;
using base::SizeF;
using base::Str;
using base::Vec;

using Optf = float;

inline constexpr uint32_t kOptfNoneBits = 0x7fc0beefu;

constexpr Optf None() {
    return __builtin_bit_cast(float, kOptfNoneBits);
}

constexpr Optf Some(float v) {
    return v;
}

constexpr bool IsNone(Optf v) {
    return __builtin_bit_cast(uint32_t, v) == kOptfNoneBits;
}

constexpr bool IsSome(Optf v) {
    return __builtin_bit_cast(uint32_t, v) != kOptfNoneBits;
}

constexpr float Unwrap(Optf v) {
    return v;
}

constexpr float UnwrapOr(Optf v, float alt) {
    return IsSome(v) ? v : alt;
}

constexpr Optf Or(Optf v, Optf alt) {
    return IsSome(v) ? v : alt;
}

constexpr bool OptfEq(Optf a, Optf b) {
    return IsNone(a) ? IsNone(b) : (a == b);
}

constexpr bool OptfNe(Optf a, Optf b) {
    return !OptfEq(a, b);
}

inline bool F32IsNan(float v) {
    return (__builtin_bit_cast(uint32_t, v) & 0x7fffffffu) > 0x7f800000u;
}

inline float F32Max(float a, float b) {
    if (F32IsNan(a)) {
        return b;
    }
    if (F32IsNan(b)) {
        return a;
    }
    return a > b ? a : b;
}

inline float F32Min(float a, float b) {
    if (F32IsNan(a)) {
        return b;
    }
    if (F32IsNan(b)) {
        return a;
    }
    return a < b ? a : b;
}

inline float F32Round(float v) {
    return floorf(v + 0.5f);
}

enum class AbsoluteAxis : uint8_t {
    Horizontal,
    Vertical
};

constexpr AbsoluteAxis OtherAxis(AbsoluteAxis axis) {
    return axis == AbsoluteAxis::Horizontal ? AbsoluteAxis::Vertical
                                            : AbsoluteAxis::Horizontal;
}

enum class AbstractAxis : uint8_t {
    Inline,
    Block
};

constexpr AbstractAxis Other(AbstractAxis axis) {
    return axis == AbstractAxis::Inline ? AbstractAxis::Block
                                        : AbstractAxis::Inline;
}

constexpr AbsoluteAxis AsAbsNaive(AbstractAxis axis) {
    return axis == AbstractAxis::Inline ? AbsoluteAxis::Horizontal
                                        : AbsoluteAxis::Vertical;
}

enum class FlexDirection : uint8_t {
    Row,
    Column,
    RowReverse,
    ColumnReverse
};

constexpr bool IsRow(FlexDirection d) {
    return d == FlexDirection::Row || d == FlexDirection::RowReverse;
}

constexpr bool IsColumn(FlexDirection d) {
    return d == FlexDirection::Column || d == FlexDirection::ColumnReverse;
}

constexpr bool IsReverse(FlexDirection d) {
    return d == FlexDirection::RowReverse || d == FlexDirection::ColumnReverse;
}

constexpr AbsoluteAxis MainAxis(FlexDirection d) {
    return IsRow(d) ? AbsoluteAxis::Horizontal : AbsoluteAxis::Vertical;
}

constexpr AbsoluteAxis CrossAxis(FlexDirection d) {
    return IsRow(d) ? AbsoluteAxis::Vertical : AbsoluteAxis::Horizontal;
}

constexpr float GetAbs(SizeF s, AbsoluteAxis a) {
    return a == AbsoluteAxis::Horizontal ? s.w : s.h;
}
constexpr float Get(SizeF s, AbstractAxis a) {
    return a == AbstractAxis::Inline ? s.w : s.h;
}
constexpr void Set(SizeF* s, AbstractAxis a, float v) {
    if (a == AbstractAxis::Inline) {
        s->w = v;
    } else {
        s->h = v;
    }
}
constexpr float Main(SizeF s, FlexDirection d) {
    return IsRow(d) ? s.w : s.h;
}
constexpr float Cross(SizeF s, FlexDirection d) {
    return IsRow(d) ? s.h : s.w;
}
constexpr void SetMain(SizeF* s, FlexDirection d, float v) {
    if (IsRow(d)) {
        s->w = v;
    } else {
        s->h = v;
    }
}
constexpr void SetCross(SizeF* s, FlexDirection d, float v) {
    if (IsRow(d)) {
        s->h = v;
    } else {
        s->w = v;
    }
}
constexpr SizeF WithMain(SizeF s, FlexDirection d, float v) {
    SetMain(&s, d, v);
    return s;
}
constexpr SizeF WithCross(SizeF s, FlexDirection d, float v) {
    SetCross(&s, d, v);
    return s;
}

inline SizeF Max(SizeF a, SizeF b) {
    return {F32Max(a.w, b.w), F32Max(a.h, b.h)};
}
inline SizeF Min(SizeF a, SizeF b) {
    return {F32Min(a.w, b.w), F32Min(a.h, b.h)};
}
constexpr bool HasNonZeroArea(SizeF s) {
    return s.w > 0.0f && s.h > 0.0f;
}

constexpr float Get(PointF p, AbstractAxis a) {
    return a == AbstractAxis::Inline ? p.x : p.y;
}
constexpr void Set(PointF* p, AbstractAxis a, float v) {
    if (a == AbstractAxis::Inline) {
        p->x = v;
    } else {
        p->y = v;
    }
}
constexpr float Main(PointF p, FlexDirection d) {
    return IsRow(d) ? p.x : p.y;
}
constexpr float Cross(PointF p, FlexDirection d) {
    return IsRow(d) ? p.y : p.x;
}
constexpr PointF Transpose(PointF p) {
    return {p.y, p.x};
}
constexpr SizeF IntoSize(PointF p) {
    return {p.x, p.y};
}

constexpr float GridAxisSum(RectF r, AbsoluteAxis a) {
    return a == AbsoluteAxis::Horizontal ? r.left + r.right : r.top + r.bottom;
}
constexpr float MainAxisSum(RectF r, FlexDirection d) {
    return IsRow(d) ? r.left + r.right : r.top + r.bottom;
}
constexpr float CrossAxisSum(RectF r, FlexDirection d) {
    return IsRow(d) ? r.top + r.bottom : r.left + r.right;
}
constexpr float MainStart(RectF r, FlexDirection d) {
    return IsRow(d) ? r.left : r.top;
}
constexpr float MainEnd(RectF r, FlexDirection d) {
    return IsRow(d) ? r.right : r.bottom;
}
constexpr float CrossStart(RectF r, FlexDirection d) {
    return IsRow(d) ? r.top : r.left;
}
constexpr float CrossEnd(RectF r, FlexDirection d) {
    return IsRow(d) ? r.bottom : r.right;
}

using SizeFOpt = SizeF;
using PointFOpt = PointF;
using RectFOpt = RectF;

constexpr SizeFOpt SizeFOptNone() {
    return {None(), None()};
}

constexpr PointFOpt PointFOptNone() {
    return {None(), None()};
}

constexpr RectFOpt RectFOptNone() {
    return {None(), None(), None(), None()};
}

constexpr SizeFOpt SizeFOptFromCross(FlexDirection d, Optf v) {
    return IsRow(d) ? SizeFOpt{None(), v} : SizeFOpt{v, None()};
}

constexpr SizeF UnwrapOr(SizeFOpt s, SizeF alt) {
    return {UnwrapOr(s.w, alt.w), UnwrapOr(s.h, alt.h)};
}

constexpr SizeFOpt Or(SizeFOpt s, SizeFOpt alt) {
    return {Or(s.w, alt.w), Or(s.h, alt.h)};
}

constexpr bool BothAxisDefined(SizeFOpt s) {
    return IsSome(s.w) && IsSome(s.h);
}

constexpr SizeFOpt MaybeApplyAspectRatio(SizeFOpt s, Optf aspectRatio) {
    if (IsNone(aspectRatio)) {
        return s;
    }
    if (IsSome(s.w) && IsNone(s.h)) {
        return {s.w, s.w / aspectRatio};
    }
    if (IsNone(s.w) && IsSome(s.h)) {
        return {s.h * aspectRatio, s.h};
    }
    return s;
}

constexpr bool SizeFOptEq(SizeFOpt a, SizeFOpt b) {
    return OptfEq(a.w, b.w) && OptfEq(a.h, b.h);
}

struct LineF {
    float start = 0.0f;
    float end = 0.0f;

    constexpr float Sum() const { return start + end; }
};

struct LineBool {
    bool start = false;
    bool end = false;

    static constexpr LineBool True() { return {true, true}; }
    static constexpr LineBool False() { return {false, false}; }
};

template <typename T>
struct Slice {
    T* els = nullptr;
    int len = 0;

    T& operator[](int i) const { return els[i]; }
    bool IsEmpty() const { return len == 0; }
    T* begin() const { return els; }
    T* end() const { return els + len; }
};

template <typename T>
Slice<T> SliceNew(Arena* a, int n) {
    if (n <= 0) {
        return {};
    }
    return {(T*)a->Alloc((int)sizeof(T) * n), n};
}

template <typename T>
Slice<T> SliceDup(Arena* a, const T* src, int n) {
    Slice<T> out = SliceNew<T>(a, n);
    if (out.els && src) {
        memcpy((void*)out.els, (const void*)src, sizeof(T) * (size_t)n);
    }
    return out;
}

template <typename T>
Slice<T> SliceOne(Arena* a, const T& v) {
    Slice<T> out = SliceNew<T>(a, 1);
    if (out.els) {
        out.els[0] = v;
    }
    return out;
}

}

#endif

#line 1 "src/taffy/style.h"

#ifndef GPUI_TAFFY_STYLE_H_
#define GPUI_TAFFY_STYLE_H_

namespace taffy {

struct CompactLength {
    uint64_t bits = 0;

    static constexpr uint64_t kCalcTag = 0;
    static constexpr uint64_t kLengthTag = 0x01;
    static constexpr uint64_t kPercentTag = 0x02;
    static constexpr uint64_t kAutoTag = 0x03;
    static constexpr uint64_t kFrTag = 0x04;
    static constexpr uint64_t kMinContentTag = 0x07;
    static constexpr uint64_t kMaxContentTag = 0x0f;
    static constexpr uint64_t kFitContentPxTag = 0x17;
    static constexpr uint64_t kFitContentPercentTag = 0x1f;

    static constexpr uint64_t kTagMask = 0xff;
    static constexpr uint64_t kCalcTagMask = 0x07;

    uint64_t Tag() const { return bits & kTagMask; }

    float Value() const {
        return __builtin_bit_cast(float, (uint32_t)(bits >> 32));
    }
    const void* CalcValue() const { return (const void*)(uintptr_t)bits; }

    bool IsCalc() const { return (bits & kCalcTagMask) == 0; }
    bool IsZero() const { return bits == FromVal(0.0f, kLengthTag).bits; }
    bool IsLengthOrPercentage() const {
        uint64_t t = Tag();
        return t == kLengthTag || t == kPercentTag;
    }
    bool IsAuto() const { return Tag() == kAutoTag; }
    bool IsMinContent() const { return Tag() == kMinContentTag; }
    bool IsMaxContent() const { return Tag() == kMaxContentTag; }
    bool IsFitContent() const {
        uint64_t t = Tag();
        return t == kFitContentPxTag || t == kFitContentPercentTag;
    }
    bool IsMaxOrFitContent() const {
        uint64_t t = Tag();
        return t == kMaxContentTag || t == kFitContentPxTag ||
               t == kFitContentPercentTag;
    }

    bool IsMaxContentAlike() const {
        uint64_t t = Tag();
        return t == kAutoTag || t == kMaxContentTag || t == kFitContentPxTag ||
               t == kFitContentPercentTag;
    }
    bool IsMinOrMaxContent() const {
        uint64_t t = Tag();
        return t == kMinContentTag || t == kMaxContentTag;
    }
    bool IsIntrinsic() const {
        uint64_t t = Tag();
        return t == kAutoTag || t == kMinContentTag || t == kMaxContentTag ||
               t == kFitContentPxTag || t == kFitContentPercentTag;
    }
    bool IsFr() const { return Tag() == kFrTag; }

    bool UsesPercentage() const {
        uint64_t t = Tag();
        return t == kPercentTag || t == kFitContentPercentTag || IsCalc();
    }

    static CompactLength FromVal(float v, uint64_t tag) {
        return CompactLength{((uint64_t)__builtin_bit_cast(uint32_t, v) << 32) |
                             tag};
    }
    static CompactLength FromTag(uint64_t tag) { return CompactLength{tag}; }
    static CompactLength FromPtr(const void* p) {
        return CompactLength{(uint64_t)(uintptr_t)p};
    }

    static CompactLength Length(float v) { return FromVal(v, kLengthTag); }

    static CompactLength Percent(float v) { return FromVal(v, kPercentTag); }
    static CompactLength Auto() { return FromTag(kAutoTag); }
    static CompactLength Fr(float v) { return FromVal(v, kFrTag); }
    static CompactLength MinContent() { return FromTag(kMinContentTag); }
    static CompactLength MaxContent() { return FromTag(kMaxContentTag); }
    static CompactLength FitContentPx(float limit) {
        return FromVal(limit, kFitContentPxTag);
    }
    static CompactLength FitContentPercent(float limit) {
        return FromVal(limit, kFitContentPercentTag);
    }
    static CompactLength Zero() { return Length(0.0f); }

    static CompactLength Calc(const void* p) { return FromPtr(p); }
};

inline bool operator==(CompactLength a, CompactLength b) {
    return a.bits == b.bits;
}

inline bool operator!=(CompactLength a, CompactLength b) {
    return a.bits != b.bits;
}

using CalcResolverFn = float (*)(const void* handle, float parentSize,
                                 void* ctx);

struct CalcResolver {
    CalcResolverFn fn = nullptr;
    void* ctx = nullptr;

    float Resolve(const void* handle, float parentSize) const {
        return fn ? fn(handle, parentSize, ctx) : 0.0f;
    }
};

inline Optf ResolvedPercentageSize(CompactLength cl, float parentSize,
                                   CalcResolver calc) {
    if (cl.Tag() == CompactLength::kPercentTag) {
        return Some(cl.Value() * parentSize);
    }
    if (cl.IsCalc()) {
        return Some(calc.Resolve(cl.CalcValue(), parentSize));
    }
    return None();
}

struct LengthPercentage {
    CompactLength raw = CompactLength::Zero();

    static LengthPercentage Length(float v) {
        return {CompactLength::Length(v)};
    }
    static LengthPercentage Percent(float v) {
        return {CompactLength::Percent(v)};
    }
    static LengthPercentage Calc(const void* p) {
        return {CompactLength::Calc(p)};
    }
    static LengthPercentage Zero() { return {CompactLength::Zero()}; }
};

struct LengthPercentageAuto {
    CompactLength raw = CompactLength::Zero();

    static LengthPercentageAuto Length(float v) {
        return {CompactLength::Length(v)};
    }
    static LengthPercentageAuto Percent(float v) {
        return {CompactLength::Percent(v)};
    }
    static LengthPercentageAuto Auto() { return {CompactLength::Auto()}; }
    static LengthPercentageAuto Calc(const void* p) {
        return {CompactLength::Calc(p)};
    }
    static LengthPercentageAuto Zero() { return {CompactLength::Zero()}; }
    static LengthPercentageAuto From(LengthPercentage lp) { return {lp.raw}; }

    bool IsAuto() const { return raw.IsAuto(); }

    Optf ResolveToOption(float context, CalcResolver calc) const;

    Optf MaybeResolve(Optf context, CalcResolver calc) const;
};

struct Dimension {
    CompactLength raw = CompactLength::Zero();

    static Dimension Length(float v) { return {CompactLength::Length(v)}; }
    static Dimension Percent(float v) { return {CompactLength::Percent(v)}; }
    static Dimension Auto() { return {CompactLength::Auto()}; }
    static Dimension Calc(const void* p) { return {CompactLength::Calc(p)}; }
    static Dimension Zero() { return {CompactLength::Zero()}; }
    static Dimension From(LengthPercentage lp) { return {lp.raw}; }
    static Dimension From(LengthPercentageAuto lp) { return {lp.raw}; }

    bool IsAuto() const { return raw.IsAuto(); }
    uint64_t Tag() const { return raw.Tag(); }
    float Value() const { return raw.Value(); }

    Optf MaybeResolve(Optf context, CalcResolver calc) const;

    Optf IntoOption() const {
        return raw.Tag() == CompactLength::kLengthTag ? Some(raw.Value())
                                                      : None();
    }
};

inline bool operator==(LengthPercentage a, LengthPercentage b) {
    return a.raw == b.raw;
}
inline bool operator!=(LengthPercentage a, LengthPercentage b) {
    return a.raw != b.raw;
}
inline bool operator==(LengthPercentageAuto a, LengthPercentageAuto b) {
    return a.raw == b.raw;
}
inline bool operator!=(LengthPercentageAuto a, LengthPercentageAuto b) {
    return a.raw != b.raw;
}
inline bool operator==(Dimension a, Dimension b) {
    return a.raw == b.raw;
}
inline bool operator!=(Dimension a, Dimension b) {
    return a.raw != b.raw;
}

struct AvailableSpace {
    enum class Kind : uint8_t {
        Definite,
        MinContent,
        MaxContent
    };

    Kind kind = Kind::MaxContent;
    float value = 0.0f;

    static AvailableSpace Definite(float v) { return {Kind::Definite, v}; }
    static AvailableSpace MinContent() { return {Kind::MinContent, 0.0f}; }
    static AvailableSpace MaxContent() { return {Kind::MaxContent, 0.0f}; }
    static AvailableSpace Zero() { return Definite(0.0f); }

    static AvailableSpace From(Optf v) {
        return IsSome(v) ? Definite(v) : MaxContent();
    }

    bool IsDefinite() const { return kind == Kind::Definite; }
    Optf IntoOption() const {
        return kind == Kind::Definite ? Some(value) : None();
    }
    float UnwrapOr(float def) const {
        return kind == Kind::Definite ? value : def;
    }

    float Unwrap() const { return value; }
    AvailableSpace Or(AvailableSpace def) const {
        return kind == Kind::Definite ? *this : def;
    }
    AvailableSpace MaybeSet(Optf v) const {
        return IsSome(v) ? Definite(v) : *this;
    }
    float ComputeFreeSpace(float usedSpace) const {
        switch (kind) {
            case Kind::MaxContent:
                return INFINITY;
            case Kind::MinContent:
                return 0.0f;
            default:
                return value - usedSpace;
        }
    }

    bool IsRoughlyEqual(AvailableSpace other) const {
        if (kind != other.kind) {
            return false;
        }
        if (kind != Kind::Definite) {
            return true;
        }
        return fabsf(value - other.value) < FLT_EPSILON;
    }
};

inline bool operator==(AvailableSpace a, AvailableSpace b) {
    return a.kind == b.kind &&
           (a.kind != AvailableSpace::Kind::Definite || a.value == b.value);
}

inline bool operator!=(AvailableSpace a, AvailableSpace b) {
    return !(a == b);
}

enum class AlignItemsKeyword : uint8_t {
    Start,
    End,
    FlexStart,
    FlexEnd,
    Center,
    Baseline,
    Stretch
};

enum class AlignContentKeyword : uint8_t {
    Start,
    End,
    FlexStart,
    FlexEnd,
    Center,
    Stretch,
    SpaceBetween,
    SpaceEvenly,
    SpaceAround
};

constexpr AlignContentKeyword Reversed(AlignContentKeyword k) {
    switch (k) {
        case AlignContentKeyword::Start:
            return AlignContentKeyword::End;
        case AlignContentKeyword::End:
            return AlignContentKeyword::Start;
        case AlignContentKeyword::FlexStart:
            return AlignContentKeyword::FlexEnd;
        case AlignContentKeyword::FlexEnd:
            return AlignContentKeyword::FlexStart;
        case AlignContentKeyword::Stretch:
            return AlignContentKeyword::End;
        default:
            return k;
    }
}

enum class AlignmentSafety : uint8_t {
    Unsafe,
    Safe
};

struct AlignItems {
    AlignItemsKeyword keyword = AlignItemsKeyword::Start;
    AlignmentSafety safety = AlignmentSafety::Unsafe;

    constexpr bool IsSafe() const { return safety == AlignmentSafety::Safe; }
    constexpr AlignItemsKeyword Keyword() const { return keyword; }
};

struct AlignContent {
    AlignContentKeyword keyword = AlignContentKeyword::Start;
    AlignmentSafety safety = AlignmentSafety::Unsafe;

    constexpr bool IsSafe() const { return safety == AlignmentSafety::Safe; }
    constexpr AlignContentKeyword Keyword() const { return keyword; }
};

constexpr bool operator==(AlignItems a, AlignItems b) {
    return a.keyword == b.keyword && a.safety == b.safety;
}
constexpr bool operator!=(AlignItems a, AlignItems b) {
    return !(a == b);
}
constexpr bool operator==(AlignContent a, AlignContent b) {
    return a.keyword == b.keyword && a.safety == b.safety;
}
constexpr bool operator!=(AlignContent a, AlignContent b) {
    return !(a == b);
}

using JustifyItems = AlignItems;
using AlignSelf = AlignItems;
using JustifySelf = AlignItems;
using JustifyContent = AlignContent;

struct OptAlignItems {
    AlignItems val;
    bool has = false;

    constexpr OptAlignItems() = default;
    constexpr explicit OptAlignItems(AlignItems v) : val(v), has(true) {}

    constexpr bool IsSome() const { return has; }
    constexpr AlignItems UnwrapOr(AlignItems alt) const {
        return has ? val : alt;
    }
    constexpr OptAlignItems Or(OptAlignItems alt) const {
        return has ? *this : alt;
    }
};

using OptAlignSelf = OptAlignItems;

struct OptAlignContent {
    AlignContent val;
    bool has = false;

    constexpr OptAlignContent() = default;
    constexpr explicit OptAlignContent(AlignContent v) : val(v), has(true) {}

    constexpr bool IsSome() const { return has; }
    constexpr AlignContent UnwrapOr(AlignContent alt) const {
        return has ? val : alt;
    }
};

using OptJustifyContent = OptAlignContent;

constexpr bool operator==(OptAlignItems a, OptAlignItems b) {
    return a.has == b.has && (!a.has || a.val == b.val);
}
constexpr bool operator!=(OptAlignItems a, OptAlignItems b) {
    return !(a == b);
}
constexpr bool operator==(OptAlignContent a, OptAlignContent b) {
    return a.has == b.has && (!a.has || a.val == b.val);
}
constexpr bool operator!=(OptAlignContent a, OptAlignContent b) {
    return !(a == b);
}

enum class Display : uint8_t {
    Block,
    Flex,
    Grid,
    None
};

enum class BoxGenerationMode : uint8_t {
    Normal,
    None
};

enum class Position : uint8_t {
    Relative,
    Absolute
};

enum class BoxSizing : uint8_t {
    BorderBox,
    ContentBox
};

enum class Overflow : uint8_t {
    Visible,
    Clip,
    Hidden,
    Scroll
};

constexpr bool IsScrollContainer(Overflow o) {
    return o == Overflow::Hidden || o == Overflow::Scroll;
}

constexpr Optf MaybeIntoAutomaticMinSize(Overflow o) {
    return IsScrollContainer(o) ? Some(0.0f) : None();
}

enum class Direction : uint8_t {
    Ltr,
    Rtl
};

constexpr bool IsRtl(Direction d) {
    return d == Direction::Rtl;
}

enum class TextAlign : uint8_t {
    Auto,
    LegacyLeft,
    LegacyRight,
    LegacyCenter
};

enum class FlexWrap : uint8_t {
    NoWrap,
    Wrap,
    WrapReverse
};

enum class Float : uint8_t {
    Left,
    Right,
    None
};

enum class FloatDirection : uint8_t {
    Left = 0,
    Right = 1
};

constexpr bool IsFloated(Float f) {
    return f == Float::Left || f == Float::Right;
}

struct OptFloatDirection {
    FloatDirection val = FloatDirection::Left;
    bool has = false;

    constexpr OptFloatDirection() = default;
    constexpr explicit OptFloatDirection(FloatDirection v)
        : val(v), has(true) {}

    constexpr bool IsSome() const { return has; }
};

constexpr OptFloatDirection FloatDir(Float f) {
    switch (f) {
        case Float::Left:
            return OptFloatDirection(FloatDirection::Left);
        case Float::Right:
            return OptFloatDirection(FloatDirection::Right);
        default:
            return OptFloatDirection();
    }
}

enum class Clear : uint8_t {
    Left,
    Right,
    Both,
    None
};

struct SizeDim {
    Dimension width = Dimension::Auto();
    Dimension height = Dimension::Auto();

    static SizeDim Auto() { return {}; }
    static SizeDim FromLengths(float w, float h) {
        return {Dimension::Length(w), Dimension::Length(h)};
    }
    static SizeDim FromPercent(float w, float h) {
        return {Dimension::Percent(w), Dimension::Percent(h)};
    }

    Dimension GetAbs(AbsoluteAxis a) const {
        return a == AbsoluteAxis::Horizontal ? width : height;
    }
    Dimension Get(AbstractAxis a) const {
        return a == AbstractAxis::Inline ? width : height;
    }
    Dimension Main(FlexDirection d) const { return IsRow(d) ? width : height; }
    Dimension Cross(FlexDirection d) const { return IsRow(d) ? height : width; }

    SizeFOpt MaybeResolve(SizeFOpt context, CalcResolver calc) const;
    SizeF ResolveOrZero(SizeFOpt context, CalcResolver calc) const;
};

struct SizeLp {
    LengthPercentage width = LengthPercentage::Zero();
    LengthPercentage height = LengthPercentage::Zero();

    static SizeLp Zero() { return {}; }

    LengthPercentage GetAbs(AbsoluteAxis a) const {
        return a == AbsoluteAxis::Horizontal ? width : height;
    }
    LengthPercentage Get(AbstractAxis a) const {
        return a == AbstractAxis::Inline ? width : height;
    }
    LengthPercentage Main(FlexDirection d) const {
        return IsRow(d) ? width : height;
    }
    LengthPercentage Cross(FlexDirection d) const {
        return IsRow(d) ? height : width;
    }
    SizeF ResolveOrZero(SizeFOpt context, CalcResolver calc) const;
    SizeF ResolveOrZero(Optf context, CalcResolver calc) const;
};

struct SizeAvail {
    AvailableSpace width = AvailableSpace::MaxContent();
    AvailableSpace height = AvailableSpace::MaxContent();

    static SizeAvail MaxContent() {
        return {AvailableSpace::MaxContent(), AvailableSpace::MaxContent()};
    }
    static SizeAvail MinContent() {
        return {AvailableSpace::MinContent(), AvailableSpace::MinContent()};
    }
    static SizeAvail Definite(SizeF s) {
        return {AvailableSpace::Definite(s.w),
                AvailableSpace::Definite(s.h)};
    }
    static SizeAvail From(SizeFOpt s) {
        return {AvailableSpace::From(s.w), AvailableSpace::From(s.h)};
    }

    AvailableSpace GetAbs(AbsoluteAxis a) const {
        return a == AbsoluteAxis::Horizontal ? width : height;
    }
    AvailableSpace Get(AbstractAxis a) const {
        return a == AbstractAxis::Inline ? width : height;
    }
    void Set(AbstractAxis a, AvailableSpace v) {
        if (a == AbstractAxis::Inline) {
            width = v;
        } else {
            height = v;
        }
    }
    AvailableSpace Main(FlexDirection d) const {
        return IsRow(d) ? width : height;
    }
    AvailableSpace Cross(FlexDirection d) const {
        return IsRow(d) ? height : width;
    }
    void SetMain(FlexDirection d, AvailableSpace v) {
        if (IsRow(d)) {
            width = v;
        } else {
            height = v;
        }
    }
    void SetCross(FlexDirection d, AvailableSpace v) {
        if (IsRow(d)) {
            height = v;
        } else {
            width = v;
        }
    }
    SizeFOpt IntoOptions() const {
        return {width.IntoOption(), height.IntoOption()};
    }
    SizeAvail MaybeSet(SizeFOpt v) const {
        return {width.MaybeSet(v.w), height.MaybeSet(v.h)};
    }
};

inline bool operator==(SizeAvail a, SizeAvail b) {
    return a.width == b.width && a.height == b.height;
}

inline bool operator!=(SizeAvail a, SizeAvail b) {
    return !(a == b);
}

struct RectLp {
    LengthPercentage left = LengthPercentage::Zero();
    LengthPercentage right = LengthPercentage::Zero();
    LengthPercentage top = LengthPercentage::Zero();
    LengthPercentage bottom = LengthPercentage::Zero();

    static RectLp Zero() { return {}; }

    RectF ResolveOrZero(SizeFOpt context, CalcResolver calc) const;
    RectF ResolveOrZero(Optf context, CalcResolver calc) const;
};

struct RectLpa {
    LengthPercentageAuto left = LengthPercentageAuto::Zero();
    LengthPercentageAuto right = LengthPercentageAuto::Zero();
    LengthPercentageAuto top = LengthPercentageAuto::Zero();
    LengthPercentageAuto bottom = LengthPercentageAuto::Zero();

    static RectLpa Zero() { return {}; }
    static RectLpa Auto() {
        return {LengthPercentageAuto::Auto(), LengthPercentageAuto::Auto(),
                LengthPercentageAuto::Auto(), LengthPercentageAuto::Auto()};
    }

    LengthPercentageAuto MainStart(FlexDirection d) const {
        return IsRow(d) ? left : top;
    }
    LengthPercentageAuto MainEnd(FlexDirection d) const {
        return IsRow(d) ? right : bottom;
    }
    LengthPercentageAuto CrossStart(FlexDirection d) const {
        return IsRow(d) ? top : left;
    }
    LengthPercentageAuto CrossEnd(FlexDirection d) const {
        return IsRow(d) ? bottom : right;
    }
    RectF ResolveOrZero(SizeFOpt context, CalcResolver calc) const;
    RectF ResolveOrZero(Optf context, CalcResolver calc) const;

    RectFOpt MaybeResolve(Optf context, CalcResolver calc) const;

    RectFOpt MaybeResolveZip(SizeFOpt context, CalcResolver calc) const;
};

struct PointOverflow {
    Overflow x = Overflow::Visible;
    Overflow y = Overflow::Visible;

    Overflow Get(AbstractAxis a) const {
        return a == AbstractAxis::Inline ? x : y;
    }
    Overflow Main(FlexDirection d) const { return IsRow(d) ? x : y; }
    Overflow Cross(FlexDirection d) const { return IsRow(d) ? y : x; }
    PointOverflow Transpose() const { return {y, x}; }
};

inline bool operator==(SizeDim a, SizeDim b) {
    return a.width == b.width && a.height == b.height;
}
inline bool operator!=(SizeDim a, SizeDim b) {
    return !(a == b);
}
inline bool operator==(SizeLp a, SizeLp b) {
    return a.width == b.width && a.height == b.height;
}
inline bool operator!=(SizeLp a, SizeLp b) {
    return !(a == b);
}
inline bool operator==(RectLp a, RectLp b) {
    return a.left == b.left && a.right == b.right && a.top == b.top &&
           a.bottom == b.bottom;
}
inline bool operator!=(RectLp a, RectLp b) {
    return !(a == b);
}
inline bool operator==(RectLpa a, RectLpa b) {
    return a.left == b.left && a.right == b.right && a.top == b.top &&
           a.bottom == b.bottom;
}
inline bool operator!=(RectLpa a, RectLpa b) {
    return !(a == b);
}
inline bool operator==(PointOverflow a, PointOverflow b) {
    return a.x == b.x && a.y == b.y;
}
inline bool operator!=(PointOverflow a, PointOverflow b) {
    return !(a == b);
}

struct GridLine {
    int16_t v = 0;

    int16_t AsI16() const { return v; }
};

struct OriginZeroLine {
    int16_t v = 0;
};

constexpr bool operator==(GridLine a, GridLine b) {
    return a.v == b.v;
}
constexpr bool operator!=(GridLine a, GridLine b) {
    return a.v != b.v;
}
constexpr bool operator==(OriginZeroLine a, OriginZeroLine b) {
    return a.v == b.v;
}
constexpr bool operator!=(OriginZeroLine a, OriginZeroLine b) {
    return a.v != b.v;
}
constexpr bool operator<(OriginZeroLine a, OriginZeroLine b) {
    return a.v < b.v;
}
constexpr bool operator>(OriginZeroLine a, OriginZeroLine b) {
    return a.v > b.v;
}
constexpr bool operator<=(OriginZeroLine a, OriginZeroLine b) {
    return a.v <= b.v;
}
constexpr bool operator>=(OriginZeroLine a, OriginZeroLine b) {
    return a.v >= b.v;
}
constexpr OriginZeroLine operator+(OriginZeroLine a, OriginZeroLine b) {
    return {(int16_t)(a.v + b.v)};
}
constexpr OriginZeroLine operator-(OriginZeroLine a, OriginZeroLine b) {
    return {(int16_t)(a.v - b.v)};
}
constexpr OriginZeroLine operator+(OriginZeroLine a, uint16_t b) {
    return {(int16_t)(a.v + (int16_t)b)};
}
constexpr OriginZeroLine operator-(OriginZeroLine a, uint16_t b) {
    return {(int16_t)(a.v - (int16_t)b)};
}

struct OptOriginZeroLine {
    OriginZeroLine val;
    bool has = false;

    constexpr OptOriginZeroLine() = default;
    constexpr explicit OptOriginZeroLine(OriginZeroLine v)
        : val(v), has(true) {}

    constexpr bool IsSome() const { return has; }
};

OriginZeroLine IntoOriginZeroLine(GridLine line, uint16_t explicitTrackCount);

constexpr uint16_t ImpliedNegativeImplicitTracks(OriginZeroLine l) {
    return l.v < 0 ? (uint16_t)(-(int32_t)l.v) : (uint16_t)0;
}

constexpr uint16_t ImpliedPositiveImplicitTracks(OriginZeroLine l,
                                                 uint16_t explicitTrackCount) {
    return l.v > (int16_t)explicitTrackCount
               ? (uint16_t)((uint16_t)l.v - explicitTrackCount)
               : (uint16_t)0;
}

struct LineOzl {
    OriginZeroLine start;
    OriginZeroLine end;

    uint16_t Span() const {
        int32_t d = (int32_t)end.v - (int32_t)start.v;
        return d > 0 ? (uint16_t)d : (uint16_t)0;
    }
};

struct LineOptOzl {
    OptOriginZeroLine start;
    OptOriginZeroLine end;
};

enum class GridAutoFlow : uint8_t {
    Row,
    Column,
    RowDense,
    ColumnDense
};

constexpr bool IsDense(GridAutoFlow f) {
    return f == GridAutoFlow::RowDense || f == GridAutoFlow::ColumnDense;
}

constexpr AbsoluteAxis PrimaryAxis(GridAutoFlow f) {
    return (f == GridAutoFlow::Row || f == GridAutoFlow::RowDense)
               ? AbsoluteAxis::Horizontal
               : AbsoluteAxis::Vertical;
}

enum class GridPlacementKind : uint8_t {
    Auto,
    Line,
    Span,
    NamedLine,
    NamedSpan
};

struct GridPlacement {
    GridPlacementKind kind = GridPlacementKind::Auto;
    int16_t line = 0;
    uint16_t span = 0;
    Str name;

    static GridPlacement Auto() { return {}; }
    static GridPlacement FromLineIndex(int16_t index) {
        GridPlacement p;
        p.kind = GridPlacementKind::Line;
        p.line = index;
        return p;
    }
    static GridPlacement FromSpan(uint16_t span) {
        GridPlacement p;
        p.kind = GridPlacementKind::Span;
        p.span = span;
        return p;
    }
    static GridPlacement FromNamedLine(Str name, int16_t index) {
        GridPlacement p;
        p.kind = GridPlacementKind::NamedLine;
        p.name = name;
        p.line = index;
        return p;
    }
    static GridPlacement FromNamedSpan(Str name, uint16_t span) {
        GridPlacement p;
        p.kind = GridPlacementKind::NamedSpan;
        p.name = name;
        p.span = span;
        return p;
    }
};

struct PlainPlacement {
    GridPlacementKind kind = GridPlacementKind::Auto;
    int16_t line = 0;
    uint16_t span = 0;

    static PlainPlacement Auto() { return {}; }
    static PlainPlacement AtLine(int16_t l) {
        return {GridPlacementKind::Line, l, 0};
    }
    static PlainPlacement Spanning(uint16_t s) {
        return {GridPlacementKind::Span, 0, s};
    }

    bool IsAuto() const { return kind == GridPlacementKind::Auto; }
    bool IsLine() const { return kind == GridPlacementKind::Line; }
    bool IsSpan() const { return kind == GridPlacementKind::Span; }
    OriginZeroLine Ozl() const { return OriginZeroLine{line}; }
};

constexpr bool operator==(PlainPlacement a, PlainPlacement b) {
    return a.kind == b.kind && a.line == b.line && a.span == b.span;
}
constexpr bool operator!=(PlainPlacement a, PlainPlacement b) {
    return !(a == b);
}

struct LinePlacement {
    GridPlacement start;
    GridPlacement end;

    bool IsDefinite() const;

    struct LinePlain IntoOriginZeroIgnoringNamed(
        uint16_t explicitTrackCount) const;
};

struct LinePlain {
    PlainPlacement start;
    PlainPlacement end;

    bool IsDefinite() const;

    bool IsDefiniteGridLine() const;

    LinePlain IntoOriginZero(uint16_t explicitTrackCount) const;

    uint16_t IndefiniteSpan() const;

    LineOzl ResolveDefiniteGridLines() const;

    LineOptOzl ResolveAbsolutelyPositionedGridTracks() const;

    LineOzl ResolveIndefiniteGridTracks(OriginZeroLine start) const;
};

struct MaxTrackSizingFunction {
    CompactLength raw = CompactLength::Auto();

    static MaxTrackSizingFunction Length(float v) {
        return {CompactLength::Length(v)};
    }
    static MaxTrackSizingFunction Percent(float v) {
        return {CompactLength::Percent(v)};
    }
    static MaxTrackSizingFunction Auto() { return {CompactLength::Auto()}; }
    static MaxTrackSizingFunction MinContent() {
        return {CompactLength::MinContent()};
    }
    static MaxTrackSizingFunction MaxContent() {
        return {CompactLength::MaxContent()};
    }
    static MaxTrackSizingFunction FitContentPx(float v) {
        return {CompactLength::FitContentPx(v)};
    }
    static MaxTrackSizingFunction FitContentPercent(float v) {
        return {CompactLength::FitContentPercent(v)};
    }
    static MaxTrackSizingFunction FitContent(LengthPercentage lp) {
        return lp.raw.Tag() == CompactLength::kPercentTag
                   ? FitContentPercent(lp.raw.Value())
                   : FitContentPx(lp.raw.Value());
    }
    static MaxTrackSizingFunction Fr(float v) { return {CompactLength::Fr(v)}; }
    static MaxTrackSizingFunction Zero() { return {CompactLength::Zero()}; }
    static MaxTrackSizingFunction From(LengthPercentage v) { return {v.raw}; }
    static MaxTrackSizingFunction From(LengthPercentageAuto v) {
        return {v.raw};
    }
    static MaxTrackSizingFunction From(Dimension v) { return {v.raw}; }

    bool IsIntrinsic() const { return raw.IsIntrinsic(); }
    bool IsMaxContentAlike() const { return raw.IsMaxContentAlike(); }
    bool IsFr() const { return raw.IsFr(); }
    bool IsAuto() const { return raw.IsAuto(); }
    bool IsMinContent() const { return raw.IsMinContent(); }
    bool IsMaxContent() const { return raw.IsMaxContent(); }
    bool IsFitContent() const { return raw.IsFitContent(); }
    bool IsMaxOrFitContent() const { return raw.IsMaxOrFitContent(); }
    bool UsesPercentage() const { return raw.UsesPercentage(); }

    bool HasDefiniteValue(Optf parentSize) const;
    Optf DefiniteValue(Optf parentSize, CalcResolver calc) const;

    Optf DefiniteLimit(Optf parentSize, CalcResolver calc) const;
    Optf ResolvedPercentageSize(float parentSize, CalcResolver calc) const {
        return taffy::ResolvedPercentageSize(raw, parentSize, calc);
    }
};

struct MinTrackSizingFunction {
    CompactLength raw = CompactLength::Auto();

    static MinTrackSizingFunction Length(float v) {
        return {CompactLength::Length(v)};
    }
    static MinTrackSizingFunction Percent(float v) {
        return {CompactLength::Percent(v)};
    }
    static MinTrackSizingFunction Auto() { return {CompactLength::Auto()}; }
    static MinTrackSizingFunction MinContent() {
        return {CompactLength::MinContent()};
    }
    static MinTrackSizingFunction MaxContent() {
        return {CompactLength::MaxContent()};
    }
    static MinTrackSizingFunction Zero() { return {CompactLength::Zero()}; }
    static MinTrackSizingFunction From(LengthPercentage v) { return {v.raw}; }
    static MinTrackSizingFunction From(LengthPercentageAuto v) {
        return {v.raw};
    }
    static MinTrackSizingFunction From(Dimension v) { return {v.raw}; }

    static MinTrackSizingFunction From(MaxTrackSizingFunction v) {
        if (v.raw.IsFr() || v.raw.IsFitContent()) {
            return Auto();
        }
        return {v.raw};
    }

    bool IsAuto() const { return raw.IsAuto(); }
    bool IsMinContent() const { return raw.IsMinContent(); }
    bool IsMaxContent() const { return raw.IsMaxContent(); }
    bool IsMinOrMaxContent() const { return raw.IsMinOrMaxContent(); }
    bool UsesPercentage() const {
        return raw.Tag() == CompactLength::kPercentTag || raw.IsCalc();
    }

    Optf DefiniteValue(Optf parentSize, CalcResolver calc) const;
    Optf ResolvedPercentageSize(float parentSize, CalcResolver calc) const {
        return taffy::ResolvedPercentageSize(raw, parentSize, calc);
    }
};

struct TrackSizingFunction {
    MinTrackSizingFunction min = MinTrackSizingFunction::Auto();
    MaxTrackSizingFunction max = MaxTrackSizingFunction::Auto();

    static TrackSizingFunction Auto() { return {}; }
    static TrackSizingFunction MinContent() {
        return {MinTrackSizingFunction::MinContent(),
                MaxTrackSizingFunction::MinContent()};
    }
    static TrackSizingFunction MaxContent() {
        return {MinTrackSizingFunction::MaxContent(),
                MaxTrackSizingFunction::MaxContent()};
    }
    static TrackSizingFunction Zero() {
        return {MinTrackSizingFunction::Zero(), MaxTrackSizingFunction::Zero()};
    }
    static TrackSizingFunction Length(float v) {
        return {MinTrackSizingFunction::Length(v),
                MaxTrackSizingFunction::Length(v)};
    }
    static TrackSizingFunction Percent(float v) {
        return {MinTrackSizingFunction::Percent(v),
                MaxTrackSizingFunction::Percent(v)};
    }
    static TrackSizingFunction Fr(float v) {
        return {MinTrackSizingFunction::Auto(), MaxTrackSizingFunction::Fr(v)};
    }
    static TrackSizingFunction FitContent(LengthPercentage lp) {
        return {MinTrackSizingFunction::Auto(),
                MaxTrackSizingFunction::FitContent(lp)};
    }
    static TrackSizingFunction MinMax(MinTrackSizingFunction mn,
                                      MaxTrackSizingFunction mx) {
        return {mn, mx};
    }
    static TrackSizingFunction From(LengthPercentage v) {
        return {MinTrackSizingFunction::From(v),
                MaxTrackSizingFunction::From(v)};
    }

    MinTrackSizingFunction MinSizingFunction() const { return min; }
    MaxTrackSizingFunction MaxSizingFunction() const { return max; }

    bool HasFixedComponent() const {
        return min.raw.IsLengthOrPercentage() || max.raw.IsLengthOrPercentage();
    }
};

struct RepetitionCount {
    enum class Kind : uint8_t {
        AutoFill,
        AutoFit,
        Count
    };
    Kind kind = Kind::Count;
    uint16_t count = 1;

    static RepetitionCount AutoFill() { return {Kind::AutoFill, 0}; }
    static RepetitionCount AutoFit() { return {Kind::AutoFit, 0}; }
    static RepetitionCount Exactly(uint16_t n) { return {Kind::Count, n}; }

    bool IsAuto() const {
        return kind == Kind::AutoFill || kind == Kind::AutoFit;
    }
};

constexpr bool operator==(RepetitionCount a, RepetitionCount b) {
    return a.kind == b.kind &&
           (a.kind != RepetitionCount::Kind::Count || a.count == b.count);
}

struct LineNameSet {
    Slice<Str> names;
};

struct GridTemplateRepetition {
    RepetitionCount count;
    Slice<TrackSizingFunction> tracks;
    Slice<LineNameSet> lineNames;

    uint16_t TrackCount() const { return (uint16_t)tracks.len; }
};

struct GridTemplateComponent {
    bool isRepeat = false;
    TrackSizingFunction single;
    GridTemplateRepetition repeat;

    static GridTemplateComponent Single(TrackSizingFunction t) {
        GridTemplateComponent c;
        c.single = t;
        return c;
    }
    static GridTemplateComponent Repeat(GridTemplateRepetition r) {
        GridTemplateComponent c;
        c.isRepeat = true;
        c.repeat = r;
        return c;
    }

    bool IsAutoRepetition() const { return isRepeat && repeat.count.IsAuto(); }
};

struct GridTemplateArea {
    Str name;
    uint16_t rowStart = 0;
    uint16_t rowEnd = 0;
    uint16_t columnStart = 0;
    uint16_t columnEnd = 0;
};

enum class GridAreaAxis : uint8_t {
    Row,
    Column
};

enum class GridAreaEnd : uint8_t {
    Start,
    End
};

struct Style {
    Display display = Display::Flex;

    bool itemIsTable = false;

    bool itemIsReplaced = false;
    BoxSizing boxSizing = BoxSizing::BorderBox;
    Direction direction = Direction::Ltr;

    PointOverflow overflow;
    float scrollbarWidth = 0.0f;

    Float floatMode = Float::None;
    Clear clear = Clear::None;

    Position position = Position::Relative;
    RectLpa inset = RectLpa::Auto();

    SizeDim size;
    SizeDim minSize;
    SizeDim maxSize;
    Optf aspectRatio = None();

    RectLpa margin;
    RectLp padding;
    RectLp border;

    OptAlignItems alignItems;
    OptAlignSelf alignSelf;
    OptAlignItems justifyItems;
    OptAlignSelf justifySelf;
    OptAlignContent alignContent;
    OptJustifyContent justifyContent;
    SizeLp gap;

    TextAlign textAlign = TextAlign::Auto;

    FlexDirection flexDirection = FlexDirection::Row;
    FlexWrap flexWrap = FlexWrap::NoWrap;

    Dimension flexBasis = Dimension::Auto();
    float flexGrow = 0.0f;
    float flexShrink = 1.0f;

    Slice<GridTemplateComponent> gridTemplateRows;
    Slice<GridTemplateComponent> gridTemplateColumns;
    Slice<TrackSizingFunction> gridAutoRows;
    Slice<TrackSizingFunction> gridAutoColumns;
    GridAutoFlow gridAutoFlow = GridAutoFlow::Row;

    Slice<GridTemplateArea> gridTemplateAreas;
    Slice<LineNameSet> gridTemplateColumnNames;
    Slice<LineNameSet> gridTemplateRowNames;

    LinePlacement gridRow;
    LinePlacement gridColumn;

    BoxGenerationMode BoxGenMode() const {
        return display == Display::None ? BoxGenerationMode::None
                                        : BoxGenerationMode::Normal;
    }
    bool IsBlock() const { return display == Display::Block; }
    bool IsCompressibleReplaced() const { return itemIsReplaced; }
};

bool operator==(const Style& a, const Style& b);

inline bool operator!=(const Style& a, const Style& b) {
    return !(a == b);
}

inline bool SameOptf(Optf a, Optf b) {
    uint32_t ab = 0;
    uint32_t bb = 0;
    memcpy(&ab, &a, sizeof(ab));
    memcpy(&bb, &b, sizeof(bb));
    return ab == bb;
}

inline bool SameFloatBits(float a, float b) {
    uint32_t ab = 0;
    uint32_t bb = 0;
    memcpy(&ab, &a, sizeof(ab));
    memcpy(&bb, &b, sizeof(bb));
    return ab == bb;
}

inline bool SameStr(base::Str a, base::Str b) {
    if (a.len != b.len) {
        return false;
    }
    if (a.len == 0) {
        return true;
    }
    return memcmp(a.s, b.s, (size_t)a.len) == 0;
}

inline bool operator==(MinTrackSizingFunction a, MinTrackSizingFunction b) {
    return a.raw == b.raw;
}

inline bool operator==(MaxTrackSizingFunction a, MaxTrackSizingFunction b) {
    return a.raw == b.raw;
}

inline bool operator==(TrackSizingFunction a, TrackSizingFunction b) {
    return a.min == b.min && a.max == b.max;
}

inline bool operator==(const GridPlacement& a, const GridPlacement& b) {
    return a.kind == b.kind && a.line == b.line && a.span == b.span &&
           SameStr(a.name, b.name);
}

inline bool operator==(const LinePlacement& a, const LinePlacement& b) {
    return a.start == b.start && a.end == b.end;
}

inline bool operator==(const GridTemplateArea& a, const GridTemplateArea& b) {
    return SameStr(a.name, b.name) && a.rowStart == b.rowStart &&
           a.rowEnd == b.rowEnd && a.columnStart == b.columnStart &&
           a.columnEnd == b.columnEnd;
}

template <typename T>
inline bool SameSlice(Slice<T> a, Slice<T> b) {
    if (a.len != b.len) {
        return false;
    }
    for (int i = 0; i < a.len; i++) {
        if (!(a.els[i] == b.els[i])) {
            return false;
        }
    }
    return true;
}

inline bool SameNames(Slice<base::Str> a, Slice<base::Str> b) {
    if (a.len != b.len) {
        return false;
    }
    for (int i = 0; i < a.len; i++) {
        if (!SameStr(a.els[i], b.els[i])) {
            return false;
        }
    }
    return true;
}

inline bool operator==(const LineNameSet& a, const LineNameSet& b) {
    return SameNames(a.names, b.names);
}

inline bool operator==(const GridTemplateRepetition& a,
                       const GridTemplateRepetition& b) {
    return a.count == b.count && SameSlice(a.tracks, b.tracks) &&
           SameSlice(a.lineNames, b.lineNames);
}

inline bool operator==(const GridTemplateComponent& a,
                       const GridTemplateComponent& b) {
    if (a.isRepeat != b.isRepeat) {
        return false;
    }
    return a.isRepeat ? a.repeat == b.repeat : a.single == b.single;
}

}

#endif

#line 1 "src/taffy/tree.h"

#ifndef GPUI_TAFFY_TREE_H_
#define GPUI_TAFFY_TREE_H_

namespace taffy {

struct NodeId {
    uint64_t raw = 0;

    static constexpr NodeId New(uint64_t v) { return NodeId{v}; }
    constexpr uint64_t AsU64() const { return raw; }
};

constexpr bool operator==(NodeId a, NodeId b) {
    return a.raw == b.raw;
}

constexpr bool operator!=(NodeId a, NodeId b) {
    return a.raw != b.raw;
}

enum class RunMode : uint8_t {

    PerformLayout,

    ComputeSize,

    PerformHiddenLayout
};

enum class SizingMode : uint8_t {

    ContentSize,

    InherentSize
};

enum class RequestedAxis : uint8_t {
    Horizontal,
    Vertical,
    Both
};

constexpr RequestedAxis ToRequestedAxis(AbsoluteAxis a) {
    return a == AbsoluteAxis::Horizontal ? RequestedAxis::Horizontal
                                         : RequestedAxis::Vertical;
}

struct CollapsibleMarginSet {

    float positive = 0.0f;

    float negative = 0.0f;

    static constexpr CollapsibleMarginSet Zero() { return {}; }

    static CollapsibleMarginSet FromMargin(float margin) {
        if (margin >= 0.0f) {
            return {margin, 0.0f};
        }
        return {0.0f, margin};
    }
    CollapsibleMarginSet CollapseWithMargin(float margin) const {
        CollapsibleMarginSet out = *this;
        if (margin >= 0.0f) {
            out.positive = F32Max(out.positive, margin);
        } else {
            out.negative = F32Min(out.negative, margin);
        }
        return out;
    }
    CollapsibleMarginSet CollapseWithSet(CollapsibleMarginSet other) const {
        return {F32Max(positive, other.positive),
                F32Min(negative, other.negative)};
    }

    float Resolve() const { return positive + negative; }
};

struct LayoutInput {

    RunMode runMode = RunMode::PerformLayout;

    SizingMode sizingMode = SizingMode::InherentSize;

    RequestedAxis axis = RequestedAxis::Both;

    SizeFOpt knownDimensions = SizeFOptNone();

    SizeFOpt parentSize = SizeFOptNone();

    SizeAvail availableSpace = SizeAvail::MaxContent();

    LineBool verticalMarginsAreCollapsible;

    static LayoutInput Hidden() {
        LayoutInput in;
        in.runMode = RunMode::PerformHiddenLayout;
        return in;
    }
};

struct LayoutOutput {
    SizeF size;

    SizeF contentSize;

    PointFOpt firstBaselines = PointFOptNone();

    CollapsibleMarginSet topMargin;
    CollapsibleMarginSet bottomMargin;

    bool marginsCanCollapseThrough = false;

    static LayoutOutput Hidden() { return {}; }

    static LayoutOutput FromSizesAndBaselines(SizeF size, SizeF contentSize,
                                              PointFOpt firstBaselines) {
        LayoutOutput out;
        out.size = size;
        out.contentSize = contentSize;
        out.firstBaselines = firstBaselines;
        return out;
    }
    static LayoutOutput FromSizes(SizeF size, SizeF contentSize) {
        return FromSizesAndBaselines(size, contentSize, PointFOptNone());
    }
    static LayoutOutput FromOuterSize(SizeF size) {
        return FromSizes(size, SizeF::Zero());
    }
};

struct Layout {

    uint32_t order = 0;

    PointF location;
    SizeF size;

    SizeF contentSize;

    SizeF scrollbarSize;
    RectF border;
    RectF padding;
    RectF margin;

    static Layout New() { return {}; }
    static Layout WithOrder(uint32_t order) {
        Layout l;
        l.order = order;
        return l;
    }

    float ContentBoxWidth() const {
        return size.w - padding.left - padding.right - border.left -
               border.right;
    }
    float ContentBoxHeight() const {
        return size.h - padding.top - padding.bottom - border.top -
               border.bottom;
    }
    SizeF ContentBoxSize() const {
        return {ContentBoxWidth(), ContentBoxHeight()};
    }

    float ContentBoxX() const {
        return location.x + border.left + padding.left;
    }
    float ContentBoxY() const { return location.y + border.top + padding.top; }

    float ScrollWidth() const {
        return F32Max(0.0f, contentSize.w +
                                F32Min(scrollbarSize.w, size.w) -
                                size.w + border.right);
    }
    float ScrollHeight() const {
        return F32Max(0.0f, contentSize.h +
                                F32Min(scrollbarSize.h, size.h) -
                                size.h + border.bottom);
    }
};

constexpr int kCacheSize = 9;

struct CacheKey {
    uint64_t kdAvailableSpace = 0;
    uint64_t parentSize = 0;

    static CacheKey From(const LayoutInput& input);

    uint64_t XAxisParentSize() const;
};

constexpr bool operator==(CacheKey a, CacheKey b) {
    return a.kdAvailableSpace == b.kdAvailableSpace &&
           a.parentSize == b.parentSize;
}

template <typename T>
struct CacheEntry {
    CacheKey key;
    T content{};
};

struct Cache {
    CacheEntry<LayoutOutput> finalLayoutEntry;
    CacheEntry<SizeF> measureEntries[kCacheSize];

    uint16_t presentMask = 0;

    bool isEmpty = true;

    static constexpr uint16_t kFinalBit = 1;
    static constexpr uint16_t MeasureBit(int i) {
        return (uint16_t)(1u << (i + 1));
    }

    bool Get(const LayoutInput& input, LayoutOutput* out) const;
    void Store(const LayoutInput& input, const LayoutOutput& output);

    bool GetWithKey(CacheKey key, RunMode runMode, LayoutOutput* out) const;
    void StoreWithKey(CacheKey key, const LayoutInput& input,
                      const LayoutOutput& output);

    bool Clear();
    bool IsEmpty() const;
};

}

#endif

#line 1 "src/taffy/taffy_tree.h"

#ifndef GPUI_TAFFY_TAFFY_TREE_H_
#define GPUI_TAFFY_TAFFY_TREE_H_

namespace taffy {

struct BlockContext;

using MeasureFn = SizeF (*)(SizeFOpt knownDimensions, SizeAvail availableSpace,
                            NodeId node, void* nodeContext, const Style* style,
                            void* userData);

struct NodeData {
    Style style;

    Layout unroundedLayout;

    Layout finalLayout;

    bool hasContext = false;
    void* context = nullptr;

    Cache cache;

    uint32_t generation = 0;
    bool alive = false;

    Vec<NodeId> children;
    NodeId parent;
    bool hasParent = false;
};

struct TaffyTree {

    Vec<NodeData*> slots;
    Vec<int32_t> freeSlots;
    int32_t liveCount = 0;

    bool useRounding = true;

    MeasureFn measureFn = nullptr;
    void* measureUserData = nullptr;

    Arena* styleArena = nullptr;

    CalcResolver calc;

    void Init(int capacity = 16);
    void Free();

    void EnableRounding() { useRounding = true; }
    void DisableRounding() { useRounding = false; }

    NodeId NewLeaf(const Style& style);
    NodeId NewLeafWithContext(const Style& style, void* context);
    NodeId NewWithChildren(const Style& style, const NodeId* children, int n);

    void Clear();

    void Remove(NodeId node);

    void SetNodeContext(NodeId node, void* context, bool hasContext);
    void* GetNodeContext(NodeId node) const;

    void AddChild(NodeId parent, NodeId child);

    bool InsertChildAtIndex(NodeId parent, int childIndex, NodeId child);
    void SetChildren(NodeId parent, const NodeId* children, int n);

    NodeId RemoveChild(NodeId parent, NodeId child);

    NodeId RemoveChildAtIndex(NodeId parent, int childIndex);
    void RemoveChildrenRange(NodeId parent, int start, int end);

    NodeId ReplaceChildAtIndex(NodeId parent, int childIndex, NodeId newChild);

    NodeId ChildAtIndex(NodeId parent, int childIndex) const;
    int TotalNodeCount() const { return liveCount; }

    NodeId Parent(NodeId child, bool* hasParent) const;

    void SetStyle(NodeId node, const Style& style);
    const Style& GetStyle(NodeId node) const;

    const Layout& GetLayout(NodeId node) const;
    const Layout& UnroundedLayout(NodeId node) const;

    void MarkDirty(NodeId node);
    bool Dirty(NodeId node) const;

    void ComputeLayoutWithMeasure(NodeId node, SizeAvail availableSpace,
                                  MeasureFn measure, void* userData);
    void ComputeLayout(NodeId node, SizeAvail availableSpace);

    void PrintTree(NodeId root);

    int ChildCount(NodeId parent) const;
    NodeId GetChildId(NodeId parent, int index) const;

    void SetUnroundedLayout(NodeId node, const Layout& layout);
    Layout GetUnroundedLayout(NodeId node) const;
    void SetFinalLayout(NodeId node, const Layout& layout);
    Layout GetFinalLayout(NodeId node) const;

    const char* GetDebugLabel(NodeId node) const;

    bool CacheGet(NodeId node, const LayoutInput& input,
                  LayoutOutput* out) const;
    void CacheStore(NodeId node, const LayoutInput& input,
                    const LayoutOutput& output);
    void CacheClear(NodeId node);

    float ResolveCalcValue(const void* handle, float basis) const {
        return calc.Resolve(handle, basis);
    }

    LayoutOutput ComputeChildLayout(NodeId node, LayoutInput inputs);
    LayoutOutput ComputeBlockChildLayout(NodeId node, LayoutInput inputs,
                                         BlockContext* blockCtx);

    float MeasureChildSize(NodeId node, SizeFOpt knownDimensions,
                           SizeFOpt parentSize, SizeAvail availableSpace,
                           SizingMode sizingMode, AbsoluteAxis axis,
                           LineBool verticalMarginsAreCollapsible);
    SizeF MeasureChildSizeBoth(NodeId node, SizeFOpt knownDimensions,
                               SizeFOpt parentSize, SizeAvail availableSpace,
                               SizingMode sizingMode,
                               LineBool verticalMarginsAreCollapsible);
    LayoutOutput PerformChildLayout(NodeId node, SizeFOpt knownDimensions,
                                    SizeFOpt parentSize,
                                    SizeAvail availableSpace,
                                    SizingMode sizingMode,
                                    LineBool verticalMarginsAreCollapsible);

    NodeData* Get(NodeId node) const;
};

}

#endif

#line 1 "src/gpui/gpui.h"

namespace gpui {
using namespace base;
}

namespace gpui {

struct Rgba {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

inline Rgba Rgb(uint8_t r, uint8_t g, uint8_t b) {
    return Rgba{r, g, b, 255};
}
inline Rgba Rgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return Rgba{r, g, b, a};
}
inline Rgba RgbaHex(uint32_t hex) {

    if (hex > 0xFFFFFFu) {
        return Rgba{(uint8_t)((hex >> 16) & 0xff), (uint8_t)((hex >> 8) & 0xff),
                    (uint8_t)(hex & 0xff), (uint8_t)((hex >> 24) & 0xff)};
    }
    return Rgba{(uint8_t)((hex >> 16) & 0xff), (uint8_t)((hex >> 8) & 0xff),
                (uint8_t)(hex & 0xff), 255};
}
Rgba RgbaOpacity(Rgba c, float a01);

Rgba RgbaMix(Rgba a, Rgba b, float t);

struct Hsla {
    float h = 0;
    float s = 0;
    float l = 0;
    float a = 0;
};

Hsla HslaNew(float h, float s, float l, float a);

Hsla HslaFromRgba(Rgba c);

Rgba HslaToRgba(Hsla c);

Rgba RgbaHsla(float h, float s, float l, float a01);

Rgba RgbaWithHue(Rgba c, float h01);

Rgba RgbaMixHsl(Rgba a, Rgba b, float factor);

struct ColorStop {
    Rgba color = {};

    float percentage = 0;
};

struct Background {
    Rgba color = {};
    ColorStop from = {};
    ColorStop to = {};

    float angle = 180.f;
    bool gradient = false;

    Background() = default;

    Background(Rgba c) : color(c) {}
};

Background BackgroundLinear(float angle, ColorStop from, ColorStop to);
inline ColorStop ColorStopAt(Rgba c, float pct) {
    return ColorStop{c, pct};
}

Background BackgroundOpacity(Background b, float factor);

Background BackgroundClampAlpha(Background b, float max);
inline bool BackgroundIsSolid(const Background& b) {
    return !b.gradient;
}

struct InputState;

constexpr float kAuto = -1.f;
constexpr float kFill = -2.f;
constexpr float kPi = 3.14159265358979f;

struct App;

struct ThemeTokens {
    Background background = {};
    Background titleBar = {};
    Background statusBar = {};
    Background tabBar = {};
    Background tabActiveBg = {};
    Background primary = {};
    Background secondary = {};
    Background accent = {};
    Background muted = {};
    Background popover = {};
    Background danger = {};
    Background info = {};
    Background success = {};
    Background warning = {};
    Background progress = {};
    Background scrollbarThumb = {};
    Background scrollbarThumbHover = {};
    Background skeleton = {};
    Background selection = {};
    Background listActive = {};
    Background tableBg = {};
    Background tableActive = {};
    Background tableEven = {};
    Background tableHead = {};
    Background tableFoot = {};
    Background sidebarAccent = {};
    Background sidebarPrimary = {};
    Background overlay = {};
    Background switchThumb = {};
    Background sliderThumb = {};

    Background button = {};
    Background buttonHover = {};
    Background buttonActive = {};
    Background primaryHover = {};
    Background primaryActive = {};
    Background buttonPrimary = {};
    Background buttonPrimaryHover = {};
    Background buttonPrimaryActive = {};
    Background secondaryHover = {};
    Background secondaryActive = {};
    Background buttonSecondary = {};
    Background buttonSecondaryHover = {};
    Background buttonSecondaryActive = {};
    Background successHover = {};
    Background successActive = {};
    Background buttonSuccess = {};
    Background buttonSuccessHover = {};
    Background buttonSuccessActive = {};
    Background infoHover = {};
    Background infoActive = {};
    Background buttonInfo = {};
    Background buttonInfoHover = {};
    Background buttonInfoActive = {};
    Background warningHover = {};
    Background warningActive = {};
    Background buttonWarning = {};
    Background buttonWarningHover = {};
    Background buttonWarningActive = {};
    Background dangerHover = {};
    Background dangerActive = {};
    Background buttonDanger = {};
    Background buttonDangerHover = {};
    Background buttonDangerActive = {};
    Background accordion = {};
    Background dropTarget = {};
    Background list = {};
    Background listEven = {};
    Background listHead = {};
    Background listHover = {};
    Background sliderBar = {};
    Background switchBg = {};
    Background tab = {};
    Background tabBarSegmented = {};
    Background tableHover = {};
    Background tiles = {};
    Background scrollbarBg = {};
    Background sidebar = {};
    Background groupBox = {};
    Background descListLabel = {};
};

struct Theme {
    Rgba background;
    Rgba foreground;
    Rgba border;
    Rgba mutedFg;

    Rgba inputBorder;
    Rgba inputBg;

    Rgba ring;
    Rgba caret;

    Rgba selection;

    Rgba dragBorder;
    Rgba titleBar;
    Rgba titleBarBorder;

    Rgba statusBar;

    Rgba statusBarBorder;
    Rgba tabBar;
    Rgba tabActiveBg;
    Rgba tabActiveFg;
    Rgba tabFg;
    Rgba tableBg;
    Rgba tableHead;
    Rgba tableHeadFg;

    Rgba tableFoot;
    Rgba tableFootFg;
    Rgba tableRowBorder;
    Rgba tableEven;

    Rgba listActive;
    Rgba listActiveBorder;
    Rgba tableActive;
    Rgba tableActiveBorder;
    Rgba progress;
    Rgba red;
    Rgba green;
    Rgba blue;
    Rgba yellow;
    Rgba cyan;
    Rgba magenta;

    Rgba redLight;
    Rgba greenLight;
    Rgba blueLight;
    Rgba yellowLight;
    Rgba cyanLight;
    Rgba magentaLight;

    Rgba chart1;
    Rgba chart2;
    Rgba chart3;
    Rgba chart4;
    Rgba chart5;
    Rgba chartBullish;
    Rgba chartBearish;
    Rgba danger;
    Rgba dangerFg;

    Rgba popover;
    Rgba popoverFg;
    Rgba secondaryHover;
    Rgba secondaryActive;
    Rgba secondaryFg;
    Rgba secondary;
    Rgba muted;
    Rgba accent;

    Rgba accentFg;
    Rgba primary;
    Rgba primaryFg;
    Rgba primaryHover;
    Rgba primaryActive;
    Rgba sidebar;
    Rgba sidebarFg;
    Rgba sidebarPrimary;
    Rgba sidebarPrimaryFg;
    Rgba sidebarAccent;
    Rgba sidebarAccentFg;
    Rgba sidebarBorder;
    Rgba scrollbarThumb;

    Rgba scrollbarThumbHover;

    Rgba scrollbarBg;
    Rgba info;
    Rgba infoFg;
    Rgba success;
    Rgba successFg;
    Rgba warning;
    Rgba warningFg;
    Rgba skeleton;

    Rgba switchThumb;
    Rgba sliderThumb;

    Rgba overlay;

    Rgba groupBox;
    Rgba groupBoxFg;

    Rgba descListLabel;
    Rgba descListLabelFg;

    Rgba button;
    Rgba buttonFg;
    Rgba buttonHover;
    Rgba buttonActive;
    Rgba buttonPrimary;
    Rgba buttonPrimaryFg;
    Rgba buttonPrimaryHover;
    Rgba buttonPrimaryActive;
    Rgba buttonSecondary;
    Rgba buttonSecondaryFg;
    Rgba buttonSecondaryHover;
    Rgba buttonSecondaryActive;
    Rgba buttonDanger;
    Rgba buttonDangerFg;
    Rgba buttonDangerHover;
    Rgba buttonDangerActive;
    Rgba buttonSuccess;
    Rgba buttonSuccessFg;
    Rgba buttonSuccessHover;
    Rgba buttonSuccessActive;
    Rgba buttonInfo;
    Rgba buttonInfoFg;
    Rgba buttonInfoHover;
    Rgba buttonInfoActive;
    Rgba buttonWarning;
    Rgba buttonWarningFg;
    Rgba buttonWarningHover;
    Rgba buttonWarningActive;

    Rgba dangerHover;
    Rgba dangerActive;
    Rgba successHover;
    Rgba successActive;
    Rgba infoHover;
    Rgba infoActive;
    Rgba warningHover;
    Rgba warningActive;

    Rgba accordion;

    Rgba dropTarget;

    Rgba link;
    Rgba linkActive;
    Rgba linkHover;

    Rgba list;
    Rgba listEven;
    Rgba listHead;
    Rgba listHover;
    Rgba tableHover;

    Rgba sliderBar;

    Rgba switchBg;

    Rgba tab;
    Rgba tabBarSegmented;

    Rgba tiles;

    Rgba windowBorder;

    float radius;
    float radiusLg;

    float radiusFull;

    ThemeTokens tokens = {};
};

void ThemeTokensReset(Theme* t);

void ThemeFillDerived(Theme* t, bool dark);

Rgba RgbaTransparent();

Rgba RgbaBlend(Rgba base, Rgba over);

Rgba RgbaLighten(Rgba c, float amount);
Rgba RgbaDarken(Rgba c, float amount);

Str RgbaToHex(Arena* a, Rgba c, bool upper = true);

Rgba RgbaMixOklab(Rgba a, Rgba b, float factor);

enum class ThemeMode : uint8_t {
    Light,
    Dark
};

const Theme& ThemeDark();
const Theme& ThemeLight();

const Theme& ThemeDefaultDark();
const Theme& ThemeDefaultLight();

void ThemeInstall(ThemeMode mode, const Theme& t);

void ThemeSetRadius(float radius);

float ThemeFontSize();

float WheelNotchPixels();
void ThemeSetFontSize(float px);

bool ThemeFocusRing();
void ThemeSetFocusRing(bool on);
const Theme& ThemeNow();
void ThemeSet(App* app, ThemeMode mode);
ThemeMode ThemeGet();

enum class Axis : uint8_t {
    Horizontal,
    Vertical
};

using Point = base::PointF;
using Size = base::SizeF;
using Edges = base::RectF;

struct Bounds {
    float x = 0, y = 0, w = 0, h = 0;

    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    float CenterX() const { return x + w * 0.5f; }
    float CenterY() const { return y + h * 0.5f; }

    bool Contains(Point p) const {
        return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
    }

    Bounds Inset(float d) const { return {x + d, y + d, w - d - d, h - d - d}; }

    Bounds Inset(Edges e) const {
        return {x + e.left, y + e.top, w - e.HorizontalAxisSum(),
                h - e.VerticalAxisSum()};
    }
};

inline Bounds BoundsAt(Point origin, Size size) {
    return {origin.x, origin.y, size.w, size.h};
}

void BackgroundLine(const Background& b, Bounds box, Point* p0, Point* p1);

struct Window;
struct Ctx;
struct El;
struct SliderState;

struct TextLayout;

struct EntityId {
    int32_t index = -1;
    uint32_t gen = 0;

    bool IsValid() const { return index >= 0 && gen != 0; }
};

inline bool operator==(EntityId a, EntityId b) {
    return a.index == b.index && a.gen == b.gen;
}
inline bool operator!=(EntityId a, EntityId b) {
    return !(a == b);
}

using RenderFn = El* (*)(void* self, Ctx* cx);
using DropFn = void (*)(void* self);

struct EntitySlot {
    void* ptr = nullptr;
    uint32_t gen = 0;
    RenderFn render = nullptr;
    DropFn drop = nullptr;
};

struct MotionSlotRec {
    uint32_t key = 0;

    uint64_t frame = 0;
    void* ptr = nullptr;
};

struct KeyedSlot {
    uint32_t key = 0;
    void* ptr = nullptr;
    DropFn drop = nullptr;

    EntityId entity = {};
};

enum class MouseButton : uint8_t {
    Left,
    Right,
    Middle,
    NavigateBack,
    NavigateForward
};

struct Modifiers {
    bool control = false;
    bool alt = false;
    bool shift = false;
    bool platform = false;
    bool function = false;

    bool Modified() const {
        return control || alt || shift || platform || function;
    }

    bool Secondary() const {
#if GPUI_OS_MAC
        return platform;
#else
        return control;
#endif
    }
    int Count() const {
        return (int)control + (int)alt + (int)shift + (int)platform +
               (int)function;
    }
};

enum class TouchPhase : uint8_t {
    Started,
    Moved,
    Ended,
    Cancelled
};

enum class DispatchPhase : uint8_t {
    Capture,
    Bubble
};

struct MouseDownEvent {
    MouseButton button = MouseButton::Left;
    float x = 0;
    float y = 0;

    Bounds el = {};
    Modifiers modifiers = {};

    int clickCount = 1;

    bool firstMouse = false;

    DispatchPhase phase = DispatchPhase::Bubble;

    bool IsFocusing() const { return button == MouseButton::Left; }
};

struct MouseUpEvent {
    MouseButton button = MouseButton::Left;
    float x = 0;
    float y = 0;

    Bounds el = {};
    Modifiers modifiers = {};
    int clickCount = 1;
    DispatchPhase phase = DispatchPhase::Bubble;

    bool IsFocusing() const { return button == MouseButton::Left; }
};

struct MouseMoveEvent {
    float x = 0;
    float y = 0;

    bool pressed = false;
    MouseButton pressedButton = MouseButton::Left;
    Modifiers modifiers = {};

    bool Dragging() const {
        return pressed && pressedButton == MouseButton::Left;
    }
};

inline bool StrSame(Str a, Str b) {
    return a.len == b.len &&
           (a.len == 0 || memcmp(a.s, b.s, (size_t)a.len) == 0);
}

struct DragPayload {
    Str kind = {};
    int ix = 0;
    void* data = nullptr;

    bool IsValid() const { return kind.s != nullptr; }
};

struct DragMoveEvent {
    DragPayload drag = {};
    MouseMoveEvent event = {};

    Bounds el = {};
};

struct DropEvent {
    DragPayload drag = {};

    float x = 0;
    float y = 0;

    Bounds el = {};
};

struct MouseExitEvent {
    float x = 0;
    float y = 0;
    bool pressed = false;
    MouseButton pressedButton = MouseButton::Left;
    Modifiers modifiers = {};
};

struct ScrollWheelEvent {
    float x = 0;
    float y = 0;

    float deltaX = 0;
    float deltaY = 0;

    bool precise = false;
    Modifiers modifiers = {};
    TouchPhase phase = TouchPhase::Moved;
};

enum class PlatformInputKind : uint8_t {
    MouseDown,
    MouseUp,
    MouseMove,
    MouseExited,
    ScrollWheel
};

struct PlatformInput {
    PlatformInputKind kind = PlatformInputKind::MouseMove;
    union {
        MouseDownEvent mouseDown = {};
        MouseUpEvent mouseUp;
        MouseMoveEvent mouseMove;
        MouseExitEvent mouseExited;
        ScrollWheelEvent scrollWheel;
    };
};

struct ClickEvent {
    float x = 0;
    float y = 0;
    MouseButton button = MouseButton::Left;

    int id = 0;

    Bounds el = {};
    int clickCount = 1;
    Modifiers modifiers = {};

    bool keyboard = false;

    int keyboardKey = 0;
};

enum {
    KeyBack = 8,
    KeyTab = 9,
    KeyReturn = 13,
    KeyShift = 16,
    KeyControl = 17,
    KeyMenu = 18,
    KeyEscape = 27,
    KeySpace = 32,
    KeyPageUp = 33,
    KeyPageDown = 34,
    KeyEnd = 35,
    KeyHome = 36,
    KeyLeft = 37,
    KeyUp = 38,
    KeyRight = 39,
    KeyDown = 40,
    KeyDelete = 46,

    KeyA = 65,
    KeyC = 67,
    KeyE = 69,
    KeyF = 70,
    KeyH = 72,
    KeyV = 86,
    KeyX = 88,
    KeyY = 89,
    KeyZ = 90,

    KeyLeftBracket = 219,
    KeyRightBracket = 221
};

struct KeyEvent {
    int vk = 0;
    uint32_t ch = 0;
    bool down = false;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;

    bool platform = false;

    bool propagate = true;
};

enum class CursorKind : uint8_t {
    Arrow,
    IBeam,

    Pointer,

    ColResize,

    RowResize,

    Crosshair
};

struct TickEvent {
    int ms = 0;
};

struct HoverEvent {
    bool hovered = false;
};

using ListenerFn = void (*)(void* self, Ctx* cx, const void* ev);
using ListenerArgFn = void (*)(void* self, Ctx* cx, const void* ev,
                               intptr_t arg);

struct Listener {
    void* fn = nullptr;
    EntityId view = {};
    intptr_t arg = 0;

    bool hasArg = false;

    bool argBound = false;

    bool IsValid() const { return fn != nullptr; }
};

struct TimerSub {
    int id = 0;
    int ms = 0;
    double dueAt = 0;
    bool repeat = false;
    Listener l;
};

enum class ElKind : uint8_t {
    Div,
    Text,
    Chart,
    Progress,
    Icon,

    Image
};

enum class Display : uint8_t {
    Block,
    Flex
};

enum class FlexDir : uint8_t {
    Row,
    Col,

    RowReverse,
    ColReverse
};
enum class Align : uint8_t {
    Start,
    Center,
    End,
    Stretch
};
enum class Justify : uint8_t {
    Start,
    Center,
    End,
    SpaceBetween,
    SpaceAround
};

enum class Overflow : uint8_t {
    Visible,
    Hidden,
    Scroll
};

enum class ScrollbarMode : uint8_t {
    Always,
    Hover,
    Scrolling
};

const double kInactiveFrameInterval = 0.5;

const float kRadiusFull = 9999.f;

const int kPaintLayerTree = 0;

const int kPaintLayerPopup = 1;

const int kPaintLayerTooltip = 2;

const int kPaintLayerInspector = 3;

enum class ScrollbarEntrance : uint8_t {

    Fade,

    SlideAndFade
};

struct ScrollbarMotion {

    float idle = 2.f;

    float enter = 0.3f;

    float exit = 0.5f;

    float expand = 0.3f;
    ScrollbarEntrance entrance = ScrollbarEntrance::Fade;
    ScrollbarEntrance thumbHoverEntrance = ScrollbarEntrance::Fade;
};

ScrollbarMotion ScrollbarMotionFor(ScrollbarMode mode);

struct ScrollbarVisibility {
    float opacity = 0;
    float position = 0;
    bool running = false;
};

void ScrollbarVisibilitySet(int scrollId, bool visible,
                            ScrollbarEntrance entrance, float enter, float exit,
                            double now);
ScrollbarVisibility ScrollbarVisibilityAt(int scrollId, double now);

float ScrollbarSlideOffset(float trackWidth, float position);

ScrollbarMode ScrollbarModeNow();
void ScrollbarModeSet(ScrollbarMode m);

void ScrollFadeClear();

enum class IconName : uint8_t {
    None = 0,
    Inbox,
    Bot,
    Cpu,
    MemoryStick,
    HardDrive,
    Battery,
    BatteryCharging,
    BatteryMedium,
    BatteryFull,
    WindowMinimize,
    WindowMaximize,
    WindowRestore,
    WindowClose,
    LayoutDashboard,
    Calendar,
    Folder,
    Settings,
    GalleryVerticalEnd,
    CircleUser,
    User,
    PanelLeft,
    PanelLeftOpen,
    PanelLeftClose,
    PanelRight,
    PanelRightOpen,
    PanelRightClose,
    PanelBottom,
    PanelBottomOpen,
    Info,
    X,
    CircleCheck,
    TriangleAlert,
    CircleX,
    Loader,
    LoaderCircle,
    Ellipsis,
    ChevronsUpDown,
    SquareTerminal,
    BookOpen,
    Settings2,
    Frame,
    ChartPie,
    File,
    FolderOpen,
    ChevronDown,
    ChevronLeft,
    ChevronRight,

    CaseSensitive,
    Replace,
    ChevronUp,
    Check,
    Search,
    Minus,
    Plus,
    Palette,
    Copy,
    Bell,
    Star,
    StarFill,
    Eye,
    EyeOff,
    Heart,
    ArrowLeft,
    ArrowRight,
    ArrowUp,
    ArrowDown,
    Building2,
    Asterisk,
    Sun,
    Moon,
    Play,
    Maximize,
    Minimize,
    Map,
    Globe,
    Github,
    ExternalLink,
    HeartOff,
};

struct PaintCtx;

struct TextSpan {
    int lo = 0;
    int hi = 0;
    Rgba color = {};

    Rgba bg = {0, 0, 0, 0};

    bool underline = false;

    bool wavy = false;
};

enum class ChartKind : uint8_t {
    Area,
    Line,
    Bar,
    Candlestick,
    Radar
};

enum class ChartStroke : uint8_t {
    Natural,
    Linear,
    StepAfter
};

enum class BarAlign : uint8_t {
    Bottom,
    Top,
    Left,
    Right
};

struct ChartSeriesExtra {
    const float* ys = nullptr;
    Rgba stroke = {};
    Rgba fillTop = {};
    Rgba fillBot = {};

    Str name = {};
};

struct ChartSeries {
    ChartKind kind = ChartKind::Area;
    const float* ys = nullptr;
    int n = 0;

    const ChartSeriesExtra* more = nullptr;
    int nMore = 0;
    int tickMargin = 15;

    const char* const* labels = nullptr;

    bool overlay = false;
    Rgba stroke = {};
    Rgba fillTop = {};
    Rgba fillBot = {};

    float domainMin = 0;
    float domainMax = 0;

    const float* opens = nullptr;
    const float* highs = nullptr;
    const float* lows = nullptr;
    Rgba up = {};
    Rgba down = {};

    float bandPadding = 0.2f;
    float barRadius = 4;
    BarAlign barAlign = BarAlign::Bottom;

    const float* bases = nullptr;

    bool barLabels = false;

    bool valueAxis = false;

    int valueTickCount = 4;

    const Rgba* barFills = nullptr;

    bool barGradient = false;
    bool barGradientPerBar = false;

    bool barGradientDiagonal = false;
    Rgba barFillFrom = {};
    Rgba barFillTo = {};

    ChartStroke strokeStyle = ChartStroke::Natural;
    bool dot = false;

    float bodyWidthRatio = 0.8f;

    float radarRadius = 0;
    int gridLevels = 4;

    bool tooltip = false;

    Str name = {};
};

struct Corners {
    float tl = 0;
    float tr = 0;
    float br = 0;
    float bl = 0;

    bool IsUniform() const { return tl == tr && tr == br && br == bl; }
};

struct Style {
    Display display = Display::Block;
    FlexDir dir = FlexDir::Row;
    Align align = Align::Stretch;
    Justify justify = Justify::Start;
    Overflow overflowY = Overflow::Visible;
    Overflow overflowX = Overflow::Visible;
    float width = kAuto;
    float height = kAuto;

    float widthFrac = 0;

    float minW = kAuto;
    float minH = kAuto;
    float maxW = 1e9f;
    float maxH = 1e9f;

    float aspect = 0;
    float flexGrow = 0;
    float flexShrink = 1;

    float flexBasis = kAuto;

    float flexBasisFrac = 0;
    Edges pad = {};

    float gapX = 0;
    float gapY = 0;
    float border = 0;
    float borderT = 0;
    float borderB = 0;
    float borderL = 0;
    float borderR = 0;
    float radius = 0;

    Corners corners = {};
    bool hasCorners = false;
    Background bg = {};
    Rgba borderColor = {};
    Rgba color = {};

    float rotate = 0;

    float opacity = 1;
    float fontSize = 0;

    float lineHeight = 0;
    bool truncate = false;
    bool wrap = false;

    bool flexWrap = false;
    bool hasBg = false;
    bool hasColor = false;
    bool fontBold = false;
    bool fontSemibold = false;
    bool fontMedium = false;
    bool fontMono = false;
    bool underline = false;

    bool strike = false;
    bool italic = false;
    bool borderDashed = false;

    float dashOn = 2;
    float dashOff = 1;
    bool absolute = false;
    bool fixed = false;

    bool deferred = false;

    bool anchorFlip = false;
    bool anchorBelow = false;
    bool anchorAbove = false;
    bool anchorCenterX = false;
    float anchorGap = 0;
    float absTop = kAuto, absLeft = kAuto, absBottom = kAuto, absRight = kAuto;

    float absLeftRel = 0, absRightRel = 0;
    Background hoverBg = {};
    bool hasHoverBg = false;

    Rgba hoverFg = {};
    bool hasHoverFg = false;

    Background activeBg = {};
    bool hasActiveBg = false;

    bool group = false;

    bool groupHoverVisible = false;

    Background groupHoverBg = {};
    bool hasGroupHoverBg = false;
    int focusId = 0;

    bool focusFromPath = false;

    int tabIndex = 0;
    bool tabStop = true;

    bool focusOnPress = false;

    uint32_t keyContext = 0;

    bool focusRing = true;
    int trapId = 0;
    Str tooltip;
};

struct ActionSlot {
    uint32_t action = 0;
    Listener fn = {};
    ActionSlot* next = nullptr;
};

struct ActionEvent {
    uint32_t action = 0;

    intptr_t arg = 0;

    bool propagate = false;
};

enum class SelectionFormat : uint8_t {
    Plain = 0,
    Source
};

struct SelBlock {

    Str pre;
    Str post;

    Str linePre;

    bool join = false;
};

struct SelSource {

    Str pre;
    Str post;

    const SelBlock* block = nullptr;
};

struct FocusHandle {
    int id = 0;
    bool IsValid() const { return id != 0; }
    bool operator==(const FocusHandle& o) const { return id == o.id; }
    bool operator!=(const FocusHandle& o) const { return id != o.id; }
};

struct El {
    ElKind kind = ElKind::Div;

    Arena* arena = nullptr;
    Style style;
    Str id;
    Str text;
    IconName icon = IconName::None;
    Str iconPath;

    Str imgSrc;
    ChartSeries chart = {};
    float progress = 0;
    int clickId = 0;

    uint32_t pathId = 0;

    bool clickFromPath = false;

    bool stopClick = false;
    Func0 onClick;
    Listener listener;

    uint32_t clickAction = 0;
    intptr_t clickActionArg = 0;

    Listener onHover;
    Listener onScroll;
    ActionSlot* actions = nullptr;

    Listener onMouseDown;
    Listener onMouseUp;
    DispatchPhase mouseDownPhase = DispatchPhase::Bubble;
    DispatchPhase mouseUpPhase = DispatchPhase::Bubble;

    Listener onDragMove;

    Style refine = {};
    uint32_t refineSet = 0;

    Style hoverStyle = {};
    uint32_t hoverSet = 0;
    Style dragOverStyle = {};
    uint32_t dragOverSet = 0;
    Str dragOverKind = {};

    DragPayload drag = {};

    CursorKind cursor = CursorKind::Arrow;

    Listener onMouseUpOut;

    Str dropKind = {};
    Listener onDrop;

    gpui::Bounds* boundsOut = nullptr;

    SliderState* slider = nullptr;
    Axis sliderAxis = Axis::Horizontal;

    InputState* input = nullptr;

    SliderState* sliderBounds = nullptr;
    void (*customPaint)(PaintCtx* ctx, El* e, void* user) = nullptr;
    void* customUser = nullptr;
    El* first = nullptr;
    El* last = nullptr;
    El* next = nullptr;
    float x = 0, y = 0, w = 0, h = 0;

    gpui::Bounds Bounds() const { return {x, y, w, h}; }
    float scrollY = 0;

    float scrollX = 0;

    ScrollbarMode scrollMode = ScrollbarMode::Always;
    bool scrollModeSet = false;

    bool noScrollbar = false;

    bool noScrollbarX = false;
    bool noScrollbarY = false;
    int scrollId = 0;

    bool scrollFromPath = false;
    float contentW = 0;
    float contentH = 0;
    int selLo = -1;
    int selHi = -1;

    const TextSpan* spans = nullptr;
    int nSpans = 0;

    const TextSpan* washes = nullptr;
    int nWashes = 0;

    const TextSpan* underlines = nullptr;
    int nUnderlines = 0;

    int rangeOutLo = -1;
    int rangeOutHi = -1;
    gpui::Bounds* rangeOut = nullptr;
    float* caretOutX = nullptr;
    float* caretOutY = nullptr;
    Rgba selColor = Rgba8(0x6b, 0xb3, 0xf0, 90);

    int markLo = -1;
    int markHi = -1;
    bool selectable = false;

    const SelSource* selSrc = nullptr;
    bool selJoin = false;

    int caretOff = -1;
    Rgba caretColor = {};
    float caretW = 2;

    uint64_t layoutNode = 0;
    float laidFont = 0;
    float laidMaxW = 0;

    TextLayout* laidLayout = nullptr;

    float measKeyW[4] = {};
    Size measSize[4] = {};
    uint8_t measCount = 0;
    uint8_t measNext = 0;

    El* Flex();
    El* FlexRow();
    El* FlexCol();
    El* FlexRowReverse();
    El* FlexColReverse();
    El* FlexWrap();
    El* Grow(float g = 1);
    El* Shrink0();

    El* Flex1();

    El* FlexNone();
    El* Basis(float v);

    El* BasisFrac(float f);

    El* Shrink(float f);
    El* W(float v);
    El* WFrac(float f);

    El* Rotate(float turns);

    El* HideScrollbar();

    El* HideScrollbarX();
    El* HideScrollbarY();

    El* Opacity(float f);
    El* H(float v);
    El* SizeFull();
    El* MinH(float v);
    El* MinW(float v);
    El* MaxW(float v);
    El* MaxH(float v);
    El* Gap(float v);
    El* GapX(float v);
    El* GapY(float v);
    El* Pad(float v);
    El* PadX(float v);
    El* PadY(float v);
    El* PadL(float v);
    El* PadR(float v);
    El* PadT(float v);
    El* PadB(float v);
    El* ItemsCenter();
    El* ItemsStart();
    El* ItemsEnd();
    El* ItemsStretch();
    El* JustifyBetween();
    El* JustifyAround();
    El* JustifyCenter();
    El* JustifyEnd();
    El* JustifyStart();
    El* Bg(Background c);
    El* Border(float width, Rgba c);
    El* BorderT(float width, Rgba c);
    El* BorderB(float width, Rgba c);
    El* BorderL(float width, Rgba c);
    El* BorderR(float width, Rgba c);
    El* Radius(float r);

    El* Corners(float tl, float tr, float br, float bl);
    El* Fg(Rgba c);
    El* Font(float px);
    El* LineHeight(float mult);
    El* Truncate();
    El* ClipY();
    El* ScrollY(float off);
    El* ScrollX(float off);
    El* ClipX();
    El* ScrollMode(ScrollbarMode m);
    El* ScrollId(int v);
    El* Click(int v);

    El* PathId(Str name);

    El* PathClick(Str name);

    El* PathFocus(Str name);

    El* ScrollFromPath();
    El* OnClick(Func0 fn);
    El* OnClick(Listener l);

    El* OnScroll(Listener l);
    El* OnHover(Listener l);
    El* OnMouseDown(Listener l, DispatchPhase phase = DispatchPhase::Bubble);
    El* OnMouseUp(Listener l, DispatchPhase phase = DispatchPhase::Bubble);
    El* OnDragMove(Listener l);
    El* OnDrag(Str dragKind, int ix = 0, void* data = nullptr);
    El* OnMouseUpOut(Listener l);

    El* StopClick();
    El* OnDrop(Str acceptKind, Listener l);

    El* Refine(const Style& s, uint32_t fields);

    El* Hover(const struct StateStyle& s);

    El* DragOver(Str dragKind, const struct StateStyle& s);
    El* BoundsOut(gpui::Bounds* out);
    El* Cursor(CursorKind c);
    El* BindSlider(SliderState* s, Axis axis = Axis::Horizontal);
    El* BindSliderBounds(SliderState* s);
    El* BindInput(InputState* s);

    El* SelRange(int lo, int hi, Rgba color);

    El* CaretOut(float* outX, float* outY);

    El* RangeOut(int lo, int hi, gpui::Bounds* out);
    El* Washes(const TextSpan* runs, int n);
    El* Underlines(const TextSpan* runs, int n);
    El* Spans(const TextSpan* runs, int n);

    El* MarkRange(int lo, int hi);
    El* Caret(int off, Rgba color, float width = 2);
    El* Child(El* c);
    El* Bold();
    El* Semibold();
    El* Medium();
    El* Mono();
    El* Underline();
    El* Strikethrough();
    El* Italic();
    El* Selectable();

    El* SelSrc(const SelSource* s, bool join);
    El* Wrap();
    El* Dashed();
    El* DashArray(float on, float off);
    El* Absolute();
    El* Fixed();
    El* Deferred();
    El* AnchorBelow(float gap = 0);

    El* AnchorFlip(bool on = true);
    El* AnchorAbove(float gap = 0);
    El* AnchorCenterX();
    El* Top(float v);
    El* Left(float v);
    El* Bottom(float v);
    El* Right(float v);
    El* LeftRel(float frac);
    El* RightRel(float frac);
    El* HoverBg(Background c);
    El* HoverFg(Rgba c);

    El* ActiveBg(Background c);

    El* FocusOnPress(bool v = true);
    El* Group();
    El* GroupHoverVisible();

    El* GroupHoverBg(Background c);
    El* FocusId(int v);

    El* TrackFocus(FocusHandle handle);
    El* KeyContext(Str name);

    El* OnAction(uint32_t action, Listener fn);

    El* OnClickAction(uint32_t action, intptr_t arg = 0);

    El* OnKeyDown(Listener fn);
    El* TabIndex(int v);
    El* TabStop(bool v);
    El* FocusRing(bool v);
    El* TrapId(int v);
    El* Tip(Str s);
    El* Id(Str s);
};

enum class BtnKind : uint8_t {
    Default,
    Primary,
    Outline
};

El* ButtonEl(Arena* a, int clickId, Str label, BtnKind kind = BtnKind::Default);
El* ButtonSmall(Arena* a, int clickId, Str label, BtnKind kind, bool selected);

El* Div(Arena* a);
El* TextEl(Arena* a, Str s);
El* IconEl(Arena* a, IconName name);
El* IconEl(Arena* a, IconName name, float size);

El* ImageEl(Arena* a, Str src, Str alt = {});
El* ProgressEl(Arena* a, float value01to100, float barW, float barH);
El* ChartEl(Arena* a, const float* ys, int n, Rgba stroke, Rgba fillTop,
            Rgba fillBot, int tickMargin);

struct HitRect {

    int focusId = 0;
    int id = 0;
    Bounds bounds = {};
    Func0 onClick;
    Listener listener;
    Listener onHover;
    Listener onMouseDown;
    Listener onMouseUp;

    DispatchPhase mouseDownPhase = DispatchPhase::Bubble;
    DispatchPhase mouseUpPhase = DispatchPhase::Bubble;

    int parent = -1;
    Listener onDragMove;
    DragPayload drag = {};
    Listener onMouseUpOut;
    Str dropKind = {};
    Listener onDrop;
    CursorKind cursor = CursorKind::Arrow;

    Str tooltip = {};
    SliderState* slider = nullptr;
    Axis sliderAxis = Axis::Horizontal;
    InputState* input = nullptr;

    uint32_t clickAction = 0;
    intptr_t clickActionArg = 0;

    bool stopClick = false;
};

struct ScrollRect {
    int id = 0;
    Bounds bounds = {};
    float contentH = 0;
    float scrollY = 0;
    float contentW = 0;
    float scrollX = 0;
    ScrollbarMode mode = ScrollbarMode::Always;

    bool barX = true;
    bool barY = true;

    bool barVisible = true;
    Listener onScroll;

    InputState* input = nullptr;
};

const float kScrollbarThumbW = 6.f;
const float kScrollbarThumbActiveW = 8.f;
const float kScrollbarThumbMargin = 4.f;
const float kScrollbarBandW =
    kScrollbarThumbActiveW + kScrollbarThumbMargin * 2.f;

struct ScrollEvent {
    int id = 0;
    float offsetY = 0;

    float offsetX = 0;
};

struct TextHit {
    Bounds bounds = {};
    Str text;
    float font = 14;
    float maxW = 0;
    bool wrap = false;
    int docOff = 0;

    const SelSource* src = nullptr;

    bool join = false;

    bool atom = false;

    int scope = 0;
};

struct TextMeasCache {
    void* slots = nullptr;
    int cap = 0;
    int used = 0;
    uint32_t frame = 0;
};

struct PaintApp;
struct PaintTarget;

enum StyleField : uint32_t {
    StyleFieldBg = 1u << 0,
    StyleFieldColor = 1u << 1,
    StyleFieldBorderColor = 1u << 2,
    StyleFieldPad = 1u << 3,
    StyleFieldGap = 1u << 4,
    StyleFieldRadius = 1u << 5,
    StyleFieldBorder = 1u << 6,
    StyleFieldFontSize = 1u << 7,
    StyleFieldWidth = 1u << 8,
    StyleFieldHeight = 1u << 9,
    StyleFieldOpacity = 1u << 10,

    StyleFieldHoverBg = 1u << 11,
    StyleFieldHoverFg = 1u << 12,
    StyleFieldActiveBg = 1u << 13,

    StyleFieldBorderT = 1u << 14,
    StyleFieldBorderB = 1u << 15,
    StyleFieldBorderL = 1u << 16,
    StyleFieldBorderR = 1u << 17
};

void StyleApplyFields(Style* into, const Style& over, uint32_t fields);

void StyleOverrideSet(int clickId, uint32_t fields, const Style& style);
void StyleOverrideClear(int clickId);
void StyleOverrideClearAll();

void StyleOverrideApply(El* e);

struct InspectorPick {
    int id = 0;
    Str elId = {};

    Style style = {};
    Bounds bounds = {};

    int kind = 0;
    int depth = 0;
    bool hasBg = false;
    Rgba bg = {};
    float pad = 0;
    float gap = 0;
    float radius = 0;
    float border = 0;
    bool row = true;
    float font = 0;

    Str text = {};
};

struct InspectorState {
    bool on = false;
    bool picking = false;
    bool hasPick = false;
    InspectorPick pick = {};

    bool pending = false;
    float pendingX = 0;
    float pendingY = 0;
};

struct PaintCtx {
    PaintApp* pa = nullptr;
    PaintTarget* rt = nullptr;

    float opacity = 1;
    float dpi = 96;
    float viewW = 0;
    float viewH = 0;
    int hoverId = 0;

    int dragOverId = 0;
    Str dragKind = {};

    int activeId = 0;

    bool groupHovered = false;
    int focusId = 0;

    int focusGen = 0;

    float mouseX = -1;
    float mouseY = -1;

    int scrollDragId = 0;
    bool scrollDragHorizontal = false;

    bool wantsAnimFrame = false;

    int hitParent = -1;

    bool picking = false;
    bool pickHit = false;

    int paintDepth = 0;

    int paintLayer = 0;

    int pickTier = 0;
    InspectorPick pick = {};
    Vec<HitRect> hits;
    Vec<ScrollRect> scrolls;
    Vec<TextHit> texts;

    Vec<InputState*> inputs;
    int textDocLen = 0;
    int selA = -1;
    int selB = -1;

    int selScope = -1;
    TextMeasCache textCache;

    PaintCtx() = default;
};

struct FocusRect {
    int id = 0;
    int trapId = 0;
    int tabIndex = 0;
    bool tabStop = true;
    bool focusOnPress = false;

    int dispatchIx = 0;
    Bounds bounds = {};
};

struct DispatchNode {
    int subtreeEnd = 0;
    uint32_t context = 0;
    uint32_t action = 0;
    Listener fn = {};
};

enum class CharKind : uint8_t {
    Word,
    Whitespace,
    Newline,
    Other
};

int Utf8At(Str s, int i, uint32_t* out);

int Utf8Prev(Str s, int i);

enum class Bias : uint8_t {
    Left,
    Right
};

struct RopePoint {
    int row = 0;
    int column = 0;
};

int RopeClipOffset(Str text, int offset, Bias bias);

int RopeCharAt(Str text, int offset, uint32_t* out);
int RopeLinesLen(Str text);
int RopeLineStartOffset(Str text, int row);
int RopeLineEndOffset(Str text, int row);

Str RopeSliceLine(Str text, int row);
int RopeLineLen(Str text, int row);
RopePoint RopeOffsetToPoint(Str text, int offset);
int RopePointToOffset(Str text, RopePoint point);
int RopeOffsetUtf16ToOffset(Str text, int offsetUtf16);
int RopeOffsetToOffsetUtf16(Str text, int offset);
int RopeCharIndexToOffset(Str text, int charIndex);
int RopeOffsetToCharIndex(Str text, int offset);

enum class InputEventKind : uint8_t {
    Change,
    PressEnter,
    Focus,
    Blur
};

struct InputEvent {
    InputEventKind kind = InputEventKind::Change;

    bool secondary = false;
    bool shift = false;
};

struct Selection {
    int start = 0;
    int end = 0;

    int Len() const { return end > start ? end - start : 0; }
    bool IsEmpty() const { return start == end; }
    bool Contains(int offset) const { return offset >= start && offset < end; }
};

inline Selection SelectionAt(int offset) {
    return Selection{offset, offset};
}

enum class EditIntent : uint8_t {
    Typing,
    Backspace,
    DeleteForward,
    Atomic
};

struct Change {
    Selection oldRange = {};
    Str oldText = {};
    Selection newRange = {};
    Str newText = {};
    Selection selBefore = {};
    Selection selAfter = {};
};

struct UndoTransaction {
    EditIntent intent = EditIntent::Atomic;
    Change* changes = nullptr;
    int len = 0;
    int cap = 0;
};

struct UndoManager {
    Vec<UndoTransaction> undos;
    Vec<UndoTransaction> redos;
    bool ignoring = false;
    bool transactionOpen = false;
    bool hasPending = false;
    Change pending = {};

    bool hasPendingIntent = false;
    EditIntent pendingIntent = EditIntent::Atomic;
    bool coalescingBoundary = false;

    ~UndoManager();
};

void UndoRecordTransaction(UndoManager* m, Change change, EditIntent intent);
void UndoBeginTransaction(UndoManager* m);
void UndoCommitTransaction(UndoManager* m);
void UndoBreakCoalescing(UndoManager* m);
void UndoSetIgnoring(UndoManager* m, bool ignoring);
bool UndoIsIgnoring(const UndoManager* m);
void UndoClear(UndoManager* m);

const UndoTransaction* UndoPopUndo(UndoManager* m);
const UndoTransaction* UndoPopRedo(UndoManager* m);

enum class MaskToken : uint8_t {
    Digit,
    Letter,
    LetterOrDigit,
    Any,
    Sep
};

enum class MaskKind : uint8_t {
    None,
    Pattern,
    Number
};

struct MaskPattern {
    MaskKind kind = MaskKind::None;

    Str pattern = {};

    uint32_t separator = 0;

    int fraction = -1;
};

MaskPattern MaskPatternNew(Str pattern);
MaskPattern MaskPatternNumber(uint32_t separator);
void MaskPatternFree(MaskPattern* p);

bool MaskTokenAt(const MaskPattern& p, int pos, MaskToken* out, uint32_t* sep);
bool MaskIsNone(const MaskPattern& p);
bool MaskIsValid(const MaskPattern& p, Str maskText);
bool MaskIsValidAt(const MaskPattern& p, uint32_t ch, int pos);

Str MaskApply(Arena* a, const MaskPattern& p, Str text);

Str MaskUnapply(Arena* a, const MaskPattern& p, Str maskText);

Str MaskPlaceholder(Arena* a, const MaskPattern& p);

Str NormalizeNumberInput(Arena* a, Str text);

enum class InputKind : uint8_t {
    Input,
    Textarea,
    Editor
};

enum class LayoutModeKind : uint8_t {
    PlainText,
    AutoGrow,
    CodeEditor
};

struct LayoutMode {
    LayoutModeKind kind = LayoutModeKind::PlainText;
    int rows = 1;
    int minRows = 1;
    int maxRows = 0;
    int tabSize = 4;
    bool lineNumber = false;

    bool folding = false;
};

bool LayoutModeIsFolding(const LayoutMode& m);

void LayoutModeSetRows(LayoutMode* m, int rows);
int LayoutModeRows(const LayoutMode& m);
int LayoutModeMinRows(const LayoutMode& m);

const float kAutoScrollMinSpeed = 12.f;
const float kAutoScrollMaxSpeed = 64.f;

const float kAutoScrollInnerZone = 16.f;

const float kAutoScrollOuterRamp = 80.f;

bool AutoScrollComputeDelta(float y, Bounds bounds, float* out);

struct AutoScroll {

    float delta = 0;
    bool active = false;

    Point lastDrag = {};
    bool hasLastDrag = false;

    bool IsActive() const { return active; }

    void Set(float d) {
        delta = d;
        active = true;
    }
    void SetNone() {
        delta = 0;
        active = false;
    }

    void Stop() {
        SetNone();
        lastDrag = {};
        hasLastDrag = false;
    }
};

struct SearchMatcher {

    Vec<Selection> ranges;
    int current = 0;

    bool caseInsensitive = true;

    Str query = {};

    Vec<char> text;

    bool replacing = false;

    ~SearchMatcher() {
        ranges.Reset();
        text.Reset();
        StrFree(query);
    }
};

void SearchMatcherReset(SearchMatcher* m);

void SearchMatcherUpdate(SearchMatcher* m, Str text);
void SearchMatcherUpdateQuery(SearchMatcher* m, Str query, bool insensitive);
inline int SearchMatcherLen(const SearchMatcher* m) {
    return m->ranges.len;
}
inline bool SearchMatcherIsEmpty(const SearchMatcher* m) {
    return m->ranges.len == 0;
}
inline int SearchMatcherIndex(const SearchMatcher* m) {
    return m->current;
}

Str SearchMatcherLabel(Arena* a, const SearchMatcher* m);

void SearchMatcherSetIndex(SearchMatcher* m, int ix);
void SearchMatcherBeginReplacement(SearchMatcher* m);
bool SearchMatcherHasNextWithoutWrap(const SearchMatcher* m);

bool SearchMatcherPeek(const SearchMatcher* m, Selection* out);

bool SearchMatcherCurrent(const SearchMatcher* m, Selection* out);

void SearchMatcherCursorByOffset(SearchMatcher* m, int offset);

bool SearchMatcherNext(SearchMatcher* m, Selection* out);
bool SearchMatcherPrev(SearchMatcher* m, Selection* out);

struct FoldRange {
    int startLine = 0;
    int endLine = 0;
};

struct FoldMap {

    Vec<FoldRange> candidates;

    Vec<FoldRange> folded;

    Vec<int> visibleLines;

    Vec<int> lineToDisplayRow;
    bool needsRebuild = true;

    int cachedLineCount = 0;
};

void FoldMapSetCandidates(FoldMap* m, const FoldRange* ranges, int n);

void FoldMapSetFolded(FoldMap* m, int startLine, bool folded);
void FoldMapToggle(FoldMap* m, int startLine);
bool FoldMapIsFolded(const FoldMap* m, int startLine);
bool FoldMapIsCandidate(const FoldMap* m, int startLine);

void FoldMapClearFolds(FoldMap* m);

void FoldMapAdjustForEdit(FoldMap* m, int editStartLine, int editEndLine,
                          int lineDelta);

void FoldMapRebuild(FoldMap* m, int lineCount);

int FoldMapDisplayRowCount(const FoldMap* m);

int FoldMapDisplayRow(const FoldMap* m, int line);
int FoldMapLineAt(const FoldMap* m, int displayRow);

bool FoldMapLineHidden(const FoldMap* m, int line);

int FoldMapNearestVisibleLine(const FoldMap* m, int line);

struct FoldIconBox {
    int line = 0;
    Bounds bounds = {};
};

struct SearchSession {
    bool open = false;
    bool replaceMode = false;
    bool caseInsensitive = true;
    Str query = {};
    Str replacement = {};

    int anchorOffset = -1;
    SearchMatcher matcher;

    ~SearchSession() {
        StrFree(query);
        StrFree(replacement);
    }
};

void SearchSessionSetQuery(SearchSession* s, Str query, bool insensitive);
void SearchSessionSetReplacement(SearchSession* s, Str replacement);

enum class DiagnosticSeverity : uint8_t {
    Hint,
    Error,
    Warning,
    Info
};

struct Diagnostic {
    Selection range = {};
    DiagnosticSeverity severity = DiagnosticSeverity::Info;
    Str message = {};
    Str source = {};
    Str code = {};
};

struct TextEditItem {
    Selection range = {};
    Str newText = {};
};

void InputApplyEdits(InputState* s, App* app, Window* win,
                     const TextEditItem* edits, int n);

struct CompletionItem {

    Str label = {};

    Str detail = {};

    Str insertText = {};

    Str documentation = {};
    bool deprecated = false;

    bool resolved = false;

    const TextEditItem* additionalEdits = nullptr;
    int nAdditionalEdits = 0;
};

using CompletionFn = int (*)(void* data, Str text, int offset, Str query,
                             CompletionItem* out, int cap);

struct DocumentColor {
    Selection range = {};
    Rgba color = {};
};

using DocumentColorFn = int (*)(void* data, Str text, DocumentColor* out,
                                int cap);

struct CodeActionItem {
    Str title = {};

    int provider = 0;
    Selection range = {};
    Str newText = {};

    const TextEditItem* edits = nullptr;
    int nEdits = 0;
};

struct CodeActionItem;
using CodeActionPerformFn = bool (*)(void* data, InputState* s, App* app,
                                     Window* win, const CodeActionItem* item);

using CodeActionFn = int (*)(void* data, Arena* a, Str text, Selection sel,
                             CodeActionItem* out, int cap);

struct CodeActionSession {
    bool open = false;
    int selected = 0;
    Vec<CodeActionItem> items;

    uint64_t revision = 0;

    Arena* arena = nullptr;

    ~CodeActionSession();
};

using HoverFn = Str (*)(void* data, Str text, int offset);

enum class CompletionTrigger : uint8_t {

    Continue,

    Open,

    Close
};

using CompletionTriggerFn = CompletionTrigger (*)(void* data, Str text,
                                                  int offset, Str typed);

using CompletionResolveFn = Str (*)(void* data, Arena* a,
                                    const CompletionItem* item);

const float kCompletionMenuMaxW = 320.f;

using InlineCompletionFn = Str (*)(void* data, Arena* a, Str text, int offset);

const float kInlineCompletionDebounceMs = 300.f;

struct InlineCompletion {

    Str text = {};
    int at = -1;

    double dueAt = 0;
    bool asked = true;
    Arena* arena = nullptr;

    ~InlineCompletion();
};

struct SemanticToken {
    uint32_t deltaLine = 0;
    uint32_t deltaStart = 0;
    uint32_t length = 0;
    uint32_t tokenType = 0;
    uint32_t tokenModifiers = 0;
};

struct SemanticSpan {
    int line = 0;
    int col = 0;
    int len = 0;
    Str name = {};
};

struct SemanticRange {
    Selection range = {};
    Str name = {};
};

using SemanticTokensFn = int (*)(void* data, Str text, Selection range,
                                 SemanticToken* out, int cap);

int SemanticTokensDecode(const SemanticToken* toks, int n, const Str* names,
                         int nNames, SemanticSpan* out, int cap);

int SemanticTokensForRange(const SemanticSpan* toks, int n, Str text,
                           Selection visible, SemanticRange* out, int cap);

struct DefinitionLink {

    Selection origin = {};
    Str uri = {};

    Selection target = {};
};

using DefinitionFn = int (*)(void* data, Arena* a, Str text, int offset,
                             DefinitionLink* out, int cap);

using ShowDocumentFn = bool (*)(void* data, Str uri, bool external,
                                Selection selection);

struct HoverDefinition {
    Selection symbolRange = {};
    Vec<DefinitionLink> locations;
    Selection lastRange = {};
    Vec<DefinitionLink> lastLocations;

    Bounds bounds = {};

    Arena* arena = nullptr;

    ~HoverDefinition();
};

struct CompletionSession {
    bool open = false;

    int triggerStart = -1;
    int offset = 0;
    int selected = 0;

    Vec<CompletionItem> items;

    uint64_t revision = 0;

    Arena* arena = nullptr;

    ~CompletionSession();
};

struct DiagnosticColors {
    Rgba error = {};
    Rgba warning = {};
    Rgba info = {};
    Rgba hint = {};
};

enum class InputAction : uint8_t;

enum class InputOverlayKind : uint8_t {
    Completion,
    CodeAction
};

using OverlayActionFn = bool (*)(void* data, InputOverlayKind kind,
                                 InputAction action);

struct InputState {
    InputKind kind = InputKind::Input;
    LayoutMode mode = {};

    Vec<char> text;
    Selection selectedRange = {};
    bool selectionReversed = false;

    bool hasSelectedWordRange = false;
    Selection selectedWordRange = {};
    UndoManager undo;
    MaskPattern maskPattern = {};
    bool maskPatternSet = false;
    Str placeholder = {};
    bool focused = false;

    Window* focusWin = nullptr;
    bool disabled = false;
    bool readonly = false;
    bool loading = false;

    bool masked = false;
    bool cleanOnEscape = false;
    bool submitOnEnter = false;

    bool searchable = false;
    bool replaceable = true;
    SearchSession search;

    FoldMap folds;
    Vec<FoldIconBox> foldIcons;

    Bounds gutterBox = {};
    bool softWrap = true;

    int align = 0;

    bool selecting = false;

    AutoScroll autoScroll;

    EntityId blink = {};
    Listener onChange = {};

    bool (*validate)(Str text, intptr_t arg) = nullptr;
    intptr_t validateArg = 0;

    Bounds lastBounds = {};
    float lastFont = 0;
    float lastLineH = 0;

    bool lastMono = false;

    Vec<Bounds> rowBoxes;

    Vec<Diagnostic> diagnostics;

    int hoverDiagnostic = -1;
    float hoverDiagnosticX = 0;
    float hoverDiagnosticY = 0;

    HoverFn hoverProvider = nullptr;
    void* hoverData = nullptr;
    Str hoverText = {};
    Selection hoverRange = {};
    float hoverX = 0;
    float hoverY = 0;

    double hoverDueAt = 0;
    Selection hoverPending = {};
    bool hoverAsked = true;

    DocumentColorFn documentColorProvider = nullptr;
    void* documentColorData = nullptr;
    Vec<DocumentColor> documentColors;
    bool documentColorsDirty = true;

    CodeActionSession codeActions;

    static const int kMaxCodeActionProviders = 4;
    CodeActionFn codeActionProviders[kMaxCodeActionProviders] = {};
    void* codeActionDatas[kMaxCodeActionProviders] = {};

    CodeActionPerformFn codeActionPerform[kMaxCodeActionProviders] = {};
    int nCodeActionProviders = 0;

    CodeActionFn codeActionProvider = nullptr;
    void* codeActionData = nullptr;

    InlineCompletionFn inlineCompletionProvider = nullptr;
    void* inlineCompletionData = nullptr;
    InlineCompletion inlineCompletion;

    SemanticTokensFn semanticTokensProvider = nullptr;
    void* semanticTokensData = nullptr;

    const Str* semanticLegend = nullptr;
    int nSemanticLegend = 0;
    Vec<SemanticSpan> semanticTokens;
    bool semanticTokensDirty = true;

    DefinitionFn definitionProvider = nullptr;
    void* definitionData = nullptr;
    ShowDocumentFn showDocument = nullptr;
    void* showDocumentData = nullptr;
    HoverDefinition hoverDef;

    CompletionSession completion;
    CompletionFn completionProvider = nullptr;
    void* completionData = nullptr;

    OverlayActionFn overlayAction = nullptr;
    void* overlayActionData = nullptr;

    bool silentReplace = false;

    CompletionTriggerFn completionTrigger = nullptr;
    CompletionResolveFn completionResolve = nullptr;

    float completionMenuMaxW = kCompletionMenuMaxW;

    Bounds contentBox = {};

    Bounds inputBounds = {};

    float scrollX = 0;
    float scrollY = 0;
    float viewW = 0;
    float viewH = 0;

    float caretWinX = 0;
    float caretWinY = 0;

    float caretX = 0;
    float contentW = 0;
    float contentH = 0;

    bool emitEvents = true;

    Selection imeMarked = {};
    bool imeMarking = false;

    int preferredColumn = -1;

    float preferredX = -1;

    ~InputState();
};

enum class InputMoveDir : uint8_t {
    None,
    Up,
    Down
};

void InputScrollToCaret(InputState* s, float caretX, float caretY,
                        InputMoveDir dir);

void InputScrollToOffset(InputState* s, int offset, InputMoveDir dir);

void InputScrollToCursor(InputState* s, InputMoveDir dir);

Str InputValue(const InputState* s);
const char* InputCStr(const InputState* s);

Str InputUnmaskValue(Arena* a, const InputState* s);

Str InputSelectedValue(const InputState* s);
bool InputIsMultiLine(const InputState* s);
bool InputIsSingleLine(const InputState* s);
bool InputIsEditable(const InputState* s);

bool InputIsCopyable(const InputState* s);

int InputCursor(const InputState* s);

RopePoint InputCursorPosition(const InputState* s);

void InputSetValue(InputState* s, Str value);

void InputReplaceAll(InputState* s, App* app, Window* win, Str value);
void InputSetPlaceholder(InputState* s, Str value);
void InputSetMaskPattern(InputState* s, MaskPattern pattern);

void InputClean(InputState* s, App* app, Window* win);

void InputInsert(InputState* s, App* app, Window* win, Str value);

int InputPreviousBoundary(const InputState* s, int offset);
int InputNextBoundary(const InputState* s, int offset);

int InputStartOfLine(const InputState* s, Window* win = nullptr);
int InputEndOfLine(const InputState* s, Window* win = nullptr);
int InputPreviousStartOfWord(const InputState* s);
int InputNextEndOfWord(const InputState* s);

void InputMoveTo(InputState* s, App* app, Window* win, int offset);

void InputSelectTo(InputState* s, App* app, Window* win, int offset);
void InputSelectAll(InputState* s, App* app, Window* win);
void InputUnselect(InputState* s, App* app, Window* win);
void InputSetSelectedRange(InputState* s, App* app, Window* win, int a, int b);

void InputSelectWord(InputState* s, App* app, Window* win, int offset);
void InputSelectLine(InputState* s, App* app, Window* win, int offset);

enum class InputAction : uint8_t {
    None,
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    MoveHome,
    MoveEnd,
    MoveToStart,
    MoveToEnd,
    MoveToPreviousWord,
    MoveToNextWord,
    MovePageUp,
    MovePageDown,
    SelectLeft,
    SelectRight,
    SelectUp,
    SelectDown,
    SelectAll,
    SelectToStart,
    SelectToEnd,
    SelectToStartOfLine,
    SelectToEndOfLine,
    SelectToPreviousWordStart,
    SelectToNextWordEnd,
    Backspace,
    Delete,
    DeleteToBeginningOfLine,
    DeleteToEndOfLine,
    DeleteToPreviousWordStart,
    DeleteToNextWordEnd,
    Enter,
    Escape,

    IndentInline,
    OutdentInline,

    Indent,
    Outdent,
    Copy,
    Cut,
    Paste,
    Undo,
    Redo,

    Search,
    Replace,

    ToggleCodeActions
};

InputAction InputActionForKey(const InputState* s, int vk, bool shift,
                              bool ctrl, bool alt, bool platform = false);

bool InputPerform(InputState* s, App* app, Window* win, InputAction action,
                  bool shift);

Str InputCompletionQuery(const InputState* s, int* startOut);

void InputRequestCompletion(InputState* s, App* app, Window* win, bool force);

void InputDismissCompletion(InputState* s);

void InputAcceptCompletion(InputState* s, App* app, Window* win);

bool InputCompletionAction(InputState* s, App* app, Window* win,
                           InputAction action);

void InputShowCompletions(InputState* s, App* app, Window* win);

void InputUpdateDocumentColors(InputState* s);

void InputPresentCompletionItems(InputState* s, int triggerStart, Str query,
                                 const CompletionItem* items, int n);

void InputPresentCodeActions(InputState* s, const CodeActionItem* items, int n);

void InputPresentHover(InputState* s, Selection symbolRange, Str text);
void InputPresentDiagnostic(InputState* s, int index);
void InputClearDiagnosticPopover(InputState* s);

bool InputRouteOverlayAction(InputState* s, App* app, Window* win,
                             InputAction action);

void InputDismissLspOverlays(InputState* s);

bool InputIsContextMenuOpen(const InputState* s);

void InputInsertCompletion(InputState* s, App* app, Window* win,
                           const CompletionItem* item, Selection fallback);

Str InputCompletionDocumentation(InputState* s);

void InputScheduleInlineCompletion(InputState* s);

bool InputUpdateInlineCompletion(InputState* s, bool menuOpen);

bool InputHasInlineCompletion(const InputState* s);
void InputClearInlineCompletion(InputState* s);
bool InputAcceptInlineCompletion(InputState* s, App* app, Window* win);

void InputAddCodeActionProvider(InputState* s, CodeActionFn fn, void* data,
                                CodeActionPerformFn perform = nullptr);

void InputLspUpdate(InputState* s);

void InputLspReset(InputState* s);

void InputUpdateSemanticTokens(InputState* s);

void InputHoverDefinition(InputState* s, int offset);

void InputClearHoverDefinition(InputState* s);

bool InputClickDefinition(InputState* s, App* app, Window* win, int offset,
                          bool secondary);

void InputGoToDefinition(InputState* s, App* app, Window* win);

bool InputCanGoToDefinition(const InputState* s);

void InputFollowDefinition(InputState* s, App* app, Window* win,
                           const DefinitionLink& link);

void InputToggleCodeActions(InputState* s, App* app, Window* win);
void InputDismissCodeActions(InputState* s);

void InputPerformCodeAction(InputState* s, App* app, Window* win);

bool InputCodeActionAction(InputState* s, App* app, Window* win,
                           InputAction action);

bool InputReplaceTextInRange(InputState* s, App* app, Window* win,
                             const Selection* range, Str newText);

int Utf8OffsetToUtf16(Str s, int u8);
int Utf16OffsetToUtf8(Str s, int u16);

bool InputMarkedRange(const InputState* s, Selection* out);

void InputReplaceAndMarkText(InputState* s, App* app, Window* win,
                             const Selection* range, Str newText,
                             const Selection* sel);

void InputUnmarkText(InputState* s, App* app, Window* win);

void InputTypeChar(InputState* s, App* app, Window* win, uint32_t ch);

void InputOpenSearch(InputState* s, App* app, Window* win, bool replaceMode);
void InputCloseSearch(InputState* s, App* app, Window* win);

bool InputIsReplaceable(const InputState* s);
void InputSetSearchReplaceMode(InputState* s, App* app, Window* win, bool on);
void InputSetSearchQuery(InputState* s, App* app, Window* win, Str query,
                         bool insensitive);

bool InputSearchNext(InputState* s, App* app, Window* win, Selection* out);
bool InputSearchPrev(InputState* s, App* app, Window* win, Selection* out);

bool InputSearchReplaceOne(InputState* s, App* app, Window* win, Str with);

int InputSearchReplaceAll(InputState* s, App* app, Window* win, Str with);

void InputUpdateSearch(InputState* s);

void InputFocus(InputState* s, App* app, Window* win);
void InputBlur(InputState* s, App* app, Window* win);

int InputIndexForPosition(const InputState* s, PaintCtx* ctx, float x, float y);

int InputFoldIconAt(const InputState* s, float x, float y);

void InputToggleFold(InputState* s, App* app, Window* win, int line);

void InputSetFoldCandidates(InputState* s, const FoldRange* ranges, int n);

InputState* InputAtPosition(PaintCtx* ctx, float x, float y);

struct SliderValue {
    float lo = 0;
    float hi = 0;
    bool range = false;

    float Start() const { return range ? lo : hi; }
    float End() const { return hi; }
};

inline SliderValue SliderSingle(float v) {
    return {0, v, false};
}
inline SliderValue SliderRange(float lo, float hi) {
    return {lo, hi, true};
}

SliderValue SliderValueClamp(SliderValue v, float min, float max);

void SliderValueSetStart(SliderValue* v, float value);
void SliderValueSetEnd(SliderValue* v, float value);

enum class SliderScale : uint8_t {
    Linear,
    Logarithmic
};

enum class SliderEventKind : uint8_t {
    Change,
    Release
};

struct SliderEvent {
    SliderEventKind kind = SliderEventKind::Change;
    SliderValue value = {};
};

struct SliderState {
    float min = 0;
    float max = 100;
    float step = 1;
    SliderValue value = {};

    float pctLo = 0;
    float pctHi = 0;

    Bounds bounds = {};
    SliderScale scale = SliderScale::Linear;

    bool dragging = false;

    bool dragStart = false;
    Listener onChange = {};
};

SliderState SliderStateNew(float min, float max, SliderValue value,
                           float step = 1,
                           SliderScale scale = SliderScale::Linear);

void SliderSetLimits(SliderState* s, float min, float max);

void SliderSetStep(SliderState* s, float step);

void SliderSetScale(SliderState* s, SliderScale scale);

void SliderSetValue(SliderState* s, SliderValue v);

inline void SliderSetBounds(SliderState* s, Bounds b) {
    s->bounds = b;
}

float SliderPctToValue(const SliderState* s, float pct);
float SliderValueToPct(const SliderState* s, float value);

void SliderUpdateThumbPos(SliderState* s);

bool SliderUpdateByPosition(SliderState* s, Axis axis, Point pos, bool isStart);

bool SliderIsStartAt(const SliderState* s, Axis axis, Point pos);

bool SliderHandleRelease(SliderState* s);

bool SliderStepBy(SliderState* s, int dir, bool isStart);

struct Overlay {
    int kind = 0;
    char title[128] = {};
    char body[2048] = {};
};

struct MenuState {
    bool open = false;
    float x = 0, y = 0;
    char items[8][32] = {};
    int nItems = 0;
    int clickBase = 0;
};

struct WinSize {
    float dipW = 0;
    float dipH = 0;
    int pxW = 0;
    int pxH = 0;
};

float PxToDip(PaintCtx* ctx, int px);
int DipToPx(PaintCtx* ctx, float dip);

Size MeasureText(PaintCtx* ctx, Str s, float fontSize, float maxW,
                 bool wrap = false, int weight = 0, float lineH = 0);

void DrawTextBaseline(PaintCtx* ctx, Str s, float x, float baselineY,
                      float fontSize, Rgba color, int weight = 0);
void TextMeasBeginFrame(PaintCtx* ctx);
void TextMeasEndFrame(PaintCtx* ctx);
void TextMeasClear(PaintCtx* ctx);

bool TextPointAt(PaintCtx* ctx, Str s, float fontSize, float maxW, bool wrap,
                 int off, float* outX, float* outY, float* outH,
                 bool mono = false, float lineHeight = 0);
int TextIndexAt(PaintCtx* ctx, Str s, float fontSize, float maxW, bool wrap,
                float relX, float relY, bool mono = false,
                float lineHeight = 0);

void PaintTextRange(PaintCtx* ctx, Str s, float fontSize, float maxW, bool wrap,
                    uint8_t weight, float lineH, float x, float y, int u8a,
                    int u8b, Rgba color);
void PaintTextUnderline(PaintCtx* ctx, Str s, float fontSize, float maxW,
                        bool wrap, float x, float y, int u8a, int u8b,
                        Rgba color, bool wavy = false);

struct LayoutCache;

LayoutCache* LayoutCacheNew();
void LayoutCacheFree(LayoutCache* lc);

struct LayoutCacheStats {

    int nodes = 0;

    int made = 0;
    int dropped = 0;

    int restyled = 0;
    int remeasured = 0;
};

LayoutCacheStats LayoutCacheLastStats(const LayoutCache* lc);

void LayoutScratchFree();

void LayoutEl(PaintCtx* ctx, El* e, float x, float y, float availW,
              float availH, float inheritFont, Rgba inheritFg,
              LayoutCache* lc = nullptr);

Size MeasureEl(PaintCtx* ctx, El* e, float inheritFont = 0,
               Rgba inheritFg = {});
void PaintEl(PaintCtx* ctx, El* e);
int HitTest(PaintCtx* ctx, float x, float y);
const HitRect* HitTestRect(PaintCtx* ctx, float x, float y);

const HitRect* HitTestDrop(PaintCtx* ctx, float x, float y, Str kind);
const ScrollRect* HitScrollRect(PaintCtx* ctx, float x, float y);
int TextHitOffsetAt(PaintCtx* ctx, float x, float y, bool nearest);

int TextHitOffsetIn(PaintCtx* ctx, float x, float y, bool nearest, int scope,
                    int* outScope);
int CopyTextHits(PaintCtx* ctx, int selA, int selB, char* out, int cap);

int CopyTextHitsIn(PaintCtx* ctx, int selA, int selB, int scope, char* out,
                   int cap, SelectionFormat fmt = SelectionFormat::Plain);

bool TextMultiClickRange(PaintCtx* ctx, float x, float y, int clickCount,
                         int* outA, int* outB);
bool TextMultiClickRangeIn(PaintCtx* ctx, float x, float y, int clickCount,
                           int scope, int* outA, int* outB, int* outScope);
int HashClickId(Str s);

enum class BoxFill : uint8_t {
    Base,
    Hover,
    Active
};
BoxFill BoxFillFor(bool hasActiveBg, bool hasHoverBg, int clickId, int activeId,
                   int hoverId);

bool ClickFromRelease(bool pending, int pressedId, MouseButton pressedButton,
                      bool dragged, int upId, MouseButton upButton);

bool ClickFromKeyRelease(bool pending, int pendingGen, int focusGen, int key,
                         bool modified);

enum {
    ClickWinMin = -1,
    ClickWinMax = -2,
    ClickWinClose = -3,
    ClickWinCaption = -4,
};

struct App;
struct Window;

struct WinOpts {
    bool borderless = false;

    bool clientTitleBar = false;
    bool anim = false;
    int timerMs = 500;
};

struct FrameTiming {
    float drawSecs = 0;
};

enum {
    kFrameTraceCap = 256
};

struct EntitySub {
    int id = 0;
    EntityId emitter = {};
    Listener handler = {};
};

struct App {
    PaintApp* paint = nullptr;
    ThemeMode themeMode = ThemeMode::Light;
    Vec<Window*> windows;

    Vec<EntitySlot> entities;
    Vec<int32_t> freeSlots;

    Vec<EntitySub> subs;

    Vec<EntitySub> observers;
    int nextSubId = 1;
    int exitCode = 0;

    App() = default;
};

struct PlatWindow;

struct WindowSelection;

struct Window {
    App* app = nullptr;
    PlatWindow* plat = nullptr;
    PaintCtx paint = {};
    Arena* frameArena = nullptr;

    LayoutCache* layout = nullptr;

    WindowSelection* sel = nullptr;

    EntityId root = {};

    Vec<EntityId> rendered;
    int hoverId = 0;
    int focusId = 0;

    int focusGen = 0;
    float mouseX = 0;
    float mouseY = 0;

    Modifiers mouseModifiers = {};

    CursorKind cursor = CursorKind::Arrow;
    bool maximized = false;

    bool active = true;
    bool running = true;
    bool anim = false;

    bool animFrame = false;

    double frameNow = 0;
    bool mouseDown = false;

    bool stopPropagation = false;

    double lastDownAt = 0;
    float lastDownX = 0;
    float lastDownY = 0;
    MouseButton lastDownButton = MouseButton::Left;
    int clickRun = 0;

    int pressedId = 0;

    int pressedCount = 1;

    bool pressPending = false;

    float pressedX = 0;
    float pressedY = 0;
    bool pressedMoved = false;
    MouseButton pressedButton = MouseButton::Left;
    Modifiers pressedModifiers = {};

    DragPayload activeDrag = {};
    int dragOverId = 0;

    float dragOffX = 0;
    float dragOffY = 0;
    bool eatReturn = false;

    bool eatChar = false;

    bool keyPressPending = false;
    int keyPressGen = 0;

    int scrollDragId = 0;
    float scrollDragGrab = 0;

    bool scrollDragHorizontal = false;
    InputState* input = nullptr;

    EntityId tooltip = {};
    Overlay overlay = {};
    InspectorState inspector = {};
    MenuState menu = {};
    Vec<FocusRect> focusEls;

    int pendingTrap = 0;

    int pendingTrapHost = 0;
    Vec<KeyedSlot> keyed;

    Vec<DispatchNode> dispatch;
    Vec<MotionSlotRec> motionSlots;
    WinOpts opts = {};

    Listener onKey = {};
    Listener onClick = {};
    Listener onMouseDown = {};
    Listener onMouseUp = {};
    Listener onMouseMove = {};
    Listener onMouseExit = {};
    Listener onScrollWheel = {};

    Vec<TimerSub> timers;
    int nextTimerId = 1;

    InputState* prevInput = nullptr;

    FrameTiming frameTrace[kFrameTraceCap] = {};
    uint64_t frameSeq = 0;

    Window() = default;

    ~Window();
};

struct Ctx {
    App* app = nullptr;
    Window* win = nullptr;
    Arena* a = nullptr;
    EntityId self = {};

    uint32_t path = 0;

    const Theme& theme() const;
    ThemeMode themeMode() const;
};

uint32_t IdFoldName(uint32_t parent, Str name);

struct IdScope {
    Ctx* cx = nullptr;
    uint32_t prev = 0;

    IdScope(Ctx* c, Str name) : cx(c), prev(c ? c->path : 0) {
        if (cx) {
            cx->path = IdFoldName(prev, name);
        }
    }
    ~IdScope() {
        if (cx) {
            cx->path = prev;
        }
    }
    IdScope(const IdScope&) = delete;
    IdScope& operator=(const IdScope&) = delete;
};

EntityId EntityNewRaw(App* app, void* ptr, RenderFn render, DropFn drop);
void* EntityGet(App* app, EntityId id);
void EntityDrop(App* app, EntityId id);
void EntityDropAll(App* app);

template <typename T>
struct Entity {
    EntityId id = {};

    bool IsValid() const { return id.IsValid(); }
    T* Get(App* app) const { return (T*)EntityGet(app, id); }
    T* Get(Ctx* cx) const { return (T*)EntityGet(cx->app, id); }
};

template <typename T>
void EntityDropT(void* p) {
    delete (T*)p;
}

template <typename T>
Entity<T> EntityNew(App* app) {
    Entity<T> e;
    e.id = EntityNewRaw(app, new T(), (RenderFn)&T::Render, &EntityDropT<T>);
    return e;
}

template <typename T>
Entity<T> EntityNew(Ctx* cx) {
    return EntityNew<T>(cx->app);
}

template <typename T>
Entity<T> EntityNewState(App* app) {
    Entity<T> e;
    e.id = EntityNewRaw(app, new T(), nullptr, &EntityDropT<T>);
    return e;
}

template <typename T, typename E>
Listener Listen(Ctx* cx, void (*fn)(T*, Ctx*, const E*)) {
    Listener l;
    l.fn = (void*)fn;
    l.view = cx->self;
    return l;
}

template <typename T, typename E>
Listener Listen(Ctx* cx, void (*fn)(T*, Ctx*, const E*, intptr_t),
                intptr_t arg) {
    Listener l;
    l.fn = (void*)fn;
    l.view = cx->self;
    l.arg = arg;
    l.hasArg = true;
    l.argBound = true;
    return l;
}

template <typename T, typename E>
Listener Listen(Ctx* cx, void (*fn)(T*, Ctx*, const E*, intptr_t)) {
    Listener l;
    l.fn = (void*)fn;
    l.view = cx->self;
    l.hasArg = true;
    return l;
}

inline Listener ListenerArg(Listener l, intptr_t arg) {
    if (l.IsValid()) {
        l.arg = arg;
        l.hasArg = true;
        l.argBound = true;
    }
    return l;
}

inline Listener ListenerFill(Listener l, intptr_t v) {
    if (l.IsValid() && !l.argBound) {
        l.arg = v;
        l.hasArg = true;
    }
    return l;
}

template <typename T, typename E>
Listener ListenTo(Entity<T> e, void (*fn)(T*, Ctx*, const E*)) {
    Listener l;
    l.fn = (void*)fn;
    l.view = e.id;
    return l;
}

template <typename T, typename E>
Listener ListenTo(Entity<T> e, void (*fn)(T*, Ctx*, const E*, intptr_t)) {
    Listener l;
    l.fn = (void*)fn;
    l.view = e.id;
    l.hasArg = true;
    return l;
}

template <typename T, typename E>
Listener ListenTo(Entity<T> e, void (*fn)(T*, Ctx*, const E*, intptr_t),
                  intptr_t arg) {
    Listener l;
    l.fn = (void*)fn;
    l.view = e.id;
    l.arg = arg;
    l.hasArg = true;
    l.argBound = true;
    return l;
}

struct Subscription {
    int id = 0;

    bool IsValid() const { return id != 0; }
};

Subscription EntitySubscribeRaw(App* app, EntityId emitter, Listener handler);
void EntityUnsubscribe(App* app, Subscription sub);

Subscription EntityObserveRaw(App* app, EntityId observed, Listener handler);
void EntityUnobserve(App* app, Subscription sub);
int EntityObserverCount(App* app, EntityId observed);

void NotifyEntity(App* app, EntityId id, Window* from);

void EntityEmit(App* app, Window* win, EntityId emitter, const void* ev);

int EntitySubscriberCount(App* app, EntityId emitter);

template <typename T, typename S, typename E>
Subscription Subscribe(Ctx* cx, Entity<T> emitter,
                       void (*fn)(S*, Ctx*, const E*)) {
    Listener l;
    l.fn = (void*)fn;
    l.view = cx->self;
    return EntitySubscribeRaw(cx->app, emitter.id, l);
}

template <typename T, typename S>
Subscription Observe(Ctx* cx, Entity<T> observed,
                     void (*handler)(S*, Ctx*, const EntityId*)) {
    Listener l = Listen(cx, handler);
    return EntityObserveRaw(cx->app, observed.id, l);
}

template <typename T, typename S>
Subscription ObserveTo(App* app, Entity<T> observed, Entity<S> observer,
                       void (*handler)(S*, Ctx*, const EntityId*)) {
    Listener l = ListenTo(observer, handler);
    return EntityObserveRaw(app, observed.id, l);
}

template <typename T, typename S, typename E>
Subscription SubscribeTo(App* app, Entity<T> emitter, Entity<S> subscriber,
                         void (*fn)(S*, Ctx*, const E*)) {
    Listener l;
    l.fn = (void*)fn;
    l.view = subscriber.id;
    return EntitySubscribeRaw(app, emitter.id, l);
}

template <typename E>
void Emit(Ctx* cx, EntityId emitter, const E* ev) {
    EntityEmit(cx->app, cx->win, emitter, ev);
}

void Notify(Ctx* cx);
void NotifyApp(App* app);
void ListenerCall(App* app, Window* win, const Listener& l, const void* ev);

El* EntityRender(App* app, Window* win, Arena* a, EntityId id);

void* WindowKeyedState(Window* win, uint32_t key, int size, DropFn drop);
void WindowKeyedFree(Window* win);

void* WindowMotionState(Window* win, uint32_t key, int size);

void WindowMotionSweep(Window* win);
void WindowMotionFree(Window* win);

template <typename T>
T* KeyedState(Ctx* cx, uint32_t key) {
    void* p = WindowKeyedState(cx->win, key, (int)sizeof(T), &EntityDropT<T>);
    return (T*)p;
}

EntityId WindowKeyedEntity(Window* win, App* app, uint32_t key, void* fresh,
                           DropFn drop);

inline uint32_t KeyedKey(uint32_t name, uint32_t kind) {
    uint32_t h = name * 2654435761u;
    h ^= kind + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h ? h : 1u;
}

inline uint32_t KeyedName(Ctx* cx, Str name) {
    return IdFoldName(cx ? cx->path : 0, name);
}

template <typename T>
Entity<T> KeyedEntity(Ctx* cx, uint32_t key) {
    Entity<T> e;
    e.id = WindowKeyedEntity(cx->win, cx->app, key, new T(), &EntityDropT<T>);
    return e;
}

template <typename T>
T* ElementState(Ctx* cx, Str name, Str kind) {
    return KeyedState<T>(cx, KeyedKey(KeyedName(cx, name), HashClickId(kind)));
}

template <typename T>
Entity<T> ElementStateEntity(Ctx* cx, Str name, Str kind) {
    return KeyedEntity<T>(cx, KeyedKey(KeyedName(cx, name), HashClickId(kind)));
}

void WindowOnKey(Window* win, Listener l);

void WindowOnUnhandledClick(Window* win, Listener l);

void WindowToggleInspector(Window* win);
void WindowInspectorPick(Window* win, bool picking);
const InspectorState* WindowInspector(Ctx* cx);

const DragPayload* WindowActiveDrag(Ctx* cx);

bool WindowIsActive(Ctx* cx);

void WindowSetActive(Window* win, bool active);

int WindowDragOverId(Ctx* cx);

void WindowStopPropagation(Ctx* cx);

Point WindowDragOffset(Ctx* cx);

void WindowOnMouseDown(Window* win, Listener l);
void WindowOnMouseUp(Window* win, Listener l);
void WindowOnMouseMove(Window* win, Listener l);
void WindowOnMouseExit(Window* win, Listener l);
void WindowOnScrollWheel(Window* win, Listener l);

void WindowPost(Window* win, Listener l, const void* ev = nullptr);

int WindowSetInterval(Window* win, int ms, Listener l);

int WindowSetTimeout(Window* win, int ms, Listener l);
void WindowCancelTimer(Window* win, int id);

struct BlinkCursor {
    bool visible = false;
    bool paused = false;

    int timer = 0;

    static void OnFlip(BlinkCursor* self, Ctx* cx, const TickEvent* ev);
    static void OnResume(BlinkCursor* self, Ctx* cx, const TickEvent* ev);
};

struct TooltipOverlay {

    Str text = {};
    Bounds triggerBounds = {};
    bool visible = false;
    bool hadRecent = false;
    int showTimer = 0;
    int hideTimer = 0;

    ~TooltipOverlay();

    static void OnShow(TooltipOverlay* self, Ctx* cx, const TickEvent* ev);
    static void OnHide(TooltipOverlay* self, Ctx* cx, const TickEvent* ev);
};

void TooltipRequestShow(Window* win, Str text, Bounds triggerBounds);
void TooltipRequestHide(Window* win);

const TooltipOverlay* TooltipShowing(Window* win);
void TooltipPaint(PaintCtx* ctx, const TooltipOverlay* tip);

void BlinkStart(App* app, Window* win, EntityId* handle);
void BlinkStop(App* app, Window* win, EntityId* handle);

void BlinkPause(App* app, Window* win, EntityId* handle);

bool BlinkVisible(App* app, EntityId handle);

inline void BlinkStart(Ctx* cx, EntityId* handle) {
    BlinkStart(cx->app, cx->win, handle);
}
inline void BlinkStop(Ctx* cx, EntityId* handle) {
    BlinkStop(cx->app, cx->win, handle);
}
inline void BlinkPause(Ctx* cx, EntityId* handle) {
    BlinkPause(cx->app, cx->win, handle);
}
inline bool BlinkVisible(Ctx* cx, EntityId handle) {
    return BlinkVisible(cx->app, handle);
}

inline void InputFocus(InputState* s, Ctx* cx) {
    InputFocus(s, cx->app, cx->win);
}
inline void InputBlur(InputState* s, Ctx* cx) {
    InputBlur(s, cx->app, cx->win);
}
inline void InputMoveTo(InputState* s, Ctx* cx, int offset) {
    InputMoveTo(s, cx->app, cx->win, offset);
}
inline void InputSelectAll(InputState* s, Ctx* cx) {
    InputSelectAll(s, cx->app, cx->win);
}
inline void InputClean(InputState* s, Ctx* cx) {
    InputClean(s, cx->app, cx->win);
}
inline void InputReplaceAll(InputState* s, Ctx* cx, Str value) {
    InputReplaceAll(s, cx->app, cx->win, value);
}
inline void InputInsert(InputState* s, Ctx* cx, Str value) {
    InputInsert(s, cx->app, cx->win, value);
}
inline bool InputPerform(InputState* s, Ctx* cx, InputAction action,
                         bool shift = false) {
    return InputPerform(s, cx->app, cx->win, action, shift);
}

Window* WindowOpenView(App* app, Str title, int dipW, int dipH, EntityId root,
                       WinOpts opts);
int AppRunView(Str title, int dipW, int dipH, EntityId root, App* app,
               WinOpts opts);

template <typename T>
T* WindowRoot(Window* win) {
    return win ? (T*)EntityGet(win->app, win->root) : nullptr;
}

WinSize WindowSize(Window* win);

int WindowCollectFrames(Window* win, uint64_t* cursor, FrameTiming* out,
                        int max);

double TimeNow();

App* AppNew();
void AppFree(App* app);

void ClipboardSetText(Window* win, Str text);

Str ClipboardGetText(Arena* a, Window* win);

void OpenUrl(Str url);

struct PathPrompt {

    bool files = true;
    bool directories = false;

    Str title = {};
};

bool PromptForPath(Window* win, const PathPrompt& opts, char* out, int cap);

int AppRun(App* app);
Window* WindowOpen(App* app, Str title, int dipW, int dipH, WinOpts opts);
void AppSetTitle(Window* win, Str title);
void AppRequestAnim(Window* win, bool on);

void WindowRequestAnimationFrame(Window* win);

void FocusCollect(Window* win, El* root);
void IdsCollect(El* root);
int FocusNext(Window* win, int trapId, bool backward);

FocusHandle FocusHandleNew(App* app);
FocusHandle FocusHandleNew(Ctx* cx);

bool FocusHandleIsFocused(const Window* win, FocusHandle h);

bool FocusHandleContainsFocused(const Window* win, FocusHandle h);
void FocusHandleFocus(Window* win, FocusHandle h);
FocusHandle WindowFocused(const Window* win);

bool FocusHandleRestore(Window* win, FocusHandle h);

void WindowSetFocusId(Window* win, int id);

int WindowFocusedId(const Window* win);

bool WindowFocusWithin(const Window* win, int id);

bool WindowRestoreFocus(Window* win, int id);

bool WindowDispatchKeyAction(Window* win, int vk, bool shift, bool ctrl,
                             bool alt, bool platform = false);

uint32_t WindowResolveKeyAction(Window* win, int vk, bool shift, bool ctrl,
                                bool alt, bool platform, intptr_t* arg,
                                bool* pending);

constexpr bool KeySecondary(bool ctrl, bool platform) {
#if GPUI_OS_MAC
    (void)ctrl;
    return platform;
#else
    (void)platform;
    return ctrl;
#endif
}

bool WindowDispatchAction(Window* win, uint32_t action, intptr_t arg = 0);

bool WindowDispatchKeyEvent(Window* win, KeyEvent* ev);

using ActionFn = void (*)(Window* win, ActionEvent* ev);
void AppOnAction(uint32_t action, ActionFn fn);
void AppQuit(Window* win);

void AppQuitAll(App* app);
void AppInvalidate(Window* win);

void AppRefreshWindows(App* app);

void AppOnShutdown(void (*fn)());

bool WindowClientDecorated(Window* win);

struct MenuRow {
    Str label = {};

    uint32_t action = 0;
    intptr_t arg = 0;
    bool separator = false;
    bool disabled = false;
    bool checked = false;
    const MenuRow* submenu = nullptr;
    int submenuN = 0;
};

struct MenuDef {
    Str name = {};
    const MenuRow* items = nullptr;
    int n = 0;
};

bool AppHasMenuBar();

void AppSetMenus(App* app, const MenuDef* menus, int n);

bool AppMenuRowForId(int id, uint32_t* action, intptr_t* arg);

void AppActivate(Window* win);
void AppMinimize(Window* win);
void AppToggleMaximize(Window* win);
void AppClose(Window* win);
void AppDrag(Window* win);
bool AppIsMaximized(Window* win);
}

int GpuiMain(int argc, char** argv);

#line 1 "src/gpui/assets.h"

namespace gpui {

void AssetsClear();

int AssetsRootCount();
void AssetsAddRoot(Str dir);

void AssetsAddDefaultRoots(Str exampleName);
bool AssetsLoad(Str relPath, Vec<uint8_t>* out);
TempStr AssetsLoadTextTemp(Str relPath);

bool AssetsFindDir(Str relDir, char* out, int cap);
bool AssetsExists(Str relPath);
}

#line 1 "src/gpui/drawops.h"

namespace gpui {

enum DrawOp : uint16_t {
    kOpEnd = 0,

    kOpViewBox = 1,

    kOpStrokeWidth = 2,

    kOpColor = 3,
    kOpColorReset = 4,

    kOpLine = 5,
    kOpRect = 6,
    kOpFillRect = 7,
    kOpEllipse = 8,
    kOpFillEllipse = 9,

    kOpArc = 10,

    kOpMoveTo = 11,
    kOpLineTo = 12,
    kOpCubicTo = 13,
    kOpClosePath = 14,
    kOpFillPath = 15,
    kOpStrokePath = 16,

    kOpFillStrokePath = 17,

    kOpText = 18,
};

enum DrawOpTextFlag : uint32_t {
    kTextAnchorMask = 3,
    kTextAnchorStart = 0,
    kTextAnchorMiddle = 1,
    kTextAnchorEnd = 2,
    kTextBold = 4,
};

struct DrawOpsTarget {
    float x = 0;
    float y = 0;
    float w = 16;
    float h = 16;

    Rgba color = {};
    float turns = 0;
};

bool ExecuteDrawOps(PaintCtx* ctx, const void* data, int dataLen,
                    const DrawOpsTarget& t);

bool DrawOpsViewBox(const void* data, int dataLen, Size* out);

struct DrawOpsBuilder {
    Vec<uint8_t> data;

    void Op(DrawOp op);

    void F2(float a, float b);
    void U32(uint32_t v);

    void ViewBox(float x, float y, float w, float h);
    void StrokeWidth(float w);
    void Color(Rgba c);
    void ColorReset();
    void Line(float x1, float y1, float x2, float y2);
    void MoveTo(float x, float y);
    void LineTo(float x, float y);
    void CubicTo(float x1, float y1, float x2, float y2, float x, float y);
    void ClosePath();

    void Text(float x, float y, float size, float textLength, uint32_t flags,
              Str s);
    void End();
};

}

#line 1 "src/gpui/svg.h"

namespace gpui {

bool SvgViewBox(Str assetPath, Size* out);

bool SvgDraw(PaintCtx* ctx, Str assetPath, float x, float y, float size,
             Rgba color, float turns = 0);

bool SvgDrawOps(PaintCtx* ctx, const uint8_t* ops, int len, float x, float y,
                float w, float h, Rgba color, float turns = 0);

bool SvgRasterize(PaintApp* pa, Str assetPath, int px, Rgba color,
                  uint8_t* outBgra);

bool SvgToDrawOps(Str xml, DrawOpsBuilder* out);

const uint8_t* SvgDrawOpsFor(Str assetPath, int* lenOut);

Str IconNamePath(IconName name);
}

#line 1 "src/base/element_ext.h"

namespace gpui {

inline El* UiRoot(Arena* a, Str id, int clickId = 0) {
    El* e = Div(a)->Id(id);
    if (clickId) {
        e->Click(clickId)->FocusId(clickId);
    }
    return e;
}
}

#line 1 "src/base/accordion.h"

namespace gpui {

struct Accordion {
    static El* New(Ctx* cx, Str id);
};

struct AccordionTrigger {
    static El* New(Ctx* cx, Str id, bool open = false, bool disabled = false,
                   Listener onChange = {});
};

struct AccordionHeader {
    static El* New(Ctx* cx, El* trigger);
};

struct AccordionPanel {
    static El* New(Ctx* cx);
};

struct AccordionItem {
    El* root = nullptr;
    bool open = false;
    bool keepMounted = false;

    static AccordionItem* New(Ctx* cx);
    AccordionItem* Open(bool v);
    AccordionItem* KeepMounted(bool v);
    AccordionItem* Header(El* header);
    AccordionItem* Panel(El* panel);
    El* IntoEl();
};
}

#line 1 "src/base/dialog.h"

namespace gpui {

enum class DialogChangeReason : uint8_t {
    TriggerPress,
    BackdropPress,
    Cancel,
    Confirm,
    Imperative
};

enum class DialogAction : uint8_t {
    None,
    Cancel,
    Confirm
};

void DialogInitKeys();
Str DialogContext();
DialogAction DialogActionOf(uint32_t id);

struct DialogKeys {

    Listener onCancel = {};
    Listener onOk = {};

    Listener onClose = {};

    static void OnAction(DialogKeys* self, Ctx* cx, const ActionEvent* ev);
};

void DialogBindKeys(Ctx* cx, El* popup, Str name, Listener onCancel,
                    Listener onOk, Listener onClose);

bool DialogBackdropCloses(bool overlayClosable, bool topmost,
                          MouseButton button, float pressY,
                          float dismissBelowY);

struct DialogBackdrop {
    static El* New(Ctx* cx);
};
struct DialogPopup {
    static El* New(Ctx* cx);
};
struct DialogTitle {
    static El* New(Ctx* cx);
};
struct DialogDescription {
    static El* New(Ctx* cx);
};
struct DialogClose {
    static El* New(Ctx* cx, int clickId = 0);
};

struct DialogTrigger {
    static El* New(Ctx* cx, Listener onOpen = {});
};

struct Dialog {
    Ctx* cx = nullptr;
    El* root = nullptr;

    Str trap = {};

    static Dialog* New(Ctx* cx);
    Dialog* Trap(Str name);
    Dialog* Backdrop(El* backdrop);
    Dialog* Popup(El* popup);
    El* IntoEl();
};
}

#line 1 "src/base/alert_dialog.h"

namespace gpui {

const bool kAlertDialogClosesOnBackdropPress = false;

struct AlertDialogBackdrop {
    static El* New(Ctx* cx);
};
struct AlertDialogPopup {
    static El* New(Ctx* cx);
};
struct AlertDialogTitle {
    static El* New(Ctx* cx);
};
struct AlertDialogDescription {
    static El* New(Ctx* cx);
};

struct AlertDialogCancel {
    static El* New(Ctx* cx);
};
struct AlertDialogAction {
    static El* New(Ctx* cx);
};
struct AlertDialogTrigger {
    static El* New(Ctx* cx, Listener onOpen = {});
};

struct AlertDialog {
    Ctx* cx = nullptr;
    El* root = nullptr;

    Str trap = {};

    static AlertDialog* New(Ctx* cx);
    AlertDialog* Trap(Str name);
    AlertDialog* Backdrop(El* backdrop);
    AlertDialog* Popup(El* popup);
    El* IntoEl();
};
}

#line 1 "src/base/avatar.h"

namespace gpui {

struct Avatar {
    El* root = nullptr;
    El* image = nullptr;
    El* fallback = nullptr;

    static Avatar* New(Ctx* cx);
    Avatar* Size(float px);
    Avatar* Image(El* image);
    Avatar* Fallback(El* fallback);
    El* IntoEl();
};

struct AvatarImage {
    static El* New(Ctx* cx);
};
struct AvatarFallback {
    static El* New(Ctx* cx);
};
}

#line 1 "src/base/state_style.h"

namespace gpui {

enum StateField : uint32_t {
    StateFieldBg = StyleFieldBg,
    StateFieldFg = StyleFieldColor,

    StateFieldBorder = StyleFieldBorder | StyleFieldBorderColor,

    StateFieldBorderL = StyleFieldBorderL | StyleFieldBorderColor,
    StateFieldBorderR = StyleFieldBorderR | StyleFieldBorderColor,
    StateFieldBorderT = StyleFieldBorderT | StyleFieldBorderColor,
    StateFieldBorderB = StyleFieldBorderB | StyleFieldBorderColor,
    StateFieldRadius = StyleFieldRadius,
    StateFieldHoverBg = StyleFieldHoverBg,
    StateFieldHoverFg = StyleFieldHoverFg,
    StateFieldActiveBg = StyleFieldActiveBg,

    StateFieldOpacity = StyleFieldOpacity,
};

struct StateStyle {
    Style style = {};
    uint32_t set = 0;

    StateStyle& Bg(Background c);
    StateStyle& Fg(Rgba c);
    StateStyle& Border(float w, Rgba c);
    StateStyle& BorderL(float w, Rgba c);
    StateStyle& BorderR(float w, Rgba c);
    StateStyle& BorderT(float w, Rgba c);
    StateStyle& BorderB(float w, Rgba c);
    StateStyle& Radius(float v);
    StateStyle& HoverBg(Background c);
    StateStyle& HoverFg(Rgba c);
    StateStyle& ActiveBg(Background c);
    StateStyle& Opacity(float v);

    bool Has(StateField f) const { return (set & (uint32_t)f) != 0; }
};

void StateStyleRefine(StateStyle* into, const StateStyle& over);

StateStyle StateStyleResolve(const StateStyle& instance,
                             const StateStyle* const* states, int n);

El* ElRefine(El* e, const StateStyle& s);

}

#line 1 "src/base/button.h"

namespace gpui {

struct ButtonStyles {
    StateStyle selected = {};
    StateStyle disabled = {};
};

struct Button {
    static El* New(Ctx* cx, Str id, bool disabled = false,
                   Listener onClick = {}, bool focusable = true,
                   const ButtonStyles* styles = nullptr, bool selected = false);
};
}

#line 1 "src/base/date_picker.h"

namespace gpui {

enum class DatePickerAction : uint8_t {
    None,
    Open,
    Dismiss,

    Clear
};

void DatePickerInitKeys();
Str DatePickerContext();

DatePickerAction DatePickerActionOf(uint32_t id, bool open, bool disabled);

struct DatePickerKeys {

    Listener onToggle = {};

    Listener onClear = {};
    bool open = false;
    bool disabled = false;

    static void OnAction(DatePickerKeys* self, Ctx* cx, const ActionEvent* ev);
};

void DatePickerBindKeys(Ctx* cx, El* root, Str name, Listener onToggle,
                        Listener onClear, bool open, bool disabled);

enum class DateMatcherKind : uint8_t {
    None,
    Weekdays,
    Interval,
    Range,
    Custom
};

struct DateMatcher {
    DateMatcherKind kind = DateMatcherKind::None;
    uint8_t weekdayMask = 0;
    LocalDate from = {};
    LocalDate to = {};
    bool (*custom)(LocalDate date) = nullptr;
};

DateMatcher DateMatcherWeekdays(uint8_t weekdayMask);
DateMatcher DateMatcherInterval(LocalDate before, LocalDate after);
DateMatcher DateMatcherRange(LocalDate from, LocalDate to);
DateMatcher DateMatcherCustom(bool (*fn)(LocalDate date));
bool DateMatcherMatches(const DateMatcher& matcher, LocalDate date);
intptr_t DatePickerDateKey(LocalDate date);
LocalDate DatePickerDateFromKey(intptr_t key);

enum class DateSelectionResult : uint8_t {
    Rejected,
    Partial,
    Complete
};

DateSelectionResult DatePickerSelectDate(bool range, LocalDate value,
                                         LocalDate* start, LocalDate* end,
                                         const DateMatcher& disabled);

struct DatePicker {
    static El* New(Ctx* cx, Str id, bool disabled = false);
};
}

#line 1 "src/base/calendar.h"

namespace gpui {

enum class CalendarView : uint8_t {
    Day,
    Month,
    Year
};

struct CalendarState {
    int currentYear = 0;

    int currentMonth = 1;
    int numberOfMonths = 1;

    int yearPage = 0;
    int yearPageCount = 0;
    CalendarView view = CalendarView::Day;
};

void CalendarPrevMonth(CalendarState* s);
void CalendarNextMonth(CalendarState* s);

bool CalendarHasPrevYearPage(const CalendarState* s);
bool CalendarHasNextYearPage(const CalendarState* s);
bool CalendarPrevYearPage(CalendarState* s);
bool CalendarNextYearPage(CalendarState* s);

int CalendarGridOffset(int firstWeekday);

int CalendarGridCells(int offset, int daysInMonth);

enum class CalendarItemKind : uint8_t {
    Previous,
    MonthToggle,
    YearToggle,
    Next,
    Weekday,
    Day,
    Month,
    Year
};

struct CalendarItemState {
    CalendarItemKind kind = CalendarItemKind::Day;

    int value = 0;

    LocalDate date = {};
    bool active = false;
    bool inRange = false;
    bool muted = false;
    bool disabled = false;
    bool today = false;
};

using CalendarItemFn = El* (*)(void* user, Ctx* cx, El* item,
                               const CalendarItemState& st);

struct CalendarOpts {
    int year = 0;
    int month = 1;
    int numberOfMonths = 1;
    CalendarView view = CalendarView::Day;

    float cellSize = 32;

    LocalDate selected = {};
    LocalDate rangeEnd = {};

    LocalDate today = {};
    DateMatcher disabledMatcher = {};

    int yearMin = 0;
    int yearMax = 0;
    int yearPageStart = 0;
    Listener onDay = {};
    Listener onDate = {};
    Listener onPrev = {};
    Listener onNext = {};
    Listener onMonthToggle = {};
    Listener onYearToggle = {};
    Listener onMonth = {};
    Listener onYear = {};
    CalendarItemFn item = nullptr;
    void* user = nullptr;
};

struct Calendar {

    static El* New(Ctx* cx, Str id);

    static El* New(Ctx* cx, Str id, const CalendarOpts& o);
};

void CalendarOffsetMonth(int year, int month, int offset, int* outYear,
                         int* outMonth);

int CalendarWeekday(int year, int month, int day);

int CalendarDaysInMonth(int year, int month);

struct CalendarItem {
    static El* New(Ctx* cx, Str id = {}, Listener onClick = {});
};
}

#line 1 "src/base/checkbox.h"

namespace gpui {

enum class CheckboxState : uint8_t {
    Unchecked,
    Checked,
    Indeterminate
};

CheckboxState CheckboxActivated(CheckboxState state);

struct Checkbox {
    static El* New(Ctx* cx, Str id,
                   CheckboxState state = CheckboxState::Unchecked,
                   bool disabled = false, Listener onChange = {});
};

struct CheckboxIndicator {
    static El* New(Ctx* cx);
};
}

#line 1 "src/base/collapsible.h"

namespace gpui {

struct Collapsible {
    El* root = nullptr;
    bool open = false;

    static Collapsible* New(Ctx* cx);

    Collapsible* FlexCol();
    Collapsible* Open(bool v);
    Collapsible* Child(El* e);
    Collapsible* Content(El* e);
    El* IntoEl();
};
}

#line 1 "src/base/color_picker.h"

namespace gpui {

struct ColorPickerState {
    uint32_t value = 0;
    bool hasValue = false;
    uint32_t preview = 0;
    bool hasPreview = false;
    bool open = false;
    int activeTab = 0;

    SliderState sliders[4] = {};
    InputState hexInput;

    bool needsSliderSync = true;

    Listener onChange = {};

    ColorPickerState();

    static void OnToggleOpen(ColorPickerState* s, Ctx* cx, const ClickEvent*);
    static void OnTab(ColorPickerState* s, Ctx* cx, const ClickEvent*,
                      intptr_t ix);
    static void OnSwatchClick(ColorPickerState* s, Ctx* cx, const ClickEvent*,
                              intptr_t hex);
    static void OnSwatchHover(ColorPickerState* s, Ctx* cx,
                              const HoverEvent* ev, intptr_t hex);
    static void OnSlider(ColorPickerState* s, Ctx* cx, const SliderEvent*);
    static void OnHexChange(ColorPickerState* s, Ctx* cx, const InputEvent* ev);
    static void OnHexFocus(ColorPickerState* s, Ctx* cx, const ClickEvent*);
};

bool ColorPickerShown(const ColorPickerState* s, uint32_t* out);

void ColorPickerPreview(ColorPickerState* s, uint32_t color);
bool ColorPickerClearPreview(ColorPickerState* s);

void ColorPickerSetValue(ColorPickerState* s, uint32_t color);
void ColorPickerClearValue(ColorPickerState* s);

void ColorPickerSelect(ColorPickerState* s, uint32_t color);

void ColorPickerUpdateColor(ColorPickerState* s, uint32_t color);

void ColorPickerSetOpen(ColorPickerState* s, bool open);
void ColorPickerSetActiveTab(ColorPickerState* s, int tab);

void ColorPickerSyncPending(ColorPickerState* s);

uint32_t ColorPickerSliderColor(const ColorPickerState* s);

bool ColorPickerParseHex(Str text, uint32_t* out);

Str ColorPickerHexString(Arena* a, uint32_t color);

struct ColorPicker {
    static El* New(Ctx* cx, Str id);
};

struct ColorSwatch {
    static El* New(Ctx* cx, Str id, Listener onClick = {},
                   Listener onHover = {});
};
}

#line 1 "src/base/select.h"

namespace gpui {

enum class SelectAction : uint8_t {

    None,

    Open,

    Confirm,

    Dismiss
};

void SelectInitKeys();
Str SelectContext();

SelectAction SelectActionOf(uint32_t id, bool open, bool disabled);

struct Select {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/base/combobox.h"

namespace gpui {

struct Combobox {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/base/dock.h"

namespace gpui {

const float kDockPanelMinSize = 100.f;

const float kDockHandleW = 4;

const float kDockCollapsedH = 29.f;

const float kDockDragPreviewW = 96.f;
const float kDockDragPreviewH = 30.f;

extern const Str kDockPanelDrag;
extern const Str kDockResizeDrag;

enum class DockPlacement : uint8_t {
    Center,
    Left,
    Bottom,
    Right
};

enum class DockDrop : uint8_t {
    Center,
    Left,
    Right,
    Top,
    Bottom
};

DockDrop DockDropAt(Bounds b, float x, float y);

Bounds DockDropPlaceholder(Bounds b, DockDrop d);

const int kDockSideBase = 1 << 20;

enum class DockPanelControl : uint8_t {
    No,
    Menu,
    Toolbar,
    Both
};

inline bool DockPanelControlToolbar(DockPanelControl c) {
    return c == DockPanelControl::Toolbar || c == DockPanelControl::Both;
}
inline bool DockPanelControlMenu(DockPanelControl c) {
    return c == DockPanelControl::Menu || c == DockPanelControl::Both;
}

enum class DockPanelStyle : uint8_t {
    Auto,
    TabBar
};

struct DockPanelDef {

    Str name = {};
    Str title = {};
    El* (*render)(Ctx* cx, void* data) = nullptr;

    El* (*titleSuffix)(Ctx* cx, void* data) = nullptr;

    Str tabName = {};
    void* data = nullptr;
    bool closable = true;

    bool visible = true;
    DockPanelControl zoomable = DockPanelControl::Menu;
};

struct DockNode {
    DockNode() = default;

    bool used = false;
    bool split = false;
    Axis axis = Axis::Horizontal;
    int parent = -1;

    Vec<int> child;
    Vec<float> size;
    Vec<int> panel;
    int activeIx = 0;

    Bounds bounds = {};

    float tabScrollX = 0;
    int pendingScrollIx = -1;
    Bounds tabStripBounds = {};
    Bounds activeTabBounds = {};

    int activeTabBoundsIx = -1;
};

struct DockSide {
    int node = -1;
    bool open = true;
    bool collapsible = true;
    float size = 200;
};

enum class DockEventKind : uint8_t {

    LayoutChanged
};

struct DockEvent {
    DockEventKind kind = DockEventKind::LayoutChanged;
};

struct DockState {
    Vec<DockPanelDef> panels;

    Vec<DockNode> nodes;
    int center = -1;
    DockSide left = {};
    DockSide bottom = {};
    DockSide right = {};

    int zoomPanel = -1;

    bool locked = false;

    DockPanelStyle panelStyle = DockPanelStyle::Auto;

    bool toggleButtonVisible = true;

    bool hasVersion = false;
    int version = 0;

    Bounds bounds = {};

    int dropNode = -1;
    DockDrop dropAt = DockDrop::Center;

    Bounds dropFrom = {};
    bool dropFromPending = false;

    int menuNode = -1;

    DockPlacement resizingSide = DockPlacement::Center;
    bool resizing = false;

    Listener onEvent;

    static void OnTabClick(DockState* self, Ctx* cx, const ClickEvent* ev,
                           intptr_t nodeAndIx);
    static void OnCloseClick(DockState* self, Ctx* cx, const ClickEvent* ev,
                             intptr_t nodeAndIx);
    static void OnZoomClick(DockState* self, Ctx* cx, const ClickEvent* ev,
                            intptr_t panelIx);
    static void OnToggleSide(DockState* self, Ctx* cx, const ClickEvent* ev,
                             intptr_t placement);
    static void OnTabDragMove(DockState* self, Ctx* cx,
                              const DragMoveEvent* ev);
    static void OnTabDragEnd(DockState* self, Ctx* cx, const MouseUpEvent* ev);
    static void OnDropPanel(DockState* self, Ctx* cx, const DropEvent* ev,
                            intptr_t node);

    static void OnDropTab(DockState* self, Ctx* cx, const DropEvent* ev,
                          intptr_t nodeAndIx);
    static void OnDropTabBar(DockState* self, Ctx* cx, const DropEvent* ev,
                             intptr_t node);

    static void OnMenuItem(DockState* self, Ctx* cx, const ClickEvent* ev,
                           intptr_t nodeAndIx);

    static void OnTabBarScroll(DockState* self, Ctx* cx, const ScrollEvent* ev,
                               intptr_t node);
    static void OnResizeDrag(DockState* self, Ctx* cx, const DragMoveEvent* ev);
    static void OnResizeEnd(DockState* self, Ctx* cx, const MouseUpEvent* ev);

    ~DockState() {
        for (int i = 0; i < nodes.len; i++) {
            nodes[i].child.Reset();
            nodes[i].size.Reset();
            nodes[i].panel.Reset();
        }
        nodes.Reset();
        panels.Reset();
    }
};

inline intptr_t DockPack(int node, int ix) {
    return (intptr_t)(node * 64 + ix);
}
inline int DockUnpackNode(intptr_t v) {
    return (int)(v / 64);
}
inline int DockUnpackIx(intptr_t v) {
    return (int)(v % 64);
}

void DockNormalize(DockState* s);

int DockAddPanelDef(DockState* s, DockPanelDef def);

int DockNewTabs(DockState* s);
int DockNewSplit(DockState* s, Axis axis);

void DockTabsAdd(DockState* s, int node, int panelIx);

void DockTabsInsert(DockState* s, int node, int panelIx, int at);
void DockSplitAdd(DockState* s, int node, int childNode, float size);

void DockSetActive(DockState* s, Ctx* cx, int node, int ix);

void DockClosePanel(DockState* s, Ctx* cx, int node, int ix);

void DockMovePanel(DockState* s, Ctx* cx, int panelIx, int to, DockDrop drop,
                   int atIx = -1);

bool DockClosePanelAt(DockState* s, int node, int ix);
bool DockMovePanelTo(DockState* s, int panelIx, int to, DockDrop drop,
                     int atIx = -1);

float DockTabScrollTo(float scrollX, Bounds strip, Bounds tab);

void DockToggleSide(DockState* s, Ctx* cx, DockPlacement p);
void DockResizeSide(DockState* s, Ctx* cx, DockPlacement p, float x, float y);

void DockToggleZoom(DockState* s, Ctx* cx, int panelIx);

DockSide* DockSideOf(DockState* s, DockPlacement p);

int DockNodeOfPanel(const DockState* s, int panelIx);

int DockPanelByName(const DockState* s, Str name);

DockPlacement DockPlacementOfNode(const DockState* s, int node);

int DockVisibleCount(const DockState* s, int node);

bool DockNodeVisible(const DockState* s, int node);
int DockActiveIx(const DockState* s, int node);

bool DockNodeLocked(const DockState* s, int node);

bool DockIsLastPanel(const DockState* s, int node);

bool DockNodeDraggable(const DockState* s, int node);
bool DockNodeDroppable(const DockState* s, int node);

int DockLeftTopTabs(const DockState* s, int node);
int DockRightTopTabs(const DockState* s, int node);

void DockSetCollapsible(DockState* s, DockPlacement p, bool collapsible);

struct DockCtx {
    Ctx* cx = nullptr;
    Entity<DockState> state = {};
    Str id = {};
    DockPlacement placement = DockPlacement::Left;
    float size = 0;
    bool open = true;
};

struct DockHandleCtx {
    Axis axis = Axis::Horizontal;
    bool active = false;
};

struct DockTabGroup {
    Ctx* cx = nullptr;
    Entity<DockState> state = {};
    Str id = {};
    int node = -1;

    bool collapsed = false;
};

int DockGroupCount(const DockTabGroup* g);
const DockPanelDef* DockGroupPanel(const DockTabGroup* g, int ix);
int DockGroupActiveIx(const DockTabGroup* g);
DockPlacement DockGroupPlacement(const DockTabGroup* g);

bool DockGroupHasToggle(const DockTabGroup* g, DockPlacement p);

El* DockBindTab(const DockTabGroup* g, int ix, El* tab);

El* DockBindTabRest(const DockTabGroup* g, El* rest);

El* DockBindTabStrip(const DockTabGroup* g, El* strip);

bool DockGroupDroppable(const DockTabGroup* g);

El* DockBindTitleDrag(const DockTabGroup* g, int ix, El* e);
El* DockBindToggle(const DockTabGroup* g, DockPlacement p, El* e);
El* DockBindZoom(const DockTabGroup* g, int panelIx, El* e);
El* DockBindClose(const DockTabGroup* g, int ix, El* e);

El* DockBindResizeStrip(const DockCtx* d, El* e);

void DockGroupOpenMenu(const DockTabGroup* g, bool open);

struct DockRenderer {
    void* data = nullptr;

    El* (*frame)(Ctx* cx, void* data) = nullptr;

    El* (*centerFrame)(Ctx* cx, void* data) = nullptr;
    El* (*splitFrame)(Ctx* cx, void* data, int node, Axis axis) = nullptr;

    El* (*splitHandle)(Ctx* cx, void* data, const DockHandleCtx* h) = nullptr;

    El* (*dock)(Ctx* cx, void* data, const DockCtx* d, El* content) = nullptr;

    El* (*tabGroupFrame)(Ctx* cx, void* data, const DockTabGroup* g) = nullptr;
    El* (*tabContentFrame)(Ctx* cx, void* data,
                           const DockTabGroup* g) = nullptr;
    El* (*tabBar)(Ctx* cx, void* data, const DockTabGroup* g) = nullptr;

    El* (*dropIndicator)(Ctx* cx, void* data, Bounds to) = nullptr;

    El* (*dragPreview)(Ctx* cx, void* data, const DockPanelDef* def) = nullptr;
};

struct DockArea {
    static El* New(Ctx* cx, Str id, Entity<DockState> state,
                   const DockRenderer* r);
};

}

#line 1 "src/base/json.h"

namespace gpui {

enum class JsonKind : uint8_t {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

struct JsonValue;

struct JsonValue {
    JsonKind kind = JsonKind::Null;
    bool b = false;
    double num = 0;
    Str str = {};
    Str key = {};

    JsonValue* first = nullptr;
    JsonValue* next = nullptr;
};

JsonValue* JsonParse(Arena* a, Str text);

const JsonValue* JsonGet(const JsonValue* v, const char* key);

const JsonValue* JsonAt(const JsonValue* v, int index);

int JsonLen(const JsonValue* v);

double JsonNumber(const JsonValue* v, double fallback = 0);
bool JsonBool(const JsonValue* v, bool fallback = false);
Str JsonString(const JsonValue* v, Str fallback = {});

struct JsonWriter {
    StrBuilder* out = nullptr;

    bool wrote[32] = {};
    int depth = 0;

    void BeginObject(const char* key = nullptr);
    void EndObject();
    void BeginArray(const char* key = nullptr);
    void EndArray();
    void Number(const char* key, double v);
    void Bool(const char* key, bool v);
    void String(const char* key, Str v);
    void Null(const char* key);
};

}

#line 1 "src/base/tiles.h"

namespace gpui {

const float kTileMinW = 100.f;
const float kTileMinH = 100.f;

const float kTileDragBarH = 30.f;

const float kTileHandleSize = 5.f;

const float kTileGridSize = 8.f;

const float kTileKeepVisible = 64.f;

extern const Str kTileMoveDrag;
extern const Str kTileResizeDrag;

enum class TileSide : uint8_t {
    None,
    Left,
    Right,
    Top,
    Bottom,
    BottomRight
};

const int kMaxTileChanges = 64;

struct TileItem {

    int panel = 0;
    Bounds bounds = {};
    int zIndex = 0;
};

struct TileChange {
    int tile = 0;
    bool hasBounds = false;
    Bounds oldBounds = {};
    Bounds newBounds = {};
    bool hasOrder = false;
    int oldOrder = 0;
    int newOrder = 0;
};

struct TilesState {

    Vec<TileItem> items;

    int dragging = -1;

    int pressed = -1;
    Point dragInitialMouse = {};
    Bounds dragInitialBounds = {};

    int resizing = -1;
    TileSide side = TileSide::None;
    Point resizeInitialMouse = {};
    Bounds resizeInitialBounds = {};

    Bounds bounds = {};

    float scrollX = 0;
    float scrollY = 0;

    ScrollbarMode scrollbarMode = ScrollbarMode::Always;

    TileChange changes[kMaxTileChanges] = {};
    int nChange = 0;
    int cursor = 0;
    bool ignoring = false;

    static void OnMoveDown(TilesState* self, Ctx* cx, const MouseDownEvent* ev,
                           intptr_t ix);
    static void OnResizeDown(TilesState* self, Ctx* cx,
                             const MouseDownEvent* ev, intptr_t packed);
    static void OnMoveDrag(TilesState* self, Ctx* cx, const DragMoveEvent* ev);
    static void OnResizeDrag(TilesState* self, Ctx* cx,
                             const DragMoveEvent* ev);
    static void OnDragEnd(TilesState* self, Ctx* cx, const MouseUpEvent* ev);
    static void OnScroll(TilesState* self, Ctx* cx, const ScrollEvent* ev);

    static void OnTileDown(TilesState* self, Ctx* cx, const MouseDownEvent* ev,
                           intptr_t ix);
    static void OnTileUp(TilesState* self, Ctx* cx, const MouseUpEvent* ev,
                         intptr_t ix);

    ~TilesState() { items.Reset(); }
};

inline int TileResizePack(int ix, TileSide side) {
    return ix * 8 + (int)side;
}
inline int TileResizeTile(int packed) {
    return packed / 8;
}
inline TileSide TileResizeSide(int packed) {
    return (TileSide)(packed % 8);
}

Size TilesContentSize(const TilesState* s);

void TilesPaintOrder(const TilesState* s, int* out);

int TilesAdd(TilesState* s, int panel, Bounds bounds);
void TilesRemove(TilesState* s, int ix);

int TilesIndexOfPanel(const TilesState* s, int panel);

bool TileSnapEdge(float edge, const float* candidates, int n, float threshold,
                  float* out);

float TileRoundToGrid(float v, float grid);

Bounds TileComputeResizedBounds(Bounds prev, const float* newX,
                                const float* newY, const float* newW,
                                const float* newH, const Bounds* others,
                                int nOthers, float grid);

void TilesMagneticSnap(const TilesState* s, Bounds dragging, int itemIx,
                       float threshold, bool* hasX, float* snapX, bool* hasY,
                       float* snapY);

Point TilesConstrainOrigin(const TilesState* s, Point origin);

void TilesBeginMove(TilesState* s, int ix, float x, float y);
void TilesBeginResize(TilesState* s, int ix, TileSide side, float x, float y);

void TilesUpdatePosition(TilesState* s, float x, float y);

void TilesUpdateResize(TilesState* s, float x, float y);

void TilesMouseUp(TilesState* s);

int TilesBringToFront(TilesState* s, int ix);

bool TilesCanUndo(const TilesState* s);
bool TilesCanRedo(const TilesState* s);
void TilesUndo(TilesState* s);
void TilesRedo(TilesState* s);

}

#line 1 "src/base/dock_state.h"

namespace gpui {

enum class PanelInfoKind : uint8_t {
    Panel,
    Stack,
    Tabs,
    Tiles
};

struct TileMeta {
    Bounds bounds = {10, 10, 200, 200};
    int zIndex = 0;
};

struct PanelStateNode {
    Str panelName = {};

    Vec<int> children;
    PanelInfoKind kind = PanelInfoKind::Panel;

    Vec<float> sizes;
    Axis axis = Axis::Horizontal;

    int activeIndex = 0;

    Vec<TileMeta> metas;

    Str info = {};
};

struct DockSideState {
    bool present = false;
    int node = -1;
    DockPlacement placement = DockPlacement::Left;
    float size = 0;
    bool open = true;
};

struct DockAreaState {

    bool hasVersion = false;
    int version = 0;
    Vec<PanelStateNode> nodes;
    int center = -1;
    DockSideState left = {};
    DockSideState right = {};
    DockSideState bottom = {};

    int NewNode(Str panelName);

    void Clear();

    ~DockAreaState() {
        for (int i = 0; i < nodes.len; i++) {
            nodes[i].children.Reset();
            nodes[i].sizes.Reset();
            nodes[i].metas.Reset();
        }
        nodes.Reset();
    }
};

bool DockAreaStateParse(Arena* a, Str json, DockAreaState* out);

void DockAreaStateWrite(const DockAreaState* s, StrBuilder* out);

void DockDump(const DockState* s, DockAreaState* out);

struct DockInvalidPanel {
    Str name = {};
};

bool DockLoad(DockState* s, const DockAreaState* st, Arena* a,
              El* (*invalidRender)(Ctx* cx, void* data) = nullptr);

int TilesToMetas(const TilesState* s, TileMeta* out, int* outPanels, int cap);

void TilesFromMetas(TilesState* s, const TileMeta* metas, const int* panels,
                    int n);

}

#line 1 "src/base/focus_trap.h"

namespace gpui {

int FocusTrapId(Str name);

int FocusTrapActive(const Window* win);

int FocusTrapOf(const Window* win, int focusId);

int FocusTrapTab(Window* win, bool backward);

bool FocusTrapEnter(Window* win, int trapId, bool backward = false);

void FocusTrapArm(Window* win, int trapId, int hostFocusId = 0);

void FocusTrapApplyPending(Window* win);

}

#line 1 "src/base/geometry.h"

namespace gpui {

enum class Placement : uint8_t {
    Top,
    Bottom,
    Left,
    Right
};

enum class Side : uint8_t {
    Left,
    Right
};

inline bool SideIsLeft(Side s) {
    return s == Side::Left;
}

inline bool AxisIsHorizontal(Axis a) {
    return a == Axis::Horizontal;
}

}

#line 1 "src/base/history.h"

namespace gpui {

struct History {
    Str items[32] = {};
    int n = 0;
    int cursor = -1;

    void Push(Str s);
    bool CanUndo() const;
    bool CanRedo() const;
    Str Undo();
    Str Redo();
};

}

#line 1 "src/base/hover_card.h"

namespace gpui {

struct HoverCardState {
    bool open = false;
    bool hoveringTrigger = false;
    bool hoveringContent = false;
    int openDelayMs = 600;
    int closeDelayMs = 300;

    int timer = 0;

    static void OnOpen(HoverCardState* self, Ctx* cx, const TickEvent* ev);
    static void OnClose(HoverCardState* self, Ctx* cx, const TickEvent* ev);
};

void HoverCardTriggerHover(HoverCardState* self, Ctx* cx, const HoverEvent* ev);
void HoverCardContentHover(HoverCardState* self, Ctx* cx, const HoverEvent* ev);

void HoverCardSetDelays(Ctx* cx, Entity<HoverCardState> state, int openMs,
                        int closeMs);
bool HoverCardIsOpen(Ctx* cx, Entity<HoverCardState> state);

Entity<HoverCardState> HoverCardStateFor(Ctx* cx, Str id);

bool HoverCardIsOpen(Ctx* cx, Str id);

struct HoverCard {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    El* root = nullptr;
    Str id = {};
    Entity<HoverCardState> state = {};

    static HoverCard* New(Ctx* cx, Str id, Entity<HoverCardState> state = {});

    bool IsOpen() const;
    HoverCard* Trigger(El* trigger);
    HoverCard* Content(El* content);
    El* IntoEl();
};
}

#line 1 "src/base/input.h"

namespace gpui {

struct InputBase {
    static El* New(Ctx* cx, Str id, bool interactive = false);
};

struct InputEditorStyle {
    Rgba foreground = Rgb(0x17, 0x17, 0x17);
    Rgba mutedForeground = Rgb(0x73, 0x73, 0x73);
    Rgba caret = Rgb(0x17, 0x17, 0x17);
    Rgba selection = Rgba8(0x6b, 0xb3, 0xf0, 90);
    float fontSize = 12;

    bool mono = false;

    bool mask = false;
    int align = 0;

    const TextSpan* spans = nullptr;
    int nSpans = 0;

    const Selection* matches = nullptr;
    int nMatches = 0;

    int currentMatch = -1;

    Rgba matchBg = {0, 0, 0, 0};
    Rgba currentMatchBg = {0, 0, 0, 0};

    Rgba background = {0, 0, 0, 0};

    Rgba linkText = {0, 0, 0, 0};

    DiagnosticColors diagnostics = {};

    Rgba activeLine = {0, 0, 0, 0};
    Rgba indentGuide = {0, 0, 0, 0};

    int indentWidth = 4;
};

struct Input {
    static El* New(Ctx* cx, InputState* state);
    static El* New(Ctx* cx, InputState* state, const InputEditorStyle& style);
};

struct Textarea {
    static El* New(Ctx* cx, InputState* state);
    static El* New(Ctx* cx, InputState* state, const InputEditorStyle& style,
                   bool lineNumbers = false);
};

struct Editor {
    static El* New(Ctx* cx, InputState* state);
    static El* New(Ctx* cx, InputState* state, const InputEditorStyle& style);
};
}

#line 1 "src/base/link.h"

namespace gpui {

struct LinkStyles {
    StateStyle disabled = {};
};

struct Link {
    static El* New(Ctx* cx, Str id, bool disabled = false,
                   Listener onActivate = {},
                   const LinkStyles* styles = nullptr);
};
}

#line 1 "src/base/list_settings.h"

namespace gpui {

struct ListSettings {

    bool activeHighlight = true;
};

const ListSettings& ListSettingsNow();
void ListSettingsSet(ListSettings s);

struct ListActiveStyle {
    Background bg = {};
    Rgba border = {};
    bool hasBorder = false;
};
ListActiveStyle ListActiveStyleOf(Background active, Rgba activeBorder,
                                  Background accent, bool selected);

El* ListActiveOverlay(Arena* a, Rgba border, float radius);

}

#line 1 "src/base/number_input.h"

namespace gpui {

enum class StepAction : uint8_t {
    Decrement,
    Increment
};

bool NumberStepValue(Str value, StepAction action, double step, bool hasMin,
                     double min, bool hasMax, double max, char* out,
                     int outCap);

bool NumberStepForKey(int key, StepAction* out);

struct NumberInput {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/base/otp_input.h"

namespace gpui {

struct OtpState {
    char value[16] = {};
    int len = 0;
    int length = 6;
    bool masked = false;
    bool disabled = false;
    bool focused = false;

    EntityId blink = {};

    FocusHandle focus = {};
};

char OtpDigitChar(uint32_t c);

bool OtpEditValue(OtpState* s, int key, uint32_t ch);

void OtpFocus(OtpState* s, App* app, Window* win);
void OtpBlur(OtpState* s, App* app, Window* win);
bool OtpCursorVisible(OtpState* s, App* app);

void OtpKeyDown(OtpState* self, Ctx* cx, const KeyEvent* ev);

void OtpClick(OtpState* self, Ctx* cx, const ClickEvent* ev);

struct OtpInput {
    static El* New(Ctx* cx, Str id = {});

    static El* New(Ctx* cx, Str id, Entity<OtpState> state);
};
}

#line 1 "src/base/pagination.h"

namespace gpui {

struct PaginationItem {
    int page = 0;
    int from = 0;
    int to = 0;
};

struct PaginationState {
    int currentPage = 1;
    int totalPages = 1;
    int visiblePages = 5;
    bool disabled = false;
};

PaginationState PaginationStateNew(int currentPage, int totalPages);

int PaginationPrevPage(const PaginationState* s);
int PaginationNextPage(const PaginationState* s);

bool PaginationCanRequest(const PaginationState* s, int page);

int PaginationItems(const PaginationState* s, PaginationItem* out, int cap);

struct Pagination {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/base/popup.h"

namespace gpui {

enum class PopupAnchor : uint8_t {
    TopLeft,
    TopCenter,
    TopRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
    LeftCenter,
    RightCenter
};

Point PopupResolvedCorner(PopupAnchor anchor, Bounds triggerBounds);

El* PopupPlaceContent(El* content, PopupAnchor anchor, float gap);

struct Popup {
    El* root = nullptr;

    PopupAnchor anchor = PopupAnchor::TopLeft;

    static Popup* New(Ctx* cx, Str id, El* trigger,
                      PopupAnchor anchor = PopupAnchor::TopLeft);
    Popup* Anchor(PopupAnchor a);

    Popup* AnchorRight(bool on = true);
    Popup* Content(El* content);
    El* IntoEl();
};
}

#line 1 "src/base/popover.h"

namespace gpui {

struct PopoverState {
    bool open = false;

    bool seeded = false;

    FocusHandle focus = {};
    FocusHandle trackedFocus = {};

    FocusHandle previousFocus = {};
};

void PopoverSetOpenFocused(PopoverState* s, Ctx* cx, bool open);

bool PopoverIsOpen(Ctx* cx, Entity<PopoverState> state);
void PopoverSetOpen(Ctx* cx, Entity<PopoverState> state, bool open);

void PopoverToggle(PopoverState* self, Ctx* cx, const MouseDownEvent* ev,
                   intptr_t button);
void PopoverDismiss(PopoverState* self, Ctx* cx, const ClickEvent* ev);

struct Popover {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    El* root = nullptr;
    Entity<PopoverState> state = {};
    FocusHandle focus = {};
    MouseButton button = MouseButton::Left;

    PopupAnchor anchor = PopupAnchor::TopLeft;

    static Popover* New(Ctx* cx, Str id, Entity<PopoverState> state = {},
                        MouseButton button = MouseButton::Left);
    Popover* Anchor(PopupAnchor v);

    Popover* TrackedFocus(FocusHandle tracked);
    Popover* Trigger(El* trigger);
    Popover* Content(El* content);
    El* IntoEl();
};
}

#line 1 "src/base/positioner.h"

namespace gpui {

enum class PopupAlign : uint8_t {
    Start,
    Center,
    End
};

enum class Anchor : uint8_t {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
};

struct Positioned {
    Bounds bounds;
    Placement placement = Placement::Top;
    bool hasPlacement = false;
};

constexpr float kPopupMargin = 4.f;

Positioned PositionSide(Bounds trigger, Size popup, Size view, float margin,
                        const Placement* preferred, PopupAlign align,
                        float offset);

Positioned PositionCorner(Anchor anchor, Point at, Size popup, Size view,
                          float margin);

}

#line 1 "src/base/progress.h"

namespace gpui {

float ProgressClampValue(float value);

struct Progress {
    static El* New(Ctx* cx, Str id);
};

struct ProgressTrack {
    static El* New(Ctx* cx);
};

struct ProgressIndicator {
    static El* New(Ctx* cx);
};
}

#line 1 "src/base/radio.h"

namespace gpui {

struct Radio {
    static El* New(Ctx* cx, Str id, bool checked = false, bool disabled = false,
                   Listener onChange = {});
};
}

#line 1 "src/base/radio_group.h"

namespace gpui {

struct RadioGroup {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/base/resizable.h"

namespace gpui {

const float kResizablePanelMinSize = 100.f;

bool ResizablePanelResize(float* sizes, const float* mins, const float* maxs,
                          int n, int ix, float size, float containerSize);

void ResizableAdjustToContainer(float* sizes, int n, float containerSize);

const float kResizeHandleSize = 1.f;
const float kResizeHandlePadding = 4.f;

struct ResizeHandleState {
    bool active = false;

    Listener nextDown;
    Listener nextUp;

    static void OnDown(ResizeHandleState* self, Ctx* cx,
                       const MouseDownEvent* ev);
    static void OnUp(ResizeHandleState* self, Ctx* cx, const MouseUpEvent* ev);
};

Entity<ResizeHandleState> ResizeHandleStateFor(Ctx* cx, Str name);

struct ResizableState {
    Axis axis = Axis::Horizontal;

    Vec<float> sizes;
    Vec<float> mins;
    Vec<float> maxs;

    Vec<bool> grows;

    Vec<bool> shown;

    Vec<Bounds> laid;

    Bounds bounds = {};
    float lastContainer = 0;

    int dragging = -1;

    Listener onResized = {};

    ~ResizableState() {
        sizes.Reset();
        mins.Reset();
        maxs.Reset();
        grows.Reset();
        shown.Reset();
        laid.Reset();
    }

    static void OnHandleDown(ResizableState* self, Ctx* cx,
                             const MouseDownEvent* ev, intptr_t ix);
    static void OnHandleDrag(ResizableState* self, Ctx* cx,
                             const DragMoveEvent* ev);
    static void OnHandleUp(ResizableState* self, Ctx* cx,
                           const MouseUpEvent* ev);
};

float ResizablePanelSize(const ResizableState* s, int ix, float declared);

struct Resizable {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Entity<ResizableState> state = {};
    float width = kFill;
    float height = kFill;

    Rgba handleColor = {};
    Rgba handleDragColor = {};

    ArenaVec<El*> panels;
    ArenaVec<float> sizes;
    ArenaVec<float> mins;
    ArenaVec<float> maxs;
    ArenaVec<bool> grows;
    ArenaVec<bool> shown;

    static Resizable* New(Ctx* cx, Str id, Entity<ResizableState> state = {},
                          Axis axis = Axis::Horizontal);
    Resizable* W(float v);
    Resizable* H(float v);
    Resizable* HandleColors(Rgba rest, Rgba dragging);

    Resizable* Panel(El* content, float size,
                     float min = kResizablePanelMinSize, float max = 0);

    Resizable* Grow(El* content, float min = kResizablePanelMinSize);

    Resizable* Flex();

    Resizable* Visible(bool v);
    El* IntoEl();
};

struct ResizablePanel {
    static El* New(Ctx* cx);
};
}

#line 1 "src/base/sankey.h"

namespace gpui {

enum class SankeyAlign : uint8_t {
    Left,
    Right,
    Center,
    Justify
};

enum class SankeyValueScale : uint8_t {
    Linear,
    Sqrt
};

enum class SankeyError : uint8_t {
    None,

    MissingNode,
    CircularLink
};

struct SankeyLink {
    int source = 0;
    int target = 0;
    double value = 0;
};

struct SankeyNodeLayout {
    int index = 0;

    double value = 0;

    int depth = 0;
    int height = 0;

    int layer = 0;
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;

    int srcStart = 0, srcCount = 0;
    int tgtStart = 0, tgtCount = 0;
};

struct SankeyLinkLayout {
    int index = 0;
    int source = 0;
    int target = 0;
    double value = 0;
    float y0 = 0, y1 = 0;

    float width = 0;
    float sourceWidth = 0;
    float targetWidth = 0;
};

struct SankeyGraph {
    Vec<SankeyNodeLayout> nodes;
    Vec<SankeyLinkLayout> links;

    Vec<int> srcLinks;
    Vec<int> tgtLinks;

    int errNode = 0;

    void Reset() {
        nodes.Reset();
        links.Reset();
        srcLinks.Reset();
        tgtLinks.Reset();
    }
};

int SankeyLayerCount(const SankeyGraph* g);

struct Sankey {
    float nodeWidth = 24.f;
    float nodePadding = 8.f;
    SankeyAlign align = SankeyAlign::Justify;
    int iterations = 6;
    SankeyValueScale valueScale = SankeyValueScale::Linear;

    float x0 = 0, y0 = 0, x1 = 1, y1 = 1;
};

SankeyError SankeyTopology(const Sankey* s, int nodeCount,
                           const SankeyLink* links, int nLinks,
                           SankeyGraph* out);

void SankeyLayoutFrom(const Sankey* s, SankeyGraph* g);

SankeyError SankeyLayout(const Sankey* s, int nodeCount,
                         const SankeyLink* links, int nLinks, SankeyGraph* out);

}

#line 1 "src/base/scrollbar.h"

namespace gpui {

float ScrollbarThumbSize(float track, float container, float content);

float ScrollbarThumbPos(float track, float thumb, float offset, float container,
                        float content);

float ScrollbarOffsetForTrackPress(float pos, float trackOrigin, float track,
                                   float thumb, float container, float content);

float ScrollbarOffsetForDrag(float pos, float grab, float trackOrigin,
                             float track, float thumb, float container,
                             float content);

enum class ScrollAxis : uint8_t {
    Vertical,
    Horizontal,
    Both
};

struct Scrollbar {

    static El* New(Ctx* cx);

    static El* New(Ctx* cx, Str id, float scrollY, float scrollX,
                   Listener onScroll, ScrollAxis axis = ScrollAxis::Vertical,
                   ScrollbarMode mode = ScrollbarMode::Always);

    static El* Vertical(Ctx* cx, Str id, float scrollY, Listener onScroll,
                        ScrollbarMode mode = ScrollbarMode::Always);
};
}

#line 1 "src/base/sheet.h"

namespace gpui {

enum class SheetOverlayPress : uint8_t {

    Ignore,

    Swallow,

    Close
};

SheetOverlayPress SheetOverlayPressAction(bool overlayInteractive,
                                          bool overlayClosable,
                                          MouseButton button, float pressY,
                                          bool hasDismissBefore,
                                          float dismissBeforeY);

bool SheetClosesOnKey(int key);

struct Sheet {
    Ctx* cx = nullptr;
    El* root = nullptr;

    Str trap = {};

    static Sheet* New(Ctx* cx);
    Sheet* Trap(Str name);
    Sheet* Overlay(El* overlay);
    Sheet* Surface(El* surface);
    El* IntoEl();
};
}

#line 1 "src/base/slider.h"

namespace gpui {

struct Slider {
    static El* New(Ctx* cx, int clickId = 0);
};

struct SliderTrack {
    static El* New(Ctx* cx, SliderState* state = nullptr,
                   Axis axis = Axis::Horizontal);
};

struct SliderIndicator {
    static El* New(Ctx* cx, SliderState* state = nullptr);
};
struct SliderThumb {
    static El* New(Ctx* cx);
};
}

#line 1 "src/base/switch.h"

namespace gpui {

struct Switch {
    static El* New(Ctx* cx, Str id, bool checked = false, bool disabled = false,
                   Listener onChange = {});
};

struct SwitchTrack {
    static El* New(Ctx* cx, Str id);
};

struct SwitchThumb {
    static El* New(Ctx* cx);
};
}

#line 1 "src/base/table.h"

namespace gpui {

struct Table {
    static El* New(Ctx* cx, Str id);
};
struct TableHeader {
    static El* New(Ctx* cx, Str id);
};
struct TableBody {
    static El* New(Ctx* cx, Str id);
};
struct TableRow {
    static El* New(Ctx* cx, Str id);
};
struct TableHead {
    static El* New(Ctx* cx, Str id);
};
struct TableCell {
    static El* New(Ctx* cx, Str id);
};
struct TableCaption {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/base/tabs.h"

namespace gpui {

struct Tabs {
    static El* New(Ctx* cx, Str id);
};

struct Tab {
    static El* New(Ctx* cx, Str id, bool disabled = false,
                   Listener onClick = {});
};
}

#line 1 "src/base/text_boundary.h"

namespace gpui {

CharKind CharKindOf(uint32_t c);

int Utf8ClipLeft(Str s, int off);

bool TextWordRangeAt(Str s, int off, int* outA, int* outB);

void TextLineRangeAt(Str s, int off, int* outA, int* outB);

}

#line 1 "src/base/text_selection.h"

namespace gpui {

struct TextSelectionGesture {
    bool selecting = false;
    bool didHitText = false;
};

void TextSelectionBegin(TextSelectionGesture* g, bool insideText);

void TextSelectionExtend(TextSelectionGesture* g, bool insideText);

void TextSelectionEnd(TextSelectionGesture* g);

bool TextSelectionPublishes(const TextSelectionGesture* g);

void TextSelectionClear(TextSelectionGesture* g);

struct TextSelection {
    static El* New(Ctx* cx, Str id, int clickId = 0);
};

struct WindowSelection {
    TextSelectionGesture gesture;

    int anchor = -1;
    int cursor = -1;

    int scope = 0;

    SelectionFormat format = SelectionFormat::Plain;
};

WindowSelection* WindowSelectionOf(Window* win);
void WindowSelectionFree(Window* win);

void WindowSelectionPress(Window* win, float x, float y, int clickCount,
                          bool extend);

void WindowSelectionDrag(Window* win, float x, float y);

void WindowSelectionRelease(Window* win);

bool WindowSelectionHas(const Window* win);

void WindowSelectionClear(Window* win);

int WindowSelectionText(Window* win, char* out, int cap);
int WindowSelectionTextAs(Window* win, char* out, int cap, SelectionFormat fmt);

void WindowSelectionSetFormat(Window* win, SelectionFormat fmt);
SelectionFormat WindowSelectionFormat(Window* win);

bool WindowSelectionCopy(Window* win);

void WindowSelectionApply(Window* win);
}

#line 1 "src/base/theme_tokens.h"

namespace gpui {

struct SemanticColorTokens {
    Rgba background = {};
    Rgba foreground = {};
    Rgba surface = {};
    Rgba surfaceForeground = {};
    Rgba primary = {};
    Rgba primaryForeground = {};
    Rgba secondary = {};
    Rgba secondaryForeground = {};
    Rgba muted = {};
    Rgba mutedForeground = {};
    Rgba accent = {};
    Rgba accentForeground = {};
    Rgba destructive = {};
    Rgba destructiveForeground = {};
    Rgba border = {};
    Rgba input = {};
    Rgba ring = {};
};

struct SemanticRadiusTokens {
    float none = 0;
    float sm = 3;
    float md = 6;
    float lg = 8;
    float xl = 12;
    float full = 9999;
};

struct SemanticSpacingTokens {
    float xxs = 2;
    float xs = 4;
    float sm = 8;
    float md = 12;
    float lg = 16;
    float xl = 24;
    float xxl = 32;
};

struct SemanticTextStyle {
    float size = 16;
    float lineHeight = 24;
    float weight = 400;
};

struct SemanticTypographyTokens {
    Str sans = {};
    Str mono = {};
    SemanticTextStyle xs = {12, 16, 400};
    SemanticTextStyle sm = {14, 20, 400};
    SemanticTextStyle md = {16, 24, 400};
    SemanticTextStyle lg = {18, 28, 400};
    SemanticTextStyle xl = {20, 28, 400};
    SemanticTextStyle monoMd = {13, 20, 400};
};

struct SemanticShadow {
    float x = 0;
    float y = 0;
    float blur = 0;
    float spread = 0;
    Rgba color = {};
};

struct SemanticShadowTokens {

    bool has = false;
    SemanticShadow sm = {};
    SemanticShadow md = {};
    SemanticShadow lg = {};
};

const float kMonoFontSize = 13.f;

SemanticShadowTokens SemanticShadowElevations(Rgba color);

struct SemanticThemeTokens {
    SemanticColorTokens colors = {};
    SemanticRadiusTokens radius = {};
    SemanticSpacingTokens spacing = {};
    SemanticTypographyTokens typography = {};
    SemanticShadowTokens shadow = {};
};

SemanticThemeTokens ThemeSemanticTokens(const Theme& t);

void ThemeApplySemanticTokens(Theme* t, const SemanticThemeTokens& tokens);

}

#line 1 "src/base/toast.h"

namespace gpui {

enum class ToastStatus : uint8_t {

    Starting,

    Present,

    Ending
};

struct ToastEntry {
    int id = 0;
    ToastStatus status = ToastStatus::Starting;

    bool hasTimeout = false;
    int timeoutRemainingMs = 0;

    int elapsedMs = 0;
};

const int kToastStackCap = 16;

const int kToastTransitionMs = 400;
const int kToastExitMs = 200;

const float kToastCollapsedPeek = 14.f;
const float kToastExpandedGap = 14.f;
const float kToastCollapsedScaleStep = 0.05f;
const int kToastCollapsedVisible = 3;

struct ToastStackState {
    ToastEntry entries[kToastStackCap] = {};
    int n = 0;

    int transitionMs = kToastTransitionMs;
    int exitMs = kToastExitMs;

    bool hovered = false;
    bool focused = false;

    bool IsExpanded() const { return hovered || focused; }
};

float ToastStackGeometry(const float* heights, int n, float peek, float gap,
                         bool anchoredBottom, float* collapsedOffsets,
                         float* expandedOffsets, float* expandedHeight);

bool ToastPush(ToastStackState* s, int id, int timeoutMs);

bool ToastRemove(ToastStackState* s, int id);

bool ToastAdvance(ToastStackState* s, int deltaMs, bool paused);

struct Toast {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/base/toggle.h"

namespace gpui {

struct Toggle {
    static El* New(Ctx* cx, Str id, bool pressed = false, bool disabled = false,
                   Listener onChange = {});
};
}

#line 1 "src/base/toggle_group.h"

namespace gpui {

struct ToggleGroup {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/base/tooltip.h"

namespace gpui {

struct Tooltip {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/base/virtual_list.h"

namespace gpui {

struct VirtualRange {
    int first = 0;
    int end = 0;
};

VirtualRange VirtualListVisibleRange(const float* sizes, int count,
                                     float offset, float viewport);

VirtualRange VirtualListVisibleRows(int count, float rowSize, float offset,
                                    float viewport);

float VirtualListItemOrigin(const float* sizes, int count, int ix);

enum class ScrollStrategy : uint8_t {
    Top,
    Center,
    Bottom
};

float VirtualListScrollTo(float origin, float size, float offset,
                          float viewport, float contentSize,
                          ScrollStrategy strategy);

float VirtualListScrollToItem(const float* sizes, int count, int ix,
                              float offset, float viewport,
                              ScrollStrategy strategy);

float VirtualListScrollToRow(int count, float rowSize, int ix, float offset,
                             float viewport, ScrollStrategy strategy);

float VirtualListContentSize(const float* sizes, int count);

struct VirtualListScrollHandle {
    Axis axis = Axis::Vertical;
    int itemsCount = 0;

    float offset = 0;
    float viewport = 0;
    float contentSize = 0;

    bool pending = false;
    int pendingIx = 0;

    int pendingOffset = 0;
    ScrollStrategy pendingStrategy = ScrollStrategy::Top;
};

void VirtualListScrollToItemDeferred(VirtualListScrollHandle* h, int ix,
                                     ScrollStrategy strategy);
void VirtualListScrollToItemDeferredWithOffset(VirtualListScrollHandle* h,
                                               int ix, ScrollStrategy strategy,
                                               int offset);

void VirtualListScrollToBottomDeferred(VirtualListScrollHandle* h);

bool VirtualListHandleLayout(VirtualListScrollHandle* h, const float* sizes,
                             int count, float itemSize, float viewport);

VirtualRange VirtualListHandleRange(const VirtualListScrollHandle* h,
                                    const float* sizes, int count,
                                    float itemSize);

using VirtualRowFn = El* (*)(void* user, Ctx* cx, int ix);

struct VirtualListOpts {
    int count = 0;
    float rowH = 32;
    float viewH = 192;
    const float* sizes = nullptr;

    float scrollY = 0;
    float scrollX = 0;
    VirtualListScrollHandle* handle = nullptr;

    int scrollId = 0;
    Listener onScroll = {};

    ScrollAxis axis = ScrollAxis::Both;

    float pad = 0;
    VirtualRowFn row = nullptr;
    void* user = nullptr;
};

struct VirtualList {

    static El* New(Ctx* cx, Str id);

    static El* New(Ctx* cx, Str id, const VirtualListOpts& o);
};
}

#line 1 "src/base/tree.h"

namespace gpui {

enum class TreeAction : uint8_t {
    None,
    SelectPrev,
    SelectNext,
    Collapse,
    Expand,
    Confirm
};

void TreeInitKeys();
Str TreeContext();
TreeAction TreeActionOf(uint32_t id);

int TreeSelectPrev(int selected, int count);
int TreeSelectNext(int selected, int count);

bool TreeCollapses(bool isFolder, bool isExpanded);
bool TreeExpands(bool isFolder, bool isExpanded);

struct TreeItem {
    Str id = {};
    Str label = {};
    int parent = -1;
    int depth = 0;

    bool folder = false;
    bool expanded = false;
    bool disabled = false;
};

enum class TreeEventKind : uint8_t {
    Expanded,
    Collapsed
};

struct TreeEvent {
    TreeEventKind kind = TreeEventKind::Expanded;

    Str id = {};
    int ix = 0;
};

struct TreeState {

    Vec<TreeItem> items;

    Vec<int> entries;
    int selected = -1;
    int rightClicked = -1;

    float rowH = 34;
    float scrollY = 0;

    float viewportH = 0;
    Listener onEvent;

    EntityId self = {};

    static void OnRowClick(TreeState* self, Ctx* cx, const ClickEvent* ev,
                           intptr_t entryIx);
    static void OnRowMouseDown(TreeState* self, Ctx* cx,
                               const MouseDownEvent* ev, intptr_t entryIx);
    static void OnScroll(TreeState* self, Ctx* cx, const ScrollEvent* ev);

    ~TreeState() {
        items.Reset();
        entries.Reset();
    }
};

int TreeAddItem(TreeState* s, Str id, Str label, int parent);

void TreeRebuild(TreeState* s);

int TreeIndexOf(const TreeState* s, Str id);

const TreeItem* TreeEntryItem(const TreeState* s, int entryIx);

bool TreeToggleExpandAt(TreeState* s, int entryIx, bool* expandedOut);
void TreeToggleExpand(TreeState* s, Ctx* cx, int entryIx);

int TreeRevealItem(TreeState* s, Str id);

void TreeScrollToItem(TreeState* s, int entryIx, ScrollStrategy strategy);
void TreeSetSelected(TreeState* s, Ctx* cx, int entryIx);

void TreeClickEntry(TreeState* s, Ctx* cx, int entryIx);
void TreePerform(TreeState* s, Ctx* cx, TreeAction act);

void TreeOnAction(TreeState* self, Ctx* cx, const ActionEvent* ev);
void TreeBindKeys(Ctx* cx, El* root, Entity<TreeState> state);

struct Tree {
    static El* New(Ctx* cx);
};

using TreeRowFn = El* (*)(void* user, Ctx* cx, int entryIx);

struct TreeList {

    static El* New(Ctx* cx, Str id, Entity<TreeState> state, float h,
                   TreeRowFn row, void* user);
};

struct TreeItemEl {
    static El* New(Ctx* cx, Str id = {}, Listener onClick = {});
};
}

#line 1 "src/base/lib.h"

#line 1 "src/ui/sizing.h"

namespace gpui {

enum class UiSize : uint8_t {
    XSmall,
    Small,
    Medium,
    Large
};

inline float UiSizePx(UiSize s) {
    switch (s) {
        case UiSize::XSmall:
            return 20;
        case UiSize::Small:
            return 24;
        case UiSize::Large:
            return 36;
        default:
            return 28;
    }
}

inline float UiIconPx(UiSize s) {
    switch (s) {
        case UiSize::XSmall:
            return 12;
        case UiSize::Small:
            return 14;
        case UiSize::Large:
            return 24;
        default:
            return 16;
    }
}

inline Edges UiTableCellPadding(UiSize s) {

    switch (s) {
        case UiSize::XSmall:
            return Edges::New(4, 4, 2, 2);
        case UiSize::Small:
            return Edges::New(6, 6, 3, 3);
        case UiSize::Large:
            return Edges::New(12, 12, 8, 8);
        default:
            return Edges::New(8, 8, 4, 4);
    }
}

inline float UiTableRowHeight(UiSize s) {
    switch (s) {
        case UiSize::XSmall:
            return 26;
        case UiSize::Small:
            return 30;
        case UiSize::Large:
            return 40;
        default:
            return 32;
    }
}

inline float UiFontPx(UiSize s) {
    switch (s) {
        case UiSize::XSmall:
            return 11;
        case UiSize::Small:
            return 12;
        case UiSize::Large:
            return 16;
        default:
            return 14;
    }
}

namespace component {

inline El* BindClick(El* e, Str name, Listener onClick) {
    e->PathId(name);
    if (onClick.IsValid()) {
        e->OnClick(onClick);
    }
    return e;
}

inline El* BindPathClick(El* e, Str name, Listener onClick) {
    e->PathClick(name);
    if (onClick.IsValid()) {
        e->OnClick(onClick);
    }
    return e;
}
}
}

#line 1 "src/ui/accordion.h"

namespace gpui {

namespace component {

struct AccordionStyle {
    float padT = -1;
    float padB = -1;
    float padL = -1;
    float padR = -1;
    Rgba fg = {};
};

struct AccordionItem {
    Ctx* cx = nullptr;
    El* title = nullptr;
    El* content = nullptr;
    bool open = false;
    IconName icon = IconName::None;
    AccordionStyle titleStyle = {};
    AccordionStyle contentStyle = {};

    static AccordionItem* New(Ctx* cx);
    AccordionItem* Title(El* t);
    AccordionItem* Title(Str s);
    AccordionItem* Icon(IconName i);
    AccordionItem* Open(bool v);
    AccordionItem* Child(El* c);
    AccordionItem* Child(Str s);
    AccordionItem* TitleStyle(const AccordionStyle& s);
    AccordionItem* ContentStyle(const AccordionStyle& s);
};

struct Accordion {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    bool multiple = false;
    bool bordered = true;
    bool disabled = false;
    UiSize size = UiSize::Medium;
    AccordionItem* items[8] = {};
    int nItems = 0;
    Listener onToggle;

    static Accordion* New(Ctx* cx, Str id);
    Accordion* Multiple(bool v);
    Accordion* Bordered(bool v);
    Accordion* Disabled(bool v);
    Accordion* WithSize(UiSize s);
    Accordion* Item(AccordionItem* it);
    Accordion* OnToggle(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/alert.h"

namespace gpui {

namespace component {

enum class AlertVariant : uint8_t {
    Default,
    Info,
    Success,
    Warning,
    Error
};

struct Alert {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    AlertVariant variant = AlertVariant::Default;
    IconName icon = IconName::Info;
    Str title = {};
    Str message = {};

    bool markdown = false;

    El* content = nullptr;
    UiSize size = UiSize::Medium;
    bool banner = false;
    bool visible = true;
    Listener onClose;

    static Alert* New(Ctx* cx, Str id, Str message);
    static Alert* Info(Ctx* cx, Str id, Str message);
    static Alert* Success(Ctx* cx, Str id, Str message);
    static Alert* Warning(Ctx* cx, Str id, Str message);
    static Alert* Error(Ctx* cx, Str id, Str message);
    Alert* Title(Str s);
    Alert* Icon(IconName n);
    Alert* Markdown(bool v = true);
    Alert* Content(El* e);
    Alert* Banner();
    Alert* Visible(bool v);
    Alert* OnClose(Listener fn);
    Alert* WithSize(UiSize s);
    El* IntoEl();
};

}
}

#line 1 "src/ui/avatar.h"

namespace gpui {

namespace component {

float AvatarSizePx(UiSize s);

Str AvatarInitials(char* out, int cap, Str name);

struct Avatar {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str initials = {};
    Background bg = {};
    bool hasBg = false;
    float size = 48;

    float textPx = -1;
    float radius = -1;
    float borderW = 1;
    Rgba borderC = {};
    bool hasBorderC = false;
    IconName placeholder = IconName::User;

    Str src = {};

    static Avatar* New(Ctx* cx);

    Avatar* Name(Str s);
    Avatar* Src(Str url);
    Avatar* Initials(Str s);
    Avatar* Bg(Background c);
    Avatar* Size(float v);
    Avatar* WithSize(UiSize s);
    Avatar* Radius(float v);
    Avatar* Border(float w, Rgba c);
    Avatar* Placeholder(IconName n);
    El* IntoEl();
};

struct AvatarGroup {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Avatar* avatars[16] = {};
    int n = 0;
    UiSize size = UiSize::Medium;
    int limit = 3;
    bool ellipsis = false;

    static AvatarGroup* New(Ctx* cx);
    AvatarGroup* Child(Avatar* av);
    AvatarGroup* WithSize(UiSize s);
    AvatarGroup* Limit(int v);
    AvatarGroup* Ellipsis();
    El* IntoEl();
};

}
}

#line 1 "src/ui/badge.h"

namespace gpui {

namespace component {

enum class BadgeKind : uint8_t {
    Number,
    Dot,
    Icon
};

struct Badge {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    int count = 0;
    int max = 99;
    BadgeKind kind = BadgeKind::Number;
    IconName icon = IconName::None;
    Rgba color = {};
    bool hasColor = false;
    UiSize size = UiSize::Medium;
    El* child = nullptr;

    static Badge* New(Ctx* cx);
    Badge* Count(int n);
    Badge* Max(int n);
    Badge* Dot();
    Badge* Icon(IconName n);
    Badge* Color(Rgba c);
    Badge* WithSize(UiSize s);
    Badge* Child(El* c);
    El* IntoEl();
};

}
}

#line 1 "src/ui/breadcrumb.h"

namespace gpui {

namespace component {

struct BreadcrumbItem {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str label = {};
    Listener onClick;
    bool disabled = false;

    bool isLast = false;
    int ix = 0;

    static BreadcrumbItem* New(Ctx* cx, Str label);
    BreadcrumbItem* Disabled(bool v);
    BreadcrumbItem* OnClick(Listener fn);
    El* IntoEl();
};

struct Breadcrumb {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    BreadcrumbItem* items[8] = {};
    int n = 0;

    static Breadcrumb* New(Ctx* cx);
    Breadcrumb* Child(BreadcrumbItem* item);

    Breadcrumb* Child(Str label);
    El* IntoEl();
};

}
}

#line 1 "src/ui/button.h"

namespace gpui {

namespace component {

enum class ButtonVariant : uint8_t {
    Default,
    Primary,
    Secondary,
    Danger,
    Info,
    Success,
    Warning,
    Ghost,
    Link,
    Text
};

struct Button {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Str label = {};
    IconName icon = IconName::None;

    bool hasIconColor = false;
    Rgba iconColor = {};

    IconName iconRight = IconName::None;
    ButtonVariant variant = ButtonVariant::Default;
    UiSize size = UiSize::Medium;
    bool outline = false;
    bool disabled = false;
    bool loading = false;
    bool compact = false;
    bool justifyStart = false;
    bool selected = false;
    bool dropdown = false;
    bool focusRing = true;
    int tabIndex = 0;
    bool tabStop = true;
    bool hasCustom = false;
    Rgba custom = {};
    Str tooltip = {};
    El* extra = nullptr;

    float sizePx = 0;
    IconName loadingIcon = IconName::Loader;

    bool joined = false;
    bool edgeT = true, edgeB = true, edgeL = true, edgeR = true;
    Listener onClick;

    StateStyle selectedStyle = {};
    StateStyle disabledStyle = {};

    static Button* New(Ctx* cx, Str id);
    Button* Label(Str s);
    Button* Icon(IconName n);
    Button* IconColor(Rgba c);
    Button* IconRight(IconName n);
    Button* Primary();
    Button* Secondary();
    Button* Danger();
    Button* Warning();
    Button* Success();
    Button* Info();
    Button* Ghost();
    Button* Link();
    Button* Text();
    Button* Outline();
    Button* Compact();

    Button* JustifyStart(bool v = true);
    Button* Selected(bool v);
    Button* SelectedStyle(const StateStyle& s);
    Button* DisabledStyle(const StateStyle& s);
    Button* DropdownCaret(bool v = true);
    Button* Custom(Rgba c);
    Button* Extra(El* e);
    Button* Loading(bool v);
    Button* Disabled(bool v);
    Button* WithSize(UiSize s);

    Button* Size(float px);

    Button* LoadingIcon(IconName n);

    Button* TabIndex(int v);
    Button* TabStop(bool v);
    Button* FocusRing(bool v);
    Button* Tooltip(Str s);
    Button* OnClick(Listener l);
    El* IntoEl();
};

struct DropdownMenu;
struct PopupMenu;

struct DropdownButton {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Button* button = nullptr;
    PopupMenu* menu = nullptr;
    bool selected = false;
    bool disabled = false;
    bool outline = false;

    bool hasVariant = false;
    ButtonVariant variant = ButtonVariant::Default;
    bool hasSize = false;
    UiSize size = UiSize::Medium;

    bool anchorRight = true;

    static DropdownButton* New(Ctx* cx, Str id);
    DropdownButton* Button_(component::Button* b);
    DropdownButton* Menu(PopupMenu* m);
    DropdownButton* Selected(bool v);
    DropdownButton* Disabled(bool v);
    DropdownButton* Outline();
    DropdownButton* WithVariant(ButtonVariant v);
    DropdownButton* WithSize(UiSize s);
    El* IntoEl();
};

struct ButtonGroup {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Button* children[8] = {};
    int n = 0;
    bool multiple = false;
    bool disabled = false;
    bool vertical = false;
    bool compact = false;
    bool outline = false;
    bool hasVariant = false;
    ButtonVariant variant = ButtonVariant::Default;
    bool hasSize = false;
    UiSize size = UiSize::Medium;

    Listener onClick;

    static ButtonGroup* New(Ctx* cx, Str id);
    ButtonGroup* Child(Button* b);
    ButtonGroup* Multiple(bool v);
    ButtonGroup* Disabled(bool v);
    ButtonGroup* Vertical(bool v = true);
    ButtonGroup* Compact();
    ButtonGroup* Outline();
    ButtonGroup* WithVariant(ButtonVariant v);
    ButtonGroup* WithSize(UiSize s);
    ButtonGroup* OnClick(Listener l);
    El* IntoEl();
};

}
}

#line 1 "src/ui/time.h"

namespace gpui {

namespace component {

struct Calendar {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    int year = 2026;
    int month = 1;
    int day = 1;
    int selectedYear = 0;
    int selectedMonth = 0;
    LocalDate rangeEnd = {};
    UiSize size = UiSize::Medium;
    int numberOfMonths = 1;
    CalendarView view = CalendarView::Day;
    int yearMin = 0;
    int yearMax = 0;
    int yearPageStart = 0;
    DateMatcher disabledMatcher = {};

    bool bare = false;
    Listener onDay;
    Listener onDate;
    Listener onPrev;
    Listener onNext;
    Listener onMonthToggle;
    Listener onYearToggle;
    Listener onMonth;
    Listener onYear;

    static Calendar* New(Ctx* cx);
    Calendar* Year(int y);
    Calendar* Month(int m);
    Calendar* Day(int d);
    Calendar* Selection(int y, int m, int d);
    Calendar* RangeEnd(int y, int m, int d);
    Calendar* WithSize(UiSize s);
    Calendar* NumberOfMonths(int count);
    Calendar* View(CalendarView value);
    Calendar* YearRange(int minYear, int maxYear, int pageStart);
    Calendar* DisabledMatcher(DateMatcher matcher);
    Calendar* Bare();
    Calendar* OnDay(Listener fn);
    Calendar* OnDate(Listener fn);
    Calendar* OnPrev(Listener fn);
    Calendar* OnNext(Listener fn);
    Calendar* OnMonthToggle(Listener fn);
    Calendar* OnYearToggle(Listener fn);
    Calendar* OnMonth(Listener fn);
    Calendar* OnYear(Listener fn);
    El* IntoEl();
};

struct DateRangePreset {
    Str label = {};
    LocalDate start = {};
    LocalDate end = {};
    intptr_t arg = 0;
};

enum class DateFormat : uint8_t {
    Slash,
    Dash
};

struct DatePicker {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    int year = 2026;
    int month = 1;
    int day = 1;
    int viewYear = 0;
    int viewMonth = 0;

    int year2 = 0;
    int month2 = 0;
    int day2 = 0;
    Str placeholder = {};
    DateFormat format = DateFormat::Slash;
    UiSize size = UiSize::Medium;
    float width = kFill;

    bool cleanable = false;
    bool appearance = true;
    bool focusRing = true;
    bool range = false;
    bool open = false;
    int numberOfMonths = 1;
    CalendarView calendarView = CalendarView::Day;
    int yearMin = 0;
    int yearMax = 0;
    int yearPageStart = 0;
    DateMatcher disabledMatcher = {};
    const DateRangePreset* presets = nullptr;
    int presetsCount = 0;
    Listener onToggle;
    Listener onDay;
    Listener onDate;
    Listener onClear;
    Listener onPrev;
    Listener onNext;
    Listener onMonthToggle;
    Listener onYearToggle;
    Listener onMonth;
    Listener onYear;
    Listener onPreset;

    static DatePicker* New(Ctx* cx);
    DatePicker* Id(Str value);
    DatePicker* Year(int y);
    DatePicker* Month(int m);
    DatePicker* Day(int d);
    DatePicker* View(int y, int m);
    DatePicker* RangeEnd(int y, int m, int d);
    DatePicker* Placeholder(Str s);
    DatePicker* Format(DateFormat f);
    DatePicker* WithSize(UiSize s);
    DatePicker* W(float v);
    DatePicker* Cleanable(bool v = true);
    DatePicker* Appearance(bool v);

    DatePicker* FocusRing(bool v);
    DatePicker* Range(bool v = true);
    DatePicker* NumberOfMonths(int count);
    DatePicker* CalendarMode(CalendarView value);
    DatePicker* YearRange(int minYear, int maxYear, int pageStart);
    DatePicker* DisabledMatcher(DateMatcher matcher);
    DatePicker* Presets(const DateRangePreset* values, int count,
                        Listener onSelect);
    DatePicker* Open(bool v);
    DatePicker* OnToggle(Listener fn);
    DatePicker* OnDay(Listener fn);
    DatePicker* OnDate(Listener fn);
    DatePicker* OnClear(Listener fn);
    DatePicker* OnPrev(Listener fn);
    DatePicker* OnNext(Listener fn);
    DatePicker* OnMonthToggle(Listener fn);
    DatePicker* OnYearToggle(Listener fn);
    DatePicker* OnMonth(Listener fn);
    DatePicker* OnYear(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/chart.h"

namespace gpui {

namespace component {

struct PieSlice {
    float value = 0;
    Rgba color = {};

    float outerInset = 0;
    Str label = {};
};

struct PieChart {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    PieSlice slices[12] = {};
    int n = 0;
    float outerRadius = 100;
    float innerRadius = 0;
    float padAngle = 0;

    bool hasLabels = false;

    float labelGap = 15;
    bool hasLabelColor = false;
    Rgba labelColor = {};

    static PieChart* New(Ctx* cx);
    PieChart* Slice(float value, Rgba color, float outerInset = 0);
    PieChart* Label(Str text);
    PieChart* OuterRadius(float r);
    PieChart* InnerRadius(float r);
    PieChart* PadAngle(float radians);
    PieChart* LabelGap(float gap);
    PieChart* LabelColor(Rgba c);
    El* IntoEl();
};

struct AreaChart {
    Arena* a = nullptr;
    Str tooltipName = {};
    bool tooltip = false;
    Ctx* cx = nullptr;
    const float* ys = nullptr;
    int n = 0;
    const char* const* labels = nullptr;
    int tickMargin = 15;

    bool overlay = false;
    Rgba stroke = {};
    Rgba fill = {};

    Rgba fillBottom = {};
    ChartStroke strokeStyle = ChartStroke::Natural;

    ChartSeriesExtra more[4] = {};
    int nMore = 0;

    static AreaChart* New(Ctx* cx, const float* ys, int n);

    AreaChart* Y(const float* ys);

    AreaChart* Tooltip(Str name);
    AreaChart* Stroke(Rgba c);
    AreaChart* Fill(Rgba c);

    AreaChart* Fill(Rgba top, Rgba bottom);
    AreaChart* Labels(const char* const* l);
    AreaChart* TickMargin(int n);
    AreaChart* Overlay(bool v = true);

    AreaChart* Linear();
    AreaChart* StepAfter();
    El* IntoEl();
};

struct LineChart {
    Arena* a = nullptr;
    Str tooltipName = {};
    bool tooltip = false;
    Ctx* cx = nullptr;
    const float* ys = nullptr;
    int n = 0;
    const char* const* labels = nullptr;
    int tickMargin = 15;
    Rgba stroke = {};
    float domainMin = 0;
    float domainMax = 0;
    ChartStroke strokeStyle = ChartStroke::Natural;
    bool dot = false;

    static LineChart* New(Ctx* cx, const float* ys, int n);

    LineChart* Tooltip(Str name);
    LineChart* Stroke(Rgba c);
    LineChart* Labels(const char* const* l);
    LineChart* TickMargin(int n);
    LineChart* Domain(float lo, float hi);
    LineChart* Linear();
    LineChart* StepAfter();
    LineChart* Dot(bool v = true);
    El* IntoEl();
};

struct BarChart {
    Arena* a = nullptr;
    Str tooltipName = {};
    bool tooltip = false;
    Ctx* cx = nullptr;
    const float* ys = nullptr;
    int n = 0;
    const char* const* labels = nullptr;
    int tickMargin = 1;
    Rgba fill = {};

    float padding = 0.2f;
    float radius = 4;
    float domainMin = 0;
    float domainMax = 0;
    BarAlign align = BarAlign::Bottom;

    const float* bases = nullptr;

    bool overlay = false;
    bool labelValues = false;

    bool valueAxis = false;
    int valueTickCount = 4;

    const Rgba* fills = nullptr;
    bool gradient = false;
    bool gradientPerBar = false;
    bool gradientDiagonal = false;
    Rgba gradientFrom = {};
    Rgba gradientTo = {};

    static BarChart* New(Ctx* cx, const float* ys, int n);

    BarChart* Tooltip(Str name);
    BarChart* Fill(Rgba c);
    BarChart* Labels(const char* const* l);
    BarChart* TickMargin(int n);

    BarChart* ValueAxis(bool on = true);

    BarChart* ValueTickCount(int count);
    BarChart* Padding(float v);
    BarChart* Radius(float v);
    BarChart* Domain(float lo, float hi);
    BarChart* Alignment(BarAlign v);
    BarChart* Base(const float* y0);
    BarChart* Overlay(bool v = true);

    BarChart* LabelValues(bool v = true);
    BarChart* Fills(const Rgba* colors);

    BarChart* FillGradient(Rgba from, Rgba to, bool perBar = false);

    BarChart* FillGradientDiagonal(Rgba from, Rgba to);
    El* IntoEl();
};

struct CandlestickChart {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    const float* opens = nullptr;
    const float* highs = nullptr;
    const float* lows = nullptr;
    const float* closes = nullptr;
    int n = 0;
    const char* const* labels = nullptr;
    int tickMargin = 1;
    Rgba up = {};
    Rgba down = {};
    float padding = 0.3f;
    float bodyWidthRatio = 0.8f;

    static CandlestickChart* New(Ctx* cx, const float* opens,
                                 const float* highs, const float* lows,
                                 const float* closes, int n);
    CandlestickChart* Colors(Rgba up, Rgba down);
    CandlestickChart* Labels(const char* const* l);
    CandlestickChart* TickMargin(int n);
    CandlestickChart* Padding(float v);
    CandlestickChart* BodyWidthRatio(float v);
    El* IntoEl();
};

struct RadarChart {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    const float* values = nullptr;
    int n = 0;
    const char* const* labels = nullptr;
    Rgba stroke = {};
    Rgba fill = {};
    float domainMin = 0;
    float domainMax = 0;

    bool overlay = false;
    bool dot = false;
    float outerRadius = 0;
    int gridLevels = 4;

    static RadarChart* New(Ctx* cx, const float* values, int n);
    RadarChart* Stroke(Rgba c);
    RadarChart* Fill(Rgba c);
    RadarChart* Labels(const char* const* l);
    RadarChart* Domain(float lo, float hi);
    RadarChart* Overlay(bool v = true);
    RadarChart* Dot(bool v = true);
    RadarChart* OuterRadius(float v);
    RadarChart* GridLevels(int v);
    El* IntoEl();
};

const float kSankeyChartNodeWidth = 10;
const float kSankeyChartNodePadding = 16;
const float kSankeyChartLinkOpacity = 0.3f;
const float kSankeyChartMinLinkWidth = 1;
const float kSankeyChartLabelGap = 6;

const float kSankeyMaxLabelWidthRatio = 0.2f;
const float kSankeyMaxLabelMarginRatio = 0.6f;

struct SankeyChartNode {
    Str label = {};

    Str value = {};

    Str note = {};
    Rgba noteColor = {};
    Rgba color = {};
    bool hasColor = false;
};

struct SankeyChart {
    Arena* a = nullptr;
    Ctx* cx = nullptr;

    ArenaVec<SankeyChartNode> nodes;
    ArenaVec<SankeyLink> links;
    float nodeWidth = kSankeyChartNodeWidth;
    float nodePadding = kSankeyChartNodePadding;
    SankeyAlign align = SankeyAlign::Justify;
    int iterations = 6;
    SankeyValueScale valueScale = SankeyValueScale::Linear;
    float nodeRadius = 0;
    float linkOpacity = kSankeyChartLinkOpacity;
    float minLinkWidth = kSankeyChartMinLinkWidth;
    float labelGap = kSankeyChartLabelGap;

    bool showValues = false;

    static SankeyChart* New(Ctx* cx);

    SankeyChart* Node(Str label);
    SankeyChart* NodeColored(Str label, Rgba color);

    SankeyChart* NodeValue(Str text);
    SankeyChart* NodeNote(Str text, Rgba color);
    SankeyChart* Link(int source, int target, double value);
    SankeyChart* NodeWidth(float v);
    SankeyChart* NodePadding(float v);
    SankeyChart* NodeAlign(SankeyAlign v);
    SankeyChart* Iterations(int v);
    SankeyChart* ValueScale(SankeyValueScale v);
    SankeyChart* NodeCornerRadius(float v);
    SankeyChart* LinkOpacity(float v);
    SankeyChart* MinLinkWidth(float v);
    SankeyChart* LabelGap(float v);
    SankeyChart* ShowValues(bool v = true);
    El* IntoEl();
};

void SankeyChartThroughput(const SankeyLink* links, int nLinks, double* out,
                           int n);

}
}

#line 1 "src/ui/checkbox.h"

namespace gpui {

namespace component {

struct Checkbox {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Str label = {};
    Str hint = {};

    El* child = nullptr;
    bool checked = false;
    bool disabled = false;
    UiSize size = UiSize::Medium;
    Str tooltip = {};
    bool focusRing = true;
    float w = 0;
    Listener onClick;

    static Checkbox* New(Ctx* cx, Str id);
    Checkbox* Label(Str s);
    Checkbox* Hint(Str s);
    Checkbox* Child(El* e);
    Checkbox* Checked(bool v);
    Checkbox* Disabled(bool v);
    Checkbox* WithSize(UiSize s);
    Checkbox* W(float v);

    Checkbox* FocusRing(bool v);
    Checkbox* Tooltip(Str s);
    Checkbox* OnClick(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/clipboard.h"

namespace gpui {

namespace component {

struct ClipboardEvent {
    Str value = {};
};

struct ClipboardState {
    bool copied = false;
    int timer = 0;
    Str value = {};
    Listener onCopied = {};

    ~ClipboardState();
    static void OnCopy(ClipboardState* self, Ctx* cx, const ClickEvent* ev);
    static void OnReset(ClipboardState* self, Ctx* cx, const TickEvent* ev);
};

struct Clipboard {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Str value = {};
    Str tooltipText = {};
    Listener onCopied;

    static Clipboard* New(Ctx* cx, Str id);
    Clipboard* Value(Str v);
    Clipboard* Tooltip(Str t);
    Clipboard* OnCopied(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/collapsible.h"

namespace gpui {

namespace component {

struct Collapsible {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    bool open = false;
    El* trigger = nullptr;
    El* content = nullptr;

    float width = 0;
    float gap = 0;

    static Collapsible* New(Ctx* cx);
    Collapsible* W(float v);
    Collapsible* Gap(float v);
    Collapsible* Open(bool v);
    Collapsible* Trigger(El* e);
    Collapsible* Content(El* e);
    El* IntoEl();
};

}
}

#line 1 "src/ui/color_picker.h"

namespace gpui {

namespace component {

Entity<ColorPickerState> ColorPickerStateFor(Ctx* cx, Str id);

struct ColorPicker {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Str label = {};

    IconName icon = IconName::None;
    UiSize size = UiSize::Medium;

    const uint32_t* featured = nullptr;
    int nFeatured = 0;

    Listener onChange;

    static ColorPicker* New(Ctx* cx, Str id);
    ColorPicker* Label(Str s);
    ColorPicker* Icon(IconName v);
    ColorPicker* WithSize(UiSize s);
    ColorPicker* FeaturedColors(const uint32_t* colors, int n);
    ColorPicker* OnChange(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/base/index_path.h"

namespace gpui {

struct IndexPath {
    int section = 0;
    int row = 0;
    int column = 0;

    IndexPath Section(int v) const { return IndexPath{v, row, column}; }
    IndexPath Row(int v) const { return IndexPath{section, v, column}; }
    IndexPath Column(int v) const { return IndexPath{section, row, v}; }

    bool EqRow(IndexPath o) const {
        return section == o.section && row == o.row;
    }
};

inline IndexPath IndexPathNew(int row) {
    return IndexPath{0, row, 0};
}

inline bool operator==(IndexPath a, IndexPath b) {
    return a.section == b.section && a.row == b.row && a.column == b.column;
}
inline bool operator!=(IndexPath a, IndexPath b) {
    return !(a == b);
}

Str IndexPathIdStr(Arena* a, IndexPath p);

uint32_t IndexPathClickId(IndexPath p);

}

#line 1 "src/base/list.h"

namespace gpui {

enum class ListEventKind : uint8_t {

    Select,

    Confirm,

    Cancel
};

struct ListEvent {
    ListEventKind kind = ListEventKind::Select;

    int index = -1;

    bool secondary = false;
};

enum class ListAction : uint8_t {
    None,
    SelectPrev,
    SelectNext,
    Confirm,
    Cancel
};

struct ListKeyAction {
    ListAction action = ListAction::None;
    bool secondary = false;
};

void ListInitKeys();
Str ListContext();

ListKeyAction ListActionOf(uint32_t id, intptr_t arg = 0);

enum class ListRowKind : uint8_t {
    Entry,
    SectionHeader,
    SectionFooter
};

struct ListRow {
    ListRowKind kind = ListRowKind::Entry;
    int section = 0;

    int row = 0;
    int entry = -1;

    IndexPath Path() const { return IndexPath{section, row, 0}; }
};

struct ListState {

    int count = 0;

    int selected = -1;

    int rightClicked = -1;
    bool selectable = true;

    bool resetOnCancel = true;

    Vec<int> sectionCounts;
    bool sectionHeaders = false;
    bool sectionFooters = false;

    float rowH = 32;
    float headerH = 0;
    float footerH = 0;

    IndexPath itemToMeasure = {};

    Vec<float> rowHeights;
    float scrollY = 0;
    float viewportH = 0;

    bool loading = false;
    bool hasMore = false;
    int loadMoreThreshold = 20;
    Listener onEvent = {};
    Listener onLoadMore = {};

    EntityId self = {};

    FocusHandle focus = {};

    ~ListState() {
        sectionCounts.Reset();
        rowHeights.Reset();
    }

    static void OnRowClick(ListState* self, Ctx* cx, const ClickEvent* ev,
                           intptr_t ix);
    static void OnRowMouseDown(ListState* self, Ctx* cx,
                               const MouseDownEvent* ev, intptr_t ix);
    static void OnScroll(ListState* self, Ctx* cx, const ScrollEvent* ev);
};

void ListSetSections(ListState* s, const int* counts, int n, bool headers,
                     bool footers);

void ListSetCount(ListState* s, int count);

int ListRowCount(const ListState* s);

ListRow ListRowAt(const ListState* s, int rowIx);

int ListRowOfEntry(const ListState* s, int entry);

void ListPrepareRowHeights(ListState* s, float itemH, float headerH,
                           float footerH);

const float* ListRowHeights(const ListState* s);

IndexPath ListIndexPathOf(const ListState* s, int entry);
int ListEntryOf(const ListState* s, IndexPath path);

void ListScrollToItem(ListState* s, int entry, ScrollStrategy strategy);

bool ListShouldLoadMore(const ListState* s, int lastVisibleRow);

int ListNextIndex(const ListState* s);
int ListPrevIndex(const ListState* s);

void ListPerform(ListState* s, Ctx* cx, ListAction act, bool secondary);

void ListOnAction(ListState* self, Ctx* cx, const ActionEvent* ev);

void ListBindKeys(Ctx* cx, El* root, Entity<ListState> state);

void ListClickRow(ListState* s, Ctx* cx, int ix, bool secondary);

void ListRightClickRow(ListState* s, Ctx* cx, int ix);

}

#line 1 "src/ui/list.h"

namespace gpui {

namespace component {

El* ListLoadingView(Ctx* cx, float h = 0);

struct ListItem {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    El* child = nullptr;
    bool selected = false;
    bool secondarySelected = false;
    bool confirmed = false;
    bool disabled = false;

    StateStyle style = {};

    static ListItem* New(Ctx* cx, El* child);
    ListItem* Selected(bool v);
    ListItem* SecondarySelected(bool v);
    ListItem* Confirmed(bool v);
    ListItem* Disabled(bool v);
    ListItem* Style(const StateStyle& s);
    El* IntoEl(Str id, Listener onClick, Listener onMouseDown);
};

ListItem* ListSeparatorItem(Ctx* cx, El* child = nullptr);

struct List {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Entity<ListState> state = {};

    void* data = nullptr;
    ListItem* (*item)(Ctx* cx, void* data, int section, int row,
                      int entry) = nullptr;
    El* (*header)(Ctx* cx, void* data, int section) = nullptr;
    El* (*footer)(Ctx* cx, void* data, int section) = nullptr;

    InputState* search = nullptr;
    Listener onSearchFocus;

    El* loading = nullptr;
    El* initial = nullptr;

    El* empty = nullptr;
    float h = 320;

    static List* New(Ctx* cx, Str id, Entity<ListState> state);

    List* Sections(const int* counts, int n);
    List* Count(int n);
    List* Items(void* data,
                ListItem* (*fn)(Ctx*, void*, int section, int row, int entry));
    List* Headers(El* (*headerFn)(Ctx*, void*, int),
                  El* (*footerFn)(Ctx*, void*, int) = nullptr);
    List* Searchable(InputState* search, Listener onFocus);
    List* Loading(El* e);
    List* Initial(El* e);
    List* Empty(El* e);
    List* H(float px);
    El* IntoEl();
};

}
}

#line 1 "src/ui/searchable_list.h"

namespace gpui {

namespace component {

struct SearchableItem {
    Str title = {};
    Str value = {};

    int section = 0;
    bool disabled = false;

    IconName icon = IconName::None;

    bool pinned = false;

    Str badge = {};

    Str display = {};
};

enum class SearchableListMode : uint8_t {
    Single,
    Multi
};

enum class SearchableListChangeKind : uint8_t {
    Select,
    Deselect
};

struct SearchableListChange {
    SearchableListChangeKind kind = SearchableListChangeKind::Select;
    int index = 0;
};

bool SearchableItemMatches(const SearchableItem* it, Str query);

struct SearchableListState {

    ListState list;
    SearchableListMode mode = SearchableListMode::Single;

    Vec<int> selected;
    bool open = false;

    bool closeOnSelect = true;

    Vec<int> matches;

    const SearchableItem* items = nullptr;
    int nItems = 0;

    int maxSelected = 0;

    Listener onChange = {};

    FocusHandle triggerFocus = {};
    FocusHandle contentFocus = {};
    FocusHandle previousFocus = {};

    static void OnRowClick(SearchableListState* self, Ctx* cx,
                           const ClickEvent* ev, intptr_t match);

    static void OnAction(SearchableListState* self, Ctx* cx,
                         const ActionEvent* ev);

    static void OnListAction(SearchableListState* self, Ctx* cx,
                             const ActionEvent* ev);

    ~SearchableListState() {
        selected.Reset();
        matches.Reset();
    }
};

void SearchableListSelectOnly(SearchableListState* s, int index);

void SearchableListChangesFor(const SearchableListState* s,
                              const SearchableItem* items, int nItems,
                              int index, Vec<SearchableListChange>* out);

void SearchableListApply(SearchableListState* s, const SearchableItem* items,
                         int nItems, const SearchableListChange* changes,
                         int n);

bool SearchableListIsChecked(const SearchableListState* s,
                             const SearchableItem* items, int nItems,
                             int index);

bool SearchableListIsEnabled(const SearchableListState* s,
                             const SearchableItem* items, int nItems,
                             int index);

bool SearchableListClick(SearchableListState* s, int index);

void SearchableListSearch(SearchableListState* s, const SearchableItem* items,
                          int nItems, Str query);

struct SearchableList {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Entity<SearchableListState> state = {};
    const SearchableItem* items = nullptr;
    int nItems = 0;

    const Str* sections = nullptr;
    int nSections = 0;
    InputState* query = nullptr;
    Listener onQueryFocus = {};
    El* empty = nullptr;

    El* footer = nullptr;
    float w = 240;
    float maxH = 320;

    IconName checkIcon = IconName::Check;

    bool inSelect = false;

    static SearchableList* New(Ctx* cx, Str id, Entity<SearchableListState> st,
                               InputState* query);
    SearchableList* Items(const SearchableItem* items, int n);
    SearchableList* Sections(const Str* titles, int n);
    SearchableList* OnQueryFocus(Listener fn);
    SearchableList* Empty(El* e);
    SearchableList* Footer(El* e);
    SearchableList* W(float v);
    SearchableList* MaxH(float v);
    SearchableList* CheckIcon(IconName n);
    SearchableList* InSelect(bool v);
    El* IntoEl();
};

}
}

#line 1 "src/ui/select.h"

namespace gpui {

namespace component {

struct Select {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Entity<SearchableListState> state = {};

    const SearchableItem* items = nullptr;
    int nItems = 0;
    const Str* sections = nullptr;
    int nSections = 0;
    Str placeholder = {};
    Str titlePrefix = {};
    Str empty = {};
    float width = kFill;
    float menuWidth = 0;
    float menuMaxH = 0;
    UiSize size = UiSize::Medium;
    IconName icon = IconName::None;

    IconName checkIcon = IconName::Check;
    bool disabled = false;
    bool cleanable = false;
    bool appearance = true;
    bool focusRing = true;

    InputState* query = nullptr;
    Listener onQueryFocus = {};

    El* trigger = nullptr;

    El* footer = nullptr;
    Listener onToggle;
    Listener onClear;

    static Select* New(Ctx* cx, Str id, Entity<SearchableListState> state);
    Select* Items(const SearchableItem* items, int n);
    Select* Sections(const Str* titles, int n);
    Select* Placeholder(Str s);
    Select* TitlePrefix(Str s);
    Select* Empty(Str s);
    Select* W(float v);
    Select* MenuWidth(float v);
    Select* MenuMaxH(float v);
    Select* WithSize(UiSize s);
    Select* Icon(IconName n);
    Select* CheckIcon(IconName n);
    Select* Disabled(bool v);
    Select* Cleanable(bool v = true);
    Select* Appearance(bool v);

    Select* FocusRing(bool v);
    Select* Searchable(InputState* query, Listener onFocus);
    Select* Trigger(El* e);
    Select* Footer(El* e);

    Select* Multiple(bool v = true);
    Select* OnToggle(Listener fn);
    Select* OnClear(Listener fn);
    El* IntoEl();
};

Str SelectTriggerTitle(const SearchableListState* s, Str placeholder,
                       Str titlePrefix, Arena* a);

void SelectToggleOpen(SearchableListState* s, Ctx* cx);

void SelectBindKeys(Ctx* cx, El* root, Entity<SearchableListState> state);
void SelectClear(SearchableListState* s, Ctx* cx);

}
}

#line 1 "src/ui/combobox.h"

namespace gpui {

namespace component {

struct Combobox {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Entity<SearchableListState> state = {};
    const SearchableItem* items = nullptr;
    int nItems = 0;
    const Str* sections = nullptr;
    int nSections = 0;
    Str placeholder = {};
    Str searchPlaceholder = {};
    Str empty = {};

    IconName icon = IconName::None;

    IconName checkIcon = IconName::Check;
    float width = 280;
    float menuMaxH = 0;
    bool disabled = false;
    bool cleanable = false;
    bool focusRing = true;
    InputState* query = nullptr;
    Listener onQueryFocus = {};

    El* trigger = nullptr;
    El* footer = nullptr;
    Listener onToggle;
    Listener onClear;

    static Combobox* New(Ctx* cx, Str id, Entity<SearchableListState> state,
                         InputState* query);
    Combobox* Items(const SearchableItem* items, int n);
    Combobox* Sections(const Str* titles, int n);
    Combobox* Placeholder(Str s);
    Combobox* SearchPlaceholder(Str s);
    Combobox* Empty(Str s);
    Combobox* Icon(IconName n);
    Combobox* CheckIcon(IconName n);
    Combobox* W(float v);
    Combobox* MenuMaxH(float v);
    Combobox* Disabled(bool v);
    Combobox* Cleanable(bool v = true);

    Combobox* FocusRing(bool v);
    Combobox* Multiple(bool v = true);
    Combobox* Trigger(El* e);
    Combobox* Footer(El* e);

    Combobox* MaxSelected(int n);
    Combobox* OnQueryFocus(Listener fn);
    Combobox* OnToggle(Listener fn);
    Combobox* OnClear(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/kbd.h"

namespace gpui {

namespace component {

struct Keystroke {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;

    bool platform = false;
    Str key = {};
};

int KbdFormat(Keystroke stroke, char* out, int cap);

Str KbdFormatStr(Ctx* cx, Keystroke stroke);

bool KeystrokeForAction(uint32_t action, const char* context, Keystroke* out);

struct Kbd {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str stroke = {};
    bool appearance = true;
    bool outline = false;

    static Kbd* New(Ctx* cx, Str stroke);

    static Kbd* New(Ctx* cx, Keystroke stroke);

    static Kbd* ForAction(Ctx* cx, uint32_t action,
                          const char* context = nullptr);
    Kbd* Appearance(bool v);
    Kbd* Outline();
    El* IntoEl();
};

}
}

#line 1 "src/ui/command.h"

namespace gpui {

namespace component {

struct CommandItem {
    Str label = {};

    const Str* keywords = nullptr;
    int nKeywords = 0;
    IconName icon = IconName::None;

    uint32_t action = 0;
    intptr_t actionArg = 0;

    const char* actionContext = nullptr;

    bool checked = false;
    bool disabled = false;

    El* (*content)(Ctx* cx, const CommandItem* item) = nullptr;

    float contentH = 0;

    intptr_t data = 0;
};

struct CommandGroup {
    Str heading = {};
    const CommandItem* items = nullptr;
    int nItems = 0;
};

enum class CommandEntryKind : uint8_t {
    Item,
    Group,

    Separator
};

struct CommandEntry {
    CommandEntryKind kind = CommandEntryKind::Item;
    CommandItem item = {};
    CommandGroup group = {};
};

CommandEntry CommandEntryOf(const CommandItem& item);
CommandEntry CommandEntryOf(const CommandGroup& group);
CommandEntry CommandSeparatorEntry();

bool CommandItemMatches(const CommandItem* item, Str query);

enum class CommandRowKind : uint8_t {
    Heading,
    Item,
    Separator
};

struct CommandRow {
    CommandRowKind kind = CommandRowKind::Item;
    Str heading = {};

    int match = 0;
};

struct CommandMatch {
    int entry = 0;
    int itemIx = 0;

    IndexPath path = {};
    int row = 0;
    bool disabled = false;
    intptr_t data = 0;
};

enum class CommandEventKind : uint8_t {
    Query,
    Select,
    Confirm,
    Cancel
};

struct CommandEvent {
    CommandEventKind kind = CommandEventKind::Select;
    IndexPath path = {};
    Str query = {};
    intptr_t data = 0;
};

Str CommandContext();
void CommandInitKeys();

struct CommandState {

    InputState query;
    VirtualListScrollHandle scroll = {};

    const CommandEntry* entries = nullptr;
    int nEntries = 0;
    bool searchable = true;

    bool filterable = true;
    Vec<CommandRow> rows;
    Vec<CommandMatch> matched;
    Vec<float> rowSizes;

    int selected = -1;

    bool preserveNoSelection = false;
    bool loading = false;

    int pendingScroll = -1;

    Vec<char> applied;
    Listener onQuery = {};
    Listener onSelect = {};
    Listener onConfirm = {};
    Listener onCancel = {};

    static void OnRowClick(CommandState* self, Ctx* cx, const ClickEvent* ev,
                           intptr_t match);
    static void OnRowHover(CommandState* self, Ctx* cx, const HoverEvent* ev,
                           intptr_t match);
    static void OnAction(CommandState* self, Ctx* cx, const ActionEvent* ev);

    ~CommandState() {
        rows.Reset();
        matched.Reset();
        rowSizes.Reset();
        applied.Reset();
    }
};

void CommandInstall(CommandState* s, Ctx* cx, const CommandEntry* entries,
                    int nEntries, bool searchable, bool filterable = true);

bool CommandSelectedIndex(const CommandState* s, IndexPath* out);

void CommandSetSelectedIndex(CommandState* s, Ctx* cx, const IndexPath* path);

void CommandSelectBy(CommandState* s, Ctx* cx, int step);

int CommandMatchedCount(const CommandState* s);

void CommandSetQuery(CommandState* s, Ctx* cx, Str query);

void CommandSetLoading(CommandState* s, Ctx* cx, bool loading);

struct Command {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Entity<CommandState> state = {};
    const CommandEntry* entries = nullptr;
    int nEntries = 0;
    bool searchable = true;
    bool filterable = true;
    Str placeholder = {};

    El* empty = nullptr;
    El* header = nullptr;
    El* footer = nullptr;
    float maxH = 300;
    bool bordered = true;
    float w = kFill;
    Listener onQuery = {};
    Listener onSelect = {};
    Listener onConfirm = {};
    Listener onCancel = {};

    static Command* New(Ctx* cx, Str id, Entity<CommandState> state);
    Command* Entries(const CommandEntry* entries, int n);

    Command* Items(const CommandItem* items, int n);
    Command* Searchable(bool v);

    Command* Filterable(bool v);
    Command* Placeholder(Str s);
    Command* Empty(El* e);
    Command* Header(El* e);
    Command* Footer(El* e);
    Command* MaxH(float v);
    Command* Bordered(bool v);
    Command* W(float v);
    Command* OnQuery(Listener fn);
    Command* OnSelect(Listener fn);
    Command* OnConfirm(Listener fn);
    Command* OnCancel(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/description_list.h"

namespace gpui {

namespace component {

struct DescriptionItem {
    Str label = {};
    El* value = nullptr;
    int span = 1;
    bool separator = false;
};

struct DescriptionList {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    DescriptionItem items[16] = {};
    int n = 0;
    int columns = 3;
    float labelWidth = 120;
    bool bordered = true;

    bool vertical = false;
    UiSize size = UiSize::Medium;

    static DescriptionList* New(Ctx* cx);

    DescriptionList* Item(Str label, Str value, int span = 1);
    DescriptionList* ItemEl(Str label, El* value, int span = 1);
    DescriptionList* Separator();
    DescriptionList* Columns(int n);
    DescriptionList* LabelWidth(float w);
    DescriptionList* Bordered(bool v);
    DescriptionList* Vertical(bool v = true);
    DescriptionList* WithSize(UiSize s);
    El* IntoEl();
};

}
}

#line 1 "src/ui/dialog.h"

namespace gpui {

namespace component {

struct Dialog {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str title = {};
    Str description = {};
    bool open = false;
    El* body = nullptr;

    El* surface = nullptr;

    Listener onClose;
    Listener onCancel;
    Listener onOk;

    float width = 448;
    float height = 0;

    bool overlay = true;
    bool overlayClosable = true;

    bool keyboard = true;

    int layerIx = 0;
    float radius = 0;
    Background background = {};
    Rgba foreground = {};
    bool hasBackground = false;
    bool hasForeground = false;

    IconName icon = IconName::None;
    Rgba iconColor = {};
    bool hasIconColor = false;
    float iconSize = 16;
    bool headerCentered = false;

    Str okText = {};
    Str cancelText = {};
    ButtonVariant okVariant = ButtonVariant::Primary;
    bool okOutline = false;

    bool showCancel = false;

    bool closeButton = false;

    El* footer = nullptr;
    bool footerVertical = false;

    bool footerStretch = false;
    bool footerMuted = false;
    bool footerDivider = false;

    static Dialog* New(Ctx* cx);
    Dialog* Title(Str s);
    Dialog* Description(Str s);
    Dialog* Open(bool v);
    Dialog* Body(El* e);
    Dialog* Surface(El* e);
    Dialog* W(float px);
    Dialog* H(float px);
    Dialog* Overlay(bool v);
    Dialog* OverlayClosable(bool v);
    Dialog* Keyboard(bool v);
    Dialog* Layer(int ix);
    Dialog* Radius(float px);
    Dialog* Bg(Background color);
    Dialog* Fg(Rgba color);
    Dialog* Icon(IconName n, Rgba color, float size = 16);
    Dialog* HeaderCentered(bool v = true);
    Dialog* OkText(Str s);
    Dialog* CancelText(Str s);
    Dialog* OkVariant(ButtonVariant v, bool outline = false);
    Dialog* ShowCancel(bool v);

    Dialog* Confirm();
    Dialog* CloseButton(bool v = true);
    Dialog* Footer(El* e);
    Dialog* FooterVertical(bool v = true);
    Dialog* FooterStretch(bool v = true);
    Dialog* FooterMuted(bool v = true);
    Dialog* FooterDivider(bool v = true);
    Dialog* OnClose(Listener fn);
    Dialog* OnCancel(Listener fn);
    Dialog* OnOk(Listener fn);
    El* IntoEl(WinSize size);

  private:

    Str LayerId(Str base) const;
    El* Header();
    El* Actions();
};

}
}

#line 1 "src/ui/dock.h"

namespace gpui {

namespace component {

El* DockInvalidPanelRender(Ctx* cx, void* data);

const float kDockTabBarH = 30;

struct DockArea {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Entity<DockState> state = {};

    static DockArea* New(Ctx* cx, Str id, Entity<DockState> state);
    El* IntoEl();
};

}
}

#line 1 "src/ui/tiles.h"

namespace gpui {

namespace component {

struct TilePanelDef {
    Str title = {};
    El* content = nullptr;

    El* suffix = nullptr;
};

struct Tiles {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Entity<TilesState> state = {};

    ArenaVec<TilePanelDef> panels;

    static Tiles* New(Ctx* cx, Str id, Entity<TilesState> state);

    Tiles* Panel(Str title, El* content, El* suffix = nullptr);
    El* IntoEl();
};

}
}

#line 1 "src/ui/inspector.h"

namespace gpui {

namespace component {

const float kInspectorWidth = 320;

Str StyleToJson(Arena* a, const Style& style);
bool StyleFromJson(Arena* a, Str text, Style* style, uint32_t* fields,
                   Str* error);

struct Inspector {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    float width = kInspectorWidth;

    static Inspector* New(Ctx* cx);
    Inspector* W(float v);

    El* IntoEl();
};

}
}

#line 1 "src/ui/form.h"

namespace gpui {

namespace component {

enum class FieldAlign : uint8_t {
    Center,
    Start,
    End
};

struct FormField {
    Str label = {};

    El* labelEl = nullptr;
    El* control = nullptr;

    Str description = {};
    El* descriptionEl = nullptr;
    bool required = false;

    bool spanAll = false;

    bool visible = true;

    bool labelIndent = true;
    FieldAlign align = FieldAlign::Center;
};

struct Form {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    FormField fields[12] = {};
    int n = 0;
    bool horizontal = false;
    int columns = 1;
    float labelWidth = 0;

    UiSize size = UiSize::Medium;

    float labelTextSize = 0;

    static Form* New(Ctx* cx);

    Form* Field(Str label, El* control);

    Form* FieldEl(El* label, El* control);
    Form* Required(bool v = true);
    Form* Description(Str s);
    Form* DescriptionEl(El* e);
    Form* SpanAll(bool v = true);
    Form* Visible(bool v);
    Form* LabelIndent(bool v);
    Form* Align(FieldAlign v);
    Form* Horizontal(bool v = true);
    Form* Columns(int n);
    Form* LabelWidth(float w);
    Form* WithSize(UiSize v);
    Form* LabelTextSize(float px);
    El* IntoEl();
};

}
}

#line 1 "src/ui/group_box.h"

namespace gpui {

namespace component {

struct GroupBox {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str title = {};

    El* titleEl = nullptr;
    El* child = nullptr;

    bool outline = false;
    bool filled = false;

    bool titleSemibold = false;
    float titlePadX = 0;
    Background contentBg = {};
    bool hasContentBg = false;
    float contentRadius = -1;
    float contentPad = -1;
    float contentBorder = -1;

    static GroupBox* New(Ctx* cx, Str title);
    GroupBox* Title(El* e);
    GroupBox* Child(El* e);
    GroupBox* Outline();
    GroupBox* Filled(bool v);
    GroupBox* TitleSemibold(bool v = true);
    GroupBox* TitlePadX(float px);
    GroupBox* ContentBg(Background c);
    GroupBox* ContentRadius(float px);
    GroupBox* ContentPad(float px);
    GroupBox* ContentBorder(float px);
    El* IntoEl();
};

}
}

#line 1 "src/ui/syntax.h"

namespace gpui {

namespace component {

enum class SyntaxTok : uint8_t {
    Text,
    Keyword,
    Type,
    Function,
    Property,
    String,
    Number,
    Boolean,
    Comment,
    Tag,
    Attribute,
};

using SyntaxLang = int8_t;
constexpr SyntaxLang SyntaxLangNone = -1;

SyntaxLang SyntaxLangFor(Str info);

Str SyntaxLangName(SyntaxLang lang);

struct SyntaxLexer {
    const void* def = nullptr;
    Str src = {};
    int at = 0;
    SyntaxTok tok = SyntaxTok::Text;
    Str text = {};

    bool inTag = false;

    bool tagName = false;

    bool inFence = false;
    bool linkDest = false;
};

void SyntaxLexStart(SyntaxLexer* lx, SyntaxLang lang, Str src);

bool SyntaxLexNext(SyntaxLexer* lx);

Rgba SyntaxTokColor(SyntaxTok tok, ThemeMode mode, Rgba fallback);

}
}

#line 1 "src/ui/highlighter.h"

namespace gpui {

namespace component {

struct Highlighter {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};

    InputState* state = nullptr;

    float h = 0;

    float fontSize = 0;

    SyntaxLang lang = SyntaxLangNone;

    const TextSpan* decorations = nullptr;
    int nDecorations = 0;

    bool activeLine = false;
    bool indentGuides = false;

    bool searchable = true;

    bool folding = false;
    const Diagnostic* diagnostics = nullptr;
    int nDiagnostics = 0;

    static Highlighter* New(Ctx* cx, InputState* state);
    static Highlighter* New(Ctx* cx, Str id, InputState* state);
    Highlighter* H(float v);

    Highlighter* Font(float px);
    Highlighter* Language(Str name);
    Highlighter* Decorations(const TextSpan* runs, int n);
    Highlighter* ActiveLine(bool v = true);
    Highlighter* IndentGuides(bool v = true);
    Highlighter* Searchable(bool v);

    Highlighter* Diagnostics(const Diagnostic* items, int n);
    Highlighter* Folding(bool v = true);
    El* IntoEl();
};

}
}

#line 1 "src/ui/hover_card.h"

namespace gpui {

namespace component {

using HoverCardAnchor = PopupAnchor;

bool HoverCardOpen(Ctx* cx, Str id);

struct HoverCard {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    El* trigger = nullptr;
    El* content = nullptr;

    bool controlled = false;
    bool open = false;
    int openDelayMs = 600;
    int closeDelayMs = 300;

    HoverCardAnchor anchor = HoverCardAnchor::TopCenter;

    static HoverCard* New(Ctx* cx);
    static HoverCard* New(Ctx* cx, Str id);
    HoverCard* Trigger(El* e);
    HoverCard* Content(El* e);
    HoverCard* Open(bool v);
    HoverCard* OpenDelay(int ms);
    HoverCard* CloseDelay(int ms);
    HoverCard* Anchor(HoverCardAnchor a);
    El* IntoEl();
};

}
}

#line 1 "src/ui/i18n.h"

namespace gpui {

namespace component {

struct LocaleRow {
    const char* key;
    const char* const* values;
};

Str Tr(const char* key);

bool LocaleSet(Str name);

Str LocaleNow();

int LocaleCount();
Str LocaleAt(int i);

int LocaleIndex(Str name);

int LocaleRowCount();
const LocaleRow* LocaleRowAt(int i);

}
}

#line 1 "src/ui/icon.h"

namespace gpui {

namespace component {

struct Icon {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    IconName name = IconName::None;
    float size = 16;
    Rgba color = {};
    bool hasColor = false;

    static Icon* New(Ctx* cx, IconName name);
    Icon* Size(float v);
    Icon* Color(Rgba c);
    El* IntoEl();
};

}
}

#line 1 "src/ui/input.h"

namespace gpui {

namespace component {

enum class InputAlign : uint8_t {
    Left,
    Center,
    Right
};

struct Input {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    InputState* state = nullptr;
    Str label = {};
    float width = kFill;
    El* prefix = nullptr;
    El* suffix = nullptr;
    UiSize size = UiSize::Medium;
    InputAlign align = InputAlign::Left;
    bool disabled = false;
    bool cleanable = false;

    bool masked = false;
    bool maskToggle = false;
    bool appearance = true;
    bool focusRing = true;
    Rgba textColor = {};
    bool hasTextColor = false;
    Listener onChange;
    Listener onFocus;
    Listener onClear;
    Listener onToggleMask;

    static Input* New(Ctx* cx, Str id, InputState* state);
    Input* Label(Str s);
    Input* WithSize(UiSize s);
    Input* Align(InputAlign v);
    Input* Disabled(bool v);
    Input* Cleanable(bool v = true);
    Input* Masked(bool v);
    Input* MaskToggle(bool v = true);
    Input* Appearance(bool v);

    Input* FocusRing(bool v);
    Input* TextColor(Rgba c);
    Input* OnClear(Listener fn);
    Input* OnToggleMask(Listener fn);

    Input* Prefix(El* el);
    Input* Suffix(El* el);

    Input* W(float v);
    Input* OnChange(Listener fn);
    Input* OnFocus(Listener fn);
    El* IntoEl();
};

struct SearchPanel {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    InputState* target = nullptr;

    static SearchPanel* New(Ctx* cx, Str id, InputState* target);
    El* IntoEl();
};

struct Textarea {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};

    InputState* state = nullptr;
    int rows = 0;

    float height = 0;
    bool softWrap = true;
    Listener onFocus;

    static Textarea* New(Ctx* cx, Str id, InputState* state);

    Textarea* Rows(int n);
    Textarea* H(float px);
    Textarea* SoftWrap(bool v);
    Textarea* OnFocus(Listener fn);
    El* IntoEl();
};

struct NumberInput {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    InputState* state = nullptr;
    float width = kFill;
    UiSize size = UiSize::Medium;
    bool disabled = false;
    bool appearance = true;
    bool focusRing = true;
    El* suffix = nullptr;
    Background bg = {};
    bool hasBg = false;
    Rgba textColor = {};
    bool hasTextColor = false;
    Listener onInc;
    Listener onDec;
    Listener onFocus;

    static NumberInput* New(Ctx* cx, InputState* state);
    static NumberInput* New(Ctx* cx, Str id, InputState* state);

    NumberInput* W(float v);
    NumberInput* WithSize(UiSize s);
    NumberInput* Disabled(bool v);
    NumberInput* Appearance(bool v);

    NumberInput* FocusRing(bool v);
    NumberInput* Suffix(El* el);
    NumberInput* Bg(Background c);
    NumberInput* TextColor(Rgba c);
    NumberInput* OnFocus(Listener fn);
    NumberInput* OnInc(Listener fn);
    NumberInput* OnDec(Listener fn);
    El* IntoEl();
};

struct OtpInput {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    const char* value = nullptr;
    int len = 0;
    int slots = 6;

    int groups = 2;
    bool masked = false;
    bool disabled = false;
    bool focusRing = true;
    UiSize size = UiSize::Medium;
    float cellPx = 0;
    Listener onFocus;

    Entity<OtpState> state = {};

    static OtpInput* New(Ctx* cx, const char* value, int len);
    static OtpInput* New(Ctx* cx, Str id, Entity<OtpState> state);
    OtpInput* Id(Str s);
    OtpInput* Slots(int n);
    OtpInput* Groups(int n);
    OtpInput* Masked(bool v);
    OtpInput* Disabled(bool v);

    OtpInput* FocusRing(bool v);
    OtpInput* WithSize(UiSize s);
    OtpInput* CellSize(float px);
    OtpInput* OnFocus(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/label.h"

namespace gpui {

namespace component {

struct Label {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str text = {};
    Str secondary = {};
    bool masked = false;
    bool semibold = false;
    float font = 14;

    Str highlights = {};
    bool prefixMatch = false;

    int align = 0;
    float lineHeight = 0;

    static Label* New(Ctx* cx, Str text);
    Label* Secondary(Str s);
    Label* Masked(bool v);
    Label* Semibold();
    Label* Font(float px);
    Label* Highlights(Str matched, bool prefix = false);
    Label* TextCenter();
    Label* TextRight();
    Label* LineHeight(float mult);
    El* IntoEl();
};

}
}

#line 1 "src/ui/link.h"

namespace gpui {

namespace component {

struct Link {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Str href = {};
    Str text = {};
    bool disabled = false;
    Listener onOpen;

    static Link* New(Ctx* cx, Str id);
    Link* Href(Str s);
    Link* Text(Str s);
    Link* Disabled(bool v);
    Link* OnOpen(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/base/popup_menu.h"

namespace gpui {

enum class PopupMenuAction : uint8_t {
    None,
    SelectPrev,
    SelectNext,
    Confirm,
    Cancel,

    OpenSubmenu,
    CloseSubmenu
};

void PopupMenuInitKeys();

Str PopupMenuContext();

PopupMenuAction PopupMenuActionOf(uint32_t id, Side side);

int PopupMenuNextIndex(const bool* clickable, int n, int selected);
int PopupMenuPrevIndex(const bool* clickable, int n, int selected);

struct PopupMenuRow {
    bool clickable = false;
    bool submenu = false;
    bool link = false;
    Str href = {};
};

struct PopupMenuState {
    bool open = false;

    int selected = -1;

    int openSubmenu = -1;

    Side side = Side::Right;

    float x = 0;
    float y = 0;

    float scrollY = 0;

    FocusHandle focus = {};
    FocusHandle previousFocus = {};

    Entity<PopupMenuState> parent = {};

    Bounds bounds = {};

    Listener onConfirm = {};

    Vec<PopupMenuRow> rows;

    static void OnAction(PopupMenuState* self, Ctx* cx, const ActionEvent* ev);

    static void OnItemClick(PopupMenuState* self, Ctx* cx, const ClickEvent* ev,
                            intptr_t ix);
    static void OnItemHover(PopupMenuState* self, Ctx* cx, const HoverEvent* ev,
                            intptr_t ix);
    static void OnSubmenuClick(PopupMenuState* self, Ctx* cx,
                               const ClickEvent* ev, intptr_t ix);
    static void OnSubmenuHover(PopupMenuState* self, Ctx* cx,
                               const HoverEvent* ev, intptr_t ix);
    static void OnScroll(PopupMenuState* self, Ctx* cx, const ScrollEvent* ev);

    static void OnTriggerClick(PopupMenuState* self, Ctx* cx,
                               const ClickEvent* ev, intptr_t wasOpen);

    static void OnPressOutside(PopupMenuState* self, Ctx* cx,
                               const MouseUpEvent* ev);
    static void OnContextDown(PopupMenuState* self, Ctx* cx,
                              const MouseDownEvent* ev);

    ~PopupMenuState() { rows.Reset(); }
};

void PopupMenuOpen(PopupMenuState* s, Ctx* cx);
void PopupMenuDismiss(PopupMenuState* s, Ctx* cx);
void PopupMenuDismissAll(PopupMenuState* s, Ctx* cx);

void PopupMenuPerform(PopupMenuState* s, Ctx* cx, PopupMenuAction act,
                      const bool* clickable, const bool* hasSubmenu, int n);

void PopupMenuPerformRows(PopupMenuState* s, Ctx* cx, PopupMenuAction act);

void PopupMenuBeginRows(PopupMenuState* s);
void PopupMenuAddRow(PopupMenuState* s, const PopupMenuRow& row);

void PopupMenuConfirm(PopupMenuState* s, Ctx* cx, int ix);

}

#line 1 "src/ui/menu.h"

namespace gpui {

namespace component {

enum class MenuItemKind : uint8_t {
    Item,
    Separator,
    Label
};

struct PopupMenu;

struct MenuItem {
    MenuItemKind kind = MenuItemKind::Item;
    Str label = {};
    IconName icon = IconName::None;

    Str kbd = {};

    uint32_t action = 0;
    intptr_t actionArg = 0;
    bool checked = false;
    bool disabled = false;
    bool isLink = false;
    Str href = {};

    PopupMenu* submenu = nullptr;

    El* element = nullptr;
};

struct PopupMenu {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Entity<PopupMenuState> state = {};

    ArenaVec<MenuItem> items;
    UiSize size = UiSize::Medium;
    float minW = 128;
    float maxH = 450;
    bool scrollable = false;
    bool externalLinkIcon = true;

    Side checkSide = Side::Left;

    const char* actionContext = nullptr;

    static PopupMenu* New(Ctx* cx, Str id);
    static PopupMenu* New(Ctx* cx, Str id, Entity<PopupMenuState> state);
    PopupMenu* Menu(Str label, IconName icon = IconName::None);
    PopupMenu* MenuWithCheck(Str label, bool checked);
    PopupMenu* MenuWithKbd(Str label, Str kbd);

    PopupMenu* MenuWithAction(Str label, uint32_t action, intptr_t arg = 0);

    PopupMenu* Action(uint32_t action, intptr_t arg = 0);
    PopupMenu* Link(Str label, Str href, IconName icon = IconName::None);
    PopupMenu* Separator();
    PopupMenu* Label(Str label);
    PopupMenu* Element(El* el);
    PopupMenu* Submenu(Str label, PopupMenu* menu);

    PopupMenu* Disabled(bool v);
    PopupMenu* Checked(bool v);
    PopupMenu* Icon(IconName v);
    PopupMenu* Kbd(Str v);
    PopupMenu* WithSize(UiSize s);
    PopupMenu* MinW(float v);
    PopupMenu* MaxH(float v);
    PopupMenu* Scrollable(bool v = true);
    PopupMenu* CheckSide(Side s);
    PopupMenu* ActionContext(const char* ctx);
    PopupMenu* ExternalLinkIcon(bool v);
    El* IntoEl();
};

Entity<PopupMenuState> PopupMenuStateFor(Ctx* cx, Str id);

struct DropdownMenu {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    El* trigger = nullptr;
    PopupMenu* menu = nullptr;

    bool anchorRight = false;
    float gap = 4;

    static DropdownMenu* New(Ctx* cx, Str id);
    DropdownMenu* Trigger(El* e);
    DropdownMenu* Menu(PopupMenu* m);
    DropdownMenu* AnchorRight(bool v = true);
    El* IntoEl();
};

struct ContextMenu {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    El* child = nullptr;
    PopupMenu* menu = nullptr;

    static ContextMenu* New(Ctx* cx, Str id);
    ContextMenu* Child(El* e);
    ContextMenu* Menu(PopupMenu* m);
    El* IntoEl();
};

struct AppMenuBarState {
    int selected = -1;

    static void OnMenuClick(AppMenuBarState* self, Ctx* cx,
                            const ClickEvent* ev, intptr_t ix);

    static void OnMenuHover(AppMenuBarState* self, Ctx* cx,
                            const HoverEvent* ev, intptr_t ix);
};

int AppMenuBarNextIndex(int selected, int count);
int AppMenuBarPrevIndex(int selected, int count);

void AppMenuBarSelect(AppMenuBarState* s, Ctx* cx, int ix);

struct AppMenuBar {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Entity<AppMenuBarState> state = {};
    Str titles[12] = {};
    PopupMenu* menus[12] = {};
    int n = 0;

    static AppMenuBar* New(Ctx* cx, Str id, Entity<AppMenuBarState> state);
    AppMenuBar* Menu(Str title, PopupMenu* menu);
    El* IntoEl();
};

}
}

#line 1 "src/ui/native_menu.h"

namespace gpui {

namespace component {

enum class NativeMenuItemKind : uint8_t {
    Item,
    Separator,
    Submenu
};

struct NativeMenu;

struct NativeMenuItem {
    NativeMenuItemKind kind = NativeMenuItemKind::Item;
    Str label = {};
    bool disabled = false;
    bool checked = false;

    IconName icon = IconName::None;

    intptr_t id = 0;
    NativeMenu* submenu = nullptr;
};

struct NativeMenu {
    Arena* a = nullptr;
    Ctx* cx = nullptr;

    ArenaVec<NativeMenuItem> items;

    Listener onSelect = {};

    static NativeMenu* New(Ctx* cx);
    NativeMenu* Menu(Str label, intptr_t id);
    NativeMenu* MenuWithDisabled(Str label, bool disabled, intptr_t id);
    NativeMenu* MenuWithCheck(Str label, bool checked, intptr_t id);
    NativeMenu* MenuWithIcon(Str label, IconName icon, intptr_t id);
    NativeMenu* Separator();
    NativeMenu* Submenu(Str label, NativeMenu* menu);
    NativeMenu* OnSelect(Listener l);
    bool IsEmpty() const { return items.len == 0; }

    bool Show(float x, float y);

    PopupMenu* IntoPopupMenu(Str id) const;
};

int NativeMenuSelectable(const NativeMenu* m, const NativeMenuItem** out,
                         int cap);

}
}

#line 1 "src/ui/notification.h"

namespace gpui {

namespace component {

enum class NotificationKind : uint8_t {

    None,
    Info,
    Success,
    Warning,
    Error
};

enum class NotificationAnchor : uint8_t {
    None,
    TopLeft,
    TopCenter,
    TopRight,
    LeftCenter,
    RightCenter,
    BottomLeft,
    BottomCenter,
    BottomRight
};

enum class NotificationDelivery : uint8_t {
    InApp,
    System,
    InAppAndSystem
};

bool NotificationDeliveryIncludesInApp(NotificationDelivery d);
bool NotificationDeliveryIncludesSystem(NotificationDelivery d);

const float kNotificationMargin = 16;

const float kNotificationWidth = 382;

const int kNotificationMaxItems = 10;

const int kNotificationTickMs = 50;

struct NotificationItem {
    int id = 0;
    NotificationKind kind = NotificationKind::None;
    Str title = {};
    Str message = {};
    El* content = nullptr;
    Listener onClick = {};

    bool hasDelivery = false;
    NotificationDelivery delivery = NotificationDelivery::InApp;
};

struct NotificationListState {
    ToastStackState stack = {};
    NotificationItem items[kToastStackCap] = {};
    int n = 0;
    int nextId = 1;
    NotificationAnchor placement = NotificationAnchor::TopRight;
    float width = kNotificationWidth;
    int maxItems = kNotificationMaxItems;

    float itemH = 76;

    double lastTickAt = 0;

    NotificationDelivery delivery = NotificationDelivery::InApp;

    EntityId self = {};

    static void OnCloseClick(NotificationListState* self, Ctx* cx,
                             const ClickEvent* ev, intptr_t id);
    static void OnItemClick(NotificationListState* self, Ctx* cx,
                            const ClickEvent* ev, intptr_t id);
    static void OnHover(NotificationListState* self, Ctx* cx,
                        const HoverEvent* ev);
    static void OnTick(NotificationListState* self, Ctx* cx,
                       const TickEvent* ev);

    static void OnSystemResponse(NotificationListState* self, Ctx* cx,
                                 const ClickEvent* ev, intptr_t id);
};

int NotificationPush(NotificationListState* s, Ctx* cx, NotificationItem item,
                     int timeoutMs);

void NotificationDismiss(NotificationListState* s, Ctx* cx, int id);

void NotificationClear(NotificationListState* s, Ctx* cx);

bool NotificationAdvance(NotificationListState* s, int deltaMs);

int NotificationIndexOf(const NotificationListState* s, int id);

const int kNotificationSystemMax = 100;

struct NotificationSystemEntry {
    int id = 0;
    EntityId list = {};
    Window* win = nullptr;
    Listener onClick = {};
};

Str NotificationSystemTag(char* buf, int cap, int id);

bool NotificationTagId(Str tag, int* outId);

void NotificationInitSystem();

void NotificationSystemInsert(const NotificationSystemEntry& e);

void NotificationSystemDismiss(int id, Window* win);

void NotificationSystemDismissAll(Window* win);
const NotificationSystemEntry* NotificationSystemFind(int id, Window* win);
int NotificationSystemCount();

void NotificationSystemResponse(Str tag);

struct Notification {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    NotificationKind kind = NotificationKind::None;
    Str title = {};
    Str message = {};

    El* action = nullptr;

    El* content = nullptr;
    NotificationAnchor anchor = NotificationAnchor::None;

    float width = 382;
    Listener onClose;

    Listener onClick;

    static Notification* New(Ctx* cx, Str title, Str message);
    Notification* Kind(NotificationKind k);
    Notification* Action(El* e);
    Notification* Content(El* e);
    Notification* Placement(NotificationAnchor p);
    Notification* OnClose(Listener fn);
    Notification* OnClick(Listener fn);
    El* IntoEl();
};

struct NotificationList {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Entity<NotificationListState> state = {};

    static NotificationList* New(Ctx* cx, Entity<NotificationListState> state);
    El* IntoEl();
};

}
}

#line 1 "src/ui/pagination.h"

namespace gpui {

namespace component {

struct PaginationMenuState {
    int firstPage = 1;
    Listener onChange = {};

    static void OnItem(PaginationMenuState* self, Ctx* cx, const ClickEvent* ev,
                       intptr_t ix);
};

struct Pagination {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    int page = 1;
    int total = 1;

    int visiblePages = 5;
    bool compact = false;
    bool disabled = false;
    UiSize size = UiSize::Medium;
    Listener onChange;

    static Pagination* New(Ctx* cx, int page, int total);
    Pagination* Id(Str s);
    Pagination* VisiblePages(int n);
    Pagination* Compact(bool v = true);
    Pagination* Disabled(bool v);
    Pagination* WithSize(UiSize s);
    Pagination* OnChange(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/plot.h"

namespace gpui {

namespace component {

struct ScaleLinear {
    int domainLen = 0;
    float domainStart = 0;
    float domainDiff = 0;
    float rangeStart = 0;
    float rangeDiff = 0;

    static ScaleLinear New(const float* domain, int domainN, const float* range,
                           int rangeN);

    bool Tick(float value, float* out) const;

    void LeastIndexWithDomain(float tick, const float* domain, int domainN,
                              int* outIndex, float* outTick) const;
};

struct ScalePoint {
    const float* domain = nullptr;
    int domainLen = 0;
    float rangeStart = 0;
    float rangeTick = 0;

    static ScalePoint New(const float* domain, int domainN, const float* range,
                          int rangeN);

    bool Tick(float value, float* out) const;

    int LeastIndex(float tick) const;
};

struct ScaleBand {
    int domainLen = 0;
    float rangeDiff = 0;
    float avgWidth = 0;
    float paddingInner = 0;
    float paddingOuter = 0;

    static ScaleBand New(int domainN, const float* range, int rangeN);

    float BandWidth() const;

    bool Tick(int index, float* out) const;

    int LeastIndex(float tick) const;
};

struct ScaleOrdinal {
    int rangeLen = 0;

    int unknown = -1;

    int Map(int domainIndex) const;
};

Point PlotTooltipPlace(Point cursor, Size within, Size box, float gap);

const float kPlotAxisGap = 18;

const float kPlotTextSize = 10;
const float kPlotTextGap = 4;

}
}

#line 1 "src/ui/popover.h"

namespace gpui {

namespace component {

El* PopoverSurface(Ctx* cx, El* e);

const float kDropdownEnterMs = 150.f;

const float kDropdownEnterOffset = -8.f;

El* DropdownOpen(Ctx* cx, El* surface, uint32_t key);

El* DropdownPlaceContent(El* content, float gap = 4);

struct Popover {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    El* trigger = nullptr;
    El* content = nullptr;

    bool controlled = false;
    bool open = false;
    bool defaultOpen = false;

    PopupAnchor anchor = PopupAnchor::TopLeft;

    MouseButton button = MouseButton::Left;
    Listener onClose;

    static Popover* New(Ctx* cx);
    static Popover* New(Ctx* cx, Str id);
    Popover* Trigger(El* e);
    Popover* Content(El* e);
    Popover* Open(bool v);
    Popover* DefaultOpen(bool v);
    Popover* Button(MouseButton b);
    Popover* Anchor(PopupAnchor v);

    Popover* OnClose(Listener fn);
    El* IntoEl();
};

bool PopoverOpen(Ctx* cx, Str id);

}
}

#line 1 "src/base/animation.h"

namespace gpui {

float CubicBezier(float x1, float y1, float x2, float y2, float t);

using EaseFn = float (*)(float);

float EaseLinear(float t);

float EaseOutCubic(float t);

float EaseInCubic(float t);

float EaseInOutCubic(float t);

float EaseQuadratic(float t);

float EaseInOutQuad(float t);
float EaseOutQuint(float t);

float EaseBounce(EaseFn e, float t);

float EaseBounceInOut(float t);

float ClampF01(float t);

float Lerp(float a, float b, float t);
Point Lerp(Point a, Point b, float t);

Rgba Lerp(Rgba a, Rgba b, float t);

}

#line 1 "src/base/motion.h"

namespace gpui {

struct Motion {
    float durationMs = 0;
    float delayMs = 0;
    EaseFn ease = EaseOutCubic;
};

inline Motion MotionNew(float durationMs) {
    Motion m;
    m.durationMs = durationMs;
    return m;
}

uint32_t MotionId(Str id);
uint32_t MotionId(Str id, Str channel);

uint32_t MotionName(Ctx* cx, Str name);

float MotionProgress(const Motion& m, float elapsedMs);

float MotionSample(const Motion& m, float progress);

bool MotionReduced();

void MotionSetReduced(bool on);

template <typename T>
struct MotionState {
    T from = {};
    T target = {};
    double startedAt = 0;
    bool init = false;
};

template <typename T>
struct MotionStep {
    T value = {};

    bool running = false;
};

inline bool MotionEq(float a, float b) {
    return a == b;
}
inline bool MotionEq(Point a, Point b) {
    return a.x == b.x && a.y == b.y;
}
inline bool MotionEq(Rgba a, Rgba b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

template <typename T>
MotionStep<T> MotionAdvance(MotionState<T>* st, T target, const Motion& m,
                            double now, bool reduced) {
    MotionStep<T> out;
    if (!st->init) {
        st->init = true;
        st->from = target;
        st->target = target;
        st->startedAt = now;
    }
    if (reduced || m.durationMs <= 0) {
        st->from = target;
        st->target = target;
        st->startedAt = now;
        out.value = target;
        return out;
    }
    float elapsedMs = (float)((now - st->startedAt) * 1000.0);
    float progress = MotionProgress(m, elapsedMs);
    T sampled = Lerp(st->from, st->target, MotionSample(m, progress));
    if (!MotionEq(st->target, target)) {

        st->from = sampled;
        st->target = target;
        st->startedAt = now;
        out.value = sampled;
        out.running = true;
        return out;
    }
    out.value = sampled;
    out.running = progress < 1.f && !MotionEq(st->from, st->target);
    return out;
}

double MotionNow(Ctx* cx);

void* MotionSlot(Ctx* cx, uint32_t key, int size);
void MotionWantsFrame(Ctx* cx);

float MotionRepeat(Ctx* cx, uint32_t key, float periodMs,
                   EaseFn ease = nullptr);

float MotionAppear(Ctx* cx, uint32_t key, float durationMs,
                   EaseFn ease = nullptr);

template <typename T>
void MotionSeed(Ctx* cx, uint32_t key, T from) {
    auto* st =
        (MotionState<T>*)MotionSlot(cx, key, (int)sizeof(MotionState<T>));
    if (!st) {
        return;
    }
    st->init = true;
    st->from = from;
    st->target = from;
    st->startedAt = MotionNow(cx);
}

struct Spring {

    float responseMs = 0;

    float damping = 1.f;

    float epsilon = 0.001f;

    bool travel = true;
};

inline Spring SpringNew(float responseMs) {
    Spring s;
    s.responseMs = responseMs;
    return s;
}

struct SpringState {
    float position = 0;
    float velocity = 0;
    float target = 0;
    double updatedAt = 0;
    bool init = false;
};

struct SpringStep {
    float value = 0;
    bool running = false;
};

SpringStep SpringAdvance(SpringState* st, float target, const Spring& s,
                         double now, bool reduced);

float SpringValue(Ctx* cx, uint32_t key, float target, const Spring& s);

inline void SpringSeed(Ctx* cx, uint32_t key, float from) {
    auto* st = (SpringState*)MotionSlot(cx, key, (int)sizeof(SpringState));
    if (!st) {
        return;
    }
    st->init = true;
    st->position = from;
    st->velocity = 0;
    st->target = from;
    st->updatedAt = MotionNow(cx);
}

template <typename T>
T MotionValue(Ctx* cx, uint32_t key, T target, const Motion& m) {
    auto* st =
        (MotionState<T>*)MotionSlot(cx, key, (int)sizeof(MotionState<T>));
    if (!st) {
        return target;
    }
    MotionStep<T> step =
        MotionAdvance(st, target, m, MotionNow(cx), MotionReduced());
    if (step.running) {
        MotionWantsFrame(cx);
    }
    return step.value;
}

}

#line 1 "src/ui/progress.h"

namespace gpui {

namespace component {

struct Progress {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    float value = 0;
    float w = 200;
    float h = 8;

    bool loading = false;

    Str id = {};

    static Progress* New(Ctx* cx);
    Progress* Value(float v);
    Progress* W(float v);
    Progress* H(float v);
    Progress* Loading(bool v);
    Progress* Id(Str v);
    El* IntoEl();
};

struct ProgressCircle {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    float value = 0;
    float size = 48;

    float startValue = 0;
    Rgba color = {};
    bool hasColor = false;
    bool showLabel = true;
    bool loading = false;
    Str id = {};

    static ProgressCircle* New(Ctx* cx);
    ProgressCircle* Loading(bool v);
    ProgressCircle* Id(Str v);
    ProgressCircle* Value(float v);
    ProgressCircle* Size(float v);
    ProgressCircle* Color(Rgba c);
    ProgressCircle* Label(bool v);
    El* IntoEl();
};

}
}

#line 1 "src/ui/radio.h"

namespace gpui {

namespace component {

struct Radio {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Str label = {};
    Str hint = {};
    bool checked = false;
    bool disabled = false;
    UiSize size = UiSize::Medium;
    bool focusRing = true;
    Listener onClick;

    static Radio* New(Ctx* cx, Str id);
    Radio* Label(Str s);
    Radio* Hint(Str s);
    Radio* Checked(bool v);
    Radio* Disabled(bool v);
    Radio* WithSize(UiSize s);

    Radio* FocusRing(bool v);
    Radio* OnClick(Listener fn);
    El* IntoEl();
};

struct RadioGroup {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Radio* radios[16] = {};
    int n = 0;
    bool horizontal = false;
    int selected = -1;
    bool disabled = false;
    UiSize size = UiSize::Medium;
    Listener onClick;

    static RadioGroup* Vertical(Ctx* cx, Str id);
    static RadioGroup* Horizontal(Ctx* cx, Str id);
    RadioGroup* Child(Radio* r);

    RadioGroup* Child(Str label);
    RadioGroup* Selected(int ix);
    RadioGroup* Disabled(bool v);
    RadioGroup* WithSize(UiSize s);
    RadioGroup* OnClick(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/rating.h"

namespace gpui {

namespace component {

struct RatingState {

    int defaultValue = 0;
    int value = 0;

    int hoveredValue = 0;
    Listener onClick = {};

    static void OnStarHover(RatingState* self, Ctx* cx, const HoverEvent* ev,
                            intptr_t ix);
    static void OnStarClick(RatingState* self, Ctx* cx, const ClickEvent* ev,
                            intptr_t ix);
};

struct Rating {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    int value = 0;
    int max = 5;
    bool disabled = false;
    Rgba color = {};
    bool hasColor = false;
    UiSize size = UiSize::Medium;
    Listener onClick;

    static Rating* New(Ctx* cx, Str id);
    Rating* Value(int v);
    Rating* Max(int v);
    Rating* Disabled(bool v);
    Rating* Color(Rgba c);
    Rating* WithSize(UiSize s);
    Rating* OnClick(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/sheet.h"

namespace gpui {

namespace component {

enum class SheetPlacement : uint8_t {
    Left,
    Top,
    Right,
    Bottom
};

struct Sheet {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str title = {};
    bool open = false;

    float size = 350;
    SheetPlacement placement = SheetPlacement::Right;
    bool overlay = true;
    El* body = nullptr;

    El* footer = nullptr;

    float scrollY = 0;
    int scrollId = 0;
    Listener onScroll;
    Listener onClose;

    static Sheet* New(Ctx* cx);
    Sheet* Title(Str s);
    Sheet* Placement(SheetPlacement p);
    Sheet* Size(float px);
    Sheet* Overlay(bool v);
    Sheet* Open(bool v);
    Sheet* Body(El* e);
    Sheet* Footer(El* e);
    Sheet* Scroll(int id, float y, Listener fn);
    Sheet* OnClose(Listener fn);
    El* IntoEl(WinSize size);
};

}
}

#line 1 "src/ui/window_border.h"

namespace gpui {

namespace component {

#if GPUI_OS_LINUX
const float kWindowShadowSize = 20;
#else
const float kWindowShadowSize = 0;
#endif
const float kWindowBorderSize = 1;

const float kWindowResizeHitSize = 4;

const float kWindowBorderRadius = 0;

struct WindowTiling {
    bool top = false;
    bool bottom = false;
    bool left = false;
    bool right = false;

    bool IsTiled() const { return top || bottom || left || right; }
    bool AllTiled() const { return top && bottom && left && right; }
};

Edges WindowBorderInsets(float shadowSize, WindowTiling tiling);

enum class WindowEdge : int8_t {
    None = -1,
    TopLeft = 0,
    Top = 1,
    TopRight = 2,
    Right = 3,
    BottomRight = 4,
    Bottom = 5,
    BottomLeft = 6,
    Left = 7
};

WindowEdge WindowResizeEdge(float x, float y, float w, float h, Edges insets,
                            WindowTiling tiling, float hitSize);

struct WindowBorder {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    El* child = nullptr;
    float shadowSize = kWindowShadowSize;
    WindowTiling tiling = {};

    static WindowBorder* New(Ctx* cx);
    WindowBorder* Child(El* e);
    WindowBorder* ShadowSize(float v);
    WindowBorder* Tiling(WindowTiling v);
    El* IntoEl();
};

}
}

#line 1 "src/ui/window_ext.h"

namespace gpui {

struct WindowLayer {
    EntityId view = {};

    bool overlay = true;
    component::SheetPlacement placement = component::SheetPlacement::Right;
    float size = 0;
};

struct WindowLayers {

    Vec<WindowLayer> dialogs;
    WindowLayer sheet = {};
    bool hasSheet = false;

    Entity<component::NotificationListState> notifications = {};
    int notifyTimer = 0;

    ~WindowLayers() { dialogs.Reset(); }
};

WindowLayers* WindowLayersOf(Window* win);

void WindowOpenDialog(Ctx* cx, EntityId view, bool overlay = true);
template <typename T>
inline void WindowOpenDialog(Ctx* cx, Entity<T> e, bool overlay = true) {
    WindowOpenDialog(cx, e.id, overlay);
}
bool WindowHasActiveDialog(Ctx* cx);
int WindowDialogCount(Ctx* cx);

void WindowCloseDialog(Ctx* cx);
void WindowCloseAllDialogs(Ctx* cx);

void WindowOpenSheetAt(Ctx* cx, EntityId view,
                       component::SheetPlacement placement, float size);

inline void WindowOpenSheet(Ctx* cx, EntityId view, float size) {
    WindowOpenSheetAt(cx, view, component::SheetPlacement::Right, size);
}
template <typename T>
inline void WindowOpenSheet(Ctx* cx, Entity<T> e, float size) {
    WindowOpenSheetAt(cx, e.id, component::SheetPlacement::Right, size);
}
bool WindowHasActiveSheet(Ctx* cx);
void WindowCloseSheet(Ctx* cx);

Entity<component::NotificationListState> WindowNotifications(Ctx* cx);

int WindowPushNotification(Ctx* cx, component::NotificationItem item,
                           int timeoutMs = 5000);

int WindowPushNotification(Ctx* cx, component::NotificationKind kind,
                           Str message);
void WindowClearNotifications(Ctx* cx);
int WindowNotificationCount(Ctx* cx);

InputState* WindowFocusedInput(Ctx* cx);
bool WindowHasFocusedInput(Ctx* cx);

}

#line 1 "src/ui/root.h"

namespace gpui {

namespace component {

int RootDialogOverlayIndex(const bool* wantsOverlay, int n);

Edges RootNotificationInsets(bool hasSheet, SheetPlacement placement,
                             float size);

struct Root {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    El* child = nullptr;
    bool bordered = true;

    float shadowSize = kWindowShadowSize;

    El* notifications = nullptr;
    El* sheet = nullptr;
    bool hasSheet = false;
    SheetPlacement sheetPlacement = SheetPlacement::Right;
    float sheetSize = 0;

    ArenaVec<El*> dialogs;
    ArenaVec<bool> dialogOverlay;

    bool windowLayers = true;

    static Root* New(Ctx* cx);
    Root* Bordered(bool v);
    Root* ShadowSize(float v);
    Root* Child(El* e);

    Root* Notifications(El* e);

    Root* Sheet(El* e, SheetPlacement placement, float size);

    Root* Dialog(El* e, bool overlay = true);
    Root* UseWindowLayers(bool v);
    El* IntoEl();
};

}
}

#line 1 "src/ui/scroll.h"

namespace gpui {

namespace component {

using ScrollAxis = gpui::ScrollAxis;

struct Scrollable {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    El* child = nullptr;
    Str id = {};
    float scrollY = 0;
    float scrollX = 0;
    float h = 200;
    ScrollAxis axis = ScrollAxis::Vertical;

    ScrollbarMode mode = ScrollbarMode::Always;

    Listener onScroll;

    static Scrollable* New(Ctx* cx);
    static Scrollable* New(Ctx* cx, Str id);
    Scrollable* Child(El* e);
    Scrollable* ScrollY(float v);
    Scrollable* ScrollX(float v);
    Scrollable* Axis(ScrollAxis v);
    Scrollable* Mode(ScrollbarMode v);
    Scrollable* H(float v);
    Scrollable* OnScroll(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/separator.h"

namespace gpui {

namespace component {

enum class SeparatorStyle : uint8_t {
    Solid,
    Dashed
};

struct Separator {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    bool vertical = false;
    SeparatorStyle line = SeparatorStyle::Solid;
    Str label = {};
    Rgba color = {};
    bool hasColor = false;

    static Separator* Vertical(Ctx* cx);
    static Separator* Horizontal(Ctx* cx);
    Separator* Dashed();
    Separator* Label(Str s);
    Separator* Color(Rgba c);
    El* IntoEl();
};

}
}

#line 1 "src/ui/setting.h"

namespace gpui {

namespace component {

enum class SettingFieldKind : uint8_t {
    Element,
    Switch,
    Checkbox,
    Input,
    NumberInput,
    Dropdown
};

struct NumberFieldOptions {
    double min = -1e300;
    double max = 1e300;
    double step = 1;
};

const int kMaxSettingKeywords = 4;

struct SettingItem {
    Str title = {};
    Str description = {};
    El* control = nullptr;
    Str keywords[kMaxSettingKeywords] = {};
    int nKeywords = 0;
    bool disabled = false;

    bool dirty = false;
    Listener onReset = {};

    SettingFieldKind field = SettingFieldKind::Element;

    bool* boolValue = nullptr;
    bool defBool = false;

    Str value = {};
    Str defStr = {};
    NumberFieldOptions num = {};

    Entity<SearchableListState> list = {};
    const SearchableItem* items = nullptr;
    int nItems = 0;
    int defIndex = 0;

    bool hasDefault = false;

    float fieldW = 0;

    Axis layout = Axis::Horizontal;
};

struct SettingGroup {
    Str title = {};
    Str description = {};
    ArenaVec<SettingItem> items;
};

struct SettingPage {
    Str title = {};
    Str description = {};
    IconName icon = IconName::None;
    ArenaVec<SettingGroup> groups;

    El* titleSuffix = nullptr;

    bool resettable = true;
};

bool SettingItemMatches(const SettingItem* it, Str query);
bool SettingGroupMatches(const SettingGroup* g, Str query);
bool SettingPageMatches(const SettingPage* p, Str query);

struct SettingBinding {
    SettingFieldKind kind = SettingFieldKind::Element;
    bool* boolValue = nullptr;
    bool defBool = false;
    InputState* input = nullptr;
    Str defStr = {};
    NumberFieldOptions num = {};
    Entity<SearchableListState> list = {};
    int defIndex = 0;
};

struct SettingFieldInput {
    InputState input;
    bool seeded = false;
};

struct SettingsState {
    int page = 0;
    int group = -1;

    InputState search;
    Vec<SettingBinding> fields;

    ~SettingsState() { fields.Reset(); }

    static void OnPageClick(SettingsState* self, Ctx* cx, const ClickEvent* ev,
                            intptr_t page);
    static void OnGroupClick(SettingsState* self, Ctx* cx, const ClickEvent* ev,
                             intptr_t packed);

    static void OnFieldClick(SettingsState* self, Ctx* cx, const ClickEvent* ev,
                             intptr_t ix);
    static void OnFieldReset(SettingsState* self, Ctx* cx, const ClickEvent* ev,
                             intptr_t ix);

    static void OnFieldInc(SettingsState* self, Ctx* cx, const ClickEvent* ev,
                           intptr_t ix);
    static void OnFieldDec(SettingsState* self, Ctx* cx, const ClickEvent* ev,
                           intptr_t ix);

    static void OnResetPage(SettingsState* self, Ctx* cx, const ClickEvent* ev,
                            intptr_t unused);

    static void OnSearchFocus(SettingsState* self, Ctx* cx,
                              const ClickEvent* ev);
};

struct Settings {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Entity<SettingsState> state = {};

    ArenaVec<SettingPage> pages;
    float sidebarWidth = 220;
    float h = 480;

    bool bordered = true;

    static Settings* New(Ctx* cx, Str id, Entity<SettingsState> state = {});
    Settings* Page(Str title, IconName icon = IconName::None,
                   Str description = {});
    Settings* Group(Str title, Str description = {});
    Settings* Item(Str title, Str description, El* control = nullptr);

    Settings* SwitchField(bool* value, bool defValue = false,
                          bool hasDefault = false);
    Settings* CheckboxField(bool* value, bool defValue = false,
                            bool hasDefault = false);
    Settings* InputField(Str value = {}, Str defValue = {});
    Settings* NumberField(Str value = {}, NumberFieldOptions opts = {},
                          Str defValue = {});
    Settings* DropdownField(Entity<SearchableListState> list,
                            const SearchableItem* items, int nItems,
                            int defIndex = -1);

    Settings* FieldWidth(float v);

    Settings* PageResettable(bool v);

    Settings* PageTitleSuffix(El* e);

    Settings* Keywords(Str a1, Str a2 = {}, Str a3 = {});
    Settings* Disabled(bool v = true);
    Settings* Resettable(bool dirty, Listener onReset);
    Settings* Layout(Axis axis);
    Settings* SidebarWidth(float v);
    Settings* H(float v);
    Settings* Bordered(bool v);
    El* IntoEl();
};

}
}

#line 1 "src/ui/sidebar.h"

namespace gpui {

namespace component {

enum class SidebarCollapsible : uint8_t {
    Icon,
    Offcanvas,
    None
};

enum class SidebarWrapperKind : uint8_t {

    None,

    Static,

    Animated
};

struct SidebarLayout {
    bool iconCollapsed = false;
    bool offcanvasCollapsed = false;

    bool alignChildToEnd = false;
    SidebarWrapperKind wrapper = SidebarWrapperKind::None;
    float wrapperWidth = 0;
};

SidebarLayout SidebarLayoutFor(SidebarCollapsible collapsible, bool collapsed,
                               float expandedWidth, Side side);

const float kSidebarCollapsedWidth = 48;

struct SidebarMenuState {
    bool open = false;

    bool seeded = false;
    bool clickToOpen = false;
    bool clickToToggle = false;
    Listener onClick = {};

    static void OnItemClick(SidebarMenuState* self, Ctx* cx,
                            const ClickEvent* ev);
    static void OnCaretClick(SidebarMenuState* self, Ctx* cx,
                             const ClickEvent* ev);
};

struct SidebarMenuItem {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    IconName icon = IconName::None;
    Str label = {};
    Listener onClick;
    bool active = false;
    bool disabled = false;
    bool defaultOpen = false;
    bool clickToOpen = false;
    bool clickToToggle = false;
    El* suffix = nullptr;
    SidebarMenuItem* children[16] = {};
    PopupMenu* contextMenu = nullptr;
    int nChildren = 0;

    bool collapsed = false;

    static SidebarMenuItem* New(Ctx* cx, Str label);
    SidebarMenuItem* Icon(IconName v);
    SidebarMenuItem* Active(bool v);
    SidebarMenuItem* Disabled(bool v);
    SidebarMenuItem* DefaultOpen(bool v);
    SidebarMenuItem* ClickToOpen(bool v);
    SidebarMenuItem* ClickToToggle(bool v);
    SidebarMenuItem* Suffix(El* e);
    SidebarMenuItem* Child(SidebarMenuItem* item);
    SidebarMenuItem* OnClick(Listener fn);

    SidebarMenuItem* ContextMenu(PopupMenu* menu);

    El* IntoEl(Str id);
};

struct SidebarMenu {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    SidebarMenuItem* items[24] = {};
    int n = 0;
    bool collapsed = false;

    static SidebarMenu* New(Ctx* cx);
    SidebarMenu* Child(SidebarMenuItem* item);
    El* IntoEl(Str id);
};

struct SidebarGroup {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str label = {};
    SidebarMenu* menus[8] = {};
    int n = 0;
    bool collapsed = false;

    static SidebarGroup* New(Ctx* cx, Str label);
    SidebarGroup* Child(SidebarMenu* menu);
    El* IntoEl(Str id);
};

El* SidebarHeader(Ctx* cx, El* child, bool selected = false,
                  Listener onClick = {});
El* SidebarFooter(Ctx* cx, El* child, bool selected = false,
                  Listener onClick = {});

struct SidebarToggleButton {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    bool collapsed = false;
    Side side = Side::Left;
    Listener onClick;

    static SidebarToggleButton* New(Ctx* cx);
    SidebarToggleButton* Collapsed(bool v);
    SidebarToggleButton* WithSide(Side v);
    SidebarToggleButton* OnClick(Listener fn);
    El* IntoEl();
};

struct Sidebar {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    El* header = nullptr;
    El* footer = nullptr;
    SidebarGroup* groups[8] = {};
    int n = 0;
    Side side = Side::Left;
    SidebarCollapsible collapsible = SidebarCollapsible::Icon;
    bool collapsed = false;

    float width = 255;

    static Sidebar* New(Ctx* cx, Str id);
    Sidebar* WithSide(Side v);
    Sidebar* Collapsible(SidebarCollapsible v);
    Sidebar* Collapsed(bool v);
    Sidebar* Header(El* e);
    Sidebar* Footer(El* e);
    Sidebar* Child(SidebarGroup* group);
    Sidebar* W(float px);
    El* IntoEl();
};

}
}

#line 1 "src/ui/skeleton.h"

namespace gpui {

namespace component {

struct Skeleton {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    bool secondary = false;
    float w = kFill;
    float h = 16;

    static Skeleton* New(Ctx* cx);
    Skeleton* Secondary();
    Skeleton* W(float v);
    Skeleton* H(float v);
    El* IntoEl();
};

}
}

#line 1 "src/ui/slider.h"

namespace gpui {

namespace component {

struct Slider {
    Arena* a = nullptr;
    Ctx* cx = nullptr;

    Str id = {};

    SliderState* state = nullptr;

    bool reverse = false;
    bool disabled = false;

    Axis axis = Axis::Horizontal;

    float width = 224;

    Rgba bar = {};
    bool hasBar = false;
    Listener onChange;

    static Slider* New(Ctx* cx, Str id, SliderState* state);
    Slider* Reverse(bool v = true);
    Slider* Vertical(bool v = true);
    Slider* WithAxis(Axis v);
    Slider* Disabled(bool v = true);
    Slider* W(float px);

    Slider* WFill();
    Slider* Bg(Rgba c);

    Slider* OnChange(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/spinner.h"

namespace gpui {

namespace component {

struct Spinner {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    UiSize size = UiSize::Medium;
    float px = 0;
    IconName icon = IconName::Loader;
    Rgba color = {};
    bool hasColor = false;

    float speed = 0;
    EaseFn ease = nullptr;

    Str id = {};

    static Spinner* New(Ctx* cx);
    Spinner* Speed(float ms);
    Spinner* Ease(EaseFn fn);
    Spinner* Id(Str v);
    Spinner* WithSize(UiSize s);
    Spinner* Size(float v);
    Spinner* Icon(IconName n);
    Spinner* Color(Rgba c);
    El* IntoEl();
};

}
}

#line 1 "src/ui/status_bar.h"

namespace gpui {

namespace component {

struct StatusBar {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    ArenaVec<El*> left;
    ArenaVec<El*> center;
    ArenaVec<El*> right;

    static StatusBar* New(Ctx* cx);
    StatusBar* Left(El* e);
    StatusBar* Left(Str s);

    StatusBar* Center(El* e);
    StatusBar* Center(Str s);
    StatusBar* Right(El* e);
    StatusBar* Right(Str s);
    El* IntoEl();
};

}
}

#line 1 "src/ui/stepper.h"

namespace gpui {

namespace component {

struct StepperItem {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    IconName icon = IconName::None;

    El* child = nullptr;
    bool disabled = false;

    int step = 0;
    int checkedStep = 0;
    Axis layout = Axis::Horizontal;
    bool isLast = false;
    bool textCenter = false;
    UiSize size = UiSize::Medium;
    Listener onClick;

    static StepperItem* New(Ctx* cx);
    StepperItem* Icon(IconName v);
    StepperItem* Child(El* e);
    StepperItem* Disabled(bool v);
    El* IntoEl();
};

struct Stepper {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    StepperItem* items[16] = {};
    int n = 0;
    int step = 0;
    Axis layout = Axis::Horizontal;
    bool disabled = false;
    bool textCenter = false;
    bool itemsCenter = false;
    UiSize size = UiSize::Medium;

    float width = kFill;
    Listener onClick;

    static Stepper* New(Ctx* cx, Str id);
    Stepper* Item(StepperItem* item);
    Stepper* SelectedIndex(int i);
    Stepper* Layout(Axis v);
    Stepper* Vertical();
    Stepper* TextCenter(bool v);

    Stepper* ItemsCenter(bool v = true);
    Stepper* Disabled(bool v);
    Stepper* WithSize(UiSize s);
    Stepper* W(float px);
    Stepper* OnClick(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/switch.h"

namespace gpui {

namespace component {

struct Switch {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Str label = {};
    bool checked = false;
    bool disabled = false;
    UiSize size = UiSize::Medium;
    Rgba color = {};
    bool hasColor = false;
    Listener onClick;

    static Switch* New(Ctx* cx, Str id);
    Switch* Label(Str s);
    Switch* Checked(bool v);
    Switch* Disabled(bool v);
    Switch* WithSize(UiSize s);
    Switch* Color(Rgba c);
    Switch* OnClick(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/ui/tab.h"

namespace gpui {

namespace component {

enum class TabVariant : uint8_t {
    Tab,
    Outline,
    Pill,
    Segmented,
    Underline
};

float TabHeight(TabVariant v, UiSize size);
float TabInnerHeight(TabVariant v, UiSize size);

float TabPadX(TabVariant v, UiSize size);

float TabMarginTop(TabVariant v, UiSize size);
float TabMarginBottom(TabVariant v, UiSize size);

float TabBarGap(TabVariant v, UiSize size);

float TabBarPadX(TabVariant v, UiSize size);

float TabBarRadius(TabVariant v, UiSize size, float radius, float radiusLg);
float TabRadius(TabVariant v, UiSize size, float radius, float radiusLg);
float TabInnerRadius(TabVariant v, UiSize size, float radius, float radiusLg);

struct TabItem {
    Str label = {};
    IconName icon = IconName::None;
    bool disabled = false;

    bool flex1 = false;
};

struct Tabs {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};

    ArenaVec<TabItem> items;
    int selected = 0;
    TabVariant variant = TabVariant::Tab;
    UiSize size = UiSize::Medium;

    float maxWidth = 0;

    float width = kAuto;
    El* prefix = nullptr;
    El* suffix = nullptr;

    bool menu = false;
    Listener onChange;

    static Tabs* New(Ctx* cx);
    static Tabs* New(Ctx* cx, Str id);
    Tabs* Tab(Str label);
    Tabs* Tab(Str label, IconName icon, bool disabled = false);

    Tabs* Flex1();
    Tabs* Disabled(int ix, bool v = true);
    Tabs* Selected(int i);
    Tabs* OnChange(Listener fn);
    Tabs* Variant(TabVariant v);
    Tabs* Outline();
    Tabs* Pill();
    Tabs* Segmented();
    Tabs* Underline();
    Tabs* Size(UiSize v);
    Tabs* MaxWidth(float v);

    Tabs* W(float v);
    Tabs* WFill();
    Tabs* Prefix(El* e);
    Tabs* Suffix(El* e);
    Tabs* Menu(bool v = true);
    El* IntoEl();
};

}
}

#line 1 "src/base/data_table.h"

namespace gpui {

enum class TableSelectionMode : uint8_t {
    None,
    Row,
    Column,
    Cell
};

enum class ColumnSort : uint8_t {
    Default,
    Ascending,
    Descending
};

enum class TableAction : uint8_t {
    None,
    SelectPrev,
    SelectNext,
    SelectPrevColumn,
    SelectNextColumn,
    SelectFirst,
    SelectLast,
    SelectPageUp,
    SelectPageDown,
    Cancel
};

void TableInitKeys();
Str TableContext();
TableAction TableActionOf(uint32_t id);

enum class TableEventKind : uint8_t {
    SelectRow,
    SelectCol,
    SelectCell,
    DoubleClickedRow,
    DoubleClickedCell,

    RightClickedRow,
    RightClickedCell,
    Sort,
    ColumnWidthsChanged,

    MoveColumn,
    Cancel
};

struct TableEvent {
    TableEventKind kind = TableEventKind::SelectRow;
    int row = -1;
    int col = -1;

    ColumnSort sort = ColumnSort::Default;

    const float* widths = nullptr;
    int nWidths = 0;
};

extern const Str kTableResizeDrag;

extern const Str kTableColDrag;

const float kTableResizeHandleW = 2;

struct TableVisibleRange {
    int rowFirst = 0;
    int rowEnd = 0;
    int colFirst = 0;
    int colEnd = 0;
};

struct TableState {
    int rowCount = 0;
    int colCount = 0;
    TableSelectionMode mode = TableSelectionMode::None;
    int selectedRow = -1;
    int selectedCol = -1;
    int selectedCellRow = -1;
    int selectedCellCol = -1;

    int rightClickedRow = -1;

    int rightClickedCellRow = -1;
    int rightClickedCellCol = -1;
    bool rowSelectable = true;
    bool colSelectable = true;
    bool cellSelectable = false;

    bool rowHeader = true;

    bool loopSelection = false;
    bool sortable = true;

    int sortCol = -1;
    ColumnSort sort = ColumnSort::Default;

    int pageRows = 10;

    bool colResizable = true;

    Vec<float> colWidth;

    float colMinWidth = 20;
    float colMaxWidth = 0;

    int resizingCol = -1;

    Vec<int> colOrder;
    bool colOrderSeeded = false;

    bool colMovable = true;
    int draggingCol = -1;
    int dropGap = -1;

    Vec<Bounds> colBounds;

    float rowH = 32;
    float scrollY = 0;
    float viewportH = 0;

    float scrollX = 0;
    float viewportW = 0;

    int fixedCols = 0;

    bool colFixed = true;

    Bounds bodyBounds = {};

    TableVisibleRange visibleRange = {};

    bool loading = false;
    bool hasMore = false;
    int loadMoreThreshold = 20;
    Listener onEvent = {};

    EntityId self = {};

    FocusHandle focus = {};

    ~TableState() {
        colWidth.Reset();
        colOrder.Reset();
        colBounds.Reset();
    }
    Listener onLoadMore = {};

    static void OnRowClick(TableState* self, Ctx* cx, const ClickEvent* ev,
                           intptr_t row);

    static void OnCellClick(TableState* self, Ctx* cx, const ClickEvent* ev,
                            intptr_t packed);
    static void OnRowMouseDown(TableState* self, Ctx* cx,
                               const MouseDownEvent* ev, intptr_t row);

    static void OnCellMouseDown(TableState* self, Ctx* cx,
                                const MouseDownEvent* ev, intptr_t packed);
    static void OnHeadClick(TableState* self, Ctx* cx, const ClickEvent* ev,
                            intptr_t col);
    static void OnSortClick(TableState* self, Ctx* cx, const ClickEvent* ev,
                            intptr_t col);

    static void OnResizeDrag(TableState* self, Ctx* cx,
                             const DragMoveEvent* ev);
    static void OnResizeEnd(TableState* self, Ctx* cx, const MouseUpEvent* ev);
    static void OnColDragMove(TableState* self, Ctx* cx,
                              const DragMoveEvent* ev);
    static void OnColDrop(TableState* self, Ctx* cx, const DropEvent* ev);
    static void OnColDragEnd(TableState* self, Ctx* cx, const MouseUpEvent* ev);
    static void OnScroll(TableState* self, Ctx* cx, const ScrollEvent* ev);

    static void OnScrollXY(TableState* self, Ctx* cx, const ScrollEvent* ev);
};

void TableSeedColOrder(TableState* s, int colCount);
int TableColAt(const TableState* s, int display);

int TableDisplayOfCol(const TableState* s, int col);

bool TableMoveColumn(TableState* s, int from, int to);
void TableMoveColumnEvent(TableState* s, Ctx* cx, int from, int to);

int TableDragGapAt(const Bounds* colBounds, int n, float x, int dragCol);

void TableEnsureCols(TableState* s, int n);

inline intptr_t TableCellPack(int row, int col) {
    return ((intptr_t)row << 12) | (intptr_t)(col & 0xfff);
}
inline int TableCellRow(intptr_t packed) {
    return (int)(packed >> 12);
}
inline int TableCellCol(intptr_t packed) {
    return (int)(packed & 0xfff);
}

bool TableVisibleRowsChanged(TableState* s, int first, int end);
bool TableVisibleColsChanged(TableState* s, int first, int end);

void TableVisibleCols(const TableState* s, int* first, int* end);

bool TableShouldLoadMore(const TableState* s, int visibleEnd);

void TableScrollToRow(TableState* s, int row, ScrollStrategy strategy);

void TableScrollToCol(TableState* s, int col, ScrollStrategy strategy);

void TableRefreshCols(TableState* s);

float TableColWidth(const TableState* s, int col, float declared);

void TableSeedColWidth(TableState* s, int col, float declared);

float TableClampColWidth(const TableState* s, float width);

void TableResizeCol(TableState* s, Ctx* cx, int col, float width);

ColumnSort TableNextSort(ColumnSort s);

ColumnSort TableSortOf(const TableState* s, int col);

void TablePerformSort(TableState* s, Ctx* cx, int col);
void TableSetSelectedRow(TableState* s, Ctx* cx, int row);
void TableSetSelectedCol(TableState* s, Ctx* cx, int col);
void TableSetSelectedCell(TableState* s, Ctx* cx, int row, int col);
void TableClearSelection(TableState* s, Ctx* cx);

bool TableEscalatesToRow(const TableState* s, int row, int col,
                         bool doubleClick);

void TablePerform(TableState* s, Ctx* cx, TableAction act);

void TableOnAction(TableState* self, Ctx* cx, const ActionEvent* ev);
void TableBindKeys(Ctx* cx, El* root, Entity<TableState> state);

}

#line 1 "src/ui/table.h"

namespace gpui {

namespace component {

struct TableColumn {
    Str title = {};
    float width = 100;
    bool right = false;
    bool sortable = false;
    bool selectable = true;

    bool resizable = true;

    bool fixed = false;
};

struct TableGroupCell {
    Str label = {};
    int span = 1;
};

struct DataTable {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Entity<TableState> state = {};
    const TableColumn* columns = nullptr;
    int nColumns = 0;

    El* (*cell)(Ctx* cx, void* data, int row, int col) = nullptr;
    void* data = nullptr;
    int nRows = 0;
    bool stripe = false;

    const TableGroupCell* groupRows[4] = {};
    int groupRowLens[4] = {};
    int nGroupHeaders = 0;

    float h = 0;

    El* empty = nullptr;

    PopupMenu* (*contextMenu)(Ctx* cx, void* data, int row,
                              PopupMenu* menu) = nullptr;

    El* (*lastEmptyCol)(Ctx* cx, void* data) = nullptr;

    void (*visibleRowsChanged)(Ctx* cx, void* data, int first,
                               int end) = nullptr;
    void (*visibleColsChanged)(Ctx* cx, void* data, int first,
                               int end) = nullptr;

    Str (*cellText)(Ctx* cx, void* data, int row, int col) = nullptr;

    float rowHeight = 32;

    UiSize size = UiSize::Medium;

    static DataTable* New(Ctx* cx, Str id, Entity<TableState> state);
    DataTable* Columns(const TableColumn* cols, int n);
    DataTable* Rows(int n, void* data,
                    El* (*cell)(Ctx* cx, void* data, int row, int col));
    DataTable* Stripe(bool v);
    DataTable* WithSize(UiSize s);

    DataTable* RowHeight(float px);
    DataTable* GroupHeader(const TableGroupCell* cells, int n);
    DataTable* H(float px);
    DataTable* Empty(El* e);
    DataTable* ContextMenu(PopupMenu* (*fn)(Ctx*, void*, int, PopupMenu*));
    DataTable* LastEmptyCol(El* (*fn)(Ctx*, void*));
    DataTable* OnVisibleRows(void (*fn)(Ctx*, void*, int, int));
    DataTable* OnVisibleCols(void (*fn)(Ctx*, void*, int, int));
    DataTable* CellText(Str (*fn)(Ctx*, void*, int, int));

    void Dump(Vec<Str>* heads, Vec<Str>* cells);

    void Headers(Vec<Str>* heads);

    void DumpRange(int lo, int hi, Vec<Str>* heads, Vec<Str>* cells);
    El* IntoEl();

    El* BuildEl();

    float ColWidth(const TableState* s, int col) const;
};

enum class TableAlign : uint8_t {
    Left,
    Center,
    Right
};

struct TableCellEl {
    Arena* a = nullptr;
    Ctx* cx = nullptr;

    bool head = false;
    int ix = 0;
    int colSpan = 1;
    TableAlign align = TableAlign::Left;
    UiSize size = UiSize::Medium;

    float width = kAuto;
    ArenaVec<El*> children;

    TableCellEl* ColSpan(int n);
    TableCellEl* TextCenter();
    TableCellEl* TextRight();
    TableCellEl* W(float v);
    TableCellEl* WithSize(UiSize s);
    TableCellEl* Child(El* e);
    El* IntoEl();
};

struct TableHead {
    static TableCellEl* New(Ctx* cx);
};
struct TableCell {
    static TableCellEl* New(Ctx* cx);
};

struct TableRow {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    int ix = 0;
    UiSize size = UiSize::Medium;
    bool hasBg = false;
    Background bg = {};
    ArenaVec<TableCellEl*> cells;

    static TableRow* New(Ctx* cx);
    TableRow* Bg(Background c);
    TableRow* Child(TableCellEl* c);
    El* IntoEl();
};

enum class TableGroupKind : uint8_t {
    Header,
    Body,
    Footer
};

struct TableGroup {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    TableGroupKind kind = TableGroupKind::Body;
    int ix = 0;
    UiSize size = UiSize::Medium;
    ArenaVec<TableRow*> rows;

    TableGroup* Child(TableRow* r);
    El* IntoEl();
};

struct TableHeader {
    static TableGroup* New(Ctx* cx);
};
struct TableBody {
    static TableGroup* New(Ctx* cx);
};
struct TableFooter {
    static TableGroup* New(Ctx* cx);
};

struct TableCaption {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    UiSize size = UiSize::Medium;
    ArenaVec<El*> children;

    static TableCaption* New(Ctx* cx);
    TableCaption* Child(El* e);
    El* IntoEl();
};

struct Table {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    UiSize size = UiSize::Medium;
    bool bordered = false;
    ArenaVec<TableGroup*> groups;
    TableCaption* caption = nullptr;

    Str id = {};

    static Table* New(Ctx* cx, Str id);
    Table* WithSize(UiSize s);

    Table* Bordered(bool v = true);
    Table* Child(TableGroup* g);
    Table* Child(TableCaption* c);
    El* IntoEl();
};

}
}

#line 1 "src/ui/tag.h"

namespace gpui {

namespace component {

enum class TagVariant : uint8_t {
    Primary,
    Secondary,
    Danger,
    Success,
    Warning,
    Info
};

struct Tag {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    TagVariant variant = TagVariant::Secondary;
    bool outline = false;
    UiSize size = UiSize::Medium;
    float radius = -1;
    Str text = {};
    Rgba customBg = {};
    Rgba customFg = {};
    Rgba customBorder = {};
    bool hasCustom = false;

    static Tag* New(Ctx* cx, Str text);
    Tag* Primary();
    Tag* Secondary();
    Tag* Danger();
    Tag* Success();
    Tag* Warning();
    Tag* Info();
    Tag* Outline();
    Tag* WithSize(UiSize s);
    Tag* Radius(float v);

    Tag* Custom(Rgba bg, Rgba fg, Rgba border = {});
    El* IntoEl();
};

}
}

#line 1 "src/ui/text.h"

namespace gpui {

namespace component {

enum MdMark : uint8_t {
    MdBold = 1 << 0,
    MdItalic = 1 << 1,
    MdCode = 1 << 2,
    MdDel = 1 << 3,
    MdUnderline = 1 << 4,
    MdLink = 1 << 5,

    MdHighlight = 1 << 6,
};

struct MdRun {
    Str text = {};

    Str href = {};

    Str imgSrc = {};

    float imgW = 0;
    float imgH = 0;
    MdRun* next = nullptr;
    uint8_t marks = 0;
};

enum class MdKind : uint8_t {
    Doc,
    Paragraph,
    Heading,
    Quote,
    List,
    Item,
    Code,
    Table,
    Row,
    Cell,
    Rule,

    Html,

    Group,
};

enum MdAlign : uint8_t {
    MdAlignDefault = 0,
    MdAlignLeft = 1,
    MdAlignCenter = 2,
    MdAlignRight = 3,
};

struct MdNode {
    MdKind kind = MdKind::Doc;
    MdNode* parent = nullptr;
    MdNode* first = nullptr;
    MdNode* last = nullptr;
    MdNode* next = nullptr;

    MdRun* runFirst = nullptr;
    MdRun* runLast = nullptr;

    Str lang = {};

    Str raw = {};

    int start = 1;

    uint8_t level = 0;

    uint8_t align = 0;
    bool ordered = false;

    bool head = false;

    bool hasCheck = false;
    bool checked = false;
};

struct MdPluginNode {

    Str name = {};

    Str text = {};
    Str markdown = {};

    void* data = nullptr;
};

using MdPluginParseFn = bool (*)(Ctx* cx, MdNode* node, Str text, void* data,
                                 MdPluginNode* out);

using MdPluginRenderFn = El* (*)(Ctx * cx, const MdPluginNode* node,
                                 void* data);

struct MdPlugin {
    Str name = {};
    MdPluginParseFn parse = nullptr;
    MdPluginRenderFn render = nullptr;
    void* data = nullptr;
};

using CodeBlockActionsFn = El* (*)(Ctx * cx, void* data, Str code, Str lang);

struct TableData {

    const Str* header = nullptr;
    const Str* rows = nullptr;
    int cols = 0;
    int rowCount = 0;
    Str markdown = {};

    Str Cell(int row, int col) const {
        if (row < 0 || col < 0 || col >= cols || row >= rowCount) {
            return {};
        }
        return rows[row * cols + col];
    }
};

using TableActionsFn = El* (*)(Ctx * cx, void* data, const TableData* table);

Str MdTableToMarkdown(Arena* a, MdNode* table);

struct TextView {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str source = {};

    float baseFont = 16;

    float headingFont = 14;

    float codeFont = 13;

    float paragraphGap = 16;

    bool selectable = false;

    bool html = false;

    Listener onLink;
    CodeBlockActionsFn codeActions = nullptr;
    TableActionsFn tableActions = nullptr;
    void* tableActionsData = nullptr;
    void* codeActionsData = nullptr;

    static const int kMaxPlugins = 8;
    MdPlugin plugins[kMaxPlugins] = {};
    int nPlugins = 0;

    float tableColW = 64;

    bool tableScroll = false;

    int tableIx = 0;

    gpui::SelectionFormat selFormat = gpui::SelectionFormat::Plain;

    static TextView* New(Ctx* cx, Str source);
    static TextView* NewHtml(Ctx* cx, Str source);
    TextView* Font(float px);
    TextView* HeadingFont(float px);
    TextView* Selectable(bool on = true);

    TextView* SelFormat(gpui::SelectionFormat fmt);
    TextView* TableColumnWidth(float px);
    TextView* TableScroll(bool on = true);
    TextView* ParagraphGap(float px);

    TextView* OnLink(Listener fn);

    TextView* CodeBlockActions(CodeBlockActionsFn fn, void* data = nullptr);

    TextView* TableActions(TableActionsFn fn, void* data = nullptr);

    TextView* Plugin(Str name, MdPluginParseFn parse, MdPluginRenderFn render,
                     void* data = nullptr);
    El* IntoEl();

  private:

    Rgba blockFg = {};
    bool blockFgSet = false;
    Rgba BlockFg() const;

    Str srcLinePre = {};

    Str srcMarker = {};

    Str srcItemMarker = {};

    Str srcItemPad = {};

    bool inTodo = false;

    const SelBlock* srcBlock = nullptr;
    bool srcLineStart = true;

    const SelSource* srcRunLast = nullptr;
    uint8_t srcRunMarks = 0;
    Str srcRunHref = {};

    const SelBlock* SrcOpen(Str marker, Str post, bool join = false);

    void SrcCell(MdNode* row, MdNode* c, int nCols, const uint8_t* colAlign);

    El* SrcMark(El* t, uint8_t marks, Str href = {});

    void SrcBreak();

    El* SrcImage(El* e, MdRun* r);

    Str BlockText(MdNode* n);
    El* PluginBlock(MdNode* n);

    El* ScrollTable(MdNode* n);

    El* Block(MdNode* n, int depth, bool inList, bool isLast);
    El* Blocks(El* into, MdNode* n, int depth, bool inList);
    El* Item(MdNode* n, Str marker, int depth);
    El* Table(MdNode* n);

    El* TableActionsRow(MdNode* n, int nCols, const uint8_t* colAlign);
    El* CodeBlock(MdNode* n);

    El* CodeLines(Str code, SyntaxLang lang);

    El* ImageRun(MdRun* r, float font, Rgba color, bool inFlow);

    El* Word(Str w, float font, Rgba color, uint8_t marks, int weight,
             Str href);

    El* Inline(MdNode* n, float font, Rgba color, int weight,
               uint8_t align = MdAlignDefault);
};

MdNode* MdParse(Arena* a, Str source);

Str MdDecodeEntity(Arena* a, Str e);

}
}

#line 1 "src/ui/title_bar.h"

namespace gpui {

namespace component {

constexpr float kTitleBarHeight = 34.f;

constexpr float kTitleBarLeftPad = GPUI_OS_MAC ? 80.f : 12.f;

struct TitleBar {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    El* bar = nullptr;
    El* content = nullptr;

    static TitleBar* New(Ctx* cx);
    TitleBar* Child(El* e);
    El* IntoEl();
};

}
}

#line 1 "src/ui/tooltip.h"

namespace gpui {

namespace component {

struct Tooltip {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str text = {};

    static Tooltip* New(Ctx* cx, Str text);
    El* IntoEl();
};

}
}

#line 1 "src/ui/tree.h"

namespace gpui {

namespace component {

struct Tree {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Entity<TreeState> state = {};
    float h = 320;

    bool icons = true;

    static Tree* New(Ctx* cx, Str id, Entity<TreeState> state);
    Tree* H(float v);
    Tree* Icons(bool v);
    El* IntoEl();
};

}
}

#line 1 "src/ui/virtual_list.h"

namespace gpui {

namespace component {

struct VirtualList {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = StrL("vlist");
    int count = 0;
    float rowH = 32;
    float viewH = 192;
    float scrollY = 0;

    float scrollX = 0;

    const float* sizes = nullptr;

    VirtualListScrollHandle* handle = nullptr;
    Listener onRenderRow;
    El* (*row)(Ctx* cx, int ix) = nullptr;

    int scrollId = 0;
    Listener onScroll = {};

    ScrollAxis axis = ScrollAxis::Both;

    float pad = 0;

    static VirtualList* New(Ctx* cx, int count);
    VirtualList* Id(Str v);
    VirtualList* RowH(float v);
    VirtualList* ViewH(float v);
    VirtualList* ScrollY(float v);
    VirtualList* ScrollX(float v);
    VirtualList* Sizes(const float* v);
    VirtualList* Handle(VirtualListScrollHandle* h);
    VirtualList* Scroll(int id, Listener onScroll);
    VirtualList* Axis(ScrollAxis v);
    VirtualList* Pad(float v);

    VirtualList* Row(El* (*fn)(Ctx*, int));
    El* IntoEl();
};

}
}

#line 1 "src/ui/lib.h"

#line 1 "src/sys/sysinfo.h"

namespace gpui {

struct ProcessInfo {
    uint32_t pid = 0;
    char name[260] = {};
    float cpu = 0;
    uint64_t memory = 0;
};

struct ProcSample {
    uint32_t pid = 0;
    uint64_t cpu100ns = 0;
};

struct DiskInfo {
    float usedPct = 0;
    uint64_t total = 0;
    uint64_t used = 0;
};

struct BatteryInfo {
    bool present = false;
    bool charging = false;
    float pct = 0;
};

struct SysTimes {
    uint64_t idle = 0;
    uint64_t kernel = 0;
    uint64_t user = 0;
    bool valid = false;
};

struct SysState {
    SysTimes prevCpu;
    Vec<ProcSample> prevProcs;
    float cpu;
    float mem;
    uint64_t memTotal;
    uint64_t memUsed;
    DiskInfo disk;
    BatteryInfo battery;
    Vec<ProcessInfo> procs;
    int ncpu;

    SysState() : cpu(0), mem(0), memTotal(0), memUsed(0), ncpu(1) {
        ZeroStruct(&prevCpu);
        ZeroStruct(&disk);
        ZeroStruct(&battery);
    }
};

void SysStateInit(SysState* s);
void SysStateFree(SysState* s);
void SysRefresh(SysState* s);

enum class ProcessSort : int32_t {
    Pid = 0,
    Name = 1,
    Cpu = 2,
    Memory = 3
};
void SysSortProcesses(SysState* s, ProcessSort field, bool descending,
                      int keepTop);

TempStr FormatBytes(uint64_t bytes);
TempStr FormatPct(float v, int decimals);
}

#line 1 "src/base/actions.h"

namespace gpui {

namespace action {

uint32_t Confirm();

constexpr intptr_t kConfirmSecondary = 1;
uint32_t Cancel();
uint32_t SelectUp();
uint32_t SelectDown();
uint32_t SelectLeft();
uint32_t SelectRight();
uint32_t SelectFirst();
uint32_t SelectLast();
uint32_t SelectPrevColumn();
uint32_t SelectNextColumn();
uint32_t SelectPageUp();
uint32_t SelectPageDown();

}

struct CancelKeys {
    Listener onCancel = {};

    static void OnAction(CancelKeys* self, Ctx* cx, const ActionEvent* ev);
};

void CancelInitKeys(const char* context);

void CancelBindKeys(Ctx* cx, El* root, const char* context, Str name,
                    Listener onCancel);

}

#line 1 "src/gpui/keymap.h"

namespace gpui {

struct KeyChord {
    int vk = 0;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;

    bool platform = false;
};

bool KeyChordParse(Str spec, KeyChord* out);
bool KeyChordEq(const KeyChord& a, const KeyChord& b);

const int kMaxContextDepth = 8;

const int kMaxStrokes = 3;

int KeyChordsParse(Str spec, KeyChord* out, int maxChords);

uint32_t ActionOf(Str name);

uint32_t KeyContextOf(Str name);

struct KeyBinding {
    const char* stroke = nullptr;
    uint32_t action = 0;
    const char* context = nullptr;

    intptr_t arg = 0;
};

void KeymapBind(const KeyBinding* bindings, int n);
void KeymapClear();

uint32_t KeymapGeneration();

struct KeyMatch {
    uint32_t action = 0;

    intptr_t arg = 0;
    bool pending = false;
};

KeyMatch KeymapMatch(const KeyChord& chord, const uint32_t* contexts,
                     int nContexts);

bool KeymapBindingForAction(uint32_t action, const uint32_t* contexts,
                            int nContexts, KeyChord* out);

bool KeymapAnyBindingForAction(uint32_t action, KeyChord* out);

Str KeyName(int vk);

bool KeymapPending();

void KeymapClearPending();

}

#line 1 "src/base/input_keys.h"

namespace gpui {

namespace input {

uint32_t Backspace();
uint32_t Copy();
uint32_t Cut();
uint32_t Delete();
uint32_t DeleteToBeginningOfLine();
uint32_t DeleteToEndOfLine();
uint32_t DeleteToNextWordEnd();
uint32_t DeleteToPreviousWordStart();
uint32_t Escape();
uint32_t Indent();
uint32_t IndentInline();
uint32_t MoveDown();
uint32_t MoveEnd();
uint32_t MoveHome();
uint32_t MoveLeft();
uint32_t MovePageDown();
uint32_t MovePageUp();
uint32_t MoveRight();
uint32_t MoveToEnd();
uint32_t MoveToNextWord();
uint32_t MoveToPreviousWord();
uint32_t MoveToStart();
uint32_t MoveUp();
uint32_t Outdent();
uint32_t OutdentInline();
uint32_t Paste();
uint32_t Redo();
uint32_t Replace();
uint32_t Search();
uint32_t SelectAll();
uint32_t SelectDown();
uint32_t SelectLeft();
uint32_t SelectRight();
uint32_t SelectToEnd();
uint32_t SelectToEndOfLine();
uint32_t SelectToNextWordEnd();
uint32_t SelectToPreviousWordStart();
uint32_t SelectToStart();
uint32_t SelectToStartOfLine();
uint32_t SelectUp();
uint32_t Undo();
uint32_t Enter();

}

Str InputContext();

void InputInitKeys();

InputAction InputActionOf(uint32_t id, intptr_t arg = 0);

constexpr bool InputEnterShift(intptr_t arg) {
    return (arg & 2) != 0;
}

}

#line 1 "src/fps/fps.h"

namespace gpui {

struct FpsStyle {
    Rgba background;
    Rgba foreground;
    Rgba muted;
    Rgba good;
    Rgba warn;
    Rgba bad;
};

const FpsStyle& FpsStyleDark();

Rgba FpsLevelColor(const FpsStyle& style, float frameSecs, float budgetSecs);

enum {

    kFpsCapacity = 120,

    kFpsArrivals = 512,
};

struct FrameSampler {
    float draws[kFpsCapacity] = {};
    int n = 0;
    int capacity = kFpsCapacity;
    double arrivals[kFpsArrivals] = {};
    int nArrivals = 0;
    uint64_t cursor = 0;
};

void FrameSamplerTick(FrameSampler* s, Window* win);

void FrameSamplerIngest(FrameSampler* s, const float* drawSecs, int n,
                        double now);
void FrameSamplerSetCapacity(FrameSampler* s, int capacity);

float FrameSamplerFps(const FrameSampler* s);
float FrameSamplerMeanDraw(const FrameSampler* s);
float FrameSamplerPeakDraw(const FrameSampler* s);

float FrameSamplerOverBudget(const FrameSampler* s, float budgetSecs);

struct ResourceSample {

    float cpuPercent = 0;
    uint64_t memoryBytes = 0;

    float gpuPercent = -1.f;
};

struct ResourceProbe {
    uint64_t prevCpu100ns = 0;
    double prevAt = 0;
    float cores = 1;
    bool primed = false;
};

bool ResourceProbeSample(ResourceProbe* probe, ResourceSample* out);

struct FpsReadout {
    float fps = 0;
    float frameMillis = 0;
    float droppedPercent = 0;
};

struct FpsMonitor {
    FrameSampler sampler;
    FpsReadout readout;
    double readoutAt = -1;

    float frameBudget = 1.f / 60.f;

    bool continuous = true;
    bool showResources = true;
    float resourceInterval = 0.5f;
    ResourceProbe probe;
    ResourceSample resources;
    bool hasResources = false;
    double resourcesAt = -1;
    bool compact = false;

    float axisMax = (1.f / 60.f) * 2.f;

    static El* Render(FpsMonitor* self, Ctx* cx);
    static void OnToggleCompact(FpsMonitor* self, Ctx* cx, const ClickEvent*);
};

enum class FpsAnchor : uint8_t {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    TopCenter,
    BottomCenter,
    LeftCenter,
    RightCenter,
};

El* FpsOverlayEl(Ctx* cx, Entity<FpsMonitor> monitor, FpsAnchor anchor);

El* FpsMonitorEl(Ctx* cx);

}

#line 1 "src/gpui/asset_icons.h"

namespace gpui {

struct AssetIcon {
    int offset;
    int len;
};

extern const uint8_t kAssetIconsData[];
extern const int kAssetIconsDataLen;

extern const char kAssetIconNames[];
extern const AssetIcon kAssetIcons[];
extern const int kAssetIconsCount;

const uint8_t* AssetIconFind(Str name, int* lenOut);

const uint8_t* AssetIconForPath(Str assetPath, int* lenOut);

}

#line 1 "src/gpui/image.h"

namespace gpui {

struct Image;

Image* ImageForSrc(PaintApp* pa, Str src);

const uint8_t* ImageVectorForSrc(Str src, int* lenOut);

bool ImageSrcIsLocal(Str src);

Str ImageAssetFor(Arena* a, Str src);

void ImageCacheClear();

}

#line 1 "src/gpui/paint.h"

namespace gpui {

enum {
    kFontWeightMask = 3,
    kFontWeightNormal = 0,
    kFontWeightSemibold = 1,
    kFontWeightBold = 2,
    kFontWeightMedium = 3,
    kFontMono = 4,
    kFontUnderline = 8,
    kFontItalic = 16,

    kFontStrike = 32
};

const float kLineHeight = 1.618034f;

PaintApp* PaintAppNew();
void PaintAppFree(PaintApp* pa);

bool PaintTargetBegin(PaintCtx* ctx, void* native, int pxW, int pxH);

bool PaintTargetBeginOffscreen(PaintCtx* ctx, int pxW, int pxH);

bool PaintTargetEndOffscreen(PaintCtx* ctx, uint8_t* outBgra);

bool PaintTargetEnd(PaintCtx* ctx);

void PaintTargetFree(PaintCtx* ctx);

inline Rgba PaintFade(const PaintCtx* ctx, Rgba c) {
    if (!ctx || ctx->opacity >= 1.f) {
        return c;
    }
    float a = (float)c.a * (ctx->opacity < 0 ? 0 : ctx->opacity);
    c.a = (uint8_t)(a <= 0 ? 0 : (a >= 255 ? 255 : a + 0.5f));
    return c;
}

void CanvasClear(PaintCtx* ctx, Rgba c);
void CanvasFillRect(PaintCtx* ctx, float x, float y, float w, float h, Rgba c);
void CanvasFillRound(PaintCtx* ctx, float x, float y, float w, float h, float r,
                     Rgba c);

void CanvasStrokeRound(PaintCtx* ctx, float x, float y, float w, float h,
                       float r, float stroke, Rgba c,
                       const float* dash = nullptr);
void CanvasLine(PaintCtx* ctx, float x1, float y1, float x2, float y2,
                float stroke, Rgba c, const float* dash = nullptr);

void CanvasEllipse(PaintCtx* ctx, float cx, float cy, float rx, float ry,
                   float stroke, Rgba c);
void CanvasPushClip(PaintCtx* ctx, float x, float y, float w, float h);
void CanvasPopClip(PaintCtx* ctx);

struct Path;

Path* PathNew(PaintCtx* ctx, bool winding);
void PathFree(Path* p);
void PathMoveTo(Path* p, float x, float y);
void PathLineTo(Path* p, float x, float y);
void PathCubicTo(Path* p, float x1, float y1, float x2, float y2, float x,
                 float y);

void PathArcTo(Path* p, float cx, float cy, float r, float a0, float a1,
               bool clockwise);
void PathClose(Path* p);

void PathFill(PaintCtx* ctx, Path* p, Rgba c);

void PathFillGradient(PaintCtx* ctx, Path* p, float x0, float y0, float x1,
                      float y1, Rgba from, Rgba to);
void PathFillGradientV(PaintCtx* ctx, Path* p, float y0, float y1, Rgba top,
                       Rgba bot);
void PathStroke(PaintCtx* ctx, Path* p, float stroke, Rgba c,
                bool roundCaps = false);

void PathRealize(PaintCtx* ctx, Path* p);

struct Image;

Image* ImageDecode(PaintApp* pa, const uint8_t* bytes, int len);
void ImageFree(Image* img);

Size ImageSizePx(const Image* img);

void ImageDraw(PaintCtx* ctx, Image* img, Bounds b, float radius = 0);

struct TextLayout;

TextLayout* TextLayoutNew(PaintCtx* ctx, Str s, float fontSize, float maxW,
                          bool wrap, uint8_t weight, float lineH,
                          Size* outSize);

Size TextLayoutSize(TextLayout* tl);
void TextLayoutAddRef(TextLayout* tl);
void TextLayoutRelease(TextLayout* tl);

void TextLayoutDraw(PaintCtx* ctx, TextLayout* tl, float x, float y, Rgba c,
                    bool clip, float clipW = 0);

int TextLayoutHitPoint(TextLayout* tl, Str s, float relX, float relY);

int TextLayoutRangeRects(TextLayout* tl, Str s, int u8a, int u8b, Bounds* out,
                         int max);

float TextLayoutBaseline(TextLayout* tl);

}

#line 1 "src/gpui/paintgpu.h"

#if GPUI_OS_WINDOWS

namespace gpui {

bool PaintGpuOn();

int PaintGpuSamples();

void* PaintSharedD3dDevice(PaintApp* pa);

void* PaintSharedDxgiFactory(PaintApp* pa);

void* PaintSharedDwrite(PaintApp* pa);

bool PaintImagePixels(const Image* img, const uint8_t** bgra, int* w, int* h);

namespace gpuw {

bool PaintTargetBegin(PaintCtx* ctx, void* native, int pxW, int pxH);
bool PaintTargetBeginOffscreen(PaintCtx* ctx, int pxW, int pxH);
bool PaintTargetEndOffscreen(PaintCtx* ctx, uint8_t* outBgra);
bool PaintTargetEnd(PaintCtx* ctx);
void PaintTargetFree(PaintCtx* ctx);

void CanvasClear(PaintCtx* ctx, Rgba c);
void CanvasFillRect(PaintCtx* ctx, float x, float y, float w, float h, Rgba c);
void CanvasFillRound(PaintCtx* ctx, float x, float y, float w, float h, float r,
                     Rgba c);
void CanvasStrokeRound(PaintCtx* ctx, float x, float y, float w, float h,
                       float r, float stroke, Rgba c, const float* dash);
void CanvasLine(PaintCtx* ctx, float x1, float y1, float x2, float y2,
                float stroke, Rgba c, const float* dash);
void CanvasEllipse(PaintCtx* ctx, float cx, float cy, float rx, float ry,
                   float stroke, Rgba c);
void CanvasPushClip(PaintCtx* ctx, float x, float y, float w, float h);
void CanvasPopClip(PaintCtx* ctx);

Path* PathNew(PaintCtx* ctx, bool winding);
void PathFree(Path* p);
void PathMoveTo(Path* p, float x, float y);
void PathLineTo(Path* p, float x, float y);
void PathCubicTo(Path* p, float x1, float y1, float x2, float y2, float x,
                 float y);
void PathArcTo(Path* p, float cx, float cy, float r, float a0, float a1,
               bool clockwise);
void PathClose(Path* p);
void PathFill(PaintCtx* ctx, Path* p, Rgba c);
void PathFillGradient(PaintCtx* ctx, Path* p, float x0, float y0, float x1,
                      float y1, Rgba from, Rgba to);
void PathStroke(PaintCtx* ctx, Path* p, float stroke, Rgba c, bool roundCaps);
void PathRealize(PaintCtx* ctx, Path* p);

void ImageDraw(PaintCtx* ctx, Image* img, Bounds b, float radius);
void TextLayoutDraw(PaintCtx* ctx, TextLayout* tl, float x, float y, Rgba c,
                    bool clip, float clipW);

struct FrameStats {
    int instances = 0;
    int draws = 0;
    int glyphsRasterized = 0;
    int pathTriangles = 0;
};
const FrameStats& LastFrameStats();

}

}

#endif

#line 1 "src/gpui/platform.h"

namespace gpui {

void WindowDrawFrame(Window* win, void* native, int pxW, int pxH, float dipW,
                     float dipH);

bool PlatReduceMotion();

void WindowKeyDown(Window* win, int key, bool shift, bool ctrl, bool alt,
                   bool platform = false);

void WindowKeyUp(Window* win, int key, bool shift, bool ctrl, bool alt,
                 bool platform = false);

void WindowChar(Window* win, uint32_t ch, bool ctrl, bool alt);

void WindowDispatchInput(Window* win, const PlatformInput* input);

PlatformInput InputMouseDown(MouseButton button, float x, float y,
                             Modifiers modifiers, int clickCount,
                             bool firstMouse);
PlatformInput InputMouseUp(MouseButton button, float x, float y,
                           Modifiers modifiers, int clickCount);
PlatformInput InputMouseMove(float x, float y, bool pressed,
                             MouseButton pressedButton, Modifiers modifiers);
PlatformInput InputMouseExited(float x, float y, bool pressed,
                               MouseButton pressedButton, Modifiers modifiers);
PlatformInput InputScrollWheel(float x, float y, float deltaX, float deltaY,
                               bool precise, Modifiers modifiers,
                               TouchPhase phase);

int WindowClickCount(Window* win, float x, float y, MouseButton button);

int WindowCurrentClickCount(Window* win);

void WindowTimerTick(Window* win);

int WindowChromeHit(Window* win, float x, float y);

int WindowTimerMs(Window* win);

void WindowClosed(Window* win);
bool AppAnyWindowOpen(App* app);

Window* WindowAlloc(App* app, WinOpts opts);

void WindowClampToDisplay(int* dipW, int* dipH, int screenW, int screenH);

bool WindowGeomRequested(int* x, int* y, int* w, int* h);

int GpuiTakeRuntimeArgs(int argc, char** argv);

bool PlatInit(App* app);
void PlatShutdown(App* app);

void PlatSetTimer(Window* win, int ms);

void PlatWake(App* app);

void PlatSetCursor(Window* win, CursorKind kind);

void PlatSetMouseCapture(Window* win, bool capture);

int PlatDoubleClickMs();

void* PlatWindowHandle(Window* win);

void PlatInstallAccessibilityHitTest(Window* win);

struct PlatMenuItem {
    const char* label = nullptr;

    int id = 0;
    bool separator = false;
    bool disabled = false;
    bool checked = false;

    const char* iconPath = nullptr;
    const PlatMenuItem* submenu = nullptr;
    int submenuN = 0;

    const char* key = nullptr;
    Modifiers keyMods = {};
};

bool PlatHasMenu();

int PlatShowMenu(Window* win, const PlatMenuItem* items, int n, float x,
                 float y, bool dark);

bool PlatHasAppMenu();

void PlatSetAppMenu(App* app, const PlatMenuItem* items, int n);

void AppMenuChosen(int id);

}

#line 1 "src/gpui/scene.h"

namespace gpui {

enum SceneLevel : int {
    kSceneOff = 0,
    kSceneReplay = 1,
    kSceneCache = 2,
    kSceneSkip = 3,
    kSceneDamage = 4
};

int SceneLevelOn();
inline bool SceneOn() {
    return SceneLevelOn() > kSceneOff;
}

namespace scene {

bool Recording();

void FrameBegin(PaintCtx* ctx);

bool FrameEnd(PaintCtx* ctx, Bounds* damage);

void Replay(PaintCtx* ctx, const Bounds* damage);

bool SuspendBegin();
void SuspendEnd(bool prev);

bool SkipPresent();

void Invalidate();

void Reset();

void RecClear(PaintCtx* ctx, Rgba c);
void RecFillRect(PaintCtx* ctx, float x, float y, float w, float h, Rgba c);
void RecFillRound(PaintCtx* ctx, float x, float y, float w, float h, float r,
                  Rgba c);
void RecStrokeRound(PaintCtx* ctx, float x, float y, float w, float h, float r,
                    float stroke, Rgba c, const float* dash);
void RecLine(PaintCtx* ctx, float x1, float y1, float x2, float y2,
             float stroke, Rgba c, const float* dash);
void RecEllipse(PaintCtx* ctx, float cx, float cy, float rx, float ry,
                float stroke, Rgba c);
void RecPushClip(PaintCtx* ctx, float x, float y, float w, float h);
void RecPopClip(PaintCtx* ctx);

Path* RecPathNew(bool winding);
void RecPathFree(Path* p);
void RecPathMoveTo(Path* p, float x, float y);
void RecPathLineTo(Path* p, float x, float y);
void RecPathCubicTo(Path* p, float x1, float y1, float x2, float y2, float x,
                    float y);
void RecPathArcTo(Path* p, float cx, float cy, float r, float a0, float a1,
                  bool clockwise);
void RecPathClose(Path* p);
void RecPathFill(PaintCtx* ctx, Path* p, Rgba c);
void RecPathFillGradient(PaintCtx* ctx, Path* p, float x0, float y0, float x1,
                         float y1, Rgba from, Rgba to);
void RecPathStroke(PaintCtx* ctx, Path* p, float stroke, Rgba c,
                   bool roundCaps);

void RecImageDraw(PaintCtx* ctx, Image* img, Bounds b, float radius);
void RecTextDraw(PaintCtx* ctx, TextLayout* tl, float x, float y, Rgba c,
                 bool clip, float clipW);

struct SceneStats {

    int prims = 0;
    int layers = 0;

    int maskChanges = 0;

    int culled = 0;

    int clipPushes = 0;
    int pathPrims = 0;
    int pathVerbs = 0;

    int pathCacheHits = 0;
    int pathCacheMisses = 0;
    int pathCacheLive = 0;

    int frames = 0;
    int framesUnchanged = 0;
    int framesPartial = 0;

    float damageFracSum = 0;

    int primsChanged = 0;
};
const SceneStats& Stats();

}

}

#line 1 "src/markdown/constant.h"

#ifndef GPUI_MARKDOWN_CONSTANT_H_
#define GPUI_MARKDOWN_CONSTANT_H_

namespace markdown {

using base::SeqStrAdvance;
using base::SeqStrAt;
using base::SeqStrings;
using base::Str;

constexpr int kAutolinkSchemeSizeMax = 32;

constexpr int kAutolinkDomainSizeMax = 63;

constexpr int kCharacterReferenceDecimalSizeMax = 7;

constexpr int kCharacterReferenceHexadecimalSizeMax = 6;

constexpr int kCharacterReferenceNamedSizeMax = 31;

constexpr int kCodeFencedSequenceSizeMin = 3;

constexpr int kFrontmatterSequenceSize = 3;

constexpr int kHardBreakPrefixSizeMin = 2;

constexpr int kHeadingAtxOpeningFenceSizeMax = 6;

extern const Str kHtmlCdataPrefix;

constexpr int kHtmlRawSizeMax = 8;

constexpr int kLinkReferenceSizeMax = 999;

constexpr int kListItemValueSizeMax = 10;

constexpr int kMathFlowSequenceSizeMin = 2;

constexpr int kResourceDestinationBalanceMax = 32;

constexpr int kTabSize = 4;

constexpr int kThematicBreakMarkerCountMin = 3;

extern const char kHtmlBlockNames[];
extern const char kHtmlRawNames[];

struct CharacterReference {
    int32_t nameOff;
    int32_t valueOff;
};
extern const char kCharacterReferenceNames[];
extern const char kCharacterReferenceValues[];
extern const CharacterReference kCharacterReferences[2125];

}

#endif

#line 1 "src/markdown/mdast.h"

#ifndef GPUI_MARKDOWN_MDAST_H_
#define GPUI_MARKDOWN_MDAST_H_

namespace markdown {

using base::Arena;
using base::ArenaPtr;
using base::ArenaPtrGet;
using base::ArenaPtrOf;
using base::ArenaStr;
using base::ArenaStrGet;
using base::ArenaVec;
using base::kArenaStrNone;
using base::Str;

struct Node;

using ArenaNode = ArenaPtr<Node>;

struct UnistPoint {
    int32_t line = 1;
    int32_t column = 1;
    int32_t offset = 0;
};

struct UnistPosition {
    UnistPoint start = {};
    UnistPoint end = {};
};

UnistPosition GetUnistPosition(Str md, uint32_t start, uint32_t end);

enum class ReferenceKind : uint8_t {
    Shortcut,
    Collapsed,
    Full,
};

enum class AlignKind : uint8_t {
    Left,
    Right,
    Center,
    None,
};

using ArenaAlign = uint32_t;
constexpr ArenaAlign kArenaAlignNone = 0;

static_assert((int)AlignKind::None < 4, "an alignment has to fit in 2 bits");

ArenaAlign ArenaAlignNew(Arena* a, int32_t count);
int32_t ArenaAlignCount(Arena* a, ArenaAlign al);
AlignKind ArenaAlignAt(Arena* a, ArenaAlign al, int32_t i);
void ArenaAlignSet(Arena* a, ArenaAlign al, int32_t i, AlignKind k);

enum class NodeKind : uint8_t {
    Root,
    Blockquote,
    FootnoteDefinition,
    List,
    Toml,
    Yaml,
    Break,
    InlineCode,
    InlineMath,
    Delete,
    Emphasis,
    FootnoteReference,
    Html,
    Image,
    ImageReference,
    Link,
    LinkReference,
    Strong,
    Text,
    Code,
    Math,
    Heading,
    Table,
    ThematicBreak,
    TableRow,
    TableCell,
    ListItem,
    Definition,
    Paragraph,
};

enum class NodeStrKind : uint8_t {

    Value,

    Url,

    Title,

    Alt,

    Identifier,

    Label,

    Lang,
    Meta,

    PerKind,
};

enum NodeFlag : uint8_t {

    NodeHasStart = 1 << 0,

    NodeOrdered = 1 << 1,

    NodeSpread = 1 << 2,

    NodeChecked = 1 << 3,
    NodeHasChecked = 1 << 4,

    NodeRefKindMask = 3 << 6,
};

struct Node {

    ArenaNode lastKid = {};

    ArenaNode sibling = {};

    ArenaStr firstStr = kArenaStrNone;

    NodeKind kind = NodeKind::Root;

    uint8_t flags = 0;

    bool Has(NodeFlag f) const { return (flags & f) != 0; }
    void Set(NodeFlag f, bool on) {
        flags = on ? (uint8_t)(flags | f) : (uint8_t)(flags & ~f);
    }
};

static_assert(sizeof(Node) == 3 * 4 + 4,
              "Node has picked up padding; order the fields largest first");

static_assert(alignof(Node) == 4, "a Node holds nothing wider than a word");

uint32_t NodePerKind(Arena* a, const Node* n);
void NodeSetPerKind(Arena* a, Node* n, uint32_t word);

Str NodeGetStr(Arena* a, const Node* n, NodeStrKind k);

int32_t NodeGetStrLen(Arena* a, const Node* n, NodeStrKind k);

bool NodeHasStr(Arena* a, const Node* n, NodeStrKind k);

void NodeSetStr(Arena* a, Node* n, NodeStrKind k, Str s);

void NodeClearStr(Arena* a, Node* n, NodeStrKind k);

void NodeGrowStr(Arena* a, Node* n, NodeStrKind k, Str more);

inline ReferenceKind NodeRefKind(const Node* n) {
    return (ReferenceKind)((n->flags & NodeRefKindMask) >> 6);
}

inline void NodeSetRefKind(Node* n, ReferenceKind k) {
    n->flags = (uint8_t)((n->flags & ~NodeRefKindMask) |
                         (((uint8_t)k & 3) << 6));
}

Node* NodeNew(Arena* a, NodeKind kind);

inline void NodeAddChild(Arena* a, Node* parent, Node* child) {
    ArenaNode at = ArenaPtrOf(a, child);
    if (parent->lastKid.IsSet()) {
        Node* last = ArenaPtrGet(a, parent->lastKid);
        child->sibling = last->sibling;
        last->sibling = at;
    } else {

        child->sibling = at;
    }
    parent->lastKid = at;
}

inline Node* NodeLastChild(Arena* a, const Node* n) {
    return n->lastKid.IsSet() ? ArenaPtrGet(a, n->lastKid) : nullptr;
}

inline ArenaNode NodeFirstKid(Arena* a, const Node* n) {
    Node* last = NodeLastChild(a, n);
    return last ? last->sibling : ArenaNode{};
}

inline Node* NodeFirstChild(Arena* a, const Node* n) {
    ArenaNode first = NodeFirstKid(a, n);
    return first.IsSet() ? ArenaPtrGet(a, first) : nullptr;
}

inline Node* NodeChild(Arena* a, const Node* n, int i) {
    if (i < 0 || !n->lastKid.IsSet()) {
        return nullptr;
    }
    Node* last = ArenaPtrGet(a, n->lastKid);
    Node* at = ArenaPtrGet(a, last->sibling);
    for (int k = 0; k < i; k++) {
        if (at == last) {
            return nullptr;
        }
        at = ArenaPtrGet(a, at->sibling);
    }
    return at;
}

inline int NodeChildCount(Arena* a, const Node* n) {
    if (!n->lastKid.IsSet()) {
        return 0;
    }
    Node* last = ArenaPtrGet(a, n->lastKid);
    int count = 1;
    for (Node* at = ArenaPtrGet(a, last->sibling); at != last;
         at = ArenaPtrGet(a, at->sibling)) {
        count++;
    }
    return count;
}

struct NodeKidsRange {
    Arena* a;
    ArenaNode at;
    ArenaNode last;

    struct Iter {
        Arena* a;
        ArenaNode at;
        ArenaNode last;

        Node* operator*() const { return ArenaPtrGet(a, at); }
        Iter& operator++() {
            at = at == last ? ArenaNode{} : ArenaPtrGet(a, at)->sibling;
            return *this;
        }
        bool operator!=(const Iter& o) const { return at != o.at; }
    };

    Iter begin() const { return Iter{a, at, last}; }
    Iter end() const { return Iter{a, ArenaNode{}, last}; }
};

inline NodeKidsRange NodeKids(Arena* a, const Node* n) {
    return NodeKidsRange{a, NodeFirstKid(a, n), n->lastKid};
}

inline Str NodeStr(Arena* a, ArenaStr s) {
    return ArenaStrGet(a, s);
}

bool NodeHasChildren(NodeKind kind);

Str NodeToString(Arena* a, const Node* node);

}

#endif

#line 1 "src/markdown/markdown.h"

#ifndef GPUI_MARKDOWN_MARKDOWN_H_
#define GPUI_MARKDOWN_MARKDOWN_H_

namespace markdown {

struct Constructs {
    bool attention = true;
    bool autolink = true;
    bool blockQuote = true;
    bool characterEscape = true;
    bool characterReference = true;
    bool codeIndented = true;
    bool codeFenced = true;
    bool codeText = true;
    bool definition = true;
    bool frontmatter = false;
    bool gfmAutolinkLiteral = false;
    bool gfmFootnoteDefinition = false;
    bool gfmLabelStartFootnote = false;
    bool gfmStrikethrough = false;
    bool gfmTable = false;
    bool gfmTaskListItem = false;
    bool hardBreakEscape = true;
    bool hardBreakTrailing = true;
    bool headingAtx = true;
    bool headingSetext = true;
    bool htmlFlow = true;
    bool htmlText = true;
    bool labelStartImage = true;
    bool labelStartLink = true;
    bool labelEnd = true;
    bool listItem = true;
    bool mathFlow = false;
    bool mathText = false;
    bool thematicBreak = true;

    static Constructs Gfm();
};

struct ParseOptions {
    Constructs constructs = {};
    bool gfmStrikethroughSingleTilde = true;
    bool mathTextSingleDollar = true;

    static ParseOptions Gfm();
};

Node* ToMdast(Arena* a, Str source, const ParseOptions& options);

Str DecodeNamed(Arena* a, Str name);

Str DecodeNumeric(Arena* a, Str value, int radix);

}

#endif

#line 1 "src/markdown/state.h"

#ifndef GPUI_MARKDOWN_STATE_H_
#define GPUI_MARKDOWN_STATE_H_

namespace markdown {

struct Tokenizer;

enum class StateName : uint16_t {
    AttentionStart,
    AttentionInside,
    AutolinkStart,
    AutolinkOpen,
    AutolinkSchemeOrEmailAtext,
    AutolinkSchemeInsideOrEmailAtext,
    AutolinkUrlInside,
    AutolinkEmailAtSignOrDot,
    AutolinkEmailAtext,
    AutolinkEmailValue,
    AutolinkEmailLabel,
    BlankLineStart,
    BlankLineAfter,
    BlockQuoteStart,
    BlockQuoteContStart,
    BlockQuoteContBefore,
    BlockQuoteContAfter,
    BomStart,
    BomInside,
    CharacterEscapeStart,
    CharacterEscapeInside,
    CharacterReferenceStart,
    CharacterReferenceOpen,
    CharacterReferenceNumeric,
    CharacterReferenceValue,
    CodeIndentedStart,
    CodeIndentedAtBreak,
    CodeIndentedAfter,
    CodeIndentedFurtherStart,
    CodeIndentedInside,
    CodeIndentedFurtherBegin,
    CodeIndentedFurtherAfter,
    ContentChunkStart,
    ContentChunkInside,
    ContentDefinitionBefore,
    ContentDefinitionAfter,
    DataStart,
    DataInside,
    DataAtBreak,
    DefinitionStart,
    DefinitionBefore,
    DefinitionLabelAfter,
    DefinitionLabelNok,
    DefinitionMarkerAfter,
    DefinitionDestinationBefore,
    DefinitionDestinationAfter,
    DefinitionDestinationMissing,
    DefinitionTitleBefore,
    DefinitionAfter,
    DefinitionAfterWhitespace,
    DefinitionTitleBeforeMarker,
    DefinitionTitleAfter,
    DefinitionTitleAfterOptionalWhitespace,
    DestinationStart,
    DestinationEnclosedBefore,
    DestinationEnclosed,
    DestinationEnclosedEscape,
    DestinationRaw,
    DestinationRawEscape,
    DocumentStart,
    DocumentBeforeFrontmatter,
    DocumentContainerExistingBefore,
    DocumentContainerExistingAfter,
    DocumentContainerNewBefore,
    DocumentContainerNewBeforeNotBlockQuote,
    DocumentContainerNewBeforeNotList,
    DocumentContainerNewBeforeNotGfmFootnoteDefinition,
    DocumentContainerNewAfter,
    DocumentContainersAfter,
    DocumentFlowInside,
    DocumentFlowEnd,
    FlowStart,
    FlowBeforeGfmTable,
    FlowBeforeCodeIndented,
    FlowBeforeRaw,
    FlowBeforeHtml,
    FlowBeforeHeadingAtx,
    FlowBeforeHeadingSetext,
    FlowBeforeThematicBreak,
    FlowAfter,
    FlowBlankLineBefore,
    FlowBlankLineAfter,
    FlowBeforeContent,
    FrontmatterStart,
    FrontmatterOpenSequence,
    FrontmatterOpenAfter,
    FrontmatterAfter,
    FrontmatterContentStart,
    FrontmatterContentInside,
    FrontmatterContentEnd,
    FrontmatterCloseStart,
    FrontmatterCloseSequence,
    FrontmatterCloseAfter,
    GfmAutolinkLiteralProtocolStart,
    GfmAutolinkLiteralProtocolAfter,
    GfmAutolinkLiteralProtocolPrefixInside,
    GfmAutolinkLiteralProtocolSlashesInside,
    GfmAutolinkLiteralWwwStart,
    GfmAutolinkLiteralWwwAfter,
    GfmAutolinkLiteralWwwPrefixInside,
    GfmAutolinkLiteralWwwPrefixAfter,
    GfmAutolinkLiteralDomainInside,
    GfmAutolinkLiteralDomainAtPunctuation,
    GfmAutolinkLiteralDomainAfter,
    GfmAutolinkLiteralPathInside,
    GfmAutolinkLiteralPathAtPunctuation,
    GfmAutolinkLiteralPathAfter,
    GfmAutolinkLiteralTrail,
    GfmAutolinkLiteralTrailCharRefInside,
    GfmAutolinkLiteralTrailCharRefStart,
    GfmAutolinkLiteralTrailBracketAfter,
    GfmFootnoteDefinitionStart,
    GfmFootnoteDefinitionLabelBefore,
    GfmFootnoteDefinitionLabelAtMarker,
    GfmFootnoteDefinitionLabelInside,
    GfmFootnoteDefinitionLabelEscape,
    GfmFootnoteDefinitionLabelAfter,
    GfmFootnoteDefinitionWhitespaceAfter,
    GfmFootnoteDefinitionContStart,
    GfmFootnoteDefinitionContBlank,
    GfmFootnoteDefinitionContFilled,
    GfmLabelStartFootnoteStart,
    GfmLabelStartFootnoteOpen,
    GfmTaskListItemCheckStart,
    GfmTaskListItemCheckInside,
    GfmTaskListItemCheckClose,
    GfmTaskListItemCheckAfter,
    GfmTaskListItemCheckAfterSpaceOrTab,
    GfmTableStart,
    GfmTableHeadRowBefore,
    GfmTableHeadRowStart,
    GfmTableHeadRowBreak,
    GfmTableHeadRowData,
    GfmTableHeadRowEscape,
    GfmTableHeadDelimiterStart,
    GfmTableHeadDelimiterBefore,
    GfmTableHeadDelimiterCellBefore,
    GfmTableHeadDelimiterValueBefore,
    GfmTableHeadDelimiterLeftAlignmentAfter,
    GfmTableHeadDelimiterFiller,
    GfmTableHeadDelimiterRightAlignmentAfter,
    GfmTableHeadDelimiterCellAfter,
    GfmTableHeadDelimiterNok,
    GfmTableBodyRowStart,
    GfmTableBodyRowBreak,
    GfmTableBodyRowData,
    GfmTableBodyRowEscape,
    HardBreakEscapeStart,
    HardBreakEscapeAfter,
    HeadingAtxStart,
    HeadingAtxBefore,
    HeadingAtxSequenceOpen,
    HeadingAtxAtBreak,
    HeadingAtxSequenceFurther,
    HeadingAtxData,
    HeadingSetextStart,
    HeadingSetextBefore,
    HeadingSetextInside,
    HeadingSetextAfter,
    HtmlFlowStart,
    HtmlFlowBefore,
    HtmlFlowOpen,
    HtmlFlowDeclarationOpen,
    HtmlFlowCommentOpenInside,
    HtmlFlowCdataOpenInside,
    HtmlFlowTagCloseStart,
    HtmlFlowTagName,
    HtmlFlowBasicSelfClosing,
    HtmlFlowCompleteClosingTagAfter,
    HtmlFlowCompleteEnd,
    HtmlFlowCompleteAttributeNameBefore,
    HtmlFlowCompleteAttributeName,
    HtmlFlowCompleteAttributeNameAfter,
    HtmlFlowCompleteAttributeValueBefore,
    HtmlFlowCompleteAttributeValueQuoted,
    HtmlFlowCompleteAttributeValueQuotedAfter,
    HtmlFlowCompleteAttributeValueUnquoted,
    HtmlFlowCompleteAfter,
    HtmlFlowBlankLineBefore,
    HtmlFlowContinuation,
    HtmlFlowContinuationDeclarationInside,
    HtmlFlowContinuationAfter,
    HtmlFlowContinuationStart,
    HtmlFlowContinuationBefore,
    HtmlFlowContinuationCommentInside,
    HtmlFlowContinuationRawTagOpen,
    HtmlFlowContinuationRawEndTag,
    HtmlFlowContinuationClose,
    HtmlFlowContinuationCdataInside,
    HtmlFlowContinuationStartNonLazy,
    HtmlTextStart,
    HtmlTextOpen,
    HtmlTextDeclarationOpen,
    HtmlTextTagCloseStart,
    HtmlTextTagClose,
    HtmlTextTagCloseBetween,
    HtmlTextTagOpen,
    HtmlTextTagOpenBetween,
    HtmlTextTagOpenAttributeName,
    HtmlTextTagOpenAttributeNameAfter,
    HtmlTextTagOpenAttributeValueBefore,
    HtmlTextTagOpenAttributeValueQuoted,
    HtmlTextTagOpenAttributeValueQuotedAfter,
    HtmlTextTagOpenAttributeValueUnquoted,
    HtmlTextCdata,
    HtmlTextCdataOpenInside,
    HtmlTextCdataClose,
    HtmlTextCdataEnd,
    HtmlTextCommentOpenInside,
    HtmlTextComment,
    HtmlTextCommentClose,
    HtmlTextCommentEnd,
    HtmlTextDeclaration,
    HtmlTextEnd,
    HtmlTextInstruction,
    HtmlTextInstructionClose,
    HtmlTextLineEndingBefore,
    HtmlTextLineEndingAfter,
    HtmlTextLineEndingAfterPrefix,
    LabelStart,
    LabelAtBreak,
    LabelEolAfter,
    LabelEscape,
    LabelInside,
    LabelNok,
    LabelEndStart,
    LabelEndAfter,
    LabelEndResourceStart,
    LabelEndResourceBefore,
    LabelEndResourceOpen,
    LabelEndResourceDestinationAfter,
    LabelEndResourceDestinationMissing,
    LabelEndResourceBetween,
    LabelEndResourceTitleAfter,
    LabelEndResourceEnd,
    LabelEndOk,
    LabelEndNok,
    LabelEndReferenceFull,
    LabelEndReferenceFullAfter,
    LabelEndReferenceFullMissing,
    LabelEndReferenceNotFull,
    LabelEndReferenceCollapsed,
    LabelEndReferenceCollapsedOpen,
    LabelStartImageStart,
    LabelStartImageOpen,
    LabelStartImageAfter,
    LabelStartLinkStart,
    ListItemStart,
    ListItemBefore,
    ListItemBeforeOrdered,
    ListItemBeforeUnordered,
    ListItemValue,
    ListItemMarker,
    ListItemMarkerAfter,
    ListItemAfter,
    ListItemMarkerAfterFilled,
    ListItemWhitespace,
    ListItemPrefixOther,
    ListItemWhitespaceAfter,
    ListItemContStart,
    ListItemContBlank,
    ListItemContFilled,
    NonLazyContinuationStart,
    NonLazyContinuationAfter,
    ParagraphStart,
    ParagraphLineStart,
    ParagraphInside,
    RawFlowStart,
    RawFlowBeforeSequenceOpen,
    RawFlowSequenceOpen,
    RawFlowInfoBefore,
    RawFlowInfo,
    RawFlowMetaBefore,
    RawFlowMeta,
    RawFlowAtNonLazyBreak,
    RawFlowCloseStart,
    RawFlowBeforeSequenceClose,
    RawFlowSequenceClose,
    RawFlowAfterSequenceClose,
    RawFlowContentBefore,
    RawFlowContentStart,
    RawFlowBeforeContentChunk,
    RawFlowContentChunk,
    RawFlowAfter,
    RawTextStart,
    RawTextSequenceOpen,
    RawTextBetween,
    RawTextData,
    RawTextSequenceClose,
    SpaceOrTabStart,
    SpaceOrTabInside,
    SpaceOrTabAfter,
    SpaceOrTabEolStart,
    SpaceOrTabEolAfterFirst,
    SpaceOrTabEolAfterEol,
    SpaceOrTabEolAtEol,
    SpaceOrTabEolAfterMore,
    StringStart,
    StringBefore,
    StringBeforeData,
    TextStart,
    TextBefore,
    TextBeforeHtml,
    TextBeforeHardBreakEscape,
    TextBeforeLabelStartLink,
    TextBeforeData,
    ThematicBreakStart,
    ThematicBreakBefore,
    ThematicBreakSequence,
    ThematicBreakAtBreak,
    TitleStart,
    TitleBegin,
    TitleAfterEol,
    TitleAtBreak,
    TitleEscape,
    TitleInside,
    TitleNok,
};

struct State {
    enum class Kind : uint8_t {
        Ok,
        Nok,
        Next,
        Retry,
    };

    Kind kind = Kind::Ok;
    StateName name = StateName::AttentionStart;

    bool operator==(const State& other) const {
        if (kind != other.kind) {
            return false;
        }
        if (kind == Kind::Next || kind == Kind::Retry) {
            return name == other.name;
        }
        return true;
    }
    bool operator!=(const State& other) const { return !(*this == other); }
};

inline State StateNext(StateName name) {
    return State{State::Kind::Next, name};
}
inline State StateRetry(StateName name) {
    return State{State::Kind::Retry, name};
}
inline State StateOk() {
    return State{State::Kind::Ok, StateName::AttentionStart};
}
inline State StateNok() {
    return State{State::Kind::Nok, StateName::AttentionStart};
}

State Call(Tokenizer* tokenizer, StateName name);

}

#endif

#line 1 "src/markdown/event.h"

#ifndef GPUI_MARKDOWN_EVENT_H_
#define GPUI_MARKDOWN_EVENT_H_

namespace markdown {

using base::Str;
using base::Vec;

enum class Name : uint8_t {
    AttentionSequence,
    Autolink,
    AutolinkEmail,
    AutolinkMarker,
    AutolinkProtocol,
    BlankLineEnding,
    BlockQuote,
    BlockQuoteMarker,
    BlockQuotePrefix,
    ByteOrderMark,
    CharacterEscape,
    CharacterEscapeMarker,
    CharacterEscapeValue,
    CharacterReference,
    CharacterReferenceMarker,
    CharacterReferenceMarkerHexadecimal,
    CharacterReferenceMarkerNumeric,
    CharacterReferenceMarkerSemi,
    CharacterReferenceValue,
    CodeFenced,
    CodeFencedFence,
    CodeFencedFenceInfo,
    CodeFencedFenceMeta,
    CodeFencedFenceSequence,
    CodeFlowChunk,
    CodeIndented,
    CodeText,
    CodeTextData,
    CodeTextSequence,
    Content,
    Data,
    Definition,
    DefinitionDestination,
    DefinitionDestinationLiteral,
    DefinitionDestinationLiteralMarker,
    DefinitionDestinationRaw,
    DefinitionDestinationString,
    DefinitionLabel,
    DefinitionLabelMarker,
    DefinitionLabelString,
    DefinitionMarker,
    DefinitionTitle,
    DefinitionTitleMarker,
    DefinitionTitleString,
    Emphasis,
    EmphasisSequence,
    EmphasisText,
    Frontmatter,
    FrontmatterChunk,
    FrontmatterFence,
    FrontmatterSequence,
    GfmAutolinkLiteralEmail,
    GfmAutolinkLiteralMailto,
    GfmAutolinkLiteralProtocol,
    GfmAutolinkLiteralWww,
    GfmAutolinkLiteralXmpp,
    GfmFootnoteCall,
    GfmFootnoteCallLabel,
    GfmFootnoteCallMarker,
    GfmFootnoteDefinition,
    GfmFootnoteDefinitionPrefix,
    GfmFootnoteDefinitionLabel,
    GfmFootnoteDefinitionLabelMarker,
    GfmFootnoteDefinitionLabelString,
    GfmFootnoteDefinitionMarker,
    GfmStrikethrough,
    GfmStrikethroughSequence,
    GfmStrikethroughText,
    GfmTable,
    GfmTableBody,
    GfmTableCell,
    GfmTableCellText,
    GfmTableCellDivider,
    GfmTableDelimiterRow,
    GfmTableDelimiterMarker,
    GfmTableDelimiterCell,
    GfmTableDelimiterCellValue,
    GfmTableDelimiterFiller,
    GfmTableHead,
    GfmTableRow,
    GfmTaskListItemCheck,
    GfmTaskListItemMarker,
    GfmTaskListItemValueChecked,
    GfmTaskListItemValueUnchecked,
    HardBreakEscape,
    HardBreakTrailing,
    HeadingAtx,
    HeadingAtxSequence,
    HeadingAtxText,
    HeadingSetext,
    HeadingSetextText,
    HeadingSetextUnderline,
    HeadingSetextUnderlineSequence,
    HtmlFlow,
    HtmlFlowData,
    HtmlText,
    HtmlTextData,
    Image,
    Label,
    LabelEnd,
    LabelImage,
    LabelImageMarker,
    LabelLink,
    LabelMarker,
    LabelText,
    LineEnding,
    Link,
    ListItem,
    ListItemMarker,
    ListItemPrefix,
    ListItemValue,
    ListOrdered,
    ListUnordered,
    MathFlow,
    MathFlowFence,
    MathFlowFenceMeta,
    MathFlowFenceSequence,
    MathFlowChunk,
    MathText,
    MathTextData,
    MathTextSequence,
    Paragraph,
    Reference,
    ReferenceMarker,
    ReferenceString,
    Resource,
    ResourceDestination,
    ResourceDestinationLiteral,
    ResourceDestinationLiteralMarker,
    ResourceDestinationRaw,
    ResourceDestinationString,
    ResourceMarker,
    ResourceTitle,
    ResourceTitleMarker,
    ResourceTitleString,
    SpaceOrTab,
    Strong,
    StrongSequence,
    StrongText,
    ThematicBreak,
    ThematicBreakSequence,
    LinePrefix,
};

bool IsVoidEvent(Name name);

enum class ContentKind : uint8_t {
    Flow,
    Content,
    String,
    Text,
};

struct Link {
    int32_t previous = -1;
    int32_t next = -1;
    ContentKind content = ContentKind::Flow;
};

struct Point {
    int32_t line = 1;
    int32_t column = 1;
    int32_t index = 0;
    int32_t vs = 0;
};

Point PointShiftTo(const Point& from, Str bytes, int32_t index);

enum class Kind : uint8_t {
    Enter,
    Exit,
};

struct Event {
    Kind kind = Kind::Enter;
    Name name = Name::Data;
    bool hasLink = false;
    Point point = {};
    Link link = {};
};

}

#endif

#line 1 "src/markdown/util.h"

#ifndef GPUI_MARKDOWN_UTIL_H_
#define GPUI_MARKDOWN_UTIL_H_

namespace markdown {

using base::Arena;
using base::ArenaVec;

Str StrOwn(Arena* a, Str s);
Str StrOwn(Arena* a, const char* s, int32_t len);

bool StrEq(Str a, Str b);

bool StrEqAsciiI(Str a, Str b);

enum class CharKind : uint8_t {
    Whitespace,
    Punctuation,
    Other,
};

bool IsUnicodePunctuation(uint32_t cp);

int32_t CharBeforeIndex(Str bytes, int32_t index);
int32_t CharAfterIndex(Str bytes, int32_t index);

CharKind Classify(int32_t cp);
CharKind KindAfterIndex(Str bytes, int32_t index);

inline bool IsAsciiWhitespace(uint8_t b) {
    return b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == 0x0c;
}
inline bool IsAsciiPunctuation(uint8_t b) {
    return (b >= '!' && b <= '/') || (b >= ':' && b <= '@') ||
           (b >= '[' && b <= '`') || (b >= '{' && b <= '~');
}
inline bool IsAsciiDigit(uint8_t b) {
    return b >= '0' && b <= '9';
}
inline bool IsAsciiHexDigit(uint8_t b) {
    return IsAsciiDigit(b) || (b >= 'a' && b <= 'f') || (b >= 'A' && b <= 'F');
}
inline bool IsAsciiAlpha(uint8_t b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z');
}
inline bool IsAsciiAlphanumeric(uint8_t b) {
    return IsAsciiAlpha(b) || IsAsciiDigit(b);
}
inline bool IsAsciiControl(uint8_t b) {
    return b < 0x20 || b == 0x7f;
}

int32_t Utf8Encode(char* out, uint32_t cp);

struct Position {
    Point start = {};
    Point end = {};
};

Position PositionFromExitEvent(const Vec<Event>& events, int32_t index);

struct Slice {
    Str bytes = {};
    int32_t before = 0;
    int32_t after = 0;

    int32_t Len() const { return bytes.len + before + after; }
};

Slice SliceFromPosition(Str bytes, const Position& position);
Slice SliceFromIndices(Str bytes, int32_t start, int32_t end);

Str SliceSerialize(Arena* a, const Slice& slice);

struct EditMap {
    struct Entry {
        int32_t at = 0;
        int32_t remove = 0;
        ArenaVec<Event> add = {};
    };

    Arena* a = nullptr;
    Vec<Entry> map;

    Vec<int32_t> buckets;
};

void EditMapAdd(EditMap& map, int32_t index, int32_t remove,
                const Event* add, int32_t addLen);
void EditMapAddBefore(EditMap& map, int32_t index, int32_t remove,
                      const Event* add, int32_t addLen);
inline bool EditMapEmpty(const EditMap& map) {
    return map.map.len == 0;
}
void EditMapConsume(EditMap& map, Vec<Event>& events);

int32_t SkipOpt(const Vec<Event>& events, int32_t index, const Name* names,
                int32_t namesLen);
int32_t SkipOptBack(const Vec<Event>& events, int32_t index, const Name* names,
                    int32_t namesLen);
int32_t SkipTo(const Vec<Event>& events, int32_t index, const Name* names,
               int32_t namesLen);
int32_t SkipToBack(const Vec<Event>& events, int32_t index, const Name* names,
                   int32_t namesLen);

Str NormalizeIdentifier(Arena* a, Str value);

bool ListLoose(const Vec<Event>& events, int32_t index, bool includeItems);
bool ListItemLoose(const Vec<Event>& events, int32_t index);

ArenaAlign GfmTableAlign(const Vec<Event>& events, int32_t index, Arena* a);

int32_t CharacterReferenceValueMax(uint8_t marker);
bool CharacterReferenceValueTest(uint8_t marker, uint8_t byte);

Str CharacterReferenceDecode(Arena* a, Str value, uint8_t marker);

Str CharacterReferenceDecodeInto(char buf[4], Str value, uint8_t marker);

}

#endif

#line 1 "src/markdown/tokenizer.h"

#ifndef GPUI_MARKDOWN_TOKENIZER_H_
#define GPUI_MARKDOWN_TOKENIZER_H_

namespace markdown {

enum class Container : uint8_t {
    BlockQuote,
    ListItem,
    GfmFootnoteDefinition,
};

struct ContainerState {
    Container kind = Container::BlockQuote;
    bool blankInitial = false;
    int32_t size = 0;
};

enum class LabelKind : uint8_t {
    Image,
    Link,
    GfmFootnote,
    GfmUndefinedFootnote,
};

struct LabelStartMark {
    LabelKind kind = LabelKind::Image;
    int32_t startA = 0;
    int32_t startB = 0;
    bool inactive = false;
};

struct Label {
    LabelKind kind = LabelKind::Image;
    int32_t startA = 0;
    int32_t startB = 0;
    int32_t endA = 0;
    int32_t endB = 0;
};

enum class ResolveName : uint8_t {
    Label,
    Attention,
    GfmTable,
    HeadingAtx,
    HeadingSetext,
    ListItem,
    Content,
    Data,
    String,
    Text,
};

struct Subresult {
    bool done = false;
    Vec<Str> gfmFootnoteDefinitions;
    Vec<Str> definitions;
};

void SubresultAppend(Subresult& dst, Subresult& src);

struct Tokenizer;

struct ParseState {

    Arena* a = nullptr;

    Arena* scratch = nullptr;
    const ParseOptions* options = nullptr;
    Str bytes = {};
    Vec<Str> definitions;
    Vec<Str> gfmFootnoteDefinitions;
};

struct TokenizeState {
    Tokenizer* documentChild = nullptr;
    State documentChildState = {};
    bool documentChildStateSome = false;
    Vec<ContainerState> documentContainerStack;
    int32_t documentContinued = 0;

    int32_t documentDataIndex = -1;

    Vec<ArenaVec<Event>> documentExits;
    bool documentLazyAcceptingBefore = false;
    bool documentAtFirstParagraphOfListItem = false;

    ContentKind spaceOrTabEolContent = ContentKind::Flow;
    bool spaceOrTabEolContentSome = false;
    bool spaceOrTabEolConnect = false;
    bool spaceOrTabEolOk = false;
    bool spaceOrTabConnect = false;
    ContentKind spaceOrTabContent = ContentKind::Flow;
    bool spaceOrTabContentSome = false;
    int32_t spaceOrTabMin = 0;
    int32_t spaceOrTabMax = 0;
    int32_t spaceOrTabSize = 0;
    Name spaceOrTabToken = Name::SpaceOrTab;

    Vec<LabelStartMark> labelStarts;
    Vec<LabelStartMark> labelStartsLoose;
    Vec<Label> labels;
    Vec<Str> definitions;
    Vec<Str> gfmFootnoteDefinitions;

    bool connect = false;
    uint8_t marker = 0;
    uint8_t markerB = 0;
    const uint8_t* markers = nullptr;
    int32_t markersLen = 0;
    bool seen = false;
    int32_t size = 0;
    int32_t sizeB = 0;
    int32_t sizeC = 0;
    int32_t start = 0;
    int32_t end = 0;
    Name token1 = Name::Data;
    Name token2 = Name::Data;
    Name token3 = Name::Data;
    Name token4 = Name::Data;
    Name token5 = Name::Data;
    Name token6 = Name::Data;
};

struct IndexVs {
    int32_t index = 0;
    int32_t vs = 0;
};

struct Progress {
    int32_t eventsLen = 0;
    int32_t stackLen = 0;
    int32_t previous = -1;
    int32_t current = -1;
    Point point = {};
};

struct Attempt {
    State ok = {};
    State nok = {};

    bool check = false;
    bool hasProgress = false;
    Progress progress = {};
};

struct Tokenizer {

    Vec<IndexVs> columnStart;
    int32_t firstLine = 1;
    Point lineStart = {};
    bool consumed = true;
    Vec<Attempt> attempts;

    int32_t current = -1;
    int32_t previous = -1;
    Point point = {};
    Vec<Event> events;
    Vec<Name> stack;
    EditMap map;
    Vec<ResolveName> resolvers;
    ParseState* parseState = nullptr;
    TokenizeState tokenizeState;
    bool interrupt = false;
    bool concrete = false;
    bool pierce = false;
    bool lazy = false;
};

Tokenizer* TokenizerNew(Point point, ParseState* parseState);
void TokenizerFree(Tokenizer* tokenizer);

void RegisterResolver(Tokenizer* t, ResolveName name);
void RegisterResolverBefore(Tokenizer* t, ResolveName name);
void DefineSkip(Tokenizer* t, Point point);
void Consume(Tokenizer* t);
void Enter(Tokenizer* t, Name name);
void EnterLink(Tokenizer* t, Name name, Link link);
void Exit(Tokenizer* t, Name name);

void TokenizerCheck(Tokenizer* t, State ok, State nok);
void TokenizerAttempt(Tokenizer* t, State ok, State nok);

State Push(Tokenizer* t, int32_t fromIndex, int32_t fromVs, int32_t toIndex,
           int32_t toVs, State state);
Subresult Flush(Tokenizer* t, State state, bool resolve);

bool ResolveCall(Tokenizer* t, ResolveName name, Subresult* out);

void SubtokenizeLink(Vec<Event>& events, int32_t index);
void SubtokenizeLinkTo(Vec<Event>& events, int32_t previous, int32_t next);

Subresult Subtokenize(Vec<Event>& events, ParseState* parseState,
                      bool hasFilter, ContentKind filter);
void DivideEvents(EditMap& map, const Vec<Event>& events, int32_t linkIndex,
                  Vec<Event>& childEvents, int32_t* accA, int32_t* accB);

Vec<Event> Parse(ParseState* parseState);

Node* ToMdastCompile(const Vec<Event>& events, ParseState* parseState);

}

#endif

#line 1 "src/markdown/construct.h"

#ifndef GPUI_MARKDOWN_CONSTRUCT_H_
#define GPUI_MARKDOWN_CONSTRUCT_H_

namespace markdown {

State AttentionStart(Tokenizer* t);
State AttentionInside(Tokenizer* t);
State AutolinkStart(Tokenizer* t);
State AutolinkOpen(Tokenizer* t);
State AutolinkSchemeOrEmailAtext(Tokenizer* t);
State AutolinkSchemeInsideOrEmailAtext(Tokenizer* t);
State AutolinkUrlInside(Tokenizer* t);
State AutolinkEmailAtSignOrDot(Tokenizer* t);
State AutolinkEmailAtext(Tokenizer* t);
State AutolinkEmailValue(Tokenizer* t);
State AutolinkEmailLabel(Tokenizer* t);
State BlankLineStart(Tokenizer* t);
State BlankLineAfter(Tokenizer* t);
State BlockQuoteStart(Tokenizer* t);
State BlockQuoteContStart(Tokenizer* t);
State BlockQuoteContBefore(Tokenizer* t);
State BlockQuoteContAfter(Tokenizer* t);
State BomStart(Tokenizer* t);
State BomInside(Tokenizer* t);
State CharacterEscapeStart(Tokenizer* t);
State CharacterEscapeInside(Tokenizer* t);
State CharacterReferenceStart(Tokenizer* t);
State CharacterReferenceOpen(Tokenizer* t);
State CharacterReferenceNumeric(Tokenizer* t);
State CharacterReferenceValue(Tokenizer* t);
State CodeIndentedStart(Tokenizer* t);
State CodeIndentedAtBreak(Tokenizer* t);
State CodeIndentedAfter(Tokenizer* t);
State CodeIndentedFurtherStart(Tokenizer* t);
State CodeIndentedInside(Tokenizer* t);
State CodeIndentedFurtherBegin(Tokenizer* t);
State CodeIndentedFurtherAfter(Tokenizer* t);
State ContentChunkStart(Tokenizer* t);
State ContentChunkInside(Tokenizer* t);
State ContentDefinitionBefore(Tokenizer* t);
State ContentDefinitionAfter(Tokenizer* t);
State DataStart(Tokenizer* t);
State DataInside(Tokenizer* t);
State DataAtBreak(Tokenizer* t);
State DefinitionStart(Tokenizer* t);
State DefinitionBefore(Tokenizer* t);
State DefinitionLabelAfter(Tokenizer* t);
State DefinitionLabelNok(Tokenizer* t);
State DefinitionMarkerAfter(Tokenizer* t);
State DefinitionDestinationBefore(Tokenizer* t);
State DefinitionDestinationAfter(Tokenizer* t);
State DefinitionDestinationMissing(Tokenizer* t);
State DefinitionTitleBefore(Tokenizer* t);
State DefinitionAfter(Tokenizer* t);
State DefinitionAfterWhitespace(Tokenizer* t);
State DefinitionTitleBeforeMarker(Tokenizer* t);
State DefinitionTitleAfter(Tokenizer* t);
State DefinitionTitleAfterOptionalWhitespace(Tokenizer* t);
State DestinationStart(Tokenizer* t);
State DestinationEnclosedBefore(Tokenizer* t);
State DestinationEnclosed(Tokenizer* t);
State DestinationEnclosedEscape(Tokenizer* t);
State DestinationRaw(Tokenizer* t);
State DestinationRawEscape(Tokenizer* t);
State DocumentStart(Tokenizer* t);
State DocumentBeforeFrontmatter(Tokenizer* t);
State DocumentContainerExistingBefore(Tokenizer* t);
State DocumentContainerExistingAfter(Tokenizer* t);
State DocumentContainerNewBefore(Tokenizer* t);
State DocumentContainerNewBeforeNotBlockQuote(Tokenizer* t);
State DocumentContainerNewBeforeNotList(Tokenizer* t);
State DocumentContainerNewBeforeNotGfmFootnoteDefinition(Tokenizer* t);
State DocumentContainerNewAfter(Tokenizer* t);
State DocumentContainersAfter(Tokenizer* t);
State DocumentFlowInside(Tokenizer* t);
State DocumentFlowEnd(Tokenizer* t);
State FlowStart(Tokenizer* t);
State FlowBeforeGfmTable(Tokenizer* t);
State FlowBeforeCodeIndented(Tokenizer* t);
State FlowBeforeRaw(Tokenizer* t);
State FlowBeforeHtml(Tokenizer* t);
State FlowBeforeHeadingAtx(Tokenizer* t);
State FlowBeforeHeadingSetext(Tokenizer* t);
State FlowBeforeThematicBreak(Tokenizer* t);
State FlowAfter(Tokenizer* t);
State FlowBlankLineBefore(Tokenizer* t);
State FlowBlankLineAfter(Tokenizer* t);
State FlowBeforeContent(Tokenizer* t);
State FrontmatterStart(Tokenizer* t);
State FrontmatterOpenSequence(Tokenizer* t);
State FrontmatterOpenAfter(Tokenizer* t);
State FrontmatterAfter(Tokenizer* t);
State FrontmatterContentStart(Tokenizer* t);
State FrontmatterContentInside(Tokenizer* t);
State FrontmatterContentEnd(Tokenizer* t);
State FrontmatterCloseStart(Tokenizer* t);
State FrontmatterCloseSequence(Tokenizer* t);
State FrontmatterCloseAfter(Tokenizer* t);
State GfmAutolinkLiteralProtocolStart(Tokenizer* t);
State GfmAutolinkLiteralProtocolAfter(Tokenizer* t);
State GfmAutolinkLiteralProtocolPrefixInside(Tokenizer* t);
State GfmAutolinkLiteralProtocolSlashesInside(Tokenizer* t);
State GfmAutolinkLiteralWwwStart(Tokenizer* t);
State GfmAutolinkLiteralWwwAfter(Tokenizer* t);
State GfmAutolinkLiteralWwwPrefixInside(Tokenizer* t);
State GfmAutolinkLiteralWwwPrefixAfter(Tokenizer* t);
State GfmAutolinkLiteralDomainInside(Tokenizer* t);
State GfmAutolinkLiteralDomainAtPunctuation(Tokenizer* t);
State GfmAutolinkLiteralDomainAfter(Tokenizer* t);
State GfmAutolinkLiteralPathInside(Tokenizer* t);
State GfmAutolinkLiteralPathAtPunctuation(Tokenizer* t);
State GfmAutolinkLiteralPathAfter(Tokenizer* t);
State GfmAutolinkLiteralTrail(Tokenizer* t);
State GfmAutolinkLiteralTrailCharRefInside(Tokenizer* t);
State GfmAutolinkLiteralTrailCharRefStart(Tokenizer* t);
State GfmAutolinkLiteralTrailBracketAfter(Tokenizer* t);
State GfmFootnoteDefinitionStart(Tokenizer* t);
State GfmFootnoteDefinitionLabelBefore(Tokenizer* t);
State GfmFootnoteDefinitionLabelAtMarker(Tokenizer* t);
State GfmFootnoteDefinitionLabelInside(Tokenizer* t);
State GfmFootnoteDefinitionLabelEscape(Tokenizer* t);
State GfmFootnoteDefinitionLabelAfter(Tokenizer* t);
State GfmFootnoteDefinitionWhitespaceAfter(Tokenizer* t);
State GfmFootnoteDefinitionContStart(Tokenizer* t);
State GfmFootnoteDefinitionContBlank(Tokenizer* t);
State GfmFootnoteDefinitionContFilled(Tokenizer* t);
State GfmLabelStartFootnoteStart(Tokenizer* t);
State GfmLabelStartFootnoteOpen(Tokenizer* t);
State GfmTaskListItemCheckStart(Tokenizer* t);
State GfmTaskListItemCheckInside(Tokenizer* t);
State GfmTaskListItemCheckClose(Tokenizer* t);
State GfmTaskListItemCheckAfter(Tokenizer* t);
State GfmTaskListItemCheckAfterSpaceOrTab(Tokenizer* t);
State GfmTableStart(Tokenizer* t);
State GfmTableHeadRowBefore(Tokenizer* t);
State GfmTableHeadRowStart(Tokenizer* t);
State GfmTableHeadRowBreak(Tokenizer* t);
State GfmTableHeadRowData(Tokenizer* t);
State GfmTableHeadRowEscape(Tokenizer* t);
State GfmTableHeadDelimiterStart(Tokenizer* t);
State GfmTableHeadDelimiterBefore(Tokenizer* t);
State GfmTableHeadDelimiterCellBefore(Tokenizer* t);
State GfmTableHeadDelimiterValueBefore(Tokenizer* t);
State GfmTableHeadDelimiterLeftAlignmentAfter(Tokenizer* t);
State GfmTableHeadDelimiterFiller(Tokenizer* t);
State GfmTableHeadDelimiterRightAlignmentAfter(Tokenizer* t);
State GfmTableHeadDelimiterCellAfter(Tokenizer* t);
State GfmTableHeadDelimiterNok(Tokenizer* t);
State GfmTableBodyRowStart(Tokenizer* t);
State GfmTableBodyRowBreak(Tokenizer* t);
State GfmTableBodyRowData(Tokenizer* t);
State GfmTableBodyRowEscape(Tokenizer* t);
State HardBreakEscapeStart(Tokenizer* t);
State HardBreakEscapeAfter(Tokenizer* t);
State HeadingAtxStart(Tokenizer* t);
State HeadingAtxBefore(Tokenizer* t);
State HeadingAtxSequenceOpen(Tokenizer* t);
State HeadingAtxAtBreak(Tokenizer* t);
State HeadingAtxSequenceFurther(Tokenizer* t);
State HeadingAtxData(Tokenizer* t);
State HeadingSetextStart(Tokenizer* t);
State HeadingSetextBefore(Tokenizer* t);
State HeadingSetextInside(Tokenizer* t);
State HeadingSetextAfter(Tokenizer* t);
State HtmlFlowStart(Tokenizer* t);
State HtmlFlowBefore(Tokenizer* t);
State HtmlFlowOpen(Tokenizer* t);
State HtmlFlowDeclarationOpen(Tokenizer* t);
State HtmlFlowCommentOpenInside(Tokenizer* t);
State HtmlFlowCdataOpenInside(Tokenizer* t);
State HtmlFlowTagCloseStart(Tokenizer* t);
State HtmlFlowTagName(Tokenizer* t);
State HtmlFlowBasicSelfClosing(Tokenizer* t);
State HtmlFlowCompleteClosingTagAfter(Tokenizer* t);
State HtmlFlowCompleteEnd(Tokenizer* t);
State HtmlFlowCompleteAttributeNameBefore(Tokenizer* t);
State HtmlFlowCompleteAttributeName(Tokenizer* t);
State HtmlFlowCompleteAttributeNameAfter(Tokenizer* t);
State HtmlFlowCompleteAttributeValueBefore(Tokenizer* t);
State HtmlFlowCompleteAttributeValueQuoted(Tokenizer* t);
State HtmlFlowCompleteAttributeValueQuotedAfter(Tokenizer* t);
State HtmlFlowCompleteAttributeValueUnquoted(Tokenizer* t);
State HtmlFlowCompleteAfter(Tokenizer* t);
State HtmlFlowBlankLineBefore(Tokenizer* t);
State HtmlFlowContinuation(Tokenizer* t);
State HtmlFlowContinuationDeclarationInside(Tokenizer* t);
State HtmlFlowContinuationAfter(Tokenizer* t);
State HtmlFlowContinuationStart(Tokenizer* t);
State HtmlFlowContinuationBefore(Tokenizer* t);
State HtmlFlowContinuationCommentInside(Tokenizer* t);
State HtmlFlowContinuationRawTagOpen(Tokenizer* t);
State HtmlFlowContinuationRawEndTag(Tokenizer* t);
State HtmlFlowContinuationClose(Tokenizer* t);
State HtmlFlowContinuationCdataInside(Tokenizer* t);
State HtmlFlowContinuationStartNonLazy(Tokenizer* t);
State HtmlTextStart(Tokenizer* t);
State HtmlTextOpen(Tokenizer* t);
State HtmlTextDeclarationOpen(Tokenizer* t);
State HtmlTextTagCloseStart(Tokenizer* t);
State HtmlTextTagClose(Tokenizer* t);
State HtmlTextTagCloseBetween(Tokenizer* t);
State HtmlTextTagOpen(Tokenizer* t);
State HtmlTextTagOpenBetween(Tokenizer* t);
State HtmlTextTagOpenAttributeName(Tokenizer* t);
State HtmlTextTagOpenAttributeNameAfter(Tokenizer* t);
State HtmlTextTagOpenAttributeValueBefore(Tokenizer* t);
State HtmlTextTagOpenAttributeValueQuoted(Tokenizer* t);
State HtmlTextTagOpenAttributeValueQuotedAfter(Tokenizer* t);
State HtmlTextTagOpenAttributeValueUnquoted(Tokenizer* t);
State HtmlTextCdata(Tokenizer* t);
State HtmlTextCdataOpenInside(Tokenizer* t);
State HtmlTextCdataClose(Tokenizer* t);
State HtmlTextCdataEnd(Tokenizer* t);
State HtmlTextCommentOpenInside(Tokenizer* t);
State HtmlTextComment(Tokenizer* t);
State HtmlTextCommentClose(Tokenizer* t);
State HtmlTextCommentEnd(Tokenizer* t);
State HtmlTextDeclaration(Tokenizer* t);
State HtmlTextEnd(Tokenizer* t);
State HtmlTextInstruction(Tokenizer* t);
State HtmlTextInstructionClose(Tokenizer* t);
State HtmlTextLineEndingBefore(Tokenizer* t);
State HtmlTextLineEndingAfter(Tokenizer* t);
State HtmlTextLineEndingAfterPrefix(Tokenizer* t);
State LabelStart(Tokenizer* t);
State LabelAtBreak(Tokenizer* t);
State LabelEolAfter(Tokenizer* t);
State LabelEscape(Tokenizer* t);
State LabelInside(Tokenizer* t);
State LabelNok(Tokenizer* t);
State LabelEndStart(Tokenizer* t);
State LabelEndAfter(Tokenizer* t);
State LabelEndResourceStart(Tokenizer* t);
State LabelEndResourceBefore(Tokenizer* t);
State LabelEndResourceOpen(Tokenizer* t);
State LabelEndResourceDestinationAfter(Tokenizer* t);
State LabelEndResourceDestinationMissing(Tokenizer* t);
State LabelEndResourceBetween(Tokenizer* t);
State LabelEndResourceTitleAfter(Tokenizer* t);
State LabelEndResourceEnd(Tokenizer* t);
State LabelEndOk(Tokenizer* t);
State LabelEndNok(Tokenizer* t);
State LabelEndReferenceFull(Tokenizer* t);
State LabelEndReferenceFullAfter(Tokenizer* t);
State LabelEndReferenceFullMissing(Tokenizer* t);
State LabelEndReferenceNotFull(Tokenizer* t);
State LabelEndReferenceCollapsed(Tokenizer* t);
State LabelEndReferenceCollapsedOpen(Tokenizer* t);
State LabelStartImageStart(Tokenizer* t);
State LabelStartImageOpen(Tokenizer* t);
State LabelStartImageAfter(Tokenizer* t);
State LabelStartLinkStart(Tokenizer* t);
State ListItemStart(Tokenizer* t);
State ListItemBefore(Tokenizer* t);
State ListItemBeforeOrdered(Tokenizer* t);
State ListItemBeforeUnordered(Tokenizer* t);
State ListItemValue(Tokenizer* t);
State ListItemMarker(Tokenizer* t);
State ListItemMarkerAfter(Tokenizer* t);
State ListItemAfter(Tokenizer* t);
State ListItemMarkerAfterFilled(Tokenizer* t);
State ListItemWhitespace(Tokenizer* t);
State ListItemPrefixOther(Tokenizer* t);
State ListItemWhitespaceAfter(Tokenizer* t);
State ListItemContStart(Tokenizer* t);
State ListItemContBlank(Tokenizer* t);
State ListItemContFilled(Tokenizer* t);
State NonLazyContinuationStart(Tokenizer* t);
State NonLazyContinuationAfter(Tokenizer* t);
State ParagraphStart(Tokenizer* t);
State ParagraphLineStart(Tokenizer* t);
State ParagraphInside(Tokenizer* t);
State RawFlowStart(Tokenizer* t);
State RawFlowBeforeSequenceOpen(Tokenizer* t);
State RawFlowSequenceOpen(Tokenizer* t);
State RawFlowInfoBefore(Tokenizer* t);
State RawFlowInfo(Tokenizer* t);
State RawFlowMetaBefore(Tokenizer* t);
State RawFlowMeta(Tokenizer* t);
State RawFlowAtNonLazyBreak(Tokenizer* t);
State RawFlowCloseStart(Tokenizer* t);
State RawFlowBeforeSequenceClose(Tokenizer* t);
State RawFlowSequenceClose(Tokenizer* t);
State RawFlowAfterSequenceClose(Tokenizer* t);
State RawFlowContentBefore(Tokenizer* t);
State RawFlowContentStart(Tokenizer* t);
State RawFlowBeforeContentChunk(Tokenizer* t);
State RawFlowContentChunk(Tokenizer* t);
State RawFlowAfter(Tokenizer* t);
State RawTextStart(Tokenizer* t);
State RawTextSequenceOpen(Tokenizer* t);
State RawTextBetween(Tokenizer* t);
State RawTextData(Tokenizer* t);
State RawTextSequenceClose(Tokenizer* t);
State SpaceOrTabStart(Tokenizer* t);
State SpaceOrTabInside(Tokenizer* t);
State SpaceOrTabAfter(Tokenizer* t);
State SpaceOrTabEolStart(Tokenizer* t);
State SpaceOrTabEolAfterFirst(Tokenizer* t);
State SpaceOrTabEolAfterEol(Tokenizer* t);
State SpaceOrTabEolAtEol(Tokenizer* t);
State SpaceOrTabEolAfterMore(Tokenizer* t);
State StringStart(Tokenizer* t);
State StringBefore(Tokenizer* t);
State StringBeforeData(Tokenizer* t);
State TextStart(Tokenizer* t);
State TextBefore(Tokenizer* t);
State TextBeforeHtml(Tokenizer* t);
State TextBeforeHardBreakEscape(Tokenizer* t);
State TextBeforeLabelStartLink(Tokenizer* t);
State TextBeforeData(Tokenizer* t);
State ThematicBreakStart(Tokenizer* t);
State ThematicBreakBefore(Tokenizer* t);
State ThematicBreakSequence(Tokenizer* t);
State ThematicBreakAtBreak(Tokenizer* t);
State TitleStart(Tokenizer* t);
State TitleBegin(Tokenizer* t);
State TitleAfterEol(Tokenizer* t);
State TitleAtBreak(Tokenizer* t);
State TitleEscape(Tokenizer* t);
State TitleInside(Tokenizer* t);
State TitleNok(Tokenizer* t);

constexpr int32_t kSizeMax = 0x7fffffff;

struct SpaceOrTabOptions {
    int32_t min = 0;
    int32_t max = 0;
    Name kind = Name::SpaceOrTab;
    bool connect = false;
    ContentKind content = ContentKind::Flow;
    bool contentSome = false;
};

StateName SpaceOrTab(Tokenizer* t);
StateName SpaceOrTabMinMax(Tokenizer* t, int32_t min, int32_t max);
StateName SpaceOrTabWithOptions(Tokenizer* t, const SpaceOrTabOptions& options);

struct SpaceOrTabEolOptions {
    bool connect = false;
    ContentKind content = ContentKind::Flow;
    bool contentSome = false;
};

StateName SpaceOrTabEol(Tokenizer* t);
StateName SpaceOrTabEolWithOptions(Tokenizer* t,
                                   const SpaceOrTabEolOptions& options);

void ResolveWhitespace(Tokenizer* t, bool hardBreak, bool trimWhole);

void GfmAutolinkLiteralResolve(Tokenizer* t);

bool LabelEndResolve(Tokenizer* t, Subresult* out);
bool AttentionResolve(Tokenizer* t, Subresult* out);
bool GfmTableResolve(Tokenizer* t, Subresult* out);
bool HeadingAtxResolve(Tokenizer* t, Subresult* out);
bool HeadingSetextResolve(Tokenizer* t, Subresult* out);
bool ListItemResolve(Tokenizer* t, Subresult* out);
bool ContentResolve(Tokenizer* t, Subresult* out);
bool DataResolve(Tokenizer* t, Subresult* out);
bool StringResolve(Tokenizer* t, Subresult* out);
bool TextResolve(Tokenizer* t, Subresult* out);

}

#endif

#line 1 "src/markdown-mini/markdown.h"

#ifndef GPUI_MARKDOWN_MINI_MARKDOWN_H_
#define GPUI_MARKDOWN_MINI_MARKDOWN_H_

#endif

#line 1 "src/sys/executor.h"

namespace gpui {

using TaskId = int;

void ExecInit();

void ExecShutdown();

bool ExecOnMainThread();

void ExecSetWake(Func0 wake);

void ExecPost(Func1<void*> f, void* arg = nullptr);

void ExecPostNow(Func1<void*> f, void* arg = nullptr);

int ExecDrain();

int ExecQueued();

TaskId ExecSpawn(Func0 work, Func0 done = Func0{});

bool ExecCancel(TaskId id);

int ExecPending();

bool ExecWaitIdle(int timeoutMs);

int ExecWorkerCount();

bool ExecHasThreads();

constexpr int kExecMaxWorkers = 8;

}

#line 1 "src/sys/gpu.h"

#ifndef GPUI_SYS_GPU_H_
#define GPUI_SYS_GPU_H_

namespace gpui {

bool GpuAvailable();

float GpuUsagePercent();

void GpuProbeFree();

}

#endif

#line 1 "src/sys/http.h"

namespace gpui {

struct HttpRsp {

    int status = 0;
    Vec<uint8_t> body;

    Str contentType;
};

void HttpRspFree(HttpRsp* r);

bool HttpGet(Str url, HttpRsp* out);

constexpr int kHttpMaxBody = 16 * 1024 * 1024;
constexpr int kHttpTimeoutMs = 15000;

bool HttpUrlIsRemote(Str url);

enum class FetchState : int32_t {

    None = 0,

    Pending,

    Done,

    Failed
};

FetchState HttpFetch(Str url, const uint8_t** bytes, int* len);

int HttpFetchPending();

void HttpSetOnFetchDone(Func0 f);

void HttpFetchClear();

void HttpSetEnabled(bool on);
bool HttpEnabled();

}

#line 1 "src/sys/notify.h"

namespace gpui {

typedef void (*SysNotifyResponseFn)(Str tag, void* user);

bool SysNotifyAvailable();

void SysNotifySetAppIdentity(Str appId, Str appName);

bool SysNotifyShow(Str tag, Str title, Str body);

void SysNotifyDismiss(Str tag);

void SysNotifyOnResponse(SysNotifyResponseFn fn, void* user);

void SysNotifyShutdown();

}

#line 1 "src/taffy/taffy_math.h"

#ifndef GPUI_TAFFY_MATH_H_
#define GPUI_TAFFY_MATH_H_

#if defined(_MSC_VER)
#define TAFFY_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define TAFFY_INLINE inline __attribute__((always_inline))
#else
#define TAFFY_INLINE inline
#endif

namespace taffy {

inline Optf MaybeMin(Optf a, Optf b) {
    if (IsNone(a)) {
        return None();
    }
    return IsSome(b) ? F32Min(a, b) : a;
}

inline Optf MaybeMax(Optf a, Optf b) {
    if (IsNone(a)) {
        return None();
    }
    return IsSome(b) ? F32Max(a, b) : a;
}

inline Optf MaybeAdd(Optf a, Optf b) {
    if (IsNone(a)) {
        return None();
    }
    return IsSome(b) ? a + b : a;
}

inline Optf MaybeSub(Optf a, Optf b) {
    if (IsNone(a)) {
        return None();
    }
    return IsSome(b) ? a - b : a;
}

inline Optf MaybeClamp(Optf a, Optf lo, Optf hi) {
    if (IsNone(a)) {
        return None();
    }
    float v = a;
    if (IsSome(hi)) {
        v = F32Min(v, hi);
    }
    if (IsSome(lo)) {
        v = F32Max(v, lo);
    }
    return v;
}

inline AvailableSpace MaybeMin(AvailableSpace a, Optf b) {
    if (a.kind == AvailableSpace::Kind::Definite) {
        return IsSome(b) ? AvailableSpace::Definite(F32Min(a.value, b)) : a;
    }
    return IsSome(b) ? AvailableSpace::Definite(b) : a;
}

inline AvailableSpace MaybeMax(AvailableSpace a, Optf b) {
    if (a.kind == AvailableSpace::Kind::Definite && IsSome(b)) {
        return AvailableSpace::Definite(F32Max(a.value, b));
    }
    return a;
}

inline AvailableSpace MaybeAdd(AvailableSpace a, Optf b) {
    if (a.kind == AvailableSpace::Kind::Definite && IsSome(b)) {
        return AvailableSpace::Definite(a.value + b);
    }
    return a;
}

inline AvailableSpace MaybeSub(AvailableSpace a, Optf b) {
    if (a.kind == AvailableSpace::Kind::Definite && IsSome(b)) {
        return AvailableSpace::Definite(a.value - b);
    }
    return a;
}

inline AvailableSpace MaybeClamp(AvailableSpace a, Optf lo, Optf hi) {
    if (a.kind != AvailableSpace::Kind::Definite) {
        return a;
    }
    float v = a.value;
    if (IsSome(hi)) {
        v = F32Min(v, hi);
    }
    if (IsSome(lo)) {
        v = F32Max(v, lo);
    }
    return AvailableSpace::Definite(v);
}

TAFFY_INLINE SizeFOpt MaybeMin(SizeFOpt a, SizeFOpt b) {
    return {MaybeMin(a.w, b.w), MaybeMin(a.h, b.h)};
}

TAFFY_INLINE SizeFOpt MaybeMax(SizeFOpt a, SizeFOpt b) {
    return {MaybeMax(a.w, b.w), MaybeMax(a.h, b.h)};
}

TAFFY_INLINE SizeFOpt MaybeAdd(SizeFOpt a, SizeFOpt b) {
    return {MaybeAdd(a.w, b.w), MaybeAdd(a.h, b.h)};
}

TAFFY_INLINE SizeFOpt MaybeSub(SizeFOpt a, SizeFOpt b) {
    return {MaybeSub(a.w, b.w), MaybeSub(a.h, b.h)};
}

TAFFY_INLINE SizeFOpt MaybeClamp(SizeFOpt a, SizeFOpt lo, SizeFOpt hi) {
    return {MaybeClamp(a.w, lo.w, hi.w), MaybeClamp(a.h, lo.h, hi.h)};
}

TAFFY_INLINE SizeAvail MaybeSub(SizeAvail a, SizeFOpt b) {
    return {MaybeSub(a.width, b.w), MaybeSub(a.height, b.h)};
}

TAFFY_INLINE SizeAvail MaybeClamp(SizeAvail a, SizeFOpt lo, SizeFOpt hi) {
    return {MaybeClamp(a.width, lo.w, hi.w), MaybeClamp(a.height, lo.h, hi.h)};
}

TAFFY_INLINE SizeAvail MaybeSet(SizeAvail a, SizeFOpt v) {
    return a.MaybeSet(v);
}

constexpr SizeFOpt AsOptional(SizeF s) {
    return s;
}

}

#endif

#line 1 "src/taffy/compute.h"

#ifndef GPUI_TAFFY_COMPUTE_H_
#define GPUI_TAFFY_COMPUTE_H_

namespace taffy {

void ComputeRootLayout(TaffyTree* tree, NodeId root, SizeAvail availableSpace);

LayoutOutput ComputeHiddenLayout(TaffyTree* tree, NodeId node);

void RoundLayout(TaffyTree* tree, NodeId node);

using LeafMeasureFn = SizeF (*)(SizeFOpt knownDimensions,
                                SizeAvail availableSpace, void* ctx);

LayoutOutput ComputeLeafLayout(const LayoutInput& inputs, const Style& style,
                               CalcResolver calc, LeafMeasureFn measure,
                               void* measureCtx);

LayoutOutput ComputeFlexboxLayout(TaffyTree* tree, NodeId node,
                                  const LayoutInput& inputs);

LayoutOutput ComputeGridLayout(TaffyTree* tree, NodeId node,
                               const LayoutInput& inputs);

LayoutOutput ComputeBlockLayout(TaffyTree* tree, NodeId node,
                                const LayoutInput& inputs,
                                BlockContext* blockCtx);

inline AlignItemsKeyword ResolveSelfAlignmentSafety(AlignItems alignment,
                                                    bool overflows) {
    if (alignment.safety == AlignmentSafety::Safe && overflows) {
        return AlignItemsKeyword::Start;
    }
    return alignment.keyword;
}

AlignContentKeyword ApplyAlignmentFallback(float freeSpace, int numItems,
                                           AlignContent alignmentMode);

float ComputeAlignmentOffset(float freeSpace, int numItems, float gap,
                             AlignContentKeyword alignmentMode,
                             bool layoutIsFlexReversed, bool isFirst);

void GridExplicitSizeForTest(const Style& style, Optf autoFitContainerSize,
                             bool maxRepetitions, AbsoluteAxis axis,
                             CalcResolver calc, uint16_t* outAutoRepetitions,
                             uint16_t* outTrackCount);

void GridChildMinMaxSpanForTest(LinePlacement line, uint16_t explicitTrackCount,
                                int16_t* outMinLine, int16_t* outMaxLine,
                                uint16_t* outSpan);

void GridSizeEstimateForTest(uint16_t explicitColCount,
                             uint16_t explicitRowCount, Direction direction,
                             const LinePlacement* columns,
                             const LinePlacement* rows, int n,
                             uint16_t* outColCounts, uint16_t* outRowCounts);

struct GridTrackForTest {
    bool isGutter = false;
    bool isCollapsed = false;
    CompactLength min;
    CompactLength max;
};

int GridInitTracksForTest(const Style& style, AbsoluteAxis axis,
                          uint16_t negativeImplicit, uint16_t explicitCount,
                          uint16_t positiveImplicit, GridTrackForTest* out,
                          int cap);

struct GridPlacementForTest {
    int16_t columnStart = 0;
    int16_t columnEnd = 0;
    int16_t rowStart = 0;
    int16_t rowEnd = 0;
};

int GridPlaceForTest(TaffyTree* tree, NodeId parent, uint16_t explicitColCount,
                     uint16_t explicitRowCount, GridAutoFlow flow,
                     GridPlacementForTest* out, int cap, uint16_t* outColCounts,
                     uint16_t* outRowCounts);

SizeF ComputeContentSizeContribution(PointF location, SizeF size,
                                     SizeF contentSize, PointOverflow overflow);

}

#endif

#line 1 "src/ui/html.h"

namespace gpui {

namespace component {

MdNode* HtmlParse(Arena* a, Str source);

void HtmlParseInto(Arena* a, MdNode* parent, Str source);

struct HtmlInlineTag {

    uint8_t mark = 0;
    bool close = false;
    bool known = false;

    bool isBreak = false;

    Str href = {};
    Str alt = {};
    Str src = {};
    float width = 0;
    float height = 0;
    bool isImage = false;
};

HtmlInlineTag HtmlParseInlineTag(Arena* a, Str tag);

Str HtmlAttrValue(Arena* a, Str attrs, const char* name);

}
}

#line 1 "src/ui/resizable.h"

namespace gpui {

namespace component {

using ResizableState = gpui::ResizableState;

struct Resizable {

    static gpui::Resizable* New(Ctx* cx, Str id,
                                Entity<ResizableState> state = {},
                                Axis axis = Axis::Horizontal);
};

}
}

#line 1 "src/ui/theme.h"

namespace gpui {

const int kNumShadcnColumns = 11;

struct ShadcnScale {
    const char* name;
    uint32_t hex[kNumShadcnColumns];
};

extern const ShadcnScale kShadcnScales[];
extern const int kNumShadcnScales;
extern const int kShadcnScaleNums[kNumShadcnColumns];

extern const uint32_t kShadcnStone[kNumShadcnColumns];
extern const uint32_t kShadcnBlack;
extern const uint32_t kShadcnWhite;

extern const char* const kDefaultThemeJson;

bool ThemeParseColor(Str s, Rgba* out);

bool ThemeParseBackground(Str s, Background* out);

struct ThemeConfig {
    Str name = {};
    Str author = {};
    Str url = {};
    ThemeMode mode = ThemeMode::Light;
    bool isDefault = false;

    const JsonValue* colors = nullptr;

    float fontSize = 0;
    float radius = -1;
    float radiusLg = -1;
};

bool ThemeConfigNames(const ThemeConfig* cfg, const char* key);

void ThemeConfigResolve(Theme* out, const ThemeConfig* cfg, const Theme& base);

void ThemeRegistryInit();

int ThemeRegistryLoadStr(Str json);

int ThemeRegistryLoadDir(Str dir);

bool ThemeApplySemanticConfigStr(ThemeMode mode, Str json,
                                 SemanticThemeTokens* out = nullptr);

bool ThemeSemanticConfigApply(const JsonValue* doc, SemanticThemeTokens* io);

int ThemeRegistryCount();
const ThemeConfig* ThemeRegistryAt(int ix);
const ThemeConfig* ThemeRegistryFind(Str name);

Str ThemeRegistryActive(ThemeMode mode);

bool ThemeRegistryApply(App* app, const ThemeConfig* cfg);
bool ThemeRegistryApply(App* app, Str name);

void ThemeRegistryReset(App* app);

void ThemeRegistryFree();

}

#line 1 "src/wry/wry.h"

#ifndef GPUI_WRY_WRY_H_
#define GPUI_WRY_WRY_H_

namespace wry {

using base::Str;
using base::Vec;

struct Position {
    double x = 0;
    double y = 0;
    bool logical = true;
};

struct Size {
    double width = 0;
    double height = 0;
    bool logical = true;
};

struct Rect {
    Position position;
    Size size;
};

inline Position LogicalPosition(double x, double y) {
    return Position{x, y, true};
}
inline Position PhysicalPosition(double x, double y) {
    return Position{x, y, false};
}
inline Size LogicalSize(double w, double h) {
    return Size{w, h, true};
}
inline Size PhysicalSize(double w, double h) {
    return Size{w, h, false};
}

struct Rgba {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;
};

enum class Theme {
    Dark,
    Light,
    Auto,
};

enum class PageLoadEvent {
    Started,
    Finished,
};

enum class ScrollBarStyle {
    Default,
    FluentOverlay,
};

enum class MemoryUsageLevel {
    Normal,
    Low,
};

enum class ProxyKind {
    None,
    Http,
    Socks5,
};

struct ProxyConfig {
    ProxyKind kind = ProxyKind::None;
    Str host;
    Str port;
};

struct InitializationScript {
    Str script;

    bool forMainFrameOnly = true;
};

struct Header {
    Str name;
    Str value;
};

struct Request {
    Str method;
    Str uri;
    const Header* headers = nullptr;
    int headerCount = 0;
    const uint8_t* body = nullptr;
    int bodyLen = 0;
};

struct Response {
    int status = 200;
    const Header* headers = nullptr;
    int headerCount = 0;
    const uint8_t* body = nullptr;
    int bodyLen = 0;
};

struct RequestResponder;
void Respond(RequestResponder* responder, const Response* response);

struct CustomProtocol {
    Str name;
    void* ctx = nullptr;
    void (*handler)(void* ctx, Str id, const Request* request,
                    RequestResponder* responder) = nullptr;
};

enum class NewWindowResponse {
    Allow,
    Deny,
};

struct NewWindowFeatures {
    bool hasPosition = false;
    double x = 0;
    double y = 0;
    bool hasSize = false;
    double width = 0;
    double height = 0;
};

struct WebViewAttributes {

    Str id;

    Str dataDirectory;
    Str userAgent;
    bool visible = true;
    bool transparent = false;
    bool hasBackgroundColor = false;
    Rgba backgroundColor;

    Str url;

    const Header* headers = nullptr;
    int headerCount = 0;

    Str html;
    bool zoomHotkeysEnabled = false;
    const InitializationScript* initializationScripts = nullptr;
    int initializationScriptCount = 0;
    const CustomProtocol* customProtocols = nullptr;
    int customProtocolCount = 0;

    void* ctx = nullptr;

    void (*ipcHandler)(void* ctx, Str url, Str body) = nullptr;

    bool (*navigationHandler)(void* ctx, Str url) = nullptr;
    void (*documentTitleChangedHandler)(void* ctx, Str title) = nullptr;
    void (*onPageLoadHandler)(void* ctx, PageLoadEvent event, Str url) = nullptr;

    NewWindowResponse (*newWindowReqHandler)(void* ctx, Str url,
                                             const NewWindowFeatures* features) = nullptr;

    bool clipboard = false;
#if defined(DEBUG) || defined(_DEBUG)
    bool devtools = true;
#else
    bool devtools = false;
#endif

    bool acceptFirstMouse = false;
    bool backForwardNavigationGestures = false;
    bool incognito = false;
    bool autoplay = true;
    ProxyConfig proxyConfig;
    bool focused = true;

    Rect bounds = {{0, 0, true}, {200, 200, true}};
    bool javascriptDisabled = false;

    Str additionalBrowserArgs;
    bool browserAcceleratorKeys = true;
    bool defaultContextMenus = true;
    bool hasTheme = false;
    Theme theme = Theme::Auto;

    bool useHttpsScheme = false;
    ScrollBarStyle scrollBarStyle = ScrollBarStyle::Default;
    bool browserExtensionsEnabled = false;
    Str extensionPath;
};

struct WebView;

WebView* WebViewNew(void* parentWindow, const WebViewAttributes* attrs, bool asChild);

void WebViewFree(WebView* webview);

Str WebViewId(WebView* webview);

bool WebViewEval(WebView* webview, Str js);

bool WebViewEvalWithCallback(WebView* webview, Str js, void* ctx,
                             void (*callback)(void* ctx, Str result));

Str WebViewUrlTemp(WebView* webview);

bool WebViewLoadUrl(WebView* webview, Str url);

bool WebViewLoadUrlWithHeaders(WebView* webview, Str url, const Header* headers,
                               int headerCount);

bool WebViewLoadHtml(WebView* webview, Str html);

bool WebViewReload(WebView* webview);

bool WebViewBounds(WebView* webview, Rect* out);

bool WebViewSetBounds(WebView* webview, Rect bounds);

bool WebViewSetVisible(WebView* webview, bool visible);

bool WebViewFocus(WebView* webview);

bool WebViewFocusParent(WebView* webview);

bool WebViewZoom(WebView* webview, double scaleFactor);

bool WebViewSetBackgroundColor(WebView* webview, Rgba color);

bool WebViewSetTheme(WebView* webview, Theme theme);

bool WebViewSetMemoryUsageLevel(WebView* webview, MemoryUsageLevel level);

bool WebViewReparent(WebView* webview, void* parentWindow);

bool WebViewPrint(WebView* webview);

bool WebViewClearAllBrowsingData(WebView* webview);

void WebViewOpenDevtools(WebView* webview);
void WebViewCloseDevtools(WebView* webview);
bool WebViewIsDevtoolsOpen(WebView* webview);

Str WorkAroundUriPrefix(Str httpOrHttps, Str protocol);

bool IsWorkAroundUri(Str uri, Str httpOrHttps, Str protocol);

Str ApplyUriWorkAround(Str uri, Str httpOrHttps, Str protocol);
Str RevertUriWorkAround(Str uri, Str httpOrHttps, Str protocol);

Str WebViewVersionTemp();

bool WebViewAvailable();

}

#endif

#line 1 "src/webview/webview.h"

#ifndef GPUI_WEBVIEW_WEBVIEW_H_
#define GPUI_WEBVIEW_WEBVIEW_H_

namespace gpui {

struct WebView {
    wry::WebView* webview = nullptr;
    bool visible = true;

    Bounds bounds = {};

    Bounds applied = {};
    bool hasApplied = false;

    bool subscribed = false;

    ~WebView();

    static void OnWindowMouseDown(WebView* self, Ctx* cx,
                                  const MouseDownEvent* ev);
};

Entity<WebView> WebViewNew(Ctx* cx, const wry::WebViewAttributes* attrs);

void WebViewShow(WebView* self);
void WebViewHide(WebView* self);
bool WebViewVisible(const WebView* self);

Bounds WebViewBounds(const WebView* self);

void WebViewLoadUrl(WebView* self, Str url);

void WebViewBack(WebView* self);

wry::WebView* WebViewRaw(WebView* self);

El* WebViewEl(Entity<WebView> view, Ctx* cx);

}

#endif

#endif
