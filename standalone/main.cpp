// ============================================================================
// Audio Hardware Volume Preset - Standalone Background Application
//
// Automatically sets audio playback devices to a configured volume on connect.
// Runs as a lightweight native Windows system tray utility.
// ============================================================================

#include <initguid.h>
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ----------------------------------------------------------------------------
// Constants & Identifiers
// ----------------------------------------------------------------------------
#define WM_APP_TRAYMSG        (WM_APP + 1)
#define IDI_TRAY_ICON         101
#define IDM_TITLE             2000
#define IDM_OPEN_CONFIG       2001
#define IDM_RELOAD_CONFIG     2002
#define IDM_STARTUP_TOGGLE    2003
#define IDM_EXIT              2004

static const wchar_t kAppMutexName[]  = L"AudioHardwareVolumePreset_SingleInstance_Mutex";
static const wchar_t kWindowClassName[] = L"AudioHardwareVolumePreset_TrayWindowClass";
static const wchar_t kAppRegistryRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kAppRegistryValueName[] = L"AudioHardwareVolumePreset";

// ----------------------------------------------------------------------------
// Data Structures
// ----------------------------------------------------------------------------
struct DeviceRule {
  std::wstring originalName;
  std::wstring nameLower;
  std::vector<std::wstring> tokens;
  int volume;
};

// ----------------------------------------------------------------------------
// Global State
// ----------------------------------------------------------------------------
static HINSTANCE g_hInstance = NULL;
static HWND g_hWnd = NULL;
static NOTIFYICONDATAW g_nid = {};

static SRWLOCK g_lock = SRWLOCK_INIT;
static int g_defaultVolume = 30;
static int g_maxVolumeLimit = 100;
static BOOL g_applyToAllDevices = TRUE;
static BOOL g_autoUnmute = TRUE;
static BOOL g_applyOnStartup = TRUE;
static std::vector<DeviceRule> g_rules;

// Debouncing state (worker thread only)
static std::wstring g_lastDeviceId;
static ULONGLONG g_lastApplyTick = 0;

// Queue and event handles for worker thread
static HANDLE g_hWorkEvent = NULL;
static SRWLOCK g_queueLock = SRWLOCK_INIT;
static std::vector<std::wstring> g_pendingDeviceIds;

// COM & Worker Thread handles
static HANDLE g_hWorkerThread = NULL;
static HANDLE g_hStopEvent = NULL;
static IMMDeviceEnumerator *g_pEnumerator = NULL;

// ----------------------------------------------------------------------------
// Forward Declarations
// ----------------------------------------------------------------------------
static void LoadSettings();
static void QueueApply(LPCWSTR deviceId);
static void ApplyVolumeToDevice(LPCWSTR pwstrDeviceId);
static void ApplyToCurrentDefaultDevice();
static DWORD WINAPI AudioMonitorThread(LPVOID lpParam);
static std::wstring GetConfigFilePath();
static void EnsureDefaultConfigFile(const std::wstring &path);
static bool IsRunAtStartupEnabled();
static void SetRunAtStartup(bool enable);

// ----------------------------------------------------------------------------
// String & Token Matching Helpers
// ----------------------------------------------------------------------------
static std::wstring ToLower(const std::wstring &str) {
  std::wstring result = str;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](wchar_t c) { return (wchar_t)towlower(c); });
  return result;
}

static std::wstring Trim(const std::wstring &str) {
  size_t first = str.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return L"";
  size_t last = str.find_last_not_of(L" \t\r\n");
  return str.substr(first, (last - first + 1));
}

static bool IsStopWord(const std::wstring &word) {
  static const std::wstring kStopWords[] = {
      L"headphones", L"speakers", L"headset", L"earphone", L"earphones",
      L"audio",      L"device",   L"input",   L"output"};
  for (const auto &sw : kStopWords) {
    if (word == sw)
      return true;
  }
  return false;
}

