/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MspProtocol.h
 * Role: Parsing and validation for MUD Sound Protocol commands.
 */

#ifndef QMUD_MSPPROTOCOL_H
#define QMUD_MSPPROTOCOL_H

#include <QString>
#include <optional>

namespace QMudMspProtocol
{
	/** @brief Media class selected by an MSP command. */
	enum class MediaType
	{
		Sound,
		Music
	};

	/** @brief Validated values carried by one line-form MSP command. */
	struct Command
	{
			MediaType type{MediaType::Sound};
			QString   fileName;
			QString   url;
			QString   tag;
			bool      urlProvided{false};
			int       volume{100};
			int       repeatCount{1};
			int       priority{50};
			bool      continueMusic{true};
			bool      stop{false};
			bool      setDefaultUrl{false};
	};

	/**
	 * @brief Parses one complete MSP command.
	 * @param text Command text from its marker through its closing parenthesis.
	 * @return Validated command, or no value when the text is not exactly one MSP command.
	 */
	[[nodiscard]] std::optional<Command> parseLine(const QString &text);
} // namespace QMudMspProtocol

#endif // QMUD_MSPPROTOCOL_H
