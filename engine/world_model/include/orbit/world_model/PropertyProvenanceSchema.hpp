#pragma once

#include <orbit/world_model/PropertyProvenance.hpp>

namespace orbit::world_model
{
inline constexpr schema::TypeId kPropertyProvenanceType{
    .high = 0x4f5242495450524fULL, .low = 0x56454e414e434501ULL};

inline constexpr schema::PropertyId kProvenanceTargetProperty{
    .high = 0x4f5242495450524fULL, .low = 0x5654475450524f01ULL};
inline constexpr schema::PropertyId kProvenanceSourceMode{
    .high = 0x4f5242495450524fULL, .low = 0x565352434d4f4401ULL};
inline constexpr schema::PropertyId kProvenanceSolveState{
    .high = 0x4f5242495450524fULL, .low = 0x56534f4c56450101ULL};
inline constexpr schema::PropertyId kProvenanceSourceObject{
    .high = 0x4f5242495450524fULL, .low = 0x56534f424a454301ULL};
inline constexpr schema::PropertyId kProvenanceSourceAsset{
    .high = 0x4f5242495450524fULL, .low = 0x5653415353455401ULL};
inline constexpr schema::PropertyId kProvenanceSourceProperty{
    .high = 0x4f5242495450524fULL, .low = 0x565350524f500001ULL};
inline constexpr schema::PropertyId kProvenanceDiagnostic{
    .high = 0x4f5242495450524fULL, .low = 0x5644494147000001ULL};
inline constexpr schema::PropertyId kProvenanceUncertainty{
    .high = 0x4f5242495450524fULL, .low = 0x56554e4345525401ULL};

void RegisterPropertyProvenanceSchema(schema::SchemaRegistry& schemas);
} // namespace orbit::world_model
