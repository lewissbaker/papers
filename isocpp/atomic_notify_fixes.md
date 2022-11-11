---
document: D2616R2
title: Making std::atomic notification/wait operations usable in more situations
author: Lewis Baker <lewissbaker@gmail.com>
date: 2022-11-11
target: C++26
audience: SG1
---

- [Abstract](#abstract)
- [Motivation](#motivation)
- [Conclusion](#conclusion)
- [References](#references)

# Revision History

## R2

- Reduce paper down to option 3 only at request of SG1
- Add design discussion
- Add wording
- Rephrase abstract

## R1

- Added Option 3 - `atomic_notify_token` approach.
- Incorporate feedback on limitations of option 2.

## R0

Proposed two options.
- Option 1 - allow `std::atomic_notify_one/all()` on potentially destroyed pointer
- Option 2 - add fused modify + notify overloads, `std::memory_notification` enum

# Abstract

C++20 added support to `std::atomic` for `notify_one()`, `notify_all()` and `wait()` operations
which give applications an efficient, light-weight way to block until the value of an atomic
object reaches a certain value.

A waiting thread calls the `x.wait(oldValue)` to wait until the atomic object, `x`'s value
changes from `oldValue` to something else and a signalling thread first stores a new value
to the atomic object, `x`, and then calls one of the notify-methods to wake up any threads
that have blocked inside a call to `wait()`.

For use-cases where a waiting thread can go on to destroy the atomic object once it
has observed the store, the signalling thread's subsequent call to the atomic object's
`notify_one()` or `notify_all()` member function can potentially have undefined behaviour.
This is becasue the standard `[basic.life]` p6.2 states that a call to a member function
on a pointer to an object whose lifetime has ended has undefined behaviour. 

This paper proposes introducing a new API for obtaining a `std::atomic_notify_token<T>` from
a `std::atomic<T>` or `std::atomic_ref<T>` which can then be used to notify threads waiting
on that atomic object without worrying about whether the underlying atomic object is still
alive.

This paper also proposes deprecating the existing `notify_one()` and `notify_all()` member
functions of `std::atomic` and `std::atomic_ref` type specialisations.

It also proposes redefining `std::atomic_notify_one/all()` in terms of the new facilities
instead of in terms of the deprecated member functions.

Usages of the namespace-scope functions will still have potential for undefined behaviour
in some cases, however. Resolving these issues is deferred pending the outcome of core
language changes proposed by the "pointer zap" papers (P1726 and P2188) with regards to
pointer provenance.

This paper does not attempt to address the undefined behaviour C compatibility layer for `std::atomic`.

# Motivation

Consider the following example:

```c++
int main() {
  thread_pool tp;

  {
    std::atomic<int> x{0}; // #1
    tp.execute([&] {       // #2
      x.store(1);          // #3
      x.notify_one();      // #4
    });
    x.wait(0);             // #5
    assert(x.load() == 1);
  }                        // #6
}
```

Let T1 be the thread executing `main()` and T2 be the thread-pool thread that executes the lambda.

We might end up with a situation where the following sequence of events is executed:
```
T1: constructs the atomic `x` and initialises to 0 (#1)
T1: enqueues the task to execute on the T1 (#2)
T2: dequeues the task and invokes the lambda
T2: stores the value 1 to the `x` (#3)
T1: executes `x.wait(0)`, sees value is now 1 and returns immediately (#5)
T1: destroys the object `x` (#6)
T2: executes `x.notify_one()` on a now destroyed object `x` (#4)
```

The final call to `x.notify_one()` member function on a destroyed object in (#4) has
undefined behaviour.

To work around this potential for operating on a dangling reference, we can use two
separate atomic variables - `wait()` on one and then spin-wait on the other.

```c++
int main() {
  thread_pool tp;

  {
    std::atomic<int> x{0};
    std::atomic_flag f{true};
    tp.execute([&] {
      x.store(1);
      x.notify_one();
      f.clear();
    });
    x.wait(0);        // efficient-wait
    while (f.test()); // spin-wait
    assert(x.load() == 1);
  }
}
```

This has a couple of downsides, however:
* It needs to perform two atomic operations instead of one
* The spin-wait can still result in occasional long busy-wait times, wasting CPU resources
  e.g. if the signalling thread is context-switched out by the OS after the call to
  `x.notify_one()` but before the call to `f.clear()`, the main thread will busy-wait
  spin until the signalling thread is rescheduled.

For this particular situation, we could instead use a `std::binary_semaphore`, as its
`acquire()` and `release()` operations are defined as being atomic, and thus they do
not have the same lifetime issues as the first `std::atomic` implementation.

```c++
int main() {
  thread_pool tp;

  {
    std::binary_semaphore sem{0};
    tp.execute([&] {
      sem.release();
    });
    sem.acquire();
  }   
}
```

However, if we then ask the question "Can we implement `std::counting_semaphore` using `std::atomic`?",
we quickly run into the same lifetime questions regarding use of `notify_one/all()` methods.

Indeed, if we look at some of the standard library implementations of `std::counting_semaphore` we
see that they do actually follow the same pattern as above for the `release()` operation - an atomic
store followed by a call to either `notify_all()` or `notify_one()` on the atomic object.

* See [libc++ counting_semaphore::release() implementation](https://github.com/llvm/llvm-project/blob/643df8fa8ef58d883cbb554c7e71910dc8a8673c/libcxx/include/semaphore#L90-L99).
* See [libstdc++ counting_semaphore::release() implementation](https://github.com/gcc-mirror/gcc/blob/8467574d8daac47e0cf5b694f6c012aad8d630a6/libstdc%2B%2B-v3/include/bits/semaphore_base.h#L248-L259)
* See [msvc counting_semaphore::release() implementation](https://github.com/microsoft/STL/blob/c34f24920e463a71791c2ee3d2bed14926518965/stl/inc/semaphore#L74-L112)

So why don't they run into the same lifetime issues?

The reason is that in all major standard library implementations of `std::atomic::notify_all()` and
`std::atomic::notify_one()` depend only on the address of the atomic object, but do not actually
access any data-members of the atomic object. These platforms also do not seem to target architectures
that enforce pointer provenance rules.

The underlying implementations of these notifying functions typically boil down to a call to:
* `WakeByAddressSingle()` / `WakeByAddressAll()` on Windows platforms
* `futex()` with `futex_op` set to `FUTEX_WAKE` on Linux platforms
* `__ulock_wake()` on Mac OS platforms
* Use of one of a collection of statically-allocated condition-variables, with the particular one
  chosen based on the bit-representation of the provided address.

All of these depend only on the bit-representation of address of the atomic variable,
which makes calling `count.notify_all()` work, even though the `count` object may have
since been destroyed, as it doesn't try to dereference the pointer.

Standard library implementations have extra knowledge about the semantics of the `std::atomic::notify_one/all()`
methods which they can leverage to allow safely implementing `std::counting_semaphore` in terms of
`std::atomic` operations. However, users of the standard library which want to be portable cannot make
these same assumptions - the standard library specification does not currently require that all implementations
have this same behaviour, and so users must resort to other, more portable means.

Instead of requiring users to work-around these limitations of the interface, we should provide
some standard/portable way for users to safely modify an atomic value and notify waiting threads
of the change.

# Proposed design

This paper proposes the following:

* Adding the `std::atomic_notify_token<T>` class template which provides an interface for
  safely notifying threads waiting on 
* Adding the `get_notify_token()` member function to `std::atomic<T>` and `std::atomic_ref<T>` member
  functions for obtaining a notify-token for the corresponding atomic object.
* Marking the `notify_one()` and `notify_all()` member functions of `std::atomic<T>` and
  `std::atomic_ref<T>` as deprecated.

Synopsis:
```c++
namespace std {
  template<typename T>
  class atomic_notify_token;

  template<typename T>
  class atomic {
  public:
    // Existing members...
    
	[[deprecated]] void notify_one() noexcept;
	[[deprecated]] void notify_all() noexcept;
	
    atomic_notify_token<T> get_notify_token() noexcept;
    
  };
  
  template<typename T>
  class atomic_ref {
  public:
    // Existing members...
	
	[[deprecated]] void notify_one() noexcept;
	[[deprecated]] void notify_all() noexcept;
    
    atomic_notify_token<T> get_notify_token() noexcept;
  };
  
  template<typename T>
  class atomic_notify_token {
  public:
    // Copyable
    atomic_notify_token(const atomic_notify_token&) noxcept = default;
    atomic_notify_token& operator=(const atomic_notify_token&) noxcept = default;
    
    // Perform notifications
    void notify_one() const noexcept;
    void notify_all() const noexcept;
  private:
    // exposition-only
    friend class atomic<T>;
    explicit atomic_notify_token(std::uintptr_t p) noexcept : address(p) {}
    std::uintptr_t address;
  };
}
```

The API would be used as follows:

```c++
int main() {
  thread_pool tp;

  {
    std::atomic<int> x{0};
    tp.execute([&] {
	  // Obtain a notify-token while the object is definitely still alive.
      auto tok = x.get_notify_token();
	  
	  // Perform the store - this may cause wait() to return and the main
	  // thread to destroy `x'.
      x.store(1);
	  
      // `x' is potentially destroyed from this point on
	  
	  // Safely notify any objects still waiting on `x' (if any)
	  tok.notify_one();
    });
    x.wait(0);
    assert(x.load() == 1);
  }
}
```

# Design discussion

## Why does this solution avoid the undefined behaviour?

Current implementations of `std::atomic` notify/wait mechanisms use some kind of hash-table
of synchronisation primitives for recording waiting threads and waking them up.

This could either be implemented on the kernel-side of the underlying OS in a
`futex()`-like API, or could be implemented in user-space using standard C++
synchronisation primitives.

When obtaining a notify-token, the implementation can perform any necessary hashing
of the atomic object address at the time that `get_notify_token()` is called, when the
atomic object is still known to be alive.

Then later, when actually performing the notification the implementation can lookup the
corresponding hash-table entry to use for synchronisation using the pre-hashed address
without needing the address of a potentially destroyed atomic object.

For implementations where the hashing of the address is done in the OS, they would
need to at least define it as valid to pass the address of a potentially destroyed
object to the syscall. 

## Conditional notification

This design still retains the ability to conditionally notify waiting threads based
on the result of a read-modify-write operation.

This would not be possible were we to take a fused store-and-notify approach.

## Why is `std::atomic_notify_token` a template?

The strategy used for notify/wait can vary depending on the type of value being synchronised.

For example, on Linux the `futex()` API on which notifications are based only works natively
for 32-bit values. When notifying/waiting for types that are not 32-bits in size, the
implementation needs to use a proxy 32-bit value which is incremented for every call to notify.

As different types of atomics may need to have different strategies for notification, the
`atomic_notify_token` needs to be be a type that depends on the type of atomic value.

## Const-qualification of `get_notify_token()` and `atomic_notify_token` methods

The status quo is that the `std::atomic` methods `notify_one()` and `notify_all()`
methods are non-const and thus are only callable on a non-const `std::atomic` value.

This proposal preserves the requirement that the user cannot notify a const-qualified
atomic by marking the `get_notify_token()` method as non-const. This prevents the user
from obtaining a token if they only have a const-reference to an atomic object.

Once the user has obtained a notify-token, they have shown that they have non-const
access to the atomic object.

The notify-token member-functions do not need to also be non-const to enforce const-correctness
as the notify-tokens effectively have pointer-semantics. Marking the member-functions as non-const
would be easily worked around by just copying the token.

## Where should the notify-token type be defined?

This paper proposes introducing a new namespace-scope class template `std::atomic_notify_token<T>`.

This type is needed by methods in both `std::atomic<T>` and `std::atomic_ref<T>`.
	
An alternative to consider is defining a nested `std::atomic<T>::notify_token` type and
then defining `std::atomic_ref<T>::notify_token` as a type-alias of `std::atomic<T>::notify_token`.
However, this would have the consequence of forcing an template instantiation of `std::atomic<T>`
for every template instantiation of `std::atomic_ref<T>`, even if it's not used.

## Naming of `atomic_notify_token`

Is the use of the term "token" here consistent with other usages in the standard library?
e.g. `std::stop_token`

## Fixing `std::atomic_notify_one/all()`

This proposal does not attempt to change the definition of the namespace-scope functions
`std::atomic_notify_one/all()`.

These functions are invoked with a pointer to the atomic object rather than being member
functions. So unlike a call to the notify member-functions, which has undefined behaviour
if the object's lifetime has ended, the behaviour of passing a pointer to an object whose
lifetime has ended is implementation-defined.

So while some implementations might define passing a pointer to an object whose lifetime
has ended as valid, other implementations might choose to trap on such uses of a pointer,
making writing portably correct code using this API difficult/impossible.

Whether or not this can be made portably safe depends on the resolution to the "pointer zap"
issue, which is the subject of papers P1726 and P2188, and so I do not attempt to address
the issue in this paper.

If desired, the existing namespace-scope functions could be deprecated and new
notify-token-based replacements added. However, it has not yet been explored what the
impacts would be on compatibility with C atomics.

In the meantime, we could consider changing the wording of `std::atomic_notify_one/all()`
to no longer be in terms of the corresponding atomic member functions so that we can
at least allow the correctness to be implementation-defined instead of undefined-behaviour
for the end-of-lifetime cases.

# Potential Implementation Strategies

## Platforms without native OS support

On platforms without native OS support for address-based notification, the notify/wait
mechanisms of `std::atomic` could be implemented in terms of existing synchronisation
primitives.

For example: Given the following `__wait_state` helper class definition
```c++
struct __wait_state {
    std::atomic<uint64_t> _M_waiters{0};
    std::mutex _M_mut;
    std::condition_variable _M_cv;
    std::uint64_t _M_version{0};

	// Get the wait state for a given address.
    static __wait_state& __for_address(void* __address) noexcept {
        constexpr std::uintptr_t __count = 16;
        static __wait_state __w[__count];
        auto __key = (reinterpret_cast<std::uintptr_t>(__address) >> 2) % __count;
        return __w[__key];
    }

    void __notify() noexcept {
        if (_M_waiters.load() != 0) {
            {
                std::lock_guard __lk{_M_mut};
                ++_M_version;
            }
            _M_cv.notify_all();
        }
    }

    template<typename _Pred>
    void __wait(_Pred __pred) noexcept {
        for (int __i = 0; __i < 10; ++__i) {
            if (__pred()) return;
            __yield_thread();
        }

        _M_waiters.fetch_add(1, std::memory_order_seq_cst);
        std::uint64_t __prev_version = [&] {
            std::unique_lock __lk{_M_mut};
            return _M_version;
        }();
        while (!__pred()) {
            std::unique_lock __lk{_M_mut};
            if (_M_version == __prev_version) {
                _M_cv.wait(__lk);
            }
            __prev_version = _M_version;
        }
        _M_waiters.fetch_sub(1, std::memory_order_release);
    }
}
```

The atomic notify/wait methods can then be defined as follows:
```c++
namespace std {

template<typename T>
class atomic_notify_token {
public:
	void notify_one() noexcept { __state_->__notify(); }
	void notify_all() noexcept { __state_->__notify(); }
private:
    friend class atomic<T>;
	friend class atomic_ref<T>;
    explicit atomic_notify_token(__wait_state& __state) noexcept
	: __state_(&__state) {}
	__wait_state* __state_;
};
	
template<typename T>
class atomic {
public:
  
  [[deprecated("Use get_notify_token().notify_one() instead")]]
  void notify_one() noexcept {
	  get_notify_token().notify_one();
  }
  
  [[deprecated("Use get_notify_token().notify_all() instead")]]
  void notify_all() noexcept {
	  get_notify_token().notify_all();
  }

  atomic_notify_token<T> get_notify_token() noexcept {
	  __wait_state& __s = __wait_state::__for_address(this);
	  return atomic_notify_token<T>{__s};
  }
	
  void wait(T __old, memory_order __mo) noexcept {
    auto __pred = [__mo, __old, this]() noexcept { return this->load(__mo) != __old; };
    auto& __s = __wait_state::__for_address(this);
    __s.wait(__pred);
  }
};
	
} // namespace std
```

In usage of this implementation, the `__wait_state*` object is computed from the address
of the atomic object during the call to `get_notify_token()`, while the atomic object is
still alive.

Once the address of the `__wait_state` object is computed there is no need for the atomic
object address any more - and thus no need to use a pointer to a potentially destroyed object.

## Platforms with native OS support

On platforms that use a `futex()`-like OS API to implement waiting, the token could hold the address
of the object (e.g. as a `void*` or `std::uintptr_t`) assuming on this particular implementation that
it defines it as valid to pass a pointer to a potentially destroyed object.

Example: Possible implementation on Windows
```c++
namespace std {
	
template<typename T>
class atomic_notify_token {
public:
  void notify_one() noexcept { WakeByAddressSingle(ptr); }
  void notify_all() noexcept { WakeByAddressAll(ptr); }
private:
  friend class atomic<T>;
  explicit atomic_notify_token(void* p) noexcept : ptr(p) {}
  void* ptr;
};

template<typename T>
class atomic {
public:
  [[deprecated("Use get_notify_token().notify_one() instead")]]
  void notify_one() noexcept {
	get_notify_token().notify_one();
  }
  
  [[deprecated("Use get_notify_token().notify_all() instead")]]
  void notify_all() noexcept {
	get_notify_token().notify_all();
  }

  atomic_notify_token<T> get_notify_token() noexcept {
	return atomic_notify_token<T>{this};
  }
	
  void wait(T __old, memory_order __mo) noexcept {
	while (load(__mo) == __old) {
	  WaitOnAddress(this, std::addressof(__old), sizeof(T), INFINITE);
    }
  }
};

}
```
# Wording

TODO

# References

SG1 mailing list thread on the issue
https://lists.isocpp.org/parallel/2020/07/3270.php

P1726R5 - "Pointer lifetime-end zap and provenance, too" (Paul McKenney, Maged Michael, et. al.)
https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2021/p1726r5.pdf

P2188R1 - "Zap the Zap: Pointers are sometimes just bags of bits"
https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p2188r1.html
