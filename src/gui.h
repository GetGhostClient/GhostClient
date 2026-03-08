#pragma once
#include "process.h"
#include "fflags.h"
#include "parser.h"
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <atomic>

class GhostGUI {
public:
    GhostGUI(ProcessManager& proc, FFlagManager& flags);

    void Render();
    void AddLog(const std::string& msg);

    static void ApplyTheme();

private:
    void RenderTitleBar();
    void RenderSidebar();
    void RenderStatusBar();
    void RenderPageBrowser();
    void RenderPageInjector();
    void RenderPagePresets();
    void RenderPageLogs();
    void RenderPageSettings();

    enum class Page { Browser, Injector, Presets, Logs, Settings };
    Page m_activePage = Page::Browser;

    void DoAttach();
    void DoAttachByPid();
    void DoInject();
    void DoLoadFile();
    void DoSavePreset();

    void SaveConfig();
    void LoadConfig();
    int InjectFlags(const std::string& text, bool silent = false);

    ProcessManager& m_proc;
    FFlagManager& m_flags;

    // Browser state
    char m_searchBuf[256] = {};
    std::vector<FFlagEntry*> m_filteredFlags;
    bool m_needsRefresh = true;
    FlagType m_typeFilter = FlagType::Unknown; // Unknown = show all
    int  m_sortCol = 1;   // 0=type, 1=name, 2=offset, 3=value
    bool m_sortAsc = true;

    // Copy flash indicator
    float       m_copyFlashTimer = 0.0f;
    std::string m_copyFlashLabel; // label of what was copied

    // PID attach state
    char m_pidBuf[16] = {};

    // Injector state
    char m_injectBuf[1024 * 64] = {};
    std::string m_injectStatus;
    std::vector<std::string> m_injectErrors;
    int m_injectSuccessCount = 0;

    // Preset state
    char m_presetNameBuf[128] = {};
    std::vector<std::string> m_presetFiles;
    bool m_presetsScanned = false;

    // Log state
    std::deque<std::string> m_logs;
    static constexpr size_t MAX_LOGS = 500;
    bool m_logAutoScroll = true;
    bool m_logToFile = true;

    // Auto-attach state
    bool m_autoAttach = true;
    bool m_autoInject = true;
    bool m_wasAttached = false;
    static constexpr float AUTO_ATTACH_INTERVAL = 2.0f; // kept for inject-retry reuse

    // Watcher thread: detects Roblox spawn at ~100ms latency, no frame-timer polling
    std::atomic<bool> m_robloxDetected{ false };
    std::atomic<bool> m_watcherRunning{ false };
    HANDLE m_watcherThread = nullptr;
    void StartWatcherThread();
    void StopWatcherThread();

    // Auto-read state
    bool m_autoRead = true;
    float m_autoReadTimer = 0.0f;
    float m_autoReadInterval = 2.0f;

    // Value cache flush state
    bool  m_valueCacheDirty = false;
    float m_valueCacheTimer = 0.0f;

    // Auto-update offsets on mismatch
    bool m_autoUpdateOffsets = true;
    bool m_offsetUpdateTriggered = false;

    // Re-injection state: wait for flags to initialize, then inject repeatedly
    bool m_pendingInject = false;
    float m_injectDelayTimer = 0.0f;
    int m_injectAttempts = 0;
    static constexpr int MAX_INJECT_ATTEMPTS = 15;
    static constexpr float INJECT_RETRY_INTERVAL = 2.0f;

    // Streamproof
    bool m_streamproof = false;
    HWND m_hwnd = nullptr;

    // General state
    bool m_attached = false;
    std::string m_statusMsg;
    std::chrono::steady_clock::time_point m_lastFrame;

    // Background offset auto-fetch
    std::atomic<bool> m_fetchingOffsets{ false };
    std::atomic<bool> m_fetchOffsetsDone{ false };
    std::atomic<bool> m_fetchOffsetsSuccess{ false };
    HANDLE m_fetchThread = nullptr;
    void StartAutoFetchOffsets();

    // Background FVariables type-stamp (fires after offsets are loaded)
    HANDLE m_fvarsThread = nullptr;
    void StartFetchFVariables();

    // All data stored in %LOCALAPPDATA%\GhostClient
    std::string m_dataDir;
    std::string m_configPath;
    std::string m_logPath;
    std::string m_presetsDir;
};
