// VolLambda.java — libjbc 语料：lambda 全形态（捕获/非捕获/方法引用/构造器引用/this 捕获/循环内捕获）。
// release 8 → invokedynamic + LambdaMetafactory BSM + lambda$ 合成方法。
package jbcx.corpus;

import java.util.function.IntBinaryOperator;
import java.util.function.IntSupplier;
import java.util.function.IntUnaryOperator;
import java.util.function.Supplier;

public final class VolLambda {
    private int state;

    public VolLambda() {
    }

    public VolLambda(int init) { this.state = init; }

    // ---- 非捕获 lambda（单例形态）----
    static IntUnaryOperator id = x -> x;
    static IntBinaryOperator add = (a, b) -> a + b;
    static IntBinaryOperator max = (a, b) -> a > b ? a : b;
    static Supplier<String> hello = () -> "hello";
    static Runnable noop = () -> {
    };
    static IntSupplier fortyTwo = () -> 42;

    static int applyId(int v) { return id.applyAsInt(v); }
    static int applyAdd(int a, int b) { return add.applyAsInt(a, b); }
    static int applyMax(int a, int b) { return max.applyAsInt(a, b); }
    static String applyHello() { return hello.get(); }
    static void applyNoop() { noop.run(); }
    static int apply42() { return fortyTwo.getAsInt(); }

    // ---- 捕获 lambda（参数/局部/字段捕获）----
    IntUnaryOperator captureParam(int k) { return x -> x + k; }
    IntUnaryOperator captureTwo(int a, int b) { return x -> x * a + b; }
    IntSupplier captureField() { return () -> state; }
    IntUnaryOperator captureThis() { return x -> this.state + x; }
    Supplier<VolLambda> captureCtor() { return () -> new VolLambda(state); }
    IntUnaryOperator captureLocal() {
        final int base = 10;
        return x -> x + base;
    }
    IntUnaryOperator captureEffectivelyFinal() {
        int base = 7;
        return x -> x * base;
    }
    IntUnaryOperator captureNested(int k) {
        return x -> {
            int y = x + k;
            return y * y;
        };
    }
    IntBinaryOperator captureBoth(Object o, int k) { return (a, b) -> o.hashCode() + a * k + b; }
    int useCaptures(int v) {
        return captureParam(2).applyAsInt(v)
            + captureTwo(3, 4).applyAsInt(v)
            + captureField().getAsInt()
            + captureLocal().applyAsInt(v);
    }

    // ---- 方法引用全形态 ----
    static IntUnaryOperator staticRef = VolArith::ineg;
    IntUnaryOperator boundRef = this::bump;
    java.util.function.IntFunction<VolLambda> ctorRef = VolLambda::new;
    java.util.function.IntFunction<int[]> arrayRef = int[]::new;

    int bump(int v) { return ++state + v; }
    int useMethodRefs(int v) {
        return staticRef.applyAsInt(v) + boundRef.applyAsInt(v);
    }
    int useCtorRef(int v) { return ctorRef.apply(v).state + v; }
    static int useArrayRef(int v) { return new VolLambda().arrayRef.apply(v).length; }

    // ---- 循环/分支内 lambda（合成方法多形态）----
    static int lambdasInLoop(int n) {
        int s = 0;
        for (int i = 0; i < n; ++i) {
            final int k = i;
            IntUnaryOperator f = x -> x + k;
            s += f.applyAsInt(k);
        }
        return s;
    }
    static int lambdaInSwitch(int k) {
        switch (k) {
        case 0: return ((IntUnaryOperator) x -> x + 1).applyAsInt(0);
        case 1: return ((Supplier<String>) () -> "one").get().length();
        default: return ((Runnable) () -> {
        }).hashCode();
        }
    }
    static int lambdaInTry(int k) {
        try {
            return ((IntSupplier) () -> 100 / k).getAsInt();
        } catch (ArithmeticException e) {
            return -1;
        }
    }
    static int lambdaChain(int v) {
        IntUnaryOperator f = x -> x + 1;
        IntUnaryOperator g = x -> x * 2;
        return g.applyAsInt(f.applyAsInt(v));
    }
    static int lambdaWithControl(int v) {
        IntUnaryOperator f = x -> {
            if (x < 0) { return -x; }
            while (x > 100) { x -= 100; }
            return x;
        };
        return f.applyAsInt(v);
    }
    static int recursiveLambda(int v) {
        IntUnaryOperator f = x -> x <= 0 ? 0 : x + v;
        return f.applyAsInt(v) + f.applyAsInt(v - 1);
    }

    // ---- 装箱/泛型桥接形态 ----
    static java.util.function.Function<Integer, Integer> boxF = x -> x + 1;
    static int useBoxF(int v) { return boxF.apply(v); }
    static Supplier<Integer> boxS = () -> 5;
    static int useBoxS() { return boxS.get(); }

    int instanceEntry(int v) {
        return useCaptures(v) + useMethodRefs(v) + captureThis().applyAsInt(v);
    }
}
