#pragma once
#include "../RelAlgVisitor.h"

class LogRelAlgVisitor : public RelAlgVisitor<std::string> {
 public:
  virtual std::string visitAggregate(const hdk::ir::Aggregate* n) const { return "Aggregate: " + addrToString(n); }

  virtual std::string visitFilter(const hdk::ir::Filter* n) const { return "Filter: " + addrToString(n); }

  virtual std::string visitJoin(const hdk::ir::Join* n) const { return "Join: " + addrToString(n); }

  virtual std::string visitProject(const hdk::ir::Project* n) const { return "Project: " + addrToString(n); }

  virtual std::string visitScan(const hdk::ir::Scan* n) const { return "Scan: " + addrToString(n); }

  virtual std::string visitSort(const hdk::ir::Sort* n) const { return "Sort: " + addrToString(n); }

  virtual std::string visitLogicalValues(const hdk::ir::LogicalValues* n) const {
    return "LogicalValues: " + addrToString(n);
  }

  virtual std::string visitLogicalUnion(const hdk::ir::LogicalUnion* n) const {
    
    return "LogicalUnion: " + addrToString(n);
  }

 protected:
  virtual std::string aggregateResult(const std::string& aggregate, const std::string& next_result) const {
    return aggregate + ", " + next_result;
  }

  virtual std::string defaultResult() const { return ""; }

  std::string addrToString(const hdk::ir::Node* n) const {
    std::stringstream stream;
    stream << std::hex << (size_t) n;
    return stream.str();
  }
};