/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_TelnetProcessor_Options.cpp
 * Role: QTest coverage for TelnetProcessor Options behavior.
 */

#include "TelnetProcessor.h"

#include <QtTest/QTest>

#include <array>

/**
 * @brief Provides deterministic access to TelnetProcessor's monotonic renegotiation limiter.
 */
class TelnetProcessorTestAccess
{
	public:
		enum class Disposition
		{
			Proceed,
			SettleAndSuppress,
			Suppress
		};

		/**
		 * @brief Records one negotiation attempt at a supplied monotonic timestamp.
		 * @param processor Processor whose limiter is exercised.
		 * @param serverOption Selects WILL/WONT direction when `true`, DO/DONT otherwise.
		 * @param option Telnet option code.
		 * @param nowMilliseconds Monotonic timestamp to record.
		 * @return Public test representation of the limiter decision.
		 */
		static Disposition record(TelnetProcessor &processor, const bool serverOption,
		                          const unsigned char option, const qint64 nowMilliseconds)
		{
			const auto direction = serverOption ? TelnetProcessor::NegotiationDirection::ServerOption
			                                    : TelnetProcessor::NegotiationDirection::ClientOption;
			switch (processor.recordNegotiationAttempt(direction, option, nowMilliseconds))
			{
			case TelnetProcessor::NegotiationDisposition::Proceed:
				return Disposition::Proceed;
			case TelnetProcessor::NegotiationDisposition::SettleAndSuppress:
				return Disposition::SettleAndSuppress;
			case TelnetProcessor::NegotiationDisposition::Suppress:
				return Disposition::Suppress;
			}
			Q_UNREACHABLE_RETURN(Disposition::Proceed);
		}
};

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
	constexpr unsigned char TELOPT_START_TLS     = 46;
	constexpr unsigned char TELOPT_TERMINAL_TYPE = 24;
	constexpr unsigned char TELOPT_COMPRESS2     = 86;
	constexpr unsigned char CHARSET_REQUEST      = 1;
	constexpr unsigned char CHARSET_ACCEPTED     = 2;
	constexpr unsigned char CHARSET_REJECTED     = 3;
	constexpr unsigned char START_TLS_FOLLOWS    = 1;
	constexpr unsigned char TTYPE_SEND           = 1;
	constexpr unsigned char TTYPE_IS             = 0;

	QByteArray              bytes(std::initializer_list<unsigned char> raw)
	{
		QByteArray out;
		out.reserve(static_cast<qsizetype>(raw.size()));
		for (const unsigned char c : raw)
			out.append(static_cast<char>(c));
		return out;
	}
	/**
	 * @brief QTest fixture covering TelnetProcessor Options scenarios.
	 */
	class tst_TelnetProcessor_Options : public QObject
	{
			Q_OBJECT

		private slots:
			static void queueInitialNegotiationIsIdempotent()
			{
				TelnetProcessor processor;
				processor.queueInitialNegotiation(true, true);
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, SGA, IAC, DO, WILL_END_OF_RECORD}));

				processor.queueInitialNegotiation(true, true);
				QVERIFY(processor.takeOutboundData().isEmpty());
			}

			static void repeatedSgaNegotiationRemainsEnabledByDefault()
			{
				TelnetProcessor processor;

				processor.processBytes(bytes({IAC, WILL, SGA}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, SGA}));

				processor.processBytes(bytes({IAC, WILL, SGA}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, SGA}));
			}

			static void repeatedSgaNegotiationCanBeSuppressedWhenConfigured()
			{
				TelnetProcessor processor;
				processor.setNegotiateOptionsOnce(true);

				processor.processBytes(bytes({IAC, WILL, SGA}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, SGA}));

				processor.processBytes(bytes({IAC, WILL, SGA}));
				QVERIFY(processor.takeOutboundData().isEmpty());

				processor.processBytes(bytes({IAC, DO, SGA}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WILL, SGA}));

				processor.processBytes(bytes({IAC, DO, SGA}));
				QVERIFY(processor.takeOutboundData().isEmpty());
			}

			static void automaticProtectionOverridesManualNegotiateOnceMode()
			{
				TelnetProcessor processor;
				processor.setNegotiateOptionsOnce(true);
				processor.setAutomaticRenegotiationLoopProtection(true);

				processor.processBytes(bytes({IAC, WILL, SGA}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, SGA}));
				processor.processBytes(bytes({IAC, WILL, SGA}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, SGA}));

				processor.setAutomaticRenegotiationLoopProtection(false);
				processor.processBytes(bytes({IAC, WILL, SGA}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, SGA}));
				processor.processBytes(bytes({IAC, WILL, SGA}));
				QVERIFY(processor.takeOutboundData().isEmpty());
			}

			static void automaticProtectionSettlesThenSuppressesNegotiationReplies()
			{
				TelnetProcessor            processor;
				QList<bool>                noEchoStates;
				TelnetProcessor::Callbacks callbacks;
				callbacks.onNoEchoChanged = [&noEchoStates](const bool enabled)
				{ noEchoStates.append(enabled); };
				processor.setCallbacks(callbacks);
				processor.setAutomaticRenegotiationLoopProtection(true);

				for (int attempt = 0; attempt < 9; ++attempt)
				{
					const unsigned char command = attempt % 2 == 0 ? WILL : WONT;
					processor.processBytes(bytes({IAC, command, TELOPT_ECHO}));
					QCOMPARE(processor.takeOutboundData(),
					         bytes({IAC, command == WILL ? DO : DONT, TELOPT_ECHO}));
				}

				processor.processBytes(bytes({IAC, WONT, TELOPT_ECHO}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, TELOPT_ECHO}));
				QVERIFY(noEchoStates.constLast());

				processor.processBytes(bytes({IAC, DO, SGA}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WILL, SGA}));
				processor.processBytes(bytes({IAC, WONT, TELOPT_ECHO}));
				QVERIFY(processor.takeOutboundData().isEmpty());
				QCOMPARE(processor.processBytes(QByteArrayLiteral("ordinary text")),
				         QByteArrayLiteral("ordinary text"));

				processor.resetConnectionState();
				processor.processBytes(bytes({IAC, DO, SGA}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WILL, SGA}));
			}

			static void automaticProtectionUsesRollingWindowAndFiniteCooldown()
			{
				using Disposition = TelnetProcessorTestAccess::Disposition;

				TelnetProcessor processor;
				processor.setAutomaticRenegotiationLoopProtection(true);
				for (int attempt = 0; attempt < 9; ++attempt)
				{
					QCOMPARE(TelnetProcessorTestAccess::record(processor, true, TELOPT_ECHO, 0),
					         Disposition::Proceed);
				}
				QCOMPARE(TelnetProcessorTestAccess::record(processor, true, TELOPT_ECHO, 60'001),
				         Disposition::Proceed);

				processor.setAutomaticRenegotiationLoopProtection(false);
				processor.setAutomaticRenegotiationLoopProtection(true);
				for (int attempt = 0; attempt < 9; ++attempt)
				{
					QCOMPARE(TelnetProcessorTestAccess::record(processor, true, TELOPT_ECHO, attempt),
					         Disposition::Proceed);
				}
				QCOMPARE(TelnetProcessorTestAccess::record(processor, true, TELOPT_ECHO, 9),
				         Disposition::SettleAndSuppress);
				QCOMPARE(TelnetProcessorTestAccess::record(processor, true, TELOPT_ECHO, 10'008),
				         Disposition::Suppress);
				QCOMPARE(TelnetProcessorTestAccess::record(processor, true, TELOPT_ECHO, 10'009),
				         Disposition::Proceed);
			}

			static void automaticProtectionTracksEachOptionAndDirectionSeparately()
			{
				using Disposition = TelnetProcessorTestAccess::Disposition;

				TelnetProcessor processor;
				processor.setAutomaticRenegotiationLoopProtection(true);
				for (int attempt = 0; attempt < 9; ++attempt)
				{
					QCOMPARE(TelnetProcessorTestAccess::record(processor, true, TELOPT_ECHO, attempt),
					         Disposition::Proceed);
					QCOMPARE(TelnetProcessorTestAccess::record(processor, false, TELOPT_ECHO, attempt),
					         Disposition::Proceed);
					QCOMPARE(TelnetProcessorTestAccess::record(processor, true, SGA, attempt),
					         Disposition::Proceed);
				}
				QCOMPARE(TelnetProcessorTestAccess::record(processor, false, TELOPT_ECHO, 9),
				         Disposition::SettleAndSuppress);
				QCOMPARE(TelnetProcessorTestAccess::record(processor, true, TELOPT_ECHO, 10),
				         Disposition::SettleAndSuppress);
				QCOMPARE(TelnetProcessorTestAccess::record(processor, true, SGA, 11),
				         Disposition::SettleAndSuppress);
				QCOMPARE(TelnetProcessorTestAccess::record(processor, true, TELOPT_CHARSET, 12),
				         Disposition::Proceed);

				QCOMPARE(TelnetProcessorTestAccess::record(processor, false, TELOPT_ECHO, 10'008),
				         Disposition::Suppress);
				QCOMPARE(TelnetProcessorTestAccess::record(processor, false, TELOPT_ECHO, 10'009),
				         Disposition::Proceed);
				QCOMPARE(TelnetProcessorTestAccess::record(processor, true, TELOPT_ECHO, 10'009),
				         Disposition::Suppress);
				QCOMPARE(TelnetProcessorTestAccess::record(processor, true, TELOPT_ECHO, 10'010),
				         Disposition::Proceed);
				QCOMPARE(TelnetProcessorTestAccess::record(processor, true, SGA, 10'010),
				         Disposition::Suppress);
				QCOMPARE(TelnetProcessorTestAccess::record(processor, true, SGA, 10'011),
				         Disposition::Proceed);
			}

			static void queueEnableCompression2NegotiationSendsDoCompress2()
			{
				TelnetProcessor processor;
				processor.queueEnableCompression2Negotiation();
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, TELOPT_COMPRESS2}));
			}

			static void queueDisableCompressionNegotiationDefaultsToCompress2()
			{
				TelnetProcessor processor;
				processor.queueDisableCompressionNegotiation();
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DONT, TELOPT_COMPRESS2}));
			}

			static void echoNegotiationCallbacksAndReplies()
			{
				TelnetProcessor            processor;
				QList<bool>                noEchoStates;
				TelnetProcessor::Callbacks callbacks;
				callbacks.onNoEchoChanged = [&noEchoStates](const bool enabled)
				{ noEchoStates.append(enabled); };
				processor.setCallbacks(callbacks);

				processor.processBytes(bytes({IAC, WILL, TELOPT_ECHO}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, TELOPT_ECHO}));
				QCOMPARE(noEchoStates, QList<bool>{true});

				processor.processBytes(bytes({IAC, WONT, TELOPT_ECHO}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DONT, TELOPT_ECHO}));
				QCOMPARE(noEchoStates, QList<bool>({true, false}));
			}

			static void noEchoOffRejectsEchoNegotiation()
			{
				TelnetProcessor            processor;
				bool                       callbackFired = false;

				TelnetProcessor::Callbacks callbacks;
				callbacks.onNoEchoChanged = [&callbackFired](bool) { callbackFired = true; };
				processor.setCallbacks(callbacks);
				processor.setNoEchoOff(true);

				processor.processBytes(bytes({IAC, WILL, TELOPT_ECHO}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DONT, TELOPT_ECHO}));
				QVERIFY(!callbackFired);
			}

			static void noEchoStaysEnabledAcrossIncomingData()
			{
				TelnetProcessor            processor;
				QList<bool>                noEchoStates;
				TelnetProcessor::Callbacks callbacks;
				callbacks.onNoEchoChanged = [&noEchoStates](const bool enabled)
				{ noEchoStates.append(enabled); };
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

			static void resetConnectionStateClearsNoEchoOnce()
			{
				TelnetProcessor            processor;
				QList<bool>                noEchoStates;
				TelnetProcessor::Callbacks callbacks;
				callbacks.onNoEchoChanged = [&noEchoStates](const bool enabled)
				{ noEchoStates.append(enabled); };
				processor.setCallbacks(callbacks);

				processor.processBytes(bytes({IAC, WILL, TELOPT_ECHO}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, TELOPT_ECHO}));
				QCOMPARE(noEchoStates, QList<bool>{true});

				processor.resetConnectionState();
				QCOMPARE(noEchoStates, QList<bool>({true, false}));

				processor.resetConnectionState();
				QCOMPARE(noEchoStates, QList<bool>({true, false}));
			}

			static void doNawsSendsWillAndWindowSizeWhenEnabled()
			{
				TelnetProcessor processor;
				processor.setNawsEnabled(true);
				processor.setWindowSize(80, 24);
				QVERIFY(!processor.isNawsNegotiated());

				processor.processBytes(bytes({IAC, DO, TELOPT_NAWS}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WILL, TELOPT_NAWS, IAC, SB, TELOPT_NAWS,
				                                              0x00, 0x50, 0x00, 0x18, IAC, SE}));
				QVERIFY(processor.isNawsNegotiated());
			}

			static void doNawsEscapesIacBytesInWindowSizePayload()
			{
				TelnetProcessor processor;
				processor.setNawsEnabled(true);
				processor.setWindowSize(255, 255);

				processor.processBytes(bytes({IAC, DO, TELOPT_NAWS}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WILL, TELOPT_NAWS, IAC, SB, TELOPT_NAWS,
				                                              0x00, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, IAC, SE}));
			}

			static void doNawsSendsWontWhenDisabled()
			{
				TelnetProcessor processor;
				processor.setNawsEnabled(false);
				QVERIFY(!processor.isNawsNegotiated());

				processor.processBytes(bytes({IAC, DO, TELOPT_NAWS}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WONT, TELOPT_NAWS}));
				QVERIFY(!processor.isNawsNegotiated());
			}

			static void doNawsSendsUpdatedWindowSizeAfterNegotiation()
			{
				TelnetProcessor processor;
				processor.setNawsEnabled(true);
				processor.setWindowSize(80, 24);

				processor.processBytes(bytes({IAC, DO, TELOPT_NAWS}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WILL, TELOPT_NAWS, IAC, SB, TELOPT_NAWS,
				                                              0x00, 0x50, 0x00, 0x18, IAC, SE}));

				processor.setWindowSize(132, 40);
				QCOMPARE(processor.takeOutboundData(),
				         bytes({IAC, SB, TELOPT_NAWS, 0x00, 0x84, 0x00, 0x28, IAC, SE}));

				processor.setWindowSize(132, 40);
				QVERIFY(processor.takeOutboundData().isEmpty());
			}

			static void nawsNegotiationStateClearsOnDontWontAndReset()
			{
				TelnetProcessor processor;
				processor.setNawsEnabled(true);
				processor.setWindowSize(80, 24);
				QVERIFY(!processor.isNawsNegotiated());

				processor.processBytes(bytes({IAC, DO, TELOPT_NAWS}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WILL, TELOPT_NAWS, IAC, SB, TELOPT_NAWS,
				                                              0x00, 0x50, 0x00, 0x18, IAC, SE}));
				QVERIFY(processor.isNawsNegotiated());

				processor.processBytes(bytes({IAC, DONT, TELOPT_NAWS}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WONT, TELOPT_NAWS}));
				QVERIFY(!processor.isNawsNegotiated());

				processor.processBytes(bytes({IAC, DO, TELOPT_NAWS}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WILL, TELOPT_NAWS, IAC, SB, TELOPT_NAWS,
				                                              0x00, 0x50, 0x00, 0x18, IAC, SE}));
				QVERIFY(processor.isNawsNegotiated());

				processor.processBytes(bytes({IAC, WONT, TELOPT_NAWS}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DONT, TELOPT_NAWS}));
				QVERIFY(!processor.isNawsNegotiated());

				processor.processBytes(bytes({IAC, DO, TELOPT_NAWS}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WILL, TELOPT_NAWS, IAC, SB, TELOPT_NAWS,
				                                              0x00, 0x50, 0x00, 0x18, IAC, SE}));
				QVERIFY(processor.isNawsNegotiated());

				processor.resetConnectionState();
				QVERIFY(!processor.isNawsNegotiated());
			}

			static void disablingNawsAfterNegotiationSendsWontAndClearsNegotiatedState()
			{
				TelnetProcessor processor;
				processor.setNawsEnabled(true);
				processor.setWindowSize(120, 40);

				processor.processBytes(bytes({IAC, DO, TELOPT_NAWS}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WILL, TELOPT_NAWS, IAC, SB, TELOPT_NAWS,
				                                              0x00, 0x78, 0x00, 0x28, IAC, SE}));
				QVERIFY(processor.isNawsNegotiated());

				processor.setNawsEnabled(false);
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WONT, TELOPT_NAWS}));
				QVERIFY(!processor.isNawsNegotiated());

				processor.setWindowSize(132, 45);
				QVERIFY(processor.takeOutboundData().isEmpty());
			}

			static void terminalTypeRequestReturnsConfiguredName()
			{
				TelnetProcessor processor;
				processor.setTerminalIdentification(QStringLiteral("QMudTerm"));

				processor.processBytes(bytes({IAC, DO, TELOPT_TERMINAL_TYPE}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, WILL, TELOPT_TERMINAL_TYPE}));

				processor.processBytes(bytes({IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_SEND, IAC, SE}));
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_IS, 'Q',
				                                              'M', 'u', 'd', 'T', 'e', 'r', 'm', IAC, SE}));
			}

			static void charsetRequestAcceptedAndRejected()
			{
				TelnetProcessor processor;
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

				processor.processBytes(bytes({IAC, SB, TELOPT_CHARSET, CHARSET_REQUEST, ',', 'U', 'S', '-',
				                              'A', 'S', 'C', 'I', 'I', IAC, SE}));
				QCOMPARE(processor.takeOutboundData(),
				         bytes({IAC, SB, TELOPT_CHARSET, CHARSET_REJECTED, IAC, SE}));
			}

			static void startTlsNegotiationQueuesDoAndRequestsUpgradeOnWill()
			{
				TelnetProcessor processor;
				processor.setStartTlsEnabled(true);
				processor.queueStartTlsNegotiation();
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, TELOPT_START_TLS}));

				processor.processBytes(bytes({IAC, WILL, TELOPT_START_TLS}));
				QCOMPARE(processor.takeOutboundData(),
				         bytes({IAC, SB, TELOPT_START_TLS, START_TLS_FOLLOWS, IAC, SE}));
				QVERIFY(processor.takeStartTlsUpgradeRequest());
				QVERIFY(!processor.takeStartTlsUpgradeRequest());
			}

			static void startTlsFollowsSubnegotiationRequestsUpgrade()
			{
				TelnetProcessor processor;
				processor.setStartTlsEnabled(true);
				processor.processBytes(bytes({IAC, SB, TELOPT_START_TLS, START_TLS_FOLLOWS, IAC, SE}));
				QVERIFY(processor.takeStartTlsUpgradeRequest());
				QVERIFY(!processor.takeStartTlsUpgradeRequest());

				processor.setStartTlsActive(true);
				processor.processBytes(bytes({IAC, SB, TELOPT_START_TLS, START_TLS_FOLLOWS, IAC, SE}));
				QVERIFY(!processor.takeStartTlsUpgradeRequest());
			}

			static void startTlsRejectionIsReported()
			{
				TelnetProcessor processor;
				processor.setStartTlsEnabled(true);
				processor.queueStartTlsNegotiation();
				QCOMPARE(processor.takeOutboundData(), bytes({IAC, DO, TELOPT_START_TLS}));

				processor.processBytes(bytes({IAC, WONT, TELOPT_START_TLS}));
				QVERIFY(processor.takeStartTlsNegotiationRejected());
				QVERIFY(!processor.takeStartTlsNegotiationRejected());
				QVERIFY(!processor.takeStartTlsUpgradeRequest());
			}

			static void gaCanConvertToNewline()
			{
				TelnetProcessor            processor;
				int                        gaCount = 0;

				TelnetProcessor::Callbacks callbacks;
				callbacks.onIacGa = [&gaCount]() { ++gaCount; };
				processor.setCallbacks(callbacks);
				processor.setConvertGAtoNewline(true);

				const QByteArray output = processor.processBytes(bytes({IAC, GA}));
				QCOMPARE(output, QByteArray("\n"));
				QCOMPARE(gaCount, 1);
			}

		public:
			/** @brief Keeps every Qt test entry point source-visible to static analysis. */
			tst_TelnetProcessor_Options()
			{
				constexpr std::array testFunctions = {
				    &queueInitialNegotiationIsIdempotent,
				    &repeatedSgaNegotiationRemainsEnabledByDefault,
				    &repeatedSgaNegotiationCanBeSuppressedWhenConfigured,
				    &automaticProtectionOverridesManualNegotiateOnceMode,
				    &automaticProtectionSettlesThenSuppressesNegotiationReplies,
				    &automaticProtectionUsesRollingWindowAndFiniteCooldown,
				    &automaticProtectionTracksEachOptionAndDirectionSeparately,
				    &queueEnableCompression2NegotiationSendsDoCompress2,
				    &queueDisableCompressionNegotiationDefaultsToCompress2,
				    &echoNegotiationCallbacksAndReplies,
				    &noEchoOffRejectsEchoNegotiation,
				    &noEchoStaysEnabledAcrossIncomingData,
				    &resetConnectionStateClearsNoEchoOnce,
				    &doNawsSendsWillAndWindowSizeWhenEnabled,
				    &doNawsEscapesIacBytesInWindowSizePayload,
				    &doNawsSendsWontWhenDisabled,
				    &doNawsSendsUpdatedWindowSizeAfterNegotiation,
				    &nawsNegotiationStateClearsOnDontWontAndReset,
				    &disablingNawsAfterNegotiationSendsWontAndClearsNegotiatedState,
				    &terminalTypeRequestReturnsConfiguredName,
				    &charsetRequestAcceptedAndRejected,
				    &startTlsNegotiationQueuesDoAndRequestsUpgradeOnWill,
				    &startTlsFollowsSubnegotiationRequestsUpgrade,
				    &startTlsRejectionIsReported,
				    &gaCanConvertToNewline,
				};
				static_assert(testFunctions.size() == 25);
			}
	};

} // namespace

QTEST_APPLESS_MAIN(tst_TelnetProcessor_Options)

#if __has_include("tst_TelnetProcessor_Options.moc")
#include "tst_TelnetProcessor_Options.moc"
#endif
