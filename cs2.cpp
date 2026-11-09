// language: C++, file: cs2_esp.cpp, target: Windows 11, MSVC, x64
// external ESP: ReadProcessMemory + ImGui DX11 overlay, offsets from a2x/cs2-dumper
// build: cl /std:c++20 /EHsc /O2 cs2_esp.cpp /link d3d11.lib dxgi.lib winhttp.lib
// LICENSED UNDER MIT LICENSE BY SOFT3434
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <thread>
#include <chrono>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ---------- process / memory ----------
static HANDLE g_proc = nullptr;
static DWORD  g_pid  = 0;
static uintptr_t g_client = 0;

static bool attach(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!_wcsicmp(pe.szExeFile, name)) {
                g_pid = pe.th32ProcessID;
                g_proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, g_pid);
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!g_proc) return false;

    // client.dll base via module snapshot
    HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, g_pid);
    MODULEENTRY32W me{ sizeof(me) };
    if (Module32FirstW(ms, &me)) {
        do {
            if (!_wcsicmp(me.szModule, L"client.dll")) {
                g_client = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                break;
            }
        } while (Module32NextW(ms, &me));
    }
    CloseHandle(ms);
    return g_client != 0;
}

template <typename T>
static bool rpm(uintptr_t addr, T& out) {
    SIZE_T got = 0;
    return ReadProcessMemory(g_proc, reinterpret_cast<LPCVOID>(addr), &out, sizeof(T), &got) && got == sizeof(T);
}

// ---------- offsets (a2x/cs2-dumper) ----------
struct Offsets {
    // client_dll.hpp field names as of dumper schema
    uintptr_t dwEntityList;
    uintptr_t dwLocalPlayerPawn;
    uintptr_t dwViewMatrix;
    // schema fields (client_dll.hpp::client::...)
    uintptr_t m_iHealth;
    uintptr_t m_iTeamNum;
    uintptr_t m_pGameSceneNode;
    uintptr_t m_vecOrigin;
    uintptr_t m_bDormant;
    uintptr_t m_modelState;   // CSkeletonInstance bone array lives here
    // netvars for bone: CSkeletonInstance::m_modelState + 0x80 = bone ptr
};

static Offsets g_off{};
static std::mutex g_off_mtx;

// parse a2x dumper raw .hpp: lines like:
//   constexpr std::ptrdiff_t dwEntityList = 0x1A2B3C4;
//   constexpr std::ptrdiff_t m_iHealth = 0x344;
static std::string http_get(const wchar_t* host, const wchar_t* path) {
    std::string out;
    HINTERNET s = WinHttpOpen(L"cs2esp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return out;
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (c) {
        HINTERNET r = WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD avail = 0;
            do {
                WinHttpQueryDataAvailable(r, &avail);
                if (!avail) break;
                std::string chunk(avail, '\0');
                DWORD rd = 0;
                WinHttpReadData(r, chunk.data(), avail, &rd);
                out.append(chunk.data(), rd);
            } while (avail > 0);
        }
        if (r) WinHttpCloseHandle(r);
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return out;
}

static uintptr_t find_hex(const std::string& src, const std::string& field) {
    auto p = src.find(field + " =");
    if (p == std::string::npos) return 0;
    p = src.find("0x", p);
    if (p == std::string::npos) return 0;
    return std::strtoull(src.c_str() + p, nullptr, 16);
}

static void refresh_offsets() {
    // raw endpoints from a2x/cs2-dumper repo (main branch)
    auto client = http_get(L"raw.githubusercontent.com", L"/a2x/cs2-dumper/main/output/client_dll.hpp");
    auto offs   = http_get(L"raw.githubusercontent.com", L"/a2x/cs2-dumper/main/output/offsets.hpp");
    if (client.empty() || offs.empty()) return;

    Offsets o{};
    o.dwEntityList      = find_hex(offs, "dwEntityList");
    o.dwLocalPlayerPawn = find_hex(offs, "dwLocalPlayerPawn");
    o.dwViewMatrix      = find_hex(offs, "dwViewMatrix");

    o.m_iHealth         = find_hex(client, "m_iHealth");
    o.m_iTeamNum        = find_hex(client, "m_iTeamNum");
    o.m_pGameSceneNode  = find_hex(client, "m_pGameSceneNode");
    o.m_vecOrigin       = find_hex(client, "m_vecOrigin");
    o.m_bDormant        = find_hex(client, "m_bDormant");
    o.m_modelState      = find_hex(client, "m_modelState"); // CSkeletonInstance

    std::lock_guard lk(g_off_mtx);
    g_off = o;
}

// ---------- math ----------
struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

struct ViewMatrix { float m[4][4]; };

static bool world_to_screen(const ViewMatrix& vm, const Vec3& w, Vec2& s, int W, int H) {
    float x = vm.m[0][0]*w.x + vm.m[0][1]*w.y + vm.m[0][2]*w.z + vm.m[0][3];
    float y = vm.m[1][0]*w.x + vm.m[1][1]*w.y + vm.m[1][2]*w.z + vm.m[1][3];
    float w4= vm.m[3][0]*w.x + vm.m[3][1]*w.y + vm.m[3][2]*w.z + vm.m[3][3];
    if (w4 < 0.01f) return false;
    float inv = 1.0f / w4;
    s.x = (W * 0.5f) * (1.0f + x * inv);
    s.y = (H * 0.5f) * (1.0f - y * inv);
    return s.x >= 0 && s.y >= 0 && s.x <= W && s.y <= H;
}

// bone indices for CS2 skeleton (head, neck, pelvis, knees, feet)
static constexpr int BONE_HEAD    = 6;
static constexpr int BONE_NECK    = 5;
static constexpr int BONE_PELVIS  = 0;
static constexpr int BONE_L_THIGH = 22;
static constexpr int BONE_R_THIGH = 25;
static constexpr int BONE_L_ANKLE = 24;
static constexpr int BONE_R_ANKLE = 27;

// CSkeletonInstance::m_modelState layout: bone array ptr at +0x80, bone count at +0x88
struct Bone { Vec3 pos; char pad[0x14]; };

static bool read_bone(uintptr_t scene_node, int idx, Vec3& out) {
    uintptr_t model_state = scene_node + 0x170; // CSkeletonInstance::m_modelState (schema)
    uintptr_t bone_arr = 0;
    if (!rpm(model_state + 0x80, bone_arr) || !bone_arr) return false;
    Bone b{};
    if (!rpm(bone_arr + sizeof(Bone) * idx, b)) return false;
    out = b.pos;
    return true;
}

// ---------- overlay ----------
static ID3D11Device*           g_dev  = nullptr;
static ID3D11DeviceContext*    g_ctx  = nullptr;
static IDXGISwapChain*         g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv  = nullptr;
static HWND                    g_hwnd = nullptr;
static bool                    g_run  = true;

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static bool init_d3d(HWND h) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = h;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &fl, &g_ctx) != S_OK) return false;

    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(h);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    return true;
}

