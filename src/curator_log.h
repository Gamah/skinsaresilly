#pragma once
// Minimal timestamped logger for MuseumCurator.exe.
// Writes to <exe_dir>\MuseumCurator.log.
// Not thread-safe by design — all calls come from the main/UI thread.

#include <windows.h>
#include <cstdio>
#include <string>

namespace CuratorLog {

static FILE* g_fp = nullptr;

inline void Init(const std::wstring& exeDir)
{
    std::wstring path = exeDir + L"MuseumCurator.log";
    _wfopen_s(&g_fp, path.c_str(), L"w");
}

inline void Write(const char* fmt, ...)
{
    if (!g_fp) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);

    fprintf(g_fp, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_fp, fmt, ap);
    va_end(ap);

    fputc('\n', g_fp);
    fflush(g_fp);
}

inline void Close()
{
    if (g_fp) { fclose(g_fp); g_fp = nullptr; }
}

} // namespace CuratorLog
