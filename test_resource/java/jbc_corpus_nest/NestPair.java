/**
 * nest 语料：真 nestmate 对——成员内部类直读外围私有成员（javac 11+ 产出
 * NestHost+NestMembers 对；主语料的 --release 8 产物是 v52，不带这三件，
 * 故立独立小语料以 --release 17 现场编译）。
 */
public class NestPair {
    private int secret = 7;

    public class Inner {
        public int read() {
            return secret;          // 私有成员跨类访问 → 真 nestmate
        }

        public int add(int v) {
            return v + secret;
        }
    }

    public int use(int v) {
        Inner in = new Inner();
        return in.add(v) + in.read();
    }
}
