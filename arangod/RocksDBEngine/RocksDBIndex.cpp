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

#include "RocksDBIndex.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/VelocyPackHelper.h"
#include "Cache/BinaryKeyHasher.h"
#include "Cache/CacheManagerFeature.h"
#include "Cache/Common.h"
#include "Cache/Manager.h"
#include "Cache/TransactionalCache.h"
#include "Logger/LogMacros.h"
#include "RocksDBEngine/RocksDBCollection.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBComparator.h"
#include "RocksDBEngine/RocksDBCuckooIndexEstimator.h"
#include "RocksDBEngine/RocksDBIndexingDisabler.h"
#include "RocksDBEngine/RocksDBMethods.h"
#include "RocksDBEngine/RocksDBTransactionState.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/Context.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"

#include <rocksdb/comparator.h>
#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/utilities/write_batch_with_index.h>

using namespace arangodb;
using namespace arangodb::rocksutils;

constexpr uint64_t arangodb::RocksDBIndex::ESTIMATOR_SIZE;

namespace {
inline uint64_t ensureObjectId(uint64_t oid) {
  return (oid != 0) ? oid : TRI_NewTickServer();
}
}  // namespace

RocksDBIndex::RocksDBIndex(
    IndexId id, LogicalCollection& collection, std::string const& name,
    std::vector<std::vector<arangodb::basics::AttributeName>> const& attributes,
    bool unique, bool sparse, rocksdb::ColumnFamilyHandle* cf,
    uint64_t objectId, bool useCache, cache::Manager* cacheManager,
    RocksDBEngine& engine)
    : Index(id, collection, name, attributes, unique, sparse),
      _cf(cf),
      _cacheManager(cacheManager != nullptr && !collection.system() &&
                            !collection.isAStub() &&
                            !ServerState::instance()->isCoordinator()
                        ? cacheManager
                        : nullptr),
      _cacheEnabled(useCache && (_cacheManager != nullptr)),
      _engine(engine),
      _objectId(::ensureObjectId(objectId)) {
  TRI_ASSERT(cf != nullptr &&
             cf != RocksDBColumnFamilyManager::get(
                       RocksDBColumnFamilyManager::Family::Definitions));
  _engine.addIndexMapping(_objectId, collection.vocbase().id(), collection.id(),
                          _iid);
}

RocksDBIndex::RocksDBIndex(IndexId id, LogicalCollection& collection,
                           arangodb::velocypack::Slice info,
                           rocksdb::ColumnFamilyHandle* cf, bool useCache,
                           cache::Manager* cacheManager, RocksDBEngine& engine)
    : Index(id, collection, info),
      _cf(cf),
      _cacheManager(cacheManager != nullptr && !collection.system() &&
                            !collection.isAStub() &&
                            !ServerState::instance()->isCoordinator()
                        ? cacheManager
                        : nullptr),
      _cacheEnabled(useCache && (_cacheManager != nullptr)),
      _engine(engine),
      _objectId(::ensureObjectId(basics::VelocyPackHelper::stringUInt64(
          info, StaticStrings::ObjectId))) {
  TRI_ASSERT(cf != nullptr &&
             cf != RocksDBColumnFamilyManager::get(
                       RocksDBColumnFamilyManager::Family::Definitions));
  _engine.addIndexMapping(_objectId, collection.vocbase().id(), collection.id(),
                          _iid);
}

RocksDBIndex::~RocksDBIndex() {
  _engine.removeIndexMapping(_objectId);
  if (useCache()) {
    TRI_ASSERT(_cacheManager != nullptr);
    destroyCache();
  }
}

rocksdb::Comparator const* RocksDBIndex::comparator() const {
  return _cf->GetComparator();
}

