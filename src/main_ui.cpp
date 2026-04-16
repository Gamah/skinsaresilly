#include "injector.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#include <wchar.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "shared_skin_state.h"
#include "skin_loader.h"    // DynamicSkin, LoadSkinsJson, BuildStaticFallback, WearGrade
#include "inspect_api.h"    // InspectResult, QueryInspectUrl

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// Shared memory — IPC to SovereignHook.dll
// ---------------------------------------------------------------------------
static HANDLE           g_hSkinShmem = nullptr;
static SharedSkinState* g_pSkinState = nullptr;

// ---------------------------------------------------------------------------
// D3D11 globals
// ---------------------------------------------------------------------------
static HWND                     g_hwnd                = nullptr;
static ID3D11Device*            g_pd3dDevice          = nullptr;
static ID3D11DeviceContext*     g_pd3dContext         = nullptr;
static IDXGISwapChain*          g_pSwapChain          = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

static bool CreateDeviceD3D(HWND);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
static LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
static bool    g_disclaimerAccepted = false;
static bool    g_demoMode           = false;
static wchar_t g_statusMsg[256]     = L"Ready. CS2 must be running with -insecure.";

// Skin catalogue
static std::vector<DynamicSkin> g_skins;
static bool                     g_skinsFromJson = false;
static int                      g_selectedIdx   = -1;   // -1 = nothing selected

// Filter state
static char g_searchBuf[128] = {};
static int  g_catFilter      = kCatAll;

// Async inspect-link lookup
static char                  g_inspectBuf[512] = {};
static std::string           g_inspectStatus;
static std::atomic<bool>     g_inspectPending{false};
static std::atomic<bool>     g_inspectResultReady{false};
static std::mutex            g_inspectMutex;
static InspectResult         g_inspectResult;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void WriteToSharedMem(int defIdx, int paintKit, float wear) {
    if (g_pSkinState) {
        g_pSkinState->weaponDefIndex = defIdx;
        g_pSkinState->paintKitId     = paintKit;
        g_pSkinState->wear           = wear;
    }
}

static void ClearSharedMem() {
    if (g_pSkinState) {
        g_pSkinState->weaponDefIndex = 0;
        g_pSkinState->paintKitId     = 0;
        g_pSkinState->wear           = 0.0f;
    }
}

static std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

