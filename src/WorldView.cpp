/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldView.cpp
 * Role: World output-view rendering and interaction implementation, including text painting, selection, and viewport
 * behavior.
 */

#include "WorldView.h"
#include "AcceleratorUtils.h"
#include "AppController.h"
#include "FontUtils.h"
#include "MainFrame.h"
#include "MainWindowHost.h"
#include "MainWindowHostResolver.h"
#include "Version.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "dialogs/CommandHistoryDialog.h"
#include "dialogs/FindDialog.h"
#include "scripting/ScriptingErrors.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextOption>
#include <QTimer>
#include <QToolTip>
#include <QUrl>

#include <limits>
#include <memory>
#include <ranges>

namespace
{
	int sizeToInt(const qsizetype value)
	{
		constexpr qsizetype kMin = 0;
		constexpr qsizetype kMax = std::numeric_limits<int>::max();
		return static_cast<int>(qBound(kMin, value, kMax));
	}

	QString anchorAtGlobalCursor(const QTextBrowser *browser)
	{
		if (!browser || !browser->isVisible() || !browser->viewport())
			return {};
		const QPoint localPos = browser->viewport()->mapFromGlobal(QCursor::pos());
		if (!browser->viewport()->rect().contains(localPos))
			return {};
		return browser->anchorAt(localPos).trimmed();
	}

	class ContextMenuDismissReplayFilter final : public QObject
	{
		public:
			explicit ContextMenuDismissReplayFilter(const QMenu *menu) : m_menu(menu)
			{
			}

			[[nodiscard]] bool hasReplayPoint() const
			{
				return m_hasReplayPoint;
			}

			[[nodiscard]] QPoint replayPoint() const
			{
				return m_replayPoint;
			}

		protected:
			bool eventFilter(QObject *watched, QEvent *event) override
			{
				if (const auto *watchedWidget = qobject_cast<QWidget *>(watched);
				    m_menu && watchedWidget &&
				    (watchedWidget == m_menu || m_menu->isAncestorOf(watchedWidget)))
					return false;

				if (event->type() == QEvent::MouseButtonPress)
				{
					if (const auto *mouse = dynamic_cast<QMouseEvent *>(event);
					    mouse && mouse->button() == Qt::RightButton)
					{
						m_hasReplayPoint = true;
						m_replayPoint    = mouse->globalPosition().toPoint();
					}
				}
				else if (event->type() == QEvent::ContextMenu)
				{
					if (const auto *contextEvent = dynamic_cast<QContextMenuEvent *>(event);
					    contextEvent && contextEvent->reason() == QContextMenuEvent::Mouse)
					{
						m_hasReplayPoint = true;
						m_replayPoint    = contextEvent->globalPos();
					}
				}
				return false;
			}

		private:
			const QMenu *m_menu{nullptr};
			bool         m_hasReplayPoint{false};
			QPoint       m_replayPoint;
	};

	void forceOpaqueMenu(QMenu *menu);

	void trimTrailingPromptWhitespace(QTextDocument *document)
	{
		if (!document)
			return;

		QTextCursor cursor(document);
		cursor.movePosition(QTextCursor::End);
		while (cursor.position() > 0)
		{
			cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, 1);
			const QString selected = cursor.selectedText();
			if (selected.isEmpty())
				break;
			const QChar ch = selected.at(0);
			if (ch == QChar::ParagraphSeparator || ch == QChar::LineSeparator || ch == QLatin1Char('\n') ||
			    ch == QLatin1Char('\r'))
			{
				cursor.clearSelection();
				break;
			}
			if (ch == QLatin1Char(' ') || ch == QLatin1Char('\t') || ch == QChar(0x00A0))
			{
				cursor.removeSelectedText();
				continue;
			}
			cursor.clearSelection();
			break;
		}
	}

	constexpr int kMiniMouseShift      = 0x01;
	constexpr int kMiniMouseCtrl       = 0x02;
	constexpr int kMiniMouseAlt        = 0x04;
	constexpr int kMiniMouseLeft       = 0x10;
	constexpr int kMiniMouseRight      = 0x20;
	constexpr int kMiniMouseDouble     = 0x40;
	constexpr int kMiniMouseNotFirst   = 0x80;
	constexpr int kMiniMouseScrollBack = 0x100;
	constexpr int kMiniMouseMiddle     = 0x200;
	const auto    kLineInfoTooltipId   = QStringLiteral("__line_info__");

	bool          miniWindowMouseDebugEnabled()
	{
		static const bool enabled = !qEnvironmentVariableIsEmpty("QMUD_DEBUG_MINIWINDOW_MOUSE");
		return enabled;
	}

	void miniWindowMouseDebug(const QString &message)
	{
		if (!miniWindowMouseDebugEnabled())
			return;
		fprintf(stderr, "MINIWIN_MOUSE %s\n", message.toUtf8().constData());
	}

	int withMiniWindowModifierFlags(int flags)
	{
		const Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();
		if (modifiers & Qt::ShiftModifier)
			flags |= kMiniMouseShift;
		if (modifiers & Qt::ControlModifier)
			flags |= kMiniMouseCtrl;
		if (modifiers & Qt::AltModifier)
			flags |= kMiniMouseAlt;
		return flags;
	}

	QCursor hotspotCursor(int code)
	{
		switch (code)
		{
		case -1:
			return Qt::BlankCursor;
		case 0:
			return Qt::ArrowCursor;
		case 1:
			return Qt::PointingHandCursor;
		case 2:
			return Qt::IBeamCursor;
		case 3:
			return Qt::CrossCursor;
		case 4:
			return Qt::WaitCursor;
		case 5:
			return Qt::UpArrowCursor;
		case 6:
			return Qt::SizeFDiagCursor;
		case 7:
			return Qt::SizeBDiagCursor;
		case 8:
			return Qt::SizeHorCursor;
		case 9:
			return Qt::SizeVerCursor;
		case 10:
			return Qt::SizeAllCursor;
		case 11:
			return Qt::ForbiddenCursor;
		case 12:
			return Qt::WhatsThisCursor;
		default:
			return Qt::ArrowCursor;
		}
	}

	QString formatHotspotTooltip(const QString &tooltip)
	{
		const qsizetype split = tooltip.indexOf(QLatin1Char('\t'));
		if (split < 0)
			return tooltip;

		const QString title  = tooltip.left(split);
		const QString body   = tooltip.mid(split + 1);
		auto          toHtml = [](const QString &text)
		{ return text.toHtmlEscaped().replace(QLatin1Char('\n'), QLatin1String("<br>")); };

		if (title.isEmpty())
			return toHtml(body);
		if (body.isEmpty())
			return QStringLiteral("<b>%1</b>").arg(toHtml(title));
		return QStringLiteral("<b>%1</b><br>%2").arg(toHtml(title), toHtml(body));
	}

	bool hasScaledAbsoluteRect(const MiniWindow *window)
	{
		if (!window)
			return false;
		if ((window->flags & kMiniWindowAbsoluteLocation) == 0)
			return false;
		return window->rect.width() != window->width || window->rect.height() != window->height;
	}

	QPoint miniWindowDisplayToContent(const MiniWindow *window, const QPoint &displayPoint)
	{
		if (!hasScaledAbsoluteRect(window))
			return displayPoint;

		const int displayWidth  = qMax(1, window->rect.width());
		const int displayHeight = qMax(1, window->rect.height());
		const int contentWidth  = qMax(1, window->width);
		const int contentHeight = qMax(1, window->height);

		const int x = qBound(0, (displayPoint.x() * contentWidth) / displayWidth, contentWidth - 1);
		const int y = qBound(0, (displayPoint.y() * contentHeight) / displayHeight, contentHeight - 1);
		return {x, y};
	}

	QPoint miniWindowContentToDisplay(const MiniWindow *window, const QPoint &contentPoint)
	{
		if (!hasScaledAbsoluteRect(window))
			return contentPoint;

		const int displayWidth  = qMax(0, window->rect.width());
		const int displayHeight = qMax(0, window->rect.height());
		const int contentWidth  = qMax(1, window->width);
		const int contentHeight = qMax(1, window->height);

		const int x = qBound(0,
		                     qRound(static_cast<double>(contentPoint.x()) *
		                            static_cast<double>(displayWidth) / static_cast<double>(contentWidth)),
		                     displayWidth);
		const int y = qBound(0,
		                     qRound(static_cast<double>(contentPoint.y()) *
		                            static_cast<double>(displayHeight) / static_cast<double>(contentHeight)),
		                     displayHeight);
		return {x, y};
	}

	QRect positionImageRect(const QSize &imageSize, const QSize &clientSize, const QSize &ownerSize,
	                        int position)
	{
		const int w            = imageSize.width();
		const int h            = imageSize.height();
		const int clientWidth  = clientSize.width();
		const int clientHeight = clientSize.height();
		const int ownerWidth   = ownerSize.width();
		const int ownerHeight  = ownerSize.height();

		switch (position)
		{
		case 0:
			return {0, 0, clientWidth, clientHeight};
		case 1:
			if (h > 0)
			{
				const double ratio = static_cast<double>(w) / static_cast<double>(h);
				return {0, 0, static_cast<int>(clientHeight * ratio), clientHeight};
			}
			return {0, 0, clientWidth, clientHeight};
		case 2:
			return {0, 0, ownerWidth, ownerHeight};
		case 3:
			if (h > 0)
			{
				const double ratio = static_cast<double>(w) / static_cast<double>(h);
				return {0, 0, static_cast<int>(ownerHeight * ratio), ownerHeight};
			}
			return {0, 0, ownerWidth, ownerHeight};
		case 4:
			return {0, 0, w, h};
		case 5:
			return {(clientWidth - w) / 2, 0, w, h};
		case 6:
			return {clientWidth - w, 0, w, h};
		case 7:
			return {clientWidth - w, (clientHeight - h) / 2, w, h};
		case 8:
			return {clientWidth - w, clientHeight - h, w, h};
		case 9:
			return {(clientWidth - w) / 2, clientHeight - h, w, h};
		case 10:
			return {0, clientHeight - h, w, h};
		case 11:
			return {0, (clientHeight - h) / 2, w, h};
		case 12:
			return {(clientWidth - w) / 2, (clientHeight - h) / 2, w, h};
		default:
			break;
		}
		return {0, 0, w, h};
	}
} // namespace

class WrapTextBrowser : public QTextBrowser
{
	public:
		explicit WrapTextBrowser(WorldView *view, QWidget *parent = nullptr, bool isLive = false)
		    : QTextBrowser(parent), m_view(view), m_isLive(isLive)
		{
		}

		void setViewportMarginsPublic(int left, int top, int right, int bottom)
		{
			setViewportMargins(left, top, right, bottom);
		}

	protected:
		void mouseMoveEvent(QMouseEvent *event) override
		{
			if (m_view && m_view->handleMiniWindowMouseMove(event, viewport()))
			{
				event->accept();
				return;
			}
			QTextBrowser::mouseMoveEvent(event);
		}

		void mousePressEvent(QMouseEvent *event) override
		{
			if (m_view && m_view->handleMiniWindowMousePress(event, false, viewport()))
			{
				event->accept();
				return;
			}
			QTextBrowser::mousePressEvent(event);
		}

		void mouseReleaseEvent(QMouseEvent *event) override
		{
			if (m_view && m_view->handleMiniWindowMouseRelease(event, viewport()))
			{
				event->accept();
				return;
			}
			QTextBrowser::mouseReleaseEvent(event);
		}

		void mouseDoubleClickEvent(QMouseEvent *event) override
		{
			if (m_view && m_view->handleMiniWindowMousePress(event, true, viewport()))
			{
				event->accept();
				return;
			}
			QTextBrowser::mouseDoubleClickEvent(event);
		}

		void wheelEvent(QWheelEvent *event) override
		{
			if (m_view && m_view->handleMiniWindowWheel(event, viewport()))
			{
				event->accept();
				return;
			}
			if (m_isLive)
			{
				if (m_view)
					m_view->handleOutputWheel(event->angleDelta(), event->pixelDelta());
				event->accept();
				return;
			}
			if (m_view)
				m_view->noteUserScrollAction();
			QTextBrowser::wheelEvent(event);
		}

		void keyPressEvent(QKeyEvent *event) override
		{
			if (m_view && m_view->handleWorldHotkey(event))
				return;
			if (m_isLive)
			{
				event->accept();
				return;
			}
			if (m_view)
			{
				switch (event->key())
				{
				case Qt::Key_PageUp:
				case Qt::Key_PageDown:
				case Qt::Key_Up:
				case Qt::Key_Down:
				case Qt::Key_Home:
				case Qt::Key_End:
					m_view->noteUserScrollAction();
					break;
				default:
					break;
				}
			}
			QTextBrowser::keyPressEvent(event);
		}

		void contextMenuEvent(QContextMenuEvent *event) override
		{
			if (m_view)
			{
				const QPoint local = m_view->mapEventToOutputStack(QPointF(event->pos()), viewport());
				if (m_view->m_outputStack && m_view->m_outputStack->rect().contains(local))
				{
					QString hotspotId;
					QString windowName;
					if (m_view->hitTestMiniWindow(local, hotspotId, windowName, true))
						return;
				}
				if (m_view->showWorldContextMenuAtGlobalPos(event->globalPos()))
					return;
			}
			QTextBrowser::contextMenuEvent(event);
		}

	private:
		WorldView *m_view{nullptr};
		bool       m_isLive{false};
};

class InputTextEdit : public QPlainTextEdit
{
	public:
		explicit InputTextEdit(WorldView *view, QWidget *parent = nullptr)
		    : QPlainTextEdit(parent), m_view(view)
		{
		}

		void setViewportMarginsPublic(int left, int top, int right, int bottom)
		{
			setViewportMargins(left, top, right, bottom);
		}

	protected:
		void keyPressEvent(QKeyEvent *event) override;
		void contextMenuEvent(QContextMenuEvent *event) override;

	private:
		WorldView *m_view{nullptr};
};

class MiniWindowLayer : public QWidget
{
	public:
		explicit MiniWindowLayer(WorldView *view, bool underneath, QWidget *parent = nullptr)
		    : QWidget(parent), m_view(view), m_underneath(underneath)
		{
			setAttribute(Qt::WA_TransparentForMouseEvents);
			setAutoFillBackground(false);
		}

	protected:
		void paintEvent(QPaintEvent *event) override
		{
			Q_UNUSED(event);
			if (!m_view)
				return;
			QPainter painter(this);
			painter.setRenderHint(QPainter::SmoothPixmapTransform);
			m_view->paintMiniWindows(&painter, m_underneath);
		}

	private:
		WorldView *m_view{nullptr};
		bool       m_underneath{false};
};

struct WorldView::OutputFindState
{
		QStringList              history;
		QString                  title{QStringLiteral("Find in output buffer...")};
		QString                  lastFindText;
		bool                     matchCase{false};
		bool                     forwards{false};
		bool                     regexp{false};
		bool                     again{false};
		bool                     repeatOnSameLine{true};
		int                      startColumn{-1};
		int                      endColumn{-1};
		int                      currentLine{0};
		QVector<QPair<int, int>> matchesOnLine;
		QRegularExpression       regex;
};

WorldView::WorldView(QWidget *parent) : QWidget(parent)
{
	auto *layout   = new QVBoxLayout(this);
	auto *splitter = new QSplitter(Qt::Vertical, this);
	m_splitter     = splitter;

	m_outputContainer = new QWidget(splitter);
	m_tooltipTimer    = new QTimer(this);
	m_tooltipTimer->setSingleShot(true);
	connect(m_tooltipTimer, &QTimer::timeout, this, &WorldView::showScheduledHotspotTooltip);
	m_fadeTimer = new QTimer(this);
	m_fadeTimer->setSingleShot(false);
	connect(m_fadeTimer, &QTimer::timeout, this,
	        [this]
	        {
		        if (!m_runtime || m_frozen || m_fadeOutputBufferAfterSeconds <= 0 || m_fadeOutputSeconds <= 0)
			        return;
		        if (!fadeRebuildNeededNow())
			        return;
		        rebuildOutputFromLines(m_runtime->lines());
	        });
	m_timeFadeCancelled = QDateTime::currentDateTime();
	auto *outputLayout  = new QHBoxLayout(m_outputContainer);
	outputLayout->setContentsMargins(0, 0, 0, 0);
	outputLayout->setSpacing(0);

	m_outputStack           = new QWidget(m_outputContainer);
	auto *outputStackLayout = new QGridLayout(m_outputStack);
	outputStackLayout->setContentsMargins(0, 0, 0, 0);
	outputStackLayout->setSpacing(0);

	m_outputSplitter = new QSplitter(Qt::Vertical, m_outputStack);
	m_outputSplitter->setChildrenCollapsible(false);

	m_output = new WrapTextBrowser(this, m_outputSplitter);
	m_output->setReadOnly(true);
	m_output->setOpenLinks(false);
	m_output->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_output->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	m_liveOutput = new WrapTextBrowser(this, m_outputSplitter, true);
	m_liveOutput->setReadOnly(true);
	m_liveOutput->setOpenLinks(false);
	m_liveOutput->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_liveOutput->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_liveOutput->setVisible(false);

	m_miniUnderlay = new MiniWindowLayer(this, true, m_outputStack);
	m_miniOverlay  = new MiniWindowLayer(this, false, m_outputStack);

	m_outputDocument = new QTextDocument(this);
	m_outputDocument->setUndoRedoEnabled(false);
	m_output->setDocument(m_outputDocument);
	m_liveOutput->setDocument(m_outputDocument);
	m_defaultOutputFont = m_output->font();
	setMouseTracking(true);
	m_output->setMouseTracking(true);
	m_output->installEventFilter(this);
	if (m_output->viewport())
	{
		m_output->viewport()->setMouseTracking(true);
		m_output->viewport()->installEventFilter(this);
	}
	if (m_liveOutput)
	{
		m_liveOutput->setMouseTracking(true);
		m_liveOutput->installEventFilter(this);
		if (m_liveOutput->viewport())
		{
			m_liveOutput->viewport()->setMouseTracking(true);
			m_liveOutput->viewport()->installEventFilter(this);
		}
	}
	connect(m_outputDocument, &QTextDocument::contentsChanged, this,
	        [this]
	        {
		        if (m_scrollbackSplitActive && !m_frozen)
			        scrollViewToEnd(m_liveOutput);
	        });

	m_outputSplitter->addWidget(m_output);
	m_outputSplitter->addWidget(m_liveOutput);
	m_outputSplitter->setStretchFactor(0, 1);
	m_outputSplitter->setStretchFactor(1, 0);

	m_outputScrollBar = new QScrollBar(Qt::Vertical, m_outputContainer);
	outputStackLayout->addWidget(m_miniUnderlay, 0, 0);
	outputStackLayout->addWidget(m_outputSplitter, 0, 0);
	outputStackLayout->addWidget(m_miniOverlay, 0, 0);
	m_miniUnderlay->lower();
	m_miniOverlay->raise();

	outputLayout->addWidget(m_outputStack, 1);
	outputLayout->addWidget(m_outputScrollBar);
	m_outputContainer->setMouseTracking(true);
	m_outputStack->setMouseTracking(true);
	m_outputSplitter->setMouseTracking(true);
	m_outputScrollBar->setMouseTracking(true);
	m_outputContainer->installEventFilter(this);
	m_outputStack->installEventFilter(this);
	m_outputSplitter->installEventFilter(this);
	m_outputScrollBar->installEventFilter(this);

	m_input = new InputTextEdit(this, splitter);
	m_input->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_input->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	m_defaultInputFont = m_input->font();

	splitter->addWidget(m_outputContainer);
	splitter->addWidget(m_input);
	splitter->setStretchFactor(0, 1);
	splitter->setStretchFactor(1, 0);
	applyDefaultInputHeight(true);

	layout->addWidget(splitter);

	connect(m_input, &QPlainTextEdit::textChanged, this,
	        [this]
	        {
		        if (m_settingText)
			        return;
		        m_inputChanged = true;
		        m_partialCommand.clear();
		        m_partialIndex = -1;
		        m_historyIndex = -1;
		        updateInputHeight();
		        if (m_runtime && !m_notifyingPluginCommandChanged)
		        {
			        m_notifyingPluginCommandChanged = true;
			        m_runtime->firePluginCommandChanged();
			        m_notifyingPluginCommandChanged = false;
		        }
	        });

	connect(m_output, &QTextBrowser::selectionChanged, this, [this] { handleOutputSelectionChanged(); });
	if (m_liveOutput)
	{
		connect(m_liveOutput, &QTextBrowser::selectionChanged, this,
		        [this] { handleOutputSelectionChanged(); });
	}
	if (m_output->verticalScrollBar())
	{
		QScrollBar *bar = m_output->verticalScrollBar();
		if (m_outputScrollBar)
		{
			auto syncExternal = [this, bar]
			{
				if (!m_outputScrollBar)
					return;
				QSignalBlocker block(m_outputScrollBar);
				m_outputScrollBar->setRange(bar->minimum(), bar->maximum());
				m_outputScrollBar->setPageStep(bar->pageStep());
				m_outputScrollBar->setSingleStep(bar->singleStep());
				m_outputScrollBar->setValue(bar->value());
			};
			connect(bar, &QScrollBar::rangeChanged, this,
			        [this, bar](int min, int max)
			        {
				        if (!m_outputScrollBar)
					        return;
				        QSignalBlocker block(m_outputScrollBar);
				        m_outputScrollBar->setRange(min, max);
				        m_outputScrollBar->setPageStep(bar->pageStep());
				        m_outputScrollBar->setSingleStep(bar->singleStep());
			        });
			connect(bar, &QScrollBar::valueChanged, this,
			        [this](int value)
			        {
				        if (!m_outputScrollBar)
					        return;
				        QSignalBlocker block(m_outputScrollBar);
				        m_outputScrollBar->setValue(value);
			        });
			connect(m_outputScrollBar, &QScrollBar::valueChanged, this,
			        [bar](int value)
			        {
				        if (!bar)
					        return;
				        bar->setValue(value);
			        });
			connect(m_outputScrollBar, &QScrollBar::sliderPressed, this, [this] { noteUserScrollAction(); });
			connect(m_outputScrollBar, &QScrollBar::sliderMoved, this,
			        [this](int) { noteUserScrollAction(); });
			connect(m_outputScrollBar, &QScrollBar::actionTriggered, this,
			        [this](int) { noteUserScrollAction(); });
			syncExternal();
		}
		connect(bar, &QScrollBar::valueChanged, this, [this](int) { emit outputScrollChanged(); });
	}
	connect(m_input, &QPlainTextEdit::selectionChanged, this, [this] { emit inputSelectionChanged(); });
	connect(this, &WorldView::outputScrollChanged, this, [this] { requestDrawOutputWindowNotification(); });

	connect(m_output, &QTextBrowser::anchorClicked, this,
	        [this](const QUrl &url) { emit hyperlinkActivated(url.toString()); });
	connect(m_output, &QTextBrowser::highlighted, this,
	        [this](const QUrl &) { refreshHoveredHyperlinkFromCursor(); });

	if (m_liveOutput)
	{
		connect(m_liveOutput, &QTextBrowser::anchorClicked, this,
		        [this](const QUrl &url) { emit hyperlinkActivated(url.toString()); });

		connect(m_liveOutput, &QTextBrowser::highlighted, this,
		        [this](const QUrl &) { refreshHoveredHyperlinkFromCursor(); });
	}

	if (m_outputSplitter)
	{
		connect(m_outputSplitter, &QSplitter::splitterMoved, this,
		        [this](int, int)
		        {
			        if (!m_scrollbackSplitActive || !m_outputSplitter)
				        return;
			        if (const QList<int> sizes = m_outputSplitter->sizes(); sizes.size() >= 2)
				        m_lastLiveSplitSize = sizes.at(1);
			        if (m_scrollbackSplitActive)
				        scrollViewToEnd(m_liveOutput);
		        });
	}
}

