/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_TelnetProcessor_Subnegotiation.cpp
 * Role: QTest coverage for TelnetProcessor Subnegotiation behavior.
 */

#include "TelnetCallbackSpy.h"
#include "TelnetProcessor.h"

#include <QtTest/QTest>

namespace
{
	constexpr unsigned char IAC                  = 0xFF;
	constexpr unsigned char SB                   = 0xFA;
	constexpr unsigned char SE                   = 0xF0;
	constexpr unsigned char TELOPT_MUD_SPECIFIC  = 102;
	constexpr unsigned char TELOPT_TERMINAL_TYPE = 24;
	constexpr unsigned char TTYPE_SEND           = 1;

	QByteArray              bytes(std::initializer_list<unsigned char> raw)
	{
		QByteArray out;
		for (const unsigned char c : raw)
			out.append(static_cast<char>(c));
		return out;
	}
	/**
	 * @brief QTest fixture covering TelnetProcessor Subnegotiation scenarios.
	 */
	class tst_TelnetProcessor_Subnegotiation : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			/**
			 * @brief Restores every split point of Telnet tokens without replaying callbacks or packet transforms.
			 */
			static void incompleteInputState()
			{
				const QList<QByteArray> tokens = {bytes({IAC, SB, 200, 'a', IAC, IAC, 'b', IAC, SE}),
				                                  bytes({IAC, 251, 200}),
				                                  bytes({IAC, 252, 200}),
				                                  bytes({IAC, 253, 200}),
				                                  bytes({IAC, 254, 200}),
				                                  bytes({IAC, 249}),
				                                  bytes({IAC, IAC})};
				for (const auto &token : tokens)
					for (qsizetype split = 1; split < token.size(); ++split)
					{
						TelnetProcessor   baseline, original, restored;
						TelnetCallbackSpy baselineSpy, originalSpy, restoredSpy;
						baseline.setCallbacks(baselineSpy.callbacks());
						original.setCallbacks(originalSpy.callbacks());
						restored.setCallbacks(restoredSpy.callbacks());
						QByteArray originalTransformed, restoredTransformed;
						const auto prefix =
						    original.processBytes(token.left(split), [&](const QByteArray &value)
							                      { originalTransformed += value; });
						const auto saved = original.saveTelnetInputState();
						QVERIFY(!saved.isEmpty());
						QVERIFY(restored.restoreTelnetInputState(saved));
						QCOMPARE(restored.saveTelnetInputState(), saved);
						QVERIFY(restoredSpy.subnegotiations.isEmpty());
						QCOMPARE(restoredSpy.iacGaCount, 0);
						const auto suffix =
						    restored.processBytes(token.mid(split) + "tail", [&](const QByteArray &value)
							                      { restoredTransformed += value; });
						QCOMPARE(prefix + suffix, baseline.processBytes(token + "tail"));
						QCOMPARE(originalTransformed + restoredTransformed, token + "tail");
						QCOMPARE(restoredSpy.subnegotiations.size(), baselineSpy.subnegotiations.size());
						if (!baselineSpy.subnegotiations.isEmpty())
						{
							QCOMPARE(restoredSpy.subnegotiations.front().option,
							         baselineSpy.subnegotiations.front().option);
							QCOMPARE(restoredSpy.subnegotiations.front().data,
							         baselineSpy.subnegotiations.front().data);
						}
						QCOMPARE(restoredSpy.iacGaCount, baselineSpy.iacGaCount);
						QCOMPARE(restored.takeOutboundData(), baseline.takeOutboundData());
						QVERIFY(restored.saveTelnetInputState().isEmpty());
						QVERIFY(!restored.restoreTelnetInputState(QByteArrayLiteral("invalid")));
						QVERIFY(restored.saveTelnetInputState().isEmpty());
						original.resetConnectionState();
						QVERIFY(original.saveTelnetInputState().isEmpty());
					}
			}

			void genericSubnegotiationCallback()
			{
				TelnetProcessor   processor;
				TelnetCallbackSpy spy;
				processor.setCallbacks(spy.callbacks());

				processor.processBytes(bytes({IAC, SB, 200, 'a', 'b', IAC, SE}));
				QCOMPARE(spy.subnegotiations.size(), 1);
				QCOMPARE(spy.subnegotiations.at(0).option, 200);
				QCOMPARE(spy.subnegotiations.at(0).data, QByteArrayLiteral("ab"));
			}

