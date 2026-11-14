package org.libsdl.app;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.text.InputType;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Collections;

/* Pantalla de opciones antes de jugar (como la ventana de opciones del launcher de PC): gráficos,
 * botones en pantalla y comprobación de la carpeta. Lo guarda en ProjectBaseball.ini y abre el juego
 * (ProjectBaseballActivity). Hecha con vistas en código: sin XML. */
public class OpcionesActivity extends Activity {
    private static final int FONDO = Color.rgb(18, 20, 26), PANEL = Color.rgb(30, 33, 42), TEXTO = Color.rgb(235, 236, 240),
                             GRIS = Color.rgb(150, 155, 170), ACENTO = Color.rgb(58, 130, 246), OK = Color.rgb(70, 190, 120),
                             MAL = Color.rgb(230, 90, 90);
    private Ini ini;
    private LinearLayout raiz;
    private TextView estado;
    private Button jugar;
    /* valores */
    private int res, fps, brillo, nitidez, tamBotones, opacidad, zoom, publico;
    private boolean verFps, verTemp, suavizado, sombras, jugAlta, botones;
    private static boolean en;   /* textos en inglés ([android] idioma=en) */

    /* Idioma de la app: [android] idioma = es (por defecto) / en (selector «Idioma / Language»). */
    static boolean ingles(Ini ini) {
        return ini.get("android", "idioma", "es").equals("en");
    }
    private static String t(String es, String ingl) { return en ? ingl : es; }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setBackgroundColor(FONDO);
    }

    @Override
    protected void onResume() {
        super.onResume();
        construir();   /* al volver de dar el permiso se relee todo */
    }

    private boolean permiso() {
        return Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
    }
    private void pedirPermiso() {
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                i.setData(Uri.parse("package:" + getPackageName()));
                startActivity(i);
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            requestPermissions(new String[] { "android.permission.READ_EXTERNAL_STORAGE", "android.permission.WRITE_EXTERNAL_STORAGE" }, 1);
        }
    }
    @Override
    public void onRequestPermissionsResult(int c, String[] p, int[] r) { construir(); }

    private int dp(float v) { return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()); }
    private GradientDrawable fondo(int color, float radio) {
        GradientDrawable g = new GradientDrawable(); g.setColor(color); g.setCornerRadius(dp(radio)); return g;
    }
    private TextView texto(String s, float sp, int color, boolean negrita) {
        TextView t = new TextView(this); t.setText(s); t.setTextSize(sp); t.setTextColor(color);
        if (negrita) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private void construir() {
        ini = new Ini();
        en = ingles(ini);
        res = clamp(ini.getInt("video", "resolucion", 1), 1, 2);
        /* segmento: 0 = x1, 1 = x1,5 ([video] media=1), 2 = x2 */
        res = res >= 2 ? 2 : (ini.getInt("video", "media", 0) != 0 ? 1 : 0);
        fps = 30;   /* fijo en Android */
        verFps = ini.getInt("video", "mostrar_fps", 0) != 0;
        verTemp = ini.getInt("video", "mostrar_temp", 1) != 0;
        brillo = clamp(ini.getInt("video", "brillo", 85), 50, 120);
        nitidez = clamp(ini.getInt("video", "nitidez", 35), 0, 100);
        zoom = clamp(ini.getInt("video", "zoom", 100), 100, 125);
        suavizado = ini.getInt("video", "suavizado", 1) != 0;
        sombras = ini.getInt("video", "sombra_jugadores", 1) != 0;
        jugAlta = ini.getInt("video", "jugadores_alta", 1) != 0;
        /* segmento: 0 = sin público, 1 = cartón ([android] publico=2), 2 = completo (publico=1) */
        { int p = ini.getInt("android", "publico", 0); publico = p == 2 ? 1 : p == 1 ? 2 : 0; }
        botones = ini.getInt("pantalla_tactil", "botones", 1) != 0;
        tamBotones = clamp(ini.getInt("pantalla_tactil", "tamano", 100), 70, 140);
        opacidad = clamp(ini.getInt("pantalla_tactil", "opacidad", 80), 20, 100);

        ScrollView sv = new ScrollView(this);
        raiz = new LinearLayout(this);
        raiz.setOrientation(LinearLayout.VERTICAL);
        raiz.setPadding(dp(24), dp(18), dp(24), dp(24));
        sv.addView(raiz);

        /* cabecera: título + botón JUGAR */
        LinearLayout cab = new LinearLayout(this);
        cab.setOrientation(LinearLayout.HORIZONTAL); cab.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout tit = new LinearLayout(this); tit.setOrientation(LinearLayout.VERTICAL);
        tit.addView(texto("Project Baseball", 26, TEXTO, true));
        tit.addView(texto(t("port nativo de MLB 15 The Show · opciones", "native port of MLB 15 The Show · options"), 13, GRIS, false));
        cab.addView(tit, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        jugar = new Button(this);
        jugar.setText(t("JUGAR", "PLAY")); jugar.setTextSize(18); jugar.setTypeface(Typeface.DEFAULT_BOLD); jugar.setTextColor(Color.WHITE);
        jugar.setBackground(fondo(ACENTO, 12)); jugar.setPadding(dp(36), dp(12), dp(36), dp(12));
        jugar.setOnClickListener(v -> empezar());
        cab.addView(jugar);
        raiz.addView(cab);

        /* estado de la carpeta */
        estado = texto("", 13, GRIS, false);
        estado.setPadding(dp(14), dp(10), dp(14), dp(10));
        estado.setBackground(fondo(PANEL, 10));
        LinearLayout.LayoutParams lpE = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lpE.topMargin = dp(14);
        raiz.addView(estado, lpE);
        comprobar();

        /* dos columnas */
        LinearLayout cols = new LinearLayout(this); cols.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout c1 = panel(), c2 = panel();
        LinearLayout.LayoutParams lc = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
        lc.topMargin = dp(14); lc.rightMargin = dp(7);
        LinearLayout.LayoutParams lc2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
        lc2.topMargin = dp(14); lc2.leftMargin = dp(7);
        cols.addView(c1, lc); cols.addView(c2, lc2);
        raiz.addView(cols);

        c1.addView(segmento("Idioma / Language", new String[] { "Español", "English" }, en ? 1 : 0, i -> {
            ini.set("android", "idioma", i == 1 ? "en" : "es");
            if (guardar()) construir();
        }, null));
        TextView gr = texto(t("Gráficos", "Graphics"), 17, TEXTO, true); gr.setPadding(0, dp(16), 0, 0);
        c1.addView(gr);
        c1.addView(segmento(t("Resolución interna", "Internal resolution"), new String[] { "x1", "x1,5", "x2" }, res, i -> res = i,
                            t("x1 = como la Vita (recomendado). x1,5 = algo más nítido y calienta un poco más. x2 = lo más nítido, pero la GPU trabaja el doble y calienta más.", "x1 = like the Vita (recommended). x1.5 = a bit sharper, slightly warmer. x2 = sharpest, but the GPU works twice as hard and gets hotter.")));
        /* El juego va siempre a 30 FPS en Android (sin opción): la Vita lo hacía a 30 y a 60 solo calienta */
        c1.addView(deslizador(t("Brillo", "Brightness"), 50, 120, brillo, "%", v -> brillo = v));
        c1.addView(deslizador(t("Nitidez", "Sharpness"), 0, 100, nitidez, "%", v -> nitidez = v));
        c1.addView(deslizador(t("Ampliar imagen (recorta arriba y abajo)", "Zoom image (crops top and bottom)"), 100, 125, zoom, "%", v -> zoom = v));
        c1.addView(interruptor(t("Suavizado de bordes (FXAA)", "Edge smoothing (FXAA)"), suavizado, v -> suavizado = v));
        c1.addView(interruptor(t("Sombras de los jugadores", "Player shadows"), sombras, v -> sombras = v));
        c1.addView(interruptor(t("Jugadores en alta calidad", "High quality players"), jugAlta, v -> jugAlta = v));
        c1.addView(interruptor(t("Mostrar FPS", "Show FPS"), verFps, v -> verFps = v));
        c1.addView(interruptor(t("Mostrar temperatura del teléfono", "Show phone temperature"), verTemp, v -> verTemp = v));
        c1.addView(segmento(t("Público en las gradas", "Crowd in the stands"), new String[] { "No", t("Cartón", "Cardboard"), t("Completo", "Full") }, publico, i -> publico = i,
                            t("Cartón = público que se mueve poco y casi no gasta. Completo = como en la Vita, más calor y batería.", "Cardboard = crowd that barely moves and costs almost nothing. Full = like the Vita, more heat and battery.")));

        c2.addView(texto(t("Botones en pantalla", "On-screen buttons"), 17, TEXTO, true));
        c2.addView(interruptor(t("Mostrar los botones al empezar", "Show buttons at start"), botones, v -> botones = v));
        c2.addView(deslizador(t("Tamaño", "Size"), 70, 140, tamBotones, "%", v -> tamBotones = v));
        c2.addView(deslizador(t("Opacidad", "Opacity"), 20, 100, opacidad, "%", v -> opacidad = v));
        TextView ayuda = texto(t("Durante el juego, el botón ◐ de arriba oculta o muestra los botones. Tocar fuera de los botones ", "During the game, the ◐ button at the top hides or shows the buttons. Touching outside the buttons ")
                               + t("es la pantalla táctil de la Vita. También sirve un mando Bluetooth.", "is the Vita touch screen. A Bluetooth controller also works."), 12, GRIS, false);
        ayuda.setPadding(0, dp(10), 0, 0);
        c2.addView(ayuda);

        Button rest = new Button(this);
        rest.setText(t("Valores por defecto", "Defaults")); rest.setTextColor(TEXTO); rest.setAllCaps(false);
        rest.setBackground(fondo(Color.rgb(48, 52, 64), 10));
        LinearLayout.LayoutParams lr = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lr.topMargin = dp(16);
        rest.setOnClickListener(v -> {
            res = 1; fps = 30; verFps = false; verTemp = true; brillo = 85; nitidez = 35; zoom = 100; suavizado = true; sombras = true; jugAlta = true; publico = 0;
            botones = true; tamBotones = 100; opacidad = 80;
            if (guardar()) construir();
        });
        c2.addView(rest, lr);

        /* Multijugador en la misma wifi (docs/MULTIJUGADOR.md): el juego trae un modo de conexión directa por IP
         * (olHost / olConnect=). Crear sala = anfitrión; Unirse = conectar al código del amigo. */
        LinearLayout c3 = panel();
        LinearLayout.LayoutParams lc3 = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lc3.topMargin = dp(16);
        c2.addView(c3, lc3);
        c3.addView(texto(t("Multijugador (misma wifi)", "Multiplayer (same Wi-Fi)"), 17, TEXTO, true));
        TextView exp = texto(t("EXPERIMENTAL", "EXPERIMENTAL"), 12, Color.rgb(250, 190, 60), true);
        exp.setPadding(0, dp(2), 0, 0);
        c3.addView(exp);
        TextView exa = texto(t("Todavía en pruebas: el partido puede cortarse o desincronizarse. Los dos teléfonos con la misma versión de la app.", "Still being tested: the game may drop or go out of sync. Both phones need the same app version."), 12, Color.rgb(250, 190, 60), false);
        c3.addView(exa);
        String ip = ipWifi();
        c3.addView(texto(ip == null ? t("Conéctate a una wifi para jugar con otro teléfono.", "Connect to Wi-Fi to play with another phone.") :
                         t("Tu código de sala: ", "Your room code: ") + codigoDe(ip), 14, ip == null ? MAL : OK, false));
        TextView am = texto(t("Los dos en la misma wifi. Uno toca «Crear sala» y el otro escribe su código y toca «Unirse». ", "Both on the same Wi-Fi. One taps \"Create room\" and the other types that code and taps \"Join\". ")
                            + t("Al empezar una Exhibición el partido es entre los dos (el que crea la sala juega en casa).", "When an Exhibition starts, the game is between you two (the room creator plays at home)."), 12, GRIS, false);
        am.setPadding(0, dp(6), 0, 0);
        c3.addView(am);
        Button crear = new Button(this);
        crear.setText(t("Crear sala", "Create room")); crear.setTextColor(Color.WHITE); crear.setAllCaps(false);
        crear.setBackground(fondo(ACENTO, 10));
        LinearLayout.LayoutParams lcr = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(44));
        lcr.topMargin = dp(10);
        crear.setOnClickListener(v -> empezar("host"));
        c3.addView(crear, lcr);
        LinearLayout fila = new LinearLayout(this); fila.setOrientation(LinearLayout.HORIZONTAL);
        final EditText cod = new EditText(this);
        cod.setHint(t("Código del amigo", "Friend's code")); cod.setTextColor(TEXTO); cod.setHintTextColor(GRIS);
        cod.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL); cod.setSingleLine(true);
        cod.setText(ini.get("multijugador", "ultimo_codigo", ""));
        fila.addView(cod, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        Button unir = new Button(this);
        unir.setText(t("Unirse", "Join")); unir.setTextColor(TEXTO); unir.setAllCaps(false);
        unir.setBackground(fondo(Color.rgb(48, 52, 64), 10));
        unir.setOnClickListener(v -> {
            String destino = ipDeCodigo(cod.getText().toString().trim(), ipWifi());
            if (destino == null) { Toast.makeText(this, t("Código no válido (son números como 1.23)", "Invalid code (numbers like 1.23)"), Toast.LENGTH_LONG).show(); return; }
            ini.set("multijugador", "ultimo_codigo", cod.getText().toString().trim());
            empezar(destino);
        });
        LinearLayout.LayoutParams lu = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(44));
        lu.leftMargin = dp(8);
        fila.addView(unir, lu);
        LinearLayout.LayoutParams lf = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lf.topMargin = dp(8);
        c3.addView(fila, lf);

        setContentView(sv);
    }

    private LinearLayout panel() {
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        p.setPadding(dp(16), dp(14), dp(16), dp(16));
        p.setBackground(fondo(PANEL, 12));
        return p;
    }

    interface AlElegir { void elegido(int i); }
    interface AlCambiar { void valor(int v); }
    interface AlMarcar { void marcado(boolean v); }

    private View segmento(String titulo, String[] ops, int sel, AlElegir f, String nota) {
        LinearLayout v = new LinearLayout(this); v.setOrientation(LinearLayout.VERTICAL); v.setPadding(0, dp(12), 0, 0);
        v.addView(texto(titulo, 14, TEXTO, false));
        LinearLayout fila = new LinearLayout(this); fila.setOrientation(LinearLayout.HORIZONTAL); fila.setPadding(0, dp(6), 0, 0);
        final Button[] bs = new Button[ops.length];
        for (int i = 0; i < ops.length; i++) {
            final int k = i;
            Button b = new Button(this);
            b.setText(ops[i]); b.setAllCaps(false); b.setTextSize(15); b.setMinHeight(dp(40)); b.setMinimumHeight(dp(40));
            b.setOnClickListener(x -> { f.elegido(k); for (int j = 0; j < bs.length; j++) pinta(bs[j], j == k); });
            bs[i] = b; pinta(b, i == sel);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(42), 1);
            if (i > 0) lp.leftMargin = dp(6);
            fila.addView(b, lp);
        }
        v.addView(fila);
        if (nota != null) { TextView n = texto(nota, 11, GRIS, false); n.setPadding(0, dp(4), 0, 0); v.addView(n); }
        return v;
    }
    private void pinta(Button b, boolean on) {
        b.setBackground(fondo(on ? ACENTO : Color.rgb(48, 52, 64), 8));
        b.setTextColor(on ? Color.WHITE : TEXTO);
    }
    private View deslizador(String titulo, int min, int max, int val, String unidad, AlCambiar f) {
        LinearLayout v = new LinearLayout(this); v.setOrientation(LinearLayout.VERTICAL); v.setPadding(0, dp(12), 0, 0);
        TextView t = texto(titulo + ": " + val + unidad, 14, TEXTO, false);
        v.addView(t);
        SeekBar s = new SeekBar(this);
        s.setMax(max - min); s.setProgress(val - min);
        s.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar sb, int p, boolean u) { f.valor(p + min); t.setText(titulo + ": " + (p + min) + unidad); }
            public void onStartTrackingTouch(SeekBar sb) {}
            public void onStopTrackingTouch(SeekBar sb) {}
        });
        v.addView(s);
        return v;
    }
    private View interruptor(String titulo, boolean val, AlMarcar f) {
        Switch s = new Switch(this);
        s.setText(titulo); s.setTextColor(TEXTO); s.setTextSize(14); s.setChecked(val);
        s.setPadding(0, dp(10), 0, 0);
        s.setOnCheckedChangeListener((x, c) -> f.marcado(c));
        return s;
    }

    private static int clamp(int v, int a, int b) { return v < a ? a : v > b ? b : v; }

    /* Comprueba lo que necesita el juego en la carpeta ProjectBaseball (igual que launcher_android.c). */
    private boolean comprobar() {
        if (!permiso()) {
            estado.setText(t("✗ Falta el permiso «Acceso a todos los archivos» (toca JUGAR para darlo).", "✗ Missing \"All files access\" permission (tap PLAY to grant it)."));
            estado.setTextColor(MAL);
            return false;
        }
        StringBuilder sb = new StringBuilder();
        boolean todo = true;
        File arc = new File(Ini.CARPETA + "/PCSA00511/VITA_COMPRESSED_WAD.arc");
        String carpeta = ini.get("juego", "carpeta", "");
        if (!arc.exists() && !carpeta.isEmpty()) arc = new File(carpeta + "/VITA_COMPRESSED_WAD.arc");
        boolean arcOk = arc.exists(), claro = false;
        if (arcOk) {
            try (FileInputStream f = new FileInputStream(arc)) {
                byte[] m = new byte[4];
                claro = f.read(m) == 4 && m[0] == 'P' && m[1] == 'S' && m[2] == 'A' && m[3] == 'R';
            } catch (Exception e) { claro = false; }
        }
        sb.append(arcOk ? (claro ? t("✓ Carpeta del juego (PCSA00511)", "✓ Game folder (PCSA00511)") : t("✗ PCSA00511 está CIFRADA (usa la de Vita3K)", "✗ PCSA00511 is ENCRYPTED (use the Vita3K one)")) : t("✗ Falta la carpeta PCSA00511", "✗ PCSA00511 folder missing"));
        todo &= arcOk && claro;
        File elf = new File(Ini.CARPETA + "/eboot.elf");
        sb.append(elf.length() > 1000000 ? t("     ✓ eboot.elf", "     ✓ eboot.elf") : t("     ✗ falta eboot.elf", "     ✗ eboot.elf missing")); todo &= elf.length() > 1000000;
        File so = new File(Ini.CARPETA + "/libjuego.so");
        sb.append(so.length() > 1000000 ? t("     ✓ libjuego.so", "     ✓ libjuego.so") : t("     ✗ falta libjuego.so", "     ✗ libjuego.so missing")); todo &= so.length() > 1000000;
        if (!todo) sb.append(t("\nTodo va en la memoria interna, carpeta ProjectBaseball (junto a Download, DCIM...).", "\nEverything goes in internal storage, ProjectBaseball folder (next to Download, DCIM...)."));
        estado.setText(sb.toString());
        estado.setTextColor(todo ? OK : MAL);
        return todo;
    }

    private boolean guardar() {
        ini.setInt("video", "resolucion", res == 2 ? 2 : 1);
        ini.setInt("video", "media", res == 1 ? 1 : 0);
        ini.setInt("video", "fps_max", fps);
        ini.setInt("video", "mostrar_fps", verFps ? 1 : 0);
        ini.setInt("video", "mostrar_temp", verTemp ? 1 : 0);
        ini.setInt("video", "brillo", brillo);
        ini.setInt("video", "nitidez", nitidez);
        ini.setInt("video", "zoom", zoom);
        ini.setInt("video", "suavizado", suavizado ? 1 : 0);
        ini.setInt("video", "sombra_jugadores", sombras ? 1 : 0);
        ini.setInt("video", "jugadores_alta", jugAlta ? 1 : 0);
        ini.setInt("android", "publico", publico == 1 ? 2 : publico == 2 ? 1 : 0);
        ini.setInt("pantalla_tactil", "botones", botones ? 1 : 0);
        ini.setInt("pantalla_tactil", "tamano", tamBotones);
        ini.setInt("pantalla_tactil", "opacidad", opacidad);
        boolean ok = ini.guardar();
        if (!ok) Toast.makeText(this, t("No pude guardar ProjectBaseball.ini (¿falta el permiso?)", "Couldn't save ProjectBaseball.ini (missing permission?)"), Toast.LENGTH_LONG).show();
        return ok;
    }

    /* IPv4 de la wifi (o de cualquier interfaz de red local). */
    static String ipWifi() {
        try {
            String otra = null;
            for (NetworkInterface n : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!n.isUp() || n.isLoopback()) continue;
                for (InetAddress a : Collections.list(n.getInetAddresses())) {
                    if (!(a instanceof Inet4Address) || !a.isSiteLocalAddress()) continue;
                    if (n.getName().startsWith("wlan")) return a.getHostAddress();
                    if (otra == null) otra = a.getHostAddress();
                }
            }
            return otra;
        } catch (Exception e) { return null; }
    }
    /* Código de sala = las dos últimas cifras de la IP (192.168.1.23 -> 1.23). */
    static String codigoDe(String ip) {
        String[] p = ip.split("\\.");
        return p.length == 4 ? p[2] + "." + p[3] : ip;
    }
    /* Código -> IP: «23» (misma subred), «1.23» (mismas dos primeras cifras) o la IP entera. */
    static String ipDeCodigo(String c, String mia) {
        String[] p = c.split("\\.");
        for (String x : p) { try { int v = Integer.parseInt(x); if (v < 0 || v > 255) return null; } catch (Exception e) { return null; } }
        if (p.length == 4) return c;
        if (mia == null) return null;
        String[] m = mia.split("\\.");
        if (m.length != 4) return null;
        if (p.length == 2) return m[0] + "." + m[1] + "." + p[0] + "." + p[1];
        if (p.length == 1) return m[0] + "." + m[1] + "." + m[2] + "." + p[0];
        return null;
    }

    private void empezar() { empezar(null); }
    /* multi: null = partida normal, "host" = crear sala, una IP = unirse a esa sala */
    private void empezar(String multi) {
        if (!permiso()) { pedirPermiso(); return; }
        guardar();
        if (!comprobar()) {
            Toast.makeText(this, t("Faltan archivos del juego: mira el recuadro rojo", "Game files missing: see the red box"), Toast.LENGTH_LONG).show();
            return;
        }
        Intent i = new Intent(this, ProjectBaseballActivity.class);
        if (multi != null) i.putExtra("multi", multi);
        startActivity(i);
        finish();
    }
}
