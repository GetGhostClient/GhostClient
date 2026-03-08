#include "gui.h"
#include "imgui.h"
#include "icons.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <Windows.h>
#include <ShlObj.h>
#include <dwmapi.h>
#include <commdlg.h>

namespace fs = std::filesystem;

// Toggle button with smooth slide animation. Drop-in for Checkbox; returns true when toggled.
static bool ToggleButton(const char* label, bool* v) {
    ImGui::PushID(label);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    const float w = h * 2.2f;
    const ImVec2 toggle_sz(w, h);

    ImGui::InvisibleButton("t", toggle_sz);
    bool pressed = ImGui::IsItemClicked();
    if (pressed) *v = !*v;

    // Animate knob position (0 = off, 1 = on)
    const float target = *v ? 1.0f : 0.0f;
    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGuiID id = ImGui::GetID("t");
    float* pAnim = storage->GetFloatRef(id, target);
    const float speed = 10.0f;
    float dt = ImGui::GetIO().DeltaTime;
    float step = (speed * dt < 1.0f) ? (speed * dt) : 1.0f;
    *pAnim += (target - *pAnim) * step;
    if (*pAnim < 0.001f) *pAnim = 0.0f;
    if (*pAnim > 0.999f) *pAnim = 1.0f;
    const float t = *pAnim;

    const ImVec2 track_min = ImGui::GetItemRectMin();
    const ImVec2 track_max = ImGui::GetItemRectMax();
    const float rounding = toggle_sz.y * 0.5f;
    const float pad = 2.0f;
    const float knob_radius = (toggle_sz.y - pad * 2.0f) * 0.5f;
    const float knob_min_x = track_min.x + pad + knob_radius;
    const float knob_max_x = track_max.x - pad - knob_radius;
    const float knob_x = knob_min_x + (knob_max_x - knob_min_x) * t;
    const float knob_y = (track_min.y + track_max.y) * 0.5f;

    bool hovered = ImGui::IsItemHovered();
    ImU32 track_col = ImGui::GetColorU32(
        (hovered && ImGui::IsItemActive()) ? ImGuiCol_FrameBgActive
        : hovered ? ImGuiCol_FrameBgHovered
        : ImGuiCol_FrameBg);
    ImU32 knob_col = ImGui::GetColorU32(ImGuiCol_CheckMark);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(track_min, track_max, track_col, rounding);
    draw_list->AddCircleFilled(ImVec2(knob_x, knob_y), knob_radius, knob_col);

    if (label[0] != '#' || label[1] != '#') {
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        ImGui::TextUnformatted(label);
    }
    ImGui::PopID();
    return pressed;
}

std::string GetAppDataDir() {
    char* localAppData = nullptr;
    size_t len = 0;
    _dupenv_s(&localAppData, &len, "LOCALAPPDATA");
    std::string base;
    if (localAppData) {
        base = localAppData;
        free(localAppData);
    } else {
        base = ".";
    }
    std::string dir = base + "\\GhostClient";
    fs::create_directories(dir);
    return dir;
}

GhostGUI::GhostGUI(ProcessManager& proc, FFlagManager& flags)
    : m_proc(proc), m_flags(flags), m_lastFrame(std::chrono::steady_clock::now())
{
    m_dataDir    = GetAppDataDir();
    m_configPath = m_dataDir + "\\ghostclient_config.json";
    m_logPath    = m_dataDir + "\\ghostclient_log.txt";
    m_presetsDir = m_dataDir + "\\presets";
    fs::create_directories(m_presetsDir);

    auto logCb = [this](const std::string& msg) { AddLog(msg); };
    m_proc.SetLogCallback(logCb);
    m_flags.SetLogCallback(logCb);

    if (m_logToFile)
        std::ofstream(m_logPath, std::ios::trunc).close();

    LoadConfig();
    AddLog("Ghost Client started");
    AddLog("Data dir: " + m_dataDir);

    size_t flagCount = m_flags.GetTotalCount();
    AddLog("Loaded " + std::to_string(flagCount) + " FFlag offsets");
    // Offsets, types, and value cache are all loaded synchronously before
    // the window is shown (FFlagManager constructor + main.cpp ordering).
    // Nothing async needed here at startup.

    if (m_autoAttach) {
        AddLog("Auto-attach enabled - watching for RobloxPlayerBeta.exe...");
        StartWatcherThread();
    }
}

// ── Background fetch thread ──────────────────────────────────────────────────

struct FetchThreadParam {
    FFlagManager*      flags;
    std::atomic<bool>* success;
    std::atomic<bool>* done;
    std::atomic<bool>* running;
};

static DWORD WINAPI FetchOffsetsThread(LPVOID param) {
    auto* p = reinterpret_cast<FetchThreadParam*>(param);
    bool ok = p->flags->FetchAndUpdateOffsets();
    if (ok) {
        // Re-stamp types and restore cached values after fresh offset fetch
        p->flags->FetchAndApplyFVariables();
        p->flags->LoadValueCache();
    }
    p->success->store(ok);
    p->done->store(true);
    p->running->store(false);
    delete p;
    return 0;
}

void GhostGUI::StartAutoFetchOffsets() {
    if (m_fetchingOffsets.load()) return;
    m_fetchingOffsets    = true;
    m_fetchOffsetsDone   = false;
    m_fetchOffsetsSuccess = false;
    m_statusMsg          = "Downloading offsets...";

    if (m_fetchThread) { CloseHandle(m_fetchThread); m_fetchThread = nullptr; }

    auto* p = new FetchThreadParam{
        &m_flags, &m_fetchOffsetsSuccess, &m_fetchOffsetsDone, &m_fetchingOffsets
    };
    m_fetchThread = CreateThread(nullptr, 0, FetchOffsetsThread, p, 0, nullptr);
}

static DWORD WINAPI FetchFVariablesThread(LPVOID param) {
    auto* flags = reinterpret_cast<FFlagManager*>(param);
    flags->FetchAndApplyFVariables();
    return 0;
}

void GhostGUI::StartFetchFVariables() {
    if (m_fvarsThread) { CloseHandle(m_fvarsThread); m_fvarsThread = nullptr; }
    m_fvarsThread = CreateThread(nullptr, 0, FetchFVariablesThread, &m_flags, 0, nullptr);
}

// ── Process watcher thread ────────────────────────────────────────────────────
// Polls every 100ms for RobloxPlayerBeta.exe and sets m_robloxDetected.
// Restarts after attach so it catches the next launch too.

struct WatcherParam {
    std::atomic<bool>* detected;
    std::atomic<bool>* running;
};

