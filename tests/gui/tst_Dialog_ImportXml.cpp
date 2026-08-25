/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Dialog_ImportXml.cpp
 * Role: QTest coverage for Dialog ImportXml behavior.
 */

#include "AppController.h"
#include "MainFrame.h"
#include "WorldChildWindow.h"
#include "WorldRuntime.h"
#include "dialogs/ImportXmlDialog.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QMessageBox>
#include <QPushButton>
#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Captures and dismisses modal import results so GUI tests cannot block.
	 */
	class MessageBoxObserver final : public QObject
	{
		public:
			/**
			 * @brief Returns the first captured message-box text.
			 * @return Captured text, or an empty string when no message box appeared.
			 */
			[[nodiscard]] QString message() const
			{
				return m_message;
			}

			/**
			 * @brief Captures message text and queues rejection of the modal dialog.
			 * @param watched Object receiving the event.
			 * @param event Event delivered to the watched object.
			 * @return Base event-filter result.
			 */
			bool eventFilter(QObject *watched, QEvent *event) override
			{
				if (event && event->type() == QEvent::Show)
				{
					if (auto *messageBox = qobject_cast<QMessageBox *>(watched))
					{
						if (m_message.isEmpty())
							m_message = messageBox->text();
						QMetaObject::invokeMethod(messageBox, &QDialog::reject, Qt::QueuedConnection);
					}
				}
				return QObject::eventFilter(watched, event);
			}

		private:
			QString m_message;
	};

	/**
	 * @brief Production application/window/runtime harness for XML imports.
	 */
	struct ActiveWorldHarness
	{
			/**
			 * @brief Creates and activates a production world presentation.
			 */
			ActiveWorldHarness()
			{
				app.setMainWindow(&frame);
				runtime = new WorldRuntime(&frame);
				runtime->setStartupDirectory(QDir::currentPath());
				child = new WorldChildWindow(QStringLiteral("Import Test"));
				child->setRuntime(runtime);
				frame.addMdiSubWindow(child, true);
				QCoreApplication::processEvents();
			}

			/**
			 * @brief Detaches the runtime before the owning frame destroys its children.
			 */
			~ActiveWorldHarness()
			{
				if (child)
					child->setRuntime(nullptr);
			}

			AppController     app;
			MainWindow        frame;
			WorldRuntime     *runtime{nullptr};
			WorldChildWindow *child{nullptr};
	};

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

} // namespace

namespace
{
	/**
	 * @brief QTest fixture covering Dialog ImportXml scenarios.
	 */
	class tst_Dialog_ImportXml : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			void initTestCase()
			{
				m_originalCurrentPath = QDir::currentPath();
				m_hadOriginalTmpDir   = qEnvironmentVariableIsSet("TMPDIR");
				m_originalTmpDir      = qgetenv("TMPDIR");
				const QString artifactDirectory =
				    QDir(QCoreApplication::applicationDirPath())
				        .filePath(QStringLiteral("test-artifacts/tst_Dialog_ImportXml/%1")
				                      .arg(QCoreApplication::applicationPid()));
				QVERIFY(QDir().mkpath(artifactDirectory));
				QVERIFY(QDir::setCurrent(artifactDirectory));
				QVERIFY(qputenv("TMPDIR", artifactDirectory.toUtf8()));
			}

			void cleanupTestCase() const
			{
				if (m_hadOriginalTmpDir)
					QVERIFY(qputenv("TMPDIR", m_originalTmpDir));
				else
					qunsetenv("TMPDIR");
				QVERIFY(QDir::setCurrent(m_originalCurrentPath));
			}

			void clipboardButtonTracksXmlValidity()
			{
				QClipboard *clipboard = QGuiApplication::clipboard();
				QVERIFY(clipboard);

				clipboard->setText(QStringLiteral("plain text"));
				ImportXmlDialog dialog(nullptr);
				QPushButton *importClipboard = findButtonByText(dialog, QStringLiteral("Import Clipboard"));
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
				QClipboard *clipboard = QGuiApplication::clipboard();
				QVERIFY(clipboard);
				clipboard->setText(QStringLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<qmud>
  <triggers>
    <trigger name="first" match="^first$"><send>one</send></trigger>
    <trigger name="second" match="^second$"><send>two</send></trigger>
  </triggers>
  <aliases>
    <alias name="alias_one" match="^alias$"><send>alias</send></alias>
  </aliases>
  <timers>
    <timer name="unchecked_timer" hour="0" minute="1" second="0"><send>timer</send></timer>
  </timers>
</qmud>)xml"));

				ActiveWorldHarness harness;
				ImportXmlDialog    dialog(&harness.app);
				dialog.show();

				QCheckBox *triggers = findCheckBoxByText(dialog, QStringLiteral("Triggers"));
				QCheckBox *aliases  = findCheckBoxByText(dialog, QStringLiteral("Aliases"));
				QVERIFY(triggers);
				QVERIFY(aliases);
				triggers->setChecked(true);
				aliases->setChecked(true);

				MessageBoxObserver messageObserver;
				QApplication::instance()->installEventFilter(&messageObserver);
				QVERIFY(QMetaObject::invokeMethod(&dialog, "onImportClipboard", Qt::DirectConnection));
				QApplication::instance()->removeEventFilter(&messageObserver);

				QVERIFY(!messageObserver.message().isEmpty());
				QCOMPARE(harness.runtime->triggers().size(), 2);
				QCOMPARE(harness.runtime->aliases().size(), 1);
				QVERIFY(harness.runtime->timers().isEmpty());
				QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
			}

			void importClipboardFailureDoesNotAcceptDialog()
			{
				QClipboard *clipboard = QGuiApplication::clipboard();
				QVERIFY(clipboard);
				clipboard->setText(
				    QStringLiteral("<?xml version=\"1.0\"?><qmud><aliases><alias name=\"broken\">"));

				ActiveWorldHarness harness;
				ImportXmlDialog    dialog(&harness.app);
				dialog.show();

				QCheckBox *aliases = findCheckBoxByText(dialog, QStringLiteral("Aliases"));
				QVERIFY(aliases);
				aliases->setChecked(true);

				MessageBoxObserver messageObserver;
				QApplication::instance()->installEventFilter(&messageObserver);
				QVERIFY(QMetaObject::invokeMethod(&dialog, "onImportClipboard", Qt::DirectConnection));
				QApplication::instance()->removeEventFilter(&messageObserver);

				QVERIFY(!messageObserver.message().isEmpty());
				QVERIFY(harness.runtime->aliases().isEmpty());
				QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
			}
			// NOLINTEND(readability-convert-member-functions-to-static)

		private:
			QString    m_originalCurrentPath;
			QByteArray m_originalTmpDir;
			bool       m_hadOriginalTmpDir{false};
	};
} // namespace

QTEST_MAIN(tst_Dialog_ImportXml)

#if __has_include("tst_Dialog_ImportXml.moc")
#include "tst_Dialog_ImportXml.moc"
#endif
