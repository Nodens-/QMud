/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: GmcpProtocol.cpp
 * Role: GMCP JSON policies, immutable publication, and GMCP session persistence.
 */
#include "GmcpProtocol.h"
#include "Version.h"
#include <QDataStream>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStringDecoder>
#include <QThreadPool>
#include <algorithm>
#include <utility>
#include <vector>

namespace QMudGmcp
{
	void PackageIndex::add(const QString &name)
	{
		QString parent;
		bool    first = true;
		for (const auto &part : name.split(QLatin1Char('.')))
		{
			const QString path = first ? part.toLower() : parent + QLatin1Char('.') + part.toLower();
			if (!branches.contains(path))
			{
				branches = branches.withUpdates({
				    {path, QSharedPointer<const PackageBranch>::create(PackageBranch{part, {}})}
                });
				if (first)
					roots.insert(std::ranges::lower_bound(roots, path), path);
				else
				{
					auto branch = *branches.value(parent);
					branch.children.insert(std::ranges::lower_bound(branch.children, path), path);
					branches = branches.withUpdates({
					    {parent, QSharedPointer<const PackageBranch>::create(std::move(branch))}
                    });
				}
			}
			first  = false;
			parent = path;
		}
	}
	namespace
	{
		constexpr unsigned char Iac = 255, Will = 251, Wont = 252, Do = 253, Dont = 254, Sb = 250;

		std::atomic<quint64>    nextSession{1};
		/**
		 * @brief Validates a message token.
		 * @param name Candidate token.
		 * @return Whether it is nonempty ASCII without whitespace.
		 */
		bool                    validName(const QString &name)
		{
			if (name.isEmpty())
				return false;
			return std::ranges::all_of(name,
			                           [](const QChar c) { return c.unicode() >= 33 && c.unicode() <= 126; });
		}
		/**
		 * @brief Applies object merging.
		 * @param old Previous value.
		 * @param incoming New value.
		 * @param mode Selected mode.
		 * @return Updated value.
		 */
		QJsonValue merge(const QJsonValue &old, const QJsonValue &incoming, const MergeMode mode)
		{
			if (mode == MergeMode::Replace || !old.isObject() || !incoming.isObject())
				return incoming;
			QJsonObject       result = old.toObject();
			const QJsonObject update = incoming.toObject();
			for (auto it = update.constBegin(); it != update.constEnd(); ++it)
				result.insert(it.key(), mode == MergeMode::Recursive
				                            ? merge(result.value(it.key()), it.value(), mode)
				                            : it.value());
			return result;
		}
		/**
		 * @brief Encodes an arbitrary JSON value.
		 * @param value Value to encode.
		 * @return JSON bytes, empty for an absent value.
		 */
		QByteArray json(const QJsonValue &value)
		{
			if (value.isUndefined())
				return {};
			const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
			return wrapped.mid(1, wrapped.size() - 2);
		}
		/**
		 * @brief Removes declarations for the native-owned merge extension from registered modules.
		 * @param modules Module declarations to filter in place.
		 */
		void removeMergeCapability(QMap<QString, int> &modules)
		{
			for (auto it = modules.begin(); it != modules.end();)
				if (isMergeCapability(it.key()))
					it = modules.erase(it);
				else
					++it;
		}
	} // namespace

	bool decode(const QByteArray &raw, Message &message, QString &error)
	{
		QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
		const QString  text = decoder.decode(raw);
		if (decoder.hasError())
		{
			error = QStringLiteral("Invalid UTF-8");
			return false;
		}
		const qsizetype space = text.indexOf(QLatin1Char(' '));
		const QString   name  = space < 0 ? text : text.left(space);
		if (!validName(name))
		{
			error = QStringLiteral("Invalid GMCP message name");
			return false;
		}
		Message parsed;
		parsed.name = name;
		parsed.raw  = raw;
		if (space >= 0)
		{
			const QByteArray body = text.mid(space + 1).toUtf8();
			QJsonParseError  parseError;
			const auto       document =
			    QJsonDocument::fromJson(QByteArrayLiteral("[") + body + QByteArrayLiteral("]"), &parseError);
			if (parseError.error != QJsonParseError::NoError || !document.isArray() ||
			    document.array().size() != 1)
			{
				error = QStringLiteral("Invalid GMCP JSON: %1").arg(parseError.errorString());
				return false;
			}
			parsed.payload = document.array().at(0);
		}
		parsed.state = parsed.payload;
		message      = std::move(parsed);
		error.clear();
		return true;
	}