static DWORD WINAPI ProcessWatcherThread(LPVOID param) {
    auto* p = reinterpret_cast<WatcherParam*>(param);

    while (p->running->load()) {
        // Snapshot all processes and look for Roblox
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{ sizeof(pe) };
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (std::wstring(pe.szExeFile) == L"RobloxPlayerBeta.exe") {
                        p->detected->store(true);
                        CloseHandle(snap);
                        delete p;
                        return 0; // signal sent — thread exits, will be restarted on next detach
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        Sleep(100);
    }

    delete p;
    return 0;
}

void GhostGUI::StartWatcherThread() {
    StopWatcherThread();
    m_watcherRunning = true;
    m_robloxDetected = false;
    auto* p = new WatcherParam{ &m_robloxDetected, &m_watcherRunning };
    m_watcherThread = CreateThread(nullptr, 0, ProcessWatcherThread, p, 0, nullptr);
}

void GhostGUI::StopWatcherThread() {
    m_watcherRunning = false;
    if (m_watcherThread) {
        WaitForSingleObject(m_watcherThread, 500);
        CloseHandle(m_watcherThread);
        m_watcherThread = nullptr;
    }
}

// ── Logging ──────────────────────────────────────────────────────────────────

void GhostGUI::AddLog(const std::string& msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    std::string line = std::string(timeBuf) + msg;
    m_logs.push_back(line);
    if (m_logs.size() > MAX_LOGS) m_logs.pop_front();

    if (m_logToFile && !m_logPath.empty()) {
        std::ofstream logFile(m_logPath, std::ios::app);
        if (logFile.is_open()) logFile << line << "\n";
    }
}

// ── Theme ────────────────────────────────────────────────────────────────────

void GhostGUI::ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c     = s.Colors;

    const ImVec4 bg0    = { 0.05f, 0.05f, 0.05f, 1.00f };
    const ImVec4 bg1    = { 0.09f, 0.09f, 0.09f, 1.00f };
    const ImVec4 bg2    = { 0.13f, 0.13f, 0.13f, 1.00f };
    const ImVec4 bg3    = { 0.19f, 0.19f, 0.19f, 1.00f };
    const ImVec4 bg4    = { 0.27f, 0.27f, 0.27f, 1.00f };
    const ImVec4 bdr    = { 0.18f, 0.18f, 0.18f, 1.00f };
    const ImVec4 txt    = { 0.93f, 0.93f, 0.93f, 1.00f };
    const ImVec4 dim    = { 0.38f, 0.38f, 0.38f, 1.00f };
    const ImVec4 white  = { 1.00f, 1.00f, 1.00f, 1.00f };
    const ImVec4 none   = { 0.00f, 0.00f, 0.00f, 0.00f };

    c[ImGuiCol_Text]                  = txt;
    c[ImGuiCol_TextDisabled]          = dim;
    c[ImGuiCol_WindowBg]              = bg0;
    c[ImGuiCol_ChildBg]               = bg0;
    c[ImGuiCol_PopupBg]               = bg1;
    c[ImGuiCol_Border]                = bdr;
    c[ImGuiCol_BorderShadow]          = none;
    c[ImGuiCol_FrameBg]               = bg2;
    c[ImGuiCol_FrameBgHovered]        = bg3;
    c[ImGuiCol_FrameBgActive]         = bg4;
    c[ImGuiCol_TitleBg]               = bg1;
    c[ImGuiCol_TitleBgActive]         = bg1;
    c[ImGuiCol_TitleBgCollapsed]      = bg0;
    c[ImGuiCol_MenuBarBg]             = bg1;
    c[ImGuiCol_ScrollbarBg]           = bg0;
    c[ImGuiCol_ScrollbarGrab]         = bg3;
    c[ImGuiCol_ScrollbarGrabHovered]  = bg4;
    c[ImGuiCol_ScrollbarGrabActive]   = white;
    c[ImGuiCol_CheckMark]             = white;
    c[ImGuiCol_SliderGrab]            = bg4;
    c[ImGuiCol_SliderGrabActive]      = white;
    c[ImGuiCol_Button]                = bg2;
    c[ImGuiCol_ButtonHovered]         = bg3;
    c[ImGuiCol_ButtonActive]          = bg4;
    c[ImGuiCol_Header]                = bg2;
    c[ImGuiCol_HeaderHovered]         = bg3;
    c[ImGuiCol_HeaderActive]          = bg4;
    c[ImGuiCol_Separator]             = bdr;
    c[ImGuiCol_SeparatorHovered]      = bg3;
    c[ImGuiCol_SeparatorActive]       = white;
    c[ImGuiCol_ResizeGrip]            = none;
    c[ImGuiCol_ResizeGripHovered]     = bg3;
    c[ImGuiCol_ResizeGripActive]      = white;
    c[ImGuiCol_Tab]                   = bg1;
    c[ImGuiCol_TabHovered]            = bg3;
    c[ImGuiCol_TabSelected]           = bg2;
    c[ImGuiCol_TabDimmed]             = bg1;
    c[ImGuiCol_TabDimmedSelected]     = bg2;
    c[ImGuiCol_TableHeaderBg]         = bg1;
    c[ImGuiCol_TableBorderStrong]     = bdr;
    c[ImGuiCol_TableBorderLight]      = bg2;
    c[ImGuiCol_TableRowBg]            = none;
    c[ImGuiCol_TableRowBgAlt]         = { 0.07f, 0.07f, 0.07f, 1.00f };
    c[ImGuiCol_TextSelectedBg]        = { 1.00f, 1.00f, 1.00f, 0.16f };
    c[ImGuiCol_NavHighlight]          = white;

    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 3.0f;
    s.FrameRounding     = 3.0f;
    s.GrabRounding      = 3.0f;
    s.TabRounding       = 3.0f;
    s.PopupRounding     = 4.0f;
    s.ScrollbarRounding = 3.0f;
    s.WindowPadding     = { 0, 0 };
    s.FramePadding      = { 8, 4 };
    s.ItemSpacing       = { 8, 5 };
    s.ItemInnerSpacing  = { 6, 4 };
    s.ScrollbarSize     = 9.0f;
    s.GrabMinSize       = 7.0f;
    s.WindowBorderSize  = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.TabBorderSize     = 0.0f;
    s.WindowMinSize     = { 400, 300 };
}

// ── Main render ──────────────────────────────────────────────────────────────

