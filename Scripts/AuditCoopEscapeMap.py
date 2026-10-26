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
