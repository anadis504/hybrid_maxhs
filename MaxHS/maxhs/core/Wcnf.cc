/***********[Wcnf.cc]
Copyright (c) 2012-2013 Jessica Davies, Fahiem Bacchus

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

***********/

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <set>

#ifdef GLUCOSE
#include "glucose/core/SolverTypes.h"
#include "glucose/utils/System.h"
#else
#include "minisat/core/SolverTypes.h"
#include "minisat/utils/System.h"
#endif

#include "maxhs/core/Bvars.h"
#include "maxhs/core/Dimacs.h"
#include "maxhs/core/Wcnf.h"
#include "maxhs/ifaces/cadicalSatSolver.h"
#include "maxhs/utils/Params.h"
#include "maxhs/utils/hash.h"
#include "maxhs/utils/io.h"

#ifdef GLUCOSE
namespace Minisat = Glucose;
#endif

using MaxHS_Iface::cadicalSolver;
using MaxHS_Iface::SatSolver_uniqp;
using Minisat::lbool;
using Minisat::mkLit;
using Minisat::sign;
using Minisat::toInt;
using Minisat::toLit;
using Minisat::var_Undef;

void Wcnf::set_dimacs_params(int nvars, int nclauses, Weight top) {
  dimacs_nvars = nvars;
  dimacs_nclauses = nclauses;
  dimacs_top = top;
}

bool Wcnf::inputDimacs(std::string filename, bool verify) {
  // verify == reading input 2nd time to verify result (don't apply
  // preprocessing)
  instance_file_name = filename;
  auto suffix = filename.substr(filename.find_last_of(".") + 1);

  double start_time = Minisat::cpuTime();
  gzFile input = gzopen(filename.c_str(), "rb");
  if (input == NULL) {
    cout << "c ERROR: problem opening input file: " << instance_file_name
         << "\n";
    return false;
  }
  if (!parse_DIMACS(input, this, suffix, verify)) {
    cout << "c ERROR: Parsing error on input file: " << instance_file_name
         << "\n";
    return false;
  }
  if (!verify) {
    computeWtInfo();
    parsing_time = Minisat::cpuTime() - start_time;
    printFormulaStats();
    // simplify();
    if (params.verbosity > 0) printSimpStats();
  }
  if (!nSofts()) {
    total_cls_wt = dimacs_top;
  }
  return true;
}

void Wcnf::addDimacsClause(vector<Lit>& lits, Weight w) {
  // This routine needs to know dimacs_top (so set_dimacs_params should
  // have been called first) to determine if the clause is soft or hard
  if (w >= dimacs_top)
    addHardClause(lits);
  else
    addSoftClause(lits, w);
}

bool Wcnf::prepareClause(vector<Lit>& lits) {
  if (lits.size() > 1) {
    std::sort(lits.begin(), lits.end());
    size_t cur_size, examine;
    for (cur_size = examine = 1; examine < lits.size(); examine++) {
      if (lits[cur_size - 1] == ~lits[examine]) {
        return false;  // Tautologies
      }
      if (lits[cur_size - 1] != lits[examine]) lits[cur_size++] = lits[examine];
    }
    lits.resize(cur_size);
  }
  return true;
}

void Wcnf::update_maxorigvar(vector<Lit>& lits) {
  for (auto l : lits)
    if (var(l) > maxorigvar) maxorigvar = var(l);
  if (maxorigvar > maxvar) maxvar = maxorigvar;
}

void Wcnf::addHardClause(vector<Lit>& lits) {
  update_maxorigvar(lits);
  if (lits.size() == 1) ++nOrigUnits;
  _addHardClause(lits);
}

void Wcnf::_addHardClause(vector<Lit>& lits) {
  // Use this routine when adding a clause not contained in the
  // original formula, e.g., adding preprocessing clause
  if (unsat) return;
  if (!prepareClause(lits)) return;  // skip tautologies
  for (auto l : lits)
    if (maxvar < var(l)) maxvar = var(l);
  hard_cls.addVec(lits);
  noDups = false;
}

void Wcnf::addSoftClause(vector<Lit>& lits, Weight w) {
  // zero weight clauses discarded by this interface function.
  if (w < 0)
    cout << "c ERROR: soft clause cannot have negative weight: " << w << "\n";
  else if (w > 0) {
    update_maxorigvar(lits);
    _addSoftClause(lits, w);
  }
}

void Wcnf::_addSoftClause(vector<Lit>& lits, Weight w) {
  // Use this routine when adding a clause not contained in the
  // original formula, e.g., adding preprocessing clause
  if (unsat) return;
  if (!prepareClause(lits)) return;  // skip tautologies
  Weight intPart;
  if (lits.size() > 0) {
    if (modf(w, &intPart) > 0) intWts = false;
    soft_cls.addVec(lits);
    soft_clswts.push_back(w);
    total_cls_wt += w;
    for (auto l : lits)
      if (maxvar < var(l)) maxvar = var(l);
  } else
    base_cost += w;
  noDups = false;
}

void Wcnf::addCardConstr(vector<Lit>& lits, int k, char sense,
                         vector<Lit>& o_lits) {
  cardConstr.push_back(std::make_tuple(lits, k, sense, o_lits));
}

void Wcnf::addClausalConstr(vector<Lit>& lits) {
  update_maxorigvar(lits);
  for (auto l : lits)
    if (maxvar < var(l)) maxvar = var(l);
  clausal_cnstr_for_mip.addVec(lits);
}

void Wcnf::simplify() {
  // We allow a Wcnf to transform itself in various model equivalent
  // ways.  Only the remaining hard and soft clauses after
  // simplification will be passed to the solver. After the solver has
  // found a model for the transformed wcnf, it will need to invoke the
  // method 'rewriteModelToInput' to convert that model to a model of
  // the original input formula. (E.g., if eliminated variables must be
  // added back).

  orig_all_lits_soft = test_all_lits_are_softs();

  // wcnf_harden --- test some softs can be hardened because of their high
  // weight
  if (params.wcnf_harden) simpleHarden();

  // Look for units and/or equalities implied by the hard clauses.
  // Simplify by hard units and replace y by x if x==y.
  if (params.wcnf_eqs || params.wcnf_units) subEqsAndUnits();

  // New b-variables are not added to soft units, e.g., (x). Instead
  // we reuse the literal in the soft unit as its own b-variable. In this case
  // we need to ensure that we have no duplicate softs for correctness.
  remDupCls();
  // Find groups of mutually exclusive b-variables. Both positive
  // \sum bi <= 1 and negative \sum -bi <= 1.
  //  These are given a special treatment.
  if (params.mx_find_mxes) {
    if (params.mx_find_mxes == 3) {
      params.mx_find_mxes = 2;
      mxBvars();
      params.mx_find_mxes = 1;
      mxBvars();
    } else
      mxBvars();
  }

  // Some of these transformations might increase the forced (base) cost of
  // the WCNF and change the weights
  computeWtInfo();

  // remap the variables
  remapVars();

  if (params.simplify_and_exit) {
    // write simplified wcnf to cout then exit.
    printDimacs(cout);
  }
}

bool Wcnf::test_all_lits_are_softs() {
  vector<char> is_soft(nVars(), false);
  for (const auto cls : soft_cls) {
    if (cls.size() == 1) is_soft[var(cls[0])] = true;
  }
  for (const auto cls : hard_cls)
    for (auto l : cls)
      if (!is_soft[var(l)]) return false;
  for (const auto cls : soft_cls)
    for (auto l : cls)
      if (!is_soft[var(l)]) return false;
  return true;
}