	QByteArray frame(const QByteArray &raw)
	{
		QByteArray escaped = raw;
		escaped.replace(QByteArray(1, static_cast<char>(Iac)), QByteArray(2, static_cast<char>(Iac)));
		return QByteArray::fromHex("fffac9") + escaped + QByteArray::fromHex("fff0");
	}
	QString modeName(const MergeMode mode)
	{
		switch (mode)
		{
		case MergeMode::Replace:
			return QStringLiteral("replace");
		case MergeMode::Shallow:
			return QStringLiteral("shallow");
		case MergeMode::Recursive:
			return QStringLiteral("recursive");
		}
		Q_UNREACHABLE();
	}
	bool parseMode(const QString &name, MergeMode &mode)
	{
		if (name == QLatin1String("replace"))
			mode = MergeMode::Replace;
		else if (name == QLatin1String("shallow"))
			mode = MergeMode::Shallow;
		else if (name == QLatin1String("recursive"))
			mode = MergeMode::Recursive;
		else
			return false;
		return true;
	}

	bool isMergeCapability(const QString &name)
	{
		return name.compare(QLatin1String("Client.GMCPMerge"), Qt::CaseInsensitive) == 0;
	}

	/**
	 * @brief Private transport-thread state; only published immutable objects cross threads.
	 */
	struct Session::Impl
	{
			/**
			 * @brief Binds private state to its owning session before any protocol operation can run.
			 * @param session Session that owns this state and its observers.
			 */
			explicit Impl(Session &session) : owner(&session)
			{
			}
			struct Policy
			{
					MergeMode mode;
					bool      force;
			};
			Session *const               owner;
			std::shared_ptr<Publication> publication{std::make_shared<Publication>()};
			Snapshot                     state;
			QMap<QString, Policy>        local;
			QMap<QString, int>           modules{
			    {QStringLiteral("Char"), 1},
                {QStringLiteral("Comm"), 1},
                {QStringLiteral("Room"), 1}
            };
			bool    publishedEnabled{false};
			bool    modulesChangedWhileRestoring{false};
			bool    refusedGmcpDo{false};
			quint64 sequence{0};

