// VolTry.java — libjbc 语料：异常表全形态（try/catch/finally/multi-catch/嵌套/TWR/rethrow）。
package jbcx.corpus;

public final class VolTry {
    private VolTry() {
    }

    static class SmallExc extends RuntimeException {
        SmallExc(String m) { super(m); }
    }

    static class TinyExc extends RuntimeException {
        TinyExc(String m) { super(m); }
    }

    // ---- 基础 try/catch ----
    static int basic(String[] a) {
        try {
            return a[0].length();
        } catch (RuntimeException e) {
            return -1;
        }
    }
    static int twoHandlers(String[] a) {
        try {
            return a[0].length();
        } catch (NullPointerException e) {
            return -2;
        } catch (RuntimeException e) {
            return -1;
        }
    }
    static int multiCatch(String s) {
        try {
            return Integer.parseInt(s);
        } catch (IllegalStateException | NumberFormatException e) {
            return 0;
        }
    }
    static int withFinally(int[] a) {
        int r = 0;
        try {
            r = a[0];
            return r;
        } finally {
            r = -r;
        }
    }
    static int catchAndFinally(String[] a) {
        try {
            return a[0].length();
        } catch (RuntimeException e) {
            return -1;
        } finally {
            Object o = a;
        }
    }
    static int finallyOnly(int[] a, boolean t) {
        try {
            if (t) { return a[1]; }
            return 0;
        } finally {
            a[0] = 1;
        }
    }
    static int finallyThrows(int[] a) {
        try {
            return a[0];
        } finally {
            if (a == null) { throw new SmallExc("null"); }
        }
    }

    // ---- 嵌套 try ----
    static int nested(String[] a) {
        try {
            try {
                return a[0].length();
            } catch (NullPointerException e) {
                return -2;
            }
        } catch (RuntimeException e) {
            return -1;
        }
    }
    static int nestedLoop(String[] a, int n) {
        int s = 0;
        for (int i = 0; i < n; ++i) {
            try {
                s += a[i].length();
            } catch (ArrayIndexOutOfBoundsException e) {
                s += 100;
            }
        }
        return s;
    }
    static int tryInLoopBreak(int n) {
        int s = 0;
        while (true) {
            try {
                if (s > n) { break; }
                s += 1;
            } finally {
                s += 0;
            }
        }
        return s;
    }
    static int deep3(String[] a) {
        try {
            try {
                try {
                    return a[0].length();
                } finally {
                    a[0] = "x";
                }
            } catch (RuntimeException e) {
                throw new SmallExc("m");
            }
        } catch (SmallExc e) {
            return 7;
        }
    }

    // ---- rethrow / 异常包装 ----
    static int wrap(String[] a) {
        try {
            return a[0].length();
        } catch (RuntimeException e) {
            throw new SmallExc(e.getMessage());
        }
    }
    static void throwIfNull(Object o) {
        if (o == null) { throw new NullPointerException("o"); }
    }
    static int throwChain(int k) {
        try {
            throwIfNull(null);
            return k;
        } catch (NullPointerException e) {
            return -k;
        }
    }
    static int reraise(RuntimeException e) {
        try {
            throw e;
        } catch (RuntimeException x) {
            return x.getMessage() == null ? 0 : 1;
        }
    }

    // ---- synchronized + 异常边（monitor 出口）----
    static int syncTry(Object o, int k) {
        try {
            synchronized (o) {
                return k * 2;
            }
        } catch (RuntimeException e) {
            return -1;
        }
    }
    static int syncFinally(Object o, int[] a) {
        try {
            synchronized (o) {
                return a[0];
            }
        } finally {
            a[0] += 1;
        }
    }
    int syncInstanceTry(Object o) {
        synchronized (this) {
            try {
                o.toString();
                return 1;
            } catch (RuntimeException e) {
                return 0;
            }
        }
    }

    // ---- try-with-resources（release 8 → $closeResource 合成方法）----
    static int twr(Res r) {
        try (Res rr = r) {
            return rr.use();
        } catch (Exception e) {
            return -1;
        }
    }
    static int twr2(Res a, Res b) {
        try (Res x = a; Res y = b) {
            return x.use() + y.use();
        } catch (Exception e) {
            return -2;
        }
    }
    static int twrLoop(int n) {
        int s = 0;
        for (int i = 0; i < n; ++i) {
            try (Res r = new Res(i)) {
                s += r.use();
            } catch (Exception e) {
                s -= 1;
            }
        }
        return s;
    }
    static int twrThrow(Res r) throws Exception {
        try (Res rr = r) {
            return rr.use();
        }
    }

    // ---- 方法级 throws（Exceptions 属性形态）----
    static void declaredThrows(int k) throws SmallExc, TinyExc {
        if (k == 1) { throw new SmallExc("1"); }
        if (k == 2) { throw new TinyExc("2"); }
    }
    static int callDeclared(int k) {
        try {
            declaredThrows(k);
            return 0;
        } catch (SmallExc e) {
            return 1;
        } catch (TinyExc e) {
            return 2;
        }
    }

    static final class Res implements AutoCloseable {
        private final int v;
        Res(int v) { this.v = v; }
        int use() { return v + 1; }
        @Override public void close() {
            if (v < 0) { throw new SmallExc("close"); }
        }
    }
}
