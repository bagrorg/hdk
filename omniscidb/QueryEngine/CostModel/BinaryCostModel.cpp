#include "BinaryCostModel.h"
#include "Dispatchers/DefaultExecutionPolicy.h"

std::unique_ptr<policy::ExecutionPolicy> costmodel::BinaryCostModel::predict(
    QueryInfo queryInfo) {
        std::lock_guard<std::mutex> l(latch_);

    size_t cpuExtrapolation = dp_[ExecutorDeviceType::CPU][queryInfo.templ]->getExtrapolatedData(queryInfo.bytesSize);
    size_t gpuExtrapolation = dp_[ExecutorDeviceType::GPU][queryInfo.templ]->getExtrapolatedData(queryInfo.bytesSize);
    
    if (cpuExtrapolation > gpuExtrapolation) return std::make_unique<policy::FragmentIDAssignmentExecutionPolicy>(ExecutorDeviceType::GPU);
    
    return std::make_unique<policy::FragmentIDAssignmentExecutionPolicy>(ExecutorDeviceType::CPU);
}
