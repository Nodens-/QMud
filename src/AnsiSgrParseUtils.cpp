/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AnsiSgrParseUtils.cpp
 * Role: ANSI SGR parsing helpers used by runtime render paths that mix escape-coded text with higher-level markup.
 */

#include "AnsiSgrParseUtils.h"

#include <QList>
#include <QUrl>

namespace
{
	constexpr int kMaxCsiPendingBytes = 128;
	constexpr int kMaxOscPendingBytes = 8192;

	QString rgbTripletToHex(const int r, const int g, const int b)
	{
		const auto clampChannel = [](const int value)
		{
			if (value < 0)
				return 0;
			if (value > 255)
				return 255;
			return value;
		};

		return QStringLiteral("#%1%2%3")
		    .arg(clampChannel(r), 2, 16, QLatin1Char('0'))
		    .arg(clampChannel(g), 2, 16, QLatin1Char('0'))
		    .arg(clampChannel(b), 2, 16, QLatin1Char('0'));
	}

	void applyAnsiSgr(const QVector<int> &codes, QMudStyledTextState &state, const QString &defaultFore,
	                  const QString &defaultBack,
	                  const std::function<QString(int)> &normalAnsiColorFromIndex,
	                  const std::function<QString(int)> &boldAnsiColorFromIndex,
	                  const std::function<QString(int)> &colorFromIndex)
	{
		QVector<int> params = codes;
		if (params.isEmpty())
			params.push_back(0);

		for (int i = 0; i < params.size(); ++i)
		{
			const int code = params.at(i);
			if (code == 0)
			{
				// SGR reset should not clear non-visual action/link context.
				const int     actionType = state.actionType;
				const QString action     = state.action;
				const QString hint       = state.hint;
				const QString variable   = state.variable;
				const bool    startTag   = state.startTag;
				const bool    monospace  = state.monospace;
				state                   = QMudStyledTextState{};
				state.actionType        = actionType;
				state.action            = action;
				state.hint              = hint;
				state.variable          = variable;
				state.startTag          = startTag;
				state.monospace         = monospace;
				state.fore              = defaultFore;
				state.back              = defaultBack;
			}
			else if (code == 1)
			{
				state.bold = true;
				if (!state.fore.isEmpty())
				{
					for (int idx = 0; idx < 8; ++idx)
					{
						if (state.fore == normalAnsiColorFromIndex(idx))
						{
							state.fore = boldAnsiColorFromIndex(idx);
							break;
						}
					}
				}
			}
			else if (code == 3)
				state.italic = true;
			else if (code == 5)
				state.blink = true;
			else if (code == 4)
				state.underline = true;
			else if (code == 7)
			{
				state.inverse = true;
				if (!state.fore.isEmpty() || !state.back.isEmpty())
					qSwap(state.fore, state.back);
			}
			else if (code == 9)
				state.strike = true;
			else if (code == 22)
			{
				state.bold = false;
				if (!state.fore.isEmpty())
				{
					for (int idx = 0; idx < 8; ++idx)
					{
						if (state.fore == boldAnsiColorFromIndex(idx))
						{
							state.fore = normalAnsiColorFromIndex(idx);
							break;
						}
					}
				}
			}
			else if (code == 23)
				state.italic = false;
			else if (code == 25)
				state.blink = false;
			else if (code == 24)
				state.underline = false;
			else if (code == 27)
				state.inverse = false;
			else if (code == 29)
				state.strike = false;
			else if (code == 39)
				state.fore = defaultFore;
			else if (code == 49)
				state.back = defaultBack;
			else if (code >= 30 && code <= 37)
			{
				const int idx = code - 30;
				if (idx >= 0 && idx < 8)
					state.fore = state.bold ? boldAnsiColorFromIndex(idx) : normalAnsiColorFromIndex(idx);
				else
					state.fore.clear();
			}
			else if (code >= 40 && code <= 47)
			{
				const int idx = code - 40;
				if (idx >= 0 && idx < 8)
					state.back = normalAnsiColorFromIndex(idx);
				else
					state.back.clear();
			}
			else if (code >= 90 && code <= 97)
			{
				const int idx = code - 90;
				if (idx >= 0 && idx < 8)
					state.fore = boldAnsiColorFromIndex(idx);
			}
			else if (code >= 100 && code <= 107)
			{
				const int idx = code - 100;
				if (idx >= 0 && idx < 8)
					state.back = boldAnsiColorFromIndex(idx);
			}
			else if (code == 38 || code == 48)
			{
				const bool isFore = (code == 38);
				if (i + 1 >= params.size())
					continue;
				const int mode = params.at(i + 1);
				if (mode == 5 && i + 2 < params.size())
				{
					const int idx = params.at(i + 2);
					if (isFore)
						state.fore = colorFromIndex(idx);
					else
						state.back = colorFromIndex(idx);
					i += 2;
				}
				else if (mode == 2 && i + 4 < params.size())
				{
					const int     r   = params.at(i + 2);
					const int     g   = params.at(i + 3);
					const int     b   = params.at(i + 4);
					const QString rgb = rgbTripletToHex(r, g, b);
					if (isFore)
						state.fore = rgb;
					else
						state.back = rgb;
					i += 4;
				}
			}
		}
	}
} // namespace