void RocksDBIndex::toVelocyPackFigures(VPackBuilder& builder) const {
  TRI_ASSERT(builder.isOpenObject());
  Index::toVelocyPackFigures(builder);
  auto cache = useCache();
  builder.add("cacheInUse", VPackValue(cache != nullptr));
  if (cache != nullptr) {
    auto [size, usage] = cache->sizeAndUsage();
    builder.add("cacheSize", VPackValue(size));
    builder.add("cacheUsage", VPackValue(usage));
    auto hitRates = cache->hitRates();
    double rate = hitRates.first;
    rate = std::isnan(rate) ? 0.0 : rate;
    builder.add("cacheLifeTimeHitRate", VPackValue(rate));
    rate = hitRates.second;
    rate = std::isnan(rate) ? 0.0 : rate;
    builder.add("cacheWindowedHitRate", VPackValue(rate));
  } else {
    builder.add("cacheSize", VPackValue(0));
    builder.add("cacheUsage", VPackValue(0));
  }
}

void RocksDBIndex::load() {
  if (_cacheEnabled.load(std::memory_order_relaxed)) {
    TRI_ASSERT(_cacheManager != nullptr);
    setupCache();
  }
}

void RocksDBIndex::unload() {
  if (useCache()) {
    TRI_ASSERT(_cacheManager != nullptr);
    destroyCache();
  }
}

/// @brief return a VelocyPack representation of the index
void RocksDBIndex::toVelocyPack(
    VPackBuilder& builder, std::underlying_type<Serialize>::type flags) const {
  Index::toVelocyPack(builder, flags);
  if (Index::hasFlag(flags, Index::Serialize::Internals)) {
    // If we store it, it cannot be 0
    TRI_ASSERT(_objectId != 0);
    builder.add(StaticStrings::ObjectId, VPackValue(std::to_string(_objectId)));
  }
  builder.add(arangodb::StaticStrings::IndexUnique, VPackValue(unique()));
  builder.add(arangodb::StaticStrings::IndexSparse, VPackValue(sparse()));
}

void RocksDBIndex::setCacheEnabled(bool enable) noexcept {
  // allow disabling and enabling of caches for the primary index
  _cacheEnabled.store(enable, std::memory_order_relaxed);
}

void RocksDBIndex::setupCache() {
  TRI_ASSERT(_cacheManager != nullptr);
  if (!_cacheEnabled.load(std::memory_order_relaxed)) {
    // if we cannot have a cache, return immediately
    return;
  }

  // there will never be a cache on the coordinator. this should be handled
  // by _cacheEnabled already.
  TRI_ASSERT(!ServerState::instance()->isCoordinator());

  auto cache = _cache;
  if (cache == nullptr) {
    TRI_ASSERT(_cacheManager != nullptr);
    LOG_TOPIC("49e6c", DEBUG, Logger::CACHE) << "Creating index cache";
    // virtual call!
    cache = makeCache();
    std::atomic_store_explicit(&_cache, std::move(cache),
                               std::memory_order_relaxed);
  }
}

void RocksDBIndex::destroyCache() noexcept {
  auto cache = _cache;
  if (cache != nullptr) {
    try {
      std::atomic_store_explicit(&_cache, {}, std::memory_order_relaxed);
      TRI_ASSERT(_cacheManager != nullptr);
      LOG_TOPIC("b5d85", DEBUG, Logger::CACHE) << "Destroying index cache";
      _cacheManager->destroyCache(std::move(cache));
    } catch (...) {
      // meh
    }
  }
}

Result RocksDBIndex::drop() {
  auto* coll = toRocksDBCollection(_collection);
  // edge index needs to be dropped with prefixSameAsStart = false
  // otherwise full index scan will not work
  bool const prefixSameAsStart = type() != Index::TRI_IDX_TYPE_EDGE_INDEX;
  bool const useRangeDelete = coll->meta().numberDocuments() >= 32 * 1024;

  Result r = rocksutils::removeLargeRange(_engine.db(), getBounds(),
                                          prefixSameAsStart, useRangeDelete);

  // Try to drop the cache as well.
  destroyCache();

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // check if documents have been deleted
  size_t numDocs = rocksutils::countKeyRange(_engine.db(), getBounds(), nullptr,
                                             prefixSameAsStart);
  if (numDocs > 0) {
    std::string errorMsg(
        "deletion check in index drop failed - not all documents in the index "
        "have been deleted. remaining: ");
    errorMsg.append(std::to_string(numDocs));
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, errorMsg);
  }
