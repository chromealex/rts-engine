// Surface of the battlefield: the flat plane of classic maps, or the cube-sphere surfaces of the bodies of
// a `world` (docs/planets.md). Integer / Fixed only.
//
// Sphere layout in the simulation
// - Every body with a surface gets 6 faces of N x N cells. The faces of all bodies are stacked as rows of
//   the one MapGrid: width W = max faceSize, body b occupies rows [rowBase, rowBase + 6N) (face f, row y
//   -> grid row rowBase + f*N + y). Cells x >= N of a smaller body are void (never passable, no neighbours).
//   Every per-cell layer of MapGrid (flags, levels, fields, vision, blocked, ...) works unchanged.
// - Positions are body-local, offset by the body's anchor: sim position = anchor(b) + local, anchor(b) =
//   (kSphereOrigin + b * kBodySpacing, 0, 0). Flat maps never reach x = kSphereOrigin - kBodySpacing/2
//   (kMaxMapSize = 2048), so a position tells by itself whether it lies on a planet (onSphere) and on which
//   body, and bodies are so far apart that nothing interacts across them (only transit moves between them).
//   World (render) position at time t = bodyPosition(b, t) + (pos - anchor(b)).
// - Surface point of a cell = dir * (radius + height); "up" = normalize(local).
#pragma once
#include "RtsSphere.h"
#include <memory>
#include <string>
#include <vector>

namespace rts::game {

enum class BodyKind : uint8_t { Plane, Planet, Moon, Star };

struct BodyDef {
    std::string id;
    BodyKind kind = BodyKind::Planet;
    Fixed radius = Fixed::fromInt(32);
    int faceSize = 0;                  // 0: no surface (stars)
    Fixed seaLevel{};
    Fixed orbitAltitude = Fixed::fromInt(10);   // shell of move.layer "orbit" = radius + orbitAltitude
    // circular orbit around `parent` (index, -1: fixed at `pos`), angle(t) = phase + t / period (turns)
    int parent = -1;
    Fixed orbitRadius{}, orbitPeriod{}, orbitPhase{};
    Vec3 pos{};
    // derived
    int rowBase = 0;
    size_t cornerBase = 0;
    bool hasSurface() const { return faceSize > 0; }
    Fixed shellRadius() const { return radius + orbitAltitude; }
};

struct SphereTables {                  // derived in build()
    std::vector<Vec3> dir;             // per grid cell: unit direction of the cell centre (zero: void)
    std::vector<int32_t> nb;           // per grid cell: 8 neighbours (Surface::kDx/kDy order), -1 none
    std::vector<int16_t> bodyOfRow;    // per grid row: body index
    std::vector<CubeSphere> cs;        // per body (faceSize 1 for bodies without a surface)
    std::vector<Fixed> cellSize;       // per body: mean cell edge in world units (2 pi r / 4N)
    // spatial hash buckets: a coarse cube-sphere per body (about 4 x 4 cells per bucket)
    std::vector<CubeSphere> coarse;
    std::vector<int32_t> coarseBase;   // per body, + total at the end
    std::vector<Fixed> coarseSize;     // per body: bucket edge in world units
};

class Surface {
public:
    static constexpr int kSphereOrigin = 8192;
    static constexpr int kBodySpacing = 4096;   // body radius + orbit altitude must stay below ~1500
    static constexpr int kMaxBodies = 16;
    static constexpr int64_t kSphereMinRaw = int64_t(kSphereOrigin - kBodySpacing / 2) << Fixed::kFracBits;
    static bool onSphere(Vec3 p) { return p.x.raw >= kSphereMinRaw; }
    static constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static constexpr int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

    std::vector<BodyDef> bodies;       // empty: flat map
    int startBody = 0;
    int W = 0, H = 0;                  // grid size (sphere)
    std::vector<int16_t> corners;      // per surface body: 6 faces x (N+1)^2, 1/16 units (all zero: smooth)
    std::shared_ptr<const SphereTables> tab;
    // Surface objects are immutable once built: GameState, MapGrid and SpatialHash share one through a
    // shared_ptr<const Surface> (snapshots copy the pointer, never the tables).

