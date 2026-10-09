#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <sstream>
#include <cstdio>

namespace fs = std::filesystem;

struct DiscordFlavor {
    std::wstring name;
    std::wstring procName;
    std::wstring localDir;
};

std::vector<DiscordFlavor> g_flavors;
DWORD g_launchedPid = 0;
std::wstring g_launchedProcName;
std::vector<fs::path> g_injectDlls;
volatile bool g_running = true;
volatile bool g_exiting = false;

std::vector<DiscordFlavor> GetFlavors() {
    wchar_t* localAppData = nullptr;
    _wdupenv_s(&localAppData, nullptr, L"LOCALAPPDATA");
    std::wstring base = localAppData ? localAppData : L"";
    free(localAppData);
    return {
        { L"Discord (Stable)", L"Discord.exe",       base + L"\\Discord" },
        { L"Discord Canary",   L"DiscordCanary.exe", base + L"\\DiscordCanary" },
        { L"Discord PTB",      L"DiscordPTB.exe",    base + L"\\DiscordPTB" }
    };
}

struct ProcInfo {
    DWORD pid;
    std::wstring name;
    std::wstring fullPath;
};

bool IsProcessRunning(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD exitCode = 0;
    BOOL ok = GetExitCodeProcess(h, &exitCode);
    CloseHandle(h);
    return ok && exitCode == STILL_ACTIVE;
}

std::wstring GetProcessPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH * 2]{};
    DWORD size = sizeof(buf) / sizeof(wchar_t);
    std::wstring result;
    if (QueryFullProcessImageNameW(h, 0, buf, &size)) {
        result = buf;
    }
    CloseHandle(h);
    return result;
}

std::vector<ProcInfo> FindRunningDiscords() {
    std::vector<ProcInfo> result;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            for (auto& f : g_flavors) {
                if (_wcsicmp(pe.szExeFile, f.procName.c_str()) == 0) {
                    ProcInfo info;
                    info.pid = pe.th32ProcessID;
                    info.name = pe.szExeFile;
                    info.fullPath = GetProcessPath(pe.th32ProcessID);
                    result.push_back(info);
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return result;
}

DWORD FindProcessByName(const std::wstring& procName) {
    DWORD pid = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, procName.c_str()) == 0) {
                pid = pe.th32ProcessID; break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return pid;
}

bool KillPid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return false;
    bool ok = TerminateProcess(h, 0);
    CloseHandle(h);
    return ok;
}

void KillByName(const std::wstring& procName) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, procName.c_str()) == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) { TerminateProcess(h, 0); CloseHandle(h); }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

std::vector<fs::path> ScanDllsNextToExe() {
    std::vector<fs::path> dlls;
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path toolDir = fs::path(exePath).parent_path();
    if (!fs::exists(toolDir)) return dlls;
    for (auto& e : fs::directory_iterator(toolDir)) {
        if (!e.is_regular_file()) continue;
        auto ext = e.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext == L".dll") dlls.push_back(e.path());
    }
    std::sort(dlls.begin(), dlls.end());
    return dlls;
}

bool HasModuleFiles(const fs::path& dir) {
    try {
        for (auto& f : fs::directory_iterator(dir)) {
            if (!f.is_regular_file()) continue;
            auto ext = f.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext == L".node" || ext == L".dll") return true;
        }
    } catch (...) {}
    return false;
}

fs::path FindDiscordVoiceFolder(const std::wstring& discordRoot) {
    if (!fs::exists(discordRoot)) return {};

    std::vector<fs::path> apps;
    for (auto& e : fs::directory_iterator(discordRoot)) {
        if (!e.is_directory()) continue;
        std::wstring n = e.path().filename().wstring();
        if (n.rfind(L"app-", 0) == 0) apps.push_back(e.path());
    }
    if (apps.empty()) return {};
    std::sort(apps.begin(), apps.end());

    for (auto it = apps.rbegin(); it != apps.rend(); ++it) {
        fs::path modulesDir = *it / L"modules";
        if (!fs::exists(modulesDir) || !fs::is_directory(modulesDir)) continue;

        std::vector<fs::path> voiceDirs;
        for (auto& e : fs::directory_iterator(modulesDir)) {
            if (!e.is_directory()) continue;
            std::wstring n = e.path().filename().wstring();
            if (n.rfind(L"discord_voice", 0) == 0) {
                voiceDirs.push_back(e.path());
            }
        }
        if (voiceDirs.empty()) continue;
        std::sort(voiceDirs.begin(), voiceDirs.end());

        for (auto vit = voiceDirs.rbegin(); vit != voiceDirs.rend(); ++vit) {
            fs::path outer = *vit;

            fs::path inner = outer / L"discord_voice";
            if (fs::exists(inner) && fs::is_directory(inner)) {
                if (HasModuleFiles(inner) || fs::is_empty(inner)) {
                    return inner;
                }
            }

            if (HasModuleFiles(outer)) {
                return outer;
            }
        }
    }
    return {};
}

