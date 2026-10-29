/// @file        writer.cpp
/// @brief       classfile 保真写回。
///
/// 写回顺序照 JVMS 4.1，与 reader 完全对称。保真由两处 copy-through 保证：
/// 常量池未脏条目 copy @ref ConstantPool::Entry::raw，未修改属性 copy
/// @ref AttrSlot::data——两者都不经过任何编码器。
#include "jbc/core/writer.hpp"

#include "jbc/core/attr_codec.hpp"

#include <limits>

namespace jbc::core {

namespace {

constexpr ca::u32 kMagic = 0xCAFEBABEu;

/// 属性槽写出是否需要源字节：copy-through 路径（本槽或任一嵌套槽走 copy）
/// 都要源 span 在场。槽未置脏 → copy 本槽 span；槽重编码但其 Code 体里有
/// 未触碰的嵌套槽 → 嵌套 copy 各自 span。全 New 的合成槽树返回 false。
bool slotNeedsSource(const AttrSlot& s) noexcept
{
    if (!s.needsReencode()) {
        return true;
    }
    const AttrBody* body = s.peek();
    if (body == nullptr || body->kind() != AttrKind::Code) {
        return false;
    }
    for (const AttrSlot& nested : static_cast<const CodeAttr&>(*body).attrs) {
        if (slotNeedsSource(nested)) {
            return true;
        }
    }
    return false;
}

/// 写出是否需要源字节：任一走 copy-through 路径的 CP 条目/属性槽都需要。
///
/// 全合成模型（从零建类：CP 条目全部经 add* 追加即脏、属性槽全部 New）
/// 不含任何 copy 路径，无源字节也可写出——MissingSource 只挡「需要
/// copy-through 而源缺席」的真违约，不再挡从零构建的合法形态。
bool writeNeedsSource(const ClassFile& cf) noexcept
{
    for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
        if (!cf.cp.valid(i)) {
            continue;   // wide 续槽随主槽编码
        }
        if (!cf.cp.at(i).dirty) {
            return true;
        }
    }
    const auto memberNeedsSource = [](const MemberInfo& m) {
        for (const AttrSlot& s : m.attrs) {
            if (slotNeedsSource(s)) {
                return true;
            }
        }
        return false;
    };
    for (const MemberInfo& m : cf.fields) {
        if (memberNeedsSource(m)) {
            return true;
        }
    }
    for (const MemberInfo& m : cf.methods) {
        if (memberNeedsSource(m)) {
            return true;
        }
    }
    for (const AttrSlot& s : cf.attrs) {
        if (slotNeedsSource(s)) {
            return true;
        }
    }
    return false;
}

bool validCpTag(const ClassFile& cf, ca::u32 index, CpTag tag)
{
    return cf.cp.valid(index) && cf.cp.tagOf(index) == tag;
}

bool validClassReferences(const ClassFile& cf)
{
    if (!validCpTag(cf, cf.thisClass, CpTag::Class) ||
        (cf.superClass != 0 && !validCpTag(cf, cf.superClass, CpTag::Class))) {
        return false;
    }
    for (ca::u16 index : cf.interfaces) {
        if (!validCpTag(cf, index, CpTag::Class)) {
            return false;
        }
    }
    const auto validMembers = [&cf](const std::vector<MemberInfo>& members) {
        for (const MemberInfo& m : members) {
            if (!cf.cp.isUtf8(m.nameCp) || !cf.cp.isUtf8(m.descCp)) {
                return false;
            }
            for (const AttrSlot& attr : m.attrs) {
                if (!cf.cp.isUtf8(attr.nameCp)) {
                    return false;
                }
            }
        }
        return true;
    };
    if (!validMembers(cf.fields) || !validMembers(cf.methods)) {
        return false;
    }
    for (const AttrSlot& attr : cf.attrs) {
        if (!cf.cp.isUtf8(attr.nameCp)) {
            return false;
        }
    }
    return true;
}

Result<void, WriteError> writeMember(const ClassFile& cf,
                                     const MemberInfo& m,
                                     ByteSink& out,
                                     ByteView source)
{
    if (m.nameCp > std::numeric_limits<ca::u16>::max() ||
        m.descCp > std::numeric_limits<ca::u16>::max() ||
        m.attrs.size() > std::numeric_limits<ca::u16>::max()) {
        return Err(WriteError::Unencodable);
    }
    out.u16(m.access);
    out.u16(static_cast<ca::u16>(m.nameCp));
    out.u16(static_cast<ca::u16>(m.descCp));
    out.u16(static_cast<ca::u16>(m.attrs.size()));
    for (const AttrSlot& s : m.attrs) {
        const Result<void, WriteError> r = encodeAttr(cf, s, out, source);
        if (r.is_err()) {
            return Err(r.unwrap_err());
        }
    }
    return Ok();
}

}   // namespace

Result<std::vector<ca::u8>, WriteError> writeClass(const ClassFile& cf)
{
    const ByteView source = cf.source();
    // 源字节缺席只在「确实要走 copy-through」时才是错误：全合成模型每个
    // 条目都在重编码态，照常写出（从零建类的写出路径）。
    if (!cf.hasSource() && writeNeedsSource(cf)) {
        return Err(WriteError::MissingSource);
    }
    if (cf.interfaces.size() > std::numeric_limits<ca::u16>::max() ||
        cf.fields.size() > std::numeric_limits<ca::u16>::max() ||
        cf.methods.size() > std::numeric_limits<ca::u16>::max() ||
        cf.attrs.size() > std::numeric_limits<ca::u16>::max()) {
        return Err(WriteError::Unencodable);
    }
    if (!validClassReferences(cf)) {
        return Err(WriteError::Unencodable);
    }

    ByteSink out;

    out.u32(kMagic);
    out.u16(cf.minor);
    out.u16(cf.major);

    const Result<void, WriteError> rcp = cf.cp.encode(out, source);
    if (rcp.is_err()) {
        return Err(rcp.unwrap_err());
    }

    out.u16(cf.access);
    out.u16(cf.thisClass);
    out.u16(cf.superClass);
    out.u16(static_cast<ca::u16>(cf.interfaces.size()));
    for (const ca::u16 i : cf.interfaces) {
        out.u16(i);
    }

    out.u16(static_cast<ca::u16>(cf.fields.size()));
    for (const MemberInfo& m : cf.fields) {
        const Result<void, WriteError> r = writeMember(cf, m, out, source);
        if (r.is_err()) {
            return Err(r.unwrap_err());
        }
    }

    out.u16(static_cast<ca::u16>(cf.methods.size()));
    for (const MemberInfo& m : cf.methods) {
        const Result<void, WriteError> r = writeMember(cf, m, out, source);
        if (r.is_err()) {
            return Err(r.unwrap_err());
        }
    }

    out.u16(static_cast<ca::u16>(cf.attrs.size()));
    for (const AttrSlot& s : cf.attrs) {
        const Result<void, WriteError> r = encodeAttr(cf, s, out, source);
        if (r.is_err()) {
            return Err(r.unwrap_err());
        }
    }

    // 类级 trailing：畸形输入的尾部字节原样写回，
    // 与属性体 trailing 同族——copy-through 保真，不丢尾。
    if (!cf.trailing.empty()) {
        out.raw(ByteView{cf.trailing});
    }

    return Ok(std::move(out.bytes()));
}

}   // namespace jbc::core
