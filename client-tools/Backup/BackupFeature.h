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
/// @author Dan Larkin-York
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Backup/arangobackup.h"
#include "ApplicationFeatures/ApplicationFeature.h"

#include "Utils/ClientManager.h"

namespace arangodb {

class BackupFeature : public ArangoBackupFeature {
 public:
  static constexpr std::string_view name() noexcept { return "Backup"; }

  BackupFeature(Server& server, int& exitCode);

  // for documentation of virtual methods, see `ApplicationFeature`
  virtual void collectOptions(
      std::shared_ptr<options::ProgramOptions>) override final;
  virtual void validateOptions(
      std::shared_ptr<options::ProgramOptions> options) override final;
  virtual void start() override final;

  /**
   * @brief Returns the feature name (for registration with `ApplicationServer`)
   * @return The name of the feature
   */
  static std::string featureName();

  /**
   * @brief Construct a list of the valid operations, using the given separator
   * @return A strifigied list of valid operations
   */
  static std::string operationList(std::string const& separator);

 public:
  struct Options {
    bool allowInconsistent = false;
    std::string identifier = "";
    std::string label = "";
    std::string statusId = "";
    std::string rcloneConfigFile = "";
    std::string remoteDirectory = "";
    double maxWaitForLock = 60.0;
    double maxWaitForRestart = 0.0;
    std::string operation = "list";
    bool abort = false;
    bool abortTransactionsIfNeeded = false;
    bool ignoreVersion = false;
  };

 private:
  ClientManager _clientManager;
  int& _exitCode;
  Options _options;
};

}  // namespace arangodb
