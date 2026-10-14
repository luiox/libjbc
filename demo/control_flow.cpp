/// @file        control_flow.cpp
/// @brief       控制流标签 API 演示：导入解析、setTarget 重定向、insertAtLabel
///              （指令边界重绑 + End 追加）、erase 角色保护、lower 写回重读验证。
///
/// 自包含：样例类是手工字节（goto 跳转 + 异常表 + LineNumberTable），
/// 不依赖任何本地语料，任何配置下可直接运行。
///
/// 用法：xmake build libjbc_flow_demo && xmake run libjbc_flow_demo
#include <cstdio>
#include <vector>

#include "jbc/jbc.hpp"

using namespace jbc::core;
using jbc::ir::ClassSession;
using jbc::ir::CodeEditor;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::makeSession;
using jbc::ir::promoteMethod;
using jbc::ir::lowerMethod;
using Bytes = std::vector<ca::u8>;

namespace {

/// p/T { public static m()V }：goto +6 → pc6 的 nop；异常表
/// {start=0, end=code_length, handler=pc3}；LNT 一行 {0→1}。
const Bytes& sampleClass()
{
    static const Bytes cls = {
        0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0x00, 0x34,
        0x00, 0x09,                                       // cpCount = 9
        0x01, 0x00, 0x03, 'p', '/', 'T',                 // 1: Utf8 "p/T"
        0x07, 0x00, 0x01,                                 // 2: Class #1
        0x01, 0x00, 0x10, 'j', 'a', 'v', 'a', '/', 'l', 'a', 'n', 'g', '/',
        'O', 'b', 'j', 'e', 'c', 't',                     // 3: Utf8 "java/lang/Object"
        0x07, 0x00, 0x03,                                 // 4: Class #3
        0x01, 0x00, 0x01, 'm',                            // 5: Utf8 "m"
        0x01, 0x00, 0x03, '(', ')', 'V',                  // 6: Utf8 "()V"
        0x01, 0x00, 0x04, 'C', 'o', 'd', 'e',             // 7: Utf8 "Code"
        0x01, 0x00, 0x0F, 'L', 'i', 'n', 'e', 'N', 'u', 'm', 'b', 'e', 'r',
        'T', 'a', 'b', 'l', 'e',                           // 8: Utf8 "LineNumberTable"
        0x00, 0x21,                                       // access: public super
        0x00, 0x02,                                       // this = #2
        0x00, 0x04,                                       // super = #4
        0x00, 0x00,                                       // interfaces = 0
        0x00, 0x00,                                       // fields = 0
        0x00, 0x01,                                       // methods = 1
        0x00, 0x09, 0x00, 0x05, 0x00, 0x06, 0x00, 0x01,   // m()V, 1 attr
        0x00, 0x07,                                       // attr name = Code
        0x00, 0x00, 0x00, 0x27,                           // attr len = 39
        0x00, 0x01, 0x00, 0x00,                           // maxStack=1, maxLocals=0
        0x00, 0x00, 0x00, 0x07,                           // code len = 7
        0xA7, 0x00, 0x06,                                 // pc0: goto +6 → pc6
        0x01,                                             // pc3: aconst_null（handler）
        0xB1,                                             // pc4: return
        0x00,                                             // pc5: nop
        0x00,                                             // pc6: nop（goto 目标）
        0x00, 0x01,                                       // exc count = 1
        0x00, 0x00, 0x00, 0x07, 0x00, 0x03, 0x00, 0x00,   // {0, 7, 3, any}
        0x00, 0x01,                                       // nested attrs = 1
        0x00, 0x08,                                       // LNT name = #8
        0x00, 0x00, 0x00, 0x06,                           // LNT len = 6
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01,               // {pc0 → line 1}
        0x00, 0x00,                                       // class attrs = 0
    };
    return cls;
}

bool ok(bool cond, const char* step)
{
    std::printf("[%s] %s\n", cond ? " ok" : "FAIL", step);
    return cond;
}

}   // namespace

