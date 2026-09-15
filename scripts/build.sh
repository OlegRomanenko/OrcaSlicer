#!/usr/bin/env bash
set -euo pipefail

# Контейнерная сборка форка через штатный `build_linux.sh`: зависимости, приложение и AppImage.
# Аргументы передаются штатному скрипту; параллелизм задаёт `CMAKE_BUILD_PARALLEL_LEVEL` или `nproc`.
# Нужны Docker/Podman и стандартные утилиты shell. Полный лог остаётся во временном каталоге;
# в терминал поступают ошибки и периодические строки прогресса. Код ошибки конвейера сохраняется.
# Пакеты устанавливаются только в контейнере; переданные `-c`/`-F` разрешают штатную очистку сборки/образа.

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd -- "$repo_root"

jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}
log_dir=$(mktemp -d "${TMPDIR:-/tmp}/orcaslicer-build.XXXXXX")
log_file="$log_dir/build.log"

printf 'Лог сборки: %s\n' "$log_file"

# `tee` сохраняет весь вывод; фильтр ограничивает только отображение, а не содержимое лога.
# `pipefail` не позволяет успешному завершению фильтра скрыть ошибку сборки или записи лога.
if ./build_linux.sh -g -dsi -j "$jobs" "$@" 2>&1 |
    tee "$log_file" |
    awk '
        NR == 1 || /error:|FAILED:|CMake Error|^ERROR:|^Build completed|^AppImage dependency audit/ ||
        systime() - last >= 15 {
            print
            fflush()
            last = systime()
        }
    '
then
    printf 'Сборка завершена. Лог: %s\n' "$log_file"
else
    result=$?
    printf 'Сборка завершилась с ошибкой (код %s). Лог: %s\n' "$result" "$log_file" >&2
    exit "$result"
fi
