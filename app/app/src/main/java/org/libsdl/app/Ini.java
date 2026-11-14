package org.libsdl.app;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/* ProjectBaseball.ini (el mismo formato que en PC: [seccion] y clave=valor). Conserva las líneas que no
 * toca (p. ej. [juego] carpeta, que escribe el port). */
public class Ini {
    public static final String CARPETA = android.os.Environment.getExternalStorageDirectory().getAbsolutePath() + "/ProjectBaseball";
    public static final String RUTA = CARPETA + "/ProjectBaseball.ini";
    private final List<String> l = new ArrayList<>();

    public Ini() {
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(RUTA), StandardCharsets.UTF_8))) {
            String s;
            while ((s = r.readLine()) != null) l.add(s.startsWith("﻿") ? s.substring(1) : s);
        } catch (Exception e) { /* sin archivo: vacío */ }
    }

    private int seccion(String sec) {
        for (int i = 0; i < l.size(); i++) if (l.get(i).trim().equalsIgnoreCase("[" + sec + "]")) return i;
        return -1;
    }
    public String get(String sec, String clave, String def) {
        int i = seccion(sec);
        if (i < 0) return def;
        for (i++; i < l.size() && !l.get(i).trim().startsWith("["); i++) {
            String s = l.get(i); int k = s.indexOf('=');
            if (k > 0 && s.substring(0, k).trim().equalsIgnoreCase(clave)) return s.substring(k + 1).trim();
        }
        return def;
    }
    public int getInt(String sec, String clave, int def) {
        try { return Integer.parseInt(get(sec, clave, Integer.toString(def))); } catch (Exception e) { return def; }
    }
    public void set(String sec, String clave, String v) {
        int i = seccion(sec);
        if (i < 0) { l.add("[" + sec + "]"); l.add(clave + "=" + v); return; }
        int fin = i + 1;
        for (int j = i + 1; j < l.size() && !l.get(j).trim().startsWith("["); j++) {
            String s = l.get(j); int k = s.indexOf('=');
            if (k > 0 && s.substring(0, k).trim().equalsIgnoreCase(clave)) { l.set(j, clave + "=" + v); return; }
            if (!s.trim().isEmpty()) fin = j + 1;
        }
        l.add(fin, clave + "=" + v);
    }
    public void setInt(String sec, String clave, int v) { set(sec, clave, Integer.toString(v)); }
    public boolean guardar() {
        try {
            new File(CARPETA).mkdirs();
            File tmp = new File(RUTA + ".tmp");
            try (Writer w = new OutputStreamWriter(new FileOutputStream(tmp), StandardCharsets.UTF_8)) {
                for (String s : l) { w.write(s); w.write("\n"); }
            }
            return tmp.renameTo(new File(RUTA)) || copiar(tmp);
        } catch (Exception e) { return false; }
    }
    private boolean copiar(File tmp) {
        File d = new File(RUTA); d.delete();
        return tmp.renameTo(d);
    }
}
