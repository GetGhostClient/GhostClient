#include "gui.h"
#include "imgui.h"
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
    m_dataDir = GetAppDataDir();
    m_configPath = m_dataDir + "\\ghostclient_config.json";
    m_logPath = m_dataDir + "\\ghostclient_log.txt";
    m_presetsDir = m_dataDir + "\\presets";
    fs::create_directories(m_presetsDir);

    auto logCb = [this](const std::string& msg) { AddLog(msg); };
    m_proc.SetLogCallback(logCb);
    m_flags.SetLogCallback(logCb);

    if (m_logToFile) {
        std::ofstream(m_logPath, std::ios::trunc).close();
    }

    LoadConfig();
    AddLog("Ghost Client started");
    AddLog("Data dir: " + m_dataDir);

    size_t flagCount = m_flags.GetTotalCount();
    AddLog("Loaded " + std::to_string(flagCount) + " FFlag offsets");

    if (flagCount == 0) {
        AddLog("[OFFSETS] No offsets found - fetching automatically...");
        StartAutoFetchOffsets();
    }

    if (m_autoAttach)
        AddLog("Auto-attach enabled - waiting for RobloxPlayerBeta.exe...");
}

struct FetchThreadParam {
    FFlagManager* flags;
    std::atomic<bool>* success;
    std::atomic<bool>* done;
    std::atomic<bool>* running;
};

static DWORD WINAPI FetchOffsetsThread(LPVOID param) {
    auto* p = reinterpret_cast<FetchThreadParam*>(param);
    bool ok = p->flags->FetchAndUpdateOffsets();
    p->success->store(ok);
    p->done->store(true);
    p->running->store(false);
    delete p;
    return 0;
}

void GhostGUI::StartAutoFetchOffsets() {
    if (m_fetchingOffsets.load()) return;
    m_fetchingOffsets = true;
    m_fetchOffsetsDone = false;
    m_fetchOffsetsSuccess = false;
    m_statusMsg = "Downloading offsets...";

    if (m_fetchThread) {
        CloseHandle(m_fetchThread);
        m_fetchThread = nullptr;
    }

    auto* p = new FetchThreadParam{
        &m_flags,
        &m_fetchOffsetsSuccess,
        &m_fetchOffsetsDone,
        &m_fetchingOffsets
    };
    m_fetchThread = CreateThread(nullptr, 0, FetchOffsetsThread, p, 0, nullptr);
}

void GhostGUI::AddLog(const std::string& msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    std::string line = std::string(timeBuf) + msg;
    m_logs.push_back(line);
    if (m_logs.size() > MAX_LOGS)
        m_logs.pop_front();

    if (m_logToFile && !m_logPath.empty()) {
        std::ofstream logFile(m_logPath, std::ios::app);
        if (logFile.is_open())
            logFile << line << "\n";
    }
}

