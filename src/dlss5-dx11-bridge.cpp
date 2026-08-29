// dlss5-dx11-bridge - ReShade add-on.
//
// Lets a DLSS 5 neural-rendering add-on that only hooks D3D12 run inside a game
// that renders with D3D11.
//
// The game's NVSDK_NGX_D3D11 CreateFeature and EvaluateFeature are hooked and
// always forwarded untouched, so the game keeps working even if everything here
// fails. The bridge then mirrors the same DLSS contract onto a second NGX
// session running on its own D3D12 device -- and that D3D12 evaluate is the
// call a DLSS 5 add-on detours and inserts itself into. Nothing about the other
// add-on is modified; it simply receives genuine D3D12 NGX calls.
//
// Nothing on disk is patched. The only writes to foreign code are 14 bytes at
// three function entry points, in memory, restored around every call.
//
// Behaviour is driven by dlss5-dx11-bridge.cfg, re-read while the game runs, so
// settings can be changed without restarting. dlss5-dx11-bridge.log records the
// contract that was read, which resource-sharing direction the driver accepted,
// and the result of every NGX call.
//
// Tested on Baldur's Gate 3. Nothing here is specific to it: the NGX entry
// points are located by export name in whatever module exports them, and every
// size and offset is taken from the game's own parameter block.
//
// Build:
//   cl /nologo /LD /EHsc /O2 /MT dlss5-dx11-bridge.cpp \
//      /link /OUT:dlss5-dx11-bridge.addon64 kernel32.lib user32.lib

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>

// Kept in step with version.rc, which is where ReShade's overlay reads it from.
#define BRIDGE_VERSION "1.0.10"

extern "C" __declspec(dllexport) const char *NAME =
    "DLSS 5 DX11 Bridge " BRIDGE_VERSION;
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Lets D3D12-only DLSS 5 add-ons run in a D3D11 game. Intercepts the game's "
    "NVSDK_NGX_D3D11 evaluate, forwards it untouched, and mirrors the same "
    "contract onto a second NGX session on its own D3D12 device -- which is "
    "where a DLSS 5 neural-rendering add-on can insert itself. "
    "Settings in dlss5-dx11-bridge.cfg, re-read while the game runs.";

// ---------------------------------------------------------------------------
// NGX declarations
//
// Deliberately mirrors the declaration order of NVIDIA's nvsdk_ngx.h. MSVC
// emits same-name virtual overloads in reverse declaration order, which puts
// Get(const char*, ID3D12Resource**) on vtable slot 0x48 -- the slot the
// shipping NGX consumers call. Keeping the order identical means the compiler
// reproduces NVIDIA's layout and no offset has to be hardcoded here.
// ---------------------------------------------------------------------------

struct ID3D12Resource;  // opaque, never dereferenced

typedef int NVSDK_NGX_Result;
static const NVSDK_NGX_Result NGX_SUCCESS = 1;

struct NVSDK_NGX_Handle { unsigned int Id; };

struct NVSDK_NGX_Parameter
{
    virtual void Set(const char *InName, unsigned long long InValue) = 0;
    virtual void Set(const char *InName, float InValue) = 0;
    virtual void Set(const char *InName, double InValue) = 0;
    virtual void Set(const char *InName, unsigned int InValue) = 0;
    virtual void Set(const char *InName, int InValue) = 0;
    virtual void Set(const char *InName, ID3D11Resource *InValue) = 0;
    virtual void Set(const char *InName, ID3D12Resource *InValue) = 0;
    virtual void Set(const char *InName, void *InValue) = 0;

    virtual NVSDK_NGX_Result Get(const char *InName, unsigned long long *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, float *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, double *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, unsigned int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D11Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D12Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, void **OutValue) const = 0;

    virtual void Reset() = 0;
};

typedef NVSDK_NGX_Result (*PFN_Evaluate)(ID3D11DeviceContext *, const NVSDK_NGX_Handle *,
                                         const NVSDK_NGX_Parameter *, void *);
typedef NVSDK_NGX_Result (*PFN_Create)(ID3D11DeviceContext *, int,
                                       NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static CRITICAL_SECTION g_log_cs;
static char             g_log_path[MAX_PATH];
static HMODULE          g_self;

// Anything that means "your setup is wrong" also goes into ReShade's own log,
// where its overlay shows it. People reliably post ReShade.log instead of this
// add-on's, so the messages that matter should be in both.
typedef void (*PFN_ReShadeLogMessage)(HMODULE, int, const char *);
static PFN_ReShadeLogMessage g_reshade_log;
static HMODULE               g_reshade_module;

static void Log(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    // A per-frame message that nobody stopped can fill a disk on someone else's
    // machine. Bound the file so the worst case is a truncated log, not that.
    static long   written = 0;
    static bool   capped  = false;
    const  long   kCap    = 8 * 1024 * 1024;

    EnterCriticalSection(&g_log_cs);
    if (!capped)
    {
        FILE *f = nullptr;
        if (fopen_s(&f, g_log_path, "a") == 0 && f != nullptr)
        {
            written += fprintf(f, "%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute,
                               st.wSecond, st.wMilliseconds, line);
            if (written > kCap)
            {
                fprintf(f, "\n--- log capped at 8 MB. Something is repeating every "
                           "frame; the lines above still say what. ---\n");
                capped = true;
            }
            fclose(f);
        }
    }
    LeaveCriticalSection(&g_log_cs);
}

// Same as Log, but the message is also raised in ReShade so the user sees it in
// the overlay without having to find a file. Reserved for conditions that stop
// the bridge working -- routine progress stays in this add-on's own log.
static void Warn(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    Log("%s", line);

    if (g_reshade_log != nullptr)
    {
        char tagged[1100];
        _snprintf_s(tagged, sizeof(tagged), _TRUNCATE, "[DLSS 5 DX11 Bridge] %s", line);
        g_reshade_log(g_reshade_module, 1 /* error */, tagged);
    }
}

// ---------------------------------------------------------------------------
// Crash reporting
//
// Reports so far have all ended the same way: a log that simply stops, with no
// way to tell whether the game died there or the file was just truncated when
// it was copied. These three pieces answer that without having to ask.
//
// The breadcrumb names what was last being attempted, the filter records where
// a fatal exception actually landed and in whose code, and the next run says
// whether the previous one ended cleanly.
// ---------------------------------------------------------------------------

static const char *volatile g_where = "starting up";

static void Breadcrumb(const char *what) { g_where = what; }

static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep)
{
    const void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;

    // Whose code faulted matters more than the address itself: it separates a
    // bug in here from one in the game, the driver, or another add-on.
    wchar_t owner[MAX_PATH] = L"unknown";
    HMODULE mod = nullptr;
    if (addr != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &mod) && mod != nullptr)
        GetModuleFileNameW(mod, owner, MAX_PATH);

    Log("");
    // A marker the next run looks for. Recording a crash when one actually
    // happens is reliable; inferring one from a missing clean-exit marker is
    // not, because plenty of games terminate without ever running DLL detach.
    Log("### CRASH RECORDED ###");
    Log("  exception 0x%08X at %p", code, addr);
    Log("  in: %ls", owner);
    Log("  this add-on was last doing: %s", g_where);
    Log("  %s", mod == g_self ? "That address is inside this add-on, so this one is on me."
                              : "That address is not in this add-on.");
    Log("#####################################################");

    // Always chain: games install their own crash handlers and this must not
    // replace them.
    return g_prev_filter != nullptr ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}


