/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Dialog_GlobalPreferencesUpdates.cpp
 * Role: QTest coverage for Global Preferences update settings behavior and persistence.
 */

#include "AppController.h"
#include "MainFrame.h"
// ReSharper disable once CppUnusedIncludeDirective
#include "NameGeneration.h"
#include "WorldChildWindow.h"
#include "WorldRuntime.h"
#include "dialogs/GlobalPreferencesDialog.h"

#include <QCheckBox>
#include <QCoreApplication>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QWidget>
#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Returns per-process test INI file path.
	 * @return Absolute INI path under temporary directory.
	 */
	QString testIniFilePath()
	{
		static const QString path =
		    QDir::temp().filePath(QStringLiteral("qmud-test-global-preferences-dialog-%1.ini")
		                              .arg(QCoreApplication::applicationPid()));
		return path;
	}

	/**
	 * @brief Shared state for AppController/dialog test doubles.
	 */
	struct StubState
	{
			QHash<QString, QVariant> globalOptions;
			bool                     updateMechanismAvailable{true};
			QString                  updateMechanismUnavailableReason;
			int                      checkForUpdatesNowCallCount{0};
			QPointer<QWidget>        lastUpdateCheckParent;
			int                      applyGlobalPreferencesCallCount{0};
	};

	/**
	 * @brief Returns mutable singleton stub state.
	 * @return Shared test-double state.
	 */
	StubState &stubState()
	{
		static StubState state;
		return state;
	}

	/**
	 * @brief Resets test-double state and seeds update defaults used by the dialog.
	 */
	void resetStubState()
	{
		StubState &state = stubState();
		state.globalOptions.clear();
		state.updateMechanismAvailable         = true;
		state.updateMechanismUnavailableReason = QString();
		state.checkForUpdatesNowCallCount      = 0;
		state.lastUpdateCheckParent            = nullptr;
		state.applyGlobalPreferencesCallCount  = 0;

		state.globalOptions.insert(QStringLiteral("Locale"), QStringLiteral("en"));
		state.globalOptions.insert(QStringLiteral("AutoCheckForUpdates"), 1);
		state.globalOptions.insert(QStringLiteral("UpdateCheckIntervalHours"), 1);
		state.globalOptions.insert(QStringLiteral("EnableReloadFeature"), 1);
		state.globalOptions.insert(QStringLiteral("ReloadMccpDisableTimeoutMs"), 1000);

		QFile::remove(testIniFilePath());
	}

	/**
	 * @brief Finds a checkbox by exact text within a widget tree.
	 * @param root Root object for recursive search.
	 * @param text Checkbox text to match.
	 * @return Matching checkbox, or `nullptr`.
	 */
	QCheckBox *findCheckBoxByText(const QObject &root, const QString &text)
	{
		const QList<QCheckBox *> boxes = root.findChildren<QCheckBox *>();
		for (QCheckBox *box : boxes)
		{
			if (box && box->text() == text)
				return box;
		}
		return nullptr;
	}

	/**
	 * @brief Finds a label by exact text within a widget tree.
	 * @param root Root object for recursive search.
	 * @param text Label text to match.
	 * @return Matching label, or `nullptr`.
	 */
	QLabel *findLabelByText(const QObject &root, const QString &text)
	{
		const QList<QLabel *> labels = root.findChildren<QLabel *>();
		for (QLabel *label : labels)
		{
			if (label && label->text() == text)
				return label;
		}
		return nullptr;
	}

	/**
	 * @brief Finds a push button by exact text within a widget tree.
	 * @param root Root object for recursive search.
	 * @param text Button text to match.
	 * @return Matching button, or `nullptr`.
	 */
	QPushButton *findButtonByText(const QObject &root, const QString &text)
	{
		const QList<QPushButton *> buttons = root.findChildren<QPushButton *>();
		for (QPushButton *button : buttons)
		{
			if (button && button->text() == text)
				return button;
		}
		return nullptr;
	}

	/**
	 * @brief Finds a spin box by exact suffix within a widget tree.
	 * @param root Root object for recursive search.
	 * @param suffix Spin-box suffix to match.
	 * @return Matching spin box, or `nullptr`.
	 */
	QSpinBox *findSpinBySuffix(const QObject &root, const QString &suffix)
	{
		const QList<QSpinBox *> spins = root.findChildren<QSpinBox *>();
		for (QSpinBox *spin : spins)
		{
			if (spin && spin->suffix() == suffix)
				return spin;
		}
		return nullptr;
	}
} // namespace

// NOLINTBEGIN(readability-convert-member-functions-to-static)
/**
 * @brief Creates AppController test double.
 * @param parent Optional Qt parent object.
 */
AppController::AppController(QObject *parent) : QObject(parent)
{
}

/**
 * @brief Destroys AppController test double.
 */
AppController::~AppController() = default;

/**
 * @brief Returns singleton AppController test double.
 * @return Singleton test-double instance.
 */
AppController *AppController::instance()
{
	static AppController app;
	return &app;
}

