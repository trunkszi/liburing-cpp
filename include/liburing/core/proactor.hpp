/** @file @brief One issuer stages intents and releases copied CQ batches before callbacks. */
#pragma once
#include <atomic>
#include <liburing/caps/kernel_caps.hpp>
#include <liburing/config/proactor_config.hpp>
#include <liburing/core/control_command.hpp>
#include <liburing/core/dispatcher.hpp>
#include <liburing/core/drain.hpp>
#include <liburing/core/operation_slab.hpp>
#include <liburing/ops/core.hpp>
#include <liburing/resources/resource_registry.hpp>
#include <liburing/ring/ring.hpp>
#include <liburing/sys/mmap_region.hpp>
#include <memory>
namespace liburing {
namespace detail {
struct SubmissionIntent {
  UserDataToken token{};
  std::uint32_t deferred_slot{};
  std::uint8_t count{};
  std::uint8_t subsystem_id{};
  std::uint16_t reserved{};
};
struct DeferredSqePack {
  std::array<abi::FlatSqe128, 4> images{};
  std::array<std::uint8_t, 4> widths{};
  std::uint8_t count{};
  std::array<std::uint8_t, 3> reserved{};
};
static_assert(uarch::NoPadding<SubmissionIntent> && sizeof(SubmissionIntent) == 16 && uarch::GprPair<SubmissionIntent>);
static_assert(uarch::NoPadding<DeferredSqePack> && sizeof(DeferredSqePack) == 520);
template <unsigned LogCapacity, unsigned LogDeferredCapacity>
struct ProactorArena {
  static constexpr std::size_t kCapacity = 1ULL << LogCapacity;
  static constexpr std::size_t kDeferredCapacity = 1ULL << LogDeferredCapacity;
  OperationSlab<SubmissionIntent, LogCapacity> slab{kInternalSubsystemId};
  DeferredQueue<kCapacity> deferred{};
  DeferredQueue<kCapacity> cancellations{};
  DeferredQueue<kCapacity> failed{};
  ControlQueue<kCapacity> controls{};
  DeferredQueue<kCapacity> control_order{};
  resources::ResourceRegistry resources{};
  std::array<UserDataToken, kCapacity> active{};
  std::array<std::uint32_t, 256> subsystem_in_flight{};
  std::array<uarch::Bitmap64, 4> stopped{};
  std::array<uarch::Bitmap64, 4> routable{};
  bool restrict_routable{};
  std::array<DeferredSqePack, kDeferredCapacity> deferred_packs{};
  uarch::Pool<DeferredSqePack, LogDeferredCapacity> deferred_pool{
      *uarch::Pool<DeferredSqePack, LogDeferredCapacity>::try_new(std::span<DeferredSqePack>{deferred_packs})};
  std::array<std::uint32_t, kDeferredCapacity> deferred_next{};
  std::uint32_t deferred_free{1};
  ProactorArena() noexcept {
    for (std::uint32_t i = 1; i < kDeferredCapacity; ++i) deferred_next[i] = i + 1 < kDeferredCapacity ? i + 1 : 0;
  }
};

// Ownership moves only the mapping and pointer; self-referencing pool views stay at their mapped address.
template <class Tables>
class ProactorArenaStorage {
  static_assert(alignof(Tables) <= 4096);
  sys::MmapRegion mapping_;
  Tables* tables_;
  explicit ProactorArenaStorage(sys::MmapRegion mapping) noexcept
      : mapping_(std::move(mapping)), tables_(std::construct_at(reinterpret_cast<Tables*>(mapping_.bytes().data()))) {}

 public:
  ProactorArenaStorage(const ProactorArenaStorage&) = delete ("arena ownership is unique");
  ProactorArenaStorage& operator=(const ProactorArenaStorage&) = delete ("arena ownership is unique");
  ProactorArenaStorage(ProactorArenaStorage&& other) noexcept
      : mapping_(std::move(other.mapping_)), tables_(std::exchange(other.tables_, nullptr)) {}
  ProactorArenaStorage& operator=(ProactorArenaStorage&&) = delete ("arena storage is stable after construction");
  ~ProactorArenaStorage() noexcept {
    if (tables_) std::destroy_at(tables_);
  }
  [[nodiscard]] static Result<ProactorArenaStorage> Create() noexcept {
    constexpr auto bytes = (sizeof(Tables) + 4095U) & ~std::size_t{4095U};
    auto mapping =
        sys::MmapRegion::Map(nullptr, bytes, sys::kProtRead | sys::kProtWrite, sys::kMapPrivate | sys::kMapAnonymous, -1, 0);
    if (!mapping) return std::unexpected(mapping.error());
    return ProactorArenaStorage{std::move(*mapping)};
  }
  [[nodiscard]] static ProactorArenaStorage Require() noexcept {
    auto arena = Create();
    if (!arena) sys::Panic("Proactor arena allocation failed");
    return std::move(*arena);
  }
  [[nodiscard]] Tables* operator->() noexcept { return tables_; }
  [[nodiscard]] const Tables* operator->() const noexcept { return tables_; }
};
inline std::atomic<std::uint64_t> next_ring_instance{1};
}  // namespace detail

template <class RingBackend = ring::KernelRing, unsigned LogCapacity = 11, unsigned LogDeferredCapacity = 8>
class BasicProactor final : public detail::ProactorLoops<BasicProactor<RingBackend, LogCapacity, LogDeferredCapacity>> {
  template <class>
  friend class detail::ProactorLoops;
  static_assert(LogCapacity >= 1 && LogCapacity <= 22);
  static_assert(LogDeferredCapacity >= 1 && LogDeferredCapacity <= 22);

