"""恢复原埃及图布局并同步正式关卡特效；原图只读，目标图和配置先备份。"""
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import unreal

project = Path(unreal.Paths.project_dir()).resolve()
backup = project / 'Saved/CombatAssetImport' / ('BeforeOriginalLayoutRestore-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
targets = [project / 'Content/Combat/Maps/CoopEscape.umap', project / 'Content/Combat/Escape/DA_EscapeCombat.uasset']
original = project / 'Content/Stylized_Egypt/Maps/Stylized_Egypt_Demo.umap'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

protected = [path for path in (project / 'Content/Combat').rglob('*') if path.is_file() and path.suffix in ['.uasset', '.umap', '.uexp', '.ubulk'] and path not in targets]
original_companions = [path for path in original.parent.rglob('*') if path.is_file()]
protected.extend(original_companions)
before = {str(path): digest(path) for path in protected}
for target in targets + original_companions:
    destination = backup / target.relative_to(project)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(target, destination)
    if digest(destination) != digest(target):
        raise RuntimeError('Backup hash mismatch: ' + str(target))

def config_snapshot():
    config = unreal.load_asset('/Game/Combat/Escape/DA_EscapeCombat')
    result = {}
    for name in ['attack', 'defend', 'heal', 'area', 'dash']:
        rule = config.get_editor_property(name)
        result[name] = {field: rule.get_editor_property(field) for field in ['cost', 'cooldown', 'amount', 'range', 'duration', 'windup', 'recovery']}
    for name in ['defense_shield_amount', 'defense_damage_multiplier', 'burning_damage_per_tick', 'burning_duration', 'area_root_duration']:
        result[name] = config.get_editor_property(name)
    return result

def presentation_snapshot():
    config = unreal.load_asset('/Game/Combat/Escape/DA_EscapeCombat')
    result = {}
    for name in ['defense_feedback', 'heal_feedback', 'attack_feedback']:
        feedback = config.get_editor_property(name)
        result[name] = {}
        for field in ['effect', 'cascade_effect']:
            asset = feedback.get_editor_property(field)
            result[name][field] = asset.get_path_name() if asset else None
    return result

rules_before = config_snapshot()
vfx_before = presentation_snapshot()
files_before = {str(path) for path in (project / 'Content/Combat/Maps').rglob('*') if path.is_file()}
unreal.log('ESCAPE_FORMAL_VFX_BEFORE ' + json.dumps(vfx_before))
commandlet = unreal.CoopEscapeBuildCommandlet()
result = commandlet.main('-RestoreOriginalLayout')
unexpected = [path for path, value in before.items() if digest(Path(path)) != value]
rules_after = config_snapshot()
vfx_after = presentation_snapshot()
new_map_files = [str(path) for path in (project / 'Content/Combat/Maps').rglob('*') if path.is_file() and str(path) not in files_before]
report = dict(result=result, original_map_sha256=digest(original), original_file_preserved=not unexpected,
              unexpected_changed_assets=unexpected, gameplay_rules_preserved=rules_before == rules_after,
              before_vfx=vfx_before, after_vfx=vfx_after, new_map_files=new_map_files, backups=str(backup))
(backup / 'RestoreReport.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
if result != 0 or unexpected or rules_before != rules_after:
    raise RuntimeError('Layout restore verification failed; original backup and failure report retained')
if any(vfx_after['defense_feedback'].values()) or not vfx_after['heal_feedback']['effect']:
    raise RuntimeError('Formal Q/E VFX assignment is invalid')
unreal.log('ESCAPE_ORIGINAL_RESTORE_VERIFIED ' + json.dumps(report, ensure_ascii=False))