// ---------------------------------------------------------------------------
// Inline hook
//
// 14-byte absolute jump, no trampoline: the original bytes are restored around
// the forwarded call and the patch is written back afterwards. That avoids
// needing a length disassembler to relocate the prologue.
//
// Every write re-acquires and then restores page protection rather than leaving
// the page writable. The D3D11 and D3D12 NGX entry points sit within a few
// hundred bytes of each other, so another add-on hooking the D3D12 side (RenoDX
// uses Detours there) shares this page and will reset its protection when it is
// done. Assuming the page stays writable would fault the moment that happens.
// ---------------------------------------------------------------------------

struct Hook
{
    BYTE *target;
    BYTE  saved[14];
    BYTE  patch[14];
    bool  active;
};

// Several modules can export the NGX D3D11 API at the same time: a game or mod
// that links NGX, the driver's loader beneath it, and the feature snippet
// beneath that. Which one the caller actually enters is not knowable from the
// outside -- Baldur's Gate 3 calls its own exports, Skyrim's upscaler links NGX
// statically and calls straight into nvngx_dlss.dll, never touching the loader.
// Earlier builds tried to pick the right layer by name and got it wrong twice.
// Hook every layer instead and let the outermost call win; see g_nest below.
//
// Twelve because the same snippet can be mapped more than once: Baldur's Gate 3
// alone reaches six, with nvngx_dlss.dll appearing at two different bases.
static const int kMaxLayers = 12;

struct Layer
{
    HMODULE mod;
    Hook    eval;
    Hook    eval_c;
    Hook    create;
};

static Layer            g_layer[kMaxLayers];
static volatile LONG    g_layer_count;
static CRITICAL_SECTION g_hook_cs;

// Depth of NGX calls this thread is currently inside. Only the outermost is the
// caller's; anything nested is NGX talking to itself, with parameters it has
// already rewritten for its own use. Reading those as if they were the game's
// is what made 1.0.5 necessary.
static __declspec(thread) int g_nest;

struct NestGuard
{
    NestGuard()  { ++g_nest; }
    ~NestGuard() { --g_nest; }
};

static bool WriteCode(void *dst, const void *src, size_t len)
{
    DWORD old = 0;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(dst, src, len);
    VirtualProtect(dst, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return true;
}

static bool HookInstall(Hook &h, void *target, void *detour)
{
    h.target = static_cast<BYTE *>(target);
    memcpy(h.saved, h.target, sizeof(h.saved));

    // jmp qword ptr [rip+0]; <8-byte absolute address>
    h.patch[0] = 0xFF;
    h.patch[1] = 0x25;
    h.patch[2] = h.patch[3] = h.patch[4] = h.patch[5] = 0x00;
    memcpy(h.patch + 6, &detour, sizeof(detour));

    if (!WriteCode(h.target, h.patch, sizeof(h.patch)))
        return false;

    h.active = true;
    return true;
}

static void HookRemove(Hook &h)
{
    if (!h.active) return;
    WriteCode(h.target, h.saved, sizeof(h.saved));
}

static void HookRestore(Hook &h)
{
    if (!h.active) return;
    WriteCode(h.target, h.patch, sizeof(h.patch));
}

// ---------------------------------------------------------------------------
// Parameter dumping
// ---------------------------------------------------------------------------

static const char *FormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R11G11B10_FLOAT:       return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:    return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return "R32G32B32A32_TYPELESS";
    case DXGI_FORMAT_R16_FLOAT:             return "R16_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_R16G16_FLOAT:          return "R16G16_FLOAT";
    case DXGI_FORMAT_R16G16_TYPELESS:       return "R16G16_TYPELESS";
    case DXGI_FORMAT_R32_FLOAT:             return "R32_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS:          return "R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT:             return "D32_FLOAT";
    case DXGI_FORMAT_R24G8_TYPELESS:        return "R24G8_TYPELESS";
    case DXGI_FORMAT_R32G8X24_TYPELESS:     return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_R16_UNORM:             return "R16_UNORM";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:  return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
    case DXGI_FORMAT_R16_TYPELESS:          return "R16_TYPELESS";
    case DXGI_FORMAT_D16_UNORM:             return "D16_UNORM";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:     return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R8_UNORM:              return "R8_UNORM";
    default:                                return "unnamed";
    }
}

// A typeless texture carries no interpretation of its bits, so nothing can build
// a shader resource view or a typed UAV over it. The bridge used to give its
// shared copies whatever format the game used, typeless included, and a DLSS 5
// add-on then refused the result: "DLSS output format 1 is not a supported typed
// codec format". Escape from Tarkov hands DLSS an R32G32B32A32_TYPELESS colour
// buffer and an R16G16_TYPELESS motion vector buffer, so the bridge delivered
// frames perfectly and the neural pass never ran on any of them.
//
// The copy is given the natural typed format of the same typeless family.
// CopyResource checks the family, not the exact format, so it stays legal in
// both directions, and the far side gets something it can actually view.
static DXGI_FORMAT TypedFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS:    return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R8G8_TYPELESS:         return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UNORM;
    default:                                return f;
    }
}

// The decisive field for a zero-copy bridge. Anything other than "none" means
// the texture can be opened on a second device without an intermediate copy.
static const char *ShareText(UINT misc)
{
    if (misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)    return "  <== SHARED_NTHANDLE";
    if (misc & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)  return "  <== SHARED_KEYEDMUTEX";
    if (misc & D3D11_RESOURCE_MISC_SHARED)             return "  <== SHARED";
    return "  (not shared)";
}

