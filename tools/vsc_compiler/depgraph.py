"""Dependency graph builder — extracts max_ref/min_ref edges and topo-sorts."""

from typing import List, Tuple

from vsc_compiler.ast_nodes import RegistryAST


# edge: (src_driver, src_prop, tgt_driver, tgt_prop)
DepEdge = Tuple[str, str, str, str]


def build(registry: RegistryAST) -> List[DepEdge]:
    """Build topologically-sorted dependency edge list.

    Each edge (A→B) means "A depends on B" (A.max_ref → B).
    The returned list is sorted so that B comes before A.
    """
    adj: dict[tuple[str, str], list[tuple[str, str]]] = {}
    all_nodes: set[tuple[str, str]] = set()
    edges: List[DepEdge] = []

    for drv in registry.drivers:
        if drv.status != "active":
            continue
        for prop in drv.properties:
            src = (drv.name, prop.name)
            all_nodes.add(src)
            adj.setdefault(src, [])

            for ref_name in [prop.max_ref, prop.min_ref]:
                if ref_name:
                    tgt = (drv.name, ref_name)  # same-driver refs only
                    adj[src].append(tgt)
                    edges.append((drv.name, prop.name, drv.name, ref_name))

    # Kahn topological sort on the adjacency
    in_degree: dict[tuple[str, str], int] = {n: 0 for n in all_nodes}
    for src, targets in adj.items():
        for tgt in targets:
            in_degree[tgt] = in_degree.get(tgt, 0) + 1

    queue = [n for n in all_nodes if in_degree.get(n, 0) == 0]
    order: dict[tuple[str, str], int] = {}
    pos = 0
    while queue:
        u = queue.pop(0)
        order[u] = pos
        pos += 1
        for v in adj.get(u, []):
            in_degree[v] -= 1
            if in_degree[v] == 0:
                queue.append(v)

    # sort edges: target-first (lower order = must be evaluated first)
    edges.sort(key=lambda e: (order.get((e[2], e[3]), 0),
                               order.get((e[0], e[1]), 0)))

    return edges
