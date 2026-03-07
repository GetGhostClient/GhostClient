#pragma once
#include "process.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

std::string GetAppDataDir();

struct FFlagEntry {
    std::string name;
    uintptr_t offset;
    std::string currentValue;
    bool readSuccess;
};

class FFlagManager {
public:
    FFlagManager(ProcessManager& proc);

    void SetLogCallback(LogCallback cb) { m_log = std::move(cb); }

    void RefreshAllNames();
    bool ReadFlagValue(FFlagEntry& entry);
    bool WriteFlagValue(const std::string& name, const std::string& value, const std::string& originalName = "");
    bool IsFlagInitialized(const std::string& name);

    const std::vector<FFlagEntry>& GetEntries() const { return m_entries; }
    std::vector<FFlagEntry>& GetEntries() { return m_entries; }

    const FFlagEntry* FindFlag(const std::string& name) const;
    std::vector<FFlagEntry*> Search(const std::string& query);

    size_t GetTotalCount() const { return m_entries.size(); }
    bool IsVersionMatch() const { return m_versionMatch; }
    std::string GetExpectedVersion() const {
        return m_dynamicVersion.empty() ? "unknown" : m_dynamicVersion;
    }
    const std::string& GetVersionMismatchMsg() const { return m_versionMsg; }

    void CheckVersion();

    bool FetchLatestVersion(std::string& outVersion);
    bool FetchAndUpdateOffsets();
    bool LoadCachedOffsets();

private:
    void Log(const std::string& msg) const { if (m_log) m_log(msg); }
    void ApplyParsedFlags(std::unordered_map<std::string, uintptr_t>& flags, const std::string& version);

    ProcessManager& m_proc;
    LogCallback m_log;
    std::vector<FFlagEntry> m_entries;
    std::unordered_map<std::string, size_t> m_nameIndex;
    bool m_versionMatch = false;
    std::string m_versionMsg;
    std::string m_dynamicVersion;
};
