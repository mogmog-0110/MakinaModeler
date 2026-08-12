// Primitive signed distance functions, written once and included from both C++ and HLSL (D-03).
//
// Why scalar arguments instead of a vector type: HLSL has float3 and C++ does not, so a shared
// source either needs a hand-written C++ vector type or has to avoid vectors entirely. Avoiding
// them wins here for a second reason -- Grasp3D's SceneSdf.java is itself scalar, so this file
// stays a near line-by-line transcription of the reference implementation, which is what the
// Phase 1 agreement test compares against. A vectorised rewrite would be harder to audit against
// the original for exactly the cases where the two are most likely to drift.
//
// Precision: MK_FLOAT is double under C++ (matching Java's double) and float under HLSL. The two
// therefore do not agree bit for bit, and the agreement test has to carry a tolerance.
//
// Sign convention: negative inside, positive outside. Zero-thickness primitives (Disc, Triangle)
// have no interior and so never return a negative value -- see PatchSolid for how CSG copes.

#ifndef MAKINA_SDF_INCLUDED
#define MAKINA_SDF_INCLUDED

// MK_FN: every function here is a definition in a header, so under C++ it has to be inline or the
// second translation unit that includes this file collides with the first at link time. HLSL has
// no such keyword and needs nothing. This was not caught for a long time because until two
// consumers appeared in one binary, there was only ever one definition.
#ifdef __cplusplus
    #include <cmath>
    #define MK_FN inline
    #define MK_FLOAT double
    #define MK_SQRT(x)      std::sqrt(x)
    #define MK_ABS(x)       std::fabs(x)
    #define MK_MIN(a, b)    ((a) < (b) ? (a) : (b))
    #define MK_MAX(a, b)    ((a) > (b) ? (a) : (b))
    namespace makina {
#else
    #define MK_FN
    #define MK_FLOAT float
    #define MK_SQRT(x)      sqrt(x)
    #define MK_ABS(x)       abs(x)
    #define MK_MIN(a, b)    min(a, b)
    #define MK_MAX(a, b)    max(a, b)
#endif

// Stands in for "this subtree contributes no geometry". A finite sentinel rather than a true
// infinity: INF survives min/max on the CPU but is awkward on the GPU, and 1e30 is far beyond any
// scene scale. Anything at or above MK_EMPTY_THRESHOLD means empty.
#define MK_EMPTY 1.0e30
#define MK_EMPTY_THRESHOLD 1.0e29

MK_FN MK_FLOAT mkClamp(MK_FLOAT v, MK_FLOAT lo, MK_FLOAT hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------- canonical forms
//
// Two spellings of each shape. The canonical one is centered on the origin and takes only the
// dimensions that matter; the Grasp3D one takes the parameters the user authored and delegates.
//
// The CPU evaluator uses the authored spelling, because the reference implementation does and the
// agreement test compares against it. The flattener bakes the offset into the primitive's
// transform and emits the canonical spelling, which is what keeps a GPU node down to four
// parameters instead of six. Both call the same arithmetic, so they cannot drift.

MK_FN MK_FLOAT mkSdBoxCentered(MK_FLOAT x, MK_FLOAT y, MK_FLOAT z,
                         MK_FLOAT hx, MK_FLOAT hy, MK_FLOAT hz) {
    MK_FLOAT qx = MK_ABS(x) - hx;
    MK_FLOAT qy = MK_ABS(y) - hy;
    MK_FLOAT qz = MK_ABS(z) - hz;
    MK_FLOAT ox = MK_MAX(qx, 0.0), oy = MK_MAX(qy, 0.0), oz = MK_MAX(qz, 0.0);
    return MK_SQRT(ox * ox + oy * oy + oz * oz) + MK_MIN(MK_MAX(qx, MK_MAX(qy, qz)), 0.0);
}

/// Y-axis cylinder centered on the origin.
MK_FN MK_FLOAT mkSdCylinderCentered(MK_FLOAT x, MK_FLOAT y, MK_FLOAT z, MK_FLOAT r, MK_FLOAT halfHeight) {
    MK_FLOAT dx = MK_SQRT(x * x + z * z) - r;
    MK_FLOAT dy = MK_ABS(y) - halfHeight;
    MK_FLOAT ox = MK_MAX(dx, 0.0), oy = MK_MAX(dy, 0.0);
    return MK_MIN(MK_MAX(dx, dy), 0.0) + MK_SQRT(ox * ox + oy * oy);
}

/// Box from two opposite corners, the way Grasp3D authors it. The corners may be given in any
/// order; the half extents take the absolute difference.
MK_FN MK_FLOAT mkSdBox(MK_FLOAT x, MK_FLOAT y, MK_FLOAT z,
                 MK_FLOAT x1, MK_FLOAT y1, MK_FLOAT z1,
                 MK_FLOAT x2, MK_FLOAT y2, MK_FLOAT z2) {
    MK_FLOAT cx = (x1 + x2) * 0.5, cy = (y1 + y2) * 0.5, cz = (z1 + z2) * 0.5;
    MK_FLOAT hx = MK_ABS(x2 - x1) * 0.5, hy = MK_ABS(y2 - y1) * 0.5, hz = MK_ABS(z2 - z1) * 0.5;
    return mkSdBoxCentered(x - cx, y - cy, z - cz, hx, hy, hz);
}

MK_FN MK_FLOAT mkSdSphere(MK_FLOAT x, MK_FLOAT y, MK_FLOAT z, MK_FLOAT r) {
    return MK_SQRT(x * x + y * y + z * z) - r;
}

/// Y-axis cylinder spanning base..cap. Either bound may be the larger one.
MK_FN MK_FLOAT mkSdCylinder(MK_FLOAT x, MK_FLOAT y, MK_FLOAT z,
                      MK_FLOAT r, MK_FLOAT base, MK_FLOAT cap) {
    MK_FLOAT cy = (base + cap) * 0.5;
    MK_FLOAT hh = MK_ABS(cap - base) * 0.5;
    MK_FLOAT dx = MK_SQRT(x * x + z * z) - r;
    MK_FLOAT dy = MK_ABS(y - cy) - hh;
    MK_FLOAT ox = MK_MAX(dx, 0.0), oy = MK_MAX(dy, 0.0);
    return MK_MIN(MK_MAX(dx, dy), 0.0) + MK_SQRT(ox * ox + oy * oy);
}

/// Inigo Quilez's capped cone. q is (radial, y); y runs over [-h,h], bottom radius r1, top r2.
MK_FN MK_FLOAT mkSdCappedCone(MK_FLOAT qx, MK_FLOAT qy, MK_FLOAT h, MK_FLOAT r1, MK_FLOAT r2) {
    MK_FLOAT k1x = r2, k1y = h;
    MK_FLOAT k2x = r2 - r1, k2y = 2.0 * h;
    MK_FLOAT cax = qx - MK_MIN(qx, (qy < 0.0) ? r1 : r2);
    MK_FLOAT cay = MK_ABS(qy) - h;
    MK_FLOAT t = mkClamp(((k1x - qx) * k2x + (k1y - qy) * k2y) / (k2x * k2x + k2y * k2y), 0.0, 1.0);
    MK_FLOAT cbx = qx - k1x + k2x * t;
    MK_FLOAT cby = qy - k1y + k2y * t;
    MK_FLOAT s = (cbx < 0.0 && cay < 0.0) ? -1.0 : 1.0;
    return s * MK_SQRT(MK_MIN(cax * cax + cay * cay, cbx * cbx + cby * cby));
}

/// Y-axis cone centered on the origin: base radius r1 at y=-halfHeight, apex at y=+halfHeight.
MK_FN MK_FLOAT mkSdConeCentered(MK_FLOAT x, MK_FLOAT y, MK_FLOAT z, MK_FLOAT r1, MK_FLOAT halfHeight) {
    return mkSdCappedCone(MK_SQRT(x * x + z * z), y, halfHeight, r1, 0.0);
}

/// Grasp3D's Cone is a Y-axis cone: base radius r1 sitting at y=0, apex at y=height.
///
/// The element also carries X1/Y1/Z1/X2/Y2/Radius2/Open, but every consumer has them commented
/// out -- Cone.render() reads only Radius1 and Z2 and calls glutSolidCone(r1, z2) after rotating
/// -90 degrees about X. SceneSdf matches that, so those fields are dead data, not a second
/// endpoint. Reading them here would disagree with what Grasp3D actually draws.
///
/// A negative height is evaluated flipped, which is what the reference does.
MK_FN MK_FLOAT mkSdCone(MK_FLOAT x, MK_FLOAT y, MK_FLOAT z, MK_FLOAT r1, MK_FLOAT height) {
    if (height == 0.0) {
        return MK_EMPTY;
    }
    MK_FLOAT hh = MK_ABS(height) * 0.5;
    MK_FLOAT py = (height > 0.0 ? y - hh : y + hh) * (height > 0.0 ? 1.0 : -1.0);
    return mkSdCappedCone(MK_SQRT(x * x + z * z), py, hh, r1, 0.0);
}

MK_FN MK_FLOAT mkSdTorus(MK_FLOAT x, MK_FLOAT y, MK_FLOAT z, MK_FLOAT major, MK_FLOAT minor) {
    MK_FLOAT qx = MK_SQRT(x * x + z * z) - major;
    return MK_SQRT(qx * qx + y * y) - minor;
}

/// Ring on the XZ plane at y=0. No thickness, so no interior and never negative.
MK_FN MK_FLOAT mkSdDisc(MK_FLOAT x, MK_FLOAT y, MK_FLOAT z, MK_FLOAT r, MK_FLOAT hole) {
    MK_FLOAT rho = MK_SQRT(x * x + z * z);
    MK_FLOAT re = MK_MAX(MK_MAX(hole - rho, rho - r), 0.0);
    return MK_SQRT(re * re + y * y);
}

/// POV's plane{y, Y}: the inside is y <= h, i.e. the ground.
MK_FN MK_FLOAT mkSdPlane(MK_FLOAT y, MK_FLOAT h) {
    return y - h;
}

/// Inigo Quilez's udTriangle: unsigned distance from a point to a triangle. No interior.
MK_FN MK_FLOAT mkSdTriangle(MK_FLOAT px, MK_FLOAT py, MK_FLOAT pz,
                      MK_FLOAT ax, MK_FLOAT ay, MK_FLOAT az,
                      MK_FLOAT bx, MK_FLOAT by, MK_FLOAT bz,
                      MK_FLOAT cx, MK_FLOAT cy, MK_FLOAT cz) {
    MK_FLOAT bax = bx - ax, bay = by - ay, baz = bz - az;
    MK_FLOAT pax = px - ax, pay = py - ay, paz = pz - az;
    MK_FLOAT cbx = cx - bx, cby = cy - by, cbz = cz - bz;
    MK_FLOAT pbx = px - bx, pby = py - by, pbz = pz - bz;
    MK_FLOAT acx = ax - cx, acy = ay - cy, acz = az - cz;
    MK_FLOAT pcx = px - cx, pcy = py - cy, pcz = pz - cz;

    MK_FLOAT norx = bay * acz - baz * acy;
    MK_FLOAT nory = baz * acx - bax * acz;
    MK_FLOAT norz = bax * acy - bay * acx;

    MK_FLOAT c1x = bay * norz - baz * nory;
    MK_FLOAT c1y = baz * norx - bax * norz;
    MK_FLOAT c1z = bax * nory - bay * norx;
    MK_FLOAT c2x = cby * norz - cbz * nory;
    MK_FLOAT c2y = cbz * norx - cbx * norz;
    MK_FLOAT c2z = cbx * nory - cby * norx;
    MK_FLOAT c3x = acy * norz - acz * nory;
    MK_FLOAT c3y = acz * norx - acx * norz;
    MK_FLOAT c3z = acx * nory - acy * norx;

    MK_FLOAT s1 = c1x * pax + c1y * pay + c1z * paz;
    MK_FLOAT s2 = c2x * pbx + c2y * pby + c2z * pbz;
    MK_FLOAT s3 = c3x * pcx + c3y * pcy + c3z * pcz;
    // Two-way sign, matching SceneSdf.sign: exactly zero counts as positive. A three-way sign
    // would change which side of the `< 2.0` test a point exactly on an edge plane falls on.
    MK_FLOAT sgn = (s1 < 0.0 ? -1.0 : 1.0)
                 + (s2 < 0.0 ? -1.0 : 1.0)
                 + (s3 < 0.0 ? -1.0 : 1.0);

    if (sgn < 2.0) {
        // Outside the prism over the triangle: nearest point lies on an edge.
        MK_FLOAT baLen2 = bax * bax + bay * bay + baz * baz;
        MK_FLOAT cbLen2 = cbx * cbx + cby * cby + cbz * cbz;
        MK_FLOAT acLen2 = acx * acx + acy * acy + acz * acz;

        MK_FLOAT t1 = mkClamp((bax * pax + bay * pay + baz * paz) / baLen2, 0.0, 1.0);
        MK_FLOAT d1x = bax * t1 - pax, d1y = bay * t1 - pay, d1z = baz * t1 - paz;
        MK_FLOAT t2 = mkClamp((cbx * pbx + cby * pby + cbz * pbz) / cbLen2, 0.0, 1.0);
        MK_FLOAT d2x = cbx * t2 - pbx, d2y = cby * t2 - pby, d2z = cbz * t2 - pbz;
        MK_FLOAT t3 = mkClamp((acx * pcx + acy * pcy + acz * pcz) / acLen2, 0.0, 1.0);
        MK_FLOAT d3x = acx * t3 - pcx, d3y = acy * t3 - pcy, d3z = acz * t3 - pcz;

        MK_FLOAT m1 = d1x * d1x + d1y * d1y + d1z * d1z;
        MK_FLOAT m2 = d2x * d2x + d2y * d2y + d2z * d2z;
        MK_FLOAT m3 = d3x * d3x + d3y * d3y + d3z * d3z;
        return MK_SQRT(MK_MIN(m1, MK_MIN(m2, m3)));
    }

    MK_FLOAT dn = norx * pax + nory * pay + norz * paz;
    return MK_SQRT(dn * dn / (norx * norx + nory * nory + norz * norz));
}

#ifdef __cplusplus
    }  // namespace makina
#endif

#endif  // MAKINA_SDF_INCLUDED
