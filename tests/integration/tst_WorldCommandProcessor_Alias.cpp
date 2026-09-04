/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldCommandProcessor_Alias.cpp
 * Role: QTest coverage for WorldCommandProcessor Alias behavior.
 */

#include "AliasMatchUtils.h"
#include "WorldCommandProcessor.h"
#include "WorldCommandProcessorUtils.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "WorldRuntimeTestAccess.h"
#include "WorldView.h"
#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QRegularExpression>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QtTest/QTest>

namespace
{
	const QString kAliasCapturePluginId = QStringLiteral("66778899aabbccddeeff0011");

	/**
	 * @brief Writes a Lua plugin fixture containing the mapper signpost alias.
	 * @param path Destination plugin file path.
	 * @return `true` when the complete fixture was written.
	 */
	bool          writeAliasCapturePlugin(const QString &path)
	{
		QFile file(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
			return false;

		const QString xml = QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="AliasCapture"
    author="QMud Test"
    id=")xml") + kAliasCapturePluginId +
		                    QStringLiteral(R"xml("
    language="lua"
    enabled="y"
    save_state="n"
    sequence="100">
    <script><![CDATA[
function capture_mapper_signpost(name, line, wildcards)
  local status = "missing"
  if wildcards.id ~= nil then
    status = "present:" .. wildcards.id
  end
  SetVariable("mapper_signpost_id", status)
  SetVariable("mapper_signpost_line", line)
end
]]></script>
  </plugin>
  <aliases>
    <alias
      enabled="y"
      match="^mapper[ ]+(sp|signpost)(|[ ]+(?&lt;id>[0-9A-F]+))$"
      regexp="y"
      sequence="100"
      send_to="2"
      script="capture_mapper_signpost"/>
  </aliases>
</muclient>
)xml");
		const QByteArray bytes = xml.toUtf8();
		return file.write(bytes) == bytes.size();
	}

	/**
	 * @brief Binds a runtime to the real view and command-processing path used by plugin installation.
	 */
	struct RuntimeCommandHarness
	{
			/**
			 * @brief Constructs and wires the runtime, view, and command processor.
			 * @param boundRuntime Runtime to bind.
			 */
			explicit RuntimeCommandHarness(WorldRuntime &boundRuntime) : runtime(boundRuntime)
			{
				view.resize(640, 480);
				processor.setView(&view);
				processor.setRuntime(&runtime);
				runtime.setCommandProcessor(&processor);
				view.setRuntime(&runtime);
			}

			/**
			 * @brief Detaches runtime pointers before harness members are destroyed.
			 */
			~RuntimeCommandHarness()
			{
				view.setRuntime(nullptr);
				runtime.setCommandProcessor(nullptr);
				processor.setRuntime(nullptr);
			}

			/**
			 * @brief Shows the view and waits until it can service plugin installation.
			 * @return `true` when the view is exposed.
			 */
			bool showAndWait()
			{
				view.show();
				return QTest::qWaitForWindowExposed(&view);
			}

			WorldRuntime         &runtime;
			WorldView             view;
			WorldCommandProcessor processor;
	};

	/**
	 * @brief QTest fixture covering WorldCommandProcessor Alias scenarios.
	 */
	class tst_WorldCommandProcessor_Alias final : public QObject
	{
			Q_OBJECT

		private slots:
			static void wildcardAliasCapturesWildcards()
			{
				const QString pattern = QMudCommandPattern::convertToRegularExpression(
				    QStringLiteral("buy * from *"), true, true);
				const QRegularExpression regex(pattern);
				QVERIFY(regex.isValid());

				const QMudAliasMatch::MatchResult result =
				    QMudAliasMatch::matchWithCaptures(regex, QStringLiteral("buy sword from merchant"), true);
				QVERIFY(result.matched);
				QCOMPARE(result.wildcards.size(), 3);
				QCOMPARE(result.wildcards.at(1), QStringLiteral("sword"));
				QCOMPARE(result.wildcards.at(2), QStringLiteral("merchant"));
			}

			static void regexpAliasCapturesNamedWildcard()
			{
				const QRegularExpression regex(QStringLiteral("^give (?<what>\\w+) to (?<who>\\w+)$"));
				QVERIFY(regex.isValid());

				const QMudAliasMatch::MatchResult result =
				    QMudAliasMatch::matchWithCaptures(regex, QStringLiteral("give coin to guard"), true);
				QVERIFY(result.matched);
				QCOMPARE(result.namedWildcards.value(QStringLiteral("what")), QStringLiteral("coin"));
				QCOMPARE(result.namedWildcards.value(QStringLiteral("who")), QStringLiteral("guard"));
			}

