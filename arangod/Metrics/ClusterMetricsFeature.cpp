////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Valery Mironov
////////////////////////////////////////////////////////////////////////////////

#include "Metrics/ClusterMetricsFeature.h"
#include "Basics/system-functions.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterMethods.h"
#include "Metrics/MetricsFeature.h"
#include "Network/NetworkFeature.h"
#include "Scheduler/SchedulerFeature.h"

using namespace std::chrono_literals;

namespace arangodb::metrics {

namespace {
constexpr std::chrono::steady_clock::duration someDelay = 1s;
}

ClusterMetricsFeature::ClusterMetricsFeature(application_features::ApplicationServer& server)
    : ApplicationFeature{server, "ClusterMetrics"} {
  startsAfter<MetricsFeature>();
  startsAfter<ClusterFeature>();
  startsAfter<NetworkFeature>();
  startsAfter<SchedulerFeature>();
}

void ClusterMetricsFeature::asyncUpdate() {
  if (_count.fetch_add(1, std::memory_order_acq_rel) == 0) {
    forceAsyncUpdate();
  }
}

void ClusterMetricsFeature::forceAsyncUpdate() {
  SchedulerFeature::SCHEDULER->queue(RequestLane::CLUSTER_INTERNAL, [this] {
    _count.store(1, std::memory_order_relaxed);
    metricsOnCoordinator(server().getFeature<NetworkFeature>(),
                         server().getFeature<ClusterFeature>())
        .then([this](futures::Try<std::vector<std::string>>&& metrics) mutable {
          bool needRetry = !metrics.hasValue();
          if (!needRetry) {
            set(std::move(*metrics));
            needRetry = _count.fetch_sub(1, std::memory_order_acq_rel) != 1;  //  if true then was call asyncUpdate() after _count.store(1)
          }
          if (needRetry) {
            forceAsyncUpdate();
          }
        });
  });
}

RawClusterMetrics ClusterMetricsFeature::get() {
  auto data = std::atomic_load<RawClusterMetrics>(&_data);
  if (data) {
    return *data;
  }
  return {};
}

void ClusterMetricsFeature::set(RawClusterMetrics&& metrics) {
  std::atomic_store_explicit(&_data, std::make_shared<RawClusterMetrics>(std::move(metrics)),
                             std::memory_order_release);
}

}  // namespace arangodb::metrics
