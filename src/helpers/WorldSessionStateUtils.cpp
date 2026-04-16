/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldSessionStateUtils.cpp
 * Role: Binary persistence helpers for per-world output-buffer and command-history session state.
 */

#include "WorldSessionStateUtils.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <array>
#include <limits>

namespace
{
	constexpr quint32 kSessionStateMagic             = 0x514D5353u; // "QMSS"
	constexpr quint32 kSessionStateSchemaVersion     = 1u;
	constexpr quint32 kContainerFlagCompressed       = 0x01u;
	constexpr quint32 kPayloadFlagOutputBuffer       = 0x01u;
	constexpr quint32 kPayloadFlagCommandHistory     = 0x02u;
	constexpr quint32 kPayloadFlagCustomMxpElements  = 0x04u;
	constexpr quint32 kPayloadFlagMxpSessionState    = 0x08u;
	constexpr quint16 kStyleFlagBold                 = 0x0001u;
	constexpr quint16 kStyleFlagUnderline            = 0x0002u;
	constexpr quint16 kStyleFlagItalic               = 0x0004u;
	constexpr quint16 kStyleFlagBlink                = 0x0008u;
	constexpr quint16 kStyleFlagStrike               = 0x0010u;
	constexpr quint16 kStyleFlagInverse              = 0x0020u;
	constexpr quint16 kStyleFlagChanged              = 0x0040u;
	constexpr quint16 kStyleFlagStartTag             = 0x0080u;
	constexpr qint64  kInvalidTimeMarker             = std::numeric_limits<qint64>::min();
	constexpr quint32 kMaxPersistedOutputLineCount   = 500000u;
	constexpr quint32 kMaxPersistedSpanCountPerLine  = 100000u;
	constexpr quint32 kMaxPersistedHistoryEntryCount = 5000u;
	constexpr quint32 kMaxPersistedCustomMxpElements = 2048u;

