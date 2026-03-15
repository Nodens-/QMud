/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TelnetCallbackSpy.cpp
 * Role: Test-support callback spy implementation for capturing TelnetProcessor events in integration tests.
 */

#include "TelnetCallbackSpy.h"

TelnetProcessor::Callbacks TelnetCallbackSpy::callbacks()
{
	TelnetProcessor::Callbacks cb;

	cb.onTelnetRequest = [this](const int option, const QString &type)
	{
		requests.append({option, type});
		return telnetRequestResult;
	};

	cb.onTelnetSubnegotiation = [this](const int option, const QByteArray &data)
	{
		subnegotiations.append({option, data});
	};

	cb.onTelnetOption = [this](const QByteArray &data) { telnetOptions.append(data); };

	cb.onIacGa = [this]() { ++iacGaCount; };

	cb.onMxpStart = [this](const bool pueblo, const bool manual) { mxpStarts.append({pueblo, manual}); };

	cb.onMxpReset = [this]() { ++mxpResetCount; };

	cb.onMxpStop = [this](const bool completely, const bool puebloActive)
	{
		mxpStops.append({completely, puebloActive});
	};

	cb.onMxpModeChange = [this](const int oldMode, const int newMode, const bool shouldLog)
	{
		mxpModeChanges.append({oldMode, newMode, shouldLog});
	};

	cb.onMxpDiagnostic = [this](const int level, const long messageNumber, const QString &message)
	{
		mxpDiagnostics.append({level, messageNumber, message});
	};

	cb.onMxpDiagnosticNeeded = [this](int) { return mxpDiagnosticNeeded; };

	cb.onNoEchoChanged = [this](const bool enabled) { noEchoStates.append(enabled); };

	cb.onFatalProtocolError = [this](const QString &message) { fatalProtocolErrors.append(message); };

	return cb;
}

