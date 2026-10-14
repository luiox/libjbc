// VolString.java — libjbc 语料：字符串 switch/比较/装箱字符串/char 运算/UTF-8 常量。
package jbcx.corpus;

public final class VolString {
    private VolString() {
    }

    // ---- 字符串 switch（hashCode + equals 链 + $SwitchMap 无关）----
    static int strSwitch(String s) {
        switch (s) {
        case "aa": return 1;
        case "bb": return 2;
        case "cc": return 3;
        case "dd": return 4;
        default: return -1;
        }
    }
    static int strSwitchSparse(String s) {
        switch (s) {
        case "zebra": return 26;
        case "apple": return 1;
        case "mango": return 13;
        default: return 0;
        }
    }
    static int strSwitchFall(String s) {
        int r;
        switch (s) {
        case "x":
        case "y":
        case "z": r = 26; break;
        default: r = 0; break;
        }
        return r;
    }
    static int strSwitchVoid(String s) {
        switch (s) {
        case "go": System.lineSeparator(); break;
        case "stop": System.out.flush(); break;
        default: break;
        }
        return s.length();
    }

    // ---- 字符串比较 / 方法 ----
    static boolean same(String a, String b) { return a.equals(b); }
    static boolean sameIgnoreCase(String a, String b) { return a.equalsIgnoreCase(b); }
    static int cmp(String a, String b) { return a.compareTo(b); }
    static boolean starts(String a, String p) { return a.startsWith(p); }
    static boolean ends(String a, String p) { return a.endsWith(p); }
    static boolean contains(String a, String p) { return a.contains(p); }
    static int indexOf(String a, String p) { return a.indexOf(p); }
    static String upper(String a) { return a.toUpperCase(); }
    static String trim(String a) { return a.trim(); }
    static String replace(String a) { return a.replace('a', 'b'); }
    static String[] split(String a) { return a.split(","); }
    static String substring(String a, int i) { return a.substring(i); }
    static char first(String a) { return a.isEmpty() ? '?' : a.charAt(0); }
    static boolean empty(String a) { return a.isEmpty(); }
    static String valueOfInt(int v) { return String.valueOf(v); }
    static String format(String f, Object... args) { return String.format(f, args); }
    static int hash(String a) { return a.hashCode(); }
    static String interned(String a) { return a.intern(); }
    static boolean matches(String a) { return a.matches("[a-z]+"); }
    static String join(String[] xs) { return String.join("-", xs); }
    static boolean isEmptyOrNull(String a) { return a == null || a.isEmpty(); }
    static int cmpChain(String a, String b) {
        int c = a.compareTo(b);
        if (c != 0) { return c; }
        return a.length() - b.length();
    }

    // ---- char 运算 ----
    static boolean isDigit(char c) { return c >= '0' && c <= '9'; }
    static boolean isLower(char c) { return c >= 'a' && c <= 'z'; }
    static char toUpper(char c) { return isLower(c) ? (char) (c - 32) : c; }
    static int digitVal(char c) { return isDigit(c) ? c - '0' : -1; }
    static int countUpper(String s) {
        int n = 0;
        for (int i = 0; i < s.length(); ++i) { if (isUpper(s.charAt(i))) { ++n; } }
        return n;
    }
    private static boolean isUpper(char c) { return c >= 'A' && c <= 'Z'; }
    static String reverseChars(String s) {
        char[] cs = s.toCharArray();
        int i = 0, j = cs.length - 1;
        while (i < j) { char t = cs[i]; cs[i] = cs[j]; cs[j] = t; ++i; --j; }
        return new String(cs);
    }

    // ---- UTF-8 常量形态（多字节字符进 CP Utf8）----
    static String ascii() { return "plain-ascii-0123456789"; }
    static String cjk() { return "中文样本"; }
    static String mixed() { return "mix中en文样本-Ω≈ç√"; }
    static String emoji() { return "emoji-\u00e9\u00e8"; }
    static String escapes() { return "tab\tnewline\nquote\"backslash\\"; }
    static String emptyStr() { return ""; }

    // ---- String 常量 + switch map 协同 ----
    static final String A = "const-a";
    static final String B = "const-b";
    static int pickConst(String s) {
        if (s == A) { return 1; }
        if (s == B) { return 2; }
        return 0;
    }
    static int eqConst(String s) {
        if (A.equals(s)) { return 1; }
        if (B.equals(s)) { return 2; }
        return s.length();
    }
}
