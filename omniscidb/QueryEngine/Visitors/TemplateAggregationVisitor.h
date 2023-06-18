#pragma once

#include "QueryEngine/CostModel/Measurements.h"
#include "QueryEngine/RelAlgVisitor.h"

#include <unordered_set>
#include <vector>

struct TemplateSample {
  bool consider;
  costmodel::AnalyticalTemplate templ;
  size_t size;
};

class TemplateAggregationVisitor : public RelAlgVisitor<TemplateSample> {
 public:
  virtual TemplateSample visitAggregate(const hdk::ir::Aggregate* n) const {
    LOG(DEBUG1) << "BAGRORG: VISITED visitAggregate";
    return registerNode(n, costmodel::AnalyticalTemplate::GroupBy);
  }

  virtual TemplateSample visitFilter(const hdk::ir::Filter* n) const {
    LOG(DEBUG1) << "BAGRORG: VISITED visitFilter";
    return registerNode(n, costmodel::AnalyticalTemplate::Scan);
  }

  virtual TemplateSample visitJoin(const hdk::ir::Join* n) const {
    LOG(DEBUG1) << "BAGRORG: VISITED visitJoin";
    return registerNode(n, costmodel::AnalyticalTemplate::Join);
  }

  virtual TemplateSample visitProject(const hdk::ir::Project* n) const {
    LOG(DEBUG1) << "BAGRORG: VISITED visitProject";
    return rejectNode();
  }

  virtual TemplateSample visitScan(const hdk::ir::Scan* n) const {
    LOG(DEBUG1) << "BAGRORG: VISITED visitScan";
    return registerNode(n, costmodel::AnalyticalTemplate::Scan);
  }

  virtual TemplateSample visitSort(const hdk::ir::Sort* n) const {
    LOG(DEBUG1) << "BAGRORG: VISITED visitSort";
    return registerNode(n, costmodel::AnalyticalTemplate::Sort);
  }

  virtual TemplateSample visitLogicalValues(const hdk::ir::LogicalValues* n) const {
    LOG(DEBUG1) << "BAGRORG: VISITED visitLogicalValues";
    return rejectNode();
  }

  virtual TemplateSample visitLogicalUnion(const hdk::ir::LogicalUnion* n) const {
    LOG(DEBUG1) << "BAGRORG: VISITED visitLogicalUnion";
    return rejectNode();
  }

  std::vector<costmodel::AnalyticalTemplate> getTemplates() {
    std::vector<costmodel::AnalyticalTemplate> ret(collected_templates_);
    collected_templates_.clear();

    return ret;
  }

 protected:
  virtual TemplateSample aggregateResult(const TemplateSample&,
                                         const TemplateSample& next_result) const {
    if (next_result.consider && f_) {
      LOG(DEBUG1) << "BAGRORG: aggreg: " << next_result.consider << ' ' << toString(next_result.templ);
      if (next_result.templ == costmodel::AnalyticalTemplate::Scan && 
            std::find(collected_templates_.begin(), collected_templates_.end(), (costmodel::AnalyticalTemplate::GroupBy)) != collected_templates_.end() &&
            std::find(collected_templates_.begin(), collected_templates_.end(), (costmodel::AnalyticalTemplate::Sort)) != collected_templates_.end()) {} else {

              collected_templates_.push_back(next_result.templ);
            }
      
      if ((next_result.templ == costmodel::AnalyticalTemplate::GroupBy || next_result.templ == costmodel::AnalyticalTemplate::Sort) 
              && (std::find(collected_templates_.begin(), collected_templates_.end(), (costmodel::AnalyticalTemplate::Scan)) != collected_templates_.end())) {
                  collected_templates_.erase(std::find(collected_templates_.begin(), collected_templates_.end(), (costmodel::AnalyticalTemplate::Scan)));
              }
      f_ = false;
    }
    return next_result;
  }

  virtual TemplateSample defaultResult() const {
    return {false, costmodel::AnalyticalTemplate::Unknown};
  }

  TemplateSample registerNode(const hdk::ir::Node* n,
                              costmodel::AnalyticalTemplate templ) const {
    bool consider = visited_nodes_.count(n);
    if (!consider) {
      visited_nodes_.insert(n);
      consider = true;
    }
    f_ = true;
    return {consider, templ, collected_templates_.size()};
  }

  TemplateSample rejectNode() const {
    return {false, costmodel::AnalyticalTemplate::Unknown};
  }

  mutable std::vector<costmodel::AnalyticalTemplate> collected_templates_;
  mutable std::unordered_set<const hdk::ir::Node*> visited_nodes_;
  mutable bool f_ = false;
};
