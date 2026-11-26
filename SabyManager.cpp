// SabyManagerGUI.cpp - графическая версия менеджера Saby (СБИС) Desktop.
// Поиск установленных версий, удаление отмеченных и установка новой версии (msi)
// на локальном или удалённом компьютере.
//
// Сборка в Visual Studio: проект "Классическое приложение Windows" (Windows
// Desktop Application) или пустой проект C++, Release, x64 или x86.
// Подсистема Windows и подключение библиотек прописаны прямо в коде (#pragma).
//
// Как это работает:
//   * поиск: реестр (раздел Uninstall, HKLM x64/x86 и загруженные профили HKU);
//     для удалённого ПК - служба RemoteRegistry (при необходимости запускается sc.exe);
//   * удаление и установка на удалённом ПК: сначала стандартными средствами
//     (WMI/DCOM, на целевой машине запускается msiexec), если не вышло - через
//     pse.exe (переименованный psexec) с ключом -r ps_SB;
//   * pse.exe ищется рядом с программой, затем в PATH.
//
// Запускать от учётной записи с правами администратора на целевом ПК.

#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#include <commctrl.h>
#include <commdlg.h>
#include <sddl.h>
#include <shellapi.h>

#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
//#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------- Настройки ----------
static const wchar_t* DEFAULT_MSI = L""; //"\\\\voip\\Install_KZ\\SBIS\\saby-setup-full.msi";
static const wchar_t* PSE_SERVICE = L"ps_SB";                          // имя службы для pse.exe -r
static const wchar_t* NAME_FILTERS[] = { L"saby", L"sbis", L"сбис" };   // подстроки в DisplayName

// ---------- Окно и сообщения ----------
#define WM_APP_LOG  (WM_APP + 1)
#define WM_APP_LIST (WM_APP + 2)
#define WM_APP_DONE (WM_APP + 3)

enum {
    IDC_PCLBL = 100, IDC_PC, IDC_SCAN, IDC_LISTLBL, IDC_LIST,
    IDC_SELALL, IDC_SELNONE, IDC_UNINSTALL,
    IDC_MSILBL, IDC_MSI, IDC_BROWSE, IDC_INSTALL, IDC_LOG
};

static HWND g_hwnd = nullptr;
static int g_dpi = 96;
static bool g_busy = false;
static HFONT g_font = nullptr, g_logFont = nullptr;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// ---------- Данные ----------
struct App {
    std::wstring name, version, keyName, uninstall, quiet, scope;
    bool msi = false;
};

struct ListResult {
    bool ok = false;
    std::wstring pc;
    std::vector<App> apps;
};

static std::vector<App> g_apps;   // список, показанный в окне (только UI-поток)
static std::wstring g_pc;         // для какого компьютера он получен
static bool g_hasList = false;

// Лог можно писать из любого потока
static void Log(const std::wstring& m) {
    std::wstring* s = new std::wstring(m);
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_APP_LOG, 0, (LPARAM)s)) delete s;
}

static void PostList(const std::wstring& pc, const std::vector<App>& apps, bool ok) {
    ListResult* r = new ListResult();
    r->ok = ok;
    r->pc = pc;
    r->apps = apps;
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_APP_LIST, 0, (LPARAM)r)) delete r;
}

// ---------- Утилиты ----------
static std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::wstring Unquote(const std::wstring& in) {
    std::wstring s = Trim(in);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    return s;
}

static bool FileExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool IsSaby(const std::wstring& name) {
    std::wstring l = Lower(name);
    for (const wchar_t* f : NAME_FILTERS)
        if (l.find(f) != std::wstring::npos) return true;
    return false;
}

static std::wstring NormalizePc(std::wstring s) {
    s = Trim(s);
    while (!s.empty() && s[0] == L'\\') s.erase(0, 1);
    if (s.empty() || s == L"." || Lower(s) == L"localhost") return L"";
    wchar_t me[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(me, &n) && Lower(s) == Lower(me)) return L"";
    return s;
}

static std::wstring Disp(const std::wstring& pc) {
    return pc.empty() ? std::wstring(L"этом компьютере") : pc;
}

static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}

