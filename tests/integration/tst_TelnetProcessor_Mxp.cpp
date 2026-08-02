/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_TelnetProcessor_Mxp.cpp
 * Role: QTest coverage for TelnetProcessor MXP behavior.
 */

#include "MxpDiagnostics.h"
#include "TelnetCallbackSpy.h"
#include "TelnetProcessor.h"
#include "WorldOptions.h"

#include <QtTest/QTest>

#include <algorithm>

namespace
{
	constexpr unsigned char ESC        = 0x1B;
	constexpr unsigned char IAC        = 0xFF;
	constexpr unsigned char GA         = 0xF9;
	constexpr unsigned char EOR        = 0xEF;
	constexpr unsigned char SB         = 0xFA;
	constexpr unsigned char SE         = 0xF0;
	constexpr unsigned char TELOPT_MXP = 91;

	using MxpMode = TelnetProcessor::MxpMode;

	QByteArray bytes(std::initializer_list<unsigned char> raw)
	{
		QByteArray out;
		for (const unsigned char c : raw)
			out.append(static_cast<char>(c));
		return out;
	}

	bool hasMxpCollectionTooLongErrorDiagnostic(const TelnetCallbackSpy &spy)
	{
		return std::ranges::any_of(spy.mxpDiagnostics,
		                           [](const TelnetCallbackSpy::MxpDiagnostic &diagnostic)
		                           {
			                           return diagnostic.level == DBG_ERROR &&
			                                  diagnostic.messageNumber == errMXP_CollectionTooLong;
		                           });
	}

	void expectOnlyMxpErrorDiagnostic(const TelnetCallbackSpy &spy, const long messageNumber)
	{
		QCOMPARE(spy.mxpDiagnostics.size(), 1);
		QCOMPARE(spy.mxpDiagnostics.constFirst().level, DBG_ERROR);
		QCOMPARE(spy.mxpDiagnostics.constFirst().messageNumber, messageNumber);
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
			processor.setUseMxp(eUseMXP);

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
			processor.setUseMxp(eUseMXP);
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
			processor.setUseMxp(eOnCommandMXP);

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

			processor.setUseMxp(eUseMXP);
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
			processor.setUseMxp(eUseMXP);

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
			processor.setUseMxp(eUseMXP);

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
			processor.setUseMxp(eOnCommandMXP);
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
			processor.setUseMxp(eOnCommandMXP);
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
			processor.setUseMxp(eOnCommandMXP);
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
			QCOMPARE(modeChanges.at(0).newMode, TelnetProcessor::mxpModeCode(MxpMode::Secure));
			QCOMPARE(modeChanges.at(0).offset, 0);
			QCOMPARE(modeChanges.at(1).newMode, TelnetProcessor::mxpModeCode(MxpMode::PermanentLocked));
			QCOMPARE(modeChanges.at(1).offset, static_cast<int>(QByteArrayLiteral("Testuser").size()));
			QVERIFY(modeChanges.at(0).sequence < modeChanges.at(1).sequence);
		}

		void resetConnectionStateClearsStaleMxpLockedMode()
		{
			TelnetProcessor processor;
			processor.setUseMxp(eUseMXP);

			// Move to permanently locked mode (server-side state before reconnect).
			QByteArray lockMode;
			lockMode.append(static_cast<char>(ESC));
			lockMode.append('[');
			lockMode.append('7');
			lockMode.append('z');
			QCOMPARE(processor.processBytes(lockMode), QByteArray());

			// In locked mode, tags are treated as literal text.
			QCOMPARE(processor.processBytes(QByteArrayLiteral("<send href='look'>go</send>")),
			         QByteArrayLiteral("<send href='look'>go</send>"));

			processor.resetConnectionState();
			// Runtime reapplies world setting every packet; keep same mode and verify
			// "same value" path can re-enable MXP after reset.
			processor.setUseMxp(eUseMXP);

			// After reset + reapply, MXP should parse again, not leak raw tags.
			QCOMPARE(processor.processBytes(QByteArrayLiteral("<send href='look'>go</send>")),
			         QByteArrayLiteral("go"));
			const QList<TelnetProcessor::MxpEvent> events = processor.takeMxpEvents();
			QCOMPARE(events.size(), 2);
			QCOMPARE(events.at(0).type, TelnetProcessor::MxpEvent::StartTag);
			QCOMPARE(events.at(1).type, TelnetProcessor::MxpEvent::EndTag);
		}

