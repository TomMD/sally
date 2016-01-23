/**
 * This file is part of sally.
 * Copyright (C) 2015 SRI International.
 *
 * Sally is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Sally is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with sally.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "engine/ic3/ic3_engine.h"
#include "engine/ic3/solvers.h"
#include "engine/factory.h"

#include "smt/factory.h"
#include "system/state_trace.h"
#include "utils/trace.h"
#include "expr/gc_relocator.h"

#include <stack>
#include <cassert>
#include <sstream>
#include <iostream>
#include <fstream>
#include <algorithm>

#define unused_var(x) { (void)x; }

namespace sally {
namespace ic3 {

induction_obligation::induction_obligation(expr::term_manager& tm, expr::term_ref F_fwd, expr::term_ref F_cex, size_t d, double score)
: F_fwd(F_fwd)
, F_cex(F_cex)
, d(d)
, score(score)
{}

bool induction_obligation::operator == (const induction_obligation& o) const {
  return F_cex == o.F_cex;
}

void induction_obligation::bump_score(double amount) {
  if (score + amount >= 0) {
    score += amount;
  } else {
    score = 0;
  }
}

bool induction_obligation::operator < (const induction_obligation& o) const {
  // Smaller depth wins
  if (d != o.d) {
    return d > o.d;
  }
  // Larger score wins
  if (score != o.score) {
    return score < o.score;
  }
  // Break ties
  return F_cex > o.F_cex;
}

ic3_engine::ic3_engine(const system::context& ctx)
: engine(ctx)
, d_transition_system(0)
, d_property(0)
, d_trace(0)
, d_smt(0)
, d_reachability(ctx)
, d_induction_frame_index(0)
, d_induction_frame_depth(0)
, d_induction_frame_index_next(0)
, d_property_invalid(false)
, d_learning_type(LEARN_UNDEFINED)
{
  d_stats.frame_index = new utils::stat_int("ic3::frame_index", 0);
  d_stats.induction_depth = new utils::stat_int("ic3::induction_depth", 0);
  d_stats.frame_size = new utils::stat_int("ic3::frame_size", 0);
  d_stats.frame_pushed = new utils::stat_int("ic3::frame_pushed", 0);
  d_stats.queue_size = new utils::stat_int("ic3::queue_size", 0);
  d_stats.max_cex_depth = new utils::stat_int("ic3::max_cex_depth", 0);
  ctx.get_statistics().add(new utils::stat_delimiter());
  ctx.get_statistics().add(d_stats.frame_index);
  ctx.get_statistics().add(d_stats.induction_depth);
  ctx.get_statistics().add(d_stats.frame_size);
  ctx.get_statistics().add(d_stats.frame_pushed);
  ctx.get_statistics().add(d_stats.queue_size);
  ctx.get_statistics().add(d_stats.max_cex_depth);
}

ic3_engine::~ic3_engine() {
  delete d_trace;
  delete d_smt;
}

void ic3_engine::reset() {

  d_transition_system = 0;
  d_property = 0;
  delete d_trace;
  d_trace = 0;
  d_induction_frame.clear();
  d_induction_frame_index = 0;
  d_induction_frame_depth = 0;
  d_induction_obligations.clear();
  d_induction_obligations_next.clear();
  d_induction_obligations_count.clear();
  delete d_smt;
  d_smt = 0;
  d_properties.clear();
  d_property_invalid = false;
  d_frame_formula_invalid_info.clear();
  d_frame_formula_parent_info.clear();
  d_reachability.clear();

  if (ai()) {
    ai()->clear();
  }
}

induction_obligation ic3_engine::pop_induction_obligation() {
  assert(d_induction_obligations.size() > 0);
  induction_obligation ind = d_induction_obligations.top();
  d_induction_obligations.pop();
  d_induction_obligations_handles.erase(ind.F_cex);
  d_stats.queue_size->get_value() = d_induction_obligations.size();
  return ind;
}

void ic3_engine::add_to_induction_frame(expr::term_ref F, solvers::induction_assertion_type type) {
  assert(d_induction_frame.find(F) == d_induction_frame.end());
  if (type == solvers::INDUCTION_FIRST) {
    d_induction_frame.insert(F);
  }
  d_smt->add_to_induction_solver(F, type);
  d_stats.frame_size->get_value() = d_induction_frame.size();
}

void ic3_engine::enqueue_induction_obligation(const induction_obligation& ind) {
  assert(d_induction_obligations_handles.find(ind.F_cex) == d_induction_obligations_handles.end());
  assert(d_induction_frame.find(ind.F_cex) != d_induction_frame.end());
  induction_obligation_queue::handle_type h = d_induction_obligations.push(ind);
  d_induction_obligations_handles[ind.F_cex] = h;
  d_stats.queue_size->get_value() = d_induction_obligations.size();
}

void ic3_engine::bump_induction_obligation(expr::term_ref f, double amount) {
  expr::term_ref_hash_map<induction_obligation_queue::handle_type>::const_iterator find = d_induction_obligations_handles.find(f);
  if (find == d_induction_obligations_handles.end()) {
    return; // not in queue... already processed
  }
  induction_obligation_queue::handle_type h = find->second;
  induction_obligation& ind = *h;
  ind.bump_score(amount);
  d_induction_obligations.update(h);
}

ic3_engine::induction_result ic3_engine::push_if_inductive(induction_obligation& ind) {

  expr::term_ref F_cex = ind.F_cex;
  expr::term_ref F_fwd = ind.F_fwd;
  size_t d = ind.d;

  TRACE("ic3") << "ic3: pushing F_cex at " << d_induction_frame_index << ": " << F_cex << std::endl;

  // Check if inductive
  expr::term_ref F_cex_not = tm().mk_term(expr::TERM_NOT, F_cex);
  solvers::query_result cex_result = d_smt->check_inductive(F_cex_not);

  // If counter-example is inductive
  if (cex_result.result == smt::solver::UNSAT) {
    TRACE("ic3") << "ic3: pushing F_fwd at " << d_induction_frame_index << ": " << F_fwd << std::endl;
    // Counter-example is inductive, try to push the learnt
    solvers::query_result fwd_result = d_smt->check_inductive(F_fwd);
    if (fwd_result.result != smt::solver::UNSAT) {
      // The current refutation is too strong, refine and replace it
      expr::term_ref I_ind = d_smt->interpolate_induction(F_cex_not);
      d_induction_frame.erase(F_fwd);
      F_fwd = tm().mk_term(expr::TERM_OR, F_fwd, I_ind);
      d_induction_frame.insert(F_fwd);
    }
    // Push the it
    d_induction_obligations_next.push_back(induction_obligation(tm(), F_fwd, F_cex, d, 0));
    d_smt->add_to_induction_solver(F_fwd, solvers::INDUCTION_FIRST);
    d_smt->add_to_induction_solver(F_fwd, solvers::INDUCTION_INTERMEDIATE);
    d_stats.frame_pushed->get_value() = d_induction_obligations_next.size();
    return INDUCTION_SUCCESS;
  }

  // Counter-example is not inductive
  expr::term_ref G = cex_result.generalization;
  TRACE("ic3::generalization") << "ic3: generalization " << G << std::endl;

  // Check if G is reachable (give a budget enough for frame length fails)
  size_t start = (d_induction_frame_index + 1) - d_induction_frame_depth;
  size_t end = d_induction_frame_index;
  reachability::status reachable = d_reachability.check_reachable(start, end, G, cex_result.model);
  
  // If reachable, we're not inductive
  if (reachable == reachability::REACHABLE) {
    d_property_invalid = true;
    return INDUCTION_FAIL;
  }

  // Basic thing to learn is the generalization
  expr::term_ref G_not = tm().mk_term(expr::TERM_NOT, G);
  TRACE("ic3") << "ic3: backward learnt: " << G_not << std::endl;

  // Learn forward that refutes G
  expr::term_ref learnt = d_smt->learn_forward(d_induction_frame_index, G);
  TRACE("ic3") << "ic3: forward learnt: " << learnt << std::endl;

  // Add to induction frame
  add_to_induction_frame(G_not, solvers::INDUCTION_FIRST);
  d_induction_frame.insert(learnt);

  // Enqueue to retry
  enqueue_induction_obligation(induction_obligation(tm(), learnt, G, d + d_induction_frame_depth, 0));

  return INDUCTION_RETRY;
}

void ic3_engine::extend_induction_failure(expr::term_ref f) {

  const reachability::cex_type& cex = d_reachability.get_cex();

  assert(cex.size() > 0);
  //                                  !F
  //                         depth     |
  //       cex          ****************
  // *******************       |      |
  // .......................................................
  //                   G       |      |
  // !G!G          !G!G        |      |
  // FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF
  //                           |
  //                         frame

  // We have a counter-example to inductiveness of f at frame cex_depth + induction_size
  // and cex d_counterexample has its generalization at the back.
  assert(cex.size() - 1 + d_induction_frame_depth > d_induction_frame_index);

  // Solver for checking
  smt::solver_scope solver_scope;
  d_smt->ensure_counterexample_solver_depth(d_induction_frame_index+1); // Smalleet depth needed
  d_smt->get_counterexample_solver(solver_scope);
  solver_scope.push();
  smt::solver* solver = solver_scope.get_solver();

  assert(cex.size() - 1 <= d_smt->get_counterexample_solver_depth());

  // Assert all the generalizations
  size_t k = 0;
  for (; k < cex.size(); ++ k) {
    // Add the generalization to frame k
    expr::term_ref G_k = d_trace->get_state_formula(cex[k], k);
    solver->add(G_k, smt::solver::CLASS_A);
  }

  // Move to where f is false
  k = cex.size() - 1 + d_induction_frame_depth;

  // Assert f at next frame
  d_smt->ensure_counterexample_solver_depth(k);
  solver->add(d_trace->get_state_formula(tm().mk_term(expr::TERM_NOT, f), k), smt::solver::CLASS_A);

  // Should be SAT
  smt::solver::result r = solver->check();
  assert(r == smt::solver::SAT);
  expr::model::ref model = solver->get_model();
  d_trace->set_model(model);

  if (ctx().get_options().get_bool("ic3-dont-extend")) {
    return;
  }

  // Try to extend it
  for (;;) {

    // We know there is a counterexample to induction of f: 0, ..., k-1, with f
    // being false at k. We try to extend it to falsify the reason we
    // introduced f. We introduced f to refute the counterexample to induction
    // of parent(f), which is witnessed by generalization refutes(f). We are
    // therefore looking to satisfy refutes(f) at k.
    assert(is_invalid(f));

    d_stats.max_cex_depth->get_value() = std::max((int) k, d_stats.max_cex_depth->get_value());

    // Bump the assertion
    bump_induction_obligation(f, 1);

    // If no more parents, we're done
    if (!has_parent(f)) {
      // If this is a counter-example to the property itself => full counterexample
      if (d_properties.find(f) != d_properties.end()) {
        MSG(1) << "ic3: CEX found at depth " << d_smt->get_counterexample_solver_depth() << " (with induction frame at " << d_induction_frame_index << ")" << std::endl;
      }
      break;
    }

    // Try to extend
    k = k + get_refutes_depth(f);
    f = get_parent(f);

    // If invalid already, done
    if (is_invalid(f)) {
      break;
    }

    // Check at frame of the parent
    d_smt->ensure_counterexample_solver_depth(k);
    solver->add(d_trace->get_state_formula(tm().mk_term(expr::TERM_NOT, f), k), smt::solver::CLASS_A);

    // If not a generalization we need to check
    r = solver->check();

    // If not sat, we can't extend any more
    if (r != smt::solver::SAT) {
      break;
    }

    // We're sat (either by knowing, or by checking), so we extend further
    set_invalid(f, k);
    model = solver->get_model();
    d_trace->set_model(model);
  }
}

void ic3_engine::push_current_frame() {

  // Search while we have something to do
  while (!d_induction_obligations.empty() && !d_property_invalid) {

    // Pick a formula to try and prove inductive, i.e. that F_k & P & T => P'
    induction_obligation ind = pop_induction_obligation();

    // Push the formula forward if it's inductive at the frame
    induction_result ind_result = push_if_inductive(ind);

    // See what happened
    switch (ind_result) {
    case INDUCTION_RETRY:
      // We'll retry the same formula (it's already added to the solver)
      enqueue_induction_obligation(ind);
      break;
    case INDUCTION_SUCCESS:
      // Boss
      break;
    case INDUCTION_FAIL:
      // Failure, it's marked
      break;
    case INDUCTION_INCONCLUSIVE:
      break;
    }
  }

  // Dump dependency graph if asked
  if (ctx().get_options().get_bool("ic3-dump-dependencies")) {
    dump_dependencies();
  }
}

engine::result ic3_engine::search() {

  // Push frame by frame */
  for(;;) {

    // Push the current induction frame forward
    push_current_frame();

    // If we've disproved the property, we're done
    if (d_property_invalid) {
      return engine::INVALID;
    }

    if (d_induction_frame.size() == d_induction_obligations_next.size()) {
      if (ctx().get_options().get_bool("ic3-show-invariant")) {
        d_smt->print_formulas(d_induction_frame, std::cout);
      }
      return engine::VALID;
    }

    // Clear induction obligations
    d_induction_obligations.clear();

    // Depth of induction
    d_induction_frame_depth = d_induction_frame_index + 1;
    d_smt->reset_induction_solver(d_induction_frame_depth);

    MSG(1) << "ic3: Extending trace to " << d_induction_frame_index << " with induction depth " << d_induction_frame_depth <<
        " (" << d_induction_obligations_next.size() << "/" << d_induction_frame.size() << ")" << std::endl;

    d_stats.frame_index->get_value() = d_induction_frame_index;
    d_stats.induction_depth->get_value() = d_induction_frame_depth;
    d_stats.frame_size->get_value() = 0;

    // If exceeded number of frames
    if (ctx().get_options().get_unsigned("ic3-max") > 0 && d_induction_frame_index >= ctx().get_options().get_unsigned("ic3-max")) {
      return engine::INTERRUPTED;
    }

    // Add formulas to the new frame
    d_induction_frame.clear();
    std::vector<induction_obligation>::const_iterator next_it = d_induction_obligations_next.begin();
    for (; next_it != d_induction_obligations_next.end(); ++ next_it) {
      // The formula
      expr::term_ref to_assert = tm().mk_term(expr::TERM_NOT, next_it->F_cex);
      add_to_induction_frame(to_assert, solvers::INDUCTION_FIRST);
      add_to_induction_frame(to_assert, solvers::INDUCTION_INTERMEDIATE);
      enqueue_induction_obligation(*next_it);
      d_induction_frame.insert(next_it->F_fwd);
    }

    // Next frame and safe position
    d_induction_obligations_next.clear();
    d_stats.frame_pushed->get_value() = 0;
    d_induction_frame_index_next = d_induction_frame_index + d_induction_frame_depth;

    // Do garbage collection
    d_smt->gc();
  }

  // Didn't prove or disprove, so unknown
  return engine::UNKNOWN;
}

