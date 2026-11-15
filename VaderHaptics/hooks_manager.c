#include "hooks_manager.h"
#include "vader_hid.h"
#include "minhook/include/MinHook.h"
#include <windows.h>
#include <objbase.h>
#include <xinput.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Configuration (vader_haptics.ini / X1nput.ini)
// -----------------------------------------------------------------------------
typedef struct {
    float leftMotorStrength;
    float rightMotorStrength;
    float leftTriggerStrength;
    float rightTriggerStrength;
    int   triggerSynthMode; // 0=Off, 1=Smart Auto (Adaptive Synth in 2-motor games only, strict 0 in 4-motor native games) [Default], 2=Linked, 3=Split
} VaderConfig;

static VaderConfig g_cfg = { 1.0f, 1.0f, 1.0f, 1.0f, 1 };

static float ParseStrengthValue(const char *str, float defaultVal) {
    if (!str || !*str) return defaultVal;
    float val = (float)atof(str);
    if (val < 0.0f) val = 0.0f;
    // Support both normalized (e.g., "1.0") and percentage (e.g., "100")
    if (strchr(str, '.') == NULL && val > 2.0f) {
        val /= 100.0f;
    }
    if (val > 2.0f) val = 2.0f;
    return val;
}

static void LoadConfig(void) {
    char iniPath[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, iniPath, MAX_PATH);
    char *slash = strrchr(iniPath, '\\');
    if (slash) *(slash + 1) = '\0';

    char vaderIni[MAX_PATH];
    snprintf(vaderIni, MAX_PATH, "%svader_haptics.ini", iniPath);

    char x1nputIni[MAX_PATH];
    snprintf(x1nputIni, MAX_PATH, "%sX1nput.ini", iniPath);

    char buf[64];
    if (GetFileAttributesA(vaderIni) != INVALID_FILE_ATTRIBUTES) {
        int enableLog = GetPrivateProfileIntA("General", "EnableLogging", 1, vaderIni);
        Vader_SetLoggingEnabled(enableLog != 0);

        GetPrivateProfileStringA("Motors", "LeftMotorStrength", "100", buf, sizeof(buf), vaderIni);
        g_cfg.leftMotorStrength = ParseStrengthValue(buf, 1.0f);

        GetPrivateProfileStringA("Motors", "RightMotorStrength", "100", buf, sizeof(buf), vaderIni);
        g_cfg.rightMotorStrength = ParseStrengthValue(buf, 1.0f);

        GetPrivateProfileStringA("Triggers", "LeftTriggerStrength", "100", buf, sizeof(buf), vaderIni);
        g_cfg.leftTriggerStrength = ParseStrengthValue(buf, 1.0f);

        GetPrivateProfileStringA("Triggers", "RightTriggerStrength", "100", buf, sizeof(buf), vaderIni);
        g_cfg.rightTriggerStrength = ParseStrengthValue(buf, 1.0f);

        g_cfg.triggerSynthMode = GetPrivateProfileIntA("Triggers", "TriggerSynthMode", 1, vaderIni);

        Vader_Log("[Config] Loaded vader_haptics.ini (LG=%.2f RG=%.2f LT=%.2f RT=%.2f Mode=%d Log=%d)",
                  g_cfg.leftMotorStrength, g_cfg.rightMotorStrength,
                  g_cfg.leftTriggerStrength, g_cfg.rightTriggerStrength,
                  g_cfg.triggerSynthMode, enableLog);
    } else if (GetFileAttributesA(x1nputIni) != INVALID_FILE_ATTRIBUTES) {
        GetPrivateProfileStringA("Motors", "LeftStrength", "1.0", buf, sizeof(buf), x1nputIni);
        g_cfg.leftMotorStrength = ParseStrengthValue(buf, 1.0f);

        GetPrivateProfileStringA("Motors", "RightStrength", "1.0", buf, sizeof(buf), x1nputIni);
        g_cfg.rightMotorStrength = ParseStrengthValue(buf, 1.0f);

        GetPrivateProfileStringA("Triggers", "LeftStrength", "1.0", buf, sizeof(buf), x1nputIni);
        g_cfg.leftTriggerStrength = ParseStrengthValue(buf, 1.0f);

        GetPrivateProfileStringA("Triggers", "RightStrength", "1.0", buf, sizeof(buf), x1nputIni);
        g_cfg.rightTriggerStrength = ParseStrengthValue(buf, 1.0f);

        Vader_Log("[Config] Loaded fallback X1nput.ini (LG=%.2f RG=%.2f LT=%.2f RT=%.2f)",
                  g_cfg.leftMotorStrength, g_cfg.rightMotorStrength,
                  g_cfg.leftTriggerStrength, g_cfg.rightTriggerStrength);
    }
}

// -----------------------------------------------------------------------------
// Shared Haptics State & Synchronization
// -----------------------------------------------------------------------------
static CRITICAL_SECTION g_csHooks;
static volatile LONG g_hooksInitState = 0;

static float g_leftTriggerPull = 0.0f;
static float g_rightTriggerPull = 0.0f;

static volatile LONGLONG g_lastNative4ChTick = 0;
static volatile LONGLONG g_lastHapticsSubmitTick = 0;
static volatile LONGLONG g_lastTriggerPullTick = 0;
static volatile LONG g_gameUsesNativeTriggers = 0;
static volatile LONG g_cachedExe4Motor = -1;

static BOOL IsKnown4MotorGameRuntime(void);

static BOOL ShouldSynthesizeLegacyTriggers(void) {
    if (g_cfg.triggerSynthMode == 0) return FALSE;
    if (IsKnown4MotorGameRuntime()) return FALSE;
    return TRUE;
}

static float g_prevLowNorm = 0.0f;
static float g_prevHighNorm = 0.0f;
static float g_impulseEnvLeft = 0.0f;
static float g_impulseEnvRight = 0.0f;

static volatile float g_lastLegacyLow = 0.0f;
static volatile float g_lastLegacyHigh = 0.0f;
static uint8_t g_lastSynthLT = 0;
static uint8_t g_lastSynthRT = 0;

static float Clamp01(float v) {
    if (!(v > 0.0f)) return 0.0f; // Also catches NaN
    if (v > 1.0f) return 1.0f;
    return v;
}

// -----------------------------------------------------------------------------
// 100% Linear 1-to-1 Microsoft Xbox Native Motor Scaling
// Matches Windows.Gaming.Input.dll (xusbdevice.cpp) & GameInputRedist.dll
// (bexinputdevice.cpp): round(Clamp01(val * scale) * 255.0f)
// -----------------------------------------------------------------------------
static uint8_t ScaleMotorByte(float norm, float strengthScale) {
    float x = Clamp01(norm * strengthScale);
    if (x <= 0.0f) return 0;
    float out = x * 255.0f;
    if (out >= 255.0f) return 255;
    return (uint8_t)(out + 0.5f);
}

static void SynthesizeEliteTriggers(float lowNorm, float highNorm, float *outLT, float *outRT) {
    if (g_cfg.triggerSynthMode == 0 || (lowNorm <= 0.01f && highNorm <= 0.01f)) {
        g_prevLowNorm = 0.0f;
        g_prevHighNorm = 0.0f;
        g_impulseEnvLeft = 0.0f;
        g_impulseEnvRight = 0.0f;
        *outLT = 0.0f;
        *outRT = 0.0f;
        return;
    }

    float ltPull = g_leftTriggerPull;
    float rtPull = g_rightTriggerPull;

    if (g_cfg.triggerSynthMode == 2) {
        float combined = fmaxf(lowNorm, highNorm) * 0.7f + ((lowNorm + highNorm) * 0.5f) * 0.3f;
        *outLT = (ltPull > 0.08f) ? powf(ltPull, 0.75f) * combined : 0.0f;
        *outRT = (rtPull > 0.08f) ? powf(rtPull, 0.70f) * combined : 0.0f;
        return;
    }

    if (g_cfg.triggerSynthMode == 3) {
        *outLT = (ltPull > 0.08f) ? powf(ltPull, 0.75f) * lowNorm : 0.0f;
        *outRT = (rtPull > 0.08f) ? powf(rtPull, 0.70f) * highNorm : 0.0f;
        return;
    }

    // Mode 1 (Optional via INI): Adaptive synthesis for 2-motor-only games
    float deltaLow  = (lowNorm  > g_prevLowNorm)  ? (lowNorm  - g_prevLowNorm)  : 0.0f;
    float deltaHigh = (highNorm > g_prevHighNorm) ? (highNorm - g_prevHighNorm) : 0.0f;
    g_prevLowNorm  = lowNorm;
    g_prevHighNorm = highNorm;

    g_impulseEnvLeft  = fmaxf(g_impulseEnvLeft * 0.82f, deltaLow * 0.45f + deltaHigh * 0.35f);
    g_impulseEnvRight = fmaxf(g_impulseEnvRight * 0.82f, deltaHigh * 0.55f + deltaLow * 0.45f);

    if (ltPull > 0.08f) {
        float ltPressure = powf(ltPull, 0.72f);
        float baseLT = fmaxf(lowNorm * 0.85f + highNorm * 0.35f, highNorm * 0.70f);
        *outLT = Clamp01(ltPressure * (baseLT + g_impulseEnvLeft) * 0.85f);
    } else {
        *outLT = 0.0f;
    }

    if (rtPull > 0.08f) {
        float rtPressure = powf(rtPull, 0.68f);
        float baseRT = fmaxf(highNorm * 0.85f + lowNorm * 0.40f, lowNorm * 0.75f);
        *outRT = Clamp01(rtPressure * (baseRT + g_impulseEnvRight) * 0.85f);
    } else {
        *outRT = 0.0f;
    }
}

static void SubmitNative4ChannelHaptics(const char *sourceTag, float leftGrip, float rightGrip, float leftTrig, float rightTrig, uint8_t *outCalLow, uint8_t *outCalHigh) {
    float lg = Clamp01(leftGrip);
    float rg = Clamp01(rightGrip);
    float lt = Clamp01(leftTrig);
    float rt = Clamp01(rightTrig);

    ULONGLONG now = GetTickCount64();
    InterlockedExchange64(&g_lastNative4ChTick, (LONGLONG)now);
    InterlockedExchange64(&g_lastHapticsSubmitTick, (LONGLONG)now);

    if (InterlockedCompareExchange(&g_gameUsesNativeTriggers, 1, 0) == 0) {
        Vader_Log("[%s] Engaging 100%% linear 1-to-1 4-motor passthrough (LG, RG, LT, RT).", sourceTag);
    }

    uint8_t lowByte  = ScaleMotorByte(lg, g_cfg.leftMotorStrength);
    uint8_t highByte = ScaleMotorByte(rg, g_cfg.rightMotorStrength);
    uint8_t ltByte   = ScaleMotorByte(lt, g_cfg.leftTriggerStrength);
    uint8_t rtByte   = ScaleMotorByte(rt, g_cfg.rightTriggerStrength);

    if (outCalLow)  *outCalLow  = lowByte;
    if (outCalHigh) *outCalHigh = highByte;

    static ULONGLONG lastLogTick = 0;
    if (now - lastLogTick > 1000 && (lowByte > 0 || highByte > 0 || ltByte > 0 || rtByte > 0)) {
        lastLogTick = now;
        Vader_Log("[%s] 4-Ch: LT=%3u | RT=%3u | LG=%3u | RG=%3u", sourceTag, ltByte, rtByte, lowByte, highByte);
    }

    Vader_SetVibration(lowByte, highByte, ltByte, rtByte);
}

