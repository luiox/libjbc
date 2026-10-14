/// @file        bytes.hpp
/// @brief       字节视图、源字节区段与边界检查游标。
///
/// libjbc core 的底座。三条不变量支撑上层的 lazy 与保真：
///   - @ref ByteSpan 只记录「在原字节流中的 offset/length」，不复制内容；
///   - @ref Cursor 的所有读取都返回 Result，越界即 Err，绝不 UB；
///   - 大端定长读取集中在此，codec 层不再手写位移。
#pragma once

#include <cstring>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/core/error.hpp"

namespace jbc::core {

// 本层所有错误通道都是 ca::core::Result（Ok/Err 工厂）。显式引入而非
// `using namespace`，避免把 ca::core 的其余名字带进 jbc::core。
using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// @brief 源字节流中的一段：lazy 与保真的共同载体。
///
/// 只记 offset/length，不复制内容。模型中一切「尚未被修改的实体」都持有
/// 一段 span——常量池条目、属性内容、Code 字节——写回时原样 copy 回去，
/// 因此保真不依赖重编码正确，也不依赖 relocation 猜测。
struct ByteSpan
{
    ca::u32 offset = 0;
    ca::u32 length = 0;

    bool empty() const noexcept { return length == 0; }
};

/// @brief 只读字节视图（借用语义，不拥有数据）。
class ByteView
{
public:
    ByteView() noexcept = default;

    ByteView(const ca::u8* data, ca::usize size) noexcept : data_(data), size_(size) {}

    /// @brief 从连续容器构造；容器须在本视图使用期内保持存活且不变容。
    explicit ByteView(const std::vector<ca::u8>& v) noexcept : data_(v.data()), size_(v.size()) {}

    const ca::u8* data() const noexcept { return data_; }
    ca::usize size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    /// @brief 越界即断言：调用方必须先经 @ref covers 或游标校验。
    const ca::u8& operator[](ca::usize i) const noexcept { return data_[i]; }

    /// @brief [offset, offset+length) 是否完全落在本视图内（lazy span 校验入口）。
    bool covers(ca::usize offset, ca::usize length) const noexcept
    {
        return offset <= size_ && length <= size_ - offset;
    }

    /// @brief 取子视图；越界返回空视图而非断言（供防御式路径使用）。
    ByteView subOrEmpty(ca::usize offset, ca::usize length) const noexcept
    {
        if (!covers(offset, length)) {
            return ByteView{};
        }
        return ByteView{data_ + offset, length};
    }

    /// @brief 按 @ref ByteSpan 取子视图；span 越界返回空视图。
    ByteView spanOrEmpty(const ByteSpan& s) const noexcept
    {
        return subOrEmpty(s.offset, s.length);
    }

    /// @brief 逐字节相等（长度不同先否）。
    bool equals(const ByteView& other) const noexcept
    {
        return size_ == other.size_ && (size_ == 0 || std::memcmp(data_, other.data_, size_) == 0);
    }

    /// @brief 以 std::string_view 观察字节（零分配；不保证以 \0 终止）。
    ///
    /// 此处刻意用 string_view 而非 Utf8StringRef：classfile 的 UTF8 条目在被
    /// 解码前只是字节，属性名/描述符的比较走在 @ref ConstantPool::utf8Equals
    /// 的 memcmp 热路径上，构造 Utf8StringRef 需要 O(n) 计码点，属于无谓开销。
    /// 需要文本语义时由调用方显式转 Utf8StringRef。
    std::string_view asStringView() const noexcept
    {
        return data_ ? std::string_view(reinterpret_cast<const char*>(data_), size_)
                     : std::string_view{};
    }

private:
    const ca::u8* data_ = nullptr;
    ca::usize size_    = 0;
};

/// @brief 边界检查游标：classfile 顺序解析的唯一读取入口。
///
/// 每次读取前校验剩余长度，不足即返回 Err(ReadError::Truncated)，绝不越界读。
/// 解析器的每一行都是「取 n 字节 → 前进 n」，不手工维护裸指针。
class Cursor
{
public:
    explicit Cursor(ByteView view) noexcept : view_(view) {}

