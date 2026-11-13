/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: GmcpProtocol.h
 * Role: GMCP protocol state, immutable publication, and history.
 */
#ifndef QMUD_GMCPPROTOCOL_H
#define QMUD_GMCPPROTOCOL_H

#include "AtomicSharedPtrCompat.h"
#include "helpers/PersistentStringMap.h"
// ReSharper disable once CppUnusedIncludeDirective
#include <QDateTime>
#include <QJsonValue>
#include <QMap>
#include <QObject>
#include <QSet>
#include <functional>
#include <memory>

namespace QMudGmcp
{
	/**
	 * @brief Persistent world preferences for automatic Core messages.
	 */
	struct CoreOptions
	{
			bool pingEnabled{true};
			int  pingIntervalMinutes{5};
			bool sendRtt{true};
			bool keepAliveEnabled{true};
			int  keepAliveIntervalMinutes{15};
	};
	/**
	 * @brief Policy for applying a received JSON payload.
	 */
	enum class MergeMode
	{
		Replace,
		Shallow,
		Recursive
	};
	/**
	 * @brief One immutable application value and its original wire message.
	 */
	struct Message
	{
			QString    name;
			QByteArray raw;
			QJsonValue payload{QJsonValue::Undefined};
			QJsonValue state{QJsonValue::Undefined};
			quint64    sequence{0};
			quint64    session{0};
	};
	/**
	 * @brief Published package navigation metadata; payloads remain in the message map.
	 */
	struct PackageBranch
	{
			QString     label;
			QStringList children;
	};
	/**
	 * @brief Structurally shared package hierarchy maintained when new message names arrive.
	 */
	struct PackageIndex
	{
			QStringList                                   roots;
			LuaCallbackPersistentStringMap<PackageBranch> branches;
			/**
			 * @brief Adds a message's namespace path without copying payloads.
			 * @param name Original message name.
			 */
			void                                          add(const QString &name);
	};
	/**
	 * @brief Immutable native-session view shared with Lua and browser readers.
	 */
	struct Snapshot
	{
			bool                                    native{false};
			bool                                    negotiated{false};
			bool                                    closed{false};
			bool                                    restoring{false};
			bool                                    connectionSelected{false};
			quint64                                 generation{0};
			quint64                                 session{0};
			quint64                                 revision{0};
			qint64                                  roundTripMs{-1};
			LuaCallbackPersistentStringMap<Message> messages;
			PackageIndex                            packages;
			QSet<QString>                           namespaces;
			QMap<QString, MergeMode>                serverModes;
			QMap<QString, MergeMode>                localModes;
			QSet<QString>                           forcedModes;
			/**
			 * @brief Checks whether a live or restored connection has selected native handling.
			 * @return Whether native GMCP APIs are available independently of preferences.
			 */
			[[nodiscard]] bool                      nativeAvailable() const noexcept
			{
				return connectionSelected && native && !closed;
			}
			/**
			 * @brief Resolves the nearest inherited policy within each authority tier for a message.
			 * @param name Message name.
			 * @return Effective merge policy for its next update.
			 */
			[[nodiscard]] MergeMode effectiveMode(const QString &name) const;
	};
	/**
	 * @brief Lifetime-independent publication endpoint; its writer is the transport thread.
	 */
	struct Publication
	{
			QMudAtomicSharedPtr<const Snapshot> current{std::make_shared<const Snapshot>()};
	};
	/**
	 * @brief One diagnostic/history record with a structurally shared version of the package values.
	 */
	struct Record
	{
			QDateTime                               time;
			quint64                                 generation{0};
			quint64                                 sequence{0};
			bool                                    outgoing{false};
			QByteArray                              raw;
			QString                                 diagnostic;
			LuaCallbackPersistentStringMap<Message> messages;
			PackageIndex                            packages;
			quint64                                 revision{0};
	};
	/**
	 * @brief Indexed arrival-order history whose copies retain immutable versions without copying records.
	 *
	 * Mutations belong to the runtime thread. Shared pages and their index paths are copied only when
	 * changed; a paused version never observes later edits. Released versions are retired on the global
	 * thread pool without retaining any runtime, widget, or Lua object.
	 */
	class History
	{
		public:
			/**
			 * @brief Contiguous arrival-order ranks selected by periodic history maintenance.
			 */
			struct Range
			{
					qsizetype first;
					qsizetype count;
			};
			/**
			 * @brief Constructs empty history.
			 */
			History() = default;
			/**
			 * @brief Captures another history's current version in constant time.
			 * @param other History whose existing storage is retained.
			 */
			History(const History &other) = default;
			/**
			 * @brief Takes another history's storage.
			 * @param other History whose storage is transferred.
			 */
			History(History &&other) noexcept = default;
			/**
			 * @brief Retires this version without releasing its records on the caller's thread.
			 */
			~History();
			/**
			 * @brief Captures another version and retires the previously retained version.
			 * @param other History whose existing storage is retained.
			 * @return This history.
			 */
			History                    &operator=(const History &other);
			/**
			 * @brief Takes another version and retires the previously retained version.
			 * @param other History whose storage is transferred.
			 * @return This history.
			 */
			History                    &operator=(History &&other) noexcept;
			/**
			 * @brief Counts retained records without traversing storage.
			 * @return Record count.
			 */
			[[nodiscard]] qsizetype     size() const noexcept;
			/**
			 * @brief Checks whether this version contains no records.
			 * @return Whether history is empty.
			 */
			[[nodiscard]] bool          isEmpty() const noexcept;
			/**
			 * @brief Reads an existing record by its arrival-order rank.
			 * @param index Zero-based rank, bounded by size().
			 * @return Existing record, valid until this history is mutated or released.
			 */
			[[nodiscard]] const Record &at(qsizetype index) const;
			/**
			 * @brief Reads the oldest retained record.
			 * @return Existing first record; history must be nonempty.
			 */
			[[nodiscard]] const Record &first() const;
			/**
			 * @brief Reads the latest retained record.
			 * @return Existing last record; history must be nonempty.
			 */
			[[nodiscard]] const Record &last() const;
			/**
			 * @brief Appends one record, preserving all previously captured versions.
			 * @param record Received or sent GMCP record.
			 */
			void                        push_back(const Record &record);
			/**
			 * @brief Removes a contiguous range without relocating unrelated pages.
			 * @param index First arrival-order rank to remove.
			 * @param count Number of records to remove, bounded by the remaining size.
			 */
			void                        remove(qsizetype index, qsizetype count);
			/**
			 * @brief Removes ascending disjoint ranges from newest to oldest, retiring one original root.
			 * @param ranges Ranks in the version before any of these removals.
			 * @param aboutToRemove Optional owner-thread notification before each range changes.
			 * @param removed Optional owner-thread notification after each range changes.
			 */
			void                        removeRanges(const QList<Range>                       &ranges,
			                                         const std::function<void(const Range &)> &aboutToRemove,
			                                         const std::function<void()>              &removed);
			/**
			 * @brief Finds expired ranges in one-page traversal, protecting the latest five records.
			 * @param cutoff Records strictly older than this timestamp may be removed.
			 * @return Disjoint ranges in ascending arrival order, including out-of-order timestamps.
			 */
			[[nodiscard]] QList<Range>  expiredRanges(const QDateTime &cutoff) const;
			/**
			 * @brief Retires every record owned by this version.
			 */
			void                        clear();

