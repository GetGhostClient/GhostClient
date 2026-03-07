#pragma once
#include "process.h"
#include "fflags.h"
#include "parser.h"
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <atomic>
#include <d3d11.h>

class GhostGUI {
public:
    GhostGUI(ProcessManager& proc, FFlagManager& flags);

    void Render();
    void AddLog(const std::string& msg);

    static void ApplyTheme();
    void SetLogoTexture(ID3D11ShaderResourceView* srv, int w, int h) {
        m_logoSRV = srv; m_logoW = w; m_logoH = h;
    }

private:
    void RenderMenuBar();
    void RenderStatusBar();
    void RenderBrowserTab();
    void RenderInjectorTab();
    void RenderPresetsTab();
    void RenderLogsTab();
    void RenderSettingsTab();

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
    float m_autoAttachTimer = 0.0f;
    static constexpr float AUTO_ATTACH_INTERVAL = 2.0f;
    bool m_wasAttached = false;

    // Auto-read state
    bool m_autoRead = true;
    float m_autoReadTimer = 0.0f;
    float m_autoReadInterval = 2.0f;

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

    // Logo texture (optional, loaded from assets/logo.png)
    ID3D11ShaderResourceView* m_logoSRV = nullptr;
    int m_logoW = 0, m_logoH = 0;

    // All data stored in %LOCALAPPDATA%\GhostClient
    std::string m_dataDir;
    std::string m_configPath;
    std::string m_logPath;
    std::string m_presetsDir;
};
