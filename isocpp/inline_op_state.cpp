#include <concepts>
#include <type_traits>
#include <atomic>
#include <new>
#include <thread>
#include <mutex>
#include <utility>
#include <functional>

namespace std::execution
{
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
    protected:
        explicit inlinable_operation_state(Rcvr&&) noexcept {}

        Rcvr get_receiver() noexcept { return Rcvr::make_receiver_for(static_cast<Op*>(this)); }
    };
#endif

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

    template<typename ParentOp, typename ChildTag, typename Env, typename Child>
    struct _manual_child_operation {
        struct _child_receiver {
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
                parent_->_set_stopped(ChildTag{});
            }

        private:
            friend _manual_child_operation;
            explicit _child_receiver(ParentOp* parent) noexcept : parent_(parent) {}

            ParentOp* parent_;
        };

    protected:
        static constexpr bool _is_nothrow_connectable = execution::is_nothrow_connectable_v<Child, _child_receiver>;

        void _start() & noexcept {
            execution::start(_get());
        }

        void _construct(Child&& child) noexcept(_is_nothrow_connectable) {
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
}

#include <print>

namespace stdex = std::execution;

struct print_rcvr {
    template<typename ChildOp>
    static print_rcvr make_receiver_for(ChildOp*) noexcept {
        return {};
    }

    stdex::empty_env get_env() const noexcept { return {}; }

    template<typename... Vs>
    void set_value(Vs&&... vs) noexcept {
        std::print("set_value(");
        (std::print("{} ", vs), ...);
        std::print(")\n");
    }

    template<typename E>
    void set_error(E&& e) noexcept {
        std::print("set_error()\n");
    }
};

int main() {
    auto a = stdex::just();
    auto b = stdex::just(1, 2, 3);

    auto c = stdex::then(a, [] noexcept {});
    auto d = stdex::then(b, [] (int a, int b, int c) noexcept { return a + b + c; });

    auto op1 = stdex::connect(a, print_rcvr{});
    stdex::start(op1);
    std::print("op1 size = {}\n", sizeof(op1));

    auto op2 = stdex::connect(b, print_rcvr{});
    stdex::start(op2);
    std::print("op2 size = {}\n", sizeof(op2));

    auto op3 = stdex::connect(c, print_rcvr{});
    stdex::start(op3);
    std::print("op3 size = {}\n", sizeof(op3));

    auto op4 =  stdex::connect(d, print_rcvr{});
    stdex::start(op4);
    std::print("op4 size = {}\n", sizeof(op4));
}