engine::result ic3_engine::query(const system::transition_system* ts, const system::state_formula* sf) {

  // Initialize
  result r = UNKNOWN;

  // Reset the engine
  reset();

  // Remember the input
  d_transition_system = ts;
  d_property = sf;

  // Make the trace
  if (d_trace) { delete d_trace; }
  d_trace = new system::state_trace(sf->get_state_type());

  // Initialize the solvers
  if (d_smt) { delete d_smt; }
  d_smt = new solvers(ctx(), ts, d_trace);

  // Initialize the reachability solver
  d_reachability.init(d_transition_system, d_smt);

  // Initialize the induction solver
  d_induction_frame_index = 0;
  d_induction_frame_depth = 1;
  d_induction_frame_index_next = 1;
  d_smt->reset_induction_solver(1);

  // Add the property we're trying to prove (if not already invalid at frame 0)
  bool ok = add_property(d_property->get_formula());
  if (!ok) {
    return engine::INVALID;
  }

  while (r == UNKNOWN) {

    MSG(1) << "ic3: starting search" << std::endl;

    // Search
    r = search();
  }

  MSG(1) << "ic3: search done: " << r << std::endl;

  return r;
}

bool ic3_engine::add_property(expr::term_ref P) {
  if (tm().term_of(P).op() == expr::TERM_AND) {
    size_t size = tm().term_of(P).size();
    for (size_t i = 0; i < size; ++ i) {
      bool ok = add_property(tm().term_of(P)[i]);
      if (!ok) return false;
    }
    return true;
  } else {
    smt::solver::result result = d_smt->query_at_init(tm().mk_term(expr::TERM_NOT, P));
    if (result == smt::solver::UNSAT) {
      if (d_induction_frame.find(P) == d_induction_frame.end()) {
        add_to_induction_frame(P, solvers::INDUCTION_FIRST);
        add_to_induction_frame(P, solvers::INDUCTION_INTERMEDIATE);
        enqueue_induction_obligation(induction_obligation(tm(), P, tm().mk_term(expr::TERM_NOT, P), 0, 0));
      }
      bump_induction_obligation(P, 1);
      d_properties.insert(P);
      return true;
    } else {
      return false;
    }
  }
}

