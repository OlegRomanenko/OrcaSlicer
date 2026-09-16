# Регрессионная проверка версии и шаблонов упаковки через настоящий Git и CMake.
# `scripts/test/run.sh version` задаёт новый `TEST_ROOT` внутри одноразового контейнера.
# Создаются только локальные тестовые репозитории и файлы под `TEST_ROOT`; сеть,
# рабочая ветка исходников и пользовательские настройки Git не изменяются.
# Повторный запуск с существующим каталогом запрещён; его удаляет владелец контейнера.
# Любое несовпадение или ошибка дочерней команды завершают проверку с ненулевым кодом.
# `CHECK_APPIMAGE` добавляет запуск готового AppImage с `--help`: пользовательские
# каталоги и рабочая директория изолируются внутри `TEST_ROOT`, GUI не запускается.

cmake_minimum_required(VERSION 3.13)
get_filename_component(source_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Дочерний процесс включает `version.inc` из проверяемого checkout и проверяет
# публичные результаты: строку версии, её компоненты и оба сгенерированных шаблона.
if(DEFINED VERSION_PROBE)
    include("${VERSION_PROBE}/version.inc")
    if(NOT SoftFever_VERSION STREQUAL EXPECTED_VERSION)
        message(FATAL_ERROR "Версия '${SoftFever_VERSION}', ожидалась '${EXPECTED_VERSION}'")
    endif()
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" match "${EXPECTED_VERSION}")
    set(expected_numbers "${CMAKE_MATCH_1};${CMAKE_MATCH_2};${CMAKE_MATCH_3}")
    if(NOT "${ORCA_VERSION_MAJOR};${ORCA_VERSION_MINOR};${ORCA_VERSION_PATCH}" STREQUAL expected_numbers)
        message(FATAL_ERROR "Числовые компоненты не соответствуют версии")
    endif()
    if(NOT SLIC3R_VERSION STREQUAL EXPECTED_INTERNAL_VERSION)
        message(FATAL_ERROR "Изменилась внутренняя версия совместимости")
    endif()
    configure_file("${source_root}/src/libslic3r/libslic3r_version.h.in" "${TEST_ROOT}/version.h" @ONLY)
    configure_file("${source_root}/src/dev-utils/platform/unix/build_appimage.sh.in" "${TEST_ROOT}/appimage.sh" @ONLY)
    file(STRINGS "${TEST_ROOT}/version.h" header_version REGEX "^#define SoftFever_VERSION ")
    file(STRINGS "${TEST_ROOT}/appimage.sh" image_name REGEX "^APP_IMAGE=")
    if(NOT header_version STREQUAL "#define SoftFever_VERSION \"${EXPECTED_VERSION}\"")
        message(FATAL_ERROR "Заголовок содержит другую версию: ${header_version}")
    endif()
    if(NOT image_name STREQUAL "APP_IMAGE=\"OrcaSlicer_Linux_V${EXPECTED_VERSION}.AppImage\"")
        message(FATAL_ERROR "Имя AppImage содержит другую версию: ${image_name}")
    endif()
    return()
endif()

if(NOT IS_ABSOLUTE "${TEST_ROOT}" OR EXISTS "${TEST_ROOT}")
    message(FATAL_ERROR "TEST_ROOT должен быть абсолютным путём к новому тестовому каталогу")
endif()
file(MAKE_DIRECTORY "${TEST_ROOT}/repo" "${TEST_ROOT}/empty-hooks")
set(repo "${TEST_ROOT}/repo")
file(COPY "${source_root}/version.inc" DESTINATION "${repo}")

# Резервная версия принадлежит апстриму; тест не закрепляет её числовое значение.
set(CMAKE_DISABLE_FIND_PACKAGE_Git TRUE)
include("${source_root}/version.inc")
set(upstream_version "${SoftFever_VERSION}")
set(internal_version "${SLIC3R_VERSION}")
unset(CMAKE_DISABLE_FIND_PACKAGE_Git)
find_package(Git REQUIRED)

# Команда передаётся списком аргументов без shell. При ошибке сохраняется диагностика;
# вызывающий код управляет только временными репозиториями и сборкой без компилятора.
function(run_checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Команда завершилась с кодом ${result}: ${ARGV}\n${output}\n${error}")
    endif()
endfunction()

# Все Git-операции направлены на тестовый checkout. Подпись и hooks отключены,
# личность тестового коммита задаётся аргументами, не через пользовательский config.
function(test_git)
    run_checked("${GIT_EXECUTABLE}" -C "${repo}"
        -c user.name=VersionTest -c user.email=version-test@example.invalid
        -c commit.gpgsign=false "-c" "core.hooksPath=${TEST_ROOT}/empty-hooks" ${ARGV})
endfunction()

# Свежий процесс CMake исключает влияние предыдущего поиска Git и состояния include.
# `disable_git` воспроизводит сборку без необязательного Git; результат виден в логе.
function(expect_version name directory expected disable_git)
    run_checked("${CMAKE_COMMAND}" "-DVERSION_PROBE=${directory}" "-DEXPECTED_VERSION=${expected}"
        "-DEXPECTED_INTERNAL_VERSION=${internal_version}" "-DTEST_ROOT=${TEST_ROOT}"
        "-DCMAKE_DISABLE_FIND_PACKAGE_Git=${disable_git}" -P "${CMAKE_CURRENT_LIST_FILE}")
    message(STATUS "${name}: ${expected}")
endfunction()

test_git(init -q --template=)
test_git(symbolic-ref HEAD refs/heads/main)
test_git(commit -q --allow-empty -m "Фикстура версии")
foreach(version IN ITEMS 2.5.0-lora 3.12.4-custom.2 3.12.4)
    test_git(checkout -q -B "v${version}")
    expect_version("Именованная ветка" "${repo}" "${version}" FALSE)
endforeach()
foreach(branch IN ITEMS main feature/v2.5.0-lora v2.5.0-other/extra v2.5.0-lora+metadata)
    test_git(checkout -q -B "${branch}")
    expect_version("Резервная версия для ${branch}" "${repo}" "${upstream_version}" FALSE)
endforeach()
test_git(checkout -q -B v2.5.0-lora)
test_git(tag v2.5.0-lora)
expect_version("Ветка с одноимённым тегом" "${repo}" "2.5.0-lora" FALSE)
expect_version("Git недоступен" "${repo}" "${upstream_version}" TRUE)

# Вложенный архив не должен заимствовать версию родительского Git-репозитория.
file(MAKE_DIRECTORY "${repo}/archive")
file(COPY "${source_root}/version.inc" DESTINATION "${repo}/archive")
expect_version("Архив без .git" "${repo}/archive" "${upstream_version}" FALSE)
test_git(tag v9.8.7-lora)
test_git(checkout -q --detach)
expect_version("detached HEAD с тегом" "${repo}" "${upstream_version}" FALSE)

test_git(worktree add -q -b v4.1.0-lora "${TEST_ROOT}/worktree" HEAD)
file(COPY "${source_root}/version.inc" DESTINATION "${TEST_ROOT}/worktree")
expect_version("Git worktree" "${TEST_ROOT}/worktree" "4.1.0-lora" FALSE)

# Обычная сборка должна заметить смену ветки по зависимости от Git HEAD, без
# явного повторного `cmake -S`. Минимальный проект не включает языки и компилятор.
file(WRITE "${repo}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.13)
project(VersionFixture NONE)
include(version.inc)
file(WRITE "${CMAKE_BINARY_DIR}/selected-version.txt" "${SoftFever_VERSION}")
]=])
test_git(checkout -q -B v2.5.0-lora)
run_checked("${CMAKE_COMMAND}" -S "${repo}" -B "${TEST_ROOT}/build" -G Ninja)
test_git(checkout -q -B v3.12.4-custom.2)
run_checked("${CMAKE_COMMAND}" --build "${TEST_ROOT}/build")
file(READ "${TEST_ROOT}/build/selected-version.txt" rebuilt_version)
if(NOT rebuilt_version STREQUAL "3.12.4-custom.2")
    message(FATAL_ERROR "Смена ветки не обновила настроенную сборку: ${rebuilt_version}")