QVector<QMudStyledChunk> qmudParseAnsiSgrChunks(
    const QByteArray &bytes, QMudAnsiStreamState &streamState, const QString &defaultFore,
    const QString &defaultBack, const std::function<QString(int)> &normalAnsiColorFromIndex,
    const std::function<QString(int)> &boldAnsiColorFromIndex, const std::function<QString(int)> &colorFromIndex,
    const std::function<QString(QByteArrayView)> &decodeBytes, QMudStyledTextState &state,
    const QMudOscActionIds &oscActionIds)
{
	QVector<QMudStyledChunk> chunks;
	if (bytes.isEmpty())
		return chunks;

	auto flushPlain = [&](QByteArray &plainBytes)
	{
		if (plainBytes.isEmpty())
			return;
		const QString decoded = decodeBytes(QByteArrayView(plainBytes));
		plainBytes.clear();
		if (decoded.isEmpty())
			return;
		QMudStyledChunk chunk;
		chunk.text     = decoded;
		chunk.state    = state;
		chunks.push_back(chunk);
		state.startTag = false;
	};

	auto applyOsc = [&](const QByteArray &params)
	{
		if (!params.startsWith("8;"))
			return;

		const int secondSemicolon = params.indexOf(';', 2);
		if (secondSemicolon < 0)
			return;

		QByteArray url = params.mid(secondSemicolon + 1);
		if (url.isEmpty())
		{
			state.actionType = oscActionIds.none;
			state.action.clear();
			state.hint.clear();
			state.variable.clear();
			state.startTag = false;
			return;
		}

		const QString urlText = QString::fromUtf8(url);
		const QString lower   = urlText.toLower();
		if (lower.startsWith(QStringLiteral("send:")))
		{
			state.actionType = oscActionIds.send;
			state.action     = QUrl::fromPercentEncoding(url.mid(5));
			state.hint.clear();
			state.variable.clear();
			state.startTag = false;
			return;
		}
		if (lower.startsWith(QStringLiteral("prompt:")))
		{
			state.actionType = oscActionIds.prompt;
			state.action     = QUrl::fromPercentEncoding(url.mid(7));
			state.hint.clear();
			state.variable.clear();
			state.startTag = false;
			return;
		}
		if (lower.startsWith(QStringLiteral("http://")) || lower.startsWith(QStringLiteral("https://")) ||
		    lower.startsWith(QStringLiteral("mailto:")))
		{
			state.actionType = oscActionIds.hyperlink;
			state.action     = urlText;
			state.hint.clear();
			state.variable.clear();
			state.startTag = false;
			return;
		}

		// Unsupported schemes are intentionally inert.
		state.actionType = oscActionIds.none;
		state.action.clear();
		state.hint.clear();
		state.variable.clear();
		state.startTag = false;
	};

	QByteArray plainBytes;
	plainBytes.reserve(bytes.size());
	for (int i = 0; i < bytes.size(); ++i)
	{
		const char ch = bytes.at(i);

		switch (streamState.mode)
		{
			case QMudAnsiStreamState::Mode::Normal:
				if (ch == '\x1b')
				{
					flushPlain(plainBytes);
					streamState.mode = QMudAnsiStreamState::Mode::Escape;
				}
				else
					plainBytes.append(ch);
				break;

			case QMudAnsiStreamState::Mode::Escape:
				if (ch == '[')
				{
					streamState.pending.clear();
					streamState.mode = QMudAnsiStreamState::Mode::Csi;
				}
				else if (ch == ']')
				{
					streamState.pending.clear();
					streamState.mode = QMudAnsiStreamState::Mode::Osc;
				}
				else
				{
					// Unknown ESC prefix: keep the non-ESC byte as visible text.
					plainBytes.append(ch);
					streamState.mode = QMudAnsiStreamState::Mode::Normal;
				}
				break;

			case QMudAnsiStreamState::Mode::Csi:
			{
				const unsigned char byte = static_cast<unsigned char>(ch);
				if (byte >= 0x40 && byte <= 0x7E)
				{
					if (ch == 'm')
					{
						QByteArray params = streamState.pending;
						params.replace(':', ';');
						const QList<QByteArray> parts = params.split(';');
						QVector<int>            codes;
						for (const QByteArray &part : parts)
						{
							if (part.isEmpty())
							{
								codes.push_back(0);
								continue;
							}
							bool      ok    = false;
							const int value = part.toInt(&ok);
							if (ok)
								codes.push_back(value);
						}
						applyAnsiSgr(codes, state, defaultFore, defaultBack, normalAnsiColorFromIndex,
						             boldAnsiColorFromIndex, colorFromIndex);
					}
					streamState.pending.clear();
					streamState.mode = QMudAnsiStreamState::Mode::Normal;
				}
				else if (byte >= 0x20 && byte <= 0x3F)
				{
					streamState.pending.append(ch);
					if (streamState.pending.size() > kMaxCsiPendingBytes)
					{
						streamState.pending.clear();
						streamState.mode = QMudAnsiStreamState::Mode::Normal;
					}
				}
				else
				{
					// Corrupt CSI: drop it and reprocess current byte as normal text.
					streamState.pending.clear();
					streamState.mode = QMudAnsiStreamState::Mode::Normal;
					--i;
				}
				break;
			}

			case QMudAnsiStreamState::Mode::Osc:
				if (ch == '\a')
				{
					applyOsc(streamState.pending);
					streamState.pending.clear();
					streamState.mode = QMudAnsiStreamState::Mode::Normal;
				}
				else if (ch == '\x1b')
					streamState.mode = QMudAnsiStreamState::Mode::OscEsc;
				else
				{
					streamState.pending.append(ch);
					if (streamState.pending.size() > kMaxOscPendingBytes)
					{
						streamState.pending.clear();
						streamState.mode = QMudAnsiStreamState::Mode::Normal;
					}
				}
				break;

			case QMudAnsiStreamState::Mode::OscEsc:
				if (ch == '\\')
				{
					applyOsc(streamState.pending);
					streamState.pending.clear();
					streamState.mode = QMudAnsiStreamState::Mode::Normal;
				}
				else
				{
					// ESC not followed by ST terminator: keep bytes inside OSC payload.
					streamState.pending.append('\x1b');
					if (streamState.pending.size() > kMaxOscPendingBytes)
					{
						streamState.pending.clear();
						streamState.mode = QMudAnsiStreamState::Mode::Normal;
						break;
					}
					if (ch == '\a')
					{
						applyOsc(streamState.pending);
						streamState.pending.clear();
						streamState.mode = QMudAnsiStreamState::Mode::Normal;
					}
					else
					{
						streamState.pending.append(ch);
						if (streamState.pending.size() > kMaxOscPendingBytes)
						{
							streamState.pending.clear();
							streamState.mode = QMudAnsiStreamState::Mode::Normal;
							break;
						}
						streamState.mode = QMudAnsiStreamState::Mode::Osc;
					}
				}
				break;
		}
	}

	flushPlain(plainBytes);
	return chunks;
}
