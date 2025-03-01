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

#include "RelAlgExecutor.h"
#include "IR/TypeUtils.h"
#include "QueryEngine/CalciteDeserializerUtils.h"
#include "QueryEngine/CardinalityEstimator.h"
#include "QueryEngine/ColumnFetcher.h"
#include "QueryEngine/EquiJoinCondition.h"
#include "QueryEngine/ErrorHandling.h"
#include "QueryEngine/ExpressionRewrite.h"
#include "QueryEngine/ExtensionFunctionsBinding.h"
#include "QueryEngine/ExternalExecutor.h"
#include "QueryEngine/FromTableReordering.h"
#include "QueryEngine/MemoryLayoutBuilder.h"
#include "QueryEngine/QueryPhysicalInputsCollector.h"
#include "QueryEngine/QueryPlanDagExtractor.h"
#include "QueryEngine/RangeTableIndexVisitor.h"
#include "QueryEngine/RelAlgDagBuilder.h"
#include "QueryEngine/RelAlgTranslator.h"
#include "QueryEngine/RelAlgVisitor.h"
#include "QueryEngine/ResultSetBuilder.h"
#include "QueryEngine/WindowContext.h"
#include "SessionInfo.h"
#include "Shared/measure.h"
#include "Shared/misc.h"

#include <boost/algorithm/cxx11/any_of.hpp>
#include <boost/make_unique.hpp>
#include <boost/range/adaptor/reversed.hpp>

#include <algorithm>
#include <functional>
#include <numeric>

using namespace std::string_literals;

size_t g_estimator_failure_max_groupby_size{256000000};
bool g_columnar_large_projections{true};
size_t g_columnar_large_projections_threshold{1000000};

extern bool g_enable_table_functions;

namespace {

bool is_projection(const RelAlgExecutionUnit& ra_exe_unit) {
  return ra_exe_unit.groupby_exprs.size() == 1 && !ra_exe_unit.groupby_exprs.front();
}

bool should_output_columnar(const RelAlgExecutionUnit& ra_exe_unit) {
  if (!is_projection(ra_exe_unit)) {
    return false;
  }
  if (!ra_exe_unit.sort_info.order_entries.empty()) {
    // disable output columnar when we have top-sort node query
    return false;
  }
  for (const auto& target_expr : ra_exe_unit.target_exprs) {
    // We don't currently support varlen columnar projections, so
    // return false if we find one
    if (target_expr->type()->isString() || target_expr->type()->isArray()) {
      return false;
    }
  }

  return ra_exe_unit.scan_limit >= g_columnar_large_projections_threshold;
}

bool node_is_aggregate(const hdk::ir::Node* ra) {
  const auto compound = dynamic_cast<const hdk::ir::Compound*>(ra);
  const auto aggregate = dynamic_cast<const hdk::ir::Aggregate*>(ra);
  return ((compound && compound->isAggregate()) || aggregate);
}

bool is_extracted_dag_valid(ExtractedPlanDag& dag) {
  return !dag.contain_not_supported_rel_node &&
         dag.extracted_dag.compare(EMPTY_QUERY_PLAN) != 0;
}

class RelLeftDeepTreeIdsCollector : public RelAlgVisitor<std::vector<unsigned>> {
 public:
  std::vector<unsigned> visitLeftDeepInnerJoin(
      const hdk::ir::LeftDeepInnerJoin* left_deep_join_tree) const override {
    return {left_deep_join_tree->getId()};
  }

 protected:
  std::vector<unsigned> aggregateResult(
      const std::vector<unsigned>& aggregate,
      const std::vector<unsigned>& next_result) const override {
    auto result = aggregate;
    std::copy(next_result.begin(), next_result.end(), std::back_inserter(result));
    return result;
  }
};

}  // namespace

RelAlgExecutor::RelAlgExecutor(Executor* executor,
                               SchemaProviderPtr schema_provider,
                               DataProvider* data_provider)
    : executor_(executor)
    , schema_provider_(schema_provider)
    , data_provider_(data_provider)
    , config_(executor_->getConfig())
    , now_(0)
    , queue_time_ms_(0) {}

RelAlgExecutor::RelAlgExecutor(Executor* executor,
                               SchemaProviderPtr schema_provider,
                               DataProvider* data_provider,
                               std::unique_ptr<hdk::ir::QueryDag> query_dag)
    : executor_(executor)
    , query_dag_(std::move(query_dag))
    , schema_provider_(std::make_shared<RelAlgSchemaProvider>(*query_dag_->getRootNode()))
    , data_provider_(data_provider)
    , config_(executor_->getConfig())
    , now_(0)
    , queue_time_ms_(0) {}

size_t RelAlgExecutor::getOuterFragmentCount(const CompilationOptions& co,
                                             const ExecutionOptions& eo) {
  if (eo.find_push_down_candidates) {
    return 0;
  }

  if (eo.just_explain) {
    return 0;
  }

  CHECK(query_dag_);

  query_dag_->resetQueryExecutionState();
  const auto ra = query_dag_->getRootNode();

  ScopeGuard row_set_holder = [this] { cleanupPostExecution(); };
  const auto col_descs = get_physical_inputs(ra);
  const auto phys_table_ids = get_physical_table_inputs(ra);
  executor_->setSchemaProvider(schema_provider_);
  executor_->setupCaching(data_provider_, col_descs, phys_table_ids);

  ScopeGuard restore_metainfo_cache = [this] { executor_->clearMetaInfoCache(); };
  auto ed_seq = RaExecutionSequence(ra);

  if (!getSubqueries().empty()) {
    return 0;
  }

  CHECK(!ed_seq.empty());
  if (ed_seq.size() > 1) {
    return 0;
  }

  decltype(temporary_tables_)().swap(temporary_tables_);
  decltype(target_exprs_owned_)().swap(target_exprs_owned_);
  executor_->setSchemaProvider(schema_provider_);
  executor_->temporary_tables_ = &temporary_tables_;

  WindowProjectNodeContext::reset(executor_);
  auto exec_desc_ptr = ed_seq.getDescriptor(0);
  CHECK(exec_desc_ptr);
  auto& exec_desc = *exec_desc_ptr;
  const auto body = exec_desc.getBody();
  if (body->isNop()) {
    return 0;
  }

  const auto project = dynamic_cast<const hdk::ir::Project*>(body);
  if (project) {
    auto work_unit =
        createProjectWorkUnit(project, {{}, SortAlgorithm::Default, 0, 0}, eo);

    return get_frag_count_of_table(work_unit.exe_unit.input_descs[0].getDatabaseId(),
                                   work_unit.exe_unit.input_descs[0].getTableId(),
                                   executor_);
  }

  const auto compound = dynamic_cast<const hdk::ir::Compound*>(body);
  if (compound) {
    if (compound->isAggregate()) {
      return 0;
    }

    const auto work_unit =
        createCompoundWorkUnit(compound, {{}, SortAlgorithm::Default, 0, 0}, eo);

    return get_frag_count_of_table(work_unit.exe_unit.input_descs[0].getDatabaseId(),
                                   work_unit.exe_unit.input_descs[0].getTableId(),
                                   executor_);
  }

  return 0;
}

ExecutionResult RelAlgExecutor::executeRelAlgQuery(const CompilationOptions& co,
                                                   const ExecutionOptions& eo,
                                                   const bool just_explain_plan) {
  CHECK(query_dag_);
  auto timer = DEBUG_TIMER(__func__);
  INJECT_TIMER(executeRelAlgQuery);

  auto run_query = [&](const CompilationOptions& co_in) {
    auto execution_result = executeRelAlgQueryNoRetry(co_in, eo, just_explain_plan);

    constexpr bool vlog_result_set_summary{false};
    if constexpr (vlog_result_set_summary) {
      VLOG(1) << execution_result.getRows()->summaryToString();
    }

    if (post_execution_callback_) {
      VLOG(1) << "Running post execution callback.";
      (*post_execution_callback_)();
    }
    return execution_result;
  };

  try {
    return run_query(co);
  } catch (const QueryMustRunOnCpu&) {
    if (!config_.exec.heterogeneous.allow_cpu_retry) {
      throw;
    }
  }
  LOG(INFO) << "Query unable to run in GPU mode, retrying on CPU";
  auto co_cpu = CompilationOptions::makeCpuOnly(co);

  return run_query(co_cpu);
}

std::pair<CompilationOptions, ExecutionOptions> RelAlgExecutor::handle_hint(
    const CompilationOptions& co,
    const ExecutionOptions& eo,
    const hdk::ir::Node* body) {
  ExecutionOptions eo_hint_applied = eo;
  CompilationOptions co_hint_applied = co;
  auto target_node = body;
  if (auto sort_body = dynamic_cast<const hdk::ir::Sort*>(body)) {
    target_node = sort_body->getInput(0);
  }
  auto query_hints = getParsedQueryHint(target_node);
  auto columnar_output_hint_enabled = false;
  auto rowwise_output_hint_enabled = false;
  if (query_hints) {
    if (query_hints->isHintRegistered(QueryHint::kCpuMode)) {
      VLOG(1) << "A user forces to run the query on the CPU execution mode";
      co_hint_applied.device_type = ExecutorDeviceType::CPU;
    }
    if (query_hints->isHintRegistered(QueryHint::kColumnarOutput)) {
      VLOG(1) << "A user forces the query to run with columnar output";
      columnar_output_hint_enabled = true;
    } else if (query_hints->isHintRegistered(QueryHint::kRowwiseOutput)) {
      VLOG(1) << "A user forces the query to run with rowwise output";
      rowwise_output_hint_enabled = true;
    }
  }
  auto columnar_output_enabled = eo.output_columnar_hint ? !rowwise_output_hint_enabled
                                                         : columnar_output_hint_enabled;
  if (columnar_output_hint_enabled || rowwise_output_hint_enabled) {
    LOG(INFO) << "Currently, we do not support applying query hint to change query "
                 "output layout in distributed mode.";
  }
  eo_hint_applied.output_columnar_hint = columnar_output_enabled;
  return std::make_pair(co_hint_applied, eo_hint_applied);
}

void RelAlgExecutor::prepareStreamingExecution(const CompilationOptions& co,
                                               const ExecutionOptions& eo) {
  query_dag_->resetQueryExecutionState();
  const auto ra = query_dag_->getRootNode();
  if (config_.exec.watchdog.enable_dynamic) {
    executor_->resetInterrupt();
  }

  //  ScopeGuard row_set_holder = [this] { cleanupPostExecution(); };
  const auto col_descs = get_physical_inputs(ra);
  const auto phys_table_ids = get_physical_table_inputs(ra);

  decltype(temporary_tables_)().swap(temporary_tables_);
  decltype(target_exprs_owned_)().swap(target_exprs_owned_);
  decltype(left_deep_join_info_)().swap(left_deep_join_info_);

  executor_->setSchemaProvider(schema_provider_);
  executor_->setupCaching(data_provider_, col_descs, phys_table_ids);
  executor_->temporary_tables_ = &temporary_tables_;

  //   ScopeGuard restore_metainfo_cache = [this] { executor_->clearMetaInfoCache(); };
  auto ed_seq = RaExecutionSequence(ra);

  if (getSubqueries().size() != 0) {
    throw std::runtime_error("Streaming queries with subqueries are not supported yet");
  }

  if (ed_seq.size() != 1) {
    throw std::runtime_error("Multistep streaming queries are not supported yet");
  }

  auto exec_desc_ptr = ed_seq.getDescriptor(0);
  CHECK(exec_desc_ptr);
  auto& exec_desc = *exec_desc_ptr;
  const auto body = exec_desc.getBody();

  const ExecutionOptions eo_work_unit{eo};

  auto [co_hint_applied, eo_hint_applied] = handle_hint(co, eo_work_unit, body);

  auto work_unit = createWorkUnitForStreaming(body, co, eo);

  auto ra_exe_unit = work_unit.exe_unit;
  ra_exe_unit.query_hint = RegisteredQueryHint::fromConfig(config_);
  auto candidate = query_dag_->getQueryHint(body);
  if (candidate) {
    ra_exe_unit.query_hint = *candidate;
  }
  auto column_cache = std::make_unique<ColumnCacheMap>();

  auto table_infos = get_table_infos(work_unit.exe_unit, executor_);

  stream_execution_context_ = executor_->prepareStreamingExecution(ra_exe_unit,
                                                                   co_hint_applied,
                                                                   eo_hint_applied,
                                                                   table_infos,
                                                                   data_provider_,
                                                                   *column_cache);

  stream_execution_context_->column_cache = std::move(column_cache);
  stream_execution_context_->is_agg = node_is_aggregate(body);
}

RelAlgExecutor::WorkUnit RelAlgExecutor::createWorkUnitForStreaming(
    const hdk::ir::Node* body,
    const CompilationOptions& co,
    const ExecutionOptions& eo) {
  if (const auto compound = dynamic_cast<const hdk::ir::Compound*>(body)) {
    return createCompoundWorkUnit(compound, {{}, SortAlgorithm::Default, 0, 0}, eo);
  }
  if (const auto project = dynamic_cast<const hdk::ir::Project*>(body)) {
    auto work_unit =
        createProjectWorkUnit(project, {{}, SortAlgorithm::Default, 0, 0}, eo);
    CHECK(!project->isSimple());  // check if input table is not temporary
    return work_unit;
  }
  if (const auto aggregate = dynamic_cast<const hdk::ir::Aggregate*>(body)) {
    return createAggregateWorkUnit(
        aggregate, {{}, SortAlgorithm::Default, 0, 0}, eo.just_explain);
  }
  if (const auto filter = dynamic_cast<const hdk::ir::Filter*>(body)) {
    return createFilterWorkUnit(
        filter, {{}, SortAlgorithm::Default, 0, 0}, eo.just_explain);
  }

  throw std::runtime_error("that query type is not supported in streaming mode");
}

ResultSetPtr RelAlgExecutor::runOnBatch(const FragmentsPerTable& fragments) {
  FragmentsList fl{fragments};
  return executor_->runOnBatch(stream_execution_context_, fl);
}

ResultSetPtr RelAlgExecutor::finishStreamingExecution() {
  return executor_->finishStreamExecution(stream_execution_context_);
}

ExecutionResult RelAlgExecutor::executeRelAlgQueryNoRetry(const CompilationOptions& co,
                                                          const ExecutionOptions& eo,
                                                          const bool just_explain_plan) {
  INJECT_TIMER(executeRelAlgQueryNoRetry);
  auto timer = DEBUG_TIMER(__func__);
  auto timer_setup = DEBUG_TIMER("Query pre-execution steps");

  query_dag_->resetQueryExecutionState();
  const auto ra = query_dag_->getRootNode();

  // capture the lock acquistion time
  auto clock_begin = timer_start();
  if (config_.exec.watchdog.enable_dynamic) {
    executor_->resetInterrupt();
  }

  int64_t queue_time_ms = timer_stop(clock_begin);
  ScopeGuard row_set_holder = [this] { cleanupPostExecution(); };
  const auto col_descs = get_physical_inputs(ra);
  const auto phys_table_ids = get_physical_table_inputs(ra);
  executor_->setSchemaProvider(schema_provider_);
  executor_->setupCaching(data_provider_, col_descs, phys_table_ids);

  ScopeGuard restore_metainfo_cache = [this] { executor_->clearMetaInfoCache(); };
  auto ed_seq = RaExecutionSequence(ra);

  if (just_explain_plan) {
    std::stringstream ss;
    std::vector<const hdk::ir::Node*> nodes;
    for (size_t i = 0; i < ed_seq.size(); i++) {
      nodes.emplace_back(ed_seq.getDescriptor(i)->getBody());
    }
    size_t ctr = nodes.size();
    size_t tab_ctr = 0;
    for (auto& body : boost::adaptors::reverse(nodes)) {
      const auto index = ctr--;
      const auto tabs = std::string(tab_ctr++, '\t');
      CHECK(body);
      ss << tabs << std::to_string(index) << " : " << body->toString() << "\n";
      if (auto sort = dynamic_cast<const hdk::ir::Sort*>(body)) {
        ss << tabs << "  : " << sort->getInput(0)->toString() << "\n";
      }
      if (dynamic_cast<const hdk::ir::Project*>(body) ||
          dynamic_cast<const hdk::ir::Compound*>(body)) {
        if (auto join =
                dynamic_cast<const hdk::ir::LeftDeepInnerJoin*>(body->getInput(0))) {
          ss << tabs << "  : " << join->toString() << "\n";
        }
      }
    }
    const auto& subqueries = getSubqueries();
    if (!subqueries.empty()) {
      ss << "Subqueries: "
         << "\n";
      for (const auto& subquery : subqueries) {
        const auto ra = subquery->node();
        ss << "\t" << ra->toString() << "\n";
      }
    }
    auto rs = std::make_shared<ResultSet>(ss.str());
    return {rs, {}};
  }

  if (eo.find_push_down_candidates) {
    // this extra logic is mainly due to current limitations on multi-step queries
    // and/or subqueries.
    return executeRelAlgQueryWithFilterPushDown(ed_seq, co, eo, queue_time_ms);
  }
  timer_setup.stop();

  // Dispatch the subqueries first
  for (auto& subquery : getSubqueries()) {
    auto subquery_ra = subquery->node();
    CHECK(subquery_ra);
    if (subquery_ra->hasContextData()) {
      continue;
    }
    // Execute the subquery and cache the result.
    RelAlgExecutor ra_executor(executor_, schema_provider_, data_provider_);
    RaExecutionSequence subquery_seq(subquery_ra);
    auto result = ra_executor.executeRelAlgSeq(subquery_seq, co, eo, 0);
    auto shared_result = std::make_shared<ExecutionResult>(std::move(result));
    subquery_ra->setResult(shared_result);
  }
  return executeRelAlgSeq(ed_seq, co, eo, queue_time_ms);
}

AggregatedColRange RelAlgExecutor::computeColRangesCache() {
  AggregatedColRange agg_col_range_cache;
  const auto col_descs = get_physical_inputs(getRootNode());
  return executor_->computeColRangesCache(col_descs);
}

