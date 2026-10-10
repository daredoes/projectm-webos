cmake_minimum_required(VERSION 3.21)
project(projectm-webos LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# projectM is cloned into third_party/projectm by the build (see .github/workflows/build.yml).
set(ENABLE_GLES ON CACHE BOOL "" FORCE)
# projectM sets USE_GLES after its vendored SOIL2 is configured; set it first so SOIL2 uses GLES3 headers.
set(USE_GLES ON)
set(ENABLE_PLAYLIST ON CACHE BOOL "" FORCE)
set(ENABLE_SYSTEM_PROJECTM_EVAL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/projectm EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple libpulse)

add_executable(projectm-webos src/main.cpp)
target_link_libraries(projectm-webos PRIVATE
    libprojectM::projectM
    libprojectM::playlist
    PkgConfig::SDL2
    PkgConfig::PULSE
    Threads::Threads)
# The TV's libstdc++ is older than the toolchain's; carry our own C++ runtime.
target_link_options(projectm-webos PRIVATE -static-libstdc++ -static-libgcc)

