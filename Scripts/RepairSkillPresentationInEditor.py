"""修复正式 HUD、防御姿态与技能特效项目副本；先备份，禁止改原 Widget、地图及第三方原件。"""
import datetime
import hashlib
import json
import pathlib
import shutil
import unreal


project = pathlib.Path(unreal.Paths.project_dir()).resolve()
vfx_only = '-vfxonly' in unreal.SystemLibrary.get_command_line().lower()
formal_animation_path = None
packages = ['/Game/Combat/DA_CombatConfig']
if not vfx_only:
    player = unreal.load_asset('/Game/Combat/Presentation/BP_CombatPlayer')
    if not player or not player.generated_class():
        raise RuntimeError('Formal combat player is required')
    defaults = unreal.get_default_object(player.generated_class())
    anim_class = defaults.get_editor_property('mesh').get_editor_property('anim_class')
    if not anim_class:
        raise RuntimeError('Formal combat player has no animation class')
    formal_animation_path = anim_class.get_path_name()
    animation_package = formal_animation_path.split('.')[0]
    if not animation_package.startswith('/Game/Combat/Presentation/'):
        raise RuntimeError('Refusing to modify a source-pack or non-project animation blueprint')
    packages += ['/Game/Combat/UI/BP_CombatHUD', '/Game/Combat/Presentation/BP_CombatPlayer', animation_package]
created_packages = [
    '/Game/Combat/Presentation/Effects/P_CombatFireProjectile',
    '/Game/Combat/Presentation/Effects/P_CombatFireCast',
    '/Game/Combat/Presentation/Effects/NS_CombatDefenseShield',
]
required_targets = [project / ('Content/' + name.removeprefix('/Game/') + '.uasset') for name in packages]
targets = required_targets + [project / ('Content/' + name.removeprefix('/Game/') + '.uasset') for name in created_packages]
if any(not path.is_file() for path in required_targets):
    raise RuntimeError('Every repair target must already exist')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def config_non_vfx_snapshot():
    config = unreal.load_asset('/Game/Combat/DA_CombatConfig')
    result = {}
    for name in ['attack', 'defend', 'heal', 'area', 'dash']:
        rule = config.get_editor_property(name)
        result[name] = {field: rule.get_editor_property(field)
                        for field in ['cost', 'cooldown', 'amount', 'range', 'duration', 'windup', 'recovery']}
    for name in ['parry_window', 'parry_front_dot', 'max_projectile_reflections', 'bleedout_duration',
                 'rescue_duration', 'rescue_range', 'area_radius', 'area_effect_reference_radius',
                 'projectile_speed', 'defense_damage_multiplier', 'defense_shield_amount',
                 'burning_damage_per_tick', 'burning_duration', 'area_root_duration']:
        result[name] = config.get_editor_property(name)
    marker = config.get_editor_property('area_marker_material')
    result['area_marker_material'] = marker.get_path_name() if marker else None
    for name in ['attack_feedback', 'impact_feedback', 'heal_feedback', 'defense_feedback',
                 'casting_feedback', 'area_feedback', 'stun_feedback', 'failure_feedback']:
        feedback = config.get_editor_property(name)
        for field in ['effect', 'cascade_effect', 'sound']:
            if name in ['attack_feedback', 'defense_feedback', 'heal_feedback'] and field != 'sound':
                continue
            value = feedback.get_editor_property(field)
            result[name + '.' + field] = value.get_path_name() if value else None
    return result


config_before = config_non_vfx_snapshot()
stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
backup_root = project / 'Saved/CombatAssetImport' / ('BeforeSkillPresentationRepair-' + stamp)
before = {}
allowed = set()
for asset in targets:
    for extension in ['.uasset', '.uexp', '.ubulk', '.uptnl']:
        source = asset.with_suffix(extension)
        allowed.add(source)
        if not source.is_file():
            continue
        backup = backup_root / source.relative_to(project)
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, backup)
        before[str(source.relative_to(project))] = digest(source)
        if digest(source) != digest(backup):
            raise RuntimeError('Repair backup checksum mismatch')

# 本项目全部非目标包均受保护；第三方原动画、骨架与材质只读，不触发 SaveAll。
protected = [path for path in (project / 'Content/Combat').rglob('*')
             if path.is_file() and path.suffix in ['.uasset', '.umap', '.uexp', '.ubulk', '.uptnl']
             and path not in allowed]
protected.append(project / 'Content/Stylized_Egypt/Maps/Stylized_Egypt_Demo.umap')
source_packages = [
    '/Game/AdvancedARPGCombat/MagicCombatSystem/DemoAssets/IggyVFX/Particles/Primary/P_IggyScorch_Projectile_Trail',
    '/Game/AdvancedARPGCombat/MagicCombatSystem/DemoAssets/IggyVFX/Particles/Primary/P_Fireball_Projectile_Cast',
    '/Game/Pack_Effects_2/Water_Magic/VFX_Niagara/NS_Water_Magic_Shield',
]
source_files = [project / ('Content/' + name.removeprefix('/Game/') + '.uasset') for name in source_packages]
if any(not path.is_file() for path in source_files):
    raise RuntimeError('Original VFX sources are required before calibrated project copies can be created')
for source in source_files:
    protected.extend(source.with_suffix(extension) for extension in ['.uasset', '.uexp', '.ubulk', '.uptnl']
                     if source.with_suffix(extension).is_file())
protected_before = {str(path): digest(path) for path in protected if path.is_file()}
audit_path = backup_root / 'InputBindingsBefore.txt'
result = None
failure = None
try:
    operation = '-RepairCombatVfx' if vfx_only else '-RepairSkillPresentation -RepairAuditPath="' + str(audit_path) + '"'
    result = unreal.CombatAssetBuildCommandlet().main(operation)
except Exception as error:
    failure = repr(error)
config_after = config_non_vfx_snapshot()
config_changes = [name for name in config_before if config_before[name] != config_after[name]]
changed = [path for path, checksum in protected_before.items()
           if not pathlib.Path(path).is_file() or digest(pathlib.Path(path)) != checksum]
new_protected = [str(path) for path in (project / 'Content/Combat').rglob('*')
                 if path.is_file() and path.suffix in ['.uasset', '.umap', '.uexp', '.ubulk', '.uptnl']
                 and path not in allowed and str(path) not in protected_before]
report = {
    'result': result,
    'mode': 'vfx_only' if vfx_only else 'skill_presentation',
    'exception': failure,
    'target_packages': packages,
    'calibrated_project_copies': created_packages,
    'original_source_sha256_before': {str(path.relative_to(project)): protected_before[str(path)] for path in source_files},
    'original_source_sha256_after': {str(path.relative_to(project)): digest(path) for path in source_files if path.is_file()},
    'config_non_vfx_before': config_before,
    'config_non_vfx_after': config_after,
    'unexpected_config_fields': config_changes,
    'formal_player_animation': formal_animation_path,
    'input_bindings_before': str(audit_path),
    'before_sha256': before,
    'after_sha256': {str(path.relative_to(project)): digest(path) for path in allowed if path.is_file()},
    'unexpected_saved_assets': changed,
    'unexpected_new_assets': new_protected,
    'original_widget_and_map_preserved': not changed and not new_protected,
    'runtime_visual_validation': False,
}
report_file = backup_root / 'RepairReport.json'
report_file.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
unreal.log('COMBAT_SKILL_PRESENTATION_REPAIR_REPORT ' + str(report_file))
if result != 0 or failure or changed or new_protected or config_changes:
    raise RuntimeError('Skill presentation repair failed; backups and report preserved')