StringDictionaryGenerations RelAlgExecutor::computeStringDictionaryGenerations() {
  const auto col_descs = get_physical_inputs(getRootNode());
  return executor_->computeStringDictionaryGenerations(col_descs);
}

TableGenerations RelAlgExecutor::computeTableGenerations() {
  const auto phys_table_ids = get_physical_table_inputs(getRootNode());
  return executor_->computeTableGenerations(phys_table_ids);
}

Executor* RelAlgExecutor::getExecutor() const {
  return executor_;
}

void RelAlgExecutor::cleanupPostExecution() {
  CHECK(executor_);
  executor_->row_set_mem_owner_ = nullptr;
}

std::pair<std::vector<unsigned>, std::unordered_map<unsigned, JoinQualsPerNestingLevel>>
RelAlgExecutor::getJoinInfo(const hdk::ir::Node* root_node) {
  auto sort_node = dynamic_cast<const hdk::ir::Sort*>(root_node);
  if (sort_node) {
    // we assume that test query that needs join info does not contain any sort node
    return {};
  }
  auto work_unit = createWorkUnit(root_node, {}, ExecutionOptions::fromConfig(Config()));
  RelLeftDeepTreeIdsCollector visitor;
  auto left_deep_tree_ids = visitor.visit(root_node);
  return {left_deep_tree_ids, getLeftDeepJoinTreesInfo()};
}

namespace {

inline void check_sort_node_source_constraint(const hdk::ir::Sort* sort) {
  CHECK_EQ(size_t(1), sort->inputCount());
  const auto source = sort->getInput(0);
  if (dynamic_cast<const hdk::ir::Sort*>(source)) {
    throw std::runtime_error("Sort node not supported as input to another sort");
  }
}

}  // namespace

QueryStepExecutionResult RelAlgExecutor::executeRelAlgQuerySingleStep(
    const RaExecutionSequence& seq,
    const size_t step_idx,
    const CompilationOptions& co,
    const ExecutionOptions& eo) {
  INJECT_TIMER(executeRelAlgQueryStep);

  auto exe_desc_ptr = seq.getDescriptor(step_idx);
  CHECK(exe_desc_ptr);
  const auto sort = dynamic_cast<const hdk::ir::Sort*>(exe_desc_ptr->getBody());

  auto merge_type = [](const hdk::ir::Node* body) -> MergeType {
    return node_is_aggregate(body) ? MergeType::Reduce : MergeType::Union;
  };

  if (sort) {
    check_sort_node_source_constraint(sort);
    const auto source_work_unit = createSortInputWorkUnit(sort, eo);
    // No point in sorting on the leaf, only execute the input to the sort node.
    CHECK_EQ(size_t(1), sort->inputCount());
    const auto source = sort->getInput(0);
    if (sort->collationCount() || node_is_aggregate(source)) {
      auto temp_seq = RaExecutionSequence(std::make_unique<RaExecutionDesc>(source));
      CHECK_EQ(temp_seq.size(), size_t(1));
      const ExecutionOptions eo_copy = [&]() {
        ExecutionOptions copy = eo;
        copy.just_validate = eo.just_validate || sort->isEmptyResult();
        return copy;
      }();

      // Use subseq to avoid clearing existing temporary tables
      return {executeRelAlgSubSeq(temp_seq, std::make_pair(0, 1), co, eo_copy, 0),
              merge_type(source),
              source->getId(),
              false};
    }
  }
  QueryStepExecutionResult result{
      executeRelAlgSubSeq(
          seq, std::make_pair(step_idx, step_idx + 1), co, eo, queue_time_ms_),
      merge_type(exe_desc_ptr->getBody()),
      exe_desc_ptr->getBody()->getId(),
      false};
  if (post_execution_callback_) {
    VLOG(1) << "Running post execution callback.";
    (*post_execution_callback_)();
  }
  return result;
}

void RelAlgExecutor::prepareLeafExecution(
    const AggregatedColRange& agg_col_range,
    const StringDictionaryGenerations& string_dictionary_generations,
    const TableGenerations& table_generations) {
  // capture the lock acquistion time
  auto clock_begin = timer_start();
  if (config_.exec.watchdog.enable_dynamic) {
    executor_->resetInterrupt();
  }
  queue_time_ms_ = timer_stop(clock_begin);
  executor_->row_set_mem_owner_ = std::make_shared<RowSetMemoryOwner>(
      data_provider_, Executor::getArenaBlockSize(), cpu_threads());
  executor_->row_set_mem_owner_->setDictionaryGenerations(string_dictionary_generations);
  executor_->table_generations_ = table_generations;
  executor_->agg_col_range_cache_ = agg_col_range;
}

ExecutionResult RelAlgExecutor::executeRelAlgSeq(const RaExecutionSequence& seq,
                                                 const CompilationOptions& co,
                                                 const ExecutionOptions& eo,
                                                 const int64_t queue_time_ms,
                                                 const bool with_existing_temp_tables) {
  INJECT_TIMER(executeRelAlgSeq);
  auto timer = DEBUG_TIMER(__func__);
  if (!with_existing_temp_tables) {
    decltype(temporary_tables_)().swap(temporary_tables_);
  }
  decltype(target_exprs_owned_)().swap(target_exprs_owned_);
  decltype(left_deep_join_info_)().swap(left_deep_join_info_);
  executor_->setSchemaProvider(schema_provider_);
  executor_->temporary_tables_ = &temporary_tables_;

  time(&now_);
  CHECK(!seq.empty());

  auto get_descriptor_count = [&seq, &eo]() -> size_t {
    if (eo.just_explain) {
      if (dynamic_cast<const hdk::ir::LogicalValues*>(seq.getDescriptor(0)->getBody())) {
        // run the logical values descriptor to generate the result set, then the next
        // descriptor to generate the explain
        CHECK_GE(seq.size(), size_t(2));
        return 2;
      } else {
        return 1;
      }
    } else {
      return seq.size();
    }
  };

  const auto exec_desc_count = get_descriptor_count();
  // this join info needs to be maintained throughout an entire query runtime
  for (size_t i = 0; i < exec_desc_count; i++) {
    VLOG(1) << "Executing query step " << i;
    try {
      executeRelAlgStep(seq, i, co, eo, queue_time_ms);
    } catch (const QueryMustRunOnCpu&) {
      // Do not allow per-step retry if flag is off or in distributed mode
      // TODO(todd): Determine if and when we can relax this restriction
      // for distributed
      CHECK(co.device_type == ExecutorDeviceType::GPU);
      if (!config_.exec.heterogeneous.allow_query_step_cpu_retry) {
        throw;
      }
      LOG(INFO) << "Retrying current query step " << i << " on CPU";
      const auto co_cpu = CompilationOptions::makeCpuOnly(co);
      executeRelAlgStep(seq, i, co_cpu, eo, queue_time_ms);
    } catch (const NativeExecutionError&) {
      if (!config_.exec.enable_interop) {
        throw;
      }
      auto eo_extern = eo;
      eo_extern.executor_type = ::ExecutorType::Extern;
      auto exec_desc_ptr = seq.getDescriptor(i);
      const auto body = exec_desc_ptr->getBody();
      const auto compound = dynamic_cast<const hdk::ir::Compound*>(body);
      if (compound && (compound->getGroupByCount() || compound->isAggregate())) {
        LOG(INFO) << "Also failed to run the query using interoperability";
        throw;
      }
      executeRelAlgStep(seq, i, co, eo_extern, queue_time_ms);
    }
  }

  return seq.getDescriptor(exec_desc_count - 1)->getResult();
}

ExecutionResult RelAlgExecutor::executeRelAlgSubSeq(
    const RaExecutionSequence& seq,
    const std::pair<size_t, size_t> interval,
    const CompilationOptions& co,
    const ExecutionOptions& eo,
    const int64_t queue_time_ms) {
  INJECT_TIMER(executeRelAlgSubSeq);
  executor_->setSchemaProvider(schema_provider_);
  executor_->temporary_tables_ = &temporary_tables_;
  decltype(left_deep_join_info_)().swap(left_deep_join_info_);
  time(&now_);
  for (size_t i = interval.first; i < interval.second; i++) {
    try {
      executeRelAlgStep(seq, i, co, eo, queue_time_ms);
    } catch (const QueryMustRunOnCpu&) {
      // Do not allow per-step retry if flag is off or in distributed mode
      // TODO(todd): Determine if and when we can relax this restriction
      // for distributed
      CHECK(co.device_type == ExecutorDeviceType::GPU);
      if (!config_.exec.heterogeneous.allow_query_step_cpu_retry) {
        throw;
      }
      LOG(INFO) << "Retrying current query step " << i << " on CPU";
      const auto co_cpu = CompilationOptions::makeCpuOnly(co);
      executeRelAlgStep(seq, i, co_cpu, eo, queue_time_ms);
    }
  }

  return seq.getDescriptor(interval.second - 1)->getResult();
}

void RelAlgExecutor::executeRelAlgStep(const RaExecutionSequence& seq,
                                       const size_t step_idx,
                                       const CompilationOptions& co,
                                       const ExecutionOptions& eo,
                                       const int64_t queue_time_ms) {
  INJECT_TIMER(executeRelAlgStep);
  auto timer = DEBUG_TIMER(__func__);
  WindowProjectNodeContext::reset(executor_);
  auto exec_desc_ptr = seq.getDescriptor(step_idx);
  CHECK(exec_desc_ptr);
  auto& exec_desc = *exec_desc_ptr;
  const auto body = exec_desc.getBody();
  if (body->isNop()) {
    handleNop(exec_desc);
    return;
  }

  const auto eo_work_unit = [&]() {
    ExecutionOptions new_eo = eo;
    new_eo.with_watchdog = eo.with_watchdog &&
                           (step_idx == 0 || dynamic_cast<const hdk::ir::Project*>(body));
    new_eo.outer_fragment_indices =
        step_idx == 0 ? eo.outer_fragment_indices : std::vector<size_t>();
    return new_eo;
  }();

  auto hint_applied = handle_hint(co, eo_work_unit, body);
  const auto compound = dynamic_cast<const hdk::ir::Compound*>(body);
  if (compound) {
    exec_desc.setResult(executeCompound(
        compound, hint_applied.first, hint_applied.second, queue_time_ms));
    VLOG(3) << "Returned from executeCompound(), addTemporaryTable("
            << static_cast<int>(-compound->getId()) << ", ...)"
            << " exec_desc.getResult().getDataPtr()->rowCount()="
            << exec_desc.getResult().getDataPtr()->rowCount();
    if (exec_desc.getResult().isFilterPushDownEnabled()) {
      return;
    }
    addTemporaryTable(-compound->getId(), exec_desc.getResult().getDataPtr());
    return;
  }
  const auto project = dynamic_cast<const hdk::ir::Project*>(body);
  if (project) {
    std::optional<size_t> prev_count;
    // Disabling the intermediate count optimization in distributed, as the previous
    // execution descriptor will likely not hold the aggregated result.
    if (config_.opts.skip_intermediate_count && step_idx > 0) {
      // If the previous node produced a reliable count, skip the pre-flight count.
      hdk::ir::Node const* const prev_body = project->getInput(0);
      if (shared::dynamic_castable_to_any<hdk::ir::Compound, hdk::ir::LogicalValues>(
              prev_body)) {
        if (RaExecutionDesc const* const prev_exec_desc =
                prev_body->hasContextData()
                    ? prev_body->getContextData()
                    : seq.getDescriptorByBodyId(prev_body->getId(), step_idx - 1)) {
          const auto& prev_exe_result = prev_exec_desc->getResult();
          const auto prev_result = prev_exe_result.getRows();
          if (prev_result) {
            prev_count = prev_result->rowCount();
            VLOG(3) << "Setting output row count for projection node to previous node ("
                    << prev_exec_desc->getBody()->toString() << ") to " << *prev_count;
          }
        }
      }
    }
    // For intermediate results we want to keep the result fragmented
    // to have higher parallelism on next steps.
    bool multifrag_result =
        config_.exec.enable_multifrag_rs && (step_idx != seq.size() - 1);
    exec_desc.setResult(
        executeProject(project,
                       co,
                       eo_work_unit.with_multifrag_result(multifrag_result),
                       queue_time_ms,
                       prev_count));
    if (exec_desc.getResult().isFilterPushDownEnabled()) {
      return;
    }
    addTemporaryTable(-project->getId(), exec_desc.getResult().getTable());
    return;
  }
  const auto aggregate = dynamic_cast<const hdk::ir::Aggregate*>(body);
  if (aggregate) {
    exec_desc.setResult(executeAggregate(
        aggregate, hint_applied.first, hint_applied.second, queue_time_ms));
    addTemporaryTable(-aggregate->getId(), exec_desc.getResult().getDataPtr());
    return;
  }
  const auto filter = dynamic_cast<const hdk::ir::Filter*>(body);
  if (filter) {
    exec_desc.setResult(
        executeFilter(filter, hint_applied.first, hint_applied.second, queue_time_ms));
    addTemporaryTable(-filter->getId(), exec_desc.getResult().getDataPtr());
    return;
  }
  const auto sort = dynamic_cast<const hdk::ir::Sort*>(body);
  if (sort) {
    exec_desc.setResult(
        executeSort(sort, hint_applied.first, hint_applied.second, queue_time_ms));
    if (exec_desc.getResult().isFilterPushDownEnabled()) {
      return;
    }
    addTemporaryTable(-sort->getId(), exec_desc.getResult().getDataPtr());
    return;
  }
  const auto logical_values = dynamic_cast<const hdk::ir::LogicalValues*>(body);
  if (logical_values) {
    exec_desc.setResult(executeLogicalValues(logical_values, hint_applied.second));
    addTemporaryTable(-logical_values->getId(), exec_desc.getResult().getDataPtr());
    return;
  }
  const auto logical_union = dynamic_cast<const hdk::ir::LogicalUnion*>(body);
  if (logical_union) {
    exec_desc.setResult(executeUnion(
        logical_union, seq, co, eo_work_unit.with_preserve_order(true), queue_time_ms));
    addTemporaryTable(-logical_union->getId(), exec_desc.getResult().getDataPtr());
    return;
  }
  const auto table_func = dynamic_cast<const hdk::ir::TableFunction*>(body);
  if (table_func) {
    exec_desc.setResult(executeTableFunction(
        table_func, hint_applied.first, hint_applied.second, queue_time_ms));
    addTemporaryTable(-table_func->getId(), exec_desc.getResult().getDataPtr());
    return;
  }
  LOG(FATAL) << "Unhandled body type: " << body->toString();
}

void RelAlgExecutor::handleNop(RaExecutionDesc& ed) {
  // just set the result of the previous node as the result of no op
  auto body = ed.getBody();
  CHECK(dynamic_cast<const hdk::ir::Aggregate*>(body));
  CHECK_EQ(size_t(1), body->inputCount());
  const auto input = body->getInput(0);
  body->setOutputMetainfo(input->getOutputMetainfo());
  const auto it = temporary_tables_.find(-input->getId());
  CHECK(it != temporary_tables_.end());

  CHECK_EQ(it->second.getFragCount(), 1);
  ed.setResult({it->second.getResultSet(0), input->getOutputMetainfo()});

  // set up temp table as it could be used by the outer query or next step
  addTemporaryTable(-body->getId(), it->second);
}

