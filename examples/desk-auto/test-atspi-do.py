#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("atspi-do.py")
SPEC = importlib.util.spec_from_file_location("atspi_do", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def node(token, *, role="text", name="", interfaces=(), width=0, height=0):
    return token, {
        "role": role,
        "name": name,
        "text": name,
        "ifaces": list(interfaces),
        "x": 0,
        "y": 0,
        "w": width,
        "h": height,
    }


class TypeTargetTest(unittest.TestCase):
    def test_exact_application_name_wins_over_substring_match(self):
        class App:
            def __init__(self, name):
                self.name = name

        exact = App("wps")
        matches = MODULE.select_applications([App("WPS for Pad"), exact], "wps")
        self.assertEqual([exact], matches)

    def test_wps_license_label_is_not_an_editor(self):
        nodes = [node(
            "license",
            role="label",
            interfaces=("EditableText",),
            width=900,
            height=500,
        )]
        self.assertEqual((None, None, None), MODULE.choose_type_target(nodes))

    def test_large_text_editor_is_supported(self):
        nodes = [node(
            "editor",
            role="text",
            interfaces=("EditableText",),
            width=800,
            height=600,
        )]
        method, target, info = MODULE.choose_type_target(nodes)
        self.assertEqual("editable", method)
        self.assertEqual("editor", target)
        self.assertEqual("text", info["role"])

    def test_wps_document_frame_uses_keyboard_events(self):
        nodes = [node(
            "canvas",
            role="frame",
            name="Document1",
            width=1200,
            height=800,
        )]
        method, target, info = MODULE.choose_type_target(nodes)
        self.assertEqual("keyboard", method)
        self.assertEqual("canvas", target)
        self.assertEqual("Document1", info["name"])

    def test_write_only_editable_text_accepts_successful_operation(self):
        self.assertEqual(
            (True, False),
            MODULE.editable_write_result(True, "", "", "hello"),
        )

    def test_readable_editable_text_requires_verification(self):
        self.assertEqual(
            (False, False),
            MODULE.editable_write_result(True, "before", "before", "hello"),
        )


if __name__ == "__main__":
    unittest.main()
