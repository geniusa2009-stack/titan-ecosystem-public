#include "TitanWorldScheduler.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace Titan {
namespace {
long long DistanceSquared(Cell a, Cell b) {
    const long long x = static_cast<long long>(a.X) - b.X;
    const long long y = static_cast<long long>(a.Y) - b.Y;
    return x*x + y*y;
}
bool IsInside(Cell a, Cell b, int radius) {
    return DistanceSquared(a,b) <= static_cast<long long>(radius)*radius;
}
Cell ToCell(double x, double y, double size) {
    return Cell(static_cast<int>(std::floor(x/size)), static_cast<int>(std::floor(y/size)));
}
bool SafeCoordinate(double value, double size) {
    return std::isfinite(value) && std::abs(value/size) < 1000000.0;
}
}
WorldScheduler::WorldScheduler(StreamConfig config) { SetConfig(config); }
void WorldScheduler::SetConfig(const StreamConfig& c) {
    if (!(c.CellWorldSize > 0.0) || !std::isfinite(c.CellWorldSize) ||
        c.LoadRadius < 0 || c.LoadRadius > 64 || c.UnloadRadius < c.LoadRadius || c.UnloadRadius > 128 ||
        !std::isfinite(c.PredictionSeconds) || c.PredictionSeconds < 0.0 || c.PredictionSeconds > 10.0 ||
        c.MaxTiles == 0 || c.MaxResidentBytes == 0 || c.EstimatedTileBytes == 0 ||
        c.MaxInFlightLoads == 0)
        throw std::invalid_argument("TITAN WorldScheduler invalid configuration");
    Config_ = c;
}
StreamStatistics WorldScheduler::Stats() const {
    StreamStatistics s;
    for (const auto& item : Records_) {
        const Entry& e = item.second;
        if (e.State == TileState::Loading) ++s.Loading;
        if (e.State == TileState::Resident) ++s.Resident;
        if (e.State == TileState::Unloading) ++s.Unloading;
        if (e.Bytes > std::numeric_limits<std::size_t>::max() - s.ReservedBytes) {
            s.ReservedBytes = std::numeric_limits<std::size_t>::max();
        } else s.ReservedBytes += e.Bytes;
    }
    s.TotalRecords = Records_.size();
    return s;
}
TileState WorldScheduler::State(Cell cell) const {
    auto it = Records_.find(cell);
    if (it == Records_.end()) throw std::out_of_range("unknown cell");
    return it->second.State;
}
void WorldScheduler::Reset() { Records_.clear(); }
bool WorldScheduler::CompleteLoad(Cell key, bool success, std::size_t actualBytes) {
    auto it = Records_.find(key);
    if (it == Records_.end() || it->second.State != TileState::Loading) return false;
    if (!success) { Records_.erase(it); return true; }
    it->second.State = TileState::Resident;
    if (actualBytes != 0) it->second.Bytes = actualBytes;
    return true;
}
bool WorldScheduler::CompleteUnload(Cell key, bool success) {
    auto it = Records_.find(key);
    if (it == Records_.end() || it->second.State != TileState::Unloading) return false;
    if (success) Records_.erase(it);
    else it->second.State = TileState::Resident;
    return true;
}
std::vector<StreamRequest> WorldScheduler::Tick(double x, double y, double vx, double vy) {
    std::vector<StreamRequest> out;
    if (!SafeCoordinate(x, Config_.CellWorldSize) || !SafeCoordinate(y, Config_.CellWorldSize) ||
        !std::isfinite(vx) || !std::isfinite(vy)) return out;
    const Cell here = ToCell(x,y,Config_.CellWorldSize);
    const double predictedX = x + vx*Config_.PredictionSeconds;
    const double predictedY = y + vy*Config_.PredictionSeconds;
    const Cell predicted = SafeCoordinate(predictedX,Config_.CellWorldSize) &&
        SafeCoordinate(predictedY,Config_.CellWorldSize) ?
        ToCell(predictedX,predictedY,Config_.CellWorldSize) : here;
    auto st = Stats();
    // Release far-away resident tiles first; treat pending unloads as still consuming memory.
    std::vector<Cell> eviction;
    for (const auto& item : Records_) {
        if (item.second.State == TileState::Resident && !IsInside(item.first,here,Config_.UnloadRadius))
            eviction.push_back(item.first);
    }
    auto farthest = [here](Cell a, Cell b) {
        auto da=DistanceSquared(a,here), db=DistanceSquared(b,here);
        return da != db ? da > db : a < b;
    };
    std::sort(eviction.begin(),eviction.end(),farthest);
    // If over budget, also queue farthest residents within the hysteresis zone.
    if (st.ReservedBytes > Config_.MaxResidentBytes || st.TotalRecords > Config_.MaxTiles) {
        std::vector<Cell> extras;
        for (const auto& item : Records_)
            if (item.second.State == TileState::Resident &&
                std::find(eviction.begin(),eviction.end(),item.first) == eviction.end())
                extras.push_back(item.first);
        std::sort(extras.begin(),extras.end(),farthest);
        eviction.insert(eviction.end(),extras.begin(),extras.end());
    }
    const std::size_t unloadCount = std::min(eviction.size(), Config_.MaxUnloadsPerTick);
    for (std::size_t i=0;i<unloadCount;++i) {
        Records_.at(eviction[i]).State = TileState::Unloading;
        out.push_back({StreamOp::Unload, eviction[i], Records_.at(eviction[i]).Bytes});
    }
    // Can't reuse memory until the actual unload is acknowledged by caller.
    st = Stats();
    std::vector<Cell> potential;
    for (int dx = -Config_.LoadRadius; dx <= Config_.LoadRadius; ++dx)
        for (int dy = -Config_.LoadRadius; dy <= Config_.LoadRadius; ++dy) {
            Cell key(here.X + dx, here.Y + dy);
            if (IsInside(key,here,Config_.LoadRadius) && !Contains(key)) potential.push_back(key);
        }
    // Prefer proximity but bias requests toward player's velocity direction.
    std::sort(potential.begin(),potential.end(),[here,predicted](Cell a, Cell b) {
        const double scoreA = DistanceSquared(a,here) + 0.55*DistanceSquared(a,predicted);
        const double scoreB = DistanceSquared(b,here) + 0.55*DistanceSquared(b,predicted);
        return scoreA == scoreB ? a < b : scoreA < scoreB;
    });
    std::size_t loads=0;
    for (Cell cell : potential) {
        if (loads >= Config_.MaxLoadsPerTick || st.Loading >= Config_.MaxInFlightLoads ||
            st.TotalRecords >= Config_.MaxTiles || st.ReservedBytes > Config_.MaxResidentBytes ||
            Config_.EstimatedTileBytes > Config_.MaxResidentBytes - st.ReservedBytes) break;
        Records_.emplace(cell,Entry{TileState::Loading, Config_.EstimatedTileBytes});
        out.push_back({StreamOp::Load,cell,Config_.EstimatedTileBytes});
        ++loads; ++st.Loading; ++st.TotalRecords; st.ReservedBytes += Config_.EstimatedTileBytes;
    }
    return out;
}
}