namespace {

struct ColumnRefHash {
  size_t operator()(const hdk::ir::ColumnRef& col_ref) const { return col_ref.hash(); }
};

using ColumnRefSet = std::unordered_set<hdk::ir::ColumnRef, ColumnRefHash>;

class UsedInputsCollector
    : public hdk::ir::ExprCollector<ColumnRefSet, UsedInputsCollector> {
 protected:
  void visitColumnRef(const hdk::ir::ColumnRef* col_ref) override {
    result_.insert(*col_ref);
  }
};

const hdk::ir::Node* get_data_sink(const hdk::ir::Node* ra_node) {
  if (auto table_func = dynamic_cast<const hdk::ir::TableFunction*>(ra_node)) {
    return table_func;
  }
  if (auto join = dynamic_cast<const hdk::ir::Join*>(ra_node)) {
    CHECK_EQ(size_t(2), join->inputCount());
    return join;
  }
  if (!dynamic_cast<const hdk::ir::LogicalUnion*>(ra_node)) {
    CHECK_EQ(size_t(1), ra_node->inputCount());
  }
  auto only_src = ra_node->getInput(0);
  const bool is_join = dynamic_cast<const hdk::ir::Join*>(only_src) ||
                       dynamic_cast<const hdk::ir::LeftDeepInnerJoin*>(only_src);
  return is_join ? only_src : ra_node;
}

ColumnRefSet get_used_inputs(const hdk::ir::Compound* compound) {
  UsedInputsCollector collector;
  const auto filter_expr = compound->getFilter();
  if (filter_expr) {
    collector.visit(filter_expr.get());
  }
  for (auto& expr : compound->getGroupByExprs()) {
    collector.visit(expr.get());
  }
  for (auto& expr : compound->getExprs()) {
    collector.visit(expr.get());
  }
  return std::move(collector.result());
}

ColumnRefSet get_used_inputs(const hdk::ir::Aggregate* aggregate) {
  UsedInputsCollector collector;
  ColumnRefSet res;
  const auto source = aggregate->getInput(0);
  const auto& in_metainfo = source->getOutputMetainfo();
  const auto group_count = aggregate->getGroupByCount();
  CHECK_GE(in_metainfo.size(), group_count);
  for (unsigned i = 0; i < static_cast<unsigned>(group_count); ++i) {
    res.insert({getColumnType(source, i), source, i});
  }
  for (const auto& expr : aggregate->getAggs()) {
    auto agg_expr = dynamic_cast<const hdk::ir::AggExpr*>(expr.get());
    CHECK(agg_expr);
    if (agg_expr->arg()) {
      collector.visit(agg_expr->arg());
    }
  }
  res.insert(collector.result().begin(), collector.result().end());
  return res;
}

ColumnRefSet get_used_inputs(const hdk::ir::Project* project) {
  UsedInputsCollector collector;
  for (auto& expr : project->getExprs()) {
    collector.visit(expr.get());
  }
  return std::move(collector.result());
}

ColumnRefSet get_used_inputs(const hdk::ir::TableFunction* table_func) {
  UsedInputsCollector collector;
  for (auto& expr : table_func->getTableFuncInputExprs()) {
    collector.visit(expr.get());
  }
  return std::move(collector.result());
}

ColumnRefSet get_used_inputs(const hdk::ir::Filter* filter) {
  ColumnRefSet res;
  const auto data_sink_node = get_data_sink(filter);
  for (size_t nest_level = 0; nest_level < data_sink_node->inputCount(); ++nest_level) {
    const auto source = data_sink_node->getInput(nest_level);
    const auto scan_source = dynamic_cast<const hdk::ir::Scan*>(source);
    auto input_count =
        scan_source ? scan_source->size() : source->getOutputMetainfo().size();
    for (unsigned i = 0; i < static_cast<unsigned>(input_count); ++i) {
      res.insert({getColumnType(source, i), source, i});
    }
  }
  return res;
}

ColumnRefSet get_used_inputs(const hdk::ir::LogicalUnion* logical_union) {
  ColumnRefSet res;
  auto const n_inputs = logical_union->inputCount();
  for (size_t nest_level = 0; nest_level < n_inputs; ++nest_level) {
    auto input = logical_union->getInput(nest_level);
    for (unsigned i = 0; i < static_cast<unsigned>(input->size()); ++i) {
      res.insert({getColumnType(input, i), input, i});
    }
  }
  return res;
}

int db_id_from_ra(const hdk::ir::Node* ra_node) {
  const auto scan_ra = dynamic_cast<const hdk::ir::Scan*>(ra_node);
  if (scan_ra) {
    return scan_ra->getDatabaseId();
  }
  return 0;
}

int table_id_from_ra(const hdk::ir::Node* ra_node) {
  const auto scan_ra = dynamic_cast<const hdk::ir::Scan*>(ra_node);
  if (scan_ra) {
    return scan_ra->getTableId();
  }
  return -ra_node->getId();
}

std::unordered_map<const hdk::ir::Node*, int> get_input_nest_levels(
    const hdk::ir::Node* ra_node,
    const std::vector<size_t>& input_permutation) {
  const auto data_sink_node = get_data_sink(ra_node);
  std::unordered_map<const hdk::ir::Node*, int> input_to_nest_level;
  for (size_t input_idx = 0; input_idx < data_sink_node->inputCount(); ++input_idx) {
    const auto input_node_idx =
        input_permutation.empty() ? input_idx : input_permutation[input_idx];
    const auto input_ra = data_sink_node->getInput(input_node_idx);
    // Having a non-zero mapped value (input_idx) results in the query being
    // interpretted as a JOIN within CodeGenerator::codegenColVar() due to rte_idx
    // being set to the mapped value (input_idx) which originates here. This would be
    // incorrect for UNION.
    size_t const idx =
        dynamic_cast<const hdk::ir::LogicalUnion*>(ra_node) ? 0 : input_idx;
    const auto it_ok = input_to_nest_level.emplace(input_ra, idx);
    CHECK(it_ok.second);
    LOG_IF(INFO, !input_permutation.empty())
        << "Assigned input " << input_ra->toString() << " to nest level " << input_idx;
  }
  return input_to_nest_level;
}

ColumnRefSet get_join_source_used_inputs(const hdk::ir::Node* ra_node) {
  const auto data_sink_node = get_data_sink(ra_node);
  if (auto join = dynamic_cast<const hdk::ir::Join*>(data_sink_node)) {
    CHECK_EQ(join->inputCount(), 2u);
    const auto condition = join->getCondition();
    return UsedInputsCollector::collect(condition);
  }

  if (auto left_deep_join =
          dynamic_cast<const hdk::ir::LeftDeepInnerJoin*>(data_sink_node)) {
    CHECK_GE(left_deep_join->inputCount(), 2u);
    UsedInputsCollector collector;
    collector.visit(left_deep_join->getInnerCondition());
    for (size_t nesting_level = 1; nesting_level <= left_deep_join->inputCount() - 1;
         ++nesting_level) {
      const auto outer_condition = left_deep_join->getOuterCondition(nesting_level);
      if (outer_condition) {
        collector.visit(outer_condition);
      }
    }
    return std::move(collector.result());
  }

  if (dynamic_cast<const hdk::ir::LogicalUnion*>(ra_node)) {
    CHECK_GT(ra_node->inputCount(), 1u) << ra_node->toString();
  } else if (dynamic_cast<const hdk::ir::TableFunction*>(ra_node)) {
    // no-op
    CHECK_GE(ra_node->inputCount(), 0u) << ra_node->toString();
  } else {
    CHECK_EQ(ra_node->inputCount(), 1u) << ra_node->toString();
  }
  return {};
}

void collect_used_input_desc(
    std::vector<InputDescriptor>& input_descs,
    std::unordered_set<std::shared_ptr<const InputColDescriptor>>& input_col_descs_unique,
    const hdk::ir::Node* ra_node,
    const ColumnRefSet& source_used_inputs,
    const std::unordered_map<const hdk::ir::Node*, int>& input_to_nest_level) {
  for (const auto col_ref : source_used_inputs) {
    const auto source = col_ref.node();
    const int table_id = table_id_from_ra(source);
    const auto col_id = col_ref.index();
    auto it = input_to_nest_level.find(source);
    if (it != input_to_nest_level.end()) {
      const int nest_level = it->second;
      auto scan = dynamic_cast<const hdk::ir::Scan*>(source);
      ColumnInfoPtr col_info = scan
                                   ? scan->getColumnInfo(col_id)
                                   : std::make_shared<ColumnInfo>(
                                         -1, table_id, col_id, "", col_ref.type(), false);
      input_col_descs_unique.insert(
          std::make_shared<const InputColDescriptor>(col_info, nest_level));
    } else if (!dynamic_cast<const hdk::ir::LogicalUnion*>(ra_node)) {
      throw std::runtime_error("Bushy joins not supported");
    }
  }
}

template <class RA>
std::pair<std::vector<InputDescriptor>,
          std::list<std::shared_ptr<const InputColDescriptor>>>
get_input_desc(const RA* ra_node,
               const std::unordered_map<const hdk::ir::Node*, int>& input_to_nest_level,
               const std::vector<size_t>& input_permutation) {
  auto used_inputs = get_used_inputs(ra_node);
  std::vector<InputDescriptor> input_descs;
  const auto data_sink_node = get_data_sink(ra_node);
  for (size_t input_idx = 0; input_idx < data_sink_node->inputCount(); ++input_idx) {
    const auto input_node_idx =
        input_permutation.empty() ? input_idx : input_permutation[input_idx];
    auto input_ra = data_sink_node->getInput(input_node_idx);
    const int db_id = db_id_from_ra(input_ra);
    const int table_id = table_id_from_ra(input_ra);
    input_descs.emplace_back(db_id, table_id, input_idx);
  }
  std::sort(input_descs.begin(),
            input_descs.end(),
            [](const InputDescriptor& lhs, const InputDescriptor& rhs) {
              return lhs.getNestLevel() < rhs.getNestLevel();
            });
  std::unordered_set<std::shared_ptr<const InputColDescriptor>> input_col_descs_unique;
  collect_used_input_desc(input_descs,
                          input_col_descs_unique,  // modified
                          ra_node,
                          used_inputs,
                          input_to_nest_level);
  auto join_source_used_inputs = get_join_source_used_inputs(ra_node);
  collect_used_input_desc(input_descs,
                          input_col_descs_unique,  // modified
                          ra_node,
                          join_source_used_inputs,
                          input_to_nest_level);
  std::vector<std::shared_ptr<const InputColDescriptor>> input_col_descs(
      input_col_descs_unique.begin(), input_col_descs_unique.end());

  std::sort(
      input_col_descs.begin(),
      input_col_descs.end(),
      [](std::shared_ptr<const InputColDescriptor> const& lhs,
         std::shared_ptr<const InputColDescriptor> const& rhs) {
        return std::make_tuple(lhs->getNestLevel(), lhs->getColId(), lhs->getTableId()) <
               std::make_tuple(rhs->getNestLevel(), rhs->getColId(), rhs->getTableId());
      });
  return {input_descs,
          std::list<std::shared_ptr<const InputColDescriptor>>(input_col_descs.begin(),
                                                               input_col_descs.end())};
}

hdk::ir::ExprPtr set_transient_dict(const hdk::ir::ExprPtr expr) {
  auto type = expr->type();
  if (!type->isString()) {
    return expr;
  }
  auto transient_dict_type = type->ctx().extDict(type, TRANSIENT_DICT_ID);
  return expr->cast(transient_dict_type);
}

hdk::ir::ExprPtr set_transient_dict_maybe(hdk::ir::ExprPtr expr) {
  try {
    return set_transient_dict(fold_expr(expr.get()));
  } catch (...) {
    return expr;
  }
}

hdk::ir::ExprPtr cast_dict_to_none(const hdk::ir::ExprPtr& input) {
  auto input_type = input->type();
  if (input_type->isExtDictionary()) {
    return input->cast(input_type->ctx().text(input_type->nullable()));
  }
  return input;
}

hdk::ir::ExprPtr translate(const hdk::ir::Expr* expr,
                           const RelAlgTranslator& translator,
                           ::ExecutorType executor_type) {
  auto res = translator.normalize(expr);
  res = rewrite_array_elements(res.get());
  res = rewrite_expr(res.get());
  if (executor_type == ExecutorType::Native) {
    // This is actually added to get full match of translated legacy
    // rex expressions and new Exprs. It's done only for testing purposes
    // and shouldn't have any effect on functionality and performance.
    // TODO: remove when rex are not used anymore
    if (auto* agg = dynamic_cast<const hdk::ir::AggExpr*>(res.get())) {
      if (agg->arg()) {
        auto new_arg = set_transient_dict_maybe(agg->argShared());
        res = hdk::ir::makeExpr<hdk::ir::AggExpr>(
            agg->type(), agg->aggType(), new_arg, agg->isDistinct(), agg->arg1());
      }
    } else {
      res = set_transient_dict_maybe(res);
    }
  } else if (executor_type == ExecutorType::TableFunctions) {
    res = fold_expr(res.get());
  } else {
    res = cast_dict_to_none(fold_expr(res.get()));
  }
  return res;
}

std::list<hdk::ir::ExprPtr> translate_groupby_exprs(const hdk::ir::Compound* compound,
                                                    const RelAlgTranslator& translator,
                                                    ::ExecutorType executor_type) {
  if (!compound->isAggregate()) {
    return {nullptr};
  }
  std::list<hdk::ir::ExprPtr> groupby_exprs;
  for (size_t group_idx = 0; group_idx < compound->getGroupByCount(); ++group_idx) {
    auto expr = compound->getGroupByExpr(group_idx);
    expr = translate(expr.get(), translator, executor_type);
    groupby_exprs.push_back(expr);
  }
  return groupby_exprs;
}

std::list<hdk::ir::ExprPtr> translate_groupby_exprs(
    const hdk::ir::Aggregate* aggregate,
    const std::vector<hdk::ir::ExprPtr>& scalar_sources) {
  std::list<hdk::ir::ExprPtr> groupby_exprs;
  for (size_t group_idx = 0; group_idx < aggregate->getGroupByCount(); ++group_idx) {
    groupby_exprs.push_back(set_transient_dict(scalar_sources[group_idx]));
  }
  return groupby_exprs;
}

QualsConjunctiveForm translate_quals(const hdk::ir::Compound* compound,
                                     const RelAlgTranslator& translator) {
  if (auto filter = compound->getFilter()) {
    auto filter_expr = translator.normalize(filter.get());
    filter_expr = fold_expr(filter_expr.get());
    return qual_to_conjunctive_form(filter_expr);
  }
  return {};
}

std::vector<const hdk::ir::Expr*> translate_targets(
    std::vector<hdk::ir::ExprPtr>& target_exprs_owned,
    const std::list<hdk::ir::ExprPtr>& groupby_exprs,
    const hdk::ir::Compound* compound,
    const RelAlgTranslator& translator,
    const ExecutorType executor_type,
    bool bigint_count) {
  std::vector<const hdk::ir::Expr*> target_exprs;
  for (size_t i = 0; i < compound->size(); ++i) {
    const auto* expr = compound->getExprs()[i].get();
    hdk::ir::ExprPtr target_expr;
    if (auto* group_ref = dynamic_cast<const hdk::ir::GroupColumnRef*>(expr)) {
      const auto ref_idx = group_ref->index();
      CHECK_GE(ref_idx, size_t(1));
      CHECK_LE(ref_idx, groupby_exprs.size());
      const auto groupby_expr = *std::next(groupby_exprs.begin(), ref_idx - 1);
      target_expr = var_ref(groupby_expr.get(), hdk::ir::Var::kGROUPBY, ref_idx);
    } else {
      target_expr = translate(expr, translator, executor_type);
    }

    target_exprs_owned.push_back(target_expr);
    target_exprs.push_back(target_expr.get());
  }
  return target_exprs;
}

std::vector<const hdk::ir::Expr*> translate_targets(
    std::vector<hdk::ir::ExprPtr>& target_exprs_owned,
    const std::vector<hdk::ir::ExprPtr>& scalar_sources,
    const std::list<hdk::ir::ExprPtr>& groupby_exprs,
    const hdk::ir::Aggregate* aggregate,
    const RelAlgTranslator& translator,
    bool bigint_count) {
  std::vector<const hdk::ir::Expr*> target_exprs;
  size_t group_key_idx = 1;
  for (const auto& groupby_expr : groupby_exprs) {
    auto target_expr =
        var_ref(groupby_expr.get(), hdk::ir::Var::kGROUPBY, group_key_idx++);
    target_exprs_owned.push_back(target_expr);
    target_exprs.push_back(target_expr.get());
  }

  for (const auto& agg : aggregate->getAggs()) {
    auto target_expr = translator.normalize(agg.get());
    target_expr = fold_expr(target_expr.get());
    target_exprs.emplace_back(target_expr.get());
    target_exprs_owned.emplace_back(std::move(target_expr));
  }
  return target_exprs;
}

bool is_count_distinct(const hdk::ir::Expr* expr) {
  const auto agg_expr = dynamic_cast<const hdk::ir::AggExpr*>(expr);
  return agg_expr && agg_expr->isDistinct();
}

bool is_agg(const hdk::ir::Expr* expr) {
  const auto agg_expr = dynamic_cast<const hdk::ir::AggExpr*>(expr);
  if (agg_expr && agg_expr->containsAgg()) {
    auto agg_type = agg_expr->aggType();
    if (agg_type == hdk::ir::AggType::kMin || agg_type == hdk::ir::AggType::kMax ||
        agg_type == hdk::ir::AggType::kSum || agg_type == hdk::ir::AggType::kAvg) {
      return true;
    }
  }
  return false;
}

inline const hdk::ir::Type* canonicalTypeForExpr(const hdk::ir::Expr& expr) {
  if (is_count_distinct(&expr)) {
    return expr.type()->ctx().int64();
  }
  auto res = expr.type()->canonicalize();
  if (is_agg(&expr)) {
    res = res->withNullable(true);
  }
  return res;
}

template <class RA>
std::vector<TargetMetaInfo> get_targets_meta(
    const RA* ra_node,
    const std::vector<const hdk::ir::Expr*>& target_exprs) {
  std::vector<TargetMetaInfo> targets_meta;
  CHECK_EQ(ra_node->size(), target_exprs.size());
  for (size_t i = 0; i < ra_node->size(); ++i) {
    CHECK(target_exprs[i]);
    // TODO(alex): remove the count distinct type fixup.
    targets_meta.emplace_back(ra_node->getFieldName(i),
                              canonicalTypeForExpr(*target_exprs[i]));
  }
  return targets_meta;
}

template <>
std::vector<TargetMetaInfo> get_targets_meta(
    const hdk::ir::Filter* filter,
    const std::vector<const hdk::ir::Expr*>& target_exprs) {
  hdk::ir::Node const* input0 = filter->getInput(0);
  if (auto const* input = dynamic_cast<hdk::ir::Compound const*>(input0)) {
    return get_targets_meta(input, target_exprs);
  } else if (auto const* input = dynamic_cast<hdk::ir::Project const*>(input0)) {
    return get_targets_meta(input, target_exprs);
  } else if (auto const* input = dynamic_cast<hdk::ir::LogicalUnion const*>(input0)) {
    return get_targets_meta(input, target_exprs);
  } else if (auto const* input = dynamic_cast<hdk::ir::Aggregate const*>(input0)) {
    return get_targets_meta(input, target_exprs);
  } else if (auto const* input = dynamic_cast<hdk::ir::Scan const*>(input0)) {
    return get_targets_meta(input, target_exprs);
  }
  UNREACHABLE() << "Unhandled node type: " << input0->toString();
  return {};
}

}  // namespace

ExecutionResult RelAlgExecutor::executeCompound(const hdk::ir::Compound* compound,
                                                const CompilationOptions& co,
                                                const ExecutionOptions& eo,
                                                const int64_t queue_time_ms) {
  auto timer = DEBUG_TIMER(__func__);
  const auto work_unit =
      createCompoundWorkUnit(compound, {{}, SortAlgorithm::Default, 0, 0}, eo);
  CompilationOptions co_compound = co;
  return executeWorkUnit(work_unit,
                         compound->getOutputMetainfo(),
                         compound->isAggregate(),
                         co_compound,
                         eo,
                         queue_time_ms);
}

ExecutionResult RelAlgExecutor::executeAggregate(const hdk::ir::Aggregate* aggregate,
                                                 const CompilationOptions& co,
                                                 const ExecutionOptions& eo,
                                                 const int64_t queue_time_ms) {
  auto timer = DEBUG_TIMER(__func__);
  const auto work_unit = createAggregateWorkUnit(
      aggregate, {{}, SortAlgorithm::Default, 0, 0}, eo.just_explain);
  return executeWorkUnit(
      work_unit, aggregate->getOutputMetainfo(), true, co, eo, queue_time_ms);
}