static void SubmitLegacy2ChannelHaptics(const char *sourceTag, float lowNorm, float highNorm, uint8_t *outCalLow, uint8_t *outCalHigh) {
    float lg = Clamp01(lowNorm);
    float rg = Clamp01(highNorm);
    float lt = 0.0f;
    float rt = 0.0f;

    ULONGLONG now = GetTickCount64();
    InterlockedExchange64(&g_lastHapticsSubmitTick, (LONGLONG)now);

    if (lg <= 0.001f && rg <= 0.001f) {
        g_lastLegacyLow = 0.0f;
        g_lastLegacyHigh = 0.0f;
    } else {
        g_lastLegacyLow = lg;
        g_lastLegacyHigh = rg;
    }

    if (ShouldSynthesizeLegacyTriggers()) {
        SynthesizeEliteTriggers(lg, rg, &lt, &rt);
    }

    uint8_t lowByte  = ScaleMotorByte(lg, g_cfg.leftMotorStrength);
    uint8_t highByte = ScaleMotorByte(rg, g_cfg.rightMotorStrength);
    uint8_t ltByte   = ScaleMotorByte(lt, g_cfg.leftTriggerStrength);
    uint8_t rtByte   = ScaleMotorByte(rt, g_cfg.rightTriggerStrength);

    g_lastSynthLT = ltByte;
    g_lastSynthRT = rtByte;

    if (outCalLow)  *outCalLow  = lowByte;
    if (outCalHigh) *outCalHigh = highByte;

    static ULONGLONG lastLogTick = 0;
    if (now - lastLogTick > 1000 && (lowByte > 0 || highByte > 0 || ltByte > 0 || rtByte > 0)) {
        lastLogTick = now;
        Vader_Log("[%s] 2-Ch: LT=%3u | RT=%3u | LG=%3u | RG=%3u",
                  sourceTag, ltByte, rtByte, lowByte, highByte);
    }

    Vader_SetVibration(lowByte, highByte, ltByte, rtByte);
}

static void RefreshSynthesizedTriggersOnPullChange(void) {
    if (!ShouldSynthesizeLegacyTriggers()) return;
    float low = g_lastLegacyLow;
    float high = g_lastLegacyHigh;
    if (low <= 0.01f && high <= 0.01f) return;

    BOOL ltActive = (g_leftTriggerPull > 0.08f);
    BOOL rtActive = (g_rightTriggerPull > 0.08f);
    if ((g_lastSynthLT > 0) != ltActive || (g_lastSynthRT > 0) != rtActive) {
        SubmitLegacy2ChannelHaptics("PullUpdate", low, high, NULL, NULL);
    }
}

// -----------------------------------------------------------------------------
// Hook Safety & Deduplication Helpers
// -----------------------------------------------------------------------------
#define MAX_HOOKED_TARGETS 192
static void *g_hookedTargets[MAX_HOOKED_TARGETS];
static int g_hookedCount = 0;

static BOOL IsExecutableAddress(const void *p) {
    if (!p) return FALSE;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return FALSE;
    if (mbi.State != MEM_COMMIT) return FALSE;
    DWORD execMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & execMask) != 0;
}

static BOOL IsWritableAddress(const void *p, size_t len) {
    if (!p || len == 0) return FALSE;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return FALSE;
    if (mbi.State != MEM_COMMIT) return FALSE;
    DWORD writeMask = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & writeMask) != 0 && (mbi.Protect & PAGE_GUARD) == 0;
}

static BOOL TryRegisterHook(void *target, void *detour, void **originalOut, const char *label) {
    if (!IsExecutableAddress(target) || target == detour) return FALSE;

    EnterCriticalSection(&g_csHooks);
    for (int i = 0; i < g_hookedCount; ++i) {
        if (g_hookedTargets[i] == target) {
            LeaveCriticalSection(&g_csHooks);
            return FALSE;
        }
    }

    void *orig = NULL;
    MH_STATUS st = MH_CreateHook(target, detour, &orig);
    if (st == MH_OK) {
        if (originalOut && *originalOut == NULL) {
            *originalOut = orig;
        }
        MH_EnableHook(target);
        if (g_hookedCount < MAX_HOOKED_TARGETS) {
            g_hookedTargets[g_hookedCount++] = target;
        }
        LeaveCriticalSection(&g_csHooks);
        Vader_Log("[Hook] %s hooked at %p", label, target);
        return TRUE;
    }

    LeaveCriticalSection(&g_csHooks);
    return FALSE;
}

// -----------------------------------------------------------------------------
// 1. Windows.Gaming.Input (WGI) Hooks
// -----------------------------------------------------------------------------
typedef void* HSTRING;
typedef HRESULT (WINAPI *PFN_WindowsCreateStringReference)(PCWSTR sourceString, UINT32 length, void *hstringHeader, HSTRING *string);
typedef PCWSTR  (WINAPI *PFN_WindowsGetStringRawBuffer)(HSTRING string, UINT32 *length);
static PFN_WindowsGetStringRawBuffer pfn_WindowsGetStringRawBuffer = NULL;
static PFN_WindowsCreateStringReference pfn_WindowsCreateStringReference = NULL;

typedef struct {
    double LeftMotor;
    double RightMotor;
    double LeftTrigger;
    double RightTrigger;
} GamepadVibration;

typedef struct {
    UINT64 Timestamp;
    UINT32 Buttons;
    double LeftTrigger;
    double RightTrigger;
    double LeftThumbstickX;
    double LeftThumbstickY;
    double RightThumbstickX;
    double RightThumbstickY;
} GamepadReading;

static const GUID IID_IGamepadStatics2 = { 0x42676DC5, 0x0856, 0x47C4, { 0x92, 0x13, 0xB3, 0x95, 0x50, 0x4C, 0x3A, 0x3C } };
static const GUID IID_IGamepadStatics  = { 0x8BBCE529, 0xD49C, 0x39E9, { 0x95, 0x60, 0xE4, 0x7D, 0xDE, 0x96, 0xB7, 0xC8 } };
static const GUID IID_IGamepad         = { 0xBC7BB43C, 0x0A69, 0x3903, { 0x9E, 0x9D, 0xA5, 0x0F, 0x86, 0xA4, 0x5D, 0xE5 } };

typedef HRESULT (WINAPI *PFN_RoGetActivationFactory)(HSTRING activatableClassId, REFIID iid, void **factory);
static PFN_RoGetActivationFactory Original_RoGetActivationFactory = NULL;

typedef HRESULT (STDMETHODCALLTYPE *PFN_IUnknown_QueryInterface)(void *This, REFIID riid, void **ppvObject);
typedef ULONG   (STDMETHODCALLTYPE *PFN_IUnknown_Release)(void *This);

typedef HRESULT (STDMETHODCALLTYPE *PFN_put_Vibration)(void *This, GamepadVibration vibration);
typedef HRESULT (STDMETHODCALLTYPE *PFN_WGI_GetCurrentReading)(void *This, GamepadReading *value);
typedef HRESULT (STDMETHODCALLTYPE *PFN_add_GamepadAdded)(void *This, void *value, INT64 *token);
typedef HRESULT (STDMETHODCALLTYPE *PFN_get_Gamepads)(void *This, void **value);
typedef HRESULT (STDMETHODCALLTYPE *PFN_FromGameController)(void *This, void *gameController, void **value);
typedef HRESULT (STDMETHODCALLTYPE *PFN_VectorView_GetAt)(void *This, unsigned int index, void **item);
typedef HRESULT (STDMETHODCALLTYPE *PFN_VectorView_get_Size)(void *This, unsigned int *size);

static PFN_put_Vibration         Original_put_Vibration = NULL;
static PFN_WGI_GetCurrentReading Original_WGI_GetCurrentReading = NULL;
static PFN_add_GamepadAdded      Original_add_GamepadAdded = NULL;
static PFN_get_Gamepads          Original_get_Gamepads = NULL;
static PFN_FromGameController    Original_FromGameController = NULL;
static PFN_VectorView_GetAt      Original_VectorView_GetAt = NULL;

static volatile LONG g_wgiStaticsHooked = 0;