static std::vector<std::wstring> GetTokens(const std::wstring &str) {
  std::vector<std::wstring> tokens;
  std::wstring cur;
  for (wchar_t ch : str) {
    if (iswalnum(ch)) {
      cur += (wchar_t)towlower(ch);
    } else {
      if (!cur.empty()) {
        if (cur.length() >= 2 && !IsStopWord(cur)) {
          tokens.push_back(cur);
        }
        cur.clear();
      }
    }
  }
  if (!cur.empty()) {
    if (cur.length() >= 2 && !IsStopWord(cur)) {
      tokens.push_back(cur);
    }
  }
  return tokens;
}

static bool CheckCandidateMatch(const std::wstring &candidate,
                                const DeviceRule &rule,
                                bool allowTokens) {
  if (candidate.empty() || rule.nameLower.empty())
    return false;

  std::wstring candidateLower = ToLower(candidate);

  // 1. Direct substring match: device name contains the rule keyword
  if (candidateLower.find(rule.nameLower) != std::wstring::npos) {
    return true;
  }

  if (!allowTokens) {
    return false;
  }

  // 2. Token match: check if any device token matches or contains a rule token
  auto devTokens = GetTokens(candidateLower);
  for (const auto &rt : rule.tokens) {
    for (const auto &dt : devTokens) {
      if (dt == rt || dt.find(rt) != std::wstring::npos) {
        return true;
      }
    }
  }

  return false;
}

// ----------------------------------------------------------------------------
// Configuration Management
// ----------------------------------------------------------------------------
static std::wstring GetConfigFilePath() {
  WCHAR szPath[MAX_PATH];
  GetModuleFileNameW(NULL, szPath, MAX_PATH);
  PathRemoveFileSpecW(szPath);
  PathCombineW(szPath, szPath, L"config.ini");
  return std::wstring(szPath);
}

static void EnsureDefaultConfigFile(const std::wstring &path) {
  if (PathFileExistsW(path.c_str())) {
    return;
  }

  const char defaultContent[] =
      "; ==========================================================\n"
      "; Audio Hardware Volume Preset - Configuration\n"
      "; ==========================================================\n"
      "; Changes take effect after saving and clicking \"Reload Settings\"\n"
      "; in the system tray menu, or after restarting the application.\n"
      "\n"
      "[Settings]\n"
      "; Master fallback volume percentage (0-100) applied when an audio\n"
      "; device connects without a specific matching rule below.\n"
      "DefaultVolume = 30\n"
      "\n"
      "; Maximum volume ceiling (0-100). Presets will never exceed this cap.\n"
      "MaxVolumeLimit = 100\n"
      "\n"
      "; 1 = Apply DefaultVolume to unlisted devices; 0 = Leave them untouched.\n"
      "ApplyToAllDevices = 1\n"
      "\n"
      "; 1 = Automatically unmute device on connect; 0 = Leave mute status.\n"
      "AutoUnmute = 1\n"
      "\n"
      "; 1 = Apply preset to current default playback device at app start.\n"
      "ApplyOnStartup = 1\n"
      "\n"
      "[Rules]\n"
      "; Format: Keyword or Device Name = Volume %\n"
      "; The first matching rule wins. Place more specific rules first.\n"
      "AirPods = 30\n"
      "Sony = 25\n"
      "Speakers = 50\n"
      "DAC = 40\n";

  HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hFile != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(hFile, defaultContent, (DWORD)(sizeof(defaultContent) - 1), &written, NULL);
    CloseHandle(hFile);
  }
}

