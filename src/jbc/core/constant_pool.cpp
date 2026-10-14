/// @file        constant_pool.cpp
/// @brief       常量池解析与写回。
///
/// 解析期不复制 UTF8 字节、不校验 UTF-8 合法性：classfile 的 UTF8 是 modified
/// UTF-8，而本层只负责「忠实表达字节」。合法性由需要文本语义的上层决定。
#include "jbc/core/constant_pool.hpp"

#include <cstring>
#include <limits>

namespace jbc::core {

bool cpTagIsWide(CpTag tag) noexcept
{
    return tag == CpTag::Long || tag == CpTag::Double;
}

namespace {

bool fitsU16(ca::u32 v) noexcept
{
    return v <= static_cast<ca::u32>(std::numeric_limits<ca::u16>::max());
}

bool validRef(const ConstantPool& cp, ca::u32 index, CpTag tag) noexcept
{
    return cp.valid(index) && cp.tagOf(index) == tag;
}

bool validMethodHandleRef(const ConstantPool& cp, ca::u32 kind, ca::u32 index) noexcept
{
    if (kind < 1 || kind > 9 || !cp.valid(index)) {
        return false;
    }
    const CpTag tag = cp.tagOf(index);
    if (kind >= 1 && kind <= 4) {
        return tag == CpTag::Fieldref;
    }
    if (kind == 9) {
        return tag == CpTag::InterfaceMethodref;
    }
    return tag == CpTag::Methodref || tag == CpTag::InterfaceMethodref;
}

/// @brief 单条目编码：只写内容，不写 count（供整池编码与脏条目重编码共用）。
void encodeEntry(ByteSink& out, const ConstantPool::Entry& e)
{
    out.u8(static_cast<ca::u8>(e.tag));
    switch (e.tag) {
        case CpTag::Utf8:
            out.u16(static_cast<ca::u16>(e.utf8Len));
            if (e.utf8Len > 0 && e.utf8 != nullptr) {
                out.raw(ByteView{e.utf8, e.utf8Len});
            }
            break;
        case CpTag::Integer:
        case CpTag::Float:
            out.u32(static_cast<ca::u32>(e.ival));
            break;
        case CpTag::Long:
        case CpTag::Double:
            out.u32(static_cast<ca::u32>(static_cast<ca::u64>(e.lval) >> 32));
            out.u32(static_cast<ca::u32>(static_cast<ca::u64>(e.lval) & 0xFFFFFFFFu));
            break;
        case CpTag::MethodHandle:
            out.u8(static_cast<ca::u8>(e.a));
            out.u16(static_cast<ca::u16>(e.b));
            break;
        case CpTag::Class:
        case CpTag::String:
        case CpTag::MethodType:
        case CpTag::Module:
        case CpTag::Package:
            out.u16(static_cast<ca::u16>(e.a));
            break;
        case CpTag::Fieldref:
        case CpTag::Methodref:
        case CpTag::InterfaceMethodref:
        case CpTag::NameAndType:
        case CpTag::Dynamic:
        case CpTag::InvokeDynamic:
            out.u16(static_cast<ca::u16>(e.a));
            out.u16(static_cast<ca::u16>(e.b));
            break;
    }
}

/// @brief 按 tag 取该条目在 classfile 中的定长内容字节数（Utf8 为变长，返回 -1）。
int fixedContentSize(CpTag tag) noexcept
{
    switch (tag) {
        case CpTag::Integer:
        case CpTag::Float:
            return 4;
        case CpTag::Long:
        case CpTag::Double:
            return 8;
        case CpTag::MethodHandle:
            return 3;
        case CpTag::Class:
        case CpTag::String:
        case CpTag::MethodType:
        case CpTag::Module:
        case CpTag::Package:
            return 2;
        case CpTag::Fieldref:
        case CpTag::Methodref:
        case CpTag::InterfaceMethodref:
        case CpTag::NameAndType:
        case CpTag::Dynamic:
        case CpTag::InvokeDynamic:
            return 4;
        default:
            return -1;
    }
}

bool isValidTag(ca::u8 t) noexcept
{
    switch (t) {
        case 1: case 3: case 4: case 5: case 6: case 7: case 8: case 9:
        case 10: case 11: case 12: case 15: case 16: case 17: case 18:
        case 19: case 20:
            return true;
        default:
            return false;
    }
}

}   // namespace

Result<void, ReadError> ConstantPool::parse(Cursor& cur, Diagnostics& diag)
{
    const Result<ca::u16, ReadError> rc = cur.u16();
    if (rc.is_err()) {
        return Err(rc.unwrap_err());
    }
    const ca::u16 count = rc.unwrap();
    if (count < 1) {
        return Err(ReadError::BadConstantPoolCount);
    }

    entries_.clear();
    entries_.reserve(count);
    entries_.emplace_back();  // 0 号占位

    for (ca::u32 i = 1; i < static_cast<ca::u32>(count); ++i) {
        const ca::usize entryStart = cur.pos();

        const Result<ca::u8, ReadError> rt = cur.u8();
        if (rt.is_err()) {
            return Err(rt.unwrap_err());
        }
        const ca::u8 rawTag = rt.unwrap();
        if (!isValidTag(rawTag)) {
            diag.error(ReadError::BadConstantPoolTag, static_cast<ca::u32>(entryStart));
            return Err(ReadError::BadConstantPoolTag);
        }

        const CpTag tag = static_cast<CpTag>(rawTag);
        Entry e;
        e.tag = tag;
        e.raw = ByteSpan{static_cast<ca::u32>(entryStart), 0};

        if (tag == CpTag::Utf8) {
            const Result<ca::u16, ReadError> rl = cur.u16();
            if (rl.is_err()) {
                return Err(rl.unwrap_err());
            }
            const ca::u16 len = rl.unwrap();
            const Result<ByteView, ReadError> rb = cur.take(len);
            if (rb.is_err()) {
                return Err(rb.unwrap_err());
            }
            const ByteView b = rb.unwrap();
            e.utf8    = b.data();
            e.utf8Len = static_cast<ca::u32>(b.size());
        }
        else if (const int fixed = fixedContentSize(tag); fixed > 0) {
            // 定长条目：一次性取走内容，再按 tag 解释字段。
            const Result<ByteView, ReadError> rb =
                cur.take(static_cast<ca::usize>(fixed));
            if (rb.is_err()) {
                return Err(rb.unwrap_err());
            }
            const ByteView b = rb.unwrap();
            const ca::u8* p = b.data();
            switch (tag) {
                case CpTag::Integer:
                case CpTag::Float:
                    e.ival = static_cast<ca::i32>(
                        (static_cast<ca::u32>(p[0]) << 24) |
                        (static_cast<ca::u32>(p[1]) << 16) |
                        (static_cast<ca::u32>(p[2]) << 8) |
                        static_cast<ca::u32>(p[3]));
                    break;
                case CpTag::Long:
                case CpTag::Double: {
                    ca::u64 v = 0;
                    for (int k = 0; k < 8; ++k) {
                        v = (v << 8) | static_cast<ca::u64>(p[k]);
                    }
                    e.lval = static_cast<ca::i64>(v);
                    break;
                }
                case CpTag::MethodHandle:
                    e.a = p[0];
                    e.b = (static_cast<ca::u32>(p[1]) << 8) | static_cast<ca::u32>(p[2]);
                    break;
                case CpTag::Class:
                case CpTag::String:
                case CpTag::MethodType:
                case CpTag::Module:
                case CpTag::Package:
                    e.a = (static_cast<ca::u32>(p[0]) << 8) | static_cast<ca::u32>(p[1]);
                    break;
                default:  // 两操作数条目
                    e.a = (static_cast<ca::u32>(p[0]) << 8) | static_cast<ca::u32>(p[1]);
                    e.b = (static_cast<ca::u32>(p[2]) << 8) | static_cast<ca::u32>(p[3]);
                    break;
            }
        }
        else {
            diag.error(ReadError::BadConstantPoolTag, static_cast<ca::u32>(entryStart));
            return Err(ReadError::BadConstantPoolTag);
        }

        e.raw.length = static_cast<ca::u32>(cur.pos() - entryStart);
        entries_.push_back(e);

        // Long/Double 占两个槽位：补一个不可用的占位槽。
        if (cpTagIsWide(tag)) {
            Entry ph;
            ph.placeholder = true;
            ph.raw         = e.raw;
            entries_.push_back(ph);
            ++i;
            if (i >= static_cast<ca::u32>(count)) {
                // 宽条目的第二个槽位越出 constant_pool_count：CP 结构损坏——
                // 后续条目边界无法确定，且宽容写回会使槽位比输入多 1（既不
                // 保真也不报错）。与其他 CP 结构错误一致，reader 阶段即 Err
                // （design.md §27.3），不走降级通道。
                diag.error(ReadError::BadConstantPoolCount, static_cast<ca::u32>(entryStart));
                return Err(ReadError::BadConstantPoolCount);
            }
        }
    }

    restorePoint_ = static_cast<ca::u32>(entries_.size());
    return Ok();
}

bool ConstantPool::valid(ca::u32 i) const noexcept
{
    return i > 0 && i < entries_.size() && !entries_[i].placeholder;
}

const ConstantPool::Entry& ConstantPool::at(ca::u32 i) const noexcept
{
    static thread_local const Entry invalid{};
    return valid(i) ? entries_[i] : invalid;
}

ConstantPool::Entry& ConstantPool::at(ca::u32 i) noexcept
{
    static thread_local Entry invalid{};
    return valid(i) ? entries_[i] : invalid;
}

ConstantPool::Entry& ConstantPool::mutateAt(ca::u32 i) noexcept
{
    Entry& e = at(i);
    e.dirty = true;
    return e;
}

CpTag ConstantPool::tagOf(ca::u32 i) const noexcept
{
    return valid(i) ? entries_[i].tag : CpTag::Utf8;
}

Result<void, ReadError> ConstantPool::validateReferences(Diagnostics& diag) const
{
    for (ca::u32 i = 1; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (e.placeholder) {
            continue;
        }
        bool ok = true;
        switch (e.tag) {
            case CpTag::Class:
            case CpTag::Module:
            case CpTag::Package:
            case CpTag::String:
            case CpTag::MethodType:
                ok = validRef(*this, e.a, CpTag::Utf8);
                break;
            case CpTag::NameAndType:
                ok = validRef(*this, e.a, CpTag::Utf8) &&
                     validRef(*this, e.b, CpTag::Utf8);
                break;
            case CpTag::Fieldref:
            case CpTag::Methodref:
            case CpTag::InterfaceMethodref:
                ok = validRef(*this, e.a, CpTag::Class) &&
                     validRef(*this, e.b, CpTag::NameAndType);
                break;
            case CpTag::MethodHandle:
                ok = validMethodHandleRef(*this, e.a, e.b);
                break;
            case CpTag::Dynamic:
            case CpTag::InvokeDynamic:
                ok = fitsU16(e.a) && validRef(*this, e.b, CpTag::NameAndType);
                break;
            case CpTag::Utf8:
            case CpTag::Integer:
            case CpTag::Float:
            case CpTag::Long:
            case CpTag::Double:
                break;
        }
        if (!ok) {
            diag.error(ReadError::BadConstantPoolRef, e.raw.offset);
            return Err(ReadError::BadConstantPoolRef);
        }
    }
    return Ok();
}

std::string_view ConstantPool::utf8Of(ca::u32 i) const noexcept
{
    if (!isUtf8(i)) {
        return std::string_view{};
    }
    const Entry& e = entries_[i];
    return e.utf8Len == 0 ? std::string_view{}
                          : std::string_view{reinterpret_cast<const char*>(e.utf8), e.utf8Len};
}

bool ConstantPool::utf8Equals(ca::u32 i, std::string_view lit) const noexcept
{
    if (!isUtf8(i)) {
        return false;
    }
    const Entry& e = entries_[i];
    if (e.utf8Len != lit.size()) {
        return false;
    }
    return e.utf8Len == 0 || std::memcmp(e.utf8, lit.data(), e.utf8Len) == 0;
}

bool ConstantPool::utf8StartsWith(ca::u32 i, std::string_view prefix) const noexcept
{
    if (!isUtf8(i)) {
        return false;
    }
    const Entry& e = entries_[i];
    if (e.utf8Len < prefix.size()) {
        return false;
    }
    return prefix.empty() || std::memcmp(e.utf8, prefix.data(), prefix.size()) == 0;
}

ca::u32 ConstantPool::findUtf8(std::string_view s) const noexcept
{
    for (ca::u32 i = 1; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (e.tag == CpTag::Utf8 && e.utf8Len == s.size()
            && (s.empty() || std::memcmp(e.utf8, s.data(), s.size()) == 0)) {
            return i;
        }
    }
    return 0;
}

ca::str::Utf8StringRef ConstantPool::utf8Ref(ca::u32 i) const noexcept
{
    if (!isUtf8(i)) {
        return ca::str::Utf8StringRef{};
    }
    const Entry& e = entries_[i];
    if (e.utf8 == nullptr || e.utf8Len == 0) {
        return ca::str::Utf8StringRef{};
    }
    // 码点数取字节长度上界（modified UTF-8 下这是保守值，不影响字节语义）。
    return ca::str::Utf8StringRef{e.utf8, e.utf8Len, e.utf8Len};
}

bool ConstantPool::arenaOwnsAllAppended() const noexcept
{
    for (const Entry& e : entries_) {
        if (!e.utf8InArena) {
            continue;
        }
        const ca::str::Utf8StringRef ref{e.utf8, e.utf8Len, e.utf8Len};
        if (!arena_.owns(ref)) {
            return false;
        }
    }
    return true;
}

ca::u32 ConstantPool::addUtf8(std::string_view s)
{
    Entry e;
    e.tag   = CpTag::Utf8;
    e.dirty = true;

    if (!s.empty()) {
        // modified UTF-8 不保证是合法 UTF-8（混淆常往里塞任意字节），走
        // intern_raw 跳过校验——它与 JVM 的编解码 1:1 往返，字节不会失真。
        const ca::str::Utf8StringRef ref =
            arena_.intern_raw(reinterpret_cast<const ca::u8*>(s.data()), s.size());
        e.utf8        = ref.data();
        e.utf8Len     = static_cast<ca::u32>(ref.byte_length());
        e.utf8InArena = true;
    }

    entries_.push_back(e);
    return static_cast<ca::u32>(entries_.size() - 1);
}

ca::u32 ConstantPool::addClass(ca::u32 nameCp)
{
    Entry e;
    e.tag   = CpTag::Class;
    e.a     = nameCp;
    e.dirty = true;
    entries_.push_back(e);
    return static_cast<ca::u32>(entries_.size() - 1);
}

ca::u32 ConstantPool::addNameAndType(ca::u32 nameCp, ca::u32 descCp)
{
    Entry e;
    e.tag   = CpTag::NameAndType;
    e.a     = nameCp;
    e.b     = descCp;
    e.dirty = true;
    entries_.push_back(e);
    return static_cast<ca::u32>(entries_.size() - 1);
}

ca::u32 ConstantPool::addString(ca::u32 utf8Cp)
{
    Entry e;
    e.tag   = CpTag::String;
    e.a     = utf8Cp;
    e.dirty = true;
    entries_.push_back(e);
    return static_cast<ca::u32>(entries_.size() - 1);
}

ca::u32 ConstantPool::addInteger(ca::i32 v)
{
    Entry e;
    e.tag   = CpTag::Integer;
    e.ival  = v;
    e.dirty = true;
    entries_.push_back(e);
    return static_cast<ca::u32>(entries_.size() - 1);
}

ca::u32 ConstantPool::addFloatBits(ca::u32 bits)
{
    Entry e;
    e.tag   = CpTag::Float;
    e.ival  = static_cast<ca::i32>(bits);
    e.dirty = true;
    entries_.push_back(e);
    return static_cast<ca::u32>(entries_.size() - 1);
}

ca::u32 ConstantPool::addLong(ca::i64 v)
{
    return appendWide(CpTag::Long, v);
}

ca::u32 ConstantPool::addDoubleBits(ca::u64 bits)
{
    return appendWide(CpTag::Double, static_cast<ca::i64>(bits));
}

ca::u32 ConstantPool::addRef(CpTag tag, ca::u32 classCp, ca::u32 nameAndTypeCp)
{
    Entry e;
    e.tag   = tag;
    e.a     = classCp;
    e.b     = nameAndTypeCp;
    e.dirty = true;
    entries_.push_back(e);
    return static_cast<ca::u32>(entries_.size() - 1);
}

ca::u32 ConstantPool::appendWide(CpTag tag, ca::i64 lval)
{
    Entry e;
    e.tag   = tag;
    e.lval  = lval;
    e.dirty = true;
    entries_.push_back(e);

    Entry ph;
    ph.placeholder = true;
    entries_.push_back(ph);
    return static_cast<ca::u32>(entries_.size() - 2);
}

Result<void, WriteError> ConstantPool::encode(ByteSink& out, ByteView source) const
{
    Diagnostics diagnostics;
    if (validateReferences(diagnostics).is_err()) {
        return Err(WriteError::Unencodable);
    }
    if (entries_.size() > static_cast<ca::usize>(std::numeric_limits<ca::u16>::max())) {
        return Err(WriteError::Unencodable);
    }
    out.u16(static_cast<ca::u16>(entries_.size()));

    for (ca::u32 i = 1; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (e.placeholder) {
            continue;  // 续槽不单独编码，它的字节随主槽一起写出
        }

        if (!e.dirty) {
            const ByteView v = source.spanOrEmpty(e.raw);
            if (v.size() != e.raw.length) {
                return Err(WriteError::BadSpan);
            }
            out.raw(v);  // copy-through：保真的核心动作
        }
        else {
            if (e.tag == CpTag::Utf8 && e.utf8Len > std::numeric_limits<ca::u16>::max()) {
                return Err(WriteError::Unencodable);
            }
            const bool singleIndexTag =
                e.tag == CpTag::Class || e.tag == CpTag::String ||
                e.tag == CpTag::MethodType || e.tag == CpTag::Module ||
                e.tag == CpTag::Package;
            const bool doubleIndexTag =
                e.tag == CpTag::Fieldref || e.tag == CpTag::Methodref ||
                e.tag == CpTag::InterfaceMethodref || e.tag == CpTag::NameAndType ||
                e.tag == CpTag::Dynamic || e.tag == CpTag::InvokeDynamic;
            if ((e.tag == CpTag::MethodHandle &&
                 (e.a > std::numeric_limits<ca::u8>::max() ||
                  e.b > std::numeric_limits<ca::u16>::max())) ||
                (singleIndexTag && !fitsU16(e.a)) ||
                (doubleIndexTag && (!fitsU16(e.a) || !fitsU16(e.b)))) {
                return Err(WriteError::Unencodable);
            }
            encodeEntry(out, e);
        }

        if (cpTagIsWide(e.tag)) {
            ++i;
        }
    }
    return Ok();
}

}   // namespace jbc::core