    ByteView view() const noexcept { return view_; }
    ca::usize pos() const noexcept { return pos_; }
    ca::usize remaining() const noexcept { return view_.size() - pos_; }
    bool eof() const noexcept { return pos_ >= view_.size(); }

    /// @brief 大端读取定长无符号量；剩余不足即 Err(Truncated)。
    Result<ca::u8, ReadError> u8() noexcept
    {
        if (remaining() < 1) {
            return Err(ReadError::Truncated);
        }
        return Ok(view_[pos_++]);
    }

    Result<ca::u16, ReadError> u16() noexcept
    {
        if (remaining() < 2) {
            return Err(ReadError::Truncated);
        }
        const ca::u16 v = static_cast<ca::u16>((static_cast<ca::u16>(view_[pos_]) << 8) |
                                              static_cast<ca::u16>(view_[pos_ + 1]));
        pos_ += 2;
        return Ok(v);
    }

    Result<ca::u32, ReadError> u32() noexcept
    {
        if (remaining() < 4) {
            return Err(ReadError::Truncated);
        }
        const ca::u32 v = (static_cast<ca::u32>(view_[pos_]) << 24) |
                          (static_cast<ca::u32>(view_[pos_ + 1]) << 16) |
                          (static_cast<ca::u32>(view_[pos_ + 2]) << 8) |
                          static_cast<ca::u32>(view_[pos_ + 3]);
        pos_ += 4;
        return Ok(v);
    }

    /// @brief 取 n 字节视图并前进；不足即 Err(Truncated)。
    Result<ByteView, ReadError> take(ca::usize n) noexcept
    {
        if (remaining() < n) {
            return Err(ReadError::Truncated);
        }
        const ByteView v{view_.data() + pos_, n};
        pos_ += n;
        return Ok(v);
    }

    /// @brief 跳过 n 字节（lazy 属性的全部实现：跳过而不解析）。
    Result<void, ReadError> skip(ca::usize n) noexcept
    {
        if (remaining() < n) {
            return Err(ReadError::Truncated);
        }
        pos_ += n;
        return Ok();
    }

    /// @brief 绝对定位；越界即 Err(Truncated) 且不改变游标。
    Result<void, ReadError> seek(ca::usize pos) noexcept
    {
        if (pos > view_.size()) {
            return Err(ReadError::Truncated);
        }
        pos_ = pos;
        return Ok();
    }

    /// @brief 不移动游标地取 [offset, offset+length)；越界返回 Err。
    Result<ByteView, ReadError> peekAt(ca::usize offset, ca::usize length) const noexcept
    {
        if (!view_.covers(offset, length)) {
            return Err(ReadError::Truncated);
        }
        return Ok(ByteView{view_.data() + offset, length});
    }

private:
    ByteView view_;
    ca::usize pos_ = 0;
};

/// @brief 字节输出缓冲：writer 的唯一写出入口（大端定长）。
class ByteSink
{
public:
    void u8(ca::u8 v) { bytes_.push_back(v); }

    void u16(ca::u16 v)
    {
        bytes_.push_back(static_cast<ca::u8>(v >> 8));
        bytes_.push_back(static_cast<ca::u8>(v));
    }

    void u32(ca::u32 v)
    {
        bytes_.push_back(static_cast<ca::u8>(v >> 24));
        bytes_.push_back(static_cast<ca::u8>(v >> 16));
        bytes_.push_back(static_cast<ca::u8>(v >> 8));
        bytes_.push_back(static_cast<ca::u8>(v));
    }

    /// @brief 原样追加一段字节（copy-through：保真写回的核心动作）。
    void raw(ByteView v)
    {
        if (v.empty()) {
            return;
        }
        bytes_.insert(bytes_.end(), v.data(), v.data() + v.size());
    }

    std::vector<ca::u8>& bytes() noexcept { return bytes_; }
    const std::vector<ca::u8>& bytes() const noexcept { return bytes_; }
    ca::usize size() const noexcept { return bytes_.size(); }

private:
    std::vector<ca::u8> bytes_;
};

}   // namespace jbc::core
