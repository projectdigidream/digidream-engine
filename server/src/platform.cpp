#include "platform.hpp"

#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // MinGW's libstdc++ already defines it
#endif
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace dnp::server {

#ifdef _WIN32
bool make_dir(const char* path) {
    if (::_mkdir(path) == 0) return true;
    const DWORD a = ::GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool replace_file(const char* from, const char* to) {
    return ::MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
#else
bool make_dir(const char* path) {
    if (::mkdir(path, 0755) == 0) return true;
    struct stat st;
    return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
bool replace_file(const char* from, const char* to) { return std::rename(from, to) == 0; }
#endif

}  // namespace dnp::server
