/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_ImportMergeUtils.cpp
 * Role: QTest coverage for ImportMergeUtils behavior.
 */

#include "ImportMergeUtils.h"

#include <QtTest/QTest>

namespace
{
	struct NamedItem
	{
		QMap<QString, QString> attributes;
		QMap<QString, QString> children;
	};

	struct VariableItem
	{
		QMap<QString, QString> attributes;
		QString                content;
	};

	NamedItem makeNamed(const QString &name, const QString &send)
	{
		NamedItem item;
		item.attributes.insert(QStringLiteral("name"), name);
		item.children.insert(QStringLiteral("send"), send);
		return item;
	}

	VariableItem makeVariable(const QString &name, const QString &value)
	{
		VariableItem item;
		item.attributes.insert(QStringLiteral("name"), name);
		item.content = value;
		return item;
	}
} // namespace

/**
 * @brief QTest fixture covering ImportMergeUtils scenarios.
 */
class tst_ImportMergeUtils : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void namedDuplicateWithoutFlagsFails()
		{
			QList<NamedItem> dest{makeNamed(QStringLiteral("dup"), QStringLiteral("main"))};
			QList<NamedItem> src{makeNamed(QStringLiteral("dup"), QStringLiteral("incoming"))};
			int              duplicates = 0;
			QString          error;

			QVERIFY(!QMudImportMerge::mergeNamedList(dest, src, QStringLiteral("trigger"), false, false,
			                                         &duplicates, &error));
			QCOMPARE(error, QStringLiteral("Duplicate trigger label \"dup\""));
			QCOMPARE(duplicates, 0);
			QCOMPARE(dest.size(), 1);
			QCOMPARE(dest.front().children.value(QStringLiteral("send")), QStringLiteral("main"));
		}

		void namedDuplicateOverwriteReplacesEntry()
		{
			QList<NamedItem> dest{makeNamed(QStringLiteral("dup"), QStringLiteral("main"))};
			QList<NamedItem> src{makeNamed(QStringLiteral("dup"), QStringLiteral("incoming"))};
			int              duplicates = 0;
			QString          error;

			QVERIFY(QMudImportMerge::mergeNamedList(dest, src, QStringLiteral("trigger"), true, false,
			                                        &duplicates, &error));
			QVERIFY(error.isEmpty());
			QCOMPARE(duplicates, 1);
			QCOMPARE(dest.size(), 1);
			QCOMPARE(dest.front().children.value(QStringLiteral("send")), QStringLiteral("incoming"));
		}

		void namedDuplicatePasteDuplicateAppendsEntry()
		{
			QList<NamedItem> dest{makeNamed(QStringLiteral("dup"), QStringLiteral("main"))};
			QList<NamedItem> src{makeNamed(QStringLiteral("dup"), QStringLiteral("incoming"))};
			int              duplicates = 0;
			QString          error;

			QVERIFY(QMudImportMerge::mergeNamedList(dest, src, QStringLiteral("trigger"), false, true,
			                                        &duplicates, &error));
			QVERIFY(error.isEmpty());
			QCOMPARE(duplicates, 1);
			QCOMPARE(dest.size(), 2);
			QCOMPARE(dest[0].children.value(QStringLiteral("send")), QStringLiteral("main"));
			QCOMPARE(dest[1].children.value(QStringLiteral("send")), QStringLiteral("incoming"));
		}

		void variablesDefaultModeReplacesAndCountsDifference()
		{
			QList<VariableItem> dest{makeVariable(QStringLiteral("hp"), QStringLiteral("10"))};
			QList<VariableItem> src{makeVariable(QStringLiteral("hp"), QStringLiteral("25"))};
			int                 duplicates = 0;

			QMudImportMerge::mergeVariables(dest, src, false, false, &duplicates);
			QCOMPARE(duplicates, 1);
			QCOMPARE(dest.size(), 1);
			QCOMPARE(dest.front().content, QStringLiteral("25"));
		}

		void variablesOverwriteModeReplacesAndCounts()
		{
			QList<VariableItem> dest{makeVariable(QStringLiteral("hp"), QStringLiteral("10"))};
			QList<VariableItem> src{makeVariable(QStringLiteral("hp"), QStringLiteral("25"))};
			int                 duplicates = 0;

			QMudImportMerge::mergeVariables(dest, src, true, false, &duplicates);
			QCOMPARE(duplicates, 1);
			QCOMPARE(dest.size(), 1);
			QCOMPARE(dest.front().content, QStringLiteral("25"));
		}

		void variablesPasteDuplicateModeAppendsAndCounts()
		{
			QList<VariableItem> dest{makeVariable(QStringLiteral("hp"), QStringLiteral("10"))};
			QList<VariableItem> src{makeVariable(QStringLiteral("hp"), QStringLiteral("25"))};
			int                 duplicates = 0;

			QMudImportMerge::mergeVariables(dest, src, false, true, &duplicates);
			QCOMPARE(duplicates, 1);
			QCOMPARE(dest.size(), 2);
			QCOMPARE(dest[0].content, QStringLiteral("10"));
			QCOMPARE(dest[1].content, QStringLiteral("25"));
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_ImportMergeUtils)

#include "tst_ImportMergeUtils.moc"
