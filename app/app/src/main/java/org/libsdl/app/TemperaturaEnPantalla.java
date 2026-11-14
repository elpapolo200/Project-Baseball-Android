package org.libsdl.app;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.BatteryManager;
import android.os.Build;
import android.os.PowerManager;
import android.view.View;

/* Temperatura del teléfono en pantalla mientras se juega, debajo de los FPS ([video] mostrar_temp).
 * Es la temperatura de la batería que da Android (el sensor que puede leer cualquier app sin permisos y
 * el que mejor refleja lo caliente que está el aparato en la mano); los sensores de CPU y GPU no son
 * legibles para una app normal. El color avisa: blanco normal, amarillo caliente, rojo muy caliente o
 * cuando Android dice que ya está frenando el rendimiento por temperatura.
 * Se lee cada 2 s; no recibe toques (pasan al mando y al juego). */
public class TemperaturaEnPantalla extends View {
    private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float grados = -1000f; private int estado;   /* estado térmico de Android: 0 = normal ... */
    private String texto = "";
    private final IntentFilter filtro = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
    private final Runnable lee = new Runnable() { public void run() { leer(); postDelayed(this, 2000); } };

    public TemperaturaEnPantalla(Context c) {
        super(c);
        p.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD));
        setClickable(false); setFocusable(false);
    }
    @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); removeCallbacks(lee); post(lee); }
    @Override protected void onDetachedFromWindow() { removeCallbacks(lee); super.onDetachedFromWindow(); }
    @Override public boolean onTouchEvent(android.view.MotionEvent e) { return false; }

    private void leer() {
        try {
            Intent i = getContext().registerReceiver(null, filtro);   /* último aviso de batería (no deja nada registrado) */
            float g = i != null ? i.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -10000) / 10f : -1000f;
            int e = 0;
            if (Build.VERSION.SDK_INT >= 29) {
                PowerManager pm = (PowerManager) getContext().getSystemService(Context.POWER_SERVICE);
                if (pm != null) e = pm.getCurrentThermalStatus();
            }
            if (g != grados || e != estado) {
                grados = g; estado = e;
                texto = g > -100f ? String.format(java.util.Locale.US, "%.1f C", g) : "";
                invalidate();
            }
        } catch (Exception ex) { /* sin dato: no se dibuja */ }
    }

    @Override
    protected void onDraw(Canvas c) {
        if (texto.isEmpty()) return;
        float h = getHeight(), t = h * 0.030f, x = h * 0.012f, y = h * 0.075f;
        int color = estado >= 3 || grados >= 44f ? Color.rgb(255, 90, 90)
                  : estado >= 2 || grados >= 40f ? Color.rgb(255, 205, 80) : Color.WHITE;
        p.setTextSize(t);
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(t * 0.16f); p.setColor(Color.argb(200, 0, 0, 0));
        c.drawText(texto, x, y, p);   /* borde oscuro para que se lea sobre cualquier fondo */
        p.setStyle(Paint.Style.FILL); p.setColor(color);
        c.drawText(texto, x, y, p);
    }
}
