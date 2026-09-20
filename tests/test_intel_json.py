#!/usr/bin/env python3
# Fixture documents the two shapes LiteMon's brace-stream parser must accept.
import json
sample = '''[
 {"period":{"duration":300},"frequency":{"actual":500},"power":{"GPU":1.2,"Package":8.4},"engines":{"Render/3D/0":{"busy":12.3},"Video/0":{"busy":2.0}}},
 {"period":{"duration":300},"frequency":{"actual":650},"power":{"GPU":2.4,"Package":10.1},"engines":{"Render/3D/0":{"busy":44.0},"Video/0":{"busy":8.0}}}
]'''
arr=json.loads(sample)
assert max(v['busy'] for v in arr[-1]['engines'].values()) == 44.0
assert arr[-1]['power']['GPU'] == 2.4
print('intel_gpu_top fixture: PASS')
