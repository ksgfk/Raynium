#pragma once
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <vector>

namespace raynium {
// Nonzero primitive IDs map to dense hot-table indices; lookups return values, not references into this table.
class TerrainPrimitiveIndex final {
public:
    static constexpr uint32_t Missing=UINT32_MAX;
    uint32_t Find(uint64_t id) const noexcept {
        if(_slots.empty() || !id) return Missing;
        const auto slot=FindSlot(id);
        return _slots[slot].id?_slots[slot].index:Missing;
    }
    void Insert(uint64_t id,uint32_t index) {
        assert(id && index!=Missing);
        Reserve(_size+1);
        auto& slot=_slots[FindSlot(id)];
        if(!slot.id) ++_size;
        slot={id,index};
    }
    void Set(uint64_t id,uint32_t index) noexcept {
        const auto slot=FindSlot(id); assert(_slots[slot].id==id);
        _slots[slot].index=index;
    }
    void Erase(uint64_t id) noexcept {
        if(_slots.empty() || !id) return;
        auto hole=FindSlot(id); if(!_slots[hole].id) return;
        const auto mask=_slots.size()-1;
        for(auto next=(hole+1)&mask;_slots[next].id;next=(next+1)&mask) {
            const auto home=Hash(_slots[next].id)&mask;
            if(((hole-home)&mask)<((next-home)&mask)) { _slots[hole]=_slots[next]; hole=next; }
        }
        _slots[hole]={}; --_size;
    }
    void Reserve(uint32_t count) {
        const auto capacity=std::bit_ceil(std::max<size_t>(16,size_t(count)*2));
        if(capacity<=_slots.size()) return;
        std::vector<Entry> slots(capacity);
        for(const auto& entry:_slots) if(entry.id) {
            auto slot=Hash(entry.id)&(capacity-1);
            while(slots[slot].id) slot=(slot+1)&(capacity-1);
            slots[slot]=entry;
        }
        _slots.swap(slots);
    }
    uint32_t Size() const noexcept { return _size; }
private:
    struct Entry { uint64_t id{}; uint32_t index{}; };
    static size_t Hash(uint64_t id) noexcept {
        id^=id>>30; id*=0xbf58476d1ce4e5b9ULL; id^=id>>27; id*=0x94d049bb133111ebULL;
        return size_t(id^(id>>31));
    }
    size_t FindSlot(uint64_t id) const noexcept {
        const auto mask=_slots.size()-1; auto slot=Hash(id)&mask;
        while(_slots[slot].id && _slots[slot].id!=id) slot=(slot+1)&mask;
        return slot;
    }
    std::vector<Entry> _slots;
    uint32_t _size{};
};
}