WorldView::~WorldView() = default;

void WorldView::setWorldName(const QString &name)
{
	setWindowTitle(name);
}

void WorldView::setRuntime(WorldRuntime *runtime)
{
	if (m_runtime && m_runtime->view() == this)
		m_runtime->setView(nullptr);
	m_runtime = runtime;
	applyRuntimeSettings();
	if (m_runtime)
		m_runtime->setView(this);
}

void WorldView::setRuntimeObserver(WorldRuntime *runtime)
{
	m_runtime = runtime;
	applyRuntimeSettings();
}

void WorldView::refreshMiniWindows() const
{
	if (m_miniUnderlay)
		m_miniUnderlay->update();
	if (m_miniOverlay)
		m_miniOverlay->update();
}

QPoint WorldView::miniWindowGlobalPosition(const MiniWindow *window, int x, int y) const
{
	if (!window)
		return mapToGlobal(QPoint(0, 0));
	const QPoint local = window->rect.topLeft() + miniWindowContentToDisplay(window, QPoint(x, y));
	if (m_outputStack)
		return m_outputStack->mapToGlobal(local);
	return mapToGlobal(local);
}

bool WorldView::showWorldContextMenuAtGlobalPos(const QPoint &globalPos)
{
	WrapTextBrowser *source = nullptr;
	if (m_output && m_output->viewport() &&
	    m_output->viewport()->rect().contains(m_output->viewport()->mapFromGlobal(globalPos)))
		source = m_output;
	else if (m_liveOutput && m_liveOutput->viewport() &&
	         m_liveOutput->viewport()->rect().contains(m_liveOutput->viewport()->mapFromGlobal(globalPos)))
		source = m_liveOutput;
	else
		source = m_output ? m_output : m_liveOutput;

	if (!source)
		return false;

	const QPoint sourcePos = source->viewport()->mapFromGlobal(globalPos);
	QMenu       *menu      = source->createStandardContextMenu(sourcePos);
	if (!menu)
		return false;

	menu->addSeparator();
	if (QAction *copyHtml = menu->addAction(QStringLiteral("Copy as HTML")); copyHtml)
	{
		copyHtml->setEnabled(hasOutputSelection());
		connect(copyHtml, &QAction::triggered, this, [this] { copySelectionAsHtml(); });
	}

	forceOpaqueMenu(menu);
	ContextMenuDismissReplayFilter replayFilter(menu);
	qApp->installEventFilter(&replayFilter);
	QAction *const selected = menu->exec(globalPos);
	qApp->removeEventFilter(&replayFilter);
	delete menu;

	if (!selected && replayFilter.hasReplayPoint())
	{
		const QPoint        replayPos = replayFilter.replayPoint();
		QPointer<WorldView> view      = this;
		QTimer::singleShot(0, qApp,
		                   [view, replayPos]
		                   {
			                   if (view)
				                   view->showContextMenuAtGlobalPos(replayPos);
		                   });
	}

	return true;
}

bool WorldView::showContextMenuAtGlobalPos(const QPoint &globalPos)
{
	if (!m_outputStack)
		return false;

	const QPoint local = mapEventToOutputStack(globalPos);
	if (m_outputStack->rect().contains(local))
	{
		if (QString hotspotId, windowName; hitTestMiniWindow(local, hotspotId, windowName, true))
			return replayMiniWindowRightClickAtGlobalPos(globalPos);
	}

	return showWorldContextMenuAtGlobalPos(globalPos);
}

bool WorldView::replayMiniWindowRightClickAtGlobalPos(const QPoint &globalPos)
{
	if (!m_runtime || !m_outputStack)
		return false;

	const QPoint local = mapEventToOutputStack(globalPos);
	if (!m_outputStack->rect().contains(local))
		return false;

	if (QString hotspotId, windowName; hitTestMiniWindow(local, hotspotId, windowName, true))
	{
		const Qt::KeyboardModifiers mods = QGuiApplication::keyboardModifiers();
		QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(local), QPointF(globalPos),
		                  Qt::RightButton, Qt::RightButton, mods);
		if (!handleMiniWindowMousePress(&press, false, m_outputStack))
			return false;

		QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), QPointF(local), QPointF(globalPos),
		                    Qt::RightButton, Qt::NoButton, mods);
		handleMiniWindowMouseRelease(&release, m_outputStack);
		return true;
	}
	return showWorldContextMenuAtGlobalPos(globalPos);
}

bool WorldView::isMiniWindowCaptureActive() const
{
	return !m_capturedWindowName.isEmpty();
}

bool WorldView::hasLastMousePosition() const
{
	return m_hasLastMousePos;
}

QPoint WorldView::lastMousePosition() const
{
	return m_lastMousePos;
}

