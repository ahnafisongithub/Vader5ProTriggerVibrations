#include "vader_hid.h"
#include "hooks_manager.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

// GUID for HID devices: {4D1E55B2-F16F-11CF-88CB-001111000030}
DEFINE_GUID(GUID_HID_INTERFACE, 0x4D1E55B2, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30);

typedef LONG (NTAPI *PFN_NtSetTimerResolution)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);

static volatile HANDLE g_hDevice = INVALID_HANDLE_VALUE;
static HANDLE g_hWorkerThread = NULL;
static volatile BOOL g_bRunning = FALSE;
static DWORD g_outputReportLen = 33;

static volatile LONG g_low_grip = 0;
static volatile LONG g_high_grip = 0;
static volatile LONG g_left_trigger = 0;
static volatile LONG g_right_trigger = 0;
static volatile LONGLONG g_lastSetTick = 0;
static volatile LONGLONG g_lastWorkerHeartbeat = 0;

static CRITICAL_SECTION g_csHidIo;
static volatile LONG g_hidIoInitState = 0;
static uint8_t g_directLastVals[4] = {0, 0, 0, 0};
static int g_directZeroBursts = 0;
static ULONGLONG g_directLastTick = 0;
static ULONGLONG g_directLastFindTick = 0;

// Flydigi VID_37D7&PID_2401 wireless dongle shares radio bandwidth between MI_01 OUT writes
// and MI_00 XInput IN reports; >45Hz OUT writes starve stick/XInput reports to 0Hz.
#define VADER_MIN_TRANSITION_MS   16u
#define VADER_MIN_RAMP_MS         32u
#define VADER_ACTIVE_KEEPALIVE_MS 150u
#define VADER_IDLE_KEEPALIVE_MS   250u
#define VADER_ZERO_BURST_MS       25u
#define VADER_CHANGE_DEADBAND     3

static int AbsByteDiff(uint8_t a, uint8_t b) {
    return (a > b) ? (int)(a - b) : (int)(b - a);
}

static BOOL ShouldSendVaderPacket(
    uint8_t cur_low, uint8_t cur_high, uint8_t cur_lt, uint8_t cur_rt,
    uint8_t last_low, uint8_t last_high, uint8_t last_lt, uint8_t last_rt,
    ULONGLONG elapsedMs, int *zeroBurstsRemaining)
{
    BOOL isActive  = (cur_low > 0 || cur_high > 0 || cur_lt > 0 || cur_rt > 0);
    BOOL wasActive = (last_low > 0 || last_high > 0 || last_lt > 0 || last_rt > 0);

    int maxDelta = AbsByteDiff(cur_low, last_low);
    int dHigh = AbsByteDiff(cur_high, last_high);
    int dLt   = AbsByteDiff(cur_lt, last_lt);
    int dRt   = AbsByteDiff(cur_rt, last_rt);
    if (dHigh > maxDelta) maxDelta = dHigh;
    if (dLt   > maxDelta) maxDelta = dLt;
    if (dRt   > maxDelta) maxDelta = dRt;

    BOOL edgeTransition =
        ((cur_low  == 0) != (last_low  == 0)) ||
        ((cur_high == 0) != (last_high == 0)) ||
        ((cur_lt   == 0) != (last_lt   == 0)) ||
        ((cur_rt   == 0) != (last_rt   == 0));

    if (isActive) {
        if (zeroBurstsRemaining) *zeroBurstsRemaining = 2;
        if ((edgeTransition || maxDelta >= 24) && elapsedMs >= VADER_MIN_TRANSITION_MS) {
            return TRUE;
        }
        if (elapsedMs >= VADER_MIN_RAMP_MS &&
            (maxDelta >= VADER_CHANGE_DEADBAND || (maxDelta > 0 && elapsedMs >= 64))) {
            return TRUE;
        }
        if (elapsedMs >= VADER_ACTIVE_KEEPALIVE_MS) {
            return TRUE;
        }
        return FALSE;
    }

    if (wasActive && zeroBurstsRemaining) {
        *zeroBurstsRemaining = 2;
    }

    if (wasActive || (zeroBurstsRemaining && *zeroBurstsRemaining > 0)) {
        ULONGLONG minWait = wasActive ? VADER_MIN_TRANSITION_MS : VADER_ZERO_BURST_MS;
        if (elapsedMs >= minWait) {
            if (zeroBurstsRemaining && *zeroBurstsRemaining > 0) {
                (*zeroBurstsRemaining)--;
            }
            return TRUE;
        }
        return FALSE;
    }

    return (elapsedMs >= VADER_IDLE_KEEPALIVE_MS);
}

