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

// ReSharper disable once CppUnusedIncludeDirective
#include <QApplication>
#include <QComboBox>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialog>
#include <QElapsedTimer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QEvent>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFragment>
#include <QTimer>
#include <QtTest/QTest>

#include <functional>
#include <memory>

namespace
{
	QMap<QString, QString> g_worldAttrs;
	QMap<QString, QString> g_worldMultilineAttrs;
	QMap<QString, QVariant> g_globalOptions;
	QVector<WorldRuntime::LineEntry> g_runtimeLines;
	unsigned short g_currentActionSource{WorldRuntime::eUnknownActionSource};
	bool g_useFakeAppController{false};

	AppController* fakeAppControllerPointer()
	{
		return reinterpret_cast<AppController*>(static_cast<quintptr>(1));
	}

	WorldRuntime* fakeRuntimePointer()
	{
		return reinterpret_cast<WorldRuntime*>(static_cast<quintptr>(1));
	}

	const QMap<QString, QString>& emptyAttributes()
	{
		static const QMap<QString, QString> attrs;
		return attrs;
	}

	const QVector<WorldRuntime::LineEntry>& lineStorage()
	{
		return g_runtimeLines;
	}

	const QList<WorldRuntime::Macro>& macroStorage()
	{
		static const QList<WorldRuntime::Macro> macros;
		return macros;
	}

	const QList<WorldRuntime::Keypad>& keypadStorage()
	{
		static const QList<WorldRuntime::Keypad> entries;
		return entries;
	}

	void resetTestState()
	{
		g_worldAttrs.clear();
		g_worldMultilineAttrs.clear();
		g_globalOptions.clear();
		g_runtimeLines.clear();
		g_currentActionSource = WorldRuntime::eUnknownActionSource;
		g_useFakeAppController = false;
	}

	QPushButton* findButtonByText(const QObject& root, const QString& text)
	{
		const auto buttons = root.findChildren<QPushButton*>();
		for (QPushButton* button : buttons)
		{
			if (button && button->text() == text)
				return button;
		}
		return nullptr;
	}

	QRadioButton* findRadioButtonByText(const QObject& root, const QString& text)
	{
		const auto buttons = root.findChildren<QRadioButton*>();
		for (QRadioButton* button : buttons)
		{
			if (button && button->text() == text)
				return button;
		}
		return nullptr;
	}

	void scheduleDialogInteraction(const std::function<bool(const QDialog*)>& matcher,
	                               const std::function<void(const QDialog*)>& action)
	{
		auto runner = std::make_shared<std::function<void(int)>>();
		*runner = [runner, matcher, action](int retriesLeft)
		{
			for (QWidget* widget : QApplication::topLevelWidgets())
			{
				auto* dialog = qobject_cast<QDialog*>(widget);
				if (!dialog || !matcher(dialog))
					continue;
				action(dialog);
				return;
			}
			if (retriesLeft > 0)
				QTimer::singleShot(10, qApp, [runner, retriesLeft] { (*runner)(retriesLeft - 1); });
		};
		QTimer::singleShot(0, qApp, [runner] { (*runner)(300); });
	}

	QTextBrowser* findVisibleOutputBrowser(const WorldView& view)
	{
		const auto browsers = view.findChildren<QTextBrowser*>();
		for (QTextBrowser* browser : browsers)
		{
			if (browser && browser->isVisible() && browser->viewport())
				return browser;
		}
		return nullptr;
	}

	QPoint findAnchorPoint(const QTextBrowser& browser, const QString& href)
	{
		if (!browser.viewport())
			return {-1, -1};
		const QRect area = browser.viewport()->rect();
		for (int y = area.top(); y <= area.bottom(); ++y)
		{
			for (int x = area.left(); x <= area.right(); ++x)
			{
				if (browser.anchorAt(QPoint(x, y)).trimmed() == href)
					return {x, y};
			}
		}
		return {-1, -1};
	}

	QPoint findNonAnchorPoint(const QTextBrowser& browser)
	{
		if (!browser.viewport())
			return {-1, -1};
		const QRect area = browser.viewport()->rect();
		for (int y = area.top(); y <= area.bottom(); ++y)
		{
			for (int x = area.left(); x <= area.right(); ++x)
			{
				if (browser.anchorAt(QPoint(x, y)).trimmed().isEmpty())
					return {x, y};
			}
		}
		return {-1, -1};
	}

	struct AnchorFormatSnapshot
	{
		bool found{false};
		int blockNumber{-1};
		QTextCharFormat format;
	};

