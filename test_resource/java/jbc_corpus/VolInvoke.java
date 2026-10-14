// VolInvoke.java — libjbc 语料：调用全族（invokevirtual/special/static/interface + 构造链 + varargs + 递归）。
package jbcx.corpus;

public final class VolInvoke {

    // ---- invokestatic（同类 / 跨类 / 库类）----
    static int sSame(int a) { return a + 1; }
    static int sChain(int a) { return sSame(sSame(a)) + sSame(a); }
    static int sLib(int a) { return Math.abs(a) + Math.max(a, 0); }
    static int sLib2(long a) { return Long.bitCount(a) + Integer.bitCount((int) a); }
    static int sLib3(String s) { return Integer.parseInt(s) + Integer.reverse(1); }
    static int sLib4(double d) { return (int) Math.round(d) + (int) Math.floor(d); }
    static int sVolField() { return VolField.getSf0() + VolField.staticSum(); }
    static int sCorpus() { return VolArith.iadd(1, 2) + VolBranch.eq(1, 1); }
    static native void nativeShape(int x);

    // ---- invokevirtual / invokespecial（实例方法与构造器）----
    int vSelf(int a) { return a * 2; }
    int vChain(int a) { return vSelf(vSelf(a)) + this.vSelf(a); }
    int vOther(VolField o) { return o.getF0() + o.sumAll(); }
    int vLib(String s) { return s.length() + s.hashCode() + s.charAt(0); }
    int vSb(int a) {
        StringBuilder sb = new StringBuilder();
        sb.append(a).append('-').append("x");
        return sb.length() + sb.hashCode();
    }
    int vObj(Object o) { return o.hashCode() + o.toString().length() + o.getClass().getName().length(); }
    int vCtorChain(int a) {
        VolField f = new VolField();
        f.setF0(a);
        StringBuilder sb = new StringBuilder(16);
        sb.append(a);
        return f.getF0() + sb.length();
    }
    int vCtorArg(int a) {
        Integer boxed = new Integer(a);
        return boxed.intValue() + boxed.compareTo(0);
    }
    int vExceptionCtor(String msg) {
        return new IllegalStateException(msg).getMessage().length();
    }

    // ---- 私有调用（invokespecial）----
    private int pHelper(int a) { return a ^ 0x55; }
    int pCaller(int a) { return pHelper(a) + pHelper(a + 1); }
    private static int psHelper(int a) { return a * 3; }
    static int psCaller(int a) { return psHelper(a) + psHelper(a - 1); }

    // ---- invokeinterface ----
    int ifaceCall(Runnable r) { r.run(); return 1; }
    int ifaceLen(CharSequence cs) { return cs.length(); }
    int ifaceList(java.util.List<Integer> xs) { return xs.size() + xs.get(0); }
    int ifaceMap(java.util.Map<String, Integer> m) { return m.size() + (m.isEmpty() ? 0 : 1); }
    int ifaceComparable(Comparable<Integer> c) { return c.compareTo(1); }
    int ifaceAutoClose(AutoCloseable c) {
        try {
            return c.hashCode();
        } catch (Exception e) {
            return -1;
        }
    }

    // ---- varargs（invoke with array pass-through）----
    static int sum(String sep, Object... parts) {
        int n = sep.length();
        for (Object p : parts) { n += p.toString().length(); }
        return n;
    }
    static int callVarargs0() { return sum(","); }
    static int callVarargs1() { return sum(",", "a", "b"); }
    static int callVarargs2() { return sum(",", Integer.valueOf(1), Double.valueOf(2.0)); }
    static int callVarargs3() {
        Object[] xs = { "x", "y", "z" };
        return sum("-", xs);
    }
    static int callVarargsMixed(int a, String b, Object... rest) {
        return a + b.length() + sum("", rest);
    }
    static int callVarargsMixed1() { return callVarargsMixed(1, "s"); }
    static int callVarargsMixed2() { return callVarargsMixed(1, "s", "t", "u"); }

    // ---- super 调用与虚分派形态（内部基类）----
    static class Base {
        int f(int a) { return a + 1; }
        int g(int a) { return f(a) * 10; }
        Object h() { return "base"; }
    }

    static final class Derived extends Base {
        @Override int f(int a) { return super.f(a) + 1; }
        @Override Object h() { return "derived" + f(1); }
        int both(int a) { return super.f(a) + f(a) + g(a); }
    }

    static int useDerived(int a) {
        Derived d = new Derived();
        Base b = d;
        return d.both(a) + b.f(a) + b.g(a) + d.h().hashCode();
    }
    static int dispatchTable(Base b, int a) { return b.f(a) + b.g(a); }
    static int dispatchBoth() {
        return dispatchTable(new Base(), 1) + dispatchTable(new Derived(), 2);
    }

    // ---- 递归（深调用链 + 合并点）----
    static int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
    static long fibRec(int n) { return n < 2 ? n : fibRec(n - 1) + fibRec(n - 2); }
    static int ack(int m, int n) {
        if (m == 0) { return n + 1; }
        return n == 0 ? ack(m - 1, 1) : ack(m - 1, ack(m, n - 1));
    }
    static int mutual1(int n) { return n <= 0 ? 0 : mutual2(n - 1); }
    static int mutual2(int n) { return n <= 0 ? 1 : mutual1(n - 1) + 2; }

    // ---- 方法链（流式）----
    static int streamish(String s) {
        return s.trim().toLowerCase().replace('a', 'b').substring(1).length();
    }
    static int boxUnbox(int a) {
        Integer i = a;
        int r = i + 1;
        Integer j = r;
        return j.compareTo(i) + i.intValue() + j.intValue();
    }
    static int charAtSum(String s) {
        int t = 0;
        for (int i = 0; i < s.length(); ++i) { t += s.charAt(i); }
        return t;
    }
    static int callVolatileRead(VolField f) { return f.getF0() + f.getF1() + f.getF2(); }
    static int callMixed(int a) {
        return sChain(a) + new VolInvoke().vChain(a) + psCaller(a);
    }
    static int invokeVirtualNull(Object o) {
        int r = 0;
        try {
            r = o.hashCode();
        } catch (NullPointerException e) {
            r = -1;
        }
        return r;
    }
    static int callThroughThis(VolInvoke v) { return v.vSelf(3) + v.pCaller(4); }

    int instanceEntry(int a) {
        return vChain(a) + pCaller(a) + ifaceCall(new Runnable() { @Override public void run() { } });
    }
}