static void EnsureHidIoLock(void) {
    if (InterlockedCompareExchange(&g_hidIoInitState, 1, 0) == 0) {
        InitializeCriticalSection(&g_csHidIo);
        InterlockedExchange(&g_hidIoInitState, 2);
    }
    while (InterlockedCompareExchange(&g_hidIoInitState, 2, 2) != 2) {
        SwitchToThread();
    }
}

static CRITICAL_SECTION g_csLog;
static volatile LONG g_logInitState = 0; // 0=uninit, 1=initing, 2=ready
static volatile LONG g_loggingEnabled = 1;
static char g_logFilePath[MAX_PATH] = "vader_haptics.log";

static void EnsureLogInit(void) {
    if (InterlockedCompareExchange(&g_logInitState, 1, 0) == 0) {
        InitializeCriticalSection(&g_csLog);
        GetModuleFileNameA(NULL, g_logFilePath, MAX_PATH);
        char *slash = strrchr(g_logFilePath, '\\');
        if (slash) {
            strcpy(slash + 1, "vader_haptics.log");
        } else {
            strcpy(g_logFilePath, "vader_haptics.log");
        }
        InterlockedExchange(&g_logInitState, 2);
    } else {
        while (InterlockedCompareExchange(&g_logInitState, 2, 2) != 2) {
            Sleep(0);
        }
    }
}

void Vader_SetLoggingEnabled(BOOL enabled) {
    InterlockedExchange(&g_loggingEnabled, enabled ? 1 : 0);
}

void Vader_Log(const char *format, ...) {
    if (InterlockedCompareExchange(&g_loggingEnabled, 0, 0) == 0) return;
    EnsureLogInit();
    EnterCriticalSection(&g_csLog);
    FILE *f = fopen(g_logFilePath, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list args;
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);
        fprintf(f, "\n");
        fclose(f);
    }
    LeaveCriticalSection(&g_csLog);
}

static BOOL WcsContainsI(const wchar_t *haystack, const wchar_t *needle) {
    if (!haystack || !needle || !*needle) return FALSE;
    size_t nlen = wcslen(needle);
    for (const wchar_t *p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] && towlower(p[i]) == towlower(needle[i])) {
            ++i;
        }
        if (i == nlen) return TRUE;
    }
    return FALSE;
}

static BOOL IsFlydigiPath(const wchar_t *path) {
    if (WcsContainsI(path, L"vid_37d7")) {
        // Match Vader 5 Pro / V2 wired and wireless dongle PIDs (and generic 37d7 controllers)
        return (WcsContainsI(path, L"pid_2401") ||
                WcsContainsI(path, L"pid_2501") ||
                WcsContainsI(path, L"pid_2502") ||
                WcsContainsI(path, L"pid_24")   ||
                WcsContainsI(path, L"pid_25"));
    }
    if (WcsContainsI(path, L"vid_04b4") && WcsContainsI(path, L"pid_2412")) {
        return TRUE;
    }
    return FALSE;
}

static HANDLE OpenHidCandidate(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(path, GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    }
    return h;
}

