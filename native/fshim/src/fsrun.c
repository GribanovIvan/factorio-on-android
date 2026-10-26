/* fsrun: adb-pushable harness for early bring-up, no APK required.
 *
 *   ./fsrun --version
 *   FSHIM_GAME=/path/libfactorio_game.so ./fsrun --config /path/config.ini
 */
#define _GNU_SOURCE

#include <stdlib.h>

#include "fshim.h"

int main(int argc, char **argv)
{
    const char *game = getenv("FSHIM_GAME");
    if (!game || !*game) {
        game = "/data/local/tmp/factorio/libfactorio_game.so";
    }
    return fshim_run(game, argc, argv);
}
