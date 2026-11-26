// ============================================================
//  InfluClick — Ultra Fast AutoClicker
//  C++20 | ImGui 1.91 | DirectX 11 | SendInput
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // Windows 10 target (runs on Win 10 and Win 11)
#endif
#include <windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>

#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")

// ════════════════════════════════════════════════════════════
//  Shared state (lock-free atomics)
// ════════════════════════════════════════════════════════════
static std::atomic<bool> g_running    {true};
static std::atomic<bool> g_clicking   {false};
static std::atomic<int>  g_cps_target {100};
static std::atomic<int>  g_actual_cps {0};
static std::atomic<int>  g_hotkey     {VK_F6};
static std::atomic<bool> g_capturing        {false}; // true while hotkey capture is active
static std::atomic<bool> g_skip_next_hotkey {false}; // skip first press after hotkey reassign
static std::atomic<bool> g_paused_by_hover  {false}; // true when cursor is over our window
static std::atomic<int>  g_mouse_btn  {0};   // 0=left 1=right 2=middle
static std::atomic<bool> g_jitter     {false};
static std::atomic<int>  g_jitter_pct {10};

// Global HWND so threads can check window position
static HWND g_hwnd = nullptr;

// UI scale = 1.10 (window +10 % in width and height) * system DPI scale
static float g_ui = 1.10f;
static inline float S(float v) { return v * g_ui; }

