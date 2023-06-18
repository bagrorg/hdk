/*
    Copyright (c) 2023 Intel Corporation
    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at
        http://www.apache.org/licenses/LICENSE-2.0
    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include "IterativeCostModel.h"
#include "Dispatchers/ProportionBasedExecutionPolicy.h"

#ifdef HAVE_DWARF_BENCH
#include "DataSources/DwarfBench.h"
#endif

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace costmodel {

#ifdef HAVE_DWARF_BENCH
IterativeCostModel::IterativeCostModel()
    : CostModel({std::make_unique<DwarfBenchDataSource>()}) {
      namespace fs = std::filesystem;

      std::string interData = "/home/bagrorg/.cache/dwarfs/inter";

      if (fs::exists(fs::path(interData))) {
        LOG(DEBUG1) << "BAGRORG: READING INTER DATA";
        std::ifstream in(interData);
        std::string line;

        size_t i = 0;
        LOG(DEBUG1) << "BAGRORG: i=" << i;
        size_t sum = 0;
        while (std::getline(in, line)) {
          size_t var = std::stoull(line);
          sum += var;
          preds[{ i, 10 - i }] = var;
          i++;
        }

        for (auto &[p, v]: preds) {
          preds[p] /= sum;
          LOG(DEBUG1) << "BAGRORG: PRED FOR " << p.first << ' ' << p.second << ' ' << preds[p];
        }
        
        LOG(DEBUG1) << "BAGRORG: i=" << i;

        return;
      }


      preds[{ 0, 10 }] = 263.967782636232;
      preds[{ 1, 9 }] = 220.94561178472978;
      preds[{ 2, 8 }] = 196.97923599919187;
      preds[{ 3, 7 }] = 176.35550378244133;
      preds[{ 4, 6 }] = 154.21859523918056;
      preds[{ 5, 5 }] = 133.697250221349;
      preds[{ 6, 4 }] = 111.85243763501131;
      preds[{ 7, 3 }] = 90.02210520490816;
      preds[{ 8, 2 }] = 71.10991357248041;
      preds[{ 9, 1 }] = 68.47539129136484;
      preds[{ 10, 0 }] = 65.25445588027375;


    }
#else
IterativeCostModel::IterativeCostModel()
    : CostModel({std::make_unique<EmptyDataSource>()}) {
      namespace fs = std::filesystem;

      std::string interData = "/home/bagrorg/.cache/dwarfs/inter";

      if (fs::exists(fs::path(interData))) {
        LOG(DEBUG1) << "BAGRORG: READING INTER DATA";
        std::ifstream in(interData);
        std::string line;

        size_t i = 0;
        LOG(DEBUG1) << "BAGRORG: i=" << i;
        size_t sum = 0;
        while (std::getline(in, line)) {
          size_t var = std::stoull(line);
          sum += var;
          preds[{ i, 10 - i }] = var;
          i++;
        }

        for (auto &[p, v]: preds) {
          preds[p] /= sum;
          LOG(DEBUG1) << "BAGRORG: PRED FOR " << p.first << ' ' << p.second << ' ' << preds[p];
        }
        
        LOG(DEBUG1) << "BAGRORG: i=" << i;

        LOG(DEBUG1) << "BAGRORG: i=" << i;

        return;
      }


      preds[{ 0, 10 }] = 263.967782636232;
      preds[{ 1, 9 }] = 220.94561178472978;
      preds[{ 2, 8 }] = 196.97923599919187;
      preds[{ 3, 7 }] = 176.35550378244133;
      preds[{ 4, 6 }] = 154.21859523918056;
      preds[{ 5, 5 }] = 133.697250221349;
      preds[{ 6, 4 }] = 111.85243763501131;
      preds[{ 7, 3 }] = 90.02210520490816;
      preds[{ 8, 2 }] = 71.10991357248041;
      preds[{ 9, 1 }] = 68.47539129136484;
      preds[{ 10, 0 }] = 65.25445588027375;

    }
#endif

std::unique_ptr<policy::ExecutionPolicy> IterativeCostModel::predict(
    QueryInfo query_info) const {
  std::shared_lock<std::shared_mutex> l(latch_);

  unsigned cpu_prop = 1, gpu_prop = 0;
  size_t opt_step =
      std::ceil(static_cast<float>(query_info.bytes_size) / optimization_iterations_);
  size_t runtime_prediction = std::numeric_limits<size_t>::max();

  std::vector<DeviceExtrapolations> devices_extrapolations = getExtrapolations(
      {ExecutorDeviceType::CPU, ExecutorDeviceType::GPU}, query_info.templs);

  //todo <=?
  std::vector<double> predictions;
  std::vector<size_t> cpu_props;
  size_t summ = 0;
  for (size_t cur_size = 0; cur_size <= query_info.bytes_size; cur_size += opt_step) {
    size_t cpu_size = cur_size;
    size_t gpu_size = query_info.bytes_size - cur_size;

    size_t cpu_prediction = 0;
    size_t gpu_prediction = 0;

    for (DeviceExtrapolations dev_extrapolations : devices_extrapolations) {
      for (auto extrapolation : dev_extrapolations.extrapolations) {
        if (dev_extrapolations.device == ExecutorDeviceType::CPU) {
          cpu_prediction += extrapolation->getExtrapolatedData(cpu_size);
        } else if (dev_extrapolations.device == ExecutorDeviceType::GPU) {
          gpu_prediction += extrapolation->getExtrapolatedData(gpu_size);
        }
      }
    }

    size_t cur_prediction = std::max(gpu_prediction, cpu_prediction);
    predictions.push_back(cur_prediction);
    summ += cur_prediction;

    cpu_props.push_back(cpu_size);

    // exp
    // size_t cmpcpu = 10 * ((float) cpu_size / query_info.bytes_size);
    // size_t cmpgpu = 10 - cmpcpu;
    // assert(cmpcpu + cmpgpu == 10);
    // assert(cmpcpu <= 10);
    // assert(cmpgpu <= 10);
    // auto pred_ideal = preds[std::pair<size_t, size_t>{cmpcpu, cmpgpu}];


    // cur_prediction = cur_prediction * (1 - query_info.step) + pred_ideal * query_info.step;
    
    // LOG(DEBUG1) << "BAGRORG ERROR: " << cur_prediction << ' ' << pred_ideal << ' ' << std::abs((double) cur_prediction - pred_ideal) / pred_ideal;

    // if (cur_prediction <= runtime_prediction) {
    //   runtime_prediction = cur_prediction;

    //   cpu_prop = cpu_size;
    //   gpu_prop = gpu_size;
    // }
  }

  double best = 2.0;
  for (size_t i = 0; i < predictions.size(); i++) {
    size_t cmpcpu = 10 * ((float) cpu_props[i] / query_info.bytes_size);
    size_t cmpgpu = 10 - cmpcpu;
    assert(cmpcpu + cmpgpu == 10);
    assert(cmpcpu <= 10);
    assert(cmpgpu <= 10);
    double pred_ideal = preds[std::pair<size_t, size_t>{cmpcpu, cmpgpu}];

    double prompred = (1 - query_info.step) * (predictions[i] / summ) + pred_ideal * query_info.step;

    LOG(DEBUG1) << "BAGRORG PRED: " << prompred << ' ' << predictions[i] / summ << ' ' << pred_ideal;

    if (prompred <= best) {
      best = prompred;
      cpu_prop = cmpcpu;
      gpu_prop = cmpgpu;
    }
  }

  std::map<ExecutorDeviceType, unsigned> proportion;

  proportion[ExecutorDeviceType::GPU] = gpu_prop;
  proportion[ExecutorDeviceType::CPU] = cpu_prop;

  return std::make_unique<policy::ProportionBasedExecutionPolicy>(std::move(proportion));
}
}  // namespace costmodel