void Wcnf::subEqsAndUnits() {
  if (unsat) return;

  // reduce formula by hard units. Optionally find
  // equalities implied by the hard clauses and
  // if x <==> y then replace all occurances of y by x
  //  - during replace shrink the clause if this results in duplicates.
  //  - remove the clause if this results in a tautology
  // After all y's have been replaced, add the clauses (-x, y) and (-y, x)
  // to the set of hards...so that the final solution will set y appropriately.
  //( 'y'  longer appears elsewhere in the theory).

  // finding equalities might also force some units
  // so in general we need propagation over equalities and units.
  // but for now do incomplete prop.

  int ph = hard_cls.size();
  int ph_lits = hard_cls.total_size();
  int ps = soft_cls.size();
  int ps_lits = soft_cls.total_size();

  // 1. Find current units in the hard clauses and then find/binaries
  // among the hard clauses reduced by those units.

  SatSolver_uniqp sat_solver{new cadicalSolver};
  for (size_t i = 0; i < nHards(); i++) sat_solver->addClause(getHard(i));
  if (sat_solver->theory_is_unsat()) {
    unsat = true;
    return;
  }
  sat_solver->unit_propagate();
  hard_units = sat_solver->getForced(0);

  if (params.wcnf_eqs) {
    vector<Lit> binaries = get_binaries(sat_solver);
    vector<vector<Lit>> edges;
    for (size_t i = 0; i < binaries.size(); i += 2) {
      Lit x = binaries[i];
      Lit y = binaries[i + 1];
      size_t max_index = std::max(std::max(toInt(x), toInt(~x)),
                                  std::max(toInt(y), toInt(~y)));
      if (max_index >= edges.size()) edges.resize(max_index + 1);
      edges[toInt(x)].push_back(y);
      edges[toInt(y)].push_back(x);
    }
    all_scc = binary_scc(edges);
  } else
    all_scc.clear();

  // debug
  // cout << "hard_units" << hard_units << "\n";
  // cout << "all_scc " << all_scc << "\n";

  // modify the wcnf by the detected equivalents and units
  if (!all_scc.empty() || !hard_units.empty()) {
    hard_cls = reduce_by_eqs_and_units(hard_cls, false);
    soft_cls = reduce_by_eqs_and_units(soft_cls, true);
  }

  // check for pures and units.
  auto n_hards{hard_units.size()};
  int pures{0};

  vector<uint8_t> appears(nVars(), 0);
  for (const auto cls : hard_cls) {
    if (cls.size() == 1) {
      hard_units.push_back(cls[0]);
      // debug
      // cout << "Hard unit after reduction " << cls.getVec() << "\n";
    }
    for (auto l : cls) appears[var(l)] |= sign(l) ? 0b01 : 0b10;
  }
  for (auto cls : soft_cls)
    for (auto l : cls) appears[var(l)] |= sign(l) ? 0b01 : 0b10;

  for (size_t v = 0; v != nVars(); v++)
    if (appears[v] == 0b01) {  // pure -v
      ++pures;
      // debug
      // cout << "pure unit after reduction " << mkLit(v, true) << "\n";
      hard_units.push_back(mkLit(v, true));
    } else if (appears[v] == 0b10) {  // pure
      ++pures;
      // debug
      // cout << "pure unit after reduction " << mkLit(v, false) << "\n";
      hard_units.push_back(mkLit(v, false));
    }

  if (hard_units.size() > n_hards) {
    if (params.verbosity > 0)
      cout << "c WCNF eqs: found additional units after scc "
           << hard_units.size() - n_hards << " pures " << pures << "\n";

    for (auto l : hard_units) sat_solver->addClause(l);
    if (sat_solver->theory_is_unsat()) {
      unsat = true;
      return;
    }
    sat_solver->unit_propagate();
    hard_units = sat_solver->getForced(0);

    // debug
    // cout << "New hard units = " << hard_units << "\n";

    hard_cls = reduce_by_eqs_and_units(hard_cls, false);
    soft_cls = reduce_by_eqs_and_units(soft_cls, true);
  }

  if (params.verbosity > 0) {
    int nvars_removed = hard_units.size();
    for (size_t i = 0; i < all_scc.size(); i++)
      nvars_removed += all_scc[i].size() - 1;

    cout << "c WCNF units: found " << hard_units.size() << " units\n"
         << "c WCNF SCCs: found " << all_scc.size()
         << " strongly connected components\n"
         << "c WCNF removed: " << nvars_removed << " variables\n"
         << "c WCNF removed: " << ph - static_cast<int>(hard_cls.size())
         << " hard clauses\n"
         << "c WCNF removed: "
         << ph_lits - static_cast<int>(hard_cls.total_size())
         << " lits from hard clauses\n"
         << "c WCNF removed: " << ps - static_cast<int>(soft_cls.size())
         << " soft clauses\n"
         << "c WCNF removed: "
         << ps_lits - static_cast<int>(soft_cls.total_size())
         << " lits from softs clauses\n";
  }
}

vector<Lit> Wcnf::get_units() {
  // feed hard clauses of wcnf into a sat solver, do unit prop, and then
  // return the found units
  SatSolver_uniqp sat_solver{new cadicalSolver};
  for (size_t i = 0; i < nHards(); i++) sat_solver->addClause(getHard(i));
  if (sat_solver->theory_is_unsat()) {
    unsat = true;
    return {};
  }
  return sat_solver->getForced(0);
}

vector<Lit> Wcnf::get_binaries(SatSolver_uniqp& sat_solver) {
  vector<Lit> binaries{};
  for (auto& clause : hard_cls) {
    int nlits = 0;
    for (auto l : clause) {
      auto truth_value = sat_solver->fixedValue(l);
      if (truth_value == l_Undef) nlits++;
      if (truth_value == l_True)
        // treat satisfied clauses as if they are too big.
        nlits = 3;
      if (nlits > 2) break;
    }
    if (nlits == 2) {
      int sz{};
      for (auto l : clause)
        if (sat_solver->fixedValue(l) == l_Undef) {
          binaries.push_back(l);
          sz++;
        }
      if (sz != 2) {
        cout << "c ERROR in WCNF get_binaries...binary of size " << sz
             << "\n [";
        for (auto l : clause) {
          cout << l << ":" << sat_solver->fixedValue(l);
          cout << ", ";
        }
        cout << "]\n";
      }
    }
  }
  return binaries;
}

Packed_vecs<Lit> Wcnf::reduce_by_eqs_and_units(const Packed_vecs<Lit>& clauses,
                                               bool softs) {
  if (unsat) return Packed_vecs<Lit>{};

  vector<lbool> truth_vals(2 * nVars(), l_Undef);
  vector<Lit> eqLit(2 * nVars());

  for (size_t i = 0; i < nVars(); i++) {
    auto lt = mkLit(i);
    eqLit[toInt(lt)] = lt;
    eqLit[toInt(~lt)] = ~lt;
  }

  for (auto lt : hard_units) {
    truth_vals[toInt(lt)] = l_True;
    truth_vals[toInt(~lt)] = l_False;
  }

  for (const auto& scc : all_scc)
    for (size_t i = 0; i < scc.size(); i++) {
      eqLit[toInt(scc[i])] = scc[0];
      eqLit[toInt(~scc[i])] = ~scc[0];
    }

  Packed_vecs<Lit> tmp;
  size_t j = 0;
  vector<Lit> c;
  for (size_t i = 0; i < clauses.size(); i++) {
    c.clear();
    bool isSat{false};
    for (auto l : clauses[i]) {
      auto eqL = eqLit[toInt(l)];
      if (truth_vals[toInt(eqL)] == l_Undef) c.push_back(eqL);
      if (truth_vals[toInt(eqL)] == l_True) {
        isSat = true;
        break;
      }
    }
    if (isSat)
      continue;
    else if (c.empty()) {
      if (!softs) {
        // empty hards should be caught when clauses added to sat solver.
        cout << "c ERROR: Wcnf::reduce_by_units found empty hard clause\n";
        unsat = true;
        return Packed_vecs<Lit>{};
      }
      base_cost += soft_clswts[i];
    } else if (prepareClause(c)) {
      tmp.addVec(c);
      // Note equality replacement might generate new units. But all units
      // passed in hard_units will be satisfied...do not added to the updated
      // clauses.
      if (softs) soft_clswts[j++] = soft_clswts[i];
    }
  }

  if (softs) {
    soft_clswts.resize(j);
    soft_clswts.shrink_to_fit();
    total_cls_wt = 0;
    for (auto wt : soft_clswts) total_cls_wt += wt;
  }
  return tmp;
}

vector<vector<Lit>> Wcnf::binary_scc(vector<vector<Lit>>& edges) {
  // find strongly connected components of binary implication graph
  //(BIG) of size > 1. The BIG has duality. So if x is in an SCC -x
  // will be in a dual SCC. Only return the first of these dual SCCs
  // found.
  // We ignore the possibility that the BIG might have implied
  // units.  These could be checked for after scc detection if wanted.
  // Edges are an adjacency map edges[toInt(l)] = {y1, y2, ..., yk} iff
  // the binary clauses (l, y1), ..., (l, yk) exist.
  //
  // The nodes of the graph can be viewed to be
  //  literals l, where toInt(l) is index of l in edges
  //  OR indicies i into edges, where toLit(i) is the literal of that node.
  //  We use both--dependent on what is convenient.

  // We use a stack for dfs. Push when a node is first explored, pop
  // when a node is finished.

  // stacks to hold the tentative sccs (store nodes as indicies)
  vector<int> unfinished;
  vector<int> roots;

  // dfsnum---DFS visit order of a node (as an index). -1 not yet visited
  vector<int> dfsnum(edges.size(), -1);
  int dfscount{0};

  // component number---scc number node (as an index) is in. -1 not yet
  // processed
  vector<int> comp_num(edges.size(), -1);
  int comp_count{0};

  // dfs stack---a pair, node index and number of child to be processed next.
  // initially a for a node n we push (n,0)---first child to be visited next.
  // at final stage stack has (n, k) where k >= edges[n].size() indicating
  // that all children have been processed---time to finish n.
  vector<pair<int, int>> dfs_stack;

  // hold the return set of sccs
  vector<vector<Lit>> all_sccs;

  for (int nd = 0; static_cast<size_t>(nd) < edges.size(); nd++) {
    if (dfsnum[nd] != -1) continue;  // already explored

    dfs_stack.push_back({nd, 0});

    while (!dfs_stack.empty()) {
      auto n_ci = dfs_stack.back();
      auto node = n_ci.first;
      auto neg_node = toInt(~toLit(node));
      auto childi = n_ci.second;  // index into neg_node's binary
                                  // clauses...these are implicants of node

      if (childi == 0) {
        // 0 we are first visiting node. Mark its visit number and put
        // it in its own tentative scc.
        dfsnum[node] = dfscount++;
        unfinished.push_back(node);
        roots.push_back(node);
      }

      if (static_cast<size_t>(childi) >= edges[neg_node].size()) {
        // processed all children...finish the node.
        dfs_stack.pop_back();
        if (node == roots.back()) {
          vector<Lit> scc;
          int w{};
          do {
            w = unfinished.back();
            unfinished.pop_back();
            comp_num[w] = comp_count;
            if (comp_num[neg_node] == -1)  // dual scc not processed
              scc.push_back(toLit(w));
          } while (w != node);
          comp_count++;
          roots.pop_back();
          if (scc.size() > 1) all_sccs.push_back(std::move(scc));
        }
      } else {
        // explore next child and update stack entry
        dfs_stack.back().second++;
        auto w = toInt(edges[neg_node][childi]);
        if (dfsnum[w] == -1)  // put on stack to explore.
          dfs_stack.push_back({w, 0});
        else if (comp_num[w] == -1) {
          // merge scc
          while (dfsnum[roots.back()] > dfsnum[w]) roots.pop_back();
        }
      }
    }
  }
  return all_sccs;
}