static void DumpTexture(const NVSDK_NGX_Parameter *p, const char *key)
{
    ID3D11Resource *res = nullptr;
    NVSDK_NGX_Result r = p->Get(key, &res);
    if (r != NGX_SUCCESS || res == nullptr)
    {
        Log("    %-22s  absent (result=%d)", key, r);
        return;
    }

    ID3D11Texture2D *tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) &&
        tex != nullptr)
    {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        Log("    %-22s  %p  %ux%u  fmt=%u %s  mips=%u arr=%u samp=%u usage=%u "
            "bind=0x%X cpu=0x%X misc=0x%X%s",
            key, static_cast<void *>(res), d.Width, d.Height, d.Format, FormatName(d.Format),
            d.MipLevels, d.ArraySize, d.SampleDesc.Count, d.Usage, d.BindFlags,
            d.CPUAccessFlags, d.MiscFlags, ShareText(d.MiscFlags));
        tex->Release();
    }
    else
    {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        Log("    %-22s  %p  not a Texture2D (dimension=%d)", key, static_cast<void *>(res), dim);
    }
}

static void DumpUInt(const NVSDK_NGX_Parameter *p, const char *key)
{
    unsigned int v = 0;
    if (p->Get(key, &v) == NGX_SUCCESS)
        Log("    %-40s = %u", key, v);
}

static void DumpInt(const NVSDK_NGX_Parameter *p, const char *key)
{
    int v = 0;
    if (p->Get(key, &v) == NGX_SUCCESS)
        Log("    %-40s = %d", key, v);
}

static void DumpFloat(const NVSDK_NGX_Parameter *p, const char *key)
{
    float v = 0.0f;
    if (p->Get(key, &v) == NGX_SUCCESS)
        Log("    %-40s = %.6f", key, static_cast<double>(v));
}

static void DumpContext(ID3D11DeviceContext *ctx)
{
    if (ctx == nullptr) { Log("    context                 = null"); return; }

    D3D11_DEVICE_CONTEXT_TYPE t = ctx->GetType();
    Log("    context                 = %p  type=%s", static_cast<void *>(ctx),
        t == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "IMMEDIATE" : "DEFERRED  <== blocks a simple bridge");

    ID3D11Device *dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev == nullptr) return;

    Log("    device                  = %p  feature_level=0x%04X",
        static_cast<void *>(dev), dev->GetFeatureLevel());

    // ID3D11Device5 is required to create a fence that can be shared with a
    // D3D12 queue, which any copy-based bridge would need.
    ID3D11Device5 *dev5 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev5))) &&
        dev5 != nullptr)
    {
        Log("    ID3D11Device5           = yes (shared fences available)");
        dev5->Release();
    }
    else
    {
        Log("    ID3D11Device5           = NO (no shared fence support)");
    }

    dev->Release();
}

static void DumpEvaluate(ID3D11DeviceContext *ctx, const NVSDK_NGX_Handle *handle,
                         const NVSDK_NGX_Parameter *p, long n)
{
    Log("--- EvaluateFeature #%ld  handle=%p params=%p ---", n,
        static_cast<const void *>(handle), static_cast<const void *>(p));

    DumpContext(ctx);
    if (p == nullptr) { Log("    params is null"); return; }

    Log("  resources:");
    DumpTexture(p, "Color");
    DumpTexture(p, "Output");
    DumpTexture(p, "Depth");
    DumpTexture(p, "MotionVectors");
    DumpTexture(p, "ExposureTexture");
    DumpTexture(p, "TransparencyMask");
    DumpTexture(p, "BiasCurrentColorMask");

    Log("  scalars:");
    DumpUInt(p, "Width");
    DumpUInt(p, "Height");
    DumpUInt(p, "OutWidth");
    DumpUInt(p, "OutHeight");
    DumpUInt(p, "DLSS.Render.Subrect.Dimensions.Width");
    DumpUInt(p, "DLSS.Render.Subrect.Dimensions.Height");
    DumpUInt(p, "DLSS.Input.Color.Subrect.Base.X");
    DumpUInt(p, "DLSS.Input.Color.Subrect.Base.Y");
    DumpUInt(p, "DLSS.Input.Depth.Subrect.Base.X");
    DumpUInt(p, "DLSS.Input.Depth.Subrect.Base.Y");
    DumpUInt(p, "DLSS.Input.MV.Subrect.Base.X");
    DumpUInt(p, "DLSS.Input.MV.Subrect.Base.Y");
    DumpUInt(p, "DLSS.Output.Subrect.Base.X");
    DumpUInt(p, "DLSS.Output.Subrect.Base.Y");
    DumpFloat(p, "MV.Scale.X");
    DumpFloat(p, "MV.Scale.Y");
    DumpFloat(p, "Jitter.Offset.X");
    DumpFloat(p, "Jitter.Offset.Y");
    DumpFloat(p, "Sharpness");
    DumpFloat(p, "DLSS.Pre.Exposure");
    DumpFloat(p, "DLSS.Exposure.Scale");
    DumpInt(p, "Reset");
    DumpInt(p, "DLSS.Feature.Create.Flags");
    DumpInt(p, "PerfQualityValue");
}

// ---------------------------------------------------------------------------
// D3D12 NGX feasibility test
//
// Runs once. Asks the single question that decides whether a D3D11 -> D3D12 NGX
// bridge is possible at all: can a second NGX session be initialised on a D3D12
// device inside a process where NGX is already live on D3D11, and can a DLSS
// feature be created on it?
//
// If this fails, no bridge architecture -- copy-based or d3d11on12 -- can work,
// because both end at the same NVSDK_NGX_D3D12_CreateFeature call.
//
// Side benefit: that call is the one RenoDX's DLSS 5 add-on detours, so when the
// add-on is present its own log lines reveal whether it engages.
// ---------------------------------------------------------------------------

typedef HRESULT (WINAPI *PFN_D3D12CreateDevice_)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);

// Argument order taken from the disassembly of _nvngx.dll, not from memory.
// In Init_Ext the fourth argument is read as a 32-bit value (mov ebp,r9d) and
// the fifth as a qword, so the version precedes the FeatureCommonInfo pointer.
// Init_ProjectID follows the same pattern in its stack arguments.
typedef NVSDK_NGX_Result (*PFN_Init_ProjectID)(const char *, int, const char *, const wchar_t *,
                                               ID3D12Device *, int, const void *);