static HANDLE FindVaderInterface(void) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);

    HANDLE fallbackHandle = INVALID_HANDLE_VALUE;
    DWORD fallbackReportLen = 33;
    wchar_t fallbackPath[512] = {0};

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData); ++i) {
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &requiredSize, NULL);
        if (requiredSize == 0) continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(requiredSize);
        if (!detailData) continue;
        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detailData, requiredSize, NULL, NULL)) {
            const wchar_t *p = detailData->DevicePath;
            if (IsFlydigiPath(p)) {
                HANDLE h = OpenHidCandidate(p);
                if (h != INVALID_HANDLE_VALUE) {
                    PHIDP_PREPARSED_DATA ppd = NULL;
                    HIDP_CAPS caps;
                    ZeroMemory(&caps, sizeof(caps));
                    BOOL haveCaps = FALSE;

                    if (HidD_GetPreparsedData(h, &ppd) && ppd) {
                        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                            haveCaps = TRUE;
                        }
                        HidD_FreePreparsedData(ppd);
                    }

                    if (haveCaps && caps.UsagePage == 0xFFA0 && caps.Usage == 0x0001) {
                        DWORD repLen = (caps.OutputReportByteLength >= 33 && caps.OutputReportByteLength <= 64)
                                           ? (DWORD)caps.OutputReportByteLength
                                           : 33;
                        g_outputReportLen = repLen;
                        Vader_Log("[HID] Connected to Flydigi Vendor HID (UsagePage=0x%04X Usage=0x%04X OutLen=%lu) on %ls",
                                  caps.UsagePage, caps.Usage, repLen, p);
                        if (fallbackHandle != INVALID_HANDLE_VALUE) {
                            CloseHandle(fallbackHandle);
                        }
                        free(detailData);
                        SetupDiDestroyDeviceInfoList(devInfo);
                        return h;
                    }

                    // Fallback candidate if UsagePage wasn't readable or firmware exposes MI_01/MI_02 directly
                    if (fallbackHandle == INVALID_HANDLE_VALUE &&
                        (WcsContainsI(p, L"mi_01") || WcsContainsI(p, L"mi_02")) &&
                        (!haveCaps || caps.OutputReportByteLength >= 32 || caps.OutputReportByteLength == 0)) {
                        fallbackHandle = h;
                        fallbackReportLen = (haveCaps && caps.OutputReportByteLength >= 33 && caps.OutputReportByteLength <= 64)
                                                ? (DWORD)caps.OutputReportByteLength
                                                : 33;
                        wcsncpy(fallbackPath, p, 511);
                        fallbackPath[511] = L'\0';
                    } else {
                        CloseHandle(h);
                    }
                }
            }
        }
        free(detailData);
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    if (fallbackHandle != INVALID_HANDLE_VALUE) {
        g_outputReportLen = fallbackReportLen;
        Vader_Log("[HID] Connected to Flydigi HID fallback (OutLen=%lu) on %ls", g_outputReportLen, fallbackPath);
        return fallbackHandle;
    }

    return INVALID_HANDLE_VALUE;
}

static BOOL SendVaderReport(HANDLE hDev, uint8_t low, uint8_t high, uint8_t lt, uint8_t rt) {
    if (hDev == INVALID_HANDLE_VALUE) return FALSE;

    uint8_t packet[64];
    ZeroMemory(packet, sizeof(packet));
    packet[0] = 0x00; // Report ID 0
    packet[1] = 0x5A; // Flydigi Magic 1
    packet[2] = 0xA5; // Flydigi Magic 2
    packet[3] = 0x12; // 4-Channel Haptic Motor Control Command
    packet[4] = 0x06; // Payload length + 2
    packet[5] = low;  // Left Grip (Heavy low-frequency motor)
    packet[6] = high; // Right Grip (Crisp high-frequency motor)
    packet[7] = lt;   // Left Impulse Trigger motor
    packet[8] = rt;   // Right Impulse Trigger motor
    packet[9] = 0x00;

    DWORD written = 0;
    DWORD len = g_outputReportLen;
    if (len < 33 || len > 64) len = 33;

    return WriteFile(hDev, packet, len, &written, NULL);
}