void GhostGUI::Render() {
    auto  now = std::chrono::steady_clock::now();
    float dt  = std::chrono::duration<float>(now - m_lastFrame).count();
    m_lastFrame = now;

    // Background offset fetch completion
    if (m_fetchOffsetsDone.load()) {
        m_fetchOffsetsDone = false;
        if (m_fetchOffsetsSuccess.load()) {
            m_statusMsg = "Offsets downloaded: " + std::to_string(m_flags.GetTotalCount()) + " flags";
            AddLog("[OFFSETS] Auto-fetch complete: " + std::to_string(m_flags.GetTotalCount()) + " flags loaded");
            // FVariables + value cache are re-applied inside FetchOffsetsThread.
            m_needsRefresh = true;
        } else {
            m_statusMsg = "Failed to download offsets - check connection";
            AddLog("[OFFSETS] Auto-fetch failed - use Settings > Update Offsets to retry");
        }
    }

    m_attached = m_proc.IsAttached();

    if (!m_hwnd)
        m_hwnd = FindWindowW(L"GhostClientClass", nullptr);

    // Auto-attach: watcher thread signals m_robloxDetected, we attach on the render thread
    if (m_autoAttach && !m_attached && m_robloxDetected.load()) {
        m_robloxDetected = false;
        if (m_proc.Attach(L"RobloxPlayerBeta.exe")) {
            m_flags.CheckVersion();
            m_attached = true;
            m_offsetUpdateTriggered = false;
            m_statusMsg = "Auto-attached (PID: " + std::to_string(m_proc.GetProcessId()) + ")";
            AddLog("[AUTO] Attached to Roblox (PID: " + std::to_string(m_proc.GetProcessId()) + ")");
            m_needsRefresh = true;

            if (m_autoInject && m_injectBuf[0] != '\0') {
                m_pendingInject    = true;
                m_injectDelayTimer = 0.0f;
                m_injectAttempts   = 0;
                AddLog("[AUTO] Waiting for FFlags to initialize before injecting...");
            }
        }
    }

    // Track detach
    if (m_wasAttached && !m_attached) {
        AddLog("[AUTO] Roblox process lost");
        m_pendingInject = false;
        m_injectAttempts = 0;
        m_offsetUpdateTriggered = false;
        m_statusMsg = "Roblox disconnected";
        // Persist flag values so they show on next startup / reconnect
        m_flags.SaveValueCache();
        // Restart watcher so we catch the next Roblox launch
        if (m_autoAttach) StartWatcherThread();
    }
    m_wasAttached = m_attached;

    // Auto-update offsets on version mismatch
    if (m_autoUpdateOffsets && m_attached && !m_fetchingOffsets.load()
        && !m_offsetUpdateTriggered && !m_flags.IsVersionMatch()
        && m_flags.GetTotalCount() > 0)
    {
        m_offsetUpdateTriggered = true;
        AddLog("[OFFSETS] Version mismatch detected - fetching latest offsets...");
        StartAutoFetchOffsets();
    }

    // Auto-read visible flag values
    if (m_autoRead && m_attached && !m_filteredFlags.empty()) {
        m_autoReadTimer += dt;
        if (m_autoReadTimer >= m_autoReadInterval) {
            m_autoReadTimer = 0.0f;
            for (auto* e : m_filteredFlags) m_flags.ReadFlagValue(*e);
            m_valueCacheDirty = true;
        }
    }

    // Flush value cache to disk periodically (every ~10s) when dirty
    if (m_valueCacheDirty) {
        m_valueCacheTimer += dt;
        if (m_valueCacheTimer >= 10.0f) {
            m_valueCacheTimer = 0.0f;
            m_valueCacheDirty = false;
            m_flags.SaveValueCache();
        }
    }

    // Delayed injection
    if (m_pendingInject && m_attached) {
        m_injectDelayTimer += dt;
        if (m_injectDelayTimer >= INJECT_RETRY_INTERVAL) {
            m_injectDelayTimer = 0.0f;
            m_injectAttempts++;
            bool ready = m_flags.IsFlagInitialized("DebugDisplayFPS")
                      || m_flags.IsFlagInitialized("TaskSchedulerTargetFps")
                      || m_injectAttempts >= 5;
            if (ready) {
                AddLog("[AUTO] Injecting flags (attempt " + std::to_string(m_injectAttempts) + ")...");
                int ok = InjectFlags(m_injectBuf, true);
                if (ok > 0 || m_injectAttempts >= MAX_INJECT_ATTEMPTS) {
                    m_pendingInject = false;
                    if (ok > 0) m_statusMsg = "Auto-injected " + std::to_string(ok) + " flags";
                    else AddLog("[AUTO] Giving up after " + std::to_string(m_injectAttempts) + " attempts");
                }
            } else {
                AddLog("[AUTO] Flags not ready yet, waiting... (attempt " + std::to_string(m_injectAttempts) + ")");
            }
        }
    }

    // ── Full-screen root window ───────────────────────────────────────────────
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    RenderTitleBar();

    // Below title bar: sidebar + content
    const float TITLE_H  = 40.0f;
    const float SIDEBAR_W = 140.0f;
    float contentH = ImGui::GetWindowHeight() - TITLE_H;

    ImGui::SetCursorPos(ImVec2(0, TITLE_H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##body", ImVec2(0, contentH), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    // Sidebar
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 8));
    ImGui::BeginChild("##sidebar", ImVec2(SIDEBAR_W, 0), false,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    RenderSidebar();
    ImGui::EndChild();

    // Vertical divider
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 divTop = ImGui::GetCursorScreenPos();
    // GetCursorScreenPos after EndChild points to right after the sidebar
    // We need to draw it manually using absolute coords
    {
        ImVec2 wpos = ImGui::GetWindowPos();
        float divX  = wpos.x + SIDEBAR_W;
        float divY0 = wpos.y;
        float divY1 = wpos.y + contentH;
        dl->AddLine(ImVec2(divX, divY0), ImVec2(divX, divY1), IM_COL32(30, 30, 30, 255));
    }

    // Content pane
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
    ImGui::BeginChild("##content", ImVec2(0, 0), false);
    ImGui::PopStyleVar();

    switch (m_activePage) {
        case Page::Browser:  RenderPageBrowser();  break;
        case Page::Injector: RenderPageInjector(); break;
        case Page::Presets:  RenderPagePresets();  break;
        case Page::Logs:     RenderPageLogs();     break;
        case Page::Settings: RenderPageSettings(); break;
    }

    ImGui::EndChild(); // ##content

    // ── Copy flash toast ─────────────────────────────────────────────────────
    if (m_copyFlashTimer > 0.0f) {
        m_copyFlashTimer -= dt;
        if (m_copyFlashTimer < 0.0f) m_copyFlashTimer = 0.0f;

        float alpha = (m_copyFlashTimer > 0.3f) ? 1.0f
                    : (m_copyFlashTimer / 0.3f); // fade out last 0.3s

        ImGuiViewport* tvp = ImGui::GetMainViewport();
        // Build toast text
        char toast[320];
        std::string lbl = m_copyFlashLabel;
        if (lbl.size() > 32) lbl = lbl.substr(0, 30) + "..";
        snprintf(toast, sizeof(toast), "Copied  %s", lbl.c_str());

        ImVec2 textSz = ImGui::CalcTextSize(toast);
        float padX = 12.0f, padY = 6.0f;
        float toastW = textSz.x + padX * 2;
        float toastH = textSz.y + padY * 2;
        float margin = 12.0f;
        ImVec2 tpos = {
            tvp->WorkPos.x + tvp->WorkSize.x - toastW - margin,
            tvp->WorkPos.y + tvp->WorkSize.y - toastH - margin
        };

        ImDrawList* tdl = ImGui::GetForegroundDrawList();
        tdl->AddRectFilled(tpos,
            { tpos.x + toastW, tpos.y + toastH },
            IM_COL32(30, 30, 30, (int)(220 * alpha)), 4.0f);
        tdl->AddRect(tpos,
            { tpos.x + toastW, tpos.y + toastH },
            IM_COL32(60, 60, 60, (int)(220 * alpha)), 4.0f);
        tdl->AddText(
            { tpos.x + padX, tpos.y + padY },
            IM_COL32(200, 240, 200, (int)(255 * alpha)),
            toast);
    }

    ImGui::EndChild(); // ##body
    ImGui::End();      // ##root
}

// ── Title bar ────────────────────────────────────────────────────────────────

void GhostGUI::RenderTitleBar() {
    const float TITLE_H = 40.0f;
    float winW = ImGui::GetWindowWidth();

    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImVec2 wpos      = ImGui::GetWindowPos();
    ImVec2 titleMin  = wpos;
    ImVec2 titleMax  = { wpos.x + winW, wpos.y + TITLE_H };
    ImVec2 mp        = ImGui::GetIO().MousePos;

    dl->AddRectFilled(titleMin, titleMax, IM_COL32(10, 10, 10, 255));
    dl->AddLine({ titleMin.x, titleMax.y }, titleMax, IM_COL32(28, 28, 28, 255));

    // App name
    const char* appName = "Ghost Client";
    ImVec2 nt = ImGui::CalcTextSize(appName);
    dl->AddText({ wpos.x + 14.0f, wpos.y + (TITLE_H - nt.y) * 0.5f },
        IM_COL32(230, 230, 230, 255), appName);

    // Status pill
    {
        const char* lbl = m_attached ? "ATTACHED" : m_autoAttach ? "WAITING" : "OFFLINE";
        ImU32 pillBg    = m_attached ? IM_COL32(65, 65, 65, 220)
                        : m_autoAttach ? IM_COL32(40, 40, 40, 220)
                                       : IM_COL32(25, 25, 25, 220);
        ImVec2 ps = ImGui::CalcTextSize(lbl);
        float px  = wpos.x + 14.0f + nt.x + 10.0f;
        float py  = wpos.y + (TITLE_H - ps.y - 6.0f) * 0.5f;
        dl->AddRectFilled({ px - 6, py }, { px + ps.x + 6, py + ps.y + 6 }, pillBg, 4.0f);
        dl->AddText({ px, py + 3.0f }, IM_COL32(200, 200, 200, 220), lbl);
    }

    // Window control buttons (_, [], x)
    const float BTN_W = 40.0f;
    float btnX = winW - BTN_W * 3.0f;

    // Drag region
    ImGui::SetCursorPos({ 0, 0 });
    ImGui::InvisibleButton("##drag", { btnX, TITLE_H });
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 2.0f)) {
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().MouseDown[0] = false;
        ImGui::GetIO().MouseDownDuration[0] = -1.0f;
        ReleaseCapture();
        SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }

    auto WinBtn = [&](const char* id, const char* lbl, ImU32 hoverCol) -> bool {
        ImVec2 bPos = ImGui::GetCursorScreenPos();
        bool hov = mp.x >= bPos.x && mp.x < bPos.x + BTN_W
                && mp.y >= bPos.y && mp.y < bPos.y + TITLE_H;
        if (hov) dl->AddRectFilled(bPos, { bPos.x + BTN_W, bPos.y + TITLE_H }, hoverCol);
        ImVec2 tc = ImGui::CalcTextSize(lbl);
        dl->AddText({ bPos.x + (BTN_W - tc.x) * 0.5f, bPos.y + (TITLE_H - tc.y) * 0.5f },
            IM_COL32(200, 200, 200, 255), lbl);
        ImGui::InvisibleButton(id, { BTN_W, TITLE_H });
        bool clicked = ImGui::IsItemClicked();
        ImGui::SameLine(0, 0);
        return clicked;
    };

    bool maximized = IsZoomed(m_hwnd);
    ImGui::SetCursorPos({ btnX, 0 });
    if (WinBtn("##min",   "_",                     IM_COL32(50, 50, 50, 220))) ShowWindow(m_hwnd, SW_MINIMIZE);
    if (WinBtn("##max",   maximized ? "[]" : "[ ]", IM_COL32(50, 50, 50, 220))) ShowWindow(m_hwnd, maximized ? SW_RESTORE : SW_MAXIMIZE);
    if (WinBtn("##close", "x",                     IM_COL32(180, 38, 38, 230))) PostQuitMessage(0);
}

