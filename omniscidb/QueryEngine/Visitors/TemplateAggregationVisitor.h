#pragma once

#include "QueryEngine/CostModel/Measurements.h"
#include "QueryEngine/RelAlgVisitor.h"

#include <unordered_set>
#include <vector>

struct TemplateSample {
  bool consider;
  costmodel::AnalyticalTemplate templ;
};

class TemplateAggregationVisitor : public RelAlgVisitor<TemplateSample> {
 public:
  virtual TemplateSample visitAggregate(const hdk::ir::Aggregate* n) const {
    bool consider = true;
    if (visited_nodes_.find(n) != visited_nodes_.end())
      consider = false;
    else
      visited_nodes_.insert(n);
    return {consider, costmodel::AnalyticalTemplate::GroupBy};
  }

  virtual TemplateSample visitFilter(const hdk::ir::Filter* n) const {
    bool consider = true;
    if (visited_nodes_.find(n) != visited_nodes_.end())
      consider = false;
    else
      visited_nodes_.insert(n);
    return {consider, costmodel::AnalyticalTemplate::Scan};
  }

  virtual TemplateSample visitJoin(const hdk::ir::Join* n) const {
    bool consider = true;
    if (visited_nodes_.find(n) != visited_nodes_.end())
      consider = false;
    else
      visited_nodes_.insert(n);
    return {consider, costmodel::AnalyticalTemplate::Join};
  }

  virtual TemplateSample visitProject(const hdk::ir::Project* n) const {
    return {false, costmodel::AnalyticalTemplate::Unknown};
  }

  virtual TemplateSample visitScan(const hdk::ir::Scan* n) const {
    bool consider = true;
    if (visited_nodes_.find(n) != visited_nodes_.end())
      consider = false;
    else
      visited_nodes_.insert(n);
    return {consider, costmodel::AnalyticalTemplate::Scan};
  }

  virtual TemplateSample visitSort(const hdk::ir::Sort* n) const {
    bool consider = true;
    if (visited_nodes_.find(n) != visited_nodes_.end())
      consider = false;
    else
      visited_nodes_.insert(n);
    return {consider, costmodel::AnalyticalTemplate::Sort};
  }

  virtual TemplateSample visitLogicalValues(const hdk::ir::LogicalValues* n) const {
    return {false, costmodel::AnalyticalTemplate::Unknown};
  }

  virtual TemplateSample visitLogicalUnion(const hdk::ir::LogicalUnion* n) const {
    return {false, costmodel::AnalyticalTemplate::Unknown};
  }

  std::vector<costmodel::AnalyticalTemplate> getTemplates() {
    std::vector<costmodel::AnalyticalTemplate> ret(collected_templates_);
    collected_templates_.clear();

    return ret;
  }

 protected:
  virtual TemplateSample aggregateResult(const TemplateSample&,
                                         const TemplateSample& next_result) const {
    if (next_result.consider)
      collected_templates_.push_back(next_result.templ);
    return next_result;
  }

  virtual TemplateSample defaultResult() const {
    return {false, costmodel::AnalyticalTemplate::Unknown};
  }

  mutable std::vector<costmodel::AnalyticalTemplate> collected_templates_;
  mutable std::unordered_set<const hdk::ir::Node*> visited_nodes_;
};