void WorldView::recheckMiniWindowHover()
{
	if (!m_hasLastMousePos || !m_capturedWindowName.isEmpty())
		return;

	QMouseEvent event(QEvent::MouseMove, QPointF(m_lastMousePos), QPointF(m_lastMousePos),
	                  QPointF(mapToGlobal(m_lastMousePos)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
	handleMiniWindowMouseMove(&event, m_outputStack ? m_outputStack : this);
}

bool WorldView::isScrollbackSplitActive() const
{
	return m_scrollbackSplitActive;
}

void WorldView::collapseScrollbackSplitToLiveOutput()
{
	if (!m_scrollbackSplitActive)
		return;
	setScrollbackSplitActive(false);
	// Return to current output immediately when collapsing split view.
	scrollOutputToEnd();
}

WorldRuntime *WorldView::runtime() const
{
	return m_runtime;
}

void WorldView::setNoEcho(bool enabled)
{
	if (m_noEchoOff)
		m_noEcho = false;
	else
		m_noEcho = enabled;
}

void WorldView::setFrozen(bool frozen)
{
	if (m_frozen == frozen)
		return;
	m_frozen = frozen;
	if (m_frozen)
		m_timeFadeCancelled = QDateTime::currentDateTime();
	if (m_runtime)
		m_runtime->setOutputFrozen(m_frozen);
	emit freezeStateChanged(m_frozen);
	if (!m_frozen && !m_pendingOutput.isEmpty())
	{
		m_flushingPending                  = true;
		const QVector<PendingHtml> pending = m_pendingOutput;
		m_pendingOutput.clear();
		for (const PendingHtml &entry : pending)
			appendOutputHtml(entry.html, entry.newLine);
		m_flushingPending = false;
	}
}

bool WorldView::isFrozen() const
{
	return m_frozen;
}

void WorldView::noteUserScrollAction()
{
	m_timeFadeCancelled = QDateTime::currentDateTime();
	if (!m_autoPause || !m_outputScrollBar)
		return;
	const bool atEnd = isAtBufferEnd();
	setScrollbackSplitActive(!atEnd);
}

void WorldView::handleOutputWheel(const QPoint &angleDelta, const QPoint &pixelDelta)
{
	if (!m_outputScrollBar)
		return;
	int  delta = 0;
	bool pixel = false;
	if (!pixelDelta.isNull())
	{
		delta = pixelDelta.y();
		pixel = true;
	}
	else if (!angleDelta.isNull())
	{
		delta = angleDelta.y();
	}
	if (delta == 0)
		return;
	int value = m_outputScrollBar->value();
	if (pixel)
	{
		value -= delta;
	}
	else
	{
		int steps = delta / 120;
		if (steps == 0)
			steps = (delta > 0) ? 1 : -1;
		value -= steps * m_outputScrollBar->singleStep();
	}
	m_outputScrollBar->setValue(value);
	noteUserScrollAction();
}

void WorldView::setScrollbackSplitActive(bool active)
{
	if (m_scrollbackSplitActive == active)
		return;
	m_scrollbackSplitActive = active;
	if (!m_outputSplitter || !m_liveOutput)
		return;

	if (m_scrollbackSplitActive)
	{
		m_liveOutput->setVisible(true);
		const int total    = m_outputSplitter->size().height();
		int       liveSize = m_lastLiveSplitSize;
		if (liveSize <= 0 && total > 0)
			liveSize = qMax(1, total / 4);
		if (total > 0)
		{
			const int topSize = qMax(1, total - liveSize);
			m_outputSplitter->setSizes(QList<int>() << topSize << liveSize);
		}
		if (!m_frozen)
			scrollViewToEnd(m_liveOutput);
	}
	else
	{
		if (const QList<int> sizes = m_outputSplitter->sizes(); sizes.size() >= 2)
			m_lastLiveSplitSize = sizes.at(1);
		m_liveOutput->setVisible(false);
		m_outputSplitter->setSizes(QList<int>() << 1 << 0);
		m_userScrollAction = false;
		if (!m_frozen)
			scrollViewToEnd(m_output);
	}
}

void WorldView::scrollViewToEnd(const WrapTextBrowser *view)
{
	if (!view)
		return;
	QScrollBar *const bar = view->verticalScrollBar();
	if (!bar)
		return;
	bar->setValue(bar->maximum());
}

void WorldView::requestOutputScrollToEnd()
{
	if (m_frozen)
		return;
	if (m_scrollToEndQueued)
		return;
	m_scrollToEndQueued = true;
	QPointer<WorldView> that(this);
	QTimer::singleShot(0, this,
	                   [that]
	                   {
		                   if (!that)
			                   return;
		                   that->m_scrollToEndQueued = false;
		                   if (that->m_frozen)
			                   return;
		                   if (that->m_scrollbackSplitActive)
			                   scrollViewToEnd(that->m_liveOutput);
		                   else
			                   scrollViewToEnd(that->m_output);
	                   });
}

WrapTextBrowser *WorldView::activeOutputView() const
{
	if (m_scrollbackSplitActive && m_liveOutput && m_liveOutput->isVisible())
		return m_liveOutput;
	return m_output;
}

void WorldView::scrollOutputToStart() const
{
	WrapTextBrowser *const view = activeOutputView();
	if (!view)
		return;
	if (QScrollBar *bar = view->verticalScrollBar())
		bar->setValue(bar->minimum());
}

void WorldView::scrollOutputToEnd() const
{
	scrollViewToEnd(activeOutputView());
}

void WorldView::scrollOutputPageUp() const
{
	WrapTextBrowser *const view = activeOutputView();
	if (!view)
		return;
	if (QScrollBar *bar = view->verticalScrollBar())
		bar->setValue(bar->value() - bar->pageStep());
}

void WorldView::scrollOutputPageDown() const
{
	WrapTextBrowser *const view = activeOutputView();
	if (!view)
		return;
	if (QScrollBar *bar = view->verticalScrollBar())
		bar->setValue(bar->value() + bar->pageStep());
}

void WorldView::scrollOutputLineUp() const
{
	WrapTextBrowser *const view = activeOutputView();
	if (!view)
		return;
	if (QScrollBar *bar = view->verticalScrollBar())
		bar->setValue(bar->value() - bar->singleStep());
}

void WorldView::scrollOutputLineDown() const
{
	WrapTextBrowser *const view = activeOutputView();
	if (!view)
		return;
	if (QScrollBar *bar = view->verticalScrollBar())
		bar->setValue(bar->value() + bar->singleStep());
}
void WorldView::addHyperlinkToHistory(const QString &text)
{
	if (!m_hyperlinkAddsToCommandHistory)
		return;
	addToHistory(text);
}

void WorldView::appendOutputText(const QString &text, bool newLine)
{
	appendOutputTextInternal(text, newLine, true, WorldRuntime::LineOutput);
}

void WorldView::appendOutputTextStyled(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans,
                                       bool newLine)
{
	appendOutputTextInternal(text, newLine, true, WorldRuntime::LineOutput, spans);
}

void WorldView::appendNoteText(const QString &text, bool newLine)
{
	if (!m_runtime || text.isEmpty())
	{
		appendOutputTextInternal(text, newLine, true, WorldRuntime::LineNote);
		return;
	}

	const unsigned short    noteStyle = m_runtime->noteStyle();
	const long              foreValue = m_runtime->noteColourFore();
	const long              backValue = m_runtime->noteColourBack();
	WorldRuntime::StyleSpan span;
	span.length    = sizeToInt(text.size());
	span.fore      = QColor(static_cast<int>(foreValue & 0xFF), static_cast<int>((foreValue >> 8) & 0xFF),
	                        static_cast<int>((foreValue >> 16) & 0xFF));
	span.back      = QColor(static_cast<int>(backValue & 0xFF), static_cast<int>((backValue >> 8) & 0xFF),
	                        static_cast<int>((backValue >> 16) & 0xFF));
	span.bold      = (noteStyle & 0x0001) != 0;
	span.underline = (noteStyle & 0x0002) != 0;
	span.blink     = (noteStyle & 0x0004) != 0;
	span.inverse   = (noteStyle & 0x0008) != 0;
	span.changed   = true;
	appendOutputTextInternal(text, newLine, true, WorldRuntime::LineNote, {span});
}

void WorldView::appendNoteTextStyled(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans,
                                     bool newLine)
{
	if (!m_runtime || spans.isEmpty())
	{
		appendNoteText(text, newLine);
		return;
	}

	QVector<WorldRuntime::StyleSpan> normalized    = spans;
	const long                       noteForeValue = m_runtime->noteColourFore();
	const long                       noteBackValue = m_runtime->noteColourBack();
	const QColor                     noteFore(static_cast<int>(noteForeValue & 0xFF),
	                                          static_cast<int>((noteForeValue >> 8) & 0xFF),
	                                          static_cast<int>((noteForeValue >> 16) & 0xFF));
	const QColor                     noteBack(static_cast<int>(noteBackValue & 0xFF),
	                                          static_cast<int>((noteBackValue >> 8) & 0xFF),
	                                          static_cast<int>((noteBackValue >> 16) & 0xFF));
	for (WorldRuntime::StyleSpan &span : normalized)
	{
		if (!span.fore.isValid())
			span.fore = noteFore;
		if (!span.back.isValid())
			span.back = noteBack;
	}
	appendOutputTextInternal(text, newLine, true, WorldRuntime::LineNote, normalized);
}

QString WorldView::pasteCommand(const QString &text)
{
	if (!m_input)
		return {};
	QTextCursor   cursor  = m_input->textCursor();
	const int     start   = cursor.selectionStart();
	const int     end     = cursor.selectionEnd();
	const QString current = m_input->toPlainText();
	QString       selected;
	if (end > start)
		selected = current.mid(start, end - start);
	cursor.insertText(text);
	m_input->setTextCursor(cursor);
	m_inputChanged = true;
	return selected;
}

QString WorldView::pushCommand()
{
	if (!m_input)
		return {};
	const QString command     = m_input->toPlainText();
	const bool    savedNoEcho = m_noEcho;
	m_noEcho                  = false;
	addToHistory(command);
	m_noEcho = savedNoEcho;

	m_settingText = true;
	m_input->clear();
	m_settingText  = false;
	m_inputChanged = false;
	resetHistoryRecall();
	return command;
}

namespace
{
	void forceOpaqueMenu(QMenu *menu)
	{
		if (!menu)
			return;

		const QPalette appPalette      = QApplication::palette();
		QColor         base            = appPalette.color(QPalette::Base);
		QColor         text            = appPalette.color(QPalette::Text);
		QColor         highlight       = appPalette.color(QPalette::Highlight);
		QColor         highlightedText = appPalette.color(QPalette::HighlightedText);

		if (!base.isValid())
			base = QColor(40, 40, 40);
		if (!text.isValid())
			text = QColor(230, 230, 230);
		if (!highlight.isValid())
			highlight = QColor(60, 120, 200);
		if (!highlightedText.isValid())
			highlightedText = QColor(255, 255, 255);

		base.setAlpha(255);
		text.setAlpha(255);
		highlight.setAlpha(255);
		highlightedText.setAlpha(255);

		QPalette palette = menu->palette();
		palette.setColor(QPalette::Window, base);
		palette.setColor(QPalette::Base, base);
		palette.setColor(QPalette::Text, text);
		palette.setColor(QPalette::WindowText, text);
		menu->setPalette(palette);
		menu->setWindowOpacity(1.0);
		menu->setAttribute(Qt::WA_TranslucentBackground, false);
		// Allow right-clicks that dismiss one menu to be replayed so a new context
		// menu can open immediately at the clicked location (expected behavior).
		menu->setAttribute(Qt::WA_NoMouseReplay, false);
		menu->setStyleSheet(QStringLiteral("QMenu { background-color: %1; color: %2; } "
		                                   "QMenu::item:selected { background-color: %3; color: %4; }")
		                        .arg(base.name(), text.name(), highlight.name(), highlightedText.name()));
	}

	QString escapeWithBreaks(const QString &input)
	{
		QString escaped;
		escaped.reserve(input.size() * 2);
		for (const QChar ch : input)
		{
			if (ch == QLatin1Char('\r'))
				continue;
			if (ch == QLatin1Char('\n'))
			{
				escaped += QStringLiteral("<br/>");
				continue;
			}
			if (ch == QLatin1Char('\t'))
			{
				escaped += QStringLiteral("&nbsp;&nbsp;&nbsp;&nbsp;");
				continue;
			}
			if (ch == QLatin1Char(' '))
			{
				escaped += QStringLiteral("&nbsp;");
				continue;
			}
			if (ch == QLatin1Char('<'))
				escaped += QStringLiteral("&lt;");
			else if (ch == QLatin1Char('>'))
				escaped += QStringLiteral("&gt;");
			else if (ch == QLatin1Char('&'))
				escaped += QStringLiteral("&amp;");
			else if (ch == QLatin1Char('\"'))
				escaped += QStringLiteral("&quot;");
			else
				escaped += ch;
		}
		return escaped;
	}

	QString buildOutputHtml(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans, bool showBold,
	                        bool showItalic, bool showUnderline, bool alternativeInverse, int lineSpacing,
	                        double opacity)
	{
		if (spans.isEmpty())
		{
			const QString escaped = escapeWithBreaks(text);
			return QStringLiteral("<span style=\"white-space: pre-wrap;\">%1</span>").arg(escaped);
		}

		QString html;
		int     offset = 0;
		for (const WorldRuntime::StyleSpan &span : spans)
		{
			const int     length = qMax(0, span.length);
			const QString chunk  = text.mid(offset, length);
			offset += length;
			if (chunk.isEmpty())
				continue;

			auto style = QStringLiteral("white-space: pre-wrap;");
			if (lineSpacing > 0)
				style += QStringLiteral("line-height:%1%;").arg(100 + lineSpacing);
			if (opacity < 0.999)
				style +=
				    QStringLiteral("opacity:%1;").arg(QString::number(qBound(0.0, opacity, 1.0), 'f', 3));
			QColor fore = span.fore;
			QColor back = span.back;
			if (span.inverse)
			{
				qSwap(fore, back);
				if (alternativeInverse && span.bold)
					qSwap(fore, back);
			}
			if (fore.isValid())
				style += QStringLiteral("color:%1;").arg(fore.name());
			if (back.isValid())
				style += QStringLiteral("background-color:%1;").arg(back.name());
			if (span.bold && showBold)
				style += QStringLiteral("font-weight:bold;");
			if (span.italic && showItalic)
				style += QStringLiteral("font-style:italic;");
			if ((span.underline && showUnderline) || span.strike)
			{
				QStringList decorations;
				if (span.underline && showUnderline)
					decorations << QStringLiteral("underline");
				if (span.strike)
					decorations << QStringLiteral("line-through");
				style += QStringLiteral("text-decoration:%1;").arg(decorations.join(' '));
			}

			const QString escaped = escapeWithBreaks(chunk);
			QString       wrapped = QStringLiteral("<span style=\"%1\">%2</span>").arg(style, escaped);

			if (span.actionType == WorldRuntime::ActionHyperlink ||
			    span.actionType == WorldRuntime::ActionSend || span.actionType == WorldRuntime::ActionPrompt)
			{
				const QString href = span.action.toHtmlEscaped();
				if (!span.hint.isEmpty())
					wrapped = QStringLiteral("<a href=\"%1\" title=\"%2\">%3</a>")
					              .arg(href, span.hint.toHtmlEscaped(), wrapped);
				else
					wrapped = QStringLiteral("<a href=\"%1\">%2</a>").arg(href, wrapped);
			}

			html += wrapped;
		}

		if (offset < text.size())
		{
			const QString escaped = escapeWithBreaks(text.mid(offset));
			html += QStringLiteral("<span style=\"white-space: pre-wrap;\">%1</span>").arg(escaped);
		}

		return html;
	}

} // namespace

void WorldView::appendOutputHtml(const QString &html, bool newLine)
{
	if (!m_outputDocument)
		return;
	if (m_frozen && !m_flushingPending)
	{
		m_pendingOutput.push_back(PendingHtml{html, newLine});
		return;
	}
	QTextCursor cursor(m_outputDocument);
	cursor.movePosition(QTextCursor::End);
	if (m_output)
	{
		QTextCharFormat defaultFormat = cursor.charFormat();
		defaultFormat.setForeground(m_output->palette().color(QPalette::Text));
		const QColor defaultBack =
		    m_outputBackground.isValid() ? m_outputBackground : m_output->palette().color(QPalette::Base);
		defaultFormat.setBackground(defaultBack);
		cursor.setCharFormat(defaultFormat);
	}
	if (!html.isEmpty())
		cursor.insertHtml(html);
	if (newLine)
	{
		const QTextBlockFormat blockFormat = cursor.blockFormat();
		const QTextCharFormat  charFormat  = cursor.charFormat();
		cursor.insertBlock(blockFormat, charFormat);
	}
	if (m_bulkOutputRebuild)
		return;
	requestOutputScrollToEnd();
}

void WorldView::echoInputText(const QString &text)
{
	if (!m_displayMyInput || !m_output)
		return;
	QString trimmed = text;
	if (trimmed.endsWith(QStringLiteral("\r\n")))
		trimmed.chop(2);
	QVector<WorldRuntime::StyleSpan> echoSpans;
	if (m_runtime && !trimmed.isEmpty())
	{
		bool      ok         = false;
		const int echoColour = m_runtime->worldAttributes().value(QStringLiteral("echo_colour")).toInt(&ok);
		if (ok && echoColour > 0 && echoColour <= MAX_CUSTOM)
		{
			auto fromColorRef = [](long value)
			{
				return QColor(static_cast<int>(value & 0xFF), static_cast<int>((value >> 8) & 0xFF),
				              static_cast<int>((value >> 16) & 0xFF));
			};
			WorldRuntime::StyleSpan span;
			span.length = sizeToInt(trimmed.size());
			span.fore   = fromColorRef(m_runtime->customColourText(echoColour));
			span.back   = fromColorRef(m_runtime->customColourBackground(echoColour));
			echoSpans.push_back(span);
		}
	}
	if (m_keepCommandsOnSameLine)
	{
		trimTrailingPromptWhitespace(m_outputDocument);
		appendOutputTextInternal(trimmed, false, true, WorldRuntime::LineInput, echoSpans);
		m_breakBeforeNextServerOutput = true;
	}
	else
		appendOutputTextInternal(trimmed, true, true, WorldRuntime::LineInput, echoSpans);

	// Echoed command text is now committed output. Do not let subsequent partial
	// server updates replace this region.
	m_hasPartialOutput    = false;
	m_partialOutputStart  = 0;
	m_partialOutputLength = 0;
}

QStringList WorldView::outputLines() const
{
	if (!m_output)
		return {};
	return m_output->toPlainText().split(QLatin1Char('\n'));
}

bool WorldView::hasOutputSelection() const
{
	if (m_output && m_output->textCursor().hasSelection())
		return true;
	if (m_liveOutput && m_liveOutput->textCursor().hasSelection())
		return true;
	return false;
}

bool WorldView::hasInputSelection() const
{
	if (!m_input)
		return false;
	return m_input->textCursor().hasSelection();
}

QString WorldView::currentHoveredHyperlink() const
{
	if (const QString liveHref = anchorAtGlobalCursor(m_liveOutput); !liveHref.isEmpty())
		return liveHref;
	return anchorAtGlobalCursor(m_output);
}

void WorldView::applyHoveredHyperlink(const QString &href)
{
	const QString normalized = href.trimmed();
	if (normalized == m_hoveredHyperlinkHref)
		return;
	m_hoveredHyperlinkHref = normalized;
	emit hyperlinkHighlighted(m_hoveredHyperlinkHref);
	if (m_hoveredHyperlinkHref.isEmpty())
	{
		if (m_anchorHoverActive)
			QToolTip::hideText();
		m_anchorHoverActive = false;
	}
	else
	{
		m_anchorHoverActive = true;
	}
}

void WorldView::refreshHoveredHyperlinkFromCursor()
{
	applyHoveredHyperlink(currentHoveredHyperlink());
}

bool WorldView::hyperlinkHoverActive() const
{
	return !m_hoveredHyperlinkHref.isEmpty() || !currentHoveredHyperlink().isEmpty();
}

bool WorldView::isAtBufferEnd() const
{
	if (!m_output)
		return true;
	QScrollBar *const bar = m_output->verticalScrollBar();
	if (!bar)
		return true;
	return bar->value() >= bar->maximum();
}

void WorldView::selectOutputLine(int zeroBasedLine) const
{
	if (!m_output)
		return;

	QTextDocument *doc = m_output->document();
	if (!doc)
		return;

	if (zeroBasedLine < 0)
		zeroBasedLine = 0;

	const QTextBlock block = doc->findBlockByNumber(zeroBasedLine);
	if (!block.isValid())
		return;

	QTextCursor cursor(doc);
	cursor.setPosition(block.position());
	cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
	m_output->setTextCursor(cursor);
	m_output->ensureCursorVisible();
}

void WorldView::selectOutputRange(int zeroBasedLine, int startColumn, int endColumn) const
{
	if (!m_output)
		return;

	QTextDocument *doc = m_output->document();
	if (!doc)
		return;

	if (zeroBasedLine < 0)
		zeroBasedLine = 0;

	const QTextBlock block = doc->findBlockByNumber(zeroBasedLine);
	if (!block.isValid())
		return;

	const QString text     = block.text();
	const int     textSize = sizeToInt(text.size());
	startColumn            = qMax(0, startColumn);
	endColumn              = qMax(startColumn, endColumn);
	if (startColumn > textSize)
		startColumn = textSize;
	if (endColumn > textSize)
		endColumn = textSize;

	QTextCursor cursor(doc);
	cursor.setPosition(block.position() + startColumn);
	cursor.setPosition(block.position() + endColumn, QTextCursor::KeepAnchor);
	m_output->setTextCursor(cursor);
	m_output->ensureCursorVisible();
}

void WorldView::setOutputSelection(int startLine, int endLine, int startColumn, int endColumn) const
{
	if (!m_output)
		return;

	QTextDocument *doc = m_output->document();
	if (!doc)
		return;

	if (startLine < 1)
		startLine = 1;
	if (endLine < 1)
		endLine = 1;
	if (endLine < startLine)
		qSwap(endLine, startLine);

	const int blockCount = doc->blockCount();
	if (blockCount <= 0)
		return;
	if (startLine > blockCount)
		startLine = blockCount;
	if (endLine > blockCount)
		endLine = blockCount;

	const QTextBlock startBlock = doc->findBlockByNumber(startLine - 1);
	const QTextBlock endBlock   = doc->findBlockByNumber(endLine - 1);
	if (!startBlock.isValid() || !endBlock.isValid())
		return;

	startColumn = qMax(1, startColumn);
	endColumn   = qMax(1, endColumn);

	const int   startMax = sizeToInt(startBlock.text().size());
	const int   endMax   = sizeToInt(endBlock.text().size());
	const int   startPos = startBlock.position() + qMin(startColumn - 1, startMax);
	const int   endPos   = endBlock.position() + qMin(endColumn - 1, endMax);

	QTextCursor cursor(doc);
	cursor.setPosition(startPos);
	cursor.setPosition(endPos, QTextCursor::KeepAnchor);
	m_output->setTextCursor(cursor);
	m_output->ensureCursorVisible();
}

int WorldView::setOutputScroll(int position, bool visible)
{
	if (!m_output)
		return eBadParameter;
	m_outputScrollBarWanted = visible;

	if (m_outputScrollBar)
		m_outputScrollBar->setVisible(visible);

	QScrollBar *bar = m_output->verticalScrollBar();
	if (!bar)
		return eBadParameter;

	if (position != -2)
	{
		int target = position;
		if (position == -1)
			target = bar->maximum();
		if (target < bar->minimum())
			target = bar->minimum();
		if (target > bar->maximum())
			target = bar->maximum();
		if (m_outputScrollBar)
			m_outputScrollBar->setValue(target);
		else
			bar->setValue(target);
	}
	return eOK;
}

bool WorldView::doOutputFind(bool again)
{
	if (!m_runtime)
		return false;

	if (!m_outputFind)
		m_outputFind.reset(new OutputFindState());

	OutputFindState &state = *m_outputFind;
	QTextDocument   *doc   = m_output ? m_output->document() : nullptr;
	if (!doc)
		return false;
	const int totalLines = doc->blockCount();
	auto      lineTextAt = [doc](const int zeroBasedLine)
	{
		const QTextBlock block = doc->findBlockByNumber(zeroBasedLine);
		return block.isValid() ? block.text() : QString();
	};

	state.again = again;

	QString findText;
	if (!state.again || state.history.isEmpty())
	{
		FindDialog dlg(state.history);
		dlg.setTitleText(state.title);
		if (!state.history.isEmpty())
			dlg.setFindText(state.history.front());
		dlg.setMatchCase(state.matchCase);
		dlg.setForwards(state.forwards);
		dlg.setRegexp(state.regexp);

		if (dlg.execModal() != QDialog::Accepted)
			return false;

		state.matchCase = dlg.matchCase();
		state.forwards  = dlg.forwards();
		state.regexp    = dlg.regexp();
		findText        = dlg.findText();

		if (!findText.isEmpty() && (state.history.isEmpty() || state.history.front() != findText))
			state.history.prepend(findText);

		state.lastFindText = findText;
		state.matchesOnLine.clear();
		state.startColumn = -1;
		state.endColumn   = -1;
		state.currentLine = state.forwards ? 0 : totalLines - 1;

		if (state.regexp)
		{
			QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
			if (!state.matchCase)
				options |= QRegularExpression::CaseInsensitiveOption;
			state.regex = QRegularExpression(findText, options);
			if (!state.regex.isValid())
			{
				QMessageBox::warning(
				    this, QStringLiteral("Find"),
				    QStringLiteral("Regular expression error: %1").arg(state.regex.errorString()));
				return false;
			}
		}
	}
	else
	{
		if (!state.history.isEmpty())
			state.lastFindText = state.history.front();
		findText = state.lastFindText;
		if (state.matchesOnLine.isEmpty())
			state.currentLine += state.forwards ? 1 : -1;
	}

	MainWindowHost                  *main = resolveMainWindowHost(window());
	std::unique_ptr<QProgressDialog> progress;
	const int remaining = state.forwards ? (totalLines - state.currentLine) : state.currentLine;
	if (remaining > 500)
	{
		progress = std::make_unique<QProgressDialog>(QStringLiteral("Finding: %1").arg(findText),
		                                             QStringLiteral("Cancel"), 0, totalLines, this);
		progress->setWindowTitle(QStringLiteral("Finding..."));
		progress->setAutoClose(true);
		progress->setAutoReset(false);
		progress->setMinimumDuration(0);
	}
	auto wrapUp = [&]
	{
		if (main)
			main->setStatusNormal();
		if (progress)
			progress->close();
	};
	auto notFound = [&](bool againSearch)
	{
		const QString typeLabel =
		    state.regexp ? QStringLiteral("regular expression") : QStringLiteral("text");
		const QString suffix = againSearch ? QStringLiteral(" again.") : QStringLiteral(".");
		QMessageBox::information(
		    this, QStringLiteral("Find"),
		    QStringLiteral("The %1 \"%2\" was not found%3").arg(typeLabel, findText, suffix));
		state.startColumn = -1;
		state.matchesOnLine.clear();
		wrapUp();
	};

	if (totalLines <= 0)
	{
		notFound(state.again);
		return false;
	}

	if (main)
		main->setStatusMessageNow(QStringLiteral("Finding: %1").arg(findText));

	int milestone = 0;
	while (true)
	{
		if (!state.matchesOnLine.isEmpty())
		{
			const QPair<int, int> match =
			    state.forwards ? state.matchesOnLine.takeFirst() : state.matchesOnLine.takeLast();
			state.startColumn = match.first;
			state.endColumn   = match.second;
			if (!state.repeatOnSameLine)
				state.matchesOnLine.clear();
			selectOutputRange(state.currentLine, state.startColumn, state.endColumn);
			wrapUp();
			return true;
		}

		if (state.currentLine < 0 || state.currentLine >= totalLines)
		{
			notFound(state.again);
			return false;
		}

		QString line = lineTextAt(state.currentLine);
		state.matchesOnLine.clear();
		++milestone;

		if (progress && milestone > 31)
		{
			milestone       = 0;
			const int value = state.forwards ? state.currentLine : (totalLines - state.currentLine);
			progress->setValue(qBound(0, value, totalLines));
			if (progress->wasCanceled())
			{
				state.startColumn = -1;
				wrapUp();
				return false;
			}
		}

		if (state.regexp)
		{
			QRegularExpressionMatchIterator it = state.regex.globalMatch(line);
			while (it.hasNext())
			{
				const QRegularExpressionMatch match = it.next();
				if (!match.hasMatch())
					continue;
				const int start = sizeToInt(match.capturedStart());
				const int end   = sizeToInt(match.capturedEnd());
				if (start >= 0 && end >= start)
					state.matchesOnLine.push_back(qMakePair(start, end));
				if (end >= sizeToInt(line.size()))
					break;
			}
		}
		else
		{
			Qt::CaseSensitivity sensitivity = state.matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
			const QString      &needle      = findText;
			const int           lineSize    = sizeToInt(line.size());
			int                 start       = 0;
			while (true)
			{
				const qsizetype index = line.indexOf(needle, start, sensitivity);
				if (index < 0)
					break;
				const int indexInt = sizeToInt(index);
				const int end      = indexInt + sizeToInt(needle.size());
				state.matchesOnLine.push_back(qMakePair(indexInt, end));
				start = qMax(end, start + 1);
				if (start >= lineSize)
					break;
			}
		}

		if (state.matchesOnLine.isEmpty())
			state.currentLine += state.forwards ? 1 : -1;
	}
}

void WorldView::setOutputFindDirection(bool forwards)
{
	if (!m_outputFind)
		m_outputFind.reset(new OutputFindState());
	m_outputFind->forwards = forwards;
}

bool WorldView::hasOutputFindHistory() const
{
	if (!m_outputFind)
		return false;
	return !m_outputFind->history.isEmpty();
}

CommandHistoryFindState *WorldView::commandHistoryFindState()
{
	if (!m_commandHistoryFind)
		m_commandHistoryFind.reset(new CommandHistoryFindState());
	return m_commandHistoryFind.data();
}

QStringList WorldView::commandHistoryList() const
{
	QStringList list;
	list.reserve(m_history.size());
	for (const QString &entry : m_history)
		list.append(entry);
	return list;
}

void WorldView::clearCommandHistory()
{
	m_history.clear();
	resetHistoryRecall();
	if (m_commandHistoryFind)
	{
		m_commandHistoryFind->currentLine = 0;
		m_commandHistoryFind->again       = false;
	}
}

bool WorldView::hasCommandHistory() const
{
	return !m_history.isEmpty();
}

void WorldView::sendCommandFromHistory(const QString &text)
{
	if (text.isEmpty())
		return;
	emit sendText(text);
	if (m_autoRepeat)
	{
		setInputText(text);
		if (m_input)
			m_input->selectAll();
	}
	else if (m_input)
	{
		m_settingText = true;
		m_input->clear();
		m_settingText  = false;
		m_inputChanged = false;
	}
	resetHistoryRecall();
}

void WorldView::showCommandHistoryDialog()
{
	QStringList historyList = commandHistoryList();
	if (historyList.isEmpty())
		return;

	CommandHistoryDialog dlg(this);
	dlg.m_msgList          = &historyList;
	dlg.m_sendview         = this;
	dlg.m_pHistoryFindInfo = commandHistoryFindState();
	dlg.populateList();
	dlg.exec();
}

QString WorldView::wordUnderCursor() const
{
	if (!m_output)
		return {};
	if (m_wordDelimiters.isEmpty())
	{
		QTextCursor cursor = m_output->textCursor();
		cursor.select(QTextCursor::WordUnderCursor);
		return cursor.selectedText();
	}

	QTextCursor   cursor = m_output->textCursor();
	const QString text   = m_output->toPlainText();
	if (text.isEmpty())
		return {};

	qsizetype pos = cursor.position();
	if (pos > 0 && pos == text.size())
		pos = text.size() - 1;
	if (pos < 0 || pos >= text.size())
		return {};

	auto isDelim = [this](QChar ch) { return ch.isSpace() || m_wordDelimiters.contains(ch); };

	if (isDelim(text[pos]))
	{
		if (pos > 0 && !isDelim(text[pos - 1]))
			pos -= 1;
		else
			return {};
	}

	qsizetype start = pos;
	while (start > 0 && !isDelim(text[start - 1]))
		--start;
	qsizetype end = pos;
	while (end < text.size() && !isDelim(text[end]))
		++end;

	return text.mid(start, end - start);
}

void WorldView::setWordDelimiters(const QString &delimiters, const QString &doubleClickDelimiters)
{
	m_wordDelimiters         = delimiters;
	m_wordDelimitersDblClick = doubleClickDelimiters;
}

void WorldView::setSmoothScrolling(bool smooth, bool smoother)
{
	m_smoothScrolling   = smooth;
	m_smootherScrolling = smoother;
	if (m_output && m_output->verticalScrollBar())
		m_output->verticalScrollBar()->setSingleStep(smoother ? 1 : 3);
	if (m_input && m_input->verticalScrollBar())
		m_input->verticalScrollBar()->setSingleStep(smoother ? 1 : 3);
	if (m_outputScrollBar)
		m_outputScrollBar->setSingleStep(smoother ? 1 : 3);
}

void WorldView::setBleedBackground(bool enabled)
{
	if (m_bleedBackground == enabled)
		return;
	m_bleedBackground = enabled;
	if (m_outputStack)
		m_outputStack->update();
	if (m_outputContainer)
		m_outputContainer->update();
}

void WorldView::setAllTypingToCommandWindow(bool enabled)
{
	m_allTypingToCommandWindow = enabled;
}

int WorldView::inputSelectionStartColumn() const
{
	if (!m_input)
		return 0;
	const QTextCursor cursor = m_input->textCursor();
	const int         start  = cursor.selectionStart();
	// Legacy GetInfo(236): return caret/selection start + 1 even when no selection.
	return start + 1;
}

int WorldView::inputSelectionEndColumn() const
{
	if (!m_input)
		return 0;
	const QTextCursor cursor = m_input->textCursor();
	const int         start  = cursor.selectionStart();
	const int         end    = cursor.selectionEnd();
	if (end <= start)
		return 0;
	return end;
}

QPlainTextEdit *WorldView::inputEditor() const
{
	return m_input;
}

static bool getOutputSelectionBounds(const WrapTextBrowser *output, const WrapTextBrowser *liveOutput,
                                     int &startLine, int &startColumn, int &endLine, int &endColumn)
{
	QTextCursor cursor;
	if (output && output->textCursor().hasSelection())
		cursor = output->textCursor();
	else if (liveOutput && liveOutput->textCursor().hasSelection())
		cursor = liveOutput->textCursor();
	else
		return false;

	const int start = cursor.selectionStart();
	const int end   = cursor.selectionEnd();
	if (end <= start)
		return false;

	QTextDocument *doc = cursor.document();
	QTextCursor    startCursor(doc);
	startCursor.setPosition(start);
	QTextCursor endCursor(doc);
	endCursor.setPosition(end);

	const QTextBlock startBlock = startCursor.block();
	const QTextBlock endBlock   = endCursor.block();

	startLine   = startBlock.blockNumber() + 1;
	endLine     = endBlock.blockNumber() + 1;
	startColumn = start - startBlock.position() + 1;
	endColumn   = end - endBlock.position() + 1;
	return true;
}

void WorldView::handleOutputSelectionChanged()
{
	int        startLine   = 0;
	int        startColumn = 0;
	int        endLine     = 0;
	int        endColumn   = 0;
	const bool hasSelection =
	    getOutputSelectionBounds(m_output, m_liveOutput, startLine, startColumn, endLine, endColumn);
	const bool selectionSame =
	    (hasSelection == m_hasOutputSelection && startLine == m_lastSelectionStartLine &&
	     startColumn == m_lastSelectionStartColumn && endLine == m_lastSelectionEndLine &&
	     endColumn == m_lastSelectionEndColumn);
	if (selectionSame)
		return;

	m_hasOutputSelection       = hasSelection;
	m_lastSelectionStartLine   = startLine;
	m_lastSelectionStartColumn = startColumn;
	m_lastSelectionEndLine     = endLine;
	m_lastSelectionEndColumn   = endColumn;

	if (hasSelection && m_runtime)
	{
		const QMap<QString, QString> &attrs   = m_runtime->worldAttributes();
		auto                          enabled = [](const QString &value)
		{
			return value == QStringLiteral("1") ||
			       value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
			       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
		};
		if (enabled(attrs.value(QStringLiteral("copy_selection_to_clipboard"))))
		{
			if (enabled(attrs.value(QStringLiteral("auto_copy_to_clipboard_in_html"))))
				copySelectionAsHtml();
			else
				copySelection();
		}
	}

	emit outputSelectionChanged();
	if (m_runtime)
		m_runtime->notifyOutputSelectionChanged();
}

int WorldView::outputSelectionStartLine() const
{
	int startLine   = 0;
	int startColumn = 0;
	int endLine     = 0;
	int endColumn   = 0;
	if (!getOutputSelectionBounds(m_output, m_liveOutput, startLine, startColumn, endLine, endColumn))
		return 0;
	return startLine;
}

int WorldView::outputSelectionEndLine() const
{
	int startLine   = 0;
	int startColumn = 0;
	int endLine     = 0;
	int endColumn   = 0;
	if (!getOutputSelectionBounds(m_output, m_liveOutput, startLine, startColumn, endLine, endColumn))
		return 0;
	return endLine;
}

int WorldView::outputSelectionStartColumn() const
{
	int startLine   = 0;
	int startColumn = 0;
	int endLine     = 0;
	int endColumn   = 0;
	if (!getOutputSelectionBounds(m_output, m_liveOutput, startLine, startColumn, endLine, endColumn))
		return 0;
	return startColumn;
}

int WorldView::outputSelectionEndColumn() const
{
	int startLine   = 0;
	int startColumn = 0;
	int endLine     = 0;
	int endColumn   = 0;
	if (!getOutputSelectionBounds(m_output, m_liveOutput, startLine, startColumn, endLine, endColumn))
		return 0;
	return endColumn;
}

int WorldView::outputClientHeight() const
{
	if (m_outputStack)
		return m_outputStack->height();
	if (!m_output)
		return 0;
	return m_output->viewport()->height();
}

int WorldView::outputClientWidth() const
{
	if (m_outputStack)
		return m_outputStack->width();
	if (!m_output)
		return 0;
	return m_output->viewport()->width();
}

QRect WorldView::outputTextRectangle() const
{
	if (!m_outputStack || !m_output || !m_output->viewport())
		return {};

	const QWidget *viewport = m_output->viewport();
	const QPoint   topLeft  = viewport->mapTo(m_outputStack, QPoint(0, 0));
	return {topLeft, viewport->size()};
}

QFont WorldView::outputFont() const
{
	if (!m_output)
		return {};
	return m_output->font();
}

void WorldView::focusInput() const
{
	if (!m_input)
		return;
	m_input->setFocus(Qt::OtherFocusReason);
}

int WorldView::outputScrollPosition() const
{
	if (!m_output)
		return 0;
	QScrollBar *const bar = m_output->verticalScrollBar();
	if (!bar)
		return 0;
	return bar->value();
}

bool WorldView::outputScrollBarVisible() const
{
	return m_outputScrollBar && m_outputScrollBar->isVisible();
}

bool WorldView::outputScrollBarWanted() const
{
	return m_outputScrollBarWanted;
}

void WorldView::requestDrawOutputWindowNotification()
{
	if (m_drawNotifyQueued)
		return;
	if (!m_drawNotifyThrottle.isValid())
		m_drawNotifyThrottle.start();
	constexpr qint64 kMinDrawNotifyIntervalMs = 16;
	const qint64     nowMs                    = m_drawNotifyThrottle.elapsed();
	const qint64     sinceLastMs              = nowMs - m_lastDrawNotifyMs;
	const int        delayMs                  = (sinceLastMs >= kMinDrawNotifyIntervalMs)
	                                                ? 0
	                                                : static_cast<int>(kMinDrawNotifyIntervalMs - sinceLastMs);
	m_drawNotifyQueued                        = true;
	QTimer::singleShot(delayMs, this,
	                   [this]
	                   {
		                   m_drawNotifyQueued = false;
		                   if (!m_drawNotifyThrottle.isValid())
			                   m_drawNotifyThrottle.start();
		                   m_lastDrawNotifyMs = m_drawNotifyThrottle.elapsed();
		                   notifyDrawOutputWindow();
	                   });
}

void WorldView::notifyDrawOutputWindow() const
{
	if (!m_runtime)
		return;
	const int fontHeight     = m_runtime->outputFontHeight();
	const int adjustedScroll = outputScrollPosition() - m_inputPixelOffset;
	int       firstLine      = 1;
	if (fontHeight > 0)
	{
		firstLine = (adjustedScroll / fontHeight) + 1;
		if (firstLine < 1)
			firstLine = 1;
	}
	m_runtime->notifyDrawOutputWindow(firstLine, adjustedScroll);
}

void WorldView::copySelection() const
{
	if (hasOutputSelection())
	{
		QTextCursor cursor;
		if (m_output && m_output->textCursor().hasSelection())
			cursor = m_output->textCursor();
		else if (m_liveOutput && m_liveOutput->textCursor().hasSelection())
			cursor = m_liveOutput->textCursor();
		else
			return;
		QString text = cursor.selectedText();
		text.replace(QChar(0x2029), QLatin1Char('\n'));
		QGuiApplication::clipboard()->setText(text);
		return;
	}

	if (hasInputSelection())
	{
		const QTextCursor cursor = m_input->textCursor();
		QString           text   = cursor.selectedText();
		text.replace(QChar(0x2029), QLatin1Char('\n'));
		QGuiApplication::clipboard()->setText(text);
	}
}

QString WorldView::outputSelectionText() const
{
	if (!hasOutputSelection())
		return {};
	QTextCursor cursor;
	if (m_output && m_output->textCursor().hasSelection())
		cursor = m_output->textCursor();
	else if (m_liveOutput && m_liveOutput->textCursor().hasSelection())
		cursor = m_liveOutput->textCursor();
	else
		return {};
	QString text = cursor.selectedText();
	text.replace(QChar(0x2029), QLatin1Char('\n'));
	return text;
}

QString WorldView::inputSelectionText() const
{
	if (!hasInputSelection())
		return {};
	const QTextCursor cursor = m_input->textCursor();
	QString           text   = cursor.selectedText();
	text.replace(QChar(0x2029), QLatin1Char('\n'));
	return text;
}

QString WorldView::outputPlainText() const
{
	if (m_output)
		return m_output->toPlainText();
	if (m_liveOutput)
		return m_liveOutput->toPlainText();
	return {};
}

QString WorldView::outputSelectedText() const
{
	QTextCursor cursor;
	if (m_output && m_output->textCursor().hasSelection())
		cursor = m_output->textCursor();
	else if (m_liveOutput && m_liveOutput->textCursor().hasSelection())
		cursor = m_liveOutput->textCursor();
	else
		return {};

	QString text = cursor.selectedText();
	text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
	text.replace(QChar::LineSeparator, QLatin1Char('\n'));
	return text;
}

void WorldView::copySelectionAsHtml() const
{
	if (!hasOutputSelection())
		return;

	QTextCursor            cursor;
	const WrapTextBrowser *source = nullptr;
	if (m_output && m_output->textCursor().hasSelection())
	{
		cursor = m_output->textCursor();
		source = m_output;
	}
	else if (m_liveOutput && m_liveOutput->textCursor().hasSelection())
	{
		cursor = m_liveOutput->textCursor();
		source = m_liveOutput;
	}
	else
	{
		return;
	}
	const QTextDocumentFragment frag(cursor);
	QString                     text = frag.toPlainText();
	text.replace(QChar(0x2029), QLatin1Char('\n'));

	const QString   fragmentHtml = frag.toHtml();
	QString         bodyContent  = fragmentHtml;
	const qsizetype bodyStart    = fragmentHtml.indexOf(QStringLiteral("<body"));
	if (bodyStart >= 0)
	{
		const qsizetype bodyTagEnd = fragmentHtml.indexOf(QLatin1Char('>'), bodyStart);
		const qsizetype bodyEnd    = fragmentHtml.indexOf(QStringLiteral("</body>"), bodyTagEnd);
		if (bodyTagEnd >= 0 && bodyEnd > bodyTagEnd)
			bodyContent = fragmentHtml.mid(bodyTagEnd + 1, bodyEnd - bodyTagEnd - 1);
	}
	if (bodyContent.isEmpty())
		bodyContent = fragmentHtml;

	const QFont   font     = source->font();
	const QString fontFace = font.family();
	const QColor  back     = source->palette().color(QPalette::Base);

	QString       html;
	html += QStringLiteral("<!-- Produced by QMud v %1 - www.qmud.dev -->\n")
	            .arg(QString::fromLatin1(kVersionString));
	html += QStringLiteral("<table border=0 cellpadding=5 bgcolor=\"#%1%2%3\">\n")
	            .arg(back.red(), 2, 16, QLatin1Char('0'))
	            .arg(back.green(), 2, 16, QLatin1Char('0'))
	            .arg(back.blue(), 2, 16, QLatin1Char('0'));
	html += QStringLiteral("<tr><td>\n");
	html += QStringLiteral(
	            "<pre><code>"
	            "<font size=2 face=\"%1, DejaVu Sans Mono, Consolas, Menlo, Monaco, Courier New, Courier\">"
	            "<font color=\"#0\">")
	            .arg(fontFace.toHtmlEscaped());
	html += bodyContent;
	html += QStringLiteral("</font></font></code></pre>\n");
	html += QStringLiteral("</td></tr></table>\n");

	auto data = std::make_unique<QMimeData>();
	data->setHtml(html);
	data->setText(text);
	QGuiApplication::clipboard()->setMimeData(data.release());
}

void WorldView::appendOutputTextInternal(const QString &text, bool newLine, bool recordLine, int flags,
                                         const QVector<WorldRuntime::StyleSpan> &spans)
{
	int recordedFlags = flags;
	if (m_runtime)
	{
		const QMap<QString, QString> &attrs   = m_runtime->worldAttributes();
		auto                          enabled = [](const QString &value)
		{
			return value == QStringLiteral("1") ||
			       value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
			       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
		};
		const bool logOutput = enabled(attrs.value(QStringLiteral("log_output")));
		const bool logInput  = enabled(attrs.value(QStringLiteral("log_input")));
		const bool logNotes  = enabled(attrs.value(QStringLiteral("log_notes")));
		bool       shouldLog = false;
		if (flags & WorldRuntime::LineNote)
			shouldLog = logNotes;
		else if (flags & WorldRuntime::LineInput)
			shouldLog = logInput;
		else if (flags & WorldRuntime::LineOutput)
			shouldLog = logOutput;
		if (shouldLog)
			recordedFlags |= WorldRuntime::LineLog;
	}

	QVector<WorldRuntime::StyleSpan> displaySpans = spans;
	if (displaySpans.isEmpty() && !text.isEmpty())
	{
		WorldRuntime::StyleSpan span;
		span.length = sizeToInt(text.size());
		if (m_output)
		{
			QColor fore;
			QColor back;
			if (m_runtime)
			{
				const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
				fore = parseColor(attrs.value(QStringLiteral("output_text_colour")));
				back = parseColor(attrs.value(QStringLiteral("output_background_colour")));
			}
			if (!fore.isValid())
				fore = m_output->palette().color(QPalette::Text);
			if (!back.isValid())
				back = m_outputBackground.isValid() ? m_outputBackground
				                                    : m_output->palette().color(QPalette::Base);
			span.fore = fore;
			span.back = back;
		}
		displaySpans.push_back(span);
	}

	WorldRuntime::LineEntry displayEntry;
	displayEntry.text  = text;
	displayEntry.flags = recordedFlags;
	displayEntry.spans = displaySpans;
	displayEntry.time  = QDateTime::currentDateTime();
	QDateTime previousLineTime;

	if (recordLine && m_runtime)
	{
		if (displaySpans.isEmpty())
			m_runtime->addLine(text, recordedFlags, newLine);
		else
			m_runtime->addLine(text, recordedFlags, displaySpans, newLine);

		const QVector<WorldRuntime::LineEntry> &lines = m_runtime->lines();
		if (!lines.isEmpty())
		{
			displayEntry = lines.last();
			if (lines.size() > 1)
				previousLineTime = lines.at(lines.size() - 2).time;
		}
	}

	if (!m_outputDocument)
		return;

	const bool shouldBreakAfterInlineInput =
	    (flags & (WorldRuntime::LineOutput | WorldRuntime::LineNote)) != 0;

	if (m_frozen)
	{
		if (shouldBreakAfterInlineInput && m_breakBeforeNextServerOutput)
		{
			// If this call already hard-breaks (e.g. Note()), just consume the
			// pending break flag. Otherwise, inject a line break first.
			if (!(text.isEmpty() && newLine))
				m_pendingOutput.push_back(PendingHtml{QString(), true});
			m_breakBeforeNextServerOutput = false;
		}

		QString                          displayText   = displayEntry.text;
		QVector<WorldRuntime::StyleSpan> renderedSpans = displayEntry.spans;
		buildDisplayLine(displayEntry, previousLineTime, displayText, renderedSpans);

		const double opacity = lineOpacityForTimestamp(displayEntry.time);
		m_pendingOutput.push_back(
		    PendingHtml{buildOutputHtml(displayText, renderedSpans, m_showBold, m_showItalic, m_showUnderline,
		                                m_alternativeInverse, m_lineSpacing, opacity),
		                newLine});
		return;
	}

	if (shouldBreakAfterInlineInput && m_breakBeforeNextServerOutput)
	{
		// If this call already hard-breaks (e.g. Note()), just consume the
		// pending break flag. Otherwise, inject a line break first.
		if (!(text.isEmpty() && newLine))
			appendOutputHtml(QString(), true);
		m_breakBeforeNextServerOutput = false;
	}

	QString                          displayText   = displayEntry.text;
	QVector<WorldRuntime::StyleSpan> renderedSpans = displayEntry.spans;
	buildDisplayLine(displayEntry, previousLineTime, displayText, renderedSpans);

	const double opacity = lineOpacityForTimestamp(displayEntry.time);
	if (!appendOutputTextFast(displayText, renderedSpans, newLine, opacity))
	{
		appendOutputHtml(buildOutputHtml(displayText, renderedSpans, m_showBold, m_showItalic,
		                                 m_showUnderline, m_alternativeInverse, m_lineSpacing, opacity),
		                 newLine);
	}
	requestDrawOutputWindowNotification();
}

bool WorldView::appendOutputTextFast(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans,
                                     bool newLine, double opacity)
{
	if (!m_outputDocument)
		return false;
	if (m_lineSpacing > 0)
		return false;
	if (opacity < 0.999)
		return false;

	QTextCursor cursor(m_outputDocument);
	cursor.movePosition(QTextCursor::End);

	QTextCharFormat baseFormat = cursor.charFormat();
	baseFormat.setAnchor(false);
	baseFormat.setAnchorHref(QString());
	baseFormat.setToolTip(QString());
	baseFormat.setAnchorNames(QStringList());
	if (m_output)
	{
		baseFormat.setForeground(m_output->palette().color(QPalette::Text));
		const QColor defaultBack =
		    m_outputBackground.isValid() ? m_outputBackground : m_output->palette().color(QPalette::Base);
		baseFormat.setBackground(defaultBack);
	}

	if (spans.isEmpty())
	{
		if (!text.isEmpty())
			cursor.insertText(text, baseFormat);
	}
	else
	{
		int offset = 0;
		for (const WorldRuntime::StyleSpan &span : spans)
		{
			const int length = qMax(0, span.length);
			if (length == 0)
				continue;
			if (span.actionType != WorldRuntime::ActionNone)
				return false;

			const QString chunk = text.mid(offset, length);
			offset += length;
			if (chunk.isEmpty())
				continue;

			QTextCharFormat format = baseFormat;
			QColor          fore   = span.fore;
			QColor          back   = span.back;
			if (span.inverse)
			{
				qSwap(fore, back);
				if (m_alternativeInverse && span.bold)
					qSwap(fore, back);
			}
			if (fore.isValid())
				format.setForeground(fore);
			if (back.isValid())
				format.setBackground(back);
			format.setFontWeight(span.bold && m_showBold ? QFont::Bold : baseFormat.fontWeight());
			format.setFontItalic(span.italic && m_showItalic);
			format.setFontUnderline(span.underline && m_showUnderline);
			format.setFontStrikeOut(span.strike);
			cursor.insertText(chunk, format);
		}

		if (offset < text.size())
			cursor.insertText(text.mid(offset), baseFormat);
	}

	if (newLine)
		cursor.insertBlock(cursor.blockFormat(), baseFormat);

	if (!m_bulkOutputRebuild)
		requestOutputScrollToEnd();
	return true;
}

void WorldView::updatePartialOutputText(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans)
{
	if (!m_outputDocument || m_frozen)
		return;

	if (text.isEmpty())
	{
		clearPartialOutput();
		return;
	}

	QTextCursor cursor(m_outputDocument);
	cursor.movePosition(QTextCursor::End);

	if (m_breakBeforeNextServerOutput && !text.isEmpty())
	{
		m_hasPartialOutput    = false;
		m_partialOutputStart  = 0;
		m_partialOutputLength = 0;
		appendOutputHtml(QString(), true);
		m_breakBeforeNextServerOutput = false;
		cursor                        = QTextCursor(m_outputDocument);
		cursor.movePosition(QTextCursor::End);
	}

	if (m_hasPartialOutput)
	{
		cursor.setPosition(m_partialOutputStart);
		cursor.setPosition(m_partialOutputStart + m_partialOutputLength, QTextCursor::KeepAnchor);
		cursor.removeSelectedText();
		cursor.setPosition(m_partialOutputStart);
	}
	else
	{
		m_partialOutputStart = cursor.position();
	}

	const double  opacity = lineOpacityForTimestamp(QDateTime::currentDateTime());
	const QString html    = buildOutputHtml(text, spans, m_showBold, m_showItalic, m_showUnderline,
	                                        m_alternativeInverse, m_lineSpacing, opacity);
	cursor.insertHtml(html);

	m_partialOutputLength = cursor.position() - m_partialOutputStart;
	m_hasPartialOutput    = true;

	requestOutputScrollToEnd();
}

void WorldView::clearPartialOutput()
{
	if (!m_outputDocument || !m_hasPartialOutput)
		return;

	QTextCursor cursor(m_outputDocument);
	cursor.setPosition(m_partialOutputStart);
	cursor.setPosition(m_partialOutputStart + m_partialOutputLength, QTextCursor::KeepAnchor);
	cursor.removeSelectedText();

	m_hasPartialOutput    = false;
	m_partialOutputStart  = 0;
	m_partialOutputLength = 0;

	requestOutputScrollToEnd();
}

void WorldView::clearOutputBuffer()
{
	if (!m_outputDocument)
		return;

	m_outputDocument->clear();
	m_pendingOutput.clear();
	m_hasPartialOutput    = false;
	m_partialOutputStart  = 0;
	m_partialOutputLength = 0;

	if (m_scrollbackSplitActive)
		scrollViewToEnd(m_liveOutput);
	else
		scrollViewToEnd(m_output);
	requestDrawOutputWindowNotification();
}

void WorldView::rebuildOutputFromLines(const QVector<WorldRuntime::LineEntry> &lines)
{
	if (!m_outputDocument)
		return;

	m_outputDocument->clear();
	m_pendingOutput.clear();
	m_hasPartialOutput    = false;
	m_partialOutputStart  = 0;
	m_partialOutputLength = 0;

	const bool previousBulkState = m_bulkOutputRebuild;
	m_bulkOutputRebuild          = true;
	QDateTime previousLineTime;
	for (const auto &entry : lines)
	{
		QString                          displayText  = entry.text;
		QVector<WorldRuntime::StyleSpan> displaySpans = entry.spans;
		buildDisplayLine(entry, previousLineTime, displayText, displaySpans);
		appendOutputHtml(buildOutputHtml(displayText, displaySpans, m_showBold, m_showItalic, m_showUnderline,
		                                 m_alternativeInverse, m_lineSpacing,
		                                 lineOpacityForTimestamp(entry.time)),
		                 entry.hardReturn);
		previousLineTime = entry.time;
	}
	m_bulkOutputRebuild = previousBulkState;

	if (m_scrollbackSplitActive)
		scrollViewToEnd(m_liveOutput);
	else
		scrollViewToEnd(m_output);
	requestDrawOutputWindowNotification();
}

void WorldView::buildDisplayLine(const WorldRuntime::LineEntry &entry, const QDateTime &previousLineTime,
                                 QString &displayText, QVector<WorldRuntime::StyleSpan> &displaySpans) const
{
	if (displaySpans.isEmpty() && !displayText.isEmpty())
	{
		WorldRuntime::StyleSpan span;
		span.length = sizeToInt(displayText.size());
		if (m_output)
		{
			QColor fore;
			QColor back;
			if (m_runtime)
			{
				const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
				fore = parseColor(attrs.value(QStringLiteral("output_text_colour")));
				back = parseColor(attrs.value(QStringLiteral("output_background_colour")));
			}
			if (!fore.isValid())
				fore = m_output->palette().color(QPalette::Text);
			if (!back.isValid())
				back = m_outputBackground.isValid() ? m_outputBackground
				                                    : m_output->palette().color(QPalette::Base);
			span.fore = fore;
			span.back = back;
		}
		displaySpans.push_back(span);
	}

	if (!m_runtime)
		return;

	auto preambleKey           = QStringLiteral("timestamp_output");
	auto preambleTextColourKey = QStringLiteral("timestamp_output_text_colour");
	auto preambleBackColourKey = QStringLiteral("timestamp_output_back_colour");
	if (entry.flags & WorldRuntime::LineNote)
	{
		preambleKey           = QStringLiteral("timestamp_notes");
		preambleTextColourKey = QStringLiteral("timestamp_notes_text_colour");
		preambleBackColourKey = QStringLiteral("timestamp_notes_back_colour");
	}
	else if (entry.flags & WorldRuntime::LineInput)
	{
		preambleKey           = QStringLiteral("timestamp_input");
		preambleTextColourKey = QStringLiteral("timestamp_input_text_colour");
		preambleBackColourKey = QStringLiteral("timestamp_input_back_colour");
	}

	const QMap<QString, QString> &attrs    = m_runtime->worldAttributes();
	QString                       preamble = attrs.value(preambleKey);
	if (preamble.isEmpty())
		return;

	const QDateTime lineTime   = entry.time.isValid() ? entry.time : QDateTime::currentDateTime();
	const QDateTime worldStart = m_runtime->worldStartTime();
	const double    elapsed    = (worldStart.isValid() && lineTime.isValid())
	                                 ? (static_cast<double>(worldStart.msecsTo(lineTime)) / 1000.0)
	                                 : 0.0;
	const double    delta      = (previousLineTime.isValid() && lineTime.isValid())
	                                 ? (static_cast<double>(previousLineTime.msecsTo(lineTime)) / 1000.0)
	                                 : 0.0;

	preamble.replace(QStringLiteral("%e"), QStringLiteral("%1").arg(elapsed, 0, 'f', 6));
	preamble.replace(QStringLiteral("%D"), QStringLiteral("%1").arg(delta, 10, 'f', 6));
	preamble = m_runtime->formatTime(lineTime, preamble, false);
	if (preamble.isEmpty())
		return;

	WorldRuntime::StyleSpan prefixSpan;
	prefixSpan.length = sizeToInt(preamble.size());
	prefixSpan.fore   = parseColor(attrs.value(preambleTextColourKey));
	prefixSpan.back   = parseColor(attrs.value(preambleBackColourKey));
	if (!prefixSpan.fore.isValid() && m_output)
		prefixSpan.fore = m_output->palette().color(QPalette::Text);
	if (!prefixSpan.back.isValid())
	{
		if (m_outputBackground.isValid())
			prefixSpan.back = m_outputBackground;
		else if (m_output)
			prefixSpan.back = m_output->palette().color(QPalette::Base);
	}
	prefixSpan.changed = true;

	displayText.prepend(preamble);
	displaySpans.prepend(prefixSpan);
}

QColor WorldView::parseColor(const QString &value)
{
	if (value.isEmpty())
		return {};

	QColor color(value);
	if (color.isValid())
		return color;

	bool      ok      = false;
	const int numeric = value.toInt(&ok);
	if (ok)
	{
		const int r = (numeric >> 16) & 0xFF;
		const int g = (numeric >> 8) & 0xFF;
		const int b = numeric & 0xFF;
		return {r, g, b};
	}

	return {};
}

QFont::Weight WorldView::mapFontWeight(int weight)
{
	if (weight >= 700)
		return QFont::Bold;
	if (weight >= 600)
		return QFont::DemiBold;
	if (weight >= 500)
		return QFont::Medium;
	return QFont::Normal;
}

void WorldView::paintMiniWindows(QPainter *painter, bool underneath) const
{
	if (!painter)
		return;

	if (underneath)
	{
		const QColor back = m_outputBackground.isValid() ? m_outputBackground : QColor(0, 0, 0);
		painter->fillRect(painter->viewport(), back);
	}

	if (!m_runtime)
		return;

	if (!underneath && m_bleedBackground)
	{
		const QColor back = m_outputBackground.isValid() ? m_outputBackground : QColor(0, 0, 0);
		painter->fillRect(painter->viewport(), back);
	}

	const QSize clientSize = m_outputStack ? m_outputStack->size() : size();
	const QSize ownerSize  = size();
	if (underneath)
	{
		const QImage image = m_runtime->backgroundImage();
		if (!image.isNull())
		{
			const int mode = m_runtime->backgroundImageMode();
			if (mode == 13)
			{
				for (int x = 0; x < clientSize.width(); x += image.width())
				{
					for (int y = 0; y < clientSize.height(); y += image.height())
						painter->drawImage(QPoint(x, y), image);
				}
			}
			else
			{
				const QRect rect = positionImageRect(image.size(), clientSize, ownerSize, mode);
				painter->drawImage(rect, image);
			}
		}
	}
	else
	{
		const QImage image = m_runtime->foregroundImage();
		if (!image.isNull())
		{
			const int mode = m_runtime->foregroundImageMode();
			if (mode == 13)
			{
				for (int x = 0; x < clientSize.width(); x += image.width())
				{
					for (int y = 0; y < clientSize.height(); y += image.height())
						painter->drawImage(QPoint(x, y), image);
				}
			}
			else
			{
				const QRect rect = positionImageRect(image.size(), clientSize, ownerSize, mode);
				painter->drawImage(rect, image);
			}
		}
	}
	m_runtime->layoutMiniWindows(clientSize, ownerSize, underneath);

	const auto windows         = m_runtime->sortedMiniWindows();
	auto       makeTransparent = [](const QImage &source, const QColor &key) -> QImage
	{
		if (source.isNull())
			return source;
		QImage     image  = source.convertToFormat(QImage::Format_ARGB32);
		const QRgb keyRgb = qRgb(key.red(), key.green(), key.blue());
		for (int y = 0; y < image.height(); ++y)
		{
			auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
			for (int x = 0; x < image.width(); ++x)
			{
				if ((line[x] & 0x00FFFFFF) == (keyRgb & 0x00FFFFFF))
					line[x] &= 0x00FFFFFF;
				else
					line[x] |= 0xFF000000;
			}
		}
		return image;
	};

	for (MiniWindow *window : windows)
	{
		if (!window || !window->show || window->temporarilyHide)
			continue;
		const bool drawUnder = (window->flags & kMiniWindowDrawUnderneath) != 0;
		if (drawUnder != underneath)
			continue;

		QImage image = window->surface;
		if ((window->flags & kMiniWindowTransparent) != 0)
			image = makeTransparent(window->surface, window->background);

		if (!image.isNull())
		{
			if ((window->flags & kMiniWindowAbsoluteLocation) == 0 && window->position == 13)
			{
				const int tileWidth  = image.width();
				const int tileHeight = image.height();
				if (tileWidth > 0 && tileHeight > 0)
				{
					for (int x = 0; x < clientSize.width(); x += tileWidth)
					{
						for (int y = 0; y < clientSize.height(); y += tileHeight)
							painter->drawImage(QPoint(x, y), image);
					}
				}
				continue;
			}

			if ((window->flags & kMiniWindowAbsoluteLocation) == 0 && window->position >= 0 &&
			    window->position <= 3)
			{
				painter->drawImage(window->rect, image);
			}
			else
			{
				if ((window->flags & kMiniWindowAbsoluteLocation) != 0 && hasScaledAbsoluteRect(window))
				{
					const bool smooth = painter->testRenderHint(QPainter::SmoothPixmapTransform);
					painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
					painter->drawImage(window->rect, image);
					painter->setRenderHint(QPainter::SmoothPixmapTransform, smooth);
				}
				else
				{
					painter->drawImage(window->rect.topLeft(), image);
				}
			}
		}
	}
}

QPoint WorldView::mapEventToOutputStack(const QPoint &globalPos) const
{
	if (m_outputStack)
		return m_outputStack->mapFromGlobal(globalPos);
	return mapFromGlobal(globalPos);
}

QPoint WorldView::mapEventToOutputStack(const QPointF &localPos, const QWidget *source) const
{
	if (m_outputStack && source)
	{
		if (source == m_outputStack || m_outputStack->isAncestorOf(source))
			return source->mapTo(m_outputStack, localPos.toPoint());
		return m_outputStack->mapFromGlobal(source->mapToGlobal(localPos.toPoint()));
	}
	if (m_outputStack)
		return m_outputStack->mapFromGlobal(localPos.toPoint());
	return localPos.toPoint();
}

MiniWindow *WorldView::hitTestMiniWindow(const QPoint &localPos, QString &hotspotId, QString &windowName,
                                         bool includeUnderneath) const
{
	hotspotId.clear();
	windowName.clear();
	if (!m_runtime || !m_outputStack)
		return nullptr;

	const auto windows = m_runtime->sortedMiniWindows();
	for (MiniWindow *window : windows | std::views::reverse)
	{
		if (!window || !window->show || window->temporarilyHide)
			continue;
		// Some migrated profiles/plugins persist stale flags; if hotspots exist, keep them interactive.
		if ((window->flags & kMiniWindowIgnoreMouse) && window->hotspots.isEmpty())
			continue;
		if (!includeUnderneath && (window->flags & kMiniWindowDrawUnderneath))
			continue;
		if (window->rect.width() <= 0 || window->rect.height() <= 0)
			continue;
		if (!window->rect.contains(localPos))
			continue;
		const QPoint relative = miniWindowDisplayToContent(window, localPos - window->rect.topLeft());
		for (auto hit = window->hotspots.constBegin(); hit != window->hotspots.constEnd(); ++hit)
		{
			if (hit.value().rect.contains(relative))
			{
				hotspotId  = hit.key();
				windowName = window->name;
				return window;
			}
		}
		windowName = window->name;
		return window;
	}

	return nullptr;
}

void WorldView::callHotspotCallback(MiniWindow *window, const QString &hotspotId, const QString &callbackName,
                                    int flags) const
{
	if (!window || callbackName.isEmpty() || hotspotId.isEmpty())
		return;
	if (!m_runtime)
		return;
	window->executingScript = true;
	bool ok                 = false;
	if (window->callbackPlugin.isEmpty())
		ok = m_runtime->callWorldHotspotFunction(callbackName, flags, hotspotId);
	else
		ok = m_runtime->callPluginHotspotFunction(window->callbackPlugin, callbackName, flags, hotspotId);
	window->executingScript = false;
	if (miniWindowMouseDebugEnabled())
	{
		miniWindowMouseDebug(
		    QStringLiteral("callback window=%1 hotspot=%2 plugin=%3 fn=%4 flags=0x%5 ok=%6")
		        .arg(window->name, hotspotId,
		             window->callbackPlugin.isEmpty() ? QStringLiteral("<world>") : window->callbackPlugin,
		             callbackName, QString::number(flags, 16),
		             ok ? QStringLiteral("1") : QStringLiteral("0")));
	}
}

void WorldView::cancelMouseOver(MiniWindow *window, const QString &hotspotId)
{
	if (!window || hotspotId.isEmpty())
		return;
	const auto it = window->hotspots.find(hotspotId);
	if (it != window->hotspots.end() && !it->cancelMouseOver.isEmpty())
		callHotspotCallback(window, hotspotId, it->cancelMouseOver, withMiniWindowModifierFlags(0));
	window->mouseOverHotspot.clear();
	if (m_tooltipHotspot == hotspotId)
	{
		QToolTip::hideText();
		m_tooltipHotspot.clear();
	}
	clearPendingHotspotTooltip();
	clearHotspotCursor();
}

void WorldView::clearHotspotCursor()
{
	applyOutputCursor(nullptr);
}

void WorldView::applyOutputCursor(const QCursor *cursor)
{
	auto apply = [cursor](QWidget *widget)
	{
		if (!widget)
			return;
		if (cursor)
			widget->setCursor(*cursor);
		else
			widget->unsetCursor();
	};

	apply(this);
	apply(m_outputContainer);
	apply(m_outputStack);
	apply(m_outputSplitter);
	apply(m_output);
	apply(m_output ? m_output->viewport() : nullptr);
	apply(m_liveOutput);
	apply(m_liveOutput ? m_liveOutput->viewport() : nullptr);
}

void WorldView::updateHotspotCursor(MiniWindow *window, const QString &hotspotId)
{
	if (!window || hotspotId.isEmpty())
	{
		clearHotspotCursor();
		return;
	}
	const auto it = window->hotspots.find(hotspotId);
	if (it == window->hotspots.end())
	{
		clearHotspotCursor();
		return;
	}
	const int cursorCode = it->cursor;
	if (cursorCode == -1)
	{
		clearHotspotCursor();
		return;
	}
	const QCursor cursor = hotspotCursor(cursorCode);
	applyOutputCursor(&cursor);
}

int WorldView::computeMiniWindowMouseFlags(const QMouseEvent *event, bool doubleClick, int baseFlags)
{
	Q_UNUSED(event);
	int flags = baseFlags;
	if (doubleClick)
		flags |= kMiniMouseDouble;
	return withMiniWindowModifierFlags(flags);
}

bool WorldView::handleMiniWindowMouseLeave()
{
	if (!m_runtime)
	{
		clearPendingHotspotTooltip();
		m_hoverWindowName.clear();
		return false;
	}
	m_lastMousePos    = QPoint(-1, -1);
	m_hasLastMousePos = true;
	m_runtime->notifyMiniWindowMouseMoved(-1, -1, QString());
	if (m_hoverWindowName.isEmpty())
		return false;
	MiniWindow *window = m_runtime->miniWindow(m_hoverWindowName);
	if (window && !window->mouseOverHotspot.isEmpty())
		cancelMouseOver(window, window->mouseOverHotspot);
	clearPendingHotspotTooltip();
	m_hoverWindowName.clear();
	return true;
}

bool WorldView::handleMiniWindowMouseMove(const QMouseEvent *event, const QWidget *source)
{
	if (!m_runtime || !m_outputStack)
		return false;

	const QPoint globalPos  = event->globalPosition().toPoint();
	const QPoint local      = mapEventToOutputStack(event->position(), source);
	QPoint       hitLocal   = local;
	const QRect  outputRect = m_outputStack->rect();
	if (!outputRect.contains(hitLocal))
	{
		if (!m_capturedWindowName.isEmpty())
		{
			hitLocal.setX(qBound(outputRect.left(), hitLocal.x(), outputRect.right()));
			hitLocal.setY(qBound(outputRect.top(), hitLocal.y(), outputRect.bottom()));
		}
		else
		{
			handleMiniWindowMouseLeave();
			return false;
		}
	}
	m_lastMousePos             = hitLocal;
	m_hasLastMousePos          = true;
	const QPoint callbackLocal = hitLocal;

	QString      hotspotId;
	QString      windowName;
	MiniWindow  *window = hitTestMiniWindow(hitLocal, hotspotId, windowName);
	if (miniWindowMouseDebugEnabled())
	{
		miniWindowMouseDebug(
		    QStringLiteral("move local=(%1,%2) hit=(%3,%4) window=%5 hotspot=%6 captured=%7")
		        .arg(local.x())
		        .arg(local.y())
		        .arg(hitLocal.x())
		        .arg(hitLocal.y())
		        .arg(windowName.isEmpty() ? QStringLiteral("<none>") : windowName)
		        .arg(hotspotId.isEmpty() ? QStringLiteral("<none>") : hotspotId)
		        .arg(m_capturedWindowName.isEmpty() ? QStringLiteral("<none>") : m_capturedWindowName));
	}
	if (m_runtime)
		m_runtime->notifyMiniWindowMouseMoved(callbackLocal.x(), callbackLocal.y(), windowName);

	if (!m_capturedWindowName.isEmpty())
	{
		MiniWindow *captured = m_runtime->miniWindow(m_capturedWindowName);
		if (captured)
			captured->clientMousePosition = callbackLocal;
		if (!m_tooltipHotspot.isEmpty())
		{
			QToolTip::hideText();
			m_tooltipHotspot.clear();
		}
		clearPendingHotspotTooltip();
		if (captured && !captured->mouseDownHotspot.isEmpty())
		{
			const QString down = captured->mouseDownHotspot;
			auto          it   = captured->hotspots.find(down);
			if (it != captured->hotspots.end())
			{
				const QString moveCallback = it->moveCallback;
				if (!moveCallback.isEmpty())
					callHotspotCallback(captured, down, moveCallback,
					                    withMiniWindowModifierFlags(captured->flagsOnMouseDown));
			}
			return true;
		}
	}
	const QString previousHoverWindow = m_hoverWindowName;
	if (windowName != previousHoverWindow)
	{
		if (!previousHoverWindow.isEmpty())
		{
			if (MiniWindow *oldWindow = m_runtime->miniWindow(previousHoverWindow); oldWindow)
			{
				oldWindow->lastMousePosition =
				    miniWindowDisplayToContent(oldWindow, hitLocal - oldWindow->rect.topLeft());
				oldWindow->lastMouseUpdate++;
				if (!oldWindow->mouseOverHotspot.isEmpty())
					cancelMouseOver(oldWindow, oldWindow->mouseOverHotspot);
			}
		}
		m_hoverWindowName = windowName;
	}

	if (!window)
	{
		if (previousHoverWindow.isEmpty())
			clearHotspotCursor();
		return !previousHoverWindow.isEmpty() || !m_capturedWindowName.isEmpty();
	}

	window->lastMousePosition = miniWindowDisplayToContent(window, hitLocal - window->rect.topLeft());
	window->lastMouseUpdate++;

	const QString currentHotspot = window->mouseOverHotspot;
	if (hotspotId.isEmpty())
	{
		if (!currentHotspot.isEmpty())
			cancelMouseOver(window, currentHotspot);
		clearHotspotCursor();
		return true;
	}

	const auto hotspotIt = window->hotspots.find(hotspotId);

	if (currentHotspot != hotspotId)
	{
		if (!currentHotspot.isEmpty())
			cancelMouseOver(window, currentHotspot);
		window->mouseOverHotspot = hotspotId;
		updateHotspotCursor(window, hotspotId);
		if (hotspotIt != window->hotspots.end())
		{
			const QString mouseOverCallback = hotspotIt->mouseOver;
			if (!mouseOverCallback.isEmpty())
				callHotspotCallback(window, hotspotId, mouseOverCallback, withMiniWindowModifierFlags(0));
			const auto refreshedHotspot = window->hotspots.find(hotspotId);
			if (refreshedHotspot != window->hotspots.end() && !refreshedHotspot->tooltip.isEmpty())
			{
				scheduleHotspotTooltip(hotspotId, formatHotspotTooltip(refreshedHotspot->tooltip), globalPos);
			}
		}
	}
	else if (hotspotIt != window->hotspots.end())
	{
		const int     hotspotFlags      = hotspotIt->flags;
		const QString mouseOverCallback = hotspotIt->mouseOver;
		if ((hotspotFlags & 0x01) && !mouseOverCallback.isEmpty())
			callHotspotCallback(window, hotspotId, mouseOverCallback,
			                    withMiniWindowModifierFlags(kMiniMouseNotFirst));
		const auto refreshedHotspot = window->hotspots.find(hotspotId);
		if (refreshedHotspot != window->hotspots.end() && !refreshedHotspot->tooltip.isEmpty())
		{
			scheduleHotspotTooltip(hotspotId, formatHotspotTooltip(refreshedHotspot->tooltip), globalPos);
		}
	}

	return true;
}

bool WorldView::handleMiniWindowMousePress(const QMouseEvent *event, bool doubleClick, const QWidget *source)
{
	if (!m_runtime || !m_outputStack)
		return false;

	const QPoint globalPos = event->globalPosition().toPoint();
	const QPoint local     = mapEventToOutputStack(event->position(), source);
	m_lastMousePos         = local;
	m_hasLastMousePos      = true;
	if (!m_outputStack->rect().contains(local))
		return false;

	QString     hotspotId;
	QString     windowName;
	MiniWindow *window = hitTestMiniWindow(local, hotspotId, windowName, true);
	if (miniWindowMouseDebugEnabled())
	{
		miniWindowMouseDebug(QStringLiteral("press button=%1 dbl=%2 local=(%3,%4) window=%5 hotspot=%6")
		                         .arg(static_cast<int>(event->button()))
		                         .arg(doubleClick ? 1 : 0)
		                         .arg(local.x())
		                         .arg(local.y())
		                         .arg(windowName.isEmpty() ? QStringLiteral("<none>") : windowName)
		                         .arg(hotspotId.isEmpty() ? QStringLiteral("<none>") : hotspotId));
	}
	if (!m_hoverWindowName.isEmpty())
	{
		MiniWindow *hoverWindow = m_runtime->miniWindow(m_hoverWindowName);
		if (hoverWindow && !hoverWindow->mouseOverHotspot.isEmpty())
			cancelMouseOver(hoverWindow, hoverWindow->mouseOverHotspot);
		m_hoverWindowName.clear();
	}
	if (!window)
		return false;

	m_hoverWindowName           = windowName;
	window->clientMousePosition = local;
	window->lastMousePosition   = miniWindowDisplayToContent(window, local - window->rect.topLeft());
	window->lastMouseUpdate++;
	if (!window->mouseOverHotspot.isEmpty())
		cancelMouseOver(window, window->mouseOverHotspot);

	if (hotspotId.isEmpty())
	{
		if (!m_tooltipHotspot.isEmpty())
		{
			QToolTip::hideText();
			m_tooltipHotspot.clear();
		}
		clearPendingHotspotTooltip();
		if (!m_mouseCaptured)
		{
			if (m_outputStack)
				m_outputStack->grabMouse();
			else
				grabMouse();
			m_mouseCaptured = true;
		}
		m_capturedWindowName = window->name;
		return true;
	}

	int base = 0;
	switch (event->button())
	{
	case Qt::LeftButton:
		base = kMiniMouseLeft;
		break;
	case Qt::RightButton:
		base = kMiniMouseRight;
		break;
	case Qt::MiddleButton:
		base = kMiniMouseMiddle;
		break;
	default:
		break;
	}

	const int flags          = computeMiniWindowMouseFlags(event, doubleClick, base);
	window->mouseDownHotspot = hotspotId;
	window->flagsOnMouseDown =
	    flags & (kMiniMouseLeft | kMiniMouseRight | kMiniMouseDouble | kMiniMouseMiddle);
	m_capturedWindowName = window->name;
	if (!m_mouseCaptured)
	{
		if (m_outputStack)
			m_outputStack->grabMouse();
		else
			grabMouse();
		m_mouseCaptured = true;
	}

	const auto it = window->hotspots.find(hotspotId);
	if (it != window->hotspots.end())
	{
		const QString mouseDownCallback = it->mouseDown;
		if (!mouseDownCallback.isEmpty())
			callHotspotCallback(window, hotspotId, mouseDownCallback, flags);
		const auto refreshedHotspot = window->hotspots.find(hotspotId);
		if (refreshedHotspot != window->hotspots.end() && !refreshedHotspot->tooltip.isEmpty())
		{
			scheduleHotspotTooltip(hotspotId, refreshedHotspot->tooltip, globalPos);
		}
	}

	return true;
}

bool WorldView::handleMiniWindowMouseRelease(const QMouseEvent *event, const QWidget *source)
{
	if (!m_runtime || !m_mouseCaptured)
		return false;

	const QPoint local        = mapEventToOutputStack(event->position(), source);
	QPoint       boundedLocal = local;
	const QRect  outputRect   = m_outputStack->rect();
	if (!outputRect.contains(boundedLocal))
	{
		boundedLocal.setX(qBound(outputRect.left(), boundedLocal.x(), outputRect.right()));
		boundedLocal.setY(qBound(outputRect.top(), boundedLocal.y(), outputRect.bottom()));
	}
	m_lastMousePos    = boundedLocal;
	m_hasLastMousePos = true;
	QString     hotspotId;
	QString     windowName;
	MiniWindow *windowUnderCursor = hitTestMiniWindow(local, hotspotId, windowName, true);
	if (miniWindowMouseDebugEnabled())
	{
		miniWindowMouseDebug(
		    QStringLiteral("release button=%1 local=(%2,%3) window=%4 hotspot=%5 captured=%6")
		        .arg(static_cast<int>(event->button()))
		        .arg(local.x())
		        .arg(local.y())
		        .arg(windowName.isEmpty() ? QStringLiteral("<none>") : windowName)
		        .arg(hotspotId.isEmpty() ? QStringLiteral("<none>") : hotspotId)
		        .arg(m_capturedWindowName.isEmpty() ? QStringLiteral("<none>") : m_capturedWindowName));
	}

	MiniWindow *pressedWindow =
	    !m_capturedWindowName.isEmpty() ? m_runtime->miniWindow(m_capturedWindowName) : nullptr;
	QString previousDownHotspot;
	if (pressedWindow)
	{
		pressedWindow->clientMousePosition = boundedLocal;
		previousDownHotspot                = pressedWindow->mouseDownHotspot;
		pressedWindow->mouseDownHotspot.clear();
	}

	if (m_outputStack)
		m_outputStack->releaseMouse();
	else
		releaseMouse();
	m_mouseCaptured = false;
	m_capturedWindowName.clear();

	if (pressedWindow && !previousDownHotspot.isEmpty())
	{
		const int flags = withMiniWindowModifierFlags(pressedWindow->flagsOnMouseDown);
		auto      it    = pressedWindow->hotspots.find(previousDownHotspot);
		if (it != pressedWindow->hotspots.end())
		{
			const QString releaseCallback = it->releaseCallback;
			if (!releaseCallback.isEmpty())
				callHotspotCallback(pressedWindow, previousDownHotspot, releaseCallback, flags);
			it = pressedWindow->hotspots.find(previousDownHotspot);
			if (it != pressedWindow->hotspots.end())
			{
				if (windowUnderCursor == pressedWindow && hotspotId == previousDownHotspot)
				{
					const QString mouseUpCallback = it->mouseUp;
					if (!mouseUpCallback.isEmpty())
						callHotspotCallback(pressedWindow, previousDownHotspot, mouseUpCallback, flags);
				}
				else
				{
					const QString cancelMouseDownCallback = it->cancelMouseDown;
					if (!cancelMouseDownCallback.isEmpty())
						callHotspotCallback(pressedWindow, previousDownHotspot, cancelMouseDownCallback,
						                    flags);
				}
			}
		}
	}

	if (windowUnderCursor)
	{
		windowUnderCursor->lastMousePosition =
		    miniWindowDisplayToContent(windowUnderCursor, boundedLocal - windowUnderCursor->rect.topLeft());
		windowUnderCursor->lastMouseUpdate++;
	}
	if (windowUnderCursor)
		handleMiniWindowMouseMove(event, source);

	return true;
}

bool WorldView::handleMiniWindowWheel(const QWheelEvent *event, const QWidget *source) const
{
	if (!m_runtime || !m_outputStack)
		return false;

	const QPoint local = mapEventToOutputStack(event->position(), source);
	if (!m_outputStack->rect().contains(local))
		return false;

	QString     hotspotId;
	QString     windowName;
	MiniWindow *window = hitTestMiniWindow(local, hotspotId, windowName, true);
	if (!window || hotspotId.isEmpty())
		return false;

	const auto it = window->hotspots.find(hotspotId);
	if (it == window->hotspots.end() || it->scrollwheelCallback.isEmpty())
		return false;

	int       flags = 0;
	const int delta = event->angleDelta().y();
	if (delta < 0)
		flags |= kMiniMouseScrollBack;
	flags |= (qAbs(delta) << 16);
	callHotspotCallback(window, hotspotId, it->scrollwheelCallback, withMiniWindowModifierFlags(flags));
	return true;
}

void WorldView::applyRuntimeSettings()
{
	if (!m_runtime)
		return;

	const QMap<QString, QString> &attrs          = m_runtime->worldAttributes();
	const QMap<QString, QString> &multilineAttrs = m_runtime->worldMultilineAttributes();
	const QString                 outputFontName = attrs.value(QStringLiteral("output_font_name"));
	const QString                 inputFontName  = attrs.value(QStringLiteral("input_font_name"));
	const int                     outputHeight   = attrs.value(QStringLiteral("output_font_height")).toInt();
	const int                     inputHeight    = attrs.value(QStringLiteral("input_font_height")).toInt();
	const int                     outputWeight   = attrs.value(QStringLiteral("output_font_weight")).toInt();
	const int                     inputWeight    = attrs.value(QStringLiteral("input_font_weight")).toInt();
	const int                     inputItalic    = attrs.value(QStringLiteral("input_font_italic")).toInt();
	const int                     outputCharset  = attrs.value(QStringLiteral("output_font_charset")).toInt();
	const int                     inputCharset   = attrs.value(QStringLiteral("input_font_charset")).toInt();
	const QString useDefaultOutputFontValue      = attrs.value(QStringLiteral("use_default_output_font"));
	const bool    useDefaultOutputFont =
	    (useDefaultOutputFontValue.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
	     useDefaultOutputFontValue == QStringLiteral("1") ||
	     useDefaultOutputFontValue.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
	const QString useDefaultInputFontValue = attrs.value(QStringLiteral("use_default_input_font"));
	const bool    useDefaultInputFont =
	    (useDefaultInputFontValue.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
	     useDefaultInputFontValue == QStringLiteral("1") ||
	     useDefaultInputFontValue.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
	const AppController *app = AppController::instance();

	auto                 effectiveDefaultOutputFont = [&]
	{
		QString outputDefaultFamily;
		int     outputDefaultHeight  = 9;
		int     outputDefaultCharset = 1;
		if (app)
		{
			outputDefaultFamily =
			    app->getGlobalOption(QStringLiteral("DefaultOutputFont")).toString().trimmed();
			outputDefaultHeight  = app->getGlobalOption(QStringLiteral("DefaultOutputFontHeight")).toInt();
			outputDefaultCharset = app->getGlobalOption(QStringLiteral("DefaultOutputFontCharset")).toInt();
		}

		QFont         font            = qmudPreferredMonospaceFont(outputDefaultFamily, outputDefaultHeight);
		const QString preferredFamily = outputDefaultFamily.isEmpty() ? font.family() : outputDefaultFamily;
		if (const QString charsetFamily = qmudFamilyForCharset(preferredFamily, outputDefaultCharset);
		    !charsetFamily.isEmpty())
		{
			qmudApplyMonospaceFallback(font, charsetFamily);
		}
		font.setWeight(mapFontWeight(400));
		font.setItalic(false);
		return font;
	};

	auto effectiveDefaultInputFont = [&]
	{
		QString inputDefaultFamily;
		int     inputDefaultHeight  = 9;
		int     inputDefaultWeight  = 400;
		int     inputDefaultItalic  = 0;
		int     inputDefaultCharset = 1;
		if (app)
		{
			inputDefaultFamily =
			    app->getGlobalOption(QStringLiteral("DefaultInputFont")).toString().trimmed();
			inputDefaultHeight  = app->getGlobalOption(QStringLiteral("DefaultInputFontHeight")).toInt();
			inputDefaultWeight  = app->getGlobalOption(QStringLiteral("DefaultInputFontWeight")).toInt();
			inputDefaultItalic  = app->getGlobalOption(QStringLiteral("DefaultInputFontItalic")).toInt();
			inputDefaultCharset = app->getGlobalOption(QStringLiteral("DefaultInputFontCharset")).toInt();
		}

		QFont         font            = qmudPreferredMonospaceFont(inputDefaultFamily, inputDefaultHeight);
		const QString preferredFamily = inputDefaultFamily.isEmpty() ? font.family() : inputDefaultFamily;
		if (const QString charsetFamily = qmudFamilyForCharset(preferredFamily, inputDefaultCharset);
		    !charsetFamily.isEmpty())
		{
			qmudApplyMonospaceFallback(font, charsetFamily);
		}
		if (inputDefaultWeight > 0)
			font.setWeight(mapFontWeight(inputDefaultWeight));
		font.setItalic(inputDefaultItalic != 0);
		return font;
	};

	if (m_output && useDefaultOutputFont)
	{
		const QFont outputFont = effectiveDefaultOutputFont();
		m_output->setFont(outputFont);
		if (m_liveOutput)
			m_liveOutput->setFont(outputFont);
		if (m_outputDocument)
			m_outputDocument->setDefaultFont(outputFont);
	}
	else if (m_output && (!outputFontName.isEmpty() || outputHeight > 0 || outputWeight > 0))
	{
		QFont font = m_output->font();
		if (!outputFontName.isEmpty())
			qmudApplyMonospaceFallback(font, outputFontName);
		const QString preferredFamily = outputFontName.isEmpty() ? font.family() : outputFontName;
		const QString charsetFamily   = qmudFamilyForCharset(preferredFamily, outputCharset);
		if (!charsetFamily.isEmpty())
			qmudApplyMonospaceFallback(font, charsetFamily);
		if (outputHeight > 0)
			font.setPointSize(outputHeight);
		if (outputWeight > 0)
			font.setWeight(mapFontWeight(outputWeight));
		m_output->setFont(font);
		if (m_liveOutput)
			m_liveOutput->setFont(font);
		if (m_outputDocument)
			m_outputDocument->setDefaultFont(font);
	}

	if (m_input && useDefaultInputFont)
	{
		m_input->setFont(effectiveDefaultInputFont());
		updateInputHeight();
	}
	else if (m_input && (!inputFontName.isEmpty() || inputHeight > 0 || inputWeight > 0 || inputItalic != 0))
	{
		QFont font = m_input->font();
		if (!inputFontName.isEmpty())
			qmudApplyMonospaceFallback(font, inputFontName);
		const QString preferredFamily = inputFontName.isEmpty() ? font.family() : inputFontName;
		const QString charsetFamily   = qmudFamilyForCharset(preferredFamily, inputCharset);
		if (!charsetFamily.isEmpty())
			qmudApplyMonospaceFallback(font, charsetFamily);
		if (inputHeight > 0)
			font.setPointSize(inputHeight);
		if (inputWeight > 0)
			font.setWeight(mapFontWeight(inputWeight));
		font.setItalic(inputItalic != 0);
		m_input->setFont(font);
		updateInputHeight();
	}

	if (m_runtime)
	{
		if (m_output)
		{
			const QFontMetrics metrics(m_output->font());
			m_runtime->setOutputFontMetrics(metrics.height(), metrics.horizontalAdvance(QLatin1Char('M')));
		}
		if (m_input)
		{
			const QFontMetrics metrics(m_input->font());
			m_runtime->setInputFontMetrics(metrics.height(), metrics.horizontalAdvance(QLatin1Char('M')));
		}
	}

	if (m_input)
	{
		QPalette       palette      = m_input->palette();
		const QColor   inputBack    = parseColor(attrs.value(QStringLiteral("input_background_colour")));
		const QColor   inputText    = parseColor(attrs.value(QStringLiteral("input_text_colour")));
		constexpr QRgb fallbackBack = qRgb(0, 0, 0);
		constexpr QRgb fallbackText = qRgb(192, 192, 192);
		const QColor   resolvedBack = inputBack.isValid() ? inputBack : QColor::fromRgb(fallbackBack);
		QColor         resolvedText = inputText.isValid() ? inputText : QColor::fromRgb(fallbackText);
		if (resolvedText == resolvedBack)
			resolvedText = QColor::fromRgb(fallbackText);
		palette.setColor(QPalette::Base, resolvedBack);
		palette.setColor(QPalette::Text, resolvedText);
		palette.setColor(QPalette::Window, palette.color(QPalette::Base));
		m_input->setAutoFillBackground(true);
		m_input->setPalette(palette);
	}

	if (m_output)
	{
		QPalette       palette      = m_output->palette();
		const QColor   outputBack   = parseColor(attrs.value(QStringLiteral("output_background_colour")));
		const QColor   outputText   = parseColor(attrs.value(QStringLiteral("output_text_colour")));
		constexpr QRgb fallbackBack = qRgb(0, 0, 0);
		constexpr QRgb fallbackText = qRgb(192, 192, 192);
		QColor         resolvedBack = outputBack;
		if (!resolvedBack.isValid() && m_runtime && m_runtime->backgroundColour() != 0)
		{
			const long colour = m_runtime->backgroundColour();
			const int  r      = static_cast<int>(colour & 0xFF);
			const int  g      = static_cast<int>((colour >> 8) & 0xFF);
			const int  b      = static_cast<int>((colour >> 16) & 0xFF);
			resolvedBack      = QColor(r, g, b);
		}
		m_outputBackground = resolvedBack.isValid() ? resolvedBack : QColor::fromRgb(fallbackBack);
		palette.setColor(QPalette::Base, Qt::transparent);
		palette.setColor(QPalette::Window, Qt::transparent);
		palette.setColor(QPalette::Text, outputText.isValid() ? outputText : QColor::fromRgb(fallbackText));
		m_output->setAutoFillBackground(false);
		m_output->setPalette(palette);
		m_output->viewport()->setAutoFillBackground(false);
		m_output->setStyleSheet(QStringLiteral("background: transparent;"));
		if (m_liveOutput)
		{
			m_liveOutput->setAutoFillBackground(false);
			m_liveOutput->setPalette(palette);
			m_liveOutput->viewport()->setAutoFillBackground(false);
			m_liveOutput->setStyleSheet(QStringLiteral("background: transparent;"));
		}
		if (m_miniUnderlay)
			m_miniUnderlay->update();
		if (m_miniOverlay)
			m_miniOverlay->update();
	}

	const int     wrapColumn  = attrs.value(QStringLiteral("wrap_column")).toInt();
	const QString wrapEnabled = attrs.value(QStringLiteral("wrap"));
	const bool    wrapOutput  = (wrapEnabled.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
                             wrapEnabled == QStringLiteral("1") ||
                             wrapEnabled.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
	const QString autoWrap    = attrs.value(QStringLiteral("auto_wrap_window_width"));
	const bool    autoWrapWindow =
	    (autoWrap.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 || autoWrap == QStringLiteral("1") ||
	     autoWrap.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
	const QString nawsValue   = attrs.value(QStringLiteral("naws"));
	const bool    nawsEnabled = (nawsValue.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
                              nawsValue == QStringLiteral("1") ||
                              nawsValue.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
	if (m_output)
	{
		m_wrapColumn = wrapColumn;
		if (!wrapOutput)
		{
			m_output->setLineWrapMode(QTextBrowser::NoWrap);
			if (m_liveOutput)
				m_liveOutput->setLineWrapMode(QTextBrowser::NoWrap);
			m_output->setViewportMarginsPublic(0, 0, 0, 0);
			if (m_liveOutput)
				m_liveOutput->setViewportMarginsPublic(0, 0, 0, 0);
		}
		else
		{
			if (const bool fixedColumnWrap = !autoWrapWindow && m_wrapColumn > 0 && !nawsEnabled;
			    fixedColumnWrap)
			{
				// Runtime already applies fixed-column wrapping before text reaches the view.
				// Keep the Qt view in NoWrap mode so existing output does not reflow on resize.
				m_output->setLineWrapMode(QTextBrowser::NoWrap);
				m_output->setWordWrapMode(QTextOption::NoWrap);
				if (m_liveOutput)
				{
					m_liveOutput->setLineWrapMode(QTextBrowser::NoWrap);
					m_liveOutput->setWordWrapMode(QTextOption::NoWrap);
				}
				m_output->setViewportMarginsPublic(0, 0, 0, 0);
				if (m_liveOutput)
					m_liveOutput->setViewportMarginsPublic(0, 0, 0, 0);
			}
			else
			{
				m_output->setLineWrapMode(QTextBrowser::WidgetWidth);
				m_output->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
				if (m_liveOutput)
				{
					m_liveOutput->setLineWrapMode(QTextBrowser::WidgetWidth);
					m_liveOutput->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
				}
				m_output->setViewportMarginsPublic(0, 0, 0, 0);
				if (m_liveOutput)
					m_liveOutput->setViewportMarginsPublic(0, 0, 0, 0);
			}
		}
	}

	const QString wrapInput = attrs.value(QStringLiteral("wrap_input"));
	const auto    isEnabled = [](const QString &value)
	{
		return value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 || value == QStringLiteral("1") ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	};
	const auto isDisabled = [](const QString &value)
	{
		return value.compare(QStringLiteral("n"), Qt::CaseInsensitive) == 0 || value == QStringLiteral("0") ||
		       value.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0;
	};
	m_wrapInput                      = isEnabled(wrapInput);
	const QString showBold           = attrs.value(QStringLiteral("show_bold"));
	m_showBold                       = !isDisabled(showBold);
	const QString showItalic         = attrs.value(QStringLiteral("show_italic"));
	m_showItalic                     = !isDisabled(showItalic);
	const QString showUnderline      = attrs.value(QStringLiteral("show_underline"));
	m_showUnderline                  = !isDisabled(showUnderline);
	const QString alternativeInverse = attrs.value(QStringLiteral("alternative_inverse"));
	m_alternativeInverse             = isEnabled(alternativeInverse);
	bool lineSpacingOk               = false;
	int  lineSpacing                 = attrs.value(QStringLiteral("line_spacing")).toInt(&lineSpacingOk);
	if (!lineSpacingOk || lineSpacing < 0)
		lineSpacing = 0;
	m_lineSpacing      = lineSpacing;
	bool fadeAfterOk   = false;
	bool fadeOpacityOk = false;
	bool fadeSecondsOk = false;
	int  fadeAfter     = attrs.value(QStringLiteral("fade_output_buffer_after_seconds")).toInt(&fadeAfterOk);
	int  fadeOpacity   = attrs.value(QStringLiteral("fade_output_opacity_percent")).toInt(&fadeOpacityOk);
	int  fadeSeconds   = attrs.value(QStringLiteral("fade_output_seconds")).toInt(&fadeSecondsOk);
	if (!fadeAfterOk || fadeAfter < 0)
		fadeAfter = 0;
	if (!fadeOpacityOk)
		fadeOpacity = 100;
	fadeOpacity = qBound(0, fadeOpacity, 100);
	if (!fadeSecondsOk || fadeSeconds <= 0)
		fadeSeconds = 1;
	m_fadeOutputBufferAfterSeconds = fadeAfter;
	m_fadeOutputOpacityPercent     = fadeOpacity;
	m_fadeOutputSeconds            = fadeSeconds;
	if (m_fadeTimer)
	{
		if (m_fadeOutputBufferAfterSeconds > 0)
		{
			if (!m_fadeTimer->isActive())
				m_fadeTimer->start(1000);
		}
		else
			m_fadeTimer->stop();
	}
	m_inputPixelOffset = attrs.value(QStringLiteral("pixel_offset")).toInt();

	const QString displayInput    = attrs.value(QStringLiteral("display_my_input"));
	m_displayMyInput              = isEnabled(displayInput);
	const QString lineInformation = attrs.value(QStringLiteral("line_information"));
	m_lineInformation             = isEnabled(lineInformation);

	const QString escapeDeletes          = attrs.value(QStringLiteral("escape_deletes_input"));
	m_escapeDeletesInput                 = isEnabled(escapeDeletes);
	const QString saveDeleted            = attrs.value(QStringLiteral("save_deleted_command"));
	m_saveDeletedCommand                 = isEnabled(saveDeleted);
	const QString confirmPaste           = attrs.value(QStringLiteral("confirm_on_paste"));
	m_confirmOnPaste                     = isEnabled(confirmPaste);
	const QString ctrlBackspace          = attrs.value(QStringLiteral("ctrl_backspace_deletes_last_word"));
	m_ctrlBackspaceDeletesLastWord       = isEnabled(ctrlBackspace);
	const QString arrowsHistory          = attrs.value(QStringLiteral("arrows_change_history"));
	m_arrowsChangeHistory                = isEnabled(arrowsHistory);
	const QString arrowWrap              = attrs.value(QStringLiteral("arrow_keys_wrap"));
	m_arrowKeysWrap                      = isEnabled(arrowWrap);
	const QString arrowPartial           = attrs.value(QStringLiteral("arrow_recalls_partial"));
	m_arrowRecallsPartial                = isEnabled(arrowPartial);
	const QString altArrowPartial        = attrs.value(QStringLiteral("alt_arrow_recalls_partial"));
	m_altArrowRecallsPartial             = isEnabled(altArrowPartial);
	const QString ctrlZToEnd             = attrs.value(QStringLiteral("ctrl_z_goes_to_end_of_buffer"));
	m_ctrlZGoesToEndOfBuffer             = isEnabled(ctrlZToEnd);
	const QString ctrlPToPrev            = attrs.value(QStringLiteral("ctrl_p_goes_to_previous_command"));
	m_ctrlPGoesToPreviousCommand         = isEnabled(ctrlPToPrev);
	const QString ctrlNToNext            = attrs.value(QStringLiteral("ctrl_n_goes_to_next_command"));
	m_ctrlNGoesToNextCommand             = isEnabled(ctrlNToNext);
	const QString confirmReplace         = attrs.value(QStringLiteral("confirm_before_replacing_typing"));
	m_confirmBeforeReplacingTyping       = isEnabled(confirmReplace);
	const QString doubleClickInserts     = attrs.value(QStringLiteral("double_click_inserts"));
	m_doubleClickInserts                 = isEnabled(doubleClickInserts);
	const QString doubleClickSends       = attrs.value(QStringLiteral("double_click_sends"));
	m_doubleClickSends                   = isEnabled(doubleClickSends);
	const QString autoRepeat             = attrs.value(QStringLiteral("auto_repeat"));
	m_autoRepeat                         = isEnabled(autoRepeat);
	const QString lowerCaseTabCompletion = attrs.value(QStringLiteral("lower_case_tab_completion"));
	m_lowerCaseTabCompletion             = isEnabled(lowerCaseTabCompletion);
	const QString tabCompletionSpace     = attrs.value(QStringLiteral("tab_completion_space"));
	m_tabCompletionSpace                 = isEnabled(tabCompletionSpace);
	bool tabLinesOk                      = false;
	int  tabLines = attrs.value(QStringLiteral("tab_completion_lines")).toInt(&tabLinesOk);
	if (!tabLinesOk || tabLines < 1)
		tabLines = 200;
	m_tabCompletionLines    = tabLines;
	m_tabCompletionDefaults = multilineAttrs.value(QStringLiteral("tab_completion_defaults"));
	if (m_tabCompletionDefaults.isEmpty())
		m_tabCompletionDefaults = attrs.value(QStringLiteral("tab_completion_defaults"));
	const QString autoResize  = attrs.value(QStringLiteral("auto_resize_command_window"));
	m_autoResizeCommandWindow = isEnabled(autoResize);
	bool minOk                = false;
	bool maxOk                = false;
	int  minLines             = attrs.value(QStringLiteral("auto_resize_minimum_lines")).toInt(&minOk);
	int  maxLines             = attrs.value(QStringLiteral("auto_resize_maximum_lines")).toInt(&maxOk);
	if (!minOk || minLines <= 0)
		minLines = 1;
	if (!maxOk || maxLines <= 0)
		maxLines = 20;
	m_autoResizeMinimumLines        = minLines;
	m_autoResizeMaximumLines        = maxLines;
	const QString keepCommands      = attrs.value(QStringLiteral("keep_commands_on_same_line"));
	m_keepCommandsOnSameLine        = isEnabled(keepCommands);
	const QString noEchoOff         = attrs.value(QStringLiteral("no_echo_off"));
	m_noEchoOff                     = isEnabled(noEchoOff);
	const QString alwaysRecord      = attrs.value(QStringLiteral("always_record_command_history"));
	m_alwaysRecordCommandHistory    = isEnabled(alwaysRecord);
	const QString hyperlinkHistory  = attrs.value(QStringLiteral("hyperlink_adds_to_command_history"));
	m_hyperlinkAddsToCommandHistory = isEnabled(hyperlinkHistory);
	const QString useCustomLink     = attrs.value(QStringLiteral("use_custom_link_colour"));
	m_useCustomLinkColour           = isEnabled(useCustomLink);
	const QString underlineLinks    = attrs.value(QStringLiteral("underline_hyperlinks"));
	m_underlineHyperlinks           = isEnabled(underlineLinks);
	const QColor linkColour         = parseColor(attrs.value(QStringLiteral("hyperlink_colour")));
	if (linkColour.isValid())
		m_hyperlinkColour = linkColour;
	if (m_noEchoOff)
		m_noEcho = false;
	m_historyLimit = attrs.value(QStringLiteral("history_lines")).toInt();
	if (m_historyLimit < 0)
		m_historyLimit = 0;
	if (m_historyLimit > 0 && m_history.size() > m_historyLimit)
		m_history.remove(0, m_history.size() - m_historyLimit);
	if (m_outputDocument)
	{
		bool ok             = false;
		int  maxOutputLines = attrs.value(QStringLiteral("max_output_lines")).toInt(&ok);
		m_outputDocument->setMaximumBlockCount(maxOutputLines > 0 ? maxOutputLines : 0);

		QString css = QStringLiteral("a { text-decoration: %1;")
		                  .arg(m_underlineHyperlinks ? QStringLiteral("underline") : QStringLiteral("none"));
		if (m_useCustomLinkColour && m_hyperlinkColour.isValid())
			css += QStringLiteral(" color: %1;").arg(m_hyperlinkColour.name());
		css += QStringLiteral(" }");
		m_outputDocument->setDefaultStyleSheet(css);
	}

	m_autoPause                  = false;
	m_keepPauseAtBottom          = false;
	const QString autoPauseValue = attrs.value(QStringLiteral("auto_pause"));
	if (!autoPauseValue.isEmpty())
		m_autoPause = isEnabled(autoPauseValue);
	if (!m_autoPause)
		setScrollbackSplitActive(false);
	const QString keepPauseValue = attrs.value(QStringLiteral("keep_pause_at_bottom"));
	if (!keepPauseValue.isEmpty())
		m_keepPauseAtBottom = isEnabled(keepPauseValue);
	const QString startPausedValue = attrs.value(QStringLiteral("start_paused"));
	if (!m_startPausedApplied && isEnabled(startPausedValue))
	{
		setFrozen(true);
		m_startPausedApplied = true;
	}

	if (m_runtime)
		rebuildOutputFromLines(m_runtime->lines());

	updateInputWrap();
	updateInputHeight();
	applyDefaultInputHeight(false);
}

int WorldView::tooltipStartDelayMs() const
{
	if (!m_runtime)
		return 400;
	const QString value  = m_runtime->worldAttributes().value(QStringLiteral("tool_tip_start_time"));
	bool          ok     = false;
	int           parsed = value.toInt(&ok);
	if (!ok || parsed < 0)
		parsed = 400;
	return parsed;
}

int WorldView::tooltipVisibleDurationMs() const
{
	if (!m_runtime)
		return 5000;
	const QString value  = m_runtime->worldAttributes().value(QStringLiteral("tool_tip_visible_time"));
	bool          ok     = false;
	int           parsed = value.toInt(&ok);
	if (!ok || parsed <= 0)
		parsed = 5000;
	return parsed;
}

void WorldView::scheduleHotspotTooltip(const QString &hotspotId, const QString &tooltipText,
                                       const QPoint &globalPos)
{
	if (tooltipText.isEmpty())
		return;

	if (m_tooltipTimer)
		m_tooltipTimer->stop();

	m_pendingTooltipHotspot   = hotspotId;
	m_pendingTooltipText      = tooltipText;
	m_pendingTooltipGlobalPos = globalPos;

	const int delay = tooltipStartDelayMs();
	if (delay <= 0)
	{
		showScheduledHotspotTooltip();
		return;
	}

	if (m_tooltipTimer)
		m_tooltipTimer->start(delay);
}

void WorldView::showScheduledHotspotTooltip()
{
	if (m_pendingTooltipText.isEmpty())
		return;

	const int duration = tooltipVisibleDurationMs();
	QToolTip::showText(m_pendingTooltipGlobalPos, m_pendingTooltipText, this, QRect(), duration);
	m_tooltipHotspot = m_pendingTooltipHotspot;
	m_pendingTooltipHotspot.clear();
	m_pendingTooltipText.clear();
}

void WorldView::clearPendingHotspotTooltip()
{
	if (m_tooltipTimer)
		m_tooltipTimer->stop();
	m_pendingTooltipHotspot.clear();
	m_pendingTooltipText.clear();
}

double WorldView::lineOpacityForTimestamp(const QDateTime &when) const
{
	if (m_fadeOutputBufferAfterSeconds <= 0 || m_fadeOutputSeconds <= 0 || m_frozen)
		return 1.0;
	if (!when.isValid())
		return 1.0;

	const QDateTime now              = QDateTime::currentDateTime();
	qint64          timeSinceArrived = when.secsTo(now);
	if (timeSinceArrived < 0)
		timeSinceArrived = 0;

	if (m_timeFadeCancelled.isValid())
	{
		const qint64 cancelled   = m_timeFadeCancelled.secsTo(now);
		const auto   resetWindow = static_cast<qint64>(m_fadeOutputBufferAfterSeconds) + m_fadeOutputSeconds;
		if (cancelled >= 0 && cancelled < resetWindow && when <= m_timeFadeCancelled)
		{
			timeSinceArrived = cancelled;
		}
	}

	if (timeSinceArrived <= m_fadeOutputBufferAfterSeconds)
		return 1.0;

	const double fadeLimit   = qBound(0.0, static_cast<double>(m_fadeOutputOpacityPercent) / 100.0, 1.0);
	const qint64 fadeElapsed = timeSinceArrived - m_fadeOutputBufferAfterSeconds;
	if (fadeElapsed >= m_fadeOutputSeconds)
		return fadeLimit;

	const double progress = static_cast<double>(fadeElapsed) / static_cast<double>(m_fadeOutputSeconds);
	const double opacity  = 1.0 - ((1.0 - fadeLimit) * progress);
	return qBound(fadeLimit, opacity, 1.0);
}

bool WorldView::fadeRebuildNeededNow() const
{
	if (!m_runtime || m_fadeOutputBufferAfterSeconds <= 0 || m_fadeOutputSeconds <= 0 || m_frozen)
		return false;

	const QVector<WorldRuntime::LineEntry> &lines = m_runtime->lines();
	if (lines.isEmpty())
		return false;

	const QDateTime now        = QDateTime::currentDateTime();
	const qint64    lowerBound = m_fadeOutputBufferAfterSeconds;
	const auto      upperBound = static_cast<qint64>(m_fadeOutputBufferAfterSeconds) + m_fadeOutputSeconds;

	for (const WorldRuntime::LineEntry &line : lines)
	{
		if (!line.time.isValid())
			continue;

		qint64 timeSinceArrived = line.time.secsTo(now);
		if (timeSinceArrived < 0)
			timeSinceArrived = 0;

		if (m_timeFadeCancelled.isValid())
		{
			const qint64 cancelled = m_timeFadeCancelled.secsTo(now);
			if (cancelled >= 0 && cancelled < upperBound && line.time <= m_timeFadeCancelled)
			{
				timeSinceArrived = cancelled;
			}
		}

		if (timeSinceArrived > lowerBound && timeSinceArrived < upperBound)
			return true;
	}

	return false;
}

void WorldView::updateLineInformationTooltip(const QWidget *watched, const QMouseEvent *event)
{
	auto hideLineInfoTooltip = [this]
	{
		clearPendingHotspotTooltip();
		if (m_tooltipHotspot == kLineInfoTooltipId)
		{
			QToolTip::hideText();
			m_tooltipHotspot.clear();
		}
	};

	if (!m_lineInformation || !m_runtime || !event)
	{
		hideLineInfoTooltip();
		return;
	}

	WrapTextBrowser *view = nullptr;
	if (watched == m_output || watched == (m_output ? m_output->viewport() : nullptr))
		view = m_output;
	else if (watched == m_liveOutput || watched == (m_liveOutput ? m_liveOutput->viewport() : nullptr))
		view = m_liveOutput;
	if (!view || !view->document())
		return;

	QPoint posInView;
	if (watched == view)
		posInView = view->viewport()->mapFrom(view, event->position().toPoint());
	else if (watched == view->viewport())
		posInView = event->position().toPoint();
	else
		posInView = view->viewport()->mapFromGlobal(event->globalPosition().toPoint());

	QAbstractTextDocumentLayout *documentLayout = view->document()->documentLayout();
	if (!documentLayout)
	{
		hideLineInfoTooltip();
		return;
	}

	const QPointF documentPos(posInView.x() + view->horizontalScrollBar()->value(),
	                          posInView.y() + view->verticalScrollBar()->value());
	const QSizeF  documentSize = documentLayout->documentSize();
	if (documentPos.y() < 0.0 || documentPos.y() >= documentSize.height())
	{
		hideLineInfoTooltip();
		return;
	}

	const int hitPos = documentLayout->hitTest(documentPos, Qt::ExactHit);
	if (hitPos < 0)
	{
		// Avoid nearest-block fallback from cursorForPosition() when pointer is over blank area.
		hideLineInfoTooltip();
		return;
	}

	QTextCursor cursor(view->document());
	cursor.setPosition(hitPos);
	if (!cursor.block().isValid())
	{
		hideLineInfoTooltip();
		return;
	}

	const int                               lineIndex = cursor.block().blockNumber();
	const QVector<WorldRuntime::LineEntry> &lines     = m_runtime->lines();
	if (lineIndex < 0 || lineIndex >= lines.size())
	{
		hideLineInfoTooltip();
		return;
	}

	const WorldRuntime::LineEntry &line = lines.at(lineIndex);
	const QString when = line.time.isValid() ? line.time.toString(QStringLiteral("dddd, MMMM dd, HH:mm:ss"))
	                                         : QStringLiteral("(unknown time)");
	QString       suffix;
	if (line.flags & WorldRuntime::LineNote)
		suffix = QStringLiteral(", (note)");
	else if (line.flags & WorldRuntime::LineInput)
		suffix = QStringLiteral(", (input)");
	const QString text = QStringLiteral("Line %1, %2%3").arg(lineIndex + 1).arg(when).arg(suffix);
	scheduleHotspotTooltip(kLineInfoTooltipId, text, event->globalPosition().toPoint());
}

void WorldView::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	updateWrapMargin();
	updateInputWrap();
	if (m_scrollbackSplitActive && m_outputSplitter)
	{
		const QList<int> sizes = m_outputSplitter->sizes();
		if (sizes.size() >= 2)
			m_lastLiveSplitSize = sizes.at(1);
	}
	refreshMiniWindows();
	if (m_runtime)
	{
		// Preserve timing: plugins install only once the output area is actually sized/visible.
		m_runtime->installPendingPlugins();
		m_runtime->notifyWorldOutputResized();
	}
	requestDrawOutputWindowNotification();
}

void WorldView::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	applyDefaultInputHeight(true);
	if (m_runtime)
		m_runtime->installPendingPlugins();
}

void WorldView::mouseMoveEvent(QMouseEvent *event)
{
	if (m_mouseCaptured)
	{
		handleMiniWindowMouseMove(event, this);
		event->accept();
		return;
	}
	QWidget::mouseMoveEvent(event);
}

void WorldView::mouseReleaseEvent(QMouseEvent *event)
{
	if (m_mouseCaptured)
	{
		handleMiniWindowMouseRelease(event, this);
		event->accept();
		return;
	}
	QWidget::mouseReleaseEvent(event);
}

bool WorldView::eventFilter(QObject *watched, QEvent *event)
{
	const QWidget *outputViewport     = m_output ? m_output->viewport() : nullptr;
	const QWidget *liveOutputViewport = m_liveOutput ? m_liveOutput->viewport() : nullptr;
	const bool     isOutputWidget     = watched == m_outputContainer || watched == m_outputStack ||
	                            watched == m_outputSplitter || watched == m_outputScrollBar ||
	                            watched == m_output || watched == m_liveOutput || watched == outputViewport ||
	                            watched == liveOutputViewport;

	if ((watched == m_outputContainer || watched == m_outputStack || watched == m_outputSplitter) &&
	    (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest))
	{
		refreshMiniWindows();
		if (m_runtime)
			m_runtime->notifyWorldOutputResized();
	}

	if (m_allTypingToCommandWindow && isOutputWidget && event->type() == QEvent::KeyPress && m_input)
	{
		if (const auto *keyEvent = dynamic_cast<QKeyEvent *>(event))
		{
			QKeyEvent forwarded(keyEvent->type(), keyEvent->key(), keyEvent->modifiers(), keyEvent->text(),
			                    keyEvent->isAutoRepeat(), keyEvent->count());
			m_input->setFocus(Qt::OtherFocusReason);
			QCoreApplication::sendEvent(m_input, &forwarded);
			return true;
		}
	}

	if (isOutputWidget && event->type() == QEvent::KeyPress)
	{
		if (auto *keyEvent = dynamic_cast<QKeyEvent *>(event); keyEvent && handleWorldHotkey(keyEvent))
		{
			event->accept();
			return true;
		}
	}

	if (event->type() == QEvent::Wheel)
	{
		if (auto *wheel = dynamic_cast<QWheelEvent *>(event))
		{
			if (isOutputWidget && handleMiniWindowWheel(wheel, qobject_cast<QWidget *>(watched)))
			{
				event->accept();
				return true;
			}

			if (watched == m_outputContainer || watched == m_outputStack || watched == m_outputSplitter ||
			    watched == m_outputScrollBar)
			{
				handleOutputWheel(wheel->angleDelta(), wheel->pixelDelta());
				event->accept();
				return true;
			}
		}
	}

	if (isOutputWidget)
	{
		if (event->type() == QEvent::MouseMove)
			refreshHoveredHyperlinkFromCursor();

		bool handled = false;
		switch (event->type())
		{
		case QEvent::MouseMove:
			if (auto *mouseEvent = dynamic_cast<QMouseEvent *>(event))
				handled = handleMiniWindowMouseMove(mouseEvent, qobject_cast<QWidget *>(watched));
			break;
		case QEvent::MouseButtonPress:
			if (auto *mouseEvent = dynamic_cast<QMouseEvent *>(event))
				handled = handleMiniWindowMousePress(mouseEvent, false, qobject_cast<QWidget *>(watched));
			break;
		case QEvent::MouseButtonRelease:
			if (auto *mouseEvent = dynamic_cast<QMouseEvent *>(event))
				handled = handleMiniWindowMouseRelease(mouseEvent, qobject_cast<QWidget *>(watched));
			break;
		case QEvent::MouseButtonDblClick:
			if (auto *mouseEvent = dynamic_cast<QMouseEvent *>(event))
				handled = handleMiniWindowMousePress(mouseEvent, true, qobject_cast<QWidget *>(watched));
			break;
		case QEvent::Leave:
			handleMiniWindowMouseLeave();
			applyHoveredHyperlink(QString());
			if (m_tooltipHotspot == kLineInfoTooltipId)
			{
				QToolTip::hideText();
				m_tooltipHotspot.clear();
			}
			break;
		case QEvent::ContextMenu:
		{
			if (const auto *contextEvent = dynamic_cast<QContextMenuEvent *>(event))
			{
				const QPoint local =
				    mapEventToOutputStack(QPointF(contextEvent->pos()), qobject_cast<QWidget *>(watched));
				if (m_outputStack && m_outputStack->rect().contains(local))
				{
					QString hotspotId;
					QString windowName;
					if (const auto *window = hitTestMiniWindow(local, hotspotId, windowName, true); window)
						handled = true;
				}
			}
		}
		break;
		default:
			break;
		}

		if (!handled && event->type() == QEvent::MouseMove)
		{
			if (auto *mouseEvent = dynamic_cast<QMouseEvent *>(event))
				updateLineInformationTooltip(qobject_cast<QWidget *>(watched), mouseEvent);
		}

		if (!handled && event->type() == QEvent::MouseButtonDblClick)
		{
			if (auto *dblClick = dynamic_cast<QMouseEvent *>(event);
			    dblClick && dblClick->button() == Qt::LeftButton &&
			    (m_doubleClickSends || m_doubleClickInserts))
			{
				QTimer::singleShot(0, this,
				                   [this]
				                   {
					                   QString selected = outputSelectedText().replace(
					                       QChar::ParagraphSeparator, QLatin1Char(' '));
					                   selected = selected.trimmed();
					                   if (selected.isEmpty())
						                   return;

					                   if (m_doubleClickSends)
					                   {
						                   if (!m_runtime || !m_runtime->isConnected())
							                   return;
						                   emit sendText(selected);
						                   return;
					                   }

					                   if (m_doubleClickInserts && m_input)
					                   {
						                   m_input->insertPlainText(selected);
						                   m_inputChanged = true;
					                   }
				                   });
			}
		}

		if (handled)
		{
			event->accept();
			return true;
		}
	}

	return QWidget::eventFilter(watched, event);
}

void WorldView::updateWrapMargin() const
{
	auto applyMargin = [this](WrapTextBrowser *view)
	{
		if (!view)
			return;

		if (m_wrapColumn <= 0)
		{
			view->setViewportMarginsPublic(0, 0, 0, 0);
			return;
		}

		const QFontMetrics metrics(view->font());
		const int          desiredWidth  = metrics.horizontalAdvance(QLatin1Char('M')) * m_wrapColumn;
		const int          viewportWidth = view->viewport()->width();
		int                rightMargin   = 0;
		if (desiredWidth > 0 && viewportWidth > desiredWidth)
			rightMargin = viewportWidth - desiredWidth;
		view->setViewportMarginsPublic(0, 0, rightMargin, 0);
	};

	applyMargin(m_output);
	applyMargin(m_liveOutput);
}

void WorldView::updateInputWrap() const
{
	if (!m_input)
		return;

	if (!m_wrapInput || m_wrapColumn <= 0)
	{
		m_input->setLineWrapMode(QPlainTextEdit::NoWrap);
		m_input->setWordWrapMode(QTextOption::NoWrap);
		m_input->setViewportMarginsPublic(m_inputPixelOffset, 0, m_inputPixelOffset, 0);
		return;
	}

	int iWidth = m_wrapColumn + 1;
	if (iWidth < 20)
		iWidth = 20;
	if (iWidth > MAX_LINE_WIDTH)
		iWidth = MAX_LINE_WIDTH;

	const int charWidth     = QFontMetrics(m_input->font()).horizontalAdvance(QLatin1Char('M'));
	const int desiredWidth  = iWidth * charWidth;
	const int viewportWidth = m_input->viewport()->width();
	int       rightMargin   = viewportWidth - desiredWidth - m_inputPixelOffset;
	if (rightMargin < m_inputPixelOffset)
		rightMargin = m_inputPixelOffset;

	m_input->setViewportMarginsPublic(m_inputPixelOffset, 0, rightMargin, 0);
	m_input->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	m_input->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
}

void WorldView::updateInputHeight() const
{
	if (!m_input)
		return;

	const int frame      = m_input->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, m_input);
	const int lineHeight = QFontMetrics(m_input->font()).lineSpacing();
	const int singleLine = lineHeight + frame * 2 + 4;
	if (!m_autoResizeCommandWindow)
	{
		m_input->setMinimumHeight(singleLine);
		m_input->setMaximumHeight(QWIDGETSIZE_MAX);
		return;
	}

	int minLines = qMax(1, m_autoResizeMinimumLines);
	int maxLines = qMax(1, m_autoResizeMaximumLines);
	if (minLines > maxLines)
		qSwap(minLines, maxLines);

	int lineCount = 1;
	if (QTextDocument *doc = m_input->document())
		lineCount = qMax(1, doc->blockCount());
	const int targetLines  = qBound(minLines, lineCount, maxLines);
	const int targetHeight = (lineHeight * targetLines) + frame * 2 + 4;
	m_input->setMinimumHeight(targetHeight);
	m_input->setMaximumHeight(targetHeight);
}

void WorldView::applyDefaultInputHeight(bool setSplitterSizes)
{
	if (!m_input || m_defaultInputHeightApplied)
		return;

	const int frame      = m_input->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, m_input);
	const int lineHeight = QFontMetrics(m_input->font()).lineSpacing();
	const int singleLine = lineHeight + frame * 2 + 4;

	m_input->setMinimumHeight(singleLine);
	m_input->setMaximumHeight(singleLine);

	if (setSplitterSizes && m_splitter)
	{
		const int total = m_splitter->size().height();
		if (total > 0)
		{
			const int outputSize = qMax(0, total - singleLine);
			m_splitter->setSizes(QList<int>() << outputSize << singleLine);
			m_input->setMaximumHeight(QWIDGETSIZE_MAX);
			m_defaultInputHeightApplied = true;
			return;
		}
	}

	m_input->setMaximumHeight(QWIDGETSIZE_MAX);
}

void WorldView::addToHistory(const QString &text)
{
	if (m_noEcho && !m_alwaysRecordCommandHistory)
		return;
	if (m_historyLimit <= 0)
		return;

	const QString trimmed = text.trimmed();
	if (trimmed.isEmpty())
		return;
	if (text == m_lastCommand)
		return;

	m_history.append(text);
	if (m_history.size() > m_historyLimit)
		m_history.remove(0, m_history.size() - m_historyLimit);
	m_lastCommand = text;
}

void WorldView::addToHistoryForced(const QString &text)
{
	const bool savedNoEcho = m_noEcho;
	m_noEcho               = false;
	addToHistory(text);
	m_noEcho = savedNoEcho;
}

void WorldView::recallNextCommand()
{
	recallHistory(1);
}

void WorldView::recallPreviousCommand()
{
	recallHistory(-1);
}

void WorldView::repeatLastCommand()
{
	if (!m_input || m_lastCommand.isEmpty())
		return;
	if (!confirmReplaceTyping(m_lastCommand))
		return;
	setInputText(m_lastCommand, true);
}

void WorldView::recallLastWord()
{
	if (!m_input)
		return;

	if (m_ctrlBackspaceDeletesLastWord)
	{
		QTextCursor cursor = m_input->textCursor();
		if (cursor.hasSelection())
		{
			cursor.removeSelectedText();
			m_input->setTextCursor(cursor);
			m_inputChanged = true;
			updateInputHeight();
			return;
		}

		const QString current = m_input->toPlainText();
		if (current.isEmpty())
			return;

		const int     position = cursor.position();
		QString       before   = current.left(position);
		const QString after    = current.mid(position);
		while (before.endsWith(QLatin1Char(' ')))
			before.chop(1);
		if (const qsizetype spacePos = before.lastIndexOf(QLatin1Char(' ')); spacePos < 0)
			before.clear();
		else
			before = before.left(spacePos);

		const QString updated = before + after;
		m_input->setPlainText(updated);
		cursor = m_input->textCursor();
		cursor.setPosition(sizeToInt(before.length()));
		m_input->setTextCursor(cursor);
		m_inputChanged = true;
		updateInputHeight();
		return;
	}

	if (m_history.isEmpty())
		return;

	QString line = m_history.last().trimmed();
	if (line.isEmpty())
		return;

	const qsizetype pos  = line.lastIndexOf(QLatin1Char(' '));
	const QString   word = line.mid(pos + 1);
	if (word.isEmpty())
		return;

	m_input->insertPlainText(word);
	m_inputChanged = true;
	updateInputHeight();
}

void WorldView::removeLastHistoryEntry()
{
	if (m_history.isEmpty())
		return;
	m_history.removeLast();
	m_lastCommand = m_history.isEmpty() ? QString() : m_history.last();
	resetHistoryRecall();
}

QString WorldView::inputText() const
{
	if (!m_input)
		return {};
	return m_input->toPlainText();
}

bool WorldView::confirmReplaceTyping(const QString &replacement)
{
	if (!m_confirmBeforeReplacingTyping)
		return true;

	if (!m_inputChanged || !m_input)
		return true;

	QString current = m_input->toPlainText();
	if (current.isEmpty())
		return true;

	constexpr int limit              = 200;
	QString       trimmedCurrent     = current;
	QString       trimmedReplacement = replacement;
	if (trimmedCurrent.size() > limit)
		trimmedCurrent = trimmedCurrent.left(limit) + QStringLiteral(" ...");
	if (trimmedReplacement.size() > limit)
		trimmedReplacement = trimmedReplacement.left(limit) + QStringLiteral(" ...");

	const int result =
	    QMessageBox::question(this, QStringLiteral("QMud"),
	                          QStringLiteral("Replace your typing of\n\n\"%1\"\n\nwith\n\n\"%2\"?")
	                              .arg(trimmedCurrent, trimmedReplacement),
	                          QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);

	if (result != QMessageBox::Ok)
	{
		resetHistoryRecall();
		return false;
	}

	if (m_saveDeletedCommand)
		addToHistory(current);

	return true;
}

bool WorldView::executeMacroByName(const QString &name)
{
	if (!m_runtime || name.isEmpty())
		return false;

	const QList<WorldRuntime::Macro> &macros = m_runtime->macros();
	const WorldRuntime::Macro        *macro  = nullptr;
	for (const auto &entry : macros)
	{
		const QString macroName = entry.attributes.value(QStringLiteral("name")).trimmed();
		if (macroName.compare(name, Qt::CaseInsensitive) == 0)
		{
			macro = &entry;
			break;
		}
	}
	if (!macro)
		return false;

	const QString send = macro->children.value(QStringLiteral("send"));
	if (send.isEmpty())
		return false;

	QString type = macro->attributes.value(QStringLiteral("type")).trimmed();
	if (type.isEmpty())
		type = QStringLiteral("replace");

	if (type == QStringLiteral("replace"))
	{
		if (!confirmReplaceTyping(send))
			return true;
		setInputText(send, true);
		return true;
	}
	if (type == QStringLiteral("send_now"))
	{
		const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
		const QString noHistoryFlag = attrs.value(QStringLiteral("do_not_add_macros_to_command_history"));
		const bool    noHistory     = (noHistoryFlag == QStringLiteral("1") ||
                                noHistoryFlag.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
                                noHistoryFlag.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
		m_runtime->setCurrentActionSource(WorldRuntime::eUserMacro);
		(void)m_runtime->sendCommand(send, true, false, true, !noHistory, true);
		m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
		return true;
	}
	if (type == QStringLiteral("insert"))
	{
		if (m_input)
		{
			m_input->insertPlainText(send);
			m_inputChanged = true;
			updateInputHeight();
		}
		return true;
	}

	return false;
}

bool WorldView::handleWorldHotkey(QKeyEvent *event)
{
	if (!m_runtime || !event)
		return false;

	const Qt::KeyboardModifiers mods           = event->modifiers();
	const bool                  hasShift       = (mods & Qt::ShiftModifier) != 0;
	const bool                  hasCtrl        = (mods & Qt::ControlModifier) != 0;
	const bool                  hasAlt         = (mods & Qt::AltModifier) != 0;
	const bool                  hasMeta        = (mods & Qt::MetaModifier) != 0;
	const bool                  isRepeat       = event->isAutoRepeat();
	const bool                  keypadModifier = (mods & Qt::KeypadModifier) != 0;
	if (!isRepeat)
	{
		m_keypadRepeatArmed = false;
		m_keypadRepeatQtKey = 0;
		m_keypadRepeatCtrl  = false;
	}
	const bool keypadRepeatFallback = !keypadModifier && isRepeat && m_keypadRepeatArmed &&
	                                  event->key() == m_keypadRepeatQtKey && hasCtrl == m_keypadRepeatCtrl &&
	                                  !hasMeta && !hasAlt && !hasShift;
	const bool keypad = keypadModifier || keypadRepeatFallback;

	auto       tryAccelerator = [&]() -> bool
	{
		if (hasMeta)
			return false;
		const int key = event->key();
		if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta ||
		    key == Qt::Key_unknown)
			return false;

		quint32 virt = AcceleratorUtils::kVirtKeyFlag | AcceleratorUtils::kNoInvertFlag;
		if (hasShift)
			virt |= AcceleratorUtils::kShiftFlag;
		if (hasCtrl)
			virt |= AcceleratorUtils::kControlFlag;
		if (hasAlt)
			virt |= AcceleratorUtils::kAltFlag;
		const quint16 keyCode = AcceleratorUtils::qtKeyToVirtualKey(static_cast<Qt::Key>(key), keypad);
		if (keyCode == 0)
			return false;
		const qint64 mapKey = (static_cast<qint64>(virt) << 16) | keyCode;

		const int    commandId = m_runtime->acceleratorCommandForKey(mapKey);
		if (commandId < 0)
			return false;

		m_runtime->setCurrentActionSource(WorldRuntime::eUserAccelerator);
		const bool ok = m_runtime->executeAcceleratorCommand(
		    commandId, AcceleratorUtils::acceleratorToString(virt, keyCode));
		m_runtime->setCurrentActionSource(WorldRuntime::eUnknownActionSource);
		return ok;
	};

	if (tryAccelerator())
	{
		event->accept();
		return true;
	}

	// 1) Macro hotkeys (F-keys and Alt+letter) as stored in macro names.
	if (!hasMeta)
	{
		QString   macroName;
		const int key = event->key();
		if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
		{
			if (!hasAlt)
			{
				macroName = QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
				if (hasShift == hasCtrl)
				{
					if (hasShift)
						macroName.clear();
				}
				else if (hasShift)
					macroName += QStringLiteral("+Shift");
				else
					macroName += QStringLiteral("+Ctrl");
			}
		}
		else if (hasAlt && !hasCtrl && !hasShift && key >= Qt::Key_A && key <= Qt::Key_Z)
		{
			macroName = QStringLiteral("Alt+%1").arg(QChar(static_cast<char>('A' + (key - Qt::Key_A))));
		}

		if (!macroName.isEmpty())
		{
			if ((event->key() == Qt::Key_F1 || event->key() == Qt::Key_F6) && !hasShift && !hasCtrl)
			{
				AppController *app     = AppController::instance();
				const bool     f1Macro = app && app->getGlobalOption(QStringLiteral("F1macro")).toInt() != 0;
				if (!f1Macro)
					macroName.clear();
			}
			if (!macroName.isEmpty() && executeMacroByName(macroName))
			{
				event->accept();
				return true;
			}
		}
	}

	// 2) World keypad mapping from world properties.
	if (keypad)
	{
		const QMap<QString, QString> &attrs         = m_runtime->worldAttributes();
		const QString                 enabled       = attrs.value(QStringLiteral("keypad_enable"));
		const bool                    keypadEnabled = (enabled == QStringLiteral("1") ||
                                    enabled.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
                                    enabled.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
		if (keypadEnabled)
		{
			QString token;
			switch (event->key())
			{
			case Qt::Key_0:
				token = QStringLiteral("0");
				break;
			case Qt::Key_1:
				token = QStringLiteral("1");
				break;
			case Qt::Key_2:
				token = QStringLiteral("2");
				break;
			case Qt::Key_3:
				token = QStringLiteral("3");
				break;
			case Qt::Key_4:
				token = QStringLiteral("4");
				break;
			case Qt::Key_5:
				token = QStringLiteral("5");
				break;
			case Qt::Key_6:
				token = QStringLiteral("6");
				break;
			case Qt::Key_7:
				token = QStringLiteral("7");
				break;
			case Qt::Key_8:
				token = QStringLiteral("8");
				break;
			case Qt::Key_9:
				token = QStringLiteral("9");
				break;
			case Qt::Key_Period:
			case Qt::Key_Comma:
				token = QStringLiteral(".");
				break;
			case Qt::Key_Slash:
				token = QStringLiteral("/");
				break;
			case Qt::Key_Asterisk:
				token = QStringLiteral("*");
				break;
			case Qt::Key_Minus:
				token = QStringLiteral("-");
				break;
			case Qt::Key_Plus:
				token = QStringLiteral("+");
				break;
			default:
				break;
			}
			if (!token.isEmpty() && !hasMeta && !hasAlt && !hasShift)
			{
				QString keyName = hasCtrl ? QStringLiteral("Ctrl+%1").arg(token) : token;
				const QList<WorldRuntime::Keypad> &entries = m_runtime->keypadEntries();
				for (const WorldRuntime::Keypad &entry : entries)
				{
					if (entry.attributes.value(QStringLiteral("name"))
					        .compare(keyName, Qt::CaseInsensitive) == 0)
					{
						QString send = entry.content;
						send.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
						send.replace(QLatin1Char('\r'), QLatin1Char('\n'));
						const QStringList lines = send.split(QLatin1Char('\n'));
						QString           normalized;
						for (const QString &line : lines)
						{
							if (!line.trimmed().isEmpty())
							{
								normalized = line;
								break;
							}
						}
						send = normalized;
						if (!send.isEmpty())
						{
							m_runtime->setCurrentActionSource(WorldRuntime::eUserKeypad);
							if (!isRepeat)
							{
								m_keypadRepeatArmed = true;
								m_keypadRepeatQtKey = event->key();
								m_keypadRepeatCtrl  = hasCtrl;
							}
							emit sendText(send);
							event->accept();
							return true;
						}
						break;
					}
				}
			}
		}
	}

	return false;
}

void WorldView::setInputText(const QString &text, bool markChanged)
{
	if (!m_input)
		return;
	m_settingText = true;
	m_input->setPlainText(text);
	QTextCursor cursor = m_input->textCursor();
	cursor.movePosition(QTextCursor::End);
	m_input->setTextCursor(cursor);
	m_settingText  = false;
	m_inputChanged = markChanged;
	updateInputHeight();
}

int WorldView::setCommandSelection(int first, int last) const
{
	if (!m_input)
		return eBadParameter;
	const QString current = m_input->toPlainText();
	const int     maxPos  = sizeToInt(current.size());
	const int     start   = qBound(0, first - 1, maxPos);
	const int     end     = qBound(0, last, maxPos);
	QTextCursor   cursor  = m_input->textCursor();
	cursor.setPosition(start);
	cursor.setPosition(end, QTextCursor::KeepAnchor);
	m_input->setTextCursor(cursor);
	return eOK;
}

int WorldView::setCommandWindowHeight(int height) const
{
	if (!m_input || !m_splitter)
		return eBadParameter;
	if (height < 0)
		return eBadParameter;
	const int total = m_splitter->size().height();
	if (total <= 0)
		return eBadParameter;
	const int outputSize = total - height;
	if (outputSize < 20)
		return eBadParameter;
	m_input->setMinimumHeight(0);
	m_input->setMaximumHeight(QWIDGETSIZE_MAX);
	m_splitter->setSizes(QList<int>() << outputSize << height);
	return eOK;
}

void WorldView::setWorldCursor(int cursorCode)
{
	const QCursor cursor = hotspotCursor(cursorCode);
	applyOutputCursor(&cursor);
}

void WorldView::recallPartialHistory(int direction)
{
	if (!m_input || m_history.isEmpty())
		return;

	if (m_inputChanged)
	{
		m_partialCommand = m_input->toPlainText();
		m_partialIndex   = (direction < 0) ? sizeToInt(m_history.size()) : -1;
	}

	if (m_partialCommand.isEmpty())
	{
		recallHistory(direction);
		return;
	}

	int index = m_partialIndex;
	while (true)
	{
		index += (direction < 0) ? -1 : 1;
		if (index < 0 || index >= m_history.size())
		{
			if (confirmReplaceTyping(QString()))
				setInputText(QString());
			m_partialCommand.clear();
			m_partialIndex = -1;
			return;
		}

		const QString candidate = m_history.at(index);
		if (!candidate.startsWith(m_partialCommand, Qt::CaseInsensitive))
			continue;
		if (candidate.compare(m_partialCommand, Qt::CaseInsensitive) == 0)
			continue;

		if (!confirmReplaceTyping(candidate))
			return;

		m_partialIndex = index;
		setInputText(candidate);
		return;
	}
}

void WorldView::recallHistory(int direction)
{
	if (!m_input || m_history.isEmpty())
		return;

	if (m_historyIndex == -1 && direction > 0)
	{
		if (m_arrowKeysWrap)
			m_historyIndex = 0;
		else
		{
			if (confirmReplaceTyping(QString()))
				setInputText(QString());
			return;
		}
	}

	const int lastIndex = sizeToInt(m_history.size()) - 1;
	if (direction < 0)
	{
		if (m_historyIndex < 0)
			m_historyIndex = lastIndex;
		else if (m_historyIndex > 0)
			m_historyIndex--;
		else
		{
			if (m_arrowKeysWrap)
				m_historyIndex = lastIndex;
			else
			{
				if (confirmReplaceTyping(QString()))
					setInputText(QString());
				m_historyIndex = -1;
				return;
			}
		}
	}
	else if (direction > 0)
	{
		if (m_historyIndex < 0)
			return;
		if (m_historyIndex >= lastIndex)
		{
			if (m_arrowKeysWrap)
				m_historyIndex = 0;
			else
			{
				if (confirmReplaceTyping(QString()))
					setInputText(QString());
				m_historyIndex = -1;
				return;
			}
		}
		else
			m_historyIndex++;
	}

	if (m_historyIndex >= 0 && m_historyIndex <= lastIndex)
	{
		const QString replacement = m_history.at(m_historyIndex);
		if (confirmReplaceTyping(replacement))
			setInputText(replacement);
	}
}

void WorldView::resetHistoryRecall()
{
	m_historyIndex = -1;
	m_partialIndex = -1;
	m_partialCommand.clear();
	m_inputChanged = false;
}

bool WorldView::tabCompleteOneLine(int startColumn, int endColumn, const QString &targetWordLower,
                                   const QString &line, bool insertSpace)
{
	if (!m_input || targetWordLower.isEmpty() || line.isEmpty())
		return false;

	int       i          = 0;
	const int lineLength = sizeToInt(line.size());
	while (i < lineLength)
	{
		while (i < lineLength && !line.at(i).isLetterOrNumber())
			++i;
		if (i >= lineLength)
			break;

		bool prefixMismatch = false;
		for (int j = 0; j < targetWordLower.size(); ++j)
		{
			const int index = i + j;
			if (index >= lineLength || line.at(index).toLower() != targetWordLower.at(j))
			{
				prefixMismatch = true;
				break;
			}
		}

		if (prefixMismatch)
		{
			while (i < lineLength && line.at(i).isLetterOrNumber())
				++i;
			continue;
		}

		int end = i;
		while (end < lineLength && !line.at(end).isSpace() && !m_wordDelimiters.contains(line.at(end)))
			++end;

		const int replacementLength = end - i;
		if (replacementLength > targetWordLower.size())
		{
			QString replacement = line.mid(i, replacementLength);
			if (m_lowerCaseTabCompletion)
				replacement = replacement.toLower();
			if (m_runtime)
				m_runtime->firePluginTabComplete(replacement);
			if (insertSpace)
				replacement += QLatin1Char(' ');

			QTextCursor cursor = m_input->textCursor();
			cursor.setPosition(startColumn);
			cursor.setPosition(endColumn, QTextCursor::KeepAnchor);
			cursor.insertText(replacement);
			m_input->setTextCursor(cursor);
			m_inputChanged = true;
			return true;
		}

		i = end;
	}

	return false;
}

bool WorldView::handleTabCompletionKeyPress()
{
	if (!m_input)
		return false;

	const QTextCursor cursor = m_input->textCursor();
	if (cursor.selectionStart() != cursor.selectionEnd())
		return false;

	const QString currentText = m_input->toPlainText();
	if (currentText.isEmpty())
		return false;

	const int endColumn = cursor.position();
	if (endColumn <= 0 || endColumn > currentText.size())
		return false;

	bool insertSpace = m_tabCompletionSpace;
	if (endColumn < currentText.size())
	{
		if (const QChar next = currentText.at(endColumn); !next.isSpace() && !m_wordDelimiters.contains(next))
			insertSpace = true;
	}

	int startColumn = endColumn - 1;
	while (startColumn >= 0)
	{
		if (const QChar ch = currentText.at(startColumn); ch.isSpace() || m_wordDelimiters.contains(ch))
			break;
		--startColumn;
	}
	++startColumn;

	if (startColumn >= endColumn)
		return false;

	const QString targetWordLower = currentText.mid(startColumn, endColumn - startColumn).toLower();
	if (targetWordLower.isEmpty())
		return false;

	if (tabCompleteOneLine(startColumn, endColumn, targetWordLower, m_tabCompletionDefaults, insertSpace))
		return true;

	if (!m_runtime)
		return false;

	const QVector<WorldRuntime::LineEntry> &lines   = m_runtime->lines();
	int                                     scanned = 0;
	for (int i = sizeToInt(lines.size()) - 1; i >= 0; --i)
	{
		if (++scanned > m_tabCompletionLines)
			break;
		if (tabCompleteOneLine(startColumn, endColumn, targetWordLower, lines.at(i).text, insertSpace))
			return true;
	}

	return false;
}

void InputTextEdit::keyPressEvent(QKeyEvent *event)
{
	if (m_view && m_view->handleWorldHotkey(event))
		return;

	if (m_view && event->key() == Qt::Key_Tab && event->modifiers() == Qt::NoModifier)
	{
		m_view->handleTabCompletionKeyPress();
		return;
	}

	const Qt::KeyboardModifiers modifiers  = event->modifiers();
	const bool enterWithoutActionModifiers = (modifiers == Qt::NoModifier || modifiers == Qt::KeypadModifier);

	if (m_view && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
	    enterWithoutActionModifiers)
	{
		QString text = toPlainText();
		if (m_view->m_runtime)
		{
			const QMap<QString, QString> &attrs     = m_view->m_runtime->worldAttributes();
			const auto                    isEnabled = [](const QString &value)
			{
				return value == QStringLiteral("1") ||
				       value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
				       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
			};
			const bool spellOnSend      = isEnabled(attrs.value(QStringLiteral("spell_check_on_send")));
			const bool scriptingEnabled = isEnabled(attrs.value(QStringLiteral("enable_scripts"))) &&
			                              attrs.value(QStringLiteral("script_language"))
			                                      .compare(QStringLiteral("Lua"), Qt::CaseInsensitive) == 0;
			const QString scriptPrefix = attrs.value(QStringLiteral("script_prefix"));
			const bool    scriptCommand =
			    scriptingEnabled && !scriptPrefix.isEmpty() && text.startsWith(scriptPrefix);
			if (spellOnSend && !scriptCommand)
			{
				if (AppController *app = AppController::instance())
					app->onCommandTriggered(QStringLiteral("SpellCheck"));
				text = toPlainText();
			}
		}
		emit m_view->sendText(text);
		if (m_view->m_autoRepeat)
		{
			m_view->setInputText(text);
			selectAll();
		}
		else
		{
			clear();
			m_view->m_inputChanged = false;
		}
		m_view->resetHistoryRecall();
		return;
	}

	if (event->matches(QKeySequence::Paste))
	{
		if (!m_view)
		{
			QPlainTextEdit::keyPressEvent(event);
			return;
		}
		const QString pasteText = QGuiApplication::clipboard()->text();
		insertPlainText(pasteText);
		return;
	}
	if (m_view && event->key() == Qt::Key_Backspace && (event->modifiers() & Qt::ControlModifier))
	{
		QTextCursor cursor = textCursor();
		if (cursor.hasSelection())
		{
			cursor.removeSelectedText();
			setTextCursor(cursor);
			return;
		}
		if (m_view->m_ctrlBackspaceDeletesLastWord)
		{
			const QString current  = toPlainText();
			const int     position = cursor.position();
			QString       before   = current.left(position);
			const QString after    = current.mid(position);
			while (before.endsWith(QLatin1Char(' ')))
				before.chop(1);
			if (const qsizetype spacePos = before.lastIndexOf(QLatin1Char(' ')); spacePos < 0)
				before.clear();
			else
				before = before.left(spacePos);
			const QString updated = before + after;
			setPlainText(updated);
			cursor = textCursor();
			cursor.setPosition(sizeToInt(before.length()));
			setTextCursor(cursor);
			return;
		}

		if (const int position = cursor.position(); position > 0)
		{
			QString current = toPlainText();
			current.remove(position - 1, 1);
			setPlainText(current);
			cursor = textCursor();
			cursor.setPosition(position - 1);
			setTextCursor(cursor);
		}
		return;
	}

	if (m_view && event->key() == Qt::Key_Escape)
	{
		if (m_view->m_escapeDeletesInput)
		{
			if (m_view->m_saveDeletedCommand)
				m_view->addToHistory(toPlainText());
			clear();
			m_view->resetHistoryRecall();
			return;
		}
	}

	if (m_view && (event->modifiers() & Qt::ControlModifier) &&
	    !(event->modifiers() & (Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier)))
	{
		if (event->key() == Qt::Key_N && m_view->m_ctrlNGoesToNextCommand)
		{
			m_view->recallNextCommand();
			return;
		}
		if (event->key() == Qt::Key_P && m_view->m_ctrlPGoesToPreviousCommand)
		{
			m_view->recallPreviousCommand();
			return;
		}
		if (event->key() == Qt::Key_Z && m_view->m_ctrlZGoesToEndOfBuffer)
		{
			m_view->scrollOutputToEnd();
			return;
		}
	}

	if (m_view && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down))
	{
		const int direction = (event->key() == Qt::Key_Up) ? -1 : 1;
		if (event->modifiers() == Qt::NoModifier)
		{
			if (m_view->m_arrowsChangeHistory)
			{
				if (m_view->m_arrowRecallsPartial &&
				    (m_view->m_inputChanged || !m_view->m_partialCommand.isEmpty()))
					m_view->recallPartialHistory(direction);
				else
					m_view->recallHistory(direction);
			}
			return;
		}
		if (event->modifiers() == Qt::AltModifier)
		{
			if (m_view->m_altArrowRecallsPartial || m_view->m_arrowRecallsPartial)
				m_view->recallPartialHistory(direction);
			return;
		}
	}

	QPlainTextEdit::keyPressEvent(event);
}

void InputTextEdit::contextMenuEvent(QContextMenuEvent *event)
{
	QMenu *menu = createStandardContextMenu();
	if (m_view && menu)
	{
		for (const QList<QAction *> actions = menu->actions(); QAction *action : actions)
		{
			if (action && action->shortcut() == QKeySequence::Paste)
			{
				disconnect(action, nullptr, nullptr, nullptr);
				connect(action, &QAction::triggered, this,
				        [this]
				        {
					        const QString pasteText = QGuiApplication::clipboard()->text();
					        insertPlainText(pasteText);
				        });
				break;
			}
		}
	}

	if (menu)
	{
		forceOpaqueMenu(menu);
		menu->exec(event->globalPos());
		delete menu;
		return;
	}

	QPlainTextEdit::contextMenuEvent(event);
}
