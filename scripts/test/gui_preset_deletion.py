#!/usr/bin/env python3
"""Проверяет удаление принтеров настоящими действиями GUI в отдельном каталоге.

Создаёт только синтетические профили без облачной учётной записи. Снимки библиотеки
проверяют сохранность JSON и служебных файлов после отмены и удаления последнего
принтера. Поддерживает старое подтверждение каскада для проверки воспроизводимости
дефекта на исходном AppImage; изменённый текст сам по себе не считается успехом.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

from gui_presets import Atspi, GLib, Node, PresetDialogs, prepare as prepare_presets


def library_snapshot(work: Path) -> dict[str, str]:
    """Возвращает хеши всей тестовой библиотеки печати и филаментов, включая `.info`."""
    root = work / "data" / "user" / "default"
    return {
        str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for kind in ("filament", "process")
        for path in sorted((root / kind).rglob("*"))
        if path.is_file()
    }


def prepare(work: Path) -> None:
    """Создаёт два самостоятельных принтера и профили с разными правилами совместимости.

    Штатные принтеры не устанавливаются: удаление второго действительно оставляет
    только внутренний Default Printer. Общая подготовка проверяет отдельный HOME.
    """
    prepare_presets(work)
    config_path = work / "data" / "OrcaSlicer.conf"
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["models"] = []
    config["filaments"] = []
    config["presets"] = {
        "machine": "Fork printer A",
        "print": "Fork process All",
        "filaments": ["Fork filament All"],
    }
    config_path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    root = work / "data" / "user" / "default"
    for kind in ("machine", "filament", "process"):
        (root / kind).mkdir(parents=True, exist_ok=False)
    for suffix in ("A", "B"):
        name = f"Fork printer {suffix}"
        profile = {
            "type": "machine",
            "name": name,
            "from": "User",
            "version": "2.5.0",
            "inherits": "",
            "printer_settings_id": name,
            "printer_model": "Fork Test Printer",
            "printer_variant": "0.4",
            "printer_technology": "FFF",
            "nozzle_diameter": ["0.4"],
        }
        (root / "machine" / f"{name}.json").write_text(
            json.dumps(profile, indent=2) + "\n", encoding="utf-8"
        )
    for kind in ("filament", "process"):
        for label, printers, condition, parent in (
            ("All", [], "", ""),
            ("Only A", ["Fork printer A"], "", ""),
            ("Only B", ["Fork printer B"], "", ""),
            ("Shared", ["Fork printer A", "Fork printer B"], "", ""),
            ("Condition", [], 'printer_model=="Fork Test Printer"', ""),
            ("Child", [], "", f"Fork {kind} All"),
        ):
            name = f"Fork {kind} {label}"
            profile = {
                "type": kind,
                "name": name,
                "from": "User",
                "version": "2.5.0",
                "inherits": parent,
                "compatible_printers": printers,
                "compatible_printers_condition": condition,
            }
            if kind == "filament":
                profile.update(
                    filament_settings_id=[name],
                    filament_type=["PLA"],
                    filament_is_support=["1"],
                )
            else:
                profile["print_settings_id"] = name
            (root / kind / f"{name}.json").write_text(
                json.dumps(profile, indent=2) + "\n", encoding="utf-8"
            )


class PresetDeletion(PresetDialogs):
    """Использует общий AT-SPI-драйвер; все действия ограничены тестовым приложением."""

    def confirmation(self, *, cancel: bool) -> None:
        """Нажимает кнопку стандартного либо старого каскадного подтверждения.

        wx-кнопки рисованные: выбираются только две листовые панели нужного диалога,
        по порядку создания в MessageDialog (Yes/No) или DeleteConfirmDialog (Cancel/Delete).
        """
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            nodes = self.snapshot()
            dialogs = [
                node
                for node in nodes
                if node.showing
                and node.role in ("dialog", "frame")
                and node.name
                in ("Delete Preset", "Orca Slicer - Delete", "OrcaSlicer - Delete")
            ]
            if len(dialogs) == 1:
                dialog = dialogs[0]
                buttons = []
                for node in nodes:
                    if (
                        not node.showing
                        or node.role != "panel"
                        or node.path[: len(dialog.path)] != dialog.path
                        or any(child.path[:-1] == node.path for child in nodes)
                    ):
                        continue
                    rect = node.accessible.get_component_iface().get_extents(
                        Atspi.CoordType.SCREEN
                    )
                    if rect.width >= 40 and 15 <= rect.height <= 60:
                        buttons.append(node)
                if len(buttons) != 2:
                    raise RuntimeError(
                        f"Неоднозначные кнопки подтверждения: {len(buttons)}"
                    )
                old_cascade = dialog.name != "Delete Preset"
                self.click(buttons[int(cancel != old_cascade)])
                return
            time.sleep(0.2)
        raise RuntimeError("Не найден диалог подтверждения удаления")

    def delete_button(self) -> Node:
        """Ищет кнопку удаления только внутри открытого редактора принтера."""
        dialog = self.find("Printer settings")
        buttons = [
            node
            for node in self.last_nodes
            if node.showing
            and node.description == "Delete this preset"
            and node.path[: len(dialog.path)] == dialog.path
        ]
        if len(buttons) != 1:
            raise RuntimeError("Не найдена однозначная кнопка удаления принтера")
        return buttons[0]

    def check_library(self, expected: dict[str, str]) -> None:
        """Даёт выполниться CallAfter; исчезновение процесса или изменение файлов — ошибка."""
        for _ in range(10):
            self.snapshot()
            time.sleep(0.2)
        actual = library_snapshot(self.work)
        if actual != expected:
            changed = sorted(
                key
                for key in expected.keys() | actual.keys()
                if expected.get(key) != actual.get(key)
            )
            raise RuntimeError(f"Удаление принтера изменило библиотеку: {changed}")

    def run(self) -> None:
        """Удаляет оба принтера либо проверяет библиотеку при повторном запуске gui.sh."""
        snapshot_path = self.work / "library-before.json"
        if snapshot_path.exists():
            self.check_library(json.loads(snapshot_path.read_text(encoding="utf-8")))
            print("После перезапуска библиотека сохранена", flush=True)
            return
        expected = library_snapshot(self.work)
        if len(expected) < 12:
            raise RuntimeError("Тестовые профили не были загружены полностью")
        for suffix in ("A", "B"):
            name = f"Fork printer {suffix}"
            self.open_editor(name)
            printer_file = (
                self.work / "data" / "user" / "default" / "machine" / f"{name}.json"
            )
            before = printer_file.read_bytes()
            self.click(self.delete_button())
            self.confirmation(cancel=True)
            self.check_library(expected)
            if printer_file.read_bytes() != before:
                raise RuntimeError("Отмена изменила профиль принтера")
            self.click(self.delete_button())
            self.confirmation(cancel=False)
            self.check_library(expected)
            if printer_file.exists():
                raise RuntimeError(f"GUI не удалил выбранный принтер: {name}")
            Atspi.generate_keyboard_event(0xFF1B, None, Atspi.KeySynthType.SYM)
            print(
                f"Удалён {name}; библиотека филаментов и печати сохранена", flush=True
            )
        snapshot_path.write_text(
            json.dumps(expected, indent=2) + "\n", encoding="utf-8"
        )


def main() -> int:
    """Готовит изолированную библиотеку либо выполняет реальные действия удаления."""
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
            PresetDeletion(args.work, args.group).run()
        return 0
    except (RuntimeError, OSError, GLib.Error) as error:
        print(f"GUI-регрессия не пройдена: {error}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
