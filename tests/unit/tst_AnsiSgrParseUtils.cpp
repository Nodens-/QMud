/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_AnsiSgrParseUtils.cpp
 * Role: QTest coverage for ANSI SGR parsing behavior in mixed render contexts.
 */

#include "AnsiSgrParseUtils.h"

#include <QtTest/QTest>

namespace
{
	QString normalColor(const int idx)
	{
		static constexpr const char *kPalette[8] = {"#000000", "#800000", "#008000", "#808000",
		                                             "#000080", "#800080", "#008080", "#c0c0c0"};
		if (idx < 0 || idx >= 8)
			return {};
		return QString::fromLatin1(kPalette[idx]);
	}

	QString boldColor(const int idx)
	{
		static constexpr const char *kPalette[8] = {"#808080", "#ff0000", "#00ff00", "#ffff00",
		                                             "#0000ff", "#ff00ff", "#00ffff", "#ffffff"};
		if (idx < 0 || idx >= 8)
			return {};
		return QString::fromLatin1(kPalette[idx]);
	}
} // namespace

/**
 * @brief QTest fixture covering ANSI SGR parser scenarios.
 */
class tst_AnsiSgrParseUtils : public QObject
{
		Q_OBJECT

	private slots:
		void ansiParsingAppliesInMixedActionContext(); // NOLINT(readability-convert-member-functions-to-static)
		void splitEscapeSequenceCarriesAcrossCalls(); // NOLINT(readability-convert-member-functions-to-static)
		void osc8HyperlinkActionsApplyAndClose(); // NOLINT(readability-convert-member-functions-to-static)
		void osc8SendAndPromptDecodePayload(); // NOLINT(readability-convert-member-functions-to-static)
		void osc8UnknownSchemeIsInert(); // NOLINT(readability-convert-member-functions-to-static)
		void osc8CloseClearsActionScopedMetadata(); // NOLINT(readability-convert-member-functions-to-static)
		void osc8StTerminatorAcrossChunks(); // NOLINT(readability-convert-member-functions-to-static)
		void oversizedCsiSequenceIsDiscarded(); // NOLINT(readability-convert-member-functions-to-static)
		void oversizedOscSequenceIsDiscarded(); // NOLINT(readability-convert-member-functions-to-static)
		void unknownEscapePrefixPreservesFollowingByte(); // NOLINT(readability-convert-member-functions-to-static)
};

void tst_AnsiSgrParseUtils::ansiParsingAppliesInMixedActionContext() // NOLINT(readability-convert-member-functions-to-static)
{
	QMudAnsiStreamState streamState;
	QMudStyledTextState state;
	state.actionType = 2;
	state.action     = QStringLiteral("whois Testuser");
	state.hint       = QStringLiteral("hint");
	state.variable   = QStringLiteral("mxp_var");
	state.startTag   = true;
	state.monospace  = true;

	const QVector<QMudStyledChunk> chunks = qmudParseAnsiSgrChunks(
	    QByteArrayLiteral("\x1b[0;31m[minimal\x1b[0m"), streamState, QStringLiteral("#ffffff"),
	    QStringLiteral("#000000"), normalColor, boldColor,
	    [](const int idx)
	    {
		    if (idx == 196)
			    return QStringLiteral("#ff0000");
		    return QString();
	    },
	    [](const QByteArrayView bytes) { return QString::fromLatin1(bytes); }, state);

	QCOMPARE(chunks.size(), 1);
	QCOMPARE(chunks.at(0).text, QStringLiteral("[minimal"));
	QCOMPARE(chunks.at(0).state.fore, QStringLiteral("#800000"));
	QCOMPARE(chunks.at(0).state.actionType, 2);
	QCOMPARE(chunks.at(0).state.action, QStringLiteral("whois Testuser"));
	QCOMPARE(chunks.at(0).state.hint, QStringLiteral("hint"));
	QCOMPARE(chunks.at(0).state.variable, QStringLiteral("mxp_var"));
	QVERIFY(chunks.at(0).state.startTag);
	QVERIFY(chunks.at(0).state.monospace);

	QCOMPARE(state.fore, QStringLiteral("#ffffff"));
	QCOMPARE(state.back, QStringLiteral("#000000"));
	QCOMPARE(state.actionType, 2);
	QCOMPARE(state.action, QStringLiteral("whois Testuser"));
	QCOMPARE(state.hint, QStringLiteral("hint"));
	QCOMPARE(state.variable, QStringLiteral("mxp_var"));
	QVERIFY(!state.startTag);
	QVERIFY(state.monospace);
	QVERIFY(streamState.pending.isEmpty());
	QCOMPARE(streamState.mode, QMudAnsiStreamState::Mode::Normal);
}

