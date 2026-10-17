"""仅接入 CombatTest：备份并保存指定 GameMode 与唯一 EnemySpawner 的类配置。"""
import datetime
import json
import pathlib
import shutil
import unreal

project = pathlib.Path(unreal.Paths.project_dir()).resolve()
content = (project / 'Content').resolve()
verify_only = '-CombatIntegrationVerify' in unreal.SystemLibrary.get_command_line()
restore_original_hud = '-CombatRestoreOriginalHUD' in unreal.SystemLibrary.get_command_line()
replace_hud = '-CombatReplaceHUD' in unreal.SystemLibrary.get_command_line()
map_package = '/Game/Combat/Maps/CombatTest'
mode_package = '/Game/Combat/BP_CombatGameMode'
presentation = '/Game/Combat/Presentation/'


def package_file(package):
    if not package.startswith('/Game/'):
        raise RuntimeError('Only project assets may be saved: ' + package)
    filename = (content / (package[len('/Game/'):] + '.uasset')).resolve()
    if not filename.is_relative_to(content):
        raise RuntimeError('Package path escaped Content')
    return filename


def snapshot(defaults, spawner):
    return {
        'pawn': defaults.get_editor_property('default_pawn_class').get_path_name(),
        'hud': defaults.get_editor_property('hud_class').get_path_name(),
        'enemy': spawner.get_editor_property('enemy_class').get_path_name(),
        'spawner': spawner.get_path_name(),
        'spawner_package': spawner.get_package().get_path_name(),
    }


def run():
    world = unreal.EditorLoadingAndSavingUtils.load_map(str(content / 'Combat/Maps/CombatTest.umap'))
    if not world:
        raise RuntimeError('Could not load CombatTest')
    mode = unreal.load_asset(mode_package)
    expected_mode = mode.generated_class()
    actual_mode = world.get_world_settings().get_editor_property('default_game_mode')
    if actual_mode != expected_mode:
        raise RuntimeError('CombatTest does not use expected BP_CombatGameMode; map was not changed')

    # 只加载出生器，不加载或保存整个 WorldPartition 关卡。
    descriptions = unreal.WorldPartitionBlueprintLibrary.get_actor_descs()
    if isinstance(descriptions, tuple):
        descriptions = descriptions[-1]
    selected = [item for item in descriptions
                if item.get_editor_property('native_class').get_name() == 'multiplayerEnemySpawner']
    if len(selected) != 1:
        raise RuntimeError('Expected one EnemySpawner descriptor, found ' + str(len(selected)))
    unreal.WorldPartitionBlueprintLibrary.load_actors([selected[0].get_editor_property('guid')])
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    spawners = [actor for actor in actors if actor.get_class().get_name() == 'multiplayerEnemySpawner']
    if len(spawners) != 1:
        raise RuntimeError('Expected one loaded EnemySpawner, found ' + str(len(spawners)))
    spawner = spawners[0]
    defaults = unreal.get_default_object(expected_mode)
    before = snapshot(defaults, spawner)
    if not before['spawner_package'].startswith('/Game/__ExternalActors__/Combat/Maps/CombatTest/'):
        raise RuntimeError('Spawner is not in the permitted CombatTest external actor package')
    player = unreal.load_asset(presentation + 'BP_CombatPlayer').generated_class()
    enemy = unreal.load_asset(presentation + 'BP_CombatEnemy').generated_class()
    # 动画素材接入不能顺便覆盖用户界面；只有显式恢复或替换 HUD 时才改它。
    if restore_original_hud:
        hud = unreal.load_asset('/Game/Combat/UI/BP_CombatHUD').generated_class()
        original_widget = unreal.load_asset('/Game/Combat/UI/WBP_CombatHUD')
        if not original_widget or not original_widget.generated_class():
            raise RuntimeError('Original combat widget could not be loaded; nothing was saved')
    elif replace_hud:
        hud = unreal.load_asset(presentation + 'UI/BP_CombatHUD').generated_class()
    else:
        hud = defaults.get_editor_property('hud_class')

    backups = []
    if not verify_only:
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
        backup_root = project / 'Saved/CombatAssetImport' / (('BeforeHUDRestore-' if restore_original_hud else 'BeforeIntegration-') + stamp)
        packages = [mode_package] if restore_original_hud else [mode_package, before['spawner_package']]
        for package in packages:
            filename = package_file(package)
            if not filename.is_file():
                raise RuntimeError('Cannot back up missing asset: ' + str(filename))
            target = backup_root / filename.relative_to(project)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(filename, target)
            backups.append(str(target))
            for extension in ['.uexp', '.ubulk', '.uptnl']:
                sidecar = filename.with_suffix(extension)
                if sidecar.is_file():
                    shutil.copy2(sidecar, target.with_suffix(extension))
        mode.modify()
        defaults.modify()
        if not restore_original_hud:
            defaults.set_editor_property('default_pawn_class', player)
        defaults.set_editor_property('hud_class', hud)
        unreal.BlueprintEditorLibrary.compile_blueprint(mode)
        if not unreal.EditorAssetLibrary.save_loaded_asset(mode, only_if_is_dirty=False):
            raise RuntimeError('Could not save BP_CombatGameMode; backups preserved')
        if not restore_original_hud:
            spawner.modify()
            spawner.set_editor_property('enemy_class', enemy)
            if not unreal.EditorLoadingAndSavingUtils.save_packages([spawner.get_package()], only_dirty=False):
                raise RuntimeError('Could not save EnemySpawner; backups preserved')

    defaults = unreal.get_default_object(mode.generated_class())
    after = snapshot(defaults, spawner)
    expected = ([before['pawn'], hud.get_path_name(), before['enemy']] if restore_original_hud
                else [player.get_path_name(), hud.get_path_name(), enemy.get_path_name()])
    if [after['pawn'], after['hud'], after['enemy']] != expected:
        raise RuntimeError('Actual GameMode/Spawner bindings do not match new presentation assets')
    result = {'mode': 'restore_original_hud_verify' if verify_only and restore_original_hud else
              'restore_original_hud' if restore_original_hud else 'verify' if verify_only else 'apply',
              'map': map_package, 'before': before,
              'after': after, 'backups': backups, 'saved_map': False, 'verified': True}
    report_name = ('OriginalHUDRestoreVerify.json' if verify_only else 'OriginalHUDRestore.json') if restore_original_hud else ('IntegrationVerify.json' if verify_only else 'IntegrationApply.json')
    report = project / 'Saved/CombatAssetImport' / report_name
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
    unreal.log('COMBAT_PRESENTATION_INTEGRATED ' + json.dumps(result, ensure_ascii=False))


run()
