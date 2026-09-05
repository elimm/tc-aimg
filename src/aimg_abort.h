#ifndef AIMG_ABORT_H
#define AIMG_ABORT_H

#include <atomic>

// Set for the duration of one parse by ContentGetValueW, polled by the
// parser/decoder's coarse loop checks. A thread_local pointer rather than a
// parameter threaded through ~40 functions: the ComfyUI traversal is deep,
// and a polled flag is the shape the SDK itself documents.
//
// Function-local thread_local static inside an inline function, NOT a
// namespace-scope `inline thread_local`: inline variables are a C++17
// language feature and the raw-cl build paths don't pass /std:c++17. The
// function-local form has had one-definition-across-TUs semantics since
// C++98 and needs no separate .cpp definition.
inline const std::atomic<bool>*& AbortFlagSlot() {
    static thread_local const std::atomic<bool>* flag = nullptr;
    return flag;
}

// Set only when a poll actually observed the flag, i.e. a walk really did
// bail out mid-parse. Deliberately distinct from the flag itself: a stop can
// arrive in the window between a clean finish and the in-flight registry
// entry being erased, where the result is complete and cacheable. Only this
// answers "may this result be partial". Same pattern as AbortFlagSlot().
inline bool& AbortObservedSlot() {
    static thread_local bool observed = false;
    return observed;
}

inline bool AbortRequested() {
    const std::atomic<bool>* flag = AbortFlagSlot();
    bool requested = flag && flag->load(std::memory_order_relaxed);
    if (requested) AbortObservedSlot() = true;
    return requested;
}

#endif // AIMG_ABORT_H
