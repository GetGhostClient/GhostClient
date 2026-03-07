#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

using LogCallback = std::function<void(const std::string&)>;

class ProcessManager {
public:
    ProcessManager() = default;
    ~ProcessManager();

    void SetLogCallback(LogCallback cb) { m_log = std::move(cb); }

    bool Attach(const std::wstring& processName);
    bool AttachByPid(DWORD pid, const std::wstring& moduleName = L"RobloxPlayerBeta.exe");
    void Detach();
    bool IsAttached() const;

    uintptr_t GetBaseAddress() const { return m_baseAddress; }
    DWORD GetProcessId() const { return m_processId; }

    bool ReadMemory(uintptr_t address, void* buffer, size_t size) const;
    bool WriteMemory(uintptr_t address, const void* buffer, size_t size) const;

    template<typename T>
    T Read(uintptr_t address) const {
        T value{};
        ReadMemory(address, &value, sizeof(T));
        return value;
    }

    template<typename T>
    bool Write(uintptr_t address, const T& value) const {
        return WriteMemory(address, &value, sizeof(T));
    }

    std::string ReadString(uintptr_t address, size_t maxLen = 256) const;

private:
    void Log(const std::string& msg) const { if (m_log) m_log(msg); }
    DWORD FindProcessId(const std::wstring& processName) const;
    uintptr_t FindModuleBase(DWORD pid, const std::wstring& moduleName) const;

    LogCallback m_log;
    HANDLE m_processHandle = nullptr;
    DWORD m_processId = 0;
    uintptr_t m_baseAddress = 0;
};
