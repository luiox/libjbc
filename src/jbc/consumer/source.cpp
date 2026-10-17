#include "jbc/consumer/source.hpp"

namespace jbc::consumer {

void MemoryClassSource::add(std::string_view internalName, std::vector<ca::u8> bytes)
{
    // std::map 异构查找（std::less<>）——string_view 直接定位；同键 upsert。
    const auto it = classes_.find(internalName);
    if (it != classes_.end()) {
        it->second = std::move(bytes);
        return;
    }
    // 新键：string_view 构造为 owned 键；字节移动接管（节点稳定，
    // 既有 string_view 分发的键字节不迁移）。
    classes_.emplace(std::string(internalName), std::move(bytes));
}

ConsumerResult<jbc::core::ByteView> MemoryClassSource::bytes(std::string_view internalName) const
{
    const auto it = classes_.find(internalName);
    if (it == classes_.end()) {
        return ca::core::Err(ConsumerError::NotFound);
    }
    return ca::core::Ok(jbc::core::ByteView{it->second});
}

void MemoryClassSource::listClasses(std::vector<std::string_view>& out) const
{
    // 升序（std::map 字符串排序）；视图指涉 owned 键（节点稳定）。
    for (const auto& [name, payload] : classes_) {
        out.push_back(name);
    }
}

}   // namespace jbc::consumer
