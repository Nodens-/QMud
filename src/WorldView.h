/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldView.h
 * Role: World output-view interfaces for rendered text display, selection, and visual interaction within a world
 * window.
 */

#ifndef QMUD_WORLDVIEW_H
#define QMUD_WORLDVIEW_H

#include "WorldRuntime.h"
#include <QElapsedTimer>
#include <QFont>
#include <QPoint>
#include <QRegularExpression>
#include <QScopedPointer>
// ReSharper disable once CppUnusedIncludeDirective
#include <QSet>
#include <QString>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>
#include <QWidget>

class QPlainTextEdit;
class WrapTextBrowser;
class InputTextEdit;
class WorldRuntime;
class QSplitter;
class QScrollBar;
class QWidget;
class QTextDocument;
class QTimer;

/**
 * @brief Stateful options and cursor data for command-history find operations.
 */
struct CommandHistoryFindState
{
		QStringList        history;
		QString            title{QStringLiteral("Find in command history...")};
		QString            lastFindText;
		bool               matchCase{false};
		bool               forwards{false};
		bool               regexp{false};
		bool               again{false};
		int                currentLine{0};
		QRegularExpression regex;
};

/**
 * @brief Composite widget for world output, input, and interaction state.
 *
 * Renders styled lines, handles input editing/history, and forwards user
 * interactions to the world runtime/command processor.
 */
