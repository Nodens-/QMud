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

#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QtTest/QTest>

namespace
{
	QMap<QString, QString> g_worldAttrs;
	QMap<QString, QString> g_worldMultilineAttrs;
	QMap<QString, QVariant> g_globalOptions;
	bool g_useFakeAppController{false};

	AppController *fakeAppControllerPointer()
	{
		return reinterpret_cast<AppController *>(static_cast<quintptr>(1));
	}

	WorldRuntime *fakeRuntimePointer()
	{
		return reinterpret_cast<WorldRuntime *>(static_cast<quintptr>(1));
	}

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

	void resetTestState()
	{
		g_worldAttrs.clear();
		g_worldMultilineAttrs.clear();
		g_globalOptions.clear();
		g_useFakeAppController = false;
	}
} // namespace

// NOLINTBEGIN(readability-convert-member-functions-to-static)
AppController *AppController::instance()
{
	return g_useFakeAppController ? fakeAppControllerPointer() : nullptr;
}

QVariant AppController::getGlobalOption(const QString &name) const
{
	return g_globalOptions.value(name);
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

void qmudApplyMonospaceFallback(QFont &font, const QString &preferredFamily)
{
	if (!preferredFamily.isEmpty())
		font.setFamily(preferredFamily);
}

QFont qmudPreferredMonospaceFont(const QString &preferredFamily, const int pointSize)
{
	QFont font;
	if (!preferredFamily.isEmpty())
		font.setFamily(preferredFamily);
	if (pointSize > 0)
		font.setPointSize(pointSize);
	return font;
}

QString qmudFamilyForCharset(const QString &preferredFamily, int)
{
	return preferredFamily;
}

const QMap<QString, QString> &WorldRuntime::worldAttributes() const
{
	return g_worldAttrs.isEmpty() ? emptyAttributes() : g_worldAttrs;
}

const QMap<QString, QString> &WorldRuntime::worldMultilineAttributes() const
{
	return g_worldMultilineAttrs.isEmpty() ? emptyAttributes() : g_worldMultilineAttrs;
}

int WorldRuntime::outputFontHeight() const
{
	return 0;
}

void WorldRuntime::setOutputFontMetrics(int, int) {}

void WorldRuntime::setInputFontMetrics(int, int) {}

long WorldRuntime::backgroundColour() const
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

		void defaultFontsUseGlobalPreferences()
		{
			resetTestState();
			g_useFakeAppController = true;

			g_worldAttrs.insert(QStringLiteral("use_default_output_font"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("output_font_name"), QStringLiteral("WorldSpecificOutput"));
			g_worldAttrs.insert(QStringLiteral("output_font_height"), QStringLiteral("27"));
			g_worldAttrs.insert(QStringLiteral("output_font_weight"), QStringLiteral("700"));
			g_worldAttrs.insert(QStringLiteral("output_font_charset"), QStringLiteral("1"));

			g_worldAttrs.insert(QStringLiteral("use_default_input_font"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("input_font_name"), QStringLiteral("WorldSpecificInput"));
			g_worldAttrs.insert(QStringLiteral("input_font_height"), QStringLiteral("29"));
			g_worldAttrs.insert(QStringLiteral("input_font_weight"), QStringLiteral("300"));
			g_worldAttrs.insert(QStringLiteral("input_font_italic"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("input_font_charset"), QStringLiteral("1"));

			g_globalOptions.insert(QStringLiteral("DefaultOutputFont"), QStringLiteral("DejaVu Sans Mono"));
			g_globalOptions.insert(QStringLiteral("DefaultOutputFontHeight"), 13);
			g_globalOptions.insert(QStringLiteral("DefaultOutputFontCharset"), 1);
			g_globalOptions.insert(QStringLiteral("DefaultInputFont"), QStringLiteral("DejaVu Sans Mono"));
			g_globalOptions.insert(QStringLiteral("DefaultInputFontHeight"), 15);
			g_globalOptions.insert(QStringLiteral("DefaultInputFontWeight"), 700);
			g_globalOptions.insert(QStringLiteral("DefaultInputFontItalic"), 1);
			g_globalOptions.insert(QStringLiteral("DefaultInputFontCharset"), 1);

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());

			const QFont outputFont = view.outputFont();
			QCOMPARE(outputFont.pointSize(), 13);
			QCOMPARE(outputFont.weight(), QFont::Normal);
			QVERIFY(!outputFont.italic());

			const QList<QPlainTextEdit *> inputEdits = view.findChildren<QPlainTextEdit *>();
			QVERIFY(!inputEdits.isEmpty());
			const QFont inputFont = inputEdits.first()->font();
			QCOMPARE(inputFont.pointSize(), 15);
			QCOMPARE(inputFont.weight(), QFont::Bold);
			QVERIFY(inputFont.italic());

			resetTestState();
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_WorldView_Basic)

#include "tst_WorldView_Basic.moc"
