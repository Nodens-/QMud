/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Dialog_ImportXml.cpp
 * Role: QTest coverage for Dialog ImportXml behavior.
 */

#include "AppController.h"
#include "MainFrame.h"
// ReSharper disable once CppUnusedIncludeDirective
#include "NameGeneration.h"
#include "WorldChildWindow.h"
#include "WorldDocument.h"
#include "dialogs/ImportXmlDialog.h"
#include "dialogs/PluginsDialog.h"

#include <QCheckBox>
#include <QClipboard>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QtTest/QTest>

namespace
{
	struct ImportStubState
	{
			bool          importTextOk{true};
			unsigned long lastMask{0};
			int           importTextCallCount{0};
	};

	ImportStubState &stubState()
	{
		static ImportStubState state;
		return state;
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

	QCheckBox *findCheckBoxByText(const QObject &root, const QString &text)
	{
		const auto boxes = root.findChildren<QCheckBox *>();
		for (QCheckBox *box : boxes)
		{
			if (box && box->text() == text)
				return box;
		}
		return nullptr;
	}

	void closeAnyMessageBoxSoon()
	{
		QTimer::singleShot(0,
		                   []
		                   {
			                   for (QWidget *widget : QApplication::topLevelWidgets())
			                   {
				                   if (auto *box = qobject_cast<QMessageBox *>(widget))
					                   box->accept();
			                   }
		                   });
	}
} // namespace

// NOLINTBEGIN(readability-convert-member-functions-to-static)
AppController::AppController(QObject *parent) : QObject(parent)
{
}

AppController::~AppController() = default;

MainWindow *AppController::mainWindow() const
{
	return nullptr;
}

void AppController::changeToFileBrowsingDirectory() const
{
}

void AppController::changeToStartupDirectory() const
{
}

AppController::ImportResult AppController::importXmlFromFile(const QString &, unsigned long)
{
	return {};
}

AppController::ImportResult AppController::importXmlFromText(const QString &, unsigned long mask)
{
	auto &state = stubState();
	++state.importTextCallCount;
	state.lastMask = mask;

	ImportResult result;
	result.ok           = state.importTextOk;
	result.errorMessage = state.importTextOk ? QString() : QStringLiteral("Synthetic import failure");
	result.triggers     = 2;
	result.aliases      = 1;
	result.timers       = 0;
	result.macros       = 0;
	result.variables    = 0;
	result.colours      = 0;
	result.keypad       = 0;
	result.printing     = 0;
	result.duplicates   = 0;
	return result;
}

void AppController::onCommandTriggered(const QString &)
{
}

WorldChildWindow *MainWindow::activeWorldChildWindow() const
{
	return nullptr;
}

WorldRuntime *WorldChildWindow::runtime() const
{
	return nullptr;
}

PluginsDialog::PluginsDialog(WorldRuntime *, MainWindow *, QWidget *parent) : QDialog(parent)
{
}

void PluginsDialog::onAddPlugin()
{
}

void PluginsDialog::onRemovePlugin()
{
}

void PluginsDialog::onEnablePlugin() const
{
}

void PluginsDialog::onDisablePlugin() const
{
}

void PluginsDialog::onReloadPlugin()
{
}

void PluginsDialog::onEditPlugin() const
{
}

void PluginsDialog::onShowDescription() const
{
}

void PluginsDialog::onDeleteState()
{
}

void PluginsDialog::onSelectionChanged() const
{
}

void PluginsDialog::onMoveUp() const
{
}

void PluginsDialog::onMoveDown() const
{
}
// NOLINTEND(readability-convert-member-functions-to-static)

/**
 * @brief QTest fixture covering Dialog ImportXml scenarios.
 */
class tst_Dialog_ImportXml : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void clipboardButtonTracksXmlValidity()
		{
			auto &state               = stubState();
			state.importTextCallCount = 0;
			state.lastMask            = 0;

			QClipboard *clipboard = QGuiApplication::clipboard();
			QVERIFY(clipboard);

			clipboard->setText(QStringLiteral("plain text"));
			ImportXmlDialog dialog(nullptr);
			QPushButton    *importClipboard = findButtonByText(dialog, QStringLiteral("Import Clipboard"));
			QVERIFY(importClipboard);
			QVERIFY(!importClipboard->isEnabled());

			clipboard->setText(QStringLiteral("<?xml version=\"1.0\"?><qmud><x/></qmud>"));
			QCoreApplication::processEvents();
			QVERIFY(importClipboard->isEnabled());
		}

		void closeButtonRejectsDialog()
		{
			ImportXmlDialog dialog(nullptr);
			dialog.show();

			QPushButton *closeButton = findButtonByText(dialog, QStringLiteral("Close"));
			QVERIFY(closeButton);
			QTest::mouseClick(closeButton, Qt::LeftButton);
			QCOMPARE(dialog.result(), static_cast<int>(QDialog::Rejected));
		}

		void importClipboardAcceptsOnSuccessAndUsesMask()
		{
			auto &state               = stubState();
			state.importTextOk        = true;
			state.importTextCallCount = 0;
			state.lastMask            = 0;

			QClipboard *clipboard = QGuiApplication::clipboard();
			QVERIFY(clipboard);
			clipboard->setText(QStringLiteral("<?xml version=\"1.0\"?><triggers></triggers>"));

			AppController   app;
			ImportXmlDialog dialog(&app);
			dialog.show();

			QCheckBox *triggers = findCheckBoxByText(dialog, QStringLiteral("Triggers"));
			QCheckBox *aliases  = findCheckBoxByText(dialog, QStringLiteral("Aliases"));
			QVERIFY(triggers);
			QVERIFY(aliases);
			triggers->setChecked(true);
			aliases->setChecked(true);

			closeAnyMessageBoxSoon();
			QVERIFY(QMetaObject::invokeMethod(&dialog, "onImportClipboard", Qt::DirectConnection));

			QCOMPARE(state.importTextCallCount, 1);
			QCOMPARE(state.lastMask, WorldDocument::XML_TRIGGERS | WorldDocument::XML_ALIASES);
			QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
		}

		void importClipboardFailureDoesNotAcceptDialog()
		{
			auto &state               = stubState();
			state.importTextOk        = false;
			state.importTextCallCount = 0;
			state.lastMask            = 0;

			QClipboard *clipboard = QGuiApplication::clipboard();
			QVERIFY(clipboard);
			clipboard->setText(QStringLiteral("<?xml version=\"1.0\"?><aliases></aliases>"));

			AppController   app;
			ImportXmlDialog dialog(&app);
			dialog.show();

			QCheckBox *aliases = findCheckBoxByText(dialog, QStringLiteral("Aliases"));
			QVERIFY(aliases);
			aliases->setChecked(true);

			closeAnyMessageBoxSoon();
			QVERIFY(QMetaObject::invokeMethod(&dialog, "onImportClipboard", Qt::DirectConnection));

			QCOMPARE(state.importTextCallCount, 1);
			QCOMPARE(state.lastMask, WorldDocument::XML_ALIASES);
			QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_Dialog_ImportXml)


#if __has_include("tst_Dialog_ImportXml.moc")
#include "tst_Dialog_ImportXml.moc"
#endif
