/// @file        units.hpp
/// @brief       v0.3 高层单元：ClassSession（backing + IR）与方法级 promote/lower。
///
/// 形状来自 design.md §6/§13.2：ClassSession 持有低层 ClassFile（backing）与
/// ClassUnit（IR）；pass 只通过 CodeUnit/CodeEditor 改 IR；lower 把一个方法的
/// 变换写回 backing 的 Code 属性，并按 relocation contract 处理受 pc 影响
/// 的嵌套属性。指令的 CP 操作数在 v0.3 仍是 backing 常量池索引（session
/// 保证 backing 存活）；SymbolPool 物化名称/描述符，IR 侧字符串自持。
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "libca/core/result.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/change_set.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/error.hpp"
#include "jbc/ir/symbol_pool.hpp"

namespace jbc::ir {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

class ClassSession;
struct CompactFailure;
Result<void, CompactFailure> compactConstantPool(ClassSession&);

/// @brief 会话内方法身份：与方法当前在 ClassUnit::methods 中的下标解耦。
///
/// slot 是身份槽：会话构造期为初始成员按序分配 0..n-1；成员级增删后新成员
/// 优先复用已删槽位。generation 为删除/复用预留的 ABA 防线：删除成员时该槽
/// 的 generation 自增，此后旧句柄（携带旧代）结构性失配——查询面返回空/
/// 命名哨兵，变更面返回显式 Err，不存在「删后又活」的复活形态。调用方不应
/// 把 slot 当作 backing ClassFile 的物理位置。
struct MethodId
{
    ca::u32 owner = 0;
    ca::u32 slot = 0;
    ca::u32 generation = 0;

    bool operator==(const MethodId& o) const noexcept
    {
        return owner == o.owner && slot == o.slot && generation == o.generation;
    }
    bool operator!=(const MethodId& o) const noexcept { return !(*this == o); }
};

inline constexpr MethodId kNoMethodId{};

/// @brief 会话内字段身份：与 MethodId 同协议（owner 同属一个 ClassUnit，
///        slot/generation 语义一致，只是两族成员各自独立编号）。
struct FieldId
{
    ca::u32 owner = 0;
    ca::u32 slot = 0;
    ca::u32 generation = 0;

    bool operator==(const FieldId& o) const noexcept
    {
        return owner == o.owner && slot == o.slot && generation == o.generation;
    }
    bool operator!=(const FieldId& o) const noexcept { return !(*this == o); }
};

inline constexpr FieldId kNoFieldId{};

/// @brief 一个方法的指令级 IR（promote 的产物，lower 的输入）。
struct CodeUnit
{
    CodeDom dom;          ///< 指令序列（句柄域；分支/switch 目标是 LabelId）
    ca::u16 maxStack = 0;
    ca::u16 maxLocals = 0;

    /// @brief 异常表：边界全部使用 owner-scoped LabelId。
    ///
    /// `end` 绑定到方法末尾标签时表示 JVMS 的 `end_pc == code_length`；
    /// 不再用空 InsnId 同时表达“末尾”和“无效句柄”。
    struct Exc
    {
        LabelId start;
        LabelId end;
        LabelId handler;
        ca::u32 catchCp = 0;   ///< CP Class 索引，0 = any
    };
    std::vector<Exc> exc;

    /// 方法的稳定身份；lower 通过所属 MethodUnit 的 backingIndex 找物理成员。
    MethodId method;
};

/// @brief 方法骨架（名称/描述符经 SymbolPool 物化，代码按需 promote）。
struct MethodUnit
{
    MethodId id;
    SymbolId name;
    SymbolId desc;
    ca::u32 access = 0;
    /// backing ClassFile::methods 中的物理位置；它不是方法身份。
    ca::usize backingIndex = 0;
    std::unique_ptr<CodeUnit> code;   ///< promoteMethod 后非空
};

/// @brief 字段骨架（名称/描述符经 SymbolPool 物化，身份协议与 MethodUnit 同构）。
struct FieldUnit
{
    FieldId id;
    SymbolId name;
    SymbolId desc;
    ca::u32 access = 0;
    /// backing ClassFile::fields 中的物理位置；它不是字段身份。
    ca::usize backingIndex = 0;
};

/// @brief 「无此方法」哨兵：ClassUnit::findMethod 的未命中返回值。
///
/// 命名常量替代裸下标魔法值，与 kNoInsn（F5 结清）同一惯例——指针查询
/// 用 nullptr 表达未命中，下标查询用本哨兵。
inline constexpr ca::usize kNoMethod = static_cast<ca::usize>(-1);

/// @brief 「无此字段」哨兵：语义与 @ref kNoMethod 同构。
inline constexpr ca::usize kNoField = static_cast<ca::usize>(-1);

/// @brief 类骨架：this/super/接口/成员元数据，符号自持。
///
/// 两族成员（方法/字段）各自持有一套身份簿记：slot 发号（建骨架期按序、
/// 增删后优先复用已删槽位）与 generation 台账（删除自增，旧句柄结构性
/// 失配）。身份操作只动簿记；成员的挂入/摘除与 backing 同步由 ClassSession
/// 的 add/remove 入口负责——单元自己不碰低层模型。
class ClassUnit
{
public:
    /// @brief 建骨架期为第 slot 个方法登记身份（generation 0）。
    ///
    /// 仅由 ClassSession 构造期对初始成员按 0..n-1 逐个调用；此后新增走
    /// @ref nextMethodId。slot 重复登记按重复身份处理（簿记幂等推进）。
    MethodId makeMethodId(ca::u32 slot) noexcept
    {
        methodNextSlot_ = methodNextSlot_ > slot + 1 ? methodNextSlot_ : slot + 1;
        if (methodGen_.size() <= slot) {
            methodGen_.resize(slot + 1, 0);
        }
        return MethodId{owner_, slot, methodGen_[slot]};
    }

    /// @brief 为新方法分配身份：优先复用最小已删槽位（其 generation 已在
    ///        删除时自增），否则发新槽。仅由 ClassSession 的 add 入口调用。
    MethodId nextMethodId() noexcept
    {
        const ca::u32 slot = nextSlot(methodNextSlot_, methodFree_);
        if (methodGen_.size() <= slot) {
            methodGen_.resize(slot + 1, 0);
        }
        return MethodId{owner_, slot, methodGen_[slot]};
    }

    /// @brief 方法删除侧：回收身份槽并自增 generation——携带旧代的句柄自此
    ///        全部失配。仅由 ClassSession 的 remove 入口调用。
    void retireMethodId(MethodId id) noexcept
    {
        if (id.owner != owner_) {
            return;
        }
        if (methodGen_.size() <= id.slot) {
            methodGen_.resize(id.slot + 1, 0);
        }
        methodGen_[id.slot] += 1;
        methodFree_.push_back(id.slot);
    }

