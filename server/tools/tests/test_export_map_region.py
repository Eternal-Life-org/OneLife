import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


TOOLS = Path(__file__).resolve().parents[1]
SERVER = TOOLS.parent
spec = importlib.util.spec_from_file_location("export_map_region", TOOLS / "export_map_region.py")
exporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exporter)
I = lambda n: struct.pack("<i", n)
Q = lambda x, y, s=0, b=0: struct.pack("<iiii", x, y, s, b)
XY = lambda x, y: struct.pack("<ii", x, y)
T = lambda n: struct.pack("<d", n)
X, Y = 16254, 5047


class ExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.oracle = Path(cls.build.name) / "lineardb3_fixture"
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise RuntimeError("C++ compiler required for native database verification")
        subprocess.run([compiler, "-std=c++11", "-I", str(SERVER),
                        str(Path(__file__).with_name("lineardb3_fixture.cpp")),
                        str(SERVER / "lineardb3.cpp"), "-o", str(cls.oracle)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.output = self.root / "region"
        self.rows = {
            "map.db": [(Q(X, Y), I(7233)),
                       (Q(X-400, Y-400), I(7221)), (Q(X+400, Y+400), I(0)),
                       (Q(X-401, Y), I(7243)), (Q(X, Y+401), I(7241)),
                       (Q(X, Y, 2), I(2)), (Q(X, Y, 3), I(-((7 << 17) | 200))),
                       (Q(X, Y, 2, 1), I(1)), (Q(X, Y, 3, 1), I((8 << 17) | 300)),
                       (Q(X, Y, -1, 1), I(1))],
            "mapTime.db": [(Q(X, Y, 1), T(1900000000.125)),
                           (Q(X, Y, 4, 1), T(0)), (Q(X-401, Y, 1), T(1))],
            "floor.db": [(XY(X, Y), I((9 << 17) | 100)), (XY(X, Y+401), I(1))],
            "floorTime.db": [(XY(X, Y), T(1900000100)), (XY(X-401, Y), T(1))],
            "biome.db": [(XY(X, Y), I(3)+I(2)+I(500000))],
            "lookTime.db": [(XY(X, Y), T(1800000000)), (XY(X+401, Y), T(1))],
            "persistentMap.db": [(Q(X, Y, 1), b"test-password".ljust(50, b"\0"))],
            "meta.db": [(I(i), ("metadata-%d" % i).encode().ljust(128, b"\0"))
                        for i in (7, 8, 9, 10)]}
        for name, _, key_size, value_size in exporter.DATABASES:
            self.native("write", self.source / name, key_size, value_size, self.rows[name])
        settings = self.source / "settings"
        settings.mkdir()
        (settings / "seedPepper.ini").write_bytes(b"fixture-pepper\n")
        (settings / "seedSpawnBoundingBox.ini").write_bytes(b"40000\n")
        (settings / "epochSeconds.ini").write_bytes(b"3600\n")
        (settings / "sharedGameServerSecret.ini").write_bytes(b"must-not-export\n")
        (self.source / "biomeRandSeed.txt").write_bytes(b"727 941\n")

    def native(self, mode, path, key_size, value_size, rows):
        expected = self.root / "expected.raw"
        expected.write_bytes(b"".join(k+v for k, v in rows))
        subprocess.run([str(self.oracle), mode, str(path), str(key_size), str(value_size),
                        str(expected)], check=True, capture_output=True)

    def export(self, live=False):
        return exporter.export_region(self.source, self.output, X, Y, 400, live)

    def selected(self, name):
        if name == "meta.db":
            return [(k, v) for k, v in self.rows[name] if struct.unpack("<i", k)[0] in (7, 8, 9)]
        return [(k, v) for k, v in self.rows[name]
                if abs(struct.unpack_from("<i", k)[0] - X) <= 400
                and abs(struct.unpack_from("<i", k, 4)[0] - Y) <= 400]

    def test_native_roundtrip_all_databases_and_source_read_only(self):
        original = {p: hashlib.sha256(p.read_bytes()).digest()
                    for p in self.source.rglob("*") if p.is_file()}
        for path in original:
            path.chmod(0o444)
        manifest = self.export()
        for path, digest in original.items():
            self.assertEqual(hashlib.sha256(path.read_bytes()).digest(), digest)
        for name, output, key_size, value_size in exporter.DATABASES:
            self.native("check", self.output / output, key_size, value_size, self.selected(name))
        self.assertEqual(manifest["bounds_inclusive"], [15854, 16654, 4647, 5447])
        self.assertFalse(manifest["live_best_effort"])
        self.assertEqual((self.output / "settings/seedPepper.ini").read_bytes(), b"fixture-pepper\n")
        self.assertFalse((self.output / "settings/sharedGameServerSecret.ini").exists())
        self.assertEqual((self.output / "settings/nextMetadataID.ini").read_text(), "9\n")

    def test_live_incomplete_tail_excluded_and_offline_rejected(self):
        with (self.source / "map.db").open("ab") as file:
            file.write(b"\x01\x02\x03")
        with self.assertRaises(exporter.ExportError):
            self.export()
        self.assertFalse(self.output.exists())
        manifest = self.export(live=True)
        self.assertEqual(manifest["databases"]["map.db"]["ignored_tail_bytes"], 3)
        self.native("check", self.output / "tmp.db", 16, 4, self.selected("map.db"))

    def mutate_during_scan(self, mutation, live):
        original = exporter.RegionReader.chunks
        mutated = False

        def chunks(reader):
            nonlocal mutated
            if reader.path.name == "map.db" and not mutated:
                mutated = True
                mutation(reader.path)
            yield from original(reader)

        with mock.patch.object(exporter.RegionReader, "chunks", chunks):
            return self.export(live)

    def test_live_appends_ignored_and_changes_reported(self):
        def append(path):
            with path.open("ab") as file:
                file.write(Q(X+1, Y) + I(7243))
        manifest = self.mutate_during_scan(append, True)
        self.assertTrue(manifest["databases"]["map.db"]["source_changed"])
        self.native("check", self.output / "tmp.db", 16, 4, self.selected("map.db"))

    def test_offline_concurrent_write_rejected_and_output_cleaned(self):
        def overwrite(path):
            with path.open("r+b") as file:
                file.seek(11+16)
                file.write(I(7243))
        with self.assertRaises(exporter.ExportError):
            self.mutate_during_scan(overwrite, False)
        self.assertFalse(self.output.exists())

    def test_live_source_truncation_rejected(self):
        with self.assertRaises(exporter.ExportError):
            self.mutate_during_scan(lambda p: p.write_bytes(b"Ld2"), True)
        self.assertFalse(self.output.exists())

    def test_live_replaced_source_rejected(self):
        def replace(path):
            replacement = path.with_suffix(".replacement")
            replacement.write_bytes(path.read_bytes())
            replacement.replace(path)
        with self.assertRaises(exporter.ExportError):
            self.mutate_during_scan(replace, True)
        self.assertFalse(self.output.exists())

    def test_existing_output_and_production_path_are_protected(self):
        self.output.mkdir()
        (self.output / "keep.txt").write_text("keep")
        with self.assertRaises(FileExistsError):
            self.export()
        self.assertEqual((self.output / "keep.txt").read_text(), "keep")
        with self.assertRaises(exporter.ExportError):
            exporter.export_region(self.source, self.source / "export", X, Y, 400)

    def test_missing_referenced_metadata_rejected(self):
        (self.source / "meta.db").unlink()
        with self.assertRaisesRegex(exporter.ExportError, "missing referenced metadata"):
            self.export()
        self.assertFalse(self.output.exists())

    def test_dummy_recall_filtered_and_negative_coordinates(self):
        path = self.source / "mapDummyRecall.txt"
        inside = "(%d,%d) u 7221 0\n(%d,%d) v 7222 1 [3 1]\n" % (X, Y, X-400, Y)
        path.write_text(inside + "(%d,%d) u 7221 1\n" % (X-401, Y))
        manifest = self.export()
        self.assertEqual((self.output / path.name).read_text(), inside)
        self.assertEqual(manifest["dummy_recall_exported_lines"], 2)

        shutil.rmtree(self.output)
        path.unlink()
        (self.source / "map.db").unlink()
        rows = [(Q(-101, -201), I(7233)), (Q(-99, -199, -1, 1), I(1)),
                (Q(-102, -200), I(7233))]
        self.native("write", self.source / "map.db", 16, 4, rows)
        exporter.export_region(self.source, self.output, -100, -200, 1)
        self.native("check", self.output / "tmp.db", 16, 4, rows[:2])

    def test_missing_optional_databases_created_with_native_readable_headers(self):
        for path in self.source.glob("*.db"):
            path.unlink()
        self.native("write", self.source / "map.db", 16, 4, [(Q(X, Y), I(7233))])
        manifest = self.export()
        for name, output, key_size, value_size in exporter.DATABASES[1:]:
            self.native("check", self.output / output, key_size, value_size, [])
        self.assertTrue(manifest["warnings"])

    def test_wrong_format_empty_region_and_cli_validation(self):
        path = self.source / "floor.db"
        path.write_bytes(b"Ldb" + b"\0" * 40)
        with self.assertRaises(exporter.ExportError):
            self.export()
        path.unlink()
        with self.assertRaisesRegex(exporter.ExportError, "no map records"):
            exporter.export_region(self.source, self.output, -20000, -20000, 1)
        result = subprocess.run([sys.executable, str(TOOLS / "export_map_region.py"),
                                 "--source-dir", str(self.source), "--output-dir", str(self.output),
                                 "--radius", "-1"], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