			static void regexpAliasMaterializesUnmatchedTrailingCaptures()
			{
				const QRegularExpression regex(
				    QStringLiteral("^mapper[ ]+(sp|signpost)(|[ ]+(?<id>[0-9A-F]+))$"));
				QVERIFY(regex.isValid());

				const QMudAliasMatch::MatchResult result =
				    QMudAliasMatch::matchWithCaptures(regex, QStringLiteral("mapper signpost"), true);
				QVERIFY(result.matched);
				QCOMPARE(result.wildcards.size(), regex.captureCount() + 1);
				QCOMPARE(result.wildcards.at(2), QStringLiteral(""));
				QVERIFY(!result.wildcards.at(2).isNull());
				QCOMPARE(result.wildcards.at(3), QStringLiteral(""));
				QVERIFY(!result.wildcards.at(3).isNull());
				QVERIFY(result.namedWildcards.contains(QStringLiteral("id")));
				QCOMPARE(result.namedWildcards.value(QStringLiteral("id")), QStringLiteral(""));
				QVERIFY(!result.namedWildcards.value(QStringLiteral("id")).isNull());
			}

			static void regexpAliasPreservesParticipatingDuplicateNamedCapture()
			{
				const QRegularExpression regex(QStringLiteral("(?J)^(?:(?<value>first)|(?<value>second))$"));
				QVERIFY(regex.isValid());

				const QMudAliasMatch::MatchResult firstResult =
				    QMudAliasMatch::matchWithCaptures(regex, QStringLiteral("first"), true);
				QVERIFY(firstResult.matched);
				QCOMPARE(firstResult.wildcards.size(), regex.captureCount() + 1);
				QCOMPARE(firstResult.wildcards.at(2), QStringLiteral(""));
				QCOMPARE(firstResult.namedWildcards.value(QStringLiteral("value")), QStringLiteral("first"));

				const QMudAliasMatch::MatchResult secondResult =
				    QMudAliasMatch::matchWithCaptures(regex, QStringLiteral("second"), true);
				QVERIFY(secondResult.matched);
				QCOMPARE(secondResult.wildcards.size(), regex.captureCount() + 1);
				QCOMPARE(secondResult.wildcards.at(1), QStringLiteral(""));
				QCOMPARE(secondResult.namedWildcards.value(QStringLiteral("value")),
				         QStringLiteral("second"));
			}

			static void regexpAliasDispatchesUnmatchedNamedCaptureAsEmptyLuaString()
			{
				WorldRuntime runtime;
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));
				runtime.setLuaScriptText(QStringLiteral(R"lua(
function capture_mapper_signpost(name, line, wildcards)
  local status = "missing"
  if wildcards.id ~= nil then
    status = "present:" .. wildcards.id
  end
  SetVariable("mapper_signpost_id", status)
  SetVariable("mapper_signpost_line", line)
end
)lua"));

				WorldRuntime::Alias alias;
				alias.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
				alias.attributes.insert(QStringLiteral("match"),
				                        QStringLiteral("^mapper[ ]+(sp|signpost)(|[ ]+(?<id>[0-9A-F]+))$"));
				alias.attributes.insert(QStringLiteral("regexp"), QStringLiteral("y"));
				alias.attributes.insert(QStringLiteral("sequence"), QStringLiteral("100"));
				alias.attributes.insert(QStringLiteral("send_to"), QString::number(eSendToOutput));
				alias.attributes.insert(QStringLiteral("script"), QStringLiteral("capture_mapper_signpost"));
				WorldRuntimeTestAccess::aliases(runtime).push_back(alias);
				runtime.markAliasesChanged();

				WorldCommandProcessor processor;
				processor.setRuntime(&runtime);

				QCOMPARE(processor.executeCommand(QStringLiteral("mapper signpost")), eOK);
				QString value;
				QVERIFY(runtime.findVariable(QStringLiteral("mapper_signpost_id"), value));
				QCOMPARE(value, QStringLiteral("present:"));
				QVERIFY(runtime.findVariable(QStringLiteral("mapper_signpost_line"), value));
				QCOMPARE(value, QStringLiteral("mapper signpost"));

				QCOMPARE(processor.executeCommand(QStringLiteral("mapper signpost ABC")), eOK);
				QVERIFY(runtime.findVariable(QStringLiteral("mapper_signpost_id"), value));
				QCOMPARE(value, QStringLiteral("present:ABC"));
				QCOMPARE(runtime.aliases().constFirst().matched, 2);
			}

