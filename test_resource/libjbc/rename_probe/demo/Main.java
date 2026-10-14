package demo;

import java.util.function.Function;

public final class Main {
    public static Expr CACHED;

    public static Expr make(int delta) {
        return new Add(delta);
    }

    public static Expr pick(Expr a, Expr b) {
        return a.eval(0) >= b.eval(0) ? a : b;
    }

    public static int drive(Expr e) throws Fail {
        if (e.eval(0) > 1000) {
            throw new Fail("too big");
        }
        Object o = e;
        Expr e2 = (o instanceof Expr) ? (Expr) o : null;
        Expr[] arr = new Expr[] { e2, new Add(1) };
        int sum = 0;
        for (Expr x : arr) {
            sum += x.eval(sum);
        }
        Function<Integer, Integer> f = k -> k + e.eval(k);
        sum += f.apply(sum);
        String literal = "Ldemo/Expr;";
        Box<Expr> box = new Box<>(e2);
        CACHED = pick(e, e2);
        System.out.println("sum=" + sum + " box=" + (box.get() == e2)
            + " cls=" + e.getClass().getName() + " name=" + Expr.class.getName()
            + " lit=" + literal);
        return sum;
    }

    public static void main(String[] args) throws Fail {
        drive(make(40));
        System.out.println("ok");
    }
}
