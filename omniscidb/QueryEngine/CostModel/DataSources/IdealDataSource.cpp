#include "IdealDataSource.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <sstream>
#include <boost/algorithm/string/trim.hpp>

namespace costmodel {
IdealDataSource::IdealDataSource() : DataSource(DataSourceConfig{
          .data_source_name = "IdealDataSource",
          .supported_devices = {ExecutorDeviceType::CPU, ExecutorDeviceType::GPU},
          .supported_templates = {AnalyticalTemplate::GroupBy,
                                  AnalyticalTemplate::Join,
                                  AnalyticalTemplate::Scan,
                                  AnalyticalTemplate::Sort}}) {
  std::ifstream in(path);
  std::vector<std::string> row;
  std::string temp;

  size_t templ_=0, dev_=1, size_=2, time_=3;

  while (in >> temp) {
    row.clear();
    boost::split(row, temp, boost::is_any_of(","));

    std::string templ = row[templ_];
    std::string device = row[dev_];
    std::string size = row[size_];
    std::string time = row[time_];

    boost::algorithm::trim(templ);
    boost::algorithm::trim(device);
    boost::algorithm::trim(size);
    boost::algorithm::trim(time);

    AnalyticalTemplate templReal;
    ExecutorDeviceType deviceReal;
    size_t timeReal = std::stoull(time);
    size_t sizeReal = std::stoull(size);

    if (device == "CPU") {
        deviceReal = ExecutorDeviceType::CPU;
    } else if (device == "GPU") {
        deviceReal = ExecutorDeviceType::GPU;
    } else {
        throw std::runtime_error("unknown device: " + device);
    }

    if (templ == "GroupBy") {
        templReal = AnalyticalTemplate::GroupBy;
    } else if (templ == "Sort") {
        templReal = AnalyticalTemplate::Sort;
    } else if (templ == "Join") {
        templReal = AnalyticalTemplate::Join;
    } else if (templ == "Scan") {
        templReal = AnalyticalTemplate::Scan;
    } else {
        throw std::runtime_error("unknown template: " + templ);
    }

    measurements_[deviceReal][templReal].push_back({sizeReal, timeReal});
  }
}

Detail::DeviceMeasurements IdealDataSource::getMeasurements(
    const std::vector<ExecutorDeviceType>& devices,
    const std::vector<AnalyticalTemplate>& templates) {
  Detail::DeviceMeasurements needed;
  for (auto d: devices) {
    for (auto t: templates) {
        needed[d][t] = measurements_[d][t];
    }
  }

  return measurements_;
}
}  // namespace costmodel