const system::state_trace* ic3_engine::get_trace() {
  return d_trace;
}

void ic3_engine::set_invalid(expr::term_ref f, size_t frame) {
  assert(frame >= d_induction_frame_index);
  assert(d_frame_formula_invalid_info.find(f) == d_frame_formula_invalid_info.end());
  d_frame_formula_invalid_info[f] = frame;
  if (d_properties.find(f) != d_properties.end()) {
    d_property_invalid = true;
  }
}

bool ic3_engine::is_invalid(expr::term_ref f) const {
  expr::term_ref_map<size_t>::const_iterator find = d_frame_formula_invalid_info.find(f);
  return (find != d_frame_formula_invalid_info.end());
}

void ic3_engine::gc_collect(const expr::gc_relocator& gc_reloc) {
  d_frame_formula_parent_info.reloc(gc_reloc);
  assert(d_induction_obligations_next.size() == 0);
  d_smt->gc_collect(gc_reloc);
  d_reachability.gc_collect(gc_reloc);
}

void ic3_engine::set_refutes_info(expr::term_ref f, expr::term_ref g, expr::term_ref l) {
  d_frame_formula_parent_info[l] = frame_formula_parent_info(f, g, d_induction_frame_depth);
}

expr::term_ref ic3_engine::get_refutes(expr::term_ref l) const {
  assert(has_parent(l));
  expr::term_ref_map<frame_formula_parent_info>::const_iterator find = d_frame_formula_parent_info.find(l);
  return find->second.refutes;
}