// Truncate a float-to-string to N chars (avoids exposing all the noise digits)
static std::string FloatStr(float v, int chars = 6) {
    std::string s = std::to_string(v);
    if ((int)s.size() > chars) s = s.substr(0, chars);
    return s;
}

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
        L"SkinsAreSilly \u2014 Museum Curator",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        760, 600,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd || !CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    // Shared memory for SovereignHook.dll
    g_hSkinShmem = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(SharedSkinState), SKINSARESILLY_SHMEM_NAME);
    if (g_hSkinShmem)
        g_pSkinState = static_cast<SharedSkinState*>(
            MapViewOfFile(g_hSkinShmem, FILE_MAP_WRITE, 0, 0, sizeof(SharedSkinState)));

    // Load skin catalogue (try skins.json first, fall back to static list)
    {
        std::wstring jsonPath = GetExeDir() + L"skins.json";
        auto loaded = LoadSkinsJson(jsonPath);
        if (!loaded.empty()) {
            g_skins       = std::move(loaded);
            g_skinsFromJson = true;
        } else {
            g_skins       = BuildStaticFallback();
            g_skinsFromJson = false;
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& style           = ImGui::GetStyle();
    style.WindowRounding        = 4.0f;
    style.FrameRounding         = 3.0f;
    style.ScrollbarRounding     = 3.0f;
    style.TabRounding           = 3.0f;
    style.ItemSpacing           = {8.0f, 5.0f};
    style.Colors[ImGuiCol_Tab]              = ImVec4(0.18f, 0.18f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_TabSelected]      = ImVec4(0.28f, 0.48f, 0.72f, 1.0f);
    style.Colors[ImGuiCol_TabHovered]       = ImVec4(0.35f, 0.55f, 0.80f, 1.0f);

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

        // ------------------------------------------------------------------
        // Consume async inspect result (pre-frame, outside ImGui)
        // ------------------------------------------------------------------
        if (g_inspectResultReady.exchange(false)) {
            std::lock_guard<std::mutex> lock(g_inspectMutex);
            const InspectResult& r = g_inspectResult;
            if (r.ok) {
                // Try to find a matching skin in the catalogue and select it
                int found = -1;
                for (int i = 0; i < (int)g_skins.size(); ++i) {
                    if (g_skins[i].weaponDefIndex == r.defIndex &&
                        g_skins[i].paintKitId     == r.paintIndex) {
                        found = i; break;
                    }
                }
                float clampedWear = r.floatValue;
                if (found >= 0) {
                    auto& s = g_skins[found];
                    clampedWear    = std::clamp(r.floatValue, s.minWear, s.maxWear);
                    s.wear         = clampedWear;
                    g_selectedIdx  = found;
                    g_inspectStatus = "Applied: " + r.fullName
                        + "  [" + r.wearName + " " + FloatStr(clampedWear) + "]";
                } else {
                    // Not in the catalogue — write directly to shared mem
                    g_inspectStatus = "Applied (not in catalogue): " + r.fullName
                        + "  [" + r.wearName + " " + FloatStr(r.floatValue) + "]";
                }
                WriteToSharedMem(r.defIndex, r.paintIndex, clampedWear);
            } else {
                g_inspectStatus = "Error: " + r.error;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ------------------------------------------------------------------
        // Disclaimer modal
        // ------------------------------------------------------------------
        if (!g_disclaimerAccepted) {
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                    ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::SetNextWindowSize({480, 0}, ImGuiCond_Always);
            ImGui::OpenPopup("##disclaimer");

            if (ImGui::BeginPopupModal("##disclaimer", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove)) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImGui::TextWrapped("WARNING \xe2\x80\x94 READ BEFORE CONTINUING");
                ImGui::PopStyleColor();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextWrapped(
                    "This tool is NOT a cheat. It provides no competitive advantage. "
                    "Skins are rendered locally on your own client only.\n\n"
                    "However:\n\n"
                    "  INJECTING WITHOUT THE -insecure LAUNCH OPTION WILL\n"
                    "  RESULT IN A PERMANENT VAC BAN ON YOUR ACCOUNT.\n\n"
                    "There is no appeal process. Valve does not review VAC bans.\n\n"
                    "The developer accepts this risk deliberately as a statement "
                    "about digital scarcity. Only proceed if you understand and "
                    "accept the same risk.");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                float btnW = 200.0f;
                float indent = (ImGui::GetContentRegionAvail().x - btnW) * 0.5f;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
                if (ImGui::Button("I Understand \xe2\x80\x94 Proceed", {btnW, 0})) {
                    g_disclaimerAccepted = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // ------------------------------------------------------------------
        // Main window (fills entire client area)
        // ------------------------------------------------------------------
        ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::Begin("##main", nullptr,
            ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoResize  |
            ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ---- Header ----
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.70f, 0.20f, 1.0f));
        ImGui::Text("SkinsAreSilly \xe2\x80\x94 Museum Curator");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("  v0.1.0-alpha  |  GPL-3.0  |  -insecure mode only");
        ImGui::Separator();
        ImGui::Spacing();

        // ---- Demo mode ----
        ImGui::Checkbox("Demo Mode  (no injection \xe2\x80\x94 UI preview only)", &g_demoMode);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "When enabled, skin selection updates the preview without injecting.\n"
                "Verify the UI works before running against a live game.");
        if (!g_skinsFromJson) {
            ImGui::SameLine(0, 16.0f);
            ImGui::TextDisabled("(static catalogue \xe2\x80\x94 put skins.json beside the exe for all skins)");
        }
        ImGui::Spacing();

        // ==================================================================
        // SKIN SELECTOR
        // ==================================================================
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.85f, 1.0f, 1.0f));
        ImGui::TextUnformatted("SKIN SELECTOR");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // Category tabs — ImGui owns the selection; we mirror it into g_catFilter
        static const char* kCatLabels[kCatCount] = {
            "All", "Rifles", "Pistols", "SMGs", "Heavy", "Knives"
        };
        if (ImGui::BeginTabBar("##cats")) {
            for (int ci = 0; ci < kCatCount; ++ci) {
                if (ImGui::BeginTabItem(kCatLabels[ci])) {
                    g_catFilter = ci;
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        // Search box
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##search", "Search skins...",
                                 g_searchBuf, sizeof(g_searchBuf));

        // Build filtered index list
        std::string searchLower = ToLower(g_searchBuf);
        std::vector<int> filtered;
        filtered.reserve(g_skins.size());
        for (int i = 0; i < (int)g_skins.size(); ++i) {
            const auto& s = g_skins[i];
            if (g_catFilter != kCatAll && (int)s.category != g_catFilter) continue;
            if (!searchLower.empty()) {
                if (ToLower(s.name).find(searchLower) == std::string::npos) continue;
            }
            filtered.push_back(i);
        }

        // Skin list height: leave fixed space below for the selected-skin panel,
        // inspect section, inject button, and status bar
        const float kBelowList = 215.0f;
        float listH = std::max(80.0f,
            ImGui::GetContentRegionAvail().y - kBelowList);

        // Table-based skin list with per-row rarity colour swatch
        ImGuiTableFlags tableFlags =
            ImGuiTableFlags_RowBg        |
            ImGuiTableFlags_ScrollY      |
            ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_SizingStretchSame;

        if (ImGui::BeginTable("##skinTable", 3, tableFlags, {-1.0f, listH})) {
            ImGui::TableSetupColumn("##col_c", ImGuiTableColumnFlags_WidthFixed,   12.0f);
            ImGui::TableSetupColumn("##col_n", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##col_g", ImGuiTableColumnFlags_WidthFixed,   26.0f);

            if (filtered.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("No skins match the current filter.");
            }

            for (int idx : filtered) {
                auto& s = g_skins[idx];
                bool  isSelected = (g_selectedIdx == idx);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                // Full-row selectable in col 0, spanning all columns.
                // The rarity swatch is painted overtop via DrawList (purely visual).
                ImGui::PushID(idx);
                bool clicked = ImGui::Selectable("##sel", isSelected,
                    ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowOverlap,
                    {0.0f, 0.0f});
                if (clicked) {
                    if (!isSelected) {
                        g_selectedIdx = idx;
                        WriteToSharedMem(s.weaponDefIndex, s.paintKitId, s.wear);
                    } else {
                        g_selectedIdx = -1;
                        ClearSharedMem();
                    }
                }
                ImGui::PopID();

                // Rarity swatch — drawn over the selectable in col 0
                {
                    float lh = ImGui::GetTextLineHeight();
                    ImVec2 rMin = ImGui::GetItemRectMin();
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        {rMin.x + 1.0f, rMin.y + 2.0f},
                        {rMin.x + 9.0f, rMin.y + lh - 1.0f},
                        ImGui::ColorConvertFloat4ToU32(s.rarityColor), 1.5f);
                }

                // Col 1 — skin name
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(s.name.c_str());

                // Col 2 — wear grade
                ImGui::TableSetColumnIndex(2);
                ImGui::PushStyleColor(ImGuiCol_Text, {0.50f, 0.50f, 0.50f, 1.0f});
                ImGui::TextUnformatted(WearGrade(s.wear));
                ImGui::PopStyleColor();
            }

            ImGui::EndTable();
        }

        // ------------------------------------------------------------------
        // Selected skin panel — wear slider
        // ------------------------------------------------------------------
        ImGui::Spacing();
        if (g_selectedIdx >= 0 && g_selectedIdx < (int)g_skins.size()) {
            auto& s = g_skins[g_selectedIdx];

            ImGui::PushStyleColor(ImGuiCol_Text, s.rarityColor);
            ImGui::Text("[%s]", s.rarityName.empty() ? "?" : s.rarityName.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 6.0f);
            ImGui::TextUnformatted(s.name.c_str());

            ImGui::SetNextItemWidth(260.0f);
            bool changed = ImGui::SliderFloat("##wear", &s.wear,
                                              s.minWear, s.maxWear, "Wear: %.4f");
            ImGui::SameLine(0, 10.0f);
            ImGui::TextDisabled("[%s]  min %.2f  max %.2f",
                WearGrade(s.wear), s.minWear, s.maxWear);

            if (changed)
                WriteToSharedMem(s.weaponDefIndex, s.paintKitId, s.wear);
        } else {
            ImGui::TextDisabled("Click a skin above to select and apply it.");
            ImGui::Dummy({0, ImGui::GetTextLineHeight()});
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ==================================================================
        // INSPECT LINK IMPORT
        // ==================================================================
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.85f, 1.0f, 1.0f));
        ImGui::TextUnformatted("IMPORT FROM INSPECT LINK");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        bool pending = g_inspectPending.load();
        if (pending) ImGui::BeginDisabled();

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 72.0f);
        ImGui::InputTextWithHint("##iurl",
            "steam://rungame/730/.../+csgo_econ_action_preview ...",
            g_inspectBuf, sizeof(g_inspectBuf));
        ImGui::SameLine();

        const char* applyLabel = pending ? "Wait..." : "Apply";
        if (ImGui::Button(applyLabel, {-1.0f, 0}) && !pending) {
            std::string url = g_inspectBuf;
            if (!url.empty()) {
                g_inspectStatus  = "Querying CSGOFloat API...";
                g_inspectPending = true;
                std::thread([url]() {
                    InspectResult result = QueryInspectUrl(url);
                    {
                        std::lock_guard<std::mutex> lock(g_inspectMutex);
                        g_inspectResult = result;
                    }
                    g_inspectResultReady.store(true);
                    g_inspectPending.store(false);
                }).detach();
            }
        }

        if (pending) ImGui::EndDisabled();

        if (!g_inspectStatus.empty()) {
            bool isErr = g_inspectStatus.find("Error") != std::string::npos;
            ImVec4 col = pending  ? ImVec4(0.9f, 0.9f, 0.4f, 1.0f)
                       : isErr    ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                       :            ImVec4(0.4f, 0.9f, 0.4f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextWrapped("%s", g_inspectStatus.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ==================================================================
        // INJECT BUTTON
        // ==================================================================
        bool canInject = g_disclaimerAccepted && !g_demoMode;
        if (!canInject) ImGui::BeginDisabled();
        if (ImGui::Button("Inject SovereignHook.dll into CS2", {-1.0f, 38.0f})) {
            DWORD pid = FindProcessId(L"cs2.exe");
            if (pid == 0) {
                wcscpy_s(g_statusMsg, L"CS2 not found. Launch CS2 with -insecure first.");
            } else {
                wchar_t exePath[MAX_PATH]{};
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                wchar_t* slash = wcsrchr(exePath, L'\\');
                if (slash)
                    wcscpy_s(slash + 1,
                             MAX_PATH - (slash - exePath + 1),
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
        if (g_demoMode)
            ImGui::TextDisabled("Demo Mode active \xe2\x80\x94 injection disabled.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Status bar
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.9f, 0.6f, 1.0f));
        ImGui::TextWrapped("%ls", g_statusMsg);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextDisabled("Find this useful? buymeacoffee.com/Gamah");

        ImGui::End();

        // ------------------------------------------------------------------
        // Render
        // ------------------------------------------------------------------
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
// D3D11 helpers (unchanged from original)
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

    const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevels, 1, D3D11_SDK_VERSION,
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
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
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
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
