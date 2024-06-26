/* Copyright 2024 The OpenXLA Authors.

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

#include "xla/service/cpu/runtime/kernel_thunk.h"

#include <cstdint>
#include <string>
#include <utility>

#include "absl/container/inlined_vector.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "xla/runtime/buffer_use.h"
#include "xla/service/buffer_assignment.h"
#include "xla/service/cpu/runtime/thunk.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/host/host_kernel.h"
#include "xla/stream_executor/host/host_kernel_c_api.h"
#include "xla/stream_executor/launch_dim.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/logging.h"
#include "tsl/platform/statusor.h"
#include "tsl/profiler/lib/traceme.h"

namespace xla::cpu {

KernelThunk::KernelThunk(
    Info info, absl::Span<const BufferAllocation::Slice> arguments_buffers,
    absl::Span<const BufferAllocation::Slice> results_buffers,
    std::string kernel_name, se::ThreadDim thread_dim)
    : Thunk(Kind::kKernel, std::move(info)),
      arguments_buffers_(arguments_buffers.begin(), arguments_buffers.end()),
      results_buffers_(results_buffers.begin(), results_buffers.end()),
      kernel_name_(std::move(kernel_name)),
      thread_dim_(thread_dim) {}

absl::Status KernelThunk::Execute(const ExecuteParams& params) {
  tsl::profiler::TraceMe trace([&] { return TraceMeEncode(); });

  VLOG(3) << absl::StreamFormat(
      "Launch host kernel %s with %d arguments buffers and %d results buffers: "
      "#threads=%s",
      kernel_name_, arguments_buffers_.size(), results_buffers_.size(),
      thread_dim_.ToString());

  absl::InlinedVector<se::DeviceMemoryBase, 8> buffers_data;
  buffers_data.reserve(arguments_buffers_.size() + results_buffers_.size());

  int64_t arg_num = 0;
  for (BufferAllocation::Slice& buffer : arguments_buffers_) {
    TF_ASSIGN_OR_RETURN(buffers_data.emplace_back(),
                        params.buffer_allocations->GetDeviceAddress(buffer));
    VLOG(3) << absl::StreamFormat("  arg #%d: %s (%p)", arg_num++,
                                  buffer.ToString(),
                                  buffers_data.back().opaque());
  }

  int64_t res_num = 0;
  for (BufferAllocation::Slice& buffer : results_buffers_) {
    TF_ASSIGN_OR_RETURN(buffers_data.emplace_back(),
                        params.buffer_allocations->GetDeviceAddress(buffer));
    VLOG(3) << absl::StreamFormat("  res #%d: %s (%p)", res_num++,
                                  buffer.ToString(),
                                  buffers_data.back().opaque());
  }

  // TODO(ezhulenev): Kernel ptr should be loaded as a part of Thunk
  // initialization stage.
  TF_ASSIGN_OR_RETURN(SE_HOST_Kernel * kernel_ptr,
                      params.host_kernels->Find(kernel_name_));

  // TODO(ezhulenev): Instead of using HostKernel directly we should be going
  // through the stream executor APIs.
  se::host::HostKernel kernel(buffers_data.size(), kernel_ptr, nullptr);
  TF_RETURN_IF_ERROR(kernel.Launch(thread_dim_, buffers_data));

  return absl::OkStatus();
}

KernelThunk::BufferUses KernelThunk::buffer_uses() const {
  BufferUses buffer_uses;
  for (const BufferAllocation::Slice& buffer : arguments_buffers_) {
    buffer_uses.emplace_back(buffer, BufferUse::kRead);
  }
  for (const BufferAllocation::Slice& buffer : results_buffers_) {
    buffer_uses.emplace_back(buffer, BufferUse::kWrite);
  }
  return buffer_uses;
}

}  // namespace xla::cpu