static void LoadSettings() {
  std::wstring configPath = GetConfigFilePath();
  EnsureDefaultConfigFile(configPath);

  AcquireSRWLockExclusive(&g_lock);

  g_defaultVolume = GetPrivateProfileIntW(L"Settings", L"DefaultVolume", 30, configPath.c_str());
  if (g_defaultVolume < 0) g_defaultVolume = 0;
  if (g_defaultVolume > 100) g_defaultVolume = 100;

  g_maxVolumeLimit = GetPrivateProfileIntW(L"Settings", L"MaxVolumeLimit", 100, configPath.c_str());
  if (g_maxVolumeLimit < 0) g_maxVolumeLimit = 0;
  if (g_maxVolumeLimit > 100) g_maxVolumeLimit = 100;

  g_applyToAllDevices = GetPrivateProfileIntW(L"Settings", L"ApplyToAllDevices", 1, configPath.c_str()) ? TRUE : FALSE;
  g_autoUnmute = GetPrivateProfileIntW(L"Settings", L"AutoUnmute", 1, configPath.c_str()) ? TRUE : FALSE;
  g_applyOnStartup = GetPrivateProfileIntW(L"Settings", L"ApplyOnStartup", 1, configPath.c_str()) ? TRUE : FALSE;

  g_rules.clear();

  // Read the entire [Rules] section
  const DWORD kBufferSize = 16384;
  std::vector<wchar_t> buffer(kBufferSize, 0);
  DWORD length = GetPrivateProfileSectionW(L"Rules", buffer.data(), kBufferSize, configPath.c_str());

  if (length > 0) {
    const wchar_t *p = buffer.data();
    while (*p) {
      std::wstring line(p);
      p += line.length() + 1;

      size_t eqPos = line.find(L'=');
      if (eqPos == std::wstring::npos) continue;

      std::wstring key = Trim(line.substr(0, eqPos));
      std::wstring valStr = Trim(line.substr(eqPos + 1));

      if (key.empty() || key[0] == L';' || key[0] == L'#') continue;

      int vol = _wtoi(valStr.c_str());
      if (vol < 0) vol = 0;
      if (vol > 100) vol = 100;

      DeviceRule rule;
      rule.originalName = key;
      rule.nameLower = ToLower(key);
      rule.tokens = GetTokens(rule.nameLower);
      rule.volume = vol;
      g_rules.push_back(rule);
    }
  }

  ReleaseSRWLockExclusive(&g_lock);
}

// ----------------------------------------------------------------------------
// Startup Registry Helper
// ----------------------------------------------------------------------------
static bool IsRunAtStartupEnabled() {
  HKEY hKey = NULL;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kAppRegistryRunKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
    WCHAR szVal[MAX_PATH];
    DWORD dwType = 0;
    DWORD cbData = sizeof(szVal);
    LONG res = RegQueryValueExW(hKey, kAppRegistryValueName, NULL, &dwType, (LPBYTE)szVal, &cbData);
    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS);
  }
  return false;
}

static void SetRunAtStartup(bool enable) {
  HKEY hKey = NULL;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kAppRegistryRunKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
    if (enable) {
      WCHAR szExePath[MAX_PATH + 2];
      szExePath[0] = L'"';
      GetModuleFileNameW(NULL, szExePath + 1, MAX_PATH);
      wcscat_s(szExePath, L"\"");
      RegSetValueExW(hKey, kAppRegistryValueName, 0, REG_SZ,
                      (const BYTE *)szExePath, (DWORD)((wcslen(szExePath) + 1) * sizeof(WCHAR)));
    } else {
      RegDeleteValueW(hKey, kAppRegistryValueName);
    }
    RegCloseKey(hKey);
  }
}

// ----------------------------------------------------------------------------
// Core Audio Volume Logic
// ----------------------------------------------------------------------------
static void QueueApply(LPCWSTR deviceId) {
  if (!deviceId) return;

  AcquireSRWLockExclusive(&g_queueLock);
  g_pendingDeviceIds.emplace_back(deviceId);
  ReleaseSRWLockExclusive(&g_queueLock);
  SetEvent(g_hWorkEvent);
}

