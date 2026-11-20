#pragma once
// Deterministic dependency scheduler, data-hazard ordering and bounded concurrent execution.
// Functions must be pure/native-thread safe if maxConcurrency > 1; never touch UObjects off game thread.
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Titan {
struct TaskSpec {
    std::string Name;
    std::vector<std::string> Reads;
    std::vector<std::string> Writes;
    std::function<void()> Execute;
};
struct TaskPlan {
    std::vector<std::vector<int>> Waves;
    std::size_t Dependencies = 0;
};
class TaskGraph {
public:
    int AddTask(TaskSpec task);
    void AddDependency(int before, int after);
    TaskPlan Compile() const;
    void Run(const TaskPlan& plan, unsigned int maxConcurrency = 1) const;
    std::size_t TaskCount() const { return Tasks_.size(); }
private:
    std::vector<TaskSpec> Tasks_;
    std::vector<std::pair<int,int>> Explicit_;
};
}
