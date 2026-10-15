/// @file        cp_compact.cpp
/// @brief       A8：CP 显式压缩——计划/应用两相的全量重索引（design.md §36）。
///
/// 计划相只读：解码全部属性面 → 按封闭集分派 → 以规范根序遍历引用面 intern
/// 出新池与旧→新映射（任何 Err 不留半截模型）。应用相按映射一次性换池并重写
/// 全部引用面——每个写入值都在计划相定形，构造上不可失败。
#include "jbc/ir/cp_compact.hpp"

#include <map>
#include <string_view>
#include <tuple>
#include <vector>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/insn_codec.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/sem_index.hpp"

namespace jbc::ir {

namespace {

using core::AttrKind;
using core::AttrSlot;
using core::ByteView;
using core::ClassFile;
using core::CodeAttr;
using core::ConstantPool;

// ---- 压缩侧结构化属性（§36.1 第 2 层）--------------------------------
// B3 四件（SourceFile/InnerClasses/EnclosingMethod/Exceptions）已立 typed
// 模型并收编进两相 switch 的 typed 分支，Raw 名字分派的本地解析按 §36.5
// 挂账退役（SourceFile 分支随其接线批保留为不可达兜底）。仍走 Raw 名单
// 分派的只剩 ConstantValue。

constexpr std::string_view kConstantValueName  = "ConstantValue";
constexpr std::string_view kSourceFileName     = "SourceFile";

/// @brief 指令是否携带 CP 操作数（CP1/CP2/CP2Count/CP2Zero2/CP2Dims 五形）。
bool insnHasCpOperand(core::Op op) noexcept
{
    switch (core::shapeOf(op)) {
        case core::Shape::CP1:
        case core::Shape::CP2:
        case core::Shape::CP2Count:
        case core::Shape::CP2Zero2:
        case core::Shape::CP2Dims:
            return true;
        default:
            return false;
    }
}

class Compactor
{
public:
    Compactor(ClassSession& s, ClassFile& cf)
        : s_(s)
        , cf_(cf)
        , old_(cf.cp)
        , map_(cf.cp.size(), 0)
        , live_(cf.cp.size(), 0)
        , classOf_(cf.cp.size(), 0)
    {}

    // ---- 计划相 · 第一步：闭包标记（只读模型；解码置 Decoded 不改字节）----
    //
    // 映射核心采用「子序 + 代表去重」方案（§36.2）：新池 = 活条目按老索引
    // 升序的子序列，等价类只保留最小老索引作代表。由此 map_[k] ≤ k 对一切
    // 活条目恒成立（代表：升序秩不超自身老下标；重复：并入更小老下标的代表
    // 槽），CP1（ldc）操作数域宽因此从不变宽——这是宽形态不收缩裁决的安全
    // 前提。规范根序只决定「哪些条目活」，不影响新索引。

    Result<void, CompactFailure> plan()
    {
        const Result<void, CompactFailure> head = markCp(cf_.thisClass);
        if (head.is_err()) {
            return head.unwrap_err();
        }
        if (cf_.superClass != 0) {
            const Result<void, CompactFailure> r = markCp(cf_.superClass);
            if (r.is_err()) {
                return r.unwrap_err();
            }
        }
        for (const ca::u16 i : cf_.interfaces) {
            const Result<void, CompactFailure> r = markCp(i);
            if (r.is_err()) {
                return r.unwrap_err();
            }
        }
        const auto planMember = [&](std::vector<core::MemberInfo>& ms, const bool isMethod)
            -> Result<void, CompactFailure> {
            ca::usize backingIdx = 0;
            for (core::MemberInfo& m : ms) {
                ctx_ = std::string_view{};
                const Result<void, CompactFailure> rn = markCp(m.nameCp);
                if (rn.is_err()) {
                    return rn.unwrap_err();
                }
                const Result<void, CompactFailure> rd = markCp(m.descCp);
                if (rd.is_err()) {
                    return rd.unwrap_err();
                }
                // 升格方法的 backing 指令面是陈旧面（DOM 为准，lower 整体重建）。
                const bool staleCode = isMethod && backingMethodPromoted(backingIdx);
                for (AttrSlot& a : m.attrs) {
                    const Result<void, CompactFailure> r = planAttr(a, staleCode);
                    if (r.is_err()) {
                        return r.unwrap_err();
                    }
                }
                backingIdx += 1;
            }
            return Ok();
        };
        if (const Result<void, CompactFailure> r = planMember(cf_.fields, false); r.is_err()) {
            return r.unwrap_err();
        }
        if (const Result<void, CompactFailure> r = planMember(cf_.methods, true); r.is_err()) {
            return r.unwrap_err();
        }
        for (AttrSlot& a : cf_.attrs) {
            const Result<void, CompactFailure> r = planAttr(a);
            if (r.is_err()) {
                return r.unwrap_err();
            }
        }

        // 升格体的引用面在这里进场（非陈旧面）：DOM 指令操作数可能是会话期
        // 语义 intern、backing 字节面尚未引用的条目（如 setString 后未 lower）。
        // backing 侧的陈旧指令/异常表已在 planAttr(staleCode) 中跳过，两边
        // 合起来才是升格方法的完整引用面（§36.3）。
        for (MethodUnit& mu : s_.unit().methods) {
            if (mu.code == nullptr) {
                continue;
            }
            CodeUnit& cu = *mu.code;
            for (const InsnId id : cu.dom.order()) {
                const Result<Insn*, EditError> g = cu.dom.get(id);
                if (g.is_err()) {
                    continue;   // 句柄簿记自洽，不可达
                }
                const Insn& in = *g.unwrap();
                if (!insnHasCpOperand(in.op)) {
                    continue;
                }
                const Result<void, CompactFailure> r = markCp(in.cp);
                if (r.is_err()) {
                    return r.unwrap_err();
                }
            }
            for (const CodeUnit::Exc& e : cu.exc) {
                const Result<void, CompactFailure> r = markCp(e.catchCp);
                if (r.is_err()) {
                    return r.unwrap_err();
                }
            }
        }
        return Ok();
    }