static void ApplyVolumeToDevice(LPCWSTR pwstrDeviceId) {
  if (!pwstrDeviceId || !g_pEnumerator) return;

  ULONGLONG currentTick = GetTickCount64();

  // Debounce rapid duplicate notifications for the same device (1.5 seconds)
  if (g_lastDeviceId == pwstrDeviceId && (currentTick - g_lastApplyTick) < 1500) {
    return;
  }
  g_lastDeviceId = pwstrDeviceId;
  g_lastApplyTick = currentTick;

  IMMDevice *pDevice = NULL;
  HRESULT hr = g_pEnumerator->GetDevice(pwstrDeviceId, &pDevice);
  if (FAILED(hr) || !pDevice) return;

  // Filter: only handle render (playback) endpoints
  IMMEndpoint *pEndpoint = NULL;
  hr = pDevice->QueryInterface(__uuidof(IMMEndpoint), (void **)&pEndpoint);
  if (SUCCEEDED(hr) && pEndpoint) {
    EDataFlow flow;
    hr = pEndpoint->GetDataFlow(&flow);
    pEndpoint->Release();
    if (SUCCEEDED(hr) && flow != eRender) {
      pDevice->Release();
      return;
    }
  }

  // Retrieve device descriptive properties
  WCHAR szFriendlyName[256] = L"Unknown Device";
  WCHAR szInterfaceName[256] = L"";
  WCHAR szDeviceDesc[256] = L"";

  IPropertyStore *pProps = NULL;
  hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
  if (SUCCEEDED(hr) && pProps) {
    PROPVARIANT var;
    PropVariantInit(&var);

    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &var)) &&
        var.vt == VT_LPWSTR && var.pwszVal) {
      wcsncpy_s(szFriendlyName, var.pwszVal, _TRUNCATE);
    }
    PropVariantClear(&var);

    if (SUCCEEDED(pProps->GetValue(PKEY_DeviceInterface_FriendlyName, &var)) &&
        var.vt == VT_LPWSTR && var.pwszVal) {
      wcsncpy_s(szInterfaceName, var.pwszVal, _TRUNCATE);
    }
    PropVariantClear(&var);

    if (SUCCEEDED(pProps->GetValue(PKEY_Device_DeviceDesc, &var)) &&
        var.vt == VT_LPWSTR && var.pwszVal) {
      wcsncpy_s(szDeviceDesc, var.pwszVal, _TRUNCATE);
    }
    PropVariantClear(&var);

    pProps->Release();
  }

  int targetVolume = -1;

  AcquireSRWLockShared(&g_lock);

  // Pass 1: Direct substring check across all rules
  for (const auto &rule : g_rules) {
    if (CheckCandidateMatch(szFriendlyName, rule, false) ||
        CheckCandidateMatch(szInterfaceName, rule, false) ||
        CheckCandidateMatch(szDeviceDesc, rule, false)) {
      targetVolume = rule.volume;
      break;
    }
  }

  // Pass 2: Token match fallback
  if (targetVolume == -1) {
    for (const auto &rule : g_rules) {
      if (CheckCandidateMatch(szFriendlyName, rule, true) ||
          CheckCandidateMatch(szInterfaceName, rule, true) ||
          CheckCandidateMatch(szDeviceDesc, rule, true)) {
        targetVolume = rule.volume;
        break;
      }
    }
  }

  // Fallback to default volume if enabled
  if (targetVolume == -1 && g_applyToAllDevices) {
    targetVolume = g_defaultVolume;
  }

  // Enforce max volume safety cap
  if (targetVolume > g_maxVolumeLimit) {
    targetVolume = g_maxVolumeLimit;
  }

  BOOL doUnmute = g_autoUnmute;
  ReleaseSRWLockShared(&g_lock);

  if (targetVolume < 0) {
    pDevice->Release();
    return;
  }

  // Apply master volume level
  IAudioEndpointVolume *pEndpointVolume = NULL;
  hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL,
                         (void **)&pEndpointVolume);
  if (SUCCEEDED(hr) && pEndpointVolume) {
    float scalar = (float)targetVolume / 100.0f;
    pEndpointVolume->SetMasterVolumeLevelScalar(scalar, NULL);
    if (doUnmute) {
      pEndpointVolume->SetMute(FALSE, NULL);
    }
    pEndpointVolume->Release();
  }

  pDevice->Release();
}

