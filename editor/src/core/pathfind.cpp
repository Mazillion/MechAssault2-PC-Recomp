#include "pathfind.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace ma2 {

void NavGraph::parse(std::string_view text) {
    nodes.clear();
    enum { NONE, NODES, OTHER } sec = NONE;
    for (auto& raw : split(text, '\n')) {
        std::string line = raw;
        size_t c = line.find(';');
        if (c != std::string::npos) line.erase(c);
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == '[') { sec = istarts_with(line, "[Nodes]") ? NODES : OTHER; continue; }
        if (sec != NODES || line[0] != '=') continue;
        const char* q = line.c_str() + 1;
        double v[8];
        int k = 0;
        while (k < 8) {
            char* end;
            double x = strtod(q, &end);
            if (end == q) break;
            v[k++] = x;
            q = end;
        }
        if (k >= 6) {
            NavNode n;
            n.id = int(v[0]); n.type = int(v[1]); n.flags = int(v[2]);
            n.pos = {float(v[3]), float(v[4]), float(v[5])};
            n.owner = k >= 8 ? int(v[7]) : 0;
            nodes.push_back(std::move(n));
        } else if (k == 1 && !nodes.empty()) {
            nodes.back().links.push_back(int(v[0]));
        }
    }
}

std::string NavGraph::serialize() const {
    std::string s;
    char buf[256];
    snprintf(buf, sizeof buf, "[Params]\r\nNodes:\t  = %d\t\t\t\t\t\t\t\t\t\t\t\t; Number of nodes\r\n", int(nodes.size()));
    s += buf;
    s += "Quadrants: = 0\t\t\t\t\t\t\t\t\t\t\t\t\t; Number of quadrants\r\n\r\n[Nodes]\r\n";
    for (auto& n : nodes) {
        snprintf(buf, sizeof buf, " = %d %d %d %.6f %.6f %.6f %d %d\r\n", n.id, n.type, n.flags, n.pos.x, n.pos.y, n.pos.z,
                 int(n.links.size()), n.owner);
        s += buf;
        for (int l : n.links) { snprintf(buf, sizeof buf, " = %d\r\n", l); s += buf; }
    }
    s += "\r\n[Quadrants]\r\n\r\n";
    return s;
}

NavGraph empty_nav() { return NavGraph(); }

// One lattice at the given spacing. Links are 8-neighbour; the edge is sampled every few metres
// so long edges (wider spacing) still respect water, buildings and ridges.
static NavGraph lattice(const Terrain& t, const NavGenParams& p, float cell, const std::function<bool(float, float)>& blocked) {
    NavGraph g;
    int nx = int(std::floor(t.size_x() / cell)) + 1, nz = int(std::floor(t.size_z() / cell)) + 1;
    int stride = nz + 1;
    std::vector<int> index(size_t(nx) * nz, -1);
    float max_slope = std::cos(p.max_slope_deg * 3.14159265f / 180.f);
    float step_limit = p.max_step * cell / 20.f;  // same slope at any spacing
    for (int gx = 0; gx < nx; gx++)
        for (int gz = 0; gz < nz; gz++) {
            float x = gx * cell, z = gz * cell;
            if (x < p.edge_margin || z < p.edge_margin || x > t.size_x() - p.edge_margin || z > t.size_z() - p.edge_margin) continue;
            if (blocked && blocked(x, z)) continue;
            if (t.normal_at(x, z).y < max_slope) continue;
            NavNode n;
            n.id = gx * stride + gz + 1;
            n.type = 4;
            n.pos = {x, t.height_at(x, z) + p.lift, z};
            index[size_t(gx) * nz + gz] = int(g.nodes.size());
            g.nodes.push_back(std::move(n));
        }
    static const int DX[8] = {-1, -1, -1, 0, 0, 1, 1, 1}, DZ[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int gx = 0; gx < nx; gx++)
        for (int gz = 0; gz < nz; gz++) {
            int a = index[size_t(gx) * nz + gz];
            if (a < 0) continue;
            for (int k = 0; k < 8; k++) {
                int bx = gx + DX[k], bz = gz + DZ[k];
                if (bx < 0 || bz < 0 || bx >= nx || bz >= nz) continue;
                int b = index[size_t(bx) * nz + bz];
                if (b < 0) continue;
                const Vec3 &pa = g.nodes[a].pos, &pb = g.nodes[b].pos;
                if (std::fabs(pa.y - pb.y) > step_limit) continue;
                // Points along the edge must be walkable too (narrow ridges, water channels).
                float len = std::sqrt((pb.x - pa.x) * (pb.x - pa.x) + (pb.z - pa.z) * (pb.z - pa.z));
                int samples = std::max(1, int(len / 8.f));
                bool ok = true;
                for (int sidx = 1; sidx <= samples && ok; sidx++) {
                    float f = float(sidx) / float(samples + 1);
                    float mx = pa.x + (pb.x - pa.x) * f, mz = pa.z + (pb.z - pa.z) * f;
                    if (blocked && blocked(mx, mz)) ok = false;
                    float line = pa.y + (pb.y - pa.y) * f, ground = t.height_at(mx, mz) + p.lift;
                    if (std::fabs(ground - line) > std::max(3.f, step_limit * 0.75f)) ok = false;
                }
                if (ok) g.nodes[a].links.push_back(g.nodes[b].id);
            }
        }
    return g;
}

