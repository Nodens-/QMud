/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: GmcpBrowserDialog.cpp
 * Role: Direct JSON model views and independent raw inspection for current and historical GMCP data.
 */
#include "GmcpBrowserDialog.h"
#include "GmcpProtocol.h"
#include "WorldRuntime.h"
// ReSharper disable once CppUnusedIncludeDirective
#include <QAbstractItemModel>
#include <QAbstractListModel>
#include <QCheckBox>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPromise>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableView>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QThreadPool>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <map>
#include <optional>
#include <set>

namespace
{
	/**
	 * @brief Prepared text for one package; offsets are UTF-16 document positions.
	 */
	struct GmcpRawPart
	{
			QString key;
			QString name;
			QString text;
			int     start{0};
	};
	/**
	 * @brief Stable document blocks delimiting a package without retaining a cursor for every package.
	 */
	struct GmcpRawRange
	{
			QTextBlock first;
			QTextBlock last;
	};
	/**
	 * @brief Background formatting result for one binding or a set of changed packages.
	 */
	struct GmcpRawBatch
	{
			quint64            version{0};
			bool               full{false};
			bool               cancelled{false};
			std::set<QString>  keys;
			QList<GmcpRawPart> parts;
			QString            document;
	};
	/**
	 * @brief Formats immutable package values off the GUI thread, touching all packages only for a new
	 * binding.
	 * @param messages Captured immutable package values.
	 * @param keys Changed canonical package names.
	 * @param full Whether this is the initial document for a binding.
	 * @param version Binding identity used to reject obsolete completions.
	 * @param cancelled Cancellation shared with the owning view.
	 * @return Prepared package fragments and optional initial document.
	 */
	GmcpRawBatch formatRawPackages(const LuaCallbackPersistentStringMap<QMudGmcp::Message> &messages,
	                               std::set<QString> keys, bool full, quint64 version,
	                               const std::shared_ptr<std::atomic_bool> &cancelled)
	{
		GmcpRawBatch result;
		result.version = version;
		result.full    = full;
		result.keys    = std::move(keys);
		auto append    = [&](const QString &key, const auto &message)
		{
			if (cancelled->load(std::memory_order_relaxed))
				return;
			const auto bytes =
			    QJsonDocument(QJsonObject{
			                      {message->name, message->state.isUndefined() ? QJsonValue(QJsonValue::Null)
			                                                                   : message->state}
            })
			        .toJson(QJsonDocument::Indented);
			// Retain the member indentation while removing the singleton object framing.
			result.parts.push_back({key, message->name, QString::fromUtf8(bytes.mid(2, bytes.size() - 5))});
		};
		if (full)
			messages.forEach(append);
		else
			for (const auto &key : result.keys)
			{
				if (cancelled->load(std::memory_order_relaxed))
					break;
				if (const auto message = messages.value(key))
					append(key, message);
			}
		result.cancelled = cancelled->load(std::memory_order_relaxed);
		if (full && !result.cancelled)
		{
			std::ranges::sort(result.parts, {}, &GmcpRawPart::name);
			result.document = QStringLiteral("{\n");
			for (auto &part : result.parts)
			{
				if (result.document.size() > 2)
					result.document += QStringLiteral(",\n");
				part.start = static_cast<int>(result.document.size());
				result.document += part.text;
			}
			if (!result.parts.isEmpty())
				result.document += QLatin1Char('\n');
			result.document += QStringLiteral("}\n");
		}
		return result;
	}

	constexpr int               PathRole = Qt::UserRole + 1;
	/**
	 * @brief Identifies one immutable history entry across list updates.
	 * @param record Version record.
	 * @return Stable connection/sequence identity.
	 */
	std::pair<quint64, quint64> identity(const QMudGmcp::Record &record)
	{
		return {record.generation, record.sequence};
	}
	/**
	 * @brief Releases discarded immutable browser data without accessing any GUI or runtime object.
	 * @param messages Released message map.
	 * @param packages Released namespace index.
	 * @param wire Released historical wire message.
	 */
	void retireBrowserData(LuaCallbackPersistentStringMap<QMudGmcp::Message> messages,
	                       QMudGmcp::PackageIndex packages, std::optional<QByteArray> wire = {})
	{
		if (messages.isEmpty() && packages.branches.isEmpty() && packages.roots.isEmpty() && !wire)
			return;
		QThreadPool::globalInstance()->start(
		    [messages = std::move(messages), packages = std::move(packages), wire = std::move(wire)]() mutable
		    {
			    messages = {};
			    packages = {};
			    wire.reset();
		    });
	}
	/**
	 * @brief Supplies a requested cell without serializing its JSON.
	 * @param value Published JSON value.
	 * @return Container description or existing scalar value.
	 */
	QVariant displayValue(const QJsonValue &value)
	{
		if (value.isObject())
			return QStringLiteral("object");
		if (value.isArray())
			return QStringLiteral("array [%1]").arg(value.toArray().size());
		if (value.isUndefined())
			return QStringLiteral("(no payload)");
		if (value.isNull())
			return QStringLiteral("null");
		return value.toVariant();
	}

	/**
	 * @brief Semantic location retained for explicit view choices and Qt's requested indexes.
	 */
	struct GmcpAddress
	{
			QString     package;
			QStringList fields;
	};
	/**
	 * @brief Moves a semantic path to its immediate parent without reading JSON.
	 * @param address Path to shorten in place.
	 * @return Whether a parent exists.
	 */
	bool parentAddress(GmcpAddress &address)
	{
		if (!address.fields.isEmpty())
		{
			address.fields.removeLast();
			return true;
		}
		const auto dot = address.package.lastIndexOf(QLatin1Char('.'));
		if (dot < 0)
			return false;
		address.package.truncate(dot);
		return true;
	}

	/**
	 * @brief Reads the existing JSON type without encoding its value.
	 * @param value Published value.
	 * @return Fixed type marker.
	 */
	QString typeMarker(const QJsonValue &value)
	{
		switch (value.type())
		{
		case QJsonValue::Null:
			return QStringLiteral("null");
		case QJsonValue::Bool:
			return QStringLiteral("boolean");
		case QJsonValue::Double:
			return QStringLiteral("number");
		case QJsonValue::String:
			return QStringLiteral("string");
		case QJsonValue::Array:
			return QStringLiteral("array");
		case QJsonValue::Object:
			return QStringLiteral("object");
		case QJsonValue::Undefined:
			return QStringLiteral("no payload");
		}
		Q_UNREACHABLE();
	}
	/**
	 * @brief Direct JSON adapter with replacement confined to the package named by a received message.
	 */
	class GmcpTreeModel final : public QAbstractItemModel
	{
			/**
			 * @brief Flat Qt index token; owns no JSON data and has no child navigation tree.
			 */
			struct Location
			{
					GmcpAddress address;
			};
			using Locations = std::map<QStringList, std::unique_ptr<Location>>;