void GhostGUI::ApplyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    // Pure monochrome palette — no hue anywhere
    const ImVec4 bg0     = ImVec4(0.04f, 0.04f, 0.04f, 1.00f); // near black
    const ImVec4 bg1     = ImVec4(0.09f, 0.09f, 0.09f, 1.00f); // dark grey
    const ImVec4 bg2     = ImVec4(0.14f, 0.14f, 0.14f, 1.00f); // mid grey (frames/inputs)
    const ImVec4 bg3     = ImVec4(0.20f, 0.20f, 0.20f, 1.00f); // lighter grey (hover)
    const ImVec4 bg4     = ImVec4(0.28f, 0.28f, 0.28f, 1.00f); // active/pressed
    const ImVec4 border  = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    const ImVec4 text    = ImVec4(0.95f, 0.95f, 0.95f, 1.00f); // near white
    const ImVec4 textDim = ImVec4(0.40f, 0.40f, 0.40f, 1.00f); // muted grey
    const ImVec4 white   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    const ImVec4 none    = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_Text]                 = text;
    c[ImGuiCol_TextDisabled]         = textDim;
    c[ImGuiCol_WindowBg]             = bg0;
    c[ImGuiCol_ChildBg]              = bg0;
    c[ImGuiCol_PopupBg]              = bg1;
    c[ImGuiCol_Border]               = border;
    c[ImGuiCol_BorderShadow]         = none;
    c[ImGuiCol_FrameBg]              = bg2;
    c[ImGuiCol_FrameBgHovered]       = bg3;
    c[ImGuiCol_FrameBgActive]        = bg4;
    c[ImGuiCol_TitleBg]              = bg1;
    c[ImGuiCol_TitleBgActive]        = bg1;
    c[ImGuiCol_TitleBgCollapsed]     = bg0;
    c[ImGuiCol_MenuBarBg]            = bg1;
    c[ImGuiCol_ScrollbarBg]          = bg0;
    c[ImGuiCol_ScrollbarGrab]        = bg3;
    c[ImGuiCol_ScrollbarGrabHovered] = bg4;
    c[ImGuiCol_ScrollbarGrabActive]  = white;
    c[ImGuiCol_CheckMark]            = white;
    c[ImGuiCol_SliderGrab]           = bg4;
    c[ImGuiCol_SliderGrabActive]     = white;
    c[ImGuiCol_Button]               = bg2;
    c[ImGuiCol_ButtonHovered]        = bg3;
    c[ImGuiCol_ButtonActive]         = bg4;
    c[ImGuiCol_Header]               = bg2;
    c[ImGuiCol_HeaderHovered]        = bg3;
    c[ImGuiCol_HeaderActive]         = bg4;
    c[ImGuiCol_Separator]            = border;
    c[ImGuiCol_SeparatorHovered]     = bg3;
    c[ImGuiCol_SeparatorActive]      = white;
    c[ImGuiCol_ResizeGrip]           = none;
    c[ImGuiCol_ResizeGripHovered]    = bg3;
    c[ImGuiCol_ResizeGripActive]     = white;
    c[ImGuiCol_Tab]                  = bg1;
    c[ImGuiCol_TabHovered]           = bg3;
    c[ImGuiCol_TabSelected]          = bg2;
    c[ImGuiCol_TabDimmed]            = bg1;
    c[ImGuiCol_TabDimmedSelected]    = bg2;
    c[ImGuiCol_TableHeaderBg]        = bg1;
    c[ImGuiCol_TableBorderStrong]    = border;
    c[ImGuiCol_TableBorderLight]     = bg2;
    c[ImGuiCol_TableRowBg]           = none;
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(1.00f, 1.00f, 1.00f, 0.18f);
    c[ImGuiCol_NavHighlight]         = white;

    style.WindowRounding    = 4.0f;
    style.ChildRounding     = 3.0f;
    style.FrameRounding     = 3.0f;
    style.GrabRounding      = 3.0f;
    style.TabRounding       = 3.0f;
    style.PopupRounding     = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.WindowPadding     = ImVec2(14, 14);
    style.FramePadding      = ImVec2(8, 4);
    style.ItemSpacing       = ImVec2(8, 5);
    style.ItemInnerSpacing  = ImVec2(6, 4);
    style.ScrollbarSize     = 10.0f;
    style.GrabMinSize       = 8.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.TabBorderSize     = 0.0f;
    style.WindowMinSize     = ImVec2(400, 300);
}

