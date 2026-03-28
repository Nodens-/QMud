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
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialog>
#include <QElapsedTimer>
#include <QImage>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QWheelEvent>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <functional>
#include <limits>
#include <memory>

namespace
{
	QMap<QString, QString>              g_worldAttrs;
	QMap<QString, QString>              g_worldMultilineAttrs;
	QMap<QString, QVariant>             g_globalOptions;
	QVector<WorldRuntime::LineEntry>    g_runtimeLines;
	WorldRuntime::TextRectangleSettings g_textRectangle;
	int                                 g_outputFontHeight{0};
	int                                 g_drawOutputNotifyCount{0};
	int                                 g_lastDrawOutputFirstLine{0};
	int                                 g_lastDrawOutputAdjustedScroll{0};
	int                                 g_worldOutputResizedNotifyCount{0};
	QVector<MiniWindow>                 g_testMiniWindows;
	int                                 g_worldHotspotCallbackCount{0};
	QString                             g_lastWorldHotspotFunction;
	QString                             g_lastWorldHotspotId;
	long                                g_lastWorldHotspotFlags{0};
	WorldView                          *g_runtimeView{nullptr};
	unsigned short                      g_currentActionSource{WorldRuntime::eUnknownActionSource};
	bool                                g_connected{false};
	bool                                g_nawsNegotiated{false};
	bool                                g_useFakeAppController{false};

