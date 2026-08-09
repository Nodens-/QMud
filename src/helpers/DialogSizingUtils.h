/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: DialogSizingUtils.h
 * Role: Shared helpers for constraining top-level dialog client geometry to the active screen.
 */

#ifndef QMUD_DIALOG_SIZING_UTILS_H
#define QMUD_DIALOG_SIZING_UTILS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QGuiApplication>
#include <QMargins>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QWidget>
#include <QWindow>

namespace DialogSizingUtils
{
	/**
	 * @brief Calculates the desired dialog size following a change in its global content requirement.
	 * @param sizeBeforeChange Dialog size captured before the sizing-triggering change.
	 * @param lastAppliedSize Last client size applied automatically by the sizing code.
	 * @param previousDesiredSize Unclamped desired client size retained from the previous calculation.
	 * @param previousContentMinimum Content requirement measured for the previous application font.
	 * @param currentContentMinimum Content requirement measured for the current application font.
	 * @return Unclamped size preserving user-added space even when an intermediate size was screen-bounded.
	 */
	[[nodiscard]] inline QSize desiredClientSizeForContentChange(const QSize sizeBeforeChange,
	                                                             const QSize lastAppliedSize,
	                                                             const QSize previousDesiredSize,
	                                                             const QSize previousContentMinimum,
	                                                             const QSize currentContentMinimum)
	{
		const bool hasPreviousMeasurement = previousContentMinimum.isValid();
		const bool canRetainPreviousDesired =
		    hasPreviousMeasurement && lastAppliedSize.isValid() && previousDesiredSize.isValid();
		const int baseWidth = canRetainPreviousDesired && sizeBeforeChange.width() == lastAppliedSize.width()
		                          ? previousDesiredSize.width()
		                          : sizeBeforeChange.width();
		const int baseHeight =
		    canRetainPreviousDesired && sizeBeforeChange.height() == lastAppliedSize.height()
		        ? previousDesiredSize.height()
		        : sizeBeforeChange.height();
		auto adjustedDimension =
		    [hasPreviousMeasurement](const int before, const int previous, const int current)
		{
			const qint64 delta =
			    hasPreviousMeasurement ? static_cast<qint64>(current) - static_cast<qint64>(previous) : 0;
			const qint64 adjusted = static_cast<qint64>(before) + delta;
			const qint64 required = qMax<qint64>(1, current);
			return static_cast<int>(qMin<qint64>(QWIDGETSIZE_MAX, qMax(required, adjusted)));
		};
		return {
		    adjustedDimension(baseWidth, previousContentMinimum.width(), currentContentMinimum.width()),
		    adjustedDimension(baseHeight, previousContentMinimum.height(), currentContentMinimum.height())};
	}

	/**
	 * @brief Merges native frame margins with independently observed total frame overhead.
	 * @param nativeMargins Per-edge margins reported by the native window system.
	 * @param observedOverhead Total horizontal and vertical overhead observed from widget geometry.
	 * @return Non-negative margins whose totals cover both sources without counting either source twice.
	 *
	 * Geometry differences reliably describe the total frame overhead, but their apparent per-edge
	 * distribution is platform-dependent for top-level widgets. Any overhead missing from the native
	 * totals is therefore assigned to the trailing edge solely for size accounting.
	 */
	[[nodiscard]] inline QMargins mergedFrameMarginsForObservedOverhead(const QMargins nativeMargins,
	                                                                    const QSize    observedOverhead)
	{
		QMargins     merged(qMax(0, nativeMargins.left()), qMax(0, nativeMargins.top()),
		                    qMax(0, nativeMargins.right()), qMax(0, nativeMargins.bottom()));
		const qint64 nativeHorizontal = static_cast<qint64>(merged.left()) + merged.right();
		const qint64 nativeVertical   = static_cast<qint64>(merged.top()) + merged.bottom();
		const qint64 missingHorizontal =
		    qMax<qint64>(0, static_cast<qint64>(qMax(0, observedOverhead.width())) - nativeHorizontal);
		const qint64 missingVertical =
		    qMax<qint64>(0, static_cast<qint64>(qMax(0, observedOverhead.height())) - nativeVertical);
		merged.setRight(static_cast<int>(static_cast<qint64>(merged.right()) + missingHorizontal));
		merged.setBottom(static_cast<int>(static_cast<qint64>(merged.bottom()) + missingVertical));
		return merged;
	}

	/**
	 * @brief Calculates the largest client size for an available screen size and frame margins.
	 * @param availableSize Available screen work-area size.
	 * @param frameMargins Non-client frame margins surrounding the client area.
	 * @return Largest positive client size that keeps the complete frame inside the work area.
	 */
	[[nodiscard]] inline QSize maximumClientSizeForAvailableSize(const QSize    availableSize,
	                                                             const QMargins frameMargins)
	{
		const qint64 horizontalFrame =
		    static_cast<qint64>(qMax(0, frameMargins.left())) + qMax(0, frameMargins.right());
		const qint64 verticalFrame =
		    static_cast<qint64>(qMax(0, frameMargins.top())) + qMax(0, frameMargins.bottom());
		const qint64 clientWidth =
		    qMax<qint64>(1, static_cast<qint64>(availableSize.width()) - horizontalFrame);
		const qint64 clientHeight =
		    qMax<qint64>(1, static_cast<qint64>(availableSize.height()) - verticalFrame);
		return {static_cast<int>(qMin<qint64>(clientWidth, QWIDGETSIZE_MAX)),
		        static_cast<int>(qMin<qint64>(clientHeight, QWIDGETSIZE_MAX))};
	}

