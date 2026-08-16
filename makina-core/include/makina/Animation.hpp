// Motion as a function from time to a still scene (PLAN.md D-15).
//
// The one place that knows what time is. `sampleAt(scene, t)` returns a copy of the scene with
// every animated parameter replaced by its value at t; the copy is an ordinary Scene, so the
// evaluator, the flattener, the exporters and every gate work on it unchanged. Nothing else in
// makina-core takes a time argument, which is what keeps rewind, round trip and the comparisons
// -- all written for still scenes -- honest when a scene has tracks.
//
// Interpolation is linear or Catmull-Rom (through every key, tangents from the neighbours, the
// end tangents one-sided), held flat outside the first and last key. Both are what a person
// expects a keyframe to do; anything richer (eases, IK) is a later stage and would live here.

#pragma once

#include "Scene.hpp"

#include <cstdint>

namespace makina {

namespace detail {

/// The track's value at t, by its own interpolation rule.
inline float trackValueAt(const Track& tr, float t) {
    if (tr.keyCount == 0) {
        return 0.0f;
    }
    if (tr.keyCount == 1 || t <= tr.time[0]) {
        return tr.value[0];
    }
    const int last = tr.keyCount - 1;
    if (t >= tr.time[last]) {
        return tr.value[last];
    }
    int i = 0;
    while (i + 1 < last && tr.time[i + 1] <= t) {
        ++i;
    }
    const float t0 = tr.time[i], t1 = tr.time[i + 1];
    const float span = t1 - t0;
    const float u = span > 0.0f ? (t - t0) / span : 0.0f;
    const float v0 = tr.value[i], v1 = tr.value[i + 1];
    if (static_cast<TrackInterp>(tr.interp) != TrackInterp::CatmullRom) {
        return v0 + (v1 - v0) * u;
    }
    // Catmull-Rom on possibly uneven key spacing: tangents as centred differences in value per
    // unit of time, one-sided at the ends, then a cubic Hermite over this span. Through every
    // key by construction; the tangent at a key is what makes it C1 across the key.
    const float vPrev = i > 0 ? tr.value[i - 1] : v0;
    const float tPrev = i > 0 ? tr.time[i - 1] : t0 - span;
    const float vNext = i + 2 <= last ? tr.value[i + 2] : v1;
    const float tNext = i + 2 <= last ? tr.time[i + 2] : t1 + span;
    const float m0 = (v1 - vPrev) / (t1 - tPrev) * span;
    const float m1 = (vNext - v0) / (tNext - t0) * span;
    const float u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * v0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * v1 +
           (u3 - u2) * m1;
}

}  // namespace detail

/// The scene as it stands at time t: a copy with every track's parameter written in.
///
/// A track whose node id has left the scene is skipped -- an id that is dangling names nothing,
/// and inventing a node for it would be worse than a key that no longer moves anything.
[[nodiscard]] inline Scene sampleAt(const Scene& s, float t) {
    Scene out = s;
    for (std::uint32_t k = 0; k < s.tracks.count; ++k) {
        const Track& tr = s.tracks[k];
        for (std::uint32_t i = 0; i < out.nodes.count; ++i) {
            if (out.nodes[i].id == tr.nodeId) {
                if (tr.paramIndex < 12) {
                    out.nodes[i].params[tr.paramIndex] = detail::trackValueAt(tr, t);
                }
                break;
            }
        }
    }
    return out;
}

/// The last key time over every track, or 0 for a still scene: how long the motion is.
[[nodiscard]] inline float animationLength(const Scene& s) {
    float end = 0.0f;
    for (std::uint32_t k = 0; k < s.tracks.count; ++k) {
        const Track& tr = s.tracks[k];
        if (tr.keyCount > 0 && tr.time[tr.keyCount - 1] > end) {
            end = tr.time[tr.keyCount - 1];
        }
    }
    return end;
}

/// Writes or replaces the key at time t on (nodeId, paramIndex), keeping the keys sorted.
/// Returns false when the track is full or the scene holds no more tracks.
inline bool setKey(Scene& s, std::uint32_t nodeId, std::uint8_t paramIndex, float t, float value,
                   TrackInterp interp = TrackInterp::Linear) {
    Track* tr = nullptr;
    for (std::uint32_t k = 0; k < s.tracks.count; ++k) {
        if (s.tracks[k].nodeId == nodeId && s.tracks[k].paramIndex == paramIndex) {
            tr = &s.tracks[k];
        }
    }
    if (tr == nullptr) {
        if (s.tracks.count >= Scene::kMaxTracks) {
            return false;
        }
        tr = &s.tracks[s.tracks.count++];
        *tr = Track{};
        tr->nodeId = nodeId;
        tr->paramIndex = paramIndex;
        tr->interp = static_cast<std::uint8_t>(interp);
    }
    for (int i = 0; i < tr->keyCount; ++i) {
        if (tr->time[i] == t) {
            tr->value[i] = value;
            return true;
        }
    }
    if (tr->keyCount >= Track::kMaxKeys) {
        return false;
    }
    int at = tr->keyCount;
    while (at > 0 && tr->time[at - 1] > t) {
        tr->time[at] = tr->time[at - 1];
        tr->value[at] = tr->value[at - 1];
        --at;
    }
    tr->time[at] = t;
    tr->value[at] = value;
    ++tr->keyCount;
    return true;
}

}  // namespace makina
