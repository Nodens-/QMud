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
    # Shared support supplies startup configuration and silent output even for direct binary launches.
    target_link_libraries(${target_name}
            PRIVATE
            Qt6::Core
            Qt6::Test
            qmud_app_core
            ${QMUD_QTEST_LIBRARIES}
            qmud_test_support)
    target_include_directories(${target_name}
            PRIVATE
            "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_SOURCE_DIR}/src/helpers"
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

# Link support objects into every test so their startup initializer cannot be discarded by archive extraction.
add_library(qmud_test_support OBJECT
        support/TestEnvironment.cpp
        support/TelnetCallbackSpy.cpp)
target_link_libraries(qmud_test_support PUBLIC Qt6::Core)
target_include_directories(qmud_test_support
        PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/support"
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/helpers")

qmud_add_qtest(tst_QTestSmoke
        SOURCES
        smoke/tst_QTestSmoke.cpp
        LABELS
        unit
        smoke
        TIMEOUT
        10)

qmud_add_qtest(tst_PluginStatePersistence
        SOURCES
        unit/tst_PluginStatePersistence.cpp
        LABELS
        unit)

qmud_add_qtest(tst_StringUtils
        SOURCES
        unit/tst_StringUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_EncodingUtils
        SOURCES
        unit/tst_EncodingUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_IndexedRingBuffer
        SOURCES
        unit/tst_IndexedRingBuffer.cpp
        LABELS
        unit)

qmud_add_qtest(tst_OutputWrapUtils
        SOURCES
        unit/tst_OutputWrapUtils.cpp
        LIBRARIES
        Qt6::Network
        Qt6::Gui
        LABELS
        unit)

qmud_add_qtest(tst_AccessibleTextUtils
        SOURCES
        unit/tst_AccessibleTextUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_PluginPathUtils
        SOURCES
        unit/tst_PluginPathUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_FileAssociations
        SOURCES
        unit/tst_FileAssociations.cpp
        LABELS
        unit)
target_compile_definitions(tst_FileAssociations
        PRIVATE
        QMUD_TEST_GENERATED_INFO_PLIST="${QMUD_MAC_INFO_PLIST_TEMPLATE}")

qmud_add_qtest(tst_HyperlinkActionUtils
        SOURCES
        unit/tst_HyperlinkActionUtils.cpp
        LIBRARIES
        Qt6::Network
        Qt6::Gui
        LABELS
        unit)

qmud_add_qtest(tst_MiniWindowUtils
        SOURCES
        unit/tst_MiniWindowUtils.cpp
        LIBRARIES
        Qt6::Network
        Qt6::Widgets
        LABELS
        unit)

qmud_add_qtest(tst_Environment
        SOURCES
        unit/tst_Environment.cpp
        LABELS
        unit)

qmud_add_qtest(tst_GuiSystemUtils
        GUI
        SOURCES
        unit/tst_GuiSystemUtils.cpp
        LABELS
        unit
        TIMEOUT
        10)

qmud_add_qtest(tst_NameGeneration
        SOURCES
        unit/tst_NameGeneration.cpp
        LABELS
        unit
        TIMEOUT
        10)

qmud_add_qtest(tst_WorldEditUtils
        GUI
        SOURCES
        unit/tst_WorldEditUtils.cpp
        LABELS
        unit
        TIMEOUT
        10)

qmud_add_qtest(tst_TimeFormatUtils
        SOURCES
        unit/tst_TimeFormatUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_TraceDispatchUtils
        SOURCES
        unit/tst_TraceDispatchUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_TtsEngine
        SOURCES
        unit/tst_TtsEngine.cpp
        LIBRARIES
        ${QMUD_QT_TEXTTOSPEECH_TARGET}
        LABELS
        unit)
target_compile_definitions(tst_TtsEngine
        PRIVATE
        QMUD_ENABLE_TEXT_TO_SPEECH=${QMUD_ENABLE_TEXT_TO_SPEECH_VALUE})

qmud_add_qtest(tst_NativePluginRegistry
        SOURCES
        unit/tst_NativePluginRegistry.cpp
        LABELS
        unit)

qmud_add_qtest(tst_MspProtocol
        SOURCES
        unit/tst_MspProtocol.cpp
        LABELS
        unit)

qmud_add_qtest(tst_WorldRuntime_Msp
        SOURCES
        integration/tst_WorldRuntime_Msp.cpp
        LABELS
        integration)

if (QMUD_ENABLE_LUA_SCRIPTING)
    qmud_add_qtest(tst_LuaApiExport
            GUI
            SOURCES
            unit/tst_LuaApiExport.cpp
            LABELS
            unit
            TIMEOUT
            20)

    qmud_add_qtest(tst_LuaExecution
            SOURCES
            unit/tst_LuaExecution.cpp
            LABELS
            unit)

    qmud_add_qtest(tst_LuaCallbackEngine
            GUI
            SOURCES
            unit/tst_LuaCallbackEngine.cpp
            LABELS
            unit
            TIMEOUT
            120)

    qmud_add_qtest(tst_LuaLuacomShim
            SOURCES
            unit/tst_LuaLuacomShim.cpp
            LIBRARIES
            ${LUA_LIBRARIES}
            ${QMUD_QT_TEXTTOSPEECH_TARGET}
            LABELS
            unit)
    if (TARGET Lua::Lua)
        target_link_libraries(tst_LuaLuacomShim PRIVATE Lua::Lua)
    endif ()
    if (DEFINED LUA_INCLUDE_DIR)
        target_include_directories(tst_LuaLuacomShim SYSTEM BEFORE PRIVATE ${LUA_INCLUDE_DIR})
    elseif (DEFINED LUA_INCLUDE_DIRS)
        target_include_directories(tst_LuaLuacomShim SYSTEM BEFORE PRIVATE ${LUA_INCLUDE_DIRS})
    endif ()
    target_compile_definitions(tst_LuaLuacomShim
            PRIVATE
            QMUD_ENABLE_TEXT_TO_SPEECH=${QMUD_ENABLE_TEXT_TO_SPEECH_VALUE})

endif ()

qmud_add_qtest(tst_UpdateCheckUtils
        SOURCES
        unit/tst_UpdateCheckUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_WorldSessionStateUtils
        GUI
        SOURCES
        unit/tst_WorldSessionStateUtils.cpp
        LIBRARIES
        Qt6::Network
        ${QMUD_ZLIB_TARGET}
        LABELS
        unit)

qmud_add_qtest(tst_WorldRuntime_OutputBufferSeal
        GUI
        SOURCES
        unit/tst_WorldRuntime_OutputBufferSeal.cpp
        LABELS
        unit)

qmud_add_qtest(tst_WorldRuntime_BlendPixel
        SOURCES
        unit/tst_WorldRuntime_BlendPixel.cpp
        LABELS
        unit)

qmud_add_qtest(tst_WorldSessionRestoreFlowUtils
        SOURCES
        unit/tst_WorldSessionRestoreFlowUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_ReloadUnit
        SOURCES
        unit/tst_ReloadUnit.cpp
        LABELS
        unit)

qmud_add_qtest(tst_AnsiSgrParseUtils
        SOURCES
        unit/tst_AnsiSgrParseUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_ColorUtils
        SOURCES
        unit/tst_ColorUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_NoteColourUtils
        SOURCES
        unit/tst_NoteColourUtils.cpp
        LIBRARIES
        Qt6::Gui
        LABELS
        unit)

qmud_add_qtest(tst_FontUtils
        GUI
        SOURCES
        unit/tst_FontUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_AcceleratorUtils
        GUI
        SOURCES
        unit/tst_AcceleratorUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_MainFrameMdiUtils
        GUI
        SOURCES
        unit/tst_MainFrameMdiUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_BraceMatch
        GUI
        SOURCES
        unit/tst_BraceMatch.cpp
        LABELS
        unit)

qmud_add_qtest(tst_WorldOptions
        SOURCES
        unit/tst_WorldOptions.cpp
        LIBRARIES
        Qt6::Gui
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
        LABELS
        unit)

qmud_add_qtest(tst_PluginCallbackCatalogUtils
        SOURCES
        unit/tst_PluginCallbackCatalogUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_MemoryImageDecodeCacheUtils
        GUI
        SOURCES
        unit/tst_MemoryImageDecodeCacheUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_LuaUtf8Utils
        SOURCES
        unit/tst_LuaUtf8Utils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_WorldRuntimeAttributeUtils
        SOURCES
        unit/tst_WorldRuntimeAttributeUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_SoundFileRoutingUtils
        SOURCES
        unit/tst_SoundFileRoutingUtils.cpp
        LIBRARIES
        ${QMUD_QT_MULTIMEDIA_TARGET}
        LABELS
        unit)
target_compile_definitions(tst_SoundFileRoutingUtils
        PRIVATE
        QMUD_ENABLE_SOUND=${QMUD_ENABLE_SOUND_VALUE})
set_property(TEST tst_SoundFileRoutingUtils APPEND PROPERTY ENVIRONMENT
        "QMUD_HOME=${CMAKE_BINARY_DIR}")

qmud_add_qtest(tst_SoundCacheUtils
        SOURCES
        unit/tst_SoundCacheUtils.cpp
        LABELS
        unit)

if (QMUD_ENABLE_SOUND_VALUE)
    qmud_add_qtest(tst_SoundPcmStream
            SOURCES unit/tst_SoundPcmStream.cpp
            LABELS unit)
    qmud_add_qtest(tst_WorldRuntime_SoundLifetime
            GUI
            SOURCES
            unit/tst_WorldRuntime_SoundLifetime.cpp
            LABELS
            unit
            TIMEOUT 60)
    set_property(TEST tst_WorldRuntime_SoundLifetime APPEND PROPERTY ENVIRONMENT
            "QMUD_HOME=${CMAKE_BINARY_DIR}")
endif ()

qmud_add_qtest(tst_StartTlsFallbackUtils
        SOURCES
        unit/tst_StartTlsFallbackUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_WorldRuleEnableUtils
        SOURCES
        unit/tst_WorldRuleEnableUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_PluginBroadcastSelectionUtils
        SOURCES
        unit/tst_PluginBroadcastSelectionUtils.cpp
        LABELS
        unit)

qmud_add_qtest(tst_DoubleMetaphone
        SOURCES
        unit/tst_DoubleMetaphone.cpp
        LABELS
        unit)

qmud_add_qtest(tst_AsciiArt
        SOURCES
        unit/tst_AsciiArt.cpp
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
        LIBRARIES
        ${QMUD_ZLIB_TARGET}
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_TelnetProcessor_Mxp
        SOURCES
        integration/tst_TelnetProcessor_Mxp.cpp
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_TelnetProcessor_Subnegotiation
        SOURCES
        integration/tst_TelnetProcessor_Subnegotiation.cpp
        LIBRARIES
        ${QMUD_ZLIB_TARGET}
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_TelnetProcessor_Encoding
        SOURCES
        integration/tst_TelnetProcessor_Encoding.cpp
        LIBRARIES
        ${QMUD_ZLIB_TARGET}
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_TelnetProcessor_Compression
        SOURCES
        integration/tst_TelnetProcessor_Compression.cpp
        LIBRARIES
        ${QMUD_ZLIB_TARGET}
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_TelnetProcessor_Malformed
        SOURCES
        integration/tst_TelnetProcessor_Malformed.cpp
        LIBRARIES
        ${QMUD_ZLIB_TARGET}
        LABELS
        integration
        regression
        TIMEOUT
        20)

qmud_add_qtest(tst_ReloadIntegration
        SOURCES
        integration/tst_ReloadIntegration.cpp
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_SingleInstanceIpc
        SOURCES
        integration/tst_SingleInstanceIpc.cpp
        LIBRARIES
        Qt6::Network
        LABELS
        integration
        TIMEOUT
        10)

qmud_add_qtest(tst_MushclientImportUtils
        SOURCES
        integration/tst_MushclientImportUtils.cpp
        LIBRARIES
        Qt6::Sql
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_LuaBridgeDispatcherIntegration
        SOURCES
        integration/tst_LuaBridgeDispatcherIntegration.cpp
        LABELS
        integration
        TIMEOUT
        90)

qmud_add_qtest(tst_WorldSocket_KeepAlive
        SOURCES
        integration/tst_WorldSocket_KeepAlive.cpp
        LIBRARIES
        Qt6::Network ${QMUD_ZLIB_TARGET}
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldRuntime_PluginLifecycle
        GUI
        SOURCES
        integration/tst_WorldRuntime_PluginLifecycle.cpp
        LIBRARIES
        Qt6::Network
        LABELS
        integration
        TIMEOUT
        120)
target_compile_definitions(tst_WorldRuntime_PluginLifecycle PRIVATE QMUD_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}")

qmud_add_qtest(tst_WorldObserverLifecycle
        GUI
        SOURCES
        gui/tst_WorldObserverLifecycle.cpp
        LABELS
        gui
        integration
        TIMEOUT
        30)

qmud_add_qtest(tst_WorldRuntime_MiniWindowGeometry
        GUI
        SOURCES
        integration/tst_WorldRuntime_MiniWindowGeometry.cpp
        LABELS
        integration
        TIMEOUT
        30)

qmud_add_qtest(tst_WorldRuntime_TextRectangle
        GUI
        SOURCES
        integration/tst_WorldRuntime_TextRectangle.cpp
        LABELS
        integration
        TIMEOUT
        30)

qmud_add_qtest(tst_WorldDocument_Load
        SOURCES
        integration/tst_WorldDocument_Load.cpp
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldDocument_Masks
        SOURCES
        integration/tst_WorldDocument_Masks.cpp
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldDocument_Includes
        SOURCES
        integration/tst_WorldDocument_Includes.cpp
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldDocument_RoundTrip
        SOURCES
        integration/tst_WorldDocument_RoundTrip.cpp
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldCommandProcessor_Speedwalk
        SOURCES
        integration/tst_WorldCommandProcessor_Speedwalk.cpp
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldCommandProcessor_Alias
        SOURCES
        integration/tst_WorldCommandProcessor_Alias.cpp
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldCommandProcessor_CommandStacking
        SOURCES
        integration/tst_WorldCommandProcessor_CommandStacking.cpp
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldCommandProcessor_Regex
        SOURCES
        integration/tst_WorldCommandProcessor_Regex.cpp
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldCommandProcessor_SendTargets
        SOURCES
        integration/tst_WorldCommandProcessor_SendTargets.cpp
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldCommandProcessor_Trigger
        GUI
        SOURCES
        integration/tst_WorldCommandProcessor_Trigger.cpp
        LABELS
        integration
        TIMEOUT
        120)

qmud_add_qtest(tst_WorldCommandProcessor_Queueing
        SOURCES
        integration/tst_WorldCommandProcessor_Queueing.cpp
        LABELS
        integration
        TIMEOUT
        20)

qmud_add_qtest(tst_WorldCommandProcessor_TimerAccounting
        GUI
        SOURCES
        integration/tst_WorldCommandProcessor_TimerAccounting.cpp
        LIBRARIES
        Qt6::Network
        LABELS
        integration
        TIMEOUT
        20)

if (QMUD_ENABLE_GUI_TESTS)
    qmud_add_qtest(tst_AppControllerTextCommands
            GUI
            SOURCES
            gui/tst_AppControllerTextCommands.cpp
            LABELS
            gui
            integration
            TIMEOUT
            20)

    qmud_add_qtest(tst_AppControllerFileOpen
            GUI
            SOURCES
            gui/tst_AppControllerFileOpen.cpp
            LABELS
            gui
            TIMEOUT
            10)

	qmud_add_qtest(tst_DialogUtils
			GUI
			SOURCES
			gui/tst_DialogUtils.cpp
            LABELS
            gui
            TIMEOUT
            10)

    qmud_add_qtest(tst_MainFrame_Actions
            GUI
            SOURCES
            gui/tst_MainFrame_Actions.cpp
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_MainFrame_WindowTiling
            GUI
            SOURCES
            gui/tst_MainFrame_WindowTiling.cpp
            LABELS
            integration
            TIMEOUT
            20)

    qmud_add_qtest(tst_MainFrame_StatusMessage
            GUI
            SOURCES
            gui/tst_MainFrame_StatusMessage.cpp
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_Dialog_WorldPreferences
            GUI
            SOURCES
            gui/tst_Dialog_WorldPreferences.cpp
            LABELS
            gui
            TIMEOUT
            20)
    set_property(TEST tst_Dialog_WorldPreferences APPEND PROPERTY ENVIRONMENT
            "QMUD_HOME=${CMAKE_BINARY_DIR}")

    qmud_add_qtest(tst_WorldView_Basic
            GUI
            SOURCES
            gui/tst_WorldView_Basic.cpp
            LABELS
            gui
            TIMEOUT
            120)
    qmud_add_qtest(tst_Dialog_ImportXml
            GUI
            SOURCES
            gui/tst_Dialog_ImportXml.cpp
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_Dialog_Plugins
            GUI
            SOURCES
            gui/tst_Dialog_Plugins.cpp
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_Dialog_PluginWizard
            GUI
            SOURCES
            gui/tst_Dialog_PluginWizard.cpp
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_Dialog_ColourPicker
            GUI
            SOURCES
            gui/tst_Dialog_ColourPicker.cpp
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_Dialog_LogSession
            GUI
            SOURCES
            gui/tst_Dialog_LogSession.cpp
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_Dialog_GlobalPreferencesUpdates
            GUI
            SOURCES
            gui/tst_Dialog_GlobalPreferencesUpdates.cpp
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_MdiTabs
            GUI
            SOURCES
            gui/tst_MdiTabs.cpp
            LABELS
            gui
            TIMEOUT
            20)

    qmud_add_qtest(tst_TabActivationHistory
            GUI
            SOURCES
            gui/tst_TabActivationHistory.cpp
            LABELS
            gui
            TIMEOUT
            20)
endif ()

qmud_add_qtest(tst_GmcpProtocol
        SOURCES unit/tst_GmcpProtocol.cpp
        LABELS unit)

qmud_add_qtest(tst_GmcpIntegration GUI
        SOURCES integration/tst_GmcpIntegration.cpp
        LABELS integration
        TIMEOUT 90)

set_property(TEST tst_GmcpIntegration APPEND PROPERTY ENVIRONMENT "QMUD_HOME=${CMAKE_BINARY_DIR}")
set_tests_properties(tst_GmcpIntegration PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(tst_GmcpIntegration PRIVATE QMUD_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}")
