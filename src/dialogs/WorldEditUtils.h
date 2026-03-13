/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldEditUtils.h
 * Role: Shared helper interfaces for alias/trigger/timer editor dialogs, including validation and field normalization.
 */

// Helper functions for editing world items (aliases/triggers/timers).
#ifndef QMUD_WORLDEDITUTILS_H
#define QMUD_WORLDEDITUTILS_H

#include <QRegularExpression>
#include <QString>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QWidget;

namespace WorldEditUtils
{
	/**
	 * @brief Populates send-target combobox with standard destinations.
	 * @param combo Target combobox.
	 */
	void    populateSendToCombo(QComboBox *combo);
	/**
	 * @brief Returns display label for send-target id.
	 * @param sendTo Send-target id.
	 * @return Send-target display label.
	 */
	QString sendToLabel(int sendTo);

	/**
	 * @brief Converts wildcard text to regular-expression pattern.
	 * @param matchString Wildcard pattern text.
	 * @param wholeLine Anchor to whole line when `true`.
	 * @param makeAsterisksWildcards Treat `*` as wildcard when `true`.
	 * @return Equivalent regular-expression pattern.
	 */
	QString convertToRegularExpression(const QString &matchString, bool wholeLine = true,
	                                   bool makeAsterisksWildcards = true);
	/**
	 * @brief Expands speedwalk syntax using filler separator.
	 * @param speedWalkString Speedwalk text.
	 * @param filler Separator used between expanded commands.
	 * @return Expanded command string.
	 */
	QString evaluateSpeedwalk(const QString &speedWalkString, const QString &filler);
	/**
	 * @brief Produces reverse speedwalk path text.
	 * @param speedWalkString Speedwalk text.
	 * @return Reverse speedwalk string.
	 */
	QString reverseSpeedwalk(const QString &speedWalkString);

	/**
	 * @brief Validates user label text for script/rule entries.
	 * @param label Label text.
	 * @param script Validate with script-label rules when `true`.
	 * @return `true` when label is invalid.
	 */
	bool    checkLabelInvalid(const QString &label, bool script);
	/**
	 * @brief Returns index of first invalid character in text.
	 * @param text Text to validate.
	 * @param invalid List of invalid character codes.
	 * @return Index of first invalid character, or `-1`.
	 */
	int     findInvalidChar(const QString &text, const QList<ushort> &invalid);
	/**
	 * @brief Validates regular-expression syntax and optionally reports errors.
	 * @param parent Optional parent widget for error UI.
	 * @param pattern Regular-expression pattern text.
	 * @param options Regex pattern options.
	 * @return `true` when pattern is valid.
	 */
	bool    checkRegularExpression(QWidget *parent, const QString &pattern,
	                               QRegularExpression::PatternOptions options);
	/**
	 * @brief Warns user about ambiguous multiple-asterisk patterns.
	 * @param parent Optional parent widget for warning UI.
	 * @return `true` when user accepts continuing.
	 */
	bool    showMultipleAsterisksWarning(QWidget *parent);
	/**
	 * @brief Applies editor preference settings to line edit.
	 * @param edit Target line-edit widget.
	 */
	void    applyEditorPreferences(QLineEdit *edit);
	/**
	 * @brief Applies editor preference settings to plain-text edit.
	 * @param edit Target plain-text edit widget.
	 */
	void    applyEditorPreferences(QPlainTextEdit *edit);
} // namespace WorldEditUtils

#endif // QMUD_WORLDEDITUTILS_H
