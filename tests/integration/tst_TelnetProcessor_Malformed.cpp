/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_TelnetProcessor_Malformed.cpp
 * Role: QTest coverage for TelnetProcessor Malformed behavior.
 */

#include "TelnetCallbackSpy.h"
#include "TelnetProcessor.h"

#include <QtTest/QTest>

namespace
{
	constexpr unsigned char IAC              = 0xFF;
	constexpr unsigned char SB               = 0xFA;
	constexpr unsigned char SE               = 0xF0;
	constexpr unsigned char WILL             = 0xFB;
	constexpr unsigned char TELOPT_COMPRESS  = 85;

	QByteArray bytes(std::initializer_list<unsigned char> raw)
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
			TelnetProcessor  processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(2);

			// The nested '<' inside an element forces an MXP unterminated-element diagnostic path.
			processor.processBytes(QByteArrayLiteral("<<a>"));
			QVERIFY(!spy.mxpDiagnostics.isEmpty());
			QVERIFY(spy.fatalProtocolErrors.isEmpty());
		}

		void truncatedSubnegotiationDoesNotEmitUntilTerminated()
		{
			TelnetProcessor  processor;
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

