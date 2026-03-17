set(QMUD_TEST_DEFAULT_ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen"
        "LANG=C.UTF-8"
        "LC_ALL=C.UTF-8"
        "TZ=UTC")

function(qmud_add_qtest target_name)
    set(options GUI)
    set(one_value_args TIMEOUT)
    set(multi_value_args SOURCES LIBRARIES LABELS)
    cmake_parse_arguments(QMUD_QTEST "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if (NOT QMUD_QTEST_SOURCES)
        message(FATAL_ERROR "qmud_add_qtest(${target_name}) requires SOURCES")
    endif ()

    add_executable(${target_name} ${QMUD_QTEST_SOURCES})
    set_target_properties(${target_name} PROPERTIES AUTOMOC ON)
    target_compile_definitions(${target_name}
            PRIVATE
            QMUD_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(${target_name}
            PRIVATE
            Qt6::Core
            Qt6::Test
            ${QMUD_QTEST_LIBRARIES})
    target_include_directories(${target_name}
            PRIVATE
            "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/support")

    if (QMUD_QTEST_GUI)
        target_link_libraries(${target_name} PRIVATE Qt6::Widgets)
    endif ()

    add_test(NAME ${target_name} COMMAND ${target_name})

    set(test_labels "${QMUD_QTEST_LABELS}")
    if (NOT test_labels)
        set(test_labels "unit")
    endif ()
    if (QMUD_QTEST_GUI)
        list(APPEND test_labels "gui")
    endif ()
    list(REMOVE_DUPLICATES test_labels)

    set(test_timeout "${QMUD_QTEST_TIMEOUT}")
    if (NOT test_timeout)
        set(test_timeout 30)
    endif ()

    set_tests_properties(${target_name}
            PROPERTIES
            LABELS "${test_labels}"
            TIMEOUT "${test_timeout}"
            ENVIRONMENT "${QMUD_TEST_DEFAULT_ENVIRONMENT}")
endfunction()

add_library(qmud_test_support STATIC
        support/TestEnvironment.cpp
        support/TelnetCallbackSpy.cpp)
target_link_libraries(qmud_test_support PUBLIC Qt6::Core)
target_include_directories(qmud_test_support
        PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/support"
        "${CMAKE_SOURCE_DIR}/src")

qmud_add_qtest(tst_QTestSmoke
        SOURCES
        smoke/tst_QTestSmoke.cpp
        LIBRARIES
        qmud_test_support
        LABELS
        unit
        smoke
        TIMEOUT
        10)

qmud_add_qtest(tst_StringUtils
        SOURCES
        unit/tst_StringUtils.cpp
        "${CMAKE_SOURCE_DIR}/src/StringUtils.cpp"
        "${CMAKE_SOURCE_DIR}/src/Accelerators.cpp"
        LABELS
        unit)

qmud_add_qtest(tst_EncodingUtils
        SOURCES
        unit/tst_EncodingUtils.cpp
        "${CMAKE_SOURCE_DIR}/src/EncodingUtils.cpp"
        LABELS
        unit)

qmud_add_qtest(tst_AnsiSgrParseUtils
        SOURCES
        unit/tst_AnsiSgrParseUtils.cpp
        "${CMAKE_SOURCE_DIR}/src/AnsiSgrParseUtils.cpp"
        LABELS
        unit)

qmud_add_qtest(tst_ColorUtils
        SOURCES
        unit/tst_ColorUtils.cpp
        "${CMAKE_SOURCE_DIR}/src/ColorUtils.cpp"
        LABELS
        unit)

qmud_add_qtest(tst_FontUtils
        GUI
        SOURCES
        unit/tst_FontUtils.cpp
        "${CMAKE_SOURCE_DIR}/src/FontUtils.cpp"
        LABELS
        unit)

qmud_add_qtest(tst_AcceleratorUtils
        GUI
        SOURCES
        unit/tst_AcceleratorUtils.cpp
        "${CMAKE_SOURCE_DIR}/src/AcceleratorUtils.cpp"
        LABELS
        unit)

qmud_add_qtest(tst_BraceMatch
        GUI
        SOURCES
        unit/tst_BraceMatch.cpp
        "${CMAKE_SOURCE_DIR}/src/BraceMatch.cpp"
        LABELS
        unit)

qmud_add_qtest(tst_WorldOptions
        SOURCES
        unit/tst_WorldOptions.cpp
        "${CMAKE_SOURCE_DIR}/src/WorldOptions.cpp"
        LABELS
        unit)

qmud_add_qtest(tst_ImportMergeUtils
        SOURCES
        unit/tst_ImportMergeUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_WorldOptionDefaults
        SOURCES
        unit/tst_WorldOptionDefaults.cpp
        "${CMAKE_SOURCE_DIR}/src/WorldOptionDefaults.cpp"
        "${CMAKE_SOURCE_DIR}/src/StringUtils.cpp"
        "${CMAKE_SOURCE_DIR}/src/Accelerators.cpp"
        LABELS
        unit)

qmud_add_qtest(tst_DoubleMetaphone
        SOURCES
        unit/tst_DoubleMetaphone.cpp
        "${CMAKE_SOURCE_DIR}/src/DoubleMetaphone.cpp"
        LABELS
        unit)

qmud_add_qtest(tst_AsciiArt
        SOURCES
        unit/tst_AsciiArt.cpp
        "${CMAKE_SOURCE_DIR}/src/AsciiArt.cpp"
        LABELS
        unit)

qmud_add_qtest(tst_RegexCompat
        SOURCES
        regression/tst_RegexCompat.cpp
        LABELS
        integration
        regression
        TIMEOUT
        15)

qmud_add_qtest(tst_TelnetProcessor_Options
        SOURCES
        integration/tst_TelnetProcessor_Options.cpp
        "${CMAKE_SOURCE_DIR}/src/TelnetProcessor.cpp"
        LIBRARIES
        qmud_test_support
        ${QMUD_ZLIB_TARGET}
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_TelnetProcessor_Mxp
        SOURCES
        integration/tst_TelnetProcessor_Mxp.cpp
        "${CMAKE_SOURCE_DIR}/src/TelnetProcessor.cpp"
        LIBRARIES
        qmud_test_support
        ${QMUD_ZLIB_TARGET}
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_TelnetProcessor_Subnegotiation
        SOURCES
        integration/tst_TelnetProcessor_Subnegotiation.cpp
        "${CMAKE_SOURCE_DIR}/src/TelnetProcessor.cpp"
        LIBRARIES
        qmud_test_support
        ${QMUD_ZLIB_TARGET}
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_TelnetProcessor_Encoding
        SOURCES
        integration/tst_TelnetProcessor_Encoding.cpp
        "${CMAKE_SOURCE_DIR}/src/TelnetProcessor.cpp"
        LIBRARIES
        qmud_test_support
        ${QMUD_ZLIB_TARGET}
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_TelnetProcessor_Compression
        SOURCES
        integration/tst_TelnetProcessor_Compression.cpp
        "${CMAKE_SOURCE_DIR}/src/TelnetProcessor.cpp"
        LIBRARIES
        qmud_test_support
        ${QMUD_ZLIB_TARGET}
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_TelnetProcessor_Malformed
        SOURCES
        integration/tst_TelnetProcessor_Malformed.cpp
        "${CMAKE_SOURCE_DIR}/src/TelnetProcessor.cpp"
        LIBRARIES
        qmud_test_support
        ${QMUD_ZLIB_TARGET}
        LABELS
        integration
        regression
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldDocument_Load
        SOURCES
        integration/tst_WorldDocument_Load.cpp
        "${CMAKE_SOURCE_DIR}/src/WorldDocument.cpp"
        LIBRARIES
        qmud_test_support
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldDocument_Masks
        SOURCES
        integration/tst_WorldDocument_Masks.cpp
        "${CMAKE_SOURCE_DIR}/src/WorldDocument.cpp"
        LIBRARIES
        qmud_test_support
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldDocument_Includes
        SOURCES
        integration/tst_WorldDocument_Includes.cpp
        "${CMAKE_SOURCE_DIR}/src/WorldDocument.cpp"
        LIBRARIES
        qmud_test_support
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldDocument_RoundTrip
        SOURCES
        integration/tst_WorldDocument_RoundTrip.cpp
        "${CMAKE_SOURCE_DIR}/src/WorldDocument.cpp"
        LIBRARIES
        qmud_test_support
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldCommandProcessor_Speedwalk
        SOURCES
        integration/tst_WorldCommandProcessor_Speedwalk.cpp
        "${CMAKE_SOURCE_DIR}/src/SpeedwalkParser.cpp"
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldCommandProcessor_Alias
        SOURCES
        integration/tst_WorldCommandProcessor_Alias.cpp
        "${CMAKE_SOURCE_DIR}/src/AliasMatchUtils.cpp"
        "${CMAKE_SOURCE_DIR}/src/CommandPatternUtils.cpp"
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldCommandProcessor_Regex
        SOURCES
        integration/tst_WorldCommandProcessor_Regex.cpp
        "${CMAKE_SOURCE_DIR}/src/CommandPatternUtils.cpp"
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldCommandProcessor_SendTargets
        SOURCES
        integration/tst_WorldCommandProcessor_SendTargets.cpp
        "${CMAKE_SOURCE_DIR}/src/CommandTextUtils.cpp"
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldCommandProcessor_Queueing
        SOURCES
        integration/tst_WorldCommandProcessor_Queueing.cpp
        "${CMAKE_SOURCE_DIR}/src/CommandQueueUtils.cpp"
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldCommandProcessor_TimerAccounting
        GUI
        SOURCES
        integration/tst_WorldCommandProcessor_TimerAccounting.cpp
        "${CMAKE_SOURCE_DIR}/src/TimerSchedulingUtils.cpp"
        LABELS
        integration
        TIMEOUT
        20)

if (QMUD_ENABLE_GUI_TESTS)
    qmud_add_qtest(tst_MainFrame_Actions
            SOURCES
            gui/tst_MainFrame_Actions.cpp
            "${CMAKE_SOURCE_DIR}/src/MainFrameActionUtils.cpp"
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_Dialog_WorldPreferences
            GUI
            SOURCES
            gui/tst_Dialog_WorldPreferences.cpp
            "${CMAKE_SOURCE_DIR}/src/WorldPreferencesRoutingUtils.cpp"
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_WorldView_Basic
            GUI
            SOURCES
            gui/tst_WorldView_Basic.cpp
            "${CMAKE_SOURCE_DIR}/src/WorldView.cpp"
            "${CMAKE_SOURCE_DIR}/src/WorldView.h"
            "${CMAKE_SOURCE_DIR}/src/MainWindowHostResolver.cpp"
            "${CMAKE_SOURCE_DIR}/src/dialogs/FindDialog.cpp"
            LIBRARIES
            Qt6::Sql
            LABELS
            gui
            TIMEOUT
            20)
    target_compile_options(tst_WorldView_Basic PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(tst_WorldView_Basic PRIVATE -Wl,--gc-sections)

    qmud_add_qtest(tst_Dialog_ImportXml
            GUI
            SOURCES
            gui/tst_Dialog_ImportXml.cpp
            "${CMAKE_SOURCE_DIR}/src/dialogs/ImportXmlDialog.cpp"
            "${CMAKE_SOURCE_DIR}/src/AppController.h"
            "${CMAKE_SOURCE_DIR}/src/dialogs/PluginsDialog.h"
            LIBRARIES
            Qt6::Sql
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_Dialog_Plugins
            GUI
            SOURCES
            gui/tst_Dialog_Plugins.cpp
            "${CMAKE_SOURCE_DIR}/src/dialogs/PluginsDialog.cpp"
            "${CMAKE_SOURCE_DIR}/src/AppController.h"
            "${CMAKE_SOURCE_DIR}/src/WorldRuntime.h"
            LIBRARIES
            Qt6::Sql
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_Dialog_ColourPicker
            GUI
            SOURCES
            gui/tst_Dialog_ColourPicker.cpp
            "${CMAKE_SOURCE_DIR}/src/dialogs/ColourPickerDialog.cpp"
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_Dialog_LogSession
            GUI
            SOURCES
            gui/tst_Dialog_LogSession.cpp
            "${CMAKE_SOURCE_DIR}/src/dialogs/LogSessionDialog.cpp"
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_MdiTabs
            GUI
            SOURCES
            gui/tst_MdiTabs.cpp
            "${CMAKE_SOURCE_DIR}/src/WorldChildWindow.h"
            "${CMAKE_SOURCE_DIR}/src/MdiTabs.cpp"
            LABELS
            gui
            TIMEOUT
            20)
endif ()
