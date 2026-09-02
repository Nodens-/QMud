/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldOptions.h
 * Role: Typed world-option metadata/interfaces defining supported options, ranges, and conversion helpers.
 * This is used for MXP <option>/<recommend_option> handling.
 */

#ifndef QMUD_WORLDOPTIONS_H
#define QMUD_WORLDOPTIONS_H

#include <QString>
#include <optional>
// Canonical world/runtime option constants
constexpr int DEFAULT_CHAT_PORT        = 4050;
constexpr int MAX_CUSTOM               = 16;
constexpr int MAX_LINE_WIDTH           = 500;
constexpr int DEFAULT_TRIGGER_SEQUENCE = 100;
constexpr int DEFAULT_ALIAS_SEQUENCE   = 100;

namespace QMudPluginSequence
{
	/** Default plugin dispatch sequence when no sequence is declared. */
	inline constexpr int                    kDefault = 5000;
	/** Lowest plugin dispatch sequence accepted by the legacy format. */
	inline constexpr int                    kMinimum = -10000;
	/** Highest plugin dispatch sequence accepted by the legacy format. */
	inline constexpr int                    kMaximum = 10000;

	/**
	 * @brief Parses and validates a plugin dispatch sequence.
	 * @param text Serialized sequence value.
	 * @return Parsed sequence, or no value when malformed or outside the supported range.
	 */
	[[nodiscard]] inline std::optional<int> parse(const QString &text)
	{
		bool      ok       = false;
		const int sequence = text.trimmed().toInt(&ok);
		if (!ok || sequence < kMinimum || sequence > kMaximum)
			return std::nullopt;
		return sequence;
	}
} // namespace QMudPluginSequence

#ifdef DEFAULT_CHARSET
#undef DEFAULT_CHARSET
#endif
constexpr int DEFAULT_CHARSET                  = 1;
constexpr int FW_DONTCARE                      = 0;
constexpr int MAX_WILDCARDS                    = 10;
constexpr int OTHER_CUSTOM                     = MAX_CUSTOM;
constexpr int TRIGGER_COLOUR_CHANGE_BOTH       = 0;
constexpr int TRIGGER_COLOUR_CHANGE_FOREGROUND = 1;
constexpr int TRIGGER_COLOUR_CHANGE_BACKGROUND = 2;

// Send-to enum values (OtherTypes.h)
enum
{
	eSendToWorld,
	eSendToCommand,
	eSendToOutput,
	eSendToStatus,
	eSendToNotepad,
	eAppendToNotepad,
	eSendToLogFile,
	eReplaceNotepad,
	eSendToCommandQueue,
	eSendToVariable,
	eSendToExecute,
	eSendToSpeedwalk,
	eSendToScript,
	eSendImmediate,
	eSendToScriptAfterOmit,
	eSendToLast
};

// Proxy type enum values
enum
{
	eProxyServerNone,
	eProxyServerSocks4,
	eProxyServerSocks5,
	eProxyServerLast,
};

// TLS mode enum values
enum
{
	eTlsDirect,
	eTlsStartTls,
	eTlsMethodLast,
};

// Auto-connect values
enum
{
	eNoAutoConnect,
	eConnectMUSH,
	eConnectDiku,
	eConnectMXP,
	eConnectTypeMax,
};

// Script reload options
enum
{
	eReloadConfirm,
	eReloadAlways,
	eReloadNever,
};

// MXP use options
enum
{
	eOnCommandMXP,
	eQueryMXP,
	eUseMXP,
	eNoMXP,
};

// MXP debug levels
enum
{
	DBG_NONE,
	DBG_ERROR,
	DBG_WARNING,
	DBG_INFO,
	DBG_ALL,
};

// Option flags
inline constexpr int OPT_CUSTOM_COLOUR =
    0x000001; // color number  (add 1 to color to save, subtract 1 to load)
inline constexpr int OPT_RGB_COLOUR          = 0x000002;  // color is RGB color
inline constexpr int OPT_DOUBLE              = 0x000004;  // option is a double
inline constexpr int OPT_UPDATE_VIEWS        = 0x000100;  // if changed, update all views
inline constexpr int OPT_UPDATE_INPUT_FONT   = 0x000200;  // if changed, update input font
inline constexpr int OPT_UPDATE_OUTPUT_FONT  = 0x000400;  // if changed, update output font
inline constexpr int OPT_FIX_OUTPUT_BUFFER   = 0x000800;  // if changed, rework output buffer size
inline constexpr int OPT_FIX_WRAP_COLUMN     = 0x001000;  // if changed, wrap column has changed
inline constexpr int OPT_FIX_SPEEDWALK_DELAY = 0x002000;  // if changed, speedwalk delay has changed
inline constexpr int OPT_USE_MXP             = 0x004000;  // if changed, use_mxp has changed
inline constexpr int OPT_PLUGIN_CANNOT_READ  = 0x100000;  // plugin may not read its value
inline constexpr int OPT_PLUGIN_CANNOT_WRITE = 0x200000;  // plugin may not write its value
inline constexpr int OPT_PLUGIN_CANNOT_RW    = 0x300000;  // plugin may not read or write its value
inline constexpr int OPT_CANNOT_WRITE        = 0x400000;  // cannot be changed by any script
inline constexpr int OPT_SERVER_CAN_WRITE    = 0x800000;  // CAN be changed by <recommend_option> tag
inline constexpr int OPT_FIX_INPUT_WRAP      = 0x1000000; // Added for input wrapping.
inline constexpr int OPT_FIX_TOOLTIP_VISIBLE = 0x2000000; // Added for tooltip delay changing.
inline constexpr int OPT_FIX_TOOLTIP_START   = 0x4000000; // Added for tooltip delay changing.

