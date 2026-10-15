package cpprobe;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;

@Retention(RetentionPolicy.RUNTIME)
@Target({ElementType.TYPE, ElementType.METHOD})
@interface Mark {
    String name();
    int level();
    Class<?> via();
}

@Mark(name = "probe", level = 3, via = Runnable.class)
public class Probe extends Base<String> {
    @Deprecated
    public static final String K = "const-tag";

    public interface Fold {
        String fold(String a, String b);
    }

    private final String tag = "probe-field";

    @Mark(name = "calc", level = 7, via = Fold.class)
    public <X extends Number> String calc(X x) throws Exception {
        Fold f = (a, b) -> a + "#" + b;
        return f.fold(tag + "/" + K, x.toString());
    }

    @Override
    public String echo(String v) {
        return super.echo(v);
    }

    public static void main(String[] args) throws Exception {
        Mark cm = Probe.class.getAnnotation(Mark.class);
        System.out.println("anno=" + cm.name() + "/" + cm.level() + "/"
                + cm.via().getSimpleName());
        ParameterizedType sup = (ParameterizedType) Probe.class.getGenericSuperclass();
        System.out.println("super=" + sup.getActualTypeArguments()[0].getTypeName());
        Method calc = Probe.class.getMethod("calc", Number.class);
        System.out.println("sig=" + calc.getGenericParameterTypes()[0].getTypeName());
        Mark mm = calc.getAnnotation(Mark.class);
        System.out.println("manno=" + mm.name() + "/" + mm.level() + "/"
                + mm.via().getSimpleName());
        System.out.println("ex=" + calc.getExceptionTypes().length);
        System.out.println("sf=" + Probe.class.getSimpleName());
        Probe p = new Probe();
        System.out.println("calc=" + p.calc(42));
        System.out.println("echo=" + p.echo("e1"));
        System.out.println("inner=" + Fold.class.getName());
        System.out.println("fields=" + Probe.class.getDeclaredFields().length);
    }
}
