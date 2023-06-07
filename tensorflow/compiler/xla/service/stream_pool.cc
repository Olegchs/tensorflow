/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/stream_pool.h"

#include <memory>
#include <utility>

#include "tensorflow/tsl/platform/logging.h"

namespace xla {

StreamPool::Ptr StreamPool::BorrowStream(
    se::StreamExecutor* executor, stream_executor::StreamPriority priority) {
  std::unique_ptr<se::Stream> stream;

  {
    absl::MutexLock lock(&mu_);
    if (streams_with_pri_.find(priority) == streams_with_pri_.end()) {
      stream = nullptr;
    } else {
      while (!streams_with_pri_[priority].empty() && !stream) {
        // Re-use an existing stream from the pool.
        stream = std::move(streams_with_pri_[priority].back());
        streams_with_pri_[priority].pop_back();
        if (stream->ok()) {
          VLOG(1) << stream->DebugStreamPointers()
                  << " StreamPool reusing existing stream with priority: "
                  << stream_executor::StreamPriorityToString(priority);
        } else {
          VLOG(1) << stream->DebugStreamPointers()
                  << " stream was not ok, StreamPool deleting with priority: "
                  << stream_executor::StreamPriorityToString(priority);
          stream = nullptr;
        }
      }
    }
  }

  if (!stream) {
    // Create a new stream.
    stream = std::make_unique<se::Stream>(executor);
    auto stream_impl = stream->implementation();
    stream_impl->SetPriority(priority);
    VLOG(1) << "Set stream priority to: "
            << stream_executor::StreamPriorityToString(priority);
    stream->Init();
    VLOG(1) << stream->DebugStreamPointers()
            << " StreamPool created new stream";
  }

  // Return the stream wrapped in Ptr, which has our special deleter semantics.
  PtrDeleter deleter = {this};
  return Ptr(stream.release(), deleter);
}

void StreamPool::ReturnStream(se::Stream* stream) {
  if (stream->ok()) {
    VLOG(1) << stream->DebugStreamPointers()
            << " StreamPool returning ok stream";
    absl::MutexLock lock(&mu_);
    auto priority = std::get<stream_executor::StreamPriority>(
        stream->implementation()->priority());
    streams_with_pri_[priority].emplace_back(stream);
  } else {
    // If the stream has encountered any errors, all subsequent operations on it
    // will fail. So just delete the stream, and rely on new streams to be
    // created in the future.
    VLOG(1) << stream->DebugStreamPointers()
            << " StreamPool deleting !ok stream";
    delete stream;
  }
}

}  // namespace xla
