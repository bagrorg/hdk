/*
 * Copyright 2017 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef QUERYENGINE_COMPILATIONOPTIONS_H
#define QUERYENGINE_COMPILATIONOPTIONS_H

#include <cstdio>
#include <ostream>
#include <vector>

#ifndef __CUDACC__
#include <ostream>
#endif

#include "Shared/Config.h"

enum class ExecutorDeviceType { CPU = 0, GPU };
#ifndef __CUDACC__
inline std::ostream& operator<<(std::ostream& os, ExecutorDeviceType const dt) {
  constexpr char const* strings[]{"CPU", "GPU"};
  return os << strings[static_cast<int>(dt)];
}
#endif

enum class ExecutorOptLevel { Default, ReductionJIT };

enum class ExecutorExplainType { Default, Optimized };

enum class ExecutorDispatchMode { KernelPerFragment, MultifragmentKernel };

std::string deviceToString(const ExecutorDeviceType& dt);

struct CompilationOptions {
  ExecutorDeviceType device_type;
  bool hoist_literals;
  ExecutorOptLevel opt_level;
  bool with_dynamic_watchdog;
  bool allow_lazy_fetch;
  bool filter_on_deleted_column{true};  // if false, ignore the delete column during table
                                        // scans. Primarily disabled for delete queries.
  ExecutorExplainType explain_type{ExecutorExplainType::Default};
  bool register_intel_jit_listener{false};
  bool use_groupby_buffer_desc{false};

  static CompilationOptions makeCpuOnly(const CompilationOptions& in) {
    return CompilationOptions{ExecutorDeviceType::CPU,
                              in.hoist_literals,
                              in.opt_level,
                              in.with_dynamic_watchdog,
                              in.allow_lazy_fetch,
                              in.filter_on_deleted_column,
                              in.explain_type,
                              in.register_intel_jit_listener,
                              in.use_groupby_buffer_desc};
  }

  static CompilationOptions defaults(
      const ExecutorDeviceType device_type = ExecutorDeviceType::GPU) {
    return CompilationOptions{device_type,
                              /*hoist_literals=*/true,
                              /*opt_level=*/ExecutorOptLevel::Default,
                              /*with_dynamic_watchdog=*/false,
                              /*allow_lazy_fetch=*/true,
                              /*filter_on_delted_column=*/true,
                              /*explain_type=*/ExecutorExplainType::Default,
                              /*register_intel_jit_listener=*/false,
                              /*use_groupby_buffer_desc=*/false};
  }
};

enum class ExecutorType { Native, Extern, TableFunctions };

struct ExecutionOptions {
  bool output_columnar_hint;
  bool allow_multifrag;
  bool just_explain;  // return the generated IR for the first step
  bool allow_loop_joins;
  bool with_watchdog;  // Per work unit, not global.
  bool jit_debug;
  bool just_validate;
  bool with_dynamic_watchdog;            // Per work unit, not global.
  unsigned dynamic_watchdog_time_limit;  // Dynamic watchdog time limit, in milliseconds.
  bool find_push_down_candidates;
  bool just_calcite_explain;
  double gpu_input_mem_limit_percent;  // punt to CPU if input memory exceeds this
  bool allow_runtime_query_interrupt;
  double running_query_interrupt_freq;
  unsigned pending_query_interrupt_freq;
  ExecutorType executor_type = ExecutorType::Native;
  std::vector<size_t> outer_fragment_indices{};
  bool multifrag_result = false;
  bool preserve_order = false;

  static ExecutionOptions fromConfig(const Config& config) {
    auto eo = ExecutionOptions();
    eo.output_columnar_hint = config.rs.enable_columnar_output;
    eo.allow_multifrag = true;
    eo.just_explain = false;
    eo.allow_loop_joins = config.exec.join.allow_loop_joins;
    eo.with_watchdog = config.exec.watchdog.enable;
    eo.jit_debug = false;
    eo.just_validate = false;
    eo.with_dynamic_watchdog = config.exec.watchdog.enable_dynamic;
    eo.dynamic_watchdog_time_limit = config.exec.watchdog.time_limit;
    eo.find_push_down_candidates = config.opts.filter_pushdown.enable;
    eo.just_calcite_explain = false;
    eo.gpu_input_mem_limit_percent = config.mem.gpu.input_mem_limit_percent;
    eo.allow_runtime_query_interrupt =
        config.exec.interrupt.enable_runtime_query_interrupt;
    eo.running_query_interrupt_freq = config.exec.interrupt.running_query_interrupt_freq;
    eo.pending_query_interrupt_freq = 0;

    eo.multifrag_result = config.exec.enable_multifrag_rs;
    eo.preserve_order = false;

    return eo;
  }

  ExecutionOptions with_multifrag_result(bool enable = true) const {
    ExecutionOptions eo = *this;
    eo.multifrag_result = enable;
    return eo;
  }

  ExecutionOptions with_preserve_order(bool enable = true) const {
    ExecutionOptions eo = *this;
    eo.preserve_order = enable;
    return eo;
  }

 private:
  ExecutionOptions() {}
};

#ifndef __CUDACC__
std::ostream& operator<<(std::ostream& os, const ExecutionOptions& eo);
#endif

#endif  // QUERYENGINE_COMPILATIONOPTIONS_H