static void ApplyToCurrentDefaultDevice() {
  if (!g_pEnumerator) return;

  IMMDevice *pDefaultDevice = NULL;
  HRESULT hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDefaultDevice);
  if (SUCCEEDED(hr) && pDefaultDevice) {
    LPWSTR pwstrId = NULL;
    if (SUCCEEDED(pDefaultDevice->GetId(&pwstrId)) && pwstrId) {
      ApplyVolumeToDevice(pwstrId);
      CoTaskMemFree(pwstrId);
    }
    pDefaultDevice->Release();
  }
}

// ----------------------------------------------------------------------------
// Core Audio Notification Client
// ----------------------------------------------------------------------------
class AudioNotificationClient : public IMMNotificationClient {
  LONG m_cRef;

public:
  AudioNotificationClient() : m_cRef(1) {}
  virtual ~AudioNotificationClient() {}

  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown)) {
      *ppv = static_cast<IUnknown *>(this);
    } else if (riid == __uuidof(IMMNotificationClient)) {
      *ppv = static_cast<IMMNotificationClient *>(this);
    } else {
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return InterlockedIncrement(&m_cRef);
  }

  STDMETHODIMP_(ULONG) Release() override {
    ULONG ulRef = InterlockedDecrement(&m_cRef);
    if (ulRef == 0) delete this;
    return ulRef;
  }

  STDMETHODIMP OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override {
    if (dwNewState == DEVICE_STATE_ACTIVE && pwstrDeviceId) {
      QueueApply(pwstrDeviceId);
    }
    return S_OK;
  }

  STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
  STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return S_OK; }

  STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId) override {
    if (flow == eRender && pwstrDeviceId) {
      QueueApply(pwstrDeviceId);
    }
    return S_OK;
  }

  STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
    return S_OK;
  }
};

static AudioNotificationClient *g_pClient = NULL;

static DWORD WINAPI AudioMonitorThread(LPVOID) {
  HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (FAILED(hrCom)) return 1;

  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void **)&g_pEnumerator);

  if (SUCCEEDED(hr) && g_pEnumerator) {
    g_pClient = new AudioNotificationClient();
    hr = g_pEnumerator->RegisterEndpointNotificationCallback(g_pClient);
    if (SUCCEEDED(hr)) {
      BOOL applyStartup = FALSE;
      AcquireSRWLockShared(&g_lock);
      applyStartup = g_applyOnStartup;
      ReleaseSRWLockShared(&g_lock);

      if (applyStartup) {
        ApplyToCurrentDefaultDevice();
      }

      HANDLE handles[] = {g_hStopEvent, g_hWorkEvent};
      while (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == (WAIT_OBJECT_0 + 1)) {
        std::vector<std::wstring> ids;
        AcquireSRWLockExclusive(&g_queueLock);
        ids.swap(g_pendingDeviceIds);
        ReleaseSRWLockExclusive(&g_queueLock);

        for (const auto &id : ids) {
          ApplyVolumeToDevice(id.c_str());
        }
      }

      g_pEnumerator->UnregisterEndpointNotificationCallback(g_pClient);
    }

    g_pClient->Release();
    g_pClient = NULL;
    g_pEnumerator->Release();
    g_pEnumerator = NULL;
  }

  CoUninitialize();
  return 0;
}