// Diagnostic log: influclick_log.txt next to the exe (helps debugging on other PCs)
static void Log(const char* fmt, ...)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(path, L"influclick_log.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || !f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ════════════════════════════════════════════════════════════
//  Click thread — maximum speed
// ════════════════════════════════════════════════════════════
static void ClickerThread()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    // ── PRNG: xorshift64 — fast, good distribution ──────────
    // Seeded from QPC so every run is different
    LARGE_INTEGER seed_qi; QueryPerformanceCounter(&seed_qi);
    unsigned long long xr = (unsigned long long)seed_qi.QuadPart ^ 0xDEADBEEFCAFEULL;
    auto rng64 = [&]() -> unsigned long long {
        xr ^= xr << 13; xr ^= xr >> 7; xr ^= xr << 17; return xr;
    };
    // Uniform float [0, 1)
    auto rndf = [&]() -> double {
        return (double)(rng64() >> 11) * (1.0 / (double)(1ULL << 53));
    };
    // Box-Muller: Gaussian with mean=0, stddev=1
    // Returns values clustered around 0, rarely exceeding ±3
    bool   bm_has_spare = false;
    double bm_spare     = 0.0;
    auto gauss = [&]() -> double {
        if (bm_has_spare) { bm_has_spare = false; return bm_spare; }
        double u, v, s;
        do { u = rndf()*2.0-1.0; v = rndf()*2.0-1.0; s = u*u+v*v; }
        while (s >= 1.0 || s == 0.0);
        double m = std::sqrt(-2.0 * std::log(s) / s);
        bm_spare     = v * m; bm_has_spare = true;
        return u * m;
    };

    // ── Human rhythm model ───────────────────────────────────
    // drift: slow sinusoidal rhythm variation (breathing / fatigue)
    // Every ~3-8 seconds the rhythm shifts slightly
    double drift_phase    = rndf() * 6.283;      // random start phase
    double drift_speed    = 0.008 + rndf()*0.006; // how fast drift evolves
    double drift_amp      = 0.012;                // ±1.2% slow drift
    int    clicks_since_reset = 0;

    int  count     = 0;
    auto sec_start = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_relaxed))
    {
        if (!g_clicking.load(std::memory_order_relaxed) ||
             g_paused_by_hover.load(std::memory_order_relaxed))
        {
            Sleep(1);
            count             = 0;
            clicks_since_reset = 0;
            sec_start         = std::chrono::steady_clock::now();
            g_actual_cps.store(0, std::memory_order_relaxed);
            // Reset drift on stop so each session feels fresh
            drift_phase = rndf() * 6.283;
            drift_speed = 0.008 + rndf()*0.006;
            continue;
        }

        const int  target = g_cps_target.load(std::memory_order_relaxed);
        const bool jitter = g_jitter.load(std::memory_order_relaxed);
        const int  jpct   = g_jitter_pct.load(std::memory_order_relaxed);

        double base_ms = 1000.0 / (double)target; // ideal interval in ms

        if (jitter && jpct > 0)
        {
            // ── 1. Gaussian inter-click jitter ───────────────
            // stddev = jpct% of base interval, clamped to ±3σ
            double sigma = base_ms * (jpct / 100.0);
            double g     = gauss();
            // Soft-clamp: values beyond ±2.5σ get compressed, not cut
            if (g >  2.5) g =  2.5 + (g -  2.5) * 0.2;
            if (g < -2.5) g = -2.5 + (g + 2.5) * 0.2;
            double interval = base_ms + g * sigma;

            // ── 2. Slow rhythm drift (fatigue / breathing) ───
            // Sinusoidal, evolves each click — imperceptible per-click
            // but creates a natural ebb-and-flow over seconds
            drift_phase += drift_speed;
            double drift  = std::sin(drift_phase) * drift_amp * base_ms;
            interval     += drift;

            // ── 3. Rare hesitation spike ─────────────────────
            // ~1 in 180 clicks: +15-45 ms pause (finger stumble)
            // Only at lower CPS where a 30ms pause is plausible
            if (target <= 25 && (rng64() % 180) == 0)
                interval += 15.0 + rndf() * 30.0;

            if (interval < 1.0) interval = 1.0;
            base_ms = interval;
        }

        long long base_ticks = (long long)(base_ms * (double)freq.QuadPart / 1000.0);
        if (base_ticks < 1) base_ticks = 1;

        // ── 4. Variable press duration ───────────────────────
        // Humans hold the button for a short random time before release.
        // Typical: 20-80 ms at low CPS, shorter at high CPS.
        // At very high CPS (>100) we skip this to preserve timing.
        long long hold_ticks = 0;
        if (jitter && target <= 80)
        {
            double max_hold_ms = std::min(base_ms * 0.35, 55.0); // max 35% of interval
            double hold_ms     = 8.0 + rndf() * (max_hold_ms - 8.0);
            hold_ticks = (long long)(hold_ms * (double)freq.QuadPart / 1000.0);
        }

        // ── Send mouse down ──────────────────────────────────
        DWORD dn, up;
        switch (g_mouse_btn.load(std::memory_order_relaxed))
        {
        case 1:  dn = MOUSEEVENTF_RIGHTDOWN;  up = MOUSEEVENTF_RIGHTUP;  break;
        case 2:  dn = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; break;
        default: dn = MOUSEEVENTF_LEFTDOWN;   up = MOUSEEVENTF_LEFTUP;   break;
        }

        INPUT inp_dn = {}; inp_dn.type = INPUT_MOUSE; inp_dn.mi.dwFlags = dn;
        INPUT inp_up = {}; inp_up.type = INPUT_MOUSE; inp_up.mi.dwFlags = up;

        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        SendInput(1, &inp_dn, sizeof(INPUT));

        // Hold: spin-wait before releasing
        if (hold_ticks > 0)
        {
            const long long hold_end = t0.QuadPart + hold_ticks;
            LARGE_INTEGER th;
            do {
                QueryPerformanceCounter(&th);
                if (hold_end - th.QuadPart > freq.QuadPart / 2000LL) Sleep(0);
            } while (th.QuadPart < hold_end);
        }

        SendInput(1, &inp_up, sizeof(INPUT));
        ++count;
        ++clicks_since_reset;

        auto now_tp = std::chrono::steady_clock::now();
        auto ms_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now_tp - sec_start).count();
        if (ms_elapsed >= 1000)
        {
            g_actual_cps.store(count, std::memory_order_relaxed);
            count     = 0;
            sec_start = now_tp;
        }

        // ── Spin-wait for next click ─────────────────────────
        const long long target_tick = t0.QuadPart + base_ticks;
        LARGE_INTEGER tnow;
        do {
            QueryPerformanceCounter(&tnow);
            if (target_tick - tnow.QuadPart > freq.QuadPart / 2000LL)
                Sleep(0);
        } while (tnow.QuadPart < target_tick);
    }
}

