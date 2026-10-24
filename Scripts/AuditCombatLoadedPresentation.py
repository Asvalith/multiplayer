"""只读核对当前数据资产、CDO、音效时长与递归包依赖；不编译或保存资产。"""
import json
import pathlib
import unreal

project = pathlib.Path(unreal.Paths.project_dir()).resolve()
if '-CombatPresentationValidateGraphs' in unreal.SystemLibrary.get_command_line():
    # 可选冷验由原生工具核实图连线、唯一 Slot 与实际 RHI 就绪；不保存任何资产。
    if unreal.CombatAssetBuildCommandlet().main('-Validate') != 0:
        raise RuntimeError('Cold graph / effect validation failed')
config = unreal.load_asset('/Game/Combat/DA_CombatConfig')
registry = unreal.AssetRegistryHelpers.get_asset_registry()
registry.scan_paths_synchronous(['/Game/Combat', '/Game/Pack_Effects_2'], True)
options = unreal.AssetRegistryDependencyOptions(include_soft_package_references=True, include_hard_package_references=True)
result = {'feedback': {}, 'characters': [], 'missing_package_dependencies': [], 'scope': 'read-only binding/dependency check; visual verification separate'}
packages = set()


def path(value):
    return value.get_path_name() if value else None


for field in ['attack_feedback', 'impact_feedback', 'heal_feedback', 'defense_feedback',
              'casting_feedback', 'area_feedback', 'stun_feedback', 'failure_feedback']:
    value = config.get_editor_property(field)
    effect = value.get_editor_property('effect')
    sound = value.get_editor_property('sound')
    record = {'effect': path(effect), 'sound': path(sound),
              'sound_duration': sound.get_editor_property('duration') if sound else None}
    result['feedback'][field] = record
    for resource in [effect, sound]:
        if resource:
            packages.add(resource.get_package().get_path_name())
projectile = config.get_editor_property('projectile_effect')
result['projectile_effect'] = path(projectile)
if projectile:
    packages.add(projectile.get_package().get_path_name())

for name in ['BP_CombatPlayer', 'BP_CombatEnemy']:
    asset = unreal.load_asset('/Game/Combat/Presentation/' + name)
    defaults = unreal.get_default_object(asset.generated_class())
    mesh = defaults.get_editor_property('mesh')
    result['characters'].append({
        'asset': path(asset), 'mesh': path(mesh.get_skeletal_mesh_asset()),
        'skeleton': path(mesh.get_skeletal_mesh_asset().get_editor_property('skeleton')),
        'anim_class': path(mesh.get_editor_property('anim_class')),
        'ability_system_class': defaults.get_editor_property('ability_system').get_class().get_name(),
        'config': path(defaults.get_editor_property('combat_config')),
        'montages': {field: path(defaults.get_editor_property(field))
                     for field in ['attack_montage', 'heal_montage', 'death_montage', 'area_montage']},
    })

todo = list(packages)
visited = set()
while todo:
    item = todo.pop()
    if item in visited or not item.startswith('/Game/'):
        continue
    visited.add(item)
    if not unreal.EditorAssetLibrary.does_asset_exist(item):
        result['missing_package_dependencies'].append(item)
        continue
    todo.extend(str(dep) for dep in registry.get_dependencies(unreal.Name(item), options))
result['dependency_package_count'] = len(visited)
result['dependency_packages'] = sorted(visited)
# 只列出本工具按原路径新增的特效目录，不触及用户原 ZIP、Mage 或其他历史资产。
effect_root = project / 'Content/Pack_Effects_2'
imported_effects = {'/Game/' + filename.relative_to(project / 'Content').with_suffix('').as_posix()
                    for filename in effect_root.rglob('*.uasset')}
result['imported_effect_package_count'] = len(imported_effects)
result['unused_imported_effects'] = sorted(imported_effects - visited)
result['unused_imported_effect_count'] = len(result['unused_imported_effects'])
result['verified'] = bool(projectile) and not result['missing_package_dependencies'] and all(
    entry['config'] == '/Game/Combat/DA_CombatConfig.DA_CombatConfig' and all(entry['montages'].values())
    and entry['ability_system_class'] == 'multiplayerCombatAbilitySystem'
    for entry in result['characters']) and all(
    value['sound_duration'] is None or value['sound_duration'] > 0 for value in result['feedback'].values())
report = project / 'Saved/CombatAssetImport/LoadedPresentationAudit.json'
report.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
unreal.log('COMBAT_LOADED_PRESENTATION_AUDIT ' + json.dumps(result, ensure_ascii=False))
if not result['verified']:
    raise RuntimeError('Loaded presentation assets did not pass the read-only audit')