endif()
message(STATUS "Версия обновляется при сборке после смены ветки")

if(CHECK_APPIMAGE)
    # Проверяется не только шаблон имени, но и версия внутри упакованного бинарника.
    # FUSE не нужен; временное извлечение AppImage и конфигурация остаются в контейнере.
    include("${source_root}/version.inc")
    set(appimage "${source_root}/build/${SLIC3R_APP_KEY}_Linux_V${SoftFever_VERSION}.AppImage")
    if(NOT EXISTS "${appimage}")
        message(FATAL_ERROR "AppImage не собран: ${appimage}")
    endif()
    file(MAKE_DIRECTORY "${TEST_ROOT}/home" "${TEST_ROOT}/run")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "HOME=${TEST_ROOT}/home"
        "XDG_CONFIG_HOME=${TEST_ROOT}/home/config" "XDG_CACHE_HOME=${TEST_ROOT}/home/cache"
        "XDG_DATA_HOME=${TEST_ROOT}/home/data" "TMPDIR=${TEST_ROOT}"
        "${appimage}" --appimage-extract-and-run --help
        WORKING_DIRECTORY "${TEST_ROOT}/run"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "AppImage завершился с кодом ${result}:\n${output}\n${error}")
    endif()
    string(FIND "${output}" "${SLIC3R_APP_KEY}-${SoftFever_VERSION}:" version_position)
    if(version_position EQUAL -1)
        message(FATAL_ERROR "AppImage не вывел ожидаемую версию:\n${output}")
    endif()
    message(STATUS "Готовый AppImage сообщает ${SLIC3R_APP_KEY}-${SoftFever_VERSION}")
endif()