void Wcnf::remDupCls() {
  /*compute hash code for each clause. Use this to detect identical
    clauses hash code for units is the hash code of the var---so that
    -x and x get the same hash code and we can detect this clash. */
  if (noDups || unsat) return;
  noDups = true;

  vector<ClsData> cdata;
  initClsData(cdata);
  auto byHash = [](const ClsData& a, const ClsData& b) {
    return a.hash < b.hash;
  };
  sort(cdata.begin(), cdata.end(), byHash);

  Packed_vecs<Lit> tmpH, tmpS;
  vector<Weight> tmp_wts;

  for (size_t i = 0; i < cdata.size(); i++) {
    if (cdata[i].w == 0) continue;  // w==0 indicates clause is deleted;

    uint32_t i_index{cdata[i].index};
    bool ihard{cdata[i].w < 0};
    auto vi = ihard ? hard_cls[i_index] : soft_cls[i_index];
    for (size_t j = i + 1; j < cdata.size() && cdata[i].hash == cdata[j].hash;
         j++) {
      if (cdata[j].w == 0) continue;

      uint32_t j_index{cdata[j].index};
      bool jhard{cdata[j].w < 0};
      auto vj = jhard ? hard_cls[j_index] : soft_cls[j_index];
      if (vi.size() == 1 && vj.size() == 1 &&
          vi[0] == ~vj[0]) {  // contradictory units
        if (ihard && jhard) {
          unsat = true;
          return;
        } else if (ihard || jhard) {  // soft is falsified
          if (jhard) vi[0] = vj[0];
          auto cost = jhard ? getWt(i_index) : getWt(j_index);
          base_cost += cost;
          cdata[j].w = 0;
          cdata[i].w = -1;
        } else {  // neither is hard. This is a resolution of unit softs.
          Weight cost{}, residue{};
          if (cdata[i].w < cdata[j].w) {
            vi[0] = vj[0];  // higher cost unit is preserved
            cost = cdata[i].w;
            residue = cdata[j].w - cost;
          } else {  // note that if costs are equal residue becomes 0 and both
                    // clauses vanish
            cost = cdata[j].w;
            residue = cdata[i].w - cost;
          }
          base_cost += cost;
          cdata[i].w = residue;
          cdata[j].w = 0;
        }
      }

      else if (eqVecs(cdata[i], cdata[j])) {  // equal clauses are merged.
        if (ihard || jhard) {  // hard subsumes both soft and hard.
          cdata[i].w = -1;
          cdata[j].w = 0;
        } else {
          cdata[i].w += cdata[j].w;  // join softs.
          cdata[j].w = 0;
        }
      }
    }
    // finished processing all clauses that match with vi.
  }

  auto byIndex = [](const ClsData& a, const ClsData& b) {
    return a.index < b.index;
  };
  sort(cdata.begin(), cdata.end(), byIndex);

  for (size_t i = 0; i < cdata.size(); i++) {
    auto i_index = cdata[i].index;
    auto ohard = cdata[i].origHard;
    auto vi = ohard ? hard_cls[i_index] : soft_cls[i_index];

    if (cdata[i].w < 0) {
      tmpH.addVec(vi.getVec());
      // cout << "i = " << i << " Added hard " << vi.getVec() << "\n\n";
    } else if (cdata[i].w > 0) {
      tmpS.addVec(vi.getVec());
      tmp_wts.push_back(cdata[i].w);
      // cout << "i = " << i << " Added soft " << vi.getVec() << " weight = " <<
      // cdata[i].w << "\n\n";
    }
  }

  size_t ph = hard_cls.size();
  size_t ps = soft_cls.size();

  hard_cls = std::move(tmpH);
  soft_cls = std::move(tmpS);
  soft_clswts = std::move(tmp_wts);
  total_cls_wt = 0;
  for (auto wt : soft_clswts) total_cls_wt += wt;

  if (params.verbosity > 0)
    cout << "c WCNF found " << ph - hard_cls.size() << " redundant hards and "
         << ps - soft_cls.size() << " duplicate or subsumed softs\n";
}

bool Wcnf::eqVecs(const ClsData& a, const ClsData& b) {
  // auxilary function for rmDupCls
  // relies on all clauses being sorted on input.

  size_t sa = (a.w < 0) ? hardSize(a.index) : softSize(a.index);
  size_t sb = (b.w < 0) ? hardSize(b.index) : softSize(b.index);
  if (sa != sb) return false;

  auto va = (a.w < 0) ? getHard(a.index) : getSoft(a.index);
  auto vb = (b.w < 0) ? getHard(b.index) : getSoft(b.index);
  for (size_t i = 0; i < va.size(); i++)
    if (va[i] != vb[i]) return false;
  return true;
}

void Wcnf::initClsData(vector<ClsData>& cdata) {
  // auxilary function for rmDupcls.
  // units are hashed as variables not as lits.
  for (size_t i = 0; i < nHards(); i++)
    if (hardSize(i) == 1) {
      vector<Var> unit{var(hard_cls[i][0])};
      cdata.emplace_back(static_cast<uint32_t>(i),
                         hashCode(unit.begin(), unit.end()), -1, true);
    } else
      cdata.emplace_back(static_cast<uint32_t>(i),
                         hashCode(hard_cls[i].begin(), hard_cls[i].end()), -1,
                         true);

  for (size_t i = 0; i < nSofts(); i++)
    if (softSize(i) == 1) {
      vector<Var> unit{var(soft_cls[i][0])};
      cdata.emplace_back(static_cast<uint32_t>(i),
                         hashCode(unit.begin(), unit.end()), getWt(i), false);
    } else
      cdata.emplace_back(static_cast<uint32_t>(i),
                         hashCode(soft_cls[i].begin(), soft_cls[i].end()),
                         getWt(i), false);

  if (params.verbosity > 2) {
    vector<uint32_t> hcodes;
    for (auto& item : cdata) hcodes.push_back(item.hash);
    sort(hcodes.begin(), hcodes.end());
    auto it = std::unique(hcodes.begin(), hcodes.end());
    hcodes.resize(std::distance(hcodes.begin(), it));
    cout << "c Hashed " << cdata.size() << " clauses with "
         << it - hcodes.begin() << " (" << hcodes.size()
         << ") different hash codes\n";
  }
}

void Wcnf::simpleHarden() {
  // Check if we can make some soft clauses hard before we do other
  // preprocessing that might be aided by additional hard clauses.
  // This hardening uses the transition weights if there are any.
  // In particular W is a transition weight if the sum of the weights
  // of the clauses with weight less than W is less than W:
  // \sum_{soft clauses c s.t. wt(c) < W} wt(c) < W
  //
  // This means that we would prefer to falsify all clauses of weight
  // less than W rather than a single clause of weight >= W.  We check
  // to see if the set of soft clauses with weight >= W are satisfiable
  //(in conjunction with the hard clauses), i.e., the set
  // HIGHWT = {c| wt(c) >= W or c is hard}.

  // If these clauses HIGHWT are satisfiable then there is no need to
  // ever falsify any clause in HIGHWT, and we can harden these
  // clauses.

  if (unsat) return;
  computeWtInfo();
  // cout << "BEFORE HARDENING\n";
  // printFormula();

  SatSolver_uniqp sat_solver{new cadicalSolver};
  for (size_t i = 0; i < nHards(); i++) sat_solver->addClause(getHard(i));
  if (sat_solver->theory_is_unsat()) {
    unsat = true;
    if (params.verbosity > 0) cout << "c WCNF found hards are unsat\n";
    return;
  }

  // Debug
  /*cout << "Diffwts = [";
    for(size_t i=0; i< diffWts.size(); i++)
    cout << "(" << diffWts[i] << ", " << diffWtCounts[i] << "), ";
    cout << "]\n";*/
  // Debug

  Weight maxHardenWt = wt_max + 1;
  Weight maxWt = wt_max + 1;

  if (params.verbosity > 0)
    cout << "c transitionWts = " << transitionWts << "\n";

  for (int i = transitionWts.size() - 1; i >= 0; i--) {
    // DEBUG
    // cout << "Processing wt " << transitionWts[i] << " maxWt = " << maxWt <<
    // "\n";
    int n{0};

    for (size_t c = 0; c < nSofts(); c++) {
      if (soft_clswts[c] >= transitionWts[i] && soft_clswts[c] < maxWt) {
        n++;
        sat_solver->addClause(getSoft(c));
        if (sat_solver->theory_is_unsat()) break;
      }
    }

    // DEBUG
    // cout << "Added " << n << " soft clauses to sat solver\n";

    if (sat_solver->theory_is_unsat()) break;
    maxWt = transitionWts[i];
    auto canHarden = sat_solver->solvePropBudget(1024 * 1024);
    // DEBUG
    // cout << "sat solver returns " << canHarden << "\n";

    if (canHarden == l_True) {
      maxHardenWt = transitionWts[i];
      // DEBUG
      // cout << "Incrementing hardening wt to " << maxHardenWt << "\n";
    } else
      break;
  }

  // DEBUG
  // cout << "maxhardenwt = " << maxHardenWt << " wt_max = " << wt_max << "\n";

  if (maxHardenWt > wt_max) {
    if (params.verbosity > 0) cout << "c WCNF hardened 0 soft clauses\n";
    return;
  }

  Packed_vecs<Lit> tmp;
  vector<Weight> tmpWts;

  int nHardened{0};
  for (size_t i = 0; i < nSofts(); i++)
    if (soft_clswts[i] >= maxHardenWt) {
      nHardened++;
      auto sftcls = getSoft(i);
      _addHardClause(sftcls);
    } else {
      tmp.addVec(getSoft(i));
      tmpWts.push_back(soft_clswts[i]);
    }
  soft_cls = std::move(tmp);
  soft_clswts = std::move(tmpWts);

  total_cls_wt = 0;
  for (auto wt : soft_clswts) total_cls_wt += wt;

  if (params.verbosity > 0)
    cout << "c WCNF hardened " << nHardened
         << " soft clauses. New total_cls wt = " << wt_fmt(total_cls_wt)
         << "\n";

  // cout << "AFTER HARDENING\n";
  // printFormula();
}

