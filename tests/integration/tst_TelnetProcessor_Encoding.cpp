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
	constexpr unsigned char TTYPE_SEND           = 1;
	constexpr unsigned char TTYPE_IS             = 0;

	QByteArray bytes(std::initializer_list<unsigned char> raw)
	{
		QByteArray out;
		for (const unsigned char c : raw)
			out.append(static_cast<char>(c));
		return out;
	}
} // namespace

/**
 * @brief QTest fixture covering TelnetProcessor Encoding scenarios.
 */
class tst_TelnetProcessor_Encoding : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void escapedIacInNormalText()
		{
			TelnetProcessor processor;
			const QByteArray output = processor.processBytes(bytes({'a', IAC, IAC, 'b'}));
			QByteArray expected;
			expected.append('a');
			expected.append(static_cast<char>(IAC));
			expected.append('b');
			QCOMPARE(output, expected);
		}

		void gaHandlingWithAndWithoutNewline()
		{
			TelnetProcessor  processor;
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
			processor.processBytes(
			    bytes({IAC, SB, TELOPT_CHARSET, CHARSET_REQUEST, ',', 'U', 'T', 'F', '-', '8', ',', 'U', 'S', '-',
			           'A', 'S', 'C', 'I', 'I', IAC, SE}));
			QCOMPARE(processor.takeOutboundData(),
			         bytes({IAC, SB, TELOPT_CHARSET, CHARSET_ACCEPTED, 'U', 'S', '-', 'A', 'S', 'C', 'I', 'I', IAC, SE}));

			processor.setUseUtf8(true);
			processor.processBytes(
			    bytes({IAC, SB, TELOPT_CHARSET, CHARSET_REQUEST, ',', 'U', 'T', 'F', '-', '8', ',', 'U', 'S', '-',
			           'A', 'S', 'C', 'I', 'I', IAC, SE}));
			QCOMPARE(processor.takeOutboundData(),
			         bytes({IAC, SB, TELOPT_CHARSET, CHARSET_ACCEPTED, 'U', 'T', 'F', '-', '8', IAC, SE}));
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
			QCOMPARE(processor.takeOutboundData(),
			         bytes({IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_IS, 'M', 'T', 'T', 'S', ' ', '2', '6', '9', IAC, SE}));
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_TelnetProcessor_Encoding)

#include "tst_TelnetProcessor_Encoding.moc"