	AnchorFormatSnapshot findAnchorFormatSnapshot(const QTextDocument& document, const QString& href)
	{
		for (QTextBlock block = document.begin(); block.isValid(); block = block.next())
		{
			for (QTextBlock::Iterator iterator = block.begin(); !iterator.atEnd(); ++iterator)
			{
				const QTextFragment fragment = iterator.fragment();
				if (!fragment.isValid())
					continue;
				const QTextCharFormat format = fragment.charFormat();
				if (!format.isAnchor() || format.anchorHref() != href)
					continue;
				return AnchorFormatSnapshot{true, block.blockNumber(), format};
			}
		}
		return {};
	}
} // namespace

// NOLINTBEGIN(readability-convert-member-functions-to-static)
AppController* AppController::instance()
{
	return g_useFakeAppController ? fakeAppControllerPointer() : nullptr;
}

QVariant AppController::getGlobalOption(const QString& name) const
{
	return g_globalOptions.value(name);
}

void AppController::onCommandTriggered(const QString&)
{
}

quint16 AcceleratorUtils::qtKeyToVirtualKey(Qt::Key, bool)
{
	return 0;
}

QString AcceleratorUtils::acceleratorToString(quint32, quint16)
{
	return {};
}

void qmudApplyMonospaceFallback(QFont& font, const QString& preferredFamily)
{
	if (!preferredFamily.isEmpty())
		font.setFamily(preferredFamily);
}

QFont qmudPreferredMonospaceFont(const QString& preferredFamily, const int pointSize)
{
	QFont font;
	if (!preferredFamily.isEmpty())
		font.setFamily(preferredFamily);
	if (pointSize > 0)
		font.setPointSize(pointSize);
	return font;
}

QString qmudFamilyForCharset(const QString& preferredFamily, int)
{
	return preferredFamily;
}

const QMap<QString, QString>& WorldRuntime::worldAttributes() const
{
	return g_worldAttrs.isEmpty() ? emptyAttributes() : g_worldAttrs;
}

const QMap<QString, QString>& WorldRuntime::worldMultilineAttributes() const
{
	return g_worldMultilineAttrs.isEmpty() ? emptyAttributes() : g_worldMultilineAttrs;
}

int WorldRuntime::outputFontHeight() const
{
	return 0;
}

void WorldRuntime::setOutputFontMetrics(int, int)
{
}

void WorldRuntime::setInputFontMetrics(int, int)
{
}

long WorldRuntime::backgroundColour() const
{
	return 0;
}

long WorldRuntime::customColourText(int) const
{
	return 0;
}

long WorldRuntime::customColourBackground(int) const
{
	return 0;
}

void WorldRuntime::notifyDrawOutputWindow(int, int)
{
}

QImage WorldRuntime::backgroundImage() const
{
	return {};
}

int WorldRuntime::backgroundImageMode() const
{
	return 0;
}

void WorldRuntime::layoutMiniWindows(const QSize&, const QSize&, bool)
{
}

QVector<MiniWindow*> WorldRuntime::sortedMiniWindows()
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

const QVector<WorldRuntime::LineEntry>& WorldRuntime::lines() const
{
	return lineStorage();
}

void WorldRuntime::installPendingPlugins()
{
}

void WorldRuntime::notifyWorldOutputResized()
{
}

void WorldRuntime::firePluginCommandChanged()
{
}

void WorldRuntime::firePluginTabComplete(QString&)
{
}

bool WorldRuntime::isConnected() const
{
	return false;
}

bool WorldRuntime::callPluginHotspotFunction(const QString&, const QString&, long, const QString&)
{
	return false;
}

bool WorldRuntime::callWorldHotspotFunction(const QString&, long, const QString&)
{
	return false;
}

void WorldRuntime::notifyMiniWindowMouseMoved(int, int, const QString&)
{
}

MiniWindow* WorldRuntime::miniWindow(const QString&)
{
	return nullptr;
}

void WorldRuntime::notifyOutputSelectionChanged()
{
}

void WorldRuntime::setOutputFrozen(bool)
{
}

const QList<WorldRuntime::Macro>& WorldRuntime::macros() const
{
	return macroStorage();
}

void WorldRuntime::setCurrentActionSource(unsigned short source)
{
	g_currentActionSource = source;
}

unsigned short WorldRuntime::currentActionSource() const
{
	return g_currentActionSource;
}

int WorldRuntime::sendCommand(const QString&, bool, bool, bool, bool, bool) const
{
	return 0;
}

int WorldRuntime::acceleratorCommandForKey(qint64) const
{
	return 0;
}

bool WorldRuntime::executeAcceleratorCommand(int, const QString&)
{
	return false;
}

const QList<WorldRuntime::Keypad>& WorldRuntime::keypadEntries() const
{
	return keypadStorage();
}

QDateTime WorldRuntime::worldStartTime() const
{
	return {};
}

QString WorldRuntime::formatTime(const QDateTime&, const QString& format, bool) const
{
	return format;
}