    // ---- 计划相 · 第二步：等价类 + 升序秩（子序映射的成形处）----

    /// @brief 完成映射：等价类归类 → 老索引升序赋秩（代表 = 首触 = 最小老
    ///        下标；Long/Double 双槽在秩空间同步占两席）→ 升序填充新池 →
    ///        Code 体操作数重编码。全程只读旧池、只写 map_/build_/计划缓存。
    Result<void, CompactFailure> materialize()
    {
        assignRanks();
        const Result<void, CompactFailure> f = fillPool();
        if (f.is_err()) {
            return f.unwrap_err();
        }
        fillCodeBodies();
        core::Diagnostics diag;
        if (build_.validateReferences(diag).is_err()) {
            return Err(failOf(CompactError::Internal, {}));
        }
        return Ok();
    }

    /// @brief 引用闭包标记：递归标活条目（子条目随行）。越界/wide 续槽引用
    ///        显式失败，不猜（封闭集内结构知识全部定长可判）。
    Result<void, CompactFailure> markCp(ca::u32 oldIdx)
    {
        if (oldIdx == 0) {
            return Ok();
        }
        if (oldIdx >= live_.size() || !old_.valid(oldIdx)) {
            return Err(failOf(CompactError::MalformedAttr, ctx_));
        }
        if (live_[oldIdx] != 0) {
            return Ok();
        }
        live_[oldIdx] = 1;
        const ConstantPool::Entry& e = old_.at(oldIdx);
        switch (e.tag) {
            case core::CpTag::Utf8:
            case core::CpTag::Integer:
            case core::CpTag::Float:
            case core::CpTag::Long:
            case core::CpTag::Double:
                return Ok();   // 叶子：无子引用
            case core::CpTag::MethodHandle:
            case core::CpTag::Dynamic:
            case core::CpTag::InvokeDynamic: {
                // a 列分别是 reference_kind / bootstrap 下标——非 CP 索引。
                const Result<void, CompactFailure> rb = markCp(e.b);
                if (rb.is_err()) {
                    return rb.unwrap_err();
                }
                return Ok();
            }
            default: {
                const Result<void, CompactFailure> ra = markCp(e.a);
                if (ra.is_err()) {
                    return ra.unwrap_err();
                }
                if (e.tag != core::CpTag::Class && e.tag != core::CpTag::String
                    && e.tag != core::CpTag::MethodType && e.tag != core::CpTag::Module
                    && e.tag != core::CpTag::Package) {
                    const Result<void, CompactFailure> rb = markCp(e.b);
                    if (rb.is_err()) {
                        return rb.unwrap_err();
                    }
                }
                return Ok();
            }
        }
    }

    /// @brief 内容等价类：叶子按字节/位模式，复合按（tag, 子类号）。类号递归
    ///        带备忘，只触活条目；同类条目在升序扫里的首触即代表。
    ca::u32 classId(ca::u32 oldIdx)
    {
        if (classOf_[oldIdx] != 0) {
            return classOf_[oldIdx];
        }
        const ConstantPool::Entry& e = old_.at(oldIdx);
        const ca::u8 tag             = static_cast<ca::u8>(e.tag);
        ca::u32 cid                  = 0;
        if (e.tag == core::CpTag::Utf8) {
            const std::string_view s =
                e.utf8 == nullptr || e.utf8Len == 0
                    ? std::string_view{}
                    : std::string_view{reinterpret_cast<const char*>(e.utf8), e.utf8Len};
            const auto it = utf8Class_.find(s);
            cid = it != utf8Class_.end() ? it->second : 0;
            if (cid == 0) {
                cid = nextClass_++;
                utf8Class_.emplace(s, cid);
            }
        } else if (e.tag == core::CpTag::Integer || e.tag == core::CpTag::Float) {
            const auto key = std::make_tuple(tag, static_cast<ca::u32>(e.ival), ca::u32{0});
            cid = classByKey_(key);
        } else if (e.tag == core::CpTag::Long || e.tag == core::CpTag::Double) {
            // 位模式即身份（-0.0 / NaN 载荷保真）；64 位拆两半进复合键。
            const auto lo = static_cast<ca::u32>(static_cast<ca::u64>(e.lval) & 0xFFFFFFFFu);
            const auto hi = static_cast<ca::u32>(static_cast<ca::u64>(e.lval) >> 32);
            cid = classByKey_(std::make_tuple(tag, lo, hi));
        } else {
            // MethodHandle 的 a 是 reference_kind、Dynamic/InvokeDynamic 的 a
            // 是 bootstrap 下标——非 CP 索引，原样参与键，递归只进真正的
            // 引用列（MH/Dyn/Indy 是 b 列；其余 a（必要时 b）列）。
            if (e.tag == core::CpTag::MethodHandle || e.tag == core::CpTag::Dynamic
                || e.tag == core::CpTag::InvokeDynamic) {
                const ca::u32 cb = classId(e.b);
                cid = classByKey_(std::make_tuple(tag, static_cast<ca::u32>(e.a), cb));
            } else {
                const ca::u32 ca = classId(e.a);
                ca::u32 cb = 0;
                if (e.tag != core::CpTag::Class && e.tag != core::CpTag::String
                    && e.tag != core::CpTag::MethodType && e.tag != core::CpTag::Module
                    && e.tag != core::CpTag::Package) {
                    cb = classId(e.b);
                }
                cid = classByKey_(std::make_tuple(tag, ca, cb));
            }
        }
        classOf_[oldIdx] = cid;
        return cid;
    }

