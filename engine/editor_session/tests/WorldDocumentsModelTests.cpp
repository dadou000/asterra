#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/editor_session/WorldDocumentsModel.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "World documents model test failed.\n";
        std::exit(1);
    }
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-world-documents-model-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "World Documents Test");
        orbit::editor_session::EditorWorldSession session(project);
        orbit::editor_session::WorldDocumentsModel model(session);

        auto catalog = model.Catalog();
        Check(catalog.size() == 1U);
        Check(catalog.front().active);
        Check(catalog.front().descriptor.startup);
        Check(catalog.front().descriptor.displayName == "Main");

        const auto mainId = catalog.front().descriptor.id;
        const auto generationBeforeSwitch = session.Generation();

        const auto secondary =
            model.Create(
                "Systems/Secondary",
                "Secondary");
        Check(
            secondary.relativePath ==
            std::filesystem::path(
                "Worlds/Systems/Secondary.orbitworld"));
        Check(secondary.id != mainId);

        const auto renamed =
            model.Rename(
                secondary.relativePath,
                "Secondary System");
        Check(renamed.id == secondary.id);
        Check(renamed.displayName == "Secondary System");

        const auto startup =
            model.SetStartup(
                secondary.relativePath);
        Check(startup.startup);
        Check(model.Active()->descriptor.id == mainId);

        const auto opened =
            model.Open(
                secondary.relativePath);
        Check(opened.id == secondary.id);
        Check(session.Generation() > generationBeforeSwitch);
        Check(model.Active()->descriptor.id == secondary.id);

        catalog = model.Catalog();
        Check(catalog.size() == 2U);

        bool foundActiveSecondary = false;
        bool foundInactiveMain = false;
        for (const auto& item : catalog)
        {
            if (item.descriptor.id == secondary.id)
            {
                Check(item.active);
                Check(item.descriptor.startup);
                foundActiveSecondary = true;
            }
            else if (item.descriptor.id == mainId)
            {
                Check(!item.active);
                Check(!item.descriptor.startup);
                foundInactiveMain = true;
            }
        }
        Check(foundActiveSecondary);
        Check(foundInactiveMain);

        model.Close();
        Check(!model.Active().has_value());
        Check(model.Catalog().size() == 2U);

        const auto reopened = model.Open("Main");
        Check(reopened.id == mainId);
        Check(model.Active()->descriptor.id == mainId);
    }

    std::filesystem::remove_all(root);
    return 0;
}