void Wcnf::mxBvars() {
  // modify the WCNF by finding at most one constraints among the bvars
  // and replacing all of these vars by one bvar.
  Bvars bvars{this};
  if (params.verbosity > 4) {
    cout << "BEFORE mxbvars\n";
    printFormula(bvars);
  }
  processMxs(mxFinder(bvars), bvars);
  if (params.verbosity > 4) {
    Bvars newbvars{this};
    cout << "AFTER mxbvars\n";
    printFormula(newbvars);
  }
}

void Wcnf::processMxs(vector<vector<Lit>> mxs, Bvars& bvars) {
  // mxs should be disjoint collection of mx sets. Each set should be a
  // non empty set of blits all of which have the same weight. These
  // blits have the property that at most one of them can be true
  //(given the hard clauses).

  // If the blits are cores (making them true relaxes the soft clause),
  // then at most one of the corresponding soft clauses can be
  // falsified. If the blits are non-cores then at most one of the
  // corresponding soft clauses can be true.

  // Modify the WCNF to account for this mx.
  // Also store the mxes so that users of Wcnf can access them.

  if (unsat) return;

  // debug
  // cout << "Before processmx\n";
  // printFormula(bvars);

  vector<char> delMarks(nSofts(), 0);
  auto orig_nsofts = nSofts();
  vector<Lit> blits{};
  for (auto& mx : mxs) {
    // debug
    /*cout << "Processing " << (bvars.isCore(mx[0]) ? "Core Mx: [ " : "NonCore
    Mx: [ "); for(auto l : mx) cout << l << "(ci = " << bvars.clsIndex(l) << "),
    "; cout << "]\n"; if(mx.size() < 2) cout << "ERROR: Got mutual exclusion
    with less than 2 vars" << mx << "\n"; bool dc = bvars.isCore(mx[0]);
    for(auto l : mx) {
      if(dc && !bvars.isCore(l) || !dc && bvars.isCore(l))
        cout << "ERROR mx with mixed core/non-cores" << mx << "\n";
      if(bvars.wt(var(l)) != bvars.wt(var(mx[0])))
      cout << "ERROR mx with different wts" << mx << "\n";
      }*/
    // debug
    if (mx.empty()) {
      cout << "c WARNING. Mx finder returned empty mx\n";
      continue;
    }
    Weight unitWt = mx.size() > 0 ? bvars.wt(var(mx[0])) : 0.0;
    bool core = mx.size() > 0 ? bvars.isCore(mx[0]) : false;
    auto dvar = var_Undef;
    auto dlit = lit_Undef;
    blits.clear();
    // if the mx is a core-mx it means that of a set of soft clauses
    // at most one can be false. One could define a new variable d
    // that is true if one of these soft clauses if false, and then a
    // unit clause (-d) asking the solver to satisfy all of the soft
    // clauses.  However, having the solver assume -d leads to weaker
    // conflicts than having it assume (-b1, -b2, ..., -bk) (where
    // these are the soft clauses in the mutex. So we do not do the
    // transformation, but we do obtain a bvar for each soft and tell
    // the solver that these bvars are mutex (this info can be added
    // to the hitting set solvers.
    if (core) {
      for (auto l : mx) {
        auto ci = bvars.clsIndex(l);
        auto sftcls = getSoft(ci);
        if (sftcls.size() == 0) {
          cout << "c ERROR WCNF processMxs encountered zero length soft "
                  "clause\n";
          continue;
        } else if (sftcls.size() == 1)
          blits.push_back(~sftcls[0]);
        else {
          auto blit = mkLit(bvars.newBVar());
          blits.push_back(blit);
          sftcls.push_back(blit);
          delMarks[ci] = 1;
          _addHardClause(sftcls);
          _addSoftClause(~blit, unitWt);
        }
      }
      mutexes.emplace_back(blits, core, lit_Undef);
      // dvar = bvars.newBvar();
      // dlit = mkLit(dvar);
      // for (auto l : mx) {
      //   auto ci = bvars.clsIndex(l);
      //   auto sftcls = getSoft(ci);
      //   if (sftcls.size() == 0) {
      //     cout << "c ERROR WCNF processMxs encountered zero length soft "
      //             "clause\n";
      //     continue;
      //   }
      //   sftcls.push_back(dlit);
      //   delMarks[ci] = 1;
      //   _addHardClause(sftcls);
      // }
      // _addSoftClause(~dlit, unitWt);
    } else {
      // If the mutex is a non-core then there is a set of soft clauses at most
      // one of which can be TRUE. So we can add the wt of all but one to base
      // cost. In additon the transformation to a single new soft clause
      // (-d) where -d == one is TRUE, (d == all are false) is beneficial. As
      // the solver assuming -d corresponds to assuming a disjunction (-b1 \/
      // -b2 \/ ... \/ -bk) (where the bi are the softs in the mutex. Assuming
      // this disjunction leads to more powerful clauses than picking an
      // arbitrary -bi to make true (i.e., an arbitrary soft clause to make
      // true).
      //
      // We encode d as -d -> the disjunction of all literals in these
      // soft clauses. But TODO we should also check the approach of  Bacchus
      // and Katesirelos CAV 2015 of introducing a b-var for each soft
      // clause and then have -d -> the disjunction of these bvars

      for (auto l : mx) {
        auto ci = bvars.clsIndex(l);
        auto sftcls = getSoft(ci);
        if (sftcls.size() == 0) {
          cout << "c ERROR WCNF processMxs encountered zero length soft "
                  "clause\n";
          continue;
        }
        for (auto x : sftcls) blits.push_back(x);  // union of softs
        delMarks[ci] = 1;
      }
      dvar = bvars.newBVar();
      dlit = mkLit(dvar);
      blits.push_back(dlit);
      _addHardClause(blits);
      base_cost += unitWt * (mx.size() - 1);
      _addSoftClause(~dlit, unitWt);

      // if (!orig_usoft.empty())
      //   mutexes.emplace_back(std::move(orig_usoft), coreMx, lit_Undef);
    }
  }

  // now rewite the softs
  Packed_vecs<Lit> tmp;
  size_t j{0};
  for (size_t i = 0; i < nSofts(); i++)
    if (i >= delMarks.size() || !delMarks[i]) {
      // delmarks don't extend to newly added softs
      tmp.addVec(getSoft(i));
      soft_clswts[j++] = soft_clswts[i];
    }
  soft_clswts.resize(j);
  soft_clswts.shrink_to_fit();
  soft_cls = std::move(tmp);

  total_cls_wt = 0;
  for (auto wt : soft_clswts) total_cls_wt += wt;
  computeWtInfo();

  if (params.verbosity) {
    cout << "c WCNF mutexes: original #softs " << orig_nsofts
         << " #softs after mx-transforms " << nSofts() << "\n";
    cout << "c WCNF mutexes: reduction in softs " << orig_nsofts - nSofts()
         << "\n";
  }

  if (params.verbosity > 2) {
    cout << "Process mx\n";
    cout << "mutexes\n";
    for (auto& mx : mutexes) cout << mx << "\n";
  }

  // debug
  // Bvars newbvars {this};
  // cout << "After processmx\n";
  // printFormula(newbvars);
}

class MXFinder {
  // helper class for finding mutually exclusive bvars
 public:
  MXFinder(Wcnf* f, Bvars& b)
      : bvars{b},
        theWcnf{f},
        sat_solver{new cadicalSolver},
        blitMarks(2 * bvars.n_vars(), 0) {}
  ~MXFinder() {
    for (auto p : blitMXes)
      if (p) delete p;
  }
  bool findMxs(vector<vector<Lit>>&);
  int nImpCalls{0};

