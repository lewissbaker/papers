---
author: Lewis Baker
reply_to: lewissbaker@gmail.com
audience: Evolution
---

# On allowing both `co_return;` and `co_return value;` in coroutines

## Introduction

In the Coroutines TS (N4680) section 8.4.4(4) currently specifies that:
> The _unqualified-ids_ `return_void` and `return_value` are looked up in the
> scope of class _P_. **If both are found, the program is ill-formed**.

I believe this restriction is unnecessary to place on coroutine promise types
and prevents some interesting use cases. In particular, this restriction makes
syntax for implementing tail-recursion of coroutines unnecessarily cumbersome.

I propose that we amend the Coroutines TS to allow defining coroutine promise
types that define both the `return_void` and `return_value` members by removing
the sentence highlighted in bold above.

## Background

The original motivation for this restriction, as far as I understand it, is to
maintain consistency with normal functions. A normal function cannot contain
both `return;` and `return someValue;` statements.

This rule makes perfect sense for normal functions. The type of the expression passed
to the `return` statement in a function directly corresponds to the return-type of that
function. A function cannot have both a `void` and non-`void` return-type at the same time
so it cannot make sense to have both `return;` and `return someValue;` in the same
function.

For example, the following function body isn't able to fulfill the contract
of the function signature with the second `return` statement (it needs to return an `int`)
and so the program is considered ill-formed.
```c++
int f()
{
  if (someCond) return 123;
  else return; // Error: f() needs to return an 'int' value.
}
```

However, with coroutines, the return-type of a coroutine is _not_ directly
tied to the type passed to the `co_return` statement. The author of the coroutine
promise type is able to control the semantics of the `co_return` statement
by defining either a `return_value` or `return_void` method on the promise type.

A common example of this is a coroutine that has a return-type of `generator<int>`.
This coroutine allows a `co_return;` statement, a statement that is equivalent to
returning a `void` value, even though the coroutine's return-type is not `void`.

```c++
generator<int> collatz_sequence(int n)
{
  while (true)
  {
    co_yield n;

    // Returning 'void' even though coroutine return-type is generator<int>.
    if (n == 1) co_return; 

    if (n % 2 == 0) n /= 2;
    else n = (3 * n + 1) / 2;
  }
}
```

With the current TS wording a coroutine promise type author is also able to
define multiple overloads of `return_value()`, allowing the coroutine to define
different semantics for `co_return` statements based on the expression type passed.

It does not seem like a big step to go from allowing the user to define different
semantics for two different types passed to `co_return <expr>` by defining
two overloads of `return_value()` to allowing the user to define different semantics
for `co_return` and `co_return <expr>` by defining both `return_void()` and
`return_value()`.

## Tail-Recursion of Coroutines

The motivating use-case for the proposed change is to allow use of the `co_return`
keyword as a convenient and intuitive syntax for indicating a tail-recursive call
to a coroutine.

A tail-recursive call to another coroutine is an operation where the calling coroutine
frame is destroyed before resuming execution of the coroutine that is being called
in the tail-position. This allows you to perform recursion in the tail-position to an
arbitrary recursion depth while needing at most two coroutine frames to be allocated
at any one time.

Example: A simple tail-call of an async task.
```c++
recursive_task<T> bar();

recursive_task<T> foo_no_tail_recursion()
{
  co_await do_something();

  // Call bar() and await result, unwrapping task<T> to obtain T value.
  // Then returns the value of type T.
  //  - calls return_value(T) if T is non-void.
  //  - calls return_void() if T is void.
  co_return co_await bar();
}

recursive_task<T> foo_tail_recursive()
{
  co_await do_something();

  // Return recursive_task<T> value directly as way of indicating that
  // the result of bar()'s task should be used as the result
  // of foo_tail_recursive().
  co_return bar();
}

task<> usage()
{
  recursive_task<T> t = foo_tail_recursive();
  T result = co_await t;
}
```

The semantics of the tail-recursive statement `co_return bar();` is as follows:
* Call `bar()` to create a coroutine frame for `bar()`.
  This coroutine will immediately suspend at `initial_suspend` point and return the `recursive_task<T>` RAII object.
* Transfer ownership of `bar()`'s coroutine frame to the `recursive_task<T>` object that currently owns the
  `foo_tail_recursive()` coroutine frame.
  In the example above, this means updating the coroutine handle stored in the variable `t` in the `usage()` coroutine.
* Transfer responsibility for resuming awaiter of `foo_tail_recursive()`'s task object to `bar()`'s promise object.
  In the example above, the awaiting coroutine is `usage()` so this would be copying the `coroutine_handle` for the `usage()`
  coroutine into `bar()`'s promise object.
* Destroy `foo_tail_recursive()` coroutine frame.
* Resume execution of `bar()`.

Note: To guarantee that these tail-recursive statements have bounded memory usage of both heap-allocated coroutine frames
and stack-frames, it requires an extension to Coroutines TS to allow symmetric transfer of execution when suspending
one coroutine and resuming another.
I believe Gor Nishanov is working on a proposal to allows a third variant of `await_suspend()` that returns a
`coroutine_handle<>` to resume with tail-call semantics. He has already implemented an experimental implementation
of this extension in Clang.

Assuming that we have the above-mentioned symmetric-transfer extension to Coroutines TS,
it is already possible to implement such a tail-recursive call operation for tasks that
return a value. This is because we can define two overloads of `return_value`; one taking
a `T` and one taking a `recursive_task<T>&&`.

However, it is not currently possible to do this for `void`-returning tasks, as that would
require defining `return_void()` for the normal return case and `return_value(recursive_task<void>&&)`
to handle the tail-recursive call case. Something that is currently disallowed by the current TS wording.