			/**
			 * @brief Publishes the current immutable version.
			 */
			void    publish()
			{
				++state.revision;
				publication->current.store(std::make_shared<const Snapshot>(state),
				                           std::memory_order_release);
				const bool enabled = state.native && state.negotiated && !state.closed;
				if (!state.restoring && enabled != publishedEnabled)
				{
					publishedEnabled = enabled;
					if (owner->stateChanged)
						owner->stateChanged(enabled);
				}
			}
			/**
			 * @brief Reports a fatal GMCP write failure.
			 * @param error Diagnostic text.
			 */
			void fail(const QString &error)
			{
				state.negotiated = false;
				publish();
				if (owner->fatal)
					owner->fatal(error);
			}
			/**
			 * @brief Observes an unframed native message.
			 * @param raw Message bytes.
			 * @param outgoing Whether it was sent.
			 * @param error Diagnostic text.
			 * @param message Optional decoded event.
			 */
			void record(const QByteArray &raw, bool outgoing, const QString &error = {},
			            const std::shared_ptr<const Message> &message = {})
			{
				if (owner->observed)
					owner->observed(Record{QDateTime::currentDateTimeUtc(), state.generation, ++sequence,
					                       outgoing, raw, error, state.messages, state.packages,
					                       state.revision},
					                message);
			}
			/**
			 * @brief Writes protocol bytes without involving the runtime.
			 * @param bytes Framed bytes.
			 * @return Whether all bytes were accepted.
			 */
			bool write(const QByteArray &bytes)
			{
				if (owner->write && owner->write(bytes) == bytes.size())
					return true;
				fail(QStringLiteral("Unable to send complete native Telnet protocol data."));
				return false;
			}
			/**
			 * @brief Sends one GMCP message.
			 * @param raw Message bytes.
			 * @return Whether accepted.
			 */
			bool send(const QByteArray &raw)
			{
				if (!write(frame(raw)))
					return false;

				return true;
			}
			/**
			 * @brief Advertises native merge extension version 1 and the current registered modules.
			 */
			void supports()
			{
				QJsonArray list{QStringLiteral("Client.GMCPMerge 1")};
				for (auto it = modules.constBegin(); it != modules.constEnd(); ++it)
					list.append(it.key() + QLatin1Char(' ') + QString::number(it.value()));
				send(QByteArrayLiteral("Core.Supports.Set ") +
				     QJsonDocument(list).toJson(QJsonDocument::Compact));
			}
			/**
			 * @brief Applies one complete received GMCP payload.
			 * @param payload Unframed GMCP message.
			 */
			void message(const QByteArray &payload)
			{
				if (!state.negotiated)
				{
					record(payload, false, QStringLiteral("GMCP received before negotiation"));
					return;
				}
				Message decoded;
				QString error;
				if (!decode(payload, decoded, error))
				{
					record(payload, false, error);
					return;
				}
				const QString key = decoded.name.toLower();
				if (decoded.payload.isObject())
				{
					auto          object   = decoded.payload.toObject();
					const QString metadata = QStringLiteral("__gmcp_merge");
					if (object.contains(metadata))
					{
						MergeMode mode;
						if (object.value(metadata).isString() &&
						    parseMode(object.value(metadata).toString(), mode))
							state.serverModes.insert(key, mode);
						else
							error = QStringLiteral("Invalid __gmcp_merge declaration");
						object.remove(metadata);
						decoded.payload = object;
					}
				}
				const MergeMode mode     = state.effectiveMode(key);
				const auto      previous = state.messages.value(key);
				if (!previous)
				{
					state.packages.add(decoded.name);
					QString path = key;
					while (path.contains(QLatin1Char('.')))
					{
						path = path.left(path.lastIndexOf(QLatin1Char('.')));
						state.namespaces.insert(path);
					}
				}

				decoded.state    = decoded.payload.isUndefined()
				                       ? QJsonValue(QJsonValue::Undefined)
				                       : merge(previous ? previous->state : QJsonValue(QJsonValue::Undefined),
				                               decoded.payload, mode);
				decoded.sequence = sequence + 1;
				decoded.session  = state.session;
				auto message     = std::make_shared<const Message>(decoded);
				state.messages   = state.messages.withUpdates({
				    {key, QSharedPointer<const Message>::create(decoded)}
                });
				publish();
				record(payload, false, error, message);
			}
			/**
			 * @brief Handles one complete GMCP negotiation or subnegotiation.
			 * @param verb Telnet command for option 201.
			 * @param payload Complete unescaped GMCP subnegotiation payload.
			 */
			void receive(unsigned char verb, const QByteArray &payload)
			{
				if (verb == Sb)
					message(payload);
				else if (verb == Will)
				{
					if (!state.negotiated)
					{
						if (!write(QByteArray::fromHex("fffdc9")))
							return;
						state.negotiated = true;
						state.session    = nextSession.fetch_add(1, std::memory_order_relaxed);
						QString version  = QString::fromLatin1(kVersionString);
#ifdef CI_BUILD
						version += QStringLiteral("-ci");
#endif
						send(QByteArrayLiteral("Core.Hello ") +
						     QJsonDocument(QJsonObject{
						                       {QStringLiteral("client"),  QStringLiteral("QMud")},
						                       {QStringLiteral("version"), version               }
                        })
						         .toJson(QJsonDocument::Compact));
						supports();
						publish();
					}
				}
				else if (verb == Wont)
				{
					if (state.negotiated)
						write(QByteArray::fromHex("fffec9"));
					state.negotiated = false;
					state.messages   = {};
					state.packages   = {};
					state.namespaces.clear();
					state.session = nextSession.fetch_add(1, std::memory_order_relaxed);
					state.serverModes.clear();
					publish();
				}
				else if (verb == Do && !refusedGmcpDo)
				{
					refusedGmcpDo = true;
					write(QByteArray::fromHex("fffcc9"));
				}
				else if (verb == Dont)
					refusedGmcpDo = false;
			}
	};

