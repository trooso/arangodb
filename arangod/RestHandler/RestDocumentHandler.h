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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/Common.h"
#include "RestHandler/RestVocbaseBaseHandler.h"

namespace arangodb {
struct OperationOptions;

namespace transaction {
class Methods;
}

class RestDocumentHandler : public RestVocbaseBaseHandler {
 public:
  RestDocumentHandler(ArangodServer&, GeneralRequest*, GeneralResponse*);
  ~RestDocumentHandler();

 public:
  RestStatus execute() override final;
  char const* name() const override final { return "RestDocumentHandler"; }
  RequestLane lane() const override final;

  void shutdownExecute(bool isFinalized) noexcept override final;

 private:
  // inserts a document
  [[nodiscard]] futures::Future<futures::Unit> insertDocument();

  // reads a single or all documents
  RestStatus readDocument();

  // reads a single document
  [[nodiscard]] futures::Future<futures::Unit> readSingleDocument(
      bool generateBody);

  // reads multiple documents
  [[nodiscard]] futures::Future<futures::Unit> readManyDocuments();

  // reads a single document head
  RestStatus checkDocument();

  // replaces a document
  RestStatus replaceDocument();

  // updates a document
  RestStatus updateDocument();

  // helper function for replace and update
  [[nodiscard]] futures::Future<futures::Unit> modifyDocument(bool);

  // removes a document
  [[nodiscard]] futures::Future<futures::Unit> removeDocument();

  void handleFillIndexCachesValue(OperationOptions& options);

  void addTransactionHints(std::string const& collectionName, bool isMultiple,
                           bool isOverwritingInsert);

  std::unique_ptr<transaction::Methods> _activeTrx;
};
}  // namespace arangodb