static void EnableHighPrecisionTimer(void) {
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        PFN_NtSetTimerResolution fnSetTimer = (PFN_NtSetTimerResolution)GetProcAddress(hNtdll, "NtSetTimerResolution");
        if (fnSetTimer) {
            ULONG actual = 0;
            fnSetTimer(10000, TRUE, &actual); // Request 1.0ms timer resolution
        }
    }
}

static void PlayStartupIndicator(HANDLE hDev) {
    if (hDev == INVALID_HANDLE_VALUE) return;

    Hooks_PollLateModules();
    InterlockedExchange64(&g_lastWorkerHeartbeat, (LONGLONG)GetTickCount64());
    if (g_bRunning) Sleep(100);
    InterlockedExchange64(&g_lastWorkerHeartbeat, (LONGLONG)GetTickCount64());
    Hooks_PollLateModules();

    int motorCount = Hooks_DetectGameMotorCount();
    int pulses = (motorCount == 4) ? 4 : 2;
    Vader_Log("[HID] Playing %d x 0.5s L2/R2 trigger launch indicator pulses (%d-motor game)...", pulses, motorCount);

    for (int p = 0; p < pulses && g_bRunning; ++p) {
        InterlockedExchange64(&g_lastWorkerHeartbeat, (LONGLONG)GetTickCount64());
        EnsureHidIoLock();
        EnterCriticalSection(&g_csHidIo);
        BOOL ok = SendVaderReport(hDev, 0, 0, 200, 200);
        LeaveCriticalSection(&g_csHidIo);
        if (!ok) break;
        for (int i = 0; i < 5 && g_bRunning; ++i) {
            if (i == 2) {
                EnsureHidIoLock();
                EnterCriticalSection(&g_csHidIo);
                ok = SendVaderReport(hDev, 0, 0, 200, 200);
                LeaveCriticalSection(&g_csHidIo);
                if (!ok) break;
            }
            Sleep(100);
            InterlockedExchange64(&g_lastWorkerHeartbeat, (LONGLONG)GetTickCount64());
        }

        EnsureHidIoLock();
        EnterCriticalSection(&g_csHidIo);
        SendVaderReport(hDev, 0, 0, 0, 0);
        LeaveCriticalSection(&g_csHidIo);
        Sleep(20);
        EnsureHidIoLock();
        EnterCriticalSection(&g_csHidIo);
        SendVaderReport(hDev, 0, 0, 0, 0);
        LeaveCriticalSection(&g_csHidIo);
        InterlockedExchange64(&g_lastWorkerHeartbeat, (LONGLONG)GetTickCount64());

        if (p + 1 < pulses && g_bRunning) {
            Sleep(180);
            InterlockedExchange64(&g_lastWorkerHeartbeat, (LONGLONG)GetTickCount64());
        }
    }

    Sleep(20);
    EnsureHidIoLock();
    EnterCriticalSection(&g_csHidIo);
    SendVaderReport(hDev, 0, 0, 0, 0);
    LeaveCriticalSection(&g_csHidIo);
    InterlockedExchange64(&g_lastWorkerHeartbeat, (LONGLONG)GetTickCount64());
}

