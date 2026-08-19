#ifndef GPUI_H_
#define GPUI_H_
#ifndef GPUI_AMALGAM
#define GPUI_AMALGAM 1
#endif

#line 1 "src/Base.h"

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

#if defined(_WIN32)
#define GPUI_OS_WINDOWS 1
#define GPUI_OS_LINUX 0
#define GPUI_OS_MAC 0
#elif defined(__APPLE__)
#define GPUI_OS_WINDOWS 0
#define GPUI_OS_LINUX 0
#define GPUI_OS_MAC 1
#elif defined(__linux__)
#define GPUI_OS_WINDOWS 0
#define GPUI_OS_LINUX 1
#define GPUI_OS_MAC 0
#else
#error "unsupported platform: gpui builds on Windows, Linux and macOS"
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

namespace gpui {

enum {
    kMaxPath = 1024
};

struct Arena;

struct Str {
    char* s;
    int len;

    Str() : s(nullptr), len(0) {}
    Str(const char* s_) : s((char*)s_), len(0) {
        len = s_ ? (int)strlen(s_) : 0;
    }
    explicit Str(const char* s_, int len_) : s((char*)s_), len(len_) {}
    explicit Str(char* s_) : s(s_), len(0) { len = s ? (int)strlen(s) : 0; }
    explicit Str(char* s_, int len_) : s(s_), len(len_) {}

    explicit operator bool() const { return len > 0 && s; }
};

void log(Str s);

using TempStr = Str;

#define StrL(lit) ::gpui::Str((char*)(lit), (int)(sizeof(lit) - 1))

Str AllocStrTemp(int size);

#if GPUI_OS_WINDOWS

WCHAR* ToCWstrTemp(Str s);
#endif

uint64_t PlatPageSize();
uint64_t PlatLargePageSize();

void* PlatMemReserve(uint64_t size);
bool PlatMemCommit(void* base, uint64_t size, bool largePages);
void* PlatMemReserveCommit(uint64_t size, bool largePages);
void PlatMemRelease(void* base, uint64_t size);

int StrCmpI(const char* a, const char* b);
int StrCmpNI(const char* a, const char* b, int n);

void StrCopyZ(char* dst, int cap, const char* src);

bool PlatDirExists(const char* path);
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
    void* fn = nullptr;
    uintptr_t userData = 0;

    Func0() = default;

