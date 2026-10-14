// VolAnno.java — libjbc 语料：注解全形态（RUNTIME/CLASS 保留 + 元素值全 tag）。
package jbcx.corpus;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

public final class VolAnno {
    private VolAnno() {
    }

    @Retention(RetentionPolicy.RUNTIME)
    @Target({ ElementType.TYPE, ElementType.METHOD, ElementType.FIELD })
    @interface Marker {
        String value() default "";
    }

    @Retention(RetentionPolicy.RUNTIME)
    @interface Full {
        int num() default 0;
        long big() default 0L;
        double real() default 0.0;
        String text() default "";
        Class<?> cls() default Object.class;
        Mode mode() default Mode.ON;
        int[] nums() default {};
        String[] texts() default {};
        Nested nested() default @Nested(id = 0);
    }

    @Retention(RetentionPolicy.RUNTIME)
    @interface Nested {
        int id();
    }

    @Retention(RetentionPolicy.CLASS)
    @interface Invisible {
        boolean flag() default true;
    }

    enum Mode { ON, OFF }

    @Marker("class-level")
    @Full(num = 3, big = 9_000_000_000L, real = 2.5, text = "t", cls = String.class,
          mode = Mode.OFF, nums = { 1, 2, 3 }, texts = { "x", "y" }, nested = @Nested(id = 7))
    @Invisible
    static final class Annotated {
        @Marker("field") @Invisible final int field = 1;

        @Marker("m1") @Full(num = 1)
        static int m1() { return 1; }

        @Marker @Nested(id = 2)
        static int m2(String s) { return s.length(); }

        @Invisible(flag = false)
        static long m3(long v) { return v * 2; }

        @Full(texts = { "a", "b", "c" }, nested = @Nested(id = 9))
        static void m4() {
        }

        static int readAll() {
            return m1() + m2("ab") + (int) m3(2L);
        }
    }

    // 运行时反射读注解（类在，读法不在字节码层面——仅作编译形态）。
    static int annotationShapes() {
        return Annotated.class.getAnnotations().length
            + Full.class.getMethods().length
            + Marker.class.getSimpleName().length();
    }

    @SuppressWarnings({ "rawtypes", "unchecked" })
    static int suppressShape() {
        raw = new java.util.ArrayList();
        raw.add("x");
        return ((java.util.List) raw).size();
    }

    @SuppressWarnings("rawtypes")
    static java.util.List raw;
}
