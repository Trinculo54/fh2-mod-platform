cmake_minimum_required(VERSION 3.20)
# Bump VERSION before tagging a release (the tag build checks they match).
project(fh2_mod_platform VERSION 2.9.0 LANGUAGES C CXX RC)

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
# The build's own name, in platform.log's first line, crash reports and ProductVersion. Left empty, it's the
# version above, as a release names itself (scripts/build.sh, which the tag build runs, passes it empty, so a
# build folder a test build named before can't keep that name). A local or test build can name itself,
# -DFH2MOD_PLATFORM_BUILD="2.9.0 dev", and platform.log's first line then says it isn't a release. Empty
# rather than the version itself, so a build folder kept across a version bump names the new version.
set(FH2MOD_PLATFORM_BUILD "" CACHE STRING "The build's name in platform.log, crash reports and ProductVersion (empty: the version)")
if(FH2MOD_PLATFORM_BUILD STREQUAL "")
    set(FH2MOD_PLATFORM_BUILD "${PROJECT_VERSION}")
endif()
configure_file(cmake/platform_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/platform_version.h)
# The resource compiler's output doesn't list the headers it read, so ProductVersion is rebuilt by hand when the name changes.
set_source_files_properties(src/version.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated/platform_version.h)

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

# SQLite (public domain, third_party/sqlite/VERSION.txt), built in for the game
# database changes. Windows has its own (winsqlite3.dll), but Wine and Proton
# don't, so relying on it switched database changes off for Linux players.
set(_sqlite_src third_party/sqlite/sqlite3.c)
set_source_files_properties(${_sqlite_src} PROPERTIES
    COMPILE_OPTIONS "$<IF:$<BOOL:${MSVC}>,/W0;/O1,-w;-Os>"
    COMPILE_DEFINITIONS "SQLITE_OMIT_LOAD_EXTENSION;SQLITE_DEFAULT_MEMSTATUS=0")

add_library(fh2mod_platform SHARED src/platform.cpp src/strings.cpp src/updates.cpp src/files.cpp
            src/database.cpp src/car_ids.cpp src/media_overrides.cpp src/car_packs.cpp src/titles.cpp src/session.cpp src/hotkeys.cpp src/commands.cpp src/crash.cpp src/hooks.cpp src/settings.cpp src/pad_layer.cpp src/xml_text.cpp src/lang.cpp src/text.cpp src/l10n.cpp src/ui_templates.cpp src/ui_rules.cpp src/ui_sounds.cpp src/popup_queue.cpp src/popups.cpp src/seen.cpp src/wine.cpp src/xaml_safety.cpp src/page_guard.cpp src/ui_player.cpp src/db_guards.cpp src/db_guard_rules.cpp src/version.rc ${_minhook_src} ${_sqlite_src})
target_include_directories(fh2mod_platform PRIVATE third_party/minhook/include ${_minhook} third_party/sqlite)
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
# The HUD layer preview (include/fh2mod_hud.h): the interface, its template and its state.
target_sources(fh2mod_platform PRIVATE src/hud_layer.cpp src/hud_state.cpp src/hud_template.cpp)
# Only kernel32/user32 (and the C runtime) at load time: azimuth.exe loads
# this DLL very early, and an app-folder version.dll or winhttp.dll proxy
# from some other mod must not get pulled in by us. WinHTTP is loaded from
# System32 when the first update check runs. The platform test checks the
# built DLL keeps to this, delay-loaded DLLs too (tests/imports_test.cpp).
target_link_libraries(fh2mod_platform PRIVATE user32)
set_target_properties(fh2mod_platform PROPERTIES OUTPUT_NAME d3dcompiler_47 PREFIX "")
# Azimuth 1.0.3 no longer imports d3dcompiler_47.dll, while 1.0.1 and later import DWrite.dll.
# The DLL forwards both, so the build leaves a copy of it under that name too.
add_custom_command(TARGET fh2mod_platform POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fh2mod_platform>
            $<TARGET_FILE_DIR:fh2mod_platform>/DWrite.dll)