    /// @brief 字段侧三个身份入口：语义与方法侧逐条同构。
    FieldId makeFieldId(ca::u32 slot) noexcept
    {
        fieldNextSlot_ = fieldNextSlot_ > slot + 1 ? fieldNextSlot_ : slot + 1;
        if (fieldGen_.size() <= slot) {
            fieldGen_.resize(slot + 1, 0);
        }
        return FieldId{owner_, slot, fieldGen_[slot]};
    }
    FieldId nextFieldId() noexcept
    {
        const ca::u32 slot = nextSlot(fieldNextSlot_, fieldFree_);
        if (fieldGen_.size() <= slot) {
            fieldGen_.resize(slot + 1, 0);
        }
        return FieldId{owner_, slot, fieldGen_[slot]};
    }
    void retireFieldId(FieldId id) noexcept
    {
        if (id.owner != owner_) {
            return;
        }
        if (fieldGen_.size() <= id.slot) {
            fieldGen_.resize(id.slot + 1, 0);
        }
        fieldGen_[id.slot] += 1;
        fieldFree_.push_back(id.slot);
    }

    /// @brief 当前逻辑顺序位置对应的稳定方法身份；越界返回 kNoMethodId。
    MethodId methodId(ca::usize index) const noexcept
    {
        return index < methods.size() ? methods[index].id : kNoMethodId;
    }

    /// @brief 按稳定身份查找当前逻辑位置；未命中返回 kNoMethod。
    ca::usize indexOf(MethodId id) const noexcept
    {
        for (ca::usize i = 0; i < methods.size(); ++i) {
            if (methods[i].id == id) {
                return i;
            }
        }
        return kNoMethod;
    }

    MethodUnit* method(MethodId id) noexcept
    {
        const ca::usize i = indexOf(id);
        return i == kNoMethod ? nullptr : &methods[i];
    }
    const MethodUnit* method(MethodId id) const noexcept
    {
        const ca::usize i = indexOf(id);
        return i == kNoMethod ? nullptr : &methods[i];
    }

    /// @brief 当前逻辑顺序位置对应的稳定字段身份；越界返回 kNoFieldId。
    FieldId fieldId(ca::usize index) const noexcept
    {
        return index < fields.size() ? fields[index].id : kNoFieldId;
    }

    /// @brief 按稳定身份查找字段当前逻辑位置；未命中返回 kNoField。
    ca::usize indexOf(FieldId id) const noexcept
    {
        for (ca::usize i = 0; i < fields.size(); ++i) {
            if (fields[i].id == id) {
                return i;
            }
        }
        return kNoField;
    }

    FieldUnit* field(FieldId id) noexcept
    {
        const ca::usize i = indexOf(id);
        return i == kNoField ? nullptr : &fields[i];
    }
    const FieldUnit* field(FieldId id) const noexcept
    {
        const ca::usize i = indexOf(id);
        return i == kNoField ? nullptr : &fields[i];
    }

    SymbolPool& symbols() noexcept { return symbols_; }
    const SymbolPool& symbols() const noexcept { return symbols_; }

    /// @brief 按名字定位第一个方法（ergonomics F4）。
    ///
    /// 与 SymbolPool 物化文本逐字节比较（零分配，不做 UTF8 解码）；
    /// 同名重载命中下标最小者。前置条件：name 为字节视图（不要求以 NUL
    /// 结尾）。未命中返回 kNoMethod（探测式，不抛错）。返回下标可直接
    /// 喂 promoteMethod / lowerMethod。
    ca::usize findMethod(std::string_view name) const noexcept
    {
        for (ca::usize i = 0; i < methods.size(); ++i) {
            if (symbols_.str(methods[i].name) == name) {
                return i;
            }
        }
        return kNoMethod;
    }

    /// @brief 按名字定位稳定方法身份。
    MethodId findMethodId(std::string_view name) const noexcept
    {
        const ca::usize i = findMethod(name);
        return i == kNoMethod ? kNoMethodId : methods[i].id;
    }

    /// @brief 按名字 + 描述符定位第一个方法（重载分辨形态）。
    ///
    /// 错误语义同单参版；desc 不匹配时即使名字命中也继续扫——未命中
    /// 返回 kNoMethod。
    ca::usize findMethod(std::string_view name, std::string_view desc) const noexcept
    {
        for (ca::usize i = 0; i < methods.size(); ++i) {
            if (symbols_.str(methods[i].name) == name
                && symbols_.str(methods[i].desc) == desc) {
                return i;
            }
        }
        return kNoMethod;
    }

    /// @brief 按名字与描述符定位稳定方法身份。
    MethodId findMethodId(std::string_view name, std::string_view desc) const noexcept
    {
        const ca::usize i = findMethod(name, desc);
        return i == kNoMethod ? kNoMethodId : methods[i].id;
    }

    /// @brief 按名字定位第一个字段；语义与 findMethod 单参版同构。
    ca::usize findField(std::string_view name) const noexcept
    {
        for (ca::usize i = 0; i < fields.size(); ++i) {
            if (symbols_.str(fields[i].name) == name) {
                return i;
            }
        }
        return kNoField;
    }

    /// @brief 按名字定位稳定字段身份。
    FieldId findFieldId(std::string_view name) const noexcept
    {
        const ca::usize i = findField(name);
        return i == kNoField ? kNoFieldId : fields[i].id;
    }

    /// @brief 按名字 + 描述符定位第一个字段；语义与 findMethod 双参版同构。
    ca::usize findField(std::string_view name, std::string_view desc) const noexcept
    {
        for (ca::usize i = 0; i < fields.size(); ++i) {
            if (symbols_.str(fields[i].name) == name
                && symbols_.str(fields[i].desc) == desc) {
                return i;
            }
        }
        return kNoField;
    }

    /// @brief 按名字与描述符定位稳定字段身份。
    FieldId findFieldId(std::string_view name, std::string_view desc) const noexcept
    {
        const ca::usize i = findField(name, desc);
        return i == kNoField ? kNoFieldId : fields[i].id;
    }

    ca::u32 version = 0;
    ca::u32 access = 0;
    SymbolId thisClass;
    SymbolId superClass;
    std::vector<SymbolId> interfaces;
    std::vector<FieldUnit> fields;
    std::vector<MethodUnit> methods;

private:
    /// @brief 发号共用体：有已删槽位取最小者，否则取新槽。
    static ca::u32 nextSlot(ca::u32& next, std::vector<ca::u32>& free_) noexcept
    {
        if (free_.empty()) {
            return next++;
        }
        ca::usize best = 0;
        for (ca::usize i = 1; i < free_.size(); ++i) {
            if (free_[i] < free_[best]) {
                best = i;
            }
        }
        const ca::u32 slot = free_[best];
        free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(best));
        return slot;
    }

    ca::u32 owner_ = nextOwnerId();
    ca::u32 methodNextSlot_ = 0;
    std::vector<ca::u32> methodGen_;    ///< slot → 当前代（删除自增）
    std::vector<ca::u32> methodFree_;   ///< 已删方法槽位（复用队列）
    ca::u32 fieldNextSlot_ = 0;
    std::vector<ca::u32> fieldGen_;
    std::vector<ca::u32> fieldFree_;
    SymbolPool symbols_;
};

