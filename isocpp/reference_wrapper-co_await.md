---
author: Lewis Baker
reply_to: ...
audience: Library Evolution
---

# Add an `operator co_await` for `std::reference_wrapper<T>`

## Summary

This paper proposes that we extend the `std::reference_wrapper<T>` type to be conditionally `Awaitable`
if and only if type `T&` is `Awaitable`.

This is achieved by defining an `operator co_await()` overload for `std::reference_wrapper<T>` that
returns the result of calling `std::get_awaiter(this->get())` (proposed in P1288R0).

This allows passing awaitable objects by reference into template coroutine functions that normally
take arguments by value by using `std::ref()`.

## Rationale

There are cases where a caller needs to be able to pass a reference to an awaitable type into a coroutine
and then be able to `co_await` that reference from within the coroutine.

For example:
```c++
task<> say_hello_when_signalled(async_manual_reset_event& event)
{
  co_await event;
  std::cout << "Hello";
}
```

However, one of the common pitfalls of coroutines is that the coroutine can easily outlive the lifetime of the
parameters, particularly if passing values by rvalue-reference. This can result in unexpected dangling references
within the coroutine. For an in-depth discussion of the issue see Toby Allsopp's article
[Coroutines and Reference Parameters](https://toby-allsopp.github.io/2017/04/22/coroutines-reference-params.html).

For example: An error prone implementation of `make_task()`
```c++
template<typename AWAITABLE>
auto make_task(AWAITABLE&& awaitable)
-> task<await_result_t<AWAITABLE>>
{
  co_return co_await std::forward<AWAITABLE>(awaitable);
}

task<> buggy()
{
  auto t = make_task(some_awaitable{}); 
  // Whoops! make_task() just captured a dangling reference to a temporary.
  co_await t; // boom!
}
```

For this reason, generic coroutine functions are generally recommended to take parameters by value
to ensure that dangling references are not silently captured.

```c++
template<typename AWAITABLE>
auto make_task(AWAITABLE awaitable)
-> task<await_result_t<AWAITABLE>>
{
  co_return co_await std::move(awaitable);
}

task<> safe()
{
  auto t = make_task(some_awaitable{});
  // Ok. make_task() captured some_awaitable temporary by value.
  co_await t;
}
```

However, in some cases you still want to allow passing an object by reference but want to ensure that the
capture-by-reference is explicitly called out at the call-site through the use of `std::ref`. This also
has the benefit of avoiding capture of temporary values by reference since `std::ref()` does not accept
rvalue references.

For example:
```c++
task<> foo(async_manual_reset_event& event)
{
  // Explicitly pass 'event' by reference using std::ref().
  auto t = make_task(std::ref(event));
  // ... 
  co_await t;
}
```

The problem with this approach, however, is that `std::reference_wrapper<T>` does not implement
`operator co_await()` and so is not awaitable.

One possible solution would be to have the coroutine explicitly detect the use of `std::reference_wrapper<T>`
argument and unwrap the reference. However, this approach is undesirable as it complicates and places a burden
on all such coroutine implementations.

Ideally, `std::reference_wrapper<T>` would be awaitable itself if `T&` is awaitable.

## Possible Implementation

Below is a candidate implementation of `operator co_await` for `std::reference_wrapper<T>`.

You can also try out this code using Compiler Explorer here: https://godbolt.org/g/WSCMw1

```c++
#include <functional>
#include <type_traits>
#include <exception>
#include <experimental/coroutine>

namespace std
{
  // Assuming existence of std::get_awaiter() proposed in P1288R0

  // The actual operator co_await() definition for std::reference_wrapper<T>
  // Define with trailing return-type to trigger SFINAE behaviour.
  template<typename T>
  auto operator co_await(std::reference_wrapper<T> ref)
    noexcept(noexcept(std::get_awaiter(ref.get())))
    -> decltype(std::get_awaiter(ref.get()))
  {
    return std::get_awaiter(ref.get());
  }
}
```

Here are some test-cases that can be used to verify that `std::reference_wraper<T>` is indeed
awaitable but only if `T&` is awaitable.
```c++
struct some_awaitable
{
  bool await_ready();
  void await_suspend(std::experimental::coroutine_handle<>);
  int await_resume();
};

struct another_awaitable
{
  some_awaitable operator co_await() { return {}; }
};

// Using Awaitable concept from P1288R0
static_assert(std::Awaitable<some_awaitable>);
static_assert(std::Awaitable<another_awaitable>);
static_assert(!std::Awaitable<int>);


static_assert(std::Awaitable<std::reference_wrapper<some_awaitable>>);
static_assert(std::Awaitable<std::reference_wrapper<another_awaitable>>);
static_assert(!std::Awaitable<std::reference_wrapper<int>>);

// Dummy task coroutine type
struct task
{
  struct promise_type
  {
    task get_return_object() { return {}; }
    std::experimental::suspend_never initial_suspend() { return {}; }
    std::experimental::suspend_never final_suspend() { return {}; }
    void return_void() {}
    [[noreturn]] void unhandled_exception() { std::terminate(); }
  };

  task() {}
};

task some_coroutine(std::reference_wrapper<some_awaitable> ref)
{
  // std::reference_wrapper<T> is awaitable iff T is awaitable
  int result = co_await ref;
}
```

## Proposed Wording

TODO: Implement me.
