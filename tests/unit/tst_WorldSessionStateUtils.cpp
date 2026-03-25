/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldSessionStateUtils.cpp
 * Role: QTest coverage for world session-state binary persistence helpers.
 */

#include "WorldSessionStateUtils.h"

#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QFileInfo>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Builds one styled sample line used by round-trip assertions.
	 * @return Line entry with style metadata.
	 */
	WorldRuntime::LineEntry makeSampleLine()
	{
		WorldRuntime::StyleSpan span;
		span.length     = 5;
		span.fore       = QColor(QStringLiteral("#112233"));
		span.back       = QColor(QStringLiteral("#445566"));
		span.bold       = true;
		span.underline  = true;
		span.italic     = true;
		span.blink      = true;
		span.strike     = true;
		span.inverse    = true;
		span.changed    = true;
		span.actionType = WorldRuntime::ActionHyperlink;
		span.action     = QStringLiteral("look");
		span.hint       = QStringLiteral("hint");
		span.variable   = QStringLiteral("var");
		span.startTag   = true;

		WorldRuntime::LineEntry line;
		line.text       = QStringLiteral("Hello world");
		line.flags      = WorldRuntime::LineOutput | WorldRuntime::LineBookmark;
		line.hardReturn = false;
		line.spans      = {span};
		line.time       = QDateTime::fromMSecsSinceEpoch(1710000000000);
		line.lineNumber = 42;
		line.ticks      = 12.5;
		line.elapsed    = 12.5;
		return line;
	}
} // namespace

/**
 * @brief QTest fixture covering world session-state persistence behavior.
 */
