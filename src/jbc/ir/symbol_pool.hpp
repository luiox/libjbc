/// @file        symbol_pool.hpp
/// @brief       ClassUnit 自持的符号池：去重、按 slot 寻址、生命周期即池。
///
/// ：字符串与描述符不存指向 reader 临时区的裸视图，而是
/// SymbolId；对外返回受池生命周期约束的 Utf8StringRef。实现上用
/// Utf8StringArena（整批共存）+ slot 表 + 去重索引；字节走 intern_raw
/// （classfile 的 modified UTF-8 与 arena 1:1 往返，不校验不失真）。
#pragma once

#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/str/utf8_string_arena.hpp"

#include "jbc/ir/code_dom.hpp"

namespace jbc::ir {

/// @brief 符号句柄：owner 参与 ==（跨池同号结构性不等）。
struct SymbolId
{
    ca::u32 owner = 0;
    ca::u32 slot = 0;

    bool operator==(const SymbolId& o) const noexcept
    {
        return owner == o.owner && slot == o.slot;
    }
    bool operator!=(const SymbolId& o) const noexcept { return !(*this == o); }
};

/// @brief 去重符号池：每个 ClassUnit 一个，独立释放。
class SymbolPool
{
public:
    SymbolPool() noexcept : owner_(nextOwnerId()) {}

    SymbolPool(const SymbolPool&)            = delete;
    SymbolPool& operator=(const SymbolPool&) = delete;

    ca::u32 owner() const noexcept { return owner_; }
    ca::usize size() const noexcept { return slots_.size(); }

    /// @brief 物化一个字符串（modified UTF-8 字节原样）；重复内容返回同句柄。
    SymbolId intern(const ca::u8* data, ca::u32 len)
    {
        const std::string_view key(reinterpret_cast<const char*>(data), len);
        const auto it = byStr_.find(key);
        if (it != byStr_.end()) {
            return SymbolId{owner_, it->second};
        }
        const auto ref  = arena_.intern_raw(data, len);
        const ca::u32 slot = static_cast<ca::u32>(slots_.size());
        slots_.push_back(ref);
        // key 指向 arena 内部（arena 只增不移），视图长期有效。
        byStr_.emplace(std::string_view(reinterpret_cast<const char*>(ref.data()),
                                        ref.byte_length()),
                       slot);
        return SymbolId{owner_, slot};
    }

    /// @brief 便捷重载（std::string_view 的字符按字节看待）。
    SymbolId intern(std::string_view s)
    {
        return intern(reinterpret_cast<const ca::u8*>(s.data()),
                      static_cast<ca::u32>(s.size()));
    }

    /// @brief 取符号文本；句柄无效返回空视图（探测式，不抛错）。
    std::string_view str(SymbolId id) const noexcept
    {
        if (id.owner != owner_ || id.slot >= slots_.size()) {
            return {};
        }
        const auto& r = slots_[id.slot];
        return std::string_view(reinterpret_cast<const char*>(r.data()), r.byte_length());
    }

private:
    ca::u32 owner_;
    ca::str::Utf8StringArena arena_;
    std::vector<ca::str::Utf8StringRef> slots_;
    std::unordered_map<std::string_view, ca::u32> byStr_;
};

}   // namespace jbc::ir
