package demo;

/** 搬移前的行为 oracle：反射读供体成员（与搬移后宿主侧完全同构的读法）。 */
public final class DonorMain {
    public static void main(String[] args) throws Exception {
        System.out.println("grab=" + Donor.class.getMethod("grab").invoke(null));
        System.out.println("stash=" + Donor.class.getField("stash").getInt(null));
        System.out.println("classify="
                           + Donor.class.getMethod("classify", int.class).invoke(null, 7));
        System.out.println("pickName="
                           + Donor.class.getMethod("pickName", String.class)
                                 .invoke(null, "bb"));
    }
}