class SemIndex;
class MemberSemRef;
class ConstSemRef;
class ClassSemRef;

/// @brief 新增方法的种子（A3）：成员元数据 + 可选的空 Code 体。
///
/// name/desc 是字节视图（挂载点常量由会话 intern 进 backing CP，命中既有
/// 池不膨胀）；access 语义同 classfile 的 access_flags（u16 域，超界 Err）。
/// withCode=true 时随建一个空 Code 属性（maxStack/maxLocals 由种子给定），
/// promoteMethod 后经 CodeEditor 出活，lowerMethod 写回；abstract/native
/// 置 false（成员不带 Code 属性）。名字+描述符在会话内必须唯一（JVM 对
/// 重复成员是加载期错误，这里在挂入时挡掉）。
struct MethodSeed
{
    std::string_view name;
    std::string_view desc;
    ca::u32 access = 0;
    bool withCode = false;
    ca::u16 maxStack = 0;
    ca::u16 maxLocals = 0;
};

/// @brief 新增字段的种子（A4）：与 MethodSeed 同构，字段没有方法体。
struct FieldSeed
{
    std::string_view name;
    std::string_view desc;
    ca::u32 access = 0;
};

// ---------------------------------------------------------------------------
// 类构建挂载面的合成规格模型（attr_build 批 1：注解族 + 简单注解 + 通用挂载）。
//
// 与 MethodSeed/FieldSeed 同一输入形态：全部 string_view + 值语义，CP 落位由
// 会话语义 intern 完成（命中既有池不膨胀）。规格树由调用方构造，合成期被完整
// 消化进 typed 属性体——接口不返回 CP 索引视图，规格本身不需要跨调用存活。
// ---------------------------------------------------------------------------

/// @brief ConstantValue 的值规格：常态十族经 classfile 常量池同化后 map 到
///        五种 CP 形态（bool/byte/char/short → Integer，JVMS 4.7.2）。
struct ConstValueSpec
{
    enum class Kind : ca::u8 {
        Int, Bool, Byte, Char, Short,   // → CP Integer（num）
        Long,                           // → CP Long（num）
        Float,                          // → CP Float（fbits，位模式）
        Double,                         // → CP Double（dbits，位模式）
        String,                         // → CP String（str）
    };

    Kind kind = Kind::Int;
    ca::i64 num = 0;          ///< Int 数族 / Long 的值（Bool 0/1）
    ca::u32 fbits = 0;        ///< Float 位模式
    ca::u64 dbits = 0;        ///< Double 位模式
    std::string_view str;     ///< String 内容
};

/// @brief MethodParameters 的一行：参数名（空串 = 合成参数不指名，name_index 0）
///        与旗语位集（ACC_FINAL/SYNTHETIC/MANDATED，不透明 u16 原样承载）。
struct ParamNameRow
{
    std::string_view name;
    ca::u16 flags = 0;
};

struct SemAnnotation;   // 前置声明：ElemValue 的 '@' 槽引用（定义见下）

/// @brief 注解元素值的合成规格：JVMS 4.7.16.1 全 forms（const 十态 + 枚举 +
///        类字面量 + 嵌套注解 + 数组）。数值族按 JVM 语义窄化为 32/16/8 位
///        后落 CP Integer（与 @ref ConstValueSpec 同化口径），double 位模式
///        原样承接（JVMS 注解无 64 位整型截断问题——'J'/'D' 各走 Long/Double）。
struct SemElemValue
{
    enum class Form : ca::u8 {
        Int, Bool, Byte, Char, Short,   // 'B' 'C' 'I' 'S' 'Z'（'Z' → Integer 0/1）
        Long, Float, Double,            // 'J' 'F'(fbits) 'D'(dbits)
        String,                         // 's'
        Class,                          // 'c'（str = 类型描述符形态：基本类型/void
                                        //      用 "I"/"V"，类类型用 "Ljava/lang/String;"；
                                        //      落位是 Utf8 直索引，javac 同形——
                                        //      非 CONSTANT_Class 条目）
        Enum,                           // 'e'（enumType = 字段描述符形态 "Lx/Y;"
                                        //      str = 枚举常量名）
        Nested,                         // '@'（nested）
        Array,                          // '['（array）
    };

    Form form = Form::Int;
    ca::i64 num = 0;          ///< Int 数族 / Long 的值
    ca::u32 fbits = 0;        ///< Float 位模式
    ca::u64 dbits = 0;        ///< Double 位模式
    std::string_view str;     ///< String 内容 / 'c' 类型描述符 / 枚举常量名
    std::string_view enumType;          ///< 'e' 的 type_name（描述符形态，原样承接）
    std::shared_ptr<SemAnnotation> nested;   ///< '@'
    std::vector<SemElemValue> array;         ///< '['
};

/// @brief 一条注解的合成规格：类型描述符 + 元素对表（与 low-level
///        AnnotationsAttr::Annotation 同构，CP 槽经合成面 intern）。
struct SemAnnotation
{
    std::string_view typeDesc;   ///< 注解类型的字段描述符形态（"Lx/Y;"）
    std::vector<std::pair<std::string_view, SemElemValue>> values;
};

/// @brief 方法句柄合成规格（JVMS 4.4.8）：reference_kind 1..9，目标成员
///        owner（内部名 '/' 分隔）/name/desc。类型层一致性（kind 与成员
///        形态的匹配，如 invokeStatic 对 <init>）归上层并归 JVM 运行时证伪，
///        结构面只做 kind 值域闸（1..9 是 JVM 类解析即拒的垃圾）。
struct MethodHandleSpec
{
    ca::u16 referenceKind = 0;   ///< 1..9；域外 → Err(InvalidSeed)
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
};

/// @brief BSM 静态参数合成规格（JVMS 4.7.23 bootstrap_arguments，loadable
///        十族）。整型一律 CP Integer（bootstrap 参数无窄化语义，与注解
///        元素族的窄化不同账）；Dynamic 形态绑定既有 BSM 条目 + 名/型
///        （叶子平挂，无嵌套规格树——嵌套条件常量在池层经同形重用互达），
///        下标锚点校验与显式 internDynamic 同规则。
struct BsmArg
{
    enum class Kind : ca::u8 {
        Int, Long, Float, Double,   ///< num / fbits / dbits（同 ConstValueSpec 口径）
        String,                     ///< str 内容 → CONSTANT_String
        Class,                      ///< str 内部名 → CONSTANT_Class
        MethodType,                 ///< str 方法描述符 → CONSTANT_MethodType
        MethodHandle,               ///< handle
        Dynamic,                    ///< 条件常量（bsm + name + desc）
    };

    Kind kind = Kind::Int;
    ca::i64 num    = 0; ///< Int / Long 的值
    ca::u32 fbits  = 0; ///< Float 位模式
    ca::u64 dbits  = 0; ///< Double 位模式
    std::string_view str;      ///< String 内容 / Class 内部名 / MethodType 描述符
    MethodHandleSpec handle;   ///< Kind::MethodHandle
    ca::u32 bsm  = 0;          ///< Kind::Dynamic 的 bootstrap 条目下标
    std::string_view name;     ///< Kind::Dynamic 的 NameAndType 名
    std::string_view desc;     ///< Kind::Dynamic 的 NameAndType 描述符
};