If we allowed promise types to define both `return_void` and `return_value` then this
would allow implementation of the tail-call capapbilities of `recursive_task` for all types,
not just non-`void` types.

## Tail-Recursive Generators

Another use-case for coroutine tail-recursion is with generator coroutines.

The cppcoro library provides a `recursive_generator<T>` type that allows a
coroutine to lazily produce a sequence of values of type, `T`, by `co_yield`ing
either a value of type `T` or a `recursive_generator<T>` value.

If a `recursive_generator<T>` value is yielded then this is equivalent to yielding
all of the values produced by that generator. Execution of the current coroutine only
resumes once all of the nested generator values have been consumed.

The advantage of this approach is that it allows the consumer to directly resume the
leaf-most nested coroutine to produce the next element inside its call to `iterator::operator++()`
rather than requiring O(depth) resume/suspend operations that would be required if
using a non-recursive `generator`.

For example: Traversing values of a binary tree in left-node-right order.
```c++
// Given a basic tree structure
template<typename T>
struct tree
{
  tree<T>* left;
  tree<T>* right;
  T value;
};

recursive_generator<T> traverse_tree(tree<T>* t)
{
  if (t->left) co_yield traverse_tree(t->left);
  co_yield t->value;
  if (t->right) co_yield traverse_tree(t->right);
}

void usage()
{
  tree<std::string>* root = get_a_tree();
  for (std::string& value : traverse_tree(root))
  {
    std::cout << value << std::endl;
  }
}
```

Notice in the `traverse_tree()` coroutine that there is no logic
in the coroutine body after the `co_yield traverse_tree(t->right);`
statement. The coroutine has suspended execution, but the only thing
the continuation is going to do when it resumes is run to completion,
suspend at `co_await promise.final_suspend()` and then resume its
parent coroutine or return to the consumer.

When execution returns to the parent/consumer after the
generator has run to completion the parent/consumer will then
typically immediately destroy the generator object, which will
in turn destroy the coroutine frame, freeing its memory.

However, this means that we are deferring releasing the memory used
by the parent coroutine frame until the consumer has finished consuming
all of the child generator's elements even though the parent coroutine
is not going to be producing any more values.

If we could instead make use of tail-recursion in the case where a 
`co_yield` statement occurs in the tail position then this would allow
the parent coroutine frame to be freed earlier, before resuming execution
of the nested coroutine.

Doing so would allow coroutines to support recursion to an arbitrary depth
when recursing in the tail position. This can be done using only a bounded
amount of memory for the coroutine frames; typically at most two coroutine
frames allocated at any one time.

### Tail call syntax

My preferred syntax for indicating a tail-recursive yield is to allow
`co_return std::move(childGenerator);` in the place of `co_yield childGenerator;`.

For example, the above `traverse_tree()` would become:
```c++
recursive_generator<T> traverse_tree_tail_recursive(tree<T>* t)
{
  if (t->left) co_yield traverse_tree_tail_recursive(t->left);
  co_yield t->value;
  // Use of co_return indicates tail-recursion on right subtree
  if (t->right) co_return traverse_tree_tail_recursive(t->right);
}
```

The problem with this approach is that it requires the `promise_type` for the
coroutine to have both a `return_void()` method (for the case where execution runs
off the end of the coroutine) and a `return_value(recursive_generator<T>&&)` method
(for the case where we are performing tail-recursion). This is something which is
currently banned by the Coroutines TS wording in N4680.

### Alternative tail call syntax

There are alternative syntaxes that could be implemented while staying within the current
wording of the TS. However, these alternative syntaxes have downsides.

**Alternative Syntax 1: Overload `co_yield` with a `tail_call()` helper function.**
```c++
recursive_generator<T> traverse_tree_alternative1(tree<T>* t)
{
  if (t->left) co_yield traverse_tree_alternative1(t->left);
  co_yield t->value;
  if (t->right) co_yield tail_call(traverse_tree_alternative1(t->right));
}
```

This would work by having the `tail_call()` helper wrap the `recursive_generator<T>` object
in a new type, say `recursive_generator_tail_call<T>`, and then providing a `yield_value()`
overload for that type which could then perform the tail-recursion operation.

This has the downside of requiring the more verbose syntax.

It is also less obvious to the developer that the coroutine will not continue exection after
executing the `co_yield tail_call(...)` expression.

It may also be difficult for the compiler to determine that execution does not continue after
the `co_yield tail_call(...)` expression which could make it more difficult to issue warnings
about dead-code, etc. that would otherwise be possible were we using `co_return`.

**Alternative 2: Remove `return_void()` and keep only `return_value(recursive_generator<T>&&)`**
```c++
recursive_generator<T> traverse_tree_alternative2(tree<T>* t)
{
  if (t->left) co_yield traverse_tree_alternative2(t->left);
  co_yield t->value;
  if (t->right) co_return traverse_tree_alternative2(t->left);

  // Execution not allowed to run-off end.
  // Return a sentinel value instead.
  co_return recursive_generator<T>{};
}
```

This approach works by using a special sentinel value (in this case a default-constructed
`recursive_generator<T>` value) that can be returned to indicate that no tail-recursion
should be performed.

This has the downside of making simple generator coroutines more verbose
as it forces every `recursive_generator` coroutine to have a `co_return`
statement, not just the ones that make use of tail-recursion. A generator
coroutine can no longer just let execution run off the end since that is
undefined-behaviour if the promise object has no `return_void()` method.

# Final Words

TODO: Summary + Call-to-action.
