/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ReloadUtils.h
 * Role: Consolidated reload/copyover state, policy, recovery, and MCCP probe helper APIs.
 */

#ifndef QMUD_RELOADUTILS_H
#define QMUD_RELOADUTILS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>
#include <QtGlobal>

/**
 * @brief Policy describing how one world socket should be handled during reload startup.
 */
enum class ReloadSocketPolicy
{
	Reattach,
	ParkReconnect
};

/**
 * @brief Serialized handoff state for one world entry during reload.
 */
struct ReloadWorldState
{
		int                sequence{0};
		QString            worldId;
		QString            displayName;
		QString            worldFilePath;
		QString            host;
		quint16            port{0};
		int                socketDescriptor{-1};
		ReloadSocketPolicy policy{ReloadSocketPolicy::Reattach};
		bool               connectedAtReload{false};
		bool               mccpWasActive{false};
		bool               mccpDisableAttempted{false};
		bool               mccpDisableSucceeded{false};
		bool               utf8Enabled{false};
		QString            notes;
};

/**
 * @brief Full reload manifest persisted before `exec` and consumed on startup.
 */
struct ReloadStateSnapshot
{
		int                     schemaVersion{1};
		QDateTime               createdAtUtc;
		QString                 reloadToken;
		QString                 targetExecutable;
		int                     activeWorldSequence{0};
		QStringList             arguments;
		QList<ReloadWorldState> worlds;
};

/**
 * @brief Validation expectations used for startup reload manifest checks.
 */
struct ReloadStartupValidationInput
{
		QString expectedToken;
		QString expectedTargetExecutable;
};

/**
 * @brief Input snapshot used to decide reload policy for one world.
 */
struct ReloadWorldPolicyInput
{
		bool connected{false};
		bool connecting{false};
		int  socketDescriptor{-1};
		bool tlsEnabled{false};
		bool mccpWasActive{false};
		bool mccpDisableSucceeded{false};
};

/**
 * @brief Computed reload policy decision for one world.
 */
struct ReloadWorldPolicyDecision
{
		bool               connectedAtReload{false};
		bool               shouldAttemptDescriptorInheritance{false};
		ReloadSocketPolicy policy{ReloadSocketPolicy::Reattach};
};

/**
 * @brief Per-world startup recovery socket handling outcome.
 */
enum class ReloadRecoverySocketOutcome
{
	Noop,
	Reattached,
	ReconnectQueued
};

/**
 * @brief Decision result from per-world startup recovery socket handling.
 */
struct ReloadRecoverySocketDecision
{
		ReloadRecoverySocketOutcome outcome{ReloadRecoverySocketOutcome::Noop};
		bool                        closeSocketFirst{false};
		QString                     error;
};

/**
 * @brief Timeout handling action for reload MCCP probe sequencing.
 */
enum class ReloadMccpProbeTimeoutAction
{
	WaitSecondPass,
	KeepReattach,
	Reconnect
};

/**
 * @brief Pair of payloads used by reload MCCP probe finalization.
 */
struct ReloadMccpProbePayloads
{
		QByteArray replayPayload;
		QByteArray decisionPayload;
};

/**
 * @brief Inputs used to evaluate one incoming chunk during probe quarantine.
 */
struct ReloadMccpProbeIngressInput
{
		bool       simulatedInput{false};
		bool       probePending{false};
		qsizetype  rawSize{0};
		QByteArray processedPayload;
};

/**
 * @brief Runtime operations required by startup descriptor recovery logic.
 */
class ReloadSocketRecoveryOps
{
	public:
		virtual ~ReloadSocketRecoveryOps() = default;
		[[nodiscard]] virtual bool adoptConnectedSocketDescriptor(int descriptor, QString *errorMessage) = 0;
		virtual void               markReloadReattachConnectActionsSuppressed()                          = 0;
		[[nodiscard]] virtual bool consumeReloadReattachConnectActionsSuppressed()                       = 0;
};

/**
 * @brief Runtime operations required by reconnect fallback startup flow.
 */
class ReloadReconnectOps
{
	public:
		virtual ~ReloadReconnectOps()                                                              = default;
		virtual void                  closeSocketForReloadReconnect()                              = 0;
		virtual void                  setIncomingSocketDataPaused(bool paused)                     = 0;
		[[nodiscard]] virtual QString worldAttribute(const QString &name) const                    = 0;
		virtual void                  setWorldAttribute(const QString &name, const QString &value) = 0;
		[[nodiscard]] virtual bool    connectToWorld(const QString &host, quint16 port)            = 0;
};

/**
 * @brief Runtime operations required by post-reattach startup actions.
 */
class ReloadPostReattachOps
{
	public:
		virtual ~ReloadPostReattachOps()                            = default;
		virtual void configureReloadMccpReattachProbe(bool enabled) = 0;
		virtual void requestMccpResumeAfterReloadReattach()         = 0;
		virtual void sendReloadReattachLookProbe()                  = 0;
};

/**
 * @brief Returns default absolute reload-state file path under startup/data directory.
 * @param workingDirectory Startup/data directory path.
 * @return Absolute reload-state file path.
 */