// ---------- Запуск процессов (без окон, вывод уходит в лог) ----------
static bool RunProcess(const std::wstring& cmd, DWORD& exitCode, bool logOutput = true) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = (nul != INVALID_HANDLE_VALUE) ? nul : nullptr;

    PROCESS_INFORMATION pi = {};
    std::wstring c = cmd;
    BOOL ok = CreateProcessW(nullptr, &c[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }

    std::string buf;
    char tmp[4096] = {};
    DWORD n = 0;
    while (ReadFile(rd, tmp, sizeof(tmp), &n, nullptr) && n) buf.append(tmp, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (logOutput && !buf.empty()) {
        int wl = MultiByteToWideChar(CP_OEMCP, 0, buf.data(), (int)buf.size(), nullptr, 0);
        std::wstring w(wl, L'\0');
        MultiByteToWideChar(CP_OEMCP, 0, buf.data(), (int)buf.size(), &w[0], wl);
        size_t pos = 0;
        while (pos < w.size()) {
            size_t e = w.find(L'\n', pos);
            if (e == std::wstring::npos) e = w.size();
            std::wstring line = Trim(w.substr(pos, e - pos));
            if (!line.empty()) Log(L"    " + line);
            pos = e + 1;
        }
    }
    return true;
}

static std::wstring PsQuote(const std::wstring& s) {
    std::wstring r = L"'";
    for (wchar_t c : s) {
        if (c == L'\'') r += L"''";
        else r += c;
    }
    return r + L"'";
}

static std::wstring Base64Utf16(const std::wstring& s) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char* p = (const unsigned char*)s.data();
    size_t n = s.size() * sizeof(wchar_t);
    std::wstring out;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = p[i] << 16;
        if (i + 1 < n) v |= p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        out += (wchar_t)T[(v >> 18) & 63];
        out += (wchar_t)T[(v >> 12) & 63];
        out += (i + 1 < n) ? (wchar_t)T[(v >> 6) & 63] : L'=';
        out += (i + 2 < n) ? (wchar_t)T[v & 63] : L'=';
    }
    return out;
}

static std::wstring FindPse() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d = exe;
    size_t p = d.find_last_of(L'\\');
    d = d.substr(0, p + 1) + L"pse.exe";
    if (FileExists(d)) return d;
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"pse.exe", nullptr, MAX_PATH, buf, nullptr)) return buf;
    return L"";
}

// Выполняет команду на компьютере pc (пусто = локально).
// true - команду удалось запустить (успех самой операции проверяется по реестру).
static bool ExecOn(const std::wstring& pc, const std::wstring& cmd) {
    DWORD rc = 0;
    if (pc.empty()) {
        if (!RunProcess(cmd, rc)) {
            Log(L"  Не удалось запустить: " + cmd);
            return false;
        }
        Log(L"  Код завершения: " + std::to_wstring(rc));
        return true;
    }

    // 1) Стандартные средства: WMI/DCOM, msiexec запускается на целевом ПК
    Log(L"  Запуск через WMI (может занять несколько минут)...");
    std::wstring ps =
        L"try{"
        L"$o=New-CimSessionOption -Protocol Dcom;"
        L"$s=New-CimSession -ComputerName " + PsQuote(pc) + L" -SessionOption $o -ErrorAction Stop;"
        L"$r=Invoke-CimMethod -CimSession $s -ClassName Win32_Process -MethodName Create"
        L" -Arguments @{CommandLine=" + PsQuote(cmd) + L"} -ErrorAction Stop;"
        L"if($r.ReturnValue -ne 0){exit 100}"
        L"$id=$r.ProcessId;$n=0;"
        L"while($n -lt 600 -and (Get-CimInstance -CimSession $s -ClassName Win32_Process"
        L" -Filter ('ProcessId='+$id) -ErrorAction SilentlyContinue)){Start-Sleep -Seconds 3;$n++}"
        L"if($n -ge 600){exit 102}"
        L"exit 0"
        L"}catch{exit 101}";
    std::wstring line = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " +
                        Base64Utf16(ps);
    if (RunProcess(line, rc, false) && rc == 0) return true;
    Log(L"  WMI не сработал (код " + std::to_wstring(rc) + L"), пробую pse.exe...");

    // 2) psexec, переименованный в pse.exe
    std::wstring pse = FindPse();
    if (pse.empty()) {
        Log(L"  pse.exe не найден (положите рядом с программой или в PATH).");
        return false;
    }
    std::wstring pl = L"\"" + pse + L"\" \\\\" + pc + L" -accepteula -s -r " + PSE_SERVICE + L" " + cmd;
    if (!RunProcess(pl, rc)) {
        Log(L"  Не удалось запустить pse.exe");
        return false;
    }
    Log(L"  pse.exe завершился с кодом " + std::to_wstring(rc));
    return true;
}

// ---------- Реестр ----------
static LSTATUS ConnectReg(const std::wstring& pc, HKEY root, HKEY& out) {
    if (pc.empty()) {
        out = root;  // предопределённый ключ, RegCloseKey для него безопасен
        return ERROR_SUCCESS;
    }
    std::wstring unc = L"\\\\" + pc;
    return RegConnectRegistryW(unc.c_str(), root, &out);
}

