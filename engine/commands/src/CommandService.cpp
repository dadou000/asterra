#include <orbit/commands/CommandService.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <unordered_set>
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

scene::ObjectRecord
CommandService::RequireObject(
    const scene::ObjectId id) const
{
    const auto object =
        objects_.Find(id);

    if (!object.has_value())
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

    if (activeTransaction_.has_value())
    {
        action.redo(key);
        activeTransaction_->actions.push_back(
            std::move(action));
        return;
    }

    objects_.BeginTransaction(key);

    try
    {
        action.redo(key);
        objects_.CommitTransaction(key);
    }
    catch (...)
    {
        objects_.RollbackTransaction(key);
        throw;
    }

    Transaction transaction;
    transaction.label = std::string(label);
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
    const std::optional<scene::ObjectId> parent,
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
                [this, record](scene::MutationKey key)
                {
                    objects_.Insert(key, record);
                },
            .undo =
                [this, id = record.id](scene::MutationKey key)
                {
                    objects_.Erase(key, id);
                }
        });

    return record.id;
}

scene::ObjectId
CommandService::DuplicateObject(
    const scene::ObjectId object,
    const std::string_view nameSuffix)
{
    const scene::ObjectRecord source =
        RequireObject(object);

    if (!objects_.Children(object).empty())
    {
        throw std::invalid_argument(
            "Generic duplicate supports leaf objects only; duplicate the hierarchy explicitly.");
    }

    const schema::TypeSchema* typeSchema =
        schemas_.FindType(source.type);
    if (typeSchema == nullptr)
    {
        throw std::runtime_error(
            "Cannot duplicate an object with an unknown schema type.");
    }

    const scene::ObjectId duplicate =
        CreateObject(
            source.type,
            source.name + std::string(nameSuffix),
            source.parent,
            source.sortOrder + 1);

    for (const auto& property : typeSchema->properties)
    {
        if (property.readOnly)
        {
            continue;
        }

        const auto value =
            objects_.GetProperty(object, property.id);
        if (value.has_value())
        {
            SetProperty(
                duplicate,
                property.id,
                *value);
        }
    }

    return duplicate;
}

void CommandService::DeleteObject(const scene::ObjectId object)
{
    const std::array roots{object};
    DeleteObjects(roots);
}

void CommandService::DeleteObjects(
    const std::span<const scene::ObjectId> objects)
{
    struct SavedObject
    {
        scene::ObjectRecord record;
        std::vector<std::pair<schema::PropertyId,schema::PropertyValue>>
            properties;
    };

    if(objects.empty())return;

    // Read and validate the ENTIRE subtree before mutating the world.
    // Preorder gives parent-first insertion on undo and child-first erasure
    // on redo. A shared child selected alongside its parent is visited once.
    std::vector<SavedObject> snapshot;
    std::unordered_set<scene::ObjectId> visited;
    std::unordered_set<scene::ObjectId> selected;
    for(const auto id:objects)
    {
        if(!id)continue;
        static_cast<void>(RequireObject(id));
        selected.insert(id);
    }

    // Normalize overlapping selections BEFORE traversal: if a child was
    // selected before its parent, undo must still insert the parent first.
    std::vector<scene::ObjectId> pending;
    pending.reserve(selected.size());
    for(const auto id:selected)
    {
        bool nested=false;
        auto parent=RequireObject(id).parent;
        while(parent)
        {
            if(selected.contains(*parent)) {nested=true;break;}
            parent=RequireObject(*parent).parent;
        }
        if(!nested)pending.push_back(id);
    }
    while(!pending.empty())
    {
        const auto id=pending.back();
        pending.pop_back();
        if(!visited.insert(id).second)continue;

        const auto record=RequireObject(id);
        const auto* schema=schemas_.FindType(record.type);
        if(!schema)
            throw std::runtime_error(
                "Cannot delete subtree containing an unknown schema type.");

        SavedObject saved{.record=record};
        for(const auto& property:schema->properties)
        {
            if(const auto value=objects_.GetProperty(id,property.id))
                saved.properties.emplace_back(property.id,*value);
        }
        snapshot.push_back(std::move(saved));
        for(const auto& child:objects_.Children(id))
            pending.push_back(child.id);
    }
    if(snapshot.empty())return;

    // Large hierarchies carry potentially large property blobs. Share one
    // immutable undo snapshot across both lambdas rather than copying every
    // descendant's data twice into command history.
    const auto saved=std::make_shared<const std::vector<SavedObject>>(
        std::move(snapshot));
    ApplyAndRecord(
        saved->size()==1U?"Delete Object":"Delete Object Hierarchy",
        Action{
            .redo=[this,saved](scene::MutationKey key)
            {
                for(auto it=saved->rbegin();it!=saved->rend();++it)
                    objects_.Erase(key,it->record.id);
            },
            .undo=[this,saved](scene::MutationKey key)
            {
                for(const auto& item:*saved)
                {
                    objects_.Insert(key,item.record);
                    for(const auto& [property,value]:item.properties)
                        objects_.SetProperty(
                            key,item.record.id,property,value);
                }
            }
        });
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
                [this, object, name](scene::MutationKey key)
                {
                    objects_.Rename(
                        key,
                        object,
                        name);
                },
            .undo =
                [this, object, oldName](scene::MutationKey key)
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
    const std::optional<scene::ObjectId> parent)
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
                [this, object, parent](scene::MutationKey key)
                {
                    objects_.Reparent(
                        key,
                        object,
                        parent);
                },
            .undo =
                [this, object, oldParent](scene::MutationKey key)
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

    const schema::PropertySchema* propertySchema =
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
                [this, object, property, value](scene::MutationKey key)
                {
                    objects_.SetProperty(
                        key,
                        object,
                        property,
                        value);
                },
            .undo =
                [this, object, property, previous](scene::MutationKey key)
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

