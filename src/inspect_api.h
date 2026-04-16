#pragma once
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <string>
#include "nlohmann/json.hpp"

// ---------------------------------------------------------------------------
// Result of a CSGOFloat API inspect-link query
// ---------------------------------------------------------------------------
struct InspectResult {
    bool        ok         = false;
    std::string error;
    int         defIndex   = 0;
    int         paintIndex = 0;
    float       floatValue = 0.0f;
    std::string fullName;   // e.g. "AK-47 | Asiimov"
    std::string wearName;   // e.g. "Field-Tested"
};

// ---------------------------------------------------------------------------
// Percent-encode a string for use as a URL query-parameter value.
// Encodes everything except unreserved characters (RFC 3986).
// ---------------------------------------------------------------------------
inline std::string UrlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Query the CSGOFloat API with a Steam inspect link.
// Blocking — call from a background thread.
//
// Accepts both forms:
//   steam://rungame/730/.../+csgo_econ_action_preview S...A...D...
//   steam://rungame/730/.../+csgo_econ_action_preview M...A...D...
// ---------------------------------------------------------------------------
inline InspectResult QueryInspectUrl(const std::string& inspectUrl) {
    InspectResult res;

    if (inspectUrl.find("csgo_econ_action_preview") == std::string::npos) {
        res.error = "Not a valid CS2 inspect link (missing csgo_econ_action_preview)";
        return res;
    }

    // Build wide query path:  /?url=<percent-encoded inspect link>
    std::string encoded = UrlEncode(inspectUrl);
    std::wstring path   = L"/?url=";
    path += std::wstring(encoded.begin(), encoded.end());

    HINTERNET hSess = WinHttpOpen(
        L"SkinsAreSilly/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) { res.error = "WinHttpOpen failed"; return res; }

    HINTERNET hConn = WinHttpConnect(
        hSess, L"api.csgofloat.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConn) {
        WinHttpCloseHandle(hSess);
        res.error = "WinHttpConnect failed"; return res;
    }

    HINTERNET hReq = WinHttpOpenRequest(
        hConn, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hReq) {
        WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        res.error = "WinHttpOpenRequest failed"; return res;
    }

    bool ok = WinHttpSendRequest(hReq,
                  WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
           && WinHttpReceiveResponse(hReq, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        res.error = "HTTP request failed (check your internet connection)"; return res;
    }

    // Read response body
    std::string body;
    DWORD bytesRead = 0;
    char  buf[4096];
    while (WinHttpReadData(hReq, buf, sizeof(buf) - 1, &bytesRead) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        body += buf;
    }
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);

    // Parse JSON
    try {
        auto j = nlohmann::json::parse(body);

        if (j.contains("code") || (j.contains("error") && !j["error"].is_null())) {
            // API returned an error object
            if (j.contains("message") && j["message"].is_string())
                res.error = j["message"].get<std::string>();
            else if (j.contains("error") && j["error"].is_string())
                res.error = j["error"].get<std::string>();
            else
                res.error = "API error (no details)";
            return res;
        }

        auto& info     = j.at("iteminfo");
        res.defIndex   = info.value("defindex",        0);
        res.paintIndex = info.value("paintindex",       0);
        res.floatValue = info.value("floatvalue",       0.0f);
        res.fullName   = info.value("full_item_name",  "");
        res.wearName   = info.value("wear_name",       "");
        res.ok         = true;
    } catch (const std::exception& e) {
        res.error = std::string("JSON parse error: ") + e.what();
    }
    return res;
}
