/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldDocument.h
 * Role: Persistent world-data model interfaces covering options, triggers, aliases, timers, and serialized
 * configuration.
 */

#ifndef QMUD_WORLDDOCUMENT_H
#define QMUD_WORLDDOCUMENT_H

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

/**
 * @brief Parsed world-file document model and XML load/save entry point.
 *
 * Stores world attributes, entities, scripts, and option collections used by
 * runtime initialization and persistence workflows.
 */
class WorldDocument : public QObject
{
		Q_OBJECT
	public:
		/**
		 * @brief Creates an empty world document model.
		 * @param parent Optional Qt parent object.
		 */
		explicit WorldDocument(QObject *parent = nullptr);

		/**
		 * @brief Loads a world XML file into this document.
		 * @param fileName World XML file path.
		 * @return `true` on successful load/parse.
		 */
		bool                    loadFromFile(const QString &fileName);
		/**
		 * @brief Loads a plugin XML file into this document.
		 * @param fileName Plugin XML file path.
		 * @return `true` on successful load/parse.
		 */
		bool                    loadFromPluginFile(const QString &fileName);
		/**
		 * @brief Expands include directives and merges referenced documents.
		 * @param worldFilePath Current world file path.
		 * @param pluginsDir Plugins directory path.
		 * @param programDir Program directory path.
		 * @param stateDir State-files directory path.
		 * @return `true` when include expansion succeeds.
		 */
		bool                    expandIncludes(const QString &worldFilePath, const QString &pluginsDir,
		                                       const QString &programDir, const QString &stateDir);
		/**
		 * @brief Returns last load/parse error text.
		 * @return Error string.
		 */
		[[nodiscard]] QString   errorString() const;