fs::path BackupVoiceFolder(const fs::path& voiceDir, const fs::path& toolDir) {
    std::error_code ec;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[64];
    swprintf_s(ts, L"%04d-%02d-%02d_%02d-%02d-%02d",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);

    fs::path backupRoot = toolDir / L"backup";
    fs::path backupDir  = backupRoot / (std::wstring(L"discord_voice_") + ts);

    fs::create_directories(backupDir, ec);
    if (ec) {
        std::wcout << L"[!] Khong tao duoc backup dir: "
                   << backupDir.wstring() << L"\n";
        return {};
    }

    size_t count = 0;
    uintmax_t totalBytes = 0;

    try {
        for (auto& e : fs::recursive_directory_iterator(voiceDir)) {
            if (!e.is_regular_file()) continue;
            fs::path rel = fs::relative(e.path(), voiceDir, ec);
            fs::path dst = backupDir / rel;
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(e.path(), dst, fs::copy_options::overwrite_existing, ec);
            if (ec) { ec.clear(); continue; }
            count++;
            totalBytes += e.file_size(ec);
        }
    } catch (std::exception& ex) {
        std::wcout << L"[!] Backup exception: " << ex.what() << L"\n";
        return {};
    }

    std::wcout << L"[+] Backup " << count << L" file ("
               << (totalBytes / 1024) << L" KB) -> "
               << backupDir.wstring() << L"\n";
    return backupDir;
}

std::vector<fs::path> ListBackups(const fs::path& toolDir) {
    std::vector<fs::path> list;
    fs::path backupRoot = toolDir / L"backup";
    if (!fs::exists(backupRoot) || !fs::is_directory(backupRoot)) return list;

    for (auto& e : fs::directory_iterator(backupRoot)) {
        if (!e.is_directory()) continue;
        std::wstring n = e.path().filename().wstring();
        if (n.rfind(L"discord_voice_", 0) == 0) list.push_back(e.path());
    }
    std::sort(list.begin(), list.end());
    return list;
}

bool RestoreFromBackup(const fs::path& backupDir, const fs::path& voiceDir) {
    std::error_code ec;

    if (!fs::exists(backupDir) || !fs::is_directory(backupDir)) {
        std::wcout << L"[!] Backup khong ton tai: " << backupDir.wstring() << L"\n";
        return false;
    }

    std::wcout << L"[*] Xoa discord_voice hien tai...\n";
    try {
        for (auto& e : fs::directory_iterator(voiceDir))
            fs::remove_all(e.path());
    } catch (std::exception& ex) {
        std::wcout << L"[!] Xoa fail: " << ex.what() << L"\n";
        return false;
    }

    std::wcout << L"[*] Restore tu: " << backupDir.wstring() << L"\n";
    size_t count = 0;
    try {
        for (auto& e : fs::recursive_directory_iterator(backupDir)) {
            if (!e.is_regular_file()) continue;
            fs::path rel = fs::relative(e.path(), backupDir, ec);
            fs::path dst = voiceDir / rel;
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(e.path(), dst, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::wcout << L"[!] Restore fail: " << rel.wstring() << L"\n";
                return false;
            }
            count++;
        }
    } catch (std::exception& ex) {
        std::wcout << L"[!] Restore exception: " << ex.what() << L"\n";
        return false;
    }

    std::wcout << L"[+] Restore " << count << L" file xong.\n";
    return true;
}

