// Teaches MitiruEngine's reflection how to read makina-core's containers.
//
// makina-core cannot include the engine (PLAN.md 3.1): the whole model has to stay testable
// without a device. So it carries its own FixedArray rather than the engine's FixedVec, and the
// two types are unrelated as far as the compiler is concerned.
//
// This header is the seam. It lives in the application, the one place that legitimately knows
// both, and it specialises the engine's IsFixedVec trait for makina::FixedArray. That is all the
// engine needs: with the trait in place, makeFieldDescriptor computes `offsetof(M, count)` against
// makina's own type, so the two layouts do not have to agree -- only the member name does, and
// that is checked at compile time.
//
// Without this, a 23-node model reflects as 256 entries with 233 of them zeroed, and anything
// reading the scene has to guess which are real.

#pragma once

#include <makina/Scene.hpp>

#include <mitiru/core/FixedVec.hpp>
#include <mitiru/module/Reflection.hpp>

#include <cstddef>
#include <type_traits>

namespace mitiru::module::detail {

template <class E, std::size_t N>
struct IsFixedVec<::makina::FixedArray<E, N>> : std::true_type {
    using Elem = E;
    static constexpr std::size_t cap = N;
};

}  // namespace mitiru::module::detail

namespace mitiru::module {

// Names for the element types, so a collection reflects as a list of named structures rather than
// as an unlabelled run of bytes. Without these the host reports elemType as "" and anything reading
// the scene -- an inspector, an agent -- gets 87 opaque blocks instead of 87 nodes it can talk
// about. The names are what make "the Difference called cable-hole" a sentence the reader can form.
template <> struct ReflectName<::makina::CsgNode> {
    static constexpr const char* value = "makina::CsgNode";
};
template <> struct ReflectName<::makina::Material> {
    static constexpr const char* value = "makina::Material";
};
template <> struct ReflectName<::makina::NameSlot<::makina::Scene::kNameLen>> {
    static constexpr const char* value = "makina::NameSlot";
};

}  // namespace mitiru::module

namespace makina_app {

/// The engine holds GameMemory as one memcpy-able value; everything it gives for free -- rewind,
/// replay, structured observation, what-if -- follows from that and nothing else.
static_assert(std::is_trivially_copyable_v<::makina::Scene>,
              "makina::Scene must stay trivially copyable or it cannot be GameMemory");

/// The trait above reads the live length from here. A rename would compile but silently report
/// capacity instead of length, so it is pinned.
///
/// The alias is not decoration: offsetof is a macro, and the comma inside the template argument
/// list would otherwise be read as a second macro argument.
using ProbeArray = ::makina::FixedArray<::makina::CsgNode, 4>;
static_assert(offsetof(ProbeArray, count) == sizeof(::makina::CsgNode) * 4,
              "FixedArray must keep `count` after `data`, where the reflection expects it");

/// MITIRU_REFLECT takes at most 16 fields per struct.
static_assert(::makina::Scene::kMaxNodes <= 65535,
              "node indices are uint16 in CsgNode::firstChild");

}  // namespace makina_app
