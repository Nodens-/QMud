/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldRuntime_OutputBufferSeal.cpp
 * Role: Unit coverage for WorldRuntime session-state output-buffer sealing.
 */

#include "NativePluginRegistry.h"
#include "WorldRuntime.h"

#include <QDateTime>
#include <QScopeGuard>
#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Builds a deterministic output line for replacement assertions.
	 * @param text Line text.
	 * @param lineNumber Absolute runtime line number.
	 * @return Runtime line entry.
	 */
	WorldRuntime::LineEntry makeLine(const QString &text, const qint64 lineNumber)
	{
		WorldRuntime::LineEntry line;
		line.text       = text;
		line.flags      = WorldRuntime::LineOutput;
		line.hardReturn = true;
		line.time       = QDateTime::fromMSecsSinceEpoch(1710000000000 + lineNumber);
		line.lineNumber = lineNumber;
		line.ticks      = static_cast<double>(lineNumber);
		line.elapsed    = static_cast<double>(lineNumber);
		return line;
	}
} // namespace

/**
 * @brief QTest fixture covering the runtime output-buffer save seal.
 */
class tst_WorldRuntime_OutputBufferSeal : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void sealBlocksAppendReplaceBookmarkAndDelete()
		{
			WorldRuntime::StyleSpan span;
			span.length = 14;
			span.fore   = QColor(QStringLiteral("#112233"));

			WorldRuntime runtime;
			runtime.addLine(QStringLiteral("base"), WorldRuntime::LineOutput);
			QCOMPARE(runtime.lines().size(), 1);
			QCOMPARE(runtime.lines().at(0).text, QStringLiteral("base"));

			runtime.setSessionStateOutputBufferSealed(true);
			QVERIFY(runtime.isSessionStateOutputBufferSealed());
			runtime.addLine(QStringLiteral("blocked-append"), WorldRuntime::LineOutput);
			runtime.addLine(QStringLiteral("blocked-styled"), WorldRuntime::LineOutput,
			                QVector<WorldRuntime::StyleSpan>{span});
			runtime.replaceOutputLines(
			    QVector<WorldRuntime::LineEntry>{makeLine(QStringLiteral("blocked-replace"), 50)});
			runtime.bookmarkLine(1, true);
			runtime.deleteLines(1);
			runtime.deleteOutput();

			QCOMPARE(runtime.lines().size(), 1);
			QCOMPARE(runtime.lines().at(0).text, QStringLiteral("base"));
			QVERIFY((runtime.lines().at(0).flags & WorldRuntime::LineBookmark) == 0);

			runtime.setSessionStateOutputBufferSealed(false);
			QVERIFY(!runtime.isSessionStateOutputBufferSealed());
			runtime.addLine(QStringLiteral("after-unseal"), WorldRuntime::LineOutput);
			QCOMPARE(runtime.lines().size(), 2);
			QCOMPARE(runtime.lines().at(1).text, QStringLiteral("after-unseal"));
			runtime.addLine(QStringLiteral("styled-after-unseal"), WorldRuntime::LineOutput,
			                QVector<WorldRuntime::StyleSpan>{span});
			QCOMPARE(runtime.lines().size(), 3);
			QCOMPARE(runtime.lines().at(2).text, QStringLiteral("styled-after-unseal"));
			QCOMPARE(runtime.lines().at(2).spans.size(), 1);

			runtime.replaceOutputLines(
			    QVector<WorldRuntime::LineEntry>{makeLine(QStringLiteral("replacement"), 80)});
			QCOMPARE(runtime.lines().size(), 1);
			QCOMPARE(runtime.lines().at(0).text, QStringLiteral("replacement"));

			runtime.bookmarkLine(1, true);
			QVERIFY((runtime.lines().at(0).flags & WorldRuntime::LineBookmark) != 0);

			runtime.deleteLines(1);
			QCOMPARE(runtime.lines().size(), 0);

			runtime.addLine(QStringLiteral("clearable"), WorldRuntime::LineOutput);
			QCOMPARE(runtime.lines().size(), 1);
			runtime.deleteOutput();
			QCOMPARE(runtime.lines().size(), 0);
		}

		void sealBlocksHardReturnMutations()
		{
			WorldRuntime runtime;
			runtime.addLine(QStringLiteral("input"), WorldRuntime::LineInput, false);
			QCOMPARE(runtime.lines().size(), 1);
			QVERIFY(!runtime.lines().at(0).hardReturn);

			runtime.setSessionStateOutputBufferSealed(true);
			runtime.finalizePendingInputLineHardReturn();
			QVERIFY(!runtime.lines().at(0).hardReturn);

			runtime.setSessionStateOutputBufferSealed(false);
			runtime.finalizePendingInputLineHardReturn();
			QVERIFY(runtime.lines().at(0).hardReturn);

			runtime.setSessionStateOutputBufferSealed(true);
			runtime.clearLastLineHardReturn();
			QVERIFY(runtime.lines().at(0).hardReturn);

			runtime.setSessionStateOutputBufferSealed(false);
			runtime.clearLastLineHardReturn();
			QVERIFY(!runtime.lines().at(0).hardReturn);
		}

		void sealBlocksPendingIncomingPartialCommitWithoutClearing()
		{
			WorldRuntime runtime;
			runtime.receiveRawData(QByteArrayLiteral("prompt> "));
			QCOMPARE(runtime.lines().size(), 0);

			runtime.setSessionStateOutputBufferSealed(true);
			QVERIFY(!runtime.commitPendingIncomingPartialLine());
			QCOMPARE(runtime.lines().size(), 0);

			runtime.setSessionStateOutputBufferSealed(false);
			QVERIFY(runtime.commitPendingIncomingPartialLine());
			QCOMPARE(runtime.lines().size(), 1);
			QCOMPARE(runtime.lines().at(0).text, QStringLiteral("prompt> "));
			QVERIFY(runtime.lines().at(0).hardReturn);
		}

		void sealBlocksLuaContextLineMutations()
		{
			WorldRuntime runtime;
			runtime.beginIncomingLineLuaContext(QStringLiteral("incoming"), WorldRuntime::LineOutput, {});

			runtime.setSessionStateOutputBufferSealed(true);
			QVERIFY(!runtime.reserveIncomingLineLuaContextInBuffer());
			QCOMPARE(runtime.lines().size(), 0);

			runtime.setSessionStateOutputBufferSealed(false);
			QVERIFY(runtime.reserveIncomingLineLuaContextInBuffer());
			QCOMPARE(runtime.lines().size(), 1);
			QCOMPARE(runtime.lines().at(0).text, QStringLiteral("incoming"));

			runtime.setSessionStateOutputBufferSealed(true);
			QVERIFY(!runtime.updateBufferedIncomingLineLuaContext(QStringLiteral("blocked-update"),
			                                                      WorldRuntime::LineOutput, {}));
			QVERIFY(!runtime.hideBufferedIncomingLineLuaContextForReplacement());
			QVERIFY(!runtime.removeBufferedIncomingLineLuaContext());
			QCOMPARE(runtime.lines().size(), 1);
			QCOMPARE(runtime.lines().at(0).text, QStringLiteral("incoming"));
			QVERIFY((runtime.lines().at(0).flags & WorldRuntime::LineHidden) == 0);

			runtime.setSessionStateOutputBufferSealed(false);
			QVERIFY(runtime.updateBufferedIncomingLineLuaContext(QStringLiteral("updated"),
			                                                     WorldRuntime::LineOutput, {}));
			QCOMPARE(runtime.lines().at(0).text, QStringLiteral("updated"));
			QVERIFY(runtime.hideBufferedIncomingLineLuaContextForReplacement());
			QVERIFY((runtime.lines().at(0).flags & WorldRuntime::LineHidden) != 0);

			const qint64 hiddenLineNumber = runtime.lines().at(0).lineNumber;
			runtime.setSessionStateOutputBufferSealed(true);
			QVERIFY(!runtime.removeHiddenLuaContextLineByAbsoluteNumber(hiddenLineNumber));
			QCOMPARE(runtime.lines().size(), 1);

			runtime.setSessionStateOutputBufferSealed(false);
			QVERIFY(runtime.removeHiddenLuaContextLineByAbsoluteNumber(hiddenLineNumber));
			QCOMPARE(runtime.lines().size(), 0);
		}

		void sealBlocksLuaCallbackAnchorWrites()
		{
			WorldRuntime runtime;
			runtime.addLine(QStringLiteral("anchor"), WorldRuntime::LineOutput);
			QCOMPARE(runtime.lines().size(), 1);
			const qint64 anchorLineNumber = runtime.lines().at(0).lineNumber;

			runtime.setSessionStateOutputBufferSealed(true);
			QVERIFY(!runtime.writeLuaCallbackOutputAtLineAnchor(anchorLineNumber, 1, false,
			                                                    QStringLiteral("blocked-anchor"),
			                                                    WorldRuntime::LineOutput, {}, true));
			QCOMPARE(runtime.lines().size(), 1);
			QCOMPARE(runtime.lines().at(0).text, QStringLiteral("anchor"));

			runtime.setSessionStateOutputBufferSealed(false);
			QVERIFY(runtime.writeLuaCallbackOutputAtLineAnchor(
			    anchorLineNumber, 1, false, QStringLiteral("inserted"), WorldRuntime::LineOutput, {}, true));
			QCOMPARE(runtime.lines().size(), 2);
			QCOMPARE(runtime.lines().at(1).text, QStringLiteral("inserted"));
		}

		void luaContextOmittedLineCleanupClearsMushReaderPartialSuppression()
		{
			QVector<QMudNativePluginRegistry::TestSpeechEvent> events;
			QMudNativePluginRegistry::setTestSpeechSink(
			    [&events](const QMudNativePluginRegistry::TestSpeechEvent &event)
			    { events.push_back(event); });
			const auto restoreSpeechSink =
			    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });

			auto verifyCleanupAllowsScreenDraw = [&events](const QString &text, const bool hideForReplacement)
			{
				WorldRuntime runtime;
				QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);

				events.clear();
				QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, text);
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, text);

				runtime.beginIncomingLineLuaContext(text, WorldRuntime::LineOutput, {});
				QVERIFY(runtime.reserveIncomingLineLuaContextInBuffer());
				QCOMPARE(runtime.lines().size(), 1);
				QCOMPARE(runtime.lines().at(0).text, text);

				if (hideForReplacement)
				{
					QVERIFY(runtime.hideBufferedIncomingLineLuaContextForReplacement());
					QVERIFY((runtime.lines().at(0).flags & WorldRuntime::LineHidden) != 0);
				}
				else
				{
					QVERIFY(runtime.removeBufferedIncomingLineLuaContext());
					QCOMPARE(runtime.lines().size(), 0);
				}

				events.clear();
				QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, text);
				QCOMPARE(events.size(), 1);
				QCOMPARE(events.constLast().text, text);
			};

			verifyCleanupAllowsScreenDraw(QStringLiteral("<omitted> "), false);
			verifyCleanupAllowsScreenDraw(QStringLiteral("<replacement> "), true);
		}

		void committedPendingPartialClearsMushReaderPartialSuppression()
		{
			WorldRuntime                                       runtime;
			QVector<QMudNativePluginRegistry::TestSpeechEvent> events;
			QMudNativePluginRegistry::setTestSpeechSink(
			    [&events](const QMudNativePluginRegistry::TestSpeechEvent &event)
			    { events.push_back(event); });
			const auto restoreSpeechSink =
			    qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });
			QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);

			const QString prompt = QStringLiteral("<committed> ");
			QMudNativePluginRegistry::handleMushReaderPartialLine(&runtime, prompt);
			QCOMPARE(events.size(), 1);
			QCOMPARE(events.constLast().text, prompt);

			runtime.receiveRawData(prompt.toUtf8());
			QVERIFY(runtime.commitPendingIncomingPartialLine());
			QCOMPARE(runtime.lines().size(), 1);
			QCOMPARE(runtime.lines().constLast().text, prompt);

			events.clear();
			QMudNativePluginRegistry::handleMushReaderScreenDraw(&runtime, 1, 0, prompt);
			QCOMPARE(events.size(), 1);
			QCOMPARE(events.constLast().text, prompt);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_WorldRuntime_OutputBufferSeal)

#if __has_include("tst_WorldRuntime_OutputBufferSeal.moc")
#include "tst_WorldRuntime_OutputBufferSeal.moc"
#endif
