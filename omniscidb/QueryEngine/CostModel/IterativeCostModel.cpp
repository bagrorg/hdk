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
    QueryInfo queryInfo) {
        std::lock_guard<std::mutex> l(latch_);

    unsigned cpuProp, gpuProp;
    size_t optStep = std::ceil(static_cast<float>(queryInfo.bytesSize) / optimizationIterations);
    size_t runtimePrediction = std::numeric_limits<size_t>::max();

    for (size_t curSize = 0; curSize < queryInfo.bytesSize; curSize += optStep) {
        size_t cpuSize = curSize;
        size_t gpuSize = queryInfo.bytesSize - curSize;

        size_t cpuPrediction = dp_[ExecutorDeviceType::CPU][queryInfo.templ]->getExtrapolatedData(cpuSize);
        size_t gpuPrediction = dp_[ExecutorDeviceType::GPU][queryInfo.templ]->getExtrapolatedData(gpuSize);
        size_t curPrediction = std::max(gpuPrediction, cpuPrediction);

        if (curPrediction < runtimePrediction) {
            runtimePrediction = curPrediction;

            cpuProp = static_cast<float>(cpuSize) / queryInfo.bytesSize * 10;
            gpuProp = 10 - cpuProp;
        }
    }

    std::map<ExecutorDeviceType, unsigned> proportion;
    
    proportion[ExecutorDeviceType::GPU] = gpuProp;
    proportion[ExecutorDeviceType::CPU] = cpuProp;
    

    return std::make_unique<policy::ProportionBasedExecutionPolicy>(std::move(proportion));
}
}
