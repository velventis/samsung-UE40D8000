// Decompile functions by name or address: -postScript Decomp.java out.c name|0xaddr ...
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class Decomp extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(a[0], true));
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        for (int i = 1; i < a.length; i++) {
            List<Function> fs = new ArrayList<>();
            if (a[i].startsWith("0x")) {
                Address ad = toAddr(a[i]);
                Function f = fm.getFunctionContaining(ad);
                if (f != null) fs.add(f);
            } else if (a[i].startsWith("refs:")) {
                // functions referencing an address (data or code)
                Address ad = toAddr(a[i].substring(5));
                for (Reference r : getReferencesTo(ad)) {
                    Function f = fm.getFunctionContaining(r.getFromAddress());
                    out.println("// ref from " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (f == null ? "?" : f.getName()));
                }
                continue;
            } else if (a[i].startsWith("xref:")) {
                // functions referencing a string containing the text
                String t = a[i].substring(5);
                for (Data dd : currentProgram.getListing().getDefinedData(true)) {
                    Object v = dd.getValue();
                    if (v instanceof String && ((String) v).contains(t)) {
                        for (Reference r : getReferencesTo(dd.getAddress())) {
                            Function f = fm.getFunctionContaining(r.getFromAddress());
                            if (f != null && !fs.contains(f)) fs.add(f);
                        }
                    }
                }
            } else {
                for (Symbol s : currentProgram.getSymbolTable().getSymbols(a[i])) {
                    Function f = fm.getFunctionAt(s.getAddress());
                    if (f != null) fs.add(f);
                }
                for (Function f : fm.getFunctions(true))
                    if (f.getName().equals(a[i]) && !fs.contains(f)) fs.add(f);
            }
            if (fs.isEmpty()) out.println("// " + a[i] + ": not found");
            for (Function f : fs) {
                DecompileResults r = d.decompileFunction(f, 120, monitor);
                out.println("// ===== " + a[i] + " @ " + f.getEntryPoint());
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed");
            }
        }
        out.close();
    }
}
