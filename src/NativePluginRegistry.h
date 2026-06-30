/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: NativePluginRegistry.h
 * Role: Native compatibility plugin registry and shim dispatch API.
 */

#ifndef QMUD_NATIVEPLUGINREGISTRY_H
#define QMUD_NATIVEPLUGINREGISTRY_H

#include <QDateTime>
#include <QList>
#include <QString>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

class WorldRuntime;

namespace QMudNativePluginRegistry
{
	/**
	 * @brief Metadata exposed by a native compatibility shim through legacy plugin APIs.
	 */
	struct NativePluginMetadata
	{
			QString   id;
			QString   name;
			QString   author;
			QString   purpose;
			QString   description;
			QString   language;
			QString   source;
			QString   directory;
			double    version{0.0};
			double    requiredVersion{0.0};
			int       sequence{5000};
			QDateTime dateWritten;
			QDateTime dateModified;
	};

	/**
	 * @brief Return payload for native CallPlugin dispatch.
	 */
	struct NativeCallResult
	{
			int               errorCode{0};
			QString           errorText;
			QVector<QVariant> returnValues;
	};

	/**
	 * @brief Runtime-level LuaAudio playback settings shared by direct `audio.*` and native CallPlugin paths.
	 */
	struct LuaAudioRuntimeBufferState
	{
			double                            volume{100.0};
			double                            pan{0.0};
			double                            pitch{0.0};
			bool                              loop{false};
			const void                       *ownerKey{nullptr};
			std::shared_ptr<std::atomic_bool> pendingCancel;
			quint64                           generation{0};
	};

	/**
	 * @brief Runtime-level LuaAudio master settings shared by direct `audio.*` and native CallPlugin paths.
	 */
	struct LuaAudioRuntimeMasterState
	{
			double volume{100.0};
			double pan{0.0};
			double pitch{0.0};
	};