		public:
			/**
			 * @brief Constructs an empty object adapter.
			 * @param parent QObject owner.
			 */
			explicit GmcpTreeModel(QObject *parent) : QAbstractItemModel(parent)
			{
			}
			/**
			 * @brief Transfers the model's remaining immutable binding to background cleanup.
			 */
			~GmcpTreeModel() override
			{
				retireBrowserData(std::exchange(m_messages, {}), std::exchange(m_packages, {}));
			}
			/**
			 * @brief Binds an existing version for initial display, explicit resume, or history selection.
			 * @param messages Published message map.
			 * @param packages Published namespace index.
			 */
			void setVersion(const LuaCallbackPersistentStringMap<QMudGmcp::Message> &messages,
			                const QMudGmcp::PackageIndex                            &packages)
			{
				beginResetModel();
				auto previousMessages = std::exchange(m_messages, messages);
				auto previousPackages = std::exchange(m_packages, packages);
				m_jsonLocations.clear();
				m_packageLocations.clear();
				endResetModel();
				retireBrowserData(std::move(previousMessages), std::move(previousPackages));
			}
			/**
			 * @brief Replaces only a received package's bound JSON object, without inspecting its fields.
			 * @param record Exact publication containing the message.
			 * @param message Received package.
			 */
			void replacePackage(const QMudGmcp::Record &record, const QMudGmcp::Message &message)
			{
				const auto key = message.name.toLower();
				if (!m_packages.branches.contains(key))
				{
					QString parentPath;
					bool    first = true;
					for (const auto &part : message.name.split(QLatin1Char('.')))
					{
						const QString path =
						    first ? part.toLower() : parentPath + QLatin1Char('.') + part.toLower();
						if (!m_packages.branches.contains(path))
						{
							const auto  parentIndex = first ? QModelIndex{} : packageIndex(parentPath);
							const auto &siblings = first
							                           ? record.packages.roots
							                           : record.packages.branches.value(parentPath)->children;
							const int   row      = static_cast<int>(std::ranges::lower_bound(siblings, path) -
							                                        siblings.cbegin());
							beginInsertRows(parentIndex, row, row);
							m_messages = record.messages;
							m_packages = record.packages;
							endInsertRows();
							return;
						}
						first      = false;
						parentPath = path;
					}
				}
				const auto package  = packageIndex(key);
				const auto old      = m_messages.value(key);
				const int  oldCount = old ? count(old->state) : 0;
				const int  newCount = count(message.state);
				const int  offset   = static_cast<int>(children(key).size());
				if (oldCount > 0)
					beginRemoveRows(package, offset, offset + oldCount - 1);
				m_hiddenPackage = key;
				m_jsonLocations.erase(key);
				m_messages = record.messages;
				m_packages = record.packages;
				if (oldCount > 0)
					endRemoveRows();
				if (newCount > 0)
					beginInsertRows(package, offset, offset + newCount - 1);
				m_hiddenPackage.reset();
				if (newCount > 0)
					endInsertRows();
				emit dataChanged(package, package.siblingAtColumn(2));
			}
			/**
			 * @brief Returns the semantic address of an existing Qt index.
			 * @param index Requested index.
			 * @return Its package and typed JSON field path.
			 */
			static GmcpAddress address(const QModelIndex &index)
			{
				Q_ASSERT(index.isValid());
				return static_cast<Location *>(index.internalPointer())->address;
			}
			/**
			 * @brief Resolves a saved view choice through direct key/index lookups.
			 * @param wanted Package and typed field path.
			 * @param nearest Whether a missing path falls back to its closest surviving ancestor.
			 * @return Requested index, nearest ancestor when enabled, or invalid if none survives.
			 */
			QModelIndex indexFor(const GmcpAddress &wanted, bool nearest = false) const
			{
				auto current = packageIndex(wanted.package);
				if (!current.isValid() && nearest)
				{
					auto ancestor = wanted.package;
					for (auto dot = ancestor.lastIndexOf(QLatin1Char('.')); dot >= 0;
					     dot      = ancestor.lastIndexOf(QLatin1Char('.')))
					{
						ancestor.truncate(dot);
						if (const auto parent = packageIndex(ancestor); parent.isValid())
							return parent;
					}
				}
				if (!current.isValid())
					return {};
				for (const auto &field : wanted.fields)
				{
					const auto owner = address(current);
					const auto value = jsonValue(owner);
					const int  offset =
					    owner.fields.isEmpty() ? static_cast<int>(children(owner.package).size()) : 0;
					int row = -1;
					if (field.startsWith(QLatin1String("m:")) && value.isObject())
					{
						const auto object = value.toObject();
						const auto it     = object.constFind(field.mid(2));
						if (it != object.constEnd())
							row = static_cast<int>(it - object.constBegin());
					}
					else if (field.startsWith(QLatin1String("a:")) && value.isArray())
					{
						const int element = field.mid(2).toInt();
						if (element >= 0 && element < value.toArray().size())
							row = element;
					}
					if (row < 0)
						return nearest ? current : QModelIndex{};
					current = index(offset + row, 0, current);
				}
				return current;
			}
			/**
			 * @brief Reads one requested child directly from the existing containers.
			 * @param row Requested row.
			 * @param column Requested column.
			 * @param parent Parent index.
			 * @return Qt index for that row.
			 */
			QModelIndex index(int row, int column, const QModelIndex &parent) const override
			{
				if (row < 0 || column < 0 || column >= 3 || (parent.isValid() && parent.column() != 0))
					return {};
				if (!parent.isValid())
					return row < m_packages.roots.size() ? packageIndex(m_packages.roots.at(row), column)
					                                     : QModelIndex{};
				auto        owner      = address(parent);
				const auto &namespaces = owner.fields.isEmpty() ? children(owner.package) : emptyChildren();
				if (row < namespaces.size())
					return packageIndex(namespaces.at(row), column);
				const auto value  = jsonValue(owner);
				const int  offset = row - static_cast<int>(namespaces.size());
				QString    field;
				if (value.isObject())
				{
					const auto object = value.toObject();
					if (offset >= object.size())
						return {};
					field = QStringLiteral("m:") + (object.constBegin() + offset).key();
				}
				else if (value.isArray())
				{
					if (offset >= value.toArray().size())
						return {};
					field = QStringLiteral("a:") + QString::number(offset);
				}
				else
					return {};
				owner.fields.append(field);
				auto &location = m_jsonLocations[owner.package][owner.fields];
				if (!location)
					location = std::make_unique<Location>(Location{owner});
				return createIndex(row, column, location.get());
			}
			/**
			 * @brief Resolves a parent on demand without retaining parent/child navigation trees.
			 * @param child Child index.
			 * @return Parent index, or invalid for roots.
			 */
			QModelIndex parent(const QModelIndex &child) const override
			{
				if (!child.isValid())
					return {};
				auto location = address(child);
				if (!location.fields.isEmpty())
				{
					location.fields.removeLast();
					return indexFor(location);
				}
				const auto dot = location.package.lastIndexOf(QLatin1Char('.'));
				return dot < 0 ? QModelIndex{} : packageIndex(location.package.left(dot));
			}
			/**
			 * @brief Counts immediate children without traversing their values.
			 * @param parent Parent index.
			 * @return Existing container's child count.
			 */
			int rowCount(const QModelIndex &parent) const override
			{
				if (!parent.isValid())
					return static_cast<int>(m_packages.roots.size());
				if (parent.column() != 0)
					return 0;
				const auto location = address(parent);
				return (location.fields.isEmpty() ? static_cast<int>(children(location.package).size()) : 0) +
				       count(jsonValue(location));
			}
			/**
			 * @brief Returns the name, value, and type columns.
			 * @param parent Unused parent index.
			 * @return Three columns.
			 */
			int columnCount(const QModelIndex &parent) const override
			{
				Q_UNUSED(parent);
				return 3;
			}
			/**
			 * @brief Reads one requested cell or dotted filter path.
			 * @param index Requested index.
			 * @param role Qt data role.
			 * @return Existing value, label, or type marker.
			 */
			QVariant data(const QModelIndex &index, int role) const override
			{
				if (!index.isValid())
					return {};
				const auto location = address(index);
				if (role == PathRole)
				{
					QString path = location.package;
					for (const auto &field : location.fields)
						path += QLatin1Char('.') + (field.startsWith(QLatin1String("a:"))
						                                ? QString::number(field.mid(2).toInt() + 1)
						                                : field.mid(2));
					return path;
				}
				if (role != Qt::DisplayRole)
					return {};
				if (index.column() == 0)
				{
					if (location.fields.isEmpty())
					{
						const auto branch = m_packages.branches.value(location.package);
						return branch ? branch->label : QString{};
					}
					const auto &field = location.fields.last();
					return field.startsWith(QLatin1String("a:")) ? QString::number(field.mid(2).toInt() + 1)
					                                             : field.mid(2);
				}
				if (location.fields.isEmpty() && !m_messages.contains(location.package))
					return index.column() == 2 ? QVariant(QStringLiteral("namespace")) : QVariant{};
				const auto value = jsonValue(location);
				return index.column() == 2 ? QVariant(typeMarker(value)) : displayValue(value);
			}
			/**
			 * @brief Supplies the three column labels.
			 * @param section Header column.
			 * @param orientation Header axis.
			 * @param role Qt data role.
			 * @return Requested header label.
			 */
			QVariant headerData(int section, Qt::Orientation orientation, int role) const override
			{
				if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
					return {};
				switch (section)
				{
				case 0:
					return QStringLiteral("Package / field");
				case 1:
					return QStringLiteral("Value");
				case 2:
					return QStringLiteral("Type");
				default:
					return {};
				}
			}