/// @brief 从零建类的种子（A5）：空类起步的头四件 + 接口表。
///
/// 名字一律内部名（'/' 分隔）。version 打包同 ClassUnit::version：
/// (major<<16)|minor。superName 为空串表示不写 superClass 条目（索引 0，
/// JVMS 对 java/lang/Object 自身与部分老版本的合法形态；普通类应给
/// "java/lang/Object"）。CP 引导在会话构造期完成：this/super/接口的 Utf8
/// 与 Class 条目即时落位，backing 从第一刻起就是自洽（可写出的）模型。
struct NewClassSeed
{
    ca::u32 version = (52u << 16);   ///< (major<<16)|minor，缺省 Java 8
    ca::u32 access = 0x0021;         ///< ACC_PUBLIC | ACC_SUPER
    std::string_view thisName;       ///< 必须非空
    std::string_view superName;      ///< 空串 = superClass 0
    std::vector<std::string_view> interfaces;
};

/// @brief 单类变换会话：backing（低层 DOM）+ IR（高层单元）同生共死。
///
/// 除 promote/lower 外，会话还是指令 CP 操作数的**语义层**：读路径把裸
/// CP 索引解析成成员/类/常量的语义视图（`memberOf`/`classOf`/`constOf`），
/// 写路径把语义描述去重 intern 成 CP 索引（`internClass`/`internMethodRef`
/// 等，先查既有池、miss 才追加——遵循 CP 只追加契约）。消费者因此不需要
/// 理解 Methodref→NameAndType→Utf8 的两跳布局，也不需要手工拼条目。
///
/// 构造收口：保活句柄（promoteMethod / collectCodeUnits 的返回值）内部
/// 依赖 shared_from_this，会话必须以 shared_ptr 持有——构造函数私有，
/// 唯一入口是 @ref makeSession / @ref makeSessionFromBytes（design.md §24
/// 寿命接管）。
class ClassSession : public std::enable_shared_from_this<ClassSession>
{
public:
    ~ClassSession();   // SemIndex 完整类型在 operand.cpp

    core::ClassFile& backing() noexcept { return *backing_; }
    const core::ClassFile& backing() const noexcept { return *backing_; }
    ClassUnit& unit() noexcept { return unit_; }
    const ClassUnit& unit() const noexcept { return unit_; }

    // ---- ChangeSet 协议（E2，design.md §35）----

    /// @brief 本会话的变更台账（观察面，恒开启）：会话各变更通道在实改发生时
    ///        记账，消费者按粒度查询决定失效重建范围。
    const ChangeSet& changeSet() const noexcept { return changes_; }

    /// @brief 记账一条变更（库内通道：成员增删/元数据提交在成员函数内完成，
    ///        lowerMethod 等自由函数经此落账；消费者只读 @ref changeSet，
    ///        不伪造记录）。
    void noteChange(ChangeRecord r) { changes_.record(std::move(r)); }

    // ---- 指令操作数语义视图（读路径）----
    // 字符串视图指向模型内部（源字节副本 / CP arena），存活期 = 会话。
    // 操作数与 CP 不符（或该 op 无 CP 操作数）返回 false / 空视图，不抛错。

    /// @brief 成员引用语义：invoke 家族与字段访问族共用。
    struct MemberSem
    {
        enum class Kind : ca::u8 { Field, Method, InterfaceMethod, InvokeDynamic };
        Kind kind = Kind::Field;
        std::string_view owner;   ///< 类内部名（'/' 分隔）；indy 为空
        std::string_view name;
        std::string_view desc;    ///< 方法/字段描述符（indy 为调用点描述符）
    };

    /// @brief ldc 族常量语义。
    struct ConstSem
    {
        enum class Kind : ca::u8 {
            Int, Long, Float, Double, String, Class, MethodType, MethodHandle, Dynamic
        };
        Kind kind = Kind::Int;
        std::string_view str;   ///< String 内容 / Class 内部名 / MethodType 描述符
        ca::i64 num  = 0;       ///< Int / Long 的值
        ca::u64 bits = 0;       ///< Float / Double 的原始位
    };

    /// @brief 解析 invoke/get/put 族指令的成员引用；无 CP 操作数或引用
    ///        结构不符返回 false。
    bool memberOf(const Insn& in, MemberSem& out) const noexcept;

    /// @brief 解析 new/anewarray/checkcast/instanceof/multianewarray 的类名；
    ///        失败返回空。
    std::string_view classOf(const Insn& in) const noexcept;

    /// @brief 解析 ldc/ldc_w/ldc2_w 的常量；无 CP 操作数返回 false。
    bool constOf(const Insn& in, ConstSem& out) const noexcept;

    // ---- 语义改写（读写合一：改一条指令引用的成员/类/常量）----
    // 对齐 ASM 的 `insn.owner = "..."; insn.name = "...";`——一步改写，
    // 不再「memberOf 读出来 → intern 新的 → 手动赋 cp」三连。
    // op 族与 kind 不符（如对 getfield 调 setMember(Method)）返回 false，
    // 指令保持原样。

    /// @brief 改写 invoke/get/put 族指令引用的成员（按 kind 选 ref 形态）。
    bool setMember(Insn& in, MemberSem::Kind kind, std::string_view owner,
                   std::string_view name, std::string_view desc);

    /// @brief 改写 new/anewarray/checkcast/instanceof/multianewarray 的类。
    bool setClass(Insn& in, std::string_view internalName);

    /// @brief 改写 ldc 族指令装载的字符串常量；新索引超 u1 域自动升格
    ///        LDC→LDC_W（宽度陷阱在 setter 内挡掉，不留给调用方）。
    bool setString(Insn& in, std::string_view s);

    // ---- 保活孪生（跨作用域持有的默认形态；借用版视图见上）----

    /// @brief memberOf 的保活版：结果连同会话寿命一起交付，拷到哪里都安全。
    ///        无 CP 操作数或解析失败时 sem.name 为空。
    MemberSemRef memberSemRef(const Insn& in) const noexcept;
    /// @brief constOf 的保活版。
    ConstSemRef constSemRef(const Insn& in) const noexcept;
    /// @brief classOf 的保活版（失败时 name 为空）。
    ClassSemRef classSemRef(const Insn& in) const noexcept;

    // ---- 语义 intern（写路径：去重追加，miss 才分配新索引）----

    ca::u32 internClass(std::string_view internalName);
    ca::u32 internString(std::string_view s);
    ca::u32 internInteger(ca::i32 v);
    ca::u32 internFieldRef(std::string_view owner, std::string_view name,
                           std::string_view desc);
    ca::u32 internMethodRef(std::string_view owner, std::string_view name,
                            std::string_view desc);
    ca::u32 internInterfaceMethodRef(std::string_view owner, std::string_view name,
                                     std::string_view desc);

