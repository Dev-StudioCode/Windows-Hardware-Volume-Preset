// ==WindhawkMod==
// @id              audio-hardware-volume-preset
// @name            Audio Hardware Volume Preset
// @description     Sets audio playback devices to a configured volume whenever they connect
// @version         1.0.0
// @author          Community
// @github          https://github.com/
// @include         explorer.exe
// @compilerOptions -lole32 -loleaut32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Audio Hardware Volume Preset

Automatically sets your headphones, speakers, Bluetooth headsets, USB DACs, or any other audio playback hardware to a specific preset volume whenever they connect.

## Key Features
- **Prevent Sudden Hearing Blasts:** Never plug in headphones only to be blasted by 100% volume again.
- **Device-Specific Presets:** Set specific volume levels for distinct hardware (e.g., "AirPods" at 30%, "Sony WH-1000XM4" at 25%, "Speakers" at 50%).
- **Global Default Option:** Apply a sensible fallback volume to any newly connected audio device.
- **Auto-Unmute:** Optionally unmute the device on connection.
- **Intelligent Debouncing:** Prevents rapid re-triggering while allowing you to manually change volume afterward.
- **Safe & Efficient:** Runs inside `explorer.exe` using standard Windows Core Audio event notifications (`IMMNotificationClient`). No polling, zero CPU overhead when idle.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- defaultVolume: 30
  $name: Default Volume (%)
  $description: Master volume percentage (0-100) applied when any audio hardware connects, if no specific rule matches.

- applyToAllDevices: true
  $name: Apply to all devices by default
  $description: If enabled, any connected playback device without an explicit rule will use the Default Volume.

- autoUnmute: true
  $name: Unmute on connect
  $description: Automatically unmutes the audio device when setting its volume.

- applyOnStartup: false
  $name: Apply on mod startup
  $description: Immediately set volume for currently connected audio devices when the mod initializes.

- deviceRules:
  - - name: "Headphones"
      $name: Device Name (keyword)
    - volume: 25
      $name: Volume (%)
  - - name: "AirPods"
      $name: Device Name (keyword)
    - volume: 30
      $name: Volume (%)
  - - name: "Speakers"
      $name: Device Name (keyword)
    - volume: 50
      $name: Volume (%)
  $name: Custom Device Presets
  $description: Define custom volume percentages for specific hardware. Matches if the device name contains the specified text (case-insensitive).
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <windhawk_api.h>

#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>

struct DeviceRule {
    std::wstring nameLower;
    int volume;
};

// Global Configuration & State
static SRWLOCK g_lock = SRWLOCK_INIT;
static int g_defaultVolume = 30;
static BOOL g_applyToAllDevices = TRUE;
static BOOL g_autoUnmute = TRUE;
static BOOL g_applyOnStartup = FALSE;
static std::vector<DeviceRule> g_rules;

// Debouncing state to prevent rapid duplicate events from Windows
static std::wstring g_lastDeviceId;
static ULONGLONG g_lastApplyTick = 0;

// COM & Worker Thread handles
static HANDLE g_hWorkerThread = NULL;
static HANDLE g_hStopEvent = NULL;
static IMMDeviceEnumerator* g_pEnumerator = NULL;

static std::wstring ToLower(const std::wstring& str) {
    std::wstring result = str;
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t c) {
        return (wchar_t)towlower(c);
    });
    return result;
}

static void LoadSettings() {
    AcquireSRWLockExclusive(&g_lock);

    g_defaultVolume = Wh_GetIntSetting(L"defaultVolume");
    if (g_defaultVolume < 0) g_defaultVolume = 0;
    if (g_defaultVolume > 100) g_defaultVolume = 100;

    g_applyToAllDevices = Wh_GetIntSetting(L"applyToAllDevices");
    g_autoUnmute = Wh_GetIntSetting(L"autoUnmute");
    g_applyOnStartup = Wh_GetIntSetting(L"applyOnStartup");

    g_rules.clear();
    for (int i = 0;; i++) {
        WCHAR szKey[128];
        swprintf_s(szKey, L"deviceRules[%d].name", i);
        PCWSTR ruleName = Wh_GetStringSetting(szKey);
        if (!ruleName) {
            break;
        }
        if (*ruleName) {
            swprintf_s(szKey, L"deviceRules[%d].volume", i);
            int ruleVol = Wh_GetIntSetting(szKey);
            if (ruleVol < 0) ruleVol = 0;
            if (ruleVol > 100) ruleVol = 100;

            g_rules.push_back({ ToLower(ruleName), ruleVol });
        }
        Wh_FreeStringSetting(ruleName);
    }

    ReleaseSRWLockExclusive(&g_lock);

    Wh_Log(L"Settings loaded. Default volume: %d%%, Apply all: %d, Auto-unmute: %d, Rules count: %zu",
           g_defaultVolume, g_applyToAllDevices, g_autoUnmute, g_rules.size());
}

