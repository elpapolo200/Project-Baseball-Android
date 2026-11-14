package org.libsdl.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

/* Mando en pantalla de Project Baseball (segunda versión, 2026-11-10). Todos los controles de la Vita:
 * cruceta, △○✕□, L, R, START, SELECT y las DOS palancas (izquierda y derecha).
 *
 *  - Palancas analógicas: mandan la posición exacta al port (nativoStick -> input.c); si la función
 *    nativa no existe, caen a las teclas W/A/S/D e I/J/K/L. Son «semiflotantes»: si el pulgar cae cerca
 *    pero no encima, la base se coloca bajo el dedo y vuelve a su sitio al soltar.
 *  - Cruceta de una pieza: se puede deslizar el dedo de una dirección a otra (con diagonales).
 *  - Botones de acción: se puede rodar el pulgar de uno a otro sin levantarlo.
 *  - Respuesta: cada control crece y se ilumina al pulsarlo, con un toque de vibración
 *    ([pantalla_tactil] vibracion=0 lo quita).
 *  - Si no se toca nada en unos segundos, el mando se atenúa para dejar ver el juego y vuelve al tocar.
 *
 * Los botones mandan a SDL la tecla que el port ya entiende ([teclas] de ProjectBaseball.ini):
 *   X = Cruz, C = Círculo, Z = Cuadrado, V = Triángulo, Q = L, E = R, Intro = START,
 *   Mayús. derecha = SELECT, flechas = cruceta.
 * Los toques fuera de los controles pasan al juego como pantalla táctil de la Vita. */
public class MandoEnPantalla extends View {
    private static final int IZQ = 0, DER = 1, CENTRO = 2;                 /* de dónde se mide la x */
    private static final int G_TRI = 1, G_CIR = 2, G_CRUZ = 3, G_CUAD = 4;  /* símbolo dibujado */

    private static class Boton {
        String txt; int tecla, color, glifo, ancla; float ax, ay, fr;       /* posición y radio en fracción del alto */
        boolean pildora, pulsado; float anim; int dedo = -1;
        Boton(String t, int k, int ancla, float x, float y, float r, int c, int g, boolean pil) {
            txt = t; tecla = k; this.ancla = ancla; ax = x; ay = y; fr = r; color = c; glifo = g; pildora = pil;
        }
    }
    private static class Palanca {
        int cual, ancla; float ax, ay, fr; int[] teclas; String txt;
        float bx, by; boolean movida;                                       /* base bajo el dedo (px) */
        float dx, dy, anim; int dedo = -1; boolean[] dir = new boolean[4];
        Palanca(int cual, int ancla, float x, float y, float r, int[] k, String t) {
            this.cual = cual; this.ancla = ancla; ax = x; ay = y; fr = r; teclas = k; txt = t;
        }
    }

    private static final int C_TRI = 0xFF4DE3B7, C_CIR = 0xFFFF6B6B, C_CRUZ = 0xFF6FA8FF, C_CUAD = 0xFFF291D8,
                             C_NEUTRO = 0xFFE8EEF7, C_ACENTO = 0xFFFFC94A;
    private final Boton[] b;
    private final Boton ocultar;
    private final Palanca[] pal;
    /* cruceta */
    private final float cax = 0.205f, cay = 0.655f, cfr = 0.135f;
    private int cruDedo = -1; private final boolean[] cru = new boolean[4]; private final float[] cruAnim = new float[4];
    private static final int[] CRU_TECLA = { KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT };

    private boolean visible = true, vibrar = true;
    private float esc = 1f;
    private float presencia = 1f; private long ultimoToque, ultimoDibujo;
    private static final long ESPERA_ATENUAR = 3500;
    private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path camino = new Path();
    private final RectF rect = new RectF();
    private static boolean nativoOk = true;
    private static native void nativoStick(int cual, float x, float y, boolean activo);

