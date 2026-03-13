/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: Accelerators.cpp
 * Role: Static accelerator map definitions used by the command layer to resolve keyboard input into QMud actions.
 */

#include "Accelerators.h"
#include "CommandIds.h"
#include "ResourceIds.h"

namespace
{
	constexpr CommandIdMapping  kCommandIdMappingEndMarker{0, nullptr};
	constexpr VirtualKeyMapping kVirtualKeyMappingEndMarker{0, nullptr};
} // namespace

const CommandIdMapping kCommandIdMappingTable[] = {
    {commandIdValue(CommandId::AppAbout),                                 "About"                       },
    {resourceIdValue(ResourceId::AltA),                                   "AltA"                        },
    {resourceIdValue(ResourceId::AltB),                                   "AltB"                        },
    {resourceIdValue(ResourceId::AltJ),                                   "AltJ"                        },
    {resourceIdValue(ResourceId::AltK),                                   "AltK"                        },
    {resourceIdValue(ResourceId::AltL),                                   "AltL"                        },
    {resourceIdValue(ResourceId::AltM),                                   "AltM"                        },
    {resourceIdValue(ResourceId::AltN),                                   "AltN"                        },
    {resourceIdValue(ResourceId::AltO),                                   "AltO"                        },
    {resourceIdValue(ResourceId::AltP),                                   "AltP"                        },
    {resourceIdValue(ResourceId::AltQ),                                   "AltQ"                        },
    {resourceIdValue(ResourceId::AltR),                                   "AltR"                        },
    {resourceIdValue(ResourceId::AltS),                                   "AltS"                        },
    {resourceIdValue(ResourceId::AltT),                                   "AltT"                        },
    {resourceIdValue(ResourceId::AltU),                                   "AltU"                        },
    {resourceIdValue(ResourceId::AltV),                                   "AltV"                        },
    {resourceIdValue(ResourceId::AltX),                                   "AltX"                        },
    {resourceIdValue(ResourceId::AltY),                                   "AltY"                        },
    {resourceIdValue(ResourceId::AltZ),                                   "AltZ"                        },
    {resourceIdValue(ResourceId::KeysActivatecommandview),                "ActivateInputArea"           },
    {resourceIdValue(ResourceId::DisplayActivitylist),                    "ActivityList"                },
    {resourceIdValue(ResourceId::ViewActivitytoolbar),                    "ActivityToolbar"             },
    {resourceIdValue(ResourceId::AltDownarrow),                           "AltDownArrow"                },
    {resourceIdValue(ResourceId::AltUparrow),                             "AltUpArrow"                  },
    {resourceIdValue(ResourceId::ViewAlwaysontop),                        "AlwaysOnTop"                 },
    {resourceIdValue(ResourceId::EditAsciiart),                           "ASCIIart"                    },
    {commandIdValue(CommandId::WindowArrange),                            "ArrangeIcons"                },
    {resourceIdValue(ResourceId::ConnectionAutoconnect),                  "AutoConnect"                 },
    {resourceIdValue(ResourceId::GameAutosay),                            "AutoSay"                     },
    {resourceIdValue(ResourceId::ConvertBase64encode),                    "Base64Encode"                },
    {resourceIdValue(ResourceId::ConvertBase64decode),                    "Base64Decode"                },
    {resourceIdValue(ResourceId::DisplayBookmarkselection),               "BookmarkSelection"           },
    {resourceIdValue(ResourceId::HelpBugreportsuggestion),                "BugReports"                  },
    {commandIdValue(CommandId::WindowCascade),                            "CascadeWindows"              },
    {resourceIdValue(ResourceId::GameChatsessions),                       "ChatSessions"                },
    {resourceIdValue(ResourceId::DisplayClearCommandHistory),             "ClearCommandHistory"         },
    {resourceIdValue(ResourceId::DisplayClearOutput),                     "ClearOutput"                 },
    {commandIdValue(CommandId::FileClose),                                "Close"                       },
    {resourceIdValue(ResourceId::WindowCloseallnotepadwindows),           "CloseAllNotepadWindows"      },
    {resourceIdValue(ResourceId::EditColourpicker),                       "ColourPicker"                },
    {resourceIdValue(ResourceId::CommandEnd),                             "CommandEnd"                  },
    {resourceIdValue(ResourceId::GameCommandhistory),                     "CommandHistory"              },
    {resourceIdValue(ResourceId::CommandHome),                            "CommandHome"                 },
    {resourceIdValue(ResourceId::GameConfigureAliases),                   "ConfigureAliases"            },
    {resourceIdValue(ResourceId::GameConfigureAutosay),                   "ConfigureAutosay"            },
    {resourceIdValue(ResourceId::GameConfigureChat),                      "ConfigureChat"               },
    {resourceIdValue(ResourceId::GameConfigureColours),                   "ConfigureColours"            },
    {resourceIdValue(ResourceId::GameConfigureCommands),                  "ConfigureCommands"           },
    {resourceIdValue(ResourceId::GameConfigureCustomColours),             "ConfigureCustomColours"      },
    //{ resourceIdValue(ResourceId::GameConfigureHighlighting),			       "ConfigureHighlighting"					  },
    {resourceIdValue(ResourceId::GameConfigureKeypad),                    "ConfigureKeypad"             },
    {resourceIdValue(ResourceId::GameConfigureLogging),                   "ConfigureLogging"            },
    {resourceIdValue(ResourceId::GameConfigureMacros),                    "ConfigureMacros"             },
    {resourceIdValue(ResourceId::GameConfigureMudaddress),                "ConfigureMudaddress"         },
    {resourceIdValue(ResourceId::GameConfigureMxppueblo),                 "ConfigureMxpPueblo"          },
    {resourceIdValue(ResourceId::GameConfigureNameAndPassword),           "ConfigureNameAndPassword"    },
    {resourceIdValue(ResourceId::GameConfigureNotes),                     "ConfigureNotes"              },
    {resourceIdValue(ResourceId::GameConfigureOutput),                    "ConfigureOutput"             },
    {resourceIdValue(ResourceId::GameConfigurePastetoworld),              "ConfigurePasteToWorld"       },
    {resourceIdValue(ResourceId::GameConfigurePrinting),                  "ConfigurePrinting"           },
    {resourceIdValue(ResourceId::GameConfigureScripting),                 "ConfigureScripting"          },
    {resourceIdValue(ResourceId::GameConfigureSendfile),                  "ConfigureSendFile"           },
    {resourceIdValue(ResourceId::GameConfigureTimers),                    "ConfigureTimers"             },
    {resourceIdValue(ResourceId::GameConfigureTriggers),                  "ConfigureTriggers"           },
    {resourceIdValue(ResourceId::GameConfigureVariables),                 "ConfigureVariables"          },
    {resourceIdValue(ResourceId::ConnectionConnect),                      "Connect"                     },
    {resourceIdValue(ResourceId::ConnectDisconnect),                      "Connect_Or_Reconnect"        },
    {resourceIdValue(ResourceId::ConnectionConnecttoallopenworlds),       "ConnectToAllOpenWorlds"      },
    {resourceIdValue(ResourceId::ConnectionConnecttoworldsinstartuplist), "ConnectToWorldsInStartupList"},
    {commandIdValue(CommandId::ContextHelp),                              "ContextHelp"                 },
    {resourceIdValue(ResourceId::ConvertConverthtmlspecial),              "ConvertHTMLspecial"          },
    {resourceIdValue(ResourceId::EditConvertclipboardforumcodes),         "ConvertForumCodes"           },
    {commandIdValue(CommandId::EditCopy),                                 "Copy"                        },
    {resourceIdValue(ResourceId::EditCopyashtml),                         "CopyAsHTML"                  },
    {resourceIdValue(ResourceId::FileCtrlN),                              "CtrlN"                       },
    {resourceIdValue(ResourceId::FileCtrlP),                              "CtrlP"                       },
    {resourceIdValue(ResourceId::EditCtrlZ),                              "CtrlZ"                       },
    {commandIdValue(CommandId::EditCut),                                  "Cut"                         },
    {resourceIdValue(ResourceId::EditDebugincomingpackets),               "DebugIncomingPackets"        },
    {resourceIdValue(ResourceId::DebugWorldInput),                        "DebugWorldInput"             },
    {resourceIdValue(ResourceId::InputDiscardqueuedcommands),             "DiscardQueuedCommands"       },
    {resourceIdValue(ResourceId::ConnectionDisconnect),                   "Disconnect"                  },
    {resourceIdValue(ResourceId::GameDomappercomment),                    "DoMapperComment"             },
    {resourceIdValue(ResourceId::GameDomapperspecial),                    "DoMapperSpecial"             },
    {resourceIdValue(ResourceId::ConvertDostounix),                       "DosToUnix"                   },
    {resourceIdValue(ResourceId::GameEast),                               "East"                        },
    {resourceIdValue(ResourceId::GameEditscriptfile),                     "EditScriptFile"              },
    {resourceIdValue(ResourceId::TestEnd),                                "End"                         },
    {resourceIdValue(ResourceId::GameExamine),                            "Examine"                     },
    {commandIdValue(CommandId::AppExit),                                  "ExitClient"                  },
    {resourceIdValue(ResourceId::DisplayFind),                            "Find"                        },
    {resourceIdValue(ResourceId::DisplayFindagain),                       "FindAgain"                   },
    {resourceIdValue(ResourceId::EditFliptonotepad),                      "FlipToNotepad"               },
    {resourceIdValue(ResourceId::HelpForum),                              "Forum"                       },
    {resourceIdValue(ResourceId::DisplayFreezeoutput),                    "FreezeOutput"                },
    {resourceIdValue(ResourceId::ViewFullscreenmode),                     "FullScreenMode"              },
    {resourceIdValue(ResourceId::GameFunctionslist),                      "FunctionList"                },
    {resourceIdValue(ResourceId::HelpFunctionswebpage),                   "FunctionsWebPage"            },
    {resourceIdValue(ResourceId::HelpGettingstarted),                     "GettingStarted"              },
    {resourceIdValue(ResourceId::EditGeneratecharactername),              "GenerateCharacterName"       },
    {resourceIdValue(ResourceId::EditGenerateuniqueid),                   "GenerateUniqueID"            },
    {resourceIdValue(ResourceId::FilePreferences),                        "GlobalPreferences"           },
    {resourceIdValue(ResourceId::DisplayGotobookmark),                    "GoToBookmark"                },
    {resourceIdValue(ResourceId::DisplayGotoline),                        "GoToLine"                    },
    {resourceIdValue(ResourceId::EditGotomatchingbrace),                  "GoToMatchingBrace"           },
    {resourceIdValue(ResourceId::DisplayGotourl),                         "GoToURL"                     },
    {commandIdValue(CommandId::Help),                                     "Help"                        },
    {commandIdValue(CommandId::HelpIndex),                                "HelpIndex"                   },
    {resourceIdValue(ResourceId::HelpContents),                           "HelpContents"                },
    {resourceIdValue(ResourceId::DisplayHighlightphrase),                 "HighlightWord"               },
    {resourceIdValue(ResourceId::DisplayMultilinetrigger),                "MakeMultiLineTrigger"        },
    {resourceIdValue(ResourceId::GameImmediate),                          "Immediate"                   },
    {resourceIdValue(ResourceId::FileImport),                             "Import"                      },
    {resourceIdValue(ResourceId::GameConfigureInfo),                      "Info"                        },
    {resourceIdValue(ResourceId::ViewInfobar),                            "InfoBar"                     },
    {resourceIdValue(ResourceId::InputGlobalchange),                      "InputGlobalChange"           },
    {resourceIdValue(ResourceId::EditInsertdatetime),                     "InsertDateTime"              },
    {resourceIdValue(ResourceId::InputKeyname),                           "KeyName"                     },
    {resourceIdValue(ResourceId::Keypad0),                                "Keypad0"                     },
    {resourceIdValue(ResourceId::Keypad1),                                "Keypad1"                     },
    {resourceIdValue(ResourceId::Keypad2),                                "Keypad2"                     },
    {resourceIdValue(ResourceId::Keypad3),                                "Keypad3"                     },
    {resourceIdValue(ResourceId::Keypad4),                                "Keypad4"                     },
    {resourceIdValue(ResourceId::Keypad5),                                "Keypad5"                     },
    {resourceIdValue(ResourceId::Keypad6),                                "Keypad6"                     },
    {resourceIdValue(ResourceId::Keypad7),                                "Keypad7"                     },
    {resourceIdValue(ResourceId::Keypad8),                                "Keypad8"                     },
    {resourceIdValue(ResourceId::Keypad9),                                "Keypad9"                     },
    {resourceIdValue(ResourceId::KeypadDash),                             "KeypadDash"                  },
    {resourceIdValue(ResourceId::KeypadDot),                              "KeypadDot"                   },
    {resourceIdValue(ResourceId::KeypadPlus),                             "KeypadPlus"                  },
    {resourceIdValue(ResourceId::KeypadSlash),                            "KeypadSlash"                 },
    {resourceIdValue(ResourceId::KeypadStar),                             "KeypadStar"                  },
    {resourceIdValue(ResourceId::CompleteFunction),                       "CompleteFunction"            },
    {resourceIdValue(ResourceId::CtrlKeypad0),                            "CtrlKeypad0"                 },
    {resourceIdValue(ResourceId::CtrlKeypad1),                            "CtrlKeypad1"                 },
    {resourceIdValue(ResourceId::CtrlKeypad2),                            "CtrlKeypad2"                 },
    {resourceIdValue(ResourceId::CtrlKeypad3),                            "CtrlKeypad3"                 },
    {resourceIdValue(ResourceId::CtrlKeypad4),                            "CtrlKeypad4"                 },
    {resourceIdValue(ResourceId::CtrlKeypad5),                            "CtrlKeypad5"                 },
    {resourceIdValue(ResourceId::CtrlKeypad6),                            "CtrlKeypad6"                 },
    {resourceIdValue(ResourceId::CtrlKeypad7),                            "CtrlKeypad7"                 },
    {resourceIdValue(ResourceId::CtrlKeypad8),                            "CtrlKeypad8"                 },
    {resourceIdValue(ResourceId::CtrlKeypad9),                            "CtrlKeypad9"                 },
    {resourceIdValue(ResourceId::CtrlKeypadDash),                         "CtrlKeypadDash"              },
    {resourceIdValue(ResourceId::CtrlKeypadDot),                          "CtrlKeypadDot"               },
    {resourceIdValue(ResourceId::CtrlKeypadPlus),                         "CtrlKeypadPlus"              },
    {resourceIdValue(ResourceId::CtrlKeypadSlash),                        "CtrlKeypadSlash"             },
    {resourceIdValue(ResourceId::CtrlKeypadStar),                         "CtrlKeypadStar"              },
    {resourceIdValue(ResourceId::KeysEscape),                             "KeysEscape"                  },
    {resourceIdValue(ResourceId::KeysTab),                                "KeysTab"                     },
    {resourceIdValue(ResourceId::TestLinedown),                           "LineDown"                    },
    {resourceIdValue(ResourceId::TestLineup),                             "LineUp"                      },
    {resourceIdValue(ResourceId::FileLogsession),                         "LogSession"                  },
    {resourceIdValue(ResourceId::GameLook),                               "Look"                        },
    {resourceIdValue(ResourceId::MacroCtrlF1),                            "MacroCtrlF1"                 },
    {resourceIdValue(ResourceId::MacroCtrlF10),                           "MacroCtrlF10"                },
    {resourceIdValue(ResourceId::MacroCtrlF11),                           "MacroCtrlF11"                },
    {resourceIdValue(ResourceId::MacroCtrlF12),                           "MacroCtrlF12"                },
    {resourceIdValue(ResourceId::MacroCtrlF2),                            "MacroCtrlF2"                 },
    {resourceIdValue(ResourceId::MacroCtrlF3),                            "MacroCtrlF3"                 },
    {resourceIdValue(ResourceId::MacroCtrlF5),                            "MacroCtrlF5"                 },
    {resourceIdValue(ResourceId::MacroCtrlF6),                            "MacroCtrlF6"                 },
    {resourceIdValue(ResourceId::MacroCtrlF7),                            "MacroCtrlF7"                 },
    {resourceIdValue(ResourceId::MacroCtrlF8),                            "MacroCtrlF8"                 },
    {resourceIdValue(ResourceId::MacroCtrlF9),                            "MacroCtrlF9"                 },
    {resourceIdValue(ResourceId::MacroF1),                                "MacroF1"                     },
    {resourceIdValue(ResourceId::MacroF10),                               "MacroF10"                    },
    {resourceIdValue(ResourceId::MacroF11),                               "MacroF11"                    },
    {resourceIdValue(ResourceId::MacroF12),                               "MacroF12"                    },
    {resourceIdValue(ResourceId::MacroF2),                                "MacroF2"                     },
    {resourceIdValue(ResourceId::MacroF3),                                "MacroF3"                     },
    {resourceIdValue(ResourceId::MacroF4),                                "MacroF4"                     },
    {resourceIdValue(ResourceId::MacroF5),                                "MacroF5"                     },
    {resourceIdValue(ResourceId::MacroF6),                                "MacroF6"                     },
    {resourceIdValue(ResourceId::MacroF7),                                "MacroF7"                     },
    {resourceIdValue(ResourceId::MacroF8),                                "MacroF8"                     },
    {resourceIdValue(ResourceId::MacroF9),                                "MacroF9"                     },
    {resourceIdValue(ResourceId::MacroShiftF1),                           "MacroShiftF1"                },
    {resourceIdValue(ResourceId::MacroShiftF10),                          "MacroShiftF10"               },
    {resourceIdValue(ResourceId::MacroShiftF11),                          "MacroShiftF11"               },
    {resourceIdValue(ResourceId::MacroShiftF12),                          "MacroShiftF12"               },
    {resourceIdValue(ResourceId::MacroShiftF2),                           "MacroShiftF2"                },
    {resourceIdValue(ResourceId::MacroShiftF3),                           "MacroShiftF3"                },
    {resourceIdValue(ResourceId::MacroShiftF4),                           "MacroShiftF4"                },
    {resourceIdValue(ResourceId::MacroShiftF5),                           "MacroShiftF5"                },
    {resourceIdValue(ResourceId::MacroShiftF6),                           "MacroShiftF6"                },
    {resourceIdValue(ResourceId::MacroShiftF7),                           "MacroShiftF7"                },
    {resourceIdValue(ResourceId::MacroShiftF8),                           "MacroShiftF8"                },
    {resourceIdValue(ResourceId::MacroShiftF9),                           "MacroShiftF9"                },
    {resourceIdValue(ResourceId::GameMapper),                             "Mapper"                      },
    {resourceIdValue(ResourceId::GameMinimiseprogram),                    "MinimiseProgram"             },
    {resourceIdValue(ResourceId::WindowMinimize),                         "Minimize"                    },
    {resourceIdValue(ResourceId::WindowMaximize),                         "Maximize"                    },
    {resourceIdValue(ResourceId::WindowRestore),                          "Restore"                     },
    {resourceIdValue(ResourceId::HelpMudlists),                           "MudLists"                    },
    {resourceIdValue(ResourceId::KeysNextcommand),                        "NextCommand"                 },
    {commandIdValue(CommandId::NextPane),                                 "NextPane"                    },
    {commandIdValue(CommandId::FileNew),                                  "New"                         },
    {commandIdValue(CommandId::WindowNew),                                "NewWindow"                   },
    {resourceIdValue(ResourceId::DisplayNocommandecho),                   "NoCommandEcho"               },
    {resourceIdValue(ResourceId::GameNorth),                              "North"                       },
    {resourceIdValue(ResourceId::EditNotesworkarea),                      "NotesWorkArea"               },
    {resourceIdValue(ResourceId::FileOpenworldsinstartuplist),            "OpenWorldsInStartupList"     },
    {commandIdValue(CommandId::FileOpen),                                 "Open"                        },
    {resourceIdValue(ResourceId::TestPagedown),                           "PageDown"                    },
    {resourceIdValue(ResourceId::TestPageup),                             "Pageup"                      },
    {commandIdValue(CommandId::EditPaste),                                "Paste"                       },
    {resourceIdValue(ResourceId::GamePastefile),                          "PasteFile"                   },
    {resourceIdValue(ResourceId::EditPastetomush),                        "PasteToMush"                 },
    {resourceIdValue(ResourceId::FilePluginwizard),                       "PluginWizard"                },
    {resourceIdValue(ResourceId::FilePlugins),                            "Plugins"                     },
    {resourceIdValue(ResourceId::GamePreferences),                        "Preferences"                 },
    {resourceIdValue(ResourceId::KeysPrevcommand),                        "PreviousCommand"             },
    {resourceIdValue(ResourceId::FilePrintWorld),                         "Print"                       },
    {commandIdValue(CommandId::FilePrintSetup),                           "PrintSetup"                  },
    {resourceIdValue(ResourceId::ConnectionQuickConnect),                 "QuickConnect"                },
    {resourceIdValue(ResourceId::ActionsQuit),                            "Quit"                        },
    {resourceIdValue(ResourceId::ConvertQuoteforumcodes),                 "QuoteForumCodes"             },
    {resourceIdValue(ResourceId::ConvertQuotelines),                      "QuoteLines"                  },
    {resourceIdValue(ResourceId::DisplayRecalltext),                      "RecallText"                  },
    {resourceIdValue(ResourceId::ConnectionReconnectondisconnect),        "ReconnectOnDisconnect"       },
    {resourceIdValue(ResourceId::FileReloaddefaults),                     "ReloadDefaults"              },
    {resourceIdValue(ResourceId::EditReloadnamesfile),                    "ReloadNamesFile"             },
    {resourceIdValue(ResourceId::GameReloadScriptFile),                   "ReloadScriptFile"            },
    {resourceIdValue(ResourceId::RepeatLastCommand),                      "RepeatLastCommand"           },
    {resourceIdValue(ResourceId::RepeatLastWord),                         "RepeatLastWord"              },
    {resourceIdValue(ResourceId::GameResetalltimers),                     "ResetAllTimers"              },
    {resourceIdValue(ResourceId::ViewResetToolbars),                      "ResetToolbars"               },
    {commandIdValue(CommandId::FileSave),                                 "Save"                        },
    {commandIdValue(CommandId::FileSaveAs),                               "SaveAs"                      },
    {resourceIdValue(ResourceId::FileSaveselection),                      "SaveSelection"               },
    {commandIdValue(CommandId::EditSelectAll),                            "SelectAll"                   },
    {resourceIdValue(ResourceId::EditSelecttomatchingbrace),              "SelectMatchingBrace"         },
    {resourceIdValue(ResourceId::DisplaySendmailto),                      "SendMailTo"                  },
    {resourceIdValue(ResourceId::GameSendtoallworlds),                    "SendToAllWorlds"             },
    {resourceIdValue(ResourceId::EditSendtocommandwindow),                "SendToCommandWindow"         },
    {resourceIdValue(ResourceId::EditSendtoimmediate),                    "SendToScript"                },
    {resourceIdValue(ResourceId::EditSendtoworld),                        "SendToWorld"                 },
    {resourceIdValue(ResourceId::GameSouth),                              "South"                       },
    {resourceIdValue(ResourceId::EditSpellcheck),                         "SpellCheck"                  },
    {resourceIdValue(ResourceId::TestStart),                              "Start"                       },
    {resourceIdValue(ResourceId::DisplayStopsoundplaying),                "StopSoundPlaying"            },
    {resourceIdValue(ResourceId::GameTake),                               "Take"                        },
    //{ resourceIdValue(ResourceId::GameTestcommand),							"TestCommand"							  },
    //{ resourceIdValue(ResourceId::GameTesttrigger),							"TestTrigger"							  },
    {resourceIdValue(ResourceId::DisplayTextattributes),                  "TextAttributes"              },
    {commandIdValue(CommandId::WindowTileHorz),                           "TileWindows"                 },
    {commandIdValue(CommandId::WindowTileVert),                           "TileWindowsVertically"       },
    {resourceIdValue(ResourceId::HelpTipoftheday),                        "TipOfTheDay"                 },
    {resourceIdValue(ResourceId::GameTrace),                              "Trace"                       },
    {commandIdValue(CommandId::EditUndo),                                 "Undo"                        },
    {resourceIdValue(ResourceId::UnconvertConverthtmlspecial),            "UnconvertHTMLspecial"        },
    {resourceIdValue(ResourceId::ConvertUnixtodos),                       "UnixToDos"                   },
    {resourceIdValue(ResourceId::GameUp),                                 "Up"                          },
    {commandIdValue(CommandId::HelpUsing),                                "UsingHelp"                   },
    {commandIdValue(CommandId::ViewToolbar),                              "ViewToolbar"                 },
    {resourceIdValue(ResourceId::ViewGameToolbar),                        "ViewWorldToolbar"            },
    {commandIdValue(CommandId::ViewStatusBar),                            "ViewStatusbar"               },
    {resourceIdValue(ResourceId::WebPage),                                "WebPage"                     },
    {resourceIdValue(ResourceId::GameWest),                               "West"                        },
    {resourceIdValue(ResourceId::GameWhisper),                            "Whisper"                     },
    {resourceIdValue(ResourceId::FileWinsock),                            "WindowsSocketInformation"    },
    {resourceIdValue(ResourceId::EditWordcount),                          "WordCount"                   },
    {resourceIdValue(ResourceId::BtnWorldsWorld1),                        "World1"                      },
    {resourceIdValue(ResourceId::BtnWorldsWorld2),                        "World2"                      },
    {resourceIdValue(ResourceId::BtnWorldsWorld3),                        "World3"                      },
    {resourceIdValue(ResourceId::BtnWorldsWorld4),                        "World4"                      },
    {resourceIdValue(ResourceId::BtnWorldsWorld5),                        "World5"                      },
    {resourceIdValue(ResourceId::BtnWorldsWorld6),                        "World6"                      },
    {resourceIdValue(ResourceId::BtnWorldsWorld7),                        "World7"                      },
    {resourceIdValue(ResourceId::BtnWorldsWorld8),                        "World8"                      },
    {resourceIdValue(ResourceId::BtnWorldsWorld9),                        "World9"                      },
    {resourceIdValue(ResourceId::BtnWorldsWorld0),                        "World10"                     },

    {resourceIdValue(ResourceId::ConvertRemoveendoflines),                "WrapLines"                   },
    {resourceIdValue(ResourceId::GameWraplines),                          "WrapOutput"                  },

    // Notepad

    {commandIdValue(CommandId::FilePrint),                                "PrintNotepad"                },
    {commandIdValue(CommandId::FilePrintPreview),                         "PrintPreview"                },
    {resourceIdValue(ResourceId::EditGoto),                               "GoToNotepadLine"             },
    {resourceIdValue(ResourceId::EditRefreshrecalleddata),                "RefreshRecalledData"         },
    {resourceIdValue(ResourceId::SearchFind),                             "FindNotepad"                 },
    {resourceIdValue(ResourceId::SearchFindnext),                         "FindNextNotepad"             },
    {resourceIdValue(ResourceId::SearchGlobalreplace),                    "GlobalReplace"               },
    {resourceIdValue(ResourceId::SearchReplace),                          "ReplaceNotepad"              },
    {resourceIdValue(ResourceId::ConvertMactodos),                        "MacToDos"                    },
    {resourceIdValue(ResourceId::ConvertDostomac),                        "DosToMac"                    },
    {resourceIdValue(ResourceId::ConvertUppercase),                       "UpperCase"                   },
    {resourceIdValue(ResourceId::ConvertLowercase),                       "LowerCase"                   },
    {resourceIdValue(ResourceId::ConvertRemoveextrablanks),               "RemoveBlanks"                },

    // Activity view

    {resourceIdValue(ResourceId::PopupFileClose),                         "ActivityViewCloseWorld"      },
    {resourceIdValue(ResourceId::PopupFileSave),                          "ActivityViewSaveWorld"       },
    {resourceIdValue(ResourceId::PopupSaveworlddetailsas),                "ActivityViewSaveWorldAs"     },

    kCommandIdMappingEndMarker, // end of table marker
}; // End of kCommandIdMappingTable table

