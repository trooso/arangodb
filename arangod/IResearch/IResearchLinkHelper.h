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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <memory>
#include <utils/type_id.hpp>
#include <unordered_set>

#include "Basics/Result.h"
#include "IResearch/IResearchCommon.h"
#include "RestServer/arangod.h"
#include "VocBase/Identifiers/DataSourceId.h"
#include "VocBase/Identifiers/IndexId.h"
#include "VocBase/voc-types.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/IResearch/IResearchOptimizeTopK.h"
#endif

namespace arangodb {

class LogicalCollection;
class LogicalView;

namespace application_features {

class ApplicationServer;

}  // namespace application_features
namespace velocypack {

class Slice;
class Builder;

}  // namespace velocypack
namespace iresearch {

class IResearchLink;
struct IResearchLinkMeta;
class IResearchViewSort;
class IResearchViewStoredValues;

struct IResearchLinkHelper {
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief return a reference to a static VPackSlice of an empty index
  ///        definition
  //////////////////////////////////////////////////////////////////////////////
  static velocypack::Builder emptyIndexSlice(uint64_t objectId);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief compare two link definitions for equivalience if used to create a
  ///        link instance
  //////////////////////////////////////////////////////////////////////////////
  static bool equal(ArangodServer& server, velocypack::Slice lhs,
                    velocypack::Slice rhs, std::string_view dbname);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief finds link between specified collection and view with the given id
  //////////////////////////////////////////////////////////////////////////////
  static std::shared_ptr<IResearchLink> find(
      LogicalCollection const& collection, IndexId id);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief finds first link between specified collection and view
  //////////////////////////////////////////////////////////////////////////////
  static std::shared_ptr<IResearchLink> find(
      LogicalCollection const& collection, LogicalView const& view);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief validate and copy required fields from the 'definition' into
  ///        'normalized'
  /// @note missing analyzers will be created if exceuted on db-server
  /// @note engine == nullptr then SEGFAULT in Methods constructor during insert
  /// @note true == inRecovery() then AnalyzerFeature will not allow persistence
  //////////////////////////////////////////////////////////////////////////////
  static Result normalize(
      velocypack::Builder& normalized, velocypack::Slice definition,
      bool isCreation, TRI_vocbase_t const& vocbase, LinkVersion defaultVersion,
      IResearchViewSort const* primarySort = nullptr,
      irs::type_info::type_id const* primarySortCompression = nullptr,
      IResearchViewStoredValues const* storedValues = nullptr,
#ifdef USE_ENTERPRISE
      IResearchOptimizeTopK const* optimizeTopK = nullptr,
      bool const* pkCache = nullptr, bool const* sortCache = nullptr,
#endif
      velocypack::Slice idSlice = {}, std::string_view collectionName = {});

  //////////////////////////////////////////////////////////////////////////////
  /// @brief validate the link specifications for:
  ///        * valid link meta
  ///        * collection existence
  ///        * collection permissions
  ///        * valid link meta
  //////////////////////////////////////////////////////////////////////////////
  static Result validateLinks(TRI_vocbase_t& vocbase, velocypack::Slice links);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief visits all links in a collection
  /// @return full visitation compleated
  //////////////////////////////////////////////////////////////////////////////
  static bool visit(LogicalCollection const& collection,
                    std::function<bool(IResearchLink& link)> const& visitor);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief updates the collections in 'vocbase' to match the specified
  ///        IResearchLink definitions
  /// @param modified set of modified collection IDs
  /// @param view the view to associate created links with
  /// @param links the link modification definitions, null link == link removal
  /// @param stale links to remove if there is no creation definition in 'links'
  /// @param defaultVersion link version for creation if not set in a definition
  //////////////////////////////////////////////////////////////////////////////
  static Result updateLinks(
      containers::FlatHashSet<DataSourceId>& modified, LogicalView& view,
      velocypack::Slice links, LinkVersion defaultVersion,
      containers::FlatHashSet<DataSourceId> const& stale = {});

 private:
  IResearchLinkHelper() = delete;
};

}  // namespace iresearch
}  // namespace arangodb
