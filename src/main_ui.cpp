#include "injector.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#include <wchar.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "shared_skin_state.h"
#include "skin_loader.h"    // DynamicSkin, LoadSkinsJson, BuildStaticFallback, WearGrade, GetExeDir
#include "loadout.h"        // Loadout, LoadoutEntry
#include "curator_log.h"    // CuratorLog::Init, Write, Close

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// Shared memory — IPC to SovereignHook.dll
// ---------------------------------------------------------------------------
static HANDLE              g_hLoadoutShmem  = nullptr;
static SharedLoadoutState* g_pLoadoutState  = nullptr;
static uint32_t            g_shmemVersion   = 0;

// ---------------------------------------------------------------------------
// D3D11 globals
// ---------------------------------------------------------------------------
static HWND                     g_hwnd                 = nullptr;
static ID3D11Device*            g_pd3dDevice           = nullptr;
static ID3D11DeviceContext*     g_pd3dContext          = nullptr;
static IDXGISwapChain*          g_pSwapChain           = nullptr;
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

// Skin catalogue (loaded from skins.json or static fallback)
static std::vector<DynamicSkin> g_skins;
static bool                     g_skinsFromJson = false;

// skins.json download state
//   0 = idle   1 = downloading   2 = done (reload pending)   3 = failed
static std::atomic<int> g_dlState{0};
static char             g_dlError[256]{};

// ---------------------------------------------------------------------------
// DownloadSkinsJson — runs on a background thread.
// Streams raw.githubusercontent.com to a temp file then renames it into place.
// ---------------------------------------------------------------------------
static void DownloadSkinsJson(std::wstring destPath)
{
    g_dlError[0] = '\0';
    std::wstring tempPath = destPath + L".tmp";

    HINTERNET hSess = WinHttpOpen(
        L"SkinsAreSilly/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) {
        strncpy_s(g_dlError, "WinHttpOpen failed", sizeof(g_dlError));
        g_dlState.store(3); return;
    }

    HINTERNET hConn = WinHttpConnect(
        hSess, L"raw.githubusercontent.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConn) {
        WinHttpCloseHandle(hSess);
        strncpy_s(g_dlError, "WinHttpConnect failed", sizeof(g_dlError));
        g_dlState.store(3); return;
    }

    HINTERNET hReq = WinHttpOpenRequest(
        hConn, L"GET",
        L"/ByMykel/CSGO-API/main/public/api/en/skins.json",
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hReq) {
        WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        strncpy_s(g_dlError, "WinHttpOpenRequest failed", sizeof(g_dlError));
        g_dlState.store(3); return;
    }

    bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
           && WinHttpReceiveResponse(hReq, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        strncpy_s(g_dlError, "Network request failed — check your connection", sizeof(g_dlError));
        g_dlState.store(3); return;
    }

    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        snprintf(g_dlError, sizeof(g_dlError), "HTTP %lu", statusCode);
        g_dlState.store(3); return;
    }

    FILE* f = _wfopen(tempPath.c_str(), L"wb");
    if (!f) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        strncpy_s(g_dlError, "Could not write temp file (check directory permissions)", sizeof(g_dlError));
        g_dlState.store(3); return;
    }

    char buf[8192]; DWORD n = 0; bool writeOk = true;
    while (WinHttpReadData(hReq, buf, sizeof(buf), &n) && n > 0) {
        if (fwrite(buf, 1, n, f) != n) { writeOk = false; break; }
    }
    fclose(f);
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);

    if (!writeOk) {
        _wremove(tempPath.c_str());
        strncpy_s(g_dlError, "Write error — disk full?", sizeof(g_dlError));
        g_dlState.store(3); return;
    }

    _wremove(destPath.c_str());
    if (_wrename(tempPath.c_str(), destPath.c_str()) != 0) {
        _wremove(tempPath.c_str());
        strncpy_s(g_dlError, "Could not rename temp file to skins.json", sizeof(g_dlError));
        g_dlState.store(3); return;
    }

    g_dlState.store(2); // success — main loop will reload
}

// Loadout
static Loadout       g_loadout;
static std::wstring  g_loadoutPath;

// Weapon display list — one entry per unique weapon derived from the skin catalogue
struct WeaponInfo {
    std::string    weaponId;
    std::string    weaponName;
    int            weaponDefIndex;
    WeaponCategory category;
};
static std::vector<WeaponInfo> g_weapons;

// Currently selected weapon row in the loadout table (weaponDefIndex, 0 = none)
static int g_activeWeaponDefIdx = 0;

