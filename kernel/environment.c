#include "environment.h"
#include "string.h"

typedef struct {
    char name[ENVIRONMENT_NAME_MAX];
    char value[ENVIRONMENT_VALUE_MAX];
    int used;
} VARIABLE;

/* Statically allocated rather than on the heap. The environment is read from
   the middle of command execution and written from batch files; a fixed table
   cannot fail halfway or leak, and twenty-four of them is more than a DOS-like
   system has ever needed. */
static VARIABLE variables[ENVIRONMENT_MAX];

static int same_name(const char* left, const char* right) {
    while (*left && *right) {
        char a = *left, b = *right;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
        left++;
        right++;
    }
    return !*left && !*right;
}

static VARIABLE* find(const char* name) {
    for (int index = 0; index < ENVIRONMENT_MAX; index++)
        if (variables[index].used && same_name(variables[index].name, name))
            return &variables[index];
    return (VARIABLE*)0;
}

const char* environment_get(const char* name) {
    VARIABLE* found;

    if (!name || !name[0]) return (const char*)0;
    found = find(name);
    return found ? found->value : (const char*)0;
}

int environment_set(const char* name, const char* value) {
    VARIABLE* slot;
    boot_uint64_t length;

    if (!name || !name[0]) return 0;
    if (strlen(name) + 1 > ENVIRONMENT_NAME_MAX) return 0;

    slot = find(name);

    /* Removing. `SET NAME=` in DOS deletes the variable, and a variable set to
       nothing is a distinction this shell has no use for. */
    if (!value || !value[0]) {
        if (slot) slot->used = 0;
        return 1;
    }

    length = strlen(value);
    if (length + 1 > ENVIRONMENT_VALUE_MAX) return 0;

    if (!slot) {
        for (int index = 0; index < ENVIRONMENT_MAX; index++) {
            if (variables[index].used) continue;
            slot = &variables[index];
            break;
        }
        if (!slot) return 0;
        for (boot_uint64_t index = 0; name[index]; index++)
            slot->name[index] = name[index];
        slot->name[strlen(name)] = 0;
        slot->used = 1;
    }

    for (boot_uint64_t index = 0; index <= length; index++)
        slot->value[index] = value[index];
    return 1;
}

int environment_at(int index, const char** name, const char** value) {
    int seen = 0;

    for (int at = 0; at < ENVIRONMENT_MAX; at++) {
        if (!variables[at].used) continue;
        if (seen++ != index) continue;
        if (name) *name = variables[at].name;
        if (value) *value = variables[at].value;
        return 1;
    }
    return 0;
}

void environment_expand(const char* input, char* output, boot_uint64_t size) {
    boot_uint64_t out = 0;

    if (!size) return;
    if (!input) { output[0] = 0; return; }

    while (*input && out + 1 < size) {
        char name[ENVIRONMENT_NAME_MAX];
        boot_uint64_t length = 0;
        const char* closing;
        const char* value;

        if (*input != '%') { output[out++] = *input++; continue; }

        /* `%%` is one percent sign, so a value can contain one at all. */
        if (input[1] == '%') { output[out++] = '%'; input += 2; continue; }

        /* A percent with no partner is a percent. File names may contain one,
           and a shell that swallows it cannot open the file. */
        closing = input + 1;
        while (*closing && *closing != '%' && *closing != ' ') closing++;
        if (*closing != '%') { output[out++] = *input++; continue; }

        while (input + 1 + length < closing && length + 1 < sizeof(name)) {
            name[length] = input[1 + length];
            length++;
        }
        name[length] = 0;
        input = closing + 1;

        /* An unknown name expands to nothing, as it did in DOS. Silent, and
           deliberately: a batch file that tests whether a variable is set does
           it by comparing the expansion with nothing. */
        value = environment_get(name);
        if (!value) continue;
        while (*value && out + 1 < size) output[out++] = *value++;
    }
    output[out] = 0;
}