// ── Sidebar ──────────────────────────────────────────────────────────────────

void GhostGUI::RenderSidebar() {
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2 wpos     = ImGui::GetWindowPos();
    float  w        = ImGui::GetWindowWidth();
    float  h        = ImGui::GetWindowHeight();

    // Sidebar background
    dl->AddRectFilled(wpos, { wpos.x + w, wpos.y + h }, IM_COL32(8, 8, 8, 255));

    struct NavItem { const char* icon; const char* label; Page page; };
    static const NavItem items[] = {
        { ICON_FA_LIST,     "Browser",  Page::Browser  },
        { ICON_FA_SYRINGE,  "Injector", Page::Injector },
        { ICON_FA_BOOKMARK, "Presets",  Page::Presets  },
        { ICON_FA_TERMINAL, "Logs",     Page::Logs     },
        { ICON_FA_GEAR,     "Settings", Page::Settings },
    };

    const float ROW_H    = 38.0f;
    const float ICON_X   = 16.0f;  // icon left margin
    const float LABEL_X  = 38.0f;  // label left margin (after icon)
    const float ACCENT_W = 3.0f;

    ImGui::SetCursorPos({ 0, 8 });

    for (auto& item : items) {
        bool active  = (m_activePage == item.page);
        ImVec2 rowPos = ImGui::GetCursorScreenPos();

        // Background highlight for active / hovered
        ImGui::InvisibleButton(item.label, { w, ROW_H });
        bool hovered = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked();
        if (clicked) m_activePage = item.page;

        ImU32 bgCol = active  ? IM_COL32(22, 22, 22, 255)
                    : hovered ? IM_COL32(16, 16, 16, 255)
                    :           IM_COL32(0,  0,  0,  0);
        if (bgCol >> 24)
            dl->AddRectFilled(rowPos, { rowPos.x + w, rowPos.y + ROW_H }, bgCol);

        // Left accent bar for active item
        if (active)
            dl->AddRectFilled(rowPos, { rowPos.x + ACCENT_W, rowPos.y + ROW_H },
                IM_COL32(220, 220, 220, 255));

        // Text colour
        ImU32 textCol = active  ? IM_COL32(240, 240, 240, 255)
                      : hovered ? IM_COL32(180, 180, 180, 255)
                      :           IM_COL32(110, 110, 110, 255);

        float textY = rowPos.y + (ROW_H - ImGui::GetTextLineHeight()) * 0.5f;

        // Icon (FA glyph)
        dl->AddText({ rowPos.x + ICON_X, textY }, textCol, item.icon);
        // Label
        dl->AddText({ rowPos.x + LABEL_X, textY }, textCol, item.label);
    }
}

// ── Status bar ───────────────────────────────────────────────────────────────

void GhostGUI::RenderStatusBar() {
    ImGui::Spacing();
    ImGui::Separator();

    if (!m_flags.IsVersionMatch() && !m_flags.GetVersionMismatchMsg().empty())
        ImGui::TextDisabled("%s", m_flags.GetVersionMismatchMsg().c_str());

    std::string ver     = m_flags.GetExpectedVersion();
    const char* verStr  = m_fetchingOffsets.load()          ? "fetching..."
                        : (ver.empty() || ver == "unknown") ? "no offsets"
                                                            : ver.c_str();
    ImGui::TextDisabled("FFlags: %zu  |  Version: %s  |  PID: %lu",
        m_flags.GetTotalCount(), verStr,
        m_attached ? (unsigned long)m_proc.GetProcessId() : 0UL);

    if (!m_statusMsg.empty()) {
        float msgW = ImGui::CalcTextSize(m_statusMsg.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - msgW - 20);
        ImGui::TextDisabled("%s", m_statusMsg.c_str());
    }
}

// ── FFlag Browser page ───────────────────────────────────────────────────────

static const char* FlagTypeTag(FlagType t) {
    switch (t) {
        case FlagType::Bool:    return "bool";
        case FlagType::Int:     return "int";
        case FlagType::String:  return "str";
        default:                return "?";
    }
}

// Fallback: infer type from read-back value when FVariables hasn't loaded yet.
static FlagType InferTypeFromValue(const std::string& val) {
    if (val == "0" || val == "1" || val == "true" || val == "false")
        return FlagType::Bool;
    bool hasDot = false, allNum = true;
    for (size_t i = 0; i < val.size(); ++i) {
        char c = val[i];
        if (c == '.') { hasDot = true; continue; }
        if (c == '-' && i == 0) continue;
        if (!std::isdigit((unsigned char)c)) { allNum = false; break; }
    }
    if (!val.empty() && allNum) return FlagType::Int;
    return FlagType::Unknown;
}

