#include "fflags.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>

#include <Windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

FFlagManager::FFlagManager(ProcessManager& proc)
    : m_proc(proc)
{
    if (!LoadCachedOffsets())
        RefreshAllNames();
}

void FFlagManager::RefreshAllNames() {
    m_entries.clear();
    m_nameIndex.clear();
    Log("[OFFSETS] No cached offsets found. Use Settings > Update Offsets to download them.");
}

bool FFlagManager::IsFlagInitialized(const std::string& name) {
    if (!m_proc.IsAttached())
        return false;

    auto it = m_nameIndex.find(name);
    if (it == m_nameIndex.end())
        return false;

    FFlagEntry& entry = m_entries[it->second];
    uintptr_t base = m_proc.GetBaseAddress();
    uintptr_t addr = base + entry.offset;

    // Check if the 8 bytes at the flag address and the 8 bytes after contain
    // any pointer-like values (high addresses starting with 0x7FF...).
    // If so, the FVariable object exists and has been initialized.
    uint64_t qwords[2] = {};
    m_proc.ReadMemory(addr, &qwords[0], sizeof(uint64_t));
    m_proc.ReadMemory(addr + 8, &qwords[1], sizeof(uint64_t));

    // A pointer into Roblox's address space will be > 0x10000
    // A completely zeroed region means uninitialized
    return qwords[0] != 0 || qwords[1] != 0;
}

bool FFlagManager::ReadFlagValue(FFlagEntry& entry) {
    if (!m_proc.IsAttached()) {
        entry.readSuccess = false;
        return false;
    }

    uintptr_t base = m_proc.GetBaseAddress();
    uintptr_t addr = base + entry.offset;

    int32_t rawValue = 0;
    if (!m_proc.ReadMemory(addr, &rawValue, sizeof(rawValue))) {
        entry.readSuccess = false;
        entry.currentValue = "<read error>";
        return false;
    }

    entry.currentValue = std::to_string(rawValue);
    entry.readSuccess = true;
    return true;
}

bool FFlagManager::WriteFlagValue(const std::string& name, const std::string& value, const std::string& originalName) {
    if (!m_proc.IsAttached()) {
        Log("[WRITE] Not attached");
        return false;
    }

    auto it = m_nameIndex.find(name);
    if (it == m_nameIndex.end()) {
        Log("[WRITE] Flag not found in offsets: " + name);
        return false;
    }

    FFlagEntry& entry = m_entries[it->second];
    uintptr_t base = m_proc.GetBaseAddress();
    uintptr_t addr = base + entry.offset;

    std::string prefix;
    if (!originalName.empty() && originalName.size() > name.size()) {
        prefix = originalName.substr(0, originalName.size() - name.size());
    }

    std::ostringstream addrStr;
    addrStr << "0x" << std::hex << std::uppercase << addr;

    // Read the value before writing for diagnostics
    int32_t beforeVal = 0;
    m_proc.ReadMemory(addr, &beforeVal, sizeof(beforeVal));

    try {
        bool writeOk = false;
        std::string writeDesc;

        if (value == "true" || value == "True") {
            int32_t v = 1;
            writeOk = m_proc.WriteMemory(addr, &v, sizeof(v));
            writeDesc = "true (1)";
        } else if (value == "false" || value == "False") {
            int32_t v = 0;
            writeOk = m_proc.WriteMemory(addr, &v, sizeof(v));
            writeDesc = "false (0)";
        } else {
            bool isNumeric = !value.empty();
            bool hasDecimal = false;
            for (char c : value) {
                if (c == '.') { hasDecimal = true; continue; }
                if (!std::isdigit(c) && c != '-' && c != '+') {
                    isNumeric = false;
                    break;
                }
            }

            if (!isNumeric) {
                Log("[WRITE] " + name + " = \"" + value + "\" - unsupported string value");
                return false;
            }

            if (hasDecimal || prefix == "FFloat") {
                float fVal = std::stof(value);
                writeOk = m_proc.WriteMemory(addr, &fVal, sizeof(fVal));
                writeDesc = value + " (float)";
            } else {
                int32_t intVal = static_cast<int32_t>(std::stoll(value));
                writeOk = m_proc.WriteMemory(addr, &intVal, sizeof(intVal));
                writeDesc = value + " (int32: " + std::to_string(intVal) + ")";
            }
        }

        // Read back to verify
        int32_t afterVal = 0;
        m_proc.ReadMemory(addr, &afterVal, sizeof(afterVal));

        std::ostringstream logMsg;
        logMsg << "[WRITE] " << name << " = " << writeDesc << " at " << addrStr.str();
        if (writeOk) {
            logMsg << " OK";
            if (afterVal == beforeVal && value != std::to_string(beforeVal)) {
                logMsg << " (WARNING: read-back unchanged, before=" << beforeVal << " after=" << afterVal << ")";
            } else {
                logMsg << " (before=" << beforeVal << " after=" << afterVal << ")";
            }
        } else {
            logMsg << " FAILED";
        }
        Log(logMsg.str());

        return writeOk;
    } catch (const std::exception& e) {
        Log("[WRITE] " + name + " exception: " + e.what());
        return false;
    } catch (...) {
        Log("[WRITE] " + name + " unknown exception");
        return false;
    }
}

