#!/usr/bin/env python3
"""Проверяет настоящие диалоги сохранения в изолированной сессии `gui.sh`.

Использует системные PyGObject и AT-SPI, без установки Python-пакетов. Работает
только с приложением из указанной группы процессов при отдельных HOME/XDG.
Снимки дерева доступности сохраняются в каталоге теста для диагностики; рабочие
профили не читаются. Отсутствие элемента, отказ действия и завершение приложения
считаются ошибкой, а не успешной проверкой запуска.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path

try:
    import gi

    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi, GLib
except (ImportError, ValueError) as error:
    raise SystemExit(f"Для GUI-регрессии нужны PyGObject и AT-SPI: {error}") from error


@dataclass
class Node:
    """Снимок доступного элемента; GI удерживает ссылку на его объект AT-SPI."""

    accessible: Atspi.Accessible
    name: str
    description: str
    role: str
    showing: bool
    enabled: bool
    path: tuple[int, ...]

    def describe(self) -> dict:
        """Возвращает сериализуемую диагностику без указателей и окружения процесса."""
        return {
            "name": self.name,
            "description": self.description,
            "role": self.role,
            "showing": self.showing,
            "enabled": self.enabled,
            "path": self.path,
        }


class PresetDialogs:
    """Ограничивает действия группой тестового приложения и проверяет ответы GUI."""

    def __init__(self, work: Path, group: int) -> None:
        self.work = work.resolve()
        self.group = group
        self.last_nodes: list[Node] = []
        if (
            Path(os.environ.get("HOME", "")).resolve() != self.work / "home"
            or Path(os.environ.get("XDG_RUNTIME_DIR", "")).resolve()
            != self.work / "runtime"
            or not os.environ.get("DISPLAY")
            or not os.environ.get("DBUS_SESSION_BUS_ADDRESS")
        ):
            raise RuntimeError("GUI-регрессия требует отдельную сессию gui.sh")
        Atspi.init()
        Atspi.set_timeout(1500, 3000)

    def alive(self) -> None:
        """Не позволяет трактовать исчезновение окна после падения как успешное закрытие."""
        try:
            os.kill(self.group, 0)
        except ProcessLookupError as error:
            raise RuntimeError("Тестовое приложение завершилось") from error

    def snapshot(self) -> list[Node]:
        """Обходит только дерево приложения нужной группы; снимок ограничен по размеру."""
        self.alive()
        context = GLib.MainContext.default()
        for _ in range(50):
            if not context.pending():
                break
            context.iteration(False)
        nodes: list[Node] = []

        def visit(accessible: Atspi.Accessible, path: tuple[int, ...]) -> None:
            """Добавляет узел и потомков; путь нужен для различения одинаковых подписей."""
            if len(nodes) >= 5000 or len(path) > 40:
                raise RuntimeError("Дерево GUI превысило безопасный предел обхода")
            states = accessible.get_state_set()
            nodes.append(
                Node(
                    accessible,
                    accessible.get_name() or "",
                    accessible.get_description() or "",
                    accessible.get_role_name(),
                    states.contains(Atspi.StateType.SHOWING),
                    states.contains(Atspi.StateType.SENSITIVE),
                    path,
                )
            )
            for index in range(accessible.get_child_count()):
                child = accessible.get_child_at_index(index)
                if child is not None:
                    visit(child, (*path, index))

        desktop = Atspi.get_desktop(0)
        for index in range(desktop.get_child_count()):
            app = desktop.get_child_at_index(index)
            try:
                belongs = os.getpgid(app.get_process_id()) == self.group
            except ProcessLookupError:
                continue
            if belongs:
                app.clear_cache()
                visit(app, (index,))
        if not nodes:
            # Сохраняем последний содержательный снимок, если процесс уже исчезает.
            self.alive()
            return nodes
        self.last_nodes = nodes
        (self.work / "ui-tree.json").write_text(
            json.dumps(
                [node.describe() for node in nodes], ensure_ascii=False, indent=2
            )
            + "\n",
            encoding="utf-8",
        )
        return nodes

    def find(self, label: str, *, timeout: float = 10) -> Node:
        """Ждёт единственный видимый элемент с точным именем либо описанием."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            found = [
                node
                for node in self.snapshot()
                if node.showing and label in (node.name, node.description)
            ]
            if len(found) == 1:
                return found[0]
            if len(found) > 1:
                raise RuntimeError(f"Неоднозначный элемент GUI: {label}")
            time.sleep(0.2)
        raise RuntimeError(f"Не найден элемент GUI: {label}; снимок: {self.work}")

    def click(self, node: Node, *, hover: bool = False) -> None:
        """Нажимает элемент по границам AT-SPI; hover только подводит указатель.

        Синтез мыши не блокируется в модальном wx-обработчике, в отличие от do_action.
        """
        self.alive()
        if not node.enabled:
            raise RuntimeError(f"Элемент отключён: {node.describe()}")
        print(
            json.dumps(
                {"hover" if hover else "click": node.describe()}, ensure_ascii=False
            ),
            flush=True,
        )
        component = node.accessible.get_component_iface()
        if component is None:
            raise RuntimeError(f"У элемента нет координат: {node.describe()}")
        rect = component.get_extents(Atspi.CoordType.SCREEN)
        if rect.x < 0 or rect.y < 0 or rect.width <= 0 or rect.height <= 0:
            raise RuntimeError(f"Элемент за пределами экрана: {node.describe()}")
        if not Atspi.generate_mouse_event(
            rect.x + rect.width // 2,
            rect.y + rect.height // 2,
            "abs" if hover else "b1c",
        ):
            raise RuntimeError(f"GUI отклонил действие: {node.describe()}")
        time.sleep(0.2)

    def open_editor(self, preset: str) -> None:
        """Находит кнопку редактирования рядом с заданным штатным профилем, без координат."""
        profiles = [
            node
            for node in self.snapshot()
            if node.showing and node.role == "panel" and node.description == preset
        ]
        if len(profiles) != 1:
            raise RuntimeError(f"Не найден однозначный селектор профиля: {preset}")
        # Кнопка принтера показывается только при наведении на его строку в Sidebar.
        self.click(profiles[0], hover=True)
        nodes = self.snapshot()
        candidates = []
        for node in nodes:
            if not node.showing or node.description != "Click to edit preset":
                continue
            parent = node.path[:-1]
            if any(
                sibling.showing
                and sibling.path[: len(parent)] == parent
                and preset in (sibling.name, sibling.description)
                for sibling in nodes
            ):
                candidates.append(node)
        if len(candidates) != 1:
            raise RuntimeError(f"Не найдена однозначная кнопка редактора: {preset}")
        self.click(candidates[0])

    def dialog_controls(self, roles: tuple[str, ...]) -> list[Node]:
        """Находит безымянные рисованные контролы по единственной структуре внутри диалога.

        RadioGroup содержит пары icon/panel, DialogButtons — панели OK/Cancel.
        Их нарисованные подписи недоступны через AT-SPI; неоднозначность структуры
        считается ошибкой теста, а не поводом нажимать произвольный элемент.
        """
        dialog = self.find("Save preset")
        groups = []
        for parent in self.last_nodes:
            if not parent.showing or parent.path[: len(dialog.path)] != dialog.path:
                continue
            children = [
                node
                for node in self.last_nodes
                if node.showing and node.path[:-1] == parent.path
            ]
            if tuple(node.role for node in children) == roles:
                groups.append(children)
        if len(groups) != 1:
            raise RuntimeError(f"Неоднозначная структура контролов диалога: {roles}")
        return groups[0]

    def cancel(self) -> None:
        """Нажимает Cancel в штатной паре DialogButtons и ждёт закрытия диалога."""
        self.click(self.dialog_controls(("panel", "panel"))[1])
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if not any(
                node.showing and node.name == "Save preset" for node in self.snapshot()
            ):
                self.alive()
                return
            time.sleep(0.2)
        raise RuntimeError("Cancel не закрыл диалог сохранения")

    def run(self) -> None:
        """Открывает и закрывает три диалога, включая повторные события выбора области."""
        for kind in ("Process", "Filament", "Machine"):
            if kind != "Process":
                self.open_editor(
                    "Generic PLA @BBL A1M"
                    if kind == "Filament"
                    else "Bambu Lab A1 mini 0.4 nozzle"
                )
            self.click(self.find(f"Save current {kind}"))
            self.find("Save preset")
            for index in (0, 0, 1, 0):
                controls = self.dialog_controls(("icon", "panel", "icon", "panel"))
                self.click(controls[index * 2 + 1])
                self.find("Save preset")
            self.cancel()
            # Повторное открытие проверяет новый экземпляр диалога, не только живой процесс.
            self.click(self.find(f"Save current {kind}"))
            self.find("Save preset")
            self.cancel()
            if kind != "Process":
                Atspi.generate_keyboard_event(0xFF1B, None, Atspi.KeySynthType.SYM)
                self.find("Save current Process")
            self.alive()
            print(
                f"Диалог {kind}: открытие, переключение и отмена выполнены", flush=True
            )