		private:
			/**
			 * @brief Counts immediate JSON children.
			 * @param value Existing JSON value.
			 * @return Object/array size, or zero.
			 */
			static int count(const QJsonValue &value)
			{
				return value.isObject()  ? static_cast<int>(value.toObject().size())
				       : value.isArray() ? static_cast<int>(value.toArray().size())
				                         : 0;
			}
			/**
			 * @brief Supplies an immutable empty child list.
			 * @return Empty list.
			 */
			static const QStringList &emptyChildren()
			{
				static const QStringList empty;
				return empty;
			}
			/**
			 * @brief Borrows existing namespace children.
			 * @param package Canonical namespace.
			 * @return Published child paths.
			 */
			const QStringList &children(const QString &package) const
			{
				const auto branch = m_packages.branches.value(package);
				return branch ? branch->children : emptyChildren();
			}
			/**
			 * @brief Resolves an existing namespace row and retains only its flat Qt token.
			 * @param key Canonical package path.
			 * @param column Requested column.
			 * @return Namespace index, or invalid.
			 */
			QModelIndex packageIndex(const QString &key, int column = 0) const
			{
				if (!m_packages.branches.contains(key))
					return {};
				const auto  dot      = key.lastIndexOf(QLatin1Char('.'));
				const auto &siblings = dot < 0 ? m_packages.roots : children(key.left(dot));
				const auto  found    = std::ranges::lower_bound(siblings, key);
				if (found == siblings.cend() || *found != key)
					return {};
				auto &location = m_packageLocations[key];
				if (!location)
					location = std::make_unique<Location>(Location{
					    GmcpAddress{key, {}}
                    });
				return createIndex(static_cast<int>(found - siblings.cbegin()), column, location.get());
			}
			/**
			 * @brief Follows only the requested typed path in the bound immutable JSON.
			 * @param address Package and member/array keys.
			 * @return Existing value, or undefined.
			 */
			QJsonValue jsonValue(const GmcpAddress &address) const
			{
				if (m_hiddenPackage && *m_hiddenPackage == address.package)
					return QJsonValue::Undefined;
				const auto message = m_messages.value(address.package);
				QJsonValue value   = message ? message->state : QJsonValue(QJsonValue::Undefined);
				for (const auto &field : address.fields)
				{
					if (field.startsWith(QLatin1String("m:")) && value.isObject())
						value = value.toObject().value(field.mid(2));
					else if (field.startsWith(QLatin1String("a:")) && value.isArray())
						value = value.toArray().at(field.mid(2).toInt());
					else
						return QJsonValue::Undefined;
				}
				return value;
			}
			LuaCallbackPersistentStringMap<QMudGmcp::Message>    m_messages;
			QMudGmcp::PackageIndex                               m_packages;
			std::optional<QString>                               m_hiddenPackage;
			mutable std::map<QString, std::unique_ptr<Location>> m_packageLocations;
			mutable std::map<QString, Locations>                 m_jsonLocations;
	};

	/**
	 * @brief Runs model and selection updates without scrolling to a programmatically restored index.
	 * @tparam Base Qt item view type.
	 */
	template <typename Base> class GmcpBrowserView final : public Base
	{
		public:
			/**
			 * @brief Constructs an item view with normal user-driven scrolling.
			 * @param parent Widget owner.
			 */
			explicit GmcpBrowserView(QWidget *parent) : Base(parent)
			{
			}
			/**
			 * @brief Starts a nestable synchronous model update before Qt changes selection or scrollbar
			 * ranges.
			 */
			void beginDataUpdate()
			{
				if (m_updateDepth++ != 0)
					return;
				m_scrollPosition =
				    QPoint(this->horizontalScrollBar()->value(), this->verticalScrollBar()->value());
				m_autoScrollBeforeUpdate = this->hasAutoScroll();
				this->setAutoScroll(false);
			}
			/**
			 * @brief Completes the outermost model update using the final scrollbar ranges.
			 */
			void finishDataUpdate()
			{
				Q_ASSERT(m_updateDepth > 0);
				if (m_updateDepth == 1)
				{
					// Complete Qt's pending layout before applying coordinates to the final scrollbar ranges.
					this->executeDelayedItemsLayout();
					this->horizontalScrollBar()->setValue(m_scrollPosition.x());
					this->verticalScrollBar()->setValue(m_scrollPosition.y());
					this->setAutoScroll(m_autoScrollBeforeUpdate);
				}
				--m_updateDepth;
			}
			/**
			 * @brief Restores coordinates saved before a tree's previous binding was detached.
			 * @param position Scroll coordinates to apply when the active data update finishes.
			 */
			void setDataUpdateScrollPosition(const QPoint &position)
			{
				Q_ASSERT(m_updateDepth > 0);
				m_scrollPosition = position;
			}
			/**
			 * @brief Preserves hidden scrollbar geometry while its payload binding is detached.
			 * @param detached Whether model geometry must remain frozen until a new binding is installed.
			 */
			void setBindingDetached(bool detached)
			{
				m_bindingDetached = detached;
			}
			/**
			 * @brief Preserves both scrollbar positions while data and navigation state change.
			 * @tparam Update Update operation type.
			 * @param update Synchronous operation on this view's GUI thread.
			 */
			template <typename Update> void updateData(Update update)
			{
				beginDataUpdate();
				const auto restore = qScopeGuard([this] { finishDataUpdate(); });
				update();
			}
			/**
			 * @brief Scrolls for user navigation while ignoring data-update requests to reveal an index.
			 * @param index Index to reveal.
			 * @param hint Requested placement.
			 */
			void scrollTo(const QModelIndex &index, QAbstractItemView::ScrollHint hint) override
			{
				if (m_updateDepth == 0)
					Base::scrollTo(index, hint);
			}

		protected:
			/**
			 * @brief Updates scrollbar geometry only while a payload binding is available.
			 */
			void updateGeometries() override
			{
				if (!m_bindingDetached)
					Base::updateGeometries();
			}

		private:
			QPoint m_scrollPosition;
			int    m_updateDepth{0};
			bool   m_autoScrollBeforeUpdate{false};
			bool   m_bindingDetached{false};
	};