    // ---- 合成基座补充（attr_build 类构建挂载面）：同一 intern 纪律 ----

    /// @brief 裸 Utf8 intern：属性名/注解类型描述符/枚举名等合成面的基座。
    ca::u32 internUtf8(std::string_view s);
    /// @brief 常量四件：Long/Float/Double 无去重 intern（位模式原样承载）。
    ca::u32 internLong(ca::i64 v);
    ca::u32 internFloatBits(ca::u32 bits);
    ca::u32 internDoubleBits(ca::u64 bits);

    // ---- BSM/条件常量族（批 2）：同一条去重纪律，带域校验的返回 Result ----

    /// @brief 方法句柄 intern（JVMS 4.4.8）：reference_kind 1..9 按 kind 分流
    ///        Fieldref/Methodref/InterfaceMethodref 三形态 + NameAndType。
    ///        命中既有句柄（kind+目标元）不追加；kind 域外 Err(InvalidSeed)。
    Result<ca::u32, LowerError> internMethodHandle(const MethodHandleSpec& h);
    /// @brief MethodType intern：方法描述符 → CONSTANT_MethodType 条目。
    ca::u32 internMethodType(std::string_view desc);
    /// @brief Module/Package intern（JVMS 4.4.12/4.4.11）。两者只在
    ///        module-info 类面合法——成员性约束归上层，本面只承载。
    ca::u32 internModule(std::string_view name);
    ca::u32 internPackage(std::string_view name);
    /// @brief 条件常量 intern（JVMS 4.4.10 CONSTANT_Dynamic）：绑定既有
    ///        BSM 条目下标 + NameAndType(name, desc)。BSM 属性缺位或下标
    ///        越界（JVM 类解析即拒 BSM-dependent 形态）一律 Err(InvalidSeed)。
    Result<ca::u32, LowerError> internDynamic(ca::u32 bsmIndex, std::string_view name,
                                              std::string_view desc);

    /// @brief 将指定方法的 IR 元数据提交到 backing 的 method_info。
    ///
    /// 方法名、描述符和访问标志属于 ClassUnit 的高层状态；提交入口负责把
    /// SymbolId 解析为 CP Utf8 索引并执行 u2 容量检查。lowerMethod 会自动调用
    /// 此入口，因此只改元数据而不改指令也不会静默丢失。
    /// 句柄无效（已删除/来自另一会话）返回 Err(InvalidHandle)。
    Result<void, LowerError> commitMethodMetadata(MethodId method);

    /// @brief 将指定字段的 IR 元数据提交到 backing 的 field_info（A4）。
    ///
    /// 与 commitMethodMetadata 同构：字段名/描述符/访问标志的高层改动以本
    /// 入口为唯一落盘通道（显式调用出活，无自动点）。句柄无效返回
    /// Err(InvalidHandle)。
    Result<void, LowerError> commitFieldMetadata(FieldId field);

    /// @brief 新增方法（A3）：按种子挂入元数据（withCode 时含空 Code 体）。
    ///
    /// 名字/描述符 intern 进 backing CP（命中既有池不膨胀，CP 只追加）；
    /// backing methods 追加一条 method_info，unit 挂入 MethodUnit 并分配
    /// 身份（优先复用已删槽位）。先预检后写入：CP 容量、u16 域、成员数、
    /// 名字+描述符唯一性，失败不留半截成员。
    Result<MethodId, LowerError> addMethod(const MethodSeed& seed);

    /// @brief 新增字段（A4）：按种子挂入元数据。预检与 intern 纪律同 addMethod。
    Result<FieldId, LowerError> addField(const FieldSeed& seed);

    /// @brief 删除方法（A3）：摘除 method_info 与 MethodUnit，回收身份槽。
    ///
    /// 失效语义：被删方法的 MethodId 立即失配（查询面 nullptr/kNoMethod，
    /// 变更面 Err(InvalidHandle)）；该槽 generation 自增，此后 addMethod
    /// 复用此槽时新身份携带新代——旧句柄不会复活。其余方法身份不变，
    /// backingIndex（物理位置）由本入口统一平移维护；逻辑下标视图
    /// （promoteMethod(s, idx) 等）此后指向新顺序。若该方法已 promote，
    /// 其 CodeUnit 随之销毁——消费者手里指向它的 CodeUnitRef 同步作废
    /// （与 CodeDom::erase 后旧 Insn* 作废同一纪律）。
    Result<void, LowerError> removeMethod(MethodId method);

    /// @brief 删除字段（A4）：语义与 removeMethod 同构（身份回收 + 物理平移）。
    Result<void, LowerError> removeField(FieldId field);

    /// @brief 跨类搬移（A3 的接收形态）：把另一会话的方法挂入本会话。
    ///
    /// 名字/描述符/方法体随迁，迁移语义见 design.md §34：
    /// - 挂载点常量 intern 进本会话 CP；方法体经 IR 重放随迁——指令的 CP
    ///   操作数按源会话语义视图读出、按本会话语义 intern 写入（引用结构
    ///   Methodref/NameAndType/Utf8 三跳在本会话池内重建）；分支/switch/异常
    ///   表经标签映射重建；maxes 随体拷贝。
    /// - 本会话新建的成员是「元数据 + 重放体」：源成员的非 Code 属性
    ///   （LNT/LVT/SMT/注解/Exceptions 等）不随迁；分支体在 v51+ 目标上
    ///   需调用方随后用 buildStackMapTable 再生帧（见契约挂账）。
    /// - ldc 的 MethodHandle/MethodType/Dynamic/Long/Float/Double 与
    ///   invokedynamic 调用点显式 Err(NotModeled)——不猜、不静默降级。
    /// - 源方法身份无效返回 Err(InvalidHandle)；源方法无体（abstract/
    ///   native）合法，产物同为无体成员。
    Result<MethodId, LowerError> adoptMethod(ClassSession& src, MethodId method);

    /// @brief 跨类搬移（A4 的接收形态）：把另一会话的字段挂入本会话。
    ///
    /// 字段只有元数据面：名字/描述符 intern 进本会话 CP；源字段的属性表
    /// （ConstantValue/Signature/注解）不随迁（同 §34 契约）。
    Result<FieldId, LowerError> adoptField(const ClassSession& src, FieldId field);

    /// @brief 将类级元数据提交到 backing（与 commitMethodMetadata 对称的类级面）。
    ///
    /// ClassUnit 的 version/access/thisClass/superClass/interfaces 在建会话时
    /// 从 backing 复制，直接改写这些字段不会自动落到 backing——本入口是唯一
    /// 提交通道（显式调用出活，无 lower 时机的类级自动点）。类名经 internClass
    /// 去重落位（CP 只追加），version 按 (major<<16|minor) 拆回头两个字段；
    /// superClass 空名提交为 0（= java/lang/Object）。
    ///
    /// 失败语义：access 超 u16、this 名为空、CP 追加超容量一律
    /// Err(CpAppendFailed)，先预检后写入，不留半截追加。
    Result<void, LowerError> commitClassMetadata();