static FlagType EffectiveType(const FFlagEntry& entry) {
    if (entry.type != FlagType::Unknown) return entry.type;
    if (entry.readSuccess && !entry.currentValue.empty())
        return InferTypeFromValue(entry.currentValue);
    return FlagType::Unknown;
}

void GhostGUI::RenderPageBrowser() {
    // ── Search bar ────────────────────────────────────────────────────────────
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##Search", "Search FFlags...", m_searchBuf, sizeof(m_searchBuf)))
        m_needsRefresh = true;

    ImGui::Spacing();

    // ── Type filter pill buttons ──────────────────────────────────────────────
    struct FilterBtn { const char* label; FlagType type; };
    static constexpr FilterBtn kFilters[] = {
        { "All",  FlagType::Unknown },
        { "bool", FlagType::Bool    },
        { "int",  FlagType::Int     },
        { "str",  FlagType::String  },
    };

    for (int i = 0; i < 4; ++i) {
        bool active = (m_typeFilter == kFilters[i].type);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(200, 200, 200, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(220, 220, 220, 255));
            ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(10,  10,  10,  255));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(30,  30,  30,  255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(50,  50,  50,  255));
            ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(140, 140, 140, 255));
        }
        if (ImGui::Button(kFilters[i].label, { 54, 22 })) {
            m_typeFilter = kFilters[i].type;
            m_needsRefresh = true;
        }
        ImGui::PopStyleColor(3);
        if (i < 4) ImGui::SameLine(0, 4);
    }

    // Rebuild filtered list when needed
    if (m_needsRefresh) {
        auto all = m_flags.Search(m_searchBuf);

        // Apply type filter
        if (m_typeFilter != FlagType::Unknown) {
            m_filteredFlags.clear();
            m_filteredFlags.reserve(all.size());
            for (auto* e : all)
                if (EffectiveType(*e) == m_typeFilter)
                    m_filteredFlags.push_back(e);
        } else {
            m_filteredFlags = std::move(all);
        }

        // Apply current sort (0=Type, 1=Name, 2=Offset, 3=Value)
        std::stable_sort(m_filteredFlags.begin(), m_filteredFlags.end(),
            [&](const FFlagEntry* a, const FFlagEntry* b) -> bool {
                bool less = false;
                switch (m_sortCol) {
                case 0: {
                    int ta = static_cast<int>(EffectiveType(*a));
                    int tb = static_cast<int>(EffectiveType(*b));
                    less = ta < tb;
                    break;
                }
                case 2:
                    less = a->offset < b->offset;
                    break;
                case 3: {
                    // Sort by numeric value if both parseable, else lexicographic
                    try {
                        double va = std::stod(a->currentValue.empty() ? "0" : a->currentValue);
                        double vb = std::stod(b->currentValue.empty() ? "0" : b->currentValue);
                        less = va < vb;
                    } catch (...) {
                        less = a->currentValue < b->currentValue;
                    }
                    break;
                }
                default: // 1 = Name
                    less = a->name < b->name;
                    break;
                }
                return m_sortAsc ? less : !less;
            });
        m_needsRefresh = false;
    }

    ImGui::SameLine(0, 16);
    ImGui::TextDisabled("%zu / %zu", m_filteredFlags.size(), m_flags.GetTotalCount());

    if (m_attached) {
        ImGui::SameLine(0, 16);
        if (ImGui::Button("Read All")) {
            for (auto* e : m_filteredFlags) m_flags.ReadFlagValue(*e);
            m_statusMsg = "Read " + std::to_string(m_filteredFlags.size()) + " flags";
        }
        if (m_autoRead) {
            ImGui::SameLine();
            ImGui::TextDisabled("auto-reading every %.1fs", m_autoReadInterval);
        }
    }

    ImGui::Spacing();

    float tableH = ImGui::GetContentRegionAvail().y - 50;
    ImGuiTableFlags tf =
        ImGuiTableFlags_Borders    | ImGuiTableFlags_RowBg      |
        ImGuiTableFlags_ScrollY    | ImGuiTableFlags_Resizable  |
        ImGuiTableFlags_Sortable   | ImGuiTableFlags_SortTristate;

    if (ImGui::BeginTable("##flags", 5, tf, { 0, tableH })) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_DefaultSort,   46.0f);
        ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_DefaultSort,  280.0f);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_DefaultSort,   80.0f);
        ImGui::TableSetupColumn("Value",  ImGuiTableColumnFlags_DefaultSort,   90.0f);
        ImGui::TableSetupColumn("Set",    ImGuiTableColumnFlags_NoSort,        190.0f);
        ImGui::TableHeadersRow();

        // Handle column header clicks for sorting
        // Columns: 0=Type, 1=Name, 2=Offset, 3=Value
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsDirty && specs->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
                bool newAsc = (s.SortDirection == ImGuiSortDirection_Ascending);
                if (s.ColumnIndex != m_sortCol || newAsc != m_sortAsc) {
                    m_sortCol = s.ColumnIndex;
                    m_sortAsc = newAsc;
                    m_needsRefresh = true;
                }
                specs->SpecsDirty = false;
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_filteredFlags.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                FFlagEntry* entry = m_filteredFlags[row];
                FlagType ftype = EffectiveType(*entry);

                ImGui::TableNextRow();
                ImGui::PushID(row);

                // ── Type tag (click to filter by this type) ───────────────────
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(130, 130, 130, 255));
                ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 40, 40, 200));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(60, 60, 60, 200));
                if (ImGui::SmallButton(FlagTypeTag(ftype))) {
                    m_typeFilter   = (m_typeFilter == ftype) ? FlagType::Unknown : ftype;
                    m_needsRefresh = true;
                }
                ImGui::PopStyleColor(4);

                // Helper: invisible button over full cell width, text drawn on top.
                // Returns true if clicked. Highlights row bg on hover.
                auto CopyCell = [&](const char* id, const char* text, ImU32 textCol, const char* copyStr) -> bool {
                    float cellW = ImGui::GetContentRegionAvail().x;
                    float cellH = ImGui::GetTextLineHeight();
                    ImVec2 pos  = ImGui::GetCursorScreenPos();
                    bool clicked = ImGui::InvisibleButton(id, { cellW, cellH });
                    bool hovered = ImGui::IsItemHovered();
                    if (hovered) {
                        ImGui::GetWindowDrawList()->AddRectFilled(pos,
                            { pos.x + cellW, pos.y + cellH }, IM_COL32(40, 40, 40, 160));
                        ImGui::SetTooltip("Click to copy");
                    }
                    ImGui::GetWindowDrawList()->AddText({ pos.x, pos.y }, textCol, text);
                    if (clicked) {
                        ImGui::SetClipboardText(copyStr);
                        m_copyFlashTimer = 1.2f;
                        m_copyFlashLabel = copyStr;
                    }
                    return clicked;
                };

                // ── Name (click to copy) ──────────────────────────────────────
                ImGui::TableSetColumnIndex(1);
                CopyCell(("##n" + std::to_string(row)).c_str(),
                    entry->name.c_str(), IM_COL32(220, 220, 220, 255), entry->name.c_str());

                // ── Offset (click to copy) ────────────────────────────────────
                ImGui::TableSetColumnIndex(2);
                {
                    char offsetStr[32];
                    snprintf(offsetStr, sizeof(offsetStr), "0x%X", (unsigned int)entry->offset);
                    CopyCell(("##o" + std::to_string(row)).c_str(),
                        offsetStr, IM_COL32(120, 120, 120, 255), offsetStr);
                }

                // ── Current value (click to copy) ─────────────────────────────
                ImGui::TableSetColumnIndex(3);
                if (entry->readSuccess) {
                    const char* valStr = entry->currentValue.c_str();
                    if (ftype == FlagType::Bool) {
                        bool bval = (entry->currentValue == "1" || entry->currentValue == "true");
                        // Draw colour dot manually, then the text next to it
                        ImVec2 cp  = ImGui::GetCursorScreenPos();
                        float  lh  = ImGui::GetTextLineHeight();
                        float  cellW = ImGui::GetContentRegionAvail().x;
                        ImU32  dotCol = bval ? IM_COL32(100, 200, 100, 255) : IM_COL32(80, 80, 80, 255);
                        bool   clicked = ImGui::InvisibleButton(("##v" + std::to_string(row)).c_str(), { cellW, lh });
                        bool   hovered = ImGui::IsItemHovered();
                        if (hovered) {
                            ImGui::GetWindowDrawList()->AddRectFilled(cp,
                                { cp.x + cellW, cp.y + lh }, IM_COL32(40, 40, 40, 160));
                            ImGui::SetTooltip("Click to copy");
                        }
                        valStr = bval ? "true" : "false";
                        ImGui::GetWindowDrawList()->AddCircleFilled(
                            { cp.x + 5, cp.y + lh * 0.5f }, 4.5f, dotCol);
                        ImGui::GetWindowDrawList()->AddText(
                            { cp.x + 14, cp.y }, IM_COL32(220, 220, 220, 255), valStr);
                        if (clicked) {
                            ImGui::SetClipboardText(valStr);
                            m_copyFlashTimer = 1.2f;
                            m_copyFlashLabel = valStr;
                        }
                    } else {
                        CopyCell(("##v" + std::to_string(row)).c_str(),
                            valStr, IM_COL32(220, 220, 220, 255), valStr);
                    }
                } else {
                    ImGui::TextDisabled("--");
                }

                // ── Set control ───────────────────────────────────────────────
                ImGui::TableSetColumnIndex(4);
                ImGui::SetNextItemWidth(-1);

                bool didWrite = false;
                std::string writeVal;

                if (ftype == FlagType::Bool) {
                    // Toggle — toggling writes immediately
                    bool cur = (entry->readSuccess &&
                                (entry->currentValue == "1" || entry->currentValue == "true"));
                    bool toggled = cur;
                    if (ToggleButton("##cb", &toggled) && m_attached) {
                        writeVal = toggled ? "true" : "false";
                        didWrite = true;
                    }
                } else if (ftype == FlagType::Int) {
                    // Integer input — confirm with Enter
                    int ival = 0;
                    if (entry->readSuccess && !entry->currentValue.empty()) {
                        try { ival = std::stoi(entry->currentValue); } catch (...) {}
                    }
                    if (ImGui::InputInt("##ii", &ival, 0, 0,
                            ImGuiInputTextFlags_EnterReturnsTrue) && m_attached) {
                        writeVal = std::to_string(ival);
                        didWrite = true;
                    }
                } else {
                    // Generic text input
                    char valueBuf[256] = {};
                    if (ImGui::InputTextWithHint("##val", "value", valueBuf, sizeof(valueBuf),
                            ImGuiInputTextFlags_EnterReturnsTrue) && m_attached && valueBuf[0]) {
                        writeVal = valueBuf;
                        didWrite = true;
                    }
                }

                if (didWrite && m_attached && !writeVal.empty()) {
                    if (m_flags.WriteFlagValue(entry->name, writeVal))
                        m_flags.ReadFlagValue(*entry);
                }

                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    RenderStatusBar();
}

