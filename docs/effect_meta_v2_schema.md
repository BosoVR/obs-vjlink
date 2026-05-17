# VJLink Effect Metadata V2

Every effect metadata file uses `schema_version: 2`.

Required root fields:
- schema_version
- effect_id
- name
- category
- role
- performance_group
- quality_cost
- requires_input
- description
- controls
- audio_link

Allowed roles:
- generator
- filter
- postprocess
- overlay
- flash
- utility

Allowed quality_cost:
- low
- medium
- high
- extreme

Control fields:
- param
- label
- type
- default
- min
- max
- step
- group
- basic
- advanced
- audio_target
- save

Control groups:
- Main
- Transform
- AudioLink
- Color
- Texture/Input
- Glitch
- Performance