/* Windows-compatible virtual-key values used for API/persistence compatibility. */
constexpr quint16       QMUD_VK_ACCEPT     = 0x1E;
constexpr quint16       QMUD_VK_ADD        = 0x6B;
constexpr quint16       QMUD_VK_APPS       = 0x5D;
constexpr quint16       QMUD_VK_ATTN       = 0xF6;
constexpr quint16       QMUD_VK_BACK       = 0x08;
constexpr quint16       QMUD_VK_CANCEL     = 0x03;
constexpr quint16       QMUD_VK_CAPITAL    = 0x14;
constexpr quint16       QMUD_VK_CLEAR      = 0x0C;
constexpr quint16       QMUD_VK_CONTROL    = 0x11;
constexpr quint16       QMUD_VK_CONVERT    = 0x1C;
constexpr quint16       QMUD_VK_CRSEL      = 0xF7;
constexpr quint16       QMUD_VK_DECIMAL    = 0x6E;
constexpr quint16       QMUD_VK_DELETE     = 0x2E;
constexpr quint16       QMUD_VK_DIVIDE     = 0x6F;
constexpr quint16       QMUD_VK_DOWN       = 0x28;
constexpr quint16       QMUD_VK_END        = 0x23;
constexpr quint16       QMUD_VK_EREOF      = 0xF9;
constexpr quint16       QMUD_VK_ESCAPE     = 0x1B;
constexpr quint16       QMUD_VK_EXECUTE    = 0x2B;
constexpr quint16       QMUD_VK_EXSEL      = 0xF8;
constexpr quint16       QMUD_VK_F1         = 0x70;
constexpr quint16       QMUD_VK_F2         = 0x71;
constexpr quint16       QMUD_VK_F3         = 0x72;
constexpr quint16       QMUD_VK_F4         = 0x73;
constexpr quint16       QMUD_VK_F5         = 0x74;
constexpr quint16       QMUD_VK_F6         = 0x75;
constexpr quint16       QMUD_VK_F7         = 0x76;
constexpr quint16       QMUD_VK_F8         = 0x77;
constexpr quint16       QMUD_VK_F9         = 0x78;
constexpr quint16       QMUD_VK_F10        = 0x79;
constexpr quint16       QMUD_VK_F11        = 0x7A;
constexpr quint16       QMUD_VK_F12        = 0x7B;
constexpr quint16       QMUD_VK_F13        = 0x7C;
constexpr quint16       QMUD_VK_F14        = 0x7D;
constexpr quint16       QMUD_VK_F15        = 0x7E;
constexpr quint16       QMUD_VK_F16        = 0x7F;
constexpr quint16       QMUD_VK_F17        = 0x80;
constexpr quint16       QMUD_VK_F18        = 0x81;
constexpr quint16       QMUD_VK_F19        = 0x82;
constexpr quint16       QMUD_VK_F20        = 0x83;
constexpr quint16       QMUD_VK_F21        = 0x84;
constexpr quint16       QMUD_VK_F22        = 0x85;
constexpr quint16       QMUD_VK_F23        = 0x86;
constexpr quint16       QMUD_VK_F24        = 0x87;
constexpr quint16       QMUD_VK_FINAL      = 0x18;
constexpr quint16       QMUD_VK_HANGEUL    = 0x15;
constexpr quint16       QMUD_VK_HANGUL     = 0x15;
constexpr quint16       QMUD_VK_HANJA      = 0x19;
constexpr quint16       QMUD_VK_HELP       = 0x2F;
constexpr quint16       QMUD_VK_HOME       = 0x24;
constexpr quint16       QMUD_VK_INSERT     = 0x2D;
constexpr quint16       QMUD_VK_JUNJA      = 0x17;
constexpr quint16       QMUD_VK_KANA       = 0x15;
constexpr quint16       QMUD_VK_KANJI      = 0x19;
constexpr quint16       QMUD_VK_LBUTTON    = 0x01;
constexpr quint16       QMUD_VK_LCONTROL   = 0xA2;
constexpr quint16       QMUD_VK_LEFT       = 0x25;
constexpr quint16       QMUD_VK_LMENU      = 0xA4;
constexpr quint16       QMUD_VK_LSHIFT     = 0xA0;
constexpr quint16       QMUD_VK_LWIN       = 0x5B;
constexpr quint16       QMUD_VK_MBUTTON    = 0x04;
constexpr quint16       QMUD_VK_MENU       = 0x12;
constexpr quint16       QMUD_VK_MODECHANGE = 0x1F;
constexpr quint16       QMUD_VK_MULTIPLY   = 0x6A;
constexpr quint16       QMUD_VK_NEXT       = 0x22;
constexpr quint16       QMUD_VK_NONAME     = 0xFC;
constexpr quint16       QMUD_VK_NONCONVERT = 0x1D;
constexpr quint16       QMUD_VK_NUMLOCK    = 0x90;
constexpr quint16       QMUD_VK_NUMPAD0    = 0x60;
constexpr quint16       QMUD_VK_NUMPAD1    = 0x61;
constexpr quint16       QMUD_VK_NUMPAD2    = 0x62;
constexpr quint16       QMUD_VK_NUMPAD3    = 0x63;
constexpr quint16       QMUD_VK_NUMPAD4    = 0x64;
constexpr quint16       QMUD_VK_NUMPAD5    = 0x65;
constexpr quint16       QMUD_VK_NUMPAD6    = 0x66;
constexpr quint16       QMUD_VK_NUMPAD7    = 0x67;
constexpr quint16       QMUD_VK_NUMPAD8    = 0x68;
constexpr quint16       QMUD_VK_NUMPAD9    = 0x69;
constexpr quint16       QMUD_VK_OEM_CLEAR  = 0xFE;
constexpr quint16       QMUD_VK_PA1        = 0xFD;
constexpr quint16       QMUD_VK_PAUSE      = 0x13;
constexpr quint16       QMUD_VK_PLAY       = 0xFA;
constexpr quint16       QMUD_VK_PRINT      = 0x2A;
constexpr quint16       QMUD_VK_PRIOR      = 0x21;
constexpr quint16       QMUD_VK_PROCESSKEY = 0xE5;
constexpr quint16       QMUD_VK_RBUTTON    = 0x02;
constexpr quint16       QMUD_VK_RCONTROL   = 0xA3;
constexpr quint16       QMUD_VK_RETURN     = 0x0D;
constexpr quint16       QMUD_VK_RIGHT      = 0x27;
constexpr quint16       QMUD_VK_RMENU      = 0xA5;
constexpr quint16       QMUD_VK_RSHIFT     = 0xA1;
constexpr quint16       QMUD_VK_RWIN       = 0x5C;
constexpr quint16       QMUD_VK_SCROLL     = 0x91;
constexpr quint16       QMUD_VK_SELECT     = 0x29;
constexpr quint16       QMUD_VK_SEPARATOR  = 0x6C;
constexpr quint16       QMUD_VK_SHIFT      = 0x10;
constexpr quint16       QMUD_VK_SNAPSHOT   = 0x2C;
constexpr quint16       QMUD_VK_SPACE      = 0x20;
constexpr quint16       QMUD_VK_SUBTRACT   = 0x6D;
constexpr quint16       QMUD_VK_TAB        = 0x09;
constexpr quint16       QMUD_VK_UP         = 0x26;
constexpr quint16       QMUD_VK_ZOOM       = 0xFB;
constexpr quint16       QMUD_VK_OEM_1      = 0xBA;
constexpr quint16       QMUD_VK_OEM_PLUS   = 0xBB;
constexpr quint16       QMUD_VK_OEM_COMMA  = 0xBC;
constexpr quint16       QMUD_VK_OEM_MINUS  = 0xBD;
constexpr quint16       QMUD_VK_OEM_PERIOD = 0xBE;
constexpr quint16       QMUD_VK_OEM_2      = 0xBF;
constexpr quint16       QMUD_VK_OEM_3      = 0xC0;
constexpr quint16       QMUD_VK_OEM_4      = 0xDB;
constexpr quint16       QMUD_VK_OEM_5      = 0xDC;
constexpr quint16       QMUD_VK_OEM_6      = 0xDD;
constexpr quint16       QMUD_VK_OEM_7      = 0xDE;