/**
 * @brief Returns stubbed main-window pointer.
 * @return Always `nullptr` for this test.
 */
MainWindow *AppController::mainWindow() const
{
	return nullptr;
}

/**
 * @brief Returns test-specific INI path.
 * @return INI path in `/tmp` used by this test fixture.
 */
QString AppController::iniFilePath() const
{
	return testIniFilePath();
}

/**
 * @brief Reads a stubbed global option.
 * @param name Option key.
 * @return Stubbed option value.
 */
QVariant AppController::getGlobalOption(const QString &name) const
{
	return stubState().globalOptions.value(name);
}

/**
 * @brief Writes integer option into stub storage.
 * @param name Option key.
 * @param value Option value.
 */
void AppController::setGlobalOptionInt(const QString &name, const int value)
{
	stubState().globalOptions.insert(name, value);
}

/**
 * @brief Writes string option into stub storage.
 * @param name Option key.
 * @param value Option value.
 */
void AppController::setGlobalOptionString(const QString &name, const QString &value)
{
	stubState().globalOptions.insert(name, value);
}

/**
 * @brief Tracks apply-global-preferences invocations.
 */
void AppController::applyGlobalPreferences()
{
	++stubState().applyGlobalPreferencesCallCount;
}

/**
 * @brief Stub no-op for dialog filesystem action.
 */
void AppController::changeToFileBrowsingDirectory() const
{
}

/**
 * @brief Stub no-op for dialog filesystem action.
 */
void AppController::changeToStartupDirectory() const
{
}

/**
 * @brief Returns unchanged file path for test usage.
 * @param fileName Candidate file path.
 * @return Unmodified input path.
 */
QString AppController::makeAbsolutePath(const QString &fileName) const
{
	return fileName;
}

/**
 * @brief Returns stubbed list of currently open world log files.
 * @return Empty list for this test fixture.
 */
QStringList AppController::activeOpenWorldLogFiles() const
{
	return {};
}

/**
 * @brief Tracks manual update-check invocations from dialog UI.
 * @param uiParent Parent widget passed by the dialog.
 */
void AppController::checkForUpdatesNow(QWidget *uiParent)
{
	StubState &state = stubState();
	++state.checkForUpdatesNowCallCount;
	state.lastUpdateCheckParent = uiParent;
}

/**
 * @brief Stub slot required by AppController metaobject.
 * @param commandName Ignored command name.
 */
void AppController::onCommandTriggered(const QString &commandName)
{
	Q_UNUSED(commandName);
}

/**
 * @brief Returns stubbed update-mechanism availability.
 * @return `true` when update mechanism is enabled in stub state.
 */
bool AppController::isUpdateMechanismAvailable()
{
	return stubState().updateMechanismAvailable;
}

/**
 * @brief Returns stubbed update-unavailability reason.
 * @return Unavailability reason string.
 */
QString AppController::updateMechanismUnavailableReason()
{
	return stubState().updateMechanismUnavailableReason;
}

/**
 * @brief Stub file-association registration.
 * @param errorMessage Optional output message cleared on success.
 * @return Always `true` in this test fixture.
 */
bool AppController::registerFileAssociations(QString *errorMessage)
{
	if (errorMessage)
		errorMessage->clear();
	return true;
}

/**
 * @brief Stub active world-child lookup.
 * @return Always `nullptr` for this test fixture.
 */
WorldChildWindow *MainWindow::activeWorldChildWindow() const
{
	return nullptr;
}

/**
 * @brief Stub runtime lookup.
 * @return Always `nullptr` for this test fixture.
 */
WorldRuntime *WorldChildWindow::runtime() const
{
	return nullptr;
}

/**
 * @brief Stub world-file path lookup.
 * @return Empty path for this test fixture.
 */
QString WorldRuntime::worldFilePath() const
{
	return {};
}
// NOLINTEND(readability-convert-member-functions-to-static)

/**
 * @brief QTest fixture covering Global Preferences update-settings behavior.
 */
