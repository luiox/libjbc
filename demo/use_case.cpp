/// @file        use_case.cpp
/// @brief       消费者视角可用性用例：把 libjbc 当外部库完成三件真实的事。
///
/// 任务 1（扫描）：语料统计——方法/异常表/invokedynamic 计数 + 帧自预言。
/// 任务 2（变换）：全语料方法入口插 nop → lower → 重读验证（指令数 +1、
///                maxStack 不变、字节增长统计）。
/// 任务 3（往返）：未变换类 writeClass 与原字节逐字节一致。
///
/// 本文件同时是 doc_ai/ergonomics.md 的证据来源：写它时遇到的每一处
/// 别扭都会记进摩擦点清单，不为了好看而绕开。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"

using namespace jbc::core;
using jbc::analysis::FrameEngine;
using jbc::analysis::FrameSummary;
using jbc::analysis::ShapeResolver;
using jbc::ir::CodeEditor;
using jbc::ir::makeSession;
using jbc::ir::promoteMethod;
using jbc::ir::lowerMethod;
using Bytes = std::vector<ca::u8>;

namespace {

const char* kCorpusRoot = "./local_class_corpus/";

std::vector<std::string> collectClassFiles()
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(kCorpusRoot, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
            out.push_back(it->path().string());
        }
    }
    if (out.empty()) {
        std::printf("corpus not found under %s (build private-corpus first)\n", kCorpusRoot);
    }
    return out;
}

Bytes slurpFile(const std::string& p)
{
    std::ifstream in(p.c_str(), std::ios::binary);
    if (!in) {
        return {};
    }
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// 读字节 → 会话。消费者最常走的入口，三步才能到 ClassSession。
std::shared_ptr<jbc::ir::ClassSession> openSession(const Bytes& bytes)
{
    auto r = readClass(ByteView{bytes});
    if (r.is_err()) {
        return nullptr;
    }
    auto rr  = std::move(r).unwrap();
    auto ses = makeSession(std::move(rr.file));
    return ses.is_ok() ? std::move(ses).unwrap() : nullptr;
}

}   // namespace

