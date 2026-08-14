"""Assign the existing victory UMG to the native victory presenter.

Restart is deliberately routed through UmultiplayerVictoryPresenterComponent:
the UMG keeps presentation/layout ownership while C++ owns the authoritative
client -> server restart request chain.
"""

import unreal


CHARACTER_PATH = "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"
VICTORY_WIDGET_PATH = "/Game/UI/winandquit"
RESTART_BUTTON_NAME = "\u91cd\u65b0\u5f00\u59cb"


character_blueprint = unreal.EditorAssetLibrary.load_asset(CHARACTER_PATH)
if not character_blueprint:
    raise RuntimeError(f"Character Blueprint was not found: {CHARACTER_PATH}")

victory_widget_blueprint = unreal.EditorAssetLibrary.load_asset(VICTORY_WIDGET_PATH)
if not victory_widget_blueprint:
    raise RuntimeError(f"Victory Widget Blueprint was not found: {VICTORY_WIDGET_PATH}")

# Compile first. Compiling after changing a Blueprint CDO can reconstruct the
# generated class and silently discard native default-subobject edits.
unreal.BlueprintEditorLibrary.compile_blueprint(victory_widget_blueprint)
unreal.BlueprintEditorLibrary.compile_blueprint(character_blueprint)

character_class = character_blueprint.generated_class()
victory_widget_class = victory_widget_blueprint.generated_class()
if not character_class or not victory_widget_class:
    raise RuntimeError("A configured Blueprint has no generated class")

character_cdo = unreal.get_default_object(character_class)
victory_presenters = character_cdo.get_components_by_class(
    unreal.multiplayerVictoryPresenterComponent
)
if len(victory_presenters) != 1:
    raise RuntimeError(
        "BP_ThirdPersonCharacter must contain exactly one VictoryPresenter component"
    )
victory_presenter = victory_presenters[0]
if not victory_presenter:
    raise RuntimeError("BP_ThirdPersonCharacter has no VictoryPresenter component")

victory_presenter.modify()
victory_presenter.set_editor_property("victory_widget_class", victory_widget_class)
victory_presenter.set_editor_property("restart_button_name", RESTART_BUTTON_NAME)
character_cdo.modify()
character_blueprint.modify()

if not unreal.EditorAssetLibrary.save_loaded_asset(
    character_blueprint, only_if_is_dirty=False
):
    raise RuntimeError("Failed to save BP_ThirdPersonCharacter")

configured_class = victory_presenter.get_editor_property("victory_widget_class")
configured_button = victory_presenter.get_editor_property("restart_button_name")
if configured_class != victory_widget_class:
    raise RuntimeError("VictoryWidgetClass did not persist on VictoryPresenter")
if str(configured_button) != RESTART_BUTTON_NAME:
    raise RuntimeError("RestartButtonName did not persist on VictoryPresenter")

unreal.log(
    "COOP_VICTORY_ASSET_READY: "
    f"WidgetClass={victory_widget_class.get_path_name()} "
    f"RestartButton={configured_button}"
)
