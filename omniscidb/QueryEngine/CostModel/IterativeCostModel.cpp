#include "IterativeCostModel.h"
#include "Dispatchers/ProportionBasedExecutionPolicy.h"

#ifdef HAVE_DWARF_BENCH
#include "DataSources/DwarfBench.h"
#endif

#include <cmath>

namespace costmodel {

#ifdef HAVE_DWARF_BENCH
IterativeCostModel::IterativeCostModel() : CostModel({std::make_unique<DwarfBenchDataSource>()}) {}
#else
IterativeCostModel::IterativeCostModel() : CostModel({std::make_unique<EmptyDataSource>()}) {}
#endif

std::unique_ptr<policy::ExecutionPolicy> IterativeCostModel::predict(
    QueryInfo query_info) const {
        std::shared_lock<std::shared_mutex> l(latch_);

    unsigned cpu_prop, gpu_prop;
    size_t opt_step = std::ceil(static_cast<float>(query_info.bytes_size) / optimization_iterations_);
    size_t runtime_prediction = std::numeric_limits<size_t>::max();

    for (size_t cur_size = 0; cur_size < query_info.bytes_size; cur_size += opt_step) {
        size_t cpu_size = cur_size;
        size_t gpu_size = query_info.bytes_size - cur_size;
        
        size_t cpu_prediction = getExtrapolatedData(ExecutorDeviceType::CPU, query_info.templ, cpu_size);
        size_t gpu_prediction = getExtrapolatedData(ExecutorDeviceType::GPU, query_info.templ, gpu_size);
        size_t cur_prediction = std::max(gpu_prediction, cpu_prediction);

        if (cur_prediction < runtime_prediction) {
            runtime_prediction = cur_prediction;

            cpu_prop = cpu_size;
            gpu_prop = gpu_size;
        }
    }

    std::map<ExecutorDeviceType, unsigned> proportion;
    
    proportion[ExecutorDeviceType::GPU] = gpu_prop;
    proportion[ExecutorDeviceType::CPU] = cpu_prop;
    

    return std::make_unique<policy::ProportionBasedExecutionPolicy>(std::move(proportion));
}
}
