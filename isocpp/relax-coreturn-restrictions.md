---
topic: Coroutines TS
title: In support of allowing both return_void and return_value on a coroutine promise type.
author: Lewis Baker
reply_to: lewissbaker@gmail.com
audience: Evolution
---

# Introduction

In the Coroutines TS (N4680) section 8.4.4(4) currently specifies that:
> The _unqualified-ids_ `return_void` and `return_value` are looked up in the
> scope of class _P_. **If both are found, the program is ill-formed**.

I believe this restriction is unnecessary to place on coroutine promise types
and prevents some interesting use cases.

# Background

The original motivation for this restriction, as far as I understand it, is to
maintain consistency with regular functions, where a function cannot contain
both `return;` and `return someValue;` statements.

This rule makes perfect sense for regular functions. The type of the expression passed
to the `return` statement in a function directly corresponds to the return-type of that
function. A function cannot have both a `void` and non-`void` return-type at the same time
so it cannot makes sense to have both `return;` and `return someValue;` in the same
function.

For example, the following function body isn't able to fulfill the contract
of the function signature with the second `return` statement and so is ill-formed.
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
by defining either the `return_value` or `return_void` method on the promise type.

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

# Tail-Resursive Tasks

The

# Tail-Recursive Generators

The cppcoro library I have been working on provides a `recursive_generator<T>` type that
allows a coroutine to lazily produce a sequence of values of type, `T`, by `co_yield`ing
either a value of type `T` or a `recursive_generator<T>` value.

In the case that a `recursive_generator<T>` value is yielded, the generator yields all
of the values produced by the nested generator as its own values and resumes the current
coroutine only once all elements of the nested generator have been consumed. When the
consumer asks for the next element by executing `iterator::operator++()`, the most-nested
coroutine is resumed directly to produce the next element rather than requiring O(depth)
resume/suspend operations that would otherwise be required if using a non-recursive `generator`.



For example: Traverse values of a binary tree in left-node-right order.
```c++
template<typename T>
struct tree
{
  tree<T>* left;
  tree<T>* right;
  T value;
};

// Using a regular generator<T> type.
generator<T> traverse_tree_slow(tree<T>* tree)
{
  if (tree->left)
  {
    for (auto& value : traverse_tree_slow(tree->left))
    {
      co_yield value;
    }
  }
  co_yield tree->value;
  if (tree->right)
  {
    for (auto& value : traverse_tree_slow(tree->right))
    {
      co_yield value;
    }
  }
}

// Using a recursive_generator<T> type.
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

Typically, the first thing the parent/consumer is going to do when
the generator reaches the end of the sequence is destroy the
`recursive_generator<T>` object, which will in turn destroy the
coroutine frame.

However, this means that we are deferring releasing the parent coroutine
frame until the consumer has finished consuming all of the child generator's
elements. If the tree was very deep then this could potentially be a large
number of memory allocations 

Wouldn't it be nice if we could instead make use of tail-recursion in the case
where a `co_yield childGenerator;` statement occurs in the tail position to
allow the parent coroutine frame to be freed earlier?

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

There are alternative syntaxes we could implement while staying within the current
wording of the TS.

Alternative Syntax 1: Overload `co_yield` with a `tail_call()` helper function.
```c++
recursive_generator<T> traverse_tree_alternative1(tree<T>* t)
{
  if (t->left) co_yield traverse_tree_alternative1(t->left);
  co_yield t->value;
  if (t->right) co_yield tail_call(traverse_tree_alternative1(t->right));
}
```

This has the downside of requiring the more verbose syntax.

It may also be difficult for the compiler to determine that execution does
not continue after the `co_yield tail_call(...)` expression which could
suppress warnings about dead-code, etc. that would otherwise be possible
were we using `co_return`.

Alternative 2: Remove `return_void()` and keep only `return_value(recursive_generator<T>&&)`
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

This has the downside of making simple generator coroutines more verbose
as it forces every `recursive_generator` coroutine to have a `co_return`
statement, not just the ones that make use of tail-recursion.

For another motivating example, consider a function that concatenates two sequences.
```c++
template<typename T>
recursive_generator<T> concat(recursive_generator<T> first, recursive_generator<T> second)
{
  co_yield first;
  co_return std::move(second);
}

recursive_generator<int> a();
recursive_generator<int> b();
recursive_generator<int> c();

void usage()
{
  for (int x : concat(a(), concat(b(), c())))
  {
    std::cout << x << std::endl;
  }
}
```

The use of `co_return` as a tail-recursive return causes the current coroutine frame to be
destroyed, which in turn calls the destructor on the `first` generator, allowing its coroutine
frame to be freed.

# Recursive Task

A similar use-case it to support tail-recursive `co_return` statements for async `task<T>`
objects.

We can already support both `co_return someValue;` and `co_return someTask;` in a `task<T>`
if `T` is not void by implementing both `return_value(T result)` and `return_value(task<T> result)`
on the promise type.

For example:
```c++
task<int> bar();
task<int> foo()
{
  if (someCond) co_return 123;

  // Tail-recursive call.
  co_return bar(); 
}
```

However, we can't currently support tail-recursive behaviour for `task<void>` since
that would require both `return_void()` and `return_value(task<void> task)` which
is currently banned.

For example, we can't currently do this:
```c++
task<void> bar();
task<void> foo()
{
  if (someCond) co_return;

  // Tail-recursive call.
  co_return bar();
}
```

# Final Words

The author of the coroutine promise type has full control over whether or not to
support `co_return;` or `co_return value;` and can define semantics that are
meaningful for their particular coroutine-type.

TODO: A final conclusion.
