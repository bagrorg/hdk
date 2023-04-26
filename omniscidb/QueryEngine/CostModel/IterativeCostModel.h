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

#pragma once

#include "CostModel.h"
#include "DataSources/EmptyDataSource.h"
#include "Shared/Config.h"

namespace costmodel {

class IterativeCostModel : public CostModel {
 public:
  IterativeCostModel(std::unique_ptr<DataSource> source) : CostModel(std::move(source)) {}

    virtual std::unique_ptr<policy::ExecutionPolicy> predict(QueryInfo queryInfo);
 private:
    static constexpr size_t optimizationIterations = 1024;
};

}  // namespace costmodel
