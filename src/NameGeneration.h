/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: NameGeneration.h
 * Role: Interfaces for random/generated name creation used by onboarding, dialogs, and content-generation utilities.
 */

#ifndef QMUD_NAMEGENERATION_H
#define QMUD_NAMEGENERATION_H

#include <QStringList>

class AppController;
class QTextStream;

/**
 * @brief Generates random names from configured source lists.
 *
 * Loads name dictionaries and exposes helper APIs used by runtime/UI flows.
 */
class NameGenerator
{
	public:
		/**
		 * @brief Creates generator with optional app-controller integration.
		 * @param app Optional app controller used for settings/path lookup.
		 */
		explicit NameGenerator(AppController *app = nullptr);

		/**
		 * @brief Sets owning app controller used for settings/file lookup.
		 * @param app App controller pointer.
		 */
		void               setAppController(AppController *app);
		/**
		 * @brief Loads name lists from disk.
		 * @param fileName Optional explicit names file path.
		 * @param noDialog Suppress file-open dialog fallbacks when `true`.
		 */
		void               readNames(const QString &fileName = QString(), bool noDialog = false);
		/**
		 * @brief Generates one random composite name.
		 * @return Generated name.
		 */
		QString            generateName();
		/**
		 * @brief Reports whether name dictionaries are loaded.
		 * @return `true` when dictionaries have been loaded.
		 */
		[[nodiscard]] bool isLoaded() const;

	private:
		/**
		 * @brief Reads the next non-empty logical name line from source stream.
		 * @param stream Input stream.
		 * @param line Output line text.
		 * @return `true` when a logical line is read.
		 */
		static bool           readNextNameLine(QTextStream &stream, QString &line);
		/**
		 * @brief Checks whether a line starts with a dictionary tag marker.
		 * @param line Input text line.
		 * @param tag Tag marker prefix.
		 * @return `true` when line matches the tag marker.
		 */
		static bool           matchesTag(const QString &line, const char *tag);
		/**
		 * @brief Resolves source file path from explicit input or defaults.
		 * @param fileNameInput Explicit file path input.
		 * @param noDialog Suppress file-open dialog fallback when `true`.
		 * @return Resolved names file path.
		 */
		[[nodiscard]] QString resolveNamesFilePath(const QString &fileNameInput, bool noDialog) const;

		AppController        *m_app{nullptr};
		QStringList           m_firstNames;
		QStringList           m_middleNames;
		QStringList           m_lastNames;
		bool                  m_namesRead{false};
};

/**
 * @brief Loads global/shared generator dictionaries.
 * @param fileName Optional explicit names file path.
 * @param noDialog Suppress file-open dialog fallbacks when `true`.
 */
void    qmudReadNames(const QString &fileName = QString(), bool noDialog = false);
/**
 * @brief Generates one name from global/shared generator state.
 * @return Generated name.
 */
QString qmudGenerateName();

#endif // QMUD_NAMEGENERATION_H