static HRESULT STDMETHODCALLTYPE Detour_put_Vibration(void *This, GamepadVibration v) {
    uint8_t calLow = 0, calHigh = 0;
    SubmitNative4ChannelHaptics("WGI",
                                (float)v.LeftMotor,
                                (float)v.RightMotor,
                                (float)v.LeftTrigger,
                                (float)v.RightTrigger,
                                &calLow, &calHigh);

    if (Original_put_Vibration) {
        return Original_put_Vibration(This, v);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Detour_WGI_GetCurrentReading(void *This, GamepadReading *value) {
    HRESULT hr = Original_WGI_GetCurrentReading(This, value);
    if (SUCCEEDED(hr) && value) {
        g_leftTriggerPull  = Clamp01((float)value->LeftTrigger);
        g_rightTriggerPull = Clamp01((float)value->RightTrigger);
        RefreshSynthesizedTriggersOnPullChange();
    }
    return hr;
}

static void HookGamepadInstance(void *pGamepad) {
    if (!pGamepad) return;
    void **baseVtbl = *(void***)pGamepad;
    if (!baseVtbl) return;

    PFN_IUnknown_QueryInterface fnQI = (PFN_IUnknown_QueryInterface)baseVtbl[0];
    if (IsExecutableAddress((void*)fnQI)) {
        void *pRealGamepad = NULL;
        // Strictly require IID_IGamepad so shared IVectorView::GetAt[6] calls on IRawGameController
        // never corrupt IRawGameController vtable slots 7 & 8!
        if (SUCCEEDED(fnQI(pGamepad, &IID_IGamepad, &pRealGamepad)) && pRealGamepad) {
            void **vtable = *(void***)pRealGamepad;
            if (vtable) {
                // IGamepad vtable: 6=get_Vibration, 7=put_Vibration, 8=GetCurrentReading
                TryRegisterHook(vtable[7], (void*)&Detour_put_Vibration, (void**)&Original_put_Vibration, "IGamepad::put_Vibration[7]");
                TryRegisterHook(vtable[8], (void*)&Detour_WGI_GetCurrentReading, (void**)&Original_WGI_GetCurrentReading, "IGamepad::GetCurrentReading[8]");
            }
            if (vtable && IsExecutableAddress(vtable[2])) {
                ((PFN_IUnknown_Release)vtable[2])(pRealGamepad);
            }
        }
    }
}

static HRESULT STDMETHODCALLTYPE Detour_VectorView_GetAt(void *This, unsigned int index, void **item) {
    HRESULT hr = Original_VectorView_GetAt(This, index, item);
    if (SUCCEEDED(hr) && item && *item) {
        HookGamepadInstance(*item);
    }
    return hr;
}

static void InspectAndHookVectorView(void *pView) {
    if (!pView) return;
    void **viewVtable = *(void***)pView;
    if (!viewVtable) return;

    // Hook IVectorView<Gamepad*>::GetAt (slot 6)
    TryRegisterHook(viewVtable[6], (void*)&Detour_VectorView_GetAt, (void**)&Original_VectorView_GetAt, "IVectorView::GetAt[6]");

    // Immediately enumerate all currently connected WGI gamepads
    PFN_VectorView_get_Size fnGetSize = (PFN_VectorView_get_Size)viewVtable[7];
    PFN_VectorView_GetAt fnGetAt = Original_VectorView_GetAt ? Original_VectorView_GetAt : (PFN_VectorView_GetAt)viewVtable[6];
    if (IsExecutableAddress((void*)fnGetSize) && IsExecutableAddress((void*)fnGetAt)) {
        unsigned int count = 0;
        if (SUCCEEDED(fnGetSize(pView, &count)) && count > 0 && count <= 16) {
            for (unsigned int i = 0; i < count; ++i) {
                void *pGamepad = NULL;
                if (SUCCEEDED(fnGetAt(pView, i, &pGamepad)) && pGamepad) {
                    HookGamepadInstance(pGamepad);
                    void **gpVtbl = *(void***)pGamepad;
                    if (gpVtbl && IsExecutableAddress(gpVtbl[2])) {
                        ((PFN_IUnknown_Release)gpVtbl[2])(pGamepad);
                    }
                }
            }
        }
    }
}

static HRESULT STDMETHODCALLTYPE Detour_get_Gamepads(void *This, void **value) {
    HRESULT hr = Original_get_Gamepads(This, value);
    if (SUCCEEDED(hr) && value && *value) {
        InspectAndHookVectorView(*value);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Detour_add_GamepadAdded(void *This, void *handler, INT64 *token) {
    HRESULT hr = Original_add_GamepadAdded(This, handler, token);
    if (Original_get_Gamepads) {
        void *pView = NULL;
        if (SUCCEEDED(Original_get_Gamepads(This, &pView)) && pView) {
            InspectAndHookVectorView(pView);
            void **viewVtbl = *(void***)pView;
            if (viewVtbl && IsExecutableAddress(viewVtbl[2])) {
                ((PFN_IUnknown_Release)viewVtbl[2])(pView);
            }
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Detour_FromGameController(void *This, void *gameController, void **value) {
    HRESULT hr = Original_FromGameController(This, gameController, value);
    if (SUCCEEDED(hr) && value && *value) {
        HookGamepadInstance(*value);
    }
    return hr;
}

static void HookGamepadStaticsFactory(void *pFactory) {
    if (!pFactory) return;
    void **baseVtbl = *(void***)pFactory;
    if (!baseVtbl) return;

    PFN_IUnknown_QueryInterface fnQI = (PFN_IUnknown_QueryInterface)baseVtbl[0];
    if (!IsExecutableAddress((void*)fnQI)) return;

    void *pStatics = NULL;
    if (SUCCEEDED(fnQI(pFactory, &IID_IGamepadStatics, &pStatics)) && pStatics) {
        void **vtable = *(void***)pStatics;
        if (vtable) {
            // IGamepadStatics: 6=add_GamepadAdded, 10=get_Gamepads
            TryRegisterHook(vtable[6],  (void*)&Detour_add_GamepadAdded, (void**)&Original_add_GamepadAdded, "IGamepadStatics::add_GamepadAdded[6]");
            TryRegisterHook(vtable[10], (void*)&Detour_get_Gamepads,     (void**)&Original_get_Gamepads,     "IGamepadStatics::get_Gamepads[10]");

            PFN_get_Gamepads fnGetGamepads = Original_get_Gamepads ? Original_get_Gamepads : (PFN_get_Gamepads)vtable[10];
            if (IsExecutableAddress((void*)fnGetGamepads)) {
                void *pView = NULL;
                if (SUCCEEDED(fnGetGamepads(pStatics, &pView)) && pView) {
                    InspectAndHookVectorView(pView);
                    void **viewVtbl = *(void***)pView;
                    if (viewVtbl && IsExecutableAddress(viewVtbl[2])) {
                        ((PFN_IUnknown_Release)viewVtbl[2])(pView);
                    }
                }
            }
        }
        if (vtable && IsExecutableAddress(vtable[2])) {
            ((PFN_IUnknown_Release)vtable[2])(pStatics);
        }
        InterlockedExchange(&g_wgiStaticsHooked, 1);
    }

    void *pStatics2 = NULL;
    if (SUCCEEDED(fnQI(pFactory, &IID_IGamepadStatics2, &pStatics2)) && pStatics2) {
        void **vtable2 = *(void***)pStatics2;
        if (vtable2) {
            TryRegisterHook(vtable2[6], (void*)&Detour_FromGameController, (void**)&Original_FromGameController, "IGamepadStatics2::FromGameController[6]");
        }
        if (vtable2 && IsExecutableAddress(vtable2[2])) {
            ((PFN_IUnknown_Release)vtable2[2])(pStatics2);
        }
    }
}

static HRESULT WINAPI Detour_RoGetActivationFactory(HSTRING activatableClassId, REFIID iid, void **factory) {
    HRESULT hr = Original_RoGetActivationFactory(activatableClassId, iid, factory);
    if (SUCCEEDED(hr) && factory && *factory && pfn_WindowsGetStringRawBuffer) {
        UINT32 len = 0;
        PCWSTR name = pfn_WindowsGetStringRawBuffer(activatableClassId, &len);
        if (name && wcscmp(name, L"Windows.Gaming.Input.Gamepad") == 0) {
            Vader_Log("[WGI] Intercepted RoGetActivationFactory(Windows.Gaming.Input.Gamepad)");
            HookGamepadStaticsFactory(*factory);
        }
    }
    return hr;
}

static void TryProactiveWgiHook(void) {
    if (InterlockedCompareExchange(&g_wgiStaticsHooked, 0, 0) != 0 ||
        !Original_RoGetActivationFactory || !pfn_WindowsCreateStringReference) return;
    if (!GetModuleHandleW(L"Windows.Gaming.Input.dll")) return;

    static BOOL s_workerComInited = FALSE;
    if (!s_workerComInited) {
        s_workerComInited = TRUE;
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
    }

    union { PVOID alignPtr; UINT8 reserved[24]; } hstrHeader;
    HSTRING hClassId = NULL;
    const wchar_t *className = L"Windows.Gaming.Input.Gamepad";
    if (SUCCEEDED(pfn_WindowsCreateStringReference(className, (UINT32)wcslen(className), &hstrHeader, &hClassId)) && hClassId) {
        void *pFactory = NULL;
        HRESULT hr = Original_RoGetActivationFactory(hClassId, &IID_IGamepadStatics, &pFactory);
        if (SUCCEEDED(hr) && pFactory) {
            Vader_Log("[WGI] Proactively acquired IGamepadStatics singleton!");
            HookGamepadStaticsFactory(pFactory);
            void **vtbl = *(void***)pFactory;
            if (vtbl && IsExecutableAddress(vtbl[2])) {
                ((PFN_IUnknown_Release)vtbl[2])(pFactory);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 2. GameInput (gameinput.dll & GameInputRedist.dll v0, v1, v2, v3) Hooks
// -----------------------------------------------------------------------------
typedef struct {
    float lowFrequency;
    float highFrequency;
    float leftTrigger;
    float rightTrigger;
} GameInputRumbleParams;

typedef struct {
    uint32_t buttons;
    float leftTrigger;
    float rightTrigger;
    float leftThumbstickX;
    float leftThumbstickY;
    float rightThumbstickX;
    float rightThumbstickY;
} GameInputGamepadState;

// Exact IIDs extracted from GameInputRedist.dll & gameinput.dll .rdata
static const GUID IID_IGameInput_v0        = { 0x11BE2A7E, 0x4254, 0x445A, { 0x9C, 0x09, 0xFF, 0xC4, 0x0F, 0x00, 0x69, 0x18 } };
static const GUID IID_IGameInput_v1        = { 0x40FFB7E4, 0x6150, 0x407A, { 0xB4, 0x39, 0x13, 0x2B, 0xAD, 0xC0, 0x8D, 0x2D } };
static const GUID IID_IGameInput_v2        = { 0xBBAA66D2, 0x837A, 0x40F7, { 0xA3, 0x03, 0x91, 0x7D, 0x50, 0x09, 0x55, 0xF4 } };
static const GUID IID_IGameInput_v3        = { 0x20EFC1C7, 0x5D9A, 0x43BA, { 0xB2, 0x6F, 0xB8, 0x07, 0xFA, 0x48, 0x60, 0x9C } };

static const GUID IID_IGameInputDevice_v0  = { 0x31DD86FB, 0x4C1B, 0x408A, { 0x86, 0x8F, 0x43, 0x9B, 0x3C, 0xD4, 0x71, 0x25 } };
static const GUID IID_IGameInputDevice_v1  = { 0xB169652A, 0x4A32, 0x40D7, { 0x9F, 0xA9, 0x90, 0x59, 0x97, 0x95, 0x25, 0x16 } };
static const GUID IID_IGameInputDevice_v2  = { 0x39A0C9F2, 0xF055, 0x4089, { 0x96, 0xE7, 0x25, 0xB2, 0x75, 0x6D, 0x30, 0x77 } };
static const GUID IID_IGameInputDevice_v3  = { 0x63E2F38B, 0xA399, 0x4275, { 0x8A, 0xE7, 0xD4, 0xC6, 0xE5, 0x24, 0xD1, 0x2A } };

static const GUID IID_IGameInputReading_v0 = { 0x2156947A, 0xE1FA, 0x4DE0, { 0xA3, 0x0B, 0xD8, 0x12, 0x93, 0x1D, 0xBD, 0x8D } };

typedef HRESULT (WINAPI *PFN_GameInputCreate)(void **ppGameInput);
typedef HRESULT (WINAPI *PFN_GameInputInitialize)(REFIID riid, void **ppv);

typedef HRESULT (STDMETHODCALLTYPE *PFN_GI_GetCurrentReading)(void *This, uint32_t inputKind, void *device, void **reading);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GI_GetNextReading)(void *This, void *referenceReading, uint32_t inputKind, void *device, void **reading);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GI_RegisterDeviceCallback)(void *This, void *device, uint32_t inputKind, uint32_t statusFilter, uint32_t enumKind, void *context, void *callbackFunc, uint64_t *callbackToken);
typedef void    (STDMETHODCALLTYPE *PFN_GIReading_GetDevice)(void *This, void **device);
typedef BOOL    (STDMETHODCALLTYPE *PFN_GIReading_GetGamepadState)(void *This, GameInputGamepadState *state);
typedef const uint8_t* (STDMETHODCALLTYPE *PFN_GIDev_GetDeviceInfo_V0)(void *This);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GIDev_GetDeviceInfo_V123)(void *This, const uint8_t **info);
typedef void    (STDMETHODCALLTYPE *PFN_SetRumbleState)(void *This, const GameInputRumbleParams *params);
typedef void    (CALLBACK *PFN_GameInputDeviceCallback)(uint64_t token, void *context, void *device, uint64_t timestamp, uint32_t currentStatus, uint32_t previousStatus);

static PFN_GameInputCreate     Original_GameInputCreate_V0 = NULL;
static PFN_GameInputCreate     Original_GameInputCreate_Redist = NULL;
static PFN_GameInputInitialize Original_GameInputInitialize_Redist = NULL;

#define MAX_GI_SLOTS 8
static PFN_GI_GetCurrentReading      g_origGIGetCur[MAX_GI_SLOTS];
static PFN_GI_GetNextReading         g_origGIGetNext[MAX_GI_SLOTS];
static PFN_GI_RegisterDeviceCallback g_origGIRegCb[MAX_GI_SLOTS];
static PFN_SetRumbleState            g_origGISetRumble[MAX_GI_SLOTS];
static int g_giGetCurCount = 0;
static int g_giGetNextCount = 0;
static int g_giRegCbCount = 0;
static int g_giSetRumbleCount = 0;

static void HandleGISetRumbleSlot(int slot, void *This, const GameInputRumbleParams *params) {
    if (params) {
        SubmitNative4ChannelHaptics("GameInput",
                                    params->lowFrequency,
                                    params->highFrequency,
                                    params->leftTrigger,
                                    params->rightTrigger,
                                    NULL, NULL);
    } else {
        SubmitNative4ChannelHaptics("GameInput", 0.0f, 0.0f, 0.0f, 0.0f, NULL, NULL);
    }
    if (g_origGISetRumble[slot]) {
        g_origGISetRumble[slot](This, params);
    }
}

#define DEFINE_GI_RUMBLE_SLOT(N) \
static void STDMETHODCALLTYPE Detour_GISetRumble_##N(void *This, const GameInputRumbleParams *params) { \
    HandleGISetRumbleSlot(N, This, params); \
}

DEFINE_GI_RUMBLE_SLOT(0)
DEFINE_GI_RUMBLE_SLOT(1)
DEFINE_GI_RUMBLE_SLOT(2)
DEFINE_GI_RUMBLE_SLOT(3)
DEFINE_GI_RUMBLE_SLOT(4)
DEFINE_GI_RUMBLE_SLOT(5)
DEFINE_GI_RUMBLE_SLOT(6)
DEFINE_GI_RUMBLE_SLOT(7)

static void *g_giSetRumbleDetours[MAX_GI_SLOTS] = {
    (void*)Detour_GISetRumble_0, (void*)Detour_GISetRumble_1, (void*)Detour_GISetRumble_2, (void*)Detour_GISetRumble_3,
    (void*)Detour_GISetRumble_4, (void*)Detour_GISetRumble_5, (void*)Detour_GISetRumble_6, (void*)Detour_GISetRumble_7
};

static void TryHookGISetRumble(void *target, const char *desc) {
    if (!IsExecutableAddress(target)) return;
    EnterCriticalSection(&g_csHooks);
    for (int i = 0; i < g_hookedCount; ++i) {
        if (g_hookedTargets[i] == target) {
            LeaveCriticalSection(&g_csHooks);
            return;
        }
    }
    if (g_giSetRumbleCount < MAX_GI_SLOTS) {
        int slot = g_giSetRumbleCount;
        void *orig = NULL;
        if (MH_CreateHook(target, g_giSetRumbleDetours[slot], &orig) == MH_OK) {
            g_origGISetRumble[slot] = (PFN_SetRumbleState)orig;
            g_giSetRumbleCount++;
            if (g_hookedCount < MAX_HOOKED_TARGETS) g_hookedTargets[g_hookedCount++] = target;
            MH_EnableHook(target);
            Vader_Log("[Hook] %s[%d] hooked at %p", desc, slot, target);
        }
    }
    LeaveCriticalSection(&g_csHooks);

    // If target is a 9-byte adjustor thunk `48 83 E9 XX E9 disp32`, also hook the underlying implementation!
    const uint8_t *code = (const uint8_t *)target;
    if (code[0] == 0x48 && code[1] == 0x83 && code[2] == 0xE9 && code[4] == 0xE9) {
        int32_t disp = *(const int32_t *)(code + 5);
        void *realImpl = (void *)(code + 9 + disp);
        if (IsExecutableAddress(realImpl)) {
            TryHookGISetRumble(realImpl, "IGameInputDevice::SetRumbleState(Impl)");
        }
    }
}

static void UpgradeGameInputDeviceRumbleCaps(const uint8_t *info, size_t inputOff, size_t rumbleOff, const char *verTag) {
    if (!info) return;
    if (!IsWritableAddress(info + rumbleOff, sizeof(uint32_t))) return;
    uint32_t supportedInput = *(const uint32_t *)(info + inputOff);
    uint32_t supportedRumble = *(const uint32_t *)(info + rumbleOff);
    // GameInputKindGamepad = 0x00040000; GameInputRumbleLowFrequency(1) | HighFrequency(2) = 0x03
    if ((supportedInput & 0x00040000u) != 0 && (supportedRumble & 0x03u) != 0 && (supportedRumble & 0x0Cu) != 0x0Cu) {
        *(uint32_t *)(info + rumbleOff) = supportedRumble | 0x0Fu;
        Vader_Log("[GameInput] Upgraded %s GameInputDeviceInfo->supportedRumbleMotors: 0x%X -> 0x0F (4-Motor Impulse Triggers enabled)",
                  verTag, supportedRumble);
    }
}

static void HookGameInputDevice(void *pDevice) {
    if (!pDevice) return;
    void **baseVtbl = *(void***)pDevice;
    if (!baseVtbl) return;

    PFN_IUnknown_QueryInterface fnQI = (PFN_IUnknown_QueryInterface)baseVtbl[0];
    if (!IsExecutableAddress((void*)fnQI)) return;

    // 1. IGameInputDevice v0: GetDeviceInfo[3] returns ptr, SetRumbleState[10]
    void *pDevV0 = NULL;
    if (SUCCEEDED(fnQI(pDevice, &IID_IGameInputDevice_v0, &pDevV0)) && pDevV0) {
        void **vtbl0 = *(void***)pDevV0;
        if (vtbl0) {
            TryHookGISetRumble(vtbl0[10], "IGameInputDevice(v0)::SetRumbleState[10]");
            if (IsExecutableAddress(vtbl0[3])) {
                const uint8_t *info0 = ((PFN_GIDev_GetDeviceInfo_V0)vtbl0[3])(pDevV0);
                UpgradeGameInputDeviceRumbleCaps(info0, 0x68, 0x6C, "v0");
            }
        }
        if (vtbl0 && IsExecutableAddress(vtbl0[2])) ((PFN_IUnknown_Release)vtbl0[2])(pDevV0);
    }

    // 2. IGameInputDevice v1: GetDeviceInfo[3] outputs ptr, SetRumbleState[8]
    void *pDevV1 = NULL;
    if (SUCCEEDED(fnQI(pDevice, &IID_IGameInputDevice_v1, &pDevV1)) && pDevV1) {
        void **vtbl1 = *(void***)pDevV1;
        if (vtbl1) {
            TryHookGISetRumble(vtbl1[8], "IGameInputDevice(v1)::SetRumbleState[8]");
            if (IsExecutableAddress(vtbl1[3])) {
                const uint8_t *info1 = NULL;
                if (SUCCEEDED(((PFN_GIDev_GetDeviceInfo_V123)vtbl1[3])(pDevV1, &info1)) && info1) {
                    UpgradeGameInputDeviceRumbleCaps(info1, 0x4C, 0x50, "v1");
                }
            }
        }
        if (vtbl1 && IsExecutableAddress(vtbl1[2])) ((PFN_IUnknown_Release)vtbl1[2])(pDevV1);
    }

    // 3. IGameInputDevice v2: GetDeviceInfo[3] outputs ptr, SetRumbleState[9]
    void *pDevV2 = NULL;
    if (SUCCEEDED(fnQI(pDevice, &IID_IGameInputDevice_v2, &pDevV2)) && pDevV2) {
        void **vtbl2 = *(void***)pDevV2;
        if (vtbl2) {
            TryHookGISetRumble(vtbl2[9], "IGameInputDevice(v2)::SetRumbleState[9]");
            if (IsExecutableAddress(vtbl2[3])) {
                const uint8_t *info2 = NULL;
                if (SUCCEEDED(((PFN_GIDev_GetDeviceInfo_V123)vtbl2[3])(pDevV2, &info2)) && info2) {
                    UpgradeGameInputDeviceRumbleCaps(info2, 0x60, 0x64, "v2");
                }
            }
        }
        if (vtbl2 && IsExecutableAddress(vtbl2[2])) ((PFN_IUnknown_Release)vtbl2[2])(pDevV2);
    }

    // 4. IGameInputDevice v3: GetDeviceInfo[3] outputs ptr, SetRumbleState[9]
    void *pDevV3 = NULL;
    if (SUCCEEDED(fnQI(pDevice, &IID_IGameInputDevice_v3, &pDevV3)) && pDevV3) {
        void **vtbl3 = *(void***)pDevV3;
        if (vtbl3) {
            TryHookGISetRumble(vtbl3[9], "IGameInputDevice(v3)::SetRumbleState[9]");
            if (IsExecutableAddress(vtbl3[3])) {
                const uint8_t *info3 = NULL;
                if (SUCCEEDED(((PFN_GIDev_GetDeviceInfo_V123)vtbl3[3])(pDevV3, &info3)) && info3) {
                    UpgradeGameInputDeviceRumbleCaps(info3, 0x60, 0x64, "v3");
                }
            }
        }
        if (vtbl3 && IsExecutableAddress(vtbl3[2])) ((PFN_IUnknown_Release)vtbl3[2])(pDevV3);
    }
}

static void ExtractAndHookDeviceFromReading(void *pReading) {
    if (!pReading) return;
    void **baseVtbl = *(void***)pReading;
    if (!baseVtbl) return;

    PFN_IUnknown_QueryInterface fnQI = (PFN_IUnknown_QueryInterface)baseVtbl[0];
    if (!IsExecutableAddress((void*)fnQI)) return;

    void *pReadV0 = NULL;
    if (SUCCEEDED(fnQI(pReading, &IID_IGameInputReading_v0, &pReadV0)) && pReadV0) {
        void **vtbl0 = *(void***)pReadV0;
        if (vtbl0) {
            // IGameInputReading v0: 6=GetDevice, 22=GetGamepadState
            if (IsExecutableAddress(vtbl0[22])) {
                GameInputGamepadState state;
                ZeroMemory(&state, sizeof(state));
                if (((PFN_GIReading_GetGamepadState)vtbl0[22])(pReadV0, &state)) {
                    g_leftTriggerPull  = Clamp01(state.leftTrigger);
                    g_rightTriggerPull = Clamp01(state.rightTrigger);
                    RefreshSynthesizedTriggersOnPullChange();
                }
            }
            if (IsExecutableAddress(vtbl0[6])) {
                void *pDevice = NULL;
                ((PFN_GIReading_GetDevice)vtbl0[6])(pReadV0, &pDevice);
                if (pDevice) {
                    HookGameInputDevice(pDevice);
                    void **devVtbl = *(void***)pDevice;
                    if (devVtbl && IsExecutableAddress(devVtbl[2])) {
                        ((PFN_IUnknown_Release)devVtbl[2])(pDevice);
                    }
                }
            }
        }
        if (vtbl0 && IsExecutableAddress(vtbl0[2])) {
            ((PFN_IUnknown_Release)vtbl0[2])(pReadV0);
        }
    }
}

static HRESULT HandleGIGetCurrentReadingSlot(int slot, void *This, uint32_t kind, void *dev, void **reading) {
    if (dev) HookGameInputDevice(dev);
    HRESULT hr = g_origGIGetCur[slot](This, kind, dev, reading);
    if (SUCCEEDED(hr) && reading && *reading) {
        ExtractAndHookDeviceFromReading(*reading);
    }
    return hr;
}

static HRESULT HandleGIGetNextReadingSlot(int slot, void *This, void *refReading, uint32_t kind, void *dev, void **reading) {
    if (dev) HookGameInputDevice(dev);
    HRESULT hr = g_origGIGetNext[slot](This, refReading, kind, dev, reading);
    if (SUCCEEDED(hr) && reading && *reading) {
        ExtractAndHookDeviceFromReading(*reading);
    }
    return hr;
}

#define MAX_GI_CALLBACKS 8
static PFN_GameInputDeviceCallback g_gameDevCallbacks[MAX_GI_CALLBACKS];
static int g_gameDevCallbackCount = 0;

static void HandleGameDevCallback(int idx, uint64_t token, void *context, void *device, uint64_t ts, uint32_t curSt, uint32_t prevSt) {
    if (device) {
        HookGameInputDevice(device);
    }
    if (g_gameDevCallbacks[idx]) {
        g_gameDevCallbacks[idx](token, context, device, ts, curSt, prevSt);
    }
}

#define DEFINE_GI_CB_SLOT(N) \
static void CALLBACK Detour_GI_DevCb_##N(uint64_t tok, void *ctx, void *dev, uint64_t ts, uint32_t cs, uint32_t ps) { \
    HandleGameDevCallback(N, tok, ctx, dev, ts, cs, ps); \
}

DEFINE_GI_CB_SLOT(0)
DEFINE_GI_CB_SLOT(1)
DEFINE_GI_CB_SLOT(2)
DEFINE_GI_CB_SLOT(3)
DEFINE_GI_CB_SLOT(4)
DEFINE_GI_CB_SLOT(5)
DEFINE_GI_CB_SLOT(6)
DEFINE_GI_CB_SLOT(7)

static void *g_giCbDetours[MAX_GI_CALLBACKS] = {
    (void*)Detour_GI_DevCb_0, (void*)Detour_GI_DevCb_1, (void*)Detour_GI_DevCb_2, (void*)Detour_GI_DevCb_3,
    (void*)Detour_GI_DevCb_4, (void*)Detour_GI_DevCb_5, (void*)Detour_GI_DevCb_6, (void*)Detour_GI_DevCb_7
};

static void HookRegisteredGameCallback(void *callbackFunc) {
    if (!IsExecutableAddress(callbackFunc)) return;
    EnterCriticalSection(&g_csHooks);
    for (int i = 0; i < g_hookedCount; ++i) {
        if (g_hookedTargets[i] == callbackFunc) {
            LeaveCriticalSection(&g_csHooks);
            return;
        }
    }
    if (g_gameDevCallbackCount < MAX_GI_CALLBACKS) {
        int slot = g_gameDevCallbackCount;
        void *orig = NULL;
        if (MH_CreateHook(callbackFunc, g_giCbDetours[slot], &orig) == MH_OK) {
            g_gameDevCallbacks[slot] = (PFN_GameInputDeviceCallback)orig;
            g_gameDevCallbackCount++;
            if (g_hookedCount < MAX_HOOKED_TARGETS) g_hookedTargets[g_hookedCount++] = callbackFunc;
            MH_EnableHook(callbackFunc);
            Vader_Log("[GameInput] Hooked GameInputDeviceCallback[%d] at %p", slot, callbackFunc);
        }
    }
    LeaveCriticalSection(&g_csHooks);
}

static void CALLBACK InternalEnumDevCallback(uint64_t tok, void *ctx, void *dev, uint64_t ts, uint32_t cs, uint32_t ps);

static HRESULT HandleGIRegisterDevCbSlot(int slot, void *This, void *dev, uint32_t kind, uint32_t status, uint32_t enumKind, void *ctx, void *cb, uint64_t *tok) {
    if (dev) HookGameInputDevice(dev);
    if (cb && cb != (void*)&InternalEnumDevCallback) HookRegisteredGameCallback(cb);
    return g_origGIRegCb[slot](This, dev, kind, status, enumKind, ctx, cb, tok);
}

#define DEFINE_GI_METHOD_SLOTS(N) \
static HRESULT STDMETHODCALLTYPE Detour_GIGetCur_##N(void *This, uint32_t kind, void *dev, void **reading) { \
    return HandleGIGetCurrentReadingSlot(N, This, kind, dev, reading); \
} \
static HRESULT STDMETHODCALLTYPE Detour_GIGetNext_##N(void *This, void *ref, uint32_t kind, void *dev, void **reading) { \
    return HandleGIGetNextReadingSlot(N, This, ref, kind, dev, reading); \
} \
static HRESULT STDMETHODCALLTYPE Detour_GIRegCb_##N(void *This, void *dev, uint32_t kind, uint32_t st, uint32_t ek, void *ctx, void *cb, uint64_t *tok) { \
    return HandleGIRegisterDevCbSlot(N, This, dev, kind, st, ek, ctx, cb, tok); \
}

DEFINE_GI_METHOD_SLOTS(0)
DEFINE_GI_METHOD_SLOTS(1)
DEFINE_GI_METHOD_SLOTS(2)
DEFINE_GI_METHOD_SLOTS(3)
DEFINE_GI_METHOD_SLOTS(4)
DEFINE_GI_METHOD_SLOTS(5)
DEFINE_GI_METHOD_SLOTS(6)
DEFINE_GI_METHOD_SLOTS(7)

static void *g_giGetCurDetours[MAX_GI_SLOTS] = {
    (void*)Detour_GIGetCur_0, (void*)Detour_GIGetCur_1, (void*)Detour_GIGetCur_2, (void*)Detour_GIGetCur_3,
    (void*)Detour_GIGetCur_4, (void*)Detour_GIGetCur_5, (void*)Detour_GIGetCur_6, (void*)Detour_GIGetCur_7
};
static void *g_giGetNextDetours[MAX_GI_SLOTS] = {
    (void*)Detour_GIGetNext_0, (void*)Detour_GIGetNext_1, (void*)Detour_GIGetNext_2, (void*)Detour_GIGetNext_3,
    (void*)Detour_GIGetNext_4, (void*)Detour_GIGetNext_5, (void*)Detour_GIGetNext_6, (void*)Detour_GIGetNext_7
};
static void *g_giRegCbDetours[MAX_GI_SLOTS] = {
    (void*)Detour_GIRegCb_0, (void*)Detour_GIRegCb_1, (void*)Detour_GIRegCb_2, (void*)Detour_GIRegCb_3,
    (void*)Detour_GIRegCb_4, (void*)Detour_GIRegCb_5, (void*)Detour_GIRegCb_6, (void*)Detour_GIRegCb_7
};

static void TryHookGIGetCur(void *target, const char *desc) {
    if (!IsExecutableAddress(target)) return;
    EnterCriticalSection(&g_csHooks);
    for (int i = 0; i < g_hookedCount; ++i) {
        if (g_hookedTargets[i] == target) { LeaveCriticalSection(&g_csHooks); return; }
    }
    if (g_giGetCurCount < MAX_GI_SLOTS) {
        int slot = g_giGetCurCount;
        void *orig = NULL;
        if (MH_CreateHook(target, g_giGetCurDetours[slot], &orig) == MH_OK) {
            g_origGIGetCur[slot] = (PFN_GI_GetCurrentReading)orig;
            g_giGetCurCount++;
            if (g_hookedCount < MAX_HOOKED_TARGETS) g_hookedTargets[g_hookedCount++] = target;
            MH_EnableHook(target);
            Vader_Log("[Hook] %s[%d] hooked at %p", desc, slot, target);
        }
    }
    LeaveCriticalSection(&g_csHooks);
}

static void TryHookGIGetNext(void *target, const char *desc) {
    if (!IsExecutableAddress(target)) return;
    EnterCriticalSection(&g_csHooks);
    for (int i = 0; i < g_hookedCount; ++i) {
        if (g_hookedTargets[i] == target) { LeaveCriticalSection(&g_csHooks); return; }
    }
    if (g_giGetNextCount < MAX_GI_SLOTS) {
        int slot = g_giGetNextCount;
        void *orig = NULL;
        if (MH_CreateHook(target, g_giGetNextDetours[slot], &orig) == MH_OK) {
            g_origGIGetNext[slot] = (PFN_GI_GetNextReading)orig;
            g_giGetNextCount++;
            if (g_hookedCount < MAX_HOOKED_TARGETS) g_hookedTargets[g_hookedCount++] = target;
            MH_EnableHook(target);
            Vader_Log("[Hook] %s[%d] hooked at %p", desc, slot, target);
        }
    }
    LeaveCriticalSection(&g_csHooks);
}

static void TryHookGIRegCb(void *target, const char *desc) {
    if (!IsExecutableAddress(target)) return;
    EnterCriticalSection(&g_csHooks);
    for (int i = 0; i < g_hookedCount; ++i) {
        if (g_hookedTargets[i] == target) { LeaveCriticalSection(&g_csHooks); return; }
    }
    if (g_giRegCbCount < MAX_GI_SLOTS) {
        int slot = g_giRegCbCount;
        void *orig = NULL;
        if (MH_CreateHook(target, g_giRegCbDetours[slot], &orig) == MH_OK) {
            g_origGIRegCb[slot] = (PFN_GI_RegisterDeviceCallback)orig;
            g_giRegCbCount++;
            if (g_hookedCount < MAX_HOOKED_TARGETS) g_hookedTargets[g_hookedCount++] = target;
            MH_EnableHook(target);
            Vader_Log("[Hook] %s[%d] hooked at %p", desc, slot, target);
        }
    }
    LeaveCriticalSection(&g_csHooks);
}

static void CALLBACK InternalEnumDevCallback(uint64_t tok, void *ctx, void *dev, uint64_t ts, uint32_t cs, uint32_t ps) {
    (void)tok; (void)ctx; (void)ts; (void)cs; (void)ps;
    if (dev) {
        HookGameInputDevice(dev);
    }
}

static void HookIGameInputInstance(void *pGameInput) {
    if (!pGameInput) return;
    void **baseVtbl = *(void***)pGameInput;
    if (!baseVtbl) return;

    PFN_IUnknown_QueryInterface fnQI = (PFN_IUnknown_QueryInterface)baseVtbl[0];
    if (!IsExecutableAddress((void*)fnQI)) return;

    const struct {
        const GUID *iid;
        int regCbIdx;
        const char *verName;
    } versions[4] = {
        { &IID_IGameInput_v0, 9, "v0" },
        { &IID_IGameInput_v1, 8, "v1" },
        { &IID_IGameInput_v2, 8, "v2" },
        { &IID_IGameInput_v3, 8, "v3" }
    };

    BOOL enumeratedDevices = FALSE;

    for (int i = 0; i < 4; ++i) {
        void *pGI = NULL;
        if (SUCCEEDED(fnQI(pGameInput, versions[i].iid, &pGI)) && pGI) {
            void **vtbl = *(void***)pGI;
            if (vtbl) {
                // Proactively enumerate existing GameInput gamepad devices BEFORE hooking RegisterDeviceCallback
                // so SetRumbleState is hooked and supportedRumbleMotors is upgraded to 0x0F immediately!
                if (!enumeratedDevices && IsExecutableAddress(vtbl[versions[i].regCbIdx])) {
                    PFN_GI_RegisterDeviceCallback fnReg = (PFN_GI_RegisterDeviceCallback)vtbl[versions[i].regCbIdx];
                    uint64_t tok = 0;
                    // GameInputKindGamepad=0x40000, Connected=0x1, BlockingEnumeration=0x1
                    if (SUCCEEDED(fnReg(pGI, NULL, 0x00040000u, 0x00000001u, 0x00000001u, NULL, (void*)&InternalEnumDevCallback, &tok))) {
                        enumeratedDevices = TRUE;
                    }
                }

                char labelCur[64], labelNext[64], labelCb[64];
                snprintf(labelCur,  sizeof(labelCur),  "IGameInput(%s)::GetCurrentReading[4]", versions[i].verName);
                snprintf(labelNext, sizeof(labelNext), "IGameInput(%s)::GetNextReading[5]",    versions[i].verName);
                snprintf(labelCb,   sizeof(labelCb),   "IGameInput(%s)::RegisterDeviceCallback[%d]", versions[i].verName, versions[i].regCbIdx);

                TryHookGIGetCur(vtbl[4], labelCur);
                TryHookGIGetNext(vtbl[5], labelNext);
                TryHookGIRegCb(vtbl[versions[i].regCbIdx], labelCb);
            }
            if (vtbl && IsExecutableAddress(vtbl[2])) {
                ((PFN_IUnknown_Release)vtbl[2])(pGI);
            }
        }
    }
}

static HRESULT WINAPI Detour_GameInputCreate_V0(void **ppGameInput) {
    HRESULT hr = Original_GameInputCreate_V0(ppGameInput);
    if (SUCCEEDED(hr) && ppGameInput && *ppGameInput) {
        Vader_Log("[GameInput] GameInputCreate (v0) succeeded -> hooking IGameInput v0..v3");
        HookIGameInputInstance(*ppGameInput);
    }
    return hr;
}

static HRESULT WINAPI Detour_GameInputCreate_Redist(void **ppGameInput) {
    HRESULT hr = Original_GameInputCreate_Redist(ppGameInput);
    if (SUCCEEDED(hr) && ppGameInput && *ppGameInput) {
        Vader_Log("[GameInput] GameInputCreate (Redist) succeeded -> hooking IGameInput v0..v3");
        HookIGameInputInstance(*ppGameInput);
    }
    return hr;
}

static HRESULT WINAPI Detour_GameInputInitialize_Redist(REFIID riid, void **ppv) {
    HRESULT hr = Original_GameInputInitialize_Redist(riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        Vader_Log("[GameInput] GameInputInitialize (Redist) succeeded -> hooking IGameInput v0..v3");
        HookIGameInputInstance(*ppv);
    }
    return hr;
}

// -----------------------------------------------------------------------------
// 3. Multi-Module XInput Hooks (Named + Ordinal 2/3 + Ordinal 100 GetStateEx)
// -----------------------------------------------------------------------------
typedef DWORD (WINAPI *PFN_XInputSetState)(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration);
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD dwUserIndex, XINPUT_STATE *pState);

#define MAX_XINPUT_SLOTS 12
static PFN_XInputSetState g_origXInputSet[MAX_XINPUT_SLOTS];
static PFN_XInputGetState g_origXInputGet[MAX_XINPUT_SLOTS];
static int g_xinputSetSlotCount = 0;
static int g_xinputGetSlotCount = 0;
static volatile LONG g_activeXInputSlot = 0;
static __thread int t_inXInputGet = 0;
static __thread int t_inXInputSet = 0;

static DWORD HandleXInputGetState(int slot, DWORD dwUserIndex, XINPUT_STATE *pState) {
    if (t_inXInputGet > 0) {
        return g_origXInputGet[slot](dwUserIndex, pState);
    }
    t_inXInputGet++;
    DWORD res = g_origXInputGet[slot](dwUserIndex, pState);
    if (res == ERROR_SUCCESS && pState && dwUserIndex < 4) {
        InterlockedExchange(&g_activeXInputSlot, (LONG)dwUserIndex);
        g_leftTriggerPull  = pState->Gamepad.bLeftTrigger / 255.0f;
        g_rightTriggerPull = pState->Gamepad.bRightTrigger / 255.0f;
        InterlockedExchange64(&g_lastTriggerPullTick, (LONGLONG)GetTickCount64());
        RefreshSynthesizedTriggersOnPullChange();
    }
    t_inXInputGet--;
    return res;
}

static DWORD HandleXInputSetState(int slot, DWORD dwUserIndex, XINPUT_VIBRATION *pVibration) {
    if (t_inXInputSet > 0) {
        return g_origXInputSet[slot](dwUserIndex, pVibration);
    }
    t_inXInputSet++;
    if (pVibration && dwUserIndex < 4) {
        ULONGLONG now = GetTickCount64();
        ULONGLONG lastNative = (ULONGLONG)InterlockedCompareExchange64(&g_lastNative4ChTick, 0, 0);

        // If native 4-channel haptics (WGI / GameInput / Steam Extended / SDL) is actively driving triggers,
        // do not overwrite them with 2-channel XInput fallback.
        if (InterlockedCompareExchange(&g_gameUsesNativeTriggers, 0, 0) != 0 && (now - lastNative) < 1500) {
            DWORD r = g_origXInputSet[slot](dwUserIndex, pVibration);
            t_inXInputSet--;
            return r;
        }

        DWORD activeSlot = (DWORD)InterlockedCompareExchange(&g_activeXInputSlot, 0, 0);
        if (dwUserIndex == activeSlot || dwUserIndex == 0 ||
            pVibration->wLeftMotorSpeed > 0 || pVibration->wRightMotorSpeed > 0) {
            if (g_xinputGetSlotCount > 0 && g_origXInputGet[0]) {
                XINPUT_STATE st;
                ZeroMemory(&st, sizeof(st));
                if (g_origXInputGet[0](dwUserIndex, &st) == ERROR_SUCCESS) {
                    g_leftTriggerPull  = st.Gamepad.bLeftTrigger / 255.0f;
                    g_rightTriggerPull = st.Gamepad.bRightTrigger / 255.0f;
                    InterlockedExchange64(&g_lastTriggerPullTick, (LONGLONG)now);
                }
            }

            float lowNorm  = pVibration->wLeftMotorSpeed / 65535.0f;
            float highNorm = pVibration->wRightMotorSpeed / 65535.0f;
            SubmitLegacy2ChannelHaptics("XInput", lowNorm, highNorm, NULL, NULL);
        }
    }
    DWORD res = g_origXInputSet[slot](dwUserIndex, pVibration);
    t_inXInputSet--;
    return res;
}

#define DEFINE_XINPUT_SLOT(N) \
static DWORD WINAPI Detour_XInputSet_##N(DWORD idx, XINPUT_VIBRATION *vib) { return HandleXInputSetState(N, idx, vib); } \
static DWORD WINAPI Detour_XInputGet_##N(DWORD idx, XINPUT_STATE *st)      { return HandleXInputGetState(N, idx, st); }

DEFINE_XINPUT_SLOT(0)
DEFINE_XINPUT_SLOT(1)
DEFINE_XINPUT_SLOT(2)
DEFINE_XINPUT_SLOT(3)
DEFINE_XINPUT_SLOT(4)
DEFINE_XINPUT_SLOT(5)
DEFINE_XINPUT_SLOT(6)
DEFINE_XINPUT_SLOT(7)
DEFINE_XINPUT_SLOT(8)
DEFINE_XINPUT_SLOT(9)
DEFINE_XINPUT_SLOT(10)
DEFINE_XINPUT_SLOT(11)

static void *g_xinputSetDetours[MAX_XINPUT_SLOTS] = {
    (void*)Detour_XInputSet_0, (void*)Detour_XInputSet_1, (void*)Detour_XInputSet_2,  (void*)Detour_XInputSet_3,
    (void*)Detour_XInputSet_4, (void*)Detour_XInputSet_5, (void*)Detour_XInputSet_6,  (void*)Detour_XInputSet_7,
    (void*)Detour_XInputSet_8, (void*)Detour_XInputSet_9, (void*)Detour_XInputSet_10, (void*)Detour_XInputSet_11
};

static void *g_xinputGetDetours[MAX_XINPUT_SLOTS] = {
    (void*)Detour_XInputGet_0, (void*)Detour_XInputGet_1, (void*)Detour_XInputGet_2,  (void*)Detour_XInputGet_3,
    (void*)Detour_XInputGet_4, (void*)Detour_XInputGet_5, (void*)Detour_XInputGet_6,  (void*)Detour_XInputGet_7,
    (void*)Detour_XInputGet_8, (void*)Detour_XInputGet_9, (void*)Detour_XInputGet_10, (void*)Detour_XInputGet_11
};

static void TryHookXInputTarget(void *target, BOOL isSet, const char *desc) {
    if (!IsExecutableAddress(target)) return;
    EnterCriticalSection(&g_csHooks);
    for (int i = 0; i < g_hookedCount; ++i) {
        if (g_hookedTargets[i] == target) {
            LeaveCriticalSection(&g_csHooks);
            return;
        }
    }

    if (isSet && g_xinputSetSlotCount < MAX_XINPUT_SLOTS) {
        int slot = g_xinputSetSlotCount;
        void *orig = NULL;
        if (MH_CreateHook(target, g_xinputSetDetours[slot], &orig) == MH_OK) {
            g_origXInputSet[slot] = (PFN_XInputSetState)orig;
            g_xinputSetSlotCount++;
            if (g_hookedCount < MAX_HOOKED_TARGETS) g_hookedTargets[g_hookedCount++] = target;
            MH_EnableHook(target);
            Vader_Log("[Hook] %s[%d] hooked at %p", desc, slot, target);
        }
    } else if (!isSet && g_xinputGetSlotCount < MAX_XINPUT_SLOTS) {
        int slot = g_xinputGetSlotCount;
        void *orig = NULL;
        if (MH_CreateHook(target, g_xinputGetDetours[slot], &orig) == MH_OK) {
            g_origXInputGet[slot] = (PFN_XInputGetState)orig;
            g_xinputGetSlotCount++;
            if (g_hookedCount < MAX_HOOKED_TARGETS) g_hookedTargets[g_hookedCount++] = target;
            MH_EnableHook(target);
            Vader_Log("[Hook] %s[%d] hooked at %p", desc, slot, target);
        }
    }
    LeaveCriticalSection(&g_csHooks);
}

// -----------------------------------------------------------------------------
// 4. SDL2 / SDL3 Trigger & Grip Rumble Hooks
// -----------------------------------------------------------------------------
typedef int (__cdecl *PFN_SDL_Rumble)(void *dev, uint16_t leftOrLow, uint16_t rightOrHigh, uint32_t duration_ms);

static PFN_SDL_Rumble Original_SDL_JoystickRumble = NULL;
static PFN_SDL_Rumble Original_SDL_GameControllerRumble = NULL;
static PFN_SDL_Rumble Original_SDL_GamepadRumble = NULL;
static PFN_SDL_Rumble Original_SDL_JoystickRumbleTriggers = NULL;
static PFN_SDL_Rumble Original_SDL_GameControllerRumbleTriggers = NULL;
static PFN_SDL_Rumble Original_SDL_GamepadRumbleTriggers = NULL;

static float g_sdlLastLG = 0.0f;
static float g_sdlLastRG = 0.0f;
static float g_sdlLastLT = 0.0f;
static float g_sdlLastRT = 0.0f;

static int __cdecl Detour_SDL_JoystickRumble(void *dev, uint16_t low, uint16_t high, uint32_t dur) {
    g_sdlLastLG = low / 65535.0f;
    g_sdlLastRG = high / 65535.0f;
    SubmitNative4ChannelHaptics("SDL", g_sdlLastLG, g_sdlLastRG, g_sdlLastLT, g_sdlLastRT, NULL, NULL);
    return Original_SDL_JoystickRumble ? Original_SDL_JoystickRumble(dev, low, high, dur) : 0;
}

static int __cdecl Detour_SDL_GameControllerRumble(void *dev, uint16_t low, uint16_t high, uint32_t dur) {
    g_sdlLastLG = low / 65535.0f;
    g_sdlLastRG = high / 65535.0f;
    SubmitNative4ChannelHaptics("SDL", g_sdlLastLG, g_sdlLastRG, g_sdlLastLT, g_sdlLastRT, NULL, NULL);
    return Original_SDL_GameControllerRumble ? Original_SDL_GameControllerRumble(dev, low, high, dur) : 0;
}

static int __cdecl Detour_SDL_GamepadRumble(void *dev, uint16_t low, uint16_t high, uint32_t dur) {
    g_sdlLastLG = low / 65535.0f;
    g_sdlLastRG = high / 65535.0f;
    SubmitNative4ChannelHaptics("SDL3", g_sdlLastLG, g_sdlLastRG, g_sdlLastLT, g_sdlLastRT, NULL, NULL);
    return Original_SDL_GamepadRumble ? Original_SDL_GamepadRumble(dev, low, high, dur) : 1;
}

static int __cdecl Detour_SDL_JoystickRumbleTriggers(void *dev, uint16_t lt, uint16_t rt, uint32_t dur) {
    g_sdlLastLT = lt / 65535.0f;
    g_sdlLastRT = rt / 65535.0f;
    SubmitNative4ChannelHaptics("SDL-Trig", g_sdlLastLG, g_sdlLastRG, g_sdlLastLT, g_sdlLastRT, NULL, NULL);
    if (Vader_IsConnected()) return 0; // SDL2 0 = success
    return Original_SDL_JoystickRumbleTriggers ? Original_SDL_JoystickRumbleTriggers(dev, lt, rt, dur) : 0;
}

static int __cdecl Detour_SDL_GameControllerRumbleTriggers(void *dev, uint16_t lt, uint16_t rt, uint32_t dur) {
    g_sdlLastLT = lt / 65535.0f;
    g_sdlLastRT = rt / 65535.0f;
    SubmitNative4ChannelHaptics("SDL-Trig", g_sdlLastLG, g_sdlLastRG, g_sdlLastLT, g_sdlLastRT, NULL, NULL);
    if (Vader_IsConnected()) return 0; // SDL2 0 = success
    return Original_SDL_GameControllerRumbleTriggers ? Original_SDL_GameControllerRumbleTriggers(dev, lt, rt, dur) : 0;
}

static int __cdecl Detour_SDL_GamepadRumbleTriggers(void *dev, uint16_t lt, uint16_t rt, uint32_t dur) {
    g_sdlLastLT = lt / 65535.0f;
    g_sdlLastRT = rt / 65535.0f;
    SubmitNative4ChannelHaptics("SDL3-Trig", g_sdlLastLG, g_sdlLastRG, g_sdlLastLT, g_sdlLastRT, NULL, NULL);
    if (Vader_IsConnected()) return 1; // SDL3 true (1) = success
    return Original_SDL_GamepadRumbleTriggers ? Original_SDL_GamepadRumbleTriggers(dev, lt, rt, dur) : 1;
}

// -----------------------------------------------------------------------------
// 5. kernel32!DeviceIoControl Hook (Microsoft XUSB 0x8000A010 & Steam Extended)
// -----------------------------------------------------------------------------
typedef BOOL (WINAPI *PFN_DeviceIoControl)(
    HANDLE hDevice, DWORD dwIoControlCode,
    LPVOID lpInBuffer, DWORD nInBufferSize,
    LPVOID lpOutBuffer, DWORD nOutBufferSize,
    LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped);
static PFN_DeviceIoControl Original_DeviceIoControl = NULL;
static PFN_DeviceIoControl Original_KernelBaseDeviceIoControl = NULL;
static __thread int t_inDeviceIoctl = 0;

#define IOCTL_XUSB_SET_GAMEPAD_STATE 0x8000A010u
#define IOCTL_STEAM_XBOX_SET_STATE   0x002aac08u
#define IOCTL_STEAM_XBOX_GET_STATE   0x002aec04u

static BOOL HandleDeviceIoControl(
    PFN_DeviceIoControl origFn,
    HANDLE hDevice, DWORD dwIoControlCode,
    LPVOID lpInBuffer, DWORD nInBufferSize,
    LPVOID lpOutBuffer, DWORD nOutBufferSize,
    LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped)
{
    if (!origFn) return FALSE;
    if (t_inDeviceIoctl > 0) {
        return origFn(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize, lpBytesReturned, lpOverlapped);
    }

    t_inDeviceIoctl++;
    uint8_t zeroedXusbBuf[16];

    // 1. Microsoft XUSB Set Gamepad State (used by Windows.Gaming.Input, GameInput, and XInput)
    if (dwIoControlCode == IOCTL_XUSB_SET_GAMEPAD_STATE && lpInBuffer) {
        const uint8_t *buf = (const uint8_t *)lpInBuffer;
        ULONGLONG now = GetTickCount64();
        ULONGLONG lastSubmit = (ULONGLONG)InterlockedCompareExchange64(&g_lastHapticsSubmitTick, 0, 0);

        if (nInBufferSize == 9 && buf[4] == 0x02) {
            // 9-byte 4-motor XUSB packet: buf[2]=LG, buf[3]=RG, buf[7]=LT, buf[8]=RT
            if (now - lastSubmit > 12) {
                SubmitNative4ChannelHaptics("XUSB-4Ch",
                                            buf[2] / 255.0f,
                                            buf[3] / 255.0f,
                                            buf[7] / 255.0f,
                                            buf[8] / 255.0f,
                                            NULL, NULL);
            }
        } else if (nInBufferSize == 5 && buf[4] == 0x01) {
            // 5-byte 2-motor XUSB packet: buf[2]=LG, buf[3]=RG
            ULONGLONG lastNative = (ULONGLONG)InterlockedCompareExchange64(&g_lastNative4ChTick, 0, 0);
            if (now - lastSubmit > 12 &&
                !(InterlockedCompareExchange(&g_gameUsesNativeTriggers, 0, 0) != 0 && (now - lastNative) < 1500)) {
                ULONGLONG lastPull = (ULONGLONG)InterlockedCompareExchange64(&g_lastTriggerPullTick, 0, 0);
                if ((now - lastPull) > 25 && g_xinputGetSlotCount > 0 && g_origXInputGet[0]) {
                    XINPUT_STATE st;
                    ZeroMemory(&st, sizeof(st));
                    DWORD slot = (DWORD)InterlockedCompareExchange(&g_activeXInputSlot, 0, 0);
                    if (g_origXInputGet[0](slot, &st) == ERROR_SUCCESS) {
                        g_leftTriggerPull  = st.Gamepad.bLeftTrigger / 255.0f;
                        g_rightTriggerPull = st.Gamepad.bRightTrigger / 255.0f;
                        InterlockedExchange64(&g_lastTriggerPullTick, (LONGLONG)now);
                    }
                }
                SubmitLegacy2ChannelHaptics("XUSB-2Ch",
                                            buf[2] / 255.0f,
                                            buf[3] / 255.0f,
                                            NULL, NULL);
            }
        }

        // When Flydigi Vendor HID (MI_01) is active, zero out the MI_00 XUSB motor bytes
        // so MI_00 2-motor USB packets never compete with or clear MI_01 4-motor Impulse Trigger packets,
        // while preserving full kernel OVERLAPPED/IOCP completion semantics!
        if (Vader_IsConnected() && nInBufferSize <= sizeof(zeroedXusbBuf)) {
            memcpy(zeroedXusbBuf, lpInBuffer, nInBufferSize);
            if (nInBufferSize >= 4) {
                zeroedXusbBuf[2] = 0;
                zeroedXusbBuf[3] = 0;
            }
            if (nInBufferSize >= 9) {
                zeroedXusbBuf[7] = 0;
                zeroedXusbBuf[8] = 0;
            }
            lpInBuffer = zeroedXusbBuf;
        }
    }
    // 2. Steam Extended Xbox Set State
    else if (dwIoControlCode == IOCTL_STEAM_XBOX_SET_STATE && lpInBuffer && nInBufferSize >= 7) {
        const uint8_t *buf = (const uint8_t*)lpInBuffer;
        DWORD offset = nInBufferSize - 7;
        uint8_t lt = buf[offset + 0];
        uint8_t rt = buf[offset + 1];
        uint8_t lg = buf[offset + 2];
        uint8_t rg = buf[offset + 3];
        if (lt > 0 || rt > 0 || InterlockedCompareExchange(&g_gameUsesNativeTriggers, 0, 0) != 0) {
            SubmitNative4ChannelHaptics("SteamIOCTL",
                                        lg / 255.0f, rg / 255.0f, lt / 255.0f, rt / 255.0f,
                                        NULL, NULL);
        }
    }

    BOOL res = origFn(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize, lpBytesReturned, lpOverlapped);

    if (res && lpOutBuffer) {
        DWORD bytesOut = lpBytesReturned ? *lpBytesReturned : nOutBufferSize;
        if (dwIoControlCode == IOCTL_STEAM_XBOX_GET_STATE && bytesOut >= 19) {
            const uint8_t *out = (const uint8_t*)lpOutBuffer;
            g_leftTriggerPull  = out[17] / 255.0f;
            g_rightTriggerPull = out[18] / 255.0f;
        }
    }

    t_inDeviceIoctl--;
    return res;
}

static BOOL WINAPI Detour_DeviceIoControl(
    HANDLE hDevice, DWORD dwIoControlCode,
    LPVOID lpInBuffer, DWORD nInBufferSize,
    LPVOID lpOutBuffer, DWORD nOutBufferSize,
    LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped)
{
    return HandleDeviceIoControl(Original_DeviceIoControl, hDevice, dwIoControlCode,
                                 lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize,
                                 lpBytesReturned, lpOverlapped);
}

static BOOL WINAPI Detour_KernelBaseDeviceIoControl(
    HANDLE hDevice, DWORD dwIoControlCode,
    LPVOID lpInBuffer, DWORD nInBufferSize,
    LPVOID lpOutBuffer, DWORD nOutBufferSize,
    LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped)
{
    return HandleDeviceIoControl(Original_KernelBaseDeviceIoControl, hDevice, dwIoControlCode,
                                 lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize,
                                 lpBytesReturned, lpOverlapped);
}

static void HookXInputModule(HMODULE hX, const wchar_t *dllName, const char *modLabel) {
    if (!hX) return;

    void *pGetNamed = (void*)GetProcAddress(hX, "XInputGetState");
    void *pSetNamed = (void*)GetProcAddress(hX, "XInputSetState");
    if (pGetNamed) TryHookXInputTarget(pGetNamed, FALSE, modLabel);
    if (pSetNamed) TryHookXInputTarget(pSetNamed, TRUE,  modLabel);

    // Also hook ordinals 2 (XInputGetState), 3 (XInputSetState), and 100 (XInputGetStateEx)
    if (_wcsicmp(dllName, L"XInput9_1_0.dll") != 0) {
        void *pGetOrd   = (void*)GetProcAddress(hX, (LPCSTR)(uintptr_t)2);
        void *pSetOrd   = (void*)GetProcAddress(hX, (LPCSTR)(uintptr_t)3);
        void *pGetExOrd = (void*)GetProcAddress(hX, (LPCSTR)(uintptr_t)100);
        if (pGetOrd   && pGetOrd   != pGetNamed) TryHookXInputTarget(pGetOrd,   FALSE, modLabel);
        if (pSetOrd   && pSetOrd   != pSetNamed) TryHookXInputTarget(pSetOrd,   TRUE,  modLabel);
        if (pGetExOrd && pGetExOrd != pGetNamed) TryHookXInputTarget(pGetExOrd, FALSE, modLabel);
    }
}

// -----------------------------------------------------------------------------
// Module Discovery (Safe on Init & During Runtime Polling)
// -----------------------------------------------------------------------------
static void ScanAndHookModules(BOOL allowLoad) {
    // 1. combase.dll -> RoGetActivationFactory
    HMODULE hCombase = GetModuleHandleW(L"combase.dll");
    if (!hCombase && allowLoad) hCombase = LoadLibraryW(L"combase.dll");
    if (hCombase) {
        if (!pfn_WindowsGetStringRawBuffer) {
            pfn_WindowsGetStringRawBuffer = (PFN_WindowsGetStringRawBuffer)GetProcAddress(hCombase, "WindowsGetStringRawBuffer");
        }
        if (!pfn_WindowsCreateStringReference) {
            pfn_WindowsCreateStringReference = (PFN_WindowsCreateStringReference)GetProcAddress(hCombase, "WindowsCreateStringReference");
        }
        void *pRoGet = (void*)GetProcAddress(hCombase, "RoGetActivationFactory");
        TryRegisterHook(pRoGet, (void*)&Detour_RoGetActivationFactory, (void**)&Original_RoGetActivationFactory, "combase!RoGetActivationFactory");
    }

    if (allowLoad) {
        TryProactiveWgiHook();
    }

    // 2. GameInput.dll (v0) & GameInputRedist.dll (v0/v1/v2/v3)
    HMODULE hGameInputV0 = GetModuleHandleW(L"gameinput.dll");
    if (hGameInputV0) {
        void *pGICreate = (void*)GetProcAddress(hGameInputV0, "GameInputCreate");
        TryRegisterHook(pGICreate, (void*)&Detour_GameInputCreate_V0, (void**)&Original_GameInputCreate_V0, "gameinput!GameInputCreate");
    }

    HMODULE hGameInputRedist = GetModuleHandleW(L"GameInputRedist.dll");
    if (hGameInputRedist) {
        void *pGICreateR = (void*)GetProcAddress(hGameInputRedist, "GameInputCreate");
        void *pGIInitR   = (void*)GetProcAddress(hGameInputRedist, "GameInputInitialize");
        TryRegisterHook(pGICreateR, (void*)&Detour_GameInputCreate_Redist,     (void**)&Original_GameInputCreate_Redist,     "GameInputRedist!GameInputCreate");
        TryRegisterHook(pGIInitR,   (void*)&Detour_GameInputInitialize_Redist, (void**)&Original_GameInputInitialize_Redist, "GameInputRedist!GameInputInitialize");
    }

    // 3. SDL2.dll & SDL3.dll
    HMODULE hSdl2 = GetModuleHandleW(L"SDL2.dll");
    if (hSdl2) {
        TryRegisterHook((void*)GetProcAddress(hSdl2, "SDL_JoystickRumble"),               (void*)&Detour_SDL_JoystickRumble,               (void**)&Original_SDL_JoystickRumble,               "SDL2!SDL_JoystickRumble");
        TryRegisterHook((void*)GetProcAddress(hSdl2, "SDL_GameControllerRumble"),         (void*)&Detour_SDL_GameControllerRumble,         (void**)&Original_SDL_GameControllerRumble,         "SDL2!SDL_GameControllerRumble");
        TryRegisterHook((void*)GetProcAddress(hSdl2, "SDL_JoystickRumbleTriggers"),       (void*)&Detour_SDL_JoystickRumbleTriggers,       (void**)&Original_SDL_JoystickRumbleTriggers,       "SDL2!SDL_JoystickRumbleTriggers");
        TryRegisterHook((void*)GetProcAddress(hSdl2, "SDL_GameControllerRumbleTriggers"), (void*)&Detour_SDL_GameControllerRumbleTriggers, (void**)&Original_SDL_GameControllerRumbleTriggers, "SDL2!SDL_GameControllerRumbleTriggers");
    }
    HMODULE hSdl3 = GetModuleHandleW(L"SDL3.dll");
    if (hSdl3) {
        TryRegisterHook((void*)GetProcAddress(hSdl3, "SDL_JoystickRumble"),         (void*)&Detour_SDL_JoystickRumble,         (void**)&Original_SDL_JoystickRumble,         "SDL3!SDL_JoystickRumble");
        TryRegisterHook((void*)GetProcAddress(hSdl3, "SDL_GamepadRumble"),          (void*)&Detour_SDL_GamepadRumble,          (void**)&Original_SDL_GamepadRumble,          "SDL3!SDL_GamepadRumble");
        TryRegisterHook((void*)GetProcAddress(hSdl3, "SDL_JoystickRumbleTriggers"), (void*)&Detour_SDL_JoystickRumbleTriggers, (void**)&Original_SDL_JoystickRumbleTriggers, "SDL3!SDL_JoystickRumbleTriggers");
        TryRegisterHook((void*)GetProcAddress(hSdl3, "SDL_GamepadRumbleTriggers"),  (void*)&Detour_SDL_GamepadRumbleTriggers,  (void**)&Original_SDL_GamepadRumbleTriggers,  "SDL3!SDL_GamepadRumbleTriggers");
    }

    // 4. XInput DLLs (check real System32 copy first to bypass ASI loader stubs, then local fallback)
    const wchar_t *xinputDlls[] = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"XInput9_1_0.dll",
        L"xinput1_2.dll", L"xinput1_1.dll", L"XInputUap.dll", NULL
    };
    wchar_t sysDir[MAX_PATH] = {0};
    UINT sysLen = GetSystemDirectoryW(sysDir, MAX_PATH);

    for (int i = 0; xinputDlls[i]; ++i) {
        HMODULE hSys = NULL;
        if (sysLen > 0 && sysLen < MAX_PATH - 32) {
            wchar_t sysFullPath[MAX_PATH];
            _snwprintf(sysFullPath, MAX_PATH, L"%ls\\%ls", sysDir, xinputDlls[i]);
            sysFullPath[MAX_PATH - 1] = L'\0';
            hSys = GetModuleHandleW(sysFullPath);
            if (hSys) {
                char sysLabel[96];
                snprintf(sysLabel, sizeof(sysLabel), "System32\\%ls", xinputDlls[i]);
                HookXInputModule(hSys, xinputDlls[i], sysLabel);
            }
        }

        HMODULE hLocal = GetModuleHandleW(xinputDlls[i]);
        if (hLocal && hLocal != hSys && allowLoad) {
            char modName[64];
            snprintf(modName, sizeof(modName), "Local\\%ls", xinputDlls[i]);
            HookXInputModule(hLocal, xinputDlls[i], modName);
        }
    }

    // 5. kernel32!DeviceIoControl & kernelbase!DeviceIoControl (captures Microsoft XUSB 0x8000A010 & Steam Extended Xbox I/O)
    void *pK32Ioctl = NULL;
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) {
        pK32Ioctl = (void*)GetProcAddress(hK32, "DeviceIoControl");
        TryRegisterHook(pK32Ioctl, (void*)&Detour_DeviceIoControl, (void**)&Original_DeviceIoControl, "kernel32!DeviceIoControl");
    }
    HMODULE hKb = GetModuleHandleW(L"kernelbase.dll");
    if (hKb) {
        void *pKbIoctl = (void*)GetProcAddress(hKb, "DeviceIoControl");
        if (pKbIoctl && pKbIoctl != pK32Ioctl) {
            TryRegisterHook(pKbIoctl, (void*)&Detour_KernelBaseDeviceIoControl, (void**)&Original_KernelBaseDeviceIoControl, "kernelbase!DeviceIoControl");
        }
    }
}