	quint32           crc32ForBytes(const QByteArray &bytes)
	{
		static constexpr std::array<quint32, 256> table = []
		{
			std::array<quint32, 256> result{};
			for (size_t i = 0; i < result.size(); ++i)
			{
				quint32 value = static_cast<quint32>(i);
				for (int bit = 0; bit < 8; ++bit)
				{
					if (value & 1u)
						value = (value >> 1u) ^ 0xEDB88320u;
					else
						value >>= 1u;
				}
				result[i] = value;
			}
			return result;
		}();

		quint32 crc = 0xFFFFFFFFu;
		for (const unsigned char byte : bytes)
			crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8u);
		return crc ^ 0xFFFFFFFFu;
	}

	void writeColor(QDataStream &stream, const QColor &color)
	{
		const bool valid = color.isValid();
		stream << valid;
		if (valid)
			stream << color.rgba();
	}

	void readColor(QDataStream &stream, QColor *color)
	{
		bool valid = false;
		stream >> valid;
		if (!valid)
		{
			*color = {};
			return;
		}
		quint32 rgba = 0;
		stream >> rgba;
		*color = QColor::fromRgba(rgba);
	}

	void writeStyleSpan(QDataStream &stream, const WorldRuntime::StyleSpan &span)
	{
		quint16 styleFlags = 0;
		if (span.bold)
			styleFlags |= kStyleFlagBold;
		if (span.underline)
			styleFlags |= kStyleFlagUnderline;
		if (span.italic)
			styleFlags |= kStyleFlagItalic;
		if (span.blink)
			styleFlags |= kStyleFlagBlink;
		if (span.strike)
			styleFlags |= kStyleFlagStrike;
		if (span.inverse)
			styleFlags |= kStyleFlagInverse;
		if (span.changed)
			styleFlags |= kStyleFlagChanged;
		if (span.startTag)
			styleFlags |= kStyleFlagStartTag;

		stream << static_cast<qint32>(span.length);
		stream << styleFlags;
		stream << static_cast<qint32>(span.actionType);
		stream << span.action;
		stream << span.hint;
		stream << span.variable;
		writeColor(stream, span.fore);
		writeColor(stream, span.back);
	}

	void readStyleSpan(QDataStream &stream, WorldRuntime::StyleSpan *span)
	{
		qint32  length     = 0;
		quint16 styleFlags = 0;
		qint32  actionType = 0;
		stream >> length;
		stream >> styleFlags;
		stream >> actionType;
		stream >> span->action;
		stream >> span->hint;
		stream >> span->variable;
		readColor(stream, &span->fore);
		readColor(stream, &span->back);

		span->length     = qMax(0, length);
		span->bold       = (styleFlags & kStyleFlagBold) != 0;
		span->underline  = (styleFlags & kStyleFlagUnderline) != 0;
		span->italic     = (styleFlags & kStyleFlagItalic) != 0;
		span->blink      = (styleFlags & kStyleFlagBlink) != 0;
		span->strike     = (styleFlags & kStyleFlagStrike) != 0;
		span->inverse    = (styleFlags & kStyleFlagInverse) != 0;
		span->changed    = (styleFlags & kStyleFlagChanged) != 0;
		span->startTag   = (styleFlags & kStyleFlagStartTag) != 0;
		span->actionType = actionType;
	}

	void writeLineEntry(QDataStream &stream, const WorldRuntime::LineEntry &line)
	{
		const qint64 timeMs = line.time.isValid() ? line.time.toMSecsSinceEpoch() : kInvalidTimeMarker;
		stream << line.text;
		stream << static_cast<qint32>(line.flags);
		stream << line.hardReturn;
		stream << timeMs;
		stream << line.lineNumber;
		stream << line.ticks;
		stream << line.elapsed;
		stream << static_cast<quint32>(line.spans.size());
		for (const WorldRuntime::StyleSpan &span : line.spans)
			writeStyleSpan(stream, span);
	}

	bool readLineEntry(QDataStream &stream, WorldRuntime::LineEntry *line, QString *errorMessage)
	{
		qint32  flags      = 0;
		bool    hardReturn = true;
		qint64  timeMs     = kInvalidTimeMarker;
		quint32 spanCount  = 0;
		stream >> line->text;
		stream >> flags;
		stream >> hardReturn;
		stream >> timeMs;
		stream >> line->lineNumber;
		stream >> line->ticks;
		stream >> line->elapsed;
		stream >> spanCount;
		if (spanCount > kMaxPersistedSpanCountPerLine)
		{
			if (errorMessage)
			{
				*errorMessage = QStringLiteral("Session state has too many style spans (%1).").arg(spanCount);
			}
			return false;
		}
		line->flags      = flags;
		line->hardReturn = hardReturn;
		line->time = (timeMs == kInvalidTimeMarker) ? QDateTime() : QDateTime::fromMSecsSinceEpoch(timeMs);
		line->spans.clear();
		line->spans.reserve(static_cast<int>(spanCount));
		for (quint32 i = 0; i < spanCount; ++i)
		{
			WorldRuntime::StyleSpan span;
			readStyleSpan(stream, &span);
			line->spans.push_back(span);
		}
		return true;
	}

	void writeCustomMxpElement(QDataStream &stream, const TelnetProcessor::CustomElementInfo &element)
	{
		stream << element.name;
		stream << element.open;
		stream << element.command;
		stream << static_cast<qint32>(element.tag);
		stream << element.flag;
		stream << element.definition;
		stream << element.attributes;
	}

	bool readCustomMxpElement(QDataStream &stream, TelnetProcessor::CustomElementInfo &element,
	                          QString *errorMessage)
	{
		qint32 tag = 0;
		stream >> element.name;
		stream >> element.open;
		stream >> element.command;
		stream >> tag;
		stream >> element.flag;
		stream >> element.definition;
		stream >> element.attributes;
		if (stream.status() != QDataStream::Ok)
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Failed to parse custom MXP element.");
			return false;
		}
		element.tag = tag;
		return true;
	}
} // namespace