bool PromptRestoreMenu(const fs::path& toolDir, fs::path& outBackupPath) {
    auto backups = ListBackups(toolDir);
    if (backups.empty()) {
        std::wcout << L"[i] Chua co backup nao.\n";
        return false;
    }

    std::wcout << L"\nRestore backup? (y/n): ";
    std::wstring ans;
    std::getline(std::wcin, ans);
    if (ans.empty() || (ans[0] != L'y' && ans[0] != L'Y')) {
        return false;
    }

    std::wcout << L"\nDanh sach backup:\n";
    for (size_t i = 0; i < backups.size(); i++) {
        std::wcout << L"  " << (i + 1) << L". "
                   << backups[i].filename().wstring() << L"\n";
    }
    std::wcout << L"Chon (1-" << backups.size() << L"): ";

    std::wstring line;
    std::getline(std::wcin, line);
    try {
        int idx = std::stoi(line);
        if (idx >= 1 && idx <= (int)backups.size()) {
            outBackupPath = backups[idx - 1];
            return true;
        }
    } catch (...) {}
    std::wcout << L"[!] Lua chon khong hop le.\n";
    return false;
}

bool ReplaceModule(const fs::path& moduleSrc,
                   const fs::path& voiceDest,
                   const fs::path& toolDir)
{
    std::error_code ec;

    if (!fs::exists(moduleSrc) || !fs::is_directory(moduleSrc)) {
        std::wcout << L"[!] module\\ khong ton tai: " << moduleSrc.wstring() << L"\n";
        return false;
    }
    std::vector<fs::path> files;
    for (auto& e : fs::recursive_directory_iterator(moduleSrc))
        if (e.is_regular_file()) files.push_back(e.path());

    if (files.empty()) {
        std::wcout << L"[!] module\\ trong.\n";
        return false;
    }
    std::wcout << L"[1] Doc duoc " << files.size() << L" file tu module\\.\n";

    fs::path tempDir = fs::temp_directory_path() / L"discord_module_temp";
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);

    std::wcout << L"[2] Copy module\\ -> temp...\n";
    for (auto& f : files) {
        fs::path rel = fs::relative(f, moduleSrc, ec);
        fs::path dst = tempDir / rel;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(f, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::wcout << L"[!] Copy temp fail: " << rel.wstring() << L"\n";
            return false;
        }
    }
    std::wcout << L"    [+] Copy temp xong.\n";

    std::wcout << L"[2.5] Backup discord_voice goc...\n";
    fs::path backupPath = BackupVoiceFolder(voiceDest, toolDir);
    if (backupPath.empty()) {
        std::wcout << L"[!] Backup that bai. Huy thay module.\n";
        return false;
    }

    std::wcout << L"[3] Xoa sach discord_voice...\n";
    try {
        for (auto& e : fs::directory_iterator(voiceDest))
            fs::remove_all(e.path());
    } catch (std::exception& ex) {
        std::wcout << L"[!] Xoa fail: " << ex.what() << L"\n";
        std::wcout << L"[i] Backup: " << backupPath.wstring() << L"\n";
        return false;
    }
    std::wcout << L"    [+] Xoa xong.\n";

    std::wcout << L"[4] Dan tu temp -> discord_voice...\n";
    for (auto& f : fs::recursive_directory_iterator(tempDir)) {
        if (!f.is_regular_file()) continue;
        fs::path rel = fs::relative(f.path(), tempDir, ec);
        fs::path dst = voiceDest / rel;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(f.path(), dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::wcout << L"[!] Dan fail: " << rel.wstring() << L"\n";
            std::wcout << L"[i] Restore tu: " << backupPath.wstring() << L"\n";
            return false;
        }
    }

    fs::remove_all(tempDir, ec);
    std::wcout << L"    [+] Thay module hoan tat.\n";
    std::wcout << L"    [i] Backup: " << backupPath.wstring() << L"\n";
    return true;
}

DWORD LaunchDiscord(const std::wstring& discordRoot, const std::wstring& procName) {
    if (!fs::exists(discordRoot)) return 0;
    fs::path updateExe = fs::path(discordRoot) / L"Update.exe";
    fs::path appExe;
    std::vector<fs::path> apps;
    for (auto& e : fs::directory_iterator(discordRoot)) {
        if (e.is_directory()) {
            std::wstring n = e.path().filename().wstring();
            if (n.rfind(L"app-", 0) == 0) apps.push_back(e.path());
        }
    }
    if (apps.empty()) return 0;
    std::sort(apps.begin(), apps.end());
    fs::path latest = apps.back();
    for (auto& f : fs::directory_iterator(latest))
        if (f.path().filename().wstring() == procName) { appExe = f.path(); break; }

    std::wstring cmd;
    if (fs::exists(updateExe) && !appExe.empty())
        cmd = L"\"" + updateExe.wstring() + L"\" --processStart \"" +
              appExe.filename().wstring() + L"\"";
    else if (!appExe.empty())
        cmd = L"\"" + appExe.wstring() + L"\"";
    else return 0;

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) return 0;
    DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return pid;
}