    /* Desde la pantalla de opciones: mostrar al empezar, tamaño (0,7-1,4) y opacidad (0,2-1). */
    public void config(boolean mostrar, float escala, float opacidad) { visible = mostrar; esc = escala; setAlpha(opacidad); invalidate(); }
    public void vibracion(boolean si) { vibrar = si; }
    private float recorrido = 1.0f;   /* cuánto hay que mover el pulgar para llegar a tope, en radios de la palanca */
    public void recorrido(int porciento) { recorrido = Math.max(80, Math.min(250, porciento)) / 100f; }

    public MandoEnPantalla(Context c) {
        super(c);
        b = new Boton[] {
            new Boton("", KeyEvent.KEYCODE_V, DER, 0.215f, 0.420f, 0.070f, C_TRI,  G_TRI,  false),
            new Boton("", KeyEvent.KEYCODE_C, DER, 0.095f, 0.300f, 0.070f, C_CIR,  G_CIR,  false),
            new Boton("", KeyEvent.KEYCODE_X, DER, 0.215f, 0.180f, 0.070f, C_CRUZ, G_CRUZ, false),
            new Boton("", KeyEvent.KEYCODE_Z, DER, 0.335f, 0.300f, 0.070f, C_CUAD, G_CUAD, false),
            new Boton("L", KeyEvent.KEYCODE_Q, IZQ, 0.135f, 0.925f, 0.058f, C_NEUTRO, 0, true),
            new Boton("R", KeyEvent.KEYCODE_E, DER, 0.135f, 0.925f, 0.058f, C_NEUTRO, 0, true),
            /* en las esquinas de abajo, sobre las franjas negras: en el centro tapaban las pestañas del menú del juego */
            new Boton("SELECT", KeyEvent.KEYCODE_SHIFT_RIGHT, IZQ, 0.115f, 0.052f, 0.036f, C_NEUTRO, 0, true),
            new Boton("START",  KeyEvent.KEYCODE_ENTER,       DER, 0.115f, 0.052f, 0.036f, C_NEUTRO, 0, true),
        };
        ocultar = new Boton("", 0, CENTRO, 0f, 0.945f, 0.034f, C_NEUTRO, 0, false);
        pal = new Palanca[] {
            new Palanca(0, IZQ, 0.215f, 0.255f, 0.125f, new int[] { KeyEvent.KEYCODE_W, KeyEvent.KEYCODE_S, KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_D }, "L"),
            new Palanca(1, DER, 0.300f, 0.700f, 0.105f, new int[] { KeyEvent.KEYCODE_I, KeyEvent.KEYCODE_K, KeyEvent.KEYCODE_J, KeyEvent.KEYCODE_L }, "R"),
        };
        p.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        p.setStrokeCap(Paint.Cap.ROUND); p.setStrokeJoin(Paint.Join.ROUND);
        ultimoToque = System.currentTimeMillis();
    }
    @Override public boolean hasOverlappingRendering() { return false; }   /* la opacidad se aplica sin búfer intermedio */

    /* ---- geometría ---- */
    private float X(int ancla, float ax) { float h = getHeight(), w = getWidth(); return ancla == IZQ ? ax * h : ancla == DER ? w - ax * h : w * 0.5f + ax * h; }
    private float Y(float ay) { return getHeight() * (1f - ay); }
    private float R(float fr) { return fr * getHeight() * esc; }
    private float casaX(Palanca s) { return X(s.ancla, s.ax); }
    private float casaY(Palanca s) { return Y(s.ay); }

    private static int tinte(int color, int alfa) { return (color & 0x00FFFFFF) | (Math.max(0, Math.min(255, alfa)) << 24); }
    private int at(int alfa) { return Math.round(alfa * presencia); }