	Session::Session() : d(std::make_unique<Impl>(*this))
	{
	}
	Session::~Session()
	{
		stateChanged = {};
		observed     = {};
		fatal        = {};
		write        = {};
		close();
	}
	std::shared_ptr<Publication> Session::publication() const
	{
		return impl().publication;
	}
	void Session::reset(bool native, quint64 generation)
	{
		auto &data                    = impl();
		data.state                    = {};
		data.state.native             = native;
		data.state.generation         = generation;
		data.state.connectionSelected = true;
		data.state.session            = nextSession.fetch_add(1, std::memory_order_relaxed);
		for (auto it = data.local.constBegin(); it != data.local.constEnd(); ++it)
		{
			data.state.localModes.insert(it.key(), it->mode);
			if (it->force)
				data.state.forcedModes.insert(it.key());
		}

		data.sequence                     = 0;
		data.refusedGmcpDo                = false;
		data.modulesChangedWhileRestoring = false;
		data.publish();
	}
	void Session::close()
	{
		auto &data = impl();
		if (data.state.closed && !data.state.restoring)
			return;
		data.state.negotiated         = false;
		data.state.closed             = true;
		data.state.restoring          = false;
		data.state.connectionSelected = false;
		data.state.session            = nextSession.fetch_add(1, std::memory_order_relaxed);
		data.state.messages           = {};
		data.state.packages           = {};
		data.state.namespaces.clear();
		data.state.serverModes.clear();
		data.publish();
	}
	bool Session::setMergePolicy(const QString &name, MergeMode mode, bool force)
	{
		auto &data = impl();
		if (!validName(name))
			return false;
		data.local.insert(name.toLower(), {mode, force});
		const QString key = name.toLower();
		data.state.localModes.insert(key, mode);
		if (force)
			data.state.forcedModes.insert(key);
		else
			data.state.forcedModes.remove(key);
		data.publish();

		return true;
	}
	bool Session::send(const QByteArray &raw, quint64 generation)
	{
		auto   &data = impl();
		Message message;
		QString error;
		return data.state.negotiated && !data.state.closed && !data.state.restoring &&
		       generation == data.state.session && decode(raw, message, error) && data.send(raw);
	}
	void Session::setModules(const QMap<QString, int> &modules)
	{
		auto &data    = impl();
		auto  desired = modules;
		removeMergeCapability(desired);
		if (desired == data.modules)
			return;
		data.modules = desired;
		if (data.state.restoring)
			data.modulesChangedWhileRestoring = true;
		else if (data.state.negotiated)
			data.supports();
	}
	void Session::receive(unsigned char verb, const QByteArray &payload)
	{
		auto &data = impl();
		if (data.state.native && !data.state.closed)
			data.receive(verb, payload);
	}

