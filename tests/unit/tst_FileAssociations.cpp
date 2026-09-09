/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_FileAssociations.cpp
 * Role: Verifies the public file-association contract and generated macOS bundle declarations.
 */

#include "FileExtensions.h"

#include <QFile>
#include <QSet>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVariantList>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVariantMap>
// ReSharper disable once CppUnusedIncludeDirective
#include <QXmlStreamReader>
#include <QtTest>

namespace
{
	/** @brief Parses a plist value whose start element is current. */
	QVariant readPlistValue(QXmlStreamReader &reader)
	{
		const QStringView name = reader.name();
		if (name == QLatin1String("string"))
			return reader.readElementText();
		if (name == QLatin1String("true"))
		{
			reader.skipCurrentElement();
			return true;
		}
		if (name == QLatin1String("false"))
		{
			reader.skipCurrentElement();
			return false;
		}
		if (name == QLatin1String("array"))
		{
			QVariantList values;
			while (reader.readNextStartElement())
				values.push_back(readPlistValue(reader));
			return values;
		}
		if (name == QLatin1String("dict"))
		{
			QVariantMap values;
			while (reader.readNextStartElement())
			{
				if (reader.name() != QLatin1String("key"))
				{
					reader.raiseError(QStringLiteral("Expected plist dictionary key"));
					return {};
				}
				const QString key = reader.readElementText();
				if (!reader.readNextStartElement())
				{
					reader.raiseError(QStringLiteral("Missing plist dictionary value"));
					return {};
				}
				values.insert(key, readPlistValue(reader));
			}
			return values;
		}

		reader.raiseError(QStringLiteral("Unsupported plist value type"));
		return {};
	}

	/** @brief Reads the generated plist root dictionary. */
	QVariantMap readGeneratedPlist(QString *error)
	{
		QFile file(QStringLiteral(QMUD_TEST_GENERATED_INFO_PLIST));
		if (!file.open(QIODevice::ReadOnly))
		{
			*error = file.errorString();
			return {};
		}

		QXmlStreamReader reader(&file);
		while (reader.readNextStartElement())
		{
			if (reader.name() != QLatin1String("plist"))
			{
				reader.skipCurrentElement();
				continue;
			}
			if (!reader.readNextStartElement() || reader.name() != QLatin1String("dict"))
			{
				reader.raiseError(QStringLiteral("Missing plist root dictionary"));
				break;
			}
			const QVariantMap result = readPlistValue(reader).toMap();
			if (!reader.hasError())
				return result;
			break;
		}

		*error = reader.errorString();
		return {};
	}

	class tst_FileAssociations final : public QObject
	{
			Q_OBJECT

		private slots:
			/** @brief Verifies every declared association matches the public contract and drives extension behavior. */
			static void tableEntriesDriveExtensionBehavior();
			/** @brief Verifies process argument recognition preserves one exact registered path. */
			static void recognizesOnlySoleFileAssociationArguments();
			/** @brief Verifies generated macOS declarations exactly match the public association contract. */
			static void generatedPlistMatchesAssociationContract();
	};

	void tst_FileAssociations::tableEntriesDriveExtensionBehavior()
	{
		const QList<QMudFileExtensions::Entry> &entries = QMudFileExtensions::entries();
		QVERIFY(!entries.isEmpty());

		QSet<QString> programIds;
		QSet<QString> mimeTypes;
		QSet<QString> extensions;
		QSet<QString> uniformTypeIdentifiers;
		for (const QMudFileExtensions::Entry &entry : entries)
		{
			const QString programId             = QString::fromLatin1(entry.programIdSuffix);
			const QString mimeType              = QString::fromLatin1(entry.mimeType);
			const QString modernExtension       = QString::fromLatin1(entry.modernExtension);
			const QString legacyExtension       = QString::fromLatin1(entry.legacyExtension);
			const QString uniformTypeIdentifier = QString::fromLatin1(entry.uniformTypeIdentifier);
			QVERIFY(!QString::fromLatin1(entry.description).isEmpty());
			QVERIFY(!programId.isEmpty());
			QVERIFY(!mimeType.isEmpty());
			QVERIFY(!modernExtension.isEmpty());
			QVERIFY(!legacyExtension.isEmpty());
			QVERIFY(!uniformTypeIdentifier.isEmpty());
			QVERIFY(!programIds.contains(programId));
			QVERIFY(!mimeTypes.contains(mimeType));
			QVERIFY(!extensions.contains(modernExtension));
			QVERIFY(!extensions.contains(legacyExtension));
			QVERIFY(!uniformTypeIdentifiers.contains(uniformTypeIdentifier));
			programIds.insert(programId);
			mimeTypes.insert(mimeType);
			extensions.insert(modernExtension);
			extensions.insert(legacyExtension);
			uniformTypeIdentifiers.insert(uniformTypeIdentifier);
			QCOMPARE(QMudFileExtensions::findByProgramIdSuffix(entry.programIdSuffix), &entry);
			QCOMPARE(QMudFileExtensions::findByExtension(modernExtension), &entry);
			QCOMPARE(QMudFileExtensions::findByExtension(legacyExtension.toUpper()), &entry);
			QCOMPARE(QMudFileExtensions::modernForSuffix(modernExtension), modernExtension);
			QCOMPARE(QMudFileExtensions::modernForSuffix(legacyExtension), modernExtension);
			QCOMPARE(QMudFileExtensions::canonicalizePathExtension(
			             QStringLiteral("document.%1").arg(legacyExtension)),
			         QStringLiteral("document.%1").arg(modernExtension));
		}

		QCOMPARE(programIds, QSet<QString>({QStringLiteral("World"), QStringLiteral("Triggers"),
		                                    QStringLiteral("Aliases"), QStringLiteral("Timers"),
		                                    QStringLiteral("Colours"), QStringLiteral("Macros"),
		                                    QStringLiteral("Variables")}));
		QVERIFY(QMudFileExtensions::isWorldSuffix(QStringLiteral("qdl")));
		QVERIFY(QMudFileExtensions::isWorldSuffix(QStringLiteral("mcl")));
		QVERIFY(QMudFileExtensions::isLegacyWorldSuffix(QStringLiteral("mcl")));
		QVERIFY(!QMudFileExtensions::isLegacyWorldSuffix(QStringLiteral("qdl")));
		QVERIFY(QMudFileExtensions::findByProgramIdSuffix("Unknown") == nullptr);
		QVERIFY(QMudFileExtensions::findByExtension(QStringLiteral("txt")) == nullptr);
	}

