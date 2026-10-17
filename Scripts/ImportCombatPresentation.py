"""在编辑器 Python 中导入占位音效，验证选定特效及其包依赖；不改地图。"""
import json
import pathlib
import unreal

project = pathlib.Path(unreal.Paths.project_dir())
source = project / 'Saved/CombatAssetImport/Audio'
audio_folder = '/Game/Combat/Presentation/Audio'
tasks = []
for filename in sorted(source.glob('*.wav')):
    if unreal.EditorAssetLibrary.does_asset_exist(audio_folder + '/' + filename.stem):
        continue
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', str(filename))
    task.set_editor_property('destination_path', audio_folder)
    task.set_editor_property('automated', True)
    task.set_editor_property('save', True)
    task.set_editor_property('replace_existing', False)
    tasks.append(task)
unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
for filename in sorted(source.glob('*.wav')):
    sound = unreal.load_asset(audio_folder + '/' + filename.stem)
    if sound:
        sound.set_sound_asset_compression_type(unreal.SoundAssetCompressionType.PCM)
        unreal.EditorAssetLibrary.save_loaded_asset(sound)

effects = {
    'Attack': '/Game/Pack_Effects_2/Water_Magic/VFX_Niagara/NS_Water_Magic_Projectile4',
    'Hit': '/Game/Pack_Effects_2/Water_Magic/VFX_Niagara/NS_Water_Magic_Splash2',
    'Defense': '/Game/Pack_Effects_2/Water_Magic/VFX_Niagara/NS_Water_Magic_Shield',
    'Heal': '/Game/Pack_Effects_2/Water_Magic/VFX_Niagara/NS_Water_Magic_Aura',
    'Area': '/Game/Pack_Effects_2/Poison_Magic/VFX_Niagara/NS_Posion_Magic_Area1',
    'AreaMarker': '/Game/Pack_Effects_2/Poison_Magic/VFX_Niagara/NS_Posion_Magic_Target',
    'Stun': '/Game/Pack_Effects_2/Poison_Magic/VFX_Niagara/NS_Posion_Magic_Debuff',
}
registry = unreal.AssetRegistryHelpers.get_asset_registry()
registry.scan_paths_synchronous(['/Game/Combat/Presentation', '/Game/Pack_Effects_2'], True)
result = {'effects': {}, 'sounds': [], 'missing_package_dependencies': [], 'scope': 'load/dependency validation; actual rendering not verified'}

# 原素材可能缺依赖：先列出包内候选，不能把可加载的 Niagara 外壳当作完整效果。
candidate_checks = {}
for asset_data in registry.get_assets_by_path('/Game/Pack_Effects_2', recursive=True):
    if str(asset_data.asset_class_path.asset_name) != 'NiagaraSystem':
        continue
    todo = [str(asset_data.package_name)]
    seen = set()
    missing = []
    while todo:
        item = todo.pop()
        if item in seen or not item.startswith('/Game/'):
            continue
        seen.add(item)
        if not unreal.EditorAssetLibrary.does_asset_exist(item):
            missing.append(item)
            continue
        todo.extend(str(dep) for dep in registry.get_dependencies(unreal.Name(item), unreal.AssetRegistryDependencyOptions(include_soft_package_references=True, include_hard_package_references=True)))
    candidate_checks[str(asset_data.package_name)] = sorted(missing)
result['candidate_dependencies'] = candidate_checks
visited = set()
pending = list(effects.values())
options = unreal.AssetRegistryDependencyOptions(include_soft_package_references=True, include_hard_package_references=True)
for name, package in effects.items():
    asset = unreal.load_asset(package)
    result['effects'][name] = {'path': package, 'class': asset.get_class().get_name() if asset else None}
while pending:
    package = pending.pop()
    if package in visited or not package.startswith('/Game/'):
        continue
    visited.add(package)
    if not unreal.EditorAssetLibrary.does_asset_exist(package):
        result['missing_package_dependencies'].append(package)
        continue
    pending.extend(str(item) for item in registry.get_dependencies(unreal.Name(package), options))
for filename in sorted(source.glob('*.wav')):
    asset = unreal.load_asset(audio_folder + '/' + filename.stem)
    result['sounds'].append({'path': audio_folder + '/' + filename.stem, 'class': asset.get_class().get_name() if asset else None,
                            'duration': asset.get_editor_property('duration') if asset else 0})
unreal.log('COMBAT_PRESENTATION_REPORT ' + json.dumps(result, ensure_ascii=False))
report = project / 'Saved/CombatAssetImport/PresentationReport.json'
report.parent.mkdir(parents=True, exist_ok=True)
report.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
if any(value['class'] != 'NiagaraSystem' for value in result['effects'].values()) or result['missing_package_dependencies'] or any(value['class'] != 'SoundWave' for value in result['sounds']):
    raise RuntimeError('Combat presentation import has missing assets or dependencies; see PresentationReport.json')
