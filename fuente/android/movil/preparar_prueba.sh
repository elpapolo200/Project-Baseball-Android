#!/system/bin/sh
# Prueba por adb de la preparación de libjuego.so en el teléfono (los mismos pasos que PreparacionActivity).
#   sh preparar_prueba.sh [hilos=6]
cd /data/local/tmp/pb || exit 1
H=${1:-6}
rm -rf partes obj juego.c libjuego.so; mkdir -p partes obj
t0=$(date +%s)
echo "== armrecomp"
./armrecomp emit /sdcard/ProjectBaseball/eboot.elf juego.c 999999 db 2>&1 | grep -E 'overlaps|functions|trapped' | head -5
t1=$(date +%s); echo "armrecomp $((t1-t0)) s, C $(($(stat -c %s juego.c) / 1048576)) MB"
echo "== partir"
./partir juego.c partes 12 || exit 1
rm -f juego.c
t2=$(date +%s); echo "partir $((t2-t1)) s"
echo "== clang x$H"
F="--target=aarch64-linux-android29 -march=armv8.2-a -O2 -fPIC -ffreestanding -nostdinc -fno-stack-protector -fno-math-errno -ffp-contract=off -fasynchronous-unwind-tables -w -DVITA_MEM_DIRECTA -isystem include/clang -I include/shim -I include"
i=0
for s in partes/*.c; do
    o=obj/$(basename "$s" .c).o
    ( ./clang $F -c "$s" -o "$o" 2> "$o.log" || echo "FALLO $s" ) &
    i=$((i + 1))
    if [ $((i % H)) -eq 0 ]; then wait; echo "  $i trozos, $(( $(date +%s) - t2 )) s"; fi
done
wait
./clang $F -fno-builtin -c juego_base_android.c -o obj/juego_base_android.o || echo "FALLO base"
t3=$(date +%s); echo "clang $((t3-t2)) s"
echo "== lld"
./lld -flavor gnu -shared -o libjuego.so -soname libjuego.so --version-script libjuego.map -z noexecstack -z max-page-size=16384 --hash-style=gnu --as-needed -O1 obj/*.o libmain.so || echo "FALLO lld"
t4=$(date +%s); echo "lld $((t4-t3)) s"
ls -la libjuego.so
echo "TOTAL $((t4-t0)) s"
echo HECHO