    /* ---- dibujo ---- */
    @Override
    protected void onDraw(Canvas c) {
        long ahora = System.nanoTime();
        float dt = ultimoDibujo == 0 ? 0.033f : Math.min(0.1f, (ahora - ultimoDibujo) / 1e9f);
        ultimoDibujo = ahora;
        float paso = Math.min(1f, dt * 16f);
        boolean moviendo = false;

        boolean tocando = cruDedo >= 0;
        for (Boton o : b) if (o.pulsado) tocando = true;
        for (Palanca s : pal) if (s.dedo >= 0) tocando = true;
        if (tocando) ultimoToque = System.currentTimeMillis();
        float meta = (tocando || System.currentTimeMillis() - ultimoToque < ESPERA_ATENUAR) ? 1f : 0.42f;
        if (!visible) meta = 0.35f;
        if (Math.abs(meta - presencia) > 0.01f) { presencia += (meta - presencia) * Math.min(1f, dt * (meta > presencia ? 14f : 3f)); moviendo = true; } else presencia = meta;

        moviendo |= anima(ocultar, paso);
        dibujaOcultar(c);
        if (visible) {
            for (Boton o : b) { moviendo |= anima(o, paso); if (o.pildora) dibujaPildora(c, o); else dibujaAccion(c, o); }
            for (int i = 0; i < 4; i++) { float m = cru[i] ? 1f : 0f; if (Math.abs(m - cruAnim[i]) > 0.01f) { cruAnim[i] += (m - cruAnim[i]) * paso; moviendo = true; } else cruAnim[i] = m; }
            dibujaCruceta(c);
            for (Palanca s : pal) {
                float m = s.dedo >= 0 ? 1f : 0f;
                if (Math.abs(m - s.anim) > 0.01f) { s.anim += (m - s.anim) * paso; moviendo = true; } else s.anim = m;
                if (s.dedo < 0 && s.movida) {   /* la base vuelve a su sitio */
                    float hx = casaX(s), hy = casaY(s);
                    s.bx += (hx - s.bx) * paso; s.by += (hy - s.by) * paso;
                    if (Math.abs(hx - s.bx) + Math.abs(hy - s.by) < 1f) s.movida = false; else moviendo = true;
                }
                dibujaPalanca(c, s);
            }
        }
        if (moviendo) postInvalidateOnAnimation();
        else if (presencia > 0.5f && visible) postInvalidateDelayed(ESPERA_ATENUAR + 50);   /* para empezar a atenuar */
    }
    private boolean anima(Boton o, float paso) {
        float m = o.pulsado ? 1f : 0f;
        if (Math.abs(m - o.anim) > 0.01f) { o.anim += (m - o.anim) * paso; return true; }
        o.anim = m; return false;
    }