# Only when this is the top-level project: the example mod and the tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_library(hello_mod SHARED examples/hello_mod/hello_mod.cpp)
    target_link_libraries(hello_mod PRIVATE fh2mod_api)
    target_compile_options(hello_mod PRIVATE ${_warn})
    if(NOT MSVC)
        target_link_options(hello_mod PRIVATE -static)
    endif()
    set_target_properties(hello_mod PROPERTIES OUTPUT_NAME HelloMod PREFIX "")

    # fh2mod.exe: runs mods' commands in the running game (docs/api.md, Commands).
    add_executable(fh2mod_cli tools/fh2mod.cpp)
    target_compile_definitions(fh2mod_cli PRIVATE UNICODE _UNICODE)
    target_compile_options(fh2mod_cli PRIVATE ${_warn})
    if(NOT MSVC)
        target_link_options(fh2mod_cli PRIVATE -municode -static)
    endif()
    set_target_properties(fh2mod_cli PROPERTIES OUTPUT_NAME fh2mod)

    option(FH2MOD_TESTS "Build the offline test" OFF)
    if(FH2MOD_TESTS)
        add_executable(platform_test tests/platform_test.cpp src/strings.cpp src/updates.cpp ${_sqlite_src})
        target_include_directories(platform_test PRIVATE include src third_party/sqlite ${CMAKE_CURRENT_BINARY_DIR}/generated)
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
        # The title gate test: tests/run_title_gate_test.sh
        add_executable(title_gate_test tests/title_gate_test.cpp)
        target_link_options(title_gate_test PRIVATE -static -Wl,--image-base=0x10000000)
        add_library(title_gate_mod SHARED tests/title_gate_mod.cpp)
        target_link_libraries(title_gate_mod PRIVATE fh2mod_api)
        set_target_properties(title_gate_mod PROPERTIES OUTPUT_NAME GateMod PREFIX "")
        target_link_options(title_gate_mod PRIVATE -static)
        # The hook test: tests/run_hook_test.sh <forza_durango_release_final.exe>
        add_executable(hook_test tests/hook_test.cpp)
        target_compile_definitions(hook_test PRIVATE UNICODE _UNICODE)
        target_compile_options(hook_test PRIVATE -fms-extensions)   # __try, for the database guards' converter run
        target_link_options(hook_test PRIVATE -municode -static -Wl,--image-base=0x10000000)
        add_library(hook_mod SHARED tests/hook_mod.cpp)
        target_link_libraries(hook_mod PRIVATE fh2mod_api)
        set_target_properties(hook_mod PROPERTIES OUTPUT_NAME HookMod PREFIX "")
        target_link_options(hook_mod PRIVATE -static)
        add_library(fake_winmm SHARED tests/fake_winmm.cpp)
        set_target_properties(fake_winmm PROPERTIES OUTPUT_NAME winmm PREFIX "")
        target_link_options(fake_winmm PRIVATE -static)
        add_library(offline_mod SHARED tests/offline_mod.cpp)
        target_link_libraries(offline_mod PRIVATE fh2mod_api)
        set_target_properties(offline_mod PROPERTIES OUTPUT_NAME OfflineMod PREFIX "")
        target_link_options(offline_mod PRIVATE -static)
        # A DLL that registers more than one mod, as a loader of other mods does (the "hosts" phases).
        add_library(host_mod SHARED tests/host_mod.cpp)
        target_link_libraries(host_mod PRIVATE fh2mod_api)
        set_target_properties(host_mod PROPERTIES OUTPUT_NAME HostMod PREFIX "")
        target_link_options(host_mod PRIVATE -static)
        add_library(legacy_mod SHARED tests/legacy_mod.cpp)
        set_target_properties(legacy_mod PROPERTIES OUTPUT_NAME Legacy PREFIX "")
        target_link_options(legacy_mod PRIVATE -static)
        # A stand-in for FH2MP's DLL that registers as FH2MP (the online phases from "online-late" on).
        add_library(fake_fh2mp SHARED tests/fake_fh2mp.cpp)
        target_link_libraries(fake_fh2mp PRIVATE fh2mod_api)
        set_target_properties(fake_fh2mp PROPERTIES OUTPUT_NAME FakeFH2MP PREFIX "")
        target_link_options(fake_fh2mp PRIVATE -static)
        # The database test, run by tests/run_platform_test.sh too; DbMod.dll plays each mod it needs.
        add_executable(database_test tests/database_test.cpp ${_sqlite_src})
        target_include_directories(database_test PRIVATE third_party/sqlite)
        target_compile_definitions(database_test PRIVATE UNICODE _UNICODE)
        target_link_options(database_test PRIVATE -municode -static)
        add_library(db_mod SHARED tests/db_mod.cpp)
        target_link_libraries(db_mod PRIVATE fh2mod_api)
        set_target_properties(db_mod PROPERTIES OUTPUT_NAME DbMod PREFIX "")
        target_link_options(db_mod PRIVATE -static)
        # The Ids car packs' cars are given (src/car_ids.cpp), on their own: run car_ids_test.exe.
        add_executable(car_ids_test tests/car_ids_test.cpp src/car_ids.cpp src/text.cpp ${_sqlite_src})
        target_include_directories(car_ids_test PRIVATE src third_party/sqlite)
        target_link_options(car_ids_test PRIVATE -static)
        # The game's list of mounted volumes as the platform reads it with Extra Options (src/volume_walk.hpp), on
        # made-up lists: run volume_walk_test.exe.
        add_executable(volume_walk_test tests/volume_walk_test.cpp)
        target_include_directories(volume_walk_test PRIVATE src)
        target_compile_options(volume_walk_test PRIVATE ${_warn})
        target_link_options(volume_walk_test PRIVATE -static)
        # The HUD layer preview: its template and state on their own (tests/run_hud_layer_test.sh), and the
        # mod the platform test's "hud" phase loads.
        add_executable(hud_layer_test tests/hud_layer_test.cpp src/hud_state.cpp src/hud_template.cpp src/strings.cpp)
        target_include_directories(hud_layer_test PRIVATE include src third_party/sqlite)
        target_compile_definitions(hud_layer_test PRIVATE UNICODE _UNICODE)
        target_link_options(hud_layer_test PRIVATE -municode -static)
        add_library(hud_mod SHARED tests/hud_mod.cpp)
        target_link_libraries(hud_mod PRIVATE fh2mod_api)
        set_target_properties(hud_mod PROPERTIES OUTPUT_NAME HudTestMod PREFIX "")
        target_link_options(hud_mod PRIVATE -static)
        # Mods' text in the platform's XAML (src/xml_text.cpp), on its own: run xml_text_test.exe.
        add_executable(xml_text_test tests/xml_text_test.cpp src/xml_text.cpp)
        target_include_directories(xml_text_test PRIVATE src)
        target_link_options(xml_text_test PRIVATE -static)
        # The platform's text in any language (src/text.cpp): capitals, lengths in characters, messages kept
        # until the game has its language. No game: run text_test.exe.
        add_executable(text_test tests/text_test.cpp src/text.cpp)
        target_include_directories(text_test PRIVATE src)
        target_link_options(text_test PRIVATE -static)
        # Mods' translations and text in the game's language (src/l10n.cpp), with no game: run l10n_test.exe.
        add_executable(l10n_test tests/l10n_test.cpp src/l10n.cpp src/text.cpp)
        target_include_directories(l10n_test PRIVATE include src)
        target_compile_definitions(l10n_test PRIVATE UNICODE _UNICODE)
        target_compile_options(l10n_test PRIVATE ${_warn})
        target_link_options(l10n_test PRIVATE -municode -static)
        # What the player has already seen (src/seen.cpp, fh2-mods\platform-seen.txt), with no game: run seen_test.exe.
        add_executable(seen_test tests/seen_test.cpp src/seen.cpp)
        target_include_directories(seen_test PRIVATE src)
        target_compile_options(seen_test PRIVATE ${_warn})
        target_link_options(seen_test PRIVATE -static)
        # The pop-up queue's rules (src/popup_queue.cpp: pop-ups, tips, pacing, text), with no game and a fake clock:
        # run ui_queue_test.exe.
        add_executable(ui_queue_test tests/ui_queue_test.cpp src/popup_queue.cpp src/text.cpp)
        target_include_directories(ui_queue_test PRIVATE include src)
        target_compile_options(ui_queue_test PRIVATE ${_warn})
        target_link_options(ui_queue_test PRIVATE -static)
        # The rules of message boxes, UI sounds and pages' help bars and hint lines (src/ui_rules.cpp), with no game and a
        # fake clock: run ui_rules_test.exe.
        add_executable(ui_rules_test tests/ui_rules_test.cpp src/ui_rules.cpp src/text.cpp)
        target_include_directories(ui_rules_test PRIVATE include src)
        target_compile_options(ui_rules_test PRIVATE ${_warn})
        target_link_options(ui_rules_test PRIVATE -static)
        # The platform's own UI files, the check pages of a mod's own design pass and the page crash guard's state
        # (src/xaml_safety.cpp), with no game: a small fake UI.zip and texture zip of its own. Run xaml_safety_test.exe
        # (with FH2_MEDIA set to an install's media folder, also against the real UI.zip and textures).
        add_executable(xaml_safety_test tests/xaml_safety_test.cpp src/xaml_safety.cpp src/strings.cpp)
        target_include_directories(xaml_safety_test PRIVATE src)
        target_compile_options(xaml_safety_test PRIVATE ${_warn})
        target_link_options(xaml_safety_test PRIVATE -static)
        # The database guards' rules (src/db_guard_rules.cpp), with no game: run db_guards_test.exe; given the game's exe,
        # it also checks their code against it.
        add_executable(db_guards_test tests/db_guards_test.cpp src/db_guard_rules.cpp ${_sqlite_src})
        target_include_directories(db_guards_test PRIVATE src third_party/sqlite)
        target_compile_options(db_guards_test PRIVATE ${_warn})
        target_link_options(db_guards_test PRIVATE -static)
        # What's different under Wine and Proton (src/wine.cpp), on its own: run wine_test.exe.
        add_executable(wine_test tests/wine_test.cpp src/wine.cpp)
        target_include_directories(wine_test PRIVATE src)
        target_link_options(wine_test PRIVATE -static)
        # What the platform DLL imports, load-time and delay-loaded (the rule above fh2mod_platform's
        # target_link_libraries), read from the d3dcompiler_47.dll and DWrite.dll next to it: run imports_test.exe.
        # It imports ADVAPI32.dll and delay-loads VERSION.dll itself, so the check is seen to find both.
        add_executable(imports_test tests/imports_test.cpp)
        target_compile_definitions(imports_test PRIVATE UNICODE _UNICODE)
        target_compile_options(imports_test PRIVATE ${_warn})
        target_link_libraries(imports_test PRIVATE advapi32 version)
        target_link_options(imports_test PRIVATE -municode -static -Wl,--delayload=version.dll)
        add_dependencies(imports_test fh2mod_platform)
        # The pad layer preview's rules (src/pad_engine.hpp), with no game: run pad_engine_test.exe.
        add_executable(pad_engine_test tests/pad_engine_test.cpp)
        target_include_directories(pad_engine_test PRIVATE include src)
        target_link_options(pad_engine_test PRIVATE -static)
        # The pad layer against the real game exe: tests/run_pad_test.sh <forza_durango_release_final.exe>
        add_executable(pad_hook_test tests/pad_hook_test.cpp)
        target_compile_definitions(pad_hook_test PRIVATE UNICODE _UNICODE)
        target_link_options(pad_hook_test PRIVATE -municode -static -Wl,--image-base=0x10000000)
        add_library(pad_mod SHARED tests/pad_mod.cpp)
        target_link_libraries(pad_mod PRIVATE fh2mod_api)
        set_target_properties(pad_mod PROPERTIES OUTPUT_NAME PadMod PREFIX "")
        target_link_options(pad_mod PRIVATE -static)
    endif()
endif()
