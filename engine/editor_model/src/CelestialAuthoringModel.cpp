#include <orbit/editor_model/CelestialAuthoringModel.hpp>

#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/PropertyProvenanceSchema.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace orbit::editor_model
{
namespace
{
using namespace world_model;

constexpr std::array<CelestialCapabilityDescriptor, 16>
kCapabilities{{
    {kReferenceShapeCapabilityType, "Reference Shape", true},
    {kMassPropertiesCapabilityType, "Mass Properties", true},
    {kOrbitCapabilityType, "Orbit / Ephemeris", true},
    {kRotationCapabilityType, "Rotation / Orientation", true},
    {kGravityCapabilityType, "Gravity", true},
    {kSurfaceCapabilityType, "Surface", true},
    {kAtmosphereCapabilityType, "Atmosphere", true},
    {kOceanCapabilityType, "Ocean", true},
    {kCloudLayerCapabilityType, "Cloud Layer", false},
    {kRingSystemCapabilityType, "Ring System", true},
    {kRadiativeEmitterCapabilityType, "Radiative Emitter", true},
    {kPhotosphereCapabilityType, "Photosphere", true},
    {kMagnetosphereCapabilityType, "Magnetosphere / Aurora", true},
    {kCometTailCapabilityType, "Comet Tail", true},
    {kCompactObjectCapabilityType, "Compact Object", true},
    {kAccretionFlowCapabilityType, "Accretion Flow", true}
}};

[[nodiscard]] bool IsAllowedStructuralParent(
    const schema::TypeId type) noexcept
{
    return type == kCelestialSystemType ||
           type == kCelestialReferenceNodeType ||
           type == kCelestialBodyType;
}
} // namespace

CelestialAuthoringModel::CelestialAuthoringModel(
    scene::ObjectStore& objects,
    const schema::SchemaRegistry& schemas,
    commands::CommandService& commands,
    selection::SelectionService& selection)
    : objects_(objects),
      schemas_(schemas),
      commands_(commands),
      selection_(selection)
{
}

std::optional<scene::ObjectRecord>
CelestialAuthoringModel::PrimarySelection() const
{
    if (selection_.Ordered().empty())
    {
        return std::nullopt;
    }

    return objects_.Find(
        selection_.Ordered().front());
}

std::optional<scene::ObjectRecord>
CelestialAuthoringModel::NearestBody(
    scene::ObjectId start) const
{
    auto current = objects_.Find(start);

    while (current.has_value())
    {
        if (current->type ==
            world_model::kCelestialBodyType)
        {
            return current;
        }

        if (!current->parent.has_value())
        {
            break;
        }

        current = objects_.Find(
            *current->parent);
    }

    return std::nullopt;
}

std::optional<scene::ObjectRecord>
CelestialAuthoringModel::SelectedBody() const
{
    const auto selected = PrimarySelection();

    if (!selected.has_value())
    {
        return std::nullopt;
    }

    return NearestBody(selected->id);
}

std::optional<scene::ObjectRecord>
CelestialAuthoringModel::FindWorldRoot() const
{
    for (const auto& root : objects_.Roots())
    {
        if (root.type ==
            world_model::kWorldType)
        {
            return root;
        }
    }

    return std::nullopt;
}

std::optional<scene::ObjectRecord>
CelestialAuthoringModel::FindSystemAncestor(
    scene::ObjectId start) const
{
    auto current = objects_.Find(start);

    while (current.has_value())
    {
        if (current->type ==
            world_model::kCelestialSystemType)
        {
            return current;
        }

        if (!current->parent.has_value())
        {
            break;
        }

        current = objects_.Find(
            *current->parent);
    }

    return std::nullopt;
}

void CelestialAuthoringModel::SelectOnly(
    const scene::ObjectId object)
{
    const std::array selection{object};
    selection_.Set(std::span(selection));
}

scene::ObjectId CelestialAuthoringModel::CreateSystem(
    const std::string_view name)
{
    const auto world = FindWorldRoot();

    if (!world.has_value())
    {
        throw std::runtime_error(
            "A World root is required before creating a celestial system.");
    }

    const auto created =
        commands_.CreateObject(
            world_model::kCelestialSystemType,
            name,
            world->id);

    SelectOnly(created);
    return created;
}

scene::ObjectId CelestialAuthoringModel::CreateBody(
    const std::string_view name)
{
    const auto selected = PrimarySelection();

    if (!selected.has_value())
    {
        throw std::runtime_error(
            "Select a celestial system, reference node, or body before creating a body.");
    }

    scene::ObjectId parent{};

    if (IsAllowedStructuralParent(
            selected->type))
    {
        parent = selected->id;
    }
    else if (const auto body =
                 NearestBody(selected->id);
             body.has_value())
    {
        parent = body->id;
    }
    else if (const auto system =
                 FindSystemAncestor(selected->id);
             system.has_value())
    {
        parent = system->id;
    }

    if (!parent)
    {
        throw std::runtime_error(
            "Selected object is not inside a celestial system.");
    }

    const auto created =
        commands_.CreateObject(
            world_model::kCelestialBodyType,
            name,
            parent);

    SelectOnly(created);
    return created;
}

scene::ObjectId
CelestialAuthoringModel::CreateReferenceNode(
    const std::string_view name)
{
    const auto selected = PrimarySelection();

    if (!selected.has_value() ||
        !IsAllowedStructuralParent(
            selected->type))
    {
        throw std::runtime_error(
            "Select a celestial system, reference node, or body before creating a reference node.");
    }

    const auto created =
        commands_.CreateObject(
            world_model::kCelestialReferenceNodeType,
            name,
            selected->id);

    SelectOnly(created);
    return created;
}

scene::ObjectId CelestialAuthoringModel::AddCapability(
    const schema::TypeId capabilityType,
    const std::string_view name)
{
    const auto descriptor =
        std::find_if(
            kCapabilities.begin(),
            kCapabilities.end(),
            [capabilityType](const auto& item)
            {
                return item.type == capabilityType;
            });

    if (descriptor == kCapabilities.end())
    {
        throw std::invalid_argument(
            "Requested type is not a celestial capability.");
    }

    const auto body = SelectedBody();

    if (!body.has_value())
    {
        throw std::runtime_error(
            "Select a celestial body or one of its capability children first.");
    }

    if (descriptor->singleton)
    {
        for (const auto& child :
             objects_.Children(body->id))
        {
            if (child.type ==
                capabilityType)
            {
                throw std::runtime_error(
                    std::string(descriptor->label) +
                    " already exists on this body.");
            }
        }
    }

    const auto created =
        commands_.CreateObject(
            capabilityType,
            name,
            body->id);

    SelectOnly(created);
    return created;
}

void CelestialAuthoringModel::
RemoveSelectedCapability()
{
    const auto selected = PrimarySelection();

    if (!selected.has_value() ||
        !IsCapabilityType(selected->type))
    {
        throw std::runtime_error(
            "Select a celestial capability to remove.");
    }

    const auto children =
        objects_.Children(
            selected->id);

    const bool onlyProvenanceChildren =
        std::all_of(
            children.begin(),
            children.end(),
            [](const scene::ObjectRecord& child)
            {
                return child.type ==
                    world_model::
                        kPropertyProvenanceType;
            });

    if (!onlyProvenanceChildren)
    {
        throw std::runtime_error(
            "Capability has non-provenance child records; remove those first.");
    }

    commands_.BeginTransaction(
        "Remove Celestial Capability");

    try
    {
        for (const auto& child :
             children)
        {
            commands_.DeleteObject(
                child.id);
        }

        commands_.DeleteObject(
            selected->id);
        commands_.CommitTransaction();
    }
    catch (...)
    {
        if (commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }

        throw;
    }

    selection_.Clear();
}

std::vector<CelestialCapabilityDescriptor>
CelestialAuthoringModel::AvailableCapabilities() const
{
    return {
        kCapabilities.begin(),
        kCapabilities.end()
    };
}

bool CelestialAuthoringModel::IsCapabilityType(
    const schema::TypeId type) const noexcept
{
    return std::find_if(
        kCapabilities.begin(),
        kCapabilities.end(),
        [type](const auto& item)
        {
            return item.type == type;
        }) != kCapabilities.end();
}

std::vector<CelestialDiagnostic>
CelestialAuthoringModel::Validate() const
{
    std::vector<CelestialDiagnostic> result;

    std::vector<scene::ObjectRecord> pending =
        objects_.Roots();

    while (!pending.empty())
    {
        const auto object =
            pending.back();
        pending.pop_back();

        const auto children =
            objects_.Children(object.id);

        pending.insert(
            pending.end(),
            children.begin(),
            children.end());

        if (object.type ==
            world_model::kCelestialSystemType)
        {
            const auto parent =
                object.parent.has_value()
                    ? objects_.Find(*object.parent)
                    : std::nullopt;

            if (!parent.has_value() ||
                parent->type !=
                    world_model::kWorldType)
            {
                result.push_back({
                    .severity =
                        CelestialDiagnosticSeverity::Error,
                    .message =
                        "Celestial System must be parented directly under World.",
                    .object = object.id
                });
            }
        }

        if (object.type ==
                world_model::kCelestialBodyType ||
            object.type ==
                world_model::kCelestialReferenceNodeType)
        {
            if (!object.parent.has_value())
            {
                result.push_back({
                    .severity =
                        CelestialDiagnosticSeverity::Error,
                    .message =
                        "Celestial body/reference node has no structural parent.",
                    .object = object.id
                });
            }
            else if (const auto parent =
                         objects_.Find(*object.parent);
                     !parent.has_value() ||
                     !IsAllowedStructuralParent(
                         parent->type))
            {
                result.push_back({
                    .severity =
                        CelestialDiagnosticSeverity::Error,
                    .message =
                        "Celestial body/reference node must be under a system, reference node, or body.",
                    .object = object.id
                });
            }
        }

        if (object.type ==
            world_model::kCelestialBodyType)
        {
            std::unordered_map<
                schema::TypeId,
                u32>
                counts;

            for (const auto& child : children)
            {
                if (IsCapabilityType(child.type))
                {
                    ++counts[child.type];
                }
            }

            for (const auto& descriptor :
                 kCapabilities)
            {
                if (descriptor.singleton &&
                    counts[descriptor.type] > 1U)
                {
                    result.push_back({
                        .severity =
                            CelestialDiagnosticSeverity::Error,
                        .message =
                            std::string("Body has multiple ") +
                            std::string(descriptor.label) +
                            " capabilities.",
                        .object = object.id
                    });
                }
            }
        }

        if (IsCapabilityType(object.type))
        {
            if (!object.parent.has_value())
            {
                result.push_back({
                    .severity =
                        CelestialDiagnosticSeverity::Error,
                    .message =
                        "Celestial capability has no owning body.",
                    .object = object.id
                });
            }
            else
            {
                const auto parent =
                    objects_.Find(*object.parent);

                if (!parent.has_value() ||
                    parent->type !=
                        world_model::kCelestialBodyType)
                {
                    result.push_back({
                        .severity =
                            CelestialDiagnosticSeverity::Error,
                        .message =
                            "Celestial capability must be a direct child of a Celestial Body.",
                        .object = object.id
                    });
                }
            }

            if (schemas_.FindType(
                    object.type) == nullptr)
            {
                result.push_back({
                    .severity =
                        CelestialDiagnosticSeverity::Error,
                    .message =
                        "Capability schema is not registered.",
                    .object = object.id
                });
            }
        }
    }

    if (result.empty())
    {
        result.push_back({
            .severity =
                CelestialDiagnosticSeverity::Info,
            .message =
                "Celestial hierarchy and capability ownership are valid.",
            .object = std::nullopt
        });
    }

    return result;
}
} // namespace orbit::editor_model