namespace {

// Returns true iff the execution unit contains window functions.
bool is_window_execution_unit(const RelAlgExecutionUnit& ra_exe_unit) {
  return std::any_of(ra_exe_unit.target_exprs.begin(),
                     ra_exe_unit.target_exprs.end(),
                     [](const hdk::ir::Expr* expr) {
                       return dynamic_cast<const hdk::ir::WindowFunction*>(expr);
                     });
}

}  // namespace

ExecutionResult RelAlgExecutor::executeProject(
    const hdk::ir::Project* project,
    const CompilationOptions& co,
    const ExecutionOptions& eo,
    const int64_t queue_time_ms,
    const std::optional<size_t> previous_count) {
  auto timer = DEBUG_TIMER(__func__);
  auto work_unit = createProjectWorkUnit(project, {{}, SortAlgorithm::Default, 0, 0}, eo);
  CompilationOptions co_project = co;
  if (project->isSimple()) {
    CHECK_EQ(size_t(1), project->inputCount());
    const auto input_ra = project->getInput(0);
    if (dynamic_cast<const hdk::ir::Sort*>(input_ra)) {
      co_project.device_type = ExecutorDeviceType::CPU;
      const auto& input_table =
          get_temporary_table(&temporary_tables_, -input_ra->getId());
      work_unit.exe_unit.scan_limit =
          std::min(input_table.getLimit(), input_table.rowCount());
    }
  }
  return executeWorkUnit(work_unit,
                         project->getOutputMetainfo(),
                         false,
                         co_project,
                         eo,
                         queue_time_ms,
                         previous_count);
}

ExecutionResult RelAlgExecutor::executeTableFunction(
    const hdk::ir::TableFunction* table_func,
    const CompilationOptions& co_in,
    const ExecutionOptions& eo,
    const int64_t queue_time_ms) {
  INJECT_TIMER(executeTableFunction);
  auto timer = DEBUG_TIMER(__func__);

  auto co = co_in;

  if (!g_enable_table_functions) {
    throw std::runtime_error("Table function support is disabled");
  }
  auto table_func_work_unit = createTableFunctionWorkUnit(
      table_func,
      eo.just_explain,
      /*is_gpu = */ co.device_type == ExecutorDeviceType::GPU);
  const auto body = table_func_work_unit.body;
  CHECK(body);

  const auto table_infos =
      get_table_infos(table_func_work_unit.exe_unit.input_descs, executor_);

  ExecutionResult result{std::make_shared<ResultSet>(std::vector<TargetInfo>{},
                                                     co.device_type,
                                                     QueryMemoryDescriptor(),
                                                     nullptr,
                                                     executor_->getDataMgr(),
                                                     executor_->getBufferProvider(),
                                                     executor_->blockSize(),
                                                     executor_->gridSize()),
                         {}};

  try {
    result = {executor_->executeTableFunction(
                  table_func_work_unit.exe_unit, table_infos, co, eo, data_provider_),
              body->getOutputMetainfo()};
  } catch (const QueryExecutionError& e) {
    handlePersistentError(e.getErrorCode());
    CHECK(e.getErrorCode() == Executor::ERR_OUT_OF_GPU_MEM);
    throw std::runtime_error("Table function ran out of memory during execution");
  }
  result.setQueueTime(queue_time_ms);
  return result;
}

namespace {

// Creates a new expression which has the range table index set to 1. This is needed
// to reuse the hash join construction helpers to generate a hash table for the window
// function partition: create an equals expression with left and right sides identical
// except for the range table index.
hdk::ir::ExprPtr transform_to_inner(const hdk::ir::Expr* expr) {
  const auto tuple = dynamic_cast<const hdk::ir::ExpressionTuple*>(expr);
  if (tuple) {
    std::vector<hdk::ir::ExprPtr> transformed_tuple;
    for (const auto& element : tuple->tuple()) {
      transformed_tuple.push_back(transform_to_inner(element.get()));
    }
    return hdk::ir::makeExpr<hdk::ir::ExpressionTuple>(transformed_tuple);
  }
  const auto col = dynamic_cast<const hdk::ir::ColumnVar*>(expr);
  if (!col) {
    throw std::runtime_error("Only columns supported in the window partition for now");
  }
  return hdk::ir::makeExpr<hdk::ir::ColumnVar>(col->columnInfo(), 1);
}

}  // namespace

void RelAlgExecutor::computeWindow(const RelAlgExecutionUnit& ra_exe_unit,
                                   const CompilationOptions& co,
                                   const ExecutionOptions& eo,
                                   ColumnCacheMap& column_cache_map,
                                   const int64_t queue_time_ms) {
  auto query_infos = get_table_infos(ra_exe_unit.input_descs, executor_);
  CHECK_EQ(query_infos.size(), size_t(1));
  if (query_infos.front().info.fragments.size() != 1) {
    throw std::runtime_error(
        "Only single fragment tables supported for window functions for now");
  }
  if (eo.executor_type == ::ExecutorType::Extern) {
    return;
  }
  query_infos.push_back(query_infos.front());
  auto window_project_node_context = WindowProjectNodeContext::create(executor_);
  for (size_t target_index = 0; target_index < ra_exe_unit.target_exprs.size();
       ++target_index) {
    const auto& target_expr = ra_exe_unit.target_exprs[target_index];
    const auto window_func = dynamic_cast<const hdk::ir::WindowFunction*>(target_expr);
    if (!window_func) {
      continue;
    }
    // Always use baseline layout hash tables for now, make the expression a tuple.
    const auto& partition_keys = window_func->partitionKeys();
    std::shared_ptr<const hdk::ir::BinOper> partition_key_cond;
    if (partition_keys.size() >= 1) {
      hdk::ir::ExprPtr partition_key_tuple;
      if (partition_keys.size() > 1) {
        partition_key_tuple = hdk::ir::makeExpr<hdk::ir::ExpressionTuple>(partition_keys);
      } else {
        CHECK_EQ(partition_keys.size(), size_t(1));
        partition_key_tuple = partition_keys.front();
      }
      // Creates a tautology equality with the partition expression on both sides.
      partition_key_cond = hdk::ir::makeExpr<hdk::ir::BinOper>(
          target_expr->ctx().boolean(),
          hdk::ir::OpType::kBwEq,
          hdk::ir::Qualifier::kOne,
          partition_key_tuple,
          transform_to_inner(partition_key_tuple.get()));
    }
    auto context =
        createWindowFunctionContext(window_func,
                                    partition_key_cond /*nullptr if no partition key*/,
                                    ra_exe_unit,
                                    query_infos,
                                    co,
                                    column_cache_map,
                                    executor_->getRowSetMemoryOwner());
    context->compute();
    window_project_node_context->addWindowFunctionContext(std::move(context),
                                                          target_index);
  }
}

std::unique_ptr<WindowFunctionContext> RelAlgExecutor::createWindowFunctionContext(
    const hdk::ir::WindowFunction* window_func,
    const std::shared_ptr<const hdk::ir::BinOper>& partition_key_cond,
    const RelAlgExecutionUnit& ra_exe_unit,
    const std::vector<InputTableInfo>& query_infos,
    const CompilationOptions& co,
    ColumnCacheMap& column_cache_map,
    std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner) {
  const size_t elem_count = query_infos.front().info.fragments.front().getNumTuples();
  const auto memory_level = co.device_type == ExecutorDeviceType::GPU
                                ? MemoryLevel::GPU_LEVEL
                                : MemoryLevel::CPU_LEVEL;
  std::unique_ptr<WindowFunctionContext> context;
  if (partition_key_cond) {
    const auto join_table_or_err =
        executor_->buildHashTableForQualifier(partition_key_cond,
                                              query_infos,
                                              memory_level,
                                              JoinType::INVALID,  // for window function
                                              HashType::OneToMany,
                                              data_provider_,
                                              column_cache_map,
                                              ra_exe_unit.hash_table_build_plan_dag,
                                              ra_exe_unit.query_hint,
                                              ra_exe_unit.table_id_to_node_map);
    if (!join_table_or_err.fail_reason.empty()) {
      throw std::runtime_error(join_table_or_err.fail_reason);
    }
    CHECK(join_table_or_err.hash_table->getHashType() == HashType::OneToMany);
    context = std::make_unique<WindowFunctionContext>(window_func,
                                                      config_,
                                                      join_table_or_err.hash_table,
                                                      elem_count,
                                                      co.device_type,
                                                      row_set_mem_owner);
  } else {
    context = std::make_unique<WindowFunctionContext>(
        window_func, config_, elem_count, co.device_type, row_set_mem_owner);
  }
  const auto& order_keys = window_func->orderKeys();
  std::vector<std::shared_ptr<Chunk_NS::Chunk>> chunks_owner;
  for (const auto& order_key : order_keys) {
    const auto order_col = std::dynamic_pointer_cast<const hdk::ir::ColumnVar>(order_key);
    if (!order_col) {
      throw std::runtime_error("Only order by columns supported for now");
    }
    const int8_t* column;
    size_t join_col_elem_count;
    std::tie(column, join_col_elem_count) =
        ColumnFetcher::getOneColumnFragment(executor_,
                                            *order_col,
                                            query_infos.front().info.fragments.front(),
                                            memory_level,
                                            0,
                                            nullptr,
                                            /*thread_idx=*/0,
                                            chunks_owner,
                                            data_provider_,
                                            column_cache_map);

    CHECK_EQ(join_col_elem_count, elem_count);
    context->addOrderColumn(column, order_col.get(), chunks_owner);
  }
  return context;
}

ExecutionResult RelAlgExecutor::executeFilter(const hdk::ir::Filter* filter,
                                              const CompilationOptions& co,
                                              const ExecutionOptions& eo,
                                              const int64_t queue_time_ms) {
  auto timer = DEBUG_TIMER(__func__);
  const auto work_unit =
      createFilterWorkUnit(filter, {{}, SortAlgorithm::Default, 0, 0}, eo.just_explain);
  return executeWorkUnit(
      work_unit, filter->getOutputMetainfo(), false, co, eo, queue_time_ms);
}

ExecutionResult RelAlgExecutor::executeUnion(const hdk::ir::LogicalUnion* logical_union,
                                             const RaExecutionSequence& seq,
                                             const CompilationOptions& co,
                                             const ExecutionOptions& eo,
                                             const int64_t queue_time_ms) {
  auto timer = DEBUG_TIMER(__func__);
  if (!logical_union->isAll()) {
    throw std::runtime_error("UNION without ALL is not supported yet.");
  }
  // Will throw a std::runtime_error if types don't match.
  logical_union->checkForMatchingMetaInfoTypes();
  logical_union->setOutputMetainfo(logical_union->getInput(0)->getOutputMetainfo());
  // Only Projections and Aggregates from a UNION are supported for now.
  query_dag_->eachNode([logical_union](hdk::ir::Node const* node) {
    if (node->hasInput(logical_union) &&
        !shared::dynamic_castable_to_any<hdk::ir::Project,
                                         hdk::ir::LogicalUnion,
                                         hdk::ir::Aggregate>(node)) {
      throw std::runtime_error("UNION ALL not yet supported in this context.");
    }
  });

  auto work_unit =
      createUnionWorkUnit(logical_union, {{}, SortAlgorithm::Default, 0, 0}, eo);
  return executeWorkUnit(work_unit,
                         logical_union->getOutputMetainfo(),
                         false,
                         CompilationOptions::makeCpuOnly(co),
                         eo,
                         queue_time_ms);
}

ExecutionResult RelAlgExecutor::executeLogicalValues(
    const hdk::ir::LogicalValues* logical_values,
    const ExecutionOptions& eo) {
  auto timer = DEBUG_TIMER(__func__);
  QueryMemoryDescriptor query_mem_desc(executor_,
                                       logical_values->getNumRows(),
                                       QueryDescriptionType::Projection,
                                       /*is_table_function=*/false);

  auto tuple_type = logical_values->getTupleType();
  for (size_t i = 0; i < tuple_type.size(); ++i) {
    auto& target_meta_info = tuple_type[i];
    if (target_meta_info.type()->isString() || target_meta_info.type()->isArray()) {
      throw std::runtime_error("Variable length types not supported in VALUES yet.");
    }
    if (target_meta_info.type()->isNull()) {
      // replace w/ bigint
      tuple_type[i] = TargetMetaInfo(target_meta_info.get_resname(),
                                     hdk::ir::Context::defaultCtx().int64());
    }
    query_mem_desc.addColSlotInfo({std::make_tuple(tuple_type[i].type()->size(), 8)});
  }
  logical_values->setOutputMetainfo(tuple_type);

  std::vector<TargetInfo> target_infos;
  for (const auto& tuple_type_component : tuple_type) {
    target_infos.emplace_back(TargetInfo{false,
                                         hdk::ir::AggType::kCount,
                                         tuple_type_component.type(),
                                         nullptr,
                                         false,
                                         false});
  }

  std::shared_ptr<ResultSet> rs{
      ResultSetLogicalValuesBuilder{logical_values,
                                    target_infos,
                                    ExecutorDeviceType::CPU,
                                    query_mem_desc,
                                    executor_->getRowSetMemoryOwner(),
                                    executor_}
          .build()};

  return {rs, tuple_type};
}

namespace {

// TODO(alex): Once we're fully migrated to the relational algebra model, change
// the executor interface to use the collation directly and remove this conversion.
std::list<hdk::ir::OrderEntry> get_order_entries(const hdk::ir::Sort* sort) {
  std::list<hdk::ir::OrderEntry> result;
  for (size_t i = 0; i < sort->collationCount(); ++i) {
    const auto sort_field = sort->getCollation(i);
    result.emplace_back(
        sort_field.getField() + 1,
        sort_field.getSortDir() == hdk::ir::SortDirection::Descending,
        sort_field.getNullsPosition() == hdk::ir::NullSortedPosition::First);
  }
  return result;
}

size_t get_scan_limit(const hdk::ir::Node* ra, const size_t limit) {
  const auto aggregate = dynamic_cast<const hdk::ir::Aggregate*>(ra);
  if (aggregate) {
    return 0;
  }
  const auto compound = dynamic_cast<const hdk::ir::Compound*>(ra);
  return (compound && compound->isAggregate()) ? 0 : limit;
}

bool first_oe_is_desc(const std::list<hdk::ir::OrderEntry>& order_entries) {
  return !order_entries.empty() && order_entries.front().is_desc;
}

}  // namespace

ExecutionResult RelAlgExecutor::executeSort(const hdk::ir::Sort* sort,
                                            const CompilationOptions& co,
                                            const ExecutionOptions& eo,
                                            const int64_t queue_time_ms) {
  auto timer = DEBUG_TIMER(__func__);
  check_sort_node_source_constraint(sort);
  const auto source = sort->getInput(0);
  const bool is_aggregate = node_is_aggregate(source);
  std::list<hdk::ir::ExprPtr> groupby_exprs;
  bool is_desc{false};

  auto execute_sort_query = [this,
                             sort,
                             &source,
                             &is_aggregate,
                             &eo,
                             &co,
                             queue_time_ms,
                             &groupby_exprs,
                             &is_desc]() -> ExecutionResult {
    const auto source_work_unit = createSortInputWorkUnit(sort, eo);
    is_desc = first_oe_is_desc(source_work_unit.exe_unit.sort_info.order_entries);
    const ExecutionOptions eo_copy = [&]() {
      ExecutionOptions copy = eo;
      copy.just_validate = eo.just_validate || sort->isEmptyResult();
      copy.outer_fragment_indices = {};
      return copy;
    }();

    groupby_exprs = source_work_unit.exe_unit.groupby_exprs;
    auto source_result = executeWorkUnit(source_work_unit,
                                         source->getOutputMetainfo(),
                                         is_aggregate,
                                         co,
                                         eo_copy,
                                         queue_time_ms);
    if (source_result.isFilterPushDownEnabled()) {
      return source_result;
    }
    auto rows_to_sort = source_result.getRows();
    if (eo.just_explain) {
      return {rows_to_sort, {}};
    }
    const size_t limit = sort->getLimit();
    const size_t offset = sort->getOffset();
    if (sort->collationCount() != 0 && !rows_to_sort->definitelyHasNoRows() &&
        !use_speculative_top_n(source_work_unit.exe_unit,
                               rows_to_sort->getQueryMemDesc())) {
      const size_t top_n = limit == 0 ? 0 : limit + offset;
      rows_to_sort->sort(
          source_work_unit.exe_unit.sort_info.order_entries, top_n, executor_);
    }
    if (limit || offset) {
      rows_to_sort->dropFirstN(offset);
      if (limit) {
        rows_to_sort->keepFirstN(limit);
      }
    }
    return {rows_to_sort, source_result.getTargetsMeta()};
  };

  try {
    return execute_sort_query();
  } catch (const SpeculativeTopNFailed& e) {
    CHECK_EQ(size_t(1), groupby_exprs.size());
    CHECK(groupby_exprs.front());
    speculative_topn_blacklist_.add(groupby_exprs.front(), is_desc);
    return execute_sort_query();
  }
}