    bool sphere() const { return W > 0; }
    bool world() const { return !bodies.empty(); }
    // Builds the grid layout and the tables from `bodies` (faceSize, radius). Returns the grid size.
    void build();

    static Vec3 anchor(int b) { return {Fixed::fromInt(kSphereOrigin + int64_t(b) * kBodySpacing), Fixed{}, Fixed{}}; }
    int bodyAt(Vec3 p) const;
    Vec3 local(Vec3 p) const { return p - anchor(bodyAt(p)); }
    int bodyOfCell(int c) const { return tab->bodyOfRow[size_t(c / W)]; }
    const CubeSphere& cubeOf(int b) const { return tab->cs[size_t(b)]; }

    // cells (grid index = row * W + x)
    int gridCell(int body, int face, int x, int y) const { return (bodies[size_t(body)].rowBase + face * bodies[size_t(body)].faceSize + y) * W + x; }
    bool cellFace(int c, int& body, int& face, int& x, int& y) const;   // false: void cell
    int cellAt(Vec3 p) const;                                           // -1: body without a surface
    int neighbor(int c, int k) const { return tab->nb[size_t(c) * 8 + size_t(k)]; }
    int offset(int c, int dx, int dy) const;                            // across seams, -1 none
    Vec3 cellDir(int c) const { return tab->dir[size_t(c)]; }
    Vec3 cellCenter(int c) const;                                       // on the surface (sim space)
    Fixed cellSize(int body) const { return tab->cellSize[size_t(body)]; }

    // heights (above the radius) and geometry
    int16_t corner(int body, int face, int i, int j) const;
    Fixed cellHeight(int c) const;                                      // mean of the 4 corners
    Fixed heightAt(Vec3 p) const;                                       // bilinear on the face corners
    Vec3 up(Vec3 p) const;                                              // unit, away from the body centre
    Fixed altitude(Vec3 p) const;                                       // |local| - radius
    Vec3 project(Vec3 p, Fixed alt = {}) const;                         // onto the surface, + alt along up
    Vec3 toShell(Vec3 p, Fixed r) const;                                // onto the sphere of radius r
    // Distance ignoring altitude (the sphere analogue of the flat xz distance): chord between the two
    // directions on the body radius. Different bodies: 2^20.
    Fixed groundDist(Vec3 a, Vec3 b) const;
    // Local tangent frame at p (unit east/south of p's face, both orthogonal to up). Flat-style code works in
    // these planar coordinates: toPlane(p, v) = (v.e, 0, v.s), fromPlane(p, q) = e q.x + s q.z.
    void frame(Vec3 p, Vec3& east, Vec3& south) const;
    Vec3 toPlane(Vec3 p, Vec3 v) const;
    Vec3 fromPlane(Vec3 p, Vec3 q) const;
    Vec3 tangent(Vec3 p, Vec3 v) const;                                 // v minus its component along up(p)
    // spatial hash support
    int bucketOf(Vec3 p) const;
    void bucketsAround(Vec3 p, Fixed r, std::vector<int32_t>& out) const;   // sorted, unique
    int bucketCount() const { return tab ? tab->coarseBase.back() : 0; }

    // ---- for the view / editor (body-local, no anchor)
    Vec3 facePoint(int body, int face, Fixed a, Fixed b, bool withHeight = true) const;
    Vec3 faceNormal(int body, int face, Fixed a, Fixed b) const;        // unit normal of the height surface
    // Position of the body centre at `seconds` (world space; circular orbits, FixedMath).
    Vec3 bodyPosition(int body, Fixed seconds) const;
    int findBody(const std::string& id) const;
};

} // namespace rts::game