	/**
	 * @brief Filters full dotted paths and retains the complete subtree of each match.
	 */
	class GmcpFilterModel final : public QSortFilterProxyModel
	{
		public:
			/**
			 * @brief Configures hierarchical filtering.
			 * @param parent QObject owner.
			 */
			explicit GmcpFilterModel(QObject *parent) : QSortFilterProxyModel(parent)
			{
				setRecursiveFilteringEnabled(true);
				setAutoAcceptChildRows(true);
			}
			/**
			 * @brief Applies one literal dotted search path.
			 * @param text Package/field search text.
			 */
			void setText(const QString &text)
			{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
				beginFilterChange();
				m_text = text;
				endFilterChange(Direction::Rows);
#else
				m_text = text;
				invalidateFilter();
#endif
			}
			/**
			 * @brief Checks whether filtering can remove ancestor and descendant package rows.
			 * @return Whether a nonempty search is active.
			 */
			[[nodiscard]] bool hasFilter() const noexcept
			{
				return !m_text.isEmpty();
			}

		protected:
			/**
			 * @brief Tests a row's complete path without splitting dots into terms.
			 * @param row Source row.
			 * @param parent Source parent.
			 * @return Whether the row itself matches.
			 */
			[[nodiscard]] bool filterAcceptsRow(int row, const QModelIndex &parent) const override
			{
				if (m_text.isEmpty())
					return true;
				const auto index = sourceModel()->index(row, 0, parent);
				if (index.data(PathRole).toString().contains(m_text, Qt::CaseInsensitive))
					return true;
				return !m_text.contains(QLatin1Char('.')) && sourceModel()
				                                                 ->index(row, 1, parent)
				                                                 .data()
				                                                 .toString()
				                                                 .contains(m_text, Qt::CaseInsensitive);
			}

		private:
			QString m_text;
	};

	/**
	 * @brief Direct newest-first view of the runtime's existing history records.
	 */
	class GmcpHistoryModel final : public QAbstractListModel
	{
		public:
			/**
			 * @brief Observes append/removal boundaries without copying history into widget items.
			 * @param runtime History owner, or null for layout preview.
			 * @param parent QObject owner.
			 */
			GmcpHistoryModel(WorldRuntime *runtime, QObject *parent)
			    : QAbstractListModel(parent), m_runtime(runtime)
			{
				if (!runtime)
					return;
				connect(runtime, &WorldRuntime::gmcpHistoryAboutToAppend, this,
				        [this]
				        {
					        if (!m_paused)
						        beginInsertRows({}, 0, 0);
				        });
				connect(runtime, &WorldRuntime::gmcpHistoryAppended, this,
				        [this]
				        {
					        if (!m_paused)
						        endInsertRows();
				        });
				connect(runtime, &WorldRuntime::gmcpHistoryAboutToRemove, this,
				        [this](int first, int last)
				        {
					        if (!m_paused)
						        beginRemoveRows({}, rowCount({}) - 1 - last, rowCount({}) - 1 - first);
				        });
				connect(runtime, &WorldRuntime::gmcpHistoryRemoved, this,
				        [this]
				        {
					        if (!m_paused)
						        endRemoveRows();
				        });
			}
			/**
			 * @brief Freezes or resumes history inspection by retaining its existing page index.
			 * @param paused Whether incoming history changes should remain hidden.
			 */
			void setPaused(bool paused)
			{
				if (paused == m_paused)
					return;
				if (paused)
				{
					if (m_runtime)
						m_frozen = m_runtime->gmcpHistory();
					m_paused = true;
				}
				else
				{
					beginResetModel();
					m_paused = false;
					m_frozen.clear();
					endResetModel();
				}
			}
			/**
			 * @brief Counts retained versions.
			 * @param parent Parent index.
			 * @return Number of timestamps at the root.
			 */
			[[nodiscard]] int rowCount(const QModelIndex &parent) const override
			{
				return parent.isValid() ? 0 : static_cast<int>(records().size());
			}
			/**
			 * @brief Reads one existing version without copying it.
			 * @param row Newest-first row.
			 * @return Record pointer, valid until the next history mutation, or null.
			 */
			[[nodiscard]] const QMudGmcp::Record *record(int row) const
			{
				const auto &history = records();
				return row >= 0 && row < history.size() ? &history.at(history.size() - 1 - row) : nullptr;
			}
			/**
			 * @brief Finds a pinned record using ordered connection/sequence identities, independently of timestamps.
			 * @param wanted Stable record identity.
			 * @return Newest-first row, or -1 when the record has been evicted.
			 */
			[[nodiscard]] int rowForIdentity(const std::pair<quint64, quint64> &wanted) const
			{
				const auto &history = records();
				qsizetype   first   = 0;
				qsizetype   last    = history.size();
				while (first < last)
				{
					const auto middle = first + (last - first) / 2;
					if (identity(history.at(middle)) < wanted)
						first = middle + 1;
					else
						last = middle;
				}
				return first < history.size() && identity(history.at(first)) == wanted
				           ? static_cast<int>(history.size() - 1 - first)
				           : -1;
			}
			/**
			 * @brief Reads timestamp, identity, or diagnostic from an existing record.
			 * @param index Requested row.
			 * @param role Qt data role.
			 * @return Requested history attribute.
			 */
			[[nodiscard]] QVariant data(const QModelIndex &index, int role) const override
			{
				const auto *entry = index.isValid() ? record(index.row()) : nullptr;
				if (!entry)
					return {};
				if (role == Qt::DisplayRole)
					return entry->time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
				if (role == Qt::UserRole)
					return QString::number(entry->generation) + QLatin1Char(':') +
					       QString::number(entry->sequence);
				if (role == Qt::ToolTipRole)
					return (entry->outgoing ? QStringLiteral("Sent") : QStringLiteral("Received")) +
					       QStringLiteral(" · %1").arg(entry->sequence) +
					       (entry->diagnostic.isEmpty() ? QString{} : QLatin1Char('\n') + entry->diagnostic);
				return {};
			}

		private:
			/**
			 * @brief Borrows the live history or its shared paused view.
			 * @return Oldest-first existing records.
			 */
			[[nodiscard]] const QMudGmcp::History &records() const
			{
				if (m_paused)
					return m_frozen;
				if (m_runtime)
					return m_runtime->gmcpHistory();
				static const QMudGmcp::History empty;
				return empty;
			}
			QPointer<WorldRuntime> m_runtime;
			QMudGmcp::History      m_frozen;
			bool                   m_paused{false};
	};
} // namespace

/**
 * @brief Direct package view retaining only explicit user navigation choices across replacements.
 */
class GmcpBrowserDataView final : public QWidget
{

