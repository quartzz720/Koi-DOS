#include "koi.h"

/* What a program knows about where it is.
 *
 * Three questions that look the same and are not: where the shell was standing
 * when the program started, where the program itself was loaded from, and
 * where a file that ships beside it would therefore be. A package with data
 * files gets this wrong by assuming the first is the second, and the symptom
 * is a program that runs perfectly and cannot find its own contents.
 *
 *     where              what this program can see
 *     where NAME         and where NAME would be, beside it
 */

int main(const char* arguments) {
    char text[256];
    const char* name = arguments ? arguments : "";

    while (*name == ' ') name++;
    if (!*name) name = "DATA.DAT";

    koi_print("Program  ");
    if (koi_systext(KOI_TEXT_PROGRAM_PATH, 0, text, sizeof(text)) > 0)
        koi_print(text);
    else
        koi_print("(the system did not say)");
    koi_print("\n");

    koi_print("Beside   ");
    if (koi_beside(name, text, sizeof(text)) > 0) {
        koi_print(text);
        koi_print(koi_exists(text) == 1 ? "   exists" : "   is not there");
    } else {
        koi_print("(would not fit)");
    }
    koi_print("\n");

    koi_print("Here     ");
    koi_print(name);
    koi_print(koi_exists(name) == 1 ? "   exists" : "   is not there");
    koi_print("\n");
    return 0;
}