[[nodiscard]] QString     reloadStateDefaultPath(const QString &workingDirectory);
/**
 * @brief Parses reload startup arguments from command line.
 * @param arguments Full process argument list.
 * @param statePath Output reload-state file path.
 * @param token Output reload token.
 * @return `true` when both required reload arguments were found.
 */
[[nodiscard]] bool        parseReloadStartupArguments(const QStringList &arguments, QString *statePath,
                                                      QString *token);
/**
 * @brief Returns startup arguments with reload-only flags removed.
 * @param arguments Full process argument list.
 * @return Argument list without `--reload-state=` / `--reload-token=` entries.
 */
[[nodiscard]] QStringList filterReloadStartupArguments(const QStringList &arguments);
/**
 * @brief Serializes one reload snapshot to disk atomically.
 * @param filePath Target JSON file path.
 * @param snapshot Snapshot payload to write.
 * @param errorMessage Optional output error text.
 * @return `true` on successful write.
 */
[[nodiscard]] bool writeReloadStateSnapshot(const QString &filePath, const ReloadStateSnapshot &snapshot,
                                            QString *errorMessage = nullptr);
/**
 * @brief Reads one reload snapshot from disk.
 * @param filePath Source JSON file path.
 * @param snapshot Output parsed snapshot.
 * @param errorMessage Optional output error text.
 * @return `true` when parsing/validation succeeds.
 */
[[nodiscard]] bool readReloadStateSnapshot(const QString &filePath, ReloadStateSnapshot *snapshot,
                                           QString *errorMessage = nullptr);
/**
 * @brief Validates startup snapshot metadata against expected token/executable values.
 * @param snapshot Parsed snapshot payload.
 * @param input Expected startup validation values.
 * @param errorMessage Optional output error text.
 * @return `true` when snapshot matches expected startup state.
 */
[[nodiscard]] bool validateReloadStartupSnapshot(const ReloadStateSnapshot          &snapshot,
                                                 const ReloadStartupValidationInput &input,
                                                 QString                            *errorMessage = nullptr);
/**
 * @brief Loads, validates, and consumes a startup reload-state snapshot.
 *
 * On parse/validation failure this helper still attempts to remove the manifest,
 * matching startup fallback cleanup behavior.
 *
 * @param filePath Source JSON file path.
 * @param input Expected startup validation values.
 * @param snapshot Output parsed snapshot on success.
 * @param errorMessage Optional output error text.
 * @param cleanupWarning Optional output warning when manifest cleanup fails.
 * @return `true` when load+validation succeeds; `false` otherwise.
 */
[[nodiscard]] bool loadValidatedAndConsumeReloadStateSnapshot(const QString                      &filePath,
                                                              const ReloadStartupValidationInput &input,
                                                              ReloadStateSnapshot                *snapshot,
                                                              QString *errorMessage   = nullptr,
                                                              QString *cleanupWarning = nullptr);
/**
 * @brief Removes reload-state file if it exists.
 * @param filePath Target JSON file path.
 * @param errorMessage Optional output error text.
 * @return `true` when file does not exist or deletion succeeds.
 */
[[nodiscard]] bool removeReloadStateFile(const QString &filePath, QString *errorMessage = nullptr);
/**
 * @brief Determines whether a reload-state file should be treated as stale.
 * @param filePath Reload-state file path.
 * @param maxAgeSeconds Maximum accepted file age in seconds.
 * @param nowUtc Reference UTC timestamp used for age calculation.
 * @return `true` when file exists and exceeds stale-age threshold.
 */
[[nodiscard]] bool isReloadStateFileStale(const QString &filePath, int maxAgeSeconds,
                                          const QDateTime &nowUtc = QDateTime::currentDateTimeUtc());
/**
 * @brief Returns whether reload planning should attempt MCCP shutdown for this world.
 * @param connected `true` when socket is currently connected.
 * @param socketDescriptor Native socket descriptor, or `-1`.
 * @param tlsEnabled `true` when TLS encryption is enabled for the world.
 * @param mccpWasActive `true` when MCCP was active at reload time.
 * @return `true` when MCCP disable attempt should be issued.
 */
[[nodiscard]] bool shouldAttemptReloadMccpDisable(bool connected, int socketDescriptor, bool tlsEnabled,
                                                  bool mccpWasActive);
/**
 * @brief Computes per-world reload socket policy.
 * @param input Per-world connection and MCCP state.
 * @return Decision containing connected-at-reload state, inheritance hint, and selected policy.
 */
[[nodiscard]] ReloadWorldPolicyDecision    computeReloadWorldPolicy(const ReloadWorldPolicyInput &input);
/**
 * @brief Applies startup recovery socket handling for one world.
 *
 * Reattaches the inherited descriptor when possible, otherwise queues reconnect
 * fallback.
 *
 * @param runtime Runtime operations adapter.
 * @param worldState Serialized world recovery state.
 * @return Recovery socket decision describing next action.
 */
[[nodiscard]] ReloadRecoverySocketDecision applyReloadSocketRecovery(ReloadSocketRecoveryOps &runtime,
                                                                     const ReloadWorldState  &worldState);
