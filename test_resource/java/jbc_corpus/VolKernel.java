// VolKernel.java — libjbc 语料：计算内核（循环 + 条件 + 调用协同的小算法体，丰富合并点/回边形态）。
package jbcx.corpus;

public final class VolKernel {
    private VolKernel() {
    }

    static int dot(int[] a, int[] b) {
        int s = 0;
        for (int i = 0; i < a.length && i < b.length; ++i) { s += a[i] * b[i]; }
        return s;
    }
    static double norm(int[] a) {
        double s = 0.0;
        for (int v : a) { s += v * v; }
        return Math.sqrt(s);
    }
    static int[] addVec(int[] a, int[] b) {
        int[] r = new int[Math.min(a.length, b.length)];
        for (int i = 0; i < r.length; ++i) { r[i] = a[i] + b[i]; }
        return r;
    }
    static int[] scaleVec(int[] a, int k) {
        int[] r = new int[a.length];
        for (int i = 0; i < a.length; ++i) { r[i] = a[i] * k; }
        return r;
    }
    static boolean allPositive(int[] a) {
        for (int v : a) { if (v <= 0) { return false; } }
        return true;
    }
    static boolean anyNegative(int[] a) {
        for (int v : a) { if (v < 0) { return true; } }
        return false;
    }
    static int countMatches(int[] a, int v) {
        int n = 0;
        for (int x : a) { if (x == v) { ++n; } }
        return n;
    }
    static int secondMax(int[] a) {
        int m1 = Integer.MIN_VALUE, m2 = Integer.MIN_VALUE;
        for (int v : a) {
            if (v > m1) { m2 = m1; m1 = v; }
            else if (v > m2) { m2 = v; }
        }
        return m2;
    }
    static int bubbleSortPasses(int[] a) {
        int passes = 0;
        for (int i = 0; i < a.length; ++i) {
            boolean moved = false;
            for (int j = 0; j + 1 < a.length - i; ++j) {
                if (a[j] > a[j + 1]) {
                    int t = a[j]; a[j] = a[j + 1]; a[j + 1] = t;
                    moved = true;
                }
            }
            ++passes;
            if (!moved) { break; }
        }
        return passes;
    }
    static int binarySearch(int[] a, int v) {
        int lo = 0, hi = a.length - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >>> 1;
            if (a[mid] == v) { return mid; }
            if (a[mid] < v) { lo = mid + 1; } else { hi = mid - 1; }
        }
        return -1;
    }
    static int lcg(int seed, int steps) {
        int x = seed;
        for (int i = 0; i < steps; ++i) { x = x * 1103515245 + 12345; }
        return x;
    }
    static int crc32ish(byte[] bs) {
        int c = 0xFFFFFFFF;
        for (byte b : bs) {
            c ^= b;
            for (int k = 0; k < 8; ++k) { c = (c >>> 1) ^ (0xEDB88320 & -(c & 1)); }
        }
        return ~c;
    }
    static int popcountAll(long v) {
        int n = 0;
        while (v != 0) { v &= v - 1; ++n; }
        return n;
    }
    static int digitRoot(int v) {
        while (v >= 10) {
            int s = 0;
            while (v > 0) { s += v % 10; v /= 10; }
            v = s;
        }
        return v;
    }
    static boolean armstrong(int v) {
        int digits = 0, t = v;
        while (t > 0) { ++digits; t /= 10; }
        int s = 0;
        t = v;
        while (t > 0) { int d = t % 10; int p = 1; for (int i = 0; i < digits; ++i) { p *= d; } s += p; t /= 10; }
        return s == v;
    }
    static int matrixTrace(int[][] m) {
        int s = 0;
        for (int i = 0; i < m.length; ++i) { s += m[i][i]; }
        return s;
    }
    static int[][] matMul(int[][] a, int[][] b) {
        int n = a.length, m = b[0].length, k = b.length;
        int[][] r = new int[n][m];
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < m; ++j) {
                int s = 0;
                for (int x = 0; x < k; ++x) { s += a[i][x] * b[x][j]; }
                r[i][j] = s;
            }
        }
        return r;
    }
    static int wordCount(String s) {
        int words = 0;
        boolean in = false;
        for (int i = 0; i < s.length(); ++i) {
            boolean ws = Character.isWhitespace(s.charAt(i));
            if (!ws && !in) { ++words; }
            in = !ws;
        }
        return words;
    }
    static int camelSplit(String s) {
        int parts = 1;
        for (int i = 1; i < s.length(); ++i) {
            if (Character.isUpperCase(s.charAt(i))) { ++parts; }
        }
        return parts;
    }
    static int romanValue(String s) {
        int total = 0, prev = 0;
        for (int i = s.length() - 1; i >= 0; --i) {
            int v = romanDigit(s.charAt(i));
            if (v < prev) { total -= v; } else { total += v; }
            prev = v;
        }
        return total;
    }
    private static int romanDigit(char c) {
        switch (c) {
        case 'I': return 1;
        case 'V': return 5;
        case 'X': return 10;
        case 'L': return 50;
        case 'C': return 100;
        case 'D': return 500;
        case 'M': return 1000;
        default: return 0;
        }
    }
    static int levenshtein(String a, String b) {
        int[][] d = new int[a.length() + 1][b.length() + 1];
        for (int i = 0; i <= a.length(); ++i) { d[i][0] = i; }
        for (int j = 0; j <= b.length(); ++j) { d[0][j] = j; }
        for (int i = 1; i <= a.length(); ++i) {
            for (int j = 1; j <= b.length(); ++j) {
                int cost = a.charAt(i - 1) == b.charAt(j - 1) ? 0 : 1;
                int m1 = d[i - 1][j] + 1;
                int m2 = d[i][j - 1] + 1;
                int m3 = d[i - 1][j - 1] + cost;
                d[i][j] = Math.min(m1, Math.min(m2, m3));
            }
        }
        return d[a.length()][b.length()];
    }
    static int balancedParens(String s) {
        int depth = 0, max = 0;
        for (int i = 0; i < s.length(); ++i) {
            char c = s.charAt(i);
            if (c == '(') { ++depth; max = Math.max(max, depth); }
            else if (c == ')') { --depth; }
        }
        return depth == 0 ? max : -1;
    }
    static long collatzLen(long n) {
        long steps = 0;
        while (n != 1) {
            n = (n & 1) == 0 ? n >> 1 : 3 * n + 1;
            ++steps;
        }
        return steps;
    }
    static int josephus(int n, int k) {
        int r = 0;
        for (int i = 2; i <= n; ++i) { r = (r + k) % i; }
        return r;
    }
    static int stringHashPolynomial(String s) {
        int h = 0;
        for (int i = 0; i < s.length(); ++i) { h = h * 31 + s.charAt(i); }
        return h;
    }
    static int kernighan(int v) {
        int c = 0;
        for (; v != 0; v &= (v - 1)) { ++c; }
        return c;
    }
    static int weighted(int[] vals, int[] weights) {
        int s = 0;
        for (int i = 0; i < vals.length; ++i) {
            int w = i < weights.length ? weights[i] : 1;
            s += vals[i] * w;
        }
        return s;
    }
    static int zebraPuzzle(int[] xs) {
        int best = -1;
        for (int i = 0; i < xs.length; ++i) {
            for (int j = i + 1; j < xs.length; ++j) {
                for (int k = j + 1; k < xs.length; ++k) {
                    int score = xs[i] + xs[j] * 2 + xs[k] * 3;
                    if (score > best) { best = score; }
                }
            }
        }
        return best;
    }
    static int[] histogram(int[] xs, int buckets) {
        int[] h = new int[buckets];
        for (int x : xs) {
            int b = x < 0 ? 0 : (x >= buckets ? buckets - 1 : x);
            ++h[b];
        }
        return h;
    }
    static int[][] pascalTriangle(int n) {
        int[][] t = new int[n][];
        for (int i = 0; i < n; ++i) {
            t[i] = new int[i + 1];
            t[i][0] = 1;
            for (int j = 1; j < i; ++j) { t[i][j] = t[i - 1][j - 1] + t[i - 1][j]; }
            t[i][i] = 1;
        }
        return t;
    }
    static int skewSum(int[][] m) {
        int s = 0;
        for (int i = 0; i < m.length; ++i) {
            for (int j = 0; j < m[i].length; ++j) {
                s += (i ^ j) & 1;
                if (((i + j) & 7) == 0) { s += m[i][j]; }
            }
        }
        return s;
    }
    static int romanLoopBack(int n) {
        int[] vals = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
        String[] syms = { "M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I" };
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < vals.length && n > 0; ++i) {
            while (n >= vals[i]) { sb.append(syms[i]); n -= vals[i]; }
        }
        return sb.length();
    }
    static int pipeline(int v) {
        int a = v ^ (v >>> 16);
        int b = a * 0x45d9f3b;
        int c = (b ^ (b >>> 16)) * 0x45d9f3b;
        return (c ^ (c >>> 16)) & 0x7FFFFFFF;
    }
    static int stateMachine(String input) {
        int state = 0;
        for (int i = 0; i < input.length(); ++i) {
            char c = input.charAt(i);
            switch (state) {
            case 0: state = c == 'a' ? 1 : 0; break;
            case 1: state = c == 'b' ? 2 : (c == 'a' ? 1 : 0); break;
            case 2: state = c == 'c' ? 3 : (c == 'a' ? 1 : 0); break;
            default: state = 3; break;
            }
        }
        return state;
    }
    static double harmonic(int n) {
        double h = 0.0;
        for (int i = 1; i <= n; ++i) { h += 1.0 / i; }
        return h;
    }
    static double expo(double x, int terms) {
        double sum = 1.0, term = 1.0;
        for (int i = 1; i <= terms; ++i) {
            term *= x / i;
            sum += term;
        }
        return sum;
    }
    static int newtonSqrt(int v) {
        if (v < 2) { return v; }
        int x = v, y = (x + 1) / 2;
        while (y < x) {
            x = y;
            y = (x + v / x) / 2;
        }
        return x;
    }
    static int strNaturalCmp(String a, String b) {
        int la = a.length(), lb = b.length();
        for (int i = 0; i < Math.min(la, lb); ++i) {
            int ca = a.charAt(i), cb = b.charAt(i);
            if (ca != cb) { return ca - cb; }
        }
        return la - lb;
    }
    static int vowelCount(String s) {
        int n = 0;
        for (int i = 0; i < s.length(); ++i) {
            switch (Character.toLowerCase(s.charAt(i))) {
            case 'a': case 'e': case 'i': case 'o': case 'u': ++n; break;
            default: break;
            }
        }
        return n;
    }
    static int rangeCheckAll(int[] xs, int lo, int hi) {
        int bad = 0;
        for (int x : xs) { if (x < lo || x > hi) { ++bad; } }
        return bad;
    }
    static int zigzag(int[] xs) {
        int runs = 1;
        for (int i = 1; i + 1 < xs.length; ++i) {
            boolean up = xs[i] > xs[i - 1] && xs[i] > xs[i + 1];
            boolean down = xs[i] < xs[i - 1] && xs[i] < xs[i + 1];
            if (up || down) { ++runs; }
        }
        return runs;
    }
    static int sumDigitsRecursive(int v) {
        return v < 10 ? v : v % 10 + sumDigitsRecursive(v / 10);
    }
    static int powMod(int base, int exp, int mod) {
        int r = 1;
        base %= mod;
        while (exp > 0) {
            if ((exp & 1) != 0) { r = r * base % mod; }
            base = base * base % mod;
            exp >>= 1;
        }
        return r;
    }
    static int bitReverse8(int v) {
        int r = 0;
        for (int i = 0; i < 8; ++i) { r = r << 1 | (v >>> i & 1); }
        return r & 0xFF;
    }
    static int grayCode(int v) { return v ^ (v >> 1); }
    static int unGray(int g) {
        int v = 0;
        for (int bit = 31; bit >= 0; --bit) { v = v << 1 | ((v ^ g) >>> bit & 1); }
        return v;
    }
    static int overlapping(int[][] a, int[][] b) {
        int hits = 0;
        for (int i = 0; i < a.length; ++i) {
            for (int j = 0; j < b.length; ++j) {
                if (a[i][0] == b[j][0] && a[i][1] == b[j][1]) { ++hits; }
            }
        }
        return hits;
    }
    static long byteChecksum(byte[] bs) {
        long s = 0;
        for (int i = 0; i < bs.length; ++i) { s = s * 31 + bs[i]; }
        return s;
    }
    static int chainKernels(int v) {
        return pipeline(v) ^ powMod(v & 15, 3, 97) + newtonSqrt(v & 0xFFFF);
    }
}
