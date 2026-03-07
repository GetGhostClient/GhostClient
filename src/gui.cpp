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

#pragma comment(lib, "dwmapi.lib")

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
    AddLog("GhostClient started");
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
    ImVec4* colors = style.Colors;

    ImVec4 black       = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    ImVec4 darkGray1   = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    ImVec4 darkGray2   = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    ImVec4 midGray     = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    ImVec4 lightGray   = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    ImVec4 white       = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    ImVec4 dimWhite    = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    ImVec4 hoverGray   = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    ImVec4 activeGray  = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    ImVec4 transparent = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_Text]                  = white;
    colors[ImGuiCol_TextDisabled]          = lightGray;
    colors[ImGuiCol_WindowBg]              = black;
    colors[ImGuiCol_ChildBg]               = black;
    colors[ImGuiCol_PopupBg]               = darkGray1;
    colors[ImGuiCol_Border]                = midGray;
    colors[ImGuiCol_BorderShadow]          = transparent;
    colors[ImGuiCol_FrameBg]               = darkGray2;
    colors[ImGuiCol_FrameBgHovered]        = hoverGray;
    colors[ImGuiCol_FrameBgActive]         = activeGray;
    colors[ImGuiCol_TitleBg]               = darkGray1;
    colors[ImGuiCol_TitleBgActive]         = darkGray2;
    colors[ImGuiCol_TitleBgCollapsed]      = black;
    colors[ImGuiCol_MenuBarBg]             = darkGray1;
    colors[ImGuiCol_ScrollbarBg]           = black;
    colors[ImGuiCol_ScrollbarGrab]         = midGray;
    colors[ImGuiCol_ScrollbarGrabHovered]  = lightGray;
    colors[ImGuiCol_ScrollbarGrabActive]   = dimWhite;
    colors[ImGuiCol_CheckMark]             = white;
    colors[ImGuiCol_SliderGrab]            = lightGray;
    colors[ImGuiCol_SliderGrabActive]      = white;
    colors[ImGuiCol_Button]                = darkGray2;
    colors[ImGuiCol_ButtonHovered]         = hoverGray;
    colors[ImGuiCol_ButtonActive]          = activeGray;
    colors[ImGuiCol_Header]                = darkGray2;
    colors[ImGuiCol_HeaderHovered]         = hoverGray;
    colors[ImGuiCol_HeaderActive]          = activeGray;
    colors[ImGuiCol_Separator]             = midGray;
    colors[ImGuiCol_SeparatorHovered]      = lightGray;
    colors[ImGuiCol_SeparatorActive]       = white;
    colors[ImGuiCol_ResizeGrip]            = midGray;
    colors[ImGuiCol_ResizeGripHovered]     = lightGray;
    colors[ImGuiCol_ResizeGripActive]      = white;
    colors[ImGuiCol_Tab]                   = darkGray1;
    colors[ImGuiCol_TabHovered]            = hoverGray;
    colors[ImGuiCol_TabSelected]           = midGray;
    colors[ImGuiCol_TabDimmed]             = darkGray1;
    colors[ImGuiCol_TabDimmedSelected]     = darkGray2;
    colors[ImGuiCol_TableHeaderBg]         = darkGray1;
    colors[ImGuiCol_TableBorderStrong]     = midGray;
    colors[ImGuiCol_TableBorderLight]      = darkGray2;
    colors[ImGuiCol_TableRowBg]            = transparent;
    colors[ImGuiCol_TableRowBgAlt]         = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_TextSelectedBg]        = midGray;
    colors[ImGuiCol_NavHighlight]          = white;

    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 3.0f;
    style.GrabRounding      = 2.0f;
    style.TabRounding       = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.WindowPadding     = ImVec2(12, 12);
    style.FramePadding      = ImVec2(8, 4);
    style.ItemSpacing       = ImVec2(8, 6);
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 8.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.TabBorderSize     = 0.0f;
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

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_MenuBar;

    ImGui::Begin("##GhostClient", nullptr, windowFlags);

    RenderMenuBar();

    ImGui::PushFont(nullptr);
    float titleWidth = ImGui::CalcTextSize("GHOSTCLIENT").x;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - titleWidth) * 0.5f);
    ImGui::TextUnformatted("GHOSTCLIENT");
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

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

    ImGui::End();
}

void GhostGUI::RenderMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Process")) {
            if (ImGui::MenuItem("Attach by Name", nullptr, false, !m_attached)) {
                DoAttach();
            }
            ImGui::SetNextItemWidth(100);
            ImGui::InputTextWithHint("##PidInput", "PID", m_pidBuf, sizeof(m_pidBuf), ImGuiInputTextFlags_CharsDecimal);
            ImGui::SameLine();
            if (ImGui::MenuItem("Attach by PID", nullptr, false, !m_attached && m_pidBuf[0] != '\0')) {
                DoAttachByPid();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Detach", nullptr, false, m_attached)) {
                m_proc.Detach();
                m_statusMsg = "Detached";
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                PostQuitMessage(0);
            }
            ImGui::EndMenu();
        }

        float indicatorWidth = 200.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - indicatorWidth);
        if (m_attached) {
            if (m_pendingInject) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.3f, 1.0f));
                ImGui::TextUnformatted("[INJECTING...]");
                ImGui::PopStyleColor();
            } else {
                ImGui::TextUnformatted("[ATTACHED]");
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            if (m_autoAttach)
                ImGui::TextUnformatted("[WAITING...]");
            else
                ImGui::TextUnformatted("[NOT ATTACHED]");
            ImGui::PopStyleColor();
        }

        ImGui::EndMenuBar();
    }
}

void GhostGUI::RenderStatusBar() {
    ImGui::Spacing();
    ImGui::Separator();

    if (!m_flags.IsVersionMatch() && !m_flags.GetVersionMismatchMsg().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.4f, 0.1f, 1.0f));
        ImGui::TextUnformatted(m_flags.GetVersionMismatchMsg().c_str());
        ImGui::PopStyleColor();
    }

    ImGui::TextDisabled("FFlags: %zu | Version: %s | PID: %lu",
        m_flags.GetTotalCount(),
        m_flags.GetExpectedVersion().c_str(),
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
                static char valueBuf[256] = {};
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputTextWithHint("##val", "value", valueBuf, sizeof(valueBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    if (m_attached && valueBuf[0] != '\0') {
                        if (m_flags.WriteFlagValue(entry->name, valueBuf))
                            m_flags.ReadFlagValue(*entry);
                        valueBuf[0] = '\0';
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

    ImGui::TextDisabled("  Built-in offsets: %s (%zu flags)",
        m_flags.GetExpectedVersion().c_str(),
        m_flags.GetTotalCount());

    if (!m_flags.IsVersionMatch() && !m_flags.GetVersionMismatchMsg().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.4f, 0.1f, 1.0f));
        ImGui::TextWrapped("  %s", m_flags.GetVersionMismatchMsg().c_str());
        ImGui::PopStyleColor();
    }

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
