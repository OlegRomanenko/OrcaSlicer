#!/usr/bin/env python3
"""Проверяет поиск профилей в диалоге сравнения в изолированной сессии `gui.sh`.

Создаёт синтетическую библиотеку и через настоящий диалог сохранения добавляет
одноимённые проектные копии с другими значениями. AT-SPI управляет только тестовой
группой процессов; рабочие профили, облако и устройства не используются. Журналы
и снимки доступны в каталоге теста, жизненным циклом приложения управляет `gui.sh`.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

from gui_presets import Atspi, GLib, Node, PresetDialogs, prepare as prepare_presets


def prepare(work: Path) -> None:
    """Создаёт пары профилей с разными температурами и высотой слоя в отдельном HOME.

    `Source` служит источником проектной копии с именем `Compare`: различия получаются
    через штатное сохранение, без ручного конструирования `.3mf` или скрытых API GUI.
    """
    prepare_presets(work)
    config_path = work / "data" / "OrcaSlicer.conf"
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["presets"]["machine"] = "Compare printer"
    config["orca_presets"] = [
        {
            "machine": "Compare printer",
            "process": "Source process",
            "filament": "Source filament",
        }
    ]
    config_path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    root = work / "data" / "user" / "default"
    for kind in ("machine", "filament", "process"):
        (root / kind).mkdir(parents=True, exist_ok=False)
    profiles = [
        {
            "type": "machine",
            "name": "Compare printer",
            "inherits": "Bambu Lab A1 mini 0.4 nozzle",
            "printer_settings_id": "Compare printer",
        }
    ]
    for prefix, temperature, layer in (
        ("Compare", "220", "0.16"),
        ("Source", "230", "0.24"),
    ):
        profiles.append(
            {
                "type": "filament",
                "name": f"{prefix} filament",
                "inherits": "Generic PLA @BBL A1M",
                "filament_settings_id": [f"{prefix} filament"],
                "nozzle_temperature": [temperature],
                "compatible_printers": [],
                "compatible_printers_condition": "",
            }
        )
        profiles.append(
            {
                "type": "process",
                "name": f"{prefix} process",
                "inherits": "0.20mm Standard @BBL A1M",
                "print_settings_id": f"{prefix} process",
                "layer_height": layer,
                "compatible_printers": [],
                "compatible_printers_condition": "",
            }
        )
    for profile in profiles:
        profile.update(version="2.5.0", **{"from": "User"})
        path = root / profile["type"] / f"{profile['name']}.json"
        path.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")


class PresetComparison(PresetDialogs):
    """Проверяет открытие сравнения для обеих областей без изменения пользовательских файлов."""

    def frame(self, title: str) -> Node:
        """Ждёт окно по имени, не путая его с одноимённой кнопкой в основном окне."""
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            frames = [
                node
                for node in self.snapshot()
                if node.showing
                and node.role in ("frame", "dialog")
                and node.name == title
            ]
            if len(frames) == 1:
                return frames[0]
            time.sleep(0.2)
        raise RuntimeError(f"Не найдено однозначное окно: {title}")

    def wait_closed(self, title: str) -> None:
        """Ждёт исчезновения окна, продолжая проверять, что приложение не завершилось."""
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if not any(
                node.showing and node.name == title and node.role in ("frame", "dialog")
                for node in self.snapshot()
            ):
                return
            time.sleep(0.2)
        raise RuntimeError(f"Окно осталось открытым: {title}")

    def escape(self, title: str) -> None:
        """Закрывает текущий диалог штатной клавишей Escape, не завершая приложение."""
        self.frame(title)
        Atspi.generate_keyboard_event(0xFF1B, None, Atspi.KeySynthType.SYM)
        self.wait_closed(title)

    def compare_values(self) -> str:
        """Собирает видимые подписи сравнения и немедленно отвергает ошибку поиска профиля."""
        frame = self.frame("Compare presets")
        nodes = [
            node
            for node in self.last_nodes
            if node.showing and node.path[: len(frame.path)] == frame.path
        ]
        values = [node.name for node in nodes if node.name]
        if any("One of the presets does not exist" in value for value in values):
            raise RuntimeError(
                "Compare presets не находит выбранный пользовательский или проектный профиль"
            )
        (self.work / "compare-values.json").write_text(
            json.dumps(values, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        return "\n".join(values)

    def compare_combos(self, kind: str) -> list[Node]:
        """Находит две подписанные панели нужной строки и упорядочивает их слева направо."""
        frame = self.frame("Compare presets")
        panels = [
            node
            for node in self.last_nodes
            if node.showing
            and node.role == "panel"
            and node.path[: len(frame.path)] == frame.path
            and node.description.startswith((f"Compare {kind} [", f"Source {kind} ["))
        ]
        if len(panels) != 2:
            raise RuntimeError(
                f"Не найдены две стороны сравнения {kind}: {len(panels)}"
            )
        return sorted(
            panels,
            key=lambda node: node.accessible.get_component_iface()
            .get_extents(Atspi.CoordType.SCREEN)
            .x,
        )

    def close_compare(self) -> None:
        """Передаёт фокус рисованному списку перед Escape в немодальном окне без WM."""
        self.click(self.compare_combos("process")[0])
        self.escape("Compare presets")

    def save_project_copy(self, kind: str, name: str) -> None:
        """Создаёт проектную копию через реальный диалог с новым именем и выбранной областью."""
        self.click(self.find(f"Save current {kind}"))
        dialog = self.frame("Save preset")
        entries = [
            node
            for node in self.last_nodes
            if node.showing
            and node.path[: len(dialog.path)] == dialog.path
            and node.accessible.get_state_set().contains(Atspi.StateType.EDITABLE)
        ]
        if len(entries) != 1:
            raise RuntimeError(f"Не найдено однозначное поле имени: {len(entries)}")
        if not entries[0].accessible.get_editable_text_iface().set_text_contents(name):
            raise RuntimeError("Поле имени отклонило новое значение")
        self.click(self.dialog_controls(("icon", "panel", "icon", "panel"))[3])
        self.click(self.dialog_controls(("panel", "panel"))[0])
        self.wait_closed("Save preset")

    def library_snapshot(self) -> dict[str, str]:
        """Фиксирует все файлы частной библиотеки, чтобы сравнение не сохраняло их молча."""
        root = self.work / "data" / "user" / "default"
        return {
            str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(root.rglob("*"))
            if path.is_file()
        }

    def run(self) -> None:
        """Воспроизводит ошибку поиска User и проверяет открытие сравнения проектных копий.

        Переключение сторон, дерево разных значений и перенос проверяются отдельно:
        этот сценарий не утверждает покрытие недоступных через AT-SPI пунктов ComboBox.
        """
        before = self.library_snapshot()
        self.click(self.find("Compare presets"))
        self.compare_values()
        # Проверка старого бинарника должна упасть на реальной ошибке поиска до проверки новой подписи.
        if not any(
            node.showing and "[User]" in node.description for node in self.last_nodes
        ):
            raise RuntimeError("Пользовательская пометка не сокращена до [User]")
        self.close_compare()
        self.save_project_copy("Process", "Compare process")
        self.open_editor("Source filament")
        self.save_project_copy("Filament", "Compare filament")
        self.escape("Material settings")
        self.click(self.find("Compare presets"))
        self.compare_values()
        for kind in ("process", "filament"):
            if any(
                node.description != f"Compare {kind} [Preset Inside Project]"
                for node in self.compare_combos(kind)
            ):
                raise RuntimeError(f"В сравнении не выбраны проектные копии {kind}")
        self.click(self.find("Show all presets (including incompatible)"))
        self.compare_values()
        if self.library_snapshot() != before:
            raise RuntimeError(
                "Сравнение или проектное сохранение изменили файлы пользовательской библиотеки"
            )
        print(
            "Сравнение открывается для User и Project; пользовательская библиотека не изменена",
            flush=True,
        )


def main() -> int:
    """Подготавливает только частный каталог либо выполняет сценарий в группе тестового процесса."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--group", type=int)
    parser.add_argument("--prepare", action="store_true")
    args = parser.parse_args()
    try:
        if args.prepare:
            prepare(args.work)
        elif args.group is None:
            parser.error("Для действий GUI нужен --group")
        else:
            PresetComparison(args.work, args.group).run()
        return 0
    except (RuntimeError, OSError, GLib.Error) as error:
        print(f"GUI-регрессия не пройдена: {error}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