		void customElementDefinitionParsesAndIsQueryable()
		{
			TelnetProcessor processor;
			processor.setUseMxp(eUseMXP);

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

		void mxpSessionStateRestoresDetailedModes()
		{
			TelnetProcessor processor;
			processor.setUseMxp(eUseMXP);

			TelnetProcessor::MxpSessionState saved;
			saved.enabled      = true;
			saved.puebloActive = false;
			saved.secureMode   = true;
			saved.mode         = TelnetProcessor::mxpModeCode(MxpMode::PermanentSecure);
			saved.defaultMode  = TelnetProcessor::mxpModeCode(MxpMode::PermanentSecure);
			saved.previousMode = TelnetProcessor::mxpModeCode(MxpMode::PermanentOpen);

			processor.setMxpSessionState(saved);
			const TelnetProcessor::MxpSessionState restored = processor.mxpSessionState();
			QVERIFY(restored.enabled);
			QVERIFY(restored.secureMode);
			QCOMPARE(restored.mode, saved.mode);
			QCOMPARE(restored.defaultMode, saved.defaultMode);
			QCOMPARE(restored.previousMode, saved.previousMode);
		}

		void unterminatedMxpElementDoesNotSwallowNewline()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(eUseMXP);

			const QByteArray output =
			    processor.processBytes(QByteArrayLiteral("before <unterminated\nprompt"));

			QCOMPARE(output, QByteArrayLiteral("before \nprompt"));
			QVERIFY(processor.takeMxpEvents().isEmpty());
			expectOnlyMxpErrorDiagnostic(spy, errMXP_UnterminatedElement);
		}

		void unterminatedMxpEntityDoesNotSwallowNewline()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(eUseMXP);

			const QByteArray output =
			    processor.processBytes(QByteArrayLiteral("before &unterminated\nprompt"));