 private:
  Bvars bvars;
  Wcnf* theWcnf;
  SatSolver_uniqp sat_solver;
  vector<uint8_t> blitMarks;
  const uint8_t inmx{1};
  const uint8_t in2s{2};
  bool fbeq();
  size_t getMXLitSize(Lit);
  const vector<Lit>* getMXLits(Lit);
  uint64_t totalMxMem{};
  vector<vector<Lit>*> blitMXes;
  void getMXRecomputeSizes(const vector<Lit>&);
  vector<Lit> growMx(Lit start);
  // debug
  void MXprintLit(Lit l) {
    cout << l << " (mkr=" << (int)blitMarks[toInt(l)]
         << (bvars.isCore(l) ? " C " : " NC ") << " wt = " << bvars.wt(var(l))
         << ") ";
  }
};

vector<vector<Lit>> Wcnf::mxFinder(Bvars& bvars) {
  // Return collection of mutally exclusive bvars. Where each set of
  // Bvars are cores or are non-cores and each is associated with a
  // soft clause of identical weight; finder does all the work: we use
  // a function wrapper to ensure MXFinder is deallocated after it has
  // done its work.
  MXFinder finder{this, bvars};
  vector<vector<Lit>> mxs;
  if (!finder.findMxs(mxs)) unsat = true;
  if (params.verbosity > 0)
    cout << "c WCNF mx finder used " << finder.nImpCalls
         << " calls to UP engine\n";
  return mxs;
}

bool MXFinder::findMxs(vector<vector<Lit>>& mxs) {
  // Top level findMxs method of MXFinder class.  find mx collections
  // of bvars and store in mxs. Return false if we found a
  // contradiction.

  // TODO: currently no attempt is made to reclaim
  // memory during the computation.  In particular, once a blit is in
  // an MX we shouldn't have to store its set of implicants. (BUT the
  // marking code is tricky so this is not necessarily an easy mod,
  // this needs a redesign..

  bool timedOut{false};
  double start_time = Minisat::cpuTime();

  // 1. Initialize solver with fbeq
  if (!fbeq()) {
    if (params.verbosity > 0)
      cout << "c WCNF detected input to be unsat during preprocessing\n";
    return false;
  }

  // Two stage. Note that absorbing a blit into a mutex blocks it and
  // its negation from being in any other mutex. So try to grow big
  // mutexes we delay the processing of mutexes of size 2. E.g., it
  // could be that (b1,b2) are mutex but so is (b2, b3, b4, b5) so we
  // don't want to absorbe b2 into (b1, b2).

  vector<Lit> toProcess;
  vector<Lit> twos;  // blits that might generate 2s. Process later.

  // toProcess will be used as a stack. Non core mx bump the base cost
  // so process those first (put at back of vector--top of stack)
  bool find_cores = params.mx_find_mxes == 3 || params.mx_find_mxes == 1;
  bool find_ncores = params.mx_find_mxes == 3 || params.mx_find_mxes == 2;
  if (find_cores)
    for (size_t i = 0; i < theWcnf->nSofts(); ++i) {
      toProcess.push_back(bvars.litOfCls(i));
    }
  if (find_ncores)
    for (size_t i = 0; i < theWcnf->nSofts(); ++i) {
      toProcess.push_back(~bvars.litOfCls(i));
    }

  int loops{0};
  while (!toProcess.empty()) {
    // in this loop toProcess.back() is either selected, or blits that
    // are mx with it are processed. Thus the size of
    // toProcess.back()'s mx lit set is decreased. So eventually toProcess
    // will become empty.
    ++loops;
    // check for time out and mem-limit.
    if (totalMxMem >=
            static_cast<uint64_t>(1024 * 1024) * params.mx_mem_limit ||
        (params.mx_cpu_lim > 0 && loops % 500 == 0 &&
         (Minisat::cpuTime() - start_time) > params.mx_cpu_lim)) {
      timedOut = true;
      if (totalMxMem >=
          static_cast<uint64_t>(1024 * 1024) * params.mx_mem_limit)
        cout << "c WCNF mx finder hit its memory limit. "
             << "Potentially more mxes could be found with -mx-mem-lim made "
                "larger\n";
      if ((Minisat::cpuTime() - start_time) > params.mx_cpu_lim)
        cout << "c WCNF mx finder hit its time limit. "
             << "Potentially more mxes could be found with -mx-cpu-lim made "
                "larger\n";
      break;
    }

    Lit blit = toProcess.back();
    if (blitMarks[toInt(blit)]) {
      // in an mx or in twos
      toProcess.pop_back();
      continue;
    }
    auto mx = getMXLits(blit);

    // Debug
    /*cout << "findmxs first loop unmarked processing ";
      MXprintLit(blit);
      cout << " mx = [";
      for(auto p : *mx) {
      MXprintLit(p);
      cout << " (" << getMXLitSize(p) << "), ";
      }
      cout << "]\n";*/

    if (mx->size() <= 1) {
      if (mx->size() == 1) {
        blitMarks[toInt(blit)] = in2s;
        twos.push_back(blit);
        // cout << "Pushed into twos\n";
      }
      // cout << "Got rid of\n";
      toProcess.pop_back();
      continue;
    }

    // Potential mx of size > 2 (but not guaranteed!)
    // cout << "Growing from start ";

    Lit start = blit;
    size_t size{mx->size()};

    // Find start.
    for (auto l : *mx) {
      size_t sz = getMXLitSize(l);
      if (sz > size) {
        size = sz;
        start = l;
      }
    }

    // cout << start << "\n";
    // grow from start
    auto tmp = growMx(start);

    // tmp might be small and it might not contain blit
    if (tmp.size() <= 2) {
      // cout << "too small\n";

      // no easy way to get rid of start from toProcess
      // so mark as being in twos
      blitMarks[toInt(blit)] = in2s;
      if (tmp.size() == 2) {
        // but only put there if it has potential
        twos.push_back(start);
        // cout << "put in twos\n";
      }
    }

    else {  // legit mx for this stage
      for (auto b : tmp) {
        // cout << "adding mx\n";
        blitMarks[toInt(b)] = inmx;
        blitMarks[toInt(~b)] = inmx;
      }
      mxs.push_back(std::move(tmp));
    }
  }

  if (!timedOut)
    while (!twos.empty()) {
      Lit blit = twos.back();
      // cout << "Processing twos " << blit << "\n";
      twos.pop_back();
      if (blitMarks[toInt(blit)] == inmx) {
        // cout << "Already marked\n";
        continue;
      }
      auto tmp = growMx(blit);
      if (tmp.size() > 1) {
        if (tmp.size() > 2) cout << "c WARNING. WCNF large mx got into twos\n";
        for (auto b : tmp) {
          blitMarks[toInt(b)] = inmx;
          blitMarks[toInt(~b)] = inmx;
        }
        mxs.push_back(std::move(tmp));
      }
    }

  if (params.verbosity > 0) {
    cout << "c WCNF mutexes: #mutexes found = " << mxs.size() << "\n";
    if (mxs.size() > 0) {
      double core_total_size{0};
      double ncore_total_size{0};
      int cores{0};
      int ncores{0};
      for (auto& mx : mxs) {
        if (bvars.isCore(mx[0])) {
          cores++;
          core_total_size += mx.size();
        } else {
          ncores++;
          ncore_total_size += mx.size();
        }
      }
      cout << "c WCNF mutexes: #cores mutexes = " << cores;
      if (cores) cout << " ave. size = " << fix4_fmt(core_total_size / cores);
      cout << "\n";
      cout << "c WCNF mutexes: #non-cores mutexes = " << ncores;
      if (ncores)
        cout << " ave. size = " << fix4_fmt(ncore_total_size / ncores);
      cout << "\n";
      cout << "c WCNF mutexes: time used = "
           << time_fmt(Minisat::cpuTime() - start_time) << "\n";
    }
  }
  return true;
}

