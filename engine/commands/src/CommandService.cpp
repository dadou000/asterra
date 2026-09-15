#include <orbit/commands/CommandService.hpp>

#include <stdexcept>
#include <utility>

namespace orbit::commands
{
CommandService::CommandService(
    scene::ObjectStore& objects,
    const schema::SchemaRegistry& schemas)
    : objects_(objects),
      schemas_(schemas)
{
}

const scene::ObjectRecord&
CommandService::RequireObject(
    const scene::ObjectId id) const
{
    const scene::ObjectRecord* object =
        objects_.Find(id);

    if (object == nullptr)
    {
        throw std::invalid_argument(
            "Command references unknown object.");
    }

    return *object;
}

void CommandService::ApplyAndRecord(
    const std::string_view label,
    Action action)
{
    scene::MutationKey key;
    action.redo(key);

    if (activeTransaction_.has_value())
    {
        activeTransaction_->
            actions.push_back(
                std::move(action));
        return;
    }

    Transaction transaction;
    transaction.label =
        std::string(label);
    transaction.actions.push_back(
        std::move(action));

    undoStack_.push_back(
        std::move(transaction));
    redoStack_.clear();
}

scene::ObjectId
CommandService::CreateObject(
    const schema::TypeId type,
    const std::string_view name,
    const std::optional<
        scene::ObjectId> parent,
    const i64 sortOrder)
{
    if (schemas_.FindType(type) == nullptr)
    {
        throw std::invalid_argument(
            "Cannot create object with unknown schema type.");
    }

    scene::ObjectRecord record{
        .id = scene::ObjectId::Random(),
        .parent = parent,
        .type = type,
        .name = std::string(name),
        .sortOrder = sortOrder
    };

    ApplyAndRecord(
        "Create Object",
        Action{
            .redo =
                [this, record](
                    scene::MutationKey key)
                {
                    objects_.Insert(
                        key,
                        record);
                },
            .undo =
                [this, id = record.id](
                    scene::MutationKey key)
                {
                    objects_.Erase(
                        key,
                        id);
                }
        });

    return record.id;
}

void CommandService::RenameObject(
    const scene::ObjectId object,
    std::string name)
{
    const std::string oldName =
        RequireObject(object).name;

    if (oldName == name)
    {
        return;
    }

    ApplyAndRecord(
        "Rename Object",
        Action{
            .redo =
                [this,
                 object,
                 name](
                    scene::MutationKey key)
                {
                    objects_.Rename(
                        key,
                        object,
                        name);
                },
            .undo =
                [this,
                 object,
                 oldName](
                    scene::MutationKey key)
                {
                    objects_.Rename(
                        key,
                        object,
                        oldName);
                }
        });
}

void CommandService::ReparentObject(
    const scene::ObjectId object,
    const std::optional<
        scene::ObjectId> parent)
{
    const auto oldParent =
        RequireObject(object).parent;

    if (oldParent == parent)
    {
        return;
    }

    ApplyAndRecord(
        "Reparent Object",
        Action{
            .redo =
                [this,
                 object,
                 parent](
                    scene::MutationKey key)
                {
                    objects_.Reparent(
                        key,
                        object,
                        parent);
                },
            .undo =
                [this,
                 object,
                 oldParent](
                    scene::MutationKey key)
                {
                    objects_.Reparent(
                        key,
                        object,
                        oldParent);
                }
        });
}

void CommandService::SetProperty(
    const scene::ObjectId object,
    const schema::PropertyId property,
    schema::PropertyValue value)
{
    const scene::ObjectRecord current =
        RequireObject(object);

    const schema::PropertySchema*
        propertySchema =
            schemas_.FindProperty(
                current.type,
                property);

    if (propertySchema == nullptr)
    {
        throw std::invalid_argument(
            "Property is not defined for object type.");
    }

    if (propertySchema->readOnly)
    {
        throw std::invalid_argument(
            "Property is read-only.");
    }

    if (!schemas_.ValidateValue(
            *propertySchema,
            value))
    {
        throw std::invalid_argument(
            "Property value does not satisfy schema.");
    }

    const auto previous =
        objects_.GetProperty(
            object,
            property);

    if (previous.has_value() &&
        *previous == value)
    {
        return;
    }

    ApplyAndRecord(
        "Set Property",
        Action{
            .redo =
                [this,
                 object,
                 property,
                 value](
                    scene::MutationKey key)
                {
                    objects_.SetProperty(
                        key,
                        object,
                        property,
                        value);
                },
            .undo =
                [this,
                 object,
                 property,
                 previous](
                    scene::MutationKey key)
                {
                    if (previous.has_value())
                    {
                        objects_.SetProperty(
                            key,
                            object,
                            property,
                            *previous);
                    }
                    else
                    {
                        objects_.RemoveProperty(
                            key,
                            object,
                            property);
                    }
                }
        });
}

void CommandService::BeginTransaction(
    std::string label)
{
    if (activeTransaction_.has_value())
    {
        throw std::logic_error(
            "Nested authoring transactions are not supported.");
    }

    if (label.empty())
    {
        label = "Edit";
    }

    activeTransaction_ =
        Transaction{
            .label = std::move(label)
        };
}

void CommandService::CommitTransaction()
{
    if (!activeTransaction_.has_value())
    {
        throw std::logic_error(
            "No active authoring transaction.");
    }

    if (!activeTransaction_->
            actions.empty())
    {
        undoStack_.push_back(
            std::move(
                *activeTransaction_));
        redoStack_.clear();
    }

    activeTransaction_.reset();
}

void CommandService::RollbackTransaction()
{
    if (!activeTransaction_.has_value())
    {
        throw std::logic_error(
            "No active authoring transaction.");
    }

    scene::MutationKey key;

    for (auto iterator =
             activeTransaction_->
                 actions.rbegin();
         iterator !=
             activeTransaction_->
                 actions.rend();
         ++iterator)
    {
        iterator->undo(key);
    }

    activeTransaction_.reset();
}

bool CommandService::CanUndo() const noexcept
{
    return !undoStack_.empty() &&
        !activeTransaction_.has_value();
}

bool CommandService::CanRedo() const noexcept
{
    return !redoStack_.empty() &&
        !activeTransaction_.has_value();
}

void CommandService::Undo()
{
    if (!CanUndo())
    {
        throw std::logic_error(
            "Nothing to undo.");
    }

    Transaction transaction =
        std::move(
            undoStack_.back());
    undoStack_.pop_back();

    scene::MutationKey key;

    for (auto iterator =
             transaction.actions.rbegin();
         iterator !=
             transaction.actions.rend();
         ++iterator)
    {
        iterator->undo(key);
    }

    redoStack_.push_back(
        std::move(transaction));
}

void CommandService::Redo()
{
    if (!CanRedo())
    {
        throw std::logic_error(
            "Nothing to redo.");
    }

    Transaction transaction =
        std::move(
            redoStack_.back());
    redoStack_.pop_back();

    scene::MutationKey key;

    for (Action& action :
         transaction.actions)
    {
        action.redo(key);
    }

    undoStack_.push_back(
        std::move(transaction));
}
} // namespace orbit::commands
