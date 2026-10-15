// VolTypeUse.java — libjbc 语料：类型注解全场景（TYPE_USE 触点：字段/泛型实参/
// 继承/方法返回/throws/转型/instanceof/局部变量，RuntimeVisible+InvisibleType-
// Annotations 双保留；'c' 元素值含类字面量与 void.class 基类型对照）。
package jbcx.corpus;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;
import java.util.ArrayList;
import java.util.List;

public class VolTypeUse {

    @Retention(RetentionPolicy.RUNTIME)
    @Target(ElementType.TYPE_USE)
    @interface VTag {
        int value() default 0;
    }

    @Retention(RetentionPolicy.CLASS)
    @Target(ElementType.TYPE_USE)
    @interface ITag {
        Class<?> cls() default Object.class;
    }

    // 字段类型触点：field empty_target + 泛型实参 TYPE_ARGUMENT（Code 外）。
    @VTag(1) List<@VTag(2) String> field = new ArrayList<>();

    // 继承触点：supertype_target。
    static class Sub extends @VTag(3) VolTypeUse {
    }

    // 方法返回值（method return empty_target）+ throws（throws_target）。
    static @VTag(4) String drive(@VTag(5) Object o) throws @VTag(6) Exception {
        // 转型（CAST offset 族）+ instanceof（INSTANCE_OF offset 族）。
        String s = (@VTag(7) String) o;
        boolean flag = o instanceof @ITag(cls = String.class) String;
        if (flag) {
            // 局部变量触点：localvar_target（泛型实参携带 'c' 类字面量）。
            @VTag(9) List<@ITag(cls = String.class) String> local = new ArrayList<>();
            return s + local.size();
        }
        return s;
    }

    static boolean peek(Object o) throws Exception {
        return drive(o).isEmpty() && new Sub() != null;
    }
}