/**
 * @brief Exact runtime cache updated when a numeric world option changes.
 *
 * The option table is the authoritative mapping. `SetOptionItem` uses this binding so changing one option
 * cannot accidentally re-apply unrelated world attributes.
 */
enum class WorldNumericOptionBinding
{
	None,
	AlternativeInverse,
	AltArrowRecallsPartial,
	AlwaysRecordCommandHistory,
	ArrowsChangeHistory,
	ArrowKeysWrap,
	ArrowRecallsPartial,
	AutoPause,
	AutoRepeat,
	AutoResizeCommandWindow,
	AutoResizeMaximumLines,
	AutoResizeMinimumLines,
	ChatListener,
	ConfirmBeforeReplacingTyping,
	ConfirmOnPaste,
	CtrlBackspaceDeletesLastWord,
	CtrlNGoesToNextCommand,
	CtrlPGoesToPreviousCommand,
	CtrlZGoesToEndOfBuffer,
	DisplayMyInput,
	DoubleClickInserts,
	DoubleClickSends,
	EscapeDeletesInput,
	FadeOutputAfterSeconds,
	FadeOutputOpacityPercent,
	FadeOutputSeconds,
	HistoryLines,
	HyperlinkAddsToCommandHistory,
	HyperlinkColour,
	InputBackgroundColour,
	InputFont,
	InputTextColour,
	KeepCommandsOnSameLine,
	KeepPauseAtBottom,
	LineInformation,
	LineSpacing,
	LowerCaseTabCompletion,
	NoEchoOff,
	OutputFont,
	OutputWrap,
	PartialSaveCharacterThreshold,
	PixelOffset,
	SaveDeletedCommand,
	ShowBold,
	ShowItalic,
	ShowUnderline,
	TabCompletionExcludesSymbolPrefix,
	TabCompletionExcludesSymbolSuffix,
	TabCompletionLines,
	TabCompletionSpace,
	TimestampColours,
	UnderlineHyperlinks,
	UseCustomLinkColour,
	WrapColumn,
	WrapInput,
	CommandDoNotTranslateIac,
	CommandEnableSpamPrevention,
	CommandRegexpMatchEmpty,
	CommandSpamLineCount,
	CommandSpeedWalkDelay,
	CommandTranslateBackslashSequences,
	CommandTranslateGerman,
	CommandUtf8,
};

/**
 * @brief Numeric world-option metadata entry used by option lookup tables.
 */
struct WorldNumericOption
{
		const char               *name{};
		long long                 defaultValue{};
		long long                 minValue{};
		long long                 maxValue{};
		int                       flags{};
		WorldNumericOptionBinding binding{WorldNumericOptionBinding::None};
};

/**
 * @brief Returns pointer to static numeric-option table.
 * @return Pointer to numeric-option table.
 */
const WorldNumericOption *worldNumericOptions();
/**
 * @brief Returns number of entries in numeric-option table.
 * @return Numeric-option table entry count.
 */
int                       worldNumericOptionCount();

namespace QMudWorldOptions
{
	/**
	 * @brief Looks up numeric option metadata by option name.
	 * @param name Numeric option name.
	 * @return Matching option metadata pointer, or `nullptr`.
	 */
	const WorldNumericOption *findWorldNumericOption(const QString &name);
	/**
	 * @brief Checks a public scripting value against an option table entry.
	 * @param option Numeric option metadata.
	 * @param value Public scripting value.
	 * @return `true` when the value is accepted.
	 */
	[[nodiscard]] bool        numericOptionValueInRange(const WorldNumericOption &option, long long value);
	/**
	 * @brief Checks an unconverted Lua numeric value against an option table entry.
	 * @param option Numeric option metadata.
	 * @param value Lua numeric value.
	 * @return `true` when finite and within the accepted range.
	 */
	[[nodiscard]] bool        numericOptionValueInRange(const WorldNumericOption &option, double value);
	/**
	 * @brief Converts an internal world-attribute value to its public numeric option value.
	 * @param option Numeric option metadata.
	 * @param text Internal world-attribute representation.
	 * @return Public value, or no value when the text cannot represent this option.
	 */
	[[nodiscard]] std::optional<long long> publicNumericOptionValue(const WorldNumericOption &option,
	                                                                const QString            &text);
	/**
	 * @brief Converts a public scripting value to its canonical persisted runtime text.
	 * @param option Numeric option metadata.
	 * @param value Public scripting value.
	 * @return Internal world-attribute representation.
	 */
	[[nodiscard]] QString storedNumericOptionText(const WorldNumericOption &option, long long value);
} // namespace QMudWorldOptions

#endif // QMUD_WORLDOPTIONS_H
