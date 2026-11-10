#!/usr/bin/env python3
"""Read-only regional export of OneLife LINEARDB3 files (Python 3.8+)."""

import argparse
import contextlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import time


# lineardb3.cpp: 11-byte header, followed by fixed key/value records.
# dbCommon.cpp: coordinates, slot numbers and values are little-endian.
MAGIC = b"Ld2"
HEADER = struct.Struct("<3sII")
XY = struct.Struct("<ii")
INT = struct.Struct("<i")
QUAD = struct.Struct("<iiii")
DATABASES = (
    ("map.db", "tmp.db", 16, 4),
    ("mapTime.db", "mapTime.db", 16, 8),
    ("floor.db", "floor.db", 8, 4),
    ("floorTime.db", "floorTime.db", 8, 8),
    ("biome.db", "biome.db", 8, 12),
    ("lookTime.db", "lookTime.db", 8, 8),
    ("persistentMap.db", "persistentMap.db", 16, 50),
    ("meta.db", "meta.db", 4, 128),
)


class ExportError(Exception):
    pass


def signature(stat):
    return (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns,
            stat.st_ctime_ns)


class RegionReader:
    """Capture an initial complete-record prefix without building a hash table."""

    def __init__(self, path, key_size, value_size, live):
        self.path = path
        self.file = path.open("rb", buffering=0)
        try:
            self.initial = os.fstat(self.file.fileno())
            if self.initial.st_size < HEADER.size:
                raise ExportError("{}: incomplete database header".format(path))
            expected = HEADER.pack(MAGIC, key_size, value_size)
            header = self.file.read(HEADER.size)
            if header != expected:
                raise ExportError(
                    "{}: expected LINEARDB3 Ld2 header (key={}, value={}); "
                    "old LINEARDB2/KISSDB and big-endian headers are unsupported"
                    .format(path, key_size, value_size))
            self.record_size = key_size + value_size
            payload_size = self.initial.st_size - HEADER.size
            self.records, self.tail = divmod(payload_size, self.record_size)
            if self.tail and not live:
                raise ExportError("{}: incomplete tail ({} bytes); use --live "
                                  "only for an active server".format(path, self.tail))
        except Exception:
            self.file.close()
            raise

    def close(self):
        self.file.close()

    def chunks(self):
        remaining = self.records * self.record_size
        block_size = max(1, 2 * 1024 * 1024 // self.record_size) * self.record_size
        scanned = 0
        last_progress = time.monotonic()
        while remaining:
            target = min(block_size, remaining)
            block = bytearray()
            while len(block) < target:
                data = self.file.read(target - len(block))
                if not data:
                    raise ExportError("{}: file shortened during export".format(self.path))
                block.extend(data)
            yield block
            remaining -= target
            scanned += target
            if time.monotonic() - last_progress >= 2:
                print("{}: scanned {:.1f}/{:.1f} MiB".format(
                    self.path.name, scanned / 1048576,
                    self.records * self.record_size / 1048576), file=sys.stderr)
                last_progress = time.monotonic()

    def check_source(self, live):
        final = os.fstat(self.file.fileno())
        current = self.path.stat()
        if (current.st_dev, current.st_ino) != (self.initial.st_dev, self.initial.st_ino):
            raise ExportError("{}: source file was replaced".format(self.path))
        if final.st_size < self.initial.st_size:
            raise ExportError("{}: source file was truncated".format(self.path))
        changed = signature(final) != signature(self.initial)
        if changed and not live:
            raise ExportError("{}: source changed; use a filesystem snapshot "
                              "or --live for a best-effort export".format(self.path))
        return {"source_bytes_at_start": self.initial.st_size,
                "source_bytes_at_end": final.st_size,
                "source_records_at_start": self.records,
                "ignored_tail_bytes": self.tail,
                "source_changed": changed}


def collect_metadata(key, value, metadata_ids):
    _, _, slot, _ = QUAD.unpack(key)
    if slot == 0 or slot >= 3:
        # Negative contained IDs indicate subcontainers, not missing objects.
        object_id = abs(INT.unpack(value)[0])
        metadata_id = object_id >> 17
        if metadata_id:
            if metadata_id > 16383:
                raise ExportError("invalid packed object ID {}".format(object_id))
            metadata_ids.add(metadata_id)


def export_database(reader, destination, key_size, value_size, bounds,
                    metadata_ids, is_metadata=False, is_map=False, is_floor=False):
    kept = 0
    found_metadata = set()
    with destination.open("xb") as output:
        output.write(HEADER.pack(MAGIC, key_size, value_size))
        if reader is None:
            return kept, found_metadata
        size = key_size + value_size
        min_x, max_x, min_y, max_y = bounds
        for block in reader.chunks():
            view = memoryview(block)
            selected = bytearray()
            for offset in range(0, len(block), size):
                key = view[offset:offset + key_size]
                if is_metadata:
                    metadata_id = INT.unpack(key)[0]
                    include = metadata_id in metadata_ids
                else:
                    x, y = XY.unpack_from(key)
                    include = min_x <= x <= max_x and min_y <= y <= max_y
                if not include:
                    continue
                value = view[offset + key_size:offset + size]
                if is_map:
                    collect_metadata(key, value, metadata_ids)
                elif is_floor:
                    metadata_id = max(0, INT.unpack(value)[0]) >> 17
                    if metadata_id:
                        metadata_ids.add(metadata_id)
                if is_metadata:
                    found_metadata.add(metadata_id)
                selected.extend(view[offset:offset + size])
                kept += 1
            output.write(selected)
    return kept, found_metadata


def copy_small_file(source, destination, live):
    with source.open("rb") as file:
        before = os.fstat(file.fileno())
        data = file.read(1024 * 1024 + 1)
        after = os.fstat(file.fileno())
    if len(data) > 1024 * 1024:
        raise ExportError("{}: unexpectedly large configuration file".format(source))
    if signature(before) != signature(after) and not live:
        raise ExportError("{}: changed during export".format(source))
    with destination.open("xb") as output:
        output.write(data)


def copy_dummy_recall(source, destination, bounds, live):
    """Clean shutdowns save generated object indices in this companion file."""
    min_x, max_x, min_y, max_y = bounds
    kept = 0
    with source.open("rb", buffering=1024 * 1024) as file, destination.open("xb") as output:
        before = os.fstat(file.fileno())
        remaining = before.st_size
        while remaining:
            line = file.readline(min(4096, remaining))
            if not line or not line.endswith(b"\n"):
                raise ExportError("{}: truncated or oversized dummy recall line".format(source))
            remaining -= len(line)
            match = re.match(rb"\((-?\d+),(-?\d+)\) ", line)
            if not match:
                raise ExportError("{}: malformed dummy recall line".format(source))
            x, y = map(int, match.groups())
            if min_x <= x <= max_x and min_y <= y <= max_y:
                output.write(line)
                kept += 1
        after = os.fstat(file.fileno())
        current = source.stat()
    if (current.st_dev, current.st_ino) != (before.st_dev, before.st_ino) or after.st_size < before.st_size:
        raise ExportError("{}: dummy recall file replaced or truncated".format(source))
    if signature(before) != signature(after) and not live:
        raise ExportError("{}: dummy recall file changed".format(source))
    return kept


def export_region(source, destination, x, y, radius, live=False):
    source = source.resolve()
    destination = destination.resolve()
    if destination == source or source in destination.parents:
        raise ExportError("output directory must be outside the production directory")
    if radius < 0:
        raise ExportError("radius must be non-negative")
    bounds = (x - radius, x + radius, y - radius, y + radius)
    if min(bounds) < -(2**31) or max(bounds) >= 2**31:
        raise ExportError("coordinate bounds exceed signed 32-bit range")
    started = time.time()
    manifest = {"format": "OneLife LINEARDB3 regional export", "center": [x, y],
                "radius": radius, "bounds_inclusive": list(bounds),
                "started_at_unix": started, "live_best_effort": live,
                "databases": {}, "warnings": [], "copied_configuration": []}
    if live:
        manifest["warnings"].append(
            "Live files have no transactional snapshot: records and databases may "
            "reflect different moments; buffered, newly appended and in-memory "
            "server state are not fully captured.")
    created = False
    try:
        with contextlib.ExitStack() as stack:
            readers = {}
            # Capture every file's initial extent before starting the long scan.
            for name, _, key_size, value_size in DATABASES:
                path = source / name
                if path.exists():
                    reader = RegionReader(path, key_size, value_size, live)
                    stack.callback(reader.close)
                    readers[name] = reader
                elif name == "map.db":
                    raise ExportError("{} is missing".format(path))
                else:
                    readers[name] = None
                    manifest["warnings"].append(name + " missing; exported as an empty DB")
            destination.mkdir(mode=0o700, parents=True)
            created = True
            metadata_ids = set()
            for name, output_name, key_size, value_size in DATABASES:
                kept, found = export_database(
                    readers[name], destination / output_name, key_size, value_size,
                    bounds, metadata_ids, is_metadata=name == "meta.db",
                    is_map=name == "map.db", is_floor=name == "floor.db")
                manifest["databases"][name] = {"output": output_name,
                                               "exported_records": kept}
                print("{} -> {}: {} records".format(name, output_name, kept), file=sys.stderr)
                if name == "map.db" and not kept:
                    raise ExportError("no map records in the selected region; check coordinates")
                if name == "meta.db" and metadata_ids - found:
                    raise ExportError("missing referenced metadata IDs: {}".format(
                        sorted(metadata_ids - found)))
            # Check files after ALL database scans, not just after their own scan.
            for name, reader in readers.items():
                if reader:
                    manifest["databases"][name].update(reader.check_source(live))
            settings = destination / "settings"
            settings.mkdir(mode=0o700)
            for name in ("seedPepper.ini", "seedSpawnBoundingBox.ini", "epochSeconds.ini",
                         "biomeDensity.ini", "biomeOrder.ini", "biomeWeights.ini", "specialBiomes.ini"):
                path = source / "settings" / name
                if path.exists():
                    copy_small_file(path, settings / name, live)
                    manifest["copied_configuration"].append("settings/" + name)
                else:
                    manifest["warnings"].append("settings/" + name + " missing; server default applies")
            path = source / "biomeRandSeed.txt"
            if path.exists():
                copy_small_file(path, destination / path.name, live)
                manifest["copied_configuration"].append(path.name)
            else:
                manifest["warnings"].append("biomeRandSeed.txt missing; natural terrain will differ")
            path = source / "mapDummyRecall.txt"
            if path.exists():
                manifest["dummy_recall_exported_lines"] = copy_dummy_recall(
                    path, destination / path.name, bounds, live)
            # Test-only settings: ensure seeded Eve birth and retain the fixture.
            overrides = {"forceEveOnSeededSpawn.ini": "1\n",
                         "skipLookTimeCleanup.ini": "1\n",
                         "skipRemovedObjectCleanup.ini": "1\n",
                         "longTermNoLookCullEnabled.ini": "0\n",
                         "barrierOn.ini": "0\n", "useTestMap.ini": "0\n",
                         "specialMapPlacements.ini": "\n",
                         "nextMetadataID.ini": str(max(metadata_ids, default=0)) + "\n"}
            for name, value in overrides.items():
                (settings / name).write_text(value, encoding="utf-8")
            manifest["test_only_setting_overrides"] = overrides
            manifest["completed_at_unix"] = time.time()
            (destination / "manifest.json").write_text(
                json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    except BaseException:
        if created:
            shutil.rmtree(destination)
        raise
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, required=True,
                        help="server runtime directory containing map.db and settings/")
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="NEW directory outside source-dir; refuses existing directories")
    parser.add_argument("--center-x", type=int, default=16254)
    parser.add_argument("--center-y", type=int, default=5047)
    parser.add_argument("--radius", type=int, default=400, help="inclusive square half-width")
    parser.add_argument("--live", action="store_true",
                        help="allow concurrent updates/incomplete append tail (not an atomic snapshot)")
    args = parser.parse_args()
    if args.radius < 0:
        parser.error("radius must be non-negative")
    try:
        manifest = export_region(args.source_dir, args.output_dir,
                                 args.center_x, args.center_y, args.radius, args.live)
    except (ExportError, OSError) as error:
        print("Export failed: {}".format(error), file=sys.stderr)
        return 1
    for warning in manifest["warnings"]:
        print("WARNING: " + warning, file=sys.stderr)
    print("Export complete: {} (rename tmp.db to map.db on TEST server)".format(args.output_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
