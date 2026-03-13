/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: InfoTypeData.inc
 * Role: Canonical GetInfo/info type descriptions included by InfoTypesMetadata.
 */

#ifndef QMUD_INFOTYPEDATA_INC
#define QMUD_INFOTYPEDATA_INC

#ifdef QMUD_INCLUDE_INFO_TYPE_DATA_ROWS

//   (strings - configuration)

{1, "Server name"}, {2, "World name"}, {3, "Character name"}, {4, "Send to world - file preamble"},
    {5, "Send to world - file postamble"}, {6, "Send to world - line preamble"},
    {7, "Send to world - line postamble"}, {8, "World notes"}, {9, "Sound on new activity"},
    {10, "Scripts editor"}, {11, "Log file preamble"}, {12, "Log file postamble"},
    {13, "Log file line preamble - player input"}, {14, "Log file line preamble - notes"},
    {15, "Log file line preamble - MUD output"}, {16, "Log file line postamble - player input"},
    {17, "Log file line postamble - notes"}, {18, "Log file line postamble - MUD output"},
    {19, "Speed Walk Filler"}, {20, "Output window font name"}, {21, "Speed walk prefix"},
    {22, "Text sent on connecting"}, {23, "Input font name"}, {24, "Paste to world - file preamble"},
    {25, "Paste to world - file postamble"}, {26, "Paste to world - line preamble"},
    {27, "Paste to world - line postamble"}, {28, "Scripting language"},
    {29, "Script function for world open"}, {30, "Script function for world close"},
    {31, "Script function for world connect"}, {32, "Script function for world disconnect"},
    {33, "Script function for world get focus"}, {34, "Script function for world lose focus"},
    {35, "Script file name"}, {36, "Scripting prefix"}, {37, "Auto-say string"}, {38, "Auto-say override"},
    {39, "Tab-completion defaults"}, {40, "Auto-log file name"}, {41, "Recall window - line preamble"},
    {42, "Terminal ID (telnet negotiation)"}, {43, "Mapping failure message"},
    {44, "Script function for MXP starting up"}, {45, "Script function for MXP closing down"},
    {46, "Script function for MXP error"}, {47, "Script function for MXP tag open"},
    {48, "Script function for MXP tag close"}, {49, "Script function for MXP variable set"},
    {50, "Sound to play for beeps"},

    // (strings - calculated at runtime)

    {51, "Current log file name"}, {52, "Last \"immediate\" script expression"},
    {53, "Current status line message"}, {54, "World file pathname"}, {55, "World title"},
    {56, "QMud application path name"}, {57, "World files default path (directory)"},
    {58, "Log files default path (directory)"}, {59, "Script files default path (directory)"},
    {60, "Plugin files default path (directory)"}, {61, "World IP address"}, {62, "Proxy server IP address"},
    {63, "Host name (name of this PC)"}, {64, "Current directory path"},
    {65, "Script function for world save"}, {66, "QMud application directory"}, {67, "World file directory"},
    {68, "Startup directory"}, {69, "Translation file"}, {70, "Locale"}, {71, "Fixed Pitch Font"},
    {72, "QMud version"}, {73, "QMud compilation date/time"}, {74, "Sounds directory"},
    {75, "Last telnet subnegotiation string received"}, {76, "Special font pathname"},
    {77, "OS Version - service pack"}, {78, "Foreground image filename"}, {79, "Background image filename"},
    {80, "LibPNG version number"}, {81, "LibPNG header version string"},
    {82, "Preferences database pathname"}, {83, "SQLite3 version"}, {84, "File browsing directory"},
    {85, "State files path (directory)"}, {86, "Word under mouse on mouse menu click"},
    {87, "Last command sent"}, {88, "Window title last set by SetTitle"},
    {89, "Window title last set by SetMainTitle"},

    // (booleans - calculated at runtime)

    {101, "No Echo"}, {102, "Debug incoming packets"}, {103, "Decompressing"}, {104, "MXP active"},
    {105, "Pueblo active"}, {106, "World close flag"}, {107, "Currently-connecting"},
    {108, "OK-to-disconnect"}, {109, "Trace"}, {110, "Script file changed"}, {111, "World file is modified"},
    {112, "Automapper active"}, {113, "World is active"}, {114, "Output window paused"},
    {115, "Localization active"}, {118, "Variables have changed"}, {119, "Script engine active"},
    {120, "Scroll bar visible"}, {121, "High-resolution timer available"}, {122, "SQLite3 is thread-safe"},

    // (numbers (longs) - calculated at runtime)

    {201, "Total lines received"}, {202, "Lines received but not yet seen (new lines)"},
    {203, "Total lines sent"}, {204, "Packets received"}, {205, "Packets sent"},
    {206, "Total uncompressed bytes received"}, {207, "Total compresssed bytes received"},
    {208, "MCCP protocol in use (0=none, 1 or 2)"}, {209, "MXP error count"}, {210, "MXP tags received"},
    {211, "MXP entities received"}, {212, "Output font height"}, {213, "Output font width"},
    {214, "Input font height"}, {215, "Input font width"}, {216, "Total bytes received"},
    {217, "Total bytes sent"}, {218, "Count of variables"}, {219, "Count of triggers"},
    {220, "Count of timers"}, {221, "Count of aliases"}, {222, "Count of queued commands"},
    {223, "Count of mapper items"}, {224, "Count of lines in output window"},
    {225, "Count of custom MXP elements"}, {226, "Count of custom MXP entities"}, {227, "Connect phase"},
    {228, "World IP address (as long)"}, {229, "Proxy server IP address (as long)"},
    {230, "Script execution depth"}, {231, "Log file size"}, {232, "High-performance timer"}, // double
    {233, "Trigger execution time"},                                                          // double
    {234, "Alias execution time"},                                                            // double
    {235, "Number of world windows"}, {236, "Command window start selection column"},
    {237, "Command window end selection column"}, {238, "World window display flags"},
    {239, "Source of current action"}, {240, "Average character width in output window"},
    {241, "Font height in output window"}, {242, "Number of lines received with bad UTF-8 in them"},
    {243, "Fixed Pitch Font Size"}, {244, "Count of triggers evaluated"}, {245, "Count of triggers matched"},
    {246, "Count of aliases evaluated"}, {247, "Count of aliases matched"}, {248, "Count of timers fired"},
    {249, "Main frame window height"}, {250, "Main frame window width"}, {251, "Main toolbar window height"},
    {252, "Main toolbar window width"}, {253, "Game toolbar window height"},
    {254, "Game toolbar window width"}, {255, "Activity toolbar window height"},
    {256, "Activity toolbar window width"}, {257, "Info bar window height"}, {258, "Info bar window width"},
    {259, "Status bar window height"}, {260, "Status bar window width"},
    {261, "World window non-client height"}, {262, "World window non-client width"},
    {263, "World window client height"}, {264, "World window client width"}, {265, "OS Version - major"},
    {266, "OS Version - minor"}, {267, "OS Version - build number"}, {268, "OS Version - platform ID"},
    {269, "Foreground image mode"}, {270, "Background image mode"}, {271, "Background colour"},
    {272, "Text rectangle - left"}, {273, "Text rectangle - top"}, {274, "Text rectangle - right"},
    {275, "Text rectangle - bottom"}, {276, "Text rectangle - border offset"},
    {277, "Text rectangle - border width"}, {278, "Text rectangle - outside colour"},
    {279, "Text rectangle - outside style"}, {280, "Output window client height"},
    {281, "Output window client width"}, {282, "Text rectangle - border colour"}, {283, "Mouse position - X"},
    {284, "Mouse position - Y"}, {285, "Output window available"}, {286, "Triggers matched this session"},
    {287, "Aliases matched this session"}, {288, "Timers fired this session"},
    {289, "Last line with IAC/GA or IAC/EOR in it"}, {290, "Actual text rectangle - left"},
    {291, "Actual text rectangle - top"}, {292, "Actual text rectangle - right"},
    {293, "Actual text rectangle - bottom"}, {294, "State of keyboard modifiers"},
    {295, "Times output window redrawn"}, {296, "Output window scroll bar position"},
    {297, "High-resolution timer frequency"}, {298, "SQLite3 version number"}, {299, "ANSI code-page number"},
    {300, "OEM code-page number"},

    // (dates - calculated at runtime)

    {301, "Time connected"}, {302, "Time log file was last flushed to disk"},
    {303, "When script file was last modified"}, {304, "Time now"}, {305, "When client started executing"},
    {306, "When this world was created/opened"},

    // more numbers

    {310, "Newlines received"},

    {0, ""}, // end of table marker

#endif // QMUD_INCLUDE_INFO_TYPE_DATA_ROWS

#endif // QMUD_INFOTYPEDATA_INC
