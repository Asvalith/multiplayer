"""备份并专项更新战斗动作：自然播放、上下半身分层、范围技能时序；不保存地图或原骨架。"""
import datetime
import hashlib
import json
import pathlib
import shutil

import unreal


project = pathlib.Path(unreal.Paths.project_dir()).resolve()
content = (project / "Content").resolve()
animation_folder = "/Game/Combat/Presentation/Animations/"
targets = [animation_folder + name for name in [
    "AM_Attack", "AM_Heal", "AM_Death", "AM_AreaCast", "AM_Hit",
    "ABP_CombatLocomotion", "ABP_CombatQuinn",
]] + ["/Game/Combat/DA_CombatConfig"]
protected = [
    "Characters/Mannequins/Meshes/SK_Mannequin.uasset",
    "Characters/Mannequins/Animations/ABP_Manny.uasset",
    "Characters/Mannequins/Animations/ABP_Quinn.uasset",
    "Combat/Maps/CombatTest.umap",
]
stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
backup_root = project / "Saved/CombatAssetImport" / ("BeforeAnimationUpdate-" + stamp)
manifest = {"targets": targets, "backups": [], "verified": False, "visual_preview": "NOT_RUN"}


def digest(filename):
    return hashlib.sha256(filename.read_bytes()).hexdigest()


def package_file(package):
    if package not in targets:
        raise RuntimeError("Package is outside the eight permitted animation/config assets: " + package)
    filename = (content / (package[len("/Game/"):] + ".uasset")).resolve()
    if not filename.is_relative_to(content) or not filename.is_file():
        raise RuntimeError("Cannot back up target asset: " + str(filename))
    return filename


def rules(config):
    fields = ["cost", "cooldown", "amount", "range", "duration", "windup", "recovery"]
    return {skill: {field: float(config.get_editor_property(skill).get_editor_property(field)) for field in fields}
            for skill in ["attack", "defend", "heal", "area"]}


def run():
    filenames = [package_file(package) for package in targets]
    protected_before = {name: digest(content / name) for name in protected}
    config = unreal.load_asset("/Game/Combat/DA_CombatConfig")
    if not config:
        raise RuntimeError("Combat config asset was not found")
    before_rules = rules(config)
    manifest["rules_before"] = before_rules
    manifest["protected_before"] = protected_before

    # 所有目标先完整备份成功，才允许原生工具保存任何一个资产；不碰原 Manny/Quinn、Skeleton 或地图。
    for filename in filenames:
        for candidate in [filename] + [filename.with_suffix(extension) for extension in [".uexp", ".ubulk", ".uptnl"]]:
            if not candidate.is_file():
                continue
            destination = backup_root / candidate.relative_to(project)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(candidate, destination)
            checksum = digest(candidate)
            if digest(destination) != checksum:
                raise RuntimeError("Backup hash mismatch: " + str(candidate))
            manifest["backups"].append({"source": str(candidate), "backup": str(destination), "sha256": checksum})
    manifest_path = backup_root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")

    result = unreal.CombatAssetBuildCommandlet().main("-UpdateAnimations")
    manifest["native_result"] = result
    if result != 0:
        raise RuntimeError("Combat animation update failed; exact asset backups are preserved at " + str(backup_root))
    after_rules = rules(config)
    manifest["rules_after"] = after_rules
    for skill, values in before_rules.items():
        for field, value in values.items():
            if skill == "area" and field in ["windup", "recovery"]:
                continue
            if after_rules[skill][field] != value:
                raise RuntimeError("Unrelated skill parameter changed: " + skill + "." + field)
    manifest["protected_after"] = {name: digest(content / name) for name in protected}
    if manifest["protected_after"] != protected_before:
        raise RuntimeError("A protected original asset or map changed")
    manifest["targets_after"] = {str(filename): digest(filename) for filename in filenames}
    manifest["verified"] = True
    unreal.log("COMBAT_ANIMATION_UPDATE " + json.dumps(manifest, ensure_ascii=False))


try:
    run()
except Exception as error:
    manifest["error"] = str(error)
    raise
finally:
    backup_root.mkdir(parents=True, exist_ok=True)
    (backup_root / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")

# 命令let自行退出；完整编辑器只用于离线专项更新，保存完成后正常关闭。
if "-run=" not in unreal.SystemLibrary.get_command_line().lower():
    unreal.SystemLibrary.quit_editor()