RelAlgExecutor::WorkUnit RelAlgExecutor::createSortInputWorkUnit(
    const hdk::ir::Sort* sort,
    const ExecutionOptions& eo) {
  const auto source = sort->getInput(0);
  const size_t limit = sort->getLimit();
  const size_t offset = sort->getOffset();
  const size_t scan_limit = sort->collationCount() ? 0 : get_scan_limit(source, limit);
  const size_t scan_total_limit =
      scan_limit ? get_scan_limit(source, scan_limit + offset) : 0;
  size_t max_groups_buffer_entry_guess{
      scan_total_limit ? scan_total_limit
                       : config_.exec.group_by.default_max_groups_buffer_entry_guess};
  SortAlgorithm sort_algorithm{SortAlgorithm::SpeculativeTopN};
  const auto order_entries = get_order_entries(sort);
  SortInfo sort_info{order_entries, sort_algorithm, limit, offset};
  auto source_work_unit = createWorkUnit(source, sort_info, eo);
  const auto& source_exe_unit = source_work_unit.exe_unit;

  // we do not allow sorting array types
  for (auto order_entry : order_entries) {
    CHECK_GT(order_entry.tle_no, 0);  // tle_no is a 1-base index
    const auto& te = source_exe_unit.target_exprs[order_entry.tle_no - 1];
    const auto& ti = get_target_info(te, false);
    if (ti.type->isArray()) {
      throw std::runtime_error(
          "Columns with array types cannot be used in an ORDER BY clause.");
    }
  }

  if (source_exe_unit.groupby_exprs.size() == 1) {
    if (!source_exe_unit.groupby_exprs.front()) {
      sort_algorithm = SortAlgorithm::StreamingTopN;
    } else {
      if (speculative_topn_blacklist_.contains(source_exe_unit.groupby_exprs.front(),
                                               first_oe_is_desc(order_entries))) {
        sort_algorithm = SortAlgorithm::Default;
      }
    }
  }

  sort->setOutputMetainfo(source->getOutputMetainfo());
  // NB: the `body` field of the returned `WorkUnit` needs to be the `source` node,
  // not the `sort`. The aggregator needs the pre-sorted result from leaves.
  return {RelAlgExecutionUnit{source_exe_unit.input_descs,
                              std::move(source_exe_unit.input_col_descs),
                              source_exe_unit.simple_quals,
                              source_exe_unit.quals,
                              source_exe_unit.join_quals,
                              source_exe_unit.groupby_exprs,
                              source_exe_unit.target_exprs,
                              nullptr,
                              {sort_info.order_entries, sort_algorithm, limit, offset},
                              scan_total_limit,
                              source_exe_unit.query_hint,
                              source_exe_unit.query_plan_dag,
                              source_exe_unit.hash_table_build_plan_dag,
                              source_exe_unit.table_id_to_node_map,
                              source_exe_unit.use_bump_allocator,
                              source_exe_unit.union_all},
          source,
          max_groups_buffer_entry_guess,
          std::move(source_work_unit.query_rewriter),
          source_work_unit.input_permutation,
          source_work_unit.left_deep_join_input_sizes};
}

namespace {

/**
 *  Upper bound estimation for the number of groups. Not strictly correct and not
 * tight, but if the tables involved are really small we shouldn't waste time doing
 * the NDV estimation. We don't account for cross-joins and / or group by unnested
 * array, which is the reason this estimation isn't entirely reliable.
 */
size_t groups_approx_upper_bound(const std::vector<InputTableInfo>& table_infos) {
  CHECK(!table_infos.empty());
  const auto& first_table = table_infos.front();
  size_t max_num_groups = first_table.info.getNumTuplesUpperBound();
  for (const auto& table_info : table_infos) {
    if (table_info.info.getNumTuplesUpperBound() > max_num_groups) {
      max_num_groups = table_info.info.getNumTuplesUpperBound();
    }
  }
  return std::max(max_num_groups, size_t(1));
}

/**
 * Determines whether a query needs to compute the size of its output buffer. Returns
 * true for projection queries with no LIMIT or a LIMIT that exceeds the high scan
 * limit threshold (meaning it would be cheaper to compute the number of rows passing
 * or use the bump allocator than allocate the current scan limit per GPU)
 */
bool compute_output_buffer_size(const RelAlgExecutionUnit& ra_exe_unit) {
  for (const auto target_expr : ra_exe_unit.target_exprs) {
    if (dynamic_cast<const hdk::ir::AggExpr*>(target_expr)) {
      return false;
    }
  }
  if (ra_exe_unit.groupby_exprs.size() == 1 && !ra_exe_unit.groupby_exprs.front() &&
      (!ra_exe_unit.scan_limit || ra_exe_unit.scan_limit > Executor::high_scan_limit)) {
    return true;
  }
  return false;
}

inline bool exe_unit_has_quals(const RelAlgExecutionUnit ra_exe_unit) {
  return !(ra_exe_unit.quals.empty() && ra_exe_unit.join_quals.empty() &&
           ra_exe_unit.simple_quals.empty());
}

RelAlgExecutionUnit decide_approx_count_distinct_implementation(
    const RelAlgExecutionUnit& ra_exe_unit_in,
    const std::vector<InputTableInfo>& table_infos,
    const Executor* executor,
    const ExecutorDeviceType device_type_in,
    std::vector<hdk::ir::ExprPtr>& target_exprs_owned) {
  RelAlgExecutionUnit ra_exe_unit = ra_exe_unit_in;
  for (size_t i = 0; i < ra_exe_unit.target_exprs.size(); ++i) {
    const auto target_expr = ra_exe_unit.target_exprs[i];
    const auto agg_info =
        get_target_info(target_expr, executor->getConfig().exec.group_by.bigint_count);
    if (agg_info.agg_kind != hdk::ir::AggType::kApproxCountDistinct) {
      continue;
    }
    CHECK(target_expr->is<hdk::ir::AggExpr>());
    const auto arg = target_expr->as<hdk::ir::AggExpr>()->argShared();
    CHECK(arg);
    auto arg_type = arg->type();
    // Avoid calling getExpressionRange for variable length types (string and array),
    // it'd trigger an assertion since that API expects to be called only for types
    // for which the notion of range is well-defined. A bit of a kludge, but the
    // logic to reject these types anyway is at lower levels in the stack and not
    // really worth pulling into a separate function for now.
    if (!(arg_type->isNumber() || arg_type->isBoolean() || arg_type->isDateTime() ||
          (arg_type->isExtDictionary()))) {
      continue;
    }
    const auto arg_range = getExpressionRange(arg.get(), table_infos, executor);
    if (arg_range.getType() != ExpressionRangeType::Integer) {
      continue;
    }
    // When running distributed, the threshold for using the precise implementation
    // must be consistent across all leaves, otherwise we could have a mix of precise
    // and approximate bitmaps and we cannot aggregate them.
    const auto device_type = device_type_in;
    const auto bitmap_sz_bits = arg_range.getIntMax() - arg_range.getIntMin() + 1;
    const auto sub_bitmap_count =
        get_count_distinct_sub_bitmap_count(bitmap_sz_bits, ra_exe_unit, device_type);
    int64_t approx_bitmap_sz_bits{0};
    const auto error_rate = target_expr->as<hdk::ir::AggExpr>()->arg1();
    if (error_rate) {
      CHECK(error_rate->type()->isInt32());
      CHECK_GE(error_rate->value().intval, 1);
      approx_bitmap_sz_bits = hll_size_for_rate(error_rate->value().intval);
    } else {
      approx_bitmap_sz_bits = executor->getConfig().exec.group_by.hll_precision_bits;
    }
    CountDistinctDescriptor approx_count_distinct_desc{CountDistinctImplType::Bitmap,
                                                       arg_range.getIntMin(),
                                                       approx_bitmap_sz_bits,
                                                       true,
                                                       device_type,
                                                       sub_bitmap_count};
    CountDistinctDescriptor precise_count_distinct_desc{CountDistinctImplType::Bitmap,
                                                        arg_range.getIntMin(),
                                                        bitmap_sz_bits,
                                                        false,
                                                        device_type,
                                                        sub_bitmap_count};
    if (approx_count_distinct_desc.bitmapPaddedSizeBytes() >=
        precise_count_distinct_desc.bitmapPaddedSizeBytes()) {
      auto precise_count_distinct = hdk::ir::makeExpr<hdk::ir::AggExpr>(
          get_agg_type(hdk::ir::AggType::kCount,
                       arg.get(),
                       executor->getConfig().exec.group_by.bigint_count),
          hdk::ir::AggType::kCount,
          arg,
          true,
          nullptr);
      target_exprs_owned.push_back(precise_count_distinct);
      ra_exe_unit.target_exprs[i] = precise_count_distinct.get();
    }
  }
  return ra_exe_unit;
}

inline bool can_use_bump_allocator(const RelAlgExecutionUnit& ra_exe_unit,
                                   const Config& config,
                                   const CompilationOptions& co,
                                   const ExecutionOptions& eo) {
  return config.mem.gpu.enable_bump_allocator &&
         (co.device_type == ExecutorDeviceType::GPU) && !eo.output_columnar_hint &&
         ra_exe_unit.sort_info.order_entries.empty();
}

}  // namespace

ExecutionResult RelAlgExecutor::executeWorkUnit(
    const RelAlgExecutor::WorkUnit& work_unit,
    const std::vector<TargetMetaInfo>& targets_meta,
    const bool is_agg,
    const CompilationOptions& co_in,
    const ExecutionOptions& eo_in,
    const int64_t queue_time_ms,
    const std::optional<size_t> previous_count) {
  INJECT_TIMER(executeWorkUnit);
  auto timer = DEBUG_TIMER(__func__);

  auto co = co_in;
  auto eo = eo_in;
  ColumnCacheMap column_cache;
  if (is_window_execution_unit(work_unit.exe_unit)) {
    if (!config_.exec.window_func.enable) {
      throw std::runtime_error("Window functions support is disabled");
    }
    co.device_type = ExecutorDeviceType::CPU;
    co.allow_lazy_fetch = false;
    computeWindow(work_unit.exe_unit, co, eo, column_cache, queue_time_ms);
  }
  if (!eo.just_explain && eo.find_push_down_candidates) {
    // find potential candidates:
    auto selected_filters = selectFiltersToBePushedDown(work_unit, co, eo);
    if (!selected_filters.empty() || eo.just_calcite_explain) {
      return ExecutionResult(selected_filters, eo.find_push_down_candidates);
    }
  }
  const auto body = work_unit.body;
  CHECK(body);
  const auto table_infos = get_table_infos(work_unit.exe_unit, executor_);

  auto ra_exe_unit = decide_approx_count_distinct_implementation(
      work_unit.exe_unit, table_infos, executor_, co.device_type, target_exprs_owned_);

  // register query hint if query_dag_ is valid
  ra_exe_unit.query_hint = RegisteredQueryHint::fromConfig(config_);
  if (query_dag_) {
    auto candidate = query_dag_->getQueryHint(body);
    if (candidate) {
      ra_exe_unit.query_hint = *candidate;
    }
  }
  auto max_groups_buffer_entry_guess = work_unit.max_groups_buffer_entry_guess;
  if (is_window_execution_unit(ra_exe_unit)) {
    CHECK_EQ(table_infos.size(), size_t(1));
    CHECK_EQ(table_infos.front().info.fragments.size(), size_t(1));
    max_groups_buffer_entry_guess =
        table_infos.front().info.fragments.front().getNumTuples();
    ra_exe_unit.scan_limit = max_groups_buffer_entry_guess;
  } else if (compute_output_buffer_size(ra_exe_unit) && !isRowidLookup(work_unit)) {
    if (previous_count && !exe_unit_has_quals(ra_exe_unit)) {
      ra_exe_unit.scan_limit = *previous_count;
    } else {
      if (can_use_bump_allocator(ra_exe_unit, config_, co, eo)) {
        ra_exe_unit.scan_limit = 0;
        ra_exe_unit.use_bump_allocator = true;
      } else if (eo.executor_type == ::ExecutorType::Extern) {
        ra_exe_unit.scan_limit = 0;
      } else if (!eo.just_explain) {
        const auto filter_count_all = getFilteredCountAll(work_unit, true, co, eo);
        if (filter_count_all) {
          ra_exe_unit.scan_limit = std::max(*filter_count_all, size_t(1));
        }
      }
    }
  }

  if (g_columnar_large_projections) {
    const auto prefer_columnar = should_output_columnar(ra_exe_unit);
    if (prefer_columnar) {
      VLOG(1) << "Using columnar layout for projection as output size of "
              << ra_exe_unit.scan_limit << " rows exceeds threshold of "
              << g_columnar_large_projections_threshold << ".";
      eo.output_columnar_hint = true;
    }
  }

  ExecutionResult result{std::make_shared<ResultSet>(std::vector<TargetInfo>{},
                                                     co.device_type,
                                                     QueryMemoryDescriptor(),
                                                     nullptr,
                                                     executor_->getDataMgr(),
                                                     executor_->getBufferProvider(),
                                                     executor_->blockSize(),
                                                     executor_->gridSize()),
                         {}};

  auto execute_and_handle_errors = [&](const auto max_groups_buffer_entry_guess_in,
                                       const bool has_cardinality_estimation,
                                       const bool has_ndv_estimation) -> ExecutionResult {
    // Note that the groups buffer entry guess may be modified during query execution.
    // Create a local copy so we can track those changes if we need to attempt a retry
    // due to OOM
    auto local_groups_buffer_entry_guess = max_groups_buffer_entry_guess_in;
    try {
      return {executor_->executeWorkUnit(local_groups_buffer_entry_guess,
                                         is_agg,
                                         table_infos,
                                         ra_exe_unit,
                                         co,
                                         eo,
                                         has_cardinality_estimation,
                                         data_provider_,
                                         column_cache),
              targets_meta};
    } catch (const QueryExecutionError& e) {
      if (!has_ndv_estimation && e.getErrorCode() < 0) {
        throw CardinalityEstimationRequired(/*range=*/0);
      }
      handlePersistentError(e.getErrorCode());
      return handleOutOfMemoryRetry(
          {ra_exe_unit, work_unit.body, local_groups_buffer_entry_guess},
          targets_meta,
          is_agg,
          co,
          eo,
          e.wasMultifragKernelLaunch(),
          queue_time_ms);
    }
  };

  auto cache_key = ra_exec_unit_desc_for_caching(ra_exe_unit);
  try {
    auto cached_cardinality = executor_->getCachedCardinality(cache_key);
    auto card = cached_cardinality.second;
    if (cached_cardinality.first && card >= 0) {
      result = execute_and_handle_errors(
          card, /*has_cardinality_estimation=*/true, /*has_ndv_estimation=*/false);
    } else {
      result = execute_and_handle_errors(max_groups_buffer_entry_guess,
                                         groups_approx_upper_bound(table_infos) <=
                                             config_.exec.group_by.big_group_threshold,
                                         /*has_ndv_estimation=*/false);
    }
  } catch (const CardinalityEstimationRequired& e) {
    // check the cardinality cache
    auto cached_cardinality = executor_->getCachedCardinality(cache_key);
    auto card = cached_cardinality.second;
    if (cached_cardinality.first && card >= 0) {
      result = execute_and_handle_errors(card, true, /*has_ndv_estimation=*/true);
    } else {
      const auto ndv_groups_estimation =
          getNDVEstimation(work_unit, e.range(), is_agg, co, eo);
      const auto estimated_groups_buffer_entry_guess =
          ndv_groups_estimation > 0 ? 2 * ndv_groups_estimation
                                    : std::min(groups_approx_upper_bound(table_infos),
                                               g_estimator_failure_max_groupby_size);
      CHECK_GT(estimated_groups_buffer_entry_guess, size_t(0));
      result = execute_and_handle_errors(
          estimated_groups_buffer_entry_guess, true, /*has_ndv_estimation=*/true);
      if (!(eo.just_validate || eo.just_explain)) {
        executor_->addToCardinalityCache(cache_key, estimated_groups_buffer_entry_guess);
      }
    }
  }

  result.setQueueTime(queue_time_ms);
  return result;
}

std::optional<size_t> RelAlgExecutor::getFilteredCountAll(const WorkUnit& work_unit,
                                                          const bool is_agg,
                                                          const CompilationOptions& co,
                                                          const ExecutionOptions& eo) {
  const auto count = hdk::ir::makeExpr<hdk::ir::AggExpr>(
      hdk::ir::Context::defaultCtx().integer(config_.exec.group_by.bigint_count ? 8 : 4),
      hdk::ir::AggType::kCount,
      nullptr,
      false,
      nullptr);
  const auto count_all_exe_unit = create_count_all_execution_unit(
      work_unit.exe_unit, count, config_.opts.strip_join_covered_quals);
  size_t one{1};
  TemporaryTable count_all_result;
  try {
    ColumnCacheMap column_cache;
    count_all_result =
        executor_->executeWorkUnit(one,
                                   is_agg,
                                   get_table_infos(work_unit.exe_unit, executor_),
                                   count_all_exe_unit,
                                   co,
                                   eo,
                                   false,
                                   data_provider_,
                                   column_cache);
  } catch (const QueryMustRunOnCpu&) {
    // force a retry of the top level query on CPU
    throw;
  } catch (const std::exception& e) {
    LOG(WARNING) << "Failed to run pre-flight filtered count with error " << e.what();
    return std::nullopt;
  }
  CHECK_EQ(count_all_result.getFragCount(), 1);
  const auto count_row = count_all_result[0]->getNextRow(false, false);
  CHECK_EQ(size_t(1), count_row.size());
  const auto& count_tv = count_row.front();
  const auto count_scalar_tv = boost::get<ScalarTargetValue>(&count_tv);
  CHECK(count_scalar_tv);
  const auto count_ptr = boost::get<int64_t>(count_scalar_tv);
  CHECK(count_ptr);
  CHECK_GE(*count_ptr, 0);
  auto count_upper_bound = static_cast<size_t>(*count_ptr);
  return std::max(count_upper_bound, size_t(1));
}

bool RelAlgExecutor::isRowidLookup(const WorkUnit& work_unit) {
  const auto& ra_exe_unit = work_unit.exe_unit;
  if (ra_exe_unit.input_descs.size() != 1) {
    return false;
  }
  const auto& table_desc = ra_exe_unit.input_descs.front();
  if (table_desc.getSourceType() != InputSourceType::TABLE) {
    return false;
  }
  for (const auto& simple_qual : ra_exe_unit.simple_quals) {
    const auto comp_expr = std::dynamic_pointer_cast<const hdk::ir::BinOper>(simple_qual);
    if (!comp_expr || !comp_expr->isEq()) {
      return false;
    }
    const auto lhs = comp_expr->leftOperand();
    const auto lhs_col = dynamic_cast<const hdk::ir::ColumnVar*>(lhs);
    if (!lhs_col || !lhs_col->tableId() || lhs_col->rteIdx()) {
      return false;
    }
    const auto rhs = comp_expr->rightOperand();
    const auto rhs_const = dynamic_cast<const hdk::ir::Constant*>(rhs);
    if (!rhs_const) {
      return false;
    }
    return lhs_col->isVirtual();
  }
  return false;
}