    // ---- 类构建挂载面（attr_build 批 1：注解族合成 + 通用挂载，A7 定形）----
    //
    // 挂载语义（全 API 一致）：
    // - **只挂不改**：槽上已有同名属性时 Err(AlreadyMounted)——替换语义走
    //   既有 decodeAttr + mutate() 通路，本家族不造第二条修改通道。
    // - **单例规则**：LNT/LVT/LVTT 允许多条；其余（含未知名）至多一条。
    // - **域宽/容量先算后写**：u1 行数域、CP 追加数在触碰模型前预检，失败
    //   不留半截挂载（CP 无新增、属性表无新增）。
    // - **记入 ChangeSet**：成员级挂载记 MemberMetadata，类级记 ClassMetadata。

    /// @brief 字段恒值（JVMS 4.7.2，字段级单例）。值按 @ref ConstValueSpec
    ///        同化进 CP 五形态；类型层一致性（槽形态 vs 字段描述符）归上层
    ///        语义，本面只承载。
    Result<void, LowerError> attachConstantValue(FieldId field, const ConstValueSpec& v);

    /// @brief 方法参数名/旗语表（JVMS 4.7.24，方法级单例）。行数超 u1 域
    ///        Err(DomainOverflow)；名字 intern，空串行 name_index 0。
    Result<void, LowerError> attachMethodParameters(MethodId method,
                                                    const std::vector<ParamNameRow>& rows);

    /// @brief 方法参数注解（JVMS 4.7.18/4.7.19，方法级；visible/invisible 各
    ///        至多一条）。外层下标 = 参数槽位，空内层表 = 无注解参数。
    Result<void, LowerError> attachParameterAnnotations(
        MethodId method, bool visible, const std::vector<std::vector<SemAnnotation>>& params);

    /// @brief 注解默认值（JVMS 4.7.22，方法级单例，@interface 方法专用）。
    Result<void, LowerError> attachAnnotationDefault(MethodId method, const SemElemValue& v);

    /// @brief 简单注解（JVMS 4.7.16/4.7.17；方法级；visible/invisible 各至多
    ///        一条）。类/字段级同名重载照挂载点语义同构。
    Result<void, LowerError> attachAnnotations(MethodId method, bool visible,
                                               const std::vector<SemAnnotation>& annos);
    Result<void, LowerError> attachAnnotations(FieldId field, bool visible,
                                               const std::vector<SemAnnotation>& annos);
    Result<void, LowerError> attachClassAnnotations(bool visible,
                                                    const std::vector<SemAnnotation>& annos);

    /// @brief 通用挂载（A7 定形）：给成员挂一个**调用方已构造**的 typed 体。
    ///
    /// 体上的 CP 槽由调用方先经语义 intern 家族落位；本面负责挂载点校验
    /// （Code 一律拒绝——方法体归 promote/lower 管径）、单例规则、属性名
    /// intern 与容量预检。带手写体的下面三条是注解族合成 API 的展开形态。
    Result<void, LowerError> attachAttr(MethodId method, std::string_view attrName,
                                        std::unique_ptr<core::AttrBody> body);
    Result<void, LowerError> attachAttr(FieldId field, std::string_view attrName,
                                        std::unique_ptr<core::AttrBody> body);
    Result<void, LowerError> attachClassAttr(std::string_view attrName,
                                             std::unique_ptr<core::AttrBody> body);

    // ---- BSM 属性面（批 2）：类级 BootstrapMethods 的语义构建 ----
    //
    // - **条目复用**：methodRef + 静态参数组全字段一致的既有条目直接命中
    //   复用（JVMS 对同形条目无可辨差异，javac/ASM 同思路合并）；
    // - **类级单例**：BootstrapMethods 至多一条，缺则建（"先算后写"纪律
    //   与注解族同一执行序）；
    // - **记入 ChangeSet**：实改（新建属性/追加条目）记 ClassMetadata，
    //   纯命中复用不记账。

    /// @brief 追加/复用一条 bootstrap 方法条目：句柄经 @ref internMethodHandle，
    ///        静态参数逐个按 loadable 族落位；属性缺则建、条目同形则复用。
    ///        返回条目下标（0 基，即 invokedynamic/condy 的 bootstrap 索引）。
    Result<ca::u32, LowerError> appendBootstrapMethod(const MethodHandleSpec& handle,
                                                      const std::vector<BsmArg>& args);

private:
    // CP 压缩（A8，design.md §36）需要重建语义 intern 索引并把换池前的旧池
    // 移入退居存储（§35.2 文本视图存活期契约），是会话内部的通道。
    friend Result<void, CompactFailure> compactConstantPool(ClassSession&);

    // shared_from_this 要求会话生来就在 shared_ptr 里：构造只对 makeSession
    // 开放（友元），其余路径（栈上、裸 new、make_unique）一律编译期挡掉。
    friend Result<std::shared_ptr<ClassSession>, PromoteError>
        makeSession(std::unique_ptr<core::ClassFile> backing);
    explicit ClassSession(std::unique_ptr<core::ClassFile> backing);

    SemIndex& sem();   // 惰性构建的语义去重索引
    ca::u32 internUtf8Sem(std::string_view s);
    ca::u32 internRef(core::CpTag tag, std::string_view owner, std::string_view name,
                      std::string_view desc);

    // ---- attr_build.cpp 的合成/挂载内核（成员实现文件见该文件头注）----

    /// @brief 挂载预检：名字合法性 + Code 拒收 + 槽位域宽 + 单例规则 + 属性
    ///        名容量。不触碰模型（CP/属性表零效应），所有 attach 入口在任
    ///        何合成之前先过本闸，保证失败不留半截。
    Result<void, LowerError> mountPrecheck(std::vector<core::AttrSlot>& table,
                                           std::string_view attrName);
    /// @brief 挂载落位：属性名 intern + AttrSlot::makeNew 追加。预检通过后
    ///        调用，此处不再失败（makeNew 追加是纯构造）。
    Result<void, LowerError> mountAttr(std::vector<core::AttrSlot>& table,
                                       std::string_view attrName,
                                       std::unique_ptr<core::AttrBody> body);
    /// @brief attachAnnotations 三重载的共同底座：合成 + 挂载 + 记账。rec
    ///        为空表示类级无身份视图之外的记账（类级记录由入口构造）。
    Result<void, LowerError>
        attachAnnotationsToTable(std::vector<core::AttrSlot>& table, bool visible,
                                 const std::vector<SemAnnotation>& annos,
                                 std::optional<ChangeRecord> rec);
    /// @brief 注解规格 → low-level Annotation（元素递归合成，CP 经语义 intern）。
    Result<core::AnnotationsAttr::Annotation, LowerError>
        synthAnnotation(const SemAnnotation& a, int depth);
    /// @brief 元素规格 → low-level ElemValue（同上递归）。
    Result<core::AnnotationsAttr::ElemValue, LowerError> synthElem(const SemElemValue& e,
                                                                   int depth);
    /// @brief 规格树将产生的 CP 追加数（miss 才计数的预检遍历，build 前跑）。
    Result<ca::u32, LowerError> countAnnoAdditions(const SemAnnotation& a, int depth);
    Result<ca::u32, LowerError> countElemAdditions(const SemElemValue& e, int depth);

