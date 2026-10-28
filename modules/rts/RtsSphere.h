// Deterministic cube-sphere maths (docs/planets.md §2), shared by the simulation, the view and the terrain
// generator. Fixed-point / integer only: identical results on every platform.
//
// Conventions
// - Six faces, each N x N cells (N = faceSize). Cell id = face * N * N + y * N + x.
// - Face coordinates (a, b) in cell units, a, b in [0, N]; cell (x, y) covers [x, x+1) x [y, y+1).
// - s = 2a/N - 1 in [-1, 1], tangent warp s' = tan(s / 8 turn); face point = n + u s' + v t';
//   direction = normalize(face point). Basis table: kSphereFaces.
// - Corners: (N+1) x (N+1) per face, corner id = face * (N+1)^2 + j * (N+1) + i. Corners on face edges are
//   duplicated on 2 (edge) or 3 (cube corner) faces; canonicalCorner() picks the smallest id among them.
// - Internally seams are resolved on the integer "cube lattice": the cube [-N, N]^3, where a face point with
//   face coordinate a has lattice coordinate 2a - N (corners even offsets, cell centres odd offsets).
#pragma once
#include "engine/core/Fixed.h"
#include <cstdint>

namespace rts::game {

struct IVec3 {
    int x = 0, y = 0, z = 0;
    constexpr int dot(IVec3 o) const { return x * o.x + y * o.y + z * o.z; }
    constexpr bool operator==(const IVec3&) const = default;
};

struct SphereFace { IVec3 n, u, v; };
extern const SphereFace kSphereFaces[6];

struct SphereCell { int face = 0, x = 0, y = 0; };

// Angle helpers on unit vectors (turns).
inline const Fixed kFxTwoPi = Fixed::fromRaw(411775);         // 2*pi in Q16 (a fixed integer constant)
Fixed sphereArcTurns(Vec3 a, Vec3 b);                          // angle between two directions, [0, 0.5]
// Rotate unit direction `from` toward `to` along the great circle by at most `maxTurns`; returns the new
// direction (unit). `reached` set when `to` is within maxTurns.
Vec3 sphereRotateToward(Vec3 from, Vec3 to, Fixed maxTurns, bool* reached = nullptr);
// Unit tangent at `dir` pointing toward `to` (zero if parallel/antiparallel).
Vec3 sphereTangentToward(Vec3 dir, Vec3 to);

class CubeSphere {
public:
    explicit CubeSphere(int faceSize = 1) : n_(faceSize < 1 ? 1 : faceSize) {}
    int faceSize() const { return n_; }
    int cellCount() const { return 6 * n_ * n_; }
    int cornerCount() const { return 6 * (n_ + 1) * (n_ + 1); }

    int cellId(int face, int x, int y) const { return face * n_ * n_ + y * n_ + x; }
    SphereCell cell(int id) const { return {id / (n_ * n_), (id % (n_ * n_)) % n_, (id % (n_ * n_)) / n_}; }
    bool validCell(int face, int x, int y) const { return face >= 0 && face < 6 && x >= 0 && y >= 0 && x < n_ && y < n_; }

    // (face, a, b) in cell units -> unit direction from the body centre.
    Vec3 dirFromFace(int face, Fixed a, Fixed b) const;
    Vec3 cellCenterDir(int id) const;
    // Unit direction -> face and face coordinates (a, b in [0, N]). Ties between faces resolve to the lowest
    // axis (X, then Y, then Z), so the result is unique. Any non-zero vector works (no need to normalize);
    // zero vector -> face 0 centre.
    void faceFromDir(Vec3 dir, int& face, Fixed& a, Fixed& b) const;
    int cellFromDir(Vec3 dir) const;

    // 8-connected neighbours across face seams. Returns -1 where no cell exists (the diagonal through a cube
    // corner, where only three faces meet). Cube-corner cells therefore have 7 neighbours.
    int neighbor(int id, int dx, int dy) const;
    int neighbors8(int id, int out[8]) const;          // compacted list, returns count (7 or 8)

    int cornerId(int face, int i, int j) const { return face * (n_ + 1) * (n_ + 1) + j * (n_ + 1) + i; }
    // All ids of the corner (face, i, j) incl. itself, ascending; returns count (1, 2 or 3).
    int cornerDuplicates(int face, int i, int j, int out[3]) const;
    int canonicalCorner(int face, int i, int j) const { int o[3]; cornerDuplicates(face, i, j, o); return o[0]; }
    Vec3 cornerDir(int face, int i, int j) const { return dirFromFace(face, Fixed::fromInt(i), Fixed::fromInt(j)); }

    // Unit tangents of the face grid at a direction: east = +a, south = +b (projected onto the tangent plane).
    static void faceTangents(int face, Vec3 dir, Vec3& east, Vec3& south);

private:
    int n_;
};

} // namespace rts::game