namespace QMudWorldSessionState
{
	bool writeSessionStateFile(const QString &filePath, const WorldSessionStateData &state,
	                           QString *errorMessage)
	{
		if (errorMessage)
			errorMessage->clear();
		if (filePath.trimmed().isEmpty())
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Session state file path is empty.");
			return false;
		}
		if (!state.hasOutputBuffer && !state.hasCommandHistory && !state.hasCustomMxpElements &&
		    !state.hasMxpSessionState)
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("No session-state sections were enabled.");
			return false;
		}

		const QFileInfo targetInfo(filePath);
		if (!QDir().mkpath(targetInfo.absolutePath()))
		{
			if (errorMessage)
			{
				*errorMessage = QStringLiteral("Failed to create session-state directory: %1")
				                    .arg(targetInfo.absolutePath());
			}
			return false;
		}

		QByteArray payload;
		{
			QDataStream payloadStream(&payload, QIODevice::WriteOnly);
			payloadStream.setByteOrder(QDataStream::LittleEndian);
			quint32 payloadFlags = 0;
			if (state.hasOutputBuffer)
				payloadFlags |= kPayloadFlagOutputBuffer;
			if (state.hasCommandHistory)
				payloadFlags |= kPayloadFlagCommandHistory;
			if (state.hasCustomMxpElements)
				payloadFlags |= kPayloadFlagCustomMxpElements;
			if (state.hasMxpSessionState)
				payloadFlags |= kPayloadFlagMxpSessionState;
			payloadStream << payloadFlags;
			if (state.hasOutputBuffer)
			{
				payloadStream << static_cast<quint32>(state.outputLines.size());
				for (const WorldRuntime::LineEntry &line : state.outputLines)
					writeLineEntry(payloadStream, line);
			}
			if (state.hasCommandHistory)
			{
				payloadStream << static_cast<quint32>(state.commandHistory.size());
				for (const QString &entry : state.commandHistory)
					payloadStream << entry;
			}
			if (state.hasCustomMxpElements)
			{
				payloadStream << static_cast<quint32>(state.customMxpElements.size());
				for (const TelnetProcessor::CustomElementInfo &element : state.customMxpElements)
					writeCustomMxpElement(payloadStream, element);
			}
			if (state.hasMxpSessionState)
			{
				payloadStream << state.mxpSessionState.enabled;
				payloadStream << state.mxpSessionState.puebloActive;
				payloadStream << state.mxpSessionState.secureMode;
				payloadStream << static_cast<qint32>(state.mxpSessionState.mode);
				payloadStream << static_cast<qint32>(state.mxpSessionState.defaultMode);
				payloadStream << static_cast<qint32>(state.mxpSessionState.previousMode);
			}
			if (payloadStream.status() != QDataStream::Ok)
			{
				if (errorMessage)
					*errorMessage = QStringLiteral("Failed to serialize session-state payload.");
				return false;
			}
		}

		const quint32    checksum          = crc32ForBytes(payload);
		const QByteArray compressedPayload = qCompress(payload, 1);
		if (compressedPayload.isEmpty())
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Failed to compress session-state payload.");
			return false;
		}

		QSaveFile outputFile(filePath);
		if (!outputFile.open(QIODevice::WriteOnly))
		{
			if (errorMessage)
			{
				*errorMessage = QStringLiteral("Failed to open session-state file for writing: %1")
				                    .arg(outputFile.errorString());
			}
			return false;
		}

		QDataStream outputStream(&outputFile);
		outputStream.setByteOrder(QDataStream::LittleEndian);
		outputStream << kSessionStateMagic;
		outputStream << kSessionStateSchemaVersion;
		outputStream << kContainerFlagCompressed;
		outputStream << static_cast<quint32>(payload.size());
		outputStream << checksum;
		outputStream << compressedPayload;
		if (outputStream.status() != QDataStream::Ok || !outputFile.commit())
		{
			if (errorMessage)
			{
				*errorMessage =
				    QStringLiteral("Failed to write session-state file: %1").arg(outputFile.errorString());
			}
			return false;
		}

		return true;
	}

	bool readSessionStateFile(const QString &filePath, WorldSessionStateData *state, QString *errorMessage)
	{
		if (errorMessage)
			errorMessage->clear();
		if (!state)
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Session-state output pointer is null.");
			return false;
		}
		*state = {};
		if (filePath.trimmed().isEmpty())
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Session state file path is empty.");
			return false;
		}

		QFile inputFile(filePath);
		if (!inputFile.open(QIODevice::ReadOnly))
		{
			if (errorMessage)
				*errorMessage =
				    QStringLiteral("Failed to open session-state file: %1").arg(inputFile.errorString());
			return false;
		}

		QDataStream inputStream(&inputFile);
		inputStream.setByteOrder(QDataStream::LittleEndian);
		quint32    magic          = 0;
		quint32    schemaVersion  = 0;
		quint32    containerFlags = 0;
		quint32    payloadSize    = 0;
		quint32    checksum       = 0;
		QByteArray storedPayload;
		inputStream >> magic;
		inputStream >> schemaVersion;
		inputStream >> containerFlags;
		inputStream >> payloadSize;
		inputStream >> checksum;
		inputStream >> storedPayload;
		if (inputStream.status() != QDataStream::Ok)
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Failed to read session-state container.");
			return false;
		}
		if (magic != kSessionStateMagic)
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Session-state file has invalid magic.");
			return false;
		}
		if (schemaVersion != kSessionStateSchemaVersion)
		{
			if (errorMessage)
			{
				*errorMessage =
				    QStringLiteral("Unsupported session-state schema version: %1").arg(schemaVersion);
			}
			return false;
		}

		QByteArray payload = storedPayload;
		if ((containerFlags & kContainerFlagCompressed) != 0)
		{
			payload = qUncompress(storedPayload);
			if (payload.isNull())
			{
				if (errorMessage)
					*errorMessage = QStringLiteral("Failed to decompress session-state payload.");
				return false;
			}
		}
		if (payloadSize != static_cast<quint32>(payload.size()))
		{
			if (errorMessage)
			{
				*errorMessage = QStringLiteral("Session-state payload size mismatch (expected %1, got %2).")
				                    .arg(payloadSize)
				                    .arg(payload.size());
			}
			return false;
		}
		if (crc32ForBytes(payload) != checksum)
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Session-state checksum verification failed.");
			return false;
		}

		QDataStream payloadStream(payload);
		payloadStream.setByteOrder(QDataStream::LittleEndian);
		quint32 payloadFlags = 0;
		payloadStream >> payloadFlags;
		state->hasOutputBuffer      = (payloadFlags & kPayloadFlagOutputBuffer) != 0;
		state->hasCommandHistory    = (payloadFlags & kPayloadFlagCommandHistory) != 0;
		state->hasCustomMxpElements = (payloadFlags & kPayloadFlagCustomMxpElements) != 0;
		state->hasMxpSessionState   = (payloadFlags & kPayloadFlagMxpSessionState) != 0;

		if (state->hasOutputBuffer)
		{
			quint32 lineCount = 0;
			payloadStream >> lineCount;
			if (lineCount > kMaxPersistedOutputLineCount)
			{
				if (errorMessage)
				{
					*errorMessage =
					    QStringLiteral("Session state has too many output lines (%1).").arg(lineCount);
				}
				return false;
			}
			state->outputLines.reserve(static_cast<int>(lineCount));
			for (quint32 i = 0; i < lineCount; ++i)
			{
				WorldRuntime::LineEntry line;
				if (!readLineEntry(payloadStream, &line, errorMessage))
					return false;
				state->outputLines.push_back(line);
			}
		}

		if (state->hasCommandHistory)
		{
			quint32 historyCount = 0;
			payloadStream >> historyCount;
			if (historyCount > kMaxPersistedHistoryEntryCount)
			{
				if (errorMessage)
				{
					*errorMessage = QStringLiteral("Session state has too many command-history entries (%1).")
					                    .arg(historyCount);
				}
				return false;
			}
			state->commandHistory.reserve(static_cast<int>(historyCount));
			for (quint32 i = 0; i < historyCount; ++i)
			{
				QString entry;
				payloadStream >> entry;
				state->commandHistory.push_back(entry);
			}
		}
		if (state->hasCustomMxpElements)
		{
			quint32 elementCount = 0;
			payloadStream >> elementCount;
			if (elementCount > kMaxPersistedCustomMxpElements)
			{
				if (errorMessage)
				{
					*errorMessage = QStringLiteral("Session state has too many custom MXP elements (%1).")
					                    .arg(elementCount);
				}
				return false;
			}
			state->customMxpElements.reserve(static_cast<int>(elementCount));
			for (quint32 i = 0; i < elementCount; ++i)
			{
				TelnetProcessor::CustomElementInfo element;
				if (!readCustomMxpElement(payloadStream, element, errorMessage))
					return false;
				state->customMxpElements.push_back(element);
			}
		}
		if (state->hasMxpSessionState)
		{
			payloadStream >> state->mxpSessionState.enabled;
			payloadStream >> state->mxpSessionState.puebloActive;
			payloadStream >> state->mxpSessionState.secureMode;
			if (!payloadStream.atEnd())
			{
				qint32 mode         = 0;
				qint32 defaultMode  = 0;
				qint32 previousMode = 0;
				payloadStream >> mode;
				payloadStream >> defaultMode;
				payloadStream >> previousMode;
				state->mxpSessionState.mode         = mode;
				state->mxpSessionState.defaultMode  = defaultMode;
				state->mxpSessionState.previousMode = previousMode;
			}
			else
			{
				const int fallbackMode              = state->mxpSessionState.secureMode ? 1 : 0;
				state->mxpSessionState.mode         = fallbackMode;
				state->mxpSessionState.defaultMode  = fallbackMode;
				state->mxpSessionState.previousMode = fallbackMode;
			}
		}

		if (payloadStream.status() != QDataStream::Ok)
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Failed to parse session-state payload.");
			return false;
		}

		return true;
	}

	bool removeSessionStateFile(const QString &filePath, QString *errorMessage)
	{
		if (errorMessage)
			errorMessage->clear();
		if (filePath.trimmed().isEmpty())
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Session state file path is empty.");
			return false;
		}

		QFile file(filePath);
		if (!file.exists())
			return true;
		if (!file.remove())
		{
			if (errorMessage)
				*errorMessage =
				    QStringLiteral("Failed to remove session-state file: %1").arg(file.errorString());
			return false;
		}
		return true;
	}
} // namespace QMudWorldSessionState