void Hooks_Install(void) {
    if (InterlockedCompareExchange(&g_hooksInitState, 1, 0) != 0) return;
    InitializeCriticalSection(&g_csHooks);

    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    Vader_Log("[Init] PID=%lu Exe=%s", (unsigned long)GetCurrentProcessId(), exePath);

    LoadConfig();

    if (MH_Initialize() != MH_OK) {
        Vader_Log("[-] MinHook initialization failed!");
        return;
    }
    Vader_Log("[+] MinHook initialized successfully.");

    ScanAndHookModules(FALSE);
}

void Hooks_PollLateModules(void) {
    if (InterlockedCompareExchange(&g_hooksInitState, 1, 1) != 1) return;
    ScanAndHookModules(TRUE);
}

static BOOL MemContainsBytes(const uint8_t *haystack, size_t hayLen, const void *needle, size_t needleLen) {
    if (!haystack || !needle || needleLen == 0 || hayLen < needleLen) return FALSE;
    const uint8_t *ndl = (const uint8_t *)needle;
    size_t limit = hayLen - needleLen;
    for (size_t i = 0; i <= limit; ++i) {
        if (haystack[i] == ndl[0] && memcmp(haystack + i, ndl, needleLen) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL BufferHas4MotorSignature(const uint8_t *buf, size_t len) {
    static const wchar_t kWgiWide[] = L"Windows.Gaming.Input";
    static const char kWgiAscii[]   = "Windows.Gaming.Input";
    static const char kGiCreate[]   = "GameInputCreate";
    static const char kGiInit[]     = "GameInputInitialize";
    static const char kGiRedist[]   = "GameInputRedist";
    static const char kSdlJoyTrig[] = "SDL_JoystickRumbleTriggers";
    static const char kSdlPadTrig[] = "SDL_GamepadRumbleTriggers";

    if (MemContainsBytes(buf, len, kWgiWide,    sizeof(kWgiWide) - sizeof(wchar_t))) return TRUE;
    if (MemContainsBytes(buf, len, kWgiAscii,   sizeof(kWgiAscii) - 1))              return TRUE;
    if (MemContainsBytes(buf, len, kGiCreate,   sizeof(kGiCreate) - 1))              return TRUE;
    if (MemContainsBytes(buf, len, kGiInit,     sizeof(kGiInit) - 1))                return TRUE;
    if (MemContainsBytes(buf, len, kGiRedist,   sizeof(kGiRedist) - 1))              return TRUE;
    if (MemContainsBytes(buf, len, kSdlJoyTrig, sizeof(kSdlJoyTrig) - 1))            return TRUE;
    if (MemContainsBytes(buf, len, kSdlPadTrig, sizeof(kSdlPadTrig) - 1))            return TRUE;
    return FALSE;
}

// Safe .rdata-only scan using ReadProcessMemory so Arxan/DRM guarded pages (e.g. GTA5_Enhanced.exe)
// never raise Vectored Exception Handler traps or stall the worker thread.
static BOOL ExeImageHas4MotorApis(void) {
    const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
    if (!base) return FALSE;

    IMAGE_DOS_HEADER dos;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), base, &dos, sizeof(dos), &bytesRead) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return FALSE;
    }

    IMAGE_NT_HEADERS nt;
    if (!ReadProcessMemory(GetCurrentProcess(), base + dos.e_lfanew, &nt, sizeof(nt), &bytesRead) ||
        nt.Signature != IMAGE_NT_SIGNATURE) {
        return FALSE;
    }

    const IMAGE_SECTION_HEADER *secBase = (const IMAGE_SECTION_HEADER *)(
        base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader);

    uint8_t *chunk = (uint8_t *)malloc(65536 + 128);
    if (!chunk) return FALSE;

    BOOL found = FALSE;
    WORD numSecs = nt.FileHeader.NumberOfSections;
    if (numSecs > 64) numSecs = 64;

    for (WORD i = 0; i < numSecs && !found; ++i) {
        IMAGE_SECTION_HEADER sec;
        if (!ReadProcessMemory(GetCurrentProcess(), secBase + i, &sec, sizeof(sec), &bytesRead)) {
            continue;
        }
        // Only scan read-only data (.rdata) where import/WinRT/API string literals live
        if (memcmp(sec.Name, ".rdata", 6) != 0) {
            continue;
        }

        size_t secSize = sec.Misc.VirtualSize ? (size_t)sec.Misc.VirtualSize : (size_t)sec.SizeOfRawData;
        if (secSize > 32 * 1024 * 1024) secSize = 32 * 1024 * 1024;

        const uint8_t *ptr = base + sec.VirtualAddress;
        const uint8_t *secEnd = ptr + secSize;

        while (ptr < secEnd) {
            size_t toRead = (size_t)(secEnd - ptr);
            if (toRead > 65536) toRead = 65536;
            SIZE_T got = 0;
            if (ReadProcessMemory(GetCurrentProcess(), ptr, chunk, toRead, &got) && got >= 32) {
                if (BufferHas4MotorSignature(chunk, (size_t)got)) {
                    found = TRUE;
                    break;
                }
            }
            if (toRead <= 128) break;
            ptr += (toRead - 128); // 128-byte overlap so strings crossing chunk boundaries are matched
        }
    }

    free(chunk);
    return found;
}

