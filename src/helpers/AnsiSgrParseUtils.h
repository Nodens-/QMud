/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AnsiSgrParseUtils.h
 * Role: ANSI SGR parsing helpers used by runtime render paths that mix escape-coded text with higher-level markup.
 */

#ifndef QMUD_ANSISGRPARSEUTILS_H
#define QMUD_ANSISGRPARSEUTILS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QByteArray>
#include <QByteArrayView>
// ReSharper disable once CppUnusedIncludeDirective
#include <QString>
#include <QVector>

#include <functional>

/**
 * @brief Styled text context carried while ANSI escape sequences are parsed.
 */
struct QMudStyledTextState
{
		bool    bold{false};
		bool    underline{false};
		bool    italic{false};
		bool    blink{false};
		bool    strike{false};
		bool    inverse{false};
		QString fore;
		QString back;
		int     actionType{0};
		QString action;
		QString hint;
		QString variable;
		bool    startTag{false};
		bool    monospace{false};
};

/**
 * @brief One decoded text chunk and the style state active for it.
 */
struct QMudStyledChunk
{
		QString             text;
		QMudStyledTextState state;
};

/**
 * @brief Incremental parser state for ANSI/OSC escape decoding across chunks.
 */
struct QMudAnsiStreamState
{
		enum class Mode
		{
				Normal,
				Escape,
				Csi,
				Osc,
				OscEsc
		};

		Mode       mode{Mode::Normal};
		QByteArray pending;
};

/**
 * @brief Action identifiers applied when OSC8 hyperlinks are parsed.
 */
struct QMudOscActionIds
{
		int none{0};
		int send{0};
		int prompt{0};
		int hyperlink{0};
};

/**
 * @brief Decodes plain bytes between ANSI SGR/OSC sequences into style-tagged chunks.
 *
 * The parser consumes CSI `m` and OSC `8` sequences, mutates @p state, and emits
 * only decoded plain-text chunks. Partial sequences are retained in @p streamState.
 *
 * @param bytes Incoming bytes to parse.
 * @param streamState Carry-over parser state for split escape sequences.
 * @param defaultFore Default foreground color name used by SGR reset.
 * @param defaultBack Default background color name used by SGR reset.
 * @param normalAnsiColorFromIndex Palette resolver for ANSI 30-37 / 40-47 colors.
 * @param boldAnsiColorFromIndex Palette resolver for bright ANSI colors and bold mapping.
 * @param colorFromIndex Resolver for xterm 256-color indices.
 * @param decodeBytes Byte decoder used for non-escape runs.
 * @param state In/out style context.
 * @param oscActionIds Action mapping used when OSC8 hyperlinks are parsed.
 * @return Decoded chunks with the style active at each emission point.
 */
QVector<QMudStyledChunk> qmudParseAnsiSgrChunks(const QByteArray &bytes, QMudAnsiStreamState &streamState,
                                                const QString &defaultFore, const QString &defaultBack,
                                                const std::function<QString(int)> &normalAnsiColorFromIndex,
                                                const std::function<QString(int)> &boldAnsiColorFromIndex,
                                                const std::function<QString(int)> &colorFromIndex,
                                                const std::function<QString(QByteArrayView)> &decodeBytes,
                                                QMudStyledTextState &state,
                                                const QMudOscActionIds &oscActionIds = {});

#endif // QMUD_ANSISGRPARSEUTILS_H
