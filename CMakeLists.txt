cmake_minimum_required(VERSION 3.21)
project(QMud LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force Qt deployment API to use patchelf on Linux to avoid file(RPATH_SET)
# failures with plugin binaries that don't carry pre-existing RUNPATH entries.
if (UNIX AND NOT APPLE)
    set(QT_DEPLOY_USE_PATCHELF ON)
    set(QT_DEPLOY_PATCHELF_EXECUTABLE "patchelf")
endif ()

option(QMUD_ENABLE_LUA_SCRIPTING "Enable Lua scripting callbacks" ON)
option(QMUD_ENABLE_TESTING "Enable QMud QtTest/CTest targets" ON)
option(QMUD_ENABLE_GUI_TESTS "Enable GUI-focused QtTest suites" OFF)
option(QMUD_CI_BUILD "Mark runtime display version as CI build (-ci suffix)" OFF)
option(QMUD_RELEASE_NATIVE_OPTIMIZATIONS "Tune Release builds for the local CPU (-march=native or /arch:AVX2)" OFF)
set(QMUD_SYSTEM_LUA_MODULES_PREFIX "" CACHE PATH
        "Optional root path for system Lua module files (socket.lua/mime.lua/ltn12.lua/socket/*.lua/json.lua/re.lua/json/)")
set(QMUD_LPEG_PROVIDER "AUTO" CACHE STRING "Lua LPeg provider: AUTO or SYSTEM")
set(QMUD_BC_PROVIDER "AUTO" CACHE STRING "Lua bc provider: AUTO, SYSTEM, or BUNDLED")
set(QMUD_LUASOCKET_PROVIDER "AUTO" CACHE STRING "LuaSocket provider: AUTO or SYSTEM")
set_property(CACHE QMUD_LPEG_PROVIDER PROPERTY STRINGS AUTO SYSTEM)
set_property(CACHE QMUD_BC_PROVIDER PROPERTY STRINGS AUTO SYSTEM BUNDLED)
set_property(CACHE QMUD_LUASOCKET_PROVIDER PROPERTY STRINGS AUTO SYSTEM)
string(TOUPPER "${QMUD_LPEG_PROVIDER}" QMUD_LPEG_PROVIDER)
string(TOUPPER "${QMUD_BC_PROVIDER}" QMUD_BC_PROVIDER)
string(TOUPPER "${QMUD_LUASOCKET_PROVIDER}" QMUD_LUASOCKET_PROVIDER)
if (NOT QMUD_LPEG_PROVIDER MATCHES "^(AUTO|SYSTEM)$")
    message(FATAL_ERROR "QMUD_LPEG_PROVIDER must be AUTO or SYSTEM")
endif ()
if (NOT QMUD_BC_PROVIDER MATCHES "^(AUTO|SYSTEM|BUNDLED)$")
    message(FATAL_ERROR "QMUD_BC_PROVIDER must be AUTO, SYSTEM, or BUNDLED")
endif ()
if (NOT QMUD_LUASOCKET_PROVIDER MATCHES "^(AUTO|SYSTEM)$")
    message(FATAL_ERROR "QMUD_LUASOCKET_PROVIDER must be AUTO or SYSTEM")
endif ()
set(QMUD_RESOLVED_LPEG_PROVIDER "${QMUD_LPEG_PROVIDER}")
set(QMUD_RESOLVED_BC_PROVIDER "${QMUD_BC_PROVIDER}")
set(QMUD_RESOLVED_LUASOCKET_PROVIDER "${QMUD_LUASOCKET_PROVIDER}")
if (WIN32)
    set(QMUD_LUA_MODULE_CORE_NAMES core.dll core.so)
    set(QMUD_LUA_MODULE_CORE_OUTPUT_NAME core.dll)
elseif (APPLE)
    set(QMUD_LUA_MODULE_CORE_NAMES core.so core.dylib)
    set(QMUD_LUA_MODULE_CORE_OUTPUT_NAME core.so)
else ()
    set(QMUD_LUA_MODULE_CORE_NAMES core.so)
    set(QMUD_LUA_MODULE_CORE_OUTPUT_NAME core.so)
endif ()

set(QMUD_LUA_MODULE_SEARCH_PATHS
        "${QMUD_SYSTEM_LUA_MODULES_PREFIX}"
        /usr/lib64
        /usr/lib
        /usr/local/lib64
        /usr/local/lib
        /opt/homebrew/lib
        /opt/local/lib)
if (CMAKE_LIBRARY_ARCHITECTURE)
    list(APPEND QMUD_LUA_MODULE_SEARCH_PATHS
            "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
            "/usr/local/lib/${CMAKE_LIBRARY_ARCHITECTURE}")
endif ()
list(REMOVE_DUPLICATES QMUD_LUA_MODULE_SEARCH_PATHS)

if (POLICY CMP0069)
    cmake_policy(SET CMP0069 NEW)
endif ()
option(QMUD_PROBE_IPO "Probe toolchain for IPO/LTO support at configure time" ON)
if (QMUD_PROBE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT QMUD_IPO_SUPPORTED OUTPUT QMUD_IPO_OUTPUT)
    if (NOT QMUD_IPO_SUPPORTED)
        message(STATUS "IPO/LTO is unavailable for this toolchain: ${QMUD_IPO_OUTPUT}")
    endif ()
else ()
    set(QMUD_IPO_SUPPORTED FALSE)
    set(QMUD_IPO_OUTPUT "Skipped (QMUD_PROBE_IPO=OFF)")
    message(STATUS "IPO/LTO probe skipped: ${QMUD_IPO_OUTPUT}")
endif ()

function(qmud_apply_release_optimizations target_name)
    if (MSVC)
        target_compile_options(${target_name} PRIVATE
                $<$<CONFIG:Release>:/O2>
                $<$<CONFIG:Release>:/Oi>
                $<$<CONFIG:Release>:/Gy>
                $<$<CONFIG:Release>:/GL>
        )
        target_link_options(${target_name} PRIVATE
                $<$<CONFIG:Release>:/OPT:REF>
                $<$<CONFIG:Release>:/OPT:ICF>
                $<$<CONFIG:Release>:/LTCG>
        )
        if (QMUD_RELEASE_NATIVE_OPTIMIZATIONS)
            target_compile_options(${target_name} PRIVATE
                    $<$<CONFIG:Release>:/arch:AVX2>
            )
        endif ()
    else ()
        target_compile_options(${target_name} PRIVATE
                $<$<CONFIG:Release>:-O3>
                $<$<CONFIG:Release>:-fomit-frame-pointer>
        )
        if (NOT APPLE)
            target_link_options(${target_name} PRIVATE
                    $<$<CONFIG:Release>:-Wl,-O1>
            )
        endif ()
        if (QMUD_RELEASE_NATIVE_OPTIMIZATIONS)
            target_compile_options(${target_name} PRIVATE
                    $<$<CONFIG:Release>:-march=native>
                    $<$<CONFIG:Release>:-mtune=native>
            )
        endif ()
    endif ()

    target_compile_definitions(${target_name} PRIVATE
            $<$<CONFIG:Release>:NDEBUG>
    )

    if (QMUD_IPO_SUPPORTED)
        set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif ()
endfunction()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Sql PrintSupport)
find_package(Qt6 QUIET COMPONENTS Multimedia)
if (QMUD_ENABLE_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    include(CTest)
    enable_testing()
endif ()
find_package(PkgConfig QUIET)
find_package(Python3 QUIET COMPONENTS Interpreter)

set(QMUD_ZLIB_TARGET "")
if (PkgConfig_FOUND)
    pkg_check_modules(ZLIB_PKG QUIET IMPORTED_TARGET zlib)
endif ()
if (TARGET PkgConfig::ZLIB_PKG)
    # Prefer pkg-config zlib provider (zlib-ng compat on when available).
    set(QMUD_ZLIB_TARGET PkgConfig::ZLIB_PKG)
    message(STATUS "Using pkg-config zlib provider (prefer zlib-ng compat when available)")
else ()
    find_package(ZLIB REQUIRED)
    set(QMUD_ZLIB_TARGET ZLIB::ZLIB)
    message(STATUS "Using CMake ZLIB provider fallback")
endif ()

# Basic Qt project setup (sets AUTOUIC/AUTOMOC/AUTORCC and other Qt defaults)
qt_standard_project_setup()
set(QMUD_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Core Qt entry-point sources (start minimal, expand as modules are ported)
set(QMUD_SOURCES
        Main.cpp
        MainWindowHostResolver.cpp
        MainFrame.cpp
        MainFrameActionUtils.cpp
        AppController.cpp
        UpdateCheckUtils.cpp
        ReloadPlannerUtils.cpp
        ReloadStateUtils.cpp
        Environment.cpp
        Accelerators.cpp
        AliasMatchUtils.cpp
        CommandPatternUtils.cpp
        CommandQueueUtils.cpp
        CommandTextUtils.cpp
        NameGeneration.cpp
        DoubleMetaphone.cpp
        ErrorDescriptions.cpp
        AsciiArt.cpp
        WorldPreferencesRoutingUtils.cpp
        WorldChildWindow.cpp
        dialogs/TipDialog.cpp
        dialogs/WelcomeDialog.cpp
        dialogs/WelcomeUpgradeDialog.cpp
        ActivityDocument.cpp
        ActivityWindow.cpp
        dialogs/ImportXmlDialog.cpp
        WorldDocument.cpp
        WorldRuntime.cpp
        WorldCommandProcessor.cpp
        SpeedwalkParser.cpp
        TimerSchedulingUtils.cpp
        AnsiSgrParseUtils.cpp
        WorldView.cpp
        WorldSocket.cpp
        TelnetProcessor.cpp
        WorldOptions.cpp
        WorldOptionDefaults.cpp
        MxpDebugWindow.cpp
        EncodingUtils.cpp
        ColorUtils.cpp
        FontUtils.cpp
        BraceMatch.cpp
        LuaCallbackEngine.cpp
        LuaApiExport.cpp
        LuaFunctionMetadata.cpp
        InfoTypesMetadata.cpp
        StringUtils.cpp
        LuaSupport.cpp
        AcceleratorUtils.cpp
        dialogs/LogSessionDialog.cpp
        MdiTabs.cpp
        dialogs/PluginsDialog.cpp
        dialogs/PluginWizardDialog.cpp
        dialogs/GeneratedNameDialog.cpp
        dialogs/ColourPickerDialog.cpp
        dialogs/ConfirmPreambleDialog.cpp
        dialogs/HighlightPhraseDialog.cpp
        dialogs/SpellCheckDialog.cpp
        dialogs/FindDialog.cpp
        dialogs/CommandHistoryDialog.cpp
        dialogs/WorldPreferencesDialog.cpp
        dialogs/WorldEditUtils.cpp
        dialogs/WorldAliasDialog.cpp
        dialogs/WorldTriggerDialog.cpp
        dialogs/WorldTimerDialog.cpp
        dialogs/GlobalPreferencesDialog.cpp
)

# Optionally collect headers (helps IDEs) — not required for build
set(QMUD_HEADERS
        MainWindowHost.h
        MainWindowHostResolver.h
        Version.h
        MainFrame.h
        MainFrameActionUtils.h
        AppController.h
        Environment.h
        ReloadPlannerUtils.h
        ReloadRecoveryUtils.h
        ReloadStateUtils.h
        Accelerators.h
        AcceleratorUtils.h
        AliasMatchUtils.h
        CommandMappingTypes.h
        CommandPatternUtils.h
        CommandQueueUtils.h
        CommandTextUtils.h
        ErrorDescriptions.h
        InfoTypesMetadata.h
        InfoTypeData.inc
        LuaFunctionTypes.h
        SqliteCompat.h
        AsciiArt.h
        WorldChildWindow.h
        dialogs/TipDialog.h
        dialogs/WelcomeDialog.h
        dialogs/WelcomeUpgradeDialog.h
        ActivityDocument.h
        ActivityWindow.h
        dialogs/ImportXmlDialog.h
        WorldDocument.h
        WorldRuntime.h
        MiniWindow.h
        WorldCommandProcessor.h
        SpeedwalkParser.h
        TimerSchedulingUtils.h
        AnsiSgrParseUtils.h
        WorldView.h
        CommandIds.h
        WorldSocketService.h
        WorldSocket.h
        TelnetProcessor.h
        WorldOptions.h
        WorldOptionDefaults.h
        WorldOptionsDataNumeric.inc
        MxpDiagnostics.h
        MxpDebugWindow.h
        BraceMatch.h
        LuaCallbackEngine.h
        LuaApiExport.h
        FileExtensions.h
        WorldPreferencesRoutingUtils.h
        dialogs/LogSessionDialog.h
        MdiTabs.h
        dialogs/PluginsDialog.h
        dialogs/PluginWizardDialog.h
        dialogs/GeneratedNameDialog.h
        dialogs/ColourPickerDialog.h
        dialogs/ConfirmPreambleDialog.h
        dialogs/HighlightPhraseDialog.h
        dialogs/SpellCheckDialog.h
        dialogs/FindDialog.h
        dialogs/CommandHistoryDialog.h
        dialogs/WorldPreferencesDialog.h
        dialogs/WorldEditUtils.h
        dialogs/WorldAliasDialog.h
        dialogs/WorldTriggerDialog.h
        dialogs/WorldTimerDialog.h
        dialogs/GlobalPreferencesDialog.h
)

set(QMUD_SOURCE_FILES_ABS)
foreach (QMUD_SOURCE_FILE IN LISTS QMUD_SOURCES)
    list(APPEND QMUD_SOURCE_FILES_ABS "${QMUD_SRC_DIR}/${QMUD_SOURCE_FILE}")
endforeach ()

set(QMUD_HEADER_FILES_ABS)
set(QMUD_MISSING_HEADER_FILES)
foreach (QMUD_HEADER_FILE IN LISTS QMUD_HEADERS)
    set(QMUD_HEADER_PATH "${QMUD_SRC_DIR}/${QMUD_HEADER_FILE}")
    if (EXISTS "${QMUD_HEADER_PATH}")
        list(APPEND QMUD_HEADER_FILES_ABS "${QMUD_HEADER_PATH}")
    else ()
        list(APPEND QMUD_MISSING_HEADER_FILES "${QMUD_HEADER_FILE}")
    endif ()
endforeach ()
if (QMUD_MISSING_HEADER_FILES)
    string(JOIN "\n  - " QMUD_MISSING_HEADER_FILES_JOINED ${QMUD_MISSING_HEADER_FILES})
    message(WARNING
            "Skipping missing QMud headers listed for IDE grouping:\n"
            "  - ${QMUD_MISSING_HEADER_FILES_JOINED}\n"
            "Build will continue; verify file renames/casing if unexpected.")
endif ()

add_executable(QMud ${QMUD_SOURCE_FILES_ABS} ${QMUD_HEADER_FILES_ABS})


# Ensure Qt auto tools are ON (redundant with qt_standard_project_setup, kept as fallback)
set_target_properties(QMud PROPERTIES
        CXX_STANDARD 20
        AUTOMOC ON
        AUTOUIC ON
        AUTORCC ON
)

set(QMUD_QT_MULTIMEDIA_TARGET "")
set(QMUD_ENABLE_SOUND_VALUE 0)
if (TARGET Qt6::Multimedia)
    set(QMUD_QT_MULTIMEDIA_TARGET Qt6::Multimedia)
    set(QMUD_ENABLE_SOUND_VALUE 1)
else ()
    message(WARNING "Qt6 Multimedia module not found; building with QMUD_ENABLE_SOUND=0")
endif ()

target_link_libraries(QMud PRIVATE Qt6::Widgets Qt6::Network Qt6::Sql Qt6::PrintSupport ${QMUD_ZLIB_TARGET} ${QMUD_QT_MULTIMEDIA_TARGET})
if (APPLE)
    target_link_libraries(QMud PRIVATE "-framework CoreServices")
endif ()
target_compile_definitions(QMud PRIVATE QMUD_ENABLE_SOUND=${QMUD_ENABLE_SOUND_VALUE})

if (QMUD_ENABLE_LUA_SCRIPTING)
    set(QMUD_LUA_LIBRARY_FILES "")
    set(QMUD_RESOLVED_LPEG_PROVIDER "${QMUD_LPEG_PROVIDER}")
    set(QMUD_RESOLVED_BC_PROVIDER "${QMUD_BC_PROVIDER}")
    set(QMUD_RESOLVED_LUASOCKET_PROVIDER "${QMUD_LUASOCKET_PROVIDER}")

    find_package(Lua 5.4 REQUIRED)
    target_compile_definitions(QMud PRIVATE QMUD_ENABLE_LUA_SCRIPTING)
    if (TARGET Lua::Lua)
        target_link_libraries(QMud PRIVATE Lua::Lua)
        set(QMUD_IMPORTED_LUA_LIBRARY "")
        get_target_property(QMUD_IMPORTED_LUA_LIBRARY Lua::Lua IMPORTED_LOCATION)
        if (NOT QMUD_IMPORTED_LUA_LIBRARY)
            get_target_property(QMUD_IMPORTED_LUA_LIBRARY Lua::Lua IMPORTED_LOCATION_RELEASE)
        endif ()
        if (NOT QMUD_IMPORTED_LUA_LIBRARY)
            get_target_property(QMUD_IMPORTED_LUA_LIBRARY Lua::Lua IMPORTED_LOCATION_DEBUG)
        endif ()
        if (QMUD_IMPORTED_LUA_LIBRARY AND EXISTS "${QMUD_IMPORTED_LUA_LIBRARY}")
            list(APPEND QMUD_LUA_LIBRARY_FILES "${QMUD_IMPORTED_LUA_LIBRARY}")
            file(REAL_PATH "${QMUD_IMPORTED_LUA_LIBRARY}" QMUD_IMPORTED_LUA_LIBRARY_REAL)
            if (QMUD_IMPORTED_LUA_LIBRARY_REAL AND EXISTS "${QMUD_IMPORTED_LUA_LIBRARY_REAL}")
                list(APPEND QMUD_LUA_LIBRARY_FILES "${QMUD_IMPORTED_LUA_LIBRARY_REAL}")
            endif ()
        endif ()
    else ()
        target_link_libraries(QMud PRIVATE ${LUA_LIBRARIES})
    endif ()
    foreach (QMUD_LUA_LIB_CANDIDATE IN LISTS LUA_LIBRARIES)
        if (IS_ABSOLUTE "${QMUD_LUA_LIB_CANDIDATE}" AND EXISTS "${QMUD_LUA_LIB_CANDIDATE}")
            list(APPEND QMUD_LUA_LIBRARY_FILES "${QMUD_LUA_LIB_CANDIDATE}")
            file(REAL_PATH "${QMUD_LUA_LIB_CANDIDATE}" QMUD_LUA_LIB_CANDIDATE_REAL)
            if (QMUD_LUA_LIB_CANDIDATE_REAL AND EXISTS "${QMUD_LUA_LIB_CANDIDATE_REAL}")
                list(APPEND QMUD_LUA_LIBRARY_FILES "${QMUD_LUA_LIB_CANDIDATE_REAL}")
            endif ()
        elseif (QMUD_LUA_LIB_CANDIDATE MATCHES "^-l(.+)$")
            # Resolve linker-style Lua entries (eg: -llua-5.4) to concrete shared objects for AppImage staging.
            set(QMUD_LUA_LINK_NAME "${CMAKE_MATCH_1}")
            find_library(QMUD_LUA_LINK_RESOLVED
                    NAMES "${QMUD_LUA_LINK_NAME}"
                    PATHS /usr/lib64 /usr/lib /usr/local/lib64 /usr/local/lib
            )
            if (QMUD_LUA_LINK_RESOLVED AND EXISTS "${QMUD_LUA_LINK_RESOLVED}")
                list(APPEND QMUD_LUA_LIBRARY_FILES "${QMUD_LUA_LINK_RESOLVED}")
                file(REAL_PATH "${QMUD_LUA_LINK_RESOLVED}" QMUD_LUA_LINK_RESOLVED_REAL)
                if (QMUD_LUA_LINK_RESOLVED_REAL AND EXISTS "${QMUD_LUA_LINK_RESOLVED_REAL}")
                    list(APPEND QMUD_LUA_LIBRARY_FILES "${QMUD_LUA_LINK_RESOLVED_REAL}")
                endif ()
            endif ()
        endif ()
    endforeach ()
    if (NOT QMUD_LUA_LIBRARY_FILES)
        # Fallback for distro setups where find_package() only provides includes and linker flags.
        find_library(QMUD_LUA_FALLBACK_SHARED
                NAMES lua5.4 lua-5.4 lua54 lua
                PATHS /usr/lib64 /usr/lib /usr/local/lib64 /usr/local/lib
        )
        if (QMUD_LUA_FALLBACK_SHARED AND EXISTS "${QMUD_LUA_FALLBACK_SHARED}")
            list(APPEND QMUD_LUA_LIBRARY_FILES "${QMUD_LUA_FALLBACK_SHARED}")
            file(REAL_PATH "${QMUD_LUA_FALLBACK_SHARED}" QMUD_LUA_FALLBACK_SHARED_REAL)
            if (QMUD_LUA_FALLBACK_SHARED_REAL AND EXISTS "${QMUD_LUA_FALLBACK_SHARED_REAL}")
                list(APPEND QMUD_LUA_LIBRARY_FILES "${QMUD_LUA_FALLBACK_SHARED_REAL}")
            endif ()
        endif ()
    endif ()
    list(FILTER QMUD_LUA_LIBRARY_FILES INCLUDE REGEX ".*/liblua[^/]*\\.so(\\..*)?$")
    list(REMOVE_DUPLICATES QMUD_LUA_LIBRARY_FILES)
    if (DEFINED LUA_INCLUDE_DIR)
        target_include_directories(QMud SYSTEM BEFORE PRIVATE ${LUA_INCLUDE_DIR})
    elseif (DEFINED LUA_INCLUDE_DIRS)
        target_include_directories(QMud SYSTEM BEFORE PRIVATE ${LUA_INCLUDE_DIRS})
    endif ()
    target_sources(QMud PRIVATE src/Sqlite3Lua.cpp)
    set(QMUD_HAS_SYSTEM_LPEG OFF)
    set(QMUD_LUA_INTERPRETER "")
    if (DEFINED LUA_EXECUTABLE AND EXISTS "${LUA_EXECUTABLE}")
        set(QMUD_LUA_INTERPRETER "${LUA_EXECUTABLE}")
    else ()
        find_program(QMUD_LUA_INTERPRETER NAMES lua lua5.4 lua54)
    endif ()

    if (QMUD_LPEG_PROVIDER STREQUAL "SYSTEM")
        set(QMUD_HAS_SYSTEM_LPEG ON)
    elseif (QMUD_LPEG_PROVIDER STREQUAL "AUTO")
        if (QMUD_LUA_INTERPRETER)
            execute_process(
                    COMMAND "${QMUD_LUA_INTERPRETER}" -e "require('lpeg')"
                    RESULT_VARIABLE QMUD_LPEG_REQUIRE_RESULT
                    OUTPUT_QUIET
                    ERROR_QUIET
            )
            if (QMUD_LPEG_REQUIRE_RESULT EQUAL 0)
                set(QMUD_HAS_SYSTEM_LPEG ON)
            endif ()
        endif ()

        if (NOT QMUD_HAS_SYSTEM_LPEG)
            find_file(QMUD_SYSTEM_LPEG_SO
                    NAMES lpeg.so
                    PATHS ${QMUD_LUA_MODULE_SEARCH_PATHS}
                    PATH_SUFFIXES lua/5.4 lua/5.3 lua/5.2 lua/5.1
            )
            if (QMUD_SYSTEM_LPEG_SO)
                set(QMUD_HAS_SYSTEM_LPEG ON)
            endif ()
        endif ()
    endif ()

    if (QMUD_HAS_SYSTEM_LPEG)
        set(QMUD_RESOLVED_LPEG_PROVIDER "SYSTEM")
        message(STATUS "Using system LPeg module (lua-lpeg)")
        target_compile_definitions(QMud PRIVATE QMUD_HAS_SYSTEM_LPEG=1)
    else ()
        message(FATAL_ERROR
                "System LPeg module (lpeg.so) not found. Install lua-lpeg or set "
                "QMUD_SYSTEM_LUA_MODULES_PREFIX to a path containing lpeg.so.")
    endif ()

    set(QMUD_HAS_SYSTEM_BC OFF)
    if (QMUD_BC_PROVIDER STREQUAL "SYSTEM")
        set(QMUD_HAS_SYSTEM_BC ON)
    elseif (QMUD_BC_PROVIDER STREQUAL "AUTO")
        if (QMUD_LUA_INTERPRETER)
            execute_process(
                    COMMAND "${QMUD_LUA_INTERPRETER}" -e "require('bc')"
                    RESULT_VARIABLE QMUD_BC_REQUIRE_RESULT
                    OUTPUT_QUIET
                    ERROR_QUIET
            )
            if (QMUD_BC_REQUIRE_RESULT EQUAL 0)
                set(QMUD_HAS_SYSTEM_BC ON)
            endif ()
        endif ()
        if (NOT QMUD_HAS_SYSTEM_BC)
            find_file(QMUD_SYSTEM_BC_SO
                    NAMES bc.so
                    PATHS ${QMUD_LUA_MODULE_SEARCH_PATHS}
                    PATH_SUFFIXES lua/5.4 lua/5.3 lua/5.2 lua/5.1
            )
            if (QMUD_SYSTEM_BC_SO)
                set(QMUD_HAS_SYSTEM_BC ON)
            endif ()
        endif ()
    endif ()

    if (QMUD_HAS_SYSTEM_BC)
        set(QMUD_RESOLVED_BC_PROVIDER "SYSTEM")
        message(STATUS "Using system Lua bc module")
        target_compile_definitions(QMud PRIVATE QMUD_HAS_SYSTEM_BC=1)
    else ()
        set(QMUD_RESOLVED_BC_PROVIDER "BUNDLED")
        message(STATUS "System bc not found, building bundled bc fallback")
        target_sources(QMud PRIVATE src/scripting/Number.c)
        target_sources(QMud PRIVATE src/scripting/Lbc.c)
        set_source_files_properties(
                src/scripting/Number.c
                PROPERTIES COMPILE_OPTIONS "-Wno-old-style-definition;-Wno-unknown-pragmas;-Wno-unused-but-set-variable;-Wno-unused-parameter"
        )
        target_compile_definitions(QMud PRIVATE QMUD_BUNDLED_BC=1)
    endif ()

    set(QMUD_HAS_SYSTEM_LUASOCKET OFF)
    if (QMUD_LUA_INTERPRETER)
        execute_process(
                COMMAND "${QMUD_LUA_INTERPRETER}" -e "require('socket'); require('mime')"
                RESULT_VARIABLE QMUD_LUASOCKET_REQUIRE_RESULT
                OUTPUT_QUIET
                ERROR_QUIET
        )
        if (QMUD_LUASOCKET_REQUIRE_RESULT EQUAL 0)
            set(QMUD_HAS_SYSTEM_LUASOCKET ON)
        endif ()
    endif ()
    if (NOT QMUD_HAS_SYSTEM_LUASOCKET)
        find_file(QMUD_SYSTEM_SOCKET_CORE_SO
                NAMES ${QMUD_LUA_MODULE_CORE_NAMES}
                PATHS ${QMUD_LUA_MODULE_SEARCH_PATHS}
                PATH_SUFFIXES "" socket lua/5.4/socket lua/5.3/socket lua/5.2/socket lua/5.1/socket
                NO_CMAKE_FIND_ROOT_PATH
        )
        find_file(QMUD_SYSTEM_MIME_CORE_SO
                NAMES ${QMUD_LUA_MODULE_CORE_NAMES}
                PATHS ${QMUD_LUA_MODULE_SEARCH_PATHS}
                PATH_SUFFIXES "" mime lua/5.4/mime lua/5.3/mime lua/5.2/mime lua/5.1/mime
                NO_CMAKE_FIND_ROOT_PATH
        )
        if (QMUD_SYSTEM_SOCKET_CORE_SO AND QMUD_SYSTEM_MIME_CORE_SO)
            set(QMUD_HAS_SYSTEM_LUASOCKET ON)
        endif ()
    endif ()
    if (QMUD_HAS_SYSTEM_LUASOCKET)
        set(QMUD_RESOLVED_LUASOCKET_PROVIDER "SYSTEM")
        message(STATUS "Using system LuaSocket modules (socket/mime)")
    elseif (QMUD_LUASOCKET_PROVIDER STREQUAL "SYSTEM")
        message(FATAL_ERROR
                "QMUD_LUASOCKET_PROVIDER=SYSTEM requires system LuaSocket modules (socket/mime), "
                "but they were not found in the build environment.")
    else ()
        message(FATAL_ERROR
                "System LuaSocket modules (socket/mime) not found. Bundled fallback has been removed; "
                "install LuaSocket modules in the build environment.")
    endif ()

    message(STATUS "Lua module provider resolution: lpeg=${QMUD_RESOLVED_LPEG_PROVIDER}, bc=${QMUD_RESOLVED_BC_PROVIDER}, luasocket=${QMUD_RESOLVED_LUASOCKET_PROVIDER}")
endif ()

# Qt target include root: keep build-visible sources isolated under src/.
target_include_directories(QMud PRIVATE "${QMUD_SRC_DIR}")

# Add optional subprojects (preserve original behavior)
foreach (dir lsqlite png scripting mxp lua luacom)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt")
        add_subdirectory(${dir})
    endif ()
endforeach ()

# Link common known targets if the subprojects provided them
foreach (tgt lsqlite png lua luacom)
    if (TARGET ${tgt})
        target_link_libraries(QMud PRIVATE ${tgt})
    endif ()
endforeach ()

if (QMUD_ENABLE_TESTING)
    add_subdirectory(tests)
endif ()

# Optional: resources (.qrc)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/qmud.qrc")
    # Compile the existing .qrc directly.
    target_sources(QMud PRIVATE resources/qmud.qrc)
endif ()

# Windows-only executable manifest (DPI awareness, long-path support, modern compatibility tags).
if (WIN32)
    set(QMUD_WINDOWS_MANIFEST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/QMud.Windows.manifest")
    set(QMUD_WINDOWS_RC_IN "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/QMud.Windows.rc.in")
    set(QMUD_WINDOWS_MANIFEST_DST "${CMAKE_CURRENT_BINARY_DIR}/QMud.Windows.manifest")
    set(QMUD_WINDOWS_RC_DST "${CMAKE_CURRENT_BINARY_DIR}/QMud.Windows.rc")
    set(QMUD_WINDOWS_VERSION_COMMAS "10,0,0,0")
    set(QMUD_WINDOWS_VERSION_DOTS "10.0.0.0")
    set(QMUD_WINDOWS_COMPANY_NAME "Abnormal Frequency")
    set(QMUD_WINDOWS_AUTHOR_NAME "Panagiotis Kalogiratos (Nodens)")
    set(QMUD_WINDOWS_PRODUCT_NAME "QMud")
    set(QMUD_WINDOWS_FILE_DESCRIPTION "QMud MUD Client")
    set(QMUD_WINDOWS_ORIGINAL_FILENAME "QMud.exe")
    set(QMUD_WINDOWS_LEGAL_COPYRIGHT "Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)")
    set(QMUD_WINDOWS_COMMENTS "Author: Panagiotis Kalogiratos (Nodens)")

    if (EXISTS "${QMUD_WINDOWS_MANIFEST_SRC}" AND EXISTS "${QMUD_WINDOWS_RC_IN}")
        configure_file("${QMUD_WINDOWS_MANIFEST_SRC}" "${QMUD_WINDOWS_MANIFEST_DST}" COPYONLY)
        configure_file("${QMUD_WINDOWS_RC_IN}" "${QMUD_WINDOWS_RC_DST}" @ONLY)
        target_sources(QMud PRIVATE "${QMUD_WINDOWS_RC_DST}")
    else ()
        message(WARNING "Windows manifest files are missing; QMud will use the default linker-generated manifest")
    endif ()
endif ()

# Sync skeleton/ into the build tree (copy only when new/changed).
set(SKELETON_SRC "${CMAKE_CURRENT_SOURCE_DIR}/skeleton")
set(SKELETON_DST "${CMAKE_CURRENT_BINARY_DIR}")
set(QMUD_SYSTEM_LUA_SEARCH_PATHS
        /usr/share/lua/5.4
        /usr/share/lua/5.3
        /usr/share/lua/5.2
        /usr/share/lua/5.1
        /usr/local/share/lua/5.4
        /usr/local/share/lua/5.3
        /usr/local/share/lua/5.2
        /usr/local/share/lua/5.1
)
if (QMUD_SYSTEM_LUA_MODULES_PREFIX)
    list(PREPEND QMUD_SYSTEM_LUA_SEARCH_PATHS "${QMUD_SYSTEM_LUA_MODULES_PREFIX}")
endif ()
list(REMOVE_DUPLICATES QMUD_SYSTEM_LUA_SEARCH_PATHS)
find_path(QMUD_SYSTEM_JSON_LUA_DIR
        NAMES init.lua decode.lua encode.lua util.lua
        PATHS ${QMUD_SYSTEM_LUA_SEARCH_PATHS}
        PATH_SUFFIXES json
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
)
find_file(QMUD_SYSTEM_SOCKET_LUA_FILE
        NAMES socket.lua
        PATHS ${QMUD_SYSTEM_LUA_SEARCH_PATHS}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
)
find_file(QMUD_SYSTEM_MIME_LUA_FILE
        NAMES mime.lua
        PATHS ${QMUD_SYSTEM_LUA_SEARCH_PATHS}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
)
find_file(QMUD_SYSTEM_LTN12_LUA_FILE
        NAMES ltn12.lua
        PATHS ${QMUD_SYSTEM_LUA_SEARCH_PATHS}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
)
find_file(QMUD_SYSTEM_JSON_TOP_LUA_FILE
        NAMES json.lua
        PATHS ${QMUD_SYSTEM_LUA_SEARCH_PATHS}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
)
find_file(QMUD_SYSTEM_RE_LUA_FILE
        NAMES re.lua
        PATHS ${QMUD_SYSTEM_LUA_SEARCH_PATHS}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
)
find_path(QMUD_SYSTEM_SOCKET_LUA_DIR
        NAMES http.lua ftp.lua smtp.lua tp.lua url.lua
        PATHS ${QMUD_SYSTEM_LUA_SEARCH_PATHS}
        PATH_SUFFIXES socket
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
)
if (QMUD_SYSTEM_SOCKET_LUA_DIR)
    find_file(QMUD_SYSTEM_SOCKET_DIR_SOCKET_LUA_FILE
            NAMES socket.lua
            PATHS "${QMUD_SYSTEM_SOCKET_LUA_DIR}"
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
    )
    find_file(QMUD_SYSTEM_SOCKET_DIR_LTN12_LUA_FILE
            NAMES ltn12.lua
            PATHS "${QMUD_SYSTEM_SOCKET_LUA_DIR}"
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
    )
    find_file(QMUD_SYSTEM_SOCKET_DIR_MIME_LUA_FILE
            NAMES mime.lua
            PATHS "${QMUD_SYSTEM_SOCKET_LUA_DIR}"
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
    )
    find_file(QMUD_SYSTEM_SOCKET_DIR_FTP_LUA_FILE
            NAMES ftp.lua
            PATHS "${QMUD_SYSTEM_SOCKET_LUA_DIR}"
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
    )
    find_file(QMUD_SYSTEM_SOCKET_DIR_HTTP_LUA_FILE
            NAMES http.lua
            PATHS "${QMUD_SYSTEM_SOCKET_LUA_DIR}"
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
    )
    find_file(QMUD_SYSTEM_SOCKET_DIR_SMTP_LUA_FILE
            NAMES smtp.lua
            PATHS "${QMUD_SYSTEM_SOCKET_LUA_DIR}"
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
    )
    find_file(QMUD_SYSTEM_SOCKET_DIR_TP_LUA_FILE
            NAMES tp.lua
            PATHS "${QMUD_SYSTEM_SOCKET_LUA_DIR}"
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
    )
    find_file(QMUD_SYSTEM_SOCKET_DIR_URL_LUA_FILE
            NAMES url.lua
            PATHS "${QMUD_SYSTEM_SOCKET_LUA_DIR}"
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
    )
endif ()
if (NOT QMUD_SYSTEM_SOCKET_DIR_SOCKET_LUA_FILE)
    set(QMUD_SYSTEM_SOCKET_DIR_SOCKET_LUA_FILE "${QMUD_SYSTEM_SOCKET_LUA_FILE}")
endif ()
if (NOT QMUD_SYSTEM_SOCKET_DIR_LTN12_LUA_FILE)
    set(QMUD_SYSTEM_SOCKET_DIR_LTN12_LUA_FILE "${QMUD_SYSTEM_LTN12_LUA_FILE}")
endif ()
if (NOT QMUD_SYSTEM_SOCKET_DIR_MIME_LUA_FILE)
    set(QMUD_SYSTEM_SOCKET_DIR_MIME_LUA_FILE "${QMUD_SYSTEM_MIME_LUA_FILE}")
endif ()
if (NOT QMUD_SYSTEM_SOCKET_CORE_SO)
    find_file(QMUD_SYSTEM_SOCKET_CORE_SO
            NAMES ${QMUD_LUA_MODULE_CORE_NAMES}
            PATHS ${QMUD_LUA_MODULE_SEARCH_PATHS}
            PATH_SUFFIXES "" socket lua/5.4/socket lua/5.3/socket lua/5.2/socket lua/5.1/socket
            NO_CMAKE_FIND_ROOT_PATH
    )
endif ()
if (NOT QMUD_SYSTEM_MIME_CORE_SO)
    find_file(QMUD_SYSTEM_MIME_CORE_SO
            NAMES ${QMUD_LUA_MODULE_CORE_NAMES}
            PATHS ${QMUD_LUA_MODULE_SEARCH_PATHS}
            PATH_SUFFIXES "" mime lua/5.4/mime lua/5.3/mime lua/5.2/mime lua/5.1/mime
            NO_CMAKE_FIND_ROOT_PATH
    )
endif ()
if (EXISTS "${SKELETON_SRC}")
    file(GLOB_RECURSE SKELETON_FILES CONFIGURE_DEPENDS
            "${SKELETON_SRC}/*")
    file(GLOB_RECURSE SKELETON_PATHS LIST_DIRECTORIES true CONFIGURE_DEPENDS
            "${SKELETON_SRC}/*")
    set(SKELETON_DIRS)
    foreach (_skeleton_path IN LISTS SKELETON_PATHS)
        if (IS_DIRECTORY "${_skeleton_path}")
            list(APPEND SKELETON_DIRS "${_skeleton_path}")
        endif ()
    endforeach ()
    set(QMUD_SYSTEM_TOPLEVEL_LUA_FILES)
    foreach (_qmud_system_lua_file
            "${QMUD_SYSTEM_JSON_TOP_LUA_FILE}"
            "${QMUD_SYSTEM_RE_LUA_FILE}")
        if (_qmud_system_lua_file)
            list(APPEND QMUD_SYSTEM_TOPLEVEL_LUA_FILES "${_qmud_system_lua_file}")
        endif ()
    endforeach ()
    set(QMUD_SYSTEM_SOCKET_TOPLEVEL_LUA_FILES)
    foreach (_qmud_socket_lua_file
            "${QMUD_SYSTEM_SOCKET_DIR_SOCKET_LUA_FILE}"
            "${QMUD_SYSTEM_SOCKET_DIR_LTN12_LUA_FILE}"
            "${QMUD_SYSTEM_SOCKET_DIR_MIME_LUA_FILE}")
        if (_qmud_socket_lua_file)
            list(APPEND QMUD_SYSTEM_SOCKET_TOPLEVEL_LUA_FILES "${_qmud_socket_lua_file}")
        endif ()
    endforeach ()
    set(QMUD_SYSTEM_SOCKET_MODULE_LUA_FILES)
    foreach (_qmud_socket_lua_file
            "${QMUD_SYSTEM_SOCKET_DIR_FTP_LUA_FILE}"
            "${QMUD_SYSTEM_SOCKET_DIR_HTTP_LUA_FILE}"
            "${QMUD_SYSTEM_SOCKET_DIR_SMTP_LUA_FILE}"
            "${QMUD_SYSTEM_SOCKET_DIR_TP_LUA_FILE}"
            "${QMUD_SYSTEM_SOCKET_DIR_URL_LUA_FILE}")
        if (_qmud_socket_lua_file)
            list(APPEND QMUD_SYSTEM_SOCKET_MODULE_LUA_FILES "${_qmud_socket_lua_file}")
        endif ()
    endforeach ()
    if (QMUD_ENABLE_LUA_SCRIPTING)
        if (NOT QMUD_SYSTEM_SOCKET_LUA_DIR)
            message(FATAL_ERROR "System LuaSocket socket/ module directory not found.")
        endif ()
        if (NOT QMUD_SYSTEM_SOCKET_DIR_SOCKET_LUA_FILE)
            message(FATAL_ERROR "System LuaSocket socket.lua not found.")
        endif ()
        if (NOT QMUD_SYSTEM_SOCKET_DIR_LTN12_LUA_FILE)
            message(FATAL_ERROR "System LuaSocket ltn12.lua not found.")
        endif ()
        if (NOT QMUD_SYSTEM_SOCKET_DIR_MIME_LUA_FILE)
            message(FATAL_ERROR "System LuaSocket mime.lua not found.")
        endif ()
        if (NOT QMUD_SYSTEM_SOCKET_DIR_FTP_LUA_FILE)
            message(FATAL_ERROR "System LuaSocket socket/ftp.lua not found.")
        endif ()
        if (NOT QMUD_SYSTEM_SOCKET_DIR_HTTP_LUA_FILE)
            message(FATAL_ERROR "System LuaSocket socket/http.lua not found.")
        endif ()
        if (NOT QMUD_SYSTEM_SOCKET_DIR_SMTP_LUA_FILE)
            message(FATAL_ERROR "System LuaSocket socket/smtp.lua not found.")
        endif ()
        if (NOT QMUD_SYSTEM_SOCKET_DIR_TP_LUA_FILE)
            message(FATAL_ERROR "System LuaSocket socket/tp.lua not found.")
        endif ()
        if (NOT QMUD_SYSTEM_SOCKET_DIR_URL_LUA_FILE)
            message(FATAL_ERROR "System LuaSocket socket/url.lua not found.")
        endif ()
        if (NOT QMUD_SYSTEM_JSON_TOP_LUA_FILE)
            message(FATAL_ERROR "System json.lua not found.")
        endif ()
        if (NOT QMUD_SYSTEM_RE_LUA_FILE)
            message(FATAL_ERROR "System re.lua not found.")
        endif ()
    endif ()
    if (QMUD_SYSTEM_JSON_LUA_DIR)
        file(GLOB_RECURSE QMUD_SYSTEM_JSON_LUA_FILES CONFIGURE_DEPENDS
                "${QMUD_SYSTEM_JSON_LUA_DIR}/*.lua")
    else ()
        set(QMUD_SYSTEM_JSON_LUA_FILES)
        if (QMUD_ENABLE_LUA_SCRIPTING)
            message(FATAL_ERROR "System Lua JSON module directory not found.")
        endif ()
    endif ()
    set(SKELETON_STAMP "${SKELETON_DST}/.skeleton.stamp")
    add_custom_command(
            OUTPUT "${SKELETON_STAMP}"
            COMMENT "Syncing skeleton/ to build dir"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SKELETON_DST}"
            DEPENDS
            ${SKELETON_FILES}
            ${SKELETON_DIRS}
            ${QMUD_SYSTEM_TOPLEVEL_LUA_FILES}
            ${QMUD_SYSTEM_JSON_LUA_FILES}
            ${QMUD_SYSTEM_SOCKET_TOPLEVEL_LUA_FILES}
            ${QMUD_SYSTEM_SOCKET_MODULE_LUA_FILES}
            ${QMUD_SYSTEM_SOCKET_CORE_SO}
            ${QMUD_SYSTEM_MIME_CORE_SO}
    )
    foreach (d IN LISTS SKELETON_DIRS)
        file(RELATIVE_PATH rel_dir "${SKELETON_SRC}" "${d}")
        if (NOT rel_dir STREQUAL "")
            add_custom_command(
                    OUTPUT "${SKELETON_STAMP}"
                    APPEND
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<SHELL_PATH:${SKELETON_DST}/${rel_dir}>"
            )
        endif ()
    endforeach ()
    foreach (f IN LISTS SKELETON_FILES)
        file(RELATIVE_PATH rel "${SKELETON_SRC}" "${f}")
        get_filename_component(rel_dir "${rel}" DIRECTORY)
        if (rel_dir STREQUAL "")
            set(dest_dir "${SKELETON_DST}")
        else ()
            set(dest_dir "${SKELETON_DST}/${rel_dir}")
        endif ()
        add_custom_command(
                OUTPUT "${SKELETON_STAMP}"
                APPEND
                COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<SHELL_PATH:${dest_dir}>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<SHELL_PATH:${f}>"
                "$<SHELL_PATH:${SKELETON_DST}/${rel}>"
        )
    endforeach ()
    foreach (f IN LISTS QMUD_SYSTEM_TOPLEVEL_LUA_FILES)
        get_filename_component(name "${f}" NAME)
        add_custom_command(
                OUTPUT "${SKELETON_STAMP}"
                APPEND
                COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<SHELL_PATH:${SKELETON_DST}/lua>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<SHELL_PATH:${f}>"
                "$<SHELL_PATH:${SKELETON_DST}/lua/${name}>"
        )
    endforeach ()
    foreach (f IN LISTS QMUD_SYSTEM_JSON_LUA_FILES)
        file(RELATIVE_PATH rel "${QMUD_SYSTEM_JSON_LUA_DIR}" "${f}")
        get_filename_component(rel_dir "${rel}" DIRECTORY)
        if (rel_dir STREQUAL "")
            set(dest_dir "${SKELETON_DST}/lua/json")
        else ()
            set(dest_dir "${SKELETON_DST}/lua/json/${rel_dir}")
        endif ()
        add_custom_command(
                OUTPUT "${SKELETON_STAMP}"
                APPEND
                COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<SHELL_PATH:${dest_dir}>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<SHELL_PATH:${f}>"
                "$<SHELL_PATH:${SKELETON_DST}/lua/json/${rel}>"
        )
    endforeach ()
    foreach (f IN LISTS QMUD_SYSTEM_SOCKET_TOPLEVEL_LUA_FILES)
        get_filename_component(name "${f}" NAME)
        add_custom_command(
                OUTPUT "${SKELETON_STAMP}"
                APPEND
                COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<SHELL_PATH:${SKELETON_DST}/lua>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<SHELL_PATH:${f}>"
                "$<SHELL_PATH:${SKELETON_DST}/lua/${name}>"
        )
    endforeach ()
    foreach (f IN LISTS QMUD_SYSTEM_SOCKET_MODULE_LUA_FILES)
        get_filename_component(name "${f}" NAME)
        add_custom_command(
                OUTPUT "${SKELETON_STAMP}"
                APPEND
                COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<SHELL_PATH:${SKELETON_DST}/socket>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<SHELL_PATH:${f}>"
                "$<SHELL_PATH:${SKELETON_DST}/socket/${name}>"
        )
    endforeach ()
    if (QMUD_SYSTEM_SOCKET_CORE_SO)
        add_custom_command(
                OUTPUT "${SKELETON_STAMP}"
                APPEND
                COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<SHELL_PATH:${SKELETON_DST}/socket>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<SHELL_PATH:${QMUD_SYSTEM_SOCKET_CORE_SO}>"
                "$<SHELL_PATH:${SKELETON_DST}/socket/${QMUD_LUA_MODULE_CORE_OUTPUT_NAME}>"
        )
    endif ()
    if (QMUD_SYSTEM_MIME_CORE_SO)
        add_custom_command(
                OUTPUT "${SKELETON_STAMP}"
                APPEND
                COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<SHELL_PATH:${SKELETON_DST}/mime>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<SHELL_PATH:${QMUD_SYSTEM_MIME_CORE_SO}>"
                "$<SHELL_PATH:${SKELETON_DST}/mime/${QMUD_LUA_MODULE_CORE_OUTPUT_NAME}>"
        )
    endif ()
    add_custom_command(
            OUTPUT "${SKELETON_STAMP}"
            APPEND
            COMMAND ${CMAKE_COMMAND} -E touch "${SKELETON_STAMP}"
    )
    add_custom_target(sync_skeleton DEPENDS "${SKELETON_STAMP}")
    add_dependencies(QMud sync_skeleton)
endif ()

set(QMUD_API_EXPORT_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/skeleton/docs/lua_api_inventory")
set(QMUD_API_EXPORT_COMMAND "$<TARGET_FILE:QMud>" --dump-lua-api "${QMUD_API_EXPORT_OUTPUT_DIR}")
if (UNIX AND NOT APPLE)
    set(QMUD_API_EXPORT_COMMAND
            ${CMAKE_COMMAND} -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:QMud>" --dump-lua-api "${QMUD_API_EXPORT_OUTPUT_DIR}")
endif ()
add_custom_target(ApiExport
        DEPENDS QMud
        COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_API_EXPORT_OUTPUT_DIR}"
        COMMAND ${QMUD_API_EXPORT_COMMAND}
        COMMENT "Exporting QMud Lua API inventory into skeleton/docs/lua_api_inventory"
        USES_TERMINAL
)


# Helpful compile definitions for portability.
# They are safe on Linux and useful to avoid common Windows macros bleeding into code.
string(TIMESTAMP QMUD_BUILD_DATE_UTC "%Y-%m-%d" UTC)
string(TIMESTAMP QMUD_BUILD_TIME_UTC "%H:%M:%S" UTC)
target_compile_definitions(QMud PRIVATE
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        QMUD_BUILD_DATE_UTC="${QMUD_BUILD_DATE_UTC}"
        QMUD_BUILD_TIME_UTC="${QMUD_BUILD_TIME_UTC}"
)
if (QMUD_CI_BUILD)
    target_compile_definitions(QMud PRIVATE CI_BUILD)
endif ()

# Recommended: enable warnings and reasonable flags (adjust per taste)
if (MSVC)
    target_compile_options(QMud PRIVATE /W4 /permissive-)
else ()
    target_compile_options(QMud PRIVATE -Wall -Wextra -Wpedantic)
endif ()

qmud_apply_release_optimizations(QMud)
if (APPLE AND CMAKE_CROSSCOMPILING)
    # Suppress noisy osxcross/tbd linker warnings (e.g. libz/libcups out-of-sync stubs).
    target_link_options(QMud PRIVATE -Wl,-w)
endif ()

if (APPLE)
    option(QMUD_ENABLE_MACAPP "Enable macOS .app packaging target" ON)
    if (QMUD_ENABLE_MACAPP)
        set(QMUD_MACAPP_VERSION "dev" CACHE STRING "Version string embedded in QMud.app metadata")
        set(QMUD_MACAPP_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/macapp-out" CACHE PATH "Output directory for generated macOS app bundle")
        set(QMUD_MACDEPLOYQT_EXECUTABLE "" CACHE FILEPATH "Path to macdeployqt executable")
        set(QMUD_MACAPP_ICON_ICNS "" CACHE FILEPATH "Optional explicit .icns file for the macOS app icon")

        if (NOT QMUD_MACDEPLOYQT_EXECUTABLE)
            if (DEFINED Qt6_DIR)
                get_filename_component(QMUD_QT6_CMAKE_DIR "${Qt6_DIR}" ABSOLUTE)
                get_filename_component(QMUD_QT6_ROOT "${QMUD_QT6_CMAKE_DIR}/../../.." ABSOLUTE)
                find_program(QMUD_MACDEPLOYQT_EXECUTABLE
                        NAMES macdeployqt
                        HINTS "${QMUD_QT6_ROOT}/bin")
            endif ()
        endif ()
        if (NOT QMUD_MACDEPLOYQT_EXECUTABLE)
            find_program(QMUD_MACDEPLOYQT_EXECUTABLE NAMES macdeployqt)
        endif ()

        set(QMUD_MACAPP_BUNDLE_VERSION "${QMUD_MACAPP_VERSION}")
        if (DEFINED PROJECT_VERSION AND NOT PROJECT_VERSION STREQUAL "")
            set(QMUD_MACAPP_BUNDLE_VERSION "${PROJECT_VERSION}")
        endif ()

        set(QMUD_MACAPP_ICON_SOURCE "${QMUD_MACAPP_ICON_ICNS}")
        if (NOT QMUD_MACAPP_ICON_SOURCE)
            set(QMUD_MACAPP_ICON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/resources/qmud/res/QMud.icns")
        endif ()

        set_target_properties(QMud PROPERTIES
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_BUNDLE_NAME "QMud"
                MACOSX_BUNDLE_GUI_IDENTIFIER "com.abnormalfrequency.qmud"
                MACOSX_BUNDLE_BUNDLE_VERSION "${QMUD_MACAPP_BUNDLE_VERSION}"
                MACOSX_BUNDLE_SHORT_VERSION_STRING "${QMUD_MACAPP_BUNDLE_VERSION}"
                MACOSX_RPATH TRUE
                BUILD_WITH_INSTALL_RPATH TRUE
                INSTALL_RPATH "@executable_path/../Frameworks;@loader_path/../Frameworks"
                BUILD_RPATH "@executable_path/../Frameworks;@loader_path/../Frameworks"
                INSTALL_RPATH_USE_LINK_PATH FALSE
        )

        if (EXISTS "${QMUD_MACAPP_ICON_SOURCE}")
            get_filename_component(QMUD_MACAPP_ICON_NAME "${QMUD_MACAPP_ICON_SOURCE}" NAME)
            set_source_files_properties("${QMUD_MACAPP_ICON_SOURCE}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
            target_sources(QMud PRIVATE "${QMUD_MACAPP_ICON_SOURCE}")
            set_target_properties(QMud PROPERTIES MACOSX_BUNDLE_ICON_FILE "${QMUD_MACAPP_ICON_NAME}")
        endif ()

        set(QMUD_MACAPP_BUNDLE_DIR "$<TARGET_BUNDLE_DIR:QMud>")
        set(QMUD_MACAPP_BUNDLE_RESOURCES_DIR "${QMUD_MACAPP_BUNDLE_DIR}/Contents/Resources")
        set(QMUD_MACAPP_STAGE_DIR "${QMUD_MACAPP_OUTPUT_DIR}/QMud.app")
        option(QMUD_MACAPP_FORCE_NO_MACDEPLOYQT "Force MacApp target to skip macdeployqt even when found" OFF)
        option(QMUD_MACAPP_ALLOW_NO_MACDEPLOYQT "Allow MacApp target without macdeployqt (dependencies must be staged manually)" OFF)
        option(QMUD_MACAPP_MACDEPLOYQT_NO_PLUGINS "Pass -no-plugins to macdeployqt (plugins must be staged manually)" OFF)
        set(QMUD_MACAPP_MACDEPLOYQT_LIBPATHS "" CACHE STRING "Semicolon-separated extra library search paths passed to macdeployqt via -libpath")
        set(QMUD_MACDEPLOYQT_ARGS -always-overwrite)
        if (QMUD_MACAPP_MACDEPLOYQT_NO_PLUGINS)
            list(APPEND QMUD_MACDEPLOYQT_ARGS -no-plugins)
        endif ()
        foreach (_qmud_mac_libpath IN LISTS QMUD_MACAPP_MACDEPLOYQT_LIBPATHS)
            if (NOT _qmud_mac_libpath STREQUAL "")
                list(APPEND QMUD_MACDEPLOYQT_ARGS "-libpath=${_qmud_mac_libpath}")
            endif ()
        endforeach ()
        set(QMUD_MACAPP_STAGEABLE OFF)

        if (QMUD_MACAPP_FORCE_NO_MACDEPLOYQT)
            set(QMUD_MACDEPLOYQT_EXECUTABLE "")
        endif ()

        if (QMUD_MACDEPLOYQT_EXECUTABLE AND NOT QMUD_MACAPP_FORCE_NO_MACDEPLOYQT)
            add_custom_target(MacApp
                    DEPENDS QMud
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_MACAPP_OUTPUT_DIR}"
                    COMMAND "${QMUD_MACDEPLOYQT_EXECUTABLE}" "${QMUD_MACAPP_BUNDLE_DIR}" ${QMUD_MACDEPLOYQT_ARGS}
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${QMUD_MACAPP_STAGE_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory "${QMUD_MACAPP_BUNDLE_DIR}" "${QMUD_MACAPP_STAGE_DIR}"
                    COMMENT "Building QMud.app bundle with macdeployqt"
                    USES_TERMINAL
            )
            set(QMUD_MACAPP_STAGEABLE ON)
        elseif (QMUD_MACAPP_ALLOW_NO_MACDEPLOYQT OR QMUD_MACAPP_FORCE_NO_MACDEPLOYQT)
            add_custom_target(MacApp
                    DEPENDS QMud
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_MACAPP_OUTPUT_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${QMUD_MACAPP_STAGE_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory "${QMUD_MACAPP_BUNDLE_DIR}" "${QMUD_MACAPP_STAGE_DIR}"
                    COMMENT "Building QMud.app bundle without macdeployqt (manual dependency staging expected)"
                    USES_TERMINAL
            )
            set(QMUD_MACAPP_STAGEABLE ON)
        else ()
            add_custom_target(MacApp
                    COMMAND ${CMAKE_COMMAND} -E echo "Mac app packaging requires macdeployqt."
                    COMMAND ${CMAKE_COMMAND} -E echo "Set QMUD_MACDEPLOYQT_EXECUTABLE or ensure macdeployqt is on PATH."
                    COMMAND ${CMAKE_COMMAND} -E false
            )
        endif ()

        if (QMUD_MACAPP_STAGEABLE AND EXISTS "${SKELETON_SRC}")
            add_custom_command(TARGET MacApp POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_MACAPP_STAGE_DIR}/Contents/MacOS"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "$<SHELL_PATH:${SKELETON_SRC}>"
                    "$<SHELL_PATH:${QMUD_MACAPP_STAGE_DIR}/Contents/MacOS>"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "$<SHELL_PATH:${CMAKE_CURRENT_BINARY_DIR}/lua>"
                    "$<SHELL_PATH:${QMUD_MACAPP_STAGE_DIR}/Contents/MacOS/lua>"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "$<SHELL_PATH:${CMAKE_CURRENT_BINARY_DIR}/socket>"
                    "$<SHELL_PATH:${QMUD_MACAPP_STAGE_DIR}/Contents/MacOS/socket>"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "$<SHELL_PATH:${CMAKE_CURRENT_BINARY_DIR}/mime>"
                    "$<SHELL_PATH:${QMUD_MACAPP_STAGE_DIR}/Contents/MacOS/mime>"
                    COMMENT "Copying skeleton into QMud.app resources"
            )
        endif ()
    endif ()
endif ()

if (UNIX AND NOT APPLE)
    set(QMUD_DOCKER_EXECUTABLE "docker" CACHE FILEPATH
            "Path to docker-compatible executable (docker/podman)")

    if (DEFINED QMUD_ENABLE_MACAPP_DOCKER_NEW AND NOT DEFINED QMUD_ENABLE_MAC_DOCKER)
        set(QMUD_ENABLE_MAC_DOCKER "${QMUD_ENABLE_MACAPP_DOCKER_NEW}" CACHE BOOL
                "Enable MacDocker target using a prebuilt custom macOS builder image" FORCE)
    endif ()
    unset(QMUD_ENABLE_MACAPP_DOCKER_NEW CACHE)
    option(QMUD_ENABLE_MAC_DOCKER "Enable MacDocker target using a prebuilt custom macOS builder image" ON)
    if (QMUD_ENABLE_MAC_DOCKER)
        if (DEFINED QMUD_MACAPP_DOCKER_NEW_IMAGE)
            if (NOT DEFINED QMUD_MAC_DOCKER_IMAGE)
                set(QMUD_MAC_DOCKER_IMAGE "${QMUD_MACAPP_DOCKER_NEW_IMAGE}" CACHE STRING
                        "Docker image for MacDocker (build from tools/docker/macos-qt610/Dockerfile)" FORCE)
            endif ()
            unset(QMUD_MACAPP_DOCKER_NEW_IMAGE CACHE)
        endif ()
        if (DEFINED QMUD_MACAPP_DOCKER_NEW_BUILD_DIR)
            if (NOT DEFINED QMUD_MAC_DOCKER_BUILD_DIR)
                set(QMUD_MAC_DOCKER_BUILD_DIR "${QMUD_MACAPP_DOCKER_NEW_BUILD_DIR}" CACHE PATH
                        "Host build directory mounted into container for MacDocker" FORCE)
            endif ()
            unset(QMUD_MACAPP_DOCKER_NEW_BUILD_DIR CACHE)
        endif ()
        if (DEFINED QMUD_MACAPP_DOCKER_NEW_OUTPUT_DIR)
            if (NOT DEFINED QMUD_MAC_DOCKER_OUTPUT_DIR)
                set(QMUD_MAC_DOCKER_OUTPUT_DIR "${QMUD_MACAPP_DOCKER_NEW_OUTPUT_DIR}" CACHE PATH
                        "Host output directory for MacDocker artifact" FORCE)
            endif ()
            unset(QMUD_MACAPP_DOCKER_NEW_OUTPUT_DIR CACHE)
        endif ()
        if (DEFINED QMUD_MACAPP_DOCKER_NEW_ARCH)
            if (NOT DEFINED QMUD_MAC_DOCKER_ARCH)
                set(QMUD_MAC_DOCKER_ARCH "${QMUD_MACAPP_DOCKER_NEW_ARCH}" CACHE STRING
                        "Target macOS architecture for MacDocker (x86_64 or aarch64)" FORCE)
            endif ()
            unset(QMUD_MACAPP_DOCKER_NEW_ARCH CACHE)
        endif ()
        if (DEFINED QMUD_MACAPP_DOCKER_NEW_BUILD_TYPE)
            if (NOT DEFINED QMUD_MAC_DOCKER_BUILD_TYPE)
                set(QMUD_MAC_DOCKER_BUILD_TYPE "${QMUD_MACAPP_DOCKER_NEW_BUILD_TYPE}" CACHE STRING
                        "CMake build type used inside MacDocker container" FORCE)
            endif ()
            unset(QMUD_MACAPP_DOCKER_NEW_BUILD_TYPE CACHE)
        endif ()

        set(QMUD_MAC_DOCKER_IMAGE "qmud-macos-builder:qt6.10" CACHE STRING
                "Docker image for MacDocker (build from tools/docker/macos-qt610/Dockerfile)")
        set(QMUD_MAC_DOCKER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/mac-docker-build" CACHE PATH
                "Host build directory mounted into container for MacDocker")
        set(QMUD_MAC_DOCKER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/mac-docker-out" CACHE PATH
                "Host output directory for MacDocker artifact")
        # Migrate legacy defaults from prior target naming (macos-docker-new-*).
        if (QMUD_MAC_DOCKER_BUILD_DIR STREQUAL "${CMAKE_CURRENT_BINARY_DIR}/macos-docker-new-build")
            set(QMUD_MAC_DOCKER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/mac-docker-build" CACHE PATH
                    "Host build directory mounted into container for MacDocker" FORCE)
        endif ()
        if (QMUD_MAC_DOCKER_OUTPUT_DIR STREQUAL "${CMAKE_CURRENT_BINARY_DIR}/macos-docker-new-out")
            set(QMUD_MAC_DOCKER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/mac-docker-out" CACHE PATH
                    "Host output directory for MacDocker artifact" FORCE)
        endif ()
        set(QMUD_MAC_DOCKER_BUILD_TYPE "Release" CACHE STRING
                "CMake build type used inside MacDocker container")
        set(QMUD_MAC_DOCKER_ARCH "x86_64" CACHE STRING
                "Target macOS architecture for MacDocker (x86_64 or aarch64)")
        set_property(CACHE QMUD_MAC_DOCKER_ARCH PROPERTY STRINGS x86_64 aarch64)
        set(QMUD_MAC_DOCKER_OSX_DEPLOYMENT_TARGET "13.0.0" CACHE STRING
                "CMAKE_OSX_DEPLOYMENT_TARGET used inside MacDocker container")
        set(QMUD_MAC_DOCKER_QT_PREFIX "/opt/Qt/6.10.2/macos" CACHE PATH
                "Qt macOS prefix inside MacDocker image")
        set(QMUD_MAC_DOCKER_QT_HOST_PREFIX "/usr/lib64/qt6" CACHE PATH
                "Qt host-tools prefix inside MacDocker image (must provide runnable moc/uic/rcc)")
        set(QMUD_MAC_DOCKER_LUA_PREFIX "/opt/qmud/macos-deps/lua" CACHE PATH
                "Lua prefix inside MacDocker image")
        set(QMUD_MAC_DOCKER_LUA_MODULES_PREFIX "/opt/qmud/macos-deps/lua-modules" CACHE PATH
                "Lua module prefix inside MacDocker image")
        option(QMUD_MAC_DOCKER_DISABLE_SELINUX
                "Disable SELinux labeling for MacDocker container runs (--security-opt label=disable)" ON)
        option(QMUD_MAC_DOCKER_RUN_AS_HOST_USER
                "Run MacDocker container with host UID/GID to avoid root-owned output files" ON)

        set(QMUD_MAC_DOCKER_HOST_UID "$ENV{UID}" CACHE STRING "Host UID for MacDocker --user")
        set(QMUD_MAC_DOCKER_HOST_GID "" CACHE STRING "Host GID for MacDocker --user")
        if (QMUD_MAC_DOCKER_HOST_UID STREQUAL "")
            execute_process(COMMAND id -u
                    OUTPUT_VARIABLE QMUD_MAC_DOCKER_HOST_UID
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
            if (QMUD_MAC_DOCKER_HOST_UID STREQUAL "")
                set(QMUD_MAC_DOCKER_HOST_UID "1000")
            endif ()
            set(QMUD_MAC_DOCKER_HOST_UID "${QMUD_MAC_DOCKER_HOST_UID}" CACHE STRING
                    "Host UID for MacDocker --user" FORCE)
        endif ()
        if (QMUD_MAC_DOCKER_HOST_GID STREQUAL "")
            execute_process(COMMAND id -g
                    OUTPUT_VARIABLE QMUD_MAC_DOCKER_HOST_GID
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
            if (QMUD_MAC_DOCKER_HOST_GID STREQUAL "")
                set(QMUD_MAC_DOCKER_HOST_GID "1000")
            endif ()
            set(QMUD_MAC_DOCKER_HOST_GID "${QMUD_MAC_DOCKER_HOST_GID}" CACHE STRING
                    "Host GID for MacDocker --user" FORCE)
        endif ()

        if (QMUD_MAC_DOCKER_DISABLE_SELINUX)
            set(QMUD_MAC_DOCKER_SECURITY_OPT --security-opt label=disable)
        else ()
            set(QMUD_MAC_DOCKER_SECURITY_OPT)
        endif ()

        if (QMUD_MAC_DOCKER_RUN_AS_HOST_USER)
            set(QMUD_MAC_DOCKER_USER_ARGS --user ${QMUD_MAC_DOCKER_HOST_UID}:${QMUD_MAC_DOCKER_HOST_GID})
        else ()
            set(QMUD_MAC_DOCKER_USER_ARGS)
        endif ()

        if (NOT DEFINED QMUD_DOCKER_EXECUTABLE OR QMUD_DOCKER_EXECUTABLE STREQUAL "")
            set(QMUD_DOCKER_EXECUTABLE "docker")
        endif ()

        set(QMUD_MAC_DOCKER_SCRIPT "${CMAKE_SOURCE_DIR}/tools/scripts/mac_docker_build.sh")

        find_program(QMUD_DOCKER_EXECUTABLE_FOUND_NEW
                NAMES ${QMUD_DOCKER_EXECUTABLE}
                NO_CACHE)
        set(QMUD_MAC_DOCKER_USERNS_ARGS)
        if (QMUD_DOCKER_EXECUTABLE_FOUND_NEW AND QMUD_MAC_DOCKER_RUN_AS_HOST_USER)
            execute_process(
                    COMMAND ${QMUD_DOCKER_EXECUTABLE_FOUND_NEW} --version
                    OUTPUT_VARIABLE QMUD_MAC_DOCKER_VERSION_OUTPUT
                    ERROR_VARIABLE QMUD_MAC_DOCKER_VERSION_ERROR
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_STRIP_TRAILING_WHITESPACE
            )
            string(TOLOWER "${QMUD_MAC_DOCKER_VERSION_OUTPUT}\n${QMUD_MAC_DOCKER_VERSION_ERROR}" QMUD_MAC_DOCKER_VERSION_TEXT)
            if (QMUD_MAC_DOCKER_VERSION_TEXT MATCHES "podman")
                set(QMUD_MAC_DOCKER_USERNS_ARGS --userns keep-id)
                message(STATUS "MacDocker: detected podman backend, enabling --userns keep-id for host UID/GID readability")
            endif ()
        endif ()

        if (QMUD_DOCKER_EXECUTABLE_FOUND_NEW)
            add_custom_target(MacDocker
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_MAC_DOCKER_BUILD_DIR}"
                    COMMAND /bin/chmod 0777 "${QMUD_MAC_DOCKER_BUILD_DIR}"
                    COMMAND ${QMUD_DOCKER_EXECUTABLE_FOUND_NEW} run --rm
                    ${QMUD_MAC_DOCKER_SECURITY_OPT}
                    ${QMUD_MAC_DOCKER_USERNS_ARGS}
                    ${QMUD_MAC_DOCKER_USER_ARGS}
                    -e BUILD_ARCH=${QMUD_MAC_DOCKER_ARCH}
                    -e CCACHE_DISABLE=1
                    -e QMUD_MAC_DOCKER_IMAGE=${QMUD_MAC_DOCKER_IMAGE}
                    -e QMUD_MAC_DOCKER_BUILD_TYPE=${QMUD_MAC_DOCKER_BUILD_TYPE}
                    -e QMUD_MAC_DOCKER_OSX_DEPLOYMENT_TARGET=${QMUD_MAC_DOCKER_OSX_DEPLOYMENT_TARGET}
                    -e QMUD_MAC_DOCKER_QT_PREFIX=${QMUD_MAC_DOCKER_QT_PREFIX}
                    -e QMUD_MAC_DOCKER_QT_HOST_PREFIX=${QMUD_MAC_DOCKER_QT_HOST_PREFIX}
                    -e QMUD_MAC_DOCKER_LUA_PREFIX=${QMUD_MAC_DOCKER_LUA_PREFIX}
                    -e QMUD_MAC_DOCKER_LUA_MODULES_PREFIX=${QMUD_MAC_DOCKER_LUA_MODULES_PREFIX}
                    -v "${CMAKE_SOURCE_DIR}:/home/user/project:ro"
                    -v "${QMUD_MAC_DOCKER_BUILD_DIR}:/home/user/build:rw"
                    "${QMUD_MAC_DOCKER_IMAGE}"
                    sh /home/user/project/tools/scripts/mac_docker_build.sh
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${QMUD_MAC_DOCKER_OUTPUT_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_MAC_DOCKER_OUTPUT_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${QMUD_MAC_DOCKER_BUILD_DIR}/macapp-out"
                    "${QMUD_MAC_DOCKER_OUTPUT_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E rm -f "${QMUD_MAC_DOCKER_OUTPUT_DIR}/QMud.app.zip"
                    COMMAND ${CMAKE_COMMAND} -E chdir "${QMUD_MAC_DOCKER_OUTPUT_DIR}"
                    ${CMAKE_COMMAND} -E tar cf "QMud.app.zip" --format=zip "QMud.app"
                    COMMENT "Cross-building macOS QMud.app via ${QMUD_MAC_DOCKER_IMAGE} (MacDocker)"
                    USES_TERMINAL
            )
        else ()
            add_custom_target(MacDocker
                    COMMAND ${CMAKE_COMMAND} -E echo "MacDocker requires a docker-compatible executable."
                    COMMAND ${CMAKE_COMMAND} -E echo "Set QMUD_DOCKER_EXECUTABLE to docker/podman and ensure it is available."
                    COMMAND ${CMAKE_COMMAND} -E false
            )
        endif ()
    endif ()

    option(QMUD_ENABLE_WINDOCKER "Enable WinDocker target using a prebuilt custom Windows builder image" ON)
    if (QMUD_ENABLE_WINDOCKER)
        set(QMUD_WINDOCKER_IMAGE "qmud-windows-builder:qt6.10" CACHE STRING
                "Docker image for WinDocker (build from tools/docker/windows-qt610/Dockerfile)")
        set(QMUD_WINDOCKER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/windows-docker-build" CACHE PATH
                "Host build directory mounted into container for WinDocker")
        set(QMUD_WINDOCKER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/windows-docker-out" CACHE PATH
                "Host output directory for WinDocker artifacts")
        set(QMUD_WINDOCKER_BUILD_TYPE "Release" CACHE STRING
                "CMake build type used inside WinDocker container")
        set(QMUD_WINDOCKER_QT_PREFIX "/opt/Qt/6.10.2/mingw_64" CACHE PATH
                "Qt Windows prefix inside WinDocker image")
        set(QMUD_WINDOCKER_QT_HOST_PREFIX "/usr/lib64/qt6" CACHE PATH
                "Qt host-tools prefix inside WinDocker image (must provide runnable moc/uic/rcc)")
        set(QMUD_WINDOCKER_MINGW_PREFIX "/usr/x86_64-w64-mingw32/sys-root/mingw" CACHE PATH
                "MinGW runtime prefix inside WinDocker image")
        set(QMUD_WINDOCKER_MINGW_TRIPLET "x86_64-w64-mingw32" CACHE STRING
                "MinGW compiler triplet inside WinDocker image")
        set(QMUD_WINDOCKER_LUA_PREFIX "/opt/qmud/windows-deps/lua" CACHE PATH
                "Lua prefix inside WinDocker image")
        set(QMUD_WINDOCKER_LUA_MODULES_PREFIX "/opt/qmud/windows-deps/lua-modules" CACHE PATH
                "Lua module prefix inside WinDocker image")
        option(QMUD_WINDOCKER_DISABLE_SELINUX
                "Disable SELinux labeling for WinDocker container runs (--security-opt label=disable)" ON)
        option(QMUD_WINDOCKER_RUN_AS_HOST_USER
                "Run WinDocker container with host UID/GID to avoid root-owned output files" ON)
        set(QMUD_WINDOCKER_HOST_UID "$ENV{UID}" CACHE STRING "Host UID for WinDocker --user")
        set(QMUD_WINDOCKER_HOST_GID "" CACHE STRING "Host GID for WinDocker --user")

        if (QMUD_WINDOCKER_DISABLE_SELINUX)
            set(QMUD_WINDOCKER_SECURITY_OPT --security-opt label=disable)
        else ()
            set(QMUD_WINDOCKER_SECURITY_OPT)
        endif ()

        if (QMUD_WINDOCKER_RUN_AS_HOST_USER)
            if (NOT QMUD_WINDOCKER_HOST_UID)
                execute_process(
                        COMMAND id -u
                        OUTPUT_VARIABLE QMUD_WINDOCKER_HOST_UID
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
            endif ()
            if (NOT QMUD_WINDOCKER_HOST_GID)
                execute_process(
                        COMMAND id -g
                        OUTPUT_VARIABLE QMUD_WINDOCKER_HOST_GID
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
            endif ()
            if (NOT QMUD_WINDOCKER_HOST_UID)
                set(QMUD_WINDOCKER_HOST_UID 1000)
            endif ()
            if (NOT QMUD_WINDOCKER_HOST_GID)
                set(QMUD_WINDOCKER_HOST_GID 1000)
            endif ()
            set(QMUD_WINDOCKER_HOST_UID "${QMUD_WINDOCKER_HOST_UID}" CACHE STRING
                    "Host UID for WinDocker --user" FORCE)
            set(QMUD_WINDOCKER_HOST_GID "${QMUD_WINDOCKER_HOST_GID}" CACHE STRING
                    "Host GID for WinDocker --user" FORCE)
            set(QMUD_WINDOCKER_USER_ARGS --user ${QMUD_WINDOCKER_HOST_UID}:${QMUD_WINDOCKER_HOST_GID})
        else ()
            set(QMUD_WINDOCKER_USER_ARGS)
        endif ()

        if (NOT DEFINED QMUD_DOCKER_EXECUTABLE OR QMUD_DOCKER_EXECUTABLE STREQUAL "")
            set(QMUD_DOCKER_EXECUTABLE "docker")
        endif ()

        set(QMUD_WINDOCKER_SCRIPT "${CMAKE_SOURCE_DIR}/tools/scripts/win_docker_build.sh")

        find_program(QMUD_DOCKER_EXECUTABLE_FOUND_WINDOCKER
                NAMES ${QMUD_DOCKER_EXECUTABLE}
                NO_CACHE)
        set(QMUD_WINDOCKER_USERNS_ARGS)
        if (QMUD_DOCKER_EXECUTABLE_FOUND_WINDOCKER AND QMUD_WINDOCKER_RUN_AS_HOST_USER)
            execute_process(
                    COMMAND ${QMUD_DOCKER_EXECUTABLE_FOUND_WINDOCKER} --version
                    OUTPUT_VARIABLE QMUD_WINDOCKER_VERSION_OUTPUT
                    ERROR_VARIABLE QMUD_WINDOCKER_VERSION_ERROR
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_STRIP_TRAILING_WHITESPACE
            )
            string(TOLOWER "${QMUD_WINDOCKER_VERSION_OUTPUT}\n${QMUD_WINDOCKER_VERSION_ERROR}" QMUD_WINDOCKER_VERSION_TEXT)
            if (QMUD_WINDOCKER_VERSION_TEXT MATCHES "podman")
                set(QMUD_WINDOCKER_USERNS_ARGS --userns keep-id)
                message(STATUS "WinDocker: detected podman backend, enabling --userns keep-id for host UID/GID readability")
            endif ()
        endif ()

        if (QMUD_DOCKER_EXECUTABLE_FOUND_WINDOCKER)
            add_custom_target(WinDocker
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_WINDOCKER_BUILD_DIR}"
                    COMMAND /bin/chmod 0777 "${QMUD_WINDOCKER_BUILD_DIR}"
                    COMMAND ${QMUD_DOCKER_EXECUTABLE_FOUND_WINDOCKER} run --rm
                    ${QMUD_WINDOCKER_SECURITY_OPT}
                    ${QMUD_WINDOCKER_USERNS_ARGS}
                    ${QMUD_WINDOCKER_USER_ARGS}
                    -e CCACHE_DISABLE=1
                    -e QMUD_WINDOCKER_BUILD_TYPE=${QMUD_WINDOCKER_BUILD_TYPE}
                    -e QMUD_WINDOCKER_QT_PREFIX=${QMUD_WINDOCKER_QT_PREFIX}
                    -e QMUD_WINDOCKER_QT_HOST_PREFIX=${QMUD_WINDOCKER_QT_HOST_PREFIX}
                    -e QMUD_WINDOCKER_MINGW_PREFIX=${QMUD_WINDOCKER_MINGW_PREFIX}
                    -e QMUD_WINDOCKER_MINGW_TRIPLET=${QMUD_WINDOCKER_MINGW_TRIPLET}
                    -e QMUD_WINDOCKER_LUA_PREFIX=${QMUD_WINDOCKER_LUA_PREFIX}
                    -e QMUD_WINDOCKER_LUA_MODULES_PREFIX=${QMUD_WINDOCKER_LUA_MODULES_PREFIX}
                    -v "${CMAKE_SOURCE_DIR}:/home/user/project:ro"
                    -v "${QMUD_WINDOCKER_BUILD_DIR}:/home/user/build:rw"
                    "${QMUD_WINDOCKER_IMAGE}"
                    sh /home/user/project/tools/scripts/win_docker_build.sh
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${QMUD_WINDOCKER_OUTPUT_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_WINDOCKER_OUTPUT_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${QMUD_WINDOCKER_BUILD_DIR}/windocker-out"
                    "${QMUD_WINDOCKER_OUTPUT_DIR}"
                    COMMENT "Cross-building Windows QMud package via ${QMUD_WINDOCKER_IMAGE} (WinDocker)"
                    USES_TERMINAL
            )
        else ()
            add_custom_target(WinDocker
                    COMMAND ${CMAKE_COMMAND} -E echo "WinDocker requires a docker-compatible executable."
                    COMMAND ${CMAKE_COMMAND} -E echo "Set QMUD_DOCKER_EXECUTABLE to docker/podman and ensure it is available."
                    COMMAND ${CMAKE_COMMAND} -E false
            )
        endif ()
    endif ()

    option(QMUD_ENABLE_APPIMAGE "Enable AppImage packaging target (Linux only)" ON)
    if (QMUD_ENABLE_APPIMAGE)
        set(QMUD_APPIMAGE_VERSION "dev" CACHE STRING "Version string embedded in AppImage metadata")
        set(QMUD_APPIMAGE_WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/appimage" CACHE PATH "Working directory for AppImage assembly")
        set(QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_EXECUTABLE "" CACHE FILEPATH
                "Path to linuxdeploy-plugin-appimage executable/AppImage")
        set(QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_URL
                "https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-appimage-x86_64.AppImage"
                CACHE STRING "Download URL for linuxdeploy-plugin-appimage when missing from tools/")
        option(QMUD_APPIMAGE_NO_STRIP "Disable linuxdeploy strip step (required on newer ELF relr-based distros)" ON)

        if (NOT QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_EXECUTABLE)
            file(GLOB QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_LOCAL
                    "${CMAKE_SOURCE_DIR}/tools/linuxdeploy-plugin-appimage-*-x86_64.AppImage"
                    "${CMAKE_SOURCE_DIR}/tools/linuxdeploy-plugin-appimage-x86_64.AppImage")
            list(LENGTH QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_LOCAL QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_LOCAL_COUNT)
            if (QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_LOCAL_COUNT GREATER 0)
                list(SORT QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_LOCAL COMPARE NATURAL ORDER DESCENDING)
                list(GET QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_LOCAL 0 QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_EXECUTABLE)
            else ()
                set(QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_EXECUTABLE
                        "${CMAKE_SOURCE_DIR}/tools/linuxdeploy-plugin-appimage-x86_64.AppImage")
                if (NOT EXISTS "${QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_EXECUTABLE}")
                    file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/tools")
                    message(STATUS "linuxdeploy-plugin-appimage not found in tools/. Downloading from ${QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_URL}")
                    file(DOWNLOAD
                            "${QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_URL}"
                            "${QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_EXECUTABLE}"
                            SHOW_PROGRESS
                            STATUS QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_DOWNLOAD_STATUS
                            TLS_VERIFY ON
                    )
                    list(GET QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_DOWNLOAD_STATUS 0 QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_DOWNLOAD_CODE)
                    list(GET QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_DOWNLOAD_STATUS 1 QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_DOWNLOAD_MESSAGE)
                    if (NOT QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_DOWNLOAD_CODE EQUAL 0)
                        file(REMOVE "${QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_EXECUTABLE}")
                        message(FATAL_ERROR
                                "Failed to download linuxdeploy-plugin-appimage from "
                                "${QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_URL}: "
                                "${QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_DOWNLOAD_MESSAGE}")
                    endif ()
                    execute_process(
                            COMMAND /bin/chmod 0755 "${QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_EXECUTABLE}"
                            RESULT_VARIABLE QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_CHMOD_RESULT
                    )
                    if (NOT QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_CHMOD_RESULT EQUAL 0)
                        message(FATAL_ERROR "Downloaded linuxdeploy-plugin-appimage, but failed to chmod +x it.")
                    endif ()
                endif ()
            endif ()
        endif ()
        set(QMUD_APPIMAGE_APPDIR "${QMUD_APPIMAGE_WORK_DIR}/AppDir")
        set(QMUD_APPIMAGE_DESKTOP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/resources/appimage/qmud.desktop")
        set(QMUD_APPIMAGE_DESKTOP_DST "${QMUD_APPIMAGE_APPDIR}/usr/share/applications/qmud.desktop")
        set(QMUD_APPIMAGE_DESKTOP_ROOT_DST "${QMUD_APPIMAGE_APPDIR}/QMud.desktop")
        set(QMUD_APPIMAGE_ICON_SRC "${CMAKE_CURRENT_SOURCE_DIR}/resources/qmud/res/QMud.png")
        set(QMUD_APPIMAGE_ICON_DST "${QMUD_APPIMAGE_APPDIR}/usr/share/icons/hicolor/256x256/apps/QMud.png")
        set(QMUD_APPIMAGE_ICON_ROOT_DST "${QMUD_APPIMAGE_APPDIR}/QMud.png")
        set(QMUD_APPIMAGE_CUSTOM_APPRUN "${CMAKE_CURRENT_SOURCE_DIR}/resources/appimage/AppRun")
        set(QMUD_APPIMAGE_COLLECT_RUNTIME_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tools/scripts/collect_runtime_libs.sh")
        set(QMUD_APPIMAGE_EXE_DST "${QMUD_APPIMAGE_APPDIR}/usr/bin/QMud")
        set(QMUD_APPIMAGE_SKELETON_SOCKET_DIR "${QMUD_APPIMAGE_APPDIR}/skeleton/socket")
        set(QMUD_APPIMAGE_SKELETON_MIME_DIR "${QMUD_APPIMAGE_APPDIR}/skeleton/mime")
        set(QMUD_APPIMAGE_SKELETON_LUA_DIR "${QMUD_APPIMAGE_APPDIR}/skeleton/lua")
        if (QMUD_APPIMAGE_NO_STRIP)
            set(QMUD_APPIMAGE_STRIP_ENV NO_STRIP=1)
        else ()
            set(QMUD_APPIMAGE_STRIP_ENV)
        endif ()

        if (NOT EXISTS "${QMUD_APPIMAGE_DESKTOP_SRC}")
            message(FATAL_ERROR "Missing desktop file template for AppImage: ${QMUD_APPIMAGE_DESKTOP_SRC}")
        endif ()
        if (NOT EXISTS "${QMUD_APPIMAGE_ICON_SRC}")
            message(FATAL_ERROR "Missing AppImage icon source: ${QMUD_APPIMAGE_ICON_SRC}")
        endif ()
        if (NOT EXISTS "${QMUD_APPIMAGE_CUSTOM_APPRUN}")
            message(FATAL_ERROR "Missing AppImage custom AppRun: ${QMUD_APPIMAGE_CUSTOM_APPRUN}")
        endif ()
        if (NOT EXISTS "${QMUD_APPIMAGE_COLLECT_RUNTIME_SCRIPT}")
            message(FATAL_ERROR "Missing AppImage runtime collector script: ${QMUD_APPIMAGE_COLLECT_RUNTIME_SCRIPT}")
        endif ()

        set(QMUD_APPIMAGE_QT_RUNTIME_LIB_DIR "")
        get_target_property(QMUD_APPIMAGE_QTCORE_LOCATION Qt6::Core IMPORTED_LOCATION)
        if (QMUD_APPIMAGE_QTCORE_LOCATION)
            get_filename_component(QMUD_APPIMAGE_QT_RUNTIME_LIB_DIR "${QMUD_APPIMAGE_QTCORE_LOCATION}" DIRECTORY)
        endif ()
        if (NOT QMUD_APPIMAGE_QT_RUNTIME_LIB_DIR)
            message(FATAL_ERROR "Could not resolve Qt runtime library directory from Qt6::Core.")
        endif ()
        file(GLOB QMUD_APPIMAGE_QT_RUNTIME_LIB_FILES
                "${QMUD_APPIMAGE_QT_RUNTIME_LIB_DIR}/libQt6*.so*")
        list(LENGTH QMUD_APPIMAGE_QT_RUNTIME_LIB_FILES QMUD_APPIMAGE_QT_RUNTIME_LIB_COUNT)
        if (QMUD_APPIMAGE_QT_RUNTIME_LIB_COUNT EQUAL 0)
            message(FATAL_ERROR "No Qt runtime shared libraries found in ${QMUD_APPIMAGE_QT_RUNTIME_LIB_DIR}")
        endif ()

        if (COMMAND qt_generate_deploy_app_script)
            set(QMUD_QT_DEPLOY_SCRIPT_KEYWORD OUTPUT_SCRIPT)
            set(QMUD_QT_DEPLOY_SUPPORTS_NO_UNSUPPORTED_FLAG ON)
            set(QMUD_QT_DEPLOY_SUPPORTS_PLUGIN_FILTERS ON)
            if (DEFINED Qt6Core_DIR AND EXISTS "${Qt6Core_DIR}/Qt6CoreMacros.cmake")
                file(READ "${Qt6Core_DIR}/Qt6CoreMacros.cmake" QMUD_QTCORE_MACROS_CONTENT)
                if (QMUD_QTCORE_MACROS_CONTENT MATCHES "FILENAME_VARIABLE")
                    set(QMUD_QT_DEPLOY_SCRIPT_KEYWORD FILENAME_VARIABLE)
                endif ()
                if (NOT QMUD_QTCORE_MACROS_CONTENT MATCHES "NO_UNSUPPORTED_PLATFORM_ERROR")
                    set(QMUD_QT_DEPLOY_SUPPORTS_NO_UNSUPPORTED_FLAG OFF)
                endif ()
                if (NOT QMUD_QTCORE_MACROS_CONTENT MATCHES "INCLUDE_PLUGIN_TYPES")
                    set(QMUD_QT_DEPLOY_SUPPORTS_PLUGIN_FILTERS OFF)
                endif ()
            endif ()

            set(QMUD_APPIMAGE_QT_DEPLOY_ARGS
                    TARGET QMud
                    ${QMUD_QT_DEPLOY_SCRIPT_KEYWORD} QMUD_APPIMAGE_QT_DEPLOY_SCRIPT)
            if (QMUD_QT_DEPLOY_SUPPORTS_NO_UNSUPPORTED_FLAG)
                list(APPEND QMUD_APPIMAGE_QT_DEPLOY_ARGS NO_UNSUPPORTED_PLATFORM_ERROR)
            endif ()
            if (QMUD_QT_DEPLOY_SUPPORTS_PLUGIN_FILTERS)
                list(APPEND QMUD_APPIMAGE_QT_DEPLOY_ARGS
                        INCLUDE_PLUGIN_TYPES
                        platforms
                        tls
                        sqldrivers
                        platformthemes
                        platforminputcontexts
                        xcbglintegrations
                        egldeviceintegrations
                        wayland-decoration-client
                        wayland-graphics-integration-client
                        wayland-shell-integration
                        INCLUDE_PLUGINS
                        qsqlite
                        qopensslbackend
                        qxcb
                        qwayland
                        qwayland-egl
                        qwayland-generic)
            endif ()
            qt_generate_deploy_app_script(${QMUD_APPIMAGE_QT_DEPLOY_ARGS})
        else ()
            message(FATAL_ERROR "Qt deploy API is unavailable; cannot stage AppImage runtime dependencies.")
        endif ()

        set(QMUD_APPIMAGE_LPEG_SO "${QMUD_SYSTEM_LPEG_SO}")
        if (NOT QMUD_APPIMAGE_LPEG_SO)
            find_file(QMUD_APPIMAGE_LPEG_SO
                    NAMES lpeg.so
                    PATHS ${QMUD_LUA_MODULE_SEARCH_PATHS}
                    PATH_SUFFIXES lua/5.4 lua/5.3 lua/5.2 lua/5.1
            )
        endif ()

        set(QMUD_APPIMAGE_SOCKET_CORE_SO "${QMUD_SYSTEM_SOCKET_CORE_SO}")
        if (NOT QMUD_APPIMAGE_SOCKET_CORE_SO)
            find_file(QMUD_APPIMAGE_SOCKET_CORE_SO
                    NAMES core.so
                    PATHS ${QMUD_LUA_MODULE_SEARCH_PATHS}
                    PATH_SUFFIXES lua/5.4/socket lua/5.3/socket lua/5.2/socket lua/5.1/socket
            )
        endif ()

        set(QMUD_APPIMAGE_MIME_CORE_SO "${QMUD_SYSTEM_MIME_CORE_SO}")
        if (NOT QMUD_APPIMAGE_MIME_CORE_SO)
            find_file(QMUD_APPIMAGE_MIME_CORE_SO
                    NAMES core.so
                    PATHS ${QMUD_LUA_MODULE_SEARCH_PATHS}
                    PATH_SUFFIXES lua/5.4/mime lua/5.3/mime lua/5.2/mime lua/5.1/mime
            )
        endif ()

        if (NOT QMUD_APPIMAGE_LPEG_SO)
            message(FATAL_ERROR "System lpeg.so not found; AppImage packaging requires system LPeg.")
        endif ()
        if (NOT QMUD_APPIMAGE_SOCKET_CORE_SO)
            message(FATAL_ERROR "System LuaSocket socket/core.so not found; AppImage packaging requires system LuaSocket.")
        endif ()
        if (NOT QMUD_APPIMAGE_MIME_CORE_SO)
            message(FATAL_ERROR "System LuaSocket mime/core.so not found; AppImage packaging requires system LuaSocket.")
        endif ()
        if (NOT QMUD_LUA_LIBRARY_FILES)
            message(WARNING "Lua shared library path was not resolved; AppImage may miss liblua in usr/lib.")
        endif ()

        if (QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_EXECUTABLE)
            add_custom_target(AppImage
                    DEPENDS QMud
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${QMUD_APPIMAGE_WORK_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_APPIMAGE_APPDIR}/usr/bin"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_APPIMAGE_APPDIR}/usr/lib"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_APPIMAGE_APPDIR}/usr/glibc"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_APPIMAGE_APPDIR}/usr/share/applications"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_APPIMAGE_APPDIR}/usr/share/icons/hicolor/256x256/apps"
                    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:QMud>" "${QMUD_APPIMAGE_EXE_DST}"
                    COMMAND ${CMAKE_COMMAND} -E copy "${QMUD_APPIMAGE_DESKTOP_SRC}" "${QMUD_APPIMAGE_DESKTOP_DST}"
                    COMMAND ${CMAKE_COMMAND} -E copy "${QMUD_APPIMAGE_DESKTOP_SRC}" "${QMUD_APPIMAGE_DESKTOP_ROOT_DST}"
                    COMMAND ${CMAKE_COMMAND} -E copy "${QMUD_APPIMAGE_ICON_SRC}" "${QMUD_APPIMAGE_ICON_DST}"
                    COMMAND ${CMAKE_COMMAND} -E copy "${QMUD_APPIMAGE_ICON_SRC}" "${QMUD_APPIMAGE_ICON_ROOT_DST}"
                    COMMAND ${CMAKE_COMMAND} -E copy "${QMUD_APPIMAGE_CUSTOM_APPRUN}" "${QMUD_APPIMAGE_APPDIR}/AppRun"
                    COMMAND /bin/chmod 0755 "${QMUD_APPIMAGE_APPDIR}/AppRun"
                    COMMENT "Preparing QMud AppDir for AppImage"
                    USES_TERMINAL
            )
            if (EXISTS "${SKELETON_SRC}")
                foreach (_qmud_skel_dir IN LISTS SKELETON_DIRS)
                    file(RELATIVE_PATH _qmud_skel_rel "${SKELETON_SRC}" "${_qmud_skel_dir}")
                    if (NOT _qmud_skel_rel STREQUAL "")
                        add_custom_command(TARGET AppImage POST_BUILD
                                COMMAND ${CMAKE_COMMAND} -E make_directory
                                "$<SHELL_PATH:${QMUD_APPIMAGE_APPDIR}/skeleton/${_qmud_skel_rel}>"
                        )
                    endif ()
                endforeach ()
                add_custom_command(TARGET AppImage POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E echo "Copying skeleton into AppDir"
                        COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "$<SHELL_PATH:${SKELETON_SRC}>"
                        "$<SHELL_PATH:${QMUD_APPIMAGE_APPDIR}/skeleton>"
                        COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "$<SHELL_PATH:${CMAKE_CURRENT_BINARY_DIR}/lua>"
                        "$<SHELL_PATH:${QMUD_APPIMAGE_SKELETON_LUA_DIR}>"
                        COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "$<SHELL_PATH:${CMAKE_CURRENT_BINARY_DIR}/socket>"
                        "$<SHELL_PATH:${QMUD_APPIMAGE_SKELETON_SOCKET_DIR}>"
                        COMMAND ${CMAKE_COMMAND} -E make_directory "$<SHELL_PATH:${QMUD_APPIMAGE_SKELETON_LUA_DIR}>"
                        COMMAND ${CMAKE_COMMAND} -E make_directory "$<SHELL_PATH:${QMUD_APPIMAGE_SKELETON_SOCKET_DIR}>"
                        COMMAND ${CMAKE_COMMAND} -E make_directory "$<SHELL_PATH:${QMUD_APPIMAGE_SKELETON_MIME_DIR}>"
                )
            endif ()
            add_custom_command(TARGET AppImage POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E echo "Staging Qt runtime libraries/plugins into AppDir"
                    COMMAND ${CMAKE_COMMAND}
                    -DCMAKE_INSTALL_PREFIX="$<SHELL_PATH:${QMUD_APPIMAGE_APPDIR}>"
                    -DQT_DEPLOY_PREFIX="$<SHELL_PATH:${QMUD_APPIMAGE_APPDIR}>"
                    -DQT_DEPLOY_BIN_DIR=usr/bin
                    -DQT_DEPLOY_LIB_DIR=usr/lib
                    -DQT_DEPLOY_PLUGINS_DIR=usr/plugins
                    -DQT_DEPLOY_QML_DIR=usr/qml
                    -DQT_DEPLOY_TRANSLATIONS_DIR=usr/translations
                    -P "$<SHELL_PATH:${QMUD_APPIMAGE_QT_DEPLOY_SCRIPT}>"
            )
            add_custom_command(TARGET AppImage POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E echo "Removing GTK3 platformtheme plugin from AppDir"
                    COMMAND ${CMAKE_COMMAND} -E rm -f
                    "$<SHELL_PATH:${QMUD_APPIMAGE_APPDIR}/usr/plugins/platformthemes/libqgtk3.so>"
            )
            get_filename_component(QMUD_APPIMAGE_QT_RUNTIME_ROOT "${QMUD_APPIMAGE_QT_RUNTIME_LIB_DIR}" DIRECTORY)
            set(QMUD_APPIMAGE_QT_TLS_PLUGIN_DIR "")
            set(QMUD_APPIMAGE_QT_TLS_PLUGIN_FILES)
            set(QMUD_APPIMAGE_QT_TLS_PLUGIN_CANDIDATE_DIRS
                    "${QMUD_APPIMAGE_QT_RUNTIME_ROOT}/plugins/tls"
                    "${QMUD_APPIMAGE_QT_RUNTIME_LIB_DIR}/qt6/plugins/tls"
                    "${QMUD_APPIMAGE_QT_RUNTIME_ROOT}/lib/qt6/plugins/tls"
                    "${QMUD_APPIMAGE_QT_RUNTIME_ROOT}/lib64/qt6/plugins/tls")
            if (DEFINED Qt6_DIR AND NOT Qt6_DIR STREQUAL "")
                get_filename_component(QMUD_APPIMAGE_QT6_ROOT "${Qt6_DIR}/../../.." ABSOLUTE)
                list(APPEND QMUD_APPIMAGE_QT_TLS_PLUGIN_CANDIDATE_DIRS
                        "${QMUD_APPIMAGE_QT6_ROOT}/plugins/tls"
                        "${QMUD_APPIMAGE_QT6_ROOT}/lib/qt6/plugins/tls"
                        "${QMUD_APPIMAGE_QT6_ROOT}/lib64/qt6/plugins/tls")
            endif ()
            list(REMOVE_DUPLICATES QMUD_APPIMAGE_QT_TLS_PLUGIN_CANDIDATE_DIRS)
            foreach (_qmud_tls_candidate_dir IN LISTS QMUD_APPIMAGE_QT_TLS_PLUGIN_CANDIDATE_DIRS)
                file(GLOB _qmud_tls_candidate_files
                        "${_qmud_tls_candidate_dir}/libqopensslbackend.so*")
                if (_qmud_tls_candidate_files)
                    set(QMUD_APPIMAGE_QT_TLS_PLUGIN_DIR "${_qmud_tls_candidate_dir}")
                    set(QMUD_APPIMAGE_QT_TLS_PLUGIN_FILES ${_qmud_tls_candidate_files})
                    break()
                endif ()
            endforeach ()
            if (QMUD_APPIMAGE_QT_TLS_PLUGIN_FILES)
                add_custom_command(TARGET AppImage POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E echo "Ensuring Qt OpenSSL TLS backend plugin is staged"
                        COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<SHELL_PATH:${QMUD_APPIMAGE_APPDIR}/usr/plugins/tls>"
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${QMUD_APPIMAGE_QT_TLS_PLUGIN_FILES}
                        "$<SHELL_PATH:${QMUD_APPIMAGE_APPDIR}/usr/plugins/tls>"
                )
            else ()
                string(JOIN ", " QMUD_APPIMAGE_QT_TLS_PLUGIN_SEARCH_PATHS
                        ${QMUD_APPIMAGE_QT_TLS_PLUGIN_CANDIDATE_DIRS})
                message(WARNING "Qt TLS plugin libqopensslbackend.so was not found in candidate directories: ${QMUD_APPIMAGE_QT_TLS_PLUGIN_SEARCH_PATHS}. AppImage update checks may fail without a functional TLS backend.")
            endif ()
            add_custom_command(TARGET AppImage POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E echo "Copying Qt shared libraries into AppDir/usr/lib"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${QMUD_APPIMAGE_QT_RUNTIME_LIB_FILES}
                    "$<SHELL_PATH:${QMUD_APPIMAGE_APPDIR}/usr/lib>"
            )
            if (QMUD_LUA_LIBRARY_FILES)
                add_custom_command(TARGET AppImage POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E echo "Copying Lua shared library into AppDir/usr/lib"
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${QMUD_LUA_LIBRARY_FILES}
                        "$<SHELL_PATH:${QMUD_APPIMAGE_APPDIR}/usr/lib>"
                )
            endif ()
            if (QMUD_APPIMAGE_SOCKET_CORE_SO)
                add_custom_command(TARGET AppImage POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E echo "Staging LuaSocket socket/core.so into AppDir skeleton"
                        COMMAND ${CMAKE_COMMAND} -E copy
                        "$<SHELL_PATH:${QMUD_APPIMAGE_SOCKET_CORE_SO}>"
                        "$<SHELL_PATH:${QMUD_APPIMAGE_SKELETON_SOCKET_DIR}/core.so>"
                )
            endif ()
            if (QMUD_APPIMAGE_MIME_CORE_SO)
                add_custom_command(TARGET AppImage POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E echo "Staging LuaSocket mime/core.so into AppDir skeleton"
                        COMMAND ${CMAKE_COMMAND} -E copy
                        "$<SHELL_PATH:${QMUD_APPIMAGE_MIME_CORE_SO}>"
                        "$<SHELL_PATH:${QMUD_APPIMAGE_SKELETON_MIME_DIR}/core.so>"
                )
            endif ()
            if (QMUD_APPIMAGE_LPEG_SO)
                add_custom_command(TARGET AppImage POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E echo "Staging Lua LPeg module into AppDir skeleton/lua"
                        COMMAND ${CMAKE_COMMAND} -E copy
                        "$<SHELL_PATH:${QMUD_APPIMAGE_LPEG_SO}>"
                        "$<SHELL_PATH:${QMUD_APPIMAGE_SKELETON_LUA_DIR}/lpeg.so>"
                )
            endif ()
            add_custom_command(TARGET AppImage POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E echo "Collecting transitive runtime libraries into AppDir/usr/lib and AppDir/usr/glibc"
                    COMMAND ${CMAKE_COMMAND} -E env
                    QMUD_APPIMAGE_BUNDLE_GLIBC=1
                    /bin/sh
                    "$<SHELL_PATH:${QMUD_APPIMAGE_COLLECT_RUNTIME_SCRIPT}>"
                    "$<SHELL_PATH:${QMUD_APPIMAGE_APPDIR}>"
                    "$<SHELL_PATH:${QMUD_APPIMAGE_EXE_DST}>"
                    "$<SHELL_PATH:${QMUD_APPIMAGE_SKELETON_SOCKET_DIR}/core.so>"
                    "$<SHELL_PATH:${QMUD_APPIMAGE_SKELETON_MIME_DIR}/core.so>"
                    "$<SHELL_PATH:${QMUD_APPIMAGE_SKELETON_LUA_DIR}/lpeg.so>"
            )
            add_custom_command(TARGET AppImage POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E echo "Packaging AppImage with linuxdeploy-plugin-appimage"
                    COMMAND ${CMAKE_COMMAND} -E chdir "$<SHELL_PATH:${QMUD_APPIMAGE_WORK_DIR}>"
                    ${CMAKE_COMMAND} -E env
                    APPIMAGE_EXTRACT_AND_RUN=1
                    LINUXDEPLOY_OUTPUT_VERSION=${QMUD_APPIMAGE_VERSION}
                    ARCH=${CMAKE_SYSTEM_PROCESSOR}
                    "$<SHELL_PATH:${QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_EXECUTABLE}>"
                    --appdir="$<SHELL_PATH:${QMUD_APPIMAGE_APPDIR}>"
            )
        else ()
            add_custom_target(AppImage
                    COMMAND ${CMAKE_COMMAND} -E echo "AppImage packaging requires linuxdeploy-plugin-appimage."
                    COMMAND ${CMAKE_COMMAND} -E echo "Set QMUD_LINUXDEPLOY_PLUGIN_APPIMAGE_EXECUTABLE,"
                    COMMAND ${CMAKE_COMMAND} -E echo "or place tools/linuxdeploy-plugin-appimage-*-x86_64.AppImage in the repo."
                    COMMAND ${CMAKE_COMMAND} -E false
            )
        endif ()
    endif ()

    option(QMUD_ENABLE_APPIMAGE_DOCKER "Enable AppImageDocker target using a prebuilt custom Linux builder image" ON)
    if (QMUD_ENABLE_APPIMAGE_DOCKER)
        set(QMUD_APPIMAGE_DOCKER_IMAGE "qmud-appimage-builder:qt6.10" CACHE STRING
                "Docker image for AppImageDocker (build from tools/docker/appimage-qt610/Dockerfile)")
        set(QMUD_APPIMAGE_DOCKER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/appimage-docker-build" CACHE PATH
                "Host build directory mounted into container for AppImageDocker")
        set(QMUD_APPIMAGE_DOCKER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/appimage-docker-out" CACHE PATH
                "Host output directory for AppImageDocker artifacts")
        set(QMUD_APPIMAGE_DOCKER_BUILD_TYPE "Release" CACHE STRING
                "CMake build type used inside AppImageDocker container")
        set(QMUD_APPIMAGE_DOCKER_QT_PREFIX "/opt/Qt/6.10.2/gcc_64" CACHE PATH
                "Qt Linux prefix inside AppImageDocker image")
        set(QMUD_APPIMAGE_DOCKER_LINUXDEPLOY_PLUGIN_APPIMAGE_URL
                "https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-appimage-x86_64.AppImage"
                CACHE STRING "Download URL for linuxdeploy-plugin-appimage when missing from AppImageDocker build dir")
        set(QMUD_APPIMAGE_DOCKER_VERSION_DEFAULT "dev")
        if (DEFINED QMUD_APPIMAGE_VERSION)
            set(QMUD_APPIMAGE_DOCKER_VERSION_DEFAULT "${QMUD_APPIMAGE_VERSION}")
        endif ()
        set(QMUD_APPIMAGE_DOCKER_VERSION "${QMUD_APPIMAGE_DOCKER_VERSION_DEFAULT}" CACHE STRING
                "Version string embedded in AppImage metadata when building via AppImageDocker")
        unset(QMUD_APPIMAGE_DOCKER_VERSION_DEFAULT)
        option(QMUD_APPIMAGE_DOCKER_DISABLE_SELINUX
                "Disable SELinux labeling for AppImageDocker container runs (--security-opt label=disable)" ON)
        option(QMUD_APPIMAGE_DOCKER_RUN_AS_HOST_USER
                "Run AppImageDocker container with host UID/GID to avoid root-owned output files" ON)
        set(QMUD_APPIMAGE_DOCKER_HOST_UID "$ENV{UID}" CACHE STRING "Host UID for AppImageDocker --user")
        set(QMUD_APPIMAGE_DOCKER_HOST_GID "" CACHE STRING "Host GID for AppImageDocker --user")

        if (QMUD_APPIMAGE_DOCKER_DISABLE_SELINUX)
            set(QMUD_APPIMAGE_DOCKER_SECURITY_OPT --security-opt label=disable)
        else ()
            set(QMUD_APPIMAGE_DOCKER_SECURITY_OPT)
        endif ()

        if (QMUD_APPIMAGE_DOCKER_RUN_AS_HOST_USER)
            if (NOT QMUD_APPIMAGE_DOCKER_HOST_UID)
                execute_process(
                        COMMAND id -u
                        OUTPUT_VARIABLE QMUD_APPIMAGE_DOCKER_HOST_UID
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
            endif ()
            if (NOT QMUD_APPIMAGE_DOCKER_HOST_GID)
                execute_process(
                        COMMAND id -g
                        OUTPUT_VARIABLE QMUD_APPIMAGE_DOCKER_HOST_GID
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
            endif ()
            if (NOT QMUD_APPIMAGE_DOCKER_HOST_UID)
                set(QMUD_APPIMAGE_DOCKER_HOST_UID 1000)
            endif ()
            if (NOT QMUD_APPIMAGE_DOCKER_HOST_GID)
                set(QMUD_APPIMAGE_DOCKER_HOST_GID 1000)
            endif ()
            set(QMUD_APPIMAGE_DOCKER_HOST_UID "${QMUD_APPIMAGE_DOCKER_HOST_UID}" CACHE STRING
                    "Host UID for AppImageDocker --user" FORCE)
            set(QMUD_APPIMAGE_DOCKER_HOST_GID "${QMUD_APPIMAGE_DOCKER_HOST_GID}" CACHE STRING
                    "Host GID for AppImageDocker --user" FORCE)
            set(QMUD_APPIMAGE_DOCKER_USER_ARGS --user ${QMUD_APPIMAGE_DOCKER_HOST_UID}:${QMUD_APPIMAGE_DOCKER_HOST_GID})
        else ()
            set(QMUD_APPIMAGE_DOCKER_USER_ARGS)
        endif ()

        if (NOT DEFINED QMUD_DOCKER_EXECUTABLE OR QMUD_DOCKER_EXECUTABLE STREQUAL "")
            set(QMUD_DOCKER_EXECUTABLE "docker")
        endif ()

        set(QMUD_APPIMAGE_DOCKER_SCRIPT "${CMAKE_SOURCE_DIR}/tools/scripts/appimage_docker_build.sh")

        find_program(QMUD_DOCKER_EXECUTABLE_FOUND_APPIMAGEDOCKER
                NAMES ${QMUD_DOCKER_EXECUTABLE}
                NO_CACHE)
        set(QMUD_APPIMAGE_DOCKER_USERNS_ARGS)
        if (QMUD_DOCKER_EXECUTABLE_FOUND_APPIMAGEDOCKER AND QMUD_APPIMAGE_DOCKER_RUN_AS_HOST_USER)
            execute_process(
                    COMMAND ${QMUD_DOCKER_EXECUTABLE_FOUND_APPIMAGEDOCKER} --version
                    OUTPUT_VARIABLE QMUD_APPIMAGE_DOCKER_VERSION_OUTPUT
                    ERROR_VARIABLE QMUD_APPIMAGE_DOCKER_VERSION_ERROR
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_STRIP_TRAILING_WHITESPACE
            )
            string(TOLOWER "${QMUD_APPIMAGE_DOCKER_VERSION_OUTPUT}\n${QMUD_APPIMAGE_DOCKER_VERSION_ERROR}" QMUD_APPIMAGE_DOCKER_VERSION_TEXT)
            if (QMUD_APPIMAGE_DOCKER_VERSION_TEXT MATCHES "podman")
                set(QMUD_APPIMAGE_DOCKER_USERNS_ARGS --userns keep-id)
                message(STATUS "AppImageDocker: detected podman backend, enabling --userns keep-id for host UID/GID readability")
            endif ()
        endif ()

        if (QMUD_DOCKER_EXECUTABLE_FOUND_APPIMAGEDOCKER)
            add_custom_target(AppImageDocker
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_APPIMAGE_DOCKER_BUILD_DIR}"
                    COMMAND /bin/chmod 0777 "${QMUD_APPIMAGE_DOCKER_BUILD_DIR}"
                    COMMAND ${QMUD_DOCKER_EXECUTABLE_FOUND_APPIMAGEDOCKER} run --rm
                    ${QMUD_APPIMAGE_DOCKER_SECURITY_OPT}
                    ${QMUD_APPIMAGE_DOCKER_USERNS_ARGS}
                    ${QMUD_APPIMAGE_DOCKER_USER_ARGS}
                    -e CCACHE_DISABLE=1
                    -e QMUD_APPIMAGE_DOCKER_BUILD_TYPE=${QMUD_APPIMAGE_DOCKER_BUILD_TYPE}
                    -e QMUD_APPIMAGE_DOCKER_QT_PREFIX=${QMUD_APPIMAGE_DOCKER_QT_PREFIX}
                    -e QMUD_APPIMAGE_VERSION=${QMUD_APPIMAGE_DOCKER_VERSION}
                    -e QMUD_APPIMAGE_DOCKER_LINUXDEPLOY_PLUGIN_APPIMAGE_URL=${QMUD_APPIMAGE_DOCKER_LINUXDEPLOY_PLUGIN_APPIMAGE_URL}
                    -v "${CMAKE_SOURCE_DIR}:/home/user/project:ro"
                    -v "${QMUD_APPIMAGE_DOCKER_BUILD_DIR}:/home/user/build:rw"
                    "${QMUD_APPIMAGE_DOCKER_IMAGE}"
                    sh /home/user/project/tools/scripts/appimage_docker_build.sh
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${QMUD_APPIMAGE_DOCKER_OUTPUT_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${QMUD_APPIMAGE_DOCKER_OUTPUT_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${QMUD_APPIMAGE_DOCKER_BUILD_DIR}/appimage"
                    "${QMUD_APPIMAGE_DOCKER_OUTPUT_DIR}"
                    COMMENT "Building AppImage via ${QMUD_APPIMAGE_DOCKER_IMAGE} (AppImageDocker)"
                    USES_TERMINAL
            )
        else ()
            add_custom_target(AppImageDocker
                    COMMAND ${CMAKE_COMMAND} -E echo "AppImageDocker requires a docker-compatible executable."
                    COMMAND ${CMAKE_COMMAND} -E echo "Set QMUD_DOCKER_EXECUTABLE to docker/podman and ensure it is available."
                    COMMAND ${CMAKE_COMMAND} -E false
            )
        endif ()
    endif ()

    if (CMAKE_CONFIGURATION_TYPES)
        set(QMUD_DEPLOY_BUILD_CONFIG_ARGS --config $<CONFIG>)
    else ()
        set(QMUD_DEPLOY_BUILD_CONFIG_ARGS)
    endif ()

    if (TARGET AppImageDocker AND TARGET MacDocker AND TARGET WinDocker)
        add_custom_target(Deploy
                COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" ${QMUD_DEPLOY_BUILD_CONFIG_ARGS} --target AppImageDocker
                COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" ${QMUD_DEPLOY_BUILD_CONFIG_ARGS} --target MacDocker
                COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" ${QMUD_DEPLOY_BUILD_CONFIG_ARGS} --target WinDocker
                COMMENT "Running deploy pipeline targets in order: AppImageDocker -> MacDocker -> WinDocker"
                USES_TERMINAL
        )
    else ()
        add_custom_target(Deploy
                COMMAND ${CMAKE_COMMAND} -E echo "Deploy requires AppImageDocker, MacDocker, and WinDocker targets."
                COMMAND ${CMAKE_COMMAND} -E false
        )
    endif ()
endif ()