    /// @brief 类号查/建（数值与复合键共用）。
    ca::u32 classByKey_(const std::tuple<ca::u8, ca::u32, ca::u32>& key)
    {
        const auto it = classByKeyMap_.find(key);
        if (it != classByKeyMap_.end()) {
            return it->second;
        }
        const ca::u32 cid = nextClass_++;
        classByKeyMap_.emplace(key, cid);
        return cid;
    }

    /// @brief 升序赋秩：代表拿新槽（双槽占两席），重复并入代表的新索引。
    void assignRanks()
    {
        ca::u32 next = 1;
        for (ca::u32 k = 1; k < old_.size(); ++k) {
            if (live_[k] == 0) {
                continue;
            }
            const ca::u32 cid = classId(k);
            const auto it     = classToNew_.find(cid);
            if (it != classToNew_.end()) {
                map_[k] = it->second;   // 重复：并入代表
                continue;
            }
            const bool dual = old_.at(k).tag == core::CpTag::Long
                || old_.at(k).tag == core::CpTag::Double;
            map_[k] = next;
            classToNew_.emplace(cid, next);
            classRep_.emplace(cid, k);
            next += dual ? 2 : 1;
        }
    }

    /// @brief 升序填池：只落代表条目，子索引用已完备的 map_（顺序跟随老池，
    ///        引用拓扑保真；classfile 本就允许前向引用）。
    Result<void, CompactFailure> fillPool()
    {
        for (ca::u32 k = 1; k < old_.size(); ++k) {
            if (live_[k] == 0 || classRep_[classOf_[k]] != k) {
                continue;   // 死条目与重复不落位
            }
            const ConstantPool::Entry& e = old_.at(k);
            switch (e.tag) {
                case core::CpTag::Utf8: {
                    const std::string_view sv =
                        e.utf8 == nullptr || e.utf8Len == 0
                            ? std::string_view{}
                            : std::string_view{reinterpret_cast<const char*>(e.utf8),
                                               e.utf8Len};
                    static_cast<void>(build_.addUtf8(sv));
                    break;
                }
                case core::CpTag::Integer:
                    static_cast<void>(build_.addInteger(e.ival));
                    break;
                case core::CpTag::Float:
                    static_cast<void>(build_.addFloatBits(static_cast<ca::u32>(e.ival)));
                    break;
                case core::CpTag::Long:
                    static_cast<void>(build_.addLong(e.lval));
                    break;
                case core::CpTag::Double:
                    static_cast<void>(build_.addDoubleBits(static_cast<ca::u64>(e.lval)));
                    break;
                case core::CpTag::Class:
                case core::CpTag::String:
                case core::CpTag::MethodType:
                case core::CpTag::Module:
                case core::CpTag::Package:
                    appendComposite(e.tag, map_[e.a], 0);
                    break;
                case core::CpTag::MethodHandle:
                case core::CpTag::Dynamic:
                case core::CpTag::InvokeDynamic:
                    appendComposite(e.tag, e.a, map_[e.b]);
                    break;
                default:
                    appendComposite(e.tag, map_[e.a], map_[e.b]);
                    break;
            }
        }
        return Ok();
    }

    /// @brief Code 体操作数重编码：秩完备后按映射换字面量（形态不收缩，
    ///        尺寸逐指令不变，§36.2 挂账）。
    void fillCodeBodies()
    {
        for (auto& kv : insnPlan_) {
            std::vector<core::RawInsn>& insns = kv.second;
            bool changed                      = false;
            for (core::RawInsn& in : insns) {
                if (!insnHasCpOperand(in.op)) {
                    continue;
                }
                const ca::u32 n = remap(in.cp);
                if (n != in.cp) {
                    in.cp    = n;
                    changed  = true;
                }
            }
            if (!changed) {
                continue;
            }
            const Result<std::vector<ca::u8>, core::WriteError> e = core::encodeInsns(insns);
            if (e.is_err()) {
                // 单调映射下 CP1 域宽不变宽，构造上不可达；兜底显式爆。
                codeEncodeFailed_ = true;
                return;
            }
            codePlan_[kv.first] = std::move(e).unwrap();
        }
    }

    /// @brief 池已紧凑（映射恒等且无死条目）→ 整体 no-op。
    bool isNoop() const
    {
        if (build_.size() != old_.size()) {
            return false;
        }
        for (ca::u32 i = 1; i < old_.size(); ++i) {
            if (old_.valid(i) && map_[i] != i) {
                return false;
            }
        }
        return true;
    }

    // ---- 应用相（按计划一次性落位；换池与簿记归 compactConstantPool）----

    void apply()
    {
        // 1) 类头与成员槽。
        cf_.thisClass = static_cast<ca::u16>(remap(cf_.thisClass));
        if (cf_.superClass != 0) {
            cf_.superClass = static_cast<ca::u16>(remap(cf_.superClass));
        }
        for (ca::u16& i : cf_.interfaces) {
            i = static_cast<ca::u16>(remap(i));
        }
        const auto applyMember = [this](std::vector<core::MemberInfo>& ms) {
            for (core::MemberInfo& m : ms) {
                m.nameCp = remap(m.nameCp);
                m.descCp = remap(m.descCp);
            }
        };
        applyMember(cf_.fields);
        applyMember(cf_.methods);

        // 2) 属性面（类级 + 成员级；Code 嵌套随体递归）。
        for (AttrSlot& a : cf_.attrs) {
            applyAttr(a);
        }
        const auto applyMemberAttrs = [this](std::vector<core::MemberInfo>& ms, const bool isMethod) {
            ca::usize backingIdx = 0;
            for (core::MemberInfo& m : ms) {
                const bool staleCode = isMethod && backingMethodPromoted(backingIdx);
                for (AttrSlot& a : m.attrs) {
                    applyAttr(a, staleCode);
                }
                backingIdx += 1;
            }
        };
        applyMemberAttrs(cf_.fields, false);
        applyMemberAttrs(cf_.methods, true);

        // 3) 已升格方法：IR 与 backing 保持同一索引坐标系（§36.3）。
        for (MethodUnit& mu : s_.unit().methods) {
            if (mu.code == nullptr) {
                continue;
            }
            CodeUnit& cu = *mu.code;
            for (const InsnId id : cu.dom.order()) {
                const Result<Insn*, EditError> g = cu.dom.get(id);
                if (g.is_err()) {
                    continue;   // 句柄簿记自洽，不可达
                }
                Insn& in = *g.unwrap();
                if (insnHasCpOperand(in.op)) {
                    in.cp = remap(in.cp);
                }
            }
            for (CodeUnit::Exc& e : cu.exc) {
                e.catchCp = remap(e.catchCp);
            }
        }
    }

