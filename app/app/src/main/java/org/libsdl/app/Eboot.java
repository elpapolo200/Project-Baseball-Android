package org.libsdl.app;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.Inflater;

/* eboot.bin DESCIFRADO (SELF en claro: Vita3K «Decrypt All SELF» / decrypted_selfs, NoNpDrm descifrado...) ->
 * eboot.elf, como self_to_elf del launcher de PC. Así nadie necesita un PC para sacar el eboot.elf.
 * Formato (little-endian): "SCE\0", +0x18 u64 tamaño del ELF; cabecera extendida desde 0x20 con u64:
 * [4] cabecera ELF, [5] phdr, [7] tabla de segmentos (por phdr: desplazamiento, longitud, compresión 1/2,
 * cifrado 1 = sí, 2 = no). */
final class Eboot {
    static final int SI = 0, CIFRADO = 1, NO_ES = 2;

    private static long u64(byte[] d, long o) { long v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | (d[(int) o + i] & 0xFF); return v; }
    private static int u32(byte[] d, long o) { return (d[(int) o] & 0xFF) | (d[(int) o + 1] & 0xFF) << 8 | (d[(int) o + 2] & 0xFF) << 16 | (d[(int) o + 3] & 0xFF) << 24; }
    private static int u16(byte[] d, long o) { return (d[(int) o] & 0xFF) | (d[(int) o + 1] & 0xFF) << 8; }

    static byte[] leer(InputStream in) throws Exception {
        ByteArrayOutputStream o = new ByteArrayOutputStream(20 << 20);
        byte[] b = new byte[1 << 16]; int n;
        while ((n = in.read(b)) > 0) o.write(b, 0, n);
        return o.toByteArray();
    }

    static boolean esElf(byte[] d) { return d.length > 0x10000 && d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F'; }
    static boolean esSelf(byte[] d) { return d.length > 0x80 && d[0] == 'S' && d[1] == 'C' && d[2] == 'E' && d[3] == 0; }

    /* ¿Un archivo es un eboot.bin utilizable? SI (SELF en claro o ELF), CIFRADO, NO_ES */
    static int estado(File f) {
        if (!f.isFile()) return NO_ES;
        try (InputStream in = new FileInputStream(f)) {
            byte[] h = new byte[0x1000]; int n = in.read(h);
            if (n >= 4 && h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') return SI;
            if (n < 0x80 || h[0] != 'S' || h[1] != 'C' || h[2] != 'E' || h[3] != 0) return NO_ES;
            long eh = u64(h, 0x20 + 8 * 4), si = u64(h, 0x20 + 8 * 7);
            if (eh + 0x30 > n) return NO_ES;
            int phnum = u16(h, eh + 0x2C);
            if (phnum == 0 || si + 32L * phnum > n) return NO_ES;
            for (int i = 0; i < phnum; i++) if (u64(h, si + 32L * i + 24) != 2) return CIFRADO;
            return SI;
        } catch (Exception e) { return NO_ES; }
    }

    /* Convierte (o copia, si ya es ELF) a `salida`. Devuelve null si va bien, o el motivo del fallo. */
    static String aElf(byte[] d, File salida, boolean en) {
        try {
            byte[] e;
            if (esElf(d)) e = d;
            else {
                if (!esSelf(d)) return en ? "This file is not an eboot.bin (SELF)." : "Ese archivo no es un eboot.bin (SELF).";
                long elfSize = u64(d, 0x18);
                long eh = u64(d, 0x20 + 8 * 4), ph = u64(d, 0x20 + 8 * 5), si = u64(d, 0x20 + 8 * 7);
                if (elfSize <= 0 || elfSize > (256L << 20) || eh + 52 > d.length || d[(int) eh] != 0x7F || d[(int) eh + 1] != 'E')
                    return en ? "Unrecognized SELF header." : "Cabecera del SELF no reconocida.";
                int phoff = u32(d, eh + 0x1C), phnum = u16(d, eh + 0x2C);
                if (ph + 32L * phnum > d.length || si + 32L * phnum > d.length || phoff + 32L * phnum > elfSize)
                    return en ? "Segment table out of range." : "Tabla de segmentos fuera del archivo.";
                e = new byte[(int) elfSize];
                System.arraycopy(d, (int) eh, e, 0, 52);
                System.arraycopy(d, (int) ph, e, phoff, 32 * phnum);
                for (int i = 0; i < phnum; i++) {
                    long p = ph + 32L * i, s = si + 32L * i;
                    int pOff = u32(d, p + 4), pFilesz = u32(d, p + 16);
                    long off = u64(d, s), len = u64(d, s + 8), comp = u64(d, s + 16), enc = u64(d, s + 24);
                    if (pFilesz == 0) continue;
                    if (enc != 2)
                        return en ? "This eboot.bin is still ENCRYPTED. Use the decrypted one from Vita3K (\"Decrypt All SELF\" / decrypted_selfs)."
                                  : "Este eboot.bin sigue CIFRADO. Usa el descifrado de Vita3K («Decrypt All SELF» / decrypted_selfs).";
                    if (off + len > d.length || (long) pOff + pFilesz > elfSize)
                        return (en ? "Segment out of range: " : "Segmento fuera de rango: ") + i;
                    if (comp == 2) {
                        Inflater inf = new Inflater();
                        inf.setInput(d, (int) off, (int) len);
                        int got = inf.inflate(e, pOff, pFilesz);
                        inf.end();
                        if (got != pFilesz) return (en ? "Could not decompress segment " : "No se pudo descomprimir el segmento ") + i;
                    } else System.arraycopy(d, (int) off, e, pOff, (int) Math.min(len, pFilesz));
                }
            }
            File tmp = new File(salida.getPath() + ".tmp");
            try (OutputStream o = new FileOutputStream(tmp)) { o.write(e); }
            salida.delete();
            if (!tmp.renameTo(salida)) return (en ? "Could not write " : "No pude escribir ") + salida;
            return null;
        } catch (Exception ex) { return ex.toString(); }
    }

    /* eboot.bin en claro en las carpetas de siempre (ProjectBaseball/ o PCSA00511/): el primero utilizable. */
    static File buscar() {
        File[] c = { new File(Ini.CARPETA + "/eboot.bin"), new File(Ini.CARPETA + "/PCSA00511/eboot.bin") };
        for (File f : c) if (estado(f) == SI) return f;
        return null;
    }
    static File cifrado() {
        File[] c = { new File(Ini.CARPETA + "/eboot.bin"), new File(Ini.CARPETA + "/PCSA00511/eboot.bin") };
        for (File f : c) if (estado(f) == CIFRADO) return f;
        return null;
    }
}