// ════════════════════════════════════════════════════════════
//  Hotkey thread
// ════════════════════════════════════════════════════════════
static void HotkeyThread()
{
    while (g_running.load(std::memory_order_relaxed))
    {
        if (g_capturing.load(std::memory_order_relaxed)) { Sleep(10); continue; }

        const int key = g_hotkey.load(std::memory_order_relaxed);

        // After a hotkey reassign the user still has the key held — wait for release
        // before we start monitoring, so the assign itself never triggers a toggle.
        if (g_skip_next_hotkey.load(std::memory_order_relaxed))
        {
            if (GetAsyncKeyState(key) & 0x8000) { Sleep(5); continue; }
            g_skip_next_hotkey.store(false, std::memory_order_relaxed);
            Sleep(10);
            continue;
        }

        if (GetAsyncKeyState(key) & 0x8000)
        {
            g_clicking.store(!g_clicking.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
            while (g_running.load(std::memory_order_relaxed) &&
                   (GetAsyncKeyState(key) & 0x8000))
                Sleep(5);
        }
        Sleep(10);
    }
}

// ════════════════════════════════════════════════════════════
//  DirectX 11
// ════════════════════════════════════════════════════════════
static ID3D11Device*           g_dev = nullptr;
static ID3D11DeviceContext*    g_ctx = nullptr;
static IDXGISwapChain*         g_sc  = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;

static void CreateRTV()
{
    ID3D11Texture2D* bb = nullptr;
    HRESULT hr = g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (FAILED(hr) || !bb) { Log("GetBuffer failed 0x%08X", (unsigned)hr); return; }
    hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    if (FAILED(hr)) Log("CreateRenderTargetView failed 0x%08X", (unsigned)hr);
    bb->Release();
}
static void DestroyRTV() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

static bool InitDX11(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd  = {};
    sd.BufferCount           = 2;
    sd.BufferDesc.Format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage           = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow          = hwnd;
    sd.SampleDesc.Count      = 1;
    sd.Windowed              = TRUE;
    sd.SwapEffect            = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags                 = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    D3D_FEATURE_LEVEL lvls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL out;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        lvls, 3, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &out, &g_ctx);
    // Win 10 machines with old/missing GPU drivers: fall back to software (WARP)
    if (FAILED(hr))
    {
        Log("Hardware D3D11 failed: 0x%08X -> trying WARP", (unsigned)hr);
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            lvls, 3, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &out, &g_ctx);
    }
    if (FAILED(hr)) { Log("WARP failed: 0x%08X", (unsigned)hr); return false; }
    Log("D3D11 OK, feature level 0x%X", (unsigned)out);
    CreateRTV();
    return true;
}
static void ShutdownDX11()
{
    DestroyRTV();
    if (g_sc)  { g_sc->Release();  g_sc  = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

// ════════════════════════════════════════════════════════════
//  WndProc
// ════════════════════════════════════════════════════════════
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg)
    {
    case WM_SIZE:
        if (g_dev && g_sc && wp != SIZE_MINIMIZED && LOWORD(lp) > 0 && HIWORD(lp) > 0)
        {
            DestroyRTV();
            // flags must match the ones used at swap-chain creation
            HRESULT hr = g_sc->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN,
                                             DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
            if (FAILED(hr)) Log("ResizeBuffers failed: 0x%08X", (unsigned)hr);
            CreateRTV();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ════════════════════════════════════════════════════════════
//  Neon dark theme
// ════════════════════════════════════════════════════════════
static void ApplyNeonTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 0.f;
    s.ChildRounding     = 8.f;
    s.FrameRounding     = 7.f;
    s.GrabRounding      = 7.f;
    s.PopupRounding     = 7.f;
    s.ScrollbarRounding = 7.f;
    s.TabRounding       = 7.f;
    s.WindowPadding     = {16.f, 14.f};
    s.FramePadding      = {10.f, 6.f};
    s.ItemSpacing       = {10.f, 7.f};
    s.ItemInnerSpacing  = {7.f,  5.f};
    s.WindowBorderSize  = 0.f;
    s.FrameBorderSize   = 1.f;
    s.GrabMinSize       = 12.f;
    s.ScrollbarSize     = 7.f;

    using C = ImVec4;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]          = C{0.035f,0.035f,0.060f,1.f};
    c[ImGuiCol_ChildBg]           = C{0.050f,0.050f,0.085f,1.f};
    c[ImGuiCol_PopupBg]           = C{0.040f,0.040f,0.070f,0.97f};
    c[ImGuiCol_Border]            = C{0.00f,0.72f,0.82f,0.22f};
    c[ImGuiCol_BorderShadow]      = C{0.f,0.f,0.f,0.f};
    c[ImGuiCol_FrameBg]           = C{0.08f,0.08f,0.13f,1.f};
    c[ImGuiCol_FrameBgHovered]    = C{0.00f,0.70f,0.82f,0.16f};
    c[ImGuiCol_FrameBgActive]     = C{0.00f,0.72f,0.84f,0.28f};
    c[ImGuiCol_TitleBg]           = c[ImGuiCol_WindowBg];
    c[ImGuiCol_TitleBgActive]     = c[ImGuiCol_WindowBg];
    c[ImGuiCol_TitleBgCollapsed]  = c[ImGuiCol_WindowBg];
    c[ImGuiCol_ScrollbarBg]       = C{0.03f,0.03f,0.06f,1.f};
    c[ImGuiCol_ScrollbarGrab]         = C{0.f,0.70f,0.82f,0.45f};
    c[ImGuiCol_ScrollbarGrabHovered]  = C{0.f,0.78f,0.90f,0.70f};
    c[ImGuiCol_ScrollbarGrabActive]   = C{0.f,0.90f,1.00f,1.f};
    c[ImGuiCol_CheckMark]         = C{0.f,0.90f,1.f,1.f};
    c[ImGuiCol_SliderGrab]        = C{0.f,0.78f,0.90f,0.92f};
    c[ImGuiCol_SliderGrabActive]  = C{0.f,1.00f,1.00f,1.f};
    c[ImGuiCol_Button]            = C{0.f,0.62f,0.72f,0.22f};
    c[ImGuiCol_ButtonHovered]     = C{0.f,0.72f,0.84f,0.36f};
    c[ImGuiCol_ButtonActive]      = C{0.f,0.88f,1.00f,0.55f};
    c[ImGuiCol_Header]            = C{0.f,0.62f,0.72f,0.22f};
    c[ImGuiCol_HeaderHovered]     = C{0.f,0.72f,0.84f,0.30f};
    c[ImGuiCol_HeaderActive]      = C{0.f,0.88f,1.00f,0.42f};
    c[ImGuiCol_Separator]         = C{0.f,0.60f,0.70f,0.18f};
    c[ImGuiCol_SeparatorHovered]  = C{0.f,0.75f,0.85f,0.50f};
    c[ImGuiCol_SeparatorActive]   = C{0.f,0.90f,1.00f,0.80f};
    c[ImGuiCol_ResizeGrip]        = C{0.f,0.70f,0.82f,0.18f};
    c[ImGuiCol_ResizeGripHovered] = C{0.f,0.80f,0.90f,0.50f};
    c[ImGuiCol_ResizeGripActive]  = C{0.f,0.92f,1.00f,0.80f};
    c[ImGuiCol_Tab]               = C{0.06f,0.06f,0.10f,1.f};
    c[ImGuiCol_TabHovered]        = C{0.f,0.70f,0.82f,0.30f};
    c[ImGuiCol_TabActive]         = C{0.f,0.60f,0.72f,0.45f};
    c[ImGuiCol_TabUnfocused]      = c[ImGuiCol_Tab];
    c[ImGuiCol_TabUnfocusedActive]= c[ImGuiCol_Tab];
    c[ImGuiCol_PlotLines]             = C{0.f,0.85f,0.95f,1.f};
    c[ImGuiCol_PlotLinesHovered]      = C{0.f,1.00f,1.00f,1.f};
    c[ImGuiCol_PlotHistogram]         = C{0.f,0.65f,0.78f,1.f};
    c[ImGuiCol_PlotHistogramHovered]  = C{0.f,0.88f,1.00f,1.f};
    c[ImGuiCol_Text]              = C{0.88f,0.93f,1.00f,1.f};
    c[ImGuiCol_TextDisabled]      = C{0.32f,0.40f,0.54f,1.f};
    c[ImGuiCol_NavHighlight]      = C{0.f,0.80f,0.90f,0.80f};

    s.ScaleAllSizes(g_ui);   // paddings, rounding, grabs... scaled once
}

// ════════════════════════════════════════════════════════════
//  Utilities
// ════════════════════════════════════════════════════════════
static std::string VkName(int vk)
{
    switch (vk) {
    case VK_F1:  return "F1";  case VK_F2:  return "F2";  case VK_F3:  return "F3";
    case VK_F4:  return "F4";  case VK_F5:  return "F5";  case VK_F6:  return "F6";
    case VK_F7:  return "F7";  case VK_F8:  return "F8";  case VK_F9:  return "F9";
    case VK_F10: return "F10"; case VK_F11: return "F11"; case VK_F12: return "F12";
    case VK_INSERT:   return "Ins";   case VK_DELETE: return "Del";
    case VK_HOME:     return "Home";  case VK_END:    return "End";
    case VK_PRIOR:    return "PgUp";  case VK_NEXT:   return "PgDn";
    case VK_PAUSE:    return "Pause";
    case VK_NUMPAD0:  return "Num0";  case VK_NUMPAD1: return "Num1";
    case VK_MULTIPLY: return "Num*";  case VK_ADD:     return "Num+";
    case VK_SUBTRACT: return "Num-";  case VK_DIVIDE:  return "Num/";
    default: {
        UINT scan = MapVirtualKey((UINT)vk, MAPVK_VK_TO_VSC);
        wchar_t wbuf[16] = {};
        if (GetKeyNameTextW((LONG)(scan << 16), wbuf, 16) > 0)
        {
            char cbuf[16] = {};
            WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, cbuf, 16, nullptr, nullptr);
            return cbuf;
        }
        char tmp[8]; std::snprintf(tmp, sizeof(tmp), "0x%02X", vk);
        return tmp;
    }
    }
}

