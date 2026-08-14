# Asserts one node's parent in a saved scene: parent_of.py <scene.json> <id> <expected-parent>.
#
# Exists for the reparent case in viewport-check.bat: a byte difference proves an edit
# happened, and only this proves it was the reparent asked for -- findstr cannot see nesting.
import json
import sys

scene = json.load(open(sys.argv[1], encoding='utf-8'))
target = int(sys.argv[2])
expected = int(sys.argv[3])


def find_parent(node, parent):
    if node['id'] == target:
        return parent
    for child in node.get('children', []):
        found = find_parent(child, node['id'])
        if found is not None:
            return found
    return None


actual = find_parent(scene['root'], None)
print(f'parent of {target}: {actual} (expected {expected})')
sys.exit(0 if actual == expected else 1)
