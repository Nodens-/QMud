/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_TelnetProcessor_Mxp.cpp
 * Role: QTest coverage for TelnetProcessor MXP behavior.
 */

#include "TelnetCallbackSpy.h"
#include "TelnetProcessor.h"

#include <QtTest/QTest>

namespace
{
	constexpr unsigned char ESC        = 0x1B;
	constexpr unsigned char IAC        = 0xFF;
	constexpr unsigned char SB         = 0xFA;
	constexpr unsigned char SE         = 0xF0;
	constexpr unsigned char TELOPT_MXP = 91;

	QByteArray              bytes(std::initializer_list<unsigned char> raw)
	{
		QByteArray out;
		for (const unsigned char c : raw)
			out.append(static_cast<char>(c));
		return out;
	}
} // namespace

/**
 * @brief QTest fixture covering TelnetProcessor Mxp scenarios.
 */
class tst_TelnetProcessor_Mxp : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void parseStartAndEndTags()
		{
			TelnetProcessor processor;
			processor.setUseMxp(2); // eUseMXP

			const QByteArray output = processor.processBytes(QByteArrayLiteral("<bold>Hello</bold>"));
			QCOMPARE(output, QByteArrayLiteral("Hello"));

			const QList<TelnetProcessor::MxpEvent> events = processor.takeMxpEvents();
			QCOMPARE(events.size(), 2);
			QCOMPARE(events.at(0).type, TelnetProcessor::MxpEvent::StartTag);
			QCOMPARE(events.at(1).type, TelnetProcessor::MxpEvent::EndTag);
			QCOMPARE(events.at(0).name.toLower(), QByteArrayLiteral("bold"));
			QCOMPARE(events.at(1).name.toLower(), QByteArrayLiteral("bold"));
		}

		void customEntityExpansion()
		{
			TelnetProcessor processor;
			processor.setUseMxp(2);
			processor.setCustomEntity(QByteArrayLiteral("foo"), QByteArrayLiteral("BAR"));

			QByteArray value;
			QVERIFY(processor.getCustomEntityValue(QByteArrayLiteral("foo"), value));
			QCOMPARE(value, QByteArrayLiteral("BAR"));
			QCOMPARE(processor.customEntityCount(), 1);

			const QByteArray output = processor.processBytes(QByteArrayLiteral("&foo;"));
			QCOMPARE(output, QByteArrayLiteral("BAR"));
			QCOMPARE(processor.mxpEntityCount(), 1);
		}

		void onCommandModeStartsFromSubnegotiation()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(0); // eOnCommandMXP

			QVERIFY(!processor.isMxpEnabled());
			processor.processBytes(bytes({IAC, SB, TELOPT_MXP, IAC, SE}));
			QVERIFY(processor.isMxpEnabled());
			QCOMPARE(spy.mxpStarts.size(), 1);
			QVERIFY(!spy.mxpStarts.at(0).pueblo);
			QVERIFY(!spy.mxpStarts.at(0).manual);
		}

		void resetAndDisableCallbacks()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());

			processor.setUseMxp(2);
			QVERIFY(processor.isMxpEnabled());
			QCOMPARE(spy.mxpStarts.size(), 1);

			processor.resetMxp();
			QCOMPARE(spy.mxpResetCount, 1);
			QVERIFY(processor.isMxpEnabled());

			processor.disableMxp();
			QVERIFY(!processor.isMxpEnabled());
			QCOMPARE(spy.mxpStops.size(), 1);
			QVERIFY(spy.mxpStops.at(0).completely);
		}

		void activatePuebloMode()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());

			processor.activatePuebloMode();
			QVERIFY(processor.isMxpEnabled());
			QVERIFY(processor.isPuebloActive());
			QCOMPARE(spy.mxpStarts.size(), 1);
			QVERIFY(spy.mxpStarts.at(0).pueblo);
		}

		void permLockedModeTreatsTagsAsPlainText()
		{
			TelnetProcessor processor;
			processor.setUseMxp(2); // eUseMXP

			QByteArray input;
			input.append(static_cast<char>(ESC));
			input.append('[');
			input.append('7');
			input.append('z');
			input.append(QByteArrayLiteral("<bold>Text</bold>&lt;"));

			const QByteArray output = processor.processBytes(input);
			QCOMPARE(output, QByteArrayLiteral("<bold>Text</bold>&lt;"));
			QVERIFY(processor.takeMxpEvents().isEmpty());
		}

		void secureFlagIsCapturedPerEventWithinSinglePacket()
		{
			TelnetProcessor processor;
			processor.setUseMxp(2); // eUseMXP

			QByteArray input;
			input.append(static_cast<char>(ESC));
			input.append('[');
			input.append('1');
			input.append('z');
			input.append(QByteArrayLiteral("<send href=\"zap me\">go</send>"));
			input.append(static_cast<char>(ESC));
			input.append('[');
			input.append('7');
			input.append('z');

			const QByteArray output = processor.processBytes(input);
			QCOMPARE(output, QByteArrayLiteral("go"));
			QVERIFY(!processor.isMxpSecure());

			const QList<TelnetProcessor::MxpEvent> events = processor.takeMxpEvents();
			QCOMPARE(events.size(), 2);
			QCOMPARE(events.at(0).type, TelnetProcessor::MxpEvent::StartTag);
			QCOMPARE(events.at(1).type, TelnetProcessor::MxpEvent::EndTag);
			QCOMPARE(events.at(0).name.toLower(), QByteArrayLiteral("send"));
			QCOMPARE(events.at(1).name.toLower(), QByteArrayLiteral("send"));
			QVERIFY(events.at(0).secure);
			QVERIFY(events.at(1).secure);
		}

		void permLockedModeKeepsEntitiesLiteralAcrossLines()
		{
			TelnetProcessor processor;
			processor.setUseMxp(0); // eOnCommandMXP
			processor.processBytes(bytes({IAC, SB, TELOPT_MXP, IAC, SE}));
			QVERIFY(processor.isMxpEnabled());

			QByteArray input;
			input.append(static_cast<char>(ESC));
			input.append('[');
			input.append('2');
			input.append('z');
			input.append(QByteArrayLiteral("&lt;\n"));
			input.append(static_cast<char>(ESC));
			input.append('[');
			input.append('7');
			input.append('z');
			input.append(QByteArrayLiteral("&lt;\n"));
			input.append(QByteArrayLiteral("&lt;\n"));

			const QByteArray output = processor.processBytes(input);
			QCOMPARE(output, QByteArrayLiteral("&lt;\n&lt;\n&lt;\n"));
			QVERIFY(processor.takeMxpEvents().isEmpty());
		}

		void mixedAnsiAndMxpKeepsAnsiDataAndTracksTagOffsets()
		{
			TelnetProcessor processor;
			processor.setUseMxp(0); // eOnCommandMXP
			processor.processBytes(bytes({IAC, SB, TELOPT_MXP, IAC, SE}));
			QVERIFY(processor.isMxpEnabled());

			const QByteArray prefix = QByteArrayLiteral("\x1b[0;31m[minimal\x1b[0m\n");
			QByteArray       input  = prefix;
			input.append(static_cast<char>(ESC));
			input.append('[');
			input.append('1');
			input.append('z');
			input.append(QByteArrayLiteral("<SEND href='whois Testuser'>Testuser</SEND>"));

			const QByteArray output = processor.processBytes(input);
			QCOMPARE(output, prefix + QByteArrayLiteral("Testuser"));

			const QList<TelnetProcessor::MxpEvent> events = processor.takeMxpEvents();
			QCOMPARE(events.size(), 2);
			QCOMPARE(events.at(0).type, TelnetProcessor::MxpEvent::StartTag);
			QCOMPARE(events.at(1).type, TelnetProcessor::MxpEvent::EndTag);
			QCOMPARE(events.at(0).name.toLower(), QByteArrayLiteral("send"));
			QCOMPARE(events.at(1).name.toLower(), QByteArrayLiteral("send"));
			QCOMPARE(events.at(0).offset, static_cast<int>(prefix.size()));
			QCOMPARE(events.at(1).offset,
			         static_cast<int>(prefix.size() + QByteArrayLiteral("Testuser").size()));
		}

		void modeBoundaryOffsetsTrackEscZWithinMixedStream()
		{
			TelnetProcessor processor;
			processor.setUseMxp(0); // eOnCommandMXP
			processor.processBytes(bytes({IAC, SB, TELOPT_MXP, IAC, SE}));
			QVERIFY(processor.isMxpEnabled());

			QByteArray input;
			input.append(QByteArrayLiteral("\x1b[1z<SEND href='whois Testuser'>Testuser"));
			input.append(QByteArrayLiteral("\x1b[7z after"));

			const QByteArray output = processor.processBytes(input);
			QCOMPARE(output, QByteArrayLiteral("Testuser after"));

			const QList<TelnetProcessor::MxpEvent> events = processor.takeMxpEvents();
			QCOMPARE(events.size(), 1);
			QCOMPARE(events.at(0).type, TelnetProcessor::MxpEvent::StartTag);
			QCOMPARE(events.at(0).name.toLower(), QByteArrayLiteral("send"));
			QCOMPARE(events.at(0).offset, 0);

			const QList<TelnetProcessor::MxpModeChange> modeChanges = processor.takeMxpModeChanges();
			QCOMPARE(modeChanges.size(), 2);
			QCOMPARE(modeChanges.at(0).newMode, 1); // eMXP_secure
			QCOMPARE(modeChanges.at(0).offset, 0);
			QCOMPARE(modeChanges.at(1).newMode, 7); // eMXP_perm_locked
			QCOMPARE(modeChanges.at(1).offset, static_cast<int>(QByteArrayLiteral("Testuser").size()));
			QVERIFY(modeChanges.at(0).sequence < modeChanges.at(1).sequence);
		}

		void customElementDefinitionParsesAndIsQueryable()
		{
			TelnetProcessor processor;
			processor.setUseMxp(2); // eUseMXP

			QByteArray input;
			input.append(static_cast<char>(ESC));
			input.append('[');
			input.append('1');
			input.append('z');
			input.append(
			    QByteArrayLiteral("<!el pers \"<send href='examine &name;|consider &name;' "
			                      "hint='Examine &desc;|Consider &desc;' expire=pers>\" ATT='name desc'>"));

			QCOMPARE(processor.processBytes(input), QByteArray());

			const QList<TelnetProcessor::MxpEvent> events = processor.takeMxpEvents();
			QCOMPARE(events.size(), 1);
			QCOMPARE(events.at(0).type, TelnetProcessor::MxpEvent::Definition);

			TelnetProcessor::CustomElementInfo info;
			QVERIFY(processor.getCustomElementInfo(QByteArrayLiteral("pers"), info));
			QCOMPARE(info.name, QByteArrayLiteral("pers"));
			QCOMPARE(info.attributes, QByteArrayLiteral("name desc"));
			QVERIFY(info.definition.contains(QByteArrayLiteral("send")));
			QVERIFY(info.definition.contains(QByteArrayLiteral("&name;")));
			QVERIFY(info.definition.contains(QByteArrayLiteral("&desc;")));
			QCOMPARE(processor.customElementCount(), 1);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_TelnetProcessor_Mxp)

#include "tst_TelnetProcessor_Mxp.moc"
