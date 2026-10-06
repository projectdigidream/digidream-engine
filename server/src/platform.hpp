#pragma once
//
// The two file operations whose spelling differs on Windows, so that <windows.h>
// -- and its min/max macros -- never reaches main.cpp. Everything else the
// server does with files is standard C or <dirent.h>, which MinGW provides.
//
namespace dnp::server {

// Create one directory (not its parents); true if it exists afterwards.
bool make_dir(const char* path);

// Move `from` over `to`, REPLACING `to` if it exists. POSIX rename() does that
// atomically; Windows' rename() refuses an existing target, which would turn
// every save after the first into a silent failure.
bool replace_file(const char* from, const char* to);

}  // namespace dnp::server
