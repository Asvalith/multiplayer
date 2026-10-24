import json
import unreal

# 只读导出原关卡的可玩区域和既有机关，不保存或修改原地图。
world = unreal.EditorLoadingAndSavingUtils.load_map('/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo')
if not world:
    raise RuntimeError('Original coop map could not be loaded')
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
selected = []
for actor in actors:
    name = actor.get_class().get_name()
    if any(token in name.lower() for token in ['playerstart', 'socket', 'key', 'plate', 'gate', 'platform', 'winarea', 'spawner', 'navmesh', 'boundsvolume']):
        location = actor.get_actor_location()
        selected.append({'label': actor.get_actor_label(), 'class': name,
                         'location': [location.x, location.y, location.z],
                         'rotation': str(actor.get_actor_rotation())})
unreal.log('COOP_ESCAPE_MAP_AUDIT ' + json.dumps({'count': len(actors), 'selected': selected}, ensure_ascii=False))
unreal.log('COOP_EDITOR_APIS ' + json.dumps({
    'level_library': [name for name in dir(unreal.EditorLevelLibrary) if any(token in name for token in ['build', 'save_current'])],
    'level_subsystem': [name for name in dir(unreal.LevelEditorSubsystem) if any(token in name for token in ['build', 'save_current'])],
    'navigation': [name for name in dir(unreal.NavigationSystemV1) if any(token in name for token in ['build', 'random', 'project_point'])],
    'volume': str(unreal.load_class(None, '/Script/NavigationSystem.NavMeshBoundsVolume')),
    'cube_builder': str(getattr(unreal, 'CubeBuilder', None)),
}, ensure_ascii=False))