 public:
  static constexpr unsigned kLogCapacity = LogCapacity;
  static constexpr std::size_t kDeferredCapacity = 1ULL << LogDeferredCapacity;
  static constexpr std::size_t kCapacity = 1ULL << kLogCapacity;
  static constexpr std::size_t kBatch = 16;
  using Slab = OperationSlab<detail::SubmissionIntent, kLogCapacity>;
  using Arena = detail::ProactorArena<LogCapacity, LogDeferredCapacity>;
  using ArenaStorage = detail::ProactorArenaStorage<Arena>;
  static_assert(Slab::kCapacity == kCapacity && kBatch <= kMaxDispatchPins);

 private:
  RingBackend ring_;
  ArenaStorage state_;
  std::uint64_t instance_{detail::next_ring_instance.fetch_add(1, std::memory_order_relaxed)};
  std::uint32_t cancel_tag_{MakeCancelTag()};
  std::size_t in_flight_{};
  std::size_t internal_in_flight_{};
  DrainState drain_{};
  std::span<const ring::ReapedCqe> current_completions_{};
  std::span<const Delivery> current_deliveries_{};
  bool driving_{};
  bool in_callback_dispatch_{};

  [[nodiscard]] std::uint32_t MakeCancelTag() const noexcept {
    auto value =
        instance_ ^ reinterpret_cast<std::uintptr_t>(this) ^ (reinterpret_cast<std::uintptr_t>(state_.operator->()) << 17);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return static_cast<std::uint32_t>(value ^ (value >> 31)) | 0x80000000U;
  }
  [[nodiscard]] static constexpr bool MaskContains(const std::array<uarch::Bitmap64, 4>& mask, std::uint8_t id) noexcept {
    return mask[uarch::PowerOfTwo<6>::div(id)].test(static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(id)));
  }
  static constexpr void SetMask(std::array<uarch::Bitmap64, 4>& mask, std::uint8_t id) noexcept {
    const auto word = uarch::PowerOfTwo<6>::div(id);
    mask[word] = mask[word].set(static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(id)));
  }
  static constexpr void ClearMask(std::array<uarch::Bitmap64, 4>& mask, std::uint8_t id) noexcept {
    const auto word = uarch::PowerOfTwo<6>::div(id);
    mask[word] = mask[word].clear(static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(id)));
  }
  [[nodiscard]] detail::DeferredSqePack* DeferredPack(std::uint32_t slot) noexcept {
    auto index = uarch::PoolIndex<detail::DeferredSqePack>::try_new(slot);
    return index ? state_->deferred_pool.get(*index) : nullptr;
  }
  void ReleaseDeferred(detail::SubmissionIntent& intent) noexcept {
    const auto slot = intent.deferred_slot;
    if (slot == 0) return;
    state_->deferred_next[slot] = state_->deferred_free;
    state_->deferred_free = slot;
    intent.deferred_slot = 0;
  }
  // One slab has one canonical owner; payload routing validates the external subsystem tag.
  [[nodiscard]] static constexpr UserDataToken Local(UserDataToken token) noexcept {
    return MakeToken(Slot(token), Member(token), kInternalSubsystemId, Generation(token));
  }
  [[nodiscard]] detail::SubmissionIntent* Intent(UserDataToken token) noexcept {
    auto* intent = state_->slab.Get(Local(token));
    return intent && intent->subsystem_id == Subsystem(token) ? intent : nullptr;
  }
  void RemoveActive(UserDataToken token) noexcept {
    if (state_->cancellations.Contains(Slot(token)) && !state_->cancellations.Remove(Slot(token)))
      sys::Panic("cancel removal failed");
    state_->active[Slot(token)] = {};
    --state_->subsystem_in_flight[Subsystem(token)];
    --in_flight_;
  }
  template <class Op, class Destination>
  static void EncodeTyped(const Op& op, Destination& destination) noexcept {
    if constexpr (std::same_as<Destination, typename Op::Sqe>)
      op.Encode(destination);
    else
      sys::Panic("SQE encoder width mismatch");
  }
  [[nodiscard]] Result<std::uint32_t> ValidateShape(std::uint8_t count,
                                                    const std::array<std::uint8_t, 4>& widths) const noexcept {
    if (count == 0 || count > widths.size()) return std::unexpected(UringErrorCode::kInvalid);
    const auto& sq = ring_.Sq();
    const auto mode = sq.SqMode();
    std::uint32_t slots = 0;
    bool has_128 = false;
    for (std::size_t i = 0; i < count; ++i) {
      const auto width = widths[i];
      if (width != 1 && width != 2) return std::unexpected(UringErrorCode::kInvalid);
      if (mode == SqRingMode::kSq64 && width == 2) return std::unexpected(UringErrorCode::kNotSupported);
      has_128 |= width == 2;
      slots += mode == SqRingMode::kSq128 ? 1U : static_cast<std::uint32_t>(width);
    }
    const auto worst_case = slots + static_cast<unsigned>(mode == SqRingMode::kSqMixed && has_128);
    if (worst_case > sq.RingEntries()) return std::unexpected(UringErrorCode::kInvalid);
    return slots;
  }
  template <class Encoder>
  [[nodiscard]] Result<void> Publish(UserDataToken token, std::uint8_t count, const std::array<std::uint8_t, 4>& widths,
                                     Encoder&& encode) noexcept {
    auto& sq = ring_.Sq();
    auto shape = ValidateShape(count, widths);
    if (!shape) return std::unexpected(shape.error());
    bool pad = false;
    std::uint32_t offset = 0;
    for (std::size_t i = 0; i < count; ++i) {
      if (sq.SqMode() == SqRingMode::kSqMixed && widths[i] == 2 && ((sq.Tail() + offset) & sq.Mask()) == sq.Mask()) pad = true;
      offset += sq.SqMode() == SqRingMode::kSq128 ? 1U : static_cast<std::uint32_t>(widths[i]);
    }
    if (pad) {
      offset = 1;
      for (std::size_t i = 0; i < count; ++i) {
        if (widths[i] == 2 && ((sq.Tail() + offset) & sq.Mask()) == sq.Mask())
          return std::unexpected(UringErrorCode::kSqRingFull);
        offset += widths[i];
      }
    }
    const auto required = *shape + static_cast<unsigned>(pad);
    if (sq.FreeSlots() < required) return std::unexpected(UringErrorCode::kSqRingFull);
    auto batch = SqeBatch::Begin(sq);
    if (pad) {
      auto entry = batch.Reserve64();
      if (!entry) return std::unexpected(UringErrorCode::kSqRingFull);
      **entry = ChainShape<OneShot128Op>::MakePadNopSqe();
    }
    for (std::uint8_t i = 0; i < count; ++i) {
      abi::FlatSqe* destination{};
      if (widths[i] == 2) {
        auto entry = batch.Reserve128();
        if (!entry) return std::unexpected(UringErrorCode::kSqRingFull);
        encode(i, **entry);
        destination = &(*entry)->sqe;
      } else {
        auto entry = batch.Reserve64();
        if (!entry) return std::unexpected(UringErrorCode::kSqRingFull);
        encode(i, **entry);
        destination = *entry;
      }
      destination->user_data = ring::WithChainMember(token, i).raw();
      if (i + 1 < count) destination->flags |= static_cast<std::uint8_t>(abi::kSqeIoLink);
    }
    if (!state_->slab.Arm(Local(token))) return std::unexpected(UringErrorCode::kProtocolViolation);
    batch.Commit();
    return {};
  }
  [[nodiscard]] Result<void> Replay() noexcept {
    while (!state_->deferred.Empty()) {
      const auto slot = state_->deferred.Front();
      auto* intent = Intent(state_->active[slot]);
      auto* pack = intent ? DeferredPack(intent->deferred_slot) : nullptr;
      if (!pack) return std::unexpected(UringErrorCode::kProtocolViolation);
      auto result =
          Publish(state_->active[slot], pack->count, pack->widths, [&](std::uint8_t member, auto& destination) noexcept {
            if constexpr (std::same_as<std::remove_cvref_t<decltype(destination)>, abi::FlatSqe128>)
              destination = pack->images[member];
            else
              destination = pack->images[member].sqe;
          });
      if (!result) {
        if (result.error() == UringErrorCode::kSqRingFull) return {};
        if (!state_->deferred.PopFront() || !state_->slab.Arm(Local(intent->token)) || !state_->failed.TryEnqueue(slot))
          sys::Panic("failed deferred delivery queue corrupt");
        ReleaseDeferred(*intent);
        intent->reserved = 0;
        return result;
      }
      ReleaseDeferred(*intent);
      const auto popped = state_->deferred.PopFront();
      if (popped != slot) sys::Panic("deferred FIFO corrupt");
    }
    return {};
  }
  [[nodiscard]] Result<void> SyncCancel(UserDataToken token) noexcept {
    [[maybe_unused]] const auto flushed = ring_.Sq().FlushTail();
    auto submitted = ring_.Submit();
    if (!submitted) return std::unexpected(submitted.error());
    abi::IoUringSyncCancelReg cancel{
        .addr = token.raw(), .timeout = this->bounded_shutdown_ ? abi::KernelTimespec{0, 0} : abi::KernelTimespec{-1, -1}};
    auto result = state_->resources.SyncCancel(ring_, cancel);
    if (!result && result.error() != UringErrorCode::kNoEntry &&
        !(this->bounded_shutdown_ &&
          (result.error() == UringErrorCode::kTimerExpired || result.error() == UringErrorCode::kTimedOut)))
      return result;
    return {};
  }
  [[nodiscard]] Result<void> ProcessCancellations(std::uint32_t budget) noexcept {
    for (std::uint32_t i = 0; i < budget && !state_->cancellations.Empty(); ++i) {
      const auto slot = state_->cancellations.Front();
      const auto token = state_->active[slot];
      if (token != kNullToken) {
        const auto* h = state_->slab.Header(Local(token));
        if (h && (State(*h) == OpState::kArmed || State(*h) == OpState::kAwaitNotif)) {
          const auto* intent = Intent(token);
          for (std::uint8_t member = 0; member < intent->count; ++member) {
            const auto target = ring::WithChainMember(token, member);
            auto entry = ring_.Sq().TryReserve64();
            if (entry) {
              ops::PrepareAsyncCancel(target).Encode(**entry);
              (*entry)->user_data = InternalCancelToken().raw();
              ++internal_in_flight_;
            } else {
              // An unsubmitted token is invisible to the synchronous kernel cancellation path.
              auto result = SyncCancel(target);
              if (!result) return result;
            }
          }
        }
      }
      const auto popped = state_->cancellations.PopFront();
      if (popped != slot) sys::Panic("cancel FIFO corrupt");
    }
    return {};
  }
  void RequeueControl(std::uint32_t slot, const ControlCommand& command) noexcept {
    if (!state_->controls.TryEnqueue(slot, command) || !state_->control_order.TryEnqueue(slot))
      sys::Panic("control requeue failed");
  }
  template <class Dispatcher>
  [[nodiscard]] Result<void> ApplyControls(Dispatcher& dispatcher) noexcept {
    const auto budget = state_->control_order.Size();
    for (std::uint32_t i = 0; i < budget; ++i) {
      const auto slot = state_->control_order.PopFront();
      const auto command = state_->controls.PopFront();
      if (!command) sys::Panic("control FIFO mismatch");
      if (command->kind == ControlKind::kDetach) {
        const auto id = static_cast<std::uint8_t>(command->argument);
        if (state_->subsystem_in_flight[id] != 0) {
          RequeueControl(slot, *command);
          continue;
        }
        if constexpr (requires { dispatcher.Detach(id); }) {
          auto result = dispatcher.Detach(id);
          if (!result) return result;
        }
        ClearMask(state_->stopped, id);
        ClearMask(state_->routable, id);
      } else if (command->kind == ControlKind::kSyncCancel) {
        auto* intent = Intent(command->key.token);
        if (!intent) return std::unexpected(UringErrorCode::kInvalidToken);
        if (state_->deferred.Contains(Slot(command->key.token))) {
          auto result = Cancel(command->key.token);
          if (!result) return result;
        } else {
          if (!state_->slab.RequestCancel(Local(command->key.token))) return std::unexpected(UringErrorCode::kInvalidToken);
          auto result = SyncCancel(command->key.token);
          if (!result) return result;
        }
      } else {
        if (in_flight_ != 0 || internal_in_flight_ != 0) {
          RequeueControl(slot, *command);
          continue;
        }
        const auto entries = static_cast<std::uint32_t>(command->argument);
        if constexpr (requires { ring_.ResizeRings(entries); }) {
          auto result = ring_.ResizeRings(entries);
          if (!result) return result;
        } else {
          // A register-only resize invalidates mmap cursors; require a backend that rebuilds them.
          return std::unexpected(UringErrorCode::kNotSupported);
        }
      }
    }
    return {};
  }
  template <class Encoder>
  [[nodiscard]] Result<UserDataToken> StageIntent(std::uint8_t subsystem, OpKind kind, std::uint8_t count,
                                                  const std::array<std::uint8_t, 4>& widths,
                                                  const std::array<std::uint8_t, 4>& opcodes, Encoder&& encode) noexcept {
    if (subsystem == kInternalSubsystemId || subsystem == kMailboxSubsystemId) return std::unexpected(UringErrorCode::kInvalid);
    if (state_->restrict_routable && !MaskContains(state_->routable, subsystem))
      return std::unexpected(UringErrorCode::kNoEntry);
    if (drain_.Stage() != DrainStage::kRunning || MaskContains(state_->stopped, subsystem))
      return std::unexpected(UringErrorCode::kShuttingDown);
    if (this->config_.mode.UsesIoPoll())
      for (std::uint8_t i = 0; i < count; ++i)
        if (!this->config_.mode.AdmitsOpcode(opcodes[i])) return std::unexpected(UringErrorCode::kInvalid);
    auto shape = ValidateShape(count, widths);
    if (!shape) return std::unexpected(shape.error());
    auto allocated = state_->slab.TryAllocate(kind, count);
    if (!allocated) return std::unexpected(UringErrorCode::kSlabExhausted);
    const auto token = MakeToken(Slot(*allocated), 0, subsystem, Generation(*allocated));
    auto& stored = *state_->slab.Get(*allocated);
    stored = {token, 0, count, subsystem, 0};
    Result<void> published = std::unexpected(UringErrorCode::kSqRingFull);
    if (state_->deferred.Empty()) published = Publish(token, count, widths, encode);
    if (!published) {
      if (published.error() != UringErrorCode::kSqRingFull || state_->deferred_free == 0) {
        if (!state_->slab.CancelUnsubmitted(*allocated)) sys::Panic("unsubmitted rollback failed");
        return std::unexpected(published.error());
      }
      const auto deferred_slot = state_->deferred_free;
      state_->deferred_free = state_->deferred_next[deferred_slot];
      auto& pack = *DeferredPack(deferred_slot);
      pack.count = count;
      pack.widths = widths;
      for (std::uint8_t i = 0; i < count; ++i) {
        pack.images[i] = {};
        if (widths[i] == 2)
          encode(i, pack.images[i]);
        else
          encode(i, pack.images[i].sqe);
      }
      stored.deferred_slot = deferred_slot;
      if (!state_->slab.Defer(*allocated) || !state_->deferred.TryEnqueue(Slot(token)))
        sys::Panic("deferred capacity mismatch");
    }
    state_->active[Slot(token)] = token;
    ++in_flight_;
    ++state_->subsystem_in_flight[subsystem];
    return token;
  }

 public:
  explicit BasicProactor(RingBackend&& backend) noexcept
    requires std::move_constructible<RingBackend>
      : ring_(std::move(backend)), state_(ArenaStorage::Require()) {}
  BasicProactor(RingBackend&& backend, ArenaStorage arena, ProactorConfig config = {}) noexcept
    requires std::move_constructible<RingBackend>
      : detail::ProactorLoops<BasicProactor>(config), ring_(std::move(backend)), state_(std::move(arena)) {}
  template <class... Args>
  explicit BasicProactor(std::in_place_t, Args&&... args) noexcept
      : ring_(std::forward<Args>(args)...), state_(ArenaStorage::Require()) {}
  BasicProactor(detail::ConfiguredTag, ProactorConfig config, ArenaStorage arena) noexcept
    requires std::same_as<RingBackend, ScriptedRing>
      : detail::ProactorLoops<BasicProactor>(config),
        ring_(config.entries, config.cq_entries.value_or(config.entries * 2), config.geometry),
        state_(std::move(arena)) {}
  [[nodiscard]] static ProactorBuilder Builder() noexcept { return {}; }
  [[nodiscard]] static std::expected<BasicProactor, SyscallError> CreateConfigured(ProactorConfig config) noexcept {
    auto valid = config.Validate();
    if (!valid) return std::unexpected(SyscallError{static_cast<std::uint16_t>(valid.error())});
    valid = config.ValidateAffinity();
    if (!valid) return std::unexpected(SyscallError{static_cast<std::uint16_t>(valid.error())});
    if constexpr (std::same_as<RingBackend, ScriptedRing>) {
      if (config.entries > 64 || config.cq_entries.value_or(config.entries * 2) > 64 || !std::has_single_bit(config.entries) ||
          !std::has_single_bit(config.cq_entries.value_or(config.entries * 2)))
        return std::unexpected(SyscallError{22});
    }
    auto arena = ArenaStorage::Create();
    if (!arena) return std::unexpected(SyscallError{static_cast<std::uint16_t>(arena.error())});
    if constexpr (std::same_as<RingBackend, KernelRing>) {
      auto ring = detail::OpenConfiguredRing(config);
      if (!ring) return std::unexpected(SyscallError{static_cast<std::uint16_t>(ring.error())});
      return std::expected<BasicProactor, SyscallError>{std::in_place, std::move(*ring), std::move(*arena), config};
    } else {
      return std::expected<BasicProactor, SyscallError>{std::in_place, detail::ConfiguredTag{}, config, std::move(*arena)};
    }
  }
  BasicProactor(const BasicProactor&) = delete;
  BasicProactor& operator=(const BasicProactor&) = delete;
  BasicProactor(BasicProactor&&) = delete;
  BasicProactor& operator=(BasicProactor&&) = delete;
  ~BasicProactor() noexcept {
    if (in_flight_ != 0 || internal_in_flight_ != 0) sys::Panic("drain Proactor before destruction");
  }
  [[nodiscard]] static Result<BasicProactor> Create(std::uint32_t entries = 64, RingMode mode = RingMode::kStandard64_16,
                                                    caps::RequiredCaps required = {}) noexcept
    requires std::same_as<RingBackend, KernelRing>
  {
    required.setup_flags |= ring::ModeSetupFlags(mode);
    auto capabilities = caps::KernelCaps::Probe(required);
    if (!capabilities) return std::unexpected(capabilities.error());
    abi::IoUringParams params{};
    params.flags = ring::BuildSingleIssuerFlags(mode, required.setup_flags);
    auto backend = KernelRing::CreateWithParams(entries, params);
    if (!backend) return std::unexpected(backend.error());
    auto arena = ArenaStorage::Create();
    if (!arena) return std::unexpected(arena.error());
    return Result<BasicProactor>{std::in_place, std::move(*backend), std::move(*arena)};
  }
  [[nodiscard]] std::span<const ring::ReapedCqe> CurrentCompletions() const noexcept {
    return in_callback_dispatch_ ? current_completions_ : std::span<const ring::ReapedCqe>{};
  }
  [[nodiscard]] const ring::ReapedCqe* Extended(const Delivery& delivery) const noexcept {
    if (!in_callback_dispatch_) return nullptr;
    const auto first = reinterpret_cast<std::uintptr_t>(current_deliveries_.data());
    const auto address = reinterpret_cast<std::uintptr_t>(&delivery);
    const auto offset = address - first;
    static_assert(sizeof(Delivery) == 32);
    if (address < first || offset >= current_deliveries_.size_bytes() || !uarch::PowerOfTwo<5>::is_multiple_of(offset))
      return nullptr;
    return &current_completions_[uarch::PowerOfTwo<5>::div(offset)];
  }
  [[nodiscard]] SubsystemHandle<BasicProactor> Handle(std::uint8_t id) noexcept { return {*this, id}; }
  template <class Dispatcher>
  void BindDispatcher(const Dispatcher& dispatcher) noexcept {
    state_->routable = {};
    for (std::uint16_t id = 1; id < 255; ++id) {
      if (dispatcher.CanRoute(static_cast<std::uint8_t>(id))) SetMask(state_->routable, static_cast<std::uint8_t>(id));
    }
    state_->restrict_routable = true;
  }
  [[nodiscard]] RingBackend& Backend() noexcept { return ring_; }
  [[nodiscard]] resources::ResourceRegistry& Resources() noexcept { return state_->resources; }
  [[nodiscard]] std::uint64_t Instance() const noexcept { return instance_; }
  [[nodiscard]] std::size_t InFlight() const noexcept { return in_flight_; }
  [[nodiscard]] std::size_t DeferredCount() const noexcept { return state_->deferred.Size(); }
  [[nodiscard]] DrainStage Stage() const noexcept { return drain_.Stage(); }
  [[nodiscard]] static constexpr UserDataToken InternalCancelToken(std::uint64_t instance) noexcept {
    if (instance == 0) return MakeToken(0, 0, 0, 1);
    auto tag = instance + 0x9e3779b97f4a7c15ULL;
    tag = (tag ^ (tag >> 30)) * 0xbf58476d1ce4e5b9ULL;
    tag = (tag ^ (tag >> 27)) * 0x94d049bb133111ebULL;
    return MakeToken(0, 0, 0, (static_cast<std::uint32_t>(tag ^ (tag >> 31)) & 0x7fffffffU) | 1U);
  }
  [[nodiscard]] UserDataToken InternalCancelToken() const noexcept { return MakeToken(0, 0, 0, cancel_tag_); }
  [[nodiscard]] bool Contains(UserDataToken token) noexcept { return Intent(token) != nullptr; }
  template <ops::OpDescriptor Op>
  [[nodiscard]] Result<UserDataToken> Arm(std::uint8_t subsystem, const Op& op) noexcept {
    static_assert(!Op::kUntracked, "Proactor user operations require terminal CQEs");
    if constexpr (Op::kRequiresCqe32) {
      if (ring_.CqMode() == CqRingMode::kCq16) return std::unexpected(UringErrorCode::kNotSupported);
    }
    const std::array<std::uint8_t, 4> widths{static_cast<std::uint8_t>(Op::kSqeSlots), 0, 0, 0};
    return StageIntent(subsystem, Op::kKind, 1, widths, {static_cast<std::uint8_t>(Op::kOpcode), 0, 0, 0},
                       [&](std::uint8_t, auto& destination) noexcept { EncodeTyped(op, destination); });
  }
  template <ops::OpDescriptor Op>
    requires(Op::kKind == OpKind::kOneShot)
  [[nodiscard]] Result<UserDataToken> ArmOneShot(std::uint8_t id, const Op& op) noexcept {
    return Arm(id, op);
  }
  template <ops::OpDescriptor Op>
    requires(Op::kKind == OpKind::kMultishot)
  [[nodiscard]] Result<UserDataToken> ArmMultishot(std::uint8_t id, const Op& op) noexcept {
    return Arm(id, op);
  }
  template <ops::OpDescriptor Op>
    requires(Op::kKind == OpKind::kZeroCopy)
  [[nodiscard]] Result<UserDataToken> ArmZeroCopy(std::uint8_t id, const Op& op) noexcept {
    return Arm(id, op);
  }
  template <ops::OpDescriptor Op>
    requires(Op::kSqeSlots == 2)
  [[nodiscard]] Result<UserDataToken> Arm128(std::uint8_t id, const Op& op) noexcept {
    return Arm(id, op);
  }
  template <ops::OpDescriptor... Ops>
  [[nodiscard]] Result<UserDataToken> ArmChain(std::uint8_t id, const Ops&... ops) noexcept {
    using Shape = ChainShape<Ops...>;
    if constexpr ((Ops::kRequiresCqe32 || ...)) {
      if (ring_.CqMode() == CqRingMode::kCq16) return std::unexpected(UringErrorCode::kNotSupported);
    }
    std::array<std::uint8_t, 4> widths{};
    for (std::size_t i = 0; i < Shape::kMemberCount; ++i) widths[i] = Shape::kMemberSlots[i];
    return StageIntent(id, OpKind::kChain, static_cast<std::uint8_t>(Shape::kMemberCount), widths,
                       {static_cast<std::uint8_t>(Ops::kOpcode)...}, [&](std::uint8_t member, auto& destination) noexcept {
                         std::uint8_t index = 0;
                         auto encode = [&]<class Op>(const Op& op) noexcept {
                           static_assert(!Op::kUntracked);
                           if (index++ == member) EncodeTyped(op, destination);
                         };
                         (encode(ops), ...);
                       });
  }

  [[nodiscard]] Result<void> Cancel(UserDataToken token) noexcept {
    if (!Intent(token)) return std::unexpected(UringErrorCode::kInvalidToken);
    if (state_->cancellations.Contains(Slot(token)) || state_->failed.Contains(Slot(token))) return {};
    if (this->config_.mode.UsesIoPoll() && !state_->deferred.Contains(Slot(token)))
      return std::unexpected(UringErrorCode::kInvalid);
    if (!state_->slab.RequestCancel(Local(token))) return std::unexpected(UringErrorCode::kInvalidToken);
    const auto state = State(*state_->slab.Header(Local(token)));
    if (state == OpState::kDeferred || state == OpState::kReserved) {
      if (state_->deferred.Contains(Slot(token)) && !state_->deferred.Remove(Slot(token)))
        sys::Panic("deferred removal failed");
      ReleaseDeferred(*Intent(token));
      if (!state_->slab.CancelUnsubmitted(Local(token))) sys::Panic("unsubmitted cancellation failed");
      RemoveActive(token);
    } else if ((state == OpState::kArmed || state == OpState::kAwaitNotif) && !state_->cancellations.Contains(Slot(token))) {
      if (!state_->cancellations.TryEnqueue(Slot(token))) sys::Panic("cancel capacity mismatch");
    }
    return {};
  }
  [[nodiscard]] Result<void> Cancel(OperationKey key) noexcept {
    if (key.ring_instance != instance_) return std::unexpected(UringErrorCode::kInvalidToken);
    return Cancel(key.token);
  }
  [[nodiscard]] Result<void> QuiesceSubsystem(std::uint8_t id) noexcept {
    if (id == kInternalSubsystemId || id == kMailboxSubsystemId) return std::unexpected(UringErrorCode::kInvalid);
    SetMask(state_->stopped, id);
    for (auto token : state_->active)
      if (token != kNullToken && Subsystem(token) == id) {
        auto result = Cancel(token);
        if (!result) return result;
      }
    return {};
  }
  [[nodiscard]] Result<void> EnqueueControl(std::uint32_t queue_slot, ControlCommand command) noexcept {
    if (command.key.ring_instance != instance_ || command.reserved != 0 ||
        (command.kind != ControlKind::kDetach && command.kind != ControlKind::kResize &&
         command.kind != ControlKind::kSyncCancel) ||
        (command.kind == ControlKind::kDetach && (command.argument == 0 || command.argument >= 255)) ||
        (command.kind == ControlKind::kResize && (command.argument == 0 || command.argument > 0x7fffffffU)))
      return std::unexpected(UringErrorCode::kInvalid);
    if (!state_->controls.TryEnqueue(queue_slot, command)) return std::unexpected(UringErrorCode::kBusy);
    if (!state_->control_order.TryEnqueue(queue_slot)) sys::Panic("control FIFO capacity mismatch");
    if (command.kind == ControlKind::kDetach) return QuiesceSubsystem(static_cast<std::uint8_t>(command.argument));
    return {};
  }
  [[nodiscard]] Result<void> BeginDrain() noexcept { return drain_.BeginDrain(); }
  template <class Dispatcher>
  [[nodiscard]] Result<void> DrainBlocking(Dispatcher& dispatcher, std::size_t max_drives = 1024) noexcept {
    if (driving_) return std::unexpected(UringErrorCode::kReentrantDrive);
    if (Stage() == DrainStage::kClosed) return {};
    auto begun = BeginDrain();
    if (!begun) return begun;
    for (std::size_t i = 0; i < max_drives && Stage() != DrainStage::kQuiescent; ++i) {
      auto drove = Drive(DriveMode::kPoll, dispatcher);
      if (!drove && drove.error() != UringErrorCode::kNoEntry) return std::unexpected(drove.error());
    }
    if (Stage() != DrainStage::kQuiescent) return std::unexpected(UringErrorCode::kDrainTimeout);
    return Close();
  }
  [[nodiscard]] Result<void> Close() noexcept {
    if (driving_) return std::unexpected(UringErrorCode::kReentrantDrive);
    auto result = drain_.Close();
    if (!result) return result;
    if constexpr (std::same_as<RingBackend, KernelRing>) ring_ = KernelRing{};
    return {};
  }
  template <class Dispatcher>
  [[nodiscard]] Result<std::size_t> Drive(DriveMode mode, Dispatcher& dispatcher, detail::LoopRequest request = {}) noexcept {
    if (driving_ || in_callback_dispatch_) return std::unexpected(UringErrorCode::kReentrantDrive);
    if (Stage() == DrainStage::kClosed) return std::size_t{0};
    struct Guard {
      bool& flag;
      explicit Guard(bool& value) noexcept : flag(value) { flag = true; }
      ~Guard() noexcept { flag = false; }
    } driving{driving_};
    std::optional<UringErrorCode> error;
    auto remember = [&](const auto& result) noexcept {
      if (!result && !error) error = result.error();
    };
    if (Stage() == DrainStage::kStopping) {
      drain_.StartDraining();
      for (auto token : state_->active)
        if (token != kNullToken && !state_->slab.Header(Local(token))->template get<^^OpHeaderSpec::cancel_requested>()) {
          if (this->config_.mode.UsesIoPoll() && !state_->deferred.Contains(Slot(token))) continue;
          auto canceled = Cancel(token);
          remember(canceled);
        }
    }
    const auto cancellation_budget = state_->cancellations.Size();
    auto controls = ApplyControls(dispatcher);
    remember(controls);
    auto replay = Replay();
    remember(replay);
    const auto minimum =
        !error && state_->failed.Empty()
            ? (request.minimum != 0 ? request.minimum
                                    : (mode == DriveMode::kWait && in_flight_ != 0 && ring_.Cq().Available() == 0 ? 1U : 0U))
            : 0U;
    if (request.submit) {
      auto submitted =
          detail::EnterConfigured(ring_, this->config_, ring_.Cq().Available() >= minimum ? 0U : minimum, request.timeout);
      remember(submitted);
    } else if ((ring_.Sq().Flags() & (abi::kSqTaskrun | abi::kSqCqOverflow)) != 0 ||
               (this->config_.mode.kind == ProactorModeKind::kIoPoll && in_flight_ != 0 && ring_.Cq().Available() == 0)) {
      auto polled = detail::PollKernelOnly(ring_);
      remember(polled);
    }
    std::array<ring::ReapedCqe, kBatch> completions{};
    const auto count = ring_.Cq().ReapBatch(std::span<ring::ReapedCqe>{completions});
    std::array<Delivery, kBatch> deliveries{};
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const auto token = completions[i].completion.token;
      if (token == InternalCancelToken()) {
        if (internal_in_flight_ != 0) --internal_in_flight_;
        continue;
      }
      if (!Intent(token) || !state_->slab.Complete(Local(token), ClassifyCqe(completions[i].completion.flags))) continue;
      deliveries[accepted] = {{token, instance_}, completions[i].completion};
      completions[accepted++] = completions[i];
    }
    std::size_t synthetic = 0;
    while (accepted < kBatch && !state_->failed.Empty()) {
      ++synthetic;
      auto* intent = Intent(state_->active[state_->failed.Front()]);
      if (!intent) sys::Panic("failed deferred token missing");
      const auto token = ring::WithChainMember(intent->token, static_cast<std::uint8_t>(intent->reserved++));
      completions[accepted] = {.completion = {token, -static_cast<std::int32_t>(UringErrorCode::kCanceled), 0}};
      if (!state_->slab.Complete(Local(token), CqeClass::kTerminal)) sys::Panic("synthetic terminal rejected");
      deliveries[accepted++] = {{token, instance_}, {token, -static_cast<std::int32_t>(UringErrorCode::kCanceled), 0}};
      if (intent->reserved == intent->count) static_cast<void>(state_->failed.PopFront());
    }
    Result<void> dispatched{};
    if (accepted != 0) {
      current_completions_ = {completions.data(), accepted};
      current_deliveries_ = {deliveries.data(), accepted};
      Guard callbacks{in_callback_dispatch_};
      auto result = dispatcher.DispatchBatch(std::span<const Delivery>{deliveries.data(), accepted});
      if (!result) dispatched = std::unexpected(result.error());
    }
    current_completions_ = {};
    current_deliveries_ = {};
    for (std::size_t i = 0; i < accepted; ++i) {
      const auto token = deliveries[i].completion.token;
      if (!state_->slab.FinishDispatch(Local(token))) sys::Panic("dispatch pin mismatch");
      if (!state_->slab.Contains(Local(token))) RemoveActive(token);
    }
    remember(dispatched);
    // Terminal CQEs remove pending cancellations before they consume SQ space.
    auto cancels = ProcessCancellations(cancellation_budget);
    remember(cancels);
    replay = Replay();
    remember(replay);
    drain_.Observe(in_flight_ + internal_in_flight_, state_->deferred.Empty() && state_->cancellations.Empty() &&
                                                         state_->failed.Empty() && state_->controls.Empty());
    if (error) return std::unexpected(*error);
    return request.harvest_count ? count + synthetic : accepted;
  }
};
using Proactor = BasicProactor<ring::KernelRing>;
using ScriptedProactor = BasicProactor<ring::ScriptedRing>;
static_assert(sizeof(Proactor) <= 8192);
// ScriptedRing embeds its fake rings, independently of the mapped operation tables.
static_assert(sizeof(ring::ScriptedRing) > 4096 || sizeof(ScriptedProactor) <= 8192);
static_assert(uarch::NoPadding<SubsystemHandle<Proactor>> && uarch::GprPair<SubsystemHandle<Proactor>>);
}  // namespace liburing
