#include "platform/file_util.h"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <climits>
#endif

namespace platform {
namespace {

bool is_relative(const std::string& p) {
#if defined(_WIN32)
    if (p.size() >= 2 && p[1] == ':') return false;
    if (!p.empty() && (p[0] == '\\' || p[0] == '/')) return false;
    return true;
#else
    return p.empty() || p[0] != '/';
#endif
}

#if defined(_WIN32)
std::wstring widen(const std::string& s, UINT cp) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring exe_dir_w() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return std::wstring();
    std::wstring p(buf, n);
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring() : p.substr(0, k + 1);
}

FILE* open_one(const std::wstring& w) {
    if (w.empty()) return nullptr;
    return _wfopen(w.c_str(), L"rb");
}

FILE* open_any(const std::string& path) {
    for (UINT cp : {(UINT)CP_UTF8, (UINT)CP_ACP}) {
        std::wstring w = widen(path, cp);
        if (FILE* f = open_one(w)) return f;
        if (!w.empty() && is_relative(path)) {
            if (FILE* f = open_one(exe_dir_w() + w)) return f;
        }
    }
    return nullptr;
}
#else
std::string exe_dir() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    std::string p(buf, (size_t)n);
    size_t k = p.find_last_of('/');
    return k == std::string::npos ? std::string() : p.substr(0, k + 1);
}

FILE* open_any(const std::string& path) {
    if (FILE* f = std::fopen(path.c_str(), "rb")) return f;
    if (is_relative(path)) {
        std::string d = exe_dir();
        if (!d.empty()) return std::fopen((d + path).c_str(), "rb");
    }
    return nullptr;
}
#endif

}  // namespace

bool read_whole_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = open_any(path);
    if (!f) return false;
    out.clear();
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

bool read_head(const std::string& path, uint8_t* buf, size_t n) {
    FILE* f = open_any(path);
    if (!f) return false;
    bool ok = n == 0 || std::fread(buf, 1, n, f) == n;
    std::fclose(f);
    return ok;
}

std::string to_console(const std::string& utf8) {
#if defined(_WIN32)
    bool ascii = true;
    for (unsigned char c : utf8) {
        if (c >= 0x80) { ascii = false; break; }
    }
    if (ascii) return utf8;
    std::wstring w = widen(utf8, CP_UTF8);
    if (w.empty()) return utf8;
    int n = WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return utf8;
    std::string s(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
#else
    return utf8;
#endif
}

}  // namespace platform