    bool IsValid() const { return fn != nullptr; }
    void Call() const {
        if (!fn) {
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

template <typename T>
struct Func1 {
    void (*fn)(uintptr_t, T) = nullptr;
    uintptr_t userData = 0;

    Func1() = default;

    bool IsValid() const { return fn != nullptr; }
    void Call(T arg) const {
        if (!fn) {
            return;
        }
        fn(userData, arg);
    }
};

template <typename T1, typename T2>
Func1<T2> MkFunc1(void (*fn)(T1*, T2), T1* d) {
    auto res = Func1<T2>{};
    using fptr = void (*)(uintptr_t, T2);
    res.fn = (fptr)fn;
    res.userData = (uintptr_t)d;
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

GPUI_NOINLINE bool VecRealloc(struct Arena* a, void** els, int len, int* cap,
                              int newCap, int elSize);

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

    void FreeEls() {
        if (els) {
            Free(nullptr, (void*)els);
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
        if (els && cap > 0) {
            memset((void*)els, 0, (size_t)cap * sizeof(T));
        }
    }

    explicit Vec() = default;

    Vec(const Vec& other) {
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
            memset((void*)(els + len), 0, sizeof(T) * (size_t)(cap - len));
        }
        return *this;
    }

    ~Vec() { FreeEls(); }

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

template <typename T>
bool VecReserve(Arena* arena, T& v, int wantedSize) {
    if (wantedSize <= v.cap) {
        return true;
    }
    int newCap = std::max(v.cap * 2, wantedSize);
    return VecRealloc(arena, (void**)&v.els, v.len, &v.cap, newCap,
                      (int)sizeof(*v.els));
}

template <typename T>
inline T* VecReserve(Vec<T>& v, int capNeeded) {
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

#line 1 "src/gpui/Gpui.h"

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

Rgba RgbaHsla(float h, float s, float l, float a01);

constexpr float kAuto = -1.f;
constexpr float kFill = -2.f;
constexpr float kPi = 3.14159265358979f;

struct App;

struct Theme {
    Rgba background;
    Rgba foreground;
    Rgba border;
    Rgba mutedFg;

    Rgba inputBorder;
    Rgba inputBg;

    Rgba ring;
    Rgba caret;
    Rgba titleBar;
    Rgba titleBarBorder;
    Rgba tabBar;
    Rgba tabActiveBg;
    Rgba tabActiveFg;
    Rgba tabFg;
    Rgba tableBg;
    Rgba tableHead;
    Rgba tableHeadFg;
    Rgba tableRowBorder;
    Rgba tableEven;
    Rgba progress;
    Rgba red;
    Rgba green;
    Rgba blue;
    Rgba yellow;
    Rgba cyan;
    Rgba magenta;
    Rgba danger;
    Rgba dangerFg;
    Rgba secondaryHover;
    Rgba secondaryActive;
    Rgba secondaryFg;
    Rgba secondary;
    Rgba muted;
    Rgba accent;
    Rgba primary;
    Rgba primaryFg;
    Rgba sidebar;
    Rgba sidebarFg;
    Rgba sidebarPrimary;
    Rgba sidebarPrimaryFg;
    Rgba scrollbarThumb;
    Rgba info;
    Rgba infoFg;
    Rgba success;
    Rgba successFg;
    Rgba warning;
    Rgba warningFg;
    Rgba skeleton;

    Rgba overlay;

    Rgba groupBox;
    Rgba groupBoxFg;

    Rgba descListLabel;
    Rgba descListLabelFg;

    float radius;
    float radiusLg;
};

enum class ThemeMode : uint8_t {
    Light,
    Dark
};

const Theme& ThemeDark();
const Theme& ThemeLight();

const Theme& ThemeNow();
void ThemeSet(App* app, ThemeMode mode);
ThemeMode ThemeGet();

enum class Axis : uint8_t {
    Horizontal,
    Vertical
};

inline bool AxisIsHorizontal(Axis a) {
    return a == Axis::Horizontal;
}

struct Point {
    float x = 0;
    float y = 0;
};

struct Size {
    float w = 0;
    float h = 0;
};

struct Edges {
    float top = 0;
    float right = 0;
    float bottom = 0;
    float left = 0;

    float Horizontal() const { return left + right; }
    float Vertical() const { return top + bottom; }
};

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
        return {x + e.left, y + e.top, w - e.Horizontal(), h - e.Vertical()};
    }
};

inline Bounds BoundsAt(Point origin, Size size) {
    return {origin.x, origin.y, size.w, size.h};
}

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

struct KeyedSlot {
    uint32_t key = 0;
    void* ptr = nullptr;
    DropFn drop = nullptr;
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

struct MouseDownEvent {
    MouseButton button = MouseButton::Left;
    float x = 0;
    float y = 0;
    Modifiers modifiers = {};

    int clickCount = 1;

    bool firstMouse = false;

    bool IsFocusing() const { return button == MouseButton::Left; }
};

struct MouseUpEvent {
    MouseButton button = MouseButton::Left;
    float x = 0;
    float y = 0;
    Modifiers modifiers = {};
    int clickCount = 1;

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
    KeyV = 86,
    KeyX = 88
};

struct KeyEvent {
    int vk = 0;
    uint32_t ch = 0;
    bool down = false;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
};

enum class CursorKind : uint8_t {
    Arrow,
    IBeam
};

struct TickEvent {
    int ms = 0;
};

using ListenerFn = void (*)(void* self, Ctx* cx, const void* ev);
using ListenerArgFn = void (*)(void* self, Ctx* cx, const void* ev,
                               intptr_t arg);

struct Listener {
    void* fn = nullptr;
    EntityId view = {};
    intptr_t arg = 0;
    bool hasArg = false;

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
    Icon
};

enum class FlexDir : uint8_t {
    Row,
    Col
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
    SpaceBetween
};
enum class OverflowY : uint8_t {
    Visible,
    Hidden,
    Scroll
};

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
    ChevronUp,
    Check,
    Search,
    Minus,
    Plus,
    Copy,
    Bell,
    Star,
    StarFill,
    Eye,
    Heart,
    ArrowLeft,
    Building2,
    Asterisk,
    Sun,
    Maximize,
    Minimize,
    Map,
    Globe,
    Github,
    HeartOff,
};

struct PaintCtx;

struct ChartSeries {
    const float* ys = nullptr;
    int n = 0;
    int tickMargin = 15;

    const char* const* labels = nullptr;

    bool overlay = false;
    Rgba stroke = {};
    Rgba fillTop = {};
    Rgba fillBot = {};
};

struct Style {
    FlexDir dir = FlexDir::Row;
    Align align = Align::Stretch;
    Justify justify = Justify::Start;
    OverflowY overflowY = OverflowY::Visible;
    float width = kAuto;
    float height = kAuto;

    float widthFrac = 0;
    float minW = 0;
    float minH = 0;
    float maxW = 1e9f;
    float maxH = 1e9f;
    float flexGrow = 0;
    float flexShrink = 1;
    Edges pad = {};
    float gap = 0;
    float border = 0;
    float borderT = 0;
    float borderB = 0;
    float borderL = 0;
    float borderR = 0;
    float radius = 0;
    Rgba bg = {};
    Rgba borderColor = {};
    Rgba color = {};
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
    bool italic = false;
    bool borderDashed = false;

    float dashOn = 2;
    float dashOff = 1;
    bool absolute = false;
    bool fixed = false;

    bool deferred = false;
    bool anchorBelow = false;
    bool anchorAbove = false;
    bool anchorCenterX = false;
    float anchorGap = 0;
    float absTop = kAuto, absLeft = kAuto, absBottom = kAuto, absRight = kAuto;
    Rgba hoverBg = {};
    bool hasHoverBg = false;

    Rgba hoverFg = {};
    bool hasHoverFg = false;
    int focusId = 0;
    int trapId = 0;
    Str tooltip;
};

struct El {
    ElKind kind = ElKind::Div;
    Style style;
    Str id;
    Str text;
    IconName icon = IconName::None;
    Str iconPath;
    ChartSeries chart = {};
    float progress = 0;
    int clickId = 0;
    Func0 onClick;
    Listener listener;

    Listener onMouseDown;
    Listener onMouseUp;

    Listener onDragMove;

    SliderState* slider = nullptr;
    Axis sliderAxis = Axis::Horizontal;

    SliderState* sliderBounds = nullptr;
    void (*customPaint)(PaintCtx* ctx, El* e, void* user) = nullptr;
    void* customUser = nullptr;
    El* first = nullptr;
    El* last = nullptr;
    El* next = nullptr;
    float x = 0, y = 0, w = 0, h = 0;

    gpui::Bounds Bounds() const { return {x, y, w, h}; }
    float scrollY = 0;
    int scrollId = 0;
    float contentW = 0;
    float contentH = 0;
    int selLo = -1;
    int selHi = -1;
    bool selectable = false;
    float laidFont = 0;
    float laidMaxW = 0;

    TextLayout* laidLayout = nullptr;

    float memoAvailW = 0;
    float memoAvailH = 0;
    float memoFont = 0;
    Rgba memoFg = {};
    float memoW = 0;
    float memoH = 0;
    float memoContentW = 0;
    float memoContentH = 0;
    bool memoValid = false;

    El* FlexRow();
    El* FlexCol();
    El* FlexWrap();
    El* Grow(float g = 1);
    El* Shrink0();
    El* W(float v);
    El* WFrac(float f);
    El* H(float v);
    El* SizeFull();
    El* MinH(float v);
    El* MinW(float v);
    El* MaxW(float v);
    El* MaxH(float v);
    El* Gap(float v);
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
    El* JustifyBetween();
    El* JustifyCenter();
    El* JustifyEnd();
    El* JustifyStart();
    El* Bg(Rgba c);
    El* Border(float width, Rgba c);
    El* BorderT(float width, Rgba c);
    El* BorderB(float width, Rgba c);
    El* BorderL(float width, Rgba c);
    El* BorderR(float width, Rgba c);
    El* Radius(float r);
    El* Fg(Rgba c);
    El* Font(float px);
    El* LineHeight(float mult);
    El* Truncate();
    El* ClipY();
    El* ScrollY(float off);
    El* ScrollId(int v);
    El* Click(int v);
    El* OnClick(Func0 fn);
    El* OnClick(Listener l);
    El* OnMouseDown(Listener l);
    El* OnMouseUp(Listener l);
    El* OnDragMove(Listener l);
    El* BindSlider(SliderState* s, Axis axis = Axis::Horizontal);
    El* BindSliderBounds(SliderState* s);
    El* Child(El* c);
    El* Bold();
    El* Semibold();
    El* Medium();
    El* Mono();
    El* Underline();
    El* Italic();
    El* Selectable();
    El* Wrap();
    El* Dashed();
    El* DashArray(float on, float off);
    El* Absolute();
    El* Fixed();
    El* Deferred();
    El* AnchorBelow(float gap = 0);
    El* AnchorAbove(float gap = 0);
    El* AnchorCenterX();
    El* Top(float v);
    El* Left(float v);
    El* Bottom(float v);
    El* Right(float v);
    El* HoverBg(Rgba c);
    El* HoverFg(Rgba c);
    El* FocusId(int v);
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
El* ProgressEl(Arena* a, float value01to100, float barW, float barH);
El* ChartEl(Arena* a, const float* ys, int n, Rgba stroke, Rgba fillTop,
            Rgba fillBot, int tickMargin);

struct HitRect {
    int id = 0;
    Bounds bounds = {};
    Func0 onClick;
    Listener listener;
    Listener onMouseDown;
    Listener onMouseUp;
    Listener onDragMove;
    SliderState* slider = nullptr;
    Axis sliderAxis = Axis::Horizontal;
};

struct ScrollRect {
    int id = 0;
    Bounds bounds = {};
    float contentH = 0;
};

struct TextHit {
    Bounds bounds = {};
    Str text;
    float font = 14;
    float maxW = 0;
    bool wrap = false;
    int docOff = 0;
};

struct TextMeasCache {
    void* slots = nullptr;
    int cap = 0;
    int used = 0;
    uint32_t frame = 0;
};

struct PaintApp;
struct PaintTarget;

struct PaintCtx {
    PaintApp* pa = nullptr;
    PaintTarget* rt = nullptr;
    float dpi = 96;
    float viewW = 0;
    float viewH = 0;
    int hoverId = 0;
    int focusId = 0;
    Vec<HitRect> hits;
    Vec<ScrollRect> scrolls;
    Vec<TextHit> texts;
    int textDocLen = 0;
    int selA = -1;
    int selB = -1;
    TextMeasCache textCache;

    PaintCtx() = default;
};

struct FocusRect {
    int id = 0;
    int trapId = 0;
    Bounds bounds = {};
};

enum class InputEventKind : uint8_t {
    Change
};

struct InputEvent {
    InputEventKind kind = InputEventKind::Change;
};

struct LineInput {
    char buf[512] = {};
    int len = 0;
    int cursor = 0;
    char placeholder[128] = {};
    bool focused = false;
    Listener onChange = {};

    EntityId blink = {};
};

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
void TextMeasBeginFrame(PaintCtx* ctx);
void TextMeasEndFrame(PaintCtx* ctx);
void TextMeasClear(PaintCtx* ctx);
int TextIndexAt(PaintCtx* ctx, Str s, float fontSize, float maxW, bool wrap,
                float relX, float relY);
void PaintTextRange(PaintCtx* ctx, Str s, float fontSize, float maxW, bool wrap,
                    float x, float y, int u8a, int u8b, Rgba color);
void LayoutEl(PaintCtx* ctx, El* e, float x, float y, float availW,
              float availH, float inheritFont, Rgba inheritFg);
void PaintEl(PaintCtx* ctx, El* e);
int HitTest(PaintCtx* ctx, float x, float y);
const HitRect* HitTestRect(PaintCtx* ctx, float x, float y);
const ScrollRect* HitScrollRect(PaintCtx* ctx, float x, float y);
int TextHitOffsetAt(PaintCtx* ctx, float x, float y, bool nearest);
int CopyTextHits(PaintCtx* ctx, int selA, int selB, char* out, int cap);

bool TextWordRangeAt(Str s, int off, int* outA, int* outB);

void TextLineRangeAt(Str s, int off, int* outA, int* outB);

bool TextMultiClickRange(PaintCtx* ctx, float x, float y, int clickCount,
                         int* outA, int* outB);
int HashClickId(Str s);

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

struct App {
    PaintApp* paint = nullptr;
    ThemeMode themeMode = ThemeMode::Light;
    Vec<Window*> windows;

    Vec<EntitySlot> entities;
    Vec<int32_t> freeSlots;
    int exitCode = 0;

    App() = default;
};

struct PlatWindow;

struct Window {
    App* app = nullptr;
    PlatWindow* plat = nullptr;
    PaintCtx paint = {};
    Arena* frameArena = nullptr;

    EntityId root = {};
    int hoverId = 0;
    int focusId = 0;
    float mouseX = 0;
    float mouseY = 0;

    CursorKind cursor = CursorKind::Arrow;
    bool maximized = false;
    bool running = true;
    bool anim = false;
    bool mouseDown = false;

    double lastDownAt = 0;
    float lastDownX = 0;
    float lastDownY = 0;
    MouseButton lastDownButton = MouseButton::Left;
    int clickRun = 0;

    int pressedId = 0;
    bool eatReturn = false;
    LineInput* input = nullptr;
    Overlay overlay = {};
    MenuState menu = {};
    Vec<FocusRect> focusEls;
    Vec<KeyedSlot> keyed;
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

    LineInput* prevInput = nullptr;

    FrameTiming frameTrace[kFrameTraceCap] = {};
    uint64_t frameSeq = 0;

    Window() = default;
};

struct Ctx {
    App* app = nullptr;
    Window* win = nullptr;
    Arena* a = nullptr;
    EntityId self = {};

    const Theme& theme() const;
    ThemeMode themeMode() const;
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
Listener ListenTo(Entity<T> e, void (*fn)(T*, Ctx*, const E*, intptr_t),
                  intptr_t arg) {
    Listener l;
    l.fn = (void*)fn;
    l.view = e.id;
    l.arg = arg;
    l.hasArg = true;
    return l;
}

void Notify(Ctx* cx);
void NotifyApp(App* app);
void ListenerCall(App* app, Window* win, const Listener& l, const void* ev);

El* EntityRender(App* app, Window* win, Arena* a, EntityId id);

void* WindowKeyedState(Window* win, uint32_t key, int size, DropFn drop);
void WindowKeyedFree(Window* win);

template <typename T>
T* KeyedState(Ctx* cx, uint32_t key) {
    void* p = WindowKeyedState(cx->win, key, (int)sizeof(T), &EntityDropT<T>);
    return (T*)p;
}

void WindowOnKey(Window* win, Listener l);

void WindowOnUnhandledClick(Window* win, Listener l);

void WindowOnMouseDown(Window* win, Listener l);
void WindowOnMouseUp(Window* win, Listener l);
void WindowOnMouseMove(Window* win, Listener l);
void WindowOnMouseExit(Window* win, Listener l);
void WindowOnScrollWheel(Window* win, Listener l);

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
int AppRun(App* app);
Window* WindowOpen(App* app, Str title, int dipW, int dipH, WinOpts opts);
void AppSetTitle(Window* win, Str title);
void AppRequestAnim(Window* win, bool on);

void FocusCollect(Window* win, El* root);
int FocusNext(Window* win, int trapId, bool backward);
void AppQuit(Window* win);
void AppInvalidate(Window* win);
void AppMinimize(Window* win);
void AppToggleMaximize(Window* win);
void AppClose(Window* win);
void AppDrag(Window* win);
bool AppIsMaximized(Window* win);
}

int GpuiMain(int argc, char** argv);

#line 1 "src/gpui/Assets.h"

namespace gpui {

void AssetsClear();
void AssetsAddRoot(Str dir);

void AssetsAddDefaultRoots(Str exampleName);
bool AssetsLoad(Str relPath, Vec<uint8_t>* out);
TempStr AssetsLoadTextTemp(Str relPath);
bool AssetsExists(Str relPath);
}

#line 1 "src/gpui/Svg.h"

namespace gpui {

bool SvgDraw(PaintCtx* ctx, Str assetPath, float x, float y, float size,
             Rgba color);

Str IconNamePath(IconName name);
}

#line 1 "src/ui/Primitive.h"

namespace gpui {

inline El* UiRoot(Arena* a, Str id, int clickId = 0) {
    El* e = Div(a)->Id(id);
    if (clickId) {
        e->Click(clickId)->FocusId(clickId);
    }
    return e;
}
}

#line 1 "src/ui/Accordion.h"

namespace gpui {

struct Accordion {
    static El* New(Ctx* cx, Str id);
};

struct AccordionTrigger {
    static El* New(Ctx* cx, Str id, int clickId = 0);
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

    static AccordionItem* New(Ctx* cx);
    AccordionItem* Open(bool v);
    AccordionItem* Header(El* header);
    AccordionItem* Panel(El* panel);
    El* IntoEl();
};
}

#line 1 "src/ui/AlertDialog.h"

namespace gpui {

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

struct AlertDialog {
    El* root = nullptr;

    static AlertDialog* New(Ctx* cx);
    AlertDialog* Backdrop(El* backdrop);
    AlertDialog* Popup(El* popup);
    El* IntoEl();
};
}

#line 1 "src/ui/Avatar.h"

namespace gpui {

struct Avatar {
    El* root = nullptr;
    El* fallback = nullptr;

    static Avatar* New(Ctx* cx);
    Avatar* Size(float px);
    Avatar* Fallback(El* fallback);
    El* IntoEl();
};

struct AvatarFallback {
    static El* New(Ctx* cx);
};
}

#line 1 "src/ui/Button.h"

namespace gpui {

struct Button {

    static El* New(Ctx* cx, Str id, int clickId = 0);
};
}

#line 1 "src/ui/Calendar.h"

namespace gpui {

struct Calendar {
    static El* New(Ctx* cx, Str id);
};
struct CalendarItem {
    static El* New(Ctx* cx, int clickId = 0);
};
}

#line 1 "src/ui/Checkbox.h"

namespace gpui {

struct Checkbox {
    static El* New(Ctx* cx, Str id, int clickId = 0);
};

struct CheckboxIndicator {
    static El* New(Ctx* cx);
};
}

#line 1 "src/ui/Collapsible.h"

namespace gpui {

struct Collapsible {
    El* root = nullptr;
    bool open = false;

    static Collapsible* New(Ctx* cx);
    Collapsible* Open(bool v);
    Collapsible* Child(El* e);
    Collapsible* Content(El* e);
    El* IntoEl();
};
}

#line 1 "src/ui/ColorPicker.h"

namespace gpui {

struct ColorPicker {
    static El* New(Ctx* cx, Str id);
};
struct ColorSwatch {
    static El* New(Ctx* cx, Str id, int clickId = 0);
};
}

#line 1 "src/ui/Combobox.h"

namespace gpui {

struct Combobox {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/ui/DatePicker.h"

namespace gpui {

struct DatePicker {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/ui/Dialog.h"

namespace gpui {

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

struct Dialog {
    El* root = nullptr;

    static Dialog* New(Ctx* cx);
    Dialog* Backdrop(El* backdrop);
    Dialog* Popup(El* popup);
    El* IntoEl();
};
}

#line 1 "src/ui/HoverCard.h"

namespace gpui {

struct HoverCard {
    El* root = nullptr;

    static HoverCard* New(Ctx* cx, Str id);
    HoverCard* Trigger(El* trigger);
    HoverCard* Content(El* content);
    El* IntoEl();
};
}

#line 1 "src/ui/Input.h"

namespace gpui {

struct InputBase {
    static El* New(Ctx* cx, Str id, int clickId = 0);
};

struct InputEditorStyle {
    Rgba foreground = Rgb(0x17, 0x17, 0x17);
    Rgba mutedForeground = Rgb(0x73, 0x73, 0x73);
    Rgba caret = Rgb(0x17, 0x17, 0x17);
    float fontSize = 12;

    bool mask = false;
    int align = 0;
};

struct Input {
    static El* New(Ctx* cx, LineInput* state);
    static El* New(Ctx* cx, LineInput* state, const InputEditorStyle& style);
};

struct Textarea {
    static El* New(Ctx* cx, const char* text, bool caret = false);

    static El* New(Ctx* cx, const char* text, const InputEditorStyle& style,
                   bool caret = false, bool softWrap = false);
};

struct Editor {
    static El* New(Ctx* cx, const char* text);
    static El* New(Ctx* cx, const char* text, int cursor, bool caret);
};
}

#line 1 "src/ui/Link.h"

namespace gpui {

struct Link {
    static El* New(Ctx* cx, Str id, int clickId = 0);
};
}

#line 1 "src/ui/NumberInput.h"

namespace gpui {

struct NumberInput {
    static El* New(Ctx* cx);
};
}

#line 1 "src/ui/OtpInput.h"

namespace gpui {

struct OtpInput {
    static El* New(Ctx* cx, int clickId = 0);
};
}

#line 1 "src/ui/Pagination.h"

namespace gpui {

struct Pagination {
    static El* New(Ctx* cx, Str id);
};
struct PaginationItem {
    static El* New(Ctx* cx, Str id, int clickId = 0);
};
}

#line 1 "src/ui/Popover.h"

namespace gpui {

struct Popover {
    El* root = nullptr;

    static Popover* New(Ctx* cx, Str id);
    Popover* Trigger(El* trigger);
    Popover* Content(El* content);
    El* IntoEl();
};
}

#line 1 "src/ui/Popup.h"

namespace gpui {

struct Popup {
    El* root = nullptr;

    bool anchorRight = false;

    static Popup* New(Ctx* cx, Str id, El* trigger);
    Popup* AnchorRight(bool on = true);
    Popup* Content(El* content);
    El* IntoEl();
};
}

#line 1 "src/ui/Progress.h"

namespace gpui {

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

#line 1 "src/ui/Radio.h"

namespace gpui {

struct Radio {
    static El* New(Ctx* cx, Str id, int clickId = 0);
};

struct RadioGroup {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/ui/Resizable.h"

namespace gpui {

struct Resizable {
    static El* New(Ctx* cx, Str id);
};
struct ResizablePanel {
    static El* New(Ctx* cx);
};
}

#line 1 "src/ui/Scrollbar.h"

namespace gpui {

struct Scrollbar {
    static El* New(Ctx* cx);
};
}

#line 1 "src/ui/Select.h"

namespace gpui {

struct Select {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/ui/Sheet.h"

namespace gpui {

struct Sheet {
    El* root = nullptr;

    static Sheet* New(Ctx* cx);
    Sheet* Overlay(El* overlay);
    Sheet* Surface(El* surface);
    El* IntoEl();
};
}

#line 1 "src/ui/Slider.h"

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

#line 1 "src/ui/Switch.h"

namespace gpui {

struct Switch {
    static El* New(Ctx* cx, Str id, int clickId = 0);
};

struct SwitchTrack {
    static El* New(Ctx* cx, Str id);
};

struct SwitchThumb {
    static El* New(Ctx* cx);
};
}

#line 1 "src/ui/Table.h"

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
}

#line 1 "src/ui/Tabs.h"

namespace gpui {

struct Tabs {
    static El* New(Ctx* cx, Str id);
};
struct Tab {
    static El* New(Ctx* cx, Str id, int clickId = 0);
};
}

#line 1 "src/ui/TextSelection.h"

namespace gpui {

struct TextSelection {
    static El* New(Ctx* cx, Str id, int clickId = 0);
};
}

#line 1 "src/ui/Toast.h"

namespace gpui {

struct Toast {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/ui/Toggle.h"

namespace gpui {

struct Toggle {
    static El* New(Ctx* cx, Str id, int clickId = 0);
};

struct ToggleGroup {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/ui/Tooltip.h"

namespace gpui {

struct Tooltip {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/ui/Tree.h"

namespace gpui {

struct Tree {
    static El* New(Ctx* cx);
};
struct TreeItem {
    static El* New(Ctx* cx, int clickId = 0);
};
}

#line 1 "src/ui/VirtualList.h"

namespace gpui {

struct VirtualList {
    static El* New(Ctx* cx, Str id);
};
}

#line 1 "src/ui/Ui.h"

#line 1 "src/component/Common.h"

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

inline El* BindClick(El* e, Str id, Listener onClick) {
    int cid = HashClickId(id);
    e->Id(id)->Click(cid)->FocusId(cid);
    if (onClick.IsValid()) {
        e->OnClick(onClick);
    }
    return e;
}

}
}

#line 1 "src/component/Accordion.h"

namespace gpui {

namespace component {

struct AccordionItem {
    Str title = {};
    Str body = {};
    bool open = false;
    IconName icon = IconName::None;
    Str tag = {};
    bool settings = false;
};

struct Accordion {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    bool multiple = false;
    bool bordered = true;
    bool disabled = false;
    UiSize size = UiSize::Medium;
    AccordionItem items[8] = {};
    int nItems = 0;
    Listener onToggle;

    static Accordion* New(Ctx* cx, Str id);
    Accordion* Multiple(bool v);
    Accordion* Bordered(bool v);
    Accordion* Disabled(bool v);
    Accordion* WithSize(UiSize s);
    Accordion* Item(Str title, Str body, bool open);
    Accordion* SettingsItem(Str title, Str body, bool open, IconName icon,
                            Str tag);
    Accordion* OnToggle(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Alert.h"

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
    Alert* Content(El* e);
    Alert* Banner();
    Alert* Visible(bool v);
    Alert* OnClose(Listener fn);
    Alert* WithSize(UiSize s);
    El* IntoEl();
};

}
}

#line 1 "src/component/Avatar.h"

namespace gpui {

namespace component {

float AvatarSizePx(UiSize s);

struct Avatar {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str initials = {};
    Rgba bg = {};
    bool hasBg = false;
    float size = 48;

    float textPx = -1;
    float radius = -1;
    float borderW = 1;
    Rgba borderC = {};
    bool hasBorderC = false;
    IconName placeholder = IconName::User;

    static Avatar* New(Ctx* cx);
    Avatar* Initials(Str s);
    Avatar* Bg(Rgba c);
    Avatar* Size(float v);
    Avatar* WithSize(UiSize s);
    Avatar* Radius(float v);
    Avatar* Border(float w, Rgba c);
    Avatar* Placeholder(IconName n);
    El* IntoEl();
};

}
}

#line 1 "src/component/Badge.h"

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

#line 1 "src/component/Breadcrumb.h"

namespace gpui {

namespace component {

struct Breadcrumb {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str items[8] = {};
    int n = 0;
    int clickBase = 0;
    Listener onClick;

    static Breadcrumb* New(Ctx* cx);
    Breadcrumb* Item(Str s);
    Breadcrumb* OnClick(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Button.h"

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
    ButtonVariant variant = ButtonVariant::Default;
    UiSize size = UiSize::Medium;
    bool outline = false;
    bool disabled = false;
    bool loading = false;
    bool compact = false;
    bool selected = false;
    bool dropdown = false;
    bool hasCustom = false;
    Rgba custom = {};
    Str tooltip = {};
    El* extra = nullptr;
    Listener onClick;

    static Button* New(Ctx* cx, Str id);
    Button* Label(Str s);
    Button* Icon(IconName n);
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
    Button* Selected(bool v);
    Button* DropdownCaret(bool v = true);
    Button* Custom(Rgba c);
    Button* Extra(El* e);
    Button* Loading(bool v);
    Button* Disabled(bool v);
    Button* WithSize(UiSize s);
    Button* Tooltip(Str s);
    Button* OnClick(Listener l);
    El* IntoEl();
};

}
}

#line 1 "src/component/Calendar.h"

namespace gpui {

namespace component {

struct Calendar {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    int year = 2026;
    int month = 1;
    int day = 1;
    Listener onDay;
    Listener onPrev;
    Listener onNext;

    static Calendar* New(Ctx* cx);
    Calendar* Year(int y);
    Calendar* Month(int m);
    Calendar* Day(int d);
    Calendar* OnDay(Listener fn);
    Calendar* OnPrev(Listener fn);
    Calendar* OnNext(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Chart.h"

namespace gpui {

namespace component {

struct PieSlice {
    float value = 0;
    Rgba color = {};

    float outerInset = 0;
};

struct PieChart {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    PieSlice slices[12] = {};
    int n = 0;
    float outerRadius = 100;
    float innerRadius = 0;
    float padAngle = 0;

    static PieChart* New(Ctx* cx);
    PieChart* Slice(float value, Rgba color, float outerInset = 0);
    PieChart* OuterRadius(float r);
    PieChart* InnerRadius(float r);
    PieChart* PadAngle(float radians);
    El* IntoEl();
};

struct AreaChart {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    const float* ys = nullptr;
    int n = 0;
    const char* const* labels = nullptr;
    int tickMargin = 15;

    bool overlay = false;
    Rgba stroke = {};
    Rgba fill = {};

    static AreaChart* New(Ctx* cx, const float* ys, int n);
    AreaChart* Stroke(Rgba c);
    AreaChart* Fill(Rgba c);
    AreaChart* Labels(const char* const* l);
    AreaChart* TickMargin(int n);
    AreaChart* Overlay(bool v = true);
    El* IntoEl();
};

}
}

#line 1 "src/component/Checkbox.h"

namespace gpui {

namespace component {

struct Checkbox {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Str label = {};
    Str hint = {};
    bool checked = false;
    bool disabled = false;
    UiSize size = UiSize::Medium;
    Str tooltip = {};
    float w = 0;
    Listener onClick;

    static Checkbox* New(Ctx* cx, Str id);
    Checkbox* Label(Str s);
    Checkbox* Hint(Str s);
    Checkbox* Checked(bool v);
    Checkbox* Disabled(bool v);
    Checkbox* WithSize(UiSize s);
    Checkbox* W(float v);
    Checkbox* Tooltip(Str s);
    Checkbox* OnClick(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Clipboard.h"

namespace gpui {

namespace component {

struct Clipboard {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str value = {};
    Listener onCopy;

    static Clipboard* New(Ctx* cx, Str value);
    Clipboard* OnCopy(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Collapsible.h"

namespace gpui {

namespace component {

struct Collapsible {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    bool open = false;
    El* trigger = nullptr;
    El* content = nullptr;

    static Collapsible* New(Ctx* cx);
    Collapsible* Open(bool v);
    Collapsible* Trigger(El* e);
    Collapsible* Content(El* e);
    El* IntoEl();
};

}
}

#line 1 "src/component/ColorPicker.h"

namespace gpui {

namespace component {

struct ColorPicker {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    uint32_t hex = 0;

    bool hasValue = false;
    Str label = {};
    UiSize size = UiSize::Medium;
    bool open = false;
    Listener onChange;
    Listener onToggle;

    static ColorPicker* New(Ctx* cx);
    ColorPicker* Hex(uint32_t h);
    ColorPicker* Label(Str s);
    ColorPicker* WithSize(UiSize s);
    ColorPicker* Open(bool v);
    ColorPicker* OnChange(Listener fn);
    ColorPicker* OnToggle(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Select.h"

namespace gpui {

namespace component {

struct Select {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Str options[24] = {};
    int n = 0;

    int selected = -1;
    Str placeholder = {};
    Str titlePrefix = {};
    Str empty = {};
    float width = kFill;
    float menuWidth = 0;
    float menuMaxH = 0;
    UiSize size = UiSize::Medium;
    IconName icon = IconName::None;
    bool disabled = false;
    bool cleanable = false;
    bool appearance = true;
    bool open = false;
    Listener onChange;
    Listener onToggle;
    Listener onClear;

    static Select* New(Ctx* cx, Str id);
    Select* Option(Str s);
    Select* Options(const char* const* items, int count);
    Select* Selected(int i);
    Select* Placeholder(Str s);
    Select* TitlePrefix(Str s);
    Select* Empty(Str s);
    Select* W(float v);
    Select* MenuWidth(float v);
    Select* MenuMaxH(float v);
    Select* WithSize(UiSize s);
    Select* Icon(IconName n);
    Select* Disabled(bool v);
    Select* Cleanable(bool v = true);
    Select* Appearance(bool v);
    Select* Open(bool v);
    Select* OnChange(Listener fn);
    Select* OnToggle(Listener fn);
    Select* OnClear(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Combobox.h"

namespace gpui {

namespace component {

struct Combobox {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    Str options[12] = {};
    int n = 0;

    Str selected = {};
    Str placeholder = {};
    Str searchPlaceholder = {};

    IconName icon = IconName::None;
    float width = 280;
    bool open = false;
    LineInput* query = nullptr;
    Listener onChange;
    Listener onToggle;

    static Combobox* New(Ctx* cx, Str id);
    Combobox* Option(Str s);
    Combobox* Options(const char* const* items, int count);
    Combobox* Selected(Str s);
    Combobox* Placeholder(Str s);
    Combobox* SearchPlaceholder(Str s);
    Combobox* Icon(IconName n);
    Combobox* W(float v);
    Combobox* Open(bool v);
    Combobox* Query(LineInput* q);
    Combobox* OnChange(Listener fn);
    Combobox* OnToggle(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/DatePicker.h"

namespace gpui {

namespace component {

enum class DateFormat : uint8_t {
    Slash,
    Dash
};

struct DatePicker {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    int year = 2026;
    int month = 1;
    int day = 1;

    int year2 = 0;
    int month2 = 0;
    int day2 = 0;
    Str placeholder = {};
    DateFormat format = DateFormat::Slash;
    float width = kFill;

    bool cleanable = false;
    bool appearance = true;
    bool open = false;
    Listener onToggle;
    Listener onDay;
    Listener onClear;

    static DatePicker* New(Ctx* cx);
    DatePicker* Year(int y);
    DatePicker* Month(int m);
    DatePicker* Day(int d);
    DatePicker* RangeEnd(int y, int m, int d);
    DatePicker* Placeholder(Str s);
    DatePicker* Format(DateFormat f);
    DatePicker* W(float v);
    DatePicker* Cleanable(bool v = true);
    DatePicker* Appearance(bool v);
    DatePicker* Open(bool v);
    DatePicker* OnToggle(Listener fn);
    DatePicker* OnDay(Listener fn);
    DatePicker* OnClear(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/DescriptionList.h"

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
    UiSize size = UiSize::Medium;

    static DescriptionList* New(Ctx* cx);

    DescriptionList* Item(Str label, Str value, int span = 1);
    DescriptionList* ItemEl(Str label, El* value, int span = 1);
    DescriptionList* Separator();
    DescriptionList* Columns(int n);
    DescriptionList* LabelWidth(float w);
    DescriptionList* Bordered(bool v);
    DescriptionList* WithSize(UiSize s);
    El* IntoEl();
};

}
}

#line 1 "src/component/Dialog.h"

namespace gpui {

namespace component {

struct Dialog {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str title = {};
    Str description = {};
    bool open = false;
    El* body = nullptr;

    Listener onClose;
    Listener onCancel;
    Listener onOk;

    float width = 448;

    bool overlay = true;

    IconName icon = IconName::None;
    Rgba iconColor = {};
    bool hasIconColor = false;
    float iconSize = 16;
    bool headerCentered = false;

    Str okText = {};
    Str cancelText = {};
    ButtonVariant okVariant = ButtonVariant::Primary;
    bool okOutline = false;
    bool showCancel = true;

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
    Dialog* W(float px);
    Dialog* Overlay(bool v);
    Dialog* Icon(IconName n, Rgba color, float size = 16);
    Dialog* HeaderCentered(bool v = true);
    Dialog* OkText(Str s);
    Dialog* CancelText(Str s);
    Dialog* OkVariant(ButtonVariant v, bool outline = false);
    Dialog* ShowCancel(bool v);
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
    El* Header();
    El* Actions();
};

}
}

#line 1 "src/component/Dock.h"

namespace gpui {

namespace component {

struct Dock {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    El* left = nullptr;
    El* center = nullptr;
    El* right = nullptr;

    static Dock* New(Ctx* cx);
    Dock* Left(El* e);
    Dock* Center(El* e);
    Dock* Right(El* e);
    El* IntoEl();
};

}
}

#line 1 "src/component/Form.h"

namespace gpui {

namespace component {

struct FormField {
    Str label = {};
    El* control = nullptr;

    Str description = {};
    bool required = false;

    bool spanAll = false;
};

struct Form {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    FormField fields[12] = {};
    int n = 0;
    bool horizontal = false;
    int columns = 1;
    float labelWidth = 0;

    static Form* New(Ctx* cx);

    Form* Field(Str label, El* control);
    Form* Required(bool v = true);
    Form* Description(Str s);
    Form* SpanAll(bool v = true);
    Form* Horizontal(bool v = true);
    Form* Columns(int n);
    Form* LabelWidth(float w);
    El* IntoEl();
};

}
}

#line 1 "src/component/GroupBox.h"

namespace gpui {

namespace component {

struct GroupBox {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str title = {};
    El* child = nullptr;

    bool outline = false;
    bool filled = false;

    bool titleSemibold = false;
    float titlePadX = 0;
    Rgba contentBg = {};
    bool hasContentBg = false;
    float contentRadius = -1;
    float contentPad = -1;
    float contentBorder = -1;

    static GroupBox* New(Ctx* cx, Str title);
    GroupBox* Child(El* e);
    GroupBox* Outline();
    GroupBox* Filled(bool v);
    GroupBox* TitleSemibold(bool v = true);
    GroupBox* TitlePadX(float px);
    GroupBox* ContentBg(Rgba c);
    GroupBox* ContentRadius(float px);
    GroupBox* ContentPad(float px);
    GroupBox* ContentBorder(float px);
    El* IntoEl();
};

}
}

#line 1 "src/component/Highlighter.h"

namespace gpui {

namespace component {

struct Highlighter {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    const char* text = nullptr;

    static Highlighter* New(Ctx* cx, const char* text);
    El* IntoEl();
};

}
}

#line 1 "src/component/History.h"

namespace gpui {

namespace component {

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
}

#line 1 "src/component/HoverCard.h"

namespace gpui {

namespace component {

enum class HoverCardAnchor : uint8_t {
    BottomLeft,
    BottomCenter,
    BottomRight,
    TopLeft,
    TopCenter,
    TopRight
};

struct HoverCard {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    El* trigger = nullptr;
    El* content = nullptr;
    bool open = false;
    HoverCardAnchor anchor = HoverCardAnchor::BottomLeft;

    static HoverCard* New(Ctx* cx);
    static HoverCard* New(Ctx* cx, Str id);
    HoverCard* Trigger(El* e);
    HoverCard* Content(El* e);
    HoverCard* Open(bool v);
    HoverCard* Anchor(HoverCardAnchor a);
    El* IntoEl();
};

}
}

#line 1 "src/component/Icon.h"

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

#line 1 "src/component/Input.h"

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
    LineInput* state = nullptr;
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
    Rgba textColor = {};
    bool hasTextColor = false;
    Listener onChange;
    Listener onFocus;
    Listener onClear;
    Listener onToggleMask;

    static Input* New(Ctx* cx, Str id, LineInput* state);
    Input* Label(Str s);
    Input* WithSize(UiSize s);
    Input* Align(InputAlign v);
    Input* Disabled(bool v);
    Input* Cleanable(bool v = true);
    Input* Masked(bool v);
    Input* MaskToggle(bool v = true);
    Input* Appearance(bool v);
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

struct Textarea {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    const char* text = nullptr;
    Str placeholder = {};
    int rows = 0;
    float height = 0;
    bool softWrap = true;
    Listener onFocus;

    static Textarea* New(Ctx* cx, Str id, const char* text);

    Textarea* Rows(int n);
    Textarea* H(float px);
    Textarea* Placeholder(Str s);
    Textarea* SoftWrap(bool v);
    Textarea* OnFocus(Listener fn);
    El* IntoEl();
};

struct NumberInput {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str id = {};
    LineInput* state = nullptr;
    float width = kFill;
    UiSize size = UiSize::Medium;
    bool disabled = false;
    bool appearance = true;
    El* suffix = nullptr;
    Rgba bg = {};
    bool hasBg = false;
    Rgba textColor = {};
    bool hasTextColor = false;
    Listener onInc;
    Listener onDec;
    Listener onFocus;

    static NumberInput* New(Ctx* cx, LineInput* state);
    static NumberInput* New(Ctx* cx, Str id, LineInput* state);

    NumberInput* W(float v);
    NumberInput* WithSize(UiSize s);
    NumberInput* Disabled(bool v);
    NumberInput* Appearance(bool v);
    NumberInput* Suffix(El* el);
    NumberInput* Bg(Rgba c);
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
    UiSize size = UiSize::Medium;
    float cellPx = 0;
    Listener onFocus;

    static OtpInput* New(Ctx* cx, const char* value, int len);
    OtpInput* Id(Str s);
    OtpInput* Slots(int n);
    OtpInput* Groups(int n);
    OtpInput* Masked(bool v);
    OtpInput* Disabled(bool v);
    OtpInput* WithSize(UiSize s);
    OtpInput* CellSize(float px);
    OtpInput* OnFocus(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Kbd.h"

namespace gpui {

namespace component {

struct Kbd {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str stroke = {};
    bool appearance = true;
    bool outline = false;

    static Kbd* New(Ctx* cx, Str stroke);
    Kbd* Appearance(bool v);
    Kbd* Outline();
    El* IntoEl();
};

}
}

#line 1 "src/component/Label.h"

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

    static Label* New(Ctx* cx, Str text);
    Label* Secondary(Str s);
    Label* Masked(bool v);
    Label* Semibold();
    Label* Font(float px);
    El* IntoEl();
};

}
}

#line 1 "src/component/Link.h"

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

#line 1 "src/component/List.h"

namespace gpui {

namespace component {

struct List {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str items[32] = {};
    int n = 0;
    int selected = -1;
    Listener onSelect;

    static List* New(Ctx* cx);
    List* Item(Str s);
    List* Selected(int i);
    List* OnSelect(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Menu.h"

namespace gpui {

namespace component {

struct Menu {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str items[8] = {};
    int n = 0;
    Listener onClick;

    static Menu* New(Ctx* cx);
    Menu* Item(Str s);
    Menu* OnClick(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/NativeMenu.h"

namespace gpui {

namespace component {

struct NativeMenu {
    static El* New(Ctx* cx, Menu* menu);
};

}
}

#line 1 "src/component/Notification.h"

namespace gpui {

namespace component {

enum class NotificationKind : uint8_t {
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

struct Notification {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    NotificationKind kind = NotificationKind::Info;
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

}
}

#line 1 "src/component/Pagination.h"

namespace gpui {

namespace component {

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

#line 1 "src/component/Plot.h"

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

struct ScaleOrdinal {
    int rangeLen = 0;

    int unknown = -1;

    int Map(int domainIndex) const;
};

}
}

#line 1 "src/component/Popover.h"

namespace gpui {

namespace component {

struct Popover {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    El* trigger = nullptr;
    El* content = nullptr;
    bool open = false;

    static Popover* New(Ctx* cx);
    Popover* Trigger(El* e);
    Popover* Content(El* e);
    Popover* Open(bool v);
    El* IntoEl();
};

}
}

#line 1 "src/component/Progress.h"

namespace gpui {

namespace component {

struct Progress {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    float value = 0;
    float w = 200;
    float h = 8;

    static Progress* New(Ctx* cx);
    Progress* Value(float v);
    Progress* W(float v);
    Progress* H(float v);
    El* IntoEl();
};

struct ProgressCircle {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    float value = 0;
    float size = 48;
    Rgba color = {};
    bool hasColor = false;
    bool showLabel = true;

    static ProgressCircle* New(Ctx* cx);
    ProgressCircle* Value(float v);
    ProgressCircle* Size(float v);
    ProgressCircle* Color(Rgba c);
    ProgressCircle* Label(bool v);
    El* IntoEl();
};

}
}

#line 1 "src/component/Radio.h"

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
    Listener onClick;

    static Radio* New(Ctx* cx, Str id);
    Radio* Label(Str s);
    Radio* Hint(Str s);
    Radio* Checked(bool v);
    Radio* Disabled(bool v);
    Radio* WithSize(UiSize s);
    Radio* OnClick(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Rating.h"

namespace gpui {

namespace component {

struct Rating {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    int value = 0;
    int max = 5;
    bool disabled = false;
    Rgba color = {};
    bool hasColor = false;
    UiSize size = UiSize::Medium;
    Listener onChange;

    static Rating* New(Ctx* cx);
    Rating* Value(int v);
    Rating* Max(int v);
    Rating* Disabled(bool v);
    Rating* Color(Rgba c);
    Rating* WithSize(UiSize s);
    Rating* OnChange(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Root.h"

namespace gpui {

namespace component {

struct Root {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    El* child = nullptr;
    bool bordered = true;

    static Root* New(Ctx* cx);
    Root* Bordered(bool v);
    Root* Child(El* e);
    El* IntoEl();
};

}
}

#line 1 "src/component/Scroll.h"

namespace gpui {

namespace component {

struct Scrollable {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    El* child = nullptr;
    float scrollY = 0;
    float h = 200;

    static Scrollable* New(Ctx* cx);
    Scrollable* Child(El* e);
    Scrollable* ScrollY(float v);
    Scrollable* H(float v);
    El* IntoEl();
};

}
}

#line 1 "src/component/SearchableList.h"

namespace gpui {

namespace component {

struct SearchableList {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    LineInput* query = nullptr;
    Str items[32] = {};
    int n = 0;
    Listener onSelect;

    static SearchableList* New(Ctx* cx, LineInput* query);
    SearchableList* Item(Str s);
    SearchableList* OnSelect(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Separator.h"

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

#line 1 "src/component/Setting.h"

namespace gpui {

namespace component {

struct SettingItem {
    Str label = {};
    El* control = nullptr;
};

struct Setting {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str title = {};
    SettingItem items[12] = {};
    int n = 0;

    static Setting* New(Ctx* cx, Str title);
    Setting* Item(Str label, El* control);
    El* IntoEl();
};

}
}

#line 1 "src/component/Sheet.h"

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
    Listener onClose;

    static Sheet* New(Ctx* cx);
    Sheet* Title(Str s);
    Sheet* Placement(SheetPlacement p);
    Sheet* Size(float px);
    Sheet* Overlay(bool v);
    Sheet* Open(bool v);
    Sheet* Body(El* e);
    Sheet* OnClose(Listener fn);
    El* IntoEl(WinSize size);
};

}
}

#line 1 "src/component/Sidebar.h"

namespace gpui {

namespace component {

struct Sidebar {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str title = {};
    Str items[8] = {};
    int n = 0;
    int selected = 0;
    bool collapsed = false;
    Listener onSelect;

    static Sidebar* New(Ctx* cx);
    Sidebar* Title(Str s);
    Sidebar* Item(Str s);
    Sidebar* Selected(int i);
    Sidebar* Collapsed(bool v);
    Sidebar* OnSelect(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Skeleton.h"

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

#line 1 "src/component/Slider.h"

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
    Listener onChange;

    static Slider* New(Ctx* cx, Str id, SliderState* state);
    Slider* Reverse(bool v = true);
    Slider* Vertical(bool v = true);
    Slider* WithAxis(Axis v);
    Slider* Disabled(bool v = true);
    Slider* W(float px);

    Slider* OnChange(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Spinner.h"

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

    static Spinner* New(Ctx* cx);
    Spinner* WithSize(UiSize s);
    Spinner* Size(float v);
    Spinner* Icon(IconName n);
    Spinner* Color(Rgba c);
    El* IntoEl();
};

}
}

#line 1 "src/component/StatusBar.h"

namespace gpui {

namespace component {

struct StatusBar {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str left = {};
    Str center = {};
    Str right = {};
    bool hasLeft = false;
    bool hasCenter = false;
    bool hasRight = false;

    static StatusBar* New(Ctx* cx);
    StatusBar* Left(Str s);

    StatusBar* Center(Str s);
    StatusBar* Right(Str s);
    El* IntoEl();
};

}
}

#line 1 "src/component/Stepper.h"

namespace gpui {

namespace component {

struct Stepper {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str steps[8] = {};
    IconName icons[8] = {};
    int n = 0;
    int current = 0;

    float width = kFill;
    Listener onChange;

    static Stepper* New(Ctx* cx);
    Stepper* Step(Str s);
    Stepper* Step(Str s, IconName icon);
    Stepper* Current(int i);
    Stepper* W(float px);
    Stepper* OnChange(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Switch.h"

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

#line 1 "src/component/Tab.h"

namespace gpui {

namespace component {

struct Tabs {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str labels[8] = {};
    int n = 0;
    int selected = 0;
    Listener onChange;

    static Tabs* New(Ctx* cx);
    Tabs* Tab(Str label);
    Tabs* Selected(int i);
    Tabs* OnChange(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/Table.h"

namespace gpui {

namespace component {

struct Table {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    const char** heads = nullptr;
    int nHeads = 0;
    const char*** rows = nullptr;
    int nRows = 0;

    static Table* New(Ctx* cx);
    Table* Heads(const char** h, int n);
    Table* Rows(const char*** r, int n);
    El* IntoEl();
};

}
}

#line 1 "src/component/Tag.h"

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
    Tag* Custom(Rgba bg, Rgba fg);
    El* IntoEl();
};

}
}

#line 1 "src/component/Text.h"

namespace gpui {

namespace component {

enum MdMark : uint8_t {
    MdBold = 1 << 0,
    MdItalic = 1 << 1,
    MdCode = 1 << 2,
    MdDel = 1 << 3,
    MdUnderline = 1 << 4,
    MdLink = 1 << 5,
};

struct MdRun {
    Str text = {};

    Str href = {};
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

    int start = 1;

    uint8_t level = 0;

    uint8_t align = 0;
    bool ordered = false;

    bool head = false;
};

struct TextView {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    Str source = {};

    float baseFont = 16;

    float headingFont = 14;

    float codeFont = 13;

    float paragraphGap = 16;

    bool selectable = false;

    float tableColW = 64;

    static TextView* New(Ctx* cx, Str source);
    TextView* Font(float px);
    TextView* HeadingFont(float px);
    TextView* Selectable(bool on = true);
    TextView* TableColumnWidth(float px);
    TextView* ParagraphGap(float px);
    El* IntoEl();

  private:

    El* Block(MdNode* n, int depth, bool inList, bool isLast);
    El* Blocks(El* into, MdNode* n, int depth, bool inList);
    El* Item(MdNode* n, Str marker, int depth);
    El* Table(MdNode* n);
    El* CodeBlock(MdNode* n);

    El* Inline(MdNode* n, float font, Rgba color, int weight);
};

MdNode* MdParse(Arena* a, Str source);

}
}

#line 1 "src/component/TitleBar.h"

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

#line 1 "src/component/Tooltip.h"

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

#line 1 "src/component/Tree.h"

namespace gpui {

namespace component {

struct TreeNode {
    Str label = {};
    int parent = -1;
    bool folder = false;
    bool open = false;
};

struct Tree {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    TreeNode nodes[16] = {};
    int n = 0;
    int selected = -1;
    Listener onSelect;

    static Tree* New(Ctx* cx);
    Tree* Node(Str label, int parent, bool folder, bool open);
    Tree* Selected(int i);
    Tree* OnSelect(Listener fn);
    El* IntoEl();
};

}
}

#line 1 "src/component/VirtualList.h"

namespace gpui {

namespace component {

struct VirtualList {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    int count = 0;
    float rowH = 32;
    float viewH = 192;
    float scrollY = 0;
    Listener onRenderRow;
    El* (*row)(Arena* a, int ix) = nullptr;

    static VirtualList* New(Ctx* cx, int count);
    VirtualList* RowH(float v);
    VirtualList* ViewH(float v);
    VirtualList* ScrollY(float v);
    VirtualList* Row(El* (*fn)(Arena*, int));
    El* IntoEl();
};

}
}

#line 1 "src/component/WindowBorder.h"

namespace gpui {

namespace component {

struct WindowBorder {
    Arena* a = nullptr;
    Ctx* cx = nullptr;
    El* child = nullptr;

    static WindowBorder* New(Ctx* cx);
    WindowBorder* Child(El* e);
    El* IntoEl();
};

}
}

#line 1 "src/component/Component.h"

#line 1 "src/sys/SysInfo.h"

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

#line 1 "src/gpui/Fps.h"

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

#line 1 "src/gpui/Paint.h"

namespace gpui {

enum {
    kFontWeightMask = 3,
    kFontWeightNormal = 0,
    kFontWeightSemibold = 1,
    kFontWeightBold = 2,
    kFontWeightMedium = 3,
    kFontMono = 4,
    kFontUnderline = 8,
    kFontItalic = 16
};

const float kLineHeight = 1.618034f;

PaintApp* PaintAppNew();
void PaintAppFree(PaintApp* pa);

bool PaintTargetBegin(PaintCtx* ctx, void* native, int pxW, int pxH);

bool PaintTargetEnd(PaintCtx* ctx);

void PaintTargetFree(PaintCtx* ctx);

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

void PathFillGradientV(PaintCtx* ctx, Path* p, float y0, float y1, Rgba top,
                       Rgba bot);
void PathStroke(PaintCtx* ctx, Path* p, float stroke, Rgba c,
                bool roundCaps = false);

struct TextLayout;

TextLayout* TextLayoutNew(PaintCtx* ctx, Str s, float fontSize, float maxW,
                          bool wrap, uint8_t weight, float lineH,
                          Size* outSize);
void TextLayoutAddRef(TextLayout* tl);
void TextLayoutRelease(TextLayout* tl);

void TextLayoutDraw(PaintCtx* ctx, TextLayout* tl, float x, float y, Rgba c,
                    bool clip);

int TextLayoutHitPoint(TextLayout* tl, Str s, float relX, float relY);

int TextLayoutRangeRects(TextLayout* tl, Str s, int u8a, int u8b, Bounds* out,
                         int max);

}

#line 1 "src/gpui/Platform.h"

namespace gpui {

void WindowDrawFrame(Window* win, void* native, int pxW, int pxH, float dipW,
                     float dipH);

void WindowKeyDown(Window* win, int key, bool shift, bool ctrl, bool alt);

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

void PlatSetCursor(Window* win, CursorKind kind);

int PlatDoubleClickMs();

}

#line 1 "src/gpui/Positioner.h"

namespace gpui {

enum class Placement : uint8_t {
    Top,
    Bottom,
    Left,
    Right
};

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

#if defined(_MSC_VER)
#pragma warning(push, 0)
#pragma warning(disable : 4701 4702)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#line 1 "ext/md4c/md4c.h"

#ifndef MD4C_H
#define MD4C_H

#ifdef __cplusplus
    extern "C" {
#endif

#if defined MD4C_USE_UTF16

    #ifdef _WIN32
        #include <windows.h>
        typedef WCHAR       MD_CHAR;
    #else
        #error MD4C_USE_UTF16 is only supported on Windows.
    #endif
#else
    typedef char            MD_CHAR;
#endif

typedef unsigned MD_SIZE;
typedef unsigned MD_OFFSET;

typedef enum MD_BLOCKTYPE {

    MD_BLOCK_DOC = 0,

    MD_BLOCK_QUOTE,

    MD_BLOCK_UL,

    MD_BLOCK_OL,

    MD_BLOCK_LI,

    MD_BLOCK_HR,

    MD_BLOCK_H,

    MD_BLOCK_CODE,

    MD_BLOCK_HTML,

    MD_BLOCK_P,

    MD_BLOCK_TABLE,
    MD_BLOCK_THEAD,
    MD_BLOCK_TBODY,
    MD_BLOCK_TR,
    MD_BLOCK_TH,
    MD_BLOCK_TD
} MD_BLOCKTYPE;

typedef enum MD_SPANTYPE {

    MD_SPAN_EM,

    MD_SPAN_STRONG,

    MD_SPAN_A,

    MD_SPAN_IMG,

    MD_SPAN_CODE,

    MD_SPAN_DEL,

    MD_SPAN_LATEXMATH,
    MD_SPAN_LATEXMATH_DISPLAY,

    MD_SPAN_WIKILINK,

    MD_SPAN_U
} MD_SPANTYPE;

typedef enum MD_TEXTTYPE {

    MD_TEXT_NORMAL = 0,

    MD_TEXT_NULLCHAR,

    MD_TEXT_BR,
    MD_TEXT_SOFTBR,

    MD_TEXT_ENTITY,

    MD_TEXT_CODE,

    MD_TEXT_HTML,

    MD_TEXT_LATEXMATH
} MD_TEXTTYPE;

typedef enum MD_ALIGN {
    MD_ALIGN_DEFAULT = 0,
    MD_ALIGN_LEFT,
    MD_ALIGN_CENTER,
    MD_ALIGN_RIGHT
} MD_ALIGN;

typedef struct MD_ATTRIBUTE {
    const MD_CHAR* text;
    MD_SIZE size;
    const MD_TEXTTYPE* substr_types;
    const MD_OFFSET* substr_offsets;
} MD_ATTRIBUTE;

typedef struct MD_BLOCK_UL_DETAIL {
    int is_tight;
    MD_CHAR mark;
} MD_BLOCK_UL_DETAIL;

typedef struct MD_BLOCK_OL_DETAIL {
    unsigned start;
    int is_tight;
    MD_CHAR mark_delimiter;
} MD_BLOCK_OL_DETAIL;

typedef struct MD_BLOCK_LI_DETAIL {
    int is_task;
    MD_CHAR task_mark;
    MD_OFFSET task_mark_offset;
} MD_BLOCK_LI_DETAIL;

typedef struct MD_BLOCK_H_DETAIL {
    unsigned level;
} MD_BLOCK_H_DETAIL;

typedef struct MD_BLOCK_CODE_DETAIL {
    MD_ATTRIBUTE info;
    MD_ATTRIBUTE lang;
    MD_CHAR fence_char;
} MD_BLOCK_CODE_DETAIL;

typedef struct MD_BLOCK_TABLE_DETAIL {
    unsigned col_count;
    unsigned head_row_count;
    unsigned body_row_count;
} MD_BLOCK_TABLE_DETAIL;

typedef struct MD_BLOCK_TD_DETAIL {
    MD_ALIGN align;
} MD_BLOCK_TD_DETAIL;

typedef struct MD_SPAN_A_DETAIL {
    MD_ATTRIBUTE href;
    MD_ATTRIBUTE title;
    int is_autolink;
} MD_SPAN_A_DETAIL;

typedef struct MD_SPAN_IMG_DETAIL {
    MD_ATTRIBUTE src;
    MD_ATTRIBUTE title;
} MD_SPAN_IMG_DETAIL;

typedef struct MD_SPAN_WIKILINK {
    MD_ATTRIBUTE target;
} MD_SPAN_WIKILINK_DETAIL;

#define MD_FLAG_COLLAPSEWHITESPACE          0x0001
#define MD_FLAG_PERMISSIVEATXHEADERS        0x0002
#define MD_FLAG_PERMISSIVEURLAUTOLINKS      0x0004
#define MD_FLAG_PERMISSIVEEMAILAUTOLINKS    0x0008
#define MD_FLAG_NOINDENTEDCODEBLOCKS        0x0010
#define MD_FLAG_NOHTMLBLOCKS                0x0020
#define MD_FLAG_NOHTMLSPANS                 0x0040
#define MD_FLAG_TABLES                      0x0100
#define MD_FLAG_STRIKETHROUGH               0x0200
#define MD_FLAG_PERMISSIVEWWWAUTOLINKS      0x0400
#define MD_FLAG_TASKLISTS                   0x0800
#define MD_FLAG_LATEXMATHSPANS              0x1000
#define MD_FLAG_WIKILINKS                   0x2000
#define MD_FLAG_UNDERLINE                   0x4000
#define MD_FLAG_HARD_SOFT_BREAKS            0x8000

#define MD_FLAG_PERMISSIVEAUTOLINKS         (MD_FLAG_PERMISSIVEEMAILAUTOLINKS | MD_FLAG_PERMISSIVEURLAUTOLINKS | MD_FLAG_PERMISSIVEWWWAUTOLINKS)
#define MD_FLAG_NOHTML                      (MD_FLAG_NOHTMLBLOCKS | MD_FLAG_NOHTMLSPANS)

#define MD_DIALECT_COMMONMARK               0
#define MD_DIALECT_GITHUB                   (MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS)

typedef struct MD_PARSER {

    unsigned abi_version;

    unsigned flags;

    int (*enter_block)(MD_BLOCKTYPE  , void*  , void*  );
    int (*leave_block)(MD_BLOCKTYPE  , void*  , void*  );

    int (*enter_span)(MD_SPANTYPE  , void*  , void*  );
    int (*leave_span)(MD_SPANTYPE  , void*  , void*  );

    int (*text)(MD_TEXTTYPE  , const MD_CHAR*  , MD_SIZE  , void*  );

    void (*debug_log)(const char*  , void*  );

    void (*syntax)(void);
} MD_PARSER;

typedef MD_PARSER MD_RENDERER;

int md_parse(const MD_CHAR* text, MD_SIZE size, const MD_PARSER* parser, void* userdata);

#ifdef __cplusplus
    }
#endif

#endif

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif
