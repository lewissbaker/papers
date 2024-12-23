#include <concepts>
#include <type_traits>
#include <atomic>
#include <new>
#include <thread>
#include <mutex>
#include <utility>
#include <functional>
#include <condition_variable>
#include <cassert>
#include <exception>

namespace std {
  class single_inplace_stop_token;
    template<typename CB>
    class single_inplace_stop_callback;

    class single_inplace_stop_source {
    public:
      single_inplace_stop_source() noexcept : state_(no_callback_state()) {}

      bool request_stop() noexcept;
      bool stop_requested() const noexcept;

      single_inplace_stop_token get_token() const noexcept;

    private:
      template<typename CB>
      friend class single_inplace_stop_callback;

      struct callback_base {
	void(*execute)(callback_base* self) noexcept;
      };

      bool try_register_callback(callback_base* cb) const noexcept;
      void deregister_callback(callback_base* cb) const noexcept;

      void* stop_requested_state() const noexcept {
	return &state_;
      }
      void* stop_requested_callback_done_state() const noexcept {
	return &thread_requesting_stop_;
      }
      static void* no_callback_state() noexcept {
	return nullptr;
      }

      bool is_stop_requested_state(void* state) const noexcept {
#if 1
        bool result = (state == stop_requested_state());
        result |= (state == stop_requested_callback_done_state());
        return result;
#else
        return state == stop_requested_state() || state == stop_requested_callback_done_state();
#endif
      }

      // nullptr                 - no stop-request or stop-callback
      // &state_                 - stop-requested
      // &thread_requesting_stop - stop-requested, callback-done
      // other                   - pointer to callback_base
      mutable atomic<void*> state_;
      mutable atomic<thread::id> thread_requesting_stop_;
    };

    inline bool single_inplace_stop_source::stop_requested() const noexcept {
      void* state = state_.load(std::memory_order_acquire);
      return is_stop_requested_state(state);
    }

    class single_inplace_stop_token {
    public:
      template<typename CB>
      using callback_type = single_inplace_stop_callback<CB>;

      single_inplace_stop_token() noexcept : source_(nullptr) {}

      bool stop_possible() const noexcept { return source_ != nullptr; }
      bool stop_requested() const noexcept { return stop_possible() && source_->stop_requested(); }

        friend bool operator==(single_inplace_stop_token, single_inplace_stop_token) noexcept = default;

    private:
      friend single_inplace_stop_source;
      template<typename CB>
      friend class single_inplace_stop_callback;

      explicit single_inplace_stop_token(const single_inplace_stop_source* source) noexcept
	: source_(source)
      {}

      const single_inplace_stop_source* source_;
    };

    template<typename CB>
    class single_inplace_stop_callback
      : private single_inplace_stop_source::callback_base {
    public:
      template<typename Init>
      requires std::constructible_from<CB, Init>
      single_inplace_stop_callback(single_inplace_stop_token st, Init&& init)
	noexcept(is_nothrow_constructible_v<CB, Init>)
	: source_(st.source_)
	, callback_(std::forward<Init>(init))
      {
	this->execute = &execute_impl;
	if (source_ != nullptr) {
	  if (!source_->try_register_callback(this)) {
	    source_ = nullptr;
	    execute_impl(this);
	  }
	}
      }

      ~single_inplace_stop_callback() {
	if (source_ != nullptr) {
	  source_->deregister_callback(this);
	}
      }

      single_inplace_stop_callback(single_inplace_stop_callback&&) = delete;
      single_inplace_stop_callback(const single_inplace_stop_callback&) = delete;
      single_inplace_stop_callback& operator=(single_inplace_stop_callback&&) = delete;
      single_inplace_stop_callback& operator=(const single_inplace_stop_callback&) = delete;

    private:
      static void execute_impl(single_inplace_stop_source::callback_base* base) noexcept {
	auto& self = *static_cast<single_inplace_stop_callback*>(base);
	self.callback_();
      }

      const single_inplace_stop_source* source_;
      CB callback_;
    };

    template<typename CB>
    single_inplace_stop_callback(single_inplace_stop_token, CB) -> single_inplace_stop_callback<CB>;

    inline single_inplace_stop_token single_inplace_stop_source::get_token() const noexcept {
        return single_inplace_stop_token{this};
    }

    __attribute__((noinline))
    inline bool single_inplace_stop_source::request_stop() noexcept {
      void* old_state = state_.load(std::memory_order_relaxed);
      do {
	if (is_stop_requested_state(old_state)) {
	  return false;
	}
      } while (!state_.compare_exchange_weak(old_state,
					     stop_requested_state(),
					     memory_order_acq_rel,
					     memory_order_relaxed));

      if (old_state != no_callback_state()) {
	auto* callback = static_cast<callback_base*>(old_state);
	thread_requesting_stop_.store(this_thread::get_id(), memory_order_relaxed);

	callback->execute(callback);

	state_.store(stop_requested_callback_done_state(), memory_order_release);
	state_.notify_one();
      }

      return true;
    }

    __attribute__((noinline))
    inline bool single_inplace_stop_source::try_register_callback(callback_base* base) const noexcept {
      void* old_state = state_.load(memory_order_acquire);
      if (is_stop_requested_state(old_state)) {
	return false;
      }

      assert(old_state == no_callback_state());      

      if (state_.compare_exchange_strong(old_state,
					 static_cast<void*>(base),
					 memory_order_release,
					 memory_order_acquire)) {
	// Successfully registered callback.
	return true;
      }

      // Stop request arrived while we were trying to register
      assert(old_state == stop_requested_state());

      return false;
    }

    __attribute__((noinline))
    inline void single_inplace_stop_source::deregister_callback(callback_base* base) const noexcept {
      // Initially assume that the callback has not been invoked and that the state
      // still points to the registered callback_base structure.
      void* old_state = static_cast<void*>(base);
      if (state_.compare_exchange_strong(old_state,
					 no_callback_state(),
					 memory_order_relaxed,
					 memory_order_acquire)) {
	// Successfully deregistered the callback before it could be invoked.
	return;
      }

      // Otherwise, a call to request_stop() is invoking the callback.
      if (old_state == stop_requested_state()) {
	// Callback not finished executing yet.
	if (thread_requesting_stop_.load(std::memory_order_relaxed) ==
	    std::this_thread::get_id()) {
	  // Deregistering from the same thread that is invoking the callback.
	  // Either the invocation of the callback has completed and the thread
	  // has gone on to do other things (in which case it's safe to destroy)
	  // or we are still in the middle of executing the callback (in which
	  // case we can't block as it would cause a deadlock).
	  return;
	}

	// Otherwise, callback is being called from another thread.
	// Wait for callback to finish (state changes from stop_requested_state()
	// to stop_requested_callback_done_state()).
	state_.wait(old_state, memory_order_acquire);
      }
    }
  }

namespace std
{
    template<template<class> class>
    struct _check_type_alias_exists;

    template<typename T>
    concept stoppable_token =
        requires (const T tok) {
            typename _check_type_alias_exists<T::template callback_type>;
            { tok.stop_requested() } noexcept -> same_as<bool>;
            { tok.stop_possible() } noexcept -> same_as<bool>;
            { T(tok) } noexcept;
        } &&
        copyable<T> &&
        equality_comparable<T>;
        
    template<typename T>
    concept unstoppable_token =
        stoppable_token<T> &&
        requires {
            requires bool_constant<(!T::stop_possible())>::value;
        };

    template<typename Class, typename Member>
    using _member_t = decltype(std::forward_like<Class>(std::declval<Member>()));

    template<typename T, typename... Args>
    concept _callable =
        requires(T obj, Args... args) {
            std::forward<T>(obj)(std::forward<Args>(args)...);
        };

    template<class T>
    concept queryable =
        destructible<T>;

    template<class T, class Query>
    concept _has_query =
        queryable<T> &&
        requires(T&& obj) {
            static_cast<T&&>(obj).query(Query{});
        };

    template<class T, class Query>
    concept _has_nothrow_query =
        queryable<T> &&
        _has_query<T, Query> &&
        requires(T&& obj) {
            { static_cast<T&&>(obj).query(Query{}) } noexcept;
        };

    struct never_stop_token {
        struct stop_callback {
            stop_callback(never_stop_token, auto&&) noexcept {}
        };
        template<typename CB>
        using callback_type = stop_callback;

        static constexpr bool stop_possible() noexcept { return false; }
        static constexpr bool stop_requested() noexcept { return false; }

        constexpr friend bool operator==(never_stop_token, never_stop_token) noexcept { return true; }
    };

    static_assert(unstoppable_token<never_stop_token>);

    struct get_stop_token_t {
        template<_has_query<get_stop_token_t> Obj>
        static decltype(auto) operator()(Obj&& obj) noexcept(_has_nothrow_query<Obj, get_stop_token_t>) {
            return std::forward<Obj>(obj).query(get_stop_token_t{});
        }

        template<queryable Obj>
        static never_stop_token operator()(const Obj&) noexcept {
            return {};
        }
    };
    inline constexpr get_stop_token_t get_stop_token;

    template<typename T>
    using stop_token_of_t = decltype(std::get_stop_token(std::declval<T>()));

    template<typename T, typename CallbackFn>
    using stop_callback_for_t = T::template callback_type<CallbackFn>;
}

namespace std::execution
{
    struct empty_env {};

    struct get_env_t {
        template<typename Obj>
        requires requires (const Obj& obj) {
            { obj.get_env() } -> queryable;
        }
        static decltype(auto) operator()(const Obj& obj) noexcept {
            return obj.get_env();
        }

        template<typename Obj>
        static empty_env operator()(const Obj&) noexcept {
            return {};
        }
    };
    inline constexpr get_env_t get_env{};

    struct receiver_t {};

    template<class Rcvr>
    concept receiver =
      derived_from<typename remove_cvref_t<Rcvr>::receiver_concept, receiver_t> &&
      requires(const remove_cvref_t<Rcvr>& rcvr) {
        { get_env(rcvr) } -> queryable;
      } &&
      move_constructible<remove_cvref_t<Rcvr>> &&       // rvalues are movable, and
      constructible_from<remove_cvref_t<Rcvr>, Rcvr>;   // lvalues are copyable

    template<class Rcvr, class ChildOp>
    concept inlinable_receiver =
        receiver<Rcvr> &&
        requires (ChildOp* op) {
            { Rcvr::make_receiver_for(op) } noexcept -> same_as<Rcvr>;
        };

    template<class Op, class Rcvr>
    struct inlinable_operation_state {
        explicit inlinable_operation_state(Rcvr&& r) noexcept(is_nothrow_move_constructible_v<Rcvr>)
        : rcvr_(std::move(r))
        {}

        Rcvr& get_receiver() noexcept { return rcvr_; }

    private:
        Rcvr rcvr_;
    };

#if ENABLE_OPTIMISATION
    template<class Op, class Rcvr>
    requires inlinable_receiver<Rcvr, Op>
    struct inlinable_operation_state<Op, Rcvr> {
        explicit inlinable_operation_state(Rcvr&&) noexcept {}

        Rcvr get_receiver() noexcept { return Rcvr::make_receiver_for(static_cast<Op*>(this)); }
    };
#endif

    template<typename T>
    using env_of_t = decltype(execution::get_env(std::declval<T>()));

    template<queryable Env>
    auto _fwd_env(Env&& env) {
        // TODO: Actually filter env to forwardable queries.
        return std::forward<Env>(env);
    }

    struct set_value_t {
        template<typename Rcvr, typename... Vs>
        requires requires(Rcvr rcvr, Vs... vs) {
            { std::forward<Rcvr>(rcvr).set_value(std::forward<Vs>(vs)...) } -> same_as<void>;
        }
        static void operator()(Rcvr&& rcvr, Vs&&... vs) noexcept {
            std::forward<Rcvr>(rcvr).set_value(std::forward<Vs>(vs)...);
        }
    };
    inline constexpr set_value_t set_value{};

    struct set_error_t {
        template<typename Rcvr, typename E>
        requires requires(Rcvr rcvr, E e) {
            { std::forward<Rcvr>(rcvr).set_error(std::forward<E>(e)) } -> same_as<void>;
        }
        static void operator()(Rcvr&& rcvr, E&& e) noexcept {
            std::forward<Rcvr>(rcvr).set_error(std::forward<E>(e));
        }
    };
    inline constexpr set_error_t set_error{};

    struct set_stopped_t {
        template<typename Rcvr>
        requires requires(Rcvr rcvr) {
            { std::forward<Rcvr>(rcvr).set_stopped() } -> same_as<void>;
        }
        static void operator()(Rcvr&& rcvr) noexcept {
            std::forward<Rcvr>(rcvr).set_stopped();
        }
    };
    inline constexpr set_stopped_t set_stopped{};

    struct start_t {
        template<typename Op>
        requires requires(Op& op) {
            { op.start() } noexcept -> same_as<void>;
        }
        static void operator()(Op& op) noexcept {
            op.start();
        }
    };
    inline constexpr start_t start{};

    template<typename Op>
    concept operation_state =
        destructible<Op> &&
        std::invocable<start_t, Op&>;

    struct connect_t {
        template<class Sndr, class Rcvr>
        requires requires(Sndr&& sndr, Rcvr&& rcvr) {
            std::forward<Sndr>(sndr).connect(std::forward<Rcvr>(rcvr));
        }
        static operation_state auto operator()(Sndr&& sndr, Rcvr&& rcvr)
            noexcept(noexcept(std::forward<Sndr>(sndr).connect(std::forward<Rcvr>(rcvr)))) {
            return std::forward<Sndr>(sndr).connect(std::forward<Rcvr>(rcvr));
        }
    };
    inline constexpr connect_t connect{};

    template<typename Sndr>
    concept sender =
        move_constructible<Sndr>;

    template<typename Sndr, typename Rcvr>
    concept sender_to =
        sender<Sndr> &&
        receiver<Rcvr> &&
        _callable<connect_t, Sndr, Rcvr>;

    template<typename Sndr, typename Rcvr>
    using connect_result_t = decltype(execution::connect(std::declval<Sndr>(), std::declval<Rcvr>()));

    template<typename Sndr, typename Rcvr>
    inline constexpr bool is_nothrow_connectable_v = false;

    template<typename Sndr, typename Rcvr>
    requires sender_to<Sndr, Rcvr>
    inline constexpr bool is_nothrow_connectable_v<Sndr, Rcvr> =
        noexcept(execution::connect(std::declval<Sndr>(), std::declval<Rcvr>()));

    struct schedule_t {
        template<typename Scheduler>
        requires requires (Scheduler sched) {
            sched.schedule();
        }
        static sender auto operator()(Scheduler&& sched) {
            return std::forward<Scheduler>(sched).schedule();
        }
    };
    inline constexpr schedule_t schedule{};

    template<typename T>
    concept scheduler =
        requires(const T sched) {
            execution::schedule(std::forward<T>(sched));
        } &&
        copy_constructible<T> &&
        equality_comparable<T>;

    struct get_scheduler_t {
        template<queryable Env>
        static scheduler auto operator()(const Env& env)
            noexcept(_has_nothrow_query<const Env&, get_scheduler_t>) {
            return env.query(get_scheduler_t{});
        }
    };
    inline constexpr get_scheduler_t get_scheduler{};

    template<typename ParentOp, typename ChildTag, typename Env, typename Child>
    struct _manual_child_operation {
        struct _child_receiver {
            using receiver_concept = receiver_t;

            template<typename ChildOp>
            static _child_receiver make_receiver_for(ChildOp* child) noexcept {
                static_assert(std::same_as<ChildOp, child_op_t>);
                auto* parent = static_cast<ParentOp*>(
                        reinterpret_cast<_manual_child_operation*>(
                            reinterpret_cast<storage_t*>(child)));
                return _child_receiver{parent};
            }

            Env get_env() const noexcept {
                return parent_->_get_env(ChildTag{});
            }

            template<typename... Vs>
            void set_value(Vs&&... vs) noexcept {
                parent_->_complete(ChildTag{}, set_value_t{}, std::forward<Vs>(vs)...);
            }

            template<typename E>
            void set_error(E&& e) noexcept {
                parent_->_complete(ChildTag{}, set_error_t{}, std::forward<E>(e));
            }

            void set_stopped() noexcept{
                parent_->_complete(ChildTag{}, set_stopped_t{});
            }

        private:
            friend _manual_child_operation;
            explicit _child_receiver(ParentOp* parent) noexcept : parent_(parent) {}

            ParentOp* parent_;
        };

    protected:
        static constexpr bool _is_nothrow_connectable = execution::is_nothrow_connectable_v<Child, _child_receiver>;

        _manual_child_operation() noexcept {}
        ~_manual_child_operation() {}

        void _start() & noexcept {
            execution::start(_get());
        }

        void _construct(Child&& child) noexcept(_is_nothrow_connectable) {
            // TODO: Check if this can be done in constexpr
            ParentOp* parent = static_cast<ParentOp*>(this);
            ::new (static_cast<void*>(std::addressof(storage_))) child_op_t(
                execution::connect(std::forward<Child>(child), _child_receiver{parent}));
        }

        void _destruct() noexcept {
            _get().~child_op_t();
        }

    private:
        using child_op_t = connect_result_t<Child, _child_receiver>;

        child_op_t& _get() & noexcept {
            return *std::launder(reinterpret_cast<child_op_t*>(std::addressof(storage_)));
        }

        using storage_t = std::conditional_t<
            is_empty_v<child_op_t>,
            child_op_t,
            unsigned char[sizeof(child_op_t)]>;
        union {
            [[no_unique_address]] alignas(child_op_t) storage_t storage_;
        };
    };

    template<typename ParentOp, typename ChildTag, typename Env, typename Child>
    struct _child_operation : public _manual_child_operation<ParentOp, ChildTag, Env, Child> {
    private:
        using base_t = _manual_child_operation<ParentOp, ChildTag, Env, Child>;
        using base_t::_construct;
        using base_t::_destruct;
    protected:
        _child_operation(Child&& child) noexcept(base_t::_is_nothrow_connectable) {
            base_t::_construct(std::forward<Child>(child));
        }

        ~_child_operation() {
            base_t::_destruct();
        }
    };

    struct _source_tag {};

    template<std::size_t Idx>
    struct _indexed_tag {};

    template<class ParentOp, typename ParentEnv, template<typename, std::size_t> class ChildEnv, class Indices, class... Child>
    struct _manual_child_operations;

    template<class ParentOp, typename ParentEnv, template<typename, std::size_t> class ChildEnv, std::size_t... Ids, class... Child>
    requires (sizeof...(Ids) == sizeof...(Child))
    struct _manual_child_operations<ParentOp, ParentEnv, ChildEnv, std::index_sequence<Ids...>, Child...>
        : _manual_child_operation<ParentOp, _indexed_tag<Ids>, ChildEnv<ParentEnv, Ids>, Child>... {
    protected:
        template<std::size_t Id>
        using child_t = _manual_child_operation<ParentOp, _indexed_tag<Id>, ChildEnv<ParentEnv, Id>, Child...[Id]>;

    private:
        template<std::size_t Id>
        struct _construct_helper {
            _manual_child_operations& self;
            bool& succeeded;

            _construct_helper(_manual_child_operations& self, bool& succeeded, Child...[Id]&& child)
            : self(self), succeeded(succeeded) {
                self.child_t<Id>::_construct(std::forward<Child...[Id]&&>(child));
            }

            ~_construct_helper() {
                if (!succeeded) {
                    self.child_t<Id>::_destruct();
                }
            }
        };

    public:
        void _construct(Child&&... child) noexcept((child_t<Ids>::_is_nothrow_connectable && ...)) {
            bool succeeded = false;
            ((_construct_helper<Ids>{*this, succeeded, std::forward<Child>(child)}, ...), succeeded = true);
        }

        void _destruct() noexcept {
            (child_t<Ids>::_destruct(), ...);
        }

        void _start_all() noexcept {
            (child_t<Ids>::_start(), ...);
        }
    };

    template<class Tag, class Rcvr, class State, class... Child>
    struct _basic_operation 
        : inlinable_operation_state<_basic_operation<Tag, Rcvr, State, Child...>, Rcvr>
        , State
        , _manual_child_operations<
            _basic_operation<Tag, Rcvr, State, Child...>,
            env_of_t<Rcvr>,
            State::template _env_type,
            std::index_sequence_for<Child...>,
            Child...> {
    private:
        using children_t = _manual_child_operations<
            _basic_operation<Tag, Rcvr, State, Child...>,
            env_of_t<Rcvr>,
            State::template _env_type,
            std::index_sequence_for<Child...>,
            Child...>;

        using State::_start;

    public:
        friend State;

        template<class Data>
        requires constructible_from<State, Data>
        _basic_operation(Rcvr&& rcvr, Data&& data, Child&&... child)
        : inlinable_operation_state<_basic_operation, Rcvr>(std::move(rcvr))
        , State(std::forward<Data>(data)) {
            children_t::_construct(std::forward<Child>(child)...);
        }

        ~_basic_operation() {
            children_t::_destruct();
        }

        void start() noexcept {
            this->_start();
        }
    };

    struct _default_state_impls {
        template<typename ParentEnv, std::size_t Id>
        using _env_type = ParentEnv;

        template<typename Self>
        void _start(this Self& self) noexcept {
            self._start_all();
        }

        template<typename Self, std::size_t Id>
        decltype(auto) _get_env(this Self& self, _indexed_tag<Id>) noexcept {
            return execution::get_env(self.get_receiver());
        }

        template<typename Self, std::size_t Id, typename CompletionTag, typename... Datums>
        void _complete(this Self& self, _indexed_tag<Id>, CompletionTag, Datums&&... datums) noexcept {
            return CompletionTag{}(self.get_receiver(), std::forward<Datums>(datums)...);
        }
    };

    template<typename Tag>
    struct _state_impls_for : _default_state_impls {};

    template<typename Tag, typename Data>
    struct _default_state : _state_impls_for<Tag> {
        template<typename Data2>
        requires std::constructible_from<Data, Data2>
        _default_state(Data2&& data2) : data(std::forward<Data2>(data2)) {}

        [[no_unique_address]] Data data;
    };

    struct _default_sender_impls {
        template<typename Data, typename... Child>
        static decltype(auto) _get_attrs(Data& data, Child&... child) noexcept {
            if constexpr (sizeof...(Child) == 1) {
                return _fwd_env(execution::get_env(child...[0]));
            } else {
                return empty_env{};
            }
        }

        template<typename Tag, typename Data>
        static _default_state<Tag, std::decay_t<Data>> _get_state(Tag, Data&& data) noexcept {
            return {{}, std::forward<Data>(data)};
        }
    };

    template<typename Tag>
    struct _sender_impls_for : _default_sender_impls {};

    template<typename Tag, typename Data>
    using _state_type_t = decltype(_sender_impls_for<Tag>::_get_state(Tag{}, std::declval<Data>()));

    template<class Tag, class Data, class... Child>
    struct _basic_sender {
        [[no_unique_address]] Tag tag;
        [[no_unique_address]] Data data;
        [[no_unique_address]] std::tuple<Child...> children;

        decltype(auto) get_env() const noexcept {
            return std::apply([&](auto&... children_pack) noexcept {
                return _sender_impls_for<Tag>::_get_attrs(data, children_pack...);
            }, children);
        }

        template<typename Self, typename Rcvr>
        _basic_operation<Tag, Rcvr, _state_type_t<Tag, _member_t<Self, Data>>, _member_t<Self, Child>...> connect(this Self&& self, Rcvr rcvr)
            noexcept(is_nothrow_constructible_v<
                _basic_operation<Tag, Rcvr, _state_type_t<Tag, _member_t<Self, Data>>, _member_t<Self, Child>...>,
                Rcvr,
                _member_t<Self, Data>,
                _member_t<Self, Child>...>) {
            return std::apply([&](_member_t<Self, Child>... children) {
                return _basic_operation<Tag, Rcvr, _state_type_t<Tag, _member_t<Self, Data>>, _member_t<Self, Child>...>{
                    std::move(rcvr),
                    std::forward<Self>(self).data,
                    std::forward<decltype(children)>(children)...};
            }, std::forward<Self>(self).children);
        }
    };

    struct then_t {
        template<typename Source, typename Func>
        static _basic_sender<then_t, std::decay_t<Func>, std::remove_cvref_t<Source>> operator()(Source&& src, Func&& func) {
            return {then_t{}, std::forward<Func>(func), std::forward<Source>(src)};
        }
    };
    inline constexpr then_t then{};

    template<>
    struct _state_impls_for<then_t> : _default_state_impls {
        template<typename Self, std::size_t Id, typename CompletionTag, typename... Datums>
        void _complete(this Self& self, _indexed_tag<Id>, CompletionTag, Datums&&... datums) noexcept {
            using Func = decltype(Self::data);
            if constexpr (std::same_as<CompletionTag, set_value_t>) {
                using result_t = std::invoke_result_t<Func, Datums...>;
                constexpr bool is_nothrow = std::is_nothrow_invocable_v<std::invoke_result_t<Func, Datums...>>;
                try {
                    if constexpr (std::is_void_v<result_t>) {
                        std::invoke(std::forward<Func>(self.data), std::forward<Datums>(datums)...);
                        execution::set_value(self.get_receiver());
                    } else {
                        execution::set_value(
                            self.get_receiver(),
                            std::invoke(std::forward<Func>(self.data), std::forward<Datums>(datums)...));
                    }
                } catch (...) {
                    if constexpr (!is_nothrow) {
                        execution::set_error(self.get_receiver(), std::current_exception());
                    }
                }
            } else {
                return CompletionTag{}(self.get_receiver(), std::forward<Datums>(datums)...);
            }
        }
    };

    struct just_t {
        template<typename... Vs>
        static _basic_sender<just_t, std::tuple<std::decay_t<Vs>...>> operator()(Vs&&... vs) {
            return {just_t{}, std::make_tuple(std::forward<Vs>(vs)...)};
        }
    };
    inline constexpr just_t just{};

    template<>
    struct _state_impls_for<just_t> : _default_state_impls {
        template<typename Self>
        void _start(this Self& self) noexcept {
            std::apply([&](auto&&... vs) noexcept {
                execution::set_value(self.get_receiver(), static_cast<decltype(vs)>(vs)...);
            }, std::move(self.data));
        }
    };

    class run_loop {
        struct _operation_node {
            _operation_node* next;
            _operation_node* prev;
        };
        struct _operation_base : _operation_node {
            virtual void _execute() noexcept = 0;
        };
        
        template<typename Rcvr>
        struct _schedule_op final
                : inlinable_operation_state<_schedule_op<Rcvr>, Rcvr>
                , private _operation_base {
            _schedule_op(run_loop* loop, Rcvr rcvr)
            : inlinable_operation_state<_schedule_op, Rcvr>(std::move(rcvr))
            , loop_(loop)
            {}

            void start() & noexcept {
                auto st = get_stop_token(get_env(this->get_receiver()));
                const bool stop_possible = st.stop_possible();
                if (stop_possible) {
                    loop_->_enqueue(this);
                    stop_callback_.emplace(std::move(st), on_stop{*this});
                    state_t old_state = state_.fetch_add(started_flag, std::memory_order_acq_rel);
                    if ((old_state & completed_flag) != 0) {
                        // TODO: Use atomic notify tokens here when available.
                        state_.notify_one();
                    } else if (old_state == stop_requested_flag) {
                        stop_callback_.reset();
                        execution::set_stopped(this->get_receiver());
                    }
                } else {
                    state_.store(started_flag, std::memory_order_relaxed);
                    loop_->_enqueue(this);
                }
            }

            void _execute() noexcept override {
                auto state = state_.load(std::memory_order_acquire);
                if ((state & started_flag) == 0) {
                    state = state_.fetch_add(completed_flag, std::memory_order_acq_rel);
                    while ((state & started_flag) == 0) {
                        state_.wait(state);
                    }
                }

                stop_callback_.reset();

                execution::set_value(this->get_receiver());
            }

            void _request_stop() noexcept {
                state_t old_state = state_.fetch_add(stop_requested_flag, std::memory_order_acq_rel);
                if (old_state == started_flag) {
                    if (loop_->_try_remove(this)) {
                        // If we get here then we know _execute() will not be called
                        // so it is safe to destroy the callback and complete.
                        stop_callback_.reset();
                        execution::set_stopped(this->get_receiver());
                    }
                }
            }

            struct on_stop {
                _schedule_op& op;
                void operator()() noexcept {
                    op._request_stop();
                }
            };

            using stop_callback = stop_callback_for_t<stop_token_of_t<env_of_t<Rcvr>>, on_stop>;
            using state_t = std::uint8_t;
            static constexpr state_t started_flag = 1;
            static constexpr state_t completed_flag = 2;
            static constexpr state_t stop_requested_flag = 4;

            run_loop* loop_;
            std::optional<stop_callback> stop_callback_;
            std::atomic<state_t> state_{0};
        };

        template<typename Rcvr>
        requires unstoppable_token<stop_token_of_t<env_of_t<Rcvr>>>
        struct _schedule_op<Rcvr> final : inlinable_operation_state<_schedule_op<Rcvr>, Rcvr>, private _operation_base {
            _schedule_op(run_loop* loop, Rcvr rcvr)
            : inlinable_operation_state<_schedule_op, Rcvr>(std::move(rcvr))
            , loop_(loop)
            {}

            void start() & noexcept {
                loop_->_enqueue(this);
            }

            void _execute() noexcept override {
                execution::set_value(this->get_receiver());
            }

            run_loop* loop_;
        };

    public:
        struct scheduler;

    private:
        struct schedule_sender {
            friend bool operator==(const schedule_sender&, const schedule_sender&) noexcept = default;

            template<typename Rcvr>
            _schedule_op<Rcvr> connect(Rcvr rcvr) const noexcept {
                return _schedule_op<Rcvr>(loop_, std::move(rcvr));;
            }

        private:
            friend scheduler;
            explicit schedule_sender(run_loop*loop) noexcept : loop_(loop) {}
            run_loop* loop_;
        };

    public:
        struct scheduler {
            schedule_sender schedule() const noexcept {
                return schedule_sender{loop_};
            }
        
            friend bool operator==(scheduler a, scheduler b) noexcept = default;

        private:
            friend run_loop;
            explicit scheduler(run_loop* loop) noexcept : loop_(loop) {}
            run_loop* loop_;
        };

        __attribute__((noinline))
        run_loop() {
            head_.next = &head_;
            head_.prev = &head_;
        }

        __attribute__((noinline))
        ~run_loop() {
            assert(_empty());
        }

        scheduler get_scheduler() noexcept {
            return scheduler{this};
        }

        __attribute__((noinline))
        void finish() {
            std::lock_guard lk{mut_};
            finish_ = true;
            if (_empty()) {
                cv_.notify_one();
            }
        }

        __attribute__((noinline))
        void run() {
            while (auto* op = _dequeue()) {
                op->_execute();
            }
        }

    private:
        bool _empty() const noexcept {
            return head_.next == &head_;
        }

        __attribute__((noinline))
        void _enqueue(_operation_base* op) {
            std::lock_guard lk{mut_};
            const bool notify = _empty();
            op->prev = head_.prev;
            op->next = &head_;
            head_.prev->next = op;
            head_.prev = op;
            if (notify) {
                cv_.notify_one();
            }
        }

        __attribute__((noinline))
        bool _try_remove(_operation_base* op) {
            std::lock_guard lk{mut_};
            if (op->next == nullptr) {
                // already dequeued
                return false;
            }

            op->next->prev = op->prev;
            op->prev->next = op->next;
            op->next = nullptr;
            op->prev = nullptr;

            return true;
        }

        __attribute__((noinline))
        _operation_base* _dequeue() {
            std::unique_lock lk{mut_};

            while (true) {
                if (_empty()) {
                    if (finish_) {
                        return nullptr;
                    }
                    cv_.wait(lk);
                } else {
                    auto* op = head_.next;
                    assert(op->prev == &head_);
                    op->next->prev = &head_;
                    head_.next = op->next;
                    op->next = nullptr;
                    op->prev = nullptr;
                    return static_cast<_operation_base*>(op);
                }
            }
        }

        std::mutex mut_;
        std::condition_variable cv_;
        _operation_node head_;
        bool finish_{false};
    };

    template<typename StopToken>
    struct _sync_wait_state_base {
        explicit _sync_wait_state_base(StopToken st) noexcept
        : stop_token(std::move(st))
        {}

        run_loop loop;
        [[no_unique_address]] StopToken stop_token;
    };

    template<typename StopToken>
    struct _sync_wait_env {
        _sync_wait_state_base<StopToken>* state_;

        StopToken query(get_stop_token_t) const noexcept {
            return state_->stop_token;
        }

        run_loop::scheduler query(get_scheduler_t) const noexcept {
            return state_->loop.get_scheduler();
        }
    };

    template<typename Sndr, typename StopToken>
    struct _sync_wait_state
        : _sync_wait_state_base<StopToken>
        , _child_operation<
            _sync_wait_state<Sndr, StopToken>,
            _source_tag,
            _sync_wait_env<StopToken>,
            Sndr> {
        using child_t = _child_operation<_sync_wait_state, _source_tag, _sync_wait_env<StopToken>, Sndr>;

        _sync_wait_state(Sndr&& sndr, StopToken st)
        : _sync_wait_state_base<StopToken>(std::move(st))
        ,  child_t(std::forward<Sndr>(sndr))
        {}

        std::optional<int> run() {
            child_t::_start();
            this->loop.run();
            switch (result.index())  {
                case 0: return std::nullopt;
                case 1: return std::get<1>(result);
                case 2: std::rethrow_exception(std::get<2>(result));
                default: std::unreachable();
            }
        }

        void _complete(_source_tag, set_value_t, int x) noexcept {
            result.template emplace<1>(x);
            this->loop.finish();
        }

        void _complete(_source_tag, set_error_t, std::exception_ptr e) noexcept {
            result.template emplace<2>(std::move(e));
            this->loop.finish();
        }

        void _complete(_source_tag, set_stopped_t) noexcept {
            this->loop.finish();
        }

        _sync_wait_env<StopToken> _get_env(_source_tag) noexcept {
            return _sync_wait_env<StopToken>{this};
        }

        std::variant<std::monostate, int, std::exception_ptr> result;
    };

    template<sender Sndr, stoppable_token StopToken>
    __attribute__((noinline))
    std::optional<int> sync_wait(Sndr&& sndr, StopToken st) {
        _sync_wait_state<Sndr, StopToken> state{std::forward<Sndr>(sndr), std::move(st)};
        return state.run();
    }

    template<sender Sndr>
    std::optional<int> sync_wait(Sndr&& sndr) {
        return sync_wait(std::forward<Sndr>(sndr), never_stop_token{});
    }

} // namespace std::execution

#include <cstdio>

namespace stdex = std::execution;

auto make_sender(stdex::run_loop& loop, int start, int offset, int multiplier, int a) {
    return stdex::then(
                stdex::then(
                    stdex::then(
                        stdex::then(
                            loop.get_scheduler().schedule(),
                            [start]() noexcept { return start; }),
                        [=](int x) noexcept { return x * multiplier; }),
                    [=](int x) noexcept { return x + offset; }),
                [a](int x) noexcept { return a-x; });
}

int main() {
    stdex::run_loop loop;
    std::jthread thread{[&](std::stop_token st) {
        std::stop_callback cb{st, [&] { loop.finish(); }};
        loop.run();
    }};

    std::single_inplace_stop_source ss;

    const int iters = 10000;

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        auto result = stdex::sync_wait(make_sender(loop, 6, 5, 2, 1), ss.get_token());
        if (!result.has_value()) {
            std::terminate();
        }
    }
    auto end = std::chrono::steady_clock::now();

    std::printf(
        "%i iters took %u us",
        iters,
        (std::uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}
