/// @file        reader.cpp
/// @brief       classfile 骨架解析。
///
/// 结构顺序严格照 JVMS 4.1：magic / version / CP / access / this / super /
/// interfaces / fields / methods / attributes。每一步都经 @ref Cursor 边界检查。
#include "jbc/core/reader.hpp"

#include "jbc/core/attr_codec.hpp"

namespace jbc::core {

namespace {

constexpr ca::u32 kMagic = 0xCAFEBABEu;

/// @brief 解析成员表（fields 或 methods），属性内容只建目录。
Result<void, ReadError> readMembers(Cursor& cur, ClassFile& cf, std::vector<MemberInfo>& out)
{
    const Result<ca::u16, ReadError> rc = cur.u16();
    if (rc.is_err()) {
        return Err(rc.unwrap_err());
    }
    const ca::u16 count = rc.unwrap();
    out.reserve(count);

    for (ca::u16 i = 0; i < count; ++i) {
        MemberInfo m;

        const Result<ca::u16, ReadError> ra = cur.u16();
        if (ra.is_err()) {
            return Err(ra.unwrap_err());
        }
        m.access = ra.unwrap();

        const Result<ca::u16, ReadError> rn = cur.u16();
        if (rn.is_err()) {
            return Err(rn.unwrap_err());
        }
        m.nameCp = rn.unwrap();

        const Result<ca::u16, ReadError> rd = cur.u16();
        if (rd.is_err()) {
            return Err(rd.unwrap_err());
        }
        m.descCp = rd.unwrap();

        const Result<void, ReadError> rat = readAttrSlots(cur, m.attrs, cf.diagnostics());
        if (rat.is_err()) {
            return Err(rat.unwrap_err());
        }
        out.push_back(std::move(m));
    }
    return Ok();
}

}   // namespace

Result<ReadResult, ReadError> readClass(ByteView bytes, const ReadOptions& opt)
{
    // 先复制源字节（若要求保留），再基于副本解析。
    //
    // 顺序很关键：解析产出的每一个字节视图——Utf8 条目、属性 span——都指向
    // 游标底下的 buffer。若游标基于调用方的入参，模型的视图就会借用一个自己
    // 管不着的 buffer，调用方一释放即悬垂。基于副本解析，则所有视图都落在
    // 模型内部，模型析构前一律有效。
    std::vector<ca::u8> owned;
    if (opt.preserveSourceBytes && !bytes.empty()) {
        owned.assign(bytes.data(), bytes.data() + bytes.size());
    }
    const ByteView work = opt.preserveSourceBytes ? ByteView{owned} : bytes;

    Cursor cur{work};

    const Result<ca::u32, ReadError> rm = cur.u32();
    if (rm.is_err()) {
        return Err(rm.unwrap_err());
    }
    if (rm.unwrap() != kMagic) {
        return Err(ReadError::BadMagic);
    }

    auto f = std::make_unique<ClassFile>();

    const Result<ca::u16, ReadError> rMinor = cur.u16();
    if (rMinor.is_err()) {
        return Err(rMinor.unwrap_err());
    }
    f->minor = rMinor.unwrap();

    const Result<ca::u16, ReadError> rMajor = cur.u16();
    if (rMajor.is_err()) {
        return Err(rMajor.unwrap_err());
    }
    f->major = rMajor.unwrap();
    if (f->major > opt.maxSupportedMajor) {
        // 版本偏新只降级，不拒绝：多数结构仍可安全读出。
        f->diagnostics().warn(ReadError::UnsupportedVersion, 6);
    }

    const Result<void, ReadError> rcp = f->cp.parse(cur, f->diagnostics());
    if (rcp.is_err()) {
        return Err(rcp.unwrap_err());
    }
    const Result<void, ReadError> rcpRefs = f->cp.validateReferences(f->diagnostics());
    if (rcpRefs.is_err()) {
        return Err(rcpRefs.unwrap_err());
    }

    const Result<ca::u16, ReadError> rAcc = cur.u16();
    if (rAcc.is_err()) {
        return Err(rAcc.unwrap_err());
    }
    f->access = rAcc.unwrap();

    const Result<ca::u16, ReadError> rThis = cur.u16();
    if (rThis.is_err()) {
        return Err(rThis.unwrap_err());
    }
    f->thisClass = rThis.unwrap();

    const Result<ca::u16, ReadError> rSuper = cur.u16();
    if (rSuper.is_err()) {
        return Err(rSuper.unwrap_err());
    }
    f->superClass = rSuper.unwrap();

    const Result<ca::u16, ReadError> rIc = cur.u16();
    if (rIc.is_err()) {
        return Err(rIc.unwrap_err());
    }
    const ca::u16 ifaceCount = rIc.unwrap();
    f->interfaces.reserve(ifaceCount);
    for (ca::u16 i = 0; i < ifaceCount; ++i) {
        const Result<ca::u16, ReadError> ri = cur.u16();
        if (ri.is_err()) {
            return Err(ri.unwrap_err());
        }
        f->interfaces.push_back(ri.unwrap());
    }

    const Result<void, ReadError> rf = readMembers(cur, *f, f->fields);
    if (rf.is_err()) {
        return Err(rf.unwrap_err());
    }

    const Result<void, ReadError> rmth = readMembers(cur, *f, f->methods);
    if (rmth.is_err()) {
        return Err(rmth.unwrap_err());
    }

    const Result<void, ReadError> ra = readAttrSlots(cur, f->attrs, f->diagnostics());
    if (ra.is_err()) {
        return Err(ra.unwrap_err());
    }

    // 类级属性表之后应正好结束。多余字节照 Code 体 trailing 同族处理
    // （design.md §27.1）：保留为副本，写回原样附回——而不是 Err 或静默丢弃，
    // 否则 writeClass 输出缺尾，「未修改字节级往返」对这类输入不成立。
    // trailing 是「结构可确定、语义不合规」的降级形态，读取产物标记
    // Recovered（+warning）——降级必须可查询，不能只留在诊断流里。
    bool hasTrailing = false;
    if (cur.remaining() != 0) {
        const ca::u32 tailOffset = static_cast<ca::u32>(cur.pos());
        const Result<ByteView, ReadError> rt = cur.take(cur.remaining());
        if (rt.is_err()) {
            return Err(rt.unwrap_err());
        }
        const ByteView tail = rt.unwrap();
        f->trailing.assign(tail.data(), tail.data() + tail.size());
        f->diagnostics().warn(ReadError::UnsupportedFeature, tailOffset);
        hasTrailing = true;
    }

    auto validTag = [&f](ca::u32 index, CpTag tag) {
        return f->cp.valid(index) && f->cp.tagOf(index) == tag;
    };
    if (!validTag(f->thisClass, CpTag::Class) ||
        (f->superClass != 0 && !validTag(f->superClass, CpTag::Class))) {
        f->diagnostics().error(ReadError::BadConstantPoolRef, 0);
        return Err(ReadError::BadConstantPoolRef);
    }
    for (ca::u16 index : f->interfaces) {
        if (!validTag(index, CpTag::Class)) {
            f->diagnostics().error(ReadError::BadConstantPoolRef, 0);
            return Err(ReadError::BadConstantPoolRef);
        }
    }
    auto validateMembers = [&f](const std::vector<MemberInfo>& members) {
        for (const MemberInfo& m : members) {
            if (!f->cp.isUtf8(m.nameCp) || !f->cp.isUtf8(m.descCp)) {
                return false;
            }
            for (const AttrSlot& attr : m.attrs) {
                if (!f->cp.isUtf8(attr.nameCp)) {
                    return false;
                }
            }
        }
        return true;
    };
    if (!validateMembers(f->fields) || !validateMembers(f->methods)) {
        f->diagnostics().error(ReadError::BadConstantPoolRef, 0);
        return Err(ReadError::BadConstantPoolRef);
    }
    for (const AttrSlot& attr : f->attrs) {
        if (!f->cp.isUtf8(attr.nameCp)) {
            f->diagnostics().error(ReadError::BadConstantPoolRef, 0);
            return Err(ReadError::BadConstantPoolRef);
        }
    }

    if (opt.preserveSourceBytes) {
        // vector 的移动不搬迁堆内存，此前解析出的所有字节视图地址依旧有效。
        f->adoptSource(std::move(owned));
    }

    // 状态与诊断必须在模型被 move 走之前取出。Error 级诊断只可能伴随 Err
    // 返回（每处 diag.error 后同步 return Err），这里对 Recovered 起作用的
    // 是 trailing 降级：结构读全了、但尾部不合规（§4.3 Recovered 语义）。
    ReadResult rr;
    rr.state = (f->diagnostics().hasError() || hasTrailing) ? ParseState::Recovered
                                                            : ParseState::Validated;
    rr.diagnostics = f->diagnostics();
    f->setState(rr.state);
    rr.file = std::move(f);
    return Ok(std::move(rr));
}

}   // namespace jbc::core
