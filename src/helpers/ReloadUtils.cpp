/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ReloadUtils.cpp
 * Role: Consolidated reload/copyover state, policy, recovery, and MCCP probe helper implementations.
 */

#include "ReloadUtils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cmath>
#include <limits>

namespace
{
	constexpr int kReloadStateSchemaVersion = 1;

	QString       policyToString(const ReloadSocketPolicy policy)
	{
		return policy == ReloadSocketPolicy::ParkReconnect ? QStringLiteral("park_reconnect")
		                                                   : QStringLiteral("reattach");
	}

	bool policyFromString(const QString &value, ReloadSocketPolicy &policy)
	{
		if (value.compare(QStringLiteral("reattach"), Qt::CaseInsensitive) == 0)
		{
			policy = ReloadSocketPolicy::Reattach;
			return true;
		}
		if (value.compare(QStringLiteral("park_reconnect"), Qt::CaseInsensitive) == 0)
		{
			policy = ReloadSocketPolicy::ParkReconnect;
			return true;
		}
		return false;
	}

	QString parseArgValue(const QStringList &arguments, const QString &name)
	{
		const QString prefix = name + QLatin1Char('=');
		for (const QString &arg : arguments)
		{
			if (arg.startsWith(prefix))
				return arg.mid(prefix.size());
		}
		return {};
	}

	bool parseBoolField(const QJsonObject &object, const char *name, bool &value)
	{
		if (!object.contains(QLatin1String(name)))
			return false;
		const QJsonValue raw = object.value(QLatin1String(name));
		if (!raw.isBool())
			return false;
		value = raw.toBool();
		return true;
	}

	bool parseIntField(const QJsonObject &object, const char *name, int &value)
	{
		if (!object.contains(QLatin1String(name)))
			return false;
		const QJsonValue raw = object.value(QLatin1String(name));
		if (!raw.isDouble())
			return false;
		const double parsed = raw.toDouble();
		if (!std::isfinite(parsed))
			return false;
		if (std::trunc(parsed) != parsed)
			return false;
		constexpr auto kIntMin = double{std::numeric_limits<int>::min()};
		constexpr auto kIntMax = double{std::numeric_limits<int>::max()};
		if (parsed < kIntMin || parsed > kIntMax)
			return false;
		value = static_cast<int>(parsed);
		return true;
	}

	bool parsePortField(const QJsonObject &object, quint16 &value)
	{
		int parsed = 0;
		if (!parseIntField(object, "port", parsed))
			return false;
		if (parsed < 0 || parsed > std::numeric_limits<quint16>::max())
			return false;
		value = static_cast<quint16>(parsed);
		return true;
	}

	QString normalizedPathForCompare(const QString &path)
	{
		const QFileInfo info(path);
		QString         normalized = info.canonicalFilePath();
		if (normalized.isEmpty())
			normalized = info.absoluteFilePath();
		return QDir::cleanPath(normalized);
	}
} // namespace

QString reloadStateDefaultPath(const QString &workingDirectory)
{
	if (workingDirectory.trimmed().isEmpty())
		return QStringLiteral("reload_state.json");
	QDir dir(workingDirectory);
	return dir.filePath(QStringLiteral("reload_state.json"));
}

bool parseReloadStartupArguments(const QStringList &arguments, QString *statePath, QString *token)
{
	if (!statePath || !token)
		return false;
	const QString parsedStatePath = parseArgValue(arguments, QStringLiteral("--reload-state"));
	const QString parsedToken     = parseArgValue(arguments, QStringLiteral("--reload-token"));
	if (parsedStatePath.trimmed().isEmpty() || parsedToken.trimmed().isEmpty())
		return false;
	*statePath = parsedStatePath.trimmed();
	*token     = parsedToken.trimmed();
	return true;
}

QStringList filterReloadStartupArguments(const QStringList &arguments)
{
	QStringList filtered;
	filtered.reserve(arguments.size());

	for (const QString &arg : arguments)
	{
		if (arg.startsWith(QStringLiteral("--reload-state=")) ||
		    arg.startsWith(QStringLiteral("--reload-token=")))
		{
			continue;
		}
		filtered.push_back(arg);
	}
	return filtered;
}

