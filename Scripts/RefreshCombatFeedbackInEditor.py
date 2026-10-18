"""只刷新技能表现副本、小兵受击反馈；先备份，不重存原 HUD 和关卡。"""
import datetime
import hashlib
import json
import pathlib
import shutil
import unreal


project = pathlib.Path(unreal.Paths.project_dir()).resolve()
targets = [project / ('Content/' + name + '.uasset') for name in [
    'Combat/Presentation/UI/WBP_ARPGCombatVisuals',
    'Combat/Escape/WBP_EscapeCombatVisuals',
    'Combat/DA_EnemyCombatConfig',
    'Combat/Presentation/Cues/GCN_Heal',
]]
if any(not path.is_file() for path in targets):
    raise RuntimeError('Existing combat presentation assets are required')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
backup_root = project / 'Saved/CombatAssetImport' / ('BeforeFeedbackHUD-' + stamp)
before = {}
backups = []
for asset in targets:
    for extension in ['.uasset', '.uexp', '.ubulk', '.uptnl']:
        source = asset.with_suffix(extension)
        if source.is_file():
            backup = backup_root / source.relative_to(project)
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, backup)
            before[str(source.relative_to(project))] = digest(source)
            if digest(source) != digest(backup):
                raise RuntimeError('Combat presentation backup verification failed')
            backups.append(str(backup))

# 保护本项目所有非目标表现资产与地图，不扫描数 GB 的第三方原素材。
protected = [path for path in (project / 'Content/Combat').rglob('*')
             if path.is_file() and path.suffix in ['.uasset', '.umap', '.uexp', '.ubulk', '.uptnl']
             and path not in targets]
protected.append(project / 'Content/Stylized_Egypt/Maps/Stylized_Egypt_Demo.umap')
protected_before = {str(path): digest(path) for path in protected if path.is_file()}
result = unreal.CombatAssetBuildCommandlet().main('-RefreshCombatFeedbackAndHUD')
changed = [path for path, checksum in protected_before.items()
           if not pathlib.Path(path).is_file() or digest(pathlib.Path(path)) != checksum]
report = {
    'result': result,
    'backups': backups,
    'before_sha256': before,
    'after_sha256': {str(path.relative_to(project)): digest(path) for path in targets},
    'unexpected_saved_assets': changed,
    'layout': {'vitals': 'lower-left', 'skills': 'bottom-center', 'slots': 5,
               'transparent_background': True, 'event_driven_logic_preserved': True},
    'runtime_visual_validation': False,
}
report_file = backup_root / 'RefreshReport.json'
report_file.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
unreal.log('COMBAT_FEEDBACK_REFRESH_REPORT ' + str(report_file))
if result != 0 or changed:
    raise RuntimeError('Feedback refresh failed or saved an unrelated asset; backups preserved')