static void ApplyVolumeToDevice(LPCWSTR pwstrDeviceId) {
    if (!pwstrDeviceId || !g_pEnumerator) {
        return;
    }

    ULONGLONG currentTick = GetTickCount64();

    // Check debounce (ignore duplicate notifications for same device within 1.5s)
    AcquireSRWLockExclusive(&g_lock);
    if (g_lastDeviceId == pwstrDeviceId && (currentTick - g_lastApplyTick) < 1500) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_lastDeviceId = pwstrDeviceId;
    g_lastApplyTick = currentTick;
    ReleaseSRWLockExclusive(&g_lock);

    IMMDevice* pDevice = NULL;
    HRESULT hr = g_pEnumerator->GetDevice(pwstrDeviceId, &pDevice);
    if (FAILED(hr) || !pDevice) {
        return;
    }

    // Ensure it is a render (playback) endpoint, not an input/mic device
    IMMEndpoint* pEndpoint = NULL;
    hr = pDevice->QueryInterface(__uuidof(IMMEndpoint), (void**)&pEndpoint);
    if (SUCCEEDED(hr) && pEndpoint) {
        EDataFlow flow;
        hr = pEndpoint->GetDataFlow(&flow);
        pEndpoint->Release();
        if (SUCCEEDED(hr) && flow != eRender) {
            pDevice->Release();
            return;
        }
    }

    // Retrieve friendly name of the device
    WCHAR szDeviceName[256] = L"Unknown Device";
    IPropertyStore* pProps = NULL;
    hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
    if (SUCCEEDED(hr) && pProps) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
            if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                wcsncpy_s(szDeviceName, varName.pwszVal, _TRUNCATE);
            }
            PropVariantClear(&varName);
        }
        pProps->Release();
    }

    std::wstring nameLower = ToLower(szDeviceName);
    int targetVolume = -1;

    AcquireSRWLockShared(&g_lock);
    // 1. Check custom rules for substring match
    for (const auto& rule : g_rules) {
        if (!rule.nameLower.empty() && nameLower.find(rule.nameLower) != std::wstring::npos) {
            targetVolume = rule.volume;
            Wh_Log(L"Matched rule for \"%s\" (rule: \"%s\") -> Volume %d%%",
                   szDeviceName, rule.nameLower.c_str(), targetVolume);
            break;
        }
    }

    // 2. If no custom rule matched, check if global default applies
    if (targetVolume == -1 && g_applyToAllDevices) {
        targetVolume = g_defaultVolume;
        Wh_Log(L"Applying default volume to \"%s\" -> Volume %d%%", szDeviceName, targetVolume);
    }

    BOOL doUnmute = g_autoUnmute;
    ReleaseSRWLockShared(&g_lock);

    // If neither rule matched nor global default enabled, do not touch device
    if (targetVolume < 0) {
        pDevice->Release();
        return;
    }

    // Activate volume control interface and apply settings
    IAudioEndpointVolume* pEndpointVolume = NULL;
    hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&pEndpointVolume);
    if (SUCCEEDED(hr) && pEndpointVolume) {
        float scalar = (float)targetVolume / 100.0f;
        hr = pEndpointVolume->SetMasterVolumeLevelScalar(scalar, NULL);
        if (SUCCEEDED(hr)) {
            Wh_Log(L"Successfully set volume of \"%s\" to %d%%", szDeviceName, targetVolume);
        } else {
            Wh_Log(L"Failed to set volume of \"%s\" (hr=0x%08X)", szDeviceName, hr);
        }

        if (doUnmute) {
            pEndpointVolume->SetMute(FALSE, NULL);
        }

        pEndpointVolume->Release();
    } else {
        Wh_Log(L"Failed to activate IAudioEndpointVolume for \"%s\" (hr=0x%08X)", szDeviceName, hr);
    }

    pDevice->Release();
}

