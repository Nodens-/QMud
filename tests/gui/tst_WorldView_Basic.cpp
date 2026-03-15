/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldView_Basic.cpp
 * Role: QTest coverage for WorldView Basic behavior.
 */

#include "AcceleratorUtils.h"
#include "AppController.h"
#include "WorldView.h"

#include <QSignalSpy>
#include <QtTest/QTest>

namespace
{
	const QMap<QString, QString> &emptyAttributes()
	{
		static const QMap<QString, QString> attrs;
		return attrs;
	}

	const QVector<WorldRuntime::LineEntry> &lineStorage()
	{
		static const QVector<WorldRuntime::LineEntry> lines;
		return lines;
	}

	const QList<WorldRuntime::Macro> &macroStorage()
	{
		static const QList<WorldRuntime::Macro> macros;
		return macros;
	}

	const QList<WorldRuntime::Keypad> &keypadStorage()
	{
		static const QList<WorldRuntime::Keypad> entries;
		return entries;
	}
} // namespace

// NOLINTBEGIN(readability-convert-member-functions-to-static)
AppController *AppController::instance()
{
	return nullptr;
}

QVariant AppController::getGlobalOption(const QString &) const
{
	return {};
}

void AppController::onCommandTriggered(const QString &) {}

quint16 AcceleratorUtils::qtKeyToVirtualKey(Qt::Key, bool)
{
	return 0;
}

QString AcceleratorUtils::acceleratorToString(quint32, quint16)
{
	return QString();
}

const QMap<QString, QString> &WorldRuntime::worldAttributes() const
{
	return emptyAttributes();
}

const QMap<QString, QString> &WorldRuntime::worldMultilineAttributes() const
{
	return emptyAttributes();
}

int WorldRuntime::outputFontHeight() const
{
	return 0;
}

void WorldRuntime::notifyDrawOutputWindow(int, int) {}

QImage WorldRuntime::backgroundImage() const
{
	return {};
}

int WorldRuntime::backgroundImageMode() const
{
	return 0;
}

void WorldRuntime::layoutMiniWindows(const QSize &, const QSize &, bool) {}

QVector<MiniWindow *> WorldRuntime::sortedMiniWindows()
{
	return {};
}

QImage WorldRuntime::foregroundImage() const
{
	return {};
}

int WorldRuntime::foregroundImageMode() const
{
	return 0;
}

const QVector<WorldRuntime::LineEntry> &WorldRuntime::lines() const
{
	return lineStorage();
}

void WorldRuntime::installPendingPlugins() {}

void WorldRuntime::notifyWorldOutputResized() {}

void WorldRuntime::firePluginCommandChanged() {}

void WorldRuntime::firePluginTabComplete(QString &) {}

bool WorldRuntime::isConnected() const
{
	return false;
}

bool WorldRuntime::callPluginHotspotFunction(const QString &, const QString &, long, const QString &)
{
	return false;
}

bool WorldRuntime::callWorldHotspotFunction(const QString &, long, const QString &)
{
	return false;
}

void WorldRuntime::notifyMiniWindowMouseMoved(int, int, const QString &) {}

MiniWindow *WorldRuntime::miniWindow(const QString &)
{
	return nullptr;
}

void WorldRuntime::notifyOutputSelectionChanged() {}

void WorldRuntime::setOutputFrozen(bool) {}

const QList<WorldRuntime::Macro> &WorldRuntime::macros() const
{
	return macroStorage();
}

void WorldRuntime::setCurrentActionSource(unsigned short) {}

int WorldRuntime::sendCommand(const QString &, bool, bool, bool, bool, bool) const
{
	return 0;
}

int WorldRuntime::acceleratorCommandForKey(qint64) const
{
	return 0;
}

bool WorldRuntime::executeAcceleratorCommand(int, const QString &)
{
	return false;
}

const QList<WorldRuntime::Keypad> &WorldRuntime::keypadEntries() const
{
	return keypadStorage();
}

QDateTime WorldRuntime::worldStartTime() const
{
	return {};
}

QString WorldRuntime::formatTime(const QDateTime &, const QString &format, bool) const
{
	return format;
}

void WorldRuntime::addLine(const QString &, int, bool, const QDateTime &) {}

void WorldRuntime::addLine(const QString &, int, const QVector<StyleSpan> &, bool, const QDateTime &) {}
// NOLINTEND(readability-convert-member-functions-to-static)

/**
 * @brief QTest fixture covering WorldView Basic scenarios.
 */
class tst_WorldView_Basic : public QObject
{
		Q_OBJECT

	// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void parseColorNamedAndHex()
		{
			QCOMPARE(WorldView::parseColor(QStringLiteral("red")), QColor(Qt::red));
			QCOMPARE(WorldView::parseColor(QStringLiteral("#112233")), QColor(QStringLiteral("#112233")));
		}

		void parseColorFallbackOnInvalid()
		{
			const QColor invalid = WorldView::parseColor(QStringLiteral("not-a-colour"));
			QVERIFY(!invalid.isValid());
		}

		void mapFontWeightClampsToQtEnum()
		{
			QCOMPARE(WorldView::mapFontWeight(100), QFont::Normal);
			QCOMPARE(WorldView::mapFontWeight(499), QFont::Normal);
			QCOMPARE(WorldView::mapFontWeight(500), QFont::Medium);
			QCOMPARE(WorldView::mapFontWeight(600), QFont::DemiBold);
			QCOMPARE(WorldView::mapFontWeight(700), QFont::Bold);
			QCOMPARE(WorldView::mapFontWeight(-10), QFont::Normal);
			QCOMPARE(WorldView::mapFontWeight(1000), QFont::Bold);
		}

		void constructAndBasicInputOutputSmoke()
		{
			WorldView view;
			view.setInputText(QStringLiteral("north"), true);
			QCOMPARE(view.inputText(), QStringLiteral("north"));

			view.appendOutputText(QStringLiteral("line-one"), true);
			const QStringList lines = view.outputLines();
			QVERIFY(!lines.isEmpty());
			QVERIFY(lines.contains(QStringLiteral("line-one")));
		}

		void freezeStateSignalAndBufferedFlush()
		{
			WorldView  view;
			QSignalSpy freezeSpy(&view, &WorldView::freezeStateChanged);

			view.setFrozen(true);
			QCOMPARE(freezeSpy.count(), 1);
			QVERIFY(view.isFrozen());

			view.appendOutputText(QStringLiteral("frozen-line"), true);
			QVERIFY(!view.outputLines().contains(QStringLiteral("frozen-line")));

			view.setFrozen(false);
			QCOMPARE(freezeSpy.count(), 2);
			QVERIFY(!view.isFrozen());
			QVERIFY(view.outputLines().contains(QStringLiteral("frozen-line")));
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_WorldView_Basic)

#include "tst_WorldView_Basic.moc"
