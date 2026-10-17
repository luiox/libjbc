-- libjbc：node-first JVM 字节码统一建模库（架构设计见 doc_ai/design.md）。
-- 本文件是独立仓工程根：包仓接线（libca 经 luiox-repo git 直连）、构建
-- 开关、全局编译纪律（no-RTTI）与链接闭包助手，随后定义全部 target。
-- core 层：单类 lazy classfile DOM——骨架 eager（头 / CP / 成员元数据 /
-- 属性目录），属性内容 lazy（span + skip），保真写回：未触碰 copy 源
-- span，触碰才重编码；之上 ir（CodeDom + ClassSession）、analysis
-- （FrameAt / DomainEngine）、type（描述符/签名树）按层叠加。

set_project("libjbc")
set_version("0.4.15")
set_license("MIT")
set_languages("cxx17")

-- 中心包仓（github.com/luiox/luiox-repo）git 直连；公开 CI 走同一路径。
add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")

-- 构建开关：独立仓默认开测试；只构建库本体用 xmake f --with_tests=n。
option("with_tests")
    set_default(true)
    set_showmenu(true)
    set_description("Build libjbc_unittest (pulls gtest + test_helper).")
option_end()

local ca_modules = "core,str,log,crypto,zip"   -- luiox 家族统一模块超集（包消费图一致）
if has_config("with_tests") then
    add_requires("gtest")
    -- JDK 探测经中心包仓消费（源码仓 github.com/luiox/libjdk_finder）。
    add_requires("libjdk_finder 0.0.1")
    ca_modules = ca_modules .. ",test"          -- test_helper 的 ca::test 仅测试态需要
end
add_requires("libca 0.0.13", {configs = {modules = ca_modules}})

-- libca_* 目标名垫片：库内消费点写 add_deps("libca_core"/"libca_str"/
-- "libca_test")，由这里统一转发到 libca 包。
for _, m in ipairs({"core", "str", "test"}) do
target("libca_" .. m)
    set_kind("phony")
    add_packages("libca", {public = true})
target_end()
end

-- 链接闭包助手：显式列闭包，linux 进 linkgroups 免序，其余平台裸
-- add_links（MSVC link.exe 对库顺序不敏感，且其工具模块无 linkgroup 映射）。
function binary_link_closure(...)
    local libs = {...}
    local unpack = table.unpack or unpack
    add_links(unpack(libs))
    if is_plat("linux") then
        add_linkgroups(unpack(libs))
    end
end

-- 禁用 RTTI：所有 dynamic_cast 替换为 tag_cast 体系。
if is_plat("windows") then
    add_cxflags("/GR-", {tools = "cl"})
    add_cxflags("/EHa", {tools = "cl"})
    add_cxflags("/wd4541", {tools = "cl"})
    set_exceptions("none")
else
    add_cxxflags("-fno-rtti")
end

if is_host("windows") then
    set_plat("windows")
    set_toolchains("msvc")
end

-- 测试基建子工程（libjdk_finder 经包消费，见上方 add_requires）。
includes("test_helper")

target("libjbc")
    set_kind("static")
    set_group("libs")

    add_deps("libca_core", "libca_str")
    add_files("src/jbc/core/*.cpp", "src/jbc/ir/*.cpp", "src/jbc/analysis/*.cpp",
              "src/jbc/type/*.cpp")
    add_headerfiles("src/jbc/(**.hpp)")
    add_includedirs("src", {public = true})

    set_languages("cxx17")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