void GhostGUI::Render() {
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - m_lastFrame).count();
    m_lastFrame = now;

    // Handle background offset fetch completion
    if (m_fetchOffsetsDone.load()) {
        m_fetchOffsetsDone = false;
        if (m_fetchOffsetsSuccess.load()) {
            m_statusMsg = "Offsets downloaded: " + std::to_string(m_flags.GetTotalCount()) + " flags";
            AddLog("[OFFSETS] Auto-fetch complete: " + std::to_string(m_flags.GetTotalCount()) + " flags loaded");
            m_needsRefresh = true;
        } else {
            m_statusMsg = "Failed to download offsets - check connection";
            AddLog("[OFFSETS] Auto-fetch failed - use Settings > Update Offsets to retry");
        }
    }

    m_attached = m_proc.IsAttached();

    // Cache HWND for streamproof on first frame
    if (!m_hwnd) {
        m_hwnd = FindWindowW(L"GhostClientClass", nullptr);
    }

    // Auto-attach logic
    if (m_autoAttach && !m_attached) {
        m_autoAttachTimer += dt;
        if (m_autoAttachTimer >= AUTO_ATTACH_INTERVAL) {
            m_autoAttachTimer = 0.0f;
            if (m_proc.Attach(L"RobloxPlayerBeta.exe")) {
                m_flags.CheckVersion();
                m_attached = true;
                m_offsetUpdateTriggered = false;
                m_statusMsg = "Auto-attached (PID: " + std::to_string(m_proc.GetProcessId()) + ")";
                AddLog("[AUTO] Attached to Roblox (PID: " + std::to_string(m_proc.GetProcessId()) + ")");
                m_needsRefresh = true;

                if (m_autoInject && m_injectBuf[0] != '\0') {
                    m_pendingInject = true;
                    m_injectDelayTimer = 0.0f;
                    m_injectAttempts = 0;
                    AddLog("[AUTO] Waiting for FFlags to initialize before injecting...");
                }
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
    }
    m_wasAttached = m_attached;

    // Auto-update offsets when version mismatch is detected after attach
    if (m_autoUpdateOffsets && m_attached && !m_fetchingOffsets.load()
        && !m_offsetUpdateTriggered && !m_flags.IsVersionMatch()
        && m_flags.GetTotalCount() > 0)
    {
        m_offsetUpdateTriggered = true;
        AddLog("[OFFSETS] Version mismatch detected - fetching latest offsets automatically...");
        StartAutoFetchOffsets();
    }

    // Auto-read: periodically read all visible flag values
    if (m_autoRead && m_attached && !m_filteredFlags.empty()) {
        m_autoReadTimer += dt;
        if (m_autoReadTimer >= m_autoReadInterval) {
            m_autoReadTimer = 0.0f;
            for (auto* entry : m_filteredFlags)
                m_flags.ReadFlagValue(*entry);
        }
    }

    // Delayed/repeated injection: wait for flags to initialize, then inject
    if (m_pendingInject && m_attached) {
        m_injectDelayTimer += dt;
        if (m_injectDelayTimer >= INJECT_RETRY_INTERVAL) {
            m_injectDelayTimer = 0.0f;
            m_injectAttempts++;

            // Check if a known flag has been initialized
            bool ready = m_flags.IsFlagInitialized("DebugDisplayFPS")
                      || m_flags.IsFlagInitialized("TaskSchedulerTargetFps")
                      || m_injectAttempts >= 5; // after 10 seconds, try anyway

            if (ready) {
                AddLog("[AUTO] Injecting flags (attempt " + std::to_string(m_injectAttempts) + ")...");
                int ok = InjectFlags(m_injectBuf, true);

                if (ok > 0 || m_injectAttempts >= MAX_INJECT_ATTEMPTS) {
                    m_pendingInject = false;
                    if (ok > 0) {
                        m_statusMsg = "Auto-injected " + std::to_string(ok) + " flags";
                    } else {
                        AddLog("[AUTO] Giving up after " + std::to_string(m_injectAttempts) + " attempts");
                    }
                }
            } else {
                AddLog("[AUTO] Flags not initialized yet, waiting... (attempt " + std::to_string(m_injectAttempts) + ")");
            }
        }
    }

    // drag handled via InvisibleButton below

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##GhostClient", nullptr, windowFlags);
    ImGui::PopStyleVar();

    float winW = ImGui::GetWindowWidth();

    // ── Custom titlebar ──────────────────────────────────────────────
    const float TITLE_H = 40.0f;
    ImVec2 titleMin = ImGui::GetWindowPos();
    ImVec2 titleMax = ImVec2(titleMin.x + winW, titleMin.y + TITLE_H);

    ImVec2 mp = ImGui::GetIO().MousePos;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Titlebar background
    dl->AddRectFilled(titleMin, titleMax, IM_COL32(14, 14, 14, 255));
    dl->AddLine(ImVec2(titleMin.x, titleMax.y), titleMax, IM_COL32(50, 50, 50, 255));

    float curX = titleMin.x + 12.0f;

    // Title text
    const char* title = "Ghost Client";
    ImVec2 titleTxt = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(curX, titleMin.y + (TITLE_H - titleTxt.y) * 0.5f),
        IM_COL32(220, 220, 220, 255), title);

    // Status pill — monochrome
    {
        const char* pillTxt = m_attached ? "ATTACHED"
                            : m_autoAttach ? "WAITING" : "OFFLINE";
        ImU32 pillCol = m_attached   ? IM_COL32(80, 80, 80, 220)
                      : m_autoAttach ? IM_COL32(50, 50, 50, 220)
                                     : IM_COL32(30, 30, 30, 220);
        ImVec2 ps = ImGui::CalcTextSize(pillTxt);
        float px = curX + titleTxt.x + 10.0f;
        float py = titleMin.y + (TITLE_H - ps.y - 6.0f) * 0.5f;
        dl->AddRectFilled(ImVec2(px - 6, py), ImVec2(px + ps.x + 6, py + ps.y + 6), pillCol, 4.0f);
        dl->AddText(ImVec2(px, py + 3.0f), IM_COL32(255, 255, 255, 230), pillTxt);
    }

    // Window control buttons: _ [] x  (each 40px wide, right-aligned)
    const float BTN_W = 40.0f;
    float btnX = winW - BTN_W * 3.0f;

    // Drag region: invisible button covering everything left of the buttons
    // On left-click-drag, trigger native window move via WM_NCLBUTTONDOWN HTCAPTION
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##drag", ImVec2(btnX, TITLE_H));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 2.0f)) {
        ReleaseCapture();
        SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }

    // Button renderer — uses SameLine to chain horizontally
    auto WinBtn = [&](const char* id, const char* lbl, ImU32 hoverCol) -> bool {
        ImVec2 bPos = ImGui::GetCursorScreenPos();
        bool hov = mp.x >= bPos.x && mp.x < bPos.x + BTN_W
                && mp.y >= bPos.y && mp.y < bPos.y + TITLE_H;
        if (hov) dl->AddRectFilled(bPos, ImVec2(bPos.x + BTN_W, bPos.y + TITLE_H), hoverCol);
        ImVec2 tc = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(bPos.x + (BTN_W - tc.x) * 0.5f, bPos.y + (TITLE_H - tc.y) * 0.5f),
            IM_COL32(210, 210, 210, 255), lbl);
        ImGui::InvisibleButton(id, ImVec2(BTN_W, TITLE_H));
        bool clicked = ImGui::IsItemClicked();
        ImGui::SameLine(0, 0);
        return clicked;
    };

    bool isMaximized = IsZoomed(m_hwnd);
    ImGui::SetCursorPos(ImVec2(btnX, 0));

    if (WinBtn("##min",   "_",                        IM_COL32(55, 55, 55, 220)))
        ShowWindow(m_hwnd, SW_MINIMIZE);
    if (WinBtn("##max",   isMaximized ? "[]" : "[ ]", IM_COL32(55, 55, 55, 220)))
        ShowWindow(m_hwnd, isMaximized ? SW_RESTORE : SW_MAXIMIZE);
    if (WinBtn("##close", "x",                        IM_COL32(190, 40, 40, 230)))
        PostQuitMessage(0);

    // ── Content area ─────────────────────────────────────────────────
    ImGui::SetCursorPos(ImVec2(0, TITLE_H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
    ImGui::BeginChild("##content", ImVec2(0, ImGui::GetContentRegionAvail().y), false);
    ImGui::PopStyleVar();

    RenderMenuBar();

    if (ImGui::BeginTabBar("##MainTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("FFlag Browser")) {
            RenderBrowserTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Injector")) {
            RenderInjectorTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Presets")) {
            RenderPresetsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Logs")) {
            RenderLogsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            RenderSettingsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    RenderStatusBar();

    ImGui::EndChild();
    ImGui::End();
}

void GhostGUI::RenderMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Process")) {
            if (ImGui::MenuItem("Attach to Roblox", nullptr, false, !m_attached))
                DoAttach();
            ImGui::SetNextItemWidth(90);
            ImGui::InputTextWithHint("##PidInput", "PID", m_pidBuf, sizeof(m_pidBuf), ImGuiInputTextFlags_CharsDecimal);
            ImGui::SameLine();
            if (ImGui::MenuItem("Attach by PID", nullptr, false, !m_attached && m_pidBuf[0] != '\0'))
                DoAttachByPid();
            ImGui::Separator();
            if (ImGui::MenuItem("Detach", nullptr, false, m_attached)) {
                m_proc.Detach();
                m_statusMsg = "Detached";
            }
            ImGui::EndMenu();
        }

        if (m_pendingInject) {
            ImGui::SameLine();
            ImGui::TextDisabled("injecting...");
        }
        if (m_fetchingOffsets.load()) {
            ImGui::SameLine();
            ImGui::TextDisabled("fetching offsets...");
        }

        ImGui::EndMenuBar();
    }
}

