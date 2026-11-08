//
// Copyright RIME Developers
// Distributed under the BSD License
//
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <rime/build_config.h>

#include <gtest/gtest.h>
#include <rime/lever/compile_unit.h>

using namespace rime;

static CompileUnit MakeUnit(const string& id,
                            function<bool()> run,
                            uint64_t estimated_memory = 0) {
  return CompileUnit(id, estimated_memory, std::move(run));
}

TEST(RimeCompileUnitTest, EmptyGraph) {
  CompileGraph graph;
  CompileScheduler scheduler;
  auto result = scheduler.Run(graph);
  ASSERT_TRUE(result);
  EXPECT_EQ(0, result->success);
  EXPECT_EQ(0, result->failure);
}

TEST(RimeCompileUnitTest, RunsInDependencyOrder) {
  CompileGraph graph;
  std::mutex mutex;
  vector<string> order;
  auto record = [&](const string& id) {
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back(id);
    return true;
  };

  ASSERT_TRUE(
      graph.AddUnit(MakeUnit("first", [&] { return record("first"); })));
  ASSERT_TRUE(
      graph.AddUnit(MakeUnit("second", [&] { return record("second"); })));
  ASSERT_TRUE(
      graph.AddUnit(MakeUnit("third", [&] { return record("third"); })));
  ASSERT_TRUE(graph.AddDependency("first", "third"));

  CompileScheduler scheduler;
  auto result = scheduler.Run(graph);
  ASSERT_TRUE(result);
  EXPECT_EQ(3, result->success);
  EXPECT_EQ(0, result->failure);

  auto pos = [&](const string& id) {
    return std::find(order.begin(), order.end(), id) - order.begin();
  };
  ASSERT_EQ(3u, order.size());
  EXPECT_LT(pos("first"), pos("third"));
}