-- gtest 面随 with_tests 门控：默认配置不定义本 target、不拉 gtest。
if has_config("with_tests") then
target("libjbc_unittest")
    set_kind("binary")
    set_group("libs/test")
    set_default(false)

    add_deps("libjbc", "test_helper")
    -- 显式列出完整闭包（xmake 3.x 不会自动链静态依赖的二级传递闭包）。经
    -- binary_link_closure：windows 裸 add_links 行为不变，linux 进
    -- linkgroups 免序——test_helper 的二级依赖 libca_test/libjdk_finder 在
    -- GNU ld 单遍扫描下必须同组才能解析（linux 链接期 undefined ca::test::*）。
    binary_link_closure("libjbc", "test_helper", "libca_test", "libjdk_finder")
    add_packages("gtest")
    add_files("test/unittest/main.cpp")
    add_files("test/unittest/constant_pool_test.cpp")
    add_files("test/unittest/reader_test.cpp")
    add_files("test/unittest/malformed_test.cpp")
    add_files("test/unittest/roundtrip_test.cpp")
    add_files("test/unittest/lazy_test.cpp")
    add_files("test/unittest/real_class_test.cpp")
    add_files("test/unittest/leak_test.cpp")
    add_files("test/unittest/insn_test.cpp")
    add_files("test/unittest/attr_extra_test.cpp")
    add_files("test/unittest/ir_test.cpp")
    add_files("test/unittest/frame_test.cpp")
    add_files("test/unittest/strict_frame_test.cpp")
    add_files("test/unittest/subroutine_test.cpp")
    add_files("test/unittest/smt_build_test.cpp")
    add_files("test/unittest/smt_read_test.cpp")
    add_files("test/unittest/operand_test.cpp")
    add_files("test/unittest/lifetime_test.cpp")
    add_files("test/unittest/ergonomics_test.cpp")
    add_files("test/unittest/domain_test.cpp")
    add_files("test/unittest/type_test.cpp")
    add_files("test/unittest/signature_test.cpp")
    add_files("test/unittest/sig_attr_test.cpp")
    add_files("test/unittest/type_annotation_attr_test.cpp")
    add_files("test/unittest/character_range_table_attr_test.cpp")
    add_files("test/unittest/source_file_attr_test.cpp")
    add_files("test/unittest/inner_classes_attr_test.cpp")
    add_files("test/unittest/enclosing_method_attr_test.cpp")
    add_files("test/unittest/exceptions_attr_test.cpp")
    add_files("test/unittest/struct_attrs_corpus_test.cpp")
    add_files("test/unittest/signature_corpus_test.cpp")
    add_files("test/unittest/signature_reflect_test.cpp")
    add_files("test/unittest/library_entry_test.cpp")
    add_files("test/unittest/rename_probe_test.cpp")
    add_files("test/unittest/member_ops_test.cpp")
    add_files("test/unittest/member_move_test.cpp")
    add_files("test/unittest/change_set_test.cpp")
    add_files("test/unittest/corpus_reloc_test.cpp")
    add_files("test/unittest/corpus_compact_test.cpp")
    add_files("test/unittest/cp_compact_jvm_test.cpp")
    add_files("test/unittest/cp_compact_test.cpp")
    add_files("test/unittest/no_visitor_gate_test.cpp")
    add_files("test/unittest/nest_attrs_test.cpp")
    add_files("test/unittest/record_attr_test.cpp")
    add_files("test/unittest/module_attrs_test.cpp")
    add_files("test/unittest/annotation_attrs_test.cpp")
    add_files("test/unittest/annotation_corpus_test.cpp")
    add_files("test/unittest/class_build_attrs_test.cpp")
    add_files("test/unittest/bsm_intern_test.cpp")
    add_files("test/unittest/change_set_test.cpp")
    add_files("test/unittest/character_range_table_attr_test.cpp")
    add_files("test/unittest/corpus_compact_test.cpp")
    add_files("test/unittest/corpus_reloc_test.cpp")
    add_files("test/unittest/cp_compact_jvm_test.cpp")
    add_files("test/unittest/cp_compact_test.cpp")
    add_files("test/unittest/enclosing_method_attr_test.cpp")
    add_files("test/unittest/exceptions_attr_test.cpp")
    add_files("test/unittest/inner_classes_attr_test.cpp")
    add_files("test/unittest/member_move_test.cpp")
    add_files("test/unittest/member_ops_test.cpp")
    add_files("test/unittest/source_file_attr_test.cpp")
    add_files("test/unittest/struct_attrs_corpus_test.cpp")
    add_files("test/unittest/subroutine_test.cpp")
    add_files("test/unittest/type_annotation_attr_test.cpp")
    add_includedirs("src")
    set_rundir("$(projectdir)")

    if is_plat("linux") then
        add_syslinks("pthread")
    end
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
end

-- 消费者视角可用性用例：扫描/变换/往返三件事当外部库做。
-- 同时是 doc_ai/ergonomics.md 摩擦点清单的证据来源。
target("libjbc_use_demo")
    set_kind("binary")
    set_group("libs/demo")
    set_default(false)

    add_deps("libjbc")
    binary_link_closure("libjbc", "libca_str", "libca_core")
    add_files("demo/use_case.cpp")
    add_includedirs("src")
    set_rundir("$(projectdir)")
    set_languages("cxx17")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

-- 控制流标签 API 演示：导入解析/setTarget 重定向/insertAtLabel（指令 + End
-- 边界）/erase 角色保护/lower 写回重读验证。自包含，无语料依赖。
-- 用法：xmake build libjbc_flow_demo && xmake run libjbc_flow_demo
target("libjbc_flow_demo")
    set_kind("binary")
    set_group("libs/demo")
    set_default(false)

    add_deps("libjbc")
    binary_link_closure("libjbc", "libca_str", "libca_core")
    add_files("demo/control_flow.cpp")
    add_includedirs("src")
    set_rundir("$(projectdir)")
    set_languages("cxx17")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

-- 轻量泄漏检测：只依赖 libjbc + libca，debug 下用 CRT 调试堆查泄漏。
-- 单独存在的原因：libjbc_unittest 依赖 gtest/test_helper，debug 全链重编太慢。
-- 用法：xmake f -m debug && xmake build libjbc_leak_check
target("libjbc_leak_check")
    set_kind("binary")
    set_group("libs/demo")
    set_default(false)

    add_deps("libjbc")
    binary_link_closure("libjbc", "libca_str", "libca_core")
    add_files("demo/leak_check.cpp")
    add_includedirs("src")
    set_rundir("$(projectdir)")
    set_languages("cxx17")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