const VirtualKeyMapping kVirtualKeyMappingTable[] = {

    {'0',                "0"         },
    {'1',                "1"         },
    {'2',                "2"         },
    {'3',                "3"         },
    {'4',                "4"         },
    {'5',                "5"         },
    {'6',                "6"         },
    {'7',                "7"         },
    {'8',                "8"         },
    {'9',                "9"         },
    {'A',                "A"         },
    {QMUD_VK_ACCEPT,     "Accept"    },
    {QMUD_VK_ADD,        "Add"       },
    {QMUD_VK_APPS,       "Apps"      },
    {QMUD_VK_ATTN,       "Attn"      },
    {'B',                "B"         },
    {QMUD_VK_BACK,       "Backspace" },
    {'C',                "C"         },
    {QMUD_VK_CANCEL,     "Cancel"    },
    {QMUD_VK_CAPITAL,    "Capital"   },
    {QMUD_VK_CLEAR,      "Clear"     },
    {QMUD_VK_CONTROL,    "Control"   },
    {QMUD_VK_CONVERT,    "Convert"   },
    {QMUD_VK_CRSEL,      "Crsel"     },
    {'D',                "D"         },
    {QMUD_VK_DECIMAL,    "Decimal"   },
    {QMUD_VK_DELETE,     "Delete"    },
    {QMUD_VK_DIVIDE,     "Divide"    },
    {QMUD_VK_DOWN,       "Down"      },
    {'E',                "E"         },
    {QMUD_VK_END,        "End"       },
    {QMUD_VK_EREOF,      "Ereof"     },
    {QMUD_VK_ESCAPE,     "Esc"       },
    {QMUD_VK_EXECUTE,    "Execute"   },
    {QMUD_VK_EXSEL,      "Exsel"     },
    {'F',                "F"         },
    {QMUD_VK_F1,         "F1"        },
    {QMUD_VK_F10,        "F10"       },
    {QMUD_VK_F11,        "F11"       },
    {QMUD_VK_F12,        "F12"       },
    {QMUD_VK_F13,        "F13"       },
    {QMUD_VK_F14,        "F14"       },
    {QMUD_VK_F15,        "F15"       },
    {QMUD_VK_F16,        "F16"       },
    {QMUD_VK_F17,        "F17"       },
    {QMUD_VK_F18,        "F18"       },
    {QMUD_VK_F19,        "F19"       },
    {QMUD_VK_F2,         "F2"        },
    {QMUD_VK_F20,        "F20"       },
    {QMUD_VK_F21,        "F21"       },
    {QMUD_VK_F22,        "F22"       },
    {QMUD_VK_F23,        "F23"       },
    {QMUD_VK_F24,        "F24"       },
    {QMUD_VK_F3,         "F3"        },
    {QMUD_VK_F4,         "F4"        },
    {QMUD_VK_F5,         "F5"        },
    {QMUD_VK_F6,         "F6"        },
    {QMUD_VK_F7,         "F7"        },
    {QMUD_VK_F8,         "F8"        },
    {QMUD_VK_F9,         "F9"        },
    {QMUD_VK_FINAL,      "Final"     },
    {'G',                "G"         },
    {'H',                "H"         },
    {QMUD_VK_HANGEUL,    "Hangeul"   },
    {QMUD_VK_HANGUL,     "Hangul"    },
    {QMUD_VK_HANJA,      "Hanja"     },
    {QMUD_VK_HELP,       "Help"      },
    {QMUD_VK_HOME,       "Home"      },
    {'I',                "I"         },
    {QMUD_VK_INSERT,     "Insert"    },
    {'J',                "J"         },
    {QMUD_VK_JUNJA,      "Junja"     },
    {'K',                "K"         },
    {QMUD_VK_KANA,       "Kana"      },
    {QMUD_VK_KANJI,      "Kanji"     },
    {'L',                "L"         },
    {QMUD_VK_LBUTTON,    "LButton"   },
    {QMUD_VK_LCONTROL,   "LControl"  },
    {QMUD_VK_LEFT,       "Left"      },
    {QMUD_VK_LMENU,      "LMenu"     },
    {QMUD_VK_LSHIFT,     "LShift"    },
    {QMUD_VK_LWIN,       "LWin"      },
    {'M',                "M"         },
    {QMUD_VK_MBUTTON,    "MButton"   },
    {QMUD_VK_MENU,       "Menu"      },
    {QMUD_VK_MODECHANGE, "ModeChange"},
    {QMUD_VK_MULTIPLY,   "Multiply"  },
    {'N',                "N"         },
    {QMUD_VK_NEXT,       "PageDown"  },
    {QMUD_VK_NONAME,     "Noname"    },
    {QMUD_VK_NONCONVERT, "NonConvert"},
    {QMUD_VK_NUMLOCK,    "Numlock"   },
    {QMUD_VK_NUMPAD0,    "Numpad0"   },
    {QMUD_VK_NUMPAD1,    "Numpad1"   },
    {QMUD_VK_NUMPAD2,    "Numpad2"   },
    {QMUD_VK_NUMPAD3,    "Numpad3"   },
    {QMUD_VK_NUMPAD4,    "Numpad4"   },
    {QMUD_VK_NUMPAD5,    "Numpad5"   },
    {QMUD_VK_NUMPAD6,    "Numpad6"   },
    {QMUD_VK_NUMPAD7,    "Numpad7"   },
    {QMUD_VK_NUMPAD8,    "Numpad8"   },
    {QMUD_VK_NUMPAD9,    "Numpad9"   },
    {'O',                "O"         },
    {QMUD_VK_OEM_CLEAR,  "Oem_clear" },
    {'P',                "P"         },
    {QMUD_VK_PA1,        "Pa1"       },
    {QMUD_VK_PAUSE,      "Pause"     },
    {QMUD_VK_PLAY,       "Play"      },
    {QMUD_VK_PRINT,      "Print"     },
    {QMUD_VK_PRIOR,      "PageUp"    },
    {QMUD_VK_PROCESSKEY, "ProcessKey"},
    {'Q',                "Q"         },
    {'R',                "R"         },
    {QMUD_VK_RBUTTON,    "RButton"   },
    {QMUD_VK_RCONTROL,   "RControl"  },
    {QMUD_VK_RETURN,     "Enter"     },
    {QMUD_VK_RIGHT,      "Right"     },
    {QMUD_VK_RMENU,      "RMenu"     },
    {QMUD_VK_RSHIFT,     "RShift"    },
    {QMUD_VK_RWIN,       "RWin"      },
    {'S',                "S"         },
    {QMUD_VK_SCROLL,     "Scroll"    },
    {QMUD_VK_SELECT,     "Select"    },
    {QMUD_VK_SEPARATOR,  "Separator" },
    {QMUD_VK_SHIFT,      "Shift"     },
    {QMUD_VK_SNAPSHOT,   "Snapshot"  },
    {QMUD_VK_SPACE,      "Space"     },
    {QMUD_VK_SUBTRACT,   "Subtract"  },
    {'T',                "T"         },
    {QMUD_VK_TAB,        "Tab"       },
    {'U',                "U"         },
    {QMUD_VK_UP,         "Up"        },
    {'V',                "V"         },
    {'W',                "W"         },
    {'X',                "X"         },
    {'Y',                "Y"         },
    {'Z',                "Z"         },
    {QMUD_VK_ZOOM,       "Zoom"      },
    {QMUD_VK_OEM_1,      ";"         },
    {QMUD_VK_OEM_PLUS,   "Plus"      },
    {QMUD_VK_OEM_COMMA,  ","         },
    {QMUD_VK_OEM_MINUS,  "-"         },
    {QMUD_VK_OEM_PERIOD, "."         },
    {QMUD_VK_OEM_2,      "/"         },
    {QMUD_VK_OEM_3,      "`"         },
    {QMUD_VK_OEM_4,      "["         },
    {QMUD_VK_OEM_5,      "\\"        },
    {QMUD_VK_OEM_6,      "]"         },
    {QMUD_VK_OEM_7,      "'"         },

    // Note, things like '(' are really Shift+9

    kVirtualKeyMappingEndMarker, // End of table marker
}; // End of kVirtualKeyMappingTable table