typedef NVSDK_NGX_Result (*PFN_Init_Ext)(unsigned long long, const wchar_t *, ID3D12Device *,
                                         int, const void *);
typedef NVSDK_NGX_Result (*PFN_GetCapabilityParameters)(NVSDK_NGX_Parameter **);
typedef NVSDK_NGX_Result (*PFN_AllocateParameters)(NVSDK_NGX_Parameter **);
typedef NVSDK_NGX_Result (*PFN_D3D12CreateFeature)(ID3D12GraphicsCommandList *, int,
                                                   NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (*PFN_D3D12EvaluateFeature)(ID3D12GraphicsCommandList *,
                                                     const NVSDK_NGX_Handle *,
                                                     const NVSDK_NGX_Parameter *, void *);
typedef NVSDK_NGX_Result (*PFN_D3D12ReleaseFeature)(NVSDK_NGX_Handle *);

#include "bridge.h"

static volatile LONG g_probe_done = 0;

typedef BOOL (WINAPI *PFN_EnumProcessModules)(HANDLE, HMODULE *, DWORD, LPDWORD);

static HMODULE FindNgxLoader()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto enum_modules =
        reinterpret_cast<PFN_EnumProcessModules>(GetProcAddress(k32, "K32EnumProcessModules"));
    if (enum_modules == nullptr) return nullptr;

    HMODULE mods[1024];
    DWORD   needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return nullptr;

    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i)
        if (GetProcAddress(mods[i], "NVSDK_NGX_D3D12_Init_ProjectID") != nullptr &&
            GetProcAddress(mods[i], "NVSDK_NGX_D3D12_CreateFeature") != nullptr)
            return mods[i];
    return nullptr;
}

// Every NGX entry point below is called through a guarded wrapper. These are
// undocumented exports reached with signatures recovered from disassembly; a
// wrong guess must land in the log, not take the game down. Each wrapper is its
// own function with no C++ objects so __try is legal and no unwinding is needed.
#define NGX_EXCEPTION_MARKER 0x7FFFFFFF