class tst_WorldSessionStateUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void roundTripOutputAndHistory()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString statePath = tempDir.filePath(QStringLiteral("world.qws"));

			QMudWorldSessionState::WorldSessionStateData writeData;
			writeData.hasOutputBuffer   = true;
			writeData.hasCommandHistory = true;
			writeData.outputLines.push_back(makeSampleLine());
			writeData.commandHistory = {QStringLiteral("north"), QStringLiteral("look")};

			QString error;
			QVERIFY(QMudWorldSessionState::writeSessionStateFile(statePath, writeData, &error));
			QVERIFY2(error.isEmpty(), qPrintable(error));

			QMudWorldSessionState::WorldSessionStateData readData;
			QVERIFY(QMudWorldSessionState::readSessionStateFile(statePath, &readData, &error));
			QVERIFY2(error.isEmpty(), qPrintable(error));
			QVERIFY(readData.hasOutputBuffer);
			QVERIFY(readData.hasCommandHistory);
			QCOMPARE(readData.outputLines.size(), 1);
			QCOMPARE(readData.commandHistory, writeData.commandHistory);

			const WorldRuntime::LineEntry &line = readData.outputLines.at(0);
			QCOMPARE(line.text, writeData.outputLines.at(0).text);
			QCOMPARE(line.flags, writeData.outputLines.at(0).flags);
			QCOMPARE(line.hardReturn, writeData.outputLines.at(0).hardReturn);
			QCOMPARE(line.time.toMSecsSinceEpoch(), writeData.outputLines.at(0).time.toMSecsSinceEpoch());
			QCOMPARE(line.lineNumber, writeData.outputLines.at(0).lineNumber);
			QCOMPARE(line.spans.size(), 1);
			QCOMPARE(line.spans.at(0).length, writeData.outputLines.at(0).spans.at(0).length);
			QCOMPARE(line.spans.at(0).fore, writeData.outputLines.at(0).spans.at(0).fore);
			QCOMPARE(line.spans.at(0).back, writeData.outputLines.at(0).spans.at(0).back);
			QCOMPARE(line.spans.at(0).actionType, writeData.outputLines.at(0).spans.at(0).actionType);
			QCOMPARE(line.spans.at(0).action, writeData.outputLines.at(0).spans.at(0).action);
			QCOMPARE(line.spans.at(0).hint, writeData.outputLines.at(0).spans.at(0).hint);
			QCOMPARE(line.spans.at(0).variable, writeData.outputLines.at(0).spans.at(0).variable);
			QCOMPARE(line.spans.at(0).bold, writeData.outputLines.at(0).spans.at(0).bold);
			QCOMPARE(line.spans.at(0).underline, writeData.outputLines.at(0).spans.at(0).underline);
			QCOMPARE(line.spans.at(0).italic, writeData.outputLines.at(0).spans.at(0).italic);
			QCOMPARE(line.spans.at(0).blink, writeData.outputLines.at(0).spans.at(0).blink);
			QCOMPARE(line.spans.at(0).strike, writeData.outputLines.at(0).spans.at(0).strike);
			QCOMPARE(line.spans.at(0).inverse, writeData.outputLines.at(0).spans.at(0).inverse);
			QCOMPARE(line.spans.at(0).changed, writeData.outputLines.at(0).spans.at(0).changed);
			QCOMPARE(line.spans.at(0).startTag, writeData.outputLines.at(0).spans.at(0).startTag);
		}

		void writeOutputOnlyAndHistoryOnly()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			QString       error;

			const QString outputOnlyPath = tempDir.filePath(QStringLiteral("output_only.qws"));
			QMudWorldSessionState::WorldSessionStateData outputOnly;
			outputOnly.hasOutputBuffer = true;
			outputOnly.outputLines.push_back(makeSampleLine());
			QVERIFY(QMudWorldSessionState::writeSessionStateFile(outputOnlyPath, outputOnly, &error));
			QMudWorldSessionState::WorldSessionStateData outputOnlyRead;
			QVERIFY(QMudWorldSessionState::readSessionStateFile(outputOnlyPath, &outputOnlyRead, &error));
			QVERIFY(outputOnlyRead.hasOutputBuffer);
			QVERIFY(!outputOnlyRead.hasCommandHistory);
			QCOMPARE(outputOnlyRead.outputLines.size(), 1);

			const QString historyOnlyPath = tempDir.filePath(QStringLiteral("history_only.qws"));
			QMudWorldSessionState::WorldSessionStateData historyOnly;
			historyOnly.hasCommandHistory = true;
			historyOnly.commandHistory    = {QStringLiteral("say hi"), QStringLiteral("look")};
			QVERIFY(QMudWorldSessionState::writeSessionStateFile(historyOnlyPath, historyOnly, &error));
			QMudWorldSessionState::WorldSessionStateData historyOnlyRead;
			QVERIFY(QMudWorldSessionState::readSessionStateFile(historyOnlyPath, &historyOnlyRead, &error));
			QVERIFY(!historyOnlyRead.hasOutputBuffer);
			QVERIFY(historyOnlyRead.hasCommandHistory);
			QCOMPARE(historyOnlyRead.commandHistory, historyOnly.commandHistory);
		}

		void removeSessionStateFileHandlesMissingAndExistingFiles()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString statePath = tempDir.filePath(QStringLiteral("remove_me.qws"));
			QString       error;

			QVERIFY(QMudWorldSessionState::removeSessionStateFile(statePath, &error));
			QVERIFY2(error.isEmpty(), qPrintable(error));

			QMudWorldSessionState::WorldSessionStateData data;
			data.hasCommandHistory = true;
			data.commandHistory    = {QStringLiteral("look")};
			QVERIFY(QMudWorldSessionState::writeSessionStateFile(statePath, data, &error));
			QVERIFY(QFileInfo::exists(statePath));

			QVERIFY(QMudWorldSessionState::removeSessionStateFile(statePath, &error));
			QVERIFY(!QFileInfo::exists(statePath));
		}

		void readFailsOnChecksumMismatch()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString statePath = tempDir.filePath(QStringLiteral("corrupt.qws"));
			QString       error;

			QMudWorldSessionState::WorldSessionStateData data;
			data.hasCommandHistory = true;
			data.commandHistory    = {QStringLiteral("north"), QStringLiteral("south")};
			QVERIFY(QMudWorldSessionState::writeSessionStateFile(statePath, data, &error));

			QFile file(statePath);
			QVERIFY(file.open(QIODevice::ReadOnly));
			QByteArray bytes = file.readAll();
			file.close();
			QVERIFY(!bytes.isEmpty());
			bytes[bytes.size() - 1] = static_cast<char>(bytes.back() ^ 0x5A);
			QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
			QVERIFY(file.write(bytes) == bytes.size());
			file.close();

			QMudWorldSessionState::WorldSessionStateData parsed;
			QVERIFY(!QMudWorldSessionState::readSessionStateFile(statePath, &parsed, &error));
			QVERIFY(!error.isEmpty());
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_WorldSessionStateUtils)


#if __has_include("tst_WorldSessionStateUtils.moc")
#include "tst_WorldSessionStateUtils.moc"
#endif