		private:
			struct Node;
			std::shared_ptr<Node> m_root;
			quint64               m_tailPage{0};
			int                   m_height{0};
			/**
			 * @brief Transfers a released immutable tree to background destruction.
			 * @param root Root whose references are no longer needed by its caller.
			 */
			static void           retire(std::shared_ptr<Node> root);
	};
	/**
	 * @brief Parses an exact GMCP message without applying session semantics.
	 * @param raw Message bytes without Telnet framing.
	 * @param message Output decoded message.
	 * @param error Output validation diagnostic.
	 * @return Whether the message contains valid UTF-8 and optional JSON.
	 */
	bool               decode(const QByteArray &raw, Message &message, QString &error);
	/**
	 * @brief Frames a message with Telnet IAC escaping.
	 * @param raw Unframed message bytes.
	 * @return Telnet subnegotiation bytes.
	 */
	QByteArray         frame(const QByteArray &raw);
	/**
	 * @brief Converts a merge mode to its wire name.
	 * @param mode Mode to encode.
	 * @return Reserved-property value.
	 */
	QString            modeName(MergeMode mode);
	/**
	 * @brief Parses one reserved-property mode name.
	 * @param name Mode text.
	 * @param mode Output mode.
	 * @return Whether the text names a supported mode.
	 */
	bool               parseMode(const QString &name, MergeMode &mode);
	/**
	 * @brief Identifies the native-owned merge extension capability independently of name casing.
	 * @param name Candidate capability or module name.
	 * @return Whether the name is Client.GMCPMerge.
	 */
	[[nodiscard]] bool isMergeCapability(const QString &name);