class AudioNotificationClient : public IMMNotificationClient {
    LONG m_cRef;
public:
    AudioNotificationClient() : m_cRef(1) {}
    virtual ~AudioNotificationClient() {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
        } else if (riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
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
        if (ulRef == 0) {
            delete this;
        }
        return ulRef;
    }

    // Called when a device state changes (e.g., plugged in / Bluetooth connected -> ACTIVE)
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override {
        if (dwNewState == DEVICE_STATE_ACTIVE && pwstrDeviceId) {
            Wh_Log(L"OnDeviceStateChanged (ACTIVE): %s", pwstrDeviceId);
            ApplyVolumeToDevice(pwstrDeviceId);
        }
        return S_OK;
    }

    STDMETHODIMP OnDeviceAdded(LPCWSTR pwstrDeviceId) override {
        return S_OK;
    }

    STDMETHODIMP OnDeviceRemoved(LPCWSTR pwstrDeviceId) override {
        return S_OK;
    }

    // Called when the default output device switches
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId) override {
        if (flow == eRender && pwstrDeviceId) {
            Wh_Log(L"OnDefaultDeviceChanged (flow=Render, role=%d): %s", role, pwstrDeviceId);
            ApplyVolumeToDevice(pwstrDeviceId);
        }
        return S_OK;
    }

    STDMETHODIMP OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override {
        return S_OK;
    }
};

static AudioNotificationClient* g_pClient = NULL;

static void ApplyToCurrentDefaultDevice() {
    if (!g_pEnumerator) return;

    IMMDevice* pDefaultDevice = NULL;
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

static DWORD WINAPI AudioMonitorThread(LPVOID lpParam) {
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hrCom)) {
        Wh_Log(L"CoInitializeEx failed in AudioMonitorThread: 0x%08X", hrCom);
        return 1;
    }

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        NULL,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&g_pEnumerator
    );

    if (SUCCEEDED(hr) && g_pEnumerator) {
        g_pClient = new AudioNotificationClient();
        hr = g_pEnumerator->RegisterEndpointNotificationCallback(g_pClient);
        if (SUCCEEDED(hr)) {
            Wh_Log(L"Audio endpoint notification callback successfully registered.");

            BOOL applyStartup = FALSE;
            AcquireSRWLockShared(&g_lock);
            applyStartup = g_applyOnStartup;
            ReleaseSRWLockShared(&g_lock);

            if (applyStartup) {
                ApplyToCurrentDefaultDevice();
            }

            // Wait until mod uninitialization is requested
            WaitForSingleObject(g_hStopEvent, INFINITE);

            g_pEnumerator->UnregisterEndpointNotificationCallback(g_pClient);
        } else {
            Wh_Log(L"RegisterEndpointNotificationCallback failed: 0x%08X", hr);
        }

        g_pClient->Release();
        g_pClient = NULL;

        g_pEnumerator->Release();
        g_pEnumerator = NULL;
    } else {
        Wh_Log(L"CoCreateInstance MMDeviceEnumerator failed: 0x%08X", hr);
    }

    CoUninitialize();
    return 0;
}

BOOL Wh_ModInit() {
    Wh_Log(L"Audio Hardware Volume Preset initializing...");

    LoadSettings();

    g_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_hStopEvent) {
        Wh_Log(L"CreateEvent failed: %d", GetLastError());
        return FALSE;
    }

    g_hWorkerThread = CreateThread(NULL, 0, AudioMonitorThread, NULL, 0, NULL);
    if (!g_hWorkerThread) {
        Wh_Log(L"CreateThread failed: %d", GetLastError());
        CloseHandle(g_hStopEvent);
        g_hStopEvent = NULL;
        return FALSE;
    }

    Wh_Log(L"Audio Hardware Volume Preset initialized successfully.");
    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L"Audio Hardware Volume Preset uninitializing...");

    if (g_hStopEvent) {
        SetEvent(g_hStopEvent);
    }

    if (g_hWorkerThread) {
        WaitForSingleObject(g_hWorkerThread, 5000);
        CloseHandle(g_hWorkerThread);
        g_hWorkerThread = NULL;
    }

    if (g_hStopEvent) {
        CloseHandle(g_hStopEvent);
        g_hStopEvent = NULL;
    }

    Wh_Log(L"Audio Hardware Volume Preset uninitialized.");
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Settings changed notification received.");
    LoadSettings();
}