	void tst_FileAssociations::recognizesOnlySoleFileAssociationArguments()
	{
		const QString exactPath = QStringLiteral("/QMud home/a world.QDL");
		QCOMPARE(QMudFileExtensions::fileAssociationPathArgument({QStringLiteral("QMud"), exactPath}),
		         exactPath);
		QCOMPARE(QMudFileExtensions::fileAssociationPathArgument(
		             {QStringLiteral("QMud"), QStringLiteral("relative/file.mct")}),
		         QStringLiteral("relative/file.mct"));
		QCOMPARE(QMudFileExtensions::fileAssociationPathArgument(
		             {QStringLiteral("QMud"), QStringLiteral("--noauto"), exactPath}),
		         exactPath);
		QVERIFY(QMudFileExtensions::fileAssociationPathArgument({QStringLiteral("QMud")}).isEmpty());
		QVERIFY(QMudFileExtensions::fileAssociationPathArgument(
		            {QStringLiteral("QMud"), QStringLiteral("file.txt")})
		            .isEmpty());
		QVERIFY(QMudFileExtensions::fileAssociationPathArgument(
		            {QStringLiteral("QMud"), exactPath, QStringLiteral("extra")})
		            .isEmpty());
	}

	void tst_FileAssociations::generatedPlistMatchesAssociationContract()
	{
		QString           error;
		const QVariantMap plist = readGeneratedPlist(&error);
		QVERIFY2(!plist.isEmpty(), qPrintable(error));

		const QVariantList documentTypes = plist.value(QStringLiteral("CFBundleDocumentTypes")).toList();
		const QVariantList exportedTypes = plist.value(QStringLiteral("UTExportedTypeDeclarations")).toList();
		const QList<QMudFileExtensions::Entry> &entries = QMudFileExtensions::entries();
		QCOMPARE(documentTypes.size(), entries.size());
		QCOMPARE(exportedTypes.size(), entries.size());

		for (qsizetype index = 0; index < entries.size(); ++index)
		{
			const QMudFileExtensions::Entry &expected     = entries.at(index);
			const QVariantMap                documentType = documentTypes.at(index).toMap();
			QCOMPARE(documentType.value(QStringLiteral("CFBundleTypeName")).toString(),
			         QString::fromLatin1(expected.description));
			QCOMPARE(documentType.value(QStringLiteral("CFBundleTypeRole")).toString(),
			         QStringLiteral("Editor"));
			QCOMPARE(documentType.value(QStringLiteral("LSHandlerRank")).toString(), QStringLiteral("Owner"));
			QCOMPARE(documentType.value(QStringLiteral("LSItemContentTypes")).toStringList(),
			         QStringList({QString::fromLatin1(expected.uniformTypeIdentifier)}));

			const QVariantMap exportedType = exportedTypes.at(index).toMap();
			QCOMPARE(exportedType.value(QStringLiteral("UTTypeDescription")).toString(),
			         QString::fromLatin1(expected.description));
			QCOMPARE(exportedType.value(QStringLiteral("UTTypeIdentifier")).toString(),
			         QString::fromLatin1(expected.uniformTypeIdentifier));
			QCOMPARE(exportedType.value(QStringLiteral("UTTypeConformsTo")).toStringList(),
			         QStringList({QStringLiteral("public.xml")}));
			const QVariantMap tags = exportedType.value(QStringLiteral("UTTypeTagSpecification")).toMap();
			QCOMPARE(tags.value(QStringLiteral("public.filename-extension")).toStringList(),
			         QStringList({QString::fromLatin1(expected.modernExtension),
			                      QString::fromLatin1(expected.legacyExtension)}));
			QCOMPARE(tags.value(QStringLiteral("public.mime-type")).toString(),
			         QString::fromLatin1(expected.mimeType));
		}
	}

} // namespace

QTEST_MAIN(tst_FileAssociations)

#include "tst_FileAssociations.moc"