	/**
	 * @brief Calculates the height available to a dialog's central content region.
	 * @param maximumClientHeight Largest client height available on the active screen.
	 * @param layoutMinimumHeight Current minimum height of the complete dialog layout.
	 * @param centralMinimumHeight Current minimum height contributed by the central region.
	 * @return Positive central-region height that retains the surrounding dialog chrome.
	 */
	[[nodiscard]] inline int maximumCentralHeightForLayout(const int maximumClientHeight,
	                                                       const int layoutMinimumHeight,
	                                                       const int centralMinimumHeight)
	{
		const qint64 surroundingHeight =
		    qMax<qint64>(0, static_cast<qint64>(layoutMinimumHeight) - centralMinimumHeight);
		const qint64 availableHeight = static_cast<qint64>(maximumClientHeight) - surroundingHeight;
		return static_cast<int>(qMin<qint64>(QWIDGETSIZE_MAX, qMax<qint64>(1, availableHeight)));
	}

	/**
	 * @brief Returns the frame margins currently observable for a top-level widget.
	 * @param dialog Dialog whose native and QWidget frame geometry should be inspected.
	 * @return Non-negative margins whose totals cover both geometry sources without double-counting.
	 */
	[[nodiscard]] inline QMargins effectiveFrameMargins(const QWidget *dialog)
	{
		if (!dialog)
			return {};

		QMargins nativeMargins;
		if (const QWindow *window = dialog->windowHandle())
			nativeMargins = window->frameMargins();

		const QSize  clientSize = dialog->geometry().size();
		const QSize  frameSize  = dialog->frameGeometry().size();
		const qint64 observedWidth =
		    qMax<qint64>(0, static_cast<qint64>(frameSize.width()) - clientSize.width());
		const qint64 observedHeight =
		    qMax<qint64>(0, static_cast<qint64>(frameSize.height()) - clientSize.height());
		const QSize observedOverhead(static_cast<int>(qMin<qint64>(observedWidth, QWIDGETSIZE_MAX)),
		                             static_cast<int>(qMin<qint64>(observedHeight, QWIDGETSIZE_MAX)));
		return mergedFrameMarginsForObservedOverhead(nativeMargins, observedOverhead);
	}

	/**
	 * @brief Constrains a frame's top-left position to an available screen rectangle.
	 * @param available Available screen work-area rectangle, including its global origin.
	 * @param frame Current complete window-frame rectangle.
	 * @return Closest top-left position that contains the frame, or the work-area origin when the frame is oversized.
	 */
	[[nodiscard]] inline QPoint boundedFrameTopLeft(const QRect available, const QRect frame)
	{
		const int maximumLeft = available.right() - frame.width() + 1;
		const int maximumTop  = available.bottom() - frame.height() + 1;
		const int boundedLeft = maximumLeft < available.left()
		                            ? available.left()
		                            : qBound(available.left(), frame.left(), maximumLeft);
		const int boundedTop =
		    maximumTop < available.top() ? available.top() : qBound(available.top(), frame.top(), maximumTop);
		return {boundedLeft, boundedTop};
	}

	/**
	 * @brief Returns the largest client size that fits on the dialog's active screen.
	 * @param dialog Dialog whose screen and frame margins should be used.
	 * @return Available client size, or the QWidget maximum when no screen is available.
	 */
	[[nodiscard]] inline QSize maximumClientSize(const QWidget *dialog)
	{
		const QScreen *screen = dialog ? dialog->screen() : QGuiApplication::primaryScreen();
		if (!screen)
			return {QWIDGETSIZE_MAX, QWIDGETSIZE_MAX};

		return maximumClientSizeForAvailableSize(screen->availableGeometry().size(),
		                                         effectiveFrameMargins(dialog));
	}

	/**
	 * @brief Applies a client size and keeps the complete window frame inside its active screen.
	 * @param dialog Dialog whose geometry should be constrained.
	 * @param requestedClientSize Requested client-area size.
	 */
	inline void applyAvailableGeometry(QWidget *dialog, const QSize requestedClientSize)
	{
		if (!dialog)
			return;

		const QScreen *const screen = dialog->screen();
		if (!screen)
		{
			dialog->resize(requestedClientSize.boundedTo({QWIDGETSIZE_MAX, QWIDGETSIZE_MAX}));
			return;
		}

		const QSize maximumSize = maximumClientSizeForAvailableSize(screen->availableGeometry().size(),
		                                                            effectiveFrameMargins(dialog));
		dialog->resize(requestedClientSize.boundedTo(maximumSize));

		const QRect  available  = screen->availableGeometry();
		const QRect  frame      = dialog->frameGeometry();
		const QPoint adjustment = boundedFrameTopLeft(available, frame) - frame.topLeft();
		if (!adjustment.isNull())
			dialog->move(dialog->pos() + adjustment);
	}
} // namespace DialogSizingUtils

#endif // QMUD_DIALOG_SIZING_UTILS_H