    std::unique_ptr<core::ClassFile> backing_;
    ClassUnit unit_;
    ChangeSet changes_;                     ///< E2 变更台账（§35，观察面不动模型）
    std::unique_ptr<class SemIndex> sem_;   // 语义去重索引（惰性构建）

    /// @brief 压缩换池后退居的旧常量池（A8，§36.3）。
    ///
    /// §35.2 的 ChangeSet 文本视图（改名记录的 prior 身份等）可能指向旧池
    /// arena 或源字节；压缩整体换池若直接销毁旧池，这些视图即悬垂。旧池
    /// 移入本处随会话同生共死，arena 块链整体移交，视图存活期契约不被
    /// 压缩破坏。
    std::vector<core::ConstantPool> retiredPools_;
};

const char* to_string(PromoteError e) noexcept;
const char* to_string(LowerError e) noexcept;

/// @brief 构建会话：解析骨架（this/super/接口/成员名与描述符物化进符号池）。
///
/// 返回 shared_ptr 是**寿命接管**的根（design.md §24）：保活句柄
/// （CodeUnitRef / *SemRef）内部各持一份，会话活到最后一个引用归零。
/// 共享的只是寿命，不是可变性——并发编辑依旧不被允许（单线程顺序模型）。
Result<std::shared_ptr<ClassSession>, PromoteError>
makeSession(std::unique_ptr<core::ClassFile> backing);

/// @brief 从零建会话（A5）：空类起步，不走既有 backing。
///
/// 与 take-ownership 版同参返回形态：会话以 shared_ptr 接管寿命。种子的
/// this/super/接口在构造期引导进 backing CP（Utf8 + Class 即时落位），成员
/// 面从零开始——后续成员用 addMethod/addField 挂入，类级元数据改动经
/// commitClassMetadata 提交。写回走既有 writeClass：全合成模型的每个条目
/// 都在「新编码」态，无源字节也可写出（writer 契约的从零分支）。
///
/// - 错误语义：this 名为空、名字超 u16、access 超 u16、接口数超 u16 一律
///   Err(InvalidSeed)，不产出半截会话。
/// - superName 空串 = superClass 索引 0（java/lang/Object 自身形态；普通
///   类应显式给 "java/lang/Object"）。
Result<std::shared_ptr<ClassSession>, PromoteError> makeSession(const NewClassSeed& seed);

/// @brief makeSessionFromBytes 的失败记录：哪一段失败 + 该段的错误码。
///
/// 一步入口横跨 core（readClass:ReadError）与 ir（makeSession:PromoteError）
/// 两个错误族；本结构如实保留「段 + 码」，不做跨族的有损映射，也不新增
/// 第六套错误枚举（ergonomics F3 的收敛裁决照旧）。
struct SessionError
{
    enum class Stage : ca::u8 {
        Read,     ///< readClass 失败（read 字段有效）
        Session,  ///< makeSession 失败（session 字段有效）
    };

    Stage stage = Stage::Read;
    core::ReadError read = core::ReadError::Ok;        ///< stage == Read 时有效
    PromoteError session = PromoteError::Ok;           ///< stage == Session 时有效
};

/// @brief 一步入口：class 字节 → 会话（ergonomics F2）。
///
/// 等价消化 readClass → 取 ReadResult.file → makeSession 的三步样板
/// （demo/测试此前重复 3 份）；产物与三步版完全一致，同样以 shared_ptr
/// 接管寿命（design.md §24）。
///
/// - 前置条件：bytes 在本调用期间保持有效（preserveSourceBytes 默认开启
///   时字节被复制进模型，返回后即可释放）。
/// - 所有权：成功时会话拥有独立模型；入参 buffer 不被持有。
/// - 错误语义：失败经 @ref SessionError 区分段与码；读段成功但解析降级
///   （ParseState::Recovered）不算失败，照常建会话——需要检查 ParseState
///   / diagnostics 的场合仍走三步版（本入口不透出诊断面）。
Result<std::shared_ptr<ClassSession>, SessionError>
makeSessionFromBytes(core::ByteView bytes,
                     const core::ReadOptions& opt = core::ReadOptions{});

/// @brief 保活视图：语义查询快照 + 会话保活副本。
///
/// 视图版（memberOf 等）借用会话寿命；本类型把寿命带在身上——消费者
/// 把查询结果存进容器、日志、跨线程队列都安全，会话活到最后一个引用
/// 归零（design.md §24 寿命接管）。字符串字节本身（CP 源副本/arena）
/// 在会话存活期内地址稳定，追加不失效。
class MemberSemRef
{
public:
    MemberSemRef() noexcept = default;
    MemberSemRef(std::shared_ptr<const ClassSession> keep, ClassSession::MemberSem sem) noexcept
        : keep_(std::move(keep))
        , sem(sem)
    {}

    ClassSession::MemberSem sem;

private:
    std::shared_ptr<const ClassSession> keep_;
};

/// @brief constOf 的保活版载荷。
class ConstSemRef
{
public:
    ConstSemRef() noexcept = default;
    ConstSemRef(std::shared_ptr<const ClassSession> keep, ClassSession::ConstSem c) noexcept
        : keep_(std::move(keep))
        , c(c)
    {}

    ClassSession::ConstSem c;

private:
    std::shared_ptr<const ClassSession> keep_;
};

/// @brief classOf 的保活版载荷。
class ClassSemRef
{
public:
    ClassSemRef() noexcept = default;
    ClassSemRef(std::shared_ptr<const ClassSession> keep, std::string_view name) noexcept
        : keep_(std::move(keep))
        , name(name)
    {}

    std::string_view name;

private:
    std::shared_ptr<const ClassSession> keep_;
};

/// @brief 保活句柄：CodeUnit 指针 + 会话 shared_ptr。
///
/// 拷到哪都安全——只要本对象存活，整个会话（backing + IR + 符号池）就
/// 存活。operator->/operator* 与裸指针同形，promoteMethod 的返回值从
/// CodeUnit* 换成本类型对调用方几乎透明。
class CodeUnitRef
{
public:
    CodeUnitRef() noexcept = default;
    CodeUnitRef(std::shared_ptr<ClassSession> keep, CodeUnit* u) noexcept
        : keep_(std::move(keep))
        , u_(u)
    {}

    CodeUnit* operator->() const noexcept { return u_; }
    CodeUnit& operator*() const noexcept { return *u_; }
    CodeUnit* get() const noexcept { return u_; }
    /// @brief 会话保活副本（消费者要建跨方法索引时持有它）。
    const std::shared_ptr<ClassSession>& session() const noexcept { return keep_; }
    explicit operator bool() const noexcept { return u_ != nullptr; }

private:
    std::shared_ptr<ClassSession> keep_;
    CodeUnit* u_ = nullptr;
};