void CommandService::ResetProperty(
    const scene::ObjectId object,
    const schema::PropertyId property)
{
    const scene::ObjectRecord current =
        RequireObject(object);
    const schema::PropertySchema* propertySchema =
        schemas_.FindProperty(current.type, property);

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

    const auto previous =
        objects_.GetProperty(object, property);
    if (!previous.has_value())
    {
        return;
    }

    ApplyAndRecord(
        "Reset Property",
        Action{
            .redo =
                [this, object, property](scene::MutationKey key)
                {
                    objects_.RemoveProperty(
                        key,
                        object,
                        property);
                },
            .undo =
                [this, object, property, previous](scene::MutationKey key)
                {
                    objects_.SetProperty(
                        key,
                        object,
                        property,
                        *previous);
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

    scene::MutationKey key;
    objects_.BeginTransaction(key);

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

    scene::MutationKey key;

    try
    {
        objects_.CommitTransaction(key);
    }
    catch (...)
    {
        objects_.RollbackTransaction(key);
        activeTransaction_.reset();
        throw;
    }

    if (!activeTransaction_->actions.empty())
    {
        undoStack_.push_back(
            std::move(*activeTransaction_));
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
    objects_.RollbackTransaction(key);
    activeTransaction_.reset();
}

bool CommandService::HasActiveTransaction() const noexcept
{
    return activeTransaction_.has_value();
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

std::vector<std::string> CommandService::UndoLabels(
    const std::size_t limit) const
{
    const std::size_t count =
        std::min(limit, undoStack_.size());
    std::vector<std::string> result;
    result.reserve(count);

    for (std::size_t offset = 0U;
         offset < count;
         ++offset)
    {
        result.push_back(
            undoStack_[undoStack_.size() - 1U - offset].label);
    }
    return result;
}

std::vector<std::string> CommandService::RedoLabels(
    const std::size_t limit) const
{
    const std::size_t count =
        std::min(limit, redoStack_.size());
    std::vector<std::string> result;
    result.reserve(count);

    for (std::size_t offset = 0U;
         offset < count;
         ++offset)
    {
        result.push_back(
            redoStack_[redoStack_.size() - 1U - offset].label);
    }
    return result;
}

void CommandService::Undo()
{
    if (!CanUndo())
    {
        throw std::logic_error(
            "Nothing to undo.");
    }

    Transaction transaction =
        std::move(undoStack_.back());
    undoStack_.pop_back();

    scene::MutationKey key;
    objects_.BeginTransaction(key);

    try
    {
        for (auto iterator = transaction.actions.rbegin();
             iterator != transaction.actions.rend();
             ++iterator)
        {
            iterator->undo(key);
        }

        objects_.CommitTransaction(key);
    }
    catch (...)
    {
        objects_.RollbackTransaction(key);
        undoStack_.push_back(
            std::move(transaction));
        throw;
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
        std::move(redoStack_.back());
    redoStack_.pop_back();

    scene::MutationKey key;
    objects_.BeginTransaction(key);

    try
    {
        for (Action& action : transaction.actions)
        {
            action.redo(key);
        }

        objects_.CommitTransaction(key);
    }
    catch (...)
    {
        objects_.RollbackTransaction(key);
        redoStack_.push_back(
            std::move(transaction));
        throw;
    }

    undoStack_.push_back(
        std::move(transaction));
}
} // namespace orbit::commands
