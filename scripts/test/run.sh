#!/usr/bin/env bash
set -euo pipefail

# Публичная обёртка контейнерных тестов; образ подготавливает `scripts/build.sh -t`.
# Первый аргумент выбирает набор (по умолчанию fff_print); остальные передаются Catch2.
# Набор all использует штатный run_unit_tests.sh. Набор version проверяет метаданные
# сборки без компиляции приложения; оба набора не принимают аргументы Catch2.
# Для version флаг --appimage дополнительно запускает готовый AppImage с --help,
# без GUI и с изолированными пользовательскими каталогами внутри контейнера.
# Набор gui передаёт управление gui.sh: проверяет главное окно готового AppImage
# на отдельном Xvfb-дисплее локально либо на явно указанном SSH-хосте, без контейнера.
# Флаг gui --presets дополнительно выполняет сценарий настоящих диалогов сохранения через AT-SPI.
#
# Для контейнерных наборов нужны Bash, Docker/Podman и GNU-утилиты;
# кроме version, нужна Release-сборка в build/. Зависимости gui проверяет gui.sh.
# ORCA_CONTAINER_CLI и ORCA_DOCKER_* выбирают образ как в build_linux.sh;
# CMAKE_BUILD_PARALLEL_LEVEL задаёт число задач сборки, ORCA_TEST_TIMEOUT — секунды теста.
# Скрипт пишет результаты сборки/тестов в проект и полные логи в новый временный каталог.
# В контейнерных наборах образы не загружаются, сеть отключена, контейнеры удаляются;
# логи сохраняются для всех наборов.
# Ошибки сборки, тестов, тайм-аута и логирования возвращаются вызывающему процессу.

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
suite=${1:-fff_print}
if (($#)); then shift; fi
suites=(fff_print libslic3r libnest2d slic3rutils sla_print filament_group)
case "$suite" in
    gui) exec bash "$repo_root/scripts/test/gui.sh" "$@" ;;
    fff_print|libslic3r|libnest2d|slic3rutils|sla_print|filament_group) targets=("${suite}_tests") ;;
    version)
        check_appimage=OFF
        if (($# == 1)) && [[ "$1" == --appimage ]]; then check_appimage=ON; shift; fi
        if (($#)); then
            printf 'Набор version принимает только необязательный флаг --appimage.\n' >&2
            exit 2
        fi
        targets=()
        ;;
    all)
        if (($#)); then
            printf 'Набор %s не принимает аргументы Catch2.\n' "$suite" >&2
            exit 2
        fi
        targets=("${suites[@]/%/_tests}")
        ;;
    *) printf 'Неизвестный набор тестов: %s\n' "$suite" >&2; exit 2 ;;
esac

jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}
test_timeout=${ORCA_TEST_TIMEOUT:-600}
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ || ! "$test_timeout" =~ ^[1-9][0-9]*$ ]]; then
    printf 'Параллелизм и тайм-аут должны быть положительными целыми числами.\n' >&2
    exit 2
fi
if [[ "$suite" != version && ! -f "$repo_root/build/CMakeCache.txt" ]]; then
    printf 'Нет настроенной сборки. Сначала запусти scripts/build.sh -t.\n' >&2
    exit 2
fi

container_cli=${ORCA_CONTAINER_CLI:-}
if [[ -z "$container_cli" ]]; then
    if command -v docker >/dev/null 2>&1; then container_cli=docker
    elif command -v podman >/dev/null 2>&1; then container_cli=podman
    else printf 'Нужен Docker или Podman.\n' >&2; exit 2
    fi
fi
command -v "$container_cli" >/dev/null
runner_image=${ORCA_DOCKER_IMAGE:-}
if [[ -z "$runner_image" ]]; then
    # Тег должен совпадать с get_docker_runner_image в build_linux.sh, включая recipe_hash.
    base_image=${ORCA_DOCKER_BASE_IMAGE:-ubuntu:24.04}
    cmake_version=${ORCA_DOCKER_CMAKE_VERSION-4.3.0}
    recipe_hash=$(find "$repo_root/build_linux.sh" "$repo_root/scripts/linux.d" -type f -print0 |
        sort -z | xargs -0 cat | sha256sum | cut -c1-12)
    base_tag=$(echo "$base_image" | tr '/:@' '---' | tr -cs 'A-Za-z0-9_.-' '-')
    cmake_tag=$(echo "${cmake_version:-system}" | tr -cs 'A-Za-z0-9_.-' '-')
    runner_image="orcaslicer-linux-builder:${base_tag}-cmake-${cmake_tag}-${recipe_hash}"
fi
if ! "$container_cli" image inspect "$runner_image" >/dev/null 2>&1; then
    printf 'Образ %s недоступен. Сначала запусти scripts/build.sh -t.\n' "$runner_image" >&2
    exit 2
fi

log_dir=$(mktemp -d "${TMPDIR:-/tmp}/orcaslicer-tests.XXXXXX")
printf 'Образ: %s\nЗадач сборки: %s\nЛоги: %s\n' "$runner_image" "$jobs" "$log_dir"
container_root=/__w/OrcaSlicer/OrcaSlicer
# Путь совпадает со штатной сборкой: CMakeCache содержит абсолютные пути контейнера.
# Аргументы передаются массивом напрямую, без eval и сборки shell-команды из данных.
container=("$container_cli" run --rm --pull=never --network=none --user "$(id -u):$(id -g)"
    -v "$repo_root:$container_root" -w "$container_root" "$runner_image")
if ((${#targets[@]})); then
    "${container[@]}" cmake --build build --config Release --target "${targets[@]}" --parallel "$jobs" \
        2>&1 | tee "$log_dir/build.log"
fi

if [[ "$suite" == version ]]; then
    # Изолированные Git-репозитории живут только в /tmp удаляемого контейнера.
    test_command=(cmake -DTEST_ROOT=/tmp/orcaslicer-version-tests "-DCHECK_APPIMAGE=$check_appimage"
        -P scripts/test/version.cmake)
elif [[ "$suite" == all ]]; then
    test_command=(scripts/run_unit_tests.sh build/tests Release)
else
    if (($# == 0)); then set -- '~[NotWorking]'; fi
    test_command=("build/tests/$suite/Release/${suite}_tests" --reporter compact --warn UnmatchedTestSpec "$@")
fi
if "${container[@]}" timeout --signal=TERM --kill-after=10s "${test_timeout}s" "${test_command[@]}" \
    2>&1 | tee "$log_dir/tests.log"
then
    printf 'Тесты завершены успешно. Логи: %s\n' "$log_dir"
else
    result=$?
    printf 'Тесты завершились с ошибкой (код %s). Логи: %s\n' "$result" "$log_dir" >&2
    exit "$result"
fi
