#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <dinput.h>
#include <stdarg.h>
#include <stdio.h>

#if !defined(_M_IX86)
#error MafiaAimHost must be built for Win32/x86.
#endif

typedef void (__cdecl *AimMouseFn)(LONG *lx, LONG *ly);
typedef void (__cdecl *AimInitFn)(void);
typedef HRESULT (STDMETHODCALLTYPE *GetDeviceStateFn)(void *self, DWORD size, LPVOID data);
typedef HRESULT (WINAPI *DirectInput8CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

static HINSTANCE g_self;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static wchar_t g_logicPath[MAX_PATH];
static wchar_t g_loadedCopy[MAX_PATH];
static HMODULE g_logic;
static AimMouseFn volatile g_aim;
static volatile LONG g_inFlight;
static volatile LONG g_mouseCalls;
static GetDeviceStateFn g_stateA, g_stateW;
static int g_generation;

static void Log(const char *format, ...)
{
    char line[256];
    DWORD written;
    int length;
    va_list args;
    if (g_log == INVALID_HANDLE_VALUE)
    {
        char path[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, path);
        if (!n || n > MAX_PATH - 32)
            return;
        lstrcatA(path, "MafiaAimHost.log");
        g_log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_log == INVALID_HANDLE_VALUE)
            return;
    }
    va_start(args, format);
    length = _vsnprintf(line, sizeof(line) - 3, format, args);
    va_end(args);
    if (length < 0 || length > (int)sizeof(line) - 3)
        length = (int)sizeof(line) - 3;
    line[length++] = '\r';
    line[length++] = '\n';
    WriteFile(g_log, line, (DWORD)length, &written, NULL);
    FlushFileBuffers(g_log);
}

/* ---- hot-reloadable logic ---------------------------------------------- */

static void CallAim(LONG *lx, LONG *ly)
{
    AimMouseFn fn;
    InterlockedIncrement(&g_inFlight);
    fn = g_aim;
    if (fn)
    {
        __try
        {
            fn(lx, ly);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_aim = NULL;
            Log("logic crashed; disabled until the next reload");
        }
    }
    InterlockedDecrement(&g_inFlight);
}

static int ReadStamp(FILETIME *time, DWORD *size)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(g_logicPath, GetFileExInfoStandard, &data) || data.nFileSizeLow == 0)
        return 0;
    *time = data.ftLastWriteTime;
    *size = data.nFileSizeLow;
    return 1;
}

static int ReloadLogic(void)
{
    wchar_t temp[MAX_PATH], copy[MAX_PATH];
    HMODULE module, old;
    AimMouseFn fn;
    AimInitFn init;
    int i;

    GetTempPathW(MAX_PATH, temp);
    swprintf(copy, MAX_PATH, L"%lsMafiaAimLogic_%lu_%d.dll", temp, GetCurrentProcessId(), ++g_generation);
    if (!CopyFileW(g_logicPath, copy, FALSE))
        return 0;
    module = LoadLibraryW(copy);
    if (!module)
    {
        DeleteFileW(copy);
        return 0;
    }
    fn = (AimMouseFn)GetProcAddress(module, "AimMouse");
    init = (AimInitFn)GetProcAddress(module, "AimInit");
    if (!fn)
    {
        Log("logic has no AimMouse export");
        FreeLibrary(module);
        DeleteFileW(copy);
        return 0;
    }
    if (init)
        init();

    old = g_logic;
    g_logic = module;
    g_aim = fn;
    for (i = 0; i < 100 && g_inFlight > 0; ++i)
        Sleep(10);
    if (old)
    {
        FreeLibrary(old);
        DeleteFileW(g_loadedCopy);
    }
    lstrcpyW(g_loadedCopy, copy);
    Log("logic loaded (generation %d)", g_generation);
    return 1;
}

static DWORD WINAPI WatchThread(void *unused)
{
    FILETIME loadedTime = {0, 0}, now, again;
    DWORD loadedSize = 0, nowSize, againSize;
    (void)unused;
    for (;;)
    {
        if (ReadStamp(&now, &nowSize) &&
            (CompareFileTime(&now, &loadedTime) != 0 || nowSize != loadedSize))
        {
            Sleep(300); /* let the linker finish writing */
            if (ReadStamp(&again, &againSize) && CompareFileTime(&now, &again) == 0 &&
                nowSize == againSize && ReloadLogic())
            {
                loadedTime = now;
                loadedSize = nowSize;
            }
        }
        Sleep(500);
    }
}

/* ---- DirectInput hook --------------------------------------------------- */

static void OnDeviceState(DWORD size, LPVOID data)
{
    if (data && (size == 16u || size == 20u))
    {
        LONG *axes = (LONG *)data;
        if (InterlockedIncrement(&g_mouseCalls) == 1)
            Log("first mouse state call (size %lu)", (unsigned long)size);
        CallAim(&axes[0], &axes[1]);
    }
}

static HRESULT STDMETHODCALLTYPE StateHookA(void *self, DWORD size, LPVOID data)
{
    HRESULT result = g_stateA(self, size, data);
    if (result == DI_OK)
        OnDeviceState(size, data);
    return result;
}

static HRESULT STDMETHODCALLTYPE StateHookW(void *self, DWORD size, LPVOID data)
{
    HRESULT result = g_stateW(self, size, data);
    if (result == DI_OK)
        OnDeviceState(size, data);
    return result;
}