static DWORD WINAPI VaderWorkerThread(LPVOID param) {
    (void)param;
    EnsureHidIoLock();
    InterlockedExchange64(&g_lastWorkerHeartbeat, (LONGLONG)GetTickCount64());
    EnableHighPrecisionTimer();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    Vader_Log("[Worker] HID Worker thread started (radio-safe paced loop).");

    // Hook late-loaded modules immediately so startup hooks are active right away
    Hooks_PollLateModules();

    BOOL startupBuzzDone = FALSE;
    EnterCriticalSection(&g_csHidIo);
    g_hDevice = FindVaderInterface();
    HANDLE hFoundInit = g_hDevice;
    LeaveCriticalSection(&g_csHidIo);
    if (hFoundInit != INVALID_HANDLE_VALUE) {
        PlayStartupIndicator(hFoundInit);
        startupBuzzDone = TRUE;
    }

    uint8_t last_low = 0, last_high = 0, last_lt = 0, last_rt = 0;
    int zero_bursts_remaining = 0;
    ULONGLONG lastSendTick = 0;
    ULONGLONG reconnectTick = GetTickCount64();
    ULONGLONG modulePollTick = GetTickCount64();

    while (g_bRunning) {
        ULONGLONG now = GetTickCount64();
        InterlockedExchange64(&g_lastWorkerHeartbeat, (LONGLONG)now);

        // Periodically scan for late-loaded XInput / GameInput / SDL / combase modules safely outside DllMain
        if (now - modulePollTick >= 500) {
            modulePollTick = now;
            Hooks_PollLateModules();
        }

        HANDLE hDev = g_hDevice;
        if (hDev == INVALID_HANDLE_VALUE) {
            if (now - reconnectTick >= 1500) {
                reconnectTick = now;
                g_hDevice = FindVaderInterface();
                if (g_hDevice != INVALID_HANDLE_VALUE && !startupBuzzDone) {
                    PlayStartupIndicator(g_hDevice);
                    startupBuzzDone = TRUE;
                }
            }
            Sleep(25);
            continue;
        }

        uint8_t cur_low  = (uint8_t)InterlockedCompareExchange(&g_low_grip, 0, 0);
        uint8_t cur_high = (uint8_t)InterlockedCompareExchange(&g_high_grip, 0, 0);
        uint8_t cur_lt   = (uint8_t)InterlockedCompareExchange(&g_left_trigger, 0, 0);
        uint8_t cur_rt   = (uint8_t)InterlockedCompareExchange(&g_right_trigger, 0, 0);

        BOOL is_active = (cur_low > 0 || cur_high > 0 || cur_lt > 0 || cur_rt > 0);
        if (is_active) {
            ULONGLONG lastSet = (ULONGLONG)InterlockedCompareExchange64(&g_lastSetTick, 0, 0);
            if (lastSet > 0 && now >= lastSet && (now - lastSet) > 1200) {
                InterlockedExchange(&g_low_grip, 0);
                InterlockedExchange(&g_high_grip, 0);
                InterlockedExchange(&g_left_trigger, 0);
                InterlockedExchange(&g_right_trigger, 0);
                cur_low = cur_high = cur_lt = cur_rt = 0;
            }
        }

        ULONGLONG elapsedMs = (lastSendTick == 0) ? VADER_IDLE_KEEPALIVE_MS : (now - lastSendTick);
        if (ShouldSendVaderPacket(cur_low, cur_high, cur_lt, cur_rt,
                                  last_low, last_high, last_lt, last_rt,
                                  elapsedMs, &zero_bursts_remaining)) {
            EnsureHidIoLock();
            EnterCriticalSection(&g_csHidIo);
            BOOL sentOk = SendVaderReport(hDev, cur_low, cur_high, cur_lt, cur_rt);
            if (!sentOk) {
                Vader_Log("[HID] WriteFile failed, controller disconnected. Error: %lu", GetLastError());
                CloseHandle(hDev);
                g_hDevice = INVALID_HANDLE_VALUE;
                last_low = last_high = last_lt = last_rt = 0;
                zero_bursts_remaining = 0;
            }
            LeaveCriticalSection(&g_csHidIo);
            if (sentOk) {
                // Measure cooldown after WriteFile returns so the wireless dongle gets a full quiet window
                lastSendTick = GetTickCount64();
                last_low = cur_low;
                last_high = cur_high;
                last_lt = cur_lt;
                last_rt = cur_rt;
            }
        }

        Sleep(4);
    }

    // Double stop burst on exit
    HANDLE hDevExit = (HANDLE)InterlockedExchangePointer((PVOID volatile *)&g_hDevice, INVALID_HANDLE_VALUE);
    if (hDevExit != INVALID_HANDLE_VALUE) {
        SendVaderReport(hDevExit, 0, 0, 0, 0);
        Sleep(10);
        SendVaderReport(hDevExit, 0, 0, 0, 0);
        CloseHandle(hDevExit);
    }

    Vader_Log("[Worker] HID Worker thread stopped cleanly.");
    return 0;
}