	QByteArray Session::save() const
	{
		const auto &data = impl();
		QByteArray  bytes;
		QDataStream stream(&bytes, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_6_0);
		stream << quint32{1} << data.state.native << data.state.negotiated << data.sequence << data.modules;
		const auto keys = data.state.messages.keys();
		stream << keys.size();
		for (const auto &key : keys)
		{
			const auto m = data.state.messages.value(key);
			stream << key << m->name << m->raw << json(m->payload) << json(m->state) << m->sequence;
		}
		stream << data.state.serverModes.size();
		for (auto it = data.state.serverModes.constBegin(); it != data.state.serverModes.constEnd(); ++it)
			stream << it.key() << modeName(it.value());
		stream << data.local.size();
		for (auto it = data.local.constBegin(); it != data.local.constEnd(); ++it)
			stream << it.key() << modeName(it->mode) << it->force;
		return bytes;
	}
	bool Session::restore(const QByteArray &bytes, quint64 generation, bool activate)
	{
		auto       &data = impl();
		QDataStream stream(bytes);
		stream.setVersion(QDataStream::Qt_6_0);
		quint32            version = 0;
		bool               native = false, negotiated = false;
		quint64            sequence = 0;
		QMap<QString, int> modules;
		stream >> version >> native >> negotiated >> sequence >> modules;
		if (version != 1 || !native)
			return false;
		Snapshot replacement;
		replacement.native             = native;
		replacement.connectionSelected = true;
		replacement.restoring          = !activate;
		replacement.negotiated         = negotiated;
		replacement.generation         = generation;
		replacement.session            = nextSession.fetch_add(1, std::memory_order_relaxed);
		qsizetype count                = 0;
		stream >> count;
		if (count < 0 || count > 100000)
			return false;
		QVector<LuaCallbackPersistentStringMap<Message>::Update> updates;
		for (qsizetype i = 0; i < count; ++i)
		{
			QString    key;
			Message    message;
			QByteArray body, state;
			stream >> key >> message.name >> message.raw >> body >> state >> message.sequence;
			Message parsed;
			QString error;
			if (!body.isEmpty())
			{
				if (!decode(QByteArrayLiteral("Value ") + body, parsed, error))
					return false;
				message.payload = parsed.payload;
			}
			if (!state.isEmpty())
			{
				if (!decode(QByteArrayLiteral("Value ") + state, parsed, error))
					return false;
				message.state = parsed.payload;
			}
			updates.push_back({key, QSharedPointer<const Message>::create(message)});
		}
		replacement.messages = replacement.messages.withUpdates(updates);
		for (const auto &update : updates)
		{
			replacement.packages.add(update.value->name);
			QString path = update.key;
			while (path.contains(QLatin1Char('.')))
			{
				path = path.left(path.lastIndexOf(QLatin1Char('.')));
				replacement.namespaces.insert(path);
			}
		}

		stream >> count;
		if (count < 0 || count > 100000)
			return false;
		for (qsizetype i = 0; i < count; ++i)
		{
			QString   key, name;
			MergeMode mode;
			stream >> key >> name;
			if (!parseMode(name, mode))
				return false;
			replacement.serverModes.insert(key, mode);
		}
		QMap<QString, Impl::Policy> local;
		stream >> count;
		if (count < 0 || count > 100000)
			return false;
		for (qsizetype i = 0; i < count; ++i)
		{
			QString   key, name;
			bool      force = false;
			MergeMode mode;
			stream >> key >> name >> force;
			if (!parseMode(name, mode))
				return false;
			local.insert(key, {mode, force});
		}
		if (stream.status() != QDataStream::Ok || !stream.atEnd())
			return false;
		data.state                        = std::move(replacement);
		data.sequence                     = sequence;
		data.modules                      = modules;
		data.modulesChangedWhileRestoring = false;
		data.local                        = local;
		for (auto it = local.constBegin(); it != local.constEnd(); ++it)
		{
			data.state.localModes.insert(it.key(), it->mode);
			if (it->force)
				data.state.forcedModes.insert(it.key());
		}

		data.publish();
		return true;
	}
	void Session::prepareLegacyRestoration()
	{
		auto &data                        = impl();
		data.state                        = {};
		data.state.restoring              = true;
		data.state.connectionSelected     = true;
		data.state.session                = nextSession.fetch_add(1, std::memory_order_relaxed);
		data.modulesChangedWhileRestoring = false;
		data.publish();
	}
	void Session::activateRestoration(quint64 generation)
	{
		auto &data            = impl();
		data.state.generation = generation;
		data.state.restoring  = false;
		data.publish();
		if (std::exchange(data.modulesChangedWhileRestoring, false) && data.state.negotiated)
			data.supports();
	}

} // namespace QMudGmcp

namespace QMudGmcp
{
	void Session::observeSent(const QByteArray &payload)
	{
		auto &data = impl();
		if (!data.state.native)
			return;
		Message decoded;
		QString error;
		decode(payload, decoded, error);
		data.record(payload, true, error);
	}

} // namespace QMudGmcp

namespace QMudGmcp
{
	MergeMode Snapshot::effectiveMode(const QString &name) const
	{
		const auto key = name.toLower();
		if (forcedModes.contains(key))
			return localModes.value(key);
		if (serverModes.contains(key))
			return serverModes.value(key);
		return localModes.value(key, key == QLatin1String("char.status") ? MergeMode::Shallow
		                                                                 : MergeMode::Replace);
	}
} // namespace QMudGmcp