static bool RegGetStr(HKEY k, const wchar_t* name, std::wstring& out) {
    DWORD type = 0, sz = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, nullptr, &sz) != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    std::wstring b(sz / sizeof(wchar_t) + 1, L'\0');
    DWORD sz2 = sz;
    if (RegQueryValueExW(k, name, nullptr, nullptr, (LPBYTE)&b[0], &sz2) != ERROR_SUCCESS) return false;
    b.resize(wcslen(b.c_str()));
    out = b;
    return true;
}

static bool IsGuid(const std::wstring& s) {
    return s.size() == 38 && s.front() == L'{' && s.back() == L'}';
}

static void ScanUninstallKey(HKEY root, const std::wstring& sub, REGSAM view,
                             const std::wstring& scope, std::vector<App>& apps) {
    HKEY k;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | view, &k) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        DWORD n = 512;
        LSTATUS r = RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        HKEY s;
        if (RegOpenKeyExW(k, name, 0, KEY_READ | view, &s) != ERROR_SUCCESS) continue;
        App a;
        if (RegGetStr(s, L"DisplayName", a.name) && IsSaby(a.name)) {
            a.keyName = name;
            a.scope = scope;
            RegGetStr(s, L"DisplayVersion", a.version);
            RegGetStr(s, L"UninstallString", a.uninstall);
            RegGetStr(s, L"QuietUninstallString", a.quiet);
            a.msi = IsGuid(a.keyName);
            bool dup = false;
            for (auto& e : apps)
                if (e.keyName == a.keyName && e.scope.compare(0, 4, L"HKLM") == 0 &&
                    a.scope.compare(0, 4, L"HKLM") == 0) { dup = true; break; }
            if (!dup) apps.push_back(a);
        }
        RegCloseKey(s);
    }
    RegCloseKey(k);
}

static std::wstring UserNameFromSid(const std::wstring& pc, const std::wstring& sid) {
    PSID psid = nullptr;
    if (!ConvertStringSidToSidW(sid.c_str(), &psid)) return sid;
    wchar_t user[256], dom[256];
    DWORD un = 256, dn = 256;
    SID_NAME_USE use;
    std::wstring unc = L"\\\\" + pc;
    std::wstring res = sid;
    if (LookupAccountSidW(pc.empty() ? nullptr : unc.c_str(), psid, user, &un, dom, &dn, &use))
        res = std::wstring(dom) + L"\\" + user;
    LocalFree(psid);
    return res;
}

