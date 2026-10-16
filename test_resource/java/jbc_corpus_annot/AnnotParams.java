import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * 注解族语料：注解类型默认值（AnnotationDefault）+ 参数注解双保留
 * （Runtime{Visible,Invisible}ParameterAnnotations）+ 参数名表（javac
 * -parameters 旗语产物 MethodParameters）。主语料的 --release 8 基础旗语
 * 不产参数名表，故立独立小语料以 --release 8 -g -parameters 现场编译
 * （manifest 旗语指纹记录，旗语升级即整批重编）。
 */
public class AnnotParams {

    @Retention(RetentionPolicy.RUNTIME)
    @Target(ElementType.PARAMETER)
    @interface Mark {
        String value() default "d";            // AnnotationDefault 's'
        int rank() default 1;                  // AnnotationDefault 'I'
        Class<?> ref() default Object.class;   // AnnotationDefault 'c'
        Mode mode() default Mode.OFF;          // AnnotationDefault 'e'
    }

    @Retention(RetentionPolicy.CLASS)
    @Target(ElementType.PARAMETER)
    @interface Hidden {
        boolean flag() default true;           // AnnotationDefault 'Z'
    }

    enum Mode { ON, OFF }

    // 参数注解双保留 + 混合空组：第 3 参无注解（参数维空内层表）。
    static int withAnnos(@Mark("a") int x, @Hidden @Mark(rank = 2) String s, int bare) {
        return x + s.length() + bare;
    }

    // 同参数双注解（可见 + 不可见各一条，组内注解数 > 1）。
    int mixed(@Mark int a, @Hidden long b) {
        return (int) (a + b);
    }

    // 纯参数名表（无参数注解）：MethodParameters 由 -parameters 旗语产出。
    static void plain(int first, String second) {
    }
}