	AppController                      *fakeAppControllerPointer()
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
		return g_runtimeLines;
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
		g_runtimeLines.clear();
		g_textRectangle                 = {};
		g_outputFontHeight              = 0;
		g_drawOutputNotifyCount         = 0;
		g_lastDrawOutputFirstLine       = 0;
		g_lastDrawOutputAdjustedScroll  = 0;
		g_worldOutputResizedNotifyCount = 0;
		g_testMiniWindows.clear();
		g_worldHotspotCallbackCount = 0;
		g_lastWorldHotspotFunction.clear();
		g_lastWorldHotspotId.clear();
		g_lastWorldHotspotFlags = 0;
		g_runtimeView           = nullptr;
		g_currentActionSource   = WorldRuntime::eUnknownActionSource;
		g_connected             = false;
		g_nawsNegotiated        = false;
		g_useFakeAppController  = false;
	}

	int boundedSizeToInt(const qsizetype value)
	{
		constexpr qsizetype kMin = 0;
		constexpr qsizetype kMax = std::numeric_limits<int>::max();
		return static_cast<int>(qBound(kMin, value, kMax));
	}

	QPushButton *findButtonByText(const QObject &root, const QString &text)
	{
		const auto buttons = root.findChildren<QPushButton *>();
		for (QPushButton *button : buttons)
		{
			if (button && button->text() == text)
				return button;
		}
		return nullptr;
	}

	QRadioButton *findRadioButtonByText(const QObject &root, const QString &text)
	{
		const auto buttons = root.findChildren<QRadioButton *>();
		for (QRadioButton *button : buttons)
		{
			if (button && button->text() == text)
				return button;
		}
		return nullptr;
	}

	QCheckBox *findCheckBoxByText(const QObject &root, const QString &text)
	{
		const auto boxes = root.findChildren<QCheckBox *>();
		for (QCheckBox *box : boxes)
		{
			if (box && box->text() == text)
				return box;
		}
		return nullptr;
	}

	void scheduleDialogInteraction(const std::function<bool(const QDialog *)> &matcher,
	                               const std::function<void(const QDialog *)> &action)
	{
		auto runner = std::make_shared<std::function<void(int)>>();
		*runner     = [runner, matcher, action](int retriesLeft)
		{
			for (QWidget *widget : QApplication::topLevelWidgets())
			{
				auto *dialog = qobject_cast<QDialog *>(widget);
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

	QSplitter *findOutputSplitter(const WorldView &view)
	{
		const auto splitters = view.findChildren<QSplitter *>();
		for (QSplitter *splitter : splitters)
		{
			if (!splitter || splitter->count() != 2)
				continue;
			if (!qobject_cast<QTextBrowser *>(splitter->widget(0)) ||
			    !qobject_cast<QTextBrowser *>(splitter->widget(1)))
				continue;
			return splitter;
		}
		return nullptr;
	}

	QPair<QTextBrowser *, QTextBrowser *> findSplitOutputBrowsers(const WorldView &view)
	{
		QSplitter *splitter = findOutputSplitter(view);
		if (!splitter)
			return {nullptr, nullptr};
		auto *topBrowser    = qobject_cast<QTextBrowser *>(splitter->widget(0));
		auto *bottomBrowser = qobject_cast<QTextBrowser *>(splitter->widget(1));
		return {topBrowser, bottomBrowser};
	}

	QTextBrowser *findVisibleOutputBrowser(const WorldView &view)
	{
		const auto    browsers         = view.findChildren<QTextBrowser *>();
		QTextBrowser *bestBrowser      = nullptr;
		int           bestViewportArea = -1;
		auto          viewportArea     = [](const QTextBrowser *browser)
		{
			if (!browser || !browser->viewport())
				return -1;
			if (!browser->isVisible() || !browser->viewport()->isVisible())
				return -1;
			if (browser->viewport()->visibleRegion().isEmpty())
				return -1;
			const int width  = browser->viewport()->width();
			const int height = browser->viewport()->height();
			if (width <= 1 || height <= 1)
				return -1;
			return width * height;
		};
		for (QTextBrowser *browser : browsers)
		{
			const int area = viewportArea(browser);
			if (area <= 0)
				continue;
			if (area > bestViewportArea)
			{
				bestViewportArea = area;
				bestBrowser      = browser;
			}
		}
		if (!bestBrowser && !browsers.isEmpty())
			bestBrowser = browsers.constFirst();
		return bestBrowser;
	}

	QPoint findHyperlinkPoint(WorldView &view, QTextBrowser &browser, const QString &href)
	{
		if (!browser.viewport() || href.isEmpty())
			return {-1, -1};

		auto waitForNativeOutputReady = [&view, &browser]()
		{
			auto         *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QElapsedTimer timer;
			timer.start();
			while (timer.elapsed() < 500)
			{
				const bool ready = nativeCanvas && nativeCanvas->isVisible() && view.isVisible() &&
				                   browser.viewport() && browser.viewport()->isVisible() &&
				                   browser.viewport()->width() > 1 && browser.viewport()->height() > 1;
				if (ready)
					return true;
				QCoreApplication::processEvents();
				QTest::qWait(1);
			}
			return nativeCanvas && nativeCanvas->isVisible() && view.isVisible() && browser.viewport() &&
			       browser.viewport()->isVisible() && browser.viewport()->width() > 1 &&
			       browser.viewport()->height() > 1;
		};
		if (!waitForNativeOutputReady())
			return {-1, -1};

		auto matchesHref = [&href](const QString &candidate)
		{ return candidate == href || QUrl::fromPercentEncoding(candidate.toUtf8()) == href; };

		QSignalSpy hoverSpy(&view, &WorldView::hyperlinkHighlighted);
		const auto probeHover = [&browser, &hoverSpy, &matchesHref](const QPoint &point)
		{
			const qsizetype before = hoverSpy.size();
			QTest::mouseMove(browser.viewport(), point);
			QCoreApplication::processEvents();
			for (qsizetype i = before; i < hoverSpy.size(); ++i)
			{
				if (matchesHref(hoverSpy.at(i).at(0).toString()))
					return true;
			}
			return false;
		};

		QSignalSpy activatedSpy(&view, &WorldView::hyperlinkActivated);
		const auto probeClick = [&browser, &activatedSpy, &matchesHref](const QPoint &point)
		{
			const qsizetype before = activatedSpy.size();
			QTest::mouseClick(browser.viewport(), Qt::LeftButton, Qt::NoModifier, point);
			QCoreApplication::processEvents();
			for (qsizetype i = before; i < activatedSpy.size(); ++i)
			{
				if (matchesHref(activatedSpy.at(i).at(0).toString()))
					return true;
			}
			return false;
		};

		if (QTextDocument *document = browser.document())
		{
			const int probeLimit = qMin(256, document->characterCount());
			for (int position = 0; position < probeLimit; position += 2)
			{
				QTextCursor cursor(document);
				cursor.setPosition(position);
				const QPoint point = browser.cursorRect(cursor).center();
				if (point.x() < 0 || point.y() < 0)
					continue;
				if (probeHover(point))
					return point;
			}
		}

		const QRect area = browser.viewport()->rect();
		const int   fastBottom =
		    qMin(area.bottom(), area.top() + qMax(24, QFontMetrics(browser.font()).height() * 4));
		const int fastRight = qMin(area.right(), area.left() + 320);
		for (int y = area.top(); y <= fastBottom; y += 2)
		{
			for (int x = area.left(); x <= fastRight; x += 2)
			{
				if (probeHover({x, y}))
					return {x, y};
			}
		}

		for (int y = area.top(); y <= fastBottom; y += 6)
		{
			for (int x = area.left(); x <= fastRight; x += 6)
			{
				if (probeClick({x, y}))
					return {x, y};
			}
		}

		for (int y = area.top(); y <= area.bottom(); y += 12)
		{
			for (int x = area.left(); x <= area.right(); x += 12)
			{
				if (probeClick({x, y}))
					return {x, y};
			}
		}
		return {-1, -1};
	}

	QPoint findLineInformationPoint(const WorldView &view, const QTextBrowser &browser)
	{
		if (!browser.viewport())
			return {-1, -1};

		auto         *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
		QElapsedTimer readyTimer;
		readyTimer.start();
		while (readyTimer.elapsed() < 500)
		{
			const bool ready = nativeCanvas && nativeCanvas->isVisible() && view.isVisible() &&
			                   browser.viewport()->isVisible() && browser.viewport()->width() > 1 &&
			                   browser.viewport()->height() > 1;
			if (ready)
				break;
			QCoreApplication::processEvents();
			QTest::qWait(1);
		}

		const QRect area = browser.viewport()->rect();
		const int   probeBottom =
		    qMin(area.bottom(), area.top() + qMax(24, QFontMetrics(browser.font()).height() * 6));

		for (int y = area.top(); y <= probeBottom; y += 4)
		{
			for (int x = area.left(); x <= area.right(); x += 4)
			{
				QTest::mouseDClick(browser.viewport(), Qt::LeftButton, Qt::NoModifier, {x, y});
				QCoreApplication::processEvents();
				if (view.hasOutputSelection())
					return {x, y};
			}
		}
		return {-1, -1};
	}

	QPoint findNonHyperlinkPoint(WorldView &view, QTextBrowser &browser)
	{
		if (!browser.viewport())
			return {-1, -1};

		const auto probePoint = [&view, &browser](const QPoint &point)
		{
			QTest::mouseMove(browser.viewport(), point);
			QCoreApplication::processEvents();
			return !view.hyperlinkHoverActive();
		};

		const QRect area = browser.viewport()->rect();
		for (int y = area.top(); y <= area.bottom(); y += 4)
		{
			for (int x = area.left(); x <= area.right(); x += 4)
			{
				if (probePoint({x, y}))
					return {x, y};
			}
		}
		return {-1, -1};
	}

	QPoint findTextPoint(const QTextBrowser &browser, const int documentPosition)
	{
		if (!browser.viewport() || !browser.document() || documentPosition < 0)
			return {-1, -1};
		QTextCursor cursor(browser.document());
		cursor.setPosition(documentPosition);
		return browser.cursorRect(cursor).center();
	}

	MiniWindow &appendTestMiniWindow(const QString &name, const QRect &rect, int flags, const QColor &fill)
	{
		MiniWindow window;
		window.name            = name;
		window.flags           = flags;
		window.show            = true;
		window.temporarilyHide = false;
		window.width           = rect.width();
		window.height          = rect.height();
		window.rect            = rect;
		window.surface =
		    QImage(qMax(1, rect.width()), qMax(1, rect.height()), QImage::Format_ARGB32_Premultiplied);
		window.surface.fill(fill);
		g_testMiniWindows.push_back(window);
		return g_testMiniWindows.last();
	}

	QVector<WorldRuntime::LineEntry> makeBenchmarkLines(const QString &prefix, const int count,
	                                                    const bool includeAgedTimestamps = false)
	{
		QVector<WorldRuntime::LineEntry> lines;
		lines.reserve(qMax(0, count));
		const QDateTime now = QDateTime::currentDateTime();
		for (int i = 0; i < count; ++i)
		{
			WorldRuntime::LineEntry entry;
			entry.text       = QStringLiteral("%1-%2").arg(prefix).arg(i, 5, 10, QLatin1Char('0'));
			entry.flags      = WorldRuntime::LineOutput;
			entry.hardReturn = true;
			entry.lineNumber = i + 1;
			if (includeAgedTimestamps)
				entry.time = now.addSecs(-7200 + i);
			lines.push_back(entry);
		}
		return lines;
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

void AppController::onCommandTriggered(const QString &)
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
	return g_outputFontHeight;
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

void WorldRuntime::notifyDrawOutputWindow(int firstLine, int adjustedScroll)
{
	++g_drawOutputNotifyCount;
	g_lastDrawOutputFirstLine      = firstLine;
	g_lastDrawOutputAdjustedScroll = adjustedScroll;
}

QImage WorldRuntime::backgroundImage() const
{
	return {};
}

int WorldRuntime::backgroundImageMode() const
{
	return 0;
}

void WorldRuntime::layoutMiniWindows(const QSize &, const QSize &, bool)
{
}

QVector<MiniWindow *> WorldRuntime::sortedMiniWindows()
{
	QVector<MiniWindow *> result;
	result.reserve(g_testMiniWindows.size());
	for (MiniWindow &window : g_testMiniWindows)
		result.push_back(&window);
	return result;
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

void WorldRuntime::setView(WorldView *view)
{
	g_runtimeView = view;
}

WorldView *WorldRuntime::view() const
{
	return g_runtimeView;
}

void WorldRuntime::installPendingPlugins()
{
}

void WorldRuntime::notifyWorldOutputResized()
{
	++g_worldOutputResizedNotifyCount;
}

void WorldRuntime::firePluginCommandChanged()
{
}

void WorldRuntime::firePluginTabComplete(QString &)
{
}

bool WorldRuntime::isConnected() const
{
	return g_connected;
}

bool WorldRuntime::isNawsNegotiated() const
{
	return g_nawsNegotiated;
}

bool WorldRuntime::callPluginHotspotFunction(const QString &, const QString &, long, const QString &)
{
	return false;
}

bool WorldRuntime::callWorldHotspotFunction(const QString &functionName, long flags, const QString &hotspotId)
{
	++g_worldHotspotCallbackCount;
	g_lastWorldHotspotFunction = functionName;
	g_lastWorldHotspotFlags    = flags;
	g_lastWorldHotspotId       = hotspotId;
	return true;
}

void WorldRuntime::notifyMiniWindowMouseMoved(int, int, const QString &)
{
}

MiniWindow *WorldRuntime::miniWindow(const QString &name)
{
	for (MiniWindow &window : g_testMiniWindows)
	{
		if (window.name == name)
			return &window;
	}
	return nullptr;
}

void WorldRuntime::notifyOutputSelectionChanged()
{
}

void WorldRuntime::setOutputFrozen(bool)
{
}

const WorldRuntime::TextRectangleSettings &WorldRuntime::textRectangle() const
{
	return g_textRectangle;
}

void WorldRuntime::setTextRectangle(const TextRectangleSettings &settings)
{
	g_textRectangle = settings;
}

const QList<WorldRuntime::Macro> &WorldRuntime::macros() const
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

bool WorldRuntime::isActive() const
{
	return false;
}

void WorldRuntime::prepareInputEchoForDisplay(QString &, QVector<StyleSpan> &, bool) const
{
}

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

QString WorldRuntime::startupDirectory() const
{
	return {};
}

QString WorldRuntime::defaultWorldDirectory() const
{
	return {};
}

QString WorldRuntime::defaultLogDirectory() const
{
	return {};
}

QString WorldRuntime::formatTime(const QDateTime &, const QString &format, bool) const
{
	return format;
}

void WorldRuntime::addLine(const QString &text, int flags, bool hardReturn, const QDateTime &time)
{
	LineEntry entry;
	entry.text       = text;
	entry.flags      = flags;
	entry.hardReturn = hardReturn;
	entry.time       = time;
	entry.lineNumber = g_runtimeLines.isEmpty() ? 1 : (g_runtimeLines.last().lineNumber + 1);
	g_runtimeLines.push_back(entry);
}

void WorldRuntime::addLine(const QString &text, int flags, const QVector<StyleSpan> &spans, bool hardReturn,
                           const QDateTime &time)
{
	LineEntry entry;
	entry.text       = text;
	entry.flags      = flags;
	entry.hardReturn = hardReturn;
	entry.spans      = spans;
	entry.time       = time;
	entry.lineNumber = g_runtimeLines.isEmpty() ? 1 : (g_runtimeLines.last().lineNumber + 1);
	g_runtimeLines.push_back(entry);
}

void WorldRuntime::finalizePendingInputLineHardReturn()
{
	if (g_runtimeLines.isEmpty())
		return;
	LineEntry &last = g_runtimeLines.last();
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

		void rendererBenchmark_data()
		{
			QTest::addColumn<QString>("scenario");

			QTest::newRow("sustained-native") << QStringLiteral("sustained");
			QTest::newRow("split-native") << QStringLiteral("split");
			QTest::newRow("fade-native") << QStringLiteral("fade");
		}

		void rendererBenchmark()
		{
			QFETCH(QString, scenario);

			resetTestState();

			g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("0"));
			if (scenario == QStringLiteral("split"))
				g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));
			if (scenario == QStringLiteral("fade"))
			{
				g_worldAttrs.insert(QStringLiteral("fade_output_buffer_after_seconds"), QStringLiteral("1"));
				g_worldAttrs.insert(QStringLiteral("fade_output_opacity_percent"), QStringLiteral("40"));
				g_worldAttrs.insert(QStringLiteral("fade_output_seconds"), QStringLiteral("8"));
			}

			WorldView view;
			view.resize(860, 520);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			if (scenario == QStringLiteral("split"))
			{
				for (int i = 0; i < 320; ++i)
					view.appendOutputText(QStringLiteral("split-primer-%1").arg(i), true);
				QCoreApplication::processEvents();

				QTextBrowser *browser = findVisibleOutputBrowser(view);
				QVERIFY(browser);
				const QPointF localPos(browser->viewport()->rect().center());
				const QPointF globalPos(browser->viewport()->mapToGlobal(localPos.toPoint()));
				QWheelEvent   wheelUp(localPos, globalPos, QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
				                      Qt::NoModifier, Qt::NoScrollPhase, false);
				QCoreApplication::sendEvent(browser->viewport(), &wheelUp);
				QCoreApplication::processEvents();
				QTRY_VERIFY(view.isScrollbackSplitActive());
			}

			const QVector<WorldRuntime::LineEntry> fadeLines =
			    (scenario == QStringLiteral("fade"))
			        ? makeBenchmarkLines(QStringLiteral("fade-bench"), 1800, true)
			        : QVector<WorldRuntime::LineEntry>();

			QBENCHMARK_ONCE
			{
				if (scenario == QStringLiteral("sustained"))
				{
					for (int i = 0; i < 1400; ++i)
						view.appendOutputText(QStringLiteral("sustained-bench-%1").arg(i), true);
				}
				else if (scenario == QStringLiteral("split"))
				{
					for (int i = 0; i < 900; ++i)
						view.appendOutputText(QStringLiteral("split-bench-%1").arg(i), true);
				}
				else
					view.rebuildOutputFromLines(fadeLines);
			}

			QCoreApplication::processEvents();
			if (scenario == QStringLiteral("fade"))
				QVERIFY(view.outputLines().contains(QStringLiteral("fade-bench-00000")));
			else
				QVERIFY(!view.outputLines().isEmpty());

			resetTestState();
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

		void runtimeObserverTransitionDetachesPreviouslyAttachedRuntime()
		{
			resetTestState();

			WorldView view;
			view.setRuntime(fakeRuntimePointer());
			QCOMPARE(g_runtimeView, &view);

			view.setRuntimeObserver(fakeRuntimePointer());
			QCOMPARE(g_runtimeView, nullptr);

			view.setRuntime(nullptr);
			QCOMPARE(g_runtimeView, nullptr);

			resetTestState();
		}

		void textRectangleAppliedToOutputViewport()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			const QRect fullRect = view.outputTextRectangle();
			QVERIFY2(fullRect.width() > 200,
			         "Baseline output viewport width too small for text-rectangle test.");
			QVERIFY2(fullRect.height() > 200,
			         "Baseline output viewport height too small for text-rectangle test.");

			g_textRectangle.left   = 15;
			g_textRectangle.top    = 11;
			g_textRectangle.right  = 260;
			g_textRectangle.bottom = 210;
			view.updateWrapMargin();
			QCoreApplication::processEvents();

			const QRect rect = view.outputTextRectangle();
			QCOMPARE(rect.left(), fullRect.left() + g_textRectangle.left);
			QCOMPARE(rect.top(), fullRect.top() + g_textRectangle.top);
			QVERIFY(qAbs(rect.width() - (g_textRectangle.right - g_textRectangle.left)) <= 16);
			QVERIFY(qAbs(rect.height() - (g_textRectangle.bottom - g_textRectangle.top)) <= 16);

			resetTestState();
		}

		void textRectangleSupportsNegativeRightBottomOffsets()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			const int baseWidth  = view.outputTextRectangle().width();
			const int baseHeight = view.outputTextRectangle().height();
			QVERIFY(baseWidth > 80);
			QVERIFY(baseHeight > 80);

			g_textRectangle.left   = 7;
			g_textRectangle.top    = 9;
			g_textRectangle.right  = -13;
			g_textRectangle.bottom = -17;
			view.updateWrapMargin();
			QCoreApplication::processEvents();

			const QRect rect = view.outputTextRectangle();
			QCOMPARE(rect.width(), baseWidth - g_textRectangle.left - 13);
			QCOMPARE(rect.height(), baseHeight - g_textRectangle.top - 17);

			resetTestState();
		}

		void textRectangleContainsBothSplitPanes()
		{
			resetTestState();

			WorldView view;
			view.resize(920, 660);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			g_textRectangle.left   = 17;
			g_textRectangle.top    = 13;
			g_textRectangle.right  = 320;
			g_textRectangle.bottom = 250;
			view.updateWrapMargin();
			QCoreApplication::processEvents();

			QSplitter *outputSplitter = findOutputSplitter(view);
			QVERIFY(outputSplitter);

			outputSplitter->setSizes(QList<int>() << 170 << 120);
			QCoreApplication::processEvents();

			const QRect textRect = view.outputTextRectangle();
			QCOMPARE(outputSplitter->geometry().topLeft(), textRect.topLeft());
			QVERIFY(outputSplitter->geometry().width() >= textRect.width());
			QVERIFY(outputSplitter->geometry().height() >= textRect.height());

			QWidget *const outputStack = outputSplitter->parentWidget();
			QVERIFY(outputStack);

			for (int i = 0; i < outputSplitter->count(); ++i)
			{
				auto *browser = qobject_cast<QTextBrowser *>(outputSplitter->widget(i));
				QVERIFY(browser);
				QWidget *const viewport = browser->viewport();
				QVERIFY(viewport);
				const QRect viewportRect(viewport->mapTo(outputStack, QPoint(0, 0)), viewport->size());
				QVERIFY2(textRect.contains(viewportRect),
				         "Split output viewport escaped configured text rectangle.");
			}

			resetTestState();
		}

		void textRectangleInfoBoundsAreInclusive()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			g_textRectangle.left   = 19;
			g_textRectangle.top    = 14;
			g_textRectangle.right  = 318;
			g_textRectangle.bottom = 252;
			view.updateWrapMargin();
			QCoreApplication::processEvents();

			const QRect rect = view.outputTextRectangle();
			QVERIFY(rect.width() > 0);
			QVERIFY(rect.height() > 0);

			const int info290 = rect.left();
			const int info291 = rect.top();
			const int info292 = rect.right();
			const int info293 = rect.bottom();

			QCOMPARE(info292 - info290 + 1, rect.width());
			QCOMPARE(info293 - info291 + 1, rect.height());

			resetTestState();
		}

		void collapsedScrollbackSplitterHandleIsHidden()
		{
			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			QSplitter *outputSplitter = findOutputSplitter(view);
			QVERIFY(outputSplitter);
			QCOMPARE(outputSplitter->handleWidth(), 0);
		}

		void drawOutputWindowNotificationTracksScrollPosition()
		{
			resetTestState();
			g_outputFontHeight = 10;

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			for (int i = 0; i < 320; ++i)
				view.appendOutputText(QStringLiteral("draw-notify-%1").arg(i), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());

			const int baselineToTop = g_drawOutputNotifyCount;
			QCOMPARE(view.setOutputScroll(0, true), 0);
			QTRY_VERIFY(g_drawOutputNotifyCount > baselineToTop);

			const int baselineToEnd = g_drawOutputNotifyCount;
			QCOMPARE(view.setOutputScroll(-1, true), 0);
			QTRY_VERIFY(g_drawOutputNotifyCount > baselineToEnd);
			QCOMPARE(g_lastDrawOutputAdjustedScroll, view.outputScrollPosition());
			QCOMPARE(g_lastDrawOutputFirstLine, (g_lastDrawOutputAdjustedScroll / g_outputFontHeight) + 1);

			resetTestState();
		}

		void drawOutputWindowNotificationCoalescesBurstUpdates()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			const int baseline = g_drawOutputNotifyCount;
			for (int i = 0; i < 120; ++i)
				view.appendOutputText(QStringLiteral("draw-burst-%1").arg(i), true);

			QCOMPARE(g_drawOutputNotifyCount, baseline);
			QCoreApplication::processEvents();
			QTRY_COMPARE(g_drawOutputNotifyCount, baseline + 1);

			resetTestState();
		}

		void worldOutputResizedNotificationFiresOnResize()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			const int baseline = g_worldOutputResizedNotifyCount;
			view.resize(1020, 700);
			QCoreApplication::processEvents();
			QTRY_VERIFY(g_worldOutputResizedNotifyCount > baseline);

			resetTestState();
		}

		void worldOutputResizedNotificationCoalescesResizeBurst()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			const int baseline = g_worldOutputResizedNotifyCount;

			view.resize(1000, 650);
			view.resize(1040, 680);
			view.resize(1080, 700);

			QCOMPARE(g_worldOutputResizedNotifyCount, baseline);
			QCoreApplication::processEvents();
			QTRY_COMPARE(g_worldOutputResizedNotifyCount, baseline + 1);

			resetTestState();
		}

		void outputScrollApiClampsAndTogglesVisibility()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			for (int i = 0; i < 280; ++i)
				view.appendOutputText(QStringLiteral("scroll-api-%1").arg(i), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());

			QCOMPARE(view.setOutputScroll(999999, false), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());
			QTRY_VERIFY(!view.outputScrollBarVisible());

			QCOMPARE(view.setOutputScroll(-1, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());
			QTRY_VERIFY(view.outputScrollBarVisible());

			resetTestState();
		}

		void scrollbackSplitActivatesAndCollapsesWithAutoPause()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QSplitter *outputSplitter = findOutputSplitter(view);
			QVERIFY(outputSplitter);

			for (int i = 0; i < 300; ++i)
				view.appendOutputText(QStringLiteral("split-activate-%1").arg(i), true);
			QCoreApplication::processEvents();

			QTextBrowser *topBrowser = findVisibleOutputBrowser(view);
			QVERIFY(topBrowser);
			QScrollBar *topBar = topBrowser->verticalScrollBar();
			QVERIFY(topBar);
			QTRY_VERIFY(topBar->maximum() > topBar->minimum());

			const QPointF localPos(topBrowser->viewport()->rect().center());
			const QPointF globalPos(topBrowser->viewport()->mapToGlobal(localPos.toPoint()));
			for (int i = 0; i < 6 && !view.isScrollbackSplitActive(); ++i)
			{
				QWheelEvent wheelUp(localPos, globalPos, QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
				                    Qt::NoModifier, Qt::NoScrollPhase, false);
				QCoreApplication::sendEvent(topBrowser->viewport(), &wheelUp);
				QCoreApplication::processEvents();
			}
			QTRY_VERIFY(view.isScrollbackSplitActive());
			QTRY_VERIFY(outputSplitter->handleWidth() > 0);

			for (int i = 0; i < 80 && view.isScrollbackSplitActive(); ++i)
			{
				QWheelEvent wheelDown(localPos, globalPos, QPoint(0, 0), QPoint(0, -120), Qt::NoButton,
				                      Qt::NoModifier, Qt::NoScrollPhase, false);
				QCoreApplication::sendEvent(topBrowser->viewport(), &wheelDown);
				QCoreApplication::processEvents();
			}
			QTRY_VERIFY(!view.isScrollbackSplitActive());
			QCOMPARE(outputSplitter->handleWidth(), 0);

			resetTestState();
		}

		void scrollbackSplitLivePaneSticksToBottomOnAppend()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 260; ++i)
				view.appendOutputText(QStringLiteral("split-live-stick-%1").arg(i), true);
			QCoreApplication::processEvents();

			QTextBrowser *topBrowser = findVisibleOutputBrowser(view);
			QVERIFY(topBrowser);
			QScrollBar *topBar = topBrowser->verticalScrollBar();
			QVERIFY(topBar);
			QTRY_VERIFY(topBar->maximum() > topBar->minimum());

			const QPointF localPos(topBrowser->viewport()->rect().center());
			const QPointF globalPos(topBrowser->viewport()->mapToGlobal(localPos.toPoint()));
			QWheelEvent   wheelUp(localPos, globalPos, QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
			                      Qt::NoModifier, Qt::NoScrollPhase, false);
			QCoreApplication::sendEvent(topBrowser->viewport(), &wheelUp);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isScrollbackSplitActive());

			const auto [splitTop, splitBottom] = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			QScrollBar *liveBar = splitBottom->verticalScrollBar();
			QVERIFY(liveBar);
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());

			view.appendOutputText(QStringLiteral("split-live-stick-new"), true);
			QCoreApplication::processEvents();
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());

			resetTestState();
		}

		void scrollbackSplitKeepsLivePaneSizeOnResize()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));

			WorldView view;
			view.resize(760, 520);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QSplitter *outputSplitter = findOutputSplitter(view);
			QVERIFY(outputSplitter);

			for (int i = 0; i < 260; ++i)
				view.appendOutputText(QStringLiteral("split-resize-%1").arg(i), true);
			QCoreApplication::processEvents();

			QTextBrowser *topBrowser = findVisibleOutputBrowser(view);
			QVERIFY(topBrowser);
			const QPointF localPos(topBrowser->viewport()->rect().center());
			const QPointF globalPos(topBrowser->viewport()->mapToGlobal(localPos.toPoint()));
			QWheelEvent   wheelUp(localPos, globalPos, QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
			                      Qt::NoModifier, Qt::NoScrollPhase, false);
			QCoreApplication::sendEvent(topBrowser->viewport(), &wheelUp);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isScrollbackSplitActive());

			QList<int> beforeSizes = outputSplitter->sizes();
			QVERIFY(beforeSizes.size() >= 2);
			const int beforeLive = beforeSizes.at(1);
			QVERIFY(beforeLive > 0);

			view.resize(900, 620);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isScrollbackSplitActive());

			const QList<int> afterSizes = outputSplitter->sizes();
			QVERIFY(afterSizes.size() >= 2);
			const int afterLive = afterSizes.at(1);
			QVERIFY(afterLive > 0);
			QVERIFY(qAbs(afterLive - beforeLive) <= qMax(80, beforeLive / 2));

			resetTestState();
		}

		void scrollCommandsTargetLivePaneWhenSplitActive()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 260; ++i)
				view.appendOutputText(QStringLiteral("split-scroll-cmd-%1").arg(i), true);
			QCoreApplication::processEvents();

			QTextBrowser *topBrowser = findVisibleOutputBrowser(view);
			QVERIFY(topBrowser);
			const QPointF localPos(topBrowser->viewport()->rect().center());
			const QPointF globalPos(topBrowser->viewport()->mapToGlobal(localPos.toPoint()));
			QWheelEvent   wheelUp(localPos, globalPos, QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
			                      Qt::NoModifier, Qt::NoScrollPhase, false);
			QCoreApplication::sendEvent(topBrowser->viewport(), &wheelUp);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isScrollbackSplitActive());

			const auto [splitTop, splitBottom] = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			QScrollBar *topBar  = splitTop->verticalScrollBar();
			QScrollBar *liveBar = splitBottom->verticalScrollBar();
			QVERIFY(topBar);
			QVERIFY(liveBar);

			const int topBefore = topBar->value();
			view.scrollOutputToStart();
			QTRY_COMPARE(liveBar->value(), liveBar->minimum());
			QCOMPARE(topBar->value(), topBefore);

			view.scrollOutputLineDown();
			QTRY_VERIFY(liveBar->value() > liveBar->minimum());

			view.scrollOutputPageDown();
			QTRY_VERIFY(liveBar->value() > liveBar->minimum());

			view.scrollOutputToEnd();
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());

			resetTestState();
		}

		void splitSelectionAndCopyFollowLastSelectedPane()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			view.appendOutputText(QStringLiteral("unique_top_token"), true);
			for (int i = 0; i < 240; ++i)
				view.appendOutputText(QStringLiteral("split-select-fill-%1").arg(i), true);
			view.appendOutputText(QStringLiteral("unique_live_token"), true);
			QCoreApplication::processEvents();

			QTextBrowser *topBrowser = findVisibleOutputBrowser(view);
			QVERIFY(topBrowser);
			const QPointF localPos(topBrowser->viewport()->rect().center());
			const QPointF globalPos(topBrowser->viewport()->mapToGlobal(localPos.toPoint()));
			QWheelEvent   wheelUp(localPos, globalPos, QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
			                      Qt::NoModifier, Qt::NoScrollPhase, false);
			QCoreApplication::sendEvent(topBrowser->viewport(), &wheelUp);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isScrollbackSplitActive());

			const auto [splitTop, splitBottom] = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			auto selectWordInView = [&view](const QTextBrowser *target, const QVector<QPoint> &points)
			{
				for (const QPoint &point : points)
				{
					QTest::mouseDClick(target->viewport(), Qt::LeftButton, Qt::NoModifier, point);
					QCoreApplication::processEvents();
					if (view.hasOutputSelection() && !view.outputSelectionText().isEmpty())
						return view.outputSelectionText();
				}
				return QString{};
			};

			const QVector<QPoint> topProbePoints{
			    splitTop->viewport()->rect().center(),
			    QPoint(24, qMax(8, splitTop->viewport()->rect().height() / 3)),
			    QPoint(24, qMax(8, splitTop->viewport()->rect().height() / 2)),
			};
			const QVector<QPoint> bottomProbePoints{
			    QPoint(24, qMax(8, splitBottom->viewport()->rect().height() / 2)),
			    QPoint(24, qMax(8, splitBottom->viewport()->rect().height() - 12)),
			    splitBottom->viewport()->rect().center(),
			};
			const QString topSelection  = selectWordInView(splitTop, topProbePoints);
			const QString liveSelection = selectWordInView(splitBottom, bottomProbePoints);
			QVERIFY(!topSelection.isEmpty());
			QVERIFY(!liveSelection.isEmpty());

			view.copySelection();
			QTRY_COMPARE(QGuiApplication::clipboard()->text(), liveSelection);

			view.copySelectionAsHtml();
			const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
			QVERIFY(mime);
			QVERIFY(mime->hasHtml());
			QVERIFY(mime->text().contains(liveSelection));
			QVERIFY(mime->html().contains(liveSelection));

			const QString topSelectionAgain = selectWordInView(splitTop, topProbePoints);
			QVERIFY(!topSelectionAgain.isEmpty());
			view.copySelection();
			QTRY_COMPARE(QGuiApplication::clipboard()->text(), topSelectionAgain);
			resetTestState();
		}

		void collapsedSplitIgnoresHiddenLiveSelection()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 220; ++i)
				view.appendOutputText(QStringLiteral("collapse-hidden-live-%1").arg(i), true);
			view.appendOutputText(QStringLiteral("collapse_hidden_live_token"), true);
			QCoreApplication::processEvents();

			QTextBrowser *topBrowser = findVisibleOutputBrowser(view);
			QVERIFY(topBrowser);
			const QPointF localPos(topBrowser->viewport()->rect().center());
			const QPointF globalPos(topBrowser->viewport()->mapToGlobal(localPos.toPoint()));
			QWheelEvent   wheelUp(localPos, globalPos, QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
			                      Qt::NoModifier, Qt::NoScrollPhase, false);
			QCoreApplication::sendEvent(topBrowser->viewport(), &wheelUp);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isScrollbackSplitActive());

			const auto [splitTop, splitBottom] = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			const QVector<QPoint> liveProbePoints{
			    QPoint(24, qMax(8, splitBottom->viewport()->rect().height() - 12)),
			    splitBottom->viewport()->rect().center(),
			    QPoint(24, qMax(8, splitBottom->viewport()->rect().height() / 2)),
			};
			bool selectedLiveWord = false;
			for (const QPoint &point : liveProbePoints)
			{
				QTest::mouseDClick(splitBottom->viewport(), Qt::LeftButton, Qt::NoModifier, point);
				QCoreApplication::processEvents();
				if (view.hasOutputSelection() && !view.outputSelectionText().isEmpty())
				{
					selectedLiveWord = true;
					break;
				}
			}
			QVERIFY(selectedLiveWord);

			for (int i = 0; i < 120 && view.isScrollbackSplitActive(); ++i)
			{
				QWheelEvent wheelDown(localPos, globalPos, QPoint(0, 0), QPoint(0, -120), Qt::NoButton,
				                      Qt::NoModifier, Qt::NoScrollPhase, false);
				QCoreApplication::sendEvent(topBrowser->viewport(), &wheelDown);
				QCoreApplication::processEvents();
			}
			QTRY_VERIFY(!view.isScrollbackSplitActive());
			QTRY_COMPARE(view.outputSelectionText(), QString());
			QVERIFY(!view.hasOutputSelection());
			resetTestState();
		}

		void nativeOutputRendererCreatesCanvas()
		{
			{
				WorldView view;
				view.resize(900, 640);
				view.show();
				view.setRuntimeObserver(fakeRuntimePointer());
				QCoreApplication::processEvents();
				auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
				QVERIFY(nativeCanvas);
				QVERIFY(nativeCanvas->isVisible());
			}
		}

		void runtimeSettingsKeepNativeOutputCanvasVisible()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.appendOutputText(QStringLiteral("toggle-runtime-output"), true);
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);
			QVERIFY(nativeCanvas->isVisible());
			QVERIFY(view.outputLines().contains(QStringLiteral("toggle-runtime-output")));
			view.applyRuntimeSettingsWithoutOutputRebuild();
			QCoreApplication::processEvents();

			QVERIFY(nativeCanvas->isVisible());
			QVERIFY(view.outputLines().contains(QStringLiteral("toggle-runtime-output")));
			resetTestState();
		}

		void nativeOutputRendererPaintsPlainRuntimeLines()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			view.appendOutputText(QStringLiteral("native-plain-line"), true);
			nativeCanvas->update();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 1);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("native-plain-line"));
			resetTestState();
		}

		void nativeOutputRendererPaintsStyledBackgroundRuns()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			WorldRuntime::StyleSpan span;
			span.length = QStringLiteral("styled-background").size();
			span.fore   = QColor(QStringLiteral("#ffffff"));
			span.back   = QColor(QStringLiteral("#ff0000"));
			span.bold   = true;
			span.italic = true;

			view.appendOutputTextStyled(QStringLiteral("styled-background"), {span}, true);
			QCoreApplication::processEvents();

			nativeCanvas->update();
			QCoreApplication::processEvents();

			const QImage image = nativeCanvas->grab().toImage();
			QVERIFY(!image.isNull());

			bool hasRedBackgroundPixel = false;
			for (int y = 0; y < image.height() && !hasRedBackgroundPixel; ++y)
			{
				for (int x = 0; x < image.width(); ++x)
				{
					const QColor color = image.pixelColor(x, y);
					if (color.red() > 200 && color.green() < 80 && color.blue() < 80)
					{
						hasRedBackgroundPixel = true;
						break;
					}
				}
			}
			QVERIFY(hasRedBackgroundPixel);
			resetTestState();
		}

		void nativeOutputRendererLeavesSingleLongLineUnwrappedWithoutEmbeddedBreaks()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("1"));

			WorldView view;
			view.resize(360, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			const QString longLine = QStringLiteral("native wrap test ") + QString(180, QLatin1Char('w'));
			view.appendOutputText(longLine, true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 1);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_visual_row_count").toInt(), 1);
			resetTestState();
		}

		void nativeOutputRendererRespectsEmbeddedLineBreaksInLocalOutput()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("wrap_column"), QStringLiteral("80"));

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			const QString wrappedLocalText = QStringLiteral("local one\nlocal two\nlocal three");
			view.appendOutputText(wrappedLocalText, true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 1);
			QTRY_VERIFY(nativeCanvas->property("qmud_native_visual_row_count").toInt() >= 3);
			resetTestState();
		}

		void wrapRelatedWorldSettingChangesDoNotRewrapExistingNativeOutput()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("wrap_column"), QStringLiteral("80"));

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			const QString wrappedLine = QStringLiteral("first\nsecond\nthird");
			view.appendOutputText(wrappedLine, true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_visual_row_count").toInt(), 3);
			QTRY_COMPARE(view.outputLines().size(), 1);
			QCOMPARE(view.outputLines().constFirst(), wrappedLine);

			g_worldAttrs.insert(QStringLiteral("wrap_column"), QStringLiteral("120"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("1"));
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_visual_row_count").toInt(), 3);
			QTRY_COMPARE(view.outputLines().size(), 1);
			QCOMPARE(view.outputLines().constFirst(), wrappedLine);
			resetTestState();
		}

		void nawsMiniWindowMarginChangesDoNotRewrapExistingNativeOutput()
		{
			resetTestState();

			g_connected      = true;
			g_nawsNegotiated = true;
			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("wrap_column"), QStringLiteral("80"));

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			const QString wrappedLine = QStringLiteral("alpha\nbeta\ngamma");
			view.appendOutputText(wrappedLine, true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_visual_row_count").toInt(), 3);
			QTRY_COMPARE(view.outputLines().size(), 1);
			QCOMPARE(view.outputLines().constFirst(), wrappedLine);

			MiniWindow &dock = appendTestMiniWindow(QStringLiteral("dock"), QRect(0, 0, 120, 140), 0,
			                                        QColor(20, 20, 20, 200));
			dock.position    = 6;
			view.onMiniWindowsChanged();
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_visual_row_count").toInt(), 3);
			QTRY_COMPARE(view.outputLines().size(), 1);
			QCOMPARE(view.outputLines().constFirst(), wrappedLine);

			dock.width = 220;
			dock.rect.setWidth(220);
			view.onMiniWindowsChanged();
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_visual_row_count").toInt(), 3);
			QTRY_COMPARE(view.outputLines().size(), 1);
			QCOMPARE(view.outputLines().constFirst(), wrappedLine);
			resetTestState();
		}

		void miniWindowReservationDoesNotApplyViewportClipMargins()
		{
			resetTestState();

			g_connected      = true;
			g_nawsNegotiated = true;
			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("wrap_column"), QStringLiteral("80"));

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			const int viewportWidthBefore = browser->viewport() ? browser->viewport()->width() : 0;
			QVERIFY(viewportWidthBefore > 0);

			MiniWindow &dock = appendTestMiniWindow(QStringLiteral("dock-clip"), QRect(0, 0, 150, 140), 0,
			                                        QColor(20, 20, 20, 200));
			dock.position    = 6;
			view.onMiniWindowsChanged();
			QCoreApplication::processEvents();

			browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			const int viewportWidthAfter = browser->viewport() ? browser->viewport()->width() : 0;
			QCOMPARE(viewportWidthAfter, viewportWidthBefore);
			resetTestState();
		}

		void nativeOutputRendererUsesIncrementalCacheForAppends()
		{
			resetTestState();

			WorldView view;
			view.resize(640, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			view.appendOutputText(QStringLiteral("inc-one"), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const int rebuildsBefore = nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();
			const int updatesBefore = nativeCanvas->property("qmud_native_cache_incremental_updates").toInt();
			QVERIFY(rebuildsBefore > 0 || updatesBefore > 0);

			view.appendOutputText(QStringLiteral("inc-two"), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 2);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("inc-two"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt(), rebuildsBefore);
			QTRY_VERIFY(nativeCanvas->property("qmud_native_cache_incremental_updates").toInt() >
			            updatesBefore);
			resetTestState();
		}

		void nativeOutputRendererNonContiguousLineNumbersReuseCacheOnStablePaints()
		{
			resetTestState();

			WorldView view;
			view.resize(640, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			QVector<WorldRuntime::LineEntry> restoredLines;
			restoredLines.reserve(3);
			for (int i = 0; i < 3; ++i)
			{
				WorldRuntime::LineEntry entry;
				entry.text       = QStringLiteral("noncontig-%1").arg(i);
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				entry.lineNumber = (i + 1) * 10;
				restoredLines.push_back(entry);
			}
			g_runtimeLines = restoredLines;

			view.restoreOutputFromPersistedLines(restoredLines);
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 3);
			const int rebuildsBeforeStablePaint =
			    nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();

			nativeCanvas->update();
			QCoreApplication::processEvents();
			const int rebuildsAfterFirstStablePaint =
			    nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();
			QVERIFY(rebuildsAfterFirstStablePaint >= rebuildsBeforeStablePaint);

			nativeCanvas->update();
			QCoreApplication::processEvents();
			QTRY_COMPARE(nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt(),
			             rebuildsAfterFirstStablePaint);
			resetTestState();
		}

		void nativeOutputRendererDropsTrimmedHeadLinesIncrementally()
		{
			resetTestState();

			WorldView view;
			view.resize(640, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			view.appendOutputText(QStringLiteral("trim-a"), true);
			view.appendOutputText(QStringLiteral("trim-b"), true);
			view.appendOutputText(QStringLiteral("trim-c"), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 3);

			const int rebuildsBefore  = nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();
			const int trimDropsBefore = nativeCanvas->property("qmud_native_cache_trim_drops").toInt();

			g_runtimeLines.removeFirst();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 2);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("trim-c"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt(), rebuildsBefore);
			QTRY_VERIFY(nativeCanvas->property("qmud_native_cache_trim_drops").toInt() > trimDropsBefore);
			resetTestState();
		}

		void nativeOutputRendererReusesLayoutCacheWithoutGeometryChanges()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("1"));

			WorldView view;
			view.resize(420, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			view.appendOutputText(QStringLiteral("cache-row-") + QString(200, QLatin1Char('x')), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const int resetsBefore = nativeCanvas->property("qmud_native_layout_cache_resets").toInt();
			const int rowsBefore   = nativeCanvas->property("qmud_native_layout_row_measurements").toInt();
			QVERIFY(rowsBefore > 0);

			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_layout_cache_resets").toInt(), resetsBefore);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_layout_row_measurements").toInt(), rowsBefore);
			resetTestState();
		}

		void nativeOutputRendererKeepsLayoutCacheStableOnResizeWhenNativeWrapIsDisabled()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("1"));

			WorldView view;
			view.resize(520, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			view.appendOutputText(QStringLiteral("resize-row-") + QString(220, QLatin1Char('y')), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const int resetsBefore = nativeCanvas->property("qmud_native_layout_cache_resets").toInt();
			const int rowsBefore   = nativeCanvas->property("qmud_native_layout_row_measurements").toInt();

			view.resize(300, 360);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_layout_cache_resets").toInt(), resetsBefore);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_layout_row_measurements").toInt(), rowsBefore);
			resetTestState();
		}

		void nativeOutputRendererPaintsWhenScrollbackSplitIsActive()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));

			WorldView view;
			view.resize(640, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			for (int i = 0; i < 260; ++i)
				view.appendOutputText(QStringLiteral("split-native-%1").arg(i), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *docBar = browser->verticalScrollBar();
			QVERIFY(docBar);
			QTRY_VERIFY(docBar->maximum() > docBar->minimum());
			const QPointF localPos(browser->viewport()->rect().center());
			const QPointF globalPos(browser->viewport()->mapToGlobal(localPos.toPoint()));
			for (int i = 0; i < 4 && !view.isScrollbackSplitActive(); ++i)
			{
				QWheelEvent wheelEvent(localPos, globalPos, QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
				                       Qt::NoModifier, Qt::NoScrollPhase, false);
				QCoreApplication::sendEvent(browser->viewport(), &wheelEvent);
				QCoreApplication::processEvents();
			}
			QTRY_VERIFY(view.isScrollbackSplitActive());

			nativeCanvas->setProperty("qmud_native_visual_row_count", -77);
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_VERIFY(nativeCanvas->property("qmud_native_visual_row_count").toInt() != -77);
			QTRY_VERIFY(nativeCanvas->property("qmud_native_visual_row_count").toInt() > 0);
			resetTestState();
		}

		void nativeOutputRendererWrapHandlesEdgeCases()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("1"));

			WorldView view;
			view.resize(260, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			const QString styledWrapLine =
			    QStringLiteral("style-cross-wrap ") + QString(160, QLatin1Char('s'));
			QVector<WorldRuntime::StyleSpan> styledWrapSpans;
			WorldRuntime::StyleSpan          spanA;
			spanA.length = 37;
			spanA.fore   = QColor(QStringLiteral("#ffffff"));
			spanA.back   = QColor(QStringLiteral("#203060"));
			styledWrapSpans.push_back(spanA);
			WorldRuntime::StyleSpan spanB;
			spanB.length = 53;
			spanB.bold   = true;
			spanB.fore   = QColor(QStringLiteral("#80ff80"));
			styledWrapSpans.push_back(spanB);
			WorldRuntime::StyleSpan spanC;
			spanC.length = qMax(1, boundedSizeToInt(styledWrapLine.size()) - spanA.length - spanB.length);
			spanC.italic = true;
			spanC.fore   = QColor(QStringLiteral("#ffd080"));
			styledWrapSpans.push_back(spanC);
			view.appendOutputTextStyled(styledWrapLine, styledWrapSpans, true);

			const QString unicodeLine =
			    QString::fromUtf8("Αλφα βήτα 世界🙂🙂🙂 unicode-wrap ") + QString(100, QLatin1Char('u'));
			const QString tabAndSpaceLine =
			    QStringLiteral("tabs\tand  spaces\tline ") + QString(80, QLatin1Char('t'));
			const QString longWordLine = QString(260, QLatin1Char('w'));

			view.appendOutputText(unicodeLine, true);
			view.appendOutputText(tabAndSpaceLine, true);
			view.appendOutputText(longWordLine, true);
			view.appendOutputText(QString(), true);

			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 5);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_visual_row_count").toInt(), 5);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(), QString());

			const QStringList lines = view.outputLines();
			QVERIFY(lines.contains(styledWrapLine));
			QVERIFY(lines.contains(unicodeLine));
			QVERIFY(lines.contains(tabAndSpaceLine));
			QVERIFY(lines.contains(longWordLine));
			QVERIFY(lines.contains(QString()));
			resetTestState();
		}

		void nativeOutputRendererPreservesSelectionBoundsApis()
		{
			resetTestState();

			WorldView view;
			view.resize(720, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			view.appendOutputText(QStringLiteral("alpha"), false);
			view.appendOutputText(QStringLiteral("beta"), true);
			view.appendOutputText(QStringLiteral("gamma delta"), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			view.setOutputSelection(2, 2, 1, 6);
			QCOMPARE(view.outputSelectionStartLine(), 2);
			QCOMPARE(view.outputSelectionEndLine(), 2);
			QCOMPARE(view.outputSelectionStartColumn(), 1);
			QCOMPARE(view.outputSelectionEndColumn(), 6);
			QCOMPARE(view.outputSelectionText(), QStringLiteral("gamma"));

			view.setOutputSelection(1, 2, 6, 6);
			QCOMPARE(view.outputSelectionStartLine(), 1);
			QCOMPARE(view.outputSelectionEndLine(), 2);
			QCOMPARE(view.outputSelectionStartColumn(), 6);
			QCOMPARE(view.outputSelectionEndColumn(), 6);
			const QString crossLineSelection = view.outputSelectionText();
			QVERIFY(crossLineSelection.contains(QStringLiteral("beta")));
			QVERIFY(crossLineSelection.contains(QStringLiteral("gamma")));
			resetTestState();
		}

		void nativeOutputRendererPreservesInputSelectionColumnsForGetInfo()
		{
			resetTestState();

			WorldView view;
			view.resize(720, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			view.setInputText(QStringLiteral("north"), true);

			QTextCursor cursor = input->textCursor();
			cursor.setPosition(3);
			input->setTextCursor(cursor);
			QCOMPARE(view.inputSelectionStartColumn(), 4);
			QCOMPARE(view.inputSelectionEndColumn(), 0);

			cursor.setPosition(1);
			cursor.setPosition(4, QTextCursor::KeepAnchor);
			input->setTextCursor(cursor);
			QCOMPARE(view.inputSelectionStartColumn(), 2);
			QCOMPARE(view.inputSelectionEndColumn(), 4);
			resetTestState();
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
			const qsizetype   lookAt = before.indexOf(QStringLiteral("look"));
			QVERIFY(lookAt >= 0);
			QCOMPARE(before.value(lookAt + 1), QStringLiteral("The room is quiet."));

			view.applyRuntimeSettings();

			const QStringList after      = view.outputLines();
			const qsizetype   lookAfter  = after.indexOf(QStringLiteral("look"));
			const qsizetype   mergedLine = after.indexOf(QStringLiteral("lookThe room is quiet."));
			QVERIFY(lookAfter >= 0);
			QCOMPARE(after.value(lookAfter + 1), QStringLiteral("The room is quiet."));
			QCOMPARE(mergedLine, qsizetype{-1});

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

			const QStringList lines      = view.outputLines();
			const qsizetype   mergedLine = lines.indexOf(QStringLiteral("Ready.look"));
			QVERIFY(mergedLine >= 0);
			QVERIFY(g_runtimeLines.size() >= 2);
			QVERIFY(!g_runtimeLines.at(0).hardReturn);
			QVERIFY(g_runtimeLines.at(1).hardReturn);

			resetTestState();
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

		void tabCompletionCyclesUpwardAndResetsOnNonTabKey()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("tab_completion_space"), QStringLiteral("0"));

			WorldRuntime::LineEntry older;
			older.text       = QStringLiteral("stamina");
			older.flags      = WorldRuntime::LineOutput;
			older.hardReturn = true;
			g_runtimeLines.push_back(older);

			WorldRuntime::LineEntry middle;
			middle.text       = QStringLiteral("starlight");
			middle.flags      = WorldRuntime::LineOutput;
			middle.hardReturn = true;
			g_runtimeLines.push_back(middle);

			WorldRuntime::LineEntry newer;
			newer.text       = QStringLiteral("starch");
			newer.flags      = WorldRuntime::LineOutput;
			newer.hardReturn = true;
			g_runtimeLines.push_back(newer);

			WorldView view;
			view.resize(860, 520);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus();

			view.setInputText(QStringLiteral("sta"), true);
			QTextCursor cursor = input->textCursor();
			cursor.movePosition(QTextCursor::End);
			input->setTextCursor(cursor);

			QTest::keyClick(input, Qt::Key_Tab);
			QCOMPARE(view.inputText(), QStringLiteral("starch"));

			QTest::keyClick(input, Qt::Key_Tab);
			QCOMPARE(view.inputText(), QStringLiteral("starlight"));

			QTest::keyClick(input, Qt::Key_Left);

			view.setInputText(QStringLiteral("sta"), true);
			cursor = input->textCursor();
			cursor.movePosition(QTextCursor::End);
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
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [](const QDialog *dialog)
			    {
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(QStringLiteral("ZZZ"));
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });

			scheduleDialogInteraction(
			    [](const QDialog *dialog)
			    {
				    auto *messageBox = qobject_cast<const QMessageBox *>(dialog);
				    if (!messageBox)
					    return false;
				    return messageBox->windowTitle() == QStringLiteral("Find");
			    },
			    [](const QDialog *dialog)
			    {
				    QMetaObject::invokeMethod(const_cast<QDialog *>(dialog), "accept", Qt::QueuedConnection);
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
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [](const QDialog *dialog)
			    {
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(QStringLiteral("new"));
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });

			QVERIFY(view.doOutputFind(false));
			QCOMPARE(view.outputSelectionText(), QStringLiteral("new"));

			resetTestState();
		}

		void nativeOutputRendererFindUsesRenderedBufferCoordinates()
		{
			resetTestState();

			WorldRuntime::LineEntry runtimeLine;
			runtimeLine.text = QStringLiteral("xxxxxnew staff");
			g_runtimeLines.push_back(runtimeLine);

			WorldView view;
			view.resize(720, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.appendOutputText(QStringLiteral("new staff"), true);
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);
			nativeCanvas->update();
			QCoreApplication::processEvents();

			scheduleDialogInteraction(
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [](const QDialog *dialog)
			    {
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(QStringLiteral("new"));
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });

			QVERIFY(view.doOutputFind(false));
			QCOMPARE(view.outputSelectionText(), QStringLiteral("new"));
			QVERIFY(view.outputSelectionStartLine() >= 1);
			QVERIFY(view.outputSelectionStartColumn() >= 1);
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
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [](const QDialog *dialog)
			    {
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(QStringLiteral("new"));
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
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
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [](const QDialog *dialog)
			    {
				    if (auto *down = findRadioButtonByText(*dialog, QStringLiteral("Down")))
					    down->setChecked(true);
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(QStringLiteral("new"));
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });

			QVERIFY(view.doOutputFind(false));
			QCOMPARE(view.outputSelectionText(), QStringLiteral("new"));

			QVERIFY(view.doOutputFind(true));
			QCOMPARE(view.outputSelectionText(), QStringLiteral("New"));

			resetTestState();
		}

		void outputFindAgainAdvancesToNextMatchOnSameLine()
		{
			resetTestState();

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.appendOutputText(QStringLiteral("new new"), true);

			scheduleDialogInteraction(
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [](const QDialog *dialog)
			    {
				    if (auto *down = findRadioButtonByText(*dialog, QStringLiteral("Down")))
					    down->setChecked(true);
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(QStringLiteral("new"));
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });

			QVERIFY(view.doOutputFind(false));
			QCOMPARE(view.outputSelectionText(), QStringLiteral("new"));
			const int firstLine  = view.outputSelectionStartLine();
			const int firstStart = view.outputSelectionStartColumn();
			QVERIFY(firstLine >= 0);
			QVERIFY(firstStart >= 1);

			QVERIFY(view.doOutputFind(true));
			QCOMPARE(view.outputSelectionText(), QStringLiteral("new"));
			QCOMPARE(view.outputSelectionStartLine(), firstLine);
			QVERIFY(view.outputSelectionStartColumn() > firstStart);

			resetTestState();
		}

		void outputFindHonoursMatchCaseAndRegexpFlags()
		{
			resetTestState();

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.appendOutputText(QStringLiteral("New new now"), true);

			scheduleDialogInteraction(
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [](const QDialog *dialog)
			    {
				    if (auto *down = findRadioButtonByText(*dialog, QStringLiteral("Down")))
					    down->setChecked(true);
				    if (auto *matchCase = findCheckBoxByText(*dialog, QStringLiteral("Match case")))
					    matchCase->setChecked(true);
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(QStringLiteral("new"));
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });

			QVERIFY(view.doOutputFind(false));
			QCOMPARE(view.outputSelectionText(), QStringLiteral("new"));
			QCOMPARE(view.outputSelectionStartColumn(), 5);

			scheduleDialogInteraction(
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [](const QDialog *dialog)
			    {
				    if (auto *down = findRadioButtonByText(*dialog, QStringLiteral("Down")))
					    down->setChecked(true);
				    if (auto *matchCase = findCheckBoxByText(*dialog, QStringLiteral("Match case")))
					    matchCase->setChecked(false);
				    if (auto *regexp = findCheckBoxByText(*dialog, QStringLiteral("Regular expression")))
					    regexp->setChecked(true);
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(QStringLiteral("n.w"));
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });

			QVERIFY(view.doOutputFind(false));
			QCOMPARE(view.outputSelectionText(), QStringLiteral("New"));
			QCOMPARE(view.outputSelectionStartColumn(), 1);

			resetTestState();
		}

		void historyRecallWorksWhileOutputSelectionIsActive()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("arrows_change_history"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.addToHistoryForced(QStringLiteral("north"));
			view.addToHistoryForced(QStringLiteral("south"));
			view.appendOutputText(QStringLiteral("selection-source"), true);
			QCoreApplication::processEvents();

			view.selectOutputLine(0);
			QVERIFY(view.hasOutputSelection());

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus();
			QCoreApplication::processEvents();

			QTest::keyClick(input, Qt::Key_Up);
			QCOMPARE(view.inputText(), QStringLiteral("south"));
			QTest::keyClick(input, Qt::Key_Up);
			QCOMPARE(view.inputText(), QStringLiteral("north"));

			resetTestState();
		}

		void lineInformationTooltipShowsUnknownTimeWhenRuntimeLineHasNoTimestamp()
		{
			resetTestState();
			QToolTip::hideText();
			g_worldAttrs.insert(QStringLiteral("line_information"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("tool_tip_start_time"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("tool_tip_visible_time"), QStringLiteral("5000"));

			WorldRuntime::LineEntry entry;
			entry.text       = QStringLiteral("tooltip-source-line");
			entry.flags      = WorldRuntime::LineOutput;
			entry.hardReturn = true;
			entry.lineNumber = 1;
			g_runtimeLines.push_back(entry);

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.rebuildOutputFromLines(g_runtimeLines);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			const QPoint point = findLineInformationPoint(view, *browser);
			QVERIFY2(point.x() >= 0 && point.y() >= 0,
			         "Expected line-information tooltip probe point in rendered output.");
			QTest::mouseMove(browser->viewport(), point);

			QTRY_VERIFY(QToolTip::text().contains(QStringLiteral("Line 1, ")));
			QTRY_VERIFY(QToolTip::text().contains(QStringLiteral("(unknown time)")));
			QToolTip::hideText();

			resetTestState();
		}

		void runtimePartialOutputUsesNativeOverlayWithoutDocumentWrites()
		{
			resetTestState();

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QVERIFY(browser->document());
			QVERIFY(browser->document()->toPlainText().isEmpty());

			view.updatePartialOutputText(QStringLiteral("par"));
			QCoreApplication::processEvents();
			QStringList lines = view.outputLines();
			QCOMPARE(lines.size(), 1);
			QCOMPARE(lines.first(), QStringLiteral("par"));
			QVERIFY(browser->document()->toPlainText().isEmpty());

			view.updatePartialOutputText(QStringLiteral("part"));
			QCoreApplication::processEvents();
			lines = view.outputLines();
			QCOMPARE(lines.size(), 1);
			QCOMPARE(lines.first(), QStringLiteral("part"));
			QVERIFY(browser->document()->toPlainText().isEmpty());

			view.clearPartialOutput();
			QCoreApplication::processEvents();
			QVERIFY(view.outputLines().isEmpty());
			QVERIFY(browser->document()->toPlainText().isEmpty());

			view.updatePartialOutputText(QStringLiteral("temp"));
			view.appendOutputText(QStringLiteral("final"), true);
			QCoreApplication::processEvents();
			lines = view.outputLines();
			QVERIFY(!lines.isEmpty());
			QCOMPARE(lines.last(), QStringLiteral("final"));
			QVERIFY(!lines.contains(QStringLiteral("temp")));
			QVERIFY(browser->document()->toPlainText().isEmpty());

			resetTestState();
		}

		void runtimeObserverAttachKeepsOutputDocumentEmpty()
		{
			resetTestState();

			WorldView view;
			view.resize(760, 460);
			view.show();
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QVERIFY(browser->document());

			view.appendOutputText(QStringLiteral("pre-runtime-line"), true);
			QCoreApplication::processEvents();
			QVERIFY(view.outputLines().contains(QStringLiteral("pre-runtime-line")));
			QVERIFY(browser->document()->toPlainText().isEmpty());

			WorldRuntime::LineEntry runtimeEntry;
			runtimeEntry.text       = QStringLiteral("runtime-native-line");
			runtimeEntry.flags      = WorldRuntime::LineOutput;
			runtimeEntry.hardReturn = true;
			runtimeEntry.lineNumber = 1;
			runtimeEntry.time       = QDateTime::currentDateTime();
			g_runtimeLines.push_back(runtimeEntry);

			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QTRY_COMPARE(view.outputLines().size(), 1);
			QCOMPARE(view.outputLines().first(), QStringLiteral("runtime-native-line"));
			QVERIFY(browser->document()->toPlainText().isEmpty());

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

			const QString                    text = QStringLiteral("Visit help and report issues now");
			const QString                    href = QStringLiteral("https://example.org/osc8-layout");

			QVector<WorldRuntime::StyleSpan> spans;
			spans.reserve(static_cast<int>(text.size()));
			for (qsizetype i = 0; i < text.size(); ++i)
			{
				WorldRuntime::StyleSpan span;
				span.length     = 1;
				span.actionType = WorldRuntime::ActionHyperlink;
				span.action     = href;
				span.bold       = (i % 2) == 0;
				span.fore       = (i % 3) == 0 ? QColor(QStringLiteral("#66ccff")) : QColor();
				spans.push_back(span);
			}

			view.appendOutputTextStyled(text, spans, true);
			QCoreApplication::processEvents();

			const QStringList lines = view.outputLines();
			QVERIFY(!lines.isEmpty());
			QCOMPARE(lines.first(), text);

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			const QPoint anchorPoint = findHyperlinkPoint(view, *browser, href);
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
			linkSpan.length     = QStringLiteral("example-link").size();
			linkSpan.actionType = WorldRuntime::ActionHyperlink;
			linkSpan.action     = QStringLiteral("https://example.org/status-lock");

			view.appendOutputTextStyled(QStringLiteral("example-link"), {linkSpan}, true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);

			const QString href        = linkSpan.action;
			const QPoint  anchorPoint = findHyperlinkPoint(view, *browser, href);
			QVERIFY2(anchorPoint.x() >= 0 && anchorPoint.y() >= 0,
			         "Expected hyperlink anchor in rendered output.");
			const QPoint resetPoint = findNonHyperlinkPoint(view, *browser);
			if (resetPoint.x() >= 0 && resetPoint.y() >= 0)
			{
				QTest::mouseMove(browser->viewport(), resetPoint);
				QCoreApplication::processEvents();
			}

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
			const QPoint nonAnchorPoint = findNonHyperlinkPoint(view, *browser);
			QVERIFY2(nonAnchorPoint.x() >= 0 && nonAnchorPoint.y() >= 0,
			         "Expected non-anchor point in output viewport.");
			QTest::mouseMove(browser->viewport(), nonAnchorPoint);
			QCoreApplication::processEvents();
			QTRY_VERIFY(!view.hyperlinkHoverActive());
			QTRY_VERIFY(!hoverSpy.isEmpty());
			QTRY_COMPARE(hoverSpy.back().at(0).toString(), QString());

			resetTestState();
		}

		void hyperlinkLeftClickEmitsHrefForCommandProcessorDispatch()
		{
			resetTestState();

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.resize(720, 420);
			view.show();
			QCoreApplication::processEvents();

			WorldRuntime::StyleSpan span;
			span.length     = QStringLiteral("assistant").size();
			span.actionType = WorldRuntime::ActionSend;
			span.action     = QStringLiteral("examine assistant|consider assistant|attack assistant");
			span.hint       = QStringLiteral("Right mouse click to act|Examine assistant|Consider assistant|"
			                                       "Attack assistant");
			view.appendOutputTextStyled(QStringLiteral("assistant"), {span}, true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			const QPoint anchorPoint = findHyperlinkPoint(view, *browser, span.action);
			QVERIFY(anchorPoint.x() >= 0 && anchorPoint.y() >= 0);

			QSignalSpy activatedSpy(&view, &WorldView::hyperlinkActivated);
			QTest::mouseClick(browser->viewport(), Qt::LeftButton, Qt::NoModifier, anchorPoint);
			QTRY_COMPARE(activatedSpy.count(), 1);
			const QString emittedHref =
			    QUrl::fromPercentEncoding(activatedSpy.at(0).at(0).toString().toUtf8());
			QCOMPARE(emittedHref, span.action);

			resetTestState();
		}

		void nativeOutputRendererHyperlinkLeftClickEmitsHrefForCommandProcessorDispatch()
		{
			resetTestState();

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.resize(720, 420);
			view.show();
			QCoreApplication::processEvents();

			WorldRuntime::StyleSpan span;
			span.length     = QStringLiteral("assistant").size();
			span.actionType = WorldRuntime::ActionSend;
			span.action     = QStringLiteral("examine assistant|consider assistant|attack assistant");
			span.hint       = QStringLiteral("Right mouse click to act|Examine assistant|Consider assistant|"
			                                       "Attack assistant");
			view.appendOutputTextStyled(QStringLiteral("assistant"), {span}, true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			const QPoint anchorPoint = findHyperlinkPoint(view, *browser, span.action);
			QVERIFY(anchorPoint.x() >= 0 && anchorPoint.y() >= 0);

			QSignalSpy activatedSpy(&view, &WorldView::hyperlinkActivated);
			QTest::mouseClick(browser->viewport(), Qt::LeftButton, Qt::NoModifier, anchorPoint);
			QTRY_COMPARE(activatedSpy.count(), 1);
			const QString emittedHref =
			    QUrl::fromPercentEncoding(activatedSpy.at(0).at(0).toString().toUtf8());
			QCOMPARE(emittedHref, span.action);
			resetTestState();
		}

		void nativeOutputRendererOverlayMiniWindowRendersAboveOutputCanvas()
		{
			resetTestState();

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.resize(760, 460);
			view.show();
			QCoreApplication::processEvents();

			QSplitter *outputSplitter = findOutputSplitter(view);
			QVERIFY(outputSplitter);
			QWidget *outputStack = outputSplitter->parentWidget();
			QVERIFY(outputStack);
			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			const QRect stackRect = outputStack->rect();
			const int   width     = qMin(120, qMax(24, stackRect.width() / 4));
			const int   height    = qMin(60, qMax(20, stackRect.height() / 6));
			const QRect overlayRect(10, 10, width, height);
			QVERIFY(stackRect.contains(overlayRect.adjusted(0, 0, -1, -1)));

			appendTestMiniWindow(QStringLiteral("native-overlay"), overlayRect, 0, QColor(255, 0, 0));
			nativeCanvas->update();
			outputStack->update();
			QCoreApplication::processEvents();

			const QPixmap grabbed = view.grab();
			QVERIFY(!grabbed.isNull());
			const QImage image       = grabbed.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
			const qreal  dpr         = grabbed.devicePixelRatio();
			QPoint       samplePoint = outputStack->mapTo(&view, overlayRect.center());
			samplePoint.setX(qBound(0, qRound(samplePoint.x() * dpr), image.width() - 1));
			samplePoint.setY(qBound(0, qRound(samplePoint.y() * dpr), image.height() - 1));
			const QColor sampled = image.pixelColor(samplePoint);
			QVERIFY(sampled.red() > 200);
			QVERIFY(sampled.green() < 80);
			QVERIFY(sampled.blue() < 80);
			resetTestState();
		}

		void nativeOutputRendererMiniWindowHotspotClickStillRoutesToWorldCallback()
		{
			resetTestState();

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.resize(760, 460);
			view.show();
			QCoreApplication::processEvents();

			QSplitter *outputSplitter = findOutputSplitter(view);
			QVERIFY(outputSplitter);
			QWidget *outputStack = outputSplitter->parentWidget();
			QVERIFY(outputStack);
			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);

			const QRect viewportInStack(viewport->mapTo(outputStack, QPoint(0, 0)), viewport->size());
			QVERIFY(viewportInStack.width() > 80);
			QVERIFY(viewportInStack.height() > 40);

			const QRect windowRect(viewportInStack.left() + 20, viewportInStack.top() + 20, 80, 40);
			QVERIFY(viewportInStack.contains(windowRect.adjusted(0, 0, -1, -1)));

			MiniWindow &window =
			    appendTestMiniWindow(QStringLiteral("native-hotspot"), windowRect, 0, QColor(0, 160, 0));
			MiniWindowHotspot hotspot;
			hotspot.rect      = QRect(0, 0, windowRect.width(), windowRect.height());
			hotspot.mouseDown = QStringLiteral("on_hotspot_down");
			window.hotspots.insert(QStringLiteral("hotspot_main"), hotspot);

			const QPoint clickInStack    = windowRect.center();
			const QPoint clickInViewport = viewport->mapFrom(outputStack, clickInStack);
			QVERIFY(viewport->rect().contains(clickInViewport));

			const int baseline = g_worldHotspotCallbackCount;
			QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, clickInViewport);
			QCoreApplication::processEvents();

			QTRY_VERIFY(g_worldHotspotCallbackCount >= baseline + 1);
			QCOMPARE(g_lastWorldHotspotFunction, QStringLiteral("on_hotspot_down"));
			QCOMPARE(g_lastWorldHotspotId, QStringLiteral("hotspot_main"));
			resetTestState();
		}

		void nativeOutputRendererMiniWindowBodyWithoutHotspotDoesNotBlockTextSelection()
		{
			resetTestState();

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.resize(760, 460);
			view.show();
			QCoreApplication::processEvents();

			view.appendOutputText(QStringLiteral("native-selection-pass-through-target"), true);
			QCoreApplication::processEvents();

			QSplitter *outputSplitter = findOutputSplitter(view);
			QVERIFY(outputSplitter);
			QWidget *outputStack = outputSplitter->parentWidget();
			QVERIFY(outputStack);
			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);

			const QRect viewportInStack(viewport->mapTo(outputStack, QPoint(0, 0)), viewport->size());
			QVERIFY(!viewportInStack.isEmpty());
			appendTestMiniWindow(QStringLiteral("native-pass-through-window"), viewportInStack, 0,
			                     QColor(0, 0, 0, 0));
			outputStack->update();
			QCoreApplication::processEvents();

			const QPoint dragStart(8, qBound(8, viewport->height() / 3, viewport->height() - 12));
			const QPoint dragEnd(qBound(16, dragStart.x() + 240, qMax(16, viewport->width() - 8)),
			                     dragStart.y());
			QVERIFY(viewport->rect().contains(dragStart));
			QVERIFY(viewport->rect().contains(dragEnd));

			QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, dragStart);
			QTest::mouseMove(viewport, dragEnd, 5);
			QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, dragEnd);

			QTRY_VERIFY(view.hasOutputSelection());
			QTRY_VERIFY(view.outputSelectionText().contains(QStringLiteral("selection-pass-through")));
			resetTestState();
		}

		void mxpContextMenuActionParsingBuildsRightClickEntries()
		{
			const QVector<QPair<QString, QString>> actions = WorldView::parseMxpContextMenuActions(
			    QStringLiteral("examine assistant|consider assistant|attack assistant"),
			    QStringLiteral(
			        "Right mouse click to act|Examine assistant|Consider assistant|Attack assistant"));
			QCOMPARE(actions.size(), 3);
			QCOMPARE(actions.at(0).first, QStringLiteral("examine assistant"));
			QCOMPARE(actions.at(0).second, QStringLiteral("Examine assistant"));
			QCOMPARE(actions.at(1).first, QStringLiteral("consider assistant"));
			QCOMPARE(actions.at(1).second, QStringLiteral("Consider assistant"));
			QCOMPARE(actions.at(2).first, QStringLiteral("attack assistant"));
			QCOMPARE(actions.at(2).second, QStringLiteral("Attack assistant"));
		}

		void runtimeSettingsRebuildAttributeKeysExcludeWrapAndHyperlinkPresentation()
		{
			const QSet<QString> &rebuildKeys = WorldView::runtimeSettingsRebuildAttributeKeys();
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
			runtimeLine.text       = QStringLiteral("runtime-only-line");
			runtimeLine.flags      = WorldRuntime::LineOutput;
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

		void restoreOutputFromPersistedLinesPopulatesCompleteBuffer()
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
				entry.text       = QStringLiteral("lazy-%1").arg(i, 3, 10, QLatin1Char('0'));
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				lines.push_back(entry);
			}

			view.restoreOutputFromPersistedLines(lines);
			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("lazy-399")));
			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("lazy-000")));

			resetTestState();
		}

		void applyRuntimeSettingsRebuildPreservesEndAnchorForRestoredBuffer()
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
				entry.text       = QStringLiteral("anchor-%1").arg(i, 4, 10, QLatin1Char('0'));
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				lines.push_back(entry);
			}
			g_runtimeLines = lines;

			view.restoreOutputFromPersistedLines(lines);
			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("anchor-0000")));

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *bar = browser->verticalScrollBar();
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
			    "Output viewport drifted away from end after runtime-settings rebuild.");

			resetTestState();
		}

		void resizeEventPreservesEndAnchorWhenViewportWasAtEnd()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("1"));

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.resize(1080, 520);
			view.show();
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 320; ++i)
			{
				const QString line = QStringLiteral("resize-anchor-%1 ").arg(i, 4, 10, QLatin1Char('0')) +
				                     QString(220, QLatin1Char('x'));
				view.appendOutputText(line, true);
			}
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);

			bar->setValue(bar->maximum());
			QCoreApplication::processEvents();

			view.resize(460, 520);

			QTRY_VERIFY2(
			    [&]
			    {
				    QScrollBar *scrollBar = browser->verticalScrollBar();
				    if (!scrollBar)
					    return false;
				    const int endTolerance = qMax(1, scrollBar->pageStep());
				    return scrollBar->value() >= (scrollBar->maximum() - endTolerance);
			    }(),
			    "Output viewport drifted away from end after world-view resize.");

			resetTestState();
		}

		void rebuildAfterRestoreReplacesBufferAtomically()
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
				entry.text       = QStringLiteral("lazy-queue-%1").arg(i, 4, 10, QLatin1Char('0'));
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				lazyLines.push_back(entry);
			}

			view.restoreOutputFromPersistedLines(lazyLines);
			QVector<WorldRuntime::LineEntry> queuedLines;
			queuedLines.reserve(2);
			WorldRuntime::LineEntry queuedEntryA;
			queuedEntryA.text       = QStringLiteral("queued-rebuild-a");
			queuedEntryA.flags      = WorldRuntime::LineOutput;
			queuedEntryA.hardReturn = true;
			queuedLines.push_back(queuedEntryA);
			WorldRuntime::LineEntry queuedEntryB;
			queuedEntryB.text       = QStringLiteral("queued-rebuild-b");
			queuedEntryB.flags      = WorldRuntime::LineOutput;
			queuedEntryB.hardReturn = true;
			queuedLines.push_back(queuedEntryB);

			view.rebuildOutputFromLines(queuedLines);
			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("queued-rebuild-a")));
			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("queued-rebuild-b")));
			QVERIFY(!view.outputLines().contains(QStringLiteral("lazy-queue-2999")));
			QVERIFY(!view.outputLines().contains(QStringLiteral("lazy-queue-0000")));

			resetTestState();
		}

		void nativeRestoreLatestPayloadAppliesAtomically()
		{
			resetTestState();

			WorldView view;
			view.resize(860, 520);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QVector<WorldRuntime::LineEntry> lazyLines;
			lazyLines.reserve(9000);
			for (int i = 0; i < 9000; ++i)
			{
				WorldRuntime::LineEntry entry;
				entry.text       = QStringLiteral("native-backfill-%1").arg(i, 5, 10, QLatin1Char('0'));
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				lazyLines.push_back(entry);
			}
			view.restoreOutputFromPersistedLines(lazyLines);

			QVector<WorldRuntime::LineEntry> queuedRestoreLines;
			queuedRestoreLines.reserve(6000);
			for (int i = 0; i < 6000; ++i)
			{
				WorldRuntime::LineEntry entry;
				entry.text       = QStringLiteral("native-queued-restore-%1").arg(i, 5, 10, QLatin1Char('0'));
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				queuedRestoreLines.push_back(entry);
			}
			view.restoreOutputFromPersistedLines(queuedRestoreLines);

			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("native-queued-restore-05999")));
			QVERIFY(view.outputLines().contains(QStringLiteral("native-queued-restore-00000")));
			QVERIFY(!view.outputLines().contains(QStringLiteral("native-backfill-08999")));
			resetTestState();
		}

		void nativeRestoreSequentialCallsUseLatestPayload()
		{
			resetTestState();

			WorldView view;
			view.resize(860, 520);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QVector<WorldRuntime::LineEntry> firstRestoreLines;
			firstRestoreLines.reserve(24000);
			for (int i = 0; i < 24000; ++i)
			{
				WorldRuntime::LineEntry entry;
				entry.text       = QStringLiteral("native-inflight-a-%1").arg(i, 5, 10, QLatin1Char('0'));
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				firstRestoreLines.push_back(entry);
			}

			QVector<WorldRuntime::LineEntry> secondRestoreLines;
			for (int i = 0; i < 3; ++i)
			{
				WorldRuntime::LineEntry entry;
				entry.text       = QStringLiteral("native-inflight-b-%1").arg(i);
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				secondRestoreLines.push_back(entry);
			}

			view.restoreOutputFromPersistedLines(firstRestoreLines);
			view.restoreOutputFromPersistedLines(secondRestoreLines);

			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("native-inflight-b-2")));
			QVERIFY(!view.outputLines().contains(QStringLiteral("native-inflight-a-23999")));
			resetTestState();
		}

		void nativeRestoreAppendRemainsAfterLargeRestore()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("0"));

			WorldView view;
			view.resize(860, 520);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QVector<WorldRuntime::LineEntry> restoredLines;
			restoredLines.reserve(22000);
			for (int i = 0; i < 22000; ++i)
			{
				WorldRuntime::LineEntry entry;
				entry.text  = QStringLiteral("native-inflight-restore-%1").arg(i, 5, 10, QLatin1Char('0'));
				entry.flags = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				entry.lineNumber = i + 1;
				restoredLines.push_back(entry);
			}
			g_runtimeLines = restoredLines;

			view.restoreOutputFromPersistedLines(restoredLines);
			view.appendOutputText(QStringLiteral("native-live-tail-line"), true);

			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("native-live-tail-line")));
			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("native-inflight-restore-21999")));
			resetTestState();
		}

		void nativeRestoreStyleChangeKeepsLatestSnapshot()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("output_text_colour"), QStringLiteral("#ff0000"));

			WorldView view;
			view.resize(860, 520);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QVector<WorldRuntime::LineEntry> restoredLines;
			restoredLines.reserve(32000);
			for (int i = 0; i < 32000; ++i)
			{
				WorldRuntime::LineEntry entry;
				entry.text       = QStringLiteral("native-epoch-%1").arg(i, 5, 10, QLatin1Char('0'));
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				entry.lineNumber = i + 1;
				restoredLines.push_back(entry);
			}
			g_runtimeLines = restoredLines;

			view.restoreOutputFromPersistedLines(restoredLines);
			g_worldAttrs.insert(QStringLiteral("output_text_colour"), QStringLiteral("#00ff00"));
			view.applyRuntimeSettingsWithoutOutputRebuild();

			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("native-epoch-31999")));
			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("native-epoch-00000")));
			resetTestState();
		}

		void restoreReplacementDoesNotRetainPreviousBuffer()
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
				entry.text       = QStringLiteral("initial-restore-%1").arg(i, 4, 10, QLatin1Char('0'));
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				initialRestoreLines.push_back(entry);
			}

			view.restoreOutputFromPersistedLines(initialRestoreLines);

			QVector<WorldRuntime::LineEntry> queuedRebuildLines;
			queuedRebuildLines.reserve(2);
			WorldRuntime::LineEntry queuedEntryA;
			queuedEntryA.text       = QStringLiteral("queued-lazy-a");
			queuedEntryA.flags      = WorldRuntime::LineOutput;
			queuedEntryA.hardReturn = true;
			queuedRebuildLines.push_back(queuedEntryA);
			WorldRuntime::LineEntry queuedEntryB;
			queuedEntryB.text       = QStringLiteral("queued-lazy-b");
			queuedEntryB.flags      = WorldRuntime::LineOutput;
			queuedEntryB.hardReturn = true;
			queuedRebuildLines.push_back(queuedEntryB);

			view.restoreOutputFromPersistedLines(queuedRebuildLines);
			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("queued-lazy-a")));
			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("queued-lazy-b")));
			QVERIFY(!view.outputLines().contains(QStringLiteral("initial-restore-2999")));
			QVERIFY(!view.outputLines().contains(QStringLiteral("initial-restore-0000")));

			resetTestState();
		}

		void restorePathAppliesPersistedOutputAndHistoryTogether()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("arrows_change_history"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(860, 520);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QVector<WorldRuntime::LineEntry> restoredLines;
			restoredLines.reserve(2600);
			for (int i = 0; i < 2600; ++i)
			{
				WorldRuntime::LineEntry entry;
				entry.text       = QStringLiteral("restore-combo-%1").arg(i, 4, 10, QLatin1Char('0'));
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				restoredLines.push_back(entry);
			}

			view.restoreOutputFromPersistedLines(restoredLines);

			const QStringList restoredHistory{
			    QStringLiteral("north"),
			    QStringLiteral("east"),
			    QStringLiteral("south"),
			};
			view.setCommandHistoryList(restoredHistory);
			QCOMPARE(view.commandHistoryList(), restoredHistory);

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus();
			QCoreApplication::processEvents();

			QTest::keyClick(input, Qt::Key_Up);
			QCOMPARE(view.inputText(), QStringLiteral("south"));
			QTest::keyClick(input, Qt::Key_Up);
			QCOMPARE(view.inputText(), QStringLiteral("east"));

			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("restore-combo-0000")));
			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("restore-combo-2599")));
			QCOMPARE(view.commandHistoryList(), restoredHistory);

			resetTestState();
		}

		void outputRestoreDoesNotClearExistingCommandHistory()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("arrows_change_history"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(860, 520);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			view.addToHistoryForced(QStringLiteral("look"));
			view.addToHistoryForced(QStringLiteral("north"));
			const QStringList                historyBefore = view.commandHistoryList();

			QVector<WorldRuntime::LineEntry> restoredLines;
			restoredLines.reserve(900);
			for (int i = 0; i < 900; ++i)
			{
				WorldRuntime::LineEntry entry;
				entry.text       = QStringLiteral("output-only-%1").arg(i, 4, 10, QLatin1Char('0'));
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				restoredLines.push_back(entry);
			}

			view.restoreOutputFromPersistedLines(restoredLines);
			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("output-only-0000")));
			QCOMPARE(view.commandHistoryList(), historyBefore);

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus();
			QCoreApplication::processEvents();
			QTest::keyClick(input, Qt::Key_Up);
			QCOMPARE(view.inputText(), QStringLiteral("north"));

			resetTestState();
		}

		void historyRestoreDoesNotMutateOutputBuffer()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("arrows_change_history"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();

			view.appendOutputText(QStringLiteral("history-only-output-a"), true);
			view.appendOutputText(QStringLiteral("history-only-output-b"), true);
			view.appendOutputText(QStringLiteral("history-only-output-c"), true);
			QCoreApplication::processEvents();

			const QStringList outputBefore = view.outputLines();

			const QStringList restoredHistory{
			    QStringLiteral("cast heal"),
			    QStringLiteral("drink potion"),
			};
			view.setCommandHistoryList(restoredHistory);
			QCOMPARE(view.commandHistoryList(), restoredHistory);
			QCOMPARE(view.outputLines(), outputBefore);

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus();
			QCoreApplication::processEvents();
			QTest::keyClick(input, Qt::Key_Up);
			QCOMPARE(view.inputText(), QStringLiteral("drink potion"));

			resetTestState();
		}

		void nativeRestoreFromPersistedLinesRendersWithoutDocumentFallback()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("0"));

			WorldView view;
			view.resize(860, 520);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			QVector<WorldRuntime::LineEntry> restoredLines;
			restoredLines.reserve(1800);
			for (int i = 0; i < 1800; ++i)
			{
				WorldRuntime::LineEntry entry;
				entry.text       = QStringLiteral("persist-native-%1").arg(i, 4, 10, QLatin1Char('0'));
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				restoredLines.push_back(entry);
			}
			g_runtimeLines = restoredLines;

			view.restoreOutputFromPersistedLines(restoredLines);
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(),
			             restoredLines.size());
			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("persist-native-0000")));
			QTRY_VERIFY(view.outputLines().contains(QStringLiteral("persist-native-1799")));
			resetTestState();
		}

		void nativeRestoreFromPersistedLinesPreservesHyperlinkAnchors()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("0"));

			WorldRuntime::LineEntry line;
			line.text       = QStringLiteral("assistant");
			line.flags      = WorldRuntime::LineOutput;
			line.hardReturn = true;
			line.lineNumber = 1;
			WorldRuntime::StyleSpan span;
			span.length     = boundedSizeToInt(line.text.size());
			span.actionType = WorldRuntime::ActionSend;
			span.action     = QStringLiteral("examine assistant");
			span.hint       = QStringLiteral("Examine assistant");
			line.spans.push_back(span);

			QVector<WorldRuntime::LineEntry> restoredLines{line};
			g_runtimeLines = restoredLines;

			WorldView view;
			view.resize(860, 520);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.restoreOutputFromPersistedLines(restoredLines);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			const QPoint anchorPoint = findHyperlinkPoint(view, *browser, span.action);
			QVERIFY2(anchorPoint.x() >= 0 && anchorPoint.y() >= 0,
			         "Expected preserved hyperlink hit target after native restore.");

			QSignalSpy activatedSpy(&view, &WorldView::hyperlinkActivated);
			QTest::mouseClick(browser->viewport(), Qt::LeftButton, Qt::NoModifier, anchorPoint);
			QTRY_COMPARE(activatedSpy.count(), 1);
			QCOMPARE(QUrl::fromPercentEncoding(activatedSpy.at(0).at(0).toString().toUtf8()), span.action);
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

			const QColor  targetColour(QStringLiteral("#12ab34"));
			const QString oldestHref = QStringLiteral("https://example.org/inplace/000");
			QString       newestHref;
			for (int i = 0; i < 160; ++i)
			{
				const QString lineText = QStringLiteral("link-%1").arg(i, 3, 10, QLatin1Char('0'));
				const QString href =
				    QStringLiteral("https://example.org/inplace/%1").arg(i, 3, 10, QLatin1Char('0'));
				newestHref = href;
				WorldRuntime::StyleSpan span;
				span.length     = boundedSizeToInt(lineText.size());
				span.actionType = WorldRuntime::ActionHyperlink;
				span.action     = href;
				view.appendOutputTextStyled(lineText, {span}, true);
			}
			QCoreApplication::processEvents();

			const QStringList linesBefore = view.outputLines();
			QVERIFY(linesBefore.contains(QStringLiteral("link-000")));
			QVERIFY(linesBefore.contains(QStringLiteral("link-159")));

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *scrollBar = browser->verticalScrollBar();
			QVERIFY(scrollBar);
			scrollBar->setValue(scrollBar->maximum());
			QCoreApplication::processEvents();
			const QPoint newestPointBefore = findHyperlinkPoint(view, *browser, newestHref);
			QVERIFY(newestPointBefore.x() >= 0 && newestPointBefore.y() >= 0);

			g_worldAttrs.insert(QStringLiteral("use_custom_link_colour"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("underline_hyperlinks"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("hyperlink_colour"), targetColour.name());
			g_runtimeLines.clear();

			view.applyRuntimeSettingsWithoutOutputRebuild();

			const QStringList linesAfter = view.outputLines();
			QCOMPARE(linesAfter, linesBefore);
			scrollBar->setValue(scrollBar->maximum());
			QCoreApplication::processEvents();
			const QPoint newestPointAfter = findHyperlinkPoint(view, *browser, newestHref);
			QVERIFY(newestPointAfter.x() >= 0 && newestPointAfter.y() >= 0);

			QSignalSpy activatedSpy(&view, &WorldView::hyperlinkActivated);
			QTest::mouseClick(browser->viewport(), Qt::LeftButton, Qt::NoModifier, newestPointAfter);
			QTRY_COMPARE(activatedSpy.count(), 1);
			QCOMPARE(QUrl::fromPercentEncoding(activatedSpy.at(0).at(0).toString().toUtf8()), newestHref);

			resetTestState();
		}

		// NOLINTEND(readability-convert-member-functions-to-static)
};
QTEST_MAIN(tst_WorldView_Basic)

#if __has_include("tst_WorldView_Basic.moc")
#include "tst_WorldView_Basic.moc"
#endif