bool InjectDLL(DWORD pid, const std::string& dllAnsi) {
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) return false;
    SIZE_T sz = dllAnsi.size() + 1;
    LPVOID pRemote = VirtualAllocEx(hProc, nullptr, sz,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemote) { CloseHandle(hProc); return false; }
    WriteProcessMemory(hProc, pRemote, dllAnsi.c_str(), sz, nullptr);
    FARPROC pLoad = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryA");
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)pLoad, pRemote, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }
    WaitForSingleObject(hThread, INFINITE);
    VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProc);
    return true;
}

std::vector<int> ParseIndexList(const std::wstring& line, size_t maxN) {
    std::vector<int> out;
    std::set<int> seen;
    std::wstringstream ss(line);
    std::wstring tok;
    while (std::getline(ss, tok, L',')) {
        while (!tok.empty() && iswspace(tok.front())) tok.erase(tok.begin());
        while (!tok.empty() && iswspace(tok.back()))  tok.pop_back();
        if (tok.empty()) continue;
        try {
            int idx = std::stoi(tok);
            if (idx >= 1 && idx <= (int)maxN && !seen.count(idx)) {
                seen.insert(idx); out.push_back(idx - 1);
            } else std::wcout << L"[!] Bo qua: " << tok << L"\n";
        } catch (...) { std::wcout << L"[!] Khong hop le: " << tok << L"\n"; }
    }
    return out;
}

bool IsAllKeyword(const std::wstring& line) {
    std::wstring t = line;
    while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
    while (!t.empty() && iswspace(t.back()))  t.pop_back();
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    return t == L"all";
}

std::wstring TrimQuotes(const std::wstring& s) {
    std::wstring t = s;
    while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
    while (!t.empty() && iswspace(t.back()))  t.pop_back();
    if (t.size() >= 2 && t.front() == L'"' && t.back() == L'"')
        t = t.substr(1, t.size() - 2);
    return t;
}

void CleanupAndExit(int code) {
    if (g_exiting) return;
    g_exiting = true;
    g_running = false;
    std::wcout << L"\n[*] Dung inject. Kill Discord da mo...\n";
    if (!g_launchedProcName.empty()) {
        for (int i = 0; i < 5; i++) { KillByName(g_launchedProcName); Sleep(200); }
        std::wcout << L"[+] Da tat " << g_launchedProcName << L"\n";
    }
    std::wcout << L"[+] Bye Freddy.\n";
    Sleep(300);
    ExitProcess(code);
}

BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT ||
        signal == CTRL_BREAK_EVENT || signal == CTRL_LOGOFF_EVENT ||
        signal == CTRL_SHUTDOWN_EVENT) {
        CleanupAndExit(0); return TRUE;
    }
    return FALSE;
}

