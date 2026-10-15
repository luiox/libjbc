package demo;

public final class Nest {
    /** 成员内部类：InnerClasses 行 inner=demo/Nest$Token、outer=demo/Nest。 */
    public final class Token {
        public final int v;

        public Token(int v) {
            this.v = v;
        }
    }

    /** 静态嵌套异常：mint 的 Exceptions 属性表条目（getExceptionTypes 读回面）。 */
    public static class Bad extends RuntimeException {
        public Bad(String message) {
            super(message);
        }
    }

    public Expr anonExpr() {
        // 匿名类：InnerClasses 行 outer=0/name=0（v51+ 形态），外围信息在
        // EnclosingMethod 属性（class=demo/Nest、method=anonExpr）。
        return new Expr() {
            @Override
            public int eval(int x) {
                return x * 2;
            }
        };
    }

    public Object localExpr(final int base) {
        // 局部类：EnclosingMethod 指向 demo/Nest.localExpr；InnerClasses 行
        // outer=demo/Nest、inner_name=Local。
        class Local implements Expr {
            @Override
            public int eval(int x) {
                return x + base;
            }
        }
        return new Local();
    }

    public Token mint(int v) throws Fail, Bad {
        if (v < 0) {
            throw new Bad("negative");
        }
        return new Token(v);
    }
}
