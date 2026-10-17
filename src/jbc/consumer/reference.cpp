#include "jbc/consumer/reference.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/insn_codec.hpp"
#include "jbc/core/reader.hpp"

namespace jbc::consumer {

namespace {

using jbc::core::ByteView;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::CpTag;
using jbc::core::MemberInfo;
using jbc::core::Op;
using jbc::core::RawInsn;
using jbc::core::ReadOptions;

/// @brief 一个 Code 体里的位点事实收口（单循环单 ordinal 计数）。
struct MethodFacts
{
    std::vector<MethodRefFact> invokes;
    std::vector<FieldRefFact> fieldAccesses;
    std::vector<TypeRefFact> typeOps;
    std::vector<StringRefFact> strings;
    std::vector<DynamicRefFact> indies;
};

/// @brief Utf8 槽文本。非法态返回空（reader validate 之后防御态无害）。
std::string utf8Text(const ClassFile& cf, const ca::u32 idx)
{
    return std::string(cf.cp.isUtf8(idx) ? cf.cp.utf8Of(idx) : std::string_view{});
}

/// @brief 事实型 CP 指令分派：invoke/field/type/indy 常量。
///
/// 逐 op 显式收口（shapeOf 的枚举不表达「哪侧是 ref 家族」——直白列举
/// 比间接表述稳）。防线纪律：tag 与 Utf8 可达性不符时静默跳过（reader
/// validate 已拦截基本形态；残余畸形到这一步不产生事实、不产生失败）。
/// ordinal = decode 顺序逐条递增（Code 体局部 0 起算，与字形无关）。
MethodFacts factsOfCode(const ClassFile& cf, const CodeAttr& code)
{
    using jbc::core::decodeInsns;
    MethodFacts out;
    const auto ins = decodeInsns(ByteView{code.code});
    if (ins.is_err()) {
        return out;   // 畸形 code：骨架面不把失败外推（reader 侧已 Recovered 档）
    }
    ca::u32 ordinal = 0;
    for (const RawInsn& raw : std::move(ins).unwrap()) {
        const RefSite site{raw.op, ordinal++, raw.pc};
        switch (raw.op) {
            case Op::INVOKEVIRTUAL:
            case Op::INVOKESPECIAL:
            case Op::INVOKESTATIC:
            case Op::INVOKEINTERFACE: {
                if (!cf.cp.valid(raw.cp)) {
                    break;
                }
                const auto& e = cf.cp.at(raw.cp);
                if (e.tag != CpTag::Methodref && e.tag != CpTag::InterfaceMethodref) {
                    break;
                }
                if (!cf.cp.valid(e.b) || cf.cp.tagOf(e.b) != CpTag::NameAndType) {
                    break;
                }
                const auto& nat = cf.cp.at(e.b);
                MethodRefFact f;
                f.site  = site;
                f.owner = std::string(cf.className(e.a));
                f.name  = utf8Text(cf, nat.a);
                f.desc  = utf8Text(cf, nat.b);
                out.invokes.push_back(std::move(f));
                break;
            }
            case Op::GETSTATIC:
            case Op::PUTSTATIC:
            case Op::GETFIELD:
            case Op::PUTFIELD: {
                if (!cf.cp.valid(raw.cp) || cf.cp.tagOf(raw.cp) != CpTag::Fieldref) {
                    break;
                }
                const auto& e = cf.cp.at(raw.cp);
                if (!cf.cp.valid(e.b) || cf.cp.tagOf(e.b) != CpTag::NameAndType) {
                    break;
                }
                const auto& nat = cf.cp.at(e.b);
                FieldRefFact f;
                f.site  = site;
                f.owner = std::string(cf.className(e.a));
                f.name  = utf8Text(cf, nat.a);
                f.desc  = utf8Text(cf, nat.b);
                out.fieldAccesses.push_back(std::move(f));
                break;
            }
            case Op::NEW:
            case Op::ANEWARRAY:
            case Op::CHECKCAST:
            case Op::INSTANCEOF:
            case Op::MULTIANEWARRAY: {
                if (!cf.cp.valid(raw.cp) || cf.cp.tagOf(raw.cp) != CpTag::Class) {
                    break;
                }
                TypeRefFact f;
                f.site         = site;
                f.internalName = std::string(cf.className(raw.cp));  // 数组形态原样
                out.typeOps.push_back(std::move(f));
                break;
            }
            case Op::LDC:
            case Op::LDC_W: {
                if (!cf.cp.valid(raw.cp)) {
                    break;
                }
                const auto& e = cf.cp.at(raw.cp);
                if (e.tag == CpTag::Class) {
                    TypeRefFact f;
                    f.site         = site;
                    f.internalName = std::string(cf.className(raw.cp));
                    out.typeOps.push_back(std::move(f));
                }
                else if (e.tag == CpTag::String) {
                    StringRefFact f;
                    f.site = site;
                    f.text = utf8Text(cf, e.a);
                    out.strings.push_back(std::move(f));
                }
                break;   // 其余常量家族（数值/MH/Dynamic）不属 v1 封闭面
            }
            case Op::INVOKEDYNAMIC: {
                if (!cf.cp.valid(raw.cp) || cf.cp.tagOf(raw.cp) != CpTag::InvokeDynamic) {
                    break;
                }
                const auto& e = cf.cp.at(raw.cp);
                if (!cf.cp.valid(e.b) || cf.cp.tagOf(e.b) != CpTag::NameAndType) {
                    break;
                }
                const auto& nat = cf.cp.at(e.b);
                DynamicRefFact f;
                f.site = site;
                f.bsm  = e.a;
                f.name = utf8Text(cf, nat.a);
                f.desc = utf8Text(cf, nat.b);
                out.indies.push_back(std::move(f));
                break;
            }
            default:
                break;
        }
    }
    return out;
}

}   // namespace

ConsumerResult<ReferenceSummary> ReferenceScanner::scan(const ByteView bytes) const
{
    // 属性/Code 体要 lazy 解码（decodeCode/decodeAttr 的槽 raw span 依附源
    // 字节），源字节必须保真——摘要面（SummaryScanner）不解剖属性体才关得掉。
    jbc::core::ReadOptions opt;
    auto r = jbc::core::readClass(bytes, opt);
    if (r.is_err()) {
        return ca::core::Err(ConsumerError::BadClassFile);
    }
    auto rr = std::move(r).unwrap();   // ReadResult 具名持活（file 归它所有）
    ClassFile& cf = *rr.file;

    ReferenceSummary out;
    out.thisClass  = std::string(cf.thisName());
    out.superClass = std::string(cf.superName());
    for (const ca::u16 iface : cf.interfaces) {
        out.interfaces.push_back(std::string(cf.className(iface)));
    }
    out.fields.reserve(cf.fields.size());
    for (const MemberInfo& f : cf.fields) {
        SummaryField row;
        row.name   = std::string(cf.nameOf(f));
        row.desc   = std::string(cf.descOf(f));
        row.access = f.access;
        out.fields.push_back(std::move(row));
    }

    out.methods.reserve(cf.methods.size());
    for (MemberInfo& m : cf.methods) {
        MethodSection sec;
        sec.name = std::string(cf.nameOf(m));
        sec.desc = std::string(cf.descOf(m));

        // Exceptions 属性 throws 表（保序）；解码失败按缺省跳过——
        // Recovered 类的骨架态照常出事实（Err 不外推，与 reader 档一致）。
        for (jbc::core::AttrSlot& slot : m.attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, "Exceptions")) {
                continue;
            }
            auto d = cf.decodeAttr(slot);
            if (d.is_err()) {
                break;
            }
            if (d.unwrap()->kind() != jbc::core::AttrKind::Exceptions) {
                break;
            }
            const auto& exc = static_cast<const jbc::core::ExceptionsAttr&>(*slot.body);
            for (const ca::u32 classCp : exc.exceptionCps) {
                sec.exceptionTypes.push_back(std::string(cf.className(classCp)));
            }
            break;
        }