// Для удалённого ПК: тип запуска службы RemoteRegistry -> "Автоматически" и запуск службы
static bool EnsureRemoteRegistry(const std::wstring& pc) {
    Log(L"  Проверка и запуск службы «Удалённый реестр» (RemoteRegistry) на " + pc + L"...");
    std::wstring unc = L"\\\\" + pc;
    SC_HANDLE scm = OpenSCManagerW(unc.c_str(), nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        Log(L"  Диспетчер служб на " + pc + L" недоступен (ошибка " + std::to_wstring(GetLastError()) + L").");
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, L"RemoteRegistry",
                                 SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!svc) {
        Log(L"  Служба «Удалённый реестр» (RemoteRegistry): нет доступа (ошибка " +
            std::to_wstring(GetLastError()) + L").");
        CloseServiceHandle(scm);
        return false;
    }

    bool ok = true;

    // Тип запуска
    DWORD need = 0;
    bool canReadConfig = true;
    if (!QueryServiceConfigW(svc, nullptr, 0, &need)) {
        DWORD error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER) {
            Log(L"  Не удалось определить тип запуска «Удалённого реестра» (ошибка " +
                std::to_wstring(error) + L").");
            ok = false;
            canReadConfig = false;
        }
    }

    if (canReadConfig) {
        std::vector<BYTE> cfgBuf(need ? need : sizeof(QUERY_SERVICE_CONFIGW));
        bool configRead = false;
        bool isAuto = false;
        if (QueryServiceConfigW(svc, (LPQUERY_SERVICE_CONFIGW)cfgBuf.data(),
                                (DWORD)cfgBuf.size(), &need)) {
            configRead = true;
            isAuto = ((LPQUERY_SERVICE_CONFIGW)cfgBuf.data())->dwStartType == SERVICE_AUTO_START;
        } else {
            Log(L"  Не удалось прочитать параметры «Удалённого реестра» (ошибка " +
                std::to_wstring(GetLastError()) + L").");
            ok = false;
        }

        if (configRead && !isAuto) {
            if (ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
                                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
                Log(L"  «Удалённый реестр» на " + pc + L": тип запуска изменён на «Автоматически».");
            } else {
                Log(L"  Не удалось изменить тип запуска «Удалённого реестра» (ошибка " +
                    std::to_wstring(GetLastError()) + L").");
                ok = false;
            }
        }
    }

    // Запуск
    SERVICE_STATUS_PROCESS ssp = {};
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &need)) {
        DWORD error = GetLastError();
        Log(L"  Не удалось получить состояние службы «Удалённый реестр» на " + pc +
            L" (ошибка " + std::to_wstring(error) + L").");
        ok = false;
    } else if (ssp.dwCurrentState == SERVICE_RUNNING) {
        Log(L"  Служба «Удалённый реестр» на " + pc + L" уже запущена.");
    } else {
        if (ssp.dwCurrentState == SERVICE_STOPPED) {
            Log(L"  Запуск службы «Удалённый реестр» (RemoteRegistry) на " + pc + L"...");
            if (!StartServiceW(svc, 0, nullptr)) {
                DWORD error = GetLastError();
                if (error != ERROR_SERVICE_ALREADY_RUNNING) {
                    Log(L"  Ошибка запуска службы «Удалённый реестр» на " + pc +
                        L" (код " + std::to_wstring(error) + L").");
                    ok = false;
                }
            }
        } else {
            Log(L"  Служба «Удалённый реестр» на " + pc +
                L" уже выполняет переход состояния, ожидаю запуска...");
        }

        bool statusRead = true;
        for (int i = 0; i < 60; ++i) {  // ждём до 30 секунд
            if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &need)) {
                DWORD error = GetLastError();
                Log(L"  Не удалось проверить состояние службы «Удалённый реестр» (ошибка " +
                    std::to_wstring(error) + L").");
                statusRead = false;
                ok = false;
                break;
            }
            if (ssp.dwCurrentState == SERVICE_RUNNING) break;
            Sleep(500);
        }

        if (ssp.dwCurrentState == SERVICE_RUNNING) {
            Log(L"  Служба «Удалённый реестр» на " + pc + L" запущена.");
        } else {
            if (statusRead)
                Log(L"  Служба «Удалённый реестр» на " + pc + L" не запустилась за 30 секунд.");
            ok = false;
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

static bool Scan(const std::wstring& pc, std::vector<App>& apps, std::wstring& err) {
    apps.clear();
    if (!pc.empty()) EnsureRemoteRegistry(pc);
    HKEY hklm = nullptr;
    LSTATUS r = ConnectReg(pc, HKEY_LOCAL_MACHINE, hklm);
    if (r != ERROR_SUCCESS && !pc.empty()) {
        Log(L"  Удалённый реестр недоступен (код " + std::to_wstring((long)r) + L"), повторяю через 3 секунды...");
        Sleep(3000);
        r = ConnectReg(pc, HKEY_LOCAL_MACHINE, hklm);
    }
    if (r != ERROR_SUCCESS) {
        err = L"не удалось подключиться к реестру компьютера (код " + std::to_wstring((long)r) +
              L"). Проверьте имя, доступность и права.";
        return false;
    }

    const std::wstring base = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    ScanUninstallKey(hklm, base, KEY_WOW64_64KEY, L"HKLM x64", apps);
    ScanUninstallKey(hklm, base, KEY_WOW64_32KEY, L"HKLM x86", apps);
    RegCloseKey(hklm);

    // Установки "для пользователя" - только загруженные профили (вошедшие пользователи)
    HKEY hku = nullptr;
    if (ConnectReg(pc, HKEY_USERS, hku) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            wchar_t sid[256];
            DWORD n = 256;
            LSTATUS e = RegEnumKeyExW(hku, i, sid, &n, nullptr, nullptr, nullptr, nullptr);
            if (e == ERROR_NO_MORE_ITEMS) break;
            if (e != ERROR_SUCCESS) continue;
            std::wstring s = sid;
            if (s == L".DEFAULT" || s == L"S-1-5-18" || s == L"S-1-5-19" || s == L"S-1-5-20") continue;
            if (s.size() > 8 && s.compare(s.size() - 8, 8, L"_Classes") == 0) continue;
            std::wstring label = L"HKU " + UserNameFromSid(pc, s);
            ScanUninstallKey(hku, s + L"\\" + base, 0, label, apps);
            ScanUninstallKey(hku, s + L"\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                             0, label, apps);
        }
        RegCloseKey(hku);
    }
    return true;
}

static bool Contains(const std::vector<App>& v, const App& a) {
    for (auto& e : v)
        if (e.keyName == a.keyName && e.scope == a.scope) return true;
    return false;
}

static std::wstring BuildUninstallCmd(const App& a) {
    if (a.msi)
        return L"cmd /c msiexec.exe /x " + a.keyName +
               L" /qn /norestart /L*v \"%SystemRoot%\\Temp\\saby_uninstall.log\"";
    std::wstring u = !a.quiet.empty() ? a.quiet : a.uninstall;
    if (u.empty()) return L"";
    return L"cmd /c \"" + u + L"\"";
}

// Процессы Saby, которые закрываются перед удалением/установкой (проверьте имена в диспетчере задач)
static const wchar_t* KILL_PROCS[] = { L"saby.exe", L"sabydesktop.exe", L"sbis3plugin.exe" };

static void KillSaby(const std::wstring& pc) {
    std::wstring cmd = L"cmd /c ";
    bool first = true;
    for (const wchar_t* p : KILL_PROCS) {
        if (!first) cmd += L" & ";
        cmd += std::wstring(L"taskkill /F /T /IM ") + p;
        first = false;
    }
    Log(L"Закрываю процессы Saby (если запущены)...");
    ExecOn(pc, cmd);
}

// ---------- Фоновые задания ----------
enum JobType { JOB_SCAN, JOB_UNINSTALL, JOB_INSTALL };

struct Job {
    JobType type = JOB_SCAN;
    std::wstring pc;
    std::vector<App> sel;
    std::wstring msi;
};

static void JobScan(const Job& j) {
    Log(L"Читаю список на " + Disp(j.pc) + L"...");
    std::vector<App> apps;
    std::wstring err;
    if (!Scan(j.pc, apps, err)) {
        Log(L"ОШИБКА: " + err);
        PostList(j.pc, apps, false);
        return;
    }
    Log(L"Найдено записей: " + std::to_wstring(apps.size()));
    PostList(j.pc, apps, true);
}

static void JobUninstall(const Job& j) {
    KillSaby(j.pc);
    for (auto& a : j.sel) {
        Log(L"Удаляю: " + a.name + L" " + a.version);
        std::wstring cmd = BuildUninstallCmd(a);
        if (cmd.empty()) {
            Log(L"  Нет строки деинсталляции, пропуск.");
            continue;
        }
        ExecOn(j.pc, cmd);
    }
    std::vector<App> after;
    std::wstring err;
    if (!Scan(j.pc, after, err)) {
        Log(L"Не удалось перечитать список: " + err);
        return;
    }
    Log(L"Результат:");
    for (auto& a : j.sel)
        Log(L"  " + a.name + L" " + a.version + L": " +
            (Contains(after, a) ? L"ВСЁ ЕЩЁ УСТАНОВЛЕНО (журнал: %SystemRoot%\\Temp\\saby_uninstall.log)" : L"удалено"));
    PostList(j.pc, after, true);
}

static void JobInstall(const Job& j) {
    const std::wstring& src = j.msi;
    if (!FileExists(src)) {
        Log(L"Файл не найден: " + src);
        return;
    }
    std::vector<App> before;
    std::wstring err;
    if (!Scan(j.pc, before, err)) {
        Log(L"ОШИБКА: " + err);
        return;
    }

    size_t slash = src.find_last_of(L'\\');
    std::wstring fileName = (slash == std::wstring::npos) ? src : src.substr(slash + 1);

    std::wstring runPath, remoteCopy;
    if (j.pc.empty()) {
        runPath = src;
    } else {
        // Копируем msi на целевой ПК (C:\Windows\Temp) и ставим оттуда:
        // так не нужны права SYSTEM на сетевую папку
        remoteCopy = L"\\\\" + j.pc + L"\\ADMIN$\\Temp\\" + fileName;
        Log(L"Копирую установщик на " + j.pc + L"...");
        if (!CopyFileW(src.c_str(), remoteCopy.c_str(), FALSE)) {
            Log(L"Не удалось скопировать (ошибка " + std::to_wstring(GetLastError()) +
                L"). Нужен доступ к \\\\" + j.pc + L"\\ADMIN$.");
            return;
        }
        runPath = L"%SystemRoot%\\Temp\\" + fileName;
    }

    std::wstring cmd = L"cmd /c msiexec.exe /i \"" + runPath +
                       L"\" /qn /norestart /L*v \"%SystemRoot%\\Temp\\saby_msi.log\"";
    KillSaby(j.pc);
    Log(L"Устанавливаю " + fileName + L" на " + Disp(j.pc) + L"...");
    ExecOn(j.pc, cmd);
    if (!remoteCopy.empty()) DeleteFileW(remoteCopy.c_str());

    std::vector<App> after;
    if (!Scan(j.pc, after, err)) {
        Log(L"Не удалось перечитать список: " + err);
        return;
    }
    bool found = false;
    Log(L"Результат:");
    for (auto& a : after)
        if (!Contains(before, a)) {
            Log(L"  появилось: " + a.name + L" " + a.version);
            found = true;
        }
    if (!found)
        Log(L"  Новых записей не появилось (возможно, версия уже установлена или обновилась на месте). "
            L"Журнал установки: %SystemRoot%\\Temp\\saby_msi.log на целевом ПК.");
    PostList(j.pc, after, true);
}

static DWORD WINAPI Worker(LPVOID p) {
    std::unique_ptr<Job> j((Job*)p);
    switch (j->type) {
        case JOB_SCAN:      JobScan(*j); break;
        case JOB_UNINSTALL: JobUninstall(*j); break;
        case JOB_INSTALL:   JobInstall(*j); break;
    }
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    return 0;
}

static bool IsElevated() {
    HANDLE t = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    TOKEN_ELEVATION e = {};
    DWORD sz = 0;
    BOOL ok = GetTokenInformation(t, TokenElevation, &e, sizeof(e), &sz);
    CloseHandle(t);
    return ok && e.TokenIsElevated;
}

// ---------- Интерфейс ----------
static HWND Ctl(int id) { return GetDlgItem(g_hwnd, id); }

static HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                           g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

static void AppendLog(const std::wstring& m) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t ts[32];
    wsprintfW(ts, L"%02d:%02d:%02d  ", t.wHour, t.wMinute, t.wSecond);
    std::wstring line = std::wstring(ts) + m + L"\r\n";
    HWND h = Ctl(IDC_LOG);
    int len = GetWindowTextLengthW(h);
    SendMessageW(h, EM_SETSEL, len, len);
    SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
}

