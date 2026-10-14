// VolIface.java — libjbc 语料：接口默认方法/静态方法/常量 + 多实现冲突消解 + 桥方法。
package jbcx.corpus;

public final class VolIface {
    private VolIface() {
    }

    interface Sized {
        int size();

        // 默认方法（接口里有 Code 的方法）
        default int doubled() { return size() * 2; }
        default String describe() { return "size=" + size(); }
        default int sumWith(Sized o) { return size() + o.size(); }

        static int staticHelper(int v) { return v * 10; }
        static Sized empty() { return new Sized() { @Override public int size() { return 0; } }; }
    }

    interface Named {
        String name();

        default String tagged() { return "[" + name() + "]"; }
        default int nameLen() { return name().length(); }
    }

    // 单接口实现
    static final class ArraySized implements Sized {
        private final int[] data;
        ArraySized(int[] data) { this.data = data; }
        @Override public int size() { return data.length; }
    }

    // 多接口实现（default 全保留）
    static final class Both implements Sized, Named {
        private final String label;
        Both(String label) { this.label = label; }
        @Override public int size() { return label.length(); }
        @Override public String name() { return label; }
    }

    // default 覆写
    static final class Zero implements Sized {
        @Override public int size() { return 0; }
        @Override public int doubled() { return -1; }
    }

    // 菱形：两接口的 default 冲突，必须显式消解（invokeinterface + invokespecial 组合）
    interface Left { default String who() { return "left"; } }
    interface Right { default String who() { return "right"; } }

    static final class Diamond implements Left, Right {
        @Override public String who() { return Left.super.who() + "&" + Right.super.who(); }
    }

    static int useSized() {
        Sized a = new ArraySized(new int[3]);
        Sized b = Sized.empty();
        return a.size() + a.doubled() + a.describe().length() + a.sumWith(b)
            + Sized.staticHelper(2) + b.doubled();
    }
    static int useBoth() {
        Both x = new Both("abcd");
        return x.doubled() + x.describe().length() + x.tagged().length() + x.nameLen() + x.size();
    }
    static int useZero() {
        Zero z = new Zero();
        return z.size() + z.doubled() + z.sumWith(Sized.empty());
    }
    static int useDiamond() {
        Diamond d = new Diamond();
        return d.who().length() + ((Left) d).who().length();
    }
    static int dispatchSized(Sized s) { return s.doubled() + s.describe().length(); }
    static int dispatchBothShapes() {
        return dispatchSized(new ArraySized(new int[5])) + dispatchSized(new Both("xy"))
            + dispatchSized(new Zero());
    }

    // ---- 函数式接口 + lambda（indy 调用点落在接口方法上）----
    interface IntOp {
        int op(int v);
        default int twice(int v) { return op(op(v)); }
    }

    static int useIntOp(int v) {
        IntOp inc = x -> x + 1;
        IntOp dbl = x -> x * 2;
        return inc.twice(v) + dbl.twice(v) + inc.op(v) + dbl.op(v);
    }

    // ---- 接口继承（default 继承链）----
    interface Base3 {
        default int base() { return 1; }
    }
    interface Mid3 extends Base3 {
        default int mid() { return base() + 1; }
    }
    static final class Leaf3 implements Mid3 {
        int all() { return base() + mid(); }
    }
    static int useLeaf3() {
        Leaf3 l = new Leaf3();
        return l.all() + l.base() + l.mid();
    }
}
