#pragma once
#include <radray/types.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <span>
#include <type_traits>

namespace raynium {
template<class T>
std::span<T> FrameArray(std::pmr::memory_resource& memory,size_t count) {
    static_assert(std::is_trivially_destructible_v<T>);
    if(!count) return {};
    auto* data=std::pmr::polymorphic_allocator<T>(&memory).allocate(count);
    std::uninitialized_value_construct_n(data,count);
    return {data,count};
}

template<class T>
std::span<T> FreezeFrameArray(std::pmr::memory_resource& memory,const std::pmr::vector<T>& source) {
    auto result=FrameArray<T>(memory,source.size());
    std::copy(source.begin(),source.end(),result.begin());
    return result;
}

inline std::span<const radray::byte> FramePushConstants(std::pmr::memory_resource& memory,
    std::span<const radray::byte> bytes,size_t required) {
    if(bytes.size()>=required) return bytes.first(required);
    auto padded=FrameArray<radray::byte>(memory,required);
    std::copy(bytes.begin(),bytes.end(),padded.begin());
    return padded;
}

// The tag is the index in the closed list of payload types. Small trivial payloads live inline; larger ones share the frame slot lifetime.
template<class... Types>
class FrameOp {
public:
    template<class T>
    static constexpr bool kInline=sizeof(T)<=sizeof(uintptr_t) && alignof(T)<=alignof(uintptr_t) &&
        std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

    FrameOp()=default;
    template<class T> requires (!kInline<T>)
    explicit FrameOp(T* data) noexcept:_tag(Tag<T>()) { _payload.ptr=data; }
    template<class T> requires (kInline<T>)
    explicit FrameOp(T value) noexcept:_tag(Tag<T>()) {
        _payload.raw=0;
        std::memcpy(_payload.inlineBytes,&value,sizeof(T));
    }
    template<class T> auto Get() const noexcept {
        if constexpr(kInline<T>) {
            if(_tag!=Tag<T>()) return std::optional<T>{};
            T value;
            std::memcpy(&value,_payload.inlineBytes,sizeof(T));
            return std::optional<T>{value};
        } else {
            return _tag==Tag<T>()?static_cast<T*>(_payload.ptr):nullptr;
        }
    }
private:
    template<class T> static consteval size_t Tag() {
        static_assert((std::is_same_v<T,Types> || ...));
        constexpr bool matches[]{std::is_same_v<T,Types>...};
        for(size_t i=0;i<sizeof...(Types);++i) if(matches[i]) return i;
        return sizeof...(Types);
    }
    union Payload {
        void* ptr{};
        uintptr_t raw;
        alignas(uintptr_t) radray::byte inlineBytes[sizeof(uintptr_t)];
    } _payload{};
    size_t _tag{sizeof...(Types)};
};
}
