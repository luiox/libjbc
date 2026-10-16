/*
 * module 语料：全形态 Module 属性（JVMS 4.7.25）——requires 带 transitive /
 * static 旗语、exports 双形态（无限定 + to 限定）、opens、uses、provides...
 * with。javac 对 module-info 只认 --release 9+；ModulePackages 与
 * ModuleMainClass 在 javac 直译模式（含 --module-source-path 实测）不产，
 * 两件的 typed 面由 crafted 字节用例钉住，本语料不背其分布。
 */
module jbc.corpus.module {
    requires transitive java.logging;
    requires static java.desktop;
    exports svc;
    exports app to java.logging;
    opens impl;
    uses svc.Svc;
    provides svc.Svc with impl.SvcImpl;
}