static void SetBusy(bool b) {
    g_busy = b;
    BOOL en = b ? FALSE : TRUE;
    int ids[] = { IDC_PC, IDC_SCAN, IDC_UNINSTALL, IDC_MSI, IDC_BROWSE, IDC_INSTALL, IDC_SELALL, IDC_SELNONE };
    for (int id : ids) EnableWindow(Ctl(id), en);
    SetWindowTextW(g_hwnd, b ? L"Saby Manager - выполняется операция..." : L"Saby Manager");
}

static void StartJob(Job* j) {
    SetBusy(true);
    HANDLE h = CreateThread(nullptr, 0, Worker, j, 0, nullptr);
    if (h) {
        CloseHandle(h);
    } else {
        delete j;
        SetBusy(false);
        AppendLog(L"Не удалось запустить фоновый поток.");
    }
}

static void Populate() {
    HWND lv = Ctl(IDC_LIST);
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < g_apps.size(); ++i) {
        const App& a = g_apps[i];
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)a.name.c_str();
        int idx = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
        ListView_SetItemText(lv, idx, 1, (LPWSTR)a.version.c_str());
        ListView_SetItemText(lv, idx, 2, (LPWSTR)(a.msi ? L"MSI" : L"EXE"));
        ListView_SetItemText(lv, idx, 3, (LPWSTR)a.scope.c_str());
    }
    std::wstring t;
    if (!g_hasList) t = L"Список не загружен. Введите имя компьютера (пусто = этот) и нажмите «Найти».";
    else if (g_apps.empty()) t = L"На компьютере " + (g_pc.empty() ? std::wstring(L"(этот)") : g_pc) +
                                 L" установленных версий Saby/СБИС не найдено.";
    else t = L"Компьютер " + (g_pc.empty() ? std::wstring(L"(этот)") : g_pc) +
             L": найдено записей " + std::to_wstring(g_apps.size()) + L". Отметьте галочками, что удалить.";
    SetWindowTextW(Ctl(IDC_LISTLBL), t.c_str());
}