/// @brief 方法级 promote：把 backing 第 methodIdx 个方法的 Code 属性升格为
///        指令级 IR（解码、导入、异常表句柄化）。
///
/// 无 Code 属性（abstract/native）返回 Err(NoCode)；重复 promote 幂等
/// （重建 CodeUnit）。指令 CP 操作数保持 backing 索引——session 存活期间有效。
/// 扫描场景请先用 @ref hasCode 把「无代码」分流掉，让本函数的 Err 只剩
/// 真失败（AttrNotDecoded / BadInstructions）——两者混在一个 `continue`
/// 里会把解码坏了的方法静默吞掉（ergonomics F1）。
Result<CodeUnitRef, PromoteError> promoteMethod(ClassSession& s, ca::usize methodIdx);

/// @brief 按稳定方法身份升格方法体。
Result<CodeUnitRef, PromoteError> promoteMethod(ClassSession& s, MethodId method);

/// @brief 方法级 lower：把 IR 写回 backing 的 Code 属性。
///
/// - 布局未变（导出字节与原 code 相同）且 maxes/异常表未变 → 不置脏，写回
///   走 copy-through。
/// - 布局改变时按 relocation contract 处理嵌套属性：LineNumberTable 按
///   pcmap 精确重定向（miss 即 Err）；StackMapTable 链式 offset 重算；
///   LocalVariableTable/LocalVariableTypeTable 行 start/length 按 fail-closed
///   终点规则重算；TypeAnnotations 携带偏移的目标（localvar 行与 offset/
///   cast/type-arg 族）按 pcmap 重定向、无 pc 目标原样保留；
///   CharacterRangeTable 行 start_pc/end_pc 精确重定向；其余未建模
///   嵌套属性一律 Err(RelocationNotModeled)——不静默丢、不猜。
Result<void, LowerError> lowerMethod(ClassSession& s, ca::usize methodIdx);

/// @brief 按稳定方法身份写回方法体。
Result<void, LowerError> lowerMethod(ClassSession& s, MethodId method);

/// @brief 查询第 methodIdx 个方法是否带 Code 属性（ergonomics F1）。
///
/// 把「方法没有代码」（abstract/native，预期形态）从 promote 的错误路径
/// 里拿出来：先用本查询分流，promoteMethod 的 Err 就只剩真失败。
///
/// - 前置条件：methodIdx < unit().methods.size()；越界返回 false（探测式，
///   与 CodeDom::peek 返回 nullptr 同款纪律，不抛错）。
/// - 所有权：只读查询，不改会话、不置脏、不解码属性内容。
/// - 错误语义：false 仅表示「此下标没有可升格的 Code 属性」；解码失败等
///   真失败不在此通道，留给 promoteMethod 的 Err。
bool hasCode(const ClassSession& s, ca::usize methodIdx) noexcept;

/// @brief collectCodeUnits 的单方法记录：三种互斥状态。
///
/// 设计目的是让「无 Code」与「真失败」不可混淆（ergonomics F1）：消费者
/// 想把 Failed 静默 continue，必须显式写下 Failed 分支才做得到。
struct CodeVisit
{
    enum class Kind : ca::u8 {
        NoCode,  ///< 方法没有 Code 属性（abstract/native）——预期形态
        Failed,  ///< promote 真失败（AttrNotDecoded / BadInstructions 等）
        Ok,      ///< 已升格，unit 有效
    };

    Kind kind = Kind::NoCode;
    ca::usize methodIdx = 0;
    PromoteError error = PromoteError::Ok;   ///< 仅 kind == Failed 时有效
    CodeUnitRef unit;                        ///< 仅 kind == Ok 时有效
};

/// @brief collectCodeUnits 的指令族（ergonomics F12：memberOf 一个入口返回
/// 五族，统计「调用」时字段访问会混入——查询侧按族过滤的便捷形态）。
enum class InsnFamily : ca::u8 {
    Invoke = 0,        ///< invoke 全家族（virtual/special/static/interface/indy）
    FieldAccess = 1,   ///< getstatic/putstatic/getfield/putfield
    CpConst = 2,       ///< ldc 族 + new 家族/checkcast/instanceof
    Branch = 3,        ///< 分支与 switch（含 jsr/ret 遗留形态）
};

/// @brief 指令所属族的掩码位（CodeCollectFilter 用）；不属任何族返回 0。
ca::u32 insnFamilyBit(core::Op op) noexcept;

/// @brief collectCodeUnits 的收录过滤器（ergonomics F12/D6）。
///
/// 两个维度独立生效（交集语义，维度掩码为 0 = 该维度不过滤）：
/// - @ref insnFamilies：方法体内至少含一条命中族的指令；
/// - @ref attrKinds：Code 属性嵌套属性中至少一个命中 core::AttrKind
///   （嵌套属性解码失败按未命中处理——解码失败的显式暴露在 promote 的
///   Failed 通道，本维度不做第二失败面）。
/// 全缺省（两掩码为 0 且 includeNoCode）= 不过滤，与无参版全量语义逐位
/// 一致。Failed 记录恒保留——F1 纪律：真失败不提供静默吞掉的通道。
struct CodeCollectFilter
{
    ca::u32 insnFamilies = 0;  ///< InsnFamily 位掩码（1u << int(family)）
    ca::u32 attrKinds = 0;     ///< core::AttrKind 位掩码（1u << int(kind)）
    bool includeNoCode = true; ///< 无 Code 方法（abstract/native）是否产出记录
};

/// @brief 按方法下标升序收集会话内全部方法的 Code 升格结果（ergonomics F1，
///        全量形态；按族过滤见带过滤器重载）。
///
/// - 形态：值返回（拉），不是回调推送——visitor 清零的机械判据是数据形态
///   （design.md §26：事件推给消费者即换皮）。每方法恰一条记录；提前退出
///   由调用方自己 break。
/// - 已 promote 的方法**直接复用**现有 CodeUnit，不重建——重建会使消费
///   者手里既有的 CodeUnitRef 失效（promoteMethod 的「幂等 = 重建」语义
///   在遍历场景是陷阱，这里刻意避开）；未 promote 的方法就地升格一次。
/// - 错误语义：三种 Kind 互斥；本函数自身不失败，升格失败落在
///   CodeVisit::Failed 里交由消费者处置——调用方仍须显式分支才能静默
///   跳过（F1 防静默吞错在拉形态下不回退）。
std::vector<CodeVisit> collectCodeUnits(ClassSession& s);

/// @brief 带族过滤的收集（ergonomics F12/D6）：只收录命中
///        @ref CodeCollectFilter 各维度的方法，记录形态与全量版同构。
///
/// 收录判定在升格之后进行（升格失败的 Failed 记录恒保留，见过滤器契约）；
/// 未命中的 Ok 方法不产出记录（输出 = 命中方法的下标升序序列）。
std::vector<CodeVisit> collectCodeUnits(ClassSession& s, const CodeCollectFilter& filter);

}   // namespace jbc::ir