class WorldView : public QWidget
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates world output/input composite widget.
		 * @param parent Optional Qt parent widget.
		 */
		explicit WorldView(QWidget *parent = nullptr);
		/**
		 * @brief Destroys world view and owned UI resources.
		 */
		~WorldView() override;

		/**
		 * @brief Runtime binding, output append, and history helpers.
		 * @param name World display name.
		 */
		void                        setWorldName(const QString &name);
		/**
		 * @brief Binds runtime and installs active signal handlers.
		 * @param runtime Runtime instance to bind.
		 */
		void                        setRuntime(WorldRuntime *runtime);
		/**
		 * @brief Binds runtime observer hooks without ownership change.
		 * @param runtime Runtime instance to observe.
		 */
		void                        setRuntimeObserver(WorldRuntime *runtime);
		/**
		 * @brief Returns currently bound runtime.
		 * @return Bound runtime pointer, or `nullptr` when unbound.
		 */
		[[nodiscard]] WorldRuntime *runtime() const;
		/**
		 * @brief Enables/disables local no-echo mode in input UI.
		 * @param enabled Enable no-echo display mode when `true`.
		 */
		void                        setNoEcho(bool enabled);
		/**
		 * @brief Adds hyperlink text to command history.
		 * @param text Hyperlink text to append.
		 */
		void                        addHyperlinkToHistory(const QString &text);
		/**
		 * @brief Adds history entry bypassing duplicate checks.
		 * @param text Command text to append.
		 */
		void                        addToHistoryForced(const QString &text);
		/**
		 * @brief Freezes/unfreezes output scrolling/appends.
		 * @param frozen Freeze output when `true`.
		 */
		void                        setFrozen(bool frozen);
		/**
		 * @brief Returns frozen output state.
		 * @return `true` when output is currently frozen.
		 */
		[[nodiscard]] bool          isFrozen() const;
		/**
		 * @brief Appends plain output text.
		 * @param text Text to append.
		 * @param newLine Append newline after text when `true`.
		 */
		void                        appendOutputText(const QString &text, bool newLine = true);
		/**
		 * @brief Appends styled output text.
		 * @param text Text to append.
		 * @param spans Style spans aligned to `text`.
		 * @param newLine Append newline after text when `true`.
		 */
		void    appendOutputTextStyled(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans,
		                               bool newLine = true);
		/**
		 * @brief Appends note text line.
		 * @param text Text to append.
		 * @param newLine Append newline after text when `true`.
		 */
		void    appendNoteText(const QString &text, bool newLine = true);
		/**
		 * @brief Appends styled note text line.
		 * @param text Text to append.
		 * @param spans Style spans aligned to `text`.
		 * @param newLine Append newline after text when `true`.
		 */
		void    appendNoteTextStyled(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans,
		                             bool newLine = true);
		/**
		 * @brief Pastes command text using input rules and returns normalized text.
		 * @param text Raw pasted text.
		 * @return Normalized text that was inserted/sent.
		 */
		QString pasteCommand(const QString &text);
		/**
		 * @brief Pushes current input command to processing path.
		 * @return Command text that was dispatched.
		 */
		QString pushCommand();
		/**
		 * @brief Updates partial output line preview.
		 * @param text Partial line text.
		 * @param spans Style spans for the partial line.
		 */
		void updatePartialOutputText(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans = {});
		/**
		 * @brief Clears pending partial output line state.
		 */
		void clearPartialOutput();
		/**
		 * @brief Rebuilds visible output from stored line entries.
		 * @param lines Line entries to render.
		 */
		void rebuildOutputFromLines(const QVector<WorldRuntime::LineEntry> &lines);
		/**
		 * @brief Appends raw HTML fragment to output view.
		 * @param html HTML fragment to append.
		 * @param newLine Append newline after fragment when `true`.
		 */
		void appendOutputHtml(const QString &html, bool newLine = false);
		/**
		 * @brief Echoes user input text into output stream.
		 * @param text Input text to echo.
		 */
		void echoInputText(const QString &text);
		/**
		 * @brief Returns current output lines as plain strings.
		 * @return Plain-text output lines.
		 */
		[[nodiscard]] QStringList     outputLines() const;
		/**
		 * @brief Returns true when output is scrolled to end.
		 * @return `true` when output is at buffer end.
		 */
		[[nodiscard]] bool            isAtBufferEnd() const;
		/**
		 * @brief Selects entire output line by zero-based index.
		 * @param zeroBasedLine Zero-based line index.
		 */
		void                          selectOutputLine(int zeroBasedLine) const;
		/**
		 * @brief Performs output find/next operation.
		 * @param again Continue from previous find state when `true`.
		 * @return `true` when a match is found.
		 */
		bool                          doOutputFind(bool again);
		/**
		 * @brief Sets output find direction.
		 * @param forwards Search forward when `true`, backward otherwise.
		 */
		void                          setOutputFindDirection(bool forwards);
		/**
		 * @brief Returns true when output find history exists.
		 * @return `true` when output-find history exists.
		 */
		[[nodiscard]] bool            hasOutputFindHistory() const;
		/**
		 * @brief Opens command history dialog.
		 */
		void                          showCommandHistoryDialog();
		/**
		 * @brief Returns mutable command-history find state.
		 * @return Mutable command-history find state.
		 */
		CommandHistoryFindState      *commandHistoryFindState();
		/**
		 * @brief Sends selected command from history.
		 * @param text Command text selected from history.
		 */
		void                          sendCommandFromHistory(const QString &text);
		/**
		 * @brief Returns command history entries.
		 * @return Command history entries.
		 */
		[[nodiscard]] QStringList     commandHistoryList() const;
		/**
		 * @brief Replaces command history entries.
		 * @param historyEntries Restored command history entries.
		 */
		void                          setCommandHistoryList(const QStringList &historyEntries);
		/**
		 * @brief Clears command history buffer.
		 */
		void                          clearCommandHistory();
		/**
		 * @brief Returns true when command history has entries.
		 * @return `true` when command history is non-empty.
		 */
		[[nodiscard]] bool            hasCommandHistory() const;
		/**
		 * @brief Enables/disables background bleed mode.
		 * @param enabled Enable bleed mode when `true`.
		 */
		void                          setBleedBackground(bool enabled);
		/**
		 * @brief Returns input selection start column.
		 * @return Input selection start column.
		 */
		[[nodiscard]] int             inputSelectionStartColumn() const;
		/**
		 * @brief Returns input selection end column.
		 * @return Input selection end column.
		 */
		[[nodiscard]] int             inputSelectionEndColumn() const;
		/**
		 * @brief Returns input editor widget pointer.
		 * @return Input editor widget.
		 */
		[[nodiscard]] QPlainTextEdit *inputEditor() const;
		/**
		 * @brief Scrolls output to beginning.
		 */
		void                          scrollOutputToStart() const;
		/**
		 * @brief Scrolls output to end.
		 */
		void                          scrollOutputToEnd() const;
		/**
		 * @brief Scrolls output one page up.
		 */
		void                          scrollOutputPageUp() const;
		/**
		 * @brief Scrolls output one page down.
		 */
		void                          scrollOutputPageDown() const;
		/**
		 * @brief Focuses input editor.
		 */
		void                          focusInput() const;
		/**
		 * @brief Scrolls output one line up.
		 */
		void                          scrollOutputLineUp() const;
		/**
		 * @brief Scrolls output one line down.
		 */
		void                          scrollOutputLineDown() const;
		/**
		 * @brief Recalls next command in history navigation.
		 */
		void                          recallNextCommand();
		/**
		 * @brief Recalls previous command in history navigation.
		 */
		void                          recallPreviousCommand();
		/**
		 * @brief Repeats last submitted command.
		 */
		void                          repeatLastCommand();
		/**
		 * @brief Recalls last word from history context.
		 */
		void                          recallLastWord();
		/**
		 * @brief Requests redraw/update for active miniwindows.
		 */
		void                          refreshMiniWindows() const;
		/**
		 * @brief Converts miniwindow-local point to global coordinates.
		 * @param window Miniwindow whose local coordinates are used.
		 * @param x Local x coordinate.
		 * @param y Local y coordinate.
		 * @return Corresponding global screen coordinate.
		 */
		QPoint                        miniWindowGlobalPosition(const MiniWindow *window, int x, int y) const;
		/**
		 * @brief Shows generic context menu at global position.
		 * @param globalPos Global screen position.
		 * @return `true` when a menu action was handled.
		 */
		bool                          showContextMenuAtGlobalPos(const QPoint &globalPos);
		/**
		 * @brief Shows world context menu at global position.
		 * @param globalPos Global screen position.
		 * @return `true` when a menu action was handled.
		 */
		bool                          showWorldContextMenuAtGlobalPos(const QPoint &globalPos);
		/**
		 * @brief Replays right-click event against miniwindow hotspots.
		 * @param globalPos Global click position.
		 * @return `true` when a hotspot handled the click.
		 */
		bool                          replayMiniWindowRightClickAtGlobalPos(const QPoint &globalPos);
		/**
		 * @brief Handles world hotkey keypress.
		 * @param event Key event to process.
		 * @return `true` when the hotkey is consumed.
		 */
		bool                          handleWorldHotkey(QKeyEvent *event);
		/**
		 * @brief Returns true when miniwindow mouse capture is active.
		 * @return `true` when miniwindow mouse capture is active.
		 */
		[[nodiscard]] bool            isMiniWindowCaptureActive() const;
		/**
		 * @brief Returns true when last mouse position is known.
		 * @return `true` when last mouse position is available.
		 */
		[[nodiscard]] bool            hasLastMousePosition() const;
		/**
		 * @brief Returns cached last mouse position.
		 * @return Cached last mouse position.
		 */
		[[nodiscard]] QPoint          lastMousePosition() const;
		/**
		 * @brief Re-evaluates miniwindow hover state.
		 */
		void                          recheckMiniWindowHover();
		/**
		 * @brief Returns true when scrollback split view is active.
		 * @return `true` when split view is active.
		 */
		[[nodiscard]] bool            isScrollbackSplitActive() const;
		/**
		 * @brief Collapses split output view back to live-only output.
		 */
		void                          collapseScrollbackSplitToLiveOutput();

	signals:
		/**
		 * @brief Emitted when user text should be sent.
		 * @param text Text to send.
		 */
		void sendText(const QString &text);
		/**
		 * @brief Emitted when hyperlink is activated.
		 * @param href Hyperlink target.
		 */
		void hyperlinkActivated(const QString &href);
		/**
		 * @brief Emitted when hyperlink hover/selection changes.
		 * @param href Hyperlink target.
		 */
		void hyperlinkHighlighted(const QString &href);
		/**
		 * @brief Emitted when output selection changes.
		 */
		void outputSelectionChanged();
		/**
		 * @brief Emitted when input selection changes.
		 */
		void inputSelectionChanged();
		/**
		 * @brief Emitted when output scroll position changes.
		 */
		void outputScrollChanged();
		/**
		 * @brief Emitted when frozen state toggles.
		 * @param frozen New frozen state.
		 */
		void freezeStateChanged(bool frozen);

	protected:
		/**
		 * @brief Qt event handlers for size/show/mouse filtering.
		 * @param event Resize event payload.
		 */
		void resizeEvent(QResizeEvent *event) override;
		/**
		 * @brief Handles widget show lifecycle.
		 * @param event Show event payload.
		 */
		void showEvent(QShowEvent *event) override;
		/**
		 * @brief Handles mouse move events for output/miniwindows.
		 * @param event Mouse move event payload.
		 */
		void mouseMoveEvent(QMouseEvent *event) override;
		/**
		 * @brief Handles mouse release for output/miniwindows.
		 * @param event Mouse release event payload.
		 */
		void mouseReleaseEvent(QMouseEvent *event) override;
		/**
		 * @brief Filters child-widget events used by output/input stack.
		 * @param watched Object receiving the event.
		 * @param event Event payload.
		 * @return `true` when the event is consumed.
		 */
		bool eventFilter(QObject *watched, QEvent *event) override;

	public:
		friend class InputTextEdit;
		friend class WrapTextBrowser;
		friend class MiniWindowLayer;
		/**
		 * @brief Extended editing/selection/output APIs used by runtime and widgets.
		 */
		void                  applyRuntimeSettings();
		/**
		 * @brief Applies only the runtime output line limit to the output document.
		 */
		void                  applyMaxOutputLinesSetting() const;
		/**
		 * @brief Adds command text to history buffer.
		 * @param text Command text to add.
		 */
		void                  addToHistory(const QString &text);
		/**
		 * @brief Returns whether output currently has a selection.
		 * @return `true` when output has a selection.
		 */
		[[nodiscard]] bool    hasOutputSelection() const;
		/**
		 * @brief Returns true when input editor has a selection.
		 * @return `true` when input has a selection.
		 */
		[[nodiscard]] bool    hasInputSelection() const;
		/**
		 * @brief Returns whether cursor is currently hovering a hyperlink anchor.
		 * @return `true` when output cursor is over a hyperlink anchor.
		 */
		[[nodiscard]] bool    hyperlinkHoverActive() const;
		/**
		 * @brief Returns selected output text.
		 * @return Selected output text.
		 */
		[[nodiscard]] QString outputSelectionText() const;
		/**
		 * @brief Returns selected input text.
		 * @return Selected input text.
		 */
		[[nodiscard]] QString inputSelectionText() const;
		/**
		 * @brief Returns output as plain text.
		 * @return Output plain text.
		 */
		[[nodiscard]] QString outputPlainText() const;
		/**
		 * @brief Returns output selected text with formatting rules applied.
		 * @return Formatted selected output text.
		 */
		[[nodiscard]] QString outputSelectedText() const;
		/**
		 * @brief Copies active selection to clipboard.
		 */
		void                  copySelection() const;
		/**
		 * @brief Copies active selection as HTML to clipboard.
		 */
		void                  copySelectionAsHtml() const;
		/**
		 * @brief Clears output buffer and line model.
		 */
		void                  clearOutputBuffer();
		/**
		 * @brief Returns word under output cursor.
		 * @return Word under current output cursor.
		 */
		[[nodiscard]] QString wordUnderCursor() const;
		/**
		 * @brief Sets delimiter sets for word selection/parsing.
		 * @param delimiters Primary word delimiters.
		 * @param doubleClickDelimiters Word delimiters used on double-click selection.
		 */
		void setWordDelimiters(const QString &delimiters, const QString &doubleClickDelimiters);
		/**
		 * @brief Sets smooth scrolling modes.
		 * @param smooth Enable smooth scrolling when `true`.
		 * @param smoother Enable smoother scrolling variant when `true`.
		 */
		void setSmoothScrolling(bool smooth, bool smoother);
		/**
		 * @brief Routes all typing to command window when enabled.
		 * @param enabled Route all typing to command window when `true`.
		 */
		void setAllTypingToCommandWindow(bool enabled);
		/**
		 * @brief Removes last history entry.
		 */
		void removeLastHistoryEntry();
		/**
		 * @brief Returns current input text.
		 * @return Current input text.
		 */
		[[nodiscard]] QString inputText() const;
		/**
		 * @brief Recalls command history by direction.
		 * @param direction Navigation direction.
		 */
		void                  recallHistory(int direction);
		/**
		 * @brief Recalls partial history matches by direction.
		 * @param direction Navigation direction.
		 */
		void                  recallPartialHistory(int direction);
		/**
		 * @brief Resets history recall cursor/state.
		 */
		void                  resetHistoryRecall();
		/**
		 * @brief Confirms replacing current typing with suggested text.
		 * @param replacement Replacement text candidate.
		 * @return `true` when replacement is approved.
		 */
		bool                  confirmReplaceTyping(const QString &replacement);
		/**
		 * @brief Executes macro by configured macro name.
		 * @param name Macro name.
		 * @return `true` when macro executes successfully.
		 */
		bool                  executeMacroByName(const QString &name);
		/**
		 * @brief Handles tab-completion key press.
		 * @return `true` when key press is consumed by completion.
		 */
		bool                  handleTabCompletionKeyPress();
		/**
		 * @brief Sets input text and optionally marks as changed.
		 * @param text Input text.
		 * @param markChanged Mark input as changed when `true`.
		 */
		void                  setInputText(const QString &text, bool markChanged = false);
		/**
		 * @brief Sets command selection range and returns applied end index.
		 * @param first Selection start index.
		 * @param last Selection end index.
		 * @return Applied selection end index.
		 */
		[[nodiscard]] int     setCommandSelection(int first, int last) const;
		/**
		 * @brief Sets command window height and returns applied value.
		 * @param height Requested command window height.
		 * @return Applied command window height.
		 */
		[[nodiscard]] int     setCommandWindowHeight(int height) const;
		/**
		 * @brief Applies world cursor style code.
		 * @param cursorCode Cursor style code.
		 */
		void                  setWorldCursor(int cursorCode);
		/**
		 * @brief Recomputes output wrap margin.
		 */
		void                  updateWrapMargin() const;
		/**
		 * @brief Recomputes input wrapping mode/width.
		 */
		void                  updateInputWrap() const;
		/**
		 * @brief Recomputes input widget height.
		 */
		void                  updateInputHeight() const;
		/**
		 * @brief Completes a word in one line.
		 * @param startColumn Completion start column.
		 * @param endColumn Completion end column.
		 * @param targetWordLower Lowercased target word.
		 * @param line Source line text.
		 * @param insertSpace Insert trailing space after completion when `true`.
		 * @return `true` when completion changed the line.
		 */
		bool tabCompleteOneLine(int startColumn, int endColumn, const QString &targetWordLower,
		                        const QString &line, bool insertSpace);
		/**
		 * @brief Applies default command-input height policy.
		 * @param setSplitterSizes Apply splitter sizing when `true`.
		 */
		void applyDefaultInputHeight(bool setSplitterSizes);
		/**
		 * @brief Selects output range on a single line.
		 * @param zeroBasedLine Zero-based output line index.
		 * @param startColumn Selection start column.
		 * @param endColumn Selection end column.
		 */
		void selectOutputRange(int zeroBasedLine, int startColumn, int endColumn) const;
		/**
		 * @brief Sets output selection range across lines/columns.
		 * @param startLine Selection start line.
		 * @param endLine Selection end line.
		 * @param startColumn Selection start column.
		 * @param endColumn Selection end column.
		 */
		void setOutputSelection(int startLine, int endLine, int startColumn, int endColumn) const;
		/**
		 * @brief Sets output scroll position.
		 * @param position Target scroll position.
		 * @param visible Ensure position remains visible when `true`.
		 * @return Applied scroll position.
		 */
		int  setOutputScroll(int position, bool visible);
		/**
		 * @brief Returns output selection start line.
		 * @return Output selection start line.
		 */
		[[nodiscard]] int   outputSelectionStartLine() const;
		/**
		 * @brief Returns output selection end line.
		 * @return Output selection end line.
		 */
		[[nodiscard]] int   outputSelectionEndLine() const;
		/**
		 * @brief Returns output selection start column.
		 * @return Output selection start column.
		 */
		[[nodiscard]] int   outputSelectionStartColumn() const;
		/**
		 * @brief Returns output selection end column.
		 * @return Output selection end column.
		 */
		[[nodiscard]] int   outputSelectionEndColumn() const;
		/**
		 * @brief Returns output client height in pixels.
		 * @return Output client height in pixels.
		 */
		[[nodiscard]] int   outputClientHeight() const;
		/**
		 * @brief Returns output client width in pixels.
		 * @return Output client width in pixels.
		 */
		[[nodiscard]] int   outputClientWidth() const;
		/**
		 * @brief Returns effective output font.
		 * @return Effective output font.
		 */
		[[nodiscard]] QFont outputFont() const;
		/**
		 * @brief Appends output text with internal routing flags.
		 * @param text Text payload.
		 * @param newLine Append newline when `true`.
		 * @param recordLine Store line in buffer when `true`.
		 * @param flags Line flags.
		 * @param spans Style spans aligned to `text`.
		 */
		void          appendOutputTextInternal(const QString &text, bool newLine, bool recordLine, int flags,
		                                       const QVector<WorldRuntime::StyleSpan> &spans = {});
		/**
		 * @brief Parses color string/value into QColor.
		 * @param value Color value string.
		 * @return Parsed color value.
		 */
		static QColor parseColor(const QString &value);
		/**
		 * @brief Returns runtime attribute keys that affect world-view rendering/behavior.
		 * @return Set of world-view-relevant runtime attribute keys.
		 */
		[[nodiscard]] static const QSet<QString> &runtimeSettingsAttributeKeys();
		/**
		 * @brief Returns multiline runtime attribute keys that affect world-view behavior.
		 * @return Set of world-view-relevant multiline runtime attribute keys.
		 */
		[[nodiscard]] static const QSet<QString> &runtimeSettingsMultilineAttributeKeys();
		/**
		 * @brief Compares runtime attribute values using world-view semantic equivalence.
		 * @param key Runtime attribute key.
		 * @param before Value before edit.
		 * @param after Value after edit.
		 * @return `true` when values are equivalent for world-view behavior.
		 */
		[[nodiscard]] static bool runtimeSettingValuesEquivalent(const QString &key, const QString &before,
		                                                         const QString &after);
		/**
		 * @brief Compares multiline runtime attribute values for world-view behavior.
		 * @param key Multiline runtime attribute key.
		 * @param before Value before edit.
		 * @param after Value after edit.
		 * @return `true` when values are equivalent for world-view behavior.
		 */
		[[nodiscard]] static bool runtimeMultilineSettingValuesEquivalent(const QString &key,
		                                                                  const QString &before,
		                                                                  const QString &after);
		/**
		 * @brief Maps legacy font-weight value to Qt weight.
		 * @param weight Legacy font-weight value.
		 * @return Corresponding Qt font weight.
		 */
		static QFont::Weight      mapFontWeight(int weight);
		/**
		 * @brief Returns output scrollbar position.
		 * @return Output scrollbar position.
		 */
		[[nodiscard]] int         outputScrollPosition() const;
		/**
		 * @brief Returns true when output scrollbar is visible.
		 * @return `true` when output scrollbar is visible.
		 */
		[[nodiscard]] bool        outputScrollBarVisible() const;
		/**
		 * @brief Returns desired output scrollbar visibility setting.
		 * @return Desired output scrollbar visibility setting.
		 */
		[[nodiscard]] bool        outputScrollBarWanted() const;
		/**
		 * @brief Returns output text viewport rectangle.
		 * @return Output text viewport rectangle.
		 */
		[[nodiscard]] QRect       outputTextRectangle() const;

	private:
		/**
		 * @brief Miniwindow rendering/input hit-testing and output view internals.
		 * @param painter Painter used for miniwindow rendering.
		 * @param underneath Render underneath layer when `true`; overlay otherwise.
		 */
		void                 paintMiniWindows(class QPainter *painter, bool underneath) const;
		/**
		 * @brief Handles mouse wheel scrolling over output.
		 * @param angleDelta Wheel angle delta.
		 * @param pixelDelta Wheel pixel delta.
		 */
		void                 handleOutputWheel(const QPoint &angleDelta, const QPoint &pixelDelta);
		/**
		 * @brief Marks that user initiated a manual scroll action.
		 */
		void                 noteUserScrollAction();
		/**
		 * @brief Enables/disables scrollback split mode.
		 * @param active Activate split mode when `true`.
		 */
		void                 setScrollbackSplitActive(bool active);
		/**
		 * @brief Scrolls target output view to end.
		 * @param view Target output view.
		 */
		static void          scrollViewToEnd(const WrapTextBrowser *view);
		/**
		 * @brief Queues output scroll-to-end request.
		 */
		void                 requestOutputScrollToEnd();
		/**
		 * @brief Handles output selection changed callback.
		 */
		void                 handleOutputSelectionChanged();
		/**
		 * @brief Maps global point to output stack coordinates.
		 * @param globalPos Global screen coordinate.
		 * @return Output-stack-local coordinate.
		 */
		[[nodiscard]] QPoint mapEventToOutputStack(const QPoint &globalPos) const;
		/**
		 * @brief Maps source-local point to output stack coordinates.
		 * @param localPos Source-local coordinate.
		 * @param source Source widget.
		 * @return Output-stack-local coordinate.
		 */
		QPoint               mapEventToOutputStack(const QPointF &localPos, const QWidget *source) const;
		/**
		 * @brief Hit-tests miniwindow and hotspot at output-local position.
		 * @param localPos Output-local coordinate.
		 * @param hotspotId Output hotspot id when matched.
		 * @param windowName Output miniwindow name when matched.
		 * @param includeUnderneath Include underneath miniwindows when `true`.
		 * @return Matched miniwindow, or `nullptr`.
		 */
		MiniWindow *hitTestMiniWindow(const QPoint &localPos, QString &hotspotId, QString &windowName,
		                              bool includeUnderneath = false) const;
		/**
		 * @brief Handles miniwindow mouse-move event.
		 * @param event Mouse event payload.
		 * @param source Source widget.
		 * @return `true` when event is consumed.
		 */
		bool        handleMiniWindowMouseMove(const QMouseEvent *event, const QWidget *source);
		/**
		 * @brief Returns hyperlink currently under global cursor, if any.
		 * @return Hyperlink href under cursor, or empty when none.
		 */
		[[nodiscard]] QString currentHoveredHyperlink() const;
		/**
		 * @brief Applies hovered-hyperlink state and emits change when needed.
		 * @param href Hyperlink href under cursor (or empty).
		 */
		void                  applyHoveredHyperlink(const QString &href);
		/**
		 * @brief Refreshes hovered-hyperlink state from current cursor position.
		 */
		void                  refreshHoveredHyperlinkFromCursor();
		/**
		 * @brief Handles miniwindow mouse-press event.
		 * @param event Mouse event payload.
		 * @param doubleClick `true` when event is a double click.
		 * @param source Source widget.
		 * @return `true` when event is consumed.
		 */
		bool handleMiniWindowMousePress(const QMouseEvent *event, bool doubleClick, const QWidget *source);
		/**
		 * @brief Handles miniwindow mouse-release event.
		 * @param event Mouse event payload.
		 * @param source Source widget.
		 * @return `true` when event is consumed.
		 */
		bool handleMiniWindowMouseRelease(const QMouseEvent *event, const QWidget *source);
		/**
		 * @brief Handles miniwindow mouse-wheel event.
		 * @param event Wheel event payload.
		 * @param source Source widget.
		 * @return `true` when event is consumed.
		 */
		bool handleMiniWindowWheel(const QWheelEvent *event, const QWidget *source) const;
		/**
		 * @brief Handles synthetic mouse-leave for miniwindow layer.
		 * @return `true` when leave event handling changed hover/capture state.
		 */
		bool handleMiniWindowMouseLeave();
		/**
		 * @brief Invokes hotspot callback function for window/plugin.
		 * @param window Target miniwindow.
		 * @param hotspotId Hotspot identifier.
		 * @param callbackName Callback function name.
		 * @param flags Callback mouse/keyboard flags.
		 */
		void callHotspotCallback(MiniWindow *window, const QString &hotspotId, const QString &callbackName,
		                         int flags) const;
		/**
		 * @brief Clears currently applied hotspot cursor override.
		 */
		void clearHotspotCursor();
		/**
		 * @brief Applies cursor to output surface.
		 * @param cursor Cursor to apply, or `nullptr` to clear override.
		 */
		void applyOutputCursor(const QCursor *cursor);
		/**
		 * @brief Cancels mouse-over state for hotspot/window.
		 * @param window Miniwindow to clear hover state from.
		 * @param hotspotId Hovered hotspot identifier.
		 */
		void cancelMouseOver(MiniWindow *window, const QString &hotspotId);
		/**
		 * @brief Computes MFC-compatible mouse flags for hotspot callbacks.
		 * @param event Mouse event payload.
		 * @param doubleClick `true` when event is a double click.
		 * @param baseFlags Base flags to merge.
		 * @return Combined MFC-compatible mouse flags.
		 */
		static int computeMiniWindowMouseFlags(const QMouseEvent *event, bool doubleClick, int baseFlags);
		/**
		 * @brief Applies cursor for currently hovered hotspot.
		 * @param window Hovered miniwindow.
		 * @param hotspotId Hovered hotspot id.
		 */
		void       updateHotspotCursor(MiniWindow *window, const QString &hotspotId);
		/**
		 * @brief Returns hotspot tooltip start delay.
		 * @return Tooltip start delay in milliseconds.
		 */
		[[nodiscard]] int    tooltipStartDelayMs() const;
		/**
		 * @brief Returns hotspot tooltip visible duration.
		 * @return Tooltip visible duration in milliseconds.
		 */
		[[nodiscard]] int    tooltipVisibleDurationMs() const;
		/**
		 * @brief Schedules tooltip display for hotspot.
		 * @param hotspotId Hotspot identifier.
		 * @param tooltipText Tooltip text.
		 * @param globalPos Tooltip anchor position in global coordinates.
		 */
		void                 scheduleHotspotTooltip(const QString &hotspotId, const QString &tooltipText,
		                                            const QPoint &globalPos);
		/**
		 * @brief Shows previously scheduled hotspot tooltip.
		 */
		void                 showScheduledHotspotTooltip();
		/**
		 * @brief Clears any pending hotspot tooltip request.
		 */
		void                 clearPendingHotspotTooltip();
		/**
		 * @brief Updates line-information tooltip from mouse position.
		 * @param watched Widget receiving mouse events.
		 * @param event Mouse event payload.
		 */
		void                 updateLineInformationTooltip(const QWidget *watched, const QMouseEvent *event);
		/**
		 * @brief Computes line fade opacity for timestamp.
		 * @param when Line timestamp.
		 * @return Opacity in range `[0.0, 1.0]`.
		 */
		[[nodiscard]] double lineOpacityForTimestamp(const QDateTime &when) const;
		/**
		 * @brief Returns whether fade rebuild should run now.
		 * @return `true` when fade rebuild should run.
		 */
		[[nodiscard]] bool   fadeRebuildNeededNow() const;
		/**
		 * @brief Builds display text/spans from stored line entry.
		 * @param entry Source line entry.
		 * @param previousLineTime Timestamp of previous line.
		 * @param displayText Output rendered text.
		 * @param displaySpans Output rendered style spans.
		 */
		void buildDisplayLine(const WorldRuntime::LineEntry &entry, const QDateTime &previousLineTime,
		                      QString &displayText, QVector<WorldRuntime::StyleSpan> &displaySpans) const;
		/**
		 * @brief Queues draw-output-window notification callback.
		 */
		void requestDrawOutputWindowNotification();
		/**
		 * @brief Fires draw-output-window notification callback.
		 */
		void notifyDrawOutputWindow() const;
		/**
		 * @brief Fast path append used by high-throughput output updates.
		 * @param text Text payload.
		 * @param spans Style spans aligned to `text`.
		 * @param newLine Append newline when `true`.
		 * @param opacity Effective line opacity.
		 * @return `true` when fast path append succeeded.
		 */
		bool appendOutputTextFast(const QString &text, const QVector<WorldRuntime::StyleSpan> &spans,
		                          bool newLine, double opacity);
		/**
		 * @brief Commits pending inline-input separator both in runtime state and output view.
		 *
		 * This keeps synthetic line breaks (inserted before subsequent output/input after
		 * keep-on-same-line echo) reproducible when output is rebuilt from runtime lines.
		 */
		void commitPendingInlineInputBreak();
		struct PendingHtml
		{
				QString html;
				bool    newLine{false};
		};

		WrapTextBrowser                             *m_output{nullptr};
		WrapTextBrowser                             *m_liveOutput{nullptr};
		InputTextEdit                               *m_input{nullptr};
		/**
		 * @brief Returns currently active output view (live/split).
		 * @return Active output view pointer.
		 */
		[[nodiscard]] [[nodiscard]] WrapTextBrowser *activeOutputView() const;
		QSplitter                                   *m_splitter{nullptr};
		QSplitter                                   *m_outputSplitter{nullptr};
		QWidget                                     *m_outputContainer{nullptr};
		QWidget                                     *m_outputStack{nullptr};
		QWidget                                     *m_miniUnderlay{nullptr};
		QWidget                                     *m_miniOverlay{nullptr};
		QScrollBar                                  *m_outputScrollBar{nullptr};
		bool                                         m_outputScrollBarWanted{true};
		QTextDocument                               *m_outputDocument{nullptr};
		bool                                         m_wrapInput{false};
		int                                          m_inputPixelOffset{0};
		WorldRuntime                                *m_runtime{nullptr};
		QFont                                        m_defaultOutputFont;
		QFont                                        m_defaultInputFont;
		bool                                         m_displayMyInput{false};
		bool                                         m_escapeDeletesInput{false};
		bool                                         m_saveDeletedCommand{false};
		bool                                         m_confirmOnPaste{false};
		bool                                         m_ctrlBackspaceDeletesLastWord{false};
		bool                                         m_arrowsChangeHistory{false};
		bool                                         m_arrowKeysWrap{false};
		bool                                         m_arrowRecallsPartial{false};
		bool                                         m_altArrowRecallsPartial{false};
		bool                                         m_ctrlZGoesToEndOfBuffer{false};
		bool                                         m_ctrlPGoesToPreviousCommand{false};
		bool                                         m_ctrlNGoesToNextCommand{false};
		bool                                         m_confirmBeforeReplacingTyping{false};
		bool                                         m_doubleClickInserts{false};
		bool                                         m_doubleClickSends{false};
		bool                                         m_showBold{true};
		bool                                         m_showItalic{true};
		bool                                         m_showUnderline{true};
		bool                                         m_alternativeInverse{false};
		bool                                         m_lineInformation{false};
		int                                          m_lineSpacing{0};
		bool                                         m_lowerCaseTabCompletion{false};
		bool                                         m_tabCompletionSpace{false};
		bool                                         m_autoRepeat{false};
		bool                                         m_keepCommandsOnSameLine{false};
		bool                                         m_noEchoOff{false};
		bool                                         m_noEcho{false};
		bool                                         m_alwaysRecordCommandHistory{false};
		bool                                         m_hyperlinkAddsToCommandHistory{false};
		bool                                         m_inputChanged{false};
		bool                                         m_settingText{false};
		bool                                         m_notifyingPluginCommandChanged{false};
		bool                                         m_frozen{false};
		bool                                         m_autoPause{false};
		QString                                      m_wordDelimiters;
		QString                                      m_wordDelimitersDblClick;
		bool                                         m_smoothScrolling{false};
		bool                                         m_smootherScrolling{false};
		bool                                         m_allTypingToCommandWindow{false};
		bool                                         m_autoResizeCommandWindow{false};
		int                                          m_autoResizeMinimumLines{1};
		int                                          m_autoResizeMaximumLines{20};
		int                                          m_tabCompletionLines{200};
		QString                                      m_tabCompletionDefaults;
		int                                          m_fadeOutputBufferAfterSeconds{0};
		int                                          m_fadeOutputOpacityPercent{100};
		int                                          m_fadeOutputSeconds{1};
		QTimer                                      *m_fadeTimer{nullptr};
		QDateTime                                    m_timeFadeCancelled;
		bool                                         m_breakBeforeNextServerOutput{false};
		bool                                         m_keepPauseAtBottom{false};
		bool                                         m_userScrollAction{false};
		bool                                         m_startPausedApplied{false};
		bool                                         m_defaultInputHeightApplied{false};
		bool                                         m_scrollbackSplitActive{false};
		int                                          m_lastLiveSplitSize{0};
		int                                          m_wrapColumn{0};
		int                                          m_historyLimit{0};
		bool                                         m_useCustomLinkColour{false};
		bool                                         m_underlineHyperlinks{true};
		QColor                                       m_hyperlinkColour;
		QColor                                       m_outputBackground;
		bool                                         m_bleedBackground{false};
		int                                          m_historyIndex{-1};
		int                                          m_partialIndex{-1};
		QString                                      m_partialCommand;
		QString                                      m_lastCommand;
		QVector<QString>                             m_history;
		QVector<PendingHtml>                         m_pendingOutput;
		bool                                         m_flushingPending{false};
		bool                                         m_hasPartialOutput{false};
		int                                          m_partialOutputStart{0};
		int                                          m_partialOutputLength{0};
		struct OutputFindState;
		QScopedPointer<OutputFindState>         m_outputFind;
		QScopedPointer<CommandHistoryFindState> m_commandHistoryFind;
		QString                                 m_hoverWindowName;
		QString                                 m_capturedWindowName;
		QString                                 m_tooltipHotspot;
		QString                                 m_pendingTooltipHotspot;
		QString                                 m_pendingTooltipText;
		QPoint                                  m_pendingTooltipGlobalPos;
		QTimer                                 *m_tooltipTimer{nullptr};
		bool                                    m_anchorHoverActive{false};
		QString                                 m_hoveredHyperlinkHref;
		bool                                    m_mouseCaptured{false};
		bool                                    m_hasOutputSelection{false};
		int                                     m_lastSelectionStartLine{0};
		int                                     m_lastSelectionStartColumn{0};
		int                                     m_lastSelectionEndLine{0};
		int                                     m_lastSelectionEndColumn{0};
		QPoint                                  m_lastMousePos;
		bool                                    m_hasLastMousePos{false};
		bool                                    m_drawNotifyQueued{false};
		QElapsedTimer                           m_drawNotifyThrottle;
		qint64                                  m_lastDrawNotifyMs{-1000};
		bool                                    m_scrollToEndQueued{false};
		bool                                    m_bulkOutputRebuild{false};
		bool                                    m_keypadRepeatArmed{false};
		int                                     m_keypadRepeatQtKey{0};
		bool                                    m_keypadRepeatCtrl{false};
};

#endif // QMUD_WORLDVIEW_H