static void Layout() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(10), rh = S(26), gap = S(8);

    int y = m;
    int x = m;
    MoveWindow(Ctl(IDC_PCLBL), x, y, S(90), rh, TRUE);
    int btnW = S(100);
    MoveWindow(Ctl(IDC_PC), x + S(95), y, W - x - S(95) - gap - btnW - m, rh, TRUE);
    MoveWindow(Ctl(IDC_SCAN), W - m - btnW, y, btnW, rh, TRUE);
    y += rh + gap;

    MoveWindow(Ctl(IDC_LISTLBL), m, y, W - 2 * m, S(20), TRUE);
    y += S(22);

    int logH = S(150);
    int logY = H - m - logH;
    int installY = logY - gap - rh;
    int btnY = installY - gap - rh;
    int listH = btnY - gap - y;
    if (listH < S(60)) listH = S(60);
    MoveWindow(Ctl(IDC_LIST), m, y, W - 2 * m, listH, TRUE);

    x = m;
    MoveWindow(Ctl(IDC_SELALL), x, btnY, S(120), rh, TRUE);       x += S(120) + gap;
    MoveWindow(Ctl(IDC_SELNONE), x, btnY, S(110), rh, TRUE);      x += S(110) + gap;
    MoveWindow(Ctl(IDC_UNINSTALL), x, btnY, S(180), rh, TRUE);

    int instW = S(120), browseW = S(90);
    MoveWindow(Ctl(IDC_MSILBL), m, installY, S(120), rh, TRUE);
    MoveWindow(Ctl(IDC_MSI), m + S(125), installY, W - m - S(125) - gap - browseW - gap - instW - m, rh, TRUE);
    MoveWindow(Ctl(IDC_BROWSE), W - m - instW - gap - browseW, installY, browseW, rh, TRUE);
    MoveWindow(Ctl(IDC_INSTALL), W - m - instW, installY, instW, rh, TRUE);

    MoveWindow(Ctl(IDC_LOG), m, logY, W - 2 * m, logH, TRUE);

    // ширина колонок списка: последняя занимает остаток
    HWND lv = Ctl(IDC_LIST);
    int total = W - 2 * m - S(24);
    int c0 = S(330), c1 = S(120), c2 = S(55);
    int c3 = total - c0 - c1 - c2;
    if (c3 < S(120)) c3 = S(120);
    ListView_SetColumnWidth(lv, 0, c0);
    ListView_SetColumnWidth(lv, 1, c1);
    ListView_SetColumnWidth(lv, 2, c2);
    ListView_SetColumnWidth(lv, 3, c3);
}