	/**
	 * @brief GMCP-only negotiation, payload processing, publication, and persistence.
	 */
	class Session : public QObject
	{
		public:
			/**
			 * @brief Constructs an inactive session with its own publication endpoint.
			 */
			Session();
			/**
			 * @brief Publishes closed state and releases resources without invoking observers.
			 */
			~Session() override;
			/**
			 * @brief Disables copying a stateful session.
			 */
			Session(const Session &) = delete;
			/**
			 * @brief Disables assigning a stateful session.
			 * @return Assignment is unavailable.
			 */
			Session                                   &operator=(const Session &) = delete;
			/**
			 * @brief Returns the shared live publication endpoint.
			 * @return Endpoint safe to retain beyond transport teardown.
			 */
			[[nodiscard]] std::shared_ptr<Publication> publication() const;
			/**
			 * @brief Starts a connection generation and resets remote session state.
			 * @param native Whether native handling is selected for this connection.
			 * @param generation Connection identity.
			 */
			void                                       reset(bool native, quint64 generation);
			/**
			 * @brief Publishes a closed GMCP session and releases message state.
			 */
			void                                       close();
			/**
			 * @brief Processes one complete Telnet negotiation or subnegotiation for GMCP option 201.
			 * @param verb Telnet command.
			 * @param payload Unescaped GMCP payload for a subnegotiation, empty otherwise.
			 */
			void receive(unsigned char verb, const QByteArray &payload = {});
			/**
			 * @brief Records one complete GMCP message accepted by the transport.
			 * @param payload Unescaped outgoing GMCP message.
			 */
			void observeSent(const QByteArray &payload);
			/**
			 * @brief Configures an inheritable world-wide merge policy for future messages.
			 * @param name Package or message path, matched without case and inherited by descendants.
			 * @param mode Application mode.
			 * @param force Whether to override server declarations.
			 * @return Whether the name is valid.
			 */
			bool setMergePolicy(const QString &name, MergeMode mode, bool force = false);
			/**
			 * @brief Sends a validated native GMCP message.
			 * @param raw Message without Telnet framing.
			 * @param generation Required session identity.
			 * @return Whether the active transport accepted the message.
			 */
			bool send(const QByteArray &raw, quint64 generation);
			/**
			 * @brief Sets registered module support independently of native client capabilities.
			 * @param modules Module names mapped to positive versions; Client.GMCPMerge entries are ignored.
			 */
			void setModules(const QMap<QString, int> &modules);
			/**
			 * @brief Updates Core schedules and RTT disclosure on the owning transport thread.
			 * @param options Persistent world settings; intervals are expressed in minutes.
			 */
			void setCoreOptions(const CoreOptions &options);
			/**
			 * @brief Reports the last measured RTT, or -1 when a connection has no measurement.
			 */
			std::function<void(qint64)>          roundTripChanged;
			/**
			 * @brief Delivers a server's Core.Goodbye string for world output.
			 */
			std::function<void(const QString &)> goodbye;
			/**
			 * @brief Saves GMCP ownership, negotiation, application data, policies, and module support.
			 * @return Serializable GMCP state. Transport preservation eligibility is checked by its owner.
			 */
			[[nodiscard]] QByteArray             save() const;
			/**
			 * @brief Restores native state before descriptor adoption resumes input.
			 * @param bytes Previously saved state.
			 * @param generation New transport generation.
			 * @param activate Whether the transport is already attached and may publish notifications.
			 * @return Whether the complete state was restored.
			 */
			bool restore(const QByteArray &bytes, quint64 generation, bool activate = true);
			/**
			 * @brief Prepares preserved legacy ownership before startup Lua runs.
			 */
			void prepareLegacyRestoration();
			/**
			 * @brief Activates a prepared session without renegotiation.
			 * @param generation Adopted transport identity.
			 */
			void activateRestoration(quint64 generation);

			/**
			 * @brief Transport-thread write operation; returns accepted byte count.
			 */
			std::function<qint64(const QByteArray &)>                                   write;
			/**
			 * @brief Transport-thread observer for complete native messages and diagnostics.
			 */
			std::function<void(const Record &, const std::shared_ptr<const Message> &)> observed;
			/**
			 * @brief Transport-thread fatal error notification.
			 */
			std::function<void(const QString &)>                                        fatal;
			/**
			 * @brief Reports a native negotiation transition on the transport thread.
			 */
			std::function<void(bool)>                                                   stateChanged;

		private:
			struct Impl;
			std::unique_ptr<Impl> d;
			/**
			 * @brief Returns mutable state for a session operation.
			 * @return State owned by this session.
			 */
			[[nodiscard]] Impl   &impl() noexcept
			{
				return *d;
			}
			/**
			 * @brief Returns read-only state for a const session query.
			 * @return State owned by this session.
			 */
			[[nodiscard]] const Impl &impl() const noexcept
			{
				return *d;
			}
	};
} // namespace QMudGmcp
Q_DECLARE_METATYPE(QMudGmcp::Record)
Q_DECLARE_METATYPE(std::shared_ptr<const QMudGmcp::Message>)
#endif
