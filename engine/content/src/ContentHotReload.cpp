#include <orbit/content/ContentService.hpp>

#include <orbit/core/Log.hpp>
#include <orbit/hot_reload/HotIterationService.hpp>

#include <exception>
#include <filesystem>
#include <format>
#include <system_error>

namespace orbit::content
{
namespace
{
[[nodiscard]] bool IsInsideOrEqual(
    const std::filesystem::path& path,
    const std::filesystem::path& root)
{
    std::error_code error;
    const auto canonicalPath =
        std::filesystem::weakly_canonical(
            path,
            error);
    if (error)
    {
        return false;
    }

    error.clear();
    const auto canonicalRoot =
        std::filesystem::weakly_canonical(
            root,
            error);
    if (error)
    {
        return false;
    }

    if (canonicalPath == canonicalRoot)
    {
        return true;
    }

    const auto relative =
        canonicalPath.lexically_relative(
            canonicalRoot);

    if (relative.empty() ||
        relative.is_absolute())
    {
        return false;
    }

    const auto first = relative.begin();
    return first != relative.end() &&
        *first != std::filesystem::path("..");
}
} // namespace

ContentService::HotIterationRegistration::HotIterationRegistration(
    ContentService* const owner)
    : owner_(owner)
{
    if (owner_ == nullptr)
    {
        return;
    }

    hot_reload::AddHotIterationWatchRoot(
        owner_->projectRoot_);

    const auto handle =
        [owner](const hot_reload::HotIterationEvent& event)
        {
            if (owner == nullptr ||
                !IsInsideOrEqual(
                    event.path,
                    owner->contentRoot_))
            {
                return;
            }

            try
            {
                const u64 before =
                    owner->Revision();
                owner->Scan();

                log::Info(
                    std::format(
                        "Hot iteration refreshed Content after '{}'; revision {} -> {}.",
                        event.path.filename().string(),
                        before,
                        owner->Revision()));
            }
            catch (const std::exception& exception)
            {
                log::Error(
                    std::format(
                        "Hot content refresh failed for '{}': {}",
                        event.path.string(),
                        exception.what()));
            }
        };

    contentHandler_ =
        hot_reload::AddHotIterationHandler(
            hot_reload::ChangeKind::Content,
            handle);

    // HLSL assets underneath Content are indexed by ContentService as Shader
    // assets. They use the same rescan/import path; engine-owned shader source
    // outside Content is classified for the native fallback refresh instead.
    shaderHandler_ =
        hot_reload::AddHotIterationHandler(
            hot_reload::ChangeKind::Shader,
            handle);
}

ContentService::HotIterationRegistration::~HotIterationRegistration()
{
    hot_reload::RemoveHotIterationHandler(
        contentHandler_);
    hot_reload::RemoveHotIterationHandler(
        shaderHandler_);
    contentHandler_ = 0U;
    shaderHandler_ = 0U;
    owner_ = nullptr;
}
} // namespace orbit::content
