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

class BinaryCostModel : public CostModel {
 public:
  BinaryCostModel() : CostModel(std::make_unique<EmptyDataSource>()) {}

    virtual std::unique_ptr<policy::ExecutionPolicy> predict(QueryInfo queryInfo);
};

}  // namespace costmodel
