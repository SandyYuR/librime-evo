//
// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_COMPILE_UNIT_H_
#define RIME_COMPILE_UNIT_H_

#include <cstdint>

#include <rime_api.h>
#include <rime/common.h>

namespace rime {

// A unit of the compiling process. Each compiling module declares the files it
// writes and the units that must be compiled before it.
class RIME_DLL CompileUnit {
 public:
  CompileUnit() = default;
  CompileUnit(string id, uint64_t estimated_memory, function<bool()> run);
  CompileUnit(CompileUnit&&) = default;
  CompileUnit& operator=(CompileUnit&&) = default;
  CompileUnit(const CompileUnit&) = delete;
  CompileUnit& operator=(const CompileUnit&) = delete;

  const string& id() const { return id_; }
  // indices of the units that must be compiled before this one
  const vector<size_t>& prerequisites() const { return prerequisites_; }
  // output file names this unit may write; files shared
  // by two units must not be written concurrently
  const vector<string>& outputs() const { return outputs_; }
  // estimated peak memory use in bytes, used for scheduling
  uint64_t estimated_memory() const { return estimated_memory_; }
  const function<bool()>& run() const { return run_; }

  // Records an output file. Units sharing an output are
  // serialized in declaration order.
  void AddOutput(string output);

 private:
  friend class CompileGraph;

  string id_;
  vector<size_t> prerequisites_;
  vector<string> outputs_;
  uint64_t estimated_memory_ = 0;
  function<bool()> run_;
};

// A graph of compile units. Dependencies are expressed as ordering constraints
// only: a failed unit does not prevent its successors from running.
class RIME_DLL CompileGraph {
 public:
  CompileGraph() = default;
  CompileGraph(const CompileGraph&) = delete;
  CompileGraph& operator=(const CompileGraph&) = delete;

  // Returns false if the unit is invalid or its id is already used.
  bool AddUnit(CompileUnit unit);
  // Declares that |prerequisite| is compiled before |dependent| when possible.
  // Declaration order wins: if |dependent| was declared earlier, the request
  // is ignored because output ordering already fixes declaration order.
  // Returns false only when either unit is unknown.
  bool AddDependency(const string& prerequisite, const string& dependent);

  size_t size() const { return units_.size(); }
  const CompileUnit& unit(size_t index) const { return units_[index]; }

 private:
  vector<CompileUnit> units_;
  map<string, size_t> unit_index_;
};

// Executes the compile units of a graph: sequentially when threading is
// disabled, or with a bounded worker pool otherwise.
class RIME_DLL CompileScheduler {
 public:
  struct Result {
    int success = 0;
    int failure = 0;
    vector<string> failed_units;
  };
  // Returns nullopt if the graph contains a cycle; unit failures are counted.
  std::optional<Result> Run(const CompileGraph& graph);

 private:
  bool RunUnit(const CompileUnit& unit);
};

}  // namespace rime

#endif  // RIME_COMPILE_UNIT_H_
