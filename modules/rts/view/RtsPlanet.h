// Planets in the view (docs/planets.md): the bodies of a map's `world` block as the view draws them,
// and float queries on a cube-sphere surface (height, normal, ray picking). Presentation only.
//
// Data source: loadPlanetWorld() reads the map JSON `world` block. It is the one place to switch to the
// simulation's bodies once the sim provides them (positions, orbits, per-face corners).
// Faces / cells / corners follow modules/rts/RtsSphere.h; corner directions come from CubeSphere
// (bit-identical on seams) converted to float once per body.
#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../../engine/data/Json.h"
#include "../../../engine/render/Math3D.h"
#include "../RtsSurface.h"

namespace rts::game {

struct PlanetBody {
    enum class Kind : uint8_t { Plane, Planet, Moon, Star };
    std::string id;
    Kind kind = Kind::Planet;
    float radius = 32;
    int faceSize = 0;                 // N; 0 = no surface (star)
    float seaLevel = -1e9f;           // ocean sphere radius offset (world units); -1e9 = no ocean
    float orbitAltitude = 8;          // shell of move.layer "orbit" above the radius
    int parent = -1;                  // orbit around this body; -1 = fixed at `pos`
    float orbitRadius = 0, period = 0, phase = 0;   // period seconds, phase turns
    render::float3 pos;               // fixed position (no orbit)
    render::float3 color{0.8f, 0.8f, 0.8f};          // star / far-away tint (visual.color)
    bool atmosphere = false;
    render::float3 atmoColor{0.45f, 0.65f, 1.0f};
    float atmoHeight = 0.06f;         // shell thickness as a fraction of the radius
    std::vector<int16_t> corners;     // 6 * (N+1)^2, 1/16 units, faces 0..5 row-major
    std::vector<uint8_t> flags;       // 6 * N * N cell flags (cellFlagsFromChar of the face rows)
    bool surface() const { return faceSize > 0 && kind != Kind::Star; }
    bool star() const { return kind == Kind::Star; }
};

struct PlanetWorld {
    std::vector<PlanetBody> bodies;
    int start = 0;
    // simulation surface (match maps): body centres come from Surface::bodyPosition, geometry from its corners
    std::shared_ptr<const Surface> sim;
    bool empty() const { return bodies.empty(); }
    int find(const std::string& id) const;
    // Body centre in world space at game time `seconds` (circular orbits, docs/planets.md §1).
    render::float3 center(int body, double seconds) const;
    int starOf(int body) const;       // the star lighting this body (first star), -1 none
};

// Reads map["world"]; empty world when the map has none. Problems go to `warnings`.
PlanetWorld loadPlanetWorld(const Json& map, std::vector<std::string>* warnings = nullptr);
// The world of a running match: bodies, orbits, sea level, corner heights (seam duplicates as the sim averaged
// them) and cell flags from the simulation's Surface / grid; only visual keys (atmosphere, colours, which body
// has an ocean) from `visual` (loadPlanetWorld of the same map; may be empty).
PlanetWorld planetWorldFromSim(std::shared_ptr<const Surface> surf, const std::vector<uint8_t>& gridFlags, const PlanetWorld& visual);

// Seam-aware box blur of a per-grid-cell byte field of a planet map (fog of war): every face is blurred as an
// (N + 2r)^2 image whose border comes from the neighbouring faces across the seams (Surface::offset; past a cube
// corner the nearest edge cell), separable and integer (same result everywhere), never across stacked face rows
// or into other bodies / void cells. The padding index table is built once per surface and radius.
class PlanetFieldBlur {
public:
    explicit PlanetFieldBlur(std::shared_ptr<const Surface> s) : S_(std::move(s)) {}
    void operator()(std::vector<uint8_t>& field, int radius);
private:
    std::shared_ptr<const Surface> S_;
    int radius_ = -1;
    std::vector<std::vector<int32_t>> pad_;   // per body: 6 faces x (N+2r)^2 grid cells
    std::vector<uint8_t> src_;
    std::vector<uint16_t> row_;
};

// Sim-space positions of planet maps: body b's things live around Surface::anchor(b) (RtsSurface.h).
inline render::float3 anchorF(int body) { return {float(Surface::kSphereOrigin + body * Surface::kBodySpacing), 0, 0}; }
inline bool onSphereF(render::float3 p) { return p.x >= float(Surface::kSphereOrigin - Surface::kBodySpacing / 2); }
inline int bodyAtF(render::float3 p) { return int(std::floor((p.x - float(Surface::kSphereOrigin - Surface::kBodySpacing / 2)) / float(Surface::kBodySpacing))); }

// Float cube-sphere surface of one body (body-local: origin = body centre).
class PlanetSurface {
public:
    void init(const PlanetBody& b);
    // New corner heights of the same body: heights everywhere, normals only inside the per-face corner
    // rects [x0, x1] x [y0, y1] (grown by one corner). rect[f].x1 < x0 = face unchanged.
    struct Rect { int x0 = 0, y0 = 0, x1 = -1, y1 = -1; };
    void updateHeights(const PlanetBody& b, const Rect (&rect)[6]);
    int faceSize() const { return n_; }
    float radius() const { return radius_; }
    float minHeight() const { return minH_; }
    float maxHeight() const { return maxH_; }
    // Corner data (face, i, j), i, j in 0..N: cached unit direction, height (duplicates averaged).
    int cornerIndex(int face, int i, int j) const { return (face * (n_ + 1) + j) * (n_ + 1) + i; }
    const render::float3& cornerDir(int face, int i, int j) const { return dirs_[size_t(cornerIndex(face, i, j))]; }
    float cornerHeight(int face, int i, int j) const { return heights_[size_t(cornerIndex(face, i, j))]; }
    render::float3 cornerPos(int face, int i, int j) const { return cornerDir(face, i, j) * (radius_ + cornerHeight(face, i, j)); }
    const render::float3& cornerNormal(int face, int i, int j) const { return normals_[size_t(cornerIndex(face, i, j))]; }
    int canonical(int face, int i, int j) const { return canon_[size_t(cornerIndex(face, i, j))]; }
    uint8_t cellFlags(int face, int x, int y) const;

    // Continuous maps (same tangent warp as RtsSphere, in float).
    render::float3 dirFromFace(int face, float a, float b) const;
    void faceFromDir(render::float3 d, int& face, float& a, float& b) const;
    // Surface along a direction (exactly the mesh triangles: diagonal (0,0)-(1,1) per cell).
    float heightAt(render::float3 dir) const;
    render::float3 surfacePoint(render::float3 dir) const;
    render::float3 normalAt(render::float3 dir) const;
    // Body-local ray (dir normalized) -> first hit with the terrain (or the ocean when `ocean`).
    bool raycast(render::float3 origin, render::float3 dir, render::float3& hit, float seaLevel = -1e9f) const;

private:
    int n_ = 0;
    float radius_ = 1, minH_ = 0, maxH_ = 0;
    std::vector<render::float3> dirs_, normals_;
    std::vector<float> heights_;
    std::vector<int> canon_;
    std::vector<uint8_t> flags_;
};

} // namespace rts::game