static NVSDK_NGX_Result SafeInitExt(PFN_Init_Ext fn, unsigned long long app_id,
                                    const wchar_t *path, ID3D12Device *dev, int ver, DWORD *code)
{
    *code = 0;
    __try { return fn(app_id, path, dev, ver, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeInitProjectID(PFN_Init_ProjectID fn, const char *project,
                                          const wchar_t *path, ID3D12Device *dev, int ver,
                                          DWORD *code)
{
    *code = 0;
    __try { return fn(project, 0 /* ENGINE_TYPE_CUSTOM */, "1.0", path, dev, ver, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}


static NVSDK_NGX_Result SafeAllocParams(PFN_AllocateParameters fn, NVSDK_NGX_Parameter **out, DWORD *code)
{
    *code = 0;
    __try { return fn(out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeGetCaps(PFN_GetCapabilityParameters fn, NVSDK_NGX_Parameter **out, DWORD *code)
{
    *code = 0;
    __try { return fn(out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeCreateFeature(PFN_D3D12CreateFeature fn, ID3D12GraphicsCommandList *list,
                                          NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out, DWORD *code)
{
    *code = 0;
    __try { return fn(list, 1 /* SuperSampling */, p, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}



// A detoured entry point no longer starts with its own prologue. Comparing what
// is actually at the function address against the untouched bytes shows whether
// another add-on's hook sits in front of the call this probe is about to make.
static void LogEntryBytes(const char *label, void *fn)
{
    if (fn == nullptr) { Log("    %-34s <not exported>", label); return; }
    const BYTE *p = static_cast<const BYTE *>(fn);
    char hex[64];
    int  n = 0;
    for (int i = 0; i < 14; ++i)
        n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%02X ", p[i]);
    const bool detoured = (p[0] == 0xE9) || (p[0] == 0xFF && p[1] == 0x25) ||
                          (p[0] == 0x48 && p[1] == 0xB8) || (p[0] == 0xEB);
    Log("    %-34s %p  %s %s", label, fn, hex, detoured ? " <== DETOURED" : "");
}

static void DumpCapability(NVSDK_NGX_Parameter *caps)
{
    static const char *int_keys[] = {
        "SuperSampling.Available",
        "SuperSampling.NeedsUpdatedDriver",
        "SuperSampling.MinDriverVersionMajor",
        "SuperSampling.MinDriverVersionMinor",
        "SuperSamplingDenoising.Available",
    };
    for (const char *k : int_keys)
    {
        int v = 0;
        NVSDK_NGX_Result r = caps->Get(k, &v);
        if (r == NGX_SUCCESS) Log("      %-40s = %d", k, v);
        else                  Log("      %-40s   query failed 0x%08X", k, r);
    }
}

#include "bridge.inc"


// ---------------------------------------------------------------------------
// Detours
// ---------------------------------------------------------------------------

static volatile LONG g_eval_count   = 0;
static volatile LONG g_create_count = 0;

// EvaluateFeature and EvaluateFeature_C are distinct exports at distinct
// addresses. BG3 drives DLSS through the _C variant, so hooking only the C++
// one catches CreateFeature and nothing else. Both are hooked here.
static NVSDK_NGX_Result ForwardEvaluate(Hook &h, const char *tag, ID3D11DeviceContext *ctx,
                                        const NVSDK_NGX_Handle *handle,
                                        const NVSDK_NGX_Parameter *p, void *cb)
{
    // A nested call means the layer above already handled this frame and is now
    // calling down through NGX's own plumbing. Forward it and touch nothing.
    if (g_nest > 0)
    {
        EnterCriticalSection(&g_hook_cs);
        HookRemove(h);
        NVSDK_NGX_Result inner = reinterpret_cast<PFN_Evaluate>(h.target)(ctx, handle, p, cb);
        HookRestore(h);
        LeaveCriticalSection(&g_hook_cs);
        return inner;
    }
    NestGuard nest;

    const LONG n = InterlockedIncrement(&g_eval_count);
    if (n <= 5 || (n % 1800) == 0)
    {
        Log("  (entry point: %s)", tag);
        DumpEvaluate(ctx, handle, p, n);
    }

    // The game's DLSS writes an Output that the bridge then overwrites, so when
    // the bridge is delivering, running it is pure waste. Suppressed only while
    // BridgeWillDeliver() holds; the moment anything goes wrong the call is
    // forwarded again and the game renders on its own.
    Breadcrumb("forwarding the game's DLSS evaluate");
    const bool suppress = BridgeWillDeliver();

    NVSDK_NGX_Result r = NGX_SUCCESS;
    if (!suppress)
    {
        EnterCriticalSection(&g_hook_cs);
        HookRemove(h);
        r = reinterpret_cast<PFN_Evaluate>(h.target)(ctx, handle, p, cb);
        HookRestore(h);
        LeaveCriticalSection(&g_hook_cs);
    }

    if (n <= 5)
        Log("--- EvaluateFeature #%ld returned %d%s ---", n, r,
            suppress ? " (game's own DLSS suppressed)" : "");

    // Worth saying in words. If the game's own DLSS is already failing, the
    // contract was broken before this add-on touched it, and chasing the bridge
    // is chasing the wrong thing.
    if (!suppress && r != NGX_SUCCESS && n <= 5)
        Log("  the game's own DLSS call failed (0x%08X), before the bridge changed "
            "anything. Whatever is wrong is wrong upstream of it.", r);

    // The bridge runs after the game's own evaluate has been forwarded, so the
    // game's Color holds this frame's input and its Output can be replaced.
    // stage=0 is documented as fully inert and has to mean it: opening the D3D12
    // session creates a device and starts an NGX session, and on some drivers
    // that is itself the thing that crashes. An off switch that still does the
    // dangerous part is not an off switch.
    if (g_cfg.stage >= 1 && InterlockedCompareExchange(&g_probe_done, 1, 0) == 0)
    {
        ID3D11Device *dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev != nullptr) { BridgeInitSession(dev, ctx); dev->Release(); }
    }
    BridgeFrame(ctx, p);

    return r;
}

static NVSDK_NGX_Result ForwardCreate(Hook &h, ID3D11DeviceContext *ctx, int feature_id,
                                      NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out)
{
    if (g_nest > 0)
    {
        EnterCriticalSection(&g_hook_cs);
        HookRemove(h);
        NVSDK_NGX_Result inner =
            reinterpret_cast<PFN_Create>(h.target)(ctx, feature_id, p, out);
        HookRestore(h);
        LeaveCriticalSection(&g_hook_cs);
        return inner;
    }
    NestGuard nest;

    Breadcrumb("forwarding the game's DLSS create");
    const LONG n = InterlockedIncrement(&g_create_count);
    Log("=== CreateFeature #%ld  feature_id=%d ===", n, feature_id);
    DumpContext(ctx);
    if (p != nullptr)
    {
        Log("  creation parameters:");
        DumpUInt(p, "Width");
        DumpUInt(p, "Height");
        DumpUInt(p, "OutWidth");
        DumpUInt(p, "OutHeight");
        DumpInt(p, "DLSS.Feature.Create.Flags");
        DumpInt(p, "PerfQualityValue");
        DumpInt(p, "RTXValue");
        DumpInt(p, "DLSS.Enable.Output.Subrects");
    }

    EnterCriticalSection(&g_hook_cs);
    HookRemove(h);
    NVSDK_NGX_Result r = reinterpret_cast<PFN_Create>(h.target)(ctx, feature_id, p, out);
    HookRestore(h);
    LeaveCriticalSection(&g_hook_cs);

    Log("=== CreateFeature #%ld returned %d, handle=%p ===", n, r,
        (out != nullptr && *out != nullptr) ? static_cast<void *>(*out) : nullptr);
    return r;
}

// One detour per layer, because a detour has to know which layer's saved bytes
// to restore before forwarding, and the entry point itself carries no way to
// tell. Eight sets of three is duplication a macro can carry; the alternative is
// hand-written thunks in assembly.
#define LAYER_DETOURS(i)                                                             \
    static NVSDK_NGX_Result Detour_Evaluate_##i(                                     \
        ID3D11DeviceContext *c, const NVSDK_NGX_Handle *h,                           \
        const NVSDK_NGX_Parameter *p, void *cb)                                      \
    { return ForwardEvaluate(g_layer[i].eval,                                        \
                             "NVSDK_NGX_D3D11_EvaluateFeature", c, h, p, cb); }      \
    static NVSDK_NGX_Result Detour_Evaluate_C_##i(                                   \
        ID3D11DeviceContext *c, const NVSDK_NGX_Handle *h,                           \
        const NVSDK_NGX_Parameter *p, void *cb)                                      \
    { return ForwardEvaluate(g_layer[i].eval_c,                                      \
                             "NVSDK_NGX_D3D11_EvaluateFeature_C", c, h, p, cb); }    \
    static NVSDK_NGX_Result Detour_Create_##i(                                       \
        ID3D11DeviceContext *c, int f, NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **o) \
    { return ForwardCreate(g_layer[i].create, c, f, p, o); }

LAYER_DETOURS(0) LAYER_DETOURS(1) LAYER_DETOURS(2)  LAYER_DETOURS(3)
LAYER_DETOURS(4) LAYER_DETOURS(5) LAYER_DETOURS(6)  LAYER_DETOURS(7)
LAYER_DETOURS(8) LAYER_DETOURS(9) LAYER_DETOURS(10) LAYER_DETOURS(11)

struct DetourSet
{
    PFN_Evaluate eval;
    PFN_Evaluate eval_c;
    PFN_Create   create;
};

#define LAYER_ENTRY(i) { &Detour_Evaluate_##i, &Detour_Evaluate_C_##i, &Detour_Create_##i }
static const DetourSet kDetour[kMaxLayers] = {
    LAYER_ENTRY(0), LAYER_ENTRY(1), LAYER_ENTRY(2),  LAYER_ENTRY(3),
    LAYER_ENTRY(4), LAYER_ENTRY(5), LAYER_ENTRY(6),  LAYER_ENTRY(7),
    LAYER_ENTRY(8), LAYER_ENTRY(9), LAYER_ENTRY(10), LAYER_ENTRY(11),
};

// ---------------------------------------------------------------------------
// Module discovery
// ---------------------------------------------------------------------------

typedef BOOL (WINAPI *PFN_EnumProcessModules)(HANDLE, HMODULE *, DWORD, LPDWORD);

// The NGX entry points may live in the driver's loader DLL or, when a game
// links the static NGX library, in the game executable itself -- which is where
// BG3 keeps them. Rather than guess, walk every loaded module and take whichever
// one exports the symbol.
// Hooks every module that exports the NGX D3D11 API and is not already hooked,
// and returns how many were added this pass. Modules appear at different times
// -- a game's own exports are there from the start, the feature snippet only
// once DLSS initialises -- so this runs again on every DLL load.
//
// Earlier builds picked one module and tried to reason about which layer the
// caller would enter. That reasoning was wrong for Trails (hooked too low) and
// wrong for Skyrim (hooked too high). The layers are indistinguishable from the
// outside, so all of them are hooked and g_nest decides at call time.
static void LogPrologue(const char *label, const BYTE *p);

static int HookNewNgxModules()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) return 0;
    auto enum_modules =
        reinterpret_cast<PFN_EnumProcessModules>(GetProcAddress(k32, "K32EnumProcessModules"));
    if (enum_modules == nullptr) return 0;

    HMODULE mods[1024];
    DWORD   needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return 0;

    int added = 0;
    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i)
    {
        if (g_layer_count >= kMaxLayers)
        {
            static bool said = false;
            if (!said) { said = true; Log("  layer table full at %d; later modules "
                                          "exporting NGX are NOT hooked.", kMaxLayers); }
            break;
        }

        void *eval   = reinterpret_cast<void *>(GetProcAddress(mods[i], "NVSDK_NGX_D3D11_EvaluateFeature"));
        void *eval_c = reinterpret_cast<void *>(GetProcAddress(mods[i], "NVSDK_NGX_D3D11_EvaluateFeature_C"));
        void *create = reinterpret_cast<void *>(GetProcAddress(mods[i], "NVSDK_NGX_D3D11_CreateFeature"));
        if (create == nullptr || (eval == nullptr && eval_c == nullptr)) continue;

        bool known = false;
        for (LONG k = 0; k < g_layer_count && !known; ++k)
            known = (g_layer[k].mod == mods[i]);
        if (known) continue;

        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mods[i], path, MAX_PATH);
        const wchar_t *leaf = wcsrchr(path, L'\\');
        leaf = leaf ? leaf + 1 : path;

        // The downloaded models under ProgramData are DLLs that export the full
        // API too, but nothing ever calls them as an entry point. They would eat
        // layer slots the real callers need.
        const wchar_t *ext = wcsrchr(leaf, L'.');
        if ((ext != nullptr && _wcsicmp(ext, L".bin") == 0) ||
            wcsstr(path, L"\\NGX\\models\\") != nullptr)
            continue;

        // Some builds export both names at the same address. Patching one
        // address twice would save the first patch as the second's original
        // bytes and leave the function permanently redirected.
        if (eval_c == eval) eval_c = nullptr;

        const LONG slot = g_layer_count;
        Layer &L = g_layer[slot];
        L.mod = mods[i];

        Log("NGX layer %ld: %ls (base=%p)", slot, path, static_cast<void *>(mods[i]));
        Log("  NVSDK_NGX_D3D11_CreateFeature     = %p", create);
        Log("  NVSDK_NGX_D3D11_EvaluateFeature   = %p", eval);
        Log("  NVSDK_NGX_D3D11_EvaluateFeature_C = %p", eval_c);
        if (create != nullptr) LogPrologue("CreateFeature", static_cast<const BYTE *>(create));
        if (eval   != nullptr) LogPrologue("EvaluateFeature", static_cast<const BYTE *>(eval));
        if (eval_c != nullptr) LogPrologue("EvaluateFeature_C", static_cast<const BYTE *>(eval_c));

        EnterCriticalSection(&g_hook_cs);
        const bool ok_create = create != nullptr &&
            HookInstall(L.create, create, reinterpret_cast<void *>(kDetour[slot].create));
        const bool ok_eval = eval != nullptr &&
            HookInstall(L.eval, eval, reinterpret_cast<void *>(kDetour[slot].eval));
        const bool ok_eval_c = eval_c != nullptr &&
            HookInstall(L.eval_c, eval_c, reinterpret_cast<void *>(kDetour[slot].eval_c));
        LeaveCriticalSection(&g_hook_cs);

        Log("  hooked: CreateFeature=%s EvaluateFeature=%s EvaluateFeature_C=%s",
            ok_create ? "yes" : "FAILED",
            eval   != nullptr ? (ok_eval   ? "yes" : "FAILED") : "absent",
            eval_c != nullptr ? (ok_eval_c ? "yes" : "FAILED") : "absent");

        InterlockedIncrement(&g_layer_count);
        ++added;
    }
    return added;
}

static void LogPrologue(const char *label, const BYTE *p)
{
    char hex[64];
    int  n = 0;
    for (int i = 0; i < 14; ++i)
        n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%02X ", p[i]);

    // A jump where the prologue should be means something else hooked this
    // first. Chaining onto it can work, but when it does not this is the line
    // that explains why, so it is worth saying out loud.
    const bool detoured = (p[0] == 0xE9) || (p[0] == 0xFF && p[1] == 0x25) ||
                          (p[0] == 0x48 && p[1] == 0xB8) || (p[0] == 0xEB);
    Log("  %-16s prologue: %s%s", label, hex,
        detoured ? " <== already hooked by something else" : "");
}

// A log that only describes this add-on cannot diagnose a setup problem. These
// two record what the machine and the folder actually look like, because the
// most common failure is not a bug here but a missing file next to it.
typedef LONG (WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOEXW *);

static void LogEnvironment()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log("  host: %ls", exe);

    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
    {
        if (auto rtl = reinterpret_cast<PFN_RtlGetVersion>(GetProcAddress(nt, "RtlGetVersion")))
        {
            OSVERSIONINFOEXW vi = {};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (rtl(&vi) == 0)
                Log("  windows: %lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion,
                    vi.dwBuildNumber);
        }
    }
}

// The pieces this bridge needs are supplied by other people and dropped in by
// hand, so listing which of them are actually present turns "it does nothing"
// into an answer.
static void LogNeighbours()
{
    wchar_t dir[MAX_PATH] = {};
    GetModuleFileNameW(g_self, dir, MAX_PATH);
    if (wchar_t *s = wcsrchr(dir, L'\\')) *(s + 1) = L'\0';

    static const wchar_t *needed[] = {
        L"renodx-dlss5.addon64",   // the add-on that does the neural rendering
        L"nvngx_dlssnr.dll",       // the model it loads
        L"nvngx_dlss.dll",         // optional, a newer super-resolution model
    };

    Log("  files next to this add-on:");
    for (int i = 0; i < 3; ++i)
    {
        const wchar_t *n = needed[i];
        wchar_t p[MAX_PATH];
        wcscpy_s(p, dir);
        wcscat_s(p, n);
        const bool here = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
        Log("    %-26ls %s", n, here ? "present" : "MISSING");

        // The first two are required. Missing either means nothing will happen
        // and no amount of staring at this add-on will explain why, so it is
        // raised where the user will actually see it.
        if (!here && i < 2)
            Warn("%ls is missing from the game folder. Without it this bridge "
                 "has nothing to hand the DLSS 5 add-on and will do nothing.", n);
    }

    // Everything else that could be taking part, so conflicts are visible.
    wchar_t pattern[MAX_PATH];
    wcscpy_s(pattern, dir);
    wcscat_s(pattern, L"*.addon*");
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        Log("  add-ons present:");
        do { Log("    %ls", fd.cFileName); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// When the D3D11 entry points are not found, the useful question is what IS
// loaded. Reports that only say "not found" cannot be acted on; this turns the
// next one into evidence.
static void LogNgxCandidates()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto enum_modules =
        reinterpret_cast<PFN_EnumProcessModules>(GetProcAddress(k32, "K32EnumProcessModules"));
    if (enum_modules == nullptr) return;

    HMODULE mods[1024];
    DWORD   needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;

    Log("  modules that expose any NGX or DLSS entry point:");
    bool any = false;
    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i)
    {
        const bool d3d11 = GetProcAddress(mods[i], "NVSDK_NGX_D3D11_CreateFeature") != nullptr;
        const bool d3d12 = GetProcAddress(mods[i], "NVSDK_NGX_D3D12_CreateFeature") != nullptr;
        const bool vk    = GetProcAddress(mods[i], "NVSDK_NGX_VULKAN_CreateFeature") != nullptr;

        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mods[i], path, MAX_PATH);

        // Streamline sits above NGX, so a game using it looks different from
        // one calling NGX directly. Worth naming even without NGX exports.
        const wchar_t *leaf = wcsrchr(path, L'\\');
        leaf = leaf ? leaf + 1 : path;
        const bool interesting = (_wcsnicmp(leaf, L"sl.", 3) == 0) ||
                                 (wcsstr(leaf, L"nvngx") != nullptr);

        if (!d3d11 && !d3d12 && !vk && !interesting) continue;

        Log("    %ls  ->%s%s%s%s", path, d3d11 ? " D3D11" : "", d3d12 ? " D3D12" : "",
            vk ? " VULKAN" : "",
            (!d3d11 && !d3d12 && !vk) ? " (no NGX exports)" : "");
        any = true;
    }
    if (!any)
        Log("    none. NGX is not loaded in this process, so DLSS has not been "
            "initialised -- check that DLSS is actually enabled in the game.");
}
// ---------------------------------------------------------------------------
// Finding NGX
//
// NGX is loaded well after the process starts, so the entry points are not
// there to hook at attach time. This used to poll on a background thread, which
// was wrong: an add-on can be unloaded at any moment -- Skyrim loads and drops
// them inside a 400 ms hardware-detection pass -- and a thread still executing
// inside the module when it leaves memory kills the process.
//
// The loader will say when a library arrives instead. No thread, so no window
// in which the module can vanish underneath one, and unregistering is ordered.
// ---------------------------------------------------------------------------

typedef void (CALLBACK *PFN_LdrDllNotification)(ULONG reason, const void *data, void *ctx);
typedef LONG (NTAPI *PFN_LdrRegisterDllNotification)(ULONG, PFN_LdrDllNotification, void *, void **);
typedef LONG (NTAPI *PFN_LdrUnregisterDllNotification)(void *);

static void *g_ldr_cookie;
static PFN_LdrUnregisterDllNotification g_ldr_unregister;
static volatile LONG g_hooks_installed;
static ULONGLONG     g_hook_time;
static volatile LONG g_idle_reported;

// Safe to call repeatedly and from the loader callback: it does nothing once
// the hooks are in, and everything it does before that is a name lookup.
static void TryInstallHooks()
{
    // Not "once and done" any more. The game's own exports are present from the
    // start, but the feature snippet underneath only shows up when DLSS
    // initialises, so every load is another chance to find a layer.
    if (g_layer_count >= kMaxLayers) return;

    EnterCriticalSection(&g_hook_cs);
    const int added = HookNewNgxModules();
    LeaveCriticalSection(&g_hook_cs);
    if (added == 0) return;

    if (InterlockedCompareExchange(&g_hooks_installed, 1, 0) != 0) return;

    g_hook_time = GetTickCount64();

    // Streamline is a second, parallel way to reach DLSS. A game or mod using it
    // never calls the functions above, so the hooks are in and nothing arrives --
    // which looks identical to DLSS simply being switched off. Say which it is.
    if (GetModuleHandleW(L"sl.interposer.dll") != nullptr)
        Log("  sl.interposer.dll is loaded here, so DLSS in this game may go through "
            "Streamline instead. Streamline does not call the functions above and the "
            "bridge cannot see it.");
}

// The "nothing ever called us" diagnosis used to be written only at unload, and
// most games never run DLL detach -- so the one log that needed it said nothing.
// Report it as soon as it is true instead. There is no timer thread to hang it
// on by design, so it rides the loader notification: a game doing anything at
// all keeps loading DLLs.
static void ReportIdle()
{
    if (InterlockedCompareExchange(&g_hooks_installed, 0, 0) == 0) return;
    if (g_eval_count != 0) return;
    if (GetTickCount64() - g_hook_time < 60000) return;
    if (InterlockedCompareExchange(&g_idle_reported, 1, 0) != 0) return;

    Log("");
    Log("60 seconds after hooking, nothing has called DLSS through these entry points.");
    Log("  Not a hooking problem. Either DLSS is off in the game's own settings, or");
    Log("  whatever provides it does not use the D3D11 NGX exports -- Streamline and");
    Log("  D3D12 both go elsewhere. The bridge only works from a D3D11 NGX call.");
}

static void CALLBACK OnDllLoaded(ULONG reason, const void *, void *)
{
    // 1 is LDR_DLL_NOTIFICATION_REASON_LOADED. This runs under the loader lock,
    // so it does the least it can: a name lookup, and the hook writes.
    if (reason == 1) { TryInstallHooks(); ReportIdle(); }
}

static void StartWatchingForNgx()
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto reg = nt != nullptr ? reinterpret_cast<PFN_LdrRegisterDllNotification>(
        GetProcAddress(nt, "LdrRegisterDllNotification")) : nullptr;
    g_ldr_unregister = nt != nullptr ? reinterpret_cast<PFN_LdrUnregisterDllNotification>(
        GetProcAddress(nt, "LdrUnregisterDllNotification")) : nullptr;

    if (reg != nullptr) reg(0, &OnDllLoaded, nullptr, &g_ldr_cookie);
    else Log("loader notifications unavailable; NGX will only be found if it is already loaded");

    // It may already be there, in which case no notification is coming.
    TryInstallHooks();
}

static void StopWatchingForNgx()
{
    if (g_ldr_cookie != nullptr && g_ldr_unregister != nullptr)
    {
        g_ldr_unregister(g_ldr_cookie);
        g_ldr_cookie = nullptr;
    }
}

// Said at unload, when the answer is finally known, rather than guessed at from
// a timer partway through.
static void ReportOutcome()
{
    if (InterlockedCompareExchange(&g_hooks_installed, 0, 0) == 0)
    {
        Log("");
        Log("NGX D3D11 entry points never appeared while this add-on was loaded.");
        Log("  Either DLSS was off, or whatever provides it does not use the D3D11");
        Log("  NGX path -- Streamline and D3D12 both bypass these functions.");
        LogNgxCandidates();
    }
    else if (g_eval_count == 0 &&
             InterlockedCompareExchange(&g_idle_reported, 1, 0) == 0)
    {
        Log("");
        Log("The entry points were hooked but nothing ever called DLSS through them.");
        Log("  Not a hooking problem. Either DLSS is off in the game, or the call");
        Log("  goes somewhere else.");
    }
}

// ---------------------------------------------------------------------------
// ReShade add-on registration
//
// Replicates what reshade.hpp's register_addon does, without the SDK: locate
// the module exporting ReShadeRegisterAddon and call it. The API version is
// negotiated downwards because ReShade rejects a version newer than its own.
// ---------------------------------------------------------------------------

typedef bool (*PFN_ReShadeRegisterAddon)(HMODULE, uint32_t);
typedef void (*PFN_ReShadeUnregisterAddon)(HMODULE);

static PFN_ReShadeUnregisterAddon g_unregister;

static bool RegisterWithReShade(HMODULE self)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) return false;
    auto enum_modules =
        reinterpret_cast<PFN_EnumProcessModules>(GetProcAddress(k32, "K32EnumProcessModules"));
    if (enum_modules == nullptr) return false;

    HMODULE mods[1024];
    DWORD   needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return false;

    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i)
    {
        auto reg = reinterpret_cast<PFN_ReShadeRegisterAddon>(
            GetProcAddress(mods[i], "ReShadeRegisterAddon"));
        if (reg == nullptr) continue;

        for (uint32_t version = 18; version >= 5; --version)
        {
            if (reg(self, version))
            {
                g_unregister = reinterpret_cast<PFN_ReShadeUnregisterAddon>(
                    GetProcAddress(mods[i], "ReShadeUnregisterAddon"));
                g_reshade_log = reinterpret_cast<PFN_ReShadeLogMessage>(
                    GetProcAddress(mods[i], "ReShadeLogMessage"));
                g_reshade_module = self;
                return true;
            }
        }
    }
    return false;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_log_cs);
        InitializeCriticalSection(&g_hook_cs);

        GetModuleFileNameA(module, g_log_path, MAX_PATH);
        char *slash = strrchr(g_log_path, '\\');
        if (slash != nullptr)
            strcpy_s(slash + 1, MAX_PATH - (slash + 1 - g_log_path), "dlss5-dx11-bridge.log");

        if (!RegisterWithReShade(module))
            return FALSE;  // ReShade will unload us; do not leave hooks behind

        // Carry forward the previous run's crash report, if it left one. This
        // looks for a marker the crash handler writes, not for the absence of a
        // clean-exit marker: many games terminate the process without ever
        // running DLL detach, so absence proves nothing and reporting it would
        // claim a crash on every ordinary launch.
        char carried[2048] = {};
        {
            FILE *old = nullptr;
            if (fopen_s(&old, g_log_path, "r") == 0 && old != nullptr)
            {
                char line[512];
                bool in_report = false;
                while (fgets(line, sizeof(line), old) != nullptr)
                {
                    if (strstr(line, "### CRASH RECORDED ###") != nullptr)
                    {
                        in_report = true;
                        carried[0] = '\0';
                        continue;
                    }
                    if (in_report && strstr(line, "####") != nullptr) break;
                    if (in_report) strcat_s(carried, line);
                }
                fclose(old);
            }

            FILE *f = nullptr;
            if (fopen_s(&f, g_log_path, "w") == 0 && f != nullptr) fclose(f);
        }

        g_prev_filter = SetUnhandledExceptionFilter(&CrashFilter);

        // First line of every log, so a report can name the build exactly,
        // followed by everything needed to diagnose a setup remotely.
        Log("dlss5-dx11-bridge %s (built %s %s) attached.", BRIDGE_VERSION, __DATE__, __TIME__);
        if (carried[0] != '\0')
        {
            Log("The previous run crashed. What it recorded at the time:");
            Log("%s", carried);
        }

        LogEnvironment();
        LogNeighbours();

        // Written now rather than when the D3D12 session opens, so the file
        // exists even in a game where nothing ever hooks -- and so stage=0 is
        // available as an off switch before launching, without deleting this.
        CfgWriteDefault();
        StartWatchingForNgx();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // Stop being told about new libraries before anything else, so no
        // callback can arrive while the rest of this is tearing down.
        StopWatchingForNgx();
        ReportOutcome();

        EnterCriticalSection(&g_hook_cs);
        for (LONG i = 0; i < g_layer_count; ++i)
        {
            HookRemove(g_layer[i].eval);
            HookRemove(g_layer[i].eval_c);
            HookRemove(g_layer[i].create);
            g_layer[i].eval.active = g_layer[i].eval_c.active =
                g_layer[i].create.active = false;
        }
        LeaveCriticalSection(&g_hook_cs);
        if (g_unregister != nullptr) g_unregister(g_self);

        // The marker the next run looks for. Its absence is what says the last
        // session ended abruptly rather than by quitting.
        Log("shut down cleanly.");
    }
    return TRUE;
}
