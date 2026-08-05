/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MainFrameMdiUtils.cpp
 * Role: QTest coverage for MainFrame MDI layout, fallback, and restore helper behavior.
 */

#include "MainFrameMdiUtils.h"

#include <QCoreApplication>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QVariant>
#include <QWidget>
#include <QtTest/QTest>

namespace
{
	QMdiSubWindow *addVisibleSubWindow(QMdiArea &mdiArea)
	{
		auto *window = mdiArea.addSubWindow(new QWidget);
		window->show();
		return window;
	}

	/**
	 * @brief QTest fixture covering MainFrame MDI helper scenarios.
	 */
	class tst_MainFrameMdiUtils : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			void tileSubWindowsSurvivesWindowActivation_data()
			{
				QTest::addColumn<QMudMainFrameMdiUtils::TileDirection>("direction");

				QTest::newRow("horizontal") << QMudMainFrameMdiUtils::TileDirection::Horizontal;
				QTest::newRow("vertical") << QMudMainFrameMdiUtils::TileDirection::Vertical;
			}

			void tileSubWindowsSurvivesWindowActivation()
			{
				QFETCH(QMudMainFrameMdiUtils::TileDirection, direction);

				QMdiArea mdiArea;
				mdiArea.resize(643, 487);
				mdiArea.show();

				QMdiSubWindow *first     = addVisibleSubWindow(mdiArea);
				QMdiSubWindow *second    = addVisibleSubWindow(mdiArea);
				QMdiSubWindow *third     = addVisibleSubWindow(mdiArea);
				QMdiSubWindow *minimized = addVisibleSubWindow(mdiArea);
				minimized->showMinimized();
				mdiArea.setActiveSubWindow(first);
				first->showMaximized();
				QCoreApplication::processEvents();
				QVERIFY(first->isMaximized());

				QMudMainFrameMdiUtils::tileSubWindows(&mdiArea, direction);
				QCoreApplication::processEvents();

				QVERIFY(!first->isMaximized());
				QVERIFY(!second->isMaximized());
				QVERIFY(!third->isMaximized());
				QVERIFY(minimized->isMinimized());

				const QRect                  area = mdiArea.viewport()->rect();
				const QList<QMdiSubWindow *> tiled{first, second, third};
				if (direction == QMudMainFrameMdiUtils::TileDirection::Horizontal)
				{
					QCOMPARE(first->geometry().top(), area.top());
					QCOMPARE(third->geometry().bottom(), area.bottom());
					for (qsizetype index = 0; index < tiled.size(); ++index)
					{
						QCOMPARE(tiled.at(index)->geometry().left(), area.left());
						QCOMPARE(tiled.at(index)->geometry().width(), area.width());
						if (index > 0)
							QCOMPARE(tiled.at(index)->geometry().top(),
							         tiled.at(index - 1)->geometry().bottom() + 1);
					}
				}
				else
				{
					QCOMPARE(first->geometry().left(), area.left());
					QCOMPARE(third->geometry().right(), area.right());
					for (qsizetype index = 0; index < tiled.size(); ++index)
					{
						QCOMPARE(tiled.at(index)->geometry().top(), area.top());
						QCOMPARE(tiled.at(index)->geometry().height(), area.height());
						if (index > 0)
							QCOMPARE(tiled.at(index)->geometry().left(),
							         tiled.at(index - 1)->geometry().right() + 1);
					}
				}

				QList<QRect> tiledGeometry;
				tiledGeometry.reserve(tiled.size());
				for (const QMdiSubWindow *window : tiled)
					tiledGeometry.push_back(window->geometry());

				mdiArea.setActiveSubWindow(third);
				QCoreApplication::processEvents();

				QCOMPARE(mdiArea.activeSubWindow(), third);
				for (qsizetype index = 0; index < tiled.size(); ++index)
				{
					QVERIFY(!tiled.at(index)->isMaximized());
					QCOMPARE(tiled.at(index)->geometry(), tiledGeometry.at(index));
				}
			}

			void resolveCurrentOrLastPrefersCurrentActive()
			{
				QMdiSubWindow                current;
				QMdiSubWindow                last;
				const QList<QMdiSubWindow *> windows{&current, &last};
				QCOMPARE(QMudMainFrameMdiUtils::resolveCurrentOrLastActiveSubWindow(&current, &last, windows),
				         &current);
			}

			void resolveCurrentOrLastFallsBackToLastActive()
			{
				QMdiSubWindow                last;
				const QList<QMdiSubWindow *> windows{&last};
				QCOMPARE(QMudMainFrameMdiUtils::resolveCurrentOrLastActiveSubWindow(nullptr, &last, windows),
				         &last);
			}

			void resolveCurrentOrLastReturnsNullWhenLastMissing()
			{
				QMdiSubWindow                last;
				const QList<QMdiSubWindow *> windows;
				QCOMPARE(QMudMainFrameMdiUtils::resolveCurrentOrLastActiveSubWindow(nullptr, &last, windows),
				         nullptr);
			}

			void resolveBackgroundAddRestoreTargetUsesLastWhenCurrentIsNull()
			{
				QMdiSubWindow                last;
				QMdiSubWindow                added;
				const QList<QMdiSubWindow *> windows{&last};
				QCOMPARE(
				    QMudMainFrameMdiUtils::resolveBackgroundAddRestoreTarget(nullptr, &last, windows, &added),
				    &last);
			}

