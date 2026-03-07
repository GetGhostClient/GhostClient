#include "process.h"
#include <sstream>
#include <iomanip>

ProcessManager::~ProcessManager() {
    Detach();
}

bool ProcessManager::Attach(const std::wstring& processName) {
    Detach();

    m_processId = FindProcessId(processName);
    if (m_processId == 0) {
        Log("[ATTACH] Process not found");
        return false;
    }

    Log("[ATTACH] Found PID: " + std::to_string(m_processId));

    m_processHandle = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
        FALSE, m_processId);
    if (!m_processHandle) {
        DWORD err = GetLastError();
        Log("[ATTACH] OpenProcess failed (error " + std::to_string(err) + ") - run as Administrator?");
        m_processId = 0;
        return false;
    }

    m_baseAddress = FindModuleBase(m_processId, processName);
    if (m_baseAddress == 0) {
        Log("[ATTACH] Failed to find module base");
        Detach();
        return false;
    }

    std::ostringstream ss;
    ss << "[ATTACH] Success - base: 0x" << std::hex << std::uppercase << m_baseAddress;
    Log(ss.str());
    return true;
}

bool ProcessManager::AttachByPid(DWORD pid, const std::wstring& moduleName) {
    Detach();

    m_processId = pid;
    Log("[ATTACH] Attaching to PID: " + std::to_string(pid));

    m_processHandle = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!m_processHandle) {
        DWORD err = GetLastError();
        Log("[ATTACH] OpenProcess failed (error " + std::to_string(err) + ") - run as Administrator?");
        m_processId = 0;
        return false;
    }

    m_baseAddress = FindModuleBase(pid, moduleName);
    if (m_baseAddress == 0) {
        Log("[ATTACH] Failed to find module base for PID " + std::to_string(pid));
        Detach();
        return false;
    }

    std::ostringstream ss;
    ss << "[ATTACH] Success - PID: " << pid << " base: 0x" << std::hex << std::uppercase << m_baseAddress;
    Log(ss.str());
    return true;
}

void ProcessManager::Detach() {
    if (m_processHandle) {
        CloseHandle(m_processHandle);
        m_processHandle = nullptr;
        Log("[DETACH] Closed handle for PID " + std::to_string(m_processId));
    }
    m_processId = 0;
    m_baseAddress = 0;
}

bool ProcessManager::IsAttached() const {
    if (!m_processHandle)
        return false;

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(m_processHandle, &exitCode))
        return false;

    return exitCode == STILL_ACTIVE;
}

bool ProcessManager::ReadMemory(uintptr_t address, void* buffer, size_t size) const {
    if (!m_processHandle)
        return false;

    SIZE_T bytesRead = 0;
    return ReadProcessMemory(m_processHandle, reinterpret_cast<LPCVOID>(address), buffer, size, &bytesRead)
        && bytesRead == size;
}

bool ProcessManager::WriteMemory(uintptr_t address, const void* buffer, size_t size) const {
    if (!m_processHandle)
        return false;

    // Try direct write first
    SIZE_T bytesWritten = 0;
    if (WriteProcessMemory(m_processHandle, reinterpret_cast<LPVOID>(address), buffer, size, &bytesWritten)
        && bytesWritten == size) {
        return true;
    }

    // If direct write fails, try changing page protection
    DWORD oldProtect = 0;
    if (!VirtualProtectEx(m_processHandle, reinterpret_cast<LPVOID>(address), size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        DWORD err = GetLastError();
        Log("[WRITE] VirtualProtectEx failed at 0x" + std::to_string(address) + " (error " + std::to_string(err) + ")");
        return false;
    }

    bool ok = WriteProcessMemory(m_processHandle, reinterpret_cast<LPVOID>(address), buffer, size, &bytesWritten)
        && bytesWritten == size;

    VirtualProtectEx(m_processHandle, reinterpret_cast<LPVOID>(address), size, oldProtect, &oldProtect);

    if (!ok) {
        DWORD err = GetLastError();
        Log("[WRITE] WriteProcessMemory failed at 0x" + std::to_string(address) + " (error " + std::to_string(err) + ")");
    }

    return ok;
}

std::string ProcessManager::ReadString(uintptr_t address, size_t maxLen) const {
    std::vector<char> buf(maxLen + 1, 0);
    if (!ReadMemory(address, buf.data(), maxLen))
        return "";

    buf[maxLen] = '\0';
    return std::string(buf.data());
}

DWORD ProcessManager::FindProcessId(const std::wstring& processName) const {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (processName == entry.szExeFile) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return pid;
}

uintptr_t ProcessManager::FindModuleBase(DWORD pid, const std::wstring& moduleName) const {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        Log("[MODULE] CreateToolhelp32Snapshot failed (error " + std::to_string(err) + ")");
        return 0;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    uintptr_t base = 0;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (moduleName == entry.szModule) {
                base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return base;
}
