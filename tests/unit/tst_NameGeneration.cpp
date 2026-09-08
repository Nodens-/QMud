/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_NameGeneration.cpp
 * Role: Unit coverage for name dictionary loading and deterministic name generation.
 */

#include "NameGeneration.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <stdexcept>

namespace
{
	/**
	 * @brief Writes one dictionary fixture into a test-owned directory.
	 * @param directory Fixture directory.
	 * @param fileName Fixture file name.
	 * @param contents Dictionary contents.
	 * @return Absolute fixture path.
	 */
	QString writeDictionary(const QTemporaryDir &directory, const QString &fileName,
	                        const QByteArray &contents)
	{
		QString path = directory.filePath(fileName);
		QFile   file(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(contents) != contents.size())
			return {};
		return path;
	}

	/**
	 * @brief QTest fixture covering name dictionary parsing and generation.
	 */
	class tst_NameGeneration final : public QObject
	{
			Q_OBJECT

		private slots:
			static void loadsCanonicalAndAlternateDictionaryTags()
			{
				QTemporaryDir directory(
				    QDir::current().filePath(QStringLiteral("tst_NameGeneration-XXXXXX")));
				QVERIFY(directory.isValid());

				const QString canonical = writeDictionary(
				    directory, QStringLiteral("canonical.nam"),
				    QByteArrayLiteral("/* ignored header\n[START]\nAl\n[MIDDLE]\nex\n[END]\nian\n[STOP]\n"));
				QVERIFY(!canonical.isEmpty());

				NameGenerator generator;
				QVERIFY(!generator.isLoaded());
				generator.setAppController(nullptr);
				generator.readNames(canonical, true);
				QVERIFY(generator.isLoaded());
				QCOMPARE(generator.generateName(), QStringLiteral("Alexian"));

				const QString alternate = writeDictionary(
				    directory, QStringLiteral("alternate.nam"),
				    QByteArrayLiteral("[startstav]\nBel\n[mittstav]\nla\n[slutstav]\nra\n[stop]\n"));
				QVERIFY(!alternate.isEmpty());
				generator.readNames(alternate, true);
				QCOMPARE(generator.generateName(), QStringLiteral("Bellara"));
			}

			static void rejectsIncompleteOrUnreadableDictionaries_data()
			{
				QTest::addColumn<QByteArray>("contents");

				QTest::newRow("missing-start") << QByteArrayLiteral("name\n");
				QTest::newRow("missing-middle") << QByteArrayLiteral("[start]\nAl\n");
				QTest::newRow("missing-end") << QByteArrayLiteral("[start]\nAl\n[middle]\nex\n");
				QTest::newRow("missing-stop") << QByteArrayLiteral("[start]\nAl\n[middle]\nex\n[end]\nian\n");
			}

			static void rejectsIncompleteOrUnreadableDictionaries()
			{
				QFETCH(QByteArray, contents);

				QTemporaryDir directory(
				    QDir::current().filePath(QStringLiteral("tst_NameGeneration-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString path = writeDictionary(directory, QStringLiteral("incomplete.nam"), contents);
				QVERIFY(!path.isEmpty());

				NameGenerator generator;
				QVERIFY_THROWS_EXCEPTION(std::runtime_error, generator.readNames(path, true));
				QVERIFY(!generator.isLoaded());

				QVERIFY_THROWS_EXCEPTION(
				    std::runtime_error,
				    generator.readNames(directory.filePath(QStringLiteral("missing.nam")), true));
				QVERIFY(!generator.isLoaded());
			}

			static void emptySourceAndEmptyComponentsDoNotGenerateNames()
			{
				QTemporaryDir directory(
				    QDir::current().filePath(QStringLiteral("tst_NameGeneration-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString path =
				    writeDictionary(directory, QStringLiteral("empty-component.nam"),
				                    QByteArrayLiteral("[start]\n[middle]\nmid\n[end]\nlast\n[stop]\n"));
				QVERIFY(!path.isEmpty());

				NameGenerator generator;
				generator.readNames(path, true);
				QVERIFY(generator.isLoaded());
				QVERIFY(generator.generateName().isEmpty());

				generator.readNames(QString(), true);
				QVERIFY(!generator.isLoaded());
			}

			static void globalGeneratorUsesExplicitDictionary()
			{
				QTemporaryDir directory(
				    QDir::current().filePath(QStringLiteral("tst_NameGeneration-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString path =
				    writeDictionary(directory, QStringLiteral("global.nam"),
				                    QByteArrayLiteral("[start]\nCor\n[middle]\nde\n[end]\nlia\n[stop]\n"));
				QVERIFY(!path.isEmpty());

				qmudReadNames(path, true);
				QCOMPARE(qmudGenerateName(), QStringLiteral("Cordelia"));
			}
	};
} // namespace

QTEST_APPLESS_MAIN(tst_NameGeneration)

#if __has_include("tst_NameGeneration.moc")
#include "tst_NameGeneration.moc"
#endif
