// DumpSigRef.java — B1 反射对照的 dump 助手：把样本的全部泛型声明按
// Type.getTypeName() 渲染为行式文本，供 C++ 侧与签名树渲染对拍。
// 行格式：
//   class|<fq binary>|<super>|I:<itf>...|P:<name>|PB:<bound>...
//   field|<owner>|<name>|<typeName>
//   method|<owner>|<name>|<ret>|P:<name>|PB:<bound>...|<param>...|E:<exc>...
package jbcx.sigref;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;

public final class DumpSigRef {
    private DumpSigRef() {
    }

    public static void main(String[] args) {
        dump(SigRefSamples.class);
        dump(SigRefSamples.Store.class);
        dump(SigRefSamples.Wild.class);
        dump(SigRefSamples.Pair.class);
        dump(SigRefSamples.Thrower.class);
        dump(SigRefSamples.Hold.class);
    }

    static void dump(Class<?> c) {
        final StringBuilder sb = new StringBuilder("class|").append(c.getName());
        sb.append('|').append(c.getGenericSuperclass().getTypeName());
        for (Type itf : c.getGenericInterfaces()) {
            sb.append("|I:").append(itf.getTypeName());
        }
        appendTypeParams(sb, c.getTypeParameters());
        System.out.println(sb);
        for (Field f : c.getDeclaredFields()) {
            System.out.println("field|" + c.getName() + "|" + f.getName() + "|"
                    + f.getGenericType().getTypeName());
        }
        for (Method m : c.getDeclaredMethods()) {
            final StringBuilder mb = new StringBuilder("method|").append(c.getName())
                    .append('|').append(m.getName())
                    .append('|').append(m.getGenericReturnType().getTypeName());
            appendTypeParams(mb, m.getTypeParameters());
            for (Type p : m.getGenericParameterTypes()) {
                mb.append('|').append(p.getTypeName());
            }
            for (Type ex : m.getGenericExceptionTypes()) {
                mb.append("|E:").append(ex.getTypeName());
            }
            System.out.println(mb);
        }
    }

    static void appendTypeParams(StringBuilder sb, TypeVariable<?>[] params) {
        for (TypeVariable<?> tv : params) {
            sb.append("|P:").append(tv.getName());
            for (Type b : tv.getBounds()) {
                sb.append("|PB:").append(b.getTypeName());
            }
        }
    }
}