static BOOL CALLBACK SetFontProc(HWND h, LPARAM) {
    int id = GetDlgCtrlID(h);
    SendMessageW(h, WM_SETFONT, (WPARAM)(id == IDC_LOG ? g_logFont : g_font), TRUE);
    return TRUE;
}

static void OnCreate() {
    Make(L"STATIC", L"Компьютер:", SS_LEFT | SS_CENTERIMAGE, IDC_PCLBL);
    HWND pc = Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_PC, WS_EX_CLIENTEDGE);
    SendMessageW(pc, 0x1501 /*EM_SETCUEBANNER*/, FALSE, (LPARAM)L"пусто = этот компьютер");
    Make(L"BUTTON", L"Найти", WS_TABSTOP | BS_PUSHBUTTON, IDC_SCAN);
    Make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, IDC_LISTLBL);

    HWND lv = Make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                   IDC_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                                              LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[] = { L"Название", L"Версия", L"Тип", L"Где найдено" };
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR)cols[i];
        c.cx = S(100);
        SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }

    Make(L"BUTTON", L"Отметить все", WS_TABSTOP | BS_PUSHBUTTON, IDC_SELALL);
    Make(L"BUTTON", L"Снять все", WS_TABSTOP | BS_PUSHBUTTON, IDC_SELNONE);
    Make(L"BUTTON", L"Удалить отмеченные", WS_TABSTOP | BS_PUSHBUTTON, IDC_UNINSTALL);

    Make(L"STATIC", L"Установщик (.msi):", SS_LEFT | SS_CENTERIMAGE, IDC_MSILBL);
    Make(L"EDIT", DEFAULT_MSI, WS_TABSTOP | ES_AUTOHSCROLL, IDC_MSI, WS_EX_CLIENTEDGE);
    Make(L"BUTTON", L"Обзор...", WS_TABSTOP | BS_PUSHBUTTON, IDC_BROWSE);
    Make(L"BUTTON", L"Установить", WS_TABSTOP | BS_PUSHBUTTON, IDC_INSTALL);

    HWND log = Make(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IDC_LOG,
                    WS_EX_CLIENTEDGE);
    SendMessageW(log, EM_SETLIMITTEXT, 0, 0);

    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    g_logFont = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            FIXED_PITCH | FF_MODERN, L"Consolas");
    EnumChildWindows(g_hwnd, SetFontProc, 0);

    Populate();
    AppendLog(L"Готово. Введите имя компьютера (или оставьте пустым для этого) и нажмите «Найти».");
    if (!IsElevated())
        AppendLog(L"ВНИМАНИЕ: программа запущена БЕЗ прав администратора. Удаление и установка завершатся ошибкой 1603/1730. Запустите от имени администратора.");
    SetFocus(Ctl(IDC_PC));
}

static void OnScan() {
    if (g_busy) return;
    Job* j = new Job();
    j->type = JOB_SCAN;
    j->pc = NormalizePc(GetText(Ctl(IDC_PC)));
    StartJob(j);
}