void Vader_Init(void) {
    if (g_bRunning) return;
    EnsureHidIoLock();
    Vader_Log("==========================================================");
    Vader_Log("  VaderHaptics v2 - Xbox Elite Series 2 -> Vader 5 Pro V2 ");
    Vader_Log("==========================================================");
    g_bRunning = TRUE;
    InterlockedExchange64(&g_lastWorkerHeartbeat, (LONGLONG)GetTickCount64());
    g_hWorkerThread = CreateThread(NULL, 0, VaderWorkerThread, NULL, 0, NULL);
}

void Vader_SetVibration(uint8_t low_grip, uint8_t high_grip, uint8_t left_trigger, uint8_t right_trigger) {
    InterlockedExchange(&g_low_grip, low_grip);
    InterlockedExchange(&g_high_grip, high_grip);
    InterlockedExchange(&g_left_trigger, left_trigger);
    InterlockedExchange(&g_right_trigger, right_trigger);

    ULONGLONG now = GetTickCount64();
    BOOL active = (low_grip > 0 || high_grip > 0 || left_trigger > 0 || right_trigger > 0);
    if (active) {
        InterlockedExchange64(&g_lastSetTick, (LONGLONG)now);
    }

    // Synchronous fallback: if the worker thread was suspended/terminated (e.g. anti-tamper),
    // dispatch the 4-channel HID report directly so haptics never stall.
    ULONGLONG hb = (ULONGLONG)InterlockedCompareExchange64(&g_lastWorkerHeartbeat, 0, 0);
    if (g_bRunning && (now - hb) > 300) {
        EnsureHidIoLock();
        if (TryEnterCriticalSection(&g_csHidIo)) {
            if (g_hDevice == INVALID_HANDLE_VALUE && (now - g_directLastFindTick) > 2000) {
                g_directLastFindTick = now;
                g_hDevice = FindVaderInterface();
            }
            HANDLE hDev = g_hDevice;
            if (hDev != INVALID_HANDLE_VALUE) {
                ULONGLONG elapsedMs = (g_directLastTick == 0) ? VADER_IDLE_KEEPALIVE_MS : (now - g_directLastTick);
                if (ShouldSendVaderPacket(low_grip, high_grip, left_trigger, right_trigger,
                                          g_directLastVals[0], g_directLastVals[1],
                                          g_directLastVals[2], g_directLastVals[3],
                                          elapsedMs, &g_directZeroBursts)) {
                    if (SendVaderReport(hDev, low_grip, high_grip, left_trigger, right_trigger)) {
                        g_directLastVals[0] = low_grip;
                        g_directLastVals[1] = high_grip;
                        g_directLastVals[2] = left_trigger;
                        g_directLastVals[3] = right_trigger;
                        g_directLastTick = GetTickCount64();
                    } else {
                        CloseHandle(hDev);
                        g_hDevice = INVALID_HANDLE_VALUE;
                    }
                }
            }
            LeaveCriticalSection(&g_csHidIo);
        }
    }
}

BOOL Vader_IsConnected(void) {
    return g_hDevice != INVALID_HANDLE_VALUE;
}

void Vader_EmergencyStop(void) {
    g_bRunning = FALSE;
    HANDLE hDev = (HANDLE)InterlockedExchangePointer((PVOID volatile *)&g_hDevice, INVALID_HANDLE_VALUE);
    if (hDev != INVALID_HANDLE_VALUE) {
        SendVaderReport(hDev, 0, 0, 0, 0);
        SendVaderReport(hDev, 0, 0, 0, 0);
        CloseHandle(hDev);
    }
}

void Vader_Shutdown(void) {
    if (!g_bRunning) return;
    g_bRunning = FALSE;
    if (g_hWorkerThread) {
        WaitForSingleObject(g_hWorkerThread, 800);
        CloseHandle(g_hWorkerThread);
        g_hWorkerThread = NULL;
    }
    Vader_EmergencyStop();
}