vector<Lit> MXFinder::growMx(Lit start) {
  // Starting with blit start, grow a mx constraint (at most one)
  // The algorithm is as follows.
  // We have a set mx that is initially start, and a set of candidates
  // initially the negations of the appropriate implications of start
  //(returned by getMXLits)
  // Appropriate means that these negations are cores if start is a core
  // and they are non-cores if start is a non-core. Furthermore, none
  // of them is marked (as used in another mx constraint), and they all
  // correspond to soft clauses with identical weight as start's soft clause.
  // These restrictions are maintained by the subroutine getMXLits

  // The algorithm's invariant is that
  //(a) the lits in mx form an mx constraint
  //(b) every single lit in candidates is mx with each lit in mx
  //    Thus mx can be grown by any member of candidates.

  // 1. All l in candidates are MX (mutually exclusive) with all c in
  //   mx. So when we move l from candidates to mx (i.e., we select l)
  //   we must remove from candiates all l' that are not MX with l.
  //   Here we sort candidates by |mxset(l)\cap candidates|. So that
  //   if l is selected we can retain as many members of candidates as
  //   is possible. We only sort at the start. A greedy method would
  //   always recompute the size of this intersection since candidates
  //   is shrinking after every l is selected. But we don't do that
  //   to avoid this cost.

  vector<Lit> origCandidates(*getMXLits(start));  // need copy
  std::set<Lit> candidates(origCandidates.begin(), origCandidates.end());

  // compute |mxset(l) \cap candidates| or all l\in candidates
  vector<int> interSize;
  for (auto l : origCandidates) {
    int count{0};
    int ci{bvars.clsIndex(l)};
    for (auto l1 : *getMXLits(l))
      if (candidates.find(l1) != candidates.end()) ++count;
    if (static_cast<size_t>(ci) >= interSize.size())
      interSize.resize(ci + 1, 0);
    interSize[ci] = count;
  }

  auto mxSize = [this, &interSize](Lit l1, Lit l2) {
    return interSize[bvars.clsIndex(l1)] > interSize[bvars.clsIndex(l2)];
  };
  sort(origCandidates.begin(), origCandidates.end(), mxSize);

  // debug*****
  /*cout << "growMX on " << start << "\n";
    cout << start << " mx = " << *getMXLits(start) << "\n";
    cout << "candidiates\n";
    for(size_t i = 0; i < origCandidates.size(); i++) {
    auto l = origCandidates[i];
    auto ci = bvars.clsIndex(l);
    cout << l << " mx = "
    << *getMXLits(l) << " [" << interSize[ci] << "]\n";
    }*/
  // debug*****

  vector<Lit> mx{start};
  size_t cani{0};
  while (!candidates.empty()) {
    /*cout << "Loop #" << cani << " candidiates [";
      for(auto l : candidates)
      cout << l << ", ";
      cout << "]\n";*/

    auto l = origCandidates[cani++];  // select according to computed order
    // cout << " growing " << cani << " lit = " << l;
    auto it = candidates.find(l);
    if (it == candidates.end()) {
      // cout << " Not found\n";
      continue;
    }
    // cout << " Found\n";
    mx.push_back(l);
    candidates.erase(it);
    // remove from candidates all elements not mx with the newly selected l
    auto l_mx = getMXLits(l);
    std::set<Lit> newLitMx(l_mx->begin(), l_mx->end());
    for (auto p = candidates.begin(); p != candidates.end();) {
      // cout << "checking active candiate " << *p;
      if (newLitMx.find(*p) == newLitMx.end()) {
        // this candidate not mx with newly added lit---not a candidate anymore;
        // cout << " Pruning\n";
        candidates.erase(p++);  // sets! Erase invalidates p, so pass copy and
                                // increment here.
      } else {
        ++p;
        // cout << " Keeping\n";
      }
    }
  }

  // debug
  // cout << "Grow found mx " << mx << "\n";
  /*for(size_t i=0; i<mx.size(); i++)
    for(size_t j=i+1; j < mx.size(); j++)
    if(!solver.isMX(mx[i], mx[j])) {
        cout << "ERROR MX faulty (" << mx[i] << ", " << mx[j] << ")\n";
        break;
    }
    for(size_t i=0; i< mx.size(); i++)
    if(    (bvars.isCore(mx[0]) && bvars.isNonCore(mx[i]))
        || (bvars.isNonCore(mx[0]) && bvars.isCore(mx[i]))
    || (bvars.wt(var(mx[0])) != bvars.wt(var(mx[0])))   ) {
    cout << "ERROR MX faulty (" << mx[0] << ", " << mx[i] << ")\n";
    break;
    }*/

  return mx;
}

bool MXFinder::fbeq() {
  // Add fbeq to solver.
  for (size_t i = 0; i < theWcnf->nHards(); i++)
    sat_solver->addClause(theWcnf->getHard(i));
  if (sat_solver->theory_is_unsat()) return false;

  for (size_t i = 0; i < theWcnf->nSofts(); i++) {
    Lit blit = bvars.litOfCls(i);
    if (theWcnf->softSize(i) > 1) {
      vector<Lit> sftCls{theWcnf->getSoft(i)};
      sftCls.push_back(blit);
      sat_solver->addClause(sftCls);
      if (sat_solver->theory_is_unsat()) return false;

      vector<Lit> eqcls(2, lit_Undef);
      eqcls[1] = ~blit;
      for (auto l : theWcnf->softs()[i]) {
        eqcls[0] = ~l;
        sat_solver->addClause(eqcls);
        if (sat_solver->theory_is_unsat()) return false;
      }
    }
  }
  return true;
}

const vector<Lit>* MXFinder::getMXLits(Lit l) {
  // return *unmarked* literals of same type (same wt and same core status) that
  // are mutually exclusive with l.
  // This is accomplished by asking the sat solver to find the implications
  // of l that have the same weight and the opposite core status.
  // If l --> l1, then l and ~l1 are mutually exclusive.
  // To make this more efficient we remember the compute sets in blitMXex.
  //
  // Since literals are marked at various stages, we must prune these cached
  // sets to remove newly marked literals (??do we need to do this??)

  Weight lWT{0};  // rather than set up a class of template, we capture this
                  // local var in our lambda.
  auto not_coreMX = [this, &lWT](Lit l1) {
    return blitMarks[toInt(l1)] == inmx || !bvars.isNonCore(l1) ||
           bvars.wt(var(l1)) != lWT;
  };
  auto not_nonCoreMX = [this, &lWT](Lit l1) {
    return blitMarks[toInt(l1)] == inmx || !bvars.isCore(l1) ||
           bvars.wt(var(l1)) != lWT;
  };

  if (blitMXes.size() <= static_cast<size_t>(toInt(l)))
    blitMXes.resize(toInt(l) + 1, nullptr);

  vector<Lit> imps;
  if (!blitMXes[toInt(l)]) {  // not cached compute
    if (totalMxMem >=
        static_cast<uint64_t>(1024 * 1024) * params.mx_mem_limit) {
      // no more space for storing implications (2GB)...pretend that there
      // aren't any
      blitMXes[toInt(l)] = new vector<Lit>(std::move(imps));
      return blitMXes[toInt(l)];
    }

    lWT = bvars.wt(var(l));
    ++nImpCalls;
    sat_solver->findImplications(l, imps);

    if (bvars.isCore(l))
      imps.erase(std::remove_if(imps.begin(), imps.end(), not_coreMX),
                 imps.end());
    else
      imps.erase(std::remove_if(imps.begin(), imps.end(), not_nonCoreMX),
                 imps.end());

    // debug
    /*vector<Lit> t;
      t.push_back(l);
      vector<Lit> o;
      vector<Lit> pruned;
      sat_solver->findImplications(t, o);
      t.clear();
      for(size_t i=0; i < o.size(); ++i) {
      if(blitMarks[toInt(o[i])] != inmx &&
      ((bvars.isCore(l) && bvars.isNonCore(o[i]))
          || (bvars.isNonCore(l) && bvars.isCore(o[i])))
      && (bvars.wt(var(l)) == bvars.wt(var(o[i]))))
      pruned.push_back(o[i]);
      }

      std::sort(pruned.begin(), pruned.end());
      std::sort(imps.begin(), imps.end());
      if(pruned.size() != imps.size()) {
      cout << "ERROR regular interface returned different set of imps for ";
      cout << l << " (marks = " << (int) blitMarks[toInt(l)] << (bvars.isCore(l)
      ? " Core " : " nonCore "); cout << " wt = " << bvars.wt(var(l)) << ")\n";

      cout << " imps = [ ";
      for(auto x : imps)
      cout << x << " (marks = " << (int) blitMarks[toInt(x)] << (bvars.isCore(x)
      ? " Core " : " nonCore ")
      << " wt = " << bvars.wt(var(x)) << "), ";
      cout << "] (" << imps.size() << ")\n";

      cout << " pruned = [ ";
      for(auto x : pruned)
      cout << x << " (marks = " << (int) blitMarks[toInt(x)] << (bvars.isCore(x)
      ? " Core " : " nonCore ")
      << " wt = " << bvars.wt(var(x)) << "), ";
      cout << "] (" << pruned.size() << ")\n";
      }

      else {
      for(size_t i =0; i < pruned.size(); i++)
      if(pruned[i] != imps[i]) {
          cout << "ERROR regular interface returned different set of imps\n";

          cout << l << " (marks = " << (int) blitMarks[toInt(l)] <<
      (bvars.isCore(l) ? " Core " : " nonCore "); cout << " wt = " <<
      bvars.wt(var(l)) << ")\n";

          cout << " imps = [ ";
          for(auto x : imps)
      cout << x << " (marks = " << (int) blitMarks[toInt(x)] << (bvars.isCore(x)
      ? " Core " : " nonCore ")
      << " wt = " << bvars.wt(var(x)) << "), ";
          cout << "] (" << imps.size() << ")\n";

          cout << " pruned = [ ";
          for(auto x : pruned)
      cout << x << " (marks = " << (int) blitMarks[toInt(x)] << (bvars.isCore(x)
      ? " Core " : " nonCore ")
      << " wt = " << bvars.wt(var(x)) << "), ";
          cout << "] (" << pruned.size() << ")\n";

          break;
      }
      }*/
    // debug

    for (size_t i = 0; i < imps.size(); ++i)
      imps[i] = ~imps[i];  // convert from implication to MX

    totalMxMem += sizeof(Lit) * imps.size();
    blitMXes[toInt(l)] = new vector<Lit>(std::move(imps));
    return blitMXes[toInt(l)];
  }

  // otherwise prune for marks
  auto v = blitMXes[toInt(l)];
  size_t j{0};
  for (size_t i = 0; i < v->size(); ++i)
    if (blitMarks[toInt((*v)[i])] != inmx) (*v)[j++] = (*v)[i];

  v->resize(j);
  return v;
}

