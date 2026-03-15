/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_TelnetProcessor_Options.cpp
 * Role: QTest coverage for TelnetProcessor Options behavior.
 */

#include "TelnetProcessor.h"

#include <QtTest/QTest>

namespace
{
	constexpr unsigned char IAC                  = 0xFF;
	constexpr unsigned char DO                   = 0xFD;
	constexpr unsigned char DONT                 = 0xFE;
	constexpr unsigned char WONT                 = 0xFC;
	constexpr unsigned char WILL                 = 0xFB;
	constexpr unsigned char SB                   = 0xFA;
	constexpr unsigned char GA                   = 0xF9;
	constexpr unsigned char SE                   = 0xF0;
	constexpr unsigned char TELOPT_ECHO          = 1;
	constexpr unsigned char SGA                  = 3;
	constexpr unsigned char WILL_END_OF_RECORD   = 25;
	constexpr unsigned char TELOPT_NAWS          = 31;
	constexpr unsigned char TELOPT_CHARSET       = 42;
	constexpr unsigned char TELOPT_TERMINAL_TYPE = 24;
	constexpr unsigned char CHARSET_REQUEST      = 1;
	constexpr unsigned char CHARSET_ACCEPTED     = 2;
	constexpr unsigned char CHARSET_REJECTED     = 3;
	constexpr unsigned char TTYPE_SEND           = 1;
	constexpr unsigned char TTYPE_IS             = 0;

	QByteArray bytes(std::initializer_list<unsigned char> raw)
	{
		QByteArray out;
		out.reserve(static_cast<qsizetype>(raw.size()));
		for (const unsigned char c : raw)
			out.append(static_cast<char>(c));
		return out;
	}
} // namespace

/**
 * @brief QTest fixture covering TelnetProcessor Options scenarios.
 */
