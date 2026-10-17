"""只读核验完整 Mage 原包；加载不等于画面验收，不保存或重接任何蓝图。"""
import collections
import gc
import json
import math
import pathlib

import unreal


PROJECT = pathlib.Path(unreal.Paths.project_dir()).resolve()
CONTENT = PROJECT / "Content"
REPORT = PROJECT / "Saved/CombatAssetImport/MageFullAudit.json"
EXPECTED_PACKAGES = 1107
EXPECTED_SEQUENCE_COUNTS = {"Sequence1": 486, "Sequence2": 486}
EXPECTED_SKELETONS = {
    "Sequence1": "/Game/Mage/Demo/Mannequin_UE4/Character/Mesh/UE4_Mannequin_Skeleton.UE4_Mannequin_Skeleton",
    "Sequence2": "/Game/Mage/Demo/Mannequins/Meshes/SK_Mannequin.SK_Mannequin",
}
result = {
    "verified": False,
    "scope": "read-only load, animation data and recursive /Game package dependency checks",
    "not_verified": [
        "animation pose quality, transitions or gameplay suitability",
        "source animations retargeted to the project's Characters skeleton",
        "runtime animation, input, GAS or network behaviour",
        "rendering or Niagara appearance",
    ],
    "failures": [],
    "assets": [],
    "sequence_groups": {},
    "missing_package_dependencies": [],
    "characters_dependencies": [],
    "gc": "Python references released and Engine GC requested every 32 assets; reclaimed memory not measured",
}


def fail(stage, package, reason):
    record = {"stage": stage, "package": package, "reason": str(reason)}
    result["failures"].append(record)
    unreal.log_error("MAGE_FULL_AUDIT_FAILURE " + json.dumps(record, ensure_ascii=False))


def inspect_asset(data):
    # 本函数返回的都是普通值，不把已加载 UObject 留在整个审计的结果列表中。
    package = str(data.package_name)
    record = {"package": package, "registry_class": str(data.asset_class_path.asset_name), "loaded": False}
    try:
        asset = unreal.load_asset(package)
        if asset is None:
            raise RuntimeError("load_asset returned None")
        record["loaded"] = True
        record["loaded_class"] = asset.get_class().get_name()
        if record["loaded_class"] != record["registry_class"]:
            fail("class", package, "Loaded class does not match registry class")

        group = next((name for name in EXPECTED_SEQUENCE_COUNTS
                      if package.startswith("/Game/Mage/Animations/" + name + "/")), None)
        if group:
            if not isinstance(asset, unreal.AnimSequence):
                raise RuntimeError("Source sequence path did not load as AnimSequence")
            skeleton = asset.get_editor_property("skeleton")
            length = float(unreal.AnimationLibrary.get_sequence_length(asset))
            tracks = [str(name) for name in unreal.AnimationLibrary.get_animation_track_names(asset)]
            frames = int(unreal.AnimationLibrary.get_num_frames(asset))
            record.update({
                "sequence_group": group,
                "skeleton": skeleton.get_path_name() if skeleton else None,
                "length_seconds": length,
                "frames": frames,
                "bone_track_count": len(tracks),
                "root_motion": bool(unreal.AnimationLibrary.is_root_motion_enabled(asset)),
                "rate_scale": float(asset.get_editor_property("rate_scale")),
            })
            if not skeleton or skeleton.get_class().get_name() != "Skeleton":
                fail("animation", package, "Missing or invalid Skeleton")
            elif record["skeleton"] != EXPECTED_SKELETONS[group]:
                fail("animation", package, "Unexpected source Skeleton: " + record["skeleton"])
            if not math.isfinite(length) or length <= 0 or frames <= 0:
                fail("animation", package, "Animation has no positive finite duration/frame count")
            if not tracks or len(set(tracks)) != len(tracks):
                fail("animation", package, "Bone tracks are empty or contain duplicate names")

        # 后处理 AnimBP 仅检查加载出的类和骨架；不调用 CompileBlueprint，也不修改图。
        if isinstance(asset, unreal.AnimBlueprint):
            generated = asset.generated_class()
            target = asset.get_editor_property("target_skeleton")
            record["generated_class"] = generated.get_path_name() if generated else None
            record["target_skeleton"] = target.get_path_name() if target else None
            if not generated or not target:
                fail("anim_blueprint", package, "Missing generated class or target Skeleton")
        elif isinstance(asset, unreal.SkeletalMesh):
            skeleton = asset.get_editor_property("skeleton")
            record["skeleton"] = skeleton.get_path_name() if skeleton else None
            if not skeleton:
                fail("skeletal_mesh", package, "Missing Skeleton")
    except Exception as error:
        fail("load_or_inspect", package, error)
    return record


