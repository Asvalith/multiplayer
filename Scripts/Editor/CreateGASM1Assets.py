"""Create/update the small editor-authored asset layer required by GAS M1."""

import unreal


ROOT = "/Game/GAS/M1"


def load_or_create(name, asset_class, factory):
    path = f"{ROOT}/{name}"
    existing = unreal.EditorAssetLibrary.load_asset(path)
    if existing:
        return existing
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, ROOT, asset_class, factory
    )
    if not asset:
        raise RuntimeError(f"Failed to create {path}")
    return asset


def tag(name):
    value = unreal.GameplayTag()
    value.import_text(f'(TagName="{name}")')
    return value


def input_key(name):
    value = unreal.Key()
    value.set_editor_property("key_name", name)
    return value


def ability_entry(ability_class, input_tag):
    return unreal.multiplayerAbilitySetAbility(
        ability=ability_class,
        ability_level=1,
        input_tag=tag(input_tag),
    )


def effect_entry(effect_class):
    return unreal.multiplayerAbilitySetEffect(
        effect=effect_class,
        effect_level=1.0,
    )


unreal.EditorAssetLibrary.make_directory(ROOT)

action_factory = unreal.InputAction_Factory()
damage_action = load_or_create("IA_GAS_Damage", unreal.InputAction, action_factory)
heal_action = load_or_create("IA_GAS_Heal", unreal.InputAction, unreal.InputAction_Factory())
immunity_action = load_or_create(
    "IA_GAS_Immunity", unreal.InputAction, unreal.InputAction_Factory()
)
for action in (damage_action, heal_action, immunity_action):
    action.set_editor_property("value_type", unreal.InputActionValueType.BOOLEAN)

mapping_context = load_or_create(
    "IMC_GAS_Abilities",
    unreal.InputMappingContext,
    unreal.InputMappingContext_Factory(),
)
mapping_context.unmap_all()
mapping_context.map_key(damage_action, input_key("LeftMouseButton"))
mapping_context.map_key(heal_action, input_key("Q"))
mapping_context.map_key(immunity_action, input_key("E"))

input_factory = unreal.DataAssetFactory()
input_factory.set_editor_property("data_asset_class", unreal.multiplayerInputConfig)
input_config = load_or_create(
    "DA_GAS_InputConfig", unreal.multiplayerInputConfig, input_factory
)
input_config.set_editor_property(
    "ability_input_actions",
    [
        unreal.multiplayerTaggedInputAction(
            input_action=damage_action,
            input_tag=tag("InputTag.Ability.Damage"),
        ),
        unreal.multiplayerTaggedInputAction(
            input_action=heal_action,
            input_tag=tag("InputTag.Ability.Heal"),
        ),
        unreal.multiplayerTaggedInputAction(
            input_action=immunity_action,
            input_tag=tag("InputTag.Ability.Immunity"),
        ),
    ],
)

ability_set_factory = unreal.DataAssetFactory()
ability_set_factory.set_editor_property("data_asset_class", unreal.multiplayerAbilitySet)
ability_set = load_or_create(
    "DA_GAS_DefaultAbilitySet", unreal.multiplayerAbilitySet, ability_set_factory
)
ability_set.set_editor_property(
    "granted_abilities",
    [
        ability_entry(
            unreal.multiplayerDamageAbility, "InputTag.Ability.Damage"
        ),
        ability_entry(unreal.multiplayerHealAbility, "InputTag.Ability.Heal"),
        ability_entry(
            unreal.multiplayerImmunityAbility, "InputTag.Ability.Immunity"
        ),
    ],
)
ability_set.set_editor_property(
    "granted_effects",
    [effect_entry(unreal.multiplayerInitStatsEffect)],
)

character_blueprint = unreal.EditorAssetLibrary.load_asset(
    "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"
)
if not character_blueprint:
    raise RuntimeError("BP_ThirdPersonCharacter was not found")

character_cdo = unreal.get_default_object(character_blueprint.generated_class())
character_cdo.set_editor_property("ability_input_config", input_config)
character_cdo.set_editor_property("ability_mapping_context", mapping_context)
character_cdo.set_editor_property("startup_ability_set", ability_set)
character_cdo.modify()
character_blueprint.modify()

for asset in (
    damage_action,
    heal_action,
    immunity_action,
    mapping_context,
    input_config,
    ability_set,
    character_blueprint,
):
    unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False)

unreal.log(
    "GAS_M1_ASSETS_READY: LMB=Damage Q=Heal E=Immunity; "
    "InputConfig and AbilitySet assigned to BP_ThirdPersonCharacter"
)