namespace QMudGmcp
{
	/**
	 * @brief One sparse radix-index node or one bounded record page.
	 */
	struct History::Node
	{
			static constexpr qsizetype PageSize      = 64;
			static constexpr int       BitsPerLevel  = 5;
			static constexpr int       MaximumHeight = 13;
			/**
			 * @brief One indexed child page range, ordered by radix branch.
			 */
			struct Child
			{
					quint64               branch;
					std::shared_ptr<Node> node;
			};
			std::vector<Child>  children;
			std::vector<Record> records;
			qsizetype           count{0};

			/**
			 * @brief Obtains exclusive storage before changing a node retained by another version.
			 * @param node Node to create or detach.
			 */
			static void         edit(std::shared_ptr<Node> &node)
			{
				if (!node)
					node = std::make_shared<Node>();
				else if (node.use_count() != 1)
				{
					auto replacement      = std::make_shared<Node>();
					replacement->children = node->children;
					replacement->count    = node->count;
					if (!node->records.empty())
					{
						replacement->records.reserve(PageSize);
						replacement->records.assign(node->records.begin(), node->records.end());
					}
					node = std::move(replacement);
				}
			}
			/**
			 * @brief Selects an absolute page's branch at one index height.
			 * @param page Absolute page identity.
			 * @param height Positive index height, bounded by MaximumHeight.
			 * @return Radix branch number.
			 */
			static quint64 branchFor(quint64 page, int height)
			{
				return (page >> ((height - 1) * BitsPerLevel)) & 31U;
			}
			/**
			 * @brief Resolves an absolute page without retaining or modifying index nodes.
			 * @param node Existing root.
			 * @param height Root height.
			 * @param page Absolute page identity.
			 * @return Existing page, or null when removed.
			 */
			static const Node *page(const Node *node, int height, quint64 page)
			{
				while (node && height > 0)
				{
					const auto branch = branchFor(page, height--);
					const auto child  = std::ranges::lower_bound(node->children, branch, {}, &Child::branch);
					node = child != node->children.end() && child->branch == branch ? child->node.get()
					                                                                : nullptr;
				}
				return node;
			}
			/**
			 * @brief Appends through one writable index path and tail page.
			 * @param node Root of the changed range.
			 * @param height Range height.
			 * @param page Absolute tail-page identity.
			 * @param record Record to append.
			 */
			static void append(std::shared_ptr<Node> &node, int height, quint64 page, const Record &record)
			{
				edit(node);
				if (height == 0)
				{
					node->records.reserve(PageSize);
					node->records.push_back(record);
				}
				else
				{
					const auto branch = branchFor(page, height);
					auto       child  = std::ranges::lower_bound(node->children, branch, {}, &Child::branch);
					if (child == node->children.end() || child->branch != branch)
						child = node->children.insert(child, Child{branch, {}});
					append(child->node, height - 1, page, record);
				}
				++node->count;
			}
			/**
			 * @brief Removes ranks through affected pages while an old root retains all retired storage.
			 * @param node Root of the changed range.
			 * @param height Range height.
			 * @param index First rank within the range.
			 * @param count Number of ranks to remove.
			 */
			static void remove(std::shared_ptr<Node> &node, int height, qsizetype index, qsizetype count)
			{
				if (count == node->count)
				{
					node.reset();
					return;
				}
				edit(node);
				node->count -= count;
				if (height == 0)
				{
					node->records.erase(node->records.begin() + index, node->records.begin() + index + count);
					return;
				}
				for (auto child = node->children.begin(); count > 0;)
				{
					if (index >= child->node->count)
					{
						index -= child->node->count;
						++child;
						continue;
					}
					const auto removed = std::min(count, child->node->count - index);
					remove(child->node, height - 1, index, removed);
					count -= removed;
					index = 0;
					if (!child->node)
						child = node->children.erase(child);
					else
						++child;
				}
			}
			/**
			 * @brief Visits existing pages once to collect expired arrival-order ranges.
			 * @param node Existing index or page.
			 * @param index Current arrival-order rank.
			 * @param limit First protected rank.
			 * @param cutoff Strict expiration boundary.
			 * @param ranges Output disjoint expired ranges.
			 */
			static void collectExpired(const Node &node, qsizetype &index, qsizetype limit,
			                           const QDateTime &cutoff, QList<Range> &ranges)
			{
				for (const auto &child : node.children)
				{
					if (index >= limit)
						return;
					collectExpired(*child.node, index, limit, cutoff, ranges);
				}
				for (const auto &record : node.records)
				{
					if (index >= limit)
						return;
					if (record.time < cutoff)
					{
						if (!ranges.isEmpty() && ranges.last().first + ranges.last().count == index)
							++ranges.last().count;
						else
							ranges.push_back({index, 1});
					}
					++index;
				}
			}
	};

