#pragma once

#include <raynium/bridge.h>
#include <raynium/runtime.h>

namespace raynium {
class Terrain;

// See doc/architecture/native-runtime.md for preview ownership and synchronization.
class Preview final {
public:
    static bool Create(RayniumRuntime& runtime, uint32_t width, uint32_t height,
                       radray::unique_ptr<Preview>& output, rn_preview_info& info, radray::string& error) noexcept;
    bool Draw(uint64_t sequence, radray::string& error) noexcept;
    bool DrawTerrain(uint64_t sequence, Terrain& terrain, const rn_terrain_frame& frame, radray::string& error) noexcept;
    bool Finish(radray::string& error) noexcept;
    ~Preview() noexcept = default;
    Preview(const Preview&) = delete;
    Preview& operator=(const Preview&) = delete;

private:
    bool DrawInternal(uint64_t sequence, Terrain* terrain, const rn_terrain_frame* frame, radray::string& error) noexcept;
    struct CommandSlot {
        radray::unique_ptr<radray::render::CommandAllocator> allocator;
        radray::unique_ptr<radray::render::CommandBuffer> command;
    };
    struct Resources {
        radray::unique_ptr<radray::render::Texture> texture;
        radray::unique_ptr<radray::render::Texture> depth;
        radray::unique_ptr<radray::render::TextureView> view;
        radray::unique_ptr<radray::render::TextureView> depthView;
        radray::unique_ptr<radray::render::Shader> vs, ps;
        radray::unique_ptr<radray::render::PipelineLayout> layout;
        radray::unique_ptr<radray::render::GraphicsPipelineState> pipeline;
        radray::unique_ptr<radray::render::Fence> ready, consumed;
        CommandSlot slots[2];
        radray::render::ExternalTextureExport textureExport;
        radray::render::ExternalFenceExport readyExport, consumedExport;
    };
    Preview(RayniumRuntime& runtime, Resources&& resources, uint32_t width, uint32_t height) noexcept;
    RayniumRuntime& _runtime;
    Resources _resources;
    uint32_t _width, _height;
    uint64_t _submitted{0};
    bool _depthInitialized{false};
};
}
