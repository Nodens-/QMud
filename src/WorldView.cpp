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
#include "AccessibleTextUtils.h"
#include "AppController.h"
#include "Environment.h"
#include "FontUtils.h"
#include "MainFrame.h"
#include "MainWindowHost.h"
#include "MainWindowHostResolver.h"
#include "MiniWindowUtils.h"
#include "Version.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "dialogs/CommandHistoryDialog.h"
#include "dialogs/FindDialog.h"
#include "scripting/ScriptingErrors.h"

#include <QAbstractScrollArea>
// ReSharper disable once CppUnusedIncludeDirective
#include <QAbstractTextDocumentLayout>
// ReSharper disable once CppUnusedIncludeDirective
#include <QAccessible>
#include <QAccessibleWidget>
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
// ReSharper disable once CppUnusedIncludeDirective
#include <QInputMethodEvent>
// ReSharper disable once CppUnusedIncludeDirective
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
// ReSharper disable once CppUnusedIncludeDirective
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QToolTip>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <ranges>

namespace
{
	constexpr const char *kWorldOutputAccessibleProperty = "qmud_world_output_widget";

	int                   sizeToInt(const qsizetype value)
	{
		constexpr qsizetype kMin = 0;
		constexpr qsizetype kMax = std::numeric_limits<int>::max();
		return static_cast<int>(qBound(kMin, value, kMax));
	}

	QString trimLeadingAnnouncementBreaks(QString text)
	{
		while (!text.isEmpty() && (text.front() == QLatin1Char('\n') || text.front() == QLatin1Char('\r')))
			text.remove(0, 1);
		return text;
	}

	QVector<int> prefixTableForText(const QString &text)
	{
		QVector<int> table(text.size(), 0);
		for (int i = 1; i < text.size(); ++i)
		{
			int candidate = table.at(i - 1);
			while (candidate > 0 && text.at(i) != text.at(candidate))
				candidate = table.at(candidate - 1);
			if (text.at(i) == text.at(candidate))
				++candidate;
			table[i] = candidate;
		}
		return table;
	}

	QString accessibleAnnouncementDelta(const QString &previousText, const QString &currentText)
	{
		if (previousText.isEmpty() || currentText.isEmpty() || previousText == currentText)
			return {};
		if (currentText.startsWith(previousText))
			return trimLeadingAnnouncementBreaks(currentText.mid(previousText.size()));

		const QVector<int> prefixTable = prefixTableForText(currentText);
		int                overlap     = 0;
		for (const QChar ch : previousText)
		{
			while (overlap > 0 && ch != currentText.at(overlap))
				overlap = prefixTable.at(overlap - 1);
			if (ch == currentText.at(overlap))
				++overlap;
			if (overlap == currentText.size())
				overlap = prefixTable.at(overlap - 1);
		}
		if (overlap <= 0 || overlap >= currentText.size())
			return {};
		return trimLeadingAnnouncementBreaks(currentText.mid(overlap));
	}

	void appendDeduplicatedHistoryEntry(QVector<QString> &history, const QString &entry,
	                                    const int historyLimit)
	{
		const auto newEnd = std::ranges::remove(history, entry).begin();
		history.erase(newEnd, history.end());
		history.append(entry);
		if (historyLimit > 0 && history.size() > historyLimit)
			history.remove(0, history.size() - historyLimit);
	}

	bool styleSpansEquivalent(const WorldRuntime::StyleSpan &lhs, const WorldRuntime::StyleSpan &rhs)
	{
		return lhs.length == rhs.length && lhs.fore == rhs.fore && lhs.back == rhs.back &&
		       lhs.bold == rhs.bold && lhs.underline == rhs.underline && lhs.italic == rhs.italic &&
		       lhs.blink == rhs.blink && lhs.strike == rhs.strike && lhs.inverse == rhs.inverse &&
		       lhs.changed == rhs.changed && lhs.actionType == rhs.actionType && lhs.action == rhs.action &&
		       lhs.hint == rhs.hint && lhs.variable == rhs.variable && lhs.startTag == rhs.startTag;
	}

	bool styleSpanVectorsEquivalent(const QVector<WorldRuntime::StyleSpan> &lhs,
	                                const QVector<WorldRuntime::StyleSpan> &rhs)
	{
		if (lhs.size() != rhs.size())
			return false;
		for (int i = 0; i < lhs.size(); ++i)
		{
			if (!styleSpansEquivalent(lhs.at(i), rhs.at(i)))
				return false;
		}
		return true;
	}

	bool hasNonPositiveStyleSpan(const QVector<WorldRuntime::StyleSpan> &spans)
	{
		return std::ranges::any_of(spans,
		                           [](const WorldRuntime::StyleSpan &span) { return span.length <= 0; });
	}

	void removeNonPositiveStyleSpans(QVector<WorldRuntime::StyleSpan> &spans)
	{
		if (spans.isEmpty() || !hasNonPositiveStyleSpan(spans))
			return;
		const auto newEnd = std::ranges::remove_if(spans, [](const WorldRuntime::StyleSpan &span)
		                                           { return span.length <= 0; })
		                        .begin();
		spans.erase(newEnd, spans.end());
	}

	void appendPositiveStyleSpans(QVector<WorldRuntime::StyleSpan> &target,
	                              QVector<WorldRuntime::StyleSpan> &source)
	{
		if (hasNonPositiveStyleSpan(source))
			removeNonPositiveStyleSpans(source);
		if (source.isEmpty())
			return;
		if (target.isEmpty())
		{
			target = std::move(source);
			return;
		}
		const qsizetype requiredCapacity = target.size() + source.size();
		if (target.capacity() < requiredCapacity)
		{
			const qsizetype growthCapacity = target.capacity() + qMax(source.size(), target.capacity() / 2);
			target.reserve(qMax(requiredCapacity, growthCapacity));
		}
		for (WorldRuntime::StyleSpan &span : source)
			target.push_back(std::move(span));
	}

	bool lineEntriesEquivalentForCache(const WorldRuntime::LineEntry &lhs, const WorldRuntime::LineEntry &rhs)
	{
		if (lhs.lineNumber != rhs.lineNumber || lhs.flags != rhs.flags || lhs.hardReturn != rhs.hardReturn ||
		    lhs.time != rhs.time || lhs.ticks != rhs.ticks || lhs.elapsed != rhs.elapsed ||
		    lhs.spans.size() != rhs.spans.size())
		{
			return false;
		}
		if (lhs.text != rhs.text)
		{
			return false;
		}
		for (int i = 0; i < lhs.spans.size(); ++i)
		{
			if (!styleSpansEquivalent(lhs.spans.at(i), rhs.spans.at(i)))
				return false;
		}
		return true;
	}

	bool runtimeLineNumbersAreContiguous(const QVector<WorldRuntime::LineEntry> &lines)
	{
		if (lines.isEmpty())
			return true;

		qint64 expected = lines.first().lineNumber;
		for (const WorldRuntime::LineEntry &entry : lines)
		{
			if (entry.lineNumber != expected)
				return false;
			++expected;
		}
		return true;
	}

	int findRuntimeLineIndexByNumberNear(const QVector<WorldRuntime::LineEntry> &runtimeLines,
	                                     const qint64 lineNumber, const int preferredIndex,
	                                     const bool searchBackwardFirst)
	{
		if (runtimeLines.isEmpty())
			return -1;

		const int boundedPreferredIndex = qBound(0, preferredIndex, sizeToInt(runtimeLines.size()) - 1);
		if (runtimeLines.at(boundedPreferredIndex).lineNumber == lineNumber)
			return boundedPreferredIndex;

		const qint64 firstLineNumber = runtimeLines.first().lineNumber;
		const qint64 denseCandidate  = lineNumber - firstLineNumber;
		if (denseCandidate >= 0 && denseCandidate < runtimeLines.size())
		{
			const int candidateIndex = static_cast<int>(denseCandidate);
			if (runtimeLines.at(candidateIndex).lineNumber == lineNumber)
				return candidateIndex;
		}

		if (runtimeLines.constFirst().lineNumber == lineNumber)
			return 0;
		if (runtimeLines.constLast().lineNumber == lineNumber)
			return sizeToInt(runtimeLines.size()) - 1;

		if (searchBackwardFirst)
		{
			for (int i = boundedPreferredIndex - 1; i >= 0; --i)
			{
				if (runtimeLines.at(i).lineNumber == lineNumber)
					return i;
			}
			for (int i = boundedPreferredIndex + 1; i < runtimeLines.size(); ++i)
			{
				if (runtimeLines.at(i).lineNumber == lineNumber)
					return i;
			}
			return -1;
		}

		for (int i = boundedPreferredIndex + 1; i < runtimeLines.size(); ++i)
		{
			if (runtimeLines.at(i).lineNumber == lineNumber)
				return i;
		}
		for (int i = boundedPreferredIndex - 1; i >= 0; --i)
		{
			if (runtimeLines.at(i).lineNumber == lineNumber)
				return i;
		}
		return -1;
	}

	quint64 hashCombine(const quint64 seed, const quint64 value)
	{
		return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
	}

	quint64 hashStringContent(const QString &text)
	{
		quint64      seed = 1469598103934665603ULL;
		const QChar *data = text.constData();
		for (qsizetype i = 0; i < text.size(); ++i)
		{
			seed ^= static_cast<quint64>(data[i].unicode());
			seed *= 1099511628211ULL;
		}
		return hashCombine(seed, static_cast<quint64>(text.size()));
	}

	bool isActionLinkType(const int actionType)
	{
		return actionType == WorldRuntime::ActionHyperlink || actionType == WorldRuntime::ActionSend ||
		       actionType == WorldRuntime::ActionPrompt;
	}

	void ensureCursorVisibleNowAndQueued(QPlainTextEdit *edit)
	{
		if (!edit)
			return;

		edit->ensureCursorVisible();
		if (!edit->viewport()->rect().intersects(edit->cursorRect()))
			edit->centerCursor();
		edit->viewport()->update();
		QPointer<QPlainTextEdit> guard(edit);
		QMetaObject::invokeMethod(
		    edit,
		    [guard]
		    {
			    if (!guard)
				    return;
			    guard->ensureCursorVisible();
			    if (!guard->viewport()->rect().intersects(guard->cursorRect()))
				    guard->centerCursor();
			    guard->viewport()->update();
		    },
		    Qt::QueuedConnection);
	}

	int inputVerticalChromePx(const QPlainTextEdit *edit, const QMargins &viewportMargins, const int frame)
	{
		if (!edit)
			return frame * 2;
		const QMargins contentsMargins = edit->contentsMargins();
		int            documentMargins = 0;
		if (const QTextDocument *doc = edit->document())
			documentMargins = qMax(0, qCeil(doc->documentMargin())) * 2;
		return frame * 2 + contentsMargins.top() + contentsMargins.bottom() + viewportMargins.top() +
		       viewportMargins.bottom() + documentMargins;
	}

	QString htmlStyleForSpan(const WorldRuntime::StyleSpan &span, const QColor &defaultTextColour,
	                         bool useCustomLinkColour, const QColor &hyperlinkColour, bool showBold,
	                         bool showItalic, bool showUnderline, bool alternativeInverse,
	                         bool underlineHyperlinks)
	{
		QColor foreground = span.fore;
		QColor background = span.back;
		if (span.inverse)
		{
			qSwap(foreground, background);
			if (alternativeInverse && span.bold)
				qSwap(foreground, background);
		}

		const bool hasLinkAction = isActionLinkType(span.actionType) && !span.action.isEmpty();
		if (!foreground.isValid())
		{
			if (hasLinkAction && useCustomLinkColour && hyperlinkColour.isValid())
				foreground = hyperlinkColour;
			else
				foreground = defaultTextColour;
		}

		QStringList styles;
		if (foreground.isValid())
			styles.push_back(QStringLiteral("color:%1").arg(foreground.name()));
		if (background.isValid())
			styles.push_back(QStringLiteral("background-color:%1").arg(background.name()));
		if (span.bold && showBold)
			styles.push_back(QStringLiteral("font-weight:bold"));
		if (span.italic && showItalic)
			styles.push_back(QStringLiteral("font-style:italic"));
		if (span.strike)
			styles.push_back(QStringLiteral("text-decoration-line:line-through"));
		const bool spanUnderline = span.underline && showUnderline;
		const bool linkUnderline = hasLinkAction && underlineHyperlinks;
		if (spanUnderline || linkUnderline)
			styles.push_back(QStringLiteral("text-decoration:underline"));

		return styles.join(QLatin1Char(';'));
	}

	[[nodiscard]] bool hasConfiguredTextRectangle(const WorldRuntime::TextRectangleSettings &settings)
	{
		return settings.left != 0 || settings.top != 0 || settings.right != 0 || settings.bottom != 0;
	}

	[[nodiscard]] QRect normalizeTextRectangleForClient(const WorldRuntime::TextRectangleSettings &settings,
	                                                    const QSize                               &clientSize)
	{
		const int clientWidth  = qMax(0, clientSize.width());
		const int clientHeight = qMax(0, clientSize.height());
		if (clientWidth <= 0 || clientHeight <= 0)
			return {};

		if (!hasConfiguredTextRectangle(settings))
			return {0, 0, clientWidth, clientHeight};

		int left   = settings.left;
		int top    = settings.top;
		int right  = settings.right;
		int bottom = settings.bottom;

		if (right <= 0)
		{
			right += clientWidth;
			right = qMax(right, left + 20);
		}

		if (bottom <= 0)
		{
			bottom += clientHeight;
			bottom = qMax(bottom, top + 20);
		}

		left   = qBound(0, left, clientWidth);
		top    = qBound(0, top, clientHeight);
		right  = qBound(left, right, clientWidth);
		bottom = qBound(top, bottom, clientHeight);
		return {left, top, right - left, bottom - top};
	}

	[[nodiscard]] QRect outputTextRectangleForClient(const QSize &clientSize, const WorldRuntime *runtime)
	{
		if (clientSize.width() <= 0 || clientSize.height() <= 0)
			return {};
		if (!runtime)
			return {
			    {0, 0},
                clientSize
            };
		return normalizeTextRectangleForClient(runtime->textRectangle(), clientSize);
	}

	[[nodiscard]] QImage transparentColorKeyedCopy(const QImage &source, const QRgb keyRgb)
	{
		if (source.isNull())
			return source;
		QImage image = source.convertToFormat(QImage::Format_ARGB32);
		image.setDevicePixelRatio(source.devicePixelRatio());
		for (int y = 0; y < image.height(); ++y)
		{
			auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
			for (int x = 0; x < image.width(); ++x)
			{
				if ((line[x] & 0x00FFFFFF) == (keyRgb & 0x00FFFFFF))
					line[x] &= 0x00FFFFFF;
			}
		}
		return image;
	}

	[[nodiscard]] QSize imageLogicalSize(const QImage &image)
	{
		if (image.isNull())
			return {};
		const QSizeF logicalSize = image.deviceIndependentSize();
		return {qMax(1, static_cast<int>(std::ceil(logicalSize.width()))),
		        qMax(1, static_cast<int>(std::ceil(logicalSize.height())))};
	}

	[[nodiscard]] bool traceOutputBackfillEnabled()
	{
		static const bool enabled = []
		{
			const QByteArray value = qgetenv("QMUD_TRACE_BACKFILL");
			if (value.isEmpty())
				return false;
			return value != "0" && value.compare("false", Qt::CaseInsensitive) != 0 &&
			       value.compare("n", Qt::CaseInsensitive) != 0;
		}();
		return enabled;
	}

	[[nodiscard]] bool nativeCanvasDiagnosticsEnabled()
	{
		static const bool enabled = []
		{
			const QByteArray value = qgetenv("QMUD_NATIVE_CANVAS_DIAGNOSTICS");
			if (!value.isEmpty())
			{
				return value != "0" && value.compare("false", Qt::CaseInsensitive) != 0 &&
				       value.compare("n", Qt::CaseInsensitive) != 0;
			}
			const QString appName = QCoreApplication::applicationName().trimmed();
			return appName.startsWith(QStringLiteral("tst_"), Qt::CaseInsensitive);
		}();
		return enabled;
	}

	[[nodiscard]] QString traceWorldName(const WorldRuntime *runtime)
	{
		if (!runtime)
			return QStringLiteral("<null>");
		QString name = runtime->worldAttributes().value(QStringLiteral("name")).trimmed();
		if (!name.isEmpty())
			return name;
		return QStringLiteral("<unnamed>");
	}

	const QSet<QString> &worldViewRuntimeSettingsAttributeKeys()
	{
		static const QSet<QString> keys = {QStringLiteral("output_font_name"),
		                                   QStringLiteral("input_font_name"),
		                                   QStringLiteral("output_font_height"),
		                                   QStringLiteral("input_font_height"),
		                                   QStringLiteral("output_font_weight"),
		                                   QStringLiteral("input_font_weight"),
		                                   QStringLiteral("input_font_italic"),
		                                   QStringLiteral("output_font_charset"),
		                                   QStringLiteral("input_font_charset"),
		                                   QStringLiteral("use_default_output_font"),
		                                   QStringLiteral("use_default_input_font"),
		                                   QStringLiteral("input_background_colour"),
		                                   QStringLiteral("input_text_colour"),
		                                   QStringLiteral("output_background_colour"),
		                                   QStringLiteral("output_text_colour"),
		                                   QStringLiteral("timestamp_output"),
		                                   QStringLiteral("timestamp_output_text_colour"),
		                                   QStringLiteral("timestamp_output_back_colour"),
		                                   QStringLiteral("timestamp_input"),
		                                   QStringLiteral("timestamp_input_text_colour"),
		                                   QStringLiteral("timestamp_input_back_colour"),
		                                   QStringLiteral("timestamp_notes"),
		                                   QStringLiteral("timestamp_notes_text_colour"),
		                                   QStringLiteral("timestamp_notes_back_colour"),
		                                   QStringLiteral("wrap_column"),
		                                   QStringLiteral("max_output_lines"),
		                                   QStringLiteral("wrap"),
		                                   QStringLiteral("auto_wrap_window_width"),
		                                   QStringLiteral("naws"),
		                                   QStringLiteral("wrap_input"),
		                                   QStringLiteral("show_bold"),
		                                   QStringLiteral("show_italic"),
		                                   QStringLiteral("show_underline"),
		                                   QStringLiteral("alternative_inverse"),
		                                   QStringLiteral("line_spacing"),
		                                   QStringLiteral("fade_output_buffer_after_seconds"),
		                                   QStringLiteral("fade_output_opacity_percent"),
		                                   QStringLiteral("fade_output_seconds"),
		                                   QStringLiteral("pixel_offset"),
		                                   QStringLiteral("display_my_input"),
		                                   QStringLiteral("line_information"),
		                                   QStringLiteral("escape_deletes_input"),
		                                   QStringLiteral("save_deleted_command"),
		                                   QStringLiteral("confirm_on_paste"),
		                                   QStringLiteral("ctrl_backspace_deletes_last_word"),
		                                   QStringLiteral("arrows_change_history"),
		                                   QStringLiteral("arrow_keys_wrap"),
		                                   QStringLiteral("arrow_recalls_partial"),
		                                   QStringLiteral("alt_arrow_recalls_partial"),
		                                   QStringLiteral("ctrl_z_goes_to_end_of_buffer"),
		                                   QStringLiteral("ctrl_p_goes_to_previous_command"),
		                                   QStringLiteral("ctrl_n_goes_to_next_command"),
		                                   QStringLiteral("confirm_before_replacing_typing"),
		                                   QStringLiteral("double_click_inserts"),
		                                   QStringLiteral("double_click_sends"),
		                                   QStringLiteral("auto_repeat"),
		                                   QStringLiteral("lower_case_tab_completion"),
		                                   QStringLiteral("tab_completion_space"),
		                                   QStringLiteral("tab_completion_lines"),
		                                   QStringLiteral("auto_resize_command_window"),
		                                   QStringLiteral("auto_resize_minimum_lines"),
		                                   QStringLiteral("auto_resize_maximum_lines"),
		                                   QStringLiteral("keep_commands_on_same_line"),
		                                   QStringLiteral("no_echo_off"),
		                                   QStringLiteral("always_record_command_history"),
		                                   QStringLiteral("hyperlink_adds_to_command_history"),
		                                   QStringLiteral("use_custom_link_colour"),
		                                   QStringLiteral("underline_hyperlinks"),
		                                   QStringLiteral("hyperlink_colour"),
		                                   QStringLiteral("history_lines"),
		                                   QStringLiteral("auto_pause"),
		                                   QStringLiteral("keep_pause_at_bottom"),
		                                   QStringLiteral("start_paused")};
		return keys;
	}

	const QSet<QString> &worldViewRuntimeSettingsMultilineAttributeKeys()
	{
		static const QSet<QString> keys = {QStringLiteral("tab_completion_defaults")};
		return keys;
	}

	const QSet<QString> &worldViewRuntimeSettingsRebuildAttributeKeys()
	{
		// Keep this set aligned with output-rendering branches in applyRuntimeSettingsImpl().
		// Keys listed here require rebuilding existing buffered output to avoid stale formatting/layout.
		static const QSet<QString> keys = {QStringLiteral("output_font_name"),
		                                   QStringLiteral("output_font_height"),
		                                   QStringLiteral("output_font_weight"),
		                                   QStringLiteral("output_font_charset"),
		                                   QStringLiteral("use_default_output_font"),
		                                   QStringLiteral("output_background_colour"),
		                                   QStringLiteral("output_text_colour"),
		                                   QStringLiteral("timestamp_output"),
		                                   QStringLiteral("timestamp_output_text_colour"),
		                                   QStringLiteral("timestamp_output_back_colour"),
		                                   QStringLiteral("timestamp_input"),
		                                   QStringLiteral("timestamp_input_text_colour"),
		                                   QStringLiteral("timestamp_input_back_colour"),
		                                   QStringLiteral("timestamp_notes"),
		                                   QStringLiteral("timestamp_notes_text_colour"),
		                                   QStringLiteral("timestamp_notes_back_colour"),
		                                   QStringLiteral("show_bold"),
		                                   QStringLiteral("show_italic"),
		                                   QStringLiteral("show_underline"),
		                                   QStringLiteral("alternative_inverse"),
		                                   QStringLiteral("line_spacing")};
		return keys;
	}

	const QSet<QString> &worldViewRuntimeSettingsRebuildMultilineAttributeKeys()
	{
		static const QSet<QString> keys = {};
		return keys;
	}

	const QSet<QString> &worldViewEnabledBooleanAttributeKeys()
	{
		static const QSet<QString> keys = {QStringLiteral("wrap"),
		                                   QStringLiteral("auto_wrap_window_width"),
		                                   QStringLiteral("naws"),
		                                   QStringLiteral("wrap_input"),
		                                   QStringLiteral("alternative_inverse"),
		                                   QStringLiteral("display_my_input"),
		                                   QStringLiteral("line_information"),
		                                   QStringLiteral("escape_deletes_input"),
		                                   QStringLiteral("save_deleted_command"),
		                                   QStringLiteral("confirm_on_paste"),
		                                   QStringLiteral("ctrl_backspace_deletes_last_word"),
		                                   QStringLiteral("arrows_change_history"),
		                                   QStringLiteral("arrow_keys_wrap"),
		                                   QStringLiteral("arrow_recalls_partial"),
		                                   QStringLiteral("alt_arrow_recalls_partial"),
		                                   QStringLiteral("ctrl_z_goes_to_end_of_buffer"),
		                                   QStringLiteral("ctrl_p_goes_to_previous_command"),
		                                   QStringLiteral("ctrl_n_goes_to_next_command"),
		                                   QStringLiteral("confirm_before_replacing_typing"),
		                                   QStringLiteral("double_click_inserts"),
		                                   QStringLiteral("double_click_sends"),
		                                   QStringLiteral("auto_repeat"),
		                                   QStringLiteral("lower_case_tab_completion"),
		                                   QStringLiteral("tab_completion_space"),
		                                   QStringLiteral("auto_resize_command_window"),
		                                   QStringLiteral("keep_commands_on_same_line"),
		                                   QStringLiteral("no_echo_off"),
		                                   QStringLiteral("always_record_command_history"),
		                                   QStringLiteral("hyperlink_adds_to_command_history"),
		                                   QStringLiteral("use_custom_link_colour"),
		                                   QStringLiteral("underline_hyperlinks"),
		                                   QStringLiteral("auto_pause"),
		                                   QStringLiteral("keep_pause_at_bottom"),
		                                   QStringLiteral("start_paused"),
		                                   QStringLiteral("use_default_output_font"),
		                                   QStringLiteral("use_default_input_font")};
		return keys;
	}

	const QSet<QString> &worldViewDisabledBooleanAttributeKeys()
	{
		static const QSet<QString> keys = {QStringLiteral("show_bold"), QStringLiteral("show_italic"),
		                                   QStringLiteral("show_underline")};
		return keys;
	}

	const QSet<QString> &worldViewNumericAttributeKeys()
	{
		static const QSet<QString> keys = {QStringLiteral("output_font_height"),
		                                   QStringLiteral("input_font_height"),
		                                   QStringLiteral("output_font_weight"),
		                                   QStringLiteral("input_font_weight"),
		                                   QStringLiteral("input_font_italic"),
		                                   QStringLiteral("output_font_charset"),
		                                   QStringLiteral("input_font_charset"),
		                                   QStringLiteral("wrap_column"),
		                                   QStringLiteral("max_output_lines"),
		                                   QStringLiteral("line_spacing"),
		                                   QStringLiteral("fade_output_buffer_after_seconds"),
		                                   QStringLiteral("fade_output_opacity_percent"),
		                                   QStringLiteral("fade_output_seconds"),
		                                   QStringLiteral("pixel_offset"),
		                                   QStringLiteral("tab_completion_lines"),
		                                   QStringLiteral("auto_resize_minimum_lines"),
		                                   QStringLiteral("auto_resize_maximum_lines"),
		                                   QStringLiteral("history_lines")};
		return keys;
	}

	const QSet<QString> &worldViewColorAttributeKeys()
	{
		static const QSet<QString> keys = {QStringLiteral("input_background_colour"),
		                                   QStringLiteral("input_text_colour"),
		                                   QStringLiteral("output_background_colour"),
		                                   QStringLiteral("output_text_colour"),
		                                   QStringLiteral("timestamp_output_text_colour"),
		                                   QStringLiteral("timestamp_output_back_colour"),
		                                   QStringLiteral("timestamp_input_text_colour"),
		                                   QStringLiteral("timestamp_input_back_colour"),
		                                   QStringLiteral("timestamp_notes_text_colour"),
		                                   QStringLiteral("timestamp_notes_back_colour"),
		                                   QStringLiteral("hyperlink_colour")};
		return keys;
	}

	bool isEnabledFlagValue(const QString &value)
	{
		return value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ||
		       value == QStringLiteral("1");
	}

	bool isDisabledFlagValue(const QString &value)
	{
		return value.compare(QStringLiteral("n"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0 ||
		       value == QStringLiteral("0");
	}

	int canonicalWorldViewNumericValue(const QString &key, const QString &value)
	{
		bool ok     = false;
		int  parsed = value.toInt(&ok);
		if (key == QStringLiteral("line_spacing"))
			return ok && parsed >= 0 ? parsed : 0;
		if (key == QStringLiteral("fade_output_buffer_after_seconds"))
			return ok && parsed >= 0 ? parsed : 0;
		if (key == QStringLiteral("fade_output_opacity_percent"))
		{
			if (!ok)
				parsed = 100;
			return qBound(0, parsed, 100);
		}
		if (key == QStringLiteral("fade_output_seconds"))
			return ok && parsed > 0 ? parsed : 1;
		if (key == QStringLiteral("tab_completion_lines"))
			return ok && parsed >= 1 ? parsed : 200;
		if (key == QStringLiteral("auto_resize_minimum_lines"))
			return ok && parsed > 0 ? parsed : 1;
		if (key == QStringLiteral("auto_resize_maximum_lines"))
			return ok && parsed > 0 ? parsed : 20;
		if (key == QStringLiteral("history_lines"))
			return ok && parsed >= 0 ? parsed : 0;
		return ok ? parsed : 0;
	}

	QString decodeMxpMenuText(QString text)
	{
		if (text.isEmpty())
			return {};

		text = QUrl::fromPercentEncoding(text.toUtf8());
		if (!text.contains(QLatin1Char('&')))
			return text;

		QString result;
		result.reserve(text.size());
		for (qsizetype i = 0; i < text.size(); ++i)
		{
			const QChar ch = text.at(i);
			if (ch != QLatin1Char('&'))
			{
				result.append(ch);
				continue;
			}

			const qsizetype semi = text.indexOf(QLatin1Char(';'), i + 1);
			if (semi < 0)
			{
				result.append(ch);
				continue;
			}

			const QString entity = text.mid(i + 1, semi - i - 1).trimmed();
			QString       decoded;
			if (entity.compare(QStringLiteral("quot"), Qt::CaseInsensitive) == 0)
				decoded = QStringLiteral("\"");
			else if (entity.compare(QStringLiteral("apos"), Qt::CaseInsensitive) == 0)
				decoded = QStringLiteral("'");
			else if (entity.compare(QStringLiteral("amp"), Qt::CaseInsensitive) == 0)
				decoded = QStringLiteral("&");
			else if (entity.compare(QStringLiteral("lt"), Qt::CaseInsensitive) == 0)
				decoded = QStringLiteral("<");
			else if (entity.compare(QStringLiteral("gt"), Qt::CaseInsensitive) == 0)
				decoded = QStringLiteral(">");
			else if (entity.startsWith(QLatin1Char('#')))
			{
				bool     ok   = false;
				uint32_t code = 0;
				if (entity.size() > 2 &&
				    (entity.at(1) == QLatin1Char('x') || entity.at(1) == QLatin1Char('X')))
					code = entity.mid(2).toUInt(&ok, 16);
				else
					code = entity.mid(1).toUInt(&ok, 10);
				if (ok && code <= 0x10FFFFu)
					decoded = QString(QChar::fromUcs4(code));
			}

			if (decoded.isEmpty())
			{
				result.append(ch);
				continue;
			}

			result.append(decoded);
			i = semi;
		}
		return result;
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

	void          forceOpaqueMenu(QMenu *menu);

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
		static const bool enabled =
		    !qmudEnvironmentVariableIsEmpty(QStringLiteral("QMUD_DEBUG_MINIWINDOW_MOUSE"));
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

	QRect expandedMiniWindowDirtyRect(const QRect &rect, const QRect &clipRect)
	{
		if (rect.isEmpty())
			return {};
		return rect.adjusted(-1, -1, 1, 1).intersected(clipRect);
	}

	QRect unitedMiniWindowBounds(const QRect &first, const QRect &second)
	{
		if (first.isEmpty())
			return second;
		if (second.isEmpty())
			return first;
		return first.united(second);
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

	QPoint miniWindowDisplayToContentUnbounded(const MiniWindow *window, const QPoint &displayPoint)
	{
		if (!hasScaledAbsoluteRect(window))
			return displayPoint;

		const int displayWidth  = qMax(1, window->rect.width());
		const int displayHeight = qMax(1, window->rect.height());
		const int contentWidth  = qMax(1, window->width);
		const int contentHeight = qMax(1, window->height);

		const int x = (displayPoint.x() * contentWidth) / displayWidth;
		const int y = (displayPoint.y() * contentHeight) / displayHeight;
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

	constexpr ushort kNativeLineSeparatorCode = 0x2028;

	int              nativeEstimatedHardBreakRows(const QString &text)
	{
		int          rows = 1;
		const QChar *data = text.constData();
		for (qsizetype index = 0; index < text.size(); ++index)
		{
			const ushort code = data[index].unicode();
			if (code == '\n' || code == kNativeLineSeparatorCode)
				++rows;
		}
		return qMax(1, rows);
	}

	bool nativeTextFitsEstimatedSingleRow(const QString &text, const int columnsPerRow)
	{
		if (text.size() > columnsPerRow)
			return false;

		const QChar *data = text.constData();
		for (qsizetype index = 0; index < text.size(); ++index)
		{
			const ushort code = data[index].unicode();
			if (code == '\n' || code == kNativeLineSeparatorCode)
				return false;
		}
		return true;
	}

	bool nativeEstimatedWhitespace(const ushort code)
	{
		switch (code)
		{
		case ' ':
		case '\t':
		case '\r':
		case '\v':
		case '\f':
			return true;
		default:
			break;
		}
		return false;
	}
} // namespace

class WrapTextBrowser : public QAbstractScrollArea
{
	public:
		enum LineWrapMode
		{
			NoWrap,
			WidgetWidth,
		};

		explicit WrapTextBrowser(WorldView *view, QWidget *parent = nullptr, bool isLive = false)
		    : QAbstractScrollArea(parent), m_view(view), m_isLive(isLive)
		{
			setFrameShape(QFrame::NoFrame);
			setViewport(new QWidget(this));
			if (QWidget *const vp = viewport())
			{
				vp->setAutoFillBackground(false);
				vp->setAttribute(Qt::WA_OpaquePaintEvent, false);
			}
			setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		}

		void setLineWrapMode(const LineWrapMode mode)
		{
			m_lineWrapMode = mode;
		}

		[[nodiscard]] LineWrapMode lineWrapMode() const
		{
			return m_lineWrapMode;
		}

		void setViewportMarginsPublic(int left, int top, int right, int bottom)
		{
			setViewportMargins(left, top, right, bottom);
		}

		[[nodiscard]] QMargins viewportMarginsPublic() const
		{
			return viewportMargins();
		}

	protected:
		void paintEvent(QPaintEvent *event) override
		{
			if (property("qmud_native_text_suppressed").toBool())
			{
				if (QWidget *const viewportWidget = viewport())
				{
					QPainter painter(viewportWidget);
					painter.setCompositionMode(QPainter::CompositionMode_Source);
					painter.fillRect(event ? event->rect() : viewportWidget->rect(), Qt::transparent);
				}
				return;
			}
			QAbstractScrollArea::paintEvent(event);
		}

		void mouseMoveEvent(QMouseEvent *event) override
		{
			if (m_view && m_view->handleMiniWindowMouseMove(event, viewport()))
			{
				event->accept();
				return;
			}
			QAbstractScrollArea::mouseMoveEvent(event);
		}

		void mousePressEvent(QMouseEvent *event) override
		{
			if (m_view && m_view->handleMiniWindowMousePress(event, false, viewport()))
			{
				event->accept();
				return;
			}
			QAbstractScrollArea::mousePressEvent(event);
		}

		void mouseReleaseEvent(QMouseEvent *event) override
		{
			if (m_view && m_view->handleMiniWindowMouseRelease(event, viewport()))
			{
				event->accept();
				return;
			}
			QAbstractScrollArea::mouseReleaseEvent(event);
		}

		void mouseDoubleClickEvent(QMouseEvent *event) override
		{
			if (m_view && m_view->handleMiniWindowMousePress(event, true, viewport()))
			{
				event->accept();
				return;
			}
			QAbstractScrollArea::mouseDoubleClickEvent(event);
		}

		void wheelEvent(QWheelEvent *event) override
		{
			if (m_view && m_view->handleMiniWindowWheel(event, viewport()))
			{
				event->accept();
				return;
			}
			if (m_view)
			{
				m_view->handleOutputWheel(event);
				event->accept();
				return;
			}
			QAbstractScrollArea::wheelEvent(event);
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
			QAbstractScrollArea::keyPressEvent(event);
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
			QAbstractScrollArea::contextMenuEvent(event);
		}

	private:
		WorldView   *m_view{nullptr};
		bool         m_isLive{false};
		LineWrapMode m_lineWrapMode{NoWrap};
};

class WorldOutputCanvas : public QWidget
{
	public:
		explicit WorldOutputCanvas(WorldView *view, QWidget *parent = nullptr) : QWidget(parent), m_view(view)
		{
			setAccessibleName(QStringLiteral("World output"));
			setProperty(kWorldOutputAccessibleProperty, true);
			setAttribute(Qt::WA_TransparentForMouseEvents);
			setAttribute(Qt::WA_OpaquePaintEvent);
			setAutoFillBackground(false);
		}

		[[nodiscard]] WorldView *worldView() const
		{
			return m_view;
		}

	protected:
		void paintEvent(QPaintEvent *event) override
		{
			if (!m_view)
				return;
			QPainter      painter(this);
			const QRegion dirtyRegion = event ? event->region() : QRegion(rect());
			m_view->paintNativeOutputCanvas(&painter, dirtyRegion);
			m_view->miniWindowLayerPainted(true, dirtyRegion);
		}

	private:
		WorldView *m_view{nullptr};
};

class WorldOutputAccessible final : public QAccessibleWidget, public QAccessibleTextInterface
{
	public:
		explicit WorldOutputAccessible(WorldOutputCanvas *widget)
		    : QAccessibleWidget(widget, QAccessible::Terminal, QStringLiteral("World output"))
		{
			if (WorldView *view = widget ? widget->worldView() : nullptr)
				view->primeAccessibleOutputTextState();
		}

		void *interface_cast(const QAccessible::InterfaceType type) override
		{
			if (type == QAccessible::TextInterface)
				return static_cast<QAccessibleTextInterface *>(this);
			return QAccessibleWidget::interface_cast(type);
		}

		[[nodiscard]] QString text(const QAccessible::Text type) const override
		{
			if (type == QAccessible::Name)
			{
				return QStringLiteral("World output");
			}
			if (type == QAccessible::Value)
				return accessibleText().text(0, accessibleText().characterCount());
			return QAccessibleWidget::text(type);
		}

		[[nodiscard]] QAccessible::State state() const override
		{
			QAccessible::State state = QAccessibleWidget::state();
			state.readOnly           = true;
			state.multiLine          = true;
			state.selectableText     = true;
			return state;
		}

		void selection(int selectionIndex, int *startOffset, int *endOffset) const override
		{
			if (startOffset)
				*startOffset = 0;
			if (endOffset)
				*endOffset = 0;
			if (selectionIndex != 0)
				return;

			int              startLine   = 0;
			int              startColumn = 0;
			int              endLine     = 0;
			int              endColumn   = 0;
			const WorldView *view        = worldView();
			if (!view || !view->nativeOutputSelectionBounds(startLine, startColumn, endLine, endColumn))
				return;

			const QMudAccessibleTextUtils::LineOffsetMap map = accessibleText();
			if (startOffset)
				*startOffset = map.offsetForPosition({qMax(0, startLine - 1), qMax(0, startColumn - 1)});
			if (endOffset)
				*endOffset = map.offsetForPosition({qMax(0, endLine - 1), qMax(0, endColumn - 1)});
		}

		[[nodiscard]] int selectionCount() const override
		{
			int              startLine   = 0;
			int              startColumn = 0;
			int              endLine     = 0;
			int              endColumn   = 0;
			const WorldView *view        = worldView();
			return view && view->nativeOutputSelectionBounds(startLine, startColumn, endLine, endColumn) ? 1
			                                                                                             : 0;
		}

		void addSelection(int startOffset, int endOffset) override
		{
			setSelection(0, startOffset, endOffset);
		}

		void removeSelection(int selectionIndex) override
		{
			if (selectionIndex != 0)
				return;
			if (WorldView *view = worldView())
				view->clearNativeOutputSelection(true);
		}

		void setSelection(int selectionIndex, int startOffset, int endOffset) override
		{
			if (selectionIndex != 0)
				return;
			WorldView       *view    = worldView();
			WrapTextBrowser *browser = targetBrowser();
			if (!view || !browser)
				return;

			const QMudAccessibleTextUtils::LineOffsetMap map = accessibleText();
			const QMudAccessibleTextUtils::TextPosition  start =
			    map.positionForOffset(qMin(startOffset, endOffset));
			const QMudAccessibleTextUtils::TextPosition end =
			    map.positionForOffset(qMax(startOffset, endOffset));
			view->setNativeOutputSelection(browser, {start.line, start.column}, {end.line, end.column},
			                               false);
		}

		[[nodiscard]] int cursorPosition() const override
		{
			return characterCount();
		}

		void setCursorPosition(int position) override
		{
			Q_UNUSED(position);
		}

		[[nodiscard]] QString text(int startOffset, int endOffset) const override
		{
			return accessibleText().text(startOffset, endOffset);
		}

		[[nodiscard]] int characterCount() const override
		{
			return accessibleText().characterCount();
		}

		[[nodiscard]] QRect characterRect(int offset) const override
		{
			const QMudAccessibleTextUtils::LineOffsetMap map = accessibleText();
			if (map.isEmpty())
				return {};

			const QMudAccessibleTextUtils::TextPosition position =
			    map.positionForOffset(qBound(0, offset, map.characterCount()));
			WorldView       *view    = worldView();
			WrapTextBrowser *browser = targetBrowser();
			if (!view || !browser)
				return {};

			QRect rect;
			return view->nativeOutputCharacterRect(browser, {position.line, position.column}, rect) ? rect
			                                                                                        : QRect();
		}

		[[nodiscard]] int offsetAtPoint(const QPoint &point) const override
		{
			WorldView       *view    = worldView();
			WrapTextBrowser *browser = targetBrowser();
			if (!view || !browser)
				return -1;

			WrapTextBrowser                *hitView = nullptr;
			WorldView::NativeOutputPosition position;
			if (!view->nativeOutputHitTestGlobal(point, hitView, position, nullptr, nullptr, true, true) ||
			    hitView != browser)
			{
				return -1;
			}

			const QMudAccessibleTextUtils::LineOffsetMap map = accessibleText();
			return map.offsetForPosition({position.line, position.column});
		}

		void scrollToSubstring(int startIndex, int endIndex) override
		{
			WorldView             *view    = worldView();
			const WrapTextBrowser *browser = targetBrowser();
			if (!view || !browser)
				return;

			const QMudAccessibleTextUtils::LineOffsetMap map = accessibleText();
			if (map.isEmpty())
				return;

			const QMudAccessibleTextUtils::TextPosition start =
			    map.positionForOffset(qMin(startIndex, endIndex));
			const QMudAccessibleTextUtils::TextPosition end =
			    map.positionForOffset(qMax(startIndex, endIndex));
			view->scrollNativeOutputRangeIntoView(browser, start.line, end.line);
		}

		QString attributes(int offset, int *startOffset, int *endOffset) const override
		{
			if (startOffset)
				*startOffset = qBound(0, offset, characterCount());
			if (endOffset)
				*endOffset = qBound(0, offset, characterCount());
			return {};
		}

	private:
		[[nodiscard]] WorldOutputCanvas *outputCanvas() const
		{
			return dynamic_cast<WorldOutputCanvas *>(widget());
		}

		[[nodiscard]] WrapTextBrowser *targetBrowser() const
		{
			WorldView *view = worldView();
			return view ? view->activeOutputView() : nullptr;
		}

		[[nodiscard]] WorldView *worldView() const
		{
			WorldOutputCanvas *canvas = outputCanvas();
			return canvas ? canvas->worldView() : nullptr;
		}

		[[nodiscard]] QMudAccessibleTextUtils::LineOffsetMap accessibleText() const
		{
			const WorldView *view = worldView();
			if (!view)
				return {};

			const QVector<WorldView::NativeOutputRenderLine> &renderLines = view->nativeOutputRenderLines();
			return WorldView::accessibleNativeOutputTextMap(renderLines);
		}
};

QAccessibleInterface *worldOutputAccessibleFactory(const QString &className, QObject *object)
{
	Q_UNUSED(className);
	auto *widget = qobject_cast<QWidget *>(object);
	if (!widget || !widget->property(kWorldOutputAccessibleProperty).toBool())
		return nullptr;
	auto *canvas = dynamic_cast<WorldOutputCanvas *>(widget);
	return canvas ? new WorldOutputAccessible(canvas) : nullptr;
}

void qmudInstallWorldOutputAccessibility()
{
	static bool installed = false;
	if (installed)
		return;
	QAccessible::installFactory(worldOutputAccessibleFactory);
	installed = true;
}

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

		[[nodiscard]] QMargins viewportMarginsPublic() const
		{
			return viewportMargins();
		}

	protected:
		bool event(QEvent *event) override;
		void keyPressEvent(QKeyEvent *event) override;
		void mousePressEvent(QMouseEvent *event) override;
		void mouseReleaseEvent(QMouseEvent *event) override;
		void resizeEvent(QResizeEvent *event) override;
		void contextMenuEvent(QContextMenuEvent *event) override;

	private:
		WorldView *m_view{nullptr};
#ifdef Q_OS_WIN
		bool m_suppressNextAltNumpadCommit{false};
#endif
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
			if (!m_view)
				return;
			QPainter painter(this);
			painter.setRenderHint(QPainter::SmoothPixmapTransform);
			const QRegion dirtyRegion = event ? event->region() : QRegion(rect());
			m_view->paintMiniWindows(&painter, m_underneath, dirtyRegion);
			m_view->miniWindowLayerPainted(m_underneath, dirtyRegion);
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
	m_outputSplitter->setChildrenCollapsible(true);

	m_output = new WrapTextBrowser(this, m_outputSplitter);
	m_output->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_output->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	m_liveOutput = new WrapTextBrowser(this, m_outputSplitter, true);
	m_liveOutput->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_liveOutput->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	m_miniUnderlay       = new MiniWindowLayer(this, true, m_outputStack);
	m_miniOverlay        = new MiniWindowLayer(this, false, m_outputStack);
	m_nativeOutputCanvas = new WorldOutputCanvas(this, m_outputStack);
	m_nativeOutputCanvas->setObjectName(QStringLiteral("worldOutputNativeCanvas"));
	m_nativeOutputCanvas->setVisible(isVisible());

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
	m_outputSplitter->addWidget(m_output);
	m_outputSplitter->addWidget(m_liveOutput);
	m_outputSplitter->setStretchFactor(0, 1);
	m_outputSplitter->setStretchFactor(1, 0);
	m_outputSplitter->setCollapsible(0, false);
	m_outputSplitter->setCollapsible(1, true);
	m_outputSplitter->setSizes(QList<int>() << 1 << 0);
	m_outputSplitter->setHandleWidth(0);

	m_outputScrollBar = new QScrollBar(Qt::Vertical, m_outputContainer);
	outputStackLayout->addWidget(m_miniUnderlay, 0, 0);
	if (m_nativeOutputCanvas)
		outputStackLayout->addWidget(m_nativeOutputCanvas, 0, 0);
	outputStackLayout->addWidget(m_miniOverlay, 0, 0);
	m_outputSplitter->setGeometry(m_outputStack->rect());
	m_miniUnderlay->lower();
	m_miniUnderlay->setVisible(false);
	m_outputSplitter->raise();
	if (m_nativeOutputCanvas)
		m_nativeOutputCanvas->raise();
	m_miniOverlay->raise();
	syncOutputTextVisibilityForNativeCanvas();

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
	applyDefaultInputHeight(false);

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
		        requestInputViewportSync();
		        if (m_runtime && !m_notifyingPluginCommandChanged)
		        {
			        m_notifyingPluginCommandChanged = true;
			        m_runtime->firePluginCommandChanged();
			        m_notifyingPluginCommandChanged = false;
		        }
	        });

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
				m_outputScrollBar->setSingleStep(outputScrollUnitsPerLine());
				m_outputScrollBar->setValue(bar->value());
			};
			connect(
			    bar, &QScrollBar::rangeChanged, this,
			    [this, bar](int min, int max)
			    {
				    if (traceOutputBackfillEnabled())
				    {
					    qInfo().noquote()
					        << QStringLiteral(
					               "[OutputBackfill] sb-range world=%1 min=%2 max=%3 value=%4 extValue=%5")
					               .arg(traceWorldName(m_runtime))
					               .arg(min)
					               .arg(max)
					               .arg(bar ? bar->value() : -1)
					               .arg(m_outputScrollBar ? m_outputScrollBar->value() : -1);
				    }
				    if (m_outputScrollBar)
				    {
					    QSignalBlocker block(m_outputScrollBar);
					    m_outputScrollBar->setRange(min, max);
					    m_outputScrollBar->setPageStep(bar->pageStep());
					    m_outputScrollBar->setSingleStep(outputScrollUnitsPerLine());
				    }
			    });
			connect(
			    bar, &QScrollBar::valueChanged, this,
			    [this](int value)
			    {
				    if (traceOutputBackfillEnabled())
				    {
					    const QScrollBar *const outputBar =
					        m_output ? m_output->verticalScrollBar() : nullptr;
					    qInfo().noquote()
					        << QStringLiteral(
					               "[OutputBackfill] sb-value world=%1 value=%2 max=%3 extBefore=%4 split=%5")
					               .arg(traceWorldName(m_runtime))
					               .arg(value)
					               .arg(outputBar ? outputBar->maximum() : -1)
					               .arg(m_outputScrollBar ? m_outputScrollBar->value() : -1)
					               .arg(m_scrollbackSplitActive ? QStringLiteral("1") : QStringLiteral("0"));
				    }
				    if (!m_outputScrollBar)
					    return;
				    QSignalBlocker block(m_outputScrollBar);
				    m_outputScrollBar->setValue(value);
				    clearNativeSelectionIfOutsideVisibleViewport(m_output);
				    if (!m_nativeScrollSyncInPaint)
					    requestNativeOutputRepaint();
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
	if (m_liveOutput && m_liveOutput->verticalScrollBar())
	{
		connect(m_liveOutput->verticalScrollBar(), &QScrollBar::valueChanged, this,
		        [this](int)
		        {
			        clearNativeSelectionIfOutsideVisibleViewport(m_liveOutput);
			        if (!m_nativeScrollSyncInPaint)
				        requestNativeOutputRepaint();
		        });
	}
	syncOutputScrollSingleStep();
	connect(m_input, &QPlainTextEdit::selectionChanged, this, [this] { emit inputSelectionChanged(); });
	connect(this, &WorldView::outputScrollChanged, this, [this] { requestDrawOutputWindowNotification(); });

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

WorldView::~WorldView()
{
	stopMiniWindowMouseCapture();
	m_destroying = true;
}

void WorldView::setWorldName(const QString &name)
{
	setWindowTitle(name);
}

void WorldView::setRuntime(WorldRuntime *runtime)
{
	if (runtime && m_runtime == runtime)
	{
		if (m_runtime->view() != this)
			m_runtime->setView(this);
		return;
	}

	if (m_runtime && m_runtime->view() == this)
		m_runtime->setView(nullptr);

	if (runtime)
	{
		// Runtime/native mode must clear stale standalone output state.
		stopIncrementalHyperlinkRestyle();
		m_pendingOutput.clear();
		m_hasPartialOutput       = false;
		m_partialOutputStart     = 0;
		m_partialOutputLength    = 0;
		m_nativeHasPartialOutput = false;
		m_nativePartialOutputText.clear();
		m_nativePartialOutputSpans.clear();
		m_nativeRenderLineCacheFromRuntime = true;
		m_nativeRenderLineCacheValid       = false;
		m_nativeLayoutCacheValid           = false;
		m_lastQueuedOutputClientSize       = {};
		m_lastQueuedOutputClientSizeValid  = false;
		clearNativeOutputSelection(true);
		m_nativeStandaloneOutputLines.clear();
		m_nativeStandaloneNextLineNumber       = 1;
		m_nativePrimaryPaintState              = {};
		m_nativeLivePaintState                 = {};
		m_nativeOutputScrollBlitPending        = false;
		m_nativeOutputScrollBlitExposedRect    = {};
		m_miniWindowChangeSerial               = 0;
		m_miniWindowPaintBoundsValid           = false;
		m_pendingMiniWindowUnderlayDirtyRegion = {};
		m_pendingMiniWindowOverlayDirtyRegion  = {};
		m_wrapMarginReservationCacheValid      = false;
		m_wrapMarginReservationPixels          = 0;
	}

	m_miniWindowChangeSerial               = 0;
	m_nativePrimaryPaintState              = {};
	m_nativeLivePaintState                 = {};
	m_nativeOutputScrollBlitPending        = false;
	m_nativeOutputScrollBlitExposedRect    = {};
	m_miniWindowPaintBoundsValid           = false;
	m_pendingMiniWindowUnderlayDirtyRegion = {};
	m_pendingMiniWindowOverlayDirtyRegion  = {};
	m_wrapMarginReservationCacheValid      = false;
	m_wrapMarginReservationPixels          = 0;
	m_runtime                              = runtime;
	resetRuntimeSettingsSnapshot();
	applyRuntimeSettings();
	if (m_runtime)
		m_runtime->setView(this);
	syncMiniWindowDevicePixelRatio();
}

void WorldView::setRuntimeObserver(WorldRuntime *runtime)
{
	if (runtime && m_runtime == runtime)
	{
		if (m_runtime->view() == this)
			m_runtime->setView(nullptr);
		return;
	}

	if (m_runtime && m_runtime->view() == this)
		m_runtime->setView(nullptr);

	if (runtime)
	{
		// Observer mode shares the same native-only output constraints as attached mode.
		stopIncrementalHyperlinkRestyle();
		m_pendingOutput.clear();
		m_hasPartialOutput       = false;
		m_partialOutputStart     = 0;
		m_partialOutputLength    = 0;
		m_nativeHasPartialOutput = false;
		m_nativePartialOutputText.clear();
		m_nativePartialOutputSpans.clear();
		m_nativeRenderLineCacheFromRuntime = true;
		m_nativeRenderLineCacheValid       = false;
		m_nativeLayoutCacheValid           = false;
		m_lastQueuedOutputClientSize       = {};
		m_lastQueuedOutputClientSizeValid  = false;
		clearNativeOutputSelection(true);
		m_nativeStandaloneOutputLines.clear();
		m_nativeStandaloneNextLineNumber  = 1;
		m_miniWindowChangeSerial          = 0;
		m_wrapMarginReservationCacheValid = false;
		m_wrapMarginReservationPixels     = 0;
	}

	m_miniWindowChangeSerial          = 0;
	m_wrapMarginReservationCacheValid = false;
	m_wrapMarginReservationPixels     = 0;
	m_runtime                         = runtime;
	resetRuntimeSettingsSnapshot();
	applyRuntimeSettings();
}

QVector<QPair<QString, QString>> WorldView::parseMxpContextMenuActions(const QString &rawHref,
                                                                       const QString &rawHint)
{
	const QString decodedHref = decodeMxpMenuText(rawHref).trimmed();
	if (decodedHref.isEmpty())
		return {};

	QStringList actionParts = decodedHref.split(QLatin1Char('|'), Qt::KeepEmptyParts);
	QStringList actions;
	actions.reserve(actionParts.size());
	for (const QString &part : actionParts)
	{
		const QString trimmed = part.trimmed();
		if (!trimmed.isEmpty())
			actions.push_back(trimmed);
	}
	if (actions.isEmpty())
		return {};

	const QString decodedHint = decodeMxpMenuText(rawHint).trimmed();
	QStringList   hints;
	if (!decodedHint.isEmpty())
		hints = decodedHint.split(QLatin1Char('|'), Qt::KeepEmptyParts);

	while (!hints.isEmpty() && hints.size() > actions.size())
		hints.removeFirst();

	constexpr int                    kMaxMxpContextActions = 32;
	QVector<QPair<QString, QString>> result;
	result.reserve(qMin(actions.size(), kMaxMxpContextActions));

	for (int i = 0; i < actions.size() && i < kMaxMxpContextActions; ++i)
	{
		const QString &action = actions.at(i);
		QString        label;
		if (i < hints.size())
			label = hints.at(i).trimmed();
		if (label.isEmpty())
			label = action;
		label.replace(QStringLiteral("&"), QStringLiteral("&&"));
		result.push_back({action, label});
	}

	return result;
}

WorldView::MiniWindowPaintBoundsSnapshot WorldView::miniWindowPaintBoundsSnapshot() const
{
	MiniWindowPaintBoundsSnapshot snapshot;
	snapshot.clientSize = m_outputStack ? m_outputStack->size() : size();
	snapshot.ownerSize  = size();
	if (!m_runtime)
		return snapshot;

	const QImage backgroundImage = m_runtime->backgroundImage();
	const QImage foregroundImage = m_runtime->foregroundImage();
	snapshot.backgroundImageKey  = backgroundImage.cacheKey();
	snapshot.foregroundImageKey  = foregroundImage.cacheKey();
	snapshot.backgroundImageMode = m_runtime->backgroundImageMode();
	snapshot.foregroundImageMode = m_runtime->foregroundImageMode();

	const QRect clientRect(QPoint(0, 0), snapshot.clientSize);
	const auto  windows     = m_runtime->sortedMiniWindows();
	auto        layerBounds = [this, &snapshot, &clientRect, &windows](const bool underneath)
	{
		QRect bounds;
		if (!m_runtime)
			return bounds;
		m_runtime->layoutMiniWindows(snapshot.clientSize, snapshot.ownerSize, underneath, &windows);
		for (const MiniWindow *window : windows)
		{
			if (!window || !window->show || window->temporarilyHide)
				continue;
			const bool drawUnder = (window->flags & kMiniWindowDrawUnderneath) != 0;
			if (drawUnder != underneath || window->backingSurfaceIsNull())
				continue;

			QRect windowBounds;
			if ((window->flags & kMiniWindowAbsoluteLocation) == 0 && window->position == 13)
			{
				windowBounds = clientRect;
			}
			else if (((window->flags & kMiniWindowAbsoluteLocation) == 0 && window->position >= 0 &&
			          window->position <= 3) ||
			         ((window->flags & kMiniWindowAbsoluteLocation) != 0 && hasScaledAbsoluteRect(window)))
			{
				windowBounds = window->rect;
			}
			else
			{
				windowBounds = QRect(window->rect.topLeft(), imageLogicalSize(window->backingSurface()));
			}

			windowBounds = windowBounds.intersected(clientRect);
			if (!windowBounds.isEmpty())
				bounds = bounds.isNull() ? windowBounds : bounds.united(windowBounds);
		}
		return bounds;
	};

	snapshot.underlayBounds = layerBounds(true);
	snapshot.overlayBounds  = layerBounds(false);
	return snapshot;
}

void WorldView::refreshMiniWindows(const bool forceFullRepaint) const
{
	if (m_nativeOutputCanvas && m_nativeOutputCanvas->isVisible())
	{
		const MiniWindowPaintBoundsSnapshot currentBounds = miniWindowPaintBoundsSnapshot();
		const QRect                         canvasRect(QPoint(0, 0), currentBounds.clientSize);
		const bool                          imageStateChanged =
		    m_miniWindowPaintBoundsValid &&
		    (m_miniWindowPaintBounds.backgroundImageKey != currentBounds.backgroundImageKey ||
		     m_miniWindowPaintBounds.foregroundImageKey != currentBounds.foregroundImageKey ||
		     m_miniWindowPaintBounds.backgroundImageMode != currentBounds.backgroundImageMode ||
		     m_miniWindowPaintBounds.foregroundImageMode != currentBounds.foregroundImageMode);
		const bool layoutStateChanged =
		    m_miniWindowPaintBoundsValid && (m_miniWindowPaintBounds.clientSize != currentBounds.clientSize ||
		                                     m_miniWindowPaintBounds.ownerSize != currentBounds.ownerSize);
		const bool fullRepaint = forceFullRepaint || !m_miniWindowPaintBoundsValid || imageStateChanged ||
		                         layoutStateChanged || !m_runtime;
		const bool fullOverlayRepaint = isMiniWindowCaptureActive();

		if (fullRepaint)
		{
			m_pendingMiniWindowUnderlayDirtyRegion = QRegion(canvasRect);
			m_pendingMiniWindowOverlayDirtyRegion  = QRegion(canvasRect);
			m_nativeOutputCanvas->update();
			if (m_miniOverlay)
				m_miniOverlay->update();
			m_miniWindowPaintBounds      = currentBounds;
			m_miniWindowPaintBoundsValid = true;
			return;
		}

		const QRect underlayDirty = expandedMiniWindowDirtyRect(
		    unitedMiniWindowBounds(m_miniWindowPaintBounds.underlayBounds, currentBounds.underlayBounds),
		    canvasRect);
		const QRect overlayDirty = expandedMiniWindowDirtyRect(
		    unitedMiniWindowBounds(m_miniWindowPaintBounds.overlayBounds, currentBounds.overlayBounds),
		    canvasRect);
		if (!underlayDirty.isEmpty())
		{
			m_pendingMiniWindowUnderlayDirtyRegion =
			    m_pendingMiniWindowUnderlayDirtyRegion.united(QRegion(underlayDirty));
			m_nativeOutputCanvas->update(m_pendingMiniWindowUnderlayDirtyRegion);
		}
		if (m_miniOverlay)
		{
			if (fullOverlayRepaint)
			{
				m_pendingMiniWindowOverlayDirtyRegion = QRegion(canvasRect);
				m_miniOverlay->update();
			}
			else if (!overlayDirty.isEmpty())
			{
				m_pendingMiniWindowOverlayDirtyRegion =
				    m_pendingMiniWindowOverlayDirtyRegion.united(QRegion(overlayDirty));
				m_miniOverlay->update(m_pendingMiniWindowOverlayDirtyRegion);
			}
		}
		m_miniWindowPaintBounds      = currentBounds;
		m_miniWindowPaintBoundsValid = true;
		return;
	}
	if (m_miniUnderlay)
		m_miniUnderlay->update();
	if (m_miniOverlay)
		m_miniOverlay->update();
}

void WorldView::onMiniWindowsChanged()
{
	++m_miniWindowChangeSerial;
	m_wrapMarginReservationCacheValid = false;
	if (m_wrapMarginUpdateQueued)
		return;
	m_wrapMarginUpdateQueued = true;
	QMetaObject::invokeMethod(
	    this,
	    [this]
	    {
		    m_wrapMarginUpdateQueued = false;
		    refreshMiniWindows();
		    updateWrapMargin();
		    if (m_runtime)
			    m_runtime->refreshNawsWindowSize();
	    },
	    Qt::QueuedConnection);
}

void WorldView::syncMiniWindowDevicePixelRatio() const
{
	if (!m_runtime)
		return;
	if (m_runtime->syncMiniWindowDevicePixelRatioForView())
		refreshMiniWindows(true);
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

	const QPoint         sourcePos = source->viewport()->mapFromGlobal(globalPos);
	QMenu               *menu      = nullptr;
	QString              href;
	QString              hint;
	NativeOutputPosition hit;
	bool                 textHit = false;
	if (nativeOutputHitTest(source, sourcePos, hit, &href, &hint, true, false, &textHit) && m_runtime)
		m_runtime->setWordUnderMenu(textHit ? wordAtNativeOutputPosition(hit) : QString(), true);
	const QVector<QPair<QString, QString>> actions      = parseMxpContextMenuActions(href, hint);
	const bool                             hasSelection = hasOutputSelection();
	if (!hasSelection && !actions.isEmpty())
	{
		menu = new QMenu(source);
		for (int i = 0; i < actions.size(); ++i)
		{
			const auto &entry  = actions.at(i);
			QAction    *action = menu->addAction(entry.second);
			connect(action, &QAction::triggered, this,
			        [this, actionText = entry.first] { emit hyperlinkActivated(actionText); });
			if (i == 0)
				menu->setDefaultAction(action);
		}
	}
	else
	{
		menu = new QMenu(source);
		if (QAction *copy = menu->addAction(QStringLiteral("Copy")); copy)
		{
			copy->setEnabled(hasSelection);
			connect(copy, &QAction::triggered, this, [this] { copySelection(); });
		}
		if (QAction *copyHtml = menu->addAction(QStringLiteral("Copy as HTML")); copyHtml)
		{
			copyHtml->setEnabled(hasSelection);
			connect(copyHtml, &QAction::triggered, this, [this] { copySelectionAsHtml(); });
		}
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
		if (QString hotspotId, windowName;
		    hitTestMiniWindow(local, hotspotId, windowName, true) && !hotspotId.isEmpty())
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

	if (QString hotspotId, windowName;
	    hitTestMiniWindow(local, hotspotId, windowName, true) && !hotspotId.isEmpty())
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
	if (!m_frozen)
	{
		if (m_runtime)
		{
			restoreOutputFromPersistedLines(m_runtime->lines());
			m_pendingOutput.clear();
		}
		else if (!m_pendingOutput.isEmpty())
		{
			m_flushingPending                    = true;
			const QVector<PendingOutput> pending = m_pendingOutput;
			m_pendingOutput.clear();
			for (const PendingOutput &entry : pending)
			{
				if (entry.injectBreakBeforeRender)
					commitPendingInlineInputBreak();
				appendOutputTextInternal(entry.text, entry.newLine, false, entry.flags, entry.spans);
			}
			m_flushingPending = false;
		}
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

int WorldView::outputScrollUnitsPerLine() const
{
	const WrapTextBrowser *const view = m_output ? m_output : m_liveOutput;
	if (!view)
		return 1;
	return qMax(1, QFontMetrics(view->font()).lineSpacing());
}

void WorldView::syncOutputScrollSingleStep() const
{
	const int singleStep = outputScrollUnitsPerLine();
	if (m_output)
	{
		if (QScrollBar *const bar = m_output->verticalScrollBar())
			bar->setSingleStep(singleStep);
	}
	if (m_liveOutput)
	{
		if (QScrollBar *const bar = m_liveOutput->verticalScrollBar())
			bar->setSingleStep(singleStep);
	}
	if (m_outputScrollBar)
		m_outputScrollBar->setSingleStep(singleStep);
}

void WorldView::syncNativeOutputScrollBarsFromLayout(const QVector<NativeOutputRenderLine> &lines,
                                                     const bool allowLayoutBuild) const
{
	if (!m_output || !m_nativeOutputCanvas)
		return;

	struct ScrollPane
	{
			QRect            textRect;
			WrapTextBrowser *view{nullptr};
	};

	QVector<ScrollPane> panes;
	panes.reserve(2);

	QRect primaryTextRect = nativeOutputPaneRect(m_output);
	if (primaryTextRect.isEmpty())
		primaryTextRect = outputTextRectangle();
	if (!primaryTextRect.isEmpty())
		panes.push_back({primaryTextRect, m_output});

	if (m_scrollbackSplitActive && m_liveOutput && m_liveOutput->isVisible())
	{
		const QRect liveTextRect = nativeOutputPaneRect(m_liveOutput);
		if (!liveTextRect.isEmpty())
			panes.push_back({liveTextRect, m_liveOutput});
	}

	if (panes.isEmpty())
		return;

	const bool wrapEnabled     = m_output->lineWrapMode() != WrapTextBrowser::NoWrap;
	const int  wrapWidthPixels = nativeWrapWidthPixels(panes.constFirst().textRect.width(), wrapEnabled);
	const int  localWrapWidthPixels =
	    nativeLocalWrapWidthPixels(panes.constFirst().textRect.width(), wrapEnabled);
	const int   lineSpacingSetting = qMax(0, m_lineSpacing);
	const QFont layoutFont         = m_output->font();
	const qreal lineSpacingFactor  = (100.0 + static_cast<qreal>(lineSpacingSetting)) / 100.0;
	const qreal fallbackLineAdvance =
	    static_cast<qreal>(qMax(1, QFontMetrics(layoutFont).lineSpacing())) * lineSpacingFactor;

	if (!lines.isEmpty() && !nativeLayoutCacheReadyFor(lines, wrapWidthPixels, localWrapWidthPixels,
	                                                   lineSpacingSetting, layoutFont))
	{
		if (!allowLayoutBuild)
			return;
		ensureNativeLayoutCaches(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacingSetting,
		                         layoutFont);
	}

	const qreal effectiveLineAdvance = (lines.isEmpty() || m_nativeLayoutCachedLineAdvance <= 0.0)
	                                       ? fallbackLineAdvance
	                                       : m_nativeLayoutCachedLineAdvance;
	const qreal docY          = (lines.isEmpty() || m_nativeLayoutCumulativeHeights.size() <= lines.size())
	                                ? 0.0
	                                : m_nativeLayoutCumulativeHeights.at(lines.size());
	const int   contentHeight = qMax(0, static_cast<int>(std::ceil(docY)));
	const int   lineStep      = qMax(1, static_cast<int>(std::round(effectiveLineAdvance)));

	{
		QScopedValueRollback<bool> scrollSyncGuard(m_nativeScrollSyncInPaint, true);
		for (const ScrollPane &pane : panes)
		{
			if (!pane.view)
				continue;
			QScrollBar *const bar = pane.view->verticalScrollBar();
			if (!bar)
				continue;

			const int  pageStep          = qMax(1, pane.textRect.height());
			const int  maxScroll         = qMax(0, contentHeight - pageStep);
			const int  oldValue          = bar->value();
			const int  oldMax            = bar->maximum();
			const bool wasAtEnd          = oldValue >= oldMax;
			const bool canAutoFollowTail = !(m_scrollbackSplitActive && pane.view == m_output);

			int        targetValue = qBound(0, oldValue, maxScroll);
			if (wasAtEnd && !m_frozen && canAutoFollowTail)
				targetValue = maxScroll;
			bool splitTopAnchorApplied = false;
			if (!canAutoFollowTail && m_nativePrimaryPaintState.valid &&
			    m_nativePrimaryPaintState.paneRect == pane.textRect &&
			    m_nativePrimaryPaintState.scrollY == oldValue &&
			    m_nativePrimaryPaintState.visibleLineKeys.size() ==
			        m_nativePrimaryPaintState.visibleLineIndexes.size() &&
			    m_nativePrimaryPaintState.visibleLineKeys.size() ==
			        m_nativePrimaryPaintState.visibleLineTops.size() &&
			    m_nativePrimaryPaintState.visibleLineKeys.size() ==
			        m_nativePrimaryPaintState.visibleFirstRuntimeLineNumbers.size() &&
			    m_nativeLayoutRuntimeLineKeys.size() == lines.size() &&
			    m_nativeLayoutCumulativeHeights.size() == lines.size() + 1)
			{
				const int headTrimCount =
				    (m_nativeRenderCacheDelta.revision == m_nativeRenderLineCacheRevision)
				        ? qMax(0, m_nativeRenderCacheDelta.headTrimCount)
				        : 0;
				auto findAnchorLineIndex = [this, &lines, headTrimCount](const int     oldLineIndex,
				                                                         const quint64 anchorKey,
				                                                         const qint64 anchorRuntimeLineNumber)
				{
					if (m_nativeLayoutRuntimeLineKeys.isEmpty())
						return -1;

					auto matchesAnchor = [this, anchorKey](const int lineIndex)
					{
						return anchorKey != 0 && lineIndex >= 0 &&
						       lineIndex < m_nativeLayoutRuntimeLineKeys.size() &&
						       m_nativeLayoutRuntimeLineKeys.at(lineIndex) == anchorKey;
					};
					auto containsAnchorRuntimeLine = [&lines, anchorRuntimeLineNumber](const int lineIndex)
					{
						return anchorRuntimeLineNumber > 0 && lineIndex >= 0 && lineIndex < lines.size() &&
						       WorldView::nativeRuntimeLineRangeContains(lines.at(lineIndex),
						                                                 anchorRuntimeLineNumber);
					};

					const int expectedIndex = qBound(0, oldLineIndex - headTrimCount,
					                                 sizeToInt(m_nativeLayoutRuntimeLineKeys.size()) - 1);
					if (matchesAnchor(expectedIndex))
						return expectedIndex;
					if (matchesAnchor(oldLineIndex))
						return oldLineIndex;

					const int searchRadius = qMin(256, sizeToInt(m_nativeLayoutRuntimeLineKeys.size()) - 1);
					for (int distance = 1; distance <= searchRadius; ++distance)
					{
						const int beforeIndex = expectedIndex - distance;
						if (matchesAnchor(beforeIndex))
							return beforeIndex;
						const int afterIndex = expectedIndex + distance;
						if (matchesAnchor(afterIndex))
							return afterIndex;
					}
					if (containsAnchorRuntimeLine(expectedIndex))
						return expectedIndex;
					for (int distance = 1; distance <= searchRadius; ++distance)
					{
						const int beforeIndex = expectedIndex - distance;
						if (containsAnchorRuntimeLine(beforeIndex))
							return beforeIndex;
						const int afterIndex = expectedIndex + distance;
						if (containsAnchorRuntimeLine(afterIndex))
							return afterIndex;
					}
					if (anchorRuntimeLineNumber > 0)
					{
						const bool searchFromTail = oldLineIndex > sizeToInt(lines.size()) / 2;
						return nativeRenderIndexForRuntimeLineNumber(anchorRuntimeLineNumber, searchFromTail);
					}
					return -1;
				};

				for (int anchorIndex = 0; anchorIndex < m_nativePrimaryPaintState.visibleLineKeys.size();
				     ++anchorIndex)
				{
					const quint64 anchorKey       = m_nativePrimaryPaintState.visibleLineKeys.at(anchorIndex);
					const int     anchorTopOffset = m_nativePrimaryPaintState.visibleLineTops.at(anchorIndex);
					const int     oldLineIndex = m_nativePrimaryPaintState.visibleLineIndexes.at(anchorIndex);
					const qint64  anchorRuntimeLineNumber =
					    m_nativePrimaryPaintState.visibleFirstRuntimeLineNumbers.at(anchorIndex);
					const int newLineIndex =
					    findAnchorLineIndex(oldLineIndex, anchorKey, anchorRuntimeLineNumber);
					if (newLineIndex < 0)
						continue;

					const int anchoredValue =
					    static_cast<int>(std::round(m_nativeLayoutCumulativeHeights.at(newLineIndex))) -
					    anchorTopOffset;
					targetValue           = qBound(0, anchoredValue, maxScroll);
					splitTopAnchorApplied = true;
					if (m_nativeRenderCacheDelta.revision == m_nativeRenderLineCacheRevision &&
					    m_nativeRenderCacheDelta.headTrimCount > 0)
					{
						m_nativeSplitTopHeadTrimAdjustedRevision = m_nativeRenderLineCacheRevision;
					}
					break;
				}
			}
			if (!splitTopAnchorApplied && !canAutoFollowTail &&
			    m_nativeSplitTopHeadTrimAdjustedRevision != m_nativeRenderLineCacheRevision &&
			    m_nativeRenderCacheDelta.revision == m_nativeRenderLineCacheRevision &&
			    m_nativeRenderCacheDelta.headTrimCount > 0 &&
			    m_nativeSplitTopHeadTrimPixelsRevision == m_nativeRenderLineCacheRevision)
			{
				const int trimmedScroll                  = qMax(0, m_nativeSplitTopHeadTrimPixels);
				targetValue                              = qMax(0, targetValue - trimmedScroll);
				m_nativeSplitTopHeadTrimAdjustedRevision = m_nativeRenderLineCacheRevision;
			}

			const bool needsSingleStepUpdate = bar->singleStep() != lineStep;
			const bool needsPageStepUpdate   = bar->pageStep() != pageStep;
			const bool needsRangeUpdate      = bar->minimum() != 0 || bar->maximum() != maxScroll;
			const bool needsValueUpdate      = bar->value() != targetValue;
			if (needsSingleStepUpdate || needsPageStepUpdate || needsRangeUpdate || needsValueUpdate)
			{
				const bool updatesEnabled = bar->updatesEnabled();
				if (updatesEnabled)
					bar->setUpdatesEnabled(false);
				if (needsSingleStepUpdate)
					bar->setSingleStep(lineStep);
				if (needsPageStepUpdate)
					bar->setPageStep(pageStep);
				if (needsRangeUpdate)
					bar->setRange(0, maxScroll);
				if (needsValueUpdate)
					bar->setValue(targetValue);
				if (updatesEnabled)
					bar->setUpdatesEnabled(true);
			}
			if (!canAutoFollowTail)
			{
				refreshNativeOutputPanePaintStateFromLayout(pane.view, pane.textRect, targetValue, maxScroll,
				                                            lines);
			}
		}

		if (m_outputScrollBar)
		{
			if (const QScrollBar *const outputBar = m_output->verticalScrollBar(); outputBar)
			{
				QSignalBlocker externalBlock(m_outputScrollBar);
				m_outputScrollBar->setRange(outputBar->minimum(), outputBar->maximum());
				m_outputScrollBar->setPageStep(outputBar->pageStep());
				m_outputScrollBar->setSingleStep(outputBar->singleStep());
				m_outputScrollBar->setValue(outputBar->value());
			}
		}
	}
}

void WorldView::requestNativeOutputRepaint() const
{
	requestNativeOutputRepaint({});
}

void WorldView::requestNativeOutputRepaint(const QRect &rect) const
{
	if (!m_nativeOutputCanvas)
		return;
	if (!m_nativeOutputRepaintQueued)
	{
		m_nativeOutputRepaintQueued = true;
		QMetaObject::invokeMethod(
		    const_cast<WorldView *>(this), [this] { flushQueuedNativeOutputRepaint(); },
		    Qt::QueuedConnection);
		if (rect.isValid())
			m_nativeOutputCanvas->update(rect);
		else
			m_nativeOutputCanvas->update();
		return;
	}

	if (rect.isValid())
	{
		m_nativeOutputRepaintRegion += rect;
	}
	else
	{
		m_nativeOutputRepaintAll    = true;
		m_nativeOutputRepaintRegion = {};
		m_nativeOutputCanvas->update();
	}
}

void WorldView::flushQueuedNativeOutputRepaint() const
{
	m_nativeOutputRepaintQueued = false;
	if (!m_nativeOutputCanvas)
	{
		m_nativeOutputRepaintAll    = false;
		m_nativeOutputRepaintRegion = {};
		return;
	}

	const bool    repaintAll    = m_nativeOutputRepaintAll;
	const QRegion repaintRegion = m_nativeOutputRepaintRegion;
	m_nativeOutputRepaintAll    = false;
	m_nativeOutputRepaintRegion = {};
	if (repaintAll)
		m_nativeOutputCanvas->update();
	else if (!repaintRegion.isEmpty())
		m_nativeOutputCanvas->update(repaintRegion);
}

void WorldView::requestNativeOutputTailRepaint() const
{
	if (!m_nativeOutputCanvas)
		return;

	const int tailBandHeight = qMax(16, outputScrollUnitsPerLine() * 4);
	QRect     repaintRect;
	auto      appendTailBand = [&repaintRect, tailBandHeight](const QRect &paneRect)
	{
		if (paneRect.isEmpty())
			return;
		const int   bandHeight = qMin(tailBandHeight, paneRect.height());
		const int   top        = qMax(paneRect.top(), paneRect.bottom() - bandHeight + 1);
		const QRect bandRect(paneRect.left(), top, paneRect.width(), bandHeight);
		repaintRect = repaintRect.isNull() ? bandRect : repaintRect.united(bandRect);
	};

	appendTailBand(nativeOutputPaneRect(m_output));
	if (m_scrollbackSplitActive && m_liveOutput && m_liveOutput->isVisible())
		appendTailBand(nativeOutputPaneRect(m_liveOutput));

	if (repaintRect.isValid())
		requestNativeOutputRepaint(repaintRect);
	else
		requestNativeOutputRepaint();
}

bool WorldView::nativeOutputDeltaRepaintRect(const QVector<NativeOutputRenderLine> &lines,
                                             QRect                                 &repaintRect) const
{
	repaintRect = {};
	if (!m_nativeOutputCanvas || !m_output || lines.isEmpty())
		return false;
	if (m_nativeRenderCacheDelta.revision != m_nativeRenderLineCacheRevision ||
	    m_nativeRenderCacheDelta.newLineCount != sizeToInt(lines.size()))
	{
		return false;
	}
	if (!m_nativeLayoutCacheValid || m_nativeLayoutCachedRenderRevision != m_nativeRenderLineCacheRevision ||
	    m_nativeLayoutCumulativeDirtyFrom < sizeToInt(lines.size()))
	{
		return false;
	}
	if (m_nativeLayoutCumulativeHeights.size() <= lines.size())
		return false;

	int       firstDirtyLine = -1;
	const int lineCount      = sizeToInt(lines.size());
	switch (m_nativeRenderCacheDelta.kind)
	{
	case NativeRenderCacheDeltaKind::TailAppend:
	case NativeRenderCacheDeltaKind::TailRemove:
	{
		const int oldLineCount = qBound(0, m_nativeRenderCacheDelta.oldLineCount, lineCount);
		firstDirtyLine         = qMax(0, oldLineCount - (m_nativeRenderCacheDelta.tailLineMutated ? 1 : 0));
		break;
	}
	case NativeRenderCacheDeltaKind::HeadTrimTailAppend:
	case NativeRenderCacheDeltaKind::RuntimeLineRestitch:
	case NativeRenderCacheDeltaKind::RuntimeRangeRestitch:
		firstDirtyLine = qBound(0, m_nativeRenderCacheDelta.stablePrefixCount, lineCount - 1);
		break;
	case NativeRenderCacheDeltaKind::HeadMutation:
	case NativeRenderCacheDeltaKind::FullReset:
	case NativeRenderCacheDeltaKind::Unknown:
		return false;
	}
	if (firstDirtyLine < 0 || firstDirtyLine >= lineCount)
		return false;

	const qreal documentHeight      = m_nativeLayoutCumulativeHeights.at(lines.size());
	auto        appendPaneDirtyRect = [&](const WrapTextBrowser *view)
	{
		if (!view || !view->isVisible())
			return;
		QRect paneRect = nativeOutputPaneRect(view);
		if (paneRect.isEmpty())
			return;

		const int nativeMaxScroll = qMax(0, static_cast<int>(std::ceil(documentHeight)) - paneRect.height());
		const int scrollY =
		    view->verticalScrollBar() ? qBound(0, view->verticalScrollBar()->value(), nativeMaxScroll) : 0;
		const qreal dirtyTop = static_cast<qreal>(paneRect.top()) +
		                       m_nativeLayoutCumulativeHeights.at(firstDirtyLine) -
		                       static_cast<qreal>(scrollY);
		const qreal dirtyBottom =
		    static_cast<qreal>(paneRect.top()) + documentHeight - static_cast<qreal>(scrollY);
		const int   dirtyY      = static_cast<int>(std::floor(dirtyTop)) - 2;
		const int   dirtyHeight = qMax(1, static_cast<int>(std::ceil(dirtyBottom)) - dirtyY + 3);
		const QRect paneDirtyRect =
		    QRect(paneRect.left(), dirtyY, paneRect.width(), dirtyHeight).intersected(paneRect);
		if (!paneDirtyRect.isEmpty())
			repaintRect = repaintRect.isNull() ? paneDirtyRect : repaintRect.united(paneDirtyRect);
	};

	appendPaneDirtyRect(m_output);
	if (m_scrollbackSplitActive && m_liveOutput && m_liveOutput->isVisible())
		appendPaneDirtyRect(m_liveOutput);
	return true;
}

QRect WorldView::nativeOutputVisiblePaneRepaintRect() const
{
	QRect repaintRect;
	auto  appendPaneRect = [this, &repaintRect](const WrapTextBrowser *view)
	{
		if (!view || !view->isVisible())
			return;
		const QRect paneRect = nativeOutputPaneRect(view);
		if (!paneRect.isEmpty())
			repaintRect = repaintRect.isNull() ? paneRect : repaintRect.united(paneRect);
	};

	appendPaneRect(m_output);
	if (m_scrollbackSplitActive && m_liveOutput && m_liveOutput->isVisible())
		appendPaneRect(m_liveOutput);
	return repaintRect;
}

bool WorldView::requestNativeOutputScrollAwarePresentationRepaint(
    const WorldView::NativeOutputRenderLines &lines) const
{
	if (!m_nativeOutputCanvas || !m_output || !m_output->isVisible() || lines.isEmpty())
		return false;
	if (m_scrollbackSplitActive || m_nativeOutputRepaintQueued || m_nativeOutputScrollBlitPending)
		return false;
	if (m_nativeRenderCacheDelta.revision != m_nativeRenderLineCacheRevision ||
	    m_nativeRenderCacheDelta.newLineCount != sizeToInt(lines.size()))
	{
		return false;
	}
	if (m_nativeRenderLineCacheRevision == 0 ||
	    m_nativePrimaryPaintState.renderRevision != m_nativeRenderLineCacheRevision - 1)
	{
		return false;
	}

	int selectionStartLine   = 0;
	int selectionStartColumn = 0;
	int selectionEndLine     = 0;
	int selectionEndColumn   = 0;
	if (nativeOutputSelectionBounds(selectionStartLine, selectionStartColumn, selectionEndLine,
	                                selectionEndColumn))
	{
		return false;
	}

	const QRect paneRect = nativeOutputPaneRect(m_output);
	if (paneRect.isEmpty() || !m_nativePrimaryPaintState.valid ||
	    m_nativePrimaryPaintState.paneRect != paneRect)
	{
		return false;
	}
	const QScrollBar *const scrollBar = m_output->verticalScrollBar();
	const int currentScrollY   = scrollBar ? qMax(0, scrollBar->value()) : qMax(0, outputScrollPosition());
	const int currentScrollMax = scrollBar ? qMax(0, scrollBar->maximum()) : 0;
	if (currentScrollY < currentScrollMax ||
	    m_nativePrimaryPaintState.scrollY < m_nativePrimaryPaintState.scrollMax)
		return false;

	QRect dirtyRect;
	if (!nativeOutputDeltaRepaintRect(lines, dirtyRect))
		return false;
	dirtyRect = dirtyRect.intersected(paneRect);

	const bool hasHeadTrim =
	    m_nativeRenderCacheDelta.kind == NativeRenderCacheDeltaKind::HeadTrimTailAppend ||
	    m_nativeRenderCacheDelta.headTrimCount > 0;
	int scrollDelta = currentScrollY - m_nativePrimaryPaintState.scrollY;
	if (hasHeadTrim)
	{
		if (m_nativeSplitTopHeadTrimPixelsRevision != m_nativeRenderLineCacheRevision ||
		    m_nativeSplitTopHeadTrimPixels <= 0)
			return false;
		scrollDelta += m_nativeSplitTopHeadTrimPixels;
	}

	if (scrollDelta == 0)
	{
		m_nativePrimaryPaintState.scrollY        = currentScrollY;
		m_nativePrimaryPaintState.scrollMax      = currentScrollMax;
		m_nativePrimaryPaintState.renderRevision = m_nativeRenderLineCacheRevision;
		if (dirtyRect.isValid())
			requestNativeOutputRepaint(dirtyRect);
		return true;
	}
	if (scrollDelta < 0 || scrollDelta >= paneRect.height())
		return false;

	if (m_bleedBackground || (m_runtime && !m_runtime->backgroundImage().isNull()))
	{
		return false;
	}
	if (m_nativePrimaryPaintState.visibleLineKeys.isEmpty() ||
	    m_nativePrimaryPaintState.visibleLineKeys.size() !=
	        m_nativePrimaryPaintState.visibleLineTops.size() ||
	    m_nativePrimaryPaintState.visibleLineKeys.size() !=
	        m_nativePrimaryPaintState.visibleLineBottoms.size())
	{
		return false;
	}

	const bool  wrapEnabled          = m_output->lineWrapMode() != WrapTextBrowser::NoWrap;
	const int   wrapWidthPixels      = nativeWrapWidthPixels(paneRect.width(), wrapEnabled);
	const int   localWrapWidthPixels = nativeLocalWrapWidthPixels(paneRect.width(), wrapEnabled);
	const int   lineSpacingSetting   = qMax(0, m_lineSpacing);
	const QFont layoutFont           = m_output->font();
	if (!nativeLayoutCacheReadyFor(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacingSetting,
	                               layoutFont) ||
	    m_nativeLayoutCumulativeHeights.size() <= lines.size() ||
	    m_nativeLayoutRowsExact.size() != lines.size())
	{
		return false;
	}

	const auto visibleLineRangeForScroll =
	    [this, &lines](const int scrollY, const int paneHeight, int &firstLine, int &lastLine)
	{
		const auto visibleTop    = static_cast<qreal>(scrollY);
		const auto visibleBottom = static_cast<qreal>(scrollY + paneHeight - 1);
		auto       firstVisibleIt =
		    std::ranges::upper_bound(std::ranges::subrange(m_nativeLayoutCumulativeHeights.cbegin() + 1,
		                                                   m_nativeLayoutCumulativeHeights.cend()),
		                             visibleTop);
		firstLine = qMax(0, sizeToInt(firstVisibleIt - m_nativeLayoutCumulativeHeights.cbegin()) - 1);
		auto lastVisibleIt =
		    std::ranges::upper_bound(std::ranges::subrange(m_nativeLayoutCumulativeHeights.cbegin(),
		                                                   m_nativeLayoutCumulativeHeights.cend()),
		                             visibleBottom);
		lastLine = qMin(sizeToInt(lines.size()) - 1,
		                sizeToInt(lastVisibleIt - m_nativeLayoutCumulativeHeights.cbegin()) - 1);
		return firstLine <= lastLine;
	};

	int currentFirstVisibleLine = 0;
	int currentLastVisibleLine  = 0;
	if (!visibleLineRangeForScroll(currentScrollY, paneRect.height(), currentFirstVisibleLine,
	                               currentLastVisibleLine))
	{
		return false;
	}
	if (ensureNativeLayoutRange(lines, qMax(0, currentFirstVisibleLine - 2),
	                            qMin(sizeToInt(lines.size()) - 1, currentLastVisibleLine + 2),
	                            wrapWidthPixels, localWrapWidthPixels, lineSpacingSetting, layoutFont))
	{
		if (!visibleLineRangeForScroll(currentScrollY, paneRect.height(), currentFirstVisibleLine,
		                               currentLastVisibleLine))
		{
			return false;
		}
	}

	QVector<quint64> currentLineKeys;
	QVector<int>     currentLineTops;
	QVector<int>     currentLineBottoms;
	currentLineKeys.reserve(currentLastVisibleLine - currentFirstVisibleLine + 1);
	currentLineTops.reserve(currentLastVisibleLine - currentFirstVisibleLine + 1);
	currentLineBottoms.reserve(currentLastVisibleLine - currentFirstVisibleLine + 1);
	for (int i = currentFirstVisibleLine; i <= currentLastVisibleLine; ++i)
	{
		if (m_nativeLayoutRowsExact.at(i) == 0)
			return false;
		currentLineKeys.push_back(nativeRuntimeLineKey(lines.at(i)));
		currentLineTops.push_back(
		    static_cast<int>(std::round(m_nativeLayoutCumulativeHeights.at(i) - currentScrollY)));
		currentLineBottoms.push_back(
		    static_cast<int>(std::round(m_nativeLayoutCumulativeHeights.at(i + 1) - currentScrollY)));
	}

	int mappedLineCount = 0;
	for (int oldIndex = 0; oldIndex < m_nativePrimaryPaintState.visibleLineKeys.size(); ++oldIndex)
	{
		const int shiftedTop    = m_nativePrimaryPaintState.visibleLineTops.at(oldIndex) - scrollDelta;
		const int shiftedBottom = m_nativePrimaryPaintState.visibleLineBottoms.at(oldIndex) - scrollDelta;
		if (shiftedBottom <= 0 || shiftedTop >= paneRect.height())
			continue;

		const quint64 lineKey     = m_nativePrimaryPaintState.visibleLineKeys.at(oldIndex);
		bool          matchedLine = false;
		for (int currentIndex = 0; currentIndex < currentLineKeys.size(); ++currentIndex)
		{
			if (currentLineKeys.at(currentIndex) != lineKey)
				continue;
			if (std::abs(currentLineTops.at(currentIndex) - shiftedTop) > 1 ||
			    std::abs(currentLineBottoms.at(currentIndex) - shiftedBottom) > 1)
			{
				return false;
			}
			matchedLine = true;
			break;
		}
		if (!matchedLine)
			return false;
		++mappedLineCount;
	}
	if (mappedLineCount <= 0)
		return false;

	m_nativeOutputCanvas->scroll(0, -scrollDelta, paneRect);
	const QRect exposedRect(paneRect.left(), paneRect.bottom() - scrollDelta + 1, paneRect.width(),
	                        scrollDelta);
	QRect       repaintRect = exposedRect;
	if (dirtyRect.isValid())
		repaintRect = repaintRect.united(dirtyRect);
	if (m_miniWindowPaintBoundsValid && !m_miniWindowPaintBounds.underlayBounds.isEmpty())
		repaintRect = repaintRect.united(m_miniWindowPaintBounds.underlayBounds.intersected(paneRect));
	repaintRect = repaintRect.intersected(paneRect);
	if (!repaintRect.isValid())
		return false;

	m_nativePrimaryPaintState.scrollY        = currentScrollY;
	m_nativePrimaryPaintState.scrollMax      = currentScrollMax;
	m_nativePrimaryPaintState.renderRevision = m_nativeRenderLineCacheRevision;
	m_nativeOutputScrollBlitPending          = true;
	m_nativeOutputScrollBlitExposedRect      = exposedRect.intersected(paneRect);
	requestNativeOutputRepaint(repaintRect);
	return true;
}

void WorldView::requestNativeOutputPresentationRepaint(const bool repaintVisiblePanes) const
{
	requestNativeOutputPresentationRepaint(repaintVisiblePanes, nativeOutputRenderLines());
}

void WorldView::requestNativeOutputPresentationRepaint(const bool repaintVisiblePanes,
                                                       const WorldView::NativeOutputRenderLines &lines) const
{
	if (repaintVisiblePanes)
	{
		if (requestNativeOutputScrollAwarePresentationRepaint(lines))
			return;

		const QRect repaintRect = nativeOutputVisiblePaneRepaintRect();
		if (repaintRect.isValid())
			requestNativeOutputRepaint(repaintRect);
		else
			requestNativeOutputRepaint();
		return;
	}

	QRect repaintRect;
	if (nativeOutputDeltaRepaintRect(lines, repaintRect))
	{
		if (repaintRect.isValid())
			requestNativeOutputRepaint(repaintRect);
		return;
	}
	requestNativeOutputRepaint();
}

QMudAccessibleTextUtils::LineOffsetMap
WorldView::accessibleNativeOutputTextMap(const QVector<NativeOutputRenderLine> &lines)
{
	QVector<QString> textLines;
	textLines.reserve(lines.size());
	for (const NativeOutputRenderLine &line : lines)
		textLines.push_back(line.text);
	return QMudAccessibleTextUtils::LineOffsetMap(std::move(textLines));
}

void WorldView::primeAccessibleOutputTextState() const
{
	const QMudAccessibleTextUtils::LineOffsetMap map =
	    accessibleNativeOutputTextMap(nativeOutputRenderLines());
	m_accessibleOutputCharacterCount = map.characterCount();
	m_accessibleOutputRevision       = m_nativeRenderLineCacheRevision;
	m_accessibleOutputText           = map.text(0, map.characterCount());
}

void WorldView::notifyAccessibleOutputPresented(const QVector<NativeOutputRenderLine> &lines) const
{
	if (!QAccessible::isActive())
	{
		m_accessibleOutputCharacterCount = -1;
		m_accessibleOutputRevision       = 0;
		m_accessibleOutputText.clear();
		m_accessibleOutputPendingTailAppend = false;
		return;
	}

	const QMudAccessibleTextUtils::LineOffsetMap map                   = accessibleNativeOutputTextMap(lines);
	const int                                    currentCharacterCount = map.characterCount();
	const QString                                currentText           = map.text(0, currentCharacterCount);
	if (m_accessibleOutputCharacterCount < 0)
	{
		m_accessibleOutputCharacterCount = currentCharacterCount;
		m_accessibleOutputRevision       = m_nativeRenderLineCacheRevision;
		m_accessibleOutputText           = currentText;
		return;
	}

	const bool revisionChanged = m_accessibleOutputRevision != m_nativeRenderLineCacheRevision;
	if (!revisionChanged)
	{
		m_accessibleOutputCharacterCount = currentCharacterCount;
		m_accessibleOutputText           = currentText;
		return;
	}

	const bool exactTailAppend = m_accessibleOutputPendingTailAppend &&
	                             (m_nativeRenderCacheDelta.kind == NativeRenderCacheDeltaKind::TailAppend ||
	                              !m_nativeRenderLineCacheFromRuntime);
	auto       announceCanvasText = [this](const QString &message)
	{
		const QString announcement = trimLeadingAnnouncementBreaks(message);
		if (!m_nativeOutputCanvas || !m_nativeOutputCanvas->isVisible() || announcement.isEmpty())
			return;
		QAccessibleAnnouncementEvent event(m_nativeOutputCanvas, announcement);
		event.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
		QAccessible::updateAccessibility(&event);
	};
	if (currentCharacterCount > m_accessibleOutputCharacterCount && exactTailAppend)
	{
		const QString insertedText = map.text(m_accessibleOutputCharacterCount, currentCharacterCount);
		if (!insertedText.isEmpty())
		{
			if (m_nativeOutputCanvas && m_nativeOutputCanvas->isVisible())
			{
				QAccessibleTextInsertEvent event(m_nativeOutputCanvas, m_accessibleOutputCharacterCount,
				                                 insertedText);
				QAccessible::updateAccessibility(&event);
			}
			announceCanvasText(insertedText);
		}
	}
	else if (m_nativeOutputCanvas && m_nativeOutputCanvas->isVisible())
	{
		if (m_accessibleOutputText != currentText)
		{
			QAccessibleTextUpdateEvent event(m_nativeOutputCanvas, 0, m_accessibleOutputText, currentText);
			QAccessible::updateAccessibility(&event);
			announceCanvasText(accessibleAnnouncementDelta(m_accessibleOutputText, currentText));
		}
	}
	m_accessibleOutputCharacterCount    = currentCharacterCount;
	m_accessibleOutputRevision          = m_nativeRenderLineCacheRevision;
	m_accessibleOutputText              = currentText;
	m_accessibleOutputPendingTailAppend = false;
}

WorldView::NativeOutputPanePaintState *
WorldView::nativeOutputPanePaintStateForView(const WrapTextBrowser *view) const
{
	if (view == m_output)
		return &m_nativePrimaryPaintState;
	if (view == m_liveOutput)
		return &m_nativeLivePaintState;
	return nullptr;
}

void WorldView::refreshNativeOutputPanePaintStateFromLayout(
    const WrapTextBrowser *view, const QRect &paneRect, const int scrollY, const int scrollMax,
    const QVector<NativeOutputRenderLine> &lines) const
{
	NativeOutputPanePaintState *const state = nativeOutputPanePaintStateForView(view);
	if (!state || paneRect.isEmpty())
		return;

	QVector<int>     visibleLineIndexes;
	QVector<quint64> visibleLineKeys;
	QVector<qint64>  visibleFirstRuntimeLineNumbers;
	QVector<qint64>  visibleLastRuntimeLineNumbers;
	QVector<int>     visibleLineTops;
	QVector<int>     visibleLineBottoms;
	if (!lines.isEmpty() && m_nativeLayoutCumulativeHeights.size() > lines.size() &&
	    m_nativeLayoutRowsExact.size() == lines.size())
	{
		const auto visibleTop    = static_cast<qreal>(scrollY);
		const auto visibleBottom = static_cast<qreal>(scrollY + paneRect.height() - 1);
		auto       firstVisibleIt =
		    std::ranges::upper_bound(std::ranges::subrange(m_nativeLayoutCumulativeHeights.cbegin() + 1,
		                                                   m_nativeLayoutCumulativeHeights.cend()),
		                             visibleTop);
		int firstVisibleLine =
		    qMax(0, sizeToInt(firstVisibleIt - m_nativeLayoutCumulativeHeights.cbegin()) - 1);
		auto lastVisibleIt =
		    std::ranges::upper_bound(std::ranges::subrange(m_nativeLayoutCumulativeHeights.cbegin(),
		                                                   m_nativeLayoutCumulativeHeights.cend()),
		                             visibleBottom);
		int lastVisibleLine = qMin(sizeToInt(lines.size()) - 1,
		                           sizeToInt(lastVisibleIt - m_nativeLayoutCumulativeHeights.cbegin()) - 1);
		if (firstVisibleLine <= lastVisibleLine)
		{
			visibleLineIndexes.reserve(lastVisibleLine - firstVisibleLine + 1);
			visibleLineKeys.reserve(lastVisibleLine - firstVisibleLine + 1);
			visibleFirstRuntimeLineNumbers.reserve(lastVisibleLine - firstVisibleLine + 1);
			visibleLastRuntimeLineNumbers.reserve(lastVisibleLine - firstVisibleLine + 1);
			visibleLineTops.reserve(lastVisibleLine - firstVisibleLine + 1);
			visibleLineBottoms.reserve(lastVisibleLine - firstVisibleLine + 1);
			for (int i = firstVisibleLine; i <= lastVisibleLine; ++i)
			{
				const NativeOutputRenderLine &line = lines.at(i);
				visibleLineIndexes.push_back(i);
				visibleLineKeys.push_back(nativeRuntimeLineKey(line));
				visibleFirstRuntimeLineNumbers.push_back(line.firstRuntimeLineNumber);
				visibleLastRuntimeLineNumbers.push_back(line.lastRuntimeLineNumber);
				visibleLineTops.push_back(
				    static_cast<int>(std::round(m_nativeLayoutCumulativeHeights.at(i) - visibleTop)));
				visibleLineBottoms.push_back(
				    static_cast<int>(std::round(m_nativeLayoutCumulativeHeights.at(i + 1) - visibleTop)));
			}
		}
	}

	state->paneRect                       = paneRect;
	state->scrollY                        = scrollY;
	state->scrollMax                      = scrollMax;
	state->renderRevision                 = m_nativeRenderLineCacheRevision;
	state->visibleLineIndexes             = std::move(visibleLineIndexes);
	state->visibleLineKeys                = std::move(visibleLineKeys);
	state->visibleFirstRuntimeLineNumbers = std::move(visibleFirstRuntimeLineNumbers);
	state->visibleLastRuntimeLineNumbers  = std::move(visibleLastRuntimeLineNumbers);
	state->visibleLineTops                = std::move(visibleLineTops);
	state->visibleLineBottoms             = std::move(visibleLineBottoms);
	state->valid                          = true;
}

void WorldView::updateNativeOutputPanePaintState(const WrapTextBrowser *view, const QRect &paneRect,
                                                 const QRegion &paintedRegion, const int scrollY,
                                                 const int                              scrollMax,
                                                 const QVector<NativeOutputRenderLine> &lines) const
{
	NativeOutputPanePaintState *const state = nativeOutputPanePaintStateForView(view);
	if (!state || paneRect.isEmpty())
		return;

	const bool fullPanePainted     = QRegion(paneRect).subtracted(paintedRegion).isEmpty();
	const bool confirmsScrollBlit  = m_nativeOutputScrollBlitPending && view == m_output;
	const bool confirmsCurrentPane = state->valid && state->paneRect == paneRect &&
	                                 state->scrollY == scrollY &&
	                                 state->renderRevision == m_nativeRenderLineCacheRevision;
	if (!fullPanePainted && !confirmsScrollBlit && !confirmsCurrentPane)
		return;

	refreshNativeOutputPanePaintStateFromLayout(view, paneRect, scrollY, scrollMax, lines);
}

void WorldView::primeNativeOutputCaches() const
{
	if (!m_output || !m_output->viewport() || !m_nativeOutputCanvas)
		return;
	if (!isVisible() || !m_output->viewport()->isVisible())
		return;
	if (m_runtime && !m_runtime->isActive())
		return;

	const QRect viewportRect = m_output->viewport()->rect();
	if (viewportRect.width() <= 0 || viewportRect.height() <= 0)
		return;

	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	if (lines.isEmpty())
		return;

	const bool  wrapEnabled          = m_output->lineWrapMode() != WrapTextBrowser::NoWrap;
	const int   wrapWidthPixels      = nativeWrapWidthPixels(viewportRect.width(), wrapEnabled);
	const int   localWrapWidthPixels = nativeLocalWrapWidthPixels(viewportRect.width(), wrapEnabled);
	const int   lineSpacingSetting   = qMax(0, m_lineSpacing);
	const QFont layoutFont           = m_output->font();
	if (!nativeLayoutCacheReadyFor(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacingSetting,
	                               layoutFont))
	{
		ensureNativeLayoutCaches(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacingSetting,
		                         layoutFont);
	}
}

QRect WorldView::nativeOutputPaneRect(const WrapTextBrowser *view) const
{
	if (!m_nativeOutputCanvas || !view || !view->viewport())
		return {};
	const QPoint globalTopLeft = view->viewport()->mapToGlobal(QPoint(0, 0));
	return {m_nativeOutputCanvas->mapFromGlobal(globalTopLeft), view->viewport()->size()};
}

bool WorldView::nativeServerSideWrapActive() const
{
	return m_runtime && m_runtime->isConnected() && m_runtime->isNawsNegotiated();
}

int WorldView::nativeWrapWidthPixels(const int viewportWidth, const bool wrapEnabled) const
{
	if (!wrapEnabled)
		return 1000000;
	if (nativeServerSideWrapActive())
		return 1000000;
	const bool inactiveAndNotExposed =
	    m_runtime && !m_runtime->isActive() &&
	    (!isVisible() || (m_nativeOutputCanvas && !m_nativeOutputCanvas->isVisible()) ||
	     (window() && !window()->isActiveWindow()));
	if (inactiveAndNotExposed && m_nativeLayoutCacheValid && m_nativeLayoutCachedWrapWidth > 0)
		return m_nativeLayoutCachedWrapWidth;
	return qMax(1, viewportWidth);
}

int WorldView::nativeLocalWrapWidthPixels(const int viewportWidth, const bool wrapEnabled) const
{
	if (!wrapEnabled)
		return 1000000;
	if (nativeServerSideWrapActive())
		return 1000000;
	const bool inactiveAndNotExposed =
	    m_runtime && !m_runtime->isActive() &&
	    (!isVisible() || (m_nativeOutputCanvas && !m_nativeOutputCanvas->isVisible()) ||
	     (window() && !window()->isActiveWindow()));
	if (inactiveAndNotExposed && m_nativeLayoutCacheValid && m_nativeLayoutCachedLocalWrapWidth > 0)
		return m_nativeLayoutCachedLocalWrapWidth;
	return qMax(1, viewportWidth);
}

quint64 WorldView::nativeLineContentHash(const NativeOutputRenderLine &line)
{
	quint64 seed = 1469598103934665603ULL;
	seed         = hashCombine(seed, hashStringContent(line.text));
	seed         = hashCombine(seed, static_cast<quint64>(line.spans.size()));
	for (const WorldRuntime::StyleSpan &span : line.spans)
	{
		seed = hashCombine(seed, static_cast<quint64>(span.length));
		seed = hashCombine(seed, static_cast<quint64>(span.actionType));
		seed = hashCombine(seed, static_cast<quint64>(span.bold ? 1 : 0));
		seed = hashCombine(seed, static_cast<quint64>(span.underline ? 1 : 0));
		seed = hashCombine(seed, static_cast<quint64>(span.italic ? 1 : 0));
		seed = hashCombine(seed, static_cast<quint64>(span.strike ? 1 : 0));
		seed = hashCombine(seed, static_cast<quint64>(span.inverse ? 1 : 0));
		seed = hashCombine(seed, static_cast<quint64>(span.fore.isValid() ? span.fore.rgba() : 0));
		seed = hashCombine(seed, static_cast<quint64>(span.back.isValid() ? span.back.rgba() : 0));
		seed = hashCombine(seed, static_cast<quint64>(!span.action.isEmpty() ? 1 : 0));
	}
	return seed;
}

void WorldView::recordNativeAppendDiagnostic(const NativeAppendDiagnosticKind kind,
                                             const int runtimeStartIndex, const int rebuiltRuntimeCount) const
{
#ifdef NDEBUG
	Q_UNUSED(kind);
	Q_UNUSED(runtimeStartIndex);
	Q_UNUSED(rebuiltRuntimeCount);
	Q_UNUSED(m_nativeTailAppendDiag);
#else
	if (rebuiltRuntimeCount <= 0)
		return;

	NativeAppendDiagnosticBucket *bucket = nullptr;
	switch (kind)
	{
	case NativeAppendDiagnosticKind::TailAppend:
		bucket = &m_nativeTailAppendDiag;
		break;
	case NativeAppendDiagnosticKind::NonContiguousTailAppend:
		bucket = &m_nativeNonContiguousTailAppendDiag;
		break;
	case NativeAppendDiagnosticKind::HeadTrimTailAppend:
		bucket = &m_nativeHeadTrimAppendDiag;
		break;
	case NativeAppendDiagnosticKind::TailRestitch:
		bucket = &m_nativeTailRestitchAppendDiag;
		break;
	case NativeAppendDiagnosticKind::RangeRestitch:
		bucket = &m_nativeRangeRestitchAppendDiag;
		break;
	case NativeAppendDiagnosticKind::SoftCacheInvalid:
		bucket = &m_nativeSoftCacheInvalidDiag;
		break;
	case NativeAppendDiagnosticKind::SoftRuntimeDisjoint:
		bucket = &m_nativeSoftRuntimeDisjointDiag;
		break;
	case NativeAppendDiagnosticKind::SoftNonContiguousNoOverlap:
		bucket = &m_nativeSoftNonContiguousNoOverlapDiag;
		break;
	case NativeAppendDiagnosticKind::SoftRestitchFailure:
		bucket = &m_nativeSoftRestitchFailureDiag;
		break;
	case NativeAppendDiagnosticKind::SoftAppendStartOutOfRange:
		bucket = &m_nativeSoftAppendStartOutOfRangeDiag;
		break;
	case NativeAppendDiagnosticKind::FullRebuild:
		bucket = &m_nativeFullRebuildAppendDiag;
		break;
	}

	++bucket->count;
	bucket->minStart = bucket->minStart < 0 ? runtimeStartIndex : qMin(bucket->minStart, runtimeStartIndex);
	bucket->rebuiltTotal += rebuiltRuntimeCount;
	bucket->rebuiltMax = qMax(bucket->rebuiltMax, rebuiltRuntimeCount);
#endif
}

void WorldView::recordNativeRestitchFailureDiagnostic(const NativeRestitchFailureDiagnosticKind kind,
                                                      const int runtimeStartIndex) const
{
#ifdef NDEBUG
	Q_UNUSED(kind);
	Q_UNUSED(runtimeStartIndex);
	Q_UNUSED(m_nativeRestitchFailRangeDropMissDiag);
#else
	NativeAppendDiagnosticBucket *bucket = nullptr;
	switch (kind)
	{
	case NativeRestitchFailureDiagnosticKind::RangeHeadTrim:
		bucket = &m_nativeRestitchFailRangeHeadTrimDiag;
		break;
	case NativeRestitchFailureDiagnosticKind::RangeEmpty:
		bucket = &m_nativeRestitchFailRangeEmptyDiag;
		break;
	case NativeRestitchFailureDiagnosticKind::RangeDropMiss:
		bucket = &m_nativeRestitchFailRangeDropMissDiag;
		break;
	case NativeRestitchFailureDiagnosticKind::RangeAppendNoChange:
		bucket = &m_nativeRestitchFailRangeAppendNoChangeDiag;
		break;
	case NativeRestitchFailureDiagnosticKind::LineHiddenOrOpen:
		bucket = &m_nativeRestitchFailLineHiddenOrOpenDiag;
		break;
	case NativeRestitchFailureDiagnosticKind::LineRenderMiss:
		bucket = &m_nativeRestitchFailLineRenderMissDiag;
		break;
	case NativeRestitchFailureDiagnosticKind::LineComposite:
		bucket = &m_nativeRestitchFailLineCompositeDiag;
		break;
	case NativeRestitchFailureDiagnosticKind::TailIndexMiss:
		bucket = &m_nativeRestitchFailTailIndexMissDiag;
		break;
	case NativeRestitchFailureDiagnosticKind::TailAppendNoChange:
		bucket = &m_nativeRestitchFailTailAppendNoChangeDiag;
		break;
	case NativeRestitchFailureDiagnosticKind::HeadTrim:
		bucket = &m_nativeRestitchFailHeadTrimDiag;
		break;
	}

	++bucket->count;
	bucket->minStart = bucket->minStart < 0 ? runtimeStartIndex : qMin(bucket->minStart, runtimeStartIndex);
#endif
}

void WorldView::logAndResetNativeAppendDiagnostics() const
{
#ifdef NDEBUG
	Q_UNUSED(m_nativeTailAppendDiag);
#else
	QStringList parts;
	auto        appendPart = [&parts](const QString &label, const NativeAppendDiagnosticBucket &bucket)
	{
		if (bucket.count <= 0)
			return;
		parts.push_back(QStringLiteral("%1=%2/%3/%4/%5")
		                    .arg(label)
		                    .arg(bucket.count)
		                    .arg(bucket.minStart)
		                    .arg(bucket.rebuiltTotal)
		                    .arg(bucket.rebuiltMax));
	};

	appendPart(QStringLiteral("tail"), m_nativeTailAppendDiag);
	appendPart(QStringLiteral("noncontigTail"), m_nativeNonContiguousTailAppendDiag);
	appendPart(QStringLiteral("headTail"), m_nativeHeadTrimAppendDiag);
	appendPart(QStringLiteral("tailRestitch"), m_nativeTailRestitchAppendDiag);
	appendPart(QStringLiteral("rangeRestitch"), m_nativeRangeRestitchAppendDiag);
	appendPart(QStringLiteral("softInvalid"), m_nativeSoftCacheInvalidDiag);
	appendPart(QStringLiteral("softDisjoint"), m_nativeSoftRuntimeDisjointDiag);
	appendPart(QStringLiteral("softNoOverlap"), m_nativeSoftNonContiguousNoOverlapDiag);
	appendPart(QStringLiteral("softRestitch"), m_nativeSoftRestitchFailureDiag);
	appendPart(QStringLiteral("softAppendIndex"), m_nativeSoftAppendStartOutOfRangeDiag);
	appendPart(QStringLiteral("full"), m_nativeFullRebuildAppendDiag);

	if (!parts.isEmpty())
		qInfo().noquote().nospace() << "[QMud][NativeAppend] " << parts.join(QLatin1Char(' '));

	QStringList restitchParts;
	auto        appendRestitchPart =
	    [&restitchParts](const QString &label, const NativeAppendDiagnosticBucket &bucket)
	{
		if (bucket.count <= 0)
			return;
		restitchParts.push_back(QStringLiteral("%1=%2/%3").arg(label).arg(bucket.count).arg(bucket.minStart));
	};
	appendRestitchPart(QStringLiteral("rangeHeadTrim"), m_nativeRestitchFailRangeHeadTrimDiag);
	appendRestitchPart(QStringLiteral("rangeEmpty"), m_nativeRestitchFailRangeEmptyDiag);
	appendRestitchPart(QStringLiteral("rangeDropMiss"), m_nativeRestitchFailRangeDropMissDiag);
	appendRestitchPart(QStringLiteral("rangeNoChange"), m_nativeRestitchFailRangeAppendNoChangeDiag);
	appendRestitchPart(QStringLiteral("lineHiddenOpen"), m_nativeRestitchFailLineHiddenOrOpenDiag);
	appendRestitchPart(QStringLiteral("lineRenderMiss"), m_nativeRestitchFailLineRenderMissDiag);
	appendRestitchPart(QStringLiteral("lineComposite"), m_nativeRestitchFailLineCompositeDiag);
	appendRestitchPart(QStringLiteral("tailIndexMiss"), m_nativeRestitchFailTailIndexMissDiag);
	appendRestitchPart(QStringLiteral("tailNoChange"), m_nativeRestitchFailTailAppendNoChangeDiag);
	appendRestitchPart(QStringLiteral("headTrim"), m_nativeRestitchFailHeadTrimDiag);
	if (!restitchParts.isEmpty())
		qInfo().noquote().nospace() << "[QMud][RestitchFail] " << restitchParts.join(QLatin1Char(' '));

	m_nativeTailAppendDiag                      = {};
	m_nativeNonContiguousTailAppendDiag         = {};
	m_nativeHeadTrimAppendDiag                  = {};
	m_nativeTailRestitchAppendDiag              = {};
	m_nativeRangeRestitchAppendDiag             = {};
	m_nativeSoftCacheInvalidDiag                = {};
	m_nativeSoftRuntimeDisjointDiag             = {};
	m_nativeSoftNonContiguousNoOverlapDiag      = {};
	m_nativeSoftRestitchFailureDiag             = {};
	m_nativeSoftAppendStartOutOfRangeDiag       = {};
	m_nativeFullRebuildAppendDiag               = {};
	m_nativeRestitchFailRangeHeadTrimDiag       = {};
	m_nativeRestitchFailRangeEmptyDiag          = {};
	m_nativeRestitchFailRangeDropMissDiag       = {};
	m_nativeRestitchFailRangeAppendNoChangeDiag = {};
	m_nativeRestitchFailLineHiddenOrOpenDiag    = {};
	m_nativeRestitchFailLineRenderMissDiag      = {};
	m_nativeRestitchFailLineCompositeDiag       = {};
	m_nativeRestitchFailTailIndexMissDiag       = {};
	m_nativeRestitchFailTailAppendNoChangeDiag  = {};
	m_nativeRestitchFailHeadTrimDiag            = {};
#endif
}

void WorldView::extendNativeRuntimeLineRange(NativeOutputRenderLine &line, const qint64 lineNumber)
{
	if (lineNumber <= 0)
		return;
	const qint64 previousFirstRuntimeLine = line.firstRuntimeLineNumber;
	const qint64 previousLastRuntimeLine  = line.lastRuntimeLineNumber;
	if (line.firstRuntimeLineNumber <= 0 || lineNumber < line.firstRuntimeLineNumber)
		line.firstRuntimeLineNumber = lineNumber;
	if (line.lastRuntimeLineNumber <= 0 || lineNumber > line.lastRuntimeLineNumber)
		line.lastRuntimeLineNumber = lineNumber;
	if (line.sourceRuntimeLineNumbers.isEmpty() && previousFirstRuntimeLine > 0 &&
	    previousFirstRuntimeLine != lineNumber)
	{
		line.sourceRuntimeLineNumbers.push_back(previousFirstRuntimeLine);
		if (previousLastRuntimeLine > 0 && previousLastRuntimeLine != previousFirstRuntimeLine &&
		    previousLastRuntimeLine != lineNumber)
		{
			line.sourceRuntimeLineNumbers.push_back(previousLastRuntimeLine);
		}
	}
	if (line.sourceRuntimeLineNumbers.isEmpty() || line.sourceRuntimeLineNumbers.constLast() != lineNumber)
		line.sourceRuntimeLineNumbers.push_back(lineNumber);
	line.sourceRuntimeLineKey = nativeRuntimeLineKey(line.sourceRuntimeLineNumbers,
	                                                 line.firstRuntimeLineNumber, line.lastRuntimeLineNumber);
}

bool WorldView::nativeRuntimeLineRangeContains(const NativeOutputRenderLine &line, const qint64 lineNumber)
{
	if (lineNumber <= 0)
		return false;
	if (line.firstRuntimeLineNumber > 0 && line.lastRuntimeLineNumber > 0 &&
	    (lineNumber < line.firstRuntimeLineNumber || lineNumber > line.lastRuntimeLineNumber))
	{
		return false;
	}
	if (!line.sourceRuntimeLineNumbers.isEmpty())
		return line.sourceRuntimeLineNumbers.contains(lineNumber);
	if (line.firstRuntimeLineNumber <= 0 || line.lastRuntimeLineNumber <= 0)
		return false;
	return line.firstRuntimeLineNumber <= lineNumber && line.lastRuntimeLineNumber >= lineNumber;
}

QPair<qint64, qint64> WorldView::nativeRuntimeLineRange(const NativeOutputRenderLine &line)
{
	return {line.firstRuntimeLineNumber, line.lastRuntimeLineNumber};
}

QVector<qint64> WorldView::nativeRuntimeLineNumbers(const NativeOutputRenderLine &line)
{
	if (!line.sourceRuntimeLineNumbers.isEmpty())
		return line.sourceRuntimeLineNumbers;
	QVector<qint64> lineNumbers;
	if (line.firstRuntimeLineNumber > 0)
		lineNumbers.push_back(line.firstRuntimeLineNumber);
	if (line.lastRuntimeLineNumber > 0 && line.lastRuntimeLineNumber != line.firstRuntimeLineNumber)
		lineNumbers.push_back(line.lastRuntimeLineNumber);
	return lineNumbers;
}

int WorldView::nativeRenderIndexForRuntimeLineNumber(const qint64 lineNumber, const bool searchFromTail) const
{
	if (lineNumber <= 0)
		return -1;

	const int renderLineCount = sizeToInt(m_nativeRenderLineCache.size());
	if (renderLineCount <= 0)
		return -1;

	constexpr int kBoundedRenderLineLookupCount = 256;
	auto findInRange = [this, lineNumber](const int firstIndex, const int lastIndex, const bool reverse)
	{
		if (reverse)
		{
			for (int renderIndex = lastIndex; renderIndex >= firstIndex; --renderIndex)
			{
				if (nativeRuntimeLineRangeContains(m_nativeRenderLineCache.at(renderIndex), lineNumber))
					return renderIndex;
			}
			return -1;
		}

		for (int renderIndex = firstIndex; renderIndex <= lastIndex; ++renderIndex)
		{
			if (nativeRuntimeLineRangeContains(m_nativeRenderLineCache.at(renderIndex), lineNumber))
				return renderIndex;
		}
		return -1;
	};

	if (searchFromTail)
	{
		const int boundedFirstIndex = qMax(0, renderLineCount - kBoundedRenderLineLookupCount);
		if (const int renderIndex = findInRange(boundedFirstIndex, renderLineCount - 1, true);
		    renderIndex >= 0)
		{
			return renderIndex;
		}
		return boundedFirstIndex > 0 ? findInRange(0, boundedFirstIndex - 1, true) : -1;
	}

	const int boundedLastIndex = qMin(renderLineCount - 1, kBoundedRenderLineLookupCount - 1);
	if (const int renderIndex = findInRange(0, boundedLastIndex, false); renderIndex >= 0)
		return renderIndex;
	return boundedLastIndex + 1 < renderLineCount
	           ? findInRange(boundedLastIndex + 1, renderLineCount - 1, false)
	           : -1;
}

quint64 WorldView::nativeRuntimeLineKey(const QVector<qint64> &lineNumbers,
                                        const qint64           firstRuntimeLineNumber,
                                        const qint64           lastRuntimeLineNumber)
{
	quint64 seed = 1469598103934665603ULL;
	seed         = hashCombine(seed, static_cast<quint64>(lineNumbers.size()));
	if (lineNumbers.isEmpty())
	{
		seed = hashCombine(seed, static_cast<quint64>(firstRuntimeLineNumber));
		seed = hashCombine(seed, static_cast<quint64>(lastRuntimeLineNumber));
		return seed;
	}
	for (const qint64 lineNumber : lineNumbers)
		seed = hashCombine(seed, static_cast<quint64>(lineNumber));
	return seed;
}

quint64 WorldView::nativeRuntimeLineKey(const NativeOutputRenderLine &line)
{
	if (line.sourceRuntimeLineKey != 0)
		return line.sourceRuntimeLineKey;
	return nativeRuntimeLineKey(line.sourceRuntimeLineNumbers, line.firstRuntimeLineNumber,
	                            line.lastRuntimeLineNumber);
}

QVector<WorldRuntime::StyleSpan> WorldView::nativePartialOutputSpansForText() const
{
	QVector<WorldRuntime::StyleSpan> partialSpans;
	if (!m_nativeHasPartialOutput || m_nativePartialOutputText.isEmpty())
		return partialSpans;

	if (m_nativePartialOutputSpans.isEmpty())
	{
		WorldRuntime::StyleSpan span;
		span.length = sizeToInt(m_nativePartialOutputText.size());
		if (m_output)
		{
			span.fore = m_output->palette().color(QPalette::Text);
			span.back =
			    m_outputBackground.isValid() ? m_outputBackground : m_output->palette().color(QPalette::Base);
		}
		partialSpans.push_back(span);
		return partialSpans;
	}

	partialSpans.reserve(m_nativePartialOutputSpans.size());
	for (const WorldRuntime::StyleSpan &span : m_nativePartialOutputSpans)
	{
		if (span.length > 0)
			partialSpans.push_back(span);
	}
	return partialSpans;
}

bool WorldView::nativeRenderBaseCacheMatchesCurrentSource() const
{
	if (!m_nativeRenderLineCacheValid)
		return false;
	if (m_nativeRuntimeTailRestitchPending || m_nativeRuntimeLineRestitchIndex >= 0 ||
	    m_nativeRuntimeRangeRestitchStartIndex >= 0)
		return false;

	if (!m_runtime)
		return !m_nativeRenderLineCacheFromRuntime;

	const QVector<WorldRuntime::LineEntry> &runtimeLines = m_runtime->lines();
	if (!m_nativeRenderLineCacheFromRuntime)
		return false;
	if (runtimeLines.isEmpty())
		return m_nativeCachedRuntimeCount == 0 && m_nativeCachedRuntimeFirstLineNumber == 0 &&
		       m_nativeCachedRuntimeLastLineNumber == 0;
	if (m_nativeCachedRuntimeCount != runtimeLines.size())
		return false;
	if (m_nativeCachedRuntimeFirstLineNumber != runtimeLines.first().lineNumber ||
	    m_nativeCachedRuntimeLastLineNumber != runtimeLines.last().lineNumber)
	{
		return false;
	}
	if (m_nativeCachedRuntimeLastHardReturn != runtimeLines.last().hardReturn)
		return false;
	return lineEntriesEquivalentForCache(runtimeLines.first(), m_nativeCachedRuntimeFirstEntry) &&
	       lineEntriesEquivalentForCache(runtimeLines.last(), m_nativeCachedRuntimeLastEntry);
}

bool WorldView::nativePartialRenderLineOverlayCurrent(const QVector<WorldRuntime::StyleSpan> &partialSpans,
                                                      const bool appendToLastBaseLine) const
{
	return m_nativePartialRenderLineApplied &&
	       m_nativePartialRenderLineEffectiveRevision == m_nativeRenderLineCacheRevision &&
	       m_nativePartialRenderLineAppended == (!appendToLastBaseLine) &&
	       m_nativePartialRenderLineText == m_nativePartialOutputText &&
	       styleSpanVectorsEquivalent(m_nativePartialRenderLineSpans, partialSpans) &&
	       m_nativePartialRenderBaseLastHardReturn == m_nativeCachedRuntimeLastHardReturn;
}

void WorldView::removeNativePartialRenderLineOverlay(const bool bumpRevision) const
{
	if (!m_nativePartialRenderLineApplied)
		return;

	const int  oldLineCount = sizeToInt(m_nativeRenderLineCache.size());
	const bool tailMutated  = !m_nativePartialRenderLineAppended;
	if (m_nativePartialRenderLineAppended)
	{
		if (!m_nativeRenderLineCache.isEmpty())
			m_nativeRenderLineCache.removeLast();
	}
	else if (!m_nativeRenderLineCache.isEmpty())
	{
		m_nativeRenderLineCache.last() = std::move(m_nativePartialRenderLineBaseTail);
	}

	m_nativePartialRenderLineApplied           = false;
	m_nativePartialRenderLineAppended          = false;
	m_nativePartialRenderBaseLastHardReturn    = true;
	m_nativePartialRenderLineEffectiveRevision = 0;
	m_nativePartialRenderLineBaseTail          = {};
	m_nativePartialRenderLineText.clear();
	m_nativePartialRenderLineSpans.clear();

	if (bumpRevision)
		bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::TailRemove, oldLineCount, tailMutated);
}

void WorldView::applyNativePartialRenderLineOverlay() const
{
	const QVector<WorldRuntime::StyleSpan> partialSpans = nativePartialOutputSpansForText();
	if (partialSpans.isEmpty())
	{
		removeNativePartialRenderLineOverlay(true);
		return;
	}

	const bool appendToLastBaseLine =
	    !m_nativeCachedRuntimeLastHardReturn && !m_nativeRenderLineCache.isEmpty();
	if (nativePartialRenderLineOverlayCurrent(partialSpans, appendToLastBaseLine))
	{
		const double partialOpacity = qBound(0.0, lineOpacityForTimestamp(QDateTime::currentDateTime()), 1.0);
		if (!m_nativeRenderLineCache.isEmpty())
			m_nativeRenderLineCache.last().opacity = partialOpacity;
		return;
	}

	removeNativePartialRenderLineOverlay(false);

	const int    oldLineCount      = sizeToInt(m_nativeRenderLineCache.size());
	const double partialOpacity    = qBound(0.0, lineOpacityForTimestamp(QDateTime::currentDateTime()), 1.0);
	m_nativePartialRenderLineText  = m_nativePartialOutputText;
	m_nativePartialRenderLineSpans = partialSpans;
	m_nativePartialRenderLineAppended       = !appendToLastBaseLine;
	m_nativePartialRenderBaseLastHardReturn = m_nativeCachedRuntimeLastHardReturn;

	if (appendToLastBaseLine)
	{
		m_nativePartialRenderLineBaseTail = m_nativeRenderLineCache.last();
		NativeOutputRenderLine &line      = m_nativeRenderLineCache.last();
		line.text += m_nativePartialOutputText;
		line.flags |= WorldRuntime::LineOutput;
		line.opacity = partialOpacity;
		line.spans.reserve(line.spans.size() + partialSpans.size());
		for (const WorldRuntime::StyleSpan &span : partialSpans)
			line.spans.push_back(span);
		line.visualHash = nativeLineContentHash(line);
	}
	else
	{
		const qint64 baseLastLineNumber =
		    m_nativeRenderLineCache.isEmpty() ? 0 : m_nativeRenderLineCache.constLast().lastRuntimeLineNumber;
		const qint64           runtimeLineNumber = baseLastLineNumber + 1;
		NativeOutputRenderLine renderLine{
		    m_nativePartialOutputText,
		    partialSpans,
		    partialOpacity,
		    runtimeLineNumber,
		    runtimeLineNumber,
		    WorldRuntime::LineOutput,
		    0,
		    {},
		};
		renderLine.visualHash = nativeLineContentHash(renderLine);
		renderLine.sourceRuntimeLineKey =
		    nativeRuntimeLineKey(renderLine.sourceRuntimeLineNumbers, renderLine.firstRuntimeLineNumber,
		                         renderLine.lastRuntimeLineNumber);
		m_nativeRenderLineCache.push_back(std::move(renderLine));
	}

	m_nativePartialRenderLineApplied = true;
	bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::TailAppend, oldLineCount,
	                                  appendToLastBaseLine);
	m_nativePartialRenderLineEffectiveRevision = m_nativeRenderLineCacheRevision;
}

const QVector<WorldView::NativeOutputRenderLine> &WorldView::finalizeNativeOutputRenderLines() const
{
	if (!m_nativeHasPartialOutput || m_nativePartialOutputText.isEmpty())
	{
		removeNativePartialRenderLineOverlay(true);
		return m_nativeRenderLineCache;
	}

	applyNativePartialRenderLineOverlay();
	return m_nativeRenderLineCache;
}

quint64 WorldView::nativeLayoutStyleKey() const
{
	quint64 seed = 1099511628211ULL;
	seed         = hashCombine(seed, static_cast<quint64>(m_hyperlinkColour.rgba()));
	seed         = hashCombine(seed, static_cast<quint64>(m_showBold ? 1 : 0));
	seed         = hashCombine(seed, static_cast<quint64>(m_showItalic ? 1 : 0));
	seed         = hashCombine(seed, static_cast<quint64>(m_showUnderline ? 1 : 0));
	seed         = hashCombine(seed, static_cast<quint64>(m_alternativeInverse ? 1 : 0));
	seed         = hashCombine(seed, static_cast<quint64>(m_useCustomLinkColour ? 1 : 0));
	seed         = hashCombine(seed, static_cast<quint64>(m_underlineHyperlinks ? 1 : 0));
	seed         = hashCombine(seed, static_cast<quint64>(nativeServerSideWrapActive() ? 1 : 0));
	return seed;
}

QVector<QTextLayout::FormatRange> WorldView::buildNativeFormatRanges(const NativeOutputRenderLine &line,
                                                                     const QFont &layoutFont) const
{
	Q_UNUSED(layoutFont);
	QVector<QTextLayout::FormatRange> ranges;
	if (line.spans.isEmpty() || line.text.isEmpty())
		return ranges;

	ranges.reserve(line.spans.size());
	QColor defaultBackground;
	if (m_output)
	{
		defaultBackground =
		    m_outputBackground.isValid() ? m_outputBackground : m_output->palette().color(QPalette::Base);
	}

	const int textSize = sizeToInt(line.text.size());
	int       offset   = 0;
	for (const WorldRuntime::StyleSpan &span : line.spans)
	{
		const int rawLength = qMax(0, span.length);
		if (rawLength <= 0)
			continue;
		if (offset >= textSize)
			break;
		const int remaining  = qMax(0, textSize - offset);
		const int spanLength = qMin(rawLength, remaining);
		if (spanLength <= 0)
			break;

		QColor foreground           = span.fore;
		QColor background           = span.back;
		bool   useDefaultForeground = false;
		if (span.inverse)
		{
			qSwap(foreground, background);
			if (m_alternativeInverse && span.bold)
				qSwap(foreground, background);
		}

		const bool hasLinkAction = isActionLinkType(span.actionType) && !span.action.isEmpty();
		if (!foreground.isValid())
		{
			if (hasLinkAction && m_useCustomLinkColour && m_hyperlinkColour.isValid())
				foreground = m_hyperlinkColour;
			else
				useDefaultForeground = true;
		}

		const bool boldActive            = span.bold && m_showBold;
		const bool italicActive          = span.italic && m_showItalic;
		const bool spanUnderlineActive   = span.underline && m_showUnderline;
		const bool linkUnderlineActive   = hasLinkAction && m_underlineHyperlinks;
		const bool underlineActive       = spanUnderlineActive || linkUnderlineActive;
		const bool strikeActive          = span.strike;
		const bool hasForegroundOverride = !useDefaultForeground && foreground.isValid();
		const bool hasBackgroundOverride =
		    background.isValid() && (!defaultBackground.isValid() || background != defaultBackground);
		const bool hasFontOverride = boldActive || italicActive || underlineActive || strikeActive;
		if (!hasFontOverride && !hasForegroundOverride && !hasBackgroundOverride)
		{
			offset += rawLength;
			continue;
		}

		QTextCharFormat format;
		if (boldActive)
			format.setFontWeight(static_cast<int>(QFont::Bold));
		if (italicActive)
			format.setFontItalic(true);
		if (underlineActive)
			format.setFontUnderline(true);
		if (strikeActive)
			format.setFontStrikeOut(true);
		if (hasForegroundOverride)
			format.setForeground(foreground);
		if (hasBackgroundOverride)
			format.setBackground(background);

		if (!ranges.isEmpty())
		{
			QTextLayout::FormatRange &last = ranges.last();
			if (last.start + last.length == offset && last.format == format)
			{
				last.length += spanLength;
				offset += rawLength;
				continue;
			}
		}

		QTextLayout::FormatRange range;
		range.start  = offset;
		range.length = spanLength;
		range.format = format;
		ranges.push_back(range);

		offset += rawLength;
	}
	return ranges;
}

bool WorldView::nativeLayoutCacheReadyFor(const QVector<NativeOutputRenderLine> &lines,
                                          const int wrapWidthPixels, const int localWrapWidthPixels,
                                          const int lineSpacingSetting, const QFont &layoutFont) const
{
	const qreal lineSpacingFactor = (100.0 + static_cast<qreal>(lineSpacingSetting)) / 100.0;
	const qreal lineAdvance =
	    static_cast<qreal>(qMax(1, QFontMetrics(layoutFont).lineSpacing())) * lineSpacingFactor;
	const quint64 styleKey             = nativeLayoutStyleKey();
	const bool    cacheDimensionsMatch = m_nativeLayoutCumulativeHeights.size() == lines.size() + 1 &&
	                                     m_nativeLayoutVisualRows.size() == lines.size() &&
	                                     m_nativeLayoutLineLayouts.size() == lines.size() &&
	                                     m_nativeLayoutLineContentHashes.size() == lines.size() &&
	                                     m_nativeLayoutRowsExact.size() == lines.size();
	return m_nativeLayoutCacheValid && cacheDimensionsMatch &&
	       m_nativeLayoutCachedRenderRevision == m_nativeRenderLineCacheRevision &&
	       m_nativeLayoutCachedWrapWidth == wrapWidthPixels &&
	       m_nativeLayoutCachedLocalWrapWidth == localWrapWidthPixels &&
	       m_nativeLayoutCachedLineSpacing == lineSpacingSetting &&
	       m_nativeLayoutCachedStyleKey == styleKey && m_nativeLayoutCachedFont == layoutFont &&
	       qFuzzyCompare(m_nativeLayoutCachedLineAdvance + 1.0, lineAdvance + 1.0) &&
	       m_nativeLayoutCumulativeDirtyFrom >= sizeToInt(lines.size());
}

int WorldView::estimateNativeLineRows(const NativeOutputRenderLine &line, const int effectiveWrapWidth,
                                      const QFontMetrics &fontMetrics)
{
	if (line.text.isEmpty())
		return 1;

	if (effectiveWrapWidth >= 1000000)
	{
		return nativeEstimatedHardBreakRows(line.text);
	}

	const int averageCharWidth = qMax(1, fontMetrics.averageCharWidth());
	const int columnsPerRow    = qMax(1, effectiveWrapWidth / averageCharWidth);
	if (nativeTextFitsEstimatedSingleRow(line.text, columnsPerRow))
		return 1;

	int  rows           = 1;
	int  currentColumns = 0;
	int  pendingSpaces  = 0;
	int  wordColumns    = 0;
	auto commitWord     = [&]
	{
		if (wordColumns <= 0)
			return;
		if (currentColumns <= 0)
		{
			currentColumns = wordColumns;
		}
		else if (currentColumns + pendingSpaces + wordColumns <= columnsPerRow)
		{
			currentColumns += pendingSpaces + wordColumns;
		}
		else
		{
			++rows;
			currentColumns = wordColumns;
		}
		pendingSpaces = 0;
		wordColumns   = 0;
	};

	const QChar *data = line.text.constData();
	for (qsizetype index = 0; index < line.text.size(); ++index)
	{
		const ushort code = data[index].unicode();
		if (code == '\n' || code == kNativeLineSeparatorCode)
		{
			commitWord();
			++rows;
			currentColumns = 0;
			pendingSpaces  = 0;
			continue;
		}

		if (nativeEstimatedWhitespace(code))
		{
			commitWord();
			if (currentColumns <= 0)
			{
				++currentColumns;
				if (currentColumns >= columnsPerRow)
				{
					++rows;
					currentColumns = 0;
				}
			}
			else
			{
				++pendingSpaces;
			}
			continue;
		}

		++wordColumns;
	}
	commitWord();
	return qMax(1, rows);
}

bool WorldView::ensureNativeLayoutRange(const QVector<NativeOutputRenderLine> &lines, int firstLine,
                                        int lastLine, const int wrapWidthPixels,
                                        const int localWrapWidthPixels, const int lineSpacingSetting,
                                        const QFont &layoutFont) const
{
	if (lines.isEmpty())
		return false;

	if (!nativeLayoutCacheReadyFor(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacingSetting,
	                               layoutFont))
	{
		ensureNativeLayoutCaches(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacingSetting,
		                         layoutFont);
	}

	firstLine = qBound(0, firstLine, sizeToInt(lines.size()) - 1);
	lastLine  = qBound(firstLine, lastLine, sizeToInt(lines.size()) - 1);

	const qreal lineSpacingFactor = (100.0 + static_cast<qreal>(lineSpacingSetting)) / 100.0;
	const qreal defaultLineAdvance =
	    static_cast<qreal>(qMax(1, QFontMetrics(layoutFont).lineSpacing())) * lineSpacingFactor;

	int firstHeightChangedLine = -1;
	for (int i = firstLine; i <= lastLine; ++i)
	{
		const int   previousRows = (i < m_nativeLayoutVisualRows.size()) ? m_nativeLayoutVisualRows.at(i) : 0;
		const uchar previousExact = (i < m_nativeLayoutRowsExact.size()) ? m_nativeLayoutRowsExact.at(i) : 0;
		const int   exactRows     = ensureNativeLineLayout(lines, i, wrapWidthPixels, localWrapWidthPixels,
		                                                   defaultLineAdvance, layoutFont);
		if (firstHeightChangedLine < 0 &&
		    (previousRows != exactRows || (previousExact == 0 && previousRows <= 0)))
		{
			firstHeightChangedLine = i;
		}
	}

	if (firstHeightChangedLine < 0)
		return false;

	qreal docY =
	    firstHeightChangedLine > 0 ? m_nativeLayoutCumulativeHeights.at(firstHeightChangedLine) : 0.0;
	if (firstHeightChangedLine == 0 && !m_nativeLayoutCumulativeHeights.isEmpty())
		m_nativeLayoutCumulativeHeights[0] = 0.0;
	for (int i = firstHeightChangedLine; i < lines.size(); ++i)
	{
		const int rows = (i < m_nativeLayoutVisualRows.size() && m_nativeLayoutVisualRows.at(i) > 0)
		                     ? m_nativeLayoutVisualRows.at(i)
		                     : 1;
		docY += static_cast<qreal>(rows) * defaultLineAdvance;
		if (i + 1 < m_nativeLayoutCumulativeHeights.size())
			m_nativeLayoutCumulativeHeights[i + 1] = docY;
	}
	m_nativeLayoutCumulativeDirtyFrom = sizeToInt(lines.size());
	return true;
}

int WorldView::ensureNativeLineLayout(const QVector<NativeOutputRenderLine> &lines, const int index,
                                      const int wrapWidthPixels, const int localWrapWidthPixels,
                                      const qreal defaultLineAdvance, const QFont &layoutFont) const
{
	if (index < 0 || index >= lines.size())
		return 1;
	if (index >= m_nativeLayoutLineLayouts.size() || index >= m_nativeLayoutLineContentHashes.size() ||
	    index >= m_nativeLayoutVisualRows.size() || index >= m_nativeLayoutRowsExact.size())
	{
		return 1;
	}

	const NativeOutputRenderLine &line = lines.at(index);
	const bool    hasLocalContent    = (line.flags & (WorldRuntime::LineNote | WorldRuntime::LineInput)) != 0;
	const int     effectiveWrapWidth = hasLocalContent ? localWrapWidthPixels : wrapWidthPixels;
	const quint64 visualHash         = line.visualHash != 0 ? line.visualHash : nativeLineContentHash(line);
	const quint64 contentHash        = hashCombine(visualHash, static_cast<quint64>(effectiveWrapWidth));
	if (const auto &cachedLayout = m_nativeLayoutLineLayouts.at(index);
	    cachedLayout && m_nativeLayoutLineContentHashes.at(index) == contentHash)
	{
		if (m_nativeLayoutRowsExact.at(index) != 0 && m_nativeLayoutVisualRows.at(index) > 0)
			return m_nativeLayoutVisualRows.at(index);

		if (!cachedLayout->cacheEnabled())
			cachedLayout->setCacheEnabled(true);

		QTextOption option = cachedLayout->textOption();
		option.setWrapMode(effectiveWrapWidth >= 1000000 ? QTextOption::NoWrap : QTextOption::WordWrap);
		cachedLayout->setTextOption(option);
		cachedLayout->clearLayout();
		cachedLayout->beginLayout();
		int rowCount = 0;
		while (true)
		{
			QTextLine textLine = cachedLayout->createLine();
			if (!textLine.isValid())
				break;
			textLine.setLineWidth(static_cast<qreal>(effectiveWrapWidth));
			textLine.setPosition({0.0, static_cast<qreal>(rowCount) * defaultLineAdvance});
			++rowCount;
		}
		cachedLayout->endLayout();
		rowCount                        = qMax(1, rowCount);
		m_nativeLayoutVisualRows[index] = rowCount;
		m_nativeLayoutRowsExact[index]  = 1;
		++m_nativeLayoutRowMeasurements;
		return rowCount;
	}

	if (line.text.isEmpty())
	{
		m_nativeLayoutLineLayouts[index].clear();
		m_nativeLayoutLineContentHashes[index] = contentHash;
		m_nativeLayoutVisualRows[index]        = 1;
		m_nativeLayoutRowsExact[index]         = 1;
		return 1;
	}

	QString layoutText = line.text;
	// QTextLayout does not reliably treat '\n' in raw line text as explicit
	// hard line breaks in all native-render paths. Convert to Unicode line
	// separators so pre-wrapped local output (echo/notes) always paints as
	// wrapped rows instead of clipped single rows.
	layoutText.replace(QLatin1Char('\n'), QChar::LineSeparator);
	auto layout = QSharedPointer<QTextLayout>::create(layoutText, layoutFont);
	layout->setCacheEnabled(true);
	QTextOption option;
	option.setWrapMode(effectiveWrapWidth >= 1000000 ? QTextOption::NoWrap : QTextOption::WordWrap);
	layout->setTextOption(option);
	layout->setFormats(buildNativeFormatRanges(line, layoutFont));
	layout->beginLayout();
	int rowCount = 0;
	while (true)
	{
		QTextLine textLine = layout->createLine();
		if (!textLine.isValid())
			break;
		textLine.setLineWidth(static_cast<qreal>(effectiveWrapWidth));
		textLine.setPosition({0.0, static_cast<qreal>(rowCount) * defaultLineAdvance});
		++rowCount;
	}
	layout->endLayout();
	rowCount = qMax(1, rowCount);

	m_nativeLayoutLineLayouts[index]       = layout;
	m_nativeLayoutLineContentHashes[index] = contentHash;
	m_nativeLayoutVisualRows[index]        = rowCount;
	m_nativeLayoutRowsExact[index]         = 1;
	++m_nativeLayoutRowMeasurements;
	return rowCount;
}

void WorldView::ensureNativeLayoutCaches(const QVector<NativeOutputRenderLine> &lines,
                                         const int wrapWidthPixels, const int localWrapWidthPixels,
                                         const int lineSpacingSetting, const QFont &layoutFont) const
{
	const qreal lineSpacingFactor = (100.0 + static_cast<qreal>(lineSpacingSetting)) / 100.0;
	const qreal defaultLineAdvance =
	    static_cast<qreal>(qMax(1, QFontMetrics(layoutFont).lineSpacing())) * lineSpacingFactor;
	const quint64 styleKey      = nativeLayoutStyleKey();
	const bool    cacheWasValid = m_nativeLayoutCacheValid;
	const bool    renderRevisionChanged =
	    cacheWasValid && m_nativeLayoutCachedRenderRevision != m_nativeRenderLineCacheRevision;
	const bool wrapChanged      = cacheWasValid && m_nativeLayoutCachedWrapWidth != wrapWidthPixels;
	const bool localWrapChanged = cacheWasValid && m_nativeLayoutCachedLocalWrapWidth != localWrapWidthPixels;
	const bool lineSpacingChanged = cacheWasValid && m_nativeLayoutCachedLineSpacing != lineSpacingSetting;
	const bool fontChanged        = cacheWasValid && m_nativeLayoutCachedFont != layoutFont;
	const bool styleChanged       = cacheWasValid && m_nativeLayoutCachedStyleKey != styleKey;
	const bool lineAdvanceChanged =
	    cacheWasValid && !qFuzzyCompare(m_nativeLayoutCachedLineAdvance + 1.0, defaultLineAdvance + 1.0);

	const bool layoutCacheKeyChanged = !cacheWasValid || wrapChanged || lineSpacingChanged || fontChanged ||
	                                   styleChanged || lineAdvanceChanged;

	const bool cacheDimensionsMatch = m_nativeLayoutCumulativeHeights.size() == lines.size() + 1 &&
	                                  m_nativeLayoutVisualRows.size() == lines.size() &&
	                                  m_nativeLayoutLineLayouts.size() == lines.size() &&
	                                  m_nativeLayoutLineContentHashes.size() == lines.size() &&
	                                  m_nativeLayoutRowsExact.size() == lines.size();
	if (cacheWasValid && !layoutCacheKeyChanged && !localWrapChanged && !renderRevisionChanged &&
	    cacheDimensionsMatch && m_nativeLayoutCumulativeDirtyFrom >= sizeToInt(lines.size()))
	{
		return;
	}

	if (cacheWasValid && renderRevisionChanged &&
	    m_nativeRenderCacheDelta.revision == m_nativeRenderLineCacheRevision &&
	    m_nativeRenderCacheDelta.headTrimCount > 0)
	{
		const int oldSize       = qMax(0, m_nativeRenderCacheDelta.oldLineCount);
		const int headTrimCount = qBound(0, m_nativeRenderCacheDelta.headTrimCount, oldSize);
		if (headTrimCount > 0 && m_nativeLayoutVisualRows.size() == oldSize)
		{
			qreal trimmedHeadDocY = -1.0;
			if (m_nativeLayoutCumulativeHeights.size() == oldSize + 1 &&
			    headTrimCount < m_nativeLayoutCumulativeHeights.size() &&
			    m_nativeLayoutCumulativeDirtyFrom >= headTrimCount)
			{
				trimmedHeadDocY = qMax<qreal>(0.0, m_nativeLayoutCumulativeHeights.at(headTrimCount));
			}
			else
			{
				const qreal oldLineAdvance = m_nativeLayoutCachedLineAdvance > 0.0
				                                 ? m_nativeLayoutCachedLineAdvance
				                                 : defaultLineAdvance;
				qreal       sumRows        = 0.0;
				bool        haveExactRows  = true;
				for (int i = 0; i < headTrimCount; ++i)
				{
					const int visualRows = m_nativeLayoutVisualRows.at(i);
					if (visualRows <= 0)
					{
						haveExactRows = false;
						break;
					}
					sumRows += static_cast<qreal>(visualRows) * oldLineAdvance;
				}
				if (haveExactRows)
					trimmedHeadDocY = sumRows;
			}
			if (trimmedHeadDocY >= 0.0)
			{
				m_nativeSplitTopHeadTrimPixelsRevision = m_nativeRenderLineCacheRevision;
				m_nativeSplitTopHeadTrimPixels = qMax(0, static_cast<int>(std::round(trimmedHeadDocY)));
			}
		}
	}

	auto runtimeLineKeyForLine = [](const NativeOutputRenderLine &line)
	{ return nativeRuntimeLineKey(line); };

	auto lineContentHashForWidth = [&](const NativeOutputRenderLine &line)
	{
		const bool hasLocalContent = (line.flags & (WorldRuntime::LineNote | WorldRuntime::LineInput)) != 0;
		const int  effectiveWrapWidth = hasLocalContent ? localWrapWidthPixels : wrapWidthPixels;
		const quint64 visualHash      = line.visualHash != 0 ? line.visualHash : nativeLineContentHash(line);
		return hashCombine(visualHash, static_cast<quint64>(effectiveWrapWidth));
	};

	auto refreshLayoutRuntimeLineKeys = [this, &lines, &runtimeLineKeyForLine]
	{
		m_nativeLayoutRuntimeLineKeys.resize(lines.size());
		for (int i = 0; i < lines.size(); ++i)
		{
			m_nativeLayoutRuntimeLineKeys[i] = runtimeLineKeyForLine(lines.at(i));
		}
	};

	auto dropLayoutHead = [this](const int requestedHeadTrimCount)
	{
		const int headTrimCount =
		    qBound(0, requestedHeadTrimCount, sizeToInt(m_nativeLayoutVisualRows.size()));
		if (headTrimCount <= 0)
			return;

		qreal trimmedDocY = 0.0;
		if (m_nativeLayoutCumulativeHeights.size() > headTrimCount)
			trimmedDocY = qMax<qreal>(0.0, m_nativeLayoutCumulativeHeights.at(headTrimCount));

		m_nativeLayoutVisualRows.remove(0, headTrimCount);
		m_nativeLayoutLineLayouts.remove(0, headTrimCount);
		m_nativeLayoutLineContentHashes.remove(0, headTrimCount);
		m_nativeLayoutRuntimeLineKeys.remove(0, headTrimCount);
		m_nativeLayoutRowsExact.remove(0, headTrimCount);

		if (m_nativeLayoutCumulativeHeights.size() > headTrimCount)
		{
			m_nativeLayoutCumulativeHeights.remove(0, headTrimCount);
			for (qreal &height : m_nativeLayoutCumulativeHeights)
				height = qMax<qreal>(0.0, height - trimmedDocY);
			if (!m_nativeLayoutCumulativeHeights.isEmpty())
				m_nativeLayoutCumulativeHeights[0] = 0.0;
		}
		else
		{
			m_nativeLayoutCumulativeHeights = QVector<qreal>(1, 0.0);
		}

		m_nativeLayoutCumulativeDirtyFrom = qMax(0, m_nativeLayoutCumulativeDirtyFrom - headTrimCount);
	};

	auto preserveVerifiedLayoutPrefix =
	    [this, &lines, &runtimeLineKeyForLine, &lineContentHashForWidth, &dropLayoutHead,
	     defaultLineAdvance](const int newSize, const int headTrimCount, const int requestedStablePrefix)
	{
		if (newSize < 0 || headTrimCount < 0 || requestedStablePrefix < 0)
			return false;

		const int       sourceOffset = qMax(0, headTrimCount);
		const qsizetype availableSourceSize =
		    std::min({m_nativeLayoutVisualRows.size(), m_nativeLayoutLineLayouts.size(),
		              m_nativeLayoutLineContentHashes.size(), m_nativeLayoutRuntimeLineKeys.size(),
		              m_nativeLayoutRowsExact.size()});
		const int availableSourceCount =
		    sizeToInt(qMax<qsizetype>(0, availableSourceSize - static_cast<qsizetype>(sourceOffset)));
		const int prefixLimit = qBound(0, requestedStablePrefix, qMin(newSize, availableSourceCount));
		if (prefixLimit <= 0 && newSize > 0)
			return false;

		int reusablePrefix = 0;
		for (; reusablePrefix < prefixLimit; ++reusablePrefix)
		{
			const int sourceIndex = sourceOffset + reusablePrefix;
			if (m_nativeLayoutRuntimeLineKeys.at(sourceIndex) !=
			    runtimeLineKeyForLine(lines.at(reusablePrefix)))
			{
				break;
			}

			const NativeOutputRenderLine &line = lines.at(reusablePrefix);
			const bool exactLineHasLayout      = m_nativeLayoutRowsExact.at(sourceIndex) == 0 ||
			                                     line.text.isEmpty() ||
			                                     static_cast<bool>(m_nativeLayoutLineLayouts.at(sourceIndex));
			if (m_nativeLayoutVisualRows.at(sourceIndex) <= 0 || !exactLineHasLayout ||
			    m_nativeLayoutLineContentHashes.at(sourceIndex) != lineContentHashForWidth(line))
			{
				break;
			}
		}
		if (reusablePrefix <= 0 && newSize > 0)
			return false;

		bool cumulativePrefixPreserved = false;
		if (sourceOffset > 0)
		{
			if (reusablePrefix == availableSourceCount)
			{
				dropLayoutHead(sourceOffset);
				cumulativePrefixPreserved = true;
			}
			else
			{
				for (int i = 0; i < reusablePrefix; ++i)
				{
					const int sourceIndex              = sourceOffset + i;
					m_nativeLayoutVisualRows[i]        = m_nativeLayoutVisualRows.at(sourceIndex);
					m_nativeLayoutLineLayouts[i]       = std::move(m_nativeLayoutLineLayouts[sourceIndex]);
					m_nativeLayoutLineContentHashes[i] = m_nativeLayoutLineContentHashes.at(sourceIndex);
					m_nativeLayoutRuntimeLineKeys[i]   = m_nativeLayoutRuntimeLineKeys.at(sourceIndex);
					m_nativeLayoutRowsExact[i]         = m_nativeLayoutRowsExact.at(sourceIndex);
				}
			}
		}

		m_nativeLayoutVisualRows.resize(newSize);
		m_nativeLayoutLineLayouts.resize(newSize);
		m_nativeLayoutLineContentHashes.resize(newSize);
		m_nativeLayoutRuntimeLineKeys.resize(newSize);
		m_nativeLayoutRowsExact.resize(newSize);
		for (int i = reusablePrefix; i < newSize; ++i)
		{
			m_nativeLayoutVisualRows[i] = -1;
			m_nativeLayoutLineLayouts[i].clear();
			m_nativeLayoutLineContentHashes[i] = 0;
			m_nativeLayoutRuntimeLineKeys[i]   = runtimeLineKeyForLine(lines.at(i));
			m_nativeLayoutRowsExact[i]         = 0;
		}

		m_nativeLayoutCumulativeHeights.resize(newSize + 1);
		if (!m_nativeLayoutCumulativeHeights.isEmpty())
			m_nativeLayoutCumulativeHeights[0] = 0.0;
		if (cumulativePrefixPreserved)
		{
			m_nativeLayoutCumulativeDirtyFrom =
			    qBound(0, qMin(m_nativeLayoutCumulativeDirtyFrom, reusablePrefix), newSize);
		}
		else
		{
			qreal prefixDocY = 0.0;
			for (int i = 0; i < reusablePrefix; ++i)
			{
				prefixDocY += static_cast<qreal>(m_nativeLayoutVisualRows.at(i)) * defaultLineAdvance;
				m_nativeLayoutCumulativeHeights[i + 1] = prefixDocY;
			}
			m_nativeLayoutCumulativeDirtyFrom = qBound(0, reusablePrefix, newSize);
		}
		return true;
	};

	bool renderDeltaFastPathApplied = false;
	if (cacheWasValid && !layoutCacheKeyChanged && !localWrapChanged && renderRevisionChanged &&
	    m_nativeRenderCacheDelta.revision == m_nativeRenderLineCacheRevision &&
	    m_nativeRenderCacheDelta.kind == NativeRenderCacheDeltaKind::TailAppend &&
	    m_nativeRenderCacheDelta.newLineCount == sizeToInt(lines.size()))
	{
		const int  oldSize = qBound(0, m_nativeRenderCacheDelta.oldLineCount, sizeToInt(lines.size()));
		const bool oldDimensionsMatch =
		    m_nativeLayoutVisualRows.size() == oldSize && m_nativeLayoutLineLayouts.size() == oldSize &&
		    m_nativeLayoutLineContentHashes.size() == oldSize &&
		    m_nativeLayoutRuntimeLineKeys.size() == oldSize && m_nativeLayoutRowsExact.size() == oldSize &&
		    m_nativeLayoutCumulativeHeights.size() == oldSize + 1;
		if (oldDimensionsMatch)
		{
			const int  newSize         = sizeToInt(lines.size());
			const bool tailLineMutated = m_nativeRenderCacheDelta.tailLineMutated && oldSize > 0;
			const int  stablePrefix    = qBound(0, oldSize - (tailLineMutated ? 1 : 0), newSize);

			m_nativeLayoutVisualRows.resize(lines.size());
			m_nativeLayoutLineLayouts.resize(lines.size());
			m_nativeLayoutLineContentHashes.resize(lines.size());
			m_nativeLayoutRuntimeLineKeys.resize(lines.size());
			m_nativeLayoutRowsExact.resize(lines.size());
			for (int i = stablePrefix; i < newSize; ++i)
			{
				m_nativeLayoutVisualRows[i] = -1;
				m_nativeLayoutLineLayouts[i].clear();
				m_nativeLayoutLineContentHashes[i] = 0;
				m_nativeLayoutRuntimeLineKeys[i]   = runtimeLineKeyForLine(lines.at(i));
				m_nativeLayoutRowsExact[i]         = 0;
			}

			m_nativeLayoutCumulativeHeights.resize(lines.size() + 1);
			if (stablePrefix == 0 && !m_nativeLayoutCumulativeHeights.isEmpty())
				m_nativeLayoutCumulativeHeights[0] = 0.0;
			m_nativeLayoutCumulativeDirtyFrom =
			    qBound(0, qMin(m_nativeLayoutCumulativeDirtyFrom, stablePrefix), newSize);
			renderDeltaFastPathApplied = true;
		}
		else
		{
			const int newSize = sizeToInt(lines.size());
			const int stablePrefix =
			    qBound(0, oldSize - (m_nativeRenderCacheDelta.tailLineMutated ? 1 : 0), newSize);
			renderDeltaFastPathApplied = preserveVerifiedLayoutPrefix(newSize, 0, stablePrefix);
		}
	}
	else if (cacheWasValid && !layoutCacheKeyChanged && !localWrapChanged && renderRevisionChanged &&
	         m_nativeRenderCacheDelta.revision == m_nativeRenderLineCacheRevision &&
	         m_nativeRenderCacheDelta.kind == NativeRenderCacheDeltaKind::TailRemove &&
	         m_nativeRenderCacheDelta.newLineCount == sizeToInt(lines.size()))
	{
		const int  oldSize = qMax(0, m_nativeRenderCacheDelta.oldLineCount);
		const int  newSize = sizeToInt(lines.size());
		const bool oldDimensionsMatch =
		    m_nativeLayoutVisualRows.size() == oldSize && m_nativeLayoutLineLayouts.size() == oldSize &&
		    m_nativeLayoutLineContentHashes.size() == oldSize &&
		    m_nativeLayoutRuntimeLineKeys.size() == oldSize && m_nativeLayoutRowsExact.size() == oldSize &&
		    m_nativeLayoutCumulativeHeights.size() == oldSize + 1;
		const bool tailLineMutated = m_nativeRenderCacheDelta.tailLineMutated && newSize > 0;
		const int  stablePrefix    = qBound(0, newSize - (tailLineMutated ? 1 : 0), newSize);
		if (oldDimensionsMatch)
		{
			m_nativeLayoutVisualRows.resize(newSize);
			m_nativeLayoutLineLayouts.resize(newSize);
			m_nativeLayoutLineContentHashes.resize(newSize);
			m_nativeLayoutRuntimeLineKeys.resize(newSize);
			m_nativeLayoutRowsExact.resize(newSize);
			for (int i = stablePrefix; i < newSize; ++i)
			{
				m_nativeLayoutVisualRows[i] = -1;
				m_nativeLayoutLineLayouts[i].clear();
				m_nativeLayoutLineContentHashes[i] = 0;
				m_nativeLayoutRuntimeLineKeys[i]   = runtimeLineKeyForLine(lines.at(i));
				m_nativeLayoutRowsExact[i]         = 0;
			}

			m_nativeLayoutCumulativeHeights.resize(newSize + 1);
			if (stablePrefix == 0 && !m_nativeLayoutCumulativeHeights.isEmpty())
				m_nativeLayoutCumulativeHeights[0] = 0.0;
			m_nativeLayoutCumulativeDirtyFrom =
			    qBound(0, qMin(m_nativeLayoutCumulativeDirtyFrom, stablePrefix), newSize);
			renderDeltaFastPathApplied = true;
		}
		else
		{
			renderDeltaFastPathApplied = preserveVerifiedLayoutPrefix(newSize, 0, stablePrefix);
		}
	}
	else if (cacheWasValid && !layoutCacheKeyChanged && !localWrapChanged && renderRevisionChanged &&
	         m_nativeRenderCacheDelta.revision == m_nativeRenderLineCacheRevision &&
	         m_nativeRenderCacheDelta.kind == NativeRenderCacheDeltaKind::HeadTrimTailAppend &&
	         m_nativeRenderCacheDelta.newLineCount == sizeToInt(lines.size()))
	{
		const int  oldSize       = qMax(0, m_nativeRenderCacheDelta.oldLineCount);
		const int  newSize       = sizeToInt(lines.size());
		const int  headTrimCount = qBound(0, m_nativeRenderCacheDelta.headTrimCount, oldSize);
		const bool oldDimensionsMatch =
		    m_nativeLayoutVisualRows.size() == oldSize && m_nativeLayoutLineLayouts.size() == oldSize &&
		    m_nativeLayoutLineContentHashes.size() == oldSize &&
		    m_nativeLayoutRuntimeLineKeys.size() == oldSize && m_nativeLayoutRowsExact.size() == oldSize &&
		    m_nativeLayoutCumulativeHeights.size() == oldSize + 1;
		if (oldDimensionsMatch && headTrimCount > 0 && oldSize >= headTrimCount)
		{
			const int preservedCount = qBound(0, oldSize - headTrimCount, newSize);
			dropLayoutHead(headTrimCount);

			m_nativeLayoutVisualRows.resize(newSize);
			m_nativeLayoutLineLayouts.resize(newSize);
			m_nativeLayoutLineContentHashes.resize(newSize);
			m_nativeLayoutRuntimeLineKeys.resize(newSize);
			m_nativeLayoutRowsExact.resize(newSize);
			for (int i = preservedCount; i < newSize; ++i)
			{
				m_nativeLayoutVisualRows[i] = -1;
				m_nativeLayoutLineLayouts[i].clear();
				m_nativeLayoutLineContentHashes[i] = 0;
				m_nativeLayoutRuntimeLineKeys[i]   = runtimeLineKeyForLine(lines.at(i));
				m_nativeLayoutRowsExact[i]         = 0;
			}

			m_nativeLayoutCumulativeHeights.resize(newSize + 1);
			if (!m_nativeLayoutCumulativeHeights.isEmpty())
				m_nativeLayoutCumulativeHeights[0] = 0.0;
			m_nativeLayoutCumulativeDirtyFrom =
			    qBound(0, qMin(m_nativeLayoutCumulativeDirtyFrom, preservedCount), newSize);
			renderDeltaFastPathApplied = true;
		}
		else
		{
			const int preservedCount   = qBound(0, oldSize - headTrimCount, newSize);
			renderDeltaFastPathApplied = preserveVerifiedLayoutPrefix(newSize, headTrimCount, preservedCount);
		}
	}
	else if (cacheWasValid && !layoutCacheKeyChanged && !localWrapChanged && renderRevisionChanged &&
	         m_nativeRenderCacheDelta.revision == m_nativeRenderLineCacheRevision &&
	         m_nativeRenderCacheDelta.kind == NativeRenderCacheDeltaKind::RuntimeLineRestitch &&
	         m_nativeRenderCacheDelta.newLineCount == sizeToInt(lines.size()))
	{
		const int  oldSize            = qMax(0, m_nativeRenderCacheDelta.oldLineCount);
		const int  newSize            = sizeToInt(lines.size());
		const bool oldDimensionsMatch = oldSize == newSize && m_nativeLayoutVisualRows.size() == oldSize &&
		                                m_nativeLayoutLineLayouts.size() == oldSize &&
		                                m_nativeLayoutLineContentHashes.size() == oldSize &&
		                                m_nativeLayoutRuntimeLineKeys.size() == oldSize &&
		                                m_nativeLayoutRowsExact.size() == oldSize &&
		                                m_nativeLayoutCumulativeHeights.size() == oldSize + 1;
		if (oldDimensionsMatch && newSize > 0)
		{
			const int changedIndex = qBound(0, m_nativeRenderCacheDelta.stablePrefixCount, newSize - 1);
			m_nativeLayoutVisualRows[changedIndex] = -1;
			m_nativeLayoutLineLayouts[changedIndex].clear();
			m_nativeLayoutLineContentHashes[changedIndex] = 0;
			m_nativeLayoutRuntimeLineKeys[changedIndex]   = runtimeLineKeyForLine(lines.at(changedIndex));
			m_nativeLayoutRowsExact[changedIndex]         = 0;
			m_nativeLayoutCumulativeDirtyFrom =
			    qBound(0, qMin(m_nativeLayoutCumulativeDirtyFrom, changedIndex), newSize);
			renderDeltaFastPathApplied = true;
		}
		else
		{
			const int stablePrefix =
			    qBound(0, m_nativeRenderCacheDelta.stablePrefixCount, qMin(oldSize, newSize));
			renderDeltaFastPathApplied = preserveVerifiedLayoutPrefix(newSize, 0, stablePrefix);
		}
	}
	else if (cacheWasValid && !layoutCacheKeyChanged && !localWrapChanged && renderRevisionChanged &&
	         m_nativeRenderCacheDelta.revision == m_nativeRenderLineCacheRevision &&
	         m_nativeRenderCacheDelta.kind == NativeRenderCacheDeltaKind::RuntimeRangeRestitch &&
	         m_nativeRenderCacheDelta.newLineCount == sizeToInt(lines.size()))
	{
		const int oldSize          = qMax(0, m_nativeRenderCacheDelta.oldLineCount);
		const int newSize          = sizeToInt(lines.size());
		const int headTrimCount    = qBound(0, m_nativeRenderCacheDelta.headTrimCount, oldSize);
		const int preservedOldSize = qMax(0, oldSize - headTrimCount);
		const int stablePrefix =
		    qBound(0, m_nativeRenderCacheDelta.stablePrefixCount, qMin(preservedOldSize, newSize));
		const bool oldDimensionsMatch =
		    m_nativeLayoutVisualRows.size() == oldSize && m_nativeLayoutLineLayouts.size() == oldSize &&
		    m_nativeLayoutLineContentHashes.size() == oldSize &&
		    m_nativeLayoutRuntimeLineKeys.size() == oldSize && m_nativeLayoutRowsExact.size() == oldSize &&
		    m_nativeLayoutCumulativeHeights.size() == oldSize + 1;
		if (oldDimensionsMatch)
		{
			if (headTrimCount > 0)
				dropLayoutHead(headTrimCount);

			m_nativeLayoutVisualRows.resize(newSize);
			m_nativeLayoutLineLayouts.resize(newSize);
			m_nativeLayoutLineContentHashes.resize(newSize);
			m_nativeLayoutRuntimeLineKeys.resize(newSize);
			m_nativeLayoutRowsExact.resize(newSize);
			for (int i = stablePrefix; i < newSize; ++i)
			{
				m_nativeLayoutVisualRows[i] = -1;
				m_nativeLayoutLineLayouts[i].clear();
				m_nativeLayoutLineContentHashes[i] = 0;
				m_nativeLayoutRuntimeLineKeys[i]   = runtimeLineKeyForLine(lines.at(i));
				m_nativeLayoutRowsExact[i]         = 0;
			}

			m_nativeLayoutCumulativeHeights.resize(newSize + 1);
			if (!m_nativeLayoutCumulativeHeights.isEmpty())
				m_nativeLayoutCumulativeHeights[0] = 0.0;
			m_nativeLayoutCumulativeDirtyFrom =
			    qBound(0, qMin(m_nativeLayoutCumulativeDirtyFrom, stablePrefix), newSize);
			renderDeltaFastPathApplied = true;
		}
		else
		{
			renderDeltaFastPathApplied = preserveVerifiedLayoutPrefix(newSize, headTrimCount, stablePrefix);
		}
	}

	const bool layoutRangesMatch      = renderDeltaFastPathApplied ? true : [&]
	{
		if (m_nativeLayoutRuntimeLineKeys.size() != lines.size())
			return false;
		if (lines.isEmpty())
			return true;
		return m_nativeLayoutRuntimeLineKeys.constFirst() == runtimeLineKeyForLine(lines.constFirst()) &&
		       m_nativeLayoutRuntimeLineKeys.constLast() == runtimeLineKeyForLine(lines.constLast());
	}();
	const bool layoutRangesFullyMatch = (!renderDeltaFastPathApplied && renderRevisionChanged) ? [&]
	{
		if (m_nativeLayoutRuntimeLineKeys.size() != lines.size())
			return false;
		for (int i = 0; i < lines.size(); ++i)
		{
			if (m_nativeLayoutRuntimeLineKeys.at(i) != runtimeLineKeyForLine(lines.at(i)))
				return false;
		}
		return true;
	}()
	                                                                                           : false;
	if (cacheWasValid && !layoutCacheKeyChanged && !localWrapChanged && renderRevisionChanged &&
	    ((cacheDimensionsMatch && layoutRangesFullyMatch) ||
	     (renderDeltaFastPathApplied && m_nativeLayoutCumulativeDirtyFrom >= sizeToInt(lines.size()))))
	{
		m_nativeLayoutCachedRenderRevision = m_nativeRenderLineCacheRevision;
		return;
	}

	if (layoutCacheKeyChanged)
	{
		if (const bool preserveLineLayouts = cacheWasValid && !fontChanged && !styleChanged;
		    preserveLineLayouts)
		{
			if (m_nativeLayoutVisualRows.size() != lines.size())
				m_nativeLayoutVisualRows = QVector<int>(lines.size(), -1);
			else if (wrapChanged)
				std::ranges::fill(m_nativeLayoutVisualRows, -1);

			if (m_nativeLayoutLineLayouts.size() != lines.size())
				m_nativeLayoutLineLayouts = QVector<QSharedPointer<QTextLayout>>(lines.size());
			if (m_nativeLayoutLineContentHashes.size() != lines.size())
				m_nativeLayoutLineContentHashes = QVector<quint64>(lines.size(), 0);
			if (m_nativeLayoutRowsExact.size() != lines.size())
				m_nativeLayoutRowsExact = QVector<uchar>(lines.size(), 0);
		}
		else
		{
			m_nativeLayoutVisualRows        = QVector<int>(lines.size(), -1);
			m_nativeLayoutLineLayouts       = QVector<QSharedPointer<QTextLayout>>(lines.size());
			m_nativeLayoutLineContentHashes = QVector<quint64>(lines.size(), 0);
			m_nativeLayoutRowsExact         = QVector<uchar>(lines.size(), 0);
		}
		m_nativeLayoutCumulativeHeights    = QVector<qreal>(lines.size() + 1, 0.0);
		m_nativeLayoutCumulativeDirtyFrom  = 0;
		m_nativeLayoutCacheValid           = true;
		m_nativeLayoutCachedWrapWidth      = wrapWidthPixels;
		m_nativeLayoutCachedLocalWrapWidth = localWrapWidthPixels;
		m_nativeLayoutCachedLineSpacing    = lineSpacingSetting;
		m_nativeLayoutCachedStyleKey       = styleKey;
		m_nativeLayoutCachedLineAdvance    = defaultLineAdvance;
		m_nativeLayoutCachedFont           = layoutFont;
		m_nativeLayoutCachedRenderRevision = m_nativeRenderLineCacheRevision;
		refreshLayoutRuntimeLineKeys();
		++m_nativeLayoutCacheResets;
	}
	else if ((renderRevisionChanged && !renderDeltaFastPathApplied) || !layoutRangesMatch)
	{
		bool       remappedFastPath = false;
		const bool oldDimensionsMatch =
		    m_nativeLayoutRuntimeLineKeys.size() == m_nativeLayoutVisualRows.size() &&
		    m_nativeLayoutRuntimeLineKeys.size() == m_nativeLayoutLineLayouts.size() &&
		    m_nativeLayoutRuntimeLineKeys.size() == m_nativeLayoutLineContentHashes.size() &&
		    m_nativeLayoutRuntimeLineKeys.size() == m_nativeLayoutRowsExact.size();

		if (oldDimensionsMatch && !m_nativeLayoutRuntimeLineKeys.isEmpty() && !lines.isEmpty())
		{
			const int     oldSize = sizeToInt(m_nativeLayoutRuntimeLineKeys.size());
			const int     newSize = sizeToInt(lines.size());

			const quint64 firstNewLineKey = runtimeLineKeyForLine(lines.constFirst());
			int           oldStartIndex   = -1;
			for (int i = 0; i < oldSize; ++i)
			{
				if (m_nativeLayoutRuntimeLineKeys.at(i) == firstNewLineKey)
				{
					oldStartIndex = i;
					break;
				}
			}

			if (oldStartIndex >= 0)
			{
				int mappedCount = 0;
				while (mappedCount < newSize && oldStartIndex + mappedCount < oldSize &&
				       m_nativeLayoutRuntimeLineKeys.at(oldStartIndex + mappedCount) ==
				           runtimeLineKeyForLine(lines.at(mappedCount)))
				{
					++mappedCount;
				}

				const bool oldSuffixFullyMapped = oldStartIndex + mappedCount == oldSize;
				if (const bool simpleRemap =
				        mappedCount > 0 && (oldSuffixFullyMapped || mappedCount == newSize);
				    simpleRemap)
				{
					if (oldStartIndex > 0)
					{
						for (int i = 0; i < mappedCount; ++i)
						{
							const int sourceIndex        = oldStartIndex + i;
							m_nativeLayoutVisualRows[i]  = m_nativeLayoutVisualRows.at(sourceIndex);
							m_nativeLayoutLineLayouts[i] = std::move(m_nativeLayoutLineLayouts[sourceIndex]);
							m_nativeLayoutLineContentHashes[i] =
							    m_nativeLayoutLineContentHashes.at(sourceIndex);
							m_nativeLayoutRuntimeLineKeys[i] = m_nativeLayoutRuntimeLineKeys.at(sourceIndex);
							m_nativeLayoutRowsExact[i]       = m_nativeLayoutRowsExact.at(sourceIndex);
						}
					}

					m_nativeLayoutVisualRows.resize(newSize);
					m_nativeLayoutLineLayouts.resize(newSize);
					m_nativeLayoutLineContentHashes.resize(newSize);
					m_nativeLayoutRuntimeLineKeys.resize(newSize);
					m_nativeLayoutRowsExact.resize(newSize);
					for (int i = mappedCount; i < newSize; ++i)
					{
						m_nativeLayoutVisualRows[i] = -1;
						m_nativeLayoutLineLayouts[i].clear();
						m_nativeLayoutLineContentHashes[i] = 0;
						m_nativeLayoutRuntimeLineKeys[i]   = runtimeLineKeyForLine(lines.at(i));
						m_nativeLayoutRowsExact[i]         = 0;
					}

					m_nativeLayoutCumulativeHeights.resize(newSize + 1);
					if (oldStartIndex == 0)
					{
						m_nativeLayoutCumulativeDirtyFrom =
						    qBound(0, qMin(m_nativeLayoutCumulativeDirtyFrom, mappedCount), newSize);
					}
					else
					{
						m_nativeLayoutCumulativeHeights[0] = 0.0;
						qreal prefixDocY                   = 0.0;
						int   firstChangedVisualLine       = mappedCount;
						for (int i = 0; i < mappedCount; ++i)
						{
							const NativeOutputRenderLine &line = lines.at(i);
							const bool                    exactLineHasLayout =
							    m_nativeLayoutRowsExact.at(i) == 0 || line.text.isEmpty() ||
							    static_cast<bool>(m_nativeLayoutLineLayouts.at(i));
							if (m_nativeLayoutVisualRows.at(i) <= 0 || !exactLineHasLayout ||
							    m_nativeLayoutLineContentHashes.at(i) != lineContentHashForWidth(line))
							{
								firstChangedVisualLine = i;
								break;
							}

							prefixDocY +=
							    static_cast<qreal>(m_nativeLayoutVisualRows.at(i)) * defaultLineAdvance;
							m_nativeLayoutCumulativeHeights[i + 1] = prefixDocY;
						}
						m_nativeLayoutCumulativeDirtyFrom =
						    qBound(0, firstChangedVisualLine, sizeToInt(lines.size()));
					}

					remappedFastPath = true;
				}
			}
		}

		if (!remappedFastPath)
		{
			QVector<int>                         oldRows;
			QVector<quint64>                     oldLineKeys;
			QVector<QSharedPointer<QTextLayout>> oldLayouts;
			QVector<quint64>                     oldHashes;
			QVector<uchar>                       oldExactRows;
			oldRows.swap(m_nativeLayoutVisualRows);
			oldLineKeys.swap(m_nativeLayoutRuntimeLineKeys);
			oldLayouts.swap(m_nativeLayoutLineLayouts);
			oldHashes.swap(m_nativeLayoutLineContentHashes);
			oldExactRows.swap(m_nativeLayoutRowsExact);

			m_nativeLayoutVisualRows        = QVector<int>(lines.size(), -1);
			m_nativeLayoutLineLayouts       = QVector<QSharedPointer<QTextLayout>>(lines.size());
			m_nativeLayoutLineContentHashes = QVector<quint64>(lines.size(), 0);
			m_nativeLayoutRowsExact         = QVector<uchar>(lines.size(), 0);
			QVector<int> remappedOldIndexes(lines.size(), -1);

			const bool   oldDimensionsStillMatch =
			    !oldLineKeys.isEmpty() && oldLineKeys.size() == oldRows.size() &&
			    oldLineKeys.size() == oldLayouts.size() && oldLineKeys.size() == oldHashes.size() &&
			    oldLineKeys.size() == oldExactRows.size();
			auto mapLineFromOld = [&](const int newIndex, const int oldIndex)
			{
				if (newIndex < 0 || newIndex >= lines.size() || oldIndex < 0 ||
				    oldIndex >= oldLineKeys.size())
				{
					return;
				}
				m_nativeLayoutVisualRows[newIndex]        = oldRows.at(oldIndex);
				m_nativeLayoutLineLayouts[newIndex]       = std::move(oldLayouts[oldIndex]);
				m_nativeLayoutLineContentHashes[newIndex] = oldHashes.at(oldIndex);
				m_nativeLayoutRowsExact[newIndex]         = oldExactRows.at(oldIndex);
				remappedOldIndexes[newIndex]              = oldIndex;
			};
			if (oldDimensionsStillMatch && !lines.isEmpty())
			{
				int oldIndex = 0;
				for (int newIndex = 0; newIndex < lines.size() && oldIndex < oldLineKeys.size(); ++newIndex)
				{
					const quint64 newLineKey = runtimeLineKeyForLine(lines.at(newIndex));
					while (oldIndex < oldLineKeys.size() && oldLineKeys.at(oldIndex) != newLineKey)
					{
						++oldIndex;
					}
					if (oldIndex >= oldLineKeys.size())
						break;
					mapLineFromOld(newIndex, oldIndex);
					++oldIndex;
				}
			}

			refreshLayoutRuntimeLineKeys();
			m_nativeLayoutCumulativeHeights.resize(lines.size() + 1);
			m_nativeLayoutCumulativeHeights[0] = 0.0;
			auto lineCanReuseMappedLayout      = [&](const int i)
			{
				if (i < 0 || i >= lines.size())
					return false;

				const NativeOutputRenderLine &line = lines.at(i);
				const bool exactLineHasLayout = m_nativeLayoutRowsExact.at(i) == 0 || line.text.isEmpty() ||
				                                static_cast<bool>(m_nativeLayoutLineLayouts.at(i));
				if (m_nativeLayoutVisualRows.at(i) <= 0 || !exactLineHasLayout)
					return false;

				return m_nativeLayoutLineContentHashes.at(i) == lineContentHashForWidth(line);
			};

			int   firstChangedVisualLine = 0;
			qreal prefixDocY             = 0.0;
			while (firstChangedVisualLine < lines.size())
			{
				if (remappedOldIndexes.at(firstChangedVisualLine) < 0 ||
				    !lineCanReuseMappedLayout(firstChangedVisualLine))
				{
					break;
				}
				prefixDocY += static_cast<qreal>(m_nativeLayoutVisualRows.at(firstChangedVisualLine)) *
				              defaultLineAdvance;
				m_nativeLayoutCumulativeHeights[firstChangedVisualLine + 1] = prefixDocY;
				++firstChangedVisualLine;
			}
			m_nativeLayoutCumulativeDirtyFrom = qBound(0, firstChangedVisualLine, sizeToInt(lines.size()));
		}
	}
	else
	{
		if (m_nativeLayoutVisualRows.size() != lines.size())
		{
			m_nativeLayoutVisualRows          = QVector<int>(lines.size(), -1);
			m_nativeLayoutCumulativeDirtyFrom = 0;
		}
		if (m_nativeLayoutRowsExact.size() != lines.size())
		{
			m_nativeLayoutRowsExact           = QVector<uchar>(lines.size(), 0);
			m_nativeLayoutCumulativeDirtyFrom = 0;
		}
		if (m_nativeLayoutRuntimeLineKeys.size() != lines.size())
			refreshLayoutRuntimeLineKeys();
		if (m_nativeLayoutCumulativeHeights.size() != lines.size() + 1)
		{
			m_nativeLayoutCumulativeHeights.resize(lines.size() + 1);
			m_nativeLayoutCumulativeDirtyFrom = 0;
		}
		if (m_nativeLayoutLineLayouts.size() != lines.size())
		{
			m_nativeLayoutLineLayouts         = QVector<QSharedPointer<QTextLayout>>(lines.size());
			m_nativeLayoutLineContentHashes   = QVector<quint64>(lines.size(), 0);
			m_nativeLayoutCumulativeDirtyFrom = 0;
		}
		else if (m_nativeLayoutLineContentHashes.size() != lines.size())
		{
			m_nativeLayoutLineContentHashes   = QVector<quint64>(lines.size(), 0);
			m_nativeLayoutCumulativeDirtyFrom = 0;
		}
	}

	if (cacheWasValid && localWrapChanged)
	{
		m_nativeLayoutCachedLocalWrapWidth = localWrapWidthPixels;
		m_nativeLayoutCumulativeDirtyFrom  = 0;
	}

	if (m_nativeLayoutCumulativeHeights.size() != lines.size() + 1)
	{
		m_nativeLayoutCumulativeHeights.resize(lines.size() + 1);
		m_nativeLayoutCumulativeDirtyFrom = 0;
	}

	const int dirtyFrom = qBound(0, m_nativeLayoutCumulativeDirtyFrom, sizeToInt(lines.size()));
	if (dirtyFrom == 0)
		m_nativeLayoutCumulativeHeights[0] = 0.0;

	qreal              docY = (dirtyFrom > 0 && dirtyFrom < m_nativeLayoutCumulativeHeights.size())
	                              ? m_nativeLayoutCumulativeHeights.at(dirtyFrom)
	                              : 0.0;
	const QFontMetrics layoutFontMetrics(layoutFont);
	for (int i = dirtyFrom; i < lines.size(); ++i)
	{
		const NativeOutputRenderLine &line = lines.at(i);
		const bool hasLocalContent = (line.flags & (WorldRuntime::LineNote | WorldRuntime::LineInput)) != 0;
		const int  effectiveWrapWidth = hasLocalContent ? localWrapWidthPixels : wrapWidthPixels;
		const quint64 contentHash     = lineContentHashForWidth(line);
		const bool    hashMatches     = i < m_nativeLayoutLineContentHashes.size() &&
		                                m_nativeLayoutLineContentHashes.at(i) == contentHash;
		if (!hashMatches)
		{
			m_nativeLayoutLineLayouts[i].clear();
			m_nativeLayoutLineContentHashes[i] = contentHash;
			m_nativeLayoutVisualRows[i] = estimateNativeLineRows(line, effectiveWrapWidth, layoutFontMetrics);
			m_nativeLayoutRowsExact[i]  = line.text.isEmpty() ? 1 : 0;
		}
		else if (m_nativeLayoutVisualRows.at(i) <= 0)
		{
			m_nativeLayoutVisualRows[i] = estimateNativeLineRows(line, effectiveWrapWidth, layoutFontMetrics);
			m_nativeLayoutRowsExact[i]  = line.text.isEmpty() ? 1 : 0;
		}
		const int visualRowsForLine = qMax(1, m_nativeLayoutVisualRows.at(i));
		docY += static_cast<qreal>(visualRowsForLine) * defaultLineAdvance;
		if (i + 1 < m_nativeLayoutCumulativeHeights.size())
			m_nativeLayoutCumulativeHeights[i + 1] = docY;
	}
	m_nativeLayoutCumulativeDirtyFrom  = sizeToInt(lines.size());
	m_nativeLayoutCachedRenderRevision = m_nativeRenderLineCacheRevision;
}

const QTextLayout *WorldView::nativeLayoutForLine(const int index) const
{
	if (index < 0 || index >= m_nativeLayoutLineLayouts.size())
		return nullptr;
	if (const auto &layout = m_nativeLayoutLineLayouts.at(index); layout)
		return layout.data();
	return nullptr;
}

void WorldView::bumpNativeRenderLineCacheRevision(const NativeRenderCacheDeltaKind kind,
                                                  const int oldLineCount, const bool tailLineMutated,
                                                  const int headTrimCount, const int stablePrefixCount) const
{
	++m_nativeRenderLineCacheRevision;
	m_nativeRenderCacheDelta.kind              = kind;
	m_nativeRenderCacheDelta.revision          = m_nativeRenderLineCacheRevision;
	m_nativeRenderCacheDelta.oldLineCount      = qMax(0, oldLineCount);
	m_nativeRenderCacheDelta.newLineCount      = sizeToInt(m_nativeRenderLineCache.size());
	m_nativeRenderCacheDelta.tailLineMutated   = tailLineMutated;
	m_nativeRenderCacheDelta.headTrimCount     = qMax(0, headTrimCount);
	m_nativeRenderCacheDelta.stablePrefixCount = qMax(0, stablePrefixCount);
	if (m_nativeRenderCacheDelta.headTrimCount > 0)
		m_nativeSelectionPendingHeadTrimLines += m_nativeRenderCacheDelta.headTrimCount;
	m_nativeSplitTopHeadTrimPixelsRevision = 0;
	m_nativeSplitTopHeadTrimPixels         = 0;
}

void WorldView::markNativeRuntimeTailRestitchPending() const
{
	if (!m_runtime)
		return;

	m_nativeRenderLineCacheFromRuntime = true;
	m_nativeRuntimeTailRestitchPending = true;
	if (m_nativeLayoutCacheValid)
	{
		const int tailIndex               = qMax(0, sizeToInt(m_nativeLayoutVisualRows.size()) - 1);
		m_nativeLayoutCumulativeDirtyFrom = qMin(m_nativeLayoutCumulativeDirtyFrom, tailIndex);
	}
	requestNativeOutputTailRepaint();
}

void WorldView::markNativeRuntimeLineRestitchPending(const int runtimeLineIndex) const
{
	if (!m_runtime || runtimeLineIndex < 0)
		return;

	m_nativeRenderLineCacheFromRuntime = true;
	if (m_nativeRuntimeLineRestitchIndex < 0)
		m_nativeRuntimeLineRestitchIndex = runtimeLineIndex;
	else if (m_nativeRuntimeLineRestitchIndex != runtimeLineIndex)
	{
		m_nativeRuntimeRangeRestitchStartIndex =
		    m_nativeRuntimeRangeRestitchStartIndex < 0
		        ? qMin(m_nativeRuntimeLineRestitchIndex, runtimeLineIndex)
		        : qMin(m_nativeRuntimeRangeRestitchStartIndex,
		               qMin(m_nativeRuntimeLineRestitchIndex, runtimeLineIndex));
		m_nativeRuntimeLineRestitchIndex = -1;
	}
}

void WorldView::rebuildNativeRenderCacheFromLineEntries(const QVector<WorldRuntime::LineEntry> &lines,
                                                        const bool fromRuntimeSource) const
{
	removeNativePartialRenderLineOverlay(false);
	const int oldLineCount = sizeToInt(m_nativeRenderLineCache.size());
	m_nativeRenderLineCache.clear();
	m_nativeRenderLineCache.reserve(lines.size());

	QDateTime                        previousLineTime;
	QString                          currentLineText;
	QVector<WorldRuntime::StyleSpan> currentLineSpans;
	double                           currentLineOpacity      = 1.0;
	qint64                           currentFirstRuntimeLine = 0;
	qint64                           currentLastRuntimeLine  = 0;
	QVector<qint64>                  currentSourceRuntimeLines;
	int                              currentLineFlags            = 0;
	bool                             currentLogicalLineHasSource = false;
	for (const WorldRuntime::LineEntry &entry : lines)
	{
		if ((entry.flags & WorldRuntime::LineHidden) != 0)
			continue;

		QString                          displayText(entry.text);
		QVector<WorldRuntime::StyleSpan> displaySpans(entry.spans);
		buildDisplayLine(entry, previousLineTime, displayText, displaySpans);

		if (!currentLogicalLineHasSource)
		{
			currentFirstRuntimeLine     = entry.lineNumber;
			currentLastRuntimeLine      = entry.lineNumber;
			currentLineFlags            = entry.flags;
			currentLogicalLineHasSource = true;
		}
		else
		{
			if (currentSourceRuntimeLines.isEmpty())
				currentSourceRuntimeLines.push_back(currentFirstRuntimeLine);
			currentSourceRuntimeLines.push_back(entry.lineNumber);
			currentLineFlags |= entry.flags;
			currentFirstRuntimeLine = qMin(currentFirstRuntimeLine, entry.lineNumber);
			currentLastRuntimeLine  = qMax(currentLastRuntimeLine, entry.lineNumber);
		}

		currentLineText += displayText;
		currentLineOpacity = lineOpacityForTimestamp(entry.time);
		appendPositiveStyleSpans(currentLineSpans, displaySpans);

		if (entry.hardReturn)
		{
			NativeOutputRenderLine renderLine{
			    std::move(currentLineText),
			    std::move(currentLineSpans),
			    currentLineOpacity,
			    currentFirstRuntimeLine,
			    currentLastRuntimeLine,
			    currentLineFlags,
			    0,
			    std::move(currentSourceRuntimeLines),
			};
			renderLine.visualHash = nativeLineContentHash(renderLine);
			renderLine.sourceRuntimeLineKey =
			    nativeRuntimeLineKey(renderLine.sourceRuntimeLineNumbers, renderLine.firstRuntimeLineNumber,
			                         renderLine.lastRuntimeLineNumber);
			m_nativeRenderLineCache.push_back(std::move(renderLine));
			currentLineText.clear();
			currentLineSpans.clear();
			currentSourceRuntimeLines.clear();
			currentLineOpacity          = 1.0;
			currentFirstRuntimeLine     = 0;
			currentLastRuntimeLine      = 0;
			currentLineFlags            = 0;
			currentLogicalLineHasSource = false;
		}
		previousLineTime = entry.time;
	}

	if (!currentLineText.isEmpty() && currentLogicalLineHasSource)
	{
		NativeOutputRenderLine renderLine{
		    std::move(currentLineText),
		    std::move(currentLineSpans),
		    currentLineOpacity,
		    currentFirstRuntimeLine,
		    currentLastRuntimeLine,
		    currentLineFlags,
		    0,
		    std::move(currentSourceRuntimeLines),
		};
		renderLine.visualHash = nativeLineContentHash(renderLine);
		renderLine.sourceRuntimeLineKey =
		    nativeRuntimeLineKey(renderLine.sourceRuntimeLineNumbers, renderLine.firstRuntimeLineNumber,
		                         renderLine.lastRuntimeLineNumber);
		m_nativeRenderLineCache.push_back(std::move(renderLine));
	}

	m_nativeRenderLineCacheValid       = true;
	m_nativeRenderLineCacheFromRuntime = fromRuntimeSource;

	if (!fromRuntimeSource || lines.isEmpty())
	{
		m_nativeCachedRuntimeCount                 = 0;
		m_nativeCachedRuntimeFirstLineNumber       = 0;
		m_nativeCachedRuntimeLastLineNumber        = 0;
		m_nativeCachedRuntimeLastHardReturn        = true;
		m_nativeCachedRuntimeLineNumbersContiguous = true;
		m_nativeCachedRuntimeFirstEntry            = {};
		m_nativeCachedRuntimeLastEntry             = {};
	}
	else
	{
		m_nativeCachedRuntimeCount                 = sizeToInt(lines.size());
		m_nativeCachedRuntimeFirstLineNumber       = lines.first().lineNumber;
		m_nativeCachedRuntimeLastLineNumber        = lines.last().lineNumber;
		m_nativeCachedRuntimeLastHardReturn        = lines.last().hardReturn;
		m_nativeCachedRuntimeLineNumbersContiguous = runtimeLineNumbersAreContiguous(lines);
		m_nativeCachedRuntimeFirstEntry            = lines.first();
		m_nativeCachedRuntimeLastEntry             = lines.last();
	}

	++m_nativeRenderCacheFullRebuilds;
	bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::FullReset, oldLineCount);
}

const QVector<WorldView::NativeOutputRenderLine> &WorldView::nativeOutputRenderLines() const
{
	enum class RuntimeRebuildReason
	{
		CacheInvalid,
		RuntimeDisjoint,
		NonContiguousNoOverlap,
		RestitchFailure,
		AppendStartOutOfRange,
	};

	auto incrementRebuildReason = [this](const RuntimeRebuildReason reason)
	{
		switch (reason)
		{
		case RuntimeRebuildReason::CacheInvalid:
			++m_nativeRenderCacheRebuildReasonCacheInvalid;
			break;
		case RuntimeRebuildReason::RuntimeDisjoint:
			++m_nativeRenderCacheRebuildReasonRuntimeDisjoint;
			break;
		case RuntimeRebuildReason::NonContiguousNoOverlap:
			++m_nativeRenderCacheRebuildReasonNonContigNoOverlap;
			break;
		case RuntimeRebuildReason::RestitchFailure:
			++m_nativeRenderCacheRebuildReasonRestitchFailure;
			break;
		case RuntimeRebuildReason::AppendStartOutOfRange:
			++m_nativeRenderCacheRebuildReasonAppendIndex;
			break;
		}
	};

	auto clearRuntimeCache = [this]
	{
		const int  oldLineCount = sizeToInt(m_nativeRenderLineCache.size());
		const bool hadCacheState =
		    !m_nativeRenderLineCache.isEmpty() || !m_nativeRenderLineCacheValid ||
		    !m_nativeRenderLineCacheFromRuntime || m_nativeCachedRuntimeCount != 0 ||
		    m_nativeCachedRuntimeFirstLineNumber != 0 || m_nativeCachedRuntimeLastLineNumber != 0 ||
		    !m_nativeCachedRuntimeLastHardReturn || !m_nativeCachedRuntimeLineNumbersContiguous;
		m_nativeRenderLineCache.clear();
		m_nativeRenderLineCacheValid               = true;
		m_nativeRenderLineCacheFromRuntime         = true;
		m_nativeCachedRuntimeCount                 = 0;
		m_nativeCachedRuntimeFirstLineNumber       = 0;
		m_nativeCachedRuntimeLastLineNumber        = 0;
		m_nativeCachedRuntimeLastHardReturn        = true;
		m_nativeCachedRuntimeLineNumbersContiguous = true;
		m_nativeCachedRuntimeFirstEntry            = {};
		m_nativeCachedRuntimeLastEntry             = {};
		if (hadCacheState)
			bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::FullReset, oldLineCount);
	};

	auto fullRebuildFromStandalone = [this]
	{
		recordNativeAppendDiagnostic(NativeAppendDiagnosticKind::FullRebuild, 0,
		                             sizeToInt(m_nativeStandaloneOutputLines.size()));
		rebuildNativeRenderCacheFromLineEntries(m_nativeStandaloneOutputLines, false);
	};

	auto fullRebuildFromRuntime = [this](const QVector<WorldRuntime::LineEntry> &runtimeLines)
	{
		m_nativeRuntimeTailRestitchPending     = false;
		m_nativeRuntimeLineRestitchIndex       = -1;
		m_nativeRuntimeRangeRestitchStartIndex = -1;
		recordNativeAppendDiagnostic(NativeAppendDiagnosticKind::FullRebuild, 0,
		                             sizeToInt(runtimeLines.size()));
		rebuildNativeRenderCacheFromLineEntries(runtimeLines, true);
	};

	if (nativeRenderBaseCacheMatchesCurrentSource())
		return finalizeNativeOutputRenderLines();
	removeNativePartialRenderLineOverlay(false);

	if (!m_runtime)
	{
		if (!m_nativeRenderLineCacheValid || m_nativeRenderLineCacheFromRuntime)
			fullRebuildFromStandalone();
		return finalizeNativeOutputRenderLines();
	}
	const QVector<WorldRuntime::LineEntry> &runtimeLines = m_runtime->lines();
	if (m_nativeRenderLineCacheValid && !m_nativeRenderLineCacheFromRuntime)
	{
		// Recover from stale pinned state: if runtime lines exist but the pinned
		// cache is empty, rebuild from runtime so the native canvas cannot remain
		// gray indefinitely.
		if (m_nativeRenderLineCache.isEmpty() && !runtimeLines.isEmpty())
			fullRebuildFromRuntime(runtimeLines);
		return finalizeNativeOutputRenderLines();
	}
	if (!m_nativeRenderLineCacheFromRuntime)
	{
		if (!runtimeLines.isEmpty())
		{
			fullRebuildFromRuntime(runtimeLines);
			return finalizeNativeOutputRenderLines();
		}
		const int oldLineCount = sizeToInt(m_nativeRenderLineCache.size());
		m_nativeRenderLineCache.clear();
		if (oldLineCount > 0)
		{
			bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::FullReset, oldLineCount);
		}
		m_nativeRenderLineCacheValid = true;
		return finalizeNativeOutputRenderLines();
	}

	if (runtimeLines.isEmpty())
	{
		clearRuntimeCache();
		return finalizeNativeOutputRenderLines();
	}

	auto previousRenderableRuntimeIndex = [&runtimeLines](const int beforeIndex)
	{
		const int startIndex = qMin(beforeIndex - 1, sizeToInt(runtimeLines.size()) - 1);
		for (int i = startIndex; i >= 0; --i)
		{
			if ((runtimeLines.at(i).flags & WorldRuntime::LineHidden) == 0)
				return i;
		}
		return -1;
	};

	auto nextRenderableRuntimeIndex = [&runtimeLines](const int fromIndex)
	{
		const int startIndex = qBound(0, fromIndex, sizeToInt(runtimeLines.size()));
		for (int i = startIndex; i < runtimeLines.size(); ++i)
		{
			if ((runtimeLines.at(i).flags & WorldRuntime::LineHidden) == 0)
				return i;
		}
		return -1;
	};

	auto renderIndexForRuntimeLineNumberNear = [this, &runtimeLines](const qint64 runtimeLineNumber,
	                                                                 const int    runtimeIndex,
	                                                                 const bool   searchFromTail)
	{
		const int renderLineCount = sizeToInt(m_nativeRenderLineCache.size());
		if (runtimeLineNumber <= 0 || renderLineCount <= 0 || runtimeLines.isEmpty())
			return -1;

		constexpr int kLocalRenderLookupRadius = 96;
		const int     boundedRuntimeIndex      = qBound(0, runtimeIndex, sizeToInt(runtimeLines.size()) - 1);
		const int     estimatedRenderIndex =
		    qBound(0,
		           sizeToInt((static_cast<qint64>(boundedRuntimeIndex) * renderLineCount) /
		                     qMax<qint64>(1, runtimeLines.size())),
		           renderLineCount - 1);

		auto findInRange =
		    [this, runtimeLineNumber](const int firstIndex, const int lastIndex, const bool reverse)
		{
			if (firstIndex > lastIndex)
				return -1;
			if (reverse)
			{
				for (int renderIndex = lastIndex; renderIndex >= firstIndex; --renderIndex)
				{
					if (nativeRuntimeLineRangeContains(m_nativeRenderLineCache.at(renderIndex),
					                                   runtimeLineNumber))
					{
						return renderIndex;
					}
				}
				return -1;
			}
			for (int renderIndex = firstIndex; renderIndex <= lastIndex; ++renderIndex)
			{
				if (nativeRuntimeLineRangeContains(m_nativeRenderLineCache.at(renderIndex),
				                                   runtimeLineNumber))
				{
					return renderIndex;
				}
			}
			return -1;
		};

		const int localFirst = qMax(0, estimatedRenderIndex - kLocalRenderLookupRadius);
		const int localLast  = qMin(renderLineCount - 1, estimatedRenderIndex + kLocalRenderLookupRadius);
		if (const int renderIndex = findInRange(localFirst, localLast, searchFromTail); renderIndex >= 0)
			return renderIndex;

		const int tailFirst = qMax(0, renderLineCount - kLocalRenderLookupRadius);
		return findInRange(tailFirst, renderLineCount - 1, true);
	};

	auto logicalRenderLineStartIndex =
	    [&runtimeLines, &previousRenderableRuntimeIndex](const int runtimeIndex)
	{
		int startIndex = runtimeIndex;
		for (;;)
		{
			const int previousIndex = previousRenderableRuntimeIndex(startIndex);
			if (previousIndex < 0 || runtimeLines.at(previousIndex).hardReturn)
				return startIndex;
			startIndex = previousIndex;
		}
	};

	auto renderLineMaxRuntimeIndexBefore =
	    [&runtimeLines](const NativeOutputRenderLine &line, const int beforeIndex)
	{
		auto runtimeIndexForLineNumberBefore = [&runtimeLines, beforeIndex](const qint64 lineNumber)
		{
			return findRuntimeLineIndexByNumberNear(
			    runtimeLines, lineNumber, qMin(beforeIndex - 1, sizeToInt(runtimeLines.size()) - 1), true);
		};

		if (line.sourceRuntimeLineNumbers.isEmpty())
		{
			if (line.lastRuntimeLineNumber <= 0)
				return -1;
			const int lastRuntimeIndex = runtimeIndexForLineNumberBefore(line.lastRuntimeLineNumber);
			if (lastRuntimeIndex < 0)
				return -1;
			if (line.firstRuntimeLineNumber > 0 &&
			    line.firstRuntimeLineNumber != line.lastRuntimeLineNumber &&
			    runtimeIndexForLineNumberBefore(line.firstRuntimeLineNumber) < 0)
			{
				return -1;
			}
			return lastRuntimeIndex;
		}

		int maxRuntimeIndex = -1;
		for (const qint64 lineNumber : line.sourceRuntimeLineNumbers)
		{
			const int runtimeIndex = runtimeIndexForLineNumberBefore(lineNumber);
			if (runtimeIndex < 0)
				return -1;
			maxRuntimeIndex = qMax(maxRuntimeIndex, runtimeIndex);
		}
		return maxRuntimeIndex;
	};

	auto appendRuntimeRange =
	    [this, &runtimeLines, &previousRenderableRuntimeIndex](
	        const int startIndex, const bool suppressRevisionBump = false, bool *outCacheChanged = nullptr,
	        bool *outTailMutated = nullptr, int *outOldLineCount = nullptr, const int endExclusive = -1,
	        const NativeAppendDiagnosticKind diagnosticKind = NativeAppendDiagnosticKind::TailAppend)
	{
		if (outCacheChanged)
			*outCacheChanged = false;
		if (outTailMutated)
			*outTailMutated = false;
		if (outOldLineCount)
			*outOldLineCount = sizeToInt(m_nativeRenderLineCache.size());
		if (startIndex < 0 || startIndex >= runtimeLines.size())
			return;
		const int rangeEnd = endExclusive < 0
		                         ? sizeToInt(runtimeLines.size())
		                         : qBound(startIndex, endExclusive, sizeToInt(runtimeLines.size()));
		if (startIndex >= rangeEnd)
			return;

		const int oldLineCount                      = sizeToInt(m_nativeRenderLineCache.size());
		bool      cacheChanged                      = false;
		bool      tailMutated                       = false;
		int       pendingMutatedRenderLineHashIndex = -1;
		QDateTime previousLineTime;
		if (const int previousIndex = previousRenderableRuntimeIndex(startIndex); previousIndex >= 0)
		{
			const WorldRuntime::LineEntry &previousEntry = runtimeLines.at(previousIndex);
			m_nativeCachedRuntimeLastHardReturn          = previousEntry.hardReturn;
			m_nativeCachedRuntimeLastEntry               = previousEntry;
			previousLineTime                             = runtimeLines.at(previousIndex).time;
		}
		else
		{
			m_nativeCachedRuntimeLastHardReturn = true;
			m_nativeCachedRuntimeLastEntry      = {};
		}

		for (int i = startIndex; i < rangeEnd; ++i)
		{
			const WorldRuntime::LineEntry &entry = runtimeLines.at(i);
			if ((entry.flags & WorldRuntime::LineHidden) != 0)
				continue;

			QString                          displayText(entry.text);
			QVector<WorldRuntime::StyleSpan> displaySpans(entry.spans);
			buildDisplayLine(entry, previousLineTime, displayText, displaySpans);

			if (!m_nativeCachedRuntimeLastHardReturn && !m_nativeRenderLineCache.isEmpty())
			{
				NativeOutputRenderLine &line = m_nativeRenderLineCache.last();
				tailMutated                  = true;
				line.text += displayText;
				line.opacity = lineOpacityForTimestamp(entry.time);
				extendNativeRuntimeLineRange(line, entry.lineNumber);
				line.flags |= entry.flags;
				cacheChanged = true;
				if (!displaySpans.isEmpty())
					appendPositiveStyleSpans(line.spans, displaySpans);
				pendingMutatedRenderLineHashIndex = sizeToInt(m_nativeRenderLineCache.size()) - 1;
			}
			else
			{
				removeNonPositiveStyleSpans(displaySpans);
				NativeOutputRenderLine renderLine{
				    std::move(displayText),
				    std::move(displaySpans),
				    lineOpacityForTimestamp(entry.time),
				    entry.lineNumber,
				    entry.lineNumber,
				    entry.flags,
				    0,
				    {},
				};
				renderLine.visualHash = nativeLineContentHash(renderLine);
				renderLine.sourceRuntimeLineKey =
				    nativeRuntimeLineKey(renderLine.sourceRuntimeLineNumbers,
				                         renderLine.firstRuntimeLineNumber, renderLine.lastRuntimeLineNumber);
				m_nativeRenderLineCache.push_back(std::move(renderLine));
				cacheChanged = true;
			}

			m_nativeCachedRuntimeLastHardReturn = entry.hardReturn;
			previousLineTime                    = entry.time;
			if (entry.hardReturn && pendingMutatedRenderLineHashIndex >= 0 &&
			    pendingMutatedRenderLineHashIndex < m_nativeRenderLineCache.size())
			{
				NativeOutputRenderLine &line = m_nativeRenderLineCache[pendingMutatedRenderLineHashIndex];
				line.visualHash              = nativeLineContentHash(line);
				pendingMutatedRenderLineHashIndex = -1;
			}
		}
		if (pendingMutatedRenderLineHashIndex >= 0 &&
		    pendingMutatedRenderLineHashIndex < m_nativeRenderLineCache.size())
		{
			NativeOutputRenderLine &line = m_nativeRenderLineCache[pendingMutatedRenderLineHashIndex];
			line.visualHash              = nativeLineContentHash(line);
		}
		if (outCacheChanged)
			*outCacheChanged = cacheChanged;
		if (outTailMutated)
			*outTailMutated = tailMutated;
		if (outOldLineCount)
			*outOldLineCount = oldLineCount;
		if (cacheChanged)
		{
			recordNativeAppendDiagnostic(diagnosticKind, startIndex, rangeEnd - startIndex);
			++m_nativeRenderCacheIncrementalUpdates;
			if (!suppressRevisionBump)
			{
				bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::TailAppend, oldLineCount,
				                                  tailMutated);
			}
		}
	};

	auto restitchTrimmedHeadLogicalLine =
	    [this, &runtimeLines](const qint64 newFirstLineNumber, const bool suppressRevisionBump = false)
	{
		if (m_nativeRenderLineCache.isEmpty())
			return true;

		const NativeOutputRenderLine &existingHead = m_nativeRenderLineCache.first();
		if (!existingHead.sourceRuntimeLineNumbers.isEmpty())
		{
			bool hasTrimmedSource   = false;
			bool hasRemainingSource = false;
			for (const qint64 lineNumber : existingHead.sourceRuntimeLineNumbers)
			{
				if (lineNumber < newFirstLineNumber)
					hasTrimmedSource = true;
				else
					hasRemainingSource = true;
			}
			if (!hasTrimmedSource)
				return true;
			if (!hasRemainingSource)
				return false;
		}
		else
		{
			if (existingHead.firstRuntimeLineNumber >= newFirstLineNumber)
				return true;
			if (existingHead.lastRuntimeLineNumber < newFirstLineNumber)
				return false;
		}

		const int startIndex = findRuntimeLineIndexByNumberNear(runtimeLines, newFirstLineNumber, 0, false);
		if (startIndex < 0 || startIndex >= runtimeLines.size())
			return false;

		QString                          rebuiltText;
		QVector<WorldRuntime::StyleSpan> rebuiltSpans;
		double                           rebuiltOpacity      = 1.0;
		qint64                           rebuiltFirstRuntime = 0;
		qint64                           rebuiltLastRuntime  = 0;
		QVector<qint64>                  rebuiltSourceRuntimeLines;
		int                              rebuiltFlags = 0;
		bool                             hasSource    = false;
		bool                             closed       = false;
		QDateTime                        previousLineTime;

		for (int i = startIndex; i < runtimeLines.size(); ++i)
		{
			const WorldRuntime::LineEntry   &entry = runtimeLines.at(i);
			QString                          displayText(entry.text);
			QVector<WorldRuntime::StyleSpan> displaySpans(entry.spans);
			buildDisplayLine(entry, previousLineTime, displayText, displaySpans);

			if (!hasSource)
			{
				rebuiltFirstRuntime = entry.lineNumber;
				rebuiltLastRuntime  = entry.lineNumber;
				rebuiltFlags        = entry.flags;
				hasSource           = true;
			}
			else
			{
				rebuiltFlags |= entry.flags;
				rebuiltFirstRuntime = qMin(rebuiltFirstRuntime, entry.lineNumber);
				rebuiltLastRuntime  = qMax(rebuiltLastRuntime, entry.lineNumber);
			}
			rebuiltSourceRuntimeLines.push_back(entry.lineNumber);

			rebuiltText += displayText;
			rebuiltOpacity = lineOpacityForTimestamp(entry.time);
			appendPositiveStyleSpans(rebuiltSpans, displaySpans);

			if (entry.hardReturn)
			{
				closed = true;
				break;
			}
			previousLineTime = entry.time;
		}

		if (!hasSource || rebuiltSourceRuntimeLines.isEmpty() ||
		    rebuiltSourceRuntimeLines.constFirst() != newFirstLineNumber)
			return false;
		if (!existingHead.sourceRuntimeLineNumbers.isEmpty())
		{
			const qsizetype suffixStart = existingHead.sourceRuntimeLineNumbers.indexOf(newFirstLineNumber);
			if (suffixStart < 0)
				return false;
			const QVector<qint64> expectedSourceRuntimeLines =
			    existingHead.sourceRuntimeLineNumbers.sliced(suffixStart);
			if (rebuiltSourceRuntimeLines != expectedSourceRuntimeLines)
				return false;
		}
		else if (rebuiltLastRuntime != existingHead.lastRuntimeLineNumber)
		{
			return false;
		}
		if (!closed && rebuiltLastRuntime != runtimeLines.last().lineNumber)
			return false;

		NativeOutputRenderLine rebuiltLine{
		    std::move(rebuiltText),
		    std::move(rebuiltSpans),
		    rebuiltOpacity,
		    rebuiltFirstRuntime,
		    rebuiltLastRuntime,
		    rebuiltFlags,
		    0,
		    std::move(rebuiltSourceRuntimeLines),
		};
		rebuiltLine.visualHash = nativeLineContentHash(rebuiltLine);
		rebuiltLine.sourceRuntimeLineKey =
		    nativeRuntimeLineKey(rebuiltLine.sourceRuntimeLineNumbers, rebuiltLine.firstRuntimeLineNumber,
		                         rebuiltLine.lastRuntimeLineNumber);
		const int oldLineCount          = sizeToInt(m_nativeRenderLineCache.size());
		m_nativeRenderLineCache.first() = std::move(rebuiltLine);
		if (!suppressRevisionBump)
		{
			++m_nativeRenderCacheIncrementalUpdates;
			bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::HeadMutation, oldLineCount);
		}
		return true;
	};

	if (!m_nativeRenderLineCacheValid || !m_nativeRenderLineCacheFromRuntime)
	{
		incrementRebuildReason(RuntimeRebuildReason::CacheInvalid);
		fullRebuildFromRuntime(runtimeLines);
		return finalizeNativeOutputRenderLines();
	}

	const qint64 newFirstLineNumber   = runtimeLines.first().lineNumber;
	const qint64 newLastLineNumber    = runtimeLines.last().lineNumber;
	const qint64 oldFirstLineNumber   = m_nativeCachedRuntimeFirstLineNumber;
	const qint64 oldLastLineNumber    = m_nativeCachedRuntimeLastLineNumber;
	const bool   runtimeNowContiguous = newLastLineNumber >= newFirstLineNumber &&
	                                    (newLastLineNumber - newFirstLineNumber + 1) == runtimeLines.size();

	auto         renderLineBeforeRuntimeLine = [](const NativeOutputRenderLine &line, const qint64 lineNumber)
	{ return line.lastRuntimeLineNumber > 0 && line.lastRuntimeLineNumber < lineNumber; };

	auto renderLineStraddlesRuntimeLine = [](const NativeOutputRenderLine &line, const qint64 lineNumber)
	{
		return line.firstRuntimeLineNumber > 0 && line.firstRuntimeLineNumber < lineNumber &&
		       line.lastRuntimeLineNumber >= lineNumber;
	};

	auto finalizeRuntimeCacheMetadata =
	    [this, &runtimeLines, &newFirstLineNumber, &newLastLineNumber](const bool contiguous)
	{
		m_nativeRenderLineCacheValid               = true;
		m_nativeRenderLineCacheFromRuntime         = true;
		m_nativeCachedRuntimeCount                 = sizeToInt(runtimeLines.size());
		m_nativeCachedRuntimeFirstLineNumber       = newFirstLineNumber;
		m_nativeCachedRuntimeLastLineNumber        = newLastLineNumber;
		m_nativeCachedRuntimeLastHardReturn        = runtimeLines.last().hardReturn;
		m_nativeCachedRuntimeLineNumbersContiguous = contiguous;
		m_nativeCachedRuntimeFirstEntry            = runtimeLines.first();
		m_nativeCachedRuntimeLastEntry             = runtimeLines.last();
	};

	auto softRebuildFromRuntime = [this, &appendRuntimeRange, &clearRuntimeCache,
	                               &runtimeLines](const bool contiguous, auto &&finalizeMetadata,
	                                              const NativeAppendDiagnosticKind diagnosticKind)
	{
		m_nativeRuntimeTailRestitchPending     = false;
		m_nativeRuntimeLineRestitchIndex       = -1;
		m_nativeRuntimeRangeRestitchStartIndex = -1;
		clearRuntimeCache();
		m_nativeRenderLineCache.reserve(runtimeLines.size());
		appendRuntimeRange(0, false, nullptr, nullptr, nullptr, -1, diagnosticKind);
		finalizeMetadata(contiguous);
		++m_nativeRenderCacheSoftRebuilds;
	};

	auto promoteRuntimeLineRestitchToRange = [this](const int runtimeIndex)
	{
		if (runtimeIndex < 0)
			return;
		m_nativeRuntimeRangeRestitchStartIndex =
		    m_nativeRuntimeRangeRestitchStartIndex < 0
		        ? runtimeIndex
		        : qMin(m_nativeRuntimeRangeRestitchStartIndex, runtimeIndex);
	};

	auto promoteRuntimeTailRestitchToRange =
	    [&runtimeLines, &promoteRuntimeLineRestitchToRange](const qint64 runtimeLineNumber)
	{
		const int runtimeIndex = findRuntimeLineIndexByNumberNear(runtimeLines, runtimeLineNumber,
		                                                          sizeToInt(runtimeLines.size()) - 1, true);
		if (runtimeIndex >= 0)
			promoteRuntimeLineRestitchToRange(runtimeIndex);
		else if (!runtimeLines.isEmpty())
			promoteRuntimeLineRestitchToRange(qMax(0, sizeToInt(runtimeLines.size()) - 2));
	};

	auto restitchPendingRuntimeTail = [&]() -> bool
	{
		if (!m_nativeRuntimeTailRestitchPending)
			return true;
		m_nativeRuntimeTailRestitchPending = false;

		if (!m_nativeRenderLineCacheValid || !m_nativeRenderLineCacheFromRuntime ||
		    m_nativeRenderLineCache.isEmpty() || oldLastLineNumber <= 0)
		{
			return true;
		}

		const qint64 tailFirstLineNumber = m_nativeRenderLineCache.constLast().firstRuntimeLineNumber;
		const int    tailLookupHint      = sizeToInt(runtimeLines.size()) - 1;
		const int    tailStartIndex =
		    findRuntimeLineIndexByNumberNear(runtimeLines, tailFirstLineNumber, tailLookupHint, true);
		const int cachedLastIndex =
		    findRuntimeLineIndexByNumberNear(runtimeLines, oldLastLineNumber, tailLookupHint, true);
		if (tailStartIndex < 0 || cachedLastIndex < tailStartIndex || cachedLastIndex >= runtimeLines.size())
		{
			recordNativeRestitchFailureDiagnostic(NativeRestitchFailureDiagnosticKind::TailIndexMiss,
			                                      qMax(0, tailStartIndex));
			promoteRuntimeTailRestitchToRange(tailFirstLineNumber);
			return false;
		}

		const int oldLineCount = sizeToInt(m_nativeRenderLineCache.size());
		m_nativeRenderLineCache.removeLast();
		if (tailStartIndex > 0)
		{
			const WorldRuntime::LineEntry &previousEntry = runtimeLines.at(tailStartIndex - 1);
			m_nativeCachedRuntimeLastHardReturn          = previousEntry.hardReturn;
			m_nativeCachedRuntimeLastEntry               = previousEntry;
		}
		else
		{
			m_nativeCachedRuntimeLastHardReturn = true;
			m_nativeCachedRuntimeLastEntry      = {};
		}

		bool appendChanged     = false;
		bool appendTailMutated = false;
		appendRuntimeRange(tailStartIndex, true, &appendChanged, &appendTailMutated, nullptr,
		                   cachedLastIndex + 1, NativeAppendDiagnosticKind::TailRestitch);
		if (!appendChanged)
		{
			recordNativeRestitchFailureDiagnostic(NativeRestitchFailureDiagnosticKind::TailAppendNoChange,
			                                      tailStartIndex);
			promoteRuntimeLineRestitchToRange(tailStartIndex);
			return false;
		}

		const WorldRuntime::LineEntry &cachedLastEntry = runtimeLines.at(cachedLastIndex);
		m_nativeCachedRuntimeLastHardReturn            = cachedLastEntry.hardReturn;
		m_nativeCachedRuntimeLastEntry                 = cachedLastEntry;
		bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::TailAppend, oldLineCount, true);
		return true;
	};

	bool runtimeRangeRestitchApplied = false;
	auto restitchPendingRuntimeRange = [&]() -> bool
	{
		const int requestedStartIndex = m_nativeRuntimeRangeRestitchStartIndex;
		if (requestedStartIndex < 0)
			return true;
		m_nativeRuntimeRangeRestitchStartIndex = -1;
		m_nativeRuntimeLineRestitchIndex       = -1;

		if (!m_nativeRenderLineCacheValid || !m_nativeRenderLineCacheFromRuntime ||
		    m_nativeRenderLineCache.isEmpty())
		{
			return true;
		}

		const int restitchStartIndex = qBound(0, requestedStartIndex, sizeToInt(runtimeLines.size()));
		if (restitchStartIndex >= runtimeLines.size())
			return true;

		const int oldLineCount     = sizeToInt(m_nativeRenderLineCache.size());
		int       trimmedHeadCount = 0;
		if (newFirstLineNumber > oldFirstLineNumber)
		{
			while (
			    trimmedHeadCount < m_nativeRenderLineCache.size() &&
			    renderLineBeforeRuntimeLine(m_nativeRenderLineCache.at(trimmedHeadCount), newFirstLineNumber))
			{
				++trimmedHeadCount;
			}
			if (trimmedHeadCount > 0)
			{
				m_nativeRenderLineCache.remove(0, trimmedHeadCount);
				++m_nativeRenderCacheTrimDrops;
			}
			if (!m_nativeRenderLineCache.isEmpty() &&
			    renderLineStraddlesRuntimeLine(m_nativeRenderLineCache.first(), newFirstLineNumber))
			{
				if (!restitchTrimmedHeadLogicalLine(newFirstLineNumber, true))
				{
					recordNativeRestitchFailureDiagnostic(NativeRestitchFailureDiagnosticKind::RangeHeadTrim,
					                                      restitchStartIndex);
					return false;
				}
			}
		}
		if (m_nativeRenderLineCache.isEmpty())
		{
			recordNativeRestitchFailureDiagnostic(NativeRestitchFailureDiagnosticKind::RangeEmpty,
			                                      restitchStartIndex);
			return false;
		}

		int       dropRenderIndex     = -1;
		int       rebuildStartIndex   = restitchStartIndex;
		const int nextRenderableIndex = nextRenderableRuntimeIndex(restitchStartIndex);
		if (nextRenderableIndex < 0)
		{
			runtimeRangeRestitchApplied = true;
			return true;
		}
		const bool searchRenderCacheFromTail = restitchStartIndex >= sizeToInt(runtimeLines.size()) / 2;
		const int  previousRuntimeIndex      = previousRenderableRuntimeIndex(nextRenderableIndex);
		if (previousRuntimeIndex >= 0)
		{
			const WorldRuntime::LineEntry &previousEntry       = runtimeLines.at(previousRuntimeIndex);
			const int                      previousRenderIndex = renderIndexForRuntimeLineNumberNear(
			    previousEntry.lineNumber, previousRuntimeIndex, searchRenderCacheFromTail);
			if (previousRenderIndex >= 0)
			{
				if (previousEntry.hardReturn)
				{
					dropRenderIndex   = previousRenderIndex + 1;
					rebuildStartIndex = nextRenderableIndex;
				}
				else
				{
					dropRenderIndex   = previousRenderIndex;
					rebuildStartIndex = logicalRenderLineStartIndex(previousRuntimeIndex);
				}
			}
		}
		if (dropRenderIndex < 0)
		{
			const int nextRenderIndex =
			    renderIndexForRuntimeLineNumberNear(runtimeLines.at(nextRenderableIndex).lineNumber,
			                                        nextRenderableIndex, searchRenderCacheFromTail);
			if (nextRenderIndex >= 0)
			{
				dropRenderIndex   = nextRenderIndex;
				rebuildStartIndex = nextRenderableIndex;
			}
			else
			{
				if (previousRuntimeIndex >= 0)
				{
					for (int renderIndex = sizeToInt(m_nativeRenderLineCache.size()) - 1; renderIndex >= 0;
					     --renderIndex)
					{
						const int maxRuntimeIndex = renderLineMaxRuntimeIndexBefore(
						    m_nativeRenderLineCache.at(renderIndex), nextRenderableIndex);
						if (maxRuntimeIndex < 0)
							continue;
						dropRenderIndex   = renderIndex + 1;
						rebuildStartIndex = maxRuntimeIndex + 1;
						break;
					}
					if (dropRenderIndex < 0)
					{
						recordNativeRestitchFailureDiagnostic(
						    NativeRestitchFailureDiagnosticKind::RangeDropMiss, restitchStartIndex);
						return false;
					}
				}
				else
				{
					dropRenderIndex   = 0;
					rebuildStartIndex = nextRenderableIndex;
				}
			}
		}

		auto runtimeLogicalSourceLinesFrom = [&runtimeLines](const int startIndex, int &endExclusive)
		{
			QVector<qint64> sourceLines;
			endExclusive = startIndex;
			for (int index = startIndex; index < runtimeLines.size(); ++index)
			{
				const WorldRuntime::LineEntry &entry = runtimeLines.at(index);
				endExclusive                         = index + 1;
				if ((entry.flags & WorldRuntime::LineHidden) != 0)
					continue;
				sourceLines.push_back(entry.lineNumber);
				if (entry.hardReturn)
					break;
			}
			return sourceLines;
		};

		auto renderLineHasSourceLines =
		    [](const NativeOutputRenderLine &line, const QVector<qint64> &sourceLines)
		{
			if (!line.sourceRuntimeLineNumbers.isEmpty())
				return line.sourceRuntimeLineNumbers == sourceLines;
			if (sourceLines.isEmpty())
				return false;
			return line.firstRuntimeLineNumber == sourceLines.constFirst() &&
			       line.lastRuntimeLineNumber == sourceLines.constLast();
		};

		int  suffixRenderIndex  = -1;
		int  rebuildEndIndex    = -1;
		auto findReusableSuffix = [&]() -> bool
		{
			int       runtimeProbeIndex = rebuildStartIndex;
			int       skippedEndIndex   = rebuildStartIndex;
			const int maxProbeIndex     = qMin(sizeToInt(runtimeLines.size()), rebuildStartIndex + 256);
			for (;;)
			{
				const int candidateStartIndex = nextRenderableRuntimeIndex(runtimeProbeIndex);
				if (candidateStartIndex < 0 || candidateStartIndex >= maxProbeIndex)
					return false;

				int                   candidateEndIndex = candidateStartIndex + 1;
				const QVector<qint64> candidateSourceLines =
				    runtimeLogicalSourceLinesFrom(candidateStartIndex, candidateEndIndex);
				if (candidateSourceLines.isEmpty())
				{
					runtimeProbeIndex = qMax(candidateEndIndex, candidateStartIndex + 1);
					continue;
				}

				if (candidateStartIndex == rebuildStartIndex)
				{
					skippedEndIndex   = candidateEndIndex;
					runtimeProbeIndex = qMax(candidateEndIndex, candidateStartIndex + 1);
					continue;
				}

				for (int renderIndex = dropRenderIndex; renderIndex < m_nativeRenderLineCache.size();
				     ++renderIndex)
				{
					if (!renderLineHasSourceLines(m_nativeRenderLineCache.at(renderIndex),
					                              candidateSourceLines))
					{
						continue;
					}
					suffixRenderIndex = renderIndex;
					rebuildEndIndex   = candidateStartIndex;
					return rebuildEndIndex >= skippedEndIndex && suffixRenderIndex >= dropRenderIndex;
				}
				runtimeProbeIndex = qMax(candidateEndIndex, candidateStartIndex + 1);
			}
		};
		const bool preserveSuffix = findReusableSuffix();

#ifndef NDEBUG
		const int droppedNativeCount =
		    (preserveSuffix ? suffixRenderIndex : sizeToInt(m_nativeRenderLineCache.size())) -
		    dropRenderIndex;
		const int rebuiltRuntimeCount =
		    (preserveSuffix ? rebuildEndIndex : sizeToInt(runtimeLines.size())) - rebuildStartIndex;
		++m_nativeRangeRestitchDiagCount;
		m_nativeRangeRestitchDiagMinStart = m_nativeRangeRestitchDiagMinStart < 0
		                                        ? rebuildStartIndex
		                                        : qMin(m_nativeRangeRestitchDiagMinStart, rebuildStartIndex);
		m_nativeRangeRestitchDiagRebuiltTotal += rebuiltRuntimeCount;
		m_nativeRangeRestitchDiagRebuiltMax = qMax(m_nativeRangeRestitchDiagRebuiltMax, rebuiltRuntimeCount);
		m_nativeRangeRestitchDiagDroppedTotal += droppedNativeCount;
		m_nativeRangeRestitchDiagDroppedMax = qMax(m_nativeRangeRestitchDiagDroppedMax, droppedNativeCount);
#endif

		QVector<NativeOutputRenderLine> preservedSuffix;
		if (preserveSuffix && suffixRenderIndex < m_nativeRenderLineCache.size())
		{
			preservedSuffix.reserve(m_nativeRenderLineCache.size() - suffixRenderIndex);
			for (int renderIndex = suffixRenderIndex; renderIndex < m_nativeRenderLineCache.size();
			     ++renderIndex)
			{
				preservedSuffix.push_back(std::move(m_nativeRenderLineCache[renderIndex]));
			}
		}
		const int removeEndIndex =
		    preserveSuffix ? suffixRenderIndex : sizeToInt(m_nativeRenderLineCache.size());
		if (dropRenderIndex < removeEndIndex)
			m_nativeRenderLineCache.remove(dropRenderIndex, removeEndIndex - dropRenderIndex);
		else if (!preserveSuffix && dropRenderIndex < m_nativeRenderLineCache.size())
			m_nativeRenderLineCache.remove(dropRenderIndex, m_nativeRenderLineCache.size() - dropRenderIndex);
		if (const int cachePreviousRuntimeIndex = previousRenderableRuntimeIndex(rebuildStartIndex);
		    cachePreviousRuntimeIndex >= 0)
		{
			const WorldRuntime::LineEntry &previousEntry = runtimeLines.at(cachePreviousRuntimeIndex);
			m_nativeCachedRuntimeLastHardReturn          = previousEntry.hardReturn;
			m_nativeCachedRuntimeLastEntry               = previousEntry;
		}
		else
		{
			m_nativeCachedRuntimeLastHardReturn = true;
			m_nativeCachedRuntimeLastEntry      = {};
		}

		bool appendChanged     = false;
		bool appendTailMutated = false;
		appendRuntimeRange(rebuildStartIndex, true, &appendChanged, &appendTailMutated, nullptr,
		                   preserveSuffix ? rebuildEndIndex : -1, NativeAppendDiagnosticKind::RangeRestitch);
		if (!preservedSuffix.isEmpty())
		{
			m_nativeRenderLineCache.reserve(m_nativeRenderLineCache.size() + preservedSuffix.size());
			for (NativeOutputRenderLine &line : preservedSuffix)
				m_nativeRenderLineCache.push_back(std::move(line));
		}
		if (!appendChanged)
		{
			if (dropRenderIndex >= oldLineCount && preservedSuffix.isEmpty())
			{
				runtimeRangeRestitchApplied = true;
				return true;
			}
			bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::RuntimeRangeRestitch, oldLineCount,
			                                  true, trimmedHeadCount, dropRenderIndex);
			runtimeRangeRestitchApplied = true;
			return true;
		}

		bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::RuntimeRangeRestitch, oldLineCount,
		                                  true, trimmedHeadCount, dropRenderIndex);
		runtimeRangeRestitchApplied = true;
		return true;
	};

	auto restitchPendingRuntimeLine = [&]() -> bool
	{
		const int requestedIndex = m_nativeRuntimeLineRestitchIndex;
		if (requestedIndex < 0)
			return true;
		m_nativeRuntimeLineRestitchIndex = -1;

		if (!m_nativeRenderLineCacheValid || !m_nativeRenderLineCacheFromRuntime ||
		    m_nativeRenderLineCache.isEmpty())
		{
			return true;
		}

		const int runtimeIndex = qBound(0, requestedIndex, sizeToInt(runtimeLines.size()) - 1);
		if (runtimeIndex < 0 || runtimeIndex >= runtimeLines.size())
			return true;

		const WorldRuntime::LineEntry &entry = runtimeLines.at(runtimeIndex);
		if ((entry.flags & WorldRuntime::LineHidden) != 0 || !entry.hardReturn)
		{
			recordNativeRestitchFailureDiagnostic(NativeRestitchFailureDiagnosticKind::LineHiddenOrOpen,
			                                      runtimeIndex);
			promoteRuntimeLineRestitchToRange(runtimeIndex);
			return false;
		}

		const qint64 runtimeLineNumber = entry.lineNumber;
		const int    renderIndex       = nativeRenderIndexForRuntimeLineNumber(runtimeLineNumber, false);
		if (renderIndex < 0)
		{
			recordNativeRestitchFailureDiagnostic(NativeRestitchFailureDiagnosticKind::LineRenderMiss,
			                                      runtimeIndex);
			promoteRuntimeLineRestitchToRange(runtimeIndex);
			return false;
		}

		const QVector<qint64> sourceRuntimeLines =
		    nativeRuntimeLineNumbers(m_nativeRenderLineCache.at(renderIndex));
		if (sourceRuntimeLines.size() != 1 || sourceRuntimeLines.constFirst() != runtimeLineNumber)
		{
			recordNativeRestitchFailureDiagnostic(NativeRestitchFailureDiagnosticKind::LineComposite,
			                                      runtimeIndex);
			promoteRuntimeLineRestitchToRange(runtimeIndex);
			return false;
		}

		QDateTime previousLineTime;
		if (runtimeIndex > 0)
			previousLineTime = runtimeLines.at(runtimeIndex - 1).time;

		QString                          displayText(entry.text);
		QVector<WorldRuntime::StyleSpan> displaySpans(entry.spans);
		buildDisplayLine(entry, previousLineTime, displayText, displaySpans);

		removeNonPositiveStyleSpans(displaySpans);

		NativeOutputRenderLine renderLine{
		    std::move(displayText),
		    std::move(displaySpans),
		    lineOpacityForTimestamp(entry.time),
		    runtimeLineNumber,
		    runtimeLineNumber,
		    entry.flags,
		    0,
		    {},
		};
		renderLine.visualHash = nativeLineContentHash(renderLine);
		renderLine.sourceRuntimeLineKey =
		    nativeRuntimeLineKey(renderLine.sourceRuntimeLineNumbers, renderLine.firstRuntimeLineNumber,
		                         renderLine.lastRuntimeLineNumber);

		const int oldLineCount               = sizeToInt(m_nativeRenderLineCache.size());
		m_nativeRenderLineCache[renderIndex] = std::move(renderLine);
		if (runtimeIndex == 0)
			m_nativeCachedRuntimeFirstEntry = entry;
		if (runtimeIndex == runtimeLines.size() - 1)
		{
			m_nativeCachedRuntimeLastHardReturn = entry.hardReturn;
			m_nativeCachedRuntimeLastEntry      = entry;
		}
		++m_nativeRenderCacheIncrementalUpdates;
		bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::RuntimeLineRestitch, oldLineCount,
		                                  renderIndex == oldLineCount - 1, 0, renderIndex);
		return true;
	};

	if (!restitchPendingRuntimeRange())
	{
		incrementRebuildReason(RuntimeRebuildReason::RestitchFailure);
		softRebuildFromRuntime(runtimeNowContiguous, finalizeRuntimeCacheMetadata,
		                       NativeAppendDiagnosticKind::SoftRestitchFailure);
		return finalizeNativeOutputRenderLines();
	}
	if (runtimeRangeRestitchApplied)
	{
		finalizeRuntimeCacheMetadata(runtimeNowContiguous);
		return finalizeNativeOutputRenderLines();
	}

	if (!restitchPendingRuntimeLine())
	{
		if (!restitchPendingRuntimeRange())
		{
			incrementRebuildReason(RuntimeRebuildReason::RestitchFailure);
			softRebuildFromRuntime(runtimeNowContiguous, finalizeRuntimeCacheMetadata,
			                       NativeAppendDiagnosticKind::SoftRestitchFailure);
			return finalizeNativeOutputRenderLines();
		}
		if (runtimeRangeRestitchApplied)
		{
			finalizeRuntimeCacheMetadata(runtimeNowContiguous);
			return finalizeNativeOutputRenderLines();
		}
	}

	if (!restitchPendingRuntimeTail())
	{
		if (!restitchPendingRuntimeRange())
		{
			incrementRebuildReason(RuntimeRebuildReason::RestitchFailure);
			softRebuildFromRuntime(runtimeNowContiguous, finalizeRuntimeCacheMetadata,
			                       NativeAppendDiagnosticKind::SoftRestitchFailure);
			return finalizeNativeOutputRenderLines();
		}
		if (runtimeRangeRestitchApplied)
		{
			finalizeRuntimeCacheMetadata(runtimeNowContiguous);
			return finalizeNativeOutputRenderLines();
		}
	}

	auto processNonContiguousCache = [&]()
	{
		const bool sameCount = runtimeLines.size() == m_nativeCachedRuntimeCount;
		if (sameCount &&
		    lineEntriesEquivalentForCache(runtimeLines.first(), m_nativeCachedRuntimeFirstEntry) &&
		    lineEntriesEquivalentForCache(runtimeLines.last(), m_nativeCachedRuntimeLastEntry))
		{
			if (m_nativeCachedRuntimeLineNumbersContiguous != runtimeNowContiguous)
				m_nativeCachedRuntimeLineNumbersContiguous = runtimeNowContiguous;
			return;
		}

		const bool forwardProgress =
		    newFirstLineNumber >= oldFirstLineNumber && newLastLineNumber >= oldLastLineNumber;
		const bool structuralChange = newFirstLineNumber > oldFirstLineNumber ||
		                              newLastLineNumber > oldLastLineNumber ||
		                              runtimeLines.size() != m_nativeCachedRuntimeCount;
		if (m_nativeCachedRuntimeCount > 0 && forwardProgress && structuralChange)
		{
			int overlapLastIndex = -1;
			for (qsizetype i = runtimeLines.size(); i > 0; --i)
			{
				const qsizetype                index       = i - 1;
				const WorldRuntime::LineEntry &runtimeLine = runtimeLines.at(index);
				if (runtimeLine.lineNumber != m_nativeCachedRuntimeLastEntry.lineNumber ||
				    !lineEntriesEquivalentForCache(runtimeLine, m_nativeCachedRuntimeLastEntry))
				{
					continue;
				}
				overlapLastIndex = sizeToInt(index);
				break;
			}

			if (overlapLastIndex >= 0)
			{
				const int oldLineCountBeforeHeadTrim = sizeToInt(m_nativeRenderLineCache.size());
				bool      cacheTrimmed               = false;
				int       trimmedHeadCount           = 0;
				if (newFirstLineNumber > oldFirstLineNumber)
				{
					int dropCount = 0;
					while (dropCount < m_nativeRenderLineCache.size() &&
					       renderLineBeforeRuntimeLine(m_nativeRenderLineCache.at(dropCount),
					                                   newFirstLineNumber))
					{
						++dropCount;
					}
					if (dropCount > 0)
					{
						m_nativeRenderLineCache.remove(0, dropCount);
						++m_nativeRenderCacheTrimDrops;
						cacheTrimmed     = true;
						trimmedHeadCount = dropCount;
					}
					if (!m_nativeRenderLineCache.isEmpty() &&
					    renderLineStraddlesRuntimeLine(m_nativeRenderLineCache.first(), newFirstLineNumber))
					{
						if (!restitchTrimmedHeadLogicalLine(newFirstLineNumber))
						{
							recordNativeRestitchFailureDiagnostic(
							    NativeRestitchFailureDiagnosticKind::HeadTrim, qMax(0, overlapLastIndex));
							incrementRebuildReason(RuntimeRebuildReason::RestitchFailure);
							softRebuildFromRuntime(runtimeNowContiguous, finalizeRuntimeCacheMetadata,
							                       NativeAppendDiagnosticKind::SoftRestitchFailure);
							return;
						}
					}
				}

				const int appendStartIndex = overlapLastIndex + 1;
				if (appendStartIndex < runtimeLines.size())
				{
					if (cacheTrimmed)
					{
						bool appendChanged     = false;
						bool appendTailMutated = false;
						appendRuntimeRange(appendStartIndex, true, &appendChanged, &appendTailMutated,
						                   nullptr, -1, NativeAppendDiagnosticKind::HeadTrimTailAppend);
						if (appendChanged)
						{
							bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::HeadTrimTailAppend,
							                                  oldLineCountBeforeHeadTrim, appendTailMutated,
							                                  trimmedHeadCount);
						}
						else
						{
							bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::HeadMutation,
							                                  oldLineCountBeforeHeadTrim, false,
							                                  trimmedHeadCount);
						}
					}
					else
					{
						appendRuntimeRange(appendStartIndex, false, nullptr, nullptr, nullptr, -1,
						                   NativeAppendDiagnosticKind::NonContiguousTailAppend);
					}
				}
				else if (cacheTrimmed)
				{
					bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::HeadMutation,
					                                  oldLineCountBeforeHeadTrim, false, trimmedHeadCount);
				}

				finalizeRuntimeCacheMetadata(runtimeNowContiguous);
				return;
			}

			incrementRebuildReason(RuntimeRebuildReason::NonContiguousNoOverlap);
			softRebuildFromRuntime(runtimeNowContiguous, finalizeRuntimeCacheMetadata,
			                       NativeAppendDiagnosticKind::SoftNonContiguousNoOverlap);
			return;
		}

		incrementRebuildReason(RuntimeRebuildReason::RuntimeDisjoint);
		softRebuildFromRuntime(runtimeNowContiguous, finalizeRuntimeCacheMetadata,
		                       NativeAppendDiagnosticKind::SoftRuntimeDisjoint);
	};

	if (!m_nativeCachedRuntimeLineNumbersContiguous)
	{
		processNonContiguousCache();
		return finalizeNativeOutputRenderLines();
	}

	if (newFirstLineNumber == oldFirstLineNumber && newLastLineNumber == oldLastLineNumber &&
	    runtimeLines.size() == m_nativeCachedRuntimeCount)
	{
		if (runtimeLines.last().hardReturn != m_nativeCachedRuntimeLastHardReturn)
		{
			// Last-line hard-return flips do not change rendered text; keep the cache
			// and only refresh tail metadata used by partial overlay stitching.
			m_nativeCachedRuntimeLastHardReturn = runtimeLines.last().hardReturn;
			m_nativeCachedRuntimeLastEntry      = runtimeLines.last();
		}
		return finalizeNativeOutputRenderLines();
	}

	if (newFirstLineNumber < oldFirstLineNumber || newFirstLineNumber > oldLastLineNumber + 1 ||
	    newLastLineNumber < newFirstLineNumber)
	{
		incrementRebuildReason(RuntimeRebuildReason::RuntimeDisjoint);
		softRebuildFromRuntime(runtimeNowContiguous, finalizeRuntimeCacheMetadata,
		                       NativeAppendDiagnosticKind::SoftRuntimeDisjoint);
		return finalizeNativeOutputRenderLines();
	}

	if (!runtimeNowContiguous)
	{
		processNonContiguousCache();
		return finalizeNativeOutputRenderLines();
	}

	const int oldLineCountBeforeHeadTrim = sizeToInt(m_nativeRenderLineCache.size());
	bool      cacheTrimmed               = false;
	int       trimmedHeadCount           = 0;
	if (newFirstLineNumber > oldFirstLineNumber)
	{
		int dropCount = 0;
		while (dropCount < m_nativeRenderLineCache.size() &&
		       renderLineBeforeRuntimeLine(m_nativeRenderLineCache.at(dropCount), newFirstLineNumber))
		{
			++dropCount;
		}
		if (dropCount > 0)
		{
			m_nativeRenderLineCache.remove(0, dropCount);
			++m_nativeRenderCacheTrimDrops;
			cacheTrimmed     = true;
			trimmedHeadCount = dropCount;
		}
		if (!m_nativeRenderLineCache.isEmpty() &&
		    renderLineStraddlesRuntimeLine(m_nativeRenderLineCache.first(), newFirstLineNumber))
		{
			if (!restitchTrimmedHeadLogicalLine(newFirstLineNumber))
			{
				recordNativeRestitchFailureDiagnostic(NativeRestitchFailureDiagnosticKind::HeadTrim,
				                                      qMax(0, trimmedHeadCount));
				incrementRebuildReason(RuntimeRebuildReason::RestitchFailure);
				softRebuildFromRuntime(true, finalizeRuntimeCacheMetadata,
				                       NativeAppendDiagnosticKind::SoftRestitchFailure);
				return finalizeNativeOutputRenderLines();
			}
		}
	}

	if (newLastLineNumber > oldLastLineNumber)
	{
		const qint64 appendStartLineNumber = oldLastLineNumber + 1;
		int          appendStartIndex      = static_cast<int>(appendStartLineNumber - newFirstLineNumber);
		if (appendStartIndex < 0 || appendStartIndex >= runtimeLines.size())
		{
			appendStartIndex = findRuntimeLineIndexByNumberNear(runtimeLines, appendStartLineNumber,
			                                                    sizeToInt(runtimeLines.size()) - 1, true);
			if (appendStartIndex < 0 || appendStartIndex >= runtimeLines.size())
			{
				incrementRebuildReason(RuntimeRebuildReason::AppendStartOutOfRange);
				softRebuildFromRuntime(true, finalizeRuntimeCacheMetadata,
				                       NativeAppendDiagnosticKind::SoftAppendStartOutOfRange);
				return finalizeNativeOutputRenderLines();
			}
		}
		if (appendStartIndex > 0 && appendStartIndex - 1 < runtimeLines.size())
		{
			const WorldRuntime::LineEntry &runtimePreviousEntry = runtimeLines.at(appendStartIndex - 1);
			m_nativeCachedRuntimeLastHardReturn                 = runtimePreviousEntry.hardReturn;
			m_nativeCachedRuntimeLastEntry                      = runtimePreviousEntry;
		}
		if (cacheTrimmed)
		{
			bool appendChanged     = false;
			bool appendTailMutated = false;
			appendRuntimeRange(appendStartIndex, true, &appendChanged, &appendTailMutated, nullptr, -1,
			                   NativeAppendDiagnosticKind::HeadTrimTailAppend);
			if (appendChanged)
			{
				bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::HeadTrimTailAppend,
				                                  oldLineCountBeforeHeadTrim, appendTailMutated,
				                                  trimmedHeadCount);
			}
			else
			{
				bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::HeadMutation,
				                                  oldLineCountBeforeHeadTrim, false, trimmedHeadCount);
			}
		}
		else
		{
			appendRuntimeRange(appendStartIndex, false, nullptr, nullptr, nullptr, -1,
			                   NativeAppendDiagnosticKind::TailAppend);
		}
	}
	else if (cacheTrimmed)
	{
		bumpNativeRenderLineCacheRevision(NativeRenderCacheDeltaKind::HeadMutation,
		                                  oldLineCountBeforeHeadTrim, false, trimmedHeadCount);
	}

	finalizeRuntimeCacheMetadata(true);
	return finalizeNativeOutputRenderLines();
}

bool WorldView::nativeOutputInteractionActive() const
{
	if (m_destroying)
		return false;
	if (!m_nativeOutputCanvas || !m_nativeOutputCanvas->isVisible() || !isVisible())
		return false;
	const bool outputVisible = m_output && m_output->viewport() && m_output->viewport()->isVisible();
	const bool liveVisible =
	    m_liveOutput && m_liveOutput->viewport() && m_liveOutput->viewport()->isVisible();
	return outputVisible || liveVisible;
}

bool WorldView::nativeOutputHitTest(const WrapTextBrowser *view, const QPoint &viewPos,
                                    NativeOutputPosition &position, QString *href, QString *hint,
                                    const bool allowCacheBuild, const bool requireTextHit,
                                    bool *const textHit) const
{
	position = {};
	if (href)
		href->clear();
	if (hint)
		hint->clear();
	if (textHit)
		*textHit = false;
	if (!nativeOutputInteractionActive() || !view || !view->viewport())
		return false;

	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	if (lines.isEmpty())
		return false;

	const QRect viewportRect = view->viewport()->rect();
	if (viewportRect.width() <= 0 || viewportRect.height() <= 0)
		return false;

	const int    x = qBound(0, viewPos.x(), viewportRect.width() - 1);
	const int    y = qBound(0, viewPos.y(), viewportRect.height() - 1);

	const bool   wrapEnabled          = view->lineWrapMode() != WrapTextBrowser::NoWrap;
	const int    wrapWidthPixels      = nativeWrapWidthPixels(viewportRect.width(), wrapEnabled);
	const int    localWrapWidthPixels = nativeLocalWrapWidthPixels(viewportRect.width(), wrapEnabled);
	const int    lineSpacing          = qMax(0, m_lineSpacing);
	const qreal  lineAdvance = static_cast<qreal>(qMax(1, QFontMetrics(view->font()).lineSpacing())) *
	                           ((100.0 + static_cast<qreal>(lineSpacing)) / 100.0);
	const QFont &layoutFont  = view->font();
	const bool   cacheReadyForHitTest =
	    nativeLayoutCacheReadyFor(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacing, layoutFont);
	if (!allowCacheBuild && !cacheReadyForHitTest)
		return false;
	if (!cacheReadyForHitTest)
		ensureNativeLayoutCaches(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacing, layoutFont);
	const qreal effectiveLineAdvance =
	    m_nativeLayoutCachedLineAdvance > 0.0 ? m_nativeLayoutCachedLineAdvance : lineAdvance;
	qreal     docY = m_nativeLayoutCumulativeHeights.size() > lines.size()
	                     ? m_nativeLayoutCumulativeHeights.at(lines.size())
	                     : 0.0;

	const int nativeMaxScroll = qMax(0, static_cast<int>(std::ceil(docY)) - viewportRect.height());
	int       scrollY         = 0;
	int       scrollMax       = 0;
	if (const QScrollBar *const bar = view->verticalScrollBar())
	{
		scrollY   = qMax(0, bar->value());
		scrollMax = qMax(0, bar->maximum());
	}
	int effectiveScrollY = 0;
	if (nativeMaxScroll > 0)
	{
		if (scrollMax > 0)
		{
			const double ratio = static_cast<double>(scrollY) / static_cast<double>(scrollMax);
			effectiveScrollY =
			    qBound(0, static_cast<int>(std::round(ratio * nativeMaxScroll)), nativeMaxScroll);
		}
		else
			effectiveScrollY = qBound(0, scrollY, nativeMaxScroll);
	}

	const qreal yDoc       = static_cast<qreal>(effectiveScrollY) + static_cast<qreal>(y);
	int         lineIndex  = sizeToInt(lines.size()) - 1;
	qreal       lineY      = 0.0;
	qreal       lineYEnd   = docY;
	const auto  firstAbove = std::upper_bound(m_nativeLayoutCumulativeHeights.cbegin() + 1,
	                                          m_nativeLayoutCumulativeHeights.cend(), yDoc);
	lineIndex = qBound(0, static_cast<int>(firstAbove - m_nativeLayoutCumulativeHeights.cbegin()) - 1,
	                   sizeToInt(lines.size()) - 1);
	lineY     = m_nativeLayoutCumulativeHeights.at(lineIndex);
	lineYEnd  = m_nativeLayoutCumulativeHeights.at(lineIndex + 1);
	if (allowCacheBuild &&
	    ensureNativeLayoutRange(lines, qMax(0, lineIndex - 2),
	                            qMin(sizeToInt(lines.size()) - 1, lineIndex + 2), wrapWidthPixels,
	                            localWrapWidthPixels, lineSpacing, layoutFont))
	{
		docY                             = m_nativeLayoutCumulativeHeights.size() > lines.size()
		                                       ? m_nativeLayoutCumulativeHeights.at(lines.size())
		                                       : 0.0;
		const int updatedNativeMaxScroll = qMax(0, static_cast<int>(std::ceil(docY)) - viewportRect.height());
		if (updatedNativeMaxScroll > 0)
		{
			if (scrollMax > 0)
			{
				const double ratio = static_cast<double>(scrollY) / static_cast<double>(scrollMax);
				effectiveScrollY   = qBound(0, static_cast<int>(std::round(ratio * updatedNativeMaxScroll)),
				                            updatedNativeMaxScroll);
			}
			else
				effectiveScrollY = qBound(0, scrollY, updatedNativeMaxScroll);
		}
		const qreal updatedYDoc       = static_cast<qreal>(effectiveScrollY) + static_cast<qreal>(y);
		const auto  updatedFirstAbove = std::upper_bound(m_nativeLayoutCumulativeHeights.cbegin() + 1,
		                                                 m_nativeLayoutCumulativeHeights.cend(), updatedYDoc);
		lineIndex =
		    qBound(0, static_cast<int>(updatedFirstAbove - m_nativeLayoutCumulativeHeights.cbegin()) - 1,
		           sizeToInt(lines.size()) - 1);
		lineY    = m_nativeLayoutCumulativeHeights.at(lineIndex);
		lineYEnd = m_nativeLayoutCumulativeHeights.at(lineIndex + 1);
	}

	const NativeOutputRenderLine &line = lines.at(lineIndex);
	if (line.text.isEmpty())
	{
		if (requireTextHit)
			return false;
		position = {lineIndex, 0};
		return true;
	}

	const QTextLayout *layout = nativeLayoutForLine(lineIndex);
	if (!layout || layout->lineCount() <= 0)
	{
		if (requireTextHit)
			return false;
		position = {lineIndex, 0};
		return true;
	}

	const qreal insideY  = qBound<qreal>(0.0, yDoc - lineY, qMax<qreal>(0.0, lineYEnd - lineY));
	const int   rowCount = qMax(1, layout->lineCount());

	QTextLine   targetRow = layout->lineAt(0);
	if (!targetRow.isValid())
	{
		if (requireTextHit)
			return false;
		position = {lineIndex, 0};
		return true;
	}
	for (int i = 0; i < rowCount; ++i)
	{
		const QTextLine row       = layout->lineAt(i);
		const qreal     rowTop    = row.y();
		const qreal     rowBottom = rowTop + effectiveLineAdvance;
		if (insideY < rowBottom || i == rowCount - 1)
		{
			targetRow = row;
			break;
		}
	}

	const int   rowStartColumn  = targetRow.textStart();
	const int   rowEndColumn    = rowStartColumn + targetRow.textLength();
	const qreal rowLeft         = targetRow.cursorToX(rowStartColumn);
	const qreal rowRight        = targetRow.cursorToX(rowEndColumn);
	const qreal minX            = qMin(rowLeft, rowRight);
	const qreal maxX            = qMax(rowLeft, rowRight);
	const bool  insideTextRow   = static_cast<qreal>(x) >= minX && static_cast<qreal>(x) < maxX;
	const bool  resolvedTextHit = insideTextRow && rowEndColumn > rowStartColumn;
	if (textHit)
		*textHit = resolvedTextHit;
	if (requireTextHit && !resolvedTextHit)
		return false;

	int column = targetRow.xToCursor(static_cast<qreal>(x), QTextLine::CursorBetweenCharacters);
	column     = qBound(rowStartColumn, column, rowEndColumn);
	position   = {lineIndex, column};

	if (href || hint)
	{
		if (!resolvedTextHit)
			return true;

		const int textSize = sizeToInt(line.text.size());
		int       probe    = column;
		if (probe >= rowEndColumn)
			probe = rowEndColumn - 1;
		if (textSize > 0 && probe >= textSize)
			probe = textSize - 1;

		int offset = 0;
		for (const WorldRuntime::StyleSpan &span : line.spans)
		{
			const int spanLength = qMax(0, span.length);
			if (spanLength <= 0)
				continue;
			const int spanStart = offset;
			const int spanEnd   = offset + spanLength;
			offset              = spanEnd;
			if (probe < spanStart || probe >= spanEnd)
				continue;
			if (!isActionLinkType(span.actionType) || span.action.isEmpty())
				break;
			if (href)
				*href = span.action.trimmed();
			if (hint)
				*hint = span.hint.trimmed();
			break;
		}
	}

	return true;
}

bool WorldView::nativeOutputCharacterRect(const WrapTextBrowser *view, const NativeOutputPosition &position,
                                          QRect &globalRect) const
{
	globalRect = {};
	if (!nativeOutputInteractionActive() || !view || !view->viewport())
		return false;

	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	if (lines.isEmpty())
		return false;

	const QRect viewportRect = view->viewport()->rect();
	if (viewportRect.width() <= 0 || viewportRect.height() <= 0)
		return false;

	const int    lineIndex = qBound(0, position.line, sizeToInt(lines.size()) - 1);

	const bool   wrapEnabled          = view->lineWrapMode() != WrapTextBrowser::NoWrap;
	const int    wrapWidthPixels      = nativeWrapWidthPixels(viewportRect.width(), wrapEnabled);
	const int    localWrapWidthPixels = nativeLocalWrapWidthPixels(viewportRect.width(), wrapEnabled);
	const int    lineSpacing          = qMax(0, m_lineSpacing);
	const QFont &layoutFont           = view->font();
	const qreal  fallbackLineAdvance  = static_cast<qreal>(qMax(1, QFontMetrics(layoutFont).lineSpacing())) *
	                                    ((100.0 + static_cast<qreal>(lineSpacing)) / 100.0);

	if (!nativeLayoutCacheReadyFor(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacing, layoutFont))
		ensureNativeLayoutCaches(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacing, layoutFont);
	if (ensureNativeLayoutRange(lines, qMax(0, lineIndex - 2),
	                            qMin(sizeToInt(lines.size()) - 1, lineIndex + 2), wrapWidthPixels,
	                            localWrapWidthPixels, lineSpacing, layoutFont))
	{
		syncNativeOutputScrollBarsFromLayout(lines, false);
	}

	if (lineIndex + 1 >= m_nativeLayoutCumulativeHeights.size())
		return false;

	const qreal effectiveLineAdvance =
	    m_nativeLayoutCachedLineAdvance > 0.0 ? m_nativeLayoutCachedLineAdvance : fallbackLineAdvance;
	const qreal docY            = m_nativeLayoutCumulativeHeights.size() > lines.size()
	                                  ? m_nativeLayoutCumulativeHeights.at(lines.size())
	                                  : 0.0;
	const int   nativeMaxScroll = qMax(0, static_cast<int>(std::ceil(docY)) - viewportRect.height());
	int         scrollY         = 0;
	int         scrollMax       = 0;
	if (const QScrollBar *const bar = view->verticalScrollBar())
	{
		scrollY   = qMax(0, bar->value());
		scrollMax = qMax(0, bar->maximum());
	}

	int effectiveScrollY = 0;
	if (nativeMaxScroll > 0)
	{
		if (scrollMax > 0)
		{
			const double ratio = static_cast<double>(scrollY) / static_cast<double>(scrollMax);
			effectiveScrollY =
			    qBound(0, static_cast<int>(std::round(ratio * nativeMaxScroll)), nativeMaxScroll);
		}
		else
			effectiveScrollY = qBound(0, scrollY, nativeMaxScroll);
	}

	const qreal                   lineTop    = m_nativeLayoutCumulativeHeights.at(lineIndex);
	const qreal                   lineBottom = m_nativeLayoutCumulativeHeights.at(lineIndex + 1);
	const qreal                   lineHeight = qMax<qreal>(1.0, lineBottom - lineTop);
	const NativeOutputRenderLine &line       = lines.at(lineIndex);

	const QTextLayout            *layout = nativeLayoutForLine(lineIndex);
	if (line.text.isEmpty() || !layout || layout->lineCount() <= 0)
	{
		const QRect localRect(0, static_cast<int>(std::floor(lineTop - effectiveScrollY)), 1,
		                      qMax(1, static_cast<int>(std::ceil(lineHeight))));
		const QRect visibleRect = localRect.intersected(viewportRect);
		if (visibleRect.isEmpty())
			return false;
		globalRect = QRect(view->viewport()->mapToGlobal(visibleRect.topLeft()), visibleRect.size());
		return true;
	}

	const int textSize = sizeToInt(line.text.size());
	const int column   = qBound(0, position.column, textSize);

	QTextLine targetRow;
	for (int i = 0; i < layout->lineCount(); ++i)
	{
		const QTextLine row      = layout->lineAt(i);
		const int       rowStart = row.textStart();
		const int       rowEnd   = rowStart + row.textLength();
		if (column < rowEnd || i == layout->lineCount() - 1)
		{
			targetRow = row;
			break;
		}
	}
	if (!targetRow.isValid())
		return false;

	const int   rowStartColumn = targetRow.textStart();
	const int   rowEndColumn   = rowStartColumn + targetRow.textLength();
	const int   leadingColumn  = qBound(rowStartColumn, column, rowEndColumn);
	const int   trailingColumn = qBound(rowStartColumn, leadingColumn + 1, rowEndColumn);
	const qreal leadingX       = targetRow.cursorToX(leadingColumn);
	const qreal trailingX =
	    trailingColumn > leadingColumn ? targetRow.cursorToX(trailingColumn) : leadingX + 1.0;
	const qreal leftX  = qMin(leadingX, trailingX);
	const qreal rightX = qMax(leadingX, trailingX);
	const QRect localRect(static_cast<int>(std::floor(leftX)),
	                      static_cast<int>(std::floor(lineTop - effectiveScrollY + targetRow.y())),
	                      qMax(1, static_cast<int>(std::ceil(rightX - leftX))),
	                      qMax(1, static_cast<int>(std::ceil(effectiveLineAdvance))));
	const QRect visibleRect = localRect.intersected(viewportRect);
	if (visibleRect.isEmpty())
		return false;

	globalRect = QRect(view->viewport()->mapToGlobal(visibleRect.topLeft()), visibleRect.size());
	return true;
}

void WorldView::scrollNativeOutputRangeIntoView(const WrapTextBrowser *view, int firstLine,
                                                int lastLine) const
{
	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	if (lines.isEmpty() || !view || !view->viewport())
		return;

	QScrollBar *const bar = view->verticalScrollBar();
	if (!bar)
		return;

	const QRect viewportRect = view->viewport()->rect();
	if (viewportRect.isEmpty())
		return;

	firstLine = qBound(0, firstLine, sizeToInt(lines.size()) - 1);
	lastLine  = qBound(0, lastLine, sizeToInt(lines.size()) - 1);
	if (firstLine > lastLine)
		std::swap(firstLine, lastLine);

	const bool   wrapEnabled          = view->lineWrapMode() != WrapTextBrowser::NoWrap;
	const int    wrapWidthPixels      = nativeWrapWidthPixels(viewportRect.width(), wrapEnabled);
	const int    localWrapWidthPixels = nativeLocalWrapWidthPixels(viewportRect.width(), wrapEnabled);
	const int    lineSpacingSetting   = qMax(0, m_lineSpacing);
	const QFont &layoutFont           = view->font();
	ensureNativeLayoutCaches(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacingSetting, layoutFont);
	const bool layoutHeightChanged = ensureNativeLayoutRange(
	    lines, firstLine, lastLine, wrapWidthPixels, localWrapWidthPixels, lineSpacingSetting, layoutFont);
	if (layoutHeightChanged)
		syncNativeOutputScrollBarsFromLayout(lines, false);

	if (lastLine + 1 >= m_nativeLayoutCumulativeHeights.size())
		return;

	const int rangeTop = qMax(0, static_cast<int>(std::floor(m_nativeLayoutCumulativeHeights.at(firstLine))));
	const int rangeBottom =
	    qMax(rangeTop + 1, static_cast<int>(std::ceil(m_nativeLayoutCumulativeHeights.at(lastLine + 1))));

	const int currentTop    = bar->value();
	const int pageStep      = qMax(1, bar->pageStep());
	const int currentBottom = currentTop + pageStep;
	int       target        = currentTop;
	if (rangeTop < currentTop)
		target = rangeTop;
	else if (rangeBottom > currentBottom)
		target = rangeBottom - pageStep;

	target = qBound(bar->minimum(), target, bar->maximum());
	if (view == m_output && target < bar->maximum())
		const_cast<WorldView *>(this)->setScrollbackSplitActive(true);
	if (target == bar->value())
		return;
	if (view == m_output && m_outputScrollBar)
		m_outputScrollBar->setValue(target);
	else
		bar->setValue(target);
}

bool WorldView::nativeOutputHitTestGlobal(const QPoint &globalPos, WrapTextBrowser *&view,
                                          NativeOutputPosition &position, QString *href, QString *hint,
                                          const bool allowCacheBuild, const bool requireTextHit,
                                          bool *const textHit) const
{
	view     = nullptr;
	position = {};
	if (textHit)
		*textHit = false;
	if (!nativeOutputInteractionActive())
		return false;

	auto tryView = [this, &globalPos, &view, &position, href, hint, allowCacheBuild, requireTextHit,
	                textHit](WrapTextBrowser *candidate)
	{
		if (!candidate || !candidate->isVisible() || !candidate->viewport())
			return false;
		const QPoint local = candidate->viewport()->mapFromGlobal(globalPos);
		if (!candidate->viewport()->rect().contains(local))
			return false;
		if (!nativeOutputHitTest(candidate, local, position, href, hint, allowCacheBuild, requireTextHit,
		                         textHit))
		{
			return false;
		}
		view = candidate;
		return true;
	};

	if (tryView(m_liveOutput))
		return true;
	return tryView(m_output);
}

bool WorldView::nativeOutputHitTestForMouseEvent(const QWidget *watched, const QMouseEvent *event,
                                                 WrapTextBrowser *&view, QPoint &viewPos,
                                                 NativeOutputPosition &position, QString *href, QString *hint,
                                                 const bool allowCacheBuild, bool *const textHit) const
{
	view     = nullptr;
	viewPos  = {};
	position = {};
	if (href)
		href->clear();
	if (hint)
		hint->clear();
	if (textHit)
		*textHit = false;
	if (!watched || !event || !nativeOutputInteractionActive())
		return false;

	if (watched == m_output || watched == (m_output ? m_output->viewport() : nullptr))
		view = m_output;
	else if (watched == m_liveOutput || watched == (m_liveOutput ? m_liveOutput->viewport() : nullptr))
		view = m_liveOutput;
	if (!view || !view->viewport())
		return false;

	if (watched == view->viewport())
		viewPos = event->position().toPoint();
	else if (watched == view)
		viewPos = view->viewport()->mapFrom(view, event->position().toPoint());
	else
		viewPos = view->viewport()->mapFromGlobal(event->globalPosition().toPoint());

	if (!view->viewport()->rect().contains(viewPos))
		return false;
	return nativeOutputHitTest(view, viewPos, position, href, hint, allowCacheBuild, false, textHit);
}

void WorldView::applyResolvedOutputSelection(const bool hasSelection, const int startLine,
                                             const int startColumn, const int endLine, const int endColumn)
{
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

void WorldView::clearNativeOutputSelection(const bool notify)
{
	const bool  hadNativeState = m_nativeOutputSelection.hasSelection || m_nativeOutputSelection.dragging ||
	                             m_nativeOutputSelection.sourceView != nullptr;
	const QRect oldPaneRect    = nativeOutputPaneRect(m_nativeOutputSelection.sourceView);
	m_nativeOutputSelection    = {};
	m_nativeSelectionPendingHeadTrimLines = 0;
	requestNativeOutputRepaint(oldPaneRect);
	if (notify && hadNativeState)
		applyResolvedOutputSelection(false, 0, 0, 0, 0);
}

void WorldView::applyPendingNativeSelectionHeadTrim(const QVector<NativeOutputRenderLine> &lines)
{
	const int trimCount = m_nativeSelectionPendingHeadTrimLines;
	if (trimCount <= 0)
		return;
	m_nativeSelectionPendingHeadTrimLines = 0;

	if (!m_nativeOutputSelection.hasSelection && !m_nativeOutputSelection.dragging)
		return;
	if (lines.isEmpty())
	{
		clearNativeOutputSelection(true);
		return;
	}

	auto shiftLineIndex = [trimCount](NativeOutputPosition &position) { position.line -= trimCount; };
	shiftLineIndex(m_nativeOutputSelection.anchor);
	shiftLineIndex(m_nativeOutputSelection.cursor);
	shiftLineIndex(m_nativeOutputSelection.start);
	shiftLineIndex(m_nativeOutputSelection.end);

	if (m_nativeOutputSelection.anchor.line < 0 || m_nativeOutputSelection.cursor.line < 0 ||
	    m_nativeOutputSelection.start.line < 0 || m_nativeOutputSelection.end.line < 0)
	{
		clearNativeOutputSelection(true);
		return;
	}

	const int maxLine       = sizeToInt(lines.size()) - 1;
	auto      clampPosition = [&lines, maxLine](NativeOutputPosition &position)
	{
		position.line    = qBound(0, position.line, maxLine);
		const int maxCol = sizeToInt(lines.at(position.line).text.size());
		position.column  = qBound(0, position.column, maxCol);
	};
	clampPosition(m_nativeOutputSelection.anchor);
	clampPosition(m_nativeOutputSelection.cursor);
	clampPosition(m_nativeOutputSelection.start);
	clampPosition(m_nativeOutputSelection.end);

	NativeOutputPosition start = m_nativeOutputSelection.anchor;
	NativeOutputPosition end   = m_nativeOutputSelection.cursor;
	if (start.line > end.line || (start.line == end.line && start.column > end.column))
		qSwap(start, end);
	m_nativeOutputSelection.start        = start;
	m_nativeOutputSelection.end          = end;
	m_nativeOutputSelection.hasSelection = start.line != end.line || start.column != end.column;
	if (!m_nativeOutputSelection.hasSelection)
		clearNativeOutputSelection(true);
}

void WorldView::clearNativeSelectionIfOutsideVisibleViewport(const WrapTextBrowser *const view)
{
	if (!view || !m_nativeOutputSelection.hasSelection || m_nativeOutputSelection.sourceView != view)
		return;
	if (!view->viewport())
	{
		clearNativeOutputSelection(true);
		return;
	}
	if (view == m_liveOutput && (!m_scrollbackSplitActive || !m_liveOutput || !m_liveOutput->isVisible()))
	{
		clearNativeOutputSelection(true);
		return;
	}

	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	if (lines.isEmpty())
	{
		clearNativeOutputSelection(true);
		return;
	}

	applyPendingNativeSelectionHeadTrim(lines);
	if (!m_nativeOutputSelection.hasSelection)
		return;
}

void WorldView::setNativeOutputSelection(const WrapTextBrowser      *sourceView,
                                         const NativeOutputPosition &anchor,
                                         const NativeOutputPosition &cursor, const bool dragging)
{
	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	if (lines.isEmpty() || !sourceView)
	{
		clearNativeOutputSelection(true);
		return;
	}
	auto clampLineColumn = [&lines](int line, int column, int *clampedLine = nullptr)
	{
		const int boundedLine = qBound(0, line, sizeToInt(lines.size()) - 1);
		if (clampedLine)
			*clampedLine = boundedLine;
		const int maxColumn = sizeToInt(lines.at(boundedLine).text.size());
		return qBound(0, column, maxColumn);
	};

	m_nativeSelectionPendingHeadTrimLines = 0;
	int                  anchorLine       = anchor.line;
	int                  cursorLine       = cursor.line;
	int                  anchorCol        = clampLineColumn(anchorLine, anchor.column, &anchorLine);
	int                  cursorCol        = clampLineColumn(cursorLine, cursor.column, &cursorLine);

	NativeOutputPosition start{anchorLine, anchorCol};
	NativeOutputPosition end{cursorLine, cursorCol};
	if (start.line > end.line || (start.line == end.line && start.column > end.column))
		qSwap(start, end);

	const bool                   hasSelection       = start.line != end.line || start.column != end.column;
	const WrapTextBrowser *const previousSourceView = m_nativeOutputSelection.sourceView;
	const bool                   changed =
	    m_nativeOutputSelection.hasSelection != hasSelection ||
	    m_nativeOutputSelection.dragging != dragging || m_nativeOutputSelection.sourceView != sourceView ||
	    m_nativeOutputSelection.anchor.line != anchorLine ||
	    m_nativeOutputSelection.anchor.column != anchorCol ||
	    m_nativeOutputSelection.cursor.line != cursorLine ||
	    m_nativeOutputSelection.cursor.column != cursorCol ||
	    m_nativeOutputSelection.start.line != start.line ||
	    m_nativeOutputSelection.start.column != start.column ||
	    m_nativeOutputSelection.end.line != end.line || m_nativeOutputSelection.end.column != end.column;
	if (!changed)
		return;

	m_nativeOutputSelection.hasSelection = hasSelection;
	m_nativeOutputSelection.dragging     = dragging;
	m_nativeOutputSelection.sourceView   = const_cast<WrapTextBrowser *>(sourceView);
	m_nativeOutputSelection.anchor       = {anchorLine, anchorCol};
	m_nativeOutputSelection.cursor       = {cursorLine, cursorCol};
	m_nativeOutputSelection.start        = start;
	m_nativeOutputSelection.end          = end;
	if (hasSelection)
		m_lastOutputSelectionView = const_cast<WrapTextBrowser *>(sourceView);
	else if (m_lastOutputSelectionView == sourceView)
		m_lastOutputSelectionView = nullptr;

	QRect repaintRect = nativeOutputPaneRect(previousSourceView);
	repaintRect       = repaintRect.united(nativeOutputPaneRect(m_nativeOutputSelection.sourceView));
	requestNativeOutputRepaint(repaintRect);
	if (hasSelection)
		applyResolvedOutputSelection(true, start.line + 1, start.column + 1, end.line + 1, end.column + 1);
	else
		applyResolvedOutputSelection(false, 0, 0, 0, 0);
}

bool WorldView::nativeOutputSelectionBounds(int &startLine, int &startColumn, int &endLine,
                                            int &endColumn) const
{
	startLine   = 0;
	startColumn = 0;
	endLine     = 0;
	endColumn   = 0;
	if (!m_nativeOutputSelection.hasSelection)
		return false;
	if (m_nativeOutputSelection.sourceView == m_liveOutput &&
	    (!m_scrollbackSplitActive || !m_liveOutput || !m_liveOutput->isVisible()))
		return false;

	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	if (lines.isEmpty())
		return false;
	const_cast<WorldView *>(this)->applyPendingNativeSelectionHeadTrim(lines);
	if (!m_nativeOutputSelection.hasSelection)
		return false;

	auto clampLineColumn = [&lines](int line, int column, int *clampedLine = nullptr)
	{
		const int boundedLine = qBound(0, line, sizeToInt(lines.size()) - 1);
		if (clampedLine)
			*clampedLine = boundedLine;
		const int maxColumn = sizeToInt(lines.at(boundedLine).text.size());
		return qBound(0, column, maxColumn);
	};

	int startLineZero = m_nativeOutputSelection.start.line;
	int endLineZero   = m_nativeOutputSelection.end.line;
	int startColZero  = clampLineColumn(startLineZero, m_nativeOutputSelection.start.column, &startLineZero);
	int endColZero    = clampLineColumn(endLineZero, m_nativeOutputSelection.end.column, &endLineZero);
	if (startLineZero > endLineZero || (startLineZero == endLineZero && startColZero >= endColZero))
		return false;

	startLine   = startLineZero + 1;
	startColumn = startColZero + 1;
	endLine     = endLineZero + 1;
	endColumn   = endColZero + 1;
	return true;
}

QString WorldView::nativeOutputSelectionText() const
{
	int startLine   = 0;
	int startColumn = 0;
	int endLine     = 0;
	int endColumn   = 0;
	if (!nativeOutputSelectionBounds(startLine, startColumn, endLine, endColumn))
		return {};

	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	if (lines.isEmpty())
		return {};

	QString text;
	for (int lineIndex = startLine - 1; lineIndex <= endLine - 1 && lineIndex < lines.size(); ++lineIndex)
	{
		const NativeOutputRenderLine &line = lines.at(lineIndex);
		const int                     size = sizeToInt(line.text.size());
		const int from = (lineIndex == startLine - 1) ? qBound(0, startColumn - 1, size) : 0;
		const int to   = (lineIndex == endLine - 1) ? qBound(0, endColumn - 1, size) : size;
		if (to > from)
			text += line.text.mid(from, to - from);
		if (lineIndex < endLine - 1)
			text += QLatin1Char('\n');
	}
	return text;
}

QString WorldView::nativeOutputSelectionHtml() const
{
	int startLine   = 0;
	int startColumn = 0;
	int endLine     = 0;
	int endColumn   = 0;
	if (!nativeOutputSelectionBounds(startLine, startColumn, endLine, endColumn))
		return {};

	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	if (lines.isEmpty())
		return {};

	QColor defaultTextColour = m_outputTextColour;
	if (!defaultTextColour.isValid())
		defaultTextColour =
		    m_output ? m_output->palette().color(QPalette::Text) : palette().color(QPalette::Text);

	QString html;
	for (int lineIndex = startLine - 1; lineIndex <= endLine - 1 && lineIndex < lines.size(); ++lineIndex)
	{
		if (!html.isEmpty())
			html += QStringLiteral("<br>");

		const NativeOutputRenderLine &line     = lines.at(lineIndex);
		const int                     textSize = sizeToInt(line.text.size());
		const int lineStart = (lineIndex == startLine - 1) ? qBound(0, startColumn - 1, textSize) : 0;
		const int lineEnd   = (lineIndex == endLine - 1) ? qBound(0, endColumn - 1, textSize) : textSize;
		if (lineEnd <= lineStart)
			continue;

		if (line.spans.isEmpty())
		{
			html += line.text.mid(lineStart, lineEnd - lineStart).toHtmlEscaped();
			continue;
		}

		int offset = 0;
		for (const WorldRuntime::StyleSpan &span : line.spans)
		{
			const int rawLength = qMax(0, span.length);
			if (rawLength <= 0)
				continue;
			const int spanStart = offset;
			const int spanEnd   = qMin(textSize, offset + rawLength);
			offset              = spanEnd;
			if (spanEnd <= lineStart)
				continue;
			if (spanStart >= lineEnd)
				break;

			const int selectStart = qMax(spanStart, lineStart);
			const int selectEnd   = qMin(spanEnd, lineEnd);
			if (selectEnd <= selectStart)
				continue;
			const QString segment = line.text.mid(selectStart, selectEnd - selectStart).toHtmlEscaped();
			const QString style   = htmlStyleForSpan(
			    span, defaultTextColour, m_useCustomLinkColour, m_hyperlinkColour, m_showBold, m_showItalic,
			    m_showUnderline, m_alternativeInverse, m_underlineHyperlinks);
			if (style.isEmpty())
				html += segment;
			else
				html += QStringLiteral("<span style=\"%1\">%2</span>").arg(style, segment);
		}

		if (offset < lineEnd)
		{
			const int from = qMax(offset, lineStart);
			if (lineEnd > from)
				html += line.text.mid(from, lineEnd - from).toHtmlEscaped();
		}
	}

	return html;
}

bool WorldView::handleNativeOutputMouseEvent(const QEvent *event, const QWidget *watched)
{
	if (!nativeOutputInteractionActive() || !event)
		return false;

	WrapTextBrowser *sourceView = nullptr;
	if (watched == m_output || watched == (m_output ? m_output->viewport() : nullptr))
		sourceView = m_output;
	else if (watched == m_liveOutput || watched == (m_liveOutput ? m_liveOutput->viewport() : nullptr))
		sourceView = m_liveOutput;
	if (!sourceView || !sourceView->viewport())
		return false;
	if (sourceView == m_liveOutput &&
	    (!m_scrollbackSplitActive || !m_liveOutput || !m_liveOutput->isVisible()))
		return false;

	auto clampedPosForView = [](const WrapTextBrowser *view, const QPoint &globalPos)
	{
		const QRect rect  = view->viewport()->rect();
		QPoint      local = view->viewport()->mapFromGlobal(globalPos);
		if (rect.width() <= 0 || rect.height() <= 0)
			return QPoint(0, 0);
		local.setX(qBound(0, local.x(), rect.width() - 1));
		local.setY(qBound(0, local.y(), rect.height() - 1));
		return local;
	};
	auto cacheWordUnderMouse = [this](const NativeOutputPosition &position, const bool textHit)
	{
		if (!m_runtime)
			return;
		m_runtime->setWordUnderMenu(textHit ? wordAtNativeOutputPosition(position) : QString(), true);
	};

	switch (event->type())
	{
	case QEvent::MouseButtonPress:
	{
		const auto *mouseEvent = dynamic_cast<const QMouseEvent *>(event);
		if (!mouseEvent || mouseEvent->button() != Qt::LeftButton)
			return false;

		NativeOutputPosition position;
		bool                 textHit = false;
		if (!nativeOutputHitTest(sourceView, mouseEvent->position().toPoint(), position, nullptr, nullptr,
		                         true, false, &textHit))
			return false;
		cacheWordUnderMouse(position, textHit);

		setNativeOutputSelection(sourceView, position, position, true);
		return true;
	}
	case QEvent::MouseMove:
	{
		const auto *mouseEvent = dynamic_cast<const QMouseEvent *>(event);
		if (!mouseEvent)
			return false;
		if (!m_nativeOutputSelection.dragging)
			return false;
		if ((mouseEvent->buttons() & Qt::LeftButton) == Qt::NoButton)
		{
			m_nativeOutputSelection.dragging = false;
			return false;
		}

		WrapTextBrowser *dragView =
		    m_nativeOutputSelection.sourceView ? m_nativeOutputSelection.sourceView : sourceView;
		if (!dragView || !dragView->viewport())
			return false;
		NativeOutputPosition position;
		bool                 textHit = false;
		if (!nativeOutputHitTest(dragView,
		                         clampedPosForView(dragView, mouseEvent->globalPosition().toPoint()),
		                         position, nullptr, nullptr, true, false, &textHit))
		{
			return true;
		}
		cacheWordUnderMouse(position, textHit);

		setNativeOutputSelection(dragView, m_nativeOutputSelection.anchor, position, true);
		return true;
	}
	case QEvent::MouseButtonRelease:
	{
		const auto *mouseEvent = dynamic_cast<const QMouseEvent *>(event);
		if (!mouseEvent || mouseEvent->button() != Qt::LeftButton || !m_nativeOutputSelection.dragging)
			return false;

		WrapTextBrowser *dragView =
		    m_nativeOutputSelection.sourceView ? m_nativeOutputSelection.sourceView : sourceView;
		if (!dragView || !dragView->viewport())
			return false;

		NativeOutputPosition position;
		QString              href;
		QString              hint;
		bool                 textHit = false;
		if (!nativeOutputHitTest(dragView,
		                         clampedPosForView(dragView, mouseEvent->globalPosition().toPoint()),
		                         position, &href, &hint, true, false, &textHit))
		{
			m_nativeOutputSelection.dragging = false;
			return true;
		}
		cacheWordUnderMouse(position, textHit);
		const bool wasClick = m_nativeOutputSelection.anchor.line == position.line &&
		                      m_nativeOutputSelection.anchor.column == position.column;
		setNativeOutputSelection(dragView, m_nativeOutputSelection.anchor, position, false);
		if (wasClick && !href.isEmpty())
			emit hyperlinkActivated(href);
		return true;
	}
	case QEvent::MouseButtonDblClick:
	{
		const auto *mouseEvent = dynamic_cast<const QMouseEvent *>(event);
		if (!mouseEvent || mouseEvent->button() != Qt::LeftButton)
			return false;

		NativeOutputPosition hit;
		bool                 textHit = false;
		if (!nativeOutputHitTest(sourceView, mouseEvent->position().toPoint(), hit, nullptr, nullptr, true,
		                         false, &textHit))
			return false;
		cacheWordUnderMouse(hit, textHit);

		const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
		if (hit.line < 0 || hit.line >= lines.size())
			return false;
		const QString text = lines.at(hit.line).text;
		if (text.isEmpty())
		{
			setNativeOutputSelection(sourceView, hit, hit, false);
			return true;
		}

		auto isDelim = [this](const QChar ch)
		{
			if (ch.isSpace())
				return true;
			const QString &delims =
			    m_wordDelimitersDblClick.isEmpty() ? m_wordDelimiters : m_wordDelimitersDblClick;
			return delims.contains(ch);
		};

		int probe = qBound(0, hit.column, sizeToInt(text.size()) - 1);
		if (isDelim(text.at(probe)))
		{
			setNativeOutputSelection(sourceView, {hit.line, probe}, {hit.line, probe + 1}, false);
			return true;
		}

		int start = probe;
		while (start > 0 && !isDelim(text.at(start - 1)))
			--start;
		int end = probe;
		while (end < text.size() && !isDelim(text.at(end)))
			++end;
		setNativeOutputSelection(sourceView, {hit.line, start}, {hit.line, end}, false);
		return true;
	}
	default:
		break;
	}

	return false;
}

void WorldView::paintTextRectangleCompatibilityFrame(QPainter *painter, const QRegion &updateRegion) const
{
	if (!painter || !m_outputStack || !m_runtime)
		return;

	const WorldRuntime::TextRectangleSettings &settings = m_runtime->textRectangle();
	if (!hasConfiguredTextRectangle(settings))
		return;

	const QRect outputRect = m_outputStack->rect();
	if (outputRect.isEmpty())
		return;

	const QRect textRect = outputTextRectangleUnreserved().intersected(outputRect);
	if (textRect.isEmpty())
		return;

	const int borderOffset = qMax(0, settings.borderOffset);
	QRect     frameRect =
	    textRect.adjusted(-borderOffset, -borderOffset, borderOffset, borderOffset).intersected(outputRect);
	if (frameRect.isEmpty())
		return;

	const QBrush outsideBrush = MiniWindowUtils::makeBrush(settings.outsideFillStyle, settings.borderColour,
	                                                       settings.outsideFillColour);
	if (outsideBrush.style() != Qt::NoBrush)
	{
		painter->save();
		painter->setClipRegion(updateRegion);
		QPainterPath outsidePath;
		outsidePath.setFillRule(Qt::OddEvenFill);
		outsidePath.addRect(outputRect);
		outsidePath.addRect(frameRect);
		painter->fillPath(outsidePath, outsideBrush);
		painter->restore();
	}

	const int borderWidth = qMax(0, settings.borderWidth);
	if (borderWidth <= 0)
		return;

	const QColor borderColor = MiniWindowUtils::colorFromRefOrTransparent(settings.borderColour);
	if (borderColor.alpha() == 0)
		return;

	painter->save();
	painter->setClipRegion(updateRegion);
	painter->setPen(borderColor);
	painter->setBrush(Qt::NoBrush);
	QRect borderRect = frameRect;
	for (int i = 0; i < borderWidth; ++i)
	{
		if (borderRect.width() <= 0 || borderRect.height() <= 0)
			break;
		painter->drawRect(borderRect.adjusted(0, 0, -1, -1));
		borderRect.adjust(1, 1, -1, -1);
	}
	painter->restore();
}

void WorldView::paintNativeOutputCanvas(QPainter *painter, const QRegion &updateRegion) const
{
	if (!painter || !m_nativeOutputCanvas || !m_output)
		return;
	const QRect   viewportRect = painter->viewport();
	const QRegion clippedUpdateRegion =
	    (updateRegion.isEmpty() ? QRegion(viewportRect) : updateRegion).intersected(viewportRect);
	if (clippedUpdateRegion.isEmpty())
		return;

	struct ScrollBlitPendingClearer
	{
			const WorldView *view{nullptr};

			~ScrollBlitPendingClearer()
			{
				if (view)
				{
					view->m_nativeOutputScrollBlitPending     = false;
					view->m_nativeOutputScrollBlitExposedRect = {};
				}
			}
	};
	const ScrollBlitPendingClearer scrollBlitPendingClearer{this};

	paintNativeOutputBackground(painter, clippedUpdateRegion);
	paintMiniWindows(painter, true, clippedUpdateRegion);

	const bool renderCacheNeedsSync = !nativeRenderBaseCacheMatchesCurrentSource();
	if (renderCacheNeedsSync)
		const_cast<WorldView *>(this)->requestNativeRuntimeOutputPresentationSync(false, false);

	const QVector<NativeOutputRenderLine> &lines =
	    renderCacheNeedsSync ? m_nativeRenderLineCache : finalizeNativeOutputRenderLines();
	const bool diagnosticsEnabled = nativeCanvasDiagnosticsEnabled();
	if (diagnosticsEnabled)
	{
		m_nativeOutputCanvas->setProperty("qmud_native_plain_line_count", lines.size());
		m_nativeOutputCanvas->setProperty("qmud_native_plain_last_line",
		                                  lines.isEmpty() ? QString() : lines.constLast().text);
		m_nativeOutputCanvas->setProperty("qmud_native_visual_row_count", 0);
		m_nativeOutputCanvas->setProperty("qmud_native_cache_full_rebuilds", m_nativeRenderCacheFullRebuilds);
		m_nativeOutputCanvas->setProperty("qmud_native_cache_soft_rebuilds", m_nativeRenderCacheSoftRebuilds);
		m_nativeOutputCanvas->setProperty("qmud_native_cache_incremental_updates",
		                                  m_nativeRenderCacheIncrementalUpdates);
		m_nativeOutputCanvas->setProperty("qmud_native_cache_trim_drops", m_nativeRenderCacheTrimDrops);
		m_nativeOutputCanvas->setProperty("qmud_native_cache_rebuild_reason_cache_invalid",
		                                  m_nativeRenderCacheRebuildReasonCacheInvalid);
		m_nativeOutputCanvas->setProperty("qmud_native_cache_rebuild_reason_runtime_disjoint",
		                                  m_nativeRenderCacheRebuildReasonRuntimeDisjoint);
		m_nativeOutputCanvas->setProperty("qmud_native_cache_rebuild_reason_noncontig_no_overlap",
		                                  m_nativeRenderCacheRebuildReasonNonContigNoOverlap);
		m_nativeOutputCanvas->setProperty("qmud_native_cache_rebuild_reason_restitch_failure",
		                                  m_nativeRenderCacheRebuildReasonRestitchFailure);
		m_nativeOutputCanvas->setProperty("qmud_native_cache_rebuild_reason_append_index",
		                                  m_nativeRenderCacheRebuildReasonAppendIndex);
		m_nativeOutputCanvas->setProperty("qmud_native_render_cache_revision",
		                                  m_nativeRenderLineCacheRevision);
		m_nativeOutputCanvas->setProperty("qmud_native_layout_cache_resets", m_nativeLayoutCacheResets);
		m_nativeOutputCanvas->setProperty("qmud_native_layout_row_measurements",
		                                  m_nativeLayoutRowMeasurements);
	}
	if (lines.isEmpty())
		return;

	struct RenderPane
	{
			QRect            textRect;
			int              scrollY{0};
			int              scrollMax{0};
			WrapTextBrowser *view{nullptr};
	};
	QVector<RenderPane> panes;
	panes.reserve(2);

	QRect primaryTextRect = nativeOutputPaneRect(m_output);
	if (primaryTextRect.isEmpty())
		primaryTextRect = outputTextRectangle();
	if (!primaryTextRect.isEmpty())
	{
		const int primaryScrollY = qMax(0, outputScrollPosition());
		const int primaryScrollMax =
		    m_output && m_output->verticalScrollBar() ? qMax(0, m_output->verticalScrollBar()->maximum()) : 0;
		panes.push_back({primaryTextRect, primaryScrollY, primaryScrollMax, m_output});
	}

	if (m_scrollbackSplitActive && m_liveOutput && m_liveOutput->isVisible())
	{
		const QRect liveTextRect = nativeOutputPaneRect(m_liveOutput);
		if (!liveTextRect.isEmpty())
		{
			const int liveScrollY =
			    m_liveOutput->verticalScrollBar() ? qMax(0, m_liveOutput->verticalScrollBar()->value()) : 0;
			const int liveScrollMax =
			    m_liveOutput->verticalScrollBar() ? qMax(0, m_liveOutput->verticalScrollBar()->maximum()) : 0;
			panes.push_back({liveTextRect, liveScrollY, liveScrollMax, m_liveOutput});
		}
	}

	if (panes.isEmpty())
		return;
	const bool hasClippedPane =
	    std::ranges::any_of(panes, [&clippedUpdateRegion](const RenderPane &pane)
	                        { return clippedUpdateRegion.intersects(pane.textRect); });
	if (!hasClippedPane)
		return;

	QColor defaultTextColour = m_outputTextColour;
	if (!defaultTextColour.isValid())
		defaultTextColour =
		    m_output ? m_output->palette().color(QPalette::Text) : palette().color(QPalette::Text);
	const bool wrapEnabled     = m_output->lineWrapMode() != WrapTextBrowser::NoWrap;
	const int  wrapWidthPixels = nativeWrapWidthPixels(panes.constFirst().textRect.width(), wrapEnabled);
	const int  localWrapWidthPixels =
	    nativeLocalWrapWidthPixels(panes.constFirst().textRect.width(), wrapEnabled);
	const int    lineSpacingSetting = qMax(0, m_lineSpacing);
	const QFont &layoutFont         = m_output->font();
	const qreal  lineSpacingFactor  = (100.0 + static_cast<qreal>(lineSpacingSetting)) / 100.0;
	const qreal  fallbackLineAdvance =
	    static_cast<qreal>(qMax(1, QFontMetrics(layoutFont).lineSpacing())) * lineSpacingFactor;
	if (!nativeLayoutCacheReadyFor(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacingSetting,
	                               layoutFont))
	{
		ensureNativeLayoutCaches(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacingSetting,
		                         layoutFont);
	}
	const bool clearScrollBlitLineBackgrounds =
	    m_nativeOutputScrollBlitPending && !m_bleedBackground &&
	    (!m_runtime || m_runtime->backgroundImage().isNull()) && m_miniWindowPaintBoundsValid &&
	    m_miniWindowPaintBounds.underlayBounds.isEmpty() && !m_nativeOutputScrollBlitExposedRect.isEmpty();
	const QRect  scrollBlitExposedRect  = m_nativeOutputScrollBlitExposedRect;
	const QColor nativeBackgroundColour = m_outputBackground.isValid() ? m_outputBackground : QColor(0, 0, 0);
	syncNativeOutputScrollBarsFromLayout(lines, false);
	if (diagnosticsEnabled)
	{
		m_nativeOutputCanvas->setProperty("qmud_native_layout_cache_resets", m_nativeLayoutCacheResets);
		m_nativeOutputCanvas->setProperty("qmud_native_layout_row_measurements",
		                                  m_nativeLayoutRowMeasurements);
	}
	const qreal effectiveLineAdvance =
	    m_nativeLayoutCachedLineAdvance > 0.0 ? m_nativeLayoutCachedLineAdvance : fallbackLineAdvance;
	qreal     docY = m_nativeLayoutCumulativeHeights.size() > lines.size()
	                     ? m_nativeLayoutCumulativeHeights.at(lines.size())
	                     : 0.0;
	const int totalVisualRows =
	    effectiveLineAdvance > 0.0 ? qMax(0, static_cast<int>(std::round(docY / effectiveLineAdvance))) : 0;
	for (RenderPane &pane : panes)
	{
		if (!pane.view)
			continue;
		if (const QScrollBar *const bar = pane.view->verticalScrollBar())
		{
			pane.scrollY   = qMax(0, bar->value());
			pane.scrollMax = qMax(0, bar->maximum());
		}
	}

	int        nativeSelStartLine     = 0;
	int        nativeSelStartColumn   = 0;
	int        nativeSelEndLine       = 0;
	int        nativeSelEndColumn     = 0;
	const bool hasNativeSelection     = nativeOutputSelectionBounds(nativeSelStartLine, nativeSelStartColumn,
	                                                                nativeSelEndLine, nativeSelEndColumn);
	const int  selectionStartLineZero = nativeSelStartLine - 1;
	const int  selectionEndLineZero   = nativeSelEndLine - 1;
	const int  selectionStartColZero  = nativeSelStartColumn - 1;
	const int  selectionEndColZero    = nativeSelEndColumn - 1;
	QColor     selectionColour        = m_output->palette().color(QPalette::Highlight);
	if (!selectionColour.isValid())
		selectionColour = QColor(60, 120, 200);
	selectionColour.setAlpha(110);

	for (const RenderPane &pane : panes)
	{
		const QRegion clippedPaneRegion = clippedUpdateRegion.intersected(pane.textRect);
		const QRect   clippedPaneRect   = clippedPaneRegion.boundingRect();
		if (clippedPaneRect.isEmpty())
			continue;
		int   nativeMaxScroll  = qMax(0, static_cast<int>(std::ceil(docY)) - pane.textRect.height());
		int   effectiveScrollY = nativeMaxScroll > 0 ? qBound(0, pane.scrollY, nativeMaxScroll) : 0;

		qreal visibleTop    = static_cast<qreal>(effectiveScrollY) +
		                      static_cast<qreal>(clippedPaneRect.top() - pane.textRect.top());
		qreal visibleBottom = static_cast<qreal>(effectiveScrollY) +
		                      static_cast<qreal>(clippedPaneRect.bottom() - pane.textRect.top());

		painter->save();
		painter->setClipRegion(clippedPaneRegion);
		const bool drawSelectionForPane =
		    hasNativeSelection && pane.view && pane.view == m_nativeOutputSelection.sourceView;
		auto firstVisibleIt =
		    std::ranges::upper_bound(std::ranges::subrange(m_nativeLayoutCumulativeHeights.cbegin() + 1,
		                                                   m_nativeLayoutCumulativeHeights.cend()),
		                             visibleTop);
		int firstVisibleLine =
		    qMax(0, sizeToInt(firstVisibleIt - m_nativeLayoutCumulativeHeights.cbegin()) - 1);
		auto lastVisibleIt =
		    std::ranges::upper_bound(std::ranges::subrange(m_nativeLayoutCumulativeHeights.cbegin(),
		                                                   m_nativeLayoutCumulativeHeights.cend()),
		                             visibleBottom);
		int lastVisibleLine = qMin(sizeToInt(lines.size()) - 1,
		                           sizeToInt(lastVisibleIt - m_nativeLayoutCumulativeHeights.cbegin()) - 1);
		if (firstVisibleLine > lastVisibleLine)
		{
			painter->restore();
			updateNativeOutputPanePaintState(pane.view, pane.textRect, clippedPaneRegion, effectiveScrollY,
			                                 nativeMaxScroll, lines);
			continue;
		}
		if (ensureNativeLayoutRange(lines, qMax(0, firstVisibleLine - 2),
		                            qMin(sizeToInt(lines.size()) - 1, lastVisibleLine + 2), wrapWidthPixels,
		                            localWrapWidthPixels, lineSpacingSetting, layoutFont))
		{
			docY             = m_nativeLayoutCumulativeHeights.size() > lines.size()
			                       ? m_nativeLayoutCumulativeHeights.at(lines.size())
			                       : 0.0;
			nativeMaxScroll  = qMax(0, static_cast<int>(std::ceil(docY)) - pane.textRect.height());
			effectiveScrollY = nativeMaxScroll > 0 ? qBound(0, pane.scrollY, nativeMaxScroll) : 0;
			visibleTop       = static_cast<qreal>(effectiveScrollY) +
			                   static_cast<qreal>(clippedPaneRect.top() - pane.textRect.top());
			visibleBottom    = static_cast<qreal>(effectiveScrollY) +
			                   static_cast<qreal>(clippedPaneRect.bottom() - pane.textRect.top());
			firstVisibleIt =
			    std::ranges::upper_bound(std::ranges::subrange(m_nativeLayoutCumulativeHeights.cbegin() + 1,
			                                                   m_nativeLayoutCumulativeHeights.cend()),
			                             visibleTop);
			firstVisibleLine =
			    qMax(0, sizeToInt(firstVisibleIt - m_nativeLayoutCumulativeHeights.cbegin()) - 1);
			lastVisibleIt =
			    std::ranges::upper_bound(std::ranges::subrange(m_nativeLayoutCumulativeHeights.cbegin(),
			                                                   m_nativeLayoutCumulativeHeights.cend()),
			                             visibleBottom);
			lastVisibleLine = qMin(sizeToInt(lines.size()) - 1,
			                       sizeToInt(lastVisibleIt - m_nativeLayoutCumulativeHeights.cbegin()) - 1);
		}
		for (int i = firstVisibleLine; i <= lastVisibleLine; ++i)
		{
			const NativeOutputRenderLine &line        = lines.at(i);
			const qreal                   lineTop     = m_nativeLayoutCumulativeHeights.at(i);
			const qreal                   lineBottom  = m_nativeLayoutCumulativeHeights.at(i + 1);
			const qreal                   lineOpacity = qBound(0.0, line.opacity, 1.0);
			const int                     lineY       = static_cast<int>(
			    std::floor(static_cast<qreal>(pane.textRect.top() - effectiveScrollY) + lineTop));
			const int lineBottomY = static_cast<int>(
			    std::ceil(static_cast<qreal>(pane.textRect.top() - effectiveScrollY) + lineBottom));
			QRect lineBackgroundRect(pane.textRect.left(), lineY, pane.textRect.width(),
			                         qMax(1, lineBottomY - lineY));
			lineBackgroundRect = lineBackgroundRect.intersected(clippedPaneRect);
			if (lineBackgroundRect.isEmpty() || !clippedPaneRegion.intersects(lineBackgroundRect))
				continue;
			if (clearScrollBlitLineBackgrounds)
			{
				if (!lineBackgroundRect.isEmpty() && !scrollBlitExposedRect.contains(lineBackgroundRect))
				{
					painter->fillRect(lineBackgroundRect, nativeBackgroundColour);
				}
			}
			if ((line.flags & WorldRuntime::LineHorizontalRule) != 0)
			{
				const int leftX  = pane.textRect.left() + 10;
				const int rightX = pane.textRect.right() - 10;
				if (leftX <= rightX)
				{
					const qreal lineHeight = qMax<qreal>(1.0, lineBottom - lineTop);
					const int   baseY      = static_cast<int>(
					    std::round(pane.textRect.top() - effectiveScrollY + lineTop + (lineHeight / 2.0)));
					painter->save();
					if (lineOpacity < 0.999)
						painter->setOpacity(painter->opacity() * lineOpacity);
					painter->setPen(QColor(132, 132, 132));
					painter->drawLine(leftX, baseY, rightX, baseY);
					painter->setPen(QColor(198, 198, 198));
					painter->drawLine(leftX, baseY + 1, rightX, baseY + 1);
					painter->restore();
				}
			}

			if (line.text.isEmpty())
				continue;

			const QTextLayout *layout = nativeLayoutForLine(i);
			if (!layout || layout->lineCount() <= 0)
				continue;

			QVector<QRectF> selectionRects;
			if (drawSelectionForPane && i >= selectionStartLineZero && i <= selectionEndLineZero)
			{
				const int textSize = sizeToInt(line.text.size());
				const int lineStart =
				    (i == selectionStartLineZero) ? qBound(0, selectionStartColZero, textSize) : 0;
				const int lineEnd =
				    (i == selectionEndLineZero) ? qBound(0, selectionEndColZero, textSize) : textSize;
				if (lineEnd > lineStart)
				{
					for (int row = 0; row < layout->lineCount(); ++row)
					{
						const QTextLine textLine = layout->lineAt(row);
						if (!textLine.isValid())
							continue;
						const int rowStart     = textLine.textStart();
						const int rowEnd       = rowStart + textLine.textLength();
						const int overlapStart = qMax(lineStart, rowStart);
						const int overlapEnd   = qMin(lineEnd, rowEnd);
						if (overlapEnd <= overlapStart)
							continue;

						qreal x1 = textLine.cursorToX(overlapStart);
						qreal x2 = textLine.cursorToX(overlapEnd);
						if (x2 < x1)
							qSwap(x1, x2);
						const qreal rowY = static_cast<qreal>(pane.textRect.top() - effectiveScrollY) +
						                   lineTop + textLine.y();
						selectionRects.push_back(QRectF(static_cast<qreal>(pane.textRect.left()) + x1, rowY,
						                                qMax<qreal>(1.0, x2 - x1), effectiveLineAdvance));
					}
				}
			}

			const QPointF layoutPosition(static_cast<qreal>(pane.textRect.left()),
			                             static_cast<qreal>(pane.textRect.top() - effectiveScrollY) +
			                                 lineTop);
			painter->save();
			if (lineOpacity < 0.999)
				painter->setOpacity(painter->opacity() * lineOpacity);
			QColor fallbackColour = defaultTextColour;
			if (!fallbackColour.isValid())
				fallbackColour = m_output->palette().color(QPalette::Text);
			painter->setFont(layoutFont);
			painter->setPen(fallbackColour);
			layout->draw(painter, layoutPosition);
			painter->restore();

			for (const QRectF &rect : std::as_const(selectionRects))
				painter->fillRect(rect, selectionColour);
		}
		painter->restore();
		updateNativeOutputPanePaintState(pane.view, pane.textRect, clippedPaneRegion, effectiveScrollY,
		                                 nativeMaxScroll, lines);
	}
	if (m_scrollbackSplitActive && m_outputSplitter && m_liveOutput && m_liveOutput->isVisible())
	{
		if (QSplitterHandle *const handle = m_outputSplitter->handle(1); handle && handle->isVisible())
		{
			const QRect   handleRect(m_nativeOutputCanvas->mapFromGlobal(handle->mapToGlobal(QPoint(0, 0))),
			                         handle->size());
			const QRegion clippedHandleRegion = clippedUpdateRegion.intersected(handleRect);
			const QRect   clippedHandleRect   = clippedHandleRegion.boundingRect();
			if (!clippedHandleRect.isEmpty())
			{
				QColor markerColor = m_input ? m_input->palette().color(QPalette::Mid) : QColor();
				if (!markerColor.isValid() && m_input)
					markerColor = m_input->palette().color(QPalette::Dark);
				if (!markerColor.isValid())
					markerColor = QColor(132, 132, 132);
				markerColor = markerColor.lighter(165);
				if (m_outputBackground.isValid())
				{
					const int bgLum     = qGray(m_outputBackground.rgb());
					int       markerLum = qGray(markerColor.rgb());
					if (qAbs(markerLum - bgLum) < 52)
					{
						if (bgLum < 128)
							markerColor = markerColor.lighter(145);
						else
							markerColor = markerColor.darker(145);
						markerLum = qGray(markerColor.rgb());
						if (qAbs(markerLum - bgLum) < 52)
							markerColor = (bgLum < 128) ? QColor(190, 190, 190) : QColor(64, 64, 64);
					}
				}
				QPen markerPen(markerColor);
				markerPen.setCosmetic(true);
				markerPen.setWidth(0);
				painter->save();
				painter->setClipRegion(clippedHandleRegion);
				painter->setPen(markerPen);
				const int y = handleRect.center().y();
				painter->drawLine(handleRect.left(), y, handleRect.right(), y);
				painter->restore();
			}
		}
	}
	if (diagnosticsEnabled)
		m_nativeOutputCanvas->setProperty("qmud_native_visual_row_count", totalVisualRows);
}

void WorldView::handleOutputWheel(const QWheelEvent *event)
{
	if (!m_outputScrollBar || !event)
		return;

	const QPoint angleDelta    = event->angleDelta();
	const QPoint pixelDelta    = event->pixelDelta();
	const bool   preferAngle   = !angleDelta.isNull();
	const int    wheelLineStep = m_smootherScrolling ? 1 : 5;
	const int    lineUnits     = outputScrollUnitsPerLine();

	int          value = m_outputScrollBar->value();
	bool         moved = false;
	if (preferAngle)
	{
		const int delta = angleDelta.y();
		if (delta == 0)
			return;

		if (m_wheelAngleRemainderY != 0 &&
		    ((m_wheelAngleRemainderY > 0 && delta < 0) || (m_wheelAngleRemainderY < 0 && delta > 0)))
		{
			// Suppress tiny opposite-direction deltas from sensitive wheels unless they
			// are large enough to clearly indicate a real direction change.
			const int reversalGuard = qMax(60, 120 - qAbs(m_wheelAngleRemainderY));
			if (qAbs(delta) < reversalGuard)
				return;
			m_wheelAngleRemainderY = 0;
		}

		// Prefer notch-based wheel deltas when available.
		// This avoids high-resolution pixel deltas making wheel scroll feel one-line-at-a-time.
		m_wheelAngleRemainderY += delta;
		const int steps = m_wheelAngleRemainderY / 120;
		m_wheelAngleRemainderY %= 120;
		if (steps != 0)
		{
			value -= steps * wheelLineStep * lineUnits;
			moved = true;
		}
	}
	else if (!pixelDelta.isNull())
	{
		const int delta = pixelDelta.y();
		if (delta == 0)
			return;

		m_wheelAngleRemainderY = 0;
		value -= delta;
		moved = true;
	}
	if (!moved)
		return;
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
	const int defaultHandleWidth =
	    qMax(1, style()->pixelMetric(QStyle::PM_SplitterWidth, nullptr, m_outputSplitter));
	m_outputSplitter->setHandleWidth(m_scrollbackSplitActive ? defaultHandleWidth : 0);

	if (m_scrollbackSplitActive)
	{
		m_nativeSplitTopHeadTrimAdjustedRevision = m_nativeRenderLineCacheRevision;
		const int total                          = m_outputSplitter->size().height();
		int       liveSize                       = m_lastLiveSplitSize;
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
		m_outputSplitter->setSizes(QList<int>() << 1 << 0);
		if (m_lastOutputSelectionView == m_liveOutput)
			m_lastOutputSelectionView = nullptr;
		if (m_nativeOutputSelection.sourceView == m_liveOutput)
			clearNativeOutputSelection(true);
		m_userScrollAction = false;
		if (!m_frozen)
			scrollViewToEnd(m_output);
	}
	syncNativeOutputScrollBarsFromLayout(nativeOutputRenderLines());
	requestNativeOutputRepaint();
}

void WorldView::markNativeRuntimeRangeRestitchPending(const int runtimeLineIndex) const
{
	if (!m_runtime || runtimeLineIndex < 0)
		return;

	m_nativeRenderLineCacheFromRuntime = true;
	if (m_nativeRuntimeRangeRestitchStartIndex < 0)
		m_nativeRuntimeRangeRestitchStartIndex = runtimeLineIndex;
	else
		m_nativeRuntimeRangeRestitchStartIndex =
		    qMin(m_nativeRuntimeRangeRestitchStartIndex, runtimeLineIndex);
}

const WorldView::NativeOutputRenderLines &
WorldView::synchronizeNativeRuntimeOutputPresentation(const bool allowLayoutBuild, const bool followTail)
{
	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	syncNativeOutputScrollBarsFromLayout(lines, allowLayoutBuild);
	if (followTail && !m_frozen)
	{
		if (m_scrollbackSplitActive)
			scrollViewToEnd(m_liveOutput);
		else
			scrollViewToEnd(m_output);
	}
	clearNativeSelectionIfOutsideVisibleViewport(m_output);
	clearNativeSelectionIfOutsideVisibleViewport(m_liveOutput);
	notifyAccessibleOutputPresented(lines);
	return lines;
}

void WorldView::requestNativeRuntimeOutputPresentationSync(const bool allowLayoutBuild, const bool followTail)
{
	m_nativeRuntimeOutputPresentationNeedsLayoutSync =
	    m_nativeRuntimeOutputPresentationNeedsLayoutSync || allowLayoutBuild;
	m_nativeRuntimeOutputPresentationFollowTail = m_nativeRuntimeOutputPresentationFollowTail || followTail;
	if (m_nativeRuntimeOutputPresentationQueued)
		return;

	m_nativeRuntimeOutputPresentationQueued = true;
	QPointer<WorldView> that(this);
	const bool          queued = QMetaObject::invokeMethod(
	    this,
	    [that]
	    {
		    if (!that)
			    return;
		    const bool allowLayoutBuild = that->m_nativeRuntimeOutputPresentationNeedsLayoutSync;
		    const bool followTail       = that->m_nativeRuntimeOutputPresentationFollowTail;
		    that->m_nativeRuntimeOutputPresentationQueued          = false;
		    that->m_nativeRuntimeOutputPresentationNeedsLayoutSync = false;
		    that->m_nativeRuntimeOutputPresentationFollowTail      = false;
		    const QVector<NativeOutputRenderLine> &lines =
		        that->synchronizeNativeRuntimeOutputPresentation(allowLayoutBuild, followTail);
#ifndef NDEBUG
		    if (that->m_nativeRangeRestitchDiagCount > 0)
		    {
			    qInfo().nospace() << "[QMud][RangeRestitch] count=" << that->m_nativeRangeRestitchDiagCount
			                      << " minStart=" << that->m_nativeRangeRestitchDiagMinStart
			                      << " rebuiltTotal=" << that->m_nativeRangeRestitchDiagRebuiltTotal
			                      << " rebuiltMax=" << that->m_nativeRangeRestitchDiagRebuiltMax
			                      << " droppedTotal=" << that->m_nativeRangeRestitchDiagDroppedTotal
			                      << " droppedMax=" << that->m_nativeRangeRestitchDiagDroppedMax;
			    that->m_nativeRangeRestitchDiagCount        = 0;
			    that->m_nativeRangeRestitchDiagMinStart     = -1;
			    that->m_nativeRangeRestitchDiagRebuiltTotal = 0;
			    that->m_nativeRangeRestitchDiagRebuiltMax   = 0;
			    that->m_nativeRangeRestitchDiagDroppedTotal = 0;
			    that->m_nativeRangeRestitchDiagDroppedMax   = 0;
		    }
		    that->logAndResetNativeAppendDiagnostics();
#endif
		    that->requestNativeOutputPresentationRepaint(followTail, lines);
	    },
	    Qt::QueuedConnection);
	if (!queued)
	{
		m_nativeRuntimeOutputPresentationQueued          = false;
		m_nativeRuntimeOutputPresentationNeedsLayoutSync = false;
		m_nativeRuntimeOutputPresentationFollowTail      = false;
		const QVector<NativeOutputRenderLine> &lines =
		    synchronizeNativeRuntimeOutputPresentation(allowLayoutBuild, followTail);
#ifndef NDEBUG
		if (m_nativeRangeRestitchDiagCount > 0)
		{
			qInfo().nospace() << "[QMud][RangeRestitch] count=" << m_nativeRangeRestitchDiagCount
			                  << " minStart=" << m_nativeRangeRestitchDiagMinStart
			                  << " rebuiltTotal=" << m_nativeRangeRestitchDiagRebuiltTotal
			                  << " rebuiltMax=" << m_nativeRangeRestitchDiagRebuiltMax
			                  << " droppedTotal=" << m_nativeRangeRestitchDiagDroppedTotal
			                  << " droppedMax=" << m_nativeRangeRestitchDiagDroppedMax;
			m_nativeRangeRestitchDiagCount        = 0;
			m_nativeRangeRestitchDiagMinStart     = -1;
			m_nativeRangeRestitchDiagRebuiltTotal = 0;
			m_nativeRangeRestitchDiagRebuiltMax   = 0;
			m_nativeRangeRestitchDiagDroppedTotal = 0;
			m_nativeRangeRestitchDiagDroppedMax   = 0;
		}
		logAndResetNativeAppendDiagnostics();
#endif
		requestNativeOutputPresentationRepaint(followTail, lines);
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

void WorldView::requestOutputScrollToEnd(const bool allowLayoutBuild)
{
	if (m_frozen)
		return;
	m_scrollToEndNeedsLayoutSync = m_scrollToEndNeedsLayoutSync || allowLayoutBuild;
	if (m_scrollToEndQueued)
		return;
	if (traceOutputBackfillEnabled())
	{
		const QScrollBar *const bar = m_output ? m_output->verticalScrollBar() : nullptr;
		qInfo().noquote() << QStringLiteral(
		                         "[OutputBackfill] scroll-to-end request world=%1 value=%2 max=%3 split=%4")
		                         .arg(traceWorldName(m_runtime))
		                         .arg(bar ? bar->value() : -1)
		                         .arg(bar ? bar->maximum() : -1)
		                         .arg(m_scrollbackSplitActive ? QStringLiteral("1") : QStringLiteral("0"));
	}
	m_scrollToEndQueued = true;
	QPointer<WorldView> that(this);
	const bool          queued = QMetaObject::invokeMethod(
	    this,
	    [that]
	    {
		    if (!that)
			    return;
		    const bool allowLayoutBuild        = that->m_scrollToEndNeedsLayoutSync;
		    that->m_scrollToEndQueued          = false;
		    that->m_scrollToEndNeedsLayoutSync = false;
		    if (that->m_frozen)
			    return;
		    if (traceOutputBackfillEnabled())
		    {
			    const QScrollBar *const bar = that->m_output ? that->m_output->verticalScrollBar() : nullptr;
			    qInfo().noquote() << QStringLiteral(
			                             "[OutputBackfill] scroll-to-end fire world=%1 value=%2 max=%3 "
			                             "split=%4")
			                             .arg(traceWorldName(that->m_runtime))
			                             .arg(bar ? bar->value() : -1)
			                             .arg(bar ? bar->maximum() : -1)
			                             .arg(that->m_scrollbackSplitActive ? QStringLiteral("1")
			                                                                : QStringLiteral("0"));
		    }
		    const QVector<NativeOutputRenderLine> &lines =
		        that->synchronizeNativeRuntimeOutputPresentation(allowLayoutBuild, true);
		    that->requestNativeOutputPresentationRepaint(true, lines);
	    },
	    Qt::QueuedConnection);
	if (!queued)
	{
		m_scrollToEndQueued          = false;
		m_scrollToEndNeedsLayoutSync = false;
		const QVector<NativeOutputRenderLine> &lines =
		    synchronizeNativeRuntimeOutputPresentation(allowLayoutBuild, true);
		requestNativeOutputPresentationRepaint(true, lines);
	}
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
		bar->setValue(bar->value() - outputScrollUnitsPerLine());
}

void WorldView::scrollOutputLineDown() const
{
	WrapTextBrowser *const view = activeOutputView();
	if (!view)
		return;
	if (QScrollBar *bar = view->verticalScrollBar())
		bar->setValue(bar->value() + outputScrollUnitsPerLine());
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

void WorldView::appendHorizontalRule()
{
	int flags = WorldRuntime::LineHorizontalRule;
	if (m_runtime)
	{
		const QMap<QString, QString> &attrs   = m_runtime->worldAttributes();
		auto                          enabled = [](const QString &value)
		{
			return value == QStringLiteral("1") ||
			       value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
			       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
		};
		if (enabled(attrs.value(QStringLiteral("log_notes"))))
			flags |= WorldRuntime::LineLog;
	}
	appendOutputTextInternal(QString(), true, true, flags);
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
	if (text.isEmpty())
		return selected;
	cursor.insertText(text);
	m_input->setTextCursor(cursor);
	m_inputChanged = true;
	requestInputViewportSync();
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

	setInputText(QString(), false);
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

} // namespace

void WorldView::commitPendingInlineInputBreak()
{
	if (m_runtime)
		m_runtime->finalizePendingInputLineHardReturn();
	if (!m_runtime)
	{
		appendStandaloneOutputEntry(QString(), {}, true, WorldRuntime::LineOutput,
		                            QDateTime::currentDateTime());
		m_nativeRenderLineCacheFromRuntime = false;
		m_nativeRenderLineCacheValid       = false;
		if (!m_frozen)
			requestOutputScrollToEnd();
		requestNativeOutputRepaint();
	}
	else
	{
		markNativeRuntimeTailRestitchPending();
	}
	m_breakBeforeNextServerOutput = false;
}

void WorldView::echoInputText(const QString &text)
{
	if (!m_displayMyInput || !m_output)
		return;
	QString trimmed = text;
	if (trimmed.endsWith(QStringLiteral("\r\n")))
		trimmed.chop(2);
	bool keepOnSameLine = m_keepCommandsOnSameLine;
	if (m_runtime)
	{
		const unsigned short source = m_runtime->currentActionSource();
		const bool           interactiveSource =
		    source == WorldRuntime::eUserTyping || source == WorldRuntime::eUserMacro ||
		    source == WorldRuntime::eUserKeypad || source == WorldRuntime::eUserAccelerator ||
		    source == WorldRuntime::eUserMenuAction;
		if (!interactiveSource)
			keepOnSameLine = false;
	}
	const bool                       appendToCurrentLine = keepOnSameLine && !m_breakBeforeNextServerOutput;
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
		m_runtime->prepareInputEchoForDisplay(trimmed, echoSpans, appendToCurrentLine);
	}

	if (keepOnSameLine)
	{
		bool insertedBreakBeforeEcho = false;
		if (m_breakBeforeNextServerOutput)
		{
			commitPendingInlineInputBreak();
			insertedBreakBeforeEcho = true;
		}
		if (m_runtime)
		{
			bool consumedExistingBreak = false;
			if (!insertedBreakBeforeEcho)
			{
				m_runtime->clearLastLineHardReturn();
				consumedExistingBreak = true;
				markNativeRuntimeTailRestitchPending();
			}
			appendOutputTextInternal(trimmed, false, true, WorldRuntime::LineInput, echoSpans);
			if (consumedExistingBreak)
				commitPendingInlineInputBreak();
			else
				m_breakBeforeNextServerOutput = true;
		}
		else
		{
			bool consumedExistingBreak = false;
			if (!insertedBreakBeforeEcho)
			{
				if (!m_nativeStandaloneOutputLines.isEmpty() &&
				    m_nativeStandaloneOutputLines.last().hardReturn)
				{
					m_nativeStandaloneOutputLines.last().hardReturn = false;
					consumedExistingBreak                           = true;
					m_nativeRenderLineCacheFromRuntime              = false;
					m_nativeRenderLineCacheValid                    = false;
				}
			}
			appendOutputTextInternal(trimmed, false, true, WorldRuntime::LineInput, echoSpans);
			if (consumedExistingBreak)
				commitPendingInlineInputBreak();
			if (!consumedExistingBreak)
				m_breakBeforeNextServerOutput = true;
		}
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
	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	QStringList                            result;
	result.reserve(lines.size());
	for (const NativeOutputRenderLine &line : lines)
		result.push_back(line.text);
	return result;
}

void WorldView::syncOutputTextVisibilityForNativeCanvas() const
{
	const bool nativeCanvasVisible        = m_nativeOutputCanvas && m_nativeOutputCanvas->isVisible();
	const bool runtimeAttachedMain        = m_runtime && m_runtime->view() == this;
	bool       hasRenderableNativeContent = false;
	if (nativeCanvasVisible)
	{
		const bool usesRuntimeLines =
		    m_runtime && (runtimeAttachedMain || m_nativeRenderLineCacheFromRuntime);
		const bool hasRuntimeLines    = usesRuntimeLines && m_runtime && !m_runtime->lines().isEmpty();
		const bool hasStandaloneLines = !usesRuntimeLines && !m_nativeStandaloneOutputLines.isEmpty();
		// Cached runtime lines are renderable only when runtime lines currently exist.
		// Otherwise, they may be stale across startup/reload transitions.
		const bool hasCachedLines =
		    !m_nativeRenderLineCache.isEmpty() && (!m_nativeRenderLineCacheFromRuntime || hasRuntimeLines);
		hasRenderableNativeContent = hasRuntimeLines || hasStandaloneLines || hasCachedLines ||
		                             m_nativeHasPartialOutput || !m_nativePartialOutputText.isEmpty();
	}
	// Runtime-attached world views always render through the native canvas path.
	// Keep text widgets suppressed whenever the native canvas is visible.
	const bool suppressTextPaint = nativeCanvasVisible && (runtimeAttachedMain || hasRenderableNativeContent);
	if (m_miniUnderlay)
		m_miniUnderlay->setVisible(!suppressTextPaint);
	if (m_miniOverlay)
		m_miniOverlay->setVisible(true);
	auto syncView = [suppressTextPaint](WrapTextBrowser *view)
	{
		if (!view)
			return;
		view->setProperty("qmud_native_text_suppressed", suppressTextPaint);
		view->update();
		if (QWidget *const viewport = view->viewport())
			viewport->update();
	};
	syncView(m_output);
	syncView(m_liveOutput);
}

bool WorldView::hasOutputSelection() const
{
	int startLine   = 0;
	int startColumn = 0;
	int endLine     = 0;
	int endColumn   = 0;
	return nativeOutputSelectionBounds(startLine, startColumn, endLine, endColumn);
}

bool WorldView::hasInputSelection() const
{
	if (!m_input)
		return false;
	return m_input->textCursor().hasSelection();
}

QString WorldView::currentHoveredHyperlink() const
{
	WrapTextBrowser     *view = nullptr;
	NativeOutputPosition position;
	QString              href;
	if (nativeOutputHitTestGlobal(QCursor::pos(), view, position, &href, nullptr))
		return href;
	return {};
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
	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	if (lines.isEmpty())
		return;

	const int boundedLine = qBound(0, zeroBasedLine, sizeToInt(lines.size()) - 1);
	const int textSize    = sizeToInt(lines.at(boundedLine).text.size());
	const_cast<WorldView *>(this)->setNativeOutputSelection(m_output, {boundedLine, 0},
	                                                        {boundedLine, textSize}, false);
}

void WorldView::selectOutputRange(int zeroBasedLine, int startColumn, int endColumn) const
{
	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	if (lines.isEmpty())
		return;

	zeroBasedLine      = qBound(0, zeroBasedLine, sizeToInt(lines.size()) - 1);
	const int textSize = sizeToInt(lines.at(zeroBasedLine).text.size());
	startColumn        = qBound(0, startColumn, textSize);
	endColumn          = qBound(startColumn, endColumn, textSize);
	const_cast<WorldView *>(this)->setNativeOutputSelection(m_output, {zeroBasedLine, startColumn},
	                                                        {zeroBasedLine, endColumn}, false);

	if (!m_output || !m_output->viewport())
		return;

	QScrollBar *const bar = m_output->verticalScrollBar();
	if (!bar)
		return;

	const QRect viewportRect = m_output->viewport()->rect();
	if (viewportRect.isEmpty())
		return;

	const bool  wrapEnabled          = m_output->lineWrapMode() != WrapTextBrowser::NoWrap;
	const int   wrapWidthPixels      = nativeWrapWidthPixels(viewportRect.width(), wrapEnabled);
	const int   localWrapWidthPixels = nativeLocalWrapWidthPixels(viewportRect.width(), wrapEnabled);
	const int   lineSpacingSetting   = qMax(0, m_lineSpacing);
	const QFont layoutFont           = m_output->font();
	ensureNativeLayoutCaches(lines, wrapWidthPixels, localWrapWidthPixels, lineSpacingSetting, layoutFont);
	const bool layoutHeightChanged =
	    ensureNativeLayoutRange(lines, zeroBasedLine, zeroBasedLine, wrapWidthPixels, localWrapWidthPixels,
	                            lineSpacingSetting, layoutFont);
	if (layoutHeightChanged)
		syncNativeOutputScrollBarsFromLayout(lines, false);

	if (zeroBasedLine + 1 >= m_nativeLayoutCumulativeHeights.size())
		return;

	const int lineTop =
	    qMax(0, static_cast<int>(std::floor(m_nativeLayoutCumulativeHeights.at(zeroBasedLine))));
	const int lineBottom =
	    qMax(lineTop + 1, static_cast<int>(std::ceil(m_nativeLayoutCumulativeHeights.at(zeroBasedLine + 1))));

	const int currentTop    = bar->value();
	const int pageStep      = qMax(1, bar->pageStep());
	const int currentBottom = currentTop + pageStep;
	int       target        = currentTop;
	if (lineTop < currentTop)
		target = lineTop;
	else if (lineBottom > currentBottom)
		target = lineBottom - pageStep;

	target = qBound(bar->minimum(), target, bar->maximum());
	if (target < bar->maximum())
		const_cast<WorldView *>(this)->setScrollbackSplitActive(true);
	if (target != bar->value())
	{
		if (m_outputScrollBar)
			m_outputScrollBar->setValue(target);
		else
			bar->setValue(target);
	}
}

void WorldView::setOutputSelection(int startLine, int endLine, int startColumn, int endColumn) const
{
	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	if (lines.isEmpty())
		return;

	if (startLine < 1)
		startLine = 1;
	if (endLine < 1)
		endLine = 1;
	if (endLine < startLine)
		qSwap(endLine, startLine);

	const int lineCount = sizeToInt(lines.size());
	startLine           = qMin(startLine, lineCount);
	endLine             = qMin(endLine, lineCount);

	startColumn = qMax(1, startColumn);
	endColumn   = qMax(1, endColumn);

	const int startLineZero = startLine - 1;
	const int endLineZero   = endLine - 1;
	const int startColZero  = qMin(startColumn - 1, sizeToInt(lines.at(startLineZero).text.size()));
	const int endColZero    = qMin(endColumn - 1, sizeToInt(lines.at(endLineZero).text.size()));

	const_cast<WorldView *>(this)->setNativeOutputSelection(m_output, {startLineZero, startColZero},
	                                                        {endLineZero, endColZero}, false);
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

	OutputFindState                       &state      = *m_outputFind;
	const QVector<NativeOutputRenderLine> &lines      = nativeOutputRenderLines();
	const int                              totalLines = sizeToInt(lines.size());
	auto                                   lineTextAt = [&lines](const int zeroBasedLine)
	{
		if (zeroBasedLine < 0 || zeroBasedLine >= lines.size())
			return QString{};
		return lines.at(zeroBasedLine).text;
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

void WorldView::setCommandHistoryList(const QStringList &historyEntries)
{
	m_history.clear();
	if (m_historyLimit > 0)
	{
		const auto      limit = static_cast<qsizetype>(m_historyLimit);
		const qsizetype start = qMax<qsizetype>(0, historyEntries.size() - limit);
		for (qsizetype i = start; i < historyEntries.size(); ++i)
			m_history.push_back(historyEntries.at(i));
	}
	m_lastCommand = m_history.isEmpty() ? QString() : m_history.last();
	resetHistoryRecall();
	if (m_commandHistoryFind)
	{
		m_commandHistoryFind->currentLine = 0;
		m_commandHistoryFind->again       = false;
	}
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
		setInputText(QString(), false);
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
	WrapTextBrowser     *view = nullptr;
	NativeOutputPosition position;
	if (!nativeOutputHitTestGlobal(QCursor::pos(), view, position, nullptr, nullptr))
		return {};

	return wordAtNativeOutputPosition(position);
}

QString WorldView::wordAtNativeOutputPosition(const NativeOutputPosition &position) const
{
	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	if (position.line < 0 || position.line >= lines.size())
		return {};
	const QString text = lines.at(position.line).text;
	if (text.isEmpty())
		return {};

	int  pos = qBound(0, position.column, sizeToInt(text.size()) - 1);

	auto isDelim = [this](QChar ch) { return ch.isSpace() || m_wordDelimiters.contains(ch); };

	if (isDelim(text.at(pos)))
	{
		if (pos > 0 && !isDelim(text.at(pos - 1)))
			pos -= 1;
		else
			return {};
	}

	int start = pos;
	while (start > 0 && !isDelim(text.at(start - 1)))
		--start;
	int end = pos;
	while (end < text.size() && !isDelim(text.at(end)))
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
	syncOutputScrollSingleStep();
	const int inputStep = smoother ? 1 : 5;
	if (m_input && m_input->verticalScrollBar())
		m_input->verticalScrollBar()->setSingleStep(inputStep);
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

int WorldView::outputSelectionStartLine() const
{
	int startLine   = 0;
	int startColumn = 0;
	int endLine     = 0;
	int endColumn   = 0;
	if (!nativeOutputSelectionBounds(startLine, startColumn, endLine, endColumn))
		return 0;
	return startLine;
}

int WorldView::outputSelectionEndLine() const
{
	int startLine   = 0;
	int startColumn = 0;
	int endLine     = 0;
	int endColumn   = 0;
	if (!nativeOutputSelectionBounds(startLine, startColumn, endLine, endColumn))
		return 0;
	return endLine;
}

int WorldView::outputSelectionStartColumn() const
{
	int startLine   = 0;
	int startColumn = 0;
	int endLine     = 0;
	int endColumn   = 0;
	if (!nativeOutputSelectionBounds(startLine, startColumn, endLine, endColumn))
		return 0;
	return startColumn;
}

int WorldView::outputSelectionEndColumn() const
{
	int startLine   = 0;
	int startColumn = 0;
	int endLine     = 0;
	int endColumn   = 0;
	if (!nativeOutputSelectionBounds(startLine, startColumn, endLine, endColumn))
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
	QRect rect = outputTextRectangleUnreserved();
	if (m_runtime && hasConfiguredTextRectangle(m_runtime->textRectangle()))
		return rect;
	if (m_wrapMarginReservationPixels > 0)
		rect.adjust(0, 0, -qMin(m_wrapMarginReservationPixels, qMax(0, rect.width() - 1)), 0);
	return rect;
}

QRect WorldView::outputTextRectangleUnreserved() const
{
	if (!m_outputStack || !m_outputSplitter)
		return {};
	return m_outputSplitter->geometry();
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

void WorldView::requestInputViewportSync()
{
	if (!m_input || m_inputViewportSyncQueued)
		return;
	m_inputViewportSyncQueued = true;
	QPointer<WorldView> guard(this);
	QMetaObject::invokeMethod(
	    this,
	    [guard]
	    {
		    if (!guard)
			    return;
		    guard->m_inputViewportSyncQueued = false;
		    if (!guard->m_input)
			    return;
		    guard->updateInputWrap();
		    guard->updateInputHeight();
	    },
	    Qt::QueuedConnection);
}

void WorldView::requestDrawOutputWindowNotification()
{
	if (m_drawNotifyQueued)
		return;
	m_drawNotifyQueued = true;
	QMetaObject::invokeMethod(
	    this,
	    [this]
	    {
		    m_drawNotifyQueued = false;
		    notifyDrawOutputWindow();
	    },
	    Qt::QueuedConnection);
}

void WorldView::requestWorldOutputResizedNotification()
{
	if (!m_runtime)
		return;
	const QSize clientSize(outputClientWidth(), outputClientHeight());
	if (clientSize.width() <= 0 || clientSize.height() <= 0)
		return;
	if (m_lastQueuedOutputClientSizeValid && clientSize == m_lastQueuedOutputClientSize)
		return;
	m_lastQueuedOutputClientSize      = clientSize;
	m_lastQueuedOutputClientSizeValid = true;
	if (m_worldOutputResizedQueued)
		return;
	m_worldOutputResizedQueued = true;
	QMetaObject::invokeMethod(
	    this,
	    [this]
	    {
		    m_worldOutputResizedQueued = false;
		    if (m_runtime)
			    m_runtime->notifyWorldOutputResized();
	    },
	    Qt::QueuedConnection);
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
	const QString outputText = nativeOutputSelectionText();
	if (!outputText.isEmpty())
	{
		QGuiApplication::clipboard()->setText(outputText);
		return;
	}

	if (hasInputSelection())
	{
		const QTextCursor inputCursor = m_input->textCursor();
		QString           text        = inputCursor.selectedText();
		text.replace(QChar(0x2029), QLatin1Char('\n'));
		QGuiApplication::clipboard()->setText(text);
	}
}

QString WorldView::outputSelectionText() const
{
	return nativeOutputSelectionText();
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
	const QVector<NativeOutputRenderLine> &lines = nativeOutputRenderLines();
	QStringList                            joined;
	joined.reserve(lines.size());
	for (const NativeOutputRenderLine &line : lines)
		joined.push_back(line.text);
	return joined.join(QLatin1Char('\n'));
}

QString WorldView::outputSelectedText() const
{
	return nativeOutputSelectionText();
}

void WorldView::copySelectionAsHtml() const
{
	QString text = nativeOutputSelectionText();
	if (text.isEmpty())
		return;
	QString bodyContent = nativeOutputSelectionHtml();
	if (bodyContent.isEmpty())
		bodyContent = text.toHtmlEscaped();
	const WrapTextBrowser *source =
	    m_nativeOutputSelection.sourceView ? m_nativeOutputSelection.sourceView : m_output;
	if (!source)
		source = m_output;
	if (!source)
		return;

	const QFont   font     = source->font();
	const QString fontFace = font.family();
	const QColor  back     = source->palette().color(QPalette::Base);

	QString       html;
	html += QStringLiteral("<!-- Produced by QMud v %1 - qmud.dev -->\n")
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

void WorldView::appendStandaloneOutputEntry(const QString                          &text,
                                            const QVector<WorldRuntime::StyleSpan> &spans,
                                            const bool hardReturn, const int flags, const QDateTime &time)
{
	WorldRuntime::LineEntry entry;
	entry.text       = text;
	entry.flags      = flags;
	entry.spans      = spans;
	entry.hardReturn = hardReturn;
	entry.time       = time;
	if (m_nativeStandaloneNextLineNumber <= 0)
		m_nativeStandaloneNextLineNumber = 1;
	entry.lineNumber = m_nativeStandaloneNextLineNumber++;
	m_nativeStandaloneOutputLines.push_back(entry);
	m_nativeRenderLineCacheFromRuntime = false;
	m_nativeRenderLineCacheValid       = false;
}

void WorldView::appendOutputTextInternal(const QString &text, bool newLine, bool recordLine, int flags,
                                         const QVector<WorldRuntime::StyleSpan> &spans)
{
	const bool shouldBreakAfterInlineInput =
	    (flags & (WorldRuntime::LineOutput | WorldRuntime::LineNote)) != 0;
	bool injectPendingBreakBeforeRender = false;
	if (shouldBreakAfterInlineInput && m_breakBeforeNextServerOutput)
	{
		// If this call already hard-breaks (e.g. Note()), just consume the
		// pending break flag. Otherwise, inject a line break first.
		if (!(text.isEmpty() && newLine))
		{
			if (m_runtime)
				m_runtime->finalizePendingInputLineHardReturn();
			injectPendingBreakBeforeRender = true;
		}
		else
			m_breakBeforeNextServerOutput = false;
		if (injectPendingBreakBeforeRender)
			m_breakBeforeNextServerOutput = false;
	}

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
		QColor fore;
		QColor back;
		outputDefaultSpanColours(fore, back);
		span.fore = fore;
		span.back = back;
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

	if (recordLine && m_runtime)
	{
		Q_UNUSED(injectPendingBreakBeforeRender);
		m_accessibleOutputPendingTailAppend = true;
		removeNativePartialRenderLineOverlay(false);
		m_hasPartialOutput       = false;
		m_partialOutputStart     = 0;
		m_partialOutputLength    = 0;
		m_nativeHasPartialOutput = false;
		m_nativePartialOutputText.clear();
		m_nativePartialOutputSpans.clear();
		m_nativeRenderLineCacheFromRuntime = true;
		syncOutputTextVisibilityForNativeCanvas();
		if (!m_frozen)
			requestOutputScrollToEnd();
		requestNativeOutputRepaint();
		requestDrawOutputWindowNotification();
		return;
	}

	if (m_frozen)
	{
		if (!m_flushingPending)
		{
			PendingOutput pending;
			pending.text                    = displayEntry.text;
			pending.newLine                 = newLine;
			pending.flags                   = displayEntry.flags;
			pending.spans                   = displayEntry.spans;
			pending.injectBreakBeforeRender = injectPendingBreakBeforeRender;
			m_pendingOutput.push_back(pending);
		}
		return;
	}

	QString                          displayText   = displayEntry.text;
	QVector<WorldRuntime::StyleSpan> renderedSpans = displayEntry.spans;
	buildDisplayLine(displayEntry, previousLineTime, displayText, renderedSpans);

	const double opacity = lineOpacityForTimestamp(displayEntry.time);
	Q_UNUSED(opacity);
	if (injectPendingBreakBeforeRender)
		appendStandaloneOutputEntry(QString(), {}, true, WorldRuntime::LineOutput, displayEntry.time);
	appendStandaloneOutputEntry(displayText, renderedSpans, newLine, displayEntry.flags, displayEntry.time);
	m_accessibleOutputPendingTailAppend = true;
	syncOutputTextVisibilityForNativeCanvas();
	if (!m_frozen)
		requestOutputScrollToEnd();
	requestNativeOutputTailRepaint();
	requestDrawOutputWindowNotification();
}

void WorldView::updatePartialOutputText(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans)
{
	if (m_frozen)
		return;

	if (text.isEmpty())
	{
		clearPartialOutput();
		return;
	}

	m_hasPartialOutput    = false;
	m_partialOutputStart  = 0;
	m_partialOutputLength = 0;
	if (m_breakBeforeNextServerOutput && !text.isEmpty())
		commitPendingInlineInputBreak();

	m_nativeHasPartialOutput  = true;
	m_nativePartialOutputText = text;
	m_nativePartialOutputSpans.clear();
	if (spans.isEmpty())
	{
		WorldRuntime::StyleSpan span;
		span.length = sizeToInt(text.size());
		QColor fore;
		QColor back;
		outputDefaultSpanColours(fore, back);
		span.fore = fore;
		span.back = back;
		m_nativePartialOutputSpans.push_back(span);
	}
	else
	{
		m_nativePartialOutputSpans.reserve(spans.size());
		for (const WorldRuntime::StyleSpan &span : spans)
		{
			if (span.length <= 0)
				continue;
			m_nativePartialOutputSpans.push_back(span);
		}
	}
	if (m_nativeLayoutCacheValid)
	{
		const int tailIndex               = qMax(0, sizeToInt(m_nativeLayoutVisualRows.size()) - 1);
		m_nativeLayoutCumulativeDirtyFrom = qMin(m_nativeLayoutCumulativeDirtyFrom, tailIndex);
	}
	syncOutputTextVisibilityForNativeCanvas();
	requestOutputScrollToEnd();
	requestNativeOutputTailRepaint();
}

void WorldView::clearPartialOutput()
{
	if (!m_nativeHasPartialOutput && m_nativePartialOutputText.isEmpty() &&
	    m_nativePartialOutputSpans.isEmpty() && !m_hasPartialOutput && m_partialOutputStart == 0 &&
	    m_partialOutputLength == 0)
	{
		return;
	}

	m_nativeHasPartialOutput = false;
	m_nativePartialOutputText.clear();
	m_nativePartialOutputSpans.clear();
	removeNativePartialRenderLineOverlay(false);
	m_hasPartialOutput    = false;
	m_partialOutputStart  = 0;
	m_partialOutputLength = 0;
	if (m_nativeLayoutCacheValid)
	{
		const int tailIndex               = qMax(0, sizeToInt(m_nativeLayoutVisualRows.size()) - 1);
		m_nativeLayoutCumulativeDirtyFrom = qMin(m_nativeLayoutCumulativeDirtyFrom, tailIndex);
	}
	syncOutputTextVisibilityForNativeCanvas();
	requestNativeOutputTailRepaint();
	requestOutputScrollToEnd();
}

bool WorldView::commitPendingIncomingPartialOutput()
{
	if (!m_runtime)
		return false;
	if (!m_runtime->commitPendingIncomingPartialLine())
		return false;

	m_nativeHasPartialOutput = false;
	m_nativePartialOutputText.clear();
	m_nativePartialOutputSpans.clear();
	m_hasPartialOutput    = false;
	m_partialOutputStart  = 0;
	m_partialOutputLength = 0;
	if (m_nativeLayoutCacheValid)
	{
		const int tailIndex               = qMax(0, sizeToInt(m_nativeLayoutVisualRows.size()) - 1);
		m_nativeLayoutCumulativeDirtyFrom = qMin(m_nativeLayoutCumulativeDirtyFrom, tailIndex);
	}
	m_nativeRenderLineCacheFromRuntime  = true;
	m_accessibleOutputPendingTailAppend = true;
	syncOutputTextVisibilityForNativeCanvas();
	requestNativeOutputTailRepaint();
	requestOutputScrollToEnd();
	requestDrawOutputWindowNotification();
	return true;
}

void WorldView::notifyRuntimeOutputLineChanged()
{
	m_hasPartialOutput       = false;
	m_partialOutputStart     = 0;
	m_partialOutputLength    = 0;
	m_nativeHasPartialOutput = false;
	m_nativePartialOutputText.clear();
	m_nativePartialOutputSpans.clear();
	m_nativeRenderLineCacheFromRuntime  = true;
	m_accessibleOutputPendingTailAppend = false;
	syncOutputTextVisibilityForNativeCanvas();
	if (!m_frozen)
		requestOutputScrollToEnd();
	requestNativeOutputRepaint();
	notifyAccessibleOutputPresented(nativeOutputRenderLines());
	requestDrawOutputWindowNotification();
}

void WorldView::notifyRuntimeOutputLineChanged(const int runtimeLineIndex)
{
	m_hasPartialOutput       = false;
	m_partialOutputStart     = 0;
	m_partialOutputLength    = 0;
	m_nativeHasPartialOutput = false;
	m_nativePartialOutputText.clear();
	m_nativePartialOutputSpans.clear();
	m_nativeRenderLineCacheFromRuntime  = true;
	m_accessibleOutputPendingTailAppend = false;
	markNativeRuntimeLineRestitchPending(runtimeLineIndex);
	syncOutputTextVisibilityForNativeCanvas();
	requestNativeRuntimeOutputPresentationSync(false, !m_frozen);
	requestDrawOutputWindowNotification();
}

void WorldView::notifyRuntimeOutputRangeChanged(const int runtimeLineIndex)
{
	m_hasPartialOutput       = false;
	m_partialOutputStart     = 0;
	m_partialOutputLength    = 0;
	m_nativeHasPartialOutput = false;
	m_nativePartialOutputText.clear();
	m_nativePartialOutputSpans.clear();
	m_nativeRenderLineCacheFromRuntime  = true;
	m_accessibleOutputPendingTailAppend = false;
	markNativeRuntimeRangeRestitchPending(runtimeLineIndex);
	syncOutputTextVisibilityForNativeCanvas();
	requestNativeRuntimeOutputPresentationSync(false, !m_frozen);
	requestDrawOutputWindowNotification();
}

void WorldView::clearOutputBuffer()
{
	stopIncrementalHyperlinkRestyle();
	m_pendingOutput.clear();
	m_hasPartialOutput       = false;
	m_partialOutputStart     = 0;
	m_partialOutputLength    = 0;
	m_nativeHasPartialOutput = false;
	m_nativePartialOutputText.clear();
	m_nativePartialOutputSpans.clear();
	if (m_runtime)
	{
		m_nativeRenderLineCacheFromRuntime  = true;
		m_nativeRenderLineCacheValid        = false;
		m_accessibleOutputPendingTailAppend = false;
	}
	else
	{
		m_nativeStandaloneOutputLines.clear();
		m_nativeStandaloneNextLineNumber    = 1;
		m_nativeRenderLineCacheFromRuntime  = false;
		m_nativeRenderLineCacheValid        = false;
		m_accessibleOutputPendingTailAppend = false;
	}
	syncNativeOutputScrollBarsFromLayout(nativeOutputRenderLines());
	clearNativeOutputSelection(true);
	syncOutputTextVisibilityForNativeCanvas();
	requestNativeOutputRepaint();
	notifyAccessibleOutputPresented(nativeOutputRenderLines());

	if (m_scrollbackSplitActive)
		scrollViewToEnd(m_liveOutput);
	else
		scrollViewToEnd(m_output);
	requestDrawOutputWindowNotification();
}

void WorldView::restoreOutputFromPersistedLines(const QVector<WorldRuntime::LineEntry> &lines)
{
	stopIncrementalHyperlinkRestyle();
	m_pendingOutput.clear();
	m_hasPartialOutput       = false;
	m_partialOutputStart     = 0;
	m_partialOutputLength    = 0;
	m_nativeHasPartialOutput = false;
	m_nativePartialOutputText.clear();
	m_nativePartialOutputSpans.clear();

	const bool runtimeAttached          = m_runtime && m_runtime->view() == this;
	m_accessibleOutputPendingTailAppend = false;
	if (runtimeAttached)
	{
		// Runtime-attached views always render from runtime-owned line state.
		rebuildNativeRenderCacheFromLineEntries(lines, true);
	}
	else
	{
		m_nativeStandaloneOutputLines = lines;
		qint64 maxLineNumber          = 0;
		for (const WorldRuntime::LineEntry &entry : std::as_const(m_nativeStandaloneOutputLines))
			maxLineNumber = qMax(maxLineNumber, entry.lineNumber);
		if (maxLineNumber <= 0)
		{
			qint64 nextLineNumber = 1;
			for (WorldRuntime::LineEntry &entry : m_nativeStandaloneOutputLines)
				entry.lineNumber = nextLineNumber++;
			m_nativeStandaloneNextLineNumber = nextLineNumber;
		}
		else
			m_nativeStandaloneNextLineNumber = maxLineNumber + 1;
		rebuildNativeRenderCacheFromLineEntries(m_nativeStandaloneOutputLines, false);
	}

	const bool effectiveOutputEmpty =
	    runtimeAttached ? m_runtime->lines().isEmpty() : m_nativeStandaloneOutputLines.isEmpty();
	if (effectiveOutputEmpty)
		clearNativeOutputSelection(true);
	syncOutputTextVisibilityForNativeCanvas();
	syncNativeOutputScrollBarsFromLayout(nativeOutputRenderLines());
	notifyAccessibleOutputPresented(nativeOutputRenderLines());
	if (m_scrollbackSplitActive)
		scrollViewToEnd(m_liveOutput);
	else
		scrollViewToEnd(m_output);
	primeNativeOutputCaches();
	requestDrawOutputWindowNotification();
	requestNativeOutputRepaint();
}

void WorldView::rebuildOutputFromLines(const QVector<WorldRuntime::LineEntry> &lines)
{
	restoreOutputFromPersistedLines(lines);
}

void WorldView::outputDefaultSpanColours(QColor &fore, QColor &back) const
{
	fore = m_outputTextColour;
	back = m_outputBackground;
	if (m_output)
	{
		if (!fore.isValid())
			fore = m_output->palette().color(QPalette::Text);
		if (!back.isValid())
			back = m_output->palette().color(QPalette::Base);
	}
	if (!fore.isValid())
		fore = palette().color(QPalette::Text);
	if (!back.isValid())
		back = palette().color(QPalette::Base);
}

void WorldView::refreshTimestampRenderSettings()
{
	m_outputTimestampRenderSettings = {};
	m_inputTimestampRenderSettings  = {};
	m_notesTimestampRenderSettings  = {};
	if (!m_runtime)
		return;

	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	const auto                    resolveTimestampSettings =
	    [this, &attrs](const QString &preambleKey, const QString &textColourKey, const QString &backColourKey)
	{
		TimestampRenderSettings settings;
		settings.preamble = attrs.value(preambleKey);
		settings.enabled  = !settings.preamble.isEmpty();
		if (!settings.enabled)
			return settings;

		settings.fore = parseColor(attrs.value(textColourKey));
		settings.back = parseColor(attrs.value(backColourKey));
		if (!settings.fore.isValid() && m_output)
			settings.fore = m_output->palette().color(QPalette::Text);
		if (!settings.back.isValid())
		{
			if (m_outputBackground.isValid())
				settings.back = m_outputBackground;
			else if (m_output)
				settings.back = m_output->palette().color(QPalette::Base);
		}
		if (!settings.fore.isValid())
			settings.fore = palette().color(QPalette::Text);
		if (!settings.back.isValid())
			settings.back = palette().color(QPalette::Base);
		return settings;
	};

	m_outputTimestampRenderSettings = resolveTimestampSettings(
	    QStringLiteral("timestamp_output"), QStringLiteral("timestamp_output_text_colour"),
	    QStringLiteral("timestamp_output_back_colour"));
	m_inputTimestampRenderSettings = resolveTimestampSettings(QStringLiteral("timestamp_input"),
	                                                          QStringLiteral("timestamp_input_text_colour"),
	                                                          QStringLiteral("timestamp_input_back_colour"));
	m_notesTimestampRenderSettings = resolveTimestampSettings(QStringLiteral("timestamp_notes"),
	                                                          QStringLiteral("timestamp_notes_text_colour"),
	                                                          QStringLiteral("timestamp_notes_back_colour"));
}

void WorldView::buildDisplayLine(const WorldRuntime::LineEntry &entry, const QDateTime &previousLineTime,
                                 QString &displayText, QVector<WorldRuntime::StyleSpan> &displaySpans) const
{
	if (displaySpans.isEmpty() && !displayText.isEmpty())
	{
		WorldRuntime::StyleSpan span;
		span.length = sizeToInt(displayText.size());
		QColor fore;
		QColor back;
		outputDefaultSpanColours(fore, back);
		span.fore = fore;
		span.back = back;
		displaySpans.push_back(span);
	}

	if (!m_runtime)
		return;

	const TimestampRenderSettings *timestampSettings = &m_outputTimestampRenderSettings;
	if (entry.flags & WorldRuntime::LineNote)
		timestampSettings = &m_notesTimestampRenderSettings;
	else if (entry.flags & WorldRuntime::LineInput)
		timestampSettings = &m_inputTimestampRenderSettings;
	if (!timestampSettings->enabled)
		return;

	const QDateTime lineTime   = entry.time.isValid() ? entry.time : QDateTime::currentDateTime();
	const QDateTime worldStart = m_runtime->worldStartTime();
	const double    elapsed    = (worldStart.isValid() && lineTime.isValid())
	                                 ? (static_cast<double>(worldStart.msecsTo(lineTime)) / 1000.0)
	                                 : 0.0;
	const double    delta      = (previousLineTime.isValid() && lineTime.isValid())
	                                 ? (static_cast<double>(previousLineTime.msecsTo(lineTime)) / 1000.0)
	                                 : 0.0;

	QString         preamble = timestampSettings->preamble;
	preamble.replace(QStringLiteral("%e"), QStringLiteral("%1").arg(elapsed, 0, 'f', 6));
	preamble.replace(QStringLiteral("%D"), QStringLiteral("%1").arg(delta, 10, 'f', 6));
	preamble = m_runtime->formatTime(lineTime, preamble, false);
	if (preamble.isEmpty())
		return;

	WorldRuntime::StyleSpan prefixSpan;
	prefixSpan.length  = sizeToInt(preamble.size());
	prefixSpan.fore    = timestampSettings->fore;
	prefixSpan.back    = timestampSettings->back;
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

const QSet<QString> &WorldView::runtimeSettingsAttributeKeys()
{
	return worldViewRuntimeSettingsAttributeKeys();
}

const QSet<QString> &WorldView::runtimeSettingsMultilineAttributeKeys()
{
	return worldViewRuntimeSettingsMultilineAttributeKeys();
}

bool WorldView::runtimeSettingValuesEquivalent(const QString &key, const QString &before,
                                               const QString &after)
{
	if (before == after)
		return true;
	if (const auto &enabledBoolKeys = worldViewEnabledBooleanAttributeKeys(); enabledBoolKeys.contains(key))
		return isEnabledFlagValue(before) == isEnabledFlagValue(after);
	if (const auto &disabledBoolKeys = worldViewDisabledBooleanAttributeKeys();
	    disabledBoolKeys.contains(key))
		return isDisabledFlagValue(before) == isDisabledFlagValue(after);
	if (const auto &numericKeys = worldViewNumericAttributeKeys(); numericKeys.contains(key))
		return canonicalWorldViewNumericValue(key, before) == canonicalWorldViewNumericValue(key, after);
	if (const auto &colorKeys = worldViewColorAttributeKeys(); colorKeys.contains(key))
	{
		const QColor beforeColor = parseColor(before);
		const QColor afterColor  = parseColor(after);
		if (!beforeColor.isValid() && !afterColor.isValid())
			return true;
		return beforeColor.isValid() && afterColor.isValid() && beforeColor.rgba() == afterColor.rgba();
	}
	return before == after;
}

bool WorldView::runtimeMultilineSettingValuesEquivalent(const QString &key, const QString &before,
                                                        const QString &after)
{
	if (!runtimeSettingsMultilineAttributeKeys().contains(key))
		return before == after;
	return before == after;
}

QSet<QString> WorldView::changedRuntimeSettingsAttributeKeys(const QMap<QString, QString> &before,
                                                             const QMap<QString, QString> &after)
{
	QSet<QString> changedKeys;
	for (const QString &key : runtimeSettingsAttributeKeys())
	{
		if (!runtimeSettingValuesEquivalent(key, before.value(key), after.value(key)))
			changedKeys.insert(key);
	}
	return changedKeys;
}

QSet<QString> WorldView::changedRuntimeSettingsMultilineAttributeKeys(
    const QMap<QString, QString> &beforeMultiline, const QMap<QString, QString> &afterMultiline,
    const QMap<QString, QString> &beforeAttributes, const QMap<QString, QString> &afterAttributes)
{
	QSet<QString> changedKeys;
	for (const QString &key : runtimeSettingsMultilineAttributeKeys())
	{
		QString beforeValue = beforeMultiline.value(key);
		QString afterValue  = afterMultiline.value(key);
		if (key == QStringLiteral("tab_completion_defaults"))
		{
			if (beforeValue.isEmpty())
				beforeValue = beforeAttributes.value(key);
			if (afterValue.isEmpty())
				afterValue = afterAttributes.value(key);
		}
		if (!runtimeMultilineSettingValuesEquivalent(key, beforeValue, afterValue))
			changedKeys.insert(key);
	}
	return changedKeys;
}

bool WorldView::runtimeSettingsNeedFullRebuild(const QSet<QString> &changedAttributeKeys,
                                               const QSet<QString> &changedMultilineKeys)
{
	const auto hasAnyChangedKeys = [](const QSet<QString> &changedKeys, const QSet<QString> &interestingKeys)
	{
		return std::ranges::any_of(changedKeys, [&interestingKeys](const QString &key)
		                           { return interestingKeys.contains(key); });
	};

	return hasAnyChangedKeys(changedAttributeKeys, runtimeSettingsRebuildAttributeKeys()) ||
	       hasAnyChangedKeys(changedMultilineKeys, runtimeSettingsRebuildMultilineAttributeKeys());
}

const QSet<QString> &WorldView::runtimeSettingsRebuildAttributeKeys()
{
	return worldViewRuntimeSettingsRebuildAttributeKeys();
}

const QSet<QString> &WorldView::runtimeSettingsRebuildMultilineAttributeKeys()
{
	return worldViewRuntimeSettingsRebuildMultilineAttributeKeys();
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

void WorldView::paintNativeOutputBackground(QPainter *painter, const QRegion &updateRegion) const
{
	if (!painter)
		return;
	const QRect viewportRect = painter->viewport();
	QRegion     clippedUpdateRegion =
	    (updateRegion.isEmpty() ? QRegion(viewportRect) : updateRegion).intersected(viewportRect);
	const QRect clippedUpdateRect = clippedUpdateRegion.boundingRect();
	if (clippedUpdateRegion.isEmpty())
		return;

	bool  requiresFullBackground = false;
	QRect requiredBackgroundRect = nativeOutputVisiblePaneRepaintRect();
	if (m_runtime)
	{
		requiresFullBackground = !m_runtime->backgroundImage().isNull();
		if (!requiresFullBackground)
		{
			if (!m_miniWindowPaintBoundsValid)
			{
				requiresFullBackground = true;
			}
			else if (!m_miniWindowPaintBounds.underlayBounds.isEmpty())
			{
				requiredBackgroundRect =
				    requiredBackgroundRect.united(m_miniWindowPaintBounds.underlayBounds);
			}
		}
	}
	if (!requiresFullBackground)
	{
		clippedUpdateRegion = clippedUpdateRegion.intersected(requiredBackgroundRect);
		if (clippedUpdateRegion.isEmpty())
			return;
	}

	QRegion    backgroundFillRegion = clippedUpdateRegion;
	const bool limitScrollBlitBackground =
	    m_nativeOutputScrollBlitPending && !m_bleedBackground &&
	    (!m_runtime || m_runtime->backgroundImage().isNull()) && m_miniWindowPaintBoundsValid &&
	    m_miniWindowPaintBounds.underlayBounds.isEmpty() && !m_nativeOutputScrollBlitExposedRect.isEmpty();
	if (limitScrollBlitBackground)
	{
		backgroundFillRegion = backgroundFillRegion.intersected(m_nativeOutputScrollBlitExposedRect);
		if (backgroundFillRegion.isEmpty())
			return;
	}
	painter->save();
	painter->setClipRegion(backgroundFillRegion);
	painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

	const auto drawImageToRect = [painter](const QRect &targetRect, const QImage &image)
	{ painter->drawImage(targetRect, image); };

	const QColor back = m_outputBackground.isValid() ? m_outputBackground : QColor(0, 0, 0);
	for (const QRect &rect : backgroundFillRegion)
		painter->fillRect(rect, back);
	paintTextRectangleCompatibilityFrame(painter, backgroundFillRegion);

	if (!m_runtime)
	{
		painter->restore();
		return;
	}

	const QSize clientSize            = m_outputStack ? m_outputStack->size() : size();
	const QSize ownerSize             = size();
	const auto  drawTiledImageClipped = [painter, &clippedUpdateRect, &clientSize](const QImage &image)
	{
		if (image.isNull())
			return;
		const QSize tileSize   = imageLogicalSize(image);
		const int   tileWidth  = tileSize.width();
		const int   tileHeight = tileSize.height();
		if (tileWidth <= 0 || tileHeight <= 0)
			return;
		const QRect paintBounds =
		    clippedUpdateRect.intersected(QRect(0, 0, clientSize.width(), clientSize.height()));
		if (paintBounds.isEmpty())
			return;
		const int startX = (paintBounds.left() / tileWidth) * tileWidth;
		const int startY = (paintBounds.top() / tileHeight) * tileHeight;
		for (int x = startX; x <= paintBounds.right(); x += tileWidth)
		{
			for (int y = startY; y <= paintBounds.bottom(); y += tileHeight)
				painter->drawImage(QPoint(x, y), image);
		}
	};

	const QImage image = m_runtime->backgroundImage();
	if (!image.isNull())
	{
		const int mode = m_runtime->backgroundImageMode();
		if (mode == 13)
		{
			drawTiledImageClipped(image);
		}
		else
		{
			const QRect rect = positionImageRect(image.size(), clientSize, ownerSize, mode);
			if (rect.intersects(clippedUpdateRect))
				drawImageToRect(rect, image);
		}
	}

	painter->restore();
}

void WorldView::paintMiniWindows(QPainter *painter, bool underneath, const QRegion &updateRegion) const
{
	if (!painter)
		return;
	const QRect viewportRect = painter->viewport();
	QRegion     clippedUpdateRegion =
	    (updateRegion.isEmpty() ? QRegion(viewportRect) : updateRegion).intersected(viewportRect);
	if (clippedUpdateRegion.isEmpty())
		return;

	if (underneath && m_miniWindowPaintBoundsValid)
	{
		if (m_miniWindowPaintBounds.underlayBounds.isEmpty())
			return;
		clippedUpdateRegion = clippedUpdateRegion.intersected(m_miniWindowPaintBounds.underlayBounds);
		if (clippedUpdateRegion.isEmpty())
			return;
	}
	const QRect clippedUpdateRect = clippedUpdateRegion.boundingRect();

	painter->save();
	painter->setClipRegion(clippedUpdateRegion);
	painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

	const auto drawImageToRect = [painter](const QRect &targetRect, const QImage &image)
	{ painter->drawImage(targetRect, image); };

	if (!m_runtime)
	{
		painter->restore();
		return;
	}

	if (!underneath && m_bleedBackground)
	{
		const QColor back = m_outputBackground.isValid() ? m_outputBackground : QColor(0, 0, 0);
		for (const QRect &rect : clippedUpdateRegion)
			painter->fillRect(rect, back);
	}

	const QSize clientSize            = m_outputStack ? m_outputStack->size() : size();
	const QSize ownerSize             = size();
	const auto  drawTiledImageClipped = [painter, &clippedUpdateRect, &clientSize](const QImage &image)
	{
		if (image.isNull())
			return;
		const QSize tileSize   = imageLogicalSize(image);
		const int   tileWidth  = tileSize.width();
		const int   tileHeight = tileSize.height();
		if (tileWidth <= 0 || tileHeight <= 0)
			return;
		const QRect paintBounds =
		    clippedUpdateRect.intersected(QRect(0, 0, clientSize.width(), clientSize.height()));
		if (paintBounds.isEmpty())
			return;
		const int startX = (paintBounds.left() / tileWidth) * tileWidth;
		const int startY = (paintBounds.top() / tileHeight) * tileHeight;
		for (int x = startX; x <= paintBounds.right(); x += tileWidth)
		{
			for (int y = startY; y <= paintBounds.bottom(); y += tileHeight)
				painter->drawImage(QPoint(x, y), image);
		}
	};

	if (!underneath)
	{
		const QImage image = m_runtime->foregroundImage();
		if (!image.isNull())
		{
			const int mode = m_runtime->foregroundImageMode();
			if (mode == 13)
			{
				drawTiledImageClipped(image);
			}
			else
			{
				const QRect rect = positionImageRect(image.size(), clientSize, ownerSize, mode);
				if (rect.intersects(clippedUpdateRect))
					drawImageToRect(rect, image);
			}
		}
	}

	const auto windows = m_runtime->sortedMiniWindows();
	m_runtime->layoutMiniWindows(clientSize, ownerSize, underneath, &windows);

	for (MiniWindow *window : windows)
	{
		if (!window || !window->show || window->temporarilyHide)
			continue;
		const bool drawUnder = (window->flags & kMiniWindowDrawUnderneath) != 0;
		if (drawUnder != underneath)
			continue;

		const QImage *imagePtr = &window->backingSurface();
		if ((window->flags & kMiniWindowTransparent) != 0)
		{
			const QRgb keyRgb =
			    qRgb(window->background.red(), window->background.green(), window->background.blue());
			const auto sourceKey = window->backingSurface().cacheKey();
			if (window->transparentSurfaceCache.isNull() ||
			    window->transparentSurfaceSourceKey != sourceKey ||
			    window->transparentSurfaceKeyRgb != keyRgb)
			{
				window->transparentSurfaceCache = transparentColorKeyedCopy(window->backingSurface(), keyRgb);
				window->transparentSurfaceSourceKey = sourceKey;
				window->transparentSurfaceKeyRgb    = keyRgb;
			}
			imagePtr = &window->transparentSurfaceCache;
		}
		const QImage &image = *imagePtr;

		if (!image.isNull())
		{
			if ((window->flags & kMiniWindowAbsoluteLocation) == 0 && window->position == 13)
			{
				drawTiledImageClipped(image);
				continue;
			}

			if ((window->flags & kMiniWindowAbsoluteLocation) == 0 && window->position >= 0 &&
			    window->position <= 3)
			{
				if (window->rect.intersects(clippedUpdateRect))
					drawImageToRect(window->rect, image);
			}
			else
			{
				if ((window->flags & kMiniWindowAbsoluteLocation) != 0 && hasScaledAbsoluteRect(window))
				{
					if (!window->rect.intersects(clippedUpdateRect))
						continue;
					drawImageToRect(window->rect, image);
				}
				else
				{
					const QRect imageRect(window->rect.topLeft(), imageLogicalSize(image));
					if (imageRect.intersects(clippedUpdateRect))
						painter->drawImage(window->rect.topLeft(), image);
				}
			}
		}
	}
	painter->restore();
}

void WorldView::miniWindowLayerPainted(const bool underneath, const QRegion &paintedRegion) const
{
	if (paintedRegion.isEmpty())
		return;

	QRegion &pendingDirtyRegion =
	    underneath ? m_pendingMiniWindowUnderlayDirtyRegion : m_pendingMiniWindowOverlayDirtyRegion;
	if (pendingDirtyRegion.isEmpty())
		return;
	pendingDirtyRegion = pendingDirtyRegion.subtracted(paintedRegion);
	if (pendingDirtyRegion.isEmpty())
		return;

	if (underneath && m_nativeOutputCanvas && m_nativeOutputCanvas->isVisible())
		m_nativeOutputCanvas->update(pendingDirtyRegion);
	else if (underneath && m_miniUnderlay)
		m_miniUnderlay->update(pendingDirtyRegion);
	else if (!underneath && m_miniOverlay)
		m_miniOverlay->update(pendingDirtyRegion);
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
	Q_UNUSED(includeUnderneath);
	hotspotId.clear();
	windowName.clear();
	if (!m_runtime || !m_outputStack)
		return nullptr;

	const auto  windows        = m_runtime->sortedMiniWindows();
	MiniWindow *fallbackWindow = nullptr;
	QString     fallbackWindowName;
	for (MiniWindow *window : windows | std::views::reverse)
	{
		if (!window || !window->show || window->temporarilyHide)
			continue;
		// MUSHclient parity: underneath or ignore-mouse windows are never mouse-interactive.
		if ((window->flags & kMiniWindowDrawUnderneath) || (window->flags & kMiniWindowIgnoreMouse))
			continue;
		if (window->rect.width() <= 0 || window->rect.height() <= 0)
			continue;
		if (!window->rect.contains(localPos))
			continue;
		if (!fallbackWindow)
		{
			fallbackWindow     = window;
			fallbackWindowName = window->name;
		}
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
	}

	if (fallbackWindow)
	{
		windowName = fallbackWindowName;
		return fallbackWindow;
	}

	return nullptr;
}

void WorldView::callHotspotCallback(MiniWindow *window, const QString &hotspotId, const QString &callbackName,
                                    int flags, const bool queueWhenCallbackLaneBusy) const
{
	if (!window || callbackName.isEmpty() || hotspotId.isEmpty())
		return;
	if (!m_runtime)
		return;
	m_runtime->setWordUnderMenu(QString(), true);
	bool ok = false;
	if (window->callbackPlugin.isEmpty())
		ok = m_runtime->callWorldHotspotFunction(callbackName, flags, hotspotId, window->name,
		                                         queueWhenCallbackLaneBusy);
	else
		ok = m_runtime->callPluginHotspotFunction(window->callbackPlugin, callbackName, flags, hotspotId,
		                                          window->name, queueWhenCallbackLaneBusy);
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

void WorldView::setMiniWindowCallbackMousePosition(MiniWindow &window, const QPoint &callbackLocal,
                                                   const bool updateWindowRelativePosition)
{
	m_lastMousePos             = callbackLocal;
	m_hasLastMousePos          = true;
	window.clientMousePosition = callbackLocal;
	if (updateWindowRelativePosition)
	{
		window.lastMousePosition =
		    miniWindowDisplayToContentUnbounded(&window, callbackLocal - window.rect.topLeft());
		window.lastMouseUpdate++;
	}
}

void WorldView::scheduleCapturedMiniWindowDragMove(const QPoint &callbackLocal)
{
	m_pendingCapturedMiniWindowDragMoveLocal = callbackLocal;
	m_hasPendingCapturedMiniWindowDragMove   = true;
	if (m_capturedMiniWindowDragMoveDrainQueued)
		return;

	m_capturedMiniWindowDragMoveDrainQueued = QMetaObject::invokeMethod(
	    this,
	    [this]
	    {
		    m_capturedMiniWindowDragMoveDrainQueued = false;
		    dispatchPendingCapturedMiniWindowDragMove();
	    },
	    Qt::QueuedConnection);
	if (!m_capturedMiniWindowDragMoveDrainQueued)
		dispatchPendingCapturedMiniWindowDragMove();
}

void WorldView::flushPendingCapturedMiniWindowDragMove(const QPoint &callbackLocal)
{
	if (!m_hasPendingCapturedMiniWindowDragMove)
		return;
	m_pendingCapturedMiniWindowDragMoveLocal = callbackLocal;
	m_capturedMiniWindowDragMoveDrainQueued  = false;
	dispatchPendingCapturedMiniWindowDragMove();
}

void WorldView::dispatchPendingCapturedMiniWindowDragMove()
{
	if (!m_hasPendingCapturedMiniWindowDragMove)
		return;
	m_hasPendingCapturedMiniWindowDragMove = false;
	if (!m_runtime || m_capturedWindowName.isEmpty())
		return;

	MiniWindow *captured = m_runtime->miniWindow(m_capturedWindowName);
	if (!captured || captured->mouseDownHotspot.isEmpty())
		return;

	setMiniWindowCallbackMousePosition(*captured, m_pendingCapturedMiniWindowDragMoveLocal, true);
	const QString down = captured->mouseDownHotspot;
	auto          it   = captured->hotspots.find(down);
	if (it == captured->hotspots.end())
		return;

	const QString moveCallback = it->moveCallback;
	if (!moveCallback.isEmpty())
		callHotspotCallback(captured, down, moveCallback,
		                    withMiniWindowModifierFlags(captured->flagsOnMouseDown));
}

void WorldView::startMiniWindowMouseCapture()
{
	m_mouseCaptured = true;
	if (!m_miniWindowCaptureEventFilterInstalled && qApp)
	{
		qApp->installEventFilter(this);
		m_miniWindowCaptureEventFilterInstalled = true;
	}
}

void WorldView::stopMiniWindowMouseCapture()
{
	if (m_miniWindowCaptureEventFilterInstalled && qApp)
	{
		qApp->removeEventFilter(this);
		m_miniWindowCaptureEventFilterInstalled = false;
	}
	m_mouseCaptured = false;
}

bool WorldView::handleCapturedMiniWindowMouseEvent(QObject *watched, const QMouseEvent *event)
{
	if (!m_mouseCaptured || !event)
		return false;
	auto *watchedWidget = qobject_cast<QWidget *>(watched);
	if (!watchedWidget)
		return false;

	const QWidget *const ownerWindow = window();
	if (ownerWindow && watchedWidget->window() != ownerWindow)
		return false;

	switch (event->type())
	{
	case QEvent::MouseMove:
		return handleMiniWindowMouseMove(event, watchedWidget);
	case QEvent::MouseButtonRelease:
		return handleMiniWindowMouseRelease(event, watchedWidget);
	default:
		return false;
	}
}

void WorldView::applyOutputCursor(const QCursor *cursor)
{
	if (cursor)
	{
		if (m_hasAppliedOutputCursor && m_appliedOutputCursor == *cursor)
			return;
	}
	else if (!m_hasAppliedOutputCursor)
	{
		return;
	}

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
	apply(m_nativeOutputCanvas);
	apply(m_miniUnderlay);
	apply(m_miniOverlay);
	apply(m_output);
	apply(m_output ? m_output->viewport() : nullptr);
	apply(m_liveOutput);
	apply(m_liveOutput ? m_liveOutput->viewport() : nullptr);

	if (cursor)
	{
		m_appliedOutputCursor    = *cursor;
		m_hasAppliedOutputCursor = true;
	}
	else
	{
		m_hasAppliedOutputCursor = false;
	}
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
		if (m_capturedWindowName.isEmpty())
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
			setMiniWindowCallbackMousePosition(*captured, callbackLocal, true);
		if (!m_tooltipHotspot.isEmpty())
		{
			QToolTip::hideText();
			m_tooltipHotspot.clear();
		}
		clearPendingHotspotTooltip();
		if (captured && !captured->mouseDownHotspot.isEmpty())
		{
			if (m_hasCapturedMiniWindowPressLocal && callbackLocal == m_capturedMiniWindowPressLocal)
				return true;
			scheduleCapturedMiniWindowDragMove(callbackLocal);
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
		return !m_capturedWindowName.isEmpty();
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

	m_hoverWindowName = windowName;
	setMiniWindowCallbackMousePosition(*window, local, true);
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
		return false;
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
	m_capturedWindowName                    = window->name;
	m_capturedMiniWindowPressLocal          = local;
	m_hasCapturedMiniWindowPressLocal       = true;
	m_hasPendingCapturedMiniWindowDragMove  = false;
	m_capturedMiniWindowDragMoveDrainQueued = false;
	startMiniWindowMouseCapture();

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

	const QPoint local = mapEventToOutputStack(event->position(), source);
	m_lastMousePos     = local;
	m_hasLastMousePos  = true;
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
	if (pressedWindow && m_hasPendingCapturedMiniWindowDragMove)
	{
		flushPendingCapturedMiniWindowDragMove(local);
		pressedWindow =
		    !m_capturedWindowName.isEmpty() ? m_runtime->miniWindow(m_capturedWindowName) : nullptr;
	}
	QString previousDownHotspot;
	if (pressedWindow)
	{
		setMiniWindowCallbackMousePosition(*pressedWindow, local, true);
		previousDownHotspot = pressedWindow->mouseDownHotspot;
		pressedWindow->mouseDownHotspot.clear();
	}

	stopMiniWindowMouseCapture();
	m_capturedWindowName.clear();
	m_hasCapturedMiniWindowPressLocal      = false;
	m_hasPendingCapturedMiniWindowDragMove = false;

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
						callHotspotCallback(pressedWindow, previousDownHotspot, mouseUpCallback, flags,
						                    (flags & kMiniMouseRight) != 0);
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
		    miniWindowDisplayToContent(windowUnderCursor, local - windowUnderCursor->rect.topLeft());
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
	applyRuntimeSettingsWithPolicy(true);
}

void WorldView::applyRuntimeSettingsWithoutOutputRebuild()
{
	applyRuntimeSettingsWithPolicy(false);
}

void WorldView::resetRuntimeSettingsSnapshot()
{
	m_hasRuntimeSettingsSnapshot = false;
	m_lastRuntimeSettingsAttributes.clear();
	m_lastRuntimeSettingsMultilineAttributes.clear();
	m_lastRuntimeDefaultFontSnapshot = RuntimeDefaultFontSnapshot{};
	m_outputTimestampRenderSettings  = {};
	m_inputTimestampRenderSettings   = {};
	m_notesTimestampRenderSettings   = {};
}

void WorldView::applyRuntimeSettingsWithPolicy(const bool allowRebuild)
{
	if (!m_runtime)
		return;

	const QMap<QString, QString> &attrs          = m_runtime->worldAttributes();
	const QMap<QString, QString> &multilineAttrs = m_runtime->worldMultilineAttributes();

	RuntimeDefaultFontSnapshot    currentDefaultFontSnapshot;
	if (const AppController *app = AppController::instance())
	{
		currentDefaultFontSnapshot.inputFontName =
		    app->getGlobalOption(QStringLiteral("DefaultInputFont")).toString();
		currentDefaultFontSnapshot.inputFontHeight =
		    app->getGlobalOption(QStringLiteral("DefaultInputFontHeight")).toInt();
		currentDefaultFontSnapshot.inputFontWeight =
		    app->getGlobalOption(QStringLiteral("DefaultInputFontWeight")).toInt();
		currentDefaultFontSnapshot.inputFontItalic =
		    app->getGlobalOption(QStringLiteral("DefaultInputFontItalic")).toInt();
		currentDefaultFontSnapshot.inputFontCharset =
		    app->getGlobalOption(QStringLiteral("DefaultInputFontCharset")).toInt();
		currentDefaultFontSnapshot.outputFontName =
		    app->getGlobalOption(QStringLiteral("DefaultOutputFont")).toString();
		currentDefaultFontSnapshot.outputFontHeight =
		    app->getGlobalOption(QStringLiteral("DefaultOutputFontHeight")).toInt();
		currentDefaultFontSnapshot.outputFontCharset =
		    app->getGlobalOption(QStringLiteral("DefaultOutputFontCharset")).toInt();
	}

	QSet<QString> changedViewAttributeKeys;
	QSet<QString> changedViewMultilineKeys;
	if (m_hasRuntimeSettingsSnapshot)
	{
		changedViewAttributeKeys =
		    changedRuntimeSettingsAttributeKeys(m_lastRuntimeSettingsAttributes, attrs);
		changedViewMultilineKeys = changedRuntimeSettingsMultilineAttributeKeys(
		    m_lastRuntimeSettingsMultilineAttributes, multilineAttrs, m_lastRuntimeSettingsAttributes, attrs);
	}
	else
	{
		changedViewAttributeKeys = runtimeSettingsAttributeKeys();
		changedViewMultilineKeys = runtimeSettingsMultilineAttributeKeys();
	}

	if (m_hasRuntimeSettingsSnapshot)
	{
		const bool useDefaultInputFont =
		    isEnabledFlagValue(attrs.value(QStringLiteral("use_default_input_font")));
		const bool useDefaultOutputFont =
		    isEnabledFlagValue(attrs.value(QStringLiteral("use_default_output_font")));

		const bool defaultInputFontChanged =
		    currentDefaultFontSnapshot.inputFontName != m_lastRuntimeDefaultFontSnapshot.inputFontName ||
		    currentDefaultFontSnapshot.inputFontHeight != m_lastRuntimeDefaultFontSnapshot.inputFontHeight ||
		    currentDefaultFontSnapshot.inputFontWeight != m_lastRuntimeDefaultFontSnapshot.inputFontWeight ||
		    currentDefaultFontSnapshot.inputFontItalic != m_lastRuntimeDefaultFontSnapshot.inputFontItalic ||
		    currentDefaultFontSnapshot.inputFontCharset != m_lastRuntimeDefaultFontSnapshot.inputFontCharset;
		const bool defaultOutputFontChanged =
		    currentDefaultFontSnapshot.outputFontName != m_lastRuntimeDefaultFontSnapshot.outputFontName ||
		    currentDefaultFontSnapshot.outputFontHeight !=
		        m_lastRuntimeDefaultFontSnapshot.outputFontHeight ||
		    currentDefaultFontSnapshot.outputFontCharset !=
		        m_lastRuntimeDefaultFontSnapshot.outputFontCharset;

		if (useDefaultInputFont && defaultInputFontChanged)
		{
			changedViewAttributeKeys.insert(QStringLiteral("input_font_name"));
			changedViewAttributeKeys.insert(QStringLiteral("input_font_height"));
			changedViewAttributeKeys.insert(QStringLiteral("input_font_weight"));
			changedViewAttributeKeys.insert(QStringLiteral("input_font_italic"));
			changedViewAttributeKeys.insert(QStringLiteral("input_font_charset"));
		}
		if (useDefaultOutputFont && defaultOutputFontChanged)
		{
			changedViewAttributeKeys.insert(QStringLiteral("output_font_name"));
			changedViewAttributeKeys.insert(QStringLiteral("output_font_height"));
			changedViewAttributeKeys.insert(QStringLiteral("output_font_charset"));
		}
	}

	const bool needsFullRebuild =
	    allowRebuild && runtimeSettingsNeedFullRebuild(changedViewAttributeKeys, changedViewMultilineKeys);
	applyRuntimeSettingsImpl(needsFullRebuild);

	m_lastRuntimeSettingsAttributes.clear();
	for (const QString &key : runtimeSettingsAttributeKeys())
		m_lastRuntimeSettingsAttributes.insert(key, attrs.value(key));

	m_lastRuntimeSettingsMultilineAttributes.clear();
	for (const QString &key : runtimeSettingsMultilineAttributeKeys())
		m_lastRuntimeSettingsMultilineAttributes.insert(key, multilineAttrs.value(key));

	m_lastRuntimeDefaultFontSnapshot = currentDefaultFontSnapshot;
	m_hasRuntimeSettingsSnapshot     = true;
}

void WorldView::applyRuntimeSettingsImpl(const bool rebuildOutput)
{
	if (!m_runtime)
		return;
	if (rebuildOutput && traceOutputBackfillEnabled())
	{
		qInfo().noquote() << QStringLiteral("[OutputBackfill] applyRuntimeSettings requests rebuild world=%1")
		                         .arg(traceWorldName(m_runtime));
	}

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
	}

	if (m_input && useDefaultInputFont)
	{
		m_input->setFont(effectiveDefaultInputFont());
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
	}

	syncOutputScrollSingleStep();

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
		m_outputTextColour = outputText.isValid() ? outputText : QColor::fromRgb(fallbackText);
		palette.setColor(QPalette::Base, Qt::transparent);
		palette.setColor(QPalette::Window, Qt::transparent);
		palette.setColor(QPalette::Text, m_outputTextColour);
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
	refreshTimestampRenderSettings();
	syncOutputTextVisibilityForNativeCanvas();

	const int     wrapColumn     = attrs.value(QStringLiteral("wrap_column")).toInt();
	const QString wrapEnabled    = attrs.value(QStringLiteral("wrap"));
	const bool    wrapOutput     = (wrapEnabled.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
	                                wrapEnabled == QStringLiteral("1") ||
	                                wrapEnabled.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
	const bool    nawsNegotiated = m_runtime && m_runtime->isNawsNegotiated();
	if (m_output)
	{
		m_wrapColumn = wrapColumn;
		if (!wrapOutput)
		{
			m_output->setLineWrapMode(WrapTextBrowser::NoWrap);
			if (m_liveOutput)
				m_liveOutput->setLineWrapMode(WrapTextBrowser::NoWrap);
			m_output->setViewportMarginsPublic(0, 0, 0, 0);
			if (m_liveOutput)
				m_liveOutput->setViewportMarginsPublic(0, 0, 0, 0);
		}
		else
		{
			if (!nawsNegotiated)
			{
				// Runtime applies all non-NAWS wrapping (world wrap and auto-wrap-to-window).
				// Keep the Qt view in NoWrap mode so existing output does not reflow on resize.
				m_output->setLineWrapMode(WrapTextBrowser::NoWrap);
				if (m_liveOutput)
				{
					m_liveOutput->setLineWrapMode(WrapTextBrowser::NoWrap);
				}
				m_output->setViewportMarginsPublic(0, 0, 0, 0);
				if (m_liveOutput)
					m_liveOutput->setViewportMarginsPublic(0, 0, 0, 0);
			}
			else
			{
				m_output->setLineWrapMode(WrapTextBrowser::WidgetWidth);
				if (m_liveOutput)
				{
					m_liveOutput->setLineWrapMode(WrapTextBrowser::WidgetWidth);
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
	const bool    previousUseCustomLinkColour = m_useCustomLinkColour;
	const bool    previousUnderlineHyperlinks = m_underlineHyperlinks;
	const QColor  previousHyperlinkColour     = m_hyperlinkColour;
	const QString useCustomLink               = attrs.value(QStringLiteral("use_custom_link_colour"));
	m_useCustomLinkColour                     = isEnabled(useCustomLink);
	const QString underlineLinks              = attrs.value(QStringLiteral("underline_hyperlinks"));
	m_underlineHyperlinks                     = isEnabled(underlineLinks);
	const QColor linkColour                   = parseColor(attrs.value(QStringLiteral("hyperlink_colour")));
	if (linkColour.isValid())
		m_hyperlinkColour = linkColour;
	const bool hyperlinkPresentationChanged = previousUseCustomLinkColour != m_useCustomLinkColour ||
	                                          previousUnderlineHyperlinks != m_underlineHyperlinks ||
	                                          previousHyperlinkColour != m_hyperlinkColour;
	if (m_noEchoOff)
		m_noEcho = false;
	m_historyLimit = attrs.value(QStringLiteral("history_lines")).toInt();
	if (m_historyLimit < 0)
		m_historyLimit = 0;
	if (m_historyLimit > 0 && m_history.size() > m_historyLimit)
		m_history.remove(0, m_history.size() - m_historyLimit);
	// Runtime output renders from native line state only.
	if (hyperlinkPresentationChanged)
		requestNativeOutputRepaint();

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

	if (rebuildOutput)
		stopIncrementalHyperlinkRestyle();
	if (rebuildOutput && m_scrollbackSplitActive)
	{
		setScrollbackSplitActive(false);
		scrollViewToEnd(m_output);
	}
	if (m_runtime && rebuildOutput)
		restoreOutputFromPersistedLines(m_runtime->lines());
	else if (m_runtime && !rebuildOutput)
	{
		// Pin only stable runtime-backed lines; do not snapshot transient
		// native partial-output overlay content into the persisted cache.
		if (!m_nativeRenderLineCacheValid || !m_nativeRenderLineCacheFromRuntime)
			rebuildNativeRenderCacheFromLineEntries(m_runtime->lines(), true);
		m_nativeRenderLineCacheValid       = true;
		m_nativeRenderLineCacheFromRuntime = false;
		requestNativeOutputRepaint();
	}
	else if (!rebuildOutput)
	{
		m_nativeRenderLineCacheFromRuntime = false;
		m_nativeRenderLineCacheValid       = false;
	}

	updateWrapMargin();
	updateInputWrap();
	updateInputHeight();
	applyDefaultInputHeight(false);
}

void WorldView::stopIncrementalHyperlinkRestyle()
{
	// Native output rendering no longer uses incremental hyperlink restyling.
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

void WorldView::updateLineInformationTooltip(const QWidget *watched, const QMouseEvent *event,
                                             const WrapTextBrowser      *precomputedView,
                                             const QPoint               *precomputedPosInView,
                                             const NativeOutputPosition *precomputedHit,
                                             const bool *precomputedTextHit, const bool allowCacheBuild)
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

	WrapTextBrowser     *view = nullptr;
	NativeOutputPosition hit;
	bool                 textHit = false;
	if (precomputedView && precomputedPosInView && precomputedHit)
	{
		view = const_cast<WrapTextBrowser *>(precomputedView);
		hit  = *precomputedHit;
		if (precomputedTextHit)
			textHit = *precomputedTextHit;
		else if (!nativeOutputHitTest(view, *precomputedPosInView, hit, nullptr, nullptr, allowCacheBuild,
		                              false, &textHit))
		{
			hideLineInfoTooltip();
			return;
		}
	}
	else
	{
		if (watched == m_output || watched == (m_output ? m_output->viewport() : nullptr))
			view = m_output;
		else if (watched == m_liveOutput || watched == (m_liveOutput ? m_liveOutput->viewport() : nullptr))
			view = m_liveOutput;
		if (!view)
			return;

		QPoint posInView;
		if (watched == view)
			posInView = view->viewport()->mapFrom(view, event->position().toPoint());
		else if (watched == view->viewport())
			posInView = event->position().toPoint();
		else
			posInView = view->viewport()->mapFromGlobal(event->globalPosition().toPoint());
		if (!nativeOutputHitTest(view, posInView, hit, nullptr, nullptr, allowCacheBuild, false, &textHit))
		{
			hideLineInfoTooltip();
			return;
		}
	}
	if (!textHit)
	{
		hideLineInfoTooltip();
		return;
	}

	const QVector<NativeOutputRenderLine> &renderLines = nativeOutputRenderLines();
	if (hit.line < 0 || hit.line >= renderLines.size())
	{
		hideLineInfoTooltip();
		return;
	}

	const NativeOutputRenderLine           &renderLine   = renderLines.at(hit.line);
	const QVector<WorldRuntime::LineEntry> &runtimeLines = m_runtime->lines();
	const WorldRuntime::LineEntry          *resolvedLine = nullptr;
	if (renderLine.firstRuntimeLineNumber > 0)
	{
		const int runtimeIndex = findRuntimeLineIndexByNumberNear(
		    runtimeLines, renderLine.firstRuntimeLineNumber, hit.line, false);
		if (runtimeIndex >= 0 && runtimeIndex < runtimeLines.size())
			resolvedLine = &runtimeLines.at(runtimeIndex);
	}
	if (!resolvedLine && hit.line < runtimeLines.size())
		resolvedLine = &runtimeLines.at(hit.line);
	if (!resolvedLine)
	{
		hideLineInfoTooltip();
		return;
	}

	const QString when = resolvedLine->time.isValid()
	                         ? resolvedLine->time.toString(QStringLiteral("dddd, MMMM dd, HH:mm:ss"))
	                         : QStringLiteral("(unknown time)");
	QString       suffix;
	if (renderLine.flags & WorldRuntime::LineNote)
		suffix = QStringLiteral(", (note)");
	else if (renderLine.flags & WorldRuntime::LineInput)
		suffix = QStringLiteral(", (input)");
	const QString text = QStringLiteral("Line %1, %2%3").arg(hit.line + 1).arg(when).arg(suffix);
	scheduleHotspotTooltip(kLineInfoTooltipId, text, event->globalPosition().toPoint());
}

bool WorldView::event(QEvent *event)
{
	const bool devicePixelRatioChanged = event && event->type() == QEvent::DevicePixelRatioChange;
	const bool handled                 = QWidget::event(event);
	if (devicePixelRatioChanged)
		syncMiniWindowDevicePixelRatio();
	return handled;
}

void WorldView::resizeEvent(QResizeEvent *event)
{
	if (traceOutputBackfillEnabled())
	{
		const QScrollBar *const bar = m_output ? m_output->verticalScrollBar() : nullptr;
		qInfo().noquote() << QStringLiteral("[OutputBackfill] resize world=%1 size=%2x%3 value=%4 max=%5")
		                         .arg(traceWorldName(m_runtime))
		                         .arg(width())
		                         .arg(height())
		                         .arg(bar ? bar->value() : -1)
		                         .arg(bar ? bar->maximum() : -1);
	}
	QWidget::resizeEvent(event);
	updateWrapMargin();
	updateInputWrap();
	if (!m_defaultInputHeightApplied)
		applyDefaultInputHeight(true);
	if (m_scrollbackSplitActive && m_outputSplitter)
	{
		const QList<int> sizes = m_outputSplitter->sizes();
		if (sizes.size() >= 2)
			m_lastLiveSplitSize = sizes.at(1);
	}
	refreshMiniWindows(true);
	if (m_runtime)
	{
		// Preserve timing: plugins install only once the output area is actually sized/visible.
		m_runtime->installPendingPlugins();
		requestWorldOutputResizedNotification();
	}
	if (!m_frozen)
		requestOutputScrollToEnd();
	requestDrawOutputWindowNotification();
}

void WorldView::showEvent(QShowEvent *event)
{
	if (traceOutputBackfillEnabled())
	{
		const QScrollBar *const bar = m_output ? m_output->verticalScrollBar() : nullptr;
		qInfo().noquote() << QStringLiteral("[OutputBackfill] show world=%1 value=%2 max=%3")
		                         .arg(traceWorldName(m_runtime))
		                         .arg(bar ? bar->value() : -1)
		                         .arg(bar ? bar->maximum() : -1);
	}
	QWidget::showEvent(event);
	if (m_nativeOutputCanvas)
	{
		m_nativeOutputCanvas->setVisible(true);
		m_nativeOutputCanvas->raise();
	}
	if (m_miniOverlay)
		m_miniOverlay->raise();
	syncOutputTextVisibilityForNativeCanvas();
	applyDefaultInputHeight(true);
	primeNativeOutputCaches();
	syncMiniWindowDevicePixelRatio();
	if (m_runtime)
	{
		m_runtime->installPendingPlugins();
		m_lastQueuedOutputClientSizeValid = false;
		requestWorldOutputResizedNotification();
	}
	requestDrawOutputWindowNotification();
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
	const bool isOutputWidget = watched == m_outputContainer || watched == m_outputStack ||
	                            watched == m_outputSplitter || watched == m_outputScrollBar ||
	                            watched == m_output || watched == m_liveOutput || watched == outputViewport ||
	                            watched == liveOutputViewport;

	if (m_mouseCaptured &&
	    (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease))
	{
		if (auto *mouseEvent = dynamic_cast<QMouseEvent *>(event);
		    mouseEvent && handleCapturedMiniWindowMouseEvent(watched, mouseEvent))
		{
			event->accept();
			return true;
		}
	}

	if ((watched == m_outputContainer || watched == m_outputStack || watched == m_outputSplitter) &&
	    event->type() == QEvent::Resize)
	{
		if (watched == m_outputContainer || watched == m_outputStack)
			updateWrapMargin();
		refreshMiniWindows(true);
		requestWorldOutputResizedNotification();
	}

	if (isOutputWidget && event->type() == QEvent::ShortcutOverride)
	{
		if (const auto *keyEvent = dynamic_cast<QKeyEvent *>(event);
		    keyEvent && hasWorldAcceleratorBinding(keyEvent))
		{
			event->accept();
			return true;
		}
	}

	if (isOutputWidget && event->type() == QEvent::KeyPress)
	{
		if (auto *keyEvent = dynamic_cast<QKeyEvent *>(event))
		{
			if (keyEvent->matches(QKeySequence::Copy) && hasOutputSelection())
			{
				copySelection();
				event->accept();
				return true;
			}

			auto targetOutputView = [&]() -> WrapTextBrowser *
			{
				if (watched == m_liveOutput || watched == liveOutputViewport)
					return m_liveOutput;
				if (watched == m_output || watched == outputViewport)
					return m_output;
				return activeOutputView();
			};

			if (WrapTextBrowser *const targetView = targetOutputView())
			{
				if (QScrollBar *const bar = targetView->verticalScrollBar())
				{
					const int lineStep = outputScrollUnitsPerLine();
					bool      handled  = true;
					switch (keyEvent->key())
					{
					case Qt::Key_PageUp:
						bar->setValue(bar->value() - bar->pageStep());
						break;
					case Qt::Key_PageDown:
						bar->setValue(bar->value() + bar->pageStep());
						break;
					case Qt::Key_Up:
						bar->setValue(bar->value() - lineStep);
						break;
					case Qt::Key_Down:
						bar->setValue(bar->value() + lineStep);
						break;
					case Qt::Key_Home:
						bar->setValue(bar->minimum());
						break;
					case Qt::Key_End:
						bar->setValue(bar->maximum());
						break;
					default:
						handled = false;
						break;
					}

					if (handled)
					{
						noteUserScrollAction();
						requestNativeOutputRepaint();
						event->accept();
						return true;
					}
				}
			}

			if (m_allTypingToCommandWindow && m_input)
			{
				QKeyEvent forwarded(keyEvent->type(), keyEvent->key(), keyEvent->modifiers(),
				                    keyEvent->nativeScanCode(), keyEvent->nativeVirtualKey(),
				                    keyEvent->nativeModifiers(), keyEvent->text(), keyEvent->isAutoRepeat(),
				                    keyEvent->count());
				m_input->setFocus(Qt::OtherFocusReason);
				QCoreApplication::sendEvent(m_input, &forwarded);
				return true;
			}

			if (handleWorldHotkey(keyEvent))
			{
				event->accept();
				return true;
			}
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
			    watched == m_outputScrollBar || watched == m_output || watched == m_liveOutput ||
			    watched == outputViewport || watched == liveOutputViewport)
			{
				handleOutputWheel(wheel);
				event->accept();
				return true;
			}
		}
	}

	if (isOutputWidget)
	{
		auto                *watchedWidget         = qobject_cast<QWidget *>(watched);
		bool                 hasMouseMoveNativeHit = false;
		WrapTextBrowser     *mouseMoveHitView      = nullptr;
		QPoint               mouseMoveHitPosInView;
		NativeOutputPosition mouseMoveHit;
		bool                 mouseMoveTextHit = false;

		if (event->type() == QEvent::MouseMove)
		{
			if (auto *mouseEvent = dynamic_cast<QMouseEvent *>(event))
			{
				QString mouseMoveHref;
				if (nativeOutputHitTestForMouseEvent(watchedWidget, mouseEvent, mouseMoveHitView,
				                                     mouseMoveHitPosInView, mouseMoveHit, &mouseMoveHref,
				                                     nullptr, false, &mouseMoveTextHit))
				{
					hasMouseMoveNativeHit = true;
					applyHoveredHyperlink(mouseMoveHref);
					if (m_runtime)
					{
						m_runtime->setWordUnderMenu(
						    mouseMoveTextHit ? wordAtNativeOutputPosition(mouseMoveHit) : QString(), true);
					}
				}
				else
				{
					refreshHoveredHyperlinkFromCursor();
					if (m_runtime)
						m_runtime->setWordUnderMenu(QString(), false);
				}
			}
			else
				refreshHoveredHyperlinkFromCursor();
		}

		bool handled = false;
		switch (event->type())
		{
		case QEvent::MouseMove:
			if (auto *mouseEvent = dynamic_cast<QMouseEvent *>(event))
				handled = handleMiniWindowMouseMove(mouseEvent, watchedWidget);
			break;
		case QEvent::MouseButtonPress:
			if (auto *mouseEvent = dynamic_cast<QMouseEvent *>(event))
				handled = handleMiniWindowMousePress(mouseEvent, false, watchedWidget);
			break;
		case QEvent::MouseButtonRelease:
			if (auto *mouseEvent = dynamic_cast<QMouseEvent *>(event))
				handled = handleMiniWindowMouseRelease(mouseEvent, watchedWidget);
			break;
		case QEvent::MouseButtonDblClick:
			if (auto *mouseEvent = dynamic_cast<QMouseEvent *>(event))
				handled = handleMiniWindowMousePress(mouseEvent, true, watchedWidget);
			break;
		case QEvent::Leave:
			handleMiniWindowMouseLeave();
			applyHoveredHyperlink(QString());
			if (m_runtime)
				m_runtime->setWordUnderMenu(QString(), false);
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
				const QPoint local = mapEventToOutputStack(QPointF(contextEvent->pos()), watchedWidget);
				if (m_outputStack && m_outputStack->rect().contains(local))
				{
					QString hotspotId;
					QString windowName;
					if (const auto *window = hitTestMiniWindow(local, hotspotId, windowName, true);
					    window && !hotspotId.isEmpty())
						handled = true;
				}
			}
		}
		break;
		default:
			break;
		}

		if (!handled &&
		    (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress ||
		     event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick) &&
		    nativeOutputInteractionActive())
		{
			handled = handleNativeOutputMouseEvent(event, watchedWidget);
		}

		if (!handled && event->type() == QEvent::MouseMove)
		{
			if (auto *mouseEvent = dynamic_cast<QMouseEvent *>(event))
			{
				updateLineInformationTooltip(watchedWidget, mouseEvent,
				                             hasMouseMoveNativeHit ? mouseMoveHitView : nullptr,
				                             hasMouseMoveNativeHit ? &mouseMoveHitPosInView : nullptr,
				                             hasMouseMoveNativeHit ? &mouseMoveHit : nullptr,
				                             hasMouseMoveNativeHit ? &mouseMoveTextHit : nullptr, false);
			}
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
	if (!m_outputStack || !m_outputSplitter)
		return;
	if (m_outputStack->width() <= 0 || m_outputStack->height() <= 0)
		return;

	bool        layoutChanged = false;
	const QRect textRect      = outputTextRectangleForClient(m_outputStack->size(), m_runtime);
	const QRect effectiveRect =
	    textRect.isNull() ? QRect(0, 0, m_outputStack->width(), m_outputStack->height()) : textRect;
	if (m_outputSplitter->geometry() != effectiveRect)
	{
		m_outputSplitter->setGeometry(effectiveRect);
		layoutChanged = true;
	}
	int reservedRight = 0;
	if (m_runtime && !effectiveRect.isEmpty())
	{
		const bool textRectangleCompatActive = hasConfiguredTextRectangle(m_runtime->textRectangle());
		if (!textRectangleCompatActive)
		{
			const auto windows = m_runtime->sortedMiniWindows();
			// Keep miniwindow rects current before calculating reserved margin so
			// window moves/resizes are reflected immediately in NAWS sizing.
			m_runtime->layoutMiniWindows(m_outputStack->size(), size(), false, &windows);

			const bool cacheHit = m_wrapMarginReservationCacheValid &&
			                      m_wrapMarginReservationRect == effectiveRect &&
			                      m_wrapMarginReservationSerial == m_miniWindowChangeSerial;
			if (cacheHit)
				reservedRight = m_wrapMarginReservationPixels;
			else
			{
				int safeRight = effectiveRect.right() + 1;
				for (MiniWindow *window : windows)
				{
					if (!window || !window->show || window->temporarilyHide)
						continue;
					if ((window->flags & kMiniWindowDrawUnderneath) != 0)
						continue;

					QRect candidateRect;
					if (window->position == 6 || window->position == 7 || window->position == 8)
					{
						// For docked windows, use the laid-out rect when available so stacked
						// dock panes reserve their true left edge for NAWS calculations.
						if (!window->rect.isEmpty())
						{
							candidateRect = window->rect;
						}
						else
						{
							const int width = qMax(0, window->width);
							if (width <= 0)
								continue;
							candidateRect = QRect(effectiveRect.right() + 1 - width, effectiveRect.top(),
							                      width, effectiveRect.height());
						}
					}
					else
					{
						if (window->rect.isEmpty())
							continue;
						const bool absolute = (window->flags & kMiniWindowAbsoluteLocation) != 0;
						// Skip full-surface background modes; they are not overlay blockers.
						if (!absolute && window->position >= 0 && window->position <= 3)
							continue;
						if (!absolute && window->position == 13)
							continue;

						const bool rightHalf = window->rect.center().x() >= effectiveRect.center().x();
						if (!rightHalf)
							continue;
						candidateRect = window->rect;
					}

					const QRect overlap = candidateRect.intersected(effectiveRect);
					if (overlap.isEmpty())
						continue;
					safeRight = qMin(safeRight, overlap.left());
				}
				reservedRight                     = qMax(0, (effectiveRect.right() + 1) - safeRight);
				m_wrapMarginReservationCacheValid = true;
				m_wrapMarginReservationRect       = effectiveRect;
				m_wrapMarginReservationSerial     = m_miniWindowChangeSerial;
				m_wrapMarginReservationPixels     = reservedRight;
			}
		}
		else
		{
			m_wrapMarginReservationCacheValid = false;
		}
	}
	else
	{
		m_wrapMarginReservationCacheValid = false;
	}
	m_wrapMarginReservationPixels = reservedRight;

	auto applyMargin = [&layoutChanged](WrapTextBrowser *view)
	{
		if (!view)
			return;
		if (view->width() <= 0 || view->height() <= 0)
			return;

		const QMargins     currentMargins = view->viewportMarginsPublic();
		constexpr QMargins targetMargins(0, 0, 0, 0);
		if (currentMargins != targetMargins)
		{
			view->setViewportMarginsPublic(targetMargins.left(), targetMargins.top(), targetMargins.right(),
			                               targetMargins.bottom());
			layoutChanged = true;
		}
	};

	applyMargin(m_output);
	applyMargin(m_liveOutput);
	if (layoutChanged)
	{
		syncNativeOutputScrollBarsFromLayout(nativeOutputRenderLines());
		requestNativeOutputRepaint();
	}
}

void WorldView::updateInputWrap() const
{
	if (!m_input)
		return;

	if (!m_wrapInput || m_wrapColumn <= 0)
	{
		// Mushclient parity: command input always wraps; wrap_input only constrains
		// the wrap point to wrap_column instead of full visible width.
		m_input->setLineWrapMode(QPlainTextEdit::WidgetWidth);
		m_input->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
		const QMargins     currentMargins = m_input->viewportMarginsPublic();
		constexpr QMargins targetMargins(0, 0, 0, 0);
		if (currentMargins != targetMargins)
			m_input->setViewportMarginsPublic(0, 0, 0, 0);
		return;
	}

	int iWidth = m_wrapColumn + 1;
	if (iWidth < 20)
		iWidth = 20;
	if (iWidth > MAX_LINE_WIDTH)
		iWidth = MAX_LINE_WIDTH;

	const int charWidth      = qMax(1, QFontMetrics(m_input->font()).horizontalAdvance(QLatin1Char('M')));
	const int desiredWidth   = iWidth * charWidth;
	const int frame          = m_input->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, m_input);
	const int availableWidth = qMax(1, m_input->width() - (frame * 2));
	int       leftMargin     = qMax(0, m_inputPixelOffset);
	int       rightMargin    = qMax(0, availableWidth - desiredWidth - leftMargin);

	// Never allow wrap margins to collapse the editable viewport.
	const int minVisibleWidth = charWidth * 2;
	const int maxMargins      = qMax(0, availableWidth - minVisibleWidth);
	if (leftMargin + rightMargin > maxMargins)
		rightMargin = qMax(0, maxMargins - leftMargin);
	if (leftMargin + rightMargin > maxMargins)
	{
		leftMargin  = 0;
		rightMargin = 0;
	}

	const QMargins currentMargins = m_input->viewportMarginsPublic();
	const QMargins targetMargins(leftMargin, 0, rightMargin, 0);
	if (currentMargins != targetMargins)
		m_input->setViewportMarginsPublic(leftMargin, 0, rightMargin, 0);
	m_input->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	m_input->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
}

void WorldView::updateInputHeight() const
{
	if (!m_input)
		return;

	const int frame       = m_input->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, m_input);
	const int lineHeight  = QFontMetrics(m_input->font()).lineSpacing();
	const int inputChrome = inputVerticalChromePx(m_input, m_input->viewportMarginsPublic(), frame);
	const int singleLine  = lineHeight + inputChrome;
	if (!m_autoResizeCommandWindow)
	{
		m_input->setMinimumHeight(singleLine);
		m_input->setMaximumHeight(QWIDGETSIZE_MAX);
		ensureCursorVisibleNowAndQueued(m_input);
		m_autoResizeInputDocRevision     = -1;
		m_autoResizeInputBlockCount      = 0;
		m_autoResizeInputViewportWidth   = -1;
		m_autoResizeInputAppliedLines    = 1;
		m_autoResizeLastMinLines         = 1;
		m_autoResizeLastMaxLines         = 20;
		m_autoResizeLastWrapInput        = m_wrapInput;
		m_autoResizeLastWrapColumn       = m_wrapColumn;
		m_autoResizeLastInputPixelOffset = m_inputPixelOffset;
		return;
	}

	int minLines = qMax(1, m_autoResizeMinimumLines);
	int maxLines = qMax(1, m_autoResizeMaximumLines);
	if (minLines > maxLines)
		qSwap(minLines, maxLines);
	const bool policyUnchanged =
	    minLines == m_autoResizeLastMinLines && maxLines == m_autoResizeLastMaxLines &&
	    m_wrapInput == m_autoResizeLastWrapInput && m_wrapColumn == m_autoResizeLastWrapColumn &&
	    m_inputPixelOffset == m_autoResizeLastInputPixelOffset;

	int       lineCount     = 1;
	int       docRevision   = -1;
	int       docBlockCount = 1;
	const int viewportWidth = m_input->viewport() ? m_input->viewport()->width() : -1;
	if (QTextDocument *doc = m_input->document())
	{
		docRevision   = doc->revision();
		docBlockCount = qMax(1, doc->blockCount());
		lineCount     = docBlockCount;

		if (m_input->lineWrapMode() == QPlainTextEdit::WidgetWidth)
		{
			if (QAbstractTextDocumentLayout *layout = doc->documentLayout())
				(void)layout->documentSize();

			int visualLines = 0;
			for (QTextBlock block = doc->begin(); block.isValid(); block = block.next())
			{
				const QTextLayout *layout           = block.layout();
				const int          blockVisualLines = layout ? qMax(1, layout->lineCount()) : 1;
				visualLines += blockVisualLines;
			}
			lineCount = qMax(1, visualLines);

			const QTextCursor cursor              = m_input->textCursor();
			const int         documentEndPosition = qMax(0, doc->characterCount() - 1);
			if (cursor.position() >= documentEndPosition && lineCount > 1 && doc->blockCount() == 1)
			{
				const QString inputText = m_input->toPlainText();
				const bool    hasExplicitTrailingNewline =
				    inputText.endsWith(QLatin1Char('\n')) || inputText.endsWith(QLatin1Char('\r'));
				if (!hasExplicitTrailingNewline)
				{
					const QTextBlock   lastBlock  = doc->lastBlock();
					const QTextLayout *lastLayout = lastBlock.layout();
					if (lastLayout && lastLayout->lineCount() > 1)
					{
						const QTextLine lastLine = lastLayout->lineAt(lastLayout->lineCount() - 1);
						if (lastLine.isValid() && lastLine.textLength() == 0)
							--lineCount;
					}
				}
			}
		}

		// Never collapse explicit multiline input below block count even if
		// transient layout metrics under-report wrapped visual rows.
		lineCount = qMax(lineCount, docBlockCount);
	}

	auto targetHeightForLines = [lineHeight, inputChrome](const int lines)
	{ return (lineHeight * lines) + inputChrome; };

	auto applyHeightForLines = [this, &targetHeightForLines](const int lines)
	{
		const int targetHeight = targetHeightForLines(lines);
		if (m_input->minimumHeight() == targetHeight && m_input->maximumHeight() == targetHeight)
			return;
		m_input->setMinimumHeight(targetHeight);
		m_input->setMaximumHeight(targetHeight);
	};

	int targetLines = qBound(minLines, lineCount, maxLines);
	if (docRevision >= 0)
	{
		const bool docShapeUnchanged = docRevision == m_autoResizeInputDocRevision &&
		                               docBlockCount == m_autoResizeInputBlockCount &&
		                               viewportWidth == m_autoResizeInputViewportWidth;
		if (docShapeUnchanged && policyUnchanged && targetLines < m_autoResizeInputAppliedLines)
			targetLines = m_autoResizeInputAppliedLines;
	}
	applyHeightForLines(targetLines);
	if (m_splitter)
	{
		const int total = m_splitter->size().height();
		if (total > 0)
		{
			const int        targetHeight = qBound(0, targetHeightForLines(targetLines), total);
			const int        outputHeight = total - targetHeight;
			const QList<int> currentSizes = m_splitter->sizes();
			if (currentSizes.size() >= 2 &&
			    (qAbs(currentSizes.at(0) - outputHeight) > 1 || qAbs(currentSizes.at(1) - targetHeight) > 1))
			{
				m_splitter->setSizes(QList<int>() << outputHeight << targetHeight);
			}
		}
	}
	if (docRevision >= 0)
	{
		m_autoResizeInputDocRevision     = docRevision;
		m_autoResizeInputBlockCount      = docBlockCount;
		m_autoResizeInputViewportWidth   = viewportWidth;
		m_autoResizeInputAppliedLines    = targetLines;
		m_autoResizeLastMinLines         = minLines;
		m_autoResizeLastMaxLines         = maxLines;
		m_autoResizeLastWrapInput        = m_wrapInput;
		m_autoResizeLastWrapColumn       = m_wrapColumn;
		m_autoResizeLastInputPixelOffset = m_inputPixelOffset;
	}

	if (targetLines >= lineCount)
	{
		if (QScrollBar *const bar = m_input->verticalScrollBar())
			bar->setValue(bar->minimum());
		m_input->viewport()->update();
		return;
	}

	ensureCursorVisibleNowAndQueued(m_input);
}

void WorldView::applyDefaultInputHeight(bool setSplitterSizes)
{
	if (!m_input || m_defaultInputHeightApplied)
		return;

	const int frame       = m_input->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, m_input);
	const int lineHeight  = QFontMetrics(m_input->font()).lineSpacing();
	const int inputChrome = inputVerticalChromePx(m_input, m_input->viewportMarginsPublic(), frame);
	const int singleLine  = lineHeight + inputChrome;

	m_input->setMinimumHeight(singleLine);
	m_input->setMaximumHeight(singleLine);

	if (setSplitterSizes && m_splitter)
	{
		if (!isVisible())
		{
			m_input->setMaximumHeight(QWIDGETSIZE_MAX);
			return;
		}
		const int total = m_splitter->size().height();
		if (total > singleLine)
		{
			const int outputSize = total - singleLine;
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

	appendDeduplicatedHistoryEntry(m_history, text, m_historyLimit);
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
			requestInputViewportSync();
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
		requestInputViewportSync();
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
	requestInputViewportSync();
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
		const bool    noHistory = (noHistoryFlag == QStringLiteral("1") ||
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
			requestInputViewportSync();
		}
		return true;
	}

	return false;
}

namespace
{
	struct AcceleratorLookup
	{
			quint32 virt{0};
			quint16 keyCode{0};
			qint64  mapKey{0};
	};

#ifdef Q_OS_WIN
	[[nodiscard]] quint16 windowsNumpadVirtualKey(const quint32 nativeVirtualKey)
	{
		// Windows Alt+Numpad input can omit Qt::KeypadModifier.
		switch (nativeVirtualKey)
		{
		case 0x60: // VK_NUMPAD0
		case 0x61: // VK_NUMPAD1
		case 0x62: // VK_NUMPAD2
		case 0x63: // VK_NUMPAD3
		case 0x64: // VK_NUMPAD4
		case 0x65: // VK_NUMPAD5
		case 0x66: // VK_NUMPAD6
		case 0x67: // VK_NUMPAD7
		case 0x68: // VK_NUMPAD8
		case 0x69: // VK_NUMPAD9
		case 0x6A: // VK_MULTIPLY
		case 0x6B: // VK_ADD
		case 0x6D: // VK_SUBTRACT
		case 0x6F: // VK_DIVIDE
			return static_cast<quint16>(nativeVirtualKey);
		default:
			return 0;
		}
	}

	[[nodiscard]] bool windowsNumpadDigitVirtualKey(const quint32 nativeVirtualKey)
	{
		return nativeVirtualKey >= 0x60 && nativeVirtualKey <= 0x69; // VK_NUMPAD0..VK_NUMPAD9
	}
#endif

	bool buildAcceleratorLookup(const QKeyEvent *event, const bool keypad, AcceleratorLookup &lookup)
	{
		if (!event)
			return false;

		const Qt::KeyboardModifiers mods = event->modifiers();
		if ((mods & Qt::MetaModifier) != 0)
			return false;

		const int key = event->key();
#ifdef Q_OS_WIN
		quint16 forcedWindowsNumpadKeyCode = 0;
		if (!keypad && (mods & Qt::AltModifier) != 0)
			forcedWindowsNumpadKeyCode = windowsNumpadVirtualKey(event->nativeVirtualKey());
#endif
		if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta ||
		    (key == Qt::Key_unknown
#ifdef Q_OS_WIN
		     && forcedWindowsNumpadKeyCode == 0
#endif
		     ))
			return false;

		lookup.virt = AcceleratorUtils::kVirtKeyFlag | AcceleratorUtils::kNoInvertFlag;
		if ((mods & Qt::ShiftModifier) != 0)
			lookup.virt |= AcceleratorUtils::kShiftFlag;
		if ((mods & Qt::ControlModifier) != 0)
			lookup.virt |= AcceleratorUtils::kControlFlag;
		if ((mods & Qt::AltModifier) != 0)
			lookup.virt |= AcceleratorUtils::kAltFlag;

#ifdef Q_OS_WIN
		lookup.keyCode = (forcedWindowsNumpadKeyCode != 0)
		                     ? forcedWindowsNumpadKeyCode
		                     : AcceleratorUtils::qtKeyToVirtualKey(static_cast<Qt::Key>(key), keypad);
#else
		lookup.keyCode = AcceleratorUtils::qtKeyToVirtualKey(static_cast<Qt::Key>(key), keypad);
#endif
		if (lookup.keyCode == 0)
			return false;

		lookup.mapKey = (static_cast<qint64>(lookup.virt) << 16) | lookup.keyCode;
		return true;
	}

	int findAcceleratorCommandForEvent(const WorldRuntime *runtime, const QKeyEvent *event,
	                                   const bool keypadHint, AcceleratorLookup *resolvedLookup = nullptr)
	{
		if (!runtime || !event)
			return -1;

		AcceleratorLookup lookup;
		if (!buildAcceleratorLookup(event, keypadHint, lookup))
			return -1;

		const int commandId = runtime->acceleratorCommandForKey(lookup.mapKey);
		if (commandId < 0)
			return -1;
		if (resolvedLookup)
			*resolvedLookup = lookup;
		return commandId;
	}
} // namespace

bool WorldView::hasWorldAcceleratorBinding(const QKeyEvent *event) const
{
	if (!m_runtime || !event)
		return false;

	const bool keypad = (event->modifiers() & Qt::KeypadModifier) != 0;
	return findAcceleratorCommandForEvent(m_runtime, event, keypad) >= 0;
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
	const bool keypad               = keypadModifier || keypadRepeatFallback;

	auto       tryAccelerator = [&]() -> bool
	{
		AcceleratorLookup lookup;
		const int         commandId = findAcceleratorCommandForEvent(m_runtime, event, keypad, &lookup);
		if (commandId < 0)
			return false;

		m_runtime->setCurrentActionSource(WorldRuntime::eUserAccelerator);
		const bool ok = m_runtime->executeAcceleratorCommand(
		    commandId, AcceleratorUtils::acceleratorToString(lookup.virt, lookup.keyCode));
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
		const QMap<QString, QString> &attrs   = m_runtime->worldAttributes();
		const QString                 enabled = attrs.value(QStringLiteral("keypad_enable"));
		const bool keypadEnabled = (enabled == QStringLiteral("1") ||
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
	requestInputViewportSync();
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
	ensureCursorVisibleNowAndQueued(m_input);
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
		m_partialIndex   = sizeToInt(m_history.size());
	}

	if (m_partialCommand.isEmpty())
	{
		recallHistory(direction);
		return;
	}

	const auto findMatch = [&](const int step) -> int
	{
		int index = m_partialIndex;
		while (true)
		{
			index += step;
			if (index < 0 || index >= m_history.size())
				return -1;

			const QString candidate = m_history.at(index);
			if (!candidate.startsWith(m_partialCommand, Qt::CaseInsensitive))
				continue;
			if (candidate.compare(m_partialCommand, Qt::CaseInsensitive) == 0)
				continue;
			return index;
		}
	};

	const bool preferNewest = (direction < 0) || (m_partialIndex >= m_history.size());
	int        index        = preferNewest ? findMatch(-1) : findMatch(1);
	if (index < 0)
	{
		if (confirmReplaceTyping(QString()))
			setInputText(QString());
		m_partialCommand.clear();
		m_partialIndex = -1;
		return;
	}

	const QString candidate = m_history.at(index);
	if (!confirmReplaceTyping(candidate))
		return;

	m_partialIndex = index;
	setInputText(candidate);
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
	resetTabCompletionCycle();
}

void WorldView::resetTabCompletionCycle()
{
	m_tabCompletionCycleTargetLower.clear();
	m_tabCompletionCycleStartColumn = -1;
	m_tabCompletionCycleEndColumn   = -1;
	m_tabCompletionCycleLastSource  = -2;
	m_tabCompletionCycleActive      = false;
	m_tabCompletionCycleSeenCompletions.clear();
}

bool WorldView::tabCompleteOneLine(int startColumn, int endColumn, const QString &targetWordLower,
                                   const QString &line, bool insertSpace, QString *appliedCompletion,
                                   const QSet<QString> *skipCanonicalCompletions)
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
			const QString canonicalCompletion = replacement.toCaseFolded();
			if (skipCanonicalCompletions && skipCanonicalCompletions->contains(canonicalCompletion))
			{
				i = end;
				continue;
			}
			if (appliedCompletion)
				*appliedCompletion = replacement;
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

	QTextCursor cursor = m_input->textCursor();
	if (cursor.selectionStart() != cursor.selectionEnd())
	{
		resetTabCompletionCycle();
		return false;
	}

	const QString currentText = m_input->toPlainText();
	if (currentText.isEmpty())
	{
		resetTabCompletionCycle();
		return false;
	}

	int     endColumn   = cursor.position();
	int     startColumn = -1;
	int     lastSource  = -2;
	QString targetWordLower;

	bool    continueCycle = m_tabCompletionCycleActive;
	if (continueCycle)
	{
		const bool cycleRangeValid = m_tabCompletionCycleStartColumn >= 0 &&
		                             m_tabCompletionCycleEndColumn > m_tabCompletionCycleStartColumn &&
		                             m_tabCompletionCycleEndColumn <= currentText.size();
		if (!cycleRangeValid || endColumn != m_tabCompletionCycleEndColumn)
			continueCycle = false;
	}

	if (continueCycle)
	{
		startColumn     = m_tabCompletionCycleStartColumn;
		endColumn       = m_tabCompletionCycleEndColumn;
		lastSource      = m_tabCompletionCycleLastSource;
		targetWordLower = m_tabCompletionCycleTargetLower;
		if (targetWordLower.isEmpty())
		{
			resetTabCompletionCycle();
			return false;
		}
	}
	else
	{
		m_tabCompletionCycleSeenCompletions.clear();
		if (endColumn <= 0 || endColumn > currentText.size())
		{
			resetTabCompletionCycle();
			return false;
		}

		startColumn = endColumn - 1;
		while (startColumn >= 0)
		{
			if (const QChar ch = currentText.at(startColumn); ch.isSpace() || m_wordDelimiters.contains(ch))
				break;
			--startColumn;
		}
		++startColumn;

		if (startColumn >= endColumn)
		{
			resetTabCompletionCycle();
			return false;
		}

		targetWordLower = currentText.mid(startColumn, endColumn - startColumn).toLower();
		if (targetWordLower.isEmpty())
		{
			resetTabCompletionCycle();
			return false;
		}
	}

	bool insertSpace = m_tabCompletionSpace;
	if (endColumn < currentText.size())
	{
		if (const QChar next = currentText.at(endColumn); !next.isSpace() && !m_wordDelimiters.contains(next))
			insertSpace = true;
	}

	bool    completionApplied = false;
	int     matchedSource     = -2;
	QString matchedCompletion;

	if (!continueCycle &&
	    tabCompleteOneLine(startColumn, endColumn, targetWordLower, m_tabCompletionDefaults, insertSpace,
	                       &matchedCompletion, &m_tabCompletionCycleSeenCompletions))
	{
		completionApplied = true;
		matchedSource     = -1;
	}

	if (!completionApplied)
	{
		if (!m_runtime)
		{
			resetTabCompletionCycle();
			return false;
		}

		const QVector<WorldRuntime::LineEntry> &lines     = m_runtime->lines();
		int                                     startLine = sizeToInt(lines.size()) - 1;
		if (continueCycle)
		{
			if (lastSource >= 0)
				startLine = lastSource - 1;
			else if (lastSource == -1)
				startLine = sizeToInt(lines.size()) - 1;
		}

		int scanned = 0;
		for (int i = startLine; i >= 0; --i)
		{
			if (++scanned > m_tabCompletionLines)
				break;
			if (!tabCompleteOneLine(startColumn, endColumn, targetWordLower, lines.at(i).text, insertSpace,
			                        &matchedCompletion, &m_tabCompletionCycleSeenCompletions))
				continue;
			completionApplied = true;
			matchedSource     = i;
			break;
		}
	}

	if (!completionApplied)
	{
		resetTabCompletionCycle();
		return false;
	}

	cursor = m_input->textCursor();
	if (!matchedCompletion.isEmpty())
		m_tabCompletionCycleSeenCompletions.insert(matchedCompletion.toCaseFolded());
	m_tabCompletionCycleTargetLower = targetWordLower;
	m_tabCompletionCycleStartColumn = startColumn;
	m_tabCompletionCycleEndColumn   = cursor.position();
	m_tabCompletionCycleLastSource  = matchedSource;
	m_tabCompletionCycleActive      = true;
	return true;
}

void InputTextEdit::keyPressEvent(QKeyEvent *event)
{
	const bool plainTab = event->key() == Qt::Key_Tab && event->modifiers() == Qt::NoModifier;
	if (m_view && !plainTab)
		m_view->resetTabCompletionCycle();

#ifdef Q_OS_WIN
	const bool isKeypadDigit = (event->modifiers() & Qt::KeypadModifier) != 0 && event->key() >= Qt::Key_0 &&
	                           event->key() <= Qt::Key_9;
	const bool windowsAltNumpadDigitKey =
	    (event->modifiers() & Qt::AltModifier) != 0 &&
	    (isKeypadDigit || windowsNumpadDigitVirtualKey(event->nativeVirtualKey()));
	if (m_suppressNextAltNumpadCommit)
	{
		const Qt::KeyboardModifiers modifiers = event->modifiers();
		const bool                  plainPrintable =
		    (modifiers & (Qt::ControlModifier | Qt::MetaModifier)) == 0 && !event->text().isEmpty();
		if (plainPrintable)
		{
			m_suppressNextAltNumpadCommit = false;
			event->accept();
			return;
		}
		if (event->key() != Qt::Key_Shift && event->key() != Qt::Key_Control && event->key() != Qt::Key_Alt &&
		    event->key() != Qt::Key_Meta && event->key() != Qt::Key_AltGr)
		{
			m_suppressNextAltNumpadCommit = false;
		}
	}
#endif

	if (m_view && event->matches(QKeySequence::Copy) && m_view->hasOutputSelection())
	{
		m_view->copySelection();
		return;
	}

	if (m_view && m_view->m_allTypingToCommandWindow)
	{
		const Qt::KeyboardModifiers modifiers = event->modifiers();
		const bool ctrlShiftOnlyScroll        = (modifiers & Qt::ControlModifier) != 0 &&
		                                        (modifiers & Qt::ShiftModifier) != 0 &&
		                                        (modifiers & (Qt::AltModifier | Qt::MetaModifier)) == 0;
		auto       topScrollBar               = [this]() -> QScrollBar *
		{
			if (!m_view || !m_view->m_output)
				return nullptr;
			return m_view->m_output->verticalScrollBar();
		};

		bool handled = true;
		switch (event->key())
		{
		case Qt::Key_PageUp:
			m_view->setScrollbackSplitActive(true);
			if (QScrollBar *const topBar = topScrollBar())
				topBar->setValue(topBar->value() - topBar->pageStep());
			else
				handled = false;
			break;
		case Qt::Key_PageDown:
			if (QScrollBar *const topBar = topScrollBar())
			{
				topBar->setValue(topBar->value() + topBar->pageStep());
				if (m_view->m_scrollbackSplitActive && topBar->value() >= topBar->maximum())
					m_view->setScrollbackSplitActive(false);
			}
			else
				handled = false;
			break;
		case Qt::Key_Home:
			if (ctrlShiftOnlyScroll)
			{
				m_view->setScrollbackSplitActive(true);
				if (QScrollBar *const topBar = topScrollBar())
				{
					topBar->setValue(topBar->minimum());
				}
			}
			else
			{
				handled = false;
			}
			break;
		case Qt::Key_End:
			if (ctrlShiftOnlyScroll)
			{
				m_view->setScrollbackSplitActive(false);
				m_view->scrollOutputToEnd();
			}
			else
			{
				handled = false;
			}
			break;
		default:
			handled = false;
			break;
		}

		if (handled)
		{
			m_view->noteUserScrollAction();
			m_view->requestNativeOutputRepaint();
			return;
		}
	}

	if (m_view && m_view->handleWorldHotkey(event))
	{
#ifdef Q_OS_WIN
		if (windowsAltNumpadDigitKey)
			m_suppressNextAltNumpadCommit = true;
#endif
		return;
	}

	if (m_view && plainTab)
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
			const QString scriptPrefix  = attrs.value(QStringLiteral("script_prefix"));
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
		m_view->pasteCommand(pasteText);
		return;
	}
	if (m_view && event->key() == Qt::Key_Backspace && (event->modifiers() & Qt::ControlModifier))
	{
		clear();
		m_view->m_inputChanged = false;
		m_view->resetHistoryRecall();
		return;
	}

	if (m_view && event->key() == Qt::Key_Escape)
	{
		if (m_view->isScrollbackSplitActive())
		{
			m_view->collapseScrollbackSplitToLiveOutput();
			return;
		}
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
		bool      handled   = false;
		if (event->modifiers() == Qt::NoModifier)
		{
			if (m_view->m_arrowsChangeHistory)
			{
				if (m_view->m_arrowRecallsPartial &&
				    (m_view->m_inputChanged || !m_view->m_partialCommand.isEmpty()))
					m_view->recallPartialHistory(direction);
				else
					m_view->recallHistory(direction);
				handled = true;
			}
		}
		else if (event->modifiers() == Qt::AltModifier)
		{
			if (m_view->m_altArrowRecallsPartial || m_view->m_arrowRecallsPartial)
			{
				m_view->recallPartialHistory(direction);
				handled = true;
			}
		}
		if (handled)
		{
			return;
		}
	}

	QPlainTextEdit::keyPressEvent(event);
	if (m_view)
		m_view->requestInputViewportSync();
}

void InputTextEdit::mousePressEvent(QMouseEvent *event)
{
	QPlainTextEdit::mousePressEvent(event);
	if (m_view)
		m_view->requestInputViewportSync();
}

void InputTextEdit::mouseReleaseEvent(QMouseEvent *event)
{
	QPlainTextEdit::mouseReleaseEvent(event);
	if (m_view)
		m_view->requestInputViewportSync();
}

bool InputTextEdit::event(QEvent *event)
{
#ifdef Q_OS_WIN
	if (m_suppressNextAltNumpadCommit && event->type() == QEvent::InputMethod)
	{
		if (const auto *ime = dynamic_cast<QInputMethodEvent *>(event); ime && !ime->commitString().isEmpty())
		{
			m_suppressNextAltNumpadCommit = false;
			event->accept();
			return true;
		}
	}
	if (event->type() == QEvent::FocusOut)
		m_suppressNextAltNumpadCommit = false;
#endif

	if (m_view && event->type() == QEvent::ShortcutOverride)
	{
		if (const auto *keyEvent = dynamic_cast<QKeyEvent *>(event))
		{
			const Qt::KeyboardModifiers modifiers = keyEvent->modifiers();
			const bool                  plainCtrl = (modifiers & Qt::ControlModifier) != 0 &&
			                                        (modifiers & (Qt::AltModifier | Qt::MetaModifier)) == 0;
			const bool                  isCtrlBackspace = plainCtrl && keyEvent->key() == Qt::Key_Backspace;
			const bool                  isCopyShortcut =
			    keyEvent->matches(QKeySequence::Copy) || (plainCtrl && keyEvent->key() == Qt::Key_C);
			if (isCtrlBackspace)
			{
				event->accept();
				return true;
			}
			if (isCopyShortcut && m_view->hasOutputSelection())
			{
				event->accept();
				return true;
			}
			// Claim these before window-level QActions (Undo/Print/New) steal them when compat bindings are on
			const bool ctrlNoShift = plainCtrl && !(modifiers & Qt::ShiftModifier);
			if (ctrlNoShift &&
			    ((keyEvent->key() == Qt::Key_Z && m_view->m_ctrlZGoesToEndOfBuffer) ||
			     (keyEvent->key() == Qt::Key_P && m_view->m_ctrlPGoesToPreviousCommand) ||
			     (keyEvent->key() == Qt::Key_N && m_view->m_ctrlNGoesToNextCommand)))
			{
				event->accept();
				return true;
			}
			if (m_view->hasWorldAcceleratorBinding(keyEvent))
			{
				event->accept();
				return true;
			}
		}
	}

	return QPlainTextEdit::event(event);
}

void InputTextEdit::resizeEvent(QResizeEvent *event)
{
	QPlainTextEdit::resizeEvent(event);
	if (m_view)
		m_view->requestInputViewportSync();
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
					        if (m_view)
					        {
						        m_view->pasteCommand(pasteText);
					        }
					        else
					        {
						        insertPlainText(pasteText);
						        ensureCursorVisibleNowAndQueued(this);
					        }
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
