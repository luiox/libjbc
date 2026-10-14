package demo;

/** SMT 探针样本：try/catch 嵌在 for 循环内（异常 handler 帧 + 回边帧，
 *  catch 变量槽与 try 路径在循环回边汇合——§10.4.8 裁决口径的自然形态）。 */
public final class ProbeExc {
    private ProbeExc() { }

    public static int grab(String[] a, int i) {
        int r = 0;
        for (int k = 0; k < 3; ++k) {
            try {
                r += a[i].length();
            } catch (RuntimeException e) {
                r += 10;
            }
        }
        return r;
    }
}
