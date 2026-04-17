#include "sas_log.h"
#include <cstdio>

namespace SasLog {

static HANDLE           g_hFile  = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs     = {};
static bool             g_inited = false;

void Init(HMODULE hDll)
{
    InitializeCriticalSection(&g_cs);

    wchar_t dllPath[MAX_PATH]{};
    GetModuleFileNameW(hDll, dllPath, MAX_PATH);

    wchar_t* slash = wcsrchr(dllPath, L'\\');
    if (slash)
        wcscpy_s(slash + 1, MAX_PATH - (slash - dllPath + 1), L"SovereignHook.log");

    g_hFile = CreateFileW(dllPath,
        GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    g_inited = (g_hFile != INVALID_HANDLE_VALUE);
}

void Write(const char* fmt, ...)
{
    if (!g_inited) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);

    char buf[1024];
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, fmt, ap);
    va_end(ap);

    size_t len = strlen(buf);
    if (len < sizeof(buf) - 2) { buf[len] = '\n'; buf[len + 1] = '\0'; ++len; }

    EnterCriticalSection(&g_cs);
    DWORD written;
    WriteFile(g_hFile, buf, (DWORD)len, &written, nullptr);
    FlushFileBuffers(g_hFile);
    LeaveCriticalSection(&g_cs);
}

void Close()
{
    if (g_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hFile);
        g_hFile = INVALID_HANDLE_VALUE;
    }
    if (g_inited) {
        DeleteCriticalSection(&g_cs);
        g_inited = false;
    }
}

} // namespace SasLog