// Keeps the nodes reachable from the seed points (each seed snaps to its nearest node within
// 120 m); with no usable seed, the largest connected part.
static void keep_reachable(NavGraph& g, const std::vector<Vec3>& seeds) {
    std::unordered_map<int, int> idx;
    for (size_t i = 0; i < g.nodes.size(); i++) idx[g.nodes[i].id] = int(i);
    std::vector<int> comp(g.nodes.size(), -1);
    std::vector<int> sizes;
    for (size_t i = 0; i < g.nodes.size(); i++) {
        if (comp[i] >= 0) continue;
        int c = int(sizes.size());
        sizes.push_back(0);
        std::vector<int> stack{int(i)};
        comp[i] = c;
        while (!stack.empty()) {
            int n = stack.back();
            stack.pop_back();
            sizes[size_t(c)]++;
            for (int l : g.nodes[size_t(n)].links) {
                auto it = idx.find(l);
                if (it != idx.end() && comp[size_t(it->second)] < 0) { comp[size_t(it->second)] = c; stack.push_back(it->second); }
            }
        }
    }
    std::vector<char> keep_comp(sizes.size(), 0);
    bool any = false;
    for (const Vec3& s : seeds) {
        int best = -1;
        float bd = 120.f * 120.f;
        for (size_t i = 0; i < g.nodes.size(); i++) {
            if (g.nodes[i].links.empty()) continue;
            float dx = g.nodes[i].pos.x - s.x, dz = g.nodes[i].pos.z - s.z, d = dx * dx + dz * dz;
            if (d < bd) { bd = d; best = int(i); }
        }
        if (best >= 0) { keep_comp[size_t(comp[size_t(best)])] = 1; any = true; }
    }
    if (!any && !sizes.empty()) keep_comp[size_t(std::max_element(sizes.begin(), sizes.end()) - sizes.begin())] = 1;
    std::vector<NavNode> kept;
    for (size_t i = 0; i < g.nodes.size(); i++)
        if (keep_comp[size_t(comp[i])] && !g.nodes[i].links.empty()) kept.push_back(std::move(g.nodes[i]));
    g.nodes = std::move(kept);
}

NavGraph generate_nav(const Terrain& t, const NavGenParams& p, const std::function<bool(float, float)>& blocked) {
    NavGraph g;
    if (t.w < 2 || p.cell <= 0) return g;
    float cell = p.cell;
    for (int iter = 0; iter < 6; iter++) {
        g = lattice(t, p, cell, blocked);
        keep_reachable(g, p.seeds);
        if (p.max_nodes <= 0 || int(g.nodes.size()) <= p.max_nodes) break;
        // Widen the spacing so the reachable part fits the budget (count scales with 1/cell^2).
        cell *= std::sqrt(float(g.nodes.size()) / float(p.max_nodes)) * 1.04f;
    }
    return g;
}

}  // namespace ma2