    /// @brief 移交计划相构造的新池（换池动作归 compactConstantPool：旧池退居
    ///        会话侧的簿记在会话私有面）。
    ConstantPool takeNewPool() noexcept { return std::move(build_); }

    /// @brief Code 体重编码的兜底哨兵（单调映射下不可达，见 fillCodeBodies）。
    bool encodeFailed() const noexcept { return codeEncodeFailed_; }

    /// @brief 兜底校验：换池后的引用面必须自洽（内部一致性网，正常不可达）。
    bool poolSelfConsistent() const
    {
        core::Diagnostics diag;
        return cf_.cp.validateReferences(diag).is_ok();
    }

private:
    // ---- 失败与映射 ----

    CompactFailure failOf(CompactError code, std::string_view name) const
    {
        CompactFailure f;
        f.code     = code;
        f.attrName = name;
        return f;
    }

    /// @brief 应用相的索引换算；0 是「无条目」哨兵，原样穿透。越界按 0
    ///        落到一致性网（计划/应用分派不一致的实现缺陷显式爆掉，不 UB）。
    ca::u32 remap(ca::u32 oldIdx) const noexcept
    {
        return oldIdx == 0 || oldIdx >= map_.size() ? 0 : map_[oldIdx];
    }

    /// @brief backing 物理位置上的方法是否处于升格态（CodeUnit 非空）。
    ///        升格方法的 backing 指令面是陈旧面：DOM 才是指令流的权威来源，
    ///        lower 时整体重建（§36.3）。
    bool backingMethodPromoted(const ca::usize backingIdx) const
    {
        for (const MethodUnit& mu : s_.unit().methods) {
            if (mu.backingIndex == backingIdx && mu.code != nullptr) {
                return true;
            }
        }
        return false;
    }

    /// @brief 复合条目落位：a（单索引条目）或 a+b；MethodHandle 的 a 是
    ///        reference_kind、Dynamic/InvokeDynamic 的 a 是 bootstrap 下标
    ///        （不是 CP 索引，原样携带）。
    ca::u32 appendComposite(core::CpTag tag, ca::u32 a, ca::u32 b)
    {
        switch (tag) {
            case core::CpTag::Class:
                return build_.addClass(a);
            case core::CpTag::String:
                return build_.addString(a);
            case core::CpTag::MethodType:
                return build_.addMethodType(a);
            case core::CpTag::Module:
                return build_.addModule(a);
            case core::CpTag::Package:
                return build_.addPackage(a);
            case core::CpTag::NameAndType:
                return build_.addNameAndType(a, b);
            case core::CpTag::Fieldref:
            case core::CpTag::Methodref:
            case core::CpTag::InterfaceMethodref:
                return build_.addRef(tag, a, b);
            case core::CpTag::MethodHandle:
                return build_.addMethodHandle(a, b);
            case core::CpTag::Dynamic:
                return build_.addDynamic(a, b);
            case core::CpTag::InvokeDynamic:
                return build_.addInvokeDynamic(a, b);
            default:
                return 0;   // 叶子在上游分派，不可达
        }
    }

    // ---- 计划相：引用面遍历（封闭集分派，§36.1）----

    std::string_view attrNameOf(const AttrSlot& slot) const noexcept
    {
        return old_.utf8Of(slot.nameCp);
    }