ExecutionResult RelAlgExecutor::handleOutOfMemoryRetry(
    const RelAlgExecutor::WorkUnit& work_unit,
    const std::vector<TargetMetaInfo>& targets_meta,
    const bool is_agg,
    const CompilationOptions& co,
    const ExecutionOptions& eo,
    const bool was_multifrag_kernel_launch,
    const int64_t queue_time_ms) {
  // Disable the bump allocator
  // Note that this will have basically the same affect as using the bump allocator
  // for the kernel per fragment path. Need to unify the max_groups_buffer_entry_guess
  // = 0 path and the bump allocator path for kernel per fragment execution.
  auto ra_exe_unit_in = work_unit.exe_unit;
  ra_exe_unit_in.use_bump_allocator = false;

  auto result =
      ExecutionResult{std::make_shared<ResultSet>(std::vector<TargetInfo>{},
                                                  co.device_type,
                                                  QueryMemoryDescriptor(),
                                                  nullptr,
                                                  executor_->getDataMgr(),
                                                  executor_->getBufferProvider(),
                                                  executor_->blockSize(),
                                                  executor_->gridSize()),
                      {}};

  const auto table_infos = get_table_infos(ra_exe_unit_in, executor_);
  auto max_groups_buffer_entry_guess = work_unit.max_groups_buffer_entry_guess;
  const ExecutionOptions eo_no_multifrag = [&]() {
    ExecutionOptions copy = eo;
    copy.allow_multifrag = false;
    copy.just_explain = false;
    copy.find_push_down_candidates = false;
    copy.just_calcite_explain = false;
    return copy;
  }();

  if (was_multifrag_kernel_launch) {
    try {
      // Attempt to retry using the kernel per fragment path. The smaller input size
      // required may allow the entire kernel to execute in GPU memory.
      LOG(WARNING) << "Multifrag query ran out of memory, retrying with multifragment "
                      "kernels disabled.";
      const auto ra_exe_unit = decide_approx_count_distinct_implementation(
          ra_exe_unit_in, table_infos, executor_, co.device_type, target_exprs_owned_);
      ColumnCacheMap column_cache;
      result = {executor_->executeWorkUnit(max_groups_buffer_entry_guess,
                                           is_agg,
                                           table_infos,
                                           ra_exe_unit,
                                           co,
                                           eo_no_multifrag,
                                           true,
                                           data_provider_,
                                           column_cache),
                targets_meta};
      result.setQueueTime(queue_time_ms);
    } catch (const QueryExecutionError& e) {
      handlePersistentError(e.getErrorCode());
      LOG(WARNING) << "Kernel per fragment query ran out of memory, retrying on CPU.";
    }
  }

  const auto co_cpu = CompilationOptions::makeCpuOnly(co);
  // Only reset the group buffer entry guess if we ran out of slots, which
  // suggests a
  // highly pathological input which prevented a good estimation of distinct tuple
  // count. For projection queries, this will force a per-fragment scan limit, which
  // is compatible with the CPU path
  VLOG(1) << "Resetting max groups buffer entry guess.";
  max_groups_buffer_entry_guess = 0;

  int iteration_ctr = -1;
  while (true) {
    iteration_ctr++;
    auto ra_exe_unit = decide_approx_count_distinct_implementation(
        ra_exe_unit_in, table_infos, executor_, co_cpu.device_type, target_exprs_owned_);
    ColumnCacheMap column_cache;
    try {
      result = {executor_->executeWorkUnit(max_groups_buffer_entry_guess,
                                           is_agg,
                                           table_infos,
                                           ra_exe_unit,
                                           co_cpu,
                                           eo_no_multifrag,
                                           true,
                                           data_provider_,
                                           column_cache),
                targets_meta};
    } catch (const QueryExecutionError& e) {
      // Ran out of slots
      if (e.getErrorCode() < 0) {
        // Even the conservative guess failed; it should only happen when we group
        // by a huge cardinality array. Maybe we should throw an exception instead?
        // Such a heavy query is entirely capable of exhausting all the host memory.
        CHECK(max_groups_buffer_entry_guess);
        // Only allow two iterations of increasingly large entry guesses up to a
        // maximum of 512MB per column per kernel
        if (config_.exec.watchdog.enable || iteration_ctr > 1) {
          throw std::runtime_error("Query ran out of output slots in the result");
        }
        max_groups_buffer_entry_guess *= 2;
        LOG(WARNING) << "Query ran out of slots in the output buffer, retrying with max "
                        "groups buffer entry "
                        "guess equal to "
                     << max_groups_buffer_entry_guess;
      } else {
        handlePersistentError(e.getErrorCode());
      }
      continue;
    }
    result.setQueueTime(queue_time_ms);
    return result;
  }
  return result;
}

void RelAlgExecutor::handlePersistentError(const int32_t error_code) {
  LOG(ERROR) << "Query execution failed with error "
             << getErrorMessageFromCode(error_code);
  if (error_code == Executor::ERR_OUT_OF_GPU_MEM) {
    // We ran out of GPU memory, this doesn't count as an error if the query is
    // allowed to continue on CPU because retry on CPU is explicitly allowed through
    // --allow-cpu-retry.
    LOG(INFO) << "Query ran out of GPU memory, attempting punt to CPU";
    if (!config_.exec.heterogeneous.allow_cpu_retry) {
      throw std::runtime_error(
          "Query ran out of GPU memory, unable to automatically retry on CPU");
    }
    return;
  }
  throw std::runtime_error(getErrorMessageFromCode(error_code));
}

namespace {
struct ErrorInfo {
  const char* code{nullptr};
  const char* description{nullptr};
};
ErrorInfo getErrorDescription(const int32_t error_code) {
  switch (error_code) {
    case Executor::ERR_DIV_BY_ZERO:
      return {"ERR_DIV_BY_ZERO", "Division by zero"};
    case Executor::ERR_OUT_OF_GPU_MEM:
      return {"ERR_OUT_OF_GPU_MEM",

              "Query couldn't keep the entire working set of columns in GPU memory"};
    case Executor::ERR_UNSUPPORTED_SELF_JOIN:
      return {"ERR_UNSUPPORTED_SELF_JOIN", "Self joins not supported yet"};
    case Executor::ERR_OUT_OF_CPU_MEM:
      return {"ERR_OUT_OF_CPU_MEM", "Not enough host memory to execute the query"};
    case Executor::ERR_OVERFLOW_OR_UNDERFLOW:
      return {"ERR_OVERFLOW_OR_UNDERFLOW", "Overflow or underflow"};
    case Executor::ERR_OUT_OF_TIME:
      return {"ERR_OUT_OF_TIME", "Query execution has exceeded the time limit"};
    case Executor::ERR_INTERRUPTED:
      return {"ERR_INTERRUPTED", "Query execution has been interrupted"};
    case Executor::ERR_COLUMNAR_CONVERSION_NOT_SUPPORTED:
      return {"ERR_COLUMNAR_CONVERSION_NOT_SUPPORTED",
              "Columnar conversion not supported for variable length types"};
    case Executor::ERR_TOO_MANY_LITERALS:
      return {"ERR_TOO_MANY_LITERALS", "Too many literals in the query"};
    case Executor::ERR_STRING_CONST_IN_RESULTSET:
      return {"ERR_STRING_CONST_IN_RESULTSET",
              "NONE ENCODED String types are not supported as input result set."};
    case Executor::ERR_SINGLE_VALUE_FOUND_MULTIPLE_VALUES:
      return {"ERR_SINGLE_VALUE_FOUND_MULTIPLE_VALUES",
              "Multiple distinct values encountered"};
    case Executor::ERR_WIDTH_BUCKET_INVALID_ARGUMENT:
      return {"ERR_WIDTH_BUCKET_INVALID_ARGUMENT",
              "Arguments of WIDTH_BUCKET function does not satisfy the condition"};
    default:
      return {nullptr, nullptr};
  }
}

}  // namespace

std::string RelAlgExecutor::getErrorMessageFromCode(const int32_t error_code) {
  if (error_code < 0) {
    return "Ran out of slots in the query output buffer";
  }
  const auto errorInfo = getErrorDescription(error_code);

  if (errorInfo.code) {
    return errorInfo.code + ": "s + errorInfo.description;
  } else {
    return "Other error: code "s + std::to_string(error_code);
  }
}

void RelAlgExecutor::executePostExecutionCallback() {
  if (post_execution_callback_) {
    VLOG(1) << "Running post execution callback.";
    (*post_execution_callback_)();
  }
}

RelAlgExecutor::WorkUnit RelAlgExecutor::createWorkUnit(const hdk::ir::Node* node,
                                                        const SortInfo& sort_info,
                                                        const ExecutionOptions& eo) {
  const auto compound = dynamic_cast<const hdk::ir::Compound*>(node);
  if (compound) {
    return createCompoundWorkUnit(compound, sort_info, eo);
  }
  const auto project = dynamic_cast<const hdk::ir::Project*>(node);
  if (project) {
    return createProjectWorkUnit(project, sort_info, eo);
  }
  const auto aggregate = dynamic_cast<const hdk::ir::Aggregate*>(node);
  if (aggregate) {
    return createAggregateWorkUnit(aggregate, sort_info, eo.just_explain);
  }
  const auto filter = dynamic_cast<const hdk::ir::Filter*>(node);
  if (filter) {
    return createFilterWorkUnit(filter, sort_info, eo.just_explain);
  }
  LOG(FATAL) << "Unhandled node type: " << node->toString();
  throw std::logic_error("Unexpected node type: " + node->toString());
}

namespace {

JoinType get_join_type(const hdk::ir::Node* ra) {
  auto sink = get_data_sink(ra);
  if (auto join = dynamic_cast<const hdk::ir::Join*>(sink)) {
    return join->getJoinType();
  }
  if (dynamic_cast<const hdk::ir::LeftDeepInnerJoin*>(sink)) {
    return JoinType::INNER;
  }

  return JoinType::INVALID;
}

hdk::ir::ExprPtr get_bitwise_equals(const hdk::ir::Expr* expr) {
  const auto condition = dynamic_cast<const hdk::ir::BinOper*>(expr);
  if (!condition || !condition->isOr()) {
    return nullptr;
  }
  const hdk::ir::BinOper* equi_join_condition = nullptr;
  const hdk::ir::BinOper* both_are_null_condition = nullptr;

  if (auto bin_oper = dynamic_cast<const hdk::ir::BinOper*>(condition->leftOperand())) {
    if (bin_oper->isEq()) {
      equi_join_condition = bin_oper;
    } else if (bin_oper->isAnd()) {
      both_are_null_condition = bin_oper;
    }
  }

  if (auto bin_oper = dynamic_cast<const hdk::ir::BinOper*>(condition->rightOperand())) {
    if (bin_oper->isEq()) {
      equi_join_condition = bin_oper;
    } else if (bin_oper->isAnd()) {
      both_are_null_condition = bin_oper;
    }
  }

  if (!equi_join_condition || !both_are_null_condition) {
    return nullptr;
  }

  auto lhs_is_null =
      dynamic_cast<const hdk::ir::UOper*>(both_are_null_condition->leftOperand());
  auto rhs_is_null =
      dynamic_cast<const hdk::ir::UOper*>(both_are_null_condition->rightOperand());
  if (!lhs_is_null || !rhs_is_null || !lhs_is_null->isIsNull() ||
      !rhs_is_null->isIsNull()) {
    return nullptr;
  }

  auto eq_lhs =
      dynamic_cast<const hdk::ir::ColumnRef*>(equi_join_condition->leftOperand());
  auto eq_rhs =
      dynamic_cast<const hdk::ir::ColumnRef*>(equi_join_condition->rightOperand());
  if (auto cast =
          dynamic_cast<const hdk::ir::UOper*>(equi_join_condition->leftOperand())) {
    eq_lhs = dynamic_cast<const hdk::ir::ColumnRef*>(cast->operand());
  }
  if (auto cast =
          dynamic_cast<const hdk::ir::UOper*>(equi_join_condition->rightOperand())) {
    eq_rhs = dynamic_cast<const hdk::ir::ColumnRef*>(cast->operand());
  }

  auto is_null_lhs = dynamic_cast<const hdk::ir::ColumnRef*>(lhs_is_null->operand());
  auto is_null_rhs = dynamic_cast<const hdk::ir::ColumnRef*>(rhs_is_null->operand());
  if (!eq_lhs || !eq_rhs || !is_null_lhs || !is_null_rhs) {
    return nullptr;
  }
  if ((*eq_lhs == *is_null_lhs && *eq_rhs == *is_null_rhs) ||
      (*eq_lhs == *is_null_rhs && *eq_rhs == *is_null_lhs)) {
    return hdk::ir::makeExpr<hdk::ir::BinOper>(expr->ctx().boolean(),
                                               hdk::ir::OpType::kBwEq,
                                               hdk::ir::Qualifier::kOne,
                                               equi_join_condition->leftOperandShared(),
                                               equi_join_condition->rightOperandShared());
  }
  return nullptr;
}

hdk::ir::ExprPtr get_bitwise_equals_conjunction(const hdk::ir::Expr* expr) {
  const auto condition = dynamic_cast<const hdk::ir::BinOper*>(expr);
  if (condition && condition->isAnd()) {
    auto acc = get_bitwise_equals(condition->leftOperand());
    if (!acc) {
      return nullptr;
    }
    return hdk::ir::makeExpr<hdk::ir::BinOper>(
        expr->ctx().boolean(),
        hdk::ir::OpType::kAnd,
        hdk::ir::Qualifier::kOne,
        acc,
        get_bitwise_equals_conjunction(condition->rightOperand()));
  }
  return get_bitwise_equals(expr);
}

std::vector<JoinType> left_deep_join_types(
    const hdk::ir::LeftDeepInnerJoin* left_deep_join) {
  CHECK_GE(left_deep_join->inputCount(), size_t(2));
  std::vector<JoinType> join_types(left_deep_join->inputCount() - 1, JoinType::INNER);
  for (size_t nesting_level = 1; nesting_level <= left_deep_join->inputCount() - 1;
       ++nesting_level) {
    if (left_deep_join->getOuterCondition(nesting_level)) {
      join_types[nesting_level - 1] = JoinType::LEFT;
    }
    auto cur_level_join_type = left_deep_join->getJoinType(nesting_level);
    if (cur_level_join_type == JoinType::SEMI || cur_level_join_type == JoinType::ANTI) {
      join_types[nesting_level - 1] = cur_level_join_type;
    }
  }
  return join_types;
}

template <class RA>
std::vector<size_t> do_table_reordering(
    std::vector<InputDescriptor>& input_descs,
    std::list<std::shared_ptr<const InputColDescriptor>>& input_col_descs,
    const JoinQualsPerNestingLevel& left_deep_join_quals,
    std::unordered_map<const hdk::ir::Node*, int>& input_to_nest_level,
    const RA* node,
    const std::vector<InputTableInfo>& query_infos,
    const Executor* executor) {
  for (const auto& table_info : query_infos) {
    if (table_info.table_id < 0) {
      continue;
    }
  }
  const auto input_permutation =
      get_node_input_permutation(left_deep_join_quals, query_infos, executor);
  input_to_nest_level = get_input_nest_levels(node, input_permutation);
  std::tie(input_descs, input_col_descs) =
      get_input_desc(node, input_to_nest_level, input_permutation);
  return input_permutation;
}

std::vector<size_t> get_left_deep_join_input_sizes(
    const hdk::ir::LeftDeepInnerJoin* left_deep_join) {
  std::vector<size_t> input_sizes;
  for (size_t i = 0; i < left_deep_join->inputCount(); ++i) {
    auto input_size = getNodeColumnCount(left_deep_join->getInput(i));
    input_sizes.push_back(input_size);
  }
  return input_sizes;
}

std::list<hdk::ir::ExprPtr> rewrite_quals(const std::list<hdk::ir::ExprPtr>& quals) {
  std::list<hdk::ir::ExprPtr> rewritten_quals;
  for (const auto& qual : quals) {
    const auto rewritten_qual = rewrite_expr(qual.get());
    rewritten_quals.push_back(rewritten_qual ? rewritten_qual : qual);
  }
  return rewritten_quals;
}

}  // namespace

