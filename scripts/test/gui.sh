#!/usr/bin/env bash
set -euo pipefail

# Внутренний помощник `scripts/test/run.sh gui`: запускает готовый AppImage на Xvfb
# с отдельными HOME, XDG-каталогами, D-Bus и --datadir. Нужны xvfb-run, Xvfb,
# xwininfo, dbus-run-session, setsid, timeout и GNU-утилиты; сеть приложения не изолируется.
# SSL_CERT_FILE сохраняется; если он не задан, используется существующий системный CA-файл.
#
# --appimage PATH выбирает пакет; по умолчанию в build/ должен быть ровно один AppImage.
# --remote HOST копирует пакет и этот помощник в новый /tmp-каталог SSH-хоста;
# установленное приложение и рабочие пользовательские каталоги не используются.
# --presets дополнительно открывает и переключает диалоги сохранения через
# gui_presets.py; для него нужны Python 3, PyGObject и системный AT-SPI. Начальная
# конфигурация с комплектными профилями создаётся только в каталоге теста.
#
# Процессы теста останавливаются при выходе, временные файлы и полные логи сохраняются.
# Код 0 означает показ главного окна и пять успешных проверок работающего процесса;
# При --presets код 0 также требует прохождения сценария диалогов.
# Код 1 — сбой запуска/сценария, код 2 — неверные аргументы или отсутствие инструментов.

self=$(realpath -- "${BASH_SOURCE[0]}")

# Завершает только созданную тестом группу процессов, не ищет процессы по имени.
# SIGKILL ограничивает ожидание после SIGTERM; результаты приложения остаются в логах.
stop_app() {
    if [[ -n "${app_pid:-}" ]]; then
        kill -TERM -- "-$app_pid" 2>/dev/null || true
        for _ in {1..5}; do
            kill -0 "$app_pid" 2>/dev/null || break
            sleep 1
        done
        kill -KILL -- "-$app_pid" 2>/dev/null || true
        wait "$app_pid" 2>/dev/null || true
        app_pid=
    fi
}

# Выполняется только внутри нового Xvfb и D-Bus. Маркер после MainFrame::Show
# исключает ложный успех по заставке; окно и живой процесс проверяются дополнительно.
# Третий аргумент включает сценарий диалогов; его ошибки и завершение приложения не скрываются.
check_display() {
    local appimage=$1 work=$2 presets=$3 ready=0
    ulimit -c 0
    cd -- "$work/run"
    app_pid=
    trap stop_app EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    setsid "$appimage" --appimage-extract-and-run --datadir "$work/data" >"$work/app.log" 2>&1 &
    app_pid=$!
    for _ in {1..60}; do
        if ! kill -0 "$app_pid" 2>/dev/null; then
            local result=0
            wait "$app_pid" || result=$?
            printf 'GUI завершился до подтверждения запуска, код %s. Логи: %s\n' "$result" "$work" >&2
            return 1
        fi
        if grep -ERq --include='*.log*' 'Could not load bitmap:|Failed to load image from file' \
            "$work/app.log" "$work/data/log" 2>/dev/null; then
            printf 'Ошибка загрузки изображения. Логи: %s\n' "$work" >&2
            return 1
        fi
        if grep -RFq --include='*.log*' 'main frame firstly shown' "$work/data/log" 2>/dev/null &&
            xwininfo -root -tree >"$work/windows.log" && grep -Fq 'OrcaSlicer' "$work/windows.log"; then
            ready=$((ready + 1))
            if ((ready == 5)); then
                printf 'Главное окно показано, процесс работает, ошибок загрузки изображений нет. Логи: %s\n' "$work"
                if ((presets)); then
                    if ! python3 "$(dirname -- "$self")/gui_presets.py" --work "$work" --group "$app_pid"; then
                        if ! kill -0 "$app_pid" 2>/dev/null; then
                            local app_result=0
                            wait "$app_pid" || app_result=$?
                            printf 'Тестовое приложение завершилось с кодом %s.\n' "$app_result" >&2
                        fi
                        return 1
                    fi
                    printf 'Диалоги сохранения прошли GUI-регрессию. Логи: %s\n' "$work"
                fi
                return 0
            fi
        else
            ready=0
        fi
        sleep 1
    done
    printf 'Главное окно не подтверждено за 60 проверок. Логи: %s\n' "$work" >&2
    return 1
}

