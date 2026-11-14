/* Se incluye antes de gxm.c (-include): declaraciones de funciones que gxm.c usa antes de definirlas.
 * MSVC lo tolera (declaración implícita); el clang del NDK no. Así la copia del port no se toca. */
#include <stdint.h>
static int scene_dump(void);
static uint32_t pc_data(uint32_t st);
