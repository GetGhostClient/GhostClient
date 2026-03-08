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
    if (!LoadCachedOffsets()) {
        // No cache — do a synchronous fetch before the window opens.
        // LoadCachedOffsets already calls LoadCachedFVariables + LoadValueCache on success.
        if (!FetchAndUpdateOffsets()) {
            RefreshAllNames();
        } else {
            // FetchAndUpdateOffsets doesn't stamp types — do it now synchronously.
            // Try local cache first (fast), fall back to network if not present.
            namespace fs = std::filesystem;
            std::string fvarsCachePath = GetAppDataDir() + "\\fvars_cache.txt";
            if (fs::exists(fvarsCachePath))
                LoadCachedFVariables();
            else
                FetchAndApplyFVariables(); // also saves the cache for next time
            LoadValueCache();
        }
    }
    // If LoadCachedOffsets succeeded it already called LoadCachedFVariables
    // + LoadValueCache internally, so nothing more to do here.
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
        // Don't clear readSuccess/currentValue — preserve the cached value
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
    Log("[UPDATE] Fetching latest Roblox version from imtheo.lol...");
    std::string body = WinHttpGet(
        L"imtheo.lol",
        L"/Offsets/FFlags.json");

    if (body.empty()) {
        Log("[UPDATE] Failed to fetch FFlags.json for version check");
        return false;
    }

    // Extract "Roblox Version" field
    const std::string key = "\"Roblox Version\"";
    auto kpos = body.find(key);
    if (kpos == std::string::npos) {
        Log("[UPDATE] Version key not found in FFlags.json");
        return false;
    }
    auto colon = body.find(':', kpos + key.size());
    if (colon == std::string::npos) return false;
    auto q1 = body.find('"', colon + 1);
    if (q1 == std::string::npos) return false;
    auto q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;

    outVersion = body.substr(q1 + 1, q2 - q1 - 1);
    if (outVersion.find("version-") == std::string::npos) {
        Log("[UPDATE] Unexpected version format: " + outVersion);
        return false;
    }

    Log("[UPDATE] Latest version: " + outVersion);
    return true;
}