class tst_Dialog_GlobalPreferencesUpdates : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		/**
		 * @brief Verifies update-check controls are disabled when mechanism is unavailable.
		 */
		void updateControlsDisableWhenMechanismUnavailable()
		{
			resetStubState();
			stubState().updateMechanismAvailable         = false;
			stubState().updateMechanismUnavailableReason = QStringLiteral("Updates are disabled.");

			GlobalPreferencesDialog dialog;
			dialog.show();

			QCheckBox *autoCheck =
			    findCheckBoxByText(dialog, QStringLiteral("Automatically check for updates"));
			QCheckBox *enableReload =
			    findCheckBoxByText(dialog, QStringLiteral("Enable reload feature (Linux/MacOS)"));
			QPushButton *checkNowButton  = findButtonByText(dialog, QStringLiteral("Check now"));
			QSpinBox    *hoursSpin       = findSpinBySuffix(dialog, QStringLiteral(" hour(s)"));
			QSpinBox    *timeoutSpin     = findSpinBySuffix(dialog, QStringLiteral(" ms"));
			QLabel      *checkEveryLabel = findLabelByText(dialog, QStringLiteral("Check every:"));
			QVERIFY(autoCheck);
			QVERIFY(enableReload);
			QVERIFY(checkNowButton);
			QVERIFY(hoursSpin);
			QVERIFY(timeoutSpin);
			QVERIFY(checkEveryLabel);

			QVERIFY(!autoCheck->isEnabled());
			QVERIFY(!checkNowButton->isEnabled());
			QVERIFY(!hoursSpin->isEnabled());
			QVERIFY(!checkEveryLabel->isEnabled());
			QVERIFY(enableReload->isEnabled());
			QVERIFY(timeoutSpin->isEnabled());
			QCOMPARE(autoCheck->toolTip(), QStringLiteral("Updates are disabled."));
		}

		/**
		 * @brief Verifies interval controls track auto-check toggle while check-now remains enabled.
		 */
		void updateIntervalTracksAutoCheckStateWhenMechanismAvailable()
		{
			resetStubState();
			stubState().globalOptions.insert(QStringLiteral("AutoCheckForUpdates"), 1);
			stubState().globalOptions.insert(QStringLiteral("UpdateCheckIntervalHours"), 6);

			GlobalPreferencesDialog dialog;
			dialog.show();

			QCheckBox *autoCheck =
			    findCheckBoxByText(dialog, QStringLiteral("Automatically check for updates"));
			QPushButton *checkNowButton  = findButtonByText(dialog, QStringLiteral("Check now"));
			QSpinBox    *hoursSpin       = findSpinBySuffix(dialog, QStringLiteral(" hour(s)"));
			QLabel      *checkEveryLabel = findLabelByText(dialog, QStringLiteral("Check every:"));
			QVERIFY(autoCheck);
			QVERIFY(checkNowButton);
			QVERIFY(hoursSpin);
			QVERIFY(checkEveryLabel);

			QVERIFY(autoCheck->isEnabled());
			QVERIFY(checkNowButton->isEnabled());
			QVERIFY(hoursSpin->isEnabled());
			QVERIFY(checkEveryLabel->isEnabled());
			QCOMPARE(hoursSpin->value(), 6);

			autoCheck->setChecked(false);
			QCoreApplication::processEvents();
			QVERIFY(!hoursSpin->isEnabled());
			QVERIFY(!checkEveryLabel->isEnabled());
			QVERIFY(checkNowButton->isEnabled());

			autoCheck->setChecked(true);
			QCoreApplication::processEvents();
			QVERIFY(hoursSpin->isEnabled());
			QVERIFY(checkEveryLabel->isEnabled());
			QVERIFY(checkNowButton->isEnabled());
		}

		/**
		 * @brief Verifies `Check now` forwards to AppController with this dialog as parent.
		 */
		void checkNowCallsAppController()
		{
			resetStubState();

			GlobalPreferencesDialog dialog;
			dialog.show();

			QPushButton *checkNowButton = findButtonByText(dialog, QStringLiteral("Check now"));
			QVERIFY(checkNowButton);
			QTest::mouseClick(checkNowButton, Qt::LeftButton);

			QCOMPARE(stubState().checkForUpdatesNowCallCount, 1);
			QCOMPARE(stubState().lastUpdateCheckParent.data(), static_cast<QWidget *>(&dialog));
		}

		/**
		 * @brief Verifies update-related options persist through dialog acceptance.
		 */
		void acceptPersistsUpdateSettings()
		{
			resetStubState();

			GlobalPreferencesDialog dialog;
			dialog.show();

			QCheckBox *autoCheck =
			    findCheckBoxByText(dialog, QStringLiteral("Automatically check for updates"));
			QCheckBox *enableReload =
			    findCheckBoxByText(dialog, QStringLiteral("Enable reload feature (Linux/MacOS)"));
			QSpinBox *hoursSpin   = findSpinBySuffix(dialog, QStringLiteral(" hour(s)"));
			QSpinBox *timeoutSpin = findSpinBySuffix(dialog, QStringLiteral(" ms"));
			QVERIFY(autoCheck);
			QVERIFY(enableReload);
			QVERIFY(hoursSpin);
			QVERIFY(timeoutSpin);

			autoCheck->setChecked(false);
			hoursSpin->setValue(24);
			enableReload->setChecked(false);
			timeoutSpin->setValue(850);

			dialog.accept();

			QCOMPARE(stubState().globalOptions.value(QStringLiteral("AutoCheckForUpdates")).toInt(), 0);
			QCOMPARE(stubState().globalOptions.value(QStringLiteral("UpdateCheckIntervalHours")).toInt(), 24);
			QCOMPARE(stubState().globalOptions.value(QStringLiteral("EnableReloadFeature")).toInt(), 0);
			QCOMPARE(stubState().globalOptions.value(QStringLiteral("ReloadMccpDisableTimeoutMs")).toInt(),
			         850);
			QCOMPARE(stubState().applyGlobalPreferencesCallCount, 1);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_Dialog_GlobalPreferencesUpdates)

#include "tst_Dialog_GlobalPreferencesUpdates.moc"