DWORD WINAPI InjectLoop(LPVOID) {
    while (g_running && !g_exiting) {
        if (g_injectDlls.empty() || g_launchedProcName.empty()) {
            Sleep(500); continue;
        }
        std::vector<DWORD> pids;
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
            if (Process32FirstW(hSnap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, g_launchedProcName.c_str()) == 0)
                        pids.push_back(pe.th32ProcessID);
                } while (Process32NextW(hSnap, &pe));
            }
            CloseHandle(hSnap);
        }
        for (DWORD pid : pids) {
            if (!g_running) break;
            if (!IsProcessRunning(pid)) continue;
            for (auto& dll : g_injectDlls) {
                if (!g_running) break;
                std::string ansi = dll.string();
                if (InjectDLL(pid, ansi)) {
                    std::wcout << L"[+] [" << GetTickCount64() / 1000
                               << L"s] Injected " << dll.filename().wstring()
                               << L" -> PID " << pid << L"\n";
                }
            }
        }
        for (int i = 0; i < 30 && g_running; i++) Sleep(100);
    }
    return 0;
}

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Discord Tool");
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    g_flavors = GetFlavors();

    std::wcout << L"=== Discord Module Tool ===\n\n";

    auto running = FindRunningDiscords();
    std::vector<ProcInfo> toKill;
    if (!running.empty()) {
        std::wcout << L"[1] Discord dang chay:\n";
        for (size_t i = 0; i < running.size(); i++) {
            std::wcout << L"    " << (i + 1) << L". " << running[i].name
                       << L"  (PID: " << running[i].pid << L")";
            if (!running[i].fullPath.empty())
                std::wcout << L"\n        Path: " << running[i].fullPath;
            std::wcout << L"\n";
        }
        std::wcout << L"    Nhap so TT muon TAT (vd: 1 hoac 1,2). Go 'all' de tat het. Bo trong = khong tat: ";
        std::wstring line; std::getline(std::wcin, line);

        if (IsAllKeyword(line)) {
            toKill = running;
            std::wcout << L"[+] Chon TAT HET " << running.size() << L" process.\n";
        } else {
            auto idxs = ParseIndexList(line, running.size());
            for (int i : idxs) toKill.push_back(running[i]);
        }
    } else {
        std::wcout << L"[1] Khong co Discord nao dang chay.\n";
    }

    for (auto& p : toKill) {
        if (IsProcessRunning(p.pid)) {
            if (KillPid(p.pid)) {
                Sleep(100);
                bool stillAlive = IsProcessRunning(p.pid);
                if (!stillAlive) {
                    std::wcout << L"[+] Killed " << p.name
                               << L" (PID: " << p.pid << L")\n";
                } else {
                    std::wcout << L"[!] Kill request sent nhung process van chay: "
                               << p.name << L" (PID: " << p.pid << L")\n";
                }
            } else {
                std::wcout << L"[!] Kill that bai: " << p.name
                           << L" (PID: " << p.pid << L")\n";
            }
        } else {
            std::wcout << L"[i] Process da tat truoc do: " << p.name
                       << L" (PID: " << p.pid << L")\n";
        }
    }
    if (!toKill.empty()) Sleep(1500);

    std::wcout << L"\n[2] Thay module discord_voice cho Discord nao?\n";
    for (size_t i = 0; i < g_flavors.size(); i++)
        std::wcout << L"    " << (i + 1) << L". " << g_flavors[i].name << L"\n";
    std::wcout << L"    Chon (1-" << g_flavors.size() << L"): ";

    std::wstring fLine; std::getline(std::wcin, fLine);
    int fIdx = -1;
    try { fIdx = std::stoi(fLine) - 1; } catch (...) {}
    if (fIdx < 0 || fIdx >= (int)g_flavors.size()) {
        std::wcout << L"[!] Lua chon khong hop le.\n";
        std::wcout << L"Nhan Enter..."; std::wcin.get();
        return 1;
    }
    auto& flavor = g_flavors[fIdx];

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path toolDir = fs::path(exePath).parent_path();
    fs::path moduleDir = toolDir / L"module";

    fs::path voiceDir;
    bool foundVoice = false;

    std::wcout << L"\n[3] Tim discord_voice cho " << flavor.name << L"?\n";
    std::wcout << L"    (y = tu tim, n = tu nhap duong dan): ";

    std::wstring mode;
    std::getline(std::wcin, mode);

    if (!mode.empty() && (mode[0] == L'n' || mode[0] == L'N')) {
        std::wcout << L"Nhap duong dan discord_voice: ";
        std::wstring inputPath;
        std::getline(std::wcin, inputPath);
        inputPath = TrimQuotes(inputPath);
        voiceDir = fs::path(inputPath);

        if (!fs::exists(voiceDir) || !fs::is_directory(voiceDir)) {
            std::wcout << L"[!] Duong dan khong ton tai hoac khong phai folder.\n";
            std::wcout << L"Nhan Enter..."; std::wcin.get();
            return 1;
        }
        foundVoice = true;
    } else {
        std::wcout << L"\n[*] Dang tim discord_voice...\n";
        std::wcout << L"    Go 'stop' + Enter bat cu luc nao de dung va nhap tay.\n\n";

        int attempt = 0;
        while (!foundVoice && !g_exiting) {
            attempt++;
            voiceDir = FindDiscordVoiceFolder(flavor.localDir);

            if (!voiceDir.empty() && fs::exists(voiceDir) && fs::is_directory(voiceDir)) {
                std::wcout << L"    [lan " << attempt << L"] Tim thay!\n";
                foundVoice = true;
                break;
            }

            std::wcout << L"    [lan " << attempt << L"] Chua thay, thu lai sau 2s...\n";
            Sleep(2000);
        }

        if (!foundVoice) {
            std::wcout << L"[!] Da dung tim. Thoat.\n";
            std::wcout << L"Nhan Enter..."; std::wcin.get();
            return 1;
        }
    }

    std::wcout << L"[+] discord_voice: " << voiceDir.wstring() << L"\n";

    fs::path restorePath;
    if (PromptRestoreMenu(toolDir, restorePath)) {
        std::wcout << L"\n[*] Restore tu: " << restorePath.wstring() << L"\n";
        if (RestoreFromBackup(restorePath, voiceDir)) {
            std::wcout << L"[+] Restore xong.\n";
            std::wcout << L"\nTiep tuc thay module moi? (y/n): ";
            std::wstring cont;
            std::getline(std::wcin, cont);
            if (cont.empty() || (cont[0] != L'y' && cont[0] != L'Y')) {
                std::wcout << L"[i] Dung sau restore. Thoat.\n";
                std::wcout << L"Nhan Enter..."; std::wcin.get();
                return 0;
            }
        } else {
            std::wcout << L"[!] Restore fail. Tiep tuc thay module moi.\n";
        }
    }

    std::wcout << L"\n";
    if (!ReplaceModule(moduleDir, voiceDir, toolDir)) {
        std::wcout << L"[!] Thay module that bai.\n";
        std::wcout << L"Nhan Enter..."; std::wcin.get();
        return 1;
    }

    fs::path restorePath2;
    if (PromptRestoreMenu(toolDir, restorePath2)) {
        std::wcout << L"\n[*] Restore tu: " << restorePath2.wstring() << L"\n";
        if (RestoreFromBackup(restorePath2, voiceDir)) {
            std::wcout << L"[+] Restore xong. Module da ve ban backup.\n";
        } else {
            std::wcout << L"[!] Restore fail. Module van la ban moi.\n";
        }
    }

    auto dlls = ScanDllsNextToExe();
    if (dlls.empty()) {
        std::wcout << L"\n[4] [!] Khong co DLL nao canh tool.\n";
        MessageBoxW(nullptr, L"No DLL found.", L"Tool", MB_OK | MB_ICONERROR);
        std::wcout << L"Nhan Enter..."; std::wcin.get();
        return 1;
    }

    std::wcout << L"\n[4] DLL canh tool (INJECT lien tuc):\n";
    for (size_t i = 0; i < dlls.size(); i++)
        std::wcout << L"    " << (i + 1) << L". " << dlls[i].filename().wstring()
                   << L"  (" << fs::file_size(dlls[i]) << L" bytes)\n";
    std::wcout << L"    Nhap so TT DLL (vd: 1 hoac 1,2): ";

    std::wstring dllLine; std::getline(std::wcin, dllLine);
    auto dllIdxs = ParseIndexList(dllLine, dlls.size());
    if (dllIdxs.empty()) {
        std::wcout << L"[!] Chua chon DLL nao.\n";
        std::wcout << L"Nhan Enter..."; std::wcin.get();
        return 1;
    }
    g_injectDlls.clear();
    for (int i : dllIdxs) g_injectDlls.push_back(dlls[i]);

    std::wcout << L"\n    [+] DLL se inject lien tuc:\n";
    for (auto& d : g_injectDlls)
        std::wcout << L"        - " << d.filename().wstring() << L"\n";

    std::wcout << L"\n[5] Mo " << flavor.name << L"? (Enter = dong y, n = huy): ";
    std::wstring confirm; std::getline(std::wcin, confirm);
    if (!confirm.empty() && (confirm[0] == L'n' || confirm[0] == L'N')) {
        std::wcout << L"[!] Huy.\n";
        std::wcout << L"Nhan Enter..."; std::wcin.get();
        return 0;
    }

    std::wcout << L"\n[*] Dang mo " << flavor.name << L"...\n";
    g_launchedPid = LaunchDiscord(flavor.localDir, flavor.procName);
    g_launchedProcName = flavor.procName;

    if (!g_launchedPid) {
        std::wcout << L"[!] Khong mo duoc Discord.\n";
        MessageBoxW(nullptr,
            L"Injector running successfully, Please launch discord",
            L"Tool", MB_OK | MB_ICONINFORMATION);
        std::wcout << L"Nhan Enter..."; std::wcin.get();
        return 1;
    }
    std::wcout << L"[+] Launched (PID: " << g_launchedPid << L")\n";
    Sleep(3000);

    std::wcout << L"\n[6] Bat dau inject lien tuc (moi ~3s).\n";
    std::wcout << L"    DONG CMD / Ctrl+C / Enter de DUNG + TAT Discord.\n\n";

    HANDLE hThread = CreateThread(nullptr, 0, InjectLoop, nullptr, 0, nullptr);
    if (hThread) CloseHandle(hThread);

    std::wcin.get();
    CleanupAndExit(0);
    return 0;
}
