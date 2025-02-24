////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "TransactionState.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/QueryCache.h"
#include "Aql/QueryContext.h"
#include "Basics/DebugRaceController.h"
#include "Basics/Exceptions.h"
#include "Basics/StringUtils.h"
#include "Basics/overload.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Metrics/Counter.h"
#include "Metrics/MetricsFeature.h"
#include "Statistics/ServerStatistics.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "StorageEngine/TransactionCollection.h"
#include "Transaction/Context.h"
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
#include "Transaction/History.h"
#endif
#include "Transaction/Methods.h"
#include "Transaction/Options.h"
#include "Utils/ExecContext.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"

#include <absl/strings/str_cat.h>
#include <algorithm>
#include <any>

using namespace arangodb;

/// @brief transaction type
TransactionState::TransactionState(TRI_vocbase_t& vocbase, TransactionId tid,
                                   transaction::Options const& options,
                                   transaction::OperationOrigin operationOrigin)
    : _vocbase(vocbase),
      _serverRole(ServerState::instance()->getRole()),
      _options(options),
      _id(tid),
      _operationOrigin(operationOrigin) {
  // patch intermediateCommitCount for testing
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  transaction::Options::adjustIntermediateCommitCount(_options);
#endif
}

/// @brief free a transaction container
TransactionState::~TransactionState() {
  TRI_ASSERT(_status != transaction::Status::RUNNING);

  // process collections in reverse order, free all collections
  RECURSIVE_READ_LOCKER(_collectionsLock, _collectionsLockOwner);
  for (auto it = _collections.rbegin(); it != _collections.rend(); ++it) {
    (*it)->releaseUsage();
    delete (*it);
  }
}

/// @brief return the collection from a transaction
TransactionCollection* TransactionState::collection(
    DataSourceId cid, AccessMode::Type accessType) const {
  TRI_ASSERT(_status == transaction::Status::CREATED ||
             _status == transaction::Status::RUNNING);

  auto collectionOrPos = findCollectionOrPos(cid);
  return std::visit(
      overload{
          [](CollectionNotFound const&) -> TransactionCollection* {
            return nullptr;
          },
          [&](CollectionFound const& colFound) -> TransactionCollection* {
            auto* const col = colFound.collection;
            return col->canAccess(accessType) ? col : nullptr;
          },
      },
      collectionOrPos);
}

/// @brief return the collection from a transaction
TransactionCollection* TransactionState::collection(
    std::string_view name, AccessMode::Type accessType) const {
  TRI_ASSERT(_status == transaction::Status::CREATED ||
             _status == transaction::Status::RUNNING);

  RECURSIVE_READ_LOCKER(_collectionsLock, _collectionsLockOwner);
  auto it = std::find_if(_collections.begin(), _collections.end(),
                         [&name](TransactionCollection const* trxColl) {
                           return trxColl->collectionName() == name;
                         });

  if (it == _collections.end() || !(*it)->canAccess(accessType)) {
    // not found or not accessible in the requested mode
    return nullptr;
  }

  return (*it);
}

TransactionState::Cookie* TransactionState::cookie(
    void const* key) const noexcept {
  auto itr = _cookies.find(key);

  return itr == _cookies.end() ? nullptr : itr->second.get();
}

TransactionState::Cookie::ptr TransactionState::cookie(
    void const* key, TransactionState::Cookie::ptr&& cookie) {
  _cookies[key].swap(cookie);

  return std::move(cookie);
}

std::shared_ptr<transaction::CounterGuard> TransactionState::counterGuard() {
  return _counterGuard;
}