TEST(RimeCompileUnitTest, SerializesUnitsSharingAnOutput) {
  CompileGraph graph;
  std::atomic<int> active{0};
  std::atomic<bool> overlapped{false};
  auto work = [&]() {
    if (active.fetch_add(1) != 0) {
      overlapped = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    active.fetch_sub(1);
    return true;
  };

  CompileUnit first = MakeUnit("first", work);
  first.AddOutput("shared");
  CompileUnit second = MakeUnit("second", work);
  second.AddOutput("shared");
  ASSERT_TRUE(graph.AddUnit(std::move(first)));
  ASSERT_TRUE(graph.AddUnit(std::move(second)));

  CompileScheduler scheduler;
  auto result = scheduler.Run(graph);
  ASSERT_TRUE(result);
  EXPECT_EQ(2, result->success);
  EXPECT_EQ(0, result->failure);
  EXPECT_FALSE(overlapped);
}

TEST(RimeCompileUnitTest, DuplicateOutputsOnSameUnitDoNotCreateCycle) {
  CompileGraph graph;
  CompileUnit unit = MakeUnit("unit", [] { return true; });
  unit.AddOutput("shared");
  unit.AddOutput("shared");
  ASSERT_TRUE(graph.AddUnit(std::move(unit)));

  CompileScheduler scheduler;
  auto result = scheduler.Run(graph);
  ASSERT_TRUE(result);
  EXPECT_EQ(1, result->success);
  EXPECT_EQ(0, result->failure);
}

TEST(RimeCompileUnitTest, SerializedUnitsRunOnCallingThread) {
  CompileGraph graph;
  const auto caller = std::this_thread::get_id();
  std::atomic<int> on_caller{0};
  auto check = [&] {
    if (std::this_thread::get_id() == caller) {
      ++on_caller;
    }
    return true;
  };
  ASSERT_TRUE(graph.AddUnit(MakeUnit("first", check)));
  ASSERT_TRUE(graph.AddUnit(MakeUnit("second", check)));
  ASSERT_TRUE(graph.AddDependency("first", "second"));

  CompileScheduler scheduler;
  auto result = scheduler.Run(graph);
  ASSERT_TRUE(result);
  EXPECT_EQ(2, result->success);
  EXPECT_EQ(0, result->failure);
  EXPECT_EQ(2, on_caller);
}

TEST(RimeCompileUnitTest, FailureDoesNotBlockSuccessors) {
  CompileGraph graph;
  std::atomic<int> good_runs{0};
  ASSERT_TRUE(graph.AddUnit(MakeUnit("bad", [] { return false; })));
  ASSERT_TRUE(graph.AddUnit(MakeUnit("good", [&] { return ++good_runs > 0; })));
  ASSERT_TRUE(graph.AddDependency("bad", "good"));

  CompileScheduler scheduler;
  auto result = scheduler.Run(graph);
  ASSERT_TRUE(result);
  EXPECT_EQ(1, result->success);
  EXPECT_EQ(1, result->failure);
  EXPECT_EQ(1, good_runs);
}

TEST(RimeCompileUnitTest, RejectsInvalidUnits) {
  CompileGraph graph;
  EXPECT_FALSE(graph.AddUnit(CompileUnit()));
  ASSERT_TRUE(graph.AddUnit(MakeUnit("a", [] { return true; })));
  EXPECT_FALSE(graph.AddUnit(MakeUnit("a", [] { return true; })));
  ASSERT_TRUE(graph.AddUnit(MakeUnit("b", [] { return true; })));
  EXPECT_FALSE(graph.AddDependency("missing", "a"));
  EXPECT_FALSE(graph.AddDependency("a", "missing"));
  EXPECT_TRUE(graph.AddDependency("a", "b"));
  EXPECT_TRUE(graph.AddDependency("a", "a"));
}

TEST(RimeCompileUnitTest, DeclarationOrderWinsOverContradictingDependency) {
  CompileGraph graph;
  std::mutex mutex;
  vector<string> order;
  auto record = [&](const string& id) {
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back(id);
    return true;
  };
  ASSERT_TRUE(graph.AddUnit(MakeUnit("b", [&] { return record("b"); })));
  ASSERT_TRUE(graph.AddUnit(MakeUnit("a", [&] { return record("a"); })));
  // "a" was declared after "b"; this request is ignored instead of creating a
  // cycle with declaration order.
  EXPECT_TRUE(graph.AddDependency("a", "b"));

  CompileScheduler scheduler;
  auto result = scheduler.Run(graph);
  ASSERT_TRUE(result);
  EXPECT_EQ(2, result->success);
  EXPECT_EQ(0, result->failure);
}

TEST(RimeCompileUnitTest, OversizedUnitsAreSerialized) {
  CompileGraph graph;
  std::atomic<int> active{0};
  std::atomic<bool> overlapped{false};
  auto work = [&] {
    if (active.fetch_add(1))
      overlapped = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    --active;
    return true;
  };
  ASSERT_TRUE(graph.AddUnit(MakeUnit("first", work, UINT64_MAX)));
  ASSERT_TRUE(graph.AddUnit(MakeUnit("second", work, UINT64_MAX)));
  CompileScheduler scheduler;
  auto result = scheduler.Run(graph);
  ASSERT_TRUE(result);
  EXPECT_EQ(2, result->success);
  EXPECT_FALSE(overlapped);
}

TEST(RimeCompileUnitTest, ReportsFailedAndThrowingUnitNames) {
  CompileGraph graph;
  ASSERT_TRUE(graph.AddUnit(MakeUnit("failed", [] { return false; })));
  ASSERT_TRUE(graph.AddUnit(MakeUnit("throwing", []() -> bool {
    throw std::runtime_error("compile failed");
  })));
  ASSERT_TRUE(graph.AddUnit(MakeUnit("good", [] { return true; })));
  CompileScheduler scheduler;
  auto result = scheduler.Run(graph);
  ASSERT_TRUE(result);
  EXPECT_EQ(1, result->success);
  EXPECT_EQ(2, result->failure);
  std::sort(result->failed_units.begin(), result->failed_units.end());
  EXPECT_EQ((vector<string>{"failed", "throwing"}), result->failed_units);
}

TEST(RimeCompileUnitTest, IndependentUnitsCanOverlap) {
#ifdef RIME_NO_THREADING
  GTEST_SKIP() << "Threading is disabled";
#else
  if (std::thread::hardware_concurrency() <= 1)
    GTEST_SKIP() << "Only one worker is available";
  CompileGraph graph;
  std::mutex mutex;
  std::condition_variable cv;
  int entered = 0;
  auto work = [&] {
    std::unique_lock<std::mutex> lock(mutex);
    if (++entered == 2)
      cv.notify_all();
    return cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return entered == 2; });
  };
  ASSERT_TRUE(graph.AddUnit(MakeUnit("first", work)));
  ASSERT_TRUE(graph.AddUnit(MakeUnit("second", work)));
  CompileScheduler scheduler;
  auto result = scheduler.Run(graph);
  ASSERT_TRUE(result);
  EXPECT_EQ(2, result->success);
  EXPECT_EQ(0, result->failure);
#endif
}
