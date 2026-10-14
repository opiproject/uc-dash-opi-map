#!/usr/bin/env python3
"""Smoke test for tools/render_mapping.py."""

from __future__ import annotations

import unittest

import render_mapping


class RenderMappingTest(unittest.TestCase):
    def test_core_objects_present(self) -> None:
        objects = render_mapping.parse_objects(render_mapping.YAML_PATH.read_text())
        names = [obj["dash_object"] for obj in objects]
        self.assertEqual(names, ["ENI", "VNET", "Outbound Routing", "ACL"])

    def test_markdown_contains_opi_messages(self) -> None:
        objects = render_mapping.parse_objects(render_mapping.YAML_PATH.read_text())
        table = render_mapping.markdown_table(objects)
        self.assertIn("`Vnic`", table)
        self.assertIn("`Vpc`", table)
        self.assertIn("`RouteTable`", table)
        self.assertIn("`SecurityPolicy`", table)
        self.assertIn("saiexperimentaldasheni.h", table)
        self.assertIn("network/cloud/vpc.proto", table)


if __name__ == "__main__":
    unittest.main()
