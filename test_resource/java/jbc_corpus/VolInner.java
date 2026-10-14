// VolInner.java — libjbc 语料：内部类/静态嵌套/匿名类/局部类（this$0/access$ 合成成员 + InnerClasses 属性）。
package jbcx.corpus;

public class VolInner {
    private int outerField = 7;
    private static int staticField = 8;

    public VolInner() {
    }

    // ---- 非静态内部类（this$0 捕获外部实例）----
    final class Inner {
        int innerMethod(int v) { return v + outerField; }
        int viaOuter() { return VolInner.this.outerField * 2; }
        int outerMethod() { return outerAlloc(3); }
    }

    int outerAlloc(int v) { return v + 1; }

    Inner makeInner() { return new Inner(); }

    int useInner(int v) {
        Inner in = new Inner();
        return in.innerMethod(v) + in.viaOuter() + in.outerMethod();
    }

    // ---- 静态嵌套类 ----
    static final class Nested {
        int nestedMethod(int v) { return v + staticField; }
        static int nestedStatic(int v) { return v * staticField; }
    }

    static int useNested(int v) {
        Nested n = new Nested();
        return n.nestedMethod(v) + Nested.nestedStatic(v);
    }

    // ---- 匿名类（多实现形态 + access$ 合成访问器）----
    Runnable anonRunnable() {
        return new Runnable() {
            @Override public void run() { outerField += 1; }
        };
    }
    int useAnon() {
        Runnable r = anonRunnable();
        r.run();
        return outerField;
    }
    int useAnonMulti(int v) {
        Object o = new Object() {
            @Override public int hashCode() { return v * 3; }
            @Override public String toString() { return "anon" + v; }
        };
        return o.hashCode() + o.toString().length();
    }
    static Object anonExpr(int v) {
        return new Object() {
            @Override public String toString() { return "expr" + (v + staticField); }
        };
    }

    // ---- 局部类 ----
    int useLocal(int v) {
        class Local {
            int go(int x) { return x + outerField + staticField; }
        }
        Local l = new Local();
        return l.go(v);
    }
    static int useLocalStatic(int v) {
        class LocalStatic {
            int go(int x) { return x * staticField; }
        }
        return new LocalStatic().go(v);
    }

    // ---- 继承链上的内部类（invokespecial 外部类构造器）----
    static class OuterChild extends VolInner {
        int innerSum(int v) {
            Inner in = new Inner();
            return in.innerMethod(v) + in.viaOuter();
        }
    }

    static int useOuterChild(int v) {
        OuterChild c = new OuterChild();
        return c.outerAlloc(v) + c.innerSum(v);
    }

    // ---- 内部类互访（access$ 形态）----
    int readOther(VolInner other) { return other.outerField + staticField; }
    static int readStaticViaInstance(VolInner v) { return v.outerField + Nested.nestedStatic(2); }
    void writeOuter(int v) { this.outerField = v; }

    interface Handler {
        int handle(int v);
    }

    int dispatch(int v, Handler h) { return h.handle(v); }

    int useHandlerLambdaAndAnon(int v) {
        return dispatch(v, x -> x + 1) + dispatch(v, new Handler() {
            @Override public int handle(int x) { return x * 2; }
        });
    }
}