    Result<void, CompactFailure> planAttr(AttrSlot& slot, const bool staleCode = false)
    {
        const Result<core::AttrBody*, core::ReadError> d = cf_.decodeAttr(slot);
        if (d.is_err()) {
            return Err(failOf(CompactError::MalformedAttr, attrNameOf(slot)));
        }
        ctx_ = attrNameOf(slot);
        const Result<void, CompactFailure> rn = markCp(slot.nameCp);
        if (rn.is_err()) {
            return rn.unwrap_err();
        }
        core::AttrBody& body = *slot.body;
        switch (body.kind()) {
            case AttrKind::Code:
                return planCode(slot, static_cast<CodeAttr&>(body), staleCode);
            case AttrKind::BootstrapMethods: {
                auto& b = static_cast<core::BootstrapMethodsAttr&>(body);
                for (core::BootstrapMethodsAttr::Entry& e : b.entries) {
                    const Result<void, CompactFailure> r1 = markCp(e.methodRef);
                    if (r1.is_err()) {
                        return r1.unwrap_err();
                    }
                    for (const ca::u32 arg : e.args) {
                        const Result<void, CompactFailure> r2 = markCp(arg);
                        if (r2.is_err()) {
                            return r2.unwrap_err();
                        }
                    }
                }
                return Ok();
            }
            case AttrKind::StackMapTable: {
                // 仅 Object 载荷是 CP Class；Uninitialized 载荷是 pc，不得误重映射。
                auto& b = static_cast<core::StackMapTableAttr&>(body);
                for (core::StackMapTableAttr::Frame& f : b.frames) {
                    for (core::StackMapTableAttr::VType& v : f.locals) {
                        if (v.tag == core::StackMapTableAttr::VType::kObject) {
                            const Result<void, CompactFailure> r = markCp(v.cp);
                            if (r.is_err()) {
                                return r.unwrap_err();
                            }
                        }
                    }
                    for (core::StackMapTableAttr::VType& v : f.stack) {
                        if (v.tag == core::StackMapTableAttr::VType::kObject) {
                            const Result<void, CompactFailure> r = markCp(v.cp);
                            if (r.is_err()) {
                                return r.unwrap_err();
                            }
                        }
                    }
                }
                return Ok();
            }
            case AttrKind::Annotations: {
                auto& b = static_cast<core::AnnotationsAttr&>(body);
                for (core::AnnotationsAttr::Annotation& a : b.annos) {
                    const Result<void, CompactFailure> r = planAnnotation(a);
                    if (r.is_err()) {
                        return r.unwrap_err();
                    }
                }
                return Ok();
            }
            case AttrKind::LocalVariableTable: {
                auto& b = static_cast<core::LocalVariableTableAttr&>(body);
                for (core::LocalVariableTableAttr::Row& row : b.rows) {
                    const Result<void, CompactFailure> r1 = markCp(row.nameCp);
                    if (r1.is_err()) {
                        return r1.unwrap_err();
                    }
                    const Result<void, CompactFailure> r2 = markCp(row.descCp);
                    if (r2.is_err()) {
                        return r2.unwrap_err();
                    }
                }
                return Ok();
            }
            case AttrKind::Signature: {
                auto& b = static_cast<core::SignatureAttr&>(body);
                const Result<void, CompactFailure> r = markCp(b.signatureCp);
                if (r.is_err()) {
                    return r.unwrap_err();
                }
                return Ok();
            }
            case AttrKind::SourceFile: {
                // B3 接分派后 SourceFile 为 typed 体（Raw 形不再可达）；
                // 单 u2 槽直接标记，trailing 不含 CP 引用。
                auto& b = static_cast<core::SourceFileAttr&>(body);
                const Result<void, CompactFailure> r = markCp(b.sourceFileCp);
                if (r.is_err()) {
                    return r.unwrap_err();
                }
                return Ok();
            }
            case AttrKind::InnerClasses: {
                // 行三 CP 槽（inner/outer/inner_name）标记；access_flags 是
                // 访问位不是索引；trailing 不含 CP 引用。
                auto& b = static_cast<core::InnerClassesAttr&>(body);
                for (core::InnerClassesAttr::Row& row : b.rows) {
                    const Result<void, CompactFailure> r1 = markCp(row.innerClassInfoCp);
                    if (r1.is_err()) {
                        return r1.unwrap_err();
                    }
                    const Result<void, CompactFailure> r2 = markCp(row.outerClassInfoCp);
                    if (r2.is_err()) {
                        return r2.unwrap_err();
                    }
                    const Result<void, CompactFailure> r3 = markCp(row.innerNameCp);
                    if (r3.is_err()) {
                        return r3.unwrap_err();
                    }
                }
                return Ok();
            }
            case AttrKind::EnclosingMethod: {
                // 两槽全标记；methodNatCp==0（外围为初始化器）由 markCp 放行。
                auto& b = static_cast<core::EnclosingMethodAttr&>(body);
                const Result<void, CompactFailure> r1 = markCp(b.classCp);
                if (r1.is_err()) {
                    return r1.unwrap_err();
                }
                const Result<void, CompactFailure> r2 = markCp(b.methodNatCp);
                if (r2.is_err()) {
                    return r2.unwrap_err();
                }
                return Ok();
            }
            case AttrKind::Exceptions: {
                // 全槽是 CP Class 索引。
                auto& b = static_cast<core::ExceptionsAttr&>(body);
                for (const ca::u32 cp : b.exceptionCps) {
                    const Result<void, CompactFailure> r = markCp(cp);
                    if (r.is_err()) {
                        return r.unwrap_err();
                    }
                }
                return Ok();
            }
            case AttrKind::TypeAnnotations: {
                auto& b = static_cast<core::TypeAnnotationsAttr&>(body);
                for (core::TypeAnnotationsAttr::Annotation& a : b.annotations) {
                    // throwsIndex（Exceptions 表行）/supertypeIndex（interfaces
                    // 下标）/catchIndex（异常表条目下标）不是 CP 索引，不动。
                    const Result<void, CompactFailure> r = markCp(a.typeIndexCp);
                    if (r.is_err()) {
                        return r.unwrap_err();
                    }
                    for (auto& kv : a.values) {
                        const Result<void, CompactFailure> rn = markCp(kv.first);
                        if (rn.is_err()) {
                            return rn.unwrap_err();
                        }
                        const Result<void, CompactFailure> rv = planElemValue(kv.second);
                        if (rv.is_err()) {
                            return rv.unwrap_err();
                        }
                    }
                }
                return Ok();
            }
            case AttrKind::LineNumberTable:
            case AttrKind::CharacterRangeTable:
                return Ok();   // 体不含 CP 引用
            case AttrKind::Raw:
                return planRaw(slot);
            default:
                return Err(failOf(CompactError::Internal, ctx_));
        }
    }

    Result<void, CompactFailure> planAnnotation(const core::AnnotationsAttr::Annotation& a)
    {
        const Result<void, CompactFailure> rt = markCp(a.typeCp);
        if (rt.is_err()) {
            return rt.unwrap_err();
        }
        for (const auto& kv : a.values) {
            const Result<void, CompactFailure> rn = markCp(kv.first);
            if (rn.is_err()) {
                return rn.unwrap_err();
            }
            const Result<void, CompactFailure> rv = planElemValue(kv.second);
            if (rv.is_err()) {
                return rv.unwrap_err();
            }
        }
        return Ok();
    }

