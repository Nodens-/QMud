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

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void ansiParsingAppliesInMixedActionContext()
		{
			QByteArray         pending;
			QMudStyledTextState state;
			state.actionType = 2;
			state.action     = QStringLiteral("whois Testuser");
			state.hint       = QStringLiteral("hint");
			state.variable   = QStringLiteral("mxp_var");
			state.startTag   = true;
			state.monospace  = true;

			const QVector<QMudStyledChunk> chunks = qmudParseAnsiSgrChunks(
			    QByteArrayLiteral("\x1b[0;31m[minimal\x1b[0m"), pending, QStringLiteral("#ffffff"),
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
			QVERIFY(pending.isEmpty());
		}

		void splitEscapeSequenceCarriesAcrossCalls()
		{
			QByteArray         pending;
			QMudStyledTextState state;

			const auto            defaultFore = QStringLiteral("#ffffff");
			const auto            defaultBack = QStringLiteral("#000000");
			auto                  decode = [](const QByteArrayView bytes) { return QString::fromLatin1(bytes); };
			auto                  colorLookup = [](int) { return QString(); };

			const QVector<QMudStyledChunk> first =
			    qmudParseAnsiSgrChunks(QByteArrayLiteral("\x1b[0;3"), pending, defaultFore, defaultBack,
			                           normalColor, boldColor, colorLookup, decode, state);
			QVERIFY(first.isEmpty());
			QCOMPARE(pending, QByteArrayLiteral("\x1b[0;3"));

			const QVector<QMudStyledChunk> second =
			    qmudParseAnsiSgrChunks(QByteArrayLiteral("1mX"), pending, defaultFore, defaultBack,
			                           normalColor, boldColor, colorLookup, decode, state);
			QCOMPARE(second.size(), 1);
			QCOMPARE(second.at(0).text, QStringLiteral("X"));
			QCOMPARE(second.at(0).state.fore, QStringLiteral("#800000"));
			QVERIFY(pending.isEmpty());
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_AnsiSgrParseUtils)

#include "tst_AnsiSgrParseUtils.moc"