// ----------------------------------------------------------------------------
// Tray Window & Message Loop
// ----------------------------------------------------------------------------
static void ShowTrayContextMenu(HWND hWnd) {
  POINT pt;
  GetCursorPos(&pt);

  HMENU hMenu = CreatePopupMenu();
  if (!hMenu) return;

  InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | MF_GRAYED, IDM_TITLE, L"Audio Hardware Volume Preset");
  InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
  InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_OPEN_CONFIG, L"Open Config File (config.ini)");
  InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_RELOAD_CONFIG, L"Reload Settings");
  
  UINT startupFlags = MF_BYPOSITION | MF_STRING;
  if (IsRunAtStartupEnabled()) {
    startupFlags |= MF_CHECKED;
  }
  InsertMenuW(hMenu, -1, startupFlags, IDM_STARTUP_TOGGLE, L"Run at Windows Startup");

  InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
  InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_EXIT, L"Exit");

  SetForegroundWindow(hWnd);
  TrackPopupMenuEx(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, hWnd, NULL);
  DestroyMenu(hMenu);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_APP_TRAYMSG: {
      if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
        ShowTrayContextMenu(hWnd);
      } else if (lParam == WM_LBUTTONDBLCLK) {
        std::wstring configPath = GetConfigFilePath();
        ShellExecuteW(NULL, L"open", configPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
      }
      return 0;
    }

    case WM_COMMAND: {
      WORD cmdId = LOWORD(wParam);
      switch (cmdId) {
        case IDM_OPEN_CONFIG: {
          std::wstring configPath = GetConfigFilePath();
          ShellExecuteW(NULL, L"open", configPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
          break;
        }

        case IDM_RELOAD_CONFIG: {
          LoadSettings();
          ApplyToCurrentDefaultDevice();
          break;
        }

        case IDM_STARTUP_TOGGLE: {
          bool currentlyEnabled = IsRunAtStartupEnabled();
          SetRunAtStartup(!currentlyEnabled);
          break;
        }

        case IDM_EXIT: {
          DestroyWindow(hWnd);
          break;
        }
      }
      return 0;
    }

    case WM_DESTROY: {
      // Remove tray icon
      Shell_NotifyIconW(NIM_DELETE, &g_nid);
      PostQuitMessage(0);
      return 0;
    }
  }

  return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ----------------------------------------------------------------------------
// WinMain Entry Point
// ----------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
  g_hInstance = hInstance;

  // Single-instance enforcement
  HANDLE hMutex = CreateMutexW(NULL, TRUE, kAppMutexName);
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (hMutex) CloseHandle(hMutex);
    return 0;
  }

  LoadSettings();

  // Initialize Worker Events
  g_hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  g_hWorkEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (!g_hStopEvent || !g_hWorkEvent) {
    return 1;
  }

  // Register Hidden Window Class for Tray Icon Message Handling
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.lpszClassName = kWindowClassName;

  if (!RegisterClassExW(&wc)) {
    return 1;
  }

  g_hWnd = CreateWindowExW(0, kWindowClassName, L"AudioHardwareVolumePreset_Window",
                           0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
  if (!g_hWnd) {
    return 1;
  }

  // Obtain Native Audio Speaker Icon from shell32.dll
  HICON hAudioIcon = ExtractIconW(hInstance, L"shell32.dll", 168);
  if (!hAudioIcon) {
    hAudioIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
  }

  // Setup System Tray Icon
  g_nid.cbSize = sizeof(NOTIFYICONDATAW);
  g_nid.hWnd = g_hWnd;
  g_nid.uID = IDI_TRAY_ICON;
  g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_nid.uCallbackMessage = WM_APP_TRAYMSG;
  g_nid.hIcon = hAudioIcon;
  wcsncpy_s(g_nid.szTip, L"Audio Hardware Volume Preset", _TRUNCATE);

  Shell_NotifyIconW(NIM_ADD, &g_nid);

  // Start Core Audio Worker Thread
  g_hWorkerThread = CreateThread(NULL, 0, AudioMonitorThread, NULL, 0, NULL);
  if (!g_hWorkerThread) {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    return 1;
  }

  // Standard Windows Message Loop
  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  // Clean Termination
  if (g_hStopEvent) SetEvent(g_hStopEvent);

  if (g_hWorkerThread) {
    WaitForSingleObject(g_hWorkerThread, INFINITE);
    CloseHandle(g_hWorkerThread);
    g_hWorkerThread = NULL;
  }

  if (g_hStopEvent) {
    CloseHandle(g_hStopEvent);
    g_hStopEvent = NULL;
  }

  if (g_hWorkEvent) {
    CloseHandle(g_hWorkEvent);
    g_hWorkEvent = NULL;
  }

  if (hAudioIcon) {
    DestroyIcon(hAudioIcon);
  }

  if (hMutex) {
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
  }

  return 0;
}