int main()
{
    const std::vector<std::string> files = collectClassFiles();
    if (files.empty()) {
        return 0;
    }

    // ---- 任务 1：扫描统计 ----
    ca::usize classes = 0;
    ca::usize methods = 0;
    ca::usize withExc = 0;
    ca::usize indy    = 0;
    ca::usize frameOk = 0;
    for (const std::string& path : files) {
        const Bytes bytes = slurpFile(path);
        if (bytes.empty()) {
            continue;
        }
        auto s = openSession(bytes);
        if (s == nullptr) {
            continue;
        }
        ++classes;
        for (ca::usize i = 0; i < s->unit().methods.size(); ++i) {
            auto p = promoteMethod(*s, i);
            if (p.is_err()) {
                continue;   // abstract/native：无 Code
            }
            ++methods;
            if (!p.unwrap()->exc.empty()) {
                ++withExc;
            }
            const jbc::ir::CodeDom& dom = p.unwrap()->dom;
            for (const jbc::ir::InsnId& id : dom.order()) {
                if (const jbc::ir::Insn* in = dom.peek(id); in != nullptr &&
                    in->op == Op::INVOKEDYNAMIC) {
                    ++indy;
                    break;
                }
            }
            ShapeResolver res;
            FrameEngine fe(s->backing().cp, res);
            if (fe.compute(*p.unwrap()).ok) {
                ++frameOk;
            }
        }
    }
    std::printf("[scan]   classes=%zu methods=%zu withExc=%zu indyMethods=%zu "
                "frameOk=%zu (%.1f%%)\n",
                classes, methods, withExc, indy, frameOk,
                methods != 0 ? 100.0 * static_cast<double>(frameOk) / static_cast<double>(methods)
                             : 0.0);

    // ---- 任务 2：变换（入口插桩）+ 重读验证 ----
    ca::usize edited        = 0;
    ca::usize verified      = 0;
    ca::usize rejected      = 0;   // relocation contract 拒绝（未建模嵌套属性等）
    ca::usize byteGrow      = 0;
    ca::usize expectedGrowth = 0;
    for (const std::string& path : files) {
        const Bytes bytes = slurpFile(path);
        if (bytes.empty()) {
            continue;
        }
        auto s = openSession(bytes);
        if (s == nullptr) {
            continue;
        }
        // 先快照各方法原始指令数（lower 会把编辑写回 backing，之后 re-promote
        // 拿到的是编辑后的尺寸——验证基准必须是编辑前的）。
        std::vector<ca::usize> origSizes(s->unit().methods.size(), 0);
        for (ca::usize i = 0; i < s->unit().methods.size(); ++i) {
            auto p = promoteMethod(*s, i);
            origSizes[i] = p.is_ok() ? p.unwrap()->dom.size() : 0;
        }
        // 语义层入口：字符串常量按内容去重 intern（每类一个 CP 条目），
        // 消费者不接触 Methodref→NaT→Utf8 布局，也不手工拼 addXxx。
        const ca::u32 marker = s->internString("private-corpus-marked");
        for (ca::usize i = 0; i < s->unit().methods.size(); ++i) {
            auto p = promoteMethod(*s, i);
            if (p.is_err()) {
                continue;
            }
            jbc::ir::CodeUnit& cu = *p.unwrap();
            CodeEditor ed(cu);
            // 入口插「ldc marker; pop」：带 CP 操作数的真实插桩（loadConst
            // 自动处理 LDC/LDC_W 域宽），栈中性；pc 后移两位——LNT/SMT/LVT
            // 由 relocation contract 自动重定向，帧形状保持不变。
            const auto first = cu.dom.order().empty() ? jbc::ir::InsnId{}
                                                      : cu.dom.order()[0];
            if (first == jbc::ir::InsnId{} ||
                ed.insertBefore(first, CodeEditor::loadConst(marker)).is_err()) {
                continue;
            }
            if (ed.insertAfter(cu.dom.order()[0], CodeEditor::simple(Op::POP)).is_err()) {
                continue;
            }
            ++expectedGrowth;
            if (lowerMethod(*s, i).is_err()) {
                ++rejected;   // 明确失败好过静默丢属性
                continue;
            }
            ++edited;
        }
        auto w = writeClass(s->backing());
        if (w.is_err()) {
            continue;
        }
        const Bytes out = std::move(w).unwrap();
        byteGrow += out.size() > bytes.size() ? out.size() - bytes.size() : 0;

        // 重读验证：每个被编辑方法指令数 +1、maxStack 不变。
        auto s2 = openSession(out);
        if (s2 == nullptr) {
            continue;
        }
        bool allOk = true;
        for (ca::usize i = 0; i < s2->unit().methods.size() && allOk; ++i) {
            auto p2 = promoteMethod(*s2, i);
            if (p2.is_err()) {
                continue;
            }
            if (origSizes[i] + 2 != p2.unwrap()->dom.size()) {
                allOk = false;
                break;
            }
            // 首条指令语义回到「标记字符串」——写读一致。
            const jbc::ir::Insn* f = p2.unwrap()->dom.peek(p2.unwrap()->dom.order()[0]);
            jbc::ir::ClassSession::ConstSem cv;
            if (f == nullptr || !s2->constOf(*f, cv) ||
                cv.kind != jbc::ir::ClassSession::ConstSem::Kind::String ||
                cv.str != "private-corpus-marked") {
                allOk = false;
                break;
            }
            ShapeResolver res;
            FrameEngine fe(s2->backing().cp, res);
            const FrameSummary sum = fe.compute(*p2.unwrap());
            if (!sum.ok || sum.maxStack > p2.unwrap()->maxStack) {
                allOk = false;
            }
        }
        if (allOk) {
            ++verified;
        }
    }
    std::printf("[edit]   methodsEdited=%zu (expect %zu) rejectedByContract=%zu "
                "classesVerified=%zu/%zu byteGrow=%zu\n",
                edited, expectedGrowth, rejected, verified, classes, byteGrow);

    // ---- 任务 3：未变换往返 ----
    ca::usize roundtrip = 0;
    for (const std::string& path : files) {
        const Bytes bytes = slurpFile(path);
        if (bytes.empty()) {
            continue;
        }
        auto s = openSession(bytes);
        if (s == nullptr) {
            continue;
        }
        auto w = writeClass(s->backing());
        if (w.is_err()) {
            continue;
        }
        const Bytes& out = w.unwrap();
        if (out.size() == bytes.size() &&
            std::equal(out.begin(), out.end(), bytes.begin())) {
            ++roundtrip;
        }
    }
    std::printf("[write]  byteIdentical=%zu/%zu\n", roundtrip, classes);

    return 0;
}