// ── Injector page ────────────────────────────────────────────────────────────

// Syntax colorize hook — matches ImGuiSyntaxColorizeFunc signature.
// Called by patched imgui_widgets.cpp instead of the plain AddText draw call.
static void InjectorSyntaxColorize(ImDrawList* dl, ImFont* fnt, float fs,
    ImVec2 pos, const char* buf, const char* buf_end) // matches ImGui::ImGuiSyntaxColorizeFunc
{

    const ImU32 cKey   = IM_COL32(156, 220, 254, 255);
    const ImU32 cStr   = IM_COL32(206, 145, 120, 255);
    const ImU32 cNum   = IM_COL32(181, 206, 168, 255);
    const ImU32 cBool  = IM_COL32(86,  156, 214, 255);
    const ImU32 cPunct = IM_COL32(128, 128, 128, 255);
    const ImU32 cText  = IM_COL32(212, 212, 212, 255);

    // Detect JSON vs key=value
    bool isJson = false;
    for (const char* p = buf; p < buf_end; ++p) {
        if (*p == '{' || *p == '[') { isJson = true; break; }
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') break;
    }

    // InputText advances lines by FontSize (no ItemSpacing between lines inside the widget)
    float lh = fnt->FontSize;
    float y  = pos.y;
    const char* lineStart = buf;

    auto tok = [&](float x, const char* s, size_t len, ImU32 col) {
        if (len) dl->AddText(fnt, fs, { x, y }, col, s, s + len);
    };

    while (lineStart < buf_end) {
        const char* lineEnd = lineStart;
        while (lineEnd < buf_end && *lineEnd != '\n') ++lineEnd;

        float x = pos.x;
        const char* p = lineStart;

        if (isJson) {
            while (p < lineEnd) {
                char c = *p;
                if (c=='{' || c=='}' || c=='[' || c==']' || c==':' || c==',') {
                    tok(x, p, 1, cPunct);
                    x += fnt->CalcTextSizeA(fs, FLT_MAX, 0, p, p+1).x; ++p;
                } else if (c == '"') {
                    const char* q = p + 1;
                    while (q < lineEnd && *q != '"') { if (*q == '\\') ++q; ++q; }
                    if (q < lineEnd) ++q;
                    const char* after = q;
                    while (after < lineEnd && (*after==' '||*after=='\t')) ++after;
                    ImU32 col = (*after == ':') ? cKey : cStr;
                    tok(x, p, q-p, col);
                    x += fnt->CalcTextSizeA(fs, FLT_MAX, 0, p, q).x; p = q;
                } else if (c == 't' || c == 'f') {
                    size_t rem = lineEnd - p;
                    if (rem >= 4 && strncmp(p,"true",4)==0)        { tok(x,p,4,cBool); x+=fnt->CalcTextSizeA(fs,FLT_MAX,0,p,p+4).x; p+=4; }
                    else if (rem>=5 && strncmp(p,"false",5)==0)     { tok(x,p,5,cBool); x+=fnt->CalcTextSizeA(fs,FLT_MAX,0,p,p+5).x; p+=5; }
                    else                                             { tok(x,p,1,cText); x+=fnt->CalcTextSizeA(fs,FLT_MAX,0,p,p+1).x; ++p; }
                } else if (c=='-' || (c>='0'&&c<='9')) {
                    const char* q = p; if (*q=='-') ++q;
                    while (q<lineEnd && ((*q>='0'&&*q<='9')||*q=='.'||*q=='e'||*q=='+'||*q=='-')) ++q;
                    tok(x, p, q-p, cNum); x += fnt->CalcTextSizeA(fs,FLT_MAX,0,p,q).x; p = q;
                } else if (c==' '||c=='\t') {
                    x += fnt->CalcTextSizeA(fs,FLT_MAX,0,p,p+1).x; ++p;
                } else {
                    tok(x,p,1,cText); x+=fnt->CalcTextSizeA(fs,FLT_MAX,0,p,p+1).x; ++p;
                }
            }
        } else {
            const char* eq = (const char*)memchr(lineStart, '=', lineEnd-lineStart);
            if (eq) {
                tok(x, lineStart, eq-lineStart, cKey);
                float kw = fnt->CalcTextSizeA(fs,FLT_MAX,0,lineStart,eq).x;
                tok(x+kw, "=", 1, cPunct);
                float ew = fnt->CalcTextSizeA(fs,FLT_MAX,0,"=","="+1).x;
                const char* val = eq+1; size_t vl = lineEnd-val;
                ImU32 vc = cText;
                if (vl==4 && strncmp(val,"true",4)==0)  vc=cBool;
                if (vl==5 && strncmp(val,"false",5)==0) vc=cBool;
                bool isNum = vl>0; for (size_t i=0;i<vl&&isNum;++i) if (!isdigit((unsigned char)val[i])&&val[i]!='-'&&val[i]!='.') isNum=false;
                if (isNum&&vl) vc=cNum;
                tok(x+kw+ew, val, vl, vc);
            } else {
                ImU32 col = (lineStart<lineEnd&&(*lineStart=='#'||*lineStart=='/')) ? cPunct : cText;
                tok(x, lineStart, lineEnd-lineStart, col);
            }
        }

        y += lh;
        lineStart = (lineEnd < buf_end) ? lineEnd + 1 : buf_end;
    }
}