static BOOL GameDirHas4MotorFiles(void) {
    char dirPath[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, dirPath, MAX_PATH);
    char *slash = strrchr(dirPath, '\\');
    if (slash) *(slash + 1) = '\0';

    const char *files[] = {
        "GameInputRedist.dll",
        "gameinput.dll",
        "MicrosoftGame.Config",
        "xgameruntime.dll",
        NULL
    };

    for (int i = 0; files[i]; ++i) {
        char fullPath[MAX_PATH];
        snprintf(fullPath, MAX_PATH, "%s%s", dirPath, files[i]);
        if (GetFileAttributesA(fullPath) != INVALID_FILE_ATTRIBUTES) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL IsKnown4MotorGameRuntime(void) {
    if (InterlockedCompareExchange(&g_wgiStaticsHooked, 0, 0) != 0 ||
        InterlockedCompareExchange(&g_gameUsesNativeTriggers, 0, 0) != 0 ||
        GetModuleHandleW(L"Windows.Gaming.Input.dll") != NULL ||
        GetModuleHandleW(L"gameinput.dll") != NULL ||
        GetModuleHandleW(L"GameInputRedist.dll") != NULL ||
        GameDirHas4MotorFiles()) {
        return TRUE;
    }

    LONG cached = InterlockedCompareExchange(&g_cachedExe4Motor, -1, -1);
    if (cached < 0) {
        cached = ExeImageHas4MotorApis() ? 1 : 0;
        InterlockedCompareExchange(&g_cachedExe4Motor, cached, -1);
    }
    return cached != 0;
}

int Hooks_DetectGameMotorCount(void) {
    if (IsKnown4MotorGameRuntime()) {
        Vader_Log("[Detect] Game supports 4-motor vibration (Impulse Triggers + Grips).");
        return 4;
    }

    Vader_Log("[Detect] Game uses 2-motor vibration (Grip motors only).");
    return 2;
}

void Hooks_Uninstall(void) {
    if (InterlockedCompareExchange(&g_hooksInitState, 0, 1) != 1) return;
    Vader_Log("[*] Disabling all hooks...");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    Vader_Log("[+] All hooks disabled cleanly.");
}