RelAlgExecutor::WorkUnit RelAlgExecutor::createCompoundWorkUnit(
    const hdk::ir::Compound* compound,
    const SortInfo& sort_info,
    const ExecutionOptions& eo) {
  std::vector<InputDescriptor> input_descs;
  std::list<std::shared_ptr<const InputColDescriptor>> input_col_descs;
  auto input_to_nest_level = get_input_nest_levels(compound, {});
  std::tie(input_descs, input_col_descs) =
      get_input_desc(compound, input_to_nest_level, {});
  VLOG(3) << "input_descs=" << shared::printContainer(input_descs);
  const auto query_infos = get_table_infos(input_descs, executor_);
  CHECK_EQ(size_t(1), compound->inputCount());
  const auto left_deep_join =
      dynamic_cast<const hdk::ir::LeftDeepInnerJoin*>(compound->getInput(0));
  JoinQualsPerNestingLevel left_deep_join_quals;
  const auto join_types = left_deep_join ? left_deep_join_types(left_deep_join)
                                         : std::vector<JoinType>{get_join_type(compound)};
  std::vector<size_t> input_permutation;
  std::vector<size_t> left_deep_join_input_sizes;
  std::optional<unsigned> left_deep_tree_id;
  if (left_deep_join) {
    left_deep_tree_id = left_deep_join->getId();
    left_deep_join_input_sizes = get_left_deep_join_input_sizes(left_deep_join);
    left_deep_join_quals = translateLeftDeepJoinFilter(
        left_deep_join, input_descs, input_to_nest_level, eo.just_explain);
    if (config_.opts.from_table_reordering &&
        std::find(join_types.begin(), join_types.end(), JoinType::LEFT) ==
            join_types.end()) {
      input_permutation = do_table_reordering(input_descs,
                                              input_col_descs,
                                              left_deep_join_quals,
                                              input_to_nest_level,
                                              compound,
                                              query_infos,
                                              executor_);
      input_to_nest_level = get_input_nest_levels(compound, input_permutation);
      std::tie(input_descs, input_col_descs) =
          get_input_desc(compound, input_to_nest_level, input_permutation);
      left_deep_join_quals = translateLeftDeepJoinFilter(
          left_deep_join, input_descs, input_to_nest_level, eo.just_explain);
    }
  }
  RelAlgTranslator translator(
      executor_, input_to_nest_level, join_types, now_, eo.just_explain);
  const auto groupby_exprs =
      translate_groupby_exprs(compound, translator, eo.executor_type);
  const auto quals_cf = translate_quals(compound, translator);
  const auto target_exprs = translate_targets(target_exprs_owned_,
                                              groupby_exprs,
                                              compound,
                                              translator,
                                              eo.executor_type,
                                              config_.exec.group_by.bigint_count);
  auto query_hint = RegisteredQueryHint::fromConfig(config_);
  if (query_dag_) {
    auto candidate = query_dag_->getQueryHint(compound);
    if (candidate) {
      query_hint = *candidate;
    }
  }
  CHECK_EQ(compound->size(), target_exprs.size());
  const RelAlgExecutionUnit exe_unit = {input_descs,
                                        input_col_descs,
                                        quals_cf.simple_quals,
                                        rewrite_quals(quals_cf.quals),
                                        left_deep_join_quals,
                                        groupby_exprs,
                                        target_exprs,
                                        nullptr,
                                        sort_info,
                                        0,
                                        query_hint,
                                        EMPTY_QUERY_PLAN,
                                        {},
                                        {},
                                        false,
                                        std::nullopt};
  auto query_rewriter = std::make_unique<QueryRewriter>(query_infos, executor_);
  auto rewritten_exe_unit = query_rewriter->rewrite(exe_unit);
  const auto targets_meta = get_targets_meta(compound, rewritten_exe_unit.target_exprs);
  compound->setOutputMetainfo(targets_meta);
  auto& left_deep_trees_info = getLeftDeepJoinTreesInfo();
  if (left_deep_tree_id && left_deep_tree_id.has_value()) {
    left_deep_trees_info.emplace(left_deep_tree_id.value(),
                                 rewritten_exe_unit.join_quals);
  }
  auto dag_info = QueryPlanDagExtractor::extractQueryPlanDag(compound,
                                                             schema_provider_,
                                                             left_deep_tree_id,
                                                             left_deep_trees_info,
                                                             temporary_tables_,
                                                             executor_,
                                                             translator);
  if (is_extracted_dag_valid(dag_info)) {
    rewritten_exe_unit.query_plan_dag = dag_info.extracted_dag;
    rewritten_exe_unit.hash_table_build_plan_dag = dag_info.hash_table_plan_dag;
    rewritten_exe_unit.table_id_to_node_map = dag_info.table_id_to_node_map;
  }
  return {rewritten_exe_unit,
          compound,
          config_.exec.group_by.default_max_groups_buffer_entry_guess,
          std::move(query_rewriter),
          input_permutation,
          left_deep_join_input_sizes};
}

std::shared_ptr<RelAlgTranslator> RelAlgExecutor::getRelAlgTranslator(
    const hdk::ir::Node* node) {
  auto input_to_nest_level = get_input_nest_levels(node, {});
  const auto left_deep_join =
      dynamic_cast<const hdk::ir::LeftDeepInnerJoin*>(node->getInput(0));
  const auto join_types = left_deep_join ? left_deep_join_types(left_deep_join)
                                         : std::vector<JoinType>{get_join_type(node)};
  return std::make_shared<RelAlgTranslator>(
      executor_, input_to_nest_level, join_types, now_, false);
}

namespace {

hdk::ir::ExprPtr build_logical_expression(const std::vector<hdk::ir::ExprPtr>& factors,
                                          hdk::ir::OpType sql_op) {
  CHECK(!factors.empty());
  auto acc = factors.front();
  for (size_t i = 1; i < factors.size(); ++i) {
    acc = Analyzer::normalizeOperExpr(sql_op, hdk::ir::Qualifier::kOne, acc, factors[i]);
  }
  return acc;
}

template <class QualsList>
bool list_contains_expression(const QualsList& haystack, const hdk::ir::ExprPtr& needle) {
  for (const auto& qual : haystack) {
    if (*qual == *needle) {
      return true;
    }
  }
  return false;
}

// Transform `(p AND q) OR (p AND r)` to `p AND (q OR r)`. Avoids redundant
// evaluations of `p` and allows use of the original form in joins if `p`
// can be used for hash joins.
hdk::ir::ExprPtr reverse_logical_distribution(const hdk::ir::ExprPtr& expr) {
  const auto expr_terms = qual_to_disjunctive_form(expr);
  CHECK_GE(expr_terms.size(), size_t(1));
  const auto& first_term = expr_terms.front();
  const auto first_term_factors = qual_to_conjunctive_form(first_term);
  std::vector<hdk::ir::ExprPtr> common_factors;
  // First, collect the conjunctive components common to all the disjunctive
  // components. Don't do it for simple qualifiers, we only care about expensive or
  // join qualifiers.
  for (const auto& first_term_factor : first_term_factors.quals) {
    bool is_common =
        expr_terms.size() > 1;  // Only report common factors for disjunction.
    for (size_t i = 1; i < expr_terms.size(); ++i) {
      const auto crt_term_factors = qual_to_conjunctive_form(expr_terms[i]);
      if (!list_contains_expression(crt_term_factors.quals, first_term_factor)) {
        is_common = false;
        break;
      }
    }
    if (is_common) {
      common_factors.push_back(first_term_factor);
    }
  }
  if (common_factors.empty()) {
    return expr;
  }
  // Now that the common expressions are known, collect the remaining expressions.
  std::vector<hdk::ir::ExprPtr> remaining_terms;
  for (const auto& term : expr_terms) {
    const auto term_cf = qual_to_conjunctive_form(term);
    std::vector<hdk::ir::ExprPtr> remaining_quals(term_cf.simple_quals.begin(),
                                                  term_cf.simple_quals.end());
    for (const auto& qual : term_cf.quals) {
      if (!list_contains_expression(common_factors, qual)) {
        remaining_quals.push_back(qual);
      }
    }
    if (!remaining_quals.empty()) {
      remaining_terms.push_back(
          build_logical_expression(remaining_quals, hdk::ir::OpType::kAnd));
    }
  }
  // Reconstruct the expression with the transformation applied.
  const auto common_expr =
      build_logical_expression(common_factors, hdk::ir::OpType::kAnd);
  if (remaining_terms.empty()) {
    return common_expr;
  }
  const auto remaining_expr =
      build_logical_expression(remaining_terms, hdk::ir::OpType::kOr);
  return Analyzer::normalizeOperExpr(
      hdk::ir::OpType::kAnd, hdk::ir::Qualifier::kOne, common_expr, remaining_expr);
}

}  // namespace

std::list<hdk::ir::ExprPtr> RelAlgExecutor::makeJoinQuals(
    const hdk::ir::Expr* join_condition,
    const std::vector<JoinType>& join_types,
    const std::unordered_map<const hdk::ir::Node*, int>& input_to_nest_level,
    const bool just_explain) const {
  RelAlgTranslator translator(
      executor_, input_to_nest_level, join_types, now_, just_explain);

  std::list<hdk::ir::ExprPtr> join_condition_quals;
  auto bw_equals = get_bitwise_equals_conjunction(join_condition);
  auto condition_expr =
      translator.normalize(bw_equals ? bw_equals.get() : join_condition);
  condition_expr = reverse_logical_distribution(condition_expr);
  auto join_condition_cf = qual_to_conjunctive_form(condition_expr);
  join_condition_quals.insert(join_condition_quals.end(),
                              join_condition_cf.quals.begin(),
                              join_condition_cf.quals.end());
  join_condition_quals.insert(join_condition_quals.end(),
                              join_condition_cf.simple_quals.begin(),
                              join_condition_cf.simple_quals.end());

  return combine_equi_join_conditions(join_condition_quals);
}

// Translate left deep join filter and separate the conjunctive form qualifiers
// per nesting level. The code generated for hash table lookups on each level
// must dominate its uses in deeper nesting levels.
JoinQualsPerNestingLevel RelAlgExecutor::translateLeftDeepJoinFilter(
    const hdk::ir::LeftDeepInnerJoin* join,
    const std::vector<InputDescriptor>& input_descs,
    const std::unordered_map<const hdk::ir::Node*, int>& input_to_nest_level,
    const bool just_explain) {
  const auto join_types = left_deep_join_types(join);
  const auto join_condition_quals = makeJoinQuals(
      join->getInnerCondition(), join_types, input_to_nest_level, just_explain);
  JoinQualsPerNestingLevel result(input_descs.size() - 1);
  std::unordered_set<hdk::ir::ExprPtr> visited_quals;
  for (size_t rte_idx = 1; rte_idx < input_descs.size(); ++rte_idx) {
    const auto outer_condition = join->getOuterCondition(rte_idx);
    if (outer_condition) {
      result[rte_idx - 1].quals =
          makeJoinQuals(outer_condition, join_types, input_to_nest_level, just_explain);
      CHECK_LE(rte_idx, join_types.size());
      CHECK(join_types[rte_idx - 1] == JoinType::LEFT);
      result[rte_idx - 1].type = JoinType::LEFT;
      continue;
    }
    for (const auto& qual : join_condition_quals) {
      if (visited_quals.count(qual)) {
        continue;
      }
      const auto qual_rte_idx = MaxRangeTableIndexCollector::collect(qual.get());
      if (static_cast<size_t>(qual_rte_idx) <= rte_idx) {
        const auto it_ok = visited_quals.emplace(qual);
        CHECK(it_ok.second);
        result[rte_idx - 1].quals.push_back(qual);
      }
    }
    CHECK_LE(rte_idx, join_types.size());
    CHECK(join_types[rte_idx - 1] == JoinType::INNER ||
          join_types[rte_idx - 1] == JoinType::SEMI ||
          join_types[rte_idx - 1] == JoinType::ANTI);
    result[rte_idx - 1].type = join_types[rte_idx - 1];
  }
  return result;
}

namespace {

std::vector<hdk::ir::ExprPtr> synthesize_inputs(
    const hdk::ir::Node* ra_node,
    const size_t nest_level,
    const std::vector<TargetMetaInfo>& in_metainfo,
    const std::unordered_map<const hdk::ir::Node*, int>& input_to_nest_level) {
  CHECK_LE(size_t(1), ra_node->inputCount());
  CHECK_GE(size_t(2), ra_node->inputCount());
  const auto input = ra_node->getInput(nest_level);
  const auto it_rte_idx = input_to_nest_level.find(input);
  CHECK(it_rte_idx != input_to_nest_level.end());
  const int rte_idx = it_rte_idx->second;
  const int table_id = table_id_from_ra(input);
  std::vector<hdk::ir::ExprPtr> inputs;
  const auto scan_ra = dynamic_cast<const hdk::ir::Scan*>(input);
  int input_idx = 0;
  for (const auto& input_meta : in_metainfo) {
    inputs.push_back(std::make_shared<hdk::ir::ColumnVar>(
        input_meta.type(),
        table_id,
        scan_ra ? input_idx + 1 : input_idx,
        rte_idx,
        scan_ra ? scan_ra->isVirtualCol(input_idx) : false));
    ++input_idx;
  }
  return inputs;
}

}  // namespace

RelAlgExecutor::WorkUnit RelAlgExecutor::createAggregateWorkUnit(
    const hdk::ir::Aggregate* aggregate,
    const SortInfo& sort_info,
    const bool just_explain) {
  std::vector<InputDescriptor> input_descs;
  std::list<std::shared_ptr<const InputColDescriptor>> input_col_descs;
  const auto input_to_nest_level = get_input_nest_levels(aggregate, {});
  std::tie(input_descs, input_col_descs) =
      get_input_desc(aggregate, input_to_nest_level, {});
  const auto join_type = get_join_type(aggregate);

  RelAlgTranslator translator(
      executor_, input_to_nest_level, {join_type}, now_, just_explain);
  CHECK_EQ(size_t(1), aggregate->inputCount());
  const auto source = aggregate->getInput(0);
  const auto& in_metainfo = source->getOutputMetainfo();
  const auto scalar_sources =
      synthesize_inputs(aggregate, size_t(0), in_metainfo, input_to_nest_level);
  const auto groupby_exprs = translate_groupby_exprs(aggregate, scalar_sources);
  const auto target_exprs = translate_targets(target_exprs_owned_,
                                              scalar_sources,
                                              groupby_exprs,
                                              aggregate,
                                              translator,
                                              config_.exec.group_by.bigint_count);
  const auto targets_meta = get_targets_meta(aggregate, target_exprs);
  aggregate->setOutputMetainfo(targets_meta);
  auto dag_info = QueryPlanDagExtractor::extractQueryPlanDag(aggregate,
                                                             schema_provider_,
                                                             std::nullopt,
                                                             getLeftDeepJoinTreesInfo(),
                                                             temporary_tables_,
                                                             executor_,
                                                             translator);
  auto query_hint = RegisteredQueryHint::fromConfig(config_);
  if (query_dag_) {
    auto candidate = query_dag_->getQueryHint(aggregate);
    if (candidate) {
      query_hint = *candidate;
    }
  }
  return {RelAlgExecutionUnit{input_descs,
                              input_col_descs,
                              {},
                              {},
                              {},
                              groupby_exprs,
                              target_exprs,
                              nullptr,
                              sort_info,
                              0,
                              query_hint,
                              dag_info.extracted_dag,
                              dag_info.hash_table_plan_dag,
                              dag_info.table_id_to_node_map,
                              false,
                              std::nullopt},
          aggregate,
          config_.exec.group_by.default_max_groups_buffer_entry_guess,
          nullptr};
}

RelAlgExecutor::WorkUnit RelAlgExecutor::createProjectWorkUnit(
    const hdk::ir::Project* project,
    const SortInfo& sort_info,
    const ExecutionOptions& eo) {
  std::vector<InputDescriptor> input_descs;
  std::list<std::shared_ptr<const InputColDescriptor>> input_col_descs;
  auto input_to_nest_level = get_input_nest_levels(project, {});
  std::tie(input_descs, input_col_descs) =
      get_input_desc(project, input_to_nest_level, {});
  const auto query_infos = get_table_infos(input_descs, executor_);

  const auto left_deep_join =
      dynamic_cast<const hdk::ir::LeftDeepInnerJoin*>(project->getInput(0));
  JoinQualsPerNestingLevel left_deep_join_quals;
  const auto join_types = left_deep_join ? left_deep_join_types(left_deep_join)
                                         : std::vector<JoinType>{get_join_type(project)};
  std::vector<size_t> input_permutation;
  std::vector<size_t> left_deep_join_input_sizes;
  std::optional<unsigned> left_deep_tree_id;
  if (left_deep_join) {
    left_deep_tree_id = left_deep_join->getId();
    left_deep_join_input_sizes = get_left_deep_join_input_sizes(left_deep_join);
    const auto query_infos = get_table_infos(input_descs, executor_);
    left_deep_join_quals = translateLeftDeepJoinFilter(
        left_deep_join, input_descs, input_to_nest_level, eo.just_explain);
    if (config_.opts.from_table_reordering) {
      input_permutation = do_table_reordering(input_descs,
                                              input_col_descs,
                                              left_deep_join_quals,
                                              input_to_nest_level,
                                              project,
                                              query_infos,
                                              executor_);
      input_to_nest_level = get_input_nest_levels(project, input_permutation);
      std::tie(input_descs, input_col_descs) =
          get_input_desc(project, input_to_nest_level, input_permutation);
      left_deep_join_quals = translateLeftDeepJoinFilter(
          left_deep_join, input_descs, input_to_nest_level, eo.just_explain);
    }
  }

  RelAlgTranslator translator(
      executor_, input_to_nest_level, join_types, now_, eo.just_explain);
  std::vector<const hdk::ir::Expr*> target_exprs;
  for (auto& expr : project->getExprs()) {
    auto target_expr = translate(expr.get(), translator, eo.executor_type);
    target_exprs.push_back(target_expr.get());
    target_exprs_owned_.emplace_back(std::move(target_expr));
  }
  auto query_hint = RegisteredQueryHint::fromConfig(config_);
  if (query_dag_) {
    auto candidate = query_dag_->getQueryHint(project);
    if (candidate) {
      query_hint = *candidate;
    }
  }
  const RelAlgExecutionUnit exe_unit = {input_descs,
                                        input_col_descs,
                                        {},
                                        {},
                                        left_deep_join_quals,
                                        {nullptr},
                                        target_exprs,
                                        nullptr,
                                        sort_info,
                                        0,
                                        query_hint,
                                        EMPTY_QUERY_PLAN,
                                        {},
                                        {},
                                        false,
                                        std::nullopt};
  auto query_rewriter = std::make_unique<QueryRewriter>(query_infos, executor_);
  auto rewritten_exe_unit = query_rewriter->rewrite(exe_unit);
  const auto targets_meta = get_targets_meta(project, rewritten_exe_unit.target_exprs);
  project->setOutputMetainfo(targets_meta);
  auto& left_deep_trees_info = getLeftDeepJoinTreesInfo();
  if (left_deep_tree_id && left_deep_tree_id.has_value()) {
    left_deep_trees_info.emplace(left_deep_tree_id.value(),
                                 rewritten_exe_unit.join_quals);
  }
  auto dag_info = QueryPlanDagExtractor::extractQueryPlanDag(project,
                                                             schema_provider_,
                                                             left_deep_tree_id,
                                                             left_deep_trees_info,
                                                             temporary_tables_,
                                                             executor_,
                                                             translator);
  if (is_extracted_dag_valid(dag_info)) {
    rewritten_exe_unit.query_plan_dag = dag_info.extracted_dag;
    rewritten_exe_unit.hash_table_build_plan_dag = dag_info.hash_table_plan_dag;
    rewritten_exe_unit.table_id_to_node_map = dag_info.table_id_to_node_map;
  }
  return {rewritten_exe_unit,
          project,
          config_.exec.group_by.default_max_groups_buffer_entry_guess,
          std::move(query_rewriter),
          input_permutation,
          left_deep_join_input_sizes};
}

