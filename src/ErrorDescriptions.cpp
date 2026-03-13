/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ErrorDescriptions.cpp
 * Role: Authoritative error-code-to-message table returned by runtime and Lua APIs when reporting operation failures.
 */

// Centralized error description table for Lua/runtime bindings.

#include "ErrorDescriptions.h"

namespace
{
	constexpr const char *translateNoOp(const char *stringLiteral)
	{
		return stringLiteral;
	}
} // namespace

extern const IntFlagsPair kErrorDescriptionMappingTable[] = {
    {0,     translateNoOp("No error")                                                                },
    {30001, translateNoOp("The world is already open")                                               },
    {30002, translateNoOp("The world is closed, this action cannot be performed")                    },
    {30003, translateNoOp("No name has been specified where one is required")                        },
    {30004, translateNoOp("The sound file could not be played")                                      },
    {30005, translateNoOp("The specified trigger name does not exist")                               },
    {30006, translateNoOp("Attempt to add a trigger that already exists")                            },
    {30007, translateNoOp("The trigger \"match\" string cannot be empty")                            },
    {30008, translateNoOp("The name of this object is invalid")                                      },
    {30009, translateNoOp("Script name is not in the script file")                                   },
    {30010, translateNoOp("The specified alias name does not exist")                                 },
    {30011, translateNoOp("Attempt to add a alias that already exists")                              },
    {30012, translateNoOp("The alias \"match\" string cannot be empty")                              },
    {30013, translateNoOp("Unable to open requested file")                                           },
    {30014, translateNoOp("Log file was not open")                                                   },
    {30015, translateNoOp("Log file was already open")                                               },
    {30016, translateNoOp("Bad write to log file")                                                   },
    {30017, translateNoOp("The specified timer name does not exist")                                 },
    {30018, translateNoOp("Attempt to add a timer that already exists")                              },
    {30019, translateNoOp("Attempt to delete a variable that does not exist")                        },
    {30020, translateNoOp("Attempt to use SetCommand with a non-empty command window")               },
    {30021, translateNoOp("Bad regular expression syntax")                                           },
    {30022, translateNoOp("Time given to AddTimer is invalid")                                       },
    {30023, translateNoOp("Direction given to AddToMapper is invalid")                               },
    {30024, translateNoOp("No items in mapper")                                                      },
    {30025, translateNoOp("Option name not found")                                                   },
    {30026, translateNoOp("New value for option is out of range")                                    },
    {30027, translateNoOp("Trigger sequence value invalid")                                          },
    {30028, translateNoOp("Where to send trigger text to is invalid")                                },
    {30029, translateNoOp("Trigger label not specified/invalid for 'send to variable'")              },
    {30030, translateNoOp("File name specified for plugin not found")                                },
    {30031, translateNoOp("There was a parsing or other problem loading the plugin")                 },
    {30032, translateNoOp("Plugin is not allowed to set this option")                                },
    {30033, translateNoOp("Plugin is not allowed to get this option")                                },
    {30034, translateNoOp("Requested plugin is not installed")                                       },
    {30035, translateNoOp("Only a plugin can do this")                                               },
    {30036, translateNoOp("Plugin does not support that subroutine (subroutine not in script)")      },
    {30037, translateNoOp("Plugin does not support saving state")                                    },
    {30038, translateNoOp("Plugin could not save state (eg. no state directory)")                    },
    {30039, translateNoOp("Plugin is currently disabled")                                            },
    {30040, translateNoOp("Could not call plugin routine")                                           },
    {30041, translateNoOp("Calls to \"Execute\" nested too deeply")                                  },
    {30042, translateNoOp("Unable to create socket for chat connection")                             },
    {30043, translateNoOp("Unable to do DNS (domain name) lookup for chat connection")               },
    {30044, translateNoOp("No chat connections open")                                                },
    {30045, translateNoOp("Requested chat person not connected")                                     },
    {30046, translateNoOp("General problem with a parameter to a script call")                       },
    {30047, translateNoOp("Already listening for incoming chats")                                    },
    {30048, translateNoOp("Chat session with that ID not found")                                     },
    {30049, translateNoOp("Already connected to that server/port")                                   },
    {30050, translateNoOp("Cannot get (text from the) clipboard")                                    },
    {30051, translateNoOp("Cannot open the specified file")                                          },
    {30052, translateNoOp("Already transferring a file")                                             },
    {30053, translateNoOp("Not transferring a file")                                                 },
    {30054, translateNoOp("There is not a command of that name")                                     },
    {30055, translateNoOp("That array already exists")                                               },
    {30056, translateNoOp("That array does not exist")                                               },
    {30057, translateNoOp("Values to be imported into array are not in pairs")                       },
    {30058, translateNoOp("Import succeeded, however some values were overwritten")                  },
    {30059, translateNoOp("Import/export delimiter must be a single character, other than backslash")},
    {30060, translateNoOp("Array element set, existing value overwritten")                           },
    {30061, translateNoOp("Array key does not exist")                                                },
    {30062, translateNoOp("Cannot import because cannot find unused temporary character")            },
    {30063, translateNoOp("Cannot delete trigger/alias/timer because it is executing a script")      },
    {30064, translateNoOp("Spell checker is not active")                                             },
    {30065, translateNoOp("Cannot create requested font")                                            },
    {30066, translateNoOp("Invalid settings for pen parameter")                                      },
    {30067, translateNoOp("Bitmap image could not be loaded")                                        },
    {30068, translateNoOp("Image has not been loaded into window")                                   },
    {30069, translateNoOp("Number of points supplied is incorrect")                                  },
    {30070, translateNoOp("Point is not numeric")                                                    },
    {30071, translateNoOp("Hotspot processing must all be in same plugin")                           },
    {30072, translateNoOp("Hotspot has not been defined for this window")                            },
    {30073, translateNoOp("Requested miniwindow does not exist")                                     },
    {30074, translateNoOp("Invalid settings for brush parameter")                                    },

    {0,     nullptr                                                                                  }
};