		/**
		 * @brief Returns parsed world-file version.
		 * @return Parsed world-file version.
		 */
		[[nodiscard]] int       worldFileVersion() const;
		/**
		 * @brief Returns parsed QMud version string.
		 * @return Parsed QMud version string.
		 */
		[[nodiscard]] QString   qmudVersion() const;
		/**
		 * @brief Returns world save timestamp from XML.
		 * @return Parsed save timestamp.
		 */
		[[nodiscard]] QDateTime dateSaved() const;
		/**
		 * @brief Returns single-line world attribute map.
		 * @return Immutable world attribute map.
		 */
		[[nodiscard]] const QMap<QString, QString> &worldAttributes() const;
		/**
		 * @brief Returns multiline world attribute map.
		 * @return Immutable multiline world attribute map.
		 */
		[[nodiscard]] const QMap<QString, QString> &worldMultilineAttributes() const;
		/**
		 * @brief Parsed trigger node from world XML.
		 */
		struct Trigger
		{
				QMap<QString, QString> attributes;
				QMap<QString, QString> children;
				bool                   included{false};
		};
		/**
		 * @brief Returns loaded trigger definitions.
		 * @return Immutable trigger list.
		 */
		[[nodiscard]] const QList<Trigger> &triggers() const;
		/**
		 * @brief Parsed alias node from world XML.
		 */
		struct Alias
		{
				QMap<QString, QString> attributes;
				QMap<QString, QString> children;
				bool                   included{false};
		};
		/**
		 * @brief Returns loaded alias definitions.
		 * @return Immutable alias list.
		 */
		[[nodiscard]] const QList<Alias> &aliases() const;
		/**
		 * @brief Parsed timer node from world XML.
		 */
		struct Timer
		{
				QMap<QString, QString> attributes;
				QMap<QString, QString> children;
				bool                   included{false};
		};
		/**
		 * @brief Returns loaded timer definitions.
		 * @return Immutable timer list.
		 */
		[[nodiscard]] const QList<Timer> &timers() const;
		/**
		 * @brief Parsed macro node from world XML.
		 */
		struct Macro
		{
				QMap<QString, QString> attributes;
				QMap<QString, QString> children;
		};
		/**
		 * @brief Returns loaded macro definitions.
		 * @return Immutable macro list.
		 */
		[[nodiscard]] const QList<Macro> &macros() const;
		/**
		 * @brief Parsed variable node from world XML.
		 */
		struct Variable
		{
				QMap<QString, QString> attributes;
				QString                content;
		};
		/**
		 * @brief Returns loaded variable definitions.
		 * @return Immutable variable list.
		 */
		[[nodiscard]] const QList<Variable> &variables() const;
		/**
		 * @brief Parsed color customization node from world XML.
		 */
		struct Colour
		{
				QString                group;
				QMap<QString, QString> attributes;
		};
		/**
		 * @brief Returns loaded colour definitions.
		 * @return Immutable colour list.
		 */
		[[nodiscard]] const QList<Colour> &colours() const;
		/**
		 * @brief Parsed keypad mapping node from world XML.
		 */
		struct Keypad
		{
				QMap<QString, QString> attributes;
				QString                content;
		};
		/**
		 * @brief Returns loaded keypad entries.
		 * @return Immutable keypad entry list.
		 */
		[[nodiscard]] const QList<Keypad> &keypadEntries() const;
		/**
		 * @brief Parsed printing-style node from world XML.
		 */
		struct PrintingStyle
		{
				QString                group;
				QMap<QString, QString> attributes;
		};
		/**
		 * @brief Returns loaded printing styles.
		 * @return Immutable printing-style list.
		 */
		[[nodiscard]] const QList<PrintingStyle> &printingStyles() const;
		/**
		 * @brief Returns optional comments section text.
		 * @return Comment text.
		 */
		[[nodiscard]] QString                     comments() const;
		/**
		 * @brief Parsed plugin payload from world XML.
		 */
		struct Plugin
		{
				QMap<QString, QString> attributes;
				QString                description;
				QString                script;
				QList<Trigger>         triggers;
				QList<Alias>           aliases;
				QList<Timer>           timers;
				QList<Variable>        variables;
		};
		/**
		 * @brief Returns loaded plugin entries.
		 * @return Immutable plugin list.
		 */
		[[nodiscard]] const QList<Plugin> &plugins() const;
		/**
		 * @brief Parsed include directive from world XML.
		 */
		struct Include
		{
				QMap<QString, QString> attributes;
		};
		/**
		 * @brief Returns include directives from document.
		 * @return Immutable include list.
		 */
		[[nodiscard]] const QList<Include> &includes() const;
		/**
		 * @brief Parsed script block from world XML.
		 */
		struct Script
		{
				QString content;
		};
		/**
		 * @brief Returns script sections from document.
		 * @return Immutable script-section list.
		 */
		[[nodiscard]] const QList<Script> &scripts() const;
		/**
		 * @brief Returns resolved include file list.
		 * @return Include file path list.
		 */
		[[nodiscard]] const QStringList   &includeFileList() const;
		/**
		 * @brief Sets XML section mask used by subsequent loads.
		 * @param mask XML section load mask.
		 */
		void                               setLoadMask(unsigned long mask);
		/**
		 * @brief Returns active XML load mask.
		 * @return Active XML load mask.
		 */
		[[nodiscard]] unsigned long        loadMask() const;
		/**
		 * @brief Sets include merge behavior flags.
		 * @param flags Include merge flags.
		 */
		void                               setIncludeMergeFlags(unsigned int flags);
		/**
		 * @brief Returns include merge behavior flags.
		 * @return Include merge flags.
		 */
		[[nodiscard]] unsigned int         includeMergeFlags() const;
		/**
		 * @brief Returns non-fatal load/merge warnings.
		 * @return Warning message list.
		 */
		[[nodiscard]] const QStringList   &warnings() const;

		enum LoadMask : unsigned long
		{
			XML_GENERAL               = 0x0001,
			XML_TRIGGERS              = 0x0002,
			XML_ALIASES               = 0x0004,
			XML_TIMERS                = 0x0008,
			XML_MACROS                = 0x0010,
			XML_VARIABLES             = 0x0020,
			XML_COLOURS               = 0x0040,
			XML_KEYPAD                = 0x0080,
			XML_PRINTING              = 0x0100,
			XML_INCLUDES              = 0x0200,
			XML_PLUGINS               = 0x0400,
			XML_NO_PLUGINS            = 0x0800,
			XML_OVERWRITE             = 0x1000,
			XML_PASTE_DUPLICATE       = 0x2000,
			XML_IMPORT_MAIN_FILE_ONLY = 0x4000
		};
		static constexpr unsigned long kDefaultLoadMask =
		    XML_GENERAL | XML_TRIGGERS | XML_ALIASES | XML_TIMERS | XML_MACROS | XML_VARIABLES | XML_COLOURS |
		    XML_KEYPAD | XML_PRINTING | XML_INCLUDES | XML_PLUGINS;

