package demo;

/**
 * B3 结构元数据 JVM 真跑探针：嵌套类族（成员内部类/局部类/匿名类）改名后
 * 的反射语义读出。改名 pass 重写 InnerClasses/EnclosingMethod 槽指向的外围
 * 类名后，getDeclaringClass / isAnonymousClass / getEnclosingClass 必须
 * 读出新外围名、匿名/局部判定与包围方法必须原样存活。Nest 族面：
 * getNestHost 读改名后的宿主名、getNestMembers 计数原样（成员条目指向
 * 未改名的内部类二进制名）、sealed 允许表反射原样。
 */
public final class NestProbe {
    public static void main(String[] args) throws Exception {
        Nest n = new Nest();
        Nest.Token t = n.mint(3);
        Expr anon = n.anonExpr();
        Object local = n.localExpr(5);

        System.out.println("declaring=" + t.getClass().getDeclaringClass().getName());
        System.out.println("anon=" + anon.getClass().isAnonymousClass()
            + " anonOuter=" + anon.getClass().getEnclosingClass().getName()
            + " anonMethod=" + anon.getClass().getEnclosingMethod().getName());
        System.out.println("local=" + local.getClass().isLocalClass()
            + " localOuter=" + local.getClass().getEnclosingClass().getName()
            + " localMethod=" + local.getClass().getEnclosingMethod().getName());
        System.out.println("throws=" + Nest.class.getMethod("mint", int.class)
            .getExceptionTypes()[0].getName());
        System.out.println("nestHost=" + t.getClass().getNestHost().getName());
        System.out.println("nestMembers=" + t.getClass().getNestMembers().length);
        System.out.println("permits=" + Seal.class.getPermittedSubclasses()[0].getName()
            + "=" + new Seal.Impl().v());
        System.out.println("eval=" + (t.v + anon.eval(21)));
    }
}
