#include "TitanTaskGraph.h"
#include <algorithm>
#include <exception>
#include <future>
#include <set>
#include <stdexcept>
#include <utility>

namespace Titan {
int TaskGraph::AddTask(TaskSpec task) {
    if (task.Name.empty() || !task.Execute) throw std::invalid_argument("task requires name and callback");
    for (const auto& t:Tasks_) if (t.Name==task.Name) throw std::invalid_argument("duplicate task name");
    Tasks_.push_back(std::move(task));
    return static_cast<int>(Tasks_.size()-1);
}
void TaskGraph::AddDependency(int before,int after) { Explicit_.push_back({before,after}); }
namespace {
bool Intersects(const std::vector<std::string>& a,const std::vector<std::string>& b) {
    for (const auto& s:a) if (std::find(b.begin(),b.end(),s)!=b.end()) return true;
    return false;
}
bool Conflict(const TaskSpec& a,const TaskSpec& b) {
    return Intersects(a.Writes,b.Writes) || Intersects(a.Writes,b.Reads) || Intersects(b.Writes,a.Reads);
}
}
TaskPlan TaskGraph::Compile() const {
    const int n=static_cast<int>(Tasks_.size());
    std::vector<std::set<int>> edges(static_cast<std::size_t>(n));
    std::vector<int> indegree(static_cast<std::size_t>(n),0);
    auto edge=[&](int a,int b) {
        if (a<0 || a>=n || b<0 || b>=n || a==b) throw std::invalid_argument("invalid task dependency");
        if (edges[static_cast<std::size_t>(a)].insert(b).second) ++indegree[static_cast<std::size_t>(b)];
    };
    // Insertion order defines semantics of conflicting accesses; independent tasks share a wave.
    for (int i=0;i<n;++i)
        for (int j=i+1;j<n;++j)
            if (Conflict(Tasks_[static_cast<std::size_t>(i)],Tasks_[static_cast<std::size_t>(j)])) edge(i,j);
    for (auto d:Explicit_) edge(d.first,d.second);
    TaskPlan plan;
    std::set<int> ready;
    for (int i=0;i<n;++i) if (indegree[static_cast<std::size_t>(i)]==0) ready.insert(i);
    std::size_t visited=0;
    while (!ready.empty()) {
        std::vector<int> wave(ready.begin(),ready.end());
        ready.clear();
        plan.Waves.push_back(wave);
        visited+=wave.size();
        for (int t:wave)
            for (int next:edges[static_cast<std::size_t>(t)])
                if (--indegree[static_cast<std::size_t>(next)]==0) ready.insert(next);
    }
    if (visited != Tasks_.size()) throw std::logic_error("task graph contains cycle");
    for (const auto& set:edges) plan.Dependencies+=set.size();
    return plan;
}
void TaskGraph::Run(const TaskPlan& plan,unsigned int concurrency) const {
    if (concurrency==0) throw std::invalid_argument("concurrency must be > 0");
    // Ensure a hand-constructed/stale plan cannot run arbitrary or missing tasks.
    std::vector<bool> seen(Tasks_.size(),false);
    for (const auto& wave:plan.Waves) for (int task:wave) {
        if (task<0 || static_cast<std::size_t>(task)>=Tasks_.size() || seen[static_cast<std::size_t>(task)])
            throw std::invalid_argument("invalid task plan");
        seen[static_cast<std::size_t>(task)]=true;
    }
    for (bool item:seen) if (!item) throw std::invalid_argument("incomplete task plan");
    for (const auto& wave:plan.Waves) {
        if (concurrency==1) {
            for (int id:wave) Tasks_[static_cast<std::size_t>(id)].Execute();
            continue;
        }
        // Each chunk is bounded; futures are joined before dependent wave can run.
        for (std::size_t start=0;start<wave.size();start+=concurrency) {
            std::vector<std::future<void>> futures;
            const std::size_t end=std::min(start+concurrency,wave.size());
            for (std::size_t i=start;i<end;++i) {
                const int id=wave[i];
                futures.emplace_back(std::async(std::launch::async,[this,id]() {
                    Tasks_[static_cast<std::size_t>(id)].Execute();
                }));
            }
            std::exception_ptr firstError;
            for (auto& f:futures) {
                try { f.get(); }
                catch (...) { if (!firstError) firstError=std::current_exception(); }
            }
            if (firstError) std::rethrow_exception(firstError);
        }
    }
}
}
