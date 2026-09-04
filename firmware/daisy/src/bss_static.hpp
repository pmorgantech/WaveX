#pragma once

// Static storage that lands in .bss even when T's defaults are non-zero.
//
// A namespace-scope `static T obj;` whose default member initialisers are
// constant expressions is CONSTANT-initialised: the compiler materialises the
// entire object image and the linker places it in .data, so it costs its full
// size in flash AND is copied into SRAM by the startup code, before main().
// That is the right trade for a small struct. It is the wrong one by two
// orders of magnitude for a 34 KB InstrumentBank or a 50 KB SequencerTransport
// whose only non-zero content is a handful of defaults (key_hi = 127,
// velocity = 100, tempo = 120.0): at one point 121 KB of a 438 KB image was
// such default images, and the optimisation level does not change that.
//
// BssStatic<T> keeps raw zero-initialised storage - .bss, which costs nothing
// in flash - and runs T's constructor into it during static initialisation.
// The image is replaced by the constructor's code, a loop writing the
// defaults. Audio starts long after static initialisation, so callback-owned
// state is fully constructed before it is ever read.
//
// Reconstruct() and ReconstructInPlace() are the reset idiom for the same
// objects. `obj = T{}` first builds a complete T on the stack and then copies
// it, so resetting a 34 KB bank cost 34 KB of stack against a 64 KB DTCM
// stack budget; re-running the constructor in place costs none.
//
// Header-only and HAL-free; placement new only, no heap. Host tests compile
// it directly.

#include <cstddef>
#include <cstdint>
#include <new>

namespace WaveX {

// Re-runs T's default constructor on an existing object, with no temporary.
template <typename T>
inline void ReconstructInPlace(T& obj) {
    obj.~T();
    new (&obj) T();
}

template <typename T>
class BssStatic {
   public:
    BssStatic() { new (storage_) T(); }
    BssStatic(const BssStatic&) = delete;
    BssStatic& operator=(const BssStatic&) = delete;

    T& Get() { return *std::launder(reinterpret_cast<T*>(storage_)); }
    const T& Get() const { return *std::launder(reinterpret_cast<const T*>(storage_)); }

    // Reset to defaults: destroys and re-runs the constructor in place.
    void Reconstruct() { ReconstructInPlace(Get()); }

   private:
    alignas(T) uint8_t storage_[sizeof(T)];
};

}  // namespace WaveX
