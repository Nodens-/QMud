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
			quint64                runtimeId{0};
			quint64                runtimeLocationHandle{0};
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

				QVERIFY(!QMudImportMerge::mergeNamedList(dest, src, QStringLiteral("trigger"), false,
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

				QVERIFY(QMudImportMerge::mergeNamedList(dest, src, QStringLiteral("trigger"), true,
				                                        &duplicates, &error));
				QVERIFY(error.isEmpty());
				QCOMPARE(duplicates, 1);
				QCOMPARE(dest.size(), 1);
				QCOMPARE(dest.front().children.value(QStringLiteral("send")), QStringLiteral("incoming"));
			}

			void namedDuplicateOverwritePreservesRuntimeIdentity()
			{
				NamedItem existing             = makeNamed(QStringLiteral("dup"), QStringLiteral("main"));
				existing.runtimeId             = 17;
				existing.runtimeLocationHandle = 29;
				QList<NamedItem> dest{existing};
				QList<NamedItem> src{makeNamed(QStringLiteral("DUP"), QStringLiteral("incoming"))};
				int              duplicates = 0;
				QString          error;

				QVERIFY(QMudImportMerge::mergeNamedList(dest, src, QStringLiteral("trigger"), true,
				                                        &duplicates, &error));
				QCOMPARE(dest.front().runtimeId, quint64{17});
				QCOMPARE(dest.front().runtimeLocationHandle, quint64{29});
			}

			void variablesDefaultModeReplacesAndCountsDifference()
			{
				QList<VariableItem> dest{makeVariable(QStringLiteral("hp"), QStringLiteral("10"))};
				QList<VariableItem> src{makeVariable(QStringLiteral("hp"), QStringLiteral("25"))};
				int                 duplicates = 0;

				QMudImportMerge::mergeVariables(dest, src, false, &duplicates);
				QCOMPARE(duplicates, 1);
				QCOMPARE(dest.size(), 1);
				QCOMPARE(dest.front().content, QStringLiteral("25"));
			}

			void variablesOverwriteModeReplacesAndCounts()
			{
				QList<VariableItem> dest{makeVariable(QStringLiteral("hp"), QStringLiteral("10"))};
				QList<VariableItem> src{makeVariable(QStringLiteral("hp"), QStringLiteral("25"))};
				int                 duplicates = 0;

				QMudImportMerge::mergeVariables(dest, src, true, &duplicates);
				QCOMPARE(duplicates, 1);
				QCOMPARE(dest.size(), 1);
				QCOMPARE(dest.front().content, QStringLiteral("25"));
			}

			/** @brief Verifies import keys are canonical and source spelling remains serialization metadata. */
			void pluginVariablesNormalizeKeysAndRetainLabels()
			{
				QMudVariableMapUtils::Store dest;
				dest.insert(QStringLiteral("Health"), QStringLiteral("10"));
				const QList<QPair<QString, QString>> src{
				    {QStringLiteral("health"), QStringLiteral("25")},
				    {QStringLiteral("Mana"),   QStringLiteral("8") }
                };
				int duplicates = 0;

				QMudImportMerge::mergePluginVariables(dest, src, true, &duplicates);
				QCOMPARE(duplicates, 1);
				QCOMPARE(dest.size(), 2);
				QCOMPARE(dest.value(QStringLiteral("health")), QStringLiteral("25"));
				QVERIFY(!dest.values().contains(QStringLiteral("Health")));
				QCOMPARE(dest.value(QStringLiteral("mana")), QStringLiteral("8"));
				QCOMPARE(dest.label(QStringLiteral("health")), QStringLiteral("health"));
				QCOMPARE(dest.label(QStringLiteral("mana")), QStringLiteral("Mana"));
			}

			/** @brief Verifies unchanged values retain shared snapshots while labels and removals stay coherent. */
			void variableStorePreservesUnchangedSnapshotSharing()
			{
				QMudVariableMapUtils::Store      store;
				const QMudVariableMapUtils::Name name(QStringLiteral(" ÉNERGIE "));
				QCOMPARE(name.key(), QStringLiteral("énergie"));
				QVERIFY(store.insert(name, QStringLiteral("10")));
				const auto                       snapshot = store.values();
				const QMudVariableMapUtils::Name replacement(QStringLiteral("Énergie"));
				QVERIFY(!store.insert(replacement, QStringLiteral("10")));
				QVERIFY(&store.values().constBegin().value() == &snapshot.constBegin().value());
				QCOMPARE(store.label(name.key()), replacement.label());
				QVERIFY(store.insert(replacement, QStringLiteral("20")));
				QCOMPARE(snapshot.value(name.key()), QStringLiteral("10"));
				QCOMPARE(store.value(name.key()), QStringLiteral("20"));
				QVERIFY(store.remove(name.key()));
				QVERIFY(store.values().isEmpty());
				QVERIFY(store.label(name.key()).isEmpty());
				QVERIFY(!store.insert(QStringLiteral("  "), QStringLiteral("invalid")));
				QVERIFY(store.values().isEmpty());
			}

			/** @brief Verifies ordered traversal keeps labels and values paired across copy-on-write edits. */
			void variableStoreTraversalKeepsLabelsAndValuesAligned()
			{
				QMudVariableMapUtils::Store store;
				store.insert(QStringLiteral("Zulu"), QStringLiteral("last"));
				store.insert(QStringLiteral("Alpha"), QStringLiteral("first"));
				store.insert(QStringLiteral("Middle"), QStringLiteral("removed"));
				const auto snapshot = store;
				QVERIFY(!store.insert(QStringLiteral("ALPHA"), QStringLiteral("first")));
				QVERIFY(store.remove(QStringLiteral("middle")));
				QVERIFY(store.insert(QStringLiteral("Zulu"), QStringLiteral("updated")));

				QList<QPair<QString, QString>> visited;
				store.forEachVariable([&visited](const QString &label, const QString &value)
				                      { visited.push_back({label, value}); });
				const QList<QPair<QString, QString>> expected{
				    {QStringLiteral("ALPHA"), QStringLiteral("first")  },
				    {QStringLiteral("Zulu"),  QStringLiteral("updated")}
                };
				QCOMPARE(visited, expected);
				visited.clear();
				snapshot.forEachVariable([&visited](const QString &label, const QString &value)
				                         { visited.push_back({label, value}); });
				const QList<QPair<QString, QString>> original{
				    {QStringLiteral("Alpha"),  QStringLiteral("first")  },
				    {QStringLiteral("Middle"), QStringLiteral("removed")},
				    {QStringLiteral("Zulu"),   QStringLiteral("last")   }
                };
				QCOMPARE(visited, original);
				store.clear();
				visited.clear();
				store.forEachVariable([&visited](const QString &label, const QString &value)
				                      { visited.push_back({label, value}); });
				QVERIFY(visited.isEmpty());
			}

			/** @brief Verifies sparse writes and empty values survive late capture without publishing cached reads. */
			void variableWorkingViewPreservesSparseKnowledgeAndReadIsolation()
			{
				using View = QMudVariableMapUtils::WorkingView;
				View    view;
				QString value;
				bool    resolved = true;
				QVERIFY(!view.find(QStringLiteral("unknown"), value, resolved));
				QVERIFY(!resolved);
				QVERIFY(!view.count());
				QVERIFY(!view.values());
				view.remember(QStringLiteral("observed"), true, QStringLiteral("old observation"));
				view.remember(QStringLiteral("missing"), false, QString());
				QMudVariableMapUtils::VariableMap readTarget{
				    {QStringLiteral("observed"), QStringLiteral("new target")}
                };
				view.project(readTarget);
				QCOMPARE(readTarget.value(QStringLiteral("observed")), QStringLiteral("new target"));
				view.write(QStringLiteral("empty"), QString(), false);
				view.write(QStringLiteral("removed"), QString(), true);
				view.write(QStringLiteral("empty"), QString(), false);
				QVERIFY(view.find(QStringLiteral("empty"), value, resolved));
				QVERIFY(resolved);
				QVERIFY(value.isEmpty());
				QVERIFY(!view.find(QStringLiteral("removed"), value, resolved));
				QVERIFY(resolved);
				QVERIFY(!view.count());
				QMudVariableMapUtils::VariableMap projected{
				    {QStringLiteral("observed"), QStringLiteral("new target")       },
				    {QStringLiteral("missing"),  QStringLiteral("present in target")},
				    {QStringLiteral("removed"),  QStringLiteral("remove me")        }
                };
				view.project(projected);
				QCOMPARE(projected.value(QStringLiteral("observed")), QStringLiteral("new target"));
				QVERIFY(projected.contains(QStringLiteral("missing")));
				QVERIFY(projected.contains(QStringLiteral("empty")));
				QVERIFY(!projected.contains(QStringLiteral("removed")));
				QCOMPARE(view.statistics().writtenEntries, qsizetype{2});
				QCOMPARE(view.statistics().materializations, quint64{0});
				const QMudVariableMapUtils::VariableMap captured{
				    {QStringLiteral("removed"),  QStringLiteral("captured")            },
				    {QStringLiteral("observed"), QStringLiteral("captured observation")}
                };
				view.capture(captured);
				QCOMPARE(view.count(), std::optional<qsizetype>{2});
				QVERIFY(view.find(QStringLiteral("observed"), value, resolved));
				QCOMPARE(value, QStringLiteral("captured observation"));
				QVERIFY(view.find(QStringLiteral("empty"), value, resolved));
				QVERIFY(value.isEmpty());
				QVERIFY(!view.find(QStringLiteral("removed"), value, resolved));
				QVERIFY(captured.contains(QStringLiteral("removed")));
				view.makeUnavailable();
				QCOMPARE(view.source(), View::Source::Unavailable);
				QVERIFY(!view.find(QStringLiteral("empty"), value, resolved));
				QVERIFY(resolved);
				QVERIFY(!view.count());
				QVERIFY(!view.values());
				QVERIFY(!view.hasWrites());
			}

			/** @brief Verifies repeated nested-style publications reuse maps and preserve every issued value. */
			void variableWorkingViewPublicationWorkStaysLinear()
			{
				QMudVariableMapUtils::VariableMap base;
				for (int index = 0; index < 2048; ++index)
					base.insert(QString::number(index), QStringLiteral("base"));
				QMudVariableMapUtils::WorkingView view;
				view.capture(base);
				QString value;
				bool    resolved = false;
				for (int index = 0; index < 1024; ++index)
				{
					QVERIFY(!view.find(QStringLiteral("missing-%1").arg(index), value, resolved));
					QVERIFY(resolved);
				}
				QCOMPARE(view.statistics().materializations, quint64{0});
				QCOMPARE(view.statistics().projectedWrites, quint64{0});
				QVector<QMudVariableMapUtils::VariableMap> issued;
				for (int index = 0; index < 64; ++index)
				{
					const QString expected = QString::number(index);
					view.write(QStringLiteral("0"), expected, false);
					QMudVariableMapUtils::VariableMap snapshot;
					view.project(snapshot);
					issued.push_back(snapshot);
					QCOMPARE(view.statistics().materializations, quint64{1});
					QCOMPARE(view.statistics().projectedWrites, static_cast<quint64>(index + 1));
					const auto *first = view.values();
					QVERIFY(first);
					for (int repeat = 0; repeat < 4; ++repeat)
					{
						QMudVariableMapUtils::VariableMap repeated;
						view.project(repeated);
						QVERIFY(&first->constBegin().value() == &repeated.constBegin().value());
						QCOMPARE(repeated.value(QStringLiteral("0")), expected);
					}
					QCOMPARE(view.statistics().projectedWrites, static_cast<quint64>(index + 1));
				}
				QCOMPARE(view.statistics().writtenEntries, qsizetype{1});
				QCOMPARE(view.count(), std::optional<qsizetype>{2048});
				for (int index = 0; index < issued.size(); ++index)
					QCOMPARE(issued.at(index).value(QStringLiteral("0")), QString::number(index));
				QCOMPARE(base.value(QStringLiteral("0")), QStringLiteral("base"));
				const auto beforeEqualWrite = *view.values();
				view.write(QStringLiteral("0"), QStringLiteral("63"), false);
				QVERIFY(&beforeEqualWrite.constBegin().value() == &view.values()->constBegin().value());
			}

			/** @brief Verifies additions, deletions and recreation maintain counts across materialization. */
			void variableWorkingViewTracksExistenceTransitions()
			{
				QMudVariableMapUtils::WorkingView view;
				view.capture({});
				QCOMPARE(view.count(), std::optional<qsizetype>{0});
				view.write(QStringLiteral("x"), QString(), false);
				QCOMPARE(view.count(), std::optional<qsizetype>{1});
				const auto first = *view.values();
				view.write(QStringLiteral("x"), QStringLiteral("changed"), false);
				QCOMPARE(view.count(), std::optional<qsizetype>{1});
				view.write(QStringLiteral("x"), QString(), true);
				view.write(QStringLiteral("x"), QString(), true);
				QCOMPARE(view.count(), std::optional<qsizetype>{0});
				QVERIFY(view.values()->isEmpty());
				view.write(QStringLiteral("x"), QStringLiteral("recreated"), false);
				QCOMPARE(view.count(), std::optional<qsizetype>{1});
				QCOMPARE(view.values()->value(QStringLiteral("x")), QStringLiteral("recreated"));
				QVERIFY(first.contains(QStringLiteral("x")));
				QVERIFY(first.value(QStringLiteral("x")).isEmpty());
				QCOMPARE(view.statistics().writtenEntries, qsizetype{1});
				QCOMPARE(view.statistics().materializations, quint64{1});
			}

			// NOLINTEND(readability-convert-member-functions-to-static)
	};

} // namespace

QTEST_APPLESS_MAIN(tst_ImportMergeUtils)

#include "tst_ImportMergeUtils.moc"