const FFlagEntry* FFlagManager::FindFlag(const std::string& name) const {
    auto it = m_nameIndex.find(name);
    if (it == m_nameIndex.end())
        return nullptr;
    return &m_entries[it->second];
}

std::vector<FFlagEntry*> FFlagManager::Search(const std::string& query) {
    std::vector<FFlagEntry*> results;
    if (query.empty()) {
        for (auto& e : m_entries)
            results.push_back(&e);
        return results;
    }

    std::string lowerQuery = query;
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

    for (auto& e : m_entries) {
        std::string lowerName = e.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerName.find(lowerQuery) != std::string::npos) {
            results.push_back(&e);
        }
    }

    return results;
}

void FFlagManager::CheckVersion() {
    m_versionMatch = false;
    m_versionMsg.clear();

    if (!m_proc.IsAttached())
        return;

    DWORD pid = m_proc.GetProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        m_versionMatch = true;
        return;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    std::wstring exePath;

    if (Module32FirstW(snapshot, &me)) {
        do {
            if (std::wstring(me.szModule) == L"RobloxPlayerBeta.exe") {
                exePath = me.szExePath;
                break;
            }
        } while (Module32NextW(snapshot, &me));
    }
    CloseHandle(snapshot);

    if (exePath.empty()) {
        m_versionMatch = true;
        return;
    }

    namespace fs = std::filesystem;
    fs::path p(exePath);
    std::string parentDir = p.parent_path().filename().string();

    // Folder name may have a prefix like "WEAO-LIVE-WindowsPlayer-version-abc123"
    std::string expectedVer = GetExpectedVersion();
    if (expectedVer == "unknown" || expectedVer.empty()) {
        m_versionMatch = false;
        m_versionMsg = "No offsets loaded. Use Settings > Update Offsets to download them.";
        Log("[VERSION] No offsets loaded");
    } else if (parentDir.find(expectedVer) != std::string::npos) {
        m_versionMatch = true;
        Log("[VERSION] Match: " + parentDir);
    } else {
        m_versionMatch = false;
        m_versionMsg = "Roblox version mismatch! Running: " + parentDir + ", Offsets for: " + expectedVer;
        Log("[VERSION] MISMATCH - running: " + parentDir + " expected: " + expectedVer);
        Log("[VERSION] FFlag offsets may be incorrect. Use Settings > Update Offsets to fetch latest.");
    }
}

// ---- HTTP helpers for auto-update ----

static std::string WinHttpGet(const std::wstring& host, const std::wstring& path) {
    std::string result;

    HINTERNET hSession = WinHttpOpen(
        L"GhostClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return result; }

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            std::vector<char> buf(bytesAvailable);
            DWORD bytesRead = 0;
            if (WinHttpReadData(hRequest, buf.data(), bytesAvailable, &bytesRead))
                result.append(buf.data(), bytesRead);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

bool FFlagManager::FetchLatestVersion(std::string& outVersion) {
    Log("[UPDATE] Fetching latest Roblox version from offsets API...");
    std::string body = WinHttpGet(
        L"offsets.ntgetwritewatch.workers.dev",
        L"/version");

    while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' '))
        body.pop_back();
    while (!body.empty() && (body.front() == '\n' || body.front() == '\r' || body.front() == ' '))
        body.erase(body.begin());

    if (body.empty() || body.find("version-") == std::string::npos) {
        Log("[UPDATE] Failed to fetch version");
        return false;
    }

    outVersion = body;
    Log("[UPDATE] Latest version: " + outVersion);
    return true;
}