/**
 * @brief Executes reconnect for one runtime after startup recovery.
 *
 * @param runtime Runtime operations adapter.
 * @param worldState Serialized world recovery state.
 * @param closeSocketFirst Close inherited parked socket before reconnect.
 * @return Empty string on success; warning text when reconnect cannot be started.
 */
[[nodiscard]] QString                      reconnectRecoveredRuntime(ReloadReconnectOps     &runtime,
                                                                     const ReloadWorldState &worldState, bool closeSocketFirst);
/**
 * @brief Applies post-reattach startup actions for one recovered world.
 *
 * Behavior:
 * - Arms MCCP residual-stream probe only when MCCP was active and disable timed out.
 * - Otherwise resumes MCCP negotiation when MCCP was active and disable succeeded.
 * - Always sends `look` probe command after reattach.
 *
 * @param runtime Runtime operations adapter.
 * @param worldState Serialized world recovery state.
 */
void applyReloadPostReattachActions(ReloadPostReattachOps &runtime, const ReloadWorldState &worldState);
/**
 * @brief Returns whether a post-reattach probe payload looks garbled (likely residual MCCP bytes).
 *
 * After reload reattach we send `look` and classify the received payload using a
 * single text-density rule:
 * - if at least 50% of bytes are printable ASCII or whitespace (space/tab/CR/LF),
 *   treat as valid text response (keep reattach)
 * - otherwise treat as binary/garbled (reconnect)
 *
 * @param data Telnet-processed probe decision payload (post-`look` window).
 * @return `true` when payload likely represents residual MCCP compressed bytes.
 */
[[nodiscard]] bool isLikelyResidualMccpPayload(const QByteArray &data);
/**
 * @brief Returns whether timeout completion should force reconnect.
 *
 * Timeout with no response is treated as failed probe and reconnects by policy.
 *
 * @param bufferedProbePayload Probe bytes collected before timeout.
 * @return `true` when timeout path should reconnect.
 */
[[nodiscard]] bool shouldReconnectOnReloadMccpProbeTimeout(const QByteArray &bufferedProbePayload);
/**
 * @brief Resolves probe timeout action for the current timeout pass.
 *
 * First timeout pass defers once when no post-look bytes are available.
 * Second pass (or any non-empty payload) resolves to keep/reconnect classification.
 *
 * @param bufferedProbePayload Probe bytes collected before timeout.
 * @param pass Timeout pass index (`0` first pass, `1` second pass).
 * @return Timeout action for this pass.
 */
[[nodiscard]] ReloadMccpProbeTimeoutAction
resolveReloadMccpProbeTimeoutAction(const QByteArray &bufferedProbePayload, int pass);
/**
 * @brief Extracts the payload window used for reload MCCP probe classification.
 *
 * Decision bytes must be taken only from data received after the `look` probe
 * command has been sent.
 *
 * @param probeBuffer Full buffered bytes collected during pending probe.
 * @param lookProbeSent `true` after probe `look` command has been sent.
 * @param decisionOffset Byte offset captured at `look` send time.
 * @return Decision payload slice; empty when decision window is not yet available.
 */
[[nodiscard]] QByteArray extractReloadMccpProbeDecisionPayload(const QByteArray &probeBuffer,
                                                               bool lookProbeSent, qsizetype decisionOffset);
/**
 * @brief Builds replay+decision payloads for reload MCCP probe finalization.
 *
 * Replay payload always contains all buffered bytes. Decision payload contains
 * only bytes collected after probe `look` send.
 *
 * @param probeBuffer Full buffered bytes collected during pending probe.
 * @param lookProbeSent `true` after probe `look` command has been sent.
 * @param decisionOffset Byte offset captured at `look` send time.
 * @return Payload pair for replay and classification.
 */
[[nodiscard]] ReloadMccpProbePayloads
makeReloadMccpProbePayloads(const QByteArray &probeBuffer, bool lookProbeSent, qsizetype decisionOffset);
/**
 * @brief Ingests one chunk into probe quarantine state when probe mode is active.
 *
 * When probe quarantine is active (`probePending` and not simulated input), this:
 * - increments byte/packet counters using raw socket payload size
 * - appends processed telnet payload to probe buffer
 *
 * @param input Ingress input snapshot.
 * @param bytesIn Total incoming bytes counter.
 * @param inputPacketCount Incoming packet counter.
 * @param probeBuffer Buffered processed probe payload.
 * @return `true` when chunk was consumed by probe quarantine.
 */
[[nodiscard]] bool       ingestReloadMccpProbeChunk(const ReloadMccpProbeIngressInput &input, qint64 *bytesIn,
                                                    int *inputPacketCount, QByteArray *probeBuffer);
/**
 * @brief Takes deferred probe payload for one-shot keep-path replay.
 *
 * @param probeBuffer Buffered processed probe payload.
 * @return Deferred payload; empty when buffer is null/empty.
 */
[[nodiscard]] QByteArray takeDeferredReloadMccpProbePayload(QByteArray *probeBuffer);

#endif // QMUD_RELOADUTILS_H
