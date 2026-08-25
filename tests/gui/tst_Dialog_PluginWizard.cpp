/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Dialog_PluginWizard.cpp
 * Role: QTest coverage for Plugin Wizard layout and selection behavior.
 */

#include "WorldRuntime.h"
#include "dialogs/PluginWizardDialog.h"

#include <QCheckBox>
// ReSharper disable once CppUnusedIncludeDirective
#include <QCoreApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QtTest/QTest>

namespace
{
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
		const auto checks = root.findChildren<QCheckBox *>();
		for (QCheckBox *check : checks)
		{
			if (check && check->text() == text)
				return check;
		}
		return nullptr;
	}

	QLineEdit *page1NameEdit(const QWizardPage *page)
	{
		if (!page)
			return nullptr;
		auto *form = page->findChild<QFormLayout *>();
		if (!form)
			return nullptr;
		QLayoutItem *fieldItem = form->itemAt(0, QFormLayout::FieldRole);
		if (!fieldItem)
			return nullptr;
		return qobject_cast<QLineEdit *>(fieldItem->widget());
	}

	WorldRuntime::Trigger makeTrigger(const QString &name, const bool temporary)
	{
		WorldRuntime::Trigger trigger;
		trigger.attributes.insert(QStringLiteral("name"), name);
		trigger.attributes.insert(QStringLiteral("match"), QStringLiteral("^%1$").arg(name));
		trigger.attributes.insert(QStringLiteral("group"), QStringLiteral("g"));
		trigger.attributes.insert(QStringLiteral("temporary"),
		                          temporary ? QStringLiteral("y") : QStringLiteral("n"));
		trigger.children.insert(QStringLiteral("send"), QStringLiteral("say %1").arg(name));
		return trigger;
	}

	class PluginWizardDialogAccess : public PluginWizardDialog
	{
		public:
			using PluginWizardDialog::accept;
			using PluginWizardDialog::PluginWizardDialog;
	};

	/**
	 * @brief QTest fixture covering Plugin Wizard page layout and interactions.
	 */
	class tst_Dialog_PluginWizard : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			void nestedHorizontalLayoutsAreNotParentedToPage()
			{
				WorldRuntime             runtime;
				PluginWizardDialogAccess dialog(&runtime, nullptr);
				dialog.show();
				QCoreApplication::processEvents();

				for (int pageId = 0; pageId <= 7; ++pageId)
				{
					auto *page = qobject_cast<QWizardPage *>(dialog.page(pageId));
					QVERIFY(page);
					const auto hBoxes = page->findChildren<QHBoxLayout *>();
					for (QHBoxLayout *box : hBoxes)
					{
						QVERIFY(box);
						QVERIFY2(box->parent() != page,
						         "Nested QHBoxLayout must not be parented to page widget.");
					}
				}
			}

			void descriptionHelpAliasFollowsDescriptionAndToggle()
			{
				WorldRuntime             runtime;
				PluginWizardDialogAccess dialog(&runtime, nullptr);
				dialog.show();
				QCoreApplication::processEvents();

				auto *detailsPage = qobject_cast<QWizardPage *>(dialog.page(0));
				QVERIFY(detailsPage);
				QLineEdit *nameEdit = page1NameEdit(detailsPage);
				QVERIFY(nameEdit);

				dialog.setCurrentId(1);
				QCoreApplication::processEvents();
				auto *descriptionPage = qobject_cast<QWizardPage *>(dialog.page(1));
				QVERIFY(descriptionPage);

				auto descriptionEdit = descriptionPage->findChild<QTextEdit *>();
				QVERIFY(descriptionEdit);
				QCheckBox *generateHelp =
				    findCheckBoxByText(*descriptionPage, QStringLiteral("Generate help alias"));
				QVERIFY(generateHelp);
				auto helpAliasEdit = descriptionPage->findChild<QLineEdit *>();
				QVERIFY(helpAliasEdit);

				QVERIFY(!generateHelp->isEnabled());
				QVERIFY(!helpAliasEdit->isEnabled());

				nameEdit->setText(QStringLiteral("QuestTools"));
				QCoreApplication::processEvents();
				QCOMPARE(helpAliasEdit->text(), QStringLiteral("QuestTools:help"));

				descriptionEdit->setPlainText(QStringLiteral("Show quest details."));
				QCoreApplication::processEvents();
				QVERIFY(generateHelp->isEnabled());
				QVERIFY(helpAliasEdit->isEnabled());

				generateHelp->setChecked(false);
				QCoreApplication::processEvents();
				QVERIFY(!helpAliasEdit->isEnabled());
			}

			void triggerSelectionButtonsDriveAcceptedSelectionState()
			{
				WorldRuntime runtime;
				runtime.setTriggers({makeTrigger(QStringLiteral("one"), false),
				                     makeTrigger(QStringLiteral("temp"), true),
				                     makeTrigger(QStringLiteral("two"), false)});

				PluginWizardDialogAccess dialog(&runtime, nullptr);
				dialog.show();
				QCoreApplication::processEvents();

				dialog.setCurrentId(2);
				QCoreApplication::processEvents();

				auto *triggerPage = qobject_cast<QWizardPage *>(dialog.page(2));
				QVERIFY(triggerPage);
				auto *table = triggerPage->findChild<QTableWidget *>();
				QVERIFY(table);
				QCOMPARE(table->rowCount(), 2); // temporary rows are filtered out

				QPushButton *selectAllButton  = findButtonByText(*triggerPage, QStringLiteral("Select All"));
				QPushButton *selectNoneButton = findButtonByText(*triggerPage, QStringLiteral("Select None"));
				QVERIFY(selectAllButton);
				QVERIFY(selectNoneButton);

				QTest::mouseClick(selectNoneButton, Qt::LeftButton);
				QCoreApplication::processEvents();
				QCOMPARE(table->selectionModel()->selectedRows().size(), 0);

				table->selectRow(1); // should map back to source index 2
				QCoreApplication::processEvents();

				auto *detailsPage = qobject_cast<QWizardPage *>(dialog.page(0));
				QVERIFY(detailsPage);
				QLineEdit *nameEdit = page1NameEdit(detailsPage);
				QVERIFY(nameEdit);
				nameEdit->setText(QStringLiteral("PluginExport"));

				dialog.accept();
				QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
				QCOMPARE(dialog.state().selectedTriggers.size(), 1);
				QVERIFY(dialog.state().selectedTriggers.contains(2));
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};
} // namespace

QTEST_MAIN(tst_Dialog_PluginWizard)

#if __has_include("tst_Dialog_PluginWizard.moc")
#include "tst_Dialog_PluginWizard.moc"
#endif
