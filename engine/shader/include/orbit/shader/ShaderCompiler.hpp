#pragma once

#include <orbit/core/Types.hpp>

#include <string_view>
#include <vector>

namespace orbit::shader
{
enum class Stage : u8
{
    Vertex,
    Pixel,
    Compute
};

struct CompileRequest
{
    std::string_view source;
    std::string_view entryPoint{"main"};
    Stage stage{Stage::Vertex};
    bool debug{false};
};

struct Binary
{
    Stage stage{Stage::Vertex};
    std::vector<u8> bytecode;
};

class Compiler
{
public:
    virtual ~Compiler() = default;

    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;

    [[nodiscard]] virtual Binary Compile(
        const CompileRequest& request) const = 0;

protected:
    Compiler() = default;
};
} // namespace orbit::shader
