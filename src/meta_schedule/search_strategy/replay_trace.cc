/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include "../utils.h"

namespace tvm {
namespace meta_schedule {

/*! \brief A search strategy that generates measure candidates using trace and random decisions. */
class ReplayTraceNode : public SearchStrategyNode {
 public:
  /*! \brief The state of the search strategy. */
  struct State {
    /*! \brief The search strategy itself */
    ReplayTraceNode* self;
    /*! \brief The design spaces. */
    Array<tir::Trace> design_spaces;
    /*! \brief `[st, ed)` are the indices of the next batch of candidates. */
    int st;
    /*! \brief `[st, ed)` are the indices of the next batch of candidates. */
    int ed;

    /*! \brief The module to be tuned. */
    Array<IRModule> per_thread_mod_{nullptr};

    explicit State(ReplayTraceNode* self, Array<tir::Trace> design_spaces)
        : self(self), design_spaces(design_spaces), st(0), ed(self->num_trials_per_iter) {
      const TuneContextNode* ctx = self->context_;
      ICHECK(ctx);
      IRModule mod = ctx->mod.value();
      this->per_thread_mod_.reserve(ctx->num_threads);
      for (int i = 0; i < ctx->num_threads; i++) {
        this->per_thread_mod_.push_back(DeepCopyIRModule(mod));
      }
    }

    inline Optional<Array<MeasureCandidate>> GenerateMeasureCandidates();
    inline void NotifyRunnerResults(const Array<RunnerResult>& results);
  };

  /*! \brief The number of trials per iteration. */
  int num_trials_per_iter;
  /*! \brief The number of total trials. */
  int max_trials_per_task;
  /*! \brief The max number of failures during trace replaying. */
  int max_fail_count;

  /*! \brief The tuning context of the search strategy. */
  const TuneContextNode* context_{nullptr};
  /*! \brief The random state. -1 means using random number. */
  TRandState rand_state_ = -1;
  /*! \brief The state of the search strategy. */
  std::unique_ptr<State> state_ = nullptr;

  void VisitAttrs(tvm::AttrVisitor* v) {
    v->Visit("num_trials_per_iter", &num_trials_per_iter);
    v->Visit("max_trials_per_task", &max_trials_per_task);
    v->Visit("max_fail_count", &max_fail_count);
    // `context_` is not visited.
    // `rand_state_` is not visited
    // `state_` is not visited
  }

  static constexpr const char* _type_key = "meta_schedule.ReplayTrace";
  TVM_DECLARE_FINAL_OBJECT_INFO(ReplayTraceNode, SearchStrategyNode);

  void InitializeWithTuneContext(const TuneContext& context) final {
    CHECK(context->mod.defined()) << "ValueError: TuneContext.mod is not defined";
    this->context_ = context.get();
    this->rand_state_ = ForkSeed(&context->rand_state);
    this->state_.reset();
  }

  void PreTuning(const Array<tir::Schedule>& design_spaces, const Optional<Database>& database,
                 const Optional<CostModel>& cost_model) final {
    ICHECK(!design_spaces.empty());
    CHECK(this->context_ != nullptr) << "ValueError: Did you forget to initialize the TuneContext?";
    if (this->state_ != nullptr) {
      TVM_PY_LOG(WARNING, this->context_->logging_func) << "RelayTrace is already initialized.";
      this->state_.reset();
    }
    ICHECK(this->state_ == nullptr);
    Array<tir::Trace> design_space_traces;
    design_space_traces.reserve(design_spaces.size());
    for (const tir::Schedule& space : design_spaces) {
      design_space_traces.push_back(space->trace().value()->Simplified(true));
    }
    this->state_ = std::make_unique<State>(this, design_space_traces);
  }

  void PostTuning() final {
    ICHECK(this->state_ != nullptr);
    this->state_.reset();
  }

  Optional<Array<MeasureCandidate>> GenerateMeasureCandidates() final {
    ICHECK(this->state_ != nullptr);
    return this->state_->GenerateMeasureCandidates();
  }

  void NotifyRunnerResults(const Array<MeasureCandidate>& measure_candidates,
                           const Array<RunnerResult>& results) final {
    ICHECK(this->state_ != nullptr);
    this->state_->NotifyRunnerResults(results);
  }
};

inline Optional<Array<MeasureCandidate>> ReplayTraceNode::State::GenerateMeasureCandidates() {
  if (st >= self->max_trials_per_task) {
    return NullOpt;
  }
  ed = std::min(ed, self->max_trials_per_task);
  ICHECK_LT(st, ed);
  const TuneContextNode* ctx = self->context_;
  ICHECK(ctx);
  std::vector<TRandState> per_thread_rand_state = ForkSeed(&self->rand_state_, ctx->num_threads);
  Array<MeasureCandidate> per_task_result(ed - st, MeasureCandidate{nullptr});
  ThreadedTraceApply pp(ctx->postprocs);
  auto f_worker = [this, &per_thread_rand_state, &per_task_result, &pp](int thread_id,
                                                                        int task_id) -> void {
    TRandState& rand_state = per_thread_rand_state[thread_id];
    IRModule mod = this->per_thread_mod_[thread_id];

    for (int fail_count = 0; fail_count < self->max_fail_count; fail_count++) {
      int design_space_index = tir::SampleInt(&rand_state, 0, design_spaces.size());
      tir::Trace trace = design_spaces[design_space_index];
      tir::Trace new_trace = tir::Trace(trace->insts, {});
      if (Optional<tir::Schedule> opt_sch = pp.Apply(mod, new_trace, &rand_state)) {
        tir::Schedule sch = opt_sch.value();
        Array<ArgInfo> args_info = ArgInfo::FromEntryFunc(sch->mod(), /*remove_preproc=*/true);
        per_task_result.Set(task_id, MeasureCandidate(sch, args_info));
        break;
      }
    }
  };
  support::parallel_for_dynamic(0, ed - st, ctx->num_threads, f_worker);
  Array<MeasureCandidate> filtered;
  filtered.reserve(ed - st);
  for (MeasureCandidate result : per_task_result)
    if (result.defined()) {
      filtered.push_back(result);
    }
  return filtered;
}

inline void ReplayTraceNode::State::NotifyRunnerResults(const Array<RunnerResult>& results) {
  st += self->num_trials_per_iter;
  ed += self->num_trials_per_iter;
}

SearchStrategy SearchStrategy::ReplayTrace(int num_trials_per_iter, int max_trials_per_task,
                                           int max_fail_count) {
  ObjectPtr<ReplayTraceNode> n = make_object<ReplayTraceNode>();
  n->num_trials_per_iter = num_trials_per_iter;
  n->max_trials_per_task = max_trials_per_task;
  n->max_fail_count = max_fail_count;
  return SearchStrategy(n);
}

TVM_REGISTER_NODE_TYPE(ReplayTraceNode);
TVM_REGISTER_GLOBAL("meta_schedule.SearchStrategyReplayTrace")
    .set_body_typed(SearchStrategy::ReplayTrace);

}  // namespace meta_schedule
}  // namespace tvm