	public:
		/**
		 * @brief Builds the tree/raw pane over existing JSON objects.
		 * @param name Object-name prefix.
		 * @param parent Widget owner.
		 */
		explicit GmcpBrowserDataView(const QString &name, QWidget *parent) : QWidget(parent)
		{
			auto *layout = new QVBoxLayout(this);
			layout->setContentsMargins(0, 0, 0, 0);
			m_stack = new QStackedWidget(this);
			m_tree  = new GmcpBrowserView<QTreeView>(m_stack);
			m_tree->setObjectName(name + QStringLiteral("Tree"));
			m_model  = new GmcpTreeModel(this);
			m_filter = new GmcpFilterModel(this);
			m_filter->setSourceModel(m_model);
			m_tree->setModel(m_filter);
			m_tree->setUniformRowHeights(true);
			m_tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
			m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
			m_tree->header()->setStretchLastSection(false);
			m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
			m_tree->header()->resizeSection(0, 300);
			m_tree->header()->resizeSection(2, 100);
			m_stack->addWidget(m_tree);
			m_raw = new QPlainTextEdit(m_stack);
			m_raw->setObjectName(name + QStringLiteral("Raw"));
			m_raw->setReadOnly(true);
			m_raw->setUndoRedoEnabled(false);
			m_stack->addWidget(m_raw);
			m_rawWatcher = new QFutureWatcher<GmcpRawBatch>(this);
			connect(m_rawWatcher, &QFutureWatcher<GmcpRawBatch>::finished, this,
			        [this]
			        {
				        auto batch    = m_rawWatcher->future().takeResult();
				        m_rawInFlight = false;
				        if (batch.version == m_rawVersion)
				        {
					        if (batch.cancelled || m_rawCancellation->load(std::memory_order_relaxed) ||
					            !m_toggle->isChecked() || !m_raw->isVisible())
					        {
						        m_rawNeedsFull |= batch.full;
						        m_rawPending.merge(batch.keys);
					        }
					        else
						        applyRawBatch(batch);
				        }
				        refreshRaw();
			        });
			layout->addWidget(m_stack, 1);
			auto *bottom = new QHBoxLayout;
			bottom->addStretch(1);
			m_toggle = new QCheckBox(QStringLiteral("Raw view"), this);
			m_toggle->setObjectName(name + QStringLiteral("RawToggle"));
			bottom->addWidget(m_toggle);
			layout->addLayout(bottom);
			connect(m_toggle, &QCheckBox::toggled, this,
			        [this](bool raw)
			        {
				        if (raw)
					        detachTree();
				        else
					        cancelRawFormatting();
				        m_stack->setCurrentWidget(raw ? static_cast<QWidget *>(m_raw) : m_tree);
				        displayPending();
			        });
			connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
			        [this](const QModelIndex &index)
			        {
				        if (!m_restoring && index.isValid())
					        m_selected = GmcpTreeModel::address(m_filter->mapToSource(index));
			        });
			connect(m_tree, &QTreeView::expanded, this,
			        [this](const QModelIndex &index) { rememberExpansion(index, true); });
			connect(m_tree, &QTreeView::collapsed, this,
			        [this](const QModelIndex &index) { rememberExpansion(index, false); });
		}
		/**
		 * @brief Cancels formatting and transfers retained immutable data to background cleanup.
		 */
		~GmcpBrowserDataView() override
		{
			cancelRawFormatting();
			retireBrowserData(std::exchange(m_messages, {}), std::exchange(m_packages, {}),
			                  std::exchange(m_wire, {}));
		}
		/**
		 * @brief Retains an existing version for initial display or explicit version switching.
		 * @param messages Existing message objects.
		 * @param packages Existing namespace index.
		 * @param wire Optional historical wire message.
		 */
		void setMessages(const LuaCallbackPersistentStringMap<QMudGmcp::Message> &messages,
		                 const QMudGmcp::PackageIndex &packages, const std::optional<QByteArray> &wire = {})
		{
			auto previousMessages = std::exchange(m_messages, messages);
			auto previousPackages = std::exchange(m_packages, packages);
			auto previousWire     = std::exchange(m_wire, wire);
			m_treeStale           = true;
			cancelRawFormatting();
			++m_rawVersion;
			m_rawNeedsFull = true;
			m_rawPending.clear();
			displayPending();
			retireBrowserData(std::move(previousMessages), std::move(previousPackages),
			                  std::move(previousWire));
		}
		/**
		 * @brief Applies only the named received package when its tree is displayed.
		 * @param record Exact immutable version.
		 * @param message Received package.
		 */
		void receive(const QMudGmcp::Record &record, const QMudGmcp::Message &message)
		{
			m_messages = record.messages;
			m_packages = record.packages;
			if (!m_rawNeedsFull)
				m_rawPending.insert(message.name.toLower());
			if (isVisible() && m_stack->currentWidget() == m_tree && !m_treeStale)
			{
				const QScopedValueRollback<bool> restoring(m_restoring, true);
				m_tree->updateData(
				    [&]
				    {
					    m_model->replacePackage(record, message);
					    restoreNavigation(message.name.toLower());
				    });
			}
			else
				m_treeStale = true;
			refreshRaw();
		}
		/**
		 * @brief Applies dotted filtering while retaining semantic selection and expansion choices.
		 * @param text Literal package/field search text.
		 */
		void setFilter(const QString &text)
		{
			const QScopedValueRollback<bool> restoring(m_restoring, true);
			m_tree->updateData(
			    [&]
			    {
				    m_filter->setText(text);
				    if (m_treeBound)
					    restoreNavigation();
			    });
		}

	protected:
		/**
		 * @brief Releases the hidden tree binding and stops Raw preparation while retaining navigation choices.
		 * @param event Hide notification.
		 */
		void hideEvent(QHideEvent *event) override
		{
			cancelRawFormatting();
			detachTree();
			QWidget::hideEvent(event);
		}
		/**
		 * @brief Binds deferred data only when its pane becomes displayed.
		 * @param event Show notification.
		 */
		void showEvent(QShowEvent *event) override
		{
			QWidget::showEvent(event);
			displayPending();
		}

	private:
		/**
		 * @brief Detaches a hidden tree while retaining only semantic navigation and scroll coordinates.
		 */
		void detachTree()
		{
			if (!m_treeBound)
				return;
			m_treeScroll =
			    QPoint(m_tree->horizontalScrollBar()->value(), m_tree->verticalScrollBar()->value());
			const QScopedValueRollback<bool> restoring(m_restoring, true);
			m_tree->setBindingDetached(true);
			m_tree->updateData([&] { m_model->setVersion({}, {}); });
			m_treeBound = false;
			m_treeStale = true;
		}
		/**
		 * @brief Records an explicit expansion/collapse choice without retaining payload nodes.
		 * @param index Proxy index changed by the user.
		 * @param expanded New expansion state.
		 */
		void rememberExpansion(const QModelIndex &index, bool expanded)
		{
			if (m_restoring || !index.isValid())
				return;
			const auto address = GmcpTreeModel::address(m_filter->mapToSource(index));
			m_expansion[address.package][address.fields] = expanded;
		}
		/**
		 * @brief Binds a pending version or renders raw data only when displayed.
		 */
		void displayPending()
		{
			if (!isVisible())
				return;
			if (m_stack->currentWidget() == m_tree && m_treeStale)
			{
				const QScopedValueRollback<bool> restoring(m_restoring, true);
				m_tree->updateData(
				    [&]
				    {
					    m_tree->setBindingDetached(false);
					    m_model->setVersion(m_messages, m_packages);
					    m_treeStale = false;
					    m_treeBound = true;
					    restoreNavigation();
					    if (m_treeScroll)
					    {
						    m_tree->setDataUpdateScrollPosition(*m_treeScroll);
						    m_treeScroll.reset();
					    }
				    });
			}
			refreshRaw();
		}
		/**
		 * @brief Finds an explicit saved expansion choice.
		 * @param address Semantic node location.
		 * @return Saved expanded/collapsed choice, or no explicit choice.
		 */
		[[nodiscard]] std::optional<bool> savedExpansion(const GmcpAddress &address) const
		{
			const auto package = m_expansion.find(address.package);
			if (package == m_expansion.end())
				return {};
			const auto field = package->second.find(address.fields);
			return field == package->second.end() ? std::optional<bool>{} : field->second;
		}
		/**
		 * @brief Restores saved paths and affected ancestor chains from root to leaf, falling back through
		 * missing data.
		 * @param package Optional changed package; version switches restore all saved choices.
		 */
		void restoreNavigation(const std::optional<QString> &package = {})
		{
			std::map<QString, std::map<QStringList, bool>> pending;
			auto queueChoice = [&](const GmcpAddress &address, bool expanded)
			{
				auto [choice, inserted] = pending[address.package].try_emplace(address.fields, expanded);
				if (const auto exact = savedExpansion(address))
					choice->second = *exact;
				else if (!inserted)
					choice->second = choice->second || expanded;
			};
			auto queueAncestors = [&](GmcpAddress address, bool implicit)
			{
				do
				{
					if (const auto exact = savedExpansion(address))
						queueChoice(address, *exact);
					else if (implicit)
						queueChoice(address, true);
				} while (parentAddress(address));
			};
			auto queueSaved = [&](const auto &entry)
			{
				for (const auto &[fields, expanded] : entry.second)
				{
					const auto source = m_model->indexFor({entry.first, fields}, true);
					if (!source.isValid())
						continue;
					auto address = GmcpTreeModel::address(source);
					queueChoice(address, expanded);
					if (parentAddress(address))
						queueAncestors(address, true);
				}
			};
			if (package)
			{
				if (const auto source = m_model->indexFor({*package, {}}, true); source.isValid())
					queueAncestors(GmcpTreeModel::address(source), false);
				const auto found = m_expansion.find(*package);
				if (found != m_expansion.end())
					queueSaved(*found);
				if (m_filter->hasFilter())
				{
					// A parent's temporary loss of its own filter match can hide every child package too.
					const auto prefix = *package + QLatin1Char('.');
					for (auto child = m_expansion.lower_bound(prefix);
					     child != m_expansion.end() && child->first.startsWith(prefix); ++child)
						queueSaved(*child);
				}
			}
			else
				for (const auto &entry : m_expansion)
					queueSaved(entry);
			const bool selectionAffected =
			    m_selected && (!package || m_selected->package == *package ||
			                   package->startsWith(m_selected->package + QLatin1Char('.')) ||
			                   m_selected->package.startsWith(*package + QLatin1Char('.')));
			QModelIndex selected;
			if (selectionAffected)
			{
				const auto source = m_model->indexFor(*m_selected, true);
				if (source.isValid())
				{
					m_selected = GmcpTreeModel::address(source);
					selected   = m_filter->mapFromSource(source);
					if (const auto exact = savedExpansion(*m_selected))
						queueChoice(*m_selected, *exact);
					auto ancestor = *m_selected;
					if (parentAddress(ancestor))
						queueAncestors(ancestor, true);
				}
				else
					m_selected.reset();
			}
			// Package-name prefixes and typed-path prefixes sort before their descendants.
			for (const auto &[name, choices] : pending)
				for (const auto &[fields, expanded] : choices)
					if (const auto index = m_filter->mapFromSource(m_model->indexFor({name, fields}));
					    index.isValid())
						m_tree->setExpanded(index, expanded);
			if (selectionAffected)
			{
				m_tree->setCurrentIndex(selected);
				if (m_selected)
					restoreCollapsedParents(*m_selected);
			}
		}
		/**
		 * @brief Preserves explicitly collapsed ancestors when Qt restores a descendant selection.
		 * @param address Saved semantic location.
		 */
		void restoreCollapsedParents(const GmcpAddress &address) const
		{
			auto ancestor = address;
			do
			{
				if (const auto expanded = savedExpansion(ancestor); expanded && !*expanded)
					if (const auto index = m_filter->mapFromSource(m_model->indexFor(ancestor));
					    index.isValid())
						m_tree->setExpanded(index, false);
			} while (parentAddress(ancestor));
		}
		/**
		 * @brief Requests cancellation without accessing a background task's immutable inputs.
		 */
		void cancelRawFormatting() const
		{
			if (m_rawCancellation)
				m_rawCancellation->store(true, std::memory_order_relaxed);
		}
		/**
		 * @brief Restores cursor offsets and both viewport coordinates after an edit.
		 * @param edit Synchronous document edit on the GUI thread.
		 */
		template <typename Edit> void editRawDocument(Edit edit)
		{
			const int horizontal = m_raw->horizontalScrollBar()->value();
			const int vertical   = m_raw->verticalScrollBar()->value();
			const int anchor     = m_raw->textCursor().anchor();
			const int position   = m_raw->textCursor().position();
			edit();
			auto      cursor = m_raw->textCursor();
			const int last   = m_raw->document()->characterCount() - 1;
			cursor.setPosition(qMin(anchor, last));
			cursor.setPosition(qMin(position, last), QTextCursor::KeepAnchor);
			m_raw->setTextCursor(cursor);
			m_raw->horizontalScrollBar()->setValue(horizontal);
			m_raw->verticalScrollBar()->setValue(vertical);
		}
		using RawRanges = std::map<QString, GmcpRawRange>;
		/**
		 * @brief Locates the package's last character boundary, excluding its separator.
		 * @param entry Package range.
		 * @return Document offset immediately after its JSON.
		 */
		[[nodiscard]] int rawPackageEnd(RawRanges::const_iterator entry) const
		{
			return entry->second.last.position() + entry->second.last.length() - 1 -
			       (std::next(entry) != m_rawRanges.end() ? 1 : 0);
		}
		/**
		 * @brief Removes one renamed package and its adjacent separator.
		 * @param name Previous display name.
		 */
		void removeRawPackage(const QString &name)
		{
			const auto found = m_rawRanges.find(name);
			Q_ASSERT(found != m_rawRanges.end());
			QTextCursor cursor(m_raw->document());
			const int   start = found->second.first.position();
			const auto  next  = std::next(found);
			if (next != m_rawRanges.end())
			{
				const bool single = next->second.first == next->second.last;
				cursor.setPosition(start);
				cursor.setPosition(next->second.first.position(), QTextCursor::KeepAnchor);
				cursor.removeSelectedText();
				next->second.first = m_raw->document()->findBlock(start);
				if (single)
					next->second.last = next->second.first;
			}
			else
			{
				const int end = rawPackageEnd(found);
				cursor.setPosition(found == m_rawRanges.begin() ? start : rawPackageEnd(std::prev(found)));
				cursor.setPosition(end + (found == m_rawRanges.begin() ? 1 : 0), QTextCursor::KeepAnchor);
				cursor.removeSelectedText();
			}
			m_rawRanges.erase(found);
		}
		/**
		 * @brief Replaces or inserts one prepared package while retaining unrelated document blocks.
		 * @param part Prepared package text.
		 */
		void applyRawPackage(const GmcpRawPart &part)
		{
			if (const auto old = m_rawNames.find(part.key);
			    old != m_rawNames.end() && old->second != part.name)
				removeRawPackage(old->second);
			const auto  found = m_rawRanges.find(part.name);
			QTextCursor cursor(m_raw->document());
			int         start = 2;
			if (found != m_rawRanges.end())
			{
				start = found->second.first.position();
				cursor.setPosition(start);
				cursor.setPosition(rawPackageEnd(found), QTextCursor::KeepAnchor);
				cursor.insertText(part.text);
			}
			else
			{
				const auto next = m_rawRanges.lower_bound(part.name);
				if (next != m_rawRanges.begin())
				{
					const int previousEnd = rawPackageEnd(std::prev(next));
					start                 = previousEnd + 2;
					cursor.setPosition(previousEnd);
					cursor.insertText(QStringLiteral(",\n") + part.text);
				}
				else
				{
					cursor.setPosition(1);
					cursor.insertText(QLatin1Char('\n') + part.text +
					                  (next == m_rawRanges.end() ? QString{} : QStringLiteral(",")));
				}
			}
			m_rawRanges[part.name] = {
			    m_raw->document()->findBlock(start),
			    m_raw->document()->findBlock(start + static_cast<int>(part.text.size()) - 1)};
			m_rawNames[part.key] = part.name;
		}
		/**
		 * @brief Applies prepared text with package-local edits after the initial binding.
		 * @param batch Completed formatting result.
		 */
		void applyRawBatch(const GmcpRawBatch &batch)
		{
			editRawDocument(
			    [&]
			    {
				    if (batch.full)
				    {
					    m_rawRanges.clear();
					    m_rawNames.clear();
					    m_raw->setPlainText(batch.document);
					    for (const auto &part : batch.parts)
					    {
						    m_rawRanges.emplace(
						        part.name,
						        GmcpRawRange{m_raw->document()->findBlock(part.start),
						                     m_raw->document()->findBlock(
						                         part.start + static_cast<int>(part.text.size()) - 1)});
						    m_rawNames.emplace(part.key, part.name);
					    }
				    }
				    else
				    {
					    QTextCursor edit(m_raw->document());
					    edit.beginEditBlock();
					    for (const auto &part : batch.parts)
						    applyRawPackage(part);
					    edit.endEditBlock();
				    }
			    });
		}
		/**
		 * @brief Starts at most one formatting task for a displayed Raw view; hidden updates retain only
		 * package names.
		 */
		void refreshRaw()
		{
			if (!m_toggle->isChecked() || !m_raw->isVisible())
				return;
			if (m_wire)
			{
				if (m_rawNeedsFull)
				{
					editRawDocument([&] { m_raw->setPlainText(QString::fromUtf8(*m_wire)); });
					m_rawNeedsFull = false;
				}
				return;
			}
			if (m_rawInFlight || (!m_rawNeedsFull && m_rawPending.empty()))
				return;
			m_rawInFlight   = true;
			const bool full = std::exchange(m_rawNeedsFull, false);
			auto       keys = std::move(m_rawPending);
			m_rawPending.clear();
			m_rawCancellation = std::make_shared<std::atomic_bool>(false);
			auto promise      = std::make_shared<QPromise<GmcpRawBatch>>();
			m_rawWatcher->setFuture(promise->future());
			QThreadPool::globalInstance()->start(
			    [messages = m_messages, keys = std::move(keys), full, version = m_rawVersion,
			     cancelled = m_rawCancellation, promise]() mutable
			    {
				    promise->start();
				    promise->addResult(
				        formatRawPackages(messages, std::move(keys), full, version, cancelled));
				    promise->finish();
			    });
		}

		GmcpBrowserView<QTreeView>                       *m_tree{nullptr};
		GmcpTreeModel                                    *m_model{nullptr};
		GmcpFilterModel                                  *m_filter{nullptr};
		QPlainTextEdit                                   *m_raw{nullptr};
		QStackedWidget                                   *m_stack{nullptr};
		QCheckBox                                        *m_toggle{nullptr};
		LuaCallbackPersistentStringMap<QMudGmcp::Message> m_messages;
		QMudGmcp::PackageIndex                            m_packages;
		std::optional<QByteArray>                         m_wire;
		std::optional<GmcpAddress>                        m_selected;
		std::map<QString, std::map<QStringList, bool>>    m_expansion;
		bool                                              m_treeStale{true};
		bool                                              m_treeBound{false};
		std::optional<QPoint>                             m_treeScroll;
		QFutureWatcher<GmcpRawBatch>                     *m_rawWatcher{nullptr};
		std::shared_ptr<std::atomic_bool>                 m_rawCancellation;
		RawRanges                                         m_rawRanges;
		std::map<QString, QString>                        m_rawNames;
		std::set<QString>                                 m_rawPending;
		quint64                                           m_rawVersion{0};
		bool                                              m_rawNeedsFull{true};
		bool                                              m_rawInFlight{false};
		bool                                              m_restoring{false};
};