			void resolveBackgroundAddRestoreTargetIgnoresAddedWindow()
			{
				QMdiSubWindow                added;
				const QList<QMdiSubWindow *> windows{&added};
				QCOMPARE(
				    QMudMainFrameMdiUtils::resolveBackgroundAddRestoreTarget(&added, &added, windows, &added),
				    nullptr);
			}

			void windowMatchesRuntimeIdentityPrefersRuntimeToken()
			{
				QMdiSubWindow notepad;
				notepad.setProperty("worldRuntimeToken", QVariant::fromValue<qulonglong>(42));
				notepad.setProperty("worldId", QStringLiteral("other-world"));

				QVERIFY(QMudMainFrameMdiUtils::windowMatchesRuntimeIdentity(
				    &notepad, 42, QStringLiteral("world-a"), false));
			}

			void windowMatchesRuntimeIdentityRejectsMismatchedRuntimeToken()
			{
				QMdiSubWindow notepad;
				notepad.setProperty("worldRuntimeToken", QVariant::fromValue<qulonglong>(7));

				QVERIFY(!QMudMainFrameMdiUtils::windowMatchesRuntimeIdentity(
				    &notepad, 42, QStringLiteral("world-a"), true));
			}

			void windowMatchesRuntimeIdentityUsesWorldIdWhenTokenIsAbsent()
			{
				QMdiSubWindow notepad;
				notepad.setProperty("worldId", QStringLiteral("WORLD-A"));

				QVERIFY(QMudMainFrameMdiUtils::windowMatchesRuntimeIdentity(
				    &notepad, 42, QStringLiteral("world-a"), false));
			}

			void windowMatchesRuntimeIdentityKeepsUnownedOutOfStrictMatches()
			{
				QMdiSubWindow notepad;

				QVERIFY(!QMudMainFrameMdiUtils::windowMatchesRuntimeIdentity(
				    &notepad, 42, QStringLiteral("world-a"), false));
				QVERIFY(QMudMainFrameMdiUtils::windowMatchesRuntimeIdentity(&notepad, 42,
				                                                            QStringLiteral("world-a"), true));
			}

			void windowMatchesRuntimeIdentityWithoutOwnerOnlyAcceptsUnownedWindows()
			{
				QMdiSubWindow unowned;
				QVERIFY(QMudMainFrameMdiUtils::windowMatchesRuntimeIdentity(&unowned, 0, QString(), false));

				QMdiSubWindow relatedByToken;
				relatedByToken.setProperty("worldRuntimeToken", QVariant::fromValue<qulonglong>(42));
				QVERIFY(!QMudMainFrameMdiUtils::windowMatchesRuntimeIdentity(&relatedByToken, 0, QString(),
				                                                             false));

				QMdiSubWindow relatedByWorldId;
				relatedByWorldId.setProperty("worldId", QStringLiteral("world-a"));
				QVERIFY(!QMudMainFrameMdiUtils::windowMatchesRuntimeIdentity(&relatedByWorldId, 0, QString(),
				                                                             false));
			}

			void firstWindowMatchingRuntimeIdentityUsesCreationOrder()
			{
				QMdiSubWindow unrelated;
				unrelated.setProperty("worldRuntimeToken", QVariant::fromValue<qulonglong>(7));
				QMdiSubWindow first;
				first.setProperty("worldRuntimeToken", QVariant::fromValue<qulonglong>(42));
				QMdiSubWindow second;
				second.setProperty("worldRuntimeToken", QVariant::fromValue<qulonglong>(42));

				const QList<QMdiSubWindow *> windows{&unrelated, &first, &second};
				QCOMPARE(QMudMainFrameMdiUtils::firstWindowMatchingRuntimeIdentity(
				             windows, 42, QStringLiteral("world-a"), false),
				         &first);
			}

			void prepareOpenWorldStateBeforeChildCloseAllowsCloseWithoutController()
			{
				QString error = QStringLiteral("stale");
				QVERIFY(QMudMainFrameMdiUtils::prepareOpenWorldStateBeforeChildClose({}, &error));
				QVERIFY(error.isEmpty());
			}

			void prepareOpenWorldStateBeforeChildCloseRunsSaveBeforeAllowingClose()
			{
				QString    error   = QStringLiteral("stale");
				bool       saved   = false;
				const bool proceed = QMudMainFrameMdiUtils::prepareOpenWorldStateBeforeChildClose(
				    [&saved](QString *errorMessage)
				    {
					    saved = true;
					    if (errorMessage)
						    errorMessage->clear();
					    return true;
				    },
				    &error);

				QVERIFY(proceed);
				QVERIFY(saved);
				QVERIFY(error.isEmpty());
			}

			void prepareOpenWorldStateBeforeChildCloseStopsCloseOnSaveFailure()
			{
				QString    error;
				bool       saved   = false;
				const bool proceed = QMudMainFrameMdiUtils::prepareOpenWorldStateBeforeChildClose(
				    [&saved](QString *errorMessage)
				    {
					    saved = true;
					    if (errorMessage)
						    *errorMessage = QStringLiteral("session write failed");
					    return false;
				    },
				    &error);

				QVERIFY(!proceed);
				QVERIFY(saved);
				QCOMPARE(error, QStringLiteral("session write failed"));
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};
} // namespace

QTEST_MAIN(tst_MainFrameMdiUtils)

#if __has_include("tst_MainFrameMdiUtils.moc")
#include "tst_MainFrameMdiUtils.moc"
#endif
