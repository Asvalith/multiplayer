"""Back up HUD assets, then update only the two project skill-heading widgets.

The native editor command updates existing controls without rebuilding their
trees. This wrapper never uses Python reflection to access the WidgetTree.
"""
import datetime
import hashlib
import json
import pathlib
import shutil
import unreal


TARGETS = (
    '/Game/Combat/Presentation/UI/WBP_ARPGCombatVisuals',
    '/Game/Combat/Escape/WBP_EscapeCombatVisuals',
)
ORIGINAL_HUDS = (
    '/Game/Combat/UI/WBP_CombatHUD',
    '/Game/Combat/UI/BP_CombatHUD',
    '/Game/Combat/Escape/WBP_EscapeHUD',
    '/Game/Combat/Escape/BP_EscapeHUD',
)
EXTENSIONS = ('.uasset', '.uexp', '.ubulk', '.uptnl')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def update_skill_headings():
    project = pathlib.Path(unreal.Paths.project_dir()).resolve()
    asset_file = lambda package: project / ('Content/' + package.removeprefix('/Game/') + '.uasset')
    required = [asset_file(package) for package in TARGETS + ORIGINAL_HUDS]
    if any(not path.is_file() for path in required):
        raise RuntimeError('All target and original HUD assets must already exist')

    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    backup_root = project / 'Saved/CombatAssetImport' / ('BeforeSkillHeadingUpdate-' + stamp)
    backup_root.mkdir(parents=True, exist_ok=False)
    before = {}
    for asset in required:
        for extension in EXTENSIONS:
            source = asset.with_suffix(extension)
            if not source.is_file():
                continue
            backup = backup_root / source.relative_to(project)
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, backup)
            before[str(source.relative_to(project))] = digest(source)
            if digest(source) != digest(backup):
                raise RuntimeError('Skill heading backup checksum mismatch')

    allowed = {asset_file(package).with_suffix(extension)
               for package in TARGETS for extension in EXTENSIONS}
    protected = {str(path): digest(path) for path in (project / 'Content/Combat').rglob('*')
                 if path.is_file() and path.suffix in EXTENSIONS + ('.umap',) and path not in allowed}
    result = None
    failure = None
    try:
        result = unreal.CombatAssetBuildCommandlet().main('-UpdateSkillHeadings')
    except Exception as error:
        failure = repr(error)

    changed = [path for path, checksum in protected.items()
               if not pathlib.Path(path).is_file() or digest(pathlib.Path(path)) != checksum]
    created = [str(path) for path in (project / 'Content/Combat').rglob('*')
               if path.is_file() and path.suffix in EXTENSIONS + ('.umap',)
               and path not in allowed and str(path) not in protected]
    report = {
        'target_packages': TARGETS,
        'original_hud_packages': ORIGINAL_HUDS,
        'backup_root': str(backup_root),
        'before_sha256': before,
        'after_sha256': {str(path.relative_to(project)): digest(path)
                         for path in allowed if path.is_file()},
        'result': result,
        'exception': failure,
        'unexpected_saved_assets': changed,
        'unexpected_new_assets': created,
        'original_hud_and_other_assets_preserved': not changed and not created,
        'runtime_visual_validation': False,
    }
    report_path = backup_root / 'SkillHeadingUpdateReport.json'
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    unreal.log('COMBAT_SKILL_HEADING_UPDATE_REPORT ' + str(report_path))
    if result != 0 or failure or changed or created:
        raise RuntimeError('Skill heading update failed; backups and report preserved')
    return report


if __name__ == '__main__':
    update_skill_headings()
