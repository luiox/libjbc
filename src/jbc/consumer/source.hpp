/// @file        source.hpp
/// @brief       E3 源协议：class 字节供给面 + 内存实现（design.md §9）。
///
/// design.md §9 把关系定死：库内不持有 Jar/资源容器细节——含 Jar/目录/
/// 复合资源的供给由上层（ResourceContainer 投影）提供，本面只定一个契约
/// + 一个内存实现（stage 产物、单批装料、闭包收合的通用态）：
///
///   - `bytes(内部名)` 缺失显式 Err(NotFound)（fail-closed）——不默空 使
///     上层能分档（缺失与半截字节是两类事实）；
///   - `listClasses` 枚举全部体内名（追加序为升序，由实现保证）；
///   - 视图语义：返回的 ByteView 系实现登录的存续字节，源存活期内有效
///     （storage 由源持有；外层编辑运行内即可读）。
///
/// mjbase 消费侧按此契约投影成 KeyPool-id 形态（mjbase-consumer 同名
/// 面）；libjbc 本体保持文本内部名 + 无池的裸 Achilles 态。
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/core/bytes.hpp"
#include "jbc/consumer/consumer.hpp"

namespace jbc::consumer {

/// @brief consumer 模块 Result 别名。
template <typename T>
using ConsumerResult = ca::core::Result<T, ConsumerError>;

/// @brief class 字节供给协议（E3 源面）。
///
/// 实现需保证：类字节从 `bytes()` 到 @ref listClasses 变化序列内稳定；
/// 修改后视图不续命（调用方重新取）。
class IClassSource
{
public:
    virtual ~IClassSource() = default;

    /// @brief 按内部名取 class 字节；缺失显式 Err(NotFound)（fail-closed）。
    virtual ConsumerResult<jbc::core::ByteView> bytes(std::string_view internalName) const = 0;

    /// @brief 已知类数。
    virtual ca::usize classCount() const noexcept = 0;

    /// @brief 枚举全部类内部名（追加到 out；实现保证升序/稳定序）。
    virtual void listClasses(std::vector<std::string_view>& out) const = 0;
};

/// @brief 内存 map 存续源（stage 产物/单批装料/闭包收合通用）。
///
/// `add` 为 upsert 语义：同键覆盖旧字节（旧视图立刻失效）——枚举顺序
/// 为内部名升序（std::map 节点稳定，插入再替换不重排其余键）。
class MemoryClassSource final : public IClassSource
{
public:
    MemoryClassSource() = default;
    MemoryClassSource(const MemoryClassSource&)            = delete;
    MemoryClassSource& operator=(const MemoryClassSource&) = delete;
    MemoryClassSource(MemoryClassSource&&)                 = default;
    MemoryClassSource& operator=(MemoryClassSource&&)      = default;

    /// @brief 新增或覆盖一个类（字节按值接管，源持存）。
    void add(std::string_view internalName, std::vector<ca::u8> bytes);

    ConsumerResult<jbc::core::ByteView> bytes(std::string_view internalName) const override;
    ca::usize classCount() const noexcept override { return classes_.size(); }
    void      listClasses(std::vector<std::string_view>& out) const override;

private:
    /// 内部名 -> 字节。map 节点稳定：add 对新键的插入不会挪动既有键的
    /// string 内容（string_view 分发到真正的键字节）。
    std::map<std::string, std::vector<ca::u8>, std::less<>> classes_;
};

}   // namespace jbc::consumer