bool writeReloadStateSnapshot(const QString &filePath, const ReloadStateSnapshot &snapshot,
                              QString *errorMessage)
{
	if (errorMessage)
		errorMessage->clear();
	if (filePath.trimmed().isEmpty())
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Reload state path is empty.");
		return false;
	}

	QJsonObject root;
	root.insert(QStringLiteral("schema_version"),
	            snapshot.schemaVersion > 0 ? snapshot.schemaVersion : kReloadStateSchemaVersion);
	root.insert(QStringLiteral("created_at_utc"),
	            (snapshot.createdAtUtc.isValid() ? snapshot.createdAtUtc : QDateTime::currentDateTimeUtc())
	                .toUTC()
	                .toString(Qt::ISODateWithMs));
	root.insert(QStringLiteral("reload_token"), snapshot.reloadToken);
	root.insert(QStringLiteral("target_executable"), snapshot.targetExecutable);
	root.insert(QStringLiteral("active_world_sequence"), snapshot.activeWorldSequence);

	QJsonArray args;
	for (const QString &arg : snapshot.arguments)
		args.append(arg);
	root.insert(QStringLiteral("arguments"), args);

	QJsonArray worlds;
	for (const ReloadWorldState &world : snapshot.worlds)
	{
		QJsonObject entry;
		entry.insert(QStringLiteral("sequence"), world.sequence);
		entry.insert(QStringLiteral("world_id"), world.worldId);
		entry.insert(QStringLiteral("display_name"), world.displayName);
		entry.insert(QStringLiteral("world_file_path"), world.worldFilePath);
		entry.insert(QStringLiteral("host"), world.host);
		entry.insert(QStringLiteral("port"), static_cast<int>(world.port));
		entry.insert(QStringLiteral("fd"), world.socketDescriptor);
		entry.insert(QStringLiteral("policy"), policyToString(world.policy));
		entry.insert(QStringLiteral("connected_at_reload"), world.connectedAtReload);
		entry.insert(QStringLiteral("mccp_was_active"), world.mccpWasActive);
		entry.insert(QStringLiteral("mccp_disable_attempted"), world.mccpDisableAttempted);
		entry.insert(QStringLiteral("mccp_disable_succeeded"), world.mccpDisableSucceeded);
		entry.insert(QStringLiteral("utf8_enabled"), world.utf8Enabled);
		entry.insert(QStringLiteral("notes"), world.notes);
		worlds.append(entry);
	}
	root.insert(QStringLiteral("worlds"), worlds);

	QSaveFile file(filePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Unable to open reload state file for writing: %1").arg(filePath);
		return false;
	}

	const QJsonDocument doc(root);
	if (const QByteArray payload = doc.toJson(QJsonDocument::Indented);
	    file.write(payload.constData(), payload.size()) != payload.size())
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Unable to write reload state file: %1").arg(filePath);
		return false;
	}
	if (!file.commit())
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Unable to commit reload state file: %1").arg(filePath);
		return false;
	}
	return true;
}