    private void cristal(Canvas c, float x, float y, float r, int color, float a) {
        if (a > 0.02f) { p.setStyle(Paint.Style.FILL); p.setColor(tinte(color, at((int) (46 * a)))); c.drawCircle(x, y, r * (1.22f + 0.10f * a), p); }   /* halo */
        p.setStyle(Paint.Style.FILL); p.setColor(Color.argb(at(96), 14, 18, 26)); c.drawCircle(x, y, r, p);
        if (a > 0.02f) { p.setColor(tinte(color, at((int) (150 * a)))); c.drawCircle(x, y, r, p); }
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(Math.max(2f, r * 0.045f));
        p.setColor(tinte(a > 0.5f ? 0xFFFFFFFF : color, at((int) (120 + 110 * a)))); c.drawCircle(x, y, r, p);
    }
    private void dibujaAccion(Canvas c, Boton o) {
        float x = X(o.ancla, o.ax), y = Y(o.ay), r = R(o.fr) * (1f + 0.13f * o.anim);
        cristal(c, x, y, r, o.color, o.anim);
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(r * 0.115f);
        p.setColor(tinte(o.anim > 0.5f ? 0xFFFFFFFF : o.color, at(235)));
        float g = r * 0.40f;
        switch (o.glifo) {
            case G_TRI:
                camino.rewind(); camino.moveTo(x, y - g * 1.08f); camino.lineTo(x + g * 1.05f, y + g * 0.74f); camino.lineTo(x - g * 1.05f, y + g * 0.74f); camino.close();
                c.drawPath(camino, p); break;
            case G_CIR: c.drawCircle(x, y, g, p); break;
            case G_CRUZ: c.drawLine(x - g * 0.85f, y - g * 0.85f, x + g * 0.85f, y + g * 0.85f, p); c.drawLine(x - g * 0.85f, y + g * 0.85f, x + g * 0.85f, y - g * 0.85f, p); break;
            case G_CUAD: rect.set(x - g * 0.88f, y - g * 0.88f, x + g * 0.88f, y + g * 0.88f); c.drawRoundRect(rect, g * 0.12f, g * 0.12f, p); break;
        }
    }
    private void dibujaPildora(Canvas c, Boton o) {
        float x = X(o.ancla, o.ax), y = Y(o.ay), r = R(o.fr) * (1f + 0.10f * o.anim);
        float mw = r * (o.txt.length() > 1 ? 2.25f : 1.75f);
        rect.set(x - mw, y - r, x + mw, y + r);
        p.setStyle(Paint.Style.FILL); p.setColor(Color.argb(at(96), 14, 18, 26)); c.drawRoundRect(rect, r, r, p);
        if (o.anim > 0.02f) { p.setColor(tinte(C_ACENTO, at((int) (170 * o.anim)))); c.drawRoundRect(rect, r, r, p); }
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(Math.max(2f, r * 0.07f));
        p.setColor(tinte(o.color, at((int) (110 + 120 * o.anim)))); c.drawRoundRect(rect, r, r, p);
        p.setStyle(Paint.Style.FILL); p.setTextAlign(Paint.Align.CENTER);
        p.setColor(tinte(o.anim > 0.5f ? 0xFF10141C : o.color, at(235)));
        p.setTextSize(o.txt.length() > 1 ? r * 0.82f : r * 1.15f); p.setLetterSpacing(o.txt.length() > 1 ? 0.12f : 0f);
        c.drawText(o.txt, x, y + p.getTextSize() * 0.35f, p);
        p.setLetterSpacing(0f);
    }
    private void dibujaOcultar(Canvas c) {   /* interruptor para esconder el mando: tres rayitas / un punto */
        Boton o = ocultar;
        float x = X(o.ancla, o.ax), y = Y(o.ay), r = R(o.fr) * (1f + 0.12f * o.anim);
        p.setStyle(Paint.Style.FILL); p.setColor(Color.argb(at(80), 14, 18, 26)); c.drawCircle(x, y, r, p);
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(Math.max(2f, r * 0.10f)); p.setColor(tinte(C_NEUTRO, at(170)));
        if (visible) { for (int i = -1; i <= 1; i++) c.drawLine(x - r * 0.42f, y + i * r * 0.30f, x + r * 0.42f, y + i * r * 0.30f, p); }
        else { c.drawCircle(x, y, r * 0.42f, p); p.setStyle(Paint.Style.FILL); c.drawCircle(x, y, r * 0.14f, p); }
    }
    private void dibujaCruceta(Canvas c) {
        float x = X(IZQ, cax), y = Y(cay), r = R(cfr), br = r * 0.36f, red = br * 0.42f;
        camino.rewind();
        rect.set(x - br, y - r, x + br, y + r); camino.addRoundRect(rect, red, red, Path.Direction.CW);
        rect.set(x - r, y - br, x + r, y + br); camino.addRoundRect(rect, red, red, Path.Direction.CW);
        p.setStyle(Paint.Style.FILL); p.setColor(Color.argb(at(96), 14, 18, 26)); c.drawPath(camino, p);
        float[][] d = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
        for (int i = 0; i < 4; i++) {   /* brazo pulsado iluminado */
            float a = cruAnim[i]; if (a < 0.02f) continue;
            float ex = x + d[i][0] * (r + br) * 0.5f, ey = y + d[i][1] * (r + br) * 0.5f, lx = d[i][0] != 0 ? (r - br) * 0.5f : br, ly = d[i][1] != 0 ? (r - br) * 0.5f : br;
            rect.set(ex - lx, ey - ly, ex + lx, ey + ly);
            p.setStyle(Paint.Style.FILL); p.setColor(tinte(C_ACENTO, at((int) (185 * a)))); c.drawRoundRect(rect, red, red, p);
        }
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(Math.max(2f, r * 0.028f)); p.setColor(tinte(C_NEUTRO, at(125))); c.drawPath(camino, p);
        p.setStyle(Paint.Style.FILL);
        for (int i = 0; i < 4; i++) {   /* flechitas */
            float ex = x + d[i][0] * r * 0.70f, ey = y + d[i][1] * r * 0.70f, t = br * 0.42f, nx = -d[i][1], ny = d[i][0];
            camino.rewind();
            camino.moveTo(ex + d[i][0] * t, ey + d[i][1] * t);
            camino.lineTo(ex - d[i][0] * t * 0.6f + nx * t, ey - d[i][1] * t * 0.6f + ny * t);
            camino.lineTo(ex - d[i][0] * t * 0.6f - nx * t, ey - d[i][1] * t * 0.6f - ny * t);
            camino.close();
            p.setColor(tinte(cruAnim[i] > 0.5f ? 0xFF10141C : C_NEUTRO, at(215))); c.drawPath(camino, p);
        }
    }
    private void dibujaPalanca(Canvas c, Palanca s) {
        float hx = s.movida ? s.bx : casaX(s), hy = s.movida ? s.by : casaY(s), r = R(s.fr), a = s.anim;
        p.setStyle(Paint.Style.FILL); p.setColor(Color.argb(at((int) (58 + 30 * a)), 14, 18, 26)); c.drawCircle(hx, hy, r, p);
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(Math.max(2f, r * 0.03f));
        p.setColor(tinte(C_NEUTRO, at((int) (85 + 90 * a)))); c.drawCircle(hx, hy, r, p);
        if (a > 0.05f) {   /* arco que señala hacia dónde se empuja */
            float l = (float) Math.sqrt(s.dx * s.dx + s.dy * s.dy);
            if (l > 0.15f) {
                float ang = (float) Math.toDegrees(Math.atan2(s.dy, s.dx));
                rect.set(hx - r, hy - r, hx + r, hy + r);
                p.setStrokeWidth(r * 0.075f); p.setColor(tinte(C_ACENTO, at((int) (230 * a * Math.min(1f, l)))));
                c.drawArc(rect, ang - 24f, 48f, false, p);
            }
        }
        float kx = hx + s.dx * r * 0.62f, ky = hy + s.dy * r * 0.62f, kr = r * (0.46f + 0.05f * a);
        p.setStyle(Paint.Style.FILL);
        p.setColor(Color.argb(at((int) (150 + 60 * a)), 34, 40, 54)); c.drawCircle(kx, ky, kr, p);
        p.setColor(tinte(C_NEUTRO, at((int) (38 + 50 * a)))); c.drawCircle(kx, ky - kr * 0.10f, kr * 0.72f, p);
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(Math.max(2f, r * 0.035f));
        p.setColor(tinte(a > 0.5f ? C_ACENTO : C_NEUTRO, at((int) (150 + 90 * a)))); c.drawCircle(kx, ky, kr, p);
        if (a < 0.5f) {
            p.setStyle(Paint.Style.FILL); p.setTextAlign(Paint.Align.CENTER); p.setTextSize(kr * 0.80f);
            p.setColor(tinte(C_NEUTRO, at((int) (170 * (1f - 2f * a))))); c.drawText(s.txt, kx, ky + p.getTextSize() * 0.35f, p);
        }
    }