size_t MXFinder::getMXLitSize(Lit l) {
  // potentially we could aproximate this by returning
  // the size without pruning for marks and using getMXrecomputeSizes
  // on each found mx to get most of the sizes right.
  return getMXLits(l)->size();
}

void MXFinder::getMXRecomputeSizes(const vector<Lit>& newlyMarked) {
  auto nic = nImpCalls;
  for (auto l :
       newlyMarked) {  // note we don't need to update vectors of marked lits
    auto v = getMXLits(l);  // this is automatic from getMXLits. v is set of
                            // lits that need update
    for (auto x : *v) {
      auto vx = blitMXes[toInt(x)];
      if (!vx) continue;
      size_t j{0};
      for (size_t i = 0; i < vx->size(); ++i)
        if (blitMarks[toInt((*vx)[i])] == inmx) (*vx)[j++] = (*vx)[i];
      vx->resize(j);
    }
  }
  if (nImpCalls > nic && params.verbosity > 0)
    cout << "c WARNING getMXRecomputeSizes used some implication calls!\n";
}

// Internal variable number to input file numbering.

void Wcnf::remapVars() {
  vector<char> appears(nVars(), false);
  for (const auto cls : hard_cls)
    for (auto l : cls) appears[var(l)] = true;

  flipped_vars.resize(nVars(), 0);
  for (const auto cls : soft_cls) {
    for (auto l : cls) appears[var(l)] = true;
    // convert formula so that unit softs are of the form (-x) instead
    // of (x)---so making the 'blit' x true incurs the cost
    if (cls.size() == 1 && !sign(cls[0])) flipped_vars[var(cls[0])] = true;
  }

  Var nxtvar{0};
  ex2in.resize(nVars(), var_Undef);
  in2ex.resize(nVars(), var_Undef);
  for (Var v = 0; v != static_cast<Var>(nVars()); v++)
    if (appears[v]) {
      in2ex[nxtvar] = v;
      ex2in[v] = nxtvar;
      ++nxtvar;
    }
  maxvar = nxtvar - 1;

  Packed_vecs<Lit> tmp;
  vector<Lit> c;
  for (auto cls : hard_cls) {
    c.clear();
    for (auto l : cls) {
      assert(ex2in[var(l)] != var_Undef);
      c.push_back(
          mkLit(ex2in[var(l)], flipped_vars[var(l)] ? !sign(l) : sign(l)));
    }
    tmp.addVec(c);
  }
  hard_cls = std::move(tmp);
  tmp.clear();

  for (auto cls : soft_cls) {
    c.clear();
    for (auto l : cls) {
      assert(ex2in[var(l)] != var_Undef);
      c.push_back(
          mkLit(ex2in[var(l)], flipped_vars[var(l)] ? !sign(l) : sign(l)));
    }
    tmp.addVec(c);
  }
  soft_cls = std::move(tmp);

  for (auto& mx : mutexes) {
    for (auto& l : mx.soft_clause_lits_mod())
      l = mkLit(ex2in[var(l)], flipped_vars[var(l)] ? !sign(l) : sign(l));
    auto& el = mx.encoding_lit_mod();
    if (el != lit_Undef)
      el = mkLit(ex2in[var(el)], flipped_vars[var(el)] ? !sign(el) : sign(el));
  }
}

// INPUT PROBLEM/INTERNAL PROBLEM interface
// Take model found by solver and rewrite it into model of original formula
vector<lbool> Wcnf::rewriteModelToInput(const vector<lbool>& ubmodel) {
  // all original internal vars are preserved by solver. But
  // more vars might be added after.

  vector<lbool> ex_model(nOrigVars(), l_True);
  if (!in2ex.size()) {
    ex_model = ubmodel;
    ex_model.resize(nOrigVars());
  }
  for (size_t i = 0; i < in2ex.size() && i < ubmodel.size(); ++i) {
    if (in2ex[i] != var_Undef && in2ex[i] < static_cast<Var>(nOrigVars())) {
      ex_model[in2ex[i]] =
          flipped_vars[in2ex[i]] ? ubmodel[i].neg() : ubmodel[i];
    }
  }
  for (auto l : hard_units) {
    ex_model[var(l)] = sign(l) ? l_False : l_True;
  }
  for (const auto& scc : all_scc) {
    for (size_t i = 0; i < scc.size(); ++i) {
      ex_model[var(scc[i])] = sign(scc[i]) == sign(scc[0])
                                  ? ex_model[var(scc[0])]
                                  : ex_model[var(scc[0])].neg();
    }
  }
  return ex_model;
}

// verify model against original formula.
Weight Wcnf::checkModel(const vector<lbool>& ubmodel, int& nfalseSofts,
                        bool final) {
  if (final) {
    hard_cls.clear();
    soft_cls.clear();
  }
  nfalseSofts = 0;
  Wcnf newCopy;
  newCopy.inputDimacs(fileName(), true);  // don't process input problem
  vector<lbool> ex_model = rewriteModelToInput(ubmodel);
  for (auto hc : newCopy.hards()) {
    bool isSat = false;
    for (auto lt : hc)
      if ((sign(lt) && ex_model[var(lt)] == l_False) ||
          (!sign(lt) && ex_model[var(lt)] == l_True)) {
        isSat = true;
        break;
      }
    if (!isSat) {
      cout << "c ERROR WCNF. Model does not satisfy the hards\n"
           << "c violated hard = [";
      for (auto l : hc) cout << l << ", ";
      cout << "]\n";
      log(1, "b the model ", ubmodel);
      return -1;
    }
  }
  Weight w{0};
  int i = 0;
  if (newCopy.nSofts()) {
    for (auto sc : newCopy.softs()) {
      bool isSat = false;
      for (auto lt : sc)
        if ((sign(lt) && ex_model[var(lt)] == l_False) ||
            (!sign(lt) && ex_model[var(lt)] == l_True)) {
          isSat = true;
          break;
        }
      if (!isSat) {
        w += newCopy.getWt(i);
        ++nfalseSofts;
      }
      ++i;
    }
  } else {
    for (auto& objCls : getObjectives()) {
      Weight tmp{0};
      for (auto& lit : objCls) {
        tmp += (Weight)((!sign(lit) && ex_model[var(lit)] == l_True) ||
                        (sign(lit) && ex_model[var(lit)] == l_False));
      }
      w = std::max(w, tmp);
    }
    for (auto implObj : getImplicationObj()) {
      auto cls = implObj.second;
      bool blocked{true};
      for (auto lit : cls) {
        blocked = ((!sign(lit) && ex_model[var(lit)] == l_True) ||
                   (sign(lit) && ex_model[var(lit)] == l_False));
        if (!blocked) {
          break;
        }
      }
      w += (Weight)blocked;
    }
    for (auto& cls : clausalConstraints()) {
      if (cls.size() == 2) continue;
      for (auto& blit : cls) {
        if (ex_model[var(blit)] == l_True) {
          w++;
          break;
        }
      }
    }
    return w;
  }
  return w;
}

// Stats and output
void Wcnf::computeWtInfo() {
  vector<Weight> diffWts;
  vector<int> diffWtCounts;
  transitionWts.clear();

  if (soft_clswts.size() == 0) {
    wt_min = wt_max = wt_mean = wt_var = 0;
    if (hard_cls.size() > 0) {
      if (baseCost() > 0)
        ms_type = MStype::wpms;
      else
        ms_type = MStype::pms;
    } else {
      if (baseCost() > 0)
        ms_type = MStype::wms;
      else
        ms_type = MStype::ms;
    }
    return;
  }

  wt_min = wt_max = 0;
  vector<Weight> wts(soft_clswts);
  std::sort(wts.begin(), wts.end());

  wt_min = wts.front();
  wt_max = wts.back();

  wt_mean = wt_var = 0;
  for (auto x : wts) wt_mean += x;

  wt_mean /= wts.size();
  for (auto x : wts) wt_var += (x - wt_mean) * (x - wt_mean);
  wt_var /= wts.size() - 1;

  size_t j{0};
  diffWts.push_back(wts[0]);
  diffWtCounts.push_back(0);
  for (size_t i = 0; i < wts.size(); i++)
    if (wts[j] == wts[i])
      diffWtCounts.back()++;
    else {
      j = i;
      diffWts.push_back(wts[i]);
      diffWtCounts.push_back(1);
    }

  Weight wtSoFar = diffWts[0] * diffWtCounts[0];
  for (size_t i = 1; i < diffWts.size(); i++) {
    if (diffWts[i] > wtSoFar) transitionWts.push_back(diffWts[i]);
    wtSoFar += diffWts[i] * diffWtCounts[i];
  }

  ndiffWts = diffWts.size();

  if (hard_cls.size() > 0) {
    if (diffWts.size() > 1 || baseCost() > 0)
      ms_type = MStype::wpms;
    else
      ms_type = MStype::pms;
  } else {
    if (diffWts.size() > 1 || baseCost() > 0)
      ms_type = MStype::wms;
    else
      ms_type = MStype::ms;
  }
}

