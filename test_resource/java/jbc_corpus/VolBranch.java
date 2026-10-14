// VolBranch.java — libjbc 语料：条件跳转全族 + 循环 + tableswitch/lookupswitch + break/continue/标号。
package jbcx.corpus;

public final class VolBranch {
    private VolBranch() {
    }

    // ---- if_icmp 全族 ----
    static int eq(int a, int b) { if (a == b) { return 1; } return 0; }
    static int ne(int a, int b) { if (a != b) { return 1; } return 0; }
    static int lt(int a, int b) { if (a < b) { return 1; } return 0; }
    static int le(int a, int b) { if (a <= b) { return 1; } return 0; }
    static int gt(int a, int b) { if (a > b) { return 1; } return 0; }
    static int ge(int a, int b) { if (a >= b) { return 1; } return 0; }
    static int eqE(int a, int b) { if (a == b) { return 1; } else { return 2; } }
    static int neE(int a, int b) { if (a != b) { return 1; } else { return 2; } }
    static int ltE(int a, int b) { if (a < b) { return 1; } else { return 2; } }
    static int leE(int a, int b) { if (a <= b) { return 1; } else { return 2; } }
    static int gtE(int a, int b) { if (a > b) { return 1; } else { return 2; } }
    static int geE(int a, int b) { if (a >= b) { return 1; } else { return 2; } }

    // ---- if<cond> 单操作数全族（与 0 比较）----
    static int iz(int a) { if (a == 0) { return 1; } return 0; }
    static int inz(int a) { if (a != 0) { return 1; } return 0; }
    static int ilz(int a) { if (a < 0) { return 1; } return 0; }
    static int ilez(int a) { if (a <= 0) { return 1; } return 0; }
    static int igez(int a) { if (a >= 0) { return 1; } return 0; }
    static int igz(int a) { if (a > 0) { return 1; } return 0; }
    static int abs0(int a) { if (a < 0) { return -a; } return a; }
    static int sign(int a) { if (a > 0) { return 1; } if (a < 0) { return -1; } return 0; }

    // ---- 引用条件（ifnull/ifnonnull/if_acmp*) ----
    static int isNull(Object o) { if (o == null) { return 1; } return 0; }
    static int nonNull(Object o) { if (o != null) { return 1; } return 0; }
    static int sameRef(Object a, Object b) { if (a == b) { return 1; } return 0; }
    static int diffRef(Object a, Object b) { if (a != b) { return 1; } return 0; }
    static String nullSafe(Object o) { return o == null ? "n" : o.toString(); }
    static int depth(Object a, Object b) { return a == null ? (b == null ? 0 : 1) : 2; }

    // ---- 布尔组合（短路与/或）----
    static boolean and2(boolean a, boolean b) { return a && b; }
    static boolean or2(boolean a, boolean b) { return a || b; }
    static boolean and3(boolean a, boolean b, boolean c) { return a && b && c; }
    static boolean or3(boolean a, boolean b, boolean c) { return a || b || c; }
    static boolean mixed(int a, int b) { return a > 0 && b < 10 || a == b; }
    static boolean not(boolean a) { return !a; }
    static int guard(int a, int b) { if (a > 0 && b > 0) { return a + b; } return -1; }
    static int guardOr(int a, int b) { if (a == 0 || b == 0) { return 0; } return a * b; }

    // ---- 循环形态 ----
    static int forLoop(int n) { int s = 0; for (int i = 0; i < n; ++i) { s += i; } return s; }
    static int whileLoop(int n) { int s = 0; int i = 0; while (i < n) { s += i; ++i; } return s; }
    static int doLoop(int n) { int s = 0; int i = 0; do { s += i; ++i; } while (i < n); return s; }
    static int nestedFor(int n) {
        int s = 0;
        for (int i = 0; i < n; ++i) { for (int j = 0; j < i; ++j) { s += j; } }
        return s;
    }
    static int whileTrue(int n) {
        int s = 0; int i = 0;
        while (true) { if (i >= n) { break; } s += i; ++i; }
        return s;
    }
    static int forDown(int n) { int s = 0; for (int i = n; i > 0; --i) { s += i; } return s; }
    static int forStep2(int n) { int s = 0; for (int i = 0; i < n; i += 2) { s += i; } return s; }
    static int forMulti(int n) {
        int s = 0;
        for (int i = 0, j = n; i < j; ++i, --j) { s += i * j; }
        return s;
    }
    static int infLoopBreak(int n) {
        int i = 0;
        for (;;) { ++i; if (i > n) { return i; } }
    }
    static int loopCond(int n) { int i = n; while (i-- > 0) { } return i; }

