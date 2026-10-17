import datetime
import pathlib
import shutil
import unreal

# 已存在的新表现图更新前备份，回退不涉及原 Manny/Quinn、Skeleton 或关卡。
project = pathlib.Path(unreal.Paths.project_dir()).resolve()
stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
for name in ['ABP_CombatLocomotion', 'ABP_CombatQuinn']:
    filename = project / 'Content/Combat/Presentation/Animations' / (name + '.uasset')
    if filename.is_file():
        backup = project / 'Saved/CombatAssetImport' / ('BeforeSlotUpdate-' + stamp) / filename.relative_to(project)
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(filename, backup)

# 官方 IK 批量工具会刷新 Content Browser，必须在完整编辑器环境执行，而非无 Slate 的命令let。
result = unreal.CombatAssetBuildCommandlet().main('-Create')
unreal.log('COMBAT_EDITOR_ASSET_RESULT ' + str(result))
if result != 0:
    raise RuntimeError('Combat presentation asset creation/update failed')

# 让批量工具的 Content Browser/进度通知至少走过正常帧，再关闭编辑器。
ticks = 0
handle = None


def finish(delta_seconds):
    global ticks, handle
    ticks += 1
    if ticks >= 30:
        unreal.unregister_slate_post_tick_callback(handle)
        unreal.SystemLibrary.quit_editor()


if '-run=' not in unreal.SystemLibrary.get_command_line().lower():
    handle = unreal.register_slate_post_tick_callback(finish)