    Result<void, CompactFailure> planElemValue(const core::AnnotationsAttr::ElemValue& e)
    {
        switch (e.tag) {
            case 'e': {
                const Result<void, CompactFailure> r1 = markCp(e.typeNameCp);
                if (r1.is_err()) {
                    return r1.unwrap_err();
                }
                const Result<void, CompactFailure> r2 = markCp(e.constNameCp);
                if (r2.is_err()) {
                    return r2.unwrap_err();
                }
                return Ok();
            }
            case '@':
                if (e.nested != nullptr) {
                    return planAnnotation(*e.nested);
                }
                return Ok();
            case '[':
                for (const core::AnnotationsAttr::ElemValue& it : e.items) {
                    const Result<void, CompactFailure> r = planElemValue(it);
                    if (r.is_err()) {
                        return r.unwrap_err();
                    }
                }
                return Ok();
            default:
                // 'B' 'C' 'D' 'F' 'I' 'J' 'S' 'Z' 's' 'c'：单 u2 常量/类索引。
                const Result<void, CompactFailure> r = markCp(e.cp);
                if (r.is_err()) {
                    return r.unwrap_err();
                }
                return Ok();
        }
    }

    Result<void, CompactFailure> planCode(const AttrSlot& slot, CodeAttr& code, const bool staleCode)
    {
        // 升格方法：backing 指令流与异常表是陈旧面，不进引用闭包——其中的
        // 条目（如已被 DOM 编辑替换的旧常量）应当死在压缩里；嵌套属性面
        // （LNT/LVT 等）在 lower 中按 pc 重定向保留，引用仍是活的，照常遍历。
        if (!staleCode) {
            const Result<std::vector<core::RawInsn>, core::ReadError> d =
                core::decodeInsns(ByteView{code.code});
            if (d.is_err()) {
                return Err(failOf(CompactError::MalformedAttr, attrNameOf(slot)));
            }
            // 解码只验形，操作数换字面量留给 materialize 的 fillCodeBodies
            //（秩完备后才能算新索引）；解码结果按 CodeAttr* 缓存复用。
            std::vector<core::RawInsn> insns = std::move(d).unwrap();
            for (core::RawInsn& in : insns) {
                if (!insnHasCpOperand(in.op)) {
                    continue;
                }
                const Result<void, CompactFailure> r = markCp(in.cp);
                if (r.is_err()) {
                    return r.unwrap_err();
                }
            }
            for (CodeAttr::ExcEntry& e : code.excTable) {
                const Result<void, CompactFailure> r = markCp(e.catchType);
                if (r.is_err()) {
                    return r.unwrap_err();
                }
            }
            insnPlan_[&code] = std::move(insns);
        }
        for (AttrSlot& nested : code.attrs) {
            const Result<void, CompactFailure> r = planAttr(nested);
            if (r.is_err()) {
                return r.unwrap_err();
            }
        }
        return Ok();
    }

    Result<void, CompactFailure> planRaw(AttrSlot& slot)
    {
        const auto& raw = static_cast<const core::RawAttr&>(*slot.body);
        const std::string_view name = attrNameOf(slot);
        if (raw.bytes().empty()) {
            // 零长体构造上不可能携带 CP 引用（无字节）：透传放行（§36.1 第 3 层）。
            return Ok();
        }
        const Result<std::vector<ca::u8>, CompactFailure> r =
            transcodeRawBody(name, ByteView{raw.bytes()},
                             [this](ca::u32 i) -> Result<ca::u32, CompactFailure> {
                                 // 标记相：只验形与闭包，值原样穿透（产物丢弃）。
                                 const Result<void, CompactFailure> m = markCp(i);
                                 if (m.is_err()) {
                                     return Err(m.unwrap_err());
                                 }
                                 return Ok(i);
                             });
        if (r.is_err()) {
            return r.unwrap_err();
        }
        return Ok();
    }

    // ---- 压缩侧五件的结构遍历（计划/应用共用同一形状知识）----

    template <typename Act>
    Result<std::vector<ca::u8>, CompactFailure>
    transcodeRawBody(std::string_view name, ByteView body, Act&& act)
    {
        core::Cursor cur{body};
        core::ByteSink out;
        const auto rd = [&]() -> Result<ca::u32, CompactFailure> {
            const Result<ca::u16, core::ReadError> r = cur.u16();
            if (r.is_err()) {
                return Err(failOf(CompactError::MalformedAttr, name));
            }
            return Ok(ca::u32{r.unwrap()});
        };
        const auto wr = [&out](ca::u32 v) { out.u16(static_cast<ca::u16>(v)); };
        const auto cp = [&](ca::u32 oldIdx) -> Result<ca::u32, CompactFailure> {
            return act(oldIdx);
        };

        if (name == kSourceFileName || name == kConstantValueName) {
            const Result<ca::u32, CompactFailure> v = rd();
            if (v.is_err()) {
                return v.unwrap_err();
            }
            const Result<ca::u32, CompactFailure> m = cp(v.unwrap());
            if (m.is_err()) {
                return m.unwrap_err();
            }
            wr(m.unwrap());
        } else {
            // InnerClasses/EnclosingMethod/Exceptions 已收编 typed 分支，
            // Raw 形名单分派退役（§36.5）；名单外其余名字显式拒绝。
            return Err(failOf(CompactError::AttrNotModeled, name));
        }
        if (!cur.eof()) {
            // 声明结构之外还有剩余字节：五件都是定长形状，属畸形（§36.1）。
            return Err(failOf(CompactError::MalformedAttr, name));
        }
        return Ok(out.bytes());
    }

    // ---- 应用相：引用面重写（返回该槽是否被触碰）----

    static void applyAnnotation(core::AnnotationsAttr::Annotation& a, bool& touched,
                                const Compactor& self)
    {
        const ca::u32 nt = self.remap(a.typeCp);
        if (nt != a.typeCp) {
            a.typeCp = nt;
            touched  = true;
        }
        for (auto& kv : a.values) {
            const ca::u32 nn = self.remap(kv.first);
            if (nn != kv.first) {
                kv.first = nn;
                touched  = true;
            }
            applyElemValue(kv.second, touched, self);
        }
    }

