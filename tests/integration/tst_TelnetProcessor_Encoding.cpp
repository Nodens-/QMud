/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_TelnetProcessor_Encoding.cpp
 * Role: QTest coverage for TelnetProcessor Encoding behavior.
 */

#include "TelnetCallbackSpy.h"
#include "TelnetProcessor.h"

#include <QtTest/QTest>

namespace
{
	constexpr unsigned char IAC                  = 0xFF;
	constexpr unsigned char DO                   = 0xFD;
	constexpr unsigned char SB                   = 0xFA;
	constexpr unsigned char GA                   = 0xF9;
	constexpr unsigned char SE                   = 0xF0;
	constexpr unsigned char TELOPT_CHARSET       = 42;
	constexpr unsigned char TELOPT_TERMINAL_TYPE = 24;
	constexpr unsigned char CHARSET_REQUEST      = 1;
	constexpr unsigned char CHARSET_ACCEPTED     = 2;
	constexpr unsigned char CHARSET_REJECTED     = 3;
	constexpr unsigned char TTYPE_SEND           = 1;
	constexpr unsigned char TTYPE_IS             = 0;

	QByteArray              bytes(std::initializer_list<unsigned char> raw)
	{
		QByteArray out;
		for (const unsigned char c : raw)
			out.append(static_cast<char>(c));
		return out;
	}

	/**
	 * @brief QTest fixture covering TelnetProcessor Encoding scenarios.
	 */
	class tst_TelnetProcessor_Encoding : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			/** @brief Verifies escaped IAC bytes contribute to later processed-stream event offsets. */
			void escapedIacMaintainsProcessedStreamOffsets()
			{
				TelnetProcessor  processor;
				const QByteArray output = processor.processBytes(bytes({'a', IAC, IAC, 'b'}));
				QByteArray       expected;
				expected.append('a');
				expected.append(static_cast<char>(IAC));
				expected.append('b');
				QCOMPARE(output, expected);

				TelnetProcessor splitProcessor;
				QCOMPARE(splitProcessor.processBytes(bytes({IAC})), QByteArray());
				QCOMPARE(splitProcessor.processBytes(bytes({IAC, IAC, GA})), bytes({IAC}));
				const QList<TelnetProcessor::TelnetStreamEvent> events =
				    splitProcessor.takeTelnetStreamEvents();
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constFirst().type, TelnetProcessor::TelnetStreamEvent::IacGa);
				QCOMPARE(events.constFirst().offset, 1);
			}

			void gaHandlingWithAndWithoutNewline()
			{
				TelnetProcessor   processor;
				TelnetCallbackSpy spy;
				processor.setCallbacks(spy.callbacks());

				QCOMPARE(processor.processBytes(bytes({IAC, GA})), QByteArray());
				QCOMPARE(spy.iacGaCount, 1);

				processor.setConvertGAtoNewline(true);
				QCOMPARE(processor.processBytes(bytes({IAC, GA})), QByteArray("\n"));
				QCOMPARE(spy.iacGaCount, 2);
			}

			void charsetSelectionRespectsUtf8Toggle()
			{
				TelnetProcessor processor;

				processor.setUseUtf8(false);
				processor.processBytes(bytes({IAC,
				                              SB,
				                              TELOPT_CHARSET,
				                              CHARSET_REQUEST,
				                              ',',
				                              'U',
				                              'T',
				                              'F',
				                              '-',
				                              '8',
				                              ',',
				                              'U',
				                              'S',
				                              '-',
				                              'A',
				                              'S',
				                              'C',
				                              'I',
				                              'I',
				                              IAC,
				                              SE}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, SB, TELOPT_CHARSET, CHARSET_ACCEPTED, 'U',
				                                              'S', '-', 'A', 'S', 'C', 'I', 'I', IAC, SE}));

				processor.setUseUtf8(true);
				processor.processBytes(bytes({IAC,
				                              SB,
				                              TELOPT_CHARSET,
				                              CHARSET_REQUEST,
				                              ',',
				                              'U',
				                              'T',
				                              'F',
				                              '-',
				                              '8',
				                              ',',
				                              'U',
				                              'S',
				                              '-',
				                              'A',
				                              'S',
				                              'C',
				                              'I',
				                              'I',
				                              IAC,
				                              SE}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, SB, TELOPT_CHARSET, CHARSET_ACCEPTED, 'U',
				                                              'T', 'F', '-', '8', IAC, SE}));
			}

			void charsetSelectionUsesPreferredNames()
			{
				TelnetProcessor processor;
				processor.setUseUtf8(false);
				processor.setPreferredCharsetNames({QByteArrayLiteral("GB18030"), QByteArrayLiteral("GBK")});

				processor.processBytes(bytes({IAC, SB, TELOPT_CHARSET, CHARSET_REQUEST, ',', 'U', 'T', 'F',
				                              '-', '8', ',', 'G', 'B', 'K', IAC, SE}));
				QCOMPARE(processor.takeOutboundData(),
				         bytes({IAC, SB, TELOPT_CHARSET, CHARSET_ACCEPTED, 'G', 'B', 'K', IAC, SE}));

				processor.processBytes(bytes({IAC,
				                              SB,
				                              TELOPT_CHARSET,
				                              CHARSET_REQUEST,
				                              ',',
				                              'U',
				                              'T',
				                              'F',
				                              '-',
				                              '8',
				                              ',',
				                              'S',
				                              'h',
				                              'i',
				                              'f',
				                              't',
				                              '-',
				                              'J',
				                              'I',
				                              'S',
				                              IAC,
				                              SE}));
				QCOMPARE(processor.takeOutboundData(),
				         bytes({IAC, SB, TELOPT_CHARSET, CHARSET_REJECTED, IAC, SE}));
			}

			void terminalTypeSequenceChangesWithUtf8()
			{
				TelnetProcessor processor;
				processor.setUseUtf8(true);

				processor.processBytes(bytes({IAC, DO, TELOPT_TERMINAL_TYPE}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, 0xFB, TELOPT_TERMINAL_TYPE}));

				processor.processBytes(bytes({IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_SEND, IAC, SE}));
				QCOMPARE(processor.takeOutboundData(),
				         bytes({IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_IS, 'Q', 'M', 'u', 'd', IAC, SE}));

				processor.processBytes(bytes({IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_SEND, IAC, SE}));
				QCOMPARE(processor.takeOutboundData(),
				         bytes({IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_IS, 'A', 'N', 'S', 'I', IAC, SE}));

				processor.processBytes(bytes({IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_SEND, IAC, SE}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_IS, 'M',
				                                              'T', 'T', 'S', ' ', '2', '6', '9', IAC, SE}));
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};

} // namespace

QTEST_APPLESS_MAIN(tst_TelnetProcessor_Encoding)

#include "tst_TelnetProcessor_Encoding.moc"
