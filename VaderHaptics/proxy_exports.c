#include <windows.h>
#include <stdio.h>
#include "vader_hid.h"

// -----------------------------------------------------------------------------
// DINPUT8 PROXY (System32\dinput8.dll - all 6 exports)
// -----------------------------------------------------------------------------
typedef HRESULT (WINAPI *PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_DllCanUnloadNow)(void);
typedef HRESULT (WINAPI *PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT (WINAPI *PFN_DllRegisterServer)(void);
typedef HRESULT (WINAPI *PFN_DllUnregisterServer)(void);
typedef const void* (WINAPI *PFN_GetdfDIJoystick)(void);

static volatile HMODULE g_hRealDinput8 = NULL;
static PFN_DirectInput8Create  Real_DirectInput8Create  = NULL;
static PFN_DllCanUnloadNow     Real_DllCanUnloadNow     = NULL;
static PFN_DllGetClassObject   Real_DllGetClassObject   = NULL;
static PFN_DllRegisterServer   Real_DllRegisterServer   = NULL;
static PFN_DllUnregisterServer Real_DllUnregisterServer = NULL;
static PFN_GetdfDIJoystick     Real_GetdfDIJoystick     = NULL;

static void EnsureDinput8Loaded(void) {
    if (g_hRealDinput8) return;
    wchar_t sysPath[MAX_PATH];
    UINT len = GetSystemDirectoryW(sysPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 14) return;
    wcscat(sysPath, L"\\dinput8.dll");
    HMODULE hReal = LoadLibraryW(sysPath);
    if (hReal) {
        Real_DirectInput8Create  = (PFN_DirectInput8Create)GetProcAddress(hReal, "DirectInput8Create");
        Real_DllCanUnloadNow     = (PFN_DllCanUnloadNow)GetProcAddress(hReal, "DllCanUnloadNow");
        Real_DllGetClassObject   = (PFN_DllGetClassObject)GetProcAddress(hReal, "DllGetClassObject");
        Real_DllRegisterServer   = (PFN_DllRegisterServer)GetProcAddress(hReal, "DllRegisterServer");
        Real_DllUnregisterServer = (PFN_DllUnregisterServer)GetProcAddress(hReal, "DllUnregisterServer");
        Real_GetdfDIJoystick     = (PFN_GetdfDIJoystick)GetProcAddress(hReal, "GetdfDIJoystick");
        if (InterlockedCompareExchangePointer((PVOID volatile*)&g_hRealDinput8, hReal, NULL) == NULL) {
            Vader_Log("[Proxy] Loaded real dinput8.dll from System32");
        }
    }
}

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID *ppvOut, LPUNKNOWN punkOuter) {
    EnsureDinput8Loaded();
    if (Real_DirectInput8Create) {
        return Real_DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    }
    return E_FAIL;
}