    static void applyElemValue(core::AnnotationsAttr::ElemValue& e, bool& touched,
                               const Compactor& self)
    {
        switch (e.tag) {
            case 'e': {
                const ca::u32 n1 = self.remap(e.typeNameCp);
                if (n1 != e.typeNameCp) {
                    e.typeNameCp = n1;
                    touched      = true;
                }
                const ca::u32 n2 = self.remap(e.constNameCp);
                if (n2 != e.constNameCp) {
                    e.constNameCp = n2;
                    touched       = true;
                }
                break;
            }
            case '@':
                if (e.nested != nullptr) {
                    applyAnnotation(*e.nested, touched, self);
                }
                break;
            case '[':
                for (core::AnnotationsAttr::ElemValue& it : e.items) {
                    applyElemValue(it, touched, self);
                }
                break;
            default: {
                const ca::u32 n = self.remap(e.cp);
                if (n != e.cp) {
                    e.cp    = n;
                    touched = true;
                }
                break;
            }
        }
    }

    bool applyAttr(AttrSlot& slot, const bool staleCode = false)
    {
        bool touched = false;
        // 属性名先于重映射解析（applyRaw 的格式分派按旧名查表）。
        const std::string_view rawName = attrNameOf(slot);
        const ca::u32 newName = remap(slot.nameCp);
        if (newName != slot.nameCp) {
            slot.nameCp = newName;
            touched     = true;
        }
        core::AttrBody& body = *slot.body;
        switch (body.kind()) {
            case AttrKind::Code: {
                auto& code = static_cast<CodeAttr&>(body);
                if (!staleCode) {
                    const auto it = codePlan_.find(&code);
                    if (it != codePlan_.end()) {
                        code.code = std::move(it->second);
                        codePlan_.erase(it);
                        touched = true;
                    }
                    for (CodeAttr::ExcEntry& e : code.excTable) {
                        const ca::u32 n = remap(e.catchType);
                        if (n != e.catchType) {
                            e.catchType = n;
                            touched     = true;
                        }
                    }
                }
                for (AttrSlot& nested : code.attrs) {
                    applyAttr(nested);
                }
                break;
            }
            case AttrKind::BootstrapMethods: {
                auto& b = static_cast<core::BootstrapMethodsAttr&>(body);
                for (core::BootstrapMethodsAttr::Entry& e : b.entries) {
                    const ca::u32 n = remap(e.methodRef);
                    if (n != e.methodRef) {
                        e.methodRef = n;
                        touched     = true;
                    }
                    for (ca::u32& arg : e.args) {
                        const ca::u32 na = remap(arg);
                        if (na != arg) {
                            arg    = na;
                            touched = true;
                        }
                    }
                }
                break;
            }
            case AttrKind::StackMapTable: {
                auto& b = static_cast<core::StackMapTableAttr&>(body);
                for (core::StackMapTableAttr::Frame& f : b.frames) {
                    for (core::StackMapTableAttr::VType& v : f.locals) {
                        if (v.tag == core::StackMapTableAttr::VType::kObject) {
                            const ca::u32 n = remap(v.cp);
                            if (n != v.cp) {
                                v.cp    = n;
                                touched = true;
                            }
                        }
                    }
                    for (core::StackMapTableAttr::VType& v : f.stack) {
                        if (v.tag == core::StackMapTableAttr::VType::kObject) {
                            const ca::u32 n = remap(v.cp);
                            if (n != v.cp) {
                                v.cp    = n;
                                touched = true;
                            }
                        }
                    }
                }
                break;
            }
            case AttrKind::Annotations: {
                auto& b = static_cast<core::AnnotationsAttr&>(body);
                for (core::AnnotationsAttr::Annotation& a : b.annos) {
                    applyAnnotation(a, touched, *this);
                }
                break;
            }
            case AttrKind::LocalVariableTable: {
                auto& b = static_cast<core::LocalVariableTableAttr&>(body);
                for (core::LocalVariableTableAttr::Row& row : b.rows) {
                    const ca::u32 n1 = remap(row.nameCp);
                    if (n1 != row.nameCp) {
                        row.nameCp = n1;
                        touched    = true;
                    }
                    const ca::u32 n2 = remap(row.descCp);
                    if (n2 != row.descCp) {
                        row.descCp = n2;
                        touched    = true;
                    }
                }
                break;
            }
            case AttrKind::Signature: {
                auto& b = static_cast<core::SignatureAttr&>(body);
                const ca::u32 n = remap(b.signatureCp);
                if (n != b.signatureCp) {
                    b.signatureCp = n;
                    touched       = true;
                }
                break;
            }
            case AttrKind::SourceFile: {
                auto& b = static_cast<core::SourceFileAttr&>(body);
                const ca::u32 n = remap(b.sourceFileCp);
                if (n != b.sourceFileCp) {
                    b.sourceFileCp = n;
                    touched       = true;
                }
                break;
            }
            case AttrKind::InnerClasses: {
                // 行三 CP 槽重映射；access_flags 是访问位不动。
                auto& b = static_cast<core::InnerClassesAttr&>(body);
                for (core::InnerClassesAttr::Row& row : b.rows) {
                    const ca::u32 n1 = remap(row.innerClassInfoCp);
                    if (n1 != row.innerClassInfoCp) {
                        row.innerClassInfoCp = n1;
                        touched              = true;
                    }
                    const ca::u32 n2 = remap(row.outerClassInfoCp);
                    if (n2 != row.outerClassInfoCp) {
                        row.outerClassInfoCp = n2;
                        touched              = true;
                    }
                    const ca::u32 n3 = remap(row.innerNameCp);
                    if (n3 != row.innerNameCp) {
                        row.innerNameCp = n3;
                        touched         = true;
                    }
                }
                break;
            }
            case AttrKind::EnclosingMethod: {
                // 两槽重映射；methodNatCp==0 由 remap 的 0 哨兵穿透。
                auto& b = static_cast<core::EnclosingMethodAttr&>(body);
                const ca::u32 n1 = remap(b.classCp);
                if (n1 != b.classCp) {
                    b.classCp = n1;
                    touched   = true;
                }
                const ca::u32 n2 = remap(b.methodNatCp);
                if (n2 != b.methodNatCp) {
                    b.methodNatCp = n2;
                    touched       = true;
                }
                break;
            }
            case AttrKind::Exceptions: {
                // 全槽 CP Class 重映射。
                auto& b = static_cast<core::ExceptionsAttr&>(body);
                for (ca::u32& cp : b.exceptionCps) {
                    const ca::u32 n = remap(cp);
                    if (n != cp) {
                        cp      = n;
                        touched = true;
                    }
                }
                break;
            }
            case AttrKind::TypeAnnotations: {
                auto& b = static_cast<core::TypeAnnotationsAttr&>(body);
                for (core::TypeAnnotationsAttr::Annotation& a : b.annotations) {
                    const ca::u32 n = remap(a.typeIndexCp);
                    if (n != a.typeIndexCp) {
                        a.typeIndexCp = n;
                        touched       = true;
                    }
                    for (auto& kv : a.values) {
                        const ca::u32 nn = remap(kv.first);
                        if (nn != kv.first) {
                            kv.first = nn;
                            touched  = true;
                        }
                        applyElemValue(kv.second, touched, *this);
                    }
                }
                break;
            }
            case AttrKind::Raw:
                touched |= applyRaw(slot, rawName);
                break;
            case AttrKind::LineNumberTable:
            case AttrKind::CharacterRangeTable:
                break;   // 体不含 CP 引用
            default:
                break;
        }
        if (touched) {
            slot.mutate();
        }
        return touched;
    }

