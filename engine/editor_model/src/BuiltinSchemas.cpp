#include <orbit/editor_model/BuiltinSchemas.hpp>

#include <orbit/paths/PathNetwork.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

namespace orbit::editor_model::builtin
{
void RegisterSchemas(
    schema::SchemaRegistry& schemas)
{
    world_model::RegisterSchemas(schemas);

    // Path objects are ordinary semantic scene objects. Registering them in
    // the shared catalog makes Explorer, Properties, plugins and MCP discover
    // the same production schemas without an editor-only parallel model.
    paths::RegisterSchemas(schemas);
}
} // namespace orbit::editor_model::builtin
