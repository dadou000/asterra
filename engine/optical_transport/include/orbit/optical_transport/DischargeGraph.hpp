#pragma once

#include <cstdint>
#include <vector>

namespace orbit::optical_transport
{

enum class FlashKind
{
    IntraCloud,
    CloudToGround,
    CloudToCloud,
};

// One conducting segment of a discharge with its emission history. Emission is a list of current pulses so a
// pulse far shorter than a frame still contributes its integrated energy to the exposure window.
struct Pulse
{
    double startTime = 0.0;      // s
    double duration = 0.0;       // s
    double radiantEnergy = 0.0;  // J emitted optically over the pulse
};

struct Segment
{
    double from[3] = {0, 0, 0};
    double to[3] = {0, 0, 0};
    std::int32_t parent = -1;     // upstream segment (-1 for the origin)
    double formedAt = 0.0;
    double chargeTransferred = 0.0;  // C moved through this segment
    double conductivity = 0.0;       // 0 = faint streamer only, never a bright channel
    std::vector<Pulse> pulses;
};

// Charge reservoirs the discharge connects. A discharge moves charge, never invents it.
struct Reservoir
{
    double charge = 0.0;  // C
};

class DischargeGraph
{
public:
    explicit DischargeGraph(FlashKind kind) : kind_(kind) {}

    std::int32_t AddSegment(Segment segment);
    Segment& At(std::int32_t index) { return segments_[static_cast<std::size_t>(index)]; }
    const std::vector<Segment>& Segments() const { return segments_; }
    FlashKind Kind() const { return kind_; }

    // Move `charge` coulombs from one reservoir to another through the graph. Returns false and changes nothing
    // if the source lacks the charge. Total charge across both is conserved exactly.
    static bool Transfer(Reservoir& from, Reservoir& to, double charge);

    // Radiant energy (J) emitted by the whole graph inside [t0, t1], integrating partially covered pulses
    // proportionally: a 1 microsecond pulse inside a 16 ms frame contributes all of its energy.
    double EnergyInWindow(double t0, double t1) const;

    // Bounded clusters: merge segments into at most `maxClusters` emitters (by spatial cell) preserving the energy
    // in the window. Returns cluster centroids and energy; sum(energy) == EnergyInWindow(t0, t1).
    struct Cluster
    {
        double position[3] = {0, 0, 0};
        double energy = 0.0;
    };
    std::vector<Cluster> ClusterEmission(double t0, double t1, std::size_t maxClusters) const;

    // Segments that are reused (a dart leader re-brightening an existing branch) keep their index and add a pulse.
    void Reactivate(std::int32_t index, Pulse pulse);

private:
    FlashKind kind_;
    std::vector<Segment> segments_;
};

} // namespace orbit::optical_transport
