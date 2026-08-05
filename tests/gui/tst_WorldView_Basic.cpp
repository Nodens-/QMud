/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldView_Basic.cpp
 * Role: QTest coverage for WorldView Basic behavior.
 */

#include "AcceleratorUtils.h"
#include "AnsiSgrParseUtils.h"
#include "AppController.h"
#include "MiniWindowUtils.h"
#include "NativePluginRegistry.h"
#include "OutputWrapUtils.h"
#include "TelnetProcessor.h"
#include "WorldOptions.h"
#include "WorldView.h"
#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QAbstractScrollArea>
#include <QAccessible>
// ReSharper disable once CppUnusedIncludeDirective
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialog>
#include <QElapsedTimer>
#include <QImage>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
// ReSharper disable once CppUnusedIncludeDirective
#include <QWheelEvent>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <memory>

namespace
{
	using QTextBrowser = QAbstractScrollArea;
	using QMudOutputWrapUtils::localOutputWrapConfig;
	using QMudOutputWrapUtils::splitOutputTextAtLineBreaks;
	using QMudOutputWrapUtils::wrapPlainLineForColumn;
	using QMudOutputWrapUtils::wrapStyledLineForColumn;
	QMap<QString, QString>                     g_worldAttrs;
	QMap<QString, QString>                     g_worldMultilineAttrs;
	QMap<QString, QVariant>                    g_globalOptions;
	IndexedRingBuffer<WorldRuntime::LineEntry> g_runtimeLines;
	QString                                    g_pendingIncomingPartialText;
	QVector<WorldRuntime::StyleSpan>           g_pendingIncomingPartialSpans;
	int                                        g_pendingIncomingPartialCommitCount{0};
	QList<WorldRuntime::Macro>                 g_testMacros;
	WorldRuntime::TextRectangleSettings        g_textRectangle;
	int                                        g_outputFontHeight{0};
	int                                        g_drawOutputNotifyCount{0};
	int                                        g_lastDrawOutputFirstLine{0};
	int                                        g_lastDrawOutputAdjustedScroll{0};
	int                                        g_worldOutputResizedNotifyCount{0};
	QVector<MiniWindow>                        g_testMiniWindows;
	int                                        g_worldHotspotCallbackCount{0};
	QString                                    g_lastWorldHotspotFunction;
	QString                                    g_lastWorldHotspotId;
	long                                       g_lastWorldHotspotFlags{0};
	bool                                       g_lastWorldHotspotQueueWhenBusy{false};
	QVector<QString>                           g_worldHotspotCallbackFunctions;
	QVector<bool>                              g_worldHotspotCallbackQueueWhenBusy;
	QPoint                                     g_lastResizeHotspotPressOffset;
	QSize                                      g_expectedReleaseResizeSize;
	QPoint                                     g_expectedReleaseMousePosition;
	int                                        g_resizeMoveCallbackCount{0};
	bool                                       g_releaseCallbackSawFlushedResize{false};
	WorldView                                 *g_runtimeView{nullptr};
	unsigned short                             g_currentActionSource{WorldRuntime::eUnknownActionSource};
	bool                                       g_connected{false};
	bool                                       g_nawsNegotiated{false};
	bool                                       g_useFakeAppController{false};
	bool                                       g_hasMushReaderLiveSpeechOwner{false};
	bool                                       g_qtAccessibilitySpeechEnabled{true};
	QHash<quint64, quint16>                    g_virtualKeyMap;
	QHash<qint64, int>                         g_acceleratorCommands;
	int                                        g_acceleratorExecutionCount{0};
	int                                        g_lastExecutedAcceleratorCommand{-1};
	int                                        g_sendCommandCount{0};
	int                                        g_executeCommandCount{0};
	QString                                    g_lastExecutedCommand;
	bool                                       g_lastUserMacroHistory{false};
	unsigned short g_actionSourceDuringExecute{WorldRuntime::eUnknownActionSource};
	QString        g_wordUnderMenu;
	bool           g_wordUnderMenuResolved{false};

	struct AccessibleTextInsertRecord
	{
			QObject *object{nullptr};
			int      position{0};
			QString  text;
	};

	struct AccessibleTextUpdateRecord
	{
			QObject *object{nullptr};
			int      position{0};
			QString  removedText;
			QString  insertedText;
	};

	struct AccessibleAnnouncementRecord
	{
			QObject *object{nullptr};
			QString  message;
	};

	struct AccessibleTextCursorRecord
	{
			QObject *object{nullptr};
			int      position{0};
	};

	struct AccessibleTextSelectionRecord
	{
			QObject *object{nullptr};
			int      start{0};
			int      end{0};
	};

	QVector<AccessibleTextInsertRecord>    g_accessibleTextInsertRecords;
	QVector<AccessibleTextUpdateRecord>    g_accessibleTextUpdateRecords;
	QVector<AccessibleAnnouncementRecord>  g_accessibleAnnouncementRecords;
	QVector<AccessibleTextCursorRecord>    g_accessibleTextCursorRecords;
	QVector<AccessibleTextSelectionRecord> g_accessibleTextSelectionRecords;
	int                                    g_accessibleValueChangedCount{0};

	int                                    sizeToInt(const qsizetype value)
	{
		constexpr qsizetype kMin = 0;
		constexpr qsizetype kMax = std::numeric_limits<int>::max();
		return static_cast<int>(qBound(kMin, value, kMax));
	}

	int stringIndexToInt(const qsizetype value)
	{
		constexpr qsizetype kMin = -1;
		constexpr qsizetype kMax = std::numeric_limits<int>::max();
		return static_cast<int>(qBound(kMin, value, kMax));
	}

	void captureAccessibleUpdate(QAccessibleEvent *event)
	{
		if (!event)
			return;
		if (event->type() == QAccessible::ValueChanged)
		{
			++g_accessibleValueChangedCount;
			return;
		}
		if (event->type() == QAccessible::TextCaretMoved)
		{
			const auto *cursorEvent = dynamic_cast<QAccessibleTextCursorEvent *>(event);
			if (!cursorEvent)
				return;
			g_accessibleTextCursorRecords.push_back({event->object(), cursorEvent->cursorPosition()});
			return;
		}
		if (event->type() == QAccessible::TextInserted)
		{
			const auto *insertEvent = dynamic_cast<QAccessibleTextInsertEvent *>(event);
			if (!insertEvent)
				return;
			g_accessibleTextInsertRecords.push_back(
			    {event->object(), insertEvent->changePosition(), insertEvent->textInserted()});
			return;
		}
		if (event->type() == QAccessible::TextUpdated)
		{
			const auto *updateEvent = dynamic_cast<QAccessibleTextUpdateEvent *>(event);
			if (!updateEvent)
				return;
			g_accessibleTextUpdateRecords.push_back({event->object(), updateEvent->changePosition(),
			                                         updateEvent->textRemoved(),
			                                         updateEvent->textInserted()});
			return;
		}
		if (event->type() == QAccessible::TextSelectionChanged)
		{
			const auto *selectionEvent = dynamic_cast<QAccessibleTextSelectionEvent *>(event);
			if (!selectionEvent)
				return;
			g_accessibleTextSelectionRecords.push_back(
			    {event->object(), selectionEvent->selectionStart(), selectionEvent->selectionEnd()});
			return;
		}
		if (event->type() == QAccessible::Announcement)
		{
			const auto *announcementEvent = dynamic_cast<QAccessibleAnnouncementEvent *>(event);
			if (!announcementEvent)
				return;
			g_accessibleAnnouncementRecords.push_back({event->object(), announcementEvent->message()});
		}
	}

	class ScopedAccessibleUpdateCapture
	{
		public:
			ScopedAccessibleUpdateCapture()
			    : m_previousActive(QAccessible::isActive()),
			      m_previousHandler(QAccessible::installUpdateHandler(captureAccessibleUpdate))
			{
				g_accessibleTextInsertRecords.clear();
				g_accessibleTextUpdateRecords.clear();
				g_accessibleAnnouncementRecords.clear();
				g_accessibleTextCursorRecords.clear();
				g_accessibleTextSelectionRecords.clear();
				g_accessibleValueChangedCount = 0;
				QAccessible::setActive(true);
			}

			~ScopedAccessibleUpdateCapture()
			{
				QAccessible::installUpdateHandler(m_previousHandler);
				QAccessible::setActive(m_previousActive);
				g_accessibleTextInsertRecords.clear();
				g_accessibleTextUpdateRecords.clear();
				g_accessibleAnnouncementRecords.clear();
				g_accessibleTextCursorRecords.clear();
				g_accessibleTextSelectionRecords.clear();
				g_accessibleValueChangedCount = 0;
			}

		private:
			bool                       m_previousActive{false};
			QAccessible::UpdateHandler m_previousHandler{nullptr};
	};

	class ScopedAccessibleActiveState
	{
		public:
			explicit ScopedAccessibleActiveState(const bool active)
			    : m_previousActive(QAccessible::isActive())
			{
				QAccessible::setActive(active);
			}

			~ScopedAccessibleActiveState()
			{
				QAccessible::setActive(m_previousActive);
			}

		private:
			bool m_previousActive{false};
	};

	AppController *fakeAppControllerPointer()
	{
		return reinterpret_cast<AppController *>(static_cast<quintptr>(1));
	}

	WorldRuntime *fakeRuntimePointer()
	{
		return reinterpret_cast<WorldRuntime *>(static_cast<quintptr>(1));
	}

	void setFakeMushReaderLiveSpeechOwner(const bool enable)
	{
		g_hasMushReaderLiveSpeechOwner = enable;
		QMudNativePluginRegistry::setMushReaderPluginEnabled(fakeRuntimePointer(), enable);
	}

	const QMap<QString, QString> &emptyAttributes()
	{
		static const QMap<QString, QString> attrs;
		return attrs;
	}

	const IndexedRingBuffer<WorldRuntime::LineEntry> &lineStorage()
	{
		return g_runtimeLines;
	}

	const QList<WorldRuntime::Macro> &macroStorage()
	{
		return g_testMacros;
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
		g_pendingIncomingPartialText.clear();
		g_pendingIncomingPartialSpans.clear();
		g_pendingIncomingPartialCommitCount = 0;
		g_testMacros.clear();
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
		g_lastWorldHotspotFlags         = 0;
		g_lastWorldHotspotQueueWhenBusy = false;
		g_worldHotspotCallbackFunctions.clear();
		g_worldHotspotCallbackQueueWhenBusy.clear();
		g_lastResizeHotspotPressOffset    = {};
		g_expectedReleaseResizeSize       = {};
		g_expectedReleaseMousePosition    = {};
		g_resizeMoveCallbackCount         = 0;
		g_releaseCallbackSawFlushedResize = false;
		g_runtimeView                     = nullptr;
		g_currentActionSource             = WorldRuntime::eUnknownActionSource;
		g_connected                       = false;
		g_nawsNegotiated                  = false;
		g_useFakeAppController            = false;
		g_hasMushReaderLiveSpeechOwner    = false;
		g_qtAccessibilitySpeechEnabled    = true;
		g_virtualKeyMap.clear();
		g_acceleratorCommands.clear();
		g_acceleratorExecutionCount      = 0;
		g_lastExecutedAcceleratorCommand = -1;
		g_sendCommandCount               = 0;
		g_executeCommandCount            = 0;
		g_lastExecutedCommand.clear();
		g_lastUserMacroHistory      = false;
		g_actionSourceDuringExecute = WorldRuntime::eUnknownActionSource;
		g_wordUnderMenu.clear();
		g_wordUnderMenuResolved = false;
		g_accessibleTextInsertRecords.clear();
		g_accessibleTextUpdateRecords.clear();
		g_accessibleAnnouncementRecords.clear();
		g_accessibleTextCursorRecords.clear();
		g_accessibleTextSelectionRecords.clear();
		g_accessibleValueChangedCount = 0;
		QMudNativePluginRegistry::discardRuntimeState(fakeRuntimePointer());
		QMudNativePluginRegistry::setTestSpeechSink({});
	}

	class ScopedTestStateReset
	{
		public:
			ScopedTestStateReset() = default;

			~ScopedTestStateReset()
			{
				resetTestState();
			}
	};

	qint64 makeAcceleratorMapKey(const Qt::Key key, const Qt::KeyboardModifiers modifiers,
	                             const quint16 virtualKey, const bool keypad = false)
	{
		quint32 virt = AcceleratorUtils::kVirtKeyFlag | AcceleratorUtils::kNoInvertFlag;
		if ((modifiers & Qt::ShiftModifier) != 0)
			virt |= AcceleratorUtils::kShiftFlag;
		if ((modifiers & Qt::ControlModifier) != 0)
			virt |= AcceleratorUtils::kControlFlag;
		if ((modifiers & Qt::AltModifier) != 0)
			virt |= AcceleratorUtils::kAltFlag;
		const quint64 mapId = (static_cast<quint64>(static_cast<quint32>(key)) << 1) | (keypad ? 1ULL : 0ULL);
		g_virtualKeyMap.insert(mapId, virtualKey);
		return (static_cast<qint64>(virt) << 16) | virtualKey;
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
		const auto [topBrowser, bottomBrowser] = findSplitOutputBrowsers(view);
		QTextBrowser *bestBrowser              = nullptr;
		int           bestViewportArea         = -1;
		auto          viewportArea             = [](const QTextBrowser *browser)
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

		const std::array<QTextBrowser *, 2> browsers{topBrowser, bottomBrowser};
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
		if (!bestBrowser)
			bestBrowser = topBrowser ? topBrowser : bottomBrowser;
		return bestBrowser;
	}

	WorldRuntime::Macro makeMacro(const QString &name, const QString &type, const QString &send)
	{
		WorldRuntime::Macro macro;
		macro.attributes.insert(QStringLiteral("name"), name);
		macro.attributes.insert(QStringLiteral("type"), type);
		macro.children.insert(QStringLiteral("send"), send);
		return macro;
	}

	QWidget *findNativeOutputCanvas(const WorldView &view)
	{
		return view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
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

	QPoint findLineInformationPoint(const QTextBrowser &browser)
	{
		if (!browser.viewport())
			return {-1, -1};

		QWidget *root = browser.window();
		auto    *nativeCanvas =
		    root ? root->findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas")) : nullptr;
		QElapsedTimer readyTimer;
		readyTimer.start();
		constexpr qint64 kMaxReadyWaitMs       = 500;
		constexpr qint64 kMissingCanvasGraceMs = 50;
		while (readyTimer.elapsed() < kMaxReadyWaitMs)
		{
			if (!nativeCanvas && root)
				nativeCanvas = root->findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));

			const bool viewReady = root && root->isVisible() && browser.viewport()->isVisible() &&
			                       browser.viewport()->width() > 1 && browser.viewport()->height() > 1;
			if (viewReady && nativeCanvas && nativeCanvas->isVisible())
				break;
			if (viewReady && !nativeCanvas && readyTimer.elapsed() >= kMissingCanvasGraceMs)
				break;
			QCoreApplication::processEvents();
			QTest::qWait(1);
		}

		const QRect area = browser.viewport()->rect();
		const int   probeBottom =
		    qMin(area.bottom(), area.top() + qMax(24, QFontMetrics(browser.font()).height() * 6));
		const int probeRight = qMin(area.right(), area.left() + 320);

		auto      probePoint = [&browser](const QPoint &point)
		{
			QToolTip::hideText();
			QCoreApplication::processEvents();
			QTest::mouseMove(browser.viewport(), point);
			QCoreApplication::processEvents();
			QCoreApplication::processEvents();
			return QToolTip::text().contains(QStringLiteral("Line "));
		};

		const int lineHeight = qMax(1, QFontMetrics(browser.font()).height());
		const int candidateY = qMin(probeBottom, area.top() + (lineHeight / 2));
		for (int x = area.left() + 2; x <= probeRight; x += 12)
		{
			if (probePoint({x, candidateY}))
				return {x, candidateY};
		}