    bool applyRaw(AttrSlot& slot, std::string_view name)
    {
        auto& raw = static_cast<core::RawAttr&>(*slot.body);
        if (raw.bytes().empty()) {
            return false;   // 零长透传：体不动
        }
        const Result<std::vector<ca::u8>, CompactFailure> r =
            transcodeRawBody(name, ByteView{raw.bytes()},
                             [this](ca::u32 i) -> Result<ca::u32, CompactFailure> {
                                 return Ok(remap(i));
                             });
        if (r.is_err()) {
            return false;   // 计划相已校验；不可达
        }
        if (r.unwrap() == raw.bytes()) {
            return false;
        }
        raw.bytes() = std::move(r).unwrap();
        return true;
    }

    // ---- 状态 ----

    ClassSession& s_;
    ClassFile& cf_;
    const ConstantPool& old_;
    std::vector<ca::u32> map_;     ///< 旧池逐槽新索引；0 = 死条目 / 占位
    std::vector<ca::u8> live_;     ///< 引用闭包标记（计划相第一步）
    std::vector<ca::u32> classOf_; ///< 内容等价类号（0 = 未归类）
    ca::u32 nextClass_ = 1;        ///< 等价类号分配器
    std::map<std::tuple<ca::u8, ca::u32, ca::u32>, ca::u32> classByKeyMap_;  ///< 复合/数值类号键
    std::map<std::string_view, ca::u32> utf8Class_;                          ///< Utf8 字节内容类号键
    std::map<ca::u32, ca::u32> classToNew_;  ///< 类号 → 代表的新索引
    std::map<ca::u32, ca::u32> classRep_;    ///< 类号 → 代表的老索引
    ConstantPool build_;                     ///< materialize 构造的新池
    std::map<CodeAttr*, std::vector<core::RawInsn>> insnPlan_;   ///< 解码缓存（标记相）
    std::map<CodeAttr*, std::vector<ca::u8>> codePlan_;          ///< Code 体重写字节
    bool codeEncodeFailed_ = false;          ///< fillCodeBodies 兜底哨兵（单调映射下不可达）
    std::string_view ctx_;   ///< 当前属性名（标记失败的定位上下文）
};

}   // namespace

const char* to_string(CompactError e) noexcept
{
    switch (e) {
        case CompactError::Ok: return "ok";
        case CompactError::AttrNotModeled: return "attribute outside the compact closed set";
        case CompactError::MalformedAttr: return "attribute body malformed for compaction";
        case CompactError::Internal: return "internal compaction failure";
    }
    return "unknown";
}

Result<void, CompactFailure> compactConstantPool(ClassSession& s)
{
    Compactor c(s, s.backing());
    if (const Result<void, CompactFailure> r = c.plan(); r.is_err()) {
        return r.unwrap_err();
    }
    if (const Result<void, CompactFailure> r = c.materialize(); r.is_err()) {
        return r.unwrap_err();
    }
    if (c.encodeFailed()) {
        // 单调映射下 CP1 域宽不变宽，构造上不可达；兜底显式失败不落半截。
        CompactFailure f;
        f.code = CompactError::Internal;
        return Err(f);
    }
    if (c.isNoop()) {
        return Ok();   // 池已紧凑：不换池、不改槽、不记账（§36.2 无变化早退）
    }
    c.apply();
    // 换池：旧池退居会话侧（§35.2 文本视图存活期不被压缩破坏）。
    s.retiredPools_.push_back(std::move(s.backing().cp));
    s.backing().cp = c.takeNewPool();
    // 语义 intern 索引缓存的是旧池索引，强制重建。
    s.sem_.reset();
    if (!c.poolSelfConsistent()) {
        // 内部一致性网（计划/应用分派不一致的实现缺陷在这里显式爆掉）。
        CompactFailure f;
        f.code = CompactError::Internal;
        return Err(f);
    }
    // 记账（§36.3）：类级实改 → 消费者按「重扫整类」失效。
    ChangeRecord r;
    r.kind = ChangeKind::ClassMetadata;
    s.noteChange(r);
    return Ok();
}

}   // namespace jbc::ir
