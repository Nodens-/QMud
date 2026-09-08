/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_LuaApiExport.cpp
 * Role: Unit coverage for exporting the runtime Lua API inventory and its count manifest.
 */

#include "LuaApiExport.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QMap>
#include <QSet>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest/QTest>

#include <algorithm>
#include <optional>

namespace
{
	/**
	 * @brief Reads the data entries from one exported inventory file.
	 * @param path Inventory path.
	 * @return Entries after the generated header, or no value when the file is invalid.
	 */
	std::optional<QStringList> readInventoryEntries(const QString &path)
	{
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
			return {};

		QTextStream stream(&file);
		if (stream.readLine() != QStringLiteral("# QMud Lua API inventory export"))
			return {};
		if (!stream.readLine().startsWith(QStringLiteral("# Exported (UTC): ")))
			return {};
		if (!stream.readLine().isEmpty())
			return {};

		QStringList entries;
		while (!stream.atEnd())
			entries.append(stream.readLine());
		return entries;
	}

	/**
	 * @brief QTest fixture covering Lua API inventory export behavior.
	 */
	class tst_LuaApiExport final : public QObject
	{
			Q_OBJECT

		private slots:
			static void rejectsEmptyOrUncreatableOutputDirectory()
			{
				QString error;
				QVERIFY(!exportLuaApiInventory(QStringLiteral(" \t "), &error));
				QCOMPARE(error, QStringLiteral("Output directory is empty."));

				QTemporaryDir directory(QDir::current().filePath(QStringLiteral("tst_LuaApiExport-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString filePath = directory.filePath(QStringLiteral("not-a-directory"));
				QFile         file(filePath);
				QVERIFY(file.open(QIODevice::WriteOnly));
				file.close();

				error.clear();
				QVERIFY(!exportLuaApiInventory(filePath, &error));
				QVERIFY(error.startsWith(QStringLiteral("Failed to create output directory")));
			}

			static void exportsSortedInventoriesAndConsistentCounts()
			{
				QTemporaryDir directory(QDir::current().filePath(QStringLiteral("tst_LuaApiExport-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString outputPath = directory.filePath(QStringLiteral("inventory"));

				QString       error;
				QVERIFY2(exportLuaApiInventory(outputPath, &error), qPrintable(error));

				const QStringList          suffixes = {QStringLiteral("world_binding_table"),
				                                       QStringLiteral("utils_binding_table"),
				                                       QStringLiteral("function_signature_table"),
				                                       QStringLiteral("worldlib"),
				                                       QStringLiteral("worldlib_meta"),
				                                       QStringLiteral("utils_union"),
				                                       QStringLiteral("utils_xmllib"),
				                                       QStringLiteral("utils_compresslib"),
				                                       QStringLiteral("sqlite_cnt"),
				                                       QStringLiteral("bitlib"),
				                                       QStringLiteral("rexlib"),
				                                       QStringLiteral("rex_pcremeta"),
				                                       QStringLiteral("bc"),
				                                       QStringLiteral("lpeg"),
				                                       QStringLiteral("lpeg_meta"),
				                                       QStringLiteral("progress_lib"),
				                                       QStringLiteral("progress_meta")};

				QMap<QString, QStringList> inventories;
				for (const QString &suffix : suffixes)
				{
					const QString path = QDir(outputPath).filePath(QStringLiteral("qmud_%1.txt").arg(suffix));
					QVERIFY2(QFile::exists(path), qPrintable(path));
					auto entries = readInventoryEntries(path);
					QVERIFY2(entries.has_value(), qPrintable(path));
					QVERIFY(std::ranges::is_sorted(*entries));
					QCOMPARE(QSet<QString>(entries->cbegin(), entries->cend()).size(), entries->size());

					const QString canonicalPath =
					    QDir(QStringLiteral(QMUD_TEST_SOURCE_DIR))
					        .filePath(
					            QStringLiteral("skeleton/docs/lua_api_inventory/qmud_%1.txt").arg(suffix));
					const auto canonicalEntries = readInventoryEntries(canonicalPath);
					QVERIFY2(canonicalEntries.has_value(), qPrintable(canonicalPath));
					QCOMPARE(*entries, *canonicalEntries);

					inventories.insert(suffix, *entries);
				}

				QVERIFY(
				    inventories.value(QStringLiteral("worldlib")).contains(QStringLiteral("SetAlphaOption")));
				QVERIFY(inventories.value(QStringLiteral("worldlib"))
				            .contains(QStringLiteral("SetAsyncResultFilter")));
				QVERIFY(inventories.value(QStringLiteral("utils_compresslib"))
				            .contains(QStringLiteral("base64encode")));
				QVERIFY(
				    inventories.value(QStringLiteral("sqlite_cnt")).contains(QStringLiteral("sqlite3.open")));

				const QStringList &signatureEntries =
				    inventories.value(QStringLiteral("function_signature_table"));
				QSet<QString> signatureNames;
				for (const QString &entry : signatureEntries)
				{
					const qsizetype separator = entry.indexOf(QLatin1Char('\t'));
					QVERIFY(separator > 0);
					signatureNames.insert(entry.first(separator));
				}
				QCOMPARE(signatureNames.size(), signatureEntries.size());
				for (const QString &functionName : inventories.value(QStringLiteral("worldlib")))
					QVERIFY2(signatureNames.contains(functionName), qPrintable(functionName));

				const QString countPath    = QDir(outputPath).filePath(QStringLiteral("qmud_api_count.txt"));
				const auto    countEntries = readInventoryEntries(countPath);
				QVERIFY2(countEntries.has_value(), qPrintable(countPath));
				QCOMPARE(countEntries->size(), suffixes.size());
				QVERIFY(std::ranges::is_sorted(*countEntries));
				QCOMPARE(QSet<QString>(countEntries->cbegin(), countEntries->cend()).size(),
				         countEntries->size());

				const QString canonicalCountPath =
				    QDir(QStringLiteral(QMUD_TEST_SOURCE_DIR))
				        .filePath(QStringLiteral("skeleton/docs/lua_api_inventory/qmud_api_count.txt"));
				const auto canonicalCountEntries = readInventoryEntries(canonicalCountPath);
				QVERIFY2(canonicalCountEntries.has_value(), qPrintable(canonicalCountPath));
				QCOMPARE(*countEntries, *canonicalCountEntries);

				for (const QString &entry : *countEntries)
				{
					const qsizetype comma = entry.indexOf(QLatin1Char(','));
					QVERIFY(comma > 0);
					bool      ok    = false;
					const int count = entry.sliced(comma + 1).toInt(&ok);
					QVERIFY(ok);
					const QString suffix = entry.first(comma);
					QVERIFY(inventories.contains(suffix));
					QCOMPARE(count, inventories.value(suffix).size());
				}
			}
	};
} // namespace

QTEST_MAIN(tst_LuaApiExport)

#if __has_include("tst_LuaApiExport.moc")
#include "tst_LuaApiExport.moc"
#endif