int main()
{
    // ---- 1) 读 → 会话 → promote ----
    auto r = readClass(ByteView{sampleClass()});
    if (r.is_err()) {
        return 1;
    }
    auto s = makeSession(std::move(r).unwrap().file);
    if (s.is_err()) {
        return 1;
    }
    auto p = promoteMethod(*s.unwrap(), 0);
    if (p.is_err()) {
        return 1;
    }
    CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    std::printf("promoted: %zu insns, %zu exc entries\n", cu.dom.size(), cu.exc.size());

    // ---- 2) 导入解析：分支目标经标签解析；异常表 end 是 End 边界 ----
    const Insn* g = cu.dom.peek(cu.dom.order()[0]);
    auto tgt = cu.dom.resolveLabel(g->target);
    if (!ok(tgt.is_ok() && !tgt.unwrap().isEnd() &&
                cu.dom.peek(tgt.unwrap().insn)->op == Op::NOP,
            "goto 目标解析到 pc6 的 nop（导入为独立 ControlFlow 标签）")) {
        return 1;
    }
    auto end = cu.dom.resolveLabel(cu.exc[0].end);
    if (!ok(end.is_ok() && end.unwrap().isEnd(),
            "异常表 end（end_pc == code_length）解析为 End 一等边界")) {
        return 1;
    }

    // ---- 3) setTarget：goto 改指 return；旧目标随后可删（角色保护只挡 Boundary）----
    auto rl = cu.dom.createLabel(cu.dom.order()[2]);   // return 的 Boundary 标签
    if (!ok(rl.is_ok() && ed.setTarget(cu.dom.order()[0], rl.unwrap()).is_ok(),
            "setTarget 把 goto 改指 return（只改这一个字段）")) {
        return 1;
    }
    if (!ok(ed.erase(cu.dom.order()[4]).is_ok(),
            "删除旧目标 nop：遗留的旧控制流标签不再保护锚点")) {
        return 1;
    }

    // ---- 4) insertAtLabel：在 return 前插入，标签重绑到新指令 ----
    auto ins = ed.insertAtLabel(rl.unwrap(), CodeEditor::simple(Op::NOP));
    const auto rebound = cu.dom.resolveLabel(rl.unwrap());
    if (!ok(ins.is_ok() && rebound.is_ok() && !rebound.unwrap().isEnd() &&
                rebound.unwrap().insn == ins.unwrap(),
            "insertAtLabel 在 return 前插入 nop，标签重绑到新指令")) {
        return 1;
    }

    // ---- 5) End 插入 = 追加；End 语义保持 ----
    auto app = ed.insertAtLabel(cu.exc[0].end, CodeEditor::simple(Op::NOP));
    if (!ok(app.is_ok() && cu.dom.order().back() == app.unwrap() &&
                cu.dom.resolveLabel(cu.exc[0].end).unwrap().isEnd(),
            "insertAtLabel 命中 End 只追加指令，标签仍解析为方法末尾")) {
        return 1;
    }

    // ---- 6) erase 保护：handler 锚定的指令拒绝删除 ----
    if (!ok(ed.erase(cu.dom.order()[1]).unwrap_err() == jbc::ir::EditError::StillUsed,
            "删除 handler 锚定的 aconst_null 被拒绝（Boundary 标签保护）")) {
        return 1;
    }

    // ---- 7) lower → 写回 → 重读验证 ----
    if (!ok(lowerMethod(*s.unwrap(), 0).is_ok(), "lower 成功（异常表/LNT 随布局重定向）")) {
        return 1;
    }
    auto w = writeClass(s.unwrap()->backing());
    if (!ok(w.is_ok(), "writeClass 成功")) {
        return 1;
    }
    const Bytes out = std::move(w).unwrap();

    auto r2 = readClass(ByteView{out});
    if (r2.is_err()) {
        return 1;
    }
    auto s2 = makeSession(std::move(r2).unwrap().file);
    if (s2.is_err()) {
        return 1;
    }
    auto p2 = promoteMethod(*s2.unwrap(), 0);
    if (p2.is_err()) {
        return 1;
    }
    CodeUnit& cu2 = *p2.unwrap();
    const Insn* g2 = cu2.dom.peek(cu2.dom.order()[0]);
    auto t2 = cu2.dom.resolveLabel(g2->target);
    // rl 标签在 insertAtLabel 后重绑到 return 前插入的 nop（边界语义）：
    // goto 的语义目标 = 该边界 = nop，直落即 return。
    bool jumpOk = t2.is_ok() && !t2.unwrap().isEnd() &&
                  cu2.dom.peek(t2.unwrap().insn)->op == Op::NOP;
    bool fallthroughOk = false;
    if (jumpOk) {
        const auto& order2 = cu2.dom.order();
        for (ca::usize i = 0; i + 1 < order2.size(); ++i) {
            if (order2[i] == t2.unwrap().insn) {
                fallthroughOk = cu2.dom.peek(order2[i + 1])->op == Op::RETURN;
                break;
            }
        }
    }

    auto dc = s2.unwrap()->backing().decodeAttr(
        s2.unwrap()->backing().methods[0].attrs[0]);
    CodeAttr* code2 = dc.is_ok() ? static_cast<CodeAttr*>(dc.unwrap()) : nullptr;
    bool excOk = false;
    bool lntOk = false;
    if (code2 != nullptr && code2->excTable.size() == 1) {
        // 新布局：goto@0(3B) aconst_null@3 nop@4 return@5 nop@6 nop@7 → len 8。
        excOk = code2->excTable[0].endPc == code2->code.size() &&
                code2->excTable[0].startPc == 0 && code2->excTable[0].handlerPc == 3;
        if (!code2->attrs.empty()) {
            auto dl = s2.unwrap()->backing().decodeAttr(code2->attrs[0]);
            if (dl.is_ok()) {
                const auto& lnt = static_cast<const LineNumberTableAttr&>(*dl.unwrap());
                lntOk = lnt.rows.size() == 1 && lnt.rows[0].startPc == 0;
            }
        }
    }
    if (!ok(jumpOk && fallthroughOk,
            "重读：goto 语义目标是重绑后的边界（nop），直落 return") ||
        !ok(excOk, "重读：异常表 end_pc 跟随新 code 长度，start/handler 保持") ||
        !ok(lntOk, "重读：LNT 行 startPc 重定向正确")) {
        return 1;
    }
    std::printf("all control-flow invariants verified (%zu → %zu bytes)\n",
                sampleClass().size(), out.size());
    return 0;
}