bool readReloadStateSnapshot(const QString &filePath, ReloadStateSnapshot *snapshot, QString *errorMessage)
{
	if (errorMessage)
		errorMessage->clear();
	if (!snapshot)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Reload snapshot output pointer is null.");
		return false;
	}
	if (filePath.trimmed().isEmpty())
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Reload state path is empty.");
		return false;
	}

	QFile file(filePath);
	if (!file.open(QIODevice::ReadOnly))
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Unable to open reload state file: %1").arg(filePath);
		return false;
	}

	QJsonParseError     parseError;
	const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject())
	{
		if (errorMessage)
		{
			*errorMessage =
			    parseError.error == QJsonParseError::NoError
			        ? QStringLiteral("Reload state JSON root is not an object.")
			        : QStringLiteral("Reload state JSON parse error: %1").arg(parseError.errorString());
		}
		return false;
	}

	const QJsonObject root          = doc.object();
	int               schemaVersion = 0;
	if (!parseIntField(root, "schema_version", schemaVersion) || schemaVersion != kReloadStateSchemaVersion)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Unsupported reload state schema version: %1").arg(schemaVersion);
		return false;
	}

	const QString createdAtRaw = root.value(QStringLiteral("created_at_utc")).toString().trimmed();
	const QString reloadToken  = root.value(QStringLiteral("reload_token")).toString().trimmed();
	const QString targetExe    = root.value(QStringLiteral("target_executable")).toString().trimmed();
	if (createdAtRaw.isEmpty() || reloadToken.isEmpty() || targetExe.isEmpty())
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Reload state missing required top-level fields.");
		return false;
	}

	const QDateTime createdAtUtc = QDateTime::fromString(createdAtRaw, Qt::ISODateWithMs).toUTC();
	if (!createdAtUtc.isValid())
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Reload state contains invalid created_at_utc timestamp.");
		return false;
	}

	const QJsonArray    worldsRaw = root.value(QStringLiteral("worlds")).toArray();
	const QJsonArray    argsRaw   = root.value(QStringLiteral("arguments")).toArray();

	ReloadStateSnapshot parsed;
	parsed.schemaVersion       = schemaVersion;
	parsed.createdAtUtc        = createdAtUtc;
	parsed.reloadToken         = reloadToken;
	parsed.targetExecutable    = targetExe;
	parsed.activeWorldSequence = root.value(QStringLiteral("active_world_sequence")).toInt(0);
	parsed.arguments.reserve(argsRaw.size());
	for (const auto &arg : argsRaw)
		parsed.arguments.push_back(arg.toString());

	parsed.worlds.reserve(worldsRaw.size());
	for (const auto &rawEntry : worldsRaw)
	{
		if (!rawEntry.isObject())
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Reload state contains non-object world entry.");
			return false;
		}
		const QJsonObject object = rawEntry.toObject();
		ReloadWorldState  world;
		if (!parseIntField(object, "sequence", world.sequence) ||
		    !parseIntField(object, "fd", world.socketDescriptor) || !parsePortField(object, world.port))
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Reload world entry is missing required numeric fields.");
			return false;
		}
		world.worldId       = object.value(QStringLiteral("world_id")).toString().trimmed();
		world.displayName   = object.value(QStringLiteral("display_name")).toString().trimmed();
		world.worldFilePath = object.value(QStringLiteral("world_file_path")).toString();
		world.host          = object.value(QStringLiteral("host")).toString().trimmed();
		world.notes         = object.value(QStringLiteral("notes")).toString();
		if (!policyFromString(object.value(QStringLiteral("policy")).toString(), world.policy))
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Reload world entry has invalid policy.");
			return false;
		}
		if (!parseBoolField(object, "connected_at_reload", world.connectedAtReload) ||
		    !parseBoolField(object, "mccp_was_active", world.mccpWasActive) ||
		    !parseBoolField(object, "mccp_disable_attempted", world.mccpDisableAttempted) ||
		    !parseBoolField(object, "mccp_disable_succeeded", world.mccpDisableSucceeded) ||
		    !parseBoolField(object, "utf8_enabled", world.utf8Enabled))
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Reload world entry has invalid boolean fields.");
			return false;
		}
		parsed.worlds.push_back(world);
	}

	*snapshot = parsed;
	return true;
}

bool validateReloadStartupSnapshot(const ReloadStateSnapshot          &snapshot,
                                   const ReloadStartupValidationInput &input, QString *errorMessage)
{
	if (errorMessage)
		errorMessage->clear();

	if (const QString expectedToken = input.expectedToken.trimmed();
	    expectedToken.isEmpty() || snapshot.reloadToken.trimmed() != expectedToken)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Reload token mismatch.");
		return false;
	}

	const QString expectedExecutable = normalizedPathForCompare(input.expectedTargetExecutable);
	const QString snapshotExecutable = normalizedPathForCompare(snapshot.targetExecutable);
	if (!expectedExecutable.isEmpty() && !snapshotExecutable.isEmpty() &&
	    expectedExecutable.compare(snapshotExecutable, Qt::CaseSensitive) != 0)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Reload target executable mismatch.");
		return false;
	}
	return true;
}

