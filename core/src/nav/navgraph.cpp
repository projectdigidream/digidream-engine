#include "dnp/nav/navgraph.hpp"

namespace dnp::nav {

using namespace dnp::geom;

const char* path_status_text(PathStatus s) noexcept {
    switch (s) {
        case PathStatus::Ok:          return "ok";
        case PathStatus::SameSector:  return "already in the destination sector";
        case PathStatus::BadSector:   return "sector index outside the map";
        case PathStatus::Unreachable: return "no walkable route";
        case PathStatus::PathTooLong: return "route does not fit in the caller's buffer";
        case PathStatus::GaveUp:      return "search gave up at its expansion limit";
    }
    return "unknown";
}

// --------------------------------------------------------------------- build

void NavGraph::clear() noexcept {
    node_count_ = 0;
    edge_count_ = 0;
    portal_count_ = 0;
}

void NavGraph::compute_node(const MapData& map, u32 s) noexcept {
    const Sector& sec = map.sector(s);

    // Centroid as the mean of the corners. Deliberately not the area centroid:
    // this is a routing waypoint, not a physical property, and the mean is
    // exact in integer arithmetic where the area weighting is a division per
    // triangle. For a convex-ish room they are within a hair of each other, and
    // for a room they are not, neither is "the right place to stand".
    i128 sx = 0;
    i128 sy = 0;
    for (u16 k = 0; k < sec.point_count; ++k) {
        const fx2& p = map.point(sec.first_point + k);
        sx += p.x.raw;
        sy += p.y.raw;
    }
    if (sec.point_count > 0) {
        nodes_[s].centroid = fx2{fx::from_raw(static_cast<i64>(sx / sec.point_count)),
                                 fx::from_raw(static_cast<i64>(sy / sec.point_count))};
    } else {
        nodes_[s].centroid = fx2::zero();
    }
}

bool NavGraph::build_edges(const MapData& map) noexcept {
    edge_count_ = 0;
    for (u32 s = 0; s < node_count_; ++s) {
        const Sector& sec = map.sector(s);
        nodes_[s].first_edge = edge_count_;
        nodes_[s].edge_count = 0;

        // Sides first, in side order, then this sector's portals in the order
        // they were linked. Fixed and reproducible: A* explores edges in array
        // order, and a route that depended on iteration order would differ
        // between two servers running the same map.
        for (u16 k = 0; k < sec.side_count; ++k) {
            const u32 si = sec.first_side + k;
            const Side& sd = map.side(si);
            if (sd.neighbor == NO_SECTOR) continue;
            if (static_cast<u32>(sd.neighbor) >= node_count_) continue;
            if (edge_count_ >= MAX_NAV_EDGES) return false;

            NavEdge& e = edges_[edge_count_++];
            e.to = static_cast<u32>(sd.neighbor);
            e.side = static_cast<i32>(si);
            e.width = distance(map.point(sd.a), map.point(sd.b));
            e.cost = distance(nodes_[s].centroid, nodes_[e.to].centroid);
            ++nodes_[s].edge_count;
        }

        // ADJACENCY THE MAP DOES NOT SPELL OUT.
        //
        // A side is `wall`, `neighbor N`, or `open`. Only `neighbor` names the
        // sector on the other side, and hand-authored maps increasingly do not
        // use it: maps/mylevel.map is five sectors, every edge `open`, and
        // under the neighbour-only rule its graph had ZERO edges. Every sector
        // an island, and nothing could path anywhere.
        //
        // `open` means the sector ENDS at that edge -- walk off and you fall.
        // What is on the other side is a question about the geometry, so ask
        // the geometry: step a hair past the edge and see which sector is
        // there. find_sector_at answers it through the broadphase grid, and it
        // is the same function the mover uses to decide where a body IS, so
        // nav cannot disagree with movement about what is next to what.
        //
        // WALLS ARE LEFT ALONE, deliberately. `wall` and `open` are both
        // explicit in the format precisely because the difference is invisible
        // in the geometry and total in play -- one is a barrier, the other is a
        // fall. Deriving a route through a wall because a room happens to be on
        // the far side would be the nav graph overruling the author.
        //
        // Whether a derived edge can actually be CROSSED is not decided here.
        // That stays where it was, in edge_is_open, asked of the same
        // sector_admits the mover asks -- so a three-unit drop off a platform
        // is walkable downward and not upward, and nothing here needs to know
        // that. An edge is a candidate; walkability is a query-time answer,
        // because a lift connects two rooms at one end of its travel and not
        // at the other.
        for (u16 k = 0; k < sec.side_count; ++k) {
            const u32 si = sec.first_side + k;
            const Side& sd = map.side(si);
            if (sd.neighbor != NO_SECTOR) continue;          // already has one
            if (!(sd.flags & side_flag::LEDGE)) continue;    // a wall stays a wall
            if (sd.flags & side_flag::BLOCKING) continue;

            const fx2 a = map.point(sd.a);
            const fx2 b = map.point(sd.b);
            const fx2 along{b.x - a.x, b.y - a.y};
            const fx len = length(along);
            if (len <= fx::from_frac(1, 1024)) continue;     // a degenerate side

            // The perpendicular. WHICH WAY IS OUT is not knowable from the
            // winding: core/src/geom/map.cpp is explicit that point order is
            // not normalised, and a map with inconsistent winding is meant to
            // render oddly rather than misbehave. So try both and discard the
            // one that lands back inside the sector we started in.
            const fx2 out{along.y / len, -(along.x / len)};
            const fx step = fx::from_frac(1, 64);
            const fx feet = sec.floor_z;

            // Three points along the side, because one long `open` edge can
            // run past two different sectors and the midpoint alone would find
            // whichever it happened to touch.
            const fx frac[3] = {fx::from_frac(1, 4), fx::from_frac(1, 2),
                                fx::from_frac(3, 4)};
            for (u32 f = 0; f < 3; ++f) {
                const fx2 at{a.x + along.x * frac[f], a.y + along.y * frac[f]};
                for (i32 sign = 1; sign >= -1; sign -= 2) {
                    const fx off = sign > 0 ? step : -step;
                    const fx2 probe{at.x + out.x * off, at.y + out.y * off};
                    const i32 found =
                        geom::find_sector_at(map, probe, feet, fx::zero());
                    if (found < 0 || static_cast<u32>(found) >= node_count_) continue;
                    if (static_cast<u32>(found) == s) continue;

                    // One edge per neighbouring sector. Explicit edges are NOT
                    // deduplicated -- two doorways between the same rooms are
                    // two real routes and the narrower one must not shadow the
                    // wider -- but a derived edge carries no such information,
                    // and three probes along one side would otherwise add the
                    // same sector three times.
                    bool already = false;
                    for (u32 q = 0; q < nodes_[s].edge_count; ++q) {
                        if (edges_[nodes_[s].first_edge + q].to ==
                            static_cast<u32>(found)) { already = true; break; }
                    }
                    if (already) continue;
                    if (edge_count_ >= MAX_NAV_EDGES) return false;

                    NavEdge& e = edges_[edge_count_++];
                    e.to = static_cast<u32>(found);
                    e.side = static_cast<i32>(si);
                    e.derived = true;
                    e.width = len;
                    e.cost = distance(nodes_[s].centroid, nodes_[e.to].centroid);
                    ++nodes_[s].edge_count;
                }
            }
        }

        for (u32 p = 0; p < portal_count_; ++p) {
            if (portals_[p].from != s) continue;
            if (edge_count_ >= MAX_NAV_EDGES) return false;

            NavEdge& e = edges_[edge_count_++];
            e.to = portals_[p].to;
            e.side = -1;
            // No geometric gate: a portal is an entity or a sector trigger, not
            // a doorway, so nothing about its width or the floor on the far
            // side decides whether a body fits through it. The width is left at
            // zero because edge_is_open() never reads it -- a large sentinel
            // here would look like a value someone had chosen.
            e.width = fx::zero();
            e.cost = fx::zero();
            ++nodes_[s].edge_count;
        }
    }
    return true;
}

bool NavGraph::build(const MapData& map) noexcept {
    if (map.sector_count > MAX_NAV_NODES) return false;
    node_count_ = map.sector_count;
    for (u32 s = 0; s < node_count_; ++s) compute_node(map, s);
    return build_edges(map);
}

bool NavGraph::rebuild_sector(const MapData& map, u32 s) noexcept {
    if (s >= node_count_ || map.sector_count != node_count_) return false;

    // The topology check the header promises. A commit that changed a sector's
    // side list would leave every later node's first_edge pointing at the wrong
    // place, and the graph would describe a map nobody authored.
    // COUNTED THE SAME WAY build_edges BUILDS THEM, derived edges included.
    // Counting only explicit neighbours reported a topology change on every
    // map that has an `open` side -- which is every hand-authored map -- and
    // rebuild_sector would refuse to do anything at all.
    const Sector& sec = map.sector(s);
    u32 expect = 0;
    for (u16 k = 0; k < sec.side_count; ++k) {
        const Side& sd = map.side(sec.first_side + k);
        if (sd.neighbor != NO_SECTOR && static_cast<u32>(sd.neighbor) < node_count_) ++expect;
    }
    for (u32 k = 0; k < nodes_[s].edge_count; ++k) {
        const NavEdge& e = edges_[nodes_[s].first_edge + k];
        if (!e.derived) continue;
        ++expect;
        // And it must still BE derivable: a side that has since acquired a
        // `neighbor`, or stopped being `open`, is a topology change even
        // though the count would come out the same.
        const Side& sd = map.side(static_cast<u32>(e.side));
        if (sd.neighbor != NO_SECTOR || !(sd.flags & side_flag::LEDGE)) return false;
    }
    for (u32 p = 0; p < portal_count_; ++p) if (portals_[p].from == s) ++expect;
    if (expect != nodes_[s].edge_count) return false;

    compute_node(map, s);

    // Refresh this sector's own edges, and the edges POINTING AT it. Moving a
    // sector's corners changes the cost of every route through it in both
    // directions, and refreshing only the outbound half leaves an asymmetric
    // graph in which A -> B and B -> A disagree about the distance.
    //
    // Walked node by node rather than straight down the edge array, so the
    // owning node of each edge is already known. Scanning for the owner instead
    // would make a "local" rebuild cost edges x nodes, which on a real map is
    // more work than the full build it exists to avoid.
    for (u32 n = 0; n < node_count_; ++n) {
        const NavNode& node = nodes_[n];
        for (u32 k = 0; k < node.edge_count; ++k) {
            NavEdge& e = edges_[node.first_edge + k];
            if (n != s && e.to != s) continue;
            if (e.side < 0) continue;   // a portal has no geometry to refresh

            const Side& sd = map.side(static_cast<u32>(e.side));
            e.width = distance(map.point(sd.a), map.point(sd.b));
            e.cost = distance(nodes_[n].centroid, nodes_[e.to].centroid);
        }
    }
    return true;
}

bool NavGraph::link_portal(const MapData& map, u32 from, u32 to) noexcept {
    if (portal_count_ >= MAX_NAV_PORTALS) return false;
    if (from >= map.sector_count || to >= map.sector_count) return false;
    portals_[portal_count_].from = from;
    portals_[portal_count_].to = to;
    ++portal_count_;
    // Folded into the edge array immediately, so a query never has to consider
    // two kinds of adjacency. A load-time operation -- see the header.
    return build(map);
}

bool NavGraph::edge_is_open(const MapData& map, u32 from, const NavEdge& e,
                            const NavBody& body, bool* needs_jump) const noexcept {
    if (needs_jump != nullptr) *needs_jump = false;
    if (e.to >= node_count_) return false;

    // A portal is not a doorway: no width, no floor, no headroom to check.
    if (e.side < 0) return true;

    const Side& sd = map.side(static_cast<u32>(e.side));
    if (sd.flags & side_flag::BLOCKING) return false;

    // A body has to fit through the gap, not merely be entitled to cross it.
    // The mover has no equivalent check because it resolves against the two
    // walls either side of a doorway and gets wedged; a path that routed
    // through a gap narrower than the body would send an enemy to press against
    // the frame forever, which reads as an AI bug.
    if (e.width < body.radius + body.radius) return false;

    // And the walkability rule itself, asked of the same function the mover
    // asks. Feet at the source sector's floor, because that is where a body
    // standing in it actually is.
    const fx feet = map.sector(from).floor_z;
    if (physics::sector_admits(map, e.to, feet, body.height)) return true;

    // WALKING SAID NO. Can it hop?
    //
    // Only the RISE is reconsidered. sector_admits refuses for three reasons --
    // the floor is too high, the space is too short to stand in, or the ceiling
    // is too low to cross under -- and a jump answers exactly one of them. A
    // crawlspace is still a crawlspace to something that can jump.
    if (body.jump_height <= fx::zero()) return false;

    const Sector& to = map.sector(e.to);
    if (to.floor_z - feet > sim::STEP_HEIGHT + body.jump_height) return false;
    // It still has to fit where it lands. Measured in the destination, because
    // that is where the body ends up standing -- the source sector's headroom
    // is the mover's problem and the body is already living with it.
    if (to.ceil_z - to.floor_z < body.height) return false;

    if (needs_jump != nullptr) *needs_jump = true;
    return true;
}

u64 NavGraph::hash() const noexcept {
    StateHasher h;
    h.u32v(node_count_);
    h.u32v(edge_count_);
    h.u32v(portal_count_);
    for (u32 i = 0; i < node_count_; ++i) {
        h.vec2(nodes_[i].centroid);
        h.u32v(nodes_[i].first_edge);
        h.u32v(nodes_[i].edge_count);
    }
    for (u32 i = 0; i < edge_count_; ++i) {
        h.u32v(edges_[i].to);
        h.i32v(edges_[i].side);
        h.fxv(edges_[i].width);
        h.fxv(edges_[i].cost);
        h.u32v(edges_[i].derived ? 1u : 0u);
    }
    return h.value();
}

// ------------------------------------------------------------------------ A*

namespace {

// Binary heap keyed on f, breaking ties toward the lower node index.
//
// The tie-break is not cosmetic. Two equally good routes are equally good only
// to the pathfinder; to a player watching two identical enemies take different
// corridors for no reason, they are a bug. Ordering by index makes the choice a
// function of the map rather than of the order the open set happened to fill.
bool cheaper(const NavQuery& q, u32 a, u32 b) noexcept {
    if (q.f[a].raw != q.f[b].raw) return q.f[a].raw < q.f[b].raw;
    return a < b;
}

void heap_push(NavQuery& q, u32 n) noexcept {
    if (q.heap_size >= sizeof(q.heap) / sizeof(q.heap[0])) return;
    u32 i = q.heap_size++;
    q.heap[i] = n;
    while (i > 0) {
        const u32 parent = (i - 1) / 2;
        if (!cheaper(q, q.heap[i], q.heap[parent])) break;
        const u32 t = q.heap[i]; q.heap[i] = q.heap[parent]; q.heap[parent] = t;
        i = parent;
    }
}

u32 heap_pop(NavQuery& q) noexcept {
    const u32 top = q.heap[0];
    q.heap[0] = q.heap[--q.heap_size];
    u32 i = 0;
    for (;;) {
        const u32 l = 2 * i + 1;
        const u32 r = l + 1;
        u32 best = i;
        if (l < q.heap_size && cheaper(q, q.heap[l], q.heap[best])) best = l;
        if (r < q.heap_size && cheaper(q, q.heap[r], q.heap[best])) best = r;
        if (best == i) break;
        const u32 t = q.heap[i]; q.heap[i] = q.heap[best]; q.heap[best] = t;
        i = best;
    }
    return top;
}

}  // namespace

PathResult find_path(const MapData& map, const NavGraph& g, NavQuery& q,
                     u32 from, u32 to, const NavBody& body,
                     u32* out_path, u32 max_path, bool* out_jump) noexcept {
    PathResult r;

    if (from >= g.node_count() || to >= g.node_count() || g.node_count() == 0) {
        r.status = PathStatus::BadSector;
        return r;
    }
    if (max_path == 0) {
        r.status = PathStatus::PathTooLong;
        return r;
    }
    if (from == to) {
        out_path[0] = from;
        if (out_jump != nullptr) out_jump[0] = false;
        r.status = PathStatus::SameSector;
        r.length = 1;
        return r;
    }

    // Portals make the straight-line heuristic inadmissible: a zero-cost edge
    // can cross the whole map, so the distance to the goal can be an
    // OVERestimate of the true remaining cost, and A* would then happily return
    // a route that is not the cheapest. Rather than quietly returning a
    // suboptimal path, drop to h = 0 -- uniform-cost search, which is A* with
    // an admissible heuristic and is simply slower, not wrong.
    const bool use_heuristic = g.portal_count() == 0;

    const u32 n = g.node_count();
    for (u32 i = 0; i < n; ++i) {
        q.g[i] = fx::max();
        q.f[i] = fx::max();
        q.came_from[i] = -1;
        q.came_jump[i] = 0;
        q.closed[i] = 0;
        q.opened[i] = 0;
    }
    q.heap_size = 0;

    const fx2 goal = g.node(to).centroid;
    const auto heuristic = [&](u32 s) {
        return use_heuristic ? distance(g.node(s).centroid, goal) : fx::zero();
    };

    q.g[from] = fx::zero();
    q.f[from] = heuristic(from);
    q.opened[from] = 1;
    heap_push(q, from);

    bool found = false;
    while (q.heap_size > 0) {
        const u32 cur = heap_pop(q);
        if (q.closed[cur]) continue;   // a stale duplicate; see the push below
        q.closed[cur] = 1;
        ++r.expanded;

        if (cur == to) { found = true; break; }
        if (q.max_expand != 0 && r.expanded >= q.max_expand) {
            r.status = PathStatus::GaveUp;
            return r;
        }

        const NavNode& node = g.node(cur);
        for (u32 k = 0; k < node.edge_count; ++k) {
            const NavEdge& e = g.edge(node.first_edge + k);
            if (q.closed[e.to]) continue;
            bool hop = false;
            if (!g.edge_is_open(map, cur, e, body, &hop)) continue;

            const fx tentative = q.g[cur] + e.cost;
            if (q.opened[e.to] && !(tentative < q.g[e.to])) continue;

            q.came_from[e.to] = static_cast<i32>(cur);
            // Recorded with the parent link, not with the node: whether a jump
            // is needed is a property of the EDGE taken to get here, and a
            // cheaper route found later replaces both together.
            q.came_jump[e.to] = hop ? 1 : 0;
            q.g[e.to] = tentative;
            q.f[e.to] = tentative + heuristic(e.to);
            q.opened[e.to] = 1;
            // Lazy decrease-key: push a duplicate and let the stale copy be
            // skipped when it surfaces. A real decrease-key needs a position
            // index per node, and the heap here is bounded anyway.
            heap_push(q, e.to);
        }
    }

    if (!found) {
        r.status = PathStatus::Unreachable;
        return r;
    }

    // Walk the parent chain back, then reverse in place. Counted first so a
    // route too long for the caller's buffer is reported rather than truncated
    // into a path that leads somewhere else.
    u32 len = 0;
    for (i32 at = static_cast<i32>(to); at >= 0; at = q.came_from[at]) {
        ++len;
        if (len > n) break;   // a cycle would mean the search is broken
    }
    if (len > max_path) {
        r.status = PathStatus::PathTooLong;
        r.length = len;
        return r;
    }

    u32 w = len;
    for (i32 at = static_cast<i32>(to); at >= 0; at = q.came_from[at]) {
        --w;
        out_path[w] = static_cast<u32>(at);
        // out_jump[i] is about the step INTO i, so it comes from i's own
        // parent link. Index 0 has no parent and is always false: nobody jumps
        // to arrive where they already are.
        if (out_jump != nullptr) out_jump[w] = w > 0 && q.came_jump[at] != 0;
    }

    r.status = PathStatus::Ok;
    r.length = len;
    r.cost = q.g[to];
    return r;
}

}  // namespace dnp::nav
