/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldOptionDefaultsAlpha.inc
 * Role: Canonical alpha world-option default entries included by WorldOptionDefaults and WorldRuntime.
 */

#ifdef QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_ALPHA_ROWS

	{"auto_log_file_name", ""}, {"auto_say_override_prefix", "-"}, {"auto_say_string", "say ", OPT_KEEP_SPACES},
    {"beep_sound", "sounds/QMud/beep.wav"}, {"chat_file_save_directory", ""}, {"chat_name", ""},
    {"chat_message_prefix", "", OPT_KEEP_SPACES}, {"command_stack_character", ";", OPT_COMMAND_STACK},
    {"connect_text", "", OPT_MULTLINE}, {"editor_window_name", ""}, {"filter_aliases", "", OPT_MULTLINE},
    {"filter_timers", "", OPT_MULTLINE}, {"filter_triggers", "", OPT_MULTLINE},
    {"filter_variables", "", OPT_MULTLINE}, {"id", "", OPT_WORLD_ID},
    {"input_font_name", "DejaVu Sans Mono", OPT_UPDATE_VIEWS | OPT_UPDATE_INPUT_FONT},
    {"log_file_postamble", "", OPT_MULTLINE}, {"log_file_preamble", "", OPT_MULTLINE},
    {"log_line_postamble_input", "", OPT_KEEP_SPACES}, {"log_line_postamble_notes", "", OPT_KEEP_SPACES},
    {"log_line_postamble_output", "", OPT_KEEP_SPACES}, {"log_line_preamble_input", "", OPT_KEEP_SPACES},
    {"log_line_preamble_notes", "", OPT_KEEP_SPACES}, {"log_line_preamble_output", "", OPT_KEEP_SPACES},
    {"mapping_failure", "Alas, you cannot go that way.", OPT_KEEP_SPACES},
    {"name", "CthulhuMUD", OPT_PLUGIN_CANNOT_WRITE}, {"new_activity_sound", kNoSoundLiteral},
    {"notes", "", OPT_MULTLINE}, {"on_mxp_close_tag", ""}, {"on_mxp_error", ""}, {"on_mxp_open_tag", ""},
    {"on_mxp_set_variable", ""}, {"on_mxp_start", ""}, {"on_mxp_stop", ""}, {"on_world_close", ""},
    {"on_world_save", ""}, {"on_world_connect", ""}, {"on_world_disconnect", ""}, {"on_world_get_focus", ""},
    {"on_world_lose_focus", ""}, {"on_world_open", ""},
    {"output_font_name", "DejaVu Sans Mono", OPT_UPDATE_OUTPUT_FONT},
    {"password", "", OPT_PASSWORD | OPT_PLUGIN_CANNOT_RW}, {"paste_line_postamble", "", OPT_KEEP_SPACES},
    {"paste_line_preamble", "", OPT_KEEP_SPACES}, {"paste_postamble", "", OPT_MULTLINE},
    {"paste_preamble", "", OPT_MULTLINE}, {"player", "", OPT_PLUGIN_CANNOT_WRITE},
    {"proxy_server", "", OPT_PLUGIN_CANNOT_WRITE}, {"proxy_username", "", OPT_PLUGIN_CANNOT_RW},
    {"proxy_password", "", OPT_PASSWORD | OPT_PLUGIN_CANNOT_RW},
    {"recall_line_preamble", "", OPT_KEEP_SPACES}, {"script_editor", "notepad"},
    {"script_editor_argument", "\"%file\""}, {"script_filename", ""}, {"script_language", "lua"},
    {"script_prefix", ""}, {"send_to_world_file_postamble", "", OPT_MULTLINE},
    {"send_to_world_file_preamble", "", OPT_MULTLINE}, {"send_to_world_line_postamble", ""},
    {"send_to_world_line_preamble", ""}, {"site", "cthulhumud.com", OPT_PLUGIN_CANNOT_WRITE},
    {"spam_message", "look"}, {"speed_walk_filler", "", OPT_KEEP_SPACES},
    {"speed_walk_prefix", "#", OPT_KEEP_SPACES}, {"tab_completion_defaults", "", OPT_MULTLINE},
    {"terminal_identification", "QMud"}, {"timestamp_input", "", OPT_KEEP_SPACES | OPT_UPDATE_VIEWS},
    {"timestamp_notes", "", OPT_KEEP_SPACES | OPT_UPDATE_VIEWS},
    {"timestamp_output", "", OPT_KEEP_SPACES | OPT_UPDATE_VIEWS},
{
	nullptr, nullptr
} // end of table marker

#endif // QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_ALPHA_ROWS