void GhostGUI::RenderPageInjector() {
    ImGui::TextDisabled("Paste FFlags below (JSON or key=value), or load from file.");
    ImGui::Spacing();

    float multiH = ImGui::GetContentRegionAvail().y - 130;

    // Install syntax hook before InputTextMultiline, remove after
    ImGui::GhostSyntaxColorize = InjectorSyntaxColorize;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(18, 18, 18, 255));
    ImGui::InputTextMultiline("##inject", m_injectBuf, sizeof(m_injectBuf),
        { -1, multiH }, ImGuiInputTextFlags_AllowTabInput);
    ImGui::PopStyleColor();
    ImGui::GhostSyntaxColorize = nullptr;

    ImGui::Spacing();

    if (ImGui::Button("Inject", { 110, 28 })) DoInject();
    ImGui::SameLine();
    if (ImGui::Button("Load File", { 110, 28 })) DoLoadFile();
    ImGui::SameLine();
    if (ImGui::Button("Save Config", { 120, 28 })) {
        SaveConfig();
        m_statusMsg = "Config saved";
        AddLog("[CONFIG] Saved flags to " + m_configPath);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear", { 70, 28 })) {
        m_injectBuf[0] = '\0';
        m_injectStatus.clear();
        m_injectErrors.clear();
    }

    if (!m_injectStatus.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted(m_injectStatus.c_str());
    }
    for (const auto& err : m_injectErrors) {
        ImGui::PushStyleColor(ImGuiCol_Text, { 0.55f, 0.55f, 0.55f, 1.0f });
        ImGui::TextUnformatted(err.c_str());
        ImGui::PopStyleColor();
    }

    if (m_pendingInject) {
        ImGui::Spacing();
        ImGui::TextDisabled("injecting... (attempt %d)", m_injectAttempts);
    }

    RenderStatusBar();
}

// ── Presets page ─────────────────────────────────────────────────────────────

