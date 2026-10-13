// Dump memory: -postScript Dump.java out.txt addr len [*addr len: dump at the pointer stored at addr]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import java.io.*;

public class Dump extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(a[0], true));
        for (int i = 1; i + 1 < a.length; i += 2) {
            String s = a[i];
            Address ad;
            if (s.startsWith("*")) {
                ad = toAddr(s.substring(1));
                ad = toAddr(getInt(ad) & 0xffffffffL);
            } else
                ad = toAddr(s);
            int n = Integer.decode(a[i + 1]);
            out.println("// " + s + " -> " + ad);
            for (int j = 0; j < n; j += 4)
                out.printf("%s: %08x%n", ad.add(j), getInt(ad.add(j)) & 0xffffffffL);
        }
        out.close();
    }
}