#endif

  return r;
}

ResultT<TruncateGuard> RocksDBIndex::truncateBegin(rocksdb::WriteBatch& batch) {
  auto bounds = getBounds();
  auto s =
      batch.DeleteRange(bounds.columnFamily(), bounds.start(), bounds.end());
  auto r = rocksutils::convertStatus(s);
  if (!r.ok()) {
    return r;
  }
  return {};
}

void RocksDBIndex::truncateCommit(TruncateGuard&& guard,
                                  TRI_voc_tick_t /*tick*/,
                                  transaction::Methods* /*trx*/) {
  // simply drop the cache and re-create it
  if (_cacheEnabled.load(std::memory_order_relaxed)) {
    TRI_ASSERT(_cacheManager != nullptr);
    destroyCache();
    setupCache();
  }
}

/// performs a preflight check for an insert operation, not carrying out any
/// modifications to the index.
/// the default implementation does nothing. indexes can override this and
/// perform useful checks (uniqueness checks etc.) here
Result RocksDBIndex::checkInsert(transaction::Methods& /*trx*/,
                                 RocksDBMethods* /*methods*/,
                                 LocalDocumentId /*documentId*/,
                                 arangodb::velocypack::Slice /*doc*/,
                                 OperationOptions const& /*options*/) {
  // default implementation does nothing - derived indexes can override this!
  return {};
}

/// performs a preflight check for an update/replace operation, not carrying out
/// any modifications to the index. the default implementation does nothing.
/// indexes can override this and perform useful checks (uniqueness checks etc.)
/// here
Result RocksDBIndex::checkReplace(transaction::Methods& /*trx*/,
                                  RocksDBMethods* /*methods*/,
                                  LocalDocumentId /*documentId*/,
                                  arangodb::velocypack::Slice /*doc*/,
                                  OperationOptions const& /*options*/) {
  // default implementation does nothing - derived indexes can override this!
  return {};
}

Result RocksDBIndex::update(transaction::Methods& trx, RocksDBMethods* mthd,
                            LocalDocumentId oldDocumentId,
                            velocypack::Slice oldDoc,
                            LocalDocumentId newDocumentId,
                            velocypack::Slice newDoc,
                            OperationOptions const& options,
                            bool performChecks) {
  // It is illegal to call this method on the primary index
  // RocksDBPrimaryIndex must override this method accordingly
  TRI_ASSERT(type() != TRI_IDX_TYPE_PRIMARY_INDEX);

  /// only if the insert needs to see the changes of the update, enable
  /// indexing:
  IndexingEnabler enabler(
      mthd, mthd->isIndexingDisabled() && hasExpansion() && unique());

  TRI_ASSERT((hasExpansion() && unique()) ? !mthd->isIndexingDisabled() : true);

  Result res = remove(trx, mthd, oldDocumentId, oldDoc, options);
  if (!res.ok()) {
    return res;
  }
  return insert(trx, mthd, newDocumentId, newDoc, options, performChecks);
}

void RocksDBIndex::refillCache(transaction::Methods& trx,
                               std::vector<std::string> const& /*keys*/) {}

/// @brief return the memory usage of the index
size_t RocksDBIndex::memory() const {
  rocksdb::TransactionDB* db = _engine.db();
  RocksDBKeyBounds bounds = getBounds();
  TRI_ASSERT(_cf == bounds.columnFamily());
  rocksdb::Range r(bounds.start(), bounds.end());
  uint64_t out;

  rocksdb::SizeApproximationOptions options{.include_memtables = true,
                                            .include_files = true};
  db->GetApproximateSizes(options, _cf, &r, 1, &out);
  return static_cast<size_t>(out);
}