			void subnegotiationPluginEventRecordsProcessedStreamOffset()
			{
				TelnetProcessor processor;

				QByteArray      input = QByteArrayLiteral("room text\r\n");
				input += bytes({IAC, SB, 201, 'r', 'o', 'o', 'm', IAC, SE});
				input += QByteArrayLiteral("prompt");

				const QByteArray output = processor.processBytes(input);
				QCOMPARE(output, QByteArrayLiteral("room text\r\nprompt"));

				const QList<TelnetProcessor::TelnetStreamEvent> events = processor.takeTelnetStreamEvents();
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.at(0).type, TelnetProcessor::TelnetStreamEvent::Subnegotiation);
				QCOMPARE(events.at(0).option, 201);
				QCOMPARE(events.at(0).data, QByteArrayLiteral("room"));
				QCOMPARE(events.at(0).offset, QByteArrayLiteral("room text\r\n").size());
			}

			void mudSpecificCallsOptionAndSubnegotiation()
			{
				TelnetProcessor   processor;
				TelnetCallbackSpy spy;
				processor.setCallbacks(spy.callbacks());

				processor.processBytes(bytes({IAC, SB, TELOPT_MUD_SPECIFIC, 'x', IAC, IAC, 'y', IAC, SE}));
				QCOMPARE(spy.telnetOptions.size(), 1);
				QCOMPARE(spy.subnegotiations.size(), 1);
				QCOMPARE(spy.subnegotiations.at(0).option, TELOPT_MUD_SPECIFIC);
				QCOMPARE(spy.subnegotiations.at(0).data, QByteArray("x\xFFy", 3));
				QCOMPARE(spy.telnetOptions.at(0), QByteArray("x\xFFy", 3));
			}

			void mudSpecificPluginEventsKeepOptionBeforeSubnegotiation()
			{
				TelnetProcessor  processor;

				const QByteArray output =
				    processor.processBytes(bytes({'a', IAC, SB, TELOPT_MUD_SPECIFIC, 'x', IAC, SE, 'b'}));
				QCOMPARE(output, QByteArrayLiteral("ab"));

				const QList<TelnetProcessor::TelnetStreamEvent> events = processor.takeTelnetStreamEvents();
				QCOMPARE(events.size(), 2);
				QCOMPARE(events.at(0).type, TelnetProcessor::TelnetStreamEvent::Option);
				QCOMPARE(events.at(1).type, TelnetProcessor::TelnetStreamEvent::Subnegotiation);
				QCOMPARE(events.at(0).offset, 1);
				QCOMPARE(events.at(1).offset, 1);
				QVERIFY(events.at(0).sequence < events.at(1).sequence);
			}

			void subnegotiationCanSpanPackets()
			{
				TelnetProcessor   processor;
				TelnetCallbackSpy spy;
				processor.setCallbacks(spy.callbacks());

				processor.processBytes(bytes({IAC, SB, 200, 'a'}));
				QCOMPARE(spy.subnegotiations.size(), 0);

				processor.processBytes(bytes({'b', IAC, SE}));
				QCOMPARE(spy.subnegotiations.size(), 1);
				QCOMPARE(spy.subnegotiations.at(0).option, 200);
				QCOMPARE(spy.subnegotiations.at(0).data, QByteArrayLiteral("ab"));
			}

			void splitSubnegotiationEscapedIacDoesNotStartMccp()
			{
				TelnetProcessor processor;

				processor.processBytes(bytes({IAC, SB, 200, 'a'}));

				QList<QByteArray> seenPackets;
				const QByteArray  secondPacket = bytes({IAC, IAC, SB, 86, 'b', IAC, SE});
				const QByteArray  output = processor.processBytes(secondPacket, [&](const QByteArray &payload)
				                                                  { seenPackets.append(payload); });

				QCOMPARE(output, QByteArray());
				QCOMPARE(seenPackets.size(), 1);
				QCOMPARE(seenPackets.constFirst(), secondPacket);
				QVERIFY(!processor.isCompressing());

				const QList<TelnetProcessor::TelnetStreamEvent> events = processor.takeTelnetStreamEvents();
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constFirst().type, TelnetProcessor::TelnetStreamEvent::Subnegotiation);
				QCOMPARE(events.constFirst().option, 200);
				QCOMPARE(events.constFirst().data, bytes({'a', IAC, SB, 86, 'b'}));
			}

			void terminalTypeSubnegotiationDoesNotCallGenericCallback()
			{
				TelnetProcessor   processor;
				TelnetCallbackSpy spy;
				processor.setCallbacks(spy.callbacks());

				processor.processBytes(bytes({IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_SEND, IAC, SE}));
				QCOMPARE(spy.subnegotiations.size(), 0);
				QVERIFY(processor.takeTelnetStreamEvents().isEmpty());
				QVERIFY(!processor.takeOutboundData().isEmpty());
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};

} // namespace

QTEST_APPLESS_MAIN(tst_TelnetProcessor_Subnegotiation)

#include "tst_TelnetProcessor_Subnegotiation.moc"