void GhostGUI::RenderStatusBar() {
    ImGui::Spacing();
    ImGui::Separator();

    if (!m_flags.IsVersionMatch() && !m_flags.GetVersionMismatchMsg().empty())
        ImGui::TextDisabled("%s", m_flags.GetVersionMismatchMsg().c_str());

    std::string ver = m_flags.GetExpectedVersion();
    const char* verStr = m_fetchingOffsets.load()          ? "fetching..."
                       : (ver.empty() || ver == "unknown") ? "no offsets"
                                                           : ver.c_str();
    ImGui::TextDisabled("FFlags: %zu  |  Version: %s  |  PID: %lu",
        m_flags.GetTotalCount(),
        verStr,
        m_attached ? (unsigned long)m_proc.GetProcessId() : 0UL);

    if (!m_statusMsg.empty()) {
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(m_statusMsg.c_str()).x - 20);
        ImGui::TextUnformatted(m_statusMsg.c_str());
    }
}

void GhostGUI::RenderBrowserTab() {
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##Search", "Search FFlags...", m_searchBuf, sizeof(m_searchBuf))) {
        m_needsRefresh = true;
    }

    if (m_needsRefresh) {
        m_filteredFlags = m_flags.Search(m_searchBuf);
        m_needsRefresh = false;
    }

    ImGui::Spacing();

    if (m_attached) {
        if (ImGui::Button("Read All Now")) {
            for (auto* entry : m_filteredFlags)
                m_flags.ReadFlagValue(*entry);
            m_statusMsg = "Read " + std::to_string(m_filteredFlags.size()) + " flag values";
        }
        if (m_autoRead) {
            ImGui::SameLine();
            ImGui::TextDisabled("(auto-reading every %.1fs)", m_autoReadInterval);
        }
    }

    ImGui::Text("Showing %zu / %zu flags", m_filteredFlags.size(), m_flags.GetTotalCount());
    ImGui::Spacing();

    ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Sortable;

    float tableHeight = ImGui::GetContentRegionAvail().y - 40;

    if (ImGui::BeginTable("##FlagsTable", 4, tableFlags, ImVec2(0, tableHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, 300.0f);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_NoSort, 100.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_NoSort, 150.0f);
        ImGui::TableSetupColumn("Set", ImGuiTableColumnFlags_NoSort, 180.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_filteredFlags.size()));

        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                FFlagEntry* entry = m_filteredFlags[row];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(entry->name.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("0x%X", (unsigned int)entry->offset);

                ImGui::TableSetColumnIndex(2);
                if (entry->readSuccess)
                    ImGui::TextUnformatted(entry->currentValue.c_str());
                else
                    ImGui::TextDisabled("--");

                ImGui::TableSetColumnIndex(3);
                ImGui::PushID(row);
                // Per-row buffer — must NOT be static (static = shared across all rows)
                char valueBuf[256] = {};
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputTextWithHint("##val", "value", valueBuf, sizeof(valueBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    if (m_attached && valueBuf[0] != '\0') {
                        if (m_flags.WriteFlagValue(entry->name, valueBuf))
                            m_flags.ReadFlagValue(*entry);
                    }
                }
                ImGui::PopID();
            }
        }

        ImGui::EndTable();
    }
}