	/**
	 * @brief Returns the exact UI marker used for native shim shadow rows.
	 * @return Native compatibility marker text.
	 */
	[[nodiscard]] QString nativeShimMarkerText();
	/**
	 * @brief Returns the MushReader legacy plugin id implemented natively.
	 * @return Lower-case plugin id.
	 */
	[[nodiscard]] QString mushReaderPluginId();
	/**
	 * @brief Returns the LuaAudio legacy plugin id implemented natively.
	 * @return Lower-case plugin id.
	 */
	[[nodiscard]] QString luaAudioPluginId();
	/**
	 * @brief Checks whether a plugin id is implemented as a native shim.
	 * @param pluginId Plugin id to check.
	 * @return `true` when the id maps to a native shim.
	 */
	[[nodiscard]] bool    isShimId(const QString &pluginId);
	/**
	 * @brief Checks whether a plugin id is centrally denied without a shim.
	 * @param pluginId Plugin id to check.
	 * @return `true` when loading should be ignored.
	 */
	[[nodiscard]] bool    isBlacklistedId(const QString &pluginId);
	/**
	 * @brief Checks whether a plugin id is protected by either shim or blacklist policy.
	 * @param pluginId Plugin id to check.
	 * @return `true` when the normal XML plugin loader must not execute it.
	 */
	[[nodiscard]] bool    isProtectedId(const QString &pluginId);
	/**
	 * @brief Resolves a plugin id or native shim name to a canonical shim id.
	 * @param pluginIdOrName Plugin id or display name.
	 * @return Canonical shim id, or empty when not a shim.
	 */
	[[nodiscard]] QString resolveShimIdOrName(const QString &pluginIdOrName);
	/**
	 * @brief Returns native shim metadata.
	 * @param pluginId Shim id.
	 * @param metadata Output metadata.
	 * @return `true` when metadata exists.
	 */
	[[nodiscard]] bool    metadataForShim(const QString &pluginId, NativePluginMetadata &metadata);
	/**
	 * @brief Returns native shim metadata by virtual `qmud:native` include name.
	 * @param nativeName Name after the virtual native include prefix.
	 * @param metadata Output metadata.
	 * @return `true` when metadata exists.
	 */
	[[nodiscard]] bool    metadataForNativeName(const QString &nativeName, NativePluginMetadata &metadata);
	/**
	 * @brief Normalizes a virtual native plugin source token without resolving filesystem paths.
	 * @param source Source/include token to normalize.
	 * @return Canonical `qmud:native/<Name>` source, or empty when the token is not virtual-native syntax.
	 */
	[[nodiscard]] QString normalizeNativeSource(const QString &source);
	/**
	 * @brief Returns native shim metadata by virtual source token.
	 * @param source Source/include token such as `qmud:native/MushReader`.
	 * @param metadata Output metadata.
	 * @return `true` when the source names a registered native shim.
	 */
	[[nodiscard]] bool    metadataForNativeSource(const QString &source, NativePluginMetadata &metadata);
	/**
	 * @brief Lists native routines exposed by a shim.
	 * @param pluginId Shim id.
	 * @return Routine names in stable order.
	 */
	[[nodiscard]] QStringList                supportedRoutines(const QString &pluginId);
	/**
	 * @brief Returns runtime-level LuaAudio master volume, pan, and pitch.
	 * @param runtime Owning runtime.
	 * @return Master settings, or defaults when no runtime state exists.
	 */
	[[nodiscard]] LuaAudioRuntimeMasterState luaAudioRuntimeMasterState(const WorldRuntime *runtime);
	/**
	 * @brief Updates LuaAudio master volume for a runtime.
	 * @param runtime Owning runtime.
	 * @param volume Legacy LuaAudio volume value.
	 */
	void              luaAudioSetRuntimeMasterVolume(const WorldRuntime *runtime, double volume);
	/**
	 * @brief Updates LuaAudio master pan for a runtime.
	 * @param runtime Owning runtime.
	 * @param pan Legacy LuaAudio pan value.
	 */
	void              luaAudioSetRuntimeMasterPan(const WorldRuntime *runtime, double pan);
	/**
	 * @brief Updates LuaAudio master pitch for a runtime.
	 * @param runtime Owning runtime.
	 * @param pitch Legacy LuaAudio pitch value.
	 */
	void              luaAudioSetRuntimeMasterPitch(const WorldRuntime *runtime, double pitch);
	/**
	 * @brief Reserves the first available LuaAudio buffer using shared runtime ownership.
	 * @param runtime Owning runtime.
	 * @param bufferReusableResolver Callback returning whether a buffer may be reused.
	 * @return Reserved buffer id, or zero when none is available.
	 */
	[[nodiscard]] int luaAudioReserveRuntimeBuffer(const WorldRuntime             *runtime,
	                                               const std::function<bool(int)> &bufferReusableResolver);
	/**
	 * @brief Marks a runtime LuaAudio buffer as owned and stores current playback settings.
	 * @param runtime Owning runtime.
	 * @param buffer Buffer id.
	 * @param state Playback settings.
	 */
	void              luaAudioMarkRuntimeBuffer(const WorldRuntime *runtime, int buffer,
	                                            const LuaAudioRuntimeBufferState &state);
	/**
	 * @brief Tracks a cancellable delayed operation against a LuaAudio-owned buffer.
	 * @param runtime Owning runtime.
	 * @param buffer Buffer id.
	 * @param cancelToken Shared cancellation flag checked by delayed work.
	 * @param generation Optional output for the current buffer ownership generation.
	 * @return `true` when the operation was attached to an owned buffer.
	 */
	[[nodiscard]] bool
	     luaAudioTrackRuntimeBufferPendingOperation(const WorldRuntime *runtime, int buffer,
	                                                const std::shared_ptr<std::atomic_bool> &cancelToken,
	                                                quint64                                 *generation = nullptr);
	/**
	 * @brief Removes a completed delayed operation token from a LuaAudio-owned buffer.
	 * @param runtime Owning runtime.
	 * @param buffer Buffer id.
	 * @param cancelToken Shared cancellation flag for the completed work.
	 */
	void luaAudioForgetRuntimeBufferPendingOperation(const WorldRuntime *runtime, int buffer,
	                                                 const std::shared_ptr<std::atomic_bool> &cancelToken);
	/**
	 * @brief Atomically consumes a delayed play token for a still-current LuaAudio buffer generation.
	 * @param runtime Owning runtime.
	 * @param buffer Buffer id.
	 * @param generation Expected buffer ownership generation.
	 * @param cancelToken Shared cancellation flag for the delayed play.
	 * @param state Output current buffer state.
	 * @return `true` when the delayed play may proceed.
	 */
	[[nodiscard]] bool
	luaAudioConsumeRuntimeBufferPendingPlay(const WorldRuntime *runtime, int buffer, quint64 generation,
	                                        const std::shared_ptr<std::atomic_bool> &cancelToken,
	                                        LuaAudioRuntimeBufferState              &state);
	/**
	 * @brief Atomically consumes a delayed operation token for a still-current LuaAudio buffer generation.
	 * @param runtime Owning runtime.
	 * @param buffer Buffer id.
	 * @param generation Expected buffer ownership generation.
	 * @param cancelToken Shared cancellation flag for the delayed operation.
	 * @param state Output current buffer state.
	 * @return `true` when the delayed operation may proceed.
	 */
	[[nodiscard]] bool
	luaAudioConsumeRuntimeBufferPendingOperation(const WorldRuntime *runtime, int buffer, quint64 generation,
	                                             const std::shared_ptr<std::atomic_bool> &cancelToken,
	                                             LuaAudioRuntimeBufferState              &state);
	/**
	 * @brief Returns stored runtime LuaAudio buffer settings.
	 * @param runtime Owning runtime.
	 * @param buffer Buffer id.
	 * @param state Output settings.
	 * @return `true` when the buffer is currently owned by LuaAudio.
	 */
	[[nodiscard]] bool       luaAudioRuntimeBufferState(const WorldRuntime *runtime, int buffer,
	                                                    LuaAudioRuntimeBufferState &state);
	/**
	 * @brief Returns runtime LuaAudio-owned buffers, optionally filtered by owner.
	 * @param runtime Owning runtime.
	 * @param ownerKey Optional owner token; null returns all buffers.
	 * @return Owned buffer ids.
	 */
	[[nodiscard]] QList<int> luaAudioRuntimeOwnedBuffers(const WorldRuntime *runtime,
	                                                     const void         *ownerKey = nullptr);
	/**
	 * @brief Releases a runtime LuaAudio-owned buffer and cancels pending delayed work.
	 * @param runtime Owning runtime.
	 * @param buffer Buffer id.
	 */
	void                     luaAudioReleaseRuntimeBuffer(const WorldRuntime *runtime, int buffer);
	/**
	 * @brief Releases a runtime LuaAudio buffer only if its ownership generation still matches.
	 * @param runtime Owning runtime.
	 * @param buffer Buffer id.
	 * @param generation Expected ownership generation.
	 * @return `true` when the buffer was released.
	 */
	bool                     luaAudioReleaseRuntimeBufferIfGeneration(const WorldRuntime *runtime, int buffer,
	                                                                  quint64 generation);
	/**
	 * @brief Releases runtime LuaAudio-owned buffers and cancels pending delayed work.
	 * @param runtime Owning runtime.
	 * @param buffers Buffer ids.
	 */
	void              luaAudioReleaseRuntimeBuffers(const WorldRuntime *runtime, const QList<int> &buffers);
	/**
	 * @brief Releases all runtime LuaAudio buffers owned by a specific caller token.
	 * @param runtime Owning runtime.
	 * @param ownerKey Owner token to release.
	 */
	void              luaAudioReleaseRuntimeOwner(const WorldRuntime *runtime, const void *ownerKey);
	/**
	 * @brief Stops and releases all runtime LuaAudio buffers.
	 * @param runtime Owning runtime.
	 */
	void              luaAudioStopRuntime(const WorldRuntime *runtime);
	/**
	 * @brief Stops and releases all runtime LuaAudio buffers owned by a specific caller token.
	 * @param runtime Owning runtime.
	 * @param ownerKey Owner token to stop and release.
	 */
	void              luaAudioStopRuntimeOwner(const WorldRuntime *runtime, const void *ownerKey);
	/**
	 * @brief Stops LuaAudio playback and resets runtime-level LuaAudio state.
	 * @param runtime Owning runtime.
	 */
	void              luaAudioResetRuntime(const WorldRuntime *runtime);
	/**
	 * @brief Returns a legacy PluginSupports code for a native shim routine.
	 * @param pluginId Shim id.
	 * @param routine Routine name.
	 * @return Legacy scripting error/status code.
	 */
	[[nodiscard]] int pluginSupports(const QString &pluginId, const QString &routine);
	/**
	 * @brief Returns a legacy GetPluginInfo value for a native shim.
	 * @param pluginId Shim id.
	 * @param infoType Legacy info selector.
	 * @param visibleIndex One-based list index when a shadow row exists, otherwise zero.
	 * @param enabled Runtime-visible enabled state for info selector 17.
	 * @return Requested value, or invalid QVariant when unsupported.
	 */
	[[nodiscard]] QVariant pluginInfo(const QString &pluginId, int infoType, int visibleIndex = 0,
	                                  bool enabled = false);
	/**
	 * @brief Calls a native shim routine from legacy CallPlugin APIs.
	 * @param runtime Owning runtime.
	 * @param pluginId Shim id.
	 * @param routine Routine name.
	 * @param arguments Routine arguments converted to variants.
	 * @return Legacy status plus optional return values.
	 */
	NativeCallResult callRoutine(const WorldRuntime *runtime, const QString &pluginId, const QString &routine,
	                             const QVector<QVariant> &arguments);
	/**
	 * @brief Handles native command aliases that would normally be installed by MushReader XML.
	 * @param runtime Owning runtime.
	 * @param command Entered command line.
	 * @return `true` when the command was consumed.
	 */
	bool             handleMushReaderCommand(WorldRuntime *runtime, const QString &command);
	/**
	 * @brief Handles native command aliases that would normally be installed by LuaAudio XML.
	 * @param runtime Owning runtime.
	 * @param command Entered command line.
	 * @return `true` when the command was consumed.
	 */
	bool             handleLuaAudioCommand(WorldRuntime *runtime, const QString &command);
	/**
	 * @brief Handles native LuaAudio `OnPluginPlaySound` interception.
	 * @param runtime Owning runtime.
	 * @param sound Sound path or LuaAudio control string.
	 * @return `true` when LuaAudio consumed the sound event.
	 */
	bool             handleLuaAudioPlaySound(const WorldRuntime *runtime, const QString &sound);
	/**
	 * @brief Handles native screen-draw speech for MushReader parity.
	 * @param runtime Owning runtime.
	 * @param type Legacy screen-draw type.
	 * @param log Legacy log flag.
	 * @param text Text drawn to the output surface.
	 */
	void handleMushReaderScreenDraw(const WorldRuntime *runtime, int type, int log, const QString &text);
	/**
	 * @brief Handles native tab-completion speech for MushReader parity.
	 * @param runtime Owning runtime.
	 * @param text Completed command text.
	 */
	void handleMushReaderTabComplete(const WorldRuntime *runtime, const QString &text);
	/**
	 * @brief Marks whether the native MushReader shim plugin is enabled.
	 * @param runtime Owning runtime.
	 * @param enable Enable the native MushReader shim when `true`.
	 */
	void setMushReaderPluginEnabled(const WorldRuntime *runtime, bool enable);
	/**
	 * @brief Returns whether the native MushReader shim plugin is enabled.
	 * @param runtime Owning runtime.
	 * @return `true` when the native MushReader shim plugin is enabled for the runtime.
	 */
	[[nodiscard]] bool isMushReaderPluginEnabled(const WorldRuntime *runtime);
	/**
	 * @brief Enables or disables passive screen/tab speech used when MushReader is not enabled.
	 * @param runtime Owning runtime.
	 * @param enable Enable passive screen/tab speech when `true`.
	 */
	void               setMushReaderPassiveSpeechEnabled(const WorldRuntime *runtime, bool enable);
	/**
	 * @brief Returns whether passive screen/tab speech is enabled outside the MushReader shim.
	 * @param runtime Owning runtime.
	 * @return `true` when passive speech is enabled for the runtime.
	 */
	[[nodiscard]] bool isMushReaderPassiveSpeechEnabled(const WorldRuntime *runtime);
	/**
	 * @brief Installs runtime-level native behavior that must exist even without a shadow row.
	 * @param runtime Owning runtime.
	 */
	void               ensureMushReaderRuntimeSetup(WorldRuntime *runtime);
	/**
	 * @brief Releases per-runtime native shim state.
	 * @param runtime Runtime being destroyed.
	 */
	void               discardRuntimeState(const WorldRuntime *runtime);

	/**
	 * @brief Speech event captured by native registry test hooks.
	 */
	struct TestSpeechEvent
	{
			QString text;
			bool    interrupt{false};
			bool    stop{false};
	};

	/**
	 * @brief Installs a test-only speech sink that bypasses platform reader backends.
	 * @param sink Event receiver; empty sink restores normal backend routing.
	 */
	void                      setTestSpeechSink(std::function<void(const TestSpeechEvent &)> sink);
	/**
	 * @brief Returns the number of initialized MushReader speech backends for tests.
	 * @param runtime Runtime to inspect.
	 * @return Initialized backend count, or zero when no speech state exists.
	 */
	[[nodiscard]] std::size_t mushReaderTestBackendCount(const WorldRuntime *runtime);
} // namespace QMudNativePluginRegistry

#endif // QMUD_NATIVEPLUGINREGISTRY_H
