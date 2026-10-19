cmake_minimum_required(VERSION 3.20)
# Bump VERSION before tagging a release (the tag build checks they match).
project(fh2_mod_platform VERSION 2.7.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT WIN32)
    message(FATAL_ERROR "Targets Windows. On Linux use -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake")
endif()

# When this repo is added to a mod with add_subdirectory(), the mod gets:
#   fh2mod_platform        the d3dcompiler_47.dll to ship next to azimuth.exe
#   fh2mod_api             interface target with include/fh2mod.h
#   FH2MOD_PLATFORM_VERSION_STRING   "2.0.0", to pass in FH2ModInfo
set(FH2MOD_PLATFORM_VERSION ${PROJECT_VERSION})
set(FH2MOD_PLATFORM_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(FH2MOD_PLATFORM_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(FH2MOD_PLATFORM_VERSION_PATCH ${PROJECT_VERSION_PATCH})
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FH2MOD_PLATFORM_VERSION_STRING ${PROJECT_VERSION} PARENT_SCOPE)
endif()
configure_file(cmake/platform_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/platform_version.h)

set(_warn)
if(MSVC)
    set(_warn /W4 /permissive- /utf-8 /EHsc)
else()
    # -fms-extensions: __try/__except around each mod's fh2mod_init.
    set(_warn -Wall -Wextra -Wno-missing-field-initializers -fms-extensions)
endif()

add_library(fh2mod_api INTERFACE)
target_include_directories(fh2mod_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(fh2mod_api INTERFACE FH2MOD_PLATFORM_VERSION="${PROJECT_VERSION}")

# MinHook (BSD 2-Clause, third_party/minhook/LICENSE.txt), built into the
# platform so every mod's hooks go through one copy of it.
set(_minhook third_party/minhook/src)
set(_minhook_src ${_minhook}/hook.c ${_minhook}/buffer.c ${_minhook}/trampoline.c ${_minhook}/hde/hde64.c)
set_source_files_properties(${_minhook_src} PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<BOOL:${MSVC}>>:-w>")

add_library(fh2mod_platform SHARED src/platform.cpp src/strings.cpp src/updates.cpp src/files.cpp
            src/database.cpp src/crash.cpp src/hooks.cpp src/settings.cpp src/version.rc ${_minhook_src})
target_include_directories(fh2mod_platform PRIVATE third_party/minhook/include ${_minhook})
if(NOT MSVC)
    # InterlockedCompareExchange128, for rewriting call sites atomically.
    set_source_files_properties(src/hooks.cpp PROPERTIES COMPILE_OPTIONS -mcx16)
endif()
if(NOT MSVC)
    target_sources(fh2mod_platform PRIVATE src/d3dcompiler_47.def)
    target_link_options(fh2mod_platform PRIVATE "$<$<CONFIG:Release>:-Wl,--gc-sections;-s>" -static)
endif()
target_compile_options(fh2mod_platform PRIVATE ${_warn} "$<$<AND:$<NOT:$<BOOL:${MSVC}>>,$<CONFIG:Release>>:-O2;-ffunction-sections;-fdata-sections>")
target_compile_definitions(fh2mod_platform PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
                           FH2MOD_PLATFORM_VERSION="${PROJECT_VERSION}")
target_include_directories(fh2mod_platform PRIVATE include src ${CMAKE_CURRENT_BINARY_DIR}/generated)
# Only kernel32/user32 (and the C runtime) at load time: azimuth.exe loads
# this DLL very early, and an app-folder version.dll or winhttp.dll proxy
# from some other mod must not get pulled in by us. WinHTTP is loaded from
# System32 when the first update check runs.
target_link_libraries(fh2mod_platform PRIVATE user32)
set_target_properties(fh2mod_platform PROPERTIES OUTPUT_NAME d3dcompiler_47 PREFIX "")

# Only when this is the top-level project: the example mod and the tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_library(hello_mod SHARED examples/hello_mod/hello_mod.cpp)
    target_link_libraries(hello_mod PRIVATE fh2mod_api)
    target_compile_options(hello_mod PRIVATE ${_warn})
    if(NOT MSVC)
        target_link_options(hello_mod PRIVATE -static)
    endif()
    set_target_properties(hello_mod PROPERTIES OUTPUT_NAME HelloMod PREFIX "")

    option(FH2MOD_TESTS "Build the offline test" OFF)
    if(FH2MOD_TESTS)
        add_executable(platform_test tests/platform_test.cpp src/strings.cpp src/updates.cpp)
        target_include_directories(platform_test PRIVATE include src)
        target_compile_definitions(platform_test PRIVATE UNICODE _UNICODE)
        target_link_options(platform_test PRIVATE -municode -static -Wl,--image-base=0x10000000)
        add_library(test_mod SHARED tests/test_mod.cpp)
        target_link_libraries(test_mod PRIVATE fh2mod_api)
        set_target_properties(test_mod PROPERTIES OUTPUT_NAME TestMod PREFIX "")
        target_link_options(test_mod PRIVATE -static)
        add_library(precise_mod SHARED tests/precise_mod.cpp)
        target_link_libraries(precise_mod PRIVATE fh2mod_api)
        set_target_properties(precise_mod PROPERTIES OUTPUT_NAME PreciseTest PREFIX "")
        target_link_options(precise_mod PRIVATE -static)
        add_library(aaa_mod SHARED tests/aaa_mod.cpp)
        target_link_libraries(aaa_mod PRIVATE fh2mod_api)
        set_target_properties(aaa_mod PROPERTIES OUTPUT_NAME AaaMod PREFIX "")
        target_link_options(aaa_mod PRIVATE -static)
        # The hook test: tests/run_hook_test.sh <forza_durango_release_final.exe>
        add_executable(hook_test tests/hook_test.cpp)
        target_compile_definitions(hook_test PRIVATE UNICODE _UNICODE)
        target_link_options(hook_test PRIVATE -municode -static -Wl,--image-base=0x10000000)
        add_library(hook_mod SHARED tests/hook_mod.cpp)
        target_link_libraries(hook_mod PRIVATE fh2mod_api)
        set_target_properties(hook_mod PROPERTIES OUTPUT_NAME HookMod PREFIX "")
        target_link_options(hook_mod PRIVATE -static)
        add_library(fake_winmm SHARED tests/fake_winmm.cpp)
        set_target_properties(fake_winmm PROPERTIES OUTPUT_NAME winmm PREFIX "")
        target_link_options(fake_winmm PRIVATE -static)
        add_library(legacy_mod SHARED tests/legacy_mod.cpp)
        set_target_properties(legacy_mod PROPERTIES OUTPUT_NAME Legacy PREFIX "")
        target_link_options(legacy_mod PRIVATE -static)
    endif()
endif()
