#pragma once

// TITAN-owned, engine-independent C++11 spatial streaming decision algorithm.
// Does not load or unload assets; the UE adapter emits events to an application-owned loader.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace TitanSpatial
{
    struct Tile
    {
        int X;
        int Y;
        Tile() : X(0), Y(0) {}
        Tile(int InX, int InY) : X(InX), Y(InY) {}
    };
    inline bool operator==(const Tile& A, const Tile& B) { return A.X == B.X && A.Y == B.Y; }
    inline bool operator<(const Tile& A, const Tile& B)
    {
        return A.X != B.X ? A.X < B.X : A.Y < B.Y;
    }

    struct Config
    {
        double TileWorldSize;
        int LoadRadius;
        int UnloadRadius;
        std::size_t MaxActive;
        std::size_t MaxLoadsPerUpdate;
        std::size_t MaxUnloadsPerUpdate;
        Config() : TileWorldSize(2000.0), LoadRadius(2), UnloadRadius(3),
            MaxActive(64), MaxLoadsPerUpdate(4), MaxUnloadsPerUpdate(8) {}
    };

    struct PlanResult
    {
        std::vector<Tile> ToLoad;
        std::vector<Tile> ToUnload;
        Tile Center;
    };

    inline long long DistanceSquared(const Tile& A, const Tile& B)
    {
        const long long Dx = static_cast<long long>(A.X) - B.X;
        const long long Dy = static_cast<long long>(A.Y) - B.Y;
        return Dx * Dx + Dy * Dy;
    }

    inline bool Contains(const std::vector<Tile>& Values, const Tile& Value)
    {
        return std::find(Values.begin(), Values.end(), Value) != Values.end();
    }

    inline PlanResult Plan(const Config& Settings, double WorldX, double WorldY,
                           const std::vector<Tile>& CurrentTiles)
    {
        PlanResult Output;
        if (!(Settings.TileWorldSize > 0.0) || !std::isfinite(Settings.TileWorldSize) ||
            !std::isfinite(WorldX) || !std::isfinite(WorldY) ||
            Settings.LoadRadius < 0 || Settings.UnloadRadius < Settings.LoadRadius)
            return Output;

        const double CellX = std::floor(WorldX / Settings.TileWorldSize);
        const double CellY = std::floor(WorldY / Settings.TileWorldSize);
        // Unreal worlds using ~32-bit grid coordinates must stay inside a safe range.
        if (std::abs(CellX) > 1000000.0 || std::abs(CellY) > 1000000.0)
            return Output;
        Output.Center = Tile(static_cast<int>(CellX), static_cast<int>(CellY));

        // Normalize duplicate entries (e.g. after save restoration) to prevent double events.
        std::vector<Tile> Current = CurrentTiles;
        std::sort(Current.begin(), Current.end());
        Current.erase(std::unique(Current.begin(), Current.end()), Current.end());
        const long long UnloadThreshold = static_cast<long long>(Settings.UnloadRadius) * Settings.UnloadRadius;
        std::vector<Tile> RemovalCandidates;
        for (std::size_t I = 0; I < Current.size(); ++I)
        {
            if (DistanceSquared(Current[I], Output.Center) > UnloadThreshold)
                RemovalCandidates.push_back(Current[I]);
        }
        std::sort(RemovalCandidates.begin(), RemovalCandidates.end(),
            [&Output](const Tile& A, const Tile& B)
            {
                const long long DA = DistanceSquared(A, Output.Center);
                const long long DB = DistanceSquared(B, Output.Center);
                return DA != DB ? DA > DB : A < B;
            });

        // MaxActive is enforced even when load/unload budgets are small:
        // if over capacity, skip new loads until enough removals have completed.
        if (Current.size() > Settings.MaxActive)
        {
            std::vector<Tile> AllFarthest = Current;
            std::sort(AllFarthest.begin(), AllFarthest.end(),
                [&Output](const Tile& A, const Tile& B)
                {
                    const long long DA = DistanceSquared(A, Output.Center);
                    const long long DB = DistanceSquared(B, Output.Center);
                    return DA != DB ? DA > DB : A < B;
                });
            for (std::size_t I = 0; I < AllFarthest.size() &&
                  RemovalCandidates.size() < Current.size() - Settings.MaxActive; ++I)
            {
                if (!Contains(RemovalCandidates, AllFarthest[I]))
                    RemovalCandidates.push_back(AllFarthest[I]);
            }
        }
        for (std::size_t I = 0; I < RemovalCandidates.size() &&
             Output.ToUnload.size() < Settings.MaxUnloadsPerUpdate; ++I)
            Output.ToUnload.push_back(RemovalCandidates[I]);

        std::vector<Tile> Candidates;
        const long long LoadThreshold = static_cast<long long>(Settings.LoadRadius) * Settings.LoadRadius;
        for (int Dx = -Settings.LoadRadius; Dx <= Settings.LoadRadius; ++Dx)
            for (int Dy = -Settings.LoadRadius; Dy <= Settings.LoadRadius; ++Dy)
            {
                if (static_cast<long long>(Dx) * Dx + static_cast<long long>(Dy) * Dy > LoadThreshold)
                    continue;
                const Tile Candidate(Output.Center.X + Dx, Output.Center.Y + Dy);
                if (!Contains(Current, Candidate))
                    Candidates.push_back(Candidate);
            }
        std::sort(Candidates.begin(), Candidates.end(),
            [&Output](const Tile& A, const Tile& B)
            {
                const long long DA = DistanceSquared(A, Output.Center);
                const long long DB = DistanceSquared(B, Output.Center);
                return DA != DB ? DA < DB : A < B;
            });

        const std::size_t Remaining = Current.size() >= Output.ToUnload.size()
            ? Current.size() - Output.ToUnload.size() : 0;
        const std::size_t FreeSlots = Settings.MaxActive > Remaining ? Settings.MaxActive - Remaining : 0;
        const std::size_t Limit = std::min(Settings.MaxLoadsPerUpdate, FreeSlots);
        for (std::size_t I = 0; I < Candidates.size() && I < Limit; ++I)
            Output.ToLoad.push_back(Candidates[I]);
        return Output;
    }
}
