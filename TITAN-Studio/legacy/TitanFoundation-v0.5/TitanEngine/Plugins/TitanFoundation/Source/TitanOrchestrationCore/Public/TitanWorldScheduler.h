#pragma once
// Original TITAN C++14 engine-agnostic world streaming scheduler.
// Commands are REQUESTS. Memory is released only after an explicit I/O acknowledgment.
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Titan {
struct Cell {
    int X = 0;
    int Y = 0;
    Cell() = default;
    Cell(int x, int y) : X(x), Y(y) {}
    bool operator<(const Cell& other) const { return X != other.X ? X < other.X : Y < other.Y; }
    bool operator==(const Cell& other) const { return X == other.X && Y == other.Y; }
};
enum class TileState { Loading, Resident, Unloading };
enum class StreamOp { Load, Unload };
struct StreamRequest {
    StreamOp Operation;
    Cell Key;
    std::size_t EstimatedBytes;
};
struct StreamConfig {
    double CellWorldSize = 2000.0;
    int LoadRadius = 3;
    int UnloadRadius = 5;
    double PredictionSeconds = 0.8;
    std::size_t MaxTiles = 96;
    std::size_t MaxResidentBytes = 512ULL * 1024 * 1024;
    std::size_t EstimatedTileBytes = 8ULL * 1024 * 1024;
    std::size_t MaxInFlightLoads = 6;
    std::size_t MaxLoadsPerTick = 3;
    std::size_t MaxUnloadsPerTick = 5;
};
struct StreamStatistics {
    std::size_t Loading = 0;
    std::size_t Resident = 0;
    std::size_t Unloading = 0;
    std::size_t ReservedBytes = 0;
    std::size_t TotalRecords = 0;
};
class WorldScheduler {
public:
    explicit WorldScheduler(StreamConfig config = StreamConfig());
    void SetConfig(const StreamConfig& config);
    const StreamConfig& GetConfig() const { return Config_; }
    std::vector<StreamRequest> Tick(double worldX, double worldY, double velocityX = 0.0, double velocityY = 0.0);
    // Returns false for stale/duplicate/invalid completion. No implicit asset I/O is performed.
    bool CompleteLoad(Cell cell, bool succeeded, std::size_t actualBytes = 0);
    bool CompleteUnload(Cell cell, bool succeeded);
    StreamStatistics Stats() const;
    bool Contains(Cell cell) const { return Records_.find(cell) != Records_.end(); }
    TileState State(Cell cell) const;
    void Reset(); // Call only when all external I/O has stopped/cancelled.
private:
    struct Entry { TileState State; std::size_t Bytes; };
    StreamConfig Config_;
    std::map<Cell, Entry> Records_;
};
}