// Parses FFlags.json from imtheo.lol/Offsets into a flag map and version string.
// JSON structure:
//   { "Roblox Version": "version-xxx", ...,
//     "FFlagOffsets": { "FFlagList": {...}, "FFlags": { "Name": 123456, ... } } }
//
// Uses a hand-rolled parser to avoid adding a JSON library dependency.
static bool ParseFlagsJson(const std::string& body,
    std::unordered_map<std::string, uintptr_t>& outFlags,
    std::string& outVersion)
{
    // ── Extract top-level "Roblox Version" ──────────────────────────────
    {
        const std::string vkey = "\"Roblox Version\"";
        auto kp = body.find(vkey);
        if (kp != std::string::npos) {
            auto col = body.find(':', kp + vkey.size());
            if (col != std::string::npos) {
                auto q1 = body.find('"', col + 1);
                if (q1 != std::string::npos) {
                    auto q2 = body.find('"', q1 + 1);
                    if (q2 != std::string::npos)
                        outVersion = body.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
    }

    // ── Locate the "FFlags" object inside "FFlagOffsets" ────────────────
    // We specifically want "FFlags" to skip "FFlagList"
    const std::string fkey = "\"FFlags\"";
    auto fpos = body.find(fkey);
    // Advance past outer "FFlagOffsets" if the first hit is a false match
    // (in practice "FFlags" only appears once, so this is safe)
    if (fpos == std::string::npos) {
        return false;
    }

    auto objStart = body.find('{', fpos + fkey.size());
    if (objStart == std::string::npos) return false;

    // Find the matching closing brace
    size_t depth = 1;
    size_t pos = objStart + 1;
    size_t objEnd = std::string::npos;
    while (pos < body.size() && depth > 0) {
        char c = body[pos];
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) { objEnd = pos; break; } }
        else if (c == '"') {
            // skip string contents
            ++pos;
            while (pos < body.size() && body[pos] != '"') {
                if (body[pos] == '\\') ++pos; // escape
                ++pos;
            }
        }
        ++pos;
    }

    if (objEnd == std::string::npos) {
        return false;
    }

    std::string fflagsBody = body.substr(objStart + 1, objEnd - objStart - 1);

    // ── Parse "Name": number pairs ──────────────────────────────────────
    size_t p = 0;
    while (p < fflagsBody.size()) {
        // Find next key
        auto q1 = fflagsBody.find('"', p);
        if (q1 == std::string::npos) break;
        auto q2 = fflagsBody.find('"', q1 + 1);
        if (q2 == std::string::npos) break;

        std::string name = fflagsBody.substr(q1 + 1, q2 - q1 - 1);
        p = q2 + 1;

        // Find colon, then value
        auto col = fflagsBody.find(':', p);
        if (col == std::string::npos) break;
        p = col + 1;

        // Skip whitespace
        while (p < fflagsBody.size() && (fflagsBody[p] == ' ' || fflagsBody[p] == '\t'
            || fflagsBody[p] == '\r' || fflagsBody[p] == '\n'))
            ++p;

        if (p >= fflagsBody.size()) break;

        // Parse integer value (decimal)
        bool negative = (fflagsBody[p] == '-');
        if (negative) ++p;

        if (p >= fflagsBody.size() || !std::isdigit((unsigned char)fflagsBody[p])) {
            // Skip non-numeric values (shouldn't happen but be safe)
            auto next = fflagsBody.find(',', p);
            p = (next == std::string::npos) ? fflagsBody.size() : next + 1;
            continue;
        }

        uintptr_t val = 0;
        while (p < fflagsBody.size() && std::isdigit((unsigned char)fflagsBody[p])) {
            val = val * 10 + (fflagsBody[p] - '0');
            ++p;
        }

        if (!name.empty())
            outFlags[name] = val;
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
    namespace fs = std::filesystem;

    // Remove legacy .hpp cache if it exists (old format)
    std::string legacyPath = GetAppDataDir() + "\\fflag_cache.hpp";
    if (fs::exists(legacyPath)) {
        fs::remove(legacyPath);
        Log("[CACHE] Removed legacy .hpp cache - will re-fetch");
    }

    std::string cachePath = GetAppDataDir() + "\\fflag_cache.json";
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
    if (!ParseFlagsJson(body, flags, version)) {
        Log("[CACHE] Cache file found but failed to parse - will re-fetch");
        file.close();
        fs::remove(cachePath);
        return false;
    }

    if (version.empty()) {
        Log("[CACHE] Cache has no version info - discarding, will re-fetch");
        file.close();
        fs::remove(cachePath);
        return false;
    }

    ApplyParsedFlags(flags, version);
    m_versionMatch = true;
    m_versionMsg.clear();
    Log("[CACHE] Loaded " + std::to_string(m_entries.size()) + " flags from cache (version: " + m_dynamicVersion + ")");

    // Stamp types synchronously from local cache — no network, instant
    LoadCachedFVariables();
    // Restore last-read values so booleans etc. show before attach
    LoadValueCache();

    return true;
}

// ── FVariables type stamping ─────────────────────────────────────────────────
// Parses a FVariables.txt body (lines like "[C++] DFFlagFoo" or "[Lua] FIntBar")
// and stamps the FlagType on every matching entry in m_entries.
static FlagType TypeFromPrefix(const std::string& fullName) {
    struct { const char* prefix; FlagType type; } table[] = {
        { "DFFlag",  FlagType::Bool  }, { "FFlag",   FlagType::Bool  },
        { "SFFlag",  FlagType::Bool  }, { "DFlag",   FlagType::Bool  },
        { "SFlag",   FlagType::Bool  },
        { "FFloat",  FlagType::Int   }, { "DFDouble", FlagType::Int   },
        { "DFInt",   FlagType::Int   }, { "FInt",    FlagType::Int   },
        { "SFInt",   FlagType::Int   }, { "FLog",    FlagType::Int   },
        { "DFString",FlagType::String}, { "FString", FlagType::String},
        { "SFString",FlagType::String},
    };
    for (auto& e : table) {
        size_t n = strlen(e.prefix);
        if (fullName.size() > n && fullName.substr(0, n) == e.prefix)
            return e.type;
    }
    return FlagType::Unknown;
}

// Strips the type prefix from a full flag name to get the bare name.
// e.g. "DFFlagFoo" → "Foo", "FIntBar" → "Bar"
static std::string StripTypePrefix(const std::string& fullName) {
    static const char* prefixes[] = {
        "DFDouble","DFString","SFString","DFFlag","SFFlag","DFInt","SFInt",
        "FFloat","FString","FFlag","DFlag","SFlag","FLog","FInt", nullptr
    };
    for (int i = 0; prefixes[i]; ++i) {
        size_t n = strlen(prefixes[i]);
        if (fullName.size() > n && fullName.substr(0, n) == prefixes[i])
            return fullName.substr(n);
    }
    return fullName;
}

// Parses FVariables body into a bare-name → FlagType map and stamps m_entries.
// Returns number of entries stamped.
static int ApplyFVariablesBody(const std::string& body,
    std::vector<FFlagEntry>& entries)
{
    std::unordered_map<std::string, FlagType> typeMap;
    typeMap.reserve(20000);

    std::istringstream stream(body);
    std::string line;
    while (std::getline(stream, line)) {
        auto bracket = line.find(']');
        if (bracket == std::string::npos) continue;
        std::string fullName = line.substr(bracket + 1);
        size_t start = fullName.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        fullName = fullName.substr(start);
        while (!fullName.empty() && (fullName.back() == ' ' || fullName.back() == '\r' || fullName.back() == '\n'))
            fullName.pop_back();
        FlagType ft = TypeFromPrefix(fullName);
        if (ft == FlagType::Unknown) continue;
        std::string bare = StripTypePrefix(fullName);
        if (!bare.empty()) typeMap[bare] = ft;
    }

    if (typeMap.empty()) return 0;

    int stamped = 0;
    for (auto& entry : entries) {
        auto it = typeMap.find(entry.name);
        if (it != typeMap.end()) { entry.type = it->second; ++stamped; }
    }
    return stamped;
}

bool FFlagManager::LoadCachedFVariables() {
    std::string cachePath = GetAppDataDir() + "\\fvars_cache.txt";
    std::ifstream f(cachePath);
    if (!f.is_open()) return false;
    std::ostringstream ss; ss << f.rdbuf();
    std::string body = ss.str();
    if (body.empty()) return false;

    int stamped = ApplyFVariablesBody(body, m_entries);
    if (stamped == 0) return false;

    Log("[FVARS] Loaded types from cache: " + std::to_string(stamped) + " entries stamped");
    return true;
}

void FFlagManager::FetchAndApplyFVariables() {
    Log("[FVARS] Fetching FVariables.txt from Roblox-Client-Tracker...");

    std::string body = WinHttpGet(
        L"raw.githubusercontent.com",
        L"/MaximumADHD/Roblox-Client-Tracker/refs/heads/roblox/FVariables.txt");

    if (body.empty()) {
        Log("[FVARS] Failed to fetch FVariables.txt");
        // Fall back to cache if fetch fails
        LoadCachedFVariables();
        return;
    }

    // Save to cache for next startup
    std::string cachePath = GetAppDataDir() + "\\fvars_cache.txt";
    std::ofstream cf(cachePath, std::ios::trunc);
    if (cf.is_open()) cf << body;

    int stamped = ApplyFVariablesBody(body, m_entries);
    Log("[FVARS] Stamped types for " + std::to_string(stamped) + " / "
        + std::to_string(m_entries.size()) + " entries");
}

// ── Value cache ──────────────────────────────────────────────────────────────
// Persists last-read flag values so booleans (and other values) are visible
// even before attaching or after a restart.
// Format: one entry per line — "name\tvalue\n"

void FFlagManager::SaveValueCache() {
    std::string cachePath = GetAppDataDir() + "\\value_cache.txt";
    std::ofstream f(cachePath, std::ios::trunc);
    if (!f.is_open()) return;
    for (const auto& e : m_entries) {
        if (e.readSuccess && !e.currentValue.empty())
            f << e.name << '\t' << e.currentValue << '\n';
    }
}

void FFlagManager::LoadValueCache() {
    std::string cachePath = GetAppDataDir() + "\\value_cache.txt";
    std::ifstream f(cachePath);
    if (!f.is_open()) return;

    std::unordered_map<std::string, std::string> valueMap;
    std::string line;
    while (std::getline(f, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string name = line.substr(0, tab);
        std::string val  = line.substr(tab + 1);
        // Strip trailing CR
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n'))
            val.pop_back();
        if (!name.empty() && !val.empty())
            valueMap[name] = val;
    }

    int restored = 0;
    for (auto& e : m_entries) {
        auto it = valueMap.find(e.name);
        if (it != valueMap.end()) {
            e.currentValue = it->second;
            e.readSuccess  = true;
            ++restored;
        }
    }
    if (restored > 0)
        Log("[CACHE] Restored " + std::to_string(restored) + " cached flag values");
}

bool FFlagManager::FetchAndUpdateOffsets() {
    Log("[UPDATE] Fetching latest FFlag offsets from imtheo.lol...");
    std::string body = WinHttpGet(
        L"imtheo.lol",
        L"/Offsets/FFlags.json");

    if (body.empty()) {
        Log("[UPDATE] Failed to fetch FFlags.json");
        return false;
    }

    std::unordered_map<std::string, uintptr_t> newFlags;
    std::string newVersion;
    if (!ParseFlagsJson(body, newFlags, newVersion)) {
        Log("[UPDATE] Parsed 0 flags - update failed");
        return false;
    }

    std::string cachePath = GetAppDataDir() + "\\fflag_cache.json";
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