void GhostGUI::RenderPagePresets() {
    if (!m_presetsScanned) {
        m_presetFiles.clear();
        if (fs::exists(m_presetsDir)) {
            for (const auto& e : fs::directory_iterator(m_presetsDir))
                if (e.path().extension() == ".json")
                    m_presetFiles.push_back(e.path().filename().string());
        }
        std::sort(m_presetFiles.begin(), m_presetFiles.end());
        m_presetsScanned = true;
    }

    ImGui::TextUnformatted("Save Current Injector Text as Preset");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(230);
    ImGui::InputTextWithHint("##pname", "preset name", m_presetNameBuf, sizeof(m_presetNameBuf));
    ImGui::SameLine();
    if (ImGui::Button("Save", { 90, 0 })) DoSavePreset();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Saved Presets");
    ImGui::Spacing();

    if (m_presetFiles.empty()) {
        ImGui::TextDisabled("No presets saved yet.");
    } else {
        for (size_t i = 0; i < m_presetFiles.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Button("Load", { 55, 0 })) {
                auto res = FlagParser::LoadFile(m_presetsDir + "\\" + m_presetFiles[i]);
                if (res.success) {
                    std::string text;
                    for (const auto& f : res.flags)
                        text += (f.originalName.empty() ? f.name : f.originalName) + "=" + f.value + "\n";
                    strncpy_s(m_injectBuf, text.c_str(), sizeof(m_injectBuf) - 1);
                    m_statusMsg     = "Loaded preset: " + m_presetFiles[i];
                    m_activePage    = Page::Injector;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Del", { 40, 0 })) {
                fs::remove(m_presetsDir + "\\" + m_presetFiles[i]);
                m_presetsScanned = false;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(m_presetFiles[i].c_str());
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Refresh", { 90, 0 })) m_presetsScanned = false;

    RenderStatusBar();
}

// ── Logs page ────────────────────────────────────────────────────────────────

void GhostGUI::RenderPageLogs() {
    if (ImGui::Button("Clear", { 70, 0 })) m_logs.clear();
    ImGui::SameLine();
    ToggleButton("Auto-scroll", &m_logAutoScroll);
    ImGui::SameLine();
    ToggleButton("Log to file", &m_logToFile);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu lines", m_logs.size());
    ImGui::Spacing();

    float logH = ImGui::GetContentRegionAvail().y - 50;
    ImGui::BeginChild("##logs", { 0, logH }, ImGuiChildFlags_Borders);

    for (const auto& line : m_logs) {
        bool isErr = line.find("FAILED") != std::string::npos
                  || line.find("error")  != std::string::npos
                  || line.find("failed") != std::string::npos;
        bool isOk  = line.find(" OK")      != std::string::npos
                  || line.find("Success")  != std::string::npos
                  || line.find("Attached") != std::string::npos
                  || line.find("Injected") != std::string::npos;

        if (isErr)      ImGui::PushStyleColor(ImGuiCol_Text, { 0.65f, 0.28f, 0.28f, 1.0f });
        else if (isOk)  ImGui::PushStyleColor(ImGuiCol_Text, { 0.28f, 0.65f, 0.28f, 1.0f });

        ImGui::TextUnformatted(line.c_str());

        if (isErr || isOk) ImGui::PopStyleColor();
    }

    if (m_logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    RenderStatusBar();
}

// ── Settings page ────────────────────────────────────────────────────────────

void GhostGUI::RenderPageSettings() {
    auto Section = [](const char* label) {
        ImGui::Spacing();
        ImGui::TextUnformatted(label);
        ImGui::Separator();
        ImGui::Spacing();
    };

    Section("Automation");

    if (ToggleButton("Auto-attach to RobloxPlayerBeta.exe", &m_autoAttach)) {
        if (m_autoAttach) StartWatcherThread();
        else              StopWatcherThread();
    }
    ImGui::TextDisabled("  Detects Roblox launch instantly via background watcher");
    ImGui::Spacing();

    ToggleButton("Auto-inject saved FFlags on attach", &m_autoInject);
    ImGui::TextDisabled("  Waits for flags to initialize, then injects");
    ImGui::Spacing();

    ToggleButton("Auto-read FFlag values", &m_autoRead);
    if (m_autoRead) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::SliderFloat("##ri", &m_autoReadInterval, 0.5f, 10.0f, "%.1fs");
        ImGui::SameLine();
        ImGui::TextDisabled("interval");
    }
    ImGui::Spacing();

    ToggleButton("Auto-update offsets on version mismatch", &m_autoUpdateOffsets);
    ImGui::TextDisabled("  Fetches latest offsets when Roblox has updated");

    Section("Streamproof");

    if (ToggleButton("Hide from screen capture (OBS, Discord, etc.)", &m_streamproof)) {
        if (m_hwnd) {
            SetWindowDisplayAffinity(m_hwnd, m_streamproof ? 0x11 : 0x00);
            AddLog(m_streamproof ? "[SETTINGS] Streamproof enabled" : "[SETTINGS] Streamproof disabled");
        }
    }
    ImGui::TextDisabled("  Window appears black in OBS / Discord screen share");

    Section("Offsets");

    {
        std::string ov  = m_flags.GetExpectedVersion();
        const char* ovs = m_fetchingOffsets.load()           ? "fetching..."
                        : (ov.empty() || ov == "unknown")    ? "no offsets loaded"
                                                             : ov.c_str();
        ImGui::TextDisabled("  Loaded: %s  (%zu flags)", ovs, m_flags.GetTotalCount());
    }

    if (!m_flags.IsVersionMatch() && !m_flags.GetVersionMismatchMsg().empty())
        ImGui::TextDisabled("  %s", m_flags.GetVersionMismatchMsg().c_str());

    ImGui::Spacing();

    if (m_fetchingOffsets.load()) {
        ImGui::BeginDisabled();
        ImGui::Button("Downloading...", { 200, 0 });
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Update Offsets", { 200, 0 })) {
            AddLog("[UPDATE] Starting offset update...");
            StartAutoFetchOffsets();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Check Version", { 130, 0 })) {
        std::string latest;
        if (m_flags.FetchLatestVersion(latest)) {
            m_statusMsg = (latest == m_flags.GetExpectedVersion())
                        ? "Offsets are up to date"
                        : "New version available: " + latest;
        } else {
            m_statusMsg = "Failed to check version";
        }
    }
    ImGui::TextDisabled("  Source: imtheo.lol/Offsets/FFlags.json");

    Section("Config");

    if (fs::exists(m_configPath)) {
        ImGui::TextDisabled("%s", m_configPath.c_str());
        if (ImGui::Button("Delete Config", { 130, 0 })) {
            fs::remove(m_configPath);
            AddLog("[CONFIG] Deleted config file");
        }
    } else {
        ImGui::TextDisabled("No config saved. Use 'Save Config' in the Injector tab.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Data folder: %s", m_dataDir.c_str());
    if (ImGui::Button("Open Folder", { 120, 0 }))
        ShellExecuteA(nullptr, "open", m_dataDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    RenderStatusBar();
}

// ── Actions ──────────────────────────────────────────────────────────────────

void GhostGUI::DoAttach() {
    if (m_proc.Attach(L"RobloxPlayerBeta.exe")) {
        m_flags.CheckVersion();
        m_offsetUpdateTriggered = false;
        m_statusMsg  = "Attached (PID: " + std::to_string(m_proc.GetProcessId()) + ")";
        m_needsRefresh = true;
    } else {
        m_statusMsg = "Failed to find RobloxPlayerBeta.exe";
    }
}

void GhostGUI::DoAttachByPid() {
    DWORD pid = 0;
    try { pid = static_cast<DWORD>(std::stoul(m_pidBuf)); }
    catch (...) { m_statusMsg = "Invalid PID"; return; }

    if (m_proc.AttachByPid(pid)) {
        m_flags.CheckVersion();
        m_statusMsg    = "Attached by PID: " + std::to_string(pid);
        m_needsRefresh = true;
        m_pidBuf[0]    = '\0';
    } else {
        m_statusMsg = "Failed to attach to PID " + std::to_string(pid);
    }
}

int GhostGUI::InjectFlags(const std::string& text, bool silent) {
    if (!m_proc.IsAttached()) return 0;

    auto result = FlagParser::Parse(text);
    if (!result.success) return 0;

    int success = 0, fail = 0;
    std::vector<std::string> notFound;

    for (const auto& flag : result.flags) {
        const FFlagEntry* entry = m_flags.FindFlag(flag.name);
        if (!entry) { notFound.push_back(flag.originalName.empty() ? flag.name : flag.originalName); fail++; continue; }
        if (m_flags.WriteFlagValue(flag.name, flag.value, flag.originalName)) success++;
        else fail++;
    }

    std::string summary = "Injected " + std::to_string(success) + "/" + std::to_string(result.flags.size()) + " flags";
    AddLog("[INJECT] " + summary);

    if (!silent) {
        m_injectStatus = summary;
        m_injectErrors.clear();
        if (!notFound.empty()) {
            std::string msg = "Not in offset map: ";
            for (size_t i = 0; i < notFound.size() && i < 10; ++i) {
                if (i > 0) msg += ", ";
                msg += notFound[i];
            }
            if (notFound.size() > 10) msg += " (+" + std::to_string(notFound.size() - 10) + " more)";
            m_injectErrors.push_back(msg);
        }
    }

    return success;
}

void GhostGUI::DoInject() {
    m_injectErrors.clear();
    m_injectSuccessCount = 0;

    if (!m_attached) { m_injectStatus = "Not attached to Roblox."; return; }
    std::string text(m_injectBuf);
    if (text.empty()) { m_injectStatus = "Nothing to inject."; return; }

    m_injectSuccessCount = InjectFlags(text, false);
}

void GhostGUI::DoLoadFile() {
    OPENFILENAMEA ofn = {};
    char path[MAX_PATH] = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "JSON Files\0*.json\0Text Files\0*.txt\0All Files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameA(&ofn)) {
        auto res = FlagParser::LoadFile(path);
        if (res.success) {
            std::string text;
            for (const auto& f : res.flags)
                text += (f.originalName.empty() ? f.name : f.originalName) + "=" + f.value + "\n";
            strncpy_s(m_injectBuf, text.c_str(), sizeof(m_injectBuf) - 1);
            m_statusMsg = "Loaded file";
            AddLog("[FILE] Loaded " + std::to_string(res.flags.size()) + " flags");
        } else {
            m_injectStatus = "Failed to load file";
            m_injectErrors = res.errors;
        }
    }
}

void GhostGUI::DoSavePreset() {
    std::string name(m_presetNameBuf);
    if (name.empty()) { m_statusMsg = "Enter a preset name first"; return; }
    fs::create_directories(m_presetsDir);
    auto result = FlagParser::Parse(std::string(m_injectBuf));
    if (!result.success || result.flags.empty()) { m_statusMsg = "Nothing to save"; return; }
    if (FlagParser::SaveToFile(m_presetsDir + "\\" + name + ".json", result.flags)) {
        m_statusMsg      = "Saved preset: " + name;
        m_presetsScanned = false;
    }
}

void GhostGUI::SaveConfig() {
    std::string text(m_injectBuf);
    if (text.empty()) return;
    std::ofstream f(m_configPath);
    if (f.is_open()) f << text;
}

void GhostGUI::LoadConfig() {
    if (!fs::exists(m_configPath)) return;
    std::ifstream f(m_configPath);
    if (!f.is_open()) return;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    if (!content.empty()) {
        strncpy_s(m_injectBuf, content.c_str(), sizeof(m_injectBuf) - 1);
        AddLog("[CONFIG] Loaded saved flags from " + m_configPath);
    }
}