bool loadValidatedAndConsumeReloadStateSnapshot(const QString                      &filePath,
                                                const ReloadStartupValidationInput &input,
                                                ReloadStateSnapshot *snapshot, QString *errorMessage,
                                                QString *cleanupWarning)
{
	if (errorMessage)
		errorMessage->clear();
	if (cleanupWarning)
		cleanupWarning->clear();
	if (!snapshot)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Reload snapshot output pointer is null.");
		return false;
	}

	ReloadStateSnapshot parsed;
	if (!readReloadStateSnapshot(filePath, &parsed, errorMessage))
	{
		QString cleanupError;
		if (!removeReloadStateFile(filePath, &cleanupError) && !cleanupError.isEmpty() && cleanupWarning)
			*cleanupWarning = cleanupError;
		return false;
	}

	QString validationError;
	if (!validateReloadStartupSnapshot(parsed, input, &validationError))
	{
		if (errorMessage)
			*errorMessage = validationError;
		QString cleanupError;
		if (!removeReloadStateFile(filePath, &cleanupError) && !cleanupError.isEmpty() && cleanupWarning)
			*cleanupWarning = cleanupError;
		return false;
	}

	QString consumeError;
	if (!removeReloadStateFile(filePath, &consumeError) && !consumeError.isEmpty() && cleanupWarning)
		*cleanupWarning = consumeError;

	*snapshot = parsed;
	return true;
}

bool removeReloadStateFile(const QString &filePath, QString *errorMessage)
{
	if (errorMessage)
		errorMessage->clear();
	if (filePath.trimmed().isEmpty())
		return true;
	QFile file(filePath);
	if (!file.exists())
		return true;
	if (file.remove())
		return true;
	if (errorMessage)
		*errorMessage = QStringLiteral("Unable to remove reload state file: %1").arg(filePath);
	return false;
}

bool isReloadStateFileStale(const QString &filePath, const int maxAgeSeconds, const QDateTime &nowUtc)
{
	if (filePath.trimmed().isEmpty() || maxAgeSeconds <= 0)
		return false;
	const QFileInfo info(filePath);
	if (!info.exists())
		return false;

	const QDateTime modifiedUtc = info.lastModified().toUTC();
	if (!modifiedUtc.isValid() || !nowUtc.isValid())
		return true;
	return modifiedUtc.secsTo(nowUtc) > maxAgeSeconds;
}

bool shouldAttemptReloadMccpDisable(const bool connected, const int socketDescriptor, const bool tlsEnabled,
                                    const bool mccpWasActive)
{
	return connected && socketDescriptor >= 0 && !tlsEnabled && mccpWasActive;
}

ReloadWorldPolicyDecision computeReloadWorldPolicy(const ReloadWorldPolicyInput &input)
{
	ReloadWorldPolicyDecision decision;
	decision.connectedAtReload = input.connected || input.connecting;

	if (!decision.connectedAtReload)
	{
		decision.policy = ReloadSocketPolicy::Reattach;
		return decision;
	}

	if (input.tlsEnabled)
	{
		decision.shouldAttemptDescriptorInheritance = false;
		decision.policy                             = ReloadSocketPolicy::ParkReconnect;
		return decision;
	}
	decision.shouldAttemptDescriptorInheritance = input.socketDescriptor >= 0;

	if (input.connected && input.socketDescriptor >= 0)
	{
		decision.policy = ReloadSocketPolicy::Reattach;
		return decision;
	}

	decision.policy = ReloadSocketPolicy::ParkReconnect;
	return decision;
}

ReloadRecoverySocketDecision applyReloadSocketRecovery(ReloadSocketRecoveryOps &runtime,
                                                       const ReloadWorldState  &worldState)
{
	ReloadRecoverySocketDecision decision;

	if (worldState.connectedAtReload && worldState.socketDescriptor >= 0)
	{
		runtime.markReloadReattachConnectActionsSuppressed();
		QString adoptError;
		if (!runtime.adoptConnectedSocketDescriptor(worldState.socketDescriptor, &adoptError))
		{
			(void)runtime.consumeReloadReattachConnectActionsSuppressed();
			decision.outcome          = ReloadRecoverySocketOutcome::ReconnectQueued;
			decision.closeSocketFirst = false;
			decision.error            = adoptError;
			return decision;
		}

		decision.outcome = ReloadRecoverySocketOutcome::Reattached;
		return decision;
	}

	if (worldState.connectedAtReload)
	{
		decision.outcome          = ReloadRecoverySocketOutcome::ReconnectQueued;
		decision.closeSocketFirst = false;
	}

	return decision;
}

