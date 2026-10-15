// Compiles the OTA library's C semver implementation as its own C
// translation unit for the native test build (it is not C++-clean, so it
// cannot be #included into the C++ test file like the other suites do with
// .cpp sources). The tests link against it via semver.h.
#include "semver.c"