static void cleanup_d3d() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv)  g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_ctx)  g_ctx->Release();
    if (g_dev)  g_dev->Release();
}

// ---------- esp frame ----------
static void draw_skeleton(ImDrawList* dl, int W, int H) {
    Offsets off;
    { std::lock_guard lk(g_off_mtx); off = g_off; }
    if (!off.dwEntityList) return;

    ViewMatrix vm{};
    if (!rpm(g_client + off.dwViewMatrix, vm)) return;

    uintptr_t local_pawn = 0;
    if (!rpm(g_client + off.dwLocalPlayerPawn, local_pawn) || !local_pawn) return;

    int local_team = 0;
    rpm(local_pawn + off.m_iTeamNum, local_team);

    uintptr_t list = 0;
    if (!rpm(g_client + off.dwEntityList, list) || !list) return;

    // entity list chunked: 0x10 * (i & 0x1FF) + list + 0x8 * (i >> 9)
    for (int i = 1; i < 64; ++i) {
        uintptr_t chunk = 0;
        if (!rpm(list + 0x8 * (i >> 9) + 0x10, chunk) || !chunk) continue;
        uintptr_t ent = 0;
        if (!rpm(chunk + 0x78 * (i & 0x1FF), ent) || !ent) continue;
        if (ent == local_pawn) continue;

        int hp = 0, team = 0;
        if (!rpm(ent + off.m_iHealth, hp) || hp <= 0 || hp > 100) continue;
        if (!rpm(ent + off.m_iTeamNum, team) || team == local_team) continue;

        bool dormant = false;
        rpm(ent + off.m_bDormant, dormant);
        if (dormant) continue;

        uintptr_t scene = 0;
        if (!rpm(ent + off.m_pGameSceneNode, scene) || !scene) continue;

        struct { int idx; int next; ImU32 col; } chain[] = {
            { BONE_HEAD,    BONE_NECK,    IM_COL32(0,255,128,220) },
            { BONE_NECK,    BONE_PELVIS,  IM_COL32(0,255,128,220) },
            { BONE_PELVIS,  BONE_L_THIGH, IM_COL32(0,255,128,220) },
            { BONE_L_THIGH, BONE_L_ANKLE, IM_COL32(0,255,128,220) },
            { BONE_PELVIS,  BONE_R_THIGH, IM_COL32(0,255,128,220) },
            { BONE_R_THIGH, BONE_R_ANKLE, IM_COL32(0,255,128,220) },
        };

        for (auto& c : chain) {
            Vec3 a{}, b{};
            if (!read_bone(scene, c.idx, a) || !read_bone(scene, c.next, b)) continue;
            Vec2 sa{}, sb{};
            if (!world_to_screen(vm, a, sa, W, H)) continue;
            if (!world_to_screen(vm, b, sb, W, H)) continue;
            dl->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), c.col, 1.5f);
        }

        // head dot
        Vec3 head{};
        if (read_bone(scene, BONE_HEAD, head)) {
            Vec2 sh{};
            if (world_to_screen(vm, head, sh, W, H))
                dl->AddCircleFilled(ImVec2(sh.x, sh.y), 3.0f, IM_COL32(255,64,64,220));
        }
    }
}

// ---------- main ----------
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    if (!attach(L"cs2.exe")) {
        MessageBoxW(nullptr, L"cs2.exe not running", L"cs2_esp", MB_ICONERROR);
        return 1;
    }

    // fetch offsets, then refresh every 10 min
    refresh_offsets();
    std::thread([] {
        while (g_run) {
            std::this_thread::sleep_for(std::chrono::minutes(10));
            refresh_offsets();
        }
    }).detach();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hi;
    wc.lpszClassName = L"cs2esp_overlay";
    RegisterClassExW(&wc);

    // client-area matches primary monitor, transparent click-through overlay
    int SW = GetSystemMetrics(SM_CXSCREEN);
    int SH = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"cs2_esp", WS_POPUP, 0, 0, SW, SH, nullptr, nullptr, hi, nullptr);
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    ShowWindow(g_hwnd, SW_SHOW);

    if (!init_d3d(g_hwnd)) return 1;

    MSG msg{};
    while (g_run) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_run = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // full-screen transparent window with skeleton list
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)SW, (float)SH));
        ImGui::Begin("##esp", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
        draw_skeleton(ImGui::GetWindowDrawList(), SW, SH);
        ImGui::End();

        ImGui::Render();
        const float clear[4] = { 0, 0, 0, 0 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    cleanup_d3d();
    return 0;
}