QString reconnectRecoveredRuntime(ReloadReconnectOps &runtime, const ReloadWorldState &worldState,
                                  const bool closeSocketFirst)
{
	if (closeSocketFirst)
		runtime.closeSocketForReloadReconnect();
	runtime.setIncomingSocketDataPaused(false);

	QString host = worldState.host.trimmed();
	quint16 port = worldState.port;
	if (host.isEmpty())
		host = runtime.worldAttribute(QStringLiteral("site")).trimmed();
	if (port == 0)
		port = runtime.worldAttribute(QStringLiteral("port")).toUShort();
	if (host.isEmpty() || port == 0)
	{
		return QStringLiteral("Reload reconnect skipped due to missing host/port for world %1")
		    .arg(worldState.displayName);
	}

	runtime.setWorldAttribute(QStringLiteral("site"), host);
	runtime.setWorldAttribute(QStringLiteral("port"), QString::number(port));
	if (!runtime.connectToWorld(host, port))
	{
		return QStringLiteral("Reload reconnect failed to start for world %1 (%2:%3)")
		    .arg(worldState.displayName, host, QString::number(port));
	}
	return {};
}

void applyReloadPostReattachActions(ReloadPostReattachOps &runtime, const ReloadWorldState &worldState)
{
	const bool mccpProbeOnReattach = worldState.mccpWasActive && !worldState.mccpDisableSucceeded;
	runtime.configureReloadMccpReattachProbe(mccpProbeOnReattach);
	if (worldState.mccpWasActive && !mccpProbeOnReattach)
		runtime.requestMccpResumeAfterReloadReattach();
	runtime.sendReloadReattachLookProbe();
}

bool isLikelyResidualMccpPayload(const QByteArray &data)
{
	if (data.isEmpty())
		return true;
	qsizetype textLikeCount = 0;
	for (const char rawByte : data)
	{
		const auto byte = static_cast<unsigned char>(rawByte);
		if ((byte >= 0x20 && byte <= 0x7E) || byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n')
			++textLikeCount;
	}
	return (textLikeCount * 100) < (data.size() * 50);
}

bool shouldReconnectOnReloadMccpProbeTimeout(const QByteArray &bufferedProbePayload)
{
	return bufferedProbePayload.isEmpty() || isLikelyResidualMccpPayload(bufferedProbePayload);
}

ReloadMccpProbeTimeoutAction resolveReloadMccpProbeTimeoutAction(const QByteArray &bufferedProbePayload,
                                                                 const int         pass)
{
	if (bufferedProbePayload.isEmpty() && pass == 0)
		return ReloadMccpProbeTimeoutAction::WaitSecondPass;
	return shouldReconnectOnReloadMccpProbeTimeout(bufferedProbePayload)
	           ? ReloadMccpProbeTimeoutAction::Reconnect
	           : ReloadMccpProbeTimeoutAction::KeepReattach;
}

QByteArray extractReloadMccpProbeDecisionPayload(const QByteArray &probeBuffer, const bool lookProbeSent,
                                                 const qsizetype decisionOffset)
{
	if (!lookProbeSent)
		return {};
	if (decisionOffset >= probeBuffer.size())
		return {};
	return probeBuffer.mid(decisionOffset);
}

ReloadMccpProbePayloads makeReloadMccpProbePayloads(const QByteArray &probeBuffer, const bool lookProbeSent,
                                                    const qsizetype decisionOffset)
{
	return {probeBuffer, extractReloadMccpProbeDecisionPayload(probeBuffer, lookProbeSent, decisionOffset)};
}

bool ingestReloadMccpProbeChunk(const ReloadMccpProbeIngressInput &input, qint64 *bytesIn,
                                int *inputPacketCount, QByteArray *probeBuffer)
{
	if (!bytesIn || !inputPacketCount || !probeBuffer)
		return false;
	if (input.simulatedInput || !input.probePending)
		return false;

	*bytesIn += input.rawSize;
	++(*inputPacketCount);
	if (!input.processedPayload.isEmpty())
		probeBuffer->append(input.processedPayload);
	return true;
}

QByteArray takeDeferredReloadMccpProbePayload(QByteArray *probeBuffer)
{
	if (!probeBuffer || probeBuffer->isEmpty())
		return {};

	QByteArray payload;
	payload.swap(*probeBuffer);
	return payload;
}
