target("test_helper")
    set_kind("static")
    set_group("libs/test")
    -- 通用机制委托 ca::test（#912 下沉）；libjdk_finder 经包消费（public 传导
    -- 头路径：jdk_fixture.hpp 的消费方依赖其传递可见性）。
    add_deps("libca_test")
    add_packages("libjdk_finder", {public = true})
    add_files("test_helper.cpp")
    add_includedirs(".", {public = true})
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
