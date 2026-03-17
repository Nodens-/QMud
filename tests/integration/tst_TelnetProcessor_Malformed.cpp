/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_TelnetProcessor_Malformed.cpp
 * Role: QTest coverage for TelnetProcessor Malformed behavior.
 */

#include "MxpDiagnostics.h"
#include "TelnetCallbackSpy.h"
#include "TelnetProcessor.h"

#include <QtTest/QTest>

namespace
{
	constexpr unsigned char IAC             = 0xFF;
	constexpr unsigned char SB              = 0xFA;
	constexpr unsigned char SE              = 0xF0;
	constexpr unsigned char WILL            = 0xFB;
	constexpr unsigned char TELOPT_COMPRESS = 85;

	QByteArray              bytes(std::initializer_list<unsigned char> raw)
	{
		QByteArray out;
		for (const unsigned char c : raw)
			out.append(static_cast<char>(c));
		return out;
	}
} // namespace

/**
 * @brief QTest fixture covering TelnetProcessor Malformed scenarios.
 */
class tst_TelnetProcessor_Malformed : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void unexpectedIacSequenceRecovers()
		{
			TelnetProcessor processor;
			QCOMPARE(processor.processBytes(bytes({IAC, 0x01})), QByteArray());
			QCOMPARE(processor.processBytes(QByteArrayLiteral("ok")), QByteArrayLiteral("ok"));
		}

		void malformedMccp1HandshakeDoesNotBreakParser()
		{
			TelnetProcessor processor;
			processor.processBytes(bytes({IAC, SB, TELOPT_COMPRESS, WILL, 'x'}));
			QCOMPARE(processor.processBytes(QByteArrayLiteral("plain")), QByteArrayLiteral("plain"));
		}

		void malformedMxpElementEmitsDiagnosticButNoFatal()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(2);

			// The nested '<' inside an element forces an MXP unterminated-element diagnostic path.
			processor.processBytes(QByteArrayLiteral("<<a>"));
			QVERIFY(!spy.mxpDiagnostics.isEmpty());
			QVERIFY(spy.fatalProtocolErrors.isEmpty());
		}

		void oversizedMxpElementCollectionResetsPhaseAndRecovers()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(2);

			auto oversized = QByteArrayLiteral("<");
			oversized.append(QByteArray(9000, 'a'));
			processor.processBytes(oversized);

			bool sawOverflowDiagnostic = false;
			for (const TelnetCallbackSpy::MxpDiagnostic &diag : spy.mxpDiagnostics)
			{
				if (diag.messageNumber == errMXP_CollectionTooLong)
				{
					sawOverflowDiagnostic = true;
					break;
				}
			}
			QVERIFY(sawOverflowDiagnostic);
			QCOMPARE(processor.processBytes(QByteArrayLiteral("ok")), QByteArrayLiteral("ok"));
			QVERIFY(spy.fatalProtocolErrors.isEmpty());
		}

		void oversizedMxpEntityCollectionResetsPhaseAndRecovers()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(2);

			auto oversized = QByteArrayLiteral("&");
			oversized.append(QByteArray(9000, 'a'));
			processor.processBytes(oversized);

			bool sawOverflowDiagnostic = false;
			for (const TelnetCallbackSpy::MxpDiagnostic &diag : spy.mxpDiagnostics)
			{
				if (diag.messageNumber == errMXP_CollectionTooLong)
				{
					sawOverflowDiagnostic = true;
					break;
				}
			}
			QVERIFY(sawOverflowDiagnostic);
			QCOMPARE(processor.processBytes(QByteArrayLiteral("ok")), QByteArrayLiteral("ok"));
			QVERIFY(spy.fatalProtocolErrors.isEmpty());
		}

		void oversizedMxpEventQueueIsCappedAndRecovers()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(2);

			QByteArray burst;
			burst.reserve(2500 * 8);
			for (int i = 0; i < 2500; ++i)
				burst.append(QByteArrayLiteral("<b>x</b>"));

			const QByteArray output = processor.processBytes(burst);
			QCOMPARE(output.size(), 2500);

			bool sawQueueOverflowDiagnostic = false;
			for (const TelnetCallbackSpy::MxpDiagnostic &diag : spy.mxpDiagnostics)
			{
				if (diag.messageNumber == wrnMXP_EventQueueLimitExceeded)
				{
					sawQueueOverflowDiagnostic = true;
					break;
				}
			}
			QVERIFY(sawQueueOverflowDiagnostic);

			const QList<TelnetProcessor::MxpEvent> events = processor.takeMxpEvents();
			QVERIFY(events.size() <= 4096);
			QVERIFY(!events.isEmpty());

			QCOMPARE(processor.processBytes(QByteArrayLiteral("<b>y</b>")), QByteArrayLiteral("y"));
			QCOMPARE(processor.takeMxpEvents().size(), 2);
		}

		void customEntityDefinitionLimitIsEnforced()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());

			for (int i = 0; i < 1100; ++i)
			{
				const QByteArray key = QStringLiteral("entity_%1").arg(i).toUtf8();
				processor.setCustomEntity(key, QByteArrayLiteral("x"));
			}

			QCOMPARE(processor.customEntityCount(), 1024);
			bool sawLimitDiagnostic = false;
			for (const TelnetCallbackSpy::MxpDiagnostic &diag : spy.mxpDiagnostics)
			{
				if (diag.messageNumber == wrnMXP_CustomDefinitionLimitExceeded)
				{
					sawLimitDiagnostic = true;
					break;
				}
			}
			QVERIFY(sawLimitDiagnostic);

			QByteArray value;
			QVERIFY(processor.getCustomEntityValue(QByteArrayLiteral("entity_0"), value));
			QCOMPARE(value, QByteArrayLiteral("x"));

			processor.setCustomEntity(QByteArrayLiteral("entity_0"), QByteArrayLiteral("updated"));
			QVERIFY(processor.getCustomEntityValue(QByteArrayLiteral("entity_0"), value));
			QCOMPARE(value, QByteArrayLiteral("updated"));

			processor.setCustomEntity(QByteArrayLiteral("entity_1"), QByteArray());
			QCOMPARE(processor.customEntityCount(), 1023);
			processor.setCustomEntity(QByteArrayLiteral("entity_new"), QByteArrayLiteral("x"));
			QCOMPARE(processor.customEntityCount(), 1024);
		}

		void attlistDefinitionGrowthIsCapped()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(2);

			processor.processBytes(QByteArrayLiteral("\x1b[1z<!ELEMENT foo>"));
			for (int i = 0; i < 80; ++i)
			{
				QByteArray packet = QByteArrayLiteral("\x1b[1z<!ATTLIST foo x = ");
				packet.append(QByteArray(300, 'a'));
				packet.append('>');
				processor.processBytes(packet);
			}

			bool sawAttlistLimitDiagnostic = false;
			for (const TelnetCallbackSpy::MxpDiagnostic &diag : spy.mxpDiagnostics)
			{
				if (diag.messageNumber == wrnMXP_AttlistLimitExceeded)
				{
					sawAttlistLimitDiagnostic = true;
					break;
				}
			}
			QVERIFY(sawAttlistLimitDiagnostic);

			TelnetProcessor::CustomElementInfo info;
			QVERIFY(processor.getCustomElementInfo(QByteArrayLiteral("foo"), info));
			QVERIFY(info.attributes.size() <= 16384);

			const QByteArray before = info.attributes;
			QByteArray       packet = QByteArrayLiteral("\x1b[1z<!ATTLIST foo y = ");
			packet.append(QByteArray(5000, 'b'));
			packet.append('>');
			processor.processBytes(packet);
			QVERIFY(processor.getCustomElementInfo(QByteArrayLiteral("foo"), info));
			QCOMPARE(info.attributes, before);
		}

		void truncatedSubnegotiationDoesNotEmitUntilTerminated()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());

			processor.processBytes(bytes({IAC, SB, 200, 'a', 'b'}));
			QCOMPARE(spy.subnegotiations.size(), 0);

			processor.processBytes(bytes({IAC, SE}));
			QCOMPARE(spy.subnegotiations.size(), 1);
			QCOMPARE(spy.subnegotiations.at(0).data, QByteArrayLiteral("ab"));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_TelnetProcessor_Malformed)

#include "tst_TelnetProcessor_Malformed.moc"