def audit_dependencies(registry, source_packages):
    options = unreal.AssetRegistryDependencyOptions(
        include_soft_package_references=True, include_hard_package_references=True)
    pending = list(source_packages)
    visited = set()
    missing = set()
    external = set()
    characters = set()
    while pending:
        package = pending.pop()
        if package in visited or not package.startswith("/Game/"):
            continue
        visited.add(package)
        # 检查真实包文件，而不是把 AssetRegistry 中可能过期的条目当成存在证明。
        relative = CONTENT / package[len("/Game/"):]
        if not any(relative.with_suffix(extension).is_file() for extension in [".uasset", ".umap"]):
            missing.add(package)
            continue
        if not package.startswith("/Game/Mage/"):
            external.add(package)
        if package.startswith("/Game/Characters/"):
            characters.add(package)
        try:
            pending.extend(str(item) for item in registry.get_dependencies(unreal.Name(package), options))
        except Exception as error:
            fail("dependency_query", package, error)
    result["dependency_package_count"] = len(visited)
    result["external_game_dependencies"] = sorted(external)
    result["characters_dependencies"] = sorted(characters)
    result["missing_package_dependencies"] = sorted(missing)
    for package in sorted(missing):
        fail("dependency_missing", package, "Referenced /Game package file is absent")


def run():
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous(["/Game/Mage"], True)
    data_by_package = {str(data.package_name): data
                       for data in registry.get_assets_by_path("/Game/Mage", recursive=True)}
    disk_packages = {
        "/Game/" + filename.relative_to(CONTENT).with_suffix("").as_posix()
        for filename in (CONTENT / "Mage").rglob("*")
        if filename.is_file() and filename.suffix.lower() in {".uasset", ".umap"}
    }
    result["expected_source_package_count"] = EXPECTED_PACKAGES
    result["disk_source_package_count"] = len(disk_packages)
    result["registry_source_package_count"] = len(data_by_package)
    result["unregistered_disk_packages"] = sorted(disk_packages - data_by_package.keys())
    result["registry_packages_without_files"] = sorted(data_by_package.keys() - disk_packages)
    if len(disk_packages) != EXPECTED_PACKAGES:
        fail("inventory", "/Game/Mage", "Expected 1107 source packages, found " + str(len(disk_packages)))
    for package in result["unregistered_disk_packages"]:
        fail("registry_missing", package, "Package file did not produce an AssetRegistry entry")
    for package in result["registry_packages_without_files"]:
        fail("registry_stale", package, "AssetRegistry entry has no source package file")

    for index, package in enumerate(sorted(data_by_package), 1):
        result["assets"].append(inspect_asset(data_by_package[package]))
        if index % 32 == 0:
            gc.collect()
            unreal.SystemLibrary.collect_garbage()
            unreal.log("MAGE_FULL_AUDIT_PROGRESS loaded=" + str(index) + "/" + str(len(data_by_package)))

    result["class_counts"] = dict(sorted(collections.Counter(
        record["registry_class"] for record in result["assets"]).items()))
    for name, expected in EXPECTED_SEQUENCE_COUNTS.items():
        records = [record for record in result["assets"] if record.get("sequence_group") == name]
        result["sequence_groups"][name] = {
            "expected": expected,
            "inspected": len(records),
            "skeleton_counts": dict(collections.Counter(record["skeleton"] for record in records)),
            "minimum_bone_track_count": min((record["bone_track_count"] for record in records), default=0),
        }
        if len(records) != expected:
            fail("animation_count", name, "Expected " + str(expected) + " inspected source animations, found " + str(len(records)))
    audit_dependencies(registry, disk_packages)
    result["verified"] = not result["failures"]


try:
    run()
except Exception as error:
    fail("audit_exception", "/Game/Mage", error)
finally:
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False), encoding="utf-8")
    unreal.log("MAGE_FULL_AUDIT_REPORT " + str(REPORT))

if not result["verified"]:
    raise RuntimeError("Mage full source audit failed; see " + str(REPORT))
unreal.log("MAGE_FULL_AUDIT_PASS packages=" + str(result["disk_source_package_count"])
           + " source_animations=972; visual preview and gameplay wiring NOT VERIFIED")