// Parses a FFlags.hpp body into a flag map and version string.
static bool ParseFlagsHpp(const std::string& body,
    std::unordered_map<std::string, uintptr_t>& outFlags,
    std::string& outVersion)
{
    std::istringstream stream(body);
    std::string line;
    bool inFFlagOffsets = false;
    bool passedFFlagOffsets = false;

    while (std::getline(stream, line)) {
        if (outVersion.empty()) {
            auto vpos = line.find("version-");
            if (vpos != std::string::npos) {
                auto end = line.find_first_of(" \t\r\n\"'", vpos);
                outVersion = line.substr(vpos, end == std::string::npos ? std::string::npos : end - vpos);
            }
        }

        if (line.find("namespace FFlagOffsets") != std::string::npos) {
            inFFlagOffsets = true;
            continue;
        }
        if (inFFlagOffsets && line.find('}') != std::string::npos) {
            inFFlagOffsets = false;
            passedFFlagOffsets = true;
            continue;
        }
        if (inFFlagOffsets)
            continue;

        auto eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;

        auto nameStart = line.find("uintptr_t ");
        if (nameStart == std::string::npos) continue;
        nameStart += 10;

        while (nameStart < eqPos && line[nameStart] == ' ') nameStart++;
        auto nameEnd = eqPos;
        while (nameEnd > nameStart && line[nameEnd - 1] == ' ') nameEnd--;

        std::string flagName = line.substr(nameStart, nameEnd - nameStart);
        if (flagName.empty()) continue;

        auto hexPos = line.find("0x", eqPos);
        if (hexPos == std::string::npos) hexPos = line.find("0X", eqPos);
        if (hexPos == std::string::npos) continue;

        uintptr_t val = 0;
        try {
            val = static_cast<uintptr_t>(std::stoull(line.substr(hexPos), nullptr, 16));
        } catch (...) {
            continue;
        }

        if (passedFFlagOffsets || (!inFFlagOffsets && flagName != "FFlagList" && flagName != "ValueGetSet" && flagName != "FlagToValue")) {
            outFlags[flagName] = val;
        }
    }

    return !outFlags.empty();
}

// Applies a parsed flag map into m_entries / m_nameIndex and updates m_dynamicVersion.
void FFlagManager::ApplyParsedFlags(std::unordered_map<std::string, uintptr_t>& flags,
    const std::string& version)
{
    m_entries.clear();
    m_nameIndex.clear();
    m_entries.reserve(flags.size());

    for (const auto& [flagName, offset] : flags) {
        FFlagEntry entry;
        entry.name = flagName;
        entry.offset = offset;
        entry.currentValue = "";
        entry.readSuccess = false;
        m_entries.push_back(std::move(entry));
    }

    std::sort(m_entries.begin(), m_entries.end(),
        [](const FFlagEntry& a, const FFlagEntry& b) {
            return a.name < b.name;
        });

    for (size_t i = 0; i < m_entries.size(); ++i)
        m_nameIndex[m_entries[i].name] = i;

    if (!version.empty())
        m_dynamicVersion = version;
}

bool FFlagManager::LoadCachedOffsets() {
    std::string cachePath = GetAppDataDir() + "\\fflag_cache.hpp";
    std::ifstream file(cachePath);
    if (!file.is_open())
        return false;

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string body = ss.str();
    if (body.empty())
        return false;

    std::unordered_map<std::string, uintptr_t> flags;
    std::string version;
    if (!ParseFlagsHpp(body, flags, version)) {
        Log("[CACHE] Cache file found but failed to parse - using built-in offsets");
        return false;
    }

    ApplyParsedFlags(flags, version);
    m_versionMatch = true;
    m_versionMsg.clear();
    Log("[CACHE] Loaded " + std::to_string(m_entries.size()) + " flags from cache (version: " + m_dynamicVersion + ")");
    return true;
}

bool FFlagManager::FetchAndUpdateOffsets() {
    Log("[UPDATE] Fetching latest FFlag offsets...");
    std::string body = WinHttpGet(
        L"offsets.ntgetwritewatch.workers.dev",
        L"/FFlags.hpp");

    if (body.empty()) {
        Log("[UPDATE] Failed to fetch FFlags.hpp");
        return false;
    }

    std::unordered_map<std::string, uintptr_t> newFlags;
    std::string newVersion;
    if (!ParseFlagsHpp(body, newFlags, newVersion)) {
        Log("[UPDATE] Parsed 0 flags - update failed");
        return false;
    }

    std::string cachePath = GetAppDataDir() + "\\fflag_cache.hpp";
    std::ofstream cache(cachePath);
    if (cache.is_open()) {
        cache << body;
        Log("[UPDATE] Saved offset cache to " + cachePath);
    }

    Log("[UPDATE] Parsed " + std::to_string(newFlags.size()) + " flags (version: " + newVersion + ")");

    ApplyParsedFlags(newFlags, newVersion);

    m_versionMatch = true;
    m_versionMsg.clear();
    Log("[UPDATE] Offset update complete - " + std::to_string(m_entries.size()) + " flags loaded");
    return true;
}