# Служебный вход получает только созданный внешним запуском временный каталог.
if [[ "${1:-}" == --display ]]; then
    if (($# != 4)) || [[ ! -d "$3" || ! -x "$2" || -z "${DISPLAY:-}" || ! "$4" =~ ^[01]$ ]]; then exit 2; fi
    check_display "$2" "$3" "$4"
    stop_app
    exit 0
fi

remote='' appimage='' presets=0
while (($#)); do
    case "$1" in
        --presets) presets=1; shift ;;
        --remote|--appimage)
            if (($# < 2)); then printf 'Для %s нужно значение.\n' "$1" >&2; exit 2; fi
            if [[ "$1" == --remote ]]; then remote=$2; else appimage=$2; fi
            shift 2 ;;
        *) printf 'Неизвестный аргумент GUI-проверки: %s\n' "$1" >&2; exit 2 ;;
    esac
done
if [[ -z "$appimage" ]]; then
    repo_root=$(cd -- "$(dirname -- "$self")/../.." && pwd -P)
    shopt -s nullglob
    images=("$repo_root"/build/OrcaSlicer_Linux_V*.AppImage)
    if ((${#images[@]} != 1)); then
        printf 'Укажи --appimage PATH: в build/ найдено пакетов: %s.\n' "${#images[@]}" >&2
        exit 2
    fi
    appimage=${images[0]}
fi
appimage=$(realpath -e -- "$appimage")
if [[ ! -f "$appimage" || ! -x "$appimage" ]]; then
    printf 'AppImage отсутствует или не имеет права запуска: %s\n' "$appimage" >&2
    exit 2
fi

if [[ -n "$remote" ]]; then
    if [[ ! "$remote" =~ ^[[:alnum:]_][[:alnum:]_.@-]*$ ]]; then
        printf 'Недопустимый SSH-хост: %s\n' "$remote" >&2; exit 2
    fi
    ssh_options=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=10 -o ServerAliveCountMax=3)
    remote_dir=$(ssh "${ssh_options[@]}" "$remote" 'mktemp -d /tmp/orcaslicer-gui-upload.XXXXXXXX')
    # В удалённую команду попадает только проверенный путь mktemp, не произвольная строка SSH.
    if [[ ! "$remote_dir" =~ ^/tmp/orcaslicer-gui-upload\.[[:alnum:]]+$ ]]; then
        printf 'Неожиданный временный путь удалённого теста.\n' >&2; exit 2
    fi
    printf 'Пакет и помощник: %s:%s\n' "$remote" "$remote_dir"
    scp -p "${ssh_options[@]}" -- "$self" "$remote:$remote_dir/gui.sh"
    preset_option=''
    if ((presets)); then
        scp -p "${ssh_options[@]}" -- "$(dirname -- "$self")/gui_presets.py" "$remote:$remote_dir/gui_presets.py"
        preset_option=' --presets'
    fi
    scp -p "${ssh_options[@]}" -- "$appimage" "$remote:$remote_dir/OrcaSlicer.AppImage"
    digest=$(sha256sum -- "$appimage")
    digest=${digest%% *}
    # Подстановка на клиенте намеренная: digest получен из sha256sum, remote_dir проверен выше,
    # а preset_option содержит только фиксированный флаг либо пустую строку.
    # shellcheck disable=SC2029
    ssh "${ssh_options[@]}" "$remote" \
        "printf '%s  %s\\n' '$digest' '$remote_dir/OrcaSlicer.AppImage' | sha256sum -c - && bash '$remote_dir/gui.sh' --appimage '$remote_dir/OrcaSlicer.AppImage'$preset_option"
    exit $?
fi

for tool in Xvfb xvfb-run xwininfo dbus-run-session setsid timeout; do
    if ! command -v "$tool" >/dev/null; then
        printf 'Для изолированной GUI-проверки нужен %s.\n' "$tool" >&2; exit 2
    fi
done
if ((presets)) && ! command -v python3 >/dev/null; then
    printf 'Для сценария --presets нужен Python 3 с PyGObject и AT-SPI.\n' >&2
    exit 2
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/orcaslicer-gui.XXXXXXXX")
printf 'Логи GUI-проверки: %s\n' "$work"
sha256sum -- "$appimage" >"$work/appimage.sha256"
# Изоляция задаётся до D-Bus: активированные им службы также не должны использовать
# рабочие HOME/XDG-каталоги пользователя. Xvfb создаёт собственный XAUTHORITY.
mkdir -p "$work"/{home,config,cache,data,runtime,tmp,run}
chmod 0700 "$work/runtime"
export HOME="$work/home" XDG_CONFIG_HOME="$work/config" XDG_CACHE_HOME="$work/cache"
export XDG_DATA_HOME="$work/data" XDG_RUNTIME_DIR="$work/runtime" TMPDIR="$work/tmp"
export GDK_BACKEND=x11 ZINK_DISABLE_OVERRIDE=1 WEBKIT_DISABLE_DMABUF_RENDERER=1
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe GIO_USE_VFS=local
# Статический OpenSSL из AppImage может искать CA по пути сборочного контейнера.
# Явный системный CA устраняет диалог его выбора, не отключая проверку сертификатов.
if [[ -z "${SSL_CERT_FILE:-}" && -f /etc/ssl/certs/ca-certificates.crt ]]; then
    export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
fi
unset WAYLAND_DISPLAY
test_timeout=90
if ((presets)); then
    export NO_AT_BRIDGE=0 LANG=C.UTF-8 LC_ALL=C.UTF-8
    python3 "$(dirname -- "$self")/gui_presets.py" --work "$work" --prepare
    test_timeout=240
fi
if timeout --signal=TERM --kill-after=10s "${test_timeout}s" xvfb-run -a -e "$work/xvfb.log" \
    -s '-screen 0 1600x1000x24 -nolisten tcp' \
    dbus-run-session -- bash "$self" --display "$appimage" "$work" "$presets" 2>&1 | tee "$work/test.log"; then
    exit 0
else
    result=$?
    printf 'GUI-проверка не пройдена, код %s. Логи: %s\n' "$result" "$work" >&2
    exit "$result"
fi