static void *PatchVtableSlot(void **vtable, int index, void *hook)
{
    DWORD old;
    void *previous;
    void **slot = &vtable[index];
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
        return NULL;
    previous = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return previous;
}

static int InstallHooks(void)
{
    HMODULE module = GetModuleHandleA("dinput8.dll");
    DirectInput8CreateFn create;
    IDirectInput8A *inputA = NULL;
    IDirectInput8W *inputW = NULL;
    IDirectInputDevice8A *deviceA = NULL;
    IDirectInputDevice8W *deviceW = NULL;
    void **tableA = NULL, **tableW = NULL;

    if (!module)
        return 0;
    create = (DirectInput8CreateFn)GetProcAddress(module, "DirectInput8Create");
    if (!create)
        return -1;
    if (create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&inputA, NULL) == DI_OK &&
        inputA->lpVtbl->CreateDevice(inputA, &GUID_SysMouse, &deviceA, NULL) == DI_OK)
        tableA = *(void ***)deviceA;
    if (create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8W, (void **)&inputW, NULL) == DI_OK &&
        inputW->lpVtbl->CreateDevice(inputW, &GUID_SysMouse, &deviceW, NULL) == DI_OK)
        tableW = *(void ***)deviceW;

    if (tableA)
    {
        g_stateA = (GetDeviceStateFn)PatchVtableSlot(tableA, 9, (void *)StateHookA);
        Log("GetDeviceState hooked (ANSI vtable %p)", (void *)tableA);
    }
    if (tableW && tableW != tableA)
    {
        g_stateW = (GetDeviceStateFn)PatchVtableSlot(tableW, 9, (void *)StateHookW);
        Log("GetDeviceState hooked (wide vtable %p)", (void *)tableW);
    }
    if (deviceA) deviceA->lpVtbl->Release(deviceA);
    if (deviceW) deviceW->lpVtbl->Release(deviceW);
    if (inputA) inputA->lpVtbl->Release(inputA);
    if (inputW) inputW->lpVtbl->Release(inputW);
    return (g_stateA || g_stateW) ? 1 : -1;
}

static DWORD WINAPI InstallThread(void *unused)
{
    int attempt;
    (void)unused;
    for (attempt = 0; attempt < 100; ++attempt)
    {
        int result = InstallHooks();
        if (result != 0)
        {
            if (result < 0)
                Log("DirectInput hook failed");
            return 0;
        }
        Sleep(100);
    }
    Log("dinput8.dll never appeared");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        DWORD length;
        HANDLE thread;
        DisableThreadLibraryCalls(instance);
        g_self = instance;
        length = GetModuleFileNameW(instance, g_logicPath, MAX_PATH);
        while (length && g_logicPath[length - 1] != L'\\' && g_logicPath[length - 1] != L'/')
            --length;
        if (length && length + 20 < MAX_PATH)
        {
            HANDLE guard = CreateMutexA(NULL, FALSE, "Local\\MafiaAimAssistHost");
            if (guard && GetLastError() == ERROR_ALREADY_EXISTS)
                return TRUE; /* another copy of the host already owns the hook */
            g_logicPath[length] = L'\0';
            lstrcatW(g_logicPath, L"MafiaAimLogic.dll");
            thread = CreateThread(NULL, 0, InstallThread, NULL, 0, NULL);
            if (thread)
                CloseHandle(thread);
            thread = CreateThread(NULL, 0, WatchThread, NULL, 0, NULL);
            if (thread)
                CloseHandle(thread);
        }
    }
    return TRUE;
}

#ifdef BUILD_PROXY
/* Built as dinput8.dll: the game loads this copy from its own folder and it forwards to the real one. */
#pragma comment(linker, "/EXPORT:DirectInput8Create=_ProxyDirectInput8Create@20")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow=_ProxyDllCanUnloadNow@0")
#pragma comment(linker, "/EXPORT:DllGetClassObject=_ProxyDllGetClassObject@12")

static HMODULE g_realDinput;

static FARPROC RealProc(const char *name)
{
    if (!g_realDinput)
    {
        char path[MAX_PATH];
        UINT length = GetSystemDirectoryA(path, MAX_PATH);
        if (!length || length > MAX_PATH - 16)
            return NULL;
        lstrcatA(path, "\\dinput8.dll");
        g_realDinput = LoadLibraryA(path);
    }
    return g_realDinput ? GetProcAddress(g_realDinput, name) : NULL;
}

HRESULT WINAPI ProxyDirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID *out,
                                       LPUNKNOWN outer)
{
    DirectInput8CreateFn fn = (DirectInput8CreateFn)RealProc("DirectInput8Create");
    return fn ? fn(instance, version, iid, out, outer) : E_FAIL;
}

HRESULT WINAPI ProxyDllCanUnloadNow(void)
{
    HRESULT (WINAPI *fn)(void) = (HRESULT (WINAPI *)(void))RealProc("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI ProxyDllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID *out)
{
    HRESULT (WINAPI *fn)(REFCLSID, REFIID, LPVOID *) =
        (HRESULT (WINAPI *)(REFCLSID, REFIID, LPVOID *))RealProc("DllGetClassObject");
    return fn ? fn(clsid, iid, out) : E_FAIL;
}
#endif