size_t ic3_engine::get_refutes_depth(expr::term_ref l) const {
  assert(has_parent(l));
  expr::term_ref_map<frame_formula_parent_info>::const_iterator find = d_frame_formula_parent_info.find(l);
  return find->second.depth;
}

expr::term_ref ic3_engine::get_parent(expr::term_ref l) const {
  assert(has_parent(l));
  expr::term_ref_map<frame_formula_parent_info>::const_iterator find = d_frame_formula_parent_info.find(l);
  return find->second.parent;
}

bool ic3_engine::has_parent(expr::term_ref l) const {
  expr::term_ref_map<frame_formula_parent_info>::const_iterator find = d_frame_formula_parent_info.find(l);
  if (find == d_frame_formula_parent_info.end()) { return false; }
  return !find->second.parent.is_null();
}

void ic3_engine::dump_dependencies() const {
  std::stringstream ss;
  ss << "dependency." << d_induction_frame_index << ".dot";
  std::ofstream output(ss.str().c_str());

  output << "digraph G {" << std::endl;

  // Output relationships
  expr::term_ref_map<frame_formula_parent_info>::const_iterator it = d_frame_formula_parent_info.begin();
  expr::term_ref_map<frame_formula_parent_info>::const_iterator it_end = d_frame_formula_parent_info.end();
  for (; it != it_end; ++ it) {
    expr::term_ref learnt = it->first;
    expr::term_ref parent = it->second.parent;
    if (is_invalid(learnt)) {
      output << tm().id_of(learnt) << " [color = red];" << std::endl;
    }
    output << tm().id_of(learnt) << "->" << tm().id_of(parent) << ";" << std::endl;
  }

  output << "}" << std::endl;
}


}
}
