#include <orbit/path_geometry/CorridorEngineering.hpp>
#include <cmath>
#include <iostream>
#include <limits>

#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " #x " at " << __LINE__ << '\n'; return 1; } } while(false)

int main()
{
    using namespace orbit::path_geometry;
    const CorridorEngineeringRules rules{
        .designSpeedMetersPerSecond = 20.0,
        .maximumGrade = 0.08,
        .minimumRadiusMeters = 25.0,
        .maximumLateralAccelerationMetersPerSecondSquared = 2.0,
        .maximumBankRadians = 0.15,
        .maximumBankGradientRadiansPerMeter = 0.002
    };
    const std::vector<PathStation> flat{
        {.stationMeters=0,.position={0,0,0},.tangent={1,0,0}},
        {.stationMeters=10,.position={10,0,0},.tangent={1,0,0}},
        {.stationMeters=20,.position={20,0,0},.tangent={1,0,0}}
    };
    orbit::paths::PathProfile authored;
    authored.minimumRadiusMeters = 42.0;
    authored.maximumGrade = 0.035;
    const auto adapted = EngineeringRulesForProfile(authored, rules);
    CHECK(adapted.minimumRadiusMeters == 42.0);
    CHECK(adapted.maximumGrade == 0.035);
    CHECK(adapted.designSpeedMetersPerSecond == rules.designSpeedMetersPerSecond);
    const auto straight = AnalyzeCorridorEngineering(flat,rules);
    CHECK(straight.Valid());
    CHECK(straight.findings.empty());
    CHECK(straight.stations.size() == 3);
    CHECK(std::isinf(straight.stations[1].radiusMeters));
    CHECK(straight.stations[2].recommendedBankRadians == 0.0);
    const auto repeat = AnalyzeCorridorEngineering(flat,rules);
    CHECK(repeat.findings.size() == straight.findings.size());
    CHECK(repeat.stations[1].grade == straight.stations[1].grade);

    auto steep = flat;
    steep[1].position.y = 2;
    const auto grade = AnalyzeCorridorEngineering(steep,rules);
    CHECK(grade.Valid());
    bool sawGrade = false;
    for (const auto& f : grade.findings)
        if (f.issue == CorridorIssue::ExcessiveGrade) sawGrade = true;
    CHECK(sawGrade);

    auto curved = flat;
    curved[2].tangent = {0,0,1};
    // Vertical pitch is not horizontal turning; no phantom bank/cant.
    auto crest=flat;
    crest[2].tangent = {std::sqrt(0.75), 0.5, 0.0};
    const auto verticalCurve = AnalyzeCorridorEngineering(crest,rules);
    CHECK(verticalCurve.Valid());
    CHECK(verticalCurve.stations.back().curvaturePerMeter==0.0);
    CHECK(verticalCurve.stations.back().recommendedBankRadians==0.0);
    const auto bend = AnalyzeCorridorEngineering(curved,rules);
    bool tight = false, lateral = false;
    for (const auto& f : bend.findings)
    {
        tight |= f.issue == CorridorIssue::TightRadius;
        lateral |= f.issue == CorridorIssue::ExcessiveLateralAcceleration;
    }
    CHECK(tight && lateral);
    CHECK(std::abs(bend.stations.back().recommendedBankRadians) > 0.0);
    const auto cant = AnalyzeCorridorEngineering(flat,rules,{0,0.2,-0.2});
    bool bank = false, abrupt = false;
    for (const auto& f : cant.findings)
    {
        bank |= f.issue == CorridorIssue::ExcessiveBank;
        abrupt |= f.issue == CorridorIssue::AbruptBankTransition;
    }
    CHECK(bank && abrupt);
    const auto advised = SuggestCorridorBankProfile(bend, rules);
    CHECK(advised.size() == bend.stations.size());
    for (std::size_t i = 1; i < advised.size(); ++i)
    {
        const auto delta = bend.stations[i].stationMeters -
            bend.stations[i-1].stationMeters;
        CHECK(std::abs(advised[i] - advised[i-1]) <=
            rules.maximumBankGradientRadiansPerMeter * delta + 1.e-12);
    }
    CHECK(SuggestCorridorBankProfile(
        AnalyzeCorridorEngineering(flat, rules, {1.0}), rules).empty());
    CHECK(!AnalyzeCorridorEngineering(flat,rules,{0}).Valid());
    auto invalid = flat;
    invalid[1].stationMeters = 0;
    CHECK(!AnalyzeCorridorEngineering(invalid,rules).Valid());
    invalid = flat;
    invalid[1].position.y = std::numeric_limits<double>::quiet_NaN();
    CHECK(!AnalyzeCorridorEngineering(invalid,rules).Valid());
    CHECK(AnalyzeCorridorEngineering({},rules).Valid());
    return 0;
}