HRESULT WINAPI DllCanUnloadNow(void) {
    // Always return S_FALSE so CoFreeUnusedLibraries never unloads our proxy DLL mid-game
    return S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv) {
    EnsureDinput8Loaded();
    if (Real_DllGetClassObject) return Real_DllGetClassObject(rclsid, riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void) {
    EnsureDinput8Loaded();
    if (Real_DllRegisterServer) return Real_DllRegisterServer();
    return E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void) {
    EnsureDinput8Loaded();
    if (Real_DllUnregisterServer) return Real_DllUnregisterServer();
    return E_FAIL;
}

__declspec(dllexport) const void* WINAPI GetdfDIJoystick(void) {
    EnsureDinput8Loaded();
    if (Real_GetdfDIJoystick) return Real_GetdfDIJoystick();
    return NULL;
}

// -----------------------------------------------------------------------------
// DXGI PROXY (System32\dxgi.dll - all 20 exports)
// -----------------------------------------------------------------------------
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
typedef HRESULT (WINAPI *PFN_DXGIGetDebugInterface1)(UINT, REFIID, void**);
typedef HRESULT (WINAPI *PFN_DXGIDeclareAdapterRemovalSupport)(void);
typedef HRESULT (WINAPI *PFN_DXGIDisableVBlankVirtualization)(void);

static volatile HMODULE g_hRealDxgi = NULL;
static PFN_CreateDXGIFactory                Real_CreateDXGIFactory = NULL;
static PFN_CreateDXGIFactory1               Real_CreateDXGIFactory1 = NULL;
static PFN_CreateDXGIFactory2               Real_CreateDXGIFactory2 = NULL;
static PFN_DXGIGetDebugInterface1           Real_DXGIGetDebugInterface1 = NULL;
static PFN_DXGIDeclareAdapterRemovalSupport Real_DXGIDeclareAdapterRemovalSupport = NULL;
static PFN_DXGIDisableVBlankVirtualization  Real_DXGIDisableVBlankVirtualization = NULL;

void *Real_ApplyCompatResolutionQuirking = NULL;
void *Real_CompatString = NULL;
void *Real_CompatValue = NULL;
void *Real_DXGID3D10CreateDevice = NULL;
void *Real_DXGID3D10CreateLayeredDevice = NULL;
void *Real_DXGID3D10GetLayeredDeviceSize = NULL;
void *Real_DXGID3D10RegisterLayers = NULL;
void *Real_DXGIDumpJournal = NULL;
void *Real_DXGIReportAdapterConfiguration = NULL;
void *Real_PIXBeginCapture = NULL;
void *Real_PIXEndCapture = NULL;
void *Real_PIXGetCaptureState = NULL;
void *Real_SetAppCompatStringPointer = NULL;
void *Real_UpdateHMDEmulationStatus = NULL;

void EnsureDxgiLoaded(void) {
    if (g_hRealDxgi) return;
    wchar_t sysPath[MAX_PATH];
    UINT len = GetSystemDirectoryW(sysPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 12) return;
    wcscat(sysPath, L"\\dxgi.dll");
    HMODULE hReal = LoadLibraryW(sysPath);
    if (hReal) {
        Real_CreateDXGIFactory                = (PFN_CreateDXGIFactory)GetProcAddress(hReal, "CreateDXGIFactory");
        Real_CreateDXGIFactory1               = (PFN_CreateDXGIFactory1)GetProcAddress(hReal, "CreateDXGIFactory1");
        Real_CreateDXGIFactory2               = (PFN_CreateDXGIFactory2)GetProcAddress(hReal, "CreateDXGIFactory2");
        Real_DXGIGetDebugInterface1           = (PFN_DXGIGetDebugInterface1)GetProcAddress(hReal, "DXGIGetDebugInterface1");
        Real_DXGIDeclareAdapterRemovalSupport = (PFN_DXGIDeclareAdapterRemovalSupport)GetProcAddress(hReal, "DXGIDeclareAdapterRemovalSupport");
        Real_DXGIDisableVBlankVirtualization  = (PFN_DXGIDisableVBlankVirtualization)GetProcAddress(hReal, "DXGIDisableVBlankVirtualization");

        Real_ApplyCompatResolutionQuirking  = (void*)GetProcAddress(hReal, "ApplyCompatResolutionQuirking");
        Real_CompatString                   = (void*)GetProcAddress(hReal, "CompatString");
        Real_CompatValue                    = (void*)GetProcAddress(hReal, "CompatValue");
        Real_DXGID3D10CreateDevice          = (void*)GetProcAddress(hReal, "DXGID3D10CreateDevice");
        Real_DXGID3D10CreateLayeredDevice   = (void*)GetProcAddress(hReal, "DXGID3D10CreateLayeredDevice");
        Real_DXGID3D10GetLayeredDeviceSize  = (void*)GetProcAddress(hReal, "DXGID3D10GetLayeredDeviceSize");
        Real_DXGID3D10RegisterLayers        = (void*)GetProcAddress(hReal, "DXGID3D10RegisterLayers");
        Real_DXGIDumpJournal                = (void*)GetProcAddress(hReal, "DXGIDumpJournal");
        Real_DXGIReportAdapterConfiguration = (void*)GetProcAddress(hReal, "DXGIReportAdapterConfiguration");
        Real_PIXBeginCapture                = (void*)GetProcAddress(hReal, "PIXBeginCapture");
        Real_PIXEndCapture                  = (void*)GetProcAddress(hReal, "PIXEndCapture");
        Real_PIXGetCaptureState             = (void*)GetProcAddress(hReal, "PIXGetCaptureState");
        Real_SetAppCompatStringPointer      = (void*)GetProcAddress(hReal, "SetAppCompatStringPointer");
        Real_UpdateHMDEmulationStatus       = (void*)GetProcAddress(hReal, "UpdateHMDEmulationStatus");

        if (InterlockedCompareExchangePointer((PVOID volatile*)&g_hRealDxgi, hReal, NULL) == NULL) {
            Vader_Log("[Proxy] Loaded real dxgi.dll from System32");
        }
    }
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory) {
    EnsureDxgiLoaded();
    if (Real_CreateDXGIFactory) return Real_CreateDXGIFactory(riid, ppFactory);
    return E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory) {
    EnsureDxgiLoaded();
    if (Real_CreateDXGIFactory1) return Real_CreateDXGIFactory1(riid, ppFactory);
    return E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory) {
    EnsureDxgiLoaded();
    if (Real_CreateDXGIFactory2) return Real_CreateDXGIFactory2(Flags, riid, ppFactory);
    return E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **pDebug) {
    EnsureDxgiLoaded();
    if (Real_DXGIGetDebugInterface1) return Real_DXGIGetDebugInterface1(Flags, riid, pDebug);
    return E_NOINTERFACE;
}

__declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void) {
    EnsureDxgiLoaded();
    if (Real_DXGIDeclareAdapterRemovalSupport) return Real_DXGIDeclareAdapterRemovalSupport();
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI DXGIDisableVBlankVirtualization(void) {
    EnsureDxgiLoaded();
    if (Real_DXGIDisableVBlankVirtualization) return Real_DXGIDisableVBlankVirtualization();
    return S_OK;
}

// Exact ABI-preserving tail-jump forwarders for internal DXGI exports
#define DEFINE_DXGI_TAILJUMP(fnName) \
    __asm__( \
        ".text\n\t" \
        ".globl " #fnName "\n\t" \
        ".def " #fnName "; .scl 2; .type 32; .endef\n\t" \
        #fnName ":\n\t" \
        "movq Real_" #fnName "(%rip), %rax\n\t" \
        "testq %rax, %rax\n\t" \
        "jnz 1f\n\t" \
        "pushq %rcx\n\t" \
        "pushq %rdx\n\t" \
        "pushq %r8\n\t" \
        "pushq %r9\n\t" \
        "subq $40, %rsp\n\t" \
        "call EnsureDxgiLoaded\n\t" \
        "addq $40, %rsp\n\t" \
        "popq %r9\n\t" \
        "popq %r8\n\t" \
        "popq %rdx\n\t" \
        "popq %rcx\n\t" \
        "movq Real_" #fnName "(%rip), %rax\n\t" \
        "testq %rax, %rax\n\t" \
        "jz 2f\n\t" \
        "1:\n\t" \
        "jmp *%rax\n\t" \
        "2:\n\t" \
        "movl $0x80004005, %eax\n\t" \
        "ret\n\t" \
    );

DEFINE_DXGI_TAILJUMP(ApplyCompatResolutionQuirking)
DEFINE_DXGI_TAILJUMP(CompatString)
DEFINE_DXGI_TAILJUMP(CompatValue)
DEFINE_DXGI_TAILJUMP(DXGID3D10CreateDevice)
DEFINE_DXGI_TAILJUMP(DXGID3D10CreateLayeredDevice)
DEFINE_DXGI_TAILJUMP(DXGID3D10GetLayeredDeviceSize)
DEFINE_DXGI_TAILJUMP(DXGID3D10RegisterLayers)
DEFINE_DXGI_TAILJUMP(DXGIDumpJournal)
DEFINE_DXGI_TAILJUMP(DXGIReportAdapterConfiguration)
DEFINE_DXGI_TAILJUMP(PIXBeginCapture)
DEFINE_DXGI_TAILJUMP(PIXEndCapture)
DEFINE_DXGI_TAILJUMP(PIXGetCaptureState)
DEFINE_DXGI_TAILJUMP(SetAppCompatStringPointer)
DEFINE_DXGI_TAILJUMP(UpdateHMDEmulationStatus)