	private:
		enum class PluginPolicy
		{
			AllowPlugins,
			ForbidPlugins,
			RequirePlugin
		};
		enum IncludeMergeFlag : unsigned int
		{
			IncludeMergeOverwrite = 0x02,
			IncludeMergeKeep      = 0x04,
			IncludeMergeWarn      = 0x08
		};

		/**
		 * @brief Loads file using plugin-policy constraints.
		 * @param fileName Source XML file path.
		 * @param policy Plugin-content policy.
		 * @param includeContext Whether load happens in include context.
		 * @return `true` on successful load.
		 */
		bool        loadFromFileWithPolicy(const QString &fileName, PluginPolicy policy, bool includeContext);
		/**
		 * @brief Clears all stored parse/load state.
		 */
		void        clearState();
		/**
		 * @brief Returns true when file is an archive wrapper containing XML.
		 * @param fileName Source file path.
		 * @return `true` when file is an XML archive wrapper.
		 */
		static bool isArchiveXMLFile(const QString &fileName);
		/**
		 * @brief Resolves include path using world/plugin/program/state roots.
		 * @param rawName Raw include path/name.
		 * @param worldFilePath Current world file path.
		 * @param pluginsDir Plugins directory path.
		 * @param programDir Program directory path.
		 * @param currentPluginDir Current plugin directory.
		 * @param wantPlugins Resolve within plugins scope when `true`.
		 * @return Resolved include path.
		 */
		static QString resolveIncludePath(const QString &rawName, const QString &worldFilePath,
		                                  const QString &pluginsDir, const QString &programDir,
		                                  const QString &currentPluginDir, bool wantPlugins);
		/**
		 * @brief Performs one include-expansion pass.
		 * @param worldFilePath Current world file path.
		 * @param pluginsDir Plugins directory path.
		 * @param programDir Program directory path.
		 * @param stateDir State-files directory path.
		 * @param wantPlugins Include plugin files when `true`.
		 * @param currentPluginDir Current plugin directory.
		 * @param isIncludeContext Whether currently processing include context.
		 * @return `true` when pass succeeds.
		 */
		bool           expandIncludesPass(const QString &worldFilePath, const QString &pluginsDir,
		                                  const QString &programDir, const QString &stateDir, bool wantPlugins,
		                                  const QString &currentPluginDir, bool isIncludeContext);
		/**
		 * @brief Merges variable list from another document.
		 * @param other Source document.
		 * @param overwrite Overwrite existing variables when `true`.
		 */
		void           mergeVariablesFrom(const WorldDocument &other, bool overwrite);
		/**
		 * @brief Merges another document into this one.
		 * @param other Source document.
		 * @param fromInclude Merge as include context when `true`.
		 * @return `true` when merge succeeds.
		 */
		bool           mergeFrom(const WorldDocument &other, bool fromInclude);
		/**
		 * @brief Finalizes plugin payloads after load/merge.
		 */
		void           finalizePluginContents();
		/**
		 * @brief Merges plugin-scoped variables.
		 * @param plugin Target plugin record.
		 * @param vars Variables to merge.
		 * @param overwrite Overwrite existing variables when `true`.
		 */
		static void    mergePluginVariablesFrom(Plugin &plugin, const QList<Variable> &vars, bool overwrite);

		QString        m_errorString;
		bool           m_loadedFromInclude{false};
		bool           m_pluginFile{false};
		bool           m_pluginContentFinalized{false};
		unsigned long  m_loadMask{kDefaultLoadMask};
		unsigned int   m_includeMergeFlags{0};
		QStringList    m_warnings;
		int            m_worldFileVersion{0};
			QString        m_qmudVersion;
		QDateTime      m_dateSaved;
		QMap<QString, QString> m_worldAttributes;
		QMap<QString, QString> m_worldMultilineAttributes;
		QList<Trigger>         m_triggers;
		QList<Alias>           m_aliases;
		QList<Timer>           m_timers;
		QList<Macro>           m_macros;
		QList<Variable>        m_variables;
		QList<Colour>          m_colours;
		QList<Keypad>          m_keypadEntries;
		QList<PrintingStyle>   m_printingStyles;
		QString                m_comments;
		QList<Plugin>          m_plugins;
		QMap<QString, QString> m_loadedPluginIds;
		QList<Include>         m_includes;
		QList<Script>          m_scripts;
		QStringList            m_currentIncludeStack;
		QStringList            m_includeFileList;
};

#endif // QMUD_WORLDDOCUMENT_H