void tst_AnsiSgrParseUtils::splitEscapeSequenceCarriesAcrossCalls() // NOLINT(readability-convert-member-functions-to-static)
{
	QMudAnsiStreamState streamState;
	QMudStyledTextState state;

	const auto defaultFore = QStringLiteral("#ffffff");
	const auto defaultBack = QStringLiteral("#000000");
	auto       decode      = [](const QByteArrayView bytes) { return QString::fromLatin1(bytes); };
	auto       colorLookup = [](int) { return QString(); };

	const QVector<QMudStyledChunk> first =
	    qmudParseAnsiSgrChunks(QByteArrayLiteral("\x1b[0;3"), streamState, defaultFore, defaultBack, normalColor,
	                           boldColor, colorLookup, decode, state);
	QVERIFY(first.isEmpty());
	QCOMPARE(streamState.mode, QMudAnsiStreamState::Mode::Csi);
	QCOMPARE(streamState.pending, QByteArrayLiteral("0;3"));

	const QVector<QMudStyledChunk> second =
	    qmudParseAnsiSgrChunks(QByteArrayLiteral("1mX"), streamState, defaultFore, defaultBack, normalColor,
	                           boldColor, colorLookup, decode, state);
	QCOMPARE(second.size(), 1);
	QCOMPARE(second.at(0).text, QStringLiteral("X"));
	QCOMPARE(second.at(0).state.fore, QStringLiteral("#800000"));
	QVERIFY(streamState.pending.isEmpty());
	QCOMPARE(streamState.mode, QMudAnsiStreamState::Mode::Normal);
}

void tst_AnsiSgrParseUtils::osc8HyperlinkActionsApplyAndClose() // NOLINT(readability-convert-member-functions-to-static)
{
	QMudAnsiStreamState streamState;
	QMudStyledTextState state;
	QMudOscActionIds    actions{0, 1, 2, 3};

	const QVector<QMudStyledChunk> opened = qmudParseAnsiSgrChunks(
	    QByteArrayLiteral("\x1b]8;;https://example.org\x07link"), streamState, QStringLiteral("#ffffff"),
	    QStringLiteral("#000000"), normalColor, boldColor, [](int) { return QString(); },
	    [](const QByteArrayView bytes) { return QString::fromLatin1(bytes); }, state, actions);
	QCOMPARE(opened.size(), 1);
	QCOMPARE(opened.at(0).text, QStringLiteral("link"));
	QCOMPARE(opened.at(0).state.actionType, 3);
	QCOMPARE(opened.at(0).state.action, QStringLiteral("https://example.org"));

	const QVector<QMudStyledChunk> closed = qmudParseAnsiSgrChunks(
	    QByteArrayLiteral("\x1b]8;;\x07"), streamState, QStringLiteral("#ffffff"), QStringLiteral("#000000"),
	    normalColor, boldColor, [](int) { return QString(); },
	    [](const QByteArrayView bytes) { return QString::fromLatin1(bytes); }, state, actions);
	QVERIFY(closed.isEmpty());
	QCOMPARE(state.actionType, 0);
	QCOMPARE(state.action, QString());
	QCOMPARE(streamState.mode, QMudAnsiStreamState::Mode::Normal);
}