/// @brief add a collection to a transaction
futures::Future<Result> TransactionState::addCollection(
    DataSourceId cid, std::string_view cname, AccessMode::Type accessType,
    bool lockUsage) {
#if defined(ARANGODB_ENABLE_MAINTAINER_MODE) && \
    defined(ARANGODB_ENABLE_FAILURE_TESTS)
  TRI_IF_FAILURE("WaitOnLock::" + std::string{cname}) {
    auto& raceController = basics::DebugRaceController::sharedInstance();
    if (auto data = raceController.waitForOthers(2, _id, vocbase().server());
        data) {
      TRI_ASSERT(data->size() == 2);
      // Slice out the first char, then we have a number
      uint32_t shardNum = basics::StringUtils::uint32(&cname.back(), 1);
      if (shardNum % 2 == 0) {
        auto min = *std::min_element(data->begin(), data->end(),
                                     [](std::any const& a, std::any const& b) {
                                       return std::any_cast<TransactionId>(a) <
                                              std::any_cast<TransactionId>(b);
                                     });
        if (_id == std::any_cast<TransactionId>(min)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      } else {
        auto max = *std::max_element(data->begin(), data->end(),
                                     [](std::any const& a, std::any const& b) {
                                       return std::any_cast<TransactionId>(a) <
                                              std::any_cast<TransactionId>(b);
                                     });
        if (_id == std::any_cast<TransactionId>(max)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
    }
  }
#endif

  Result res =
      co_await addCollectionInternal(cid, cname, accessType, lockUsage);

  if (res.ok()) {
    // upgrade transaction type if required
    if (AccessMode::isWriteOrExclusive(accessType) &&
        !AccessMode::isWriteOrExclusive(_type)) {
      // if one collection is written to, the whole transaction becomes a
      // write-y transaction
      if (_status == transaction::Status::CREATED) {
        // this is safe to do before the transaction has started
        _type = std::max(_type, accessType);
      } else if (_status == transaction::Status::RUNNING &&
                 _options.allowImplicitCollectionsForWrite &&
                 !isReadOnlyTransaction()) {
        // it is also safe to add another write collection to a write
        _type = std::max(_type, accessType);
      } else {
        // everything else is not safe and must be rejected
        auto message = absl::StrCat(
            TRI_errno_string(TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION),
            ": ", cname, " [", AccessMode::typeString(accessType), "]");
        res.reset(TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION, message);
      }
    }
  }

  co_return res;
}

futures::Future<Result> TransactionState::addCollectionInternal(
    DataSourceId cid, std::string_view cname, AccessMode::Type accessType,
    bool lockUsage) {
  Result res;

  RECURSIVE_WRITE_LOCKER(_collectionsLock, _collectionsLockOwner);

  // check if we already got this collection in the _collections vector
  auto colOrPos = findCollectionOrPos(cid);
  if (std::holds_alternative<CollectionFound>(colOrPos)) {
    auto* const trxColl = std::get<CollectionFound>(colOrPos).collection;
    LOG_TRX("ad6d0", TRACE, this)
        << "updating collection usage " << cid << ": '" << cname << "'";

    // we may need to recheck permissions here
    if (trxColl->accessType() < accessType) {
      res.reset(checkCollectionPermission(cid, cname, accessType));

      if (res.fail()) {
        co_return res;
      }
    }
    // collection is already contained in vector
    co_return res.reset(trxColl->updateUsage(accessType));
  }
  TRI_ASSERT(std::holds_alternative<CollectionNotFound>(colOrPos));
  auto const position = std::get<CollectionNotFound>(colOrPos).lowerBound;

  // collection not found.

  LOG_TRX("ad6e1", TRACE, this)
      << "adding new collection " << cid << ": '" << cname << "'";
  if (_status != transaction::Status::CREATED &&
      AccessMode::isWriteOrExclusive(accessType) &&
      !_options.allowImplicitCollectionsForWrite) {
    // trying to write access a collection that was not declared at start.
    // this is only supported internally for replication transactions.
    co_return res.reset(TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION,
                        std::string(TRI_errno_string(
                            TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION)) +
                            ": " + std::string{cname} + " [" +
                            AccessMode::typeString(accessType) + "]");
  }

  if (!AccessMode::isWriteOrExclusive(accessType) &&
      (isRunning() && !_options.allowImplicitCollectionsForRead)) {
    co_return res.reset(TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION,
                        std::string(TRI_errno_string(
                            TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION)) +
                            ": " + std::string{cname} + " [" +
                            AccessMode::typeString(accessType) + "]");
  }

  // now check the permissions
  res = checkCollectionPermission(cid, cname, accessType);

  if (res.fail()) {
    co_return res;
  }

  // collection was not contained. now create and insert it

  auto* const trxColl = createTransactionCollection(cid, accessType).release();

  TRI_ASSERT(trxColl != nullptr);

  // insert collection at the correct position
  try {
    _collections.insert(_collections.begin() + position, trxColl);
  } catch (...) {
    delete trxColl;
    co_return res.reset(TRI_ERROR_OUT_OF_MEMORY);
  }

  if (lockUsage) {
    TRI_ASSERT(!isRunning() || !AccessMode::isWriteOrExclusive(accessType) ||
               _options.allowImplicitCollectionsForWrite);
    res = co_await trxColl->lockUsage();
  }

  co_return res;
}

/// @brief use all participating collections of a transaction
futures::Future<Result> TransactionState::useCollections() {
  RECURSIVE_READ_LOCKER(_collectionsLock, _collectionsLockOwner);
  Result res;
  // process collections in forward order
  for (TransactionCollection* trxCollection : _collections) {
    res = co_await trxCollection->lockUsage();

    if (!res.ok()) {
      break;
    }
  }
  co_return res;
}

/// @brief find a collection in the transaction's list of collections
TransactionCollection* TransactionState::findCollection(
    DataSourceId cid) const {
  RECURSIVE_READ_LOCKER(_collectionsLock, _collectionsLockOwner);
  for (auto* trxCollection : _collections) {
    if (cid == trxCollection->id()) {
      // found
      return trxCollection;
    }
    if (cid < trxCollection->id()) {
      // collection not found
      break;
    }
  }

  return nullptr;
}

/// @brief find a collection in the transaction's list of collections
///        The idea is if a collection is found it will be returned.
///        In this case the position is not used.
///        In case the collection is not found. It will return a
///        lower bound of its position. The position
///        defines where the collection should be inserted,
///        so whenever we want to insert the collection we
///        have to use this position for insert.
auto TransactionState::findCollectionOrPos(DataSourceId cid) const
    -> std::variant<CollectionNotFound, CollectionFound> {
  RECURSIVE_READ_LOCKER(_collectionsLock, _collectionsLockOwner);
  if (_collections.empty()) {
    return CollectionNotFound{0};
  }

  auto it = std::lower_bound(_collections.begin(), _collections.end(), cid,
                             [](const auto* trxCollection, auto cid) {
                               return trxCollection->id() < cid;
                             });
  if (it != _collections.end() && (*it)->id() == cid) {
    // found
    return CollectionFound{*it};
  }

  // return the insert position if required
  return CollectionNotFound{
      static_cast<std::size_t>(std::distance(_collections.begin(), it))};
}

void TransactionState::setExclusiveAccessType() {
  if (_status != transaction::Status::CREATED) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL, "cannot change the type of a running transaction");
  }
  _type = AccessMode::Type::EXCLUSIVE;
}

void TransactionState::acceptAnalyzersRevision(
    QueryAnalyzerRevisions const& analyzersRevision) noexcept {
  // only init from default allowed! Or we have problem -> different
  // analyzersRevision in one transaction
  LOG_TOPIC_IF("9127a", ERR, Logger::AQL,
               (_analyzersRevision != analyzersRevision &&
                !_analyzersRevision.isDefault()))
      << " Changing analyzers revision for transaction from "
      << _analyzersRevision << " to " << analyzersRevision;
  TRI_ASSERT(_analyzersRevision == analyzersRevision ||
             _analyzersRevision.isDefault());
  _analyzersRevision = analyzersRevision;
}

[[nodiscard]] transaction::OperationOrigin TransactionState::operationOrigin()
    const noexcept {
  return _operationOrigin;
}

Result TransactionState::checkCollectionPermission(
    DataSourceId cid, std::string_view cname, AccessMode::Type accessType) {
  TRI_ASSERT(!cname.empty());
  ExecContext const& exec = ExecContext::current();

  // no need to check for superuser, cluster_sync tests break otherwise
  if (exec.isSuperuser()) {
    return Result{};
  }

  auto level = exec.collectionAuthLevel(_vocbase.name(), cname);
  TRI_ASSERT(level != auth::Level::UNDEFINED);  // not allowed here

  if (level == auth::Level::NONE) {
    LOG_TOPIC("24971", TRACE, Logger::AUTHORIZATION)
        << "User " << exec.user() << " has collection auth::Level::NONE";

#ifdef USE_ENTERPRISE
    if (accessType == AccessMode::Type::READ &&
        _options.skipInaccessibleCollections) {
      addInaccessibleCollection(cid, std::string{cname});
      return Result();
    }
#endif

    return Result(TRI_ERROR_FORBIDDEN,
                  std::string(TRI_errno_string(TRI_ERROR_FORBIDDEN)) + ": " +
                      std::string{cname} + " [" +
                      AccessMode::typeString(accessType) + "]");
  } else {
    bool collectionWillWrite = AccessMode::isWriteOrExclusive(accessType);

    if (level == auth::Level::RO && collectionWillWrite) {
      LOG_TOPIC("d3e61", TRACE, Logger::AUTHORIZATION)
          << "User " << exec.user() << " has no write right for collection "
          << cname;

      return Result(TRI_ERROR_ARANGO_READ_ONLY,
                    std::string(TRI_errno_string(TRI_ERROR_ARANGO_READ_ONLY)) +
                        ": " + std::string{cname} + " [" +
                        AccessMode::typeString(accessType) + "]");
    }
  }

  return Result{};
}

/// @brief clear the query cache for all collections that were modified by
/// the transaction
void TransactionState::clearQueryCache() const {
  RECURSIVE_READ_LOCKER(_collectionsLock, _collectionsLockOwner);
  if (_collections.empty()) {
    return;
  }

  try {
    std::vector<std::string> collections;

    for (auto& trxCollection : _collections) {
      if (trxCollection                      // valid instance
          && trxCollection->collection()     // has a valid collection
          && trxCollection->hasOperations()  // may have been modified
      ) {
        // we're only interested in collections that may have been modified
        collections.emplace_back(trxCollection->collection()->guid());
      }
    }

    if (!collections.empty()) {
      arangodb::aql::QueryCache::instance()->invalidate(&_vocbase, collections);
    }
  } catch (...) {
    // in case something goes wrong, we have to remove all queries from the
    // cache
    arangodb::aql::QueryCache::instance()->invalidate(&_vocbase);
  }
}

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
// only used in maintainer mode for testing
void TransactionState::setHistoryEntry(
    std::shared_ptr<transaction::HistoryEntry> const& entry) {
  TRI_ASSERT(entry != nullptr);
  TRI_ASSERT(_historyEntry == nullptr);
  _historyEntry = entry;
}

void TransactionState::clearHistoryEntry() noexcept { _historyEntry.reset(); }

void TransactionState::adjustMemoryUsage(std::int64_t value) noexcept {
  if (_historyEntry != nullptr) {
    _historyEntry->adjustMemoryUsage(value);
  }
}
#endif

#ifdef ARANGODB_USE_GOOGLE_TESTS
// reset the internal Transaction ID to none.
// Only used in the Transaction Mock for internal reasons.
void TransactionState::resetTransactionId() {
  _id = arangodb::TransactionId::none();
}
#endif

/// @brief update the status of a transaction
void TransactionState::updateStatus(transaction::Status status) noexcept {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (_status != transaction::Status::CREATED &&
      _status != transaction::Status::RUNNING) {
    LOG_TOPIC("257ea", ERR, Logger::FIXME)
        << "trying to update transaction status with "
           "an invalid state. current: "
        << _status << ", future: " << status;
  }
#endif

  TRI_ASSERT(_status == transaction::Status::CREATED ||
             _status == transaction::Status::RUNNING);

  if (_status == transaction::Status::CREATED) {
    TRI_ASSERT(status == transaction::Status::RUNNING ||
               status == transaction::Status::ABORTED);
  } else if (_status == transaction::Status::RUNNING) {
    TRI_ASSERT(status == transaction::Status::COMMITTED ||
               status == transaction::Status::ABORTED);
  }

  _status = status;
}

/// @brief returns the name of the actor the transaction runs on:
/// - leader
/// - follower
/// - coordinator
/// - single
std::string_view TransactionState::actorName() const noexcept {
  if (isDBServer()) {
    return hasHint(transaction::Hints::Hint::IS_FOLLOWER_TRX) ? "follower"
                                                              : "leader";
  } else if (isCoordinator()) {
    return "coordinator";
  }
  return "single";
}

void TransactionState::coordinatorRerollTransactionId() {
  // cppcheck-suppress ignoredReturnValue
  TRI_ASSERT(isCoordinator());
  // cppcheck-suppress ignoredReturnValue
  TRI_ASSERT(isRunning());
  auto old = _id;
  _id = transaction::Context::makeTransactionId();
  LOG_TOPIC("a565a", DEBUG, Logger::TRANSACTIONS)
      << "Rerolling transaction id from " << old << " to " << _id;
  clearKnownServers();
  // Increase sequential lock by one.
  statistics()._sequentialLocks.count();
}

/// @brief return a reference to the global transaction statistics
TransactionStatistics& TransactionState::statistics() const noexcept {
  return _vocbase.server()
      .getFeature<metrics::MetricsFeature>()
      .serverStatistics()
      ._transactionsStatistics;
}

void TransactionState::chooseReplicasNolock(
    containers::FlatHashSet<ShardID> const& shards) {
  if (_chosenReplicas == nullptr) {
    _chosenReplicas =
        std::make_unique<containers::FlatHashMap<ShardID, ServerID>>(
            shards.size());
  }
  auto& cf = _vocbase.server().getFeature<ClusterFeature>();
  auto& ci = cf.clusterInfo();
#ifdef USE_ENTERPRISE
  ci.getResponsibleServersReadFromFollower(shards, *_chosenReplicas);
#else
  *_chosenReplicas = ci.getResponsibleServers(shards);
#endif
}

void TransactionState::chooseReplicas(
    containers::FlatHashSet<ShardID> const& shards) {
  std::lock_guard guard{_replicaMutex};
  chooseReplicasNolock(shards);
}

ServerID TransactionState::whichReplica(ShardID const& shard) {
  std::lock_guard guard{_replicaMutex};
  if (_chosenReplicas != nullptr) {
    auto it = _chosenReplicas->find(shard);
    if (it != _chosenReplicas->end()) {
      return it->second;
    }
  }
  // Not yet decided
  containers::FlatHashSet<ShardID> shards;
  shards.emplace(shard);
  chooseReplicasNolock(shards);
  // cppcheck-suppress nullPointerRedundantCheck
  auto it = _chosenReplicas->find(shard);
  // cppcheck-suppress nullPointerRedundantCheck
  TRI_ASSERT(it != _chosenReplicas->end());
  return it->second;
}

containers::FlatHashMap<ShardID, ServerID> TransactionState::whichReplicas(
    containers::FlatHashSet<ShardID> const& shardIds) {
  std::lock_guard guard{_replicaMutex};
  chooseReplicasNolock(shardIds);
  containers::FlatHashMap<ShardID, ServerID> result;
  for (auto const& shard : shardIds) {
    auto it = _chosenReplicas->find(shard);
    TRI_ASSERT(it != _chosenReplicas->end());
    result.try_emplace(shard, it->second);
  }
  return result;
}
