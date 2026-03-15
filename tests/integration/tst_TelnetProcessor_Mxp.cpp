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
	constexpr unsigned char IAC       = 0xFF;
	constexpr unsigned char SB        = 0xFA;
	constexpr unsigned char SE        = 0xF0;
	constexpr unsigned char TELOPT_MXP = 91;

	QByteArray bytes(std::initializer_list<unsigned char> raw)
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
			TelnetProcessor  processor;
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
			TelnetProcessor  processor;
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
			TelnetProcessor  processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());

			processor.activatePuebloMode();
			QVERIFY(processor.isMxpEnabled());
			QVERIFY(processor.isPuebloActive());
			QCOMPARE(spy.mxpStarts.size(), 1);
			QVERIFY(spy.mxpStarts.at(0).pueblo);
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_TelnetProcessor_Mxp)

#include "tst_TelnetProcessor_Mxp.moc"

