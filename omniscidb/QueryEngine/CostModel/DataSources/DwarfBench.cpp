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

#include "DwarfBench.h"

#include "Logger/Logger.h"

#include <bench.hpp>

#include <fstream>
#include <iostream>
#include <filesystem>

namespace costmodel {

struct DwarfBenchDataSource::PrivateImpl {
  DwarfBench::Dwarf convertToDwarf(AnalyticalTemplate templ);
  DwarfBench::DeviceType convertDeviceType(ExecutorDeviceType device);
  std::vector<Detail::Measurement> convertMeasurement(
      const std::vector<DwarfBench::Measurement>& measurements);

  DwarfBench::DwarfBench db;
};

DwarfBench::Dwarf DwarfBenchDataSource::PrivateImpl::convertToDwarf(
    AnalyticalTemplate templ) {
  switch (templ) {
    case AnalyticalTemplate::GroupBy:
      return DwarfBench::Dwarf::GroupBy;
    case AnalyticalTemplate::Scan:
      return DwarfBench::Dwarf::Scan;
    case AnalyticalTemplate::Join:
      return DwarfBench::Dwarf::Join;
    case AnalyticalTemplate::Reduce:
      throw UnsupportedAnalyticalTemplate(templ);
    case AnalyticalTemplate::Sort:
      return DwarfBench::Dwarf::Sort;
  }
}

DwarfBench::DeviceType DwarfBenchDataSource::PrivateImpl::convertDeviceType(
    ExecutorDeviceType device) {
  switch (device) {
    case ExecutorDeviceType::CPU:
      return DwarfBench::DeviceType::CPU;
    case ExecutorDeviceType::GPU:
      return DwarfBench::DeviceType::GPU;
  }
}

std::vector<Detail::Measurement> DwarfBenchDataSource::PrivateImpl::convertMeasurement(
    const std::vector<DwarfBench::Measurement>& measurements) {
  std::vector<Detail::Measurement> ms;
  std::transform(measurements.begin(),
                 measurements.end(),
                 std::back_inserter(ms),
                 [](DwarfBench::Measurement m) {
                   return Detail::Measurement{.bytes = m.dataSize,
                                              .milliseconds = m.microseconds / 1000};
                 });
  return ms;
}

DwarfBenchDataSource::DwarfBenchDataSource()
    : DataSource(DataSourceConfig{"DwarfBench",
                                  {ExecutorDeviceType::CPU, ExecutorDeviceType::GPU},
                                  {AnalyticalTemplate::GroupBy,
                                   AnalyticalTemplate::Join,
                                   AnalyticalTemplate::Scan,
                                   AnalyticalTemplate::Sort}})
    , pimpl_(new PrivateImpl()) {}

DwarfBenchDataSource::~DwarfBenchDataSource() = default;

Detail::DeviceMeasurements DwarfBenchDataSource::getMeasurements(
    const std::vector<ExecutorDeviceType>& devices,
    const std::vector<AnalyticalTemplate>& templates) {
  Detail::DeviceMeasurements dm;

  namespace fs = std::filesystem;

  if (fs::exists(fs::path(cachePath))) {
    dm = readCache();
    return dm;
  }

  for (AnalyticalTemplate templ : templates) {
    CHECK(isTemplateSupported(templ));
    for (ExecutorDeviceType device : devices) {
      CHECK(isDeviceSupported(device));

      dm[device][templ] = measureTemplateOnDevice(device, templ);
    }
  }

  saveCache(dm);

  return dm;
}

void DwarfBenchDataSource::saveCache(const Detail::DeviceMeasurements &dm) {
  std::ofstream out(cachePath);
  LOG(DEBUG1) << "BAGRORG: Saving to file " << out.good() << ' ' << out.fail() << ' ' << out.bad() << ' ' << cachePath;
  

  for (auto &[device, mss]: dm) {
    for (auto &[temp, ms]: mss) {
      for (const Detail::Measurement &m: ms) {
        out << (device == ExecutorDeviceType::CPU ? "CPU" : "GPU") << "|" << toString(temp) << "|" << m.bytes << "|" << m.milliseconds << std::endl;
      }
    }
  }
}

Detail::DeviceMeasurements DwarfBenchDataSource::readCache() {
  std::ifstream in(cachePath);
  std::string line;
  Detail::DeviceMeasurements dm;

  while (std::getline(in, line)) {
    std::vector<std::string> strs;
    boost::split(strs,line,boost::is_any_of("|"));

    ExecutorDeviceType dev;
    AnalyticalTemplate temp;
    Detail::Measurement m;

    if (strs[0] == "CPU") dev = ExecutorDeviceType::CPU;
    if (strs[0] == "GPU") dev = ExecutorDeviceType::GPU;

    if (strs[1] == "Join"   ) temp = AnalyticalTemplate::Join;
    if (strs[1] == "GroupBy") temp = AnalyticalTemplate::GroupBy;
    if (strs[1] == "Scan"   ) temp = AnalyticalTemplate::Scan;
    if (strs[1] == "Reduce" ) temp = AnalyticalTemplate::Reduce;
    if (strs[1] == "Sort"   ) temp = AnalyticalTemplate::Sort;

    m = {
      .bytes = std::stoull(strs[2]),
      .milliseconds = std::stoull(strs[3])
    };

    dm[dev][temp].push_back(m);
  }

  return dm;
}

std::vector<Detail::Measurement> DwarfBenchDataSource::measureTemplateOnDevice(
    ExecutorDeviceType device,
    AnalyticalTemplate templ) {
  std::vector<Detail::Measurement> ms;
  for (size_t input_size : dwarf_bench_input_sizes_) {
    DwarfBench::RunConfig rc = {
        pimpl_->convertDeviceType(device),
        input_size,
        dwarf_bench_iterations_,
        pimpl_->convertToDwarf(templ),
    };

    std::vector<Detail::Measurement> input_size_measurements =
        pimpl_->convertMeasurement(pimpl_->db.makeMeasurements(rc));

    ms.insert(ms.end(), input_size_measurements.begin(), input_size_measurements.end());
  }

  return ms;
}

}  // namespace costmodel
