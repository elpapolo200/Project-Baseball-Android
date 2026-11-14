package org.libsdl.app;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.CRC32;

/* «Preparar el juego»: genera libjuego.so DENTRO del teléfono a partir del eboot.elf de la copia del jugador
 * (como el launcher de PC): armrecomp traduce el eboot a C, partir lo trocea, clang compila los trozos en
 * paralelo y lld enlaza contra libmain.so. Las herramientas van en la APK como lib*.so (Android solo deja
 * ejecutar lo que viene en la APK) y sus datos (base de NIDs, cabeceras) en los assets. Nada del juego va en
 * la APK: libjuego.so se queda en la carpeta ProjectBaseball de cada uno. */
public class PreparacionActivity extends Activity {
    static final String HUELLA = "libjuego.txt";
    private static final int FONDO = Color.rgb(18, 20, 26), TEXTO = Color.rgb(235, 236, 240), GRIS = Color.rgb(150, 155, 170),
                             ACENTO = Color.rgb(58, 130, 246), MAL = Color.rgb(230, 90, 90);
    private TextView paso, detalle;
    private ProgressBar barra;
    private Button boton;
    private volatile boolean cancelar;
    private final List<Process> procesos = new ArrayList<>();
    private boolean en;

    private int dp(float v) { return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()); }
    private String t(String es, String ingl) { return en ? ingl : es; }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        en = OpcionesActivity.ingles(new Ini());
        LinearLayout v = new LinearLayout(this);
        v.setOrientation(LinearLayout.VERTICAL); v.setGravity(Gravity.CENTER); v.setBackgroundColor(FONDO);
        v.setPadding(dp(48), dp(24), dp(48), dp(24));
        TextView tit = new TextView(this); tit.setText(t("Preparando el juego", "Preparing the game"));
        tit.setTextColor(TEXTO); tit.setTextSize(24); tit.setTypeface(Typeface.DEFAULT_BOLD);
        v.addView(tit);
        TextView nota = new TextView(this);
        nota.setText(t("Solo la primera vez: el juego se traduce para tu teléfono a partir de tu eboot (unos minutos). "
                       + "No salgas de la app ni bloquees la pantalla.",
                       "Only the first time: the game is translated for your phone from your eboot (a few minutes). "
                       + "Don't leave the app or lock the screen."));
        nota.setTextColor(GRIS); nota.setTextSize(13); nota.setPadding(0, dp(6), 0, dp(18));
        v.addView(nota);
        paso = new TextView(this); paso.setTextColor(TEXTO); paso.setTextSize(16); v.addView(paso);
        barra = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        barra.setMax(1000);
        LinearLayout.LayoutParams lb = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(12));
        lb.topMargin = dp(10); v.addView(barra, lb);
        detalle = new TextView(this); detalle.setTextColor(GRIS); detalle.setTextSize(12); detalle.setPadding(0, dp(6), 0, 0);
        v.addView(detalle);
        boton = new Button(this); boton.setText(t("Cancelar", "Cancel")); boton.setAllCaps(false);
        LinearLayout.LayoutParams lbt = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lbt.topMargin = dp(20); lbt.gravity = Gravity.CENTER_HORIZONTAL;
        boton.setOnClickListener(x -> { cancelar = true; matar(); finish(); });
        v.addView(boton, lbt);
        setContentView(v);
        new Thread(this::trabajo, "Preparar").start();
    }

    @Override
    public void onBackPressed() { /* no salir a medias: solo con Cancelar */ }

    private void estado(String p, String d, int milesimas) {
        runOnUiThread(() -> { if (p != null) paso.setText(p); if (d != null) detalle.setText(d); if (milesimas >= 0) barra.setProgress(milesimas); });
    }
    private void matar() { synchronized (procesos) { for (Process p : procesos) p.destroy(); } }

    /* ---------------------------------------------------------------------------------------------- */

    /* Huella: tamaño + CRC del eboot y versión de la app. Si cambia cualquiera, hay que preparar otra vez. */
    static String huella(android.content.Context c, File elf) {
        long ver = 0;
        try { PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0); ver = pi.getLongVersionCode(); } catch (Exception e) { }
        CRC32 crc = new CRC32();
        try (InputStream in = new FileInputStream(elf)) {
            byte[] b = new byte[1 << 16]; int n;
            while ((n = in.read(b)) > 0) crc.update(b, 0, n);
        } catch (Exception e) { return ""; }
        return "eboot " + elf.length() + " " + Long.toHexString(crc.getValue()) + " | app " + ver;
    }
    /* ¿Hace falta preparar? No hay libjuego.so, o la preparó la app y su huella ya no coincide (otro eboot u otra
     * versión de la app). Una libjuego.so puesta a mano sin huella (generada en el PC) se respeta. */
    static boolean faltaPreparar(android.content.Context c) {
        File elf = new File(Ini.CARPETA + "/eboot.elf"), so = new File(Ini.CARPETA + "/libjuego.so"), h = new File(Ini.CARPETA + "/" + HUELLA);
        if (!elf.exists()) return false;   /* sin eboot no se puede: lo dice la pantalla de opciones */
        if (!so.exists()) return true;
        if (!h.exists()) return false;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(h)))) {
            String linea = r.readLine();
            return linea == null || !linea.equals(huella(c, elf));
        } catch (Exception e) { return true; }
    }

    private void copiarAssets(String ruta, File destino) throws Exception {
        String[] hijos = getAssets().list(ruta);
        if (hijos == null || hijos.length == 0) {   /* archivo */
            destino.getParentFile().mkdirs();
            try (InputStream in = getAssets().open(ruta); OutputStream out = new FileOutputStream(destino)) {
                byte[] b = new byte[1 << 16]; int n;
                while ((n = in.read(b)) > 0) out.write(b, 0, n);
            }
            return;
        }
        destino.mkdirs();
        for (String h : hijos) copiarAssets(ruta + "/" + h, new File(destino, h));
    }

    /* Ejecuta una herramienta de la APK; devuelve el código de salida. La salida va a log (y la última línea a «detalle»). */
    private int ejecutar(List<String> cmd, File log, boolean mostrar) throws Exception {
        ProcessBuilder pb = new ProcessBuilder(cmd);
        pb.redirectErrorStream(true);
        if (log != null) pb.redirectOutput(log);
        Process p = pb.start();
        synchronized (procesos) { procesos.add(p); }
        if (log == null && mostrar) {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                String l; while ((l = r.readLine()) != null) { final String s = l; estado(null, s, -1); }
            }
        }
        int rc = p.waitFor();
        synchronized (procesos) { procesos.remove(p); }
        return rc;
    }

    private static void borrar(File f) {
        if (f.isDirectory()) { File[] h = f.listFiles(); if (h != null) for (File x : h) borrar(x); }
        f.delete();
    }

    private void trabajo() {
        long t0 = System.currentTimeMillis();
        File base = new File(getFilesDir(), "generador"), gen = new File(getFilesDir(), "gen");
        File partes = new File(gen, "partes"), obj = new File(gen, "obj");
        String bin = getApplicationInfo().nativeLibraryDir;
        File elf = new File(Ini.CARPETA + "/eboot.elf"), salida = new File(Ini.CARPETA + "/libjuego.so");
        try {
            borrar(gen); partes.mkdirs(); obj.mkdirs();
            estado(t("1/4 Preparando herramientas", "1/4 Setting up tools"), "", 10);
            borrar(base);
            copiarAssets("generador", base);

            /* 2. traducir */
            estado(t("2/4 Traduciendo el juego", "2/4 Translating the game"), "armrecomp", 40);
            File c = new File(gen, "juego.c");
            int rc = ejecutar(Arrays.asList(bin + "/libarmrecomp.so", "emit", elf.getPath(), c.getPath(), "999999", new File(base, "db").getPath()),
                              new File(gen, "armrecomp.log"), false);
            if (cancelar) return;
            if (rc != 0 || !c.exists()) throw new Exception("armrecomp: " + rc + " (" + ultima(new File(gen, "armrecomp.log")) + ")");
            estado(null, String.format("C: %d MB", c.length() >> 20), 200);
            rc = ejecutar(Arrays.asList(bin + "/libpartir.so", c.getPath(), partes.getPath(), "12"), new File(gen, "partir.log"), false);
            c.delete();
            if (cancelar) return;
            if (rc != 0) throw new Exception("partir: " + rc + " (" + ultima(new File(gen, "partir.log")) + ")");

            /* 3. compilar en paralelo */
            File[] trozos = partes.listFiles((d, n) -> n.endsWith(".c"));
            if (trozos == null || trozos.length == 0) throw new Exception("sin trozos");
            Arrays.sort(trozos);
            List<File> fuentes = new ArrayList<>(Arrays.asList(trozos));
            File jb = new File(base, "juego_base_android.c");
            fuentes.add(jb);
            int nucleos = Runtime.getRuntime().availableProcessors();
            android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
            android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo(); am.getMemoryInfo(mi);
            int porMem = (int) Math.max(1, (mi.availMem >> 20) / 700);   /* medido en el S22: 400-520 MB por clang (trozos de 12 MB) */
            int hilos = Math.max(1, Math.min(Math.min(nucleos - 1, porMem), 6));
            final List<String> comunes = Arrays.asList("--target=aarch64-linux-android29", "-march=armv8.2-a", "-O2", "-fPIC", "-ffreestanding",
                "-nostdinc", "-fno-stack-protector", "-fno-math-errno", "-ffp-contract=off", "-fasynchronous-unwind-tables", "-w",
                "-DVITA_MEM_DIRECTA", "-isystem", new File(base, "include/clang").getPath(), "-I", new File(base, "include/shim").getPath(),
                "-I", new File(base, "include").getPath());
            final AtomicInteger hechos = new AtomicInteger(), siguiente = new AtomicInteger();
            final String[] error = { null };
            final int total = fuentes.size();
            estado(t("3/4 Compilando", "3/4 Compiling"), String.format(t("%d trozos, %d a la vez", "%d chunks, %d at a time"), total, hilos), 250);
            List<Thread> ts = new ArrayList<>();
            for (int k = 0; k < hilos; k++) {
                Thread th = new Thread(() -> {
                    while (!cancelar && error[0] == null) {
                        int i = siguiente.getAndIncrement();
                        if (i >= total) break;
                        File s = fuentes.get(i);
                        File o = new File(obj, s.getName().replace(".c", ".o"));
                        List<String> cmd = new ArrayList<>(); cmd.add(bin + "/libclang.so"); cmd.addAll(comunes);
                        if (s.equals(jb)) cmd.add("-fno-builtin");   /* las cabeceras del runtime ya están en include/vitarecomp */
                        cmd.add("-c"); cmd.add(s.getPath()); cmd.add("-o"); cmd.add(o.getPath());
                        try {
                            File log = new File(obj, s.getName() + ".log");
                            if (ejecutar(cmd, log, false) != 0) { error[0] = "clang " + s.getName() + ": " + ultima(log); break; }
                            if (!s.equals(jb)) s.delete();   /* liberar espacio sobre la marcha */
                        } catch (Exception e) { error[0] = e.toString(); break; }
                        int h = hechos.incrementAndGet();
                        estado(null, String.format(t("%d de %d trozos", "%d of %d chunks"), h, total), 250 + 650 * h / total);
                    }
                });
                th.start(); ts.add(th);
            }
            for (Thread th : ts) th.join();
            if (cancelar) return;
            if (error[0] != null) throw new Exception(error[0]);

            /* 4. enlazar contra libmain.so */
            estado(t("4/4 Enlazando", "4/4 Linking"), "lld", 910);
            List<String> cmd = new ArrayList<>(Arrays.asList(bin + "/liblld.so", "-flavor", "gnu", "-shared", "-o", new File(gen, "libjuego.so").getPath(),
                "-soname", "libjuego.so", "--version-script", new File(base, "libjuego.map").getPath(), "-z", "noexecstack",
                "-z", "max-page-size=16384", "--hash-style=gnu", "--as-needed", "-O1"));
            File[] objs = obj.listFiles((d, n) -> n.endsWith(".o"));
            Arrays.sort(objs);
            for (File o : objs) cmd.add(o.getPath());
            cmd.add(bin + "/libmain.so");
            rc = ejecutar(cmd, new File(gen, "lld.log"), false);
            if (cancelar) return;
            File so = new File(gen, "libjuego.so");
            if (rc != 0 || !so.exists()) throw new Exception("lld: " + rc + " (" + ultima(new File(gen, "lld.log")) + ")");

            /* a la carpeta del juego + huella */
            copiarArchivo(so, salida);
            try (OutputStream o = new FileOutputStream(new File(Ini.CARPETA + "/" + HUELLA))) { o.write((huella(this, elf) + "\n").getBytes("UTF-8")); }
            borrar(gen);
            long s = (System.currentTimeMillis() - t0) / 1000;
            estado(t("Listo", "Done"), String.format(t("Preparado en %d min %d s", "Prepared in %d min %d s"), s / 60, s % 60), 1000);
            runOnUiThread(() -> {
                boton.setText(t("JUGAR", "PLAY"));
                boton.setOnClickListener(x -> { startActivity(new Intent(this, ProjectBaseballActivity.class)); finish(); });
            });
        } catch (Exception e) {
            if (cancelar) return;
            final String m = e.getMessage() != null ? e.getMessage() : e.toString();
            android.util.Log.e("ProjectBaseball", "preparar: " + m);
            runOnUiThread(() -> {
                paso.setText(t("No se pudo preparar el juego", "Could not prepare the game")); paso.setTextColor(MAL);
                detalle.setText(m);
                boton.setText(t("Volver", "Back"));
                boton.setOnClickListener(x -> { startActivity(new Intent(this, OpcionesActivity.class)); finish(); });
            });
        }
    }

    private static void copiarArchivo(File a, File b) throws Exception {
        File tmp = new File(b.getPath() + ".tmp");
        try (InputStream in = new FileInputStream(a); OutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 20]; int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        b.delete();
        if (!tmp.renameTo(b)) throw new Exception("no pude escribir " + b);
    }
    private static String ultima(File log) {
        String u = "";
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(log)))) {
            String l; while ((l = r.readLine()) != null) if (!l.trim().isEmpty()) u = l.trim();
        } catch (Exception e) { }
        return u.length() > 200 ? u.substring(0, 200) : u;
    }
}