static void OnUninstall() {
    if (g_busy) return;
    if (!g_hasList) {
        MessageBoxW(g_hwnd, L"Сначала нажмите «Найти».", L"Saby Manager", MB_ICONINFORMATION);
        return;
    }
    if (NormalizePc(GetText(Ctl(IDC_PC))) != g_pc) {
        MessageBoxW(g_hwnd, L"Список получен для другого компьютера. Нажмите «Найти» для текущего имени.",
                    L"Saby Manager", MB_ICONINFORMATION);
        return;
    }
    HWND lv = Ctl(IDC_LIST);
    std::unique_ptr<Job> j(new Job());
    j->type = JOB_UNINSTALL;
    j->pc = g_pc;
    std::wstring list;
    for (size_t i = 0; i < g_apps.size(); ++i) {
        if (ListView_GetCheckState(lv, (int)i)) {
            j->sel.push_back(g_apps[i]);
            list += L"\n  - " + g_apps[i].name + L" " + g_apps[i].version + L" (" + g_apps[i].scope + L")";
            if (!g_apps[i].msi) list += L"  [EXE: запустится строка деинсталляции]";
        }
    }
    if (j->sel.empty()) {
        MessageBoxW(g_hwnd, L"Ничего не отмечено.", L"Saby Manager", MB_ICONINFORMATION);
        return;
    }
    std::wstring q = L"Удалить с компьютера " + (g_pc.empty() ? std::wstring(L"(этот)") : g_pc) + L":" + list +
                     L"\n\nПродолжить?";
    if (MessageBoxW(g_hwnd, q.c_str(), L"Подтверждение", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    StartJob(j.release());
}

static void OnInstall() {
    if (g_busy) return;
    std::wstring msi = Unquote(GetText(Ctl(IDC_MSI)));
    if (msi.empty()) msi = DEFAULT_MSI;
    std::wstring low = Lower(msi);
    if (low.size() < 4 || low.compare(low.size() - 4, 4, L".msi") != 0) {
        MessageBoxW(g_hwnd, L"Поддерживается только установка из .msi (тихие ключи для exe не заданы).",
                    L"Saby Manager", MB_ICONWARNING);
        return;
    }
    std::wstring pc = NormalizePc(GetText(Ctl(IDC_PC)));
    std::wstring q = L"Установить\n" + msi + L"\nна компьютер " + (pc.empty() ? std::wstring(L"(этот)") : pc) +
                     L"?";
    if (MessageBoxW(g_hwnd, q.c_str(), L"Подтверждение", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    Job* j = new Job();
    j->type = JOB_INSTALL;
    j->pc = pc;
    j->msi = msi;
    StartJob(j);
}

static void OnBrowse() {
    wchar_t file[MAX_PATH] = {};
    std::wstring cur = Unquote(GetText(Ctl(IDC_MSI)));
    wcsncpy_s(file, cur.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Установщик MSI (*.msi)\0*.msi\0Все файлы\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(Ctl(IDC_MSI), file);
    }
}

static void CheckAll(BOOL on) {
    HWND lv = Ctl(IDC_LIST);
    int n = ListView_GetItemCount(lv);
    for (int i = 0; i < n; ++i) ListView_SetCheckState(lv, i, on);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            OnCreate();
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = S(760);
            mm->ptMinTrackSize.y = S(520);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_SCAN:      OnScan(); break;
                case IDC_UNINSTALL: OnUninstall(); break;
                case IDC_INSTALL:   OnInstall(); break;
                case IDC_BROWSE:    OnBrowse(); break;
                case IDC_SELALL:    CheckAll(TRUE); break;
                case IDC_SELNONE:   CheckAll(FALSE); break;
            }
            return 0;
        case WM_APP_LOG: {
            std::unique_ptr<std::wstring> s((std::wstring*)lp);
            AppendLog(*s);
            return 0;
        }
        case WM_APP_LIST: {
            std::unique_ptr<ListResult> r((ListResult*)lp);
            if (r->ok) {
                g_apps = r->apps;
                g_pc = r->pc;
                g_hasList = true;
            } else {
                g_apps.clear();
                g_hasList = false;
            }
            Populate();
            return 0;
        }
        case WM_APP_DONE:
            SetBusy(false);
            return 0;
        case WM_CLOSE:
            if (g_busy &&
                MessageBoxW(hwnd, L"Операция ещё выполняется. Всё равно закрыть программу?", L"Saby Manager",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return 0;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int nShow) {
    // Если запущены без повышения прав - перезапускаем себя с запросом UAC
    if (!IsElevated()) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if ((INT_PTR)ShellExecuteW(nullptr, L"runas", exe, nullptr, nullptr, SW_SHOWNORMAL) > 32) return 0;
    }
    SetProcessDPIAware();
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SabyManagerWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Saby Manager", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(960), S(680), nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Enter в поле имени компьютера = "Найти"
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && g_hwnd && msg.hwnd == Ctl(IDC_PC)) {
            PostMessageW(g_hwnd, WM_COMMAND, MAKEWPARAM(IDC_SCAN, BN_CLICKED), (LPARAM)Ctl(IDC_SCAN));
            continue;
        }
        if (!g_hwnd || !IsDialogMessageW(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