void tst_AnsiSgrParseUtils::osc8SendAndPromptDecodePayload() // NOLINT(readability-convert-member-functions-to-static)
{
	QMudAnsiStreamState streamState;
	QMudStyledTextState state;
	QMudOscActionIds    actions{0, 1, 2, 3};
	auto                decode = [](const QByteArrayView bytes) { return QString::fromLatin1(bytes); };

	const QVector<QMudStyledChunk> sendChunks =
	    qmudParseAnsiSgrChunks(QByteArrayLiteral("\x1b]8;;send:buy%20bread\x07Send\x1b]8;;\x07"), streamState,
	                           QStringLiteral("#ffffff"), QStringLiteral("#000000"), normalColor, boldColor,
	                           [](int) { return QString(); }, decode, state, actions);
	QCOMPARE(sendChunks.size(), 1);
	QCOMPARE(sendChunks.at(0).text, QStringLiteral("Send"));
	QCOMPARE(sendChunks.at(0).state.actionType, 1);
	QCOMPARE(sendChunks.at(0).state.action, QStringLiteral("buy bread"));

	const QVector<QMudStyledChunk> promptChunks = qmudParseAnsiSgrChunks(
	    QByteArrayLiteral("\x1b]8;;prompt:buy%20bread\x07Prompt\x1b]8;;\x07"), streamState,
	    QStringLiteral("#ffffff"), QStringLiteral("#000000"), normalColor, boldColor,
	    [](int) { return QString(); }, decode, state, actions);
	QCOMPARE(promptChunks.size(), 1);
	QCOMPARE(promptChunks.at(0).text, QStringLiteral("Prompt"));
	QCOMPARE(promptChunks.at(0).state.actionType, 2);
	QCOMPARE(promptChunks.at(0).state.action, QStringLiteral("buy bread"));
}

void tst_AnsiSgrParseUtils::osc8UnknownSchemeIsInert() // NOLINT(readability-convert-member-functions-to-static)
{
	QMudAnsiStreamState streamState;
	QMudStyledTextState state;
	QMudOscActionIds    actions{0, 1, 2, 3};
	auto                decode = [](const QByteArrayView bytes) { return QString::fromLatin1(bytes); };

	const QVector<QMudStyledChunk> chunks =
	    qmudParseAnsiSgrChunks(QByteArrayLiteral("\x1b]8;;unknown://junk\x07ignored?\x1b]8;;\x07"), streamState,
	                           QStringLiteral("#ffffff"), QStringLiteral("#000000"), normalColor, boldColor,
	                           [](int) { return QString(); }, decode, state, actions);
	QCOMPARE(chunks.size(), 1);
	QCOMPARE(chunks.at(0).text, QStringLiteral("ignored?"));
	QCOMPARE(chunks.at(0).state.actionType, 0);
	QCOMPARE(chunks.at(0).state.action, QString());
	QCOMPARE(state.actionType, 0);
	QCOMPARE(state.action, QString());
}

void tst_AnsiSgrParseUtils::osc8CloseClearsActionScopedMetadata() // NOLINT(readability-convert-member-functions-to-static)
{
	QMudAnsiStreamState streamState;
	QMudStyledTextState state;
	QMudOscActionIds    actions{0, 1, 2, 3};
	state.actionType = 3;
	state.action     = QStringLiteral("https://example.org");
	state.hint       = QStringLiteral("hint");
	state.variable   = QStringLiteral("mxp_var");
	state.startTag   = true;

	const QVector<QMudStyledChunk> closed = qmudParseAnsiSgrChunks(
	    QByteArrayLiteral("\x1b]8;;\x07"), streamState, QStringLiteral("#ffffff"), QStringLiteral("#000000"),
	    normalColor, boldColor, [](int) { return QString(); },
	    [](const QByteArrayView bytes) { return QString::fromLatin1(bytes); }, state, actions);
	QVERIFY(closed.isEmpty());
	QCOMPARE(state.actionType, 0);
	QCOMPARE(state.action, QString());
	QCOMPARE(state.hint, QString());
	QCOMPARE(state.variable, QString());
	QVERIFY(!state.startTag);
}

