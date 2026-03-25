/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldDocument_RoundTrip.cpp
 * Role: QTest coverage for WorldDocument RoundTrip behavior.
 */

#include "TestEnvironment.h"
#include "WorldDocument.h"

#include <QDir>
#include <QFile>
#include <QXmlStreamWriter>
#include <QtTest/QTest>

namespace
{
	QString fixturePath(const QString &relativePath)
	{
		return QDir(QStringLiteral(QMUD_TEST_SOURCE_DIR)).filePath(relativePath);
	}

	bool writeMapAttributes(QXmlStreamWriter &writer, const QMap<QString, QString> &attributes)
	{
		for (auto it = attributes.begin(); it != attributes.end(); ++it)
			writer.writeAttribute(it.key(), it.value());
		return !writer.hasError();
	}

	template <typename Entry>
	bool writeNamedSection(QXmlStreamWriter &writer, const QString &sectionTag, const QString &entryTag,
	                       const QList<Entry> &entries)
	{
		writer.writeStartElement(sectionTag);
		for (const Entry &entry : entries)
		{
			writer.writeStartElement(entryTag);
			if (!writeMapAttributes(writer, entry.attributes))
				return false;
			for (auto childIt = entry.children.begin(); childIt != entry.children.end(); ++childIt)
				writer.writeTextElement(childIt.key(), childIt.value());
			writer.writeEndElement();
		}
		writer.writeEndElement();
		return !writer.hasError();
	}

	bool writeRoundTripSnapshot(const WorldDocument &doc, const QString &filePath)
	{
		QFile file(filePath);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
			return false;

		QXmlStreamWriter writer(&file);
		writer.setAutoFormatting(true);
		writer.writeStartDocument(QStringLiteral("1.0"), true);
		writer.writeStartElement(QStringLiteral("qmud"));

		writer.writeStartElement(QStringLiteral("world"));
		if (!writeMapAttributes(writer, doc.worldAttributes()))
			return false;
		for (auto it = doc.worldMultilineAttributes().begin(); it != doc.worldMultilineAttributes().end(); ++it)
			writer.writeTextElement(it.key(), it.value());
		writer.writeEndElement();

		if (!writeNamedSection(writer, QStringLiteral("triggers"), QStringLiteral("trigger"), doc.triggers()))
			return false;
		if (!writeNamedSection(writer, QStringLiteral("aliases"), QStringLiteral("alias"), doc.aliases()))
			return false;
		if (!writeNamedSection(writer, QStringLiteral("timers"), QStringLiteral("timer"), doc.timers()))
			return false;

		writer.writeStartElement(QStringLiteral("includes"));
		for (const WorldDocument::Include &include : doc.includes())
		{
			writer.writeStartElement(QStringLiteral("include"));
			if (!writeMapAttributes(writer, include.attributes))
				return false;
			writer.writeEndElement();
		}
		writer.writeEndElement();

		writer.writeEndElement();
		writer.writeEndDocument();
		return !writer.hasError();
	}

	template <typename Entry>
	void compareNamedEntries(const QList<Entry> &left, const QList<Entry> &right)
	{
		QCOMPARE(left.size(), right.size());
		for (qsizetype i = 0; i < left.size(); ++i)
		{
			QCOMPARE(left[i].attributes, right[i].attributes);
			QCOMPARE(left[i].children, right[i].children);
		}
	}
} // namespace

/**
 * @brief QTest fixture covering WorldDocument RoundTrip scenarios.
 */
class tst_WorldDocument_RoundTrip : public QObject
{
		Q_OBJECT

	private slots:
		// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
		void initTestCase()
		{
			QMudTest::applyDeterministicTestEnvironment();
		}

		// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
		void representativeSectionsCanRoundTrip()
		{
			const QString sourcePath =
			    fixturePath(QStringLiteral("tests/data/worlds/world_sections.xml"));

			WorldDocument source;
			QVERIFY2(source.loadFromFile(sourcePath), qPrintable(source.errorString()));
			QVERIFY(source.warnings().isEmpty());

			QMudTest::ScopedTempDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString roundTripPath = QDir(tempDir.path()).filePath(QStringLiteral("roundtrip.xml"));
			QVERIFY(writeRoundTripSnapshot(source, roundTripPath));

			WorldDocument loaded;
			QVERIFY2(loaded.loadFromFile(roundTripPath), qPrintable(loaded.errorString()));
			QVERIFY(loaded.warnings().isEmpty());

			QCOMPARE(loaded.worldAttributes(), source.worldAttributes());
			QCOMPARE(loaded.worldMultilineAttributes(), source.worldMultilineAttributes());
			compareNamedEntries(loaded.triggers(), source.triggers());
			compareNamedEntries(loaded.aliases(), source.aliases());
			compareNamedEntries(loaded.timers(), source.timers());

			QCOMPARE(loaded.includes().size(), source.includes().size());
			for (qsizetype i = 0; i < loaded.includes().size(); ++i)
				QCOMPARE(loaded.includes()[i].attributes, source.includes()[i].attributes);
		}
};

QTEST_APPLESS_MAIN(tst_WorldDocument_RoundTrip)


#if __has_include("tst_WorldDocument_RoundTrip.moc")
#include "tst_WorldDocument_RoundTrip.moc"
#endif