    // ---- break/continue/标号 ----
    static int breakAt(int n) { int s = 0; for (int i = 0; ; ++i) { if (i == n) { break; } s += i; } return s; }
    static int skipOdd(int n) {
        int s = 0;
        for (int i = 0; i < n; ++i) { if ((i & 1) != 0) { continue; } s += i; }
        return s;
    }
    static int labeledBreak(int n) {
        int s = 0;
        outer:
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                if (i * j > n) { break outer; }
                s += 1;
            }
        }
        return s;
    }
    static int labeledContinue(int n) {
        int s = 0;
        outer:
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                if (j > i) { continue outer; }
                s += j;
            }
        }
        return s;
    }
    static int earlyReturn(int n) {
        if (n < 0) { return -1; }
        if (n == 0) { return 0; }
        int s = 1;
        for (int i = 2; i <= n; ++i) { s *= i; }
        return s;
    }

    // ---- tableswitch（连续 case）----
    static int table3(int k) {
        switch (k) {
        case 0: return 10;
        case 1: return 11;
        case 2: return 12;
        default: return -1;
        }
    }
    static int table8(int k) {
        switch (k) {
        case 0: return 0;
        case 1: return 1;
        case 2: return 2;
        case 3: return 3;
        case 4: return 4;
        case 5: return 5;
        case 6: return 6;
        case 7: return 7;
        default: return -1;
        }
    }
    static int tableOff(int k) {
        switch (k) {
        case 10: return 1;
        case 11: return 2;
        case 12: return 3;
        case 13: return 4;
        case 14: return 5;
        default: return 0;
        }
    }
    static int tableNeg(int k) {
        switch (k) {
        case -2: return 20;
        case -1: return 21;
        case 0: return 22;
        case 1: return 23;
        default: return -1;
        }
    }
    static int tableFall(int k) {
        int r;
        switch (k) {
        case 1:
        case 2:
        case 3: r = 100; break;
        case 4:
        case 5: r = 200; break;
        default: r = 0; break;
        }
        return r;
    }
    static int tableVoid(int k) {
        switch (k) {
        case 0: System.lineSeparator(); break;
        case 1: System.out.flush(); break;
        default: break;
        }
        return k;
    }

    // ---- lookupswitch（稀疏 case）----
    static int lookup3(int k) {
        switch (k) {
        case 100: return 1;
        case 5000: return 2;
        case 900000: return 3;
        default: return -1;
        }
    }
    static int lookup7(int k) {
        switch (k) {
        case -100: return 7;
        case 0: return 0;
        case 7: return 1;
        case 700: return 2;
        case 70000: return 3;
        case 7000000: return 4;
        case 700000000: return 5;
        default: return -1;
        }
    }
    static int lookupChar(char c) {
        switch (c) {
        case 'a': return 1;
        case 'z': return 26;
        case '0': return 0;
        default: return -1;
        }
    }

    // ---- switch 内外控制流 ----
    static int switchLoop(int n) {
        int s = 0;
        for (int i = 0; i < n; ++i) {
            switch (i & 3) {
            case 0: s += 1; break;
            case 1: s += 2; break;
            case 2: continue;
            default: s -= 1; break;
            }
            s += 10;
        }
        return s;
    }
    static int switchNested(int k, int m) {
        switch (k) {
        case 0:
            switch (m) {
            case 0: return 100;
            case 1: return 101;
            default: return 102;
            }
        case 1: return 200 + m;
        default: return -1;
        }
    }
    static int switchReturn(int k) {
        switch (k) {
        case 0: return 0;
        case 1: return 1;
        default: return k * 2;
        }
    }
    static long switchLongish(int k) {
        long r;
        switch (k) {
        case 0: r = 10L; break;
        case 1: r = 20L; break;
        default: r = -1L; break;
        }
        return r;
    }

    // ---- 条件表达式链 ----
    static int grade(int s) { return s >= 90 ? 4 : (s >= 80 ? 3 : (s >= 70 ? 2 : (s >= 60 ? 1 : 0))); }
    static String boolName(boolean b) { return b ? "true" : "false"; }
    static int max3(int a, int b, int c) { return a > b ? (a > c ? a : c) : (b > c ? b : c); }
    static int min3(int a, int b, int c) { return a < b ? (a < c ? a : c) : (b < c ? b : c); }
    static int select(boolean p, int a, int b) { return p ? a : b; }
    static long wideSelect(boolean p, long a, long b) { return p ? a : b; }
    static Object refSelect(boolean p, Object a, Object b) { return p ? a : b; }

    // ---- 混合分支 + 循环 + 异常边形态 ----
    static int collatz(int n) {
        int steps = 0;
        while (n != 1) {
            if ((n & 1) == 0) { n /= 2; } else { n = 3 * n + 1; }
            ++steps;
        }
        return steps;
    }
    static int digitSum(int v) {
        int s = 0;
        while (v > 0) { s += v % 10; v /= 10; }
        return s;
    }
    static int countBits(long v) {
        int c = 0;
        for (; v != 0; v &= v - 1) { ++c; }
        return c;
    }
    static int fizzle(int n) {
        int s = 0;
        for (int i = 1; i <= n; ++i) {
            if (i % 15 == 0) { s += 100; }
            else if (i % 3 == 0) { s += 3; }
            else if (i % 5 == 0) { s += 5; }
            else { s += 1; }
        }
        return s;
    }
    static int sqrtISH(int v) {
        int r = 0;
        while ((r + 1) * (r + 1) <= v) { ++r; }
        return r;
    }
    static boolean prime(int n) {
        if (n < 2) { return false; }
        for (int i = 2; i * i <= n; ++i) { if (n % i == 0) { return false; } }
        return true;
    }
}
