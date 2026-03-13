/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TipDialog.cpp
 * Role: Tip dialog implementation that loads, rotates, and displays contextual tips during startup/help flows.
 */

#include "dialogs/TipDialog.h"
#include "AppController.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>
#include <memory>

static const auto kTipFilePosKey   = QStringLiteral("Tip_FilePos");
static const auto kTipTimeStampKey = QStringLiteral("Tip_TimeStamp");
static const auto kTipStartupKey   = QStringLiteral("Tip_StartUp");

TipDialog::TipDialog(GetIntFn getInt, GetStringFn getString, WriteIntFn writeInt, WriteStringFn writeString,
                     QWidget *parent)
    : QDialog(parent), m_getInt(std::move(getInt)), m_getString(std::move(getString)),
      m_writeInt(std::move(writeInt)), m_writeString(std::move(writeString))
{
	setWindowTitle(QStringLiteral("Tip of the Day"));
	setMinimumSize(760, 420);

	auto layout = std::make_unique<QVBoxLayout>();

	auto title = std::make_unique<QLabel>(QStringLiteral("Did you know..."), this);
	auto f     = title->font();
	f.setBold(true);
	title->setFont(f);
	auto *const titleWidget = title.release();
	layout->addWidget(titleWidget);

	auto tipLabel = std::make_unique<QLabel>(this);
	m_tipLabel = tipLabel.release();
	m_tipLabel->setWordWrap(true);
	layout->addWidget(m_tipLabel, 1);

	auto startupCheck = std::make_unique<QCheckBox>(QStringLiteral("Show tips at startup"), this);
	m_startupCheck = startupCheck.release();
	layout->addWidget(m_startupCheck);

	auto buttonRow = std::make_unique<QHBoxLayout>();
	auto nextTip   = std::make_unique<QPushButton>(QStringLiteral("Next Tip"), this);
	buttonRow->addWidget(nextTip.get());
	buttonRow->addStretch(1);

	auto buttons = std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok, this);
	buttonRow->addWidget(buttons.get());
	auto *const buttonRowLayout = buttonRow.release();
	layout->addLayout(buttonRowLayout);
	auto *const rootLayout = layout.release();
	setLayout(rootLayout);

	// We need to find out what the startup and file position parameters are
	// If startup does not exist, we assume that the Tips on startup is checked TRUE.
	m_startup = m_getInt(QStringLiteral("control"), kTipStartupKey, 0) == 0;
	m_startupCheck->setChecked(m_startup);

	loadTipFile();

	// If Tips file does not exist then disable NextTip
	if (!m_file.isOpen())
		nextTip->setEnabled(false);

	m_tipLabel->setText(m_tipText);

	const auto nextTipConnection = connect(nextTip.get(), &QPushButton::clicked, this, &TipDialog::onNextTip);
	Q_UNUSED(nextTipConnection);
	const auto acceptConnection = connect(buttons.get(), &QDialogButtonBox::accepted, this, &TipDialog::onAccepted);
	Q_UNUSED(acceptConnection);
	auto *const nextTipButton = nextTip.release();
	Q_UNUSED(nextTipButton);
	auto *const buttonBox = buttons.release();
	Q_UNUSED(buttonBox);
}

TipDialog::~TipDialog()
{
	// This destructor is executed whether the user had pressed the escape key
	// or clicked on the close button. If the user had pressed the escape key,
	// it is still required to update the filepos in the ini file with the
	// latest position so that we don't repeat the tips!

	// But make sure the tips file existed in the first place....
	if (m_file.isOpen())
	{
		m_writeInt(QStringLiteral("control"), kTipFilePosKey, static_cast<int>(m_file.pos()));
		m_file.close();
	}
}

void TipDialog::onNextTip()
{
	getNextTipString(m_tipText);
	if (m_tipLabel)
		m_tipLabel->setText(m_tipText);
}

void TipDialog::onAccepted()
{
	// Update the startup information stored in the INI file
	// Tip_StartUp is inverted (1 == don't show at startup)
	if (m_startupCheck)
		m_startup = m_startupCheck->isChecked();
	m_writeInt(QStringLiteral("control"), kTipStartupKey, m_startup ? 0 : 1);
	accept();
}

void TipDialog::loadTipFile()
{
	const QString path = tipFilePath();
	m_file.setFileName(path);

	// Now try to open the tips file
	if (!m_file.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		m_tipText = QStringLiteral("Tips file not found.");
		return;
	}

	m_stream.setDevice(&m_file);

	// If the timestamp in the INI file is different from the timestamp of
	// the tips file, then we know that the tips file has been modified
	// Reset the file position to 0 and write the latest timestamp to the
	// ini file
	const QFileInfo info(m_file);
	const QString   currentTime = QLocale::c().toString(info.lastModified(), "ddd MMM d hh:mm:ss yyyy");
	const QString   storedTime  = m_getString(QStringLiteral("control"), kTipTimeStampKey, QString());
	if (currentTime != storedTime)
	{
		m_file.seek(0);
		m_writeString(QStringLiteral("control"), kTipTimeStampKey, currentTime);
	}
	else
	{
		const int pos = m_getInt(QStringLiteral("control"), kTipFilePosKey, 0);
		m_file.seek(pos);
	}

	getNextTipString(m_tipText);
}

void TipDialog::getNextTipString(QString &next)
{
	// This routine identifies the next string that needs to be
	// read from the tips file
	bool stop = false;
	while (!stop)
	{
		if (m_stream.atEnd())
		{
			// We have either reached EOF or encountered some problem
			// In both cases reset the pointer to the beginning of the file
			// Keep behavior aligned with legacy tip file handling.
			m_file.seek(0);
			m_stream.seek(0);
		}
		else
		{
			if (const QString line = m_stream.readLine(); !line.isEmpty())
			{
				if (const QChar c = line.at(0);
				    c != QChar(' ') && c != QChar('\t') && c != QChar('\n') && c != QChar(';'))
				{
					// There should be no space at the beginning of the tip
					// Keep behavior aligned with legacy tip file handling.
					// Comment lines are ignored and they start with a semicolon
					stop = true;
					next = line;
				}
			}
		}
	}
}

QString TipDialog::tipFilePath()
{
	if (const auto *app = AppController::instance())
	{
		if (QString startupTips = app->makeAbsolutePath(QStringLiteral("docs/tips.txt"));
		    QFileInfo::exists(startupTips))
			return startupTips;
		// Compatibility fallback for pre-migration datadirs.
		if (QString legacyTips = app->makeAbsolutePath(QStringLiteral("tips.txt"));
		    QFileInfo::exists(legacyTips))
			return legacyTips;
	}
	// fallback: look for docs/tips.txt in the same directory as the executable file
	if (const auto exeDir = QCoreApplication::applicationDirPath(); !exeDir.isEmpty())
		return exeDir + QStringLiteral("/docs/tips.txt");
	return QStringLiteral("docs/tips.txt");
}
