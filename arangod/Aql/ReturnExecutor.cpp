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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "ReturnExecutor.h"
#include "Aql/AqlValue.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/SingleRowFetcher.h"
#include "Basics/Common.h"
#include "Basics/Exceptions.h"

#include <algorithm>

using namespace arangodb;
using namespace arangodb::aql;

ReturnExecutorInfos::ReturnExecutorInfos(RegisterId inputRegister, bool doCount)
    : _inputRegisterId(inputRegister), _doCount(doCount) {
  // For the time being return will only write to register 0.
  // It is defined that it can only have exactly 1 output register.
  // We can easily replace this by a different register, if we
  // modify the caller within the ExecutionEngine to ask for the
  // output register from outside
}

ReturnExecutor::ReturnExecutor(Fetcher& fetcher, ReturnExecutorInfos& infos)
    : _infos(infos) {}

ReturnExecutor::~ReturnExecutor() = default;

auto ReturnExecutor::skipRowsRange(AqlItemBlockInputRange& inputRange,
                                   AqlCall& call)
    -> std::tuple<ExecutorState, Stats, size_t, AqlCall> {
  TRI_IF_FAILURE("ReturnExecutor::produceRows") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  auto stats = Stats{};
  auto skippedUpstream = inputRange.skipAll();
  call.didSkip(skippedUpstream);

  if (inputRange.hasDataRow() && call.needSkipMore()) {
    // I do not think that this is actually called.
    // It will be called first to get the upstream-Call
    // but this executor will always delegate the skipping
    // to upstream.
    TRI_ASSERT(false);
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        "ReturnExecutor::skipRowsRange shouldn't be called");
  }

  return {inputRange.upstreamState(), stats, call.getSkipCount(), call};
}

auto ReturnExecutor::produceRows(AqlItemBlockInputRange& inputRange,
                                 OutputAqlItemRow& output)
    -> std::tuple<ExecutorState, Stats, AqlCall> {
  TRI_IF_FAILURE("ReturnExecutor::produceRows") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  Stats stats{};

  while (inputRange.hasDataRow() && !output.isFull()) {
    auto [state, input] =
        inputRange.nextDataRow(AqlItemBlockInputRange::HasDataRow{});
    TRI_ASSERT(input.isInitialized());
    // REMARK: it is called `getInputRegisterId` here but FilterExecutor calls
    // it `getInputRegister`.
    AqlValue val = input.stealValue(_infos.getInputRegisterId());
    AqlValueGuard guard(val, true);
    TRI_IF_FAILURE("ReturnBlock::getSome") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }
    output.moveValueInto(_infos.getOutputRegisterId(), input, &guard);
    output.advanceRow();
    if (_infos.doCount()) {
      stats.incrCounted();
    }
  }

  return {inputRange.upstreamState(), stats, output.getClientCall()};
}

[[nodiscard]] auto ReturnExecutor::expectedNumberOfRows(
    AqlItemBlockInputRange const& input, AqlCall const& call) const noexcept
    -> size_t {
  if (input.finalState() == MainQueryState::DONE) {
    return input.countDataRows();
  }
  // Otherwise we do not know.
  return call.getLimit();
}
