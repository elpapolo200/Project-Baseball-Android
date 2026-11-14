package org.libsdl.app;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.WindowManager;
import android.widget.Toast;

/* Project Baseball: la actividad de SDL2 más el permiso para la carpeta pública ProjectBaseball
 * (ahí el jugador copia su carpeta del juego, eboot.elf y libjuego.so) y pantalla siempre encendida. */
public class ProjectBaseballActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        fijarFrecuencia();
        /* Rendimiento sostenido: el sistema mantiene la CPU/GPU en un nivel que aguanta sin calentarse
         * en vez de subir a tope y luego frenar por temperatura ([android] sostenido=0 lo quita). */
        try {
            android.os.PowerManager pm = (android.os.PowerManager) getSystemService(POWER_SERVICE);
            boolean sost = new Ini().getInt("android", "sostenido", 1) != 0;
            if (sost && pm != null && pm.isSustainedPerformanceModeSupported()) {
                getWindow().setSustainedPerformanceMode(true);
                android.util.Log.i("ProjectBaseball", "rendimiento sostenido: activado");
            } else android.util.Log.i("ProjectBaseball", "rendimiento sostenido: " + (sost ? "no disponible" : "desactivado"));
        } catch (Exception e) { android.util.Log.w("ProjectBaseball", "rendimiento sostenido: " + e); }
        if (mLayout != null) {   /* botones en pantalla encima del juego (tamaño/opacidad: pantalla de opciones) */
            MandoEnPantalla m = new MandoEnPantalla(this);
            Ini ini = new Ini();
            m.config(ini.getInt("pantalla_tactil", "botones", 1) != 0, ini.getInt("pantalla_tactil", "tamano", 100) / 100f,
                     Math.max(20, Math.min(100, ini.getInt("pantalla_tactil", "opacidad", 80))) / 100f);
            m.vibracion(ini.getInt("pantalla_tactil", "vibracion", 1) != 0);
            m.recorrido(ini.getInt("pantalla_tactil", "recorrido", 100));
            mLayout.addView(m, new android.view.ViewGroup.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT, android.view.ViewGroup.LayoutParams.MATCH_PARENT));
            if (ini.getInt("video", "mostrar_temp", 1) != 0)   /* temperatura del teléfono bajo los FPS */
                mLayout.addView(new TemperaturaEnPantalla(this), new android.view.ViewGroup.LayoutParams(
                    android.view.ViewGroup.LayoutParams.MATCH_PARENT, android.view.ViewGroup.LayoutParams.MATCH_PARENT));
        }
        /* el permiso se pide en OpcionesActivity; esto queda solo por si se abre directamente */
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            Toast.makeText(this, (OpcionesActivity.ingles(new Ini()) ? "Project Baseball needs \"All files access\" to read the ProjectBaseball folder" : "Project Baseball necesita «Acceso a todos los archivos» para leer la carpeta ProjectBaseball"),
                           Toast.LENGTH_LONG).show();
            try {
                Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                i.setData(Uri.parse("package:" + getPackageName()));
                startActivity(i);
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else if (Build.VERSION.SDK_INT < 30) {
            requestPermissions(new String[] { "android.permission.READ_EXTERNAL_STORAGE", "android.permission.WRITE_EXTERNAL_STORAGE" }, 1);
        }
    }

    /* Frecuencia de la pantalla mientras se juega: la del límite de FPS ([video] fps_max) o [android] hz.
     * El S22 va a 120 Hz por defecto: el panel y el compositor trabajarían 4 veces más de lo que el
     * juego necesita a 30 FPS (calor y batería). Se elige el modo de la pantalla con esa frecuencia y la
     * misma resolución; si no existe, el más cercano por encima. */
    private void fijarFrecuencia() {
        try {
            Ini ini = new Ini();
            int hz = ini.getInt("android", "hz", 0);
            if (hz <= 0) hz = 30;   /* el juego va siempre a 30 FPS en Android */
            android.view.Display d = getWindowManager().getDefaultDisplay();
            android.view.Display.Mode actual = d.getMode(), mejor = null;
            for (android.view.Display.Mode m : d.getSupportedModes()) {
                if (m.getPhysicalWidth() != actual.getPhysicalWidth() || m.getPhysicalHeight() != actual.getPhysicalHeight()) continue;
                float r = m.getRefreshRate();
                if (r + 0.5f < hz) continue;
                if (mejor == null || r < mejor.getRefreshRate()) mejor = m;
            }
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            if (mejor != null) lp.preferredDisplayModeId = mejor.getModeId();
            lp.preferredRefreshRate = hz;
            getWindow().setAttributes(lp);
            android.util.Log.i("ProjectBaseball", "pantalla: " + hz + " Hz pedidos, modo " + (mejor != null ? mejor.getModeId() + " (" + mejor.getRefreshRate() + " Hz)" : "ninguno"));
        } catch (Exception e) {
            android.util.Log.w("ProjectBaseball", "no pude fijar la frecuencia: " + e);
        }
    }

    /* Lista de partidas (la llama el juego por JNI desde su hilo, dialogos_android.c): muestra las
     * ranuras y espera a que el jugador elija. Devuelve el índice o -1 si cancela. */
    public static int elegirPartida(final String titulo, final String[] items, final int sel) {
        final android.app.Activity a = (android.app.Activity) mSingleton;
        if (a == null) return sel;
        final java.util.concurrent.CountDownLatch hecho = new java.util.concurrent.CountDownLatch(1);
        final int[] r = { -1 };
        a.runOnUiThread(new Runnable() {
            public void run() {
                android.app.AlertDialog.Builder b = new android.app.AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert);
                b.setTitle(titulo);
                b.setItems(items, new android.content.DialogInterface.OnClickListener() {
                    public void onClick(android.content.DialogInterface d, int w) { r[0] = w; hecho.countDown(); }
                });
                b.setNegativeButton(OpcionesActivity.ingles(new Ini()) ? "Cancel" : "Cancelar", new android.content.DialogInterface.OnClickListener() {
                    public void onClick(android.content.DialogInterface d, int w) { r[0] = -1; hecho.countDown(); }
                });
                b.setOnCancelListener(new android.content.DialogInterface.OnCancelListener() {
                    public void onCancel(android.content.DialogInterface d) { r[0] = -1; hecho.countDown(); }
                });
                b.show();
            }
        });
        try { hecho.await(); } catch (InterruptedException e) { }
        return r[0];
    }

    /* Multijugador: OpcionesActivity pasa "multi" (host o la IP del anfitrión); llega a SDL_main como argumento. */
    @Override
    protected String[] getArguments() {
        String m = getIntent() != null ? getIntent().getStringExtra("multi") : null;
        return m == null ? new String[0] : new String[] { "--multi=" + m };
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }
}