namespace {

std::vector<hdk::ir::ExprPtr> target_exprs_for_union(hdk::ir::Node const* input_node) {
  std::vector<TargetMetaInfo> const& tmis = input_node->getOutputMetainfo();
  VLOG(3) << "input_node->getOutputMetainfo()=" << shared::printContainer(tmis);
  const int negative_node_id = -input_node->getId();
  std::vector<hdk::ir::ExprPtr> target_exprs;
  target_exprs.reserve(tmis.size());
  for (size_t i = 0; i < tmis.size(); ++i) {
    target_exprs.push_back(
        std::make_shared<hdk::ir::ColumnVar>(tmis[i].type(), negative_node_id, i, 0));
  }
  return target_exprs;
}

}  // namespace

RelAlgExecutor::WorkUnit RelAlgExecutor::createUnionWorkUnit(
    const hdk::ir::LogicalUnion* logical_union,
    const SortInfo& sort_info,
    const ExecutionOptions& eo) {
  std::vector<InputDescriptor> input_descs;
  std::list<std::shared_ptr<const InputColDescriptor>> input_col_descs;
  // Map ra input ptr to index (0, 1).
  auto input_to_nest_level = get_input_nest_levels(logical_union, {});
  std::tie(input_descs, input_col_descs) =
      get_input_desc(logical_union, input_to_nest_level, {});
  const auto query_infos = get_table_infos(input_descs, executor_);
  auto const max_num_tuples =
      std::accumulate(query_infos.cbegin(),
                      query_infos.cend(),
                      size_t(0),
                      [](auto max, auto const& query_info) {
                        return std::max(max, query_info.info.getNumTuples());
                      });

  VLOG(3) << "input_to_nest_level.size()=" << input_to_nest_level.size() << " Pairs are:";
  for (auto& pair : input_to_nest_level) {
    VLOG(3) << "  (" << pair.first->toString() << ", " << pair.second << ')';
  }

  RelAlgTranslator translator(executor_, input_to_nest_level, {}, now_, eo.just_explain);

  auto const input_exprs_owned = target_exprs_for_union(logical_union->getInput(0));
  CHECK(!input_exprs_owned.empty())
      << "No metainfo found for input node " << logical_union->getInput(0)->toString();
  VLOG(3) << "input_exprs_owned.size()=" << input_exprs_owned.size();
  for (auto& input_expr : input_exprs_owned) {
    VLOG(3) << "  " << input_expr->toString();
  }
  target_exprs_owned_.insert(
      target_exprs_owned_.end(), input_exprs_owned.begin(), input_exprs_owned.end());
  const auto target_exprs = get_exprs_not_owned(input_exprs_owned);

  VLOG(3) << "input_descs=" << shared::printContainer(input_descs)
          << " input_col_descs=" << shared::printContainer(input_col_descs)
          << " target_exprs.size()=" << target_exprs.size()
          << " max_num_tuples=" << max_num_tuples;

  const RelAlgExecutionUnit exe_unit = {input_descs,
                                        input_col_descs,
                                        {},  // quals_cf.simple_quals,
                                        {},  // rewrite_quals(quals_cf.quals),
                                        {},
                                        {nullptr},
                                        target_exprs,
                                        nullptr,
                                        sort_info,
                                        max_num_tuples,
                                        RegisteredQueryHint::fromConfig(config_),
                                        EMPTY_QUERY_PLAN,
                                        {},
                                        {},
                                        false,
                                        logical_union->isAll()};
  auto query_rewriter = std::make_unique<QueryRewriter>(query_infos, executor_);
  const auto rewritten_exe_unit = query_rewriter->rewrite(exe_unit);

  hdk::ir::Node const* input0 = logical_union->getInput(0);
  if (auto const* node = dynamic_cast<const hdk::ir::Compound*>(input0)) {
    logical_union->setOutputMetainfo(
        get_targets_meta(node, rewritten_exe_unit.target_exprs));
  } else if (auto const* node = dynamic_cast<const hdk::ir::Project*>(input0)) {
    logical_union->setOutputMetainfo(
        get_targets_meta(node, rewritten_exe_unit.target_exprs));
  } else if (auto const* node = dynamic_cast<const hdk::ir::LogicalUnion*>(input0)) {
    logical_union->setOutputMetainfo(
        get_targets_meta(node, rewritten_exe_unit.target_exprs));
  } else if (auto const* node = dynamic_cast<const hdk::ir::Aggregate*>(input0)) {
    logical_union->setOutputMetainfo(
        get_targets_meta(node, rewritten_exe_unit.target_exprs));
  } else if (auto const* node = dynamic_cast<const hdk::ir::Scan*>(input0)) {
    logical_union->setOutputMetainfo(
        get_targets_meta(node, rewritten_exe_unit.target_exprs));
  } else if (auto const* node = dynamic_cast<const hdk::ir::Filter*>(input0)) {
    logical_union->setOutputMetainfo(
        get_targets_meta(node, rewritten_exe_unit.target_exprs));
  } else if (dynamic_cast<const hdk::ir::Sort*>(input0)) {
    throw hdk::ir::QueryNotSupported(
        "LIMIT and OFFSET are not currently supported with UNION.");
  } else {
    throw hdk::ir::QueryNotSupported("Unsupported input type: " + input0->toString());
  }
  VLOG(3) << "logical_union->getOutputMetainfo()="
          << shared::printContainer(logical_union->getOutputMetainfo())
          << " rewritten_exe_unit.input_col_descs.front()->getTableId()="
          << rewritten_exe_unit.input_col_descs.front()->getTableId();

  return {rewritten_exe_unit,
          logical_union,
          config_.exec.group_by.default_max_groups_buffer_entry_guess,
          std::move(query_rewriter)};
}

RelAlgExecutor::TableFunctionWorkUnit RelAlgExecutor::createTableFunctionWorkUnit(
    const hdk::ir::TableFunction* rel_table_func,
    const bool just_explain,
    const bool is_gpu) {
  std::vector<InputDescriptor> input_descs;
  std::list<std::shared_ptr<const InputColDescriptor>> input_col_descs;
  auto input_to_nest_level = get_input_nest_levels(rel_table_func, {});
  std::tie(input_descs, input_col_descs) =
      get_input_desc(rel_table_func, input_to_nest_level, {});
  const auto query_infos = get_table_infos(input_descs, executor_);
  RelAlgTranslator translator(executor_, input_to_nest_level, {}, now_, just_explain);

  hdk::ir::ExprPtrVector input_exprs_owned;
  for (auto& expr : rel_table_func->getTableFuncInputExprs()) {
    input_exprs_owned.push_back(
        translate(expr.get(), translator, ::ExecutorType::TableFunctions));
  }

  target_exprs_owned_.insert(
      target_exprs_owned_.end(), input_exprs_owned.begin(), input_exprs_owned.end());
  auto input_exprs = get_exprs_not_owned(input_exprs_owned);

  const auto table_function_impl_and_types = [=]() {
    if (is_gpu) {
      try {
        return bind_table_function(
            rel_table_func->getFunctionName(), input_exprs_owned, is_gpu);
      } catch (ExtensionFunctionBindingError& e) {
        LOG(WARNING) << "createTableFunctionWorkUnit[GPU]: " << e.what()
                     << " Redirecting " << rel_table_func->getFunctionName()
                     << " step to run on CPU.";
        throw QueryMustRunOnCpu();
      }
    } else {
      try {
        return bind_table_function(
            rel_table_func->getFunctionName(), input_exprs_owned, is_gpu);
      } catch (ExtensionFunctionBindingError& e) {
        LOG(WARNING) << "createTableFunctionWorkUnit[CPU]: " << e.what();
        throw;
      }
    }
  }();
  const auto& table_function_impl = std::get<0>(table_function_impl_and_types);
  const auto& table_function_types = std::get<1>(table_function_impl_and_types);

  size_t output_row_sizing_param = 0;
  if (table_function_impl
          .hasUserSpecifiedOutputSizeParameter()) {  // constant and row multiplier
    const auto parameter_index =
        table_function_impl.getOutputRowSizeParameter(table_function_types);
    CHECK_GT(parameter_index, size_t(0));
    if (rel_table_func->countConstantArgs() == table_function_impl.countScalarArgs()) {
      auto param_expr = rel_table_func->getTableFuncInputExprAt(parameter_index - 1);
      auto param_const = dynamic_cast<const hdk::ir::Constant*>(param_expr);
      if (!param_const) {
        throw std::runtime_error(
            "Provided output buffer sizing parameter is not a literal. Only literal "
            "values are supported with output buffer sizing configured table "
            "functions.");
      }
      if (!param_const->type()->isInteger()) {
        throw std::runtime_error(
            "Output buffer sizing parameter should have integer type.");
      }
      int64_t literal_val = param_const->intVal();
      if (literal_val < 0) {
        throw std::runtime_error("Provided output sizing parameter " +
                                 std::to_string(literal_val) +
                                 " must be positive integer.");
      }
      output_row_sizing_param = static_cast<size_t>(literal_val);
    } else {
      // RowMultiplier not specified in the SQL query. Set it to 1
      output_row_sizing_param = 1;  // default value for RowMultiplier
      static Datum d = {DEFAULT_ROW_MULTIPLIER_VALUE};
      static auto DEFAULT_ROW_MULTIPLIER_EXPR = hdk::ir::makeExpr<hdk::ir::Constant>(
          hdk::ir::Context::defaultCtx().int32(false), false, d);
      // Push the constant 1 to input_exprs
      input_exprs.insert(input_exprs.begin() + parameter_index - 1,
                         DEFAULT_ROW_MULTIPLIER_EXPR.get());
    }
  } else if (table_function_impl.hasNonUserSpecifiedOutputSize()) {
    output_row_sizing_param = table_function_impl.getOutputRowSizeParameter();
  } else {
    UNREACHABLE();
  }

  std::vector<const hdk::ir::ColumnVar*> input_col_exprs;
  size_t input_index = 0;
  size_t arg_index = 0;
  const auto table_func_args = table_function_impl.getInputArgs();
  CHECK_EQ(table_func_args.size(), table_function_types.size());
  for (auto type : table_function_types) {
    if (type->isColumnList()) {
      for (int i = 0; i < type->as<hdk::ir::ColumnListType>()->length(); i++) {
        auto& input_expr = input_exprs[input_index];
        auto input_type = type->ctx().columnList(
            input_expr->type(), type->as<hdk::ir::ColumnListType>()->length());
        auto col_var = input_expr->withType(input_type);
        CHECK(col_var->is<hdk::ir::ColumnVar>());

        target_exprs_owned_.push_back(col_var);
        input_exprs[input_index] = col_var.get();
        input_col_exprs.push_back(col_var->as<hdk::ir::ColumnVar>());
        input_index++;
      }
    } else if (type->isColumn()) {
      auto& input_expr = input_exprs[input_index];
      auto input_type = type->ctx().column(input_expr->type());
      auto col_var = input_expr->withType(input_type);
      CHECK(col_var->is<hdk::ir::ColumnVar>());

      target_exprs_owned_.push_back(col_var);
      input_exprs[input_index] = col_var.get();
      input_col_exprs.push_back(col_var->as<hdk::ir::ColumnVar>());
      input_index++;
    } else {
      auto input_expr = input_exprs[input_index];
      auto ext_func_arg_type =
          ext_arg_type_to_type(input_expr->ctx(), table_func_args[arg_index]);
      if (!ext_func_arg_type->equal(input_expr->type())) {
        target_exprs_owned_.push_back(input_expr->cast(ext_func_arg_type));
        input_exprs[input_index] = target_exprs_owned_.back().get();
      }
      input_index++;
    }
    arg_index++;
  }
  CHECK_EQ(input_col_exprs.size(), rel_table_func->getColInputsSize());
  std::vector<const hdk::ir::Expr*> table_func_outputs;
  for (size_t i = 0; i < table_function_impl.getOutputsSize(); i++) {
    auto type = table_function_impl.getOutputType(i);
    if (type->isExtDictionary()) {
      auto p = table_function_impl.getInputID(i);

      int32_t input_pos = p.first;
      // Iterate over the list of arguments to compute the offset. Use this offset to
      // get the corresponding input
      int32_t offset = 0;
      for (int j = 0; j < input_pos; j++) {
        auto type = table_function_types[j];
        offset +=
            type->isColumnList() ? type->as<hdk::ir::ColumnListType>()->length() : 1;
      }
      input_pos = offset + p.second;

      CHECK_LT(input_pos, input_exprs.size());
      auto input_type = input_exprs[input_pos]->type();
      CHECK(input_type->isColumn()) << input_type->toString();
      int32_t comp_param = input_type->as<hdk::ir::ColumnType>()
                               ->columnType()
                               ->as<hdk::ir::ExtDictionaryType>()
                               ->dictId();
      type = type->ctx().extDict(type->as<hdk::ir::ExtDictionaryType>()->elemType(),
                                 comp_param);
    }
    target_exprs_owned_.push_back(std::make_shared<hdk::ir::ColumnVar>(type, 0, i, -1));
    table_func_outputs.push_back(target_exprs_owned_.back().get());
  }
  const TableFunctionExecutionUnit exe_unit = {
      input_descs,
      input_col_descs,
      input_exprs,              // table function inputs
      input_col_exprs,          // table function column inputs (duplicates w/ above)
      table_func_outputs,       // table function projected exprs
      output_row_sizing_param,  // output buffer sizing param
      table_function_impl};
  const auto targets_meta = get_targets_meta(rel_table_func, exe_unit.target_exprs);
  rel_table_func->setOutputMetainfo(targets_meta);
  return {exe_unit, rel_table_func};
}

namespace {

std::pair<std::vector<TargetMetaInfo>, std::vector<hdk::ir::ExprPtr>> get_inputs_meta(
    const hdk::ir::Filter* filter,
    const RelAlgTranslator& translator,
    const std::unordered_map<const hdk::ir::Node*, int>& input_to_nest_level) {
  std::vector<TargetMetaInfo> in_metainfo;
  std::vector<hdk::ir::ExprPtr> exprs_owned;
  const auto data_sink_node = get_data_sink(filter);
  for (size_t nest_level = 0; nest_level < data_sink_node->inputCount(); ++nest_level) {
    const auto source = data_sink_node->getInput(nest_level);
    const auto scan_source = dynamic_cast<const hdk::ir::Scan*>(source);
    if (scan_source) {
      CHECK(source->getOutputMetainfo().empty());
      std::vector<hdk::ir::ExprPtr> scalar_sources_owned;
      for (size_t i = 0; i < scan_source->size(); ++i) {
        scalar_sources_owned.push_back(translator.normalize(
            hdk::ir::makeExpr<hdk::ir::ColumnRef>(getColumnType(source, i), source, i)
                .get()));
      }
      const auto source_metadata =
          get_targets_meta(scan_source, get_exprs_not_owned(scalar_sources_owned));
      in_metainfo.insert(
          in_metainfo.end(), source_metadata.begin(), source_metadata.end());
      exprs_owned.insert(
          exprs_owned.end(), scalar_sources_owned.begin(), scalar_sources_owned.end());
    } else {
      const auto& source_metadata = source->getOutputMetainfo();
      in_metainfo.insert(
          in_metainfo.end(), source_metadata.begin(), source_metadata.end());
      const auto scalar_sources_owned = synthesize_inputs(
          data_sink_node, nest_level, source_metadata, input_to_nest_level);
      exprs_owned.insert(
          exprs_owned.end(), scalar_sources_owned.begin(), scalar_sources_owned.end());
    }
  }
  return std::make_pair(in_metainfo, exprs_owned);
}

}  // namespace

RelAlgExecutor::WorkUnit RelAlgExecutor::createFilterWorkUnit(
    const hdk::ir::Filter* filter,
    const SortInfo& sort_info,
    const bool just_explain) {
  CHECK_EQ(size_t(1), filter->inputCount());
  std::vector<InputDescriptor> input_descs;
  std::list<std::shared_ptr<const InputColDescriptor>> input_col_descs;
  std::vector<TargetMetaInfo> in_metainfo;
  std::vector<hdk::ir::ExprPtr> target_exprs_owned;

  const auto input_to_nest_level = get_input_nest_levels(filter, {});
  std::tie(input_descs, input_col_descs) =
      get_input_desc(filter, input_to_nest_level, {});
  const auto join_type = get_join_type(filter);
  RelAlgTranslator translator(
      executor_, input_to_nest_level, {join_type}, now_, just_explain);
  std::tie(in_metainfo, target_exprs_owned) =
      get_inputs_meta(filter, translator, input_to_nest_level);

  auto filter_expr = translator.normalize(filter->getConditionExpr());
  auto qual = fold_expr(filter_expr.get());

  target_exprs_owned_.insert(
      target_exprs_owned_.end(), target_exprs_owned.begin(), target_exprs_owned.end());
  const auto target_exprs = get_exprs_not_owned(target_exprs_owned);
  filter->setOutputMetainfo(in_metainfo);
  const auto rewritten_qual = rewrite_expr(qual.get());
  auto dag_info = QueryPlanDagExtractor::extractQueryPlanDag(filter,
                                                             schema_provider_,
                                                             std::nullopt,
                                                             getLeftDeepJoinTreesInfo(),
                                                             temporary_tables_,
                                                             executor_,
                                                             translator);
  auto query_hint = RegisteredQueryHint::fromConfig(config_);
  if (query_dag_) {
    auto candidate = query_dag_->getQueryHint(filter);
    if (candidate) {
      query_hint = *candidate;
    }
  }
  return {{input_descs,
           input_col_descs,
           {},
           {rewritten_qual ? rewritten_qual : qual},
           {},
           {nullptr},
           target_exprs,
           nullptr,
           sort_info,
           0,
           query_hint,
           dag_info.extracted_dag,
           dag_info.hash_table_plan_dag,
           dag_info.table_id_to_node_map},
          filter,
          config_.exec.group_by.default_max_groups_buffer_entry_guess,
          nullptr};
}

SpeculativeTopNBlacklist RelAlgExecutor::speculative_topn_blacklist_;