        // Code 体事实（CodeAttr 已解到 code 字节 + 异常表 + 嵌套槽骨架）。
        const auto dc = cf.decodeCode(m);
        if (dc.is_ok() && dc.unwrap() != nullptr) {
            const CodeAttr* code = dc.unwrap();
            MethodFacts facts = factsOfCode(cf, *code);
            sec.invokes       = std::move(facts.invokes);
            sec.fieldAccesses = std::move(facts.fieldAccesses);
            sec.typeOps       = std::move(facts.typeOps);
            sec.strings       = std::move(facts.strings);
            sec.indies        = std::move(facts.indies);
        }
        out.methods.push_back(std::move(sec));
    }
    return ca::core::Ok(std::move(out));
}

ConsumerResult<std::vector<ReferenceSummary>>
ReferenceScanner::scanSource(const IClassSource& source) const
{
    std::vector<ReferenceSummary> out;
    std::vector<std::string_view> names;
    source.listClasses(names);
    out.reserve(names.size());
    for (const std::string_view name : names) {
        const ConsumerResult<ByteView> got = source.bytes(name);
        if (got.is_err()) {
            return ca::core::Err(got.unwrap_err());
        }
        const ConsumerResult<ReferenceSummary> one = scan(got.unwrap());
        if (one.is_err()) {
            return ca::core::Err(one.unwrap_err());
        }
        out.push_back(std::move(one).unwrap());
    }
    return ca::core::Ok(std::move(out));
}

}   // namespace jbc::consumer