void GmcpBrowserDialog::open(WorldRuntime *runtime, QWidget *parent)
{
	static QHash<WorldRuntime *, QPointer<GmcpBrowserDialog>> windows;
#ifdef NDEBUG
	if (!runtime)
		return;
#endif
	auto &browser = windows[runtime];
	if (!browser)
	{
		browser = new GmcpBrowserDialog(runtime, parent);
		QObject::connect(browser, &QObject::destroyed,
		                 [runtime]
		                 {
			                 if (auto it = windows.find(runtime); it != windows.end() && it.value().isNull())
				                 windows.erase(it);
		                 });
		if (runtime)
			QObject::connect(runtime, &QObject::destroyed, browser, [runtime] { windows.remove(runtime); });
	}
	browser->show();
	browser->raise();
	browser->activateWindow();
}

GmcpBrowserDialog::GmcpBrowserDialog(WorldRuntime *runtime, QWidget *parent)
    : QWidget(parent, Qt::Window), m_runtime(runtime)
{
	setAttribute(Qt::WA_DeleteOnClose);
	setObjectName(QStringLiteral("gmcpBrowser"));
	setWindowTitle(runtime ? QStringLiteral("GMCP Browser — %1")
	                             .arg(runtime->worldAttributes().value(QStringLiteral("name")))
	                       : QStringLiteral("GMCP Browser"));
	auto *layout = new QVBoxLayout(this);
	m_status     = new QLabel(this);
	layout->addWidget(m_status);
	auto *controls = new QHBoxLayout;
	m_filter       = new QLineEdit(this);
	m_filter->setObjectName(QStringLiteral("gmcpFilter"));
	m_filter->setPlaceholderText(QStringLiteral("Filter packages or fields"));
	m_pause = new QCheckBox(QStringLiteral("Pause"), this);
	m_pause->setObjectName(QStringLiteral("gmcpPause"));
	m_pause->setChecked(true);
	controls->addWidget(m_filter, 1);
	controls->addWidget(m_pause);
	layout->addLayout(controls);
	auto *tabs = new QTabWidget(this);
	tabs->setObjectName(QStringLiteral("gmcpTabs"));
	m_current = new GmcpBrowserDataView(QStringLiteral("gmcpCurrent"), tabs);
	tabs->addTab(m_current, QStringLiteral("Current"));
	auto *history = new QSplitter(tabs);
	m_history     = new GmcpBrowserView<QTableView>(history);
	m_history->horizontalHeader()->hide();
	m_history->verticalHeader()->hide();
	m_history->setShowGrid(false);
	m_history->setWordWrap(false);
	m_history->setSortingEnabled(false);
	m_history->setSelectionMode(QAbstractItemView::SingleSelection);
	m_history->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_history->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_history->setTabKeyNavigation(false);
	// Keep Qt's arithmetic row geometry: per-row resizing, moving, hiding, or stretching
	// would allocate section state and make history updates depend on the retained row count.
	m_history->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	m_history->verticalHeader()->setSectionsMovable(false);
	m_history->verticalHeader()->setStretchLastSection(false);
	m_history->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	auto *historyModel = new GmcpHistoryModel(runtime, m_history);
	// Capture selection before the view and its selection model handle removal.
	connect(historyModel, &QAbstractItemModel::rowsAboutToBeRemoved, this,
	        [this](const QModelIndex &, int first, int last)
	        {
		        dynamic_cast<GmcpBrowserView<QTableView> *>(m_history)->beginDataUpdate();
		        const int row             = m_history->currentIndex().row();
		        m_historySelectionEvicted = row >= first && row <= last;
	        });
	m_history->setModel(historyModel);
	m_history->setObjectName(QStringLiteral("gmcpHistoryVersions"));
	m_history->setMinimumWidth(210);
	m_past = new GmcpBrowserDataView(QStringLiteral("gmcpHistory"), history);
	history->setStretchFactor(0, 0);
	history->setStretchFactor(1, 1);
	history->setSizes({230, 700});
	tabs->addTab(history, QStringLiteral("History"));
	layout->addWidget(tabs, 1);
	resize(1000, 650);
	if (runtime)
	{
		connect(runtime, &WorldRuntime::gmcpChanged, this, &GmcpBrowserDialog::scheduleRefresh);
		connect(runtime, &WorldRuntime::nativeGmcpAvailabilityChanged, this,
		        &GmcpBrowserDialog::scheduleRefresh);
		connect(
		    runtime, &WorldRuntime::gmcpPackageReceived, this,
		    [this](const QMudGmcp::Record &record, const std::shared_ptr<const QMudGmcp::Message> &message)
		    {
			    if (m_pause->isChecked())
				    return;
			    const auto publication = m_runtime->gmcpPublication();
			    const auto snapshot =
			        publication ? publication->current.load(std::memory_order_acquire) : nullptr;
			    if (!snapshot || snapshot->session != m_currentSession ||
			        message->session != m_currentSession)
			    {
				    scheduleRefresh();
				    return;
			    }
			    if (record.revision <= m_currentRevision)
				    return;
			    m_current->receive(record, *message);
			    m_currentRevision = record.revision;
		    });
		connect(runtime, &QObject::destroyed, this, [this] { close(); });
	}
	connect(m_filter, &QLineEdit::textChanged, this,
	        [this](const QString &filter)
	        {
		        m_current->setFilter(filter);
		        m_past->setFilter(filter);
	        });
	connect(m_pause, &QCheckBox::toggled, this,
	        [this](bool paused)
	        {
		        dynamic_cast<GmcpBrowserView<QTableView> *>(m_history)->updateData(
		            [&]
		            {
			            auto      *model    = dynamic_cast<GmcpHistoryModel *>(m_history->model());
			            const auto selected = m_historySelection;
			            {
				            const QScopedValueRollback<bool> changing(m_historySelectionChanging, true);
				            model->setPaused(paused);
			            }
			            if (!paused)
			            {
				            const int row = selected ? model->rowForIdentity(*selected) : -1;
				            m_history->setCurrentIndex(model->index(row >= 0 ? row : 0));
				            showHistory();
				            bindCurrent();
				            refresh();
			            }
		            });
	        });
	connect(m_history->selectionModel(), &QItemSelectionModel::currentChanged, this,
	        [this] { showHistory(); });
	connect(m_history->model(), &QAbstractItemModel::rowsRemoved, this,
	        [this]
	        {
		        const bool evicted        = m_historySelectionEvicted;
		        m_historySelectionEvicted = false;
		        if (evicted)
		        {
			        dynamic_cast<GmcpBrowserView<QTableView> *>(m_history)->updateData(
			            [&]
			            {
				            m_history->setCurrentIndex(m_history->model()->index(0, 0));
				            showHistory();
			            });
		        }
		        dynamic_cast<GmcpBrowserView<QTableView> *>(m_history)->finishDataUpdate();
	        });
	connect(tabs, &QTabWidget::currentChanged, this,
	        [this](int index)
	        {
		        if (index == 1 && !m_historyOpened)
		        {
			        m_historyOpened = true;
			        dynamic_cast<GmcpBrowserView<QTableView> *>(m_history)->updateData(
			            [&]
			            {
				            if (m_history->model()->rowCount() > 0)
					            m_history->setCurrentIndex(m_history->model()->index(0, 0));
				            showHistory();
			            });
		        }
	        });
	historyModel->setPaused(true);
	bindCurrent();
	m_history->setCurrentIndex(historyModel->index(0));
}