// Search filter in the skin picker
static char g_searchBuf[128] = {};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Write the full Loadout to shared memory and bump the version counter.
static void PushLoadoutToSharedMem()
{
    if (!g_pLoadoutState) return;

    SharedLoadoutState state{};
    state.version = ++g_shmemVersion;

    int slot = 0;
    for (const auto& e : g_loadout.Entries()) {
        if (slot >= SAS_MAX_LOADOUT_ENTRIES) break;
        state.slots[slot].weaponDefIndex = e.weaponDefIndex;
        state.slots[slot].paintKitId     = e.paintKitId;
        state.slots[slot].wear           = e.wear;
        ++slot;
    }

    std::memcpy(g_pLoadoutState, &state, sizeof(SharedLoadoutState));
}

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

// Build the canonical weapon list from the skin catalogue.
// Deduplicates by weaponDefIndex; sorts by category then name.
static void BuildWeaponList()
{
    g_weapons.clear();
    for (const auto& skin : g_skins) {
        bool found = false;
        for (const auto& w : g_weapons) {
            if (w.weaponDefIndex == skin.weaponDefIndex) { found = true; break; }
        }
        if (!found) {
            WeaponInfo wi;
            wi.weaponId       = skin.weaponId;
            wi.weaponName     = skin.weaponName;
            wi.weaponDefIndex = skin.weaponDefIndex;
            wi.category       = skin.category;
            g_weapons.push_back(std::move(wi));
        }
    }
    std::sort(g_weapons.begin(), g_weapons.end(),
        [](const WeaponInfo& a, const WeaponInfo& b) {
            if (a.category != b.category) return (int)a.category < (int)b.category;
            return a.weaponName < b.weaponName;
        });
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
        820, 620,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd || !CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    CuratorLog::Init(GetExeDir());
    CuratorLog::Write("MuseumCurator starting");

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    // Shared memory for SovereignHook.dll
    CuratorLog::Write("Creating shared memory: Local\\SkinsAreSillyState");
    g_hLoadoutShmem = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(SharedLoadoutState), SKINSARESILLY_SHMEM_NAME);
    if (g_hLoadoutShmem) {
        g_pLoadoutState = static_cast<SharedLoadoutState*>(
            MapViewOfFile(g_hLoadoutShmem, FILE_MAP_WRITE, 0, 0, sizeof(SharedLoadoutState)));
        CuratorLog::Write("Shared memory created OK (handle 0x%p)", (void*)g_hLoadoutShmem);
    } else {
        CuratorLog::Write("ERROR: CreateFileMappingW failed (error %lu)", GetLastError());
    }

    // Load skin catalogue (try skins.json first, fall back to static list)
    {
        std::wstring jsonPath = GetExeDir() + L"skins.json";
        auto loaded = LoadSkinsJson(jsonPath);
        if (!loaded.empty()) {
            g_skins         = std::move(loaded);
            g_skinsFromJson = true;
            CuratorLog::Write("Loaded %d skins from skins.json", (int)g_skins.size());
        } else {
            g_skins         = BuildStaticFallback();
            g_skinsFromJson = false;
            CuratorLog::Write("skins.json not found — using static fallback (%d skins)", (int)g_skins.size());
        }
    }

    BuildWeaponList();

    // Load saved loadout and push it to shared memory immediately so any
    // already-running hook picks up the previous session's assignments.
    g_loadoutPath = GetExeDir() + L"loadout.json";
    g_loadout.Load(g_loadoutPath);
    PushLoadoutToSharedMem();
    CuratorLog::Write("Loadout loaded: %d weapon(s) assigned", (int)g_loadout.Entries().size());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& style       = ImGui::GetStyle();
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.TabRounding       = 3.0f;
    style.ItemSpacing       = {8.0f, 5.0f};
    style.Colors[ImGuiCol_Tab]         = ImVec4(0.18f, 0.18f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.28f, 0.48f, 0.72f, 1.0f);
    style.Colors[ImGuiCol_TabHovered]  = ImVec4(0.35f, 0.55f, 0.80f, 1.0f);

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

        // Reload skins catalogue after a successful download (state 2 is set by
        // the download thread; we do the actual reload on the UI thread).
        if (g_dlState.load() == 2) {
            std::wstring jsonPath = GetExeDir() + L"skins.json";
            auto loaded = LoadSkinsJson(jsonPath);
            if (!loaded.empty()) {
                g_skins         = std::move(loaded);
                g_skinsFromJson = true;
                BuildWeaponList();
                CuratorLog::Write("skins.json downloaded — reloaded %d skins", (int)g_skins.size());
            }
            g_dlState.store(0);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ----------------------------------------------------------------------
        // Disclaimer modal
        // ----------------------------------------------------------------------
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
                float btnW  = 200.0f;
                float indent = (ImGui::GetContentRegionAvail().x - btnW) * 0.5f;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
                if (ImGui::Button("I Understand \xe2\x80\x94 Proceed", {btnW, 0})) {
                    g_disclaimerAccepted = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // ----------------------------------------------------------------------
        // Main window (fills entire client area)
        // ----------------------------------------------------------------------
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
        ImGui::TextDisabled("  v0.2.0-alpha  |  GPL-3.0  |  -insecure mode only");
        ImGui::Separator();
        ImGui::Spacing();

        // ---- Demo mode ----
        ImGui::Checkbox("Demo Mode  (no injection \xe2\x80\x94 UI preview only)", &g_demoMode);
        ImGui::Spacing();

        // ---- skins.json download banner ----
        if (!g_skinsFromJson) {
            int dlState = g_dlState.load();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.14f, 0.06f, 1.0f));
            ImGui::BeginChild("##dlbanner", {-1.0f, 42.0f}, false);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
            ImGui::TextDisabled("Using built-in skin list (%d skins).  Download the full catalogue for all %d00+ skins:",
                (int)g_skins.size(), (int)g_skins.size() / 100 + 1);
            ImGui::SameLine(0, 12.0f);
            if (dlState == 1) {
                ImGui::BeginDisabled();
                ImGui::Button("Downloading skins.json...", {0.0f, 0.0f});
                ImGui::EndDisabled();
            } else {
                if (ImGui::Button(dlState == 3 ? "Retry Download" : "Download skins.json", {0.0f, 0.0f})) {
                    g_dlState.store(1);
                    std::thread(DownloadSkinsJson, GetExeDir() + L"skins.json").detach();
                    CuratorLog::Write("Downloading skins.json from ByMykel/CSGO-API");
                }
                if (dlState == 3) {
                    ImGui::SameLine(0, 8.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    ImGui::TextUnformatted(g_dlError);
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        // ====================================================================
        // Two-column layout: left = loadout table, right = skin picker
        // ====================================================================
        float totalW   = ImGui::GetContentRegionAvail().x;
        float leftW    = totalW * 0.42f;
        float rightW   = totalW - leftW - style.ItemSpacing.x;
        float bottomH  = 60.0f; // space below columns for inject button + status
        float colH     = ImGui::GetContentRegionAvail().y - bottomH;

        // ----------------------------------------------------------------
        // LEFT COLUMN — Loadout table
        // ----------------------------------------------------------------
        ImGui::BeginChild("##left", {leftW, colH}, false);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.85f, 1.0f, 1.0f));
        ImGui::TextUnformatted("LOADOUT");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // Enumerate assigned count
        int assignedCount = (int)g_loadout.Entries().size();
        if (assignedCount == 0)
            ImGui::TextDisabled("No skins assigned yet. Pick a weapon below.");
        else
            ImGui::TextDisabled("%d skin(s) assigned  \xe2\x80\x94  auto-applied in-game", assignedCount);
        ImGui::Spacing();

        ImGuiTableFlags tflags =
            ImGuiTableFlags_RowBg        |
            ImGuiTableFlags_ScrollY      |
            ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_SizingStretchProp;

        float tableH = colH - 72.0f;
        if (ImGui::BeginTable("##loadout", 3, tflags, {-1.0f, tableH})) {
            ImGui::TableSetupColumn("Weapon",       ImGuiTableColumnFlags_WidthStretch, 0.30f);
            ImGui::TableSetupColumn("Skin",         ImGuiTableColumnFlags_WidthStretch, 0.60f);
            ImGui::TableSetupColumn("##clr",        ImGuiTableColumnFlags_WidthFixed,   22.0f);
            ImGui::TableHeadersRow();

            for (const auto& wi : g_weapons) {
                const LoadoutEntry* assigned = g_loadout.Get(wi.weaponDefIndex);
                bool isActive = (g_activeWeaponDefIdx == wi.weaponDefIndex);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                // Full-row selectable
                ImGui::PushID(wi.weaponDefIndex);
                bool clicked = ImGui::Selectable(wi.weaponName.c_str(), isActive,
                    ImGuiSelectableFlags_SpanAllColumns, {0.0f, 0.0f});
                if (clicked)
                    g_activeWeaponDefIdx = isActive ? 0 : wi.weaponDefIndex;
                ImGui::PopID();

                // Skin name column
                ImGui::TableSetColumnIndex(1);
                if (assigned) {
                    ImGui::TextUnformatted(assigned->skinName.c_str());
                } else {
                    ImGui::TextDisabled("\xe2\x80\x94");
                }

                // Clear button column
                ImGui::TableSetColumnIndex(2);
                if (assigned) {
                    ImGui::PushID(wi.weaponDefIndex + 10000);
                    if (ImGui::SmallButton("X")) {
                        g_loadout.Clear(wi.weaponDefIndex);
                        g_loadout.Save(g_loadoutPath);
                        PushLoadoutToSharedMem();
                        CuratorLog::Write("Cleared skin for defIndex=%d", wi.weaponDefIndex);
                        if (g_activeWeaponDefIdx == wi.weaponDefIndex)
                            g_activeWeaponDefIdx = 0;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }

        ImGui::EndChild(); // ##left

        // ----------------------------------------------------------------
        // RIGHT COLUMN — Skin picker (filtered to active weapon)
        // ----------------------------------------------------------------
        ImGui::SameLine();
        ImGui::BeginChild("##right", {rightW, colH}, false);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.85f, 1.0f, 1.0f));
        if (g_activeWeaponDefIdx == 0) {
            ImGui::TextUnformatted("SKIN PICKER");
        } else {
            // Find weapon name for header
            const char* wname = "SKIN PICKER";
            for (const auto& wi : g_weapons)
                if (wi.weaponDefIndex == g_activeWeaponDefIdx) { wname = wi.weaponName.c_str(); break; }
            ImGui::Text("SKIN PICKER  \xe2\x80\x94  %s", wname);
        }
        ImGui::PopStyleColor();
        ImGui::Spacing();

        if (g_activeWeaponDefIdx == 0) {
            ImGui::TextDisabled("Click a weapon row on the left to pick a skin for it.");
        } else {
            // Search box
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##search", "Search...", g_searchBuf, sizeof(g_searchBuf));

            // Build filtered skin list for this weapon
            std::string searchLower = ToLower(g_searchBuf);
            std::vector<int> filtered;
            filtered.reserve(64);
            for (int i = 0; i < (int)g_skins.size(); ++i) {
                const auto& s = g_skins[i];
                if (s.weaponDefIndex != g_activeWeaponDefIdx) continue;
                if (!searchLower.empty() &&
                    ToLower(s.name).find(searchLower) == std::string::npos) continue;
                filtered.push_back(i);
            }

            // Find currently assigned skin index for this weapon
            const LoadoutEntry* curAssigned = g_loadout.Get(g_activeWeaponDefIdx);
            int curPaintKit = curAssigned ? curAssigned->paintKitId : -1;

            // Skin list — reserve space for wear slider below
            float skinListH = colH - 120.0f;
            ImGuiTableFlags sf =
                ImGuiTableFlags_RowBg        |
                ImGuiTableFlags_ScrollY      |
                ImGuiTableFlags_BordersOuter |
                ImGuiTableFlags_SizingStretchSame;

            if (ImGui::BeginTable("##skins", 3, sf, {-1.0f, skinListH})) {
                ImGui::TableSetupColumn("##c", ImGuiTableColumnFlags_WidthFixed,   12.0f);
                ImGui::TableSetupColumn("##n", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("##g", ImGuiTableColumnFlags_WidthFixed,   26.0f);

                if (filtered.empty()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextDisabled("No skins found.");
                }

                for (int idx : filtered) {
                    auto& s = g_skins[idx];
                    bool isCurrent = (s.paintKitId == curPaintKit);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);

                    ImGui::PushID(idx);
                    bool clicked = ImGui::Selectable("##sel", isCurrent,
                        ImGuiSelectableFlags_SpanAllColumns |
                        ImGuiSelectableFlags_AllowOverlap,
                        {0.0f, 0.0f});
                    if (clicked) {
                        if (!isCurrent) {
                            // Assign this skin to the active weapon
                            LoadoutEntry e;
                            e.weaponId       = s.weaponId;
                            e.weaponName     = s.weaponName;
                            e.weaponDefIndex = s.weaponDefIndex;
                            e.skinName       = s.name;
                            e.paintKitId     = s.paintKitId;
                            e.wear           = s.wear;
                            g_loadout.Set(e);
                            g_loadout.Save(g_loadoutPath);
                            PushLoadoutToSharedMem();
                            CuratorLog::Write("Assigned defIdx=%d paintKit=%d wear=%.4f",
                                e.weaponDefIndex, e.paintKitId, e.wear);
                        } else {
                            // Deselect — clear this weapon's assignment
                            g_loadout.Clear(g_activeWeaponDefIdx);
                            g_loadout.Save(g_loadoutPath);
                            PushLoadoutToSharedMem();
                            CuratorLog::Write("Deselected defIdx=%d", g_activeWeaponDefIdx);
                        }
                    }
                    ImGui::PopID();

                    // Rarity swatch
                    {
                        float lh = ImGui::GetTextLineHeight();
                        ImVec2 rMin = ImGui::GetItemRectMin();
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            {rMin.x + 1.0f, rMin.y + 2.0f},
                            {rMin.x + 9.0f, rMin.y + lh - 1.0f},
                            ImGui::ColorConvertFloat4ToU32(s.rarityColor), 1.5f);
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(s.name.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::PushStyleColor(ImGuiCol_Text, {0.50f, 0.50f, 0.50f, 1.0f});
                    ImGui::TextUnformatted(WearGrade(s.wear));
                    ImGui::PopStyleColor();
                }
                ImGui::EndTable();
            }

            // Wear slider — only when a skin is assigned for this weapon
            ImGui::Spacing();
            const LoadoutEntry* cur = g_loadout.Get(g_activeWeaponDefIdx);
            if (cur) {
                // Find the DynamicSkin for min/max wear bounds
                float minWear = 0.0f, maxWear = 1.0f;
                float wearVal = cur->wear;
                for (const auto& s : g_skins) {
                    if (s.weaponDefIndex == cur->weaponDefIndex && s.paintKitId == cur->paintKitId) {
                        minWear = s.minWear;
                        maxWear = s.maxWear;
                        break;
                    }
                }

                ImGui::SetNextItemWidth(-100.0f);
                bool changed = ImGui::SliderFloat("##wear", &wearVal, minWear, maxWear, "Wear: %.4f");
                ImGui::SameLine(0, 8.0f);
                ImGui::TextDisabled("[%s]", WearGrade(wearVal));

                if (changed) {
                    // Update the loadout entry's wear and push
                    LoadoutEntry updated = *cur;
                    updated.wear = wearVal;
                    g_loadout.Set(updated);
                    g_loadout.Save(g_loadoutPath);
                    PushLoadoutToSharedMem();
                }
            } else {
                ImGui::TextDisabled("Select a skin above to set its wear.");
            }
        }

        ImGui::EndChild(); // ##right

        // ====================================================================
        // Inject button + status bar (below the two columns)
        // ====================================================================
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool canInject = g_disclaimerAccepted && !g_demoMode;
        if (!canInject) ImGui::BeginDisabled();
        if (ImGui::Button("Inject SovereignHook.dll into CS2", {-1.0f, 28.0f})) {
            CuratorLog::Write("Inject button clicked");
            DWORD pid = FindProcessId(L"cs2.exe");
            if (pid == 0) {
                CuratorLog::Write("cs2.exe not found");
                wcscpy_s(g_statusMsg, L"CS2 not found. Launch CS2 with -insecure first.");
            } else {
                wchar_t exePath[MAX_PATH]{};
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                wchar_t* slash = wcsrchr(exePath, L'\\');
                if (slash)
                    wcscpy_s(slash + 1,
                             MAX_PATH - (slash - exePath + 1),
                             L"SovereignHook.dll");
                CuratorLog::Write("cs2.exe PID = %lu, DLL: %ls", pid, exePath);
                std::wstring err = InjectDll(pid, exePath);
                if (err.empty()) {
                    wcscpy_s(g_statusMsg, L"Injected. Skins active. Keep CS2 in -insecure mode.");
                } else {
                    wcscpy_s(g_statusMsg, (L"Injection failed: " + err).c_str());
                }
                CuratorLog::Write("InjectDll result: %ls", err.empty() ? L"OK" : err.c_str());
            }
        }
        if (!canInject) ImGui::EndDisabled();

        if (g_demoMode)
            ImGui::TextDisabled("Demo Mode active \xe2\x80\x94 injection disabled.");

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.9f, 0.6f, 1.0f));
        ImGui::TextWrapped("%ls", g_statusMsg);
        ImGui::PopStyleColor();

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

    CuratorLog::Write("Main loop exited — shutting down");
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();

    if (g_pLoadoutState) { UnmapViewOfFile(g_pLoadoutState); g_pLoadoutState = nullptr; }
    if (g_hLoadoutShmem) { CloseHandle(g_hLoadoutShmem);     g_hLoadoutShmem = nullptr; }
    CuratorLog::Write("MuseumCurator exiting cleanly");
    CuratorLog::Close();

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
