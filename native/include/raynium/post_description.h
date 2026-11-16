#pragma once
#include <radray/types.h>
#include <span>

namespace raynium {
struct PostConstantDescription {
    uint32_t binding,kind;
    radray::array<float,64> values;
};
struct PostInputDescription { uint32_t binding,target; bool linear; };
struct PostPassDescription {
    uint32_t pipeline,output;
    std::span<const PostConstantDescription> constants;
    std::span<const PostInputDescription> inputs;
};
struct PostChainDescription { uint32_t targetCount; std::span<const PostPassDescription> passes; };
}
