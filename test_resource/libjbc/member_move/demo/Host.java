package demo;

/**
 * 宿主类：独立可编译（对被搬成员只做反射访问，无编译期依赖）。搬移后
 * demo.Host 上必须出现 grab/stash/classify/pickName（dive 为私有面，随
 * classify 的调用图一并搬入），且行为与搬移前的 demo.Donor 等价。
 */
public final class Host {
    public static void main(String[] args) throws Exception {
        System.out.println("grab=" + Host.class.getMethod("grab").invoke(null));
        System.out.println("stash=" + Host.class.getField("stash").getInt(null));
        System.out.println("classify="
                           + Host.class.getMethod("classify", int.class).invoke(null, 7));
        System.out.println("pickName="
                           + Host.class.getMethod("pickName", String.class)
                                 .invoke(null, "bb"));
    }
}
