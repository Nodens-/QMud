/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Dialog_Plugins.cpp
 * Role: QTest coverage for Dialog Plugins behavior.
 */

#include "AppController.h"
#include "MainFrame.h"
// ReSharper disable once CppUnusedIncludeDirective
#include "NameGeneration.h"
#include "TelnetProcessor.h"
#include "WorldRuntime.h"
#include "dialogs/PluginsDialog.h"
#include "scripting/ScriptingErrors.h"

#include <QPushButton>
#include <QTableWidget>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace
{
	struct RuntimeStubState
	{
			QList<WorldRuntime::Plugin> plugins;
			QMap<QString, QString>      worldAttributes;
			QString                     pluginsDirectory;
			QString                     stateFilesDirectory;
			int                         reloadCalls{0};
			QString                     lastReloadPluginId;
	};

	QHash<const WorldRuntime *, RuntimeStubState> &runtimeStates()
	{
		static QHash<const WorldRuntime *, RuntimeStubState> states;
		return states;
	}

	RuntimeStubState &stateFor(const WorldRuntime *runtime)
	{
		return runtimeStates()[runtime];
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

	WorldRuntime::Plugin makePlugin(const QString &id, const QString &name, const bool enabled = true)
	{
		WorldRuntime::Plugin plugin;
		plugin.attributes.insert(QStringLiteral("id"), id);
		plugin.attributes.insert(QStringLiteral("name"), name);
		plugin.source  = QStringLiteral("/tmp/%1.xml").arg(id);
		plugin.enabled = enabled;
		plugin.version = 1.0;
		return plugin;
	}
} // namespace

// NOLINTBEGIN(readability-convert-member-functions-to-static,readability-make-member-function-const)
AppController::AppController(QObject *parent) : QObject(parent)
{
}

AppController::~AppController() = default;

AppController *AppController::instance()
{
	static AppController app;
	return &app;
}

QString AppController::iniFilePath() const
{
	return QStringLiteral("/tmp/qmud-test-plugins-dialog.ini");
}

bool AppController::openDocumentFile(const QString &)
{
	return true;
}

void AppController::onCommandTriggered(const QString &)
{
}

TelnetProcessor::TelnetProcessor() = default;

WorldRuntime::WorldRuntime(QObject *parent) : QObject(parent)
{
	RuntimeStubState state;
	state.pluginsDirectory    = QStringLiteral("/tmp");
	state.stateFilesDirectory = QStringLiteral("/tmp");
	state.worldAttributes.insert(QStringLiteral("id"), QStringLiteral("world-id"));
	runtimeStates().insert(this, state);
}

WorldRuntime::~WorldRuntime()
{
	runtimeStates().remove(this);
}

const QMap<QString, QString> &WorldRuntime::worldAttributes() const
{
	return stateFor(this).worldAttributes;
}

const QList<WorldRuntime::Plugin> &WorldRuntime::plugins() const
{
	return stateFor(this).plugins;
}

QList<WorldRuntime::Plugin> &WorldRuntime::pluginsMutable()
{
	return stateFor(this).plugins;
}

bool WorldRuntime::loadPluginFile(const QString &, QString *, bool)
{
	return true;
}

bool WorldRuntime::unloadPlugin(const QString &pluginId, QString *)
{
	QList<Plugin> &plugins = stateFor(this).plugins;
	for (qsizetype i = 0; i < plugins.size(); ++i)
	{
		if (plugins.at(i).attributes.value(QStringLiteral("id")) == pluginId)
		{
			plugins.removeAt(i);
			return true;
		}
	}
	return false;
}

bool WorldRuntime::enablePlugin(const QString &pluginId, const bool enable)
{
	QList<Plugin> &plugins = stateFor(this).plugins;
	for (Plugin &plugin : plugins)
	{
		if (plugin.attributes.value(QStringLiteral("id")) == pluginId)
		{
			plugin.enabled = enable;
			return true;
		}
	}
	return false;
}

int WorldRuntime::reloadPlugin(const QString &pluginId, QString *)
{
	RuntimeStubState &state = stateFor(this);
	++state.reloadCalls;
	state.lastReloadPluginId = pluginId;
	return eOK;
}

QString WorldRuntime::pluginsDirectory() const
{
	return stateFor(this).pluginsDirectory;
}

QString WorldRuntime::stateFilesDirectory() const
{
	return stateFor(this).stateFilesDirectory;
}

WorldChildWindow *MainWindow::activeWorldChildWindow() const
{
	return nullptr;
}

bool MainWindow::sendToNotepad(const QString &, const QString &, WorldRuntime *)
{
	return true;
}
// NOLINTEND(readability-convert-member-functions-to-static,readability-make-member-function-const)

/**
 * @brief QTest fixture covering Dialog Plugins scenarios.
 */
class tst_Dialog_Plugins : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void tablePopulationAndSelectionState()
		{
			WorldRuntime runtime;
			runtime.pluginsMutable().push_back(makePlugin(QStringLiteral("a"), QStringLiteral("Alpha")));
			runtime.pluginsMutable().push_back(
			    makePlugin(QStringLiteral("b"), QStringLiteral("Beta"), false));

			PluginsDialog dialog(&runtime, nullptr);
			dialog.show();

			auto *table = dialog.findChild<QTableWidget *>();
			QVERIFY(table);
			QCOMPARE(table->rowCount(), 2);

			QPushButton *enableButton  = findButtonByText(dialog, QStringLiteral("Enable"));
			QPushButton *disableButton = findButtonByText(dialog, QStringLiteral("Disable"));
			QPushButton *reloadButton  = findButtonByText(dialog, QStringLiteral("ReInstall"));
			QVERIFY(enableButton);
			QVERIFY(disableButton);
			QVERIFY(reloadButton);

			QVERIFY(!enableButton->isEnabled());
			QVERIFY(!disableButton->isEnabled());
			QVERIFY(!reloadButton->isEnabled());

			QSignalSpy selectionChangedSpy(table->selectionModel(), &QItemSelectionModel::selectionChanged);
			table->selectRow(0);
			QCoreApplication::processEvents();
			QVERIFY(selectionChangedSpy.count() >= 1);

			QVERIFY(enableButton->isEnabled());
			QVERIFY(disableButton->isEnabled());
			QVERIFY(reloadButton->isEnabled());
		}

		void enableDisableAndReloadActOnSelectedPlugin()
		{
			WorldRuntime runtime;
			runtime.pluginsMutable().push_back(
			    makePlugin(QStringLiteral("plug"), QStringLiteral("Plugin"), true));

			PluginsDialog dialog(&runtime, nullptr);
			dialog.show();

			auto *table = dialog.findChild<QTableWidget *>();
			QVERIFY(table);
			QCOMPARE(table->rowCount(), 1);
			table->selectRow(0);

			QPushButton *disableButton = findButtonByText(dialog, QStringLiteral("Disable"));
			QPushButton *enableButton  = findButtonByText(dialog, QStringLiteral("Enable"));
			QPushButton *reloadButton  = findButtonByText(dialog, QStringLiteral("ReInstall"));
			QVERIFY(disableButton);
			QVERIFY(enableButton);
			QVERIFY(reloadButton);

			QTest::mouseClick(disableButton, Qt::LeftButton);
			QVERIFY(!runtime.plugins().front().enabled);

			table->selectRow(0);
			QTest::mouseClick(enableButton, Qt::LeftButton);
			QVERIFY(runtime.plugins().front().enabled);

			table->selectRow(0);
			QTest::mouseClick(reloadButton, Qt::LeftButton);
			QCOMPARE(stateFor(&runtime).reloadCalls, 1);
			QCOMPARE(stateFor(&runtime).lastReloadPluginId, QStringLiteral("plug"));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_Dialog_Plugins)


#if __has_include("tst_Dialog_Plugins.moc")
#include "tst_Dialog_Plugins.moc"
#endif
