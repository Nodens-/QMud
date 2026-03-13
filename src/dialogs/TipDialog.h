/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TipDialog.h
 * Role: Tip-of-the-day dialog interfaces used to present rotating usage guidance and onboarding hints.
 */

#ifndef QMUD_TIPDIALOG_H
#define QMUD_TIPDIALOG_H

#include <QDialog>
#include <QFile>
#include <QTextStream>
#include <functional>

class QLabel;
class QCheckBox;

/**
 * @brief “Tip of the day” dialog with persisted navigation state.
 */
class TipDialog : public QDialog
{
		Q_OBJECT
	public:
		using GetIntFn = std::function<int(const QString &section, const QString &entry, int defaultValue)>;
		/**
		 * @brief Callback type for reading string preference values.
		 */
		using GetStringFn =
		    std::function<QString(const QString &section, const QString &entry, const QString &defaultValue)>;
		using WriteIntFn = std::function<int(const QString &section, const QString &entry, int value)>;
		/**
		 * @brief Callback type for writing string preference values.
		 */
		using WriteStringFn =
		    std::function<int(const QString &section, const QString &entry, const QString &value)>;

		/**
		 * @brief Creates tip dialog with persisted-settings callbacks.
		 * @param getInt Callback for reading integer settings.
		 * @param getString Callback for reading string settings.
		 * @param writeInt Callback for writing integer settings.
		 * @param writeString Callback for writing string settings.
		 * @param parent Optional Qt parent widget.
		 */
		TipDialog(GetIntFn getInt, GetStringFn getString, WriteIntFn writeInt, WriteStringFn writeString,
		          QWidget *parent = nullptr);
		/**
		 * @brief Destroys dialog and closes tip source stream.
		 */
		~TipDialog() override;

	private slots:
		/**
		 * @brief Advances to next tip entry.
		 */
		void onNextTip();
		/**
		 * @brief Persists state when dialog is accepted.
		 */
		void onAccepted();

	private:
		/**
		 * @brief Loads tip file and initializes stream.
		 */
		void           loadTipFile();
		/**
		 * @brief Reads next tip string from stream.
		 * @param next Output tip text.
		 */
		void           getNextTipString(QString &next);
		/**
		 * @brief Resolves path to bundled tip file.
		 * @return Tip file path.
		 */
		static QString tipFilePath();

			QFile          m_file;
			QTextStream    m_stream;
			bool           m_startup{true};
			QString        m_tipText;
			QLabel        *m_tipLabel{nullptr};
			QCheckBox     *m_startupCheck{nullptr};

			GetIntFn       m_getInt;
			GetStringFn    m_getString;
			WriteIntFn     m_writeInt;
		WriteStringFn  m_writeString;
};

#endif // QMUD_TIPDIALOG_H