def prepare(work: Path) -> None:
    """Создаёт минимальную настройку из штатных профилей только в пустом каталоге теста."""
    if Path(os.environ.get("HOME", "")).resolve() != work.resolve() / "home":
        raise RuntimeError("Подготовка требует отдельный HOME теста")
    config = {
        "app": {
            "language": "en_US",
            "version": "2.5.0-lora",
            "user_mode": "expert",
            "default_page": "1",
            "show_daily_tips": False,
            "show_splash_screen": False,
            "preset_bundle_auto_update": False,
            "sync_system_preset": False,
            "sync_user_preset": False,
            "enable_ota": False,
            "single_instance": False,
        },
        "models": [
            {"vendor": "BBL", "model": "Bambu Lab A1 mini", "nozzle_diameter": "0.4"}
        ],
        "filaments": ["Generic PLA @BBL A1M"],
        "presets": {
            "machine": "Bambu Lab A1 mini 0.4 nozzle",
            "print": "0.20mm Standard @BBL A1M",
            "filaments": ["Generic PLA @BBL A1M"],
        },
    }
    with (work / "data" / "OrcaSlicer.conf").open("x", encoding="utf-8") as output:
        json.dump(config, output, indent=2)
        output.write("\n")


def main() -> int:
    """Запускает сценарий или диагностический снимок только в каталоге тестовой сессии."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--group", type=int)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--inspect", action="store_true")
    args = parser.parse_args()
    try:
        if args.prepare:
            prepare(args.work)
            return 0
        if args.group is None:
            parser.error("Для действий GUI нужен --group")
        driver = PresetDialogs(args.work, args.group)
        if args.inspect:
            driver.snapshot()
        else:
            driver.run()
        return 0
    except (RuntimeError, OSError, GLib.Error) as error:
        print(f"GUI-регрессия не пройдена: {error}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
