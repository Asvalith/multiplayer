"""备份后显式更新技能布局及五个正式 WBP 表现图；不修改角色规则、配置或地图。"""
import datetime
import hashlib
import json
import pathlib
import shutil
import unreal


project = pathlib.Path(unreal.Paths.project_dir()).resolve()
filename = project / 'Content/Combat/Presentation/UI/WBP_CombatSkills.uasset'
if not filename.is_file():
    raise RuntimeError('Existing combat widget is required; use the initial asset builder first')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


# 与 -UpdateUI 的后置 Widget 更新一致；不能继续只允许旧技能布局单包写入。
allowed_assets = {
    filename,
    project / 'Content/Combat/Presentation/UI/WBP_ARPGCombatVisuals.uasset',
    project / 'Content/Combat/Escape/WBP_EscapeCombatVisuals.uasset',
    project / 'Content/Combat/Presentation/UI/WBP_CombatProgression.uasset',
    project / 'Content/Combat/Presentation/UI/WBP_ARPGEnemyAttributeBar.uasset',
    project / 'Content/Combat/Presentation/UI/WBP_ARPGEnemyHealth.uasset',
}
# 备份所有允许保存的既有包及附属文件，不把失败自动回滚成另一份未验证资产。
stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
backup_root = project / 'Saved/CombatAssetImport' / ('BeforeHUDUpdate-' + stamp)
backups = []
for asset_file in sorted(allowed_assets):
    for extension in ['.uasset', '.uexp', '.ubulk', '.uptnl']:
        source = asset_file.with_suffix(extension)
        if source.is_file():
            backup = backup_root / source.relative_to(project)
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, backup)
            if digest(source) != digest(backup):
                raise RuntimeError('Combat HUD backup verification failed')
            backups.append(str(backup))

# 专项更新不能连带重存其他表现资产或 GameMode。
protected = list((project / 'Content/Combat/Presentation').rglob('*.uasset'))
protected.extend((project / 'Content/Combat/Escape').rglob('*.uasset'))
protected.append(project / 'Content/Combat/BP_CombatGameMode.uasset')
before = {str(path): digest(path) for path in protected if path.is_file() and path not in allowed_assets}
allowed_before = {str(path): digest(path) if path.is_file() else None for path in sorted(allowed_assets)}
old_hash = digest(filename)
result = unreal.CombatAssetBuildCommandlet().main('-UpdateUI')
changed_others = [path for path, checksum in before.items()
                  if not pathlib.Path(path).is_file() or digest(pathlib.Path(path)) != checksum]
report = {
    'asset': '/Game/Combat/Presentation/UI/WBP_CombatSkills',
    'backups': backups,
    'before_sha256': old_hash,
    'after_sha256': digest(filename),
    'result': result,
    'unexpected_saved_assets': changed_others,
    'allowed_saved_assets': [str(path.relative_to(project)) for path in sorted(allowed_assets)],
    'allowed_before_sha256': allowed_before,
    'allowed_after_sha256': {str(path): digest(path) if path.is_file() else None for path in sorted(allowed_assets)},
    'layout_minimum_viewport': [900, 540],
    'design_reference_height': 1080,
    'design_scale': 2,
    'stats_design_size': [432, 176],
    'skill_bar_design_size': [720, 156],
    'feedback_design_size': [800, 64],
    'default_dpi_estimates': [
        {'viewport': [900, 540], 'scale': 0.4995, 'stats_pixels': [215.784, 87.912],
         'skill_bar_pixels': [359.64, 77.922], 'skill_name_font_pixels': 12.987},
        {'viewport': [1280, 720], 'scale': 0.666, 'stats_pixels': [287.712, 117.216],
         'skill_bar_pixels': [479.52, 103.896], 'skill_name_font_pixels': 17.316},
    ],
    'dpi_estimate_scope': 'legacy skill-layout reference and default ShortestSide curve only; not measured geometry of the five current widgets',
    'visual_validation': False,
}
report_file = backup_root / 'UpdateReport.json'
report_file.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
unreal.log('COMBAT_HUD_UPDATE ' + json.dumps(report, ensure_ascii=False))
if result != 0 or changed_others:
    raise RuntimeError('Combat HUD update failed or modified an unrelated asset; backup preserved')