void tst_AnsiSgrParseUtils::osc8StTerminatorAcrossChunks() // NOLINT(readability-convert-member-functions-to-static)
{
	QMudAnsiStreamState streamState;
	QMudStyledTextState state;
	QMudOscActionIds    actions{0, 1, 2, 3};
	auto                decode = [](const QByteArrayView bytes) { return QString::fromLatin1(bytes); };

	const QVector<QMudStyledChunk> first =
	    qmudParseAnsiSgrChunks(QByteArrayLiteral("\x1b]8;;send:look\x1b"), streamState, QStringLiteral("#ffffff"),
	                           QStringLiteral("#000000"), normalColor, boldColor, [](int) { return QString(); },
	                           decode, state, actions);
	QVERIFY(first.isEmpty());
	QCOMPARE(streamState.mode, QMudAnsiStreamState::Mode::OscEsc);

	const QVector<QMudStyledChunk> second =
	    qmudParseAnsiSgrChunks(QByteArrayLiteral("\\cmd"), streamState, QStringLiteral("#ffffff"),
	                           QStringLiteral("#000000"), normalColor, boldColor, [](int) { return QString(); },
	                           decode, state, actions);
	QCOMPARE(second.size(), 1);
	QCOMPARE(second.at(0).text, QStringLiteral("cmd"));
	QCOMPARE(second.at(0).state.actionType, 1);
	QCOMPARE(second.at(0).state.action, QStringLiteral("look"));
	QCOMPARE(streamState.mode, QMudAnsiStreamState::Mode::Normal);
}

void tst_AnsiSgrParseUtils::oversizedCsiSequenceIsDiscarded() // NOLINT(readability-convert-member-functions-to-static)
{
	QMudAnsiStreamState streamState;
	QMudStyledTextState state;

	QByteArray bytes("\x1b[");
	bytes.append(QByteArray(256, '1'));
	const QVector<QMudStyledChunk> chunks =
	    qmudParseAnsiSgrChunks(bytes, streamState, QStringLiteral("#ffffff"), QStringLiteral("#000000"),
	                           normalColor, boldColor, [](int) { return QString(); },
	                           [](const QByteArrayView input) { return QString::fromLatin1(input); }, state);
	for (const QMudStyledChunk &chunk : chunks)
	{
		QCOMPARE(chunk.state.actionType, 0);
		QCOMPARE(chunk.state.action, QString());
	}
	QCOMPARE(streamState.mode, QMudAnsiStreamState::Mode::Normal);
	QVERIFY(streamState.pending.isEmpty());
	QCOMPARE(state.actionType, 0);
	QCOMPARE(state.action, QString());
}

void tst_AnsiSgrParseUtils::oversizedOscSequenceIsDiscarded() // NOLINT(readability-convert-member-functions-to-static)
{
	QMudAnsiStreamState streamState;
	QMudStyledTextState state;
	QMudOscActionIds    actions{0, 1, 2, 3};

	QByteArray bytes("\x1b]8;;");
	bytes.append(QByteArray(9000, 'x'));
	const QVector<QMudStyledChunk> chunks =
	    qmudParseAnsiSgrChunks(bytes, streamState, QStringLiteral("#ffffff"), QStringLiteral("#000000"),
	                           normalColor, boldColor, [](int) { return QString(); },
	                           [](const QByteArrayView input) { return QString::fromLatin1(input); }, state,
	                           actions);
	for (const QMudStyledChunk &chunk : chunks)
	{
		QCOMPARE(chunk.state.actionType, 0);
		QCOMPARE(chunk.state.action, QString());
	}
	QCOMPARE(streamState.mode, QMudAnsiStreamState::Mode::Normal);
	QVERIFY(streamState.pending.isEmpty());
	QCOMPARE(state.actionType, 0);
	QCOMPARE(state.action, QString());
}

void tst_AnsiSgrParseUtils::unknownEscapePrefixPreservesFollowingByte() // NOLINT(readability-convert-member-functions-to-static)
{
	QMudAnsiStreamState streamState;
	QMudStyledTextState state;

	const QVector<QMudStyledChunk> chunks =
	    qmudParseAnsiSgrChunks(QByteArrayLiteral("\x1bXabc"), streamState, QStringLiteral("#ffffff"),
	                           QStringLiteral("#000000"), normalColor, boldColor, [](int) { return QString(); },
	                           [](const QByteArrayView bytes) { return QString::fromLatin1(bytes); }, state);
	QCOMPARE(chunks.size(), 1);
	QCOMPARE(chunks.at(0).text, QStringLiteral("Xabc"));
}

QTEST_APPLESS_MAIN(tst_AnsiSgrParseUtils)

#include "tst_AnsiSgrParseUtils.moc"
