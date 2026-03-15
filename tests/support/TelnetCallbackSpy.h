/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TelnetCallbackSpy.h
 * Role: Test-support callback spy declarations for capturing TelnetProcessor events in integration tests.
 */

#ifndef QMUD_TELNETCALLBACKSPY_H
#define QMUD_TELNETCALLBACKSPY_H

#include "TelnetProcessor.h"

#include <QByteArray>
#include <QList>
#include <QString>

/**
 * @brief Mutable spy sink that records `TelnetProcessor` callback activity for assertions.
 */
struct TelnetCallbackSpy
{
	struct Request
	{
			int     option{0};
			QString type;
	};

	struct Subnegotiation
	{
			int        option{0};
			QByteArray data;
	};

	struct MxpStart
	{
			bool pueblo{false};
			bool manual{false};
	};

	struct MxpStop
	{
			bool completely{false};
			bool puebloActive{false};
	};

	struct MxpModeChange
	{
			int  oldMode{0};
			int  newMode{0};
			bool shouldLog{false};
	};

	struct MxpDiagnostic
	{
			int     level{0};
			long    messageNumber{0};
			QString message;
	};

	bool                        telnetRequestResult{false};
	bool                        mxpDiagnosticNeeded{true};
	QList<Request>              requests;
	QList<Subnegotiation>       subnegotiations;
	QList<QByteArray>           telnetOptions;
	int                         iacGaCount{0};
	QList<MxpStart>             mxpStarts;
	int                         mxpResetCount{0};
	QList<MxpStop>              mxpStops;
	QList<MxpModeChange>        mxpModeChanges;
	QList<MxpDiagnostic>        mxpDiagnostics;
	QList<bool>                 noEchoStates;
	QStringList                 fatalProtocolErrors;

	/**
	 * @brief Builds callback table wired to this spy's capture buffers/counters.
	 * @return Callback set consumable by `TelnetProcessor`.
	 */
	[[nodiscard]] TelnetProcessor::Callbacks callbacks();
};

#endif // QMUD_TELNETCALLBACKSPY_H