void WorldRuntime::addLine(const QString& text, int flags, bool hardReturn, const QDateTime& time)
{
	LineEntry entry;
	entry.text = text;
	entry.flags = flags;
	entry.hardReturn = hardReturn;
	entry.time = time;
	entry.lineNumber = g_runtimeLines.isEmpty() ? 1 : (g_runtimeLines.last().lineNumber + 1);
	g_runtimeLines.push_back(entry);
}

void WorldRuntime::addLine(const QString& text, int flags, const QVector<StyleSpan>& spans, bool hardReturn,
                           const QDateTime& time)
{
	LineEntry entry;
	entry.text = text;
	entry.flags = flags;
	entry.hardReturn = hardReturn;
	entry.spans = spans;
	entry.time = time;
	entry.lineNumber = g_runtimeLines.isEmpty() ? 1 : (g_runtimeLines.last().lineNumber + 1);
	g_runtimeLines.push_back(entry);
}

void WorldRuntime::finalizePendingInputLineHardReturn()
{
	if (g_runtimeLines.isEmpty())
		return;
	LineEntry& last = g_runtimeLines.last();
	if ((last.flags & LineInput) == 0)
		return;
	last.hardReturn = true;
}

void WorldRuntime::clearLastLineHardReturn()
{
	if (g_runtimeLines.isEmpty())
		return;
	g_runtimeLines.last().hardReturn = false;
}

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

	void applyRuntimeSettingsPreservesSyntheticInputBreaks()
	{
		resetTestState();
		g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("1"));
		g_worldAttrs.insert(QStringLiteral("keep_commands_on_same_line"), QStringLiteral("1"));
		g_currentActionSource = WorldRuntime::eUserTyping;

		WorldView view;
		view.setRuntimeObserver(fakeRuntimePointer());
		view.applyRuntimeSettings();

		view.echoInputText(QStringLiteral("look\r\n"));
		view.appendOutputText(QStringLiteral("The room is quiet."), true);

		const QStringList before = view.outputLines();
		const int lookAt = before.indexOf(QStringLiteral("look"));
		QVERIFY(lookAt >= 0);
		QCOMPARE(before.value(lookAt + 1), QStringLiteral("The room is quiet."));

		view.applyRuntimeSettings();

		const QStringList after = view.outputLines();
		const int lookAfter = after.indexOf(QStringLiteral("look"));
		const int mergedLine = after.indexOf(QStringLiteral("lookThe room is quiet."));
		QVERIFY(lookAfter >= 0);
		QCOMPARE(after.value(lookAfter + 1), QStringLiteral("The room is quiet."));
		QCOMPARE(mergedLine, -1);

		resetTestState();
	}

	void keepCommandsOnSameLineConsumesPreviousLineBreak()
	{
		resetTestState();
		g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("1"));
		g_worldAttrs.insert(QStringLiteral("keep_commands_on_same_line"), QStringLiteral("1"));
		g_currentActionSource = WorldRuntime::eUserTyping;

		WorldView view;
		view.setRuntimeObserver(fakeRuntimePointer());
		view.applyRuntimeSettings();

		view.appendOutputText(QStringLiteral("Ready."), true);
		view.echoInputText(QStringLiteral("look\r\n"));

		const QStringList lines = view.outputLines();
		const int mergedLine = lines.indexOf(QStringLiteral("Ready.look"));
		QVERIFY(mergedLine >= 0);
		QVERIFY(g_runtimeLines.size() >= 2);
		QVERIFY(!g_runtimeLines.at(0).hardReturn);
		QVERIFY(g_runtimeLines.at(1).hardReturn);

		resetTestState();
	}

	void freezeStateSignalAndBufferedFlush()
	{
		WorldView view;
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

		const QList<QPlainTextEdit*> inputEdits = view.findChildren<QPlainTextEdit*>();
		QVERIFY(!inputEdits.isEmpty());
		const QFont inputFont = inputEdits.first()->font();
		QCOMPARE(inputFont.pointSize(), 15);
		QCOMPARE(inputFont.weight(), QFont::Bold);
		QVERIFY(inputFont.italic());

		resetTestState();
	}

	void tabCompletionCyclesUpwardAndResetsOnNonTabKey()
	{
		resetTestState();
		g_worldAttrs.insert(QStringLiteral("tab_completion_space"), QStringLiteral("0"));

		WorldRuntime::LineEntry older;
		older.text = QStringLiteral("stamina");
		older.flags = WorldRuntime::LineOutput;
		older.hardReturn = true;
		g_runtimeLines.push_back(older);

		WorldRuntime::LineEntry middle;
		middle.text = QStringLiteral("starlight");
		middle.flags = WorldRuntime::LineOutput;
		middle.hardReturn = true;
		g_runtimeLines.push_back(middle);

		WorldRuntime::LineEntry newer;
		newer.text = QStringLiteral("starch");
		newer.flags = WorldRuntime::LineOutput;
		newer.hardReturn = true;
		g_runtimeLines.push_back(newer);

		WorldView view;
		view.resize(860, 520);
		view.show();
		view.setRuntimeObserver(fakeRuntimePointer());
		QCoreApplication::processEvents();

		QPlainTextEdit* input = view.inputEditor();
		QVERIFY(input);
		input->setFocus();

		view.setInputText(QStringLiteral("sta"), true);
		QTextCursor cursor = input->textCursor();
		cursor.setPosition(view.inputText().size());
		input->setTextCursor(cursor);

		QTest::keyClick(input, Qt::Key_Tab);
		QCOMPARE(view.inputText(), QStringLiteral("starch"));

		QTest::keyClick(input, Qt::Key_Tab);
		QCOMPARE(view.inputText(), QStringLiteral("starlight"));

		QTest::keyClick(input, Qt::Key_Left);

		view.setInputText(QStringLiteral("sta"), true);
		cursor = input->textCursor();
		cursor.setPosition(view.inputText().size());
		input->setTextCursor(cursor);

		QTest::keyClick(input, Qt::Key_Tab);
		QCOMPARE(view.inputText(), QStringLiteral("starch"));

		resetTestState();
	}

	void outputFindNoMatchReturnsWithoutHanging()
	{
		resetTestState();
		WorldView view;
		view.setRuntimeObserver(fakeRuntimePointer());
		view.appendOutputText(QStringLiteral("Alpha beta gamma"), true);

		scheduleDialogInteraction(
			[](const QDialog* dialog)
			{
				return dialog->windowTitle() == QStringLiteral("Find in output buffer...");
			},
			[](const QDialog* dialog)
			{
				if (auto* combo = dialog->findChild<QComboBox*>())
					combo->setCurrentText(QStringLiteral("ZZZ"));
				if (QPushButton* findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			});

		scheduleDialogInteraction(
			[](const QDialog* dialog)
			{
				auto* messageBox = qobject_cast<const QMessageBox*>(dialog);
				if (!messageBox)
					return false;
				return messageBox->windowTitle() == QStringLiteral("Find");
			},
			[](const QDialog* dialog)
			{
				QMetaObject::invokeMethod(const_cast<QDialog*>(dialog), "accept", Qt::QueuedConnection);
			});

		QElapsedTimer timer;
		timer.start();
		QVERIFY(!view.doOutputFind(false));
		QVERIFY2(timer.elapsed() < 1000, "Output find should return promptly for no-match searches.");

		resetTestState();
	}

	void outputFindUsesRenderedBufferCoordinates()
	{
		resetTestState();

		// Intentionally desync runtime text indices from rendered text to catch
		// regressions where search scans runtime lines but highlights in document.
		WorldRuntime::LineEntry runtimeLine;
		runtimeLine.text = QStringLiteral("xxxxxnew staff");
		g_runtimeLines.push_back(runtimeLine);

		WorldView view;
		view.setRuntimeObserver(fakeRuntimePointer());
		view.appendOutputText(QStringLiteral("new staff"), true);

		scheduleDialogInteraction(
			[](const QDialog* dialog)
			{
				return dialog->windowTitle() == QStringLiteral("Find in output buffer...");
			},
			[](const QDialog* dialog)
			{
				if (auto* combo = dialog->findChild<QComboBox*>())
					combo->setCurrentText(QStringLiteral("new"));
				if (QPushButton* findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			});

		QVERIFY(view.doOutputFind(false));
		QCOMPARE(view.outputSelectionText(), QStringLiteral("new"));

		resetTestState();
	}

	void outputFindAgainKeepsDirectionFromInitialSearch()
	{
		resetTestState();

		WorldView view;
		view.setRuntimeObserver(fakeRuntimePointer());
		view.appendOutputText(QStringLiteral("If you're new to CthulhuMUD"), true);
		view.appendOutputText(QStringLiteral("Read through the entire Newbie School"), true);

		scheduleDialogInteraction(
			[](const QDialog* dialog)
			{
				return dialog->windowTitle() == QStringLiteral("Find in output buffer...");
			},
			[](const QDialog* dialog)
			{
				if (auto* combo = dialog->findChild<QComboBox*>())
					combo->setCurrentText(QStringLiteral("new"));
				if (QPushButton* findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			});

		QVERIFY(view.doOutputFind(false));
		QCOMPARE(view.outputSelectionText(), QStringLiteral("New"));

		QVERIFY(view.doOutputFind(true));
		QCOMPARE(view.outputSelectionText(), QStringLiteral("new"));

		resetTestState();
	}

	void outputFindAgainKeepsDownDirectionFromInitialSearch()
	{
		resetTestState();

		WorldView view;
		view.setRuntimeObserver(fakeRuntimePointer());
		view.appendOutputText(QStringLiteral("new to CthulhuMUD"), true);
		view.appendOutputText(QStringLiteral("Read through the entire Newbie School"), true);

		scheduleDialogInteraction(
			[](const QDialog* dialog)
			{
				return dialog->windowTitle() == QStringLiteral("Find in output buffer...");
			},
			[](const QDialog* dialog)
			{
				if (auto* down = findRadioButtonByText(*dialog, QStringLiteral("Down")))
					down->setChecked(true);
				if (auto* combo = dialog->findChild<QComboBox*>())
					combo->setCurrentText(QStringLiteral("new"));
				if (QPushButton* findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			});

		QVERIFY(view.doOutputFind(false));
		QCOMPARE(view.outputSelectionText(), QStringLiteral("new"));

		QVERIFY(view.doOutputFind(true));
		QCOMPARE(view.outputSelectionText(), QStringLiteral("New"));

		resetTestState();
	}

	void linkedStyledRunsPreserveExactPlainText()
	{
		resetTestState();

		WorldView view;
		view.setRuntimeObserver(fakeRuntimePointer());
		view.resize(720, 420);
		view.show();
		QCoreApplication::processEvents();

		const QString text = QStringLiteral("Visit help and report issues now");
		const QString href = QStringLiteral("https://example.org/osc8-layout");

		QVector<WorldRuntime::StyleSpan> spans;
		spans.reserve(static_cast<int>(text.size()));
		for (qsizetype i = 0; i < text.size(); ++i)
		{
			WorldRuntime::StyleSpan span;
			span.length = 1;
			span.actionType = WorldRuntime::ActionHyperlink;
			span.action = href;
			span.bold = (i % 2) == 0;
			span.fore = (i % 3) == 0 ? QColor(QStringLiteral("#66ccff")) : QColor();
			spans.push_back(span);
		}

		view.appendOutputTextStyled(text, spans, true);
		QCoreApplication::processEvents();

		const QStringList lines = view.outputLines();
		QVERIFY(!lines.isEmpty());
		QCOMPARE(lines.first(), text);

		QTextBrowser* browser = findVisibleOutputBrowser(view);
		QVERIFY(browser);
		const QPoint anchorPoint = findAnchorPoint(*browser, href);
		QVERIFY2(anchorPoint.x() >= 0 && anchorPoint.y() >= 0,
		         "Expected hyperlink anchor in rendered output.");

		resetTestState();
	}

	void hyperlinkHoverStatePersistsAndClearsDeterministically()
	{
		resetTestState();

		WorldView view;
		view.setRuntimeObserver(fakeRuntimePointer());
		view.resize(720, 420);
		view.show();
		QCoreApplication::processEvents();

		WorldRuntime::StyleSpan linkSpan;
		linkSpan.length = QStringLiteral("example-link").size();
		linkSpan.actionType = WorldRuntime::ActionHyperlink;
		linkSpan.action = QStringLiteral("https://example.org/status-lock");

		view.appendOutputTextStyled(QStringLiteral("example-link"), {linkSpan}, true);
		QCoreApplication::processEvents();

		QTextBrowser* browser = findVisibleOutputBrowser(view);
		QVERIFY(browser);

		const QString href = linkSpan.action;
		const QPoint anchorPoint = findAnchorPoint(*browser, href);
		QVERIFY2(anchorPoint.x() >= 0 && anchorPoint.y() >= 0,
		         "Expected hyperlink anchor in rendered output.");

		QSignalSpy hoverSpy(&view, &WorldView::hyperlinkHighlighted);
		QTest::mouseMove(browser->viewport(), anchorPoint);
		QTRY_VERIFY(view.hyperlinkHoverActive());
		QTRY_VERIFY(!hoverSpy.isEmpty());
		QTRY_COMPARE(hoverSpy.back().at(0).toString(), href);

		// Additional movement over the same anchor must not clear hover state.
		QTest::mouseMove(browser->viewport(), anchorPoint + QPoint(1, 0));
		QCoreApplication::processEvents();
		QVERIFY(view.hyperlinkHoverActive());

		// Moving to a non-anchor point must clear hover deterministically.
		const QPoint nonAnchorPoint = findNonAnchorPoint(*browser);
		QVERIFY2(nonAnchorPoint.x() >= 0 && nonAnchorPoint.y() >= 0,
		         "Expected non-anchor point in output viewport.");
		QTest::mouseMove(browser->viewport(), nonAnchorPoint);
		QCoreApplication::processEvents();
		QTRY_VERIFY(!view.hyperlinkHoverActive());
		QTRY_VERIFY(!hoverSpy.isEmpty());
		QTRY_COMPARE(hoverSpy.back().at(0).toString(), QString());

		resetTestState();
	}

	void runtimeSettingsRebuildAttributeKeysExcludeWrapAndHyperlinkPresentation()
	{
		const QSet<QString>& rebuildKeys = WorldView::runtimeSettingsRebuildAttributeKeys();
		QVERIFY(!rebuildKeys.contains(QStringLiteral("wrap")));
		QVERIFY(!rebuildKeys.contains(QStringLiteral("wrap_column")));
		QVERIFY(!rebuildKeys.contains(QStringLiteral("auto_wrap_window_width")));
		QVERIFY(!rebuildKeys.contains(QStringLiteral("naws")));
		QVERIFY(!rebuildKeys.contains(QStringLiteral("use_custom_link_colour")));
		QVERIFY(!rebuildKeys.contains(QStringLiteral("underline_hyperlinks")));
		QVERIFY(!rebuildKeys.contains(QStringLiteral("hyperlink_colour")));
		QVERIFY(rebuildKeys.contains(QStringLiteral("show_bold")));
		QVERIFY(rebuildKeys.contains(QStringLiteral("line_spacing")));
	}

	void applyRuntimeSettingsWithoutOutputRebuildPreservesRenderedBuffer()
	{
		resetTestState();
		g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("0"));
		g_worldAttrs.insert(QStringLiteral("output_font_height"), QStringLiteral("10"));

		WorldView view;
		view.setRuntimeObserver(fakeRuntimePointer());
		view.appendOutputText(QStringLiteral("existing-buffer-line"), true);
		QVERIFY(view.outputLines().contains(QStringLiteral("existing-buffer-line")));
		g_runtimeLines.clear();

		WorldRuntime::LineEntry runtimeLine;
		runtimeLine.text = QStringLiteral("runtime-only-line");
		runtimeLine.flags = WorldRuntime::LineOutput;
		runtimeLine.hardReturn = true;
		g_runtimeLines.push_back(runtimeLine);

		view.applyRuntimeSettingsWithoutOutputRebuild();
		QVERIFY(view.outputLines().contains(QStringLiteral("existing-buffer-line")));
		QVERIFY(!view.outputLines().contains(QStringLiteral("runtime-only-line")));

		g_worldAttrs.insert(QStringLiteral("output_font_height"), QStringLiteral("11"));
		view.applyRuntimeSettings();
		QVERIFY(view.outputLines().contains(QStringLiteral("runtime-only-line")));
		QVERIFY(!view.outputLines().contains(QStringLiteral("existing-buffer-line")));

		resetTestState();
	}

	void rebuildOutputFromLinesLazyShowsTailThenBackfills()
	{
		resetTestState();

		WorldView view;
		view.resize(860, 520);
		view.show();
		QCoreApplication::processEvents();

		QVector<WorldRuntime::LineEntry> lines;
		lines.reserve(400);
		for (int i = 0; i < 400; ++i)
		{
			WorldRuntime::LineEntry entry;
			entry.text = QStringLiteral("lazy-%1").arg(i, 3, 10, QLatin1Char('0'));
			entry.flags = WorldRuntime::LineOutput;
			entry.hardReturn = true;
			lines.push_back(entry);
		}

		view.rebuildOutputFromLinesLazy(lines);
		const QStringList immediateLines = view.outputLines();
		QVERIFY(immediateLines.contains(QStringLiteral("lazy-399")));
		QVERIFY(!immediateLines.contains(QStringLiteral("lazy-000")));

		QTRY_VERIFY(view.outputLines().contains(QStringLiteral("lazy-000")));

		resetTestState();
	}

	void applyRuntimeSettingsRebuildPreservesEndAnchorForLazyBackfill()
	{
		resetTestState();

		g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("0"));
		g_worldAttrs.insert(QStringLiteral("use_default_output_font"), QStringLiteral("0"));
		g_worldAttrs.insert(QStringLiteral("output_font_name"), QStringLiteral("Monospace"));
		g_worldAttrs.insert(QStringLiteral("output_font_height"), QStringLiteral("10"));
		g_worldAttrs.insert(QStringLiteral("output_font_weight"), QStringLiteral("400"));
		g_worldAttrs.insert(QStringLiteral("output_font_charset"), QStringLiteral("1"));

		WorldView view;
		view.setRuntimeObserver(fakeRuntimePointer());
		view.resize(860, 520);
		view.show();
		QCoreApplication::processEvents();

		QVector<WorldRuntime::LineEntry> lines;
		lines.reserve(3200);
		for (int i = 0; i < 3200; ++i)
		{
			WorldRuntime::LineEntry entry;
			entry.text = QStringLiteral("anchor-%1").arg(i, 4, 10, QLatin1Char('0'));
			entry.flags = WorldRuntime::LineOutput;
			entry.hardReturn = true;
			lines.push_back(entry);
		}
		g_runtimeLines = lines;

		view.rebuildOutputFromLinesLazy(lines);
		QTRY_VERIFY(view.outputLines().contains(QStringLiteral("anchor-0000")));

		QTextBrowser* browser = findVisibleOutputBrowser(view);
		QVERIFY(browser);
		QScrollBar* bar = browser->verticalScrollBar();
		QVERIFY(bar);
		bar->setValue(bar->maximum());
		QCoreApplication::processEvents();
		{
			const int endTolerance = qMax(1, bar->pageStep());
			QVERIFY(bar->value() >= (bar->maximum() - endTolerance));
		}

		g_worldAttrs.insert(QStringLiteral("output_font_height"), QStringLiteral("18"));
		view.applyRuntimeSettings();

		QTRY_VERIFY(view.outputLines().contains(QStringLiteral("anchor-0000")));
		QTRY_VERIFY2(
			[&]
			{
			QScrollBar *scrollBar = browser->verticalScrollBar();
			if (!scrollBar)
			return false;
			const int endTolerance = qMax(1, scrollBar->pageStep());
			return scrollBar->value() >= (scrollBar->maximum() - endTolerance);
			}(),
			"Output viewport drifted away from end after lazy runtime-settings rebuild.");

		resetTestState();
	}

	void rebuildDuringLazyRestoreQueuesUntilBackfillCompletes()
	{
		resetTestState();

		WorldView view;
		view.resize(860, 520);
		view.show();
		QCoreApplication::processEvents();

		QVector<WorldRuntime::LineEntry> lazyLines;
		lazyLines.reserve(3000);
		for (int i = 0; i < 3000; ++i)
		{
			WorldRuntime::LineEntry entry;
			entry.text = QStringLiteral("lazy-queue-%1").arg(i, 4, 10, QLatin1Char('0'));
			entry.flags = WorldRuntime::LineOutput;
			entry.hardReturn = true;
			lazyLines.push_back(entry);
		}

		view.rebuildOutputFromLinesLazy(lazyLines);
		const QStringList immediateLines = view.outputLines();
		QVERIFY(immediateLines.contains(QStringLiteral("lazy-queue-2999")));
		QVERIFY(!immediateLines.contains(QStringLiteral("lazy-queue-0000")));

		QVector<WorldRuntime::LineEntry> queuedLines;
		queuedLines.reserve(2);
		WorldRuntime::LineEntry queuedEntryA;
		queuedEntryA.text = QStringLiteral("queued-rebuild-a");
		queuedEntryA.flags = WorldRuntime::LineOutput;
		queuedEntryA.hardReturn = true;
		queuedLines.push_back(queuedEntryA);
		WorldRuntime::LineEntry queuedEntryB;
		queuedEntryB.text = QStringLiteral("queued-rebuild-b");
		queuedEntryB.flags = WorldRuntime::LineOutput;
		queuedEntryB.hardReturn = true;
		queuedLines.push_back(queuedEntryB);

		view.rebuildOutputFromLines(queuedLines);
		QVERIFY(!view.outputLines().contains(QStringLiteral("queued-rebuild-a")));

		QTRY_VERIFY(view.outputLines().contains(QStringLiteral("queued-rebuild-a")));
		QTRY_VERIFY(view.outputLines().contains(QStringLiteral("queued-rebuild-b")));
		QVERIFY(!view.outputLines().contains(QStringLiteral("lazy-queue-2999")));
		QVERIFY(!view.outputLines().contains(QStringLiteral("lazy-queue-0000")));

		resetTestState();
	}

	void lazyRebuildDuringLazyRestoreDoesNotOverwriteInitialRestore()
	{
		resetTestState();

		WorldView view;
		view.resize(860, 520);
		view.show();
		QCoreApplication::processEvents();

		QVector<WorldRuntime::LineEntry> initialRestoreLines;
		initialRestoreLines.reserve(3000);
		for (int i = 0; i < 3000; ++i)
		{
			WorldRuntime::LineEntry entry;
			entry.text = QStringLiteral("initial-restore-%1").arg(i, 4, 10, QLatin1Char('0'));
			entry.flags = WorldRuntime::LineOutput;
			entry.hardReturn = true;
			initialRestoreLines.push_back(entry);
		}

		view.rebuildOutputFromLinesLazy(initialRestoreLines);
		const QStringList immediateLines = view.outputLines();
		QVERIFY(immediateLines.contains(QStringLiteral("initial-restore-2999")));
		QVERIFY(!immediateLines.contains(QStringLiteral("initial-restore-0000")));

		QVector<WorldRuntime::LineEntry> queuedRebuildLines;
		queuedRebuildLines.reserve(2);
		WorldRuntime::LineEntry queuedEntryA;
		queuedEntryA.text = QStringLiteral("queued-lazy-a");
		queuedEntryA.flags = WorldRuntime::LineOutput;
		queuedEntryA.hardReturn = true;
		queuedRebuildLines.push_back(queuedEntryA);
		WorldRuntime::LineEntry queuedEntryB;
		queuedEntryB.text = QStringLiteral("queued-lazy-b");
		queuedEntryB.flags = WorldRuntime::LineOutput;
		queuedEntryB.hardReturn = true;
		queuedRebuildLines.push_back(queuedEntryB);

		view.rebuildOutputFromLinesLazy(queuedRebuildLines);
		QVERIFY(!view.outputLines().contains(QStringLiteral("queued-lazy-a")));

		QTRY_VERIFY(view.outputLines().contains(QStringLiteral("queued-lazy-a")));
		QTRY_VERIFY(view.outputLines().contains(QStringLiteral("queued-lazy-b")));
		QVERIFY(!view.outputLines().contains(QStringLiteral("initial-restore-2999")));
		QVERIFY(!view.outputLines().contains(QStringLiteral("initial-restore-0000")));

		resetTestState();
	}

	void hyperlinkPresentationSettingsRestyleInPlaceWithoutRebuild()
	{
		resetTestState();
		g_worldAttrs.insert(QStringLiteral("use_custom_link_colour"), QStringLiteral("0"));
		g_worldAttrs.insert(QStringLiteral("underline_hyperlinks"), QStringLiteral("0"));

		WorldView view;
		view.setRuntimeObserver(fakeRuntimePointer());
		view.resize(860, 520);
		view.show();
		view.applyRuntimeSettings();
		QCoreApplication::processEvents();

		const QColor targetColour(QStringLiteral("#12ab34"));
		const QString oldestHref = QStringLiteral("https://example.org/inplace/000");
		QString newestHref;
		for (int i = 0; i < 160; ++i)
		{
			const QString lineText = QStringLiteral("link-%1").arg(i, 3, 10, QLatin1Char('0'));
			const QString href =
				QStringLiteral("https://example.org/inplace/%1").arg(i, 3, 10, QLatin1Char('0'));
			newestHref = href;
			WorldRuntime::StyleSpan span;
			span.length = lineText.size();
			span.actionType = WorldRuntime::ActionHyperlink;
			span.action = href;
			view.appendOutputTextStyled(lineText, {span}, true);
		}
		QCoreApplication::processEvents();

		const QStringList linesBefore = view.outputLines();
		QVERIFY(linesBefore.contains(QStringLiteral("link-000")));
		QVERIFY(linesBefore.contains(QStringLiteral("link-159")));

		QTextBrowser* browser = findVisibleOutputBrowser(view);
		QVERIFY(browser);
		QTextDocument* document = browser->document();
		QVERIFY(document);

		const AnchorFormatSnapshot beforeOldest = findAnchorFormatSnapshot(*document, oldestHref);
		const AnchorFormatSnapshot beforeNewest = findAnchorFormatSnapshot(*document, newestHref);
		QVERIFY(beforeOldest.found);
		QVERIFY(beforeNewest.found);
		QVERIFY(!beforeOldest.format.fontUnderline());
		QVERIFY(!beforeNewest.format.fontUnderline());

		g_worldAttrs.insert(QStringLiteral("use_custom_link_colour"), QStringLiteral("1"));
		g_worldAttrs.insert(QStringLiteral("underline_hyperlinks"), QStringLiteral("1"));
		g_worldAttrs.insert(QStringLiteral("hyperlink_colour"), targetColour.name());
		g_runtimeLines.clear();

		view.applyRuntimeSettingsWithoutOutputRebuild();

		QTRY_VERIFY2(
			[&]
			{
			const AnchorFormatSnapshot newest = findAnchorFormatSnapshot(*document, newestHref);
			return newest.found && newest.format.fontUnderline() &&
			newest.format.foreground().color() == targetColour;
			}(),
			"Newest hyperlink was not restyled in-place to underline/custom color.");

		QTRY_VERIFY2(
			[&]
			{
			const AnchorFormatSnapshot oldest = findAnchorFormatSnapshot(*document, oldestHref);
			return oldest.found && oldest.format.fontUnderline() &&
			oldest.format.foreground().color() == targetColour;
			}(),
			"Oldest hyperlink was not restyled in-place to underline/custom color.");

		const QStringList linesAfter = view.outputLines();
		QCOMPARE(linesAfter, linesBefore);

		resetTestState();
	}

	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_WorldView_Basic)

#if __has_include("tst_WorldView_Basic.moc")
#include "tst_WorldView_Basic.moc"
#endif