    /* ---- entrada ---- */
    private void tecla(int k, boolean abajo) { if (k == 0) return; if (abajo) SDLActivity.onNativeKeyDown(k); else SDLActivity.onNativeKeyUp(k); }
    private void toque() { if (vibrar) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP, HapticFeedbackConstants.FLAG_IGNORE_VIEW_SETTING); }

    private Boton botonEn(float x, float y) {
        if (!visible) return null;
        Boton mejor = null; float md = 0;
        for (Boton o : b) {
            float dx = x - X(o.ancla, o.ax), dy = y - Y(o.ay), r = R(o.fr);
            if (o.pildora) { float mw = r * (o.txt.length() > 1 ? 2.25f : 1.75f) + r * 0.5f, mh = r * 1.6f; if (Math.abs(dx) > mw || Math.abs(dy) > mh) continue; }
            else if (dx * dx + dy * dy > r * r * 1.32f * 1.32f) continue;
            float dd = dx * dx + dy * dy;
            if (mejor == null || dd < md) { mejor = o; md = dd; }
        }
        return mejor;
    }
    private boolean enOcultar(float x, float y) { float dx = x - X(ocultar.ancla, ocultar.ax), dy = y - Y(ocultar.ay), r = R(ocultar.fr) * 1.6f; return dx * dx + dy * dy <= r * r; }
    private boolean enCruceta(float x, float y) { if (!visible) return false; float dx = x - X(IZQ, cax), dy = y - Y(cay), r = R(cfr) * 1.18f; return dx * dx + dy * dy <= r * r; }
    private Palanca palancaEn(float x, float y) {
        if (!visible) return null;
        for (Palanca s : pal) { if (s.dedo >= 0) continue; float dx = x - casaX(s), dy = y - casaY(s), r = R(s.fr) * 1.55f; if (dx * dx + dy * dy <= r * r) return s; }
        return null;
    }

    private void pulsa(Boton o, int id) { o.dedo = id; if (!o.pulsado) { o.pulsado = true; tecla(o.tecla, true); toque(); } }
    private void suelta(Boton o) { o.dedo = -1; if (o.pulsado) { o.pulsado = false; tecla(o.tecla, false); } }

    private void mueveCruceta(float x, float y) {
        float dx = x - X(IZQ, cax), dy = y - Y(cay), r = R(cfr), l = (float) Math.sqrt(dx * dx + dy * dy);
        boolean[] n = new boolean[4];
        if (l > r * 0.20f) {   /* 8 direcciones: cada eje se activa si pesa lo bastante */
            float ux = dx / l, uy = dy / l;
            n[0] = uy < -0.38f; n[1] = uy > 0.38f; n[2] = ux < -0.38f; n[3] = ux > 0.38f;
        }
        boolean nuevo = false;
        for (int i = 0; i < 4; i++) if (n[i] != cru[i]) { tecla(CRU_TECLA[i], n[i]); if (n[i]) nuevo = true; cru[i] = n[i]; }
        if (nuevo) toque();
    }
    private void sueltaCruceta() { for (int i = 0; i < 4; i++) if (cru[i]) { tecla(CRU_TECLA[i], false); cru[i] = false; } cruDedo = -1; }

    private void empiezaPalanca(Palanca s, int id, float x, float y) {
        float hx = casaX(s), hy = casaY(s), r = R(s.fr), dx = x - hx, dy = y - hy;
        s.dedo = id;
        if (dx * dx + dy * dy > r * r * 0.55f * 0.55f) {   /* el dedo cayó fuera del centro: la base va bajo el dedo */
            float w = getWidth(), h = getHeight();
            s.bx = Math.max(r, Math.min(w - r, x)); s.by = Math.max(r, Math.min(h - r, y)); s.movida = true;
        } else { s.bx = hx; s.by = hy; s.movida = false; }
        toque();
        muevePalanca(s, x, y);
    }
    private void muevePalanca(Palanca s, float x, float y) {
        /* Recorrido largo y curva suave: antes la palanca llegaba a tope con solo ~7 mm de pulgar y sin
         * curva, y apuntar (el lanzamiento) se sentía de todo o nada. Ahora hace falta ~1,5 veces el radio
         * para llegar a tope ([pantalla_tactil] recorrido, en %) y cerca del centro responde más fino. */
        float hx = s.movida ? s.bx : casaX(s), hy = s.movida ? s.by : casaY(s), r = R(s.fr) * recorrido;
        float dx = (x - hx) / r, dy = (y - hy) / r, l = (float) Math.sqrt(dx * dx + dy * dy);
        if (l > 1f) { dx /= l; dy /= l; l = 1f; }
        s.dx = dx; s.dy = dy;
        float zona = 0.07f, k = 0f;
        if (l > zona) { float t = (l - zona) / (1f - zona); k = (float) Math.pow(t, 1.2) / l;   /* 2026-11-13: 1,6 hacía lento el movimiento */ }
        enviaPalanca(s, dx * k, dy * k, true);
    }
    private void sueltaPalanca(Palanca s) { s.dedo = -1; s.dx = s.dy = 0; enviaPalanca(s, 0, 0, false); }
    private void enviaPalanca(Palanca s, float x, float y, boolean activo) {
        if (nativoOk) {
            try { nativoStick(s.cual, x, y, activo); return; }
            catch (UnsatisfiedLinkError e) { nativoOk = false; android.util.Log.w("ProjectBaseball", "palancas: sin función nativa, se usan teclas: " + e); }
        }
        boolean[] n = { y < -0.4f, y > 0.4f, x < -0.4f, x > 0.4f };   /* sin la función nativa: teclas */
        for (int i = 0; i < 4; i++) if (n[i] != s.dir[i]) { tecla(s.teclas[i], n[i]); s.dir[i] = n[i]; }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int a = e.getActionMasked(), i = e.getActionIndex(), id = e.getPointerId(i);
        boolean usado = false;
        switch (a) {
            case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN: {
                float x = e.getX(i), y = e.getY(i);
                if (enOcultar(x, y)) {
                    visible = !visible; toque();
                    if (!visible) sueltaTodo();
                    ultimoToque = System.currentTimeMillis(); invalidate(); return true;
                }
                Boton o = botonEn(x, y);
                Palanca s;
                if (o != null) { pulsa(o, id); usado = true; }
                else if (cruDedo < 0 && enCruceta(x, y)) { cruDedo = id; mueveCruceta(x, y); usado = true; }
                else if ((s = palancaEn(x, y)) != null) { empiezaPalanca(s, id, x, y); usado = true; }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int k = 0; k < e.getPointerCount(); k++) {
                    int pid = e.getPointerId(k); float x = e.getX(k), y = e.getY(k);
                    if (pid == cruDedo) { mueveCruceta(x, y); usado = true; }
                    for (Palanca s : pal) if (s.dedo == pid) { muevePalanca(s, x, y); usado = true; }
                    for (Boton o : b) if (o.dedo == pid) {   /* rodar el pulgar de un botón a otro */
                        usado = true;
                        Boton n = botonEn(x, y);
                        if (n != null && n != o && n.dedo < 0 && !n.pildora && !o.pildora) { suelta(o); pulsa(n, pid); }
                        break;
                    }
                }
                break;
            case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: case MotionEvent.ACTION_CANCEL:
                if (a != MotionEvent.ACTION_POINTER_UP) { usado = algoActivo(); sueltaTodo(); break; }
                if (id == cruDedo) { sueltaCruceta(); usado = true; }
                for (Palanca s : pal) if (s.dedo == id) { sueltaPalanca(s); usado = true; }
                for (Boton o : b) if (o.dedo == id) { suelta(o); usado = true; }
                break;
        }
        if (usado) { ultimoToque = System.currentTimeMillis(); invalidate(); }
        return usado;   /* lo que no es de un control pasa al juego (pantalla táctil de la Vita) */
    }
    private boolean algoActivo() {
        if (cruDedo >= 0) return true;
        for (Palanca s : pal) if (s.dedo >= 0) return true;
        for (Boton o : b) if (o.pulsado) return true;
        return false;
    }
    private void sueltaTodo() {
        if (cruDedo >= 0) sueltaCruceta();
        for (Palanca s : pal) if (s.dedo >= 0) sueltaPalanca(s);
        for (Boton o : b) suelta(o);
    }
}
