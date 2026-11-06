#pragma once
#include <raynium/runtime.h>
#include <raynium/bridge.h>
#include <radray/runtime/rdg.h>
#include <array>

namespace raynium {
using RenderBuffer = radray::unique_ptr<radray::render::Buffer>;
RenderBuffer CreateBuffer(radray::render::Device&, uint64_t, radray::render::MemoryType, radray::render::BufferUses,
                          radray::render::ResourceHints = {});

// Upload buffer that stays mapped for its lifetime. Callers may only overwrite or grow it after the
// previous GPU use of that owner completed; growth replaces the buffer and discards its contents.
class MappedUploadBuffer final {
public:
    MappedUploadBuffer(const MappedUploadBuffer&) = delete;
    MappedUploadBuffer& operator=(const MappedUploadBuffer&) = delete;
    ~MappedUploadBuffer() noexcept;
    static bool Create(radray::render::Device&, uint64_t capacity, radray::render::BufferUses,
                       radray::unique_ptr<MappedUploadBuffer>&, radray::string&);
    bool Reserve(radray::render::Device&, uint64_t bytes, radray::string&); // 2^n capacity, drops old contents
    void Flush(uint64_t offset, uint64_t bytes) noexcept;
    radray::render::Buffer* Get() const noexcept { return _buffer.get(); }
    std::byte* Data() const noexcept { return _data; }
    uint64_t Capacity() const noexcept { return _capacity; }
private:
    MappedUploadBuffer(RenderBuffer buffer, std::byte* data, uint64_t capacity, radray::render::BufferUses usage) noexcept
        :_buffer(std::move(buffer)),_data(data),_capacity(capacity),_usage(usage) {}
    RenderBuffer _buffer;
    std::byte* _data{};
    uint64_t _capacity{};
    radray::render::BufferUses _usage{};
};
void TextureBarrier(radray::render::CommandBuffer&, radray::render::Texture*, radray::render::TextureStates,
                    radray::render::TextureStates, radray::render::ExternalOwnershipTransfer = radray::render::ExternalOwnershipTransfer::None);
void BufferBarrier(radray::render::CommandBuffer&, radray::render::Buffer*, radray::render::BufferStates, radray::render::BufferStates);
rn_shared_image_info DescribeImage(const radray::render::ExternalTextureExport&,
                                  const radray::render::ExternalFenceExport&, const radray::render::ExternalFenceExport&);

class RenderTarget final {
public:
    static bool Create(RayniumRuntime&, const rn_render_target_desc&, radray::unique_ptr<RenderTarget>&, radray::string&);
    bool Finish(radray::string&);
    rn_shared_image_info Info() const { return DescribeImage(_r.exported,_r.readyExport,_r.consumedExport); }
    uint32_t Width() const { return _width; }
    uint32_t Height() const { return _height; }
    uint64_t Submitted() const { return _submitted; }
    radray::render::Fence* Ready() const { return _r.ready.get(); }
    radray::render::Fence* Consumed() const { return _r.consumed.get(); }
    radray::render::Texture* Color() const { return _r.color.get(); }
    radray::render::TextureView* ColorView() const { return _r.colorView.get(); }
    void Commit(uint64_t sequence) noexcept { _submitted=sequence; }
private:
    struct Resources {
        radray::unique_ptr<radray::render::Texture> color;
        radray::unique_ptr<radray::render::TextureView> colorView; // Declared after color; dies first.
        radray::unique_ptr<radray::render::Fence> ready, consumed;
        radray::render::ExternalTextureExport exported;
        radray::render::ExternalFenceExport readyExport, consumedExport;
    };
    RenderTarget(Resources&& r,uint32_t width,uint32_t height):_r(std::move(r)),_width(width),_height(height) {}
    Resources _r;
    uint32_t _width, _height;
    uint64_t _submitted{};
};

class SharedTextures final {
public:
    static bool Create(RayniumRuntime&, const rn_shared_textures_desc&, radray::unique_ptr<SharedTextures>&, radray::string&);
    bool Finish(radray::string&);
    bool Validate(uint64_t sequence,uint32_t changed,radray::string&) const;
    void DescribeExports(rn_shared_image_export*) const noexcept;
    uint32_t Count() const { return static_cast<uint32_t>(_logical.size()); }
    uint32_t Mips(uint32_t i) const { return _logical.at(i).desc.mip_levels; }
    radray::render::TextureView* View(uint32_t i) const { return _logical.at(i).view.get(); }
    radray::render::Fence* Released() const { return _sync.released.get(); }
    radray::render::Fence* Available() const { return _sync.available.get(); }
    // Imports every physical shared image with an acquire/release pair, including unused images.
    radray::vector<radray::RDGTextureHandle> SetupGraph(radray::RDGBuilder&,uint32_t changed) const;
    radray::render::Texture* SampledTexture(uint32_t i) const { return _logical.at(i).sampled; }
    void Commit(uint32_t changed) noexcept;
private:
    struct Image { radray::unique_ptr<radray::render::Texture> texture; radray::render::ExternalTextureExport exported; uint32_t logical,mip; };
    struct Logical {
        rn_texture_desc desc;
        radray::unique_ptr<radray::render::Texture> assembled;
        radray::unique_ptr<radray::render::TextureView> view;
        bool initialized{};
        radray::render::Texture* sampled{};
    };
    struct Sync {
        radray::unique_ptr<radray::render::Fence> released,available;
        radray::render::ExternalFenceExport releasedExport,availableExport;
        radray::unique_ptr<radray::render::CommandAllocator> allocator;
        radray::unique_ptr<radray::render::CommandBuffer> command;
    };
    SharedTextures(std::vector<Image>&& images,std::vector<Logical>&& logical,Sync&& sync)
        :_images(std::move(images)),_logical(std::move(logical)),_sync(std::move(sync)) {}
    std::vector<Image> _images;
    std::vector<Logical> _logical;
    Sync _sync;
    uint64_t _released{1};
};

}
