/// @file        sem_index.hpp
/// @brief       语义 intern 的去重索引（ClassSession 内部实现细节）。
///
/// 独立成内部头只有一个原因：ClassSession 的构造/析构在 units.cpp，而
/// SemIndex 定义在 operand.cpp——unique_ptr 成员的异常清理路径需要完整
/// 类型，两个编译单元都得看到同一个定义。
#pragma once

#include <string_view>
#include <unordered_map>

#include "libca/core/datatype.hpp"

#include "jbc/core/constant_pool.hpp"

namespace jbc::ir {

namespace {

/// CP 索引是 u2 域（≤ 0xFFFF），三元素键 (tag, a, b) 打包进 u64 无损。
constexpr ca::u64 packRefKey(core::CpTag tag, ca::u32 a, ca::u32 b) noexcept
{
    return (static_cast<ca::u64>(static_cast<ca::u16>(tag)) << 32) |
           (static_cast<ca::u64>(a) << 16) | static_cast<ca::u64>(b);
}
constexpr ca::u64 packNatKey(ca::u32 nameCp, ca::u32 descCp) noexcept
{
    return (static_cast<ca::u64>(nameCp) << 16) | static_cast<ca::u64>(descCp);
}

}   // namespace

/// 语义去重索引：对既有池一次扫描建表，intern 的每次追加同步登记。
/// 重复内容命中既有索引（不膨胀池）；miss 才走 CP 只追加。
class SemIndex
{
public:
    explicit SemIndex(const core::ConstantPool& cp)
    {
        for (ca::u32 i = 1; i < cp.size(); ++i) {
            if (!cp.valid(i)) {
                continue;   // Long/Double 续槽
            }
            const core::ConstantPool::Entry& e = cp.at(i);
            switch (e.tag) {
            case core::CpTag::Utf8:
                utf8.emplace(
                    std::string_view(reinterpret_cast<const char*>(e.utf8), e.utf8Len), i);
                break;
            case core::CpTag::Class:
                byName.emplace(e.a, i);
                break;
            case core::CpTag::NameAndType:
                nat.emplace(packNatKey(e.a, e.b), i);
                break;
            case core::CpTag::String:
                strConst.emplace(e.a, i);
                break;
            case core::CpTag::Integer:
                intConst.emplace(e.ival, i);
                break;
            case core::CpTag::Fieldref:
            case core::CpTag::Methodref:
            case core::CpTag::InterfaceMethodref:
            case core::CpTag::InvokeDynamic:
                ref.emplace(packRefKey(e.tag, e.a, e.b), i);
                break;
            case core::CpTag::Float:
                // Float 位按 u32 存 ival（addFloatBits 的存储形态）。
                floatConst.emplace(static_cast<ca::u32>(e.ival), i);
                break;
            case core::CpTag::Long:
                longConst.emplace(e.lval, i);
                break;
            case core::CpTag::Double:
                // Double 位经 i64 存储（appendWide 的存储形态）。
                doubleConst.emplace(static_cast<ca::u64>(e.lval), i);
                break;
            default:
                break;
            }
        }
    }

    std::unordered_map<std::string_view, ca::u32> utf8;      // 内容 → Utf8 索引
    std::unordered_map<ca::u32, ca::u32> byName;             // nameIdx → Class 索引
    std::unordered_map<ca::u64, ca::u32> nat;                // (name,desc) → NaT 索引
    std::unordered_map<ca::u32, ca::u32> strConst;           // utf8Idx → String 索引
    std::unordered_map<ca::i32, ca::u32> intConst;           // 值 → Integer 索引
    std::unordered_map<ca::u64, ca::u32> ref;                // (tag,a,b) → ref 索引
    // 注解族合成基座（attr_build 挂载面）：常量去重三件。
    std::unordered_map<ca::u32, ca::u32> floatConst;         // 位模式 → Float 索引
    std::unordered_map<ca::i64, ca::u32> longConst;          // 值 → Long 索引
    std::unordered_map<ca::u64, ca::u32> doubleConst;        // 位模式 → Double 索引
};

}   // namespace jbc::ir
