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

	QByteArray bytes(std::initializer_list<unsigned char> raw)
	{
		QByteArray out;
		for (const unsigned char c : raw)
			out.append(static_cast<char>(c));
		return out;
	}
} // namespace

/**
 * @brief QTest fixture covering TelnetProcessor Subnegotiation scenarios.
 */
class tst_TelnetProcessor_Subnegotiation : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void genericSubnegotiationCallback()
		{
			TelnetProcessor  processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());

			processor.processBytes(bytes({IAC, SB, 200, 'a', 'b', IAC, SE}));
			QCOMPARE(spy.subnegotiations.size(), 1);
			QCOMPARE(spy.subnegotiations.at(0).option, 200);
			QCOMPARE(spy.subnegotiations.at(0).data, QByteArrayLiteral("ab"));
		}

		void mudSpecificCallsOptionAndSubnegotiation()
		{
			TelnetProcessor  processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());

			processor.processBytes(bytes({IAC, SB, TELOPT_MUD_SPECIFIC, 'x', IAC, IAC, 'y', IAC, SE}));
			QCOMPARE(spy.telnetOptions.size(), 1);
			QCOMPARE(spy.subnegotiations.size(), 1);
			QCOMPARE(spy.subnegotiations.at(0).option, TELOPT_MUD_SPECIFIC);
			QCOMPARE(spy.subnegotiations.at(0).data, QByteArray("x\xFFy", 3));
			QCOMPARE(spy.telnetOptions.at(0), QByteArray("x\xFFy", 3));
		}

		void subnegotiationCanSpanPackets()
		{
			TelnetProcessor  processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());

			processor.processBytes(bytes({IAC, SB, 200, 'a'}));
			QCOMPARE(spy.subnegotiations.size(), 0);

			processor.processBytes(bytes({'b', IAC, SE}));
			QCOMPARE(spy.subnegotiations.size(), 1);
			QCOMPARE(spy.subnegotiations.at(0).option, 200);
			QCOMPARE(spy.subnegotiations.at(0).data, QByteArrayLiteral("ab"));
		}

		void terminalTypeSubnegotiationDoesNotCallGenericCallback()
		{
			TelnetProcessor  processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());

			processor.processBytes(bytes({IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_SEND, IAC, SE}));
			QCOMPARE(spy.subnegotiations.size(), 0);
			QVERIFY(!processor.takeOutboundData().isEmpty());
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_TelnetProcessor_Subnegotiation)



#if __has_include("tst_TelnetProcessor_Subnegotiation.moc")
#include "tst_TelnetProcessor_Subnegotiation.moc"
#endif