// ════════════════════════════════════════════════════════════
//  CPS graph ring buffer
// ════════════════════════════════════════════════════════════
static constexpr int kHist = 96;
static float g_hist[kHist] = {};
static int   g_hist_i      = 0;

// ════════════════════════════════════════════════════════════
//  WinMain
// ════════════════════════════════════════════════════════════
static void EnableDpiAwareness()
{
    // Win 10 1607+: SetProcessDpiAwarenessContext; older builds: SetProcessDPIAware
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    using SetCtxFn = BOOL (WINAPI*)(HANDLE);
    auto fn = u32 ? (SetCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : nullptr;
    if (fn) { fn((HANDLE)-2 /* SYSTEM_AWARE */); return; }
    SetProcessDPIAware();
}

static float GetSystemDpiScale()
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    using GetDpiFn = UINT (WINAPI*)();
    auto fn = u32 ? (GetDpiFn)GetProcAddress(u32, "GetDpiForSystem") : nullptr;
    if (fn) return fn() / 96.f;
    HDC dc = GetDC(nullptr);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi / 96.f;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    EnableDpiAwareness();
    g_ui = 1.10f * GetSystemDpiScale();

    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // ── Window ──────────────────────────────────────────────
    WNDCLASSEXW wc    = {};
    wc.cbSize         = sizeof(wc);
    wc.style          = CS_CLASSDC;
    wc.lpfnWndProc    = WndProc;
    wc.hInstance      = hInst;
    wc.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon          = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm        = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.lpszClassName  = L"InfluClick";
    RegisterClassExW(&wc);

    // Compact fixed size — fits all content with no scrolling
    // 420x468 originally, now +10 % in both directions (x DPI scale)
    const int CLIENT_W = (int)std::lround(420.f * g_ui);
    const int CLIENT_H = (int)std::lround(468.f * g_ui);
    RECT rc = {0, 0, CLIENT_W, CLIENT_H};
    AdjustWindowRectEx(&rc, WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX, FALSE, WS_EX_TOPMOST);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,                     // always on top
        wc.lpszClassName,
        L"InfluClick",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);

    // DWM dark title bar
    {
        BOOL dm = TRUE;
        if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dm, sizeof(dm))))   // DWMWA_USE_IMMERSIVE_DARK_MODE
            DwmSetWindowAttribute(hwnd, 19, &dm, sizeof(dm));           // older Win 10 builds
    }

    if (!InitDX11(hwnd))
    {
        MessageBoxW(hwnd, L"DirectX 11 init failed.", L"InfluClick", MB_ICONERROR);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInst);
        timeEndPeriod(1);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    g_hwnd = hwnd; // store for hover detection

    // ── ImGui ────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename   = nullptr;

    {
        const wchar_t* fonts[] = {
            L"C:\\Windows\\Fonts\\segoeui.ttf",
            L"C:\\Windows\\Fonts\\calibri.ttf",
            L"C:\\Windows\\Fonts\\tahoma.ttf",
            nullptr
        };
        bool loaded = false;
        for (auto** p = fonts; *p && !loaded; ++p)
            if (GetFileAttributesW(*p) != INVALID_FILE_ATTRIBUTES)
            {
                char path[MAX_PATH];
                WideCharToMultiByte(CP_UTF8,0,*p,-1,path,MAX_PATH,nullptr,nullptr);
                io.Fonts->AddFontFromFileTTF(path, 14.5f * g_ui);
                loaded = true;
            }
        if (!loaded)
        {
            ImFontConfig fc; fc.SizePixels = 13.f * g_ui;
            io.Fonts->AddFontDefault(&fc);
        }
    }

    ApplyNeonTheme();
    ImGui_ImplWin32_Init(hwnd);
    if (!ImGui_ImplDX11_Init(g_dev, g_ctx))
    {
        Log("ImGui_ImplDX11_Init failed");
        MessageBoxW(hwnd, L"ImGui DX11 init failed. See influclick_log.txt.", L"InfluClick", MB_ICONERROR);
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        ShutdownDX11();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInst);
        timeEndPeriod(1);
        return 1;
    }
    Log("ImGui init OK, ui scale %.2f", g_ui);
    if (!g_rtv) Log("WARNING: render target view is null");

    // ── Threads ──────────────────────────────────────────────
    std::thread t_click(ClickerThread);
    std::thread t_hotkey(HotkeyThread);

    // ── UI state ─────────────────────────────────────────────
    int  ui_cps    = 100;
    int  ui_mbtn   = 0;
    bool ui_jitter = true;
    int  ui_jpct   = 5;
    bool wait_key       = false;
    int  wait_key_frames = 0;  // frames to wait before scanning

    auto last_hist = std::chrono::steady_clock::now();
    const float CLR[4] = {0.035f, 0.035f, 0.060f, 1.f};

    // ── Main loop ────────────────────────────────────────────
    MSG  msg  = {};
    bool done = false;
    while (!done)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        auto now_tp = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now_tp - last_hist).count() >= 250)
        {
            g_hist[g_hist_i] = (float)g_actual_cps.load(std::memory_order_relaxed);
            g_hist_i = (g_hist_i + 1) % kHist;
            last_hist = now_tp;
        }

        // ── Hover pause: pause clicking while cursor is over our window ──
        {
            POINT pt; GetCursorPos(&pt);
            HWND hovered = WindowFromPoint(pt);
            bool on_app  = (hovered == g_hwnd || IsChild(g_hwnd, hovered));
            g_paused_by_hover.store(on_app, std::memory_order_relaxed);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const bool  clicking    = g_clicking.load(std::memory_order_relaxed);
        const bool  hover_pause = g_paused_by_hover.load(std::memory_order_relaxed);
        const int   acps     = g_actual_cps.load(std::memory_order_relaxed);
        const float t        = (float)ImGui::GetTime();

        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoResize  |
            ImGuiWindowFlags_NoScrollbar  | ImGuiWindowFlags_NoMove    |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        const float WW   = ImGui::GetWindowWidth();
        const float PAD  = ImGui::GetStyle().WindowPadding.x;
        const float FULL = WW - PAD * 2.f;
        const float SP   = ImGui::GetStyle().ItemSpacing.x;

        // Helper: bright section label on its own line
        auto SectionHeader = [](const char* s) {
            ImGui::Spacing();
            ImGui::TextColored({0.55f, 0.70f, 0.88f, 1.f}, "%s", s);
        };

        // ╔══ HEADER ════════════════════════════════════════════╗
        // Title left, animated status pill right — no emojis
        {
            float pulse = 0.55f + 0.45f*(float)std::sin(t * 1.4f);
            ImVec4 titleCol = clicking
                ? ImVec4(0.f, pulse, pulse * 0.88f, 1.f)
                : ImVec4(0.55f, 0.65f, 0.82f, 1.f);
            // "InfluClick v2" — all on one line
            ImGui::TextColored(titleCol, "InfluClick v2");


            // Status pill
            const char* statusText = (clicking && hover_pause) ? "PAUSED"
                                   : clicking                  ? "ACTIVE"
                                                               : "STOPPED";
            float sw = ImGui::CalcTextSize(statusText).x + S(22.f);
            ImGui::SameLine(WW - PAD - sw);

            float sp2 = 0.40f + 0.24f*(float)std::sin(t * 3.2f);
            ImVec4 pillBg = (clicking && hover_pause)
                ? ImVec4(0.40f, 0.28f, 0.00f, 0.55f)                  // amber — paused
                : clicking
                ? ImVec4(0.f, sp2*0.55f, sp2*0.48f, 0.36f)            // green — active
                : ImVec4(0.09f, 0.09f, 0.16f, 1.f);                   // dim — stopped
            ImVec4 pillFg = (clicking && hover_pause)
                ? ImVec4(1.f, 0.80f, 0.20f, 1.f)
                : clicking
                ? ImVec4(0.f, 0.95f, 1.f, 1.f)
                : ImVec4(0.32f, 0.40f, 0.55f, 1.f);

            ImGui::PushStyleColor(ImGuiCol_Button,        pillBg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pillBg);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  pillBg);
            ImGui::PushStyleColor(ImGuiCol_Text,          pillFg);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(20.f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {S(9.f), S(3.f)});
            ImGui::Button(statusText, {sw, 0.f});
            ImGui::PopStyleVar();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
        }

        ImGui::Separator();
        ImGui::Spacing();

        // ╔══ LIVE CPS + GRAPH ══════════════════════════════════╗
        {
            ImGui::BeginGroup();
            {
                char num[16];
                std::snprintf(num, sizeof(num), "%d", acps);
                ImGui::TextColored({0.f, 0.94f, 1.f, 1.f}, "%s", num);
                ImGui::TextColored({0.38f, 0.50f, 0.66f, 1.f}, "CPS (live)");
                ImGui::Spacing();
                char tgt[24];
                std::snprintf(tgt, sizeof(tgt), "Target:  %d", ui_cps);
                ImGui::TextColored({0.28f, 0.60f, 0.70f, 1.f}, "%s", tgt);
            }
            ImGui::EndGroup();

            ImGui::SameLine(S(108.f), 0.f);

            float gw = FULL - S(108.f) + PAD;
            ImGui::PushStyleColor(ImGuiCol_FrameBg,   {0.04f,0.04f,0.08f,1.f});
            ImGui::PushStyleColor(ImGuiCol_PlotLines, {0.f,0.88f,1.f,1.f});
            float graph_max = (float)std::max(ui_cps, acps) * 1.25f;
            if (graph_max < 10.f) graph_max = 10.f;
            ImGui::PlotLines("##g", g_hist, kHist, g_hist_i,
                             nullptr, 0.f, graph_max, {gw, S(60.f)});
            ImGui::PopStyleColor(2);
        }

        ImGui::Spacing();
        ImGui::Separator();

        // ╔══ SPEED ════════════════════════════════════════════╗
        SectionHeader("Speed (CPS)");

        // Slider + numeric input on same row
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,       {0.f,0.80f,0.92f,1.f});
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, {0.f,1.00f,1.00f,1.f});
        ImGui::SetNextItemWidth(FULL - S(74.f) - SP);
        if (ImGui::SliderInt("##spd", &ui_cps, 1, 1000))
            g_cps_target.store(ui_cps, std::memory_order_relaxed);
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(74.f));
        if (ImGui::InputInt("##si", &ui_cps, 0, 0))
        {
            ui_cps = std::clamp(ui_cps, 1, 1000);
            g_cps_target.store(ui_cps, std::memory_order_relaxed);
        }

        // Presets — full width, no label
        {
            constexpr int kPS[] = {10, 50, 100, 250, 500, 1000};
            float bw = (FULL - 5.f*SP) / 6.f;
            for (int i = 0; i < 6; ++i)
            {
                if (i > 0) ImGui::SameLine();
                int  p   = kPS[i];
                bool sel = (ui_cps == p);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, {0.f,0.48f,0.60f,0.55f});
                char lbl[8]; std::snprintf(lbl, sizeof(lbl), "%d", p);
                if (ImGui::Button(lbl, {bw, 0.f})) { ui_cps = p; g_cps_target = p; }
                if (sel) ImGui::PopStyleColor();
            }
        }

        ImGui::Dummy({0.f, S(5.f)});
        ImGui::Separator();

        // ╔══ MOUSE BUTTON ═════════════════════════════════════╗
        SectionHeader("Mouse Button");
        {
            const char* mbtn[] = {" Left ", " Right "};
            float bw = (FULL - SP) / 2.f;
            for (int i = 0; i < 2; ++i)
            {
                if (i > 0) ImGui::SameLine();
                bool sel = (ui_mbtn == i);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, {0.f,0.48f,0.60f,0.55f});
                if (ImGui::Button(mbtn[i], {bw, S(26.f)})) { ui_mbtn = i; g_mouse_btn = i; }
                if (sel) ImGui::PopStyleColor();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        // ╔══ JITTER ════════════════════════════════════════════╗
        SectionHeader("Jitter");
        ImGui::SameLine();
        if (ImGui::Checkbox("##jit", &ui_jitter))
            g_jitter.store(ui_jitter, std::memory_order_relaxed);

        if (ui_jitter)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(FULL - ImGui::GetCursorPosX() + PAD - S(42.f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, {0.f,0.80f,0.92f,1.f});
            if (ImGui::SliderInt("##jp", &ui_jpct, 1, 50))
                g_jitter_pct.store(ui_jpct, std::memory_order_relaxed);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            char jbuf[10]; std::snprintf(jbuf, sizeof(jbuf), "+/-%d%%", ui_jpct);
            ImGui::TextColored({0.42f,0.56f,0.70f,1.f}, "%s", jbuf);
        }

        ImGui::Spacing();
        ImGui::Separator();

        // ╔══ HOTKEY ════════════════════════════════════════════╗
        SectionHeader("Hotkey");
        ImGui::SameLine();
        if (wait_key)
        {
            float blink = (float)std::fmod(t, 1.0) > 0.5f ? 1.f : 0.55f;
            ImGui::TextColored({1.f, 0.75f*blink, 0.f, 1.f}, "[ Press any key... ]");

            // Wait a few frames so the click that opened capture mode
            // has been fully released before we start scanning
            if (wait_key_frames > 0)
            {
                --wait_key_frames;
            }
            else
            {
                // Only detect a key on RISING edge (just pressed, not held)
                // by checking it wasn't down last frame
                for (int vk = 8; vk < 256; ++vk)
                {
                    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                    SHORT state = GetAsyncKeyState(vk);
                    // bit 15 = currently down, bit 0 = pressed since last call
                    if ((state & 0x8001) == 0x8001)
                    {
                        if (vk != VK_ESCAPE)
                            g_hotkey.store(vk, std::memory_order_relaxed);
                        wait_key       = false;
                        wait_key_frames = 0;
                        g_capturing.store(false, std::memory_order_relaxed);
                        g_skip_next_hotkey.store(true, std::memory_order_relaxed); // prevent this keypress from triggering a toggle
                        break;
                    }
                }
            }
        }
        else
        {
            std::string kn = "  " + VkName(g_hotkey.load()) + "  ";
            ImGui::PushStyleColor(ImGuiCol_Button,        {0.f,0.42f,0.52f,0.28f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.f,0.60f,0.72f,0.42f});
            if (ImGui::Button(kn.c_str())) { wait_key = true; wait_key_frames = 8; g_capturing.store(true, std::memory_order_relaxed); }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::TextColored({0.28f,0.36f,0.50f,1.f}, "click to change");

            // START / STOP button — right-aligned on same row
            const char* ssLbl = clicking ? " STOP " : " START ";
            float ssW = ImGui::CalcTextSize(ssLbl).x + S(20.f);
            ImGui::SameLine(WW - PAD - ssW);
            float pp = 0.50f + 0.22f*(float)std::sin(t * 2.6f);
            ImVec4 ssBg = clicking
                ? ImVec4(0.55f + pp*0.08f, 0.04f, 0.04f, 0.85f)
                : ImVec4(0.00f, 0.42f + pp*0.10f, 0.52f + pp*0.10f, 0.65f);
            ImVec4 ssHov = clicking
                ? ImVec4(0.80f, 0.10f, 0.10f, 0.95f)
                : ImVec4(0.00f, 0.65f, 0.78f, 0.85f);
            ImGui::PushStyleColor(ImGuiCol_Button,        ssBg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ssHov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.f,0.90f,1.f,1.f});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6.f));
            if (ImGui::Button(ssLbl))
                g_clicking.store(!clicking, std::memory_order_relaxed);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
        }

        // ╔══ FOOTER ════════════════════════════════════════════╗
        {
            float lineH = ImGui::GetTextLineHeight();
            float fh    = lineH + ImGui::GetStyle().FramePadding.y * 2.f + S(8.f);
            float ypos  = io.DisplaySize.y - ImGui::GetStyle().WindowPadding.y - fh;
            ImGui::SetCursorPosY(ypos);

            if (clicking)
            {
                float p = 0.5f + 0.5f*(float)std::sin(t * 2.2f);
                ImVec2 a = ImGui::GetCursorScreenPos();
                a.y += S(1.f);
                ImVec2 b = {a.x + FULL, a.y};
                ImU32 col = IM_COL32(0, (int)(175+80*p), (int)(205+50*p), (int)(110+110*p));
                ImGui::GetWindowDrawList()->AddLine(a, b, col, S(2.f));
            }

            ImGui::Separator();

            char foot[72];
            std::snprintf(foot, sizeof(foot),
                "Press  %s  to toggle   |   Target: %d CPS",
                VkName(g_hotkey.load()).c_str(), ui_cps);
            float fw = ImGui::CalcTextSize(foot).x;
            ImGui::SetCursorPosX((WW - fw) * 0.5f);
            ImGui::TextColored({0.22f, 0.30f, 0.44f, 1.f}, "%s", foot);
        }

        ImGui::End();

        // ── Render ───────────────────────────────────────────
        ImGui::Render();
        if (g_rtv)
        {
            g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_ctx->ClearRenderTargetView(g_rtv, CLR);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        HRESULT phr = g_sc->Present(1, 0);
        static bool first_frame_logged = false;
        if (!first_frame_logged) { Log("First frame presented, hr=0x%08X", (unsigned)phr); first_frame_logged = true; }
        if (phr == DXGI_ERROR_DEVICE_REMOVED || phr == DXGI_ERROR_DEVICE_RESET)
        {
            Log("Device lost: 0x%08X (reason 0x%08X)", (unsigned)phr,
                (unsigned)(g_dev ? g_dev->GetDeviceRemovedReason() : 0));
            MessageBoxW(hwnd, L"Graphics device was lost. See influclick_log.txt.", L"InfluClick", MB_ICONERROR);
            break;
        }
    }

    // ── Cleanup ──────────────────────────────────────────────
    g_running.store(false,  std::memory_order_relaxed);
    g_clicking.store(false, std::memory_order_relaxed);
    t_click.join();
    t_hotkey.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ShutdownDX11();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    timeEndPeriod(1);
    return 0;
}