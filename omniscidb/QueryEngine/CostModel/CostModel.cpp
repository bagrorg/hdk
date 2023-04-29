/*
    Copyright (c) 2022 Intel Corporation
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

#include "CostModel.h"
#include "ExtrapolationModels/LinearExtrapolation.h"

#ifdef HAVE_ARMADILLO
#include "ExtrapolationModels/LinearRegression.h"
#endif

namespace costmodel {

CostModel::CostModel(CostModelConfig config)
    : config_(std::move(config)) {
  for (AnalyticalTemplate templ : templates_) {
    if (!config_.data_source->isTemplateSupported(templ))
      throw CostModelException("template " + templateToString(templ) +
                               " not supported in " + config_.data_source->getName() +
                               " data source");
  }

  for (ExecutorDeviceType device : devices_) {
    if (!config_.data_source->isDeviceSupported(device))
      throw CostModelException("device " + deviceToString(device) + " not supported in " +
                               config_.data_source->getName() + " data source");
  }
}

void CostModel::calibrate(const CaibrationConfig& conf) {
  std::unique_lock<std::shared_mutex> l(latch_);

  Detail::DeviceMeasurements dm;

  try {
    dm = config_.data_source->getMeasurements(conf.devices, templates_);
  } catch (const std::exception& e) {
    LOG(ERROR) << "Cost model calibration failure: " << e.what();
    return;
  }

  for (const auto& dm_entry : dm) {
    ExecutorDeviceType device = dm_entry.first;

    for (auto& template_measurement : dm_entry.second) {
      AnalyticalTemplate templ = template_measurement.first;
      dp_[device][templ] = extrapolation_provider_.provide(std::move(template_measurement.second));
    }
  }
}

size_t CostModel::getExtrapolatedData(ExecutorDeviceType device,
                                      AnalyticalTemplate templ,
                                      size_t bytes) const {
  auto device_measurements_it = dp_.find(device);
  if (device_measurements_it == dp_.end()) {
    throw CostModelException("there is no " + deviceToString(device) + " in measured data");
  }

  auto model_it = device_measurements_it->second.find(templ);
  if (model_it == device_measurements_it->second.end()) {
    throw CostModelException("there is no " + templateToString(templ) + " in measured data for " + deviceToString(device));
  }

  return model_it->second->getExtrapolatedData(bytes);
}

const std::vector<AnalyticalTemplate> CostModel::templates_ = {Scan,
                                                               Sort,
                                                               Join,
                                                               GroupBy};

}  // namespace costmodel