class tst_TelnetProcessor_Options : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void queueInitialNegotiationIsIdempotent()
		{
			TelnetProcessor processor;
			processor.queueInitialNegotiation(true, true);
			QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, SGA, IAC, DO, WILL_END_OF_RECORD}));

			processor.queueInitialNegotiation(true, true);
			QVERIFY(processor.takeOutboundData().isEmpty());
		}

		void echoNegotiationCallbacksAndReplies()
		{
			TelnetProcessor  processor;
			QList<bool>      noEchoStates;
			TelnetProcessor::Callbacks callbacks;
			callbacks.onNoEchoChanged = [&noEchoStates](const bool enabled) { noEchoStates.append(enabled); };
			processor.setCallbacks(callbacks);

			processor.processBytes(bytes({IAC, WILL, TELOPT_ECHO}));
			QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, TELOPT_ECHO}));
			QCOMPARE(noEchoStates, QList<bool>{true});

			processor.processBytes(bytes({IAC, WONT, TELOPT_ECHO}));
			QCOMPARE(processor.takeOutboundData(), bytes({IAC, DONT, TELOPT_ECHO}));
			QCOMPARE(noEchoStates, QList<bool>({true, false}));
		}

		void noEchoOffRejectsEchoNegotiation()
		{
			TelnetProcessor processor;
			bool            callbackFired = false;

			TelnetProcessor::Callbacks callbacks;
			callbacks.onNoEchoChanged = [&callbackFired](bool) { callbackFired = true; };
			processor.setCallbacks(callbacks);
			processor.setNoEchoOff(true);

			processor.processBytes(bytes({IAC, WILL, TELOPT_ECHO}));
			QCOMPARE(processor.takeOutboundData(), bytes({IAC, DONT, TELOPT_ECHO}));
			QVERIFY(!callbackFired);
		}

		void noEchoStaysEnabledAcrossIncomingData()
		{
			TelnetProcessor  processor;
			QList<bool>      noEchoStates;
			TelnetProcessor::Callbacks callbacks;
			callbacks.onNoEchoChanged = [&noEchoStates](const bool enabled) { noEchoStates.append(enabled); };
			processor.setCallbacks(callbacks);

			processor.processBytes(bytes({IAC, WILL, TELOPT_ECHO}));
			QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, TELOPT_ECHO}));
			QCOMPARE(noEchoStates, QList<bool>{true});

			QCOMPARE(processor.processBytes(QByteArray("secret input")), QByteArray("secret input"));
			QVERIFY(processor.takeOutboundData().isEmpty());
			QCOMPARE(noEchoStates, QList<bool>{true});

			processor.processBytes(bytes({IAC, WONT, TELOPT_ECHO}));
			QCOMPARE(processor.takeOutboundData(), bytes({IAC, DONT, TELOPT_ECHO}));
			QCOMPARE(noEchoStates, QList<bool>({true, false}));
		}

		void resetConnectionStateClearsNoEchoOnce()
		{
			TelnetProcessor  processor;
			QList<bool>      noEchoStates;
			TelnetProcessor::Callbacks callbacks;
			callbacks.onNoEchoChanged = [&noEchoStates](const bool enabled) { noEchoStates.append(enabled); };
			processor.setCallbacks(callbacks);

			processor.processBytes(bytes({IAC, WILL, TELOPT_ECHO}));
			QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, TELOPT_ECHO}));
			QCOMPARE(noEchoStates, QList<bool>{true});

			processor.resetConnectionState();
			QCOMPARE(noEchoStates, QList<bool>({true, false}));

			processor.resetConnectionState();
			QCOMPARE(noEchoStates, QList<bool>({true, false}));
		}

		void doNawsSendsWillAndWindowSizeWhenEnabled()
		{
			TelnetProcessor processor;
			processor.setNawsEnabled(true);
			processor.setWindowSize(80, 24);

			processor.processBytes(bytes({IAC, DO, TELOPT_NAWS}));
			QCOMPARE(processor.takeOutboundData(),
			         bytes({IAC, WILL, TELOPT_NAWS, IAC, SB, TELOPT_NAWS, 0x00, 0x50, 0x00, 0x18, IAC, SE}));
		}

		void doNawsEscapesIacBytesInWindowSizePayload()
		{
			TelnetProcessor processor;
			processor.setNawsEnabled(true);
			processor.setWindowSize(255, 255);

			processor.processBytes(bytes({IAC, DO, TELOPT_NAWS}));
			QCOMPARE(processor.takeOutboundData(),
			         bytes({IAC, WILL, TELOPT_NAWS, IAC, SB, TELOPT_NAWS, 0x00, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, IAC, SE}));
		}

		void doNawsSendsWontWhenDisabled()
		{
			TelnetProcessor processor;
			processor.setNawsEnabled(false);

			processor.processBytes(bytes({IAC, DO, TELOPT_NAWS}));
			QCOMPARE(processor.takeOutboundData(), bytes({IAC, WONT, TELOPT_NAWS}));
		}

		void terminalTypeRequestReturnsConfiguredName()
		{
			TelnetProcessor processor;
			processor.setTerminalIdentification(QStringLiteral("QMudTerm"));

			processor.processBytes(bytes({IAC, DO, TELOPT_TERMINAL_TYPE}));
			QCOMPARE(processor.takeOutboundData(), bytes({IAC, WILL, TELOPT_TERMINAL_TYPE}));

			processor.processBytes(bytes({IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_SEND, IAC, SE}));
			QCOMPARE(processor.takeOutboundData(),
			         bytes({IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_IS, 'Q', 'M', 'u', 'd', 'T', 'e', 'r', 'm', IAC, SE}));
		}

		void charsetRequestAcceptedAndRejected()
		{
			TelnetProcessor processor;
			processor.setUseUtf8(true);

			processor.processBytes(
			    bytes({IAC, SB, TELOPT_CHARSET, CHARSET_REQUEST, ',', 'U', 'T', 'F', '-', '8', ',', 'U', 'S', '-',
			           'A', 'S', 'C', 'I', 'I', IAC, SE}));
			QCOMPARE(processor.takeOutboundData(),
			         bytes({IAC, SB, TELOPT_CHARSET, CHARSET_ACCEPTED, 'U', 'T', 'F', '-', '8', IAC, SE}));

			processor.processBytes(
			    bytes({IAC, SB, TELOPT_CHARSET, CHARSET_REQUEST, ',', 'U', 'S', '-', 'A', 'S', 'C', 'I', 'I', IAC, SE}));
			QCOMPARE(processor.takeOutboundData(), bytes({IAC, SB, TELOPT_CHARSET, CHARSET_REJECTED, IAC, SE}));
		}

		void gaCanConvertToNewline()
		{
			TelnetProcessor processor;
			int             gaCount = 0;

			TelnetProcessor::Callbacks callbacks;
			callbacks.onIacGa = [&gaCount]() { ++gaCount; };
			processor.setCallbacks(callbacks);
			processor.setConvertGAtoNewline(true);

			const QByteArray output = processor.processBytes(bytes({IAC, GA}));
			QCOMPARE(output, QByteArray("\n"));
			QCOMPARE(gaCount, 1);
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_TelnetProcessor_Options)

#include "tst_TelnetProcessor_Options.moc"