void Wcnf::printFormulaStats() {
  // hard_units if not empty will include original units.
  // if empty it will not
  auto n_units = hard_units.size();
  if (!n_units) n_units = nOrigUnits;
  cout << "c Instance: " << instance_file_name << "\n";
  cout << "c Dimacs Vars: " << dimacs_nvars << "\n";
  cout << "c Dimacs Clauses: " << dimacs_nclauses << "\n";
  cout << "c Dimacs Top: " << wt_fmt(dimacs_top) << '\n';
  cout << "c HARD: #Clauses = " << hard_cls.size() + n_units
       << ", Total Lits = " << hard_cls.total_size() + n_units << ", Ave Len = "
       << fix4_fmt((hard_cls.size() + n_units > 0)
                       ? (1.0 * hard_cls.total_size() + n_units) /
                             (hard_cls.size() + n_units)
                       : 0.0)
       << " #units = " << n_units << "\n";
  cout << "c SOFT: #Clauses = " << soft_cls.size()
       << ", Total Lits = " << soft_cls.total_size() << ", Ave Len = "
       << fix4_fmt(soft_cls.size()
                       ? static_cast<double>(soft_cls.total_size()) /
                             soft_cls.size()
                       : 0)
       << "\n";
  cout << "c Total Soft Clause Weight (+ basecost): " << wt_fmt(totalClsWt())
       << " (+ " << wt_fmt(baseCost()) << ")\n";
  cout << "c SOFT%: "
       << fix4_fmt(soft_cls.size() + hard_cls.size() + n_units
                       ? (100.0 * soft_cls.size()) /
                             (soft_cls.size() + hard_cls.size() + n_units)
                       : 0)
       << "%\n";
  cout << "c #distinct weights: " << nDiffWts()
       << ", mean = " << fix4_fmt(aveSftWt())
       << ", std. dev = " << fix4_fmt(std::sqrt(varSftWt()))
       << ", min = " << wt_fmt(minSftWt()) << ", max = " << wt_fmt(maxSftWt())
       << "\n";
  cout << "c Total Clauses: " << hard_cls.size() + n_units + soft_cls.size()
       << "\n";
  cout << "c Parse time: " << parsing_time << "\n";
  cout << "c Wcnf Space Required: "
       << fix4_fmt(
              ((hard_cls.total_size() + soft_cls.total_size()) * sizeof(Lit) +
               soft_clswts.size() * sizeof(Weight)) /
              (1024 * 1024))
       << "MB\n";
  if (unsat) cout << "c Wcnf is UNSAT (hards are contradictory)\n";
  cout << "c ================================"
       << "\n";
}

void Wcnf::printSimpStats() {
  cout << "c After WCNF Simplification\n";
  cout << "c HARD: #Clauses = " << hard_cls.size()
       << ", Total Lits = " << hard_cls.total_size() << ", Ave Len = "
       << fix4_fmt((hard_cls.size() > 0)
                       ? (1.0 * hard_cls.total_size()) / hard_cls.size()
                       : 0.0)
       << "\n";
  cout << "c SOFT: #Clauses = " << soft_cls.size()
       << ", Total Lits = " << soft_cls.total_size() << ", Ave Len = "
       << fix4_fmt((1.0 * soft_cls.total_size()) / soft_cls.size()) << "\n";
  cout << "c Total Soft Clause Weight (+ basecost): " << wt_fmt(totalClsWt())
       << " (+ " << wt_fmt(baseCost())
       << "), Dimacs Top = " << wt_fmt(dimacs_top) << "\n";
  cout << "c #distinct weights: " << nDiffWts()
       << ", mean = " << fix4_fmt(aveSftWt())
       << ", std. dev = " << fix4_fmt(std::sqrt(varSftWt()))
       << ", min = " << wt_fmt(minSftWt()) << ", max = " << wt_fmt(maxSftWt())
       << "\n";
  cout << "c Total Clauses: " << hard_cls.size() + soft_cls.size() << "\n";
  cout << "c Wcnf Space Required: "
       << fix4_fmt(
              ((hard_cls.total_size() + soft_cls.total_size()) * sizeof(Lit) +
               soft_clswts.size() * sizeof(Weight)) /
              1000000.0)
       << "MB\n";
  if (unsat) cout << "c Wcnf is UNSAT (hards are contradictory)\n";
  cout << "c ================================"
       << "\n";
}

void Wcnf::printFormula(std::ostream& out) const {
  // TODO modify to optionally output new DIMACS file.
  out << "c Wcnf---Print Formula\n";
  out << "c Dimacs (Vars, Clauses, TOP) = (" << dimacs_nvars << " ,"
      << dimacs_nclauses << " ," << dimacs_top << ")";
  out << " maxvar = " << nVars() << "\n";
  if (unsat) out << " formula is UNSAT\n";
  out << "c Hard Clauses # = " << hard_cls.size() + hard_units.size() << "\n";
  out << "c Soft Clauses, # = " << soft_cls.size() << "\n";
  out << "c Base cost = " << wt_fmt(base_cost) << "\n";
  out << "c HARD Units\n";
  out << hard_units << "\n";
  out << "c HARD SCCs\n";
  out << all_scc << "\n";
  out << "c HARDS\n";
  out << hard_cls;

  out << "c SOFTS\n";
  for (size_t i = 0; i < soft_cls.size(); i++) {
    out << wt_fmt(soft_clswts[i]) << " ";
    for (auto& item : soft_cls[i]) out << item << " ";
    out << "0 \n";
  }
}

void Wcnf::printFormula(Bvars& bvars, std::ostream& out) const {
  // TODO modify to optionally output new DIMACS file.
  out << "c Wcnf---Print Formula\n";
  out << "c Dimacs (Vars, Clauses, TOP) = (" << dimacs_nvars << " ,"
      << dimacs_nclauses << " ," << wt_fmt(dimacs_top) << ")";
  out << " maxvar = " << nVars() << "\n";
  out << "c totalClsWt = " << wt_fmt(totalClsWt()) << "\n";
  if (unsat) out << " formula is UNSAT\n";
  out << "c Hard Clauses # = " << hard_cls.size() << "\n";
  out << "c Hard Units # = " << hard_units.size() << "\n";
  out << "c Hard SCC # = " << all_scc.size() << "\n";

  int nh{0};
  for (size_t i = 0; i < all_scc.size(); i++)
    out << "scc#" << nh++ << ": " << all_scc[i] << "\n";
  nh = 0;

  for (size_t i = 0; i < hard_units.size(); i++)
    out << "h#" << nh++ << ": " << hard_units[i] << "\n";

  for (size_t i = 0; i < nHards(); i++)
    out << "h#" << nh++ << ": " << getHard(i) << "\n";

  out << "c Soft Clauses, # = " << soft_cls.size() << "\n";
  out << "c Base cost = " << base_cost << "\n";

  int ns{0};
  for (size_t i = 0; i < nSofts(); i++)
    out << "c#" << ns++ << " blit = " << bvars.litOfCls(i)
        << " wt = " << wt_fmt(getWt(i)) << " : " << getSoft(i) << "\n";
}

// Does not do anything!
void Wcnf::printDimacs(std::ostream& out) const {
  // Broken---fix or remove on next update
  return;
  out << "c maxhs-simplify max original var: " << maxOrigVar() + 1 << "\n";
  out << "c maxhs-simplify original file name: " << instance_file_name << "\n";
  if (unsat) {
    out << "c HARDS are UNSAT\n";
    out << "p cnf 1 2\n";
    out << "-1 0\n";
    out << "1 0 \n";
    return;
  }
  auto top = totalWt() + 1;
  int nvars = nOrigVars();
  int scc_cls_size{0};
  for (const auto& scc : all_scc) scc_cls_size += (scc.size() - 1) * 2;
  int ncls = nSofts() + nHards() + hard_units.size() + scc_cls_size;

  if (baseCost() > 0) {  // Encode base cost with extra pair
    ncls += 2;           // or contradictory softs clauses
    if (nvars == 0)      // Ensure we have a variable to work with.
      nvars++;
  }

  switch (mstype()) {
    case MStype::ms:
      out << "p cnf " << nvars << " " << ncls << "\n";
      break;
    case MStype::wms:
      out << "p wcnf " << nvars << " " << ncls << "\n";
      break;
    case MStype::pms:
      out << "p wcnf " << nvars << " " << ncls << " " << wt_fmt(top) << "\n";
      break;
    case MStype::wpms:
      out << "p wcnf " << nVars() << " " << ncls << " " << wt_fmt(top) << "\n";
      break;
    case MStype::undef:
      out << "c ERROR problem finding out mstype\n";
      out << "p wcnf " << nVars() << " " << ncls << " " << wt_fmt(top) << "\n";
  }

  if (baseCost() > 0) {
    // Above we ensured that nvars >= 1. Use 1 to encode the base costs.
    out << wt_fmt(baseCost()) << " " << 1 << " 0\n";
    out << wt_fmt(baseCost()) << " " << -1 << " 0\n";
  }

  for (size_t i = 0; i < nSofts(); i++) {
    if (mstype() != MStype::ms) out << wt_fmt(soft_clswts[i]) << " ";
    for (auto l : soft_cls[i]) out << map_in2ex(l) << " ";
    out << "0\n";
  }

  if (!hard_units.empty())
    for (auto l : hard_units) out << l << " 0\n";

  for (size_t i = 0; i < nHards(); i++) {
    // if we have hards wcnf dimacs format includes weights before clauses
    out << top << " ";
    for (auto l : hard_cls[i]) out << map_in2ex(l) << " ";
    out << "0\n";
  }
}