/// compact the index, should reduce read amplification
void RocksDBIndex::compact() {
  if (_cf != RocksDBColumnFamilyManager::get(
                 RocksDBColumnFamilyManager::Family::Invalid)) {
    _engine.compactRange(getBounds());
  }
}

std::shared_ptr<cache::Cache> RocksDBIndex::useCache() const noexcept {
  // note: this can return a nullptr. the caller has to check the result
  return std::atomic_load_explicit(&_cache, std::memory_order_relaxed);
}

bool RocksDBIndex::canWarmup() const noexcept { return useCache() != nullptr; }

std::shared_ptr<cache::Cache> RocksDBIndex::makeCache() const {
  TRI_ASSERT(_cacheManager != nullptr);
  return _cacheManager->createCache<cache::BinaryKeyHasher>(
      cache::CacheType::Transactional);
}

// banish given key from transactional cache
bool RocksDBIndex::invalidateCacheEntry(char const* data, std::size_t len) {
  if (auto cache = useCache()) {
    do {
      auto status = cache->banish(data, static_cast<uint32_t>(len));
      if (status == TRI_ERROR_NO_ERROR) {
        return true;
      }
      if (status == TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND) {
        break;
      }
      if (ADB_UNLIKELY(status == TRI_ERROR_SHUTTING_DOWN)) {
        destroyCache();
        break;
      }
    } while (true);
  }
  return false;
}

/// @brief get index estimator, optional
RocksDBCuckooIndexEstimatorType* RocksDBIndex::estimator() { return nullptr; }

void RocksDBIndex::setEstimator(
    std::unique_ptr<RocksDBCuckooIndexEstimatorType>) {}

void RocksDBIndex::recalculateEstimates() {}

RocksDBKeyBounds RocksDBIndex::getBounds(Index::IndexType type,
                                         uint64_t objectId, bool unique) {
  switch (type) {
    case RocksDBIndex::TRI_IDX_TYPE_PRIMARY_INDEX:
      return RocksDBKeyBounds::PrimaryIndex(objectId);
    case RocksDBIndex::TRI_IDX_TYPE_EDGE_INDEX:
      return RocksDBKeyBounds::EdgeIndex(objectId);
    case RocksDBIndex::TRI_IDX_TYPE_HASH_INDEX:
    case RocksDBIndex::TRI_IDX_TYPE_SKIPLIST_INDEX:
    case RocksDBIndex::TRI_IDX_TYPE_TTL_INDEX:
    case RocksDBIndex::TRI_IDX_TYPE_PERSISTENT_INDEX:
    case RocksDBIndex::TRI_IDX_TYPE_INVERTED_INDEX:
      if (unique) {
        return RocksDBKeyBounds::UniqueVPackIndex(objectId, false);
      }
      return RocksDBKeyBounds::VPackIndex(objectId, false);
    case RocksDBIndex::TRI_IDX_TYPE_FULLTEXT_INDEX:
      return RocksDBKeyBounds::FulltextIndex(objectId);
    case RocksDBIndex::TRI_IDX_TYPE_GEO1_INDEX:
    case RocksDBIndex::TRI_IDX_TYPE_GEO2_INDEX:
      return RocksDBKeyBounds::LegacyGeoIndex(objectId);
    case RocksDBIndex::TRI_IDX_TYPE_GEO_INDEX:
      return RocksDBKeyBounds::GeoIndex(objectId);
    case RocksDBIndex::TRI_IDX_TYPE_IRESEARCH_LINK:
      return RocksDBKeyBounds::DatabaseViews(objectId);
    case RocksDBIndex::TRI_IDX_TYPE_ZKD_INDEX:
      return RocksDBKeyBounds::ZkdIndex(objectId);
    case RocksDBIndex::TRI_IDX_TYPE_UNKNOWN:
    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }
}