		for (int y = area.top(); y <= probeBottom; y += 4)
		{
			for (int x = area.left(); x <= probeRight; x += 4)
			{
				if (probePoint({x, y}))
					return {x, y};
			}
		}
		return {-1, -1};
	}

	void sendMiniWindowMouseMove(QWidget *viewport, const QWidget *outputStack, const QPoint &pointInStack)
	{
		const QPoint local  = viewport->mapFrom(outputStack, pointInStack);
		const QPoint global = viewport->mapToGlobal(local);
		QMouseEvent  moveEvent(QEvent::MouseMove, QPointF(local), QPointF(local), QPointF(global),
		                       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
		QCoreApplication::sendEvent(viewport, &moveEvent);
		QCoreApplication::processEvents();
	}

	void sendMiniWindowMouseRelease(QWidget *viewport, const QWidget *outputStack, const QPoint &pointInStack)
	{
		const QPoint local  = viewport->mapFrom(outputStack, pointInStack);
		const QPoint global = viewport->mapToGlobal(local);
		QMouseEvent  releaseEvent(QEvent::MouseButtonRelease, QPointF(local), QPointF(local), QPointF(global),
		                          Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
		QCoreApplication::sendEvent(viewport, &releaseEvent);
		QCoreApplication::processEvents();
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
		QImage surface(qMax(1, rect.width()), qMax(1, rect.height()), QImage::Format_ARGB32_Premultiplied);
		surface.fill(fill);
		window.setBackingSurface(surface);
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

	WorldRuntime::LineEntry makeRuntimeLine(const QString &text, const int flags, const bool hardReturn,
	                                        const qint64 lineNumber)
	{
		WorldRuntime::LineEntry entry;
		entry.text       = text;
		entry.flags      = flags;
		entry.hardReturn = hardReturn;
		entry.lineNumber = lineNumber;
		return entry;
	}

	WorldRuntime::StyleSpan makeFullSpan(const QString &text, const QColor &fore, const QColor &back)
	{
		WorldRuntime::StyleSpan span;
		span.length  = boundedSizeToInt(text.size());
		span.fore    = fore;
		span.back    = back;
		span.changed = true;
		return span;
	}

	QVector<WorldRuntime::StyleSpan> spansFromStyledChunks(const QVector<QMudStyledChunk> &chunks,
	                                                       QString                        &plainText)
	{
		QVector<WorldRuntime::StyleSpan> spans;
		plainText.clear();
		for (const QMudStyledChunk &chunk : chunks)
		{
			if (chunk.text.isEmpty())
				continue;
			plainText += chunk.text;

			WorldRuntime::StyleSpan span;
			span.length     = boundedSizeToInt(chunk.text.size());
			span.bold       = chunk.state.bold;
			span.underline  = chunk.state.underline;
			span.italic     = chunk.state.italic;
			span.blink      = chunk.state.blink;
			span.strike     = chunk.state.strike;
			span.actionType = chunk.state.actionType;
			span.action     = chunk.state.action;
			span.hint       = chunk.state.hint;
			span.variable   = chunk.state.variable;
			span.startTag   = chunk.state.startTag;
			const QColor fore(chunk.state.fore);
			const QColor back(chunk.state.back);
			if (fore.isValid())
			{
				span.fore    = fore;
				span.changed = true;
			}
			if (back.isValid())
			{
				span.back    = back;
				span.changed = true;
			}
			if (span.changed || span.bold || span.underline || span.italic || span.blink || span.strike ||
			    span.actionType != WorldRuntime::ActionNone)
				spans.push_back(span);
		}
		return spans;
	}

	bool colorsMatchExactly(const QColor &lhs, const QColor &rhs)
	{
		return lhs.red() == rhs.red() && lhs.green() == rhs.green() && lhs.blue() == rhs.blue() &&
		       lhs.alpha() == rhs.alpha();
	}

	bool colorsMatchWithinTolerance(const QColor &lhs, const QColor &rhs, const int tolerance)
	{
		return qAbs(lhs.red() - rhs.red()) <= tolerance && qAbs(lhs.green() - rhs.green()) <= tolerance &&
		       qAbs(lhs.blue() - rhs.blue()) <= tolerance && qAbs(lhs.alpha() - rhs.alpha()) <= tolerance;
	}

	int countPixelsNearColor(const QImage &image, const QColor &target, const int tolerance = 8)
	{
		int count = 0;
		for (int y = 0; y < image.height(); ++y)
		{
			for (int x = 0; x < image.width(); ++x)
			{
				if (colorsMatchWithinTolerance(image.pixelColor(x, y), target, tolerance))
					++count;
			}
		}
		return count;
	}

	int countWidgetPixelsNearColor(QWidget *widget, const QRect &rect, const QColor &target,
	                               const int tolerance = 8)
	{
		if (!widget)
			return 0;
		const QRect bounded = rect.intersected(widget->rect());
		if (bounded.isEmpty())
			return 0;
		const QImage image = widget->grab(bounded).toImage().convertToFormat(QImage::Format_ARGB32);
		if (image.isNull())
			return 0;
		return countPixelsNearColor(image, target, tolerance);
	}

	bool widgetRectMostlyMatchesColor(QWidget *widget, const QRect &rect, const QColor &target,
	                                  const int percent = 80)
	{
		if (!widget)
			return false;
		const QRect bounded = rect.intersected(widget->rect());
		if (bounded.isEmpty())
			return false;
		const QImage image = widget->grab(bounded).toImage().convertToFormat(QImage::Format_ARGB32);
		if (image.isNull())
			return false;
		const int totalPixels = image.width() * image.height();
		if (totalPixels <= 0)
			return false;
		return countPixelsNearColor(image, target) * 100 >= totalPixels * percent;
	}

	int fakeMaxOutputLinesLimit()
	{
		bool      ok         = false;
		const int configured = g_worldAttrs.value(QStringLiteral("max_output_lines")).toInt(&ok);
		if (!ok || configured <= 0)
			return 0;
		return qBound(0, configured, 500000);
	}

	void enforceFakeOutputLineLimit()
	{
		const int maxLines = fakeMaxOutputLinesLimit();
		if (maxLines <= 0 || g_runtimeLines.size() <= maxLines)
			return;
		const qsizetype removed = g_runtimeLines.size() - maxLines;
		g_runtimeLines.erase(g_runtimeLines.begin(), g_runtimeLines.begin() + removed);
	}

	void appendFakeRuntimeOutputText(WorldView &view, QString text, QVector<WorldRuntime::StyleSpan> spans,
	                                 const bool note, const bool newLine)
	{
		const bool serverSideWrapActive = g_connected && g_nawsNegotiated;
		const QMudOutputWrapUtils::FixedColumnWrapConfig wrapConfig =
		    (!note && serverSideWrapActive) ? QMudOutputWrapUtils::FixedColumnWrapConfig{}
		                                    : localOutputWrapConfig(g_worldAttrs, serverSideWrapActive, 80);
		if (wrapConfig.enabled && !text.isEmpty())
		{
			if (spans.isEmpty())
				wrapPlainLineForColumn(text, wrapConfig.wrapColumn, wrapConfig.indentParas);
			else
				wrapStyledLineForColumn(text, spans, wrapConfig.wrapColumn, wrapConfig.indentParas);
		}

		const QVector<QMudOutputWrapUtils::OutputLineSegment> segments =
		    splitOutputTextAtLineBreaks(text, spans, newLine);
		for (const QMudOutputWrapUtils::OutputLineSegment &segment : segments)
		{
			if (note)
			{
				if (segment.spans.isEmpty())
					view.appendNoteText(segment.text, segment.hardReturn);
				else
					view.appendNoteTextStyled(segment.text, segment.spans, segment.hardReturn);
			}
			else
			{
				if (segment.spans.isEmpty())
					view.appendOutputText(segment.text, segment.hardReturn);
				else
					view.appendOutputTextStyled(segment.text, segment.spans, segment.hardReturn);
			}
		}
	}

	void setFakeRuntimePendingPartial(QString text, QVector<WorldRuntime::StyleSpan> spans = {})
	{
		g_pendingIncomingPartialText  = std::move(text);
		g_pendingIncomingPartialSpans = std::move(spans);
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

quint16 AcceleratorUtils::qtKeyToVirtualKey(Qt::Key key, bool keypad)
{
	const quint64 mapId = (static_cast<quint64>(static_cast<quint32>(key)) << 1) | (keypad ? 1ULL : 0ULL);
	if (const auto it = g_virtualKeyMap.constFind(mapId); it != g_virtualKeyMap.constEnd())
		return it.value();
	const quint64 fallbackId = static_cast<quint64>(static_cast<quint32>(key)) << 1;
	return g_virtualKeyMap.value(fallbackId, 0);
}

QString AcceleratorUtils::acceleratorToString(quint32, quint16)
{
	return {};
}

qint64 AcceleratorUtils::acceleratorMapKey(const quint32 virt, const quint16 key)
{
	return (static_cast<qint64>(virt) << 16) | key;
}

bool AcceleratorUtils::keySequenceToAcceleratorMapKey(const QKeySequence &sequence, qint64 &mapKey)
{
	if (sequence.isEmpty() || sequence.count() != 1)
		return false;
	const QKeyCombination combination = sequence[0];
	Qt::KeyboardModifiers modifiers   = combination.keyboardModifiers();
	const bool            keypad      = (modifiers & Qt::KeypadModifier) != 0;
	modifiers &= ~Qt::KeypadModifier;
	if ((modifiers & Qt::MetaModifier) != 0)
		return false;
	const quint16 key = AcceleratorUtils::qtKeyToVirtualKey(combination.key(), keypad);
	if (key == 0)
		return false;
	quint32 virt = AcceleratorUtils::kVirtKeyFlag | AcceleratorUtils::kNoInvertFlag;
	if ((modifiers & Qt::ShiftModifier) != 0)
		virt |= AcceleratorUtils::kShiftFlag;
	if ((modifiers & Qt::ControlModifier) != 0)
		virt |= AcceleratorUtils::kControlFlag;
	if ((modifiers & Qt::AltModifier) != 0)
		virt |= AcceleratorUtils::kAltFlag;
	mapKey = AcceleratorUtils::acceleratorMapKey(virt, key);
	return true;
}

bool WorldRuntime::soundBufferReusableForNativeAudio(int) const
{
	return true;
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

unsigned short WorldRuntime::noteStyle() const
{
	return 0;
}

long WorldRuntime::noteColourFore() const
{
	return 0xFFFFFF;
}

long WorldRuntime::noteColourBack() const
{
	return 0x000000;
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

void WorldRuntime::layoutMiniWindows(const QSize &, const QSize &, bool, const QVector<MiniWindow *> *)
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

const IndexedRingBuffer<WorldRuntime::LineEntry> &WorldRuntime::lines() const
{
	return lineStorage();
}

void WorldRuntime::setView(WorldView *view)
{
	g_runtimeView = view;
}

bool WorldRuntime::syncMiniWindowDevicePixelRatioForView()
{
	if (!g_runtimeView)
		return false;

	const double ratio = g_runtimeView->devicePixelRatioF();
	bool         changed{false};
	for (MiniWindow &window : g_testMiniWindows)
	{
		const QSize physicalSize = MiniWindow::backingStoreSize(window.width, window.height, ratio);
		if (!qFuzzyCompare(window.devicePixelRatio, ratio) ||
		    !qFuzzyCompare(window.backingSurfaceDevicePixelRatio(), ratio) ||
		    window.backingSurfaceSize() != physicalSize)
		{
			MiniWindowUtils::setDevicePixelRatio(window, ratio);
			changed = true;
		}
	}
	return changed;
}

WorldView *WorldRuntime::view() const
{
	return g_runtimeView;
}

WorldRuntime::CommandUiSnapshot WorldRuntime::commandUiSnapshot(bool, bool, bool) const
{
	CommandUiSnapshot snapshot;
	if (!g_runtimeView)
		return snapshot;

	snapshot.hasView                    = true;
	snapshot.outputSelectionEndColumn   = g_runtimeView->outputSelectionEndColumn();
	snapshot.outputSelectionEndLine     = g_runtimeView->outputSelectionEndLine();
	snapshot.outputSelectionStartColumn = g_runtimeView->outputSelectionStartColumn();
	snapshot.outputSelectionStartLine   = g_runtimeView->outputSelectionStartLine();
	return snapshot;
}

void WorldRuntime::installPendingPlugins()
{
}

void WorldRuntime::notifyNativePluginStateChanged()
{
}

void WorldRuntime::notifyWorldOutputResized()
{
	++g_worldOutputResizedNotifyCount;
}

bool WorldRuntime::hasMushReaderLiveSpeechOwner() const
{
	return g_hasMushReaderLiveSpeechOwner;
}

bool WorldRuntime::isMushReaderSpeechEnabled() const
{
	return g_hasMushReaderLiveSpeechOwner && QMudNativePluginRegistry::isMushReaderSpeechEnabled(this);
}

bool WorldRuntime::speakMushReaderReviewText(const QString &text) const
{
	return QMudNativePluginRegistry::speakMushReaderReviewText(this, text);
}

bool WorldRuntime::isQtAccessibilitySpeechEnabled() const
{
	return g_qtAccessibilitySpeechEnabled;
}

void WorldRuntime::outputText(const QString &, bool, bool)
{
}

void WorldRuntime::refreshNawsWindowSize()
{
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

bool WorldRuntime::callPluginHotspotFunction(const QString &, const QString &, long, const QString &,
                                             const QString &, bool)
{
	return false;
}

bool WorldRuntime::callWorldHotspotFunction(const QString &functionName, long flags, const QString &hotspotId,
                                            const QString &, const bool queueWhenCallbackLaneBusy)
{
	++g_worldHotspotCallbackCount;
	g_lastWorldHotspotFunction      = functionName;
	g_lastWorldHotspotFlags         = flags;
	g_lastWorldHotspotId            = hotspotId;
	g_lastWorldHotspotQueueWhenBusy = queueWhenCallbackLaneBusy;
	g_worldHotspotCallbackFunctions.push_back(functionName);
	g_worldHotspotCallbackQueueWhenBusy.push_back(queueWhenCallbackLaneBusy);
	if (functionName == QStringLiteral("drag_test_resize_down"))
	{
		for (const MiniWindow &window : g_testMiniWindows)
		{
			if (window.mouseDownHotspot != hotspotId)
				continue;
			g_lastResizeHotspotPressOffset = window.clientMousePosition - window.rect.topLeft();
			break;
		}
	}
	if (functionName == QStringLiteral("drag_test_move"))
	{
		for (MiniWindow &window : g_testMiniWindows)
		{
			if (window.mouseDownHotspot != hotspotId)
				continue;
			QRect moved = window.rect;
			moved.moveCenter(window.clientMousePosition);
			window.rect     = moved;
			window.location = moved.topLeft();
			if (g_runtimeView)
				g_runtimeView->onMiniWindowsChanged();
			break;
		}
	}
	else if (functionName == QStringLiteral("drag_test_resize"))
	{
		++g_resizeMoveCallbackCount;
		for (MiniWindow &window : g_testMiniWindows)
		{
			if (window.mouseDownHotspot != hotspotId)
				continue;
			const QPoint delta  = window.clientMousePosition - window.rect.topLeft();
			const int    width  = qMax(16, delta.x() + 1);
			const int    height = qMax(16, delta.y() + 1);
			window.width        = width;
			window.height       = height;
			window.rect.setSize(QSize(width, height));
			QImage surface(width, height, QImage::Format_ARGB32_Premultiplied);
			surface.fill(QColor(40, 90, 210, 255));
			window.setBackingSurface(surface);
			if (g_runtimeView)
				g_runtimeView->onMiniWindowsChanged();
			break;
		}
	}
	else if (functionName == QStringLiteral("drag_test_release_after_resize"))
	{
		if (!g_testMiniWindows.isEmpty())
		{
			const MiniWindow &window          = g_testMiniWindows.constFirst();
			g_releaseCallbackSawFlushedResize = window.rect.size() == g_expectedReleaseResizeSize &&
			                                    window.clientMousePosition == g_expectedReleaseMousePosition;
		}
	}
	return true;
}

void WorldRuntime::setWordUnderMenu(const QString &value, const bool resolved)
{
	g_wordUnderMenu         = value;
	g_wordUnderMenuResolved = resolved;
}

QString WorldRuntime::wordUnderMenu() const
{
	return g_wordUnderMenu;
}

bool WorldRuntime::wordUnderMenuResolved() const
{
	return g_wordUnderMenuResolved;
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
	++g_sendCommandCount;
	return eOK;
}

int WorldRuntime::executeCommand(const QString &text) const
{
	++g_executeCommandCount;
	g_lastExecutedCommand       = text;
	g_actionSourceDuringExecute = g_currentActionSource;
	return eOK;
}

int WorldRuntime::executeUserMacroSendNow(const QString &text, bool history) const
{
	++g_executeCommandCount;
	g_lastExecutedCommand       = text;
	g_lastUserMacroHistory      = history;
	g_actionSourceDuringExecute = g_currentActionSource;
	return eOK;
}

int WorldRuntime::acceleratorCommandForKey(qint64 key) const
{
	return g_acceleratorCommands.value(key, -1);
}

bool WorldRuntime::executeAcceleratorCommand(int commandId, const QString &)
{
	if (commandId < 0)
		return false;
	++g_acceleratorExecutionCount;
	g_lastExecutedAcceleratorCommand = commandId;
	return true;
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
	enforceFakeOutputLineLimit();
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
	enforceFakeOutputLineLimit();
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

bool WorldRuntime::commitPendingIncomingPartialLine()
{
	if (g_pendingIncomingPartialText.isEmpty() && g_pendingIncomingPartialSpans.isEmpty())
		return false;

	++g_pendingIncomingPartialCommitCount;
	const QString                          text  = std::move(g_pendingIncomingPartialText);
	const QVector<WorldRuntime::StyleSpan> spans = std::move(g_pendingIncomingPartialSpans);
	g_pendingIncomingPartialText.clear();
	g_pendingIncomingPartialSpans.clear();
	if (spans.isEmpty())
		addLine(text, LineOutput, true);
	else
		addLine(text, LineOutput, spans, true);
	QMudNativePluginRegistry::clearMushReaderPartialLine(this);
	return true;
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

		void nativeLayoutHeightIndexRangeUpdateMaintainsBlockSums()
		{
			WorldView::NativeLayoutHeightIndex heightIndex;
			constexpr int                      blockSize   = WorldView::NativeLayoutHeightIndex::kBlockSize;
			constexpr int                      lineCount   = blockSize * 2 + 17;
			constexpr qreal                    lineAdvance = 10.0;
			IndexedRingBuffer<WorldView::NativeLayoutSlot> layoutSlots(lineCount);
			for (WorldView::NativeLayoutSlot &slot : layoutSlots)
				slot.visualRows = 1;

			heightIndex.assign(lineCount, lineAdvance);
			QCOMPARE(heightIndex.totalHeight(), static_cast<qreal>(lineCount) * lineAdvance);

			for (int line = 8; line < 14; ++line)
				layoutSlots[line].visualRows = 2;
			heightIndex.setLayoutSlotHeightRange(8, 14, layoutSlots, lineAdvance);
			QCOMPARE(heightIndex.heightAt(7), lineAdvance);
			QCOMPARE(heightIndex.heightAt(8), lineAdvance * 2.0);
			QCOMPARE(heightIndex.prefixHeightAt(14), (static_cast<qreal>(14) + 6.0) * lineAdvance);

			const qreal afterSingleBlockUpdate = heightIndex.totalHeight();
			heightIndex.setLayoutSlotHeightRange(8, 14, layoutSlots, lineAdvance);
			QCOMPARE(heightIndex.totalHeight(), afterSingleBlockUpdate);

			for (int line = blockSize - 2; line < blockSize + 3; ++line)
				layoutSlots[line].visualRows = 3;
			heightIndex.setLayoutSlotHeightRange(blockSize - 2, blockSize + 3, layoutSlots, lineAdvance);
			QCOMPARE(heightIndex.heightAt(blockSize - 3), lineAdvance);
			QCOMPARE(heightIndex.heightAt(blockSize - 2), lineAdvance * 3.0);
			QCOMPARE(heightIndex.heightAt(blockSize + 2), lineAdvance * 3.0);

			for (int line = blockSize + 10; line < blockSize + 14; ++line)
				layoutSlots[line].visualRows = 4;
			heightIndex.setLayoutSlotHeightRange(blockSize + 11, blockSize + 13, layoutSlots, lineAdvance);
			QCOMPARE(heightIndex.heightAt(blockSize + 10), lineAdvance);
			QCOMPARE(heightIndex.heightAt(blockSize + 11), lineAdvance * 4.0);
			QCOMPARE(heightIndex.heightAt(blockSize + 12), lineAdvance * 4.0);
			QCOMPARE(heightIndex.heightAt(blockSize + 13), lineAdvance);

			qreal expectedTotal = static_cast<qreal>(lineCount) * lineAdvance;
			expectedTotal += 6.0 * lineAdvance;
			expectedTotal += 5.0 * 2.0 * lineAdvance;
			expectedTotal += 2.0 * 3.0 * lineAdvance;
			QCOMPARE(heightIndex.totalHeight(), expectedTotal);
			QCOMPARE(heightIndex.lineAtY(heightIndex.prefixHeightAt(blockSize + 12)), blockSize + 12);
		}

		void nativeLayoutHeightIndexRemoveFrontMaintainsBlockSums()
		{
			WorldView::NativeLayoutHeightIndex heightIndex;
			constexpr int                      blockSize   = WorldView::NativeLayoutHeightIndex::kBlockSize;
			constexpr int                      lineCount   = blockSize * 3 + 19;
			constexpr qreal                    lineAdvance = 10.0;
			heightIndex.assign(lineCount, lineAdvance);
			heightIndex.removeFront(blockSize + 7);

			constexpr int expectedCount = lineCount - blockSize - 7;
			QCOMPARE(sizeToInt(heightIndex.size()), expectedCount);
			QCOMPARE(heightIndex.totalHeight(), static_cast<qreal>(expectedCount) * lineAdvance);
			QCOMPARE(heightIndex.prefixHeightAt(17), 17.0 * lineAdvance);
			QCOMPARE(heightIndex.lineAtY(heightIndex.prefixHeightAt(blockSize + 3)), blockSize + 3);

			heightIndex.removeFront(blockSize - 11);
			constexpr int secondExpectedCount = expectedCount - blockSize + 11;
			QCOMPARE(sizeToInt(heightIndex.size()), secondExpectedCount);
			QCOMPARE(heightIndex.totalHeight(), static_cast<qreal>(secondExpectedCount) * lineAdvance);
			QCOMPARE(heightIndex.prefixHeightAt(secondExpectedCount), heightIndex.totalHeight());
		}

		void nativeLayoutConsumesQueuedRenderDeltasWithoutReset()
		{
			WorldView view;
			auto      makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("queued-delta-0"), 1));
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("queued-delta-1"), 2));
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("queued-delta-2"), 3));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;

			const QFont layoutFont;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);
			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()), 3);

			view.m_nativeLayoutSlots[0].rowsExact = 1;

			int oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("queued-delta-3"), 4));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::TailAppend,
			                                       oldLineCount);
			oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("queued-delta-4"), 5));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::TailAppend,
			                                       oldLineCount);

			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 2);
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);

			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 0);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()), 5);
			QCOMPARE(static_cast<int>(view.m_nativeLayoutSlots.at(0).rowsExact), 1);
			QCOMPARE(view.m_nativeLayoutSlots.at(3).runtimeLineKey,
			         WorldView::nativeRuntimeLineKey(view.m_nativeRenderLineCache.at(3)));
			QCOMPARE(view.m_nativeLayoutSlots.at(4).runtimeLineKey,
			         WorldView::nativeRuntimeLineKey(view.m_nativeRenderLineCache.at(4)));
		}

		void nativeLayoutConsumesQueuedRuntimeRangeRestitchWithoutReset()
		{
			WorldView view;
			auto      makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			for (int i = 0; i < 6; ++i)
				view.m_nativeRenderLineCache.push_back(
				    makeRenderLine(QStringLiteral("range-restitch-old-%1").arg(i), i + 1));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;

			const QFont layoutFont;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()), 6);
			const qreal lineAdvance = view.m_nativeLayoutCachedLineAdvance;
			QVERIFY(lineAdvance > 0.0);
			view.m_nativeLayoutSlots[1].visualRows = 11;
			view.m_nativeLayoutSlots[2].visualRows = 12;
			view.m_nativeLayoutSlots[5].visualRows = 15;
			view.m_nativeLayoutHeightIndex.setHeight(1, 11.0 * lineAdvance);
			view.m_nativeLayoutHeightIndex.setHeight(2, 12.0 * lineAdvance);
			view.m_nativeLayoutHeightIndex.setHeight(5, 15.0 * lineAdvance);

			const int                          oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			WorldView::NativeOutputRenderLines restitchedLines;
			restitchedLines.push_back(makeRenderLine(QStringLiteral("range-restitch-old-1"), 2));
			restitchedLines.push_back(makeRenderLine(QStringLiteral("range-restitch-old-2"), 3));
			restitchedLines.push_back(makeRenderLine(QStringLiteral("range-restitch-new"), 99));
			restitchedLines.push_back(makeRenderLine(QStringLiteral("range-restitch-old-5"), 6));
			view.m_nativeRenderLineCache = std::move(restitchedLines);
			view.bumpNativeRuntimeRangeRestitchRenderLineCacheRevision(oldLineCount, true, 1, 2, false, 2, 2,
			                                                           1);

			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 1);
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);

			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 0);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()), 4);
			QCOMPARE(view.m_nativeLayoutSlots.at(0).visualRows, 11);
			QCOMPARE(view.m_nativeLayoutSlots.at(1).visualRows, 12);
			QCOMPARE(view.m_nativeLayoutSlots.at(3).visualRows, 15);
			QCOMPARE(view.m_nativeLayoutHeightIndex.heightAt(0), 11.0 * lineAdvance);
			QCOMPARE(view.m_nativeLayoutHeightIndex.heightAt(1), 12.0 * lineAdvance);
			QCOMPARE(view.m_nativeLayoutHeightIndex.heightAt(3), 15.0 * lineAdvance);
			QCOMPARE(view.m_nativeLayoutSlots.at(2).runtimeLineKey,
			         WorldView::nativeRuntimeLineKey(view.m_nativeRenderLineCache.at(2)));
		}

		void nativeLayoutRejectsInvalidRangePreparedRuntimeRangeRestitch()
		{
			WorldView view;
			auto      makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			for (int i = 0; i < 6; ++i)
				view.m_nativeRenderLineCache.push_back(
				    makeRenderLine(QStringLiteral("range-restitch-invalid-old-%1").arg(i), i + 1));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;

			const QFont layoutFont;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);
			const qreal lineAdvance = view.m_nativeLayoutCachedLineAdvance;
			QVERIFY(lineAdvance > 0.0);
			view.m_nativeLayoutSlots[0].visualRows = 9;
			view.m_nativeLayoutSlots[0].rowsExact  = 1;
			view.m_nativeLayoutSlots[1].visualRows = 11;
			view.m_nativeLayoutSlots[1].rowsExact  = 1;
			view.m_nativeLayoutHeightIndex.setHeight(0, 9.0 * lineAdvance);
			view.m_nativeLayoutHeightIndex.setHeight(1, 11.0 * lineAdvance);

			const int                          oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			WorldView::NativeOutputRenderLines restitchedLines;
			restitchedLines.push_back(makeRenderLine(QStringLiteral("range-restitch-invalid-old-1"), 2));
			restitchedLines.push_back(makeRenderLine(QStringLiteral("range-restitch-invalid-old-2"), 3));
			restitchedLines.push_back(makeRenderLine(QStringLiteral("range-restitch-invalid-new"), 99));
			restitchedLines.push_back(makeRenderLine(QStringLiteral("range-restitch-invalid-old-5"), 6));
			view.m_nativeRenderLineCache = std::move(restitchedLines);
			view.bumpNativeRuntimeRangeRestitchRenderLineCacheRevision(oldLineCount, false, 1, 1, false, 1, 1,
			                                                           1);
			view.m_nativeSplitTopHeadTrimPixelsRevision = view.m_nativeRenderLineCacheRevision;
			view.m_nativeSplitTopHeadTrimPixels         = 123;
			view.m_nativeSplitTopHeadTrimLines          = 1;

			QVERIFY(
			    view.prepareNativeLayoutRangeState(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont));

			QVERIFY(view.m_nativeLayoutRangePreparedOnly);
			QCOMPARE(view.m_nativeLayoutRangePreparedRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()),
			         sizeToInt(view.m_nativeRenderLineCache.size()));
			QCOMPARE(sizeToInt(view.m_nativeLayoutHeightIndex.size()),
			         sizeToInt(view.m_nativeRenderLineCache.size()));
			QCOMPARE(view.m_nativeLayoutSlots.at(0).visualRows, -1);
			QCOMPARE(view.m_nativeLayoutSlots.at(1).visualRows, -1);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, quint64{0});
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, 0);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 0);
		}

		void nativeLayoutConsumesQueuedRuntimeLineRestitchWithoutReset()
		{
			WorldView view;
			auto      makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			for (int i = 0; i < 5; ++i)
				view.m_nativeRenderLineCache.push_back(
				    makeRenderLine(QStringLiteral("line-restitch-old-%1").arg(i), i + 1));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;

			const QFont layoutFont;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()), 5);
			const qreal lineAdvance = view.m_nativeLayoutCachedLineAdvance;
			QVERIFY(lineAdvance > 0.0);
			view.m_nativeLayoutSlots[1].visualRows = 11;
			view.m_nativeLayoutSlots[1].rowsExact  = 1;
			view.m_nativeLayoutSlots[3].visualRows = 13;
			view.m_nativeLayoutSlots[3].rowsExact  = 1;
			view.m_nativeLayoutHeightIndex.setHeight(1, 11.0 * lineAdvance);
			view.m_nativeLayoutHeightIndex.setHeight(3, 13.0 * lineAdvance);

			const int oldLineCount               = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache[2].text = QStringLiteral("line-restitch-new");
			view.m_nativeRenderLineCache[2].firstRuntimeLineNumber = 30;
			view.m_nativeRenderLineCache[2].lastRuntimeLineNumber  = 30;
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::RuntimeLineRestitch,
			                                       oldLineCount, false, 0, 2);

			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 1);
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);

			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 0);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()), 5);
			QCOMPARE(view.m_nativeLayoutSlots.at(1).visualRows, 11);
			QCOMPARE(static_cast<int>(view.m_nativeLayoutSlots.at(1).rowsExact), 1);
			QCOMPARE(view.m_nativeLayoutSlots.at(3).visualRows, 13);
			QCOMPARE(static_cast<int>(view.m_nativeLayoutSlots.at(3).rowsExact), 1);
			QCOMPARE(view.m_nativeLayoutHeightIndex.heightAt(1), 11.0 * lineAdvance);
			QCOMPARE(view.m_nativeLayoutHeightIndex.heightAt(3), 13.0 * lineAdvance);
			QCOMPARE(view.m_nativeLayoutSlots.at(2).runtimeLineKey,
			         WorldView::nativeRuntimeLineKey(view.m_nativeRenderLineCache.at(2)));
			QCOMPARE(view.m_nativeLayoutSlots.at(2).visualRows, 1);
		}

		void nativeOutputSelectionBoundsRemapPendingRenderDeltasWithoutLayoutSync()
		{
			WorldView view;
			auto      makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			for (int i = 0; i < 5; ++i)
				view.appendOutputText(QStringLiteral("selection-snapshot-old-%1").arg(i), true);
			static_cast<void>(view.nativeOutputRenderLines());

			view.selectOutputRange(2, 0, 24);
			QCOMPARE(view.outputSelectionStartLine(), 3);
			QCOMPARE(view.outputSelectionEndLine(), 3);

			const int oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.remove(0, 2);
			view.m_nativeRenderLineCache.push_back(
			    makeRenderLine(QStringLiteral("selection-snapshot-tail-5"), 6));
			view.m_nativeRenderLineCache.push_back(
			    makeRenderLine(QStringLiteral("selection-snapshot-tail-6"), 7));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::HeadTrimTailAppend,
			                                       oldLineCount, false, 2);

			const quint64 layoutRevisionBefore = view.m_nativeLayoutCachedRenderRevision;
			QCOMPARE(view.m_nativeSelectionPendingHeadTrimLines, 2);
			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 1);

			QSignalSpy selectionChangedSpy(&view, &WorldView::outputSelectionChanged);
			QCOMPARE(view.outputSelectionStartLine(), 1);
			QCOMPARE(view.outputSelectionEndLine(), 1);
			QCOMPARE(view.m_nativeSelectionPendingHeadTrimLines, 2);
			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 1);
			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, layoutRevisionBefore);
			QCOMPARE(view.outputSelectionText(), QStringLiteral("selection-snapshot-old-2"));
			QCOMPARE(view.m_nativeSelectionPendingHeadTrimLines, 2);
			QCOMPARE(selectionChangedSpy.size(), 0);

			static_cast<void>(view.synchronizeNativeRuntimeOutputPresentation(false, false));
			QCOMPARE(selectionChangedSpy.size(), 1);
			QCOMPARE(view.m_nativeSelectionPendingHeadTrimLines, 0);
			QCOMPARE(view.outputSelectionStartLine(), 1);
			QCOMPARE(view.outputSelectionEndLine(), 1);
			QCOMPARE(view.outputSelectionText(), QStringLiteral("selection-snapshot-old-2"));
		}

		void nativeOutputSelectionCollapseAfterRemapCommitsNoSelection()
		{
			WorldView view;

			for (int i = 0; i < 5; ++i)
				view.appendOutputText(QStringLiteral("selection-collapse-old-%1").arg(i), true);
			static_cast<void>(view.nativeOutputRenderLines());

			view.selectOutputRange(2, 4, 18);
			QCOMPARE(view.outputSelectionStartLine(), 3);
			QCOMPARE(view.outputSelectionEndLine(), 3);
			QCOMPARE(view.m_hasOutputSelection, true);

			const int oldLineCount               = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache[2].text = QStringLiteral("x");
			view.m_nativeRenderLineCache[2].spans.clear();
			view.m_nativeRenderLineCache[2].visualHash = 0;
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::RuntimeLineRestitch,
			                                       oldLineCount, false, 0, 2);

			QSignalSpy selectionChangedSpy(&view, &WorldView::outputSelectionChanged);
			static_cast<void>(view.synchronizeNativeRuntimeOutputPresentation(false, false));

			QCOMPARE(selectionChangedSpy.size(), 1);
			QCOMPARE(view.m_nativeSelectionPendingHeadTrimLines, 0);
			QCOMPARE(view.m_nativeOutputSelection.hasSelection, false);
			QCOMPARE(view.m_hasOutputSelection, false);
			QCOMPARE(view.outputSelectionStartLine(), 0);
			QCOMPARE(view.outputSelectionEndLine(), 0);
			QCOMPARE(view.outputSelectionText(), QString());
		}

		void commandUiSnapshotSelectionBoundsRemapPendingRenderDeltasWithoutLayoutSync()
		{
			WorldView view;
			fakeRuntimePointer()->setView(&view);
			auto makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			for (int i = 0; i < 5; ++i)
				view.appendOutputText(QStringLiteral("runtime-selection-snapshot-old-%1").arg(i), true);
			static_cast<void>(view.nativeOutputRenderLines());

			view.selectOutputRange(2, 0, 32);
			const WorldRuntime::CommandUiSnapshot initialSnapshot =
			    fakeRuntimePointer()->commandUiSnapshot(false, false, false);
			QVERIFY(initialSnapshot.hasView);
			QCOMPARE(initialSnapshot.outputSelectionStartLine, 3);
			QCOMPARE(initialSnapshot.outputSelectionEndLine, 3);

			const int oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.remove(0, 2);
			view.m_nativeRenderLineCache.push_back(
			    makeRenderLine(QStringLiteral("runtime-selection-snapshot-tail-5"), 6));
			view.m_nativeRenderLineCache.push_back(
			    makeRenderLine(QStringLiteral("runtime-selection-snapshot-tail-6"), 7));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::HeadTrimTailAppend,
			                                       oldLineCount, false, 2);

			const quint64 layoutRevisionBefore = view.m_nativeLayoutCachedRenderRevision;
			QCOMPARE(view.m_nativeSelectionPendingHeadTrimLines, 2);
			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 1);

			const WorldRuntime::CommandUiSnapshot pendingSnapshot =
			    fakeRuntimePointer()->commandUiSnapshot(false, false, false);
			QVERIFY(pendingSnapshot.hasView);
			QCOMPARE(pendingSnapshot.outputSelectionStartLine, 1);
			QCOMPARE(pendingSnapshot.outputSelectionEndLine, 1);
			QCOMPARE(view.m_nativeSelectionPendingHeadTrimLines, 2);
			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 1);
			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, layoutRevisionBefore);
			QCOMPARE(view.outputSelectionText(), QStringLiteral("runtime-selection-snapshot-old-2"));
			QCOMPARE(view.m_nativeSelectionPendingHeadTrimLines, 2);

			static_cast<void>(view.synchronizeNativeRuntimeOutputPresentation(false, false));
			const WorldRuntime::CommandUiSnapshot remappedSnapshot =
			    fakeRuntimePointer()->commandUiSnapshot(false, false, false);
			QCOMPARE(view.m_nativeSelectionPendingHeadTrimLines, 0);
			QCOMPARE(remappedSnapshot.outputSelectionStartLine, 1);
			QCOMPARE(remappedSnapshot.outputSelectionEndLine, 1);
			QCOMPARE(view.outputSelectionText(), QStringLiteral("runtime-selection-snapshot-old-2"));
			fakeRuntimePointer()->setView(nullptr);
		}

		void nativeOutputHitTestUsesRangeLayoutWithoutFullCacheSync()
		{
			resetTestState();

			WorldView view;
			view.resize(720, 420);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			QCoreApplication::processEvents();

			for (int i = 0; i < 96; ++i)
				view.appendOutputText(
				    QStringLiteral("hit-test-range-layout-%1").arg(i, 3, 10, QLatin1Char('0')), true);

			const WorldView::NativeOutputRenderLines &initialLines = view.nativeOutputRenderLines();
			QVERIFY(!initialLines.isEmpty());
			auto *const browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QVERIFY(browser->viewport());

			QRect viewportRect = browser->viewport()->rect();
			QVERIFY(!viewportRect.isEmpty());
			constexpr bool wrapEnabled     = true;
			int            wrapWidthPixels = view.nativeWrapWidthPixels(viewportRect.width(), wrapEnabled);
			int localWrapWidthPixels = view.nativeLocalWrapWidthPixels(viewportRect.width(), wrapEnabled);
			const int   lineSpacing  = qMax(0, view.m_lineSpacing);
			const QFont layoutFont   = browser->font();

			view.ensureNativeLayoutCaches(initialLines, wrapWidthPixels, localWrapWidthPixels, lineSpacing,
			                              layoutFont);
			static_cast<void>(view.ensureNativeLayoutRange(initialLines, 0, 8, wrapWidthPixels,
			                                               localWrapWidthPixels, lineSpacing, layoutFont));
			QVERIFY(view.m_nativeLayoutSlots.at(0).rowsExact != 0);
			QVERIFY(view.nativeLayoutCacheReadyFor(initialLines, wrapWidthPixels, localWrapWidthPixels,
			                                       lineSpacing, layoutFont));
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()), sizeToInt(initialLines.size()));

			const int                         resetsBefore         = view.m_nativeLayoutCacheResets;
			const int                         measurementsBefore   = view.m_nativeLayoutRowMeasurements;
			const quint64                     cachedRevisionBefore = view.m_nativeLayoutCachedRenderRevision;
			WorldView::NativeOutputRenderLine tailLine;
			tailLine.text                   = QStringLiteral("hit-test-range-layout-tail");
			tailLine.opacity                = 1.0;
			tailLine.firstRuntimeLineNumber = 97;
			tailLine.lastRuntimeLineNumber  = 97;
			tailLine.flags                  = WorldRuntime::LineOutput;
			const int oldLineCount          = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.remove(0, 2);
			view.m_nativeRenderLineCache.push_back(std::move(tailLine));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::HeadTrimTailAppend,
			                                       oldLineCount, false, 2);
			QCOMPARE(view.m_nativeRenderCacheDelta.fromRevision, cachedRevisionBefore);
			QCOMPARE(view.m_nativeRenderCacheDelta.oldLineCount, oldLineCount);
			QCOMPARE(view.m_nativeRenderCacheDelta.newLineCount, oldLineCount - 1);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()), oldLineCount);

			WorldView::NativeOutputPosition position;
			const QPoint                    hitPoint = viewportRect.center();
			QVERIFY(view.nativeOutputHitTest(reinterpret_cast<WrapTextBrowser *>(browser), hitPoint, position,
			                                 nullptr, nullptr, true));

			QCOMPARE(view.m_nativeLayoutCacheResets, resetsBefore);
			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, cachedRevisionBefore);
			QCOMPARE(view.m_nativeLayoutRangePreparedRevision, view.m_nativeRenderLineCacheRevision);
			QVERIFY(view.m_nativeLayoutRangePreparedOnly);
			QVERIFY(!view.m_nativeRenderCacheDeltas.isEmpty());
			QVERIFY(view.m_nativeLayoutRowMeasurements <= measurementsBefore + 6);
			QVERIFY(position.line >= 0);
			QVERIFY(position.line < view.m_nativeRenderLineCache.size());

			wrapWidthPixels               = view.m_nativeLayoutCachedWrapWidth;
			localWrapWidthPixels          = view.m_nativeLayoutCachedLocalWrapWidth;
			const QFont rangePreparedFont = view.m_nativeLayoutCachedFont;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, wrapWidthPixels, localWrapWidthPixels,
			                              lineSpacing, rangePreparedFont);
			QCOMPARE(view.m_nativeLayoutCacheResets, resetsBefore);
			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, view.m_nativeRenderLineCacheRevision);
			QVERIFY(!view.m_nativeLayoutRangePreparedOnly);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, quint64{0});
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, 0);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 0);
		}

		void nativeOutputCharacterRectUsesRangeLayoutWithoutFullCacheSync()
		{
			resetTestState();

			WorldView view;
			view.resize(720, 420);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			QCoreApplication::processEvents();

			for (int i = 0; i < 24; ++i)
				view.appendOutputText(
				    QStringLiteral("character-rect-range-layout-%1").arg(i, 2, 10, QLatin1Char('0')), true);

			const WorldView::NativeOutputRenderLines &initialLines = view.nativeOutputRenderLines();
			QVERIFY(!initialLines.isEmpty());
			auto *const browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QVERIFY(browser->viewport());

			const QRect viewportRect = browser->viewport()->rect();
			QVERIFY(!viewportRect.isEmpty());
			constexpr bool wrapEnabled     = true;
			const int      wrapWidthPixels = view.nativeWrapWidthPixels(viewportRect.width(), wrapEnabled);
			const int      localWrapWidthPixels =
			    view.nativeLocalWrapWidthPixels(viewportRect.width(), wrapEnabled);
			const int   lineSpacing = qMax(0, view.m_lineSpacing);
			const QFont layoutFont  = browser->font();
			view.ensureNativeLayoutCaches(initialLines, wrapWidthPixels, localWrapWidthPixels, lineSpacing,
			                              layoutFont);
			QVERIFY(view.nativeLayoutCacheReadyFor(initialLines, wrapWidthPixels, localWrapWidthPixels,
			                                       lineSpacing, layoutFont));

			WorldView::NativeOutputRenderLine tailLine;
			tailLine.text                      = QStringLiteral("character-rect-range-layout-tail");
			tailLine.opacity                   = 1.0;
			tailLine.firstRuntimeLineNumber    = 25;
			tailLine.lastRuntimeLineNumber     = 25;
			tailLine.flags                     = WorldRuntime::LineOutput;
			const int     oldLineCount         = sizeToInt(view.m_nativeRenderLineCache.size());
			const int     resetsBefore         = view.m_nativeLayoutCacheResets;
			const quint64 cachedRevisionBefore = view.m_nativeLayoutCachedRenderRevision;
			view.m_nativeRenderLineCache.push_back(std::move(tailLine));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::TailAppend,
			                                       oldLineCount, false);

			QRect globalRect;
			QVERIFY(view.nativeOutputCharacterRect(reinterpret_cast<WrapTextBrowser *>(browser), {0, 0},
			                                       globalRect));
			QVERIFY(!globalRect.isEmpty());
			QCOMPARE(view.m_nativeLayoutCacheResets, resetsBefore);
			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, cachedRevisionBefore);
			QVERIFY(view.m_nativeLayoutRangePreparedOnly);
			QCOMPARE(view.m_nativeLayoutRangePreparedRevision, view.m_nativeRenderLineCacheRevision);

			resetTestState();
		}

		void nativeLayoutReplaysFromRangePreparedRevisionWithoutReset()
		{
			WorldView view;
			auto      makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			for (int i = 0; i < 6; ++i)
				view.m_nativeRenderLineCache.push_back(
				    makeRenderLine(QStringLiteral("range-prepared-old-%1").arg(i), i + 1));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;

			const QFont layoutFont;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()), 6);
			const qreal lineAdvance = view.m_nativeLayoutCachedLineAdvance;
			QVERIFY(lineAdvance > 0.0);
			view.m_nativeLayoutSlots[2].visualRows = 12;
			view.m_nativeLayoutSlots[2].rowsExact  = 1;
			view.m_nativeLayoutSlots[3].visualRows = 13;
			view.m_nativeLayoutSlots[3].rowsExact  = 1;
			view.m_nativeLayoutHeightIndex.setHeight(2, 12.0 * lineAdvance);
			view.m_nativeLayoutHeightIndex.setHeight(3, 13.0 * lineAdvance);
			const int expectedHeadTrimPixels =
			    qMax(1, static_cast<int>(std::round(view.nativeLayoutCumulativeHeightAt(2))));

			const quint64 exactRevisionBeforeRangePrep = view.m_nativeLayoutCachedRenderRevision;
			const int     resetsBefore                 = view.m_nativeLayoutCacheResets;
			int           oldLineCount                 = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.remove(0, 2);
			view.m_nativeRenderLineCache.push_back(
			    makeRenderLine(QStringLiteral("range-prepared-tail-6"), 7));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::HeadTrimTailAppend,
			                                       oldLineCount, false, 2);

			QVERIFY(
			    view.prepareNativeLayoutRangeState(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont));
			QVERIFY(view.m_nativeLayoutRangePreparedOnly);
			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, exactRevisionBeforeRangePrep);
			QCOMPARE(view.m_nativeLayoutRangePreparedRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()), 5);
			QCOMPARE(view.m_nativeLayoutSlots.at(0).visualRows, 12);
			QCOMPARE(static_cast<int>(view.m_nativeLayoutSlots.at(0).rowsExact), 1);
			QCOMPARE(view.m_nativeLayoutHeightIndex.heightAt(0), 12.0 * lineAdvance);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, expectedHeadTrimPixels);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 2);

			const quint64 rangePreparedRevision = view.m_nativeLayoutRangePreparedRevision;
			oldLineCount                        = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.push_back(
			    makeRenderLine(QStringLiteral("range-prepared-tail-7"), 8));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::TailAppend,
			                                       oldLineCount);
			QCOMPARE(view.m_nativeLayoutRangePreparedRevision, rangePreparedRevision);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, expectedHeadTrimPixels);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 2);
			QCOMPARE(view.m_nativeRenderCacheDelta.headTrimCount, 0);

			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);

			QCOMPARE(view.m_nativeLayoutCacheResets, resetsBefore);
			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, view.m_nativeRenderLineCacheRevision);
			QVERIFY(!view.m_nativeLayoutRangePreparedOnly);
			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 0);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()), 6);
			QCOMPARE(view.m_nativeLayoutSlots.at(0).visualRows, 12);
			QCOMPARE(static_cast<int>(view.m_nativeLayoutSlots.at(0).rowsExact), 1);
			QCOMPARE(view.m_nativeLayoutSlots.at(1).visualRows, 13);
			QCOMPARE(static_cast<int>(view.m_nativeLayoutSlots.at(1).rowsExact), 1);
			QCOMPARE(view.m_nativeLayoutHeightIndex.heightAt(0), 12.0 * lineAdvance);
			QCOMPARE(view.m_nativeLayoutHeightIndex.heightAt(1), 13.0 * lineAdvance);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, expectedHeadTrimPixels);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 2);
		}

		void nativeSplitTopHeadTrimPixelsDoNotCarryAcrossFullReset()
		{
			WorldView view;
			auto      makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("full-reset-old-0"), 1));
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("full-reset-old-1"), 2));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, QFont{});
			int oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("full-reset-old-2"), 3));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::TailAppend,
			                                       oldLineCount);
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, QFont{});

			QVERIFY(view.m_nativeRenderLineCacheRevision > 0);
			QVERIFY(view.m_nativeLayoutCachedRenderRevision > 0);
			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(view.m_nativeSplitTopHeadTrimAdjustedRevision, quint64{0});
			view.m_nativeSplitTopHeadTrimPixelsRevision = view.m_nativeRenderLineCacheRevision;
			view.m_nativeSplitTopHeadTrimPixels         = 42;
			view.m_nativeSplitTopHeadTrimLines          = 2;

			oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.clear();
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("full-reset-new-0"), 100));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::FullReset,
			                                       oldLineCount);

			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, quint64{0});
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, 0);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 0);
		}

		void nativeSplitTopHeadTrimPixelsClearWhenReplayHitsFullReset()
		{
			WorldView view;
			auto      makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			for (int i = 0; i < 5; ++i)
				view.m_nativeRenderLineCache.push_back(
				    makeRenderLine(QStringLiteral("full-reset-replay-old-%1").arg(i), i + 1));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;
			const QFont layoutFont;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);
			const qreal lineAdvance = view.m_nativeLayoutCachedLineAdvance;
			QVERIFY(lineAdvance > 0.0);
			view.m_nativeLayoutSlots[2].visualRows = 12;
			view.m_nativeLayoutSlots[2].rowsExact  = 1;
			view.m_nativeLayoutHeightIndex.setHeight(2, 12.0 * lineAdvance);

			int oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.remove(0, 2);
			view.m_nativeRenderLineCache.push_back(
			    makeRenderLine(QStringLiteral("full-reset-replay-tail-5"), 6));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::HeadTrimTailAppend,
			                                       oldLineCount, false, 2);

			oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.clear();
			view.m_nativeRenderLineCache.push_back(
			    makeRenderLine(QStringLiteral("full-reset-replay-new"), 100));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::FullReset,
			                                       oldLineCount);

			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 2);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, quint64{0});
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, 0);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 0);

			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);

			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 0);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, quint64{0});
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, 0);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 0);
		}

		void nativeSplitTopHeadTrimPixelsClearWhenRangeReplayFails()
		{
			WorldView view;
			auto      makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			for (int i = 0; i < 5; ++i)
				view.m_nativeRenderLineCache.push_back(
				    makeRenderLine(QStringLiteral("range-replay-fail-old-%1").arg(i), i + 1));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;
			const QFont layoutFont;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);
			const qreal lineAdvance = view.m_nativeLayoutCachedLineAdvance;
			QVERIFY(lineAdvance > 0.0);
			view.m_nativeLayoutSlots[0].visualRows = 7;
			view.m_nativeLayoutSlots[0].rowsExact  = 1;
			view.m_nativeLayoutHeightIndex.setHeight(0, 7.0 * lineAdvance);

			const int oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.remove(0, 1);
			view.m_nativeRenderLineCache.push_back(
			    makeRenderLine(QStringLiteral("range-replay-fail-tail"), 6));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::HeadTrimTailAppend,
			                                       oldLineCount, false, 1);
			view.m_nativeRenderCacheDelta.newLineCount         = oldLineCount - 1;
			view.m_nativeRenderCacheDeltas.last().newLineCount = oldLineCount - 1;
			view.m_nativeSplitTopHeadTrimPixelsRevision        = view.m_nativeRenderLineCacheRevision;
			view.m_nativeSplitTopHeadTrimPixels                = 123;
			view.m_nativeSplitTopHeadTrimLines                 = 1;

			QVERIFY(
			    view.prepareNativeLayoutRangeState(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont));

			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()),
			         sizeToInt(view.m_nativeRenderLineCache.size()));
			QCOMPARE(sizeToInt(view.m_nativeLayoutHeightIndex.size()),
			         sizeToInt(view.m_nativeRenderLineCache.size()));
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, quint64{0});
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, 0);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 0);
		}

		void nativeSplitTopHeadTrimPixelsClearWhenFullReplayFails()
		{
			WorldView view;
			auto      makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			for (int i = 0; i < 5; ++i)
				view.m_nativeRenderLineCache.push_back(
				    makeRenderLine(QStringLiteral("full-replay-fail-old-%1").arg(i), i + 1));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;
			const QFont layoutFont;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);
			const qreal lineAdvance = view.m_nativeLayoutCachedLineAdvance;
			QVERIFY(lineAdvance > 0.0);
			view.m_nativeLayoutSlots[0].visualRows = 7;
			view.m_nativeLayoutSlots[0].rowsExact  = 1;
			view.m_nativeLayoutHeightIndex.setHeight(0, 7.0 * lineAdvance);

			const int oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.remove(0, 1);
			view.m_nativeRenderLineCache.push_back(
			    makeRenderLine(QStringLiteral("full-replay-fail-tail"), 6));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::HeadTrimTailAppend,
			                                       oldLineCount, false, 1);
			view.m_nativeRenderCacheDelta.newLineCount         = oldLineCount - 1;
			view.m_nativeRenderCacheDeltas.last().newLineCount = oldLineCount - 1;
			view.m_nativeSplitTopHeadTrimPixelsRevision        = view.m_nativeRenderLineCacheRevision;
			view.m_nativeSplitTopHeadTrimPixels                = 123;
			view.m_nativeSplitTopHeadTrimLines                 = 1;

			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);

			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()),
			         sizeToInt(view.m_nativeRenderLineCache.size()));
			QCOMPARE(sizeToInt(view.m_nativeLayoutHeightIndex.size()),
			         sizeToInt(view.m_nativeRenderLineCache.size()));
			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, quint64{0});
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, 0);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 0);
		}

		void nativeSplitTopHeadTrimPixelsDoNotCommitAcrossRangeKeyMismatch()
		{
			WorldView view;
			auto      makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			for (int i = 0; i < 5; ++i)
				view.m_nativeRenderLineCache.push_back(
				    makeRenderLine(QStringLiteral("range-key-mismatch-old-%1").arg(i), i + 1));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;
			const QFont layoutFont;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);
			const qreal lineAdvance = view.m_nativeLayoutCachedLineAdvance;
			QVERIFY(lineAdvance > 0.0);
			view.m_nativeLayoutSlots[0].visualRows = 9;
			view.m_nativeLayoutSlots[0].rowsExact  = 1;
			view.m_nativeLayoutHeightIndex.setHeight(0, 9.0 * lineAdvance);

			const int oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.remove(0, 1);
			view.m_nativeRenderLineCache.push_back(
			    makeRenderLine(QStringLiteral("range-key-mismatch-tail"), 6));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::HeadTrimTailAppend,
			                                       oldLineCount, false, 1);

			QVERIFY(
			    view.prepareNativeLayoutRangeState(view.m_nativeRenderLineCache, 420, 420, 0, layoutFont));

			QVERIFY(view.m_nativeLayoutRangePreparedOnly);
			QCOMPARE(view.m_nativeLayoutRangePreparedRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()),
			         sizeToInt(view.m_nativeRenderLineCache.size()));
			QCOMPARE(view.m_nativeLayoutSlots.at(0).visualRows, -1);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, quint64{0});
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, 0);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 0);
		}

		void nativeSplitTopHeadTrimPixelsDoNotCommitAcrossFullCacheKeyMismatch()
		{
			WorldView view;
			auto      makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			for (int i = 0; i < 5; ++i)
				view.m_nativeRenderLineCache.push_back(
				    makeRenderLine(QStringLiteral("full-key-mismatch-old-%1").arg(i), i + 1));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;
			const QFont layoutFont;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);
			const qreal lineAdvance = view.m_nativeLayoutCachedLineAdvance;
			QVERIFY(lineAdvance > 0.0);
			view.m_nativeLayoutSlots[0].visualRows = 9;
			view.m_nativeLayoutSlots[0].rowsExact  = 1;
			view.m_nativeLayoutHeightIndex.setHeight(0, 9.0 * lineAdvance);

			const int oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.remove(0, 1);
			view.m_nativeRenderLineCache.push_back(
			    makeRenderLine(QStringLiteral("full-key-mismatch-tail"), 6));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::HeadTrimTailAppend,
			                                       oldLineCount, false, 1);

			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 420, 420, 0, layoutFont);

			QVERIFY(!view.m_nativeLayoutRangePreparedOnly);
			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()),
			         sizeToInt(view.m_nativeRenderLineCache.size()));
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, quint64{0});
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, 0);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 0);
		}

		void nativeSplitTopHeadTrimPixelsDoNotCommitAcrossFullCacheLocalWrapMismatch()
		{
			WorldView view;
			auto      makeRenderLine =
			    [](const QString &text, const qint64 lineNumber, const int flags = WorldRuntime::LineOutput)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = flags;
				return line;
			};

			view.m_nativeRenderLineCache.push_back(makeRenderLine(
			    QStringLiteral("full-local-wrap-mismatch-old-local-line-with-enough-text-to-wrap"), 1,
			    WorldRuntime::LineNote));
			for (int i = 1; i < 5; ++i)
				view.m_nativeRenderLineCache.push_back(
				    makeRenderLine(QStringLiteral("full-local-wrap-mismatch-old-%1").arg(i), i + 1));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;
			const QFont layoutFont;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 140, 0, layoutFont);
			const qreal lineAdvance = view.m_nativeLayoutCachedLineAdvance;
			QVERIFY(lineAdvance > 0.0);
			view.m_nativeLayoutSlots[0].visualRows = 4;
			view.m_nativeLayoutSlots[0].rowsExact  = 1;
			view.m_nativeLayoutHeightIndex.setHeight(0, 4.0 * lineAdvance);

			const int oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.remove(0, 1);
			view.m_nativeRenderLineCache.push_back(
			    makeRenderLine(QStringLiteral("full-local-wrap-mismatch-tail"), 6));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::HeadTrimTailAppend,
			                                       oldLineCount, false, 1);

			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 220, 0, layoutFont);

			QVERIFY(!view.m_nativeLayoutRangePreparedOnly);
			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(view.m_nativeLayoutCachedLocalWrapWidth, 220);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()),
			         sizeToInt(view.m_nativeRenderLineCache.size()));
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, quint64{0});
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, 0);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 0);
		}

		void hoverRefreshResolvesNativeOutputStateDeterministically()
		{
			resetTestState();

			WorldView view;
			view.resize(640, 360);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			QCoreApplication::processEvents();

			view.appendOutputText(QStringLiteral("cached passive hover"), true);
			static_cast<void>(view.nativeOutputRenderLines());
			QTextBrowser *const browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QVERIFY(browser->viewport());
			QCursor::setPos(browser->viewport()->mapToGlobal(browser->viewport()->rect().center()));

			view.m_nativeRenderLineCacheValid  = false;
			const quint64 renderRevisionBefore = view.m_nativeRenderLineCacheRevision;

			view.applyHoveredHyperlink(QStringLiteral("https://example.invalid/cached"));
			QVERIFY(view.hyperlinkHoverActive());
			QCOMPARE(view.currentHoveredHyperlink(), QStringLiteral("https://example.invalid/cached"));
			view.refreshHoveredHyperlinkFromCursor();
			QVERIFY(!view.hyperlinkHoverActive());
			QCOMPARE(view.currentHoveredHyperlink(), QString());
			QVERIFY(view.m_nativeRenderLineCacheRevision > renderRevisionBefore);
			QVERIFY(view.m_nativeRenderLineCacheValid);

			resetTestState();
		}

		void nativeMouseMoveResolvesWordUnderMenuWithRangeLayout()
		{
			resetTestState();

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.resize(640, 360);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			QCoreApplication::processEvents();

			view.appendOutputText(QStringLiteral("hoverword marker"), true);
			const WorldView::NativeOutputRenderLines &initialLines = view.nativeOutputRenderLines();
			QVERIFY(!initialLines.isEmpty());

			QTextBrowser *const browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QVERIFY(browser->viewport());

			const QRect viewportRect = browser->viewport()->rect();
			QVERIFY(!viewportRect.isEmpty());
			constexpr bool wrapEnabled     = true;
			const int      wrapWidthPixels = view.nativeWrapWidthPixels(viewportRect.width(), wrapEnabled);
			const int      localWrapWidthPixels =
			    view.nativeLocalWrapWidthPixels(viewportRect.width(), wrapEnabled);
			const int   lineSpacing = qMax(0, view.m_lineSpacing);
			const QFont layoutFont  = browser->font();
			view.ensureNativeLayoutCaches(initialLines, wrapWidthPixels, localWrapWidthPixels, lineSpacing,
			                              layoutFont);

			QPoint hoverPoint{-1, -1};
			for (int y = viewportRect.top(); y <= viewportRect.bottom() && hoverPoint.x() < 0; y += 2)
			{
				for (int x = viewportRect.left(); x <= viewportRect.right(); x += 2)
				{
					WorldView::NativeOutputPosition position;
					bool                            textHit = false;
					if (!view.nativeOutputHitTest(reinterpret_cast<WrapTextBrowser *>(browser), {x, y},
					                              position, nullptr, nullptr, true, true, &textHit) ||
					    !textHit)
					{
						continue;
					}
					if (view.wordAtNativeOutputPosition(position) == QStringLiteral("hoverword"))
					{
						hoverPoint = {x, y};
						break;
					}
				}
			}
			QVERIFY2(hoverPoint.x() >= 0 && hoverPoint.y() >= 0, "Expected point over hoverword.");

			WorldView::NativeOutputRenderLine tailLine;
			tailLine.text                   = QStringLiteral("mouse-move-range-tail");
			tailLine.opacity                = 1.0;
			tailLine.firstRuntimeLineNumber = 2;
			tailLine.lastRuntimeLineNumber  = 2;
			tailLine.flags                  = WorldRuntime::LineOutput;
			const int oldLineCount          = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.push_back(std::move(tailLine));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::TailAppend,
			                                       oldLineCount, false);
			const int resetsBefore = view.m_nativeLayoutCacheResets;
			g_wordUnderMenu.clear();
			g_wordUnderMenuResolved = false;

			const QPoint globalHoverPoint = browser->viewport()->mapToGlobal(hoverPoint);
			QMouseEvent  moveEvent(QEvent::MouseMove, QPointF(hoverPoint), QPointF(hoverPoint),
			                       QPointF(globalHoverPoint), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
			view.eventFilter(browser->viewport(), &moveEvent);
			QCoreApplication::processEvents();

			QCOMPARE(view.m_nativeLayoutCacheResets, resetsBefore);
			QVERIFY(fakeRuntimePointer()->wordUnderMenuResolved());
			QCOMPARE(fakeRuntimePointer()->wordUnderMenu(), QStringLiteral("hoverword"));
			fakeRuntimePointer()->setView(nullptr);
			resetTestState();
		}

		void nativeLayoutConsumesMixedQueuedDeltasWithoutReset()
		{
			WorldView view;
			auto      makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			for (int i = 0; i < 6; ++i)
				view.m_nativeRenderLineCache.push_back(
				    makeRenderLine(QStringLiteral("mixed-delta-old-%1").arg(i), i + 1));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;

			const QFont layoutFont;
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()), 6);
			const qreal lineAdvance = view.m_nativeLayoutCachedLineAdvance;
			QVERIFY(lineAdvance > 0.0);
			view.m_nativeLayoutSlots[2].visualRows = 12;
			view.m_nativeLayoutSlots[2].rowsExact  = 1;
			view.m_nativeLayoutSlots[3].visualRows = 13;
			view.m_nativeLayoutSlots[3].rowsExact  = 1;
			view.m_nativeLayoutHeightIndex.setHeight(2, 12.0 * lineAdvance);
			view.m_nativeLayoutHeightIndex.setHeight(3, 13.0 * lineAdvance);
			const int expectedHeadTrimPixels =
			    qMax(1, static_cast<int>(std::round(view.nativeLayoutCumulativeHeightAt(2))));

			int oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.remove(0, 2);
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("mixed-delta-tail-6"), 7));
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("mixed-delta-tail-7"), 8));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::HeadTrimTailAppend,
			                                       oldLineCount, false, 2);

			oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			WorldView::NativeOutputRenderLines restitchedLines;
			restitchedLines.push_back(makeRenderLine(QStringLiteral("mixed-delta-old-2"), 3));
			restitchedLines.push_back(makeRenderLine(QStringLiteral("mixed-delta-old-3"), 4));
			restitchedLines.push_back(makeRenderLine(QStringLiteral("mixed-delta-new-a"), 90));
			restitchedLines.push_back(makeRenderLine(QStringLiteral("mixed-delta-new-b"), 91));
			restitchedLines.push_back(makeRenderLine(QStringLiteral("mixed-delta-new-c"), 92));
			restitchedLines.push_back(makeRenderLine(QStringLiteral("mixed-delta-tail-6"), 7));
			restitchedLines.push_back(makeRenderLine(QStringLiteral("mixed-delta-tail-7"), 8));
			view.m_nativeRenderLineCache = std::move(restitchedLines);
			view.bumpNativeRuntimeRangeRestitchRenderLineCacheRevision(oldLineCount, false, 0, 2, false, 2, 2,
			                                                           3);

			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 2);
			view.ensureNativeLayoutCaches(view.m_nativeRenderLineCache, 400, 400, 0, layoutFont);

			QCOMPARE(view.m_nativeLayoutCachedRenderRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(sizeToInt(view.m_nativeRenderCacheDeltas.size()), 0);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixelsRevision, view.m_nativeRenderLineCacheRevision);
			QCOMPARE(view.m_nativeSplitTopHeadTrimPixels, expectedHeadTrimPixels);
			QCOMPARE(view.m_nativeSplitTopHeadTrimLines, 2);
			QCOMPARE(sizeToInt(view.m_nativeLayoutSlots.size()), 7);
			QCOMPARE(view.m_nativeLayoutSlots.at(0).visualRows, 12);
			QCOMPARE(static_cast<int>(view.m_nativeLayoutSlots.at(0).rowsExact), 1);
			QCOMPARE(view.m_nativeLayoutSlots.at(1).visualRows, 13);
			QCOMPARE(static_cast<int>(view.m_nativeLayoutSlots.at(1).rowsExact), 1);
			QCOMPARE(view.m_nativeLayoutHeightIndex.heightAt(0), 12.0 * lineAdvance);
			QCOMPARE(view.m_nativeLayoutHeightIndex.heightAt(1), 13.0 * lineAdvance);
			QCOMPARE(view.m_nativeLayoutSlots.at(2).runtimeLineKey,
			         WorldView::nativeRuntimeLineKey(view.m_nativeRenderLineCache.at(2)));
			QCOMPARE(view.m_nativeLayoutSlots.at(5).runtimeLineKey,
			         WorldView::nativeRuntimeLineKey(view.m_nativeRenderLineCache.at(5)));
			QCOMPARE(view.m_nativeLayoutSlots.at(6).runtimeLineKey,
			         WorldView::nativeRuntimeLineKey(view.m_nativeRenderLineCache.at(6)));
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

		void worldOutputAccessibleFactoryExposesReadOnlyTextInterface()
		{
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(640, 360);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			view.appendOutputText(QStringLiteral("alpha"), true);
			view.appendOutputText(QStringLiteral("beta"), true);
			QCoreApplication::processEvents();

			const auto [topBrowser, bottomBrowser] = findSplitOutputBrowsers(view);
			QVERIFY(topBrowser);
			QVERIFY(bottomBrowser);

			QAccessibleInterface *browserAccessible = QAccessible::queryAccessibleInterface(topBrowser);
			QVERIFY(!browserAccessible || !browserAccessible->textInterface());

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QCOMPARE(accessible->role(), QAccessible::Terminal);
			QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("World output"));

			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			QCOMPARE(textInterface->text(0, textInterface->characterCount()), QStringLiteral("alpha\nbeta"));
			int boundaryStart = -1;
			int boundaryEnd   = -1;
			QCOMPARE(textInterface->textAtOffset(0, QAccessible::WordBoundary, &boundaryStart, &boundaryEnd),
			         QStringLiteral("alpha"));
			QCOMPARE(boundaryStart, 0);
			QCOMPARE(boundaryEnd, 5);
			QCOMPARE(
			    textInterface->textAfterOffset(0, QAccessible::WordBoundary, &boundaryStart, &boundaryEnd),
			    QStringLiteral("beta"));
			QCOMPARE(boundaryStart, 6);
			QCOMPARE(boundaryEnd, 10);
			QCOMPARE(textInterface->textAtOffset(-2, QAccessible::WordBoundary, &boundaryStart, &boundaryEnd),
			         QStringLiteral("beta"));
			QCOMPARE(boundaryStart, 6);
			QCOMPARE(boundaryEnd, 10);
			QCOMPARE(
			    textInterface->textAtOffset(0, QAccessible::SentenceBoundary, &boundaryStart, &boundaryEnd),
			    QStringLiteral("alpha"));
			QCOMPARE(boundaryStart, 0);
			QCOMPARE(boundaryEnd, 5);
			QCOMPARE(textInterface->textAfterOffset(0, QAccessible::SentenceBoundary, &boundaryStart,
			                                        &boundaryEnd),
			         QStringLiteral("beta"));
			QCOMPARE(boundaryStart, 6);
			QCOMPARE(boundaryEnd, 10);
			QCOMPARE(textInterface->selectionCount(), 0);

			textInterface->setSelection(0, 2, 8);
			int startOffset = 0;
			int endOffset   = 0;
			QCOMPARE(textInterface->selectionCount(), 1);
			textInterface->selection(0, &startOffset, &endOffset);
			QCOMPARE(startOffset, 2);
			QCOMPARE(endOffset, 8);
			QCOMPARE(view.outputSelectionText(), QStringLiteral("pha\nbe"));

			textInterface->removeSelection(0);
			QCOMPARE(textInterface->selectionCount(), 0);
		}

		void worldOutputAccessibleCanvasExposesTextAndReceivesNotifications()
		{
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(640, 360);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QVERIFY(canvas->isVisible());

			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QCOMPARE(accessible->role(), QAccessible::Terminal);
			QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("World output"));
			const QAccessible::State state = accessible->state();
			QVERIFY(state.readOnly);
			QVERIFY(state.multiLine);
			QVERIFY(state.selectableText);

			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			QCOMPARE(textInterface->characterCount(), 0);

			ScopedAccessibleUpdateCapture capture;

			view.appendOutputText(QStringLiteral("alpha"), true);
			QTRY_COMPARE(g_accessibleTextInsertRecords.size(), 1);
			QCOMPARE(g_accessibleTextInsertRecords.at(0).object, canvas);
			QCOMPARE(g_accessibleTextInsertRecords.at(0).position, 0);
			QCOMPARE(g_accessibleTextInsertRecords.at(0).text, QStringLiteral("alpha"));
			QCOMPARE(textInterface->text(0, textInterface->characterCount()), QStringLiteral("alpha"));

			view.clearOutputBuffer();
			QTRY_COMPARE(g_accessibleTextUpdateRecords.size(), 1);
			QCOMPARE(g_accessibleTextUpdateRecords.at(0).object, canvas);
			QCOMPARE(g_accessibleTextUpdateRecords.at(0).position, 0);
			QCOMPARE(g_accessibleTextUpdateRecords.at(0).removedText, QStringLiteral("alpha"));
			QCOMPARE(g_accessibleTextUpdateRecords.at(0).insertedText, QString());
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.at(0).object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.at(0).message, QStringLiteral("alpha"));
			QCOMPARE(g_accessibleValueChangedCount, 0);
		}

		void worldOutputAccessibleCanvasReceivesPartialPromptNotifications()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(640, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);

			ScopedAccessibleUpdateCapture capture;

			view.appendOutputText(QStringLiteral("room"), true);
			QTRY_COMPARE(g_accessibleTextInsertRecords.size(), 1);
			QCOMPARE(g_accessibleTextInsertRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextInsertRecords.constLast().text, QStringLiteral("room"));
			g_accessibleTextInsertRecords.clear();
			g_accessibleAnnouncementRecords.clear();

			const QString prompt = QStringLiteral("N Sw 12:30pm 32/32hp 100/100m 300mv 2000xp> ");
			view.updatePartialOutputText(prompt);

			QTRY_COMPARE(view.outputLines().constLast(), prompt);
			QTRY_COMPARE(g_accessibleTextInsertRecords.size(), 1);
			QCOMPARE(g_accessibleTextInsertRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextInsertRecords.constLast().position, 4);
			QCOMPARE(g_accessibleTextInsertRecords.constLast().text, QStringLiteral("\n") + prompt);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message, prompt);
			QCOMPARE(textInterface->text(0, textInterface->characterCount()),
			         QStringLiteral("room\n") + prompt);

			view.updatePartialOutputText(prompt);
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleTextInsertRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);

			view.clearPartialOutput();
			QTRY_COMPARE(textInterface->text(0, textInterface->characterCount()), QStringLiteral("room"));
			g_accessibleTextInsertRecords.clear();
			g_accessibleTextUpdateRecords.clear();
			g_accessibleAnnouncementRecords.clear();

			view.appendOutputText(QStringLiteral("final-line"), true);
			QTRY_COMPARE(g_accessibleTextInsertRecords.size(), 1);
			QCOMPARE(g_accessibleTextInsertRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextInsertRecords.constLast().position,
			         sizeToInt(QStringLiteral("room").size()));
			QCOMPARE(g_accessibleTextInsertRecords.constLast().text, QStringLiteral("\nfinal-line"));
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message, QStringLiteral("final-line"));
			QCOMPARE(textInterface->text(0, textInterface->characterCount()),
			         QStringLiteral("room\nfinal-line"));

			resetTestState();
		}

		void worldOutputAccessiblePartialPromptFinalizedAsSameRuntimeLineIsNotAnnouncedTwice()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(640, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);

			ScopedAccessibleUpdateCapture capture;

			view.appendOutputText(QStringLiteral("room"), true);
			QTRY_COMPARE(g_accessibleTextInsertRecords.size(), 1);
			g_accessibleTextInsertRecords.clear();
			g_accessibleAnnouncementRecords.clear();

			WorldRuntime::LineEntry pendingEntry;
			pendingEntry.flags      = WorldRuntime::LineOutput;
			pendingEntry.hardReturn = false;
			pendingEntry.lineNumber = 2;
			g_runtimeLines.push_back(pendingEntry);

			const QString prompt = QStringLiteral("N Sw 12:30pm 32/32hp 100/100m 300mv 2000xp> ");
			view.updatePartialOutputText(prompt);
			QTRY_COMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message, prompt);
			QCOMPARE(textInterface->text(0, textInterface->characterCount()),
			         QStringLiteral("room\n") + prompt);

			view.clearPartialOutput();
			QTRY_COMPARE(textInterface->text(0, textInterface->characterCount()), QStringLiteral("room\n"));
			g_accessibleTextInsertRecords.clear();
			g_accessibleTextUpdateRecords.clear();
			g_accessibleAnnouncementRecords.clear();

			g_runtimeLines.last().text       = prompt;
			g_runtimeLines.last().hardReturn = true;
			view.notifyRuntimeOutputLineChanged(sizeToInt(g_runtimeLines.size()) - 1);
			QCoreApplication::processEvents();

			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(textInterface->text(0, textInterface->characterCount()),
			         QStringLiteral("room\n") + prompt);

			resetTestState();
		}

		void worldOutputAccessiblePartialPromptDoesNotEmitQtSpeechWhenMushReaderOwnsSpeech()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();
			setFakeMushReaderLiveSpeechOwner(true);

			WorldView view;
			view.resize(640, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);

			ScopedAccessibleUpdateCapture capture;

			view.appendOutputText(QStringLiteral("room"), true);
			const QString prompt = QStringLiteral("<mushreader-prompt> ");
			view.updatePartialOutputText(prompt);

			QTRY_COMPARE(view.outputLines().constLast(), prompt);
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(textInterface->text(0, textInterface->characterCount()),
			         QStringLiteral("room\n") + prompt);

			resetTestState();
		}

		void worldOutputAccessibleSuppressedPartialUpdatesDoNotBuildLineOffsetCache()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(640, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);

			for (int i = 0; i < 200; ++i)
				view.appendOutputText(QStringLiteral("suppressed-partial-baseline-%1").arg(i), true);
			QCoreApplication::processEvents();
			view.primeAccessibleOutputTextState();
			view.m_accessibleOutputLineStartCache.clear();
			view.m_accessibleOutputLineStartCacheRevision       = 0;
			view.m_accessibleOutputLineStartCacheNextOffset     = 0;
			view.m_accessibleOutputLineStartCacheCharacterCount = -1;

			const int baselineCharacterCount = view.m_accessibleOutputCharacterCount;
			QVERIFY(baselineCharacterCount > 0);
			const qsizetype               lineStartCacheSize = view.m_accessibleOutputLineStartCache.size();

			ScopedAccessibleUpdateCapture capture;
			setFakeMushReaderLiveSpeechOwner(true);

			const QString firstPrompt = QStringLiteral("<p> ");
			view.updatePartialOutputText(firstPrompt);
			QTRY_COMPARE(view.m_accessibleOutputCharacterCount,
			             baselineCharacterCount + 1 + sizeToInt(firstPrompt.size()));
			QCOMPARE(view.m_accessibleOutputLineStartCache.size(), lineStartCacheSize);
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);

			const QString secondPrompt = QStringLiteral("<prompt> ");
			view.updatePartialOutputText(secondPrompt);
			QTRY_COMPARE(view.m_accessibleOutputCharacterCount,
			             baselineCharacterCount + 1 + sizeToInt(secondPrompt.size()));
			QCOMPARE(view.m_accessibleOutputLineStartCache.size(), lineStartCacheSize);
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);

			resetTestState();
		}

		void worldOutputAccessibleGeometryUsesNativeTextHitTesting()
		{
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(640, 360);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			view.appendOutputText(QStringLiteral("alpha"), true);
			view.appendOutputText(QStringLiteral("beta"), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QVERIFY(browser->viewport());

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);

			const QRect characterRect = textInterface->characterRect(0);
			QVERIFY(!characterRect.isEmpty());
			QVERIFY(characterRect.width() < browser->viewport()->width());
			QVERIFY(characterRect.height() < browser->viewport()->height());

			const QPoint firstCharacterPoint(characterRect.left(), characterRect.center().y());
			QCOMPARE(textInterface->offsetAtPoint(firstCharacterPoint), 0);

			const QPoint outsideTextPoint = browser->viewport()->mapToGlobal(
			    QPoint(browser->viewport()->width() - 2,
			           characterRect.center().y() - browser->viewport()->mapToGlobal(QPoint(0, 0)).y()));
			QCOMPARE(textInterface->offsetAtPoint(outsideTextPoint), -1);
		}

		void worldOutputAccessibleScrollToSubstringUsesNativeOutputScroll()
		{
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(640, 240);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 120; ++i)
				view.appendOutputText(QStringLiteral("line-%1").arg(i, 3, 10, QLatin1Char('0')), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QVERIFY(browser->verticalScrollBar());
			QTRY_VERIFY(browser->verticalScrollBar()->maximum() > 0);
			browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum());
			const int tailScrollValue = browser->verticalScrollBar()->value();
			QVERIFY(tailScrollValue > 0);

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());

			ScopedAccessibleUpdateCapture capture;
			textInterface->scrollToSubstring(0, 8);
			QCoreApplication::processEvents();

			QVERIFY(browser->verticalScrollBar()->value() < tailScrollValue);
			QVERIFY(view.isScrollbackSplitActive());
			QVERIFY(view.m_accessibleOutputReviewActive);
			QTRY_COMPARE(g_accessibleTextCursorRecords.size(), 1);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, 0);
			QCOMPARE(g_accessibleTextSelectionRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message, QStringLiteral("line-000"));
			QCOMPARE(textInterface->cursorPosition(), 0);
		}

		void worldOutputAccessibleScrollToSubstringUsesMushReaderSpeechWhenItOwnsOutput()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();
			setFakeMushReaderLiveSpeechOwner(true);
			QVector<QMudNativePluginRegistry::TestSpeechEvent> speechEvents;
			const ScopedTestStateReset                         resetState;
			QMudNativePluginRegistry::setTestSpeechSink(
			    [&speechEvents](const QMudNativePluginRegistry::TestSpeechEvent &event)
			    { speechEvents.push_back(event); });

			WorldView view;
			view.resize(640, 240);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 120; ++i)
				view.appendOutputText(QStringLiteral("mush-scroll-line-%1").arg(i, 3, 10, QLatin1Char('0')),
				                      true);
			QCoreApplication::processEvents();

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);

			ScopedAccessibleUpdateCapture capture;
			textInterface->scrollToSubstring(0, 16);
			QCoreApplication::processEvents();

			QTRY_COMPARE(g_accessibleTextCursorRecords.size(), 1);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(g_accessibleTextSelectionRecords.size(), 0);
			QCOMPARE(speechEvents.size(), 1);
			QCOMPARE(speechEvents.constLast().text, QStringLiteral("mush-scroll-line-000"));
			QVERIFY(speechEvents.constLast().interrupt);
			QCOMPARE(textInterface->cursorPosition(), 0);
		}

		void worldOutputAccessibleScrollToSubstringTailReviewClearsOnLiveAppend()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(640, 240);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			view.appendOutputText(QStringLiteral("visible-tail-review-line"), true);
			QCoreApplication::processEvents();

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			QVERIFY(!view.isScrollbackSplitActive());
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());

			ScopedAccessibleUpdateCapture capture;
			textInterface->scrollToSubstring(0, sizeToInt(QStringLiteral("visible-tail").size()));
			QCoreApplication::processEvents();

			QVERIFY(!view.isScrollbackSplitActive());
			QVERIFY(view.m_accessibleOutputReviewActive);
			QCOMPARE(view.m_accessibleOutputReviewTargetKind,
			         WorldView::AccessibleOutputReviewTargetKind::Transient);
			QCOMPARE(textInterface->cursorPosition(), 0);
			QTRY_COMPARE(g_accessibleTextCursorRecords.size(), 1);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, 0);

			g_accessibleTextCursorRecords.clear();
			view.appendOutputText(QStringLiteral("new-live-output"), true);
			QCoreApplication::processEvents();

			QVERIFY(!view.m_accessibleOutputReviewActive);
			QCOMPARE(view.m_accessibleOutputReviewTargetKind,
			         WorldView::AccessibleOutputReviewTargetKind::None);
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());
			QVERIFY(!g_accessibleTextCursorRecords.isEmpty());
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, textInterface->characterCount());

			resetTestState();
		}

		void transientReviewStateClearsOnPresentationWhenQtAccessibilityInactive()
		{
			resetTestState();
			const ScopedTestStateReset        resetState;
			const ScopedAccessibleActiveState accessibleActive(false);
			QMudNativePluginRegistry::setTestSpeechSink(
			    [](const QMudNativePluginRegistry::TestSpeechEvent &) {});
			setFakeMushReaderLiveSpeechOwner(true);

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.appendOutputText(QStringLiteral("inactive-transient-review-line"), true);
			QCoreApplication::processEvents();

			const WorldView::NativeOutputRenderLines &lines = view.nativeOutputRenderLines();
			QVERIFY(!lines.isEmpty());
			view.notifyAccessibleOutputReviewRangeTarget(
			    lines, 0, 0, -1, -1, WorldView::AccessibleOutputReviewTargetKind::Transient);

			QVERIFY(view.m_accessibleOutputReviewActive);
			QCOMPARE(view.m_accessibleOutputReviewTargetKind,
			         WorldView::AccessibleOutputReviewTargetKind::Transient);

			view.appendOutputText(QStringLiteral("inactive-live-output"), true);
			QCoreApplication::processEvents();

			QVERIFY(!view.m_accessibleOutputReviewActive);
			QCOMPARE(view.m_accessibleOutputReviewTargetKind,
			         WorldView::AccessibleOutputReviewTargetKind::None);
		}

		void worldOutputAccessibleScrollToSubstringUsesTopPaneAfterAutoPauseScroll()
		{
			resetTestState();
			const ScopedTestStateReset resetState;
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(720, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 220; ++i)
				view.appendOutputText(
				    QStringLiteral("auto-pause-accessible-line-%1").arg(i, 3, 10, QLatin1Char('0')), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());
			QCOMPARE(view.setOutputScroll(999999, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());

			ScopedAccessibleUpdateCapture capture;
			const QPointF                 localPos(viewport->rect().center());
			const QPointF                 globalPos(viewport->mapToGlobal(localPos.toPoint()));
			for (int i = 0; i < 6 && !view.isScrollbackSplitActive(); ++i)
			{
				QWheelEvent wheelUp(localPos, globalPos, QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
				                    Qt::NoModifier, Qt::NoScrollPhase, false);
				QCoreApplication::sendEvent(viewport, &wheelUp);
				QCoreApplication::processEvents();
			}

			QTRY_VERIFY(view.isScrollbackSplitActive());
			QVERIFY(view.m_accessibleOutputReviewActive);
			QVERIFY(textInterface->cursorPosition() < textInterface->characterCount());

			g_accessibleTextCursorRecords.clear();
			g_accessibleTextSelectionRecords.clear();
			g_accessibleAnnouncementRecords.clear();
			textInterface->scrollToSubstring(0, 8);
			QCoreApplication::processEvents();

			QVERIFY(view.isScrollbackSplitActive());
			QVERIFY(view.m_accessibleOutputReviewActive);
			QTRY_COMPARE(g_accessibleTextCursorRecords.size(), 1);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, 0);
			QCOMPARE(g_accessibleTextSelectionRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message,
			         QStringLiteral("auto-pause-accessible-line-000"));
			QCOMPARE(textInterface->cursorPosition(), 0);
		}

		void worldOutputAccessibleSetOutputScrollSynchronizesReviewCaret()
		{
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(720, 360);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 220; ++i)
				view.appendOutputText(
				    QStringLiteral("accessible-set-scroll-line-%1").arg(i, 3, 10, QLatin1Char('0')), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());
			QCOMPARE(view.setOutputScroll(-1, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());

			ScopedAccessibleUpdateCapture capture;
			QCOMPARE(view.setOutputScroll(0, true), 0);
			QCoreApplication::processEvents();

			QTRY_VERIFY(view.isScrollbackSplitActive());
			QVERIFY(view.m_accessibleOutputReviewActive);
			QTRY_COMPARE(g_accessibleTextCursorRecords.size(), 1);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, 0);
			QCOMPARE(textInterface->cursorPosition(), 0);

			const qsizetype eventsBeforeEnd = g_accessibleTextCursorRecords.size();
			QCOMPARE(view.setOutputScroll(-1, true), 0);
			QCoreApplication::processEvents();

			QTRY_VERIFY(!view.isScrollbackSplitActive());
			QTRY_COMPARE(g_accessibleTextCursorRecords.size(), eventsBeforeEnd + 1);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, textInterface->characterCount());
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());
		}

		void worldOutputAccessiblePageUpAnnouncesReviewLineWithoutLiveOutputEvents()
		{
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(720, 360);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 220; ++i)
				view.appendOutputText(
				    QStringLiteral("accessible-review-line-%1").arg(i, 3, 10, QLatin1Char('0')), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());
			QCOMPARE(view.setOutputScroll(999999, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			const QString outputText = textInterface->text(0, textInterface->characterCount());
			QVERIFY(outputText.endsWith(QStringLiteral("accessible-review-line-219")));
			const int lastLineStart = stringIndexToInt(outputText.lastIndexOf(QLatin1Char('\n')) + 1);
			QVERIFY(lastLineStart > 0);
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());
			QVERIFY(view.m_accessibleOutputLineStartCache.isEmpty());

			ScopedAccessibleUpdateCapture capture;
			QTest::keyClick(viewport, Qt::Key_PageUp);
			QCoreApplication::processEvents();

			QTRY_COMPARE(g_accessibleTextCursorRecords.size(), 1);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			const int reviewCursor = textInterface->cursorPosition();
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, reviewCursor);
			QVERIFY(reviewCursor >= 0);
			QVERIFY(reviewCursor < lastLineStart);
			const int     nextBreak  = stringIndexToInt(outputText.indexOf(QLatin1Char('\n'), reviewCursor));
			const int     reviewEnd  = nextBreak < 0 ? sizeToInt(outputText.size()) : nextBreak;
			const QString reviewLine = outputText.mid(reviewCursor, reviewEnd - reviewCursor);
			QVERIFY(view.isScrollbackSplitActive());
			QVERIFY(!view.m_accessibleOutputLineStartCache.isEmpty());
			QVERIFY(view.m_accessibleOutputLineStartCache.size() < view.nativeOutputRenderLines().size());
			QCOMPARE(view.m_accessibleOutputLineStartCacheCharacterCount, -1);
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message, reviewLine);
			int atLineStart = -1;
			int atLineEnd   = -1;
			QCOMPARE(textInterface->textAtOffset(reviewCursor, QAccessible::LineBoundary, &atLineStart,
			                                     &atLineEnd),
			         reviewLine);
			QCOMPARE(atLineStart, reviewCursor);
			QCOMPARE(atLineEnd, reviewEnd);
			QCOMPARE(textInterface->textAtOffset(-2, QAccessible::LineBoundary, &atLineStart, &atLineEnd),
			         reviewLine);
			QCOMPARE(atLineStart, reviewCursor);
			QCOMPARE(atLineEnd, reviewEnd);
			const int followingLineStart = reviewEnd + 1;
			QVERIFY(followingLineStart < textInterface->characterCount());
			const int followingNextBreak =
			    stringIndexToInt(outputText.indexOf(QLatin1Char('\n'), followingLineStart));
			const int followingLineEnd =
			    followingNextBreak < 0 ? sizeToInt(outputText.size()) : followingNextBreak;
			const QString followingLine =
			    outputText.mid(followingLineStart, followingLineEnd - followingLineStart);
			int afterLineStart = -1;
			int afterLineEnd   = -1;
			QCOMPARE(textInterface->textAfterOffset(reviewCursor, QAccessible::LineBoundary, &afterLineStart,
			                                        &afterLineEnd),
			         followingLine);
			QCOMPARE(afterLineStart, followingLineStart);
			QCOMPARE(afterLineEnd, followingLineEnd);
			QCOMPARE(
			    textInterface->textAfterOffset(-2, QAccessible::LineBoundary, &afterLineStart, &afterLineEnd),
			    followingLine);
			QCOMPARE(afterLineStart, followingLineStart);
			QCOMPARE(afterLineEnd, followingLineEnd);

			auto [splitTop, splitBottom] = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			QTest::keyClick(splitTop->viewport(), Qt::Key_PageUp);
			QCoreApplication::processEvents();

			QTRY_COMPARE(g_accessibleTextCursorRecords.size(), 2);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			const int secondReviewCursor = textInterface->cursorPosition();
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, secondReviewCursor);
			QVERIFY(secondReviewCursor >= 0);
			QVERIFY(secondReviewCursor < reviewCursor);
			const int secondNextBreak =
			    stringIndexToInt(outputText.indexOf(QLatin1Char('\n'), secondReviewCursor));
			const int secondReviewEnd = secondNextBreak < 0 ? sizeToInt(outputText.size()) : secondNextBreak;
			const QString secondReviewLine =
			    outputText.mid(secondReviewCursor, secondReviewEnd - secondReviewCursor);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 2);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message, secondReviewLine);

			QScrollBar *topBar = splitTop->verticalScrollBar();
			QVERIFY(topBar);
			topBar->setValue(topBar->maximum());
			const qsizetype eventsBeforeCollapse = g_accessibleTextCursorRecords.size();
			QTest::keyClick(splitTop->viewport(), Qt::Key_PageDown);
			QCoreApplication::processEvents();

			QTRY_VERIFY(!view.isScrollbackSplitActive());
			QTRY_COMPARE(g_accessibleTextCursorRecords.size(), eventsBeforeCollapse + 1);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, textInterface->characterCount());
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 2);
		}

		void worldOutputAccessibleReviewSkipsBarelyVisibleTopLine()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(720, 360);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 120; ++i)
				view.appendOutputText(
				    QStringLiteral("readable-review-line-%1").arg(i, 3, 10, QLatin1Char('0')), true);
			QCoreApplication::processEvents();

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);

			auto [splitTop, splitBottom] = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			QScrollBar *topBar = splitTop->verticalScrollBar();
			QVERIFY(topBar);

			const WorldView::NativeOutputRenderLines &lines = view.nativeOutputRenderLines();
			QVERIFY(lines.size() > 40);
			constexpr int sliverLine = 30;
			topBar->setValue(view.outputScrollUnitsPerLine() * sliverLine);
			static_cast<void>(view.accessibleOutputReviewLineIndexForTopPane(lines));
			QVERIFY(view.m_nativeLayoutHeightIndex.size() == lines.size());
			const int sliverTarget =
			    static_cast<int>(std::ceil(view.nativeLayoutCumulativeHeightAt(sliverLine + 1))) - 1;
			QVERIFY(sliverTarget > 0);
			QVERIFY(sliverTarget <= topBar->maximum());
			topBar->setValue(sliverTarget);
			view.syncOutputScrollbackReviewState(false);
			QVERIFY(view.isScrollbackSplitActive());

			ScopedAccessibleUpdateCapture capture;
			view.syncOutputScrollbackReviewState();
			QCoreApplication::processEvents();

			constexpr int expectedLine   = sliverLine + 1;
			const int     expectedCursor = view.accessibleOutputOffsetForLine(lines, expectedLine);
			QTRY_COMPARE(g_accessibleTextCursorRecords.size(), 1);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, expectedCursor);
			QCOMPARE(textInterface->cursorPosition(), expectedCursor);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message, lines.at(expectedLine).text);
		}

		void worldOutputAccessibleSetCursorPositionMovesReviewCursor()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(720, 360);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 140; ++i)
				view.appendOutputText(
				    QStringLiteral("accessible-cursor-line-%1").arg(i, 3, 10, QLatin1Char('0')), true);
			QCoreApplication::processEvents();

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);

			const QString targetLine   = QStringLiteral("accessible-cursor-line-050");
			const QString outputText   = textInterface->text(0, textInterface->characterCount());
			const int     targetCursor = stringIndexToInt(outputText.indexOf(targetLine));
			QVERIFY(targetCursor >= 0);
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());

			ScopedAccessibleUpdateCapture capture;
			textInterface->setCursorPosition(targetCursor);
			QCoreApplication::processEvents();

			QVERIFY(view.m_accessibleOutputReviewActive);
			QCOMPARE(view.m_accessibleOutputReviewTargetKind,
			         WorldView::AccessibleOutputReviewTargetKind::Transient);
			QTRY_COMPARE(g_accessibleTextCursorRecords.size(), 1);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, targetCursor);
			QCOMPARE(textInterface->cursorPosition(), targetCursor);
			QVERIFY(view.isScrollbackSplitActive());
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
		}

		void worldOutputAccessiblePageUpDoesNotSpeakWhenQtAccessibilitySpeechDisabled()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();
			const ScopedTestStateReset resetState;
			g_qtAccessibilitySpeechEnabled = false;

			WorldView view;
			view.resize(720, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 220; ++i)
				view.appendOutputText(
				    QStringLiteral("muted-accessible-review-line-%1").arg(i, 3, 10, QLatin1Char('0')), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());
			QCOMPARE(view.setOutputScroll(999999, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());

			ScopedAccessibleUpdateCapture capture;
			QTest::keyClick(viewport, Qt::Key_PageUp);
			QCoreApplication::processEvents();

			QVERIFY(view.isScrollbackSplitActive());
			QVERIFY(view.m_accessibleOutputReviewActive);
			QCOMPARE(view.m_accessibleOutputReviewLastNotifiedCursorOffset, -1);
			int canvasCursorEventCount = 0;
			for (const AccessibleTextCursorRecord &record : g_accessibleTextCursorRecords)
			{
				if (record.object == canvas)
					++canvasCursorEventCount;
			}
			QCOMPARE(canvasCursorEventCount, 0);
			int canvasSelectionEventCount = 0;
			for (const AccessibleTextSelectionRecord &record : g_accessibleTextSelectionRecords)
			{
				if (record.object == canvas)
					++canvasSelectionEventCount;
			}
			QCOMPARE(canvasSelectionEventCount, 0);
			int canvasInsertEventCount = 0;
			for (const AccessibleTextInsertRecord &record : g_accessibleTextInsertRecords)
			{
				if (record.object == canvas)
					++canvasInsertEventCount;
			}
			QCOMPARE(canvasInsertEventCount, 0);
			int canvasUpdateEventCount = 0;
			for (const AccessibleTextUpdateRecord &record : g_accessibleTextUpdateRecords)
			{
				if (record.object == canvas)
					++canvasUpdateEventCount;
			}
			QCOMPARE(canvasUpdateEventCount, 0);
			int canvasAnnouncementEventCount = 0;
			for (const AccessibleAnnouncementRecord &record : g_accessibleAnnouncementRecords)
			{
				if (record.object == canvas)
					++canvasAnnouncementEventCount;
			}
			QCOMPARE(canvasAnnouncementEventCount, 0);
		}

		void worldOutputAccessiblePageUpUsesMushReaderSpeechWhenItOwnsOutput()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();
			setFakeMushReaderLiveSpeechOwner(true);
			QVector<QMudNativePluginRegistry::TestSpeechEvent> speechEvents;
			const ScopedTestStateReset                         resetState;
			QMudNativePluginRegistry::setTestSpeechSink(
			    [&speechEvents](const QMudNativePluginRegistry::TestSpeechEvent &event)
			    { speechEvents.push_back(event); });

			WorldView view;
			view.resize(720, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 220; ++i)
				view.appendOutputText(
				    QStringLiteral("mushreader-review-line-%1").arg(i, 3, 10, QLatin1Char('0')), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());
			QCOMPARE(view.setOutputScroll(999999, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);

			ScopedAccessibleUpdateCapture capture;
			QTest::keyClick(viewport, Qt::Key_PageUp);
			QCoreApplication::processEvents();

			QTRY_COMPARE(g_accessibleTextCursorRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(speechEvents.size(), 1);
			QVERIFY(speechEvents.constLast().interrupt);
			const QString outputText   = textInterface->text(0, textInterface->characterCount());
			const int     reviewCursor = textInterface->cursorPosition();
			const int     nextBreak  = stringIndexToInt(outputText.indexOf(QLatin1Char('\n'), reviewCursor));
			const int     reviewEnd  = nextBreak < 0 ? sizeToInt(outputText.size()) : nextBreak;
			const QString reviewLine = outputText.mid(reviewCursor, reviewEnd - reviewCursor);
			QCOMPARE(speechEvents.constLast().text, reviewLine);
		}

		void worldOutputAccessiblePageUpDoesNotNotifyWhenMushReaderSpeechDisabled()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();
			setFakeMushReaderLiveSpeechOwner(true);
			QVector<QMudNativePluginRegistry::TestSpeechEvent> speechEvents;
			const ScopedTestStateReset                         resetState;
			QMudNativePluginRegistry::setTestSpeechSink(
			    [&speechEvents](const QMudNativePluginRegistry::TestSpeechEvent &event)
			    { speechEvents.push_back(event); });
			QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(fakeRuntimePointer(),
			                                                          QStringLiteral("tts")));
			QVERIFY(!QMudNativePluginRegistry::isMushReaderSpeechEnabled(fakeRuntimePointer()));
			speechEvents.clear();

			WorldView view;
			view.resize(720, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 220; ++i)
				view.appendOutputText(
				    QStringLiteral("mushreader-speech-off-review-line-%1").arg(i, 3, 10, QLatin1Char('0')),
				    true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());
			QCOMPARE(view.setOutputScroll(999999, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());
			QVERIFY(view.m_accessibleOutputLineStartCache.isEmpty());

			ScopedAccessibleUpdateCapture capture;
			QTest::keyClick(viewport, Qt::Key_PageUp);
			QCoreApplication::processEvents();

			QVERIFY(view.isScrollbackSplitActive());
			QVERIFY(view.m_accessibleOutputReviewActive);
			QCOMPARE(view.m_accessibleOutputReviewLastNotifiedCursorOffset, -1);
			QVERIFY(view.m_accessibleOutputLineStartCache.isEmpty());
			QCOMPARE(g_accessibleTextCursorRecords.size(), 0);
			QCOMPARE(g_accessibleTextSelectionRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(speechEvents.size(), 0);
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());
		}

		void worldOutputAccessibleMushReaderCursorCleanupWorksAfterQtSpeechDisabled()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();
			g_qtAccessibilitySpeechEnabled = false;
			setFakeMushReaderLiveSpeechOwner(true);
			QVector<QMudNativePluginRegistry::TestSpeechEvent> speechEvents;
			const ScopedTestStateReset                         resetState;
			QMudNativePluginRegistry::setTestSpeechSink(
			    [&speechEvents](const QMudNativePluginRegistry::TestSpeechEvent &event)
			    { speechEvents.push_back(event); });

			WorldView view;
			view.resize(720, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 220; ++i)
				view.appendOutputText(
				    QStringLiteral("mushreader-muted-qt-review-line-%1").arg(i, 3, 10, QLatin1Char('0')),
				    true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());
			QCOMPARE(view.setOutputScroll(999999, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);

			auto canvasCursorEventCount = [canvas]
			{
				int count = 0;
				for (const AccessibleTextCursorRecord &record : g_accessibleTextCursorRecords)
				{
					if (record.object == canvas)
						++count;
				}
				return count;
			};

			ScopedAccessibleUpdateCapture capture;
			QTest::keyClick(viewport, Qt::Key_PageUp);
			QCoreApplication::processEvents();

			QTRY_VERIFY(view.isScrollbackSplitActive());
			QTRY_COMPARE(canvasCursorEventCount(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(speechEvents.size(), 1);
			QVERIFY(speechEvents.constLast().interrupt);

			auto [splitTop, splitBottom] = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			QScrollBar *topBar = splitTop->verticalScrollBar();
			QVERIFY(topBar);
			topBar->setValue(topBar->maximum());
			QTest::keyClick(splitTop->viewport(), Qt::Key_PageDown);
			QCoreApplication::processEvents();

			QTRY_VERIFY(!view.isScrollbackSplitActive());
			QTRY_COMPARE(canvasCursorEventCount(), 2);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, textInterface->characterCount());
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
		}

		void outputNavigationDoesNotBuildAccessibleReviewOffsetsWhenAccessibilityInactive()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();

			const ScopedAccessibleActiveState accessibleActive(false);

			WorldView                         view;
			view.resize(720, 360);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 220; ++i)
				view.appendOutputText(
				    QStringLiteral("inactive-accessible-review-line-%1").arg(i, 3, 10, QLatin1Char('0')),
				    true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());
			QCOMPARE(view.setOutputScroll(999999, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());
			QVERIFY(view.m_accessibleOutputLineStartCache.isEmpty());

			QTest::keyClick(viewport, Qt::Key_PageUp);
			QCoreApplication::processEvents();

			QVERIFY(view.isScrollbackSplitActive());
			QVERIFY(view.m_accessibleOutputReviewActive);
			QVERIFY(view.m_accessibleOutputLineStartCache.isEmpty());
			QCOMPARE(view.m_accessibleOutputReviewLastNotifiedCursorOffset, -1);
		}

		void worldOutputAccessibleInsertNotificationUsesPresentedAppendPayload()
		{
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(640, 360);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			const auto [topBrowser, bottomBrowser] = findSplitOutputBrowsers(view);
			QVERIFY(topBrowser);
			QVERIFY(bottomBrowser);
			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QCOMPARE(accessible->role(), QAccessible::Terminal);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			QCOMPARE(textInterface->characterCount(), 0);

			ScopedAccessibleUpdateCapture capture;

			view.appendOutputText(QStringLiteral("alpha"), true);
			QTRY_COMPARE(g_accessibleTextInsertRecords.size(), 1);
			QCOMPARE(g_accessibleTextInsertRecords.at(0).object, canvas);
			QCOMPARE(g_accessibleTextInsertRecords.at(0).position, 0);
			QCOMPARE(g_accessibleTextInsertRecords.at(0).text, QStringLiteral("alpha"));
			QTRY_COMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.at(0).object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.at(0).message, QStringLiteral("alpha"));

			view.appendOutputText(QStringLiteral("beta"), true);
			QTRY_COMPARE(g_accessibleTextInsertRecords.size(), 2);
			QCOMPARE(g_accessibleTextInsertRecords.at(1).object, canvas);
			QCOMPARE(g_accessibleTextInsertRecords.at(1).position, 5);
			QCOMPARE(g_accessibleTextInsertRecords.at(1).text, QStringLiteral("\nbeta"));
			QTRY_COMPARE(g_accessibleAnnouncementRecords.size(), 2);
			QCOMPARE(g_accessibleAnnouncementRecords.at(1).object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.at(1).message, QStringLiteral("beta"));

			QAccessible::setActive(false);
			view.appendOutputText(QStringLiteral("gamma"), true);
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleTextInsertRecords.size(), 2);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 2);
			QCOMPARE(g_accessibleValueChangedCount, 0);
		}

		void worldOutputAccessibleLiveEventsSuppressedWhenMushReaderOwnsSpeech()
		{
			qmudInstallWorldOutputAccessibility();
			resetTestState();
			const ScopedTestStateReset resetState;
			setFakeMushReaderLiveSpeechOwner(true);

			WorldView view;
			view.resize(640, 360);
			view.setRuntime(fakeRuntimePointer());
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			QCOMPARE(textInterface->characterCount(), 0);

			ScopedAccessibleUpdateCapture capture;

			appendFakeRuntimeOutputText(view, QStringLiteral("alpha"), {}, false, true);
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(g_accessibleValueChangedCount, 0);
			QCOMPARE(textInterface->text(0, textInterface->characterCount()), QStringLiteral("alpha"));

			appendFakeRuntimeOutputText(view, QStringLiteral("beta"), {}, false, true);
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(g_accessibleValueChangedCount, 0);
			QCOMPARE(textInterface->text(0, textInterface->characterCount()), QStringLiteral("alpha\nbeta"));

			setFakeMushReaderLiveSpeechOwner(false);
			appendFakeRuntimeOutputText(view, QStringLiteral("gamma"), {}, false, true);
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleTextInsertRecords.size(), 1);
			QCOMPARE(g_accessibleTextInsertRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextInsertRecords.constLast().position,
			         sizeToInt(QStringLiteral("alpha\nbeta").size()));
			QCOMPARE(g_accessibleTextInsertRecords.constLast().text, QStringLiteral("\ngamma"));
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message, QStringLiteral("gamma"));
		}

		void worldOutputAccessibleLiveEventsSuppressedWhenQtAccessibilitySpeechDisabled()
		{
			qmudInstallWorldOutputAccessibility();
			resetTestState();
			const ScopedTestStateReset resetState;
			g_qtAccessibilitySpeechEnabled = false;

			WorldView view;
			view.resize(640, 360);
			view.setRuntime(fakeRuntimePointer());
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			QCOMPARE(textInterface->characterCount(), 0);

			ScopedAccessibleUpdateCapture capture;

			appendFakeRuntimeOutputText(view, QStringLiteral("alpha"), {}, false, true);
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(g_accessibleValueChangedCount, 0);
			QCOMPARE(textInterface->text(0, textInterface->characterCount()), QStringLiteral("alpha"));

			appendFakeRuntimeOutputText(view, QStringLiteral("beta"), {}, false, true);
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(g_accessibleValueChangedCount, 0);
			QCOMPARE(textInterface->text(0, textInterface->characterCount()), QStringLiteral("alpha\nbeta"));

			g_qtAccessibilitySpeechEnabled = true;
			appendFakeRuntimeOutputText(view, QStringLiteral("gamma"), {}, false, true);
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleTextInsertRecords.size(), 1);
			QCOMPARE(g_accessibleTextInsertRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextInsertRecords.constLast().position,
			         sizeToInt(QStringLiteral("alpha\nbeta").size()));
			QCOMPARE(g_accessibleTextInsertRecords.constLast().text, QStringLiteral("\ngamma"));
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message, QStringLiteral("gamma"));
		}

		void worldOutputAccessibleSuppressedEmptyResetKeepsTailAppendBaseline()
		{
			qmudInstallWorldOutputAccessibility();
			resetTestState();
			const ScopedTestStateReset resetState;
			g_qtAccessibilitySpeechEnabled = false;

			WorldView view;
			view.resize(640, 360);
			view.setRuntime(fakeRuntimePointer());
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);

			ScopedAccessibleUpdateCapture capture;

			appendFakeRuntimeOutputText(view, QStringLiteral("discarded"), {}, false, true);
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);

			g_runtimeLines.clear();
			view.clearOutputBuffer();
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(textInterface->characterCount(), 0);

			appendFakeRuntimeOutputText(view, QStringLiteral("muted"), {}, false, true);
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(textInterface->text(0, textInterface->characterCount()), QStringLiteral("muted"));

			g_qtAccessibilitySpeechEnabled = true;
			appendFakeRuntimeOutputText(view, QStringLiteral("announced"), {}, false, true);
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleTextInsertRecords.size(), 1);
			QCOMPARE(g_accessibleTextInsertRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextInsertRecords.constLast().position,
			         sizeToInt(QStringLiteral("muted").size()));
			QCOMPARE(g_accessibleTextInsertRecords.constLast().text, QStringLiteral("\nannounced"));
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message, QStringLiteral("announced"));
		}

		void worldOutputAccessibleSuppressedHeadTrimAnnouncesFirstResumedTailLine()
		{
			qmudInstallWorldOutputAccessibility();
			resetTestState();
			const ScopedTestStateReset resetState;
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("2"));
			g_qtAccessibilitySpeechEnabled = false;

			WorldView view;
			view.resize(640, 360);
			view.setRuntime(fakeRuntimePointer());
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);

			ScopedAccessibleUpdateCapture capture;
			const qsizetype               lineStartCacheSize = view.m_accessibleOutputLineStartCache.size();

			appendFakeRuntimeOutputText(view, QStringLiteral("alpha"), {}, false, true);
			appendFakeRuntimeOutputText(view, QStringLiteral("beta"), {}, false, true);
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(textInterface->text(0, textInterface->characterCount()), QStringLiteral("alpha\nbeta"));

			appendFakeRuntimeOutputText(view, QStringLiteral("gamma"), {}, false, true);
			QCoreApplication::processEvents();
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(textInterface->text(0, textInterface->characterCount()), QStringLiteral("beta\ngamma"));
			QCOMPARE(view.m_accessibleOutputCharacterCount, sizeToInt(QStringLiteral("beta\ngamma").size()));
			QCOMPARE(view.m_accessibleOutputLineStartCache.size(), lineStartCacheSize);

			g_qtAccessibilitySpeechEnabled = true;
			appendFakeRuntimeOutputText(view, QStringLiteral("delta"), {}, false, true);
			QCoreApplication::processEvents();
			QCOMPARE(textInterface->text(0, textInterface->characterCount()), QStringLiteral("gamma\ndelta"));
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message, QStringLiteral("delta"));
		}

		void worldOutputAccessibleSuppressedHeadRestitchTrimAnnouncesOnlyResumedTailLine()
		{
			qmudInstallWorldOutputAccessibility();
			resetTestState();
			const ScopedTestStateReset resetState;
			g_qtAccessibilitySpeechEnabled = false;

			WorldView view;
			view.resize(640, 360);
			view.setRuntime(fakeRuntimePointer());
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);

			auto makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("trimmed-head"), 1));
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("trimmed-fragment"), 2));
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("retained-tail"), 3));
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("old-tail"), 4));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;

			ScopedAccessibleUpdateCapture capture;
			view.notifyAccessibleOutputPresented(view.m_nativeRenderLineCache);
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);

			const int oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.remove(0, 2);
			view.m_nativeRenderLineCache[0].text = QStringLiteral("restitched-head");
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("resumed-tail"), 5));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::HeadTrimTailAppend,
			                                       oldLineCount, false, 2, 0, true);

			g_qtAccessibilitySpeechEnabled = true;
			view.notifyAccessibleOutputPresented(view.m_nativeRenderLineCache);
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message, QStringLiteral("resumed-tail"));
		}

		void worldOutputAccessibleSuppressedHeadRestitchTrimUpdatesBaselineIncrementally()
		{
			qmudInstallWorldOutputAccessibility();
			resetTestState();
			const ScopedTestStateReset resetState;
			g_qtAccessibilitySpeechEnabled = false;

			WorldView view;
			view.resize(640, 360);
			view.setRuntime(fakeRuntimePointer());
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);

			auto makeRenderLine = [](const QString &text, const qint64 lineNumber)
			{
				WorldView::NativeOutputRenderLine line;
				line.text                   = text;
				line.opacity                = 1.0;
				line.firstRuntimeLineNumber = lineNumber;
				line.lastRuntimeLineNumber  = lineNumber;
				line.flags                  = WorldRuntime::LineOutput;
				return line;
			};

			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("trimmed-head"), 1));
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("trimmed-fragment"), 2));
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("retained-tail"), 3));
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("old-tail"), 4));
			view.m_nativeRenderLineCacheValid       = true;
			view.m_nativeRenderLineCacheFromRuntime = true;

			ScopedAccessibleUpdateCapture capture;
			view.notifyAccessibleOutputPresented(view.m_nativeRenderLineCache);
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(
			    view.m_accessibleOutputCharacterCount,
			    sizeToInt(QStringLiteral("trimmed-head\ntrimmed-fragment\nretained-tail\nold-tail").size()));
			QCOMPARE(sizeToInt(view.m_accessibleOutputBaselineLineLengths.size()), 4);
			const qsizetype lineStartCacheSize = view.m_accessibleOutputLineStartCache.size();

			const int       oldLineCount = sizeToInt(view.m_nativeRenderLineCache.size());
			view.m_nativeRenderLineCache.remove(0, 2);
			view.m_nativeRenderLineCache[0].text = QStringLiteral("restitched-head");
			view.m_nativeRenderLineCache.push_back(makeRenderLine(QStringLiteral("resumed-tail"), 5));
			view.bumpNativeRenderLineCacheRevision(WorldView::NativeRenderCacheDeltaKind::HeadTrimTailAppend,
			                                       oldLineCount, false, 2, 0, true);

			view.notifyAccessibleOutputPresented(view.m_nativeRenderLineCache);
			QCOMPARE(g_accessibleTextInsertRecords.size(), 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(view.m_accessibleOutputCharacterCount,
			         sizeToInt(QStringLiteral("restitched-head\nold-tail\nresumed-tail").size()));
			QCOMPARE(sizeToInt(view.m_accessibleOutputBaselineLineLengths.size()), 3);
			QCOMPARE(view.m_accessibleOutputBaselineLineLengths.at(0),
			         sizeToInt(QStringLiteral("restitched-head").size()));
			QCOMPARE(view.m_accessibleOutputBaselineLineLengths.at(1),
			         sizeToInt(QStringLiteral("old-tail").size()));
			QCOMPARE(view.m_accessibleOutputBaselineLineLengths.at(2),
			         sizeToInt(QStringLiteral("resumed-tail").size()));
			QCOMPARE(view.m_accessibleOutputLineStartCache.size(), lineStartCacheSize);
		}

		void worldOutputAccessibleNonAppendMutationsUseContentChangedFallback()
		{
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(640, 360);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			const auto [topBrowser, bottomBrowser] = findSplitOutputBrowsers(view);
			QVERIFY(topBrowser);
			QVERIFY(bottomBrowser);
			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			QCOMPARE(textInterface->characterCount(), 0);

			ScopedAccessibleUpdateCapture capture;

			view.appendOutputText(QStringLiteral("alpha"), true);
			QTRY_COMPARE(g_accessibleTextInsertRecords.size(), 1);
			QTRY_COMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleValueChangedCount, 0);
			QCOMPARE(g_accessibleTextUpdateRecords.size(), 0);

			view.clearOutputBuffer();
			QTRY_COMPARE(g_accessibleTextUpdateRecords.size(), 1);
			QCOMPARE(g_accessibleTextUpdateRecords.at(0).object, canvas);
			QCOMPARE(g_accessibleTextUpdateRecords.at(0).position, 0);
			QCOMPARE(g_accessibleTextUpdateRecords.at(0).removedText, QStringLiteral("alpha"));
			QCOMPARE(g_accessibleTextUpdateRecords.at(0).insertedText, QString());
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleValueChangedCount, 0);
			QCOMPARE(g_accessibleTextInsertRecords.size(), 1);
			QCOMPARE(textInterface->characterCount(), 0);

			QVector<WorldRuntime::LineEntry> replacementLines{
			    makeRuntimeLine(QStringLiteral("replacement"), WorldRuntime::LineOutput, true, 1)};
			view.restoreOutputFromPersistedLines(replacementLines);
			QTRY_COMPARE(g_accessibleTextUpdateRecords.size(), 2);
			QCOMPARE(g_accessibleTextUpdateRecords.at(1).object, canvas);
			QCOMPARE(g_accessibleTextUpdateRecords.at(1).position, 0);
			QCOMPARE(g_accessibleTextUpdateRecords.at(1).removedText, QString());
			QCOMPARE(g_accessibleTextUpdateRecords.at(1).insertedText, QStringLiteral("replacement"));
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleValueChangedCount, 0);
			QCOMPARE(g_accessibleTextInsertRecords.size(), 1);
			QCOMPARE(textInterface->text(0, textInterface->characterCount()), QStringLiteral("replacement"));

			QVector<WorldRuntime::LineEntry> preservedPrefixLines{
			    makeRuntimeLine(QStringLiteral("replacement"), WorldRuntime::LineOutput, true, 1),
			    makeRuntimeLine(QStringLiteral("delta"), WorldRuntime::LineOutput, true, 2)};
			view.restoreOutputFromPersistedLines(preservedPrefixLines);
			QTRY_COMPARE(g_accessibleTextUpdateRecords.size(), 3);
			QCOMPARE(g_accessibleTextUpdateRecords.at(2).object, canvas);
			QCOMPARE(g_accessibleTextUpdateRecords.at(2).position, 0);
			QCOMPARE(g_accessibleTextUpdateRecords.at(2).removedText, QStringLiteral("replacement"));
			QCOMPARE(g_accessibleTextUpdateRecords.at(2).insertedText, QStringLiteral("replacement\ndelta"));
			QTRY_COMPARE(g_accessibleAnnouncementRecords.size(), 2);
			QCOMPARE(g_accessibleAnnouncementRecords.at(1).object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.at(1).message, QStringLiteral("delta"));
			QCOMPARE(textInterface->text(0, textInterface->characterCount()),
			         QStringLiteral("replacement\ndelta"));
		}

		void worldOutputAccessibleHeadTrimAppendUsesContentChangedFallback()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("2"));
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.resize(640, 360);
			view.show();
			QVERIFY(QTest::qWaitForWindowExposed(&view));
			const auto [topBrowser, bottomBrowser] = findSplitOutputBrowsers(view);
			QVERIFY(topBrowser);
			QVERIFY(bottomBrowser);
			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QVERIFY(QAccessible::queryAccessibleInterface(canvas));

			ScopedAccessibleUpdateCapture capture;

			view.appendOutputText(QStringLiteral("one"), true);
			QTRY_COMPARE(g_accessibleTextInsertRecords.size(), 1);
			QTRY_COMPARE(g_accessibleAnnouncementRecords.size(), 1);
			view.appendOutputText(QStringLiteral("two"), true);
			QTRY_COMPARE(g_accessibleTextInsertRecords.size(), 2);
			QTRY_COMPARE(g_accessibleAnnouncementRecords.size(), 2);

			view.appendOutputText(QStringLiteral("three"), true);
			QTRY_COMPARE(g_accessibleTextUpdateRecords.size(), 1);
			QCOMPARE(g_accessibleTextUpdateRecords.at(0).object, canvas);
			QCOMPARE(g_accessibleTextUpdateRecords.at(0).position, 0);
			QCOMPARE(g_accessibleTextUpdateRecords.at(0).removedText, QStringLiteral("one\ntwo"));
			QCOMPARE(g_accessibleTextUpdateRecords.at(0).insertedText, QStringLiteral("two\nthree"));
			QTRY_COMPARE(g_accessibleAnnouncementRecords.size(), 3);
			QCOMPARE(g_accessibleAnnouncementRecords.at(2).object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.at(2).message, QStringLiteral("three"));
			QCOMPARE(g_accessibleValueChangedCount, 0);
			QCOMPARE(g_accessibleTextInsertRecords.size(), 2);
			QVERIFY(view.outputLines().contains(QStringLiteral("three")));
			QVERIFY(!view.outputLines().contains(QStringLiteral("one")));
			resetTestState();
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

		void devicePixelRatioChangeResyncsRuntimeMiniWindowBackingStore()
		{
			resetTestState();

			WorldView view;
			view.resize(640, 360);
			view.show();
			view.setRuntime(fakeRuntimePointer());
			QCoreApplication::processEvents();

			MiniWindow  &window = appendTestMiniWindow(QStringLiteral("dpi-sync"), QRect(10, 10, 80, 40), 0,
			                                           QColor(10, 20, 30));
			const double targetRatio = view.devicePixelRatioF();
			const double staleRatio  = qFuzzyCompare(targetRatio, 1.0) ? 2.0 : 1.0;
			MiniWindowUtils::setDevicePixelRatio(window, staleRatio);
			QVERIFY(!qFuzzyCompare(window.devicePixelRatio, targetRatio));

			QEvent event(QEvent::DevicePixelRatioChange);
			QCoreApplication::sendEvent(&view, &event);
			QCoreApplication::processEvents();

			QVERIFY(qFuzzyCompare(window.devicePixelRatio, targetRatio));
			QVERIFY(qFuzzyCompare(window.backingSurfaceDevicePixelRatio(), targetRatio));
			QCOMPARE(window.backingSurfaceSize(),
			         MiniWindow::backingStoreSize(window.width, window.height, targetRatio));

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

		void textRectangleInfoBoundsUseRightBottomEdges()
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
			const int info292 = rect.left() + rect.width();
			const int info293 = rect.top() + rect.height();

			QCOMPARE(info292 - info290, rect.width());
			QCOMPARE(info293 - info291, rect.height());

			resetTestState();
		}

		void textViewportRectangleExcludesOutputScrollbar()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			for (int i = 0; i < 300; ++i)
				view.appendOutputText(QStringLiteral("scrollbar-primer-%1").arg(i), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			browser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
			QCoreApplication::processEvents();
			QSplitter *const outputSplitter = findOutputSplitter(view);
			QVERIFY(outputSplitter);
			QWidget *const outputStack = outputSplitter->parentWidget();
			QVERIFY(outputStack);
			QTRY_VERIFY(browser->verticalScrollBar()->isVisible());

			const QRect viewportRect(browser->viewport()->mapTo(outputStack, QPoint(0, 0)),
			                         browser->viewport()->size());
			QCOMPARE(view.outputTextViewportRectangleUnreserved(), viewportRect);
			QVERIFY(view.outputTextViewportRectangleUnreserved().width() <
			        view.outputTextRectangleUnreserved().width());

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

		void outputKeyboardNavigationStillScrollsWhenAllTypingToCommandWindowEnabled()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			for (int i = 0; i < 320; ++i)
				view.appendOutputText(QStringLiteral("keyboard-scroll-%1").arg(i), true);
			view.setAllTypingToCommandWindow(true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());

			QCOMPARE(view.setOutputScroll(0, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->minimum());

			QTest::keyClick(viewport, Qt::Key_PageDown);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.outputScrollPosition() > bar->minimum());

			QTest::keyClick(viewport, Qt::Key_End, Qt::ControlModifier);
			QCoreApplication::processEvents();
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			QTest::keyClick(viewport, Qt::Key_Home, Qt::ControlModifier);
			QCoreApplication::processEvents();
			QTRY_COMPARE(view.outputScrollPosition(), bar->minimum());

			resetTestState();
		}

		void outputKeyboardNavigationUsesEffectiveShortcutOverrides()
		{
			resetTestState();
			g_useFakeAppController = true;
			g_globalOptions.insert(QStringLiteral("Shortcut.DisplayStart"), QStringLiteral("PgUp"));

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyShortcutPreferences();
			QCoreApplication::processEvents();

			for (int i = 0; i < 320; ++i)
				view.appendOutputText(QStringLiteral("output-effective-shortcut-scroll-%1").arg(i), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());

			int       shortcutTriggerCount = 0;
			QShortcut pageUpShortcut(QKeySequence(QStringLiteral("PageUp")), &view);
			pageUpShortcut.setContext(Qt::ApplicationShortcut);
			connect(&pageUpShortcut, &QShortcut::activated, this,
			        [&shortcutTriggerCount] { ++shortcutTriggerCount; });

			QCOMPARE(view.setOutputScroll(999999, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			QTest::keyClick(viewport, Qt::Key_PageUp);
			QCoreApplication::processEvents();

			QCOMPARE(shortcutTriggerCount, 0);
			QTRY_VERIFY(view.isScrollbackSplitActive());
			auto [splitTop, splitBottom] = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			QScrollBar *topBar = splitTop->verticalScrollBar();
			QVERIFY(topBar);
			QTRY_COMPARE(topBar->value(), topBar->minimum());

			resetTestState();
		}

		void outputKeyboardNavigationIgnoresDefaultKeyRemovedByOverride()
		{
			resetTestState();
			g_useFakeAppController = true;
			g_globalOptions.insert(QStringLiteral("Shortcut.DisplayPageUp"), QStringLiteral("Ctrl+Alt+U"));

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyShortcutPreferences();
			QCoreApplication::processEvents();

			for (int i = 0; i < 520; ++i)
				view.appendOutputText(QStringLiteral("output-removed-default-shortcut-%1").arg(i), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());

			QCOMPARE(view.setOutputScroll(999999, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			QTest::keyClick(viewport, Qt::Key_U, Qt::ControlModifier | Qt::AltModifier);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isScrollbackSplitActive());
			auto [splitTop, splitBottom] = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			QScrollBar *topBar = splitTop->verticalScrollBar();
			QVERIFY(topBar);
			QTRY_VERIFY(topBar->maximum() > topBar->minimum());
			QTRY_VERIFY(topBar->value() > topBar->minimum());

			const int positionAfterConfiguredShortcut = topBar->value();

			QTest::keyClick(splitTop->viewport(), Qt::Key_PageUp);
			QCoreApplication::processEvents();
			QCOMPARE(topBar->value(), positionAfterConfiguredShortcut);

			QTest::keyClick(splitTop->viewport(), Qt::Key_U, Qt::ControlModifier | Qt::AltModifier);
			QCoreApplication::processEvents();
			QTRY_VERIFY(topBar->value() < positionAfterConfiguredShortcut);

			resetTestState();
		}

		void ctrlCCopiesOutputSelectionWhenAllTypingToCommandWindowEnabled()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.setAllTypingToCommandWindow(true);
			QCoreApplication::processEvents();

			view.setInputText(QStringLiteral("input-copy-target"), true);
			if (QPlainTextEdit *input = view.inputEditor())
				input->selectAll();

			view.appendOutputText(QStringLiteral("output-copy-target"), true);
			QCoreApplication::processEvents();
			view.selectOutputRange(0, 0, 6);
			QTRY_VERIFY(view.hasOutputSelection());
			QCOMPARE(view.outputSelectionText(), QStringLiteral("output"));

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);

			QGuiApplication::clipboard()->setText(QStringLiteral("clipboard-before-copy"));
			QTest::keyClick(viewport, Qt::Key_C, Qt::ControlModifier);
			QCoreApplication::processEvents();
			QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("output"));

			resetTestState();
		}

		void ctrlCCopiesOutputSelectionWhenInputFocused()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.setAllTypingToCommandWindow(true);
			QCoreApplication::processEvents();

			view.setInputText(QStringLiteral("input-copy-target"), true);
			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->selectAll();

			view.appendOutputText(QStringLiteral("output-copy-target"), true);
			QCoreApplication::processEvents();
			view.selectOutputRange(0, 0, 6);
			QTRY_VERIFY(view.hasOutputSelection());
			QCOMPARE(view.outputSelectionText(), QStringLiteral("output"));

			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			QGuiApplication::clipboard()->setText(QStringLiteral("clipboard-before-copy"));
			QTest::keySequence(input, QKeySequence::Copy);
			QCoreApplication::processEvents();
			QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("output"));

			resetTestState();
		}

		void ctrlBackspaceDeletesPreviousWordWhenOptionEnabled()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("ctrl_backspace_deletes_last_word"), QStringLiteral("1"));

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			view.setInputText(QStringLiteral("alpha beta gamma"), true);
			QCOMPARE(view.inputText(), QStringLiteral("alpha beta gamma"));

			int       shortcutTriggerCount = 0;
			QShortcut conflictingShortcut(QKeySequence(QStringLiteral("Ctrl+Backspace")), input);
			conflictingShortcut.setContext(Qt::WidgetWithChildrenShortcut);
			connect(&conflictingShortcut, &QShortcut::activated, this,
			        [&shortcutTriggerCount] { ++shortcutTriggerCount; });

			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());
			QTest::keyClick(input, Qt::Key_Backspace, Qt::ControlModifier);
			QCoreApplication::processEvents();

			QCOMPARE(view.inputText(), QStringLiteral("alpha beta"));
			QCOMPARE(shortcutTriggerCount, 0);
			resetTestState();
		}

		void ctrlBackspaceRecallsLastHistoryWordWhenDeleteOptionDisabled()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.addToHistoryForced(QStringLiteral("cast fireball"));
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);

			int       shortcutTriggerCount = 0;
			QShortcut conflictingShortcut(QKeySequence(QStringLiteral("Ctrl+Backspace")), input);
			conflictingShortcut.setContext(Qt::WidgetWithChildrenShortcut);
			connect(&conflictingShortcut, &QShortcut::activated, this,
			        [&shortcutTriggerCount] { ++shortcutTriggerCount; });

			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());
			QTest::keyClick(input, Qt::Key_Backspace, Qt::ControlModifier);
			QCoreApplication::processEvents();

			QCOMPARE(view.inputText(), QStringLiteral("fireball"));
			QCOMPARE(shortcutTriggerCount, 0);
			resetTestState();
		}

		void pluginAcceleratorOverridesConflictingShortcut()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.setAllTypingToCommandWindow(true);
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			constexpr Qt::Key key        = Qt::Key_K;
			constexpr quint16 virtualKey = 0x4B;
			constexpr int     commandId  = 77;
			const qint64      mapKey     = makeAcceleratorMapKey(key, Qt::ControlModifier, virtualKey);
			g_acceleratorCommands.insert(mapKey, commandId);

			int       shortcutTriggerCount = 0;
			QShortcut conflictingShortcut(QKeySequence(QStringLiteral("Ctrl+K")), input);
			conflictingShortcut.setContext(Qt::WidgetWithChildrenShortcut);
			connect(&conflictingShortcut, &QShortcut::activated, this,
			        [&shortcutTriggerCount] { ++shortcutTriggerCount; });

			QTest::keyClick(input, key, Qt::ControlModifier);
			QCoreApplication::processEvents();

			QCOMPARE(g_acceleratorExecutionCount, 1);
			QCOMPARE(g_lastExecutedAcceleratorCommand, commandId);
			QCOMPARE(shortcutTriggerCount, 0);

			resetTestState();
		}

		void pluginAcceleratorOverridesAllTypingDisplayShortcut()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.setAllTypingToCommandWindow(true);
			for (int i = 0; i < 120; ++i)
				view.appendOutputText(QStringLiteral("accelerator-display-shortcut-%1").arg(i), true);
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			constexpr Qt::Key key        = Qt::Key_PageUp;
			constexpr quint16 virtualKey = 0x21;
			constexpr int     commandId  = 78;
			const qint64      mapKey     = makeAcceleratorMapKey(key, Qt::NoModifier, virtualKey);
			g_acceleratorCommands.insert(mapKey, commandId);

			QTest::keyClick(input, key);
			QCoreApplication::processEvents();

			QCOMPARE(g_acceleratorExecutionCount, 1);
			QCOMPARE(g_lastExecutedAcceleratorCommand, commandId);
			QVERIFY(!view.isScrollbackSplitActive());

			resetTestState();
		}

		void replaceMacroClearsChangedStateAndMovesCaretToEnd()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("confirm_before_replacing_typing"), QStringLiteral("1"));
			g_testMacros.push_back(
			    makeMacro(QStringLiteral("F2"), QStringLiteral("replace"), QStringLiteral("look")));
			g_testMacros.push_back(
			    makeMacro(QStringLiteral("F3"), QStringLiteral("replace"), QStringLiteral("say")));

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.setAllTypingToCommandWindow(true);
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			QTest::keyClick(input, Qt::Key_F2);
			QCoreApplication::processEvents();

			QCOMPARE(view.inputText(), QStringLiteral("look"));
			QCOMPARE(input->textCursor().position(), view.inputText().size());

			const auto sawDialog    = std::make_shared<bool>(false);
			const auto keepWatching = std::make_shared<bool>(true);
			auto       watcher      = std::make_shared<std::function<void()>>();
			*watcher                = [watcher, sawDialog, keepWatching]
			{
				if (!*keepWatching)
					return;
				for (QWidget *widget : QApplication::topLevelWidgets())
				{
					auto *messageBox = qobject_cast<QMessageBox *>(widget);
					if (!messageBox || !messageBox->text().contains(QStringLiteral("Replace your typing of")))
						continue;
					*sawDialog    = true;
					*keepWatching = false;
					messageBox->reject();
					return;
				}
				QTimer::singleShot(10, qApp, [watcher] { (*watcher)(); });
			};
			QTimer::singleShot(0, qApp, [watcher] { (*watcher)(); });

			QTest::keyClick(input, Qt::Key_F3);
			QCoreApplication::processEvents();
			*keepWatching = false;
			QCoreApplication::processEvents();

			QVERIFY(!*sawDialog);
			QCOMPARE(view.inputText(), QStringLiteral("say"));
			QCOMPARE(input->textCursor().position(), view.inputText().size());

			resetTestState();
		}

		void sendNowMacroUsesCommandEvaluationPath()
		{
			resetTestState();
			g_testMacros.push_back(
			    makeMacro(QStringLiteral("F2"), QStringLiteral("send_now"), QStringLiteral("north")));

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.setAllTypingToCommandWindow(true);
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			QTest::keyClick(input, Qt::Key_F2);
			QCoreApplication::processEvents();

			QCOMPARE(g_executeCommandCount, 1);
			QCOMPARE(g_lastExecutedCommand, QStringLiteral("north"));
			QVERIFY(g_lastUserMacroHistory);
			QCOMPARE(g_actionSourceDuringExecute, WorldRuntime::eUserMacro);
			QCOMPARE(g_currentActionSource, WorldRuntime::eUnknownActionSource);
			QCOMPARE(g_sendCommandCount, 0);

			resetTestState();
		}

		void commandOptionShortcutsOverrideConflictingShortcutsWithInputFocus()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("ctrl_p_goes_to_previous_command"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("ctrl_n_goes_to_next_command"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("ctrl_z_goes_to_end_of_buffer"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.addToHistoryForced(QStringLiteral("north"));
			view.addToHistoryForced(QStringLiteral("south"));
			for (int i = 0; i < 320; ++i)
				view.appendOutputText(QStringLiteral("command-option-input-scroll-%1").arg(i), true);
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());

			int       shortcutTriggerCount = 0;
			QShortcut ctrlPShortcut(QKeySequence(QStringLiteral("Ctrl+P")), &view);
			QShortcut ctrlNShortcut(QKeySequence(QStringLiteral("Ctrl+N")), &view);
			QShortcut ctrlZShortcut(QKeySequence(QStringLiteral("Ctrl+Z")), &view);
			for (QShortcut *shortcut : {&ctrlPShortcut, &ctrlNShortcut, &ctrlZShortcut})
			{
				shortcut->setContext(Qt::ApplicationShortcut);
				connect(shortcut, &QShortcut::activated, this,
				        [&shortcutTriggerCount] { ++shortcutTriggerCount; });
			}

			QTest::keyClick(input, Qt::Key_P, Qt::ControlModifier);
			QCoreApplication::processEvents();
			QCOMPARE(view.inputText(), QStringLiteral("south"));

			QTest::keyClick(input, Qt::Key_P, Qt::ControlModifier);
			QCoreApplication::processEvents();
			QCOMPARE(view.inputText(), QStringLiteral("north"));

			QTest::keyClick(input, Qt::Key_N, Qt::ControlModifier);
			QCoreApplication::processEvents();
			QCOMPARE(view.inputText(), QStringLiteral("south"));

			QCOMPARE(view.setOutputScroll(0, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->minimum());
			QTest::keyClick(input, Qt::Key_Z, Qt::ControlModifier);
			QCoreApplication::processEvents();
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());
			QVERIFY(!view.isScrollbackSplitActive());
			QCOMPARE(shortcutTriggerCount, 0);

			resetTestState();
		}

		void commandOptionShortcutsOverrideConflictingShortcutsWithOutputFocus()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("ctrl_p_goes_to_previous_command"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("ctrl_n_goes_to_next_command"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("ctrl_z_goes_to_end_of_buffer"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.addToHistoryForced(QStringLiteral("north"));
			view.addToHistoryForced(QStringLiteral("south"));
			for (int i = 0; i < 320; ++i)
				view.appendOutputText(QStringLiteral("command-option-output-scroll-%1").arg(i), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());

			int       shortcutTriggerCount = 0;
			QShortcut ctrlPShortcut(QKeySequence(QStringLiteral("Ctrl+P")), &view);
			QShortcut ctrlNShortcut(QKeySequence(QStringLiteral("Ctrl+N")), &view);
			QShortcut ctrlZShortcut(QKeySequence(QStringLiteral("Ctrl+Z")), &view);
			for (QShortcut *shortcut : {&ctrlPShortcut, &ctrlNShortcut, &ctrlZShortcut})
			{
				shortcut->setContext(Qt::ApplicationShortcut);
				connect(shortcut, &QShortcut::activated, this,
				        [&shortcutTriggerCount] { ++shortcutTriggerCount; });
			}

			QTest::keyClick(viewport, Qt::Key_P, Qt::ControlModifier);
			QCoreApplication::processEvents();
			QCOMPARE(view.inputText(), QStringLiteral("south"));

			QTest::keyClick(viewport, Qt::Key_P, Qt::ControlModifier);
			QCoreApplication::processEvents();
			QCOMPARE(view.inputText(), QStringLiteral("north"));

			QTest::keyClick(viewport, Qt::Key_N, Qt::ControlModifier);
			QCoreApplication::processEvents();
			QCOMPARE(view.inputText(), QStringLiteral("south"));

			QCOMPARE(view.setOutputScroll(0, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->minimum());
			QTest::keyClick(viewport, Qt::Key_Z, Qt::ControlModifier);
			QCoreApplication::processEvents();
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());
			QVERIFY(!view.isScrollbackSplitActive());
			QCOMPARE(shortcutTriggerCount, 0);

			resetTestState();
		}

		void commandOptionShortcutDetectionRequiresEnabledOption()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QKeyEvent ctrlPDisabled(QEvent::ShortcutOverride, Qt::Key_P, Qt::ControlModifier);
			QVERIFY(!view.hasCommandOptionShortcut(&ctrlPDisabled));

			g_worldAttrs.insert(QStringLiteral("ctrl_p_goes_to_previous_command"), QStringLiteral("1"));
			view.applyRuntimeSettings();
			QKeyEvent ctrlPEnabled(QEvent::ShortcutOverride, Qt::Key_P, Qt::ControlModifier);
			QVERIFY(view.hasCommandOptionShortcut(&ctrlPEnabled));

			QKeyEvent ctrlShiftP(QEvent::ShortcutOverride, Qt::Key_P,
			                     Qt::ControlModifier | Qt::ShiftModifier);
			QVERIFY(!view.hasCommandOptionShortcut(&ctrlShiftP));

			resetTestState();
		}

		void commandHistoryShortcutsOverrideConflictingShortcutsWithInputFocus()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("arrow_recalls_partial"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.addToHistoryForced(QStringLiteral("hitman"));
			view.addToHistoryForced(QStringLiteral("hit 321"));
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());
			view.setInputText(QStringLiteral("hit"), true);

			int       shortcutTriggerCount = 0;
			QShortcut altUpShortcut(QKeySequence(QStringLiteral("Alt+Up")), &view);
			QShortcut altDownShortcut(QKeySequence(QStringLiteral("Alt+Down")), &view);
			for (QShortcut *shortcut : {&altUpShortcut, &altDownShortcut})
			{
				shortcut->setContext(Qt::ApplicationShortcut);
				connect(shortcut, &QShortcut::activated, this,
				        [&shortcutTriggerCount] { ++shortcutTriggerCount; });
			}

			QTest::keyClick(input, Qt::Key_Up, Qt::AltModifier);
			QCoreApplication::processEvents();
			QCOMPARE(view.inputText(), QStringLiteral("hit 321"));
			QCOMPARE(shortcutTriggerCount, 0);

			resetTestState();
		}

		void commandHistoryShortcutsOverrideConflictingShortcutsWithOutputFocus()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("arrow_recalls_partial"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.addToHistoryForced(QStringLiteral("hitman"));
			view.addToHistoryForced(QStringLiteral("hit 321"));
			view.appendOutputText(QStringLiteral("history-output-focus"), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);
			view.setInputText(QStringLiteral("hit"), true);

			int       shortcutTriggerCount = 0;
			QShortcut altUpShortcut(QKeySequence(QStringLiteral("Alt+Up")), &view);
			QShortcut altDownShortcut(QKeySequence(QStringLiteral("Alt+Down")), &view);
			for (QShortcut *shortcut : {&altUpShortcut, &altDownShortcut})
			{
				shortcut->setContext(Qt::ApplicationShortcut);
				connect(shortcut, &QShortcut::activated, this,
				        [&shortcutTriggerCount] { ++shortcutTriggerCount; });
			}

			QTest::keyClick(viewport, Qt::Key_Up, Qt::AltModifier);
			QCoreApplication::processEvents();
			QCOMPARE(view.inputText(), QStringLiteral("hit 321"));
			QCOMPARE(shortcutTriggerCount, 0);

			resetTestState();
		}

		void commandHistoryShortcutDetectionRequiresEnabledOptionAndExactAltArrow()
		{
			resetTestState();

			WorldView view;
			QKeyEvent altUp(QEvent::ShortcutOverride, Qt::Key_Up, Qt::AltModifier);
			QVERIFY(!view.hasCommandHistoryShortcut(&altUp));

			g_worldAttrs.insert(QStringLiteral("alt_arrow_recalls_partial"), QStringLiteral("1"));
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();

			QVERIFY(view.hasCommandHistoryShortcut(&altUp));

			QKeyEvent altDown(QEvent::ShortcutOverride, Qt::Key_Down, Qt::AltModifier);
			QVERIFY(view.hasCommandHistoryShortcut(&altDown));

			QKeyEvent plainUp(QEvent::ShortcutOverride, Qt::Key_Up, Qt::NoModifier);
			QVERIFY(!view.hasCommandHistoryShortcut(&plainUp));

			QKeyEvent ctrlAltUp(QEvent::ShortcutOverride, Qt::Key_Up, Qt::ControlModifier | Qt::AltModifier);
			QVERIFY(!view.hasCommandHistoryShortcut(&ctrlAltUp));

			resetTestState();
		}

		void pluginNumpadAcceleratorRequiresKeypadModifier()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.setAllTypingToCommandWindow(true);
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			constexpr Qt::Key key          = Qt::Key_6;
			constexpr quint16 numpadVk     = 0x66;
			constexpr int     commandId    = 88;
			const qint64      numpadMapKey = makeAcceleratorMapKey(key, Qt::AltModifier, numpadVk, true);
			g_acceleratorCommands.insert(numpadMapKey, commandId);
			constexpr quint64 keypadMapId = (static_cast<quint64>(static_cast<quint32>(key)) << 1) | 1ULL;
			g_virtualKeyMap.insert(keypadMapId, numpadVk);

			int       shortcutTriggerCount = 0;
			QShortcut conflictingShortcut(QKeySequence(QStringLiteral("Alt+6")), input);
			conflictingShortcut.setContext(Qt::WidgetWithChildrenShortcut);
			connect(&conflictingShortcut, &QShortcut::activated, this,
			        [&shortcutTriggerCount] { ++shortcutTriggerCount; });

			QTest::keyClick(input, key, Qt::AltModifier);
			QCoreApplication::processEvents();

			QCOMPARE(g_acceleratorExecutionCount, 0);
			QCOMPARE(g_lastExecutedAcceleratorCommand, -1);
			QCOMPARE(shortcutTriggerCount, 1);

			QTest::keyClick(input, key, Qt::AltModifier | Qt::KeypadModifier);
			QCoreApplication::processEvents();

			QCOMPARE(g_acceleratorExecutionCount, 1);
			QCOMPARE(g_lastExecutedAcceleratorCommand, commandId);

			resetTestState();
		}

		void windowsAltNumpadAcceleratorDoesNotInsertCommittedCharacter()
		{
#ifndef Q_OS_WIN
			QSKIP("Windows-specific Alt+Numpad behavior");
#else
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.setAllTypingToCommandWindow(true);
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			constexpr Qt::Key key          = Qt::Key_4;
			constexpr quint16 numpadVk     = 0x64;
			constexpr int     commandId    = 108;
			const qint64      numpadMapKey = makeAcceleratorMapKey(key, Qt::AltModifier, numpadVk, true);
			g_acceleratorCommands.insert(numpadMapKey, commandId);

			QKeyEvent keyPress(QEvent::KeyPress, key, Qt::AltModifier, 0, numpadVk, 0, QStringLiteral("4"),
			                   false, 1);
			QCoreApplication::sendEvent(input, &keyPress);
			QCoreApplication::processEvents();

			QCOMPARE(g_acceleratorExecutionCount, 1);
			QCOMPARE(g_lastExecutedAcceleratorCommand, commandId);

			QInputMethodEvent ime;
			ime.setCommitString(QStringLiteral("\u2666"));
			QCoreApplication::sendEvent(input, &ime);
			QCoreApplication::processEvents();

			QCOMPARE(input->toPlainText(), QString());

			resetTestState();
#endif
		}

		void windowsAltNumpadAcceleratorDoesNotInsertFollowupPrintableKeyPress()
		{
#ifndef Q_OS_WIN
			QSKIP("Windows-specific Alt+Numpad behavior");
#else
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.setAllTypingToCommandWindow(true);
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			constexpr Qt::Key key          = Qt::Key_4;
			constexpr quint16 numpadVk     = 0x64;
			constexpr int     commandId    = 109;
			const qint64      numpadMapKey = makeAcceleratorMapKey(key, Qt::AltModifier, numpadVk, true);
			g_acceleratorCommands.insert(numpadMapKey, commandId);

			QKeyEvent acceleratorPress(QEvent::KeyPress, key, Qt::AltModifier, 0, numpadVk, 0,
			                           QStringLiteral("4"), false, 1);
			QCoreApplication::sendEvent(input, &acceleratorPress);
			QCoreApplication::processEvents();

			QCOMPARE(g_acceleratorExecutionCount, 1);
			QCOMPARE(g_lastExecutedAcceleratorCommand, commandId);

			QKeyEvent followupPrintable(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, 0, 0, 0,
			                            QStringLiteral("\u2666"), false, 1);
			QCoreApplication::sendEvent(input, &followupPrintable);
			QCoreApplication::processEvents();

			QCOMPARE(input->toPlainText(), QString());

			resetTestState();
#endif
		}

		void windowsAltNumpadAcceleratorDoesNotInsertFollowupPrintableKeyPressWithAltModifier()
		{
#ifndef Q_OS_WIN
			QSKIP("Windows-specific Alt+Numpad behavior");
#else
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.setAllTypingToCommandWindow(true);
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			constexpr Qt::Key key          = Qt::Key_4;
			constexpr quint16 numpadVk     = 0x64;
			constexpr int     commandId    = 110;
			const qint64      numpadMapKey = makeAcceleratorMapKey(key, Qt::AltModifier, numpadVk, true);
			g_acceleratorCommands.insert(numpadMapKey, commandId);

			QKeyEvent acceleratorPress(QEvent::KeyPress, key, Qt::AltModifier, 0, numpadVk, 0,
			                           QStringLiteral("4"), false, 1);
			QCoreApplication::sendEvent(input, &acceleratorPress);
			QCoreApplication::processEvents();

			QCOMPARE(g_acceleratorExecutionCount, 1);
			QCOMPARE(g_lastExecutedAcceleratorCommand, commandId);

			QKeyEvent followupPrintable(QEvent::KeyPress, Qt::Key_unknown, Qt::AltModifier, 0, 0, 0,
			                            QStringLiteral("\u2666"), false, 1);
			QCoreApplication::sendEvent(input, &followupPrintable);
			QCoreApplication::processEvents();

			QCOMPARE(input->toPlainText(), QString());

			resetTestState();
#endif
		}

		void topRowDigitDoesNotTriggerNumpadAccelerator()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.setAllTypingToCommandWindow(true);
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			constexpr Qt::Key key          = Qt::Key_8;
			constexpr quint16 numpadVk     = 0x68;
			constexpr quint16 topRowVk     = 0x38;
			constexpr int     commandId    = 99;
			const qint64      numpadMapKey = makeAcceleratorMapKey(key, Qt::NoModifier, numpadVk, true);
			g_acceleratorCommands.insert(numpadMapKey, commandId);
			constexpr quint64 topRowMapId = static_cast<quint64>(static_cast<quint32>(key)) << 1;
			g_virtualKeyMap.insert(topRowMapId, topRowVk);

			QTest::keyClick(input, key, Qt::NoModifier);
			QCoreApplication::processEvents();

			QCOMPARE(g_acceleratorExecutionCount, 0);
			QCOMPARE(g_lastExecutedAcceleratorCommand, -1);
			QCOMPARE(input->toPlainText(), QStringLiteral("8"));

			resetTestState();
		}

		void
		outputNavigationKeysFromInputUseSplitPagingAndCtrlShiftHomeEndWhenAllTypingToCommandWindowEnabled()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.setAllTypingToCommandWindow(true);
			QCoreApplication::processEvents();

			for (int i = 0; i < 320; ++i)
				view.appendOutputText(QStringLiteral("input-key-scroll-%1").arg(i), true);
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			int       shortcutTriggerCount = 0;
			QShortcut pageUpShortcut(QKeySequence(QStringLiteral("PageUp")), &view);
			QShortcut pageDownShortcut(QKeySequence(QStringLiteral("PageDown")), &view);
			for (QShortcut *shortcut : {&pageUpShortcut, &pageDownShortcut})
			{
				shortcut->setContext(Qt::ApplicationShortcut);
				connect(shortcut, &QShortcut::activated, this,
				        [&shortcutTriggerCount] { ++shortcutTriggerCount; });
			}

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());
			QVERIFY(!view.isScrollbackSplitActive());

			QCOMPARE(view.setOutputScroll(999999, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			QTest::keyClick(input, Qt::Key_PageUp);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isScrollbackSplitActive());
			QCOMPARE(shortcutTriggerCount, 0);
			auto [splitTop, splitBottom] = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			QScrollBar *topBar = splitTop->verticalScrollBar();
			QVERIFY(topBar);
			QTRY_VERIFY(topBar->value() < topBar->maximum());

			topBar->setValue(topBar->minimum());
			QTest::keyClick(input, Qt::Key_PageDown);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isScrollbackSplitActive());
			QCOMPARE(shortcutTriggerCount, 0);
			QTRY_VERIFY(topBar->value() > topBar->minimum());

			const int livePaneHeight = qMax(1, splitBottom->viewport()->height());
			QVERIFY(topBar->maximum() > livePaneHeight);
			const int splitMergeZoneValue =
			    qMax(topBar->minimum(), topBar->maximum() - qMax(1, livePaneHeight / 2));
			QVERIFY(splitMergeZoneValue < topBar->maximum());
			topBar->setValue(splitMergeZoneValue);
			QTest::keyClick(input, Qt::Key_PageDown);
			QCoreApplication::processEvents();
			QTRY_VERIFY(!view.isScrollbackSplitActive());
			QCOMPARE(shortcutTriggerCount, 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			view.setInputText(QStringLiteral("home-end-input"), true);
			QCoreApplication::processEvents();
			QCOMPARE(input->textCursor().position(), 14);
			const int scrollBeforePlainHomeEnd = view.outputScrollPosition();

			QTest::keyClick(input, Qt::Key_End);
			QCoreApplication::processEvents();
			QCOMPARE(input->textCursor().position(), 14);
			QCOMPARE(view.outputScrollPosition(), scrollBeforePlainHomeEnd);

			QTest::keyClick(input, Qt::Key_Home);
			QCoreApplication::processEvents();
			QCOMPARE(input->textCursor().position(), 0);
			QCOMPARE(view.outputScrollPosition(), scrollBeforePlainHomeEnd);

			QTest::keyClick(input, Qt::Key_Home, Qt::ShiftModifier);
			QCoreApplication::processEvents();
			QVERIFY(!view.isScrollbackSplitActive());
			QCOMPARE(view.outputScrollPosition(), scrollBeforePlainHomeEnd);

			QTest::keyClick(input, Qt::Key_Home, Qt::ControlModifier | Qt::ShiftModifier);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isScrollbackSplitActive());
			std::tie(splitTop, splitBottom) = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			topBar = splitTop->verticalScrollBar();
			QVERIFY(topBar);
			QTRY_COMPARE(topBar->value(), topBar->minimum());

			QTest::keyClick(input, Qt::Key_End, Qt::ShiftModifier);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isScrollbackSplitActive());
			QTRY_COMPARE(topBar->value(), topBar->minimum());

			QTest::keyClick(input, Qt::Key_End, Qt::ControlModifier | Qt::ShiftModifier);
			QCoreApplication::processEvents();
			QTRY_VERIFY(!view.isScrollbackSplitActive());
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			resetTestState();
		}

		void outputNavigationKeysFromInputUseEffectiveShortcutOverrides()
		{
			resetTestState();
			g_useFakeAppController = true;
			g_globalOptions.insert(QStringLiteral("Shortcut.DisplayStart"), QStringLiteral("PgUp"));

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.setAllTypingToCommandWindow(true);
			view.applyShortcutPreferences();
			QCoreApplication::processEvents();

			for (int i = 0; i < 320; ++i)
				view.appendOutputText(QStringLiteral("input-effective-shortcut-scroll-%1").arg(i), true);
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			int       shortcutTriggerCount = 0;
			QShortcut pageUpShortcut(QKeySequence(QStringLiteral("PageUp")), &view);
			pageUpShortcut.setContext(Qt::ApplicationShortcut);
			connect(&pageUpShortcut, &QShortcut::activated, this,
			        [&shortcutTriggerCount] { ++shortcutTriggerCount; });

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QTRY_VERIFY(bar->maximum() > bar->minimum());

			QCOMPARE(view.setOutputScroll(999999, true), 0);
			QTRY_COMPARE(view.outputScrollPosition(), bar->maximum());

			QTest::keyClick(input, Qt::Key_PageUp);
			QCoreApplication::processEvents();

			QCOMPARE(shortcutTriggerCount, 0);
			QTRY_VERIFY(view.isScrollbackSplitActive());
			auto [splitTop, splitBottom] = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			QScrollBar *topBar = splitTop->verticalScrollBar();
			QVERIFY(topBar);
			QTRY_COMPARE(topBar->value(), topBar->minimum());

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

		void escapeAlwaysCollapsesSplit_data()
		{
			QTest::addColumn<bool>("escapeDeletesInput");

			QTest::newRow("escape-deletes-input-disabled") << false;
			QTest::newRow("escape-deletes-input-enabled") << true;
		}

		void escapeAlwaysCollapsesSplit()
		{
			QFETCH(bool, escapeDeletesInput);
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("escape_deletes_input"),
			                    escapeDeletesInput ? QStringLiteral("1") : QStringLiteral("0"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 300; ++i)
				view.appendOutputText(QStringLiteral("split-escape-%1").arg(i), true);
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

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			view.setInputText(QStringLiteral("escape-collapse-input"), true);
			input->setFocus();
			QTest::keyClick(input, Qt::Key_Escape);
			QCoreApplication::processEvents();

			QTRY_VERIFY(!view.isScrollbackSplitActive());
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
			QScrollBar *splitTopBar = splitTop->verticalScrollBar();
			QVERIFY(splitTopBar);
			QScrollBar *liveBar = splitBottom->verticalScrollBar();
			QVERIFY(liveBar);
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());
			const int topBeforeAppend = splitTopBar->value();

			view.appendOutputText(QStringLiteral("split-live-stick-new"), true);
			QCoreApplication::processEvents();
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());
			QCOMPARE(splitTopBar->value(), topBeforeAppend);

			resetTestState();
		}

		void scrollbackSplitTopPaneStaysAnchoredDuringHeadTrimAppends()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("220"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 220; ++i)
				view.appendOutputText(QStringLiteral("split-anchor-%1").arg(i), true);
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
			QScrollBar *liveBar = splitBottom->verticalScrollBar();
			QVERIFY(liveBar);
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());

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
			const QString beforeAnchorWord = selectWordInView(splitTop, topProbePoints);
			QVERIFY(beforeAnchorWord.startsWith(QStringLiteral("split-anchor-")));

			for (int i = 0; i < 40; ++i)
				view.appendOutputText(QStringLiteral("split-anchor-tail-%1").arg(i), true);
			QCoreApplication::processEvents();
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());

			const QString afterAnchorWord = selectWordInView(splitTop, topProbePoints);
			QCOMPARE(afterAnchorWord, beforeAnchorWord);

			resetTestState();
		}

		void scrollbackSplitTopPanePreservesWrappedAnchorDuringVariableHeadTrimAppends()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("220"));
			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("1"));

			WorldView view;
			view.resize(540, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 60; ++i)
			{
				view.appendOutputText(QStringLiteral("split-variable-trim-%1 ").arg(i) +
				                          QStringLiteral("wide text ").repeated(48),
				                      true);
			}
			for (int i = 0; i < 160; ++i)
				view.appendOutputText(QStringLiteral("split-variable-anchor-%1").arg(i), true);
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
			QScrollBar *topBar = splitTop->verticalScrollBar();
			QVERIFY(topBar);
			QScrollBar *liveBar = splitBottom->verticalScrollBar();
			QVERIFY(liveBar);
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());

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
			    QPoint(24, qMax(8, splitTop->viewport()->rect().height() / 4)),
			    splitTop->viewport()->rect().center(),
			    QPoint(24, qMax(8, (splitTop->viewport()->rect().height() * 3) / 4)),
			};
			QString beforeAnchorWord;
			for (int attempt = 0; attempt <= 12 && beforeAnchorWord.isEmpty(); ++attempt)
			{
				const int targetValue =
				    topBar->minimum() + ((topBar->maximum() - topBar->minimum()) * attempt) / 12;
				topBar->setValue(targetValue);
				QCoreApplication::processEvents();
				const QString candidate = selectWordInView(splitTop, topProbePoints);
				if (candidate.startsWith(QStringLiteral("split-variable-anchor-")))
					beforeAnchorWord = candidate;
			}
			QVERIFY(beforeAnchorWord.startsWith(QStringLiteral("split-variable-anchor-")));

			for (int i = 0; i < 60; ++i)
				view.appendOutputText(QStringLiteral("split-variable-tail-%1").arg(i), true);
			QCoreApplication::processEvents();
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());

			const QString afterAnchorWord = selectWordInView(splitTop, topProbePoints);
			QCOMPARE(afterAnchorWord, beforeAnchorWord);

			resetTestState();
		}

		void scrollbackSplitTopPaneStaysAnchoredDuringCappedPluginPromptCycles()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("80"));

			WorldView view;
			view.resize(540, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 80; ++i)
				view.appendOutputText(QStringLiteral("split-plugin-anchor-%1").arg(i), true);
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
			QScrollBar *liveBar = splitBottom->verticalScrollBar();
			QVERIFY(liveBar);

			auto selectAnchorWord = [&view, splitTop]()
			{
				const QVector<QPoint> points{
				    QPoint(24, qMax(8, splitTop->viewport()->rect().height() / 4)),
				    splitTop->viewport()->rect().center(),
				    QPoint(24, qMax(8, (splitTop->viewport()->rect().height() * 3) / 4)),
				};
				for (const QPoint &point : points)
				{
					QTest::mouseDClick(splitTop->viewport(), Qt::LeftButton, Qt::NoModifier, point);
					QCoreApplication::processEvents();
					if (view.outputSelectionText().startsWith(QStringLiteral("split-plugin-anchor-")))
						return view.outputSelectionText();
				}
				return QString{};
			};

			QString     beforeAnchorWord;
			QScrollBar *topBar = splitTop->verticalScrollBar();
			QVERIFY(topBar);
			topBar->setValue(topBar->maximum());
			QCoreApplication::processEvents();
			beforeAnchorWord = selectAnchorWord();
			QVERIFY(beforeAnchorWord.startsWith(QStringLiteral("split-plugin-anchor-")));

			for (int cycle = 0; cycle < 8; ++cycle)
			{
				view.appendOutputText(QStringLiteral("split-plugin-room-%1").arg(cycle), true);
				view.updatePartialOutputText(QStringLiteral("<split-plugin-prompt-%1> ").arg(cycle));
				view.appendNoteText(QStringLiteral("W"), false);
				view.appendNoteText(QStringLiteral(" <---("), false);
				view.appendNoteText(QStringLiteral("M"), false);
				view.appendNoteText(QStringLiteral(")---> "), false);
				view.appendNoteText(QStringLiteral("E"), true);
				view.updatePartialOutputText(QStringLiteral("<split-plugin-prompt-%1> ").arg(cycle));
				view.appendOutputText(QStringLiteral("<split-plugin-prompt-%1> look").arg(cycle), true);
				QCoreApplication::processEvents();
			}
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());

			const QString afterAnchorWord = selectAnchorWord();
			QCOMPARE(afterAnchorWord, beforeAnchorWord);
			resetTestState();
		}

		void selectionPersistsDuringCappedPluginPromptCycles()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("80"));

			WorldView view;
			view.resize(540, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 80; ++i)
				view.appendOutputText(QStringLiteral("select-plugin-anchor-%1").arg(i), true);
			QCoreApplication::processEvents();

			const QStringList lines = view.outputLines();
			QVERIFY(lines.size() >= 68);
			constexpr int  selectedLineIndex = 64;
			const QString &selectedText      = lines.at(selectedLineIndex);
			view.selectOutputRange(selectedLineIndex, 0, sizeToInt(selectedText.size()));
			QTRY_COMPARE(view.outputSelectionText(), selectedText);

			for (int cycle = 0; cycle < 6; ++cycle)
			{
				view.appendOutputText(QStringLiteral("select-plugin-room-%1").arg(cycle), true);
				view.updatePartialOutputText(QStringLiteral("<select-plugin-prompt-%1> ").arg(cycle));
				view.appendNoteText(QStringLiteral("W"), false);
				view.appendNoteText(QStringLiteral(" <---("), false);
				view.appendNoteText(QStringLiteral("M"), false);
				view.appendNoteText(QStringLiteral(")---> "), false);
				view.appendNoteText(QStringLiteral("E"), true);
				view.updatePartialOutputText(QStringLiteral("<select-plugin-prompt-%1> ").arg(cycle));
				view.appendOutputText(QStringLiteral("<select-plugin-prompt-%1> look").arg(cycle), true);
				QCoreApplication::processEvents();
			}

			QTRY_VERIFY(view.hasOutputSelection());
			QTRY_COMPARE(view.outputSelectionText(), selectedText);
			QVERIFY(view.outputLines().contains(selectedText));
			resetTestState();
		}

		void splitTopSelectionPersistsDuringCappedPluginPromptCycles()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("80"));

			WorldView view;
			view.resize(540, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 80; ++i)
				view.appendOutputText(QStringLiteral("split-select-plugin-anchor-%1").arg(i), true);
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
			QScrollBar *topBar = splitTop->verticalScrollBar();
			QVERIFY(topBar);
			topBar->setValue(topBar->maximum());
			QCoreApplication::processEvents();

			const QVector<QPoint> points{
			    QPoint(24, qMax(8, splitTop->viewport()->rect().height() / 4)),
			    splitTop->viewport()->rect().center(),
			    QPoint(24, qMax(8, (splitTop->viewport()->rect().height() * 3) / 4)),
			};
			QString selectedText;
			for (const QPoint &point : points)
			{
				QTest::mouseDClick(splitTop->viewport(), Qt::LeftButton, Qt::NoModifier, point);
				QCoreApplication::processEvents();
				if (view.outputSelectionText().startsWith(QStringLiteral("split-select-plugin-anchor-")))
				{
					selectedText = view.outputSelectionText();
					break;
				}
			}
			QVERIFY(selectedText.startsWith(QStringLiteral("split-select-plugin-anchor-")));

			for (int cycle = 0; cycle < 6; ++cycle)
			{
				view.appendOutputText(QStringLiteral("split-select-plugin-room-%1").arg(cycle), true);
				view.updatePartialOutputText(QStringLiteral("<split-select-plugin-prompt-%1> ").arg(cycle));
				view.appendNoteText(QStringLiteral("W"), false);
				view.appendNoteText(QStringLiteral(" <---("), false);
				view.appendNoteText(QStringLiteral("M"), false);
				view.appendNoteText(QStringLiteral(")---> "), false);
				view.appendNoteText(QStringLiteral("E"), true);
				view.updatePartialOutputText(QStringLiteral("<split-select-plugin-prompt-%1> ").arg(cycle));
				view.appendOutputText(QStringLiteral("<split-select-plugin-prompt-%1> look").arg(cycle),
				                      true);
				QCoreApplication::processEvents();
			}

			QTRY_VERIFY(view.hasOutputSelection());
			QTRY_COMPARE(view.outputSelectionText(), selectedText);
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

		void scrollCommandsTargetTopPaneAndCollapseAtEndWhenSplitActive()
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

			const int liveBefore = liveBar->value();
			view.scrollOutputToStart();
			QTRY_COMPARE(topBar->value(), topBar->minimum());
			QCOMPARE(liveBar->value(), liveBefore);

			view.scrollOutputLineDown();
			QTRY_VERIFY(topBar->value() > topBar->minimum());
			QCOMPARE(liveBar->value(), liveBefore);

			const int topBeforePageDown = topBar->value();
			view.scrollOutputPageDown();
			QTRY_VERIFY(topBar->value() > topBeforePageDown);
			QCOMPARE(liveBar->value(), liveBefore);

			topBar->setValue(topBar->maximum());
			view.scrollOutputPageDown();
			QTRY_VERIFY(!view.isScrollbackSplitActive());
			QTRY_COMPARE(view.outputScrollPosition(), topBar->maximum());

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

		void splitSelectionDragSpansTopAndLivePanes()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 280; ++i)
				view.appendOutputText(QStringLiteral("cross-pane-select-%1").arg(i, 3, 10, QLatin1Char('0')),
				                      true);
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

			auto findWordPoint = [&view](const QTextBrowser *target, const QVector<QPoint> &points)
			{
				for (const QPoint &point : points)
				{
					QTest::mouseDClick(target->viewport(), Qt::LeftButton, Qt::NoModifier, point);
					QCoreApplication::processEvents();
					const QString selected = view.outputSelectionText();
					if (selected.startsWith(QStringLiteral("cross-pane-select-")))
						return QPair<QString, QPoint>{selected, point};
				}
				return QPair<QString, QPoint>{};
			};

			const QVector<QPoint> topProbePoints{
			    QPoint(24, qMax(8, splitTop->viewport()->rect().height() / 2)),
			    QPoint(24, qMax(8, (splitTop->viewport()->rect().height() * 3) / 4)),
			    splitTop->viewport()->rect().center(),
			};
			const QVector<QPoint> bottomProbePoints{
			    QPoint(24, qMax(8, splitBottom->viewport()->rect().height() / 3)),
			    splitBottom->viewport()->rect().center(),
			    QPoint(24, qMax(8, splitBottom->viewport()->rect().height() - 12)),
			};

			const auto [topWord, topWordPoint]       = findWordPoint(splitTop, topProbePoints);
			const auto [bottomWord, bottomWordPoint] = findWordPoint(splitBottom, bottomProbePoints);
			QVERIFY(topWord.startsWith(QStringLiteral("cross-pane-select-")));
			QVERIFY(bottomWord.startsWith(QStringLiteral("cross-pane-select-")));
			QVERIFY(topWord != bottomWord);

			const QPoint startPoint(1, topWordPoint.y());
			const QPoint endPoint(qMax(1, splitBottom->viewport()->width() - 2), bottomWordPoint.y());
			const QPoint endGlobal = splitBottom->viewport()->mapToGlobal(endPoint);
			const QPoint endInTop  = splitTop->viewport()->mapFromGlobal(endGlobal);

			QTest::mousePress(splitTop->viewport(), Qt::LeftButton, Qt::NoModifier, startPoint);
			QCoreApplication::processEvents();
			QMouseEvent moveEvent(QEvent::MouseMove, QPointF(endInTop), QPointF(endGlobal), Qt::NoButton,
			                      Qt::LeftButton, Qt::NoModifier);
			QCoreApplication::sendEvent(splitTop->viewport(), &moveEvent);
			QCoreApplication::processEvents();
			QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(endInTop), QPointF(endGlobal),
			                         Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
			QCoreApplication::sendEvent(splitTop->viewport(), &releaseEvent);
			QCoreApplication::processEvents();

			const QString selectedText = view.outputSelectionText();
			QVERIFY(selectedText.contains(topWord));
			QVERIFY(selectedText.contains(bottomWord));
			QVERIFY(selectedText.contains(QLatin1Char('\n')));

			view.copySelection();
			QTRY_COMPARE(QGuiApplication::clipboard()->text(), selectedText);

			const QPoint reverseStartPoint(qMax(1, splitBottom->viewport()->width() - 2),
			                               bottomWordPoint.y());
			const QPoint reverseEndPoint(1, topWordPoint.y());
			const QPoint reverseEndGlobal   = splitTop->viewport()->mapToGlobal(reverseEndPoint);
			const QPoint reverseEndInBottom = splitBottom->viewport()->mapFromGlobal(reverseEndGlobal);

			QTest::mousePress(splitBottom->viewport(), Qt::LeftButton, Qt::NoModifier, reverseStartPoint);
			QCoreApplication::processEvents();
			QMouseEvent reverseMoveEvent(QEvent::MouseMove, QPointF(reverseEndInBottom),
			                             QPointF(reverseEndGlobal), Qt::NoButton, Qt::LeftButton,
			                             Qt::NoModifier);
			QCoreApplication::sendEvent(splitBottom->viewport(), &reverseMoveEvent);
			QCoreApplication::processEvents();
			QMouseEvent reverseReleaseEvent(QEvent::MouseButtonRelease, QPointF(reverseEndInBottom),
			                                QPointF(reverseEndGlobal), Qt::LeftButton, Qt::NoButton,
			                                Qt::NoModifier);
			QCoreApplication::sendEvent(splitBottom->viewport(), &reverseReleaseEvent);
			QCoreApplication::processEvents();

			const QString reverseSelectedText = view.outputSelectionText();
			QVERIFY(reverseSelectedText.contains(topWord));
			QVERIFY(reverseSelectedText.contains(bottomWord));
			QVERIFY(reverseSelectedText.contains(QLatin1Char('\n')));

			view.copySelection();
			QTRY_COMPARE(QGuiApplication::clipboard()->text(), reverseSelectedText);
			resetTestState();
		}

		void selectionTracksAcrossHeadTrimWhileVisibleThenPersistsOutOfViewport()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("90"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 90; ++i)
				view.appendOutputText(QStringLiteral("trim-track-%1").arg(i, 3, 10, QLatin1Char('0')), true);
			QCoreApplication::processEvents();

			const QStringList lines = view.outputLines();
			QVERIFY(lines.size() >= 12);
			const qsizetype selectedLineIndex = lines.size() - 8;
			const QString  &selectedText      = lines.at(selectedLineIndex);
			view.selectOutputRange(boundedSizeToInt(selectedLineIndex), 0,
			                       boundedSizeToInt(selectedText.size()));
			QTRY_COMPARE(view.outputSelectionText(), selectedText);

			view.appendOutputText(QStringLiteral("trim-track-tail-primer"), true);
			QCoreApplication::processEvents();
			QTRY_COMPARE(view.outputSelectionText(), selectedText);

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			const int lineHeight              = qMax(1, QFontMetrics(browser->font()).lineSpacing());
			const int visibleLinesEstimate    = qMax(1, bar->pageStep() / lineHeight);
			const int appendCountForOutOfView = visibleLinesEstimate + 8;
			for (int i = 0; i < appendCountForOutOfView; ++i)
				view.appendOutputText(QStringLiteral("trim-track-tail-%1").arg(i), true);
			QCoreApplication::processEvents();

			QTRY_VERIFY(view.hasOutputSelection());
			QTRY_COMPARE(view.outputSelectionText(), selectedText);
			QVERIFY(view.outputLines().contains(selectedText));
			resetTestState();
		}

		void selectionPersistsWhenManualScrollMovesSelectedRangeOutOfView()
		{
			resetTestState();

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 260; ++i)
				view.appendOutputText(QStringLiteral("manual-clear-%1").arg(i, 3, 10, QLatin1Char('0')),
				                      true);
			QCoreApplication::processEvents();

			const QStringList lines = view.outputLines();
			QVERIFY(lines.size() >= 4);
			const qsizetype selectedLineIndex = lines.size() - 2;
			const QString  &selectedText      = lines.at(selectedLineIndex);
			view.selectOutputRange(boundedSizeToInt(selectedLineIndex), 0,
			                       boundedSizeToInt(selectedText.size()));
			QTRY_COMPARE(view.outputSelectionText(), selectedText);

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			bar->setValue(bar->minimum());
			QCoreApplication::processEvents();

			QTRY_VERIFY(view.hasOutputSelection());
			QTRY_COMPARE(view.outputSelectionText(), selectedText);
			QVERIFY(view.outputLines().contains(selectedText));
			resetTestState();
		}

		void clearingOffscreenSelectionDoesNotRequestFullNativeRepaint()
		{
			resetTestState();

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 260; ++i)
				view.appendOutputText(QStringLiteral("offscreen-repaint-%1").arg(i, 3, 10, QLatin1Char('0')),
				                      true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *bar = browser->verticalScrollBar();
			QVERIFY(bar);
			QVERIFY(view.m_nativeOutputCanvas);

			view.m_nativeOutputCanvas->repaint();
			QCoreApplication::processEvents();
			QVERIFY(view.m_nativePrimaryPaintState.valid);

			const QStringList lines = view.outputLines();
			QVERIFY(lines.size() >= 4);
			const qsizetype selectedLineIndex = lines.size() - 2;
			const QString  &selectedText      = lines.at(selectedLineIndex);
			view.selectOutputRange(boundedSizeToInt(selectedLineIndex), 0,
			                       boundedSizeToInt(selectedText.size()));
			QTRY_COMPARE(view.outputSelectionText(), selectedText);

			bar->setValue(bar->minimum());
			QCoreApplication::processEvents();
			view.m_nativeOutputCanvas->repaint();
			QCoreApplication::processEvents();
			QVERIFY(view.hasOutputSelection());
			QCOMPARE(view.outputSelectionText(), selectedText);
			QVERIFY(!view.nativeOutputSelectionRepaintRect(view.m_nativeOutputSelection).isValid());

			view.m_nativeOutputRepaintQueued = false;
			view.m_nativeOutputRepaintAll    = false;
			view.m_nativeOutputRepaintRegion = {};
			view.clearNativeOutputSelection(true);
			QVERIFY(!view.m_nativeOutputRepaintQueued);
			QVERIFY(!view.m_nativeOutputRepaintAll);
			QVERIFY(view.m_nativeOutputRepaintRegion.isEmpty());
			QVERIFY(!view.hasOutputSelection());
			resetTestState();
		}

		void splitTopSelectionPersistsWhenManualScrollMovesItOutOfView()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("auto_pause"), QStringLiteral("1"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 320; ++i)
				view.appendOutputText(QStringLiteral("split-clear-%1").arg(i, 3, 10, QLatin1Char('0')), true);
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
			QScrollBar *topBar = splitTop->verticalScrollBar();
			QVERIFY(topBar);

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
			const QString selectedText = selectWordInView(splitTop, topProbePoints);
			QVERIFY(!selectedText.isEmpty());
			QVERIFY(selectedText.startsWith(QStringLiteral("split-clear-")));

			const int startValue = topBar->value();
			const int step       = qMax(1, topBar->pageStep());
			topBar->setValue(qMin(topBar->maximum(), startValue + (step * 2)));
			QCoreApplication::processEvents();

			QTRY_VERIFY(view.hasOutputSelection());
			QTRY_COMPARE(view.outputSelectionText(), selectedText);
			QVERIFY(view.outputLines().contains(selectedText));
			resetTestState();
		}

		void collapsedSplitPreservesLivePaneSelection()
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
			QString selectedText;
			for (const QPoint &point : liveProbePoints)
			{
				QTest::mouseDClick(splitBottom->viewport(), Qt::LeftButton, Qt::NoModifier, point);
				QCoreApplication::processEvents();
				if (view.hasOutputSelection() && !view.outputSelectionText().isEmpty())
				{
					selectedText = view.outputSelectionText();
					break;
				}
			}
			QVERIFY(!selectedText.isEmpty());

			for (int i = 0; i < 120 && view.isScrollbackSplitActive(); ++i)
			{
				QWheelEvent wheelDown(localPos, globalPos, QPoint(0, 0), QPoint(0, -120), Qt::NoButton,
				                      Qt::NoModifier, Qt::NoScrollPhase, false);
				QCoreApplication::sendEvent(topBrowser->viewport(), &wheelDown);
				QCoreApplication::processEvents();
			}
			QTRY_VERIFY(!view.isScrollbackSplitActive());
			QTRY_COMPARE(view.outputSelectionText(), selectedText);
			QVERIFY(view.hasOutputSelection());
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
			span.length = boundedSizeToInt(QStringLiteral("styled-background").size());
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

		void nativeOutputRendererRuntimeOutputUsesConfiguredWrapColumn()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("wrap_column"), QStringLiteral("12"));
			g_worldAttrs.insert(QStringLiteral("indent_paras"), QStringLiteral("0"));

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			appendFakeRuntimeOutputText(view, QStringLiteral("alpha beta gamma\nshort"), {}, true, true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const QStringList lines = view.outputLines();
			QCOMPARE(lines.size(), 3);
			QCOMPARE(lines.at(0), QStringLiteral("alpha beta "));
			QCOMPARE(lines.at(1), QStringLiteral("gamma"));
			QCOMPARE(lines.at(2), QStringLiteral("short"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 3);
			resetTestState();
		}

		void nativeOutputRendererLuaStyleNoteRecordsThroughRuntimeLineBuffer()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			view.appendNoteText(QStringLiteral("lua-note-path"), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QCOMPARE(g_runtimeLines.size(), 1);
			QCOMPARE(g_runtimeLines.constFirst().text, QStringLiteral("lua-note-path"));
			QVERIFY((g_runtimeLines.constFirst().flags & WorldRuntime::LineNote) != 0);
			QVERIFY(g_runtimeLines.constFirst().hardReturn);
			QVERIFY(!g_runtimeLines.constFirst().spans.isEmpty());
			QCOMPARE(view.outputLines(), QStringList{QStringLiteral("lua-note-path")});
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("lua-note-path"));
			resetTestState();
		}

		void nativeOutputRendererStyledPluginOutputUsesConfiguredWrapColumn()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("wrap_column"), QStringLiteral("12"));
			g_worldAttrs.insert(QStringLiteral("indent_paras"), QStringLiteral("0"));

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			const QString text = QStringLiteral("alpha beta gamma");
			appendFakeRuntimeOutputText(
			    view, text,
			    {makeFullSpan(text, QColor(QStringLiteral("#ff0000")), QColor(QStringLiteral("#000000")))},
			    true, true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const QStringList lines = view.outputLines();
			QCOMPARE(lines.size(), 2);
			QCOMPARE(lines.at(0), QStringLiteral("alpha beta "));
			QCOMPARE(lines.at(1), QStringLiteral("gamma"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 2);
			resetTestState();
		}

		void nativeOutputRendererNawsEnabledPluginOutputUsesAutomaticLocalWrapColumn()
		{
			resetTestState();

			g_connected      = true;
			g_nawsNegotiated = true;
			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("wrap_column"), QStringLiteral("40"));
			g_worldAttrs.insert(QStringLiteral("indent_paras"), QStringLiteral("0"));

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			appendFakeRuntimeOutputText(view,
			                            QStringLiteral("one two three four five six seven eight nine ten "
			                                           "eleven twelve thirteen fourteen!"),
			                            {}, true, true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const QStringList lines = view.outputLines();
			QVERIFY(lines.size() >= 2);
			for (const QString &line : lines)
				QVERIFY(line.size() <= 80);
			QVERIFY(lines.join(QLatin1Char(' ')).contains(QStringLiteral("fourteen")));
			resetTestState();
		}

		void nativeOutputRendererComposesPluginFragmentsUntilExplicitHardBreak()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntime(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			QVector<WorldRuntime::LineEntry> lines{
			    makeRuntimeLine(QStringLiteral("A Trail of Two Tails"), WorldRuntime::LineOutput, true, 100),
			    makeRuntimeLine(QStringLiteral("W"), WorldRuntime::LineNote, false, 101),
			    makeRuntimeLine(QStringLiteral(" <---("), WorldRuntime::LineNote, false, 102),
			    makeRuntimeLine(QStringLiteral("M"), WorldRuntime::LineNote, false, 103),
			    makeRuntimeLine(QStringLiteral(")---> "), WorldRuntime::LineNote, false, 104),
			    makeRuntimeLine(QStringLiteral("E"), WorldRuntime::LineNote, false, 105),
			    makeRuntimeLine(QString(), WorldRuntime::LineNote, true, 106),
			    makeRuntimeLine(QStringLiteral("<2060hp 1694sp 1595st>"), WorldRuntime::LineOutput, true,
			                    107)};
			g_runtimeLines = lines;

			view.restoreOutputFromPersistedLines(lines);
			nativeCanvas->repaint();
			QCoreApplication::processEvents();

			QTRY_COMPARE(view.outputLines().size(), 3);
			QCOMPARE(view.outputLines().at(0), QStringLiteral("A Trail of Two Tails"));
			QCOMPARE(view.outputLines().at(1), QStringLiteral("W <---(M)---> E"));
			QCOMPARE(view.outputLines().at(2), QStringLiteral("<2060hp 1694sp 1595st>"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("<2060hp 1694sp 1595st>"));
			resetTestState();
		}

		void nativeOutputRendererComposesLivePluginFragmentsBeforePrompt()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			view.appendOutputText(QStringLiteral("A Trail of Two Tails"), true);
			view.appendNoteText(QStringLiteral("W"), false);
			view.appendNoteText(QStringLiteral(" <---("), false);
			view.appendNoteText(QStringLiteral("M"), false);
			view.appendNoteText(QStringLiteral(")---> "), false);
			view.appendNoteText(QStringLiteral("E"), false);
			view.appendNoteText(QString(), true);
			view.appendOutputText(QStringLiteral("<2060hp 1694sp 1595st>"), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(view.outputLines().size(), 3);
			QCOMPARE(view.outputLines().at(0), QStringLiteral("A Trail of Two Tails"));
			QCOMPARE(view.outputLines().at(1), QStringLiteral("W <---(M)---> E"));
			QCOMPARE(view.outputLines().at(2), QStringLiteral("<2060hp 1694sp 1595st>"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("<2060hp 1694sp 1595st>"));
			resetTestState();
		}

		void nativeOutputRendererPartialPromptSurvivesPluginInsertBeforeFinalPrompt()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			view.appendOutputText(QStringLiteral("room description"), true);
			view.updatePartialOutputText(QStringLiteral("<2060hp 1694sp> "));
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QVERIFY(view.outputLines().contains(QStringLiteral("<2060hp 1694sp> ")));

			view.appendNoteText(QStringLiteral("[gmcp: no-npcs / city street]"), true);
			view.updatePartialOutputText(QStringLiteral("<2060hp 1694sp> "));
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QStringList lines = view.outputLines();
			QVERIFY(lines.contains(QStringLiteral("room description")));
			QVERIFY(lines.contains(QStringLiteral("[gmcp: no-npcs / city street]")));
			QCOMPARE(lines.constLast(), QStringLiteral("<2060hp 1694sp> "));

			view.appendOutputText(QStringLiteral("<2060hp 1694sp> look"), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			lines = view.outputLines();
			QVERIFY(lines.contains(QStringLiteral("[gmcp: no-npcs / city street]")));
			QCOMPARE(lines.constLast(), QStringLiteral("<2060hp 1694sp> look"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("<2060hp 1694sp> look"));
			resetTestState();
		}

		void nativeOutputRendererClearsPartialPromptOverlayBeforePluginAnchoredPrompt()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			const QString prompt = QStringLiteral("[Library][SAFE]<2084hp 1806sp 1695st> ");
			view.appendOutputText(QStringLiteral("room description"), true);
			view.updatePartialOutputText(prompt);
			QCoreApplication::processEvents();
			nativeCanvas->repaint();
			QCoreApplication::processEvents();
			QVERIFY(view.outputLines().contains(prompt));

			view.clearPartialOutput();
			view.appendNoteText(QStringLiteral("["), false);
			view.appendNoteText(QStringLiteral("435"), false);
			view.appendNoteText(QStringLiteral(", "), false);
			view.appendNoteText(QStringLiteral("1226"), false);
			view.appendNoteText(QStringLiteral("]"), true);
			view.appendOutputText(prompt, true);
			QCoreApplication::processEvents();
			nativeCanvas->repaint();
			QCoreApplication::processEvents();

			const QStringList lines = view.outputLines();
			QVERIFY(lines.contains(QStringLiteral("[435, 1226]")));
			QCOMPARE(lines.constLast(), prompt);
			QVERIFY(!lines.contains(QStringLiteral("[435, 1226]<2084hp 1806sp 1695st> ")));
			QVERIFY(std::ranges::none_of(lines, [](const QString &line)
			                             { return line == QStringLiteral("<2084hp 1806sp 1695st> "); }));
			resetTestState();
		}

		void nativeOutputRendererKeepsVectorOrderWhenInsertedLineNumberIsNewerThanPrompt()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntime(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			QVector<WorldRuntime::LineEntry> lines{
			    makeRuntimeLine(QStringLiteral("room description"), WorldRuntime::LineOutput, true, 100),
			    makeRuntimeLine(QStringLiteral("[gmcp: no-npcs / city street]"), WorldRuntime::LineNote, true,
			                    300),
			    makeRuntimeLine(QStringLiteral("<2060hp 1694sp 1595st>"), WorldRuntime::LineOutput, true,
			                    200)};
			g_runtimeLines = lines;

			view.restoreOutputFromPersistedLines(lines);
			nativeCanvas->repaint();
			QCoreApplication::processEvents();

			QTRY_COMPARE(view.outputLines().size(), 3);
			QCOMPARE(view.outputLines().at(0), QStringLiteral("room description"));
			QCOMPARE(view.outputLines().at(1), QStringLiteral("[gmcp: no-npcs / city street]"));
			QCOMPARE(view.outputLines().at(2), QStringLiteral("<2060hp 1694sp 1595st>"));
			resetTestState();
		}

		void nativeOutputRendererLuaOutputApiShapesWrapAndComposeConsistently()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("wrap_column"), QStringLiteral("12"));
			g_worldAttrs.insert(QStringLiteral("indent_paras"), QStringLiteral("0"));

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			view.appendNoteText(QStringLiteral("tell "), false);
			view.appendNoteTextStyled(
			    QStringLiteral("colour-tell "),
			    {makeFullSpan(QStringLiteral("colour-tell "), QColor(QStringLiteral("#00ff00")),
			                  QColor(QStringLiteral("#000000")))},
			    false);
			appendFakeRuntimeOutputText(view, QStringLiteral("note alpha beta gamma"), {}, true, true);
			appendFakeRuntimeOutputText(
			    view, QStringLiteral("colour note delta epsilon"),
			    {makeFullSpan(QStringLiteral("colour note delta epsilon"), QColor(QStringLiteral("#ff0000")),
			                  QColor(QStringLiteral("#000000")))},
			    true, true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const QStringList lines    = view.outputLines();
			const QString     combined = lines.join(QString());
			QVERIFY(combined.contains(QStringLiteral("tell colour-tell note alpha beta gamma")));
			QVERIFY(lines.size() >= 4);
			QVERIFY(lines.contains(QStringLiteral("colour note ")));
			QVERIFY(lines.contains(QStringLiteral("delta ")));
			QCOMPARE(lines.constLast(), QStringLiteral("epsilon"));
			QVERIFY(std::ranges::any_of(g_runtimeLines, [](const WorldRuntime::LineEntry &line)
			                            { return (line.flags & WorldRuntime::LineNote) != 0; }));
			resetTestState();
		}

		void nativeOutputRendererTelnetPayloadFlowsThroughConfiguredWrap()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("wrap_column"), QStringLiteral("12"));
			g_worldAttrs.insert(QStringLiteral("indent_paras"), QStringLiteral("0"));

			WorldView view;
			view.resize(900, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			TelnetProcessor processor;
			QByteArray      incoming = QByteArrayLiteral("alpha beta gamma\nprompt tail");
			incoming.append(static_cast<char>(0xFF));
			incoming.append(static_cast<char>(0xFF));
			incoming.append(QByteArrayLiteral(" done"));
			const QByteArray processed = processor.processBytes(incoming);

			appendFakeRuntimeOutputText(view, QString::fromUtf8(processed), {}, false, false);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const QStringList lines = view.outputLines();
			QVERIFY(lines.contains(QStringLiteral("alpha beta ")));
			QVERIFY(lines.contains(QStringLiteral("gamma")));
			QVERIFY(lines.join(QString()).contains(QStringLiteral("prompt tail")));
			QVERIFY(lines.constLast().contains(QStringLiteral("done")));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(), lines.constLast());
			resetTestState();
		}

		void nativeOutputRendererMxpSendLinkSurvivesParserWrapAndRestitch()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("8"));

			WorldView view;
			view.resize(720, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			for (int i = 0; i < 8; ++i)
				view.appendOutputText(QStringLiteral("mxp-primer-%1").arg(i), true);

			TelnetProcessor processor;
			processor.setUseMxp(eOnCommandMXP);
			QByteArray mxpStart;
			mxpStart.append(static_cast<char>(0xFF));
			mxpStart.append(static_cast<char>(0xFA));
			mxpStart.append(static_cast<char>(91));
			mxpStart.append(static_cast<char>(0xFF));
			mxpStart.append(static_cast<char>(0xF0));
			processor.processBytes(mxpStart);
			QVERIFY(processor.isMxpEnabled());

			const QByteArray output = processor.processBytes(
			    QByteArrayLiteral("\x1b[1z<SEND href='look portal'>look portal</SEND>\n"));
			QCOMPARE(output, QByteArrayLiteral("look portal\n"));
			const QList<TelnetProcessor::MxpEvent> events = processor.takeMxpEvents();
			QCOMPARE(events.size(), 2);

			WorldRuntime::StyleSpan span;
			span.length     = boundedSizeToInt(QByteArrayLiteral("look portal").size());
			span.actionType = WorldRuntime::ActionSend;
			span.action = QString::fromUtf8(events.constFirst().attributes.value(QByteArrayLiteral("href")));
			appendFakeRuntimeOutputText(view, QString::fromUtf8(output.chopped(1)), {span}, false, true);
			view.appendOutputText(QStringLiteral("<mxp-prompt>"), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QVERIFY(!view.outputLines().contains(QStringLiteral("mxp-primer-0")));
			QVERIFY(view.outputLines().contains(QStringLiteral("look portal")));
			QCOMPARE(view.outputLines().constLast(), QStringLiteral("<mxp-prompt>"));

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			const QPoint anchorPoint = findHyperlinkPoint(view, *browser, span.action);
			QVERIFY2(anchorPoint.x() >= 0 && anchorPoint.y() >= 0,
			         "Expected parsed MXP SEND link in rendered output.");
			resetTestState();
		}

		void nativeOutputRendererOsc8LinkSurvivesParserWrapAndRestitch()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("8"));

			WorldView view;
			view.resize(720, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			for (int i = 0; i < 8; ++i)
				view.appendOutputText(QStringLiteral("osc-primer-%1").arg(i), true);

			const QString href = QStringLiteral("https://example.org/osc8-parser");
			QByteArray    bytes;
			bytes.append(QByteArrayLiteral("\x1b]8;;"));
			bytes.append(href.toUtf8());
			bytes.append('\a');
			bytes.append(QByteArrayLiteral("open portal"));
			bytes.append(QByteArrayLiteral("\x1b]8;;\a"));

			QMudAnsiStreamState              streamState;
			QMudStyledTextState              styleState;
			const auto                       colorResolver = [](int) { return QString(); };
			QString                          plainText;
			QVector<WorldRuntime::StyleSpan> spans = spansFromStyledChunks(
			    qmudParseAnsiSgrChunks(bytes, streamState, QString(), QString(), colorResolver, colorResolver,
			                           colorResolver, [](QByteArrayView data)
			                           { return QString::fromUtf8(data); }, styleState,
			                           {WorldRuntime::ActionNone, WorldRuntime::ActionSend,
			                            WorldRuntime::ActionPrompt, WorldRuntime::ActionHyperlink}),
			    plainText);

			QCOMPARE(plainText, QStringLiteral("open portal"));
			appendFakeRuntimeOutputText(view, plainText, spans, false, true);
			view.appendOutputText(QStringLiteral("<osc-prompt>"), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QVERIFY(!view.outputLines().contains(QStringLiteral("osc-primer-0")));
			QVERIFY(view.outputLines().contains(QStringLiteral("open portal")));
			QCOMPARE(view.outputLines().constLast(), QStringLiteral("<osc-prompt>"));

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			const QPoint anchorPoint = findHyperlinkPoint(view, *browser, href);
			QVERIFY2(anchorPoint.x() >= 0 && anchorPoint.y() >= 0,
			         "Expected parsed OSC8 link in rendered output.");
			resetTestState();
		}

		void nativeOutputRendererPendingLuaOutputAfterReloadRestoreKeepsOrdering()
		{
			resetTestState();

			WorldView view;
			view.resize(720, 420);
			view.show();
			view.setRuntime(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			QVector<WorldRuntime::LineEntry> restoredLines{
			    makeRuntimeLine(QStringLiteral("restore-room"), WorldRuntime::LineOutput, true, 10),
			    makeRuntimeLine(QStringLiteral("<restore-prompt>"), WorldRuntime::LineOutput, true, 11)};
			g_runtimeLines = restoredLines;
			view.restoreOutputFromPersistedLines(restoredLines);

			const bool queued = QMetaObject::invokeMethod(
			    &view,
			    [&view]
			    {
				    view.appendNoteText(QStringLiteral("[queued-lua-output]"), true);
				    view.appendOutputText(QStringLiteral("<queued-prompt>"), true);
			    },
			    Qt::QueuedConnection);
			QVERIFY(queued);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const QStringList lines = view.outputLines();
			QVERIFY(lines.contains(QStringLiteral("restore-room")));
			QVERIFY(lines.contains(QStringLiteral("<restore-prompt>")));
			QVERIFY(lines.contains(QStringLiteral("[queued-lua-output]")));
			QCOMPARE(lines.constLast(), QStringLiteral("<queued-prompt>"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("<queued-prompt>"));
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

		void inputWrapMarginsRecomputeWhenInputAreaIsResized()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("wrap_column"), QStringLiteral("20"));

			WorldView view;
			view.resize(1600, 480);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			QVERIFY(input->viewport());
			const int initialViewportWidth = input->viewport()->width();
			QVERIFY(initialViewportWidth > 200);

			view.resize(340, 480);
			QCoreApplication::processEvents();

			const int resizedInputWidth    = input->width();
			const int resizedViewportWidth = input->viewport()->width();
			QVERIFY(resizedInputWidth > 0);
			QVERIFY2(resizedViewportWidth > (resizedInputWidth / 4),
			         "Input viewport collapsed after resize with wrap_input enabled.");

			resetTestState();
		}

		void inputWrapDisabledStillUsesWidgetWidthWrap()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("pixel_offset"), QStringLiteral("8"));

			WorldView view;
			view.resize(700, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			QCOMPARE(input->lineWrapMode(), QPlainTextEdit::WidgetWidth);
			QCOMPARE(input->wordWrapMode(), QTextOption::WrapAtWordBoundaryOrAnywhere);

			resetTestState();
		}

		void autoResizeCommandWindowUsesWrappedVisualLines()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("6"));
			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("0"));

			WorldView view;
			view.resize(360, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			const int singleHeight = input->height();
			QVERIFY(singleHeight > 0);

			view.setInputText(QStringLiteral("word ").repeated(180), true);
			QCoreApplication::processEvents();

			QTRY_VERIFY2(input->height() > singleHeight,
			             "Auto-resize should grow for one long wrapped line.");

			resetTestState();
		}

		void autoResizeCommandWindowReflowsOnInputResize()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("50"));
			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("0"));

			WorldView view;
			view.resize(340, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			const QString text = QStringLiteral("word ").repeated(120);
			view.setInputText(text, true);
			QCoreApplication::processEvents();

			const int narrowHeight = input->height();
			QVERIFY(narrowHeight > 0);

			view.resize(980, 420);
			QCoreApplication::processEvents();

			QTRY_VERIFY2(
			    input->height() < narrowHeight,
			    "Auto-resize height should shrink when input width increases and wrapping decreases.");
			const int bottomGap = input->viewport()->rect().bottom() - input->cursorRect().bottom();
			QVERIFY2(bottomGap <= input->fontMetrics().lineSpacing(),
			         "After resize reflow, active wrapped line should stay near the bottom edge.");

			resetTestState();
		}

		void defaultInputSplitterSizingRepairsPoisonedPreShowState()
		{
			resetTestState();

			WorldView view;
			view.resize(420, 420);

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			auto *inputSplitter = qobject_cast<QSplitter *>(input->parentWidget());
			QVERIFY(inputSplitter);

			inputSplitter->setSizes(QList<int>() << 0 << 1000);

			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QTRY_VERIFY(inputSplitter->sizes().size() >= 2);
			const QList<int> sizes = inputSplitter->sizes();
			QVERIFY2(sizes.at(0) > 0, "Output pane should recover from poisoned pre-show splitter sizes.");
			QVERIFY2(sizes.at(1) > 0, "Input pane should keep a positive height after startup sizing.");
			QVERIFY2(qAbs(sizes.at(1) - input->height()) <= 2,
			         "Startup default input sizing should synchronize splitter/input heights.");

			resetTestState();
		}

		void enablingAutoResizeCompactsInputPaneWithoutBottomDeadSpace()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("0"));

			WorldView view;
			view.resize(420, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			auto *inputSplitter = qobject_cast<QSplitter *>(input->parentWidget());
			QVERIFY(inputSplitter);
			QVERIFY(inputSplitter->sizes().size() >= 2);

			const int expandedInputHeight = qMax(input->height() * 4, 140);
			QCOMPARE(view.setCommandWindowHeight(expandedInputHeight), eOK);
			QCoreApplication::processEvents();

			const QList<int> sizesBeforeToggle = inputSplitter->sizes();
			QVERIFY(sizesBeforeToggle.size() >= 2);
			QVERIFY2(sizesBeforeToggle.at(1) >= qMax(expandedInputHeight - 4, 100),
			         "Precondition failed: command pane did not expand before auto-resize toggle.");

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("20"));
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QTRY_VERIFY(inputSplitter->sizes().size() >= 2);
			const QList<int> sizesAfterToggle = inputSplitter->sizes();
			QVERIFY2(qAbs(sizesAfterToggle.at(1) - input->height()) <= 2,
			         "Auto-resize should compact splitter input pane to the actual input widget height.");
			QVERIFY2(sizesAfterToggle.at(1) + 20 < sizesBeforeToggle.at(1),
			         "Auto-resize should reduce command pane height from the pre-toggle expanded size.");
			QVERIFY2(sizesAfterToggle.at(0) > sizesBeforeToggle.at(0),
			         "Space released by one-line auto-resize should be returned to world output pane.");

			resetTestState();
		}

		void autoResizePolicyChangeReappliesWithoutInputEdit()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("20"));
			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("0"));

			WorldView view;
			view.resize(340, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			const int singleHeight = input->height();
			QVERIFY(singleHeight > 0);

			const QString longWrapped =
			    QStringLiteral("policy-check ") + QStringLiteral("word ").repeated(220);
			view.setInputText(longWrapped, true);
			QCoreApplication::processEvents();
			QTRY_VERIFY(input->height() > singleHeight);
			const int expandedHeight = input->height();

			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("1"));
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();
			QTRY_VERIFY2(input->height() < expandedHeight,
			             "Reducing auto-resize maximum lines should shrink input without any text edit.");
			QCOMPARE(input->height(), singleHeight);

			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("20"));
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();
			QTRY_VERIFY2(
			    input->height() > singleHeight,
			    "Increasing auto-resize maximum lines should re-expand input without any text edit.");

			resetTestState();
		}

		void pasteIntoInputKeepsCursorVisibleWithAutoResize()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("6"));
			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("0"));

			WorldView view;
			view.resize(360, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());
			const int singleHeight = input->height();
			QVERIFY(singleHeight > 0);

			const QString pasted = QStringLiteral("paste ") + QStringLiteral("word ").repeated(120);
			QGuiApplication::clipboard()->setText(pasted);
			QTest::keySequence(input, QKeySequence::Paste);
			QCoreApplication::processEvents();

			QCOMPARE(view.inputText(), pasted);
			QCOMPARE(input->textCursor().position(), boundedSizeToInt(pasted.size()));
			QTRY_VERIFY2(input->height() > singleHeight,
			             "Auto-resize should grow after paste when wrapped content expands.");
			QVERIFY2(input->viewport()->rect().intersects(input->cursorRect()),
			         "Input cursor should remain visible after paste with auto-resize enabled.");
			const int bottomGap = input->viewport()->rect().bottom() - input->cursorRect().bottom();
			QVERIFY2(bottomGap <= input->fontMetrics().lineSpacing(),
			         "Wrapped input should keep the active line near the bottom edge.");

			QTest::keyClicks(input, QStringLiteral(" tail"));
			QCoreApplication::processEvents();
			QCOMPARE(input->textCursor().position(), boundedSizeToInt(pasted.size() + 5));
			QVERIFY2(input->viewport()->rect().intersects(input->cursorRect()),
			         "Input cursor should stay visible while typing after large paste.");
			const int bottomGapAfterTyping =
			    input->viewport()->rect().bottom() - input->cursorRect().bottom();
			QVERIFY2(bottomGapAfterTyping <= input->fontMetrics().lineSpacing(),
			         "Typing after paste should keep the active line near the bottom edge.");

			resetTestState();
		}

		void multilinePasteAutoResizeKeepsExpectedHeightProgression()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("20"));
			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("0"));

			WorldView view;
			view.resize(360, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			const int singleHeight = input->height();
			QVERIFY(singleHeight > 0);

			const QString multilineNoTrailingBlank =
			    QStringLiteral("The automated test verifies command input auto-resize behavior with wrapped "
			                   "multiline text content\n and a continuation line that starts with a space.");

			QGuiApplication::clipboard()->setText(multilineNoTrailingBlank);
			QTest::keySequence(input, QKeySequence::Paste);
			QCoreApplication::processEvents();

			const int heightWithoutTrailingBlank = input->height();
			QTRY_VERIFY2(heightWithoutTrailingBlank > singleHeight,
			             "Two-line multiline paste should expand command input height.");
			QCOMPARE(view.inputText(), multilineNoTrailingBlank);
			QTextCursor firstLineCursor(input->document());
			firstLineCursor.movePosition(QTextCursor::Start);
			QVERIFY2(input->viewport()->rect().intersects(input->cursorRect(firstLineCursor)),
			         "First line should remain visible after multiline paste.");

			view.setInputText(QString(), true);
			QCoreApplication::processEvents();
			QCOMPARE(input->height(), singleHeight);

			const QString multilineWithTrailingBlank = multilineNoTrailingBlank + QStringLiteral("\n\n");
			QGuiApplication::clipboard()->setText(multilineWithTrailingBlank);
			QTest::keySequence(input, QKeySequence::Paste);
			QCoreApplication::processEvents();

			const int heightWithTrailingBlank = input->height();
			QTRY_VERIFY2(heightWithTrailingBlank >= heightWithoutTrailingBlank,
			             "Adding a trailing blank line should not shrink auto-resized input height.");
			QCOMPARE(view.inputText(), multilineWithTrailingBlank);
			firstLineCursor = QTextCursor(input->document());
			firstLineCursor.movePosition(QTextCursor::Start);
			QVERIFY2(input->viewport()->rect().intersects(input->cursorRect(firstLineCursor)),
			         "First line should remain visible after multiline paste with trailing blank line.");
			QVERIFY2(input->viewport()->rect().intersects(input->cursorRect()),
			         "Input caret should remain visible after multiline paste with trailing blank line.");

			const QPoint clickPoint(input->viewport()->rect().center().x(),
			                        input->viewport()->rect().bottom() - 2);
			QTest::mouseClick(input->viewport(), Qt::LeftButton, Qt::NoModifier, clickPoint);
			QCoreApplication::processEvents();
			firstLineCursor = QTextCursor(input->document());
			firstLineCursor.movePosition(QTextCursor::Start);
			QVERIFY2(input->viewport()->rect().intersects(input->cursorRect(firstLineCursor)),
			         "First line should remain visible after mouse click in command input.");

			resetTestState();
		}

		void multilineMergeSplitSequenceKeepsTopLineVisible()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("20"));
			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("0"));

			WorldView view;
			view.resize(360, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			const int singleHeight = input->height();
			QVERIFY(singleHeight > 0);

			const QString multilineNoTrailingBlank =
			    QStringLiteral("The automated test verifies command input auto-resize behavior with wrapped "
			                   "multiline text content\n and a continuation line that starts with a space.");

			QGuiApplication::clipboard()->setText(multilineNoTrailingBlank);
			for (int iteration = 0; iteration < 20; ++iteration)
			{
				view.setInputText(QString(), true);
				QCoreApplication::processEvents();
				QTest::keySequence(input, QKeySequence::Paste);
				QCoreApplication::processEvents();

				QTRY_VERIFY(input->height() > singleHeight);
				QCOMPARE(input->document()->blockCount(), 2);

				QTextCursor cursor = input->textCursor();
				cursor.movePosition(QTextCursor::End);
				input->setTextCursor(cursor);
				QCoreApplication::processEvents();

				QTest::keyClick(input, Qt::Key_Home);
				QTest::keyClick(input, Qt::Key_Backspace);
				QTest::keyClick(input, Qt::Key_Return, Qt::ShiftModifier);
				if (input->document()->blockCount() == 1)
					input->insertPlainText(QStringLiteral("\n"));
				QTest::keyClick(input, Qt::Key_End);
				QCoreApplication::processEvents();

				QCOMPARE(input->document()->blockCount(), 2);
				QTRY_VERIFY2(
				    input->height() > singleHeight,
				    "Input should remain in multiline auto-resized height after merge/split sequence.");
				if (QScrollBar *bar = input->verticalScrollBar())
					QTRY_COMPARE(bar->value(), bar->minimum());
				QTextCursor firstLineCursor(input->document());
				firstLineCursor.movePosition(QTextCursor::Start);
				QVERIFY2(input->viewport()->rect().intersects(input->cursorRect(firstLineCursor)),
				         "First line should remain visible after Home/Backspace/Shift+Enter/End sequence.");
			}

			resetTestState();
		}

		void pasteHomeBackspaceSequenceKeepsInputCaretVisibleWithAutoResize()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("8"));
			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("0"));

			WorldView view;
			view.resize(360, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			const QString pasted = QStringLiteral("first ") + QStringLiteral("word ").repeated(48) +
			                       QStringLiteral("\nsecond ") + QStringLiteral("word ").repeated(44) +
			                       QStringLiteral("\nthird line");
			QGuiApplication::clipboard()->setText(pasted);

			auto assertCaretVisible = [input]
			{
				const QRect cursorRect = input->cursorRect();
				QVERIFY2(cursorRect.isValid(), "Input cursor rectangle should remain valid.");
				QVERIFY2(cursorRect.height() > 0, "Input cursor rectangle should have positive height.");
				QVERIFY2(input->viewport()->rect().intersects(cursorRect),
				         "Input caret should remain visible inside viewport.");
			};

			for (int i = 0; i < 25; ++i)
			{
				view.setInputText(QString(), true);
				QCoreApplication::processEvents();

				QTest::keySequence(input, QKeySequence::Paste);
				QCoreApplication::processEvents();
				QCOMPARE(view.inputText(), pasted);
				assertCaretVisible();

				QTest::keyClick(input, Qt::Key_Home);
				QCoreApplication::processEvents();
				assertCaretVisible();

				QTest::keyClick(input, Qt::Key_Backspace);
				QCoreApplication::processEvents();
				assertCaretVisible();
			}

			resetTestState();
		}

		void multilinePasteCrossLineLeftAndMergeKeepsInputViewportStable()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("8"));
			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("0"));

			WorldView view;
			view.resize(360, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());
			const int singleHeight = input->height();
			QVERIFY(singleHeight > 0);

			const QString pasted = QStringLiteral("first ") + QStringLiteral("word ").repeated(44) +
			                       QStringLiteral("\nsecond ") + QStringLiteral("word ").repeated(40) +
			                       QStringLiteral("\nthird ") + QStringLiteral("word ").repeated(36);
			QGuiApplication::clipboard()->setText(pasted);

			auto assertCaretVisible = [input]
			{
				const QRect cursorRect = input->cursorRect();
				QVERIFY2(cursorRect.isValid(), "Input cursor rectangle should remain valid.");
				QVERIFY2(cursorRect.height() > 0, "Input cursor rectangle should have positive height.");
				QVERIFY2(input->viewport()->rect().intersects(cursorRect),
				         "Input caret should remain visible inside viewport.");
			};

			for (int i = 0; i < 40; ++i)
			{
				view.setInputText(QString(), true);
				QCoreApplication::processEvents();

				QTest::keySequence(input, QKeySequence::Paste);
				QCoreApplication::processEvents();
				QCOMPARE(view.inputText(), pasted);
				QTRY_VERIFY2(input->height() > singleHeight,
				             "Auto-resize should grow after multiline paste.");
				assertCaretVisible();

				QTextCursor cursor = input->textCursor();
				cursor.movePosition(QTextCursor::End);
				input->setTextCursor(cursor);
				QCoreApplication::processEvents();
				assertCaretVisible();

				QTest::keyClick(input, Qt::Key_Home);
				QCoreApplication::processEvents();
				assertCaretVisible();
				const int homePosition = input->textCursor().position();

				QTest::keyClick(input, Qt::Key_Left);
				QCoreApplication::processEvents();
				assertCaretVisible();
				QVERIFY2(input->textCursor().position() < homePosition,
				         "Left at line start should move caret to previous line.");

				QTextCursor mergeCursor = input->textCursor();
				mergeCursor.movePosition(QTextCursor::End);
				mergeCursor.movePosition(QTextCursor::StartOfBlock);
				input->setTextCursor(mergeCursor);
				QCoreApplication::processEvents();
				assertCaretVisible();
				const int blockCountBeforeMerge = input->document()->blockCount();
				QVERIFY(blockCountBeforeMerge >= 3);

				QTest::keyClick(input, Qt::Key_Backspace);
				QCoreApplication::processEvents();
				assertCaretVisible();
				QCOMPARE(input->document()->blockCount(), blockCountBeforeMerge - 1);
				QCOMPARE(view.inputText().size(), boundedSizeToInt(pasted.size()) - 1);
			}

			resetTestState();
		}

		void pasteIntoInputPreservesTrailingNewlineFromClipboard()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("6"));
			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("0"));

			WorldView view;
			view.resize(420, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			const QString pastedWithTrailingNewline = QStringLiteral("say test line\n");
			QGuiApplication::clipboard()->setText(pastedWithTrailingNewline);
			QTest::keySequence(input, QKeySequence::Paste);
			QCoreApplication::processEvents();

			QCOMPARE(view.inputText(), pastedWithTrailingNewline);
			QCOMPARE(input->textCursor().position(), boundedSizeToInt(pastedWithTrailingNewline.size()));

			resetTestState();
		}

		void pasteIntoInputEmptyClipboardDoesNothing()
		{
			resetTestState();

			WorldView view;
			view.resize(420, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			view.setInputText(QStringLiteral("say north"), true);
			QCoreApplication::processEvents();

			QGuiApplication::clipboard()->setText(QString());
			QTest::keySequence(input, QKeySequence::Paste);
			QCoreApplication::processEvents();

			QCOMPARE(view.inputText(), QStringLiteral("say north"));
			QCOMPARE(input->textCursor().position(), boundedSizeToInt(QStringLiteral("say north").size()));

			resetTestState();
		}

		void autoResizeCommandWindowDoesNotAddExtraLineForTrailingWhitespaceWrap()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("12"));
			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("0"));

			WorldView view;
			view.resize(380, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());

			const QString trailingSpaceWrappedText = QStringLiteral("word ").repeated(85);
			view.setInputText(trailingSpaceWrappedText, true);
			QCoreApplication::processEvents();

			const int bottomGap = input->viewport()->rect().bottom() - input->cursorRect().bottom();
			QVERIFY2(bottomGap <= input->fontMetrics().lineSpacing(),
			         "Trailing whitespace wrap should not allocate an extra visual line below the cursor.");

			resetTestState();
		}

		void programmaticInputClearResetsAutoResizeAndKeepsCaretVisible()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("6"));
			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("auto_repeat"), QStringLiteral("0"));

			WorldView view;
			view.resize(360, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus(Qt::OtherFocusReason);
			QTRY_VERIFY(input->hasFocus());
			const int singleHeight = input->height();
			QVERIFY(singleHeight > 0);

			view.setInputText(QStringLiteral("word ").repeated(120), true);
			QCoreApplication::processEvents();
			QTRY_VERIFY(input->height() > singleHeight);

			const QString pushed = view.pushCommand();
			QCOMPARE(pushed, QStringLiteral("word ").repeated(120));
			QCoreApplication::processEvents();

			QCOMPARE(view.inputText(), QString());
			QCOMPARE(input->height(), singleHeight);
			QVERIFY2(input->viewport()->rect().intersects(input->cursorRect()),
			         "Programmatic push-command clear should leave caret visible.");

			view.setInputText(QStringLiteral("word ").repeated(120), true);
			QCoreApplication::processEvents();
			QTRY_VERIFY(input->height() > singleHeight);

			view.sendCommandFromHistory(QStringLiteral("north"));
			QCoreApplication::processEvents();

			QCOMPARE(view.inputText(), QString());
			QCOMPARE(input->height(), singleHeight);
			QVERIFY2(input->viewport()->rect().intersects(input->cursorRect()),
			         "History-send clear should leave caret visible.");

			resetTestState();
		}

		void commandSelectionKeepsCaretVisibleForWrappedInput()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("2"));
			g_worldAttrs.insert(QStringLiteral("wrap_input"), QStringLiteral("0"));

			WorldView view;
			view.resize(360, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);

			const QString wrapped = QStringLiteral("word ").repeated(200);
			view.setInputText(wrapped, true);
			QCoreApplication::processEvents();
			QTRY_VERIFY(input->height() > 0);

			QCOMPARE(view.setCommandSelection(1, 1), eOK);
			QCoreApplication::processEvents();

			QCOMPARE(input->textCursor().anchor(), 0);
			QCOMPARE(input->textCursor().position(), 1);
			QVERIFY2(input->viewport()->rect().intersects(input->cursorRect()),
			         "SetCommandSelection should keep the caret in view.");

			resetTestState();
		}

		void textRectangleOutsideFillDoesNotCoverUnderlayMiniWindows()
		{
			resetTestState();

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			g_textRectangle.left              = 220;
			g_textRectangle.top               = 120;
			g_textRectangle.right             = 700;
			g_textRectangle.bottom            = 440;
			g_textRectangle.borderOffset      = 0;
			g_textRectangle.borderWidth       = 0;
			g_textRectangle.outsideFillStyle  = 0;        // solid
			g_textRectangle.outsideFillColour = 0x00FF00; // green (COLORREF)
			view.updateWrapMargin();
			QCoreApplication::processEvents();

			constexpr QColor underlayColour(255, 0, 255, 255);
			appendTestMiniWindow(QStringLiteral("underlay-miniw"), QRect(24, 24, 40, 40),
			                     kMiniWindowDrawUnderneath, underlayColour);
			view.onMiniWindowsChanged();
			QCoreApplication::processEvents();

			QSplitter *outputSplitter = findOutputSplitter(view);
			QVERIFY(outputSplitter);
			QWidget *outputStack = outputSplitter->parentWidget();
			QVERIFY(outputStack);
			outputStack->update();
			QCoreApplication::processEvents();

			const QImage image = outputStack->grab().toImage();
			QVERIFY(!image.isNull());
			constexpr QPoint samplePoint(40, 40);
			QVERIFY(image.rect().contains(samplePoint));
			const QColor sample = image.pixelColor(samplePoint);
			QVERIFY2(colorsMatchExactly(sample, underlayColour),
			         "TextRectangle outside fill obscured an underneath miniwindow.");

			resetTestState();
		}

		void overlayMiniWindowsRemainAboveNoWrapText()
		{
			resetTestState();

			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("0"));

			WorldView view;
			view.resize(900, 640);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			// Force long unwrapped output that crosses the overlay miniwindow bounds.
			view.appendOutputText(QStringLiteral("W").repeated(1200), true);
			QCoreApplication::processEvents();

			constexpr QColor overlayColour(255, 0, 255, 255);
			const QRect      overlayRect(120, 0, 220, 30);
			appendTestMiniWindow(QStringLiteral("overlay-nowrap"), overlayRect, 0, overlayColour);
			view.onMiniWindowsChanged();
			QCoreApplication::processEvents();

			QSplitter *outputSplitter = findOutputSplitter(view);
			QVERIFY(outputSplitter);
			QWidget *outputStack = outputSplitter->parentWidget();
			QVERIFY(outputStack);
			outputStack->update();
			QCoreApplication::processEvents();

			const QImage image = outputStack->grab().toImage();
			QVERIFY(!image.isNull());
			const QRect sampleRect = overlayRect.intersected(image.rect());
			QVERIFY(!sampleRect.isEmpty());
			for (int y = sampleRect.top(); y <= sampleRect.bottom(); ++y)
			{
				for (int x = sampleRect.left(); x <= sampleRect.right(); ++x)
				{
					const QColor sample = image.pixelColor(x, y);
					QVERIFY2(colorsMatchExactly(sample, overlayColour),
					         "No-wrap output painted above overlay miniwindow.");
				}
			}

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

		void nativeOutputRendererTreatsTailHardReturnFlipAsIncremental()
		{
			resetTestState();

			WorldView view;
			view.resize(640, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			view.appendOutputText(QStringLiteral("tail-open"), false);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 1);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("tail-open"));

			const int rebuildsBefore = nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();

			QVERIFY(!g_runtimeLines.isEmpty());
			g_runtimeLines.last().hardReturn = true;

			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 1);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("tail-open"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt(), rebuildsBefore);
			resetTestState();
		}

		void nativeOutputRendererKeepsIncrementalAppendAfterRuntimeSettingsPin()
		{
			resetTestState();

			WorldView view;
			view.resize(640, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			view.appendOutputText(QStringLiteral("settings-one"), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 1);
			const int rebuildsBeforeSettings =
			    nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();

			view.applyRuntimeSettings();
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const int rebuildsAfterSettings =
			    nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();
			const int updatesBeforeAppend =
			    nativeCanvas->property("qmud_native_cache_incremental_updates").toInt();

			view.appendOutputText(QStringLiteral("settings-two"), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 2);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("settings-two"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt(),
			             rebuildsAfterSettings);
			QTRY_VERIFY(nativeCanvas->property("qmud_native_cache_incremental_updates").toInt() >
			            updatesBeforeAppend);
			QVERIFY(rebuildsAfterSettings >= rebuildsBeforeSettings);
			resetTestState();
		}

		void nativeOutputRendererNonContiguousLineNumbersReuseCacheOnStablePaints()
		{
			resetTestState();

			WorldView view;
			view.resize(640, 420);
			view.show();
			view.setRuntime(fakeRuntimePointer());
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
			nativeCanvas->repaint();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 3);
			const int rebuildsBeforeStablePaint =
			    nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();

			nativeCanvas->repaint();
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

		void nativeOutputRendererNonContiguousRollingWindowStaysIncremental()
		{
			resetTestState();

			WorldView view;
			view.resize(640, 420);
			view.show();
			view.setRuntime(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			QVector<WorldRuntime::LineEntry> restoredLines;
			restoredLines.reserve(4);
			for (int i = 0; i < 4; ++i)
			{
				WorldRuntime::LineEntry entry;
				entry.text       = QStringLiteral("noncontig-roll-%1").arg(i);
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				entry.lineNumber = (i + 1) * 10;
				restoredLines.push_back(entry);
			}
			g_runtimeLines = restoredLines;

			view.restoreOutputFromPersistedLines(restoredLines);
			nativeCanvas->repaint();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 4);
			const int rebuildsBefore = nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();
			const int updatesBefore = nativeCanvas->property("qmud_native_cache_incremental_updates").toInt();
			const int trimDropsBefore = nativeCanvas->property("qmud_native_cache_trim_drops").toInt();

			g_runtimeLines.removeFirst();
			WorldRuntime::LineEntry appended;
			appended.text       = QStringLiteral("noncontig-roll-4");
			appended.flags      = WorldRuntime::LineOutput;
			appended.hardReturn = true;
			appended.lineNumber = 50;
			g_runtimeLines.push_back(appended);

			nativeCanvas->repaint();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 4);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("noncontig-roll-4"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt(), rebuildsBefore);
			QTRY_VERIFY(nativeCanvas->property("qmud_native_cache_incremental_updates").toInt() >
			            updatesBefore);
			QTRY_VERIFY(nativeCanvas->property("qmud_native_cache_trim_drops").toInt() > trimDropsBefore);
			resetTestState();
		}

		void nativeOutputRendererCappedBufferKeepsPluginPromptTailIncremental()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("10"));

			WorldView view;
			view.resize(700, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			for (int i = 0; i < 10; ++i)
				view.appendOutputText(QStringLiteral("cap-primer-%1").arg(i), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(g_runtimeLines.size(), 10);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 10);
			const int rebuildsBefore = nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();

			view.appendOutputText(QStringLiteral("cap-room"), true);
			view.appendNoteText(QStringLiteral("W"), false);
			view.appendNoteText(QStringLiteral(" <---("), false);
			view.appendNoteText(QStringLiteral("M"), false);
			view.appendNoteText(QStringLiteral(")---> "), false);
			view.appendNoteText(QStringLiteral("E"), true);
			view.appendOutputText(QStringLiteral("<cap-prompt>"), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const QStringList lines = view.outputLines();
			QCOMPARE(g_runtimeLines.size(), 10);
			QVERIFY(!lines.contains(QStringLiteral("cap-primer-0")));
			QVERIFY(lines.contains(QStringLiteral("cap-room")));
			QVERIFY(lines.contains(QStringLiteral("W <---(M)---> E")));
			QCOMPARE(lines.constLast(), QStringLiteral("<cap-prompt>"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("<cap-prompt>"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt(), rebuildsBefore);
			resetTestState();
		}

		void nativeOutputRendererRepeatedCappedPluginPromptCyclesStayIncremental()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("24"));

			WorldView view;
			view.resize(700, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			for (int i = 0; i < 24; ++i)
				view.appendOutputText(QStringLiteral("cycle-primer-%1").arg(i), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(g_runtimeLines.size(), 24);
			const int rebuildsBefore = nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();

			for (int cycle = 0; cycle < 5; ++cycle)
			{
				view.appendOutputText(QStringLiteral("cycle-room-%1").arg(cycle), true);
				view.appendNoteText(QStringLiteral("W"), false);
				view.appendNoteText(QStringLiteral(" <---("), false);
				view.appendNoteText(QStringLiteral("M"), false);
				view.appendNoteText(QStringLiteral(")---> "), false);
				view.appendNoteText(QStringLiteral("E"), true);
				view.appendOutputText(QStringLiteral("<cycle-prompt-%1>").arg(cycle), true);
				QCoreApplication::processEvents();
				nativeCanvas->update();
				QCoreApplication::processEvents();

				const QStringList lines = view.outputLines();
				QCOMPARE(g_runtimeLines.size(), 24);
				QVERIFY(lines.contains(QStringLiteral("W <---(M)---> E")));
				QCOMPARE(lines.constLast(), QStringLiteral("<cycle-prompt-%1>").arg(cycle));
				QTRY_COMPARE(nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt(),
				             rebuildsBefore);
			}
			resetTestState();
		}

		void nativeOutputRendererCappedLuaPluginWorkloadAvoidsFullRebuilds()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("80"));

			WorldView view;
			view.resize(720, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			for (int i = 0; i < 80; ++i)
				view.appendOutputText(QStringLiteral("guard-primer-%1").arg(i), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const int rebuildsBefore = nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();
			const int restitchFailuresBefore =
			    nativeCanvas->property("qmud_native_cache_rebuild_reason_restitch_failure").toInt();
			const int layoutRowsBefore =
			    nativeCanvas->property("qmud_native_layout_row_measurements").toInt();

			for (int cycle = 0; cycle < 12; ++cycle)
			{
				view.appendOutputText(QStringLiteral("guard-room-%1").arg(cycle), true);
				view.updatePartialOutputText(QStringLiteral("<guard-prompt-%1> ").arg(cycle));
				view.appendNoteText(QStringLiteral("[gmcp: no-npcs / city street]"), true);
				view.appendNoteText(QStringLiteral("W"), false);
				view.appendNoteText(QStringLiteral(" <---("), false);
				view.appendNoteText(QStringLiteral("M"), false);
				view.appendNoteText(QStringLiteral(")---> "), false);
				view.appendNoteText(QStringLiteral("E"), true);
				view.updatePartialOutputText(QStringLiteral("<guard-prompt-%1> ").arg(cycle));
				view.appendOutputText(QStringLiteral("<guard-prompt-%1> look").arg(cycle), true);
				QCoreApplication::processEvents();
				nativeCanvas->update();
				QCoreApplication::processEvents();

				const QStringList lines = view.outputLines();
				QCOMPARE(g_runtimeLines.size(), 80);
				QVERIFY(lines.contains(QStringLiteral("[gmcp: no-npcs / city street]")));
				QVERIFY(lines.contains(QStringLiteral("W <---(M)---> E")));
				QCOMPARE(lines.constLast(), QStringLiteral("<guard-prompt-%1> look").arg(cycle));
				QTRY_COMPARE(nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt(),
				             rebuildsBefore);
			}

			QCOMPARE(nativeCanvas->property("qmud_native_cache_rebuild_reason_restitch_failure").toInt(),
			         restitchFailuresBefore);
			const int measuredDelta =
			    nativeCanvas->property("qmud_native_layout_row_measurements").toInt() - layoutRowsBefore;
			QVERIFY2(measuredDelta <= 256,
			         qPrintable(QStringLiteral("Expected bounded layout work, delta=%1").arg(measuredDelta)));
			resetTestState();
		}

		void nativeOutputRendererCappedPartialPromptPluginCycleKeepsPromptTail()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("14"));

			WorldView view;
			view.resize(700, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			for (int i = 0; i < 14; ++i)
				view.appendOutputText(QStringLiteral("partial-primer-%1").arg(i), true);
			QCoreApplication::processEvents();

			for (int cycle = 0; cycle < 4; ++cycle)
			{
				view.appendOutputText(QStringLiteral("partial-room-%1").arg(cycle), true);
				view.updatePartialOutputText(QStringLiteral("<partial-prompt-%1> ").arg(cycle));
				view.appendNoteText(QStringLiteral("[gmcp: no-npcs / city street]"), true);
				view.updatePartialOutputText(QStringLiteral("<partial-prompt-%1> ").arg(cycle));
				QCoreApplication::processEvents();
				QCOMPARE(view.outputLines().constLast(), QStringLiteral("<partial-prompt-%1> ").arg(cycle));

				view.appendOutputText(QStringLiteral("<partial-prompt-%1> look").arg(cycle), true);
				QCoreApplication::processEvents();
				nativeCanvas->update();
				QCoreApplication::processEvents();

				const QStringList lines = view.outputLines();
				QCOMPARE(g_runtimeLines.size(), 14);
				QVERIFY(lines.contains(QStringLiteral("[gmcp: no-npcs / city street]")));
				QCOMPARE(lines.constLast(), QStringLiteral("<partial-prompt-%1> look").arg(cycle));
				QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
				             QStringLiteral("<partial-prompt-%1> look").arg(cycle));
			}
			resetTestState();
		}

		void nativeOutputPaintSynchronizesPendingPromptPluginRestitch()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("14"));

			WorldView view;
			view.resize(700, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			for (int i = 0; i < 14; ++i)
				view.appendOutputText(QStringLiteral("paint-sync-primer-%1").arg(i), true);
			QCoreApplication::processEvents();
			QVERIFY(!nativeCanvas->grab().isNull());
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("paint-sync-primer-13"));

			view.appendOutputText(QStringLiteral("paint-sync-room"), true);
			view.appendOutputText(QStringLiteral("<paint-sync-prompt> look"), true);
			QCoreApplication::processEvents();
			QVERIFY(!nativeCanvas->grab().isNull());
			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_last_line").toString(),
			             QStringLiteral("<paint-sync-prompt> look"));

			const int promptIndex = sizeToInt(g_runtimeLines.size()) - 1;
			QVERIFY(promptIndex >= 0);

			WorldRuntime::LineEntry compassLine;
			compassLine.text       = QStringLiteral("W <---(M)---> E");
			compassLine.flags      = WorldRuntime::LineNote;
			compassLine.hardReturn = true;
			compassLine.time       = QDateTime::currentDateTime();
			compassLine.lineNumber = g_runtimeLines.constLast().lineNumber + 1;
			g_runtimeLines.insert(g_runtimeLines.begin() + promptIndex, compassLine);
			const int insertedCount = sizeToInt(g_runtimeLines.size());
			enforceFakeOutputLineLimit();
			const int removedHeadCount = qMax(0, insertedCount - sizeToInt(g_runtimeLines.size()));
			const int changedIndex =
			    qBound(0, promptIndex - removedHeadCount, sizeToInt(g_runtimeLines.size()) - 1);
			view.notifyRuntimeOutputRangeChanged(changedIndex);

			QVERIFY(!nativeCanvas->grab().isNull());
			const QStringList tailLines =
			    nativeCanvas->property("qmud_native_plain_tail_lines").toStringList();
			QVERIFY(tailLines.contains(QStringLiteral("W <---(M)---> E")));
			QCOMPARE(tailLines.constLast(), QStringLiteral("<paint-sync-prompt> look"));
			resetTestState();
		}

		void nativeOutputRendererTrimIntoMergedHeadLineStaysIncremental()
		{
			resetTestState();

			WorldView view;
			view.resize(640, 420);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			view.appendOutputText(QStringLiteral("merge-a"), false);
			view.appendOutputText(QStringLiteral("merge-b"), true);
			view.appendOutputText(QStringLiteral("merge-c"), true);
			QCoreApplication::processEvents();
			nativeCanvas->repaint();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 2);
			QTRY_COMPARE(view.outputLines().value(0), QStringLiteral("merge-amerge-b"));
			QTRY_COMPARE(view.outputLines().value(1), QStringLiteral("merge-c"));
			const int rebuildsBefore = nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();
			const int updatesBefore = nativeCanvas->property("qmud_native_cache_incremental_updates").toInt();

			QVERIFY(!g_runtimeLines.isEmpty());
			g_runtimeLines.removeFirst();
			nativeCanvas->repaint();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 2);
			QTRY_COMPARE(view.outputLines().value(0), QStringLiteral("merge-b"));
			QTRY_COMPARE(view.outputLines().value(1), QStringLiteral("merge-c"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt(), rebuildsBefore);
			QTRY_VERIFY(nativeCanvas->property("qmud_native_cache_incremental_updates").toInt() >
			            updatesBefore);
			resetTestState();
		}

		void nativeOutputRendererNonContiguousMergedHeadTrimStaysIncremental()
		{
			resetTestState();

			WorldView view;
			view.resize(640, 420);
			view.show();
			view.setRuntime(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			auto *nativeCanvas = view.findChild<QWidget *>(QStringLiteral("worldOutputNativeCanvas"));
			QVERIFY(nativeCanvas);

			QVector<WorldRuntime::LineEntry> restoredLines;
			restoredLines.reserve(3);
			{
				WorldRuntime::LineEntry entry;
				entry.text       = QStringLiteral("noncontig-merge-a");
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = false;
				entry.lineNumber = 10;
				restoredLines.push_back(entry);
			}
			{
				WorldRuntime::LineEntry entry;
				entry.text       = QStringLiteral("noncontig-merge-b");
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				entry.lineNumber = 20;
				restoredLines.push_back(entry);
			}
			{
				WorldRuntime::LineEntry entry;
				entry.text       = QStringLiteral("noncontig-merge-c");
				entry.flags      = WorldRuntime::LineOutput;
				entry.hardReturn = true;
				entry.lineNumber = 30;
				restoredLines.push_back(entry);
			}
			g_runtimeLines = restoredLines;

			view.restoreOutputFromPersistedLines(restoredLines);
			nativeCanvas->repaint();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 2);
			QTRY_COMPARE(view.outputLines().value(0), QStringLiteral("noncontig-merge-anoncontig-merge-b"));
			QTRY_COMPARE(view.outputLines().value(1), QStringLiteral("noncontig-merge-c"));
			const int rebuildsBefore = nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();
			const int updatesBefore = nativeCanvas->property("qmud_native_cache_incremental_updates").toInt();

			g_runtimeLines.removeFirst();
			nativeCanvas->repaint();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 2);
			QTRY_COMPARE(view.outputLines().value(0), QStringLiteral("noncontig-merge-b"));
			QTRY_COMPARE(view.outputLines().value(1), QStringLiteral("noncontig-merge-c"));
			QTRY_COMPARE(nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt(), rebuildsBefore);
			QTRY_VERIFY(nativeCanvas->property("qmud_native_cache_incremental_updates").toInt() >
			            updatesBefore);
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

		void nativeOutputRendererTrimAppendKeepsLayoutMeasurementsLocal()
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

			for (int i = 0; i < 160; ++i)
				view.appendOutputText(
				    QStringLiteral("trim-layout-%1 ").arg(i) + QString(180, QLatin1Char('z')), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const int lineCountBefore = nativeCanvas->property("qmud_native_plain_line_count").toInt();
			const int rowsBefore      = nativeCanvas->property("qmud_native_layout_row_measurements").toInt();
			const int rebuildsBefore  = nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();
			QVERIFY(lineCountBefore > 40);
			QVERIFY(rowsBefore > 0);

			g_runtimeLines.removeFirst();
			view.appendOutputText(QStringLiteral("trim-layout-tail ") + QString(180, QLatin1Char('q')), true);
			QCoreApplication::processEvents();
			nativeCanvas->update();
			QCoreApplication::processEvents();

			const int rowsAfter = nativeCanvas->property("qmud_native_layout_row_measurements").toInt();
			QTRY_COMPARE(nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt(), rebuildsBefore);
			const int measuredDelta = rowsAfter - rowsBefore;
			QVERIFY2(
			    measuredDelta <= 64,
			    qPrintable(
			        QStringLiteral("Expected local layout remeasure only, delta=%1").arg(measuredDelta)));
			resetTestState();
		}

		void nativeOutputRendererHeadTrimKeepsLayoutGeometryAligned()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("12"));
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

			QString firstRemainingHref;
			for (int i = 0; i < 12; ++i)
			{
				const QString lineText = QStringLiteral("trim-geometry-%1 ").arg(i, 2, 10, QLatin1Char('0')) +
				                         QString(190, QLatin1Char('g'));
				const QString href =
				    QStringLiteral("https://example.org/trim-geometry/%1").arg(i, 2, 10, QLatin1Char('0'));
				if (i == 1)
					firstRemainingHref = href;
				WorldRuntime::StyleSpan span;
				span.length     = boundedSizeToInt(lineText.size());
				span.actionType = WorldRuntime::ActionHyperlink;
				span.action     = href;
				view.appendOutputTextStyled(lineText, {span}, true);
			}

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *scrollBar = browser->verticalScrollBar();
			QVERIFY(scrollBar);
			scrollBar->setValue(0);
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 12);
			const int     rebuildsBefore  = nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt();
			const int     trimDropsBefore = nativeCanvas->property("qmud_native_cache_trim_drops").toInt();

			const QString tailText = QStringLiteral("trim-geometry-tail ") + QString(190, QLatin1Char('t'));
			WorldRuntime::StyleSpan tailSpan;
			tailSpan.length     = boundedSizeToInt(tailText.size());
			tailSpan.actionType = WorldRuntime::ActionHyperlink;
			tailSpan.action     = QStringLiteral("https://example.org/trim-geometry/tail");
			view.appendOutputTextStyled(tailText, {tailSpan}, true);

			scrollBar->setValue(0);
			nativeCanvas->update();
			QCoreApplication::processEvents();

			QTRY_COMPARE(nativeCanvas->property("qmud_native_plain_line_count").toInt(), 12);
			QTRY_COMPARE(nativeCanvas->property("qmud_native_cache_full_rebuilds").toInt(), rebuildsBefore);
			QTRY_VERIFY(nativeCanvas->property("qmud_native_cache_trim_drops").toInt() > trimDropsBefore);
			QVERIFY(!view.outputLines().contains(QStringLiteral("trim-geometry-00 ") +
			                                     QString(190, QLatin1Char('g'))));
			QVERIFY(view.outputLines().contains(QStringLiteral("trim-geometry-01 ") +
			                                    QString(190, QLatin1Char('g'))));

			const QPoint firstRemainingPoint = findHyperlinkPoint(view, *browser, firstRemainingHref);
			QVERIFY2(firstRemainingPoint.x() >= 0 && firstRemainingPoint.y() >= 0,
			         "Expected first remaining wrapped hyperlink to stay hittable after native head trim.");

			QSignalSpy activatedSpy(&view, &WorldView::hyperlinkActivated);
			QTest::mouseClick(browser->viewport(), Qt::LeftButton, Qt::NoModifier, firstRemainingPoint);
			QTRY_COMPARE(activatedSpy.count(), 1);
			QCOMPARE(QUrl::fromPercentEncoding(activatedSpy.at(0).at(0).toString().toUtf8()),
			         firstRemainingHref);

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

		void keepCommandsOnSameLineRemainsMergedAfterCachePrimed()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("keep_commands_on_same_line"), QStringLiteral("1"));
			g_currentActionSource = WorldRuntime::eUserTyping;

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();

			view.appendOutputText(QStringLiteral("Ready."), true);
			QCOMPARE(view.outputLines().last(), QStringLiteral("Ready."));

			view.echoInputText(QStringLiteral("look\r\n"));

			const QStringList lines      = view.outputLines();
			const qsizetype   mergedLine = lines.indexOf(QStringLiteral("Ready.look"));
			QVERIFY(mergedLine >= 0);
			QCOMPARE(lines.indexOf(QStringLiteral("Ready.")), qsizetype{-1});
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

		void tabCompletionSkipsDuplicateMatchesWithinCycleAndResetsAfterTyping()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("tab_completion_space"), QStringLiteral("0"));

			WorldRuntime::LineEntry older;
			older.text       = QStringLiteral("stamina");
			older.flags      = WorldRuntime::LineOutput;
			older.hardReturn = true;
			g_runtimeLines.push_back(older);

			WorldRuntime::LineEntry middle;
			middle.text       = QStringLiteral("starch");
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
			QCOMPARE(view.inputText(), QStringLiteral("stamina"));

			QTest::keyClick(input, Qt::Key_A, Qt::ControlModifier);
			QTest::keyClicks(input, QStringLiteral("sta"));
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

		void outputFindSelectsExistingTextWhenDialogOpens()
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
					    combo->setCurrentText(QStringLiteral("beta"));
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });
			QVERIFY(view.doOutputFind(false));

			bool inspected = false;
			bool selected  = false;
			scheduleDialogInteraction(
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [&inspected, &selected](const QDialog *dialog)
			    {
				    inspected = true;
				    if (auto *combo = dialog->findChild<QComboBox *>())
				    {
					    if (QLineEdit *edit = combo->lineEdit())
						    selected = edit->selectedText() == QStringLiteral("beta");
				    }
				    QMetaObject::invokeMethod(const_cast<QDialog *>(dialog), "reject", Qt::QueuedConnection);
			    });

			QVERIFY(!view.doOutputFind(false));
			QVERIFY(inspected);
			QVERIFY(selected);

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

		void outputFindAnnouncesSelectedResultToQtAccessibility()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(720, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 30; ++i)
				view.appendOutputText(QStringLiteral("find-accessible-fill-before-%1").arg(i), true);
			const QString targetLine = QStringLiteral("prefix search-target-word suffix");
			view.appendOutputText(targetLine, true);
			for (int i = 0; i < 220; ++i)
				view.appendOutputText(QStringLiteral("find-accessible-fill-after-%1").arg(i), true);
			QCoreApplication::processEvents();

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			const QString targetText  = QStringLiteral("search-target-word");
			const QString outputText  = textInterface->text(0, textInterface->characterCount());
			const int     targetStart = stringIndexToInt(outputText.indexOf(targetText));
			QVERIFY(targetStart >= 0);
			const int targetEnd = targetStart + sizeToInt(targetText.size());

			scheduleDialogInteraction(
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [targetText](const QDialog *dialog)
			    {
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(targetText);
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });

			ScopedAccessibleUpdateCapture capture;
			QVERIFY(view.doOutputFind(false));
			QCoreApplication::processEvents();

			QCOMPARE(view.outputSelectionText(), targetText);
			int                        canvasCursorEventCount = 0;
			AccessibleTextCursorRecord lastCanvasCursorRecord;
			for (const AccessibleTextCursorRecord &record : g_accessibleTextCursorRecords)
			{
				if (record.object != canvas)
					continue;
				++canvasCursorEventCount;
				lastCanvasCursorRecord = record;
			}
			QCOMPARE(canvasCursorEventCount, 1);
			QCOMPARE(lastCanvasCursorRecord.position, targetStart);
			QCOMPARE(textInterface->cursorPosition(), targetStart);
			int                           canvasSelectionEventCount = 0;
			AccessibleTextSelectionRecord lastCanvasSelectionRecord;
			for (const AccessibleTextSelectionRecord &record : g_accessibleTextSelectionRecords)
			{
				if (record.object != canvas)
					continue;
				++canvasSelectionEventCount;
				lastCanvasSelectionRecord = record;
			}
			QCOMPARE(canvasSelectionEventCount, 1);
			QCOMPARE(lastCanvasSelectionRecord.start, targetStart);
			QCOMPARE(lastCanvasSelectionRecord.end, targetEnd);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 1);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleAnnouncementRecords.constLast().message, targetLine);
			int canvasInsertEventCount = 0;
			for (const AccessibleTextInsertRecord &record : g_accessibleTextInsertRecords)
			{
				if (record.object == canvas)
					++canvasInsertEventCount;
			}
			QCOMPARE(canvasInsertEventCount, 0);
			int canvasUpdateEventCount = 0;
			for (const AccessibleTextUpdateRecord &record : g_accessibleTextUpdateRecords)
			{
				if (record.object == canvas)
					++canvasUpdateEventCount;
			}
			QCOMPARE(canvasUpdateEventCount, 0);

			const int reviewCursorAfterClear = view.accessibleOutputReviewCursorPositionForTopPane();
			QVERIFY(reviewCursorAfterClear >= 0);
			QVERIFY(reviewCursorAfterClear != targetStart);
			g_accessibleTextCursorRecords.clear();
			g_accessibleTextSelectionRecords.clear();
			g_accessibleAnnouncementRecords.clear();
			view.clearNativeOutputSelection(true);
			QCoreApplication::processEvents();

			QVERIFY(view.isScrollbackSplitActive());
			QVERIFY(view.m_accessibleOutputReviewActive);
			QCOMPARE(view.m_accessibleOutputReviewTargetKind,
			         WorldView::AccessibleOutputReviewTargetKind::SplitTopPane);
			QCOMPARE(textInterface->cursorPosition(), reviewCursorAfterClear);
			QTRY_COMPARE(g_accessibleTextCursorRecords.size(), 1);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextCursorRecords.constLast().position, reviewCursorAfterClear);
			QTRY_COMPARE(g_accessibleTextSelectionRecords.size(), 1);
			QCOMPARE(g_accessibleTextSelectionRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextSelectionRecords.constLast().start, reviewCursorAfterClear);
			QCOMPARE(g_accessibleTextSelectionRecords.constLast().end, reviewCursorAfterClear);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);

			resetTestState();
		}

		void outputFindDoesNotSpeakWhenQtAccessibilitySpeechDisabled()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();
			const ScopedTestStateReset resetState;
			g_qtAccessibilitySpeechEnabled = false;

			WorldView view;
			view.resize(720, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 30; ++i)
				view.appendOutputText(QStringLiteral("find-muted-fill-before-%1").arg(i), true);
			const QString targetLine = QStringLiteral("prefix muted-search-target suffix");
			view.appendOutputText(targetLine, true);
			for (int i = 0; i < 220; ++i)
				view.appendOutputText(QStringLiteral("find-muted-fill-after-%1").arg(i), true);
			QCoreApplication::processEvents();

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			const QString targetText = QStringLiteral("muted-search-target");
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());

			scheduleDialogInteraction(
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [targetText](const QDialog *dialog)
			    {
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(targetText);
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });

			ScopedAccessibleUpdateCapture capture;
			QVERIFY(view.doOutputFind(false));
			QCoreApplication::processEvents();

			QCOMPARE(view.outputSelectionText(), targetText);
			int canvasCursorEventCount = 0;
			for (const AccessibleTextCursorRecord &record : g_accessibleTextCursorRecords)
			{
				if (record.object == canvas)
					++canvasCursorEventCount;
			}
			QCOMPARE(canvasCursorEventCount, 0);
			int canvasSelectionEventCount = 0;
			for (const AccessibleTextSelectionRecord &record : g_accessibleTextSelectionRecords)
			{
				if (record.object == canvas)
					++canvasSelectionEventCount;
			}
			QCOMPARE(canvasSelectionEventCount, 0);
			int canvasInsertEventCount = 0;
			for (const AccessibleTextInsertRecord &record : g_accessibleTextInsertRecords)
			{
				if (record.object == canvas)
					++canvasInsertEventCount;
			}
			QCOMPARE(canvasInsertEventCount, 0);
			int canvasUpdateEventCount = 0;
			for (const AccessibleTextUpdateRecord &record : g_accessibleTextUpdateRecords)
			{
				if (record.object == canvas)
					++canvasUpdateEventCount;
			}
			QCOMPARE(canvasUpdateEventCount, 0);
			int canvasAnnouncementEventCount = 0;
			for (const AccessibleAnnouncementRecord &record : g_accessibleAnnouncementRecords)
			{
				if (record.object == canvas)
					++canvasAnnouncementEventCount;
			}
			QCOMPARE(canvasAnnouncementEventCount, 0);
		}

		void outputFindUsesMushReaderSpeechForSelectedResultWhenItOwnsOutput()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();
			setFakeMushReaderLiveSpeechOwner(true);
			QVector<QMudNativePluginRegistry::TestSpeechEvent> speechEvents;
			const ScopedTestStateReset                         resetState;
			QMudNativePluginRegistry::setTestSpeechSink(
			    [&speechEvents](const QMudNativePluginRegistry::TestSpeechEvent &event)
			    { speechEvents.push_back(event); });

			WorldView view;
			view.resize(720, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			for (int i = 0; i < 30; ++i)
				view.appendOutputText(QStringLiteral("find-mushreader-fill-before-%1").arg(i), true);
			const QString targetLine = QStringLiteral("prefix mushreader-search-target suffix");
			view.appendOutputText(targetLine, true);
			for (int i = 0; i < 220; ++i)
				view.appendOutputText(QStringLiteral("find-mushreader-fill-after-%1").arg(i), true);
			QCoreApplication::processEvents();

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			const QString targetText  = QStringLiteral("mushreader-search-target");
			const QString outputText  = textInterface->text(0, textInterface->characterCount());
			const int     targetStart = stringIndexToInt(outputText.indexOf(targetText));
			QVERIFY(targetStart >= 0);
			const int targetEnd = targetStart + sizeToInt(targetText.size());

			scheduleDialogInteraction(
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [targetText](const QDialog *dialog)
			    {
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(targetText);
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });

			ScopedAccessibleUpdateCapture capture;
			QVERIFY(view.doOutputFind(false));
			QCoreApplication::processEvents();

			QCOMPARE(view.outputSelectionText(), targetText);
			int                        canvasCursorEventCount = 0;
			AccessibleTextCursorRecord lastCanvasCursorRecord;
			for (const AccessibleTextCursorRecord &record : g_accessibleTextCursorRecords)
			{
				if (record.object != canvas)
					continue;
				++canvasCursorEventCount;
				lastCanvasCursorRecord = record;
			}
			QCOMPARE(canvasCursorEventCount, 1);
			QCOMPARE(lastCanvasCursorRecord.position, targetStart);
			QCOMPARE(textInterface->cursorPosition(), targetStart);
			int                           canvasSelectionEventCount = 0;
			AccessibleTextSelectionRecord lastCanvasSelectionRecord;
			for (const AccessibleTextSelectionRecord &record : g_accessibleTextSelectionRecords)
			{
				if (record.object != canvas)
					continue;
				++canvasSelectionEventCount;
				lastCanvasSelectionRecord = record;
			}
			QCOMPARE(canvasSelectionEventCount, 1);
			QCOMPARE(lastCanvasSelectionRecord.start, targetStart);
			QCOMPARE(lastCanvasSelectionRecord.end, targetEnd);
			QCOMPARE(g_accessibleAnnouncementRecords.size(), 0);
			QCOMPARE(speechEvents.size(), 1);
			QCOMPARE(speechEvents.constLast().text, targetLine);
			QVERIFY(speechEvents.constLast().interrupt);
		}

		void outputFindAnnouncesVisibleTailResultWithoutStaleReviewCursor()
		{
			resetTestState();
			qmudInstallWorldOutputAccessibility();

			WorldView view;
			view.resize(720, 360);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QVERIFY(QTest::qWaitForWindowExposed(&view));

			view.appendOutputText(QStringLiteral("visible-tail-filler"), true);
			const QString targetLine = QStringLiteral("prefix visible-tail-search-target suffix");
			view.appendOutputText(targetLine, true);
			QCoreApplication::processEvents();

			QWidget *canvas = findNativeOutputCanvas(view);
			QVERIFY(canvas);
			QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
			QVERIFY(accessible);
			QAccessibleTextInterface *textInterface = accessible->textInterface();
			QVERIFY(textInterface);
			const QString targetText  = QStringLiteral("visible-tail-search-target");
			const QString outputText  = textInterface->text(0, textInterface->characterCount());
			const int     targetStart = stringIndexToInt(outputText.indexOf(targetText));
			QVERIFY(targetStart >= 0);
			const int targetEnd = targetStart + sizeToInt(targetText.size());
			QVERIFY(!view.isScrollbackSplitActive());
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());

			scheduleDialogInteraction(
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [targetText](const QDialog *dialog)
			    {
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(targetText);
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });

			ScopedAccessibleUpdateCapture capture;
			QVERIFY(view.doOutputFind(false));
			QCoreApplication::processEvents();

			QCOMPARE(view.outputSelectionText(), targetText);
			QVERIFY(!view.isScrollbackSplitActive());
			QVERIFY(view.m_accessibleOutputReviewActive);
			QCOMPARE(textInterface->cursorPosition(), targetStart);
			int                        canvasCursorEventCount = 0;
			AccessibleTextCursorRecord lastCanvasCursorRecord;
			for (const AccessibleTextCursorRecord &record : g_accessibleTextCursorRecords)
			{
				if (record.object != canvas)
					continue;
				++canvasCursorEventCount;
				lastCanvasCursorRecord = record;
			}
			QCOMPARE(canvasCursorEventCount, 1);
			QCOMPARE(lastCanvasCursorRecord.position, targetStart);
			int                           canvasSelectionEventCount = 0;
			AccessibleTextSelectionRecord lastCanvasSelectionRecord;
			for (const AccessibleTextSelectionRecord &record : g_accessibleTextSelectionRecords)
			{
				if (record.object != canvas)
					continue;
				++canvasSelectionEventCount;
				lastCanvasSelectionRecord = record;
			}
			QCOMPARE(canvasSelectionEventCount, 1);
			QCOMPARE(lastCanvasSelectionRecord.start, targetStart);
			QCOMPARE(lastCanvasSelectionRecord.end, targetEnd);
			int                          canvasAnnouncementEventCount = 0;
			AccessibleAnnouncementRecord lastCanvasAnnouncementRecord;
			for (const AccessibleAnnouncementRecord &record : g_accessibleAnnouncementRecords)
			{
				if (record.object != canvas)
					continue;
				++canvasAnnouncementEventCount;
				lastCanvasAnnouncementRecord = record;
			}
			QCOMPARE(canvasAnnouncementEventCount, 1);
			QCOMPARE(lastCanvasAnnouncementRecord.message, targetLine);

			g_accessibleTextSelectionRecords.clear();
			view.clearNativeOutputSelection(true);
			QCoreApplication::processEvents();

			QVERIFY(!view.m_accessibleOutputReviewActive);
			QCOMPARE(textInterface->cursorPosition(), textInterface->characterCount());
			QTRY_COMPARE(g_accessibleTextSelectionRecords.size(), 1);
			QCOMPARE(g_accessibleTextSelectionRecords.constLast().object, canvas);
			QCOMPARE(g_accessibleTextSelectionRecords.constLast().start, textInterface->characterCount());
			QCOMPARE(g_accessibleTextSelectionRecords.constLast().end, textInterface->characterCount());

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

		void outputFindActivatesSplitAndAnchorsLivePaneAtBottom()
		{
			resetTestState();

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			for (int i = 0; i < 320; ++i)
			{
				if (i == 48)
					view.appendOutputText(QStringLiteral("find_split_target_alpha"), true);
				else
					view.appendOutputText(QStringLiteral("find-split-fill-%1").arg(i), true);
			}
			QCoreApplication::processEvents();
			QVERIFY(!view.isScrollbackSplitActive());

			scheduleDialogInteraction(
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [](const QDialog *dialog)
			    {
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(QStringLiteral("find_split_target_alpha"));
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });

			QVERIFY(view.doOutputFind(false));
			QCOMPARE(view.outputSelectionText(), QStringLiteral("find_split_target_alpha"));
			QTRY_VERIFY(view.isScrollbackSplitActive());

			const auto [splitTop, splitBottom] = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			QScrollBar *topBar  = splitTop->verticalScrollBar();
			QScrollBar *liveBar = splitBottom->verticalScrollBar();
			QVERIFY(topBar);
			QVERIFY(liveBar);
			QTRY_VERIFY(topBar->value() < topBar->maximum());
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());

			view.appendOutputText(QStringLiteral("find-split-live-tail"), true);
			QCoreApplication::processEvents();
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());

			resetTestState();
		}

		void outputFindAgainKeepsSplitResultInTopPaneAndLivePaneAnchored()
		{
			resetTestState();

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			QCoreApplication::processEvents();

			for (int i = 0; i < 360; ++i)
			{
				if (i == 90 || i == 190)
					view.appendOutputText(QStringLiteral("find_split_target_beta"), true);
				else
					view.appendOutputText(QStringLiteral("find-next-split-fill-%1").arg(i), true);
			}
			QCoreApplication::processEvents();

			scheduleDialogInteraction(
			    [](const QDialog *dialog)
			    { return dialog->windowTitle() == QStringLiteral("Find in output buffer..."); },
			    [](const QDialog *dialog)
			    {
				    if (auto *combo = dialog->findChild<QComboBox *>())
					    combo->setCurrentText(QStringLiteral("find_split_target_beta"));
				    if (QPushButton *findButton = findButtonByText(*dialog, QStringLiteral("Find")))
					    QMetaObject::invokeMethod(findButton, "click", Qt::QueuedConnection);
			    });

			QVERIFY(view.doOutputFind(false));
			QCOMPARE(view.outputSelectionText(), QStringLiteral("find_split_target_beta"));
			const int firstMatchLine = view.outputSelectionStartLine();
			QVERIFY(firstMatchLine > 0);
			QTRY_VERIFY(view.isScrollbackSplitActive());

			const auto [splitTop, splitBottom] = findSplitOutputBrowsers(view);
			QVERIFY(splitTop);
			QVERIFY(splitBottom);
			QScrollBar *topBar  = splitTop->verticalScrollBar();
			QScrollBar *liveBar = splitBottom->verticalScrollBar();
			QVERIFY(topBar);
			QVERIFY(liveBar);
			QTRY_VERIFY(topBar->value() < topBar->maximum());
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());

			QVERIFY(view.doOutputFind(true));
			QCOMPARE(view.outputSelectionText(), QStringLiteral("find_split_target_beta"));
			const int nextMatchLine = view.outputSelectionStartLine();
			QVERIFY(nextMatchLine > 0);
			QVERIFY(nextMatchLine < firstMatchLine);
			QTRY_VERIFY(view.isScrollbackSplitActive());
			QTRY_VERIFY(topBar->value() < topBar->maximum());
			QTRY_COMPARE(liveBar->value(), liveBar->maximum());

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

		void arrowKeysNavigateInputCursorWhenHistoryTraversalDisabled()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("arrows_change_history"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.addToHistoryForced(QStringLiteral("north"));
			view.addToHistoryForced(QStringLiteral("south"));
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus();
			view.setInputText(QStringLiteral("line1\nline2"), true);

			QTextCursor cursor = input->textCursor();
			cursor.movePosition(QTextCursor::End);
			input->setTextCursor(cursor);
			QCoreApplication::processEvents();

			QCOMPARE(view.inputText(), QStringLiteral("line1\nline2"));
			QCOMPARE(input->textCursor().blockNumber(), 1);

			QTest::keyClick(input, Qt::Key_Up);
			QCOMPARE(view.inputText(), QStringLiteral("line1\nline2"));
			QCOMPARE(input->textCursor().blockNumber(), 0);

			QTest::keyClick(input, Qt::Key_Down);
			QCOMPARE(view.inputText(), QStringLiteral("line1\nline2"));
			QCOMPARE(input->textCursor().blockNumber(), 1);

			resetTestState();
		}

		void partialHistoryRecallUsesPrefixAndNewestFirst()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("arrows_change_history"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("arrow_recalls_partial"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.addToHistoryForced(QStringLiteral("hitman"));
			view.addToHistoryForced(QStringLiteral("hit man"));
			view.addToHistoryForced(QStringLiteral("hit123"));
			view.addToHistoryForced(QStringLiteral("hit 321"));
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus();
			view.setInputText(QStringLiteral("hit"), true);
			QCoreApplication::processEvents();

			QTest::keyClick(input, Qt::Key_Up, Qt::AltModifier);
			QCOMPARE(view.inputText(), QStringLiteral("hit 321"));

			QTest::keyClick(input, Qt::Key_Up, Qt::AltModifier);
			QCOMPARE(view.inputText(), QStringLiteral("hit123"));

			QTest::keyClick(input, Qt::Key_Up, Qt::AltModifier);
			QCOMPARE(view.inputText(), QStringLiteral("hit man"));

			QTest::keyClick(input, Qt::Key_Up, Qt::AltModifier);
			QCOMPARE(view.inputText(), QStringLiteral("hitman"));

			view.setInputText(QStringLiteral("hit "), true);
			QCoreApplication::processEvents();

			QTest::keyClick(input, Qt::Key_Up, Qt::AltModifier);
			QCOMPARE(view.inputText(), QStringLiteral("hit 321"));

			QTest::keyClick(input, Qt::Key_Up, Qt::AltModifier);
			QCOMPARE(view.inputText(), QStringLiteral("hit man"));

			resetTestState();
		}

		void partialHistoryRecallReseedsAfterManualEdit()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("arrows_change_history"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("arrow_recalls_partial"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.addToHistoryForced(QStringLiteral("hitman"));
			view.addToHistoryForced(QStringLiteral("hit man"));
			view.addToHistoryForced(QStringLiteral("hit123"));
			view.addToHistoryForced(QStringLiteral("hit 321"));
			QCoreApplication::processEvents();

			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);
			input->setFocus();
			view.setInputText(QStringLiteral("hit"), true);
			QCoreApplication::processEvents();

			QTest::keyClick(input, Qt::Key_Up, Qt::AltModifier);
			QCOMPARE(view.inputText(), QStringLiteral("hit 321"));

			QTest::keyClick(input, Qt::Key_A, Qt::ControlModifier);
			QTest::keyClicks(input, QStringLiteral("hit "));
			QCoreApplication::processEvents();

			QTest::keyClick(input, Qt::Key_Up, Qt::AltModifier);
			QCOMPARE(view.inputText(), QStringLiteral("hit 321"));

			QTest::keyClick(input, Qt::Key_Up, Qt::AltModifier);
			QCOMPARE(view.inputText(), QStringLiteral("hit man"));

			resetTestState();
		}

		void commandHistoryKeepsOnlyNewestDuplicateEntry()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();

			view.addToHistoryForced(QStringLiteral("look"));
			view.addToHistoryForced(QStringLiteral("north"));
			view.addToHistoryForced(QStringLiteral("look"));
			view.addToHistoryForced(QStringLiteral("south"));
			view.addToHistoryForced(QStringLiteral("look"));

			QCOMPARE(view.commandHistoryList(),
			         (QStringList{QStringLiteral("north"), QStringLiteral("south"), QStringLiteral("look")}));

			view.addToHistoryForced(QStringLiteral("look"));
			QCOMPARE(view.commandHistoryList(),
			         (QStringList{QStringLiteral("north"), QStringLiteral("south"), QStringLiteral("look")}));

			resetTestState();
		}

		void repeatLastCommandSendsHistoryTailAndPreservesInput()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("history_lines"), QStringLiteral("50"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			view.addToHistoryForced(QStringLiteral("qcmd-older-a14"));
			view.addToHistoryForced(QStringLiteral("qcmd-repeat-b92"));
			view.setInputText(QStringLiteral("qcmd-partial-c37"), true);
			QCoreApplication::processEvents();

			QSignalSpy sendSpy(&view, &WorldView::sendText);
			view.repeatLastCommand();

			QCOMPARE(sendSpy.size(), 1);
			QCOMPARE(sendSpy.constFirst().constFirst().toString(), QStringLiteral("qcmd-repeat-b92"));
			QCOMPARE(view.inputText(), QStringLiteral("qcmd-partial-c37"));
			QCOMPARE(view.commandHistoryList(),
			         (QStringList{QStringLiteral("qcmd-older-a14"), QStringLiteral("qcmd-repeat-b92")}));

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
			const QPoint point = findLineInformationPoint(*browser);
			QVERIFY2(point.x() >= 0 && point.y() >= 0,
			         "Expected line-information tooltip probe point in rendered output.");
			QTest::mouseMove(browser->viewport(), point);

			QTRY_VERIFY(QToolTip::text().contains(QStringLiteral("Line 1, ")));
			QTRY_VERIFY(QToolTip::text().contains(QStringLiteral("(unknown time)")));
			QToolTip::hideText();

			resetTestState();
		}

		void lineInformationTooltipDoesNotShowOnBlankAreaOfRenderedLine()
		{
			resetTestState();
			QToolTip::hideText();
			g_worldAttrs.insert(QStringLiteral("line_information"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("tool_tip_start_time"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("tool_tip_visible_time"), QStringLiteral("5000"));

			WorldRuntime::LineEntry entry;
			entry.text       = QStringLiteral("tip");
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
			const QPoint textPoint = findLineInformationPoint(*browser);
			QVERIFY2(textPoint.x() >= 0 && textPoint.y() >= 0,
			         "Expected line-information tooltip probe point in rendered output.");

			QTest::mouseMove(browser->viewport(), textPoint);
			QTRY_VERIFY(QToolTip::text().contains(QStringLiteral("Line 1, ")));

			const QRect  viewportRect = browser->viewport()->rect();
			const int    blankX = qBound(viewportRect.left(), viewportRect.right() - 2, viewportRect.right());
			const QPoint blankPoint(blankX, textPoint.y());
			QVERIFY(blankPoint.x() > textPoint.x());

			QTest::mouseMove(browser->viewport(), blankPoint);
			QTRY_VERIFY(QToolTip::text().isEmpty());

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
			QVERIFY(browser->findChildren<QTextDocument *>().isEmpty());

			view.updatePartialOutputText(QStringLiteral("par"));
			QCoreApplication::processEvents();
			QStringList lines = view.outputLines();
			QCOMPARE(lines.size(), 1);
			QCOMPARE(lines.first(), QStringLiteral("par"));
			QVERIFY(browser->findChildren<QTextDocument *>().isEmpty());

			view.updatePartialOutputText(QStringLiteral("part"));
			QCoreApplication::processEvents();
			lines = view.outputLines();
			QCOMPARE(lines.size(), 1);
			QCOMPARE(lines.first(), QStringLiteral("part"));
			QVERIFY(browser->findChildren<QTextDocument *>().isEmpty());

			view.clearPartialOutput();
			QCoreApplication::processEvents();
			QVERIFY(view.outputLines().isEmpty());
			QVERIFY(browser->findChildren<QTextDocument *>().isEmpty());

			view.updatePartialOutputText(QStringLiteral("temp"));
			view.appendOutputText(QStringLiteral("final"), true);
			QCoreApplication::processEvents();
			lines = view.outputLines();
			QVERIFY(!lines.isEmpty());
			QCOMPARE(lines.last(), QStringLiteral("final"));
			QVERIFY(!lines.contains(QStringLiteral("temp")));
			QVERIFY(browser->findChildren<QTextDocument *>().isEmpty());

			resetTestState();
		}

		void runtimePartialOutputCommitsBeforeLocalOutputBoundary()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("display_my_input"), QStringLiteral("1"));

			WorldView view;
			view.resize(760, 460);
			view.show();
			view.setRuntimeObserver(fakeRuntimePointer());
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QVERIFY(browser->findChildren<QTextDocument *>().isEmpty());

			view.appendOutputText(QStringLiteral("room"), true);
			const QString prompt = QStringLiteral("<2060hp 1694sp> ");
			setFakeRuntimePendingPartial(prompt);
			view.updatePartialOutputText(prompt);
			QCoreApplication::processEvents();

			QCOMPARE(g_pendingIncomingPartialCommitCount, 0);
			QCOMPARE(view.outputLines().constLast(), prompt);

			view.appendNoteText(QStringLiteral("[gmcp: update]"), true);
			QCoreApplication::processEvents();

			QStringList lines = view.outputLines();
			QCOMPARE(g_pendingIncomingPartialCommitCount, 1);
			QVERIFY(g_pendingIncomingPartialText.isEmpty());
			QVERIFY(lines.size() >= 3);
			QCOMPARE(lines.at(lines.size() - 2), prompt);
			QCOMPARE(lines.constLast(), QStringLiteral("[gmcp: update]"));
			QVERIFY(browser->findChildren<QTextDocument *>().isEmpty());

			const QString secondPrompt = QStringLiteral("<2060hp 1694sp> ");
			setFakeRuntimePendingPartial(secondPrompt);
			view.updatePartialOutputText(secondPrompt);
			g_currentActionSource = WorldRuntime::eUserTyping;
			view.echoInputText(QStringLiteral("look\r\n"));
			QCoreApplication::processEvents();

			lines = view.outputLines();
			QCOMPARE(g_pendingIncomingPartialCommitCount, 2);
			QVERIFY(lines.size() >= 5);
			QCOMPARE(lines.at(lines.size() - 2), secondPrompt);
			QCOMPARE(lines.constLast(), QStringLiteral("look"));
			QVERIFY(browser->findChildren<QTextDocument *>().isEmpty());

			g_worldAttrs.insert(QStringLiteral("keep_commands_on_same_line"), QStringLiteral("1"));
			view.applyRuntimeSettings();
			const QString thirdPrompt = QStringLiteral("<2060hp 1694sp> ");
			setFakeRuntimePendingPartial(thirdPrompt);
			view.updatePartialOutputText(thirdPrompt);
			g_currentActionSource = WorldRuntime::eUserTyping;
			view.echoInputText(QStringLiteral("north\r\n"));
			QCoreApplication::processEvents();

			lines = view.outputLines();
			QCOMPARE(g_pendingIncomingPartialCommitCount, 3);
			QVERIFY(lines.size() >= 6);
			QCOMPARE(lines.constLast(), thirdPrompt + QStringLiteral("north"));
			QVERIFY(browser->findChildren<QTextDocument *>().isEmpty());

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
			QVERIFY(browser->findChildren<QTextDocument *>().isEmpty());

			view.appendOutputText(QStringLiteral("pre-runtime-line"), true);
			QCoreApplication::processEvents();
			QVERIFY(view.outputLines().contains(QStringLiteral("pre-runtime-line")));
			QVERIFY(browser->findChildren<QTextDocument *>().isEmpty());

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
			QVERIFY(browser->findChildren<QTextDocument *>().isEmpty());

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
			spans.reserve(sizeToInt(text.size()));
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
			linkSpan.length     = boundedSizeToInt(QStringLiteral("example-link").size());
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
			span.length     = boundedSizeToInt(QStringLiteral("assistant").size());
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
			span.length     = boundedSizeToInt(QStringLiteral("assistant").size());
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

		void rightClickOnLinkPrefersSelectionMenuWhenOutputSelectionExists() const
		{
			resetTestState();

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.resize(720, 420);
			view.show();
			QCoreApplication::processEvents();

			WorldRuntime::StyleSpan span;
			span.length     = boundedSizeToInt(QStringLiteral("assistant").size());
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
			const QPoint globalAnchorPos = browser->viewport()->mapToGlobal(anchorPoint);

			struct MenuCaptureFilter final : QObject
			{
					explicit MenuCaptureFilter(QStringList *capturedActions, bool *captured)
					    : m_capturedActions(capturedActions), m_captured(captured)
					{
					}

					bool eventFilter(QObject *watched, QEvent *event) override
					{
						if (!m_capturedActions || !m_captured || *m_captured || event->type() != QEvent::Show)
							return QObject::eventFilter(watched, event);
						auto *menu = qobject_cast<QMenu *>(watched);
						if (!menu)
							return QObject::eventFilter(watched, event);

						const QList<QAction *> actions = menu->actions();
						m_capturedActions->reserve(actions.size());
						for (QAction *action : actions)
						{
							if (action)
								m_capturedActions->push_back(action->text());
						}
						*m_captured = true;
						QMetaObject::invokeMethod(menu, &QMenu::close, Qt::QueuedConnection);
						return QObject::eventFilter(watched, event);
					}

				private:
					QStringList *m_capturedActions{nullptr};
					bool        *m_captured{nullptr};
			};

			auto captureWorldMenuActions = [&view](const QPoint &globalPos) -> QStringList
			{
				QStringList       capturedActions;
				bool              captured = false;
				MenuCaptureFilter filter(&capturedActions, &captured);
				qApp->installEventFilter(&filter);
				const bool shown = view.showWorldContextMenuAtGlobalPos(globalPos);
				qApp->removeEventFilter(&filter);
				if (!shown || !captured)
					return {};
				return capturedActions;
			};

			const QStringList linkMenuActions = captureWorldMenuActions(globalAnchorPos);
			QCOMPARE(linkMenuActions,
			         (QStringList{QStringLiteral("Examine assistant"), QStringLiteral("Consider assistant"),
			                      QStringLiteral("Attack assistant")}));

			view.selectOutputRange(0, 0, 3);
			QTRY_VERIFY(view.hasOutputSelection());
			const QStringList selectionMenuActions = captureWorldMenuActions(globalAnchorPos);
			QCOMPARE(selectionMenuActions,
			         (QStringList{QStringLiteral("Copy"), QStringLiteral("Copy as HTML")}));

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
			hotspot.mouseUp   = QStringLiteral("on_hotspot_up");
			window.hotspots.insert(QStringLiteral("hotspot_main"), hotspot);

			const QPoint clickInStack    = windowRect.center();
			const QPoint clickInViewport = viewport->mapFrom(outputStack, clickInStack);
			QVERIFY(viewport->rect().contains(clickInViewport));

			const int baseline = g_worldHotspotCallbackCount;
			QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, clickInViewport);
			QCoreApplication::processEvents();

			QTRY_VERIFY(g_worldHotspotCallbackCount >= baseline + 2);
			QCOMPARE(g_worldHotspotCallbackFunctions.at(baseline), QStringLiteral("on_hotspot_down"));
			QCOMPARE(g_worldHotspotCallbackFunctions.at(baseline + 1), QStringLiteral("on_hotspot_up"));
			QVERIFY(!g_worldHotspotCallbackQueueWhenBusy.at(baseline));
			QVERIFY(!g_worldHotspotCallbackQueueWhenBusy.at(baseline + 1));
			QCOMPARE(g_lastWorldHotspotFunction, QStringLiteral("on_hotspot_up"));
			QCOMPARE(g_lastWorldHotspotId, QStringLiteral("hotspot_main"));
			QVERIFY(!g_lastWorldHotspotQueueWhenBusy);
			resetTestState();
		}

		void nativeOutputRendererRightButtonMouseUpQueuesHotspotWhenLaneBusy()
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

			MiniWindow &window = appendTestMiniWindow(QStringLiteral("native-right-hotspot"), windowRect, 0,
			                                          QColor(0, 160, 0));
			MiniWindowHotspot hotspot;
			hotspot.rect      = QRect(0, 0, windowRect.width(), windowRect.height());
			hotspot.mouseDown = QStringLiteral("on_right_hotspot_down");
			hotspot.mouseUp   = QStringLiteral("on_right_hotspot_up");
			window.hotspots.insert(QStringLiteral("hotspot_context"), hotspot);

			const QPoint clickInStack    = windowRect.center();
			const QPoint clickInViewport = viewport->mapFrom(outputStack, clickInStack);
			QVERIFY(viewport->rect().contains(clickInViewport));

			const int baseline = g_worldHotspotCallbackCount;
			QTest::mouseClick(viewport, Qt::RightButton, Qt::NoModifier, clickInViewport);
			QCoreApplication::processEvents();

			QTRY_VERIFY(g_worldHotspotCallbackCount >= baseline + 2);
			QCOMPARE(g_worldHotspotCallbackFunctions.at(baseline), QStringLiteral("on_right_hotspot_down"));
			QCOMPARE(g_worldHotspotCallbackFunctions.at(baseline + 1), QStringLiteral("on_right_hotspot_up"));
			QVERIFY(!g_worldHotspotCallbackQueueWhenBusy.at(baseline));
			QVERIFY(g_worldHotspotCallbackQueueWhenBusy.at(baseline + 1));
			QCOMPARE(g_lastWorldHotspotFunction, QStringLiteral("on_right_hotspot_up"));
			QCOMPARE(g_lastWorldHotspotId, QStringLiteral("hotspot_context"));
			QVERIFY(g_lastWorldHotspotQueueWhenBusy);
			resetTestState();
		}

		void nativeOutputRendererOverlayMiniWindowMoveClearsOldBounds()
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

			constexpr QColor overlayColour(220, 30, 40, 255);
			const QRect      oldRect(24, 24, 90, 48);
			const QRect      newRect(180, 120, 90, 48);
			appendTestMiniWindow(QStringLiteral("native-overlay-move"), oldRect, 0, overlayColour);
			view.onMiniWindowsChanged();
			QCoreApplication::processEvents();
			outputStack->update();
			QCoreApplication::processEvents();

			QTRY_VERIFY(widgetRectMostlyMatchesColor(outputStack, oldRect, overlayColour, 80));

			g_testMiniWindows[0].rect     = newRect;
			g_testMiniWindows[0].location = newRect.topLeft();
			view.onMiniWindowsChanged();
			QCoreApplication::processEvents();
			outputStack->update();
			QCoreApplication::processEvents();

			const int oldPixels = oldRect.width() * oldRect.height();
			QVERIFY(countWidgetPixelsNearColor(outputStack, oldRect, overlayColour) * 4 < oldPixels);
			QTRY_VERIFY(widgetRectMostlyMatchesColor(outputStack, newRect, overlayColour, 80));
			resetTestState();
		}

		void nativeOutputRendererFastMiniWindowDragRepaintsCapturedOverlay()
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
			QVERIFY(viewportInStack.width() > 260);
			QVERIFY(viewportInStack.height() > 160);

			constexpr QColor overlayColour(30, 170, 60, 255);
			const QRect      startRect(viewportInStack.left() + 24, viewportInStack.top() + 24, 100, 56);
			MiniWindow      &window =
			    appendTestMiniWindow(QStringLiteral("native-fast-drag"), startRect, 0, overlayColour);
			MiniWindowHotspot hotspot;
			hotspot.rect         = QRect(0, 0, startRect.width(), startRect.height());
			hotspot.moveCallback = QStringLiteral("drag_test_move");
			window.hotspots.insert(QStringLiteral("drag"), hotspot);
			view.onMiniWindowsChanged();
			QCoreApplication::processEvents();

			const QPoint pressInStack    = startRect.center();
			const QPoint pressInViewport = viewport->mapFrom(outputStack, pressInStack);
			QVERIFY(viewport->rect().contains(pressInViewport));
			QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, pressInViewport);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isMiniWindowCaptureActive());

			const QVector<QPoint> dragPoints{
			    QPoint(viewportInStack.left() + 210, viewportInStack.top() + 42),
			    QPoint(viewportInStack.left() + 80, viewportInStack.top() + 134),
			    QPoint(viewportInStack.left() + 236, viewportInStack.top() + 142),
			};
			for (const QPoint &pointInStack : dragPoints)
			{
				const QPoint local  = viewport->mapFrom(outputStack, pointInStack);
				const QPoint global = viewport->mapToGlobal(local);
				QMouseEvent  moveEvent(QEvent::MouseMove, QPointF(local), QPointF(local), QPointF(global),
				                       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
				QCoreApplication::sendEvent(viewport, &moveEvent);
				QCoreApplication::processEvents();
			}

			QCoreApplication::processEvents();
			QTRY_VERIFY(widgetRectMostlyMatchesColor(outputStack, g_testMiniWindows.constFirst().rect,
			                                         overlayColour, 80));

			const QPoint releaseLocal = viewport->mapFrom(outputStack, dragPoints.constLast());
			QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, releaseLocal);
			QCoreApplication::processEvents();
			QTRY_VERIFY(!view.isMiniWindowCaptureActive());
			resetTestState();
		}

		void nativeOutputRendererMiniWindowCaptureRoutesEventsFromSiblingWidget()
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
			QPlainTextEdit *input = view.inputEditor();
			QVERIFY(input);

			const QRect viewportInStack(viewport->mapTo(outputStack, QPoint(0, 0)), viewport->size());
			QVERIFY(viewportInStack.width() > 260);
			QVERIFY(viewportInStack.height() > 160);

			constexpr QColor overlayColour(30, 170, 60, 255);
			const QRect      startRect(viewportInStack.left() + 24, viewportInStack.top() + 24, 100, 56);
			MiniWindow &window = appendTestMiniWindow(QStringLiteral("native-sibling-routed-drag"), startRect,
			                                          0, overlayColour);
			MiniWindowHotspot hotspot;
			hotspot.rect         = QRect(0, 0, startRect.width(), startRect.height());
			hotspot.moveCallback = QStringLiteral("drag_test_move");
			window.hotspots.insert(QStringLiteral("drag"), hotspot);
			view.onMiniWindowsChanged();
			QCoreApplication::processEvents();

			const QPoint pressInStack    = startRect.center();
			const QPoint pressInViewport = viewport->mapFrom(outputStack, pressInStack);
			QVERIFY(viewport->rect().contains(pressInViewport));
			QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, pressInViewport);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isMiniWindowCaptureActive());

			const QPoint moveInStack = QPoint(viewportInStack.left() + 220, viewportInStack.top() + 90);
			const QPoint moveGlobal  = outputStack->mapToGlobal(moveInStack);
			const QPoint moveInInput = input->mapFromGlobal(moveGlobal);
			QMouseEvent  moveEvent(QEvent::MouseMove, QPointF(moveInInput), QPointF(moveInInput),
			                       QPointF(moveGlobal), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
			QCoreApplication::sendEvent(input, &moveEvent);
			QCoreApplication::processEvents();

			QCOMPARE(g_testMiniWindows.constFirst().clientMousePosition, moveInStack);
			QTRY_VERIFY(widgetRectMostlyMatchesColor(outputStack, g_testMiniWindows.constFirst().rect,
			                                         overlayColour, 80));

			QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(moveInInput), QPointF(moveInInput),
			                         QPointF(moveGlobal), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
			QCoreApplication::sendEvent(input, &releaseEvent);
			QCoreApplication::processEvents();
			QTRY_VERIFY(!view.isMiniWindowCaptureActive());
			resetTestState();
		}

		void nativeOutputRendererMiniWindowResizeHotspotUsesPressOrigin()
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
			QVERIFY(viewportInStack.width() > 260);
			QVERIFY(viewportInStack.height() > 160);

			constexpr QColor overlayColour(40, 90, 210, 255);
			const QRect      startRect(viewportInStack.left() + 30, viewportInStack.top() + 30, 96, 54);
			MiniWindow      &window =
			    appendTestMiniWindow(QStringLiteral("native-resize-drag"), startRect, 0, overlayColour);
			MiniWindowHotspot hotspot;
			hotspot.rect         = QRect(startRect.width() - 12, startRect.height() - 12, 12, 12);
			hotspot.mouseDown    = QStringLiteral("drag_test_resize_down");
			hotspot.moveCallback = QStringLiteral("drag_test_resize");
			window.hotspots.insert(QStringLiteral("resize"), hotspot);
			view.onMiniWindowsChanged();
			QCoreApplication::processEvents();

			const QPoint pressInStack    = QPoint(startRect.right() - 2, startRect.bottom() - 2);
			const QPoint pressInViewport = viewport->mapFrom(outputStack, pressInStack);
			QVERIFY(viewport->rect().contains(pressInViewport));
			QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, pressInViewport);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isMiniWindowCaptureActive());
			QCOMPARE(g_testMiniWindows.constFirst().rect, startRect);
			QCOMPARE(g_lastResizeHotspotPressOffset, pressInStack - startRect.topLeft());

			const QPoint firstMoveInStack = pressInStack + QPoint(18, 9);
			sendMiniWindowMouseMove(viewport, outputStack, firstMoveInStack);
			QCoreApplication::processEvents();

			const QRect firstResizeRect = g_testMiniWindows.constFirst().rect;
			QCOMPARE(firstResizeRect.topLeft(), startRect.topLeft());
			QCOMPARE(firstResizeRect.width(), firstMoveInStack.x() - startRect.left() + 1);
			QCOMPARE(firstResizeRect.height(), firstMoveInStack.y() - startRect.top() + 1);
			QCOMPARE(g_testMiniWindows.constFirst().lastMousePosition,
			         firstMoveInStack - startRect.topLeft());

			const QPoint fastMoveInStack = pressInStack + QPoint(74, 33);
			sendMiniWindowMouseMove(viewport, outputStack, fastMoveInStack);
			QCoreApplication::processEvents();
			const QRect fastResizeRect = g_testMiniWindows.constFirst().rect;
			QCOMPARE(fastResizeRect.topLeft(), startRect.topLeft());
			QCOMPARE(fastResizeRect.width(), fastMoveInStack.x() - startRect.left() + 1);
			QCOMPARE(fastResizeRect.height(), fastMoveInStack.y() - startRect.top() + 1);
			QCOMPARE(g_testMiniWindows.constFirst().lastMousePosition, fastMoveInStack - startRect.topLeft());
			QTRY_VERIFY(widgetRectMostlyMatchesColor(outputStack, fastResizeRect, overlayColour, 75));

			QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier,
			                    viewport->mapFrom(outputStack, fastMoveInStack));
			QCoreApplication::processEvents();
			QTRY_VERIFY(!view.isMiniWindowCaptureActive());
			resetTestState();
		}

		void nativeOutputRendererMiniWindowCapturedResizeUsesUnclampedDragPoint()
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
			QVERIFY(viewportInStack.width() > 260);
			QVERIFY(viewportInStack.height() > 160);

			constexpr QColor overlayColour(40, 90, 210, 255);
			const QRect      startRect(viewportInStack.left() + 30, viewportInStack.bottom() - 58, 96, 54);
			QVERIFY(viewportInStack.contains(startRect.topLeft()));
			QVERIFY(viewportInStack.contains(startRect.bottomRight()));

			MiniWindow &window =
			    appendTestMiniWindow(QStringLiteral("native-resize-unclamped"), startRect, 0, overlayColour);
			MiniWindowHotspot hotspot;
			hotspot.rect         = QRect(startRect.width() - 12, startRect.height() - 12, 12, 12);
			hotspot.mouseDown    = QStringLiteral("drag_test_resize_down");
			hotspot.moveCallback = QStringLiteral("drag_test_resize");
			window.hotspots.insert(QStringLiteral("resize"), hotspot);
			view.onMiniWindowsChanged();
			QCoreApplication::processEvents();

			const QPoint pressInStack    = QPoint(startRect.right() - 2, startRect.bottom() - 2);
			const QPoint pressInViewport = viewport->mapFrom(outputStack, pressInStack);
			QVERIFY(viewport->rect().contains(pressInViewport));
			QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, pressInViewport);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isMiniWindowCaptureActive());

			const QPoint outsideMoveInStack(pressInStack.x() + 18, viewportInStack.bottom() + 32);
			QVERIFY(!viewportInStack.contains(outsideMoveInStack));
			sendMiniWindowMouseMove(viewport, outputStack, outsideMoveInStack);
			QCoreApplication::processEvents();

			const QRect resizedRect = g_testMiniWindows.constFirst().rect;
			QCOMPARE(resizedRect.topLeft(), startRect.topLeft());
			QCOMPARE(resizedRect.width(), outsideMoveInStack.x() - startRect.left() + 1);
			QCOMPARE(resizedRect.height(), outsideMoveInStack.y() - startRect.top() + 1);
			QCOMPARE(g_testMiniWindows.constFirst().lastMousePosition,
			         outsideMoveInStack - startRect.topLeft());
			QCOMPARE(g_testMiniWindows.constFirst().clientMousePosition, outsideMoveInStack);

			sendMiniWindowMouseRelease(viewport, outputStack, outsideMoveInStack);
			QTRY_VERIFY(!view.isMiniWindowCaptureActive());
			resetTestState();
		}

		void nativeOutputRendererMiniWindowReleaseFlushesPendingResizeDrag()
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
			QVERIFY(viewportInStack.width() > 260);
			QVERIFY(viewportInStack.height() > 160);

			constexpr QColor  overlayColour(40, 90, 210, 255);
			const QRect       startRect(viewportInStack.left() + 30, viewportInStack.top() + 30, 96, 54);
			MiniWindow       &window = appendTestMiniWindow(QStringLiteral("native-release-flush-resize"),
			                                                startRect, 0, overlayColour);
			MiniWindowHotspot hotspot;
			hotspot.rect            = QRect(startRect.width() - 12, startRect.height() - 12, 12, 12);
			hotspot.mouseDown       = QStringLiteral("drag_test_resize_down");
			hotspot.moveCallback    = QStringLiteral("drag_test_resize");
			hotspot.releaseCallback = QStringLiteral("drag_test_release_after_resize");
			window.hotspots.insert(QStringLiteral("resize"), hotspot);
			view.onMiniWindowsChanged();
			QCoreApplication::processEvents();

			const QPoint pressInStack    = QPoint(startRect.right() - 2, startRect.bottom() - 2);
			const QPoint pressInViewport = viewport->mapFrom(outputStack, pressInStack);
			QVERIFY(viewport->rect().contains(pressInViewport));
			const int callbackBaseline = g_worldHotspotCallbackCount;
			QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, pressInViewport);
			QCoreApplication::processEvents();
			QTRY_VERIFY(view.isMiniWindowCaptureActive());

			const QPoint moveInStack    = pressInStack + QPoint(64, 28);
			const QPoint moveInViewport = viewport->mapFrom(outputStack, moveInStack);
			const QPoint moveGlobal     = viewport->mapToGlobal(moveInViewport);
			QMouseEvent  moveEvent(QEvent::MouseMove, QPointF(moveInViewport), QPointF(moveInViewport),
			                       QPointF(moveGlobal), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
			QCoreApplication::sendEvent(viewport, &moveEvent);
			QCOMPARE(g_resizeMoveCallbackCount, 0);

			g_expectedReleaseResizeSize =
			    QSize(moveInStack.x() - startRect.left() + 1, moveInStack.y() - startRect.top() + 1);
			g_expectedReleaseMousePosition = moveInStack;
			QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, moveInViewport);
			QCoreApplication::processEvents();

			QCOMPARE(g_resizeMoveCallbackCount, 1);
			QVERIFY(g_releaseCallbackSawFlushedResize);
			QTRY_VERIFY(g_worldHotspotCallbackCount >= callbackBaseline + 3);
			QCOMPARE(g_worldHotspotCallbackFunctions.at(callbackBaseline + 2),
			         QStringLiteral("drag_test_release_after_resize"));
			QTRY_VERIFY(!view.isMiniWindowCaptureActive());
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

		void nativeOutputSelectionDragStopsWhenMouseMoveHasNoPressedButton()
		{
			resetTestState();

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.resize(760, 460);
			view.show();
			QCoreApplication::processEvents();

			view.appendOutputText(
			    QStringLiteral("native-drag-stop-target-abcdefghijklmnopqrstuvwxyz-0123456789"), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QWidget *viewport = browser->viewport();
			QVERIFY(viewport);
			QVERIFY(viewport->width() > 80);
			QVERIFY(viewport->height() > 24);

			const QPoint dragStart(10, qBound(10, viewport->height() / 2, viewport->height() - 10));
			const QPoint dragFar(qBound(20, viewport->width() - 10, viewport->width() - 10), dragStart.y());
			const QPoint dragNear(18, dragStart.y());
			QVERIFY(viewport->rect().contains(dragStart));
			QVERIFY(viewport->rect().contains(dragFar));
			QVERIFY(viewport->rect().contains(dragNear));

			QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, dragStart);
			QTest::mouseMove(viewport, dragFar, 5);
			QTRY_VERIFY(view.hasOutputSelection());
			const QString selectionBeforeLostMove = view.outputSelectionText();
			QVERIFY(!selectionBeforeLostMove.isEmpty());

			const QPoint globalNear = viewport->mapToGlobal(dragNear);
			QMouseEvent  lostMove(QEvent::MouseMove, QPointF(dragNear), QPointF(globalNear), Qt::NoButton,
			                      Qt::NoButton, Qt::NoModifier);
			QCoreApplication::sendEvent(viewport, &lostMove);
			QCoreApplication::processEvents();
			QCOMPARE(view.outputSelectionText(), selectionBeforeLostMove);

			const QPoint globalFar = viewport->mapToGlobal(dragFar);
			QMouseEvent  hoverMove(QEvent::MouseMove, QPointF(dragFar), QPointF(globalFar), Qt::NoButton,
			                       Qt::NoButton, Qt::NoModifier);
			QCoreApplication::sendEvent(viewport, &hoverMove);
			QCoreApplication::processEvents();
			QCOMPARE(view.outputSelectionText(), selectionBeforeLostMove);

			QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, dragFar);
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

			const QVector<QPair<QString, QString>> apostropheActions = WorldView::parseMxpContextMenuActions(
			    QStringLiteral("examine a scroll of 'harm'|consider a scroll of 'harm'"),
			    QStringLiteral(
			        "Right mouse click to act|Examine a scroll of 'harm'|Consider a scroll of 'harm'"));
			QCOMPARE(apostropheActions.size(), 2);
			QCOMPARE(apostropheActions.at(0).first, QStringLiteral("examine a scroll of 'harm'"));
			QCOMPARE(apostropheActions.at(0).second, QStringLiteral("Examine a scroll of 'harm'"));
			QCOMPARE(apostropheActions.at(1).first, QStringLiteral("consider a scroll of 'harm'"));
			QCOMPARE(apostropheActions.at(1).second, QStringLiteral("Consider a scroll of 'harm'"));
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

		void nativeWrappedHyperlinkSurvivesCappedIncrementalRestitch()
		{
			resetTestState();
			g_worldAttrs.insert(QStringLiteral("max_output_lines"), QStringLiteral("35"));
			g_worldAttrs.insert(QStringLiteral("wrap"), QStringLiteral("1"));
			g_worldAttrs.insert(QStringLiteral("auto_wrap_window_width"), QStringLiteral("0"));
			g_worldAttrs.insert(QStringLiteral("wrap_column"), QStringLiteral("28"));
			g_worldAttrs.insert(QStringLiteral("indent_paras"), QStringLiteral("0"));

			WorldView view;
			view.setRuntimeObserver(fakeRuntimePointer());
			view.resize(720, 420);
			view.show();
			view.applyRuntimeSettings();
			QCoreApplication::processEvents();

			for (int i = 0; i < 35; ++i)
				view.appendOutputText(QStringLiteral("wrapped-link-primer-%1").arg(i), true);

			const QString           prefix = QStringLiteral("before words ");
			const QString           linked = QStringLiteral("click-target");
			const QString           suffix = QStringLiteral(" after words wrap here");
			const QString           text   = prefix + linked + suffix;
			WorldRuntime::StyleSpan prefixSpan;
			prefixSpan.length = boundedSizeToInt(prefix.size());
			WorldRuntime::StyleSpan linkSpan;
			linkSpan.length     = boundedSizeToInt(linked.size());
			linkSpan.actionType = WorldRuntime::ActionHyperlink;
			linkSpan.action     = QStringLiteral("https://example.org/restitch-link");
			WorldRuntime::StyleSpan suffixSpan;
			suffixSpan.length = boundedSizeToInt(suffix.size());
			appendFakeRuntimeOutputText(view, text, {prefixSpan, linkSpan, suffixSpan}, false, true);

			for (int i = 0; i < 4; ++i)
				view.appendOutputText(QStringLiteral("wrapped-link-tail-%1").arg(i), true);
			QCoreApplication::processEvents();

			QTextBrowser *browser = findVisibleOutputBrowser(view);
			QVERIFY(browser);
			QScrollBar *scrollBar = browser->verticalScrollBar();
			QVERIFY(scrollBar);
			scrollBar->setValue(scrollBar->maximum());
			QCoreApplication::processEvents();

			const QPoint anchorPoint = findHyperlinkPoint(view, *browser, linkSpan.action);
			QVERIFY2(anchorPoint.x() >= 0 && anchorPoint.y() >= 0,
			         "Expected wrapped hyperlink hit target after capped incremental restitch.");

			QSignalSpy activatedSpy(&view, &WorldView::hyperlinkActivated);
			QTest::mouseClick(browser->viewport(), Qt::LeftButton, Qt::NoModifier, anchorPoint);
			QTRY_COMPARE(activatedSpy.count(), 1);
			QCOMPARE(QUrl::fromPercentEncoding(activatedSpy.at(0).at(0).toString().toUtf8()),
			         linkSpan.action);
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

#include "tst_WorldView_Basic.moc"
