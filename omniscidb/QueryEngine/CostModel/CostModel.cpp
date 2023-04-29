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
    if (!config_.dataSource->isTemplateSupported(templ))
      throw CostModelException("template " + templateToString(templ) +
                               " not supported in " + config_.dataSource->getName() +
                               " data source");
  }

  for (ExecutorDeviceType device : devices_) {
    if (!config_.dataSource->isDeviceSupported(device))
      throw CostModelException("device " + deviceToString(device) + " not supported in " +
                               config_.dataSource->getName() + " data source");
  }
}

void CostModel::calibrate(const CaibrationConfig& conf) {
  std::lock_guard<std::mutex> g{latch_};

  Detail::DeviceMeasurements dm;

  try {
    dm = config_.dataSource->getMeasurements(conf.devices, templates_);
  } catch (const std::exception& e) {
    LOG(ERROR) << "Cost model calibration failure: " << e.what();
    return;
  }

  for (const auto& dmEntry : dm) {
    ExecutorDeviceType device = dmEntry.first;

    for (auto& templateMeasurement : dmEntry.second) {
      AnalyticalTemplate templ = templateMeasurement.first;

#ifdef HAVE_ARMADILLO
      dp_[device][templ] =
          std::make_unique<LinearRegression>(std::move(templateMeasurement.second));
#else
      dp_[device][templ] =
          std::make_unique<LinearExtrapolation>(std::move(templateMeasurement.second));
#endif
    }
  }
}

const std::vector<AnalyticalTemplate> CostModel::templates_ = {Scan,
                                                               Sort,
                                                               Join,
                                                               GroupBy};

}  // namespace costmodel
