# Verified in game: unit models and boarding

- **Pink units.** A unit draws its template's model, chosen by the instance's ForceUseLowDetail and ForceUseHighDetail flags (code 0xD7584). Most levels ship only `_low` models, so set ForceUseLowDetail=1. A missing model is only one cause of the pink marker: any failed object build draws it (see the t27 article).
- **Boarding.** A unit is boardable with StartAbandoned=1, AttackPilot=0, OverrideEnable=0 and an MAOwnerController child (MoverTemplateUID = the unit's template). Enter it by tapping Q.