void GmcpBrowserDialog::scheduleRefresh()
{
	if (m_refreshQueued || m_pause->isChecked())
		return;
	m_refreshQueued = true;
	QMetaObject::invokeMethod(
	    this,
	    [this]
	    {
		    m_refreshQueued = false;
		    refresh();
	    },
	    Qt::QueuedConnection);
}

void GmcpBrowserDialog::refresh()
{
	if (m_pause->isChecked())
		return;
	const auto publication = m_runtime ? m_runtime->gmcpPublication() : nullptr;
	const auto snapshot    = publication ? publication->current.load(std::memory_order_acquire) : nullptr;
	m_status->setText(QStringLiteral("Native GMCP: %1")
	                      .arg(!snapshot || !snapshot->nativeAvailable() ? QStringLiteral("unavailable")
	                           : snapshot->negotiated                    ? QStringLiteral("negotiated")
	                                                                     : QStringLiteral("negotiating")));
	if (snapshot && snapshot->session != m_currentSession)
		bindCurrent();
	if (m_historyOpened && !m_history->currentIndex().isValid())
		dynamic_cast<GmcpBrowserView<QTableView> *>(m_history)->updateData(
		    [&] { m_history->setCurrentIndex(m_history->model()->index(0, 0)); });
	showHistory();
}