void GhostGUI::RenderInjectorTab() {
    ImGui::TextUnformatted("Paste FFlags below (JSON or key=value format), or load from file.");
    ImGui::Spacing();

    ImGui::InputTextMultiline("##InjectText", m_injectBuf, sizeof(m_injectBuf),
        ImVec2(-1, ImGui::GetContentRegionAvail().y - 140),
        ImGuiInputTextFlags_AllowTabInput);

    ImGui::Spacing();

    if (ImGui::Button("Inject", ImVec2(120, 30)))
        DoInject();

    ImGui::SameLine();
    if (ImGui::Button("Load File", ImVec2(120, 30)))
        DoLoadFile();

    ImGui::SameLine();
    if (ImGui::Button("Save as Config", ImVec2(130, 30))) {
        SaveConfig();
        m_statusMsg = "Config saved";
        AddLog("[CONFIG] Saved flags to " + m_configPath);
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(80, 30))) {
        m_injectBuf[0] = '\0';
        m_injectStatus.clear();
        m_injectErrors.clear();
    }

    if (!m_injectStatus.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted(m_injectStatus.c_str());
    }

    for (const auto& err : m_injectErrors) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::TextUnformatted(err.c_str());
        ImGui::PopStyleColor();
    }
}

void GhostGUI::RenderPresetsTab() {
    ImGui::TextUnformatted("Save and load FFlag presets.");
    ImGui::Spacing();

    if (!m_presetsScanned) {
        m_presetFiles.clear();
        if (fs::exists(m_presetsDir)) {
            for (const auto& entry : fs::directory_iterator(m_presetsDir)) {
                if (entry.path().extension() == ".json")
                    m_presetFiles.push_back(entry.path().filename().string());
            }
        }
        std::sort(m_presetFiles.begin(), m_presetFiles.end());
        m_presetsScanned = true;
    }

    ImGui::TextUnformatted("Save Current Injector Text as Preset:");
    ImGui::SetNextItemWidth(250);
    ImGui::InputTextWithHint("##PresetName", "preset name", m_presetNameBuf, sizeof(m_presetNameBuf));
    ImGui::SameLine();
    if (ImGui::Button("Save Preset", ImVec2(120, 0)))
        DoSavePreset();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Saved Presets:");
    if (m_presetFiles.empty()) {
        ImGui::TextDisabled("No presets found.");
    } else {
        for (size_t i = 0; i < m_presetFiles.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Button("Load", ImVec2(60, 0))) {
                auto result = FlagParser::LoadFile(m_presetsDir + "\\" + m_presetFiles[i]);
                if (result.success) {
                    std::string text;
                    for (const auto& f : result.flags) {
                        text += (f.originalName.empty() ? f.name : f.originalName) + "=" + f.value + "\n";
                    }
                    strncpy_s(m_injectBuf, text.c_str(), sizeof(m_injectBuf) - 1);
                    m_statusMsg = "Loaded preset: " + m_presetFiles[i];
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                fs::remove(m_presetsDir + "\\" + m_presetFiles[i]);
                m_presetsScanned = false;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(m_presetFiles[i].c_str());
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Refresh List"))
        m_presetsScanned = false;
}

void GhostGUI::RenderLogsTab() {
    if (ImGui::Button("Clear Logs"))
        m_logs.clear();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_logAutoScroll);
    ImGui::SameLine();
    ImGui::Checkbox("Log to file", &m_logToFile);
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu lines)", m_logs.size());

    ImGui::Spacing();

    float logHeight = ImGui::GetContentRegionAvail().y - 10;
    ImGui::BeginChild("##LogScroll", ImVec2(0, logHeight), ImGuiChildFlags_Borders);

    for (const auto& line : m_logs) {
        bool isError = line.find("FAILED") != std::string::npos || line.find("error") != std::string::npos || line.find("failed") != std::string::npos;
        bool isOk = line.find(" OK") != std::string::npos || line.find("Success") != std::string::npos || line.find("Attached") != std::string::npos || line.find("Injected") != std::string::npos;

        if (isError) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
        } else if (isOk) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        }

        ImGui::TextUnformatted(line.c_str());

        if (isError || isOk)
            ImGui::PopStyleColor();
    }

    if (m_logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
}

