// Fixed-capacity array with a live count, laid out as {T data[N]; uint32 count;}.
//
// makina-core does not depend on MitiruEngine, so it cannot use the engine's FixedVec. But the
// layout is deliberately identical, because the engine's reflection reads a collection's live
// length from a count stored *inside* the field. Without that, a 23-node model is reported as 256
// entries, 233 of them zeroed -- which wastes the reading budget of anything looking at the scene
// and, worse, invites it to treat the padding as real geometry.
//
// The coupling is real and nothing in this header can check it. The Makina application, which is
// the one place that includes both, carries a static_assert that the two layouts still agree; if
// the engine ever reorders FixedVec, that assert is what fails rather than the reflection quietly
// reporting nonsense.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace makina {

template <typename T, std::size_t N>
struct FixedArray {
    static_assert(std::is_trivially_copyable_v<T>,
                  "FixedArray<T,N>: T must be trivially copyable, or the whole scene stops being "
                  "one memcpy-able value and the engine's rewind cannot hold it");

    T             data[N]{};
    std::uint32_t count = 0;

    [[nodiscard]] constexpr std::size_t size() const noexcept { return count; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }
    [[nodiscard]] constexpr bool empty() const noexcept { return count == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return count >= N; }
    constexpr void clear() noexcept { count = 0; }

    constexpr T& operator[](std::size_t i) noexcept { return data[i]; }
    constexpr const T& operator[](std::size_t i) const noexcept { return data[i]; }

    /// Returns false when full rather than throwing or growing. Callers must look: a silently
    /// dropped node is a model that quietly differs from the one on disk.
    [[nodiscard]] constexpr bool push(const T& v) noexcept {
        if (count >= N) {
            return false;
        }
        data[count++] = v;
        return true;
    }

    constexpr T* begin() noexcept { return data; }
    constexpr T* end() noexcept { return data + count; }
    constexpr const T* begin() const noexcept { return data; }
    constexpr const T* end() const noexcept { return data + count; }
};

}  // namespace makina
