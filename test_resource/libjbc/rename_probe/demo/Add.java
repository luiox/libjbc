package demo;

public final class Add implements Expr {
    private final int delta;

    public Add(int delta) {
        this.delta = delta;
    }

    @Override
    public int eval(int x) {
        return x + delta;
    }
}