			static void pluginRegexpAliasDispatchesUnmatchedNamedCaptureAsEmptyLuaString()
			{
				QTemporaryDir tempDir;
				QVERIFY(tempDir.isValid());
				const QString pluginFileName = QStringLiteral("alias_capture.xml");
				QVERIFY(writeAliasCapturePlugin(QDir(tempDir.path()).filePath(pluginFileName)));

				WorldRuntime runtime;
				runtime.setStartupDirectory(tempDir.path());
				runtime.setPluginsDirectory(QStringLiteral("."));
				runtime.setWorldAttribute(QStringLiteral("enable_aliases"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("enable_scripts"), QStringLiteral("y"));
				runtime.setWorldAttribute(QStringLiteral("script_language"), QStringLiteral("Lua"));

				RuntimeCommandHarness harness(runtime);
				QVERIFY(harness.showAndWait());

				QString loadError;
				QVERIFY2(runtime.loadPluginFile(pluginFileName, &loadError), qPrintable(loadError));
				QTRY_VERIFY_WITH_TIMEOUT(
				    runtime.plugins().size() == 1 && !runtime.plugins().constFirst().installPending, 5000);
				QVERIFY(runtime.plugins().constFirst().lua);
				QCOMPARE(runtime.plugins().constFirst().aliases.size(), 1);

				QCOMPARE(harness.processor.executeCommand(QStringLiteral("mapper signpost")), eOK);
				QString value;
				QVERIFY(runtime.findPluginVariable(kAliasCapturePluginId,
				                                   QStringLiteral("mapper_signpost_id"), value));
				QCOMPARE(value, QStringLiteral("present:"));
				QVERIFY(runtime.findPluginVariable(kAliasCapturePluginId,
				                                   QStringLiteral("mapper_signpost_line"), value));
				QCOMPARE(value, QStringLiteral("mapper signpost"));

				QCOMPARE(harness.processor.executeCommand(QStringLiteral("mapper signpost ABC")), eOK);
				QVERIFY(runtime.findPluginVariable(kAliasCapturePluginId,
				                                   QStringLiteral("mapper_signpost_id"), value));
				QCOMPARE(value, QStringLiteral("present:ABC"));
				QCOMPARE(runtime.plugins().constFirst().aliases.constFirst().matched, 2);
			}

			static void ignoreCaseOptionControlsAliasMatch()
			{
				const QString pattern =
				    QMudCommandPattern::convertToRegularExpression(QStringLiteral("Cast *"), true, true);

				const QRegularExpression sensitiveRegex(pattern);
				QVERIFY(!QMudAliasMatch::matchWithCaptures(sensitiveRegex, QStringLiteral("cast fire"), true)
				             .matched);

				const QRegularExpression insensitiveRegex(pattern, QRegularExpression::CaseInsensitiveOption);
				const QMudAliasMatch::MatchResult insensitiveResult =
				    QMudAliasMatch::matchWithCaptures(insensitiveRegex, QStringLiteral("cast fire"), true);
				QVERIFY(insensitiveResult.matched);
				QCOMPARE(insensitiveResult.wildcards.at(1), QStringLiteral("fire"));
			}

			static void emptyRegexpMatchCanBeRejected()
			{
				const QRegularExpression regex(QStringLiteral("^.*$"));
				QVERIFY(regex.isValid());

				QVERIFY(!QMudAliasMatch::matchWithCaptures(regex, QString(), false).matched);
				QVERIFY(QMudAliasMatch::matchWithCaptures(regex, QString(), true).matched);
			}

			static void recursionGuardRespectsMaxDepth()
			{
				QVERIFY(!QMudAliasMatch::exceedsExecutionDepth(0, 20));
				QVERIFY(!QMudAliasMatch::exceedsExecutionDepth(19, 20));
				QVERIFY(QMudAliasMatch::exceedsExecutionDepth(20, 20));
			}
	};
} // namespace

QTEST_MAIN(tst_WorldCommandProcessor_Alias)

#if __has_include("tst_WorldCommandProcessor_Alias.moc")
#include "tst_WorldCommandProcessor_Alias.moc"
#endif