			QCOMPARE(output, QByteArrayLiteral("before \nprompt"));
			QVERIFY(processor.takeMxpEvents().isEmpty());
			expectOnlyMxpErrorDiagnostic(spy, errMXP_UnterminatedEntity);
		}

		void unterminatedMxpQuoteDoesNotSwallowIacGa()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(eUseMXP);
			processor.setConvertGAtoNewline(true);

			QByteArray input = QByteArrayLiteral("prompt <send href='unterminated");
			input.append(bytes({IAC, GA}));

			const QByteArray output = processor.processBytes(input);

			QCOMPARE(output, QByteArrayLiteral("prompt \n"));
			QCOMPARE(spy.iacGaCount, 1);
			const QList<TelnetProcessor::TelnetPluginEvent> events = processor.takeTelnetPluginEvents();
			QCOMPARE(events.size(), 1);
			QCOMPARE(events.constFirst().type, TelnetProcessor::TelnetPluginEvent::IacGa);
			QCOMPARE(events.constFirst().option, static_cast<int>(GA));
			QCOMPARE(events.constFirst().offset, static_cast<int>(QByteArrayLiteral("prompt ").size()));
			expectOnlyMxpErrorDiagnostic(spy, errMXP_UnterminatedQuote);
		}

		void unterminatedMxpElementDoesNotSwallowIacEor()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(eUseMXP);
			processor.setConvertGAtoNewline(true);

			QByteArray input = QByteArrayLiteral("prompt <unterminated");
			input.append(bytes({IAC, EOR}));

			const QByteArray output = processor.processBytes(input);

			QCOMPARE(output, QByteArrayLiteral("prompt \n"));
			QCOMPARE(spy.iacGaCount, 1);
			const QList<TelnetProcessor::TelnetPluginEvent> events = processor.takeTelnetPluginEvents();
			QCOMPARE(events.size(), 1);
			QCOMPARE(events.constFirst().type, TelnetProcessor::TelnetPluginEvent::IacGa);
			QCOMPARE(events.constFirst().option, static_cast<int>(EOR));
			QCOMPARE(events.constFirst().offset, static_cast<int>(QByteArrayLiteral("prompt ").size()));
			expectOnlyMxpErrorDiagnostic(spy, errMXP_UnterminatedElement);
		}

		void unterminatedMxpElementDoesNotSwallowAnsiEscape()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(eUseMXP);

			QByteArray input = QByteArrayLiteral("before <unterminated");
			input.append(static_cast<char>(ESC));
			input.append(QByteArrayLiteral("[31mred"));

			const QByteArray output = processor.processBytes(input);

			QCOMPARE(output, QByteArrayLiteral("before \x1b[31mred"));
			QVERIFY(processor.takeMxpEvents().isEmpty());
			expectOnlyMxpErrorDiagnostic(spy, errMXP_UnterminatedElement);
		}

		void splitPacketUnterminatedMxpElementDoesNotSwallowNewline()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(eUseMXP);

			QCOMPARE(processor.processBytes(QByteArrayLiteral("before <unterminated")),
			         QByteArrayLiteral("before "));
			QCOMPARE(processor.processBytes(QByteArrayLiteral("\nprompt")), QByteArrayLiteral("\nprompt"));

			QVERIFY(processor.takeMxpEvents().isEmpty());
			expectOnlyMxpErrorDiagnostic(spy, errMXP_UnterminatedElement);
		}

		void splitPacketUnterminatedMxpElementDoesNotSwallowIacGa()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(eUseMXP);
			processor.setConvertGAtoNewline(true);

			QCOMPARE(processor.processBytes(QByteArrayLiteral("prompt <unterminated")),
			         QByteArrayLiteral("prompt "));
			QCOMPARE(processor.processBytes(bytes({IAC})), QByteArray());
			QCOMPARE(processor.processBytes(bytes({GA})), QByteArrayLiteral("\n"));

			QCOMPARE(spy.iacGaCount, 1);
			const QList<TelnetProcessor::TelnetPluginEvent> events = processor.takeTelnetPluginEvents();
			QCOMPARE(events.size(), 1);
			QCOMPARE(events.constFirst().type, TelnetProcessor::TelnetPluginEvent::IacGa);
			QCOMPARE(events.constFirst().option, static_cast<int>(GA));
			QCOMPARE(events.constFirst().offset, 0);
			expectOnlyMxpErrorDiagnostic(spy, errMXP_UnterminatedElement);
		}

		void unterminatedMxpElementRestoresSecureOnceBeforeIacGa()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(eUseMXP);
			processor.setConvertGAtoNewline(true);

			QByteArray input;
			input.append(static_cast<char>(ESC));
			input.append(QByteArrayLiteral("[4z<unterminated"));
			input.append(bytes({IAC, GA}));
			input.append(QByteArrayLiteral("<send href='look'>go</send>"));

			const QByteArray output = processor.processBytes(input);

			QCOMPARE(output, QByteArrayLiteral("\ngo"));
			QCOMPARE(spy.iacGaCount, 1);
			expectOnlyMxpErrorDiagnostic(spy, errMXP_UnterminatedElement);
			const QList<TelnetProcessor::MxpEvent> events = processor.takeMxpEvents();
			QCOMPARE(events.size(), 2);
			QCOMPARE(events.at(0).type, TelnetProcessor::MxpEvent::StartTag);
			QCOMPARE(events.at(1).type, TelnetProcessor::MxpEvent::EndTag);
			QVERIFY(!events.at(0).secure);
			QVERIFY(!events.at(1).secure);
		}

		void secureOnceRestoredBeforeMxpDiagnosticCallbacks()
		{
			TelnetProcessor            processor;
			TelnetCallbackSpy          spy;
			TelnetProcessor::Callbacks callbacks              = spy.callbacks();
			int                        observedNeededMode     = -1;
			int                        observedDiagnosticMode = -1;
			callbacks.onMxpDiagnosticNeeded                   = [&](int)
			{
				observedNeededMode = processor.mxpSessionState().mode;
				return true;
			};
			callbacks.onMxpDiagnostic = [&](const int level, const long messageNumber, const QString &message)
			{
				observedDiagnosticMode = processor.mxpSessionState().mode;
				spy.mxpDiagnostics.append({level, messageNumber, message});
			};
			processor.setCallbacks(callbacks);
			processor.setUseMxp(eUseMXP);

			QByteArray lockMode;
			lockMode.append(static_cast<char>(ESC));
			lockMode.append(QByteArrayLiteral("[7z"));
			QCOMPARE(processor.processBytes(lockMode), QByteArray());
			const int  lockedMode = processor.mxpSessionState().mode;

			QByteArray input;
			input.append(static_cast<char>(ESC));
			input.append(QByteArrayLiteral("[4z<unterminated\n"));

			QCOMPARE(processor.processBytes(input), QByteArrayLiteral("\n"));
			expectOnlyMxpErrorDiagnostic(spy, errMXP_UnterminatedElement);
			QCOMPARE(observedNeededMode, lockedMode);
			QCOMPARE(observedDiagnosticMode, lockedMode);
			QCOMPARE(processor.mxpSessionState().mode, lockedMode);
		}

		void overlongMxpElementRestoresSecureOnceBeforeNextTag()
		{
			TelnetProcessor   probeProcessor;
			TelnetCallbackSpy probeSpy;
			probeProcessor.setCallbacks(probeSpy.callbacks());
			probeProcessor.setUseMxp(eUseMXP);
			QByteArray probePrefix;
			probePrefix.append(static_cast<char>(ESC));
			probePrefix.append(QByteArrayLiteral("[4z<"));
			QCOMPARE(probeProcessor.processBytes(probePrefix), QByteArray());

			int overflowBytes = 0;
			for (; overflowBytes < 20000 && !hasMxpCollectionTooLongErrorDiagnostic(probeSpy);
			     ++overflowBytes)
				probeProcessor.processBytes(QByteArrayLiteral("x"));

			expectOnlyMxpErrorDiagnostic(probeSpy, errMXP_CollectionTooLong);

			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(eUseMXP);

			QByteArray input;
			input.append(static_cast<char>(ESC));
			input.append(QByteArrayLiteral("[4z<"));
			input.append(QByteArray(overflowBytes, 'x'));
			input.append(QByteArrayLiteral("<send href='look'>go</send>"));
			const QByteArray output = processor.processBytes(input);

			QCOMPARE(output, QByteArrayLiteral("go"));
			expectOnlyMxpErrorDiagnostic(spy, errMXP_CollectionTooLong);
			const QList<TelnetProcessor::MxpEvent> events = processor.takeMxpEvents();
			QCOMPARE(events.size(), 2);
			QCOMPARE(events.at(0).type, TelnetProcessor::MxpEvent::StartTag);
			QCOMPARE(events.at(1).type, TelnetProcessor::MxpEvent::EndTag);
			QVERIFY(!events.at(0).secure);
			QVERIFY(!events.at(1).secure);
		}

		void nestedMxpElementRestoresSecureOnceLockedModeBeforeStarter()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());
			processor.setUseMxp(eUseMXP);

			QByteArray input;
			input.append(static_cast<char>(ESC));
			input.append(QByteArrayLiteral("[7z"));
			input.append(static_cast<char>(ESC));
			input.append(QByteArrayLiteral("[4z<unterminated<send href='look'>go</send>"));

			const QByteArray output = processor.processBytes(input);

			QCOMPARE(output, QByteArrayLiteral("go</send>"));
			expectOnlyMxpErrorDiagnostic(spy, errMXP_UnterminatedElement);
			const QList<TelnetProcessor::MxpEvent> events = processor.takeMxpEvents();
			QCOMPARE(events.size(), 1);
			QCOMPARE(events.at(0).type, TelnetProcessor::MxpEvent::StartTag);
			QVERIFY(!events.at(0).secure);
		}

		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_TelnetProcessor_Mxp)

#if __has_include("tst_TelnetProcessor_Mxp.moc")
#include "tst_TelnetProcessor_Mxp.moc"
#endif