void GhostGUI::RenderSettingsTab() {
    ImGui::TextUnformatted("Automation");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Checkbox("Auto-attach to RobloxPlayerBeta.exe", &m_autoAttach);
    ImGui::TextDisabled("  Polls every 2 seconds for the Roblox process");

    ImGui::Spacing();

    ImGui::Checkbox("Auto-inject saved FFlags on attach", &m_autoInject);
    ImGui::TextDisabled("  Waits for flags to initialize, then injects repeatedly until they stick");

    ImGui::Spacing();

    ImGui::Checkbox("Auto-read FFlag values", &m_autoRead);
    ImGui::TextDisabled("  Continuously reads visible flag values while attached");
    if (m_autoRead) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::SliderFloat("##ReadInterval", &m_autoReadInterval, 0.5f, 10.0f, "%.1fs");
        ImGui::SameLine();
        ImGui::TextDisabled("interval");
    }

    ImGui::Spacing();

    ImGui::Checkbox("Auto-update offsets on version mismatch", &m_autoUpdateOffsets);
    ImGui::TextDisabled("  Fetches latest offsets automatically when Roblox has updated");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Streamproof");
    ImGui::Spacing();

    if (ImGui::Checkbox("Hide from screen capture (OBS, Discord, etc.)", &m_streamproof)) {
        if (m_hwnd) {
            if (m_streamproof) {
                // WDA_EXCLUDEFROMCAPTURE = 0x11 (Windows 10 2004+)
                SetWindowDisplayAffinity(m_hwnd, 0x11);
                AddLog("[SETTINGS] Streamproof enabled - window hidden from capture");
            } else {
                SetWindowDisplayAffinity(m_hwnd, 0x00);
                AddLog("[SETTINGS] Streamproof disabled");
            }
        }
    }
    ImGui::TextDisabled("  Window will appear black in OBS/Discord screen share");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Offsets");
    ImGui::Spacing();

    {
        std::string ov = m_flags.GetExpectedVersion();
        const char* ovStr = m_fetchingOffsets.load()           ? "fetching..."
                          : (ov.empty() || ov == "unknown")    ? "no offsets loaded"
                                                               : ov.c_str();
        ImGui::TextDisabled("  Loaded: %s  (%zu flags)", ovStr, m_flags.GetTotalCount());
    }

    if (!m_flags.IsVersionMatch() && !m_flags.GetVersionMismatchMsg().empty())
        ImGui::TextDisabled("  %s", m_flags.GetVersionMismatchMsg().c_str());

    if (m_fetchingOffsets.load()) {
        ImGui::BeginDisabled();
        ImGui::Button("Downloading...", ImVec2(220, 0));
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Update Offsets from API", ImVec2(220, 0))) {
            AddLog("[UPDATE] Starting offset update...");
            StartAutoFetchOffsets();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Check Version", ImVec2(140, 0))) {
        std::string latestVer;
        if (m_flags.FetchLatestVersion(latestVer)) {
            if (latestVer == m_flags.GetExpectedVersion())
                m_statusMsg = "Offsets are up to date";
            else
                m_statusMsg = "New version available: " + latestVer;
        } else {
            m_statusMsg = "Failed to check version";
        }
    }
    ImGui::TextDisabled("  Downloads latest offsets from robloxoffsets.com");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Config");
    ImGui::Spacing();

    if (fs::exists(m_configPath)) {
        ImGui::TextDisabled("Config: %s", m_configPath.c_str());
        if (ImGui::Button("Delete Config", ImVec2(140, 0))) {
            fs::remove(m_configPath);
            AddLog("[CONFIG] Deleted config file");
        }
    } else {
        ImGui::TextDisabled("No config saved. Use 'Save as Config' in the Injector tab.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Data folder: %s", m_dataDir.c_str());
    if (ImGui::Button("Open Data Folder", ImVec2(160, 0))) {
        ShellExecuteA(nullptr, "open", m_dataDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void GhostGUI::DoAttach() {
    if (m_proc.Attach(L"RobloxPlayerBeta.exe")) {
        m_flags.CheckVersion();
        m_offsetUpdateTriggered = false;
        m_statusMsg = "Attached (PID: " + std::to_string(m_proc.GetProcessId()) + ")";
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
        m_statusMsg = "Attached by PID: " + std::to_string(pid);
        m_needsRefresh = true;
        m_pidBuf[0] = '\0';
    } else {
        m_statusMsg = "Failed to attach to PID " + std::to_string(pid);
    }
}

int GhostGUI::InjectFlags(const std::string& text, bool silent) {
    if (!m_proc.IsAttached())
        return 0;

    auto result = FlagParser::Parse(text);
    if (!result.success)
        return 0;

    int success = 0, fail = 0;
    std::vector<std::string> notFound;

    for (const auto& flag : result.flags) {
        const FFlagEntry* entry = m_flags.FindFlag(flag.name);
        if (!entry) {
            notFound.push_back(flag.originalName.empty() ? flag.name : flag.originalName);
            fail++;
            continue;
        }

        if (m_flags.WriteFlagValue(flag.name, flag.value, flag.originalName))
            success++;
        else
            fail++;
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

    if (!m_attached) {
        m_injectStatus = "Not attached to Roblox. Attach first.";
        return;
    }

    std::string text(m_injectBuf);
    if (text.empty()) {
        m_injectStatus = "Nothing to inject.";
        return;
    }

    int ok = InjectFlags(text, false);
    m_injectSuccessCount = ok;
}

void GhostGUI::DoLoadFile() {
    OPENFILENAMEA ofn = {};
    char filePath[MAX_PATH] = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "JSON Files\0*.json\0Text Files\0*.txt\0All Files\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameA(&ofn)) {
        auto result = FlagParser::LoadFile(filePath);
        if (result.success) {
            std::string text;
            for (const auto& f : result.flags)
                text += (f.originalName.empty() ? f.name : f.originalName) + "=" + f.value + "\n";
            strncpy_s(m_injectBuf, text.c_str(), sizeof(m_injectBuf) - 1);
            m_statusMsg = "Loaded file";
            AddLog("[FILE] Loaded " + std::to_string(result.flags.size()) + " flags");
        } else {
            m_injectStatus = "Failed to load file";
            m_injectErrors = result.errors;
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
        m_statusMsg = "Saved preset: " + name;
        m_presetsScanned = false;
    }
}

void GhostGUI::SaveConfig() {
    std::string text(m_injectBuf);
    if (text.empty()) return;
    std::ofstream file(m_configPath);
    if (file.is_open()) file << text;
}

void GhostGUI::LoadConfig() {
    if (!fs::exists(m_configPath)) return;
    std::ifstream file(m_configPath);
    if (!file.is_open()) return;
    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();
    if (!content.empty()) {
        strncpy_s(m_injectBuf, content.c_str(), sizeof(m_injectBuf) - 1);
        AddLog("[CONFIG] Loaded saved flags from " + m_configPath);
    }
}
