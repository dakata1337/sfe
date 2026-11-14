#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <tomlc17.h>

int main(void)
{
    toml_result_t result = toml_parse_file_ex("config.toml");
    TOML_ASSERT(result, "config read");

    return 0;
}