	History::~History()
	{
		retire(std::move(m_root));
	}

	History &History::operator=(const History &other)
	{
		if (this != &other)
		{
			auto previous = std::exchange(m_root, other.m_root);
			m_tailPage    = other.m_tailPage;
			m_height      = other.m_height;
			retire(std::move(previous));
		}
		return *this;
	}

	History &History::operator=(History &&other) noexcept
	{
		if (this != &other)
		{
			auto previous = std::exchange(m_root, std::move(other.m_root));
			m_tailPage    = other.m_tailPage;
			m_height      = other.m_height;
			retire(std::move(previous));
		}
		return *this;
	}

	qsizetype History::size() const noexcept
	{
		return m_root ? m_root->count : 0;
	}

	bool History::isEmpty() const noexcept
	{
		return !m_root;
	}

	const Record &History::at(qsizetype index) const
	{
		Q_ASSERT(index >= 0 && index < size());
		const Node *node = m_root.get();
		while (!node->children.empty())
		{
			for (const auto &child : node->children)
			{
				if (index < child.node->count)
				{
					node = child.node.get();
					break;
				}
				index -= child.node->count;
			}
		}
		return node->records.at(static_cast<std::size_t>(index));
	}

	const Record &History::first() const
	{
		return at(0);
	}

	const Record &History::last() const
	{
		return at(size() - 1);
	}

	void History::push_back(const Record &record)
	{
		if (!m_root)
		{
			m_tailPage = 0;
			m_height   = 0;
		}
		else if (const auto *tail = Node::page(m_root.get(), m_height, m_tailPage);
		         !tail || tail->records.size() == Node::PageSize)
		{
			++m_tailPage;
			while (m_height < Node::MaximumHeight && (m_tailPage >> (m_height * Node::BitsPerLevel)) != 0)
			{
				auto parent   = std::make_shared<Node>();
				parent->count = m_root->count;
				parent->children.push_back({0, std::move(m_root)});
				m_root = std::move(parent);
				++m_height;
			}
		}
		Node::append(m_root, m_height, m_tailPage, record);
	}

	void History::remove(qsizetype index, qsizetype count)
	{
		Q_ASSERT(index >= 0 && count >= 0 && index <= size() && count <= size() - index);
		if (count == 0)
			return;
		auto previous = m_root;
		Node::remove(m_root, m_height, index, count);
		retire(std::move(previous));
	}

	void History::clear()
	{
		retire(std::move(m_root));
		m_tailPage = 0;
		m_height   = 0;
	}

	void History::removeRanges(const QList<Range>                       &ranges,
	                           const std::function<void(const Range &)> &aboutToRemove,
	                           const std::function<void()>              &removed)
	{
		if (ranges.isEmpty())
			return;
		auto previous = m_root;
		for (auto range = ranges.crbegin(); range != ranges.crend(); ++range)
		{
			Q_ASSERT(range->first >= 0 && range->count > 0 && range->first < size() &&
			         range->count <= size() - range->first);
			if (aboutToRemove)
				aboutToRemove(*range);
			Node::remove(m_root, m_height, range->first, range->count);
			if (removed)
				removed();
		}
		retire(std::move(previous));
	}

	QList<History::Range> History::expiredRanges(const QDateTime &cutoff) const
	{
		QList<Range> ranges;
		qsizetype    index = 0;
		if (size() > 5)
			Node::collectExpired(*m_root, index, size() - 5, cutoff, ranges);
		return ranges;
	}

	void History::retire(std::shared_ptr<Node> root)
	{
		if (root)
			QThreadPool::globalInstance()->start([root = std::move(root)]() mutable { root.reset(); });
	}
} // namespace QMudGmcp
