#include "injector.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>

// DWMWA_USE_IMMERSIVE_DARK_MODE was added in the Windows 11 SDK.
// MinGW may ship an older dwmapi.h that doesn't define it — guard against that.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#include <wchar.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "skin_catalogue.h"
#include "shared_skin_state.h"

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// Shared memory — IPC to SovereignHook.dll running inside cs2.exe
// ---------------------------------------------------------------------------
static HANDLE           g_hSkinShmem  = nullptr;
static SharedSkinState* g_pSkinState  = nullptr;

// ---------------------------------------------------------------------------
// D3D11 / Win32 globals
// ---------------------------------------------------------------------------
static HWND                     g_hwnd           = nullptr;
static ID3D11Device*            g_pd3dDevice     = nullptr;
static ID3D11DeviceContext*     g_pd3dContext    = nullptr;
static IDXGISwapChain*          g_pSwapChain     = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

static bool CreateDeviceD3D(HWND hwnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
static LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
static bool  g_disclaimerAccepted = false;
static bool  g_demoMode          = false;
static int   g_selectedSkin      = 0;
static wchar_t g_statusMsg[256]  = L"Ready. CS2 must be running with -insecure.";

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"MuseumCurator";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName,
        L"SkinsAreSilly — Museum Curator",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        520, 340,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd || !CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    // Enable DWM dark title bar (Windows 11)
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    // Create the shared memory region that SovereignHook.dll will read.
    g_hSkinShmem = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(SharedSkinState), SKINSARESILLY_SHMEM_NAME);
    if (g_hSkinShmem)
        g_pSkinState = static_cast<SharedSkinState*>(
            MapViewOfFile(g_hSkinShmem, FILE_MAP_WRITE, 0, 0, sizeof(SharedSkinState)));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't write imgui.ini

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding  = 3.0f;

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);

    const ImVec4 clearColor{0.10f, 0.10f, 0.10f, 1.0f};
    bool done = false;

    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ----------------------------------------------------------------
        // Mandatory disclaimer modal — blocks all other interaction
        // ----------------------------------------------------------------
        if (!g_disclaimerAccepted) {
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                    ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::SetNextWindowSize({460, 0}, ImGuiCond_Always);
            ImGui::OpenPopup("##disclaimer");

            if (ImGui::BeginPopupModal("##disclaimer", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove))
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImGui::TextWrapped("WARNING — READ BEFORE CONTINUING");
                ImGui::PopStyleColor();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextWrapped(
                    "This tool is NOT a cheat. It provides no competitive "
                    "advantage. It renders skins locally, on your own client only.\n\n"
                    "However:\n\n"
                    "  INJECTING THIS TOOL WITHOUT LAUNCHING CS2 WITH THE\n"
                    "  -insecure LAUNCH OPTION WILL RESULT IN A PERMANENT\n"
                    "  VAC BAN ON YOUR STEAM ACCOUNT.\n\n"
                    "There is no appeal process. Valve does not review VAC bans.\n\n"
                    "The developer accepts this risk on their own account as a "
                    "deliberate statement about digital scarcity. You should only "
                    "proceed if you understand and accept the same risk.");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                float btnWidth = 200.0f;
                ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - btnWidth) * 0.5f
                                     + ImGui::GetCursorPosX());
                if (ImGui::Button("I Understand — Proceed", {btnWidth, 0})) {
                    g_disclaimerAccepted = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // ----------------------------------------------------------------
        // Main window
        // ----------------------------------------------------------------
        {
            ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
            ImGui::Begin("##main", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoBringToFrontOnFocus);

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.2f, 1.0f));
            ImGui::Text("SkinsAreSilly — Museum Curator");
            ImGui::PopStyleColor();
            ImGui::TextDisabled("v0.1.0-alpha  |  GPL-3.0  |  -insecure mode only");
            ImGui::Separator();
            ImGui::Spacing();

            // Demo mode toggle
            ImGui::Checkbox("Demo Mode  (no injection — UI preview only)", &g_demoMode);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "When enabled, skin selection is shown without injecting into CS2.\n"
                    "Use this to verify the UI works before running against a live game.");
            ImGui::Spacing();

            // Skin dropdown
            ImGui::Text("Select weapon skin:");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##skinlist", k_skins[g_selectedSkin].name)) {
                for (int i = 0; i < k_skinCount; ++i) {
                    bool selected = (g_selectedSkin == i);
                    if (ImGui::Selectable(k_skins[i].name, selected)) {
                        g_selectedSkin = i;
                        if (g_pSkinState) {
                            g_pSkinState->weaponDefIndex = k_skins[i].weaponDefIndex;
                            g_pSkinState->paintKitId     = k_skins[i].paintKitId;
                            g_pSkinState->wear           = k_skins[i].wear;
                        }
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::Spacing();

            // Inject button
            bool canInject = g_disclaimerAccepted && !g_demoMode;
            if (!canInject) ImGui::BeginDisabled();
            if (ImGui::Button("Inject SovereignHook.dll into CS2", {-1, 40})) {
                DWORD pid = FindProcessId(L"cs2.exe");
                if (pid == 0) {
                    wcscpy_s(g_statusMsg, L"CS2 not found. Launch CS2 with -insecure first.");
                } else {
                    // Resolve DLL path relative to the executable
                    wchar_t exePath[MAX_PATH]{};
                    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                    // Replace the executable filename with the DLL filename
                    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
                    if (lastSlash) wcscpy_s(lastSlash + 1, MAX_PATH - (lastSlash - exePath + 1),
                                            L"SovereignHook.dll");

                    std::wstring err = InjectDll(pid, exePath);
                    if (err.empty()) {
                        wcscpy_s(g_statusMsg,
                            L"Injected. Skin active. Keep CS2 in -insecure mode.");
                    } else {
                        wcscpy_s(g_statusMsg, (L"Injection failed: " + err).c_str());
                    }
                }
            }
            if (!canInject) ImGui::EndDisabled();

            if (g_demoMode) {
                ImGui::TextDisabled("Demo Mode active — injection disabled.");
            }

            ImGui::Spacing();
            ImGui::Separator();

            // Status bar
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.9f, 0.6f, 1.0f));
            ImGui::TextWrapped("%ls", g_statusMsg);
            ImGui::PopStyleColor();

            // Donation nudge
            ImGui::Spacing();
            ImGui::TextDisabled("Find this useful? buymeacoffee.com/Gamah");

            ImGui::End();
        }

        // ----------------------------------------------------------------
        // Render
        // ----------------------------------------------------------------
        ImGui::Render();
        g_pd3dContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dContext->ClearRenderTargetView(g_mainRenderTargetView,
                                              reinterpret_cast<const float*>(&clearColor));
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();

    if (g_pSkinState) { UnmapViewOfFile(g_pSkinState); g_pSkinState = nullptr; }
    if (g_hSkinShmem) { CloseHandle(g_hSkinShmem);     g_hSkinShmem = nullptr; }

    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}

// ---------------------------------------------------------------------------
// D3D11 helpers
// ---------------------------------------------------------------------------
static bool CreateDeviceD3D(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = 0;
    sd.BufferDesc.Height                  = 0;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL featureLevelArray[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevelArray, 1, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dContext);
    if (FAILED(res)) return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)  { g_pSwapChain->Release();  g_pSwapChain  = nullptr; }
    if (g_pd3dContext) { g_pd3dContext->Release(); g_pd3dContext = nullptr; }
    if (g_pd3dDevice)  { g_pd3dDevice->Release();  g_pd3dDevice  = nullptr; }
}

static void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
                                         DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0; // suppress ALT menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