void GmcpBrowserDialog::bindCurrent()
{
	const auto publication = m_runtime ? m_runtime->gmcpPublication() : nullptr;
	const auto snapshot    = publication ? publication->current.load(std::memory_order_acquire) : nullptr;
	m_currentSession       = snapshot ? snapshot->session : 0;
	m_currentRevision      = snapshot ? snapshot->revision : 0;
	m_status->setText(QStringLiteral("Native GMCP: %1")
	                      .arg(!snapshot || !snapshot->nativeAvailable() ? QStringLiteral("unavailable")
	                           : snapshot->negotiated                    ? QStringLiteral("negotiated")
	                                                                     : QStringLiteral("negotiating")));
	m_current->setMessages(snapshot ? snapshot->messages
	                                : LuaCallbackPersistentStringMap<QMudGmcp::Message>{},
	                       snapshot ? snapshot->packages : QMudGmcp::PackageIndex{});
}

void GmcpBrowserDialog::showHistory()
{
	if (m_historySelectionChanging || m_historySelectionEvicted)
		return;
	const auto *model  = dynamic_cast<GmcpHistoryModel *>(m_history->model());
	const auto *record = model->record(m_history->currentIndex().row());
	const auto  key    = record ? std::optional{identity(*record)} : std::nullopt;
	m_historySelection = key;
	if (!m_historyOpened || key == m_historyIdentity)
		return;
	m_historyIdentity = key;
	if (record)
		m_past->setMessages(record->messages, record->packages, record->raw);
	else
		m_past->setMessages({}, {});
}
