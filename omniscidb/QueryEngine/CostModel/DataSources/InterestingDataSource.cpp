#include "InterestingDataSource.h"
#include <random>
#include <algorithm>

namespace costmodel {

InterestingDataSource::InterestingDataSource(size_t abs)
    : DataSource(DataSourceConfig{"InterestingDataSource",
                                  {ExecutorDeviceType::CPU, ExecutorDeviceType::GPU},
                                  {AnalyticalTemplate::GroupBy,
                                   AnalyticalTemplate::Join,
                                   AnalyticalTemplate::Reduce,
                                   AnalyticalTemplate::Scan,
                                   AnalyticalTemplate::Sort}}), abs(abs) {}

Detail::DeviceMeasurements InterestingDataSource::getMeasurements(
    const std::vector<ExecutorDeviceType>& devices,
    const std::vector<AnalyticalTemplate>& templates) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<long long> dist(-abs, abs);
  auto ms = ds.getMeasurements(devices, templates);
  Detail::DeviceMeasurements ms_res;

  for (auto [d, ts] : ms) {
    for (auto [t, rs] : ts) {
        ms_res[d][t] = {};
        for (auto r: rs) {
            auto r_ = r;
            long long noise = dist(gen);

            if (noise > 0) {
                noise = std::max((long long) abs / 2, noise);
            } else {
                noise = std::min(- (long long) abs / 2, noise);
            }
            if (noise < 0 && std::abs(noise) > r_.milliseconds) {
                noise = -r_.milliseconds;
            }

            r_.milliseconds += noise;
            ms_res[d][t].push_back(r_);
        }
    }
  }
return ms_res;
}

}  // namespace costmodel
